#pragma once

// Shared set-up for the schema jump-forward cells (schema fast-decode plan rev 1.3.5, §6 slices P1
// and P1-X; §11.3).
//
// Every cell opens with LiveGate(): it fails by name while the engine verbs are the plugin's
// stubs (SUPERSLM_WITH_SCHEMA_JUMP_FORWARD 0) or the test seam is still a stub. Then
// every cell that compares a jump-forward run to an off run also requires span activity
// (RequireSpanActivity): at least one span job that admitted at least one token. Without it an
// unimplemented setting -- which runs off's loop -- would pass every "equals off" assertion.

#include "CoreMinimal.h"
#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Misc/AutomationTest.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSchemaJumpReviewTestAccess.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSchemaJumpTestSeam.h"
#include "SuperSLMSchemaJumpTypes.h"
#include "SuperSLMSubsystem.h"

namespace SuperSLMSchemaJumpFixtures
{
	using namespace SuperSLML2S1Fixtures;

	inline constexpr double kRunSeconds = 180.0;

	// Prompts that reach potion_shop_order's object and so its forced key chains. The first is the
	// L2-S2 self-check's pinned prompt; the rest widen the population for the draft-table cells.
	inline const TArray<FString>& PopulationPrompts()
	{
		static const TArray<FString> Prompts = {
			TEXT("I would like to buy a health potion, please."),
			TEXT("Two mana potions and one antidote, please."),
			TEXT("Can I get three potions of strength?"),
			TEXT("Sell me your cheapest healing draught."),
			TEXT("I need an invisibility potion for tonight."),
			TEXT("One fire resistance potion, and make it quick."),
		};
		return Prompts;
	}

	inline const TCHAR* ModeName(ESuperSLMSchemaJumpForward Mode)
	{
		switch (Mode)
		{
			case ESuperSLMSchemaJumpForward::Off: return TEXT("Off");
			case ESuperSLMSchemaJumpForward::Exact: return TEXT("Exact");
			case ESuperSLMSchemaJumpForward::Canonical: return TEXT("Canonical");
		}
		return TEXT("?");
	}

#if SUPERSLMUNREAL_WITH_GPU
	// A schema-bound GPU generation to its end: Complete, or Faulted with the documented Schema Dead End close (the
	// plugin reports a schema-bound generation whose object closed before MaxNewTokens that way,
	// SuperSLMGpuSubsystem.h GetLastDecodeOutcome) with the walk accepting, so the object is complete: a dead end
	// mid-object leaves the walk not accepting (IsSchemaAccepting) and is an error, as is any other fault.
	// OutTokens holds the run's tokens.
	inline bool RunGpuToSchemaEnd(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const FSuperSLMGenerationRequest& Request,
		TArray<int32>& OutTokens, double MaxWallClockSeconds, FString& OutError)
	{
		if (SuperSLML2S2Fixtures::RunGpuGenerationToCompletion(Gpu, Seq, Request, OutTokens, MaxWallClockSeconds, OutError)) { return true; }
		if (Gpu.GetPhase(Seq) == ESuperSLMSequencePhase::Faulted && Gpu.GetLastDecodeOutcome(Seq) == ESuperSLMDecodeOutcome::SchemaDeadEnd
			&& Gpu.IsSchemaAccepting(Seq))
		{
			OutTokens = Gpu.GetGeneratedTokens(Seq);
			OutError.Reset();
			return true;
		}
		return false;
	}
#endif

	// The first assertion of every cell.
	inline bool LiveGate(FAutomationTestBase& T)
	{
		const bool bVerbs = T.TestTrue(TEXT("the engine's jump-forward verbs are live (SUPERSLM_WITH_SCHEMA_JUMP_FORWARD is 0: they are the plugin's red stubs until the re-vendor at engine 1.15.0)"),
			SuperSLMSchemaJumpVerbsAreReal());
		const bool bSeam = T.TestTrue(TEXT("the schema jump-forward test seam is built (FSuperSLMSchemaJumpTestAccess is still a stub)"),
			FSuperSLMSchemaJumpTestAccess::IsBuilt());
		return bVerbs && bSeam;
	}

