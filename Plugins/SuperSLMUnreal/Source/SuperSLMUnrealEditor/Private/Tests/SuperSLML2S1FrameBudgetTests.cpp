// T-2805 -- L2-S1 red suite, round 9. Plan cell R-S1b (the plan
// §9, as revised by D-SLM7407/D-SLM7414/D-SLM7424 -- the async worker tick, T-2850 folds 1-3):
// "The CPU scheduler plans every worker job into its headroom and commits each job's delivery tick
// from the plan alone; a result not ready at its tick is counted and reported" -- "A-EX, 8
// concurrent sequences, 64 tokens each, text prompts, handles recycled, one shared persona prefix
// adopted by each (D-SLM7342), a pinned TickBudgetMs of 16.6 at the default BudgetHeadroom.
// Asserted, with no threshold on the outcome: (i) every job's planned worker cost equals the
// static-cost-model figure for its own composition ... never a measured, p99, or
// Sequence-Lifecycle-Budget-predicted figure, and that cost fixes K ... with no job's committed
// delivery tick revised after planning -- checked for the fixture's eight AdoptPrefix() jobs by
// name, not only for decode/prompt/finish jobs. (ii) no Layer-1 symbol appears on a game-thread
// call stack ... (iii) Hitch count equals the number of results whose committed delivery tick came
// before the worker produced them ... (iv) A result applies on the tick it is due, or the first
// later tick at which it exists, and is never reordered ahead of an earlier sequence's result.
// (v) a job's own Layer-1 call time exceeding TickBudgetMs is reported as a worker-side overrun
// exactly on that job."
//
// D-SLM7407 retires the synchronous game-thread tick this cell's prior rounds tested against
// (D-SLM7402: "a 15.6ms tick on the game thread is an ENTIRE game frame, that's absolutely not
// ok") -- the five OLD per-tick floor-based assertions (a lone finish, a minimum-progress layer, a
// prefill floor token, a caller-pinned layer budget, a lifecycle op exempting a synchronous tick's
// own measured duration) no longer apply to anything, because the calling thread no longer runs
// any Layer-1 call at all. This round rewrites every test in this file against the NEW per-JOB
// ledger (USuperSLMSubsystem::GetJobLedger(), SuperSLMSequenceTypes.h) the async worker tick
// produces, realizing the plan's own five properties as SuperSLML2S1Fixtures::AssertRS1bProperties
// (shared with R-S1i, which reuses this cell's own 8-sequence fixture verbatim,
// SuperSLML2S1Fixtures::RunEightSequenceSharedPrefixShape).
//
// Four tests realize the cell:
//   - EightConcurrentSequences: the row's own canonical fixture (8 sequences, shared persona
//     prefix, 64 tokens each) via the shared helper; asserts all five properties against the
//     resulting ledger.
//   - PrefillAdvancesOneTokenPerTick: "a prefill job still advances at least one whole prompt
//     token" (plan §5) -- now a per-JOB claim, not a per-GAME-TICK one (a prefill job's K can
//     exceed 1 tick, so counting Prefilling-phase game ticks no longer corresponds to prompt
//     tokens). Rewritten to read the ledger: every prefill job advances exactly one whole prompt
//     token (PromptTokenCostMs alone already exceeds the default headroom share on this box, so
//     the static cost model can never fit two in one job), and the number of prefill jobs equals
//     the prompt's own token count.
//   - RecycledResetAdoptMeasured: cycles generations through a 2-sequence pool; waits for each
//     returned sequence's queued Reset to appear as delivered in the job ledger before reading
//     GetLastResetMs(), then reports it (AddInfo) beside the importer's prediction. The returned
//     sequence handle is invalid, so its pending-operation accessor cannot observe that Reset.
//   - SharedPrefixAdopted (D-SLM7342): CreatePrefix/AdoptPrefix/ReleasePrefix, updated to
//     AdoptPrefix's new handle-returning signature (D-SLM7421/D-SLM7423).
//
// Needs the tokenizer-bearing A-EX rebuild (D-SLM7339); every test fails the cell by name
// (AddError, never a silent pass -- T-2805 L23 repair) when it is absent, matching the L2-S0
// precedent for an opt-in real-scale cell (SuperSLMMemoryMappingTests.cpp's MM-5, D-SLM4006).
//
// SUPERSLM_WITH_L2S1_ASYNC (SuperSLMSlotGates.h): every test in this file targets the async
// worker tick's OWN API (the per-job ledger, handle-based AdoptPrefix) -- none of them is
// meaningful against T-2815's synchronous build, whose own five floor-based assertions D-SLM7407
// retires outright. This whole translation unit is therefore gated on the switch, which is
// always 1 in 1.0, so it builds and runs.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformTLS.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	constexpr double kTickBudgetMs = 16.6; // one 60 Hz frame -- the plan's own worked cadence throughout §5/§8

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1FrameBudgetConcurrentTest,
	"SuperSLM.L2S1.FrameBudget.EightConcurrentSequences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1FrameBudgetConcurrentTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1b EightConcurrentSequences"), Model))
	{
		case EAExAvailability::Absent: return false; // a missing A-EX now fails the cell (T-2805 L23 repair); ImportAEx already logged the error naming what is absent
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	// The row's own canonical fixture (plan §9 R-S1b): 8 concurrent sequences, 64 tokens each,
	// one shared persona prefix adopted by each. D-SLM7424 requires assertion (i) checked for
	// these eight AdoptPrefix() jobs BY NAME, not only decode/prompt/finish jobs -- the shared
	// helper's own fixture is what R-S1i (SuperSLML2S1AsyncSchedulingTests.cpp) replays twice
	// more, so both cells check the identical request schedule.
	FEightSequenceRunResult Run;
	FString RunError;
	const FSuperSLMRuntimeConfig ConfigUsed = [&]
	{
		FSuperSLMRuntimeConfig C; // mirrors RunEightSequenceSharedPrefixShape's own Config, so this
		C.MaxSequencesPerDecodeCall = 8;      // cell can compute the SAME static-cost-model
		C.MaxPrefillChunkBudget = 64;         // expectation the fixture's own subsystem was built
		C.MaxLayerBudget = 24;                // against, without the helper needing to hand its
		C.BlockCount = 8;                     // Config back out.
		C.PrefixBlockCount = 1;
		C.SequenceLifecycleBudgetMs = 1000.0;
		C.TickBudgetMs = kTickBudgetMs;
		return C;
	}();

	if (!RunEightSequenceSharedPrefixShape(*this, *Subsystem, *Model, kTickBudgetMs, Run, RunError))
	{
		AddError(FString::Printf(TEXT("R-S1b fixture setup failed: %s"), *RunError));
		return false;
	}

	AddInfo(FString::Printf(TEXT("R-S1b: %d ticks run, %d jobs in the ledger, hitch count %d"),
		Run.TicksRun, Run.Ledger.Num(), Run.HitchCount));

	bool bOk = AssertRS1bProperties(*this, Run.Ledger, ConfigUsed, Run.HitchCount,
		FPlatformTLS::GetCurrentThreadId(), TEXT("EightConcurrentSequences"));

	// This fixture's own AdoptPrefix jobs, checked by name (D-SLM7424) rather than folded
	// anonymously into AssertRS1bProperties' generic sweep: exactly 8 (one per sequence), each
	// Kind == Adopt, each PlannedJobMs == Config.AdoptCostMs exactly.
	int32 AdoptJobCount = 0;
	for (const FSuperSLMWorkerJobReport& Job : Run.Ledger)
	{
		if (Job.Kind == ESuperSLMWorkerJobKind::Adopt)
		{
			++AdoptJobCount;
			bOk &= TestTrue(*FString::Printf(TEXT("AdoptPrefix job %lld: PlannedJobMs must equal Config.AdoptCostMs (%.6f ms) exactly, not a measured/p99/Sequence-Lifecycle-Budget figure"),
					Job.JobId, ConfigUsed.AdoptCostMs),
				FMath::IsNearlyEqual(Job.PlannedJobMs, ConfigUsed.AdoptCostMs, 1e-9));
		}
	}
	bOk &= TestEqual(TEXT("exactly 8 AdoptPrefix jobs must appear in the ledger, one per sequence"), AdoptJobCount, 8);

	int32 CompletedCount = 0;
	for (const FSuperSLMSequence& Seq : Run.Sequences)
	{
		if (Subsystem->GetPhase(Seq) == ESuperSLMSequencePhase::Complete)
		{
			++CompletedCount;
		}
		Subsystem->ReturnSequence(Seq);
	}
	bOk &= TestEqual(TEXT("all 8 sequences completed (none faulted)"), CompletedCount, 8);

	return bOk;
}

