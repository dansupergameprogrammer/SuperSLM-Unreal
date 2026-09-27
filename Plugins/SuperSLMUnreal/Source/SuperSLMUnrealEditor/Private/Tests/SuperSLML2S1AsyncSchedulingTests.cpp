// T-2805 -- L2-S1 red suite, round 9. New cells the async worker tick fold owes (plan
// the plan §9, D-SLM7403/D-SLM7407/D-SLM7414/D-SLM7421/D-SLM7424,
// T-2850 folds 1-3), none of which existed against the synchronous build:
//
// R-S1g -- "The game thread's own Tick() cost -- Plan plus Apply, no Layer-1 call -- stays within
//   a bound independent of this fold's own readings" (D-SLM7407/D-SLM7410). A-EX, R-S1b's own
//   8-sequence shape, SuperSLM.Tick.Plan/.Apply scope durations read from the game thread only.
//   Asserted: the summed Plan+Apply duration stays under 1 ms PER TICK -- the 1 ms bound's own
//   provenance is independent of this cell's own reading (a bound must not come from the reading it grades): T-2815 §12.1's
//   diagnosis of the round-3/4 overrun cause measured the synchronous build's non-Layer-1-call
//   tick overhead at "microseconds" on this box, in an investigation run for a different defect,
//   roughly three orders of magnitude below 1 ms.
//
// R-S1h -- "The CPU backend's Insights markup is present and readable on a real capture"
//   (D-SLM7410/D-SLM7411). A real .utrace of R-S1b's 8-sequence A-EX run (plus a short tail that
//   reaches the §5.1 names R-S1b's run does not), opened in TraceServices and graded by exact name:
//   every §5.1 CPU scope with a real instance count bounded by the product's own job ledger and
//   tick history, every §5.1 counter with a real sample checked against a reading taken outside
//   the trace, Plan/Apply and WorkerJob placed on their threads and read against each other, and
//   the U1 FinishTask arm on an engine task thread. Its channel-off twin
//   (InsightsMarkupAbsentWhenChannelOff) runs the same assertions and must see every one fail.
//   Rebuilt at L4 (2026-09-25); the withdrawn byte scan (D-SLM7502/D-SLM7507) is not used.
//
// R-S1i -- "Delivery-tick timing replays identically across runs, independent of device pace"
//   (D-SLM7414). A-EX, R-S1b's 8-sequence shape run twice -- baseline, then under an added
//   concurrent CPU-bound load thread -- asserting every job's committed delivery tick and planned
//   composition are identical between runs (D-SLM7424: including the fixture's eight AdoptPrefix
//   jobs, named explicitly), while wall-clock worker time and hitch rate are reported, not
//   asserted, and expected to differ.
//
// R-S1j -- "A request against a sequence already mid-job is queued and runs in arrival order,
//   never refused, and never surfaces a BUSY-shaped status from Layer 1; only a full queue is
//   refused" (D-SLM7421/D-SLM7429). Two tests: the row's own fixed-arrival-order scenario (one
//   sequence with a decode job in flight -- one entry of its own budget -- then Save, Reset and
//   AdoptPrefix accepted, filling the bound of 4; a second decode/prefill request refused
//   SequenceQueueFull) and its 200-collision sweep (40 sequences x 5 requests each, no
//   pre-existing in-flight job, each sequence's queue filled to exactly 4 before its 5th is
//   refused).
//
//   RestoreSequence() DROPS OUT of the same-sequence collision list entirely (maintainer ruling
//   D-SLM7457, folded in this round): Restore always vends a fresh sequence (R-S1c's own row,
//   "restore into a fresh handle") and cannot itself be "already mid-job" the instant it is
//   vended, so it cannot collide with a busy sequence's own queue the way Save/Reset/AdoptPrefix/a
//   decode submission can. This supersedes this file's own earlier reading (a
//   RestoreSequence(TargetSequence, ...) overload targeting an existing sequence), which is
//   removed from SuperSLMSubsystem.h this round rather than kept as an unused parallel form.
//   Restore's own queue-admission behavior (never Layer 1's busy status, refused only when ITS
//   OWN freshly-vended sequence's queue is somehow already full -- which a fresh vend never is)
//   is not a distinct claim R-S1j needs a cell for.
//
// SUPERSLM_WITH_L2S1_ASYNC (SuperSLMSlotGates.h): every cell in this file is new (no old
// synchronous equivalent) and targets the async worker tick's own API. The switch is always 1 in 1.0, so
// every cell here builds and runs.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h" // TRACE_BOOKMARK
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "Trace/Trace.h"
#include "Trace/Detail/Channel.h" // UE::Trace::FChannel::IsEnabled (N5 channel restore)
#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

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

	// R-S1i's own "added concurrent CPU load" (plan §9) now lives in Fixtures.h as
	// FL2S1BusyLoadRunnable/StartL2S1BusyLoad/StopL2S1BusyLoad (T-2805 round 10), shared with the
	// ImportGate hitch-counter arm so both use the identical contention mechanism.

	// =============================================================================================
	// R-S1h's grading instrument, rebuilt on TraceServices (L4, 2026-09-25; D-SLM7502, D-SLM7507,
	// D-SLM7511, D-SLM7512, D-SLM7519). Record: the test record.
	//
	// The withdrawn raw-byte .utrace scan is gone and is not reintroduced: no byte of the capture
	// file is read here. The capture is opened by TraceServices (the module Unreal Insights is built
	// on) and read through its providers -- ITimingProfilerProvider (timers, CreateAggregation
	// instance counts, per-thread timelines with depth), ICounterProvider (every counter sample) and
	// IThreadProvider (thread names) -- by EXACT name, never substring. The reader pattern is ported
	// from R-S2h (SuperSLML2S2InsightsTests.cpp, T-2816 round 8, commissioned on its own capture
	// pair), with three corrections that sibling does not carry:
	//   * CreateAggregation rows are SUMMED per name. TraceServices merges same-named scopes into
	//     one timer, but keeps two timers with one name when both were already in use
	//     (CpuProfilerTraceAnalysis.cpp, DefineMergedTimer); R-S2h's TMap::Add keeps only the last.
	//   * A counter with zero samples in the capture is a FAILURE, never an informational pass. An
	//     absent reading is not an answer (D-SLM7519).
	//   * The reader states when it cannot answer and every grade then fails loudly (below).
	//
	// WHAT THE READER DOES WHEN IT CANNOT ANSWER. ReadRs1hCapture sets bAnswered = false, with the
	// reason, when: the TraceServices analysis service is missing; StartAnalysis returns no session;
	// analysis does not complete within 60 s; the session's duration is not positive; the timing,
	// thread or counter provider is absent; the timer table holds zero timers; no thread carries a
	// CPU timeline; CreateAggregation returns no table; or the aggregation and the timelines disagree
	// about any tracked scope's instance count. Both cells then fail with that reason and grade
	// nothing. Neither cell can pass on a dead reader: the channel-on cell needs real counts, and
	// the channel-off cell first requires the reader to see this test's own CPU probe scope
	// (kRs1hProbeCount instances) in the SAME capture, so its zeros are read, not defaulted.
	//
	// NAME CORRECTNESS (D-SLM7511). Read at source on this branch: every CPU scope site is
	// TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL with a bare literal (17 in SuperSLMSubsystem.cpp,
	// 1 in SuperSLMFinishHook.cpp), plus one TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL
	// (SuperSLMSubsystem.cpp, the DecodeLayers/Finish choice, a dynamic name). No site uses the
	// stringifying TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL. The exact intended names below are
	// therefore the names production writes; there is no corrupted-name commissioning branch here.
	//
	// ONE NAME COLLISION, FOUND BY THIS REBUILD (routed, not fixed here): SuperSLMFinishHook.cpp
	// passes "SuperSLM.FinishTask" as the ParallelFor DEBUG NAME as well as its own scope name.
	// ParallelFor's executor opens TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(DebugName) on the CPU channel
	// alone (Async/ParallelFor.h), so an executor scope of the same name appears with SuperSLMChannel
	// OFF, and TraceServices merges both into one timer. A name count of SuperSLM.FinishTask cannot
	// tell the plugin's markup from the engine's, and would pass the channel-off control. The grader
	// therefore counts only FinishTask events whose DIRECT PARENT is the ParallelFor executor scope
	// (plan §5.1: "inside UE's own ParallelFor scope"), and kFinishParallelForDebugName must track the
	// debug name SuperSLMFinishHook.cpp passes. The U1 arm this replaces could not tell them apart,
	// and it compared TraceServices thread ids (the trace's own ids, MiscTraceAnalysis.cpp
	// OnThreadInfo) with FPlatformTLS OS ids, which never match -- so its "separate thread" check
	// was vacuous. Threads are identified here by the trace's own names and by what they carry.
	// =============================================================================================

	constexpr int32 kRs1hProbeCount = 16;
	constexpr int32 kRs1hBlockCount = 8; // RunEightSequenceSharedPrefixShape's own BlockCount
	// The prompt-token cost R-S1h's workload is configured with (box session 2026-09-25). At the
	// default 36 ms a single token exceeds the tick's share (16.6 ms x 0.7), so every prefill call
	// carried one token -- 84 tokens over 84 calls -- and the PrefillChunk bracket could not tell a
	// per-token count from a per-call one. At 1 ms (and no per-position term) a chunk is up to 11
	// tokens, so calls carry several.
	constexpr double kRs1hPromptTokenCostMs = 1.0;
	// Emitted immediately before the workload's final re-Configure. That re-Configure publishes a
	// fresh, zeroed tick report (SuperSLMSubsystem.cpp:1738), so every counter's last sample in the
	// capture is a zero written after the snapshot; the grader reads each counter's last sample at
	// or before this bookmark instead.
	const TCHAR* const kRs1hBeforeReconfigureBookmark = TEXT("R-S1h/BeforeFinalReConfigure");

	// Must equal the ParallelFor debug name SuperSLMFinishHook.cpp passes (see the collision note).
	const TCHAR* const kFinishParallelForDebugName = TEXT("SuperSLM.FinishTask");
	const TCHAR* const kWorkerThreadNamePrefix = TEXT("SuperSLM CPU Worker "); // §5.1 Threads
	const TCHAR* const kProbeCpuScope = TEXT("R-S1h.Probe.Cpu");
	const TCHAR* const kProbeChannelScope = TEXT("R-S1h.Probe.SuperSLMChannel");
	const TCHAR* const kProbeCounter = TEXT("R-S1h/Probe/Counter");

	// Plan §5.1, "CPU backend": every scope it names (18, FinishTask included -- the U1 arm).
	const TCHAR* const kRs1hScopeNames[] = {
		TEXT("SuperSLM.Tick"), TEXT("SuperSLM.Tick.Plan"), TEXT("SuperSLM.Tick.Apply"),
		TEXT("SuperSLM.WorkerJob"),
		TEXT("SuperSLM.Prefill"), TEXT("SuperSLM.PrefillChunk"), TEXT("SuperSLM.PrefixPrefill"),
		TEXT("SuperSLM.DecodeLayers"), TEXT("SuperSLM.Finish"), TEXT("SuperSLM.FinishTask"),
		TEXT("SuperSLM.Reset"), TEXT("SuperSLM.Adopt"), TEXT("SuperSLM.PrefixBegin"), TEXT("SuperSLM.PrefixRelease"),
		TEXT("SuperSLM.Save"), TEXT("SuperSLM.Restore"), TEXT("SuperSLM.PoolAlloc"), TEXT("SuperSLM.PoolFree"),
	};

	// Plan §5.1, "Counters": all 11. SuperSLM/CPU/RetainedResultBytes exists at source but is not a
	// §5.1 name, so it is not graded.
	const TCHAR* const kRs1hCounterNames[] = {
		TEXT("SuperSLM/CPU/HitchCount"), TEXT("SuperSLM/CPU/TokensFinishedTotal"), TEXT("SuperSLM/CPU/LayersPerJob"),
		TEXT("SuperSLM/CPU/K"), TEXT("SuperSLM/CPU/DueJobs"), TEXT("SuperSLM/CPU/DeliveredJobs"),
		TEXT("SuperSLM/CPU/HostFinishMs"), TEXT("SuperSLM/CPU/PrefillMsPerToken"), TEXT("SuperSLM/CPU/TokensPerSecond"),
		TEXT("SuperSLM/CPU/PoolOccupied"), TEXT("SuperSLM/CPU/PoolFree"),
	};

	// The test's own counter probe. UNCHECKED, so every set is emitted (a TRACE_COUNTER_SET on an
	// ordinary counter elides an unchanged value, CountersTrace.h TCounter::Set).
	TRACE_DECLARE_UNCHECKED_INT_COUNTER(SuperSLMRs1hProbeCounter, TEXT("R-S1h/Probe/Counter"));

	// Emitted inside every capture, on the game thread, before the workload. Cpu probe: CPU channel
	// only, so it reads kRs1hProbeCount in both captures and proves the reader sees timers. Channel
	// probe: SuperSLMChannel, so it reads kRs1hProbeCount with the channel on and 0 with it off --
	// the capture's own proof of which state it was taken in. Counter probe: likewise for the
	// Counters channel.
	void EmitRs1hProbes()
	{
		for (int32 I = 0; I < kRs1hProbeCount; ++I)
		{
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("R-S1h.Probe.Cpu");
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("R-S1h.Probe.SuperSLMChannel", SuperSLMChannel);
			}
			TRACE_COUNTER_SET(SuperSLMRs1hProbeCounter, static_cast<int64>(I + 1));
		}
	}

	FSuperSLMRuntimeConfig Rs1hConfig()
	{
		// Identical to RunEightSequenceSharedPrefixShape's own Config -- used for the final
		// re-Configure only.
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = kRs1hBlockCount;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = kRs1hBlockCount;
		Config.PrefixBlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = kTickBudgetMs;
		Config.PromptTokenCostMs = kRs1hPromptTokenCostMs;
		Config.PromptTokenCostPerPositionMs = 0.0;
		return Config;
	}

	struct FRs1hLedgerTally
	{
		int32 Posted = 0;
		int32 Delivered = 0;
		int64 PromptTokensPosted = 0;
		int64 PromptTokensDelivered = 0;
	};

	// Trace-independent facts about the captured workload, read from the product's own diagnostics
	// (ledger, tick history, accessors) after it drains and before the final re-Configure. Every
	// floor, ceiling and expected counter value the grader uses comes from here or from the
	// construction -- never from the capture being graded.
	struct FRs1hWorkloadFacts
	{
		int32 TickCount = 0;
		TArray<FSuperSLMWorkerJobReport> Ledger;
		int64 TokensGenerated = 0;
		int32 ConfigureCalls = 0;
		int32 TearDownsWithState = 0;
		int32 LiveHitchCount = 0;
		double LiveLastFinishMs = 0.0;
		double LiveLastPrefillMsPerToken = 0.0;
		double LiveTokensPerSecond = 0.0;
		int32 LivePoolOccupied = 0;
		int32 LivePoolFree = 0;
		int32 LastTickJobsPlanned = 0;
		int32 LastTickJobsDelivered = 0;
		TSet<uint32> WorkerOsThreadIds;
		TMap<FString, FRs1hLedgerTally> ByScope;
		int32 Unclassified = 0;
		int32 DeliveredTotal = 0;
	};

	// The scope one ledger row's Execute opens, read at source: every lifecycle and prefix-admin
	// closure opens its own named scope; a sequence prefill opens SuperSLM.Prefill (one member), a
	// prefix prefill SuperSLM.PrefixPrefill (no member); a decode opens SuperSLM.DecodeLayers when it
	// runs any layer and SuperSLM.Finish when it runs only the finish (bAllFinishOnly). Anything else
	// is unclassified and fails the construction check rather than being guessed.
	const TCHAR* ScopeForJob(const FSuperSLMWorkerJobReport& Job)
	{
		switch (Job.Kind)
		{
			case ESuperSLMWorkerJobKind::Reset:         return TEXT("SuperSLM.Reset");
			case ESuperSLMWorkerJobKind::Adopt:         return TEXT("SuperSLM.Adopt");
			case ESuperSLMWorkerJobKind::Save:          return TEXT("SuperSLM.Save");
			case ESuperSLMWorkerJobKind::Restore:       return TEXT("SuperSLM.Restore");
			case ESuperSLMWorkerJobKind::PrefixBegin:   return TEXT("SuperSLM.PrefixBegin");
			case ESuperSLMWorkerJobKind::PrefixRelease: return TEXT("SuperSLM.PrefixRelease");
			case ESuperSLMWorkerJobKind::DecodeOrPrefill:
				if (Job.PromptTokens > 0)
				{
					return Job.MemberSequences.Num() > 0 ? TEXT("SuperSLM.Prefill") : TEXT("SuperSLM.PrefixPrefill");
				}
				if (Job.DecodeLayers > 0)
				{
					return TEXT("SuperSLM.DecodeLayers");
				}
				if (Job.TokenFinishes > 0)
				{
					return TEXT("SuperSLM.Finish");
				}
				return nullptr;
		}
		return nullptr;
	}

	void ClassifyLedger(FRs1hWorkloadFacts& Facts)
	{
		for (const FSuperSLMWorkerJobReport& Job : Facts.Ledger)
		{
			const bool bDelivered = Job.DeliveredAtTick >= 0;
			Facts.DeliveredTotal += bDelivered ? 1 : 0;
			if (bDelivered)
			{
				Facts.WorkerOsThreadIds.Add(Job.WorkerThreadId);
			}
			const TCHAR* Scope = ScopeForJob(Job);
			if (Scope == nullptr)
			{
				++Facts.Unclassified;
				continue;
			}
			FRs1hLedgerTally& Tally = Facts.ByScope.FindOrAdd(Scope);
			++Tally.Posted;
			Tally.Delivered += bDelivered ? 1 : 0;
			Tally.PromptTokensPosted += Job.PromptTokens;
			Tally.PromptTokensDelivered += bDelivered ? Job.PromptTokens : 0;
		}
	}

	int32 CountDelivered(const USuperSLMSubsystem& Subsystem, ESuperSLMWorkerJobKind Kind)
	{
		int32 N = 0;
		for (const FSuperSLMWorkerJobReport& Job : Subsystem.GetJobLedger())
		{
			N += (Job.Kind == Kind && Job.DeliveredAtTick >= 0) ? 1 : 0;
		}
		return N;
	}

	// The captured workload: R-S1b's own 8-sequence shape (plan §5.1's verification text, §9 R-S1h),
	// then a tail that exercises the §5.1 names that shape never reaches -- Save, Restore, a caller's
	// Reset, PrefixRelease, a finish-only decode, PoolFree -- each exactly by construction. R-S1h's
	// row says "every scope ... §5.1 names ... appears at least once" on R-S1b's run; R-S1b's run
	// alone cannot satisfy that, so the tail realizes the row's claim the way R-S2h's own workload
	// added a save/restore pair. Recorded as a reading in the L4 record, open for the plan.
	bool RunRs1hWorkload(FAutomationTestBase& T, USuperSLMSubsystem& Subsystem, USuperSLMModel& Model, FRs1hWorkloadFacts& Facts, FString& OutError)
	{
		// (1) R-S1b's shape: Configure (PoolAlloc), PrefixBegin, PrefixPrefill, 8 Adopt, 8 Prefill,
		// the decodes. FastAsPossible: this is a presence/count claim, not a timing one.
		FEightSequenceRunResult Run;
		if (!RunEightSequenceSharedPrefixShape(T, Subsystem, Model, kTickBudgetMs, Run, OutError,
				EL2S1DrainPacing::FastAsPossible, kRs1hPromptTokenCostMs))
		{
			return false;
		}
		Facts.ConfigureCalls = 1;
		for (const FSuperSLMSequence& Seq : Run.Sequences)
		{
			Facts.TokensGenerated += Subsystem.GetGeneratedTokens(Seq).Num();
		}

		constexpr float Step = 1.0f / 60.0f;
		constexpr EL2S1DrainPacing Pacing = EL2S1DrainPacing::FastAsPossible;
		FString Error;

		// (2) Save one of the eight (SuperSLM.Save). GetSaveResult moves the blob out on its first
		// Success and reads Consumed afterwards, so it is polled only until it resolves.
		FSuperSLMLifecycleOpHandle SaveHandle;
		if (Subsystem.SaveSequence(Run.Sequences[0], SaveHandle, Error) != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("tail: SaveSequence refused: %s"), *Error);
			return false;
		}
		TArray<uint8> Blob;
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		DrainTicks(Subsystem, Step, Pacing, [&Subsystem, &SaveHandle, &Blob, &SaveResult]()
		{
			if (SaveResult == ESuperSLMRestoreResult::Pending)
			{
				SaveResult = Subsystem.GetSaveResult(SaveHandle, Blob);
			}
			return SaveResult != ESuperSLMRestoreResult::Pending;
		}, 30.0);
		if (SaveResult != ESuperSLMRestoreResult::Success || Blob.Num() == 0)
		{
			OutError = FString::Printf(TEXT("tail: save did not deliver a blob (result %d, %d bytes)"), (int32)SaveResult, Blob.Num());
			return false;
		}

		// (3) Return the eight (each queues a recycle, a Reset job on the worker) and release the
		// shared prefix (SuperSLM.PrefixRelease); wait for all eight recycles and the release.
		const int32 RecycledCount = Run.Sequences.Num();
		for (const FSuperSLMSequence& Seq : Run.Sequences)
		{
			Subsystem.ReturnSequence(Seq);
		}
		if (!Subsystem.ReleasePrefix(Run.Prefix, Error))
		{
			OutError = FString::Printf(TEXT("tail: ReleasePrefix refused: %s"), *Error);
			return false;
		}
		DrainTicks(Subsystem, Step, Pacing, [&Subsystem, RecycledCount]()
		{
			return CountDelivered(Subsystem, ESuperSLMWorkerJobKind::Reset) >= RecycledCount
				&& CountDelivered(Subsystem, ESuperSLMWorkerJobKind::PrefixRelease) >= 1;
		}, 30.0);
		if (CountDelivered(Subsystem, ESuperSLMWorkerJobKind::Reset) < RecycledCount
			|| CountDelivered(Subsystem, ESuperSLMWorkerJobKind::PrefixRelease) < 1)
		{
			OutError = TEXT("tail: the eight recycles and the prefix release did not all deliver within 30 s");
			return false;
		}

		// (4) Restore the saved blob into a fresh vend (SuperSLM.Restore).
		FSuperSLMSequence Restored;
		FSuperSLMLifecycleOpHandle RestoreHandle;
		if (Subsystem.RestoreSequence(MoveTemp(Blob), &Model, Restored, RestoreHandle, Error) != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("tail: RestoreSequence refused: %s"), *Error);
			return false;
		}
		DrainTicks(Subsystem, Step, Pacing, [&Subsystem, &RestoreHandle]()
		{
			return Subsystem.GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Pending;
		}, 30.0);
		if (Subsystem.GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("tail: restore did not drain to Success (%d)"), (int32)Subsystem.GetLifecycleOpResult(RestoreHandle));
			return false;
		}

		// (5) A caller's own reset of the restored sequence (SuperSLM.Reset from ResetSequence, not
		// only from recycles), then return it.
		FSuperSLMLifecycleOpHandle ResetHandle;
		if (Subsystem.ResetSequence(Restored, ResetHandle, Error) != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("tail: ResetSequence refused: %s"), *Error);
			return false;
		}
		DrainTicks(Subsystem, Step, Pacing, [&Subsystem, &ResetHandle]()
		{
			return Subsystem.GetLifecycleOpResult(ResetHandle) != ESuperSLMRestoreResult::Pending;
		}, 30.0);
		if (Subsystem.GetLifecycleOpResult(ResetHandle) != ESuperSLMRestoreResult::Success)
		{
			OutError = TEXT("tail: the caller's reset did not drain to Success");
			return false;
		}
		Subsystem.ReturnSequence(Restored);

		// (6) One sequence generating alone. A completed prefill leaves bReadyForLogits set ("the
		// prompt's last token already ran every layer", SuperSLMSubsystem.cpp), so its first decode
		// runs no layer and only the finish; with no other sequence generating, that job's batch is
		// finish-only and opens SuperSLM.Finish. The eight-sequence run does not guarantee one.
		FSuperSLMSequence Lone;
		if (Subsystem.VendSequence(Lone) != ESuperSLMVendResult::Success)
		{
			OutError = TEXT("tail: could not vend the lone sequence");
			return false;
		}
		FSuperSLMGenerationRequest LoneRequest;
		if (!Subsystem.Tokenize(TEXT("Say hello."), LoneRequest.PromptTokens) || LoneRequest.PromptTokens.Num() == 0)
		{
			OutError = TEXT("tail: Tokenize failed for the lone prompt");
			return false;
		}
		LoneRequest.MaxNewTokens = 2;
		TArray<int32> LoneTokens;
		if (!RunGenerationToCompletion(Subsystem, Lone, LoneRequest, LoneTokens, /*MaxWallClockSeconds*/ 60.0, Error))
		{
			OutError = FString::Printf(TEXT("tail: lone generation did not complete: %s"), *Error);
			return false;
		}
		Facts.TokensGenerated += LoneTokens.Num();
		Subsystem.ReturnSequence(Lone);

		// (7) Settle: every posted job delivered and the ledger unchanged across 3 real ticks. The
		// tick count gates the stability count, since DrainTicks evaluates Done twice per tick.
		int32 LastTicks = -1, LastLedgerNum = -1, StableTicks = 0;
		auto Settled = [&Subsystem, &LastTicks, &LastLedgerNum, &StableTicks]()
		{
			const int32 Ticks = Subsystem.GetTickHistory().Num();
			if (Ticks != LastTicks)
			{
				LastTicks = Ticks;
				const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem.GetJobLedger();
				bool bAllDelivered = true;
				for (const FSuperSLMWorkerJobReport& Job : Ledger)
				{
					bAllDelivered &= Job.DeliveredAtTick >= 0;
				}
				StableTicks = (bAllDelivered && Ledger.Num() == LastLedgerNum) ? StableTicks + 1 : 0;
				LastLedgerNum = Ledger.Num();
			}
			return StableTicks >= 3;
		};
		DrainTicks(Subsystem, Step, Pacing, Settled, 30.0);

		// (8) Snapshot, after the last Tick() of the capture: the last PublishStats set every
		// counter from exactly these values.
		Facts.Ledger = Subsystem.GetJobLedger();
		Facts.TickCount = Subsystem.GetTickHistory().Num();
		Facts.LiveHitchCount = Subsystem.GetHitchCount();
		Facts.LiveLastFinishMs = Subsystem.GetLastFinishMs();
		Facts.LiveLastPrefillMsPerToken = Subsystem.GetLastPrefillMsPerToken();
		Facts.LiveTokensPerSecond = Subsystem.GetTokensPerSecond();
		Facts.LivePoolOccupied = Subsystem.GetPoolOccupiedCount();
		Facts.LivePoolFree = Subsystem.GetPoolFreeCount();
		Facts.LastTickJobsPlanned = Subsystem.GetLastTickReport().JobsPlannedThisTick;
		Facts.LastTickJobsDelivered = Subsystem.GetLastTickReport().JobsDeliveredThisTick;
		ClassifyLedger(Facts);

		// (9) Re-Configure: TearDown with live state opens SuperSLM.PoolFree (the only PoolFree site
		// a test can reach inside a capture) and Configure opens a second SuperSLM.PoolAlloc. It also
		// publishes a fresh, zeroed tick report (SuperSLMSubsystem.cpp:1738), which sets every counter
		// to 0 after the snapshot above -- the box run of 2026-09-25 read five counters as 0.0 for
		// that reason. The bookmark marks the point the counter reads are taken at: the grader reads
		// each counter's last sample at or before it, so the counters are read before the
		// re-Configure while PoolFree stays inside the capture.
		TRACE_BOOKMARK(TEXT("R-S1h/BeforeFinalReConfigure")); // the literal kRs1hBeforeReconfigureBookmark names (the macro takes a TCHAR array)
		const FSuperSLMConfigureReport Again = Subsystem.Configure(&Model, Rs1hConfig());
		if (Again.Result != ESuperSLMConfigureResult::Success)
		{
			OutError = FString::Printf(TEXT("tail: the final re-Configure failed: %s"), *Again.Message);
			return false;
		}
		Facts.ConfigureCalls = 2;
		Facts.TearDownsWithState = 1;
		return true;
	}

	// The construction's own minimums, asserted on the product's diagnostics BEFORE any capture is
	// graded. A floor derived from a workload that did not happen would be a floor of zero; this
	// makes that a loud failure instead of a vacuous pass (D-SLM7519).
	bool CheckRs1hConstruction(FAutomationTestBase& T, const FRs1hWorkloadFacts& F)
	{
		auto Delivered = [&F](const TCHAR* Scope) { return F.ByScope.FindRef(Scope).Delivered; };
		bool bOk = true;
		bOk &= T.TestTrue(*FString::Printf(TEXT("[construction] tick history non-empty (%d)"), F.TickCount), F.TickCount > 0);
		bOk &= T.TestEqual(TEXT("[construction] every ledger row classifies to one §5.1 scope"), F.Unclassified, 0);
		bOk &= T.TestEqual(TEXT("[construction] every posted job delivered before the snapshot"), F.DeliveredTotal, F.Ledger.Num());
		bOk &= T.TestEqual(TEXT("[construction] exactly 8 Adopt jobs (one per sequence)"), Delivered(TEXT("SuperSLM.Adopt")), kRs1hBlockCount);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 PrefixBegin job"), Delivered(TEXT("SuperSLM.PrefixBegin")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 PrefixPrefill job"), Delivered(TEXT("SuperSLM.PrefixPrefill")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 PrefixRelease job"), Delivered(TEXT("SuperSLM.PrefixRelease")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 9 sequence Prefill jobs (8 continuations + the lone prompt)"), Delivered(TEXT("SuperSLM.Prefill")) >= kRs1hBlockCount + 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 decode job running layers"), Delivered(TEXT("SuperSLM.DecodeLayers")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 finish-only decode job (the lone sequence's first token)"), Delivered(TEXT("SuperSLM.Finish")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 9 Reset jobs (8 recycles + the caller's reset)"), Delivered(TEXT("SuperSLM.Reset")) >= kRs1hBlockCount + 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 Save job"), Delivered(TEXT("SuperSLM.Save")) >= 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 Restore job"), Delivered(TEXT("SuperSLM.Restore")) >= 1);
		bOk &= T.TestTrue(*FString::Printf(TEXT("[construction] >= 9 tokens generated (%lld)"), F.TokensGenerated), F.TokensGenerated >= kRs1hBlockCount + 1);
		bOk &= T.TestTrue(TEXT("[construction] >= 1 worker thread ran a delivered job"), F.WorkerOsThreadIds.Num() >= 1);
		{
			// PrefillChunk's discrimination (box session 2026-09-25: 84 tokens over 84 calls): the
			// delivered prefill calls must carry more prompt tokens than there are calls, or a
			// per-token count and a per-call count coincide and the PrefillChunk per-call bracket
			// cannot fail on a per-token regression (plan §5.1, "L4 R-S1h rulings" item 1). A
			// construction minimum, not a graded verdict, so the channel-off control's
			// "every verdict fails" is not contradicted by a fact about the workload.
			const FRs1hLedgerTally Seq = F.ByScope.FindRef(TEXT("SuperSLM.Prefill"));
			const FRs1hLedgerTally Pre = F.ByScope.FindRef(TEXT("SuperSLM.PrefixPrefill"));
			const int64 Calls = Seq.Delivered + Pre.Delivered;
			const int64 Tokens = Seq.PromptTokensDelivered + Pre.PromptTokensDelivered;
			bOk &= T.TestTrue(*FString::Printf(TEXT("[construction] delivered prefill calls carry more prompt tokens (%lld) than calls (%lld)"), Tokens, Calls),
				Tokens > Calls);
		}
		return bOk;
	}

	// Captures the workload with the plugin's channels on or off. "Off" is SuperSLMChannel (every
	// §5.1 scope) AND UE's Counters channel: plan §5.1 and CountersTrace.h both put TRACE_COUNTER_SET
	// on the Counters channel, not on SuperSLMChannel, so turning SuperSLMChannel off alone leaves
	// every counter on.
	bool CaptureRs1hWorkload(FAutomationTestBase& T, USuperSLMSubsystem& Subsystem, USuperSLMModel& Model, bool bChannelsOn, const FString& CapturePath, FRs1hWorkloadFacts& Facts)
	{
		// Round 5 (review N5, record §10): each channel this cell toggles is put back to the state
		// it had before the cell, so an editor launched with -trace=cpu,memalloc keeps both.
		struct FPriorChannel { const TCHAR* Name; bool bWasEnabled; };
		TArray<FPriorChannel> Prior;
		for (const TCHAR* Name : { TEXT("Cpu"), TEXT("MemAlloc"), TEXT("SuperSLM"), TEXT("Counters") })
		{
			UE::Trace::FChannel* Channel = UE::Trace::FindChannel(Name);
			Prior.Add({ Name, Channel != nullptr && Channel->IsEnabled() });
		}
		UE::Trace::ToggleChannel(TEXT("Cpu"), true);
		// TraceServices' memory analyzer rejects the engine's allocation-tag stream in this capture
		// (the maintainer's isolated U1 run); this cell reads no allocation events.
		UE::Trace::ToggleChannel(TEXT("MemAlloc"), false);
		UE::Trace::ToggleChannel(TEXT("SuperSLM"), bChannelsOn);
		UE::Trace::ToggleChannel(TEXT("Counters"), bChannelsOn);

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CapturePath), /*Tree*/ true);
		IFileManager::Get().Delete(*CapturePath, /*RequireExists*/ false, /*EvenIfReadOnly*/ true);

		// Capture recipe unchanged from the round-10 cell: an ABSOLUTE path (FTraceAuxiliary resolves
		// a relative one against ProfilingDir), truncate, exclude the tail, then poll IsTracing()
		// after Stop(), which only flags the close.
		FTraceAuxiliary::FOptions TraceOptions;
		TraceOptions.bTruncateFile = true;
		TraceOptions.bExcludeTail = true;
		const bool bStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *CapturePath,
			bChannelsOn ? TEXT("cpu,Counters,SuperSLM,bookmark") : TEXT("cpu,bookmark"), &TraceOptions);
		auto RestoreChannels = [&Prior]()
		{
			for (const FPriorChannel& P : Prior)
			{
				UE::Trace::ToggleChannel(P.Name, P.bWasEnabled);
			}
		};
		if (!T.TestTrue(*FString::Printf(TEXT("FTraceAuxiliary::Start() must succeed against '%s'"), *CapturePath), bStarted))
		{
			RestoreChannels();
			return false;
		}

		EmitRs1hProbes();
		FString RunError;
		const bool bRunOk = RunRs1hWorkload(T, Subsystem, Model, Facts, RunError);
		TRACE_CPUPROFILER_EVENT_FLUSH();

		FTraceAuxiliary::Stop();
		const double DeadlineSeconds = FPlatformTime::Seconds() + 5.0;
		while (UE::Trace::IsTracing() && FPlatformTime::Seconds() < DeadlineSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		RestoreChannels();
		if (!T.TestFalse(TEXT("the trace worker must finish closing the capture before it is read"), UE::Trace::IsTracing()))
		{
			return false;
		}
		if (!T.TestTrue(*FString::Printf(TEXT("the workload must run to completion inside the capture (%s)"), *RunError), bRunOk))
		{
			return false;
		}
		T.AddInfo(FString::Printf(TEXT("R-S1h capture '%s' (%s): %lld bytes; %d ticks, %d jobs, %lld tokens"),
			*CapturePath, bChannelsOn ? TEXT("channels on") : TEXT("SuperSLM + Counters off"),
			IFileManager::Get().FileSize(*CapturePath), Facts.TickCount, Facts.Ledger.Num(), Facts.TokensGenerated));
		return true;
	}

	struct FRs1hScopeEvent
	{
		double Start = 0.0;
		double End = 0.0;
		uint32 ThreadTraceId = 0;
	};

	// One capture, read entirely through TraceServices.
	struct FRs1hTraceReading
	{
		bool bAnswered = false;
		FString WhyNot;
		double DurationSeconds = 0.0;
		uint32 TimerCount = 0;
		int32 ThreadsWithTimelines = 0;
		TMap<FString, uint64> AggregatedInstances;          // exact name -> summed CreateAggregation InstanceCount
		TMap<FString, TArray<FRs1hScopeEvent>> Events;      // exact name -> every timeline event
		TArray<FRs1hScopeEvent> PluginFinishTaskEvents;     // FinishTask directly inside the ParallelFor executor
		int32 PrefillChunkInPrefillScope = 0;               // PrefillChunk directly inside Prefill or PrefixPrefill
		TMap<uint32, FString> ThreadNames;                  // trace thread id -> name
		TMap<FString, TArray<double>> CounterSamples;       // exact name -> every sample, in time order
		TMap<FString, int32> BookmarkCounts;                // text prefix -> count (reported, not graded)
		TMap<FString, TArray<double>> CounterSampleTimes;   // parallel to CounterSamples
		double BeforeReconfigureTime = -1.0;                // kRs1hBeforeReconfigureBookmark's time; -1 when absent
	};

	FRs1hTraceReading ReadRs1hCapture(const FString& AbsolutePath)
	{
		using namespace TraceServices;
		FRs1hTraceReading R;

		TSet<FString> TrackedScopes;
		for (const TCHAR* Name : kRs1hScopeNames) { TrackedScopes.Add(Name); }
		TrackedScopes.Add(kFinishParallelForDebugName);
		TrackedScopes.Add(kProbeCpuScope);
		TrackedScopes.Add(kProbeChannelScope);
		TSet<FString> TrackedCounters;
		for (const TCHAR* Name : kRs1hCounterNames) { TrackedCounters.Add(Name); }
		TrackedCounters.Add(kProbeCounter);

		ITraceServicesModule& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
		TSharedPtr<IAnalysisService> Service = Module.GetAnalysisService();
		if (!Service.IsValid()) { R.WhyNot = TEXT("TraceServices::IAnalysisService is unavailable"); return R; }
		TSharedPtr<const IAnalysisSession> Session = Service->StartAnalysis(*AbsolutePath);
		if (!Session.IsValid()) { R.WhyNot = FString::Printf(TEXT("StartAnalysis() returned no session for '%s'"), *AbsolutePath); return R; }
		const double Deadline = FPlatformTime::Seconds() + 60.0;
		while (!Session->IsAnalysisComplete() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.05f);
		}
		if (!Session->IsAnalysisComplete()) { R.WhyNot = TEXT("analysis did not complete within 60 s"); return R; }

		FAnalysisSessionReadScope ReadScope(*Session);
		R.DurationSeconds = Session->GetDurationSeconds();
		if (!(R.DurationSeconds > 0.0)) { R.WhyNot = FString::Printf(TEXT("session duration is %f s"), R.DurationSeconds); return R; }
		const ITimingProfilerProvider* Timing = ReadTimingProfilerProvider(*Session);
		if (Timing == nullptr) { R.WhyNot = TEXT("no timing profiler provider"); return R; }
		const IThreadProvider* Threads = Session->ReadProvider<IThreadProvider>(GetThreadProviderName());
		if (Threads == nullptr) { R.WhyNot = TEXT("no thread provider"); return R; }
		const ICounterProvider* Counters = Session->ReadProvider<ICounterProvider>(GetCounterProviderName());
		if (Counters == nullptr) { R.WhyNot = TEXT("no counter provider"); return R; }

		// Timer ids carrying a tracked exact name (several ids may share one name).
		TMap<uint32, FString> TrackedTimerNames;
		Timing->ReadTimers([&R, &TrackedScopes, &TrackedTimerNames](const ITimingProfilerTimerReader& Timers)
		{
			R.TimerCount = Timers.GetTimerCount();
			for (uint32 Id = 0; Id < R.TimerCount; ++Id)
			{
				const FTimingProfilerTimer* Timer = Timers.GetTimer(Id);
				if (Timer && Timer->Name)
				{
					const FString Name(Timer->Name);
					if (TrackedScopes.Contains(Name))
					{
						TrackedTimerNames.Add(Id, Name);
					}
				}
			}
		});
		if (R.TimerCount == 0) { R.WhyNot = TEXT("the timer table is empty"); return R; }

		// Instance counts as Insights' Timers panel reports them (R-S2h's pattern), summed per name.
		{
			FCreateAggregationParams Params;
			Params.IntervalStart = 0.0;
			Params.IntervalEnd = R.DurationSeconds;
			Params.CpuThreadFilter = [](uint32) { return true; };
			TUniquePtr<ITable<FTimingProfilerAggregatedStats>> Aggregation(Timing->CreateAggregation(Params));
			if (!Aggregation.IsValid()) { R.WhyNot = TEXT("CreateAggregation returned no table"); return R; }
			TUniquePtr<ITableReader<FTimingProfilerAggregatedStats>> Reader(Aggregation->CreateReader());
			for (; Reader.IsValid() && Reader->IsValid(); Reader->NextRow())
			{
				const FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
				if (Row && Row->Timer && Row->Timer->Name)
				{
					const FString Name(Row->Timer->Name);
					if (TrackedScopes.Contains(Name))
					{
						R.AggregatedInstances.FindOrAdd(Name) += Row->InstanceCount;
					}
				}
			}
		}

		// Every CPU thread's timeline, depth-first: thread placement, timestamps and direct parents.
		const FString FinishTaskName(TEXT("SuperSLM.FinishTask"));
		const FString ExecutorName(kFinishParallelForDebugName);
		const FString PrefillChunkName(TEXT("SuperSLM.PrefillChunk"));
		const FString PrefillName(TEXT("SuperSLM.Prefill"));
		const FString PrefixPrefillName(TEXT("SuperSLM.PrefixPrefill"));
		Threads->EnumerateThreads([&](const FThreadInfo& Thread)
		{
			R.ThreadNames.Add(Thread.Id, Thread.Name ? FString(Thread.Name) : FString());
			uint32 TimelineIndex = 0;
			if (!Timing->GetCpuThreadTimelineIndex(Thread.Id, TimelineIndex))
			{
				return;
			}
			++R.ThreadsWithTimelines;
			const uint32 ThreadId = Thread.Id;
			TArray<uint32> Stack;
			Timing->ReadTimeline(TimelineIndex, [&](const ITimingProfilerTimeline& Timeline)
			{
				Timeline.EnumerateEvents(0.0, R.DurationSeconds,
					[&](double Start, double End, uint32 Depth, const FTimingProfilerEvent& Event)
					{
						const int32 D = static_cast<int32>(Depth);
						Stack.SetNum(D + 1, EAllowShrinking::No);
						Stack[D] = Event.TimerIndex;
						if (const FString* Name = TrackedTimerNames.Find(Event.TimerIndex))
						{
							R.Events.FindOrAdd(*Name).Add({ Start, End, ThreadId });
							if (*Name == FinishTaskName && D > 0)
							{
								const FString* Parent = TrackedTimerNames.Find(Stack[D - 1]);
								if (Parent && *Parent == ExecutorName)
								{
									R.PluginFinishTaskEvents.Add({ Start, End, ThreadId });
								}
							}
							else if (*Name == PrefillChunkName && D > 0)
							{
								const FString* Parent = TrackedTimerNames.Find(Stack[D - 1]);
								if (Parent && (*Parent == PrefillName || *Parent == PrefixPrefillName))
								{
									++R.PrefillChunkInPrefillScope;
								}
							}
						}
						return EEventEnumerate::Continue;
					});
			});
		});
		if (R.ThreadsWithTimelines == 0) { R.WhyNot = TEXT("no thread carries a CPU timeline"); return R; }

		// The two read paths must agree before either is trusted.
		for (const FString& Name : TrackedScopes)
		{
			const uint64 Aggregated = R.AggregatedInstances.FindRef(Name);
			const TArray<FRs1hScopeEvent>* Timeline = R.Events.Find(Name);
			const uint64 FromTimelines = Timeline ? static_cast<uint64>(Timeline->Num()) : 0;
			if (Aggregated != FromTimelines)
			{
				R.WhyNot = FString::Printf(TEXT("reader disagrees with itself on '%s': aggregation %llu, timelines %llu"), *Name, Aggregated, FromTimelines);
				return R;
			}
		}

		Counters->EnumerateCounters([&R, &TrackedCounters](uint32 /*CounterId*/, const ICounter& Counter)
		{
			if (Counter.GetName() == nullptr)
			{
				return;
			}
			const FString Name(Counter.GetName());
			if (!TrackedCounters.Contains(Name))
			{
				return;
			}
			TArray<double>& Samples = R.CounterSamples.FindOrAdd(Name);
			TArray<double>& Times = R.CounterSampleTimes.FindOrAdd(Name);
			if (Counter.IsFloatingPoint())
			{
				Counter.EnumerateFloatValues(0.0, R.DurationSeconds + 1.0, /*bIncludeExternalBounds*/ false,
					[&Samples, &Times](double Time, double Value) { Samples.Add(Value); Times.Add(Time); });
			}
			else
			{
				Counter.EnumerateValues(0.0, R.DurationSeconds + 1.0, /*bIncludeExternalBounds*/ false,
					[&Samples, &Times](double Time, int64 Value) { Samples.Add(static_cast<double>(Value)); Times.Add(Time); });
			}
		});

		if (const IBookmarkProvider* Bookmarks = Session->ReadProvider<IBookmarkProvider>(GetBookmarkProviderName()))
		{
			const TCHAR* const Prefixes[] = {
				TEXT("SuperSLM: CPU job late by"), TEXT("SuperSLM: adapter swap"),
				TEXT("SuperSLM: pool refusal"), TEXT("SuperSLM: prefix pool refusal"),
			};
			Bookmarks->EnumerateBookmarks(0.0, R.DurationSeconds + 1.0, [&R, &Prefixes](const FBookmark& Bookmark)
			{
				if (Bookmark.Text == nullptr)
				{
					return;
				}
				if (FCString::Strcmp(Bookmark.Text, kRs1hBeforeReconfigureBookmark) == 0)
				{
					R.BeforeReconfigureTime = Bookmark.Time;
				}
				for (const TCHAR* Prefix : Prefixes)
				{
					if (FCString::Strncmp(Bookmark.Text, Prefix, FCString::Strlen(Prefix)) == 0)
					{
						++R.BookmarkCounts.FindOrAdd(Prefix);
					}
				}
			});
		}

		R.bAnswered = true;
		return R;
	}

	struct FRs1hVerdict
	{
		FString What;
		bool bPass = false;
	};

	// Every scope and counter assertion R-S1h makes, as verdicts rather than test calls, so the
	// channel-on cell requires each TRUE and the channel-off cell requires each FALSE -- the same
	// assertion, run both ways. Every expectation is trace-independent and non-zero by construction
	// (CheckRs1hConstruction), and every verdict requires the reading to be non-empty, so none can
	// hold vacuously on an empty capture.
	TArray<FRs1hVerdict> GradeRs1h(const FRs1hTraceReading& R, const FRs1hWorkloadFacts& F)
	{
		TArray<FRs1hVerdict> V;
		auto Add = [&V](bool bPass, FString What) { V.Add({ MoveTemp(What), bPass }); };
		auto Count = [&R](const TCHAR* Name) { return R.AggregatedInstances.FindRef(Name); };
		auto EventsOf = [&R](const TCHAR* Name) -> const TArray<FRs1hScopeEvent>&
		{
			static const TArray<FRs1hScopeEvent> None;
			const TArray<FRs1hScopeEvent>* Found = R.Events.Find(Name);
			return Found ? *Found : None;
		};

		// --- Scopes: one per game-thread Tick() -- exact, against the tick history.
		for (const TCHAR* Name : { TEXT("SuperSLM.Tick"), TEXT("SuperSLM.Tick.Plan"), TEXT("SuperSLM.Tick.Apply") })
		{
			Add(F.TickCount > 0 && Count(Name) == static_cast<uint64>(F.TickCount),
				FString::Printf(TEXT("scope '%s': %llu instance(s) == %d Tick() calls (GetTickHistory)"), Name, Count(Name), F.TickCount));
		}
		// --- One WorkerJob per executed worker job: floor = delivered ledger rows (TearDown's
		// release-only jobs, if any, would add; there is no ceiling).
		Add(F.DeliveredTotal > 0 && Count(TEXT("SuperSLM.WorkerJob")) >= static_cast<uint64>(F.DeliveredTotal),
			FString::Printf(TEXT("scope 'SuperSLM.WorkerJob': %llu instance(s) >= %d delivered ledger jobs"), Count(TEXT("SuperSLM.WorkerJob")), F.DeliveredTotal));
		// --- One named scope per job of its kind: delivered rows <= count <= posted rows.
		for (const TCHAR* Name : { TEXT("SuperSLM.Prefill"), TEXT("SuperSLM.PrefixPrefill"), TEXT("SuperSLM.DecodeLayers"), TEXT("SuperSLM.Finish"),
			TEXT("SuperSLM.Reset"), TEXT("SuperSLM.Adopt"), TEXT("SuperSLM.PrefixBegin"), TEXT("SuperSLM.PrefixRelease"),
			TEXT("SuperSLM.Save"), TEXT("SuperSLM.Restore") })
		{
			const FRs1hLedgerTally Tally = F.ByScope.FindRef(Name);
			const uint64 N = Count(Name);
			Add(Tally.Delivered > 0 && N >= static_cast<uint64>(Tally.Delivered) && N <= static_cast<uint64>(Tally.Posted),
				FString::Printf(TEXT("scope '%s': %llu instance(s) within [%d delivered, %d posted] ledger jobs"), Name, N, Tally.Delivered, Tally.Posted));
		}
		// --- PrefillChunk: plan §5.1 ("L4 R-S1h rulings" item 1, 2026-09-25) rules one scope per
		// Layer-1 prefill call, nested in SuperSLM.Prefill or SuperSLM.PrefixPrefill. The verdict is
		// the per-job bracket every per-job scope above uses, summed over both parents: delivered
		// prefill calls <= count <= posted prefill calls. The construction minimum (prefill calls
		// carry more prompt tokens than calls, CheckRs1hConstruction) is what makes the ceiling fail
		// on a per-token regression.
		{
			const FRs1hLedgerTally Seq = F.ByScope.FindRef(TEXT("SuperSLM.Prefill"));
			const FRs1hLedgerTally Pre = F.ByScope.FindRef(TEXT("SuperSLM.PrefixPrefill"));
			const int64 Floor = static_cast<int64>(Seq.Delivered) + Pre.Delivered;
			const int64 Ceiling = static_cast<int64>(Seq.Posted) + Pre.Posted;
			const int64 TokensDelivered = Seq.PromptTokensDelivered + Pre.PromptTokensDelivered;
			const uint64 N = Count(TEXT("SuperSLM.PrefillChunk"));
			Add(Floor > 0 && N >= static_cast<uint64>(Floor) && N <= static_cast<uint64>(Ceiling),
				FString::Printf(TEXT("scope 'SuperSLM.PrefillChunk': %llu instance(s), one per prefill call: within [%lld delivered, %lld posted] Prefill + PrefixPrefill ledger jobs (%lld prompt tokens delivered)"),
					N, Floor, Ceiling, TokensDelivered));
			// The reader walks each timeline with its parent stack, so it can see the direct parent.
			Add(N > 0 && static_cast<uint64>(R.PrefillChunkInPrefillScope) == N,
				FString::Printf(TEXT("nesting: %d of %llu 'SuperSLM.PrefillChunk' instance(s) directly inside 'SuperSLM.Prefill' or 'SuperSLM.PrefixPrefill'"),
					R.PrefillChunkInPrefillScope, N));
		}
		// --- PoolAlloc opens in Configure(), PoolFree in TearDown() with live state (read at
		// source): exact, by construction.
		Add(Count(TEXT("SuperSLM.PoolAlloc")) == static_cast<uint64>(F.ConfigureCalls) && F.ConfigureCalls > 0,
			FString::Printf(TEXT("scope 'SuperSLM.PoolAlloc': %llu instance(s) == %d Configure() calls"), Count(TEXT("SuperSLM.PoolAlloc")), F.ConfigureCalls));
		Add(Count(TEXT("SuperSLM.PoolFree")) == static_cast<uint64>(F.TearDownsWithState) && F.TearDownsWithState > 0,
			FString::Printf(TEXT("scope 'SuperSLM.PoolFree': %llu instance(s) == %d TearDown() with live state"), Count(TEXT("SuperSLM.PoolFree")), F.TearDownsWithState));

		// --- Threads. The game thread is the one thread carrying Tick.Plan (the test calls Tick()
		// there); the workers are the threads the product names "SuperSLM CPU Worker %d" (§5.1).
		TSet<uint32> PlanThreads;
		for (const FRs1hScopeEvent& E : EventsOf(TEXT("SuperSLM.Tick.Plan"))) { PlanThreads.Add(E.ThreadTraceId); }
		const uint32 GameThread = PlanThreads.Num() == 1 ? *PlanThreads.CreateConstIterator() : 0;
		auto IsWorkerThread = [&R](uint32 ThreadId)
		{
			const FString* Name = R.ThreadNames.Find(ThreadId);
			return Name && Name->StartsWith(kWorkerThreadNamePrefix, ESearchCase::CaseSensitive);
		};
		{
			bool bAllOnGame = PlanThreads.Num() == 1;
			for (const TCHAR* Name : { TEXT("SuperSLM.Tick"), TEXT("SuperSLM.Tick.Plan"), TEXT("SuperSLM.Tick.Apply") })
			{
				bAllOnGame &= EventsOf(Name).Num() > 0;
				for (const FRs1hScopeEvent& E : EventsOf(Name)) { bAllOnGame &= E.ThreadTraceId == GameThread; }
			}
			bAllOnGame &= !IsWorkerThread(GameThread);
			Add(bAllOnGame, FString::Printf(TEXT("placement: Tick/Tick.Plan/Tick.Apply all on one game thread ('%s'), never a SuperSLM worker (%d thread(s) carry Tick.Plan)"),
				R.ThreadNames.Contains(GameThread) ? *R.ThreadNames[GameThread] : TEXT("<none>"), PlanThreads.Num()));
		}
		{
			const TArray<FRs1hScopeEvent>& Jobs = EventsOf(TEXT("SuperSLM.WorkerJob"));
			TSet<uint32> JobThreads;
			bool bAllOnWorkers = Jobs.Num() > 0;
			for (const FRs1hScopeEvent& E : Jobs)
			{
				JobThreads.Add(E.ThreadTraceId);
				bAllOnWorkers &= IsWorkerThread(E.ThreadTraceId) && E.ThreadTraceId != GameThread;
			}
			Add(bAllOnWorkers && JobThreads.Num() == F.WorkerOsThreadIds.Num(),
				FString::Printf(TEXT("placement: every WorkerJob on a '%s*' thread; %d such thread(s) == %d distinct ledger worker thread(s)"),
					kWorkerThreadNamePrefix, JobThreads.Num(), F.WorkerOsThreadIds.Num()));
		}
		// --- R-S1h's readability clause: Plan/Apply and WorkerJob read against each other on one
		// timeline -- a Plan begins before the first worker job it posted, and an Apply begins after
		// that job ends (the Apply that delivers it).
		{
			double FirstPlan = TNumericLimits<double>::Max();
			for (const FRs1hScopeEvent& E : EventsOf(TEXT("SuperSLM.Tick.Plan"))) { FirstPlan = FMath::Min(FirstPlan, E.Start); }
			const FRs1hScopeEvent* FirstJob = nullptr;
			for (const FRs1hScopeEvent& E : EventsOf(TEXT("SuperSLM.WorkerJob"))) { if (!FirstJob || E.Start < FirstJob->Start) { FirstJob = &E; } }
			bool bApplyAfter = false;
			if (FirstJob)
			{
				for (const FRs1hScopeEvent& E : EventsOf(TEXT("SuperSLM.Tick.Apply"))) { bApplyAfter |= E.Start >= FirstJob->End; }
			}
			Add(FirstJob != nullptr && FirstPlan <= FirstJob->Start && bApplyAfter,
				FString::Printf(TEXT("readability: first Tick.Plan at %.6f s precedes the first WorkerJob at %.6f s, and a Tick.Apply follows its end"),
					FirstPlan == TNumericLimits<double>::Max() ? -1.0 : FirstPlan, FirstJob ? FirstJob->Start : -1.0));
		}
		// --- The U1 task arm: the plugin's FinishTask (inside the ParallelFor executor) on a thread
		// that is neither the game thread nor a SuperSLM worker. Floor 1: Layer 1 chooses how many
		// tasks a finish is split into and no product diagnostic counts them, so no larger floor is
		// derivable; the claim is the placement.
		{
			int32 OnTaskThreads = 0;
			for (const FRs1hScopeEvent& E : R.PluginFinishTaskEvents)
			{
				OnTaskThreads += (E.ThreadTraceId != GameThread && !IsWorkerThread(E.ThreadTraceId)) ? 1 : 0;
			}
			Add(R.PluginFinishTaskEvents.Num() >= 1 && OnTaskThreads >= 1,
				FString::Printf(TEXT("scope 'SuperSLM.FinishTask' (plugin-owned, inside the ParallelFor executor): %d instance(s), %d on engine task threads"),
					R.PluginFinishTaskEvents.Num(), OnTaskThreads));
		}

		// --- Counters: at least one sample in the capture, and the value against a reading taken
		// outside the trace. Zero samples fails: it is not an answer.
		auto Samples = [&R](const TCHAR* Name) -> const TArray<double>&
		{
			static const TArray<double> None;
			const TArray<double>* Found = R.CounterSamples.Find(Name);
			return Found ? *Found : None;
		};
		// The counter reads are taken at the workload's snapshot, before its final re-Configure,
		// which publishes zeros (see kRs1hBeforeReconfigureBookmark). Each counter's value is its
		// last sample at or before that bookmark. No bookmark is not an answer: every LastIs fails.
		auto LastIs = [&Add, &Samples, &R](const TCHAR* Name, double Expected, double Tolerance, const TCHAR* Source)
		{
			const TArray<double>& S = Samples(Name);
			const TArray<double>* Times = R.CounterSampleTimes.Find(Name);
			int32 At = INDEX_NONE;
			if (Times != nullptr && R.BeforeReconfigureTime >= 0.0)
			{
				for (int32 I = 0; I < S.Num() && I < Times->Num(); ++I)
				{
					if ((*Times)[I] <= R.BeforeReconfigureTime)
					{
						At = I;
					}
				}
			}
			Add(At != INDEX_NONE && FMath::IsNearlyEqual(S[At], Expected, Tolerance),
				FString::Printf(TEXT("counter '%s': %d sample(s), last before the final re-Configure %s, expected %.9g within %.3g (%s)"),
					Name, S.Num(), At != INDEX_NONE ? *FString::Printf(TEXT("%.9g"), S[At]) : TEXT("<none>"), Expected, Tolerance, Source));
		};
		// Float counters (TRACE_DECLARE_FLOAT_COUNTER) reach the reader at float32 precision: the
		// product's double is rounded once to the nearest float, an error of at most half a float ULP
		// (<= 2^-24 relative). The tolerance is 4 float ULPs at the expected value's magnitude
		// (4 * 2^-23 * |Expected|, which is >= 4 ULPs anywhere in the binade), floored at FLT_MIN so an
		// expected value in float's denormal range is not held to a zero-width bound. Nothing beyond
		// float32 representation is admitted: a stale sample, a different reading or a unit slip is
		// still orders of magnitude outside it. The int counters above stay at 0.5: an int64 sample is
		// exact. (L4 2026-09-25: 1e-9 was double precision and failed on a correct capture.)
		auto FloatCounterTolerance = [](double Expected) -> double
		{
			constexpr double kFloatEpsilon = 1.0 / 8388608.0; // 2^-23, FLT_EPSILON
			constexpr double kFloatMinNormal = 1.17549435082228750797e-38; // FLT_MIN
			constexpr double kUlps = 4.0;
			return FMath::Max(kUlps * kFloatEpsilon * FMath::Abs(Expected), kFloatMinNormal);
		};
		const FSuperSLMWorkerJobReport* LastJob = F.Ledger.Num() > 0 ? &F.Ledger.Last() : nullptr;
		LastIs(TEXT("SuperSLM/CPU/HitchCount"), F.LiveHitchCount, 0.5, TEXT("GetHitchCount()"));
		LastIs(TEXT("SuperSLM/CPU/TokensFinishedTotal"), static_cast<double>(F.TokensGenerated), 0.5, TEXT("sum of GetGeneratedTokens() over every generation"));
		LastIs(TEXT("SuperSLM/CPU/LayersPerJob"), LastJob ? LastJob->DecodeLayers : -1.0, 0.5, TEXT("the last ledger job's DecodeLayers"));
		LastIs(TEXT("SuperSLM/CPU/K"), LastJob ? LastJob->K : -1.0, 0.5, TEXT("the last ledger job's K"));
		// Plan §5.1 ("L4 R-S1h rulings" item 2, 2026-09-25): per tick -- the jobs the last tick's
		// Plan posted and the results its Apply delivered.
		LastIs(TEXT("SuperSLM/CPU/DueJobs"), F.LastTickJobsPlanned, 0.5, TEXT("the last tick's JobsPlannedThisTick"));
		LastIs(TEXT("SuperSLM/CPU/DeliveredJobs"), F.LastTickJobsDelivered, 0.5, TEXT("the last tick's JobsDeliveredThisTick"));
		LastIs(TEXT("SuperSLM/CPU/HostFinishMs"), F.LiveLastFinishMs, FloatCounterTolerance(F.LiveLastFinishMs), TEXT("GetLastFinishMs()"));
		LastIs(TEXT("SuperSLM/CPU/PrefillMsPerToken"), F.LiveLastPrefillMsPerToken, FloatCounterTolerance(F.LiveLastPrefillMsPerToken), TEXT("GetLastPrefillMsPerToken()"));
		LastIs(TEXT("SuperSLM/CPU/TokensPerSecond"), F.LiveTokensPerSecond, FloatCounterTolerance(F.LiveTokensPerSecond), TEXT("GetTokensPerSecond()"));
		LastIs(TEXT("SuperSLM/CPU/PoolOccupied"), F.LivePoolOccupied, 0.5, TEXT("GetPoolOccupiedCount()"));
		LastIs(TEXT("SuperSLM/CPU/PoolFree"), F.LivePoolFree, 0.5, TEXT("GetPoolFreeCount()"));
		// A zero last tick matches a per-tick and a cumulative reading alike, so each per-tick
		// counter's samples up to the bookmark must also sum to the ledger's jobs posted, and
		// delivered, in the capture. Read at source: every job is posted by PostLedgerJob (the one
		// NextJobId++), reached only through the five Dispatch* calls in PlanNextJobs, each of which
		// adds its NextJobId delta to JobsPlannedThisTick; every delivery is the one
		// ++JobsDeliveredThisTick in Tick()'s Apply loop, which is also the only site that sets a
		// ledger row's DeliveredAtTick, and Lane.Post has one caller, always with a ledger row. So
		// the per-tick sums equal the ledger's totals exactly. Only samples inside a SuperSLM.Tick
		// scope are summed: VendSequence, ReturnSequence, CreatePrefix and ReleasePrefix republish
		// the LAST tick's report through PublishStats(S.LastReport), outside any Tick(), which would
		// count that tick again; Tick()'s own PublishStats runs inside its SuperSLM.Tick scope. The
		// sum fails on a cumulative regression and on a tick that planned or delivered jobs but did
		// not publish.
		// Assumes SuperSLM.Tick scopes are on one thread and never overlap; the placement check
		// above fails if they are not.
		{
			TArray<FRs1hScopeEvent> Ticks = EventsOf(TEXT("SuperSLM.Tick"));
			Ticks.Sort([](const FRs1hScopeEvent& A, const FRs1hScopeEvent& B) { return A.Start < B.Start; });
			auto PerTickSum = [&R, &Samples, &Ticks](const TCHAR* Name, double& OutSum, int32& OutInTick)
			{
				OutSum = 0.0;
				OutInTick = 0;
				const TArray<double>& S = Samples(Name);
				const TArray<double>* Times = R.CounterSampleTimes.Find(Name);
				if (Times == nullptr || R.BeforeReconfigureTime < 0.0)
				{
					return false;
				}
				int32 TickAt = 0;
				for (int32 I = 0; I < S.Num() && I < Times->Num(); ++I)
				{
					const double Time = (*Times)[I];
					if (Time > R.BeforeReconfigureTime)
					{
						break;
					}
					while (TickAt < Ticks.Num() && Ticks[TickAt].End < Time)
					{
						++TickAt;
					}
					if (TickAt < Ticks.Num() && Ticks[TickAt].Start <= Time)
					{
						OutSum += S[I];
						++OutInTick;
					}
				}
				return OutInTick > 0;
			};
			double DueSum = 0.0, DeliveredSum = 0.0;
			int32 DueInTick = 0, DeliveredInTick = 0;
			const bool bDue = PerTickSum(TEXT("SuperSLM/CPU/DueJobs"), DueSum, DueInTick);
			const bool bDelivered = PerTickSum(TEXT("SuperSLM/CPU/DeliveredJobs"), DeliveredSum, DeliveredInTick);
			Add(bDue && F.Ledger.Num() > 0 && FMath::IsNearlyEqual(DueSum, static_cast<double>(F.Ledger.Num()), 0.5),
				FString::Printf(TEXT("counter 'SuperSLM/CPU/DueJobs': %d per-tick sample(s) up to the bookmark (%d ticks) sum to %.9g == %d jobs posted (ledger)"),
					DueInTick, F.TickCount, DueSum, F.Ledger.Num()));
			Add(bDelivered && F.DeliveredTotal > 0 && FMath::IsNearlyEqual(DeliveredSum, static_cast<double>(F.DeliveredTotal), 0.5),
				FString::Printf(TEXT("counter 'SuperSLM/CPU/DeliveredJobs': %d per-tick sample(s) up to the bookmark (%d ticks) sum to %.9g == %d jobs delivered (ledger)"),
					DeliveredInTick, F.TickCount, DeliveredSum, F.DeliveredTotal));
		}
		{
			const TArray<double>& S = Samples(TEXT("SuperSLM/CPU/TokensPerSecond"));
			const double Max = S.Num() > 0 ? FMath::Max(S) : 0.0;
			Add(S.Num() > 0 && Max > 0.0, FString::Printf(TEXT("counter 'SuperSLM/CPU/TokensPerSecond': max sample %f > 0 (%lld tokens were delivered)"), Max, F.TokensGenerated));
		}
		{
			const TArray<double>& S = Samples(TEXT("SuperSLM/CPU/PoolOccupied"));
			const double Max = S.Num() > 0 ? FMath::Max(S) : -1.0;
			Add(S.Num() > 0 && FMath::IsNearlyEqual(Max, static_cast<double>(kRs1hBlockCount), 0.5),
				FString::Printf(TEXT("counter 'SuperSLM/CPU/PoolOccupied': max sample %f == %d (all eight vended at once)"), Max, kRs1hBlockCount));
		}
		{
			const TArray<double>& S = Samples(TEXT("SuperSLM/CPU/PoolFree"));
			const double Min = S.Num() > 0 ? FMath::Min(S) : -1.0;
			Add(S.Num() > 0 && FMath::IsNearlyEqual(Min, 0.0, 0.5),
				FString::Printf(TEXT("counter 'SuperSLM/CPU/PoolFree': min sample %f == 0 (all eight vended at once)"), Min));
		}
		return V;
	}

	void ReportRs1hReading(FAutomationTestBase& T, const FRs1hTraceReading& R, const FRs1hWorkloadFacts& F, const TCHAR* Label)
	{
		T.AddInfo(FString::Printf(TEXT("[%s] session %.3f s, %u timers, %d thread timelines"), Label, R.DurationSeconds, R.TimerCount, R.ThreadsWithTimelines));
		const int32 AllFinishTask = R.Events.Contains(TEXT("SuperSLM.FinishTask")) ? R.Events[TEXT("SuperSLM.FinishTask")].Num() : 0;
		T.AddInfo(FString::Printf(TEXT("[%s] 'SuperSLM.FinishTask' timer: %d event(s), %d plugin-owned; the rest are ParallelFor's own executor scopes under the same debug name (routed)"),
			Label, AllFinishTask, R.PluginFinishTaskEvents.Num()));
		const TArray<double>* Due = R.CounterSamples.Find(TEXT("SuperSLM/CPU/DueJobs"));
		const TArray<double>* Delivered = R.CounterSamples.Find(TEXT("SuperSLM/CPU/DeliveredJobs"));
		T.AddInfo(FString::Printf(TEXT("[%s] DueJobs/DeliveredJobs last samples %s/%s; per-tick values, as plan §5.1 now states (last tick: %d planned, %d delivered)"),
			Label, (Due && Due->Num()) ? *FString::SanitizeFloat(Due->Last()) : TEXT("<none>"),
			(Delivered && Delivered->Num()) ? *FString::SanitizeFloat(Delivered->Last()) : TEXT("<none>"), F.LastTickJobsPlanned, F.LastTickJobsDelivered));
		for (const TPair<FString, int32>& Bookmark : R.BookmarkCounts)
		{
			T.AddInfo(FString::Printf(TEXT("[%s] bookmark '%s...': %d (reported, not graded)"), Label, *Bookmark.Key, Bookmark.Value));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1GameThreadTickCostBoundedTest,
	"SuperSLM.L2S1.AsyncScheduling.GameThreadTickCostBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1GameThreadTickCostBoundedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1g GameThreadTickCostBounded"), Model))
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

	// RealTime pacing (T-2805 round 10): R-S1g's own claim is about the game thread's Plan/Apply
	// cost under a REAL tick cadence -- FastAsPossible's "wait only as long as needed" would let
	// every tick be measured back-to-back with no real frame boundary between them, which is not
	// the shape this bound is stated against (§14.7's own fix category (b)).
	FEightSequenceRunResult Run;
	FString RunError;
	if (!RunEightSequenceSharedPrefixShape(*this, *Subsystem, *Model, kTickBudgetMs, Run, RunError, EL2S1DrainPacing::RealTime))
	{
		AddError(FString::Printf(TEXT("R-S1g fixture setup failed: %s"), *RunError));
		return false;
	}
	for (const FSuperSLMSequence& Seq : Run.Sequences)
	{
		Subsystem->ReturnSequence(Seq);
	}

	const TArray<FSuperSLMTickReport>& History = Subsystem->GetTickHistory();
	if (!TestTrue(TEXT("the tick history must be non-empty after a real run"), History.Num() > 0))
	{
		return false;
	}

	// Provenance of the 1 ms bound, independent of this cell's own reading (a bound
	// must not come from the reading it grades): T-2815 §12.1's diagnosis of the round-3/4 overrun cause measured the synchronous
	// build's non-Layer-1-call tick overhead -- planning, result application, stats -- at
	// "microseconds" on this same box, in an investigation run to find a different defect, not to
	// justify this bound. 1 ms is roughly three orders of magnitude above that reading.
	constexpr double kMaxTickCostMs = 1.0;
	bool bOk = true;
	double WorstDurationMs = 0.0;
	for (const FSuperSLMTickReport& Tick : History)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("tick %d: DurationMs must equal PlanMs + ApplyMs (Plan %.6f ms, Apply %.6f ms, Duration %.6f ms)"),
				Tick.TickIndex, Tick.PlanMs, Tick.ApplyMs, Tick.DurationMs),
			FMath::IsNearlyEqual(Tick.DurationMs, Tick.PlanMs + Tick.ApplyMs, 1e-6));
		bOk &= TestTrue(*FString::Printf(TEXT("tick %d: Plan+Apply must stay under %.3f ms (got %.6f ms: Plan %.6f ms, Apply %.6f ms) -- never a Layer-1 call cost"),
				Tick.TickIndex, kMaxTickCostMs, Tick.DurationMs, Tick.PlanMs, Tick.ApplyMs),
			Tick.DurationMs < kMaxTickCostMs);
		WorstDurationMs = FMath::Max(WorstDurationMs, Tick.DurationMs);
	}
	AddInfo(FString::Printf(TEXT("R-S1g: %d ticks, worst Plan+Apply duration %.6f ms (bound %.3f ms)"),
		History.Num(), WorstDurationMs, kMaxTickCostMs));

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1InsightsMarkupPresentAndReadableTest,
	"SuperSLM.L2S1.AsyncScheduling.InsightsMarkupPresentAndReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// R-S1h (plan §9; §5.1 "Verification"), rebuilt at L4 (2026-09-25) on TraceServices -- see the
