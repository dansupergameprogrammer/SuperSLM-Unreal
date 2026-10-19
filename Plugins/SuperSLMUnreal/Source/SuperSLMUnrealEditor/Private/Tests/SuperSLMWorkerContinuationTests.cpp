// 1.1 worker continuation (plan §5 item 1a, ruled 2026-09-29). A generation
// whose lane has nothing else waiting gets its next job queued behind the running one, so the
// worker does not idle until the next Tick(). This cell pins what that must not change, each
// arm run with continuation off and then on:
//   1. Tokens are identical (the continuation is composed exactly as the next tick would compose
//      it), and so is Layer 1's state: the sequence's save blob, which carries Layer 1's own
//      serialized state and the plugin's mirrors, is byte-identical after the generation drains.
//   2. A generation ended by a stop token runs no Layer-1 work past its end: the continuation
//      queued behind the stopping job is skipped on the worker, and Layer 1's own decode-step
//      count equals the continuation-off run's.
//   3. A request that must precede the holder's next job (here an adapter swap) and arrives while
//      a continuation is queued cancels that continuation, so the swap lands on the same token
//      boundary as with continuation off (plan §2.5 row 20). A request the worker head has already
//      passed lands one token later, exactly as a continuation-off request made one token later;
//      each attempt is checked against the matching off run, and one must show the cancel.
//   4. In a batched decode, a member whose generation stopped is left out of the continuation
//      while its live sibling runs on and produces the tokens it produces with continuation off.
//   5. Continuation jobs keep R-S1b's ledger properties, with ChainedWaitTicks in the committed
//      tick and the hitch rule (AssertRS1bProperties).
// Paced in real time at 60 Hz with whole-token jobs, as the example runs, so each decode job
// (tens of milliseconds on the CPU) outlives the tick that queues its successor.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMSlotGates.h"
#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	struct FArmSpec
	{
		USuperSLMModel* Model = nullptr;
		int32 LayerBudget = 24;               // whole-token decode jobs, the example's CPU default
		TArray<TArray<int32>> Prompts;        // one per sequence; more than one batches the decodes
		TArray<TArray<int32>> StopTokenIds;   // per sequence; missing entries have none
		int32 MaxNewTokens = 12;
		// Sequence 0 requests this adapter after the first tick at which it has SwapAtGenerated
		// tokens or more (INDEX_NONE: no swap).
		FSuperSLMAdapterHandle SwapTo;
		int32 SwapAtGenerated = INDEX_NONE;
	};

	struct FSequenceResult
	{
		TArray<int32> Tokens;
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		FSuperSLMSequenceStats Stats;
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		TArray<uint8> SaveBlob;
		int64 ActiveAdapterId = 0;
	};

	struct FArmResult
	{
		TArray<FSequenceResult> Sequences;
		TArray<FSuperSLMWorkerJobReport> Ledger;
		int32 HitchCount = 0;
		double WallSeconds = 0.0;
		FSuperSLMRuntimeConfig Config;
		int32 Continuations = 0;
		int32 SkippedMembers = 0;
		int32 SwappedAtGenerated = INDEX_NONE; // sequence 0's token count when the swap was requested
	};

	bool RunArm(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FArmSpec& Spec, bool bContinuation, FArmResult& Out)
	{
		const int32 N = Spec.Prompts.Num();
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = N;
		Config.MaxSequencesPerDecodeCall = N;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = Spec.LayerBudget;
		Config.MaxConcurrentCalls = 1;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 16.6; // the example's budget
		Config.bWorkerContinuation = bContinuation;
		if (!T.TestEqual(TEXT("Configure()"), (uint8)Cpu.Configure(Spec.Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		TArray<FSuperSLMSequence> Seqs;
		Seqs.SetNum(N);
		for (int32 I = 0; I < N; ++I)
		{
			if (!T.TestEqual(TEXT("vend"), (uint8)Cpu.VendSequence(Seqs[I]), (uint8)ESuperSLMVendResult::Success))
			{
				return false;
			}
			Cpu.SetLayerBudget(Seqs[I], Spec.LayerBudget);
		}
		for (int32 I = 0; I < N; ++I)
		{
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Spec.Prompts[I];
			Request.MaxNewTokens = Spec.MaxNewTokens;
			if (Spec.StopTokenIds.IsValidIndex(I))
			{
				Request.StopTokenIds = Spec.StopTokenIds[I];
			}
			FString Error;
			if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration %d: %s"), I, *Error), Cpu.BeginGeneration(Seqs[I], Request, Error)))
			{
				return false;
			}
		}
		auto IsTerminal = [&Cpu](const FSuperSLMSequence& Seq)
		{
			const ESuperSLMSequencePhase P = Cpu.GetPhase(Seq);
			return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted;
		};
		const double Start = FPlatformTime::Seconds();
		DrainTicks(Cpu, 1.0f / 60.0f, EL2S1DrainPacing::RealTime, [&]()
		{
			// Called right after each Tick(), as a game would act between frames.
			if (Spec.SwapAtGenerated != INDEX_NONE && Out.SwappedAtGenerated == INDEX_NONE)
			{
				const int32 Generated = Cpu.GetGeneratedTokens(Seqs[0]).Num();
				if (Generated >= Spec.SwapAtGenerated)
				{
					Cpu.RequestAdapterSwap(Seqs[0], Spec.SwapTo);
					Out.SwappedAtGenerated = Generated;
				}
			}
			for (const FSuperSLMSequence& Seq : Seqs)
			{
				if (!IsTerminal(Seq))
				{
					return false;
				}
			}
			return true;
		}, /*MaxWallClockSeconds*/ 300.0);
		Out.WallSeconds = FPlatformTime::Seconds() - Start;
		// Let every job queued for the generation deliver (a skipped continuation included), so
		// the stats, the ledger and the saved state describe all of it.
		DrainTicks(Cpu, 1.0f / 60.0f, EL2S1DrainPacing::RealTime, [&]()
		{
			for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
			{
				if (Job.DeliveredAtTick < 0)
				{
					return false;
				}
			}
			return true;
		}, 30.0);
		Out.Ledger = Cpu.GetJobLedger();
		Out.HitchCount = Cpu.GetHitchCount();
		Out.Config = Config;
		for (const FSuperSLMWorkerJobReport& Job : Out.Ledger)
		{
			Out.Continuations += Job.bContinuation ? 1 : 0;
			Out.SkippedMembers += Job.SkippedMembers;
		}
		Out.Sequences.SetNum(N);
		TArray<FSuperSLMLifecycleOpHandle> SaveHandles;
		SaveHandles.SetNum(N);
		for (int32 I = 0; I < N; ++I)
		{
			FSequenceResult& R = Out.Sequences[I];
			R.Phase = Cpu.GetPhase(Seqs[I]);
			R.Tokens = Cpu.GetGeneratedTokens(Seqs[I]);
			R.Stats = Cpu.GetStats(Seqs[I]);
			R.ActiveAdapterId = Cpu.GetActiveAdapter(Seqs[I]).Id;
			// The state oracle: the save blob holds Layer 1's own serialized sequence state and the
			// plugin's mirrors (phase, context used, layers done, ready-for-logits, adapter).
			FString Error;
			T.TestEqual(*FString::Printf(TEXT("SaveSequence %d queues: %s"), I, *Error),
				(uint8)Cpu.SaveSequence(Seqs[I], SaveHandles[I], Error), (uint8)ESuperSLMRestoreResult::Success);
		}
		DrainTicks(Cpu, 1.0f / 60.0f, EL2S1DrainPacing::RealTime, [&]()
		{
			bool bAll = true;
			for (int32 I = 0; I < N; ++I)
			{
				FSequenceResult& R = Out.Sequences[I];
				if (R.SaveResult == ESuperSLMRestoreResult::Pending)
				{
					R.SaveResult = Cpu.GetSaveResult(SaveHandles[I], R.SaveBlob);
				}
				bAll &= R.SaveResult != ESuperSLMRestoreResult::Pending;
			}
			return bAll;
		}, 60.0);
		for (int32 I = 0; I < N; ++I)
		{
			Cpu.ReleaseLifecycleOpHandle(SaveHandles[I]);
			Cpu.ReturnSequence(Seqs[I]);
		}
		for (int32 I = 0; I < 40; ++I)
		{
			Cpu.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	// Sequence I of the continuation-on arm against the same sequence of the off arm: tokens, every
	// stats field, the adapter, and the saved state byte for byte.
	bool ExpectSameSequence(FAutomationTestBase& T, const FArmResult& Off, const FArmResult& On, int32 I, const TCHAR* What)
	{
		const FSequenceResult& A = Off.Sequences[I];
		const FSequenceResult& B = On.Sequences[I];
		bool bOk = true;
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: the tokens equal the continuation-off run's"), What, I), B.Tokens, A.Tokens);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: phase"), What, I), (uint8)B.Phase, (uint8)A.Phase);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: Layer 1's decode-step count"), What, I), B.Stats.DecodeStepActual, A.Stats.DecodeStepActual);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: decode-step ceiling"), What, I), B.Stats.DecodeStepCeiling, A.Stats.DecodeStepCeiling);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: forced tokens"), What, I), B.Stats.ForcedTokenCount, A.Stats.ForcedTokenCount);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: KV blocks resident"), What, I), B.Stats.KvBlocksResident, A.Stats.KvBlocksResident);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: schema accepting"), What, I), B.Stats.bSchemaAccepting, A.Stats.bSchemaAccepting);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: active adapter"), What, I), B.ActiveAdapterId, A.ActiveAdapterId);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: off-arm save succeeds"), What, I), (uint8)A.SaveResult, (uint8)ESuperSLMRestoreResult::Success);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s, seq %d: on-arm save succeeds"), What, I), (uint8)B.SaveResult, (uint8)ESuperSLMRestoreResult::Success);
		int32 FirstDiff = INDEX_NONE;
		for (int32 J = 0; J < FMath::Min(A.SaveBlob.Num(), B.SaveBlob.Num()) && FirstDiff == INDEX_NONE; ++J)
		{
			FirstDiff = A.SaveBlob[J] != B.SaveBlob[J] ? J : INDEX_NONE;
		}
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s, seq %d: the saved state is byte-identical (sizes %d / %d, first difference at %d)"),
			What, I, A.SaveBlob.Num(), B.SaveBlob.Num(), FirstDiff),
			A.SaveBlob.Num() > 0 && A.SaveBlob.Num() == B.SaveBlob.Num() && FirstDiff == INDEX_NONE);
		return bOk;
	}

	// The token's first occurrence at index 3 or later with at least two tokens after it, so a
	// stop there ends the generation early and leaves a continuation queued behind it.
	int32 FindStopIndex(const TArray<int32>& Tokens)
	{
		for (int32 I = 3; I + 2 < Tokens.Num(); ++I)
		{
			if (Tokens.IndexOfByKey(Tokens[I]) == I)
			{
				return I;
			}
		}
		return INDEX_NONE;
	}

	FSuperSLMRuntimeConfig TokenizeConfig(int32 LayerBudget)
	{
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = 1;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = LayerBudget;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 16.6;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMWorkerContinuationTest,
	"SuperSLM.L2S1.WorkerContinuation.RealRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMWorkerContinuationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	FString AExPath, Reason;
	if (!TryGetAExArtifactPath(AExPath, Reason))
	{
		AddError(FString::Printf(TEXT("SuperSLM.L2S1.WorkerContinuation: %s"), *Reason));
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu))
	{
		return false;
	}
	// Tokenize() reads the configured model, so configure once first (RunArm reconfigures).
	if (!TestEqual(TEXT("Configure() for Tokenize()"), (uint8)Cpu->Configure(Model, TokenizeConfig(24)).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}
	TArray<int32> Prompt, Prompt2;
	if (!TestTrue(TEXT("the prompt tokenizes"), Cpu->Tokenize(TEXT("Customer one: what's the wait time today?"), Prompt) && Prompt.Num() > 0)
		|| !TestTrue(TEXT("the second prompt tokenizes"), Cpu->Tokenize(TEXT("Customer two: do you do cover-ups?"), Prompt2) && Prompt2.Num() > 0))
	{
		return false;
	}
	const uint32 GameThreadId = FPlatformTLS::GetCurrentThreadId();
	bool bOk = true;

	// 1. No stop token: the same tokens and the same state with continuation off and on.
	FArmSpec Plain;
	Plain.Model = Model;
	Plain.Prompts.Add(Prompt);
	FArmResult Off, On;
	if (!RunArm(*this, *Cpu, Plain, false, Off) || !RunArm(*this, *Cpu, Plain, true, On))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("off: the generation completes"), (uint8)Off.Sequences[0].Phase, (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= ExpectSameSequence(*this, Off, On, 0, TEXT("plain"));
	bOk &= TestEqual(TEXT("off: no job is a continuation"), Off.Continuations, 0);
	bOk &= TestTrue(TEXT("on: at least one job is a continuation"), On.Continuations > 0);
	bOk &= AssertRS1bProperties(*this, Off.Ledger, Off.Config, Off.HitchCount, GameThreadId, TEXT("WorkerContinuation off"));
	bOk &= AssertRS1bProperties(*this, On.Ledger, On.Config, On.HitchCount, GameThreadId, TEXT("WorkerContinuation on"));
	AddInfo(FString::Printf(TEXT("WorkerContinuation: %d tokens; off %.3f s, %d jobs; on %.3f s, %d jobs, %d continuations"),
		On.Sequences[0].Tokens.Num(), Off.WallSeconds, Off.Ledger.Num(), On.WallSeconds, On.Ledger.Num(), On.Continuations));

	// 2. A stop token ends the generation early; nothing runs past it.
	const TArray<int32>& Reference = Off.Sequences[0].Tokens;
	const int32 StopIndex = FindStopIndex(Reference);
	if (!TestTrue(TEXT("the reference run has a first-occurrence token to stop on, with tokens after it"), StopIndex != INDEX_NONE))
	{
		return false;
	}
	FArmSpec Stop = Plain;
	Stop.StopTokenIds.Add(TArray<int32>{Reference[StopIndex]});
	const TArray<int32> Expected(Reference.GetData(), StopIndex + 1);
	FArmResult StopOff, StopOn;
	if (!RunArm(*this, *Cpu, Stop, false, StopOff) || !RunArm(*this, *Cpu, Stop, true, StopOn))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("stop, off: the tokens end at the stop token"), StopOff.Sequences[0].Tokens, Expected);
	bOk &= TestEqual(TEXT("stop, off: the generation completes"), (uint8)StopOff.Sequences[0].Phase, (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= ExpectSameSequence(*this, StopOff, StopOn, 0, TEXT("stop"));
	// The job that produced the stop token was running when the tick before it queued its
	// successor (a whole-token CPU decode outlives a 60 Hz tick), so that successor reached the
	// worker after the stop and was skipped there.
	bOk &= TestTrue(*FString::Printf(TEXT("stop, on: the continuation queued behind the stopping job was skipped on the worker (skipped members %d)"), StopOn.SkippedMembers),
		StopOn.SkippedMembers > 0);
	bOk &= AssertRS1bProperties(*this, StopOn.Ledger, StopOn.Config, StopOn.HitchCount, GameThreadId, TEXT("WorkerContinuation stop, on"));

	// 3. Batched: two sequences share each decode call; sequence 0 stops early and sequence 1 runs
	// on. The continuation queued behind sequence 0's last job leaves it out and runs sequence 1.
	FArmSpec Batched;
	Batched.Model = Model;
	Batched.Prompts.Add(Prompt);
	Batched.Prompts.Add(Prompt2);
	FArmResult BatchRef;
	if (!RunArm(*this, *Cpu, Batched, false, BatchRef))
	{
		return false;
	}
	const int32 BatchStop = FindStopIndex(BatchRef.Sequences[0].Tokens);
	if (!TestTrue(TEXT("batched: sequence 0's reference run has a token to stop on"), BatchStop != INDEX_NONE))
	{
		return false;
	}
	Batched.StopTokenIds.Add(TArray<int32>{BatchRef.Sequences[0].Tokens[BatchStop]});
	Batched.StopTokenIds.AddDefaulted();
	FArmResult BatchOff, BatchOn;
	if (!RunArm(*this, *Cpu, Batched, false, BatchOff) || !RunArm(*this, *Cpu, Batched, true, BatchOn))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("batched, off: sequence 0 ends at its stop token"), BatchOff.Sequences[0].Tokens,
		TArray<int32>(BatchRef.Sequences[0].Tokens.GetData(), BatchStop + 1));
	bOk &= TestTrue(TEXT("batched, off: sequence 1 runs on past sequence 0's stop"),
		BatchOff.Sequences[1].Tokens.Num() > BatchOff.Sequences[0].Tokens.Num());
	bOk &= ExpectSameSequence(*this, BatchOff, BatchOn, 0, TEXT("batched"));
	bOk &= ExpectSameSequence(*this, BatchOff, BatchOn, 1, TEXT("batched"));
	int32 PartialSkips = 0;
	for (const FSuperSLMWorkerJobReport& Job : BatchOn.Ledger)
	{
		PartialSkips += (Job.bContinuation && Job.MemberSequences.Num() == 2 && Job.SkippedMembers == 1 && !Job.bSkippedContinuation) ? 1 : 0;
	}
	bOk &= TestTrue(*FString::Printf(TEXT("batched, on: a two-member continuation left out the stopped member and ran the live one (%d such jobs)"), PartialSkips),
		PartialSkips > 0);
	bOk &= AssertRS1bProperties(*this, BatchOn.Ledger, BatchOn.Config, BatchOn.HitchCount, GameThreadId, TEXT("WorkerContinuation batched, on"));

	// 4. An adapter swap requested mid-generation, while a continuation is queued behind the
	// running job, lands on the same token boundary as with continuation off (row 20). A-AD: the
	// 1.5B base and its runtime adapter.
	FSuperSLMImportDiagnostic AdDiag;
	USuperSLMModel* AdBase = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), AdDiag);
	if (!TestNotNull(*FString::Printf(TEXT("A-AD base imports: %s"), *AdDiag.Message), AdBase))
	{
		return false;
	}
	FSuperSLMAdapterHandle Adapter;
	FString AdError;
	if (!TestTrue(*FString::Printf(TEXT("A-AD adapter imports: %s"), *AdError), FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *AdBase, Adapter, AdError)))
	{
		return false;
	}
	const int32 AdLayers = 28; // Qwen2.5-1.5B's layer count: whole-token jobs
	// A-AD's base carries no tokenizer (D-SLM7339), so the arm reuses the prompt A-EX tokenized:
	// both are Qwen2.5 models and share one vocabulary.
	const TArray<int32>& AdPrompt = Prompt;
	FArmSpec Swap;
	Swap.Model = AdBase;
	Swap.LayerBudget = AdLayers;
	Swap.Prompts.Add(AdPrompt);
	Swap.SwapTo = Adapter;
	Swap.SwapAtGenerated = 4;
	// A cancel happened when a skipped continuation is followed by a job that ran: the swap arm
	// has no stop token, so a skip is either a cancel or a generation's end, and nothing runs
	// after a generation's end.
	auto CancelObserved = [](const FArmResult& R)
	{
		for (int32 I = 0; I < R.Ledger.Num(); ++I)
		{
			if (!R.Ledger[I].bSkippedContinuation)
			{
				continue;
			}
			for (int32 J = I + 1; J < R.Ledger.Num(); ++J)
			{
				const FSuperSLMWorkerJobReport& Later = R.Ledger[J];
				if (Later.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && !Later.bSkippedContinuation && Later.SkippedMembers == 0)
				{
					return true;
				}
			}
		}
		return false;
	};
	// The swap is requested after a Tick() in real time, so the worker head can pass it: when
	// the running job finishes before that Tick() and the queued continuation has already started
	// by the request, the request lands after the continuation (plan §5 item 1a: the boundary is
	// the worker head) -- one token later, exactly as a continuation-off request made one token
	// later. Each on attempt is therefore checked against the off run that requested the swap at
	// the boundary it actually met, and at least one attempt must show the cancel itself.
	TMap<int32, FArmResult> OffBySwapAt;
	auto OffReference = [&](int32 SwapAt) -> const FArmResult*
	{
		if (const FArmResult* Found = OffBySwapAt.Find(SwapAt))
		{
			return Found;
		}
		FArmSpec Spec = Swap;
		Spec.SwapAtGenerated = SwapAt;
		FArmResult Ref;
		if (!RunArm(*this, *Cpu, Spec, false, Ref))
		{
			return nullptr;
		}
		TestEqual(*FString::Printf(TEXT("swap, off (request at %d): the request met that token count"), SwapAt), Ref.SwappedAtGenerated, SwapAt);
		TestEqual(TEXT("swap, off: the adapter is active at the end"), Ref.Sequences[0].ActiveAdapterId, Adapter.Id);
		TestEqual(TEXT("swap, off: the generation completes"), (uint8)Ref.Sequences[0].Phase, (uint8)ESuperSLMSequencePhase::Complete);
		return &OffBySwapAt.Add(SwapAt, MoveTemp(Ref));
	};
	bool bCancelSeen = false;
	for (int32 Attempt = 0; Attempt < 3 && !bCancelSeen; ++Attempt)
	{
		FArmResult SwapOn;
		if (!RunArm(*this, *Cpu, Swap, true, SwapOn))
		{
			return false;
		}
		bCancelSeen = CancelObserved(SwapOn);
		const int32 MetAt = SwapOn.SwappedAtGenerated + (bCancelSeen ? 0 : 1);
		const FArmResult* Ref = OffReference(MetAt);
		if (!Ref)
		{
			return false;
		}
		const FString What = FString::Printf(TEXT("swap attempt %d (%s)"), Attempt, bCancelSeen ? TEXT("cancelled the queued continuation") : TEXT("met after the continuation started"));
		bOk &= ExpectSameSequence(*this, *Ref, SwapOn, 0, *What);
		bOk &= AssertRS1bProperties(*this, SwapOn.Ledger, SwapOn.Config, SwapOn.HitchCount, GameThreadId, *What);
		AddInfo(FString::Printf(TEXT("WorkerContinuation %s: requested at %d tokens; on %.3f s, %d continuations"),
			*What, SwapOn.SwappedAtGenerated, SwapOn.WallSeconds, SwapOn.Continuations));
	}
	bOk &= TestTrue(TEXT("swap, on: a request cancelled a queued continuation, which the worker skipped, and ordinary planning carried the swap"), bCancelSeen);
	FString ReleaseError;
	bOk &= TestTrue(*FString::Printf(TEXT("the A-AD adapter releases: %s"), *ReleaseError), FSuperSLMAdapterImport::Release(Adapter, ReleaseError));
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
