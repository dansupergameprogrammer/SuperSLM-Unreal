// A CPU sequence runs one generation at a time. BeginGeneration() returns false at the call, with
// the reason in OutError, unless the sequence will be Idle when the request's turn comes: Idle now
// with nothing queued, or a reset queued or in flight after its last generation. The turn is
// projected from the sequence's own queue, newest entry first: a queued generation answers "not
// Idle"; a reset answers Idle; a restore answers the phase its blob gives (queued or in flight);
// an adopt or a save is skipped. A refused request queues nothing and changes nothing. The refusal
// names "one generation at a time", the phase that fails it (and "restore" when the phase comes
// from a restore) or "already queued", and ResetSequence() as the remedy, and never reads as the
// queue bound.
//
// A reset Layer 1 refuses faults the sequence and keeps the previous generation's tokens; a
// generation queued behind it is dropped at its turn with a named warning, and the queue drains. A
// reset that succeeds leaves the sequence Idle with no tokens.
//
// Every cell that runs two generations on one sequence uses two requests, P ({1, 2, 3}, 24 new
// tokens) and Q ({4, 5, 6}, 8 new tokens), and first asserts that their fresh references differ.
// Every reference is the same request run on a freshly vended sequence (with the same prefix
// adopted, where the cell adopts one). A cell whose call-time assertion fails returns there.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMBlueprintLibrary.h"
#include "SuperSLMBlueprintTypes.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	constexpr float kCpuOneGenStep = 1.0f / 60.0f;
	constexpr double kCpuOneGenDeadlineSeconds = 60.0;
	// Prefill chunks of at most this many prompt tokens, so a long prompt stays Prefilling for
	// several jobs (the last chunk's post flips the phase to Decoding).
	constexpr int32 kCpuOneGenPrefillChunk = 8;
	constexpr int32 kCpuOneGenLongPromptTokens = 64;

	FSuperSLMGenerationRequest CpuOneGenRequest(const TArray<int32>& Prompt, int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		return Request;
	}

	FSuperSLMGenerationRequest CpuOneGenP() { return CpuOneGenRequest({1, 2, 3}, 24); }
	FSuperSLMGenerationRequest CpuOneGenQ() { return CpuOneGenRequest({4, 5, 6}, 8); }

	FSuperSLMGenerationRequest CpuOneGenLongPrompt()
	{
		TArray<int32> Prompt;
		for (int32 I = 0; I < kCpuOneGenLongPromptTokens; ++I)
		{
			Prompt.Add(100 + I);
		}
		return CpuOneGenRequest(Prompt, 8);
	}

	// The tokens every prefix in this file holds.
	TArray<int32> CpuOneGenPrefixTokens()
	{
		TArray<int32> Tokens;
		for (int32 I = 0; I < 12; ++I)
		{
			Tokens.Add(200 + I);
		}
		return Tokens;
	}

	const TCHAR* CpuOneGenPhaseName(ESuperSLMSequencePhase Phase)
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
	bool CpuOneGenReadsAsBound(const FString& Error)
	{
		return Error.Contains(TEXT("configured bound")) || Error.Contains(TEXT("already full")) || Error.Contains(TEXT("SSLM_SEQUENCE_QUEUE_FULL"));
	}

	bool CpuOneGenSetUp(FAutomationTestBase& T, UWorld* World, USuperSLMSubsystem*& OutCpu, USuperSLMModel*& OutModel, int32 MaxQueuedOperations = 4)
	{
		FString AExPath, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return false;
		}
		OutCpu = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), OutCpu))
		{
			return false;
		}
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = kCpuOneGenPrefillChunk;
		Config.MaxLayerBudget = 24; // A-EX is Qwen2.5-0.5B: 24 hidden layers
		Config.BlockCount = 4;
		Config.PrefixBlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		Config.MaxQueuedOperationsPerSequence = MaxQueuedOperations;
		return T.TestEqual(TEXT("CPU Configure()"), (uint8)OutCpu->Configure(OutModel, Config).Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// One tick, then a short sleep so the worker makes progress: the pacing every tick-by-tick loop
	// in this file uses.
	void CpuOneGenTick(USuperSLMSubsystem& Cpu)
	{
		Cpu.Tick(kCpuOneGenStep);
		FPlatformProcess::Sleep(0.002f);
	}

	// Ticks (FastAsPossible) until Done() or the deadline; returns Done().
	bool CpuOneGenTickUntil(USuperSLMSubsystem& Cpu, TFunctionRef<bool()> Done, double DeadlineSeconds = kCpuOneGenDeadlineSeconds)
	{
		DrainTicks(Cpu, kCpuOneGenStep, EL2S1DrainPacing::FastAsPossible, Done, DeadlineSeconds);
		return Done();
	}

	// The CPU fixture's done test, without a BeginGeneration(): terminal AND nothing pending.
	bool CpuOneGenDriveToEnd(USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		return CpuOneGenTickUntil(Cpu, [&Cpu, &Seq]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Cpu.GetPendingLifecycleOperationCount(Seq) == 0;
		});
	}

	ESuperSLMRestoreResult CpuOneGenDriveHandle(USuperSLMSubsystem& Cpu, const FSuperSLMLifecycleOpHandle& Handle)
	{
		CpuOneGenTickUntil(Cpu, [&Cpu, &Handle]() { return Cpu.GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; });
		return Cpu.GetLifecycleOpResult(Handle);
	}

	// Vends, ticking while every slot is still settling a return.
	bool CpuOneGenVend(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, FSuperSLMSequence& OutSeq, const TCHAR* What)
	{
		ESuperSLMVendResult Result = Cpu.VendSequence(OutSeq);
		const double Start = FPlatformTime::Seconds();
		while (Result == ESuperSLMVendResult::PoolExhausted && FPlatformTime::Seconds() - Start < kCpuOneGenDeadlineSeconds)
		{
			CpuOneGenTick(Cpu);
			Result = Cpu.VendSequence(OutSeq);
		}
		return T.TestEqual(*FString::Printf(TEXT("VendSequence() (%s)"), What), (uint8)Result, (uint8)ESuperSLMVendResult::Success);
	}

	// Queues a restore of Blob, ticking while every slot is still settling a return. Nothing is
	// ticked once the restore is queued.
	bool CpuOneGenRestore(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel* Model, const TArray<uint8>& Blob,
		FSuperSLMSequence& OutSeq, FSuperSLMLifecycleOpHandle& OutHandle, const TCHAR* What)
	{
		FString Error;
		ESuperSLMRestoreResult Result = Cpu.RestoreSequence(Blob, Model, OutSeq, OutHandle, Error);
		const double Start = FPlatformTime::Seconds();
		while (Result == ESuperSLMRestoreResult::PoolExhausted && FPlatformTime::Seconds() - Start < kCpuOneGenDeadlineSeconds)
		{
			CpuOneGenTick(Cpu);
			Result = Cpu.RestoreSequence(Blob, Model, OutSeq, OutHandle, Error);
		}
		return T.TestEqual(*FString::Printf(TEXT("RestoreSequence() is queued (%s): %s"), What, *Error), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
	}

	bool CpuOneGenRunToEnd(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMGenerationRequest& Request,
		TArray<int32>& OutTokens, const TCHAR* What)
	{
		FString Error;
		const bool bDone = RunGenerationToCompletion(Cpu, Seq, Request, OutTokens, kCpuOneGenDeadlineSeconds, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s completes (%s)"), What, *Error), bDone && Cpu.GetPhase(Seq) == ESuperSLMSequencePhase::Complete);
	}

	// Request on a fresh sequence, with Prefix adopted first when given; the sequence is returned.
	bool CpuOneGenReference(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMGenerationRequest& Request, TArray<int32>& OutTokens,
		const TCHAR* What, const FSuperSLMPrefix* Prefix = nullptr)
	{
		FSuperSLMSequence Ref;
		if (!CpuOneGenVend(T, Cpu, Ref, What))
		{
			return false;
		}
		if (Prefix != nullptr)
		{
			FSuperSLMLifecycleOpHandle H;
			FString Error;
			if (!T.TestEqual(*FString::Printf(TEXT("%s: AdoptPrefix() is queued (%s)"), What, *Error), (uint8)Cpu.AdoptPrefix(Ref, *Prefix, H, Error), (uint8)ESuperSLMRestoreResult::Success) ||
				!T.TestEqual(*FString::Printf(TEXT("%s: the adopt succeeds"), What), (uint8)CpuOneGenDriveHandle(Cpu, H), (uint8)ESuperSLMRestoreResult::Success))
			{
				Cpu.ReturnSequence(Ref);
				return false;
			}
		}
		const bool bDone = CpuOneGenRunToEnd(T, Cpu, Ref, Request, OutTokens, What);
		Cpu.ReturnSequence(Ref);
		return bDone;
	}

	bool CpuOneGenReferencesPQ(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, TArray<int32>& OutP, TArray<int32>& OutQ)
	{
		return CpuOneGenReference(T, Cpu, CpuOneGenP(), OutP, TEXT("P's reference")) &&
			CpuOneGenReference(T, Cpu, CpuOneGenQ(), OutQ, TEXT("Q's reference")) &&
			T.TestNotEqual(TEXT("precondition: P's and Q's fresh references differ"), OutP, OutQ);
	}

	// Begins Request on Seq and ticks until Ready() holds.
	bool CpuOneGenStart(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMGenerationRequest& Request,
		const TCHAR* What, TFunctionRef<bool()> Ready)
	{
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("%s is accepted (%s)"), What, *Error), Cpu.BeginGeneration(Seq, Request, Error)))
		{
			return false;
		}
		return T.TestTrue(*FString::Printf(TEXT("%s reaches the phase the cell requests at"), What), CpuOneGenTickUntil(Cpu, Ready));
	}

	// Decoding with at least MinTokens applied and fewer than MaxNewTokens.
	bool CpuOneGenDecodingMidRun(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, int32 MaxNewTokens, int32 MinTokens = 1)
	{
		const int32 N = Cpu.GetGeneratedTokens(Seq).Num();
		return Cpu.GetPhase(Seq) == ESuperSLMSequencePhase::Decoding && N >= MinTokens && N < MaxNewTokens;
	}

	// Queues a save and takes its blob.
	bool CpuOneGenSave(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob, const TCHAR* What)
	{
		FSuperSLMLifecycleOpHandle H;
		FString Error;
		if (!T.TestEqual(*FString::Printf(TEXT("%s: SaveSequence() is queued (%s)"), What, *Error), (uint8)Cpu.SaveSequence(Seq, H, Error), (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		// The first read that returns Success moves the blob out; every later read of the handle is
		// Consumed with no blob. The predicate is called again after the drain, so it reads the
		// handle only while the save is Pending.
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		CpuOneGenTickUntil(Cpu, [&Cpu, &H, &OutBlob, &Result]()
		{
			if (Result == ESuperSLMRestoreResult::Pending)
			{
				Result = Cpu.GetSaveResult(H, OutBlob);
			}
			return Result != ESuperSLMRestoreResult::Pending;
		});
		return T.TestEqual(*FString::Printf(TEXT("%s: the save succeeds"), What), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) &&
			T.TestTrue(*FString::Printf(TEXT("%s: the save gives a blob"), What), OutBlob.Num() > 0);
	}

	bool CpuOneGenCreatePrefix(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, FSuperSLMPrefix& OutPrefix)
	{
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("CreatePrefix() (%s)"), *Error), Cpu.CreatePrefix(CpuOneGenPrefixTokens(), OutPrefix, Error)))
		{
			return false;
		}
		return T.TestTrue(TEXT("the prefix becomes Ready"), CpuOneGenTickUntil(Cpu, [&Cpu, &OutPrefix]() { return Cpu.IsPrefixReady(OutPrefix); }));
	}

	// A blob saved mid-generation (Decoding, P) and the tokens applied at the save: the saved tokens
	// plus the restored continuation equal P's reference.
	bool CpuOneGenSaveMidGeneration(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, TArray<uint8>& OutBlob, TArray<int32>& OutTokensAtSave)
	{
		FSuperSLMSequence Live;
		if (!CpuOneGenVend(T, Cpu, Live, TEXT("the sequence saved mid-generation")))
		{
			return false;
		}
		const int32 MaxNew = CpuOneGenP().MaxNewTokens;
		if (!CpuOneGenStart(T, Cpu, Live, CpuOneGenP(), TEXT("P, to be saved mid-generation"),
				[&Cpu, &Live, MaxNew]() { return CpuOneGenDecodingMidRun(Cpu, Live, MaxNew, 4); }))
		{
			Cpu.ReturnSequence(Live);
			return false;
		}
		const bool bSaved = CpuOneGenSave(T, Cpu, Live, OutBlob, TEXT("the mid-generation save"));
		OutTokensAtSave = Cpu.GetGeneratedTokens(Live);
		const ESuperSLMSequencePhase PhaseAtSave = Cpu.GetPhase(Live);
		Cpu.ReturnSequence(Live);
		return bSaved && T.TestTrue(*FString::Printf(TEXT("precondition: the save was taken mid-generation (phase %s, %d tokens)"), CpuOneGenPhaseName(PhaseAtSave), OutTokensAtSave.Num()),
			PhaseAtSave == ESuperSLMSequencePhase::Decoding && OutTokensAtSave.Num() >= 1 && OutTokensAtSave.Num() < MaxNew);
	}

	// A blob of an Idle sequence that holds Prefix, adopted.
	bool CpuOneGenSaveIdleAdopted(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Prefix, TArray<uint8>& OutBlob)
	{
		FSuperSLMSequence S;
		if (!CpuOneGenVend(T, Cpu, S, TEXT("the Idle sequence that adopts the prefix")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle H;
		FString Error;
		const bool bOk = T.TestEqual(*FString::Printf(TEXT("AdoptPrefix() is queued (%s)"), *Error), (uint8)Cpu.AdoptPrefix(S, Prefix, H, Error), (uint8)ESuperSLMRestoreResult::Success) &&
			T.TestEqual(TEXT("the adopt succeeds"), (uint8)CpuOneGenDriveHandle(Cpu, H), (uint8)ESuperSLMRestoreResult::Success) &&
			T.TestEqual(TEXT("precondition: the adopted sequence is Idle"), (uint8)Cpu.GetPhase(S), (uint8)ESuperSLMSequencePhase::Idle) &&
			CpuOneGenSave(T, Cpu, S, OutBlob, TEXT("the Idle adopted sequence's save"));
		Cpu.ReturnSequence(S);
		return bOk;
	}
}

// --- C1: BeginGeneration on an ended sequence returns false at the call. (a) Complete, through the
// subsystem and through the Blueprint node, with the same text; after a reset, Q completes as it
// does on a fresh sequence. (b) Faulted (an empty prompt with no adopted prefix, faulted at
// activation); after a reset, Q completes. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationRefusedAtCallOnEndedSequenceTest,
	"SuperSLM.U1.Cpu.GenerationRefusedAtCallOnEndedSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationRefusedAtCallOnEndedSequenceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	bool bOk = true;

	// (a) Complete.
	{
		FSuperSLMSequence Seq;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (a)")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: P")))
		{
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("(a) begin on the Complete sequence: %d, error '%s'"), bAccepted ? 1 : 0, *Error));
		if (!TestFalse(TEXT("BeginGeneration on a Complete sequence returns false at the call"), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("(a) the refusal names the rule, Complete, ResetSequence and that continuing a completed generation is not supported (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Complete")) && Error.Contains(TEXT("ResetSequence")) &&
			Error.Contains(TEXT("a generation that continues a completed one is not supported")));
		bOk &= TestFalse(*FString::Printf(TEXT("(a) the refusal does not read as the queue bound (got: '%s')"), *Error), CpuOneGenReadsAsBound(Error));
		bOk &= TestEqual(TEXT("(a) the refused request leaves the phase Complete"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete);
		bOk &= TestEqual(TEXT("(a) the refused request leaves P's tokens"), Cpu->GetGeneratedTokens(Seq), PTokens);
		bOk &= TestEqual(TEXT("(a) the refused request queues nothing (pending count)"), Cpu->GetPendingLifecycleOperationCount(Seq), 0);

		// The same through the Blueprint node, which passes the result and the text through.
		FSuperSLMGenerationRequestBP BPRequest;
		BPRequest.PromptTokens = CpuOneGenQ().PromptTokens;
		BPRequest.MaxNewTokens = CpuOneGenQ().MaxNewTokens;
		FString BPError;
		const bool bBPAccepted = USuperSLMBlueprintLibrary::BeginGeneration(Cpu, FSuperSLMSequenceBP(Seq), BPRequest, BPError);
		bOk &= TestFalse(TEXT("(a) the Blueprint node BeginGeneration on a Complete sequence returns false"), bBPAccepted);
		bOk &= TestEqual(TEXT("(a) the Blueprint node's error is the subsystem's text"), BPError, Error);
		bOk &= TestEqual(TEXT("(a) the Blueprint node's refusal queues nothing (pending count)"), Cpu->GetPendingLifecycleOperationCount(Seq), 0);

		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError;
		bOk &= TestEqual(TEXT("(a) a reset is queued"), (uint8)Cpu->ResetSequence(Seq, HReset, ResetError), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("(a) the reset, driven to resolution, succeeds"), (uint8)CpuOneGenDriveHandle(*Cpu, HReset), (uint8)ESuperSLMRestoreResult::Success);
		TArray<int32> QTokens;
		bOk &= CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenQ(), QTokens, TEXT("(a) Q after the reset"));
		bOk &= TestEqual(TEXT("(a) Q's tokens after the reset equal Q's fresh reference"), QTokens, RefQ);
		Cpu->ReturnSequence(Seq);
	}

	// (b) Faulted: an empty prompt on a fresh sequence with no adopted prefix faults at activation,
	// with no Layer-1 call.
	{
		FSuperSLMSequence Seq;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (b)")))
		{
			return false;
		}
		FString EmptyError;
		const bool bEmptyAccepted = Cpu->BeginGeneration(Seq, CpuOneGenRequest({}, 4), EmptyError);
		if (!TestTrue(*FString::Printf(TEXT("precondition: (b) the empty-prompt request is accepted at the call (%s)"), *EmptyError), bEmptyAccepted) ||
			!TestTrue(TEXT("precondition: (b) the empty prompt faults the sequence at activation"),
				CpuOneGenTickUntil(*Cpu, [Cpu, &Seq]() { return Cpu->GetPhase(Seq) == ESuperSLMSequencePhase::Faulted && Cpu->GetPendingLifecycleOperationCount(Seq) == 0; })))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("(b) begin on the Faulted sequence: %d, error '%s'"), bAccepted ? 1 : 0, *Error));
		if (!TestFalse(TEXT("BeginGeneration on a Faulted sequence returns false at the call"), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("(b) the refusal names the rule, Faulted, ResetSequence and the restore (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Faulted")) && Error.Contains(TEXT("ResetSequence")) && Error.Contains(TEXT("restore")));
		bOk &= TestEqual(TEXT("(b) the refused request leaves the phase Faulted"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= TestEqual(TEXT("(b) the refused request queues nothing (pending count)"), Cpu->GetPendingLifecycleOperationCount(Seq), 0);

		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError;
		bOk &= TestEqual(TEXT("(b) a reset is queued"), (uint8)Cpu->ResetSequence(Seq, HReset, ResetError), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("(b) the reset, driven to resolution, succeeds"), (uint8)CpuOneGenDriveHandle(*Cpu, HReset), (uint8)ESuperSLMRestoreResult::Success);
		TArray<int32> QTokens;
		bOk &= CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenQ(), QTokens, TEXT("(b) Q after the reset"));
		bOk &= TestEqual(TEXT("(b) Q's tokens after the reset equal Q's fresh reference"), QTokens, RefQ);
		Cpu->ReturnSequence(Seq);
	}
	return bOk;
}

