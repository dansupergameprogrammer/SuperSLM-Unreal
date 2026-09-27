// U1 R-S1l(j): a GPU slot returned with a binding must unbind before a new
// holder's first job, while a new holder's own request survives that recycle.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

namespace
{
	bool ConfigureBoth(FAutomationTestBase& T, UWorld* World, USuperSLMModel& Model,
		int32 Layers, int64 ContextCap, USuperSLMSubsystem*& Cpu, USuperSLMGpuSubsystem*& Gpu,
		int32 PrefixBlockCount = 0, int32 LayersPerSlice = 1)
	{
		Cpu = SuperSLML2S1Fixtures::GetSubsystem(World);
		Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("CPU subsystem"), Cpu) || !T.TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
		FSuperSLMRuntimeConfig C;
		C.BlockCount = 1;
		C.PrefixBlockCount = PrefixBlockCount;
		C.MaxSequencesPerDecodeCall = 1;
		C.MaxPrefillChunkBudget = 64;
		C.MaxLayerBudget = Layers;
		C.SequenceLifecycleBudgetMs = 1000.0;
		C.TickBudgetMs = 1000.0;
		if (!T.TestEqual(TEXT("CPU configure"), (uint8)Cpu->Configure(&Model, C).Result,
				(uint8)ESuperSLMConfigureResult::Success)) { return false; }
		FSuperSLMGpuRuntimeConfig G;
		G.ContextCap = ContextCap;
		G.BlockCount = 1;
		G.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(LayersPerSlice);
		G.K = Layers;
		G.TickBudgetMs = 1000.0;
		return T.TestEqual(TEXT("GPU configure"), (uint8)Gpu->Configure(&Model, G).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success);
	}

	bool CpuTerminal(USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq,
		const FSuperSLMGenerationRequest& Request, TArray<int32>& Tokens,
		ESuperSLMSequencePhase& Phase)
	{
		FString Error;
		if (!Cpu.BeginGeneration(Seq, Request, Error)) { return false; }
		const double End = FPlatformTime::Seconds() + 120.0;
		while (FPlatformTime::Seconds() < End)
		{
			Cpu.Tick(1.0f / 60.0f);
			Phase = Cpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				Tokens = Cpu.GetGeneratedTokens(Seq);
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}

	bool GpuTerminal(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq,
		const FSuperSLMGenerationRequest& Request, TArray<int32>& Tokens,
		ESuperSLMSequencePhase& Phase)
	{
		if (!Gpu.RequestBeginGeneration(Seq, Request).IsValid()) { return false; }
		const double End = FPlatformTime::Seconds() + 120.0;
		bool bSawNonTerminal = false;
		while (FPlatformTime::Seconds() < End)
		{
			Gpu.Tick(1.0f / 60.0f);
			Phase = Gpu.GetPhase(Seq);
			bSawNonTerminal |= Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted;
			if (bSawNonTerminal && (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted))
			{
				Tokens = Gpu.GetGeneratedTokens(Seq);
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuAdapterHandoffTest,
	"SuperSLM.U1.Gpu.AdapterHandoffOneCallAndComposed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuAdapterHandoffTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 28, 1024, Cpu, Gpu)) { return false; }
	FString Error;
	FSuperSLMAdapterHandle CpuAdapter;
	FSuperSLMGpuAdapterHandle GpuAdapter;
	if (!TestTrue(TEXT("CPU adapter imports"), FSuperSLMAdapterImport::ImportFromFile(
		SuperSLML2S1Fixtures::AAdAdapterPath(), *Model, CpuAdapter, Error)) ||
		!TestTrue(TEXT("GPU adapter maps"), Gpu->MapAdapter(
		SuperSLML2S1Fixtures::AAdAdapterPath(), *Model, GpuAdapter, Error))) { return false; }
	TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
	if (!TestTrue(TEXT("A-AD prompt case loads"), SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error)) ||
		!TestTrue(TEXT("case 0 exists"), Cases.Num() > 0)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Cases[0].PromptTokens;
	Request.MaxNewTokens = 32;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> Reference[2];
	for (int32 Bind = 0; Bind < 2; ++Bind)
	{
		FSuperSLMSequence S;
		if (Cpu->VendSequence(S) != ESuperSLMVendResult::Success) { return false; }
		if (Bind) { Cpu->RequestAdapterSwap(S, CpuAdapter); }
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("CPU reference completes"), CpuTerminal(*Cpu, S, Request, Reference[Bind], Phase)) ||
			!TestEqual(TEXT("CPU reference Complete"), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		Cpu->ReturnSequence(S);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	}
	if (!TestNotEqual(TEXT("CPU base and adapter references discriminate"), Reference[0], Reference[1])) { return false; }

	bool bOk = true;
	for (const ESuperSLMGpuDecodePath DecodePath : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		for (int32 Ask = 0; Ask < 2; ++Ask)
		{
			FSuperSLMGpuSequence Old;
			if (Gpu->VendSequence(Old, DecodePath) != ESuperSLMGpuVendResult::Success) { return false; }
			Gpu->RequestAdapterSwap(Old, GpuAdapter);
			TArray<int32> OldTokens;
			ESuperSLMSequencePhase OldPhase = ESuperSLMSequencePhase::Idle;
			if (!TestTrue(TEXT("old GPU holder completes"), GpuTerminal(*Gpu, Old, Request, OldTokens, OldPhase))) { return false; }
			bOk &= TestEqual(TEXT("old GPU holder uses adapter"), OldTokens, Reference[1]);
			Gpu->ReturnSequence(Old);
			FSuperSLMGpuSequence New;
			if (!TestEqual(TEXT("new GPU holder vends before recycle tick"),
					(uint8)Gpu->VendSequence(New, DecodePath), (uint8)ESuperSLMGpuVendResult::Success)) { return false; }
			if (Ask) { Gpu->RequestAdapterSwap(New, GpuAdapter); }
			TArray<int32> Tokens;
			ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
			bOk &= TestTrue(TEXT("new GPU holder terminates"), GpuTerminal(*Gpu, New, Request, Tokens, Phase));
			bOk &= TestEqual(TEXT("new holder tokens equal its CPU reference from token 0"), Tokens, Reference[Ask]);
			bOk &= TestEqual(TEXT("new holder reports its own adapter"), Gpu->GetActiveAdapter(New).Id,
				Ask ? GpuAdapter.Id : int64(0));
			bOk &= TestEqual(TEXT("new holder Complete"), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete);
			// The last Y arm reaches teardown while its new holder still owns the adapter.
			if (!(DecodePath == ESuperSLMGpuDecodePath::Composed && Ask == 1))
			{
				Gpu->ReturnSequence(New);
			}
		}
	}
	FSuperSLMGpuTeardownStatuses Statuses;
	bOk &= TestTrue(TEXT("teardown executes with a live adapter holder"),
		FSuperSLMGpuTestAccess::TearDownWithStatuses(*Gpu, Statuses));
	bOk &= TestTrue(TEXT("Layer 1 teardown statuses recorded"), Statuses.bRecorded);
	bOk &= TestTrue(TEXT("live slot unbound during teardown"), Statuses.SequenceUnbindStatuses.Num() > 0);
	for (const FString& Status : Statuses.SequenceUnbindStatuses)
	{
		bOk &= TestEqual(TEXT("teardown sequence unbind returns SSLM_OK"), Status, FString(TEXT("SSLM_OK")));
	}
	for (const FString& Status : Statuses.SequenceReleaseStatuses)
	{
		bOk &= TestEqual(TEXT("teardown sequence release returns SSLM_OK"), Status, FString(TEXT("SSLM_OK")));
	}
	bOk &= TestTrue(TEXT("adapter was unmapped during teardown"), Statuses.AdapterIds.Contains(GpuAdapter.Id));
	for (const FString& Status : Statuses.AdapterUnmapStatuses)
	{
		bOk &= TestEqual(TEXT("teardown adapter unmap returns SSLM_OK"), Status, FString(TEXT("SSLM_OK")));
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuSchemaHandoffTest,
	"SuperSLM.U1.Gpu.SchemaHandoffOneCallAndComposed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuSchemaHandoffTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Error;
	if (!TestTrue(TEXT("A-EX present"), SuperSLML2S2Fixtures::TryGetAExArtifactPath(Path, Error))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 24, 4096, Cpu, Gpu)) { return false; }
	FSuperSLMSchemaHandle CpuSchema;
	FSuperSLMGpuSchemaHandle GpuSchema;
	if (!TestTrue(TEXT("CPU schema resolves"), FSuperSLMSchemaLookup::LookupByName(*Model,
		SuperSLML2S2Fixtures::DemoSchemaName(), CpuSchema, Error)) ||
		!TestTrue(TEXT("GPU schema resolves"), FSuperSLMGpuSchemaLookup::LookupByName(*Model,
		SuperSLML2S2Fixtures::DemoSchemaName(), GpuSchema, Error))) { return false; }
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("pinned prompt tokenizes"), Cpu->Tokenize(SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(),
		Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 48;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> Reference[2];
	ESuperSLMSequencePhase RefPhase[2];
	for (int32 Bind = 0; Bind < 2; ++Bind)
	{
		FSuperSLMSequence S;
		if (Cpu->VendSequence(S) != ESuperSLMVendResult::Success) { return false; }
		if (Bind && !TestTrue(TEXT("CPU bind"), Cpu->SetSchema(S, CpuSchema, Error))) { return false; }
		if (!TestTrue(TEXT("CPU reference terminal"), CpuTerminal(*Cpu, S, Request, Reference[Bind], RefPhase[Bind]))) { return false; }
		Cpu->ReturnSequence(S);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	}
	if (!TestNotEqual(TEXT("CPU constrained and unconstrained references discriminate"), Reference[0], Reference[1])) { return false; }
	bool bOk = true;
	for (const ESuperSLMGpuDecodePath DecodePath : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		for (int32 Ask = 0; Ask < 2; ++Ask)
		{
			FSuperSLMGpuSequence Old;
			if (Gpu->VendSequence(Old, DecodePath) != ESuperSLMGpuVendResult::Success) { return false; }
			if (!TestTrue(TEXT("old GPU schema bind"), Gpu->SetSchema(Old, GpuSchema, Error))) { return false; }
			TArray<int32> OldTokens;
			ESuperSLMSequencePhase OldPhase = ESuperSLMSequencePhase::Idle;
			if (!TestTrue(TEXT("old GPU holder terminal"), GpuTerminal(*Gpu, Old, Request, OldTokens, OldPhase))) { return false; }
			bOk &= TestEqual(TEXT("old GPU holder uses constrained reference"), OldTokens, Reference[1]);
			Gpu->ReturnSequence(Old);
			FSuperSLMGpuSequence New;
			if (!TestEqual(TEXT("new GPU holder vends before recycle tick"),
					(uint8)Gpu->VendSequence(New, DecodePath), (uint8)ESuperSLMGpuVendResult::Success)) { return false; }
			if (Ask) { bOk &= TestTrue(TEXT("new GPU schema bind accepted"), Gpu->SetSchema(New, GpuSchema, Error)); }
			TArray<int32> Tokens;
			ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
			bOk &= TestTrue(TEXT("new GPU holder terminal"), GpuTerminal(*Gpu, New, Request, Tokens, Phase));
			bOk &= TestEqual(TEXT("new GPU holder matches CPU reference from token 0"), Tokens, Reference[Ask]);
			bOk &= TestEqual(TEXT("new GPU holder terminal phase"), (uint8)Phase, (uint8)RefPhase[Ask]);
			bOk &= TestEqual(TEXT("new holder reports its own schema"), Gpu->GetBoundSchema(New).IsNone(), Ask == 0);
			Gpu->ReturnSequence(New);
		}
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuRestoreAdapterPinTest,
	"SuperSLM.U1.Gpu.RestoreAdapterPinAndRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRestoreAdapterPinTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 28, 1024, Cpu, Gpu)) { return false; }
	FString Error;
	FSuperSLMAdapterHandle CpuAdapter;
	if (!TestTrue(TEXT("CPU adapter imports"), FSuperSLMAdapterImport::ImportFromFile(
		SuperSLML2S1Fixtures::AAdAdapterPath(), *Model, CpuAdapter, Error))) { return false; }
	TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
	if (!TestTrue(TEXT("A-AD prompt case loads"), SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error)) ||
		!TestTrue(TEXT("case 0 exists"), Cases.Num() > 0)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Cases[0].PromptTokens;
	Request.MaxNewTokens = 32;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> BaseReference, AdapterReference;
	for (int32 Bind = 0; Bind < 2; ++Bind)
	{
		FSuperSLMSequence S;
		if (Cpu->VendSequence(S) != ESuperSLMVendResult::Success) { return false; }
		if (Bind) { Cpu->RequestAdapterSwap(S, CpuAdapter); }
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("CPU reference completes"), CpuTerminal(*Cpu, S, Request,
			Bind ? AdapterReference : BaseReference, Phase))) { return false; }
		Cpu->ReturnSequence(S);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	}
	if (!TestNotEqual(TEXT("base and adapter references discriminate"), BaseReference, AdapterReference)) { return false; }

	bool bOk = true;
	for (const ESuperSLMGpuDecodePath DecodePath : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		FSuperSLMGpuAdapterHandle Adapter;
		if (!TestTrue(TEXT("GPU adapter maps"), Gpu->MapAdapter(SuperSLML2S1Fixtures::AAdAdapterPath(),
			*Model, Adapter, Error))) { return false; }
		// R-S2k's source anchor: each GPU path must first reproduce the CPU
		// adapter reference under an ordinary, unsaved generation.
		FSuperSLMGpuSequence Control;
		if (!TestEqual(TEXT("bound control vends"), (uint8)Gpu->VendSequence(Control, DecodePath),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		Gpu->RequestAdapterSwap(Control, Adapter);
		TArray<int32> ControlTokens;
		ESuperSLMSequencePhase ControlPhase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("bound GPU control runs"), GpuTerminal(*Gpu, Control, Request, ControlTokens, ControlPhase))) { return false; }
		bOk &= TestEqual(TEXT("bound GPU control equals CPU adapter reference"), ControlTokens, AdapterReference);
		bOk &= TestEqual(TEXT("bound GPU control completes"), (uint8)ControlPhase, (uint8)ESuperSLMSequencePhase::Complete);
		Gpu->ReturnSequence(Control);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the bound control"))) { break; } FPlatformProcess::Sleep(0.001f); }
		FSuperSLMGpuSequence Seq;
		if (Gpu->VendSequence(Seq, DecodePath) != ESuperSLMGpuVendResult::Success) { return false; }
		Gpu->RequestAdapterSwap(Seq, Adapter);
		if (!TestTrue(TEXT("bound generation queues"), Gpu->RequestBeginGeneration(Seq, Request).IsValid())) { return false; }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (Gpu->GetGeneratedTokens(Seq).Num() < 8 && FPlatformTime::Seconds() < Deadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("save point reached while generation is live"),
			Gpu->GetGeneratedTokens(Seq).Num() >= 8 && Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding)) { return false; }
		const FSuperSLMLifecycleOpHandle Save = Gpu->RequestSaveSequence(Seq);
		TArray<uint8> Blob;
		if (!TestEqual(TEXT("adapter-bound save succeeds"),
			(uint8)SuperSLML2S2Fixtures::DriveSaveToResolution(*Gpu, Save, Blob),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		const TArray<int32> DeliveredAtSave = Gpu->GetGeneratedTokens(Seq);
		Gpu->ReturnSequence(Seq);
		const FSuperSLMLifecycleOpHandle Restore = Gpu->RequestRestoreSequence(Blob, Model);
		if (!TestTrue(TEXT("adapter-bound restore accepted"), Restore.IsValid())) { return false; }
		Gpu->UnmapAdapter(Adapter); // must be refused while the restore owns its pin
		FSuperSLMGpuSequence Restored;
		if (!TestEqual(TEXT("pinned restore succeeds"),
			(uint8)SuperSLML2S2Fixtures::DriveRestoreToResolution(*Gpu, Restore, Restored),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		bOk &= TestEqual(TEXT("restored handle reports bound adapter"), Gpu->GetActiveAdapter(Restored).Id, Adapter.Id);
		TArray<int32> Tail;
		ESuperSLMSequencePhase TailPhase = ESuperSLMSequencePhase::Idle;
		const double TailDeadline = FPlatformTime::Seconds() + 120.0;
		while (FPlatformTime::Seconds() < TailDeadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			TailPhase = Gpu->GetPhase(Restored);
			if (TailPhase == ESuperSLMSequencePhase::Complete || TailPhase == ESuperSLMSequencePhase::Faulted)
			{
				Tail = Gpu->GetGeneratedTokens(Restored);
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		TArray<int32> DeliveredTotal = DeliveredAtSave;
		DeliveredTotal.Append(Tail);
		bOk &= TestEqual(TEXT("restored adapter continuation equals CPU reference"), DeliveredTotal, AdapterReference);
		bOk &= TestEqual(TEXT("restored adapter reaches Complete"), (uint8)TailPhase,
			(uint8)ESuperSLMSequencePhase::Complete);
		Gpu->ReturnSequence(Restored);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the restored holder"))) { break; } FPlatformProcess::Sleep(0.001f); }
		Gpu->UnmapAdapter(Adapter); // must take effect after finalize's restore pin release
		FSuperSLMGpuSequence Probe;
		if (Gpu->VendSequence(Probe, DecodePath) != ESuperSLMGpuVendResult::Success) { return false; }
		Gpu->RequestAdapterSwap(Probe, Adapter);
		TArray<int32> ProbeTokens;
		ESuperSLMSequencePhase ProbePhase = ESuperSLMSequencePhase::Idle;
		bOk &= TestTrue(TEXT("post-unmap probe completes"), GpuTerminal(*Gpu, Probe, Request, ProbeTokens, ProbePhase));
		bOk &= TestEqual(TEXT("post-unmap request cannot recover released adapter"), ProbeTokens, BaseReference);
		bOk &= TestFalse(TEXT("post-unmap adapter remains inactive"), Gpu->GetActiveAdapter(Probe).IsValid());
		Gpu->ReturnSequence(Probe);
	}
	return bOk;
}

namespace
{
	bool RestoreAndCountTicks(USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob,
		USuperSLMModel* Model, TArray<int32>& OutTail, int32& OutTicks)
	{
		const FSuperSLMLifecycleOpHandle Restore = Gpu.RequestRestoreSequence(Blob, Model);
		if (!Restore.IsValid()) { return false; }
		FSuperSLMGpuSequence Restored;
		OutTicks = 0;
		bool bRestoreResolved = false;
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Gpu.Tick(1.0f / 60.0f);
			const ESuperSLMRestoreResult Result = Gpu.GetRestoreResult(Restore, Restored);
			if (Result != ESuperSLMRestoreResult::Pending && Result != ESuperSLMRestoreResult::Success) { return false; }
			if (Result == ESuperSLMRestoreResult::Success)
			{
				if (bRestoreResolved) { ++OutTicks; }
				else { bRestoreResolved = true; }
				const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Restored);
				if (Phase == ESuperSLMSequencePhase::Faulted) { return false; }
				if (Phase == ESuperSLMSequencePhase::Complete)
				{
					OutTail = Gpu.GetGeneratedTokens(Restored);
					Gpu.ReturnSequence(Restored);
					for (int32 Drain = 0; Drain < 40; ++Drain) { if (!SuperSLML2S2Fixtures::TickWhenDeviceReady(Gpu)) { return false; } FPlatformProcess::Sleep(0.001f); }
					return true;
				}
			}
			FPlatformProcess::Sleep(1.0f / 60.0f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuSelfCheckSlotOverrideHandoffTest,
	"SuperSLM.U1.Gpu.SelfCheckSlotOverrideHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuSelfCheckSlotOverrideHandoffTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W, FreshW, OneLayerW;
	if (!W.CreateTestWorld(EWorldType::Game) || !FreshW.CreateTestWorld(EWorldType::Game) ||
		!OneLayerW.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Error;
	if (!TestTrue(TEXT("A-EX artifact"), SuperSLML2S2Fixtures::TryGetAExArtifactPath(Path, Error))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX model"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 24, 4096, Cpu, Gpu, 0, 4)) { return false; }
	USuperSLMSubsystem* FreshCpu = nullptr;
	USuperSLMGpuSubsystem* FreshGpu = nullptr;
	if (!ConfigureBoth(*this, FreshW.GetTestWorld(), *Model, 24, 4096, FreshCpu, FreshGpu, 0, 4)) { return false; }
	USuperSLMSubsystem* OneLayerCpu = nullptr;
	USuperSLMGpuSubsystem* OneLayerGpu = nullptr;
	if (!ConfigureBoth(*this, OneLayerW.GetTestWorld(), *Model, 24, 4096,
		OneLayerCpu, OneLayerGpu)) { return false; }
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("prompt tokenize"), Cpu->Tokenize(SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(),
		Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 32;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Source;
	if (!TestEqual(TEXT("source vends"), (uint8)Gpu->VendSequence(Source, ESuperSLMGpuDecodePath::Composed),
		(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	if (!TestTrue(TEXT("source generation queues"), Gpu->RequestBeginGeneration(Source, Request).IsValid())) { return false; }
	const double SaveDeadline = FPlatformTime::Seconds() + 120.0;
	while (Gpu->GetGeneratedTokens(Source).Num() < 8 && FPlatformTime::Seconds() < SaveDeadline)
	{
		Gpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("save point reached during decode"), Gpu->GetGeneratedTokens(Source).Num() >= 8 &&
		Gpu->GetPhase(Source) == ESuperSLMSequencePhase::Decoding)) { return false; }
	const FSuperSLMLifecycleOpHandle Save = Gpu->RequestSaveSequence(Source);
	TArray<uint8> Blob;
	if (!TestTrue(TEXT("save queues"), Save.IsValid()) ||
		!TestEqual(TEXT("save succeeds"), (uint8)SuperSLML2S2Fixtures::DriveSaveToResolution(*Gpu, Save, Blob),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	const TArray<int32> Prefix = Gpu->GetGeneratedTokens(Source);
	Gpu->ReturnSequence(Source);
	for (int32 Drain = 0; Drain < 40; ++Drain) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after returning the source"))) { break; } FPlatformProcess::Sleep(0.001f); }
	TArray<int32> FreshTail1, FreshTail2, OneLayerTail, AfterSeamTail;
	int32 FreshTicks1 = 0, FreshTicks2 = 0, OneLayerTicks = 0, AfterSeamTicks = 0;
	if (!TestTrue(TEXT("first fresh restore completes"), RestoreAndCountTicks(*FreshGpu, Blob, Model, FreshTail1, FreshTicks1)) ||
		!TestTrue(TEXT("second fresh restore completes"), RestoreAndCountTicks(*FreshGpu, Blob, Model, FreshTail2, FreshTicks2)) ||
		!TestTrue(TEXT("one-layer control restore completes"),
			RestoreAndCountTicks(*OneLayerGpu, Blob, Model, OneLayerTail, OneLayerTicks))) { return false; }
	bool bOk = TestEqual(TEXT("fresh restore tick count is repeatable"), FreshTicks1, FreshTicks2);
	bOk &= TestEqual(TEXT("fresh restore tokens are repeatable"), FreshTail1, FreshTail2);
	bOk &= TestEqual(TEXT("one-layer control restores the same tokens"), OneLayerTail, FreshTail1);
	bOk &= TestTrue(TEXT("one-layer control has more continuation ticks than four-layer fresh restore"),
		OneLayerTicks > FreshTicks1);
	TArray<int32> SeamTokens;
	bool bDeadEnd = false;
	bOk &= TestTrue(TEXT("composed self-check runs with one-layer override"),
		FSuperSLMGpuTestAccess::RunGeneration(*Gpu, ESuperSLMGpuDecodePath::Composed, 1,
			Request.PromptTokens, FString(), 8, SeamTokens, bDeadEnd, Error));
	bOk &= TestTrue(TEXT("self-check produced tokens"), SeamTokens.Num() > 0);
	if (!bOk) { return false; }
	for (int32 Drain = 0; Drain < 40; ++Drain) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the self-check seam"))) { break; } FPlatformProcess::Sleep(0.001f); }
	if (!TestEqual(TEXT("free slot override seed targets the restore reservation"),
		FSuperSLMGpuTestAccess::SeedFreeSlotLayersPerSliceOverride(*Gpu, 1), 0)) { return false; }
	bOk &= TestTrue(TEXT("post-seam restore completes"), RestoreAndCountTicks(*Gpu, Blob, Model, AfterSeamTail, AfterSeamTicks));
	bOk &= TestEqual(TEXT("post-seam restore has fresh-slot tick count"), AfterSeamTicks, FreshTicks1);
	bOk &= TestEqual(TEXT("post-seam restore has fresh-slot continuation"), AfterSeamTail, FreshTail1);
	TArray<int32> FreshTotal = Prefix;
	FreshTotal.Append(FreshTail1);
	bOk &= TestEqual(TEXT("restored run produces the requested total"), FreshTotal.Num(), Request.MaxNewTokens);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuMidGenerationAdapterSwitchTest,
	"SuperSLM.U1.Gpu.MidGenerationAdapterSwitchAndUnmap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuMidGenerationAdapterSwitchTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD model"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 28, 1024, Cpu, Gpu, 1)) { return false; }
	FString Error;
	FSuperSLMAdapterHandle CpuAdapter;
	if (!TestTrue(TEXT("CPU adapter imports"), FSuperSLMAdapterImport::ImportFromFile(
		SuperSLML2S1Fixtures::AAdAdapterPath(), *Model, CpuAdapter, Error))) { return false; }
	TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
	if (!TestTrue(TEXT("reference cases load"), SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error)) ||
		!TestTrue(TEXT("case 0 exists"), Cases.Num() > 0)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Cases[0].PromptTokens;
	Request.MaxNewTokens = 64;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> CpuBase, CpuBound;
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		FSuperSLMSequence Seq;
		if (!TestEqual(TEXT("CPU reference vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
		if (Arm == 1) { Cpu->RequestAdapterSwap(Seq, CpuAdapter); }
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("CPU full reference runs"), CpuTerminal(*Cpu, Seq, Request,
			Arm == 0 ? CpuBase : CpuBound, Phase))) { return false; }
		if (!TestEqual(TEXT("CPU reference completes"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		Cpu->ReturnSequence(Seq);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	}
	if (!TestNotEqual(TEXT("CPU base and bound references discriminate"), CpuBase, CpuBound) ||
		!TestEqual(TEXT("CPU base reference reaches the requested token count"), CpuBase.Num(), Request.MaxNewTokens)) { return false; }
	bool bOk = true;
	TArray<int32> OneCallBound, OneCallMid;
	int32 OneCallSwitchIndex = INDEX_NONE;
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		FSuperSLMGpuAdapterHandle Adapter;
		if (!TestTrue(TEXT("GPU adapter maps"), Gpu->MapAdapter(SuperSLML2S1Fixtures::AAdAdapterPath(),
			*Model, Adapter, Error))) { return false; }
		FSuperSLMGpuSequence Bound;
		if (!TestEqual(TEXT("bound control vend"), (uint8)Gpu->VendSequence(Bound, Path), (uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		Gpu->RequestAdapterSwap(Bound, Adapter);
		TArray<int32> BoundTokens;
		ESuperSLMSequencePhase BoundPhase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("bound control runs"), GpuTerminal(*Gpu, Bound, Request, BoundTokens, BoundPhase))) { return false; }
		bOk &= TestEqual(TEXT("GPU bound control equals CPU reference"), BoundTokens, CpuBound);
		if (Path == ESuperSLMGpuDecodePath::OneCall) { OneCallBound = BoundTokens; }
		else { bOk &= TestEqual(TEXT("composed bound control equals one-call"), BoundTokens, OneCallBound); }
		Gpu->ReturnSequence(Bound);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the bound composed control"))) { break; } FPlatformProcess::Sleep(0.001f); }

		FSuperSLMGpuSequence Mid;
		if (!TestEqual(TEXT("mid-switch vend"), (uint8)Gpu->VendSequence(Mid, Path), (uint8)ESuperSLMGpuVendResult::Success) ||
			!TestTrue(TEXT("mid-switch generation queues"), Gpu->RequestBeginGeneration(Mid, Request).IsValid())) { return false; }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (Gpu->GetGeneratedTokens(Mid).Num() < 8 && FPlatformTime::Seconds() < Deadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestEqual(TEXT("GPU request point"), Gpu->GetGeneratedTokens(Mid).Num(), 8)) { return false; }
		Gpu->RequestAdapterSwap(Mid, Adapter);
		bOk &= TestFalse(TEXT("pending swap is not reported active"), Gpu->GetActiveAdapter(Mid).IsValid());
		Gpu->UnmapAdapter(Adapter); // must be refused while the request is pending
		bool bSawReportTransition = false;
		int32 SwitchIndex = INDEX_NONE;
		while (FPlatformTime::Seconds() < Deadline)
		{
			const int32 BeforeTokens = Gpu->GetGeneratedTokens(Mid).Num();
			const int64 BeforeId = Gpu->GetActiveAdapter(Mid).Id;
			Gpu->Tick(1.0f / 60.0f);
			const int64 AfterId = Gpu->GetActiveAdapter(Mid).Id;
			if (BeforeId == 0 && AfterId == Adapter.Id)
			{
				bSawReportTransition = true;
				SwitchIndex = BeforeTokens;
				bOk &= TestTrue(TEXT("adapter report changes on the carrying token's apply tick"),
					Gpu->GetGeneratedTokens(Mid).Num() > BeforeTokens);
			}
			if (Gpu->GetPhase(Mid) == ESuperSLMSequencePhase::Complete || Gpu->GetPhase(Mid) == ESuperSLMSequencePhase::Faulted) { break; }
			FPlatformProcess::Sleep(0.001f);
		}
		const TArray<int32> MidTokens = Gpu->GetGeneratedTokens(Mid);
		bOk &= TestTrue(TEXT("adapter report transitions during generation"), bSawReportTransition);
		bOk &= TestEqual(TEXT("mid-switch completes"), (uint8)Gpu->GetPhase(Mid), (uint8)ESuperSLMSequencePhase::Complete);
		if (!TestTrue(TEXT("the switch has base and bound tokens to compare"),
			SwitchIndex >= 8 && SwitchIndex < CpuBase.Num())) { return false; } // >= 8: ruling 2026-09-26, finding 13 (the 9th token may be unplanned at the request)
		// Reconstruct the same autoregressive context from the independent CPU base run.
		// The CPU adapter is bound only after that prefix has reached a resting slot.
		TArray<int32> PrefixTokens = Request.PromptTokens;
		for (int32 I = 0; I < SwitchIndex - 1; ++I) { PrefixTokens.Add(CpuBase[I]); }
		FSuperSLMPrefix Prefix;
		if (!TestTrue(TEXT("CPU same-point prefix creates"), Cpu->CreatePrefix(PrefixTokens, Prefix, Error))) { return false; }
		const double PrefixDeadline = FPlatformTime::Seconds() + 120.0;
		while (!Cpu->IsPrefixReady(Prefix) && FPlatformTime::Seconds() < PrefixDeadline)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("CPU same-point prefix is ready"), Cpu->IsPrefixReady(Prefix))) { return false; }
		FSuperSLMSequence CpuAtSwitch;
		if (!TestEqual(TEXT("CPU same-point vend"), (uint8)Cpu->VendSequence(CpuAtSwitch),
			(uint8)ESuperSLMVendResult::Success)) { return false; }
		FSuperSLMLifecycleOpHandle Adopt;
		if (!TestEqual(TEXT("CPU same-point adopt queues"),
			(uint8)Cpu->AdoptPrefix(CpuAtSwitch, Prefix, Adopt, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		const double AdoptDeadline = FPlatformTime::Seconds() + 120.0;
		while (Cpu->GetLifecycleOpResult(Adopt) == ESuperSLMRestoreResult::Pending &&
			FPlatformTime::Seconds() < AdoptDeadline)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestEqual(TEXT("CPU same-point adopt resolves"), (uint8)Cpu->GetLifecycleOpResult(Adopt),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Cpu->RequestAdapterSwap(CpuAtSwitch, CpuAdapter);
		FSuperSLMGenerationRequest TailRequest = Request;
		TailRequest.PromptTokens = { CpuBase[SwitchIndex - 1] };
		TailRequest.MaxNewTokens = Request.MaxNewTokens - SwitchIndex;
		TArray<int32> CpuTail;
		ESuperSLMSequencePhase CpuTailPhase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("CPU same-point continuation runs"),
			CpuTerminal(*Cpu, CpuAtSwitch, TailRequest, CpuTail, CpuTailPhase)) ||
			!TestEqual(TEXT("CPU same-point continuation completes"), (uint8)CpuTailPhase,
				(uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		TArray<int32> CpuSamePoint;
		for (int32 I = 0; I < SwitchIndex; ++I) { CpuSamePoint.Add(CpuBase[I]); }
		CpuSamePoint.Append(CpuTail);
		bOk &= TestNotEqual(TEXT("same-point CPU switch distinguishes the base run"), CpuSamePoint, CpuBase);
		bOk &= TestEqual(TEXT("mid-switch GPU tokens equal independent CPU same-point reference"), MidTokens, CpuSamePoint);
		Cpu->ReturnSequence(CpuAtSwitch);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
		bOk &= TestTrue(TEXT("CPU same-point prefix releases"), Cpu->ReleasePrefix(Prefix, Error));
		if (Path == ESuperSLMGpuDecodePath::OneCall)
		{
			OneCallMid = MidTokens;
			OneCallSwitchIndex = SwitchIndex;
		}
		else if (SwitchIndex == OneCallSwitchIndex)
		{
			bOk &= TestEqual(TEXT("both GPU paths agree when they switch at the same token"), MidTokens, OneCallMid);
		}
		Gpu->UnmapAdapter(Adapter); // must also be refused while the holder is bound
		Gpu->ReturnSequence(Mid);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the mid holder"))) { break; } FPlatformProcess::Sleep(0.001f); }
		// The pending-unmap refusal is observable by a fresh swap to the same id.
		FSuperSLMGpuSequence Probe;
		if (!TestEqual(TEXT("post-refusal probe vend"), (uint8)Gpu->VendSequence(Probe, Path),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		Gpu->RequestAdapterSwap(Probe, Adapter);
		TArray<int32> ProbeTokens;
		ESuperSLMSequencePhase ProbePhase = ESuperSLMSequencePhase::Idle;
		bOk &= TestTrue(TEXT("post-refusal adapter probe runs"), GpuTerminal(*Gpu, Probe, Request, ProbeTokens, ProbePhase));
		bOk &= TestEqual(TEXT("refused unmap kept adapter mapped"), ProbeTokens, CpuBound);
		Gpu->ReturnSequence(Probe);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the refused-unmap probe"))) { break; } FPlatformProcess::Sleep(0.001f); }
		Gpu->UnmapAdapter(Adapter); // now must take effect
		if (!TestEqual(TEXT("post-unmap probe vend"), (uint8)Gpu->VendSequence(Probe, Path),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		Gpu->RequestAdapterSwap(Probe, Adapter);
		ProbeTokens.Reset();
		bOk &= TestTrue(TEXT("post-unmap base probe runs"), GpuTerminal(*Gpu, Probe, Request, ProbeTokens, ProbePhase));
		bOk &= TestEqual(TEXT("released adapter id cannot bind"), ProbeTokens, CpuBase);
		Gpu->ReturnSequence(Probe);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the released-adapter probe"))) { break; } FPlatformProcess::Sleep(0.001f); }
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuSavePendingAdapterBindTest,
	"SuperSLM.U1.Gpu.SavePendingAdapterBindTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuSavePendingAdapterBindTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD model"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ConfigureBoth(*this, W.GetTestWorld(), *Model, 28, 1024, Cpu, Gpu)) { return false; }
	FString Error;
	FSuperSLMAdapterHandle CpuAdapter;
	if (!TestTrue(TEXT("CPU adapter imports"), FSuperSLMAdapterImport::ImportFromFile(
		SuperSLML2S1Fixtures::AAdAdapterPath(), *Model, CpuAdapter, Error))) { return false; }
	TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
	if (!TestTrue(TEXT("reference cases"), SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error)) ||
		!TestTrue(TEXT("case 0 exists"), Cases.Num() > 0)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Cases[0].PromptTokens;
	Request.MaxNewTokens = 32;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> CpuReference[2];
	for (int32 Bind = 0; Bind < 2; ++Bind)
	{
		FSuperSLMSequence S;
		if (!TestEqual(TEXT("CPU reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
		if (Bind) { Cpu->RequestAdapterSwap(S, CpuAdapter); }
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("CPU reference runs"), CpuTerminal(*Cpu, S, Request, CpuReference[Bind], Phase)) ||
			!TestEqual(TEXT("CPU reference completes"), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		Cpu->ReturnSequence(S);
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	}
	if (!TestNotEqual(TEXT("CPU base and adapter references discriminate"), CpuReference[0], CpuReference[1])) { return false; }
	bool bOk = true;
	TArray<int32> OneCallRestored;
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		FSuperSLMGpuAdapterHandle Adapter;
		if (!TestTrue(TEXT("GPU adapter maps"), Gpu->MapAdapter(SuperSLML2S1Fixtures::AAdAdapterPath(),
			*Model, Adapter, Error))) { return false; }
		FSuperSLMGpuSequence BoundControl;
		if (!TestEqual(TEXT("bound control vends"), (uint8)Gpu->VendSequence(BoundControl, Path),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		Gpu->RequestAdapterSwap(BoundControl, Adapter);
		TArray<int32> ControlTokens;
		ESuperSLMSequencePhase ControlPhase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("bound control runs"), GpuTerminal(*Gpu, BoundControl, Request, ControlTokens, ControlPhase))) { return false; }
		bOk &= TestEqual(TEXT("GPU bound control equals CPU adapter reference"), ControlTokens, CpuReference[1]);
		Gpu->ReturnSequence(BoundControl);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the bound control"))) { break; } FPlatformProcess::Sleep(0.001f); }

		FSuperSLMGpuSequence Source;
		if (!TestEqual(TEXT("source vend"), (uint8)Gpu->VendSequence(Source, Path), (uint8)ESuperSLMGpuVendResult::Success) ||
			!TestTrue(TEXT("source generation queues"), Gpu->RequestBeginGeneration(Source, Request).IsValid())) { return false; }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (Gpu->GetGeneratedTokens(Source).Num() < 8 && FPlatformTime::Seconds() < Deadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestEqual(TEXT("request after eighth token's apply and next token's planning"),
			Gpu->GetGeneratedTokens(Source).Num(), 8)) { return false; }
		Gpu->RequestAdapterSwap(Source, Adapter);
		const int64 AwaitedTag = FSuperSLMGpuTestAccess::GetAwaitedBindTag(*Gpu, Source);
		bOk &= TestTrue(TEXT("swap request minted a bind tag"), AwaitedTag > 0);
		bOk &= TestFalse(TEXT("planned-before-request token cannot confirm swap"), Gpu->GetActiveAdapter(Source).IsValid());
		const FSuperSLMLifecycleOpHandle Save = Gpu->RequestSaveSequence(Source);
		if (!TestTrue(TEXT("save queues before carrying bind"), Save.IsValid())) { return false; }
		TArray<uint8> Blob;
		int64 BindTagAtSave = -1;
		bool bReadBindTag = false;
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			bReadBindTag |= FSuperSLMGpuTestAccess::GetSaveBindTag(*Gpu, Save, BindTagAtSave);
			SaveResult = Gpu->GetSaveResult(Save, Blob);
			if (SaveResult != ESuperSLMRestoreResult::Pending) { break; }
			FPlatformProcess::Sleep(0.001f);
		}
		bOk &= TestEqual(TEXT("save resolves Success"), (uint8)SaveResult,
			(uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("submission-thread save bind tag is readable"), bReadBindTag);
		bOk &= TestEqual(TEXT("forced boundary and save job precede requested bind"), BindTagAtSave, int64(0));
		bOk &= TestNotEqual(TEXT("save did not consume the pending request's tag"), BindTagAtSave, AwaitedTag);
		const TArray<int32> DeliveredAtSave = Gpu->GetGeneratedTokens(Source);
		Gpu->ReturnSequence(Source);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after returning the save source"))) { break; } FPlatformProcess::Sleep(0.001f); }
		const FSuperSLMLifecycleOpHandle Restore = Gpu->RequestRestoreSequence(Blob, Model);
		if (!TestTrue(TEXT("restore queues"), Restore.IsValid())) { return false; }
		FSuperSLMGpuSequence Restored;
		if (!TestEqual(TEXT("restore resolves"), (uint8)SuperSLML2S2Fixtures::DriveRestoreToResolution(*Gpu, Restore, Restored),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		bOk &= TestFalse(TEXT("restored handle has no inherited pending swap"), Gpu->GetActiveAdapter(Restored).IsValid());
		const double TailDeadline = FPlatformTime::Seconds() + 120.0;
		while (Gpu->GetPhase(Restored) != ESuperSLMSequencePhase::Complete &&
			Gpu->GetPhase(Restored) != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() < TailDeadline)
		{
			Gpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		bOk &= TestEqual(TEXT("restored generation completes"), (uint8)Gpu->GetPhase(Restored),
			(uint8)ESuperSLMSequencePhase::Complete);
		TArray<int32> Total = DeliveredAtSave;
		Total.Append(Gpu->GetGeneratedTokens(Restored));
		bOk &= TestEqual(TEXT("save-boundary base continuation equals CPU base reference"), Total, CpuReference[0]);
		if (Path == ESuperSLMGpuDecodePath::OneCall) { OneCallRestored = Total; }
		else { bOk &= TestEqual(TEXT("composed restore equals one-call restore"), Total, OneCallRestored); }
		Gpu->ReturnSequence(Restored);
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after the restored holder"))) { break; } FPlatformProcess::Sleep(0.001f); }
		Gpu->UnmapAdapter(Adapter);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
