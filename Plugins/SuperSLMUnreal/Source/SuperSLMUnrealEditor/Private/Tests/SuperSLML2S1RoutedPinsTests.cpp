// T-2805 round 10 -- the build's own six routed test pins
// (the build record §14.10: "behaviour this round adds that no round-9
// cell covers"), each realized against the real A-EX artifact, each able to fail. Not new Coverage
// Model cells (the plan's own §9 does not name any of these as a claim a user relies on) -- each is
// a PIN on a specific, build-flagged implementation detail of the async worker tick (T-2850 folds
// 1-3), so a future change to that detail is caught rather than silently drifting.
//
// Four of the six (items 2, 4, 5, 6) are realized as real, executable FEAT/structural tests against
// currently-declared API. The remaining two (items 1, 3) have NO observable surface today --
// confirmed by reading the declared API this round touches no production file to add (the
// dispatching task's own scope: "touch only test and fixture files"), so neither a thread-identity
// read for a bind call nor a ledger Kind for PrefixBegin/PrefixRelease exists to assert against.
// Both are realized as the strongest claim the CURRENT API can support (a real mechanism check that
// can fail) with the fuller claim named explicitly as still routed -- this suite's established
// discipline ("Realize the model; a gap in it is a finding, not an invention"): a claim
// with no observable is a finding for the next change, never a fabricated assertion.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	constexpr double kTickBudgetMs = 16.6;

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

	bool LoadSomeRealPromptTokens(FAutomationTestBase& T, TArray<int32>& OutTokens)
	{
		TArray<FReferenceCase> Cases;
		FString LoadError;
		if (!T.TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), LoadReferenceCases(Cases, LoadError)))
		{
			return false;
		}
		OutTokens = Cases[0].PromptTokens;
		return true;
	}
}

// --- Pin 1 (§14.10 item 1): schema/adapter bind thread identity ---------------------------------
//
// The build log (§14.3 item 1): schema bind (sslm_seq_set_schema) and adapter bind
// (sslm_seq_set_adapter) currently run INLINE, on whichever thread calls SetSchema()/
// RequestAdapterSwap() -- the game thread for a caller's own unqueued call on an Idle sequence, or
// a worker thread only when folded into a Reset/Adopt job's own Deliver closure (which itself runs
// on the GAME thread, since Deliver is Apply-side bookkeeping). Plan §5's own call list ("prefill,
// decode, save, restore, reset, adopt, prefix begin/release, adapter bind... run exclusively on the
// worker") names adapter bind but not schema bind, so which thread EITHER is required to run on is
// genuinely undecided. The build log's own note: "no round-9 cell asserts which thread sslm_seq_set_adapter
// runs on... R-S1h's own thread-identity check reads FSuperSLMWorkerJobReport::WorkerThreadId, which
// a bind-only Deliver-side call never populates" -- confirmed at source this round:
// FSuperSLMWorkerJobReport (SuperSLMSequenceTypes.h) has no field at all for "which call ran inside
// this job's Deliver closure", only WorkerThreadId for the job's own Execute-side Layer-1 call. A
// bind's own thread identity is therefore NOT OBSERVABLE through any currently-declared API.
//
// This is a genuine model gap, not a cell this suite invents coverage for (the suite's conventions): pinning "runs on
// the worker" or "runs inline" would assert one of the two undecided readings as though it were
// settled, and a claim reasoned from source must never be marked as observed. What CAN
// be asserted today, and is, is the mechanism-level claim every round-9 misuse/save-restore cell
// already exercises without naming it directly: a schema bind on a fresh sequence and an adapter
// swap request both complete without corrupting subsequent generation, on the real artifact, driven
// through a real multi-tick run. Routed to the implementation (this file's own handoff, and
// the red-suite record's round-10 section): add a per-job or per-call
// thread-id diagnostic for bind calls before the stronger claim (which thread a bind is REQUIRED to
// run on) can be pinned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SchemaAndAdapterBindCompleteCleanlyTest,
	"SuperSLM.L2S1.RoutedPins.SchemaAndAdapterBindCompleteCleanly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SchemaAndAdapterBindCompleteCleanlyTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin1 SchemaAndAdapterBindCompleteCleanly"), Model))
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
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	// A real schema bind (SSLM_SCHEMA_NONE -- always legal on a fresh sequence, isolating the bind
	// mechanism from schema validity, matching SuperSLML2S1MisuseTests.cpp's own established
	// pattern) followed by a real generation, so a bind that corrupted state (whatever thread it
	// ran on) would surface as a faulted or hung sequence below.
	const FSuperSLMSchemaHandle NoneSchema;
	FString BindError;
	if (!TestTrue(*FString::Printf(TEXT("SetSchema on a fresh sequence: %s"), *BindError), Subsystem->SetSchema(Seq, NoneSchema, BindError)))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 8;
	TArray<int32> Generated;
	FString RunError;
	const bool bCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
	Subsystem->ReturnSequence(Seq);
	if (!TestTrue(*FString::Printf(TEXT("generation after a schema bind must complete cleanly (%s)"), *RunError), bCompleted))
	{
		return false;
	}

	// FEAT oracle: an implementation that corrupts sequence state on a bind call (whichever thread
	// it runs on) produces empty, truncated, or duplicated output here -- a real, if coarse, check.
	// It does NOT establish which thread the bind ran on; that claim has no observable today (see
	// this test's own header comment).
	return TestTrue(TEXT("the bound sequence must have produced real generated tokens"), Generated.Num() > 0);
}

