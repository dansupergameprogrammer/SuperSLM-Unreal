// A GPU sequence runs one generation at a time. RequestBeginGeneration() is refused at the call --
// an invalid handle, GetLastLifecycleRequestError() naming the rule -- unless the sequence will be
// Idle when the request's turn comes: Idle now with nothing queued, or a reset queued (or admitted
// and not yet resolved) after its last generation. A refused request queues nothing and changes
// nothing on the sequence. The refusal names "one generation at a time", the phase that fails it or
// "already queued", and RequestResetSequence() as the remedy, and never reads as the queue bound.
//
// A reset requested while a generation runs interrupts it: from the reset's admission no token of
// that generation is applied, and the sequence keeps its phase and tokens until the reset succeeds,
// when it is Idle with no tokens.
//
// Every cell that runs two generations on one sequence uses two requests, P ({1, 2, 3}, 24 new
// tokens) and Q ({4, 5, 6}, 8 new tokens), and first asserts that their fresh references differ, so
// a read of P's tokens cannot pass as Q's. Every reference is the same request run on a freshly
// vended sequence, which is independent of the reuse path under test. A cell whose call-time
// assertion fails returns there rather than waiting out a deadline.

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
	constexpr double kOneGenDeadlineSeconds = 120.0;
	// Layers per slice for the composed arm: below the model's 24 layers, so a token takes four slices.
	constexpr int32 kOneGenComposedLayersPerSlice = 6;
	// A prompt long enough to hold the one-call path in Prefilling for dozens of ticks (one prompt
	// token per tick).
	constexpr int32 kOneGenLongPromptTokens = 64;

	FSuperSLMGenerationRequest OneGenRequest(const TArray<int32>& Prompt, int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		return Request;
	}

	FSuperSLMGenerationRequest OneGenP() { return OneGenRequest({1, 2, 3}, 24); }
	FSuperSLMGenerationRequest OneGenQ() { return OneGenRequest({4, 5, 6}, 8); }

	FSuperSLMGenerationRequest OneGenLongPrompt()
	{
		TArray<int32> Prompt;
		for (int32 I = 0; I < kOneGenLongPromptTokens; ++I)
		{
			Prompt.Add(100 + I);
		}
		return OneGenRequest(Prompt, 8);
	}

	const TCHAR* OneGenPhaseName(ESuperSLMSequencePhase Phase)
	{
		switch (Phase)
		{
			case ESuperSLMSequencePhase::Idle: return TEXT("Idle");
			case ESuperSLMSequencePhase::Prefilling: return TEXT("Prefilling");
			case ESuperSLMSequencePhase::Decoding: return TEXT("Decoding");
			case ESuperSLMSequencePhase::Complete: return TEXT("Complete");
			case ESuperSLMSequencePhase::Faulted: return TEXT("Faulted");
		}
		return TEXT("?");
	}

	// The queue bound's own phrases; the one-generation refusal must contain none of them.
	bool OneGenReadsAsBound(const FString& Error)
	{
		return Error.Contains(TEXT("configured bound")) || Error.Contains(TEXT("already full")) || Error.Contains(TEXT("SSLM_SEQUENCE_QUEUE_FULL"));
	}

	// One-call configuration on A-EX; K is the model's depth, so a decoding sequence holds tokens in
	// flight (each applies K ticks after it is requested).
	bool SetUpOneGenGpu(FAutomationTestBase& T, UWorld* World, USuperSLMGpuSubsystem*& OutGpu, int32 BlockCount = 2, int32 MaxQueuedOperations = 4)
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
		Config.ContextCap = 4096;
		Config.BlockCount = BlockCount;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // a whole token per tick; composed sequences cap their own slice
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		Config.MaxQueuedOperationsPerSequence = MaxQueuedOperations;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	// Paced ticks until Done() or the deadline; true when Done() held.
	template <typename FDone>
	bool OneGenTickUntil(USuperSLMGpuSubsystem& Gpu, FDone Done, double DeadlineSeconds = kOneGenDeadlineSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done())
		{
			if (FPlatformTime::Seconds() - Start > DeadlineSeconds || !TickWhenDeviceReady(Gpu))
			{
				return Done();
			}
		}
		return true;
	}

	// Vends a sequence on Path, ticking while the pool's slots are still settling a return; a
	// composed sequence is capped at kOneGenComposedLayersPerSlice.
	bool OneGenVend(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuSequence& OutSeq, const TCHAR* What,
		ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::OneCall)
	{
		const double Start = FPlatformTime::Seconds();
		ESuperSLMGpuVendResult Result = Gpu.VendSequence(OutSeq, Path);
		while (Result == ESuperSLMGpuVendResult::PoolExhausted && FPlatformTime::Seconds() - Start < kOneGenDeadlineSeconds && TickWhenDeviceReady(Gpu))
		{
			Result = Gpu.VendSequence(OutSeq, Path);
		}
		if (!T.TestEqual(*FString::Printf(TEXT("VendSequence() (%s)"), What), (uint8)Result, (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		if (Path == ESuperSLMGpuDecodePath::Composed)
		{
			FString Error;
			return T.TestTrue(*FString::Printf(TEXT("SetLayersPerSlice(%d) on the composed sequence (%s): %s"), kOneGenComposedLayersPerSlice, What, *Error),
				Gpu.SetLayersPerSlice(OutSeq, kOneGenComposedLayersPerSlice, Error));
		}
		return true;
	}

	// Request on a fresh sequence of Path, run to completion; the sequence is returned.
	bool OneGenReference(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGenerationRequest& Request, TArray<int32>& OutTokens,
		const TCHAR* What, ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::OneCall)
	{
		FSuperSLMGpuSequence Ref;
		if (!OneGenVend(T, Gpu, Ref, What, Path))
		{
			return false;
		}
		FString RunError;
		const bool bDone = RunGpuGenerationToCompletion(Gpu, Ref, Request, OutTokens, 60.0, RunError);
		Gpu.ReturnSequence(Ref);
		return T.TestTrue(*FString::Printf(TEXT("%s completes on a fresh sequence (%s)"), What, *RunError), bDone);
	}

	// P's and Q's fresh references, and the precondition that they differ.
	bool OneGenReferencesPQ(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, TArray<int32>& OutP, TArray<int32>& OutQ)
	{
		return OneGenReference(T, Gpu, OneGenP(), OutP, TEXT("P's reference")) &&
			OneGenReference(T, Gpu, OneGenQ(), OutQ, TEXT("Q's reference")) &&
			T.TestNotEqual(TEXT("precondition: P's and Q's fresh references differ"), OutP, OutQ);
	}

	// Begins Request on Seq, drives its handle to Success (the generation has started), then ticks
	// until Ready() holds.
	template <typename FReady>
	bool OneGenStart(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const FSuperSLMGenerationRequest& Request,
		const TCHAR* What, FReady Ready)
	{
		const FSuperSLMLifecycleOpHandle H = Gpu.RequestBeginGeneration(Seq, Request);
		if (!T.TestTrue(*FString::Printf(TEXT("%s is accepted (%s)"), What, *Gpu.GetLastLifecycleRequestError()), H.IsValid()) ||
			!T.TestEqual(*FString::Printf(TEXT("%s starts"), What), (uint8)DriveLifecycleOpToResolution(Gpu, H), (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		return T.TestTrue(*FString::Printf(TEXT("%s reaches the phase the cell requests at"), What), OneGenTickUntil(Gpu, Ready));
	}

	// Decoding with at least one token applied and fewer than MaxNewTokens.
	bool OneGenDecodingMidRun(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, int32 MaxNewTokens)
	{
		const int32 N = Gpu.GetGeneratedTokens(Seq).Num();
		return Gpu.GetPhase(Seq) == ESuperSLMSequencePhase::Decoding && N >= 1 && N < MaxNewTokens;
	}
}

// --- G2: a generation on a Complete sequence is refused at the call; after a reset, Q completes
// as it does on a fresh sequence. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuGenerationRefusedAtCallOnCompleteSequenceTest,
	"SuperSLM.U1.Gpu.GenerationRefusedAtCallOnCompleteSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuGenerationRefusedAtCallOnCompleteSequenceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	TArray<int32> RefP, RefQ;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu) || !OneGenReferencesPQ(*this, *Gpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!OneGenVend(*this, *Gpu, Seq, TEXT("the sequence under test")))
	{
		return false;
	}
	TArray<int32> PTokens;
	FString RunError;
	if (!TestTrue(*FString::Printf(TEXT("precondition: P runs to Complete (%s)"), *RunError), RunGpuGenerationToCompletion(*Gpu, Seq, OneGenP(), PTokens, 60.0, RunError)) ||
		!TestEqual(TEXT("precondition: the sequence is Complete"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	const FSuperSLMLifecycleOpHandle H = Gpu->RequestBeginGeneration(Seq, OneGenQ());
	const FString Error = Gpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("begin on the Complete sequence: handle %lld, error '%s'"), H.Id, *Error));
	if (!TestFalse(TEXT("the generation on a Complete sequence is refused at the call (invalid handle)"), H.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	bool bOk = TestTrue(*FString::Printf(TEXT("the refusal names the rule, Complete, RequestResetSequence and that continuing a completed generation is not supported (got: '%s')"), *Error),
		Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Complete")) && Error.Contains(TEXT("RequestResetSequence")) &&
		Error.Contains(TEXT("a generation that continues a completed one is not supported")));
	bOk &= TestFalse(*FString::Printf(TEXT("the refusal does not read as the queue bound (got: '%s')"), *Error), OneGenReadsAsBound(Error));
	bOk &= TestEqual(TEXT("the refused request leaves the phase Complete"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= TestEqual(TEXT("the refused request leaves P's tokens"), Gpu->GetGeneratedTokens(Seq), PTokens);
	bOk &= TestEqual(TEXT("the refused request queues nothing (pending count)"), Gpu->GetPendingLifecycleOperationCount(Seq), 0);

	bOk &= TestEqual(TEXT("a reset, driven to resolution, succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> QTokens;
	bOk &= TestTrue(*FString::Printf(TEXT("Q completes after the reset (%s)"), *RunError), RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), QTokens, 60.0, RunError));
	bOk &= TestEqual(TEXT("Q's tokens after the reset equal Q's fresh reference"), QTokens, RefQ);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- G3: a generation requested while one runs is refused at the call, in three arms: one-call
// while Decoding, composed (LayersPerSlice below the layer count) while Decoding, and one-call with
// a long prompt while Prefilling. The running generation then completes equal to its reference. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuGenerationRefusedAtCallWhileRunningTest,
	"SuperSLM.U1.Gpu.GenerationRefusedAtCallWhileRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuGenerationRefusedAtCallWhileRunningTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu))
	{
		return false;
	}

	struct FArm
	{
		const TCHAR* Name;
		ESuperSLMGpuDecodePath Path;
		FSuperSLMGenerationRequest Request;
		ESuperSLMSequencePhase PhaseAtRequest;
	};
	const FArm Arms[] = {
		{ TEXT("(a) one-call, Decoding"), ESuperSLMGpuDecodePath::OneCall, OneGenP(), ESuperSLMSequencePhase::Decoding },
		{ TEXT("(b) composed, Decoding"), ESuperSLMGpuDecodePath::Composed, OneGenP(), ESuperSLMSequencePhase::Decoding },
		{ TEXT("(c) one-call, Prefilling"), ESuperSLMGpuDecodePath::OneCall, OneGenLongPrompt(), ESuperSLMSequencePhase::Prefilling },
	};
	bool bOk = true;
	for (const FArm& Arm : Arms)
	{
		TArray<int32> Reference;
		if (!OneGenReference(*this, *Gpu, Arm.Request, Reference, *FString::Printf(TEXT("arm %s's reference"), Arm.Name), Arm.Path))
		{
			return false;
		}
		FSuperSLMGpuSequence Seq;
		if (!OneGenVend(*this, *Gpu, Seq, Arm.Name, Arm.Path))
		{
			return false;
		}
		const int32 MaxNew = Arm.Request.MaxNewTokens;
		const ESuperSLMSequencePhase Want = Arm.PhaseAtRequest;
		if (!OneGenStart(*this, *Gpu, Seq, Arm.Request, *FString::Printf(TEXT("arm %s: the running generation"), Arm.Name),
				[Gpu, &Seq, Want, MaxNew]() { return Want == ESuperSLMSequencePhase::Decoding ? OneGenDecodingMidRun(*Gpu, Seq, MaxNew) : Gpu->GetPhase(Seq) == Want; }))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		const ESuperSLMSequencePhase PhaseAtRequest = Gpu->GetPhase(Seq);
		const FSuperSLMLifecycleOpHandle H = Gpu->RequestBeginGeneration(Seq, OneGenQ());
		const FString Error = Gpu->GetLastLifecycleRequestError();
		AddInfo(FString::Printf(TEXT("arm %s: phase %s at the request, handle %lld, error '%s'"), Arm.Name, OneGenPhaseName(PhaseAtRequest), H.Id, *Error));
		if (!TestEqual(*FString::Printf(TEXT("precondition: arm %s: the phase at the request"), Arm.Name), (uint8)PhaseAtRequest, (uint8)Want))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(*FString::Printf(TEXT("the generation requested while one runs is refused at the call (invalid handle), arm %s"), Arm.Name), H.IsValid()))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm %s: the refusal names the rule, the phase %s and RequestResetSequence (got: '%s')"), Arm.Name, OneGenPhaseName(Want), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(OneGenPhaseName(Want)) && Error.Contains(TEXT("RequestResetSequence")));
		bOk &= TestFalse(*FString::Printf(TEXT("arm %s: the refusal does not read as the queue bound (got: '%s')"), Arm.Name, *Error), OneGenReadsAsBound(Error));

		TArray<int32> Tokens;
		FString RunError;
		bOk &= TestTrue(*FString::Printf(TEXT("arm %s: the running generation completes (%s)"), Arm.Name, *RunError),
			RunGpuGenerationToCompletion(*Gpu, Seq, Arm.Request, Tokens, 60.0, RunError, /*bAlreadyInProgress*/ true));
		bOk &= TestEqual(*FString::Printf(TEXT("arm %s: the running generation's tokens equal its fresh reference"), Arm.Name), Tokens, Reference);
		Gpu->ReturnSequence(Seq);
	}
	return bOk;
}

// --- G4: a generation requested behind a queued generation is refused at the call, "already
// queued": (a) on a fresh sequence behind a save and a queued generation, (b) while a generation
// runs, behind a save, a reset and a queued generation; (c) at a full queue (bound 1), the
// one-generation refusal comes before the bound's. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuGenerationRefusedAtCallBehindQueuedGenerationTest,
	"SuperSLM.U1.Gpu.GenerationRefusedAtCallBehindQueuedGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuGenerationRefusedAtCallBehindQueuedGenerationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	TArray<int32> RefP, RefQ;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu) || !OneGenReferencesPQ(*this, *Gpu, RefP, RefQ))
	{
		return false;
	}
	bool bOk = true;

	// (a) A fresh sequence: a save, admitted at its call, holds the front; generation A queues
	// behind it; generation B behind A is refused.
	{
		FSuperSLMGpuSequence Seq;
		if (!OneGenVend(*this, *Gpu, Seq, TEXT("arm (a)")))
		{
			return false;
		}
		const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(Seq);
		const FSuperSLMLifecycleOpHandle HA = Gpu->RequestBeginGeneration(Seq, OneGenP());
		const ESuperSLMRestoreResult AAtRequest = Gpu->GetLifecycleOpResult(HA);
		const FSuperSLMLifecycleOpHandle HB = Gpu->RequestBeginGeneration(Seq, OneGenQ());
		const FString Error = Gpu->GetLastLifecycleRequestError();
		AddInfo(FString::Printf(TEXT("arm (a): save %lld, A %lld (result %d), B %lld, error '%s'"), HSave.Id, HA.Id, (int32)AAtRequest, HB.Id, *Error));
		if (!TestTrue(TEXT("precondition: arm (a): the save is accepted"), HSave.IsValid()) ||
			!TestTrue(TEXT("precondition: arm (a): generation A is accepted"), HA.IsValid()) ||
			!TestEqual(TEXT("precondition: arm (a): generation A is queued behind the save (Pending)"), (uint8)AAtRequest, (uint8)ESuperSLMRestoreResult::Pending))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(TEXT("a generation requested behind a queued generation is refused at the call (invalid handle), arm (a)"), HB.IsValid()))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (a): the refusal names the rule, 'already queued' and RequestResetSequence (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("already queued")) && Error.Contains(TEXT("RequestResetSequence")));
		bOk &= TestFalse(*FString::Printf(TEXT("arm (a): the refusal does not read as the queue bound (got: '%s')"), *Error), OneGenReadsAsBound(Error));
		bOk &= TestEqual(TEXT("arm (a): generation A starts once the save resolves"),
			(uint8)DriveLifecycleOpToResolution(*Gpu, HA), (uint8)ESuperSLMRestoreResult::Success);
		TArray<int32> ATokens;
		FString RunError;
		bOk &= TestTrue(*FString::Printf(TEXT("arm (a): generation A completes (%s)"), *RunError),
			RunGpuGenerationToCompletion(*Gpu, Seq, OneGenP(), ATokens, 60.0, RunError, /*bAlreadyInProgress*/ true));
		bOk &= TestEqual(TEXT("arm (a): generation A's tokens equal P's fresh reference"), ATokens, RefP);
		Gpu->ReturnSequence(Seq);
	}

	// (b) While a generation runs: a save, a reset and generation A (all accepted), then B, refused.
	{
		FSuperSLMGpuSequence Seq;
		if (!OneGenVend(*this, *Gpu, Seq, TEXT("arm (b)")))
		{
			return false;
		}
		const int32 MaxNew = OneGenP().MaxNewTokens;
		if (!OneGenStart(*this, *Gpu, Seq, OneGenP(), TEXT("arm (b): the running generation"),
				[Gpu, &Seq, MaxNew]() { return OneGenDecodingMidRun(*Gpu, Seq, MaxNew); }))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(Seq);
		const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
		const FSuperSLMLifecycleOpHandle HA = Gpu->RequestBeginGeneration(Seq, OneGenQ());
		const FSuperSLMLifecycleOpHandle HB = Gpu->RequestBeginGeneration(Seq, OneGenQ());
		const FString Error = Gpu->GetLastLifecycleRequestError();
		AddInfo(FString::Printf(TEXT("arm (b): save %lld, reset %lld, A %lld, B %lld, error '%s'"), HSave.Id, HReset.Id, HA.Id, HB.Id, *Error));
		if (!TestTrue(TEXT("precondition: arm (b): the save, the reset and generation A are accepted"), HSave.IsValid() && HReset.IsValid() && HA.IsValid()))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(TEXT("a generation requested behind a queued generation is refused at the call (invalid handle), arm (b)"), HB.IsValid()))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (b): the refusal names the rule and 'already queued' (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("already queued")));
		// Generation A, behind the reset, then runs as Q does on a fresh sequence.
		bOk &= TestEqual(TEXT("arm (b): generation A starts once the reset resolves"),
			(uint8)DriveLifecycleOpToResolution(*Gpu, HA), (uint8)ESuperSLMRestoreResult::Success);
		TArray<int32> ATokens;
		FString RunError;
		bOk &= TestTrue(*FString::Printf(TEXT("arm (b): generation A completes (%s)"), *RunError),
			RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), ATokens, 60.0, RunError, /*bAlreadyInProgress*/ true));
		bOk &= TestEqual(TEXT("arm (b): generation A's tokens equal Q's fresh reference"), ATokens, RefQ);
		Gpu->ReturnSequence(Seq);
	}

	// (c) Precedence over the bound: at MaxQueuedOperationsPerSequence 1, a running generation and a
	// save pending behind it fill the window; a generation requested then is refused by the
	// one-generation rule, whose reason names it, not the bound.
	FTestWorldWrapper BoundW;
	if (!BoundW.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* BoundGpu = nullptr;
	if (!SetUpOneGenGpu(*this, BoundW.GetTestWorld(), BoundGpu, /*BlockCount*/ 2, /*MaxQueued*/ 1))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!OneGenVend(*this, *BoundGpu, Seq, TEXT("arm (c)")))
	{
		return false;
	}
	const int32 MaxNew = OneGenP().MaxNewTokens;
	if (!OneGenStart(*this, *BoundGpu, Seq, OneGenP(), TEXT("arm (c): the running generation"),
			[BoundGpu, &Seq, MaxNew]() { return OneGenDecodingMidRun(*BoundGpu, Seq, MaxNew); }))
	{
		BoundGpu->ReturnSequence(Seq);
		return false;
	}
	const FSuperSLMLifecycleOpHandle HSave = BoundGpu->RequestSaveSequence(Seq);
	const int32 PendingWithSave = BoundGpu->GetPendingLifecycleOperationCount(Seq);
	const FSuperSLMLifecycleOpHandle H = BoundGpu->RequestBeginGeneration(Seq, OneGenQ());
	const FString Error = BoundGpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("arm (c): save %lld, pending %d, begin %lld, error '%s'"), HSave.Id, PendingWithSave, H.Id, *Error));
	if (!TestTrue(TEXT("precondition: arm (c): the save is accepted at bound 1"), HSave.IsValid()) ||
		!TestEqual(TEXT("precondition: arm (c): the window is full (the running generation and the save)"), PendingWithSave, 2))
	{
		BoundGpu->ReturnSequence(Seq);
		return false;
	}
	bOk &= TestFalse(TEXT("arm (c): the generation at a full queue is refused at the call (invalid handle)"), H.IsValid());
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal at a full queue names the one-generation rule, not the bound (got: '%s')"), *Error),
		Error.Contains(TEXT("one generation at a time")) && !OneGenReadsAsBound(Error));
	BoundGpu->ReturnSequence(Seq);
	return bOk;
}