// D-SLM7407/D-SLM7414: prefill is now a per-JOB claim, not a per-GAME-TICK one. A prompt token's
// own PromptTokenCostMs (46.0 ms default) already exceeds this cell's headroom share (0.7 x 16.6
// = 11.62 ms), so the static cost model can never fit two whole prompt tokens into one job's
// planned cost -- but a prefill job's own K can span several game ticks before it delivers, so
// counting how many game ticks the sequence spends in Prefilling no longer equals the prompt's
// token count (round 8 and earlier rounds' own per-tick counting loop is retired). This cell now
// reads the ledger directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1FrameBudgetPrefillAdvancesOneTokenPerTickTest,
	"SuperSLM.L2S1.FrameBudget.PrefillAdvancesOneTokenPerTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1FrameBudgetPrefillAdvancesOneTokenPerTickTest::RunTest(const FString& Parameters)
{
	// Reads the whole run's job ledger, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1b PrefillAdvancesOneTokenPerTick"), Model))
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

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	// A single whole prompt token through all 24 layers is well above 16.6 ms (T-2815 §11.1's own
	// 46.0 ms median), so a generous chunk budget does not change the composition the static cost
	// model actually picks -- this cell reads what the ledger actually reports, never assumes.
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestTrue(TEXT("configured prompt intercept alone exceeds half the available share"),
		Config.PromptTokenCostMs > Config.TickBudgetMs * Config.BudgetHeadroom * 0.5)) { return false; }

	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	// A deliberately longer prompt (well above one token) so the correspondence between prefill
	// jobs and prompt length is unambiguous.
	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("Tokenize"), Subsystem->Tokenize(
			TEXT("Please describe, in detail, the kind of customer who walks into a tattoo shop on a slow Tuesday afternoon."),
			PromptTokens)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the prompt must be more than one token, or this cell cannot discriminate one-token-per-job from all-at-once"),
			PromptTokens.Num() > 1))
	{
		return false;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 1; // this cell only needs to observe prefill, not the decode that follows
	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}

	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &Seq]()
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		},
		/*MaxWallClockSeconds*/ 60.0);
	Subsystem->ReturnSequence(Seq);

	const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem->GetJobLedger();
	int32 PrefillJobCount = 0;
	bool bOk = true;
	for (const FSuperSLMWorkerJobReport& Job : Ledger)
	{
		if (Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job.PromptTokens > 0)
		{
			++PrefillJobCount;
			// D-SLM7414: "a prefill job still advances at least one whole prompt token" -- on
			// this box, at the default headroom share, PromptTokenCostMs alone already exceeds
			// the share, so the static cost model can never fit a second token in the same job.
			bOk &= TestEqual(*FString::Printf(TEXT("prefill job %lld must advance exactly one whole prompt token"), Job.JobId),
				Job.PromptTokens, 1);
			bOk &= TestEqual(*FString::Printf(TEXT("prefill job %lld must run no decode layers and no finish"), Job.JobId),
				Job.DecodeLayers + Job.TokenFinishes, 0);
		}
	}

	AddInfo(FString::Printf(TEXT("R-S1b: %d prefill jobs for a %d-token prompt"), PrefillJobCount, PromptTokens.Num()));

	// A scheduler that (incorrectly) prefills the WHOLE prompt in one job reports a job count of
	// 1, not PromptTokens.Num(); one that stalls (never advancing) never reaches
	// Decoding/Complete/Faulted within the tick cap, and this loop times out with an empty ledger.
	bOk &= TestEqual(TEXT("prefill job count must equal the prompt's own token count (one whole token per job)"),
		PrefillJobCount, PromptTokens.Num());
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1FrameBudgetRecycledResetAdoptTest,
	"SuperSLM.L2S1.FrameBudget.RecycledResetAdoptMeasured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1FrameBudgetRecycledResetAdoptTest::RunTest(const FString& Parameters)
{
	// Reads the whole run's job ledger, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1b RecycledResetAdoptMeasured"), Model))
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

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 2; // forces every one of the 6 generations below to reuse a handle
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;

	const FSuperSLMConfigureReport Report = Subsystem->Configure(Model, Config);
	if (!TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	// The predicted figure R-S1e's own budget check and the inspector will show, computed the
	// SAME way here so the report is directly comparable (§9 R-S1e's own cross-check: "the
	// inspector's footprint figures equal the bytes the subsystem allocates").
	const double Bandwidth = SuperSLMSequenceLifecycleBudget::MeasureHostWriteBandwidthBytesPerSec();
	const int64 KvBlockSizeBytes = Subsystem->GetKvPoolReservedBytes() / FMath::Max(Config.BlockCount, 1);
	const FSuperSLMSequenceLifecycleReport Predicted = SuperSLMSequenceLifecycleBudget::Predict(
		KvBlockSizeBytes, Bandwidth, /*AdoptToResetRatio*/ 2.07, Config.SequenceLifecycleBudgetMs);
	AddInfo(FString::Printf(TEXT("R-S1b: predicted reset %.3f ms, predicted adopt %.3f ms (KV block %lld bytes @ %.0f B/s)"),
		Predicted.PredictedResetMs, Predicted.PredictedAdoptMs, KvBlockSizeBytes, Bandwidth));

	bool bOk = true;
	TArray<double> MeasuredResetMs;
	constexpr int32 GenerationCount = 6;
	for (int32 G = 0; G < GenerationCount; ++G)
	{
		FSuperSLMSequence Seq;
		const ESuperSLMVendResult VendResult = Subsystem->VendSequence(Seq);
		if (!TestEqual(*FString::Printf(TEXT("Vend generation %d"), G), (uint8)VendResult, (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}

		TArray<int32> PromptTokens;
		Subsystem->Tokenize(TEXT("One short sentence about a tattoo design."), PromptTokens);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = 64;

		FString BeginError;
		const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
		if (!TestTrue(*FString::Printf(TEXT("generation %d BeginGeneration: %s"), G, *BeginError), bBeginOk))
		{
			return false;
		}

		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Seq]()
			{
				const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
				return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
			},
			/*MaxWallClockSeconds*/ 60.0);

		Subsystem->ReturnSequence(Seq);
		// ReturnSequence() invalidates Seq immediately, but its fire-and-forget Reset remains
		// queued. The ledger's delivered Reset count observes that work without the dead handle.
		// Wait for this generation's own delivery before reading GetLastResetMs().
		auto DeliveredResetCount = [Subsystem]()
		{
			int32 Count = 0;
			for (const FSuperSLMWorkerJobReport& Job : Subsystem->GetJobLedger())
			{
				if (Job.Kind == ESuperSLMWorkerJobKind::Reset && Job.DeliveredAtTick >= 0)
				{
					++Count;
				}
			}
			return Count;
		};
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&DeliveredResetCount, G]() { return DeliveredResetCount() >= G + 1; },
			/*MaxWallClockSeconds*/ 30.0);
		if (!TestEqual(*FString::Printf(TEXT("generation %d's queued Reset must deliver"), G),
				DeliveredResetCount(), G + 1))
		{
			return false;
		}
		MeasuredResetMs.Add(Subsystem->GetLastResetMs());
	}

	for (int32 G = 0; G < MeasuredResetMs.Num(); ++G)
	{
		AddInfo(FString::Printf(TEXT("R-S1b: generation %d measured reset %.3f ms (predicted %.3f ms)"),
			G, MeasuredResetMs[G], Predicted.PredictedResetMs));
	}

	// The plan's five threshold-free properties (§9 R-S1b), against this run's own ledger.
	bOk &= AssertRS1bProperties(*this, Subsystem->GetJobLedger(), Config, Subsystem->GetHitchCount(),
		FPlatformTLS::GetCurrentThreadId(), TEXT("RecycledResetAdoptMeasured"));

	// This cell's own Reset jobs, checked by name: exactly 6 (one per generation's own
	// ReturnSequence()), each PlannedJobMs == Config.ResetCostMs exactly.
	int32 ResetJobCount = 0;
	for (const FSuperSLMWorkerJobReport& Job : Subsystem->GetJobLedger())
	{
		if (Job.Kind == ESuperSLMWorkerJobKind::Reset)
		{
			++ResetJobCount;
			bOk &= TestTrue(*FString::Printf(TEXT("Reset job %lld: PlannedJobMs must equal Config.ResetCostMs (%.6f ms) exactly"),
					Job.JobId, Config.ResetCostMs),
				FMath::IsNearlyEqual(Job.PlannedJobMs, Config.ResetCostMs, 1e-9));
		}
	}
	bOk &= TestEqual(TEXT("exactly 6 Reset jobs must appear in the ledger, one per generation"), ResetJobCount, GenerationCount);

	return bOk;
}