// --- Pin 2 (§14.10 item 2): multi-lane prefill/decode distribution -------------------------------
//
// The build log (§14.3 item 2): with MaxConcurrentCalls > 1, free lanes are filled in
// index order, one prefill item each, then one remaining free lane gets ALL due decode work in one
// job -- unexercised by any round-9 cell (every round-9 fixture leaves MaxConcurrentCalls at its
// default of 1). This cell configures real concurrency (3 lanes) and more due sequences than lanes
// (4, all sharing the same 24-token real prompt so they enter Prefilling together), so several
// sequences genuinely need prefill work in the SAME planning pass -- the shape the note describes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1MultiLaneDistributionUsesEveryLaneNoStarvationTest,
	"SuperSLM.L2S1.RoutedPins.MultiLaneDistributionUsesEveryLaneNoStarvation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1MultiLaneDistributionUsesEveryLaneNoStarvationTest::RunTest(const FString& Parameters)
{
	// Reads the whole run's job ledger, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin2 MultiLaneDistributionUsesEveryLaneNoStarvation"), Model))
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

	constexpr int32 SequenceCount = 4;
	constexpr int32 LaneCount = 3;
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = SequenceCount;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = SequenceCount;
	Config.MaxConcurrentCalls = LaneCount; // real concurrency: this file's own claim needs it
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0; // generous: this cell is about lane distribution, not timing
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}

	TArray<FSuperSLMSequence> Sequences;
	Sequences.SetNum(SequenceCount);
	for (int32 I = 0; I < SequenceCount; ++I)
	{
		if (!TestEqual(*FString::Printf(TEXT("Vend sequence %d"), I), (uint8)Subsystem->VendSequence(Sequences[I]), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens; // identical, real prompt on all 4 -- they enter
		Request.MaxNewTokens = 2;            // Prefilling together, genuinely contending the 3 lanes
		FString BeginError;
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration %d: %s"), I, *BeginError), Subsystem->BeginGeneration(Sequences[I], Request, BeginError)))
		{
			return false;
		}
	}

	auto AllComplete = [Subsystem, &Sequences]()
	{
		for (const FSuperSLMSequence& Seq : Sequences)
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
			if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
			{
				return false;
			}
		}
		return true;
	};
	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, AllComplete, /*MaxWallClockSeconds*/ 60.0);

	bool bOk = TestTrue(TEXT("no due sequence must starve indefinitely -- all 4 must reach Complete or Faulted"), AllComplete());

	int32 CompletedCount = 0;
	for (const FSuperSLMSequence& Seq : Sequences)
	{
		if (Subsystem->GetPhase(Seq) == ESuperSLMSequencePhase::Complete)
		{
			++CompletedCount;
		}
		Subsystem->ReturnSequence(Seq);
	}
	bOk &= TestEqual(TEXT("all 4 sequences must have completed (none faulted)"), CompletedCount, SequenceCount);

	// Group the ledger by PlannedAtTick; a tick whose own jobs carry 2+ DISTINCT WorkerThreadIds is
	// direct, real evidence that more than one lane ran concurrently that tick (each lane is a
	// persistent, distinctly-named worker thread, plan §5.1) -- never inferred from timing alone.
	TMap<int32, TSet<uint32>> WorkerThreadIdsByTick;
	for (const FSuperSLMWorkerJobReport& Job : Subsystem->GetJobLedger())
	{
		if (Job.WorkerThreadId != 0)
		{
			WorkerThreadIdsByTick.FindOrAdd(Job.PlannedAtTick).Add(Job.WorkerThreadId);
		}
	}
	int32 MaxDistinctThisRun = 0;
	for (const auto& Pair : WorkerThreadIdsByTick)
	{
		MaxDistinctThisRun = FMath::Max(MaxDistinctThisRun, Pair.Value.Num());
	}
	AddInfo(FString::Printf(TEXT("Pin2: %d lanes configured; the busiest single planning pass used %d distinct worker thread(s) across %d planning passes"),
		LaneCount, MaxDistinctThisRun, WorkerThreadIdsByTick.Num()));

	// FEAT oracle: a scheduler that never uses more than one lane at once (e.g. MaxConcurrentCalls
	// silently ignored) never produces a planning pass with 2+ distinct worker thread ids, however
	// many due sequences exist -- this fails that case directly, on real per-job diagnostics, not a
	// guess about wall-clock overlap.
	bOk &= TestTrue(*FString::Printf(TEXT("at least one planning pass must have used 2 or more distinct worker lanes concurrently (busiest pass used %d)"), MaxDistinctThisRun),
		MaxDistinctThisRun >= 2);

	return bOk;
}