// --- G5: a generation requested behind a queued reset is accepted at the call. While A runs, a
// save (admitted, holding the front), a reset (queued behind it) and B, in one frame. The reset's
// admission interrupts A -- it is Decoding with fewer than its MaxNewTokens tokens then, and never
// reads Complete before the reset resolves -- and B then starts and completes as Q does on a fresh
// sequence. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuGenerationAcceptedBehindQueuedResetTest,
	"SuperSLM.U1.Gpu.GenerationAcceptedBehindQueuedReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuGenerationAcceptedBehindQueuedResetTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	TArray<int32> RefP, RefQ;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu) || !OneGenReferencesPQ(*this, *Gpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!OneGenVend(*this, *Gpu, Seq, TEXT("the sequence under test")))
	{
		return false;
	}
	const int32 MaxNew = OneGenP().MaxNewTokens;
	if (!OneGenStart(*this, *Gpu, Seq, OneGenP(), TEXT("generation A (P)"), [Gpu, &Seq, MaxNew]() { return OneGenDecodingMidRun(*Gpu, Seq, MaxNew); }))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(Seq);
	const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	const FSuperSLMLifecycleOpHandle HB = Gpu->RequestBeginGeneration(Seq, OneGenQ());
	const FString Error = Gpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("save %lld, reset %lld, B %lld, error '%s'"), HSave.Id, HReset.Id, HB.Id, *Error));
	if (!TestTrue(TEXT("precondition: the save and the reset are accepted"), HSave.IsValid() && HReset.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	if (!TestTrue(*FString::Printf(TEXT("a generation requested behind a queued reset is accepted at the call (%s)"), *Error), HB.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	bool bOk = TestEqual(TEXT("the accepted generation is Pending at the call"), (uint8)Gpu->GetLifecycleOpResult(HB), (uint8)ESuperSLMRestoreResult::Pending);

	// Until the reset resolves: A never reads Complete, and holds fewer than its MaxNewTokens tokens.
	// The reset is admitted in the same pass that resolves the save, so the first observation with
	// the save resolved is the reset's admission.
	bool bAdmissionObserved = false;
	ESuperSLMSequencePhase PhaseAtAdmission = ESuperSLMSequencePhase::Idle;
	int32 TokensAtAdmission = -1;
	int32 TicksCompleteBeforeReset = 0;
	int32 TicksFinishedBeforeReset = 0;
	const bool bResolved = OneGenTickUntil(*Gpu, [&]()
	{
		if (Gpu->GetLifecycleOpResult(HReset) != ESuperSLMRestoreResult::Pending)
		{
			return true;
		}
		const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Seq);
		const int32 N = Gpu->GetGeneratedTokens(Seq).Num();
		if (!bAdmissionObserved && Gpu->GetLifecycleOpResult(HSave) != ESuperSLMRestoreResult::Pending)
		{
			bAdmissionObserved = true;
			PhaseAtAdmission = Phase;
			TokensAtAdmission = N;
		}
		TicksCompleteBeforeReset += Phase == ESuperSLMSequencePhase::Complete ? 1 : 0;
		TicksFinishedBeforeReset += N >= MaxNew ? 1 : 0;
		return false;
	});
	AddInfo(FString::Printf(TEXT("at the reset's admission: phase %s, %d tokens; reset %d, B %d"),
		OneGenPhaseName(PhaseAtAdmission), TokensAtAdmission, (int32)Gpu->GetLifecycleOpResult(HReset), (int32)Gpu->GetLifecycleOpResult(HB)));
	bOk &= TestTrue(TEXT("precondition: the reset's admission was observed (the save resolved before the reset did)"), bAdmissionObserved);
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: at the reset's admission A is Decoding with fewer than its %d tokens (phase %s, %d tokens)"),
		MaxNew, OneGenPhaseName(PhaseAtAdmission), TokensAtAdmission),
		PhaseAtAdmission == ESuperSLMSequencePhase::Decoding && TokensAtAdmission >= 0 && TokensAtAdmission < MaxNew);
	bOk &= TestTrue(TEXT("the reset resolves"), bResolved);
	bOk &= TestEqual(TEXT("A was interrupted, not finished: it never reads Complete before the reset resolves (ticks observed Complete)"), TicksCompleteBeforeReset, 0);
	bOk &= TestEqual(TEXT("A was interrupted, not finished: it never holds its MaxNewTokens tokens before the reset resolves (ticks observed)"), TicksFinishedBeforeReset, 0);
	bOk &= TestEqual(TEXT("the reset succeeds"), (uint8)Gpu->GetLifecycleOpResult(HReset), (uint8)ESuperSLMRestoreResult::Success);

	bOk &= TestEqual(TEXT("B starts once the reset succeeds"), (uint8)DriveLifecycleOpToResolution(*Gpu, HB), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> BTokens;
	FString RunError;
	bOk &= TestTrue(*FString::Printf(TEXT("B completes (%s)"), *RunError),
		RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), BTokens, 60.0, RunError, /*bAlreadyInProgress*/ true));
	bOk &= TestEqual(TEXT("B's tokens equal Q's fresh reference"), BTokens, RefQ);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- G7: RunGpuGenerationToCompletion() on a reused sequence. (a) With no reset it returns false,
