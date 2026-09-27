#include "SuperSLMSubsystem.h"

#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSaveBlob.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMStatusMapping.h"

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

#include "superslm/sslm_abi.h"

#include <atomic>
#include <cmath>

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
		// queue but has not yet reached the front and run. It is never expected to exceed 1 now
		// that admission itself is gated on it; a value above 1 would mean this rule has a second
		// hole somewhere else.
		int32 OutstandingJobCount = 0;
		bool HasUndeliveredJob() const { return OutstandingJobCount > 0; }

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
		// every point IsDue() is evaluated; this cap's own condition can no longer be reached in
		// practice, and it is left in place as a named, harmless invariant rather than removed,
		// since removing it is no part of closing the race this round fixes.
		int32 SpeculativeFinishesQueued = 0;
		bool bAdopted = false;         // holds an adopted prefix's context

		// True once a queued Restore has reserved this (otherwise-free) slot but the worker has
		// not yet delivered the restore -- excluded from vend/due processing until it delivers.
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
	// Chunk x d + Chunk x (Chunk - 1) / 2 -- reported as PromptPositionSum and priced by
	// PromptTokenCostPerPositionMs.
	int64 PromptChunkPositionSum(int32 Chunk, int64 StartDepth)
	{
		return static_cast<int64>(Chunk) * StartDepth + static_cast<int64>(Chunk) * (Chunk - 1) / 2;
	}

	// A prefill chunk of Chunk tokens starting at depth d costs
	// Chunk x PromptTokenCostMs + PromptTokenCostPerPositionMs x (Chunk x d + Chunk x (Chunk - 1) / 2).
	double PrefillChunkCostMs(int32 Chunk, int64 StartDepth, double PromptTokenCostMs, double PromptTokenCostPerPositionMs)
	{
		return Chunk * PromptTokenCostMs + PromptTokenCostPerPositionMs * static_cast<double>(PromptChunkPositionSum(Chunk, StartDepth));
	}

	// The largest chunk (at least 1, at most MaxChunk) whose depth-priced cost fits ShareMs -- the
	// sequence prefill (depth = the slot's ContextUsed) and the prefix prefill (depth = the
	// prefix's own Consumed) both size their chunk here (D-SLM7794's third and fourth sites).
	int32 PlanPrefillChunk(int32 MaxChunk, int64 StartDepth, double ShareMs, double PromptTokenCostMs, double PromptTokenCostPerPositionMs)
	{
		int32 Chunk = 1;
		for (int32 C = 2; C <= MaxChunk; ++C)
		{
			if (PrefillChunkCostMs(C, StartDepth, PromptTokenCostMs, PromptTokenCostPerPositionMs) <= ShareMs)
			{
				Chunk = C;
			}
			else
			{
				break;
			}
		}
		return Chunk;
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
	constexpr int32 kMaxJobTicks = 1 << 20;
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

	// D-SLM7799 rule 3 (plan §5 "The scheduler's numeric domain"): the largest planned cost each job
	// kind can reach, computed in double with every product checked. With B = BlockCount, M =
	// MaxLayerBudget, P = min(MaxPrefillChunkBudget, context_cap) and D = context_cap - 1:
	//   decode:      B x (M x (LayerCostMs + LayerCostPerPositionMs x context_cap) + FinishCostMs)
	//   prefill:     P x PromptTokenCostMs + PromptTokenCostPerPositionMs x (P x (D - P + 1) + P x (P - 1) / 2)
	//   lifecycle:   the largest of the six op costs
	// A non-finite bound, or one whose ceil(bound / TickBudgetMs) exceeds kMaxJobTicks, refuses the
	// configuration. Each bound over-approximates what the four pricing sites and the op dispatch
	// can compute. There are two depth limits (D-SLM7803, T-3006 N1):
	//   - a decode prices its layers at depths up to context_cap itself. A prompt of exactly
	//     context_cap tokens is admitted and prefill leaves ContextUsed at the cap; the next decode
	//     job is priced at the cap, and only its Layer-1 call refuses (SSLM_CONTEXT_CAP_EXCEEDED). Bounding decode at
	//     the cap keeps that refusal the caller's context-cap error, never the plugin-defect fault;
	//   - a prefill position never exceeds D, because a prompt holds at most context_cap tokens.
	// A job's layers never exceed M, and its chunk never exceeds P.
	bool CheckCostDomain(const FSuperSLMRuntimeConfig& Config, int64 ContextCap, FString& OutError)
	{
		bool bFinite = true;
		auto Mul = [&bFinite](double A, double B)
		{
			const double R = A * B;
			bFinite = bFinite && FMath::IsFinite(R);
			return R;
		};
		auto Add = [&bFinite](double A, double B)
		{
			const double R = A + B;
			bFinite = bFinite && FMath::IsFinite(R);
			return R;
		};
		const double B = static_cast<double>(Config.BlockCount);
		const double M = static_cast<double>(Config.MaxLayerBudget);
		const double P = static_cast<double>(FMath::Min<int64>(Config.MaxPrefillChunkBudget, ContextCap));
		const double D = static_cast<double>(FMath::Max<int64>(0, ContextCap - 1));
		const double DecodeDepth = static_cast<double>(FMath::Max<int64>(0, ContextCap)); // N1: decode prices up to the cap

		struct FBound { const TCHAR* Kind; double Ms; bool bFinite; };
		FBound Bounds[3];

		bFinite = true;
		const double LayerAtCap = Add(Config.LayerCostMs, Mul(Config.LayerCostPerPositionMs, DecodeDepth));
		const double Decode = Mul(B, Add(Mul(M, LayerAtCap), Config.FinishCostMs));
		Bounds[0] = { TEXT("decode"), Decode, bFinite };

		bFinite = true;
		const double Positions = Add(Mul(P, D - P + 1.0), Mul(P, P - 1.0) / 2.0);
		const double Prefill = Add(Mul(P, Config.PromptTokenCostMs), Mul(Config.PromptTokenCostPerPositionMs, Positions));
		Bounds[1] = { TEXT("prefill"), Prefill, bFinite };

		const double Lifecycle = FMath::Max(FMath::Max(FMath::Max(Config.ResetCostMs, Config.AdoptCostMs), FMath::Max(Config.SaveCostMs, Config.RestoreCostMs)),
			FMath::Max(Config.PrefixBeginCostMs, Config.PrefixReleaseCostMs));
		Bounds[2] = { TEXT("lifecycle"), Lifecycle, FMath::IsFinite(Lifecycle) };

		for (const FBound& Bound : Bounds)
		{
			const double K = Bound.bFinite ? std::ceil(Bound.Ms / Config.TickBudgetMs) : TNumericLimits<double>::Max();
			if (!Bound.bFinite || !FMath::IsFinite(K) || K > static_cast<double>(kMaxJobTicks))
			{
				OutError = FString::Printf(
					TEXT("InvalidCostDomain: the %s job's largest planned cost is %s ms, which implies K = %s at TickBudgetMs %g; K must not exceed kMaxJobTicks = %d"),
					Bound.Kind,
					Bound.bFinite ? *FString::Printf(TEXT("%g"), Bound.Ms) : TEXT("not finite"),
					(Bound.bFinite && FMath::IsFinite(K)) ? *FString::Printf(TEXT("%.0f"), K) : TEXT("not finite"),
					Config.TickBudgetMs, kMaxJobTicks);
				return false;
			}
		}
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
		FCriticalSection QueueCS;
		TArray<FQueuedWorkerJobPtr> Queue; // guarded by QueueCS; the worker pops the front,
		                                     // the game thread only ever appends to the back
	};

	// One lane's own record of a job it posted, oldest (soonest-committed) first. Delivery
	// (Apply) drains strictly from the front: a job later in this array is never delivered
	// before an earlier one, matching "results apply in committed order, never reordered."
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

		// Game-thread only (Plan appends at the back, Apply removes from the front) -- never
		// touched by the worker thread, which only ever sees FQueuedWorkerJob via its OWN queue.
		TArray<FLanePendingDelivery> Pending;

		// D-SLM7403/D-SLM7407: this lane is available to PLAN a new job once its own schedule
		// says so -- the most recently POSTED job's own CommittedDeliveryTick -- never "once the
		// worker has actually finished." A lane with nothing pending is available immediately.
		bool IsFreeForPlanning(int32 TickIndex) const
		{
			return Pending.Num() == 0 || TickIndex >= Pending.Last().CommittedDeliveryTick;
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
			Pending.Add(MoveTemp(Entry));
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

struct FSuperSLMSubsystemState
{
	sslm_model Model = nullptr;
	uint8 ArtifactHash[32] = {};
	SuperSLMRuntime::FModelShape Shape;
	FSuperSLMRuntimeConfig Config;

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
	Super::BeginDestroy();
}

// Fold-round ruling 1: TearDown()'s body, over a state pointer rather than the subsystem's own
// member, so a state a worker prepared (BeginConfigure()) can be unwound on a failure or a
// superseded load without ever having been published. It touches only S and Layer 1, never the
// UObject, so it may run on any thread that owns S outright.
static void DestroyCpuStateContents(FSuperSLMSubsystemState& S)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PoolFree", SuperSLMChannel);

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
		const sslm_status Status = sslm_kv_pool_destroy(S.Pool);
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

	// Plan §2.5 row 19 (D-SLM7763): TearDown() releases every lifecycle-op handle entry, and with
	// them every save blob nobody took.
	S.HandleResults.Empty();
	S.RetainedResultBytes = 0;
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
	DestroyCpuStateContents(*State);
	delete State;
	State = nullptr;
	ConfiguredModel = nullptr;
}

// Configure()'s whole body after the teardown (fold-round ruling 1), over captured model bytes
// (SuperSLMRuntime::FModelBytes) rather than the UObject, so it runs on whichever thread calls
// it: the game thread for Configure(), a pool thread for BeginConfigure(). The mapping (the
// whole-file SHA-256 and copy), the lifecycle bandwidth measurement, the KV/workspace/save
// allocations, the lane threads' creation and the warm sequences all happen here. OutState is
// set only on Success; every failure unwinds what it built. Model is null for no model.
static FSuperSLMConfigureReport PrepareCpuState(const SuperSLMRuntime::FModelBytes* Model, const FSuperSLMRuntimeConfig& Config, FSuperSLMSubsystemState*& OutState)
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
	if (!SuperSLMFinishHook::IsValidTaskCount(Config.FinishParallelTasks))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("FinishParallelTasks must lie in [0, %d] (got %d): it is the most blocks one token finish is split into; 0 or 1 runs the finish serially"),
			SuperSLMFinishHook::MaxFinishParallelTasks, Config.FinishParallelTasks));
	}
	// T-2999 S1: the consumer refuses a cost the scheduler cannot size K from, whatever produced
	// it (the calibration command, a project's config, or a hand edit) -- a non-finite or
	// non-positive per-unit cost is refused by name.
	{
		const TPair<const TCHAR*, double> Costs[] = {
			{ TEXT("LayerCostMs"), Config.LayerCostMs }, { TEXT("FinishCostMs"), Config.FinishCostMs },
			{ TEXT("PromptTokenCostMs"), Config.PromptTokenCostMs }, { TEXT("ResetCostMs"), Config.ResetCostMs },
			{ TEXT("AdoptCostMs"), Config.AdoptCostMs }, { TEXT("SaveCostMs"), Config.SaveCostMs },
			{ TEXT("RestoreCostMs"), Config.RestoreCostMs }, { TEXT("PrefixBeginCostMs"), Config.PrefixBeginCostMs },
			{ TEXT("PrefixReleaseCostMs"), Config.PrefixReleaseCostMs },
		};
		for (const TPair<const TCHAR*, double>& Cost : Costs)
		{
			if (!FMath::IsFinite(Cost.Value) || Cost.Value <= 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, positive number of milliseconds (got %f)"), Cost.Key, Cost.Value));
			}
		}
		// D-SLM7793/D-SLM7794: the two per-position depth terms may be 0 (a flat cost), never
		// negative or non-finite.
		const TPair<const TCHAR*, double> Slopes[] = {
			{ TEXT("LayerCostPerPositionMs"), Config.LayerCostPerPositionMs },
			{ TEXT("PromptTokenCostPerPositionMs"), Config.PromptTokenCostPerPositionMs },
		};
		for (const TPair<const TCHAR*, double>& Slope : Slopes)
		{
			if (!FMath::IsFinite(Slope.Value) || Slope.Value < 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, non-negative number of milliseconds per position (got %f)"), Slope.Key, Slope.Value));
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
	// D-SLM7799 rule 3: the derived domain, now that the model shape is known.
	{
		FString DomainError;
		if (!CheckCostDomain(Config, Shape.ContextCap, DomainError))
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCostDomain, DomainError);
		}
	}

	FSuperSLMSubsystemState* NewState = new FSuperSLMSubsystemState();
	NewState->Shape = Shape;
	NewState->Config = Config;
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

	// §5: the Sequence Lifecycle Budget is checked BEFORE anything is allocated. The same figure
	// SuperSLMSequenceLifecycleBudget::CheckModel() computes -- Layer 1's block size on the shared
	// mapping, over this box's measured bandwidths -- read from the mapping already held.
	double WriteBytesPerSec = 0.0;
	double AdoptToResetRatio = 0.0;
	SuperSLMSequenceLifecycleBudget::MeasureHostBandwidthAndAdoptRatio(WriteBytesPerSec, AdoptToResetRatio);
	const FSuperSLMSequenceLifecycleReport Lifecycle = SuperSLMSequenceLifecycleBudget::Predict(
		static_cast<int64>(sslm_kv_block_size(S.Model)), WriteBytesPerSec, AdoptToResetRatio, Config.SequenceLifecycleBudgetMs);
	if (!Lifecycle.bWithinBudget)
	{
		const FString Message = FString::Printf(
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
	const int64 PoolBlocks64 = static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount);
	check(PoolBlocks64 > 0 && PoolBlocks64 <= static_cast<int64>(MAX_int32));
	const int32 PoolBlocks = static_cast<int32>(PoolBlocks64);
	const size_t BlockBytes = sslm_kv_block_size(S.Model);
	const size_t OverheadBytes = sslm_kv_pool_overhead_size(S.Model, static_cast<uint32_t>(PoolBlocks));
	const size_t WorkspaceBytes = sslm_workspace_size(S.Model, &CallShape);
	const size_t StateBytes = sslm_seq_state_size(S.Model);
	if (BlockBytes == 0 || WorkspaceBytes == 0 || StateBytes == 0 || OverheadBytes == SIZE_MAX ||
		BlockBytes > (SIZE_MAX - OverheadBytes) / static_cast<size_t>(PoolBlocks))
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, TEXT("Layer 1 could not size the pool/workspace for this model and call shape"));
	}

	auto AllocFail = [&TearDown, &Refuse](const FString& What)
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::AllocationFailed, What);
	};

	// KV = sslm_kv_block_size * blocks + sslm_kv_pool_overhead_size (§4), 64-byte aligned.
	S.KvBytes = static_cast<int64>(BlockBytes * static_cast<size_t>(PoolBlocks) + OverheadBytes);
	S.KvBuffer = FMemory::Malloc(static_cast<SIZE_T>(S.KvBytes), SSLM_ABI_ALIGNMENT_BYTES);
	if (S.KvBuffer == nullptr)
	{
		return AllocFail(FString::Printf(TEXT("could not allocate the %lld-byte KV pool"), S.KvBytes));
	}
	sslm_status Status = sslm_kv_pool_create(S.Model, S.KvBuffer, static_cast<size_t>(S.KvBytes), static_cast<uint32_t>(PoolBlocks), &S.Pool);
	if (Status != SSLM_OK)
	{
		return AllocFail(FString::Printf(TEXT("sslm_kv_pool_create failed (%s)"), *StatusText(Status)));
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
		if (SuperSLMFinishHook::ShouldInstall(Config.FinishParallelTasks))
		{
			const sslm_parallel_for Hook = SuperSLMFinishHook::Make(Config.FinishParallelTasks);
			Status = sslm_workspace_set_parallel_for(Lane.Workspace, &Hook);
			if (Status != SSLM_OK)
			{
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("sslm_workspace_set_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"), Config.FinishParallelTasks, *StatusText(Status)));
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
		Status = sslm_seq_create(S.Model, &S.Pool, &S.Slots[I].Seq);
		if (Status != SSLM_OK)
		{
			return AllocFail(FString::Printf(TEXT("sslm_seq_create failed for warm sequence %d of %d (%s)"), I + 1, Config.BlockCount, *StatusText(Status)));
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
	Report.Message = FString::Printf(
		TEXT("KV pool %lld bytes (%d sequence + %d prefix blocks of %llu bytes, + %llu overhead), %d worker lane(s) with a %lld-byte workspace and a %lld-byte save buffer each, token finish %s; predicted reset %.2f ms, adopt %.2f ms"),
		S.KvBytes, Config.BlockCount, Config.PrefixBlockCount, static_cast<uint64>(BlockBytes), static_cast<uint64>(OverheadBytes),
		S.Lanes.Num(), S.WorkspaceBytesEach, S.SaveStagingBytes,
		SuperSLMFinishHook::ShouldInstall(Config.FinishParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Config.FinishParallelTasks) : TEXT("serial"),
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
	FSuperSLMSubsystemState* NewState = nullptr;
	const FSuperSLMConfigureReport Report = PrepareCpuState(Model != nullptr ? &Bytes : nullptr, Config, NewState);
	if (NewState != nullptr)
	{
		State = NewState;
		ConfiguredModel = Model;
	}
	return Report;
}

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
	TWeakObjectPtr<USuperSLMSubsystem> WeakThis(this);
	TSharedPtr<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe> Flight = MakeShared<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe>();
	InFlightConfigures.Add(Flight);

	// Pool thread: PrepareCpuState(), every heavy step of Configure().
	Async(EAsyncExecution::ThreadPool,
		[WeakThis, Serial, bHasModel, Bytes = MoveTemp(Bytes), Hold = MoveTemp(Hold), Flight, Config, OnDone = MoveTemp(OnDone)]() mutable
		{
			FSuperSLMSubsystemState* Built = nullptr;
			const FSuperSLMConfigureReport Report = PrepareCpuState(bHasModel ? &Bytes : nullptr, Config, Built);
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
						Self->State = NewState;
						Self->ConfiguredModel = Hold.IsValid() ? Hold->Get() : nullptr;
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
	FSuperSLMSubsystemState& S = *State;
	if (S.Prefixes.Num() >= S.Config.PrefixBlockCount)
	{
		OutError = FString::Printf(TEXT("PrefixBlockCount (%d) prefixes are already live; release one, or configure more prefix blocks"), S.Config.PrefixBlockCount);
		TRACE_BOOKMARK(TEXT("SuperSLM: prefix pool refusal (%d live)"), S.Prefixes.Num());
		return false;
	}
	if (Tokens.Num() == 0)
	{
		OutError = TEXT("the prefix is empty");
		return false;
	}
	if (Tokens.Num() > S.Shape.ContextCap)
	{
		OutError = FString::Printf(TEXT("the prefix's %d tokens exceed the model's context_cap of %lld"), Tokens.Num(), S.Shape.ContextCap);
		return false;
	}
	for (int32 I = 0; I < Tokens.Num(); ++I)
	{
		if (Tokens[I] < 0 || Tokens[I] >= S.Shape.VocabSize)
		{
			OutError = FString::Printf(TEXT("prefix token %d at index %d is outside the model's vocabulary [0, %d)"), Tokens[I], I, S.Shape.VocabSize);
			return false;
		}
	}

	const int64 Id = GNextPrefixId++;
	FPrefixEntry& Entry = S.Prefixes.Add(Id);
	Entry.Tokens = Tokens;
	FPrefixAdminOp Op;
	Op.Kind = EPrefixAdminKind::Begin;
	Op.Ordinal = S.MintOrdinal();
	Op.PrefixId = Id;
	S.PrefixAdminQueue.Add(Op);
	OutPrefix.Id = Id;
	S.PublishStats(S.LastReport);
	return true;
}

ESuperSLMPrefixPhase USuperSLMSubsystem::GetPrefixPhase(const FSuperSLMPrefix& Prefix) const
{
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
	if (Entry->Handle == nullptr)
	{
		// Never began (still Pending with no worker job ever run, or already released):
		// remove any still-pending Begin op and drop the entry at once.
		const int64 Id = Prefix.Id;
		State->PrefixAdminQueue.RemoveAll([Id](const FPrefixAdminOp& Op) { return Op.PrefixId == Id; });
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
	State->PrefixAdminQueue.Add(Op);
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
	sslm_status Status = sslm_tokenize(State->Model, Text, OutTokens.GetData(), &Count);
	if (Status == SSLM_BUFFER_TOO_SMALL)
	{
		OutTokens.SetNumUninitialized(Count);
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
		TArray<uint8> Blob;      // Save's own output

		// One per sequence the job touches (a decode batch: one per slot, in slot order).
		TArray<FSequenceJobResult> PerSequence;

		// Reset: the recycle's unbind statuses (plan §2.5 row 17), and the binding re-apply after
		// a recreating reset for an owner that holds the slot (row 7).
		sslm_status UnbindSchemaStatus = SSLM_OK;
		sslm_status UnbindAdapterStatus = SSLM_OK;
		sslm_status ReapplySchemaStatus = SSLM_OK;
		sslm_status ReapplyAdapterStatus = SSLM_OK;

		// Prefix prefill: the freeze at the tail of the prefix's last prefill job (row 22).
		bool bFreezeAttempted = false;
		sslm_status FreezeStatus = SSLM_OK;

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
			Out.bStatsValid = true;
		}
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
		int32 TickIndex, TFunctionRef<TFunction<void()>(int32 LaneIndex)> BuildExecute, TFunction<void(double)> Deliver)
	{
		const int32 LaneIndex = ChooseLane(EntityPinnedLane, CandidateLanes);

		// The ring's next row, cleared for reuse: writing it never allocates, because Configure()
		// sized every row's member list for the largest batch.
		const int64 LedgerRow = S.JobLedger.AppendedCount();
		FSuperSLMWorkerJobReport& Job = S.JobLedger.Append();
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
		if (!TryComputeK(PlannedJobMs, S.Config.TickBudgetMs, K) || !TryCommitTick(TickIndex, K, CommittedTick))
		{
			S.LogSchedulingDefectOnce(FString::Printf(TEXT("PostLedgerJob: planned %f ms at tick %d"), PlannedJobMs, TickIndex));
			K = kMaxJobTicks;
			CommittedTick = TickIndex > TNumericLimits<int32>::Max() - K ? TNumericLimits<int32>::Max() : TickIndex + K;
		}
		Job.K = K;
		Job.PlannedAtTick = TickIndex;
		Job.CommittedDeliveryTick = CommittedTick;
		Job.DeliveredAtTick = -1;

		FLane& Lane = S.Lanes[LaneIndex];
		FQueuedWorkerJobRef WorkerJob = MakeShared<FQueuedWorkerJob, ESPMode::ThreadSafe>();
		WorkerJob->Execute = BuildExecute(LaneIndex);
		Lane.Post(WorkerJob, MoveTemp(Deliver), LedgerRow, Job);
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

		// Row 7: the binding re-apply after a recreating reset re-establishes what sslm_seq_reset
		// would have kept. The bindings are caller-owned, so it is composed only for an owner that
		// holds the slot; the recycle a refused restore queues has no owner and re-applies nothing.
		const bool bReapply = bOwnerHoldsAtPost && Slot.Seq == nullptr;
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
#if WITH_DEV_AUTOMATION_TESTS
		// The test hook is one-shot: this job, the next reset dispatched, takes it.
		Out->TestResetStatus = S.TestNextResetStatus;
		S.TestNextResetStatus = SSLM_OK;
#endif

		auto BuildExecute = [Model, PoolPtr, OldSeq, Carried, bUnbind, bReapplySchema, ReapplySchemaName, bReapplyAdapter, ReapplyAdapter, bOwnerHoldsAtPost, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, PoolPtr, OldSeq, Carried, bUnbind, bReapplySchema, ReapplySchemaName, bReapplyAdapter, ReapplyAdapter, bOwnerHoldsAtPost, Out]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Reset", SuperSLMChannel);
				FSequenceJobResult& Result = Out->PerSequence[0];
				sslm_seq Seq = OldSeq;
				if (OldSeq != nullptr)
				{
					ApplyCarriedOnWorker(Model, OldSeq, Carried, Result.CarriedStatuses); // rule 4: at the job's head
#if WITH_DEV_AUTOMATION_TESTS
					Out->Status = Out->TestResetStatus != SSLM_OK ? Out->TestResetStatus : sslm_seq_reset(OldSeq);
#else
					Out->Status = sslm_seq_reset(OldSeq);
#endif
				}
				else
				{
#if WITH_DEV_AUTOMATION_TESTS
					Out->Status = Out->TestResetStatus != SSLM_OK ? Out->TestResetStatus : sslm_seq_create(Model, PoolPtr, &Seq);
#else
					Out->Status = sslm_seq_create(Model, PoolPtr, &Seq);
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
		auto Deliver = [&S, SlotIndex, HandleId, Owner, bUnbind, Carried, ReapplyAdapterId, Out](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount;
			// D-SLM7946: the reset is no longer in flight, whether it succeeded or was refused and
			// whoever holds the slot now.
			Slot.InFlightResetOwner = 0;
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
					S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Malformed;
				}
				return;
			}

			// Slot-owned: the job that changed Layer 1 writes its mirrors, whoever holds the slot.
			Slot.Seq = Out->SeqOut;
			Slot.LayersDoneInToken = 0;
			Slot.bReadyForLogits = false;
			Slot.ContextUsed = 0;
			Slot.bAdopted = false;
			if (Out->bBoolFlag)
			{
				Slot.PinnedLane = INDEX_NONE; // D-SLM7530: the same reset path as VendSequence()
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

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Reset, 0, 0, 0, S.Config.ResetCostMs,
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
		const int64 PrefixConsumedSnapshot = Prefix ? Prefix->Consumed : 0;

		const sslm_model Model = S.Model;
		const sslm_seq SeqHandle = Slot.Seq;
		const sslm_prefix PrefixHandle = bPrefixOk ? Prefix->Handle : nullptr;
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);

		auto BuildExecute = [Model, SeqHandle, PrefixHandle, bPrefixOk, Carried, bOwnerHoldsAtPost, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, SeqHandle, PrefixHandle, bPrefixOk, Carried, bOwnerHoldsAtPost, Out]()
			{
				FSequenceJobResult& Result = Out->PerSequence[0];
				ApplyCarriedOnWorker(Model, SeqHandle, Carried, Result.CarriedStatuses); // rule 4: at the job's head
				if (!bPrefixOk)
				{
					Out->bSkipped = true;
					return;
				}
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Adopt", SuperSLMChannel);
				Out->Status = sslm_seq_adopt_prefix(SeqHandle, PrefixHandle);
				if (bOwnerHoldsAtPost)
				{
					ReadStatsOnWorker(Model, SeqHandle, Result);
				}
			};
		};

		const int64 HandleId = Op.HandleId;
		const int64 PrefixIdCap = Op.PrefixId;
		auto Deliver = [&S, SlotIndex, HandleId, Owner, PrefixIdCap, FaultText, Carried, Out, PrefixConsumedSnapshot](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			--Slot.OutstandingJobCount;
			if (FPrefixEntry* Prefix = S.Prefixes.Find(PrefixIdCap))
			{
				Prefix->PendingAdopts = FMath::Max(0, Prefix->PendingAdopts - 1);
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
			Slot.ContextUsed = PrefixConsumedSnapshot;
			Slot.bReadyForLogits = PrefixConsumedSnapshot > 0;
			Slot.LayersDoneInToken = 0;
			DeliverStats(Slot, bOwnerHolds, Out->PerSequence[0]);
			if (HandleId != 0)
			{
				S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Success;
			}
		};

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Adopt, 0, 0, 0, S.Config.AdoptCostMs,
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

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Save, 0, 0, 0, S.Config.SaveCostMs,
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

		// BuildExecute runs once (PostLedgerJob), so it hands its share on by move; the worker
		// closure is then the blob's last owner and releases it on the worker, on every path.
		auto BuildExecute = [Model, PoolPtr, OldSeq, Blob = MoveTemp(Blob), AdapterHandle, bWillAttempt, bOwnerHoldsAtPost, Out](int32 /*LaneIndex*/) mutable -> TFunction<void()>
		{
			return [Model, PoolPtr, OldSeq, Blob = MoveTemp(Blob), AdapterHandle, bWillAttempt, bOwnerHoldsAtPost, Out]() mutable
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
				Out->Status = sslm_seq_restore(Model, PoolPtr, WorkerPayload, static_cast<size_t>(WorkerPayloadBytes), &Restored);
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

		auto Deliver = [&S, SlotIndex, HandleId, Owner, Out, bParsed, bModelMatches, bAdapterOk, bAdapterPinned, ParseErrorCap, ContentsCap, AdapterId](double)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			Slot.bAwaitingRestore = false;
			--Slot.OutstandingJobCount;
			if (bAdapterPinned)
			{
				SuperSLMRuntime::UnpinAdapter(AdapterId);
			}
			const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;

			auto Refuse = [&](ESuperSLMRestoreResult Result, const FString& Message)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("RestoreSequence: %s"), *Message);
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
				switch (Out->Status)
				{
					case SSLM_RESTORE_MODEL_MISMATCH: Refuse(ESuperSLMRestoreResult::ModelMismatch, TEXT("model mismatch: Layer 1 refused the payload (SSLM_RESTORE_MODEL_MISMATCH)")); return;
					case SSLM_RESTORE_KV_MISMATCH: Refuse(ESuperSLMRestoreResult::KvMismatch, TEXT("KV precision mismatch: Layer 1 refused the payload (SSLM_RESTORE_KV_MISMATCH)")); return;
					case SSLM_RESTORE_RESIDUAL_LOST: Refuse(ESuperSLMRestoreResult::ResidualLost, TEXT("residual lost: a legacy blob saved without its residual (SSLM_RESTORE_RESIDUAL_LOST)")); return;
					default: Refuse(ESuperSLMRestoreResult::Malformed, FString::Printf(TEXT("Layer 1 refused the payload (%s)"), *StatusText(Out->Status))); return;
				}
			}

			// Slot-owned: the restored handle and the Layer-1 mirrors, whoever holds the slot.
			Slot.Seq = Out->SeqOut;
			Slot.ContextUsed = ContentsCap.ContextUsed;
			Slot.LayersDoneInToken = FMath::Min(ContentsCap.LayersDoneInToken, S.Shape.NumHiddenLayers);
			Slot.bReadyForLogits = ContentsCap.bReadyForLogits;
			Slot.bAdopted = ContentsCap.Phase == ESuperSLMSequencePhase::Idle && ContentsCap.ContextUsed > 0;
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

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::Restore, 0, 0, 0, S.Config.RestoreCostMs,
			FSuperSLMSequence{Slot.VendedId}, FSuperSLMLifecycleOpHandle{HandleId}, TickIndex, BuildExecute, MoveTemp(Deliver));
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
				case ESlotOpKind::Reset:   OpCostMs = S.Config.ResetCostMs; OpName = TEXT("reset"); break;
				case ESlotOpKind::Adopt:   OpCostMs = S.Config.AdoptCostMs; OpName = TEXT("adopt"); break;
				case ESlotOpKind::Save:    OpCostMs = S.Config.SaveCostMs; OpName = TEXT("save"); break;
				case ESlotOpKind::Restore: OpCostMs = S.Config.RestoreCostMs; OpName = TEXT("restore"); break;
				case ESlotOpKind::Generate: return; // never reaches here; filtered by the caller
			}
			if (!TrySchedule(S, OpCostMs, TickIndex, OpName))
			{
				const int64 HandleId = Front.HandleId;
				const int64 Owner = Front.Owner;
				const bool bRestore = Front.Kind == ESlotOpKind::Restore;
				Slot.OpQueue.RemoveAt(0, EAllowShrinking::No);
				if (HandleId != 0)
				{
					S.HandleResults.FindOrAdd(HandleId).Result = ESuperSLMRestoreResult::Malformed;
				}
				const bool bOwnerHolds = Owner != 0 && Owner == Slot.VendedId;
				if (bOwnerHolds)
				{
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
				}
				if (bRestore)
				{
					Slot.bAwaitingRestore = false;
				}
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

	void DispatchPrefixAdmin(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 AdminIndex, int32 TickIndex)
	{
		// D-SLM7799: checked before any state changes. A failure posts nothing: the op is dropped and
		// a Begin's prefix faults.
		{
			const FPrefixAdminOp& Front = S.PrefixAdminQueue[AdminIndex];
			const bool bBegin = Front.Kind == EPrefixAdminKind::Begin;
			if (!TrySchedule(S, bBegin ? S.Config.PrefixBeginCostMs : S.Config.PrefixReleaseCostMs, TickIndex, bBegin ? TEXT("prefix begin") : TEXT("prefix release")))
			{
				if (bBegin)
				{
					if (FPrefixEntry* Faulting = S.Prefixes.Find(Front.PrefixId))
					{
						S.FaultPrefix(*Faulting, kSchedulingDomainFault);
					}
				}
				S.PrefixAdminQueue.RemoveAt(AdminIndex, EAllowShrinking::No);
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

		auto BuildExecute = [Model, PoolPtr, HandleForRelease, bIsBegin, Out](int32 /*LaneIndex*/) -> TFunction<void()>
		{
			return [Model, PoolPtr, HandleForRelease, bIsBegin, Out]()
			{
				if (bIsBegin)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixBegin", SuperSLMChannel);
					sslm_prefix NewHandle = nullptr;
					Out->Status = sslm_prefix_begin(Model, PoolPtr, &NewHandle);
					Out->SeqOut = reinterpret_cast<sslm_seq>(NewHandle); // opaque pointer, carried through
				}
				else
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixRelease", SuperSLMChannel);
					Out->Status = sslm_prefix_release(HandleForRelease);
				}
			};
		};

		const int64 PrefixIdCap = Op.PrefixId;
		auto Deliver = [&S, PrefixIdCap, bIsBegin, Out](double)
		{
			if (bIsBegin)
			{
				FPrefixEntry* Prefix = S.Prefixes.Find(PrefixIdCap);
				if (Prefix == nullptr)
				{
					return; // released before this Begin op's own delivery could run
				}
				Prefix->bAdminJobInFlight = false;
				if (Out->Status != SSLM_OK)
				{
					Prefix->Handle = nullptr;
					S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_begin failed (%s)"), *StatusText(Out->Status)));
				}
				else
				{
					Prefix->Handle = reinterpret_cast<sslm_prefix>(Out->SeqOut);
					Prefix->Phase = ESuperSLMPrefixPhase::Prefilling;
				}
				return;
			}
			// ReleasePrefix() already removed the caller-visible entry synchronously; this
			// delivery is fire-and-forget bookkeeping for the real, worker-side release only.
			if (Out->Status != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("ReleasePrefix: sslm_prefix_release failed (%s)."), *StatusText(Out->Status));
			}
		};

		// D-SLM7504 (plan §5 item 2, §10.2 item 10): PrefixBegin/PrefixRelease are their own
		// ESuperSLMWorkerJobKind values now, priced from their own static cost fields -- never the
		// zero-composition floor -- and ledger-tracked via PostLedgerJob, exactly like the four
		// existing lifecycle-op kinds. Neither belongs to a single caller-facing sequence, so
		// LifecycleOpSequence/LifecycleOpHandle are passed invalid, the same way a DecodeOrPrefill
		// job that batches several sequences already does.
		PostLedgerJob(S, EntityPinnedLane, CandidateLanes, bIsBegin ? ESuperSLMWorkerJobKind::PrefixBegin : ESuperSLMWorkerJobKind::PrefixRelease,
			0, 0, 0, bIsBegin ? S.Config.PrefixBeginCostMs : S.Config.PrefixReleaseCostMs,
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
	}

	void DispatchSequencePrefill(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, int32 SlotIndex, int32 TickIndex)
	{
		FSlot& Slot = S.Slots[SlotIndex];
		const int64 SlotVendedIdCap = Slot.VendedId;

		const int32 Left = Slot.Prompt.Num() - Slot.PromptConsumed;
		const double ShareMs = S.Config.TickBudgetMs * S.Config.BudgetHeadroom;
		// D-SLM7794 site 3: the chunk starts at the slot's ContextUsed, then one position per token.
		const int64 StartDepth = Slot.ContextUsed;
		const int32 Chunk = PlanPrefillChunk(FMath::Min(S.Config.MaxPrefillChunkBudget, Left), StartDepth, ShareMs,
			S.Config.PromptTokenCostMs, S.Config.PromptTokenCostPerPositionMs);
		const double PlannedMs = PrefillChunkCostMs(Chunk, StartDepth, S.Config.PromptTokenCostMs, S.Config.PromptTokenCostPerPositionMs);
		// D-SLM7799: checked before any state changes.
		if (!TrySchedule(S, PlannedMs, TickIndex, TEXT("sequence prefill")))
		{
			S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
			return;
		}
		++Slot.OutstandingJobCount;

		const sslm_model Model = S.Model;
		const sslm_seq SeqHandle = Slot.Seq;
		// Copied by value, never a raw pointer into Slot.Prompt's own buffer: a job composed for
		// the NEXT chunk can now be posted to this same lane before this call's Deliver() has run
		// (the schedule-fixed redesign, §14.8), so nothing may assume it is the only reader.
		TArray<int32> PrefillTokensCopy;
		PrefillTokensCopy.Append(Slot.Prompt.GetData() + Slot.PromptConsumed, Left);
		const sslm_span_kind SpanKind = static_cast<sslm_span_kind>(Slot.SpanKind);
		FJobOutputRef Out = MakeShared<FJobOutput, ESPMode::ThreadSafe>();
		Out->PerSequence.SetNum(1);

		// Plan §2.5 row 20 rule 4: a Generate's first job carries every held entry below its op's
		// ordinal; each later job of the generation carries adapter entries only.
		const bool bFirstJob = !Slot.bGenerateFirstJobPosted;
		const TArray<FCarriedRequest> Carried = CarryHeldRequests(Slot,
			bFirstJob ? Slot.ActiveGenerateOrdinal : TNumericLimits<int64>::Max(), /*bAdaptersOnly*/ !bFirstJob);
		Slot.bGenerateFirstJobPosted = true;

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob, not chosen here -- the
		// workspace this call needs is therefore captured through BuildExecute's own resolved-lane
		// parameter, never read from a LaneIndex this function no longer receives.
		auto BuildExecute = [&S, Model, SeqHandle, PrefillTokensCopy, Left, Chunk, SpanKind, Carried, Out](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, SeqHandle, PrefillTokensCopy, Left, Chunk, SpanKind, Workspace, Carried, Out]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Prefill", SuperSLMChannel);
				FSequenceJobResult& Result = Out->PerSequence[0];
				ApplyCarriedOnWorker(Model, SeqHandle, Carried, Result.CarriedStatuses); // rule 4: at the job's head
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefillChunk", SuperSLMChannel);
					int32 Consumed = 0;
					Out->Status = sslm_prefill(Model, SeqHandle, PrefillTokensCopy.GetData(), Left, Chunk, SpanKind, Workspace, &Consumed);
					Out->Consumed = Consumed;
				}
				ReadStatsOnWorker(Model, SeqHandle, Result); // row 22: at the job's tail
			};
		};

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

		auto Deliver = [&S, SlotIndex, SlotVendedIdCap, Carried, Out](double WorkerCallMs)
		{
			FSlot& Slot = S.Slots[SlotIndex];
			// Decrement unconditionally -- this job's real work is done either way, and a slot
			// returned mid-flight waits on this count reaching zero regardless of whose content,
			// if anyone's, the job's real result gets applied to.
			--Slot.OutstandingJobCount;
			// Rule 2: caller-owned state only for the job's owner (the holder at post).
			const bool bOwnerHolds = Slot.VendedId == SlotVendedIdCap;
			ConfirmCarried(S, Slot, bOwnerHolds, Carried, Out->PerSequence[0].CarriedStatuses, /*bCarrierInGeneration*/ true);
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
				S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(
					TEXT("sslm_prefill failed after %d of %d prompt tokens (%s)"), Slot.PromptConsumed, Slot.Prompt.Num(), *StatusText(Out->Status)));
				return;
			}
		};

		Slot.NextAvailableTick = PostLedgerJob(S, Slot.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, Chunk, 0, PlannedMs,
			FSuperSLMSequence{SlotVendedIdCap}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
		S.JobLedger.Last()->PromptPositionSum = PromptChunkPositionSum(Chunk, StartDepth); // D-SLM7794 (T-3002 S1)
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
		// D-SLM7794 site 4: a prefix has no slot, so the chunk starts at the prefix's own Consumed.
		const int64 StartDepth = PrefixPtr->Consumed;
		const int32 Chunk = PlanPrefillChunk(FMath::Min(S.Config.MaxPrefillChunkBudget, Left), StartDepth, ShareMs,
			S.Config.PromptTokenCostMs, S.Config.PromptTokenCostPerPositionMs);
		const double PlannedMs = PrefillChunkCostMs(Chunk, StartDepth, S.Config.PromptTokenCostMs, S.Config.PromptTokenCostPerPositionMs);
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

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob; the workspace it
		// requires is read from BuildExecute's own resolved-lane parameter.
		auto BuildExecute = [&S, Model, PrefixHandle, PrefillTokensCopy, Left, Chunk, Out](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, PrefixHandle, PrefillTokensCopy, Left, Chunk, Workspace, Out]()
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
			};
		};

		auto Deliver = [&S, PrefixId, Out](double WorkerCallMs)
		{
			FPrefixEntry* Prefix = S.Prefixes.Find(PrefixId);
			if (Prefix == nullptr)
			{
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
				S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_prefill failed after %d of %d tokens (%s)"),
					Prefix->Consumed, Prefix->Tokens.Num(), *StatusText(Out->Status)));
				return;
			}
			if (Out->bFreezeAttempted)
			{
				if (Out->FreezeStatus == SSLM_OK)
				{
					Prefix->Phase = ESuperSLMPrefixPhase::Ready;
				}
				else
				{
					S.FaultPrefix(*Prefix, FString::Printf(TEXT("sslm_prefix_freeze failed (%s)"), *StatusText(Out->FreezeStatus)));
				}
			}
		};

		PostLedgerJob(S, PrefixPtr->PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, Chunk, 0, PlannedMs,
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
		S.JobLedger.Last()->PromptPositionSum = PromptChunkPositionSum(Chunk, StartDepth); // D-SLM7794 (T-3002 S1)
	}

	// D-SLM7414: ALL due decoding sequences sharing one layer_budget batch into ONE Layer-1 call
	// (sslm_decode_step_v2's own documented shape), so R-S1b's 8-sequence shape plans as one job
	// on one lane. PinnedBudget == 0 means scheduler-chosen (sized from the static cost model,
	// the largest composition whose planned cost fits the share); a nonzero PinnedBudget always
	// runs as pinned, unscanned.
	void DispatchDecodeBatch(FSuperSLMSubsystemState& S, TArray<int32>& CandidateLanes, const TArray<int32>& SlotIndices, int32 PinnedBudget, int32 TickIndex)
	{
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
			Composition.PlannedMs = DecodeCompositionCostMs(Composition, S.Config.LayerCostMs, S.Config.LayerCostPerPositionMs, S.Config.FinishCostMs);
		}
		else
		{
			const double ShareMs = S.Config.TickBudgetMs * S.Config.BudgetHeadroom;
			Composition = PlanDecodeBatch(SlotIndices, S.Slots, NumHiddenLayers, S.Config.MaxLayerBudget, ShareMs,
				S.Config.LayerCostMs, S.Config.LayerCostPerPositionMs, S.Config.FinishCostMs);
		}
		// D-SLM7799: the checked K and tick, before any state changes; a failure posts nothing and
		// faults every member.
		if (!TrySchedule(S, Composition.PlannedMs, TickIndex, TEXT("decode")))
		{
			for (int32 Idx : SlotIndices)
			{
				S.Fault(S.Slots[Idx], ESuperSLMDecodeOutcome::Generating, kSchedulingDomainFault);
			}
			return;
		}
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
		}

		// D-SLM7528 (plan §5 item 3): the lane is resolved by PostLedgerJob; the workspace it
		// requires is read from BuildExecute's own resolved-lane parameter.
		auto BuildExecute = [&S, Model, Seqs, LayerBudget, bAllFinishOnly, CarriedPerSlot, Out](int32 LaneIndex) -> TFunction<void()>
		{
			const sslm_workspace Workspace = S.Lanes[LaneIndex].Workspace;
			return [Model, Seqs, LayerBudget, Workspace, bAllFinishOnly, CarriedPerSlot, Out]()
			{
				for (int32 I = 0; I < Seqs.Num(); ++I)
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
					Out->DecodeTokens.SetNumUninitialized(Seqs.Num(), EAllowShrinking::No);
					Out->Status = sslm_decode_step_v2(Model, const_cast<sslm_seq*>(Seqs.GetData()), Seqs.Num(), &Params, Workspace, Out->DecodeTokens.GetData());
				}
				for (int32 I = 0; I < Seqs.Num(); ++I)
				{
					ReadStatsOnWorker(Model, Seqs[I], Out->PerSequence[I]); // row 22: at the job's tail
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
		WasReadyForLogitsCopy.Reserve(SlotIndices.Num());
		WasFinishingCopy.Reserve(SlotIndices.Num());
		for (int32 Idx : SlotIndices)
		{
			FSlot& Slot2 = S.Slots[Idx];
			SlotVendedIdsCopy.Add(Slot2.VendedId);
			WasReadyForLogitsCopy.Add(Slot2.bReadyForLogits);
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

		auto Deliver = [&S, SlotIndicesCopy, SlotVendedIdsCopy, WasReadyForLogitsCopy, WasFinishingCopy, CarriedPerSlot, Out, bAllFinishOnly](double WorkerCallMs) // LayerBudget/NumHiddenLayers no longer needed here: Left/Layers/the finishing decision are already resolved above, at POST time
		{
			// Rule 4 and row 22, per member, before its content: every job pin is released, and
			// the carried entries and the stats copy are confirmed only for a member whose owner
			// (the holder at post) still holds the slot (rule 2).
			for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
			{
				FSlot& Slot = S.Slots[SlotIndicesCopy[I]];
				const bool bOwnerHolds = Slot.VendedId == SlotVendedIdsCopy[I];
				ConfirmCarried(S, Slot, bOwnerHolds, CarriedPerSlot[I], Out->PerSequence[I].CarriedStatuses, /*bCarrierInGeneration*/ true);
				DeliverStats(Slot, bOwnerHolds, Out->PerSequence[I]);
			}
			if (Out->Status != SSLM_OK)
			{
				for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
				{
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
					S.Fault(Slot, ESuperSLMDecodeOutcome::Generating, FString::Printf(TEXT("sslm_decode_step_v2 failed (%s)"), *StatusText(Out->Status)));
				}
				return;
			}
			int32 Finishes = 0;
			for (int32 I = 0; I < SlotIndicesCopy.Num(); ++I)
			{
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
			FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
		// Diagnostic only (R-S1d): the ledger row PostLedgerJob just appended names every member.
		for (const int64 MemberId : SlotVendedIdsCopy)
		{
			S.JobLedger.Last()->MemberSequences.Add(FSuperSLMSequence{MemberId});
		}
		// D-SLM7794 (T-3002 S1): the positions this job serves, from the composition's own layer
		// count per slot and depth -- not from its planned cost.
		S.JobLedger.Last()->DecodeLayerDepthSum = Composition.DecodeLayerDepthSum;

		for (int32 Idx : SlotIndices)
		{
			S.Slots[Idx].NextAvailableTick = CommittedDeliveryTick;
			S.Slots[Idx].PinnedLane = GroupPinnedLane;
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
					if (!IsLanePinFree(Op.PinnedLane, S.Lanes, TickIndex))
					{
						continue;
					}
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
			// checks (IsFreeForPlanning/IsDue) are unchanged. Drained strictly front-to-back, so
			// per lane deliveries keep their FIFO order and one entity's results never reorder.
			while (Lane.Pending.Num() > 0)
			{
				FLanePendingDelivery& Front = Lane.Pending[0];
				checkf(Front.WorkerJob.IsValid(), TEXT("SuperSLM CPU lane holds a null pending job -- Post() never stores a null Ptr, so this is an internal invariant violation, not a runtime condition to degrade past."));
				const bool bWorkerDone = Front.WorkerJob->bDone.load(std::memory_order_acquire);
				if (!bWorkerDone)
				{
					break;
				}
				const double WorkerCallMs = Front.WorkerJob->WorkerCallMs;
				const uint32 WorkerThreadId = Front.WorkerJob->WorkerThreadId;
				const double WorkerSpanMs = (Front.WorkerJob->FinishedSeconds - Front.WorkerJob->PostedSeconds) * 1000.0;
				// The hitch rule and the counters read the job's own figures, copied at post, so they
				// hold even when the ring has overwritten the job's ledger row; the row, while
				// retained, records the same figures.
				const bool bWorkerOverran = WorkerCallMs > S.Config.TickBudgetMs;
				// D-SLM7407, plan §5 ("What counts as a hitch"): a result is a hitch when it is
				// delivered after its committed tick AND the worker was left at least K ticks'
				// worth of real wall time (K x TickBudgetMs, posted to finished) and still needed
				// more. A result delivered at or before its committed tick is never one. Without
				// the wall-time half a host ticking faster than TickBudgetMs (the editor's frame
				// loop against an 8 ms budget) counted its own pace as worker lateness (T-2818 box
				// follow-up). WorkerSpanMs is published on the ledger so tests apply this rule.
				const bool bLateByTicks = Report.TickIndex > Front.CommittedDeliveryTick;
				const bool bHitch = bLateByTicks && WorkerSpanMs > double(Front.K) * S.Config.TickBudgetMs;
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
				Lane.Pending.RemoveAt(0, EAllowShrinking::No);
			}
		}
	}
	Report.ApplyMs = (FPlatformTime::Seconds() - ApplyStart) * 1000.0;

	// 2. Plan -- sizes and dispatches the next job(s) from the static per-unit cost model.
	const double PlanStart = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Tick.Plan", SuperSLMChannel);
		if (!S.bTickHorizonReached)
		{
			PlanNextJobs(S, Report, Report.TickIndex);
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
	ESuperSLMRestoreResult QueueRestore(FSuperSLMSubsystemState* State, const TArray<uint8>& Blob, TArray<uint8>* MovableBlob,
		USuperSLMModel* ExpectedModel, FSuperSLMSequence& OutSequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError)
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
		if (Contents.Layer1Tag != CompiledTag)
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
		// slot's queued recycle (H10, kept as built: which physical slot a restore takes may depend
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
		Slot.OpQueue.Add(MoveTemp(Op));
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
	return QueueRestore(State, Blob, /*MovableBlob*/ nullptr, ExpectedModel, OutSequence, OutHandle, OutError);
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
	return QueueRestore(State, Blob, &Blob, ExpectedModel, OutSequence, OutHandle, OutError);
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
	return ESuperSLMRestoreResult::Pending;
}

ESuperSLMRestoreResult USuperSLMSubsystem::GetSaveResult(const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob)
{
	OutBlob.Reset();
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
	return State != nullptr ? State->HandleResults.Num() : 0;
}
#endif // SUPERSLM_WITH_L2S1_ASYNC

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
int32 USuperSLMSubsystem::GetLivePrefixCount() const { return State ? State->Prefixes.Num() : 0; }
int64 USuperSLMSubsystem::GetKvPoolReservedBytes() const { return State ? State->KvBytes : 0; }
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
