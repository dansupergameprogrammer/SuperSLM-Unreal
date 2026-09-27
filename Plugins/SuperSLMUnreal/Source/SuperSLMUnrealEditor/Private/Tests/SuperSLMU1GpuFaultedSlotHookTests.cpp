// The faulted-slot rules no real Layer-1 input reaches, driven through the test-access hooks.
// sslm_gpu_seq_reset's refusals cannot be produced on demand (SSLM_BUSY never occurs on the one
// FIFO, and a lost device takes the terminal path), so SetNextResetStatusOverride() makes the next
// reset return SSLM_BUSY.
//
// A sequence runs one generation at a time: a generation is refused at the call unless the
// sequence will be Idle when its turn comes, assuming every reset queued ahead of it succeeds. The
// one request that can reach its turn on a sequence that is not Idle is a generation behind a reset
// Layer 1 refuses; it resolves ResetRequired there and changes nothing.
//
// RefusedResetLeavesSlotFaulted: a reset Layer 1 refuses resolves Malformed with the status named
// and leaves the sequence Faulted; the next generation, with nothing queued, is refused at the
// call; a reset that then succeeds leaves it Idle.
//
// FaultedCheckPrecedesDeferredRefusal: on a slot that is Faulted AND holds a deferred refusal (the
// one a refused held schema bind leaves), a generation behind a refused reset reaches its turn on
// the Faulted sequence and resolves ResetRequired, and the deferred refusal is kept, not spent.
// After a reset, the next generation is admitted and faulted by name with the held refusal.
// SeedDeferredRefusal() writes the refusal a refused bind writes. With the turn's check below the
// deferred-refusal branch, the generation spends the refusal and resolves Success.
//
// GenerationBehindRefusedResetResolvesResetRequired: a generation runs to Complete; a reset Layer 1
// refuses and a generation are requested in one frame. The reset resolves Malformed, leaves the
// sequence Faulted with the completed generation's tokens intact, and the generation behind it
// resolves ResetRequired at its turn, naming the rule. A reset that succeeds leaves the sequence
// Idle with no tokens, and the next request completes as it does on a fresh sequence.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

// Layer 1's status enum (vendored C++), for the status the reset override returns.
THIRD_PARTY_INCLUDES_START
#include "superslm/gpu_1p0.h"
THIRD_PARTY_INCLUDES_END

using namespace SuperSLML2S2Fixtures;

namespace
{
	// The 17th prompt embed is refused at this cap (D-SLM7379's per-embed check), so a 20-token
	// prompt faults the sequence RecoverablePerSequenceRejection within about twenty ticks.
	constexpr int64 kHookCellContextCap = 16;
	constexpr int32 kHookCellOverCapPromptTokens = 20;
	constexpr double kHookCellDeadlineSeconds = 120.0;