// --- C2: BeginGeneration while a generation runs returns false at the call, while Prefilling (a
// long prompt) and while Decoding; the running generation then completes equal to its reference. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationRefusedAtCallWhileRunningTest,
	"SuperSLM.U1.Cpu.GenerationRefusedAtCallWhileRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationRefusedAtCallWhileRunningTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model))
	{
		return false;
	}
	struct FArm
	{
		const TCHAR* Name;
		FSuperSLMGenerationRequest Request;
		ESuperSLMSequencePhase PhaseAtRequest;
	};
	const FArm Arms[] = {
		{ TEXT("(a) Prefilling"), CpuOneGenLongPrompt(), ESuperSLMSequencePhase::Prefilling },
		{ TEXT("(b) Decoding"), CpuOneGenP(), ESuperSLMSequencePhase::Decoding },
	};
	bool bOk = true;
	for (const FArm& Arm : Arms)
	{
		TArray<int32> Reference;
		if (!CpuOneGenReference(*this, *Cpu, Arm.Request, Reference, *FString::Printf(TEXT("arm %s's reference"), Arm.Name)))
		{
			return false;
		}
		FSuperSLMSequence Seq;
		if (!CpuOneGenVend(*this, *Cpu, Seq, Arm.Name))
		{
			return false;
		}
		const ESuperSLMSequencePhase Want = Arm.PhaseAtRequest;
		const int32 MaxNew = Arm.Request.MaxNewTokens;
		if (!CpuOneGenStart(*this, *Cpu, Seq, Arm.Request, *FString::Printf(TEXT("arm %s: the running generation"), Arm.Name),
				[Cpu, &Seq, Want, MaxNew]() { return Want == ESuperSLMSequencePhase::Decoding ? CpuOneGenDecodingMidRun(*Cpu, Seq, MaxNew) : Cpu->GetPhase(Seq) == Want; }))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		const ESuperSLMSequencePhase PhaseAtRequest = Cpu->GetPhase(Seq);
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("arm %s: phase %s at the request, accepted %d, error '%s'"), Arm.Name, CpuOneGenPhaseName(PhaseAtRequest), bAccepted ? 1 : 0, *Error));
		if (!TestEqual(*FString::Printf(TEXT("precondition: arm %s: the phase at the request"), Arm.Name), (uint8)PhaseAtRequest, (uint8)Want))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(*FString::Printf(TEXT("BeginGeneration while a generation runs returns false at the call, arm %s"), Arm.Name), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm %s: the refusal names the rule, the phase %s and ResetSequence (got: '%s')"), Arm.Name, CpuOneGenPhaseName(Want), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(CpuOneGenPhaseName(Want)) && Error.Contains(TEXT("ResetSequence")));
		bOk &= TestFalse(*FString::Printf(TEXT("arm %s: the refusal does not read as the queue bound (got: '%s')"), Arm.Name, *Error), CpuOneGenReadsAsBound(Error));
		bOk &= TestTrue(*FString::Printf(TEXT("arm %s: the running generation completes"), Arm.Name), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(*FString::Printf(TEXT("arm %s: the running generation's tokens equal its fresh reference"), Arm.Name), Cpu->GetGeneratedTokens(Seq), Reference);
		Cpu->ReturnSequence(Seq);
	}
	return bOk;
}