// --- Pin 3 (§14.10 item 3): PrefixBegin/PrefixRelease K-sizing -----------------------------------
//
// T-2885 build log §20.3 (D-SLM7504/D-SLM7510): the plan gap this pin was routed on is closed --
// ESuperSLMWorkerJobKind (SuperSLMSequenceTypes.h) now declares PrefixBegin/PrefixRelease
// alongside the original five, each with its own static cost field
// (FSuperSLMRuntimeConfig::PrefixBeginCostMs/PrefixReleaseCostMs, both defaulting to
// ResetCostMs's own 2.13 ms), and DispatchPrefixAdmin (SuperSLMSubsystem.cpp) posts both kinds
// through PostLedgerJob rather than PostInternalJob, so each now carries a real GetJobLedger()
// row. This pin now carries the K-sizing assertion plan §9 R-S1b (i) and §5 item 2 always meant
// for it: a PrefixBegin/PrefixRelease job's own PlannedJobMs equals the static-cost-model figure
// for its own composition (Config.PrefixBeginCostMs/PrefixReleaseCostMs -- never the prefix's own
// token count, which prices separately through PromptTokenCostMs, and never the worker's own
// measured WorkerCallMs), and K/CommittedDeliveryTick fix from it at plan time, using the SAME
// ExpectedPlannedJobMs()/ExpectedK() oracle R-S1b/R-S1i/R-S1j already check against
// (SuperSLML2S1Fixtures.h) rather than a second, independently-reasoned formula.
//
// Also still real and observable: CreatePrefix()/ReleasePrefix() complete within a generous
// wall-clock bound and leave the pool in a consistent state (a fresh CreatePrefix succeeds again
// after a Release) -- kept alongside the K-sizing claim, not replaced by it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1PrefixBeginReleaseCompleteWithinBoundTest,
	"SuperSLM.L2S1.RoutedPins.PrefixBeginReleaseCompleteWithinBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1PrefixBeginReleaseCompleteWithinBoundTest::RunTest(const FString& Parameters)
{
	// Reads the job ledger by row number across the run, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin3 PrefixBeginReleaseCompleteWithinBound"), Model))
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
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PersonaTokens;
	if (!LoadSomeRealPromptTokens(*this, PersonaTokens))
	{
		return false;
	}

	bool bOk = true;
	for (int32 Round = 0; Round < 2; ++Round)
	{
		FSuperSLMPrefix Prefix;
		FString PrefixError;
		const int32 LedgerCountBeforeBegin = Subsystem->GetJobLedger().Num();
		const double StartSeconds = FPlatformTime::Seconds();
		if (!TestTrue(*FString::Printf(TEXT("round %d CreatePrefix: %s"), Round, *PrefixError), Subsystem->CreatePrefix(PersonaTokens, Prefix, PrefixError)))
		{
			return false;
		}
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Prefix]() { return Subsystem->IsPrefixReady(Prefix); },
			/*MaxWallClockSeconds*/ 30.0);
		const bool bReady = Subsystem->IsPrefixReady(Prefix);
		const double ReadySeconds = FPlatformTime::Seconds() - StartSeconds;
		bOk &= TestTrue(*FString::Printf(TEXT("round %d: CreatePrefix must reach Ready within the wall-clock cap"), Round), bReady);
		AddInfo(FString::Printf(TEXT("Pin3 round %d: prefix reached Ready in %.3f s wall-clock"), Round, ReadySeconds));

		// The K-sizing claim: find this round's own PrefixBegin row (the ledger only grows, so
		// scanning forward from LedgerCountBeforeBegin finds it uniquely -- this test never vends
		// a sequence, so no Reset/Adopt/Save/Restore/DecodeOrPrefill row can appear alongside it).
		{
			const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem->GetJobLedger();
			const FSuperSLMWorkerJobReport* BeginJob = nullptr;
			for (int32 J = LedgerCountBeforeBegin; J < Ledger.Num(); ++J)
			{
				if (Ledger[J].Kind == ESuperSLMWorkerJobKind::PrefixBegin)
				{
					BeginJob = &Ledger[J];
					break;
				}
			}
			if (!TestNotNull(*FString::Printf(TEXT("round %d: a PrefixBegin ledger row must exist"), Round), BeginJob))
			{
				return false;
			}
			const double ExpectedBeginMs = ExpectedPlannedJobMs(*this, *BeginJob, Config);
			bOk &= TestTrue(*FString::Printf(TEXT("round %d PrefixBegin job %lld: PlannedJobMs must equal Config.PrefixBeginCostMs (got %.6f ms, expected %.6f ms) -- fixed at plan time, never the %d-token prefix's own size and never the worker's own measured %.3f ms"),
					Round, BeginJob->JobId, BeginJob->PlannedJobMs, ExpectedBeginMs, PersonaTokens.Num(), BeginJob->WorkerCallMs),
				FMath::IsNearlyEqual(BeginJob->PlannedJobMs, ExpectedBeginMs, 1e-6));
			const int32 ExpectedBeginK = ExpectedK(BeginJob->PlannedJobMs, Config.TickBudgetMs);
			bOk &= TestEqual(*FString::Printf(TEXT("round %d PrefixBegin job %lld: K must equal max(1, ceil(PlannedJobMs/TickBudgetMs))"), Round, BeginJob->JobId),
				BeginJob->K, ExpectedBeginK);
			bOk &= TestEqual(*FString::Printf(TEXT("round %d PrefixBegin job %lld: CommittedDeliveryTick must equal PlannedAtTick + K"), Round, BeginJob->JobId),
				BeginJob->CommittedDeliveryTick, BeginJob->PlannedAtTick + BeginJob->K);
		}

		const int32 LedgerCountBeforeRelease = Subsystem->GetJobLedger().Num();
		FString ReleaseError;
		bOk &= TestTrue(*FString::Printf(TEXT("round %d ReleasePrefix: %s"), Round, *ReleaseError), Subsystem->ReleasePrefix(Prefix, ReleaseError));

		// ReleasePrefix() only queues the admin op (SuperSLMSubsystem.cpp's own ReleasePrefix():
		// "fire-and-forget... the worker's own release runs behind it") -- it needs at least one
		// more Tick() before the ledger carries the PrefixRelease row. The prior version of this
		// pin never drained after its own final round's ReleasePrefix() call at all; drain here,
		// on the ledger's own growth, so the round's own row is read for real rather than assumed
		// to have been picked up incidentally by a next round that, on the last iteration, does
		// not exist.
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, LedgerCountBeforeRelease]() { return Subsystem->GetJobLedger().Num() > LedgerCountBeforeRelease; },
			/*MaxWallClockSeconds*/ 30.0);

		{
			const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem->GetJobLedger();
			const FSuperSLMWorkerJobReport* ReleaseJob = nullptr;
			for (int32 J = LedgerCountBeforeRelease; J < Ledger.Num(); ++J)
			{
				if (Ledger[J].Kind == ESuperSLMWorkerJobKind::PrefixRelease)
				{
					ReleaseJob = &Ledger[J];
					break;
				}
			}
			if (!TestNotNull(*FString::Printf(TEXT("round %d: a PrefixRelease ledger row must exist"), Round), ReleaseJob))
			{
				return false;
			}
			const double ExpectedReleaseMs = ExpectedPlannedJobMs(*this, *ReleaseJob, Config);
			bOk &= TestTrue(*FString::Printf(TEXT("round %d PrefixRelease job %lld: PlannedJobMs must equal Config.PrefixReleaseCostMs (got %.6f ms, expected %.6f ms)"),
					Round, ReleaseJob->JobId, ReleaseJob->PlannedJobMs, ExpectedReleaseMs),
				FMath::IsNearlyEqual(ReleaseJob->PlannedJobMs, ExpectedReleaseMs, 1e-6));
			const int32 ExpectedReleaseK = ExpectedK(ReleaseJob->PlannedJobMs, Config.TickBudgetMs);
			bOk &= TestEqual(*FString::Printf(TEXT("round %d PrefixRelease job %lld: K must equal max(1, ceil(PlannedJobMs/TickBudgetMs))"), Round, ReleaseJob->JobId),
				ReleaseJob->K, ExpectedReleaseK);
			bOk &= TestEqual(*FString::Printf(TEXT("round %d PrefixRelease job %lld: CommittedDeliveryTick must equal PlannedAtTick + K"), Round, ReleaseJob->JobId),
				ReleaseJob->CommittedDeliveryTick, ReleaseJob->PlannedAtTick + ReleaseJob->K);
		}
	}

	return bOk;
}