// naming the call-time refusal, and leaves its output as the caller passed it. (b) Behind a reset
// Layer 1 refuses it returns false naming ResetRequired. (c) Behind a reset that is not driven
// first, it waits for the new generation to start and returns the new generation's tokens, not the
// previous ones the sequence still reads while the reset runs. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuFixtureRefusesAReusedSequenceTest,
	"SuperSLM.U1.Gpu.FixtureRefusesAReusedSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuFixtureRefusesAReusedSequenceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	TArray<int32> RefP, RefQ;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu) || !OneGenReferencesPQ(*this, *Gpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!OneGenVend(*this, *Gpu, Seq, TEXT("the sequence under test")))
	{
		return false;
	}
	TArray<int32> PTokens;
	FString RunError;
	if (!TestTrue(*FString::Printf(TEXT("precondition: P runs to Complete (%s)"), *RunError), RunGpuGenerationToCompletion(*Gpu, Seq, OneGenP(), PTokens, 60.0, RunError)))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	// (a) No reset: the call is refused, and the fixture says so.
	TArray<int32> OutA;
	FString ErrorA;
	const bool bA = RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), OutA, 30.0, ErrorA);
	AddInfo(FString::Printf(TEXT("(a): returned %d, %d tokens, error '%s'"), bA ? 1 : 0, OutA.Num(), *ErrorA));
	bool bOk = TestFalse(TEXT("(a) the fixture on a Complete sequence with no reset returns false"), bA);
	bOk &= TestTrue(*FString::Printf(TEXT("(a) its error names the call-time refusal (got: '%s')"), *ErrorA), ErrorA.Contains(TEXT("one generation at a time")));
	bOk &= TestEqual(TEXT("(a) its output is left as the caller passed it (empty), not the previous tokens"), OutA.Num(), 0);

	// (b) Behind a reset Layer 1 refuses: the generation resolves ResetRequired at its turn, and the
	// fixture names it. The refused reset logs one Error on the submission thread.
	AddExpectedErrorPlain(TEXT("sslm_gpu_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);
	FSuperSLMGpuTestAccess::SetNextResetStatusOverride(*Gpu, SslmGpuStatus::SSLM_BUSY);
	const FSuperSLMLifecycleOpHandle HRefused = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	TArray<int32> OutB;
	FString ErrorB;
	const bool bB = HRefused.IsValid() && RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), OutB, 30.0, ErrorB);
	AddInfo(FString::Printf(TEXT("(b): reset %d, returned %d, %d tokens, error '%s'"), (int32)Gpu->GetLifecycleOpResult(HRefused), bB ? 1 : 0, OutB.Num(), *ErrorB));
	bOk &= TestTrue(TEXT("precondition: (b) the reset is accepted"), HRefused.IsValid());
	bOk &= TestFalse(TEXT("(b) the fixture behind a refused reset returns false"), bB);
	bOk &= TestTrue(*FString::Printf(TEXT("(b) its error names ResetRequired (got: '%s')"), *ErrorB), ErrorB.Contains(TEXT("ResetRequired")));

	// Recovery for (c): a reset that succeeds, then P to Complete again.
	bOk &= TestEqual(TEXT("a reset with the override spent succeeds"),
		(uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> PAgain;
	if (!TestTrue(*FString::Printf(TEXT("precondition: (c) P runs to Complete again (%s)"), *RunError), RunGpuGenerationToCompletion(*Gpu, Seq, OneGenP(), PAgain, 60.0, RunError)) ||
		!TestEqual(TEXT("precondition: (c) the sequence is Complete with P's tokens"), PAgain, RefP))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}

	// (c) The wait for Success: a reset not driven first, and the fixture with Q in the same frame.
	const FSuperSLMLifecycleOpHandle HUndriven = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	TArray<int32> OutC;
	FString ErrorC;
	const bool bC = HUndriven.IsValid() && RunGpuGenerationToCompletion(*Gpu, Seq, OneGenQ(), OutC, 60.0, ErrorC);
	AddInfo(FString::Printf(TEXT("(c): returned %d, %d tokens, error '%s'"), bC ? 1 : 0, OutC.Num(), *ErrorC));
	bOk &= TestTrue(*FString::Printf(TEXT("(c) the fixture behind an undriven reset completes (%s)"), *ErrorC), bC);
	bOk &= TestTrue(TEXT("the fixture behind an undriven reset returns the new generation's tokens"), OutC == RefQ && OutC != RefP);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- G10: the call's verdict does not depend on when ticks run. On a fresh sequence per point, P
// run to Complete: with a reset requested and n paced ticks before the begin, for n = 0 .. K + 1
// and once more after the reset has resolved, every begin is accepted; with no reset, at the same
// points, every begin is refused. An accepted begin's generation is not driven. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuOneGenerationVerdictIndependentOfTicksTest,
	"SuperSLM.U1.Gpu.OneGenerationVerdictIndependentOfTicks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuOneGenerationVerdictIndependentOfTicksTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu))
	{
		return false;
	}
	const int32 K = Gpu->GetConfiguredK();
	if (!TestEqual(TEXT("precondition: K is the configured 24"), K, AExNumHiddenLayers))
	{
		return false;
	}
	// Points 0 .. K + 1 are tick counts; the point after them is "once the reset has resolved".
	const int32 AfterResolvedPoint = K + 2;
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		const bool bWithReset = Arm == 0;
		for (int32 Point = 0; Point <= AfterResolvedPoint; ++Point)
		{
			FSuperSLMGpuSequence Seq;
			if (!OneGenVend(*this, *Gpu, Seq, TEXT("a point's sequence")))
			{
				return false;
			}
			TArray<int32> PTokens;
			FString RunError;
			if (!TestTrue(*FString::Printf(TEXT("precondition: P runs to Complete (%s)"), *RunError), RunGpuGenerationToCompletion(*Gpu, Seq, OneGenP(), PTokens, 60.0, RunError)))
			{
				Gpu->ReturnSequence(Seq);
				return false;
			}
			FSuperSLMLifecycleOpHandle HReset;
			if (bWithReset)
			{
				HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
				if (!TestTrue(TEXT("precondition: the reset is accepted"), HReset.IsValid()))
				{
					Gpu->ReturnSequence(Seq);
					return false;
				}
			}
			if (Point == AfterResolvedPoint)
			{
				if (bWithReset)
				{
					if (!TestEqual(TEXT("precondition: the reset resolves Success before the last point's begin"),
							(uint8)DriveLifecycleOpToResolution(*Gpu, HReset), (uint8)ESuperSLMRestoreResult::Success))
					{
						Gpu->ReturnSequence(Seq);
						return false;
					}
				}
				else
				{
					for (int32 T = 0; T < K + 2; ++T)
					{
						PacedTick(*this, *Gpu, TEXT("the no-reset arm's last point"));
					}
				}
			}
			else
			{
				for (int32 T = 0; T < Point; ++T)
				{
					PacedTick(*this, *Gpu, TEXT("between the enabling call and the begin"));
				}
			}
			const FSuperSLMLifecycleOpHandle HBegin = Gpu->RequestBeginGeneration(Seq, OneGenQ());
			const FString Error = Gpu->GetLastLifecycleRequestError();
			const FString PointName = Point == AfterResolvedPoint ? FString(TEXT("after the reset resolved")) : FString::Printf(TEXT("n = %d"), Point);
			bool bVerdict = true;
			if (bWithReset)
			{
				bVerdict = TestTrue(*FString::Printf(TEXT("with a reset queued, the begin on a Complete sequence is accepted at every point (%s; %s)"), *PointName, *Error), HBegin.IsValid());
			}
			else
			{
				bVerdict = TestFalse(*FString::Printf(TEXT("without a reset, the begin on a Complete sequence is refused at every point (%s)"), *PointName), HBegin.IsValid());
				bVerdict &= TestTrue(*FString::Printf(TEXT("without a reset, the refusal names the rule and Complete (%s; got: '%s')"), *PointName, *Error),
					Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Complete")));
			}
			Gpu->ReturnSequence(Seq);
			if (!bVerdict)
			{
				return false;
			}
		}
	}
	return true;
}

