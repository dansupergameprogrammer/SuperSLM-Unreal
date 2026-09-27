// T-2805 -- L2-S1 red suite. Plan cell R-S1e (the plan §9):
// "The import gate and the inspector's numbers are true" (dims 5, 10) -- "A-EX imported under
// two Sequence Lifecycle Budget settings, one above and one below its predicted reset cost ->
// import fails with the context_cap diagnostic, then passes. The inspector's footprint figures
// equal the bytes the subsystem allocates for the same sslm_config. The stat group's counters
// equal the subsystem's own ledger after R-S1b."
//
// Four tests realize the cell (the fourth added on the maintainer's ruling on T-2805 §6 Q3: "it
// is in scope: 'the inspector's numbers are true' is a user-visible claim"). Needs A-EX
// (D-SLM7306, landed 2026-09-18, T-2804).
//
// The "stat group" clause is realized as COUNTER FIDELITY, the shape a sibling plugin in the
// project's private history (not published here) used for this recurring plan pattern: rather
// than reading UE's live Stats-thread state directly (no simple assertion surface exists for
// that without additional scaffolding, which that plugin's tests also noted for trace
// EMISSION specifically), the test asserts the SUBSYSTEM'S OWN diagnostic accessor -- which
// SuperSLMSubsystem.h documents as the value STATGROUP_SuperSLM's counters publish from,
// mirroring the sibling plugin's identical "the live values the counters publish from"
// convention -- against an INDEPENDENTLY, exactly countable expected value, never merely
// internal self-consistency. `PoolLedgerSelfConsistentAcrossVendReturn` (self-consistency) and
// `HitchCounterFidelity` (independent-value fidelity) are both kept: they check different
// things, matching SuperFAISS's own split between B8 (non-perturbation) and B9 (fidelity).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	// Three-way, not a bool: "A-EX is absent" (T-2805 L23 repair: fails the cell by name via
	// AddError, never a silent skip) and "A-EX is present but failed to import" (a genuine test
	// failure) are kept distinct only so each fails the cell for its own diagnosable reason.
	enum class EAExAvailability { Absent, ImportFailed, Ready };

	EAExAvailability ImportAEx(FAutomationTestBase& T, const TCHAR* CellName, USuperSLMModel*& OutModel)
	{
		FString AExPath, Reason;
		if (!TryGetAExArtifactPath(AExPath, Reason))
		{
			T.AddError(FString::Printf(TEXT("%s: %s"), CellName, *Reason));
			return EAExAvailability::Absent;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return EAExAvailability::ImportFailed;
		}
		return EAExAvailability::Ready;
	}

	// A real, valid token array with no dependency on A-EX's own tokenizer -- the first R-S1a
	// reference case's own recorded prompt tokens (Tests/Fixtures/L2S1/PROVENANCE.md). A-EX is
	// the same Qwen2.5-0.5B model A-CPU is (plan §8, §12 decision 9), so these ids need no
	// cross-size vocab assumption the way the A-AD-based cells in the other files do.
	bool LoadSomeRealPromptTokens(FAutomationTestBase& T, TArray<int32>& OutTokens)
	{
		TArray<FReferenceCase> Cases;
		FString LoadError;
		const bool bLoaded = LoadReferenceCases(Cases, LoadError);
		if (!T.TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), bLoaded))
		{
			return false;
		}
		OutTokens = Cases[0].PromptTokens;
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1ImportGateFailsThenPassesTest,
	"SuperSLM.L2S1.ImportGate.SequenceLifecycleBudgetFailsThenPasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1ImportGateFailsThenPassesTest::RunTest(const FString& Parameters)
{
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1e SequenceLifecycleBudgetFailsThenPasses"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	// A budget the real predicted reset cost cannot possibly clear (§5's own worked cap-4096
	// example predicts ~1.4 ms; 0 ms is below any non-zero prediction by construction) and one
	// no real box's reset cost could ever exceed.
	const FSuperSLMSequenceLifecycleReport TooLow = SuperSLMSequenceLifecycleBudget::CheckModel(*Model, /*BudgetMs*/ 0.0);
	const FSuperSLMSequenceLifecycleReport Generous = SuperSLMSequenceLifecycleBudget::CheckModel(*Model, /*BudgetMs*/ 100000.0);

	TestTrue(TEXT("KvBlockSizeBytes must be a real, positive, derived figure"), TooLow.KvBlockSizeBytes > 0);
	TestEqual(TEXT("the two checks must derive the SAME KvBlockSizeBytes (a model fact, independent of the budget argument)"),
		TooLow.KvBlockSizeBytes, Generous.KvBlockSizeBytes);
	TestTrue(TEXT("MeasuredBandwidthBytesPerSec must be a real, positive measurement"), TooLow.MeasuredBandwidthBytesPerSec > 0.0);
	TestTrue(TEXT("PredictedResetMs must be positive for a real KV block"), TooLow.PredictedResetMs > 0.0);

	// "import fails with a diagnostic naming context_cap as the lever" (§5) -- realized here as
	// the report's own bWithinBudget; the import pipeline's own wiring of this report into a
	// rejected FSuperSLMImportDiagnostic is an implementation integration point this suite does not
	// presume the exact shape of (recorded as an open question, §6 Q2).
	TestFalse(TEXT("a 0 ms budget must fail (predicted reset cost is real and positive)"), TooLow.bWithinBudget);
	return TestTrue(TEXT("a 100000 ms budget must pass"), Generous.bWithinBudget);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1PoolFootprintMatchesPredictionTest,
	"SuperSLM.L2S1.ImportGate.PoolFootprintMatchesPrediction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1PoolFootprintMatchesPredictionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1e PoolFootprintMatchesPrediction"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	constexpr int32 BlockCount = 4;
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = BlockCount;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = BlockCount;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;

	const FSuperSLMConfigureReport Report = Subsystem->Configure(Model, Config);
	if (!TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	const FSuperSLMSequenceLifecycleReport Predicted = SuperSLMSequenceLifecycleBudget::CheckModel(*Model, Config.SequenceLifecycleBudgetMs);
	const int64 ReservedBytes = Subsystem->GetKvPoolReservedBytes();

	// "The inspector's footprint figures equal the bytes the subsystem allocates for the same
	// sslm_config" (§9 R-S1e). KV = sslm_kv_block_size(model) * block_count +
	// sslm_kv_pool_overhead_size(model, block_count) (plan §4) -- the overhead term is small
	// relative to one whole block, so this pins the relationship (a completely wrong
	// allocation -- a missing multiply, a double-count -- fails both bounds) without hard-
	// coding the exact overhead formula, which is this module's own implementation detail.
	const int64 FloorBytes = Predicted.KvBlockSizeBytes * BlockCount;
	const int64 CeilingBytes = Predicted.KvBlockSizeBytes * (BlockCount + 1);
	TestTrue(FString::Printf(TEXT("reserved KV bytes (%lld) must be >= block_count * per-block size (%lld)"), ReservedBytes, FloorBytes),
		ReservedBytes >= FloorBytes);
	return TestTrue(FString::Printf(TEXT("reserved KV bytes (%lld) must be < (block_count + 1) * per-block size (%lld) -- overhead must not exceed one whole block"), ReservedBytes, CeilingBytes),
		ReservedBytes < CeilingBytes);
}

// Self-consistency half of the "stat group counters equal the ledger" clause: GetPoolFreeCount()
// + GetPoolOccupiedCount() must equal BlockCount at every point across a full vend/return cycle,
// including the (BlockCount+1)th vend refusal. HitchCounterFidelity below is this file's
// INDEPENDENT-VALUE half (see this file's own header comment for why both are kept).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1PoolLedgerSelfConsistentTest,
	"SuperSLM.L2S1.ImportGate.PoolLedgerSelfConsistentAcrossVendReturn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1PoolLedgerSelfConsistentTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1e PoolLedgerSelfConsistentAcrossVendReturn"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	constexpr int32 BlockCount = 4;
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = BlockCount;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = BlockCount;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	auto CheckLedgerSums = [this, Subsystem, BlockCount](const TCHAR* Label)
	{
		return TestEqual(FString::Printf(TEXT("%s: occupied + free must equal BlockCount"), Label),
			Subsystem->GetPoolOccupiedCount() + Subsystem->GetPoolFreeCount(), BlockCount);
	};

	bool bOk = CheckLedgerSums(TEXT("fresh after Configure()"));
	bOk &= TestEqual(TEXT("fresh after Configure(): occupied must be 0"), Subsystem->GetPoolOccupiedCount(), 0);

	TArray<FSuperSLMSequence> Vended;
	for (int32 I = 0; I < BlockCount; ++I)
	{
		FSuperSLMSequence Seq;
		Subsystem->VendSequence(Seq);
		Vended.Add(Seq);
		bOk &= CheckLedgerSums(*FString::Printf(TEXT("after vending %d"), I + 1));
		bOk &= TestEqual(*FString::Printf(TEXT("after vending %d: occupied must equal %d"), I + 1, I + 1),
			Subsystem->GetPoolOccupiedCount(), I + 1);
	}

	FSuperSLMSequence OneMore;
	bOk &= TestEqual(TEXT("a (BlockCount+1)th vend must report PoolExhausted"),
		(uint8)Subsystem->VendSequence(OneMore), (uint8)ESuperSLMVendResult::PoolExhausted);

	for (int32 I = 0; I < BlockCount; ++I)
	{
		Subsystem->ReturnSequence(Vended[I]);
		bOk &= CheckLedgerSums(*FString::Printf(TEXT("after returning %d"), I + 1));
	}
	bOk &= TestEqual(TEXT("after returning all: occupied must be 0 again"), Subsystem->GetPoolOccupiedCount(), 0);

	return bOk;
}