// --- C3: BeginGeneration behind a queued generation returns false at the call, "already queued":
// (a) a fresh sequence, behind a save and generation A; (b) a Complete sequence, behind a reset and
// generation A; (c) at a full queue (bound 1, a running generation fills it), the one-generation
// refusal comes before the bound's. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationRefusedAtCallBehindQueuedGenerationTest,
	"SuperSLM.U1.Cpu.GenerationRefusedAtCallBehindQueuedGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationRefusedAtCallBehindQueuedGenerationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	bool bOk = true;

	// (a) A fresh sequence: a save, then A (accepted), then B.
	{
		FSuperSLMSequence Seq;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (a)")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle HSave;
		FString SaveError, AError, BError;
		const ESuperSLMRestoreResult SaveQueued = Cpu->SaveSequence(Seq, HSave, SaveError);
		const bool bA = Cpu->BeginGeneration(Seq, CpuOneGenP(), AError);
		const bool bB = Cpu->BeginGeneration(Seq, CpuOneGenQ(), BError);
		AddInfo(FString::Printf(TEXT("arm (a): save %d, A %d, B %d, error '%s'"), (int32)SaveQueued, bA ? 1 : 0, bB ? 1 : 0, *BError));
		if (!TestEqual(TEXT("precondition: arm (a): the save is queued"), (uint8)SaveQueued, (uint8)ESuperSLMRestoreResult::Success) ||
			!TestTrue(*FString::Printf(TEXT("precondition: arm (a): generation A is accepted (%s)"), *AError), bA))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(TEXT("BeginGeneration behind a queued generation returns false at the call, arm (a)"), bB))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (a): the refusal names the rule, 'already queued' and ResetSequence (got: '%s')"), *BError),
			BError.Contains(TEXT("one generation at a time")) && BError.Contains(TEXT("already queued")) && BError.Contains(TEXT("ResetSequence")));
		bOk &= TestFalse(*FString::Printf(TEXT("arm (a): the refusal does not read as the queue bound (got: '%s')"), *BError), CpuOneGenReadsAsBound(BError));
		bOk &= TestTrue(TEXT("arm (a): generation A completes"), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(TEXT("arm (a): generation A's tokens equal P's fresh reference"), Cpu->GetGeneratedTokens(Seq), RefP);
		TArray<uint8> Unused;
		Cpu->GetSaveResult(HSave, Unused);
		Cpu->ReturnSequence(Seq);
	}

	// (b) A Complete sequence: a reset, A and B in one frame.
	{
		FSuperSLMSequence Seq;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (b)")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: arm (b): P")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError, AError, BError;
		const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
		const bool bA = Cpu->BeginGeneration(Seq, CpuOneGenQ(), AError);
		const bool bB = Cpu->BeginGeneration(Seq, CpuOneGenQ(), BError);
		AddInfo(FString::Printf(TEXT("arm (b): reset %d, A %d, B %d, error '%s'"), (int32)ResetQueued, bA ? 1 : 0, bB ? 1 : 0, *BError));
		if (!TestEqual(TEXT("precondition: arm (b): the reset is queued"), (uint8)ResetQueued, (uint8)ESuperSLMRestoreResult::Success) ||
			!TestTrue(*FString::Printf(TEXT("precondition: arm (b): generation A behind the reset is accepted (%s)"), *AError), bA))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestFalse(TEXT("BeginGeneration behind a queued generation returns false at the call, arm (b)"), bB))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (b): the refusal names the rule and 'already queued' (got: '%s')"), *BError),
			BError.Contains(TEXT("one generation at a time")) && BError.Contains(TEXT("already queued")));
		bOk &= TestTrue(TEXT("arm (b): generation A completes"), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(TEXT("arm (b): generation A's tokens equal Q's fresh reference"), Cpu->GetGeneratedTokens(Seq), RefQ);
		Cpu->ReturnSequence(Seq);
	}

	// (c) Bound 1: a running generation fills the queue (the pending count counts it); a
	// generation requested then is refused by the one-generation rule, not the bound.
	FTestWorldWrapper BoundW;
	if (!BoundW.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* BoundCpu = nullptr;
	USuperSLMModel* BoundModel = nullptr;
	if (!CpuOneGenSetUp(*this, BoundW.GetTestWorld(), BoundCpu, BoundModel, /*MaxQueued*/ 1))
	{
		return false;
	}
	FSuperSLMSequence Seq;
	if (!CpuOneGenVend(*this, *BoundCpu, Seq, TEXT("arm (c)")))
	{
		return false;
	}
	const int32 MaxNew = CpuOneGenP().MaxNewTokens;
	if (!CpuOneGenStart(*this, *BoundCpu, Seq, CpuOneGenP(), TEXT("arm (c): the running generation"),
			[BoundCpu, &Seq, MaxNew]() { return CpuOneGenDecodingMidRun(*BoundCpu, Seq, MaxNew); }))
	{
		BoundCpu->ReturnSequence(Seq);
		return false;
	}
	const int32 Pending = BoundCpu->GetPendingLifecycleOperationCount(Seq);
	FString Error;
	const bool bAccepted = BoundCpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
	AddInfo(FString::Printf(TEXT("arm (c): pending %d, accepted %d, error '%s'"), Pending, bAccepted ? 1 : 0, *Error));
	if (!TestEqual(TEXT("precondition: arm (c): the running generation fills the queue at bound 1"), Pending, 1))
	{
		BoundCpu->ReturnSequence(Seq);
		return false;
	}
	bOk &= TestFalse(TEXT("arm (c): BeginGeneration at a full queue returns false at the call"), bAccepted);
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal at a full queue names the one-generation rule, not the bound (got: '%s')"), *Error),
		Error.Contains(TEXT("one generation at a time")) && !CpuOneGenReadsAsBound(Error));
	BoundCpu->ReturnSequence(Seq);
	return bOk;
}