// "R-S1h's grading instrument" block in this file's anonymous namespace for the reader, the name
// list, the floors, the unknown case and the FinishTask collision. The capture recipe (absolute
// path, bExcludeTail, IsTracing() poll after Stop) is the round-10 cell's, unchanged. What is new
// is that every §5.1 CPU scope and counter is graded, by exact name, with a real count or value,
// and that the same assertions are run against a channel-off capture by the cell below, which must
// see each of them fail.
bool FSuperSLML2S1InsightsMarkupPresentAndReadableTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1h InsightsMarkupPresentAndReadable"), Model))
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

	const FString CapturePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SuperSLML2S1Scratch"), TEXT("r_s1h_cpu_markup.utrace")));
	FRs1hWorkloadFacts Facts;
	if (!CaptureRs1hWorkload(*this, *Subsystem, *Model, /*bChannelsOn*/ true, CapturePath, Facts))
	{
		return false;
	}
	if (!CheckRs1hConstruction(*this, Facts))
	{
		AddError(TEXT("R-S1h: the workload did not happen as constructed, so no floor derived from it can be trusted; nothing is graded"));
		return false;
	}

	const FRs1hTraceReading Reading = ReadRs1hCapture(CapturePath);
	if (!Reading.bAnswered)
	{
		AddError(FString::Printf(TEXT("R-S1h: the reader could not answer (%s); nothing is graded"), *Reading.WhyNot));
		return false;
	}

	// Must-accept, on this capture: the reader sees this test's own probes at their exact counts.
	// If any is off, this is not a channel-on capture the grader can be trusted on.
	const uint64 CpuProbe = Reading.AggregatedInstances.FindRef(kProbeCpuScope);
	const uint64 ChannelProbe = Reading.AggregatedInstances.FindRef(kProbeChannelScope);
	const int32 CounterProbe = Reading.CounterSamples.Contains(kProbeCounter) ? Reading.CounterSamples[kProbeCounter].Num() : 0;
	bool bCommissioned = true;
	bCommissioned &= TestEqual(TEXT("[must-accept] the reader sees the CPU-channel probe scope at its exact count"), CpuProbe, (uint64)kRs1hProbeCount);
	bCommissioned &= TestEqual(TEXT("[must-accept] SuperSLMChannel was on: the channel probe scope reads its exact count"), ChannelProbe, (uint64)kRs1hProbeCount);
	bCommissioned &= TestEqual(TEXT("[must-accept] the Counters channel was on and counters are read: the probe counter's sample count is exact"), CounterProbe, kRs1hProbeCount);
	if (!bCommissioned)
	{
		return false;
	}

	ReportRs1hReading(*this, Reading, Facts, TEXT("channels on"));
	// The counter verdicts are cut at this bookmark (kRs1hBeforeReconfigureBookmark); without it
	// every counter verdict fails, and this says why.
	bool bOk = TestTrue(*FString::Printf(TEXT("[precondition] the capture carries the '%s' bookmark the counter reads are cut at"),
		kRs1hBeforeReconfigureBookmark), Reading.BeforeReconfigureTime >= 0.0);
	for (const FRs1hVerdict& Verdict : GradeRs1h(Reading, Facts))
	{
		bOk &= TestTrue(*Verdict.What, Verdict.bPass);
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1InsightsMarkupAbsentWhenChannelOffTest,
	"SuperSLM.L2S1.AsyncScheduling.InsightsMarkupAbsentWhenChannelOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// R-S1h's negative control (D-SLM7502, D-SLM7507: "a capture taken with the channel off must fail
// every scope assertion", run rather than argued). The same workload, captured with SuperSLMChannel
// and the Counters channel off, graded by the same GradeRs1h: every verdict must be FALSE. The
// control counts as evidence of refusal only for verdicts the channel-on cell has been shown to
// pass (D-SLM7519) -- the L4 record lists which those are once the machine run is read.
//
// It cannot pass on a dead reader: the reader must answer, and must see the CPU-channel probe
// scope at its exact count in this same capture, before any zero is accepted as a zero. The two
// channel probes must read zero, which is the capture's own proof that it was taken channel-off.
bool FSuperSLML2S1InsightsMarkupAbsentWhenChannelOffTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1h InsightsMarkupAbsentWhenChannelOff"), Model))
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

	const FString CapturePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SuperSLML2S1Scratch"), TEXT("r_s1h_cpu_markup_channel_off.utrace")));
	FRs1hWorkloadFacts Facts;
	if (!CaptureRs1hWorkload(*this, *Subsystem, *Model, /*bChannelsOn*/ false, CapturePath, Facts))
	{
		return false;
	}
	if (!CheckRs1hConstruction(*this, Facts))
	{
		AddError(TEXT("R-S1h control: the workload did not happen as constructed, so a zero reading would prove nothing"));
		return false;
	}

	const FRs1hTraceReading Reading = ReadRs1hCapture(CapturePath);
	if (!Reading.bAnswered)
	{
		AddError(FString::Printf(TEXT("R-S1h control: the reader could not answer (%s); a control on a reader that cannot answer proves nothing"), *Reading.WhyNot));
		return false;
	}

	const uint64 CpuProbe = Reading.AggregatedInstances.FindRef(kProbeCpuScope);
	const uint64 ChannelProbe = Reading.AggregatedInstances.FindRef(kProbeChannelScope);
	const int32 CounterProbe = Reading.CounterSamples.Contains(kProbeCounter) ? Reading.CounterSamples[kProbeCounter].Num() : 0;
	bool bCommissioned = true;
	bCommissioned &= TestEqual(TEXT("[control, reader alive] the reader sees the CPU-channel probe scope at its exact count in this capture"), CpuProbe, (uint64)kRs1hProbeCount);
	bCommissioned &= TestEqual(TEXT("[control] SuperSLMChannel was off: the channel probe scope reads zero"), ChannelProbe, (uint64)0);
	bCommissioned &= TestEqual(TEXT("[control] the Counters channel was off: the probe counter has no sample"), CounterProbe, 0);
	if (!bCommissioned)
	{
		return false;
	}

	ReportRs1hReading(*this, Reading, Facts, TEXT("channels off"));
	bool bOk = true;
	for (const FRs1hVerdict& Verdict : GradeRs1h(Reading, Facts))
	{
		bOk &= TestFalse(*FString::Printf(TEXT("[must fail with channels off] %s"), *Verdict.What), Verdict.bPass);
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuEarlyFinishedResultDeliveredTest,
	"SuperSLM.U1.Cpu.EarlyFinishedResultDelivered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuEarlyFinishedResultDeliveredTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (ImportAEx(*this, TEXT("R-S1b(iv) early finish"), Model) != EAExAvailability::Ready) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.TickBudgetMs = 16.6;
	Config.ResetCostMs = 1000.0; // plan-time K is intentionally much larger than the actual reset
	Config.SequenceLifecycleBudgetMs = 2000.0;
	if (!TestEqual(TEXT("Configure"), (uint8)Cpu->Configure(Model, Config).Result,
			(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	FSuperSLMLifecycleOpHandle ResetHandle;
	FString Error;
	if (!TestEqual(TEXT("reset queues"), (uint8)Cpu->ResetSequence(Seq, ResetHandle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	Cpu->Tick(1.0f / 60.0f); // post the reset job
	if (!TestEqual(TEXT("reset remains pending on its posting tick"),
			(uint8)Cpu->GetLifecycleOpResult(ResetHandle), (uint8)ESuperSLMRestoreResult::Pending)) { return false; }
	FPlatformProcess::Sleep(1.0f); // give the independent worker real time to finish
	Cpu->Tick(1.0f / 60.0f); // the first Apply after the worker's result exists
	const ESuperSLMRestoreResult Result = Cpu->GetLifecycleOpResult(ResetHandle);
	bool bOk = TestEqual(TEXT("the finished reset is delivered at the next Apply"),
		(uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
	const FSuperSLMWorkerJobReport* ResetJob = nullptr;
	const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger(); // a copy: ResetJob points into it
	for (const FSuperSLMWorkerJobReport& Job : Ledger)
	{
		if (Job.Kind == ESuperSLMWorkerJobKind::Reset && Job.LifecycleOpHandle == ResetHandle)
		{
			ResetJob = &Job;
			break;
		}
	}
	if (!TestNotNull(TEXT("the reset's own ledger row"), ResetJob)) { return false; }
	bOk &= TestTrue(TEXT("the configured static cost commits a later tick"), ResetJob->K > 2);
	bOk &= TestTrue(TEXT("this job's Layer-1 reset finished within the worker interval"),
		ResetJob->WorkerCallMs >= 0.0 && ResetJob->WorkerCallMs < 1000.0);
	bOk &= TestTrue(TEXT("delivery when finished precedes the committed tick"),
		ResetJob->DeliveredAtTick >= 0 && ResetJob->DeliveredAtTick < ResetJob->CommittedDeliveryTick);
	Cpu->ReturnSequence(Seq);
	return bOk;
}

// R-S1j (D-SLM7421, D-SLM7429, and maintainer ruling D-SLM7457 dropping Restore from this list --
// see this file's own header note): the row's own fixed-arrival-order scenario. One sequence with
// a decode job already in flight on the worker -- one entry of its own
// MaxQueuedOperationsPerSequence (4) budget -- then SaveSequence(), ResetSequence() and
// AdoptPrefix(), each from a distinct handle, in that fixed arrival order. Save and Reset fill the
// remaining budget alongside the pre-existing in-flight job (3 entries total: in-flight decode,
// Save, Reset); AdoptPrefix is the fourth entry and is ALSO accepted (the bound is 4, and this is
// exactly the 4th); a second decode/prefill request arriving after the bound is reached is refused
// SequenceQueueFull.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1QueueFullFixedArrivalOrderTest,
	"SuperSLM.L2S1.AsyncScheduling.QueueFullFixedArrivalOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1QueueFullFixedArrivalOrderTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1j QueueFullFixedArrivalOrder"), Model))
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
	Config.BlockCount = 2; // the busy sequence, plus one spare for AdoptPrefix's own prefix source
	Config.PrefixBlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	// The default MaxQueuedOperationsPerSequence (4) is exactly the bound this cell exercises --
	// left at its default deliberately, matching the row's own "one entry of its own
	// MaxQueuedOperationsPerSequence = 4 budget" text.
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("Tokenize"), Subsystem->Tokenize(TEXT("Tell me about your first tattoo."), PromptTokens)))
	{
		return false;
	}

	FSuperSLMSequence Busy;
	if (!TestEqual(TEXT("Vend the busy sequence"), (uint8)Subsystem->VendSequence(Busy), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 64; // long enough that this sequence still has a decode job in flight
	                           // when the requests below are all issued
	FString BeginError;
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), Subsystem->BeginGeneration(Busy, Request, BeginError)))
	{
		return false;
	}
	Subsystem->Tick(1.0f / 60.0f); // the decode job is now planned and in flight on the worker

	// A ready prefix for the AdoptPrefix() request below -- created and drained BEFORE this
	// cell's own request sequence, so AdoptPrefix's own outcome is about the QUEUE bound alone,
	// never about the prefix not being Ready yet.
	FSuperSLMPrefix Prefix;
	{
		TArray<int32> PersonaTokens;
		Subsystem->Tokenize(TEXT("You are a patient, encouraging tattoo artist."), PersonaTokens);
		FString PrefixError;
		if (!TestTrue(*FString::Printf(TEXT("CreatePrefix: %s"), *PrefixError), Subsystem->CreatePrefix(PersonaTokens, Prefix, PrefixError)))
		{
			return false;
		}
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Prefix]() { return Subsystem->IsPrefixReady(Prefix); },
			/*MaxWallClockSeconds*/ 30.0);
		if (!TestTrue(TEXT("prefix must become ready"), Subsystem->IsPrefixReady(Prefix)))
		{
			return false;
		}
	}

	if (!TestTrue(TEXT("the busy sequence must still have work in flight before the requests below are issued"),
			Subsystem->GetPhase(Busy) == ESuperSLMSequencePhase::Decoding || Subsystem->GetPhase(Busy) == ESuperSLMSequencePhase::Prefilling))
	{
		return false;
	}

	bool bOk = true;

	// --- Request 1: SaveSequence() -- accepted, queued (fills 1 of the 3 remaining slots
	// alongside the pre-existing in-flight decode job). ---
	FSuperSLMLifecycleOpHandle SaveHandle;
	FString SaveError;
	const ESuperSLMRestoreResult SaveQueueResult = Subsystem->SaveSequence(Busy, SaveHandle, SaveError);
	bOk &= TestEqual(*FString::Printf(TEXT("request 1 (SaveSequence) must be accepted and queued: %s"), *SaveError),
		(uint8)SaveQueueResult, (uint8)ESuperSLMRestoreResult::Success);

	// --- Request 2: ResetSequence() -- accepted, queued. ---
	FSuperSLMLifecycleOpHandle ResetHandle;
	FString ResetError;
	const ESuperSLMRestoreResult ResetQueueResult = Subsystem->ResetSequence(Busy, ResetHandle, ResetError);
	bOk &= TestEqual(*FString::Printf(TEXT("request 2 (ResetSequence) must be accepted and queued: %s"), *ResetError),
		(uint8)ResetQueueResult, (uint8)ESuperSLMRestoreResult::Success);

	// --- Request 3: AdoptPrefix() -- accepted, queued. This is the fourth entry counting the
	// pre-existing in-flight decode job (in-flight + Save + Reset + Adopt = 4), filling the
	// bound exactly. ---
	FSuperSLMLifecycleOpHandle AdoptHandle;
	FString AdoptError;
	const ESuperSLMRestoreResult AdoptQueueResult = Subsystem->AdoptPrefix(Busy, Prefix, AdoptHandle, AdoptError);
	bOk &= TestEqual(*FString::Printf(TEXT("request 3 (AdoptPrefix) must be accepted and queued: %s"), *AdoptError),
		(uint8)AdoptQueueResult, (uint8)ESuperSLMRestoreResult::Success);

	// --- Request 4: a second decode/prefill submission -- the queue is now full (4 entries: the
	// in-flight decode plus Save/Reset/Adopt); refused at once, before anything is queued and
	// before Layer 1 is asked, never surfacing Layer 1's own SSLM_BUSY (D-SLM7421). ---
	FString SecondBeginError;
	const bool bSecondBeginOk = Subsystem->BeginGeneration(Busy, Request, SecondBeginError);
	bOk &= TestFalse(TEXT("a second decode/prefill request against the still-busy sequence must be refused, never accepted"), bSecondBeginOk);
	bOk &= TestTrue(TEXT("the second BeginGeneration's own refusal must name the full queue"),
		SecondBeginError.Contains(TEXT("queue"), ESearchCase::IgnoreCase) || SecondBeginError.Contains(TEXT("full"), ESearchCase::IgnoreCase));

	// --- Drain everything queued so far and confirm each of the three accepted requests
	// delivers, in arrival order, once the in-flight job and every queued entry ahead of it has
	// drained (§5 item 3). ---
	ESuperSLMRestoreResult SaveOutcome = ESuperSLMRestoreResult::Pending, ResetOutcome = ESuperSLMRestoreResult::Pending, AdoptOutcome = ESuperSLMRestoreResult::Pending;
	TArray<uint8> SavedBlob;
	int32 SaveDrainTick = -1, ResetDrainTick = -1, AdoptDrainTick = -1, Tick = 0;
	auto AllDrained = [&]()
	{
		return SaveOutcome != ESuperSLMRestoreResult::Pending
			&& ResetOutcome != ESuperSLMRestoreResult::Pending
			&& AdoptOutcome != ESuperSLMRestoreResult::Pending;
	};
	// FastAsPossible (T-2805 round 10): a content/order cell -- waits only as long as the worker
	// genuinely needs to drain the in-flight decode plus the three queued lifecycle ops, real
	// wall-clock time given between polls per Fixtures.h's own DrainTicks (§14.7's own fix).
	const double DrainStartSeconds = FPlatformTime::Seconds();
	while (!AllDrained() && FPlatformTime::Seconds() - DrainStartSeconds < 30.0)
	{
		Subsystem->Tick(1.0f / 60.0f);
		++Tick;
		if (SaveOutcome == ESuperSLMRestoreResult::Pending)
		{
			SaveOutcome = Subsystem->GetSaveResult(SaveHandle, SavedBlob);
			if (SaveOutcome != ESuperSLMRestoreResult::Pending) { SaveDrainTick = Tick; }
		}
		if (ResetOutcome == ESuperSLMRestoreResult::Pending)
		{
			ResetOutcome = Subsystem->GetLifecycleOpResult(ResetHandle);
			if (ResetOutcome != ESuperSLMRestoreResult::Pending) { ResetDrainTick = Tick; }
		}
		if (AdoptOutcome == ESuperSLMRestoreResult::Pending)
		{
			AdoptOutcome = Subsystem->GetLifecycleOpResult(AdoptHandle);
			if (AdoptOutcome != ESuperSLMRestoreResult::Pending) { AdoptDrainTick = Tick; }
		}
		if (AllDrained())
		{
			break;
		}
		FPlatformProcess::Sleep(0.001f);
	}

	bOk &= TestEqual(TEXT("the queued Save must drain to Success"), (uint8)SaveOutcome, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the queued Reset must drain to Success"), (uint8)ResetOutcome, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the queued AdoptPrefix must drain to Success"), (uint8)AdoptOutcome, (uint8)ESuperSLMRestoreResult::Success);
	// Strict arrival order: Save queued first, drains no later than Reset; Reset no later than
	// Adopt (§5 item 3: "runs in strict arrival order, one in flight at a time").
	bOk &= TestTrue(*FString::Printf(TEXT("Save must drain no later than Reset (Save tick %d, Reset tick %d)"), SaveDrainTick, ResetDrainTick),
		SaveDrainTick >= 0 && ResetDrainTick >= 0 && SaveDrainTick <= ResetDrainTick);
	bOk &= TestTrue(*FString::Printf(TEXT("Reset must drain no later than Adopt (Reset tick %d, Adopt tick %d)"), ResetDrainTick, AdoptDrainTick),
		AdoptDrainTick >= 0 && ResetDrainTick <= AdoptDrainTick);

	Subsystem->ReturnSequence(Busy);
	Subsystem->ReleasePrefix(Prefix);
	return bOk;
}