	// A scheduling config with room for a whole run's history, a frame budget that does not split
	// chains, and serial decode unless the cell says otherwise.
	inline FSuperSLMRuntimeConfig BaseConfig(int32 BlockCount = 4, int32 MaxSequencesPerDecodeCall = 4)
	{
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = BlockCount;
		Config.MaxSequencesPerDecodeCall = MaxSequencesPerDecodeCall;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		Config.bParallelDecode = false;
		Config.FinishParallelTasks = 1;
		return Config;
	}

	struct FCpuRig
	{
		USuperSLMSubsystem* Cpu = nullptr;
		USuperSLMModel* Model = nullptr;
		FSuperSLMSchemaHandle Schema;
		FSuperSLMRuntimeConfig Config;
	};

	// Loads the walk table now and waits for it (rev 1.3.6, the plan's ruling 4). The table
	// loads at the first generation that is schema-bound and not Off, so Off runs before it read no
	// forcedness and seed nothing. A cell that seeds from Off runs calls this after each Configure()
	// (a re-Configure() drops the table, O6) and before those Off runs.
	inline bool LoadWalkTableForSeeding(FAutomationTestBase& T, const FCpuRig& Rig)
	{
		return T.TestTrue(TEXT("the walk table is loaded before the seeding Off runs (ruling 4)"),
			Rig.Cpu != nullptr && FSuperSLMSchemaJumpReviewTestAccess::LoadSchemaWalkTableNow(*Rig.Cpu));
	}