// --- C4: BeginGeneration behind a queued reset returns true. (a) On a Complete sequence, a reset
// and Q in one frame; Q completes as on a fresh sequence. (b) The same while P runs: the reset
// interrupts P, which is Decoding with fewer than its MaxNewTokens tokens when the reset is
// delivered and never reads Complete before; Q completes. (c) The CPU fixture behind an undriven
// reset returns the new generation's tokens. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationAcceptedBehindQueuedResetTest,
	"SuperSLM.U1.Cpu.GenerationAcceptedBehindQueuedReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationAcceptedBehindQueuedResetTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	bool bOk = true;
	const int32 MaxNew = CpuOneGenP().MaxNewTokens;

	// (a) On a Complete sequence.
	{
		FSuperSLMSequence Seq;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (a)")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: arm (a): P")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError, Error;
		const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		if (!TestEqual(TEXT("precondition: arm (a): the reset is queued"), (uint8)ResetQueued, (uint8)ESuperSLMRestoreResult::Success))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration behind a queued reset returns true, arm (a) (%s)"), *Error), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(TEXT("arm (a): Q completes"), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(TEXT("arm (a): Q's tokens equal Q's fresh reference"), Cpu->GetGeneratedTokens(Seq), RefQ);
		Cpu->ReturnSequence(Seq);
	}

	// (b) While P runs: the reset interrupts it.
	{
		FSuperSLMSequence Seq;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (b)")) ||
			!CpuOneGenStart(*this, *Cpu, Seq, CpuOneGenP(), TEXT("arm (b): P"), [Cpu, &Seq, MaxNew]() { return CpuOneGenDecodingMidRun(*Cpu, Seq, MaxNew); }))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError, Error;
		const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		if (!TestEqual(TEXT("precondition: arm (b): the reset is queued"), (uint8)ResetQueued, (uint8)ESuperSLMRestoreResult::Success))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration behind a queued reset returns true, arm (b) (%s)"), *Error), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		// Until the reset's handle resolves: P never reads Complete, and holds fewer than its
		// MaxNewTokens tokens. The last observation before it resolves is P as the reset found it.
		ESuperSLMSequencePhase LastPhase = Cpu->GetPhase(Seq);
		int32 LastTokens = Cpu->GetGeneratedTokens(Seq).Num();
		int32 TicksComplete = 0;
		int32 TicksFinished = 0;
		const double Start = FPlatformTime::Seconds();
		while (Cpu->GetLifecycleOpResult(HReset) == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - Start < kCpuOneGenDeadlineSeconds)
		{
			LastPhase = Cpu->GetPhase(Seq);
			LastTokens = Cpu->GetGeneratedTokens(Seq).Num();
			TicksComplete += LastPhase == ESuperSLMSequencePhase::Complete ? 1 : 0;
			TicksFinished += LastTokens >= MaxNew ? 1 : 0;
			CpuOneGenTick(*Cpu);
		}
		AddInfo(FString::Printf(TEXT("arm (b): before the reset resolved, P was %s with %d tokens"), CpuOneGenPhaseName(LastPhase), LastTokens));
		bOk &= TestEqual(TEXT("arm (b): the reset resolves Success"), (uint8)Cpu->GetLifecycleOpResult(HReset), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(*FString::Printf(TEXT("precondition: arm (b): when the reset is delivered, P is Decoding with fewer than its %d tokens (%s, %d)"),
			MaxNew, CpuOneGenPhaseName(LastPhase), LastTokens), LastPhase == ESuperSLMSequencePhase::Decoding && LastTokens < MaxNew);
		bOk &= TestEqual(TEXT("arm (b): P was interrupted, not finished: it never reads Complete before the reset resolves (ticks observed Complete)"), TicksComplete, 0);
		bOk &= TestEqual(TEXT("arm (b): P was interrupted, not finished: it never holds its MaxNewTokens tokens before the reset resolves (ticks observed)"), TicksFinished, 0);
		bOk &= TestTrue(TEXT("arm (b): Q completes"), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(TEXT("arm (b): Q's tokens equal Q's fresh reference"), Cpu->GetGeneratedTokens(Seq), RefQ);
		Cpu->ReturnSequence(Seq);
	}

	// (c) The CPU fixture behind an undriven reset.
	{
		FSuperSLMSequence Seq;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("arm (c)")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: arm (c): P")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle HReset;
		FString ResetError;
		const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
		TArray<int32> Out;
		FString RunError;
		const bool bRan = ResetQueued == ESuperSLMRestoreResult::Success && RunGenerationToCompletion(*Cpu, Seq, CpuOneGenQ(), Out, kCpuOneGenDeadlineSeconds, RunError);
		AddInfo(FString::Printf(TEXT("arm (c): reset %d, fixture %d, %d tokens, error '%s'"), (int32)ResetQueued, bRan ? 1 : 0, Out.Num(), *RunError));
		bOk &= TestTrue(*FString::Printf(TEXT("arm (c): the CPU fixture behind an undriven reset completes (%s)"), *RunError), bRan);
		bOk &= TestTrue(TEXT("the CPU fixture behind an undriven reset returns the new generation's tokens"), Out == RefQ && Out != RefP);
		Cpu->ReturnSequence(Seq);
	}
	return bOk;
}

