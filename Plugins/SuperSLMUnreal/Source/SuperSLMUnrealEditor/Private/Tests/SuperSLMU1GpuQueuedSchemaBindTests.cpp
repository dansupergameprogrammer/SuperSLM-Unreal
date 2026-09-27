// U1 R-S2g(viii): a GPU schema bind issued behind a caller's queued reset must
// take effect in call order, including a later request to unbind it.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	bool DriveCpu(USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq,
		const FSuperSLMGenerationRequest& Request, TArray<int32>& Out,
		ESuperSLMSequencePhase& OutPhase)
	{
		FString Error;
		if (!Cpu.BeginGeneration(Seq, Request, Error)) { return false; }
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - Start < 90.0)
		{
			Cpu.Tick(1.0f / 60.0f);
			OutPhase = Cpu.GetPhase(Seq);
			if (OutPhase == ESuperSLMSequencePhase::Complete || OutPhase == ESuperSLMSequencePhase::Faulted)
			{
				Out = Cpu.GetGeneratedTokens(Seq);
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}

	bool DriveGpu(FAutomationTestBase& Test, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq,
		const FSuperSLMGenerationRequest& Request, TArray<int32>& Out,
		ESuperSLMSequencePhase& OutPhase)
	{
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestBeginGeneration(Seq, Request);
		const bool bQueued = Test.TestTrue(TEXT("generation request queued"), Handle.IsValid());
		if (!bQueued)
		{
			Test.AddError(FString::Printf(TEXT("generation request refusal: %s"),
				*Gpu.GetLastLifecycleRequestError()));
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		bool bSawNonTerminal = false;
		while (FPlatformTime::Seconds() - Start < 90.0)
		{
			Gpu.Tick(1.0f / 60.0f);
			OutPhase = Gpu.GetPhase(Seq);
			bSawNonTerminal |= OutPhase != ESuperSLMSequencePhase::Complete && OutPhase != ESuperSLMSequencePhase::Faulted;
			if (bSawNonTerminal && (OutPhase == ESuperSLMSequencePhase::Complete || OutPhase == ESuperSLMSequencePhase::Faulted))
			{
				Out = Gpu.GetGeneratedTokens(Seq);
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuQueuedSchemaBindTest,
	"SuperSLM.U1.Gpu.SchemaBindBehindQueuedReset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuQueuedSchemaBindTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Reason;
	if (!TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu) || !TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMRuntimeConfig C;
	C.MaxSequencesPerDecodeCall = 1;
	C.MaxPrefillChunkBudget = 64;
	C.MaxLayerBudget = AExNumHiddenLayers;
	C.BlockCount = 1;
	C.SequenceLifecycleBudgetMs = 1000.0;
	C.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU configure"), (uint8)Cpu->Configure(Model, C).Result,
			(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMGpuRuntimeConfig G;
	G.ContextCap = 4096;
	G.BlockCount = 1;
	G.DispatchBudget = DispatchBudgetForLayersPerSlice(1);
	G.K = AExNumHiddenLayers;
	G.TickBudgetMs = 1000.0;
	G.MaxQueuedOperationsPerSequence = 5;
	if (!TestEqual(TEXT("GPU configure"), (uint8)Gpu->Configure(Model, G).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMSchemaHandle CpuSchema;
	FSuperSLMGpuSchemaHandle GpuSchema;
	FString Error;
	if (!TestTrue(TEXT("CPU schema resolves"), FSuperSLMSchemaLookup::LookupByName(*Model, DemoSchemaName(), CpuSchema, Error)) ||
		!TestTrue(TEXT("GPU schema resolves"), FSuperSLMGpuSchemaLookup::LookupByName(*Model, DemoSchemaName(), GpuSchema, Error))) { return false; }
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("pinned prompt tokenizes"), Cpu->Tokenize(PinnedSelfCheckPrompt(), Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 48;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	TArray<int32> CpuReference[2];
	ESuperSLMSequencePhase CpuPhase[2] = { ESuperSLMSequencePhase::Idle, ESuperSLMSequencePhase::Idle };
	for (int32 Bind = 0; Bind < 2; ++Bind)
	{
		FSuperSLMSequence S;
		if (Cpu->VendSequence(S) != ESuperSLMVendResult::Success) { return false; }
		if (Bind && !TestTrue(TEXT("CPU constrained bind"), Cpu->SetSchema(S, CpuSchema, Error))) { return false; }
		if (!TestTrue(TEXT("CPU reference terminal"), DriveCpu(*Cpu, S, Request, CpuReference[Bind], CpuPhase[Bind]))) { return false; }
		Cpu->ReturnSequence(S);
	}
	if (!TestNotEqual(TEXT("CPU constrained reference discriminates from unconstrained"), CpuReference[0], CpuReference[1])) { return false; }

	bool bOk = true;
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		FSuperSLMGpuSequence S;
		if (!TestEqual(TEXT("GPU vend"), (uint8)Gpu->VendSequence(S, ESuperSLMGpuDecodePath::OneCall),
				(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		TArray<int32> Initial;
		ESuperSLMSequencePhase InitialPhase = ESuperSLMSequencePhase::Idle;
		if (!TestTrue(TEXT("GPU first generation terminal"), DriveGpu(*this, *Gpu, S, Request, Initial, InitialPhase))) { return false; }
		// An admitted save occupies the front. Reset must remain queued when the
		// following SetSchema calls arrive, or an inline FIFO bind can run after it.
		const FSuperSLMLifecycleOpHandle SaveAhead = Gpu->RequestSaveSequence(S);
		bOk &= TestTrue(TEXT("save admitted ahead of reset"), SaveAhead.IsValid());
		const FSuperSLMLifecycleOpHandle Reset = Gpu->RequestResetSequence(S, ESuperSLMGpuDecodePath::OneCall);
		bOk &= TestTrue(TEXT("reset queued"), Reset.IsValid());
		// Deliberately no Tick between these calls: the reset is still in the op log.
		bOk &= TestTrue(TEXT("schema bind behind queued reset accepted"), Gpu->SetSchema(S, GpuSchema, Error));
		if (Arm == 1)
		{
			bOk &= TestTrue(TEXT("later none-schema bind also accepted"),
				Gpu->SetSchema(S, FSuperSLMGpuSchemaHandle(), Error));
		}
		if (!bOk) { return false; }
		TArray<int32> Actual;
		ESuperSLMSequencePhase ActualPhase = ESuperSLMSequencePhase::Idle;
		bOk &= TestTrue(TEXT("generation after queued binds terminal"), DriveGpu(*this, *Gpu, S, Request, Actual, ActualPhase));
		const int32 Expected = Arm == 0 ? 1 : 0;
		bOk &= TestEqual(TEXT("tokens equal CPU reference for final bind"), Actual, CpuReference[Expected]);
		bOk &= TestEqual(TEXT("terminal state equals CPU reference for final bind"),
			(uint8)ActualPhase, (uint8)CpuPhase[Expected]);
		bOk &= TestEqual(TEXT("reported schema is final bind"), Gpu->GetBoundSchema(S).IsNone(), Arm == 1);
		Gpu->ReturnSequence(S);
	}
	// The same four queued operations fill the shipped bound. The fifth request
	// must be refused before a tick, independently of the bind-order oracle above.
	FTestWorldWrapper DefaultW;
	if (!DefaultW.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMGpuSubsystem* DefaultGpu = GetGpuSubsystem(DefaultW.GetTestWorld());
	if (!TestNotNull(TEXT("default-bound GPU subsystem"), DefaultGpu)) { return false; }
	FSuperSLMGpuRuntimeConfig DefaultG = G;
	DefaultG.MaxQueuedOperationsPerSequence = FSuperSLMGpuRuntimeConfig().MaxQueuedOperationsPerSequence;
	if (!TestEqual(TEXT("default queue bound is four"), DefaultG.MaxQueuedOperationsPerSequence, 4)) { return false; }
	if (!TestEqual(TEXT("default-bound GPU configure"), (uint8)DefaultGpu->Configure(Model, DefaultG).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuSequence DefaultS;
	if (!TestEqual(TEXT("default-bound vend"),
			(uint8)DefaultGpu->VendSequence(DefaultS, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	TArray<int32> DefaultInitial;
	ESuperSLMSequencePhase DefaultPhase = ESuperSLMSequencePhase::Idle;
	if (!TestTrue(TEXT("default-bound first generation terminal"),
		DriveGpu(*this, *DefaultGpu, DefaultS, Request, DefaultInitial, DefaultPhase))) { return false; }
	bOk &= TestTrue(TEXT("default-bound save admitted"), DefaultGpu->RequestSaveSequence(DefaultS).IsValid());
	bOk &= TestTrue(TEXT("default-bound reset queued"),
		DefaultGpu->RequestResetSequence(DefaultS, ESuperSLMGpuDecodePath::OneCall).IsValid());
	bOk &= TestTrue(TEXT("default-bound schema bind queued"), DefaultGpu->SetSchema(DefaultS, GpuSchema, Error));
	bOk &= TestTrue(TEXT("default-bound none-schema bind queued"),
		DefaultGpu->SetSchema(DefaultS, FSuperSLMGpuSchemaHandle(), Error));
	if (!bOk) { return false; }
	bOk &= TestFalse(TEXT("fifth request refused at default queue bound"),
		DefaultGpu->RequestBeginGeneration(DefaultS, Request).IsValid());
	bOk &= TestTrue(TEXT("fifth request names configured bound"),
		DefaultGpu->GetLastLifecycleRequestError().Contains(TEXT("configured bound (4)")));
	DefaultGpu->ReturnSequence(DefaultS);
	return bOk;
}

// U1 R-S2g(ix) (plan §9 R-S2g; §2.5 row 21's ruling as folded, review finding 9, restated by
// round 2's finding 1; kills M-64): bind-then-begin at depth 1. SetSchema() always holds the bind
// in the op log; on this fresh sequence's empty log the bind is admitted at the call, and a
// SchemaBind entry that is the submitted front does not count against
// MaxQueuedOperationsPerSequence, so at the legal floor of 1 a RequestBeginGeneration() after
// SetSchema() is still accepted, and
// GetPendingLifecycleOperationCount() reads 1 -- the begin alone -- until the bind finalizes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuBindThenBeginAtDepthOneTest,
	"SuperSLM.U1.Gpu.SchemaBindThenBeginAtDepthOne", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuBindThenBeginAtDepthOneTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Reason;
	if (!TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu) || !TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMRuntimeConfig C;
	C.MaxSequencesPerDecodeCall = 1;
	C.MaxPrefillChunkBudget = 64;
	C.MaxLayerBudget = AExNumHiddenLayers;
	C.BlockCount = 1;
	C.SequenceLifecycleBudgetMs = 1000.0;
	C.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU configure"), (uint8)Cpu->Configure(Model, C).Result,
			(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMGpuRuntimeConfig G;
	G.ContextCap = 4096;
	G.BlockCount = 1;
	G.DispatchBudget = DispatchBudgetForLayersPerSlice(1);
	G.K = AExNumHiddenLayers;
	G.TickBudgetMs = 1000.0;
	G.MaxQueuedOperationsPerSequence = 1; // Configure's floor (D-SLM7475)
	if (!TestEqual(TEXT("GPU configure at MaxQueuedOperationsPerSequence = 1"), (uint8)Gpu->Configure(Model, G).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMSchemaHandle CpuSchema;
	FSuperSLMGpuSchemaHandle GpuSchema;
	FString Error;
	if (!TestTrue(TEXT("CPU schema resolves"), FSuperSLMSchemaLookup::LookupByName(*Model, DemoSchemaName(), CpuSchema, Error)) ||
		!TestTrue(TEXT("GPU schema resolves"), FSuperSLMGpuSchemaLookup::LookupByName(*Model, DemoSchemaName(), GpuSchema, Error))) { return false; }
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("pinned prompt tokenizes"), Cpu->Tokenize(PinnedSelfCheckPrompt(), Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 48;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// The CPU constrained reference.
	TArray<int32> CpuReference;
	ESuperSLMSequencePhase CpuPhase = ESuperSLMSequencePhase::Idle;
	{
		FSuperSLMSequence S;
		if (Cpu->VendSequence(S) != ESuperSLMVendResult::Success) { return false; }
		if (!TestTrue(TEXT("CPU constrained bind"), Cpu->SetSchema(S, CpuSchema, Error))) { return false; }
		if (!TestTrue(TEXT("CPU constrained reference terminal"), DriveCpu(*Cpu, S, Request, CpuReference, CpuPhase))) { return false; }
		Cpu->ReturnSequence(S);
	}

	FSuperSLMGpuSequence S;
	if (!TestEqual(TEXT("GPU vend (fresh sequence)"), (uint8)Gpu->VendSequence(S, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	bool bOk = TestTrue(*FString::Printf(TEXT("SetSchema() returns true at depth 1 (%s)"), *Error), Gpu->SetSchema(S, GpuSchema, Error));
	const FSuperSLMLifecycleOpHandle Begin = Gpu->RequestBeginGeneration(S, Request);
	bOk &= TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() after SetSchema() returns a valid handle at depth 1 (%s)"),
		*Gpu->GetLastLifecycleRequestError()), Begin.IsValid());
	if (!bOk)
	{
		Gpu->ReturnSequence(S);
		return false;
	}
	bOk &= TestEqual(TEXT("GetPendingLifecycleOperationCount() reads 1 (the begin alone) right after both calls return"),
		Gpu->GetPendingLifecycleOperationCount(S), 1);

	// Drive to a terminal phase; until the bind has finalized (GetBoundSchema() reports it), the
	// pending count still reads 1.
	TArray<int32> Tokens;
	ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
	bool bTerminal = false;
	int32 CountReadsBeforeBind = 1;
	int32 CountMismatchesBeforeBind = 0;
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < 90.0)
	{
		Gpu->Tick(1.0f / 60.0f);
		if (Gpu->GetBoundSchema(S).Index != GpuSchema.Index)
		{
			++CountReadsBeforeBind;
			CountMismatchesBeforeBind += Gpu->GetPendingLifecycleOperationCount(S) == 1 ? 0 : 1;
		}
		Phase = Gpu->GetPhase(S);
		if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
		{
			Tokens = Gpu->GetGeneratedTokens(S);
			bTerminal = true;
			break;
		}
		FPlatformProcess::Sleep(1.0f / 60.0f);
	}
	Gpu->ReturnSequence(S);
	bOk &= TestEqual(*FString::Printf(TEXT("GetPendingLifecycleOperationCount() reads 1 on every read before the bind finalizes (%d reads)"), CountReadsBeforeBind),
		CountMismatchesBeforeBind, 0);
	bOk &= TestTrue(TEXT("the depth-1 generation reaches a terminal phase"), bTerminal);
	bOk &= TestEqual(TEXT("the depth-1 generation equals the CPU constrained reference"), Tokens, CpuReference);
	bOk &= TestEqual(TEXT("the depth-1 terminal phase equals the CPU constrained reference's"), (uint8)Phase, (uint8)CpuPhase);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