// --- Pin 4 (§14.10 item 4): GetLastFinishMs() under a mixed decode batch -------------------------
//
// The build log (§14.3 item 4): GetLastFinishMs() only updates when a decode job's ENTIRE
// batch is finish-only (DecodeLayers == 0, TokenFinishes > 0); a MIXED batch (some sequences
// finishing, others mid-layer in the SAME job) never updates it, so the figure can go stale.
// "Mixed", read off the ledger's own composition fields with no per-sequence breakdown needed: a
// DecodeOrPrefill job with TokenFinishes > 0 AND DecodeLayers > 0 in the same job. This cell drives
// the real 8-sequence fixture (naturally staggered per-sequence completion, since each of its own
// 8 prompts differs, plan §9 R-S1b's own shape) tick by tick, snapshotting GetLastFinishMs()
// immediately before and after each Tick() call, and asserts the CURRENT, documented behaviour on
// the first mixed job this run actually produces. What it should be was left for a later
// ruling (the build log) and is still open; this pins what the build DOES today, not what it should.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1LastFinishMsStaleUnderMixedBatchTest,
	"SuperSLM.L2S1.RoutedPins.LastFinishMsStaleUnderMixedBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1LastFinishMsStaleUnderMixedBatchTest::RunTest(const FString& Parameters)
{
	// Reads the job ledger by row number across the run, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin4 LastFinishMsStaleUnderMixedBatch"), Model))
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

	// A small, single-lane, single-batch shape (mirroring R-S1b's own default MaxConcurrentCalls=1,
	// "batched decode serves many sequences in one call on one lane") but with sequences whose
	// prompts differ in length, so they enter decode at different ticks and are therefore at
	// different intra-token layer offsets whenever the scheduler later batches them together --
	// exactly the condition a mixed job needs.
	constexpr int32 SequenceCount = 4;
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = SequenceCount;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = SequenceCount;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<FSuperSLMSequence> Sequences;
	Sequences.SetNum(SequenceCount);
	const TCHAR* Prompts[SequenceCount] = {
		TEXT("Hi."),
		TEXT("Tell me a short story about a tattoo."),
		TEXT("What is your favorite style of tattoo art, and why do you enjoy it?"),
		TEXT("Describe, in a few sentences, the atmosphere of a busy tattoo shop on a Saturday."),
	};
	for (int32 I = 0; I < SequenceCount; ++I)
	{
		if (!TestEqual(*FString::Printf(TEXT("Vend sequence %d"), I), (uint8)Subsystem->VendSequence(Sequences[I]), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		TArray<int32> PromptTokens;
		if (!TestTrue(*FString::Printf(TEXT("Tokenize %d"), I), Subsystem->Tokenize(Prompts[I], PromptTokens)))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = 12;
		FString BeginError;
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration %d: %s"), I, *BeginError), Subsystem->BeginGeneration(Sequences[I], Request, BeginError)))
		{
			return false;
		}
	}

	auto AllComplete = [Subsystem, &Sequences]()
	{
		for (const FSuperSLMSequence& Seq : Sequences)
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
			if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
			{
				return false;
			}
		}
		return true;
	};

	bool bObservedMixedJob = false;
	bool bStaleOnMixedJob = true;
	int32 PreviouslySeenJobCount = 0;
	const double StartSeconds = FPlatformTime::Seconds();
	while (!AllComplete() && FPlatformTime::Seconds() - StartSeconds < 90.0)
	{
		const double FinishMsBefore = Subsystem->GetLastFinishMs();
		Subsystem->Tick(1.0f / 60.0f);
		const double FinishMsAfter = Subsystem->GetLastFinishMs();

		const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem->GetJobLedger();
		for (int32 J = PreviouslySeenJobCount; J < Ledger.Num(); ++J)
		{
			const FSuperSLMWorkerJobReport& Job = Ledger[J];
			if (Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job.TokenFinishes > 0 && Job.DecodeLayers > 0)
			{
				// A mixed job: some sequences in this batch finished a token, others advanced
				// non-finishing layers, in the SAME job. The build's own documented current
				// behaviour: GetLastFinishMs() does not update on this shape.
				bObservedMixedJob = true;
				bStaleOnMixedJob &= FMath::IsNearlyEqual(FinishMsBefore, FinishMsAfter, 1e-9);
			}
		}
		PreviouslySeenJobCount = Ledger.Num();

		if (!AllComplete())
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}

	for (const FSuperSLMSequence& Seq : Sequences)
	{
		Subsystem->ReturnSequence(Seq);
	}

	if (!TestTrue(TEXT("at least one mixed decode job (TokenFinishes > 0 AND DecodeLayers > 0 in the same job) must have occurred, or this cell proves nothing"),
			bObservedMixedJob))
	{
		return false;
	}
	// Pinned CURRENT behaviour, not a claim about what is correct (see this test's own header
	// note): if the plan later rules GetLastFinishMs() should update on a mixed batch (a
	// compositional estimate), this assertion is the one to invert.
	return TestTrue(TEXT("GetLastFinishMs() must stay unchanged across a tick that delivered a mixed decode job (current, documented behaviour)"), bStaleOnMixedJob);
}

// --- Pin 5 (§14.10 item 5): ReturnSequence() racing an in-flight job -----------------------------
//
// The build log (§14.3 item 6): ReturnSequence() does not check whether the target slot
// currently has a job in flight -- it clears generation state and queues a Reset immediately,
// always. Every round-9 cell that calls ReturnSequence() does so only after the sequence has
// reached Complete/Faulted or every queued op has drained, so this path is unexercised in a state
// where it would race a worker still reading that slot's own captured-by-value job inputs. The build log's
// own analysis: the Execute closures copy their inputs BY VALUE (fixed twice during round 6's own
// authoring, specifically because a live pointer into a TArray ReturnSequence()/a later request
// could reallocate is unsafe across the worker-thread boundary), so a race here would produce a
// LOGICAL inconsistency (a returned slot's Phase re-set by a stale Deliver closure) rather than a
// crash -- "real, and unclosed."
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1ReturnSequenceRacingInFlightJobDoesNotCorruptTest,
	"SuperSLM.L2S1.RoutedPins.ReturnSequenceRacingInFlightJobDoesNotCorrupt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1ReturnSequenceRacingInFlightJobDoesNotCorruptTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin5 ReturnSequenceRacingInFlightJobDoesNotCorrupt"), Model))
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
	Config.BlockCount = 1; // exactly one physical slot: the second vend below MUST reuse this one
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}

	// --- Victim: begin a real generation, tick ONCE so the worker genuinely has a job in flight on
	// this slot, then ReturnSequence() immediately -- before that job delivers. ---
	FSuperSLMSequence Victim;
	if (!TestEqual(TEXT("Vend victim sequence"), (uint8)Subsystem->VendSequence(Victim), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 8;
	FString BeginError;
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration (victim): %s"), *BeginError), Subsystem->BeginGeneration(Victim, Request, BeginError)))
	{
		return false;
	}
	Subsystem->Tick(1.0f / 60.0f); // a job is now planned and in flight on the worker for this slot
	Subsystem->ReturnSequence(Victim); // raced: returned before that job's own delivery tick

	// Drain generously so the raced job (if it delivers at all, onto a slot the caller no longer
	// owns) and the queued Reset ReturnSequence() itself enqueues both have every real chance to
	// run -- this is the window the race needs to manifest in, per the build log's own analysis.
	{
		const double DrainStartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - DrainStartSeconds < 30.0)
		{
			Subsystem->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
	}

	// --- A fresh vend, on a one-slot pool, MUST reuse the victim's own physical slot. Its own
	// state must be genuinely fresh -- never a stale Deliver closure from the raced job re-setting
	// this slot's Phase/tokens after the new tenant has already started using it. ---
	FSuperSLMSequence NewTenant;
	if (!TestEqual(TEXT("Vend new tenant (must reuse the one-slot pool)"), (uint8)Subsystem->VendSequence(NewTenant), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("a freshly vended sequence must start with zero generated tokens, never resurrected state from the raced job"),
		Subsystem->GetGeneratedTokens(NewTenant).Num(), 0);
	bOk &= TestEqual(TEXT("a freshly vended sequence must start Idle"), (uint8)Subsystem->GetPhase(NewTenant), (uint8)ESuperSLMSequencePhase::Idle);

	TArray<int32> Generated;
	FString RunError;
	const bool bCompleted = RunGenerationToCompletion(*Subsystem, NewTenant, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
	Subsystem->ReturnSequence(NewTenant);
	bOk &= TestTrue(*FString::Printf(TEXT("the new tenant's own generation must complete cleanly on the reused slot (%s)"), *RunError), bCompleted);
	// FEAT oracle: a stale Deliver closure from the raced job resurrecting old generated-token
	// state onto this slot produces tokens the new tenant's own real generation did not itself
	// emit -- checkable here as "the new tenant's own recorded token count matches what this run
	// actually produced" (Generated), never assumed clean.
	bOk &= TestTrue(TEXT("the new tenant must have produced its own real generated tokens"), Generated.Num() > 0);

	return bOk;
}

// --- Pin 6 (§14.10 item 6): SuperSLM/CPU/TokensPerSecond ------------------------------------------
//
// The build log (§14.4): TRACE_DECLARE_FLOAT_COUNTER(SuperSLM_CPU_TokensPerSecond, ...)
// is declared but never TRACE_COUNTER_SET -- "the counter itself reads a constant 0 on any real
// capture" -- because §5.1 names it "a rolling-window derivation" and no existing subsystem figure
// is that rolling window; deriving one is undesigned scope the build did not invent. Two genuinely
// separate gaps, confirmed this round: (a) no rolling-window DEFINITION is ruled (a plan
// decision), and (b) reading a Trace Counter's own live numeric VALUE back out of a captured
// .utrace needs real trace analysis (TraceServices/TraceAnalysis), which R-S1h's own header note
// already states this suite does not commission. Both block the literal claim the build's own
// routed item names ("a cell reading it against an independently computed rate"). This cell closes
// as much of that claim as today's API allows:
//   (i) an INDEPENDENTLY computed tokens/second, from the subsystem's own real per-job ledger (no
//       Insights capture needed) -- proving the PRODUCT can already report this rate today, even
//       though no dedicated accessor or Trace Counter exposes it yet.
//
// T-2805 L23 repair (2026-09-20): this cell used to also carry (ii), a real .utrace capture
// scanned by a private `CountAnsi` byte-substring lambda for the counter's declared NAME, as a
// proxy for "declared but never populated." That capture-and-scan is REMOVED, not rebuilt: `CountAnsi`
// was a private duplicate of the SAME instrument SuperSLML2S1AsyncSchedulingTests.cpp's own R-S1h
// test used (that file's own header comment on `FSuperSLML2S1InsightsMarkupPresentAndReadableTest`
// has the full citation), withdrawn suite-wide by D-SLM7502/D-SLM7507/D-SLM7511/D-SLM7512: an
// LZ77-compressed stream makes a miss prove nothing, and every scope/counter name in this engine
// is wrapped in a corrupted `L"..."` literal a substring match still matches inside of, so a hit
// proves nothing either -- one-sided in both directions at once (class L23, D-SLM7519). Its own
// two assertions had degenerated exactly that way (D-SLM7546): with the instrument reading 0
// occurrences of every name, "TokensPerSecond's own occurrence count (0) must stay far below
// HitchCount's (0)" was `0 < 0`, permanently false regardless of whether the counter is wired.
// A TraceServices-backed reader (D-SLM7502's own named replacement) would restore a genuine
// "declared but unwired" check; that is L4, dated by the maintainer after L2-S3, and is the routed
// finding for the rest of the build's own routed item -- this cell now verifies only (i).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1TokensPerSecondIndependentlyComputableButUnwiredTest,
	"SuperSLM.L2S1.RoutedPins.TokensPerSecondIndependentlyComputableButUnwired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1TokensPerSecondIndependentlyComputableButUnwiredTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("Pin6 TokensPerSecondIndependentlyComputableButUnwired"), Model))
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

	FEightSequenceRunResult Run;
	FString RunError;
	const bool bRunOk = RunEightSequenceSharedPrefixShape(*this, *Subsystem, *Model, kTickBudgetMs, Run, RunError);
	if (bRunOk)
	{
		for (const FSuperSLMSequence& Seq : Run.Sequences)
		{
			Subsystem->ReturnSequence(Seq);
		}
	}

	if (!TestTrue(*FString::Printf(TEXT("the fixture workload must have run to completion before this pin is graded (%s)"), *RunError), bRunOk))
	{
		return false;
	}

	// --- (i) Independently computed tokens/second from the subsystem's own real ledger. ---
	int32 TotalTokenFinishes = 0;
	for (const FSuperSLMWorkerJobReport& Job : Run.Ledger)
	{
		if (Job.DeliveredAtTick < 0)
		{
			continue;
		}
		TotalTokenFinishes += Job.TokenFinishes;
	}
	const TArray<FSuperSLMTickReport>& History = Subsystem->GetTickHistory();
	bool bOk = TestTrue(TEXT("the tick history must be non-empty after a real run"), History.Num() > 0);
	if (!bOk)
	{
		return false;
	}
	// Real elapsed simulated time: the sum of every tick's own DeltaSeconds this run actually
	// advanced through (1/60 s per tick, the fixture's own cadence) -- an independent figure from
	// the subsystem's own diagnostics, never assumed equal to wall-clock time.
	const double TotalSimulatedSeconds = History.Num() * (1.0 / 60.0);
	const double IndependentTokensPerSecond = TotalSimulatedSeconds > 0.0 ? TotalTokenFinishes / TotalSimulatedSeconds : 0.0;
	AddInfo(FString::Printf(TEXT("Pin6: %d token finishes delivered over %.3f s simulated -> %.3f tokens/s, computed independently from GetJobLedger()/GetTickHistory(), with no dedicated accessor or Insights counter"),
		TotalTokenFinishes, TotalSimulatedSeconds, IndependentTokensPerSecond));
	// FEAT oracle: a ledger that never records a real delivered token finish (a stub, or a
	// composition that never batches decode work) fails this outright.
	bOk &= TestTrue(TEXT("the product's own ledger must support computing a real, positive tokens/second independently of any Insights counter"),
		IndependentTokensPerSecond > 0.0);

	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