// --- C5: BeginGeneration while the holder's reset is in flight (posted, not yet delivered) returns
// true, and Q completes; once that generation has completed, with nothing queued, a begin returns
// false naming Complete (the in-flight record ends with the reset's delivery). ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationAcceptedWhileResetInFlightTest,
	"SuperSLM.U1.Cpu.GenerationAcceptedWhileResetInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationAcceptedWhileResetInFlightTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMSequence Seq;
	TArray<int32> PTokens;
	if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("the sequence under test")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: P")))
	{
		return false;
	}
	FSuperSLMLifecycleOpHandle HReset;
	FString ResetError;
	if (!TestEqual(TEXT("precondition: the reset is queued"), (uint8)Cpu->ResetSequence(Seq, HReset, ResetError), (uint8)ESuperSLMRestoreResult::Success))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	// One tick at a time until the reset job is posted: it has left the queue (pending count 0) and
	// its handle is still Pending. A job posted in a tick's Plan is delivered no earlier than the
	// next tick's Apply, so the window is always observed unless the reset resolves first.
	int32 Ticks = 0;
	while (Ticks < 120 && Cpu->GetLifecycleOpResult(HReset) == ESuperSLMRestoreResult::Pending && Cpu->GetPendingLifecycleOperationCount(Seq) != 0)
	{
		CpuOneGenTick(*Cpu);
		++Ticks;
	}
	const ESuperSLMRestoreResult ResetAtBegin = Cpu->GetLifecycleOpResult(HReset);
	const ESuperSLMSequencePhase PhaseAtBegin = Cpu->GetPhase(Seq);
	const int32 PendingAtBegin = Cpu->GetPendingLifecycleOperationCount(Seq);
	AddInfo(FString::Printf(TEXT("after %d tick(s): reset %d, phase %s, pending %d"), Ticks, (int32)ResetAtBegin, CpuOneGenPhaseName(PhaseAtBegin), PendingAtBegin));
	if (!TestEqual(TEXT("precondition: the reset is in flight: its handle reads Pending (the window was missed if it resolved)"), (uint8)ResetAtBegin, (uint8)ESuperSLMRestoreResult::Pending) ||
		!TestEqual(TEXT("precondition: the reset is in flight: the phase still reads Complete"), (uint8)PhaseAtBegin, (uint8)ESuperSLMSequencePhase::Complete) ||
		!TestEqual(TEXT("precondition: the reset is in flight: the pending count reads 0 (a still-queued reset reads 1)"), PendingAtBegin, 0))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	FString Error;
	const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration while a reset is in flight returns true (%s)"), *Error), bAccepted))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	bool bOk = TestTrue(TEXT("Q completes"), CpuOneGenDriveToEnd(*Cpu, Seq));
	bOk &= TestEqual(TEXT("Q's tokens equal Q's fresh reference"), Cpu->GetGeneratedTokens(Seq), RefQ);

	// Nothing is queued now, and the reset that was in flight has been delivered.
	FString LastError;
	const bool bLast = Cpu->BeginGeneration(Seq, CpuOneGenP(), LastError);
	AddInfo(FString::Printf(TEXT("begin after Q: %d, error '%s'"), bLast ? 1 : 0, *LastError));
	bOk &= TestFalse(TEXT("BeginGeneration after a completed generation that followed a reset returns false at the call"), bLast);
	bOk &= TestTrue(*FString::Printf(TEXT("the last refusal names the rule and Complete (got: '%s')"), *LastError),
		LastError.Contains(TEXT("one generation at a time")) && LastError.Contains(TEXT("Complete")));
	Cpu->ReturnSequence(Seq);
	return bOk;
}