// D-SLM7342: shared-prefix adoption serving several NPCs from one persona prefill.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1FrameBudgetSharedPrefixAdoptedTest,
	"SuperSLM.L2S1.FrameBudget.SharedPrefixAdopted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1FrameBudgetSharedPrefixAdoptedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1b SharedPrefixAdopted"), Model))
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

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 3; // 3 NPCs sharing one persona prefix
	Config.PrefixBlockCount = 1; // the prefix's own pool block (plan §5 item 4)
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;

	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PersonaTokens;
	if (!TestTrue(TEXT("Tokenize persona prefix"), Subsystem->Tokenize(
			TEXT("You are a friendly, talkative tattoo artist who loves telling stories about past clients."),
			PersonaTokens)))
	{
		return false;
	}

	FSuperSLMPrefix Prefix;
	FString PrefixError;
	const bool bPrefixCreated = Subsystem->CreatePrefix(PersonaTokens, Prefix, PrefixError);
	if (!TestTrue(*FString::Printf(TEXT("CreatePrefix: %s"), *PrefixError), bPrefixCreated))
	{
		return false;
	}
	// CreatePrefix's own prefill runs on the tick queue (plan §5 item 4) -- drain it before
	// adopting.
	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &Prefix]() { return Subsystem->IsPrefixReady(Prefix); },
		/*MaxWallClockSeconds*/ 30.0);
	if (!TestTrue(TEXT("prefix must become ready within the wall-clock cap"), Subsystem->IsPrefixReady(Prefix)))
	{
		return false;
	}

	constexpr int32 NpcCount = 3;
	double WorstAdoptMs = 0.0;
	bool bOk = true;
	for (int32 I = 0; I < NpcCount; ++I)
	{
		FSuperSLMSequence Seq;
		if (!TestEqual(*FString::Printf(TEXT("Vend NPC %d"), I), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}

		// D-SLM7421/D-SLM7423 (round 9): AdoptPrefix() now queues and returns at once -- Success
		// here means QUEUED, never completed.
		FString AdoptError;
		FSuperSLMLifecycleOpHandle AdoptHandle;
		const ESuperSLMRestoreResult QueueResult = Subsystem->AdoptPrefix(Seq, Prefix, AdoptHandle, AdoptError);
		if (!TestEqual(*FString::Printf(TEXT("AdoptPrefix %d must queue: %s"), I, *AdoptError),
				(uint8)QueueResult, (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &AdoptHandle]() { return Subsystem->GetLifecycleOpResult(AdoptHandle) != ESuperSLMRestoreResult::Pending; },
			/*MaxWallClockSeconds*/ 30.0);
		const ESuperSLMRestoreResult AdoptOutcome = Subsystem->GetLifecycleOpResult(AdoptHandle);
		bOk &= TestEqual(*FString::Printf(TEXT("AdoptPrefix %d must drain to Success"), I),
			(uint8)AdoptOutcome, (uint8)ESuperSLMRestoreResult::Success);
		WorstAdoptMs = FMath::Max(WorstAdoptMs, Subsystem->GetLastAdoptMs());

		TArray<int32> Continuation;
		Subsystem->Tokenize(TEXT("Client walked in and asked:"), Continuation);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Continuation;
		Request.MaxNewTokens = 16;
		TArray<int32> Generated;
		FString RunError;
		const bool bCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
		Subsystem->ReturnSequence(Seq);
		if (!TestTrue(*FString::Printf(TEXT("NPC %d generation with the adopted prefix must complete (%s)"), I, *RunError), bCompleted))
		{
			return false;
		}
	}

	Subsystem->ReleasePrefix(Prefix);

	// "R-S1b measures it [adopt cost]" (plan §5 item 4) -- reported, not asserted against a
	// bound, matching R-S1b's own reset figures.
	AddInfo(FString::Printf(TEXT("R-S1b: worst measured adopt %.3f ms across %d NPCs sharing one prefix"), WorstAdoptMs, NpcCount));
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