// --- G11: a reset admitted at the call on a decoding sequence stops the running generation at its
// admission. K is the model's depth, so the decoding sequence has tokens in flight: its pending
// events are dropped at the admission, no token of it is applied afterwards, and the phase stays
// Decoding until the reset succeeds, which leaves the sequence Idle with no tokens. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuResetAdmissionStopsRunningGenerationTest,
	"SuperSLM.U1.Gpu.ResetAdmissionStopsRunningGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuResetAdmissionStopsRunningGenerationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpOneGenGpu(*this, W.GetTestWorld(), Gpu))
	{
		return false;
	}
	if (!TestEqual(TEXT("precondition: K is the model's depth (24)"), Gpu->GetConfiguredK(), AExNumHiddenLayers))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!OneGenVend(*this, *Gpu, Seq, TEXT("the sequence under test")))
	{
		return false;
	}
	const int32 MaxNew = OneGenP().MaxNewTokens;
	if (!OneGenStart(*this, *Gpu, Seq, OneGenP(), TEXT("P"), [Gpu, &Seq, MaxNew]() { return OneGenDecodingMidRun(*Gpu, Seq, MaxNew); }))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	const int32 PendingBefore = FSuperSLMGpuTestAccess::GetPendingEventCount(*Gpu, Seq);
	if (!TestTrue(*FString::Printf(TEXT("precondition: the running generation has pending events at the reset (%d)"), PendingBefore), PendingBefore > 0))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	const int32 PendingAfter = FSuperSLMGpuTestAccess::GetPendingEventCount(*Gpu, Seq);
	const int32 TokensAtAdmission = Gpu->GetGeneratedTokens(Seq).Num();
	AddInfo(FString::Printf(TEXT("pending events %d before the reset, %d after; %d tokens at the admission"), PendingBefore, PendingAfter, TokensAtAdmission));
	if (!TestTrue(TEXT("precondition: the reset is accepted"), HReset.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	bool bOk = TestEqual(TEXT("the reset's admission drops the running generation's pending events"), PendingAfter, 0);

	int32 TicksTokensMoved = 0;
	int32 TicksNotDecoding = 0;
	int32 TicksObserved = 0;
	const bool bResolved = OneGenTickUntil(*Gpu, [&]()
	{
		if (Gpu->GetLifecycleOpResult(HReset) != ESuperSLMRestoreResult::Pending)
		{
			return true;
		}
		++TicksObserved;
		TicksTokensMoved += Gpu->GetGeneratedTokens(Seq).Num() != TokensAtAdmission ? 1 : 0;
		TicksNotDecoding += Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Decoding ? 1 : 0;
		return false;
	});
	AddInfo(FString::Printf(TEXT("%d ticks observed before the reset resolved"), TicksObserved));
	bOk &= TestEqual(TEXT("no token of the interrupted generation is applied after the reset's admission (ticks observed otherwise)"), TicksTokensMoved, 0);
	bOk &= TestEqual(TEXT("the phase reads Decoding until the reset resolves (ticks observed otherwise)"), TicksNotDecoding, 0);
	bOk &= TestTrue(TEXT("the reset resolves"), bResolved);
	bOk &= TestEqual(TEXT("the reset resolves Success"), (uint8)Gpu->GetLifecycleOpResult(HReset), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the successful reset leaves the sequence Idle"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Idle);
	bOk &= TestEqual(TEXT("the successful reset leaves no tokens"), Gpu->GetGeneratedTokens(Seq).Num(), 0);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