// --- C6: BeginGeneration behind a restore. (a) Queued (before the restore is dispatched) and (b) in
// flight (posted, not delivered), a restore of a blob saved mid-generation carries a generation, so
// the begin returns false naming the phase and "restore"; the restored continuation completes, and
// the saved tokens plus it equal the unsaved reference. (c) A restore of an Idle sequence holding
// an adopted prefix gives Idle, so the begin returns true, and the request completes equal to a
// fresh vend that adopts the same prefix; a restore of a blob saved Complete gives Complete, so the
// begin returns false naming Complete and "restore". ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationRefusedAtCallBehindRestoreTest,
	"SuperSLM.U1.Cpu.GenerationRefusedAtCallBehindRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationRefusedAtCallBehindRestoreTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	TArray<uint8> MidBlob;
	TArray<int32> TokensAtSave;
	if (!CpuOneGenSaveMidGeneration(*this, *Cpu, MidBlob, TokensAtSave))
	{
		return false;
	}
	bool bOk = true;

	// (a) Queued: the begin comes before any tick, so the restore is still in the queue.
	{
		FSuperSLMSequence R;
		FSuperSLMLifecycleOpHandle HRestore;
		if (!CpuOneGenRestore(*this, *Cpu, Model, MidBlob, R, HRestore, TEXT("arm (a)")))
		{
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(R, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("arm (a): accepted %d, pending %d, error '%s'"), bAccepted ? 1 : 0, Cpu->GetPendingLifecycleOperationCount(R), *Error));
		if (!TestFalse(TEXT("BeginGeneration behind a restore that carries a generation returns false at the call"), bAccepted))
		{
			Cpu->ReturnSequence(R);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (a): the refusal names the rule, Decoding and the restore (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Decoding")) && Error.Contains(TEXT("restore")));
		bOk &= TestEqual(TEXT("arm (a): the restore succeeds"), (uint8)CpuOneGenDriveHandle(*Cpu, HRestore), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("arm (a): the restored continuation completes"), CpuOneGenDriveToEnd(*Cpu, R));
		TArray<int32> Continued = TokensAtSave;
		Continued.Append(Cpu->GetGeneratedTokens(R));
		bOk &= TestEqual(TEXT("arm (a): the saved tokens plus the continuation equal the unsaved reference"), Continued, RefP);
		Cpu->ReturnSequence(R);
	}

	// (b) In flight: one tick at a time until the restore job is posted and not delivered.
	{
		FSuperSLMSequence R;
		FSuperSLMLifecycleOpHandle HRestore;
		if (!CpuOneGenRestore(*this, *Cpu, Model, MidBlob, R, HRestore, TEXT("arm (b)")))
		{
			return false;
		}
		int32 Ticks = 0;
		while (Ticks < 120 && Cpu->GetLifecycleOpResult(HRestore) == ESuperSLMRestoreResult::Pending && Cpu->GetPendingLifecycleOperationCount(R) != 0)
		{
			CpuOneGenTick(*Cpu);
			++Ticks;
		}
		const ESuperSLMRestoreResult RestoreAtBegin = Cpu->GetLifecycleOpResult(HRestore);
		const int32 PendingAtBegin = Cpu->GetPendingLifecycleOperationCount(R);
		if (!TestEqual(TEXT("precondition: arm (b): the restore is in flight: its handle reads Pending (the window was missed if it resolved)"), (uint8)RestoreAtBegin, (uint8)ESuperSLMRestoreResult::Pending) ||
			!TestEqual(TEXT("precondition: arm (b): the restore is in flight: the pending count reads 0 (a still-queued restore reads 1)"), PendingAtBegin, 0))
		{
			Cpu->ReturnSequence(R);
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(R, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("arm (b): after %d tick(s): accepted %d, error '%s'"), Ticks, bAccepted ? 1 : 0, *Error));
		if (!TestFalse(TEXT("BeginGeneration while a restore that carries a generation is in flight returns false at the call"), bAccepted))
		{
			Cpu->ReturnSequence(R);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("arm (b): the refusal names the rule, Decoding and the restore (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Decoding")) && Error.Contains(TEXT("restore")));
		bOk &= TestEqual(TEXT("arm (b): the restore succeeds"), (uint8)CpuOneGenDriveHandle(*Cpu, HRestore), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("arm (b): the restored continuation completes"), CpuOneGenDriveToEnd(*Cpu, R));
		Cpu->ReturnSequence(R);
	}

	// (c) The accept side: a restore of an Idle sequence holding an adopted prefix.
	FSuperSLMPrefix Prefix;
	TArray<uint8> IdleBlob;
	TArray<int32> RefAdoptQ;
	if (!CpuOneGenCreatePrefix(*this, *Cpu, Prefix) || !CpuOneGenSaveIdleAdopted(*this, *Cpu, Prefix, IdleBlob) ||
		!CpuOneGenReference(*this, *Cpu, CpuOneGenQ(), RefAdoptQ, TEXT("Q after the prefix, on a fresh vend"), &Prefix))
	{
		return false;
	}
	{
		FSuperSLMSequence R;
		FSuperSLMLifecycleOpHandle HRestore;
		if (!CpuOneGenRestore(*this, *Cpu, Model, IdleBlob, R, HRestore, TEXT("arm (c)")))
		{
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(R, CpuOneGenQ(), Error);
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration behind a restore of an Idle sequence returns true (%s)"), *Error), bAccepted))
		{
			Cpu->ReturnSequence(R);
			return false;
		}
		bOk &= TestTrue(TEXT("arm (c): the request behind the restore completes"), CpuOneGenDriveToEnd(*Cpu, R));
		bOk &= TestEqual(TEXT("arm (c): its tokens equal a fresh vend that adopts the same prefix and runs the same request"), Cpu->GetGeneratedTokens(R), RefAdoptQ);
		Cpu->ReturnSequence(R);
	}

	// (c), second step: a restore of a blob saved Complete.
	TArray<uint8> CompleteBlob;
	{
		FSuperSLMSequence S;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, S, TEXT("the sequence saved Complete")) || !CpuOneGenRunToEnd(*this, *Cpu, S, CpuOneGenP(), PTokens, TEXT("precondition: P, to be saved Complete")))
		{
			return false;
		}
		const bool bSaved = CpuOneGenSave(*this, *Cpu, S, CompleteBlob, TEXT("the Complete save"));
		Cpu->ReturnSequence(S);
		if (!bSaved)
		{
			return false;
		}
	}
	{
		FSuperSLMSequence R;
		FSuperSLMLifecycleOpHandle HRestore;
		if (!CpuOneGenRestore(*this, *Cpu, Model, CompleteBlob, R, HRestore, TEXT("arm (c), Complete")))
		{
			return false;
		}
		FString Error;
		const bool bAccepted = Cpu->BeginGeneration(R, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("arm (c), Complete: accepted %d, error '%s'"), bAccepted ? 1 : 0, *Error));
		bOk &= TestFalse(TEXT("BeginGeneration behind a restore of a Complete sequence returns false at the call"), bAccepted);
		bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the rule, Complete and the restore (got: '%s')"), *Error),
			Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Complete")) && Error.Contains(TEXT("restore")));
		CpuOneGenDriveHandle(*Cpu, HRestore);
		Cpu->ReturnSequence(R);
	}
	return bOk;
}