// R-S1j's own 200-collision sweep: 40 sequences x 5 requests each, with NO pre-existing in-flight
// job, building each sequence's queue to exactly MaxQueuedOperationsPerSequence (4) before its
// fifth request. The fifth is refused at once with SequenceQueueFull; a retry after the queue
// drains by one succeeds. Uses Save/Reset/AdoptPrefix/a decode submission as the four filling
// requests -- RestoreSequence is not one of them (this file's header note: Restore always vends a
// fresh sequence and cannot collide with a busy one, maintainer ruling D-SLM7457); it is exercised
// in R-S1c instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1QueueFullSweepTest,
	"SuperSLM.L2S1.AsyncScheduling.QueueFullSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1QueueFullSweepTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1j QueueFullSweep"), Model))
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

	constexpr int32 SequenceCount = 40;
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = SequenceCount;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = SequenceCount;
	Config.PrefixBlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = kTickBudgetMs;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("Tokenize"), Subsystem->Tokenize(TEXT("Short reply."), PromptTokens)))
	{
		return false;
	}

	FSuperSLMPrefix Prefix;
	{
		TArray<int32> PersonaTokens;
		Subsystem->Tokenize(TEXT("Brief persona."), PersonaTokens);
		FString PrefixError;
		if (!TestTrue(*FString::Printf(TEXT("CreatePrefix: %s"), *PrefixError), Subsystem->CreatePrefix(PersonaTokens, Prefix, PrefixError)))
		{
			return false;
		}
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Prefix]() { return Subsystem->IsPrefixReady(Prefix); },
			/*MaxWallClockSeconds*/ 30.0);
		if (!TestTrue(TEXT("prefix must become ready"), Subsystem->IsPrefixReady(Prefix)))
		{
			return false;
		}
	}

	bool bOk = true;
	int32 QueueFullRefusalsObserved = 0;
	for (int32 S = 0; S < SequenceCount; ++S)
	{
		FSuperSLMSequence Seq;
		if (!TestEqual(*FString::Printf(TEXT("Vend sequence %d"), S), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}

		// Four requests that queue (no pre-existing in-flight job on a fresh sequence, so these
		// fill the queue to exactly 4): Reset, Save (against Idle -- a valid, real save of an
		// untouched sequence), AdoptPrefix, and a decode/prefill submission.
		FSuperSLMLifecycleOpHandle ResetHandle, SaveHandle, AdoptHandle;
		FString ResetError, SaveError, AdoptError, BeginError;

		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d request 1 (Reset) must queue"), S),
			(uint8)Subsystem->ResetSequence(Seq, ResetHandle, ResetError), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d request 2 (Save) must queue"), S),
			(uint8)Subsystem->SaveSequence(Seq, SaveHandle, SaveError), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d request 3 (AdoptPrefix) must queue"), S),
			(uint8)Subsystem->AdoptPrefix(Seq, Prefix, AdoptHandle, AdoptError), (uint8)ESuperSLMRestoreResult::Success);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = 8;
		const bool bFourthOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
		bOk &= TestTrue(*FString::Printf(TEXT("sequence %d request 4 (decode submission) must queue: %s"), S, *BeginError), bFourthOk);

		// The fifth request against this sequence's own queue, now at depth 4: refused at once,
		// before anything is queued and before Layer 1 is asked, never Layer 1's own busy status.
		FSuperSLMLifecycleOpHandle FifthHandle;
		FString FifthError;
		const ESuperSLMRestoreResult FifthResult = Subsystem->ResetSequence(Seq, FifthHandle, FifthError);
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d's 5th request must be refused SequenceQueueFull: %s"), S, *FifthError),
			(uint8)FifthResult, (uint8)ESuperSLMRestoreResult::SequenceQueueFull);
		bOk &= TestTrue(*FString::Printf(TEXT("sequence %d's 5th request refusal must not have consumed a handle"), S), !FifthHandle.IsValid());
		if (FifthResult == ESuperSLMRestoreResult::SequenceQueueFull)
		{
			++QueueFullRefusalsObserved;
		}

		// Drain one entry (the queued Reset), then retry the fifth request -- it must now succeed,
		// since the queue has room again.
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &ResetHandle]() { return Subsystem->GetLifecycleOpResult(ResetHandle) != ESuperSLMRestoreResult::Pending; },
			/*MaxWallClockSeconds*/ 30.0);
		const ESuperSLMRestoreResult ResetOutcome = Subsystem->GetLifecycleOpResult(ResetHandle);
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d's queued Reset must drain to Success"), S),
			(uint8)ResetOutcome, (uint8)ESuperSLMRestoreResult::Success);

		FSuperSLMLifecycleOpHandle RetryHandle;
		FString RetryError;
		const ESuperSLMRestoreResult RetryResult = Subsystem->ResetSequence(Seq, RetryHandle, RetryError);
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d's retry after the queue drains by one must succeed: %s"), S, *RetryError),
			(uint8)RetryResult, (uint8)ESuperSLMRestoreResult::Success);

		// Drain everything left so this sequence can be returned cleanly.
		DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Subsystem, &Seq]() { return Subsystem->GetPendingLifecycleOperationCount(Seq) == 0; },
			/*MaxWallClockSeconds*/ 30.0);
		Subsystem->ReturnSequence(Seq);
	}

	bOk &= TestEqual(TEXT("every one of the 40 sequences must have shown exactly one SequenceQueueFull refusal on its own 5th request"),
		QueueFullRefusalsObserved, SequenceCount);

	Subsystem->ReleasePrefix(Prefix);
	AddInfo(FString::Printf(TEXT("R-S1j: %d/%d sequences correctly refused their 5th request with SequenceQueueFull; no call ever returned Layer 1's own busy status"),
		QueueFullRefusalsObserved, SequenceCount));

	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