	inline bool SetUpCpu(FAutomationTestBase& T, UWorld* World, const FSuperSLMRuntimeConfig& Config, FCpuRig& Out)
	{
		FString Path, Error;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX present (%s)"), *Error), TryGetAExArtifactPath(Path, Error))) { return false; }
		FSuperSLMImportDiagnostic Diag;
		Out.Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		if (!T.TestNotNull(TEXT("A-EX imports"), Out.Model) || !T.TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
		Out.Cpu = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("CPU subsystem"), Out.Cpu)) { return false; }
		Out.Config = Config;
		if (!T.TestEqual(TEXT("Configure"), (uint8)Out.Cpu->Configure(Out.Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
		return T.TestTrue(*Error, FSuperSLMSchemaLookup::LookupByName(*Out.Model, AExSchemaName(), Out.Schema, Error));
	}

	inline bool MakeRequest(FAutomationTestBase& T, const FCpuRig& Rig, const FString& Prompt, ESuperSLMSchemaJumpForward Mode,
		int32 MaxNewTokens, FSuperSLMGenerationRequest& Out)
	{
		Out = FSuperSLMGenerationRequest();
		if (!T.TestTrue(TEXT("tokenize the prompt"), Rig.Cpu->Tokenize(Prompt, Out.PromptTokens)) ||
			!T.TestTrue(TEXT("the prompt is nonempty"), !Out.PromptTokens.IsEmpty()))
		{
			return false;
		}
		Out.SpanKind = ESuperSLMSpanKind::Prompt;
		Out.MaxNewTokens = MaxNewTokens;
		Out.SchemaJumpForward = Mode;
		return true;
	}

	struct FRunResult
	{
		FSuperSLMSequence Sequence;
		TArray<int32> Tokens;
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMDecodeOutcome Outcome = ESuperSLMDecodeOutcome::TokenProduced;
		FSuperSLMSequenceStats Stats;
		FString Error;
	};

	// Lets the subsystem recycle what a cell returned before the next run.
	inline void Settle(USuperSLMSubsystem& Cpu)
	{
		for (int32 I = 0; I < 40; ++I) { Cpu.Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.005f); }
	}

	// Vends, binds the schema (unless bUnbound), runs Request to Complete or Faulted, reads the
	// result, and returns the sequence unless bKeep. A run that faults is still a result: the cells
	// compare endings, not only tokens.
	inline bool RunSolo(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMGenerationRequest& Request, FRunResult& Out,
		bool bUnbound = false, bool bKeep = false)
	{
		Out = FRunResult();
		FString Error;
		if (!T.TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Out.Sequence), (uint8)ESuperSLMVendResult::Success)) { return false; }
		if (!bUnbound && !T.TestTrue(TEXT("schema bind"), Rig.Cpu->SetSchema(Out.Sequence, Rig.Schema, Error))) { return false; }
		RunGenerationToCompletion(*Rig.Cpu, Out.Sequence, Request, Out.Tokens, kRunSeconds, Out.Error);
		Out.Phase = Rig.Cpu->GetPhase(Out.Sequence);
		Out.Tokens = Rig.Cpu->GetGeneratedTokens(Out.Sequence);
		Out.Outcome = Rig.Cpu->GetLastDecodeOutcome(Out.Sequence);
		Out.Stats = Rig.Cpu->GetStats(Out.Sequence);
		const bool bEnded = T.TestTrue(*FString::Printf(TEXT("the run ends (%s)"), *Out.Error),
			Out.Phase == ESuperSLMSequencePhase::Complete || Out.Phase == ESuperSLMSequencePhase::Faulted);
		if (!bKeep)
		{
			Rig.Cpu->ReturnSequence(Out.Sequence);
			Settle(*Rig.Cpu);
		}
		return bEnded;
	}

	// The span jobs one sequence ran, in JobId order.
	inline TArray<FSuperSLMSpanJobRecord> SpanJobsOf(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		TArray<FSuperSLMSpanJobRecord> Out;
		for (const FSuperSLMSpanJobRecord& R : FSuperSLMSchemaJumpTestAccess::GetSpanJobs(Cpu))
		{
			if (R.Sequence == Seq) { Out.Add(R); }
		}
		Out.Sort([](const FSuperSLMSpanJobRecord& A, const FSuperSLMSpanJobRecord& B) { return A.JobId < B.JobId; });
		return Out;
	}

	// The non-vacuity guard: a jump-forward run must have run at least one span job that admitted
	// at least one token. An unimplemented setting runs off's loop and passes every "equals off".
	inline bool RequireSpanActivity(FAutomationTestBase& T, const TArray<FSuperSLMSpanJobRecord>& Spans, const TCHAR* What)
	{
		int32 Admitted = 0;
		for (const FSuperSLMSpanJobRecord& R : Spans) { Admitted += R.ForcedIds.Num(); }
		return T.TestTrue(*FString::Printf(TEXT("%s: at least one span job ran (%d)"), What, Spans.Num()), Spans.Num() > 0) &&
			T.TestTrue(*FString::Printf(TEXT("%s: span jobs admitted at least one token (%d)"), What, Admitted), Admitted > 0);
	}

	// The ledger's decode rows that served Seq, as JobIds.
	inline TArray<int64> DecodeJobIdsOf(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		TArray<int64> Out;
		for (const FSuperSLMWorkerJobReport& R : Cpu.GetJobLedger())
		{
			if (R.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && R.MemberSequences.Contains(Seq)) { Out.Add(R.JobId); }
		}
		return Out;
	}

	// True when A is a prefix of B.
	inline bool IsPrefixOf(const TArray<int32>& A, const TArray<int32>& B)
	{
		if (A.Num() > B.Num()) { return false; }
		for (int32 I = 0; I < A.Num(); ++I)
		{
			if (A[I] != B[I]) { return false; }
		}
		return true;
	}

	inline bool IsEnded(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		const ESuperSLMSequencePhase P = Cpu.GetPhase(Seq);
		return (P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted) && Cpu.GetPendingLifecycleOperationCount(Seq) == 0;
	}

	// Begins every request on its own vended sequence (bound to the schema unless its Unbound flag
	// is set) in one tick, so the scheduler can batch them, then ticks until all have ended. One
	// tick's worth of output snapshots per sequence is kept for the listener checks: the plugin has
	// no listener API on the CPU subsystem, so "listeners receive each token once, in order" is
	// read as GetGeneratedTokens() being append-only from tick to tick.
	struct FConcurrentRun
	{
		FSuperSLMGenerationRequest Request;
		bool bUnbound = false;
		FRunResult Result;
		TArray<TArray<int32>> Snapshots;
	};

	// bGatherAtDecode: every run is held before its next decode (the job hold) until all are held, then all are
	// released in one go, so the next tick's decode job serves them together (P1.7, P1X.9). Tokens are unchanged.
	inline bool RunConcurrently(FAutomationTestBase& T, const FCpuRig& Rig, TArray<FConcurrentRun>& Runs, bool bKeep = false,
		bool bGatherAtDecode = false)
	{
		FString Error;
		for (FConcurrentRun& R : Runs)
		{
			R.Result = FRunResult();
			R.Snapshots.Reset();
			if (!T.TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(R.Result.Sequence), (uint8)ESuperSLMVendResult::Success)) { return false; }
			if (!R.bUnbound && !T.TestTrue(TEXT("schema bind"), Rig.Cpu->SetSchema(R.Result.Sequence, Rig.Schema, Error))) { return false; }
		}
		for (FConcurrentRun& R : Runs)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration (%s)"), *Error), Rig.Cpu->BeginGeneration(R.Result.Sequence, R.Request, Error))) { return false; }
		}
		bool bGathering = bGatherAtDecode;
		if (bGathering)
		{
			for (const FConcurrentRun& R : Runs) { FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, R.Result.Sequence, ESuperSLMJobHoldPoint::BeforeDecode); }
		}
		const double Start = FPlatformTime::Seconds();
		auto AllEnded = [&]()
		{
			for (const FConcurrentRun& R : Runs) { if (!IsEnded(*Rig.Cpu, R.Result.Sequence)) { return false; } }
			return true;
		};
		while (!AllEnded() && FPlatformTime::Seconds() - Start < kRunSeconds)
		{
			Rig.Cpu->Tick(1.0f / 60.0f);
			for (FConcurrentRun& R : Runs) { R.Snapshots.Add(Rig.Cpu->GetGeneratedTokens(R.Result.Sequence)); }
			if (bGathering)
			{
				bool bAllHeld = true;
				for (const FConcurrentRun& R : Runs) { bAllHeld &= FSuperSLMSchemaJumpTestAccess::IsJobHeld(*Rig.Cpu, R.Result.Sequence); }
				if (bAllHeld)
				{
					for (const FConcurrentRun& R : Runs) { FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, R.Result.Sequence, ESuperSLMJobHoldPoint::None); }
					bGathering = false;
				}
			}
			FPlatformProcess::Sleep(0.001f);
		}
		bool bOk = T.TestTrue(TEXT("every concurrent run ends"), AllEnded());
		for (FConcurrentRun& R : Runs)
		{
			R.Result.Phase = Rig.Cpu->GetPhase(R.Result.Sequence);
			R.Result.Tokens = Rig.Cpu->GetGeneratedTokens(R.Result.Sequence);
			R.Result.Outcome = Rig.Cpu->GetLastDecodeOutcome(R.Result.Sequence);
			R.Result.Stats = Rig.Cpu->GetStats(R.Result.Sequence);
		}
		if (!bKeep)
		{
			for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
			Settle(*Rig.Cpu);
		}
		return bOk;
	}

	// Listeners receive each token once, in order: every tick's output extends the last tick's,
	// and the last equals the final stream.
	inline bool CheckAppendOnly(FAutomationTestBase& T, const TArray<TArray<int32>>& Snapshots, const TArray<int32>& Final, const TCHAR* What)
	{
		bool bOk = true;
		for (int32 I = 1; I < Snapshots.Num() && bOk; ++I)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: tick %d's output extends tick %d's (no token repeated, dropped or reordered)"), What, I, I - 1),
				IsPrefixOf(Snapshots[I - 1], Snapshots[I]));
		}
		if (Snapshots.Num() > 0)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the last tick's output is the final stream"), What), Snapshots.Last(), Final);
		}
		return bOk;
	}

	// Whether one decode job served every one of Seqs at once (the composition precondition of
	// cells P1.7 and P1X.9).
	inline bool SomeDecodeJobServedAll(const USuperSLMSubsystem& Cpu, const TArray<FSuperSLMSequence>& Seqs)
	{
		for (const FSuperSLMWorkerJobReport& R : Cpu.GetJobLedger())
		{
			if (R.Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill) { continue; }
			bool bAll = true;
			for (const FSuperSLMSequence& S : Seqs) { bAll &= R.MemberSequences.Contains(S); }
			if (bAll) { return true; }
		}
		return false;
	}

	// The run's off twin: the same request with the setting Off, solo.
	inline bool RunOff(FAutomationTestBase& T, const FCpuRig& Rig, FSuperSLMGenerationRequest Request, FRunResult& Out, bool bUnbound = false)
	{
		Request.SchemaJumpForward = ESuperSLMSchemaJumpForward::Off;
		return RunSolo(T, Rig, Request, Out, bUnbound);
	}

	// The run's ending: phase, outcome and tokens.
	inline bool SameEnding(FAutomationTestBase& T, const FRunResult& A, const FRunResult& B, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": tokens")), A.Tokens, B.Tokens);
		bOk &= T.TestEqual(*(What + TEXT(": phase")), (uint8)A.Phase, (uint8)B.Phase);
		bOk &= T.TestEqual(*(What + TEXT(": last decode outcome")), (uint8)A.Outcome, (uint8)B.Outcome);
		return bOk;
	}

	// One JobId counter for span jobs and ledger rows (rev 1.3.5; mutant XD6): no span job shares a
	// JobId with any ledger row, and every span job's JobId lies inside the ledger's range, so the
	// two order against each other. Separate counters collide or fall outside.
	inline bool CheckSharedJobIdCounter(FAutomationTestBase& T, const USuperSLMSubsystem& Cpu, const TArray<FSuperSLMSpanJobRecord>& Spans)
	{
		const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu.GetJobLedger();
		TSet<int64> LedgerIds;
		int64 Lo = TNumericLimits<int64>::Max();
		int64 Hi = TNumericLimits<int64>::Min();
		for (const FSuperSLMWorkerJobReport& R : Ledger)
		{
			LedgerIds.Add(R.JobId);
			Lo = FMath::Min(Lo, R.JobId);
			Hi = FMath::Max(Hi, R.JobId);
		}
		bool bOk = T.TestTrue(TEXT("the ledger has rows"), Ledger.Num() > 0);
		TSet<int64> SpanIds;
		for (const FSuperSLMSpanJobRecord& S : Spans)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("span job %lld: valid JobId"), S.JobId), S.JobId > 0);
			bOk &= T.TestFalse(*FString::Printf(TEXT("span job %lld: no ledger row shares its JobId (one counter)"), S.JobId), LedgerIds.Contains(S.JobId));
			bOk &= T.TestFalse(*FString::Printf(TEXT("span job %lld: no other span job shares its JobId"), S.JobId), SpanIds.Contains(S.JobId));
			bOk &= T.TestTrue(*FString::Printf(TEXT("span job %lld: inside the ledger's JobId range [%lld, %lld]"), S.JobId, Lo, Hi), S.JobId > Lo && S.JobId < Hi);
			SpanIds.Add(S.JobId);
		}
		return bOk;
	}
}