	bool SetUpHookCellGpu(FAutomationTestBase& T, UWorld* World, USuperSLMGpuSubsystem*& OutGpu, int64 ContextCap = kHookCellContextCap, int32 BlockCount = 1)
	{
		FString AExPath, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), Model) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return false;
		}
		OutGpu = GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), OutGpu))
		{
			return false;
		}
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = ContextCap;
		Config.BlockCount = BlockCount;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // one-call path; a whole token's worth
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		Config.MaxQueuedOperationsPerSequence = 4;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	FSuperSLMGenerationRequest MakeHookRequest(const TArray<int32>& Prompt, int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		return Request;
	}

	// Vends a one-call sequence, ticking while the pool's slots are still settling a return.
	bool VendHookSequence(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuSequence& OutSeq, const TCHAR* What)
	{
		const double Start = FPlatformTime::Seconds();
		ESuperSLMGpuVendResult Result = Gpu.VendSequence(OutSeq, ESuperSLMGpuDecodePath::OneCall);
		while (Result == ESuperSLMGpuVendResult::PoolExhausted && FPlatformTime::Seconds() - Start < kHookCellDeadlineSeconds && TickWhenDeviceReady(Gpu))
		{
			Result = Gpu.VendSequence(OutSeq, ESuperSLMGpuDecodePath::OneCall);
		}
		return T.TestEqual(*FString::Printf(TEXT("VendSequence() (%s)"), What), (uint8)Result, (uint8)ESuperSLMGpuVendResult::Success);
	}

	// Vends a one-call sequence and runs an over-cap prompt on it until the sequence is Faulted.
	bool VendAndFault(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuSequence& OutSeq)
	{
		if (!T.TestEqual(TEXT("VendSequence()"), (uint8)Gpu.VendSequence(OutSeq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		TArray<int32> OverCap;
		OverCap.Init(1, kHookCellOverCapPromptTokens);
		const FSuperSLMLifecycleOpHandle H = Gpu.RequestBeginGeneration(OutSeq, MakeHookRequest(OverCap, 8));
		if (!T.TestTrue(TEXT("the over-cap generation is queued"), H.IsValid()) ||
			!T.TestEqual(TEXT("the over-cap generation is admitted"), (uint8)DriveLifecycleOpToResolution(Gpu, H), (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		while (Gpu.GetPhase(OutSeq) != ESuperSLMSequencePhase::Faulted &&
			FPlatformTime::Seconds() - Start < kHookCellDeadlineSeconds && TickWhenDeviceReady(Gpu))
		{
		}
		return T.TestEqual(TEXT("precondition: the over-cap prompt faults the sequence"), (uint8)Gpu.GetPhase(OutSeq), (uint8)ESuperSLMSequencePhase::Faulted) &&
			T.TestEqual(TEXT("precondition: the fault is the context-cap rejection"), (uint8)Gpu.GetLastFaultReason(OutSeq), (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection);
	}

	// Request on a fresh one-call sequence, run to completion; the sequence is returned.
	bool RunHookReference(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGenerationRequest& Request, TArray<int32>& OutTokens, const TCHAR* What)
	{
		FSuperSLMGpuSequence Ref;
		if (!VendHookSequence(T, Gpu, Ref, What))
		{
			return false;
		}
		FString RunError;
		const bool bDone = RunGpuGenerationToCompletion(Gpu, Ref, Request, OutTokens, 60.0, RunError);
		Gpu.ReturnSequence(Ref);
		return T.TestTrue(*FString::Printf(TEXT("%s completes on a fresh sequence (%s)"), What, *RunError), bDone);
	}
}

// --- A reset Layer 1 refuses resolves Malformed and leaves the sequence Faulted, so the next
// generation is refused at the call; a reset that succeeds then leaves the sequence Idle. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuRefusedResetLeavesSlotFaultedTest,
	"SuperSLM.U1.Gpu.RefusedResetLeavesSlotFaulted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRefusedResetLeavesSlotFaultedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpHookCellGpu(*this, W.GetTestWorld(), Gpu))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!VendAndFault(*this, *Gpu, Seq))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	// The refused reset logs one Error on the submission thread ("sslm_gpu_seq_reset refused (SSLM_BUSY)
	// on GPU slot ..."). It is this cell's claim, so it is expected exactly once; the Faulted warning
	// that follows reads "sslm_gpu_seq_reset returned" and does not match.
	AddExpectedErrorPlain(TEXT("sslm_gpu_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);

	// Armed just before the reset it is meant for: the override is taken by the next reset of any kind.
	FSuperSLMGpuTestAccess::SetNextResetStatusOverride(*Gpu, SslmGpuStatus::SSLM_BUSY);
	const ESuperSLMRestoreResult ResetResult = DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall));
	const FString ResetError = Gpu->GetLastLifecycleRequestError();
	const ESuperSLMSequencePhase PhaseAfterRefusedReset = Gpu->GetPhase(Seq);
	AddInfo(FString::Printf(TEXT("refused reset: result %d, phase %d, error '%s'"), (int32)ResetResult, (int32)PhaseAfterRefusedReset, *ResetError));
	bool bOk = TestEqual(TEXT("a reset Layer 1 refuses resolves Malformed"), (uint8)ResetResult, (uint8)ESuperSLMRestoreResult::Malformed);
	bOk &= TestTrue(*FString::Printf(TEXT("the refused reset names the status (got: '%s')"), *ResetError),
		ResetError.Contains(TEXT("sslm_gpu_seq_reset")) && ResetError.Contains(TEXT("SSLM_BUSY")));
	bOk &= TestEqual(TEXT("a refused reset leaves the sequence Faulted, not Idle"), (uint8)PhaseAfterRefusedReset, (uint8)ESuperSLMSequencePhase::Faulted);

	// Nothing is queued after the refused reset, so the next generation is refused at the call.
	const FSuperSLMGenerationRequest Next = MakeHookRequest({1, 2, 3}, 4);
	const FSuperSLMLifecycleOpHandle HBegin = Gpu->RequestBeginGeneration(Seq, Next);
	const FString BeginError = Gpu->GetLastLifecycleRequestError();
	bOk &= TestFalse(*FString::Printf(TEXT("the generation after a refused reset is refused at the call (invalid handle) (%s)"), *BeginError), HBegin.IsValid());
	if (HBegin.IsValid())
	{
		DriveLifecycleOpToResolution(*Gpu, HBegin);
	}
	bOk &= TestEqual(TEXT("the refused generation leaves the sequence Faulted"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);

	// The override was one-shot: the next reset reaches Layer 1 and succeeds.
	bOk &= TestEqual(TEXT("a reset after the override is spent succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the successful reset leaves the sequence Idle"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Idle);
	TArray<int32> Tokens;
	FString RunError;
	bOk &= TestTrue(*FString::Printf(TEXT("the generation after the successful reset completes (%s)"), *RunError),
		RunGpuGenerationToCompletion(*Gpu, Seq, Next, Tokens, 60.0, RunError));
	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- On a Faulted slot that also holds a deferred refusal, a generation behind a refused reset is
// refused ResetRequired at its turn and the refusal is kept; after a reset, the next generation is
// faulted by name with it. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuFaultedCheckPrecedesDeferredRefusalTest,
	"SuperSLM.U1.Gpu.FaultedCheckPrecedesDeferredRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuFaultedCheckPrecedesDeferredRefusalTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpHookCellGpu(*this, W.GetTestWorld(), Gpu))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!VendAndFault(*this, *Gpu, Seq))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	// Plain words only: the fault log line carries it, and it is matched below as a plain substring.
	const FString Seeded = TEXT("seeded deferred refusal for the faulted check order cell");
	// The deferred branch faults the generation by name through FaultSlot(), whose warning carries
	// the refusal: exactly once, and only for the generation after the successful reset.
	AddExpectedErrorPlain(Seeded, EAutomationExpectedErrorFlags::Contains, 1);
	// The refused reset that carries the first generation to its turn logs one Error on the
	// submission thread.
	AddExpectedErrorPlain(TEXT("sslm_gpu_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);
	bool bOk = TestTrue(TEXT("the deferred refusal is seeded on the live sequence"), FSuperSLMGpuTestAccess::SeedDeferredRefusal(*Gpu, Seq, Seeded));

	// A reset Layer 1 refuses, and a generation behind it, in one frame: the generation is accepted
	// at the call (a reset is queued ahead of it) and reaches its turn on the Faulted sequence.
	const FSuperSLMGenerationRequest Next = MakeHookRequest({1, 2, 3}, 4);
	FSuperSLMGpuTestAccess::SetNextResetStatusOverride(*Gpu, SslmGpuStatus::SSLM_BUSY);
	const FSuperSLMLifecycleOpHandle HRefusedReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	const FSuperSLMLifecycleOpHandle H1 = Gpu->RequestBeginGeneration(Seq, Next);
	bOk &= TestTrue(TEXT("the reset on the faulted slot is queued"), HRefusedReset.IsValid());
	bOk &= TestTrue(*FString::Printf(TEXT("the generation behind the reset is queued (%s)"), *Gpu->GetLastLifecycleRequestError()), H1.IsValid());
	const ESuperSLMRestoreResult R1 = H1.IsValid() ? DriveLifecycleOpToResolution(*Gpu, H1) : ESuperSLMRestoreResult::Malformed;
	const FString Error1 = Gpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("begin behind the refused reset: result %d, reset %d, phase %d, error '%s'"),
		(int32)R1, (int32)Gpu->GetLifecycleOpResult(HRefusedReset), (int32)Gpu->GetPhase(Seq), *Error1));
	bOk &= TestEqual(TEXT("with a deferred refusal held, the generation on a faulted slot still resolves ResetRequired"),
		(uint8)R1, (uint8)ESuperSLMRestoreResult::ResetRequired);
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the reset rule, not the held refusal (got: '%s')"), *Error1),
		Error1.Contains(TEXT("reset"), ESearchCase::IgnoreCase) && !Error1.Contains(Seeded));
	bOk &= TestEqual(TEXT("the refused generation leaves the sequence Faulted"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);

	bOk &= TestEqual(TEXT("the reset succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);

	// The held refusal was kept: the first generation after the reset is admitted and faulted by it.
	const FSuperSLMLifecycleOpHandle H2 = Gpu->RequestBeginGeneration(Seq, Next);
	const ESuperSLMRestoreResult R2 = H2.IsValid() ? DriveLifecycleOpToResolution(*Gpu, H2) : ESuperSLMRestoreResult::Malformed;
	AddInfo(FString::Printf(TEXT("begin after the reset: result %d, phase %d, fault %d, tokens %d"),
		(int32)R2, (int32)Gpu->GetPhase(Seq), (int32)Gpu->GetLastFaultReason(Seq), Gpu->GetGeneratedTokens(Seq).Num()));
	bOk &= TestEqual(TEXT("the generation after the reset is admitted"), (uint8)R2, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the kept deferred refusal faults that generation (phase Faulted at admission)"),
		(uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the deferred refusal faults it as a per-sequence rejection"),
		(uint8)Gpu->GetLastFaultReason(Seq), (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection);
	bOk &= TestEqual(TEXT("the refused generation produced no tokens"), Gpu->GetGeneratedTokens(Seq).Num(), 0);

	// The refusal is spent once: after another reset the generation runs.
	bOk &= TestEqual(TEXT("the second reset succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> Tokens;
	FString RunError;
	bOk &= TestTrue(*FString::Printf(TEXT("with the refusal spent, the next generation completes (%s)"), *RunError),
		RunGpuGenerationToCompletion(*Gpu, Seq, Next, Tokens, 60.0, RunError));
	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- A generation behind a reset Layer 1 refuses resolves ResetRequired at its turn; the refused
// reset leaves the sequence Faulted with the completed generation's tokens intact. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuGenerationBehindRefusedResetResolvesResetRequiredTest,
	"SuperSLM.U1.Gpu.GenerationBehindRefusedResetResolvesResetRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuGenerationBehindRefusedResetResolvesResetRequiredTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpHookCellGpu(*this, W.GetTestWorld(), Gpu, /*ContextCap*/ 4096, /*BlockCount*/ 2))
	{
		return false;
	}
	// P runs first on the sequence, Q after the reset: a stale read of P's tokens cannot pass as Q's.
	const FSuperSLMGenerationRequest P = MakeHookRequest({1, 2, 3}, 24);
	const FSuperSLMGenerationRequest Q = MakeHookRequest({4, 5, 6}, 8);
	TArray<int32> RefP, RefQ;
	if (!RunHookReference(*this, *Gpu, P, RefP, TEXT("P")) || !RunHookReference(*this, *Gpu, Q, RefQ, TEXT("Q")))
	{
		return false;
	}
	if (!TestNotEqual(TEXT("precondition: P's and Q's fresh references differ"), RefP, RefQ))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!VendHookSequence(*this, *Gpu, Seq, TEXT("the sequence under test")))
	{
		return false;
	}
	TArray<int32> PTokens;
	FString PError;
	if (!TestTrue(*FString::Printf(TEXT("precondition: P runs to Complete on the sequence (%s)"), *PError), RunGpuGenerationToCompletion(*Gpu, Seq, P, PTokens, 60.0, PError)) ||
		!TestEqual(TEXT("precondition: P's tokens equal its fresh reference"), PTokens, RefP))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	// The refused reset logs one Error on the submission thread; it is expected exactly once.
	AddExpectedErrorPlain(TEXT("sslm_gpu_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);
	FSuperSLMGpuTestAccess::SetNextResetStatusOverride(*Gpu, SslmGpuStatus::SSLM_BUSY);
	const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	const FSuperSLMLifecycleOpHandle HBegin = Gpu->RequestBeginGeneration(Seq, Q);
	const FString BeginCallError = Gpu->GetLastLifecycleRequestError();
	if (!TestTrue(TEXT("precondition: the reset is accepted at the call"), HReset.IsValid()) ||
		!TestTrue(*FString::Printf(TEXT("precondition: the generation behind the reset is accepted at the call (%s)"), *BeginCallError), HBegin.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	const ESuperSLMRestoreResult ResetResult = DriveLifecycleOpToResolution(*Gpu, HReset);
	const ESuperSLMSequencePhase PhaseAfterReset = Gpu->GetPhase(Seq);
	const TArray<int32> TokensAfterReset = Gpu->GetGeneratedTokens(Seq);
	const ESuperSLMRestoreResult BeginResult = DriveLifecycleOpToResolution(*Gpu, HBegin);
	const FString TurnError = Gpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("reset %d, phase %d, %d tokens, begin %d, error '%s'"),
		(int32)ResetResult, (int32)PhaseAfterReset, TokensAfterReset.Num(), (int32)BeginResult, *TurnError));
	bool bOk = TestEqual(TEXT("the refused reset resolves Malformed"), (uint8)ResetResult, (uint8)ESuperSLMRestoreResult::Malformed);
	bOk &= TestEqual(TEXT("a refused reset leaves the sequence Faulted"), (uint8)PhaseAfterReset, (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("a refused reset leaves the previous generation's tokens"), TokensAfterReset, RefP);
	bOk &= TestEqual(TEXT("the generation behind a refused reset resolves ResetRequired at its turn"), (uint8)BeginResult, (uint8)ESuperSLMRestoreResult::ResetRequired);
	bOk &= TestTrue(*FString::Printf(TEXT("the turn's refusal names the rule, the phase Faulted, the reset and the restore (got: '%s')"), *TurnError),
		TurnError.Contains(TEXT("one generation at a time")) && TurnError.Contains(TEXT("Faulted")) &&
		TurnError.Contains(TEXT("reset")) && TurnError.Contains(TEXT("restore")));
	bOk &= TestEqual(TEXT("the refused generation leaves the sequence Faulted"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the refused generation leaves the previous generation's tokens"), Gpu->GetGeneratedTokens(Seq), RefP);

	// Recovery: a reset that succeeds leaves the sequence Idle with no tokens, and Q then completes
	// as it does on a fresh sequence.
	bOk &= TestEqual(TEXT("a reset with the override spent succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the successful reset leaves the sequence Idle"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Idle);
	bOk &= TestEqual(TEXT("the successful reset leaves no tokens"), Gpu->GetGeneratedTokens(Seq).Num(), 0);
	TArray<int32> QTokens;
	FString QError;
	bOk &= TestTrue(*FString::Printf(TEXT("Q completes after the successful reset (%s)"), *QError), RunGpuGenerationToCompletion(*Gpu, Seq, Q, QTokens, 60.0, QError));
	bOk &= TestEqual(TEXT("Q's tokens equal Q's fresh reference"), QTokens, RefQ);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
