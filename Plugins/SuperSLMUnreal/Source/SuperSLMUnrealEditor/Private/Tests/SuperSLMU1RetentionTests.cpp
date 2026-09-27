// U1 row 19: lifecycle results belong to their handles until taken or released.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	// Round 2 of the GPU apply ruling (plan §2.5 row 21, finding 6): on the GPU, PollResult() sleeps
	// 1 ms instead of ticking while the next Tick() would be gated on the device. The CPU subsystem
	// has no such gate.
	inline bool PollGatedOnDevice(const USuperSLMGpuSubsystem& Gpu) { return FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu); }
	inline bool PollGatedOnDevice(const USuperSLMSubsystem&) { return false; }

	template <typename TSubsystem, typename TRead>
	ESuperSLMRestoreResult PollResult(TSubsystem& Sub, TRead Read)
	{
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		while (Result == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() < Deadline)
		{
			if (PollGatedOnDevice(Sub))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Sub.Tick(1.0f / 60.0f);
			Result = Read();
			if (Result == ESuperSLMRestoreResult::Pending) { FPlatformProcess::Sleep(0.001f); }
		}
		return Result;
	}

	bool CorruptLayer1Magic(TArray<uint8>& Blob, const ANSICHAR* Magic)
	{
		for (int32 I = 0; I + 3 < Blob.Num(); ++I)
		{
			if (Blob[I] == Magic[0] && Blob[I + 1] == Magic[1] &&
				Blob[I + 2] == Magic[2] && Blob[I + 3] == Magic[3])
			{
				Blob[I] ^= 0xff;
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuRetentionTest,
	"SuperSLM.U1.Cpu.LifecycleResultRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuRetentionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Error;
	if (!TestTrue(TEXT("A-EX artifact"), TryGetAExArtifactPath(Path, Error))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("configure"), (uint8)Cpu->Configure(Model, Config).Result,
		(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMSequence Source;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Source), (uint8)ESuperSLMVendResult::Success)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 1;
	TArray<int32> Tokens;
	if (!TestTrue(TEXT("source generation"), RunGenerationToCompletion(*Cpu, Source, Request, Tokens, 120.0, Error))) { return false; }
	const int64 BaselineBytes = Cpu->GetRetainedResultBytes();
	const int32 BaselineEntries = Cpu->GetLifecycleHandleEntryCount();
	TArray<FSuperSLMLifecycleOpHandle> Saves;
	for (int32 I = 0; I < 16; ++I)
	{
		FSuperSLMLifecycleOpHandle H;
		if (!TestEqual(TEXT("save queues"), (uint8)Cpu->SaveSequence(Source, H, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		if (!TestEqual(TEXT("unread save resolves"), (uint8)PollResult(*Cpu, [Cpu, H]() { return Cpu->GetLifecycleOpResult(H); }),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Saves.Add(H);
	}
	const int64 Retained = Cpu->GetRetainedResultBytes();
	bool bOk = TestTrue(TEXT("sixteen unread saves retain bytes"), Retained > BaselineBytes);
	bOk &= TestEqual(TEXT("sixteen save entries retained"), Cpu->GetLifecycleHandleEntryCount(), BaselineEntries + 16);
	TArray<uint8> Reference;
	int64 TakenBytes = 0;
	for (const FSuperSLMLifecycleOpHandle& H : Saves)
	{
		TArray<uint8> Blob;
		bOk &= TestEqual(TEXT("first save read succeeds"), (uint8)Cpu->GetSaveResult(H, Blob), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("taken blob nonempty"), Blob.Num() > 0);
		if (Reference.IsEmpty()) { Reference = Blob; }
		else { bOk &= TestEqual(TEXT("saved bytes equal reference"), Blob, Reference); }
		TakenBytes += Blob.Num();
		TArray<uint8> Again;
		bOk &= TestEqual(TEXT("second save read consumed"), (uint8)Cpu->GetSaveResult(H, Again), (uint8)ESuperSLMRestoreResult::Consumed);
		bOk &= TestTrue(TEXT("consumed read has no bytes"), Again.IsEmpty());
	}
	bOk &= TestEqual(TEXT("retained bytes equal the sixteen blob sizes"), Retained - BaselineBytes, TakenBytes);
	bOk &= TestEqual(TEXT("taken saves restore byte baseline"), Cpu->GetRetainedResultBytes(), BaselineBytes);
	for (const FSuperSLMLifecycleOpHandle& H : Saves) { bOk &= TestTrue(TEXT("save handle release"), Cpu->ReleaseLifecycleOpHandle(H)); }
	bOk &= TestEqual(TEXT("save entries restore baseline"), Cpu->GetLifecycleHandleEntryCount(), BaselineEntries);
	Cpu->ReturnSequence(Source);
	for (int32 I = 0; I < 40; ++I) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
	TArray<uint8> Corrupt = Reference;
	if (!TestTrue(TEXT("CPU Layer-1 payload found"), CorruptLayer1Magic(Corrupt, "SSB5"))) { return false; }
	for (int32 I = 0; I < 16; ++I)
	{
		FSuperSLMSequence Restored;
		FSuperSLMLifecycleOpHandle H;
		const bool bRefuse = (I & 1) != 0;
		if (!TestEqual(TEXT("restore queues"), (uint8)Cpu->RestoreSequence(bRefuse ? Corrupt : Reference,
			Model, Restored, H, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		const ESuperSLMRestoreResult Result = PollResult(*Cpu, [Cpu, H]() { return Cpu->GetLifecycleOpResult(H); });
		bOk &= bRefuse ? TestTrue(TEXT("corrupt restore refused by Layer 1"),
			Result == ESuperSLMRestoreResult::KvMismatch || Result == ESuperSLMRestoreResult::Malformed)
			: TestEqual(TEXT("valid restore succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
		if (!bRefuse) { Cpu->ReturnSequence(Restored); }
		for (int32 Tick = 0; Tick < 40; ++Tick) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.001f); }
		bOk &= TestTrue(TEXT("restore handle release"), Cpu->ReleaseLifecycleOpHandle(H));
	}
	bOk &= TestEqual(TEXT("CPU bytes return to baseline"), Cpu->GetRetainedResultBytes(), BaselineBytes);
	bOk &= TestEqual(TEXT("CPU entries return to baseline"), Cpu->GetLifecycleHandleEntryCount(), BaselineEntries);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuRetentionTest,
	"SuperSLM.U1.Gpu.LifecycleResultRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRetentionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Error;
	if (!TestTrue(TEXT("A-EX artifact"), TryGetAExArtifactPath(Path, Error))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(4);
	Config.K = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("configure"), (uint8)Gpu->Configure(Model, Config).Result,
		(uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuSequence Source;
	if (!TestEqual(TEXT("vend"), (uint8)Gpu->VendSequence(Source, ESuperSLMGpuDecodePath::Composed),
		(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 1;
	TArray<int32> Tokens;
	if (!TestTrue(TEXT("source generation"), SuperSLML2S2Fixtures::RunGpuGenerationToCompletion(*Gpu, Source, Request,
		Tokens, 120.0, Error))) { return false; }
	const int64 BaselineBytes = Gpu->GetRetainedResultBytes();
	const int32 BaselineEntries = Gpu->GetLifecycleHandleEntryCount();
	TArray<FSuperSLMLifecycleOpHandle> Saves;
	for (int32 I = 0; I < 16; ++I)
	{
		const FSuperSLMLifecycleOpHandle H = Gpu->RequestSaveSequence(Source);
		if (!TestTrue(TEXT("save queues"), H.IsValid())) { return false; }
		if (!TestEqual(TEXT("unread save resolves"), (uint8)PollResult(*Gpu, [Gpu, H]() { return Gpu->GetLifecycleOpResult(H); }),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Saves.Add(H);
	}
	const int64 Retained = Gpu->GetRetainedResultBytes();
	bool bOk = TestTrue(TEXT("sixteen unread saves retain bytes"), Retained > BaselineBytes);
	bOk &= TestEqual(TEXT("sixteen save entries retained"), Gpu->GetLifecycleHandleEntryCount(), BaselineEntries + 16);
	TArray<uint8> Reference;
	int64 TakenBytes = 0;
	for (const FSuperSLMLifecycleOpHandle& H : Saves)
	{
		TArray<uint8> Blob;
		bOk &= TestEqual(TEXT("first save read succeeds"), (uint8)Gpu->GetSaveResult(H, Blob), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("taken blob nonempty"), Blob.Num() > 0);
		if (Reference.IsEmpty()) { Reference = Blob; }
		else { bOk &= TestEqual(TEXT("saved bytes equal reference"), Blob, Reference); }
		TakenBytes += Blob.Num();
		TArray<uint8> Again;
		bOk &= TestEqual(TEXT("second save read consumed"), (uint8)Gpu->GetSaveResult(H, Again), (uint8)ESuperSLMRestoreResult::Consumed);
		bOk &= TestTrue(TEXT("consumed read has no bytes"), Again.IsEmpty());
	}
	bOk &= TestEqual(TEXT("retained bytes equal the sixteen blob sizes"), Retained - BaselineBytes, TakenBytes);
	bOk &= TestEqual(TEXT("taken saves restore byte baseline"), Gpu->GetRetainedResultBytes(), BaselineBytes);
	for (const FSuperSLMLifecycleOpHandle& H : Saves) { bOk &= TestTrue(TEXT("save handle release"), Gpu->ReleaseLifecycleOpHandle(H)); }
	bOk &= TestEqual(TEXT("save entries restore baseline"), Gpu->GetLifecycleHandleEntryCount(), BaselineEntries);
	Gpu->ReturnSequence(Source);
	for (int32 I = 0; I < 40; ++I) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after returning the save source"))) { break; } FPlatformProcess::Sleep(0.001f); }
	TArray<uint8> Corrupt = Reference;
	if (!TestTrue(TEXT("GPU Layer-1 payload found"), CorruptLayer1Magic(Corrupt, "SLM5"))) { return false; }
	for (int32 I = 0; I < 16; ++I)
	{
		const bool bRefuse = (I & 1) != 0;
		const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(bRefuse ? Corrupt : Reference, Model);
		if (!TestTrue(TEXT("restore queues"), H.IsValid())) { return false; }
		FSuperSLMGpuSequence Restored;
		const ESuperSLMRestoreResult Result = PollResult(*Gpu, [Gpu, H, &Restored]() { return Gpu->GetRestoreResult(H, Restored); });
		bOk &= bRefuse ? TestTrue(TEXT("corrupt restore refused by Layer 1"),
			Result == ESuperSLMRestoreResult::KvMismatch || Result == ESuperSLMRestoreResult::Malformed)
			: TestEqual(TEXT("valid restore succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
		if (!bRefuse) { Gpu->ReturnSequence(Restored); }
		for (int32 Tick = 0; Tick < 40; ++Tick) { if (!SuperSLML2S2Fixtures::PacedTick(*this, *Gpu, TEXT("drain after each restore"))) { break; } FPlatformProcess::Sleep(0.001f); }
		bOk &= TestEqual(TEXT("resolved restore op removed"), Gpu->GetRestoreOpEntryCount(), 0);
		bOk &= TestTrue(TEXT("restore handle release"), Gpu->ReleaseLifecycleOpHandle(H));
	}
	bOk &= TestEqual(TEXT("GPU bytes return to baseline"), Gpu->GetRetainedResultBytes(), BaselineBytes);
	bOk &= TestEqual(TEXT("GPU entries return to baseline"), Gpu->GetLifecycleHandleEntryCount(), BaselineEntries);
	return bOk;
}
#endif // WITH_DEV_AUTOMATION_TESTS
