#include "SuperSLMSubsystem.h"

#include "Core/SuperSLMBuildSlicing.h"
#include "Core/SuperSLMCacheChunkPricing.h"
#include "Core/SuperSLMInSituPrefixPrefill.h"
#include "Core/SuperSLMPageLedgerCore.h"
#include "Core/SuperSLMSchemaJumpCore.h"
#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMPagedTestSeam.h"
#include "SuperSLMPrefillPlanning.h"
#include "SuperSLMRuntimeResolve.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMConfigureTestAccess.h"
#include "SuperSLMSaveBlob.h"
#include "SuperSLMSchemaJumpAbi.h"
#include "SuperSLMSchemaJumpReviewTestAccess.h"
#include "SuperSLMSchemaJumpTestSeam.h"
#include "SuperSLMSchemaJumpTypes.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMStatusMapping.h"
#include "SuperSLMTokenizeTestSeam.h"
#if SUPERSLM_WITH_PREFIX_CACHE
#include "SuperSLMSubsystemPrefixCache.h"
#endif

#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Stats/Stats.h"
#include "Trace/Trace.h"
#include "UObject/StrongObjectPtr.h"

#include "superslm/model.h"
#include "superslm/schema_masks.h"
#include "superslm/sslm_abi.h"

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

// D-SLM7407 (plan §5): the async worker tick, T-2850 fold 8, T-2805 round 9. This file replaces
// the synchronous, game-thread-executing scheduler T-2815 built: Tick() is now Apply-then-Plan bookkeeping only, and every Layer-1 call runs on a
// plugin-owned worker thread, never inline inside an API call from the game thread.

// §5.1: the dedicated trace channel, zero cost when off. The description is ANSI
// (FChannel::InitArgs::Desc is const ANSICHAR*).
UE_TRACE_CHANNEL_DEFINE(SuperSLMChannel, "SuperSLM prefill/decode/lifecycle instrumentation");

// §5.1: a CSV profiler category for the tick's measured duration and hitches.
CSV_DEFINE_CATEGORY(SuperSLM, true);

// §5.1: STATGROUP_SuperSLM (UE's legacy Stat System) -- a mechanism separate from Insights' own
// Trace Counters below. The console command takes the group's name without its STATGROUP_
// prefix, so it is `stat SuperSLM`; the "SuperSLMUnreal" below is only the group's display name.
DECLARE_STATS_GROUP(TEXT("SuperSLMUnreal"), STATGROUP_SuperSLM, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Tick"), STAT_SuperSLMTick, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool Occupied"), STAT_SuperSLMPoolOccupied, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool Free"), STAT_SuperSLMPoolFree, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Workspaces"), STAT_SuperSLMWorkspaces, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Sequences In Flight"), STAT_SuperSLMInFlight, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Live Prefixes"), STAT_SuperSLMPrefixes, STATGROUP_SuperSLM);
DECLARE_DWORD_COUNTER_STAT(TEXT("Hitch Count"), STAT_SuperSLMHitches, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Last Tick ms"), STAT_SuperSLMLastTickMs, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Last Reset ms"), STAT_SuperSLMLastResetMs, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Last Adopt ms"), STAT_SuperSLMLastAdoptMs, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Last Finish ms"), STAT_SuperSLMLastFinishMs, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Prefill ms per Token"), STAT_SuperSLMPrefillMsPerToken, STATGROUP_SuperSLM);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Tokens per Second"), STAT_SuperSLMTokensPerSecond, STATGROUP_SuperSLM);

// §5.1 D-SLM7411: the ten SuperSLM/CPU/* Insights Trace Counters, none of which existed before
// this fold. Each mirrors an existing STAT_SuperSLM* or job-ledger reading rather than computing
// a second figure, so the Stat System, the CSV profiler and Insights agree by construction.
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_HitchCount, TEXT("SuperSLM/CPU/HitchCount"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_TokensFinishedTotal, TEXT("SuperSLM/CPU/TokensFinishedTotal"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_LayersPerJob, TEXT("SuperSLM/CPU/LayersPerJob"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_K, TEXT("SuperSLM/CPU/K"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_DueJobs, TEXT("SuperSLM/CPU/DueJobs"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_DeliveredJobs, TEXT("SuperSLM/CPU/DeliveredJobs"));
TRACE_DECLARE_FLOAT_COUNTER(SuperSLM_CPU_HostFinishMs, TEXT("SuperSLM/CPU/HostFinishMs"));
TRACE_DECLARE_FLOAT_COUNTER(SuperSLM_CPU_PrefillMsPerToken, TEXT("SuperSLM/CPU/PrefillMsPerToken"));
TRACE_DECLARE_FLOAT_COUNTER(SuperSLM_CPU_TokensPerSecond, TEXT("SuperSLM/CPU/TokensPerSecond"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_PoolOccupied, TEXT("SuperSLM/CPU/PoolOccupied"));
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_PoolFree, TEXT("SuperSLM/CPU/PoolFree"));
// Plan §2.5 row 19 (D-SLM7763): save-blob bytes held in handle entries and not yet taken.
TRACE_DECLARE_INT_COUNTER(SuperSLM_CPU_RetainedResultBytes, TEXT("SuperSLM/CPU/RetainedResultBytes"));

namespace
{
	// Handles are never reused, across Configure() calls included, so a stale handle held by a
	// caller can never alias a later vend.
	int64 GNextSequenceId = 1;
	int64 GNextPrefixId = 1;

#if WITH_DEV_AUTOMATION_TESTS
	// Plugin paging plan §3.9: the test seams' hold ids, never reused across Configure() calls, so
	// a hold a test kept from an ended state can never name one of the next state's. (K2 review F7:
	// its only users are the seams.)
	int64 GNextPagedHoldId = 1;
#endif

	// Plugin paging plan §3.9: the request kinds a hold or a Layer-1 status override names, as this
	// file keeps them (ESuperSLMPagedRequestKind, which the seam header declares only under
	// WITH_DEV_AUTOMATION_TESTS, maps onto these one to one).
	enum class EPagedJobKind : uint8
	{
		Reset,
		Return,
		Adopt,
		Restore,
		PrefixBuild,
		NestedBuild,
		PrefixRelease,
		Count
	};

	FString StatusText(sslm_status Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
	}


	// D-SLM7407/D-SLM7421 (plan §5 item 3): the unified per-sequence request queue realizes
	// Reset/Adopt/Save AND a decode/prefill submission (Generate) as the SAME ordered queue, so
	// "one in flight at a time, strict arrival order" is one mechanism rather than four. Restore
	// never targets an existing sequence (it always vends fresh, D-SLM7457), so it is queued the
	// same way on its own freshly-reserved slot rather than needing a distinct kind here.
	enum class ESlotOpKind : uint8
	{
		Reset,
		Adopt,
		Save,
		Restore,
		Generate,
	};

	// Plan §2.5 row 19 (D-SLM7693): a restore's blob is held once, in a shared immutable buffer
	// created when the request is accepted; the queued op, the dispatch-time parse and the worker
	// job all share it, and no copy is taken after acceptance.
	using FSharedBlob = TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>;

	struct FSlotQueuedOp
	{
		ESlotOpKind Kind = ESlotOpKind::Reset;
		int64 Ordinal = 0;         // global request ordinal (D-SLM7428): admission order
		int64 HandleId = 0;        // Reset/Adopt/Save/Restore's own FSuperSLMLifecycleOpHandle::Id

		// Plan §2.5 row 20 rule 2: the holder that issued this op (its FSuperSLMSequence id). A
		// job's delivery writes caller-owned state only while this owner still holds the slot.
		// 0 for the recycle ReleaseUser() appends, which has no owner.
		int64 Owner = 0;

		// Plan §2.5 row 17: the recycle's reset also unbinds Layer 1's schema and adapter on the
		// worker. Set only on the recycle ReleaseUser() appends.
		bool bUnbind = false;

		int64 PrefixId = 0;        // Adopt only
		FSuperSLMGenerationRequest GenRequest; // Generate only
		FSharedBlob RestoreBlob;               // Restore only (row 19: shared, never copied)
		uint8 RestoreExpectedHash[32] = {};    // Restore only
		USuperSLMModel* RestoreExpectedModel = nullptr; // Restore only (kept alive by the caller)

		// Restore only (D-SLM7946): the phase the restore gives its sequence at delivery, read from
		// the blob when the request is accepted (QueueRestore()). The one-generation projection
		// reads it while the op is queued, and from FSlot::InFlightRestorePhase once it is posted.
		ESuperSLMSequencePhase RestoreProjectedPhase = ESuperSLMSequencePhase::Idle;

		// Plugin paging plan §3.2 ("admission at the head of the sequence's queue"): set when the op
		// first reaches the head and is admitted (DispatchSlotLifecycleOp). In budget mode a Reset
		// (the recycle included) or an Adopt takes its page-ledger ticket there, never at queue
		// time; the ticket is applied at the job's delivery. The two hold ids are the test seams'
		// (§3.9), taken at the same instant; 0 is none.
		bool bAdmitted = false;
		SuperSLMPaging::FTicket LedgerTicket = SuperSLMPaging::InvalidTicket;
		int64 StartHoldId = 0;
		int64 ApplyHoldId = 0;

		// Plugin paging plan §3.5 (step K3), budget mode. A restore waiting on region H: admitted
		// to the ledger's wait queue at the head, and not dispatched until the ledger resolves its
		// ticket Admitted (ResolveWaitingRestores). A reset the ledger made a create-first
		// normalization (FAdmission::bNormalizes).
		bool bWaiting = false;
		bool bNormalizes = false;

		// Restore, budget mode: what QueueRestore() read from the Layer-1 payload header (never the
		// wrapper), and the hint prefix it pinned (0: none, or a whole-reserve payload, which
		// ignores its hint).
		SuperSLMPaging::ERestorePayload RestorePayload = SuperSLMPaging::ERestorePayload::Budget;
		int64 RestoreBudget = 0;
		int64 RestoreOrigin = 0;
		int64 RestoreContextLength = 0;
		int64 RestoreHintPrefixId = 0;
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.11 (b) *Adopt and hint* (rev 1.18, PC-15 Q5): a leased cache handle as the hint. Its
		// entry is adopt-pinned from the call until the job delivers or the op is refused (Token 0: none).
		SuperSLMPrefixCacheGlue::FCacheRestoreHint RestoreHintCache;
#endif
	};

	// A small, per-prefix administrative op (D-SLM7342, priced by D-SLM7504): PrefixBegin (the
	// pool-block draw) and PrefixRelease. Each is now one of ESuperSLMWorkerJobKind's seven
	// declared values, priced from its own PrefixBeginCostMs/PrefixReleaseCostMs field (plan §5
	// item 2), and posted through PostLedgerJob rather than PostInternalJob -- so each carries a
	// GetJobLedger() row like every other kind, and R-S1i's existing cross-run identity assertion
	// (which reads the whole ledger rather than naming kinds by enumeration) covers it with no
	// wording change.
	enum class EPrefixAdminKind : uint8 { Begin, Release };
	struct FPrefixAdminOp
	{
		EPrefixAdminKind Kind = EPrefixAdminKind::Begin;
		int64 Ordinal = 0;
		int64 PrefixId = 0;

		// Release only (T-2815 round 13, closing the ReleasePrefix() pool-leak finding):
		// ReleasePrefix() removes the caller-visible FPrefixEntry from State->Prefixes
		// SYNCHRONOUSLY, at once, while this op still queues the worker's own real
		// sslm_prefix_release behind it -- so by the time this op is admitted and dispatched, the
		// entry it would have read Handle/PinnedLane from is already gone. ReleaseHandle and
		// PinnedLane are captured at ReleasePrefix() call time, while the entry still exists, and
		// stand in for the entry's own fields for exactly this one job. PinnedLane is a plain
		// local copy here (never written back to anything, unlike FPrefixEntry::PinnedLane) --
		// correct for Release, since nothing reads a released prefix's pin again afterward.
		sslm_prefix ReleaseHandle = nullptr;
		int32 PinnedLane = INDEX_NONE;

		// Budget mode (plugin paging plan §3.3), Release only: the page-ledger ticket its delivery
		// applies. A built prefix's release was admitted at ReleasePrefix() (AdmitPrefixRelease),
		// and its delivery applies that ticket (ApplyPrefixRelease). A prefix whose build never
		// applied (faulted, or released before its freeze) has no release admission: its delivery
		// fails the build's own ticket instead (ApplyPrefixBuild(false)), which refunds the whole
		// charge once the engine has freed the pages (bReleaseFailsBuild).
		SuperSLMPaging::FTicket LedgerTicket = SuperSLMPaging::InvalidTicket;
		bool bReleaseFailsBuild = false;

		// The test seams' holds (§3.9), taken when the op is admitted; 0 is none.
		int64 StartHoldId = 0;
		int64 ApplyHoldId = 0;

		// Budget mode (plugin paging plan §3.3, step K3), Begin only: a nested child's Begin is
		// sslm_prefix_begin_from on its parent's frozen handle, captured at CreatePrefixFrom(). The
		// ledger pins the parent from that admission until the child's build applies, so
		// ReleasePrefix() refuses it (PrefixPinned) and the handle stays live.
		bool bNested = false;
		sslm_prefix NestedParentHandle = nullptr;
	};

	struct FPrefixEntry
	{
		sslm_prefix Handle = nullptr;
		TArray<int32> Tokens;
		int32 Consumed = 0;
		ESuperSLMPrefixPhase Phase = ESuperSLMPrefixPhase::Pending;
		FString FaultMessage;
		int32 PendingAdopts = 0;
		bool bAdminJobInFlight = false; // a Begin or Release job is currently on a worker

		// Budget mode (plugin paging plan §3.3): the page-ledger node this build minted, and the
		// build's ticket while it is open (admitted at CreatePrefix(), applied at the freeze's
		// delivery, or failed when the Begin is refused or the prefix is released unbuilt).
		// bLedgerBuilt is set once the build applied: only then may the prefix be adopted, and only
		// then is its release a ledger release (AdmitPrefixRelease).
		SuperSLMPaging::FNodeId LedgerNode = SuperSLMPaging::InvalidNode;
		SuperSLMPaging::FTicket BuildTicket = SuperSLMPaging::InvalidTicket;
		bool bLedgerBuilt = false;

		// The test seams' HoldApply(PrefixBuild) (§3.9), taken at CreatePrefix(): it holds the
		// apply of the job that freezes the prefix (the build's ledger apply). 0 is none.
		int64 BuildApplyHoldId = 0;

		// Budget mode (plugin paging plan §3.3, §3.5; step K3). OriginLength is the parent's length
		// for a nested child (its positions start there), 0 for a root, so the prefix's length is
		// OriginLength + Tokens.Num(). PendingHintedRestores counts restores queued with this
		// prefix as their hint and not yet delivered or refused: ReleasePrefix() refuses while any
		// is, as it does for PendingAdopts (the ledger's own pin covers only admitted ones).
		int64 OriginLength = 0;
		int32 PendingHintedRestores = 0;

		// D-SLM7528/D-SLM7530 (plan §5 item 3): the prefix-side half of lane-pinned admission --
		// INDEX_NONE until this prefix's first-ever job (its own PrefixBegin) posts, resolved and
		// held through ChooseLane exactly like FSlot::PinnedLane, reset only at CreatePrefix() (a
		// prefix's own "next creation", mirroring a sequence's VendSequence()). Read/written ONLY
		// by ChooseLane, from DispatchPrefixAdmin and DispatchPrefixPrefill -- no second mechanism.
		int32 PinnedLane = INDEX_NONE;
	};

	// Plan §2.5 row 20 rule 4 (D-SLM7706, restated on the delivery-gated basis by D-SLM7730): a
	// SetSchema() or RequestAdapterSwap() that cannot take effect at the call. Held game-side in
	// the holder's per-user struct, carried by the holder's next job (marked carried at that
	// job's post) and applied at that job's head on the worker, in ordinal order; confirmed or
	// refused only at that job's delivery.
	enum class EHeldKind : uint8 { Schema, Adapter };
	struct FHeldRequest
	{
		int64 Owner = 0;          // the holder that made the request
		EHeldKind Kind = EHeldKind::Schema;
		FString SchemaName;       // Schema: the name to bind; empty = SSLM_SCHEMA_NONE
		int64 AdapterId = 0;      // Adapter: the adapter to bind; 0 = base
		int64 Ordinal = 0;        // minted at the call from the global op counter
		bool bCarried = false;    // captured by a posted, not yet delivered job
		bool bPinned = false;     // holds one registry pin on AdapterId (T-2987 L2)
	};

	// 1.1 worker continuation (plan §5 item 1a, ruled 2026-09-29): one
	// generation's stop signal, shared by the game thread and the worker lane its jobs run on.
	// Every prefill or decode job of the generation updates it on the worker, right after its
	// Layer-1 call, by the same rule its Deliver() applies on the game thread: a failed call, a
	// token in StopTokenIds, the MaxNewTokens-th token, or any sentinel other than -1 ends the
	// generation. A continuation job, queued behind the job it follows, reads it at its head and
	// makes no Layer-1 call once it is set, so a sequence never runs past its own end. The game
	// thread sets it when it faults the sequence or the holder returns it.
	//
	// Produced is written only on the worker. All of one generation's jobs run on its one pinned
	// lane, whose single thread runs them in post order, so no lock is needed for it.
	struct FGenerationGate
	{
		std::atomic<bool> bStop{false};
		int32 Produced = 0;
		int32 MaxNewTokens = 0;
		TArray<int32> StopTokenIds;

		void Stop() { bStop.store(true, std::memory_order_release); }
		bool IsStopped() const { return bStop.load(std::memory_order_acquire); }

		// Worker only: a decode job's result for this member, by Deliver()'s own terminal rule.
		void NoteDecodeToken(int32 Token)
		{
			if (Token >= 0)
			{
				++Produced;
				if (StopTokenIds.Contains(Token) || Produced >= MaxNewTokens)
				{
					Stop();
				}
			}
			else if (Token != -1)
			{
				Stop(); // -2 (schema dead end), -3 (no longer live), or an unrecognized value
			}
		}
	};
	using FGenerationGatePtr = TSharedPtr<FGenerationGate, ESPMode::ThreadSafe>;

	// 1.1 worker continuation (plan §5 item 1a): one member's claim on a posted, not yet started
	// continuation. A request that must precede the holder's next job (a held schema or adapter
	// request, or a queued lifecycle op) cancels it from the game thread; the continuation reads it
	// at its head on the worker, and a cancelled member is left out exactly as a stopped one is, so
	// the request is carried by the next job ordinary planning posts, as row 20 says. The boundary
	// is the worker head: a request made after the continuation has started lands after it, as a
	// request made while any job runs lands after that job.
	struct FContinuationTicket
	{
		std::atomic<bool> bCancelled{false};
	};
	using FContinuationTicketPtr = TSharedPtr<FContinuationTicket, ESPMode::ThreadSafe>;

	// Schema fast-decode plan rev 1.3.5, §6 P1 and P1-X: one CPU generation's schema jump-forward
	// state, set at activation (BeginJumpGeneration) and updated at each of its jobs' delivery.
	// Caller-owned: it lives in FSlotUser, so a release resets it with the rest of the generation.
	struct FJumpGeneration
	{
		// A schema-bound generation: its jobs read forcedness (when bReads) and it may take span jobs
		// (Mode Exact or Canonical). False for an unbound generation, which runs today's loop.
		bool bActive = false;
		SuperSLMCore::ESchemaJumpMode Mode = SuperSLMCore::ESchemaJumpMode::Off;
		int32 SchemaIndex = INDEX_NONE; // the schema's index in the model's SCM1 section
		bool bReads = false;            // the generation's jobs read sslm_seq_forced_tokens (cap 0)

		// The walk the plugin tracks itself (§6 P1-X "The walk": the ABI has no walk-state query).
		// Known only for a generation that starts from the schema's start state: a fresh or reset
		// sequence (no context, no adopted prefix). Otherwise the generation drafts nothing and feeds
		// nothing (timing only, never tokens, C13).
		bool bWalkKnown = false;
		uint32 WalkState = 0;
		uint32 StartState = 0; // the walk state before the first generated token

		// FJumpSequenceFacts' inputs (SuperSLMCore::NextJob).
		bool bPendingToken = false;
		int32 ForcedChainLength = 0;
		bool bLastJobWasSpan = false;
		bool bLastSpanFailed = false;
		bool bSpanOutstanding = false;
	};

	// Plan §2.5 row 20 rule 1 (D-SLM7701, D-SLM7706): every caller-owned field of a slot, in one
	// struct. FSlot inherits it, and ReleaseUser() -- the only code that ends a hold -- resets it
	// by value, so a field added here later is reset without anyone listing it. Vend and a
	// restore's delivery also start from a value-reset struct. Slot-owned state (the Layer-1
	// handle, the scheduler fields, the Layer-1 mirrors, the reservation flags) lives in FSlot
	// itself and is settled by the job that changes it, never by a hand-off.
	struct FSlotUser
	{
		// K9: the generation request and its progress.
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		TArray<int32> Prompt;
		int32 PromptConsumed = 0;
		ESuperSLMSpanKind SpanKind = ESuperSLMSpanKind::Prompt;
		TArray<int32> Generated;
		int32 MaxNewTokens = 0;
		TArray<int32> StopTokenIds;
		ESuperSLMDecodeOutcome LastOutcome = ESuperSLMDecodeOutcome::Generating;
		FString FaultMessage;
		double BeginSeconds = 0.0;
		double TimeToFirstTokenMs = -1.0;

		// K9, rule 4's carrying: the active generation's Generate op ordinal, and whether its
		// first job has posted. The first job carries every held entry below that ordinal; each
		// later prefill or decode job of the generation carries adapter entries only.
		int64 ActiveGenerateOrdinal = 0;
		bool bGenerateFirstJobPosted = false;

		// 1.1 worker continuation: the active generation's gate, made by its first prefill or
		// decode job (GenerationGateFor) and dropped with the generation. Stopped before this
		// struct is reset by value (ReleaseUser), so a queued continuation of the old holder skips.
		FGenerationGatePtr Gate;

		// K7: the layer-budget pin.
		int32 PinnedLayerBudget = 0;   // 0: the scheduler chooses per job

		// K3, K4: the plugin's records of what Layer 1 has bound, confirmed only at a carrying
		// job's delivery (or at an inline bind).
		FString BoundSchemaName;       // empty = SSLM_SCHEMA_NONE
		int64 ActiveAdapterId = 0;     // 0 = base

		// K5, K6, K14: rule 4's held list, in ordinal order.
		TArray<FHeldRequest> Held;

		// Plan §2.5 row 22: the stats copy GetStats() returns, rewritten at the delivery of each
		// of the holder's jobs that changes the sequence (reset, adopt, restore, prefill,
		// decode) from sslm_stats read at that job's tail on the worker. Never the recycle's,
		// never a save's (T-2988 N7).
		FSuperSLMSequenceStats Stats;

		// Rule 4's deferred-refusal field (T-2988 W5): a carried entry refused by a job outside
		// the generation it preceded. Read and cleared at the holder's next activation, which it
		// faults by name. Per-user, so a holder who returns never passes it to the next holder.
		bool bHasDeferredRefusal = false;
		FString DeferredRefusal;

		// Schema fast-decode plan rev 1.3.5, §6 P1 and P1-X: the active generation's jump-forward
		// state (default: none).
		FJumpGeneration Jump;

		// A new request activates (Generate) on a sequence that may hold an adopted prefix.
		void ClearForBegin()
		{
			Phase = ESuperSLMSequencePhase::Idle;
			Prompt.Reset();
			PromptConsumed = 0;
			SpanKind = ESuperSLMSpanKind::Prompt;
			Generated.Reset();
			MaxNewTokens = 0;
			StopTokenIds.Reset();
			LastOutcome = ESuperSLMDecodeOutcome::Generating;
			FaultMessage.Reset();
			BeginSeconds = 0.0;
			TimeToFirstTokenMs = -1.0;
			ActiveGenerateOrdinal = 0;
			bGenerateFirstJobPosted = false;
			Gate.Reset(); // activation follows every earlier delivery, so no job holds the old one
			Jump = FJumpGeneration();
		}

		bool IsGenerating() const
		{
			return Phase == ESuperSLMSequencePhase::Prefilling || Phase == ESuperSLMSequencePhase::Decoding;
		}

		// ActivateGenerate()'s schema-content check (T-2989 F21): the value of the holder's last
		// held schema entry when one exists, otherwise BoundSchemaName.
		FString EffectiveSchemaName() const
		{
			for (int32 I = Held.Num() - 1; I >= 0; --I)
			{
				if (Held[I].Kind == EHeldKind::Schema)
				{
					return Held[I].SchemaName;
				}
			}
			return BoundSchemaName;
		}
	};

	// Why a slot's bBindEligibleAfterQueue is false, for SetSchema()'s refusal message.
	enum class EBindIneligibleReason : uint8 { None, Generated, Restored, AdoptedPrefix };

	struct FSlot : FSlotUser
	{
		sslm_seq Seq = nullptr;
		int64 VendedId = 0; // 0 while on the free list; the hand-off key

		TArray<FSlotQueuedOp> OpQueue; // FIFO, bounded Config.MaxQueuedOperationsPerSequence

		// D-SLM7403/D-SLM7407 maintainer ruling 2026-09-19 (§14.6), corrected by D-SLM7515: this
		// slot's own front-of-queue op (or its active generation) reaches this tick on the
		// schedule alone -- the CommittedDeliveryTick of whatever job is CURRENTLY posted for it,
		// set at POST time, never gated on that job's real (worker) completion. Reaching this tick
		// is necessary but no longer sufficient for a NEW job to be claimable: IsDue() (below) also
		// requires !HasUndeliveredJob(), plan §5 item 3's "one in flight at a time" rule applied to
		// a decode/prefill submission the same way it already applied to Reset/Adopt/Save/Restore.
		// Composing the NEXT job correctly still needs this slot's
		// LayersDoneInToken/bReadyForLogits/PromptConsumed/ContextUsed
		// to already reflect what the CURRENT job will deterministically do -- so every Dispatch*
		// function updates them SPECULATIVELY at post time (structural progress: how many layers
		// advance, whether a token's layers complete, how many prompt tokens are consumed -- all
		// pure functions of the composition Layer 1 is documented to honour exactly). Deliver()
		// applies only what is genuinely content-dependent (the real token value, a real fault)
		// and is a no-op if an EARLIER (already-delivered) job already made this slot terminal.
		int32 NextAvailableTick = 0;

		// Separate from NextAvailableTick, and real-delivery-gated (incremented at each POST,
		// decremented only once that same job's worker has ACTUALLY run and Deliver() has ACTUALLY
		// run for it, whether or not it applied content). D-SLM7515 (plan §5 item 3, correcting
		// this comment's own prior claim): a request against one sequence -- Reset, Adopt Prefix,
		// Save, Restore, and a decode/prefill submission alike -- runs in strict arrival order, one
		// in flight at a time. IsDue() (below) now consults this counter for admission, exactly as
		// lifecycle-op admission already did (the `Slot.HasUndeliveredJob()` check ahead of
		// DispatchSlotLifecycleOp), so a NEW job for this slot -- of ANY kind -- is never posted
		// while a PRIOR one for it is still outstanding, on any lane. This is what closes the
		// cross-lane race `DispatchSequencePrefill`'s own speculative Phase flip to `Decoding`
		// opened: under `MaxConcurrentCalls > 1`, a slot's prefill could be posted to one lane while
		// the decode job that (speculatively) assumed it had already delivered was posted to a
		// DIFFERENT, currently-free lane in the same or a later tick -- two real Layer-1 calls for
		// one sequence in flight at once, which Layer 1 itself refuses
		// (`SSLM_INVALID_ARGUMENT`, confirmed at `sslm_decode_stepImpl`). A counter, not a bool,
		// because it is also consulted for a second, narrower purpose: protecting a DIRECT, inline
		// Layer-1 call this module makes from the game thread (SetSchema()/RequestAdapterSwap() on
		// an otherwise-idle sequence) from racing a worker job that has been posted to a lane's
		// queue but has not yet reached the front and run. Ordinary admission never takes it above
		// 1. The one exception is the 1.1 continuation (plan §5 item 1a, PlanContinuations): a
		// second prefill or decode job of the same generation, on the same pinned lane, whose
		// single thread runs the two in order, so it reaches 2 and never more.
		int32 OutstandingJobCount = 0;
		bool HasUndeliveredJob() const { return OutstandingJobCount > 0; }

		// 1.1 worker continuation (plan §5 item 1a): the JobId of this slot's most recently posted
		// prefill or decode job. PlanContinuations() reads it against the lane's one pending job to
		// find the job's members; a stale value never matches, because JobIds are never reused.
		int64 ContinuableJobId = 0;

		// 1.1: the ticket of this slot's posted, undelivered continuation (null when there is none),
		// cleared at that continuation's delivery.
		FContinuationTicketPtr QueuedContinuation;
		void CancelQueuedContinuation()
		{
			if (QueuedContinuation.IsValid())
			{
				QueuedContinuation->bCancelled.store(true, std::memory_order_release);
			}
		}

		int64 ContextUsed = 0;         // committed KV positions (mirrors Layer 1's context_length)

		// D-SLM7528/D-SLM7530 (plan §5 item 3): lane-pinned admission. INDEX_NONE until this
		// slot's first-ever job posts; set once, from whichever lane ChooseLane picks; held for
		// the entity's whole life; reset at the slot's own next creation (VendSequence(), a
		// fresh RestoreSequence() reservation) and where its physical identity is recreated (a
		// reset job that creates a fresh sequence for a slot whose sequence a failed restore
		// released, DispatchReset's own Deliver) -- never on delivery, never on
		// OutstandingJobCount reaching zero. The delivery gates make each reset safe: a new
		// tenant's first job is not admitted until the old tenant's last job has delivered
		// (plan §2.5 row 22). Otherwise read/written only by ChooseLane, called from
		// PostLedgerJob.
		int32 PinnedLane = INDEX_NONE;
		int32 LayersDoneInToken = 0;   // planning mirror of Layer 1's layer_index
		bool bReadyForLogits = false;  // planning mirror: the next decode call is a finish only

		// §17: how many ALREADY-POSTED, still-undelivered decode jobs are speculatively predicted
		// (at post time, from the deterministic Layers>=Left formula, §15.1) to be a "finishing"
		// call for this slot -- i.e. an upper bound on how many MORE real tokens those jobs could
		// still add to Generated, before any of their real content is known. At the time this was
		// built, schedule-only due-ness (IsDue/IsFreeForPlanning) had no other brake on how far
		// ahead of real delivery a slot may be planned, and under a pacing mode where logical ticks
		// vastly outrun real per-job time (K stays 1 under a generous TickBudgetMs while a real
		// decode call costs tens of ms), that let PlanNextJobs queue far more real Layer-1 calls
		// than the sequence could ever consume before MaxNewTokens -- confirmed as the
		// CpuDeterminism.Baseline hang (§17). Decode due-ness therefore also requires
		// `SpeculativeFinishesQueued < MaxNewTokens - Generated.Num()`; every Deliver() path
		// decrements it unconditionally, mirroring OutstandingJobCount. D-SLM7515 (plan §5 item 3)
		// has since added `!HasUndeliveredJob()` to IsDue() itself, which bounds
		// OutstandingJobCount -- and therefore this counter, which never exceeds it -- to 0 at
		// every point IsDue() is evaluated, so ordinary admission never reaches this cap. The 1.1
		// continuation (plan §5 item 1a) does: PlanContinuations reads it, with a job already in
		// flight, to decide whether that job's token is the generation's last, and then posts no
		// continuation.
		int32 SpeculativeFinishesQueued = 0;
		bool bAdopted = false;         // holds an adopted prefix's context
		// Schema fast-decode plan rev 1.3.6, §6 P1 "When the walk is known": the adopted prefix was
		// prompt-only (every CPU prefix is: CreatePrefix prefills on the unbound base model with
		// prompt spans), so the engine's walk is still at state 0 while ContextUsed equals its
		// length. Set only by an adoption's delivery; a restore, a reset and a drained recycle clear
		// it with bAdopted.
		bool bAdoptedPromptOnlyPrefix = false;
		int64 AdoptedPrefixLength = 0;

		// True once a queued Restore has reserved this (otherwise-free) slot but the worker has
		// not yet delivered the restore -- excluded from vend/due processing until it delivers.
		// Only that restore's own delivery (or drop) ends it: a previous holder's restore still
		// queued or in flight on the slot does not.
		bool bAwaitingRestore = false;

		// D-SLM7946: the slot's one lifecycle job in flight, as the one-generation projection reads
		// it once the op has left OpQueue. InFlightResetOwner is the holder whose reset
		// DispatchReset() posted while that holder held the slot (0 for none, and for the recycle,
		// which has no owner); the reset's delivery clears it on every path. InFlightRestorePhase is
		// the phase the restore DispatchRestore() posted gives at delivery, read while
		// bAwaitingRestore holds.
		int64 InFlightResetOwner = 0;
		ESuperSLMSequencePhase InFlightRestorePhase = ESuperSLMSequencePhase::Idle;

		// Plan §2.5 row 4 (D-SLM7665): the bind eligibility Layer 1's sequence will have once
		// everything already queued for it has run. True at the vend of a fresh or reset sequence
		// and whenever a reset is queued (the recycle included); false whenever an adopt, a
		// restore or a generation request is queued. SetSchema() refuses by name, without calling
		// Layer 1, while it is false. Slot-owned (K13).
		bool bBindEligibleAfterQueue = true;
		EBindIneligibleReason BindIneligibleReason = EBindIneligibleReason::None;

		// Plan §2.5 row 17: a recycle whose reset or unbind Layer 1 refused leaves a sequence
		// carrying a stale binding or an unknown state, so the slot is withheld from vending
		// until the subsystem is torn down. Logged by name when set.
		bool bWithheld = false;

		// Plugin paging plan §3.9 (FSuperSLMPagedTestSeam::SlotHolderGeneration()): rises each time
		// this slot's Layer-1 holder is replaced -- a reset that creates a fresh sequence, or a
		// restore -- at that job's successful delivery. Slot-owned; never reset.
		int64 HolderGeneration = 0;

		int32 EffectiveQueueDepth() const
		{
			return OpQueue.Num() + (IsGenerating() ? 1 : 0);
		}

		void MarkBindEligible()
		{
			bBindEligibleAfterQueue = true;
			BindIneligibleReason = EBindIneligibleReason::None;
		}
		void MarkBindIneligible(EBindIneligibleReason Reason)
		{
			bBindEligibleAfterQueue = false;
			BindIneligibleReason = Reason;
		}

		// The planning mirrors of Layer 1 a new vendee or a recycle must not inherit, adoption
		// included (K15). §18: they are guarded on HasUndeliveredJob() rather than reset
		// unconditionally -- forcing them to a fixed value while real jobs from the slot's prior
		// occupant are still in flight raced those jobs' own unconditional Deliver-side
		// decrements of SpeculativeFinishesQueued, driving it negative and disabling the
		// admission cap round 9 built. Left alone while busy, the counter settles to exactly 0 on
		// its own (every post is matched by exactly one unconditional decrement), and
		// DispatchReset's own Deliver (recycle or caller-requested) finalizes ContextUsed/bAdopted
		// once the real reset completes.
		void ClearMirrorsIfDrained()
		{
			if (!HasUndeliveredJob())
			{
				ContextUsed = 0;
				LayersDoneInToken = 0;
				bReadyForLogits = false;
				bAdopted = false;
				bAdoptedPromptOnlyPrefix = false;
				AdoptedPrefixLength = 0;
				SpeculativeFinishesQueued = 0;
			}
		}

		// Rule 4's holding condition, shared by SetSchema() and RequestAdapterSwap(): a request
		// is held when the slot has work queued or in flight (an active generation included), or
		// the held list is not empty (T-2986 S2: the inline path keeps arrival order). Otherwise
		// it runs at the call.
		bool MustHoldRequest() const
		{
			return !OpQueue.IsEmpty() || HasUndeliveredJob() || IsGenerating() || !Held.IsEmpty();
		}

		// D-SLM7515 (plan §5 item 3): schedule-only (TickIndex >= NextAvailableTick) is necessary
		// but not sufficient. !HasUndeliveredJob() is the "one in flight at a time" half of the
		// rule, applied to a decode/prefill submission the same way DispatchSlotLifecycleOp's own
		// admission check already applies it to Reset/Adopt/Save/Restore -- without it, a slot
		// whose current job has not yet really delivered (a real hitch, or simply a still-running
		// worker on another lane) is still offered up as a candidate for a NEW job, which is
		// exactly the two-jobs-in-flight race `DispatchSequencePrefill`'s own speculative Phase
		// flip to `Decoding` opened under `MaxConcurrentCalls > 1`.
		bool IsDue(int32 TickIndex) const
		{
			return VendedId != 0 && TickIndex >= NextAvailableTick && !bAwaitingRestore && !HasUndeliveredJob() &&
				(Phase == ESuperSLMSequencePhase::Prefilling || Phase == ESuperSLMSequencePhase::Decoding);
		}
	};

	const TCHAR* SequencePhaseName(ESuperSLMSequencePhase Phase)
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

	// D-SLM7946 (plan §10.5.1.1 item 1): the phase a generation requested now will find when its
	// turn comes, assuming every reset queued ahead of it succeeds.
	struct FGenerationTurnProjection
	{
		bool bIdle = false;
		bool bGenerationQueued = false; // a generation is already queued, so the answer is "not Idle"
		bool bFromRestore = false;      // Phase is what a queued or in-flight restore gives
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
	};

	// Walks the slot's own queue from the newest entry to the oldest: a Generate answers "not
	// Idle"; a Reset answers Idle; a Restore answers the phase it gives (known at the call, because
	// RestoreSequence() parses the blob before it queues anything); an Adopt or a Save is skipped.
	// With none of these queued, the one lifecycle job the slot may have in flight answers: the
	// holder's own reset gives Idle (the recycle, which has no owner, is never counted: a new
	// holder's per-user struct is reset by value at its vend), and a restore gives the phase it
	// gives. Otherwise the answer is the phase now. It depends only on the calls made, never on
	// when ticks ran between them.
	FGenerationTurnProjection ProjectGenerationTurn(const FSlot& Slot)
	{
		FGenerationTurnProjection P;
		for (int32 I = Slot.OpQueue.Num() - 1; I >= 0; --I)
		{
			const FSlotQueuedOp& Op = Slot.OpQueue[I];
			switch (Op.Kind)
			{
				case ESlotOpKind::Generate:
					P.bGenerationQueued = true;
					P.Phase = Slot.Phase;
					return P;
				case ESlotOpKind::Reset:
					P.bIdle = true;
					return P;
				case ESlotOpKind::Restore:
					P.bFromRestore = true;
					P.Phase = Op.RestoreProjectedPhase;
					P.bIdle = P.Phase == ESuperSLMSequencePhase::Idle;
					return P;
				case ESlotOpKind::Adopt:
				case ESlotOpKind::Save:
					break;
			}
		}
		if (Slot.InFlightResetOwner != 0 && Slot.InFlightResetOwner == Slot.VendedId)
		{
			P.bIdle = true;
			return P;
		}
		if (Slot.bAwaitingRestore)
		{
			P.bFromRestore = true;
			P.Phase = Slot.InFlightRestorePhase;
			P.bIdle = P.Phase == ESuperSLMSequencePhase::Idle;
			return P;
		}
		P.Phase = Slot.Phase;
		P.bIdle = P.Phase == ESuperSLMSequencePhase::Idle;
		return P;
	}

	// D-SLM7946 (plan §10.5.1.1 item 1.5): BeginGeneration()'s call-time refusal. It names the
	// rule, the state that fails it and ResetSequence() as the remedy; it never uses the queue
	// bound's own wording.
	FString OneGenerationRefusalText(const FGenerationTurnProjection& P)
	{
		if (P.bGenerationQueued)
		{
			return TEXT("one generation at a time: a generation is already queued on this sequence and no reset is queued after it; call ResetSequence() first");
		}
		const TCHAR* PhaseText = SequencePhaseName(P.Phase);
		FString Text = P.bFromRestore
			? FString::Printf(TEXT("one generation at a time: this sequence is %s once its restore is delivered, and no reset is queued after the restore; call ResetSequence() first"), PhaseText)
			: FString::Printf(TEXT("one generation at a time: this sequence is %s and no reset is queued after its last generation; call ResetSequence() first"), PhaseText);
		switch (P.Phase)
		{
			case ESuperSLMSequencePhase::Prefilling:
			case ESuperSLMSequencePhase::Decoding:
				Text += TEXT(" (a reset requested while a generation runs interrupts it)");
				break;
			case ESuperSLMSequencePhase::Complete:
				Text += TEXT(" (a generation that continues a completed one is not supported)");
				break;
			case ESuperSLMSequencePhase::Faulted:
				Text += TEXT(", or return the sequence and restore a save, which gives a new sequence");
				break;
			case ESuperSLMSequencePhase::Idle:
				break;
		}
		return Text;
	}

	// One planning-time decision for a batched decode job: the shared layer_budget every
	// sequence in the batch advances by this job, and the composition it produces.
	struct FDecodeComposition
	{
		int32 LayerBudget = 1;
		int32 TotalLayers = 0;
		int32 TotalFinishes = 0;
		// D-SLM7793/D-SLM7794: the sum over this composition's decode layers of the serving slot's
		// ContextUsed -- the positions it serves, reported as FSuperSLMWorkerJobReport::
		// DecodeLayerDepthSum and priced by LayerCostPerPositionMs.
		int64 DecodeLayerDepthSum = 0;
		double PlannedMs = 0.0;
	};

	// D-SLM7793/D-SLM7794 (plan §5 item 2): one decode layer at depth d costs
	// LayerCostMs + LayerCostPerPositionMs x d, so a composition costs
	// TotalLayers x LayerCostMs + LayerCostPerPositionMs x DecodeLayerDepthSum + finishes.
	double DecodeCompositionCostMs(const FDecodeComposition& C, double LayerCostMs, double LayerCostPerPositionMs, double FinishCostMs)
	{
		return C.TotalLayers * LayerCostMs + LayerCostPerPositionMs * static_cast<double>(C.DecodeLayerDepthSum) + C.TotalFinishes * FinishCostMs;
	}

	// D-SLM7793/D-SLM7794: the sum of the positions of Chunk prompt tokens starting at depth d,
	// Chunk x d + Chunk x (Chunk - 1) / 2 -- reported as PromptPositionSum and priced by the
	// per-position term of the pair the chunk selects.
	using SuperSLMPrefillPlanning::PromptChunkPositionSum;

	// Slice 1P (tiled-matmul plan §4A.1 item 2): the two prompt prices a configuration carries --
	// the chunk-1 pair for a chunk below 8 tokens, the batched pair from 8 up -- read from the
	// resolved configuration (decode-threading D4, D-SLM8144).
	SuperSLMPrefillPlanning::FPromptPrice Chunk1PromptPrice(const FSuperSLMResolvedCpuConfig& Costs)
	{
		return { Costs.PromptTokenCostMs, Costs.PromptTokenCostPerPositionMs };
	}

	SuperSLMPrefillPlanning::FPromptPrice BatchedPromptPrice(const FSuperSLMResolvedCpuConfig& Costs)
	{
		return { Costs.BatchedPromptTokenCostMs, Costs.BatchedPromptTokenCostPerPositionMs };
	}

	// Schema fast-decode plan rev 1.3.5/1.3.6, §6 P1 "Pricing": the per-C table for chunks of 2..7
	// tokens, the resolved configuration's field (rev 1.3.6, P1.1): the caller's or a saved
	// estimate's C = 2..7 arms, else the chunk-1 pair until B5f fills it.
	SuperSLMPrefillPlanning::FPerChunkPromptPrices PerChunkPromptPrices(const FSuperSLMResolvedCpuConfig& Costs)
	{
		return SuperSLMRuntimeResolve::PerChunkPromptPrices(Costs);
	}

	// A prefill chunk -- or a span job of Chunk rows (plan rev 1.3.5, §6) -- of Chunk tokens starting
	// at depth d costs Chunk x TokenMs + PerPositionMs x (Chunk x d + Chunk x (Chunk - 1) / 2), with
	// the pair the chunk's own size selects: the chunk-1 pair at 1, the per-C table at 2..7, the
	// batched pair from 8 (SuperSLMPrefillPlanning::SelectPromptPriceWithTable).
	double PrefillChunkCostMs(int32 Chunk, int64 StartDepth, const FSuperSLMResolvedCpuConfig& Costs)
	{
		return SuperSLMPrefillPlanning::PrefillChunkCostMsWithTable(Chunk, StartDepth, Chunk1PromptPrice(Costs), PerChunkPromptPrices(Costs), BatchedPromptPrice(Costs));
	}

	// The chunk one prefill job carries (at least 1, at most MaxChunk): the largest whose
	// pair-selected, depth-priced cost fits ShareMs, raised to min(MinPrefillChunk, MaxChunk)
	// (slice 1P's floor). The sequence prefill (depth = the slot's ContextUsed) and the prefix prefill
	// (depth = the prefix's own Consumed) both size their chunk here (D-SLM7794's third and fourth
	// sites), and so does every continuation of a sequence prefill (1.1, PlanContinuations).
	int32 PlanPrefillChunk(int32 MaxChunk, int64 StartDepth, double ShareMs, int32 MinPrefillChunk, const FSuperSLMResolvedCpuConfig& Costs)
	{
		return SuperSLMPrefillPlanning::PlanPrefillChunkWithTable(MaxChunk, StartDepth, ShareMs, MinPrefillChunk,
			Chunk1PromptPrice(Costs), PerChunkPromptPrices(Costs), BatchedPromptPrice(Costs));
	}

	// D-SLM7414 (plan §5): "the Plan step picks the largest composition whose planned cost fits
	// TickBudgetMs x BudgetHeadroom using these fixed [static] costs" -- cost(L) is monotonic
	// non-decreasing in L (more layers never finishes fewer sequences), so the largest admissible
	// L is found by a linear scan from 1. L=1 always runs even when it alone exceeds the share
	// (the floor no composition may go below, D-SLM7390).
	FDecodeComposition PlanDecodeBatch(
		const TArray<int32>& SlotIndices, const TArray<FSlot>& Slots, int32 NumHiddenLayers,
		int32 MaxLayerBudget, double ShareMs, double LayerCostMs, double LayerCostPerPositionMs, double FinishCostMs)
	{
		auto CostAt = [&](int32 L)
		{
			FDecodeComposition C;
			C.LayerBudget = L;
			for (int32 Idx : SlotIndices)
			{
				const FSlot& S = Slots[Idx];
				const int32 Left = S.bReadyForLogits ? 0 : NumHiddenLayers - S.LayersDoneInToken;
				const int32 LayersThis = FMath::Min(L, Left);
				C.TotalLayers += LayersThis;
				C.DecodeLayerDepthSum += static_cast<int64>(LayersThis) * S.ContextUsed; // D-SLM7794 site 1
				if (L >= Left)
				{
					++C.TotalFinishes;
				}
			}
			C.PlannedMs = DecodeCompositionCostMs(C, LayerCostMs, LayerCostPerPositionMs, FinishCostMs);
			return C;
		};
		FDecodeComposition Chosen = CostAt(1);
		for (int32 L = 2; L <= MaxLayerBudget; ++L)
		{
			const FDecodeComposition Candidate = CostAt(L);
			if (Candidate.PlannedMs <= ShareMs)
			{
				Chosen = Candidate;
			}
			else
			{
				break;
			}
		}
		return Chosen;
	}

	// Plan §5 "The scheduler's numeric domain" (D-SLM7799, T-3004 F1). Configure() accepts a
	// configuration only if every job it can plan has a finite planned cost and a K no larger than
	// kMaxJobTicks (about 4.85 h of 60 Hz ticks for one job). The CPU subsystem plans nothing at or
	// past kTickHorizon, so TickIndex + K never exceeds INT32_MAX.
	constexpr int32 kMaxJobTicks = SuperSLMRuntimeResolve::kMaxJobTicks;
	constexpr int32 kTickHorizon = TNumericLimits<int32>::Max() - kMaxJobTicks;

	// The fault a run-time guard raises. Both guards are defensive after Configure()'s domain check,
	// so either failing is a plugin defect by construction.
	const TCHAR* const kSchedulingDomainFault = TEXT("planned cost or delivery tick outside the validated scheduling domain (plugin defect)");

	// D-SLM7407 (plan §5): K = max(1, ceil(PlannedJobMs / TickBudgetMs)), checked (D-SLM7799). Fails
	// for a non-finite or negative planned cost, a non-finite or non-positive budget, and a quotient
	// above kMaxJobTicks. (ComputeK's FMath::Max(TickBudgetMs, 1e-9) guard is gone: Configure() refuses
	// every budget that could reach it.)
	bool TryComputeK(double PlannedJobMs, double TickBudgetMs, int32& OutK)
	{
		if (!FMath::IsFinite(PlannedJobMs) || PlannedJobMs < 0.0 || !FMath::IsFinite(TickBudgetMs) || TickBudgetMs <= 0.0)
		{
			return false;
		}
		const double Quotient = std::ceil(PlannedJobMs / TickBudgetMs);
		if (!FMath::IsFinite(Quotient) || Quotient > static_cast<double>(kMaxJobTicks))
		{
			return false;
		}
		OutK = FMath::Max(1, static_cast<int32>(Quotient));
		return true;
	}

	// The committed delivery tick TickIndex + K, checked against INT32_MAX (D-SLM7799).
	bool TryCommitTick(int32 TickIndex, int32 K, int32& OutTick)
	{
		if (TickIndex < 0 || K < 0 || static_cast<int64>(TickIndex) + static_cast<int64>(K) > static_cast<int64>(TNumericLimits<int32>::Max()))
		{
			return false;
		}
		OutTick = TickIndex + K;
		return true;
	}

	// One worker lane's persistent thread (D-SLM7407, §5.1: named "SuperSLM CPU Worker %d").
	// D-SLM7403/D-SLM7407 maintainer ruling 2026-09-19 (§14.6): PLANNING must be able to commit a
	// job's own PlannedAtTick/K/CommittedDeliveryTick from the schedule alone, decoupled from
	// when the worker actually gets around to running the PREVIOUS job -- otherwise a slow
	// worker's own real lateness pushes every LATER job's post time, and every committed tick
	// after it drifts (R-S1i's own measured symptom). The worker is therefore a real FIFO queue,
	// not a single-slot rendezvous: the game thread may enqueue a NEW job before the worker has
	// even started the one ahead of it, and the worker drains its own queue strictly in arrival
	// (== commit) order, at whatever real pace it can manage. A slow worker only makes DELIVERY
	// late (a hitch, gated in Apply exactly as before) -- it never feeds back into what gets
	// planned or when.
	struct FQueuedWorkerJob
	{
		TFunction<void()> Execute;
		std::atomic<bool> bDone{false};
		double WorkerCallMs = -1.0;
		uint32 WorkerThreadId = 0;
		// D-SLM7407's wall-clock half of "what counts as a hitch": when Enqueue() accepted the job
		// (game thread, at its planning tick) and when the worker finished it. Apply compares the
		// span against K ticks' worth of TickBudgetMs, so a host ticking faster than TickBudgetMs
		// never blames the worker for its own pace. Written before bDone's release store, read
		// only after its acquire load.
		double PostedSeconds = 0.0;
		double FinishedSeconds = 0.0;
	};
	using FQueuedWorkerJobRef = TSharedRef<FQueuedWorkerJob, ESPMode::ThreadSafe>;
	// §16: STORAGE for a queued job is always TSharedPtr, never TSharedRef -- CoreMisc.cpp:438's
	// own fatal error ("The TSharedRef() constructor is for internal usage only... Please do NOT
	// use it"). TSharedRef's default constructor exists only for hot-reload and checkf-crashes at
	// runtime; any container element or struct member of TSharedRef type is default-constructed
	// by ordinary container/struct machinery (an aggregate's own default ctor, TArray growth,
	// etc.), which is exactly what round 7's per-lane FIFO made reachable for the first time.
	// FQueuedWorkerJobRef stays the PARAMETER/return type at a call's own boundary (a caller
	// always hands over a real, already-constructed job, so a Ref correctly documents "never
	// null here") -- only the STORED, dequeue-and-possibly-shuffled copy is ever a Ptr.
	using FQueuedWorkerJobPtr = TSharedPtr<FQueuedWorkerJob, ESPMode::ThreadSafe>;

	class FSuperSLMCpuWorkerRunnable : public FRunnable
	{
	public:
		// Drains the queue BEFORE honouring a stop request, so every job Enqueue() accepted has run
		// by the time Run() returns. TearDown() depends on this: it calls Kill(bShouldWait=true)
		// and then delivers every job still pending on each lane, and a job that was posted but
		// never executed would leave the worker-side half of a handle change undone (a queued
		// prefix release never reaching sslm_prefix_release). Checking the stop flag first let a
		// worker woken by Stop() exit with a freshly enqueued job still in its queue.
		virtual uint32 Run() override
		{
			for (;;)
			{
				WakeEvent->Wait();
				for (;;)
				{
					// TSharedPtr, not TSharedRef -- see FQueuedWorkerJobPtr's own comment above.
					// The empty state here is genuinely reachable (the loop-exit case, "nothing
					// left to dequeue"); every element actually pulled from Queue is checked below
					// before use.
					FQueuedWorkerJobPtr Current;
					{
						FScopeLock Lock(&QueueCS);
						if (Queue.Num() == 0)
						{
							break;
						}
						Current = Queue[0];
						Queue.RemoveAt(0, EAllowShrinking::No);
					}
					checkf(Current.IsValid(), TEXT("SuperSLM CPU worker dequeued a null job -- Enqueue() never stores a null Ptr, so this is an internal invariant violation, not a runtime condition to degrade past."));
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.WorkerJob", SuperSLMChannel);
					Current->WorkerThreadId = FPlatformTLS::GetCurrentThreadId();
					const double Start = FPlatformTime::Seconds();
					if (Current->Execute)
					{
						Current->Execute();
					}
					Current->FinishedSeconds = FPlatformTime::Seconds();
					Current->WorkerCallMs = (Current->FinishedSeconds - Start) * 1000.0;
					Current->bDone.store(true, std::memory_order_release);
				}
				if (bStopRequested.load(std::memory_order_acquire))
				{
					return 0;
				}
			}
		}
		virtual void Stop() override
		{
			bStopRequested.store(true, std::memory_order_release);
			if (WakeEvent)
			{
				WakeEvent->Trigger();
			}
		}

		// Game-thread only: appends to the tail and wakes the worker. Safe to call whether the
		// worker is idle, mid-job, or has other entries still queued -- the worker always drains
		// strictly from the front. Takes a Ref (never null at the call boundary, §16's own
		// convention) and stores it as a Ptr (see FQueuedWorkerJobPtr's own comment).
		void Enqueue(FQueuedWorkerJobRef Job)
		{
			Job->PostedSeconds = FPlatformTime::Seconds();
			{
				FScopeLock Lock(&QueueCS);
				Queue.Add(FQueuedWorkerJobPtr(MoveTemp(Job)));
			}
			WakeEvent->Trigger();
		}

		FEvent* WakeEvent = nullptr;
		std::atomic<bool> bStopRequested{false};
#if WITH_DEV_AUTOMATION_TESTS
		// Precache plan §8 (rev 1.18), the prefix cache's build hold: the cache builds posted to this lane while the
		// hold was armed and not yet past it. While above 0 the lane is not free for planning (FLane::IsFreeForPlanning).
		std::atomic<int32> TestBuildHolds{0};
#endif
		FCriticalSection QueueCS;
		TArray<FQueuedWorkerJobPtr> Queue; // guarded by QueueCS; the worker pops the front,
		                                     // the game thread only ever appends to the back
	};

	// One lane's own record of a job it posted, oldest (soonest-committed) first. Delivery
	// (Apply) drains from the front. Per-entity order is always preserved, because each sequence
	// or prefix has at most one undelivered job; entries for different entities may apply out of
	// lane order, and only behind a test seam's HoldApply() hold (plugin paging plan §3.9, K3).
	struct FLanePendingDelivery
	{
		FQueuedWorkerJobPtr WorkerJob; // Ptr, not Ref -- see FQueuedWorkerJobPtr's own comment;
		                                 // this struct's own default construction (Post(), below)
		                                 // is exactly what CoreMisc.cpp:438 caught (§16)
		TFunction<void(double)> Deliver; // game-thread completion callback, run once actually
		                                   // delivered; the argument is the job's own measured
		                                   // WorkerCallMs
		int64 LedgerRow = INDEX_NONE;    // the job's row number in State->JobLedger; every posted
		                                   // job, PrefixBegin/Release included, gets one via PostLedgerJob
		int32 CommittedDeliveryTick = 0; // never revised after planning
		// Copied from the ledger row at post, so delivery counts hitches and reads the job's kind
		// even when the ring has since overwritten the row.
		int64 JobId = 0;
		ESuperSLMWorkerJobKind Kind = ESuperSLMWorkerJobKind::DecodeOrPrefill;
		int32 K = 0;
		int32 ChainedWaitTicks = 0; // 1.1: a continuation's ticks queued behind the job it follows
		// Plugin paging plan §3.9: the test seams' HoldApply() hold on this job's delivery (0 for
		// none). Apply leaves this job undelivered while it holds; later entries on its lane, which
		// belong to other entities, deliver past it (K3).
		int64 ApplyHoldId = 0;
		// Schema fast-decode plan rev 1.3.5, §6: a span job (one sslm_seq_jump_forward call). It has
		// no ledger row (LedgerRow INDEX_NONE) and is never a continuation's predecessor.
		bool bSpanJob = false;
	};

	struct FLane
	{
		FSuperSLMCpuWorkerRunnable* Runnable = nullptr;
		FRunnableThread* Thread = nullptr;
		void* WorkspaceBuffer = nullptr;
		sslm_workspace Workspace = nullptr;

		// Plan §2.5 row 22 (T-2985 §5): one save staging buffer per lane, because two saves on
		// two lanes can run at once at MaxConcurrentCalls > 1. Sized by sslm_seq_state_size (an
		// upper bound) and created at Configure() (§4). Overwritten by each save on this lane and
		// read only up to the bytes that save reports.
		void* SaveStaging = nullptr;

		// Game-thread only (Plan appends at the back; Apply removes from the front, or past an entry
		// a test hold keeps, K3) -- never touched by the worker thread, which only ever sees
		// FQueuedWorkerJob via its OWN queue.
		TArray<FLanePendingDelivery> Pending;

		// D-SLM7403/D-SLM7407: this lane is available to PLAN a new job once its own schedule
		// says so -- the most recently POSTED job's own CommittedDeliveryTick -- never "once the
		// worker has actually finished." A lane with nothing pending is available immediately.
		bool IsFreeForPlanning(int32 TickIndex) const
		{
#if WITH_DEV_AUTOMATION_TESTS
			// Precache plan §8 (rev 1.18): a lane holding a cache build at the test-only build hold takes no new job,
			// since a job queued behind the held one could not run until the hold is cleared. Test seams only: the
			// counter is 0 unless a cell arms the hold.
			if (Runnable != nullptr && Runnable->TestBuildHolds.load(std::memory_order_acquire) > 0)
			{
				return false;
			}
#endif
			// Precache plan rev 1.18.10 (§15.29, code review S1): a lane held for a cache build's waiting chunk takes no
			// other new job (SuperSLMCore::LaneHeldForChunk); the chunk itself is posted through IsFreeForChunk.
			if (IsHeldForChunk())
			{
				return false;
			}
			return Pending.Num() == 0 || TickIndex >= Pending.Last().CommittedDeliveryTick;
		}

		// Precache plan rev 1.18.10 (§15.29): IsFreeForPlanning without the chunk hold, for the waiting chunk itself.
		bool IsFreeForChunk(int32 TickIndex) const
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (Runnable != nullptr && Runnable->TestBuildHolds.load(std::memory_order_acquire) > 0)
			{
				return false;
			}
#endif
			return Pending.Num() == 0 || TickIndex >= Pending.Last().CommittedDeliveryTick;
		}

		// Precache plan rev 1.18.10 (§15.29, code review S1): the alternation on a sliced cache build's lane. The id of
		// the last job posted here, and, while a build's next chunk waits for this lane, the id of that build's
		// previous job here (0 when no chunk waits; the prefix-cache glue sets it at each plan step). Once the lane has
		// taken one job since that previous job, it is held for the chunk.
		int64 LastPostedJobId = 0;
		int64 ChunkWaitingAfterJobId = 0;
		bool IsHeldForChunk() const
		{
			return SuperSLMCore::LaneHeldForChunk(LastPostedJobId, ChunkWaitingAfterJobId);
		}

		void Post(FQueuedWorkerJobRef WorkerJob, TFunction<void(double)> Deliver, int64 LedgerRow, const FSuperSLMWorkerJobReport& Job)
		{
			FLanePendingDelivery Entry; // safe: WorkerJob is a Ptr, default-constructs to null
			Entry.WorkerJob = WorkerJob; // TSharedPtr = TSharedRef: a real, non-null assignment
			Entry.Deliver = MoveTemp(Deliver);
			Entry.LedgerRow = LedgerRow;
			Entry.CommittedDeliveryTick = Job.CommittedDeliveryTick;
			Entry.JobId = Job.JobId;
			Entry.Kind = Job.Kind;
			Entry.K = Job.K;
			Entry.ChainedWaitTicks = Job.ChainedWaitTicks;
			Pending.Add(MoveTemp(Entry));
			LastPostedJobId = Job.JobId;
			Runnable->Enqueue(MoveTemp(WorkerJob));
		}
	};

	// D-SLM7528 (plan §5 item 3): the ONE place PinnedLane (a sequence's or a prefix's) is ever
	// read or written. Pinned: returned unchanged. Unpinned: the lowest-index entry of
	// CandidateLanes -- a single, explicit, deterministic tie-break, matching D-SLM7428's own
	// "never container iteration order, a hash, or a pointer value" -- written back and returned.
	// The chosen lane is removed from CandidateLanes in the same step, for every caller, pinned or
	// not: this is what lets a second entity sharing the same pinned lane, also due this same
	// tick, correctly find its own lane already gone (CanClaimLane, below) instead of being posted
	// a second time onto a lane this tick already claimed.
	int32 ChooseLane(int32& EntityPinnedLane, TArray<int32>& CandidateLanes)
	{
		if (EntityPinnedLane != INDEX_NONE)
		{
			CandidateLanes.RemoveSingle(EntityPinnedLane);
			return EntityPinnedLane;
		}
		checkf(CandidateLanes.Num() > 0, TEXT("ChooseLane: CandidateLanes must never be empty for an unpinned entity -- the caller's own due-ness/claimability check (PlanNextJobs) is what guarantees this, not this function."));
		const int32 Chosen = CandidateLanes[0]; // FreeLanes is always built in ascending lane-index order (PlanNextJobs)
		CandidateLanes.RemoveAt(0);
		EntityPinnedLane = Chosen;
		return Chosen;
	}

	// PlanNextJobs's own due-ness filter, restated once here rather than per site (plan §5 item
	// 3): an entity pinned to a lane that is not free for planning this tick is not due at all --
	// it is never offered to ChooseLane, and never competes for a different lane.
	bool IsLanePinFree(int32 PinnedLane, const TArray<FLane>& Lanes, int32 TickIndex)
	{
		return PinnedLane == INDEX_NONE || Lanes[PinnedLane].IsFreeForPlanning(TickIndex);
	}

	// Two different entities pinned to the SAME lane can both be due in one planning pass (the
	// accepted "one lane busier" throughput cost, plan §5 item 3): IsLanePinFree alone, checked
	// once at the top of the tick, cannot see that an earlier dispatch THIS SAME tick already
	// claimed that lane. CanClaimLane is the check a dispatch loop makes immediately before
	// calling PostLedgerJob for each due candidate in turn, so the loser of the contention is left
	// due for a later tick rather than force-posted a lane no longer in CandidateLanes.
	bool CanClaimLane(int32 PinnedLane, const TArray<int32>& CandidateLanes)
	{
		return PinnedLane == INDEX_NONE ? !CandidateLanes.IsEmpty() : CandidateLanes.Contains(PinnedLane);
	}

	// D-SLM7505/D-SLM7506 (plan §5.1, T-2850 fold 8/9): one ring-buffer entry for the
	// tokens/second rolling window -- a real wall-clock reading (FPlatformTime::Seconds(), the
	// same call TimeToFirstTokenMs already uses) paired with the delivered-token counter at that
	// instant, never an assumed elapsed time.
	struct FTokensPerSecondSample
	{
		double RealSeconds = 0.0;
		int32 TokensFinishedTotal = 0;
	};

	// The number of rows the job ledger and the tick history each keep, in a shipping build and
	// by default in a test build. At 60 ticks a second this is the last 17 seconds of ticks; every
	// product reader takes what it needs within a tick or two of the row being written.
	constexpr int32 kReportHistoryRows = 1024;

#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMSchedulingTestAccess::SetReportHistoryCapacity(): a test that reads a whole run's
	// history sets a larger capacity before Configure(). 0 means kReportHistoryRows. The ring code
	// is the same either way; only its size differs.
	int32 GReportHistoryRowsOverride = 0;
#endif

	int32 ReportHistoryRows()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GReportHistoryRowsOverride > 0)
		{
			return GReportHistoryRowsOverride;
		}
#endif
		return kReportHistoryRows;
	}

	// A fixed-capacity ring of report rows, oldest first. Its storage is created once, by Init()
	// at Configure(), and never resized, so appending a row never allocates: once the ring is
	// full, each append overwrites the oldest row. A row's number counts every row appended since
	// Configure(), from 0, so a reader can hold a position across ticks and tell when the rows it
	// has not read yet were overwritten.
	template <typename RowType>
	struct TReportRing
	{
		void Init(int32 Capacity)
		{
			Storage.SetNum(FMath::Max(Capacity, 1));
			Head = 0;
			Count = 0;
			Appended = 0;
		}

		int32 Num() const { return Count; }
		int64 AppendedCount() const { return Appended; }
		int64 FirstRetainedRow() const { return Appended - Count; }

		// The slot the next row goes in, which is the oldest row's slot once the ring is full.
		// The caller overwrites every field of it.
		RowType& Append()
		{
			const int32 Capacity = Storage.Num();
			const int32 SlotIndex = (Head + Count) % Capacity;
			if (Count < Capacity)
			{
				++Count;
			}
			else
			{
				Head = (Head + 1) % Capacity;
			}
			++Appended;
			return Storage[SlotIndex];
		}

		// Row number Row, or null when it has been overwritten or does not exist yet.
		const RowType* Find(int64 Row) const
		{
			if (Row < FirstRetainedRow() || Row >= Appended)
			{
				return nullptr;
			}
			return &Storage[static_cast<int32>((Head + (Row - FirstRetainedRow())) % Storage.Num())];
		}
		RowType* Find(int64 Row)
		{
			return const_cast<RowType*>(static_cast<const TReportRing&>(*this).Find(Row));
		}

		const RowType* Last() const { return Count > 0 ? Find(Appended - 1) : nullptr; }
		RowType* Last() { return Count > 0 ? Find(Appended - 1) : nullptr; }

		// A copy of the retained rows, oldest first. For readers outside the request path.
		TArray<RowType> ToArray() const
		{
			TArray<RowType> Out;
			Out.Reserve(Count);
			for (int32 I = 0; I < Count; ++I)
			{
				Out.Add(Storage[(Head + I) % Storage.Num()]);
			}
			return Out;
		}

		// Every slot, retained or not, for Init()'s callers to pre-size a row's own members.
		TArray<RowType>& Slots() { return Storage; }

	private:
		TArray<RowType> Storage;
		int32 Head = 0;      // the storage index of the oldest retained row
		int32 Count = 0;     // retained rows, 0 to Storage.Num()
		int64 Appended = 0;  // rows appended since Init()
	};

	// Clears a ledger row for reuse, keeping its member list's allocation.
	void ResetJobRow(FSuperSLMWorkerJobReport& Row)
	{
		TArray<FSuperSLMSequence> Members = MoveTemp(Row.MemberSequences);
		Members.Reset();
		Row = FSuperSLMWorkerJobReport();
		Row.MemberSequences = MoveTemp(Members);
	}
}

// Plugin paging plan §3.4 "Prefixes" (K7): the GPU subsystem mints its prefix ids from this file's
// counter, so the two backends' prefix handles never coincide and a handle that crosses backends is
// refused as unknown. Game thread, as every CPU prefix verb. Declared in SuperSLMGpuSubsystem.cpp.
int64 SuperSLMMintPrefixId();
int64 SuperSLMMintPrefixId()
{
	return GNextPrefixId++;
}

// Schema fast-decode plan rev 1.3.6, review finding 1: the walk table, parsed over the pinned artifact
// bytes. SchemaMasksTable::Parse keeps pointers INTO the section, so Pin must outlive Schemas, which
// it does by being this struct's first member (destroyed last).
struct FSchemaWalkTable
{
	FSuperSLMArtifactBytesPin Pin;
	superslm::SchemaMasksTable Schemas;
};
using FSchemaWalkTableRef = TSharedPtr<const FSchemaWalkTable, ESPMode::ThreadSafe>;

// One walk-table load's output, written by the pool task and read by the game thread once bDone is
// set (release/acquire). The task owns its own copy of the captured bytes, so a state destroyed
// while it runs leaves it nothing dangling.
struct FSchemaWalkLoad
{
	std::atomic<bool> bDone{false};
	TSharedPtr<FSchemaWalkTable, ESPMode::ThreadSafe> Table; // null when the load failed or found no section
	FString Warning;                                          // logged at publish when non-empty
	bool bAnyOnGameThread = false;
	int64 SectionBytes = -1;
	int64 ArtifactBytes = -1;
};
using FSchemaWalkLoadRef = TSharedPtr<FSchemaWalkLoad, ESPMode::ThreadSafe>;

struct FSuperSLMSubsystemState
{
	sslm_model Model = nullptr;
	uint8 ArtifactHash[32] = {};
	SuperSLMRuntime::FModelShape Shape;
	FSuperSLMRuntimeConfig Config;
	// Decode-threading D4 (D-SLM8144, D-SLM8165): the mode, task count and thirteen costs this
	// configuration resolved on the calling thread. Every lane, the hook install and every price
	// read these, never Config's optional fields.
	FSuperSLMResolvedCpuConfig Resolved;

	void* KvBuffer = nullptr;
	int64 KvBytes = 0;
	sslm_kv_pool Pool = nullptr;

	int64 WorkspaceBytesEach = 0;
	TArray<FLane> Lanes; // one worker thread + workspace each (D-SLM7407, retargeted from
	                       // T-2815's ParallelFor-joined lanes to standing worker threads)

	int64 SaveStagingBytes = 0;        // §4: each lane's save buffer size (FLane::SaveStaging)

	TArray<FSlot> Slots;
	TMap<int64, int32> IdToSlot;
	TArray<int32> FreeSlots;           // FIFO

	TMap<int64, FPrefixEntry> Prefixes;
	TArray<FPrefixAdminOp> PrefixAdminQueue; // FIFO of pending Begin/Release ops

	// 1.1 (plan §5 item 1a, "Lane depth"): a prefix the caller released whose worker work is not
	// done yet -- a job still in flight, or its release still queued or in flight. It leaves
	// Prefixes (the caller's view) at ReleasePrefix(), but keeps its PrefixBlockCount place for
	// admission until its release delivers: a new prefix's Begin is not admitted while begun and
	// retiring prefixes already fill PrefixBlockCount. So a prefix never has more than one job
	// posted and undelivered, create/release cycling cannot stack jobs on a lane, and a Begin never
	// needs a pool block a release has not yet returned. An orphaned Begin's block is released,
	// not dropped. CreatePrefix() itself still counts only live prefixes.
	struct FRetiringPrefix
	{
		int32 PinnedLane = INDEX_NONE;
		bool bJobInFlight = false; // a Begin or prefill job posted before the release has not delivered

		// Budget mode (plugin paging plan §3.3): a prefix released before its build applied keeps
		// the build's open ticket here; the release its Begin's delivery queues fails the build
		// once the engine has freed the pages (FPrefixAdminOp::bReleaseFailsBuild). The two hold ids
		// are the release's own (§3.9), taken at ReleasePrefix() and handed to that release op.
		SuperSLMPaging::FTicket BuildTicket = SuperSLMPaging::InvalidTicket;
		int64 ReleaseStartHoldId = 0;
		int64 ReleaseApplyHoldId = 0;
	};
	TMap<int64, FRetiringPrefix> RetiringPrefixes;

	int64 NextOrdinal = 1;   // D-SLM7428: global request ordinal, cross-sequence admission order
	int64 NextHandleId = 1;
	int64 NextJobId = 1;
	int32 TickCounter = 0;
	// D-SLM7799: the tick horizon (kTickHorizon). Once reached, TickCounter stops advancing, every
	// live sequence is faulted with TickHorizonMessage, and new requests are refused with it until
	// Configure() builds a new state (which resets the counter).
	bool bTickHorizonReached = false;
	FString TickHorizonMessage;
#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride(): the status the next reset job
	// returns instead of calling Layer 1, taken once when that job is dispatched. SSLM_OK is unset.
	// Game thread.
	sslm_status TestNextResetStatus = SSLM_OK;
#endif

	// ---- Schema fast-decode plan rev 1.3.5, §6 slices P1 and P1-X: schema jump-forward ----------
	//
	// The walk table: the model's SCM1 section, parsed in place (§6 P1-X "The walk"; the ABI has no
	// walk-state query). Rev 1.3.6 (review finding 1, §6 P1 *Lazy*): Configure() only keeps the
	// captured artifact bytes (WalkArtifact, which pins them); the first generation that is both
	// schema-bound and in a jump mode other than Off asks for the table (RequestSchemaWalkLoad), a
	// pool thread finds the section through the artifact's own section table and parses it over the
	// pinned bytes (no re-hash, no copy, no whole-model load), and the game thread publishes it at
	// the next Tick. Until then the walk is unknown. A configuration whose generations are all Off
	// never loads it.
	SuperSLMRuntime::FModelBytes WalkArtifact;
	FSchemaWalkTableRef WalkTable;            // null until published (or when the load failed)
	FSchemaWalkLoadRef WalkLoadInFlight;       // the pool task's output, until published
	bool bWalkLoadRequested = false;           // once per state: a failed load is not retried
#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMSchemaJumpReviewTestAccess: the loads since this state began, the hold, and whether a
	// held load waits for its release.
	TArray<FSuperSLMSchemaWalkLoadRecord> WalkLoadRecords;
	bool bHoldWalkLoad = false;
	bool bWalkLoadPending = false;
	// The forcedness reads made since StartRecording(), and the ground-truth counter of Canonical
	// decode jobs planned while the engine reported a chain (review finding 4).
	TArray<FSuperSLMForcednessReadRecord> ForcednessReadRecords;
	int32 CanonicalOpenChainDecodes = 0;
#endif

	// The walk table's schemas; an empty table (every ByIndex/ByName misses) until it is ready.
	const superslm::SchemaMasksTable& WalkSchemas() const
	{
		static const superslm::SchemaMasksTable Empty;
		return WalkTable.IsValid() ? WalkTable->Schemas : Empty;
	}
	// The learned drafts (D2-online), per (this mapping, schema): immutable snapshots, merged and
	// swapped here on the game thread, dropped with this state (the model's unmap, P1X.11).
	SuperSLMCore::FDraftTableStore DraftTables;
	// Per schema index: the forced flags read so far (sslm_seq_forced_tokens, cap 0) and drafted
	// through, and the snapshot version, which rises at every swap of that schema's table (P1X.8).
	TMap<int32, SuperSLMCore::FForcednessObservations> ForcedObservations;
	TMap<int32, int64> DraftSnapshotVersions;
	// FSuperSLMSchemaJumpTestAccess::SetForcednessReadsEnabled(): on unless a test turns it off.
	bool bForcednessReadsEnabled = true;
	// The forcedness reads made since the last StartRecording() (or this state's creation).
	int64 ForcednessReadCount = 0;
	// The tick Apply is delivering in (a span job's DeliveredAtTick).
	int32 CurrentTickIndex = 0;
#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMSchemaJumpTestAccess: the span jobs recorded since StartRecording(), and the status
	// the next span job returns instead of calling the verb (0: unset).
	bool bRecordSpanJobs = false;
	TArray<FSuperSLMSpanJobRecord> SpanJobRecords;
	int32 TestNextSpanStatus = 0;
	// FSuperSLMSchemaJumpTestAccess::SetJobHold: per vended sequence (its VendedId), where its next job stops and
	// whether it is stopped there now. ReturnSequence removes the sequence's entry.
	struct FTestJobHold { ESuperSLMJobHoldPoint Point = ESuperSLMJobHoldPoint::None; bool bHeld = false; };
	TMap<int64, FTestJobHold> TestJobHolds;
#endif

	// The table key for a schema of this state's mapping.
	SuperSLMCore::FDraftTableKey DraftKey(int32 SchemaIndex) const
	{
		SuperSLMCore::FDraftTableKey Key;
		Key.Model = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(Model));
		Key.Schema = static_cast<uint32_t>(SchemaIndex);
		return Key;
	}

	// The schema's SCM1 index by name; false when the section does not hold it.
	bool FindSchemaIndex(const FString& SchemaName, int32& OutIndex) const
	{
		const FTCHARToUTF8 Utf8(*SchemaName);
		size_t Index = 0;
		if (WalkSchemas().ByName(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), &Index) == nullptr)
		{
			return false;
		}
		OutIndex = static_cast<int32>(Index);
		return true;
	}

	// Installs Table (null drops it) as SchemaIndex's snapshot; a different pointer is a swap.
	void SwapDraftTable(int32 SchemaIndex, SuperSLMCore::FDraftTableRef Table)
	{
		const SuperSLMCore::FDraftTableKey Key = DraftKey(SchemaIndex);
		if (DraftTables.Snapshot(Key) == Table)
		{
			return;
		}
		DraftTables.Seed(Key, Table);
		if (Table)
		{
			++DraftSnapshotVersions.FindOrAdd(SchemaIndex, -1);
		}
	}

	// The version of the snapshot SchemaIndex's table holds now; -1 with no table.
	int64 DraftSnapshotVersion(int32 SchemaIndex) const
	{
		if (!DraftTables.Snapshot(DraftKey(SchemaIndex)))
		{
			return -1;
		}
		const int64* Version = DraftSnapshotVersions.Find(SchemaIndex);
		return Version ? *Version : -1;
	}

	// ---- Plugin paging plan, step K2: CPU budget mode -----------------------------------------
	//
	// The page ledger (Core/SuperSLMPageLedgerCore.h), configured only when
	// FSuperSLMRuntimeConfig::SequenceBudget > 0. Game thread only: every admission is taken where
	// the request reaches the head of its queue, and every apply at its job's delivery. Legacy
	// mode leaves it unconfigured, so IsBudgetMode() is the one mode switch.
	SuperSLMPaging::FPageLedger Ledger;
	bool IsBudgetMode() const { return Ledger.IsConfigured(); }

	// B and the bytes per page, read from the engine at Configure() (budget mode only).
	int64 PagePositions = 0;
	uint64 PageBytes = 0;

#if WITH_DEV_AUTOMATION_TESTS
	// Every budget-mode prefix's ledger node, by prefix id, kept after ReleasePrefix() so the
	// test seams can read a released prefix as a ghost (§3.9). K2 review F6: the seams are its only
	// reader, so it exists only where they do and never grows in a Shipping session.
	TMap<int64, SuperSLMPaging::FNodeId> PrefixIdToNode;
#endif

	// The engine's sslm_kv_pool_stats as the last applied job that read it saw it (diagnostic;
	// GetKvPageStats()).
	bool bHasPoolSnapshot = false;
	sslm_kv_pool_stats_out LastPoolStats = {};

	// The HoldApply() hold the next PostLedgerJob() attaches to the job it posts; consumed there.
	int64 NextPostApplyHoldId = 0;
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.5: this configuration's prefix-cache glue (SuperSLMSubsystemPrefixCache.h).
	SuperSLMPrefixCacheGlue::FState PrefixCache;
#endif

	// §3.7: the engine returned KV_POOL_EXHAUSTED to one of this plan's own (admitted) requests,
	// which a correct ledger never lets happen. Counted, and logged by name as a ledger fault.
	void NoteLedgerFault(const TCHAR* Verb)
	{
		Ledger.NoteEngineExhaustion();
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM ledger fault: %s returned SSLM_KV_POOL_EXHAUSTED to a request the page ledger admitted (%llu so far); the ledger under-charged it."),
			Verb, static_cast<uint64>(Ledger.LedgerFaults()));
	}

	// K2 review F3: the ledger refused, at the head of a slot's queue, an op it should always admit
	// (a plugin defect). The op is refused by name and never runs untracked; the refusal is counted
	// with the ledger faults and logged as one.
	void NoteLedgerRefusal(int32 SlotIndex, const FString& Reason)
	{
		Ledger.NoteEngineExhaustion();
		UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM ledger fault: slot %d: %s; the op was refused, nothing was posted (%llu so far; plugin defect)."),
			SlotIndex, *Reason, static_cast<uint64>(Ledger.LedgerFaults()));
	}

#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMPagedTestSeam's holds (§3.9), in arming order. A hold is armed by HoldStart() or
	// HoldApply(), attached to the next admitted job of its kind, reached when that job arrives at
	// the hold point, and released by ReleaseHold(). They live and die with this state, so
	// Configure() and Deinitialize() release every one of them.
	struct FPagedHold
	{
		int64 Id = 0;
		EPagedJobKind Kind = EPagedJobKind::Reset;
		bool bApply = false;
		bool bAttached = false;
		bool bReached = false;
		bool bReleased = false;
	};
	TArray<FPagedHold> PagedHolds;

	// SetNextLayer1StatusOverride(): one-shot statuses by request kind, SSLM_OK when unset.
	sslm_status Layer1Overrides[static_cast<int32>(EPagedJobKind::Count)] = {};

	FPagedHold* FindPagedHold(int64 Id)
	{
		return Id != 0 ? PagedHolds.FindByPredicate([Id](const FPagedHold& H) { return H.Id == Id; }) : nullptr;
	}
#endif

	// The oldest armed hold of Kind (start or apply), attached to the job admitted now. 0 when none.
	int64 TakeArmedHold(EPagedJobKind Kind, bool bApply)
	{
#if WITH_DEV_AUTOMATION_TESTS
		for (FPagedHold& Hold : PagedHolds)
		{
			if (Hold.Kind == Kind && Hold.bApply == bApply && !Hold.bAttached && !Hold.bReleased)
			{
				Hold.bAttached = true;
				return Hold.Id;
			}
		}
		return 0;
#else
		(void)Kind;
		(void)bApply;
		return 0;
#endif
	}

	// True while the hold Id holds its job (attached or not, until ReleaseHold()).
	bool IsHoldBlocking(int64 Id)
	{
#if WITH_DEV_AUTOMATION_TESTS
		const FPagedHold* Hold = FindPagedHold(Id);
		return Hold != nullptr && !Hold->bReleased;
#else
		(void)Id;
		return false;
#endif
	}

	void MarkHoldReached(int64 Id)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (FPagedHold* Hold = FindPagedHold(Id))
		{
			Hold->bReached = true;
		}
#else
		(void)Id;
#endif
	}

	// The one-shot Layer-1 status override for the next job of Kind, taken (cleared) now. SSLM_OK
	// when none is set: the job calls Layer 1.
	sslm_status TakeLayer1Override(EPagedJobKind Kind)
	{
#if WITH_DEV_AUTOMATION_TESTS
		sslm_status& Entry = Layer1Overrides[static_cast<int32>(Kind)];
		const sslm_status Status = Entry;
		Entry = SSLM_OK;
		return Status;
#else
		(void)Kind;
		return SSLM_OK;
#endif
	}

	// D-SLM7799: the run-time guards' plugin-defect fault is logged once per configuration.
	bool bSchedulingDefectLogged = false;
	void LogSchedulingDefectOnce(const FString& Detail)
	{
		if (!bSchedulingDefectLogged)
		{
			bSchedulingDefectLogged = true;
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM: %s: %s"), kSchedulingDomainFault, *Detail);
		}
	}

	struct FHandleResult
	{
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		TArray<uint8> Blob; // Save only, until the first successful GetSaveResult() moves it out
		bool bConsumed = false; // row 19 (D-SLM7763): the blob was moved out; later reads say Consumed
	};
	TMap<int64, FHandleResult> HandleResults;
	// Row 19 (D-SLM7763), the pooled-resource monitor's retention reading: the bytes of save blobs
	// held in HandleResults and not yet taken. Kept exact at every move in and out.
	int64 RetainedResultBytes = 0;

	// The most recent ReportHistoryRows() jobs and ticks since Configure(), in rings sized there.
	TReportRing<FSuperSLMWorkerJobReport> JobLedger;
	TReportRing<FSuperSLMTickReport> TickHistory;
	FSuperSLMTickReport LastReport;

	int32 HitchCount = 0;
	double LastResetMs = 0.0;
	double LastAdoptMs = 0.0;
	double LastFinishMs = 0.0;
	double LastPrefillMsPerToken = 0.0;
	int32 TokensFinishedTotal = 0;

	// D-SLM7505/D-SLM7506/D-SLM7509 (plan §5.1): the tokens/second rolling window -- capacity
	// 2048 (fold 9's own justification: generous relative to what this project's pacing helpers
	// actually bound Tick()'s call rate to; not a proven ceiling for an unthrottled production
	// host, which is explicitly underived and safe by graceful degradation, per the plan). Never
	// evicted below one entry; the window degrades to "the oldest sample actually retained" rather
	// than ever computing a wrong-unit figure.
	static constexpr int32 TokensPerSecondWindowCapacity = 2048;
	TArray<FTokensPerSecondSample> TokensPerSecondRing;
	double LastTokensPerSecond = 0.0;

	// Appends this instant's sample and recomputes LastTokensPerSecond. Called once per
	// PublishStats() -- the same point every other CPU stat/counter is (re)computed from current
	// state, matching plan §5.1's "when it is set: once per tick, in PublishStats, the same point
	// every other CPU stat and counter is set."
	void UpdateTokensPerSecond()
	{
		const double NowSeconds = FPlatformTime::Seconds();
		FTokensPerSecondSample NewSample;
		NewSample.RealSeconds = NowSeconds;
		NewSample.TokensFinishedTotal = TokensFinishedTotal;
		TokensPerSecondRing.Add(NewSample);
		if (TokensPerSecondRing.Num() > TokensPerSecondWindowCapacity)
		{
			TokensPerSecondRing.RemoveAt(0, EAllowShrinking::No);
		}

		// The oldest ring-buffer sample whose age is <= 1.0 real second -- ages strictly decrease
		// front-to-back (samples are appended in increasing RealSeconds order), so the first (i.e.
		// smallest-index / oldest) sample satisfying the window bound is exactly what plan §5.1
		// asks for; when even the very oldest retained sample (index 0) already satisfies it, that
		// IS "the oldest sample actually retained" -- both cases fall out of the same scan, no
		// special-casing needed.
		const double WindowSeconds = 1.0;
		int32 OldestIndex = TokensPerSecondRing.Num() - 1;
		for (int32 I = 0; I < TokensPerSecondRing.Num(); ++I)
		{
			if (NowSeconds - TokensPerSecondRing[I].RealSeconds <= WindowSeconds)
			{
				OldestIndex = I;
				break;
			}
		}

		const FTokensPerSecondSample& Oldest = TokensPerSecondRing[OldestIndex];
		const FTokensPerSecondSample& Newest = TokensPerSecondRing.Last();
		const double ElapsedSeconds = Newest.RealSeconds - Oldest.RealSeconds;
		const int32 DeliveredTokens = Newest.TokensFinishedTotal - Oldest.TokensFinishedTotal;
		// Fold 9 (D-SLM7506, closing a T-2879 review note): a gap with no deliveries, fewer
		// than two samples, or an oldest-in-window sample of zero age all collapse to
		// ElapsedSeconds <= 0.0 here -- reads exactly 0.0, never divided, never the stale prior
		// value, never NaN.
		LastTokensPerSecond = ElapsedSeconds > 0.0 ? static_cast<double>(DeliveredTokens) / ElapsedSeconds : 0.0;
	}

	FSlot* Find(int64 Id)
	{
		if (const int32* Index = IdToSlot.Find(Id))
		{
			return &Slots[*Index];
		}
		return nullptr;
	}
	const FSlot* Find(int64 Id) const
	{
		if (const int32* Index = IdToSlot.Find(Id))
		{
			return &Slots[*Index];
		}
		return nullptr;
	}

	int64 MintOrdinal() { return NextOrdinal++; }
	int64 MintHandle(ESuperSLMRestoreResult InitialResult = ESuperSLMRestoreResult::Pending)
	{
		const int64 Id = NextHandleId++;
		HandleResults.Add(Id, FHandleResult{InitialResult, {}});
		return Id;
	}

	void Fault(FSlot& S, ESuperSLMDecodeOutcome Outcome, const FString& Message)
	{
		if (S.Gate.IsValid())
		{
			S.Gate->Stop(); // 1.1: a queued continuation of this generation makes no Layer-1 call
		}
		S.Phase = ESuperSLMSequencePhase::Faulted;
		S.LastOutcome = Outcome;
		S.FaultMessage = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("Sequence %lld faulted: %s"), S.VendedId, *Message);
	}

	void FaultPrefix(FPrefixEntry& P, const FString& Message)
	{
		P.Phase = ESuperSLMPrefixPhase::Faulted;
		P.FaultMessage = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("Prefix faulted: %s"), *Message);
	}

	// The inline bind (plan §5, §2.5 row 22's "at the call, outside Tick()"): runs on the calling
	// thread only when MustHoldRequest() is false -- nothing queued, nothing in flight, no active
	// generation and an empty held list -- so no worker job can hold the handle.
	bool ApplySchemaInline(FSlot& S, const FString& SchemaName, FString& OutError)
	{
		sslm_schema Schema = SSLM_SCHEMA_NONE;
		if (!SchemaName.IsEmpty())
		{
			const sslm_status LookupStatus = sslm_schema_lookup(Model, TCHAR_TO_UTF8(*SchemaName), &Schema);
			if (LookupStatus != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("schema '%s' could not be resolved on the configured model (%s)"), *SchemaName, *StatusText(LookupStatus));
				return false;
			}
		}
		const sslm_status Status = sslm_seq_set_schema(S.Seq, Schema);
		if (Status != SSLM_OK)
		{
			OutError = FString::Printf(TEXT("Layer 1 refused the schema bind (%s)"), *StatusText(Status));
			return false;
		}
		S.BoundSchemaName = SchemaName;
		return true;
	}

	// Releases the registry pin each held adapter entry carries. Entries a posted job carried
	// keep that job's own pin until its delivery (CarryHeldRequests, ConfirmCarried).
	static void ReleaseHeldPins(TArray<FHeldRequest>& Held)
	{
		for (FHeldRequest& Entry : Held)
		{
			if (Entry.bPinned)
			{
				SuperSLMRuntime::UnpinAdapter(Entry.AdapterId);
				Entry.bPinned = false;
			}
		}
	}

	// Plan §2.5 row 20 rule 1 (H1, H2): the only code that ends a hold. The holder's handle stops
	// resolving at once; its held entries are dropped and their pins released (a carried entry's
	// job still runs, and its delivery finds no owner and confirms nothing, rule 2); its queued
	// entries that return no handle (a Generate) are dropped, while its Save/Reset/Adopt ops
	// still drain in arrival order and report through their handles (§5 item 3); the per-user
	// struct is reset by value; and the recycle -- an owner-less reset that also unbinds Layer
	// 1's schema and adapter on the worker (row 17) -- is appended as the slot's last op. The
	// slot is free for a new vend at once (§5 reading 5); every job of the next holder is
	// admitted only after the recycle has delivered (row 22's gates).
	void ReleaseUser(int32 SlotIndex)
	{
		FSlot& Slot = Slots[SlotIndex];
		const int64 Owner = Slot.VendedId;
		if (Owner != 0)
		{
			IdToSlot.Remove(Owner);
		}
		Slot.VendedId = 0;
		Slot.OpQueue.RemoveAll([Owner](const FSlotQueuedOp& Op)
		{
			return Op.Kind == ESlotOpKind::Generate && Op.Owner == Owner;
		});
		ReleaseHeldPins(Slot.Held);
		if (Slot.Gate.IsValid())
		{
			Slot.Gate->Stop(); // 1.1: the old holder's queued continuation, if any, skips its call
		}
		static_cast<FSlotUser&>(Slot) = FSlotUser();
		Slot.ClearMirrorsIfDrained();

		FSlotQueuedOp Recycle;
		Recycle.Kind = ESlotOpKind::Reset;
		Recycle.Ordinal = MintOrdinal();
		Recycle.HandleId = 0; // fire-and-forget; no caller reads this handle
		Recycle.Owner = 0;    // rule 2: the recycle has no owner
		Recycle.bUnbind = true;
		Slot.OpQueue.Add(Recycle);
		Slot.MarkBindEligible();

		if (!Slot.bWithheld)
		{
			FreeSlots.AddUnique(SlotIndex);
		}
	}

	// Plan §2.5 row 17: a recycle Layer 1 refused leaves the sequence in an unknown state or
	// still bound, so the slot is never vended again until the subsystem is torn down.
	void WithholdSlot(int32 SlotIndex, const FString& Reason)
	{
		FSlot& Slot = Slots[SlotIndex];
		Slot.bWithheld = true;
		FreeSlots.Remove(SlotIndex);
		UE_LOG(LogSuperSLM, Error, TEXT("Slot %d withheld from vending until teardown: %s%s"), SlotIndex, *Reason,
			Slot.VendedId != 0 ? TEXT(" (it was already vended again; it is withheld once that holder returns it)") : TEXT(""));
	}

	void PublishStats(const FSuperSLMTickReport& Report)
	{
		UpdateTokensPerSecond();

		int32 InFlight = 0;
		for (const FSlot& S : Slots)
		{
			InFlight += (S.VendedId != 0 && (S.Phase == ESuperSLMSequencePhase::Prefilling || S.Phase == ESuperSLMSequencePhase::Decoding)) ? 1 : 0;
		}
		SET_DWORD_STAT(STAT_SuperSLMPoolOccupied, IdToSlot.Num());
		SET_DWORD_STAT(STAT_SuperSLMPoolFree, FreeSlots.Num());
		SET_DWORD_STAT(STAT_SuperSLMWorkspaces, Lanes.Num());
		SET_DWORD_STAT(STAT_SuperSLMInFlight, InFlight);
		SET_DWORD_STAT(STAT_SuperSLMPrefixes, Prefixes.Num());
		SET_DWORD_STAT(STAT_SuperSLMHitches, HitchCount);
		SET_FLOAT_STAT(STAT_SuperSLMLastTickMs, Report.DurationMs);
		SET_FLOAT_STAT(STAT_SuperSLMLastResetMs, LastResetMs);
		SET_FLOAT_STAT(STAT_SuperSLMLastAdoptMs, LastAdoptMs);
		SET_FLOAT_STAT(STAT_SuperSLMLastFinishMs, LastFinishMs);
		SET_FLOAT_STAT(STAT_SuperSLMPrefillMsPerToken, LastPrefillMsPerToken);
		SET_FLOAT_STAT(STAT_SuperSLMTokensPerSecond, LastTokensPerSecond);
		CSV_CUSTOM_STAT(SuperSLM, TickMs, Report.DurationMs, ECsvCustomStatOp::Set);

		// _ALWAYS, not TRACE_COUNTER_SET: TCounter::Set() emits only when the value changes against
		// the counter's process-lifetime cached value (CountersTrace.h), so a figure that never moves
		// (K at 1, HitchCount at 0) or that settled before a capture began records no sample in that
		// capture at all. Once per tick, and a no-op branch when CountersChannel is off.
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_HitchCount, HitchCount);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_RetainedResultBytes, RetainedResultBytes);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_TokensFinishedTotal, TokensFinishedTotal);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_DueJobs, Report.JobsPlannedThisTick);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_DeliveredJobs, Report.JobsDeliveredThisTick);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_HostFinishMs, LastFinishMs);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_PrefillMsPerToken, LastPrefillMsPerToken);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_TokensPerSecond, LastTokensPerSecond);
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_PoolOccupied, IdToSlot.Num());
		TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_PoolFree, FreeSlots.Num());
		if (const FSuperSLMWorkerJobReport* LastJob = JobLedger.Last())
		{
			TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_LayersPerJob, LastJob->DecodeLayers);
			TRACE_COUNTER_SET_ALWAYS(SuperSLM_CPU_K, LastJob->K);
		}
	}
};

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

// Review W5: one BeginConfigure()'s hand-off from its pool-thread prepare to whoever claims the
// state it built -- the game-thread continuation, or Deinitialize() when the subsystem goes away
// first (at exit the continuation may never run).
struct FSuperSLMCpuConfigureFlight
{
	FCriticalSection Lock;
	FSuperSLMSubsystemState* Prepared = nullptr; // set by the worker; taken by the first claimant
	FEventRef Done{EEventMode::ManualReset};     // triggered once the worker has set Prepared

	FSuperSLMSubsystemState* Claim()
	{
		FScopeLock ScopeLock(&Lock);
		FSuperSLMSubsystemState* Taken = Prepared;
		Prepared = nullptr;
		return Taken;
	}
};

static void DestroyCpuState(FSuperSLMSubsystemState* S);

void USuperSLMSubsystem::Deinitialize()
{
	// TearDown() first, so every continuation still queued finds its serial stale and discards.
	TearDown();
	// Plugin paging plan §3.5 (K3): the world's end drops every tombstone.
	LifecycleTombstones.Empty();
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.11 (a): the cache host outlives each configuration, not the subsystem.
	delete PrefixCacheHost;
	PrefixCacheHost = nullptr;
#endif
	// Review W5: a prepare still running is waited for (its work is bounded: the map, the
	// bandwidth measurement and the allocations), then its state is freed here, synchronously,
	// since the pool task that would otherwise free it may not run at exit.
	for (const TSharedPtr<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe>& Flight : InFlightConfigures)
	{
		Flight->Done->Wait();
		if (FSuperSLMSubsystemState* Orphan = Flight->Claim())
		{
			DestroyCpuState(Orphan);
		}
	}
	InFlightConfigures.Empty();
	Super::Deinitialize();
}

void USuperSLMSubsystem::BeginDestroy()
{
	TearDown();
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.11 (a): the cache host outlives each configuration, not the subsystem.
	delete PrefixCacheHost;
	PrefixCacheHost = nullptr;
#endif
	Super::BeginDestroy();
}

// Fold-round ruling 1: TearDown()'s body, over a state pointer rather than the subsystem's own
// member, so a state a worker prepared (BeginConfigure()) can be unwound on a failure or a
// superseded load without ever having been published. It touches only S and Layer 1, never the
// UObject, so it may run on any thread that owns S outright. OutTombstones (TearDown() only)
// receives the handle ids a budget-mode epoch leaves Pending (plugin paging plan §3.5, K3).
static void DestroyCpuStateContents(FSuperSLMSubsystemState& S, TArray<int64>* OutTombstones = nullptr)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PoolFree", SuperSLMChannel);

#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.6 / cell 1.7: before anything is delivered, the cache marks its Pending leases, so a build
	// or restore that is running now reads Cancelled after the next Configure() (code review r2 B1).
	SuperSLMPrefixCacheGlue::TeardownBegins(S);
#endif

	// Plan §2.5 row 19 (D-SLM7763, U1 round 8): the one place a queued restore op is dropped
	// without being dispatched is here, with the state -- ReleaseUser() removes only Generate
	// ops, so every other restore op reaches DispatchRestore(), whose worker closure releases the
	// blob. Each undispatched op's blob is handed to a worker as a release-only job, enqueued
	// before the workers are stopped below; a worker drains its queue before it exits, so the
	// last release of every restore blob runs on a worker thread, never on this one.
	{
		FSuperSLMCpuWorkerRunnable* Releaser = nullptr;
		for (FLane& Lane : S.Lanes)
		{
			if (Lane.Thread != nullptr && Lane.Runnable != nullptr)
			{
				Releaser = Lane.Runnable;
				break;
			}
		}
		if (Releaser != nullptr)
		{
			for (FSlot& Slot : S.Slots)
			{
				for (FSlotQueuedOp& Op : Slot.OpQueue)
				{
					if (Op.Kind != ESlotOpKind::Restore || !Op.RestoreBlob.IsValid())
					{
						continue;
					}
					FQueuedWorkerJobRef ReleaseJob = MakeShared<FQueuedWorkerJob, ESPMode::ThreadSafe>();
					ReleaseJob->Execute = [Held = MoveTemp(Op.RestoreBlob)]() mutable { Held.Reset(); };
					Releaser->Enqueue(MoveTemp(ReleaseJob));
				}
			}
		}
	}

	// Stop every worker thread first: Kill(bShouldWait=true) blocks until the thread's Run()
	// loop actually exits, which happens only after the worker has executed every job already
	// enqueued on it (FSuperSLMCpuWorkerRunnable::Run drains before it honours the stop) -- so no
	// workspace or pool buffer is ever freed while a worker might still be reading it, and every
	// job posted to a lane has run by the time the delivery pass below reads its output.
	for (FLane& Lane : S.Lanes)
	{
		if (Lane.Thread != nullptr)
		{
			Lane.Thread->Kill(/*bShouldWait*/ true);
			delete Lane.Thread;
			Lane.Thread = nullptr;
		}
		if (Lane.Runnable != nullptr)
		{
			if (Lane.Runnable->WakeEvent != nullptr)
			{
				FPlatformProcess::ReturnSynchEventToPool(Lane.Runnable->WakeEvent);
			}
			delete Lane.Runnable;
			Lane.Runnable = nullptr;
		}
	}

	// Deliver every job the workers executed that Tick() never got to apply, before any handle
	// is released. A job's Execute (worker) and Deliver (game thread) are the two halves of one
	// handle change: DispatchRestore's Execute releases the reserved slot's warm sequence and
	// creates the restored one, and only its Deliver moves FSlot::Seq to the new handle;
	// DispatchReset's create for a slot whose sequence a failed restore released, and
	// DispatchPrefixAdmin's PrefixBegin, have the same shape. Dropping the Deliver half left FSlot::Seq naming a handle the worker had
	// already released, and the release loop below then released it a second time -- the
	// EXCEPTION_ACCESS_VIOLATION in sslm_seq_release (D-SLM7553) whenever the freed object's
	// memory had been reused. Delivered in each lane's committed order, exactly as Apply would
	// have; Apply's ledger/hitch bookkeeping is skipped, since the state it describes is about to
	// be destroyed. A job that did not run is not delivered: its Execute never touched Layer 1,
	// so the slot's or prefix's own handle is still the live one.
	for (FLane& Lane : S.Lanes)
	{
		for (FLanePendingDelivery& Pending : Lane.Pending)
		{
			if (!Pending.WorkerJob.IsValid() || !Pending.WorkerJob->bDone.load(std::memory_order_acquire))
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Teardown: a posted worker job did not run before its lane stopped; its delivery and every later one on that lane are skipped."));
				break;
			}
			if (Pending.Deliver)
			{
				Pending.Deliver(Pending.WorkerJob->WorkerCallMs);
			}
		}
		Lane.Pending.Reset();
	}

	// Plan §2.5 row 20 rule 4: every held request still pinning an adapter releases its pin, so
	// the adapter can be released after this subsystem is gone. (The deliveries above released
	// each carrying job's own pins.)
	for (FSlot& Slot : S.Slots)
	{
		FSuperSLMSubsystemState::ReleaseHeldPins(Slot.Held);
	}

#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.11 (a) L1: the cache's lane jobs not yet posted run, and its engine handles
	// are released, before the subsystem releases its own and destroys the pool.
	SuperSLMPrefixCacheGlue::BeforeHandleRelease(S);
#endif

	// Order Layer 1 accepts with sequences still vended and mid-generation: every sequence and
	// every prefix is released first (sslm_seq_release is legal mid-token and drops its adapter
	// reference; a prefix holds a model reference), so the pool and the model have no live
	// handles when they are destroyed and unmapped.
	bool bHandlesReleased = true;
	for (FSlot& Slot : S.Slots)
	{
		if (Slot.Seq != nullptr)
		{
			const sslm_status Status = sslm_seq_release(Slot.Seq);
			if (Status != SSLM_OK)
			{
				bHandlesReleased = false;
				UE_LOG(LogSuperSLM, Error, TEXT("Teardown: sslm_seq_release failed (%s)."), *StatusText(Status));
			}
			Slot.Seq = nullptr;
		}
	}
	for (TPair<int64, FPrefixEntry>& Pair : S.Prefixes)
	{
		if (Pair.Value.Handle != nullptr)
		{
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::NoteRelease(S, Pair.Value.Handle);
#endif
			const sslm_status Status = sslm_prefix_release(Pair.Value.Handle);
			if (Status != SSLM_OK)
			{
				bHandlesReleased = false;
				UE_LOG(LogSuperSLM, Error, TEXT("Teardown: sslm_prefix_release failed (%s)."), *StatusText(Status));
			}
			Pair.Value.Handle = nullptr;
		}
	}
	// T-2815 round 13: ReleasePrefix()'s real-block path removes the caller-visible entry from
	// S.Prefixes synchronously, so a Release op that was queued but never actually admitted and
	// dispatched (e.g. the subsystem torn down before enough ticks drained it) has a real,
	// never-released Layer-1 prefix handle that the loop above cannot see -- it is no longer in
	// S.Prefixes at all. Every worker thread has already been stopped above (Kill(bShouldWait) runs
	// each worker's OWN queue to completion first, FSuperSLMCpuWorkerRunnable::Run), so any Release
	// op that WAS admitted and posted to a lane has already had its real sslm_prefix_release call
	// run; only a still-queued (never-dispatched) op's handle can still be live here.
	for (const FPrefixAdminOp& Op : S.PrefixAdminQueue)
	{
		if (Op.Kind == EPrefixAdminKind::Release && Op.ReleaseHandle != nullptr)
		{
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::NoteRelease(S, Op.ReleaseHandle);
#endif
			const sslm_status Status = sslm_prefix_release(Op.ReleaseHandle);
			if (Status != SSLM_OK)
			{
				bHandlesReleased = false;
				UE_LOG(LogSuperSLM, Error, TEXT("Teardown: sslm_prefix_release (queued) failed (%s)."), *StatusText(Status));
			}
		}
	}
	for (FLane& Lane : S.Lanes)
	{
		if (Lane.Workspace != nullptr)
		{
			sslm_workspace_destroy(Lane.Workspace);
		}
		if (Lane.WorkspaceBuffer != nullptr)
		{
			FMemory::Free(Lane.WorkspaceBuffer);
		}
		if (Lane.SaveStaging != nullptr)
		{
			FMemory::Free(Lane.SaveStaging);
		}
	}
	if (S.Pool != nullptr)
	{
#if SUPERSLM_WITH_PREFIX_CACHE
		SuperSLMPrefixCacheGlue::BeforePoolDestroy(S);
#endif
		const sslm_status Status = sslm_kv_pool_destroy(S.Pool);
#if SUPERSLM_WITH_PREFIX_CACHE
		SuperSLMPrefixCacheGlue::AfterPoolDestroy(S, Status);
#endif
		if (Status == SSLM_OK)
		{
			FMemory::Free(S.KvBuffer);
		}
		else
		{
			// A live handle still points into the pool's buffer; leaking it is the only safe
			// disposition.
			UE_LOG(LogSuperSLM, Error, TEXT("Teardown: sslm_kv_pool_destroy refused (%s); the KV buffer is leaked rather than freed under a live handle."), *StatusText(Status));
		}
	}
	else if (S.KvBuffer != nullptr)
	{
		FMemory::Free(S.KvBuffer);
	}
	if (bHandlesReleased)
	{
		SuperSLMRuntime::ReleaseModelMapping(S.Model);
	}

	// Plugin paging plan §3.5 "When a lifecycle exit ends a wait" (step K3, cell 1.11): before the
	// entries go, a budget-mode epoch records every entry still Pending -- a waiting restore, a
	// queued request, a job whose apply will not come -- as a tombstone the subsystem keeps, so the
	// game reads NotConfigured, not a Pending it cannot tell from a request still queued. Every job
	// that ran was delivered above, so what is Pending now never resolves. Legacy mode is unchanged.
	if (OutTombstones != nullptr && S.IsBudgetMode())
	{
		for (const TPair<int64, FSuperSLMSubsystemState::FHandleResult>& Entry : S.HandleResults)
		{
			if (Entry.Value.Result == ESuperSLMRestoreResult::Pending)
			{
				OutTombstones->Add(Entry.Key);
			}
		}
	}

	// Plan §2.5 row 19 (D-SLM7763): TearDown() releases every lifecycle-op handle entry, and with
	// them every save blob nobody took.
	S.HandleResults.Empty();
	S.RetainedResultBytes = 0;

	// Schema fast-decode plan rev 1.3.5, §6 P1-X "Lifetime": the learned drafts and the observed
	// flags are keyed by this mapping's schema indices, so they go with the mapping (P1X.11).
	S.DraftTables.DropModel(S.DraftKey(0).Model);
	S.ForcedObservations.Empty();
	S.DraftSnapshotVersions.Empty();

	// Plugin paging plan §3.5 "Fit and wait", cell 1.11: the page ledger's epoch ends with the
	// pool. Every job that ran has been delivered above (and applied its ticket); every op still
	// queued, parked at a test hold or never dispatched, is dropped with this state, and its
	// charge with the epoch. LedgerFaults survives (TearDown() carries it forward).
	if (S.Ledger.IsConfigured())
	{
		S.Ledger.EndEpoch(SuperSLMPaging::EEpochEnd::Teardown);
	}
}

static void DestroyCpuState(FSuperSLMSubsystemState* S)
{
	if (S != nullptr)
	{
		DestroyCpuStateContents(*S);
		delete S;
	}
}

void USuperSLMSubsystem::TearDown()
{
	// A BeginConfigure() still preparing is superseded: its state is discarded when it arrives.
	++ConfigureSerial;
	bConfigurePending = false;
	if (State == nullptr)
	{
		ConfiguredModel = nullptr;
		return;
	}
	TArray<int64> Tombstones;
	DestroyCpuStateContents(*State, &Tombstones);
	LifecycleTombstones.Append(Tombstones); // plugin paging plan §3.5 (K3)
	// Plugin paging plan §3.7: the ledger-fault counter is cumulative across re-Configure().
	PriorLedgerFaults += State->Ledger.LedgerFaults();
	delete State;
	State = nullptr;
	ConfiguredModel = nullptr;
}

// Schema fast-decode plan rev 1.3.6, review finding 1 (§6 P1 *Lazy*; P1.10): the walk table's load.
// It runs on a pool thread, never on the game thread and never inside Configure(): it finds the
// SchemaMasks section through the captured artifact's own section table (the mapping already
// validated and hashed the artifact) and parses it in place over the pinned bytes. A section that is
// absent or does not parse, or an allocation failure, leaves jump-forward without a walk (no learned
// draft, no learning; tokens are unchanged) and is logged at publish, never a refusal.
static void BuildSchemaWalkTable(const SuperSLMRuntime::FModelBytes& Artifact, int32 VocabSize, FSchemaWalkLoad& Load)
{
	Load.bAnyOnGameThread = IsInGameThread();
	Load.ArtifactBytes = Artifact.Size;
	try
	{
		const uint8* SectionData = nullptr;
		uint64 SectionSize = 0;
		if (!SuperSLMRuntime::FindArtifactSection(Artifact, static_cast<uint32>(superslm::SslmSectionType::SchemaMasks), SectionData, SectionSize))
		{
			return; // no compiled schemas
		}
		Load.SectionBytes = static_cast<int64>(SectionSize);
		TSharedPtr<FSchemaWalkTable, ESPMode::ThreadSafe> Table = MakeShared<FSchemaWalkTable, ESPMode::ThreadSafe>();
		Table->Pin = Artifact.Pin;
		std::string Err;
		if (!superslm::SchemaMasksTable::Parse(SectionData, SectionSize, static_cast<uint32_t>(FMath::Max(VocabSize, 0)), Table->Schemas, &Err))
		{
			Load.Warning = FString::Printf(TEXT("the SchemaMasks section does not parse here (%s)"), UTF8_TO_TCHAR(Err.c_str()));
			return;
		}
		Load.Table = MoveTemp(Table);
	}
	catch (const std::bad_alloc&)
	{
		Load.Table.Reset();
		Load.Warning = TEXT("out of memory while reading the SchemaMasks section");
	}
}

// Posts the load to the pool. Game thread; the state keeps the output until PublishSchemaWalkLoad.
static void DispatchSchemaWalkLoad(FSuperSLMSubsystemState& S)
{
	FSchemaWalkLoadRef Load = MakeShared<FSchemaWalkLoad, ESPMode::ThreadSafe>();
	S.WalkLoadInFlight = Load;
	SuperSLMRuntime::FModelBytes Artifact = S.WalkArtifact; // the task's own copy: Pin keeps the bytes alive
	const int32 VocabSize = S.Shape.VocabSize;
	Async(EAsyncExecution::ThreadPool, [Load, Artifact = MoveTemp(Artifact), VocabSize]()
	{
		BuildSchemaWalkTable(Artifact, VocabSize, *Load);
		Load->bDone.store(true, std::memory_order_release);
	});
}

// The first generation that is schema-bound and not Off asks for the table; every later ask is a
// no-op (one load per state, a failed one included). Game thread.
static void RequestSchemaWalkLoad(FSuperSLMSubsystemState& S)
{
	if (S.bWalkLoadRequested)
	{
		return;
	}
	S.bWalkLoadRequested = true;
#if WITH_DEV_AUTOMATION_TESTS
	if (S.bHoldWalkLoad)
	{
		S.bWalkLoadPending = true; // FSuperSLMSchemaJumpReviewTestAccess::HoldSchemaWalkLoad (P1.10 (c))
		return;
	}
#endif
	DispatchSchemaWalkLoad(S);
}

// Publishes a finished load at a Tick: the table becomes ready, and the walk is known for the
// generations that begin from then on. Game thread.
static void PublishSchemaWalkLoad(FSuperSLMSubsystemState& S, int32 TickIndex)
{
	if (!S.WalkLoadInFlight.IsValid() || !S.WalkLoadInFlight->bDone.load(std::memory_order_acquire))
	{
		return;
	}
	const FSchemaWalkLoadRef Load = MoveTemp(S.WalkLoadInFlight);
	S.WalkLoadInFlight.Reset();
	S.WalkTable = Load->Table;
	if (!Load->Warning.IsEmpty())
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: %s; schema jump-forward will run without a learned draft."), *Load->Warning);
	}
#if WITH_DEV_AUTOMATION_TESTS
	FSuperSLMSchemaWalkLoadRecord& Record = S.WalkLoadRecords.AddDefaulted_GetRef();
	Record.bAnyOnGameThread = Load->bAnyOnGameThread;
	Record.bInsideConfigure = false;    // requested only by a generation's activation
	Record.ArtifactBytesHashed = 0;     // the section table is read; nothing is hashed
	Record.BytesCopied = 0;             // parsed in place over the pinned bytes
	Record.SectionBytes = Load->SectionBytes;
	Record.ArtifactBytes = Load->ArtifactBytes;
	Record.bWholeModelLoad = false;     // no SslmModel::Load
	Record.bReady = S.WalkTable.IsValid();
	Record.ReadyAtTick = Record.bReady ? TickIndex : -1;
#else
	(void)TickIndex;
#endif
}

// Configure()'s whole body after the teardown (fold-round ruling 1), over captured model bytes
// (SuperSLMRuntime::FModelBytes) rather than the UObject, so it runs on whichever thread calls
// it: the game thread for Configure(), a pool thread for BeginConfigure(). The mapping (the
// whole-file SHA-256 and copy), the lifecycle bandwidth measurement, the KV/workspace/save
// allocations, the lane threads' creation and the warm sequences all happen here. OutState is
// set only on Success; every failure unwinds what it built. Model is null for no model.
//
// Decode-threading D4 (D-SLM8144): Resolved is what the calling thread resolved from Config
// (SuperSLMRuntimeResolve::Resolve) before this runs. Every check and install below that concerns
// the decode mode, the task count or a cost reads Resolved; Config supplies the call shape. The
// checks keep their order and messages, now over the resolved values.
static FSuperSLMConfigureReport PrepareCpuState(const SuperSLMRuntime::FModelBytes* Model, const FSuperSLMRuntimeConfig& Config,
	const FSuperSLMResolvedCpuConfig& Resolved, FSuperSLMSubsystemState*& OutState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PoolAlloc", SuperSLMChannel);
	OutState = nullptr;

	FSuperSLMConfigureReport Report;
	auto Refuse = [&Report](ESuperSLMConfigureResult Result, const FString& Message)
	{
		Report.Result = Result;
		Report.Message = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("Configure refused: %s"), *Message);
		return Report;
	};

	if (Config.BlockCount <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("block_count must be at least 1 (got %d): it is the number of sequences the KV pool keeps resident at once"), Config.BlockCount));
	}
	if (Config.PrefixBlockCount < 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("PrefixBlockCount must be 0 or more (got %d): it is the number of shared-prefix blocks the KV pool reserves"), Config.PrefixBlockCount));
	}
	// The pool is BlockCount + PrefixBlockCount blocks (below); the sum is taken in int64 so a pin
	// near INT32_MAX is refused by name here rather than wrapping into an unnamed sizing refusal.
	if (static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount) > static_cast<int64>(MAX_int32))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("block_count (BlockCount %d) + PrefixBlockCount (%d) = %lld exceeds %d: the KV pool holds both, so lower one of them"),
			Config.BlockCount, Config.PrefixBlockCount, static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount), MAX_int32));
	}
	if (Config.MaxSequencesPerDecodeCall <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_batch (MaxSequencesPerDecodeCall) must be at least 1 (got %d)"), Config.MaxSequencesPerDecodeCall));
	}
	if (Config.MaxPrefillChunkBudget <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_chunk_budget (MaxPrefillChunkBudget) must be at least 1 (got %d)"), Config.MaxPrefillChunkBudget));
	}
	if (Config.MaxLayerBudget <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_layer_budget (MaxLayerBudget) must be at least 1 (got %d)"), Config.MaxLayerBudget));
	}
	// Slice 1P (tiled-matmul plan §4A.1 item 1): the chunk floor is a count of tokens; 1 is 1.0's
	// prefill, and nothing below 1 means anything.
	if (Config.MinPrefillChunk <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MinPrefillChunk must be at least 1 (got %d); 1 restores one-token prefill jobs, 8 reaches the tiled GEMM"), Config.MinPrefillChunk));
	}
	if (Config.MaxConcurrentCalls <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MaxConcurrentCalls must be at least 1 (got %d)"), Config.MaxConcurrentCalls));
	}
	// D-SLM7799 rule 1: a non-finite TickBudgetMs (NaN, +inf or -inf) is refused like a non-positive one.
	if (!FMath::IsFinite(Config.TickBudgetMs) || Config.TickBudgetMs <= 0.0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("TickBudgetMs must be a finite, positive number of milliseconds (got %f)"), Config.TickBudgetMs));
	}
	if (!(Config.BudgetHeadroom > 0.0 && Config.BudgetHeadroom <= 1.0))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("BudgetHeadroom must lie in (0, 1] (got %.3f)"), Config.BudgetHeadroom));
	}
	if (Config.MaxQueuedOperationsPerSequence <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MaxQueuedOperationsPerSequence must be at least 1 (got %d)"), Config.MaxQueuedOperationsPerSequence));
	}
	if (!SuperSLMFinishHook::IsValidTaskCount(Resolved.ParallelTasks))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("FinishParallelTasks must lie in [0, %d] (got %d): it is the most tasks one token finish, one decode projection, or one prefill GEMM is split into; 0 or 1 installs no hook"),
			SuperSLMFinishHook::MaxFinishParallelTasks, Resolved.ParallelTasks));
	}
	// T-2999 S1: the consumer refuses a cost the scheduler cannot size K from, whatever produced
	// it (a caller's config, a saved estimate the resolver accepted, or a hand edit) -- a
	// non-finite or non-positive per-unit cost is refused by name.
	{
		const TPair<const TCHAR*, double> Costs[] = {
			{ TEXT("LayerCostMs"), Resolved.LayerCostMs }, { TEXT("FinishCostMs"), Resolved.FinishCostMs },
			{ TEXT("PromptTokenCostMs"), Resolved.PromptTokenCostMs }, { TEXT("ResetCostMs"), Resolved.ResetCostMs },
			{ TEXT("AdoptCostMs"), Resolved.AdoptCostMs }, { TEXT("SaveCostMs"), Resolved.SaveCostMs },
			{ TEXT("RestoreCostMs"), Resolved.RestoreCostMs }, { TEXT("PrefixBeginCostMs"), Resolved.PrefixBeginCostMs },
			{ TEXT("PrefixReleaseCostMs"), Resolved.PrefixReleaseCostMs },
			// Slice 1P (plan §4A.1 item 2, rev 11.1 audit F6): the batched pair, refused as the
			// chunk-1 pair is.
			{ TEXT("BatchedPromptTokenCostMs"), Resolved.BatchedPromptTokenCostMs },
		};
		for (const TPair<const TCHAR*, double>& Cost : Costs)
		{
			if (!FMath::IsFinite(Cost.Value) || Cost.Value <= 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, positive number of milliseconds (got %f)"), Cost.Key, Cost.Value));
			}
		}
		// D-SLM7793/D-SLM7794: the per-position depth terms may be 0 (a flat cost), never
		// negative or non-finite.
		const TPair<const TCHAR*, double> Slopes[] = {
			{ TEXT("LayerCostPerPositionMs"), Resolved.LayerCostPerPositionMs },
			{ TEXT("PromptTokenCostPerPositionMs"), Resolved.PromptTokenCostPerPositionMs },
			{ TEXT("BatchedPromptTokenCostPerPositionMs"), Resolved.BatchedPromptTokenCostPerPositionMs },
		};
		for (const TPair<const TCHAR*, double>& Slope : Slopes)
		{
			if (!FMath::IsFinite(Slope.Value) || Slope.Value < 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, non-negative number of milliseconds per position (got %f)"), Slope.Key, Slope.Value));
			}
		}
		// Precache U4 (§5.5): the cache restore's per-MiB term, refused as the slopes are.
		if (!FMath::IsFinite(Resolved.RestoreCostPerMiBMs) || Resolved.RestoreCostPerMiBMs < 0.0)
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
				TEXT("RestoreCostPerMiBMs must be a finite, non-negative number of milliseconds per MiB (got %f)"), Resolved.RestoreCostPerMiBMs));
		}
		// Precache plan rev 1.18.11 (§15.31): the cache chunk's price pair, both or neither, in the prompt price's
		// domain (a finite, positive cost per token and a finite, non-negative slope).
		if (Config.PrefixPrefillTokenCostMs.IsSet() != Config.PrefixPrefillTokenCostPerPositionMs.IsSet())
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
				TEXT("PrefixPrefillTokenCostMs and PrefixPrefillTokenCostPerPositionMs are set together or not at all (only %s is set)"),
				Config.PrefixPrefillTokenCostMs.IsSet() ? TEXT("PrefixPrefillTokenCostMs") : TEXT("PrefixPrefillTokenCostPerPositionMs")));
		}
		if (Config.PrefixPrefillTokenCostMs.IsSet() && !SuperSLMCore::IsValidChunkPricePair(SuperSLMCore::FChunkPricePair{
			Config.PrefixPrefillTokenCostMs.GetValue(), Config.PrefixPrefillTokenCostPerPositionMs.GetValue() }))
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
				TEXT("PrefixPrefillTokenCostMs must be a finite, positive number of milliseconds (got %f) and PrefixPrefillTokenCostPerPositionMs a finite, non-negative number of milliseconds per position (got %f)"),
				Config.PrefixPrefillTokenCostMs.GetValue(), Config.PrefixPrefillTokenCostPerPositionMs.GetValue()));
		}
		// Schema fast-decode plan rev 1.3.6 (P1.1): the per-C pairs are refused as the chunk-1 pair is.
		for (int32 C = 2; C <= 7; ++C)
		{
			const double TokenMs = Resolved.PerChunkPromptTokenCostMs[C];
			const double SlopeMs = Resolved.PerChunkPromptTokenCostPerPositionMs[C];
			if (!FMath::IsFinite(TokenMs) || TokenMs <= 0.0 || !FMath::IsFinite(SlopeMs) || SlopeMs < 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("PerChunkPromptTokenCostMs[%d] must be a finite, positive number of milliseconds and PerChunkPromptTokenCostPerPositionMs[%d] a finite, non-negative one (got %f, %f)"),
					C, C, TokenMs, SlopeMs));
			}
		}
	}
	if (Model == nullptr)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidModel, TEXT("no model was given"));
	}

	SuperSLMRuntime::FModelShape Shape;
	FString Error;
	if (!SuperSLMRuntime::ReadModelShape(*Model, Shape, Error))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidModel, FString::Printf(TEXT("model %s: %s"), *Model->Name, *Error));
	}
	if (Config.MaxLayerBudget > Shape.NumHiddenLayers)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_layer_budget %d exceeds the model's num_hidden_layers (%d)"), Config.MaxLayerBudget, Shape.NumHiddenLayers));
	}
	// D-SLM7799 rule 3: the derived domain, now that the model shape is known. The resolver has
	// already replaced a tripping saved estimate with the mode profile where that clears it
	// (D-SLM8145, D-SLM8167); this is the final set's check, and it refuses as before.
	{
		FString DomainError;
		if (!SuperSLMRuntimeResolve::CheckCostDomain(Config, Resolved, Shape.ContextCap, DomainError))
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCostDomain, DomainError);
		}
	}
	// Plugin paging plan §3.1 (cell 5): SequenceBudget is 0 (legacy mode) or a budget in
	// [1, context_cap]; anything else is refused by name before anything is allocated.
	if (Config.SequenceBudget < 0 || static_cast<int64>(Config.SequenceBudget) > Shape.ContextCap)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidSequenceBudget, FString::Printf(
			TEXT("SequenceBudget must lie in [0, %lld] (got %d): 0 keeps legacy mode, and a budget-mode sequence may write at most the model's context_cap (%lld) positions"),
			Shape.ContextCap, Config.SequenceBudget, Shape.ContextCap));
	}
	const bool bBudgetMode = Config.SequenceBudget > 0;
	// Slice 1P (tiled-matmul plan §4A.1 item 5): a warning, never a refusal. A prefill call of fewer
	// than kTiledMinTokens tokens runs the engine's serial GEMM, so a call shape or floor below it
	// gives up the tiled kernel. Every shipped call shape uses 64 or 512.
	if (Config.MaxPrefillChunkBudget < SuperSLMPrefillPlanning::kTiledMinTokens || Config.MinPrefillChunk < SuperSLMPrefillPlanning::kTiledMinTokens)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM Configure: MaxPrefillChunkBudget %d, MinPrefillChunk %d: prefill will not reach the tiled GEMM (a prefill call needs at least %d tokens); tokens are unchanged."),
			Config.MaxPrefillChunkBudget, Config.MinPrefillChunk, SuperSLMPrefillPlanning::kTiledMinTokens);
	}

	FSuperSLMSubsystemState* NewState = new FSuperSLMSubsystemState();
	NewState->Shape = Shape;
	NewState->Config = Config;
	NewState->Resolved = Resolved;
	if (!SuperSLMRuntime::ReadArtifactHash(*Model, NewState->ArtifactHash))
	{
		delete NewState;
		return Refuse(ESuperSLMConfigureResult::InvalidModel, TEXT("the model carries no artifact header"));
	}
	NewState->Model = SuperSLMRuntime::AcquireModelMapping(*Model, Error);
	if (NewState->Model == nullptr)
	{
		delete NewState;
		return Refuse(ESuperSLMConfigureResult::InvalidModel, FString::Printf(TEXT("model %s: %s"), *Model->Name, *Error));
	}

	// From here the state owns a mapping reference; every failure below unwinds through
	// DestroyCpuState() (TearDown()'s own body), which releases it.
	FSuperSLMSubsystemState& S = *NewState;
	auto TearDown = [NewState]() { DestroyCpuState(NewState); };

	S.WalkArtifact = *Model; // the walk table loads lazily, off the game thread (rev 1.3.6, review finding 1)

	// Plugin paging plan §3.2 (step K2): budget mode reads the page geometry from the engine on
	// this thread (the mapping is held) and sizes the page ledger's regions from it, before the
	// lifecycle budget, which prices a budget holder's reservation (§3.3 "Lifecycle pricing").
	if (bBudgetMode)
	{
		S.PagePositions = static_cast<int64>(sslm_kv_page_positions(S.Model));
		S.PageBytes = static_cast<uint64>(sslm_kv_page_size(S.Model));
		SuperSLMPaging::FPageLedgerConfig LedgerConfig;
		LedgerConfig.PagePositions = S.PagePositions;
		LedgerConfig.PageBytes = S.PageBytes;
		LedgerConfig.ContextCap = Shape.ContextCap;
		LedgerConfig.EngineContextCap = 0; // the CPU's two caps are one: Cp = Ce (§3.2 "Two caps")
		LedgerConfig.SequenceBudget = Config.SequenceBudget;
		LedgerConfig.BlockCount = Config.BlockCount;
		LedgerConfig.PrefixBlockCount = Config.PrefixBlockCount;
		LedgerConfig.RestoreHeadroomBlocks = Config.RestoreHeadroomBlocks;
		LedgerConfig.CacheGhostBlocks = Config.CacheGhostBlocks;
		LedgerConfig.bCacheConfigured = false; // no prefix cache in the paging release: A and C are 0
		LedgerConfig.CacheRegionPages = 0;
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.11 (b): with a cache, region C is its budget in whole blocks of pages.
		SuperSLMPrefixCacheGlue::ConfigureRegionC(Config, S.Model, S.PagePositions, Shape.ContextCap, LedgerConfig);
#endif
		const SuperSLMPaging::ELedgerConfigure LedgerResult = S.Ledger.Configure(LedgerConfig);
		switch (LedgerResult)
		{
			case SuperSLMPaging::ELedgerConfigure::Ok:
				break;
			case SuperSLMPaging::ELedgerConfigure::InvalidSequenceBudget:
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidSequenceBudget, FString::Printf(
					TEXT("SequenceBudget %d is outside [1, context_cap %lld]"), Config.SequenceBudget, Shape.ContextCap));
			case SuperSLMPaging::ELedgerConfigure::InvalidGeometry:
			{
				const FString Message = FString::Printf(
					TEXT("the page ledger refused the geometry (B %lld, page %llu bytes, context_cap %lld, BlockCount %d, PrefixBlockCount %d, RestoreHeadroomBlocks %d, CacheGhostBlocks %d): every count must be 0 or more and the pool must fit 2^32 - 1 pages"),
					S.PagePositions, S.PageBytes, Shape.ContextCap, Config.BlockCount, Config.PrefixBlockCount, Config.RestoreHeadroomBlocks, Config.CacheGhostBlocks);
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, Message);
			}

			case SuperSLMPaging::ELedgerConfigure::LegacyMode:
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("the page ledger read SequenceBudget %d as legacy mode (plugin defect)"), Config.SequenceBudget));
		}
	}

	// §5: the Sequence Lifecycle Budget is checked BEFORE anything is allocated. The same figure
	// SuperSLMSequenceLifecycleBudget::CheckModel() computes -- Layer 1's block size on the shared
	// mapping, over this box's measured bandwidths -- read from the mapping already held. Plugin
	// paging plan §3.3 "Lifecycle pricing" (cell 7.1): in budget mode a create or reset is priced
	// at R(SequenceBudget) x page_bytes, the reservation it touches, in place of the block size.
	const uint64 BudgetPricedBytes = bBudgetMode
		? SuperSLMPaging::BudgetModeLifecyclePricedBytes(S.PagePositions, Shape.ContextCap, Config.SequenceBudget, S.PageBytes)
		: 0;
	const int64 LifecyclePricedBytes = bBudgetMode ? static_cast<int64>(BudgetPricedBytes) : static_cast<int64>(sslm_kv_block_size(S.Model));
	double WriteBytesPerSec = 0.0;
	double AdoptToResetRatio = 0.0;
	SuperSLMSequenceLifecycleBudget::MeasureHostBandwidthAndAdoptRatio(WriteBytesPerSec, AdoptToResetRatio);
	const FSuperSLMSequenceLifecycleReport Lifecycle = SuperSLMSequenceLifecycleBudget::Predict(
		LifecyclePricedBytes, WriteBytesPerSec, AdoptToResetRatio, Config.SequenceLifecycleBudgetMs);
	if (!Lifecycle.bWithinBudget)
	{
		const FString Message = bBudgetMode
			? FString::Printf(
				TEXT("predicted reset %.2f ms / adopt %.2f ms for a %lld-byte KV reservation (R(SequenceBudget %d) = %lld pages of %llu bytes) exceed the Sequence Lifecycle Budget of %.2f ms. ")
				TEXT("The reservation grows with SequenceBudget: lower it, or raise the budget."),
				Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs, Lifecycle.KvBlockSizeBytes, Config.SequenceBudget,
				S.PageBytes > 0 ? static_cast<int64>(BudgetPricedBytes / S.PageBytes) : int64(0), S.PageBytes, Config.SequenceLifecycleBudgetMs)
			: FString::Printf(
				TEXT("predicted reset %.2f ms / adopt %.2f ms for a %lld-byte KV block exceed the Sequence Lifecycle Budget of %.2f ms. ")
				TEXT("The block size is linear in context_cap (%lld for this model): reconvert at a smaller context_cap, or raise the budget."),
				Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs, Lifecycle.KvBlockSizeBytes, Config.SequenceLifecycleBudgetMs, Shape.ContextCap);
		TearDown();
		return Refuse(ESuperSLMConfigureResult::SequenceLifecycleBudgetExceeded, Message);
	}

	// §4: sizing is a function of the artifact and the declared call shape.
	sslm_config CallShape;
	CallShape.max_batch = Config.MaxSequencesPerDecodeCall;
	CallShape.max_chunk_budget = Config.MaxPrefillChunkBudget;
	CallShape.max_layer_budget = Config.MaxLayerBudget;
	CallShape.reserved = 0;

	// The pool holds the warm sequences plus the shared-prefix blocks (D-SLM7342). The sum is
	// computed in int64; the range check above refused anything past INT32_MAX.
	const int64 PoolBlocks64 = static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount)
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.4: in legacy mode the pool gains ceil(budget / block bytes) blocks for the cache.
		+ (bBudgetMode ? int64(0) : SuperSLMPrefixCacheGlue::LegacyCacheBlocks(Config, S.Model))
#endif
		;
	check(PoolBlocks64 > 0 && PoolBlocks64 <= static_cast<int64>(MAX_int32));
	const int32 PoolBlocks = static_cast<int32>(PoolBlocks64);
	const size_t BlockBytes = sslm_kv_block_size(S.Model);
	const size_t OverheadBytes = bBudgetMode ? 0 : sslm_kv_pool_overhead_size(S.Model, static_cast<uint32_t>(PoolBlocks));
	const size_t WorkspaceBytes = sslm_workspace_size(S.Model, &CallShape);
	const size_t StateBytes = sslm_seq_state_size(S.Model);
	if (BlockBytes == 0 || WorkspaceBytes == 0 || StateBytes == 0 ||
		(!bBudgetMode && (OverheadBytes == SIZE_MAX || BlockBytes > (SIZE_MAX - OverheadBytes) / static_cast<size_t>(PoolBlocks))))
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, TEXT("Layer 1 could not size the pool/workspace for this model and call shape"));
	}

	// Plugin paging plan §3.2: in budget mode the pool is a page pool of exactly the ledger's
	// S + P + H (+ A + C, 0 here) pages, so the engine's own KV_POOL_EXHAUSTED is reachable only
	// past a charge the ledger got wrong (a ledger fault, §3.7).
	const uint32 PoolPages = bBudgetMode ? S.Ledger.PoolPages() : 0u;
	const size_t PageBytes = bBudgetMode ? static_cast<size_t>(S.PageBytes) : 0;
	const size_t PageOverheadBytes = bBudgetMode ? sslm_kv_page_pool_overhead_size(S.Model, PoolPages) : 0;
	if (bBudgetMode && (PoolPages == 0 || PageBytes == 0 || PageOverheadBytes == SIZE_MAX ||
		PageBytes > (SIZE_MAX - PageOverheadBytes) / static_cast<size_t>(PoolPages)))
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("Layer 1 could not size a page pool of %u pages of %llu bytes for this model"), PoolPages, static_cast<uint64>(PageBytes)));
	}

	auto AllocFail = [&TearDown, &Refuse](const FString& What)
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::AllocationFailed, What);
	};

	// Legacy: KV = sslm_kv_block_size * blocks + sslm_kv_pool_overhead_size (§4), 64-byte aligned.
	// Budget mode: pages * sslm_kv_page_size + sslm_kv_page_pool_overhead_size, as
	// sslm_kv_page_pool_create requires.
	S.KvBytes = bBudgetMode
		? static_cast<int64>(PageBytes * static_cast<size_t>(PoolPages) + PageOverheadBytes)
		: static_cast<int64>(BlockBytes * static_cast<size_t>(PoolBlocks) + OverheadBytes);
	S.KvBuffer = FMemory::Malloc(static_cast<SIZE_T>(S.KvBytes), SSLM_ABI_ALIGNMENT_BYTES);
	if (S.KvBuffer == nullptr)
	{
		return AllocFail(FString::Printf(TEXT("could not allocate the %lld-byte KV pool"), S.KvBytes));
	}
	sslm_status Status = bBudgetMode
		? sslm_kv_page_pool_create(S.Model, S.KvBuffer, static_cast<size_t>(S.KvBytes), PoolPages, &S.Pool)
		: sslm_kv_pool_create(S.Model, S.KvBuffer, static_cast<size_t>(S.KvBytes), static_cast<uint32_t>(PoolBlocks), &S.Pool);
	if (Status != SSLM_OK)
	{
		return AllocFail(FString::Printf(TEXT("%s failed (%s)"), bBudgetMode ? TEXT("sslm_kv_page_pool_create") : TEXT("sslm_kv_pool_create"), *StatusText(Status)));
	}

	// §4, D-SLM7341/D-SLM7407: one persistent worker thread and one workspace per lane; lanes =
	// min(MaxConcurrentCalls, BlockCount).
	const int32 LaneCount = FMath::Clamp(Config.MaxConcurrentCalls, 1, Config.BlockCount);
	S.WorkspaceBytesEach = static_cast<int64>(WorkspaceBytes);
	S.Lanes.SetNum(LaneCount);
	for (int32 I = 0; I < LaneCount; ++I)
	{
		FLane& Lane = S.Lanes[I];
		Lane.WorkspaceBuffer = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
		if (Lane.WorkspaceBuffer == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not allocate a %llu-byte workspace"), static_cast<uint64>(WorkspaceBytes)));
		}
		Status = sslm_workspace_create(S.Model, &CallShape, Lane.WorkspaceBuffer, WorkspaceBytes, &Lane.Workspace);
		if (Status != SSLM_OK)
		{
			return AllocFail(FString::Printf(TEXT("sslm_workspace_create failed (%s)"), *StatusText(Status)));
		}
		// Plan §2.5 row 2 (D-SLM7663): the finish hook, installed right after the workspace is
		// created and before this lane's worker thread starts, so it never runs concurrently with
		// a call on the workspace.
		// The resolved decode mode rides on this hook (D-SLM8166): at 0 or 1 task none is installed,
		// and the effective mode is then off.
		if (SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks))
		{
			const sslm_parallel_for Hook = SuperSLMFinishHook::Make(Resolved.ParallelTasks,
				Resolved.bDecodeModeEffective ? SSLM_PARALLEL_FOR_MATVEC : 0);
			Status = sslm_workspace_set_parallel_for(Lane.Workspace, &Hook);
			if (Status != SSLM_OK)
			{
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("sslm_workspace_set_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"), Resolved.ParallelTasks, *StatusText(Status)));
			}
		}
		// §4: save buffers are sized by sslm_seq_state_size (an upper bound) and created at
		// init, one per lane (plan §2.5 row 22).
		Lane.SaveStaging = FMemory::Malloc(StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
		if (Lane.SaveStaging == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not allocate a %llu-byte save buffer"), static_cast<uint64>(StateBytes)));
		}
		Lane.Runnable = new FSuperSLMCpuWorkerRunnable();
		Lane.Runnable->WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
		Lane.Thread = FRunnableThread::Create(Lane.Runnable, *FString::Printf(TEXT("SuperSLM CPU Worker %d"), I));
		if (Lane.Thread == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not create worker thread %d"), I));
		}
	}

	S.SaveStagingBytes = static_cast<int64>(StateBytes);

	// §5: the warm pool -- every sequence block_count allows is created now, so create and
	// release never run on a gameplay tick.
	S.Slots.SetNum(Config.BlockCount);
	for (int32 I = 0; I < Config.BlockCount; ++I)
	{
		// Plugin paging plan §3.2: in budget mode each warm holder reserves R(SequenceBudget)
		// pages, the slot's share of region S, which the ledger charged in full at its Configure.
		Status = bBudgetMode
			? sslm_seq_create_budgeted(S.Model, &S.Pool, Config.SequenceBudget, &S.Slots[I].Seq)
			: sslm_seq_create(S.Model, &S.Pool, &S.Slots[I].Seq);
		if (Status != SSLM_OK)
		{
			return AllocFail(FString::Printf(TEXT("%s failed for warm sequence %d of %d (%s)"),
				bBudgetMode ? TEXT("sslm_seq_create_budgeted") : TEXT("sslm_seq_create"), I + 1, Config.BlockCount, *StatusText(Status)));
		}
		S.Slots[I].Prompt.Reserve(Config.MaxPrefillChunkBudget);
		S.FreeSlots.Add(I);
	}
	S.IdToSlot.Reserve(Config.BlockCount);
	S.Prefixes.Reserve(Config.PrefixBlockCount);

	// The job ledger and the tick history: fixed rings, created here so a request never grows
	// them. Every ledger row's member list is sized for the largest batch, a job serving every
	// sequence block, so filling a row never allocates either. The tokens-per-second window is
	// sized here for the same reason.
	S.JobLedger.Init(ReportHistoryRows());
	for (FSuperSLMWorkerJobReport& Row : S.JobLedger.Slots())
	{
		Row.MemberSequences.Reserve(Config.BlockCount);
	}
	S.TickHistory.Init(ReportHistoryRows());
	S.TokensPerSecondRing.Reserve(FSuperSLMSubsystemState::TokensPerSecondWindowCapacity + 1);

	S.PublishStats(FSuperSLMTickReport());
	Report.Result = ESuperSLMConfigureResult::Success;
	if (bBudgetMode)
	{
		using SuperSLMPaging::ERegion;
		Report.Message = FString::Printf(
			TEXT("budget mode (SequenceBudget %d): KV page pool %lld bytes (%u pages of %llu bytes, B = %lld: S %u for %d sequences, P %u, H %u; + %llu overhead), %d worker lane(s) with a %lld-byte workspace and a %lld-byte save buffer each, token finish %s; predicted reset %.2f ms, adopt %.2f ms"),
			Config.SequenceBudget, S.KvBytes, PoolPages, static_cast<uint64>(PageBytes), S.PagePositions,
			S.Ledger.RegionCapacity(ERegion::S), Config.BlockCount, S.Ledger.RegionCapacity(ERegion::P), S.Ledger.RegionCapacity(ERegion::H),
			static_cast<uint64>(PageOverheadBytes), S.Lanes.Num(), S.WorkspaceBytesEach, S.SaveStagingBytes,
			SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Resolved.ParallelTasks) : TEXT("serial"),
			Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs);
		OutState = NewState;
		return Report;
	}
	Report.Message = FString::Printf(
		TEXT("KV pool %lld bytes (%d sequence + %d prefix blocks of %llu bytes, + %llu overhead), %d worker lane(s) with a %lld-byte workspace and a %lld-byte save buffer each, token finish %s; predicted reset %.2f ms, adopt %.2f ms"),
		S.KvBytes, Config.BlockCount, Config.PrefixBlockCount, static_cast<uint64>(BlockBytes), static_cast<uint64>(OverheadBytes),
		S.Lanes.Num(), S.WorkspaceBytesEach, S.SaveStagingBytes,
		SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Resolved.ParallelTasks) : TEXT("serial"),
		Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs);
	OutState = NewState;
	return Report;
}

FSuperSLMConfigureReport USuperSLMSubsystem::Configure(USuperSLMModel* Model, const FSuperSLMRuntimeConfig& Config)
{
	// Idempotent: whatever a prior call built is released first, vended sequences included.
	TearDown();

	// Game thread, blocking: PrepareCpuState() runs on this thread. BeginConfigure() is the same
	// work with PrepareCpuState() on a pool thread.
	SuperSLMRuntime::FModelBytes Bytes;
	if (Model != nullptr)
	{
		Bytes = SuperSLMRuntime::CaptureModelBytes(*Model);
	}
	// Decode-threading D4 (D-SLM8144): the mode, task count and costs resolve here, on the calling
	// thread, from Config, this process's selection and the two Saved/SuperSLM INI files.
	const FSuperSLMResolvedCpuConfig Resolved = SuperSLMRuntimeResolve::Resolve(Config, Model != nullptr ? &Bytes : nullptr);
	FSuperSLMSubsystemState* NewState = nullptr;
	FSuperSLMConfigureReport Report = PrepareCpuState(Model != nullptr ? &Bytes : nullptr, Config, Resolved, NewState);
	Report.Resolved = Resolved;
	if (NewState != nullptr)
	{
		NewState->NextHandleId = FirstHandleIdAfterTombstones(); // plugin paging plan §3.5 (K3)
		State = NewState;
		ConfiguredModel = Model;
#if SUPERSLM_WITH_PREFIX_CACHE
		InstallPrefixCache();
#endif
		UE_LOG(LogSuperSLM, Display, TEXT("%s"), *SuperSLMRuntimeResolve::ConfigureLine(Resolved));
	}
	return Report;
}

const FSuperSLMResolvedCpuConfig* USuperSLMSubsystem::GetResolvedConfig() const
{
	return State != nullptr ? &State->Resolved : nullptr;
}

// Precache plan rev 1.18.12 (§15.32): the in-situ prefix_prefill arm (see the declaration).
bool USuperSLMSubsystem::WriteInSituPrefixPrefillPrice(const TArray<FSuperSLMWorkerJobReport>& BuildRows, FSuperSLMInSituPrefixPrefillPrice& Out, FString& OutError)
{
	Out = FSuperSLMInSituPrefixPrefillPrice();
	if (State == nullptr || ConfiguredModel == nullptr)
	{
		OutError = TEXT("WriteInSituPrefixPrefillPrice: no CPU configuration with a model (Configure() first)");
		return false;
	}
	std::vector<SuperSLMCore::FPrefixPrefillReading> Readings;
	for (const FSuperSLMWorkerJobReport& Row : BuildRows)
	{
		if (Row.Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || Row.MemberSequences.Num() != 0 || Row.DeliveredAtTick < 0)
		{
			continue; // not a cache build's chunk, or not delivered
		}
		SuperSLMCore::FPrefixPrefillReading Reading;
		if (SuperSLMCore::ChunkRowReading(Row.PromptTokens, Row.PromptPositionSum, Row.WorkerCallMs, Reading))
		{
			Readings.push_back(Reading);
		}
	}
	Out.ChunkRows = static_cast<int32>(Readings.size());
	Out.MedianWorkerMsPerToken = SuperSLMCore::MedianMsPerToken(Readings.data(), Readings.size());
	SuperSLMCore::FChunkPricePair Pair;
	if (!SuperSLMCore::FitInSituPrefixPrefillPair(Readings, Pair))
	{
		OutError = FString::Printf(TEXT("WriteInSituPrefixPrefillPrice: the build's %d chunk rows of %d tokens or more give no price pair (the fit needs at least %d)"),
			Out.ChunkRows, SuperSLMCore::kCacheChunkTiledMinTokens, SuperSLMCore::kPrefixPrefillFitGroups);
		return false;
	}
	Out.TokenCostMs = Pair.TokenMs;
	Out.TokenCostPerPositionMs = Pair.PerPositionMs;
	Out.TokenCostText = UTF8_TO_TCHAR(SuperSLMCore::RoundUpDecimalText(Pair.TokenMs, 3).c_str());
	Out.TokenCostPerPositionText = UTF8_TO_TCHAR(SuperSLMCore::RoundUpDecimalText(Pair.PerPositionMs, 6).c_str());
	Out.Path = SuperSLMRuntimeResolve::CalibratedCostsPath();
	if (!SuperSLMRuntimeResolve::WriteSavedPrefixPrefillPair(SuperSLMRuntime::HashToHex(State->ArtifactHash), State->Resolved.bDecodeModeEffective,
			State->Resolved.ParallelTasks, Out.TokenCostText, Out.TokenCostPerPositionText, Out.Profile, OutError))
	{
		OutError = TEXT("WriteInSituPrefixPrefillPrice: ") + OutError;
		return false;
	}
	UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM: in-situ prefix_prefill pair %s=%s %s=%s from %d chunk rows (median worker %.3f ms/token), written to %s in %s; the next Configure() reads it."),
		SuperSLMRuntimeResolve::PrefixPrefillTokenKey(), *Out.TokenCostText, SuperSLMRuntimeResolve::PrefixPrefillSlopeKey(), *Out.TokenCostPerPositionText,
		Out.ChunkRows, Out.MedianWorkerMsPerToken, *Out.Profile, *Out.Path);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// FSuperSLMConfigureTestAccess: the release event the next BeginConfigure() pool job waits on.
	// Game thread.
	TSharedPtr<FEventRef, ESPMode::ThreadSafe> GHeldPoolJobRelease;
	bool bGHoldNextPoolJob = false;
}

void FSuperSLMConfigureTestAccess::HoldNextPoolJob()
{
	check(IsInGameThread());
	GHeldPoolJobRelease = MakeShared<FEventRef, ESPMode::ThreadSafe>(EEventMode::ManualReset);
	bGHoldNextPoolJob = true;
}

void FSuperSLMConfigureTestAccess::ReleasePoolJob()
{
	check(IsInGameThread());
	bGHoldNextPoolJob = false;
	if (GHeldPoolJobRelease.IsValid())
	{
		(*GHeldPoolJobRelease)->Trigger();
		GHeldPoolJobRelease.Reset();
	}
}
#endif

void USuperSLMSubsystem::BeginConfigure(USuperSLMModel* Model, const FSuperSLMRuntimeConfig& Config, TUniqueFunction<void(const FSuperSLMConfigureReport&)> OnDone)
{
	check(IsInGameThread());
	// Game thread: the teardown of whatever a prior call built (its lane threads are joined here,
	// idle ones return at once) and the capture of the model's bytes.
	TearDown();
	const uint32 Serial = ConfigureSerial;
	bConfigurePending = true;

	const bool bHasModel = Model != nullptr;
	SuperSLMRuntime::FModelBytes Bytes;
	TSharedPtr<TStrongObjectPtr<USuperSLMModel>> Hold;
	if (bHasModel)
	{
		Bytes = SuperSLMRuntime::CaptureModelBytes(*Model);
		// Keeps the model alive; created here and released in the game-thread continuation below,
		// as SuperSLMModelInspector::InspectModelAsync does. The bytes the worker reads are kept
		// alive by Bytes.Pin (review S5), which a reload of the asset meanwhile does not free.
		Hold = MakeShared<TStrongObjectPtr<USuperSLMModel>>(Model);
	}
	// Decode-threading D4 (D-SLM8144): resolution runs here, on the calling (game) thread, before
	// the pool job is queued; a selection or --save made after this call applies to the next
	// configuration. The pool job reads only Resolved.
	const FSuperSLMResolvedCpuConfig Resolved = SuperSLMRuntimeResolve::Resolve(Config, bHasModel ? &Bytes : nullptr);
	TWeakObjectPtr<USuperSLMSubsystem> WeakThis(this);
	TSharedPtr<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe> Flight = MakeShared<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe>();
	InFlightConfigures.Add(Flight);

	// FSuperSLMConfigureTestAccess::HoldNextPoolJob(): this pool job waits for ReleasePoolJob().
	TSharedPtr<FEventRef, ESPMode::ThreadSafe> HeldRelease;
#if WITH_DEV_AUTOMATION_TESTS
	if (bGHoldNextPoolJob)
	{
		bGHoldNextPoolJob = false;
		HeldRelease = GHeldPoolJobRelease;
	}
#endif

	// Pool thread: PrepareCpuState(), every heavy step of Configure().
	Async(EAsyncExecution::ThreadPool,
		[WeakThis, Serial, bHasModel, Bytes = MoveTemp(Bytes), Hold = MoveTemp(Hold), Flight, Config, Resolved, HeldRelease, OnDone = MoveTemp(OnDone)]() mutable
		{
			if (HeldRelease.IsValid())
			{
				// A test seam only: bounded, so a test that never releases cannot hang the pool.
				(*HeldRelease)->Wait(FTimespan::FromSeconds(120.0));
			}
			FSuperSLMSubsystemState* Built = nullptr;
			FSuperSLMConfigureReport Report = PrepareCpuState(bHasModel ? &Bytes : nullptr, Config, Resolved, Built);
			Report.Resolved = Resolved;
			{
				FScopeLock ScopeLock(&Flight->Lock);
				Flight->Prepared = Built;
			}
			Flight->Done->Trigger();

			// Game thread: publish, or discard when a later Configure()/BeginConfigure()/teardown
			// superseded this one (OnDone is then not called).
			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, Serial, Flight, Report, Hold = MoveTemp(Hold), OnDone = MoveTemp(OnDone)]() mutable
				{
					// Null when Deinitialize() already claimed and freed it (review W5).
					FSuperSLMSubsystemState* NewState = Flight->Claim();
					USuperSLMSubsystem* Self = WeakThis.Get();
					if (Self != nullptr)
					{
						Self->InFlightConfigures.Remove(Flight);
					}
					if (Self == nullptr || Self->ConfigureSerial != Serial || !Self->bConfigurePending)
					{
						if (NewState != nullptr)
						{
							// The unwind joins the idle lane threads and frees the pools: a pool thread's work.
							Async(EAsyncExecution::ThreadPool, [NewState]() { DestroyCpuState(NewState); });
						}
						Hold.Reset();
						return;
					}
					Self->bConfigurePending = false;
					if (NewState != nullptr)
					{
						NewState->NextHandleId = Self->FirstHandleIdAfterTombstones(); // plugin paging plan §3.5 (K3)
						Self->State = NewState;
						Self->ConfiguredModel = Hold.IsValid() ? Hold->Get() : nullptr;
#if SUPERSLM_WITH_PREFIX_CACHE
						Self->InstallPrefixCache();
#endif
						UE_LOG(LogSuperSLM, Display, TEXT("%s"), *SuperSLMRuntimeResolve::ConfigureLine(NewState->Resolved));
					}
					Hold.Reset();
					if (OnDone)
					{
						OnDone(Report);
					}
				});
		});
}

// ---------------------------------------------------------------------------------------------
// Vend / return / reset
// ---------------------------------------------------------------------------------------------

ESuperSLMVendResult USuperSLMSubsystem::VendSequence(FSuperSLMSequence& OutSequence)
{
	OutSequence = FSuperSLMSequence();
	if (State == nullptr)
	{
		return ESuperSLMVendResult::NotConfigured;
	}
	FSuperSLMSubsystemState& S = *State;
	if (S.bTickHorizonReached)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("VendSequence refused: %s"), *S.TickHorizonMessage); // D-SLM7799
		return ESuperSLMVendResult::NotConfigured;
	}
	if (S.FreeSlots.Num() == 0)
	{
		// §4: the plugin's own refusal, before SSLM_KV_POOL_EXHAUSTED can ever arise.
		TRACE_BOOKMARK(TEXT("SuperSLM: pool refusal (%d of %d vended)"), S.IdToSlot.Num(), S.Slots.Num());
		return ESuperSLMVendResult::PoolExhausted;
	}

	const int32 SlotIndex = S.FreeSlots[0];
	S.FreeSlots.RemoveAt(0, EAllowShrinking::No);

	FSlot& Slot = S.Slots[SlotIndex];
	// §18: neither OpQueue nor OutstandingJobCount is touched here. A slot fresh off
	// ReturnSequence() is added to FreeSlots immediately (§5 reading 5: "a returned sequence
	// counts as free immediately"), but its own recycle Reset op may still be queued (or
	// already dispatched and in flight) -- OpQueue.Reset() used to discard that op outright
	// (so the real Layer-1 sslm_seq was never actually reset between generations -- the
	// mechanism behind the add18 content divergence) and OutstandingJobCount = 0 used to
	// stomp the count of jobs genuinely still in flight (so their own later, unconditional
	// Deliver-side decrements drove the count negative). Leaving both alone means: a still-
	// queued Reset stays at the front of OpQueue, so this vend's own BeginGeneration() call
	// correctly appends its Generate request BEHIND it (D-SLM7421's "one in flight at a time,
	// strict arrival order"), activating only once PlanNextJobs step 0 sees Idle, an empty-
	// fronted queue and !HasUndeliveredJob() -- i.e. only after the real reset has actually
	// run. A genuinely idle/fresh slot already has an empty OpQueue and OutstandingJobCount
	// == 0, so this is a no-op for the common case.
	//
	// Plan §2.5 row 20 rule 1 (H5): the per-user struct starts from a value reset, a second
	// guard behind ReleaseUser()'s own.
	static_cast<FSlotUser&>(Slot) = FSlotUser();
	Slot.ClearMirrorsIfDrained();
	// D-SLM7528 (plan §5 item 3): a slot's own next creation resets PinnedLane -- "sticky ... held
	// for the entity's whole life; reset only at the entity's own next creation."
	Slot.PinnedLane = INDEX_NONE;
	Slot.NextAvailableTick = 0;
	Slot.bAwaitingRestore = false;
	// Plan §2.5 row 4: every vended slot is fresh from Configure() or has a reset queued (the
	// recycle), so it will be bind-eligible once its queue drains.
	Slot.MarkBindEligible();
	Slot.VendedId = GNextSequenceId++;
	S.IdToSlot.Add(Slot.VendedId, SlotIndex);
	OutSequence.Id = Slot.VendedId;
	S.PublishStats(S.LastReport);
	return ESuperSLMVendResult::Success;
}

void USuperSLMSubsystem::ReturnSequence(const FSuperSLMSequence& Sequence)
{
	if (State == nullptr)
	{
		return;
	}
	FSuperSLMSubsystemState& S = *State;
	const int32* Found = S.IdToSlot.Find(Sequence.Id);
	if (Found == nullptr)
	{
		return;
	}
	// Plan §2.5 rows 17 and 20 (H1): ReturnSequence() calls no Layer-1 verb. ReleaseUser() ends
	// the hold: the handle is dead to every caller from here, and the queued recycle resets and
	// unbinds the sequence on the worker, after every op the user queued with a handle, in
	// arrival order (D-SLM7421).
	S.ReleaseUser(*Found);
#if WITH_DEV_AUTOMATION_TESTS
	S.TestJobHolds.Remove(Sequence.Id); // returning the sequence ends its job hold (SetJobHold)
#endif
	S.PublishStats(S.LastReport);
}

#if SUPERSLM_WITH_L2S1_ASYNC
ESuperSLMRestoreResult USuperSLMSubsystem::ResetSequence(const FSuperSLMSequence& Sequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandle();
	if (State == nullptr)
	{
		OutError = TEXT("the subsystem is not configured");
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799: refused like an unconfigured subsystem
		return ESuperSLMRestoreResult::NotConfigured;
	}
	FSlot* Slot = State->Find(Sequence.Id);
	if (Slot == nullptr)
	{
		OutError = TEXT("not a live sequence handle");
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (Slot->EffectiveQueueDepth() >= State->Config.MaxQueuedOperationsPerSequence)
	{
		OutError = FString::Printf(TEXT("this sequence's own queue already holds %d entries (SSLM_SEQUENCE_QUEUE_FULL)"), State->Config.MaxQueuedOperationsPerSequence);
		return ESuperSLMRestoreResult::SequenceQueueFull;
	}

	FSlotQueuedOp Op;
	Op.Kind = ESlotOpKind::Reset;
	Op.Ordinal = State->MintOrdinal();
	Op.HandleId = State->MintHandle();
	Op.Owner = Slot->VendedId;
	Slot->OpQueue.Add(Op);
	Slot->CancelQueuedContinuation(); // 1.1: a queued op precedes the generation's next job
	Slot->MarkBindEligible(); // plan §2.5 row 4: a queued reset makes the sequence bind-eligible
	OutHandle.Id = Op.HandleId;
	return ESuperSLMRestoreResult::Success;
}
#endif // SUPERSLM_WITH_L2S1_ASYNC

// ---------------------------------------------------------------------------------------------
// Per-sequence configuration
// ---------------------------------------------------------------------------------------------

void USuperSLMSubsystem::SetLayerBudget(const FSuperSLMSequence& Sequence, int32 LayerBudget)
{
	FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	if (Slot == nullptr)
	{
		return;
	}
	if (LayerBudget <= 0)
	{
		Slot->PinnedLayerBudget = 0; // back to the scheduler's per-job choice
		return;
	}
	if (LayerBudget > State->Config.MaxLayerBudget)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("SetLayerBudget(%d) exceeds the declared max_layer_budget %d; clamped."), LayerBudget, State->Config.MaxLayerBudget);
	}
	Slot->PinnedLayerBudget = FMath::Clamp(LayerBudget, 1, State->Config.MaxLayerBudget);
}

bool USuperSLMSubsystem::SetSchema(const FSuperSLMSequence& Sequence, const FSuperSLMSchemaHandle& Schema, FString& OutError)
{
	FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	if (Slot == nullptr)
	{
		OutError = TEXT("not a live sequence handle");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799
		return false;
	}
	// Plan §2.5 row 4 (D-SLM7625, D-SLM7665): Layer 1 binds only a created or reset sequence that
	// has not since run a generation call, an adopt or a restore. The slot knows the eligibility
	// its sequence will have once everything already queued has run, and refuses by name without
	// asking Layer 1.
	if (!Slot->bBindEligibleAfterQueue)
	{
		const TCHAR* Why = TEXT("has generated");
		switch (Slot->BindIneligibleReason)
		{
			case EBindIneligibleReason::Restored: Why = TEXT("was restored"); break;
			case EBindIneligibleReason::AdoptedPrefix: Why = TEXT("adopted a prefix"); break;
			case EBindIneligibleReason::Generated:
			case EBindIneligibleReason::None: break;
		}
		OutError = FString::Printf(TEXT("bind needs a new or reset sequence; this one %s; call ResetSequence() first"), Why);
		return false;
	}

	FString SchemaName;
	if (!Schema.IsNone())
	{
		uint8 Hash[32];
		if (!SuperSLMRuntime::ResolveSchemaName(Schema.Opaque, Hash, SchemaName))
		{
			OutError = TEXT("unknown schema handle");
			return false;
		}
		if (FMemory::Memcmp(Hash, State->ArtifactHash, 32) != 0)
		{
			OutError = FString::Printf(TEXT("schema '%s' was looked up on artifact %s, but this subsystem runs artifact %s"),
				*SchemaName, *SuperSLMRuntime::HashToHex(Hash), *SuperSLMRuntime::HashToHex(State->ArtifactHash));
			return false;
		}
	}

	if (Slot->MustHoldRequest())
	{
		// Plan §2.5 row 20 rule 4: held with its ordinal, carried by the holder's next job whose
		// op is later than it, applied at that job's head on the worker, and confirmed (or
		// refused by name) at that job's delivery.
		FHeldRequest Entry;
		Entry.Owner = Slot->VendedId;
		Entry.Kind = EHeldKind::Schema;
		Entry.SchemaName = SchemaName;
		Entry.Ordinal = State->MintOrdinal();
		Slot->Held.Add(Entry);
		Slot->CancelQueuedContinuation(); // 1.1: the holder's next job carries it (row 20)
		return true;
	}
	return State->ApplySchemaInline(*Slot, SchemaName, OutError);
}

void USuperSLMSubsystem::RequestAdapterSwap(const FSuperSLMSequence& Sequence, const FSuperSLMAdapterHandle& Adapter)
{
	FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	if (Slot == nullptr)
	{
		return;
	}
	if (State->bTickHorizonReached)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap refused: %s"), *State->TickHorizonMessage); // D-SLM7799
		return;
	}
	if (Adapter.IsValid())
	{
		SuperSLMRuntime::FResolvedAdapter Resolved;
		if (!SuperSLMRuntime::ResolveAdapter(Adapter.Id, Resolved))
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap: adapter %lld is not a live adapter handle."), Adapter.Id);
			return;
		}
		if (Resolved.Base != State->Model)
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap: adapter %lld was imported against a different base model than the one configured (SSLM_ADAPTER_MODEL_MISMATCH)."), Adapter.Id);
			return;
		}
	}
	// Plan §2.5 row 20 rule 4: with nothing queued, nothing in flight, no active generation and
	// an empty held list, the swap runs at the call (the inline path keeps arrival order).
	// Otherwise -- and when Layer 1 refuses the inline swap because the sequence rests mid-token
	// -- it is held with its ordinal and carried by the holder's next job.
	if (!Slot->MustHoldRequest() && Slot->Seq != nullptr)
	{
		sslm_adapter Handle = nullptr;
		SuperSLMRuntime::FResolvedAdapter Resolved;
		if (Adapter.IsValid() && SuperSLMRuntime::ResolveAdapter(Adapter.Id, Resolved))
		{
			Handle = Resolved.Adapter;
		}
		const sslm_status Status = sslm_seq_set_adapter(Slot->Seq, Handle);
		if (Status == SSLM_OK)
		{
			Slot->ActiveAdapterId = Adapter.Id;
			TRACE_BOOKMARK(TEXT("SuperSLM: adapter swap (sequence %lld, adapter %lld)"), Slot->VendedId, Slot->ActiveAdapterId);
			return;
		}
		if (Status != SSLM_ADAPTER_SWAP_MIDTOKEN_REJECTED)
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap: sslm_seq_set_adapter refused adapter %lld (%s); the request is dropped."), Adapter.Id, *StatusText(Status));
			return;
		}
	}

	FHeldRequest Entry;
	Entry.Owner = Slot->VendedId;
	Entry.Kind = EHeldKind::Adapter;
	Entry.AdapterId = Adapter.Id;
	Entry.Ordinal = State->MintOrdinal();
	if (Adapter.IsValid())
	{
		// T-2987 L2: a held adapter keeps its import alive until the entry resolves.
		SuperSLMRuntime::FResolvedAdapter Pinned;
		if (!SuperSLMRuntime::PinAdapter(Adapter.Id, Pinned))
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap: adapter %lld is not a live adapter handle."), Adapter.Id);
			return;
		}
		Entry.bPinned = true;
	}
	Slot->Held.Add(Entry);
	Slot->CancelQueuedContinuation(); // 1.1: the holder's next job carries it (row 20)
}

FSuperSLMAdapterHandle USuperSLMSubsystem::GetActiveAdapter(const FSuperSLMSequence& Sequence) const
{
	FSuperSLMAdapterHandle Handle;
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		Handle.Id = Slot->ActiveAdapterId;
	}
	return Handle;
}

// ---------------------------------------------------------------------------------------------
// Shared prefixes (D-SLM7342)
// ---------------------------------------------------------------------------------------------

// The one prefix admission both CreatePrefix() overloads share. Legacy mode keeps its checks and
// their order: the live count against PrefixBlockCount, then an empty, over-cap or
// out-of-vocabulary token list. Budget mode (plugin paging plan §3.3, UP-6) admits an empty list
// (begun with budget 1), checks the tokens, then takes the page ledger's build admission: the
// count rule (PrefixLimit) and region P's page rule (KvPagesExhausted), charged before the Begin
// job is queued.
static ESuperSLMPrefixResult CreatePrefixOnState(FSuperSLMSubsystemState& S, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString& OutError)
{
	const bool bBudgetMode = S.IsBudgetMode();
	if (!bBudgetMode)
	{
		int32 LivePrefixes = S.Prefixes.Num();
#if SUPERSLM_WITH_PREFIX_CACHE
		LivePrefixes -= SuperSLMPrefixCacheGlue::ShadowCount(S); // precache plan §5.4: cache entries never count
#endif
		if (LivePrefixes >= S.Config.PrefixBlockCount)
		{
			OutError = FString::Printf(TEXT("PrefixBlockCount (%d) prefixes are already live; release one, or configure more prefix blocks"), S.Config.PrefixBlockCount);
			TRACE_BOOKMARK(TEXT("SuperSLM: prefix pool refusal (%d live)"), LivePrefixes);
			return ESuperSLMPrefixResult::PrefixLimit;
		}
		if (Tokens.Num() == 0)
		{
			OutError = TEXT("the prefix is empty");
			return ESuperSLMPrefixResult::Malformed;
		}
	}
	if (Tokens.Num() > S.Shape.ContextCap)
	{
		OutError = FString::Printf(TEXT("the prefix's %d tokens exceed the model's context_cap of %lld"), Tokens.Num(), S.Shape.ContextCap);
		return ESuperSLMPrefixResult::Malformed;
	}
	for (int32 I = 0; I < Tokens.Num(); ++I)
	{
		if (Tokens[I] < 0 || Tokens[I] >= S.Shape.VocabSize)
		{
			OutError = FString::Printf(TEXT("prefix token %d at index %d is outside the model's vocabulary [0, %d)"), Tokens[I], I, S.Shape.VocabSize);
			return ESuperSLMPrefixResult::Malformed;
		}
	}

	SuperSLMPaging::FAdmission Admission;
	if (bBudgetMode)
	{
		Admission = S.Ledger.AdmitPrefixBuild(Tokens.Num());
		if (Admission.Outcome == SuperSLMPaging::EPageAdmit::PrefixLimit)
		{
			OutError = FString::Printf(TEXT("PrefixBlockCount (%d) prefixes are already live; release one, or configure more prefix blocks"), S.Config.PrefixBlockCount);
			TRACE_BOOKMARK(TEXT("SuperSLM: prefix pool refusal (%u live)"), S.Ledger.LivePrefixCount());
			return ESuperSLMPrefixResult::PrefixLimit;
		}
		if (Admission.Outcome == SuperSLMPaging::EPageAdmit::KvPagesExhausted)
		{
			const uint32 Charge = SuperSLMPaging::PrefixBuildCharge(S.PagePositions, S.Shape.ContextCap, Tokens.Num());
			OutError = FString::Printf(TEXT("KvPagesExhausted: a %d-token prefix is charged %u pages on the prefix region, which has %u of %u free; release a prefix (or the sequences still adopting a released one), or configure more prefix blocks"),
				Tokens.Num(), Charge, S.Ledger.RegionFree(SuperSLMPaging::ERegion::P), S.Ledger.RegionCapacity(SuperSLMPaging::ERegion::P));
			return ESuperSLMPrefixResult::KvPagesExhausted;
		}
		if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
		{
			OutError = FString::Printf(TEXT("the page ledger refused a %d-token prefix build (outcome %d)"), Tokens.Num(), static_cast<int32>(Admission.Outcome));
			return Admission.Outcome == SuperSLMPaging::EPageAdmit::NotConfigured ? ESuperSLMPrefixResult::NotConfigured : ESuperSLMPrefixResult::Malformed;
		}
	}

	const int64 Id = GNextPrefixId++;
	FPrefixEntry& Entry = S.Prefixes.Add(Id);
	Entry.Tokens = Tokens;
	FPrefixAdminOp Op;
	Op.Kind = EPrefixAdminKind::Begin;
	Op.Ordinal = S.MintOrdinal();
	Op.PrefixId = Id;
	// §3.9: the test seams' PrefixBuild holds, taken at the build's admission. The start hold
	// parks the Begin; the apply hold parks the delivery of the job that freezes the prefix.
	Op.StartHoldId = S.TakeArmedHold(EPagedJobKind::PrefixBuild, /*bApply*/ false);
	Entry.BuildApplyHoldId = S.TakeArmedHold(EPagedJobKind::PrefixBuild, /*bApply*/ true);
	if (bBudgetMode)
	{
		Entry.LedgerNode = Admission.Node;
		Entry.BuildTicket = Admission.Ticket;
#if WITH_DEV_AUTOMATION_TESTS
		S.PrefixIdToNode.Add(Id, Admission.Node);
#endif
	}
	S.PrefixAdminQueue.Add(Op);
	OutPrefix.Id = Id;
	S.PublishStats(S.LastReport);
	return ESuperSLMPrefixResult::Success;
}

bool USuperSLMSubsystem::CreatePrefix(const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString& OutError)
{
	OutPrefix = FSuperSLMPrefix();
	if (State == nullptr)
	{
		OutError = TEXT("the subsystem is not configured");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799
		return false;
	}
	return CreatePrefixOnState(*State, Tokens, OutPrefix, OutError) == ESuperSLMPrefixResult::Success;
}

ESuperSLMPrefixResult USuperSLMSubsystem::CreatePrefix(const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	FString Error;
	ESuperSLMPrefixResult Result = ESuperSLMPrefixResult::NotConfigured;
	if (State == nullptr)
	{
		Error = TEXT("the subsystem is not configured");
	}
	else if (State->bTickHorizonReached)
	{
		Error = State->TickHorizonMessage; // D-SLM7799
	}
	else
	{
		Result = CreatePrefixOnState(*State, Tokens, OutPrefix, Error);
	}
	if (OutError != nullptr)
	{
		*OutError = MoveTemp(Error);
	}
	return Result;
}

// Plugin paging plan §3.3 "Nested prefixes" (step K3): a child continuing a frozen, live parent
// (sslm_prefix_begin_from). Budget mode only; legacy mode refuses by name. The child's budget is
// its own token count, its charge R(max(T, 1)) on P, settled at its freeze to its private pages.
// The parent is pinned from this admission until the child's build applies (the ledger's pin), and
// the child counts against PrefixBlockCount.
static ESuperSLMPrefixResult CreatePrefixFromOnState(FSuperSLMSubsystemState& S, const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString& OutError)
{
	const FPrefixEntry* ParentEntry = S.Prefixes.Find(Parent.Id);
	if (ParentEntry == nullptr || !ParentEntry->bLedgerBuilt || ParentEntry->Handle == nullptr || ParentEntry->Phase != ESuperSLMPrefixPhase::Ready)
	{
		OutError = ParentEntry == nullptr
			? TEXT("ParentNotUsable: the parent is not a live prefix of this subsystem (released, or never created here)")
			: TEXT("ParentNotUsable: the parent is not frozen yet (or faulted); wait for it to reach Ready");
		return ESuperSLMPrefixResult::ParentNotUsable;
	}
	const int64 ParentLength = ParentEntry->OriginLength + ParentEntry->Tokens.Num();
	const int64 Cap = S.Ledger.GetConfig().ContextCap;
	if (ParentLength + Tokens.Num() > Cap)
	{
		OutError = FString::Printf(TEXT("the parent's %lld positions and the child's %d tokens exceed the context cap of %lld"), ParentLength, Tokens.Num(), Cap);
		return ESuperSLMPrefixResult::Malformed;
	}
	for (int32 I = 0; I < Tokens.Num(); ++I)
	{
		if (Tokens[I] < 0 || Tokens[I] >= S.Shape.VocabSize)
		{
			OutError = FString::Printf(TEXT("prefix token %d at index %d is outside the model's vocabulary [0, %d)"), Tokens[I], I, S.Shape.VocabSize);
			return ESuperSLMPrefixResult::Malformed;
		}
	}

	const SuperSLMPaging::FAdmission Admission = S.Ledger.AdmitNestedPrefixBuild(ParentEntry->LedgerNode, Tokens.Num());
	switch (Admission.Outcome)
	{
		case SuperSLMPaging::EPageAdmit::Admitted:
			break;
		case SuperSLMPaging::EPageAdmit::ParentNotUsable:
			OutError = TEXT("ParentNotUsable: the page ledger does not hold the parent as a live, built prefix");
			return ESuperSLMPrefixResult::ParentNotUsable;
		case SuperSLMPaging::EPageAdmit::PrefixLimit:
			OutError = FString::Printf(TEXT("PrefixBlockCount (%d) prefixes are already live; release one, or configure more prefix blocks"), S.Config.PrefixBlockCount);
			return ESuperSLMPrefixResult::PrefixLimit;
		case SuperSLMPaging::EPageAdmit::KvPagesExhausted:
		{
			const uint32 Charge = SuperSLMPaging::PrefixBuildCharge(S.PagePositions, Cap, Tokens.Num());
			OutError = FString::Printf(TEXT("KvPagesExhausted: a %d-token nested prefix is charged %u pages on the prefix region, which has %u of %u free; release a prefix (or the sequences still adopting a released one), or configure more prefix blocks"),
				Tokens.Num(), Charge, S.Ledger.RegionFree(SuperSLMPaging::ERegion::P), S.Ledger.RegionCapacity(SuperSLMPaging::ERegion::P));
			return ESuperSLMPrefixResult::KvPagesExhausted;
		}
		case SuperSLMPaging::EPageAdmit::NotConfigured:
			OutError = TEXT("the page ledger is not configured");
			return ESuperSLMPrefixResult::NotConfigured;
		case SuperSLMPaging::EPageAdmit::Waiting:
		case SuperSLMPaging::EPageAdmit::KvModeMismatch:
		case SuperSLMPaging::EPageAdmit::PrefixPinned:
		case SuperSLMPaging::EPageAdmit::InvalidRequest:
		case SuperSLMPaging::EPageAdmit::ContextCapMismatch:
			OutError = FString::Printf(TEXT("the page ledger refused a %d-token nested prefix build (outcome %d)"), Tokens.Num(), static_cast<int32>(Admission.Outcome));
			return ESuperSLMPrefixResult::Malformed;
	}

	const sslm_prefix ParentHandle = ParentEntry->Handle;
	const int64 Id = GNextPrefixId++;
	FPrefixEntry& Entry = S.Prefixes.Add(Id); // ParentEntry is not read after this Add
	Entry.Tokens = Tokens;
	Entry.OriginLength = ParentLength;
	Entry.LedgerNode = Admission.Node;
	Entry.BuildTicket = Admission.Ticket;
#if WITH_DEV_AUTOMATION_TESTS
	S.PrefixIdToNode.Add(Id, Admission.Node);
#endif
	FPrefixAdminOp Op;
	Op.Kind = EPrefixAdminKind::Begin;
	Op.Ordinal = S.MintOrdinal();
	Op.PrefixId = Id;
	Op.bNested = true;
	Op.NestedParentHandle = ParentHandle;
	// §3.9: the test seams' NestedBuild holds, taken at the build's admission, as a root's are.
	Op.StartHoldId = S.TakeArmedHold(EPagedJobKind::NestedBuild, /*bApply*/ false);
	Entry.BuildApplyHoldId = S.TakeArmedHold(EPagedJobKind::NestedBuild, /*bApply*/ true);
	S.PrefixAdminQueue.Add(Op);
	OutPrefix.Id = Id;
	S.PublishStats(S.LastReport);
	return ESuperSLMPrefixResult::Success;
}

ESuperSLMPrefixResult USuperSLMSubsystem::CreatePrefixFrom(const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	FString Error;
	ESuperSLMPrefixResult Result = ESuperSLMPrefixResult::NotConfigured;
	if (State == nullptr)
	{
		Error = TEXT("the subsystem is not configured");
	}
	else if (!State->IsBudgetMode())
	{
		Error = TEXT("NestedRequiresBudgetMode: a nested prefix (sslm_prefix_begin_from) needs budget mode; configure SequenceBudget > 0");
		Result = ESuperSLMPrefixResult::NestedRequiresBudgetMode;
	}
	else if (State->bTickHorizonReached)
	{
		Error = State->TickHorizonMessage; // D-SLM7799
	}
	else
	{
		Result = CreatePrefixFromOnState(*State, Parent, Tokens, OutPrefix, Error);
	}
	if (OutError != nullptr)
	{
		*OutError = MoveTemp(Error);
	}
	return Result;
}

ESuperSLMPrefixPhase USuperSLMSubsystem::GetPrefixPhase(const FSuperSLMPrefix& Prefix) const
{
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.6: a cache lease's phase moves to Ready as a CreatePrefix handle's does. Read
	// before State->Prefixes, where an adopt's shadow entry would read Ready after the lease ended.
	if (State != nullptr && State->PrefixCache.Host != nullptr)
	{
		switch (State->PrefixCache.Host->GetLeaseState(Prefix))
		{
		case ESuperSLMPrefixLeaseState::Invalid:
			break; // not a cache lease: the subsystem's own prefixes, below
		case ESuperSLMPrefixLeaseState::Pending:
			return ESuperSLMPrefixPhase::Pending;
		case ESuperSLMPrefixLeaseState::Ready:
			return ESuperSLMPrefixPhase::Ready;
		case ESuperSLMPrefixLeaseState::BudgetExhausted:
		case ESuperSLMPrefixLeaseState::PoolExhausted:
		case ESuperSLMPrefixLeaseState::UnknownSchema:
		case ESuperSLMPrefixLeaseState::Failed:
			return ESuperSLMPrefixPhase::Faulted; // GetPrefixLeaseState() names which refusal
		default:
			return ESuperSLMPrefixPhase::Invalid; // Cancelled or Ended: no longer a live handle
		}
	}
#endif
	if (State != nullptr)
	{
		if (const FPrefixEntry* Entry = State->Prefixes.Find(Prefix.Id))
		{
			return Entry->Phase;
		}
	}
	return ESuperSLMPrefixPhase::Invalid;
}

#if SUPERSLM_WITH_L2S1_ASYNC
ESuperSLMRestoreResult USuperSLMSubsystem::AdoptPrefix(const FSuperSLMSequence& Sequence, const FSuperSLMPrefix& Prefix, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandle();
	FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	if (Slot == nullptr)
	{
		OutError = TEXT("not a live sequence handle");
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799: refused like an unconfigured subsystem
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (Slot->EffectiveQueueDepth() >= State->Config.MaxQueuedOperationsPerSequence)
	{
		OutError = FString::Printf(TEXT("this sequence's own queue already holds %d entries (SSLM_SEQUENCE_QUEUE_FULL)"), State->Config.MaxQueuedOperationsPerSequence);
		return ESuperSLMRestoreResult::SequenceQueueFull;
	}
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.6: a cache lease is adopted through this same path once Ready; a returned or
	// evicted one is PrefixLeaseEnded.
	{
		ESuperSLMRestoreResult CacheResult = ESuperSLMRestoreResult::Success;
		if (SuperSLMPrefixCacheGlue::StopCacheAdopt(*State, Prefix.Id, CacheResult, OutError))
		{
			return CacheResult;
		}
	}
#endif
	if (!State->Prefixes.Contains(Prefix.Id))
	{
		OutError = TEXT("not a live prefix handle");
		return ESuperSLMRestoreResult::Malformed;
	}

	FSlotQueuedOp Op;
	Op.Kind = ESlotOpKind::Adopt;
	Op.Ordinal = State->MintOrdinal();
	Op.HandleId = State->MintHandle();
	Op.Owner = Slot->VendedId;
	Op.PrefixId = Prefix.Id;
	Slot->OpQueue.Add(Op);
	Slot->CancelQueuedContinuation(); // 1.1: a queued op precedes the generation's next job
	Slot->MarkBindIneligible(EBindIneligibleReason::AdoptedPrefix); // plan §2.5 row 4
	if (FPrefixEntry* Entry = State->Prefixes.Find(Prefix.Id))
	{
		++Entry->PendingAdopts;
	}
	OutHandle.Id = Op.HandleId;
	return ESuperSLMRestoreResult::Success;
}
#endif // SUPERSLM_WITH_L2S1_ASYNC

bool USuperSLMSubsystem::ReleasePrefix(const FSuperSLMPrefix& Prefix, FString& OutError)
{
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache plan §5.6: a cache-owned handle is returned (ReturnPrefix), never released.
	if (State != nullptr && SuperSLMPrefixCacheGlue::RefuseRelease(*State, Prefix, OutError))
	{
		return false;
	}
#endif
	FPrefixEntry* Entry = State ? State->Prefixes.Find(Prefix.Id) : nullptr;
	if (Entry == nullptr)
	{
		OutError = TEXT("not a live prefix handle");
		return false;
	}
	if (Entry->PendingAdopts > 0)
	{
		OutError = FString::Printf(TEXT("%d adopt(s) of this prefix are still queued; release it after they drain"), Entry->PendingAdopts);
		return false;
	}
	// Plugin paging plan §3.5 (K3): a restore hinted with this prefix pins it from its call until
	// its job delivers (or it is refused).
	if (Entry->PendingHintedRestores > 0)
	{
		OutError = FString::Printf(TEXT("PrefixPinned: %d restore(s) hinted with this prefix are still queued; release it after they apply"), Entry->PendingHintedRestores);
		return false;
	}
	// Plugin paging plan §3.3, budget mode: a built prefix's release is a ledger admission, refused
	// by name while an admitted adopt, nested build or hinted restore of it is still open
	// (PrefixPinned). Checked before anything changes.
	SuperSLMPaging::FTicket ReleaseTicket = SuperSLMPaging::InvalidTicket;
	if (State->IsBudgetMode() && Entry->Handle != nullptr && Entry->bLedgerBuilt)
	{
		const SuperSLMPaging::FAdmission Admission = State->Ledger.AdmitPrefixRelease(Entry->LedgerNode);
		if (Admission.Outcome == SuperSLMPaging::EPageAdmit::PrefixPinned)
		{
			OutError = TEXT("PrefixPinned: an adopt, a nested build (CreatePrefixFrom) or a hinted restore of this prefix is still open; release it after that job applies");
			return false;
		}
		if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
		{
			OutError = FString::Printf(TEXT("the page ledger refused the release (outcome %d)"), static_cast<int32>(Admission.Outcome));
			return false;
		}
		ReleaseTicket = Admission.Ticket;
	}
	if (Entry->Handle == nullptr && Entry->bAdminJobInFlight)
	{
		// 1.1: its Begin is on a worker. The entry leaves the caller's view now, and its Begin's
		// delivery queues the release of the block it made (RetiringPrefixes). In budget mode the
		// build's open ticket goes with it, failed once the engine frees the pages (§3.3), and the
		// release's own holds (§3.9) are taken now and handed to the release that delivery queues.
		FSuperSLMSubsystemState::FRetiringPrefix& Retiring = State->RetiringPrefixes.Add(Prefix.Id);
		Retiring.PinnedLane = Entry->PinnedLane;
		Retiring.bJobInFlight = true;
		Retiring.BuildTicket = Entry->BuildTicket;
		Retiring.ReleaseStartHoldId = State->TakeArmedHold(EPagedJobKind::PrefixRelease, /*bApply*/ false);
		Retiring.ReleaseApplyHoldId = State->TakeArmedHold(EPagedJobKind::PrefixRelease, /*bApply*/ true);
		State->Prefixes.Remove(Prefix.Id);
		State->PublishStats(State->LastReport);
		return true;
	}
	if (Entry->Handle == nullptr)
	{
		// Never began (still Pending with no worker job ever run, or already released):
		// remove any still-pending Begin op and drop the entry at once. In budget mode the build
		// drew nothing, so its whole charge returns now.
		const int64 Id = Prefix.Id;
		State->PrefixAdminQueue.RemoveAll([Id](const FPrefixAdminOp& Op) { return Op.PrefixId == Id; });
		if (Entry->BuildTicket != SuperSLMPaging::InvalidTicket)
		{
			State->Ledger.ApplyPrefixBuild(Entry->BuildTicket, false);
		}
		State->Prefixes.Remove(Id);
		State->PublishStats(State->LastReport);
		return true;
	}

	// A real block is live: queue the release on the worker (D-SLM7407: "prefix begin/release"
	// runs exclusively on the worker, never inline). Fire-and-forget -- ReleasePrefix() has no
	// handle-returning form, so the entry is removed from the caller's view AT ONCE (T-2815 round
	// 13: the entry used to stay in State->Prefixes until process teardown on this path -- the one
	// path Handle != nullptr took -- because nothing here ever called State->Prefixes.Remove();
	// its own pool block was never reclaimed from the map's view even after the real
	// sslm_prefix_release below had actually run). Handle and PinnedLane are captured onto the
	// queued op now, while Entry still exists, because DispatchPrefixAdmin no longer has an entry
	// to read them from once this op is admitted.
	FPrefixAdminOp Op;
	Op.Kind = EPrefixAdminKind::Release;
	Op.Ordinal = State->MintOrdinal();
	Op.PrefixId = Prefix.Id;
	Op.ReleaseHandle = Entry->Handle;
	Op.PinnedLane = Entry->PinnedLane;
	// Plugin paging plan §3.3: a built prefix's release applies its admission at delivery; an
	// unbuilt one (still prefilling, or faulted) fails its build there instead, once the engine has
	// freed the pages. §3.9: the release's holds, taken at its admission.
	if (ReleaseTicket != SuperSLMPaging::InvalidTicket)
	{
		Op.LedgerTicket = ReleaseTicket;
	}
	else if (Entry->BuildTicket != SuperSLMPaging::InvalidTicket)
	{
		Op.LedgerTicket = Entry->BuildTicket;
		Op.bReleaseFailsBuild = true;
		Entry->BuildTicket = SuperSLMPaging::InvalidTicket;
	}
	Op.StartHoldId = State->TakeArmedHold(EPagedJobKind::PrefixRelease, /*bApply*/ false);
	Op.ApplyHoldId = State->TakeArmedHold(EPagedJobKind::PrefixRelease, /*bApply*/ true);
	State->PrefixAdminQueue.Add(Op);
#if SUPERSLM_WITH_PREFIX_CACHE
	SuperSLMPrefixCacheGlue::NoteRelease(*State, Op.ReleaseHandle);
#endif
	// 1.1: keeps its place until the release delivers, and the release waits for a prefill job
	// still in flight (RetiringPrefixes).
	FSuperSLMSubsystemState::FRetiringPrefix& Retiring = State->RetiringPrefixes.Add(Prefix.Id);
	Retiring.PinnedLane = Entry->PinnedLane;
	Retiring.bJobInFlight = Entry->bAdminJobInFlight;
	State->Prefixes.Remove(Prefix.Id);
	State->PublishStats(State->LastReport);
	return true;
}

bool USuperSLMSubsystem::ReleasePrefix(const FSuperSLMPrefix& Prefix)
{
	FString Error;
	const bool bReleased = ReleasePrefix(Prefix, Error);
	if (!bReleased)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("ReleasePrefix: %s"), *Error);
	}
	return bReleased;
}

// ---------------------------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------------------------

bool USuperSLMSubsystem::BeginGeneration(const FSuperSLMSequence& Sequence, const FSuperSLMGenerationRequest& Request, FString& OutError)
{
	FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	if (Slot == nullptr)
	{
		OutError = TEXT("not a live sequence handle");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799
		return false;
	}

	// D-SLM7946 (plan §10.5.1.1): a sequence runs one generation at a time. A generation is
	// refused here, at the call, unless the sequence will be Idle when its turn comes: Idle with
	// nothing queued, or a reset queued (or in flight) after its last generation. A refused request
	// queues nothing and changes nothing on the sequence. It comes after the tick-horizon refusal
	// and before the queue bound, so a request that can never be honoured names that reason, not
	// the transient one.
	const FGenerationTurnProjection Turn = ProjectGenerationTurn(*Slot);
	if (!Turn.bIdle)
	{
		OutError = OneGenerationRefusalText(Turn);
		return false;
	}

	// D-SLM7421/D-SLM7429 (plan §5 item 3): an accepted decode/prefill submission queues like
	// Reset/Adopt/Save, behind whatever is queued or in flight on the sequence, and is refused
	// only when the sequence's own queue is full (R-S1j's fixed-arrival-order cell: a begin behind
	// a queued reset passes the rule above and is refused here with SequenceQueueFull).
	if (Slot->EffectiveQueueDepth() >= State->Config.MaxQueuedOperationsPerSequence)
	{
		OutError = FString::Printf(TEXT("this sequence's own queue is already full (%d entries, SSLM_SEQUENCE_QUEUE_FULL)"), State->Config.MaxQueuedOperationsPerSequence);
		return false;
	}

	if (Request.MaxNewTokens < 1)
	{
		OutError = FString::Printf(TEXT("MaxNewTokens must be at least 1 (got %d)"), Request.MaxNewTokens);
		return false;
	}
	const SuperSLMRuntime::FModelShape& Shape = State->Shape;
	for (int32 I = 0; I < Request.PromptTokens.Num(); ++I)
	{
		const int32 Token = Request.PromptTokens[I];
		if (Token < 0 || Token >= Shape.VocabSize)
		{
			OutError = FString::Printf(TEXT("prompt token %d at index %d is outside the model's vocabulary [0, %d) (SSLM_TOKEN_ID_OUT_OF_RANGE)"), Token, I, Shape.VocabSize);
			return false;
		}
	}
	// An empty prompt is only legal when the sequence already holds (or is adopting) a prefix to
	// continue from -- checked at ACTIVATION time (where the adopted/adopting state is known for
	// certain), not here, since a Generate request may be queued behind an AdoptPrefix() request
	// that has not drained yet.
	if (Request.PromptTokens.Num() == 0 && Slot->OpQueue.IsEmpty() && !Slot->bAdopted && Slot->Phase != ESuperSLMSequencePhase::Idle)
	{
		// (Kept for parity with the old synchronous message shape when nothing else applies.)
	}

	FSlotQueuedOp Op;
	Op.Kind = ESlotOpKind::Generate;
	Op.Ordinal = State->MintOrdinal();
	Op.Owner = Slot->VendedId;
	Op.GenRequest = Request;
	Slot->OpQueue.Add(Op);
	Slot->MarkBindIneligible(EBindIneligibleReason::Generated); // plan §2.5 row 4
	return true;
}

ESuperSLMSequencePhase USuperSLMSubsystem::GetPhase(const FSuperSLMSequence& Sequence) const
{
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		return Slot->Phase;
	}
	// A handle that is not live will never progress.
	return ESuperSLMSequencePhase::Faulted;
}

const TArray<int32>& USuperSLMSubsystem::GetGeneratedTokens(const FSuperSLMSequence& Sequence) const
{
	static const TArray<int32> Empty;
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		return Slot->Generated;
	}
	return Empty;
}

ESuperSLMDecodeOutcome USuperSLMSubsystem::GetLastDecodeOutcome(const FSuperSLMSequence& Sequence) const
{
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		return Slot->LastOutcome;
	}
	// The ABI's -3: the handle names a sequence that is not live.
	return ESuperSLMDecodeOutcome::SequenceNoLongerValid;
}

FSuperSLMSequenceStats USuperSLMSubsystem::GetStats(const FSuperSLMSequence& Sequence) const
{
	// Plan §2.5 row 22: the delivered copy, never a game-thread sslm_stats on a live sequence (a
	// race with a job in flight whatever the gate). Refreshed at the delivery of each of the
	// holder's jobs that changes the sequence, from sslm_stats read at that job's tail on the
	// worker.
	const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr;
	return Slot != nullptr ? Slot->Stats : FSuperSLMSequenceStats();
}

bool USuperSLMSubsystem::Tokenize(const FString& Utf8Text, TArray<int32>& OutTokens) const
{
	// A read-only, stateless-per-call query -- not named among plan §5's "prefill, decode, save,
	// restore, reset, adopt, prefix begin/release, adapter bind" list of calls the worker must
	// exclusively own, and T-2815's build already ran it inline. Kept inline; flagged in the
	// round-6 build log alongside the schema/adapter-bind reading.
	OutTokens.Reset();
	if (State == nullptr)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("Tokenize: the subsystem is not configured."));
		return false;
	}
	const FTCHARToUTF8 Utf8(*Utf8Text);
	int32 Count = Utf8.Length() + 16;
	OutTokens.SetNumUninitialized(Count);
	const char* Text = reinterpret_cast<const char*>(Utf8.Get());
#if WITH_DEV_AUTOMATION_TESTS
	SuperSLMTokenizeTestSeam::CountCall(SuperSLMTokenizeTestSeam::ESite::SubsystemTokenize); // cell 7.20's counter
#endif
	sslm_status Status = sslm_tokenize(State->Model, Text, OutTokens.GetData(), &Count);
	if (Status == SSLM_BUFFER_TOO_SMALL)
	{
		OutTokens.SetNumUninitialized(Count);
#if WITH_DEV_AUTOMATION_TESTS
		SuperSLMTokenizeTestSeam::CountCall(SuperSLMTokenizeTestSeam::ESite::SubsystemTokenize);
#endif
		Status = sslm_tokenize(State->Model, Text, OutTokens.GetData(), &Count);
	}
	if (Status != SSLM_OK)
	{
		OutTokens.Reset();
		// SSLM_ARTIFACT_REJECTED here means the artifact carries no Tokenizer section.
		UE_LOG(LogSuperSLM, Warning, TEXT("Tokenize: sslm_tokenize failed (%s)%s."), *StatusText(Status),
			Status == SSLM_ARTIFACT_REJECTED ? TEXT(": the configured artifact carries no tokenizer") : TEXT(""));
		return false;
	}
	OutTokens.SetNum(Count);
	return true;
}

// ---------------------------------------------------------------------------------------------
// The worker-job machinery (§5, §5.1, D-SLM7407/D-SLM7414/D-SLM7421). Every Dispatch* function
// below builds an Execute closure (runs on the lane's own worker thread; touches only Model,
// the KV pool, the lane's own workspace, and the specific Layer-1 handles this one job owns) and
// a Deliver closure (runs on the game thread, once the worker has finished; the only place a
// job's own FSlot/FPrefixEntry/handle-map state is mutated). Nothing else needs to be atomic:
// Execute always finishes-and-signals before Deliver ever reads what it wrote (the acquire/
// release pair on FSuperSLMCpuWorkerRunnable::bJobDone is the full synchronization).
// ---------------------------------------------------------------------------------------------

namespace
{
	// Plan §2.5 row 20 rule 4: one held entry a posted job carries, captured by value at post.
	// An adapter is resolved at post through the job's own registry pin, released at the job's
	// delivery, so the import cannot be released while the worker uses the handle.
	struct FCarriedRequest
	{
		int64 Ordinal = 0;
		EHeldKind Kind = EHeldKind::Schema;
		FString SchemaName;
		int64 AdapterId = 0;
		sslm_adapter Adapter = nullptr;
		bool bJobPin = false;
		bool bUnresolved = false;     // a nonzero adapter id the registry no longer resolves
		uint8 AdapterHash[32] = {};   // the save's wrapper records it (rule 4, T-2987 F7)
	};

	// Per sequence a job touches: the statuses of the entries it carried, and the stats copy read
	// at the job's tail (plan §2.5 row 22).
	struct FSequenceJobResult
	{
		TArray<sslm_status> CarriedStatuses; // one per carried entry, in the same order; empty if
		                                     // the job never reached its head
		FSuperSLMSequenceStats Stats;
		bool bStatsValid = false;
		// sslm_stats_out::context_length at the job's tail (-1 when the read failed). A span job's
		// delivery takes the sequence's committed positions from it (schema fast-decode plan rev
		// 1.3.5, §6 P1-X: the verb commits the pending token and the confirmed drafts, a count the
		// plugin cannot know at post).
		int64 ContextLength = -1;
		// Schema fast-decode plan rev 1.3.5, §6 P1-X "Forcedness, observed": the
		// sslm_seq_forced_tokens (cap 0) read at the job's tail, when the job's sequence reads
		// forcedness. bForcedRead is true when the call returned SSLM_OK.
		bool bForcedReadAttempted = false;
		bool bForcedRead = false;
		int32 ForcedCount = 0;
		bool bForcedAccepting = false;
	};

	// Shared output shape every job kind writes on the worker and reads back on delivery.
	struct FJobOutput
	{
		sslm_status Status = SSLM_OK;
		bool bSkipped = false;   // the op could not even be attempted (e.g. Adopt's prefix faulted)
		bool bBoolFlag = false;  // Reset: bRecreated; Restore: adapter re-bind specifically failed
		bool bOldHandleReleased = false; // Restore: sslm_seq_release(OldSeq) already ran -- the
		                                  // reserved slot's prior Seq must never be touched again
		sslm_seq SeqOut = nullptr;
		int32 Consumed = 0;      // prefill tokens consumed this job
		TArray<int32> DecodeTokens; // one entry per decode-batch slot, in the same order
		// 1.1 (plan §5 item 1a): per decode-batch slot, true when a continuation found that member's
		// generation already over at its head and left it out of the call (its DecodeTokens entry
		// is then meaningless). A prefill continuation that skips sets bSkipped instead.
		TArray<bool> MemberSkipped;
		TArray<uint8> Blob;      // Save's own output

		// One per sequence the job touches (a decode batch: one per slot, in slot order).
		TArray<FSequenceJobResult> PerSequence;

		// Reset: the recycle's unbind statuses (plan §2.5 row 17), and the binding re-apply after
		// a recreating reset for an owner that holds the slot (row 7).
		sslm_status UnbindSchemaStatus = SSLM_OK;
		sslm_status UnbindAdapterStatus = SSLM_OK;
		sslm_status ReapplySchemaStatus = SSLM_OK;
		sslm_status ReapplyAdapterStatus = SSLM_OK;

		// Prefix prefill: the freeze at the tail of the prefix's last prefill job (row 22). Budget
		// mode: an empty prefix's Begin job freezes too (plugin paging plan §3.3 "Empty prefixes").
		bool bFreezeAttempted = false;
		sslm_status FreezeStatus = SSLM_OK;

		// Budget mode (plugin paging plan §3.2, GetKvPageStats()): sslm_kv_pool_stats read at the
		// tail of a job that draws or frees pages, delivered as the subsystem's pool snapshot.
		bool bPoolStatsRead = false;
		sslm_kv_pool_stats_out PoolStats = {};

		// Budget mode (plugin paging plan §3.5, step K3). Reset: a create-first normalization's
		// create status (SSLM_OK when none was attempted or it succeeded) and whether it replaced the
		// holder. Restore: the engine's out_shared_pages (sslm_seq_restore_shared).
		sslm_status CreateStatus = SSLM_OK;
		bool bNormalized = false;
		uint32 SharedPages = 0;

		// Save: what the sequence has bound at the save's position, read by the save job after
		// the entries it carries are applied (rule 4, T-2987 F7), and the job's own refusal.
		FString SavedSchemaName;
		bool bSavedAdapter = false;
		uint8 SavedAdapterHash[32] = {};
		FString SaveRefusal;

#if WITH_DEV_AUTOMATION_TESTS
		// Reset only: FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride()'s status, taken at
		// the job's dispatch. When not SSLM_OK the worker returns it in place of the Layer-1 reset
		// (or create) call.
		sslm_status TestResetStatus = SSLM_OK;
#endif
	};
	using FJobOutputRef = TSharedRef<FJobOutput, ESPMode::ThreadSafe>;

	// Rule 4's carrying, at post: captures by value every held entry of the holder that is not
	// already carried and whose ordinal is below OrdinalBelow (a lifecycle op's job and a
	// Generate's first job use their op's ordinal), and marks it carried. A later prefill or
	// decode job of an active generation passes bAdaptersOnly: it carries adapter entries only,
	// the mid-generation swap, never a schema entry (T-2988 S1). With one job in flight per
	// sequence (row 22's gates), no entry is carried twice.
	TArray<FCarriedRequest> CarryHeldRequests(FSlot& Slot, int64 OrdinalBelow, bool bAdaptersOnly)
	{
		TArray<FCarriedRequest> Carried;
		for (FHeldRequest& Entry : Slot.Held)
		{
			if (Entry.bCarried || Entry.Ordinal >= OrdinalBelow || (bAdaptersOnly && Entry.Kind != EHeldKind::Adapter))
			{
				continue;
			}
			FCarriedRequest C;
			C.Ordinal = Entry.Ordinal;
			C.Kind = Entry.Kind;
			C.SchemaName = Entry.SchemaName;
			C.AdapterId = Entry.AdapterId;
			if (Entry.Kind == EHeldKind::Adapter && Entry.AdapterId != 0)
			{
				SuperSLMRuntime::FResolvedAdapter Resolved;
				if (SuperSLMRuntime::PinAdapter(Entry.AdapterId, Resolved))
				{
					C.Adapter = Resolved.Adapter;
					C.bJobPin = true;
					FMemory::Memcpy(C.AdapterHash, Resolved.ArtifactHash, 32);
				}
				else
				{
					C.bUnresolved = true;
				}
			}
			Entry.bCarried = true;
			Carried.Add(MoveTemp(C));
		}
		return Carried;
	}

	// Rule 4's applying, on the worker at the carrying job's head, before its own Layer-1 call:
	// each carried entry in ordinal order (the held list is kept in ordinal order).
	void ApplyCarriedOnWorker(sslm_model Model, sslm_seq Seq, const TArray<FCarriedRequest>& Carried, TArray<sslm_status>& OutStatuses)
	{
		OutStatuses.SetNum(Carried.Num());
		for (int32 I = 0; I < Carried.Num(); ++I)
		{
			const FCarriedRequest& C = Carried[I];
			if (C.Kind == EHeldKind::Schema)
			{
				sslm_schema Schema = SSLM_SCHEMA_NONE;
				if (!C.SchemaName.IsEmpty())
				{
					const sslm_status LookupStatus = sslm_schema_lookup(Model, TCHAR_TO_UTF8(*C.SchemaName), &Schema);
					if (LookupStatus != SSLM_OK)
					{
						OutStatuses[I] = LookupStatus;
						continue;
					}
				}
				OutStatuses[I] = sslm_seq_set_schema(Seq, Schema);
			}
			else
			{
				OutStatuses[I] = C.bUnresolved ? SSLM_INVALID_ARGUMENT : sslm_seq_set_adapter(Seq, C.Adapter);
			}
		}
	}

	// Plan §2.5 row 22: sslm_stats at the tail of each job that changes the sequence, on the
	// worker, so GetStats() never reads a live sequence from the game thread.
	void ReadStatsOnWorker(sslm_model Model, sslm_seq Seq, FSequenceJobResult& Out)
	{
		sslm_stats_out Raw = {};
		if (Seq != nullptr && sslm_stats(Model, Seq, &Raw) == SSLM_OK)
		{
			Out.Stats.DecodeStepCeiling = Raw.decode_step_ceiling;
			Out.Stats.DecodeStepActual = Raw.decode_step_actual;
			Out.Stats.ForcedTokenCount = Raw.forced_token_count;
			Out.Stats.KvBlocksResident = Raw.kv_blocks_resident;
			Out.Stats.bSchemaAccepting = Raw.schema_accepting != 0;
			// The context length is in the sequence's K/V stats, not sslm_stats_out (sslm_abi.h); it stays -1
			// when that read fails.
			sslm_seq_kv_stats_out Kv = {};
			Kv.struct_size = sizeof(Kv);
			if (sslm_seq_kv_stats(Seq, &Kv) == SSLM_OK)
			{
				Out.ContextLength = Kv.context_length;
			}
			Out.bStatsValid = true;
		}
	}

	// Schema fast-decode plan rev 1.3.5, §6 P1-X: one read-only forcedness query (cap 0) at the
	// job's tail, on the worker, after the job's Layer-1 call. It changes nothing (§5.3), so it is
	// made only for a sequence that reads forcedness and is otherwise invisible (P1X.12 (e)).
	void ReadForcedOnWorker(sslm_model Model, sslm_seq Seq, FSequenceJobResult& Out)
	{
		if (Seq == nullptr)
		{
			return;
		}
		Out.bForcedReadAttempted = true;
		int32_t Count = 0;
		int32_t Accepting = 0;
		if (sslm_seq_forced_tokens(Model, Seq, nullptr, 0, &Count, &Accepting) == SSLM_OK)
		{
			Out.bForcedRead = true;
			Out.ForcedCount = Count;
			Out.bForcedAccepting = Accepting != 0;
		}
	}

	// Plugin paging plan §3.2: the engine's page counts at a job's tail, on the worker (the verb
	// reads them under the pool's own lock). Budget mode only; a diagnostic, never a verdict.
	void ReadPoolStatsOnWorker(const sslm_kv_pool* PoolPtr, FJobOutput& Out)
	{
		if (PoolPtr == nullptr || *PoolPtr == nullptr)
		{
			return;
		}
		sslm_kv_pool_stats_out Raw = {};
		Raw.struct_size = static_cast<uint32_t>(sizeof(sslm_kv_pool_stats_out));
		if (sslm_kv_pool_stats(*PoolPtr, &Raw) == SSLM_OK)
		{
			Out.PoolStats = Raw;
			Out.bPoolStatsRead = true;
		}
	}

	// At the job's delivery: the snapshot GetKvPageStats() reports.
	void DeliverPoolSnapshot(FSuperSLMSubsystemState& S, const FJobOutput& Out)
	{
		if (Out.bPoolStatsRead)
		{
			S.bHasPoolSnapshot = true;
			S.LastPoolStats = Out.PoolStats;
		}
	}

	// Plugin paging plan §3.5 (step K3): ends the pin a hinted restore took on its hint prefix at
	// the call (FPrefixEntry::PendingHintedRestores), when its job delivers or it is refused. No-op
	// for 0 (no hint).
	void UnpinRestoreHint(FSuperSLMSubsystemState& S, int64 HintPrefixId)
	{
		if (HintPrefixId == 0)
		{
			return;
		}
		if (FPrefixEntry* Hint = S.Prefixes.Find(HintPrefixId))
		{
			Hint->PendingHintedRestores = FMath::Max(0, Hint->PendingHintedRestores - 1);
		}
	}

	// Plan §2.5 row 19: the last release of a restore blob runs on a worker, never the game thread.
	// A restore refused before any job (at its admission) hands its share to a lane as a
	// release-only job, as the teardown does; without a running lane it is released here.
	void ReleaseBlobOnWorker(FSuperSLMSubsystemState& S, FSharedBlob&& Blob)
	{
		for (FLane& Lane : S.Lanes)
		{
			if (Lane.Thread != nullptr && Lane.Runnable != nullptr)
			{
				FQueuedWorkerJobRef ReleaseJob = MakeShared<FQueuedWorkerJob, ESPMode::ThreadSafe>();
				ReleaseJob->Execute = [Held = MoveTemp(Blob)]() mutable { Held.Reset(); };
				Lane.Runnable->Enqueue(MoveTemp(ReleaseJob));
				return;
			}
		}
		Blob.Reset();
	}

	// Rule 4's confirmation, at the carrying job's delivery. Every job pin is released. Only
	// while the job's owner still holds the slot does the delivery touch the held list (rule 2):
	// an applied entry updates BoundSchemaName or ActiveAdapterId and is removed; an adapter
	// Layer 1 refused because the sequence was mid-token goes back to the list for the next job;
	// any other refusal removes the entry and faults the holder's generation by name -- at once
	// when the carrier belongs to that generation, otherwise through the deferred-refusal field
	// at the holder's next activation. An entry whose job never reached its head goes back too.
	void ConfirmCarried(FSuperSLMSubsystemState& S, FSlot& Slot, bool bOwnerHolds, const TArray<FCarriedRequest>& Carried,
		const TArray<sslm_status>& Statuses, bool bCarrierInGeneration)
	{
		for (int32 I = 0; I < Carried.Num(); ++I)
		{
			const FCarriedRequest& C = Carried[I];
			if (C.bJobPin)
			{
				SuperSLMRuntime::UnpinAdapter(C.AdapterId);
			}
			if (!bOwnerHolds)
			{
				continue; // ReleaseUser() already dropped the entry and its pin
			}
			const int32 EntryIndex = Slot.Held.IndexOfByPredicate([&C](const FHeldRequest& E) { return E.Ordinal == C.Ordinal; });
			if (EntryIndex == INDEX_NONE)
			{
				continue;
			}
			FHeldRequest& Entry = Slot.Held[EntryIndex];
			if (!Statuses.IsValidIndex(I))
			{
				Entry.bCarried = false; // not attempted: the next job carries it
				continue;
			}
			const sslm_status Status = Statuses[I];
			if (Status == SSLM_OK)
			{
				if (C.Kind == EHeldKind::Schema)
				{
					Slot.BoundSchemaName = C.SchemaName;
				}
				else
				{
					Slot.ActiveAdapterId = C.AdapterId;
					TRACE_BOOKMARK(TEXT("SuperSLM: adapter swap (sequence %lld, adapter %lld)"), Slot.VendedId, Slot.ActiveAdapterId);
				}
				if (Entry.bPinned)
				{
					SuperSLMRuntime::UnpinAdapter(Entry.AdapterId); // Layer 1's binding now holds its own reference
				}
				Slot.Held.RemoveAt(EntryIndex);
				continue;
			}
			if (C.Kind == EHeldKind::Adapter && Status == SSLM_ADAPTER_SWAP_MIDTOKEN_REJECTED)
			{
				Entry.bCarried = false; // the next job carries it again, at a later token boundary
				continue;
			}
			const FString Message = C.Kind == EHeldKind::Schema
				? FString::Printf(TEXT("the requested schema bind ('%s') was refused by Layer 1 (%s)"), *C.SchemaName, *StatusText(Status))
				: FString::Printf(TEXT("the requested adapter swap (adapter %lld) was refused by Layer 1 (%s)"), C.AdapterId, *StatusText(Status));
			if (Entry.bPinned)
			{
				SuperSLMRuntime::UnpinAdapter(Entry.AdapterId);
			}
			Slot.Held.RemoveAt(EntryIndex);
			if (bCarrierInGeneration && Slot.IsGenerating())
			{
				S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, Message);
			}
			else
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("Sequence %lld: %s; its next generation is faulted by name."), Slot.VendedId, *Message);
				Slot.DeferredRefusal = Slot.bHasDeferredRefusal ? Slot.DeferredRefusal + TEXT("; ") + Message : Message;
				Slot.bHasDeferredRefusal = true;
			}
		}
	}

	// Rule 2 and row 22: the stats copy is written only for the job's owner.
	void DeliverStats(FSlot& Slot, bool bOwnerHolds, const FSequenceJobResult& Result)
	{
		if (bOwnerHolds && Result.bStatsValid)
		{
			Slot.Stats = Result.Stats;
		}
	}

	// Creates the job's own ledger row (Reset/Adopt/Save/Restore/PrefixBegin/PrefixRelease/
	// DecodeOrPrefill -- the seven ESuperSLMWorkerJobKind values R-S1b/R-S1i/R-S1j read by name),
	// sizes PlannedJobMs/K from the static cost model, enqueues Execute on the lane's own worker
	// queue, and returns the job's own CommittedDeliveryTick so the caller can stamp every slot
	// this job touches with its own NextAvailableTick (schedule-only; never gated on the worker's
	// real completion).
	//
	// D-SLM7528 (plan §5 item 3): PostLedgerJob is the single function that calls FLane::Post, and
	// is therefore the single place a job's lane is chosen -- it takes the entity's own
	// (sticky) PinnedLane by reference and the tick's own candidate lane pool by reference, and
	// resolves the lane itself, via ChooseLane, as its first act, before anything else runs. No
	// Dispatch* function receives a LaneIndex parameter or calls FLane::Post itself; a caller that
	// needs the resolved lane (to reach that lane's own workspace, for a Layer-1 call that takes
	// one) gets it through BuildExecute's own int32 parameter, never by choosing it independently.
	// D-SLM7799: the dispatchers' pre-check, before any state changes. False means the job must not
	// be posted: the caller faults its member sequences with kSchedulingDomainFault (logged once).
	bool TrySchedule(FSuperSLMSubsystemState& S, double PlannedJobMs, int32 TickIndex, const TCHAR* JobKind)
	{
		int32 K = 0;
		int32 CommittedTick = 0;
		if (TryComputeK(PlannedJobMs, S.Config.TickBudgetMs, K) && TryCommitTick(TickIndex, K, CommittedTick))
		{
			return true;
		}
		S.LogSchedulingDefectOnce(FString::Printf(TEXT("%s job planned %f ms at tick %d (TickBudgetMs %f)"), JobKind, PlannedJobMs, TickIndex, S.Config.TickBudgetMs));
		return false;
	}

	int32 PostLedgerJob(
		FSuperSLMSubsystemState& S, int32& EntityPinnedLane, TArray<int32>& CandidateLanes, ESuperSLMWorkerJobKind Kind,
		int32 DecodeLayers, int32 PromptTokens, int32 TokenFinishes, double PlannedJobMs,
		const FSuperSLMSequence& LifecycleOpSequence, const FSuperSLMLifecycleOpHandle& LifecycleOpHandle,
		int32 TickIndex, TFunctionRef<TFunction<void()>(int32 LaneIndex)> BuildExecute, TFunction<void(double)> Deliver,
		int32 ChainBaseTick = INDEX_NONE, bool bAppendLedgerRow = true)
	{
		const int32 LaneIndex = ChooseLane(EntityPinnedLane, CandidateLanes);
		// 1.1 worker continuation (plan §5 item 1a): a continuation's delivery is committed from
		// the tick the job it follows is committed to (ChainBaseTick, PlanContinuations), never
		// earlier, so each lane's committed ticks stay in post order. Every other job commits from
		// TickIndex, as before.
		const bool bContinuation = ChainBaseTick != INDEX_NONE;
		const int32 BaseTick = bContinuation ? FMath::Max(TickIndex, ChainBaseTick) : TickIndex;

		// The ring's next row, cleared for reuse: writing it never allocates, because Configure()
		// sized every row's member list for the largest batch. Schema fast-decode plan rev 1.3.5, §6:
		// a span job (bAppendLedgerRow false) takes its JobId from the same counter but appends no
		// row, so JobId alone orders it against the rows around it (cells P1.2, P1X.2); its figures
		// live in a local report and reach the lane's pending entry only.
		FSuperSLMWorkerJobReport SpanJobReport;
		const int64 LedgerRow = bAppendLedgerRow ? S.JobLedger.AppendedCount() : static_cast<int64>(INDEX_NONE);
		FSuperSLMWorkerJobReport& Job = bAppendLedgerRow ? S.JobLedger.Append() : SpanJobReport;
		ResetJobRow(Job);
		Job.JobId = S.NextJobId++;
		Job.Kind = Kind;
		Job.DecodeLayers = DecodeLayers;
		Job.PromptTokens = PromptTokens;
		Job.TokenFinishes = TokenFinishes;
		Job.LifecycleOpSequence = LifecycleOpSequence;
		Job.LifecycleOpHandle = LifecycleOpHandle;
		if (LifecycleOpSequence.IsValid())
		{
			Job.MemberSequences.Add(LifecycleOpSequence); // a decode batch sets its members after post
		}
		Job.PlannedJobMs = PlannedJobMs;
		// D-SLM7799: every dispatcher calls TrySchedule() on these same inputs before it changes any
		// state, so both guards hold here. Should one ever fail, the job's schedule is clamped into
		// range and the defect logged, never wrapped.
		int32 K = 0;
		int32 CommittedTick = 0;
		if (!TryComputeK(PlannedJobMs, S.Config.TickBudgetMs, K) || !TryCommitTick(BaseTick, K, CommittedTick))
		{
			S.LogSchedulingDefectOnce(FString::Printf(TEXT("PostLedgerJob: planned %f ms at tick %d"), PlannedJobMs, BaseTick));
			K = kMaxJobTicks;
			CommittedTick = BaseTick > TNumericLimits<int32>::Max() - K ? TNumericLimits<int32>::Max() : BaseTick + K;
		}
		Job.K = K;
		Job.PlannedAtTick = TickIndex;
		Job.ChainedWaitTicks = BaseTick - TickIndex;
		Job.bContinuation = bContinuation;
		Job.CommittedDeliveryTick = CommittedTick;
		Job.DeliveredAtTick = -1;

		FLane& Lane = S.Lanes[LaneIndex];
		FQueuedWorkerJobRef WorkerJob = MakeShared<FQueuedWorkerJob, ESPMode::ThreadSafe>();
		WorkerJob->Execute = BuildExecute(LaneIndex);
		Lane.Post(WorkerJob, MoveTemp(Deliver), LedgerRow, Job);
		// Plugin paging plan §3.9: the HoldApply() hold the dispatcher named for this job, if any.
		Lane.Pending.Last().ApplyHoldId = S.NextPostApplyHoldId;
		S.NextPostApplyHoldId = 0;
		Lane.Pending.Last().bSpanJob = !bAppendLedgerRow;
		return Job.CommittedDeliveryTick;
	}

	// A reset job: a caller's ResetSequence(), or the recycle ReleaseUser() appends (plan §2.5
	// rows 7, 17, 20). Layer 1 1.6.0+ resets every valid sequence state, mid-token included, so a
	// live sequence is always reset in place; a fresh sequence is created only for a slot whose
	// sequence a failed restore released.
	void DispatchReset(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const FSlotQueuedOp Op = Slot.OpQueue[0];
		Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
		++Slot.OutstandingJobCount;

		// Rule 2: the job's owner, checked again at delivery (the holder can return the slot while
		// the job is in flight). The recycle has no owner.
		const int64 Owner = Op.Owner;
		const bool bOwnerHoldsAtPost = Owner != 0 && Owner == Slot.VendedId;
		const bool bUnbind = Op.bUnbind;

		// D-SLM7946: the holder's reset is in flight until its delivery, which clears this on
		// every path. A generation requested meanwhile projects Idle (ProjectGenerationTurn()).
		if (bOwnerHoldsAtPost)
		{
			Slot.InFlightResetOwner = Owner;
		}

		// Rule 4: a caller's reset carries the holder's held entries below its ordinal; the
		// recycle carries nothing.
		const TArray<FCarriedRequest> Carried = bOwnerHoldsAtPost ? CarryHeldRequests(Slot, Op.Ordinal, false) : TArray<FCarriedRequest>();

		// Plugin paging plan §3.5 "Normalization" (step K3): the ledger made this reset a create-first
		// normalization at its admission. The job creates the configured budgeted holder first and
		// releases the old one only once the create has succeeded; a refused create keeps the old
		// holder and resets it in place.
		const bool bNormalize = Op.bNormalizes && Slot.Seq != nullptr;

		// Row 7: the binding re-apply after a recreating reset re-establishes what sslm_seq_reset
		// would have kept. The bindings are caller-owned, so it is composed only for an owner that
		// holds the slot; the recycle a refused restore queues has no owner and re-applies nothing.
		// A normalization recreates the holder too, so it re-applies them on the new one.
		const bool bReapply = bOwnerHoldsAtPost && (Slot.Seq == nullptr || bNormalize);
		const FString ReapplySchemaName = bReapply ? Slot.BoundSchemaName : FString();
		int64 ReapplyAdapterId = 0;
		sslm_adapter ReapplyAdapter = nullptr;
		if (bReapply && Slot.ActiveAdapterId != 0)
		{
			SuperSLMRuntime::FResolvedAdapter Resolved;
			if (SuperSLMRuntime::PinAdapter(Slot.ActiveAdapterId, Resolved))
			{
				ReapplyAdapterId = Slot.ActiveAdapterId;
				ReapplyAdapter = Resolved.Adapter;
			}
		}

		const sslm_model Model = S.Model;
		sslm_kv_pool* PoolPtr = &S.Pool;
		const sslm_seq OldSeq = Slot.Seq;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);
		const bool bReapplySchema = bReapply && !ReapplySchemaName.IsEmpty();
		const bool bReapplyAdapter = ReapplyAdapter != nullptr;
		// Plugin paging plan §3.2: budget mode recreates a holder with the configured budget (the
		// slot's reservation in region S), and reads the pool's page counts at the job's tail.
		const bool bBudgetMode = S.IsBudgetMode();
		const int32 SequenceBudget = S.Config.SequenceBudget;
#if WITH_DEV_AUTOMATION_TESTS
		// The test hooks are one-shot: this job, the next reset dispatched, takes them. The paged
		// seam's override (§3.9: Reset, or Return for the recycle) wins; the scheduling seam's
		// is then left for the next reset.
		const sslm_status PagedOverride = S.TakeLayer1Override(bUnbind ? EPagedJobKind::Return : EPagedJobKind::Reset);
		if (PagedOverride != SSLM_OK)
		{
			Out->TestResetStatus = PagedOverride;
		}
		else
		{
			Out->TestResetStatus = S.TestNextResetStatus;
			S.TestNextResetStatus = SSLM_OK;
		}
#endif

		auto BuildExecute = [Model, PoolPtr, OldSeq, Carried, bUnbind, bReapplySchema, ReapplySchemaName, bReapplyAdapter, ReapplyAdapter, bOwnerHoldsAtPost, bBudgetMode, SequenceBudget, bNormalize, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, PoolPtr, OldSeq, Carried, bUnbind, bReapplySchema, ReapplySchemaName, bReapplyAdapter, ReapplyAdapter, bOwnerHoldsAtPost, bBudgetMode, SequenceBudget, bNormalize, Out]()
			{
				ON_SCOPE_EXIT
				{
					if (bBudgetMode)
					{
						ReadPoolStatsOnWorker(PoolPtr, *Out);
					}
				};
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Reset", SuperSLMChannel);
				FSequenceJobResult& Result = Out->PerSequence[0];
				sslm_seq Seq = OldSeq;
				if (bNormalize)
				{
					// Create-first (§3.5): the new holder draws R(SequenceBudget), charged to H at
					// admission, before the old holder is released. The test hooks' status, when set,
					// stands in for the create.
					sslm_seq Fresh = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
					Out->CreateStatus = Out->TestResetStatus != SSLM_OK ? Out->TestResetStatus
						: sslm_seq_create_budgeted(Model, PoolPtr, SequenceBudget, &Fresh);
					// Taken by the create: a refused create's fallback below is the real reset.
					Out->TestResetStatus = SSLM_OK;
#else
					Out->CreateStatus = sslm_seq_create_budgeted(Model, PoolPtr, SequenceBudget, &Fresh);
#endif
					if (Out->CreateStatus == SSLM_OK)
					{
						const sslm_status ReleaseStatus = sslm_seq_release(OldSeq);
						if (ReleaseStatus == SSLM_OK)
						{
							Out->bNormalized = true;
							Out->bBoolFlag = true; // recreated: the tail below re-applies and carries onto it
							Out->Status = SSLM_OK;
							Seq = Fresh;
						}
						else
						{
							// The old holder is kept as it was; the new one goes back to the pool.
							sslm_seq_release(Fresh);
							Out->CreateStatus = ReleaseStatus;
						}
					}
				}
				// A normalized holder is new, so nothing below resets it.
				if (!Out->bNormalized && OldSeq != nullptr)
				{
					ApplyCarriedOnWorker(Model, OldSeq, Carried, Result.CarriedStatuses); // rule 4: at the job's head
#if WITH_DEV_AUTOMATION_TESTS
					Out->Status = Out->TestResetStatus != SSLM_OK ? Out->TestResetStatus : sslm_seq_reset(OldSeq);
#else
					Out->Status = sslm_seq_reset(OldSeq);
#endif
				}
				else if (!Out->bNormalized)
				{
#if WITH_DEV_AUTOMATION_TESTS
					Out->Status = Out->TestResetStatus != SSLM_OK ? Out->TestResetStatus
						: (bBudgetMode ? sslm_seq_create_budgeted(Model, PoolPtr, SequenceBudget, &Seq) : sslm_seq_create(Model, PoolPtr, &Seq));
#else
					Out->Status = bBudgetMode ? sslm_seq_create_budgeted(Model, PoolPtr, SequenceBudget, &Seq) : sslm_seq_create(Model, PoolPtr, &Seq);
#endif
					Out->bBoolFlag = true; // recreated
					if (Out->Status != SSLM_OK)
					{
						Seq = nullptr;
					}
				}
				Out->SeqOut = Seq;
				if (Out->Status != SSLM_OK)
				{
					return;
				}
				if (Out->bBoolFlag)
				{
					if (bReapplySchema)
					{
						sslm_schema Schema = SSLM_SCHEMA_NONE;
						Out->ReapplySchemaStatus = sslm_schema_lookup(Model, TCHAR_TO_UTF8(*ReapplySchemaName), &Schema);
						if (Out->ReapplySchemaStatus == SSLM_OK)
						{
							Out->ReapplySchemaStatus = sslm_seq_set_schema(Seq, Schema);
						}
					}
					if (bReapplyAdapter)
					{
						Out->ReapplyAdapterStatus = sslm_seq_set_adapter(Seq, ReapplyAdapter);
					}
					// No sequence existed at the head of this job, so its carried entries apply
					// to the one it created.
					ApplyCarriedOnWorker(Model, Seq, Carried, Result.CarriedStatuses);
				}
				if (bUnbind)
				{
					// Row 17: after a 1.7.0 reset the sequence is bind-eligible and at
					// layer_index 0, so Layer 1 admits both.
					Out->UnbindSchemaStatus = sslm_seq_set_schema(Seq, SSLM_SCHEMA_NONE);
					Out->UnbindAdapterStatus = sslm_seq_set_adapter(Seq, nullptr);
				}
				if (bOwnerHoldsAtPost)
				{
					ReadStatsOnWorker(Model, Seq, Result);
				}
			};
		};

		const int64 HandleId = Op.HandleId;
		const SuperSLMPaging::FTicket LedgerTicket = Op.LedgerTicket;
		auto Deliver = [&S, SlotIndex, HandleId, Owner, bUnbind, Carried, ReapplyAdapterId, LedgerTicket, Out](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount;
			// D-SLM7946: the reset is no longer in flight, whether it succeeded or was refused and
			// whoever holds the slot now.
			Slot.InFlightResetOwner = 0;
			// Plugin paging plan §3.2/§3.5: the reset's admission is applied with what the job did. A
			// reset in place (or a recreate, which takes the configured shape) returns the holder's
			// materialized pages and drops its prefix dependency; a refused one changes nothing. A
			// create-first normalization (§3.5, K3) that replaced the holder applies Normalized; one
			// whose create was refused reset the old holder in place, which the ledger reads as
			// ResetInPlace (normalization is retried at the next reset).
			DeliverPoolSnapshot(S, *Out);
			if (LedgerTicket != SuperSLMPaging::InvalidTicket)
			{
				S.Ledger.ApplyReset(LedgerTicket, Out->bNormalized ? SuperSLMPaging::EResetResult::Normalized
					: (Out->Status == SSLM_OK ? SuperSLMPaging::EResetResult::ResetInPlace : SuperSLMPaging::EResetResult::Refused));
			}
			if (Out->Status == SSLM_KV_POOL_EXHAUSTED && S.IsBudgetMode())
			{
				S.NoteLedgerFault(TEXT("sslm_seq_create_budgeted (a reset's recreate)"));
			}
			if (Out->CreateStatus == SSLM_KV_POOL_EXHAUSTED && S.IsBudgetMode())
			{
				S.NoteLedgerFault(TEXT("sslm_seq_create_budgeted (a normalization's create)"));
			}
			if (Out->CreateStatus != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Log, TEXT("Slot %d: normalization's create was refused (%s); the holder was reset in place and is normalized at its next reset."),
					SlotIndex, *StatusText(Out->CreateStatus));
			}
			if (ReapplyAdapterId != 0)
			{
				SuperSLMRuntime::UnpinAdapter(ReapplyAdapterId);
			}
			const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;
			const FSequenceJobResult& Result = Out->PerSequence[0];
			ConfirmCarried(S, Slot, bOwnerHolds, Carried, Result.CarriedStatuses, /*bCarrierInGeneration*/ false);

			if (Out->Status != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Sequence %s failed: %s refused (%s)."), bUnbind ? TEXT("recycle") : TEXT("reset"),
					Out->bBoolFlag ? TEXT("sslm_seq_create") : TEXT("sslm_seq_reset"), *StatusText(Out->Status));
				if (Out->bBoolFlag)
				{
					Slot.Seq = nullptr;
					// D-SLM7530 (plan §5 item 3): a recreated physical identity resets PinnedLane
					// exactly as a public VendSequence() does.
					Slot.PinnedLane = INDEX_NONE;
				}
				// §18: the Layer-1 mirrors settle to a sane, non-adopted, zero-context default.
				Slot.ContextUsed = 0;
				Slot.bAdopted = false;
				Slot.bAdoptedPromptOnlyPrefix = false;
				Slot.AdoptedPrefixLength = 0;
				if (bUnbind)
				{
					S.WithholdSlot(SlotIndex, FString::Printf(TEXT("the recycle's %s was refused (%s)"),
						Out->bBoolFlag ? TEXT("sslm_seq_create") : TEXT("sslm_seq_reset"), *StatusText(Out->Status)));
				}
				// D-SLM7947: a caller's reset Layer 1 refused faults the sequence, as a refused GPU
				// reset does, and stops a generation still running. Its generation state, the
				// previous generation's tokens included, is kept (D-SLM7948). A generation queued
				// behind it is dropped at its turn (PlanNextJobs() step 0).
				if (bOwnerHolds)
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(
						TEXT("%s returned %s; the sequence was not reset and is Faulted until a reset succeeds"),
						Out->bBoolFlag ? TEXT("sslm_seq_create") : TEXT("sslm_seq_reset"), *StatusText(Out->Status)));
				}
				if (HandleId != 0)
				{
					// K2 review F4 (§3.7): in budget mode an engine exhaustion on an admitted reset is
					// named KvPagesExhausted (and was counted as a ledger fault above).
					S.HandleResults.FindOrAdd(HandleId).Result = (S.IsBudgetMode() && Out->Status == SSLM_KV_POOL_EXHAUSTED)
						? ESuperSLMRestoreResult::KvPagesExhausted : ESuperSLMRestoreResult::Malformed;
				}
				return;
			}

			// Slot-owned: the job that changed Layer 1 writes its mirrors, whoever holds the slot.
			Slot.Seq = Out->SeqOut;
			Slot.LayersDoneInToken = 0;
			Slot.bReadyForLogits = false;
			Slot.ContextUsed = 0;
			Slot.bAdopted = false;
			Slot.bAdoptedPromptOnlyPrefix = false;
			Slot.AdoptedPrefixLength = 0;
			if (Out->bBoolFlag)
			{
				Slot.PinnedLane = INDEX_NONE; // D-SLM7530: the same reset path as VendSequence()
				++Slot.HolderGeneration;      // §3.9: a new Layer-1 holder
			}

			if (bOwnerHolds)
			{
				// T-2815 round 14: a caller's reset returns the sequence to Idle, which SetSchema()'s
				// reset-then-bind recovery path (plan §9 R-S1f) depends on. D-SLM7949: it also clears
				// the previous generation's request and tokens, as a successful GPU reset does, so
				// both backends read Idle with no tokens after a successful reset.
				Slot.ClearForBegin();
				if (Out->bBoolFlag && Out->ReapplySchemaStatus != SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("Sequence %lld: re-binding schema '%s' on the recreated sequence failed (%s); it is now unbound."),
						Slot.VendedId, *Slot.BoundSchemaName, *StatusText(Out->ReapplySchemaStatus));
					Slot.BoundSchemaName.Reset();
				}
				if (Out->bBoolFlag && Out->ReapplyAdapterStatus != SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("Sequence %lld: re-binding adapter %lld on the recreated sequence failed (%s); it now runs on the base model."),
						Slot.VendedId, Slot.ActiveAdapterId, *StatusText(Out->ReapplyAdapterStatus));
					Slot.ActiveAdapterId = 0;
				}
				DeliverStats(Slot, true, Result);
			}

			if (bUnbind && (Out->UnbindSchemaStatus != SSLM_OK || Out->UnbindAdapterStatus != SSLM_OK))
			{
				S.WithholdSlot(SlotIndex, FString::Printf(TEXT("the recycle's unbind was refused (sslm_seq_set_schema %s, sslm_seq_set_adapter %s)"),
					*StatusText(Out->UnbindSchemaStatus), *StatusText(Out->UnbindAdapterStatus)));
			}
			if (HandleId != 0)
			{
				S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Success;
			}
		};

		S.NextPostApplyHoldId = Op.ApplyHoldId; // §3.9: HoldApply(Reset or Return)
		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Reset, 0, 0, 0, S.Resolved.ResetCostMs,
			FSuperSLMSequence{Slot.VendedId}, FSuperSLMLifecycleOpHandle{HandleId}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	void DispatchAdopt(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const FSlotQueuedOp Op = Slot.OpQueue[0];
		Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
		++Slot.OutstandingJobCount;

		const int64 Owner = Op.Owner;
		const bool bOwnerHoldsAtPost = Owner != 0 && Owner == Slot.VendedId;
		const TArray<FCarriedRequest> Carried = bOwnerHoldsAtPost ? CarryHeldRequests(Slot, Op.Ordinal, false) : TArray<FCarriedRequest>();

		FPrefixEntry* Prefix = S.Prefixes.Find(Op.PrefixId);
		const bool bPrefixOk = Prefix != nullptr && Prefix->Phase == ESuperSLMPrefixPhase::Ready;
		const FString FaultText = Prefix == nullptr
			? TEXT("prefix released before this adopt could run")
			: (bPrefixOk ? FString() : FString::Printf(TEXT("prefix faulted (%s)"), *Prefix->FaultMessage));
		// The prefix's length: a nested child's positions start at its parent's length (§3.3, K3).
		const int64 PrefixConsumedSnapshot = Prefix ? Prefix->OriginLength + Prefix->Consumed : 0;

		const sslm_model Model = S.Model;
		const sslm_seq SeqHandle = Slot.Seq;
		const sslm_prefix PrefixHandle = bPrefixOk ? Prefix->Handle : nullptr;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);
		// Plugin paging plan §3.9: the paged seam's one-shot status for this adopt, in place of the
		// Layer-1 call. Budget mode reads the pool's page counts at the job's tail (§3.2).
		const sslm_status ForcedStatus = S.TakeLayer1Override(EPagedJobKind::Adopt);
		const bool bBudgetMode = S.IsBudgetMode();
		sslm_kv_pool* PoolPtr = &S.Pool;

		auto BuildExecute = [Model, SeqHandle, PrefixHandle, bPrefixOk, Carried, bOwnerHoldsAtPost, ForcedStatus, bBudgetMode, PoolPtr, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, SeqHandle, PrefixHandle, bPrefixOk, Carried, bOwnerHoldsAtPost, ForcedStatus, bBudgetMode, PoolPtr, Out]()
			{
				FSequenceJobResult& Result = Out->PerSequence[0];
				ApplyCarriedOnWorker(Model, SeqHandle, Carried, Result.CarriedStatuses); // rule 4: at the job's head
				if (!bPrefixOk)
				{
					Out->bSkipped = true;
					return;
				}
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Adopt", SuperSLMChannel);
				Out->Status = ForcedStatus != SSLM_OK ? ForcedStatus : sslm_seq_adopt_prefix(SeqHandle, PrefixHandle);
				if (bOwnerHoldsAtPost)
				{
					ReadStatsOnWorker(Model, SeqHandle, Result);
				}
				if (bBudgetMode)
				{
					ReadPoolStatsOnWorker(PoolPtr, *Out);
				}
			};
		};

		const int64 HandleId = Op.HandleId;
		const int64 PrefixIdCap = Op.PrefixId;
		const SuperSLMPaging::FTicket LedgerTicket = Op.LedgerTicket;
		auto Deliver = [&S, SlotIndex, HandleId, Owner, PrefixIdCap, FaultText, Carried, Out, PrefixConsumedSnapshot, LedgerTicket](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount;
			if (FPrefixEntry* Prefix = S.Prefixes.Find(PrefixIdCap))
			{
				Prefix->PendingAdopts = FMath::Max(0, Prefix->PendingAdopts - 1);
			}
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::OnAdoptDelivered(S, PrefixIdCap); // precache plan §5.6
#endif
			// Plugin paging plan §3.3 (UP-14): the adopt's admission applies with its outcome. Success
			// drops the slot's previous dependency (which may refund a ghost), registers it on the
			// prefix, returns its materialized pages and sets its origin to the prefix's length, which
			// is what the generation limit reads (SlotLimit). A budget adopt draws nothing, so it can
			// never meet the engine's KV_POOL_EXHAUSTED.
			DeliverPoolSnapshot(S, *Out);
			if (LedgerTicket != SuperSLMPaging::InvalidTicket)
			{
				S.Ledger.ApplyAdopt(LedgerTicket, !Out->bSkipped && Out->Status == SSLM_OK);
			}
			const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;
			ConfirmCarried(S, Slot, bOwnerHolds, Carried, Out->PerSequence[0].CarriedStatuses, /*bCarrierInGeneration*/ false);
			if (Out->bSkipped || Out->Status != SSLM_OK)
			{
				const FString Msg = Out->bSkipped ? FaultText : FString::Printf(TEXT("sslm_seq_adopt_prefix failed (%s)"), *StatusText(Out->Status));
				if (bOwnerHolds)
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(TEXT("AdoptPrefix: %s"), *Msg));
				}
				if (HandleId != 0)
				{
					S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Malformed;
				}
				return;
			}
			// Slot-owned mirrors: the copy carries the prefix's context and its last token's
			// computed state, so the next decode call finishes that token when the prefix
			// prefilled at least one.
			Slot.bAdopted = true;
			Slot.bAdoptedPromptOnlyPrefix = true; // every CPU prefix is prompt-only (§6 P1)
			Slot.AdoptedPrefixLength = PrefixConsumedSnapshot;
			Slot.ContextUsed = PrefixConsumedSnapshot;
			Slot.bReadyForLogits = PrefixConsumedSnapshot > 0;
			Slot.LayersDoneInToken = 0;
			DeliverStats(Slot, bOwnerHolds, Out->PerSequence[0]);
			if (HandleId != 0)
			{
				S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Success;
			}
		};

		S.NextPostApplyHoldId = Op.ApplyHoldId; // §3.9: HoldApply(Adopt)
		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Adopt, 0, 0, 0, S.Resolved.AdoptCostMs,
			FSuperSLMSequence{Slot.VendedId}, FSuperSLMLifecycleOpHandle{HandleId}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	void DispatchSave(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const FSlotQueuedOp Op = Slot.OpQueue[0];
		Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
		++Slot.OutstandingJobCount;

		const int64 Owner = Op.Owner;
		const bool bOwnerHoldsAtPost = Owner != 0 && Owner == Slot.VendedId;
		const TArray<FCarriedRequest> Carried = bOwnerHoldsAtPost ? CarryHeldRequests(Slot, Op.Ordinal, false) : TArray<FCarriedRequest>();

		// D-SLM7341: the wrapper carries the bound adapter, because Layer 1's 'SSB5' does not. The
		// adapter bound before this job's head is resolved here for its hash; what the job's own
		// carried entries bind is resolved through their job pins (rule 4, T-2987 F7).
		const int64 ActiveAdapterAtPost = Slot.ActiveAdapterId;
		uint8 ActiveAdapterHash[32] = {};
		bool bActiveAdapterResolved = true;
		if (ActiveAdapterAtPost != 0)
		{
			SuperSLMRuntime::FResolvedAdapter Resolved;
			bActiveAdapterResolved = SuperSLMRuntime::ResolveAdapter(ActiveAdapterAtPost, Resolved);
			if (bActiveAdapterResolved)
			{
				FMemory::Memcpy(ActiveAdapterHash, Resolved.ArtifactHash, 32);
			}
		}
		const FString BoundSchemaAtPost = Slot.BoundSchemaName;

		const sslm_seq SeqHandle = Slot.Seq;
		const int64 SaveStagingBytes = S.SaveStagingBytes;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);

		// Snapshots of the plugin's generation state at post. Under row 22's gates the save is
		// posted after every earlier job has delivered, so each is exact (fold 10 retires the
		// post/delivery split, D-SLM7730), the remaining budget included (row 24's CPU twin).
		const ESuperSLMSequencePhase PhaseSnapshot = Slot.Phase;
		const ESuperSLMSpanKind SpanKindSnapshot = Slot.SpanKind;
		const bool bReadyForLogitsSnapshot = Slot.bReadyForLogits;
		const int32 MaxNewRemainingSnapshot = FMath::Max(0, Slot.MaxNewTokens - Slot.Generated.Num());
		const int32 LayerBudgetSnapshot = Slot.PinnedLayerBudget;
		const int32 LayersDoneSnapshot = Slot.LayersDoneInToken;
		const int64 ContextUsedSnapshot = Slot.ContextUsed;
		TArray<int32> PromptRemainingSnapshot;
		if (Slot.PromptConsumed < Slot.Prompt.Num())
		{
			PromptRemainingSnapshot.Append(Slot.Prompt.GetData() + Slot.PromptConsumed, Slot.Prompt.Num() - Slot.PromptConsumed);
		}
		const TArray<int32> StopTokenIdsSnapshot = Slot.StopTokenIds;

		// U1 round 6 (R-S1g's class, row 19's save twin): the wrapper is written on the worker, so
		// the game thread never copies or frees a save's payload -- for a real model that payload
		// is the whole KV block. Everything the header carries is either a post-time snapshot or
		// read by this job on the worker. Delivery moves the finished blob into the handle.
		SuperSLMSaveBlob::FContents HeaderAtPost;
		HeaderAtPost.Backend = ESuperSLMBackend::CPU;
		HeaderAtPost.Phase = PhaseSnapshot;
		HeaderAtPost.SpanKind = SpanKindSnapshot;
		HeaderAtPost.bReadyForLogits = bReadyForLogitsSnapshot;
		HeaderAtPost.Layer1Tag = SuperSLMSaveBlob::CompiledLayer1Tag();
		HeaderAtPost.Layer1Commit = SuperSLMSaveBlob::CompiledLayer1Commit();
		FMemory::Memcpy(HeaderAtPost.ArtifactHash, S.ArtifactHash, 32);
		HeaderAtPost.MaxNewTokensRemaining = MaxNewRemainingSnapshot;
		HeaderAtPost.LayerBudget = LayerBudgetSnapshot;
		HeaderAtPost.LayersDoneInToken = LayersDoneSnapshot;
		HeaderAtPost.ContextUsed = ContextUsedSnapshot;
		HeaderAtPost.PromptRemaining = PromptRemainingSnapshot;
		HeaderAtPost.StopTokenIds = StopTokenIdsSnapshot;

		auto BuildExecute = [&S, SeqHandle, SaveStagingBytes, Carried, BoundSchemaAtPost, ActiveAdapterAtPost, ActiveAdapterHash, LayersDoneSnapshot, HeaderAtPost, Out](int32 LaneIndex) -> TFunction<void()>
		{
			void* SaveStaging = S.Lanes[LaneIndex].SaveStaging; // row 22: this lane's own buffer
			const sslm_model Model = S.Model;
			return [Model, SeqHandle, SaveStaging, SaveStagingBytes, Carried, BoundSchemaAtPost, ActiveAdapterAtPost, ActiveAdapterHash, LayersDoneSnapshot, HeaderAtPost, Out]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Save", SuperSLMChannel);
				FSequenceJobResult& Result = Out->PerSequence[0];
				ApplyCarriedOnWorker(Model, SeqHandle, Carried, Result.CarriedStatuses); // rule 4: at the job's head

				// Rule 4 (T-2987 F7): what the sequence has bound at the save's position, from this
				// job's own apply results over what was bound before its head.
				FString SchemaName = BoundSchemaAtPost;
				int64 AdapterId = ActiveAdapterAtPost;
				uint8 AdapterHash[32];
				FMemory::Memcpy(AdapterHash, ActiveAdapterHash, 32);
				for (int32 I = 0; I < Carried.Num(); ++I)
				{
					if (Result.CarriedStatuses[I] != SSLM_OK)
					{
						continue;
					}
					if (Carried[I].Kind == EHeldKind::Schema)
					{
						SchemaName = Carried[I].SchemaName;
					}
					else
					{
						AdapterId = Carried[I].AdapterId;
						FMemory::Memcpy(AdapterHash, Carried[I].AdapterHash, 32);
					}
				}
				// And from Layer 1: sslm_seq_schema_bound (1.6.0) says whether a schema is bound.
				int32_t bSchemaBound = SchemaName.IsEmpty() ? 0 : 1;
				const sslm_status BoundStatus = sslm_seq_schema_bound(SeqHandle, &bSchemaBound);
				if (BoundStatus != SSLM_OK)
				{
					Out->SaveRefusal = FString::Printf(TEXT("sslm_seq_schema_bound failed (%s)"), *StatusText(BoundStatus));
					Out->bSkipped = true;
					return;
				}
				if (bSchemaBound == 0)
				{
					SchemaName.Reset();
				}
				else if (SchemaName.IsEmpty())
				{
					Out->SaveRefusal = TEXT("Layer 1 reports a schema bound that the plugin has no record of, so the save cannot name it");
					Out->bSkipped = true;
					return;
				}
				// Rule 4 (T-2989 F22): the mid-token adapter refusal is evaluated here, against what
				// is bound after the head. No ABI verb reports a CPU sequence's layer_index; the
				// plugin's mirror, taken at post, equals it under row 22's gates (every earlier job
				// has delivered, and applying a binding does not move it).
				if (AdapterId != 0 && LayersDoneSnapshot > 0)
				{
					Out->SaveRefusal = TEXT("the sequence has an adapter bound and is mid-token; save it at a token boundary, because Layer 1 re-binds an adapter only there");
					Out->bSkipped = true;
					return;
				}
				Out->SavedSchemaName = SchemaName;
				Out->bSavedAdapter = AdapterId != 0;
				FMemory::Memcpy(Out->SavedAdapterHash, AdapterHash, 32);

				size_t PayloadBytes = static_cast<size_t>(SaveStagingBytes);
				Out->Status = sslm_seq_save(SeqHandle, SaveStaging, &PayloadBytes);
				if (Out->Status == SSLM_OK)
				{
					// The finished wrapper, header and payload, built here on the worker.
					SuperSLMSaveBlob::FContents Contents = HeaderAtPost;
					Contents.bHasAdapter = Out->bSavedAdapter;
					if (Out->bSavedAdapter)
					{
						FMemory::Memcpy(Contents.AdapterHash, Out->SavedAdapterHash, 32);
					}
					Contents.SchemaName = Out->SavedSchemaName;
					const int64 Payload = static_cast<int64>(PayloadBytes);
					const int64 PayloadOffset = SuperSLMSaveBlob::BeginWrite(Out->Blob, Contents, Payload);
					FMemory::Memcpy(Out->Blob.GetData() + PayloadOffset, SaveStaging, static_cast<SIZE_T>(Payload));
					SuperSLMSaveBlob::FinalizePayload(Out->Blob, PayloadOffset, Payload);
				}
			};
		};

		const int64 HandleId = Op.HandleId;
		auto Deliver = [&S, SlotIndex, HandleId, Owner, Carried, Out, bActiveAdapterResolved](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount;
			const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;
			ConfirmCarried(S, Slot, bOwnerHolds, Carried, Out->PerSequence[0].CarriedStatuses, /*bCarrierInGeneration*/ false);

			auto Fail = [&](const FString& Why)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("SaveSequence: %s"), *Why);
				if (HandleId != 0)
				{
					S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Malformed;
				}
			};
			if (!bActiveAdapterResolved)
			{
				Fail(TEXT("the sequence's active adapter is no longer registered"));
				return;
			}
			if (Out->bSkipped)
			{
				Fail(Out->SaveRefusal);
				return;
			}
			if (Out->Status != SSLM_OK)
			{
				Fail(FString::Printf(TEXT("sslm_seq_save failed (%s)."), *StatusText(Out->Status)));
				return;
			}

			// The wrapper was finished on the worker; delivery moves it, never copies it.
			if (HandleId != 0)
			{
				FSuperSLMSubsystemState::FHandleResult& Entry = S.HandleResults.FindOrAdd(HandleId);
				Entry.Result = ESuperSLMRestoreResult::Success;
				S.RetainedResultBytes -= Entry.Blob.Num();
				Entry.Blob = MoveTemp(Out->Blob);
				S.RetainedResultBytes += Entry.Blob.Num();
			}
		};

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Save, 0, 0, 0, S.Resolved.SaveCostMs,
			FSuperSLMSequence{Slot.VendedId}, FSuperSLMLifecycleOpHandle{HandleId}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	void DispatchRestore(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		// Row 19 (U1 round 6, R-S1g): the op and its blob share are MOVED out of the queue, so from
		// here on the worker's closure is the blob's only owner, and it drops that share on the
		// worker (below). The game thread never runs the last release of a restore blob -- for a
		// real model that blob carries the whole KV block, and freeing it in Apply cost 39 ms.
		FSlotQueuedOp Op = MoveTemp(Slot.OpQueue[0]);
		Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
		++Slot.OutstandingJobCount;
		// D-SLM7946: the op has left OpQueue, so the one-generation projection reads the phase this
		// restore gives from the slot while bAwaitingRestore holds (ProjectGenerationTurn()).
		Slot.InFlightRestorePhase = Op.RestoreProjectedPhase;

		const int64 Owner = Op.Owner;
		const bool bOwnerHoldsAtPost = Owner != 0 && Owner == Slot.VendedId;

		// Row 19: the blob accepted at RestoreSequence() is shared, never copied, from here to the
		// worker.
		FSharedBlob Blob = MoveTemp(Op.RestoreBlob);
		SuperSLMSaveBlob::FContents Contents;
		const uint8* Payload = nullptr;
		int64 PayloadBytes = 0;
		FString ParseError;
		const bool bParsed = Blob.IsValid() && SuperSLMSaveBlob::Read(*Blob, Contents, Payload, PayloadBytes, ParseError);

		// The blob is bound to the artifact hash it was saved against (§4) -- checked here
		// (against the EXPECTED model's own hash, snapshotted synchronously in RestoreSequence())
		// so the worker performs the comparison the same way Layer 1's own MODEL_MISMATCH would,
		// and both surface through the SAME drained (never synchronous) path (D-SLM7408).
		const bool bModelMatches = bParsed && FMemory::Memcmp(Op.RestoreExpectedHash, Contents.ArtifactHash, 32) == 0
			&& FMemory::Memcmp(Op.RestoreExpectedHash, S.ArtifactHash, 32) == 0;

		// Rule 4 (T-2987 L2): the restore's adapter is pinned from this dispatch, which resolves
		// the raw sslm_adapter the worker uses, to this job's delivery.
		int64 AdapterId = 0;
		SuperSLMRuntime::FResolvedAdapter Adapter;
		bool bAdapterOk = true;
		bool bAdapterPinned = false;
		if (bParsed && bModelMatches && Contents.bHasAdapter)
		{
			AdapterId = SuperSLMRuntime::FindAdapterByHash(Contents.AdapterHash, S.Model);
			bAdapterPinned = AdapterId != 0 && SuperSLMRuntime::PinAdapter(AdapterId, Adapter);
			bAdapterOk = bAdapterPinned;
		}

		const sslm_model Model = S.Model;
		sslm_kv_pool* PoolPtr = &S.Pool;
		const sslm_seq OldSeq = Slot.Seq;
		const sslm_adapter AdapterHandle = bAdapterPinned ? Adapter.Adapter : nullptr;
		const bool bWillAttempt = bParsed && bModelMatches && bAdapterOk;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);
		// Plugin paging plan §3.9: the paged seam's one-shot status for this restore, returned in
		// place of the Layer-1 restore call (in legacy mode the release-first step before it still
		// runs, as a refused call would leave it).
		const sslm_status ForcedStatus = S.TakeLayer1Override(EPagedJobKind::Restore);

		// Plugin paging plan §3.5 (step K3), budget mode: restore-first. The job restores into a new
		// handle with sslm_seq_restore_shared (the hint's frozen handle, or null), and releases the
		// slot's old holder only once that has succeeded, so a refused or failed restore leaves the
		// old holder exactly as it was. The hint was pinned at the call and by the ledger's
		// admission, so its handle is live here. Legacy mode keeps release-first (S5).
		const bool bBudgetMode = S.IsBudgetMode();
		const int64 HintPrefixId = Op.RestoreHintPrefixId;
		const FPrefixEntry* HintEntry = HintPrefixId != 0 ? S.Prefixes.Find(HintPrefixId) : nullptr;
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.11 (b): a cache lease's entry, adopt-pinned since the call, so its handle is live here.
		const uint64 HintCacheToken = Op.RestoreHintCache.Token;
		const sslm_prefix HintHandle = HintCacheToken != 0 ? Op.RestoreHintCache.Handle : (HintEntry != nullptr ? HintEntry->Handle : nullptr);
#else
		const sslm_prefix HintHandle = HintEntry != nullptr ? HintEntry->Handle : nullptr;
#endif
		const SuperSLMPaging::FTicket LedgerTicket = Op.LedgerTicket;
		const int64 HeaderContextLength = Op.RestoreContextLength;

		// BuildExecute runs once (PostLedgerJob), so it hands its share on by move; the worker
		// closure is then the blob's last owner and releases it on the worker, on every path.
		auto BuildExecute = [Model, PoolPtr, OldSeq, Blob = MoveTemp(Blob), AdapterHandle, bWillAttempt, bOwnerHoldsAtPost, ForcedStatus, bBudgetMode, HintHandle, Out](int32 /*LaneIndex*/) mutable -> TFunction<void()>
		{
			return [Model, PoolPtr, OldSeq, Blob = MoveTemp(Blob), AdapterHandle, bWillAttempt, bOwnerHoldsAtPost, ForcedStatus, bBudgetMode, HintHandle, Out]() mutable
			{
				ON_SCOPE_EXIT { Blob.Reset(); }; // row 19: the last release of the blob runs here, on the worker
				if (!bWillAttempt)
				{
					Out->bSkipped = true;
					return;
				}
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Restore", SuperSLMChannel);
				// The payload pointer into the shared blob, parsed here on the worker; the buffer
				// lives as long as this closure holds its share.
				SuperSLMSaveBlob::FContents WorkerContents;
				const uint8* WorkerPayload = nullptr;
				int64 WorkerPayloadBytes = 0;
				FString WorkerParseError;
				if (!SuperSLMSaveBlob::Read(*Blob, WorkerContents, WorkerPayload, WorkerPayloadBytes, WorkerParseError))
				{
					Out->bSkipped = true;
					return;
				}
				if (bBudgetMode)
				{
					// Restore-first (§3.5): the new holder is made beside the old one.
					sslm_seq Restored = nullptr;
					uint32_t Shared = 0;
					Out->Status = ForcedStatus != SSLM_OK ? ForcedStatus
						: sslm_seq_restore_shared(Model, PoolPtr, WorkerPayload, static_cast<size_t>(WorkerPayloadBytes), HintHandle, &Restored, &Shared);
					if (Out->Status == SSLM_OK && AdapterHandle != nullptr)
					{
						const sslm_status BindStatus = sslm_seq_set_adapter(Restored, AdapterHandle);
						if (BindStatus != SSLM_OK)
						{
							sslm_seq_release(Restored);
							Out->Status = BindStatus;
							Out->bBoolFlag = true; // signals "adapter re-bind failed" to Deliver
						}
					}
					if (Out->Status == SSLM_OK && OldSeq != nullptr)
					{
						// Only now does the old holder go. Should its release be refused, the new one
						// goes back instead and the old one stays as it was.
						const sslm_status ReleaseStatus = sslm_seq_release(OldSeq);
						if (ReleaseStatus != SSLM_OK)
						{
							sslm_seq_release(Restored);
							Out->Status = ReleaseStatus;
						}
						else
						{
							Out->bOldHandleReleased = true;
						}
					}
					if (Out->Status == SSLM_OK)
					{
						Out->SeqOut = Restored;
						Out->SharedPages = static_cast<uint32>(Shared);
						if (bOwnerHoldsAtPost)
						{
							ReadStatsOnWorker(Model, Restored, Out->PerSequence[0]);
						}
					}
					ReadPoolStatsOnWorker(PoolPtr, *Out);
					return;
				}
				// The reserved slot's own warm sequence gives its block back first, since
				// sslm_seq_restore creates its own sequence from the pool.
				if (OldSeq != nullptr)
				{
					const sslm_status ReleaseStatus = sslm_seq_release(OldSeq);
					if (ReleaseStatus != SSLM_OK)
					{
						Out->Status = ReleaseStatus;
						return;
					}
					Out->bOldHandleReleased = true;
				}
				sslm_seq Restored = nullptr;
				Out->Status = ForcedStatus != SSLM_OK ? ForcedStatus
					: sslm_seq_restore(Model, PoolPtr, WorkerPayload, static_cast<size_t>(WorkerPayloadBytes), &Restored);
				if (Out->Status != SSLM_OK)
				{
					return;
				}
				if (AdapterHandle != nullptr)
				{
					const sslm_status BindStatus = sslm_seq_set_adapter(Restored, AdapterHandle);
					if (BindStatus != SSLM_OK)
					{
						sslm_seq_release(Restored);
						Out->Status = BindStatus;
						Out->bBoolFlag = true; // signals "adapter re-bind failed" to Deliver
						return;
					}
				}
				Out->SeqOut = Restored;
				if (bOwnerHoldsAtPost)
				{
					ReadStatsOnWorker(Model, Restored, Out->PerSequence[0]);
				}
			};
		};

		const int64 HandleId = Op.HandleId;
		const FString ParseErrorCap = ParseError;
		const SuperSLMSaveBlob::FContents ContentsCap = Contents;

		auto Deliver = [&S, SlotIndex, HandleId, Owner, Out, bParsed, bModelMatches, bAdapterOk, bAdapterPinned, ParseErrorCap, ContentsCap, AdapterId,
			bBudgetMode, HintPrefixId, LedgerTicket, HeaderContextLength
#if SUPERSLM_WITH_PREFIX_CACHE
			, HintCacheToken
#endif
			](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			// The reservation this restore made ends here -- unless its holder returned the slot
			// first and a newer restore has reserved it since (QueueRestore()'s fallback onto an
			// undrained slot). That reservation is the newer holder's and ends at its own restore's
			// delivery (D-SLM7946: ProjectGenerationTurn() reads it).
			if (Slot.VendedId == 0 || Slot.VendedId == Owner)
			{
				Slot.bAwaitingRestore = false;
			}
			--Slot.OutstandingJobCount;
			if (bAdapterPinned)
			{
				SuperSLMRuntime::UnpinAdapter(AdapterId);
			}
			// §3.5 (K3): the hint's pin from the call ends with the job; the ledger's ends at its apply.
			UnpinRestoreHint(S, HintPrefixId);
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::UnpinCacheRestoreHint(S, HintCacheToken); // precache plan §5.11 (b)
#endif
			DeliverPoolSnapshot(S, *Out);
			const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;

			auto Refuse = [&](ESuperSLMRestoreResult Result, const FString& Message)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("RestoreSequence: %s"), *Message);
				// §3.5: a refused or failed budget restore gives its whole charge back to H, and the
				// slot's old holder is untouched (restore-first).
				if (LedgerTicket != SuperSLMPaging::InvalidTicket)
				{
					S.Ledger.ApplyRestore(LedgerTicket, false, 0);
				}
				if (HandleId != 0)
				{
					S.HandleResults.FindOrAdd(HandleId).Result = Result;
				}
				// If Execute released the slot's prior warm sequence before Layer 1 refused the
				// restore, that handle is dangling: the recycle creates a fresh one instead.
				if (Out->bOldHandleReleased)
				{
					Slot.Seq = nullptr;
				}
				// H2: the reserved slot never got a real sequence, so the hold ends here -- unless
				// its holder already returned it, which ended the hold and queued the recycle.
				if (bOwnerHolds)
				{
					S.ReleaseUser(SlotIndex);
				}
			};

			if (!bParsed)
			{
				Refuse(ESuperSLMRestoreResult::Malformed, ParseErrorCap);
				return;
			}
			if (!bModelMatches)
			{
				Refuse(ESuperSLMRestoreResult::ModelMismatch, TEXT("model mismatch: the blob's artifact hash does not match the expected/configured model (SSLM_RESTORE_MODEL_MISMATCH)"));
				return;
			}
			if (!bAdapterOk)
			{
				Refuse(ESuperSLMRestoreResult::AdapterUnavailable, FString::Printf(
					TEXT("adapter unavailable: the blob was saved with adapter %s bound, and no adapter with that artifact hash is imported against the configured model"),
					*SuperSLMRuntime::HashToHex(ContentsCap.AdapterHash)));
				return;
			}
			if (Out->bSkipped)
			{
				Refuse(ESuperSLMRestoreResult::Malformed, TEXT("restore was not attempted"));
				return;
			}
			if (Out->Status != SSLM_OK)
			{
				if (Out->bBoolFlag)
				{
					Refuse(ESuperSLMRestoreResult::AdapterUnavailable, FString::Printf(TEXT("adapter unavailable: re-binding failed (%s)"), *StatusText(Out->Status)));
					return;
				}
				// §3.7 (K3): in budget mode the ledger admitted this restore's whole draw, so the
				// engine's exhaustion is KvPagesExhausted and a ledger fault. Legacy mode keeps its
				// mapping.
				if (bBudgetMode && Out->Status == SSLM_KV_POOL_EXHAUSTED)
				{
					S.NoteLedgerFault(TEXT("sslm_seq_restore_shared"));
					Refuse(ESuperSLMRestoreResult::KvPagesExhausted, TEXT("KvPagesExhausted: Layer 1 refused the restore's draw (SSLM_KV_POOL_EXHAUSTED); the old sequence is untouched"));
					return;
				}
				switch (Out->Status)
				{
					case SSLM_RESTORE_MODEL_MISMATCH: Refuse(ESuperSLMRestoreResult::ModelMismatch, TEXT("model mismatch: Layer 1 refused the payload (SSLM_RESTORE_MODEL_MISMATCH)")); return;
					case SSLM_RESTORE_KV_MISMATCH: Refuse(ESuperSLMRestoreResult::KvMismatch, TEXT("KV precision mismatch: Layer 1 refused the payload (SSLM_RESTORE_KV_MISMATCH)")); return;
					case SSLM_RESTORE_RESIDUAL_LOST: Refuse(ESuperSLMRestoreResult::ResidualLost, TEXT("residual lost: a legacy blob saved without its residual (SSLM_RESTORE_RESIDUAL_LOST)")); return;
					default: Refuse(ESuperSLMRestoreResult::Malformed, FString::Printf(TEXT("Layer 1 refused the payload (%s)"), *StatusText(Out->Status))); return;
				}
			}

			// §3.5 (K3): the apply settles the charge to the restore's own draw (R(b) when it shared,
			// R(b) + E when not, P_blk for a whole-reserve payload) less the old holder's
			// reservation, gives the slot the payload's shape, and registers a sharer on its hint.
			if (LedgerTicket != SuperSLMPaging::InvalidTicket)
			{
				S.Ledger.ApplyRestore(LedgerTicket, true, Out->SharedPages);
			}
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::NoteSequenceRestoreApplied(S, Out->SharedPages); // precache plan §8, cell 8.15
#endif

			// Slot-owned: the restored handle and the Layer-1 mirrors, whoever holds the slot.
			// Budget mode takes the position from the payload header, never the wrapper (§3.5;
			// admission refused any blob where the two differ).
			Slot.Seq = Out->SeqOut;
			++Slot.HolderGeneration; // §3.9: a new Layer-1 holder
			Slot.ContextUsed = bBudgetMode ? HeaderContextLength : ContentsCap.ContextUsed;
			Slot.LayersDoneInToken = FMath::Min(ContentsCap.LayersDoneInToken, S.Shape.NumHiddenLayers);
			Slot.bReadyForLogits = ContentsCap.bReadyForLogits;
			Slot.bAdopted = ContentsCap.Phase == ESuperSLMSequencePhase::Idle && ContentsCap.ContextUsed > 0;
			// §6 P1 "When the walk is known": the plugin did not build a restored sequence's contents,
			// so its walk is unknown whatever the blob holds (P1.11 (b)).
			Slot.bAdoptedPromptOnlyPrefix = false;
			Slot.AdoptedPrefixLength = 0;
			Slot.SpeculativeFinishesQueued = 0;

			if (bOwnerHolds)
			{
				// Rule 1 (H4): the restore's delivery starts from a value-reset per-user struct and
				// applies the blob's fields. Rule 4: it drops nothing -- the holder's held entries
				// are all later than its restore, and apply at the head of its first job after it.
				TArray<FHeldRequest> Held = MoveTemp(Slot.Held);
				static_cast<FSlotUser&>(Slot) = FSlotUser();
				Slot.Held = MoveTemp(Held);
				Slot.Phase = ContentsCap.Phase;
				Slot.SpanKind = ContentsCap.SpanKind;
				Slot.Prompt = ContentsCap.PromptRemaining;
				Slot.MaxNewTokens = ContentsCap.MaxNewTokensRemaining;
				Slot.StopTokenIds = ContentsCap.StopTokenIds;
				Slot.PinnedLayerBudget = ContentsCap.LayerBudget > 0 ? FMath::Min(ContentsCap.LayerBudget, S.Config.MaxLayerBudget) : 0;
				Slot.Generated.Reserve(Slot.MaxNewTokens);
				Slot.BoundSchemaName = ContentsCap.SchemaName;
				Slot.ActiveAdapterId = AdapterId;
				if (Slot.Phase == ESuperSLMSequencePhase::Prefilling && Slot.Prompt.Num() == 0)
				{
					Slot.Phase = ESuperSLMSequencePhase::Decoding;
				}
				if (Slot.IsGenerating() && Slot.MaxNewTokens == 0)
				{
					Slot.Phase = ESuperSLMSequencePhase::Complete;
				}
				Slot.LastOutcome = ESuperSLMDecodeOutcome::Generating;
				// A restored generation has no Generate op, so each of its prefill or decode jobs is
				// a continuation job, and the first of them carries a held adapter request (H4: it
				// applies at the head of the holder's first job after the restore).
				Slot.bGenerateFirstJobPosted = Slot.IsGenerating();
				DeliverStats(Slot, true, Out->PerSequence[0]);
			}
			if (HandleId != 0)
			{
				S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Success;
			}
		};

		S.NextPostApplyHoldId = Op.ApplyHoldId; // §3.9: HoldApply(Restore)
		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Restore, 0, 0, 0, S.Resolved.RestoreCostMs,
			FSuperSLMSequence{Slot.VendedId}, FSuperSLMLifecycleOpHandle{HandleId}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	// The test seams' request kind of a queued op (§3.9): the recycle is Return, a caller's reset
	// Reset. False for a kind no hold or override names (a save, a generation).
	bool PagedKindOf(const FSlotQueuedOp& Op, EPagedJobKind& OutKind)
	{
		switch (Op.Kind)
		{
			case ESlotOpKind::Reset:   OutKind = Op.bUnbind ? EPagedJobKind::Return : EPagedJobKind::Reset; return true;
			case ESlotOpKind::Adopt:   OutKind = EPagedJobKind::Adopt; return true;
			case ESlotOpKind::Restore: OutKind = EPagedJobKind::Restore; return true;
			case ESlotOpKind::Save:
			case ESlotOpKind::Generate:
				return false;
		}
		return false;
	}

	// Plugin paging plan §3.5 (step K3): a restore refused at the head of its queue, before any job
	// (on arrival: KvPagesExhausted; or by its payload's fields). The op leaves the queue with its
	// handle resolved by name; nothing was drawn, and the slot it reserved keeps its warm holder
	// (its reservation ends, as a refused restore's delivery ends it). The caller has settled any
	// ledger ticket the op held.
	void RefuseQueuedRestore(FSuperSLMSubsystemState& S, int32 SlotIndex, ESuperSLMRestoreResult Result, const FString& Message)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		FSlotQueuedOp Op = MoveTemp(Slot.OpQueue[0]);
		Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
		UE_LOG(LogSuperSLM, Warning, TEXT("RestoreSequence: %s"), *Message);
		UnpinRestoreHint(S, Op.RestoreHintPrefixId);
#if SUPERSLM_WITH_PREFIX_CACHE
		SuperSLMPrefixCacheGlue::UnpinCacheRestoreHint(S, Op.RestoreHintCache.Token);
#endif
		if (Op.HandleId != 0)
		{
			S.HandleResults.FindOrAdd(Op.HandleId).Result = Result;
		}
		ReleaseBlobOnWorker(S, MoveTemp(Op.RestoreBlob));
		const bool bOwnerHolds = Op.Owner != 0 && Op.Owner == Slot.VendedId;
		if (Slot.VendedId == 0 || bOwnerHolds) // a newer restore's reservation stands (DispatchRestore())
		{
			Slot.bAwaitingRestore = false;
		}
		if (bOwnerHolds)
		{
			S.ReleaseUser(SlotIndex);
		}
	}

	// Plugin paging plan §3.5 "Fit and wait" (step K3): the ledger admits a waiting restore at the
	// apply that makes it fit (or resolves it NotConfigured at an epoch end). Drained once per
	// tick, after the apply pass: an admitted waiter becomes dispatchable at its queue's head.
	void ResolveWaitingRestores(FSuperSLMSubsystemState& S)
	{
		if (!S.IsBudgetMode())
		{
			return;
		}
		for (const SuperSLMPaging::FWaitResolution& Resolution : S.Ledger.DrainResolutions())
		{
			for (int32 SlotIndex = 0; SlotIndex < S.Slots.Num(); ++SlotIndex)
			{
				FSlot& Slot = S.Slots[SlotIndex];
				if (Slot.OpQueue.IsEmpty() || !Slot.OpQueue[0].bWaiting || Slot.OpQueue[0].LedgerTicket != Resolution.Ticket)
				{
					continue;
				}
				if (Resolution.Outcome == SuperSLMPaging::EPageAdmit::Admitted)
				{
					Slot.OpQueue[0].bWaiting = false;
				}
				else
				{
					// An epoch end resolves every waiter NotConfigured; nothing was charged for it.
					Slot.OpQueue[0].LedgerTicket = SuperSLMPaging::InvalidTicket;
					RefuseQueuedRestore(S, SlotIndex, ESuperSLMRestoreResult::NotConfigured, TEXT("NotConfigured: the restore was waiting on headroom when the page ledger's epoch ended"));
				}
				break;
			}
		}
	}

	// A slot op that has been admitted and is parked at its HoldStart() hold: not a candidate
	// while the hold stands. Marks the hold reached (the op is at its hold point).
	bool IsParkedAtStartHold(FSuperSLMSubsystemState& S, const FSlotQueuedOp& Op)
	{
		if (Op.bAdmitted && S.IsHoldBlocking(Op.StartHoldId))
		{
			S.MarkHoldReached(Op.StartHoldId);
			return true;
		}
		return false;
	}

	void DispatchSlotLifecycleOp(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		// D-SLM7799: checked before any state changes. A failure posts nothing: the op is dropped, its
		// handle resolves Malformed, and the holder faults (plan §5: "its members fault"). A restore's
		// reservation ends here; its holder keeps the faulted handle and returns it as usual, which
		// queues the recycle. (Dropping the op releases a restore blob's share on the game thread;
		// this path is unreachable after Configure()'s domain check, so row 19's worker-release
		// rule is not extended to it.)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			const FSlotQueuedOp& Front = Slot.OpQueue[0];
			double OpCostMs = 0.0;
			const TCHAR* OpName = TEXT("lifecycle");
			switch (Front.Kind)
			{
				case ESlotOpKind::Reset:   OpCostMs = S.Resolved.ResetCostMs; OpName = TEXT("reset"); break;
				case ESlotOpKind::Adopt:   OpCostMs = S.Resolved.AdoptCostMs; OpName = TEXT("adopt"); break;
				case ESlotOpKind::Save:    OpCostMs = S.Resolved.SaveCostMs; OpName = TEXT("save"); break;
				case ESlotOpKind::Restore: OpCostMs = S.Resolved.RestoreCostMs; OpName = TEXT("restore"); break;
				case ESlotOpKind::Generate: return; // never reaches here; filtered by the caller
			}
			if (!TrySchedule(S, OpCostMs, TickIndex, OpName))
			{
				const int64 HandleId = Front.HandleId;
				const int64 Owner = Front.Owner;
				const bool bRestore = Front.Kind == ESlotOpKind::Restore;
				const int64 AdoptPrefixId = Front.Kind == ESlotOpKind::Adopt ? Front.PrefixId : 0;
				// Plugin paging plan §3.2: an op admitted earlier (a test hold parked it) gives its
				// admission back, since nothing will run: a reset as refused, an adopt as failed.
				if (Front.LedgerTicket != SuperSLMPaging::InvalidTicket)
				{
					if (Front.Kind == ESlotOpKind::Adopt)
					{
						S.Ledger.ApplyAdopt(Front.LedgerTicket, false);
					}
					else if (Front.Kind == ESlotOpKind::Restore)
					{
						S.Ledger.ApplyRestore(Front.LedgerTicket, false, 0);
					}
					else
					{
						S.Ledger.ApplyReset(Front.LedgerTicket, SuperSLMPaging::EResetResult::Refused);
					}
				}
				if (bRestore)
				{
					UnpinRestoreHint(S, Front.RestoreHintPrefixId);
#if SUPERSLM_WITH_PREFIX_CACHE
					SuperSLMPrefixCacheGlue::UnpinCacheRestoreHint(S, Front.RestoreHintCache.Token);
#endif
				}
				Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
				if (AdoptPrefixId != 0)
				{
					// The dropped adopt never reaches its delivery: its pin on the prefix ends here,
					// as the refusal arms below end it.
					if (FPrefixEntry* Pinned = S.Prefixes.Find(AdoptPrefixId))
					{
						Pinned->PendingAdopts = FMath::Max(0, Pinned->PendingAdopts - 1);
					}
#if SUPERSLM_WITH_PREFIX_CACHE
					SuperSLMPrefixCacheGlue::OnAdoptDelivered(S, AdoptPrefixId); // precache plan §5.6
#endif
				}
				if (HandleId != 0)
				{
					S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Malformed;
				}
				const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;
				if (bOwnerHolds)
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
				}
				if (bRestore && (Slot.VendedId == 0 || bOwnerHolds)) // a newer restore's reservation stands (DispatchRestore())
				{
					Slot.bAwaitingRestore = false;
				}
				return;
			}
		}
		// Plugin paging plan §3.2 ("admission at the head of the sequence's queue"): the op is
		// admitted once, here, when it first reaches the head with its slot's previous job
		// delivered -- never at the call that queued it, so a sequence has at most one admitted
		// job at a time (the ledger's one-job-per-slot rule, cell 3.5). The test seams' holds
		// (§3.9) are taken at the same instant; a start hold parks the admitted op at the head,
		// unposted, until it is released (PlanNextJobs skips it meanwhile).
		{
			FSlot& Slot = S.Slots[SlotIndex];
			FSlotQueuedOp& Front = Slot.OpQueue[0];
			if (!Front.bAdmitted)
			{
				Front.bAdmitted = true;
				if (S.IsBudgetMode())
				{
					if (Front.Kind == ESlotOpKind::Reset)
					{
						// A reset (the recycle included). The ledger makes it a create-first
						// normalization (§3.5) when the holder's shape differs from the configured one,
						// H has R(SequenceBudget) free and no restore waits; otherwise a reset in place.
						const SuperSLMPaging::FAdmission Admission = S.Ledger.AdmitReset(SlotIndex);
						if (Admission.Outcome == SuperSLMPaging::EPageAdmit::Admitted)
						{
							Front.LedgerTicket = Admission.Ticket;
							Front.bNormalizes = Admission.bNormalizes;
						}
						else
						{
							// K2 review F3: fail closed. A reset the ledger did not admit never runs
							// untracked: it is refused by name with nothing posted, and counted as a
							// ledger fault. A recycle's slot is withheld (its ledger state is unknown);
							// a caller's reset faults its sequence, as a reset Layer 1 refused does.
							const int64 HandleId = Front.HandleId;
							const int64 Owner = Front.Owner;
							const bool bRecycle = Front.bUnbind;
							Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
							const FString Reason = FString::Printf(TEXT("the page ledger did not admit a %s (outcome %d)"),
								bRecycle ? TEXT("recycle") : TEXT("reset"), static_cast<int32>(Admission.Outcome));
							S.NoteLedgerRefusal(SlotIndex, Reason);
							if (HandleId != 0)
							{
								S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::NotConfigured;
							}
							if (bRecycle)
							{
								S.WithholdSlot(SlotIndex, Reason);
							}
							else if (Owner != 0 && Owner == Slot.VendedId)
							{
								S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(TEXT("NotConfigured: %s; the sequence was not reset"), *Reason));
							}
							return;
						}
					}
					else if (Front.Kind == ESlotOpKind::Adopt)
					{
						// UP-14: an adopt of a built prefix draws nothing and charges nothing; it pins
						// the prefix until its apply. A prefix that faulted or was released before its
						// build applied is not admitted: the adopt's job skips (DispatchAdopt).
						const FPrefixEntry* Target = S.Prefixes.Find(Front.PrefixId);
						if (Target != nullptr && Target->bLedgerBuilt)
						{
							SuperSLMPaging::FAdmission Admission;
#if SUPERSLM_WITH_PREFIX_CACHE
							// Precache plan §5.11 (b): a cache lease is admitted against its entry (region A).
							if (!SuperSLMPrefixCacheGlue::AdmitCacheAdopt(S, Front.PrefixId, SlotIndex, Admission))
#endif
							{
								Admission = S.Ledger.AdmitAdoptPrefix(SlotIndex, Target->LedgerNode);
							}
							if (Admission.Outcome == SuperSLMPaging::EPageAdmit::Admitted)
							{
								Front.LedgerTicket = Admission.Ticket;
							}
							else if (Admission.Outcome == SuperSLMPaging::EPageAdmit::KvModeMismatch)
							{
								// §5: a whole-reserve holder adopts only after a reset normalizes it.
								// Refused by name; nothing is posted.
								const int64 HandleId = Front.HandleId;
								const int64 PrefixId = Front.PrefixId;
								Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
								if (FPrefixEntry* Pinned = S.Prefixes.Find(PrefixId))
								{
									Pinned->PendingAdopts = FMath::Max(0, Pinned->PendingAdopts - 1);
								}
#if SUPERSLM_WITH_PREFIX_CACHE
								SuperSLMPrefixCacheGlue::OnAdoptDelivered(S, PrefixId);
#endif
								if (HandleId != 0)
								{
									S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::KvModeMismatch;
								}
								UE_LOG(LogSuperSLM, Warning, TEXT("AdoptPrefix: KvModeMismatch: sequence %lld holds a whole-reserve sequence (restored from a legacy save); reset it, which normalizes it to SequenceBudget, before adopting."),
									Slot.VendedId);
								return;
							}
							else
							{
								// K2 review F3: fail closed, as the KvModeMismatch arm does. An adopt the
								// ledger did not admit would share the prefix's pages with no edge to its
								// node, so it is refused by name with nothing posted, and counted as a
								// ledger fault.
								const int64 HandleId = Front.HandleId;
								const int64 PrefixId = Front.PrefixId;
								Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
								if (FPrefixEntry* Pinned = S.Prefixes.Find(PrefixId))
								{
									Pinned->PendingAdopts = FMath::Max(0, Pinned->PendingAdopts - 1);
								}
#if SUPERSLM_WITH_PREFIX_CACHE
								SuperSLMPrefixCacheGlue::OnAdoptDelivered(S, PrefixId);
#endif
								if (HandleId != 0)
								{
									S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::NotConfigured;
								}
								S.NoteLedgerRefusal(SlotIndex, FString::Printf(TEXT("the page ledger did not admit an adopt of a built prefix (outcome %d)"),
									static_cast<int32>(Admission.Outcome)));
								return;
							}
						}
					}
					else if (Front.Kind == ESlotOpKind::Restore)
					{
						// §3.5 "Fit and wait" (K3): charged to H from the payload header read at the
						// call, assuming no sharing. Fit: admitted. Otherwise it waits when the
						// guaranteed returns ahead cover it, or is refused on arrival with nothing drawn
						// and the old holder untouched.
						SuperSLMPaging::FRestoreRequest Request;
						Request.Slot = SlotIndex;
						Request.Payload = Front.RestorePayload;
						Request.Budget = Front.RestoreBudget;
						Request.Origin = Front.RestoreOrigin;
						if (const FPrefixEntry* Hint = Front.RestoreHintPrefixId != 0 ? S.Prefixes.Find(Front.RestoreHintPrefixId) : nullptr)
						{
							Request.Hint = SuperSLMPaging::EHintKind::Prefix;
							Request.HintNode = Hint->LedgerNode;
						}
#if SUPERSLM_WITH_PREFIX_CACHE
						if (Front.RestoreHintCache.Token != 0)
						{
							// Precache plan §5.11 (b): a cache entry hint, keyed by entry, pre-charged as paging §3.2 specifies.
							Request.Hint = SuperSLMPaging::EHintKind::CacheEntry;
							Request.HintEntry = Front.RestoreHintCache.EntryKey;
							Request.HintEntryLength = Front.RestoreHintCache.Length;
						}
#endif
						const SuperSLMPaging::FAdmission Admission = S.Ledger.AdmitRestore(Request);
						ESuperSLMRestoreResult Refusal = ESuperSLMRestoreResult::Success;
						FString RefusalText;
						switch (Admission.Outcome)
						{
							case SuperSLMPaging::EPageAdmit::Admitted:
								Front.LedgerTicket = Admission.Ticket;
								break;
							case SuperSLMPaging::EPageAdmit::Waiting:
								Front.LedgerTicket = Admission.Ticket;
								Front.bWaiting = true;
								break;
							case SuperSLMPaging::EPageAdmit::KvPagesExhausted:
								Refusal = ESuperSLMRestoreResult::KvPagesExhausted;
								RefusalText = FString::Printf(TEXT("KvPagesExhausted: the restore is charged %u pages on the restore headroom, which has %u of %u free, and the restores ahead of it guarantee too little; nothing was drawn and the old sequence is untouched. Re-issue it after a restore applies, or configure more RestoreHeadroomBlocks"),
									SuperSLMPaging::RestoreAdmissionCharge(S.PagePositions, S.Ledger.GetConfig().ContextCap, Front.RestorePayload, Front.RestoreBudget, Front.RestoreOrigin),
									S.Ledger.RegionFree(SuperSLMPaging::ERegion::H), S.Ledger.RegionCapacity(SuperSLMPaging::ERegion::H));
								break;
							case SuperSLMPaging::EPageAdmit::ContextCapMismatch:
								Refusal = ESuperSLMRestoreResult::ContextCapMismatch;
								RefusalText = FString::Printf(TEXT("ContextCapMismatch: the save's budget %lld or origin %lld is above ContextCap %lld"),
									Front.RestoreBudget, Front.RestoreOrigin, static_cast<int64>(S.Ledger.GetConfig().ContextCap));
								break;
							case SuperSLMPaging::EPageAdmit::InvalidRequest: // K3 review N1: the call already checked the payload's fields
							case SuperSLMPaging::EPageAdmit::NotConfigured:
							case SuperSLMPaging::EPageAdmit::PrefixLimit:
							case SuperSLMPaging::EPageAdmit::ParentNotUsable:
							case SuperSLMPaging::EPageAdmit::KvModeMismatch:
							case SuperSLMPaging::EPageAdmit::PrefixPinned:
								// A plugin defect, never the caller's payload: logged and counted as a
								// ledger fault (K2 review F3, K3 review N1).
								Refusal = ESuperSLMRestoreResult::NotConfigured;
								RefusalText = FString::Printf(TEXT("NotConfigured: the page ledger refused the restore (outcome %d; plugin defect)"), static_cast<int32>(Admission.Outcome));
								S.NoteLedgerRefusal(SlotIndex, FString::Printf(TEXT("the page ledger did not admit a restore (outcome %d)"), static_cast<int32>(Admission.Outcome)));
								break;
						}
						if (Refusal != ESuperSLMRestoreResult::Success)
						{
							RefuseQueuedRestore(S, SlotIndex, Refusal, RefusalText);
							return;
						}
					}
				}
				EPagedJobKind HoldKind = EPagedJobKind::Reset;
				if (PagedKindOf(Front, HoldKind))
				{
					Front.StartHoldId = S.TakeArmedHold(HoldKind, /*bApply*/ false);
					Front.ApplyHoldId = S.TakeArmedHold(HoldKind, /*bApply*/ true);
				}
			}
			// §3.5 (K3): a restore waiting on H stays at its queue's head, unposted, until the ledger
			// admits it (ResolveWaitingRestores); PlanNextJobs does not offer it meanwhile.
			if (Front.bWaiting)
			{
				return;
			}
			if (S.IsHoldBlocking(Front.StartHoldId))
			{
				S.MarkHoldReached(Front.StartHoldId);
				return;
			}
		}
		switch (S.Slots[SlotIndex].OpQueue[0].Kind)
		{
			case ESlotOpKind::Reset:   DispatchReset(S, CandidateLanes, SlotIndex, TickIndex); return;
			case ESlotOpKind::Adopt:   DispatchAdopt(S, CandidateLanes, SlotIndex, TickIndex); return;
			case ESlotOpKind::Save:    DispatchSave(S, CandidateLanes, SlotIndex, TickIndex); return;
			case ESlotOpKind::Restore: DispatchRestore(S, CandidateLanes, SlotIndex, TickIndex); return;
			case ESlotOpKind::Generate: return; // never reaches here; filtered by the caller
		}
	}

	// Plugin paging plan §3.3 "Generation limit", §7: the engine refused a step past the holder's
	// limit (SSLM_KV_BUDGET_EXCEEDED). The plugin's pre-checks (ActivateGenerate, PreCheckDecodeStep)
	// stop every generation before that, so reaching here is a limit fault: logged as one by name,
	// and the sequence still stops with the outcome the pre-check would have given it.
	void FaultAtEngineLimit(FSuperSLMSubsystemState& S, FSlot& Slot, const TCHAR* Verb)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM limit fault: %s returned SSLM_KV_BUDGET_EXCEEDED for sequence %lld at %lld positions, past the plugin's own generation-limit pre-check."),
			Verb, Slot.VendedId, Slot.ContextUsed);
		S.Fault(Slot, ESuperSLMDecodeOutcome::KvBudgetExceeded, FString::Printf(
			TEXT("KvBudgetExceeded: %s refused the step at the sequence's generation limit (SSLM_KV_BUDGET_EXCEEDED)"), Verb));
	}

	// Plugin paging plan §3.3 "Generation limit": the stop a limit pre-check refused, by name.
	// Below the cap the holder's own budget ran out (KvBudgetExceeded); at the cap it is the
	// existing cap outcome (Faulted, the outcome left Generating).
	void FaultAtLimit(FSuperSLMSubsystemState& S, FSlot& Slot, SuperSLMPaging::ELimitVerdict Verdict, int64 Limit, const TCHAR* What)
	{
		if (Verdict == SuperSLMPaging::ELimitVerdict::ContextCapExceeded)
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(
				TEXT("%s would pass the model's context_cap of %lld positions (SSLM_CONTEXT_CAP_EXCEEDED)"), What, Limit));
		}
		else
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::KvBudgetExceeded, FString::Printf(
				TEXT("KvBudgetExceeded: %s would pass the sequence's generation limit of %lld positions (its SequenceBudget from its origin)"), What, Limit));
		}
	}

	// Budget mode, engine token law E7 (§3.4 "Per-step pre-check"): whether the next decode step of
	// a decoding sequence may be posted. The step embeds a token unless the sequence rests ready for
	// logits (a finish-only step); a step already part-way through a token's layers was admitted
	// when that token's embed began. EffectiveContext is the committed positions the step starts
	// from. Faults the sequence and returns false on a refusal. Always true in legacy mode.
	bool PreCheckDecodeStep(FSuperSLMSubsystemState& S, int32 SlotIndex, int64 EffectiveContext, bool bFaultOnRefusal)
	{
		if (!S.IsBudgetMode())
		{
			return true;
		}
		FSlot& Slot = S.Slots[SlotIndex];
		if (!Slot.bReadyForLogits && Slot.LayersDoneInToken > 0)
		{
			return true;
		}
		const int64 Limit = S.Ledger.SlotLimit(SlotIndex);
		const SuperSLMPaging::ELimitVerdict Verdict = SuperSLMPaging::CheckStep(EffectiveContext, Slot.bReadyForLogits, Limit, S.Shape.ContextCap);
		if (Verdict == SuperSLMPaging::ELimitVerdict::Allowed)
		{
			return true;
		}
		if (bFaultOnRefusal)
		{
			FaultAtLimit(S, Slot, Verdict, Limit, TEXT("the next decode step"));
		}
		return false;
	}

	// Budget mode (plugin paging plan §3.3): the build's ledger apply, at the delivery of the job
	// whose freeze succeeded. The node's P charge settles to the frozen prefix's own pages, and from
	// here the prefix may be adopted and its release is a ledger release. No-op in legacy mode.
	void ApplyPrefixBuilt(FSuperSLMSubsystemState& S, FPrefixEntry& Prefix)
	{
		if (Prefix.BuildTicket != SuperSLMPaging::InvalidTicket)
		{
			S.Ledger.ApplyPrefixBuild(Prefix.BuildTicket, true);
			Prefix.BuildTicket = SuperSLMPaging::InvalidTicket;
			Prefix.bLedgerBuilt = true;
		}
	}

	// Budget mode: a build that will never freeze gives its whole charge back (the engine drew
	// nothing, or has freed what it drew). No-op without an open ticket.
	void FailPrefixBuild(FSuperSLMSubsystemState& S, SuperSLMPaging::FTicket& BuildTicket)
	{
		if (BuildTicket != SuperSLMPaging::InvalidTicket)
		{
			S.Ledger.ApplyPrefixBuild(BuildTicket, false);
			BuildTicket = SuperSLMPaging::InvalidTicket;
		}
	}

	void DispatchPrefixAdmin(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 AdminIndex, int32 TickIndex)
	{
		// D-SLM7799: checked before any state changes. A failure posts nothing: the op is dropped and
		// a Begin's prefix faults (in budget mode giving its build's charge back).
		{
			const FPrefixAdminOp& Front = S.PrefixAdminQueue[AdminIndex];
			const bool bBegin = Front.Kind == EPrefixAdminKind::Begin;
			if (!TrySchedule(S, bBegin ? S.Resolved.PrefixBeginCostMs : S.Resolved.PrefixReleaseCostMs, TickIndex, bBegin ? TEXT("prefix begin") : TEXT("prefix release")))
			{
				if (bBegin)
				{
					if (FPrefixEntry* Faulting = S.Prefixes.Find(Front.PrefixId))
					{
						FailPrefixBuild(S, Faulting->BuildTicket);
						S.FaultPrefix(*Faulting, kSchedulingDomainFault);
					}
				}
				else
				{
					S.RetiringPrefixes.Remove(Front.PrefixId);
				}
				S.PrefixAdminQueue.RemoveAt(AdminIndex, EAllowShrinking::No);
				return;
			}
			// Plugin paging plan §3.9: an op parked at its HoldStart() hold is not dispatched
			// (PlanNextJobs already skips it; checked again here).
			if (S.IsHoldBlocking(Front.StartHoldId))
			{
				S.MarkHoldReached(Front.StartHoldId);
				return;
			}
		}
		FPrefixAdminOp Op = S.PrefixAdminQueue[AdminIndex];
		S.PrefixAdminQueue.RemoveAt(AdminIndex, EAllowShrinking::No);
		const bool bIsBegin = Op.Kind == EPrefixAdminKind::Begin;

		FPrefixEntry* Prefix = bIsBegin ? S.Prefixes.Find(Op.PrefixId) : nullptr;
		if (bIsBegin)
		{
			if (Prefix == nullptr)
			{
				return; // released before this Begin op could run
			}
			Prefix->bAdminJobInFlight = true;
		}
		// Release: the caller-visible FPrefixEntry is already gone by now -- ReleasePrefix()
		// removes it from State->Prefixes synchronously, at once (T-2815 round 13's own
		// pool-leak fix, below), while queuing the worker's own real sslm_prefix_release behind
		// it. Op.ReleaseHandle/Op.PinnedLane, captured at ReleasePrefix() call time while the
		// entry still existed, stand in for the entry's own Handle/PinnedLane for exactly this one
		// job -- the same ChooseLane mechanism, not a second one (T-2815 round 13 instructions).
		int32& EntityPinnedLane = bIsBegin ? Prefix->PinnedLane : Op.PinnedLane;

		const sslm_model Model = S.Model;
		sslm_kv_pool* PoolPtr = &S.Pool;
		const sslm_prefix HandleForRelease = Op.ReleaseHandle;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();

		// Plugin paging plan §3.3: in budget mode a root prefix begins with budget max(L, 1)
		// (sslm_prefix_begin_budgeted), and an empty one is frozen by its Begin job, since no prefill
		// job follows it ("Empty prefixes"). The paged seam's one-shot status (§3.9) replaces the
		// Layer-1 call.
		const bool bBudgetMode = S.IsBudgetMode();
		const int32 BeginBudget = bIsBegin ? FMath::Max(Prefix->Tokens.Num(), 1) : 0;
		const bool bFreezeAtBegin = bIsBegin && bBudgetMode && Prefix->Tokens.Num() == 0;
		// §3.3 (K3): a nested child begins from its parent's frozen handle (sslm_prefix_begin_from,
		// budget = its own token count, at least 1); the NestedBuild seams name it.
		const bool bNested = bIsBegin && Op.bNested;
		const sslm_prefix NestedParent = Op.NestedParentHandle;
		const sslm_status ForcedStatus = S.TakeLayer1Override(bIsBegin ? (bNested ? EPagedJobKind::NestedBuild : EPagedJobKind::PrefixBuild) : EPagedJobKind::PrefixRelease);

		auto BuildExecute = [Model, PoolPtr, HandleForRelease, bIsBegin, bBudgetMode, BeginBudget, bFreezeAtBegin, bNested, NestedParent, ForcedStatus, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, PoolPtr, HandleForRelease, bIsBegin, bBudgetMode, BeginBudget, bFreezeAtBegin, bNested, NestedParent, ForcedStatus, Out]()
			{
				if (bIsBegin)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixBegin", SuperSLMChannel);
					sslm_prefix NewHandle = nullptr;
					if (ForcedStatus != SSLM_OK)
					{
						Out->Status = ForcedStatus;
					}
					else if (bNested)
					{
						Out->Status = sslm_prefix_begin_from(NestedParent, BeginBudget, &NewHandle);
					}
					else
					{
						Out->Status = bBudgetMode ? sslm_prefix_begin_budgeted(Model, PoolPtr, BeginBudget, &NewHandle) : sslm_prefix_begin(Model, PoolPtr, &NewHandle);
					}
					if (Out->Status != SSLM_OK)
					{
						NewHandle = nullptr;
					}
					Out->SeqOut = reinterpret_cast<sslm_seq>(NewHandle); // opaque pointer, carried through
					if (Out->Status == SSLM_OK && bFreezeAtBegin)
					{
						Out->bFreezeAttempted = true;
						Out->FreezeStatus = sslm_prefix_freeze(NewHandle);
					}
				}
				else
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixRelease", SuperSLMChannel);
					Out->Status = ForcedStatus != SSLM_OK ? ForcedStatus : sslm_prefix_release(HandleForRelease);
				}
				if (bBudgetMode)
				{
					ReadPoolStatsOnWorker(PoolPtr, *Out);
				}
			};
		};

		const int64 PrefixIdCap = Op.PrefixId;
		const SuperSLMPaging::FTicket ReleaseTicket = Op.LedgerTicket;
		const bool bReleaseFailsBuild = Op.bReleaseFailsBuild;
		auto Deliver = [&S, PrefixIdCap, bIsBegin, bBudgetMode, bNested, ReleaseTicket, bReleaseFailsBuild, Out](double)
		{
			DeliverPoolSnapshot(S, *Out);
			if (bIsBegin)
			{
				// §3.7: the ledger admitted this build, so the engine refusing its draw is a ledger fault.
				const bool bLedgerFault = bBudgetMode && Out->Status == SSLM_KV_POOL_EXHAUSTED;
				if (bLedgerFault)
				{
					S.NoteLedgerFault(bNested ? TEXT("sslm_prefix_begin_from") : TEXT("sslm_prefix_begin_budgeted"));
				}
				FPrefixEntry* Prefix = S.Prefixes.Find(PrefixIdCap);
				if (Prefix == nullptr)
				{
					// Released while this Begin ran (1.1, RetiringPrefixes): release the block it
					// made instead of dropping the handle, or give the prefix's place back. In budget
					// mode the build never applied: the release fails it once the engine has freed
					// the pages, or, when nothing was made, it fails now.
					if (FSuperSLMSubsystemState::FRetiringPrefix* Retiring = S.RetiringPrefixes.Find(PrefixIdCap))
					{
						Retiring->bJobInFlight = false;
						const sslm_prefix Made = reinterpret_cast<sslm_prefix>(Out->SeqOut);
						if (Out->Status == SSLM_OK && Made != nullptr)
						{
							FPrefixAdminOp Release;
							Release.Kind = EPrefixAdminKind::Release;
							Release.Ordinal = S.MintOrdinal();
							Release.PrefixId = PrefixIdCap;
							Release.ReleaseHandle = Made;
							Release.PinnedLane = Retiring->PinnedLane;
							Release.LedgerTicket = Retiring->BuildTicket;
							Release.bReleaseFailsBuild = Retiring->BuildTicket != SuperSLMPaging::InvalidTicket;
							Release.StartHoldId = Retiring->ReleaseStartHoldId;
							Release.ApplyHoldId = Retiring->ReleaseApplyHoldId;
							Retiring->BuildTicket = SuperSLMPaging::InvalidTicket;
							S.PrefixAdminQueue.Add(Release);
						}
						else
						{
							FailPrefixBuild(S, Retiring->BuildTicket);
							S.RetiringPrefixes.Remove(PrefixIdCap);
						}
					}
					return;
				}
				Prefix->bAdminJobInFlight = false;
				if (Out->Status != SSLM_OK)
				{
					Prefix->Handle = nullptr;
					FailPrefixBuild(S, Prefix->BuildTicket);
					S.FaultPrefix(*Prefix, FString::Printf(TEXT("%s failed (%s)"),
						bNested ? TEXT("sslm_prefix_begin_from") : (bBudgetMode ? TEXT("sslm_prefix_begin_budgeted") : TEXT("sslm_prefix_begin")), *StatusText(Out->Status)));
				}
				else
				{
					Prefix->Handle = reinterpret_cast<sslm_prefix>(Out->SeqOut);
#if SUPERSLM_WITH_PREFIX_CACHE
					SuperSLMPrefixCacheGlue::NoteLegacyBegin(S, Prefix->Handle);
#endif
					Prefix->Phase = ESuperSLMPrefixPhase::Prefilling;
					if (Out->bFreezeAttempted)
					{
						// An empty budget-mode prefix: frozen by this job.
						if (Out->FreezeStatus == SSLM_OK)
						{
							Prefix->Phase = ESuperSLMPrefixPhase::Ready;
							ApplyPrefixBuilt(S, *Prefix);
						}
						else
						{
							S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_freeze failed (%s)"), *StatusText(Out->FreezeStatus)));
						}
					}
				}
				return;
			}
			// ReleasePrefix() already removed the caller-visible entry synchronously; this
			// delivery is fire-and-forget bookkeeping for the real, worker-side release only.
			S.RetiringPrefixes.Remove(PrefixIdCap); // 1.1: the prefix's place is free again
			if (Out->Status != SSLM_OK)
			{
				// The engine kept the prefix, so its pages stay charged: the ticket is not applied, since
				// applying it would refund pages the engine still holds. K2 review F5: the stranded
				// charge is recorded by name (an over-charge, the safe direction) until the epoch ends.
				if (ReleaseTicket != SuperSLMPaging::InvalidTicket)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("ReleasePrefix: sslm_prefix_release failed (%s); the engine kept prefix %lld, so its ledger pages on region P stay charged until the next Configure() or teardown."),
						*StatusText(Out->Status), PrefixIdCap);
				}
				else
				{
					UE_LOG(LogSuperSLM, Warning, TEXT("ReleasePrefix: sslm_prefix_release failed (%s)."), *StatusText(Out->Status));
				}
				return;
			}
			// Plugin paging plan §3.3: a built prefix's release applies its admission (its pages stay
			// charged as a ghost while a dependent maps them); an unbuilt one fails its build, which
			// returns the whole charge now that the engine has freed it.
			if (ReleaseTicket != SuperSLMPaging::InvalidTicket)
			{
				if (bReleaseFailsBuild)
				{
					S.Ledger.ApplyPrefixBuild(ReleaseTicket, false);
				}
				else
				{
					S.Ledger.ApplyPrefixRelease(ReleaseTicket);
				}
			}
		};

		// §3.9: HoldApply(PrefixRelease) holds a release's delivery; HoldApply(PrefixBuild) holds the
		// delivery of the job that freezes the prefix, which is this Begin for an empty prefix.
		if (bIsBegin)
		{
			if (bFreezeAtBegin)
			{
				S.NextPostApplyHoldId = Prefix->BuildApplyHoldId;
				Prefix->BuildApplyHoldId = 0;
			}
		}
		else
		{
			S.NextPostApplyHoldId = Op.ApplyHoldId;
		}

		// D-SLM7504 (plan §5 item 2, §10.2 item 10): PrefixBegin/PrefixRelease are their own
		// ESuperSLMWorkerJobKind values now, priced from their own static cost fields -- never the
		// zero-composition floor -- and ledger-tracked via PostLedgerJob, exactly like the four
		// existing lifecycle-op kinds. Neither belongs to a single caller-facing sequence, so
		// LifecycleOpSequence/LifecycleOpHandle are passed invalid, the same way a DecodeOrPrefill
		// job that batches several sequences already does.
		PostLedgerJob(S, EntityPinnedLane, CandidateLanes, bIsBegin ? ESuperSLMWorkerJobKind::PrefixBegin : ESuperSLMWorkerJobKind::PrefixRelease,
			0, 0, 0, bIsBegin ? S.Resolved.PrefixBeginCostMs : S.Resolved.PrefixReleaseCostMs,
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	// 1.1 worker continuation (plan §5 item 1a): the slot's gate for its active generation, made
	// by the generation's first prefill or decode job. Produced starts from the tokens already
	// delivered, which is 0 at that first job (activation and a restore both start Generated empty).
	FGenerationGatePtr GenerationGateFor(FSlot& Slot)
	{
		if (!Slot.Gate.IsValid())
		{
			Slot.Gate = MakeShared<FGenerationGate, ESPMode::ThreadSafe>();
			Slot.Gate->Produced = Slot.Generated.Num();
			Slot.Gate->MaxNewTokens = Slot.MaxNewTokens;
			Slot.Gate->StopTokenIds = Slot.StopTokenIds;
		}
		return Slot.Gate;
	}

	// ---------------------------------------------------------------------------------------------
	// Schema jump-forward (schema fast-decode plan rev 1.3.5, §6 slices P1 and P1-X)
	//
	// The policy is SuperSLMCore's (Core/SuperSLMSchemaJumpCore.h): the setting, the learned draft,
	// the observed forcedness and the span-after-decode loop. What lives here is the wiring: the walk
	// tracked from SCM1, the forcedness reads at job tails, the span job itself (one
	// sslm_seq_jump_forward call, a JobId from the ledger's counter and no ledger row), and the
	// table fed at generation end. The GPU subsystem has none of it (P1.6, P1X.4).
	// ---------------------------------------------------------------------------------------------

	// SCM1's transitions for one schema, as the draft and the learning walk them. StartState is the
	// state the walked stream starts from; IsForced is never consulted, because
	// SuperSLMCore::FObservedSchemaAutomaton supplies the observed flag.
	class FScm1Automaton final : public SuperSLMCore::ISchemaAutomaton
	{
	public:
		FScm1Automaton(const superslm::SchemaMasksTable& InTable, const superslm::SchemaEntry& InEntry, uint32 InStart)
			: Table(InTable), Entry(InEntry), Start(InStart)
		{
		}

		virtual uint32_t StartState() const override
		{
			return Start;
		}

		virtual bool Step(uint32_t State, int32_t Token, uint32_t& OutNext) const override
		{
			if (State >= Entry.state_count || Token < 0 || static_cast<uint32_t>(Token) >= Table.VocabSize())
			{
				return false;
			}
			uint32_t Next = 0;
			if (!Table.Transition(Entry, State, static_cast<uint32_t>(Token), &Next))
			{
				return false;
			}
			OutNext = Next;
			return true;
		}

		virtual bool IsForced(uint32_t State) const override
		{
			(void)State;
			return false;
		}

	private:
		const superslm::SchemaMasksTable& Table;
		const superslm::SchemaEntry& Entry;
		uint32 Start = 0;
	};

	SuperSLMCore::ESchemaJumpMode ToCoreJumpMode(ESuperSLMSchemaJumpForward Mode)
	{
		switch (Mode)
		{
			case ESuperSLMSchemaJumpForward::Exact: return SuperSLMCore::ESchemaJumpMode::Exact;
			case ESuperSLMSchemaJumpForward::Canonical: return SuperSLMCore::ESchemaJumpMode::Canonical;
			case ESuperSLMSchemaJumpForward::Off: break;
		}
		return SuperSLMCore::ESchemaJumpMode::Off;
	}

	ESuperSLMSchemaJumpForward ToPublicJumpMode(SuperSLMCore::ESchemaJumpMode Mode)
	{
		switch (Mode)
		{
			case SuperSLMCore::ESchemaJumpMode::Exact: return ESuperSLMSchemaJumpForward::Exact;
			case SuperSLMCore::ESchemaJumpMode::Canonical: return ESuperSLMSchemaJumpForward::Canonical;
			case SuperSLMCore::ESchemaJumpMode::Off: break;
		}
		return ESuperSLMSchemaJumpForward::Off;
	}

	// Advances the generation's tracked walk by one appended token (the pending token at its
	// decode's delivery, and each confirmed or forced id at its span job's). A token SCM1 does not
	// admit there means the plugin's walk no longer matches the engine's, so the walk is dropped:
	// the generation drafts and learns nothing more (timing only, C13).
	void AdvanceJumpWalk(const FSuperSLMSubsystemState& S, FSlot& Slot, int32 Token)
	{
		FJumpGeneration& J = Slot.Jump;
		if (!J.bWalkKnown)
		{
			return;
		}
		const superslm::SchemaEntry* Entry = S.WalkSchemas().ByIndex(static_cast<size_t>(J.SchemaIndex));
		uint32_t Next = 0;
		if (Entry == nullptr || !FScm1Automaton(S.WalkSchemas(), *Entry, J.StartState).Step(J.WalkState, Token, Next))
		{
			J.bWalkKnown = false;
			return;
		}
		J.WalkState = Next;
	}

	// ActivateGenerate()'s last step: the generation's jump-forward state. A schema-bound generation
	// is active: its jobs read forcedness when it can use the reads (canonical mode always, since its
	// loop needs the chain; off and exact mode when the walk is known, for the table), and its mode is
	// the request's (§6 P1-X "The setting": per generation, default Off). An unbound one runs today's
	// loop whatever the request says.
	void BeginJumpGeneration(FSuperSLMSubsystemState& S, FSlot& Slot, const FSuperSLMGenerationRequest& Request)
	{
		FJumpGeneration& J = Slot.Jump;
		J = FJumpGeneration();
		const FString SchemaName = Slot.EffectiveSchemaName();
		if (SchemaName.IsEmpty())
		{
			return;
		}
		J.bActive = true;
		J.Mode = ToCoreJumpMode(Request.SchemaJumpForward);
		if (J.Mode != SuperSLMCore::ESchemaJumpMode::Off)
		{
			// Rev 1.3.6 (§6 P1 *Lazy*): the first schema-bound generation in a jump mode asks for the
			// walk table. Until a pool thread has built it and a Tick has published it, the walk is
			// unknown (FindSchemaIndex misses on the empty table), so this generation runs without it.
			RequestSchemaWalkLoad(S);
		}
		int32 SchemaIndex = INDEX_NONE;
		if (S.FindSchemaIndex(SchemaName, SchemaIndex))
		{
			J.SchemaIndex = SchemaIndex;
			// §6 P1 "When the walk is known" (rev 1.3.6): the walk starts at state 0 when the schema
			// is bound (§6 P1-X "The walk"), and the engine leaves it there in two cases: a fresh or
			// reset sequence (no context, no adopted prefix), and a sequence over an adopted
			// prompt-only prefix with nothing after it. A restored sequence, a sequence a generation
			// already ran on, and anything else leave it unknown. A schema-content prompt walks the
			// schema as it prefills, so its tokens are walked here.
			const bool bFresh = Slot.ContextUsed == 0 && !Slot.bAdopted;
			const bool bOverPromptOnlyPrefix = Slot.bAdopted && Slot.bAdoptedPromptOnlyPrefix && Slot.ContextUsed == Slot.AdoptedPrefixLength;
			J.bWalkKnown = bFresh || bOverPromptOnlyPrefix;
			J.WalkState = 0;
			J.StartState = 0;
			if (J.bWalkKnown && Slot.SpanKind == ESuperSLMSpanKind::SchemaContent)
			{
				for (const int32 Token : Slot.Prompt)
				{
					AdvanceJumpWalk(S, Slot, Token);
				}
				J.StartState = J.WalkState;
			}
		}
		J.bReads = S.bForcednessReadsEnabled && (J.Mode == SuperSLMCore::ESchemaJumpMode::Canonical || J.bWalkKnown);
	}

	// Counts a forcedness read a job made (FSuperSLMSchemaJumpTestAccess::GetForcednessReadCount),
	// whoever holds the slot at its delivery.
	void CountForcedRead(FSuperSLMSubsystemState& S, const FSequenceJobResult& Result)
	{
		if (Result.bForcedReadAttempted)
		{
			++S.ForcednessReadCount;
		}
	}

	// A job's forcedness read, applied for its owner: the chain length the loop reads next, and the
	// observed flag of the walk state it was read at (§6 P1-X "Forcedness, observed").
	// Records one forcedness read for FSuperSLMSchemaJumpReviewTestAccess::GetForcednessReads (dev
	// only; under recording). JobId is the job the read followed.
	void NoteForcednessReadRecord(FSuperSLMSubsystemState& S, const FSlot& Slot, int64 JobId, const FSequenceJobResult& Result,
		bool bRecordedAtWalkState, uint32 WalkState)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!S.bRecordSpanJobs)
		{
			return;
		}
		FSuperSLMForcednessReadRecord& Record = S.ForcednessReadRecords.AddDefaulted_GetRef();
		Record.Sequence = FSuperSLMSequence{Slot.VendedId};
		Record.AfterJobId = JobId;
		Record.ForcedCount = Result.bForcedRead ? Result.ForcedCount : -1;
		Record.bRecordedAtWalkState = bRecordedAtWalkState;
		Record.WalkState = WalkState;
#else
		(void)S; (void)Slot; (void)JobId; (void)Result; (void)bRecordedAtWalkState; (void)WalkState;
#endif
	}

	void ApplyJumpForcedRead(FSuperSLMSubsystemState& S, FSlot& Slot, int64 JobId, const FSequenceJobResult& Result)
	{
		FJumpGeneration& J = Slot.Jump;
		if (!J.bActive || !Result.bForcedReadAttempted)
		{
			return;
		}
		J.ForcedChainLength = Result.bForcedRead ? Result.ForcedCount : 0;
		const bool bRecorded = Result.bForcedRead && J.bWalkKnown && J.SchemaIndex != INDEX_NONE;
		if (bRecorded)
		{
			S.ForcedObservations.FindOrAdd(J.SchemaIndex).RecordRead(J.WalkState, Result.ForcedCount > 0);
		}
		NoteForcednessReadRecord(S, Slot, JobId, Result, bRecorded, J.WalkState);
	}

	// A decode job's result for one member, at its delivery (the owner holds, the generation runs):
	// a produced token is pending (§5.4) and advances the walk; -1 leaves the token mid-layer.
	void NoteJumpDecodeResult(FSuperSLMSubsystemState& S, FSlot& Slot, int64 JobId, int32 Token, const FSequenceJobResult& Result)
	{
		FJumpGeneration& J = Slot.Jump;
		if (!J.bActive)
		{
			return;
		}
		J.bLastJobWasSpan = false;
		J.bLastSpanFailed = false;
		if (Token >= 0)
		{
			AdvanceJumpWalk(S, Slot, Token);
			J.bPendingToken = true;
		}
		else if (Token == -1)
		{
			J.bPendingToken = false;
		}
		ApplyJumpForcedRead(S, Slot, JobId, Result);
	}

	// What a due decoding sequence takes next under its mode, and the span job's arguments when it
	// is a span job.
	struct FSpanPlan
	{
		SuperSLMCore::EJumpNextJob Next = SuperSLMCore::EJumpNextJob::Decode;
		int32 ChunkBudget = 0;
		TArray<int32> Draft;          // exact mode: the learned draft, at least one token
		int64 SnapshotVersion = -1;   // the table snapshot the draft was read from
		// Exact mode, for the span job record (SuperSLMSchemaJumpTestSeam.h): the draft's length with the whole
		// allowance (read only while recording), and after the allowance clip, before the stop cut.
		int32 DraftLengthBeforeAllowanceClip = -1;
		int32 DraftLengthBeforeStopCut = -1;
	};

	FSpanPlan PlanJumpNextJob(const FSuperSLMSubsystemState& S, int32 SlotIndex)
	{
		FSpanPlan Plan;
		const FSlot& Slot = S.Slots[SlotIndex];
		const FJumpGeneration& J = Slot.Jump;
		if (!J.bActive || J.Mode == SuperSLMCore::ESchemaJumpMode::Off)
		{
			return Plan;
		}
		// The verb refuses a sequence mid-layer (§5.3). A held request is carried by the
		// generation's next decode job (plan §2.5 row 20 rule 4), which a span job ahead of it
		// would reorder, so it takes that decode job first.
		if (Slot.LayersDoneInToken > 0 || !Slot.Held.IsEmpty())
		{
			return Plan;
		}
		// The rows a span may admit: the context cap and, in budget mode, the holder's generation
		// limit, less the committed positions. The verb clips to the same headroom itself (§5.5,
		// §5.10.3); clipping here keeps the job's price and its draft to what it can admit.
		int64 LimitPositions = S.Shape.ContextCap;
		if (S.IsBudgetMode())
		{
			LimitPositions = FMath::Min<int64>(LimitPositions, static_cast<int64>(S.Ledger.SlotLimit(SlotIndex)));
		}
		const int64 Headroom = LimitPositions - Slot.ContextUsed;
		const int32 Budget = static_cast<int32>(FMath::Clamp<int64>(Headroom, 0, S.Config.MaxPrefillChunkBudget));
		if (Budget < 1)
		{
			return Plan;
		}

		SuperSLMCore::FJumpSequenceFacts Facts;
		Facts.Mode = J.Mode;
		Facts.bGpu = false;
		Facts.bSchemaBound = true;
		Facts.bPendingToken = J.bPendingToken;
		Facts.ForcedChainLength = J.ForcedChainLength;
		Facts.bLastJobWasSpan = J.bLastJobWasSpan;
		Facts.bLastSpanFailed = J.bLastSpanFailed;
		Facts.bSpanJobOutstanding = J.bSpanOutstanding;
		Facts.RemainingAllowance = Slot.MaxNewTokens - Slot.Generated.Num();

		std::vector<int32_t> Draft;
		if (J.Mode == SuperSLMCore::ESchemaJumpMode::Exact && J.bPendingToken && J.ForcedChainLength > 0 && !J.bLastJobWasSpan
			&& J.bWalkKnown && J.SchemaIndex != INDEX_NONE)
		{
			const superslm::SchemaEntry* Entry = S.WalkSchemas().ByIndex(static_cast<size_t>(J.SchemaIndex));
			const SuperSLMCore::FDraftTableRef Snapshot = S.DraftTables.Snapshot(S.DraftKey(J.SchemaIndex));
			const SuperSLMCore::FForcednessObservations* Observed = S.ForcedObservations.Find(J.SchemaIndex);
			if (Entry != nullptr && Snapshot && Observed != nullptr)
			{
				const FScm1Automaton Transitions(S.WalkSchemas(), *Entry, J.StartState);
				const SuperSLMCore::FObservedSchemaAutomaton Automaton(Transitions, *Observed);
				SuperSLMCore::FDraftLimits Limits;
				Limits.MaxPrefillChunkBudget = Budget;
				// One less than the allowance: a span never admits the generation's last allowed token, so a
				// MaxNewTokens cut leaves it pending, as off's decode does, and the saved state equals off's
				// (P1.11 (f): context, ready flag and current token). The last token comes from a decode.
				Limits.RemainingAllowance = Facts.RemainingAllowance - 1;
				Limits.bThreadedDecodeEffective = S.Resolved.bDecodeModeEffective; // §5.10.6, P1X.7
				Draft = SuperSLMCore::BuildDraft(Snapshot.get(), Automaton, J.WalkState, Limits);
#if WITH_DEV_AUTOMATION_TESTS
				if (S.bRecordSpanJobs)
				{
					SuperSLMCore::FDraftLimits Whole = Limits;
					Whole.RemainingAllowance = Facts.RemainingAllowance;
					Plan.DraftLengthBeforeAllowanceClip = static_cast<int32>(SuperSLMCore::BuildDraft(Snapshot.get(), Automaton, J.WalkState, Whole).size());
				}
#endif
				Plan.DraftLengthBeforeStopCut = static_cast<int32>(Draft.size());
				// A draft never carries a stop token: the generation ends at it, so admitting it (and the ids after
				// it) would leave a state off never has. Cut before the first one; the stop token then comes from a
				// decode, which leaves it pending as off's does, and the saved state equals off's.
				for (size_t K = 0; K < Draft.size(); ++K)
				{
					if (Slot.StopTokenIds.Contains(Draft[K])) { Draft.resize(K); break; }
				}
			}
		}
		Facts.DraftLength = static_cast<int32_t>(Draft.size());

		Plan.Next = SuperSLMCore::NextJob(Facts);
		if (Plan.Next != SuperSLMCore::EJumpNextJob::Span)
		{
			return Plan;
		}
		if (J.Mode == SuperSLMCore::ESchemaJumpMode::Exact)
		{
			// Priced at its full drafted M: the pending token and the draft (§6 P1-X "Pricing").
			Plan.ChunkBudget = 1 + Facts.DraftLength;
			Plan.Draft.Append(Draft.data(), static_cast<int32>(Draft.size()));
		}
		else
		{
			Plan.ChunkBudget = SuperSLMCore::CanonicalSpanChunkBudget(Facts, Budget);
			if (Plan.ChunkBudget <= 0)
			{
				Plan.Next = SuperSLMCore::EJumpNextJob::Decode;
				return Plan;
			}
		}
		Plan.SnapshotVersion = J.SchemaIndex != INDEX_NONE ? S.DraftSnapshotVersion(J.SchemaIndex) : -1;
		return Plan;
	}

	// One span job's output, written on the worker and read at delivery.
	struct FSpanJobOutput
	{
		sslm_status Status = SSLM_OK;
		TArray<int32> ForcedIds;      // out_forced: the forced (canonical) or confirmed (exact) ids
		FSequenceJobResult Result;    // stats and the forcedness read at the job's tail
	};
	using FSpanJobOutputRef = TSharedRef<FSpanJobOutput, ESPMode::ThreadSafe>;

#if WITH_DEV_AUTOMATION_TESTS
	FSuperSLMSpanJobRecord* FindSpanJobRecord(FSuperSLMSubsystemState& S, int64 JobId)
	{
		for (int32 I = S.SpanJobRecords.Num() - 1; I >= 0; --I)
		{
			if (S.SpanJobRecords[I].JobId == JobId)
			{
				return &S.SpanJobRecords[I];
			}
		}
		return nullptr;
	}
#endif

	// A span job's delivery for its owner, the generation still running: the ids reach the output
	// once, in order (listeners, P1.5), each walked and counted toward MaxNewTokens; the committed
	// rows are the engine's own count; the next job is a decode (or, in canonical mode, the rest of
	// the chain). A status other than SSLM_OK ends the span job only: the sequence goes on to its
	// decode job, which ends the generation exactly as off's would (§6 P1-X "Statuses").
	void ApplySpanResult(FSuperSLMSubsystemState& S, FSlot& Slot, int64 JobId, const FSpanJobOutput& Out)
	{
#if WITH_DEV_AUTOMATION_TESTS
		// The job hold (SetJobHold, AfterSpanDelivery): the planner runs after Apply in this tick, so marking the
		// hold here keeps the sequence's next job unplanned until the test releases it.
		if (FSuperSLMSubsystemState::FTestJobHold* Hold = S.TestJobHolds.Find(Slot.VendedId))
		{
			if (Hold->Point == ESuperSLMJobHoldPoint::AfterSpanDelivery) { Hold->bHeld = true; }
		}
#endif
		FJumpGeneration& J = Slot.Jump;
		const bool bExact = J.Mode == SuperSLMCore::ESchemaJumpMode::Exact;
		const int64 ContextBefore = Slot.ContextUsed;
		const FSequenceJobResult& Tail = Out.Result;
		const bool bTailRead = J.bActive && Tail.bForcedReadAttempted && Tail.bForcedRead;
		const int32 Count = Out.ForcedIds.Num();
		const std::vector<int32_t> Stops(Slot.StopTokenIds.GetData(), Slot.StopTokenIds.GetData() + Slot.StopTokenIds.Num());

		// Review finding 5 (rev 1.3.6, P1X.12 (f)): the walk goes through every confirmed id the engine
		// committed, each recorded drafted through (exact mode), before any stop-token check; the
		// output is cut afterwards; the span's tail read is recorded at the state after the last id,
		// or dropped when the walk cannot reach it, never at a state a stop token broke the loop at.
		size_t AppendCount = static_cast<size_t>(Count);
		bool bReadRecorded = false;
		const superslm::SchemaEntry* Entry = (J.bWalkKnown && J.SchemaIndex != INDEX_NONE)
			? S.WalkSchemas().ByIndex(static_cast<size_t>(J.SchemaIndex)) : nullptr;
		if (Entry != nullptr)
		{
			const FScm1Automaton Transitions(S.WalkSchemas(), *Entry, J.StartState);
			SuperSLMCore::FSpanWalkResult Walk;
			if (bExact)
			{
				Walk = SuperSLMCore::WalkSpanResult(Transitions, S.ForcedObservations.FindOrAdd(J.SchemaIndex), J.WalkState,
					Out.ForcedIds.GetData(), static_cast<size_t>(Count), Stops.data(), Stops.size(), bTailRead, bTailRead && Tail.ForcedCount > 0);
			}
			else
			{
				// Canonical: forced ids are not drafted, so nothing is recorded drafted through; the
				// tail read is recorded at the state after the last id, as exact's is.
				SuperSLMCore::FForcednessObservations Scratch;
				Walk = SuperSLMCore::WalkSpanResult(Transitions, Scratch, J.WalkState,
					Out.ForcedIds.GetData(), static_cast<size_t>(Count), Stops.data(), Stops.size(), false, false);
				if (Walk.bWalkKnown && bTailRead)
				{
					S.ForcedObservations.FindOrAdd(J.SchemaIndex).RecordRead(Walk.State, Tail.ForcedCount > 0);
					Walk.bReadRecorded = true;
				}
			}
			AppendCount = Walk.AppendCount;
			bReadRecorded = Walk.bReadRecorded;
			J.bWalkKnown = Walk.bWalkKnown;
			if (Walk.bWalkKnown)
			{
				J.WalkState = Walk.State;
			}
		}
		else
		{
			J.bWalkKnown = false;
			for (int32 I = 0; I < Count; ++I)
			{
				if (Slot.StopTokenIds.Contains(Out.ForcedIds[I]))
				{
					AppendCount = static_cast<size_t>(I) + 1;
					break;
				}
			}
		}

		// The ids reach the output once, in order, through the first stop token, each counted toward
		// MaxNewTokens.
		for (int32 I = 0; I < static_cast<int32>(AppendCount); ++I)
		{
			const int32 Id = Out.ForcedIds[I];
			Slot.Generated.Add(Id);
			Slot.LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
			++S.TokensFinishedTotal;
			if (Slot.Generated.Num() == 1 && Slot.BeginSeconds > 0.0)
			{
				Slot.TimeToFirstTokenMs = (FPlatformTime::Seconds() - Slot.BeginSeconds) * 1000.0;
			}
			if (Slot.StopTokenIds.Contains(Id) || Slot.Generated.Num() >= Slot.MaxNewTokens)
			{
				Slot.Phase = ESuperSLMSequencePhase::Complete;
				break;
			}
		}
		// The verb commits the pending token and the admitted ids (§5.3, §5.10.3), or nothing (a
		// no-op, a refusal): the sequence's context_length says which.
		if (Out.Result.ContextLength >= 0)
		{
			Slot.ContextUsed = Out.Result.ContextLength;
		}
		else if (Out.ForcedIds.Num() > 0)
		{
			Slot.ContextUsed += Out.ForcedIds.Num() + (J.bPendingToken ? 1 : 0);
		}
		if (Slot.ContextUsed > ContextBefore)
		{
			// After any admission ready_for_logits is set: the next decode job is a finish only.
			Slot.bReadyForLogits = true;
			Slot.LayersDoneInToken = 0;
			J.bPendingToken = false;
		}
		J.bLastJobWasSpan = true;
		J.bLastSpanFailed = Out.Status != SSLM_OK;
		// The chain the loop reads next (ApplyJumpForcedRead's, without its walk-state record: the
		// walk above already recorded the read where it belongs, or dropped it).
		if (J.bActive && Tail.bForcedReadAttempted)
		{
			J.ForcedChainLength = Tail.bForcedRead ? Tail.ForcedCount : 0;
			NoteForcednessReadRecord(S, Slot, JobId, Tail, bReadRecorded, J.WalkState);
		}
	}

	// One span job: one sslm_seq_jump_forward call (§6: "one call per job"), on the sequence's pinned
	// lane, priced with the per-C table at its full chunk (§6 "Pricing"). Its JobId is the ledger
	// counter's next; it appends no ledger row (rev 1.3.5: one counter). Planned only for a due
	// sequence, so nothing else of it is in flight, and the sequence is not offered to a decode batch
	// until it delivers (§6 P1-X: both calls take its lifecycle_mutex).
	void DispatchSpanJob(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex, const FSpanPlan& Plan)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const int64 StartDepth = Slot.ContextUsed;
		const int32 ChunkBudget = Plan.ChunkBudget;
		const double PlannedMs = PrefillChunkCostMs(ChunkBudget, StartDepth, S.Resolved);
		if (!TrySchedule(S, PlannedMs, TickIndex, TEXT("span")))
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
			return;
		}
		++Slot.OutstandingJobCount;
		Slot.Jump.bSpanOutstanding = true;

		const int64 Owner = Slot.VendedId;
		const sslm_model Model = S.Model;
		const sslm_seq SeqHandle = Slot.Seq;
		const bool bExact = Slot.Jump.Mode == SuperSLMCore::ESchemaJumpMode::Exact;
		const TArray<int32> Draft = Plan.Draft;
		// The verb takes the decode parameters decode_step takes (§5.8): greedy. Of them it acts on
		// the mode only; the layer budget is validated, so it is one decode could take.
		const int32 LayerBudget = S.Config.MaxLayerBudget;
		const bool bRead = Slot.Jump.bReads;
		int32 OverrideStatus = 0;
#if WITH_DEV_AUTOMATION_TESTS
		OverrideStatus = S.TestNextSpanStatus; // taken when the job is dispatched
		S.TestNextSpanStatus = 0;
#endif
		FSpanJobOutputRef Out = MakeShared<FSpanJobOutput, ESPMode::ThreadSafe>();
		const int64 JobId = S.NextJobId; // PostLedgerJob takes exactly this one

		auto BuildExecute = [&S, Model, SeqHandle, ChunkBudget, bExact, Draft, LayerBudget, bRead, OverrideStatus, Out](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, SeqHandle, ChunkBudget, bExact, Draft, LayerBudget, bRead, OverrideStatus, Out, Workspace]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Span", SuperSLMChannel);
				TArray<int32> Forced;
				Forced.SetNumZeroed(ChunkBudget);
				int32_t Count = 0;
				if (OverrideStatus != 0)
				{
					// FSuperSLMSchemaJumpTestAccess::SetNextSpanStatusOverride (cell P1X.10): the
					// status, count 0, and the sequence as Layer 1 had it.
					Out->Status = static_cast<sslm_status>(OverrideStatus);
				}
				else
				{
					sslm_decode_params Params;
					Out->Status = sslm_decode_params_init(Model, SSLM_DECODE_MODE_GREEDY, LayerBudget, &Params);
					if (Out->Status == SSLM_OK)
					{
						// Canonical: NULL options (rev 1.2.4's verb). Exact: the learned draft, never NULL
						// and never empty (rev 1.3.5 cold start: an empty draft sends no span job).
						sslm_jump_options Opts;
						FMemory::Memzero(Opts);
						Opts.struct_size = sizeof(sslm_jump_options);
						Opts.mode = SSLM_JUMP_EXACT;
						Opts.draft = Draft.GetData();
						Opts.draft_count = Draft.Num();
						Out->Status = sslm_seq_jump_forward(Model, SeqHandle, ChunkBudget, &Params, bExact ? &Opts : nullptr, Workspace,
							Forced.GetData(), ChunkBudget, &Count);
					}
				}
				Count = FMath::Clamp<int32>(Count, 0, ChunkBudget);
				Out->ForcedIds.Append(Forced.GetData(), Count);
				ReadStatsOnWorker(Model, SeqHandle, Out->Result); // row 22: at the job's tail
				if (bRead)
				{
					ReadForcedOnWorker(Model, SeqHandle, Out->Result);
				}
			};
		};

		auto Deliver = [&S, SlotIndex, Owner, JobId, Out](double WorkerCallMs)
		{
			(void)WorkerCallMs;
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount; // unconditional, as every job's
			CountForcedRead(S, Out->Result);
			const bool bOwnerHolds = Slot.VendedId == Owner;
#if WITH_DEV_AUTOMATION_TESTS
			if (FSuperSLMSpanJobRecord* Record = FindSpanJobRecord(S, JobId))
			{
				Record->DeliveredAtTick = S.CurrentTickIndex;
				Record->Status = static_cast<int32>(Out->Status);
				Record->ForcedIds = Out->ForcedIds;
				Record->ForcedChainLeftAfter = Out->Result.bForcedRead ? Out->Result.ForcedCount : 0;
				Record->OutputIndexBefore = bOwnerHolds ? Slot.Generated.Num() : -1;
			}
#else
			(void)JobId;
#endif
			if (!bOwnerHolds)
			{
				return; // returned (and possibly re-vended) since this job was posted
			}
			Slot.Jump.bSpanOutstanding = false;
			DeliverStats(Slot, true, Out->Result);
			if (Slot.Phase == ESuperSLMSequencePhase::Complete || Slot.Phase == ESuperSLMSequencePhase::Faulted)
			{
				return;
			}
			ApplySpanResult(S, Slot, JobId, *Out);
		};

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, ChunkBudget, 0, PlannedMs,
			FSuperSLMSequence{Owner}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver), INDEX_NONE, /*bAppendLedgerRow*/ false);
		ensure(S.NextJobId == JobId + 1);

#if WITH_DEV_AUTOMATION_TESTS
		if (S.bRecordSpanJobs)
		{
			FSuperSLMSpanJobRecord& Record = S.SpanJobRecords.AddDefaulted_GetRef();
			Record.JobId = JobId;
			Record.Sequence = FSuperSLMSequence{Owner};
			Record.Mode = ToPublicJumpMode(Slot.Jump.Mode);
			Record.PlannedAtTick = TickIndex;
			Record.ChunkBudget = ChunkBudget;
			Record.bDraftNull = !bExact;
			Record.Draft = Draft;
			Record.PlannedJobMs = PlannedMs;
			Record.PricedChunk = ChunkBudget;
			Record.StartDepth = StartDepth;
			Record.DraftSnapshotVersion = Plan.SnapshotVersion;
			Record.DraftLengthBeforeAllowanceClip = bExact ? Plan.DraftLengthBeforeAllowanceClip : -1;
			Record.DraftLengthBeforeStopCut = bExact ? Plan.DraftLengthBeforeStopCut : -1;
		}
#endif
	}

	// After Apply: each schema-bound CPU generation that has ended feeds its learned draft (§6 P1-X
	// "Which generations feed it"): an off or exact one whose walk the plugin knows, with the reads
	// on, merged on the game thread into a new snapshot swapped in by pointer. The store itself
	// refuses a canonical stream and one that did not end with the walk accepting.
	void FinishJumpGenerations(FSuperSLMSubsystemState& S)
	{
		for (FSlot& Slot : S.Slots)
		{
			FJumpGeneration& J = Slot.Jump;
			if (!J.bActive || Slot.VendedId == 0)
			{
				continue;
			}
			if (Slot.Phase != ESuperSLMSequencePhase::Complete && Slot.Phase != ESuperSLMSequencePhase::Faulted)
			{
				continue;
			}
			J.bActive = false;
			if (!J.bWalkKnown || !J.bReads || J.SchemaIndex == INDEX_NONE)
			{
				continue;
			}
			const superslm::SchemaEntry* Entry = S.WalkSchemas().ByIndex(static_cast<size_t>(J.SchemaIndex));
			if (Entry == nullptr)
			{
				continue;
			}
			const SuperSLMCore::FDraftTableKey Key = S.DraftKey(J.SchemaIndex);
			const SuperSLMCore::FDraftTableRef Before = S.DraftTables.Snapshot(Key);
			const FScm1Automaton Transitions(S.WalkSchemas(), *Entry, J.StartState);
			const SuperSLMCore::FObservedSchemaAutomaton Automaton(Transitions, S.ForcedObservations.FindOrAdd(J.SchemaIndex));
			S.DraftTables.OnGenerationFinished(Key, J.Mode, Slot.Stats.bSchemaAccepting, Automaton,
				Slot.Generated.GetData(), static_cast<size_t>(Slot.Generated.Num()));
			if (S.DraftTables.Snapshot(Key) != Before)
			{
				++S.DraftSnapshotVersions.FindOrAdd(J.SchemaIndex, -1); // one swap (P1X.8)
			}
		}
	}

	// ChainBaseTick is INDEX_NONE for a job planned the ordinary way; for a continuation it is the
	// committed tick of the job it follows (PlanContinuations, plan §5 item 1a).
	void DispatchSequencePrefill(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex,
		int32 ChainBaseTick = INDEX_NONE)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const int64 SlotVendedIdCap = Slot.VendedId;
		const bool bContinuation = ChainBaseTick != INDEX_NONE;

		const int32 Left = Slot.Prompt.Num() - Slot.PromptConsumed;
		const double ShareMs = S.Config.TickBudgetMs * S.Config.BudgetHeadroom;
		// D-SLM7794 site 3: the chunk starts at the slot's ContextUsed, then one position per token.
		const int64 StartDepth = Slot.ContextUsed;
		const int32 Chunk = PlanPrefillChunk(FMath::Min(S.Config.MaxPrefillChunkBudget, Left), StartDepth, ShareMs, S.Config.MinPrefillChunk, S.Resolved);
		const double PlannedMs = PrefillChunkCostMs(Chunk, StartDepth, S.Resolved);
		// D-SLM7799: checked before any state changes, from the tick the job commits from.
		if (!TrySchedule(S, PlannedMs, bContinuation ? FMath::Max(TickIndex, ChainBaseTick) : TickIndex, TEXT("sequence prefill")))
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
			return;
		}
		++Slot.OutstandingJobCount;

		const sslm_model Model = S.Model;
		const sslm_seq SeqHandle = Slot.Seq;
		const FGenerationGatePtr Gate = GenerationGateFor(Slot);
		FContinuationTicketPtr Ticket;
		if (bContinuation)
		{
			Ticket = MakeShared<FContinuationTicket, ESPMode::ThreadSafe>();
		}
		// Copied by value, never a raw pointer into Slot.Prompt's own buffer: a job composed for
		// the NEXT chunk can now be posted to this same lane before this call's Deliver() has run
		// (the schedule-fixed redesign, §14.8), so nothing may assume it is the only reader.
		TArray<int32> PrefillTokensCopy;
		PrefillTokensCopy.Append(Slot.Prompt.GetData() + Slot.PromptConsumed, Left);
		const sslm_span_kind SpanKind = static_cast<sslm_span_kind>(Slot.SpanKind);
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);
		// Schema fast-decode plan rev 1.3.5, §6 P1-X: the forcedness read before the first decode
		// job is made at the tail of the prefill job that consumes the prompt's last token.
		const bool bReadForced = Slot.Jump.bActive && Slot.Jump.bReads && Slot.PromptConsumed + Chunk >= Slot.Prompt.Num();

		// Plan §2.5 row 20 rule 4: a Generate's first job carries every held entry below its op's
		// ordinal; each later job of the generation carries adapter entries only.
		const bool bFirstJob = !Slot.bGenerateFirstJobPosted;
		const TArray<FCarriedRequest> Carried = CarryHeldRequests(Slot,
			bFirstJob ? Slot.ActiveGenerateOrdinal : TNumericLimits<int64>::Max(), /*bAdaptersOnly*/ !bFirstJob);
		Slot.bGenerateFirstJobPosted = true;
		// PlanContinuations admits only a holder with an empty held list, so a continuation carries
		// nothing, and a skipped one leaves no carried entry unapplied.
		ensure(!bContinuation || Carried.IsEmpty());

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob, not chosen here -- the
		// workspace this call needs is therefore captured through BuildExecute's own resolved-lane
		// parameter, never read from a LaneIndex this function no longer receives.
		auto BuildExecute = [&S, Model, SeqHandle, PrefillTokensCopy, Left, Chunk, SpanKind, Carried, Out, Gate, Ticket, bContinuation, bReadForced](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, SeqHandle, PrefillTokensCopy, Left, Chunk, SpanKind, Workspace, Carried, Out, Gate, Ticket, bContinuation, bReadForced]()
			{
				// 1.1 (plan §5 item 1a): a continuation whose generation ended in the job ahead of
				// it (or was ended from the game thread), or which a later request cancelled, makes
				// no Layer-1 call.
				if (bContinuation && (Gate->IsStopped() || Ticket->bCancelled.load(std::memory_order_acquire)))
				{
					Out->bSkipped = true;
					return;
				}
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Prefill", SuperSLMChannel);
				FSequenceJobResult& Result = Out->PerSequence[0];
				ApplyCarriedOnWorker(Model, SeqHandle, Carried, Result.CarriedStatuses); // rule 4: at the job's head
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefillChunk", SuperSLMChannel);
					int32 Consumed = 0;
					Out->Status = sslm_prefill(Model, SeqHandle, PrefillTokensCopy.GetData(), Left, Chunk, SpanKind, Workspace, &Consumed);
					Out->Consumed = Consumed;
				}
				if (Out->Status != SSLM_OK)
				{
					Gate->Stop(); // Deliver() faults the sequence for this, so nothing may follow it
				}
				ReadStatsOnWorker(Model, SeqHandle, Result); // row 22: at the job's tail
				if (bReadForced && Out->Status == SSLM_OK)
				{
					ReadForcedOnWorker(Model, SeqHandle, Result); // §6 P1-X: before the first decode job
				}
			};
		};

		// 1.1: the mirrors this job advances at post, restored if it is skipped (Deliver, below).
		const int32 PrePromptConsumed = Slot.PromptConsumed;
		const int64 PreContextUsed = Slot.ContextUsed;
		const int32 PreLayersDoneInToken = Slot.LayersDoneInToken;
		const bool bPreReadyForLogits = Slot.bReadyForLogits;
		const ESuperSLMSequencePhase PrePhase = Slot.Phase;
		const int64 LedgerRowCap = S.JobLedger.AppendedCount(); // the row PostLedgerJob appends

		// Speculative, structural, POST-time advancement (§14.8 item 2): Chunk is the size of the
		// request this job commits to, and the static cost model already prices PlannedMs as
		// Chunk*PromptTokenCostMs -- the schedule already trusts "this job consumes exactly Chunk
		// tokens on success." Composing the NEXT prefill/decode job for this slot (which a later
		// tick may now do before THIS job's real Deliver() runs) needs PromptConsumed/ContextUsed/
		// Phase/bReadyForLogits to already reflect that outcome, so it is applied here rather than
		// in Deliver(). Deliver() below applies only the two things that are genuinely
		// content-dependent: a real failure, and the per-token timing stat.
		Slot.PromptConsumed += Chunk;
		Slot.ContextUsed += Chunk;
		if (Slot.PromptConsumed >= Slot.Prompt.Num())
		{
			Slot.Phase = ESuperSLMSequencePhase::Decoding;
			Slot.bReadyForLogits = true; // the prompt's last token already ran every layer
			Slot.LayersDoneInToken = 0;
		}

		const int64 JobIdCap = S.NextJobId; // PostLedgerJob takes exactly this one (a forcedness read's AfterJobId)
		auto Deliver = [&S, SlotIndex, SlotVendedIdCap, Carried, Out, PrePromptConsumed, PreContextUsed, PreLayersDoneInToken, bPreReadyForLogits, PrePhase, LedgerRowCap, Ticket, JobIdCap](double WorkerCallMs)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			if (Ticket.IsValid() && Slot.QueuedContinuation == Ticket)
			{
				Slot.QueuedContinuation.Reset();
			}
			// Decrement unconditionally -- this job's real work is done either way, and a slot
			// returned mid-flight waits on this count reaching zero regardless of whose content,
			// if anyone's, the job's real result gets applied to.
			--Slot.OutstandingJobCount;
			CountForcedRead(S, Out->PerSequence[0]);
			// Rule 2: caller-owned state only for the job's owner (the holder at post).
			const bool bOwnerHolds = Slot.VendedId == SlotVendedIdCap;
			ConfirmCarried(S, Slot, bOwnerHolds, Carried, Out->PerSequence[0].CarriedStatuses, /*bCarrierInGeneration*/ true);
			if (Out->bSkipped)
			{
				// 1.1 (plan §5 item 1a): a skipped continuation ran nothing. It is the slot's last
				// posted job (nothing is admitted behind it: its generation has ended, or a queued
				// request blocks further continuations and ordinary admission waits for it), so the
				// mirrors it advanced at post go back to what Layer 1 actually holds, and a phase it
				// advanced speculatively goes back too unless the sequence has since ended.
				if (FSuperSLMWorkerJobReport* Row = S.JobLedger.Find(LedgerRowCap))
				{
					Row->bSkippedContinuation = true;
					Row->SkippedMembers = 1;
				}
				if (bOwnerHolds)
				{
					Slot.PromptConsumed = PrePromptConsumed;
					Slot.ContextUsed = PreContextUsed;
					Slot.LayersDoneInToken = PreLayersDoneInToken;
					Slot.bReadyForLogits = bPreReadyForLogits;
					if (Slot.IsGenerating())
					{
						Slot.Phase = PrePhase;
					}
				}
				return;
			}
			if (!bOwnerHolds)
			{
				return; // returned (and possibly re-vended) since this job was posted
			}
			DeliverStats(Slot, true, Out->PerSequence[0]);
			if (Out->Consumed > 0)
			{
				S.LastPrefillMsPerToken = WorkerCallMs / Out->Consumed;
			}
			if (Slot.Phase == ESuperSLMSequencePhase::Complete || Slot.Phase == ESuperSLMSequencePhase::Faulted)
			{
				return; // an earlier-committed, later-delivered job already made this slot terminal
			}
			if (Out->Status != SSLM_OK)
			{
				if (Out->Status == SSLM_KV_BUDGET_EXCEEDED)
				{
					FaultAtEngineLimit(S, Slot, TEXT("sslm_prefill"));
					return;
				}
				S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(
					TEXT("sslm_prefill failed after %d of %d prompt tokens (%s)"), Slot.PromptConsumed, Slot.Prompt.Num(), *StatusText(Out->Status)));
				return;
			}
			ApplyJumpForcedRead(S, Slot, JobIdCap, Out->PerSequence[0]); // §6 P1-X: the chain at the prompt's end
		};

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, Chunk, 0, PlannedMs,
			FSuperSLMSequence{SlotVendedIdCap}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver), ChainBaseTick);
		S.JobLedger.Last()->PromptPositionSum = PromptChunkPositionSum(Chunk, StartDepth); // D-SLM7794 (T-3002 S1)
		Slot.ContinuableJobId = S.JobLedger.Last()->JobId;
		if (bContinuation)
		{
			Slot.QueuedContinuation = Ticket;
		}
	}

	void DispatchPrefixPrefill(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int64 PrefixId, int32 TickIndex)
	{
		FPrefixEntry* PrefixPtr = S.Prefixes.Find(PrefixId);
		if (PrefixPtr == nullptr)
		{
			return;
		}
		const int32 Left = PrefixPtr->Tokens.Num() - PrefixPtr->Consumed;
		const double ShareMs = S.Config.TickBudgetMs * S.Config.BudgetHeadroom;
		// D-SLM7794 site 4: a prefix has no slot, so the chunk starts at the prefix's own Consumed,
		// after its parent's positions for a nested child (plugin paging plan §3.3, K3).
		const int64 StartDepth = PrefixPtr->OriginLength + PrefixPtr->Consumed;
		const int32 Chunk = PlanPrefillChunk(FMath::Min(S.Config.MaxPrefillChunkBudget, Left), StartDepth, ShareMs, S.Config.MinPrefillChunk, S.Resolved);
		const double PlannedMs = PrefillChunkCostMs(Chunk, StartDepth, S.Resolved);
		// D-SLM7799: checked before any state changes. A prefix has no member sequence; it faults.
		if (!TrySchedule(S, PlannedMs, TickIndex, TEXT("prefix prefill")))
		{
			S.FaultPrefix(*PrefixPtr, kSchedulingDomainFault);
			return;
		}
		PrefixPtr->bAdminJobInFlight = true; // reuses the same "don't touch again while in flight" guard

		const sslm_model Model = S.Model;
		const sslm_prefix PrefixHandle = PrefixPtr->Handle;
		// Copied by value -- see DispatchSequencePrefill's own note on why a raw pointer into a
		// TArray is never captured across the worker-thread boundary.
		TArray<int32> PrefillTokensCopy;
		PrefillTokensCopy.Append(PrefixPtr->Tokens.GetData() + PrefixPtr->Consumed, Left);
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		// Plugin paging plan §3.2: in budget mode the job's tail reads the pool's page counts.
		const bool bBudgetMode = S.IsBudgetMode();
		const sslm_kv_pool* PoolPtr = &S.Pool;

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob; the workspace it
		// requires is read from BuildExecute's own resolved-lane parameter.
		auto BuildExecute = [&S, Model, PrefixHandle, PrefillTokensCopy, Left, Chunk, bBudgetMode, PoolPtr, Out](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, PrefixHandle, PrefillTokensCopy, Left, Chunk, Workspace, bBudgetMode, PoolPtr, Out]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixPrefill", SuperSLMChannel);
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefillChunk", SuperSLMChannel);
					int32 Consumed = 0;
					Out->Status = sslm_prefix_prefill(Model, PrefixHandle, PrefillTokensCopy.GetData(), Left, Chunk, SSLM_SPAN_PROMPT, Workspace, &Consumed);
					Out->Consumed = Consumed;
				}
				// Plan §2.5 row 22: the freeze runs on the worker, at the tail of the prefix's last
				// prefill job, so no delivery calls into Layer 1. Left is every token still to
				// prefill, so this job is the last when it consumed all of them.
				if (Out->Status == SSLM_OK && Out->Consumed >= Left)
				{
					Out->bFreezeAttempted = true;
					Out->FreezeStatus = sslm_prefix_freeze(PrefixHandle);
				}
				if (bBudgetMode)
				{
					ReadPoolStatsOnWorker(PoolPtr, *Out);
				}
			};
		};

		auto Deliver = [&S, PrefixId, bBudgetMode, Out](double WorkerCallMs)
		{
			DeliverPoolSnapshot(S, *Out);
			FPrefixEntry* Prefix = S.Prefixes.Find(PrefixId);
			if (Prefix == nullptr)
			{
				// Released while this job ran (1.1, RetiringPrefixes): its queued release may go now.
				if (FSuperSLMSubsystemState::FRetiringPrefix* Retiring = S.RetiringPrefixes.Find(PrefixId))
				{
					Retiring->bJobInFlight = false;
				}
				return;
			}
			Prefix->bAdminJobInFlight = false;
			Prefix->Consumed += Out->Consumed;
			if (Out->Consumed > 0)
			{
				S.LastPrefillMsPerToken = WorkerCallMs / Out->Consumed;
			}
			if (Out->Status != SSLM_OK)
			{
				// Budget mode: the build's admission covered every page this prefill draws, so an
				// exhausted pool is a ledger fault (§3.7); the faulted prefix keeps its build open
				// until its release frees the pages and fails it (ReleasePrefix()).
				if (bBudgetMode && Out->Status == SSLM_KV_POOL_EXHAUSTED)
				{
					S.NoteLedgerFault(TEXT("sslm_prefix_prefill"));
				}
				S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_prefill failed after %d of %d tokens (%s)"),
					Prefix->Consumed, Prefix->Tokens.Num(), *StatusText(Out->Status)));
				return;
			}
			if (Out->bFreezeAttempted)
			{
				if (Out->FreezeStatus == SSLM_OK)
				{
					Prefix->Phase = ESuperSLMPrefixPhase::Ready;
					ApplyPrefixBuilt(S, *Prefix); // plugin paging plan §3.3: the build applies here
				}
				else
				{
					S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_freeze failed (%s)"), *StatusText(Out->FreezeStatus)));
				}
			}
		};

		// §3.9: HoldApply(PrefixBuild) holds the delivery of the job that freezes the prefix: the
		// one that prefills its last tokens.
		if (Chunk >= Left)
		{
			S.NextPostApplyHoldId = PrefixPtr->BuildApplyHoldId;
			PrefixPtr->BuildApplyHoldId = 0;
		}
		PostLedgerJob(S, PrefixPtr->PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, Chunk, 0, PlannedMs,
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
		S.JobLedger.Last()->PromptPositionSum = PromptChunkPositionSum(Chunk, StartDepth); // D-SLM7794 (T-3002 S1)
	}

	// D-SLM7414: ALL due decoding sequences sharing one layer_budget batch into ONE Layer-1 call
	// (sslm_decode_step_v2's own documented shape), so R-S1b's 8-sequence shape plans as one job
	// on one lane. PinnedBudget == 0 means scheduler-chosen (sized from the static cost model,
	// the largest composition whose planned cost fits the share); a nonzero PinnedBudget always
	// runs as pinned, unscanned.
	// ChainBaseTick: as DispatchSequencePrefill's (INDEX_NONE unless this is a continuation).
	void DispatchDecodeBatch(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, const TArray<int32>& SlotIndices, int32 PinnedBudget, int32 TickIndex,
		int32 ChainBaseTick = INDEX_NONE)
	{
		const bool bContinuation = ChainBaseTick != INDEX_NONE;
		const int32 NumHiddenLayers = S.Shape.NumHiddenLayers;
		FDecodeComposition Composition;
		if (PinnedBudget > 0)
		{
			Composition.LayerBudget = PinnedBudget;
			for (int32 Idx : SlotIndices)
			{
				const FSlot& S2 = S.Slots[Idx];
				const int32 Left = S2.bReadyForLogits ? 0 : NumHiddenLayers - S2.LayersDoneInToken;
				const int32 LayersThis = FMath::Min(PinnedBudget, Left);
				Composition.TotalLayers += LayersThis;
				Composition.DecodeLayerDepthSum += static_cast<int64>(LayersThis) * S2.ContextUsed; // D-SLM7794 site 2
				if (PinnedBudget >= Left)
				{
					++Composition.TotalFinishes;
				}
			}
			Composition.PlannedMs = DecodeCompositionCostMs(Composition, S.Resolved.LayerCostMs, S.Resolved.LayerCostPerPositionMs, S.Resolved.FinishCostMs);
		}
		else
		{
			const double ShareMs = S.Config.TickBudgetMs * S.Config.BudgetHeadroom;
			Composition = PlanDecodeBatch(SlotIndices, S.Slots, NumHiddenLayers, S.Config.MaxLayerBudget, ShareMs,
				S.Resolved.LayerCostMs, S.Resolved.LayerCostPerPositionMs, S.Resolved.FinishCostMs);
		}
		// D-SLM7799: the checked K and tick, before any state changes; a failure posts nothing and
		// faults every member. A continuation is checked from the tick it commits from.
		if (!TrySchedule(S, Composition.PlannedMs, bContinuation ? FMath::Max(TickIndex, ChainBaseTick) : TickIndex, TEXT("decode")))
		{
			for (int32 Idx : SlotIndices)
			{
				S.Fault(S.Slots[Idx], ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
			}
			return;
		}
#if WITH_DEV_AUTOMATION_TESTS
		// FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain (review finding 4):
		// ground truth for the drain rule. Under recording, before this decode job is posted, the
		// game thread reads each Canonical member's chain itself (cap 0, read-only, §5.3). A due
		// member has no job in flight (a jump-mode generation takes no continuation), so the read
		// sees the state the job will start from. Not one of GetForcednessReads()'s.
		if (S.bRecordSpanJobs)
		{
			for (int32 Idx : SlotIndices)
			{
				const FSlot& Member = S.Slots[Idx];
				if (!Member.Jump.bActive || Member.Jump.Mode != SuperSLMCore::ESchemaJumpMode::Canonical || Member.Seq == nullptr)
				{
					continue;
				}
				int32_t Chain = 0;
				int32_t Accepting = 0;
				if (sslm_seq_forced_tokens(S.Model, Member.Seq, nullptr, 0, &Chain, &Accepting) == SSLM_OK && Chain > 0)
				{
					++S.CanonicalOpenChainDecodes;
				}
			}
		}
#endif
		for (int32 Idx : SlotIndices)
		{
			++S.Slots[Idx].OutstandingJobCount;
		}

		const sslm_model Model = S.Model;
		const int32 LayerBudget = Composition.LayerBudget;
		TArray<sslm_seq> Seqs;
		Seqs.Reserve(SlotIndices.Num());
		for (int32 Idx : SlotIndices)
		{
			Seqs.Add(S.Slots[Idx].Seq);
		}
		const TArray<int32> SlotIndicesCopy = SlotIndices;
		TArray<int64> SlotVendedIdsCopy;
		SlotVendedIdsCopy.Reserve(SlotIndices.Num());
		const bool bAllFinishOnly = Composition.TotalLayers == 0 && Composition.TotalFinishes > 0;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(SlotIndices.Num());
		Out->MemberSkipped.SetNumZeroed(SlotIndices.Num());
		TArray<FGenerationGatePtr> Gates;
		TArray<FContinuationTicketPtr> Tickets; // 1.1: one per member of a continuation, else empty
		// Schema fast-decode plan rev 1.3.5, §6 P1-X: the members whose generations read forcedness
		// after every decode job (cap 0, at the job's tail).
		TArray<bool> ReadForcedPerSlot;
		Gates.Reserve(SlotIndices.Num());
		ReadForcedPerSlot.Reserve(SlotIndices.Num());
		for (int32 Idx : SlotIndices)
		{
			ReadForcedPerSlot.Add(S.Slots[Idx].Jump.bActive && S.Slots[Idx].Jump.bReads);
			Gates.Add(GenerationGateFor(S.Slots[Idx]));
			if (bContinuation)
			{
				Tickets.Add(MakeShared<FContinuationTicket, ESPMode::ThreadSafe>());
			}
		}

		// Plan §2.5 row 20 rule 4, per member: a Generate's first job carries every held entry
		// below its op's ordinal; each later job carries adapter entries only (the mid-generation
		// swap, applied at a token boundary -- Layer 1 refuses it mid-token and the entry goes
		// back to the list for the next job).
		TArray<TArray<FCarriedRequest>> CarriedPerSlot;
		CarriedPerSlot.Reserve(SlotIndices.Num());
		for (int32 Idx : SlotIndices)
		{
			FSlot& Member = S.Slots[Idx];
			const bool bFirstJob = !Member.bGenerateFirstJobPosted;
			CarriedPerSlot.Add(CarryHeldRequests(Member, bFirstJob ? Member.ActiveGenerateOrdinal : TNumericLimits<int64>::Max(), /*bAdaptersOnly*/ !bFirstJob));
			Member.bGenerateFirstJobPosted = true;
			ensure(!bContinuation || CarriedPerSlot.Last().IsEmpty()); // see DispatchSequencePrefill
		}

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob; the workspace it
		// requires is read from BuildExecute's own resolved-lane parameter.
		auto BuildExecute = [&S, Model, Seqs, LayerBudget, bAllFinishOnly, CarriedPerSlot, Out, Gates, Tickets, bContinuation, ReadForcedPerSlot](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, Seqs, LayerBudget, Workspace, bAllFinishOnly, CarriedPerSlot, Out, Gates, Tickets, bContinuation, ReadForcedPerSlot]()
			{
				// 1.1 (plan §5 item 1a): a continuation leaves out every member whose generation
				// ended in the job ahead of it (or was ended from the game thread), and every member
				// a later request cancelled; with none left it makes no Layer-1 call at all. Layer 1's results do not depend on which sequences
				// share a call, so the members that stay produce the tokens they would have anyway.
				TArray<int32, TInlineAllocator<16>> Live;
				for (int32 I = 0; I < Seqs.Num(); ++I)
				{
					if (bContinuation && (Gates[I]->IsStopped() || Tickets[I]->bCancelled.load(std::memory_order_acquire)))
					{
						Out->MemberSkipped[I] = true;
					}
					else
					{
						Live.Add(I);
					}
				}
				if (Live.Num() == 0)
				{
					Out->bSkipped = true;
					return;
				}
				for (int32 I : Live)
				{
					ApplyCarriedOnWorker(Model, Seqs[I], CarriedPerSlot[I], Out->PerSequence[I].CarriedStatuses); // rule 4: at the job's head
				}
				// T-2883: TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL's own doc note ("the name
				// should be guaranteed to not change between calls") rules it out here -- this call
				// site's own static per-call-site spec id would cache whichever branch ran FIRST and
				// misreport every later call on the OTHER branch. TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_
				// ON_CHANNEL is the dynamic-string variant (FDynamicEventScope, no per-call-site
				// caching) built for exactly a name that legitimately varies call to call.
				TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(bAllFinishOnly ? "SuperSLM.Finish" : "SuperSLM.DecodeLayers", SuperSLMChannel);
				sslm_decode_params Params;
				Out->Status = sslm_decode_params_init(Model, SSLM_DECODE_MODE_GREEDY, LayerBudget, &Params);
				if (Out->Status == SSLM_OK)
				{
					Out->DecodeTokens.SetNumZeroed(Seqs.Num(), EAllowShrinking::No);
					if (Live.Num() == Seqs.Num())
					{
						Out->Status = sslm_decode_step_v2(Model, const_cast<sslm_seq*>(Seqs.GetData()), Seqs.Num(), &Params, Workspace, Out->DecodeTokens.GetData());
					}
					else
					{
						TArray<sslm_seq, TInlineAllocator<16>> LiveSeqs;
						TArray<int32, TInlineAllocator<16>> LiveTokens;
						for (int32 I : Live)
						{
							LiveSeqs.Add(Seqs[I]);
						}
						LiveTokens.SetNumZeroed(Live.Num());
						Out->Status = sslm_decode_step_v2(Model, LiveSeqs.GetData(), LiveSeqs.Num(), &Params, Workspace, LiveTokens.GetData());
						for (int32 J = 0; J < Live.Num(); ++J)
						{
							Out->DecodeTokens[Live[J]] = LiveTokens[J];
						}
					}
				}
				// 1.1: every member that ran updates its gate, by Deliver()'s own terminal rule.
				for (int32 I : Live)
				{
					if (Out->Status != SSLM_OK)
					{
						Gates[I]->Stop();
					}
					else
					{
						Gates[I]->NoteDecodeToken(Out->DecodeTokens[I]);
					}
				}
				for (int32 I : Live)
				{
					ReadStatsOnWorker(Model, Seqs[I], Out->PerSequence[I]); // row 22: at the job's tail
					if (ReadForcedPerSlot[I] && Out->Status == SSLM_OK)
					{
						ReadForcedOnWorker(Model, Seqs[I], Out->PerSequence[I]); // §6 P1-X: after every decode job
					}
				}
			};
		};

		// Speculative, structural, POST-time advancement (§14.8 item 2, mirrors
		// DispatchSequencePrefill's own note above): per slot, Left/Layers is the exact formula
		// Deliver used to compute for itself, now moved here so a LATER tick can compose this
		// slot's NEXT decode job -- reading LayersDoneInToken/bReadyForLogits -- before THIS job's
		// real Deliver() has run. "Finishing" (Layers >= Left) is the structural fact that Layer 1
		// will report something other than -1 for this slot; which of {a real token, -2, -3} it
		// reports is content-dependent and stays in Deliver. bWasReadyForLogits is captured per
		// slot because Deliver still needs the PRE-advancement value for its own ContextUsed rule,
		// and by delivery time this job's own speculative write has already overwritten it.
		TArray<bool> WasReadyForLogitsCopy;
		TArray<bool> WasFinishingCopy; // §17: which slots THIS job speculatively incremented
		                                 // SpeculativeFinishesQueued for -- Deliver decrements
		                                 // exactly those, unconditionally, mirroring
		                                 // OutstandingJobCount's own unconditional-decrement rule.
		TArray<int32> PreLayersDoneCopy; // 1.1: restored with WasReadyForLogitsCopy if a member is skipped
		WasReadyForLogitsCopy.Reserve(SlotIndices.Num());
		WasFinishingCopy.Reserve(SlotIndices.Num());
		PreLayersDoneCopy.Reserve(SlotIndices.Num());
		for (int32 Idx : SlotIndices)
		{
			FSlot& Slot2 = S.Slots[Idx];
			SlotVendedIdsCopy.Add(Slot2.VendedId);
			WasReadyForLogitsCopy.Add(Slot2.bReadyForLogits);
			PreLayersDoneCopy.Add(Slot2.LayersDoneInToken);
			const int32 Left2 = Slot2.bReadyForLogits ? 0 : NumHiddenLayers - Slot2.LayersDoneInToken;
			const int32 Layers2 = FMath::Min(LayerBudget, Left2);
			const bool bFinishing = Layers2 >= Left2;
			WasFinishingCopy.Add(bFinishing);
			if (bFinishing)
			{
				Slot2.LayersDoneInToken = 0;
				Slot2.bReadyForLogits = false;
				++Slot2.SpeculativeFinishesQueued;
			}
			else
			{
				Slot2.LayersDoneInToken = FMath::Min(NumHiddenLayers, Slot2.LayersDoneInToken + Layers2);
			}
		}

		const int64 LedgerRowCap = S.JobLedger.AppendedCount(); // the row PostLedgerJob appends
		const int64 JobIdCap = S.NextJobId; // PostLedgerJob takes exactly this one (a forcedness read's AfterJobId)
		auto Deliver = [&S, SlotIndicesCopy, SlotVendedIdsCopy, WasReadyForLogitsCopy, WasFinishingCopy, PreLayersDoneCopy, CarriedPerSlot, Out, bAllFinishOnly, LedgerRowCap, Tickets, JobIdCap](double WorkerCallMs) // LayerBudget/NumHiddenLayers no longer needed here: Left/Layers/the finishing decision are already resolved above, at POST time
		{
			for (int32 I = 0; I < Tickets.Num(); ++I)
			{
				FSlot& Member = S.Slots[SlotIndicesCopy[I]];
				if (Member.QueuedContinuation == Tickets[I])
				{
					Member.QueuedContinuation.Reset();
				}
			}
			if (Out->bSkipped)
			{
				if (FSuperSLMWorkerJobReport* Row = S.JobLedger.Find(LedgerRowCap))
				{
					Row->bSkippedContinuation = true;
				}
			}
			// 1.1 (plan §5 item 1a): a member a continuation left out ran nothing. The continuation
			// is its last posted job (nothing is admitted behind it: its generation has ended, or a
			// queued request blocks further continuations and ordinary admission waits for it),
			// so the planning mirrors this job advanced for it at post go back to what Layer 1 holds.
			// Returns true when member I was skipped and is fully handled.
			auto DeliverSkippedMember = [&](int32 I) -> bool
			{
				if (!Out->MemberSkipped[I])
				{
					return false;
				}
				if (FSuperSLMWorkerJobReport* Row = S.JobLedger.Find(LedgerRowCap))
				{
					++Row->SkippedMembers;
				}
				FSlot& Slot = S.Slots[SlotIndicesCopy[I]];
				--Slot.OutstandingJobCount;
				if (WasFinishingCopy[I])
				{
					--Slot.SpeculativeFinishesQueued;
				}
				if (Slot.VendedId == SlotVendedIdsCopy[I])
				{
					Slot.LayersDoneInToken = PreLayersDoneCopy[I];
					Slot.bReadyForLogits = WasReadyForLogitsCopy[I];
				}
				return true;
			};
			// Rule 4 and row 22, per member, before its content: every job pin is released, and
			// the carried entries and the stats copy are confirmed only for a member whose owner
			// (the holder at post) still holds the slot (rule 2).
			for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
			{
				FSlot& Slot = S.Slots[SlotIndicesCopy[I]];
				const bool bOwnerHolds = Slot.VendedId == SlotVendedIdsCopy[I];
				ConfirmCarried(S, Slot, bOwnerHolds, CarriedPerSlot[I], Out->PerSequence[I].CarriedStatuses, /*bCarrierInGeneration*/ true);
				DeliverStats(Slot, bOwnerHolds, Out->PerSequence[I]);
				CountForcedRead(S, Out->PerSequence[I]);
			}
			if (Out->Status != SSLM_OK)
			{
				for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
				{
					if (DeliverSkippedMember(I))
					{
						continue;
					}
					FSlot& Slot = S.Slots[SlotIndicesCopy[I]];
					// Decrement unconditionally -- see DispatchSequencePrefill's own Deliver note.
					--Slot.OutstandingJobCount;
					if (WasFinishingCopy[I])
					{
						--Slot.SpeculativeFinishesQueued;
					}
					if (Slot.VendedId != SlotVendedIdsCopy[I])
					{
						continue; // returned (and possibly re-vended) since this job was posted
					}
					// §19: mirrors the success branch's own terminal-skip, below -- an earlier-
					// committed, later-delivered job (round 9/10's own speculative multi-job-
					// per-slot queuing) may have ALREADY made this slot terminal for a real,
					// correct reason (a real token hit MaxNewTokens/a stop id, or an earlier
					// batch's own -2/-3). A STALE, already-queued job for the SAME slot still
					// executes for real once the sequence is already dead/dead-ended -- Layer 1
					// itself then refuses the call outright (SSLM_ARTIFACT_REJECTED/
					// SSLM_INVALID_ARGUMENT, confirmed at source: sslm_decode_stepImpl treats a
					// dead-ended or otherwise terminal sequence as an invalid call, not a
					// graceful per-token -2/-3) -- and without this guard, S.Fault() below
					// OVERWRITES the slot's already-correct terminal LastOutcome/FaultMessage
					// (e.g. SchemaDeadEnd) with a generic "Generating"-tagged decode failure,
					// which is exactly SchemaDeadEndSurfacedByName's own observed failure
					// ("last decode outcome must be SchemaDeadEnd").
					if (Slot.Phase == ESuperSLMSequencePhase::Complete || Slot.Phase == ESuperSLMSequencePhase::Faulted)
					{
						continue; // an earlier-committed, later-delivered job already made this slot terminal
					}
					if (Out->Status == SSLM_KV_BUDGET_EXCEEDED)
					{
						FaultAtEngineLimit(S, Slot, TEXT("sslm_decode_step_v2"));
						continue;
					}
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(TEXT("sslm_decode_step_v2 failed (%s)"), *StatusText(Out->Status)));
				}
				return;
			}
			int32 Finishes = 0;
			for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
			{
				if (DeliverSkippedMember(I))
				{
					continue;
				}
				FSlot& Slot = S.Slots[SlotIndicesCopy[I]];
				--Slot.OutstandingJobCount;
				if (WasFinishingCopy[I])
				{
					--Slot.SpeculativeFinishesQueued; // unconditional -- §17, mirrors OutstandingJobCount
				}
				if (Slot.VendedId != SlotVendedIdsCopy[I])
				{
					continue; // returned (and possibly re-vended, resetting the new occupant's own count)
				}
				if (Slot.Phase == ESuperSLMSequencePhase::Complete || Slot.Phase == ESuperSLMSequencePhase::Faulted)
				{
					continue; // an earlier-committed, later-delivered job already made this slot terminal
				}
				const int32 Token = Out->DecodeTokens[I];
				// Schema fast-decode plan rev 1.3.5, §6 P1-X: the walk, the pending token and the
				// forcedness read, before the token reaches the output.
				NoteJumpDecodeResult(S, Slot, JobIdCap, Token, Out->PerSequence[I]);

				if (Token >= 0)
				{
					++Finishes;
					if (!WasReadyForLogitsCopy[I])
					{
						Slot.ContextUsed += 1;
					}
					Slot.Generated.Add(Token);
					Slot.LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
					++S.TokensFinishedTotal;
					if (Slot.Generated.Num() == 1 && Slot.BeginSeconds > 0.0)
					{
						Slot.TimeToFirstTokenMs = (FPlatformTime::Seconds() - Slot.BeginSeconds) * 1000.0;
					}
					if (Slot.StopTokenIds.Contains(Token) || Slot.Generated.Num() >= Slot.MaxNewTokens)
					{
						Slot.Phase = ESuperSLMSequencePhase::Complete;
					}
					// A mid-generation adapter swap is carried by the next job of this generation
					// and applied at its head on the worker (plan §2.5 row 20 rule 4, row 22).
				}
				else if (Token == -1)
				{
					Slot.LastOutcome = ESuperSLMDecodeOutcome::Generating;
				}
				else if (Token == -2)
				{
					++Finishes;
					S.Fault(Slot, ESuperSLMDecodeOutcome::SchemaDeadEnd,
						FString::Printf(TEXT("schema '%s' has no legal continuation after %d generated tokens (Schema Dead End)"), *Slot.BoundSchemaName, Slot.Generated.Num()));
				}
				else if (Token == -3)
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::SequenceNoLongerValid, TEXT("Layer 1 reports the sequence is no longer live (-3)"));
				}
				else
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(TEXT("sslm_decode_step_v2 wrote an unrecognized value %d"), Token));
				}
			}
			if (bAllFinishOnly && Finishes > 0)
			{
				S.LastFinishMs = WorkerCallMs / Finishes;
			}
		};

		// D-SLM7528 (plan §5 item 3): "for a multi-entity job (a decode batch), ChooseLane is
		// called once, keyed on the group's own shared pin state -- guaranteed homogeneous by the
		// partition key below -- and its result is written back to every member's PinnedLane." The
		// caller (PlanNextJobs) groups by (PinnedLayerBudget, PinnedLane), so every member of
		// SlotIndices already shares one PinnedLane value; a plain local copy, seeded from the
		// first member, is what ChooseLane resolves/writes, and every member is then set to the
		// SAME resolved value below -- one chooser, one write-back, not N independent choices.
		int32 GroupPinnedLane = SlotIndices.Num() > 0 ? S.Slots[SlotIndices[0]].PinnedLane : INDEX_NONE;
		const int32 CommittedDeliveryTick = PostLedgerJob(S, GroupPinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, Composition.TotalLayers, 0, Composition.TotalFinishes, Composition.PlannedMs,
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver), ChainBaseTick);
		const int64 PostedJobId = S.JobLedger.Last()->JobId;
		// Diagnostic only (R-S1d): the ledger row PostLedgerJob just appended names every member.
		for (const int64 MemberId : SlotVendedIdsCopy)
		{
			S.JobLedger.Last()->MemberSequences.Add(FSuperSLMSequence{MemberId});
		}
		// D-SLM7794 (T-3002 S1): the positions this job serves, from the composition's own layer
		// count per slot and depth -- not from its planned cost.
		S.JobLedger.Last()->DecodeLayerDepthSum = Composition.DecodeLayerDepthSum;

		for (int32 I = 0; I < SlotIndices.Num(); ++I)
		{
			FSlot& Member = S.Slots[SlotIndices[I]];
			Member.NextAvailableTick = CommittedDeliveryTick;
			Member.PinnedLane = GroupPinnedLane;
			Member.ContinuableJobId = PostedJobId;
			if (bContinuation)
			{
				Member.QueuedContinuation = Tickets[I];
			}
		}
	}

	// Activates a front-of-queue Generate request on an Idle, not-in-flight sequence: the
	// caller-visible request field validation already ran synchronously in BeginGeneration();
	// this checks the prefix-context-dependent conditions that are only knowable once any
	// AdoptPrefix() ahead of it in the queue has actually drained.
	void ActivateGenerate(FSuperSLMSubsystemState& S, FSlot& Slot, const FSuperSLMGenerationRequest& Request, int64 GenerateOrdinal)
	{
		// Plan §2.5 row 20 rule 4 (T-2988 W5): a held request refused by a job outside the
		// generation it preceded faults this, the holder's next generation, by name.
		if (Slot.bHasDeferredRefusal)
		{
			const FString Refusal = Slot.DeferredRefusal;
			Slot.bHasDeferredRefusal = false;
			Slot.DeferredRefusal.Reset();
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, Refusal);
			return;
		}
		const int64 PrefixContext = Slot.bAdopted ? Slot.ContextUsed : 0;
		if (Request.PromptTokens.Num() == 0 && !Slot.bAdopted)
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, TEXT("the prompt is empty and no prefix was adopted, so there is nothing to generate from"));
			return;
		}
		if (PrefixContext + Request.PromptTokens.Num() > S.Shape.ContextCap)
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(
				TEXT("the prompt's %d tokens after a %lld-token prefix exceed the model's context_cap of %lld (SSLM_CONTEXT_CAP_EXCEEDED)"),
				Request.PromptTokens.Num(), PrefixContext, S.Shape.ContextCap));
			return;
		}
		// Plugin paging plan §3.3 "Generation limit", §3.4 "Activation pre-check": in budget mode the
		// prompt after the holder's origin must fit its limit, min(origin + SequenceBudget, cap).
		if (S.IsBudgetMode())
		{
			const int32 SlotIndex = static_cast<int32>(&Slot - S.Slots.GetData());
			const int64 Limit = S.Ledger.SlotLimit(SlotIndex);
			const SuperSLMPaging::ELimitVerdict Verdict = SuperSLMPaging::CheckActivation(PrefixContext, Request.PromptTokens.Num(), Limit, S.Shape.ContextCap);
			if (Verdict != SuperSLMPaging::ELimitVerdict::Allowed)
			{
				FaultAtLimit(S, Slot, Verdict, Limit, *FString::Printf(TEXT("the prompt's %d tokens after a %lld-token prefix"), Request.PromptTokens.Num(), PrefixContext));
				return;
			}
		}
		// Rule 4 (T-2989 F21): the value of the holder's last held schema entry counts when one
		// exists, so a held bind counts as bound and a held unbind after it as unbound. Under the
		// gates activation follows every earlier delivery, so no entry is carried and unconfirmed
		// here.
		if (Request.SpanKind == ESuperSLMSpanKind::SchemaContent && Slot.EffectiveSchemaName().IsEmpty())
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, TEXT("a schema-content span needs a bound schema (SSLM_SCHEMA_SPAN_UNBOUND); call SetSchema() first"));
			return;
		}

		Slot.ClearForBegin();
		Slot.ActiveGenerateOrdinal = GenerateOrdinal;
		Slot.bGenerateFirstJobPosted = false;
		Slot.Prompt = Request.PromptTokens;
		Slot.SpanKind = Request.SpanKind;
		Slot.MaxNewTokens = Request.MaxNewTokens;
		Slot.StopTokenIds = Request.StopTokenIds;
		Slot.Generated.Reserve(Request.MaxNewTokens);
		Slot.BeginSeconds = FPlatformTime::Seconds();
		Slot.Phase = Slot.Prompt.Num() > 0 ? ESuperSLMSequencePhase::Prefilling : ESuperSLMSequencePhase::Decoding;
		BeginJumpGeneration(S, Slot, Request); // schema fast-decode plan rev 1.3.5, §6 P1 and P1-X
	}

	// The Plan half of Tick() (§5): admits at most one lifecycle-kind job this pass (by lowest
	// global request ordinal, D-SLM7428), then fills remaining free lanes with due prefill work
	// (one sequence or prefix per lane -- prefill cannot batch) and finally, on any lane still
	// free, one batched decode job per distinct layer_budget group among due decoding sequences.
	void PlanNextJobs(FSuperSLMSubsystemState& S, FSuperSLMTickReport& Report, int32 TickIndex)
	{
		// 0. A front-of-queue Generate request on a sequence with no job in flight and no restore
		// awaited: on an Idle sequence it activates. On any other phase it is removed, with a named
		// warning, and the sequence stays as it is (D-SLM7946). BeginGeneration() refuses every
		// generation whose turn would not find the sequence Idle, so the one that reaches here is
		// behind work that left the sequence Faulted: a reset Layer 1 refused (D-SLM7947), or a prefix
		// adoption whose prefix faulted. Left at the front it would block every later request on the
		// sequence, because the lifecycle pass below
		// skips a Generate front.
		for (FSlot& Slot : S.Slots)
		{
			if (Slot.VendedId == 0 || Slot.HasUndeliveredJob() || Slot.bAwaitingRestore)
			{
				continue;
			}
			if (Slot.OpQueue.IsEmpty() || Slot.OpQueue[0].Kind != ESlotOpKind::Generate)
			{
				continue;
			}
			if (Slot.Phase != ESuperSLMSequencePhase::Idle)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("Sequence %lld: one generation at a time: the queued generation was not started, because the sequence was %s, not Idle, when its turn came (a reset or prefix adoption queued ahead of it left it so); reset it"),
					Slot.VendedId, SequencePhaseName(Slot.Phase));
				Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
				continue;
			}
			const FSuperSLMGenerationRequest Request = Slot.OpQueue[0].GenRequest;
			const int64 GenerateOrdinal = Slot.OpQueue[0].Ordinal;
			Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
			ActivateGenerate(S, Slot, Request, GenerateOrdinal);
		}

		TArray<int32> FreeLanes;
		for (int32 L = 0; L < S.Lanes.Num(); ++L)
		{
			if (S.Lanes[L].IsFreeForPlanning(TickIndex))
			{
				FreeLanes.Add(L);
			}
		}
		if (FreeLanes.IsEmpty())
		{
			return;
		}

		// 1. At most one lifecycle-kind candidate admitted this pass, by lowest global ordinal.
		// D-SLM7528 (plan §5 item 3): a candidate pinned to a lane that is not free for planning
		// this tick is not due at all -- IsLanePinFree excludes it here, the same due-ness rule
		// every other admission below applies. Nothing has been dispatched yet this tick when this
		// block runs, so a pinned candidate's own lane, once IsLanePinFree admits it, is guaranteed
		// present in FreeLanes for ChooseLane -- this block only ever dispatches ONE job total, so
		// no second candidate can have consumed it first.
		{
			int64 BestOrdinal = TNumericLimits<int64>::Max();
			int32 BestSlotIndex = INDEX_NONE;
			int32 BestAdminIndex = INDEX_NONE;
			for (int32 I = 0; I < S.Slots.Num(); ++I)
			{
				FSlot& Slot = S.Slots[I];
				// §14.8 item 4: a slot's OWN queue is what decides whether it has lifecycle work,
				// not VendedId. ReturnSequence() zeroes VendedId THEN appends this slot's own
				// recycle Reset (§4, D-SLM7421) -- a VendedId==0 exclusion here skipped that op
				// forever (FrameBudget's "6 Reset jobs" ledger count read 0), because the very
				// state ReturnSequence() leaves behind is the one this clause excluded. A
				// genuinely idle free slot has an empty OpQueue and is still excluded, below.
				if (Slot.HasUndeliveredJob() || Slot.OpQueue.IsEmpty() || !IsLanePinFree(Slot.PinnedLane, S.Lanes, TickIndex))
				{
					continue;
				}
				const FSlotQueuedOp& Front = Slot.OpQueue[0];
				if (Front.Kind == ESlotOpKind::Generate)
				{
					continue;
				}
				if (Front.Kind == ESlotOpKind::Adopt)
				{
					const FPrefixEntry* Prefix = S.Prefixes.Find(Front.PrefixId);
					const bool bRunnable = Prefix == nullptr || Prefix->Phase == ESuperSLMPrefixPhase::Ready || Prefix->Phase == ESuperSLMPrefixPhase::Faulted;
					if (!bRunnable)
					{
						continue;
					}
				}
				// Plugin paging plan §3.9: an admitted op parked at its HoldStart() hold waits there.
				// §3.5 (K3): so does a restore waiting on H, until the ledger admits it.
				if (Front.bWaiting || IsParkedAtStartHold(S, Front))
				{
					continue;
				}
				if (Front.Ordinal < BestOrdinal)
				{
					BestOrdinal = Front.Ordinal;
					BestSlotIndex = I;
					BestAdminIndex = INDEX_NONE;
				}
			}
			for (int32 A = 0; A < S.PrefixAdminQueue.Num(); ++A)
			{
				const FPrefixAdminOp& Op = S.PrefixAdminQueue[A];
				if (Op.Kind == EPrefixAdminKind::Begin)
				{
					const FPrefixEntry* Prefix = S.Prefixes.Find(Op.PrefixId);
					if (Prefix == nullptr || Prefix->bAdminJobInFlight || !IsLanePinFree(Prefix->PinnedLane, S.Lanes, TickIndex))
					{
						continue;
					}
					// 1.1 (RetiringPrefixes): a released prefix keeps its place until its release has
					// delivered. K2 review F2: this gate holds in both modes. In budget mode the page
					// ledger admitted the build's pages at CreatePrefix() (region P) and freed the
					// released prefix's count slot at its release admission, but this gate bounds the
					// prefix jobs in flight, which is what keeps the §5.3 per-lane pending-job bound
					// (BlockCount + PrefixBlockCount + 1).
					int32 Occupied = S.RetiringPrefixes.Num();
					for (const TPair<int64, FPrefixEntry>& Pair : S.Prefixes)
					{
#if SUPERSLM_WITH_PREFIX_CACHE
						if (SuperSLMPrefixCacheGlue::IsShadow(S, Pair.Key))
						{
							continue; // precache plan §5.4: a cache lease holds no prefix block
						}
#endif
						Occupied += (Pair.Value.Handle != nullptr || Pair.Value.bAdminJobInFlight) ? 1 : 0;
					}
					if (Occupied >= S.Config.PrefixBlockCount)
					{
						continue;
					}
				}
				else
				{
					// Release: T-2815 round 13's own regression, found at source after the build
					// (QueueFullFixedArrivalOrder/QueueFullSweep/SharedPrefixAdopted/
					// PrefixBeginReleaseCompleteWithinBound all failed on a teardown leak with no
					// PrefixRelease ledger row ever posted). ReleasePrefix()'s real-block path
					// removes the caller-visible FPrefixEntry from State->Prefixes SYNCHRONOUSLY
					// (the pool-leak fix, above), so `S.Prefixes.Find(Op.PrefixId)` is ALWAYS
					// nullptr for a queued Release op by the time it reaches here -- the
					// `Prefix == nullptr` exclusion above, inherited unchanged from before that
					// fix, therefore excluded every Release op permanently, not merely a stale one.
					// No in-flight check is needed here: ReleasePrefix() only ever queues a
					// Release once its prefix's own Begin has already delivered (Entry->Handle !=
					// nullptr is required to reach this path at all) and never queues a second
					// Release for the same id, so there is no admin job for this same prefix that
					// could still be in flight. Op.PinnedLane, captured at ReleasePrefix() call
					// time, stands in for the vanished entry's own field.
					// 1.1 (plan §5 item 1a, "Lane depth"): the comment above held for the Begin, but
					// a prefill job can still be in flight when its prefix is released. The release
					// waits for it, so a prefix never has two jobs posted and undelivered.
					const FSuperSLMSubsystemState::FRetiringPrefix* Retiring = S.RetiringPrefixes.Find(Op.PrefixId);
					if ((Retiring != nullptr && Retiring->bJobInFlight) || !IsLanePinFree(Op.PinnedLane, S.Lanes, TickIndex))
					{
						continue;
					}
				}
				// Plugin paging plan §3.9: an op parked at its HoldStart() hold waits there. K2 review
				// F9: checked after the op's other gates, so the hold is marked reached only where it
				// is what actually parks the op (the job would otherwise be posted).
				if (S.IsHoldBlocking(Op.StartHoldId))
				{
					S.MarkHoldReached(Op.StartHoldId);
					continue;
				}
				if (Op.Ordinal < BestOrdinal)
				{
					BestOrdinal = Op.Ordinal;
					BestSlotIndex = INDEX_NONE;
					BestAdminIndex = A;
				}
			}
			if (BestSlotIndex != INDEX_NONE)
			{
				const int64 JobIdBeforeDispatch = S.NextJobId;
				DispatchSlotLifecycleOp(S, FreeLanes, BestSlotIndex, TickIndex);
				Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
			}
			else if (BestAdminIndex != INDEX_NONE)
			{
				const int64 JobIdBeforeDispatch = S.NextJobId;
				DispatchPrefixAdmin(S, FreeLanes, BestAdminIndex, TickIndex);
				Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
			}
		}

		// 2. Due prefill work (sequences, then prefixes), one item per remaining free lane.
		// D-SLM7528: due-ness includes IsLanePinFree; CanClaimLane guards each dispatch, because a
		// SECOND candidate sharing an already-claimed pinned lane can appear later in this same
		// scan (two sequences pinned to one lane can both be due at the top of a tick) -- that
		// candidate is simply left due for a later tick rather than force-posted a stale lane.
		TArray<int32> DuePrefillSlots;
		for (int32 I = 0; I < S.Slots.Num(); ++I)
		{
			if (S.Slots[I].IsDue(TickIndex) && S.Slots[I].Phase == ESuperSLMSequencePhase::Prefilling
				&& IsLanePinFree(S.Slots[I].PinnedLane, S.Lanes, TickIndex))
			{
				DuePrefillSlots.Add(I);
			}
		}
		for (int32 CandidateSlotIndex : DuePrefillSlots)
		{
			if (FreeLanes.IsEmpty())
			{
				break;
			}
			if (!CanClaimLane(S.Slots[CandidateSlotIndex].PinnedLane, FreeLanes))
			{
				continue;
			}
			const int64 JobIdBeforeDispatch = S.NextJobId;
			DispatchSequencePrefill(S, FreeLanes, CandidateSlotIndex, TickIndex);
			Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
		}
		TArray<int64> DuePrefillPrefixes;
		for (const TPair<int64, FPrefixEntry>& Pair : S.Prefixes)
		{
			if (Pair.Value.Phase == ESuperSLMPrefixPhase::Prefilling && !Pair.Value.bAdminJobInFlight
				&& IsLanePinFree(Pair.Value.PinnedLane, S.Lanes, TickIndex))
			{
				DuePrefillPrefixes.Add(Pair.Key);
			}
		}
		for (int64 CandidatePrefixId : DuePrefillPrefixes)
		{
			if (FreeLanes.IsEmpty())
			{
				break;
			}
			const FPrefixEntry* CandidatePrefix = S.Prefixes.Find(CandidatePrefixId);
			if (CandidatePrefix == nullptr || !CanClaimLane(CandidatePrefix->PinnedLane, FreeLanes))
			{
				continue;
			}
			const int64 JobIdBeforeDispatch = S.NextJobId;
			DispatchPrefixPrefill(S, FreeLanes, CandidatePrefixId, TickIndex);
			Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
		}

		// 2b. Schema fast-decode plan rev 1.3.5, §6 P1 and P1-X: a due decoding sequence whose next job
		// is a span job (SuperSLMCore::NextJob: canonical mode's drain rule, exact mode's span after a
		// decode job that left a pending token at a forced state with a non-empty learned draft) gets
		// it on its own pinned lane, one sequence per job. A sequence that wants one is never offered
		// to a decode batch below, so mixed batches hold only drained sequences (P1.7, P1X.9).
		for (int32 I = 0; I < S.Slots.Num() && !FreeLanes.IsEmpty(); ++I)
		{
			const FSlot& CandidateSlot = S.Slots[I];
			if (!CandidateSlot.Jump.bActive || CandidateSlot.Jump.Mode == SuperSLMCore::ESchemaJumpMode::Off)
			{
				continue;
			}
			if (!CandidateSlot.IsDue(TickIndex) || CandidateSlot.Phase != ESuperSLMSequencePhase::Decoding
				|| !IsLanePinFree(CandidateSlot.PinnedLane, S.Lanes, TickIndex) || !CanClaimLane(CandidateSlot.PinnedLane, FreeLanes))
			{
				continue;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (const FSuperSLMSubsystemState::FTestJobHold* Hold = S.TestJobHolds.Find(CandidateSlot.VendedId); Hold != nullptr && Hold->bHeld)
			{
				continue; // the job hold: nothing is planned for a held sequence
			}
#endif
			const FSpanPlan Plan = PlanJumpNextJob(S, I);
			if (Plan.Next != SuperSLMCore::EJumpNextJob::Span)
			{
				continue;
			}
			const int64 JobIdBeforeDispatch = S.NextJobId;
			DispatchSpanJob(S, FreeLanes, I, TickIndex, Plan);
			Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
		}

		// 3. Remaining free lanes: due decoding sequences, grouped by (pinned budget, pinned lane)
		// -- 0/INDEX_NONE meaning the scheduler's own shared choice -- one batched job per group
		// per free lane. D-SLM7528: grouping by PinnedLane too (not PinnedLayerBudget alone) keeps
		// every group homogeneous in its own pin, which DispatchDecodeBatch's single ChooseLane
		// call for the whole group depends on; CanClaimLane guards each group's dispatch the same
		// way step 2 guards a single candidate's.
		if (!FreeLanes.IsEmpty())
		{
			TMap<int64, TArray<int32>> Groups; // key: (PinnedLayerBudget << 32) | uint32(PinnedLane)
			for (int32 I = 0; I < S.Slots.Num(); ++I)
			{
				const FSlot& CandidateSlot = S.Slots[I];
				// §17: schedule-only due-ness (IsDue) has no brake of its own on how far ahead of
				// real delivery a slot may be planned -- under a generous TickBudgetMs, K stays 1
				// and a slot becomes due again almost every tick, so nothing stopped this from
				// queuing far more real decode calls than the sequence could ever consume before
				// MaxNewTokens (confirmed as the CpuDeterminism.Baseline hang, this round). Capped
				// at the number of tokens still needed: every already-queued job that is
				// speculatively predicted to finish (SpeculativeFinishesQueued, §15.1's own
				// Layers>=Left formula) can add at most one real token, so once that count already
				// covers every remaining slot up to MaxNewTokens, composing another is pure waste
				// and is refused here -- never by falling back to real-delivery gating.
				if (CandidateSlot.IsDue(TickIndex) && CandidateSlot.Phase == ESuperSLMSequencePhase::Decoding
					&& CandidateSlot.SpeculativeFinishesQueued < CandidateSlot.MaxNewTokens - CandidateSlot.Generated.Num()
					&& IsLanePinFree(CandidateSlot.PinnedLane, S.Lanes, TickIndex))
				{
					// Schema fast-decode plan rev 1.3.5, §6: a sequence whose next job is a span job
					// (one step 2b could not post this tick) waits for it, never joining a decode batch.
					if (CandidateSlot.Jump.bActive && CandidateSlot.Jump.Mode != SuperSLMCore::ESchemaJumpMode::Off
						&& PlanJumpNextJob(S, I).Next != SuperSLMCore::EJumpNextJob::Decode)
					{
						continue;
					}
#if WITH_DEV_AUTOMATION_TESTS
					// The job hold: a BeforeDecode hold stops the sequence here, the first time it would join a decode
					// job; a held sequence joins none until the test releases it.
					if (FSuperSLMSubsystemState::FTestJobHold* Hold = S.TestJobHolds.Find(CandidateSlot.VendedId))
					{
						if (Hold->Point == ESuperSLMJobHoldPoint::BeforeDecode) { Hold->bHeld = true; }
						if (Hold->bHeld) { continue; }
					}
#endif
					// Plugin paging plan §3.4: the per-step limit pre-check, before the step is
					// grouped. A due sequence has no job in flight, so ContextUsed is exact here.
					if (!PreCheckDecodeStep(S, I, CandidateSlot.ContextUsed, /*bFaultOnRefusal*/ true))
					{
						continue;
					}
					const int64 GroupKey = (static_cast<int64>(CandidateSlot.PinnedLayerBudget) << 32) | static_cast<uint32>(CandidateSlot.PinnedLane);
					Groups.FindOrAdd(GroupKey).Add(I);
				}
			}
			for (const TPair<int64, TArray<int32>>& Pair : Groups)
			{
				if (FreeLanes.IsEmpty())
				{
					break;
				}
				// Homogeneous by construction (the group key includes PinnedLane): every member
				// shares one pin, so checking the first member speaks for the whole group.
				if (!CanClaimLane(S.Slots[Pair.Value[0]].PinnedLane, FreeLanes))
				{
					continue;
				}
				const int32 GroupBudget = static_cast<int32>(Pair.Key >> 32);
				const int64 JobIdBeforeDispatch = S.NextJobId;
				DispatchDecodeBatch(S, FreeLanes, Pair.Value, GroupBudget, TickIndex);
				Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
			}
		}
	}

	// 1.1 worker continuation (plan §5 item 1a, ruled 2026-09-29). Runs after
	// PlanNextJobs. For each lane whose one pending job is a generation's prefill or decode job, and
	// on which nothing else is waiting, it posts that generation's next job now, queued behind the
	// running one, so the worker starts it the moment the running job finishes rather than idling
	// until the next Tick() delivers it and posts the next. The next job is composed exactly as the
	// next tick would compose it: from the planning mirrors, which every dispatcher already advances
	// at post (§14.8 item 2). It is the one exception to D-SLM7515's one job in flight per sequence:
	// at most two, the second a continuation on the same pinned lane, where the lane's single thread
	// runs them in order, so two Layer-1 calls for one sequence never overlap. Whether the
	// generation is still running when the continuation reaches the worker is content-dependent,
	// and the generation's gate (FGenerationGate) answers it there, so a continuation never runs a
	// sequence past its end.
	//
	// Anything else that wants the lane blocks the continuation, and the lane goes back to 1.0's
	// hand-off at the next free tick: another sequence's work (pinned to the lane or not yet pinned),
	// a member's queued lifecycle op or held request, or any prefix work. So a continuation never
	// delays other work by more than the one job it adds, and never reorders anything queued.
	void PlanContinuations(FSuperSLMSubsystemState& S, FSuperSLMTickReport& Report, int32 TickIndex)
	{
		if (!S.Config.bWorkerContinuation)
		{
			return;
		}
		// Prefix work is rare and administrative; it keeps 1.0's hand-off on every lane.
		if (!S.PrefixAdminQueue.IsEmpty())
		{
			return;
		}
		for (const TPair<int64, FPrefixEntry>& Pair : S.Prefixes)
		{
			if (Pair.Value.Phase == ESuperSLMPrefixPhase::Prefilling || Pair.Value.bAdminJobInFlight)
			{
				return;
			}
		}
		for (int32 L = 0; L < S.Lanes.Num(); ++L)
		{
			const FLane& Lane = S.Lanes[L];
			// Precache plan rev 1.18.10 (§15.29): no continuation on a lane held for a cache build's chunk.
			if (Lane.Pending.Num() != 1 || Lane.Pending[0].Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || Lane.IsHeldForChunk() || Lane.Pending[0].bSpanJob)
			{
				continue;
			}
			const int64 RunningJobId = Lane.Pending[0].JobId;
			const int32 ChainBaseTick = Lane.Pending[0].CommittedDeliveryTick;
			TArray<int32> Members;
			bool bBlocked = false;
			for (int32 I = 0; I < S.Slots.Num() && !bBlocked; ++I)
			{
				const FSlot& Slot = S.Slots[I];
				const bool bInRunningJob = Slot.PinnedLane == L && Slot.OutstandingJobCount == 1 && Slot.ContinuableJobId == RunningJobId;
				if (bInRunningJob)
				{
					// A member with anything queued, held or returned keeps 1.0's order.
					if (Slot.VendedId == 0 || Slot.bAwaitingRestore || !Slot.OpQueue.IsEmpty() || !Slot.Held.IsEmpty())
					{
						bBlocked = true;
						continue;
					}
					// A member whose generation has ended, or whose last token is already in flight
					// (§17's cap), has nothing to continue; it neither joins nor blocks.
					if (!Slot.IsGenerating() || !Slot.bGenerateFirstJobPosted)
					{
						continue;
					}
					// Schema fast-decode plan rev 1.3.5, §6: under jump-forward the next job depends on
					// this one's forcedness read (a span job or a decode job), which only its delivery
					// gives, so the generation keeps 1.0's hand-off; it neither joins nor blocks.
					if (Slot.Jump.bActive && Slot.Jump.Mode != SuperSLMCore::ESchemaJumpMode::Off)
					{
						continue;
					}
					if (Slot.Phase == ESuperSLMSequencePhase::Decoding && Slot.SpeculativeFinishesQueued >= Slot.MaxNewTokens - Slot.Generated.Num())
					{
						continue;
					}
					// Plugin paging plan §3.4, budget mode: the running job's own embed lands in
					// ContextUsed only at its delivery, so the continuation's step is checked as if
					// it had (one position more). A member that could reach its limit neither joins
					// nor blocks; the next ordinary planning pass checks it exactly.
					if (Slot.Phase == ESuperSLMSequencePhase::Decoding && !PreCheckDecodeStep(S, I, Slot.ContextUsed + 1, /*bFaultOnRefusal*/ false))
					{
						continue;
					}
					Members.Add(I);
					continue;
				}
				const bool bMayUseLane = Slot.PinnedLane == L || Slot.PinnedLane == INDEX_NONE;
				if (bMayUseLane && (!Slot.OpQueue.IsEmpty() || Slot.IsGenerating() || Slot.bAwaitingRestore || Slot.HasUndeliveredJob()))
				{
					bBlocked = true;
				}
			}
			if (bBlocked || Members.IsEmpty())
			{
				continue;
			}

			TArray<int32> CandidateLanes;
			CandidateLanes.Add(L);
			const int64 JobIdBeforeDispatch = S.NextJobId;
			const FSlot& First = S.Slots[Members[0]];
			if (First.Phase == ESuperSLMSequencePhase::Prefilling)
			{
				// A prefill job serves one sequence, so its continuation does too.
				if (Members.Num() != 1)
				{
					continue;
				}
				DispatchSequencePrefill(S, CandidateLanes, Members[0], TickIndex, ChainBaseTick);
			}
			else
			{
				// A decode continuation keeps the running job's group: every member decoding, one
				// shared layer-budget pin (the group key PlanNextJobs batches by).
				bool bOneGroup = true;
				for (int32 Idx : Members)
				{
					const FSlot& M = S.Slots[Idx];
					bOneGroup &= M.Phase == ESuperSLMSequencePhase::Decoding && M.PinnedLayerBudget == First.PinnedLayerBudget;
				}
				if (!bOneGroup)
				{
					continue;
				}
				DispatchDecodeBatch(S, CandidateLanes, Members, First.PinnedLayerBudget, TickIndex, ChainBaseTick);
			}
			Report.JobsPlannedThisTick += static_cast<int32>(S.NextJobId - JobIdBeforeDispatch); // O1: posted jobs only
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Tick (§5, §5.1, D-SLM7403/D-SLM7407 -- the async worker tick)
// ---------------------------------------------------------------------------------------------

bool USuperSLMSubsystem::TickOncePerFrame(float DeltaSeconds)
{
	if (LastTickFrame == GFrameCounter)
	{
		return false;
	}
	Tick(DeltaSeconds);
	return true;
}

void USuperSLMSubsystem::Tick(float DeltaSeconds)
{
	LastTickFrame = GFrameCounter; // review W6
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Tick", SuperSLMChannel);
	if (State == nullptr)
	{
		return;
	}
	FSuperSLMSubsystemState& S = *State;
	FSuperSLMTickReport Report;
	// D-SLM7799: the tick horizon. At kTickHorizon the counter stops advancing and nothing more is
	// planned; jobs already posted still deliver (their committed ticks are within range), every
	// live sequence faults, and new requests are refused, until Configure() resets the counter.
	const bool bAtHorizon = S.TickCounter >= kTickHorizon;
	if (bAtHorizon && !S.bTickHorizonReached)
	{
		S.bTickHorizonReached = true;
		S.TickHorizonMessage = FString::Printf(TEXT("tick horizon reached after %d ticks: call Configure() again"), S.TickCounter);
		UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM: %s"), *S.TickHorizonMessage);
		for (FSlot& Slot : S.Slots)
		{
			if (Slot.VendedId != 0 && Slot.Phase != ESuperSLMSequencePhase::Faulted)
			{
				S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, S.TickHorizonMessage);
			}
		}
	}
	Report.TickIndex = bAtHorizon ? S.TickCounter : S.TickCounter++;
	S.CurrentTickIndex = Report.TickIndex; // a span job's DeliveredAtTick (schema fast-decode plan rev 1.3.5)
	PublishSchemaWalkLoad(S, Report.TickIndex); // rev 1.3.6, review finding 1: a finished walk-table load

	// 1. Apply -- never a Layer-1 call (R-S1b (ii), R-S1g). For every lane whose job the worker
	// has finished, copy its result onto the sequence(s)/prefix/handle it belongs to.
	const double ApplyStart = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Tick.Apply", SuperSLMChannel);
		for (FLane& Lane : S.Lanes)
		{
			// D-SLM7728 (plan §2.5 row 22, §5 Apply): a finished result is delivered at the first
			// tick after the worker finishes it; it is no longer held back to its planned tick.
			// The planned tick (CommittedDeliveryTick = PlannedAtTick + K) stays a planning figure,
			// and a result delivered after it is a hitch. PlanNextJobs' own free-lane/free-slot
			// checks (IsFreeForPlanning/IsDue) are unchanged. Drained front-to-back, stopping at the
			// first unfinished job. Per-entity order is always preserved, because each sequence or
			// prefix has at most one undelivered job; entries for different entities apply out of lane
			// order only past an entry a test seam's HoldApply() hold keeps (K3).
			int32 Index = 0;
			while (Index < Lane.Pending.Num())
			{
				FLanePendingDelivery& Front = Lane.Pending[Index];
				checkf(Front.WorkerJob.IsValid(), TEXT("SuperSLM CPU lane holds a null pending job -- Post() never stores a null Ptr, so this is an internal invariant violation, not a runtime condition to degrade past."));
				const bool bWorkerDone = Front.WorkerJob->bDone.load(std::memory_order_acquire);
				if (!bWorkerDone)
				{
					break;
				}
				// Plugin paging plan §3.9: a job held at its HoldApply() hold stays undelivered until
				// ReleaseHold(). It holds only its own delivery (step K3): every later result on its
				// lane belongs to another entity, because a sequence or a prefix never has a second
				// job posted while one is undelivered (and holds attach to no continuation), so they
				// deliver past it and their entities' order is kept. A lane shared by a held restore
				// and a sequence pinned to it (cell 1.8 (b)) needs this: FIFO would park the
				// sequence's reset behind the hold.
				if (S.IsHoldBlocking(Front.ApplyHoldId))
				{
					S.MarkHoldReached(Front.ApplyHoldId);
					++Index;
					continue;
				}
				const double WorkerCallMs = Front.WorkerJob->WorkerCallMs;
				const uint32 WorkerThreadId = Front.WorkerJob->WorkerThreadId;
				const double WorkerSpanMs = (Front.WorkerJob->FinishedSeconds - Front.WorkerJob->PostedSeconds) * 1000.0;
				// The hitch rule and the counters read the job's own figures, copied at post, so they
				// hold even when the ring has overwritten the job's ledger row; the row, while
				// retained, records the same figures.
				// Slice 1P (tiled-matmul plan §4A.1 item 4, rev 11.1 audit F8): a worker overrun is a
				// job whose own Layer-1 time exceeded its own planned K ticks -- the job's own K, never
				// ChainedWaitTicks + K (a continuation's queue wait is not worker time, and WorkerCallMs
				// excludes it). At K = 1 this is 1.0's rule. One predicate, behind the scheduling seam.
				const bool bWorkerOverran = SuperSLMPrefillPlanning::IsWorkerOverrun(WorkerCallMs, Front.K, S.Config.TickBudgetMs);
				// D-SLM7407, plan §5 ("What counts as a hitch"): a result is a hitch when it is
				// delivered after its committed tick AND the worker was left at least K ticks'
				// worth of real wall time (K x TickBudgetMs, posted to finished) and still needed
				// more. A result delivered at or before its committed tick is never one. Without
				// the wall-time half a host ticking faster than TickBudgetMs (the editor's frame
				// loop against an 8 ms budget) counted its own pace as worker lateness (T-2818 box
				// follow-up). WorkerSpanMs is published on the ledger so tests apply this rule.
				const bool bLateByTicks = Report.TickIndex > Front.CommittedDeliveryTick;
				// 1.1 (plan §5 item 1a): a continuation is posted behind the job it follows, so its
				// span includes that wait; ChainedWaitTicks (0 for every other job) is the part of the
				// committed tick the wait accounts for, and the wall-time half allows for it the same way.
				const bool bHitch = bLateByTicks && WorkerSpanMs > double(Front.ChainedWaitTicks + Front.K) * S.Config.TickBudgetMs;
				if (FSuperSLMWorkerJobReport* LedgerEntry = S.JobLedger.Find(Front.LedgerRow))
				{
					LedgerEntry->WorkerCallMs = WorkerCallMs;
					LedgerEntry->WorkerThreadId = WorkerThreadId;
					LedgerEntry->bWorkerOverran = bWorkerOverran;
					LedgerEntry->DeliveredAtTick = Report.TickIndex;
					LedgerEntry->WorkerSpanMs = WorkerSpanMs;
					LedgerEntry->bHitch = bHitch;
				}
				if (bHitch)
				{
					++S.HitchCount;
					++Report.LateJobsThisTick;
					TRACE_BOOKMARK(TEXT("SuperSLM: CPU job late by %d tick(s) (job %lld)"), Report.TickIndex - Front.CommittedDeliveryTick, Front.JobId);
				}
				if (bWorkerOverran)
				{
					++S.HitchCount;
				}
				switch (Front.Kind)
				{
					case ESuperSLMWorkerJobKind::Reset: S.LastResetMs = WorkerCallMs; break;
					case ESuperSLMWorkerJobKind::Adopt: S.LastAdoptMs = WorkerCallMs; break;
					default: break;
				}
				if (Front.Deliver)
				{
					Front.Deliver(WorkerCallMs);
				}
				++Report.JobsDeliveredThisTick;
				Lane.Pending.RemoveAt(Index, EAllowShrinking::No);
			}
		}
		// Plugin paging plan §3.5 (K3): the applies above may have admitted waiting restores.
		ResolveWaitingRestores(S);
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.5: the cache applies its arrived completions and starts its next commands.
		SuperSLMPrefixCacheGlue::Pump(S);
#endif
		// Schema fast-decode plan rev 1.3.5, §6 P1-X: the generations the applies ended feed the
		// learned draft, on the game thread.
		FinishJumpGenerations(S);
	}
	Report.ApplyMs = (FPlatformTime::Seconds() - ApplyStart) * 1000.0;

	// 2. Plan -- sizes and dispatches the next job(s) from the static per-unit cost model.
	const double PlanStart = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Tick.Plan", SuperSLMChannel);
		if (!S.bTickHorizonReached)
		{
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::NoteWaitingChunks(S); // precache plan rev 1.18.10 (§15.29): lanes held for a chunk
#endif
			PlanNextJobs(S, Report, Report.TickIndex);
			PlanContinuations(S, Report, Report.TickIndex); // 1.1, plan §5 item 1a
#if SUPERSLM_WITH_PREFIX_CACHE
			SuperSLMPrefixCacheGlue::DispatchLaneJobs(S, Report.TickIndex); // precache plan §5.5
#endif
		}
	}
	Report.PlanMs = (FPlatformTime::Seconds() - PlanStart) * 1000.0;
	Report.DurationMs = Report.PlanMs + Report.ApplyMs;

	S.TickHistory.Append() = Report;
	S.LastReport = Report;
	S.PublishStats(Report);
}

// ---------------------------------------------------------------------------------------------
// Save / restore (§4, D-SLM7408/D-SLM7421 -- queued lifecycle operations)
// ---------------------------------------------------------------------------------------------

#if SUPERSLM_WITH_L2S1_ASYNC
ESuperSLMRestoreResult USuperSLMSubsystem::SaveSequence(const FSuperSLMSequence& Sequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandle();
	if (State == nullptr)
	{
		OutError = TEXT("the subsystem is not configured");
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage; // D-SLM7799: refused like an unconfigured subsystem
		return ESuperSLMRestoreResult::NotConfigured;
	}
	FSlot* Slot = State->Find(Sequence.Id);
	if (Slot == nullptr)
	{
		OutError = TEXT("not a live sequence handle");
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (Slot->EffectiveQueueDepth() >= State->Config.MaxQueuedOperationsPerSequence)
	{
		OutError = FString::Printf(TEXT("this sequence's own queue already holds %d entries (SSLM_SEQUENCE_QUEUE_FULL)"), State->Config.MaxQueuedOperationsPerSequence);
		return ESuperSLMRestoreResult::SequenceQueueFull;
	}

	FSlotQueuedOp Op;
	Op.Kind = ESlotOpKind::Save;
	Op.Ordinal = State->MintOrdinal();
	Op.HandleId = State->MintHandle();
	Op.Owner = Slot->VendedId;
	Slot->OpQueue.Add(Op);
	Slot->CancelQueuedContinuation(); // 1.1: a queued op precedes the generation's next job
	OutHandle.Id = Op.HandleId;
	return ESuperSLMRestoreResult::Success;
}

namespace
{
	// RestoreSequence()'s one body, shared by both overloads. Synchronous refusals (§4) are
	// checked, and refused, before anything is queued and before Layer 1 is ever asked --
	// BackendMismatch and Layer1Mismatch are the plugin's own tag checks; NotConfigured and
	// PoolExhausted are pure subsystem-state checks; Malformed means the wrapper itself could not
	// even be parsed. Every other outcome (ModelMismatch, KvMismatch, ResidualLost,
	// AdapterUnavailable, Success) is a Layer-1-or-artifact-sourced result delivered through the
	// same queued path every other lifecycle op uses (D-SLM7408).
	//
	// Plan §2.5 row 19: validation reads the caller's array in place; only an accepted request
	// creates the shared immutable buffer every later holder uses -- by one copy, or by taking
	// the caller's array when MovableBlob is given, so no copy is ever taken after acceptance.
	// Plugin paging plan §3.5 (step K3): the Layer-1 payload header, read by the plugin at the call
	// (plugin code parsing bytes, never a Layer-1 call), at the offsets K0 fixed from the engine's
	// writers. The CPU reads the magic, context_length, and for 'SSB6' kv_mode, budget and origin.
	namespace PayloadHeader
	{
		constexpr int64 ContextLength = 60;   // LE64, 'SSB5' and 'SSB6'
		constexpr int64 Ssb5HeaderEnd = 124;  // through ready_for_logits (LE32 at 120)
		constexpr int64 KvMode = 156;         // LE32, 'SSB6' (0 whole_reserve, 1 budget)
		constexpr int64 Budget = 160;         // i32, 'SSB6'
		constexpr int64 Origin = 164;         // i64, 'SSB6'
		constexpr int64 Ssb6HeaderEnd = 172;

		uint64 ReadLE(const uint8* P, int32 Bytes)
		{
			uint64 Value = 0;
			for (int32 I = Bytes - 1; I >= 0; --I)
			{
				Value = (Value << 8) | uint64(P[I]);
			}
			return Value;
		}

		bool MagicIs(const uint8* Payload, int64 PayloadBytes, const char (&Magic)[5])
		{
			return PayloadBytes >= 4 && FMemory::Memcmp(Payload, Magic, 4) == 0;
		}
	}

	struct FRestoreHeader
	{
		SuperSLMPaging::ERestorePayload Payload = SuperSLMPaging::ERestorePayload::Budget;
		int64 Budget = 0;
		int64 Origin = 0;
		int64 ContextLength = 0;
	};

	// §3.5 "Malformed input": any of these is Malformed before any job, with nothing drawn. A header
	// the parser cannot read (shorter than its fixed fields); a magic other than 'SSB5' or 'SSB6';
	// an 'SSB6' kv_mode other than 0 or 1; a budget outside [1, cap], an origin or context_length
	// outside [0, cap]; or the wrapper's ContextUsed differing from the payload's context_length.
	// Cap is the model's (on the CPU the plugin's cap is the model's).
	bool ReadRestoreHeader(const uint8* Payload, int64 PayloadBytes, int64 Cap, int64 WrapperContextUsed, FRestoreHeader& Out, FString& OutError)
	{
		const bool bSsb6 = PayloadHeader::MagicIs(Payload, PayloadBytes, "SSB6");
		if (!bSsb6 && !PayloadHeader::MagicIs(Payload, PayloadBytes, "SSB5"))
		{
			OutError = TEXT("Malformed: the payload's magic is neither 'SSB5' nor 'SSB6' (or the payload is shorter than its magic)");
			return false;
		}
		const int64 HeaderEnd = bSsb6 ? PayloadHeader::Ssb6HeaderEnd : PayloadHeader::Ssb5HeaderEnd;
		if (PayloadBytes < HeaderEnd)
		{
			OutError = FString::Printf(TEXT("Malformed: the payload's %lld bytes end inside its %lld-byte header"), PayloadBytes, HeaderEnd);
			return false;
		}
		Out.ContextLength = static_cast<int64>(PayloadHeader::ReadLE(Payload + PayloadHeader::ContextLength, 8));
		if (Out.ContextLength < 0 || Out.ContextLength > Cap)
		{
			OutError = FString::Printf(TEXT("Malformed: the payload's context_length %lld is outside [0, %lld]"), Out.ContextLength, Cap);
			return false;
		}
		if (bSsb6)
		{
			const uint64 KvMode = PayloadHeader::ReadLE(Payload + PayloadHeader::KvMode, 4);
			Out.Budget = static_cast<int64>(static_cast<int32>(static_cast<uint32>(PayloadHeader::ReadLE(Payload + PayloadHeader::Budget, 4))));
			Out.Origin = static_cast<int64>(PayloadHeader::ReadLE(Payload + PayloadHeader::Origin, 8));
			if (KvMode > 1)
			{
				OutError = FString::Printf(TEXT("Malformed: the payload's kv_mode %llu is neither whole_reserve (0) nor budget (1)"), KvMode);
				return false;
			}
			if (Out.Budget < 1 || Out.Budget > Cap)
			{
				OutError = FString::Printf(TEXT("Malformed: the payload's budget %lld is outside [1, %lld]"), Out.Budget, Cap);
				return false;
			}
			if (Out.Origin < 0 || Out.Origin > Cap)
			{
				OutError = FString::Printf(TEXT("Malformed: the payload's origin %lld is outside [0, %lld]"), Out.Origin, Cap);
				return false;
			}
			// K3 review N5: the engine's own ordering, 0 <= origin <= context_length <= min(origin +
			// budget, cap), checked here at the call so a payload the engine must refuse never queues.
			if (Out.Origin > Out.ContextLength)
			{
				OutError = FString::Printf(TEXT("Malformed: the payload's origin %lld is above its context_length %lld"), Out.Origin, Out.ContextLength);
				return false;
			}
			const int64 Limit = FMath::Min<int64>(Out.Origin + Out.Budget, Cap);
			if (Out.ContextLength > Limit)
			{
				OutError = FString::Printf(TEXT("Malformed: the payload's context_length %lld is above its limit %lld, min(origin + budget, cap)"), Out.ContextLength, Limit);
				return false;
			}
			Out.Payload = KvMode == 1 ? SuperSLMPaging::ERestorePayload::Budget : SuperSLMPaging::ERestorePayload::WholeReserve;
		}
		else
		{
			Out.Payload = SuperSLMPaging::ERestorePayload::WholeReserve;
		}
		if (WrapperContextUsed != Out.ContextLength)
		{
			OutError = FString::Printf(TEXT("Malformed: the wrapper's ContextUsed %lld differs from the payload's context_length %lld"), WrapperContextUsed, Out.ContextLength);
			return false;
		}
		return true;
	}

	ESuperSLMRestoreResult QueueRestore(FSuperSLMSubsystemState* State, const TArray<uint8>& Blob, TArray<uint8>* MovableBlob,
		USuperSLMModel* ExpectedModel, const FSuperSLMPrefix* Hint, FSuperSLMSequence& OutSequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError)
	{
		OutSequence = FSuperSLMSequence();
		OutHandle = FSuperSLMLifecycleOpHandle();

		SuperSLMSaveBlob::FContents Contents;
		const uint8* Payload = nullptr;
		int64 PayloadBytes = 0;
		if (!SuperSLMSaveBlob::Read(Blob, Contents, Payload, PayloadBytes, OutError))
		{
			return ESuperSLMRestoreResult::Malformed;
		}
		if (Contents.Backend != ESuperSLMBackend::CPU)
		{
			OutError = TEXT("backend mismatch: the blob is tagged as saved by the GPU backend, and the CPU backend restores only CPU-tagged blobs");
			return ESuperSLMRestoreResult::BackendMismatch;
		}
		// Plan §2.5 row 23 (D-SLM7714): a blob saved under another Layer-1 pin is refused by name,
		// before anything is queued. Saves do not carry across a change of pin (§4, §12 decision 13).
		const FString CompiledTag = SuperSLMSaveBlob::CompiledLayer1Tag();
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.8: the shipped compatibility table admits a named predecessor's saves.
		if (!SuperSLMPrefixCacheGlue::AcceptSaveLayer1(Contents.Layer1Tag, Contents.Layer1Commit, Payload, PayloadBytes))
#else
		if (Contents.Layer1Tag != CompiledTag)
#endif
		{
			OutError = FString::Printf(
				TEXT("Layer-1 pin mismatch: the blob was saved by a build pinned to Layer 1 %s, and this build is pinned to Layer 1 %s; saves do not carry across a change of pin"),
				*Contents.Layer1Tag, *CompiledTag);
			return ESuperSLMRestoreResult::Layer1Mismatch;
		}
		if (State == nullptr)
		{
			OutError = TEXT("the subsystem is not configured");
			return ESuperSLMRestoreResult::NotConfigured;
		}
		if (State->bTickHorizonReached)
		{
			OutError = State->TickHorizonMessage; // D-SLM7799
			return ESuperSLMRestoreResult::NotConfigured;
		}
		// Plugin paging plan §3.5 (step K3): admission reads the payload header, before anything is
		// reserved or queued. The mode check reads the magic, never a wrapper field: an 'SSB6' (a
		// budget holder's save) into legacy mode is KvModeMismatch, since a budget holder in a legacy
		// pool breaks E4; legacy mode is otherwise unchanged. Budget mode refuses a malformed header
		// by name, and takes the charge's fields from it.
		FRestoreHeader Header;
		if (!State->IsBudgetMode())
		{
			if (PayloadHeader::MagicIs(Payload, PayloadBytes, "SSB6"))
			{
				OutError = TEXT("KvModeMismatch: the save is a budget-mode holder's ('SSB6'), and this subsystem is in legacy mode (SequenceBudget 0); configure SequenceBudget > 0 to restore it");
				return ESuperSLMRestoreResult::KvModeMismatch;
			}
		}
		else if (!ReadRestoreHeader(Payload, PayloadBytes, State->Ledger.GetConfig().ContextCap, Contents.ContextUsed, Header, OutError))
		{
			return ESuperSLMRestoreResult::Malformed;
		}
		if (ExpectedModel == nullptr)
		{
			OutError = TEXT("model mismatch: no expected model was given");
			return ESuperSLMRestoreResult::ModelMismatch;
		}
		FSuperSLMSubsystemState& S = *State;
		if (S.FreeSlots.Num() == 0)
		{
			OutError = FString::Printf(TEXT("every one of the %d warm sequences is vended; return one before restoring"), S.Slots.Num());
			return ESuperSLMRestoreResult::PoolExhausted;
		}
		uint8 ExpectedHash[32];
		if (!SuperSLMRuntime::ReadArtifactHash(*ExpectedModel, ExpectedHash))
		{
			OutError = FString::Printf(TEXT("model mismatch: expected model %s carries no artifact bytes"), *ExpectedModel->GetName());
			return ESuperSLMRestoreResult::ModelMismatch;
		}

		// Reserve a slot at once (a fresh vend, valid immediately, D-SLM7457: Restore always vends
		// fresh and cannot collide with a busy sequence's own queue). A drained free slot is
		// preferred; when none is drained the reservation falls back to FreeSlots[0], behind that
		// slot's queued recycle and any restore its previous holder queued, whose delivery leaves
		// this reservation standing (H10, kept as built: which physical slot a restore takes may depend
		// on worker pace, and no claim depends on the slot index, T-2991 §5.1 #7).
		int32 FreePos = 0;
		for (int32 I = 0; I < S.FreeSlots.Num(); ++I)
		{
			if (S.Slots[S.FreeSlots[I]].OpQueue.IsEmpty() && !S.Slots[S.FreeSlots[I]].HasUndeliveredJob())
			{
				FreePos = I;
				break;
			}
		}
		const int32 SlotIndex = S.FreeSlots[FreePos];
		S.FreeSlots.RemoveAt(FreePos, EAllowShrinking::No);
		FSlot& Slot = S.Slots[SlotIndex];
		// Rule 1: the reservation is a vend, so the per-user struct starts from a value reset.
		static_cast<FSlotUser&>(Slot) = FSlotUser();
		Slot.bAwaitingRestore = true;
		// D-SLM7528 (plan §5 item 3): a fresh vend resets PinnedLane, exactly like VendSequence(),
		// so the queued Restore op's own later DispatchRestore (via PostLedgerJob) resolves an
		// unpinned lane rather than inheriting this physical slot's prior occupant's pin.
		Slot.PinnedLane = INDEX_NONE;
		// Plan §2.5 row 4: Layer 1 clears bind eligibility on restore; SetSchema() refuses by name
		// until the holder resets the restored sequence.
		Slot.MarkBindIneligible(EBindIneligibleReason::Restored);
		Slot.VendedId = GNextSequenceId++;
		S.IdToSlot.Add(Slot.VendedId, SlotIndex);
		OutSequence.Id = Slot.VendedId;

		FSlotQueuedOp Op;
		Op.Kind = ESlotOpKind::Restore;
		Op.Ordinal = S.MintOrdinal();
		Op.HandleId = S.MintHandle();
		Op.Owner = Slot.VendedId;
		if (MovableBlob != nullptr)
		{
			Op.RestoreBlob = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(*MovableBlob));
		}
		else
		{
			Op.RestoreBlob = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(Blob);
		}
		FMemory::Memcpy(Op.RestoreExpectedHash, ExpectedHash, 32);
		Op.RestoreExpectedModel = ExpectedModel;
		// D-SLM7946: the phase this restore gives the sequence, as DispatchRestore()'s delivery
		// derives it from the same blob: a generating phase with no prompt left is Decoding, and one
		// with no tokens left to generate is Complete. The one-generation projection reads it.
		ESuperSLMSequencePhase ProjectedPhase = Contents.Phase;
		if (ProjectedPhase == ESuperSLMSequencePhase::Prefilling && Contents.PromptRemaining.Num() == 0)
		{
			ProjectedPhase = ESuperSLMSequencePhase::Decoding;
		}
		if ((ProjectedPhase == ESuperSLMSequencePhase::Prefilling || ProjectedPhase == ESuperSLMSequencePhase::Decoding) && Contents.MaxNewTokensRemaining == 0)
		{
			ProjectedPhase = ESuperSLMSequencePhase::Complete;
		}
		Op.RestoreProjectedPhase = ProjectedPhase;
		if (S.IsBudgetMode())
		{
			Op.RestorePayload = Header.Payload;
			Op.RestoreBudget = Header.Budget;
			Op.RestoreOrigin = Header.Origin;
			Op.RestoreContextLength = Header.ContextLength;
			// §3.5 "Restore hint" (UP-10): never a requirement. A hint that is not a built, live
			// prefix of this subsystem is ignored (the restore is private), and a whole-reserve
			// payload ignores its hint, as the engine does: it pins nothing and reaches the engine
			// as null. A usable hint is pinned from here until the job delivers or is refused.
			if (Hint != nullptr && Header.Payload == SuperSLMPaging::ERestorePayload::Budget)
			{
				FPrefixEntry* HintEntry = S.Prefixes.Find(Hint->Id);
#if SUPERSLM_WITH_PREFIX_CACHE
				// Precache plan §5.11 (b) *Adopt and hint* (rev 1.18, PC-15 Q5; the U2 draft's V9 ignored it): a Ready
				// cache lease is a hint. Its entry is adopt-pinned for the restore's life, the ledger pre-charges A for
				// its shared pages (EHintKind::CacheEntry), and the job shares its full pages. A lease standing in
				// State->Prefixes as an adopt shadow is the cache's too, never a ledger node. A lease that is not
				// Ready, or ended, is no hint: the restore is private (a hint is never a requirement).
				if (SuperSLMPrefixCacheGlue::PinCacheRestoreHint(S, Hint->Id, Op.RestoreHintCache) || SuperSLMPrefixCacheGlue::IsShadow(S, Hint->Id))
				{
					HintEntry = nullptr;
				}
#endif
				if (HintEntry != nullptr)
				{
					if (HintEntry->bLedgerBuilt && HintEntry->Handle != nullptr && HintEntry->Phase == ESuperSLMPrefixPhase::Ready)
					{
						++HintEntry->PendingHintedRestores;
						Op.RestoreHintPrefixId = Hint->Id;
					}
				}
			}
		}
		Slot.OpQueue.Add(MoveTemp(Op));
		Slot.CancelQueuedContinuation(); // 1.1: a queued op precedes the generation's next job
		OutHandle.Id = Slot.OpQueue.Last().HandleId;
		return ESuperSLMRestoreResult::Success;
	}
}

ESuperSLMRestoreResult USuperSLMSubsystem::RestoreSequence(
	const TArray<uint8>& Blob,
	USuperSLMModel* ExpectedModel,
	FSuperSLMSequence& OutSequence,
	FSuperSLMLifecycleOpHandle& OutHandle,
	FString& OutError)
{
	return QueueRestore(State, Blob, /*MovableBlob*/ nullptr, ExpectedModel, /*Hint*/ nullptr, OutSequence, OutHandle, OutError);
}

ESuperSLMRestoreResult USuperSLMSubsystem::RestoreSequence(
	TArray<uint8>&& Blob,
	USuperSLMModel* ExpectedModel,
	FSuperSLMSequence& OutSequence,
	FSuperSLMLifecycleOpHandle& OutHandle,
	FString& OutError)
{
	// Blob is validated in place and moved into the shared buffer only once accepted; a refused
	// request leaves the caller's array untouched.
	return QueueRestore(State, Blob, &Blob, ExpectedModel, /*Hint*/ nullptr, OutSequence, OutHandle, OutError);
}

// Plugin paging plan §3.5 (UP-10): the hinted forms. In budget mode the hint reaches
// sslm_seq_restore_shared, which shares the prefix's full pages when the engine's identity check
// passes; the hint is pinned while the restore is queued or waiting (QueueRestore()). A hint is
// never a requirement: legacy mode restores privately exactly as the unhinted forms do.
ESuperSLMRestoreResult USuperSLMSubsystem::RestoreSequence(
	const TArray<uint8>& Blob,
	USuperSLMModel* ExpectedModel,
	const FSuperSLMPrefix& Hint,
	FSuperSLMSequence& OutSequence,
	FSuperSLMLifecycleOpHandle& OutHandle,
	FString& OutError)
{
	return QueueRestore(State, Blob, /*MovableBlob*/ nullptr, ExpectedModel, &Hint, OutSequence, OutHandle, OutError);
}

ESuperSLMRestoreResult USuperSLMSubsystem::RestoreSequence(
	TArray<uint8>&& Blob,
	USuperSLMModel* ExpectedModel,
	const FSuperSLMPrefix& Hint,
	FSuperSLMSequence& OutSequence,
	FSuperSLMLifecycleOpHandle& OutHandle,
	FString& OutError)
{
	return QueueRestore(State, Blob, &Blob, ExpectedModel, &Hint, OutSequence, OutHandle, OutError);
}

ESuperSLMRestoreResult USuperSLMSubsystem::GetLifecycleOpResult(const FSuperSLMLifecycleOpHandle& Handle) const
{
	if (State != nullptr)
	{
		if (const FSuperSLMSubsystemState::FHandleResult* Entry = State->HandleResults.Find(Handle.Id))
		{
			return Entry->Result;
		}
	}
	// Plugin paging plan §3.5 (K3): a handle a budget-mode teardown left Pending. Its id is never
	// minted again while the tombstone stands (FirstHandleIdAfterTombstones()).
	if (LifecycleTombstones.Contains(Handle.Id))
	{
		return ESuperSLMRestoreResult::NotConfigured;
	}
	return ESuperSLMRestoreResult::Pending;
}

ESuperSLMRestoreResult USuperSLMSubsystem::GetSaveResult(const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob)
{
	OutBlob.Reset();
	// Plugin paging plan §3.5 (K3): a tombstone reads NotConfigured, with no blob.
	if (LifecycleTombstones.Contains(Handle.Id))
	{
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State != nullptr)
	{
		if (FSuperSLMSubsystemState::FHandleResult* Entry = State->HandleResults.Find(Handle.Id))
		{
			if (Entry->Result != ESuperSLMRestoreResult::Success)
			{
				return Entry->Result;
			}
			// Row 19 (D-SLM7763): the first successful read moves the blob out, with no copy; the
			// entry keeps only its result, and every later read says Consumed.
			if (Entry->bConsumed)
			{
				return ESuperSLMRestoreResult::Consumed;
			}
			State->RetainedResultBytes -= Entry->Blob.Num();
			OutBlob = MoveTemp(Entry->Blob);
			Entry->Blob.Empty();
			Entry->bConsumed = true;
			return ESuperSLMRestoreResult::Success;
		}
	}
	return ESuperSLMRestoreResult::Pending;
}

bool USuperSLMSubsystem::ReleaseLifecycleOpHandle(const FSuperSLMLifecycleOpHandle& Handle)
{
	// Plugin paging plan §3.5 (K3): releasing a tombstone erases it; the handle then reads Pending.
	if (LifecycleTombstones.Remove(Handle.Id) > 0)
	{
		return true;
	}
	if (State == nullptr)
	{
		return false;
	}
	FSuperSLMSubsystemState::FHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	if (Entry == nullptr || Entry->Result == ESuperSLMRestoreResult::Pending)
	{
		return false; // unknown, already released, or still queued: nothing is erased
	}
	State->RetainedResultBytes -= Entry->Blob.Num();
	State->HandleResults.Remove(Handle.Id);
	return true;
}

int64 USuperSLMSubsystem::GetRetainedResultBytes() const
{
	return State != nullptr ? State->RetainedResultBytes : 0;
}

int32 USuperSLMSubsystem::GetLifecycleHandleEntryCount() const
{
	// Plugin paging plan §3.5 (K3): tombstones count, since the game still owes their release.
	return (State != nullptr ? State->HandleResults.Num() : 0) + LifecycleTombstones.Num();
}
#endif // SUPERSLM_WITH_L2S1_ASYNC

// Plugin paging plan §3.5 "Handle ids stay unique" (K3): the handle counter lives in the
// per-Configure state and restarts with it, so a new state starts above the highest tombstoned id,
// and a fresh operation can never take a tombstone's id. With no tombstones it starts at 1.
int64 USuperSLMSubsystem::FirstHandleIdAfterTombstones() const
{
	int64 Highest = 0;
	for (const int64 Id : LifecycleTombstones)
	{
		Highest = FMath::Max(Highest, Id);
	}
	return Highest + 1;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

double USuperSLMSubsystem::GetLastTickDurationMs() const { return State ? State->LastReport.DurationMs : 0.0; }
double USuperSLMSubsystem::GetLastResetMs() const { return State ? State->LastResetMs : 0.0; }
double USuperSLMSubsystem::GetLastAdoptMs() const { return State ? State->LastAdoptMs : 0.0; }

#if SUPERSLM_WITH_L2S1_ASYNC
int32 USuperSLMSubsystem::GetPendingLifecycleOperationCount(const FSuperSLMSequence& Sequence) const
{
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		return Slot->EffectiveQueueDepth();
	}
	return 0;
}

TArray<FSuperSLMWorkerJobReport> USuperSLMSubsystem::GetJobLedger() const
{
	return State ? State->JobLedger.ToArray() : TArray<FSuperSLMWorkerJobReport>();
}

int64 USuperSLMSubsystem::GetJobLedgerAppendedCount() const
{
	return State ? State->JobLedger.AppendedCount() : 0;
}

const FSuperSLMWorkerJobReport* USuperSLMSubsystem::FindJobLedgerRow(int64 Row) const
{
	return State ? State->JobLedger.Find(Row) : nullptr;
}

TArray<FSuperSLMTickReport> USuperSLMSubsystem::GetTickHistory() const
{
	return State ? State->TickHistory.ToArray() : TArray<FSuperSLMTickReport>();
}

int64 USuperSLMSubsystem::GetTickHistoryAppendedCount() const
{
	return State ? State->TickHistory.AppendedCount() : 0;
}

const FSuperSLMTickReport* USuperSLMSubsystem::FindTickHistoryRow(int64 Row) const
{
	return State ? State->TickHistory.Find(Row) : nullptr;
}
#endif // SUPERSLM_WITH_L2S1_ASYNC

double USuperSLMSubsystem::GetLastPrefillMsPerToken() const { return State ? State->LastPrefillMsPerToken : 0.0; }
double USuperSLMSubsystem::GetLastFinishMs() const { return State ? State->LastFinishMs : 0.0; }
double USuperSLMSubsystem::GetTokensPerSecond() const { return State ? State->LastTokensPerSecond : 0.0; }
int32 USuperSLMSubsystem::GetHitchCount() const { return State ? State->HitchCount : 0; }
int32 USuperSLMSubsystem::GetPoolFreeCount() const { return State ? State->FreeSlots.Num() : 0; }
int32 USuperSLMSubsystem::GetPoolOccupiedCount() const { return State ? State->IdToSlot.Num() : 0; }
int32 USuperSLMSubsystem::GetLivePrefixCount() const
{
	if (State == nullptr)
	{
		return 0;
	}
#if SUPERSLM_WITH_PREFIX_CACHE
	return State->Prefixes.Num() - SuperSLMPrefixCacheGlue::ShadowCount(*State); // cache leases are not live prefixes
#else
	return State->Prefixes.Num();
#endif
}
int64 USuperSLMSubsystem::GetKvPoolReservedBytes() const { return State ? State->KvBytes : 0; }

// Plugin paging plan §3.2: the ledger's regions and counters and the last applied job's pool
// snapshot, all game-thread data. LedgerFaults is this subsystem's own count, kept across
// re-Configure() (PriorLedgerFaults holds the ended states' counts).
FSuperSLMKvPageStats USuperSLMSubsystem::GetKvPageStats() const
{
	FSuperSLMKvPageStats Stats;
	Stats.LedgerFaults = PriorLedgerFaults + (State != nullptr ? static_cast<uint64>(State->Ledger.LedgerFaults()) : 0);
	if (State == nullptr || !State->IsBudgetMode())
	{
		return Stats;
	}
	const FSuperSLMSubsystemState& S = *State;
	const SuperSLMPaging::FPageLedgerStats L = S.Ledger.GetStats();
	auto Region = [&L](SuperSLMPaging::ERegion R)
	{
		FSuperSLMKvRegionStats Out;
		Out.CapacityPages = L.Regions[static_cast<int32>(R)].Capacity;
		Out.ChargedPages = L.Regions[static_cast<int32>(R)].Charged;
		return Out;
	};
	Stats.bPagedMode = true;
	Stats.PagePositions = S.PagePositions;
	Stats.PageBytes = static_cast<int64>(S.PageBytes);
	Stats.PoolPages = L.PoolPages;
	Stats.Sequences = Region(SuperSLMPaging::ERegion::S);
	Stats.Prefixes = Region(SuperSLMPaging::ERegion::P);
	Stats.RestoreHeadroom = Region(SuperSLMPaging::ERegion::H);
	Stats.CacheSharing = Region(SuperSLMPaging::ERegion::A);
	Stats.Cache = Region(SuperSLMPaging::ERegion::C);
	Stats.LivePrefixCount = static_cast<int32>(L.LivePrefixCount);
	Stats.GhostCount = static_cast<int32>(L.GhostCount);
	Stats.InFlightRestores = static_cast<int32>(L.InFlightRestores);
	Stats.WaitingRestores = static_cast<int32>(L.WaitingRestores);
	Stats.FreeHeadroomPages = L.FreeHeadroomPages;
	Stats.bHasPoolSnapshot = S.bHasPoolSnapshot;
	if (S.bHasPoolSnapshot)
	{
		Stats.SnapshotPageCount = S.LastPoolStats.page_count;
		Stats.SnapshotFreePages = S.LastPoolStats.free_pages;
		Stats.SnapshotSharedPages = S.LastPoolStats.shared_pages;
	}
	return Stats;
}
int64 USuperSLMSubsystem::GetWorkspaceReservedBytes() const { return State ? State->WorkspaceBytesEach * State->Lanes.Num() : 0; }

double USuperSLMSubsystem::GetTimeToFirstTokenMs(const FSuperSLMSequence& Sequence) const
{
	if (const FSlot* Slot = State ? State->Find(Sequence.Id) : nullptr)
	{
		return Slot->TimeToFirstTokenMs;
	}
	return -1.0;
}

const FSuperSLMTickReport& USuperSLMSubsystem::GetLastTickReport() const
{
	static const FSuperSLMTickReport Empty;
	return State ? State->LastReport : Empty;
}

#if WITH_DEV_AUTOMATION_TESTS
// Plan §10.3.1 item 5a.3 (D-SLM7799): the scheduling seam (SuperSLMSchedulingTestAccess.h). The
// guards are called through the global scope so they are the production functions, not copies.
int32 FSuperSLMSchedulingTestAccess::MaxJobTicks() { return kMaxJobTicks; }
int32 FSuperSLMSchedulingTestAccess::TickHorizon() { return kTickHorizon; }

bool FSuperSLMSchedulingTestAccess::TryComputeK(double PlannedJobMs, double TickBudgetMs, int32& OutK)
{
	return ::TryComputeK(PlannedJobMs, TickBudgetMs, OutK);
}

// Both seams price Config as a configuration with no model would resolve it (decode-threading D4):
// its set cost fields, and the effective mode's one-time profile for the rest.
int32 FSuperSLMSchedulingTestAccess::PlanPrefillChunk(int32 MaxChunk, int64 StartDepth, double ShareMs, const FSuperSLMRuntimeConfig& Config)
{
	return ::PlanPrefillChunk(MaxChunk, StartDepth, ShareMs, Config.MinPrefillChunk, SuperSLMRuntimeResolve::Resolve(Config, nullptr));
}

double FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(int32 Chunk, int64 StartDepth, const FSuperSLMRuntimeConfig& Config)
{
	return ::PrefillChunkCostMs(Chunk, StartDepth, SuperSLMRuntimeResolve::Resolve(Config, nullptr));
}

bool FSuperSLMSchedulingTestAccess::IsWorkerOverrun(double WorkerCallMs, int32 K, double TickBudgetMs)
{
	return SuperSLMPrefillPlanning::IsWorkerOverrun(WorkerCallMs, K, TickBudgetMs);
}

int32 FSuperSLMSchedulingTestAccess::TiledMinTokens()
{
	return SuperSLMPrefillPlanning::kTiledMinTokens;
}

bool FSuperSLMSchedulingTestAccess::TryCommitTick(int32 TickIndex, int32 K, int32& OutTick)
{
	return ::TryCommitTick(TickIndex, K, OutTick);
}

bool FSuperSLMSchedulingTestAccess::SetTickCounter(USuperSLMSubsystem& Subsystem, int32 TickCounter)
{
	if (Subsystem.State == nullptr)
	{
		return false;
	}
	Subsystem.State->TickCounter = TickCounter;
	return true;
}

int32 FSuperSLMSchedulingTestAccess::GetTickCounter(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State ? Subsystem.State->TickCounter : -1;
}

void FSuperSLMSchedulingTestAccess::SetReportHistoryCapacity(int32 Rows)
{
	GReportHistoryRowsOverride = FMath::Max(Rows, 0);
}

void FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride(USuperSLMSubsystem& Subsystem, sslm_status Status)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->TestNextResetStatus = Status;
	}
}

int32 FSuperSLMSchedulingTestAccess::GetReportHistoryCapacity()
{
	return ReportHistoryRows();
}
#endif // WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------------
// The schema jump-forward test seam (schema fast-decode plan rev 1.3.5, §6 P1 and P1-X;
// SuperSLMSchemaJumpTestSeam.h). Defined here, beside the state it reads, as the scheduling and
// paged seams are. Game thread only. It records and seeds; it never changes a token.
// ---------------------------------------------------------------------------------------------

// Schema fast-decode plan §6 P1, P1-X (P1.9; rev 1.3.6, review finding 3): the example's scratch
// switch, parsed by the core parser, in the public API so the example can call it. Text outside ASCII
// is refused before conversion (no value the switch takes has any).
bool ParseSuperSLMSchemaJumpForwardSwitch(const FString& Text, ESuperSLMSchemaJumpForward& InOutMode)
{
	for (const TCHAR Ch : Text)
	{
		if (Ch == 0 || static_cast<uint32>(Ch) > 127u)
		{
			return false;
		}
	}
	const FTCHARToUTF8 Utf8(*Text);
	SuperSLMCore::ESchemaJumpMode Mode = SuperSLMCore::ESchemaJumpMode::Off;
	if (!SuperSLMCore::ParseSchemaJumpSwitch(Utf8.Get(), Mode))
	{
		return false;
	}
	InOutMode = ToPublicJumpMode(Mode);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// The schema's SCM1 index under the configured mapping: false when not configured, when the
	// handle is unknown or was looked up on another artifact, or when the section lacks it.
	bool SeamSchemaIndex(const FSuperSLMSubsystemState* S, const FSuperSLMSchemaHandle& Schema, int32& OutIndex)
	{
		if (S == nullptr || Schema.IsNone())
		{
			return false;
		}
		uint8 Hash[32];
		FString SchemaName;
		if (!SuperSLMRuntime::ResolveSchemaName(Schema.Opaque, Hash, SchemaName) || FMemory::Memcmp(Hash, S->ArtifactHash, 32) != 0)
		{
			return false;
		}
		return S->FindSchemaIndex(SchemaName, OutIndex);
	}

	SuperSLMCore::FDraftTableRef SeamSnapshot(const FSuperSLMSubsystemState* S, const FSuperSLMSchemaHandle& Schema)
	{
		int32 Index = INDEX_NONE;
		return SeamSchemaIndex(S, Schema, Index) ? S->DraftTables.Snapshot(S->DraftKey(Index)) : SuperSLMCore::FDraftTableRef();
	}

	// Requests the walk table if no generation has, and waits for its pool task, then publishes it.
	// False when the load is held (HoldSchemaWalkLoad) or does not finish within a minute.
	bool SeamEnsureWalkTable(FSuperSLMSubsystemState& S)
	{
		RequestSchemaWalkLoad(S);
		if (S.bWalkLoadPending)
		{
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		while (S.WalkLoadInFlight.IsValid() && !S.WalkLoadInFlight->bDone.load(std::memory_order_acquire))
		{
			if (FPlatformTime::Seconds() - Start > 60.0)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		PublishSchemaWalkLoad(S, S.CurrentTickIndex);
		return true;
	}

	// The tokens SCM1 admits at State, ascending.
	TArray<int32> LegalTokensAt(const superslm::SchemaEntry& Entry, uint32 State)
	{
		TArray<int32> Out;
		if (State >= Entry.state_count)
		{
			return Out;
		}
		using superslm::schema_masks_detail::ReadLE32;
		const uint32 Begin = ReadLE32(Entry.state_offsets_le + static_cast<size_t>(State) * 4);
		const uint32 End = ReadLE32(Entry.state_offsets_le + static_cast<size_t>(State + 1) * 4);
		for (uint32 K = Begin; K < End; ++K)
		{
			Out.Add(static_cast<int32>(ReadLE32(Entry.transitions_le + static_cast<size_t>(K) * 8)));
		}
		return Out;
	}
}

bool FSuperSLMSchemaJumpTestAccess::IsBuilt()
{
	return true;
}

void FSuperSLMSchemaJumpTestAccess::StartRecording(USuperSLMSubsystem& Subsystem)
{
	if (FSuperSLMSubsystemState* S = Subsystem.State)
	{
		S->bRecordSpanJobs = true;
		S->SpanJobRecords.Reset();
		S->ForcednessReadCount = 0;
		S->ForcednessReadRecords.Reset();
	}
}

TArray<FSuperSLMSpanJobRecord> FSuperSLMSchemaJumpTestAccess::GetSpanJobs(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->SpanJobRecords : TArray<FSuperSLMSpanJobRecord>();
}

bool FSuperSLMSchemaJumpTestAccess::HasDraftTable(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	return static_cast<bool>(SeamSnapshot(Subsystem.State, Schema));
}

int32 FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	const SuperSLMCore::FDraftTableRef Table = SeamSnapshot(Subsystem.State, Schema);
	return Table ? static_cast<int32>(Table->StateCount()) : 0;
}

int32 FSuperSLMSchemaJumpTestAccess::GetDraftTableMaxTokensAtAnyState(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	const SuperSLMCore::FDraftTableRef Table = SeamSnapshot(Subsystem.State, Schema);
	int32 Max = 0;
	if (Table)
	{
		for (const std::pair<const uint32_t, SuperSLMCore::FDraftStateRow>& Row : Table->Rows)
		{
			Max = FMath::Max(Max, static_cast<int32>(Table->TokenCountAt(Row.first)));
		}
	}
	return Max;
}

// The bound P1X.11 checks the table against (seam header, rev 1.3.5): the states the subsystem has
// observed forced so far -- read with out_count > 0, or drafted through inside a confirmed draft.
// Every state the table holds was observed forced (IsEveryDraftTableStateObservedForced), so the
// table's state count never exceeds it. Zero before the first forced read.
int32 FSuperSLMSchemaJumpTestAccess::GetSchemaForcedStateCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	const FSuperSLMSubsystemState* S = Subsystem.State;
	int32 Index = INDEX_NONE;
	if (!SeamSchemaIndex(S, Schema, Index))
	{
		return 0;
	}
	const SuperSLMCore::FForcednessObservations* Observed = S->ForcedObservations.Find(Index);
	if (Observed == nullptr)
	{
		return 0;
	}
	int32 Count = 0;
	for (const auto& Flag : Observed->Flags)
	{
		if (Flag.second == SuperSLMCore::EForcedObservation::Forced)
		{
			++Count;
		}
	}
	return Count;
}

bool FSuperSLMSchemaJumpTestAccess::IsEveryDraftTableStateObservedForced(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	const FSuperSLMSubsystemState* S = Subsystem.State;
	int32 Index = INDEX_NONE;
	if (!SeamSchemaIndex(S, Schema, Index))
	{
		return false;
	}
	const SuperSLMCore::FDraftTableRef Table = S->DraftTables.Snapshot(S->DraftKey(Index));
	const SuperSLMCore::FForcednessObservations* Observed = S->ForcedObservations.Find(Index);
	if (!Table)
	{
		return false;
	}
	for (const std::pair<const uint32_t, SuperSLMCore::FDraftStateRow>& Row : Table->Rows)
	{
		if (Observed == nullptr || Observed->Get(Row.first) != SuperSLMCore::EForcedObservation::Forced)
		{
			return false;
		}
	}
	return true;
}

int32 FSuperSLMSchemaJumpTestAccess::MaxTokensPerState()
{
	return static_cast<int32>(SuperSLMCore::FDraftTable::MaxTokensPerState());
}

int64 FSuperSLMSchemaJumpTestAccess::GetDraftSnapshotVersion(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	int32 Index = INDEX_NONE;
	return SeamSchemaIndex(Subsystem.State, Schema, Index) ? Subsystem.State->DraftSnapshotVersion(Index) : -1;
}

// The shipped-table path (§16 Q20) from whole streams walked from the schema's start state: a
// token counts only at a state carrying the library's forced flag as the subsystem observed it
// (rev 1.3.5: a seed carries the flags and only its forced states are loaded). The streams a cell
// seeds from are off runs on this subsystem, whose every state was read.
bool FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema,
	const TArray<TArray<int32>>& Streams)
{
	FSuperSLMSubsystemState* S = Subsystem.State;
	// Rev 1.3.6 (review finding 1): the walk table loads lazily, so a seed made before any jump-mode
	// generation asks for it and waits for the pool task here (test seam, game thread). A held load
	// is not waited for.
	if (S != nullptr && !SeamEnsureWalkTable(*S))
	{
		return false;
	}
	int32 Index = INDEX_NONE;
	if (!SeamSchemaIndex(S, Schema, Index))
	{
		return false;
	}
	const superslm::SchemaEntry* Entry = S->WalkSchemas().ByIndex(static_cast<size_t>(Index));
	if (Entry == nullptr)
	{
		return false;
	}
	const FScm1Automaton Transitions(S->WalkSchemas(), *Entry, 0);
	const SuperSLMCore::FObservedSchemaAutomaton Automaton(Transitions, S->ForcedObservations.FindOrAdd(Index));
	SuperSLMCore::FDraftTableRef Table = SuperSLMCore::MergeFinishedStream(nullptr, Automaton, nullptr, 0);
	for (const TArray<int32>& Stream : Streams)
	{
		Table = SuperSLMCore::MergeFinishedStream(Table.get(), Automaton, Stream.GetData(), static_cast<size_t>(Stream.Num()));
	}
	S->SwapDraftTable(Index, Table);
	return true;
}

// At every held state where SCM1 admits two or more tokens, the lowest legal token other than the
// row's leader gets a count above the row's whole total, so it leads with a share above 1/2 and a
// draft through the state takes it: legal there, and not the model's token, so the verb rejects it.
// A full row gives up its weakest non-leader token for it (the cap holds).
bool FSuperSLMSchemaJumpTestAccess::CorruptDraftTable(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	FSuperSLMSubsystemState* S = Subsystem.State;
	int32 Index = INDEX_NONE;
	if (!SeamSchemaIndex(S, Schema, Index))
	{
		return false;
	}
	const SuperSLMCore::FDraftTableRef Table = S->DraftTables.Snapshot(S->DraftKey(Index));
	const superslm::SchemaEntry* Entry = S->WalkSchemas().ByIndex(static_cast<size_t>(Index));
	if (!Table || Entry == nullptr)
	{
		return false;
	}
	std::shared_ptr<SuperSLMCore::FDraftTable> Corrupt = std::make_shared<SuperSLMCore::FDraftTable>(*Table);
	for (std::pair<const uint32_t, SuperSLMCore::FDraftStateRow>& Row : Corrupt->Rows)
	{
		const TArray<int32> Legal = LegalTokensAt(*Entry, Row.first);
		SuperSLMCore::FDraftStateRow& R = Row.second;
		if (Legal.Num() < 2 || R.Tokens.empty())
		{
			continue;
		}
		// The leader as the draft reads it: the highest guaranteed count (count less error, the
		// Space-Saving cap's lower bound, rev 1.3.6), ties to the lowest id.
		auto Guaranteed = [](const SuperSLMCore::FDraftKeptToken& T) { return T.Count - T.Error; };
		size_t Leader = 0;
		for (size_t K = 1; K < R.Tokens.size(); ++K)
		{
			if (Guaranteed(R.Tokens[K]) > Guaranteed(R.Tokens[Leader])
				|| (Guaranteed(R.Tokens[K]) == Guaranteed(R.Tokens[Leader]) && R.Tokens[K].Token < R.Tokens[Leader].Token))
			{
				Leader = K;
			}
		}
		const int32 LeaderToken = R.Tokens[Leader].Token;
		int32 Wrong = INDEX_NONE;
		for (const int32 Token : Legal)
		{
			if (Token != LeaderToken)
			{
				Wrong = Token;
				break;
			}
		}
		if (Wrong == INDEX_NONE)
		{
			continue;
		}
		const uint64_t Dominant = R.Total + 1;
		bool bKept = false;
		for (SuperSLMCore::FDraftKeptToken& Kept : R.Tokens)
		{
			if (Kept.Token == Wrong)
			{
				R.Total = R.Total - Kept.Count + Dominant;
				Kept.Count = Dominant;
				Kept.Error = 0;
				bKept = true;
				break;
			}
		}
		if (!bKept)
		{
			if (R.Tokens.size() >= SuperSLMCore::FDraftTable::MaxTokensPerState())
			{
				size_t Weakest = Leader == 0 ? 1 : 0;
				for (size_t K = 0; K < R.Tokens.size(); ++K)
				{
					if (K != Leader && Guaranteed(R.Tokens[K]) < Guaranteed(R.Tokens[Weakest]))
					{
						Weakest = K;
					}
				}
				R.Tokens.erase(R.Tokens.begin() + static_cast<std::ptrdiff_t>(Weakest));
			}
			R.Total += Dominant; // a dropped token's count stays in the total, as a cap eviction's does
			R.Tokens.push_back(SuperSLMCore::FDraftKeptToken{Wrong, Dominant, 0});
		}
	}
	S->SwapDraftTable(Index, std::move(Corrupt));
	return true;
}

void FSuperSLMSchemaJumpTestAccess::ClearDraftTable(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema)
{
	int32 Index = INDEX_NONE;
	if (SeamSchemaIndex(Subsystem.State, Schema, Index))
	{
		Subsystem.State->SwapDraftTable(Index, nullptr);
	}
}

int64 FSuperSLMSchemaJumpTestAccess::GetForcednessReadCount(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->ForcednessReadCount : 0;
}

void FSuperSLMSchemaJumpTestAccess::SetForcednessReadsEnabled(USuperSLMSubsystem& Subsystem, bool bEnabled)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->bForcednessReadsEnabled = bEnabled;
	}
}

void FSuperSLMSchemaJumpTestAccess::SetNextSpanStatusOverride(USuperSLMSubsystem& Subsystem, int32 Status)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->TestNextSpanStatus = Status == static_cast<int32>(SSLM_OK) ? 0 : Status;
	}
}

void FSuperSLMSchemaJumpTestAccess::SetJobHold(USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence, ESuperSLMJobHoldPoint Point)
{
	if (Subsystem.State == nullptr || Sequence.Id == 0)
	{
		return;
	}
	if (Point == ESuperSLMJobHoldPoint::None)
	{
		Subsystem.State->TestJobHolds.Remove(Sequence.Id);
		return;
	}
	FSuperSLMSubsystemState::FTestJobHold& Hold = Subsystem.State->TestJobHolds.FindOrAdd(Sequence.Id);
	Hold.Point = Point;
	Hold.bHeld = false;
}

bool FSuperSLMSchemaJumpTestAccess::IsJobHeld(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	const FSuperSLMSubsystemState::FTestJobHold* Hold = Subsystem.State != nullptr ? Subsystem.State->TestJobHolds.Find(Sequence.Id) : nullptr;
	return Hold != nullptr && Hold->bHeld;
}

void FSuperSLMSchemaJumpTestAccess::ReleaseJobHold(USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	if (FSuperSLMSubsystemState::FTestJobHold* Hold = Subsystem.State != nullptr ? Subsystem.State->TestJobHolds.Find(Sequence.Id) : nullptr)
	{
		Hold->bHeld = false;
	}
}

// ---- FSuperSLMSchemaJumpReviewTestAccess (rev 1.3.6, §17.12; SuperSLMSchemaJumpReviewTestAccess.h) ----

bool FSuperSLMSchemaJumpReviewTestAccess::IsBuilt()
{
	return true;
}

TArray<FSuperSLMSchemaWalkLoadRecord> FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->WalkLoadRecords : TArray<FSuperSLMSchemaWalkLoadRecord>();
}

bool FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr && Subsystem.State->WalkTable.IsValid();
}

void FSuperSLMSchemaJumpReviewTestAccess::HoldSchemaWalkLoad(USuperSLMSubsystem& Subsystem, bool bHold)
{
	FSuperSLMSubsystemState* S = Subsystem.State;
	if (S == nullptr)
	{
		return;
	}
	S->bHoldWalkLoad = bHold;
	if (!bHold && S->bWalkLoadPending)
	{
		S->bWalkLoadPending = false;
		DispatchSchemaWalkLoad(*S); // the held load goes now; a Tick publishes it
	}
}

bool FSuperSLMSchemaJumpReviewTestAccess::LoadSchemaWalkTableNow(USuperSLMSubsystem& Subsystem)
{
	FSuperSLMSubsystemState* S = Subsystem.State;
	return S != nullptr && SeamEnsureWalkTable(*S) && S->WalkTable.IsValid();
}

TArray<FSuperSLMForcednessReadRecord> FSuperSLMSchemaJumpReviewTestAccess::GetForcednessReads(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->ForcednessReadRecords : TArray<FSuperSLMForcednessReadRecord>();
}

int32 FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->CanonicalOpenChainDecodes : -1;
}

bool FSuperSLMSchemaJumpReviewTestAccess::IsWalkKnown(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	const FSuperSLMSubsystemState* S = Subsystem.State;
	if (S == nullptr || Sequence.Id == 0)
	{
		return false;
	}
	for (const FSlot& Slot : S->Slots)
	{
		if (Slot.VendedId == Sequence.Id)
		{
			return Slot.Jump.bWalkKnown;
		}
	}
	return false;
}

int64 FSuperSLMSchemaJumpReviewTestAccess::GetContextUsed(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	const FSuperSLMSubsystemState* S = Subsystem.State;
	if (S == nullptr || Sequence.Id == 0)
	{
		return -1;
	}
	for (const FSlot& Slot : S->Slots)
	{
		if (Slot.VendedId == Sequence.Id)
		{
			return Slot.ContextUsed;
		}
	}
	return -1;
}

int64 FSuperSLMSchemaJumpReviewTestAccess::GetContextCap(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->Shape.ContextCap : -1;
}
#endif // WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------------
// The paged-KV test seams (plugin paging plan §3.9, SuperSLMPagedTestSeam.h)
// ---------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// The seam's request kinds onto this file's job kinds, one to one.
	EPagedJobKind ToPagedJobKind(ESuperSLMPagedRequestKind Kind)
	{
		switch (Kind)
		{
		case ESuperSLMPagedRequestKind::Reset:         return EPagedJobKind::Reset;
		case ESuperSLMPagedRequestKind::Return:        return EPagedJobKind::Return;
		case ESuperSLMPagedRequestKind::Adopt:         return EPagedJobKind::Adopt;
		case ESuperSLMPagedRequestKind::Restore:       return EPagedJobKind::Restore;
		case ESuperSLMPagedRequestKind::PrefixBuild:   return EPagedJobKind::PrefixBuild;
		case ESuperSLMPagedRequestKind::NestedBuild:   return EPagedJobKind::NestedBuild;
		case ESuperSLMPagedRequestKind::PrefixRelease: return EPagedJobKind::PrefixRelease;
		default:                                       break;
		}
		return EPagedJobKind::Count;
	}

	// RunOnPoolThread()'s access object: the engine verbs on the subsystem's own model and pool,
	// called on the lane thread the callback runs on.
	class FCpuPoolThreadAccess final : public FSuperSLMPoolThreadAccess
	{
	public:
		FCpuPoolThreadAccess(sslm_model InModel, sslm_kv_pool* InPool) : Model(InModel), Pool(InPool) {}
		virtual ~FCpuPoolThreadAccess() override { ReleaseAll(); }

		virtual int64 PagePositions() const override { return sslm_kv_page_positions(Model); }
		virtual int64 PageBytes() const override { return static_cast<int64>(sslm_kv_page_size(Model)); }
		virtual int64 PagesForBudget(int32 Budget) const override { return static_cast<int64>(sslm_kv_pages_for_budget(Model, Budget)); }

		virtual sslm_status CreateBudgeted(int32 Budget) override
		{
			sslm_seq Seq = nullptr;
			const sslm_status Status = sslm_seq_create_budgeted(Model, Pool, Budget, &Seq);
			if (Status == SSLM_OK && Seq != nullptr)
			{
				Held.Add(Seq);
			}
			return Status;
		}
		virtual sslm_status CreateWholeReserve() override
		{
			sslm_seq Seq = nullptr;
			const sslm_status Status = sslm_seq_create(Model, Pool, &Seq);
			if (Status == SSLM_OK && Seq != nullptr)
			{
				Held.Add(Seq);
			}
			return Status;
		}
		virtual int32 HeldCount() const override { return Held.Num(); }
		virtual void ReleaseAll() override
		{
			for (int32 I = Held.Num() - 1; I >= 0; --I)
			{
				sslm_seq_release(Held[I]);
			}
			Held.Reset();
		}

	private:
		sslm_model Model = nullptr;
		sslm_kv_pool* Pool = nullptr;
		TArray<sslm_seq> Held;
	};

	int32 SlotIndexOfState(const FSuperSLMSubsystemState* State, const FSuperSLMSequence& Sequence)
	{
		if (State == nullptr)
		{
			return INDEX_NONE;
		}
		const int32* Index = State->IdToSlot.Find(Sequence.Id);
		return Index != nullptr ? *Index : INDEX_NONE;
	}

	const SuperSLMPaging::FNodeId* NodeOf(const FSuperSLMSubsystemState* State, const FSuperSLMPrefix& Prefix)
	{
		return (State != nullptr && State->IsBudgetMode()) ? State->PrefixIdToNode.Find(Prefix.Id) : nullptr;
	}
}

bool FSuperSLMPagedTestSeam::RunOnPoolThread(USuperSLMSubsystem& Subsystem, TFunction<void(FSuperSLMPoolThreadAccess&)> Fn)
{
	FSuperSLMSubsystemState* State = Subsystem.State;
	if (State == nullptr || State->Lanes.Num() == 0 || State->Lanes[0].Runnable == nullptr || !Fn)
	{
		return false;
	}
	// Quiesce: every posted job's Layer-1 work is finished (a HoldStart() job was never posted; a
	// HoldApply() job has finished its work and waits only for its apply). Nothing is delivered
	// here, so the job ledger and every result stay as they were.
	// Precache plan §8 (rev 1.18): a cache build stopped at the build hold has drawn its reserve and makes no Layer-1
	// call, so the barrier counts its lane as parked, as an idle one. The barrier opens when every unfinished job is one
	// held there (every held build is unfinished, so equal counts mean the same set); a build past the hold and not yet
	// frozen keeps it shut. Fn then runs on the first lane with nothing unfinished and no build at the hold.
	int32 ProbeLane = INDEX_NONE;
	for (;;)
	{
		int32 Unfinished = 0;
		int32 FirstIdle = INDEX_NONE;
		for (int32 L = 0; L < State->Lanes.Num(); ++L)
		{
			const FLane& Lane = State->Lanes[L];
			int32 LaneUnfinished = 0;
			for (const FLanePendingDelivery& Pending : Lane.Pending)
			{
				LaneUnfinished += (Pending.WorkerJob.IsValid() && Pending.WorkerJob->bDone.load(std::memory_order_acquire)) ? 0 : 1;
			}
			Unfinished += LaneUnfinished;
			bool bLaneHeld = false;
#if WITH_DEV_AUTOMATION_TESTS
			bLaneHeld = Lane.Runnable != nullptr && Lane.Runnable->TestBuildHolds.load(std::memory_order_acquire) > 0;
#endif
			if (FirstIdle == INDEX_NONE && LaneUnfinished == 0 && !bLaneHeld && Lane.Runnable != nullptr)
			{
				FirstIdle = L;
			}
		}
		int32 HeldAtBuildHold = 0;
#if SUPERSLM_WITH_PREFIX_CACHE && WITH_DEV_AUTOMATION_TESTS
		HeldAtBuildHold = SuperSLMPrefixCacheGlue::HeldBuildsForBarrier(*State);
#endif
		if (Unfinished == HeldAtBuildHold)
		{
			if (FirstIdle == INDEX_NONE)
			{
				return false; // every lane holds a build at the hold: no lane can run Fn (the cell needs K + 1 lanes)
			}
			ProbeLane = FirstIdle;
			break;
		}
		FPlatformProcess::Sleep(0.0005f);
	}
	// That lane's queue is empty now, so this job runs next on its thread. It is enqueued directly,
	// not posted: it has no ledger row and no delivery.
	const sslm_model Model = State->Model;
	sslm_kv_pool* Pool = &State->Pool;
	FQueuedWorkerJobRef Job = MakeShared<FQueuedWorkerJob, ESPMode::ThreadSafe>();
	Job->Execute = [Model, Pool, &Fn]()
	{
		FCpuPoolThreadAccess Access(Model, Pool);
		Fn(Access);
		Access.ReleaseAll();
	};
	State->Lanes[ProbeLane].Runnable->Enqueue(Job);
	while (!Job->bDone.load(std::memory_order_acquire))
	{
		FPlatformProcess::Sleep(0.0005f);
	}
	return true;
}

FSuperSLMPagedHold FSuperSLMPagedTestSeam::HoldStart(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind)
{
	FSuperSLMPagedHold Hold;
	const EPagedJobKind JobKind = ToPagedJobKind(Kind);
	if (Subsystem.State == nullptr || JobKind == EPagedJobKind::Count)
	{
		return Hold;
	}
	FSuperSLMSubsystemState::FPagedHold& Armed = Subsystem.State->PagedHolds.AddDefaulted_GetRef();
	Armed.Id = GNextPagedHoldId++;
	Armed.Kind = JobKind;
	Armed.bApply = false;
	Hold.Id = Armed.Id;
	return Hold;
}

FSuperSLMPagedHold FSuperSLMPagedTestSeam::HoldApply(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind)
{
	FSuperSLMPagedHold Hold;
	const EPagedJobKind JobKind = ToPagedJobKind(Kind);
	if (Subsystem.State == nullptr || JobKind == EPagedJobKind::Count)
	{
		return Hold;
	}
	FSuperSLMSubsystemState::FPagedHold& Armed = Subsystem.State->PagedHolds.AddDefaulted_GetRef();
	Armed.Id = GNextPagedHoldId++;
	Armed.Kind = JobKind;
	Armed.bApply = true;
	Hold.Id = Armed.Id;
	return Hold;
}

bool FSuperSLMPagedTestSeam::IsHoldReached(const USuperSLMSubsystem& Subsystem, const FSuperSLMPagedHold& Hold)
{
	if (Subsystem.State == nullptr || !Hold.IsValid())
	{
		return false;
	}
	const FSuperSLMSubsystemState::FPagedHold* Found = Subsystem.State->PagedHolds.FindByPredicate(
		[&Hold](const FSuperSLMSubsystemState::FPagedHold& H) { return H.Id == Hold.Id; });
	return Found != nullptr && Found->bReached && !Found->bReleased;
}

bool FSuperSLMPagedTestSeam::ReleaseHold(USuperSLMSubsystem& Subsystem, const FSuperSLMPagedHold& Hold)
{
	FSuperSLMSubsystemState::FPagedHold* Found = Subsystem.State != nullptr ? Subsystem.State->FindPagedHold(Hold.Id) : nullptr;
	if (Found == nullptr || Found->bReleased)
	{
		return false;
	}
	Found->bReleased = true;
	return true;
}

FSuperSLMPagedLedgerView FSuperSLMPagedTestSeam::Ledger(const USuperSLMSubsystem& Subsystem)
{
	FSuperSLMPagedLedgerView View;
	const FSuperSLMSubsystemState* State = Subsystem.State;
	View.LedgerFaults = Subsystem.PriorLedgerFaults + (State != nullptr ? static_cast<uint64>(State->Ledger.LedgerFaults()) : 0);
	if (State == nullptr || !State->IsBudgetMode())
	{
		return View;
	}
	const SuperSLMPaging::FPageLedgerStats L = State->Ledger.GetStats();
	View.bConfigured = L.bConfigured;
	View.PoolPages = L.PoolPages;
	for (int32 Region = 0; Region < static_cast<int32>(ESuperSLMPagedRegion::Count); ++Region)
	{
		View.Capacity[Region] = L.Regions[Region].Capacity;
		View.Charged[Region] = L.Regions[Region].Charged;
	}
	View.TotalCharged = State->Ledger.TotalCharged();
	View.LivePrefixCount = static_cast<int32>(L.LivePrefixCount);
	View.GhostCount = static_cast<int32>(L.GhostCount);
	View.InFlightRestores = static_cast<int32>(L.InFlightRestores);
	View.WaitingRestores = static_cast<int32>(L.WaitingRestores);
	return View;
}

int64 FSuperSLMPagedTestSeam::PrefixChargedPages(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix)
{
	const SuperSLMPaging::FNodeId* Node = NodeOf(Subsystem.State, Prefix);
	if (Node == nullptr || !(Subsystem.State->Ledger.IsPrefixLive(*Node) || Subsystem.State->Ledger.IsGhost(*Node)))
	{
		return -1;
	}
	return Subsystem.State->Ledger.NodeChargedPages(*Node);
}

int32 FSuperSLMPagedTestSeam::PrefixDependentCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix)
{
	const SuperSLMPaging::FNodeId* Node = NodeOf(Subsystem.State, Prefix);
	if (Node == nullptr || !(Subsystem.State->Ledger.IsPrefixLive(*Node) || Subsystem.State->Ledger.IsGhost(*Node)))
	{
		return -1;
	}
	return static_cast<int32>(Subsystem.State->Ledger.DependentCount(*Node));
}

bool FSuperSLMPagedTestSeam::IsPrefixGhost(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix)
{
	const SuperSLMPaging::FNodeId* Node = NodeOf(Subsystem.State, Prefix);
	return Node != nullptr && Subsystem.State->Ledger.IsGhost(*Node);
}

int64 FSuperSLMPagedTestSeam::SequenceHeadroomHold(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	const int32 Index = SlotIndexOfState(Subsystem.State, Sequence);
	if (Index == INDEX_NONE || !Subsystem.State->IsBudgetMode())
	{
		return -1;
	}
	return Subsystem.State->Ledger.SlotHeadroomHold(Index);
}

bool FSuperSLMPagedTestSeam::SequenceHasOpenJob(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	const int32 Index = SlotIndexOfState(Subsystem.State, Sequence);
	return Index != INDEX_NONE && Subsystem.State->IsBudgetMode() && Subsystem.State->Ledger.SlotHasJob(Index);
}

int32 FSuperSLMPagedTestSeam::SlotIndexOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence)
{
	return SlotIndexOfState(Subsystem.State, Sequence);
}

int64 FSuperSLMPagedTestSeam::SlotHolderGeneration(const USuperSLMSubsystem& Subsystem, int32 SlotIndex)
{
	const FSuperSLMSubsystemState* State = Subsystem.State;
	if (State == nullptr || !State->Slots.IsValidIndex(SlotIndex))
	{
		return -1;
	}
	return State->Slots[SlotIndex].HolderGeneration;
}

bool FSuperSLMPagedTestSeam::ForceUnderCharge(USuperSLMSubsystem& Subsystem, ESuperSLMPagedChargeKind Kind, uint32 Pages)
{
	if (Subsystem.State == nullptr || !Subsystem.State->IsBudgetMode())
	{
		return false;
	}
#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
	SuperSLMPaging::EChargeKind Charge = SuperSLMPaging::EChargeKind::Count;
	switch (Kind)
	{
	case ESuperSLMPagedChargeKind::PrefixBuild:         Charge = SuperSLMPaging::EChargeKind::PrefixBuild; break;
	case ESuperSLMPagedChargeKind::NestedBuild:         Charge = SuperSLMPaging::EChargeKind::NestedBuild; break;
	case ESuperSLMPagedChargeKind::BudgetRestore:       Charge = SuperSLMPaging::EChargeKind::BudgetRestore; break;
	case ESuperSLMPagedChargeKind::WholeReserveRestore: Charge = SuperSLMPaging::EChargeKind::WholeReserveRestore; break;
	default:                                            break;
	}
	if (Charge == SuperSLMPaging::EChargeKind::Count)
	{
		return false;
	}
	Subsystem.State->Ledger.SetForceUnderChargeForTest(Charge, Pages);
	return true;
#else
	(void)Kind;
	(void)Pages;
	return false;
#endif
}

bool FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind, sslm_status Status)
{
	const EPagedJobKind JobKind = ToPagedJobKind(Kind);
	if (Subsystem.State == nullptr || JobKind == EPagedJobKind::Count)
	{
		return false;
	}
	Subsystem.State->Layer1Overrides[static_cast<int32>(JobKind)] = Status;
	return true;
}

bool FSuperSLMPagedTestSeam::EditPayload(TArray<uint8>& WrappedBlob, TFunctionRef<void(TArray<uint8>& Payload)> Edit)
{
	SuperSLMSaveBlob::FContents Contents;
	const uint8* Payload = nullptr;
	int64 PayloadBytes = 0;
	FString Error;
	if (!SuperSLMSaveBlob::Read(WrappedBlob, Contents, Payload, PayloadBytes, Error) || Payload == nullptr)
	{
		return false;
	}
	// The payload is the wrapper's last field, preceded by its u64 size: everything before that
	// size is kept byte for byte.
	const int64 PayloadOffset = static_cast<int64>(Payload - WrappedBlob.GetData());
	// Plugin paging plan §3.9 (K7): a GPU blob's payload is the GPU subsystem's own section ('SLUG'
	// v1: a 24-byte head, 12 bytes per carried event, the u64 Layer-1 length) followed by the Layer-1
	// bytes, so the edit sees the Layer-1 payload alone, as on the CPU, and the section's recorded
	// Layer-1 length is rewritten with the wrapper's payload size. Its layout is
	// SuperSLMGpuSubsystem.cpp's (FinalizeSaveOp()); a GPU payload without the section is edited whole.
	int64 Layer1Offset = 0;
	if (Contents.Backend == ESuperSLMBackend::GPU && PayloadBytes >= 32 && Payload[0] == 'S' && Payload[1] == 'L' && Payload[2] == 'U' && Payload[3] == 'G')
	{
		uint32 CarriedCount = 0;
		for (int32 B = 0; B < 4; ++B)
		{
			CarriedCount |= static_cast<uint32>(Payload[20 + B]) << (8 * B);
		}
		const int64 SectionBytes = 24 + 12 * static_cast<int64>(CarriedCount) + 8;
		if (SectionBytes > PayloadBytes)
		{
			return false;
		}
		Layer1Offset = SectionBytes;
	}
	TArray<uint8> Edited;
	Edited.Append(Payload + Layer1Offset, static_cast<int32>(PayloadBytes - Layer1Offset));
	Edit(Edited);
	TArray<uint8> NewPayload;
	if (Layer1Offset > 0)
	{
		NewPayload.Append(Payload, static_cast<int32>(Layer1Offset - 8));
		const uint64 Layer1Length = static_cast<uint64>(Edited.Num());
		for (int32 B = 0; B < 8; ++B)
		{
			NewPayload.Add(static_cast<uint8>(Layer1Length >> (8 * B)));
		}
	}
	NewPayload.Append(Edited);
	WrappedBlob.SetNum(static_cast<int32>(PayloadOffset), EAllowShrinking::No);
	WrappedBlob.Append(NewPayload);
	SuperSLMSaveBlob::FinalizePayload(WrappedBlob, PayloadOffset, NewPayload.Num());
	return true;
}

bool FSuperSLMPagedTestSeam::SetWrapperContextUsed(TArray<uint8>& WrappedBlob, int64 ContextUsed)
{
	SuperSLMSaveBlob::FContents Contents;
	const uint8* Payload = nullptr;
	int64 PayloadBytes = 0;
	FString Error;
	if (!SuperSLMSaveBlob::Read(WrappedBlob, Contents, Payload, PayloadBytes, Error))
	{
		return false;
	}
	// The wrapper's "context positions used" field, little-endian at offset 112
	// (SuperSLMSaveBlob.h's layout).
	constexpr int32 kContextUsedOffset = 112;
	for (int32 I = 0; I < 8; ++I)
	{
		WrappedBlob[kContextUsedOffset + I] = static_cast<uint8>(static_cast<uint64>(ContextUsed) >> (8 * I));
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

#if SUPERSLM_WITH_PREFIX_CACHE
#include "SuperSLMSubsystemPrefixCache.inl"
#endif