// --- C7: a CPU reset Layer 1 refuses resolves Malformed and faults the sequence, naming
// sslm_seq_reset and the status, with the previous generation's tokens kept; a generation is then
// refused at the call. A reset that then succeeds leaves the sequence Idle with no tokens, and Q
// completes as on a fresh sequence. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuRefusedResetFaultsSequenceTest,
	"SuperSLM.U1.Cpu.RefusedResetFaultsSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuRefusedResetFaultsSequenceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMSequence Seq;
	TArray<int32> PTokens;
	if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("the sequence under test")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: P")))
	{
		return false;
	}

	// The delivery logs the refusal as an Error ("Sequence reset failed: sslm_seq_reset refused
	// (SSLM_INVALID_ARGUMENT).") and the fault as a Warning naming the call and the status. Both are
	// this cell's claim, so each is expected exactly once.
	AddExpectedErrorPlain(TEXT("sslm_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("faulted: sslm_seq_reset returned SSLM_INVALID_ARGUMENT; the sequence was not reset and is Faulted until a reset succeeds"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride(*Cpu, SSLM_INVALID_ARGUMENT);
	FSuperSLMLifecycleOpHandle HReset;
	FString ResetError;
	if (!TestEqual(TEXT("precondition: the reset is queued"), (uint8)Cpu->ResetSequence(Seq, HReset, ResetError), (uint8)ESuperSLMRestoreResult::Success))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	const ESuperSLMRestoreResult ResetResult = CpuOneGenDriveHandle(*Cpu, HReset);
	AddInfo(FString::Printf(TEXT("refused reset: result %d, phase %s, %d tokens"), (int32)ResetResult, CpuOneGenPhaseName(Cpu->GetPhase(Seq)), Cpu->GetGeneratedTokens(Seq).Num()));
	bool bOk = TestEqual(TEXT("a CPU reset Layer 1 refuses resolves Malformed"), (uint8)ResetResult, (uint8)ESuperSLMRestoreResult::Malformed);
	bOk &= TestEqual(TEXT("a refused reset leaves the CPU sequence Faulted"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("a refused CPU reset leaves the previous generation's tokens"), Cpu->GetGeneratedTokens(Seq), RefP);

	FString Error;
	const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
	bOk &= TestFalse(TEXT("a generation after the refused reset returns false at the call"), bAccepted);
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the rule and Faulted (got: '%s')"), *Error),
		Error.Contains(TEXT("one generation at a time")) && Error.Contains(TEXT("Faulted")));

	// The override was one-shot: the next reset reaches Layer 1 and succeeds.
	FSuperSLMLifecycleOpHandle HReset2;
	bOk &= TestEqual(TEXT("a reset with the override spent is queued"), (uint8)Cpu->ResetSequence(Seq, HReset2, ResetError), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("a reset with the override spent succeeds"), (uint8)CpuOneGenDriveHandle(*Cpu, HReset2), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestTrue(*FString::Printf(TEXT("a successful CPU reset leaves the sequence Idle with no tokens (%s, %d tokens)"),
		CpuOneGenPhaseName(Cpu->GetPhase(Seq)), Cpu->GetGeneratedTokens(Seq).Num()),
		Cpu->GetPhase(Seq) == ESuperSLMSequencePhase::Idle && Cpu->GetGeneratedTokens(Seq).Num() == 0);
	TArray<int32> QTokens;
	bOk &= CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenQ(), QTokens, TEXT("Q after the successful reset"));
	bOk &= TestEqual(TEXT("Q's tokens equal Q's fresh reference"), QTokens, RefQ);
	Cpu->ReturnSequence(Seq);
	return bOk;
}

// --- C8: a generation queued behind a reset Layer 1 refuses is dropped at its turn, with a named
// warning, and the queue drains: the sequence is Faulted with the previous generation's tokens. A
// reset then succeeds (the queue is not blocked), and Q completes. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationBehindRefusedResetDroppedAtTurnTest,
	"SuperSLM.U1.Cpu.GenerationBehindRefusedResetDroppedAtTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationBehindRefusedResetDroppedAtTurnTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ))
	{
		return false;
	}
	FSuperSLMSequence Seq;
	TArray<int32> PTokens;
	if (!CpuOneGenVend(*this, *Cpu, Seq, TEXT("the sequence under test")) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: P")))
	{
		return false;
	}

	AddExpectedErrorPlain(TEXT("sslm_seq_reset refused"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("one generation at a time: the queued generation was not started"), EAutomationExpectedErrorFlags::Contains, 1);
	FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride(*Cpu, SSLM_INVALID_ARGUMENT);
	FSuperSLMLifecycleOpHandle HReset;
	FString ResetError, Error;
	const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
	const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
	if (!TestEqual(TEXT("precondition: the reset is queued"), (uint8)ResetQueued, (uint8)ESuperSLMRestoreResult::Success) ||
		!TestTrue(*FString::Printf(TEXT("precondition: the generation behind the reset is accepted (%s)"), *Error), bAccepted))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	bool bOk = TestEqual(TEXT("the refused reset resolves Malformed"), (uint8)CpuOneGenDriveHandle(*Cpu, HReset), (uint8)ESuperSLMRestoreResult::Malformed);
	int32 Ticks = 0;
	while (Ticks < 60 && Cpu->GetPendingLifecycleOperationCount(Seq) != 0)
	{
		CpuOneGenTick(*Cpu);
		++Ticks;
	}
	AddInfo(FString::Printf(TEXT("after %d tick(s): pending %d, phase %s, %d tokens"), Ticks, Cpu->GetPendingLifecycleOperationCount(Seq),
		CpuOneGenPhaseName(Cpu->GetPhase(Seq)), Cpu->GetGeneratedTokens(Seq).Num()));
	if (!TestEqual(TEXT("the generation behind a refused reset is dropped at its turn (the queue drains)"), Cpu->GetPendingLifecycleOperationCount(Seq), 0))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	bOk &= TestEqual(TEXT("the dropped generation leaves the sequence Faulted"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the dropped generation leaves the previous generation's tokens"), Cpu->GetGeneratedTokens(Seq), RefP);

	FSuperSLMLifecycleOpHandle HReset2;
	bOk &= TestEqual(TEXT("a reset after the drop is queued"), (uint8)Cpu->ResetSequence(Seq, HReset2, ResetError), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("a reset after the drop succeeds (the queue is not blocked)"), (uint8)CpuOneGenDriveHandle(*Cpu, HReset2), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> QTokens;
	bOk &= CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenQ(), QTokens, TEXT("Q after the successful reset"));
	bOk &= TestEqual(TEXT("Q's tokens equal Q's fresh reference"), QTokens, RefQ);
	Cpu->ReturnSequence(Seq);
	return bOk;
}

// --- C9: the projection skips an adopt or a save queued between the reset and the begin. On a
// Complete sequence, in one frame: a reset, an adopt, and BeginGeneration, which returns true; it
// completes equal to a fresh vend that adopts the same prefix and runs the same request. A second
// arm puts a save in place of the adopt. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuGenerationAcceptedBehindResetAndAdoptTest,
	"SuperSLM.U1.Cpu.GenerationAcceptedBehindResetAndAdopt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuGenerationAcceptedBehindResetAndAdoptTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	TArray<int32> RefP, RefQ, RefAdoptQ;
	FSuperSLMPrefix Prefix;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model) || !CpuOneGenReferencesPQ(*this, *Cpu, RefP, RefQ) ||
		!CpuOneGenCreatePrefix(*this, *Cpu, Prefix) ||
		!CpuOneGenReference(*this, *Cpu, CpuOneGenQ(), RefAdoptQ, TEXT("Q after the prefix, on a fresh vend"), &Prefix))
	{
		return false;
	}
	bool bOk = true;
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		const bool bAdopt = Arm == 0;
		const TCHAR* Between = bAdopt ? TEXT("adopt") : TEXT("save");
		FSuperSLMSequence Seq;
		TArray<int32> PTokens;
		if (!CpuOneGenVend(*this, *Cpu, Seq, Between) || !CpuOneGenRunToEnd(*this, *Cpu, Seq, CpuOneGenP(), PTokens, TEXT("precondition: P")))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle HReset, HBetween;
		FString ResetError, BetweenError, Error;
		const ESuperSLMRestoreResult ResetQueued = Cpu->ResetSequence(Seq, HReset, ResetError);
		const ESuperSLMRestoreResult BetweenQueued = bAdopt ? Cpu->AdoptPrefix(Seq, Prefix, HBetween, BetweenError) : Cpu->SaveSequence(Seq, HBetween, BetweenError);
		const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
		AddInfo(FString::Printf(TEXT("%s arm: reset %d, %s %d, begin %d, error '%s'"), Between, (int32)ResetQueued, Between, (int32)BetweenQueued, bAccepted ? 1 : 0, *Error));
		if (!TestEqual(*FString::Printf(TEXT("precondition: %s arm: the reset is queued"), Between), (uint8)ResetQueued, (uint8)ESuperSLMRestoreResult::Success) ||
			!TestEqual(*FString::Printf(TEXT("precondition: %s arm: the %s is queued (%s)"), Between, Between, *BetweenError), (uint8)BetweenQueued, (uint8)ESuperSLMRestoreResult::Success))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration behind a queued reset and %s returns true (%s)"), Between, *Error), bAccepted))
		{
			Cpu->ReturnSequence(Seq);
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s arm: the generation completes"), Between), CpuOneGenDriveToEnd(*Cpu, Seq));
		bOk &= TestEqual(*FString::Printf(TEXT("%s arm: its tokens equal a fresh vend running the same request%s"), Between, bAdopt ? TEXT(" after the same prefix") : TEXT("")),
			Cpu->GetGeneratedTokens(Seq), bAdopt ? RefAdoptQ : RefQ);
		if (!bAdopt)
		{
			TArray<uint8> Unused;
			Cpu->GetSaveResult(HBetween, Unused);
		}
		Cpu->ReturnSequence(Seq);
	}
	return bOk;
}

// --- C10: the call's verdict does not depend on when ticks run. For each enabling call -- a reset on
// a Complete sequence, a restore of a blob saved mid-generation, and a restore of an Idle blob
// holding an adopted prefix -- the begin is issued after n paced ticks, for n = 0 .. N + 1, where N
// is the tick count at which the enabling handle first read resolved in a calibration run of the
// same arm, and once more after the enabling handle resolves, with a fresh sequence and enabling
// call per point. Every point gives the arm's verdict: accepted, refused, accepted. An accepted
// point's generation is driven to Complete; a refused point's request is not run on. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuOneGenerationVerdictIndependentOfTicksTest,
	"SuperSLM.U1.Cpu.OneGenerationVerdictIndependentOfTicks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuOneGenerationVerdictIndependentOfTicksTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!CpuOneGenSetUp(*this, W.GetTestWorld(), Cpu, Model))
	{
		return false;
	}
	TArray<uint8> MidBlob, IdleBlob;
	TArray<int32> TokensAtSave;
	FSuperSLMPrefix Prefix;
	if (!CpuOneGenSaveMidGeneration(*this, *Cpu, MidBlob, TokensAtSave) || !CpuOneGenCreatePrefix(*this, *Cpu, Prefix) ||
		!CpuOneGenSaveIdleAdopted(*this, *Cpu, Prefix, IdleBlob))
	{
		return false;
	}

	enum class EArm : uint8 { Reset, GeneratingRestore, IdleRestore };
	struct FArm
	{
		EArm Kind;
		const TCHAR* Name;
		bool bAccepted;
		const TCHAR* Verdict;
	};
	const FArm Arms[] = {
		{ EArm::Reset, TEXT("a reset on a Complete sequence"), true, TEXT("a begin behind a reset on a Complete sequence is accepted at every point") },
		{ EArm::GeneratingRestore, TEXT("a restore of a blob saved mid-generation"), false, TEXT("a begin behind a restore that carries a generation is refused at every point") },
		{ EArm::IdleRestore, TEXT("a restore of an Idle blob holding an adopted prefix"), true, TEXT("a begin behind a restore of an Idle sequence is accepted at every point") },
	};

	// The enabling call on a fresh sequence: OutSeq is the sequence the begin goes to.
	auto Enable = [this, Cpu, Model, &MidBlob, &IdleBlob](EArm Kind, FSuperSLMSequence& OutSeq, FSuperSLMLifecycleOpHandle& OutHandle) -> bool
	{
		if (Kind == EArm::Reset)
		{
			TArray<int32> PTokens;
			FString ResetError;
			return CpuOneGenVend(*this, *Cpu, OutSeq, TEXT("a reset point's sequence")) &&
				CpuOneGenRunToEnd(*this, *Cpu, OutSeq, CpuOneGenP(), PTokens, TEXT("precondition: P")) &&
				TestEqual(TEXT("precondition: the reset is queued"), (uint8)Cpu->ResetSequence(OutSeq, OutHandle, ResetError), (uint8)ESuperSLMRestoreResult::Success);
		}
		return CpuOneGenRestore(*this, *Cpu, Model, Kind == EArm::GeneratingRestore ? MidBlob : IdleBlob, OutSeq, OutHandle, TEXT("a restore point"));
	};

	for (const FArm& Arm : Arms)
	{
		// Calibration: the paced ticks until the enabling handle first reads resolved.
		int32 N = 0;
		{
			FSuperSLMSequence Seq;
			FSuperSLMLifecycleOpHandle H;
			if (!Enable(Arm.Kind, Seq, H))
			{
				return false;
			}
			while (N < 1000 && Cpu->GetLifecycleOpResult(H) == ESuperSLMRestoreResult::Pending)
			{
				CpuOneGenTick(*Cpu);
				++N;
			}
			Cpu->ReturnSequence(Seq);
			AddInfo(FString::Printf(TEXT("%s: calibration N = %d"), Arm.Name, N));
			if (!TestTrue(*FString::Printf(TEXT("precondition: %s: the calibration's N is at most 120 (%d)"), Arm.Name, N), N <= 120))
			{
				return false;
			}
		}
		// Points n = 0 .. N + 1, then one after the enabling handle has resolved.
		for (int32 Point = 0; Point <= N + 2; ++Point)
		{
			const bool bAfterResolved = Point == N + 2;
			FSuperSLMSequence Seq;
			FSuperSLMLifecycleOpHandle H;
			if (!Enable(Arm.Kind, Seq, H))
			{
				return false;
			}
			if (bAfterResolved)
			{
				CpuOneGenDriveHandle(*Cpu, H);
			}
			else
			{
				for (int32 T = 0; T < Point; ++T)
				{
					CpuOneGenTick(*Cpu);
				}
			}
			const FString PointName = bAfterResolved ? FString(TEXT("after the enabling handle resolved")) : FString::Printf(TEXT("n = %d"), Point);
			FString Error;
			const bool bAccepted = Cpu->BeginGeneration(Seq, CpuOneGenQ(), Error);
			const bool bVerdict = TestEqual(*FString::Printf(TEXT("%s (%s: %s; %s)"), Arm.Verdict, Arm.Name, *PointName, *Error), bAccepted, Arm.bAccepted);
			if (bVerdict && bAccepted)
			{
				TestTrue(*FString::Printf(TEXT("%s, %s: the accepted generation completes"), Arm.Name, *PointName), CpuOneGenDriveToEnd(*Cpu, Seq));
			}
			Cpu->ReturnSequence(Seq);
			if (!bVerdict)
			{
				return false;
			}
		}
	}
	return true;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