// Independent-value fidelity half of the "stat group counters equal the ledger" clause
// (T-2805 §6 Q3, ruled in scope by the maintainer). §5.1 names GetHitchCount() among the figures
// "measured by the plugin" that STATGROUP_SuperSLM's counters publish -- this test proves the
// counter tracks a REAL, exactly-countable input rather than being a stub or an
// always-agrees-with-itself accumulator, the same "tracks the caller's real input, not a fixed
// constant" shape the sibling plugin's counter-fidelity test established for its own
// counter.
//
// Round 9 (D-SLM7407): a hitch is now a per-JOB event, gated by real wall-clock time as well as
// tick count (plan §5, "What counts as a hitch": "given that the worker was left at least that
// many ticks' worth of wall time to run it, so a harness ticking faster than real time does not
// blame the worker for its own pace"). This retires the OLD arm-2 oracle ("hitch count equals the
// exact number of Tick() calls this test made") -- under the synchronous build that oracle held
// because a hitch was a per-TICK event and TickBudgetMs bounded the tick itself; under the async
// worker tick, TickBudgetMs bounds a JOB, and the number of jobs delivered in a run is a property
// of the scheduler's own composition-sizing, not of how many times this test happened to call
// Tick(). Both arms derive the exact expected counter value independently from each delivered
// job's committed/delivery ticks and measured worker-call time. A large TickBudgetMs sets K to 1,
// so FastAsPossible ticks can still deliver late; a small budget makes real calls overrun and
// can also deliver late. Each event contributes once to the aggregate counter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1HitchCounterFidelityTest,
	"SuperSLM.L2S1.ImportGate.HitchCounterFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1HitchCounterFidelityTest::RunTest(const FString& Parameters)
{
	// Sums hitches over the whole job ledger against GetHitchCount(), so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1e HitchCounterFidelity"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	// 2026-09-19 fix (maintainer, first real run): both arms previously called Tokenize() without
	// checking its return, and A-EX's own PromptTokens ended up empty -- BeginGeneration() then
	// had nothing to prefill or decode, Arm 1 "completed" vacuously and Arm 2's sequence never
	// reached Complete/Faulted, so its loop ran to the 4096-iteration CAP rather than a real,
	// bounded generation. Real, verified-non-empty fixture ids close this.
	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 8;
	Request.StopTokenIds = {151645, 151643}; // Qwen2.5-Instruct's own ids (SuperSLMSequenceTypes.h)

	bool bOk = true;
	// Review round 4, R4-S1: the product's lateness rule (plan §5, D-SLM7407), applied here from the
	// ledger's own figures: a job is late when it was delivered after its committed tick AND the
	// worker had more than K x TickBudgetMs of real time (posted to finished) for it. Late by ticks
	// alone is a harness ticking faster than real time, not worker lateness, and is not counted.
	// The span is FSuperSLMWorkerJobReport::WorkerSpanMs (ms, finished minus posted, -1 until
	// delivered). Each delivered job's own bHitch is also
	// checked against the rule, so a count that agrees by cancellation still fails.
	auto CountExpectedHitches = [this, &bOk](const TArray<FSuperSLMWorkerJobReport>& Ledger, double TickBudgetMs,
		int32& OutDeliveredJobs, int32& OutLateJobs, int32& OutWorkerOverruns, int32& OutLateByTicksOnly)
	{
		OutDeliveredJobs = 0;
		OutLateJobs = 0;
		OutWorkerOverruns = 0;
		OutLateByTicksOnly = 0;
		int32 HitchFlagMismatches = 0;
		int32 SpansMissing = 0;
		for (const FSuperSLMWorkerJobReport& Job : Ledger)
		{
			if (Job.DeliveredAtTick < 0)
			{
				continue;
			}
			++OutDeliveredJobs;
			SpansMissing += Job.WorkerSpanMs < 0.0 ? 1 : 0;
			const bool bLateByTicks = Job.DeliveredAtTick > Job.CommittedDeliveryTick;
			const bool bLate = bLateByTicks && Job.WorkerSpanMs > double(Job.K) * TickBudgetMs;
			OutLateJobs += bLate ? 1 : 0;
			OutLateByTicksOnly += (bLateByTicks && !bLate) ? 1 : 0;
			OutWorkerOverruns += Job.WorkerCallMs > TickBudgetMs ? 1 : 0;
			HitchFlagMismatches += Job.bHitch != bLate ? 1 : 0;
		}
		bOk &= TestEqual(TEXT("every delivered job carries its worker span (WorkerSpanMs >= 0)"), SpansMissing, 0);
		bOk &= TestEqual(TEXT("every delivered job's bHitch equals (late by ticks AND WorkerSpanMs > K x TickBudgetMs)"),
			HitchFlagMismatches, 0);
		return OutLateJobs + OutWorkerOverruns;
	};

	// --- Arm 1: a large budget floors K to 1. Count any late deliveries explicitly. ---
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 100000.0;
		Config.TickBudgetMs = 100000.0;
		if (!TestEqual(TEXT("Configure() result (generous)"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}

		FSuperSLMSequence Seq;
		if (!TestEqual(TEXT("Vend sequence (generous)"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		TArray<int32> Generated;
		FString RunError;
		const bool bCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
		Subsystem->ReturnSequence(Seq);
		bOk &= TestTrue(*FString::Printf(TEXT("generous-budget generation must complete (%s)"), *RunError), bCompleted);
		int32 DeliveredJobs = 0, LateJobs = 0, WorkerOverruns = 0, LateByTicksOnly = 0;
		const int32 ExpectedHitches = CountExpectedHitches(Subsystem->GetJobLedger(), Config.TickBudgetMs,
			DeliveredJobs, LateJobs, WorkerOverruns, LateByTicksOnly);
		bOk &= TestTrue(TEXT("large-budget arm must deliver jobs"), DeliveredJobs > 0);
		bOk &= TestEqual(*FString::Printf(TEXT("large-budget hitch count must equal late deliveries (%d) plus worker overruns (%d)"),
			LateJobs, WorkerOverruns), Subsystem->GetHitchCount(), ExpectedHitches);
	}

	// --- Arm 2 (T-2805 round 10 rewrite; see this file's own header note on the original defect).
	// The original TickBudgetMs (0.0001 ms) made K = ceil(PlannedJobMs / TickBudgetMs) astronomical
	// for ANY real job (e.g. ceil(46.0 / 0.0001) = 460000 ticks for the first prompt-token prefill),
	// so no job could reach its own CommittedDeliveryTick within a bounded loop -- confirmed at
	// source, the build record §14.6. This round raises TickBudgetMs to
	// 0.1 ms -- still 20x below the fastest measured real per-unit cost (LayerCostMs 2.0 ms, T-2815
	// §11.1), so K stays in the tens-to-low-hundreds per job (reachable within a generous,
	// wall-clock-capped drain) while EVERY real Layer-1 call still unconditionally exceeds
	// TickBudgetMs outright -- Config.TickBudgetMs itself is the wall-time allowance a single call
	// is measured against for bWorkerOverran (SuperSLMSequenceTypes.h: "a Layer-1 call inside this
	// job measured over TickBudgetMs, reported on this job regardless of K's own headroom"), a
	// property that holds by construction and needs no scheduling race, no contention, and no
	// assumption about K/delivery timing at all. Pacing: see the drain below (review round 4, R4-S1). ---
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 100000.0;
		Config.TickBudgetMs = 0.1; // 20x below the fastest measured real per-unit cost (T-2815 §11.1)
		if (!TestEqual(TEXT("Configure() result (tiny budget)"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}

		FSuperSLMSequence Seq;
		if (!TestEqual(TEXT("Vend sequence (tiny budget)"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FString BeginError;
		const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
		bOk &= TestTrue(*FString::Printf(TEXT("BeginGeneration (tiny budget): %s"), *BeginError), bBeginOk);

#if SUPERSLM_WITH_L2S1_ASYNC
		// Review round 4, R4-S1: no Sleep(0.0001) pacing. On Windows a sub-millisecond Sleep() is a
		// SwitchToThread(), so it never paced ticks to TickBudgetMs. None is needed: the expected
		// count comes from each job's own ticks and worker span, whatever the pace, and every real
		// Layer-1 call overruns a 0.1 ms budget regardless. FastAsPossible gives the worker real
		// time between ticks (1 ms) only while work is outstanding.
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Seq]()
			{
				const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
				return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
			},
			/*MaxWallClockSeconds*/ 30.0);
		Subsystem->ReturnSequence(Seq);

		int32 DeliveredJobs = 0, LateJobs = 0, WorkerOverruns = 0, LateByTicksOnly = 0;
		const int32 ExpectedHitches = CountExpectedHitches(Subsystem->GetJobLedger(), Config.TickBudgetMs,
			DeliveredJobs, LateJobs, WorkerOverruns, LateByTicksOnly);
		AddInfo(FString::Printf(TEXT("hitch fidelity: %d jobs delivered, %d late (by ticks and real time), %d late by ticks only (not hitches), %d worker overruns, hitch count %d"),
			DeliveredJobs, LateJobs, LateByTicksOnly, WorkerOverruns, Subsystem->GetHitchCount()));
		bOk &= TestTrue(TEXT("small-budget arm must deliver jobs"), DeliveredJobs > 0);
		bOk &= TestEqual(TEXT("every delivered job must overrun the small per-call budget"), WorkerOverruns, DeliveredJobs);
		bOk &= TestEqual(*FString::Printf(TEXT("small-budget hitch count must equal late deliveries (%d) plus worker overruns (%d)"),
			LateJobs, WorkerOverruns), Subsystem->GetHitchCount(), ExpectedHitches);
#else
		// T-2815's own synchronous form (retired once SUPERSLM_WITH_L2S1_ASYNC is turned on): a
		// hitch was a per-TICK event, so the exact expected count is the number of Tick() calls
		// this loop made.
		int32 TicksRun = 0;
		for (int32 Iteration = 0; Iteration < 4096; ++Iteration)
		{
			Subsystem->Tick(1.0f / 60.0f);
			++TicksRun; // the ONE place this count increments -- one tick, one count
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				break;
			}
		}
		Subsystem->ReturnSequence(Seq);

		AddInfo(FString::Printf(TEXT("hitch fidelity: %d ticks run, hitch count %d"), TicksRun, Subsystem->GetHitchCount()));
		bOk &= TestEqual(TEXT("hitch count must equal the exact number of ticks run under an unmeetable budget"),
			Subsystem->GetHitchCount(), TicksRun);
#endif // SUPERSLM_WITH_L2S1_ASYNC
	}

#if SUPERSLM_WITH_L2S1_ASYNC
	// --- Arm 3 (review round 5, R5-W2): a hitch that is late on both halves, by construction.
	// Arms 1 and 2 check bHitch against the rule but cannot force a job late by ticks AND by real
	// time, so a product that never flags a hitch passes them. Here every static cost is priced
	// near zero, so each job plans at well under TickBudgetMs (2 ms) and K = 1, while every real
	// A-EX job runs tens of ms on the worker. RealTime pacing sleeps 2 ms before each tick (a
	// Sleep of at least 1 ms is a real sleep on Windows, unlike the sub-millisecond one R4-S1
	// removed), so a job's result arrives several ticks after its committed tick, and its worker
	// span exceeds K x TickBudgetMs = 2 ms: late on both halves. ---
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 100000.0;
		Config.TickBudgetMs = 2.0;
		Config.LayerCostMs = 0.001;
		Config.LayerCostPerPositionMs = 0.0;
		Config.PromptTokenCostMs = 0.001;
		Config.PromptTokenCostPerPositionMs = 0.0;
		Config.FinishCostMs = 0.001;
		if (!TestEqual(TEXT("Configure() result (under-priced)"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}

		FSuperSLMSequence Seq;
		if (!TestEqual(TEXT("Vend sequence (under-priced)"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FString BeginError;
		const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
		bOk &= TestTrue(*FString::Printf(TEXT("BeginGeneration (under-priced): %s"), *BeginError), bBeginOk);
		constexpr float StepSeconds = 0.002f; // = TickBudgetMs, and a real sleep on every platform
		DrainTicks(*Subsystem, StepSeconds, EL2S1DrainPacing::RealTime,
			[Subsystem, &Seq]()
			{
				const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
				return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
			},
			/*MaxWallClockSeconds*/ 60.0);
		const ESuperSLMSequencePhase FinalPhase = Subsystem->GetPhase(Seq);
		Subsystem->ReturnSequence(Seq);
		bOk &= TestEqual(TEXT("under-priced generation must complete"), (uint8)FinalPhase, (uint8)ESuperSLMSequencePhase::Complete);

		int32 MaxK = 0;
		for (const FSuperSLMWorkerJobReport& Job : Subsystem->GetJobLedger())
		{
			MaxK = FMath::Max(MaxK, Job.K);
		}
		bOk &= TestEqual(TEXT("construction: every job is planned at K = 1 (the costs are under-priced)"), MaxK, 1);

		int32 DeliveredJobs = 0, LateJobs = 0, WorkerOverruns = 0, LateByTicksOnly = 0;
		const int32 ExpectedHitches = CountExpectedHitches(Subsystem->GetJobLedger(), Config.TickBudgetMs,
			DeliveredJobs, LateJobs, WorkerOverruns, LateByTicksOnly);
		AddInfo(FString::Printf(TEXT("forced hitch: %d jobs delivered, %d late (by ticks and real time), %d late by ticks only, %d worker overruns, hitch count %d"),
			DeliveredJobs, LateJobs, LateByTicksOnly, WorkerOverruns, Subsystem->GetHitchCount()));
		bOk &= TestTrue(TEXT("under-priced arm must deliver jobs"), DeliveredJobs > 0);
		bOk &= TestTrue(TEXT("under-priced arm: at least one job is late on both halves (LateJobs > 0)"), LateJobs > 0);
		bOk &= TestTrue(*FString::Printf(TEXT("under-priced arm: hitch count (%d) >= late jobs (%d)"), Subsystem->GetHitchCount(), LateJobs),
			Subsystem->GetHitchCount() >= LateJobs);
		bOk &= TestEqual(*FString::Printf(TEXT("under-priced hitch count must equal late deliveries (%d) plus worker overruns (%d)"),
			LateJobs, WorkerOverruns), Subsystem->GetHitchCount(), ExpectedHitches);
	}
#endif // SUPERSLM_WITH_L2S1_ASYNC

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
