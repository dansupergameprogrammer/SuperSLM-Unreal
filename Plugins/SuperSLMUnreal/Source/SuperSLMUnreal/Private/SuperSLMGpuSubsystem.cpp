#include "SuperSLMGpuSubsystem.h"

#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSelfCheckAccess.h"
#include "SuperSLMPrefixCacheTypes.h" // defines SUPERSLM_WITH_PREFIX_CACHE (0 unless Build.cs sets it)
#if SUPERSLM_WITH_PREFIX_CACHE
#include "Core/SuperSLMSaveCompatCore.h"
#endif
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMPagedGpuTestSeam.h" // the paged-KV test seams (plugin paging plan §3.9; declared under WITH_DEV_AUTOMATION_TESTS)
#include "SuperSLMSubsystem.h" // SuperSLMChannel (§5.1's shared trace channel, extern'd there)

#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Trace/Trace.h"

// L2-S2 (the plan §5, §10.3; D-SLM7244, D-SLM7255, D-SLM7256,
// D-SLM7334, D-SLM7337; the 1.7.0 re-pin, §2.5). The GPU backend: a GameInstance subsystem over
// Layer 1's SslmGpu* surface (include/superslm/gpu_1p0.h at the pinned tag), compiled on Win64
// only.
//
// Shape of the implementation, in the order a token moves through it:
//
//  - **One submission thread per subsystem** (plan §5: "All GPU submission, including restore
//    and adapter binds, runs on one dedicated thread ... never the game thread"). Every Layer-1
//    call that touches a sequence, the context or the model runs there, inside a process-wide
//    lock, because Layer 1's decode path submits through one process-global command allocator
//    and requires every submitting call to be externally serialized across the whole process
//    (docs/api.md, "Thread safety"). Every job drains its own submissions before it returns, so
//    no sequence is ever left Submitted between jobs: that is what keeps sslm_gpu_seq_restore
//    from ever seeing SSLM_BUSY (R-S2d).
//
//  - **The fixed-tick scheduler** (plan §5, D-SLM19). Tick() is called by the host once per sim
//    tick. It first APPLIES every result whose tick has come, then PLANS the next tick's work and
//    hands it to the submission thread as one job. A token requested at tick T (its first slice,
//    or its one call) is applied at tick T + K when the device has finished it by then, and
//    otherwise at the first later tick at which it exists; Tick() never waits on the submission
//    thread for it (plan §2.5 row 21, ruling 2026-09-26). Each event counts at most one hitch,
//    decided at its own apply tick (ExamineEvent()); a sequence the apply left holding an
//    overdue event starts no new token, and no new token starts at all while K or more tick
//    jobs are outstanding (a token in flight always continues). The tick that confirms a
//    terminal loss tears down asynchronously; it never waits (BeginTeardownAfterLoss()). Results
//    are applied in order and never reordered. The planner
//    mirrors Layer 1's own budget arithmetic (PlanDispatchBudgetGpu: whole layers only, capped at
//    the token boundary), so which tick finishes each token is decided on the game thread, from
//    the request schedule alone, and replay identity never depends on device timing.
//
//  - **Two decode paths, fixed per sequence at vend or reset (D-SLM7256 rule 3):**
//      * OneCall: SslmGpuSeqDecodeStepForG5Bridge, one whole token per tick, served to the due
//        one-call sequences in rotation, so K >= BlockCount covers them (D-SLM7381);
//      * Composed: sslm_gpu_seq_embed_token, then sslm_decode_step_gpu slices at the tick's layer
//        budget (the batch call with the array rotated each tick when more than one composed
//        sequence is due, D-SLM7255), then SslmGpuSeqFinishTokenForG5Bridge in the job that runs
//        the token's last layer. The adapter passed to every slice of a token is the one bound
//        at its embed (rule 4).
//
//  - **Prompts are fed one token at a time (D-SLM7379).** Each prompt token is embedded and its
//    layers run to full depth -- in slices on the composed path, one whole token per tick on the
//    one-call path -- with no finish. The plugin never calls SslmGpuSeqPrefillPromptForG5Bridge,
//    so there is no prompt-length ceiling, and no prefill verb sets Layer 1's ready_for_logits.
//
//  - **The duplicate-KV guard (D-SLM7256 rules 1-2).** Once the last prompt token is at full
//    depth the plugin records the sequence primed, and its first step is finish-only, never an
//    embed, on both paths. Since 1.6.0 a schema dead end and, at 1.7.0, a hook that breaks its
//    contract also set ready_for_logits; the plugin stops the sequence on both, so the bridge
//    never takes its shortcut on a live plugin sequence. Primed travels in the plugin's own save
//    wrapper.
//
//  - **GPU save/restore at token boundaries (plan §2.5 rows 5-6, D-SLM7665, D-SLM7666).** Layer 1's
//    'SLM5' blob (1.6.0) carries the schema binding and its walk, so a schema-bound sequence saves
//    and resumes mid-walk. Restore does not re-bind: it cross-checks that the restored sequence's
//    binding agrees with the wrapper's schema name.
//
//  - **The 1.7.0 context (plan §2.5 rows 1-3).** Configure() passes the plugin's own shader
//    directory as GpuContextConfig::shader_dir, installs the host parallel-for finish hook, and
//    maps the model with the device-resident head unless the asset turns it off, falling back to
//    the host head once on an out-of-memory refusal.
//
//  - **SSLM_DEVICE_LOST** means either a healthy rejection or a real fault (gpu_1p0.h). The plugin
//    probes once: a usable context faults only the sequence (RecoverablePerSequenceRejection); an
//    unusable one faults every sequence, tears the GPU backend down, and reports the backend
//    inactive, which is the signal a caller routes to the CPU backend on (plan §5). Since 1.8.0 it
//    never means that memory ran out on a live device: that is SSLM_GPU_ALLOCATION_FAILED, which
//    is recoverable per call and is never probed (plan §10.3.1 item 8).

#if SUPERSLMUNREAL_WITH_GPU

#include "Core/SuperSLMPageLedgerCore.h" // the page ledger (plugin paging plan §3.2-§3.5), pooled mode only
#include "SuperSLMFinishHook.h"
#include "SuperSLMGpuStatusMapping.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSaveBlob.h"

#include "HAL/CriticalSection.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Templates/Function.h"
#include "Async/Async.h"
#include "UObject/StrongObjectPtr.h"

#include "superslm/gpu_1p0.h"
#include "superslm/gpu_1p0_g5_bridge.h"
#include "superslm/layer_marshal.h"
#include "superslm/model.h"
#include "superslm/schema_masks.h"
#if SUPERSLM_WITH_PREFIX_CACHE
#include "SuperSLMGpuSubsystemPrefixCache.h" // precache U6 glue; wiring edit 1 (the state member) and 5 (the .inl) only
#endif

#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#pragma warning(push)
#pragma warning(disable : 4996) // Layer 1's d3d12_harness.h uses fopen; a vendored header this plugin does not edit
#include "superslm/gpu_port.h"
#include "d3d12_harness.h"
#include <dxgi1_6.h>
#pragma warning(pop)
#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"

#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <vector>

// Plugin paging plan §3.4 (K6): the engine's own page geometry, R(budget) and the pool-size refusal,
// header-only and D3D12-free (src/gpu, on this module's Win64 private include path). Included after
// the Windows headers so <limits> is read with NOMINMAX in force.
#include "gpu_kv_pool_geometry.h"

// The engine's context_length for a sequence (include/superslm/gpu_1p0_bench_bridge.h, defined in
// gpu_1p0.cpp, which this module compiles). Declared here rather than by including that header,
// which pulls the forward-pass headers in; the declaration is the header's own, verbatim. Read by
// the RunOnPoolThread() seam's ContextLength() (plan §3.9) and by the pooled adopt's job, which tells
// the engine's reset-on-fault after the share from a refusal before it (K7 review N3). The public
// ABI has no other query that tells the two apart: both arms return the same status, and a save's
// size query would report a size failure as SSLM_DEVICE_LOST. The dependency is safe to carry:
// gpu_1p0.cpp defines this accessor unconditionally (under no build flag) and this module compiles
// that file, and because the symbol is declared here, a vendored update that drops it fails this
// module's link loudly rather than changing the adopt path silently. The K7 build record carries
// it as a decision (D6) for the planner to rule on.
int64_t* SslmGpuSeqHandleContextLengthForBench(SslmGpuSequenceHandle* seq);

// Plugin paging plan §3.4 "Prefixes" (K7): GPU prefix ids come from the CPU subsystem's shared
// counter (SuperSLMSubsystem.cpp, GNextPrefixId), so a handle of one backend is unknown to the other.
// Game thread.
int64 SuperSLMMintPrefixId();

// §5.1 (plan §5.1, "GPU backend"; D-SLM7411; R-S2h; T-2826 round 5, 2026-09-19). A dedicated
// SuperSLMGpuChannel, sibling to SuperSLMSubsystem.h's own SuperSLMChannel, with the identical
// cost-when-off guarantee (a disabled TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL is a single
// disabled-channel branch) -- so the GPU backend's own capture (`-trace=SuperSLM,SuperSLMGpu` or
// FTraceAuxiliary::Start(..., TEXT("default,SuperSLMGpu"), ...), the exact request R-S2h's own
// test issues) is selectable independently of the CPU backend's. TRACE_COUNTER_SET/INCREMENT are
// not channel-gated the same way (a fixed per-set cost whether or not a session is recording), so
// every counter below is set at a per-job/per-slice/per-tick granularity, never inside a per-layer
// inner loop (§5.1's own stated cost discipline). Declared only in this translation unit -- no
// other file needs to share it, unlike the CPU channel, which SuperSLMSubsystem.cpp's own EXTERN
// makes available to this file too (R-S1a's determinism comparison runs with it on).
UE_TRACE_CHANNEL_DEFINE(SuperSLMGpuChannel, "SuperSLM GPU backend instrumentation");

// A distinct CSV category keeps the GPU backend's own figures -- driven from a dedicated
// submission thread, never the game thread's own Tick() budget -- from being read as the CPU
// backend's.
CSV_DEFINE_CATEGORY(SuperSLMGpu, true);

// Counter names are the literal `SuperSLM/GPU/*` identifiers §5.1 specifies (R-S2h's own oracle
// scans a real capture for these exact ASCII strings) -- never a separately-invented display name.
TRACE_DECLARE_FLOAT_COUNTER(SuperSLMGpuBusyMs, TEXT("SuperSLM/GPU/GpuBusyMsPerSlice"));
TRACE_DECLARE_FLOAT_COUNTER(SuperSLMGpuHostFinishMs, TEXT("SuperSLM/GPU/HostFinishMs"));
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuLayersPerTick, TEXT("SuperSLM/GPU/LayersPerSlice"));
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuHitches, TEXT("SuperSLM/GPU/HitchCount"));
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuTokensDelivered, TEXT("SuperSLM/GPU/DeliveredTokens"));
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuK, TEXT("SuperSLM/GPU/K"));
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuDueTokens, TEXT("SuperSLM/GPU/DueTokens"));
// Plan §2.5 row 19 (D-SLM7763): save-blob bytes held in handle entries and not yet taken.
TRACE_DECLARE_INT_COUNTER(SuperSLMGpuRetainedResultBytes, TEXT("SuperSLM/GPU/RetainedResultBytes"));

namespace
{
	using FGpuStatus = ::SslmGpuStatus;

	FString GpuStatusText(FGpuStatus Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMGpuStatusMapping::ToDiagnosticText(Status)));
	}

	// Layer 1's decode path submits through one process-global command allocator and list
	// (gpu_1p0.cpp, the SslmGpuContext comment), so every submitting call in the process is
	// serialized here, across every GPU subsystem instance.
	FCriticalSection& Layer1GpuLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}

	// Plan §2.5 row 21 as restated by D-SLM7762: the bound times the Layer-1 call itself, from the
	// moment the submission thread holds Layer1GpuLock() and begins the job
	// (FJob::StartedWallSeconds). Queue and lock waiting carry no timer: a backlog of calls that
	// complete is progress, not a hang.
	//
	// Frame-path calls (slices, tokens, prefill chunks, reset, save and restore jobs, binds): five
	// times Windows' default GPU timeout detection delay of 2 s -- a declared constant, not a
	// measurement of the plugin. That a faulting submission is removed and releases its fences
	// within the detection window is D3D12 device-removal behaviour, not executed here.
	constexpr double kFramePathBoundSeconds = 10.0;

	// Load-time calls (context create, model map with or without the head, adapter map) are host
	// upload and pack work that scales with bytes, which the timeout detection does not bound:
	// bytes / the slowest measured map rate x kLoadTimeBoundMargin, never below the frame-path
	// bound.
	constexpr double kLoadTimeBoundMargin = 5.0;

	// D-SLM7764 (plan row 21, fold record §22): the slowest measured map rate. Set first at 208 MB/s
	// from the second full test run (A-AD head-on, ~1.588 GB in 7.616 s, T-2997 §14), replacing
	// D-SLM7762's provisional 70 MB/s; replaced under D-SLM7764's own rule (the slowest direct
	// reading wins) by the third full test run (T-2997 §16): A-AD head-on, 1,587,104,992 bytes in 11.157 s, 0.000 s queued, the head left on
	// the device (one map, no out-of-memory fallback), so those bytes are what the call uploaded:
	// ~142.25 MB/s, taken down to 142 MB/s. A later direct reading slower than this replaces it;
	// faster readings leave it. Resulting bounds (bytes are the call's worst case, below): A-AD
	// head-on ~103.5 s, A-AD head-off ~47.6 s, A-EX head-on ~30.7 s, A-EX head-off ~12.9 s.
	constexpr double kMeasuredMapBytesPerSecond = 142.0e6;

	double LoadTimeBoundSeconds(int64 Bytes)
	{
		const double Derived = static_cast<double>(FMath::Max<int64>(0, Bytes)) / kMeasuredMapBytesPerSecond * kLoadTimeBoundMargin;
		return FMath::Max(kFramePathBoundSeconds, Derived);
	}

	// Row 21 (D-SLM7762): the process-wide lock records its holder -- which subsystem state, which
	// job, what kind of call, when it began and its bound -- so a game thread waiting on a job that
	// cannot start because another subsystem's call holds the lock takes the device-loss path once
	// that holder has overrun its own bound (the device is shared, and a hung call never releases
	// the lock). Guarded by its own small lock; written by whichever submission thread holds
	// Layer1GpuLock(), read by any waiting game thread.
	struct FLayer1LockHolder
	{
		bool bHeld = false;
		const void* Owner = nullptr;
		uint64 Seq = 0;
		const TCHAR* Kind = TEXT("");
		double StartSeconds = 0.0;
		double BoundSeconds = 0.0;
	};

	FCriticalSection& Layer1LockHolderGuard()
	{
		static FCriticalSection Guard;
		return Guard;
	}

	FLayer1LockHolder& Layer1LockHolder()
	{
		static FLayer1LockHolder Holder;
		return Holder;
	}

	// --- The work a tick hands to the submission thread ---

	enum class EActionKind : uint8
	{
		OneCallPromptToken, // embed one prompt token, all layers, no finish (D-SLM7379)
		OneCallToken,
		ComposedSlice,      // Layers > 0; embeds first when bEmbed; finishes when bFinish
		ComposedFinishOnly, // the primed first step: no embed, no layers (D-SLM7256 rule 1)
		// Plugin paging plan §3.4 "Build pace" (K7, UP-11): one build-seat turn, one whole prefix token
		// (sslm_gpu_prefix_prefill, count 1, a whole token's dispatches). Slot is INDEX_NONE: the
		// action names its prefix by PrefixId, and no sequence's generation filter applies to it.
		PrefixBuildTurn,
	};

	enum class EResultKind : uint8
	{
		Skipped,  // the slot was reset, returned or stopped before this action ran
		PrefillOk, // the last prompt token reached full depth: the sequence is primed
		Token,
		DeadEnd,  // Layer 1's native -2 from the finish (1.6.0): the schema walk has no legal continuation
		Fault,
		Progress, // a slice that did not finish its token, or a prompt token before the last
	};

	struct FAction
	{
		EActionKind Kind = EActionKind::OneCallToken;
		int32 Slot = INDEX_NONE;
		uint32 Gen = 0;
		int32 Layers = 0;
		bool bEmbed = false;
		bool bFinish = false;
		bool bBindAdapter = false;
		const SslmGpuAdapterHandle* Adapter = nullptr;
		// Plan §2.5 row 20 rule 4 (T-2983 finding 10): the per-slot bind ordinal of the adapter
		// request this action carries, echoed back in every later result so the game thread
		// confirms the swap only on a result that ran after the bind.
		int64 BindTag = 0;
		// Prompt tokens (D-SLM7379): the token to embed, whether it is the prompt's last, and, on
		// the prompt's first token, the generation limits the thread stops on.
		bool bPrompt = false;
		bool bLastPrompt = false;
		bool bFirstPrompt = false;
		int32 PromptToken = -1;
		int32 MaxNewTokens = 0;
		TArray<int32> StopTokenIds;
		// Plugin paging plan §3.4 "An empty prompt after adopt" (K7): the first step of an armed
		// sequence's empty-prompt generation carries the generation's limits (MaxNewTokens,
		// StopTokenIds), which a prompt's first token carries otherwise (bFirstPrompt).
		bool bBeginLimits = false;
		// K7 review S1, pooled mode: with bFirstPrompt or bBeginLimits, the generation's limit as the
		// page ledger had applied it when the generation was admitted (FGameSlotUser::PlanBeginLimit),
		// which the thread slot takes (FThreadSlot::Limit): the ledger's applied value wins. 0: keep
		// (dedicated mode, where the limit is not used).
		int64 BeginLimit = 0;
		// PrefixBuildTurn only (K7): the prefix whose token this turn feeds (PromptToken), its prefill
		// kind (0 prompt, 1 schema content) and, with bLastPrompt, that the turn freezes it.
		int64 PrefixId = 0;
		int32 PrefixKind = 0;
	};

	struct FActionResult
	{
		EResultKind Kind = EResultKind::Skipped;
		int32 Token = -1;
		ESuperSLMGpuFaultReason Fault = ESuperSLMGpuFaultReason::None;
		FString Message;
		// The bind tag of the adapter binding the sequence had when this result was produced
		// (FThreadSlot::AppliedBindTag); 0 when no tagged bind has run.
		int64 BindTag = 0;
		// Plan §2.5 row 12, §10.4: SslmGpuSeqSchemaAcceptingForG5Bridge read on the submission
		// thread right after this token's finish (Token and DeadEnd results of a bound sequence);
		// false when unbound. Read after the finish because the bridge reports pre-finish
		// membership until then (gpu_1p0.h).
		bool bSchemaAccepting = false;
		// Ruling 2026-09-26 (plan §2.5 row 21): SslmGpuSeqWalkStateForG5Bridge read on the submission
		// thread beside the accepting read above, carried back so GetSchemaWalkState() never waits on
		// the submission thread. bHasWalkState is set only on results whose job read it (Token and
		// DeadEnd); a restore's carried results have none and leave the game-side record as it is.
		bool bHasWalkState = false;
		uint32 WalkState = kSslmGpuDfaWalkStateUnused;
		// Plugin paging plan §3.4 (K6): the thread slot's ContextUsed after this action ran, carried
		// back so the game side keeps the slot's mirror (the activation pre-check and the seam's
		// SequenceContextUsed() read it). -1 on a result whose action did not run (not live).
		int64 ContextUsed = -1;
		// Plugin paging plan §3.4 "The armed flag" (K7): the thread slot's bArmed after this action ran,
		// 1 or 0, carried back for the slot's armed mirror. -1 on a result whose action did not run.
		int32 Armed = -1;
	};

	struct FJob
	{
		uint64 Seq = 0;
		TArray<FAction> Actions;
		TArray<FActionResult> Results;
		TUniqueFunction<void()> Custom;
		// Diagnostic only (U1 round 6, row 21's open bound): when the job was enqueued, when the
		// submission thread held the Layer-1 lock and began it, and when it finished. Written on
		// the thread that owns each moment; read only for the duration log in Run().
		double EnqueuedWallSeconds = 0.0;
		double StartedWallSeconds = 0.0;
		double CompletedWallSeconds = 0.0;
		// Round 5 (review Low 3): when the call ended -- stamped by Run() right after the Layer-1
		// unlock, before any PostCallDelaySeconds sleep or test gate. CompletedWallSeconds keeps its
		// meaning, the moment the job reads done; the difference is the test hold.
		double CallEndWallSeconds = 0.0;
		// Row 21 (D-SLM7762): what kind of call this job is and its bound, timed from
		// StartedWallSeconds. A tick job and every job not named otherwise is a frame-path call.
		const TCHAR* CallKind = TEXT("frame-path call");
		double BoundSeconds = kFramePathBoundSeconds;
		// Diagnostic only (U1 round 9): the bytes a load-time call's bound was derived from (for a
		// head-on model map, the head-on map's bytes), printed by LogJobDuration() so each reading
		// yields a map rate directly. 0 for every other job.
		int64 LoadBytes = 0;
		// False only for host-side work that makes no Layer-1 call (row 19's save assembly and blob
		// releases): it neither takes Layer1GpuLock() nor enters the lock-holder record.
		bool bTakesLayer1Lock = true;
		// Ruling 2026-09-26: true only for PlanAndIssue()'s job, the one tick job a tick enqueues.
		// It selects which outstanding counter the job is held in (OutstandingTickJobs, the cap's
		// measure, or OutstandingOtherJobs).
		bool bTickJob = false;
		// Test access only (ruling 2026-09-26, FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds()):
		// how long Run() sleeps after this job's Layer-1 work, outside the lock and the holder
		// record, before it is marked done. Copied from the setting by PlanAndIssue() when it
		// creates a tick job, so a later change of the setting never affects a job already queued;
		// 0 for every other job, which therefore never sleeps.
		double PostCallDelaySeconds = 0.0;
		// Ruling 2026-09-26, round 2 ("Terminal loss never waits"): once bTerminalLoss or
		// bUnresponsive is set, Run() completes every job it takes without executing it (every
		// result Skipped), except a job marked here -- the teardown job (ReleaseLayer1()) only.
		bool bRunsAfterLoss = false;
		// Test access only (ruling 2026-09-26, round 2, FSuperSLMGpuTestAccess::HoldNextTickJob()):
		// Run() waits on TestGateEvent after this job's call, outside the Layer-1 lock and after any
		// PostCallDelaySeconds, before its completion time and bDone. Set by PlanAndIssue() on the
		// first tick job it enqueues after the request; false for every other job.
		bool bHoldAfterCall = false;
		// Round 3 (finding 3): true when the job was completed without running -- Run()'s skip after
		// a loss, MarkUnresponsive()'s queue completion, or Enqueue()'s unresponsive return. Written
		// before bDone's release store; RunSync() reads it after, so a skipped job never reads as
		// success.
		bool bSkipped = false;
		std::atomic<bool> bDone{false};
	};

	// Plugin paging plan §3.9 (K6): the job kinds the paged-KV seams count and hold, one per
	// ESuperSLMPagedRequestKind value in the same order (that enum is declared only with the test
	// seams). Always compiled, so the counters cost the same in every build.
	enum class EGpuPagedJobKind : uint8 { Reset, Return, Adopt, Restore, PrefixBuild, NestedBuild, PrefixRelease, Count };

	// One FSuperSLMPagedGpuTestSeam::HoldApply() hold, in arming order: attached to the next admitted
	// job of its kind, reached when that job's Layer-1 work is done and its apply would run, released
	// by ReleaseHold(). They live and die with the state, so Configure() and teardown release them.
	struct FGpuPagedHold
	{
		int64 Id = 0;
		EGpuPagedJobKind Kind = EGpuPagedJobKind::Reset;
		bool bAttached = false;
		bool bReached = false;
		bool bReleased = false;
	};

#if WITH_DEV_AUTOMATION_TESTS
	// Hold ids are unique across states, so a hold from an ended state never names a new one.
	// Game thread (HoldApply()).
	int64 GNextGpuPagedHoldId = 1;
#endif

	// --- Per-slot state ---

	// Owned by the submission thread (and by the game thread only while the thread is drained).
	struct FThreadSlot
	{
		SslmGpuSequenceHandle* Handle = nullptr;
		int64 Cap = 0;
		int64 KvBytes = 0;
		uint32 Gen = 0;
		bool bPrimed = false;       // the last prompt token is at full depth; next step is finish-only
		bool bStopped = false;      // generation over on the thread (stop, max, dead end, fault)
		// The fault that stopped the sequence, re-reported by every later action planned for it,
		// so a fault on a prompt token (which has no event of its own) still reaches the game.
		bool bHasStopFault = false;
		FActionResult StopFault;
		int32 LastToken = -1;       // the most recent produced token: the next token's embed
		int64 ContextUsed = 0;
		// Plugin paging plan §3.4 (K6): a pooled holder's generation limit, min(origin + budget,
		// ContextCap). K6 review N1: its one source is the page ledger's holder shape for the slot,
		// through FSuperSLMGpuSubsystemState::LedgerHolderLimit(), the ledger's own SlotLimit()
		// formula; the job that gives the slot its holder shape (Configure's budgeted create, a reset,
		// an adopt, a pooled restore) writes it here, because the submission thread cannot read the
		// ledger. K7 review S1: a generation's first step then overwrites it with the ledger's applied
		// SlotLimit() as of the generation's admission (FAction::BeginLimit), so where the two ever
		// disagree the ledger's value wins. Never 0 on a live pooled holder (ContextHasRoom() checks);
		// 0 in dedicated mode, where ContextHasRoom() checks Cap alone, exactly as before.
		int64 Limit = 0;
		// Plugin paging plan §3.4 "The armed flag" (K7): the exact mirror of the engine's
		// ready_for_logits for this pooled holder. Set by an adopt (the prefix's last write) and by a
		// restore (the payload's ready word); cleared by a reset and by the first finish-only step
		// while armed, which runs SslmGpuSeqDecodeStepForG5Bridge's ready shortcut instead of the
		// finish bridge (ArmedFinish()). Never set in dedicated mode.
		bool bArmed = false;
		int32 Produced = 0;
		int32 MaxNewTokens = 0;
		TArray<int32> StopTokenIds;
		int32 SchemaIndex = -1;
		const SslmGpuAdapterHandle* BoundAdapter = nullptr;
		const SslmGpuAdapterHandle* PinnedAdapter = nullptr;
		int64 AppliedBindTag = 0;   // the bind tag of the most recent tagged bind that ran (rule 4)
	};

	enum class EEventKind : uint8 { Prefill, Token };

	struct FPendingEvent
	{
		EEventKind Kind = EEventKind::Token;
		int64 ApplyTick = 0;
		int64 RequestTick = 0;          // T: the tick the token was requested (first slice)
		double RequestWallSeconds = 0.0;
		double RequestSimSeconds = 0.0;
		TSharedPtr<FJob> Job;           // null until the action that completes it is issued
		int32 ResultIndex = INDEX_NONE;
		// Test access only (FSuperSLMGpuTestAccess::GetEventLog()): this event's entry in the
		// event log, or INDEX_NONE when the log was off when the event was planned.
		int32 LogIndex = INDEX_NONE;
	};

	// --- The per-sequence software queue (accepted async-tick plan, D-SLM7418/D-SLM7421/
	// D-SLM7424/D-SLM7428/D-SLM7457; plan §5 GPU path / §10.3 item 8) --- declared here, before
	// FGameSlot, because FGameSlot::OpLog holds FLifecycleOpEntry BY VALUE (TArray<T> needs T
	// complete at the point of declaration, unlike a member FUNCTION body, which can reference a
	// not-yet-declared type because the whole class is visible by the time it is parsed).

	// SchemaBind (plan §2.5 row 20 rule 4, D-SLM7758): a SetSchema() issued while the sequence has
	// ops still in its log -- a caller's queued reset, typically -- held in arrival order and bound
	// on the submission thread behind them. It returns no handle to the caller.
	enum class ELifecycleOpKind : uint8 { Reset, Save, BeginGeneration, AdoptPrefix, Restore, SchemaBind };

	// Output state for an admitted op's async Layer-1 work, allocated once at admission and held
	// by TSharedPtr so a later TArray<FLifecycleOpEntry>::Add() (which may reallocate the owning
	// array) never invalidates a submission-thread lambda's captured pointer into it. Written
	// entirely on the submission thread inside the job's own Custom lambda, before that job's
	// FJob::bDone is set; read on the game thread only after observing bDone -- the same
	// release/acquire pairing FPendingEvent/FJob already rely on elsewhere in this file.
	struct FLifecycleAsyncState
	{
		TSharedPtr<FJob> Job;

		// Save outputs.
		FGpuStatus SaveStatus = FGpuStatus::SSLM_OK;
		TArray<uint8> Layer1Bytes;
		bool bPrimed = false;
		bool bStopped = false;
		int32 LastToken = -1;
		int64 ContextUsed = 0;
		// Plan §2.5 row 20 (fold 9, D-SLM7719): the bindings the save records are the submission
		// thread's own, read by the save job, never the game side's confirmed records.
		const SslmGpuAdapterHandle* SavedAdapter = nullptr;
		int32 SavedSchemaIndex = -1;
		// Diagnostic only (R-S2d, U1; GetSaveTokenCounts()): the thread's produced-token count when
		// the save job ran, and the game's applied count at the save's finalize. Neither feeds the
		// blob: the budget it records counts applied tokens only (row 24).
		int32 ProducedAtSave = -1;
		int32 ObservedAtSave = -1;
		// Diagnostic only (R-S2k (vi), U1 round 3): FThreadSlot::AppliedBindTag when the save job ran.
		int64 BindTagAtSave = 0;
		// Row 19 (D-SLM7763): the save's wrapper is assembled off the game thread by AssembleJob,
		// enqueued at finalize; FinishedBlob is its output, moved into the handle's result entry
		// when the save resolves. bAssembled is false when the job never ran (row 21's
		// unresponsive path marks queued jobs done without running them).
		TSharedPtr<FJob> AssembleJob;
		TArray<uint8> FinishedBlob;
		bool bAssembled = false;

		// Reset outputs (plan §2.5 row 16, D-SLM7682): the reset's status and, when it unbinds,
		// both unbind statuses. A refused reset does not unbind and does not clear the thread
		// slot's generation fields.
		bool bResetRan = false;
		FGpuStatus ResetStatus = FGpuStatus::SSLM_OK;

		// Ruling 2026-09-26 (plan §2.5 row 21): SslmGpuSeqWalkStateForG5Bridge read by the reset,
		// bind or restore job after its Layer-1 call, stored into the game-side record when the op
		// finalizes (GetSchemaWalkState()).
		uint32 WalkState = kSslmGpuDfaWalkStateUnused;

		// SchemaBind output (D-SLM7758): SslmGpuSeqSetSchemaForG5Bridge's status.
		FGpuStatus BindStatus = FGpuStatus::SSLM_OK;
		FGpuStatus UnbindSchemaStatus = FGpuStatus::SSLM_OK;
		FGpuStatus UnbindAdapterStatus = FGpuStatus::SSLM_OK;

		// Restore outputs.
		FGpuStatus RestoreStatus = FGpuStatus::SSLM_OK;
		FString RestoreWhere;
		SslmGpuSequenceHandle* FreshHandle = nullptr;
		bool bAdapterBound = false; // rule 4 (T-2987 F4): the restore job bound the saved adapter
		bool bSchemaCrossCheckFailed = false; // row 5: the restored binding disagrees with the wrapper
		// Plugin paging plan §3.5 (K7): a pooled restore's out_shared_pages, for the ledger's apply.
		uint32 SharedPages = 0;

		// Plugin paging plan §3.5 "Normalization" (K7): the reset job replaced the holder create-first
		// (a budgeted SequenceBudget holder created, then the old one released). False for a reset in
		// place, including a normalization whose create was refused.
		bool bNormalized = false;

		// Plugin paging plan §3.4 "Adopt" (K7): sslm_gpu_seq_adopt_prefix's status (or the reported
		// fault after the share), and the thread slot's ContextUsed and armed flag after the job.
		FGpuStatus AdoptStatus = FGpuStatus::SSLM_OK;
		bool bAdoptResetByFault = false; // the engine left the sequence reset (§3.4 "A fault after the share")
		int64 ContextUsedAfter = 0;
		bool bArmedAfter = false;
	};

	// One requested-and-queued lifecycle operation. Lives in FGameSlot::OpLog (Reset/Save/
	// BeginGeneration/AdoptPrefix) or FSuperSLMGpuSubsystemState::RestoreOps (Restore, which has
	// no target sequence to queue against, D-SLM7457 ruling 2). The owning array's logical front
	// index (FGameSlot::OpFront / FSuperSLMGpuSubsystemState::RestoreOpsFront) tracks how far
	// admission has progressed. Resolved entries behind the front are removed by
	// CompactSlotOpLog() or PollRestoreFront() (row 19, D-SLM7763; on a sequence, not while a
	// generation runs -- see CompactSlotOpLog()), so an entry's
	// position in its array is not stable: find an entry by HandleId, never by a stored index. A resolved entry's result
	// moves to HandleResults, where GetLifecycleOpResult()/GetSaveResult()/GetRestoreResult() read
	// it by handle (a caller may poll a handle again after a LATER entry in the same queue has been
	// admitted) until ReleaseLifecycleOpHandle() releases it.
	struct FLifecycleOpEntry
	{
		int64 HandleId = 0;
		ELifecycleOpKind Kind = ELifecycleOpKind::Reset;
		int64 GlobalOrdinal = 0;
		bool bSubmitted = false;
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		// The subsystem-wide resolution-order ordinal (maintainer ruling superseding D-SLM7480,
		// T-2826 round 8): -1 until Result is first published, then the value
		// FSuperSLMGpuSubsystemState::ResolveEntry() assigned at that exact moment, strictly
		// increasing across every op resolved by this subsystem (every sequence's own queue plus
		// the subsystem-level restore queue) in the order each one actually resolved -- readable
		// even when two ops resolved on the identical tick. Read-only telemetry: nothing in this
		// file ever branches on it; it exists solely so a caller (or a test) can observe arrival
		// order directly instead of inferring it from which tick a poll happened to land on.
		int64 ResolutionOrdinal = -1;

		// Plan §2.5 row 20 rule 2: the holder that issued this op (FGameSlot::Id at the request).
		// Its completion writes caller-owned state only while that holder still holds the slot.
		// 0 for a restore, which has no holder until it succeeds.
		int64 Owner = 0;

		// Reset input.
		ESuperSLMGpuDecodePath NewDecodePath = ESuperSLMGpuDecodePath::OneCall;
		// SchemaBind input (D-SLM7758): the schema index to bind (-1 = unbind) and its name.
		FSuperSLMGpuSchemaHandle BindSchema;
		FString BindSchemaName;
		// BeginGeneration input.
		FSuperSLMGenerationRequest Request;
		// Restore input, validated and parsed synchronously at request time (mirrors the prior
		// synchronous RestoreSequence()'s own up-front checks); admission runs only the Layer-1
		// call and the finalize step this parse could not do without Layer 1. Plan §2.5 row 19:
		// the blob is held once, in a shared immutable buffer created when the request is
		// accepted; the submission job shares it and reads the Layer-1 payload in place, so no
		// copy is taken after acceptance.
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> RestoreBlob;
		int64 RestoreLayer1Offset = 0;
		int64 RestoreLayer1Size = 0;
		// Rule 4 (T-2987 L1): the restore pins its adapter from the request until finalize, so
		// UnmapAdapter() is refused while a queued or admitted restore references it.
		bool bRestoreAdapterPinned = false;
		TWeakObjectPtr<USuperSLMModel> RestoreExpectedModel;
		SuperSLMSaveBlob::FContents RestoreContents;
		int32 RestoreSchemaIndex = -1;
		int64 RestoreAdapterId = 0;
		uint8 RestorePathByte = 0;
		bool bRestorePrimed = false;
		bool bRestoreStopped = false;
		int32 RestoreLastToken = -1;
		int32 RestoreInFlightIssued = 0;
		struct FRestoreCarried { EResultKind Kind; ESuperSLMGpuFaultReason Fault; int32 Token; int32 Delay; };
		TArray<FRestoreCarried> RestoreCarried;
		FSuperSLMGpuSequence RestoreOutSequence;
		// The slot AdmitRestoreFront() reserved for this restore, set at admission (finding 2,
		// T-2885 review) -- FinalizeRestoreOp() uses this directly instead of re-deriving the
		// slot by matching the restored Layer-1 handle against every ThreadSlot.
		int32 RestoreSlotIndex = INDEX_NONE;

		// Plugin paging plan §3.2-§3.5 (K6), pooled mode only: the page ledger's ticket for this op's
		// admission (a reset's; K7 adds adopt and restore), applied when the op finalizes. 0 when the
		// op takes no ledger admission (dedicated mode, and saves, generations and binds).
		SuperSLMPaging::FTicket LedgerTicket = SuperSLMPaging::InvalidTicket;
		// Plan rev 1.9 ruling 2: RequestAdoptPrefix(Sequence, Prefix) in dedicated mode. Admission
		// resolves it UnsupportedOnGpu with the dedicated-mode reason, not the no-prefix overload's.
		bool bPrefixOverload = false;
		// FSuperSLMPagedGpuTestSeam::HoldApply() (§3.9): the hold this op's job carries; 0 for none.
		int64 HoldId = 0;

		// Plugin paging plan §3.4 "Adopt" (K7), pooled mode: the prefix RequestAdoptPrefix(Sequence,
		// Prefix) names. 0 for the no-prefix overload and in dedicated mode.
		int64 AdoptPrefixId = 0;

		// Plugin paging plan §3.5 (K7), a pooled restore: the 'SLM6' header's budget, origin and ready
		// word as the call checked them (the ledger's charge and the holder's shape and armed flag
		// read these, never the wrapper), the sharing hint's prefix id (0: none), and whether the
		// ledger holds the restore waiting on H (bSubmitted with no job yet).
		bool bRestorePooled = false;
		int64 RestoreBudget = 0;
		int64 RestoreOrigin = 0;
		bool bRestoreReady = false;
		int64 RestoreHintPrefixId = 0;
		bool bRestoreWaiting = false;

		TSharedPtr<FLifecycleAsyncState> Async;
	};

	// Plan §2.5 row 20 rule 1 (D-SLM7701, D-SLM7706): every caller-owned field of a GPU slot, in
	// one struct. FGameSlot inherits it; ReleaseUser() -- the only code that ends a hold -- resets
	// it by value, and vend and a restore's finalize start from a value-reset struct, so a field
	// added here later is reset without anyone listing it (the hand-written lists this replaces
	// missed LayersPerSliceOverride, H8). Owned by the game thread.
	struct FGameSlotUser
	{
		ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::OneCall;
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		TArray<int32> Generated;
		ESuperSLMDecodeOutcome LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
		ESuperSLMGpuFaultReason LastFault = ESuperSLMGpuFaultReason::None;
		FSuperSLMGenerationRequest Request;
		int32 PromptPlanned = 0;        // prompt tokens whose feeding has been planned (D-SLM7379)
		bool bPlanInPrompt = false;     // the token in progress on the composed path is a prompt token
		int32 PlanPromptIndex = 0;      // its index in Request.PromptTokens
		double BeginWallSeconds = 0.0;  // BeginGeneration()'s wall clock
		double FirstTokenMs = -1.0;     // BeginGeneration() to the first applied token; -1 until then
		bool bPlanPrimed = false;       // the next planned step is finish-only
		int32 TokensIssued = 0;
		int32 PlanLayerPos = 0;         // layers already planned for the token in progress
		int64 TokenRequestTick = 0;
		double TokenRequestWall = 0.0;
		double TokenRequestSim = 0.0;
		int32 LayersPerSliceOverride = 0;
		// The accepting reading of the most recent Token/DeadEnd result applied (FActionResult::
		// bSchemaAccepting); IsSchemaAccepting() reads it. Cleared with the generation.
		bool bSchemaAccepting = false;
		// Ruling 2026-09-26 (plan §2.5 row 21): GetSchemaWalkState()'s game-side record, Layer 1's
		// walk as of the last applied result or finalized op (a token's, the bind's, a reset's or the
		// restore's), carried back from the submission thread. Unused (no schema) at vend and after
		// ReleaseUser(), both of which reset this struct by value.
		uint32 WalkState = kSslmGpuDfaWalkStateUnused;
		TArray<FPendingEvent> Pending;
		FSuperSLMGpuSchemaHandle Schema;
		FString SchemaName;
		int64 ActiveAdapterId = 0;
		int64 RequestedAdapterId = 0;
		bool bSwapPending = false;
		// The bind WantsAdapterBind() most recently planned but has not yet confirmed applied
		// (finding 10, T-2885 review): committing ActiveAdapterId/clearing bSwapPending at PLAN
		// time let a dropped action -- ActionIsLive() false after a generation bump, or an
		// already-stopped sequence's SkipStopped() -- leave GetActiveAdapter() reporting a bind
		// Layer 1 never received. ApplyResult() (via ConfirmPendingBind()) commits this only once
		// a Token or PrefillOk result proves the owning action actually ran.
		int64 PendingBindAdapterId = 0;
		bool bPendingBindConfirm = false;
		// Rule 4's GPU confirmation (T-2983 finding 10): each RequestAdapterSwap() mints the next
		// per-slot bind ordinal; every action carrying that request carries it as its BindTag, and
		// the swap is confirmed only on a result echoing it. A result planned before the request
		// echoes an older tag, so it never confirms the new binding.
		int64 NextBindOrdinal = 0;
		int64 AwaitedBindTag = 0;

		// Rule 4's GPU deferred-refusal field (D-SLM7758): a held schema bind (SchemaBind op) that
		// Layer 1 refused. Faults this holder's next generation at its admission, by name, and is
		// cleared there. Per-user, so a holder who returns never passes it to the next one.
		bool bHasDeferredRefusal = false;
		FString DeferredRefusal;

		// The OpLog index of the BeginGeneration entry currently generating, or INDEX_NONE. Set
		// when that entry admits (Phase flips to Prefilling); cleared when the generation reaches
		// Complete/Faulted OR is interrupted by a Reset admitting -- whichever comes first. Lets
		// GetPendingLifecycleOperationCount() keep counting an ongoing generation even after its
		// own OpLog entry has advanced past OpFront (its "delivered" moment is admission, not
		// completion, per this header's own doc comment).
		int32 ActiveGenerationOpLogIndex = INDEX_NONE;

		// Row 19 (D-SLM7763): between a save's finalize (where its carried events and applied count
		// are snapshotted, row 24) and its resolution once the wrapper is assembled off the game
		// thread, ApplyDue() applies nothing to this slot, so the tokens a caller reads at the save's
		// resolution are exactly the ones the blob counts as applied.
		bool bSaveAssembling = false;

		// Plugin paging plan §3.4 (K6): the slot's mirror of the thread slot's ContextUsed, as of the
		// last applied result (FActionResult::ContextUsed) or finalized reset. 0 at vend: a vended
		// holder is fresh or reset. The pooled activation pre-check reads it.
		int64 ContextUsedMirror = 0;
		// Plugin paging plan §3.4 "The armed flag" (K7): the slot's bL1ReadyArmed, the mirror of the
		// thread slot's bArmed as of the last applied result (FActionResult::Armed) or finalized adopt,
		// reset or restore. False at vend: a vended holder is fresh or reset. The empty-prompt check at
		// a generation's admission and the seam's SequenceArmed() read it.
		bool bArmedMirror = false;
		// K7: the generation now planned is an armed sequence's empty-prompt one, whose first
		// (finish-only) step still has to carry the generation's limits (FAction::bBeginLimits).
		bool bPlanBeginLimits = false;
		// K7 review S1, pooled mode: the generation's limit, read from the page ledger's applied shape
		// when its BeginGeneration was admitted (it waits at the slot front while the slot has a ledger
		// job open, so no job is in flight then), carried to the thread by its first step
		// (FAction::BeginLimit). 0 in dedicated mode and outside a generation.
		int64 PlanBeginLimit = 0;
	};

	struct FGameSlot : FGameSlotUser
	{
		bool bVended = false;
		// Ruling 2026-09-26, back-pressure: set by every ApplyDue() for every slot -- true when its
		// walk stopped at an event with ApplyTick <= TickIndex whose one bDone load this tick read
		// unfinished, false otherwise (and false for a slot the walk skips). PlanAndIssue() reads it
		// and never re-reads bDone, so a job that finishes between the two holds its sequence for
		// this tick and its event applies at the next.
		bool bHeldThisTick = false;
		int64 Id = 0;     // the hand-off key
		uint32 Gen = 0;   // the stale-result filter (slot-owned, K11)
		// Plugin paging plan §3.9 (FSuperSLMPagedGpuTestSeam::SlotHolderGeneration()): rises each
		// time the slot's Layer-1 handle is replaced (a restore's finalize). Slot-owned, so it
		// survives a holder's return.
		int64 HolderGeneration = 0;

		// Plan §2.5 row 4 (D-SLM7665): the bind eligibility the Layer-1 sequence will have once
		// everything already queued for it has run -- true at the vend of a fresh or reset
		// sequence and when a reset is queued; false when a generation request is queued and after
		// a restore. SetSchema() refuses by name, without calling Layer 1, while it is false.
		bool bBindEligibleAfterQueue = true;

		// Plan §2.5 row 16 (D-SLM7682): a recycle whose reset or unbind Layer 1 refused leaves a
		// sequence with a stale binding or an unknown state, so the slot is never vended again
		// until teardown; logged by name and counted (GetWithheldSlotCount()).
		bool bWithheld = false;

		// The per-sequence software queue (D-SLM7421/D-SLM7457). OpLog holds the Request*() entries
		// issued during this vended lifetime that have not been removed yet: every entry still
		// queued or in flight, plus resolved entries CompactSlotOpLog() has kept -- a running
		// generation's own entry and every entry resolved behind it, which stay until the next entry
		// on this sequence resolves after that generation has ended (or the log is reset below).
		// OpFront is the index of the first entry still Pending (queued or in flight). Entries
		// before it are resolved, their results already in HandleResults. Reset to empty at true
		// lifetime boundaries (VendSequence's own vend, RequestRestoreSequence's own fresh vend),
		// never by an admitted Reset op, which is itself just one more entry in this same log.
		TArray<FLifecycleOpEntry> OpLog;
		int32 OpFront = 0;
		// ReturnSequence() called while OpLog still has unresolved entries: the plan's own "the
		// physical block is not handed to a new vend ... until every operation already queued for
		// this sequence ... has drained" (§5). The slot stays logically vended (IdToSlot keeps the
		// mapping, bVended stays true) until the admission step observes the log fully drained,
		// at which point it performs the deferred free.
		bool bReturnPending = false;
		// Plugin paging plan §3.5 (K7): a pooled restore holds this free slot (bVended is set with it)
		// from its arrival until it resolves -- in flight, or waiting on H. A later arrival that finds
		// no free slot waits for such a slot instead of reading PoolExhausted.
		bool bRestoreReserved = false;
	};

	struct FAdapterEntry
	{
		SslmGpuAdapterHandle* Handle = nullptr;
		uint8 ArtifactHash[32] = {};
		int64 ResidentBytes = 0;
		std::unique_ptr<superslm::SslmModelView> View;
		int32 RestorePins = 0; // queued or admitted restores that reference this adapter (rule 4, T-2987 L1)
	};

	TArray<USuperSLMGpuSubsystem*>& ConfiguredSubsystems()
	{
		static TArray<USuperSLMGpuSubsystem*> Instances;
		return Instances;
	}

	FCriticalSection& ConfiguredSubsystemsLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}

	constexpr uint32 kWrapperExtMagic = 0x47554c53u; // 'S','L','U','G'
	constexpr uint32 kWrapperExtVersion = 1;
}

// ---------------------------------------------------------------------------------------------
// The subsystem's state
// ---------------------------------------------------------------------------------------------

struct FSuperSLMGpuSubsystemState : public FRunnable
{
#if SUPERSLM_WITH_PREFIX_CACHE
	// Precache U6 wiring edit 1 (SuperSLMGpuSubsystemPrefixCache.h), the member only: nothing installs a host
	// until edits 2-4 land, so the cache methods refuse NotConfigured.
	SuperSLMGpuPrefixCacheGlue::FState PrefixCache;
#endif
	// --- Configuration (immutable after Configure) ---
	// Config.K alone is re-set after Configure(), by SetFixedTickLatency() while the pool is drained.
	FSuperSLMGpuRuntimeConfig Config;
	int32 NumHiddenLayers = 0;
	int32 VocabSize = 0;
	int64 ModelContextCap = 0;
	uint32 DispatchesPerLayer = 0;
	int32 LayersPerTick = 0;
	int32 MinimumK = 0;
	// Review round 2, R2-W3: the schedule Configure() set (LayersPerTick, DispatchBudget, MinimumK,
	// K). SetLayersPerTick()/SetFixedTickLatency() override it for one client's query;
	// RequestConfiguredScheduleRestore() puts it back at the first tick the pool is drained.
	int32 ConfiguredLayersPerTick = 0;
	uint32 ConfiguredDispatchBudget = 0;
	int32 ConfiguredMinimumK = 0;
	int32 ConfiguredK = 0;
	bool bRestoreScheduleWhenDrained = false;
	uint8 ArtifactHash[32] = {};
	int64 DeclaredModelBytes = 0;
	// The per-sequence GPU K/V byte count Configure() computed once, from Config/
	// NumHiddenLayers exactly as the local KvBytesPerSequence it is copied from -- restored
	// sequences read THIS, never a sibling FThreadSlot's own KvBytes (finding 3, T-2885
	// review: reading ThreadSlots[0].KvBytes zeroed itself when SlotIndex == 0, since TS IS
	// ThreadSlots[0]).
	int64 KvBytesPerSequence = 0;

	// --- Plugin paging plan, pooled mode (K6; §3.1-§3.5, UP-15) ---
	// SequenceBudget > 0. Set at Configure and never changed.
	bool bPooled = false;
	// The engine's page geometry, computed at the model's cap (Ce) exactly as
	// sslm_gpu_kv_pool_create computes it, and cross-checked against the pool's stats at Configure.
	superslm_gpu::kv_pool::GpuPageGeometry PageGeometry;
	// The pool: S + P + H + CacheRegionPages() pages, and its bytes (the device buffer; the host
	// mirror is the same size again, in system RAM, and is not declared GPU residency).
	uint32 PoolPageCount = 0;
	int64 PoolBytes = 0;
	// Created and destroyed on the submission thread (Configure's job 3, ReleaseLayer1()).
	SslmGpuKvPoolHandle* Pool = nullptr;
	// The page ledger. Game thread only, once the state is published (Configure() fills it on the
	// thread that prepares the state, before any other thread can see it).
	SuperSLMPaging::FPageLedger Ledger;
	// The pool's stats as Configure's cross-check read them (GetKvPageStats()' snapshot).
	bool bHasPoolSnapshot = false;
	SslmGpuKvPoolStats LastPoolStats = {};

	// Region C: the GPU has no prefix cache in the paging release, so it is 0 (plan §3.2).
	static constexpr uint32 CacheRegionPages() { return 0u; }

	bool IsPooled() const { return bPooled; }

	// §3.7: the engine returned KV_POOL_EXHAUSTED to one of this plan's own (admitted) requests,
	// which a correct ledger never lets happen. Counted, and logged by name as a ledger fault.
	// Game thread.
	void NoteLedgerFault(const TCHAR* Verb)
	{
		Ledger.NoteEngineExhaustion();
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM ledger fault: %s returned SSLM_GPU_KV_POOL_EXHAUSTED to a request the page ledger admitted (%llu so far); the ledger under-charged it."),
			Verb, static_cast<uint64>(Ledger.LedgerFaults()));
	}

	// K2 review F3's twin: the ledger refused, at the head of a slot's queue, an op it should always
	// admit (a plugin defect). The op is refused by name and never runs untracked; the refusal is
	// counted with the ledger faults and logged as one. Game thread.
	void NoteLedgerRefusal(int32 SlotIndex, const FString& Reason)
	{
		Ledger.NoteEngineExhaustion();
		UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM ledger fault: GPU slot %d: %s; the op was refused, nothing was posted (%llu so far; plugin defect)."),
			SlotIndex, *Reason, static_cast<uint64>(Ledger.LedgerFaults()));
	}

	// §3.9 seams: the submission-thread jobs of each paged kind posted since this Configure(), and
	// every SSLM_BUSY a Layer-1 call of this state returned (cell 3.3: never, when the jobs are
	// Busy-safe). JobsPostedByKind is game thread; Layer1BusyCount is written on the submission
	// thread.
	int64 JobsPostedByKind[static_cast<int32>(EGpuPagedJobKind::Count)] = {};
	std::atomic<int64> Layer1BusyCount{0};

	// Counts a Layer-1 status: SSLM_BUSY is what Layer1BusyCount measures. Any thread.
	void NoteLayer1Status(FGpuStatus Status)
	{
		if (Status == FGpuStatus::SSLM_BUSY)
		{
			Layer1BusyCount.fetch_add(1);
		}
	}

	// HoldApply()'s holds (§3.9). Game thread.
	TArray<FGpuPagedHold> PagedHolds;

	// The next armed hold of Kind, attached to the job being admitted now; 0 when none is armed.
	int64 AttachPagedHold(EGpuPagedJobKind Kind)
	{
		// K6 review N4: an armed hold released before any job took it is disarmed for good; it goes.
		PagedHolds.RemoveAll([](const FGpuPagedHold& Hold) { return Hold.bReleased && !Hold.bAttached; });
		for (FGpuPagedHold& Hold : PagedHolds)
		{
			if (Hold.Kind == Kind && !Hold.bAttached && !Hold.bReleased)
			{
				Hold.bAttached = true;
				return Hold.Id;
			}
		}
		return 0;
	}

	// True while HoldId's job must not be applied: its Layer-1 work is done (the caller checked) and
	// the hold is not released. Marks the hold reached.
	bool PagedHoldBlocks(int64 HoldId)
	{
		if (HoldId == 0)
		{
			return false;
		}
		for (int32 Index = 0; Index < PagedHolds.Num(); ++Index)
		{
			FGpuPagedHold& Hold = PagedHolds[Index];
			if (Hold.Id == HoldId)
			{
				if (Hold.bReleased)
				{
					// K6 review N4: the held apply goes ahead now, so the hold is done with. A later
					// IsHoldReached() or ReleaseHold() of it reads false, as for any released hold.
					PagedHolds.RemoveAt(Index, 1, EAllowShrinking::No);
					return false;
				}
				Hold.bReached = true;
				return true;
			}
		}
		return false;
	}

	// Configure() and teardown: every hold released and every armed one disarmed (cell 1.11).
	void ReleaseAllPagedHolds()
	{
		for (FGpuPagedHold& Hold : PagedHolds)
		{
			Hold.bReleased = true;
		}
	}

	// --- GPU prefixes (plugin paging plan §3.3, §3.4; K7), pooled mode only ---

	// The submission thread's prefix handles, by prefix id. Written and read on the submission thread
	// only (the begin, build-turn, adopt, restore and release jobs, and ReleaseLayer1()).
	struct FThreadPrefix
	{
		SslmGpuPrefixHandle* Handle = nullptr;
	};
	TMap<int64, FThreadPrefix> ThreadPrefixes;

	// A prefix job's output, written by its job before bDone and read on the game thread after.
	struct FPrefixJobState
	{
		FGpuStatus Status = FGpuStatus::SSLM_OK;
		const TCHAR* Where = TEXT("");
	};

	// One planned build turn: the tick job that carries it and its action's index there.
	struct FPrefixTurn
	{
		TSharedPtr<FJob> Job;
		int32 ActionIndex = INDEX_NONE;
	};

	// A prefix, game-side. Its Phase is Pending until its begin job has run (sslm_gpu_prefix_begin or
	// begin_from, and set_schema), Prefilling while the build seat feeds it, then Ready once the build's
	// apply has run (ApplyPrefixBuild), or Faulted. A released prefix reads Invalid; its entry is
	// kept only until release apply and its last ghost dependent drain. Game thread.
	struct FGpuPrefixEntry
	{
		int64 Id = 0;
		SuperSLMPaging::FNodeId Node = SuperSLMPaging::InvalidNode;
		ESuperSLMPrefixPhase Phase = ESuperSLMPrefixPhase::Pending;
		TArray<int32> Tokens;      // this prefix's own tokens (a nested child's: the child's alone)
		int32 Kind = 0;            // sslm_gpu_prefix_prefill's kind: 0 prompt, 1 schema content
		int64 ParentId = 0;        // a nested child's parent; 0 for a root
		int64 Length = 0;          // the engine's length once built: the parent's length + Tokens.Num()
		bool bReady = false;       // the engine's ready_for_logits after the build (§3.4 "The armed flag")
		SuperSLMPaging::FTicket BuildTicket = SuperSLMPaging::InvalidTicket;
		int64 HoldId = 0;          // HoldApply(PrefixBuild or NestedBuild): holds the build's apply
		TSharedPtr<FJob> BeginJob;
		TSharedPtr<FPrefixJobState> BeginState;
		int32 TurnsPlanned = 0;
		int32 BuiltTokens = 0;     // turns applied (§3.9 PrefixBuiltTokens())
		#if WITH_DEV_AUTOMATION_TESTS
		TArray<int64> TurnTicks;   // dev observation; no per-turn history in Shipping
		#endif
		TArray<FPrefixTurn> TurnsInFlight;
		bool bBuildFaulted = false;
		FString FaultMessage;
		bool bReleased = false;
		bool bReleaseApplied = false;
		SuperSLMPaging::FTicket ReleaseTicket = SuperSLMPaging::InvalidTicket;
		TSharedPtr<FJob> ReleaseJob;
		int64 ReleaseHoldId = 0;
	};
	TMap<int64, FGpuPrefixEntry> Prefixes;
	// The build seat's queue, in arrival order (§3.4: "one build seat per subsystem; further builds
	// queue in arrival order"). Its head is the prefix the seat feeds.
	TArray<int64> BuildQueue;

	// Plan §3.2 N2 of the K6 review (K7): GetKvPageStats()' snapshot is the last applied paged job's
	// sslm_gpu_kv_pool_stats. The jobs write the shadow under its lock on the submission thread; each
	// tick's admission pass copies it to LastPoolStats on the game thread.
	FCriticalSection PoolStatsLock;
	SslmGpuKvPoolStats PoolStatsShadow = {};
	bool bPoolStatsShadowFresh = false;

	// Submission thread: reads the pool's stats into the shadow (a diagnostic; no verdict reads it).
	void SnapshotPoolStatsOnThread()
	{
		if (Pool == nullptr)
		{
			return;
		}
		SslmGpuKvPoolStats Stats = {};
		Stats.struct_size = static_cast<uint32_t>(sizeof(Stats));
		const FGpuStatus St = sslm_gpu_kv_pool_stats(Pool, &Stats);
		NoteLayer1Status(St);
		if (St == FGpuStatus::SSLM_OK)
		{
			FScopeLock Lock(&PoolStatsLock);
			PoolStatsShadow = Stats;
			bPoolStatsShadowFresh = true;
		}
	}

	// Game thread: the latest shadow becomes GetKvPageStats()' snapshot.
	void TakePoolStatsSnapshot()
	{
		FScopeLock Lock(&PoolStatsLock);
		if (bPoolStatsShadowFresh)
		{
			LastPoolStats = PoolStatsShadow;
			bHasPoolSnapshot = true;
			bPoolStatsShadowFresh = false;
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	// §3.9 SetNextLayer1StatusOverride() for the K7 kinds and ArmAdoptFaultAfterShare(): one-shot
	// statuses written on the game thread and taken once on the submission thread with an exchange
	// back to SSLM_OK (0), the unset value, as TestNextResetStatus is.
	std::atomic<uint32> TestNextAdoptStatus{0};
	std::atomic<uint32> TestNextPrefixBuildStatus{0};
	std::atomic<uint32> TestNextNestedBuildStatus{0};
	std::atomic<uint32> TestAdoptFaultAfterShare{0};
#endif

	// Test overrides read on the submission thread: the armed one-shot status, or SSLM_OK when unset
	// (and in every build without the seams).
	static FGpuStatus TakeTestStatus(std::atomic<uint32>* Slot)
	{
		return Slot != nullptr ? static_cast<FGpuStatus>(Slot->exchange(0u, std::memory_order_acq_rel)) : FGpuStatus::SSLM_OK;
	}

	// Plan §2.5 row 1 (D-SLM7662): the absolute shader directory Configure() passed to Layer 1 as
	// GpuContextConfig::shader_dir, the same directory the staging check read.
	FString ShaderDirectory;

	// Plan §2.5 row 3 (D-SLM7664): whether the model is mapped with the device-resident head, and
	// the reason when it is not (the asset turned it off, or an out-of-memory map fell back).
	bool bDeviceHeadActive = false;
	FString DeviceHeadStatus;

	// Diagnostic only (R-S1l (j), U1 round 3): the statuses ReleaseLayer1() received, written on
	// the submission thread and read after the teardown job has been waited on.
	FSuperSLMGpuTeardownStatuses TeardownRecord;

	// --- Layer-1 handles (touched on the submission thread) ---
	SslmGpuContext* Ctx = nullptr;
	SslmGpuModelHandle* Model = nullptr;
	TArray<FThreadSlot> ThreadSlots;
	TUniquePtr<std::atomic<uint32>[]> LiveGen;
	Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter3;

	// The plugin's own parse of the artifact's SchemaMasks section, the SAME reader Layer 1 uses
	// (superslm::SchemaMasksTable): schema names for SetSchema() and restore, and the SCM1 byte
	// count the declared residency includes. The dead end itself is Layer 1's own -2 (1.6.0).
	std::vector<uint8_t> SchemaSectionBytes;
	superslm::SchemaMasksTable Schemas;

	// --- Game-thread state ---
	TArray<FGameSlot> Slots;
	TMap<int64, int32> IdToSlot;
	int64 NextSequenceId = 1;
	TMap<int64, FAdapterEntry> Adapters;
	int64 NextAdapterId = 1;
	int64 TickIndex = 0;
	double SimSeconds = 0.0;
	// Ruling 2026-09-26 (finding 14): each rotation's persistent cursor -- the slot index it served
	// first on its last tick that served anything (INDEX_NONE before the first). A tick iterates
	// its due slots cyclically from the first slot index after the cursor, so membership changes
	// (holds, the cap, lifecycle ops) never shift the rotation's base.
	int32 OneCallCursor = INDEX_NONE;
	int32 ComposedCursor = INDEX_NONE;
	bool bActive = false;
	int32 GameHitches = 0;

	// --- The per-sequence software queue: subsystem-wide bookkeeping (D-SLM7421/D-SLM7428/
	// D-SLM7457) ---
	int64 NextLifecycleOpHandleId = 1;
	// The single counter D-SLM7428 names: incremented once per Request*() call, at that call,
	// never revised afterward -- the cross-sequence admission-order key.
	int64 NextGlobalRequestOrdinal = 1;
	// Restore has no target sequence (D-SLM7457 ruling 2), so it queues at the subsystem level
	// rather than in any FGameSlot::OpLog -- the same logical-front-index shape, compacted
	// behind the front.
	TArray<FLifecycleOpEntry> RestoreOps;
	int32 RestoreOpsFront = 0;
	// GetLastLifecycleRequestError()'s own backing store -- see USuperSLMGpuSubsystem's own doc
	// comment (SuperSLMGpuSubsystem.h) for why this exists instead of an OutError parameter.
	FString LastLifecycleRequestError;
	// The subsystem-wide resolution-order counter (maintainer ruling superseding D-SLM7480, T-2826
	// round 8): read-only telemetry that never feeds a decode or the schedule -- its only reader is
	// USuperSLMGpuSubsystem::GetLifecycleOpResolutionOrdinal(). One counter across every sequence's
	// own OpLog and the subsystem-level RestoreOps queue, so cross-sequence resolution order is
	// readable too, matching NextGlobalRequestOrdinal's own subsystem-wide shape above (that one
	// keys ARRIVAL order; this one keys RESOLUTION order, a materially different quantity -- a
	// later-arriving op on an idle sequence can resolve before an earlier-arriving one still
	// waiting on Layer 1). Always touched on the game thread (every FLifecycleOpEntry::Result
	// publish site runs inside Tick()'s own AdmitQueuedOps()/ApplyDue() call, or synchronously
	// inside a Request*() call on the same thread), so it is a plain counter, not atomic.
	int64 NextResolutionOrdinal = 1;
	// The ONLY place any FLifecycleOpEntry::Result is ever set (D-SLM7481) -- every call site in
	// this file routes through this, so the two fields can never drift apart: a caller observing
	// Result != Pending is a caller for whom ResolutionOrdinal is already valid.
	void ResolveEntry(FLifecycleOpEntry& Entry, ESuperSLMRestoreResult NewResult)
	{
		Entry.Result = NewResult;
		Entry.ResolutionOrdinal = NextResolutionOrdinal++;
		// Row 19 (D-SLM7763): a restore resolved before its job took the blob (a refusal at
		// admission) hands its share to the submission thread, so its release never runs here.
		if (Entry.RestoreBlob.IsValid())
		{
			ReleaseBlobOffGameThread(MoveTemp(Entry.RestoreBlob));
		}
		// Row 19: a save refused after its job ran still holds the raw Layer-1 bytes (a successful
		// save's assembly already released them on the submission thread).
		if (Entry.Kind == ELifecycleOpKind::Save && Entry.Async.IsValid() && Entry.Async->Layer1Bytes.Num() > 0)
		{
			ReleaseBytesOffGameThread(MoveTemp(Entry.Async->Layer1Bytes));
			Entry.Async->Layer1Bytes.Reset();
		}
		// Row 19: the handle's result entry, the caller's resource until ReleaseLifecycleOpHandle()
		// or TearDown(). SchemaBind entries are internal (SetSchema() returns no handle).
		if (Entry.Kind != ELifecycleOpKind::SchemaBind && Entry.HandleId != 0)
		{
			FGpuHandleResult& Out = HandleResults.FindOrAdd(Entry.HandleId);
			Out.Result = NewResult;
			Out.ResolutionOrdinal = Entry.ResolutionOrdinal;
			Out.Kind = Entry.Kind;
			if (Entry.Kind == ELifecycleOpKind::Restore && NewResult == ESuperSLMRestoreResult::Success)
			{
				Out.RestoreOutSequence = Entry.RestoreOutSequence;
			}
		}
	}

	// Row 19 (D-SLM7763): one handle's resolved result. The save blob lives here from the save's
	// resolution until the first successful GetSaveResult() moves it out.
	struct FGpuHandleResult
	{
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		int64 ResolutionOrdinal = -1;
		ELifecycleOpKind Kind = ELifecycleOpKind::Reset;
		TArray<uint8> SaveBlob;
		bool bConsumed = false;
		bool bHasSaveCounts = false;
		int32 ObservedAtSave = -1;
		int32 ProducedAtSave = -1;
		int64 BindTagAtSave = 0;
		FSuperSLMGpuSequence RestoreOutSequence;
	};
	TMap<int64, FGpuHandleResult> HandleResults;
	// The pooled-resource monitor's retention reading: save-blob bytes held in HandleResults and
	// not yet taken. Kept exact at every move in and out.
	int64 RetainedResultBytes = 0;

	// Row 19 (D-SLM7763): drops a blob share on the submission thread, through a job that makes no
	// Layer-1 call; the job's closure is released there after it runs (Run()). With no thread, or
	// an unresponsive one, the share drops here -- only on those degenerate paths.
	void ReleaseBlobOffGameThread(TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>&& Blob)
	{
		// Round 5: also dropped here in the teardown window (bTeardownPending, bStopThread), where a
		// job enqueued now might never be taken by the stopping thread.
		if (!Blob.IsValid() || Thread == nullptr || bUnresponsive.load() || bTeardownPending || bStopThread.load())
		{
			Blob.Reset();
			return;
		}
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->CallKind = TEXT("blob release (no Layer-1 call)");
		Job->bTakesLayer1Lock = false;
		Job->Custom = [Held = MoveTemp(Blob)]() mutable { Held.Reset(); };
		Enqueue(Job);
	}

	// The same, for bytes held by value (a refused save's raw Layer-1 payload).
	void ReleaseBytesOffGameThread(TArray<uint8>&& Bytes)
	{
		if (Bytes.Num() == 0 || Thread == nullptr || bUnresponsive.load() || bTeardownPending || bStopThread.load())
		{
			Bytes.Empty(); // round 5: in the teardown window too
			return;
		}
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->CallKind = TEXT("blob release (no Layer-1 call)");
		Job->bTakesLayer1Lock = false;
		Job->Custom = [Held = MoveTemp(Bytes)]() mutable { Held.Empty(); };
		Enqueue(Job);
	}

	// Plan §2.5 row 16: the recycles (unbinding resets no caller holds a handle to) whose
	// statuses are still to be read; AdmitQueuedOps() withholds the slot on any refusal.
	struct FPendingRecycle
	{
		int32 SlotIndex = INDEX_NONE;
		TSharedPtr<FLifecycleAsyncState> Async;
		// Pooled mode (K6): the recycle's page-ledger reset ticket, applied when its statuses are
		// read, and the HoldApply(Return) hold its apply carries.
		SuperSLMPaging::FTicket LedgerTicket = SuperSLMPaging::InvalidTicket;
		int64 HoldId = 0;
		// K6 review C1, fix (b): false while the page ledger has not yet admitted this recycle's reset,
		// because the slot's previous ledger job (an earlier recycle) was still open when it was
		// queued. PollRecycles() admits it, in queue order, once that job has applied.
		bool bLedgerAdmitted = true;
	};
	TArray<FPendingRecycle> PendingRecycles;

	// --- Written on the submission thread, read on the game thread ---
	std::atomic<int64> DispatchCount{0};
	std::atomic<int32> ThreadHitches{0};
	std::atomic<bool> bTerminalLoss{false};
	// Plan §2.5 row 21 (D-SLM7703, restated by D-SLM7762): set on the game thread when the Layer-1
	// call a wait depends on has overrun its own bound. From then on no job is enqueued, the backend takes
	// the terminal device-loss path, and teardown neither joins the thread nor frees the device
	// objects or this state while a Layer-1 call may still be running on it.
	std::atomic<bool> bUnresponsive{false};
	FString UnresponsiveReason;
	// Diagnostic only (U1 round 6): the job whose wait expired, so its late return is logged once.
	std::atomic<uint64> AbandonedJobSeq{0};
	// Ruling 2026-09-26: the job this subsystem's submission thread has taken and not yet marked
	// done (0 when none), written by Run(). The tick's holder check names it as the abandoned job
	// when the overrunning holder is another subsystem's call, since no waiter job exists then.
	std::atomic<uint64> CurrentJobSeq{0};
	// Ruling 2026-09-26: jobs enqueued and not yet done, split by kind. Enqueue() increments the
	// job's counter under QueueLock only when it actually queues the job; Run() decrements it
	// immediately before it stores bDone. OutstandingTickJobs is the global cap's measure
	// (PlanAndIssue(), NextTickGatedOnDevice()); the sum gates Tick()'s holder check.
	std::atomic<int32> OutstandingTickJobs{0};
	std::atomic<int32> OutstandingOtherJobs{0};
	std::atomic<double> LastGpuBusyMs{0.0};
	FCriticalSection TimingLock;
	FSuperSLMGpuTimingSnapshot CompletedTiming; // TimingLock protects worker publication
	FSuperSLMGpuTimingSnapshot GameTiming;      // game thread only, published once per Tick
	std::atomic<double> LastHostFinishMs{0.0};

#if WITH_DEV_AUTOMATION_TESTS
	// Test access only (ruling 2026-09-26, SuperSLMGpuTestAccess.h); unused, none of it changes
	// anything. All game-thread only: TestSubmissionDelaySeconds is copied into each tick job's
	// PostCallDelaySeconds when PlanAndIssue() creates it (Run() reads only the job's copy), and
	// TestTickJobs is the list of tick jobs PlanAndIssue() enqueued while logging is on, from which
	// an examination counts the unfinished tick jobs ahead of its event's job.
	double TestSubmissionDelaySeconds = 0.0;
	bool bTestLogsEnabled = false;
	TArray<FSuperSLMGpuEventLogEntry> TestEventLog;
	TArray<FSuperSLMGpuPlanLogEntry> TestPlanLog;
	TArray<TSharedPtr<FJob>> TestTickJobs;
	// Test access only (SetNextResetStatusOverride(), SetNextRestoreStatusOverride()): a one-shot
	// status the next sslm_gpu_seq_reset / sslm_gpu_seq_restore call on the submission thread
	// returns instead of calling Layer 1. Written on the game thread, taken once on the
	// submission thread with an exchange back to SSLM_OK (0), which is also the unset value, so
	// an unset override costs one atomic exchange and changes nothing.
	std::atomic<uint32> TestNextResetStatus{0};
	std::atomic<uint32> TestNextRestoreStatus{0};
#endif

	// --- The submission thread ---
	FRunnableThread* Thread = nullptr;
	FEvent* WorkEvent = nullptr;
	FEvent* DoneEvent = nullptr;
	FCriticalSection QueueLock;
	TArray<TSharedPtr<FJob>> Queue;
	uint64 NextJobSeq = 1;
	std::atomic<bool> bStopThread{false};
	// Round 3 (finding 6): stored true by Run() immediately before it returns, and reset by
	// StartThread(). FinishTeardownIfDone() joins the thread only once it reads true, so the join
	// never waits on a busy thread.
	std::atomic<bool> bThreadExited{false};
	// Round 3 (finding 7): the tick job PlanAndIssue() last armed with bHoldAfterCall, so
	// HoldNextTickJob() refuses while it is still outstanding. Game thread.
	TSharedPtr<FJob> TestHeldJob;
	// Ruling 2026-09-26, round 2: the test-released gate a bHoldAfterCall job waits on. Manual
	// reset: HoldNextTickJob() resets it, ReleaseHeldTickJob() triggers it, and every teardown path
	// (Shutdown(), BeginTeardownAfterLoss(), StopThread(), so a re-Configure() too) triggers it, so
	// no teardown ever waits on a test gate. Pooled in StartThread(), returned in StopThread().
	FEvent* TestGateEvent = nullptr;
	// Game thread: HoldNextTickJob() was called and PlanAndIssue() has not yet enqueued the tick
	// job that carries the hold.
	bool bTestHoldNextTickJob = false;
	// Ruling 2026-09-26, round 2: the teardown BeginTeardownAfterLoss() enqueued on a confirmed
	// terminal loss, finished by a later Tick() once it reads done (or by Shutdown()). Game thread.
	bool bTeardownPending = false;
	TSharedPtr<FJob> TeardownJob;

	// ------------------------------------------------------------------ thread plumbing

	bool StartThread()
	{
		WorkEvent = FPlatformProcess::GetSynchEventFromPool(false);
		DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
		TestGateEvent = FPlatformProcess::GetSynchEventFromPool(true); // manual reset
		bThreadExited = false;
		Thread = FRunnableThread::Create(this, TEXT("SuperSLM GPU Submission"), 0, TPri_AboveNormal);
		return Thread != nullptr;
	}

	// Opens the test-released gate for good (until a HoldNextTickJob() resets it), so a held job
	// finishes. Every teardown path calls it first. Any thread.
	void ReleaseTestGate()
	{
		if (TestGateEvent != nullptr)
		{
			TestGateEvent->Trigger();
		}
	}

	void StopThread()
	{
		ReleaseTestGate();
		if (Thread != nullptr)
		{
			bStopThread = true;
			WorkEvent->Trigger();
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}
		if (TestGateEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(TestGateEvent);
			TestGateEvent = nullptr;
		}
		if (WorkEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkEvent);
			WorkEvent = nullptr;
		}
		if (DoneEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
			DoneEvent = nullptr;
		}
	}

	virtual uint32 Run() override
	{
		while (true)
		{
			TSharedPtr<FJob> Job;
			{
				FScopeLock Lock(&QueueLock);
				if (Queue.Num() > 0)
				{
					Job = Queue[0];
					Queue.RemoveAt(0, 1, EAllowShrinking::No);
				}
			}
			if (!Job.IsValid())
			{
				if (bStopThread)
				{
					bThreadExited = true; // round 3: the join that follows returns at once
					return 0;
				}
				WorkEvent->Wait(10);
				continue;
			}
			CurrentJobSeq.store(Job->Seq); // ruling 2026-09-26: the job this thread has taken
			if ((bTerminalLoss.load() || bUnresponsive.load()) && !Job->bRunsAfterLoss)
			{
				// Round 2, "Terminal loss never waits": after a confirmed loss no queued job touches
				// the lost device. It completes at once, every result Skipped: no lock, no delay, no
				// gate.
				CompleteWithoutRunning(*Job);
				CurrentJobSeq.store(0);
				if (DoneEvent != nullptr)
				{
					DoneEvent->Trigger();
				}
				continue;
			}
			const bool bLayer1 = Job->bTakesLayer1Lock;
			if (bLayer1)
			{
				Layer1GpuLock().Lock();
			}
			Job->StartedWallSeconds = FPlatformTime::Seconds();
			if (bLayer1)
			{
				// Row 21 (D-SLM7762): the holder record every waiter reads.
				FScopeLock HolderLock(&Layer1LockHolderGuard());
				FLayer1LockHolder& Holder = Layer1LockHolder();
				Holder.bHeld = true;
				Holder.Owner = this;
				Holder.Seq = Job->Seq;
				Holder.Kind = Job->CallKind;
				Holder.StartSeconds = Job->StartedWallSeconds;
				Holder.BoundSeconds = Job->BoundSeconds;
			}
			if (Job->Custom)
			{
				Job->Custom();
			}
			else
			{
				ExecuteTickJob(*Job);
			}
			if (bLayer1)
			{
				{
					FScopeLock HolderLock(&Layer1LockHolderGuard());
					Layer1LockHolder().bHeld = false;
				}
				Layer1GpuLock().Unlock();
			}
			// Round 5 (review Low 3): the call's own end, before any test delay or gate, so the
			// duration log's "in the call" is the Layer-1 call alone.
			Job->CallEndWallSeconds = FPlatformTime::Seconds();
			// Test access only (ruling 2026-09-26, SetSubmissionDelaySeconds()): a slowed device.
			// The sleep follows the job's Layer-1 work, after the Layer-1 lock and the holder record
			// are released, so it never counts against a row-21 bound, and precedes
			// CompletedWallSeconds and bDone, so the game thread sees the job finish that much
			// later. Only a tick job carries a non-zero delay (PlanAndIssue() copies it at creation).
			if (Job->PostCallDelaySeconds > 0.0)
			{
				FPlatformProcess::Sleep(static_cast<float>(Job->PostCallDelaySeconds));
			}
			// Test access only (round 2, HoldNextTickJob()): the test-released gate, also outside the
			// lock and the holder record, so it never counts against a row-21 bound either.
			if (Job->bHoldAfterCall && TestGateEvent != nullptr)
			{
				TestGateEvent->Wait(MAX_uint32);
			}
			Job->CompletedWallSeconds = FPlatformTime::Seconds();
			LogJobDuration(*Job);
			{
				// Row 19 (D-SLM7763): the closure and everything it owns (a restore blob, a save's
				// Layer-1 bytes) is released here, on this thread, and the job no longer keeps its
				// async state alive through it.
				TUniqueFunction<void()> Spent = MoveTemp(Job->Custom);
			}
			// Ruling 2026-09-26: the outstanding counter drops immediately before bDone, so a reader
			// that sees a job done never counts it outstanding; the job is then no longer current.
			(Job->bTickJob ? OutstandingTickJobs : OutstandingOtherJobs).fetch_sub(1);
			CurrentJobSeq.store(0);
			Job->bDone.store(true, std::memory_order_release);
			DoneEvent->Trigger();
		}
	}

	// Round 2, "Terminal loss never waits": completes Job without executing it -- every result
	// Skipped, its completion time set, its closure released, its outstanding counter decremented
	// immediately before bDone, as Run() does for a job it executes. For a job that was counted
	// (queued); Run()'s skip after a loss and MarkUnresponsive()'s queue completion call it.
	void CompleteWithoutRunning(FJob& Job)
	{
		Job.Results.SetNum(Job.Actions.Num());
		Job.CompletedWallSeconds = FPlatformTime::Seconds();
		Job.bSkipped = true; // round 3: never reads as success
		{
			TUniqueFunction<void()> Spent = MoveTemp(Job.Custom);
		}
		(Job.bTickJob ? OutstandingTickJobs : OutstandingOtherJobs).fetch_sub(1);
		Job.bDone.store(true, std::memory_order_release);
	}

	// Round 4 (finding 1): the one text every lifecycle finalize gives a job that was completed
	// without running, which then resolves NotConfigured and stores nothing.
	static const TCHAR* LostBeforeRanText()
	{
		return TEXT("the GPU backend was lost before this operation ran");
	}

	// Round 4 (finding 1): true when Job was completed without running (bSkipped). Every lifecycle
	// finalize asks this before reading any output the job would have written. Game thread, after
	// reading Job's bDone true (bSkipped is written before that release store).
	static bool JobWasSkipped(const TSharedPtr<FJob>& Job)
	{
		return Job.IsValid() && Job->bSkipped;
	}

	TSharedPtr<FJob> Enqueue(TSharedPtr<FJob> Job)
	{
		if (bUnresponsive.load())
		{
			// Row 21: no job is enqueued once the thread is unresponsive. The job reads as done
			// with every action Skipped, so a poller never waits on work that will not run.
			Job->Results.SetNum(Job->Actions.Num());
			Job->CompletedWallSeconds = FPlatformTime::Seconds();
			Job->bSkipped = true; // round 3
			Job->bDone.store(true, std::memory_order_release);
			return Job;
		}
		{
			FScopeLock Lock(&QueueLock);
			Job->Seq = NextJobSeq++;
			Job->EnqueuedWallSeconds = FPlatformTime::Seconds();
			// Ruling 2026-09-26: counted only when actually queued (not on the unresponsive return
			// above), and under QueueLock, so Run() cannot take and finish the job first.
			(Job->bTickJob ? OutstandingTickJobs : OutstandingOtherJobs).fetch_add(1);
			Queue.Add(Job);
		}
		WorkEvent->Trigger();
		return Job;
	}

	// Plan §2.5 row 21 as restated by D-SLM7762: the lock-holder check. What is timed is the
	// Layer-1 call currently holding the process-wide lock, from its own start, against its own
	// bound -- never a wait, and never queue or lock waiting. When that holder -- this subsystem's
	// call, or another subsystem's that WaitingJobSeq's job waits behind -- has overrun, the thread
	// is marked unresponsive and the backend takes its terminal device-loss path (FallBackToCpu() at
	// the next Tick(), or this one). Returns true when it marked the thread unresponsive. Game
	// thread. Reads the holder record under Layer1LockHolderGuard() (the submission thread writes it
	// under the same guard, Run()); nothing else. Shared by Wait() and, since the ruling of
	// 2026-09-26, by Tick() at the start of every tick with any job outstanding.
	// WaitingJobSeq names the abandoned job when the holder is another subsystem's call: Wait()
	// passes its own job. Tick() has no waiter job and passes 0, and then the job this subsystem's
	// thread has taken and is blocked on the lock with (CurrentJobSeq) is named, or, when that is 0,
	// the front of this subsystem's queue (read under QueueLock).
	bool CheckLockHolderOverrun(uint64 WaitingJobSeq)
	{
		FLayer1LockHolder Holder;
		{
			FScopeLock HolderLock(&Layer1LockHolderGuard());
			Holder = Layer1LockHolder();
		}
		if (!Holder.bHeld)
		{
			return false;
		}
		const double InCallSeconds = FPlatformTime::Seconds() - Holder.StartSeconds;
		if (InCallSeconds <= Holder.BoundSeconds)
		{
			return false;
		}
		const bool bOwn = Holder.Owner == this;
		uint64 Abandoned = bOwn ? Holder.Seq : WaitingJobSeq;
		if (Abandoned == 0)
		{
			Abandoned = CurrentJobSeq.load();
			if (Abandoned == 0)
			{
				FScopeLock Lock(&QueueLock);
				Abandoned = Queue.Num() > 0 ? Queue[0]->Seq : 0;
			}
		}
		AbandonedJobSeq.store(Abandoned);
		MarkUnresponsive(FString::Printf(TEXT("a Layer-1 GPU %s did not return within its %.1f s bound (submission job %llu of %s, %.1f s inside the call)"),
			Holder.Kind, Holder.BoundSeconds, Holder.Seq,
			bOwn ? TEXT("this GPU subsystem") : TEXT("another GPU subsystem, which this job waits behind"), InCallSeconds));
		return true;
	}

	// Plan §2.5 row 21 as restated by D-SLM7762: the game thread's wait on one submission-thread
	// job, now reached only through RunSync() -- Configure(), adapter map and unmap, the probe and
	// teardown (ruling 2026-09-26: Tick(), SetSchema() and GetSchemaWalkState() never wait). The
	// wait itself is untimed; each pass runs CheckLockHolderOverrun(), and on an overrun the game
	// thread stops waiting. Returns true only when the job finished.
	bool Wait(const FJob& Job)
	{
		if (Job.bDone.load(std::memory_order_acquire))
		{
			return true;
		}
		if (bUnresponsive.load())
		{
			return false;
		}
		while (!Job.bDone.load(std::memory_order_acquire))
		{
			if (CheckLockHolderOverrun(Job.Seq))
			{
				return false;
			}
			DoneEvent->Wait(1);
		}
		return true;
	}

	// Diagnostic only (U1 round 6, kept by D-SLM7762): the source of kMeasuredMapBytesPerSecond
	// (D-SLM7764), and of any slower reading that replaces it. The first full run hit the old wait-timed bound twice (a 1.5B map inside
	// Configure(), and a backlog) without the log recording how long either call really took. This
	// records it, with each job's call kind and bound: any job whose own Layer-1
	// work ran a second or more is logged with its queue, lock and call times, and the job whose
	// wait expired is logged when it finally returns -- the evidence the bound's ruling needs.
	// Queue time alone never logs, so a backlog of short jobs stays quiet.
	// Runs on the submission thread; the state outlives an abandoned thread (row 21 leaks it).
	void LogJobDuration(const FJob& Job) const
	{
		const double TotalSeconds = Job.CompletedWallSeconds - Job.EnqueuedWallSeconds;
		// Round 5 (review Low 3): "in the call" and the 1 s threshold are the Layer-1 call alone
		// (Started to CallEnd); a test delay or gate after it is reported separately.
		const double CallSeconds = Job.CallEndWallSeconds - Job.StartedWallSeconds;
		const double TestHoldSeconds = FMath::Max(0.0, Job.CompletedWallSeconds - Job.CallEndWallSeconds);
		const bool bAbandoned = Job.Seq != 0 && Job.Seq == AbandonedJobSeq.load();
		if (CallSeconds < 1.0 && !bAbandoned)
		{
			return;
		}
		const FString TestHold = TestHoldSeconds >= 0.0005
			? FString::Printf(TEXT(", %.3f s test hold (delay and gate)"), TestHoldSeconds)
			: FString();
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM GPU submission job %llu (%s, bound %.1f s, %lld bytes) %s after %.3f s: %.3f s queued or waiting for the Layer-1 lock, %.3f s in the call%s."),
			Job.Seq, Job.CallKind, Job.BoundSeconds, Job.LoadBytes,
			bAbandoned ? TEXT("returned, after the game thread had stopped waiting,") : TEXT("finished"),
			TotalSeconds, Job.StartedWallSeconds - Job.EnqueuedWallSeconds, CallSeconds, *TestHold);
	}

	void MarkUnresponsive(const FString& Reason)
	{
		if (!bUnresponsive.exchange(true))
		{
			UnresponsiveReason = Reason;
			bTerminalLoss = true;
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM GPU backend: %s; the submission thread is treated as unresponsive and the backend takes its terminal device-loss path."), *Reason);
			// Round 2, "Terminal loss never waits": the thread may never take the jobs still
			// queued, so every one is completed here, Skipped, and the queue emptied. The job the
			// thread is inside, if any, is not in the queue and counts until it returns.
			FScopeLock Lock(&QueueLock);
			for (const TSharedPtr<FJob>& Queued : Queue)
			{
				CompleteWithoutRunning(*Queued);
			}
			Queue.Reset();
			if (DoneEvent != nullptr)
			{
				DoneEvent->Trigger();
			}
		}
	}

	// Runs Fn on the submission thread after every job already queued, and waits for it (row 21:
	// the call is timed from its own start against BoundSeconds; CallKind names it in the message).
	// Returns false when a call overran its bound, the thread is already unresponsive, or the job was
	// skipped because a terminal loss had been confirmed (round 3). Fn may
	// then still be running, or run later, on the submission thread: it must capture its outputs
	// through shared state, never a reference into the caller's stack.
	bool RunSync(TUniqueFunction<void()> Fn, const TCHAR* CallKind = TEXT("frame-path call"), double BoundSeconds = kFramePathBoundSeconds, int64 LoadBytes = 0)
	{
		if (bUnresponsive.load())
		{
			return false;
		}
		// Round 3 (finding 7): the test gate never holds a RunSync(). A held tick job ahead of this
		// one on the FIFO is released first, whatever order the caller's body takes. Round 5: a hold
		// request not yet armed is cancelled too, not passed through unheld; the caller re-arms.
		ReleaseTestGate();
		bTestHoldNextTickJob = false;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = MoveTemp(Fn);
		Job->CallKind = CallKind;
		Job->BoundSeconds = BoundSeconds;
		Job->LoadBytes = LoadBytes;
		Enqueue(Job);
		// Round 3 (finding 3): a job completed without running (after a loss, or once the thread is
		// unresponsive) never reads as success, so the caller takes its "did not respond" path.
		return Wait(*Job) && !Job->bSkipped;
	}

	// ------------------------------------------------------------------ thread-side helpers

	uint32 FullTokenBudget() const
	{
		return static_cast<uint32>(NumHiddenLayers) * DispatchesPerLayer;
	}

	void CountSubmission(int64 N = 1)
	{
		DispatchCount.fetch_add(N);
	}

	void RecordGpuBusy()
	{
		const double Busy = superslm_gpu::LastCallTiming().gpu_busy_ms;
		LastGpuBusyMs.store(Busy);
		{
			FScopeLock Guard(&TimingLock);
			CompletedTiming = { DispatchCount.load(), Busy };
		}
		TRACE_COUNTER_SET(SuperSLMGpuBusyMs, Busy);
		if (Busy > Config.TickBudgetMs)
		{
			ThreadHitches.fetch_add(1);
			TRACE_BOOKMARK(TEXT("SuperSLM: GPU slice hitch %.2f ms over a %.2f ms budget"), Busy, Config.TickBudgetMs);
			// No TRACE_COUNTER_INCREMENT here: the counter's value is a plain int64, and Tick()
			// publishes the whole total from the game thread, so this thread only bumps the atomic.
		}
	}

	// The plan's "probe the context once" (§5): the dispatch device Layer 1 decodes on must not
	// report removal, and a scratch sequence must embed, run one layer, and drain cleanly.
	// Keyed on SuperSLM 1.8.0's contract (plan §10.3.1 item 8):
	//  - SSLM_GPU_ALLOCATION_FAILED from the probe's own calls states that the device and the
	//    context stay usable (gpu_1p0.h, beside that status), so it is read as recoverable, never
	//    as a terminal loss, provided the device still does not report removal.
	//  - That reading is inconclusive, not a proof of usability, when seq_create itself fails with
	//    SSLM_GPU_ALLOCATION_FAILED: seq_create uploads on the context's own device (used only for
	//    residency uploads), while decode and prefill submit on harness::GetDevice()'s list, so the
	//    probe returns before it has touched the submission list it exists to test. It is treated as
	//    recoverable anyway: a stranded submission that coincides with memory pressure is re-probed
	//    on the next SSLM_DEVICE_LOST, once memory is free, whereas reading it terminal would tear a
	//    healthy backend down permanently on a transient out-of-memory.
	//  - A submission device that is not available is read by why its setup failed: one that ran
	//    out of memory is set up again by the next call (d3d12_harness.h, GetDevice()), and any
	//    other cause is final (SSLM_DEVICE_LOST cause (d)). Defensive: State is published only after
	//    Configure()'s warm-up has set the device up, and `available` is never cleared once set.
	bool ProbeUsable()
	{
		superslm_gpu::harness::Device& Dev = superslm_gpu::harness::GetDevice(); // noexcept at 1.8.0
		if (!Dev.available)
		{
			return Dev.setup_failure.load() == superslm_gpu::harness::SetupFailure::Allocation;
		}
		if (Dev.dev && FAILED(Dev.dev->GetDeviceRemovedReason()))
		{
			return false;
		}
		SslmGpuSequenceHandle* Probe = nullptr;
		FGpuStatus St = sslm_gpu_seq_create(Ctx, Model, 2, &Probe);
		NoteLayer1Status(St);
		if (St != FGpuStatus::SSLM_OK || Probe == nullptr)
		{
			return St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED && (!Dev.dev || SUCCEEDED(Dev.dev->GetDeviceRemovedReason()));
		}
		bool bOk = false;
		St = sslm_gpu_seq_embed_token(Ctx, Probe, 0);
		NoteLayer1Status(St);
		if (St == FGpuStatus::SSLM_OK)
		{
			CountSubmission();
			St = sslm_decode_step_gpu(Ctx, Probe, nullptr, DispatchesPerLayer);
			NoteLayer1Status(St);
		}
		if (St == FGpuStatus::SSLM_OK)
		{
			int32_t Ready = 0;
			FGpuStatus Drained = FGpuStatus::SSLM_OK;
			St = sslm_gpu_ready(Ctx, Probe, 1, &Ready, &Drained);
			NoteLayer1Status(St);
			NoteLayer1Status(Drained);
			if (St == FGpuStatus::SSLM_OK)
			{
				bOk = Ready == 1 && (Drained == FGpuStatus::SSLM_OK || Drained == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED);
			}
		}
		if (St != FGpuStatus::SSLM_OK)
		{
			bOk = St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED;
		}
		NoteLayer1Status(sslm_gpu_seq_release(Ctx, Probe));
		return bOk && (!Dev.dev || SUCCEEDED(Dev.dev->GetDeviceRemovedReason()));
	}

	// Only SSLM_DEVICE_LOST is probed. Every other status, SSLM_GPU_ALLOCATION_FAILED included, is
	// recoverable per call by 1.8.0's contract and faults only its own sequence; probing it would
	// allocate under the same memory pressure. SSLM_DEVICE_LOST still includes a healthy
	// cap-saturation rejection, which is why the probe decides it (plan §10.3.1 item 8).
	ESuperSLMGpuFaultReason Classify(FGpuStatus Status)
	{
		if (Status != FGpuStatus::SSLM_DEVICE_LOST)
		{
			return ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection;
		}
		if (ProbeUsable())
		{
			return ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection;
		}
		bTerminalLoss = true;
		return ESuperSLMGpuFaultReason::TerminalDeviceLost;
	}

	void FaultResult(FThreadSlot& TS, FActionResult& R, FGpuStatus Status, const TCHAR* Where)
	{
		NoteLayer1Status(Status);
		R.Kind = EResultKind::Fault;
		R.Message = FString::Printf(TEXT("%s returned %s"), Where, *GpuStatusText(Status));
		if (Status == FGpuStatus::SSLM_GPU_KV_BUDGET_EXCEEDED)
		{
			// Plugin paging plan §3.4, §3.7 (K6): the engine's own budget enforcement stopped a pooled
			// holder. The plugin's per-step pre-check (ContextHasRoom()) stops every holder at its
			// limit first, so reaching this is a defect of that check: named, and logged as a limit
			// fault. Never probed: the device is not in question.
			R.Fault = ESuperSLMGpuFaultReason::KvBudgetExceeded;
			UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM limit fault: %s on GPU slot generation %u (context used %lld, limit %lld); the plugin's per-step pre-check let the step past the holder's limit."),
				*R.Message, TS.Gen, TS.ContextUsed, TS.Limit);
			StopWith(TS, R);
			return;
		}
		R.Fault = Classify(Status);
		StopWith(TS, R);
	}

	// Stops the sequence on the thread; a fault is kept so every later action re-reports it.
	void StopWith(FThreadSlot& TS, const FActionResult& R)
	{
		TS.bStopped = true;
		if (R.Kind == EResultKind::Fault && !TS.bHasStopFault)
		{
			TS.bHasStopFault = true;
			TS.StopFault = R;
		}
	}

	bool ActionIsLive(const FAction& A) const
	{
		return LiveGen[A.Slot].load(std::memory_order_acquire) == A.Gen && ThreadSlots[A.Slot].Gen == A.Gen;
	}

	// True when the action should not run because its sequence already stopped on the thread; a
	// stopping fault is copied into the action's result so its event carries it to the game.
	bool SkipStopped(const FAction& A, FActionResult& R)
	{
		const FThreadSlot& TS = ThreadSlots[A.Slot];
		if (!TS.bStopped)
		{
			return false;
		}
		if (TS.bHasStopFault)
		{
			R = TS.StopFault;
		}
		return true;
	}

	// The first prompt token of a generation sets the limits the thread stops on.
	void BeginPrompt(FThreadSlot& TS, const FAction& A)
	{
		if (A.BeginLimit > 0)
		{
			TS.Limit = A.BeginLimit; // K7 review S1: the ledger's applied limit wins over the thread's
		}
		TS.MaxNewTokens = A.MaxNewTokens;
		TS.StopTokenIds = A.StopTokenIds;
		TS.Produced = 0;
		TS.bStopped = false;
		TS.bHasStopFault = false;
		TS.StopFault = FActionResult();
		TS.bPrimed = false;
	}

	// A prompt token reached full depth. After the last one the sequence is primed: its next step
	// is finish-only (D-SLM7256 rule 1). No prefill verb ran, so Layer 1's ready_for_logits flag
	// is never set and the bridge can never misread it (D-SLM7379).
	void PromptTokenDone(FThreadSlot& TS, const FAction& A, FActionResult& R)
	{
		if (A.bLastPrompt)
		{
			TS.bPrimed = true;
			R.Kind = EResultKind::PrefillOk;
		}
		else
		{
			R.Kind = EResultKind::Progress;
		}
	}

	bool BindAdapterIfRequested(FThreadSlot& TS, const FAction& A, FActionResult& R)
	{
		if (!A.bBindAdapter)
		{
			return true;
		}
		if (TS.BoundAdapter != A.Adapter)
		{
			const FGpuStatus St = sslm_gpu_seq_bind_adapter(Ctx, TS.Handle, A.Adapter);
			if (St != FGpuStatus::SSLM_OK)
			{
				FaultResult(TS, R, St, TEXT("sslm_gpu_seq_bind_adapter"));
				return false;
			}
			TS.BoundAdapter = A.Adapter;
			TRACE_BOOKMARK(TEXT("SuperSLM: GPU adapter swap (slot %d)"), A.Slot);
		}
		// Rule 4 (T-2983 finding 10): the request this action carried is now what Layer 1 has
		// bound -- whether this call bound it or an earlier carrier of the same request did.
		TS.AppliedBindTag = A.BindTag;
		return true;
	}

	// Records a produced token. Plan §2.5 row 8 (D-SLM7667): Layer 1's finish returns -2 at
	// SSLM_OK for a schema dead end (1.6.0), leaving the walk and layer_index as they were and
	// re-arming ready_for_logits; it is handled first, before the token is used anywhere, and
	// stops the sequence. Then the stop set and MaxNewTokens.
	void RecordToken(FThreadSlot& TS, FActionResult& R, int32 Token)
	{
		if (TS.SchemaIndex >= 0)
		{
			// The finish has returned, so the sequence is no longer Submitted and the bridge
			// does not answer SSLM_BUSY; a dead end leaves the walk where it was, so its reading
			// is the last produced token's. Any other status reads as not accepting.
			int32_t Accepting = 0;
			const FGpuStatus AcceptingSt = SslmGpuSeqSchemaAcceptingForG5Bridge(Ctx, TS.Handle, &Accepting);
			NoteLayer1Status(AcceptingSt); // K6 review S2: every submission-thread status is counted
			R.bSchemaAccepting = AcceptingSt == FGpuStatus::SSLM_OK && Accepting != 0;
		}
		// Ruling 2026-09-26: the walk as of this finish, beside the accepting read, carried back for
		// GetSchemaWalkState() (Unused when no schema is bound, which the bridge itself reports).
		R.bHasWalkState = true;
		R.WalkState = SslmGpuSeqWalkStateForG5Bridge(TS.Handle);
		if (Token == -2)
		{
			R.Kind = EResultKind::DeadEnd;
			R.Message = TEXT("the bound schema has no legal continuation (schema dead end)");
			TS.bStopped = true;
			return;
		}
		R.Kind = EResultKind::Token;
		R.Token = Token;
		TS.LastToken = Token;
		TS.Produced += 1;
		if (TS.Produced >= TS.MaxNewTokens || TS.StopTokenIds.Contains(Token))
		{
			TS.bStopped = true;
		}
	}

	bool ContextHasRoom(FThreadSlot& TS, FActionResult& R)
	{
		// Plugin paging plan §3.4 "Generation limit" (K6): a pooled holder's per-step pre-check
		// (engine token law E7). An embed needs ContextUsed < limit; finish-only steps never come
		// here. Below the cap the stop is KvBudgetExceeded, by name; at limit == ContextCap the
		// existing cap outcome below fires instead. Dedicated slots (Limit 0) skip it.
		// K6 review N1: a pooled holder with no limit is a plugin defect (a shape-changing job that
		// did not write it), which would silently turn the pre-check off; it fails loudly.
		ensureMsgf(!bPooled || TS.Limit > 0, TEXT("SuperSLM GPU: a pooled holder has no generation limit (FThreadSlot::Limit 0); the job that gave it its shape did not write it"));
		if (TS.Limit > 0)
		{
			const SuperSLMPaging::ELimitVerdict Verdict = SuperSLMPaging::CheckStep(TS.ContextUsed, /*bFinishOnly*/ false, TS.Limit, TS.Cap);
			if (Verdict == SuperSLMPaging::ELimitVerdict::Allowed)
			{
				return true;
			}
			if (Verdict == SuperSLMPaging::ELimitVerdict::KvBudgetExceeded)
			{
				R.Kind = EResultKind::Fault;
				R.Fault = ESuperSLMGpuFaultReason::KvBudgetExceeded;
				R.Message = FString::Printf(TEXT("the sequence has reached its own generation limit (%lld = min(origin + SequenceBudget, ContextCap)); no budgeted position is left to embed the next token"),
					TS.Limit);
				StopWith(TS, R);
				return false;
			}
		}
		if (TS.ContextUsed < TS.Cap)
		{
			return true;
		}
		R.Kind = EResultKind::Fault;
		R.Fault = ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection;
		R.Message = FString::Printf(TEXT("the sequence has used its whole context_cap (%lld); no position is left to embed the next token"), TS.Cap);
		StopWith(TS, R);
		return false;
	}

	void FinishToken(FThreadSlot& TS, FActionResult& R)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Finish", SuperSLMGpuChannel);
		int32_t Token = -1;
		// HostFinishMs is the finish call's wall time on this thread; with the device-resident
		// head it includes the logits dispatch's own synchronous submission (plan §5.1).
		const double Start = FPlatformTime::Seconds();
		const FGpuStatus St = SslmGpuSeqFinishTokenForG5Bridge(Ctx, TS.Handle, &Token);
		const double FinishMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		LastHostFinishMs.store(FinishMs);
		TRACE_COUNTER_SET(SuperSLMGpuHostFinishMs, FinishMs);
		if (St != FGpuStatus::SSLM_OK)
		{
			FaultResult(TS, R, St, TEXT("SslmGpuSeqFinishTokenForG5Bridge"));
			return;
		}
		RecordToken(TS, R, Token);
	}

	// Plugin paging plan §3.4 "The armed flag" (K7): the first finish-only step of an armed holder (an
	// adopt of a non-empty prefix, or a restore of a ready payload; a suffix's embeds leave the flag
	// set). SslmGpuSeqDecodeStepForG5Bridge takes its ready shortcut: it finishes the resident state,
	// embeds nothing and clears ready_for_logits, so ContextUsed does not advance and no room check
	// applies (a finish-only step). The finish bridge here would leave the engine's flag set, and the
	// next embed would be one token short. On both paths (RunOneCallToken(), ComposedFinishOnly).
	void ArmedFinish(FThreadSlot& TS, FActionResult& R)
	{
		int32_t Token = -1;
		FGpuStatus St;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Finish", SuperSLMGpuChannel);
			const double Start = FPlatformTime::Seconds();
			St = SslmGpuSeqDecodeStepForG5Bridge(Ctx, TS.Handle, FMath::Max(TS.LastToken, 0), FullTokenBudget(), &Token);
			const double FinishMs = (FPlatformTime::Seconds() - Start) * 1000.0;
			LastHostFinishMs.store(FinishMs);
			TRACE_COUNTER_SET(SuperSLMGpuHostFinishMs, FinishMs);
		}
		TS.bArmed = false;
		TS.bPrimed = false;
		if (St != FGpuStatus::SSLM_OK)
		{
			FaultResult(TS, R, St, TEXT("SslmGpuSeqDecodeStepForG5Bridge (the armed step)"));
			return;
		}
		RecordToken(TS, R, Token);
	}

	// Plugin paging plan §3.4 "Build pace" (K7, UP-11): one build-seat turn. One prefix token, through
	// sslm_gpu_prefix_prefill with count 1 at a whole token's dispatches (never DispatchBudget, which
	// may be one layer, below the verb's unit), and on the prefix's last token its freeze. A refusal,
	// or a turn that consumed other than one token, faults the build: its handle is released here and
	// every later turn of it reads the fault. Submission thread.
	void RunPrefixBuildTurn(const FAction& A, FActionResult& R)
	{
		FThreadPrefix* Prefix = ThreadPrefixes.Find(A.PrefixId);
		if (Prefix == nullptr || Prefix->Handle == nullptr)
		{
			R.Kind = EResultKind::Fault;
			R.Message = TEXT("the prefix's build had already failed");
			return;
		}
		const int32_t Token = static_cast<int32_t>(A.PromptToken);
		int32_t Consumed = 0;
		FGpuStatus St;
		const TCHAR* Where = TEXT("sslm_gpu_prefix_prefill");
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.PrefixBuild", SuperSLMGpuChannel);
			CountSubmission();
			St = sslm_gpu_prefix_prefill(Ctx, Prefix->Handle, &Token, 1, static_cast<int32_t>(A.PrefixKind), FullTokenBudget(), &Consumed);
		}
		NoteLayer1Status(St);
		if (St == FGpuStatus::SSLM_OK && Consumed == 1) { RecordGpuBusy(); }
		if (St == FGpuStatus::SSLM_OK && Consumed != 1)
		{
			R.Kind = EResultKind::Fault;
			R.Message = FString::Printf(TEXT("sslm_gpu_prefix_prefill consumed %d of 1 token"), static_cast<int32>(Consumed));
		}
		else if (St == FGpuStatus::SSLM_OK && A.bLastPrompt)
		{
			Where = TEXT("sslm_gpu_prefix_freeze");
			St = sslm_gpu_prefix_freeze(Ctx, Prefix->Handle);
			NoteLayer1Status(St);
		}
		if (St != FGpuStatus::SSLM_OK)
		{
			R.Kind = EResultKind::Fault;
			R.Message = FString::Printf(TEXT("%s returned %s"), Where, *GpuStatusText(St));
			R.Fault = Classify(St); // a confirmed device loss stops the job here, as on every other call
		}
		if (R.Kind == EResultKind::Fault)
		{
			const FGpuStatus ReleaseSt = sslm_gpu_prefix_release(Ctx, Prefix->Handle);
			NoteLayer1Status(ReleaseSt);
			// K7 review N2: the faulted build's thread entry goes with its handle (a later turn of the
			// same build finds none and reports the build failed; ReleaseGpuPrefix() of a Faulted prefix
			// posts no job). Prefix is not read after this.
			ThreadPrefixes.Remove(A.PrefixId);
			return;
		}
		R.Kind = A.bLastPrompt ? EResultKind::PrefillOk : EResultKind::Progress;
		if (A.bLastPrompt)
		{
			SnapshotPoolStatsOnThread();
		}
	}

	// One whole layer loop for the token just embedded: one call at a full-token budget, drained.
	FGpuStatus RunFullDepth(FThreadSlot& TS)
	{
		FGpuStatus St;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Submit", SuperSLMGpuChannel);
			CountSubmission();
			St = sslm_decode_step_gpu(Ctx, TS.Handle, TS.BoundAdapter, FullTokenBudget());
		}
		if (St == FGpuStatus::SSLM_OK)
		{
			int32_t Ready = 0;
			FGpuStatus Drained = FGpuStatus::SSLM_OK;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Drain", SuperSLMGpuChannel);
				St = sslm_gpu_ready(Ctx, TS.Handle, 1, &Ready, &Drained);
				NoteLayer1Status(St);
			}
			if (St == FGpuStatus::SSLM_OK)
			{
				St = Drained;
			}
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Readback", SuperSLMGpuChannel);
			RecordGpuBusy();
		}
		return St;
	}

	// The one-call path's prompt step (D-SLM7379): one whole prompt token per tick, embedded and
	// driven to full depth, with no finish.
	void RunOneCallPrompt(const FAction& A, FActionResult& R)
	{
		FThreadSlot& TS = ThreadSlots[A.Slot];
		if (!BindAdapterIfRequested(TS, A, R) || !ContextHasRoom(TS, R))
		{
			return;
		}
		FGpuStatus St;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Embed", SuperSLMGpuChannel);
			St = sslm_gpu_seq_embed_token(Ctx, TS.Handle, A.PromptToken);
		}
		if (St != FGpuStatus::SSLM_OK)
		{
			FaultResult(TS, R, St, TEXT("sslm_gpu_seq_embed_token"));
			return;
		}
		TS.ContextUsed += 1;
		St = RunFullDepth(TS);
		if (St != FGpuStatus::SSLM_OK)
		{
			FaultResult(TS, R, St, TEXT("sslm_decode_step_gpu"));
			return;
		}
		PromptTokenDone(TS, A, R);
	}

	void RunOneCallToken(const FAction& A, FActionResult& R)
	{
		FThreadSlot& TS = ThreadSlots[A.Slot];
		if (!BindAdapterIfRequested(TS, A, R))
		{
			return;
		}
		if (TS.bArmed)
		{
			ArmedFinish(TS, R); // plugin paging plan §3.4 (K7): the first finish-only step while armed
			return;
		}
		int32_t Token = -1;
		FGpuStatus St = FGpuStatus::SSLM_OK;
		if (TS.bPrimed)
		{
			// The last prompt token's residual is already at full depth: finish it directly and
			// never embed (D-SLM7256 rules 1-2). The same holds after a restore, since "primed"
			// travels in the plugin's own save wrapper.
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Finish", SuperSLMGpuChannel);
			const double Start = FPlatformTime::Seconds();
			St = SslmGpuSeqFinishTokenForG5Bridge(Ctx, TS.Handle, &Token);
			const double FinishMs = (FPlatformTime::Seconds() - Start) * 1000.0;
			LastHostFinishMs.store(FinishMs);
			TRACE_COUNTER_SET(SuperSLMGpuHostFinishMs, FinishMs);
			TS.bPrimed = false;
		}
		else
		{
			if (!ContextHasRoom(TS, R))
			{
				return;
			}
			// The bridge takes its finish-only shortcut when Layer 1's ready_for_logits is set.
			// The plugin calls no prefill verb, and the two other events that set it -- a schema
			// dead end and, at 1.7.0, a hook that breaks its exactly-once contract -- both stop
			// the sequence (RecordToken(), FaultResult()), so no live plugin sequence reaches this
			// call with it set, and the bridge always embeds TS.LastToken, drives it to depth and
			// finishes. One fused Layer-1 call does embed+decode+drain, so Submit is the honest
			// scope for the whole call rather than a separately-timed Embed (there is no boundary
			// Layer 1 exposes between them here).
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Submit", SuperSLMGpuChannel);
				CountSubmission();
				St = SslmGpuSeqDecodeStepForG5Bridge(Ctx, TS.Handle, TS.LastToken, FullTokenBudget(), &Token);
			}
			TS.ContextUsed += 1;
			TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Readback", SuperSLMGpuChannel);
			RecordGpuBusy();
		}
		if (St != FGpuStatus::SSLM_OK)
		{
			FaultResult(TS, R, St, TEXT("SslmGpuSeqDecodeStepForG5Bridge"));
			return;
		}
		RecordToken(TS, R, Token);
	}

	void ExecuteTickJob(FJob& Job)
	{
		// Runs on the dedicated submission thread (already named "SuperSLM GPU Submission",
		// StartThread()); this scope and Insights' own thread-name column together place this
		// work off the game thread without any extra bookkeeping here.
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.SubmissionJob", SuperSLMGpuChannel);
		Job.Results.SetNum(Job.Actions.Num());

		// Round 2, "Terminal loss never waits": once a call in this job confirms a terminal loss
		// (Classify()), the job stops executing; every action not yet run keeps its Skipped result.
		// One-call steps (prompt tokens, then tokens), then the composed slices of this tick.
		for (int32 I = 0; I < Job.Actions.Num(); ++I)
		{
			if (bTerminalLoss.load())
			{
				break;
			}
			const FAction& A = Job.Actions[I];
			if (A.Kind == EActionKind::PrefixBuildTurn)
			{
				RunPrefixBuildTurn(A, Job.Results[I]); // the build seat's place in the one-call rotation (K7)
				continue;
			}
			if ((A.Kind != EActionKind::OneCallPromptToken && A.Kind != EActionKind::OneCallToken) || !ActionIsLive(A))
			{
				continue;
			}
			if (A.bFirstPrompt || A.bBeginLimits)
			{
				BeginPrompt(ThreadSlots[A.Slot], A);
			}
			if (SkipStopped(A, Job.Results[I]))
			{
				continue;
			}
			if (A.Kind == EActionKind::OneCallPromptToken)
			{
				RunOneCallPrompt(A, Job.Results[I]);
			}
			else
			{
				RunOneCallToken(A, Job.Results[I]);
			}
		}

		// Composed: finish-only primed steps, embeds, then one decode call for every slice.
		TArray<int32, TInlineAllocator<16>> SliceActions;
		for (int32 I = 0; I < Job.Actions.Num(); ++I)
		{
			if (bTerminalLoss.load())
			{
				break;
			}
			const FAction& A = Job.Actions[I];
			if ((A.Kind != EActionKind::ComposedSlice && A.Kind != EActionKind::ComposedFinishOnly) || !ActionIsLive(A))
			{
				continue;
			}
			if (A.bFirstPrompt || A.bBeginLimits)
			{
				BeginPrompt(ThreadSlots[A.Slot], A);
			}
			if (SkipStopped(A, Job.Results[I]))
			{
				continue;
			}
			FThreadSlot& TS = ThreadSlots[A.Slot];
			FActionResult& R = Job.Results[I];
			if (A.Kind == EActionKind::ComposedFinishOnly)
			{
				if (TS.bArmed)
				{
					ArmedFinish(TS, R); // plugin paging plan §3.4 (K7): replaces the one primed finish-bridge call
					continue;
				}
				TS.bPrimed = false;
				FinishToken(TS, R);
				continue;
			}
			if (A.bEmbed)
			{
				if (!BindAdapterIfRequested(TS, A, R) || !ContextHasRoom(TS, R))
				{
					continue;
				}
				FGpuStatus EmbedSt;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Embed", SuperSLMGpuChannel);
					EmbedSt = sslm_gpu_seq_embed_token(Ctx, TS.Handle, A.bPrompt ? A.PromptToken : TS.LastToken);
				}
				if (EmbedSt != FGpuStatus::SSLM_OK)
				{
					FaultResult(TS, R, EmbedSt, TEXT("sslm_gpu_seq_embed_token"));
					continue;
				}
				TS.ContextUsed += 1;
				TS.PinnedAdapter = TS.BoundAdapter;
			}
			SliceActions.Add(I);
		}
		if (bTerminalLoss.load())
		{
			SliceActions.Reset(); // no decode call, and no finish, on a lost device
		}

		if (SliceActions.Num() == 1)
		{
			const int32 I = SliceActions[0];
			const FAction& A = Job.Actions[I];
			FThreadSlot& TS = ThreadSlots[A.Slot];
			FActionResult& R = Job.Results[I];
			FGpuStatus St;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Submit", SuperSLMGpuChannel);
				CountSubmission();
				St = sslm_decode_step_gpu(Ctx, TS.Handle, TS.PinnedAdapter, static_cast<uint32>(A.Layers) * DispatchesPerLayer);
			}
			if (St == FGpuStatus::SSLM_OK)
			{
				int32_t Ready = 0;
				FGpuStatus Drained = FGpuStatus::SSLM_OK;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Drain", SuperSLMGpuChannel);
					St = sslm_gpu_ready(Ctx, TS.Handle, 1, &Ready, &Drained);
					NoteLayer1Status(St);
				}
				if (St == FGpuStatus::SSLM_OK)
				{
					St = Drained;
				}
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Readback", SuperSLMGpuChannel);
				RecordGpuBusy();
			}
			if (St != FGpuStatus::SSLM_OK)
			{
				FaultResult(TS, R, St, TEXT("sslm_decode_step_gpu"));
			}
		}
		else if (SliceActions.Num() > 1)
		{
			// The tick's layer budget, spent through Layer 1's own running remainder in the
			// planner's rotated order (D-SLM7255). The budget is exactly the sum of the planned
			// slices, so each sequence takes exactly the layers the planner assigned it.
			TArray<SslmGpuSequenceHandle*, TInlineAllocator<16>> Seqs;
			TArray<const SslmGpuAdapterHandle*, TInlineAllocator<16>> SliceAdapters;
			TArray<FGpuStatus, TInlineAllocator<16>> Statuses;
			uint32 Budget = 0;
			for (int32 I : SliceActions)
			{
				const FAction& A = Job.Actions[I];
				Seqs.Add(ThreadSlots[A.Slot].Handle);
				SliceAdapters.Add(ThreadSlots[A.Slot].PinnedAdapter);
				Budget += static_cast<uint32>(A.Layers) * DispatchesPerLayer;
			}
			Statuses.SetNumZeroed(SliceActions.Num());
			// Layer 1's own batch implementation drains each sequence (sslm_gpu_ready(block=1))
			// the instant its own submission succeeds -- see sslm_decode_step_batch_gpuImpl's own
			// header comment (gpu_1p0.cpp) -- so this ONE call already covers submit, drain and
			// readback for every slice in the batch; Layer 1 exposes no boundary between them
			// here, so Submit is the honest scope for the whole call (matching RunOneCallToken's
			// own fused-call reasoning above).
			FGpuStatus CallStatus;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Submit", SuperSLMGpuChannel);
				CountSubmission(SliceActions.Num());
				CallStatus = sslm_decode_step_batch_gpu(Ctx, Seqs.GetData(), SliceAdapters.GetData(),
					static_cast<uint32>(Seqs.Num()), Budget, Statuses.GetData());
			}
			{
				// No separate Drain scope here (finding 6, T-2885 review): Layer 1's batch call
				// drains internally and exposes no boundary to time (see the comment above), so an
				// empty scope was a marker with no work inside it, not a measurement -- and the
				// R-S2h Insights oracle counts scope INSTANCES, not duration, so the marker alone was
				// enough to satisfy it without a real drain ever being timed on this path.
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Readback", SuperSLMGpuChannel);
				RecordGpuBusy();
			}
			for (int32 K = 0; K < SliceActions.Num(); ++K)
			{
				const FAction& A = Job.Actions[SliceActions[K]];
				const FGpuStatus St = CallStatus != FGpuStatus::SSLM_OK ? CallStatus : Statuses[K];
				if (St != FGpuStatus::SSLM_OK)
				{
					FaultResult(ThreadSlots[A.Slot], Job.Results[SliceActions[K]], St, TEXT("sslm_decode_step_batch_gpu"));
				}
			}
		}

		for (int32 I : SliceActions)
		{
			if (bTerminalLoss.load())
			{
				break;
			}
			const FAction& A = Job.Actions[I];
			FThreadSlot& TS = ThreadSlots[A.Slot];
			FActionResult& R = Job.Results[I];
			if (R.Kind == EResultKind::Fault)
			{
				continue;
			}
			if (A.bFinish)
			{
				FinishToken(TS, R);
			}
			else
			{
				// A prompt token never finishes; the slice completing the last one primes the
				// sequence (bLastPrompt is set only on that slice).
				PromptTokenDone(TS, A, R);
			}
		}

		// Rule 4 (T-2983 finding 10): every result of a live action echoes the bind tag its
		// sequence carried when the result was produced, so the game thread confirms a requested
		// swap only on a result that ran after the bind, never on one planned before the request.
		for (int32 I = 0; I < Job.Actions.Num(); ++I)
		{
			const FAction& A = Job.Actions[I];
			if (A.Kind != EActionKind::PrefixBuildTurn && ActionIsLive(A))
			{
				Job.Results[I].BindTag = ThreadSlots[A.Slot].AppliedBindTag;
				Job.Results[I].ContextUsed = ThreadSlots[A.Slot].ContextUsed; // plugin paging plan §3.4 (K6): the mirror
				Job.Results[I].Armed = ThreadSlots[A.Slot].bArmed ? 1 : 0;    // and the armed mirror (K7)
			}
		}
	}

	// ------------------------------------------------------------------ teardown

	// Runs on the submission thread. The enforced order (plan §9 R-S2d): unbind, release every
	// sequence (Layer 1 also unbinds on release, and release is legal mid-token), unmap every
	// adapter, unmap the model, destroy the context. No step can meet a refusal: nothing is left
	// Submitted between jobs, every sequence is released before any adapter, and every adapter
	// before the model.
	void ReleaseLayer1()
	{
		// Diagnostic only (R-S1l (j), U1 round 3): every status Layer 1 returns here is recorded
		// by name; the teardown order and its dispositions are unchanged.
		TeardownRecord = FSuperSLMGpuTeardownStatuses();
		TeardownRecord.bRecorded = true;
		for (int32 SlotIndex = 0; SlotIndex < ThreadSlots.Num(); ++SlotIndex)
		{
			FThreadSlot& TS = ThreadSlots[SlotIndex];
			if (TS.Handle != nullptr)
			{
				const FGpuStatus UnbindSt = sslm_gpu_seq_bind_adapter(Ctx, TS.Handle, nullptr);
				NoteLayer1Status(UnbindSt);
				const FGpuStatus St = sslm_gpu_seq_release(Ctx, TS.Handle);
				NoteLayer1Status(St);
				TeardownRecord.SequenceSlots.Add(SlotIndex);
				TeardownRecord.SequenceUnbindStatuses.Add(GpuStatusText(UnbindSt));
				TeardownRecord.SequenceReleaseStatuses.Add(GpuStatusText(St));
				if (St != FGpuStatus::SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_seq_release refused (%s)."), *GpuStatusText(St));
				}
				TS.Handle = nullptr;
				TS.BoundAdapter = nullptr;
				TS.PinnedAdapter = nullptr;
			}
		}
		// Plugin paging plan §3.4 "Pool and sequences" (K7): every prefix, after the sequences and
		// before the pool, which refuses its destroy while a prefix drawn from it is live.
		for (TPair<int64, FThreadPrefix>& Pair : ThreadPrefixes)
		{
			if (Pair.Value.Handle != nullptr)
			{
				const FGpuStatus St = sslm_gpu_prefix_release(Ctx, Pair.Value.Handle);
				NoteLayer1Status(St);
				if (St != FGpuStatus::SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_prefix_release refused (%s)."), *GpuStatusText(St));
				}
				Pair.Value.Handle = nullptr;
			}
		}
		ThreadPrefixes.Reset();
		// Plugin paging plan §3.5 (K6): the pool, after every sequence drawn from it (and, at K7,
		// every prefix) is released, and before the adapters, the model and the context.
		if (Pool != nullptr)
		{
			const FGpuStatus St = sslm_gpu_kv_pool_destroy(Ctx, Pool);
			NoteLayer1Status(St);
			TeardownRecord.KvPoolDestroyStatus = GpuStatusText(St);
			if (St != FGpuStatus::SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_kv_pool_destroy refused (%s)."), *GpuStatusText(St));
			}
			Pool = nullptr;
		}
		for (TPair<int64, FAdapterEntry>& Pair : Adapters)
		{
			if (Pair.Value.Handle != nullptr)
			{
				const FGpuStatus St = sslm_gpu_adapter_unmap(Ctx, Pair.Value.Handle);
				NoteLayer1Status(St);
				TeardownRecord.AdapterIds.Add(Pair.Key);
				TeardownRecord.AdapterUnmapStatuses.Add(GpuStatusText(St));
				if (St != FGpuStatus::SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_adapter_unmap refused (%s)."), *GpuStatusText(St));
				}
				Pair.Value.Handle = nullptr;
			}
		}
		if (Model != nullptr)
		{
			const FGpuStatus St = sslm_gpu_model_unmap(Ctx, Model);
			NoteLayer1Status(St);
			TeardownRecord.ModelUnmapStatus = GpuStatusText(St);
			if (St != FGpuStatus::SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_model_unmap refused (%s)."), *GpuStatusText(St));
			}
			Model = nullptr;
		}
		if (Ctx != nullptr)
		{
			const FGpuStatus St = sslm_gpu_context_destroy(Ctx);
			NoteLayer1Status(St);
			TeardownRecord.ContextDestroyStatus = GpuStatusText(St);
			if (St != FGpuStatus::SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("GPU teardown: sslm_gpu_context_destroy refused (%s); the context is leaked rather than freed under a live handle."), *GpuStatusText(St));
			}
			Ctx = nullptr;
		}
	}

	// The teardown job: ReleaseLayer1() on the submission thread, marked bRunsAfterLoss so it runs
	// even after a confirmed terminal loss, when Run() skips every other job (round 2). A
	// frame-path-bounded call, as the RunSync() it replaces was.
	TSharedPtr<FJob> MakeTeardownJob()
	{
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this]() { ReleaseLayer1(); };
		Job->bRunsAfterLoss = true;
		return Job;
	}

	// TearDown()'s and Deinitialize()'s path (not the frame path): releases Layer 1 and joins the
	// thread. It waits for the teardown job -- the one BeginTeardownAfterLoss() already enqueued, or
	// a new one -- through Wait(), so the wait is bounded by the row-21 holder check; after a loss
	// every other queued job is skipped work, so the wait is the one Layer-1 call in progress.
	// Plan §2.5 row 21: the submission thread may still be inside a Layer-1 call, so it is neither
	// joined nor its device objects freed under that call. The thread is asked to stop once it
	// returns, and the thread, its events, every Layer-1 handle and this state are leaked
	// deliberately -- the precedent the context-destroy refusal sets. Never waits. Shutdown()'s leak
	// branch, and (round 3) FinishTeardownIfDone()'s whenever the thread is unresponsive.
	void LeakUnresponsiveThread()
	{
		if (Thread != nullptr)
		{
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM GPU backend teardown: the submission thread is unresponsive (%s); it is not joined, and its device objects are leaked rather than freed under a running call."),
				*UnresponsiveReason);
			bStopThread = true;
			WorkEvent->Trigger();
			Thread = nullptr;
		}
		bActive = false;
	}

	void Shutdown()
	{
		ReleaseTestGate(); // round 2: no teardown waits on a test gate
		bTestHoldNextTickJob = false; // round 3: a pending hold request does not outlive the backend
		if (bTeardownPending)
		{
			if (TeardownJob.IsValid() && !bUnresponsive.load())
			{
				Wait(*TeardownJob);
			}
			bTeardownPending = false;
			TeardownJob.Reset();
		}
		else if (Thread != nullptr && !bUnresponsive.load())
		{
			const TSharedPtr<FJob> Job = MakeTeardownJob();
			Enqueue(Job);
			Wait(*Job);
		}
		if (bUnresponsive.load())
		{
			LeakUnresponsiveThread();
			return;
		}
		StopThread();
		Adapters.Empty();
		Adapter3.Reset();
		bActive = false;
	}

	// Ruling 2026-09-26, round 2, "Terminal loss never waits": what Tick() does in place of
	// Shutdown() on the tick that confirms a terminal loss. It never waits on the submission
	// thread. FallBackToCpu() faults every live sequence by name and clears bActive, so
	// IsGpuBackendActive() reads false from this tick. When the thread is unresponsive (or absent),
	// Shutdown()'s leak branch runs, which never waits. Otherwise ReleaseLayer1() is enqueued as a
	// bRunsAfterLoss job behind the queue -- every job ahead of it now completes Skipped without
	// touching the device -- and a later Tick() (FinishTeardownIfDone()) or Shutdown() finishes
	// teardown once it reads done. Game thread.
	void BeginTeardownAfterLoss()
	{
		FallBackToCpu();
		ReleaseTestGate();
		if (bTeardownPending)
		{
			return;
		}
		if (bUnresponsive.load() || Thread == nullptr)
		{
			Shutdown(); // the leak branch, or nothing left to join: no wait either way
		}
		else
		{
			TeardownJob = MakeTeardownJob();
			Enqueue(TeardownJob);
			bTeardownPending = true;
		}
		// Last, so that every blob ResolveEntry() releases is dropped on the game thread
		// (Thread == nullptr, bUnresponsive or bTeardownPending now holds; round 5).
		ResolveUnresolvedAfterLoss();
	}

	// Ruling 2026-09-26, round 5 (finding 2): at a confirmed terminal loss no tick polls a lifecycle
	// op again, so every handle still unresolved is resolved here, NotConfigured with the loss text:
	// each vended slot's op log from OpFront to the end, submitted or not, and the restore queue from
	// its front. A restore releases its adapter pin and its slot reservation, as its refused path
	// does, and no recycle is queued (the teardown releases every Layer-1 sequence). The skipped
	// jobs of submitted ops still complete on the thread; nothing reads them. Game thread.
	void ResolveUnresolvedAfterLoss()
	{
		for (FGameSlot& S : Slots)
		{
			if (!S.bVended)
			{
				continue;
			}
			for (int32 Index = S.OpFront; Index < S.OpLog.Num(); ++Index)
			{
				FLifecycleOpEntry& Entry = S.OpLog[Index];
				if (Entry.Result == ESuperSLMRestoreResult::Pending)
				{
					ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
					LastLifecycleRequestError = LostBeforeRanText();
				}
			}
			S.OpFront = S.OpLog.Num();
			S.bSaveAssembling = false;
		}
		for (int32 Index = RestoreOpsFront; Index < RestoreOps.Num(); ++Index)
		{
			FLifecycleOpEntry& Entry = RestoreOps[Index];
			if (Entry.Result != ESuperSLMRestoreResult::Pending)
			{
				continue;
			}
			ReleaseRestorePin(Entry);
			if (Entry.bSubmitted && Slots.IsValidIndex(Entry.RestoreSlotIndex))
			{
				Slots[Entry.RestoreSlotIndex].bVended = false; // the reservation, as Refuse() releases it
				Slots[Entry.RestoreSlotIndex].bRestoreReserved = false;
			}
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			LastLifecycleRequestError = LostBeforeRanText();
		}
		// Row 19: resolved restores keep only their HandleResults entry, as PollRestoreFront() compacts.
		RestoreOps.Reset();
		RestoreOpsFront = 0;
	}

	// Round 2, restated in round 3 (findings 1 and 6): the pending-teardown window's whole tick.
	// Called at the top of every Tick(), before its inactive return; while a teardown is pending the
	// tick does only this -- it applies, admits and plans nothing. In order:
	//  - the holder check, so a hung release is caught (CheckLockHolderOverrun(0));
	//  - if the thread is unresponsive, at any point in the window, Shutdown()'s leak branch,
	//    whatever the release job reads (a job the unresponsive paths marked done never ran);
	//  - on the first tick the release job reads done, the thread is asked to stop (bStopThread,
	//    WorkEvent) and nothing is joined;
	//  - on a later tick at which bThreadExited reads true, the join (StopThread(), which then returns
	//    at once) and the adapter release, as Shutdown()'s tail does.
	// It never joins a busy thread and never waits. Returns true while a teardown was pending at
	// entry, so Tick() does nothing else. Game thread; bDone and bThreadExited read with acquire.
	bool FinishTeardownIfDone()
	{
		if (!bTeardownPending)
		{
			return false;
		}
		if (!bUnresponsive.load())
		{
			CheckLockHolderOverrun(0);
		}
		if (bUnresponsive.load())
		{
			LeakUnresponsiveThread();
			bTeardownPending = false;
			TeardownJob.Reset();
			return true;
		}
		if (!TeardownJob.IsValid() || !TeardownJob->bDone.load(std::memory_order_acquire))
		{
			return true;
		}
		if (!bStopThread.load())
		{
			bStopThread = true;
			if (WorkEvent != nullptr)
			{
				WorkEvent->Trigger();
			}
			return true;
		}
		if (!bThreadExited.load())
		{
			return true;
		}
		StopThread(); // the thread has returned from Run(): the join returns at once
		Adapters.Empty();
		Adapter3.Reset();
		bActive = false;
		bTeardownPending = false;
		TeardownJob.Reset();
		return true;
	}

	// ------------------------------------------------------------------ game-thread helpers

	FGameSlot* FindSlot(const FSuperSLMGpuSequence& Sequence, int32* OutIndex = nullptr)
	{
		const int32* Index = IdToSlot.Find(Sequence.Id);
		if (Index == nullptr)
		{
			return nullptr;
		}
		if (OutIndex != nullptr)
		{
			*OutIndex = *Index;
		}
		return &Slots[*Index];
	}

	const FGameSlot* FindSlot(const FSuperSLMGpuSequence& Sequence) const
	{
		const int32* Index = IdToSlot.Find(Sequence.Id);
		return Index != nullptr ? &Slots[*Index] : nullptr;
	}

	int32 LayersPerSliceFor(const FGameSlot& S) const
	{
		return S.LayersPerSliceOverride > 0 ? FMath::Min(S.LayersPerSliceOverride, LayersPerTick) : LayersPerTick;
	}

	void BumpGeneration(int32 SlotIndex)
	{
		FGameSlot& S = Slots[SlotIndex];
		S.Gen += 1;
		LiveGen[SlotIndex].store(S.Gen, std::memory_order_release);
	}

	void ClearGeneration(FGameSlot& S)
	{
		S.Phase = ESuperSLMSequencePhase::Idle;
		S.Generated.Reset();
		S.LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
		S.LastFault = ESuperSLMGpuFaultReason::None;
		S.Request = FSuperSLMGenerationRequest();
		S.PromptPlanned = 0;
		S.bPlanInPrompt = false;
		S.PlanPromptIndex = 0;
		S.BeginWallSeconds = 0.0;
		S.FirstTokenMs = -1.0;
		S.bPlanPrimed = false;
		S.bPlanBeginLimits = false;
		S.PlanBeginLimit = 0;
		S.TokensIssued = 0;
		S.PlanLayerPos = 0;
		S.bSchemaAccepting = false;
		DropPendingEvents(S);
		S.ActiveGenerationOpLogIndex = INDEX_NONE;
	}

	// Queues a reset of SlotIndex's Layer-1 sequence (sslm_gpu_seq_reset, legal from every Idle
	// state since 1.6.0; it discards a partial token and keeps the schema binding, rewound to its
	// start). bUnbind also unbinds the schema and adapter, returning the sequence fresh to the
	// pool. Plan §2.5 row 16 (D-SLM7682): the job records the reset status and, when it unbinds,
	// both unbind statuses into Async. A refused reset does not unbind (Layer 1 would refuse an
	// ineligible sequence anyway) and does not clear the thread slot's generation fields as if
	// reset; it still re-syncs the stale-result generation, so the filter stays consistent with
	// the game side. Returns the queued job so a caller can poll it without blocking.
	// Plugin paging plan §3.5 "Normalization" (K7): the create-first replacement of TS's holder by a
	// budgeted SequenceBudget one, on the submission thread. True when the new holder took the slot
	// (the old one released); false, with the old holder untouched, when the create, a re-bind of a
	// kept binding, or the old holder's release was refused -- the caller then resets in place.
	bool NormalizeHolderOnThread(FThreadSlot& TS, int32 SlotIndex, bool bUnbind)
	{
		SslmGpuSequenceHandle* Fresh = nullptr;
		FGpuStatus St = sslm_gpu_seq_create_budgeted(Ctx, Model, Pool, Config.SequenceBudget, &Fresh);
		NoteLayer1Status(St);
		const TCHAR* Where = TEXT("sslm_gpu_seq_create_budgeted");
		if (St == FGpuStatus::SSLM_OK && Fresh != nullptr && !bUnbind)
		{
			// A reset keeps the schema binding (rewound) and the adapter: the new holder takes both.
			if (TS.SchemaIndex >= 0)
			{
				Where = TEXT("SslmGpuSeqSetSchemaForG5Bridge (the normalized holder)");
				St = SslmGpuSeqSetSchemaForG5Bridge(Ctx, Fresh, TS.SchemaIndex);
				NoteLayer1Status(St);
			}
			if (St == FGpuStatus::SSLM_OK && TS.BoundAdapter != nullptr)
			{
				Where = TEXT("sslm_gpu_seq_bind_adapter (the normalized holder)");
				St = sslm_gpu_seq_bind_adapter(Ctx, Fresh, TS.BoundAdapter);
				NoteLayer1Status(St);
			}
		}
		if (St == FGpuStatus::SSLM_OK && Fresh != nullptr)
		{
			Where = TEXT("sslm_gpu_seq_release (the holder being normalized)");
			St = sslm_gpu_seq_release(Ctx, TS.Handle);
			NoteLayer1Status(St);
		}
		if (St != FGpuStatus::SSLM_OK || Fresh == nullptr)
		{
			if (Fresh != nullptr)
			{
				NoteLayer1Status(sslm_gpu_seq_release(Ctx, Fresh));
			}
			UE_LOG(LogSuperSLM, Display, TEXT("GPU slot %d: the create-first normalization was not made (%s returned %s); the holder is reset in place and normalized at a later reset."),
				SlotIndex, Where, *GpuStatusText(St));
			return false;
		}
		TS.Handle = Fresh;
		if (bUnbind)
		{
			TS.SchemaIndex = -1;
			TS.BoundAdapter = nullptr;
			TS.AppliedBindTag = 0;
		}
		return true;
	}

	// Plugin paging plan §3.5 "Normalization" (K7), pooled mode: bNormalize is the page ledger's
	// create-first normalization (FAdmission::bNormalizes). The job first creates a budgeted
	// SequenceBudget holder, then releases the old one, and the new holder takes the slot (a reset
	// that keeps its bindings re-binds the schema and adapter on it first). If the create (or a
	// re-bind) is refused, the old holder is reset in place, as an ordinary reset, and the ledger's
	// apply reads ResetInPlace (normalization is retried at the next reset). LimitInPlace and
	// LimitNormalized are the holder's limit after each outcome (FThreadSlot::Limit; 0 in dedicated
	// mode, where the limit is not used).
	TSharedPtr<FJob> QueueReset(int32 SlotIndex, bool bUnbind, const TSharedPtr<FLifecycleAsyncState>& Async, bool bNormalize = false,
		int64 LimitInPlace = 0, int64 LimitNormalized = 0)
	{
		// Plan §3.9 (K6): JobsPosted() counts the reset jobs posted, by kind.
		JobsPostedByKind[static_cast<int32>(bUnbind ? EGpuPagedJobKind::Return : EGpuPagedJobKind::Reset)] += 1;
		BumpGeneration(SlotIndex);
		const uint32 Gen = Slots[SlotIndex].Gen;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this, SlotIndex, Gen, bUnbind, Async, bNormalize, LimitInPlace, LimitNormalized]()
		{
			FThreadSlot& TS = ThreadSlots[SlotIndex];
			TS.Gen = Gen;
			if (TS.Handle == nullptr)
			{
				return;
			}
#if WITH_DEV_AUTOMATION_TESTS
			const FGpuStatus ForcedResetSt = static_cast<FGpuStatus>(TestNextResetStatus.exchange(0u, std::memory_order_acq_rel));
#else
			const FGpuStatus ForcedResetSt = FGpuStatus::SSLM_OK;
#endif
			bool bNormalized = false;
			if (bNormalize && ForcedResetSt == FGpuStatus::SSLM_OK)
			{
				bNormalized = NormalizeHolderOnThread(TS, SlotIndex, bUnbind);
			}
			const FGpuStatus St = bNormalized ? FGpuStatus::SSLM_OK
				: (ForcedResetSt != FGpuStatus::SSLM_OK ? ForcedResetSt : sslm_gpu_seq_reset(Ctx, TS.Handle));
			NoteLayer1Status(St);
			if (Async.IsValid())
			{
				Async->bNormalized = bNormalized;
			}
			if (St == FGpuStatus::SSLM_OK && bPooled)
			{
				// Plugin paging plan §3.4, §3.5 (K7): the holder rests at origin 0 with its own budget (or,
				// normalized, SequenceBudget), and a reset clears the engine's ready flag. A negative
				// in-place limit keeps the holder's limit as it stands (a deferred recycle, K6 review C1).
				const int64 LimitAfter = bNormalized ? LimitNormalized : LimitInPlace;
				if (LimitAfter >= 0)
				{
					TS.Limit = LimitAfter;
				}
				TS.bArmed = false;
			}
			if (Async.IsValid())
			{
				Async->bResetRan = true;
				Async->ResetStatus = St;
			}
			if (St != FGpuStatus::SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("sslm_gpu_seq_reset refused (%s) on GPU slot %d; the sequence is left as it stood."), *GpuStatusText(St), SlotIndex);
				if (Async.IsValid())
				{
					Async->WalkState = SslmGpuSeqWalkStateForG5Bridge(TS.Handle); // ruling 2026-09-26: as it stood
				}
				return;
			}
			if (bUnbind)
			{
				const FGpuStatus SchemaSt = SslmGpuSeqSetSchemaForG5Bridge(Ctx, TS.Handle, -1);
				NoteLayer1Status(SchemaSt); // K6 review S2
				const FGpuStatus AdapterSt = sslm_gpu_seq_bind_adapter(Ctx, TS.Handle, nullptr);
				NoteLayer1Status(AdapterSt);
				if (Async.IsValid())
				{
					Async->UnbindSchemaStatus = SchemaSt;
					Async->UnbindAdapterStatus = AdapterSt;
				}
				if (SchemaSt == FGpuStatus::SSLM_OK)
				{
					TS.SchemaIndex = -1;
				}
				if (AdapterSt == FGpuStatus::SSLM_OK)
				{
					TS.BoundAdapter = nullptr;
					TS.AppliedBindTag = 0;
				}
			}
			TS.bPrimed = false;
			TS.bHasStopFault = false;
			TS.StopFault = FActionResult();
			TS.bStopped = false;
			TS.LastToken = -1;
			TS.ContextUsed = 0;
			TS.Produced = 0;
			TS.MaxNewTokens = 0;
			TS.StopTokenIds.Reset();
			TS.PinnedAdapter = nullptr;
			if (Async.IsValid())
			{
				// Ruling 2026-09-26: the walk after the reset (rewound to the schema's start, or
				// Unused once unbound), stored when the reset op finalizes (PollSlotFront()).
				Async->WalkState = SslmGpuSeqWalkStateForG5Bridge(TS.Handle);
			}
		};
		return Enqueue(Job);
	}

	// Plan §2.5 rows 16 and 20 (H6, H7, H8): an unbinding reset no caller holds a handle to (the
	// recycle after a hand-off, or the re-sync after a failed restore). Its statuses are read by
	// AdmitQueuedOps(); any refusal withholds the slot.
	void QueueRecycle(int32 SlotIndex)
	{
		FPendingRecycle Recycle;
		Recycle.SlotIndex = SlotIndex;
		bool bNormalize = false;
		int64 LimitInPlace = 0;
		int64 LimitNormalized = 0;
		// Plugin paging plan §3.5 (K6): in pooled mode the recycle is a reset the page ledger admits
		// (it returns whatever the holder drew above its warm reservation, at its apply), and is
		// applied when PollRecycles() reads its statuses. A refusal is a plugin defect: the recycle
		// is not posted and the slot is withheld, so no holder runs untracked.
		//
		// K6 review C1, fix (b): a slot may be vended and returned again before its previous recycle
		// has applied (a Vend right after a Return gets the slot at once; FIFO orders every job of the
		// new holder after that recycle). The ledger admits one job per slot, so this recycle's
		// admission is deferred to PollRecycles(), which admits it in queue order once the earlier job
		// has applied. Its job is posted now, in FIFO order, as a reset in place: it draws nothing,
		// and it keeps the holder's limit, which is the one the earlier recycle left (origin 0, and no
		// shape-changing job can run on the slot between the two -- an adopt or a reset of the holder
		// in between waits for the earlier recycle, and its return waits for it). A normalization the
		// ledger would ask for at the deferred admission is applied as ResetInPlace, and retried at
		// the slot's next reset.
		if (IsPooled() && (Ledger.SlotHasJob(SlotIndex) || HasUnadmittedRecycle(SlotIndex)))
		{
			Recycle.bLedgerAdmitted = false;
			Recycle.HoldId = AttachPagedHold(EGpuPagedJobKind::Return);
			Recycle.Async = MakeShared<FLifecycleAsyncState>();
			Recycle.Async->Job = QueueReset(SlotIndex, /*bUnbind*/ true, Recycle.Async, /*bNormalize*/ false, /*LimitInPlace: keep*/ -1, 0);
			PendingRecycles.Add(MoveTemp(Recycle));
			return;
		}
		if (IsPooled())
		{
			const SuperSLMPaging::FAdmission Admission = Ledger.AdmitReset(SlotIndex);
			if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
			{
				NoteLedgerRefusal(SlotIndex, FString::Printf(TEXT("the page ledger did not admit the recycle's reset (outcome %d)"), static_cast<int32>(Admission.Outcome)));
				Slots[SlotIndex].bWithheld = true;
				return;
			}
			Recycle.LedgerTicket = Admission.Ticket;
			Recycle.HoldId = AttachPagedHold(EGpuPagedJobKind::Return);
			bNormalize = Admission.bNormalizes; // K7: a restored holder of another budget is replaced create-first
			ResetLimits(SlotIndex, LimitInPlace, LimitNormalized);
		}
		Recycle.Async = MakeShared<FLifecycleAsyncState>();
		Recycle.Async->Job = QueueReset(SlotIndex, /*bUnbind*/ true, Recycle.Async, bNormalize, LimitInPlace, LimitNormalized);
		PendingRecycles.Add(MoveTemp(Recycle));
	}

	// K6 review N1: the one formula for a pooled holder's generation limit, the page ledger's own
	// (FPageLedger::SlotLimit(): HolderLimit at the ledger's Cp). Every FThreadSlot::Limit a job
	// writes, and the activation pre-check's SlotLimit(), come from a ledger shape through it.
	int64 LedgerHolderLimit(const SuperSLMPaging::FHolderShape& Shape) const
	{
		return SuperSLMPaging::HolderLimit(Ledger.GetConfig().ContextCap, Shape);
	}

	// Plugin paging plan §3.4, §3.5 (K7): the slot holder's generation limit after a reset in place
	// (its own budget, origin 0) and after a create-first normalization (SequenceBudget, origin 0),
	// read from the ledger's shape for the slot before the reset applies. Pooled mode.
	void ResetLimits(int32 SlotIndex, int64& OutInPlace, int64& OutNormalized) const
	{
		SuperSLMPaging::FHolderShape Shape = Ledger.SlotShape(SlotIndex);
		Shape.Origin = 0;
		OutInPlace = LedgerHolderLimit(Shape);
		SuperSLMPaging::FHolderShape Normalized;
		Normalized.Mode = SuperSLMPaging::EHolderMode::Budget;
		Normalized.Budget = Config.SequenceBudget;
		Normalized.Origin = 0;
		OutNormalized = LedgerHolderLimit(Normalized);
	}

	// Plugin paging plan §3.5 (K6, K7): a reset's page-ledger apply, with what its job did: Normalized
	// when the job replaced the holder create-first (K7), ResetInPlace when it reset the holder as it
	// is (a reset in place, or a normalization whose create was refused), Refused when Layer 1
	// refused the reset or the job never ran. A stale ticket (an ended epoch) changes nothing. A
	// normalization replaced the slot's Layer-1 handle, so the slot's handle generation rises.
	void ApplyLedgerReset(int32 SlotIndex, SuperSLMPaging::FTicket& Ticket, bool bRan, bool bNormalized)
	{
		if (Ticket == SuperSLMPaging::InvalidTicket)
		{
			return;
		}
		const SuperSLMPaging::EResetResult Result = !bRan ? SuperSLMPaging::EResetResult::Refused
			: (bNormalized ? SuperSLMPaging::EResetResult::Normalized : SuperSLMPaging::EResetResult::ResetInPlace);
		Ledger.ApplyReset(Ticket, Result);
		Ticket = SuperSLMPaging::InvalidTicket;
		if (bRan && bNormalized && Slots.IsValidIndex(SlotIndex))
		{
			Slots[SlotIndex].HolderGeneration += 1;
		}
	}

	// The recycles' polls, in queue order (K6 review C1, fix (b)): a recycle whose ledger admission
	// was deferred is admitted once its slot's earlier job has applied -- in this same pass, when the
	// earlier recycle of the slot applies just ahead of it -- and every recycle whose job is done and
	// whose apply HoldApply(Return) does not hold is applied, its statuses read.
	void PollRecycles()
	{
		int32 I = 0;
		while (I < PendingRecycles.Num())
		{
			FPendingRecycle& Recycle = PendingRecycles[I];
			if (!Recycle.bLedgerAdmitted)
			{
				if (!IsPooled() || Ledger.SlotHasJob(Recycle.SlotIndex) || HasEarlierUnadmittedRecycle(I))
				{
					++I; // still behind its slot's earlier job
					continue;
				}
				Recycle.bLedgerAdmitted = true;
				const SuperSLMPaging::FAdmission Admission = Ledger.AdmitReset(Recycle.SlotIndex);
				if (Admission.Outcome == SuperSLMPaging::EPageAdmit::Admitted)
				{
					Recycle.LedgerTicket = Admission.Ticket; // its job ran (or runs) in place: see QueueRecycle()
				}
				else
				{
					// A plugin defect, as at QueueRecycle(): the reset runs untracked by the ledger, so the
					// slot is withheld once its statuses are read.
					NoteLedgerRefusal(Recycle.SlotIndex, FString::Printf(TEXT("the page ledger did not admit a deferred recycle's reset (outcome %d)"),
						static_cast<int32>(Admission.Outcome)));
					Slots[Recycle.SlotIndex].bWithheld = true;
				}
			}
			const FLifecycleAsyncState& Async = *Recycle.Async;
			if (!Async.Job.IsValid() || !Async.Job->bDone.load(std::memory_order_acquire))
			{
				++I;
				continue;
			}
			// Plan §3.9 (K6): HoldApply(Return) holds this recycle's apply once its job is done.
			if (PagedHoldBlocks(Recycle.HoldId))
			{
				++I;
				continue;
			}
			// Plugin paging plan §3.5 (K6): the recycle's ledger apply, before the slot can admit
			// its next holder's reset (one job per slot).
			ApplyLedgerReset(Recycle.SlotIndex, Recycle.LedgerTicket, !JobWasSkipped(Async.Job) && Async.bResetRan && Async.ResetStatus == FGpuStatus::SSLM_OK,
				Async.bNormalized);
			if (JobWasSkipped(Async.Job))
			{
				// Round 4 (finding 1): a recycle that never ran re-synced nothing, so the slot stays
				// out of the pool.
				Slots[Recycle.SlotIndex].bWithheld = true;
				UE_LOG(LogSuperSLM, Warning, TEXT("GPU slot %d withheld from vending: %s (its recycle did not run)."), Recycle.SlotIndex, LostBeforeRanText());
				PendingRecycles.RemoveAt(I, 1, EAllowShrinking::No);
				continue;
			}
			if (Async.bResetRan && (Async.ResetStatus != FGpuStatus::SSLM_OK || Async.UnbindSchemaStatus != FGpuStatus::SSLM_OK ||
				Async.UnbindAdapterStatus != FGpuStatus::SSLM_OK))
			{
				FGameSlot& S = Slots[Recycle.SlotIndex];
				S.bWithheld = true;
				UE_LOG(LogSuperSLM, Error, TEXT("GPU slot %d withheld from vending until teardown: the recycle's sslm_gpu_seq_reset returned %s, the schema unbind %s, the adapter unbind %s%s."),
					Recycle.SlotIndex, *GpuStatusText(Async.ResetStatus), *GpuStatusText(Async.UnbindSchemaStatus), *GpuStatusText(Async.UnbindAdapterStatus),
					S.bVended ? TEXT(" (it was already vended again; it is withheld once that holder returns it)") : TEXT(""));
			}
			PendingRecycles.RemoveAt(I, 1, EAllowShrinking::No);
		}
	}

	// True when a recycle of the same slot queued before index Index is still unadmitted: recycles of
	// one slot are admitted in the order they were queued.
	bool HasEarlierUnadmittedRecycle(int32 Index) const
	{
		const int32 SlotIndex = PendingRecycles[Index].SlotIndex;
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (PendingRecycles[Earlier].SlotIndex == SlotIndex && !PendingRecycles[Earlier].bLedgerAdmitted)
			{
				return true;
			}
		}
		return false;
	}

	// Plan §2.5 row 20 rule 1 (H6, H7): the only code that ends a hold on a GPU slot. The
	// per-user struct is reset by value (LayersPerSliceOverride included), the slot leaves the id
	// map and becomes free, and the recycle resets and unbinds the Layer-1 sequence on the
	// submission thread, ahead of any job of the next holder on the one FIFO.
	void ReleaseUser(int32 SlotIndex)
	{
		FGameSlot& S = Slots[SlotIndex];
		IdToSlot.Remove(S.Id);
		DropPendingEvents(S); // ruling 2026-09-26: logged as dropped before the by-value reset
		static_cast<FGameSlotUser&>(S) = FGameSlotUser(); // WalkState back to kSslmGpuDfaWalkStateUnused too (ruling 2026-09-26)
		S.OpLog.Reset();
		S.OpFront = 0;
		S.bVended = false;
		S.bReturnPending = false;
		S.Id = 0;
		S.bBindEligibleAfterQueue = true; // the recycle is a queued reset (row 4)
		// Round 3 (finding 2): after a confirmed loss no recycle is enqueued; the teardown releases
		// every Layer-1 sequence.
		if (Thread != nullptr && bActive && !bTerminalLoss.load())
		{
			QueueRecycle(SlotIndex);
		}
	}

	// ------------------------------------------------------------------ the per-sequence software
	// queue: admission (D-SLM7418/D-SLM7421/D-SLM7424/D-SLM7428/D-SLM7457; plan §5 GPU path /
	// §10.3 item 8)

	// The count that gates a new request's admission against the header's own "already holding
	// MaxQueuedOperationsPerSequence queued entries" bound -- the whole active window (OpFront to
	// the end: the currently Submitted entry, if any, plus every entry still waiting behind it).
	// A request that would make this window exceed the bound is refused at once, matching
	// GetPendingLifecycleOperationCount()'s own base term exactly, so the bound and the reported
	// count are the same quantity measured the same way.
	// Ruling 2026-09-26 (finding 9, corrected by round 2's finding 1): a bind that is the submitted
	// front is left out. SetSchema() now always holds its bind, and at the legal
	// MaxQueuedOperationsPerSequence = 1 a counted bind would refuse every RequestBeginGeneration()
	// that follows a SetSchema() as queue-full. Only a bind that is the submitted front is left
	// out: a bind still waiting behind other entries counts as an ordinary entry (D-SLM7758's
	// queued-bind cell depends on that), so a caller looping SetSchema() cannot grow the log without
	// bound (the window stays within MaxQueuedOperationsPerSequence + 1).
	int32 ActiveWindowCount(const FGameSlot& S) const
	{
		return S.OpLog.Num() - S.OpFront - UncountedBindCount(S);
	}

	// 1 when S's op-log front is a SchemaBind entry that has been submitted -- a bind that is the
	// submitted front, which ActiveWindowCount() and GetPendingLifecycleOperationCount() leave
	// out -- else 0. Game thread.
	static int32 UncountedBindCount(const FGameSlot& S)
	{
		return S.OpFront < S.OpLog.Num() && S.OpLog[S.OpFront].Kind == ELifecycleOpKind::SchemaBind &&
			S.OpLog[S.OpFront].bSubmitted ? 1 : 0;
	}

	// D-SLM7946 (plan §10.5.1.1 item 1): the phase a generation requested now will find when its
	// turn comes, assuming every reset queued ahead of it succeeds.
	struct FGenerationTurn
	{
		bool bIdle = false;
		bool bGenerationQueued = false; // a generation is already queued, so the answer is "not Idle"
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
	};

	// Walks S's unresolved op-log entries (OpFront to the end, result still Pending) from the newest
	// to the oldest: a BeginGeneration answers "not Idle"; a Reset, queued or admitted and not yet
	// resolved, answers Idle; every other kind is skipped. With none of these, the answer is the
	// phase now. No in-flight term is needed: an admitted reset stays an unresolved entry at the
	// front until it resolves, and a restore hands over its sequence only once it has resolved. It
	// depends only on the calls made, never on when ticks ran between them. Game thread.
	static FGenerationTurn ProjectGenerationTurn(const FGameSlot& S)
	{
		FGenerationTurn Turn;
		for (int32 Index = S.OpLog.Num() - 1; Index >= S.OpFront; --Index)
		{
			const FLifecycleOpEntry& Entry = S.OpLog[Index];
			if (Entry.Result != ESuperSLMRestoreResult::Pending)
			{
				continue;
			}
			if (Entry.Kind == ELifecycleOpKind::BeginGeneration)
			{
				Turn.bGenerationQueued = true;
				Turn.Phase = S.Phase;
				return Turn;
			}
			if (Entry.Kind == ELifecycleOpKind::Reset)
			{
				Turn.bIdle = true;
				return Turn;
			}
		}
		Turn.Phase = S.Phase;
		Turn.bIdle = Turn.Phase == ESuperSLMSequencePhase::Idle;
		return Turn;
	}

	static const TCHAR* SequencePhaseText(ESuperSLMSequencePhase Phase)
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

	// D-SLM7946 (plan §10.5.1.1 item 1.5): RequestBeginGeneration()'s call-time refusal. It names
	// the rule, the state that fails it and RequestResetSequence() as the remedy; it never uses the
	// queue bound's own wording.
	static FString OneGenerationRefusalText(const FGenerationTurn& Turn)
	{
		if (Turn.bGenerationQueued)
		{
			return TEXT("one generation at a time: a generation is already queued on this sequence and no reset is queued after it; call RequestResetSequence() first");
		}
		FString Text = FString::Printf(TEXT("one generation at a time: this sequence is %s and no reset is queued after its last generation; call RequestResetSequence() first"),
			SequencePhaseText(Turn.Phase));
		switch (Turn.Phase)
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

	// Appends a new, not-yet-admitted entry to Sequence's own queue, or refuses at once (invalid
	// handle, LastLifecycleRequestError set) if the queue is already at its configured bound --
	// before anything is queued and before Layer 1 is asked (plan §5). *OutSlotIndex is always
	// written when a live sequence is found, so callers with kind-specific extra validation can
	// still report a slot even on refusal.
	FSuperSLMLifecycleOpHandle EnqueueSlotOp(const FSuperSLMGpuSequence& Sequence, ELifecycleOpKind Kind, int32* OutSlotIndex, FLifecycleOpEntry** OutEntry)
	{
		*OutEntry = nullptr;
		int32 SlotIndex = INDEX_NONE;
		FGameSlot* S = FindSlot(Sequence, &SlotIndex);
		if (OutSlotIndex != nullptr)
		{
			*OutSlotIndex = SlotIndex;
		}
		// Round 3 (finding 2): refused after a confirmed loss too (matching EnqueueRestoreOp()), so
		// every Request...() and SetSchema() is refused by name once the backend is not active.
		if (S == nullptr || !bActive || bTerminalLoss.load() || Thread == nullptr || S->bReturnPending)
		{
			LastLifecycleRequestError = TEXT("not a live GPU sequence handle");
			return FSuperSLMLifecycleOpHandle();
		}
		// D-SLM7946 (plan §10.5.1.1): a sequence runs one generation at a time. A generation is
		// refused here, at the call, unless the sequence will be Idle when its turn comes: Idle with
		// nothing queued, or a reset queued (or admitted and unresolved) after its last generation.
		// It is never queued and nothing on the sequence changes. After the live-handle refusal and
		// before the queue bound, so a request that can never be honoured names that reason, not
		// the transient one.
		if (Kind == ELifecycleOpKind::BeginGeneration)
		{
			const FGenerationTurn Turn = ProjectGenerationTurn(*S);
			if (!Turn.bIdle)
			{
				LastLifecycleRequestError = OneGenerationRefusalText(Turn);
				return FSuperSLMLifecycleOpHandle();
			}
		}
		if (ActiveWindowCount(*S) >= Config.MaxQueuedOperationsPerSequence)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("the sequence's per-sequence queue is already at its configured bound (%d); "
				"the request is refused before anything is queued and before Layer 1 is asked"), Config.MaxQueuedOperationsPerSequence);
			return FSuperSLMLifecycleOpHandle();
		}
		FLifecycleOpEntry Entry;
		Entry.HandleId = NextLifecycleOpHandleId++;
		Entry.Kind = Kind;
		Entry.GlobalOrdinal = NextGlobalRequestOrdinal++;
		Entry.Owner = S->Id;
		const int32 Index = S->OpLog.Add(MoveTemp(Entry));
		*OutEntry = &S->OpLog[Index];
		FSuperSLMLifecycleOpHandle H;
		H.Id = (*OutEntry)->HandleId;
		return H;
	}

	// Admits SlotIndex's own front entry NOW, synchronously within the Request*() call that just
	// queued it, when the queue was empty (this entry IS the front and nothing is already
	// Submitted) -- matching the prior synchronous API's own timing for the common case (an idle
	// sequence with nothing queued): BeginGeneration's Phase flip is then observable by the
	// caller's very next line, with no intervening Tick() needed. Ruling 2026-09-26 (finding 10):
	// that holds only when nothing is queued ahead. SetSchema() always holds its bind, so a
	// RequestBeginGeneration() after a SetSchema() finds a bind that is the submitted front (left
	// out of the window, ActiveWindowCount(), so the request is accepted) and does not admit here: GetPhase() reads Idle, not Prefilling, until the tick after the bind's job
	// finishes, when AdmitQueuedOps() admits it. When the queue was NOT empty, this is a no-op and the entry is picked
	// up later by AdmitQueuedOps()'s own tick-driven poll-then-admit pass, in global-ordinal order
	// against whatever else becomes eligible that same tick (D-SLM7428) -- immediate admission
	// here never competes with that pass for ordering, because a request that admits immediately
	// was, by construction, the only thing eligible for this sequence at that instant. Called only
	// after the caller has finished populating the entry's own kind-specific fields (Request/
	// NewDecodePath), since admission reads them.
	void TryAdmitImmediately(int32 SlotIndex)
	{
		if (SlotIndex == INDEX_NONE)
		{
			return;
		}
		FGameSlot& S = Slots[SlotIndex];
		if (S.OpLog.IsValidIndex(S.OpFront) && !S.OpLog[S.OpFront].bSubmitted)
		{
			AdmitSlotFront(SlotIndex);
		}
	}

	// --- Admission: turns the front-of-queue entry into real (or immediately-resolved) work. ---

	// D-SLM7948: the admission stops a running generation and changes nothing the caller reads. Its
	// pending events are dropped, so no token it computed is applied; its active-generation index
	// is cleared; and QueueReset()'s generation bump makes every result still in flight stale.
	// Planning stays closed while this reset is the submitted front (HasBlockingLifecycleOp()). The
	// phase, tokens, request, last outcome, fault and decode path stay as they were until the reset
	// resolves: PollSlotFront() clears the generation and sets the new decode path when it succeeds,
	// and faults the sequence, keeping its tokens, when Layer 1 refuses it.
	void AdmitResetOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		Entry.bSubmitted = true;
		bool bNormalize = false;
		int64 LimitInPlace = 0;
		int64 LimitNormalized = 0;
		// Plugin paging plan §3.5 (K6): in pooled mode the page ledger admits every reset at the head
		// of its queue (AdmitSlotFront() waits while the slot still has a ledger job, so one job per
		// slot holds). A refusal here is a plugin defect: the reset is refused by name, nothing is
		// posted, and the holder is faulted, so it never runs untracked.
		if (IsPooled())
		{
			const SuperSLMPaging::FAdmission Admission = Ledger.AdmitReset(SlotIndex);
			if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
			{
				const FString Reason = FString::Printf(TEXT("the page ledger did not admit a reset at the head of the sequence's queue (outcome %d)"),
					static_cast<int32>(Admission.Outcome));
				NoteLedgerRefusal(SlotIndex, Reason);
				if (S.Id == Entry.Owner)
				{
					FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection, Reason);
				}
				ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
				LastLifecycleRequestError = Reason;
				return;
			}
			Entry.LedgerTicket = Admission.Ticket;
			Entry.HoldId = AttachPagedHold(EGpuPagedJobKind::Reset);
			bNormalize = Admission.bNormalizes; // K7: create-first, charged R(SequenceBudget) on H at this admission
			ResetLimits(SlotIndex, LimitInPlace, LimitNormalized);
		}
		DropPendingEvents(S);
		S.ActiveGenerationOpLogIndex = INDEX_NONE;
		Entry.Async = MakeShared<FLifecycleAsyncState>();
		Entry.Async->Job = QueueReset(SlotIndex, /*bUnbind*/ false, Entry.Async, bNormalize, LimitInPlace, LimitNormalized);
	}

	void AdmitAdoptPrefixOp(FLifecycleOpEntry& Entry)
	{
		// D-SLM7457 ruling (1): no GPU prefix-adopt verb exists at v1.5.0 or at the 1.7.0, 1.9.0 and 1.11.0 pins
		// (gpu_1p0.h and gpu_port.h name no adopt verb). Admits and resolves
		// directly, without ever forwarding to the submission thread.
		Entry.bSubmitted = true;
		ResolveEntry(Entry, ESuperSLMRestoreResult::UnsupportedOnGpu);
		// Plan rev 1.9 ruling 2 (K6): the prefix overload in dedicated mode names the mode and the
		// field that selects it, as the dedicated prefix verbs do.
		LastLifecycleRequestError = Entry.bPrefixOverload
			? TEXT("RequestAdoptPrefix(Sequence, Prefix) is unsupported on a dedicated GPU subsystem: GPU prefixes need pooled mode (SequenceBudget > 0); refused by name")
			: TEXT("AdoptPrefix has no GPU counterpart in 1.0: shared prefixes are CPU-only; refused by name");
	}

	// Plugin paging plan §3.4 "Adopt" (K7), pooled mode: RequestAdoptPrefix(Sequence, Prefix) at the
	// head of its sequence's queue (AdmitSlotFront() waits while the slot has a ledger job open). The
	// page ledger admits it (it draws nothing, so it charges nothing, and pins the prefix until its
	// apply), and the job runs sslm_gpu_seq_adopt_prefix on the submission thread. On success the
	// thread slot takes the prefix's length as its ContextUsed and origin, its limit min(L + budget,
	// ContextCap), and the prefix's ready flag as armed (§3.4 "The armed flag"). A prefix released or
	// faulted after the call is refused here by name, with nothing posted.
	void AdmitPooledAdoptOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		Entry.bSubmitted = true;
		const FGpuPrefixEntry* Prefix = Prefixes.Find(Entry.AdoptPrefixId);
		if (Prefix == nullptr || Prefix->bReleased || Prefix->Phase != ESuperSLMPrefixPhase::Ready)
		{
			LastLifecycleRequestError = TEXT("Malformed: the prefix was released, or its build failed, before the adopt reached the head of the sequence's queue; nothing was posted");
			ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed);
			return;
		}
		const SuperSLMPaging::FAdmission Admission = Ledger.AdmitAdoptPrefix(SlotIndex, Prefix->Node);
		if (Admission.Outcome == SuperSLMPaging::EPageAdmit::KvModeMismatch)
		{
			LastLifecycleRequestError = TEXT("KvModeMismatch: the holder is a whole-reserve sequence, which cannot adopt until a reset has normalized it");
			ResolveEntry(Entry, ESuperSLMRestoreResult::KvModeMismatch);
			return;
		}
		if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
		{
			const FString Reason = FString::Printf(TEXT("the page ledger did not admit an adopt at the head of the sequence's queue (outcome %d)"),
				static_cast<int32>(Admission.Outcome));
			NoteLedgerRefusal(SlotIndex, Reason);
			LastLifecycleRequestError = Reason;
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			return;
		}
		Entry.LedgerTicket = Admission.Ticket;
		Entry.HoldId = AttachPagedHold(EGpuPagedJobKind::Adopt);
		JobsPostedByKind[static_cast<int32>(EGpuPagedJobKind::Adopt)] += 1;
		// The holder keeps its own budget; its origin becomes the prefix's length (an adopt that faults
		// after the share leaves it reset: origin 0, the shape FinalizeAdoptOp() then applies to the
		// ledger, so the two limits agree; K7 review S1).
		SuperSLMPaging::FHolderShape Shape = Ledger.SlotShape(SlotIndex);
		Shape.Origin = Prefix->Length;
		const int64 LimitAfter = LedgerHolderLimit(Shape);
		Shape.Origin = 0;
		const int64 LimitReset = LedgerHolderLimit(Shape);
		const int64 PrefixId = Prefix->Id;
		const int64 Length = Prefix->Length;
		const bool bReady = Prefix->bReady;
		Entry.Async = MakeShared<FLifecycleAsyncState>();
		TSharedPtr<FLifecycleAsyncState> Async = Entry.Async;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this, SlotIndex, PrefixId, Length, bReady, LimitAfter, LimitReset, Async]()
		{
			FThreadSlot& TS = ThreadSlots[SlotIndex];
#if WITH_DEV_AUTOMATION_TESTS
			const FGpuStatus Forced = TakeTestStatus(&TestNextAdoptStatus);
			const FGpuStatus FaultAfterShare = TakeTestStatus(&TestAdoptFaultAfterShare);
#else
			const FGpuStatus Forced = FGpuStatus::SSLM_OK;
			const FGpuStatus FaultAfterShare = FGpuStatus::SSLM_OK;
#endif
			const FThreadPrefix* ThreadPrefix = ThreadPrefixes.Find(PrefixId);
			FGpuStatus St;
			if (Forced != FGpuStatus::SSLM_OK)
			{
				St = Forced; // §3.9: the engine is left as the refusal would leave it (unchanged)
			}
			else if (TS.Handle == nullptr || ThreadPrefix == nullptr || ThreadPrefix->Handle == nullptr)
			{
				St = FGpuStatus::SSLM_GPU_PREFIX_REJECTED;
			}
			else
			{
				superslm_gpu::harness::Device& Device = superslm_gpu::harness::GetDevice();
				const uint64 Before = Device.submission_counter();
				St = sslm_gpu_seq_adopt_prefix(Ctx, TS.Handle, ThreadPrefix->Handle);
				// The singleton advances this counter after ExecuteCommandLists, including an
				// upload whose later Signal/wait fails. Empty/aligned adopts issue no submission.
				// The worker holds the process-wide Layer-1 lock across both observations.
				CountSubmission(static_cast<int64>(Device.submission_counter() - Before));
			}
			NoteLayer1Status(St);
			if (St == FGpuStatus::SSLM_OK && FaultAfterShare != FGpuStatus::SSLM_OK)
			{
				// §3.4 "A fault after the share": the seam reproduces the documented post-state -- the
				// real adopt ran; the engine sequence is reset, and the fault is the adopt's result.
				const FGpuStatus ResetSt = sslm_gpu_seq_reset(Ctx, TS.Handle);
				NoteLayer1Status(ResetSt);
				St = FaultAfterShare;
				Async->bAdoptResetByFault = true;
			}
			else if ((St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED || St == FGpuStatus::SSLM_DEVICE_LOST) && TS.Handle != nullptr)
			{
				// The engine's own fault after the share leaves the sequence reset (gpu_1p0.h); a fault
				// before it changes nothing. The engine's context_length tells the two apart (the
				// accessor's use here is justified at its declaration; K7 review N3).
				const int64_t* EngineLength = SslmGpuSeqHandleContextLengthForBench(TS.Handle);
				Async->bAdoptResetByFault = EngineLength != nullptr && *EngineLength == 0 && TS.ContextUsed != 0;
			}
			Async->AdoptStatus = St;
			if (St == FGpuStatus::SSLM_OK || Async->bAdoptResetByFault)
			{
				const bool bAdopted = St == FGpuStatus::SSLM_OK;
				TS.ContextUsed = bAdopted ? Length : 0;
				TS.bArmed = bAdopted && bReady;
				TS.Limit = bAdopted ? LimitAfter : LimitReset;
				TS.bPrimed = false;
				TS.bStopped = false;
				TS.bHasStopFault = false;
				TS.StopFault = FActionResult();
				TS.LastToken = -1;
				TS.Produced = 0;
				TS.MaxNewTokens = 0;
				TS.StopTokenIds.Reset();
				TS.PinnedAdapter = nullptr;
			}
			Async->ContextUsedAfter = TS.ContextUsed;
			Async->bArmedAfter = TS.bArmed;
			Async->WalkState = TS.Handle != nullptr ? SslmGpuSeqWalkStateForG5Bridge(TS.Handle) : kSslmGpuDfaWalkStateUnused;
			SnapshotPoolStatsOnThread();
		};
		Async->Job = Enqueue(Job);
	}

	// K7: the pooled adopt's completion. The ledger's apply runs with what the job did (a refusal
	// leaves the slot's ledger shape and dependency as they were), then the slot's mirrors take the
	// thread slot's figures. A successful adopt clears the generation (the sequence now holds the
	// prefix and nothing else) and the bind eligibility (a successful GPU adopt revokes the bind,
	// §3.4); a refused one changes neither. A fault after the share left the engine sequence reset,
	// so the ledger then applies the reset shape (plan §3.4 rev 1.10, K7 review S1: origin 0, the
	// holder's own budget, its dependency dropped -- the shape a reset in place leaves), the
	// generation is cleared, and the bind permission the engine granted again is mirrored (K7 review
	// N1).
	void FinalizeAdoptOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		const FLifecycleAsyncState& Async = *Entry.Async;
		const bool bSkipped = JobWasSkipped(Async.Job);
		const bool bOk = !bSkipped && Async.AdoptStatus == FGpuStatus::SSLM_OK;
		if (Entry.LedgerTicket != SuperSLMPaging::InvalidTicket)
		{
			Ledger.ApplyAdopt(Entry.LedgerTicket, bOk);
			Entry.LedgerTicket = SuperSLMPaging::InvalidTicket;
			if (!bSkipped && Async.bAdoptResetByFault)
			{
				ApplyLedgerResetShape(SlotIndex);
			}
		}
		if (bSkipped)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			LastLifecycleRequestError = LostBeforeRanText();
			return;
		}
		if (S.Id == Entry.Owner)
		{
			S.ContextUsedMirror = Async.ContextUsedAfter;
			S.bArmedMirror = Async.bArmedAfter;
			S.WalkState = Async.WalkState;
			if (bOk || Async.bAdoptResetByFault)
			{
				ClearGeneration(S);
			}
			if (bOk)
			{
				S.bBindEligibleAfterQueue = false;
			}
			else if (Async.bAdoptResetByFault)
			{
				S.bBindEligibleAfterQueue = true; // K7 review N1: the engine's reset granted the bind again (gpu_1p0.h, adopt)
			}
		}
		if (bOk)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::Success);
			return;
		}
		// The mapping: a schema mismatch is Malformed by name (the CPU twin's outcome, cell 8.2); every
		// other status reads as a pooled restore's would (§3.7).
		const ESuperSLMRestoreResult Result = Async.AdoptStatus == FGpuStatus::SSLM_GPU_PREFIX_SCHEMA_MISMATCH
			? ESuperSLMRestoreResult::Malformed
			: SuperSLMGpuStatusMapping::ToRestoreFailureResult(Async.AdoptStatus, /*bPooled*/ true, /*bSchemaCrossCheckFailed*/ false);
		LastLifecycleRequestError = FString::Printf(TEXT("sslm_gpu_seq_adopt_prefix returned %s%s"), *GpuStatusText(Async.AdoptStatus),
			Async.bAdoptResetByFault ? TEXT(" after the share; the sequence was left reset (ContextUsed 0, unarmed)") : TEXT(""));
		ResolveEntry(Entry, Result);
	}

	// Plan §3.4 rev 1.10 (K7 review S1): the engine reset the sequence inside a pooled adopt that
	// faulted after the share, so the slot's ledger shape follows it to the reset shape, through the
	// core's own reset verbs: a reset admitted and applied in place at once (the engine's reset has
	// already run). A normalization the admission would ask for is applied as ResetInPlace, as at a
	// deferred recycle, and retried at the slot's next reset. The thread slot took the same limit in
	// the adopt's job (FThreadSlot::Limit, LimitReset). Game thread, right after ApplyAdopt().
	void ApplyLedgerResetShape(int32 SlotIndex)
	{
		const SuperSLMPaging::FAdmission Admission = Ledger.AdmitReset(SlotIndex);
		if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
		{
			NoteLedgerRefusal(SlotIndex, FString::Printf(TEXT("the page ledger did not admit the reset shape after an adopt that faulted after the share (outcome %d)"),
				static_cast<int32>(Admission.Outcome)));
			return;
		}
		SuperSLMPaging::FTicket Ticket = Admission.Ticket;
		ApplyLedgerReset(SlotIndex, Ticket, /*bRan*/ true, /*bNormalized*/ false);
	}

	// ------------------------------------------------------------------ GPU prefixes (K7)

	// Plugin paging plan §3.3, §3.4 "Prefixes" (K7), pooled mode: CreatePrefix(), its schema-bound
	// overload (SchemaIndex >= 0, Kind 1 for SchemaContent) and CreatePrefixFrom() (ParentId != 0).
	// Every refusal is decided here, on the game thread, before anything is charged or posted: the
	// parent first (a nested build), then the plugin's cap, then the vocabulary, then the page
	// ledger's count and page rules. An admitted build charges R(max(L, 1)) on P, mints its id from
	// the shared counter, and posts its begin job (sslm_gpu_prefix_begin or begin_from, then
	// set_schema, then, for an empty prefix, freeze); its tokens are then fed by the build seat, one
	// whole token per turn (PlanAndIssue()). Game thread.
	ESuperSLMPrefixResult CreateGpuPrefix(const TArray<int32>& Tokens, int64 ParentId, int32 SchemaIndex, int32 Kind, FSuperSLMPrefix& OutPrefix, FString& OutError)
	{
		OutPrefix = FSuperSLMPrefix();
		const bool bNested = ParentId != 0;
		SuperSLMPaging::FNodeId ParentNode = SuperSLMPaging::InvalidNode;
		int64 ParentLength = 0;
		bool bParentReady = false;
		if (bNested)
		{
			const FGpuPrefixEntry* Parent = Prefixes.Find(ParentId);
			if (Parent == nullptr || Parent->bReleased || Parent->Phase != ESuperSLMPrefixPhase::Ready)
			{
				OutError = Parent == nullptr
					? FString(TEXT("ParentNotUsable: the parent is not a GPU prefix of this subsystem (unknown, or another backend's)"))
					: FString(TEXT("ParentNotUsable: the parent is released, faulted or not yet frozen; a nested prefix continues a Ready parent"));
				return ESuperSLMPrefixResult::ParentNotUsable;
			}
			ParentNode = Parent->Node;
			ParentLength = Parent->Length;
			bParentReady = Parent->bReady;
		}
		const int64 Length = ParentLength + Tokens.Num();
		if (Length > Config.ContextCap)
		{
			OutError = FString::Printf(TEXT("Malformed: the prefix would hold %lld positions (parent %lld + %d tokens), above this subsystem's ContextCap (%lld)"),
				Length, ParentLength, Tokens.Num(), Config.ContextCap);
			return ESuperSLMPrefixResult::Malformed;
		}
		for (const int32 Token : Tokens)
		{
			if (Token < 0 || Token >= VocabSize)
			{
				OutError = FString::Printf(TEXT("Malformed: prefix token %d is outside the vocabulary [0, %d)"), Token, VocabSize);
				return ESuperSLMPrefixResult::Malformed;
			}
		}
		const SuperSLMPaging::FAdmission Admission = bNested ? Ledger.AdmitNestedPrefixBuild(ParentNode, Tokens.Num()) : Ledger.AdmitPrefixBuild(Tokens.Num());
		switch (Admission.Outcome)
		{
			case SuperSLMPaging::EPageAdmit::Admitted:
				break;
			case SuperSLMPaging::EPageAdmit::PrefixLimit:
				OutError = FString::Printf(TEXT("PrefixLimit: PrefixBlockCount (%d) prefixes and nested children are already live"), Config.PrefixBlockCount);
				return ESuperSLMPrefixResult::PrefixLimit;
			case SuperSLMPaging::EPageAdmit::KvPagesExhausted:
				OutError = FString::Printf(TEXT("KvPagesExhausted: region P has %u pages free and the build is charged R(max(%d, 1)); released prefixes still shared by a sequence hold their pages until it lets go"),
					Ledger.RegionFree(SuperSLMPaging::ERegion::P), Tokens.Num());
				return ESuperSLMPrefixResult::KvPagesExhausted;
			case SuperSLMPaging::EPageAdmit::ParentNotUsable:
				OutError = TEXT("ParentNotUsable: the page ledger does not hold the parent as a live, frozen prefix");
				return ESuperSLMPrefixResult::ParentNotUsable;
			case SuperSLMPaging::EPageAdmit::NotConfigured:
				OutError = TEXT("NotConfigured: the page ledger is not configured");
				return ESuperSLMPrefixResult::NotConfigured;
			case SuperSLMPaging::EPageAdmit::InvalidRequest:
			case SuperSLMPaging::EPageAdmit::Waiting:
			case SuperSLMPaging::EPageAdmit::KvModeMismatch:
			case SuperSLMPaging::EPageAdmit::PrefixPinned:
			case SuperSLMPaging::EPageAdmit::ContextCapMismatch:
				// The checks above make these unreachable for a build: a plugin defect, refused by name.
				OutError = FString::Printf(TEXT("Malformed: the page ledger refused the build (outcome %d)"), static_cast<int32>(Admission.Outcome));
				UE_LOG(LogSuperSLM, Error, TEXT("GPU CreatePrefix: %s (plugin defect)."), *OutError);
				return ESuperSLMPrefixResult::Malformed;
		}

		const int64 Id = SuperSLMMintPrefixId();
		FGpuPrefixEntry& Entry = Prefixes.Add(Id);
		Entry.Id = Id;
		Entry.Node = Admission.Node;
		Entry.Phase = ESuperSLMPrefixPhase::Pending;
		Entry.Tokens = Tokens;
		Entry.Kind = Kind;
		Entry.ParentId = ParentId;
		Entry.Length = Length;
		// §3.4 "The armed flag": a prefix's last write is its prefill arc when it fed a token; an empty
		// nested child keeps its parent's; an empty root has none.
		Entry.bReady = Tokens.Num() > 0 ? true : (bNested && bParentReady);
		Entry.BuildTicket = Admission.Ticket;
		const EGpuPagedJobKind JobKind = bNested ? EGpuPagedJobKind::NestedBuild : EGpuPagedJobKind::PrefixBuild;
		Entry.HoldId = AttachPagedHold(JobKind);
		JobsPostedByKind[static_cast<int32>(JobKind)] += 1;
		Entry.BeginState = MakeShared<FPrefixJobState>();
		TSharedPtr<FPrefixJobState> JobState = Entry.BeginState;
		const int32 Budget = FMath::Max(Tokens.Num(), 1); // §3.3 "Empty prefixes": the budget domain starts at 1
		const bool bFreezeNow = Tokens.Num() == 0;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this, Id, ParentId, bNested, Budget, SchemaIndex, bFreezeNow, JobState]()
		{
#if WITH_DEV_AUTOMATION_TESTS
			const FGpuStatus Forced = TakeTestStatus(bNested ? &TestNextNestedBuildStatus : &TestNextPrefixBuildStatus);
#else
			const FGpuStatus Forced = FGpuStatus::SSLM_OK;
#endif
			SslmGpuPrefixHandle* Handle = nullptr;
			FGpuStatus St = FGpuStatus::SSLM_OK;
			const TCHAR* Where = bNested ? TEXT("sslm_gpu_prefix_begin_from") : TEXT("sslm_gpu_prefix_begin");
			if (Forced != FGpuStatus::SSLM_OK)
			{
				St = Forced; // §3.9: the refusal is the engine's, which draws nothing
			}
			else if (bNested)
			{
				const FThreadPrefix* ParentPrefix = ThreadPrefixes.Find(ParentId);
				St = (ParentPrefix != nullptr && ParentPrefix->Handle != nullptr)
					? sslm_gpu_prefix_begin_from(Ctx, ParentPrefix->Handle, static_cast<int32_t>(Budget), &Handle)
					: FGpuStatus::SSLM_GPU_PREFIX_REJECTED;
			}
			else
			{
				St = sslm_gpu_prefix_begin(Ctx, Model, Pool, static_cast<int32_t>(Budget), &Handle);
			}
			NoteLayer1Status(St);
			if (St == FGpuStatus::SSLM_OK && Handle != nullptr && SchemaIndex >= 0)
			{
				Where = TEXT("sslm_gpu_prefix_set_schema");
				St = sslm_gpu_prefix_set_schema(Ctx, Handle, static_cast<int32_t>(SchemaIndex));
				NoteLayer1Status(St);
			}
			if (St == FGpuStatus::SSLM_OK && Handle != nullptr && bFreezeNow)
			{
				Where = TEXT("sslm_gpu_prefix_freeze");
				St = sslm_gpu_prefix_freeze(Ctx, Handle);
				NoteLayer1Status(St);
			}
			if (St != FGpuStatus::SSLM_OK || Handle == nullptr)
			{
				if (Handle != nullptr)
				{
					NoteLayer1Status(sslm_gpu_prefix_release(Ctx, Handle));
				}
				JobState->Status = St != FGpuStatus::SSLM_OK ? St : FGpuStatus::SSLM_GPU_PREFIX_REJECTED;
				JobState->Where = Where;
				SnapshotPoolStatsOnThread();
				return;
			}
			FThreadPrefix& Added = ThreadPrefixes.Add(Id);
			Added.Handle = Handle;
			SnapshotPoolStatsOnThread();
		};
		Entry.BeginJob = Enqueue(Job);
		if (Tokens.Num() > 0)
		{
			BuildQueue.Add(Id);
		}
		OutPrefix.Id = Id;
		return ESuperSLMPrefixResult::Success;
	}

	// A build's end: its page-ledger apply (success settles its P charge to its own pages; failure
	// refunds it all and forgets the node), unless HoldApply holds it. The phase follows the apply,
	// so a Ready prefix is always one the ledger counts as built. Game thread.
	bool ApplyPrefixBuildEnd(FGpuPrefixEntry& Entry, bool bSucceeded, const FString& FaultMessage)
	{
		if (PagedHoldBlocks(Entry.HoldId))
		{
			return false;
		}
		if (Entry.BuildTicket != SuperSLMPaging::InvalidTicket)
		{
			Ledger.ApplyPrefixBuild(Entry.BuildTicket, bSucceeded);
			Entry.BuildTicket = SuperSLMPaging::InvalidTicket;
		}
		BuildQueue.Remove(Entry.Id);
		if (bSucceeded)
		{
			Entry.Phase = ESuperSLMPrefixPhase::Ready;
			return true;
		}
		Entry.Phase = ESuperSLMPrefixPhase::Faulted;
		Entry.bBuildFaulted = true;
		Entry.FaultMessage = FaultMessage;
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU prefix %lld faulted: %s; release it."), Entry.Id, *FaultMessage);
		return true;
	}

	// Plugin paging plan §3.4 (K7): each tick's poll of the prefix jobs, before admission. A begin job
	// done moves its prefix to Prefilling (or, empty and frozen in the job, to its build's apply); the
	// build turns done are read in order, the last one applying the build; a release job done applies
	// the release (its pages stay charged as a ghost while a sequence still shares them). Game thread.
	void PollPrefixBuilds()
	{
		for (auto It = Prefixes.CreateIterator(); It; ++It)
		{
			FGpuPrefixEntry& Entry = It.Value();
			if (Entry.bReleased)
			{
				if (Entry.ReleaseTicket != SuperSLMPaging::InvalidTicket && Entry.ReleaseJob.IsValid() && Entry.ReleaseJob->bDone.load(std::memory_order_acquire)
					&& !PagedHoldBlocks(Entry.ReleaseHoldId))
				{
					const FGpuStatus St = Entry.ReleaseJob->bSkipped ? FGpuStatus::SSLM_DEVICE_LOST : Entry.BeginState.IsValid() ? Entry.BeginState->Status : FGpuStatus::SSLM_OK;
					if (St == FGpuStatus::SSLM_OK)
					{
						Entry.bReleaseApplied = Ledger.ApplyPrefixRelease(Entry.ReleaseTicket);
					}
					else
					{
						// The engine did not release the pages (or the job never ran): they stay charged.
						UE_LOG(LogSuperSLM, Warning, TEXT("GPU prefix %lld: its release did not run to completion (%s); its pages stay charged until teardown."), Entry.Id, *GpuStatusText(St));
					}
					Entry.ReleaseTicket = SuperSLMPaging::InvalidTicket;
					Entry.ReleaseJob.Reset();
					Entry.BeginState.Reset();
				}
				if (Entry.bReleaseApplied && !Ledger.IsGhost(Entry.Node)) { It.RemoveCurrent(); }
				continue;
			}
			if (Entry.Phase == ESuperSLMPrefixPhase::Pending)
			{
				if (!Entry.BeginJob.IsValid() || !Entry.BeginJob->bDone.load(std::memory_order_acquire))
				{
					continue;
				}
				const bool bSkipped = JobWasSkipped(Entry.BeginJob);
				const FGpuStatus St = Entry.BeginState->Status;
				if (bSkipped || St != FGpuStatus::SSLM_OK)
				{
					const bool bApplied = ApplyPrefixBuildEnd(Entry, false, bSkipped ? FString(LostBeforeRanText()) : FString::Printf(TEXT("%s returned %s"), Entry.BeginState->Where, *GpuStatusText(St)));
					if (bApplied && !bSkipped && St == FGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED)
					{
						NoteLedgerFault(Entry.ParentId != 0 ? TEXT("sslm_gpu_prefix_begin_from") : TEXT("sslm_gpu_prefix_begin")); // §3.7: the ledger admitted it
					}
					continue;
				}
				if (Entry.Tokens.Num() == 0)
				{
					ApplyPrefixBuildEnd(Entry, true, FString());
					continue;
				}
				Entry.Phase = ESuperSLMPrefixPhase::Prefilling;
				Entry.BeginJob.Reset();
				continue;
			}
			if (Entry.Phase != ESuperSLMPrefixPhase::Prefilling)
			{
				continue;
			}
			while (Entry.TurnsInFlight.Num() > 0 && !Entry.bBuildFaulted)
			{
				const FPrefixTurn& Turn = Entry.TurnsInFlight[0];
				if (!Turn.Job.IsValid() || !Turn.Job->bDone.load(std::memory_order_acquire))
				{
					break;
				}
				const bool bSkipped = Turn.Job->bSkipped || !Turn.Job->Results.IsValidIndex(Turn.ActionIndex);
				const FActionResult* R = bSkipped ? nullptr : &Turn.Job->Results[Turn.ActionIndex];
				if (bSkipped || R->Kind == EResultKind::Fault || R->Kind == EResultKind::Skipped)
				{
					Entry.bBuildFaulted = true;
					Entry.FaultMessage = bSkipped || R->Kind == EResultKind::Skipped ? FString(LostBeforeRanText()) : R->Message;
					break;
				}
				Entry.BuiltTokens += 1;
				Entry.TurnsInFlight.RemoveAt(0, 1, EAllowShrinking::No);
			}
			if (Entry.bBuildFaulted)
			{
				// Every later turn of a faulted build reads the fault on the thread; none is planned now.
				if (ApplyPrefixBuildEnd(Entry, false, Entry.FaultMessage))
				{
					Entry.TurnsInFlight.Reset();
				}
				continue;
			}
			if (Entry.BuiltTokens == Entry.Tokens.Num())
			{
				ApplyPrefixBuildEnd(Entry, true, FString());
			}
		}
	}

	// The build seat's head (§3.4: one seat, further builds in arrival order): the prefix whose next
	// token a turn would feed, or nullptr when the seat has nothing to plan. Game thread.
	FGpuPrefixEntry* BuildSeatHead()
	{
		for (const int64 Id : BuildQueue)
		{
			FGpuPrefixEntry* Entry = Prefixes.Find(Id);
			if (Entry == nullptr || Entry->bReleased || Entry->bBuildFaulted)
			{
				continue;
			}
			if (Entry->Phase != ESuperSLMPrefixPhase::Prefilling || Entry->TurnsPlanned >= Entry->Tokens.Num())
			{
				return nullptr; // the head is still beginning, or its last turns are in flight
			}
			return Entry;
		}
		return nullptr;
	}

	// One build turn, planned into this tick's job at the seat's place in the one-call rotation.
	void PlanBuildTurn(FGpuPrefixEntry& Entry, const TSharedPtr<FJob>& Job)
	{
		FAction A;
		A.Kind = EActionKind::PrefixBuildTurn;
		A.Slot = INDEX_NONE;
		A.PrefixId = Entry.Id;
		A.PrefixKind = Entry.Kind;
		A.PromptToken = Entry.Tokens[Entry.TurnsPlanned];
		Entry.TurnsPlanned += 1;
		A.bLastPrompt = Entry.TurnsPlanned == Entry.Tokens.Num();
		FPrefixTurn Turn;
		Turn.Job = Job;
		Turn.ActionIndex = Job->Actions.Add(MoveTemp(A));
		Entry.TurnsInFlight.Add(MoveTemp(Turn));
		#if WITH_DEV_AUTOMATION_TESTS
		Entry.TurnTicks.Add(TickIndex);
		#endif
	}

	// ReleasePrefix(), pooled mode (§3.3 "Concurrency", §3.4). Refused by name, with nothing changed,
	// for an unknown or already released prefix, a prefix still building, or one an adopt, a nested
	// build or a hinted restore names (queued or admitted). A faulted prefix (its charge already
	// refunded) is forgotten at once. Otherwise the ledger frees its count slot now, and the release
	// job runs sslm_gpu_prefix_release on the submission thread; its pages are refunded at the job's
	// apply, or kept as a ghost while a sequence still shares them. Game thread.
	bool ReleaseGpuPrefix(const FSuperSLMPrefix& Prefix, FString& OutError)
	{
		FGpuPrefixEntry* Entry = Prefix.IsValid() ? Prefixes.Find(Prefix.Id) : nullptr;
		if (Entry == nullptr || Entry->bReleased)
		{
			OutError = TEXT("not a live GPU prefix of this subsystem (unknown, released, or another backend's)");
			return false;
		}
		if (Entry->Phase == ESuperSLMPrefixPhase::Faulted)
		{
			BuildQueue.Remove(Entry->Id);
			Prefixes.Remove(Prefix.Id);
			return true;
		}
		if (Entry->Phase != ESuperSLMPrefixPhase::Ready)
		{
			OutError = TEXT("the prefix is still building; release it once GetPrefixPhase() reads Ready or Faulted");
			return false;
		}
		for (const FGameSlot& S : Slots)
		{
			for (int32 Index = S.OpFront; Index < S.OpLog.Num(); ++Index)
			{
				const FLifecycleOpEntry& Op = S.OpLog[Index];
				if (Op.Kind == ELifecycleOpKind::AdoptPrefix && Op.AdoptPrefixId == Prefix.Id && Op.Result == ESuperSLMRestoreResult::Pending)
				{
					OutError = TEXT("PrefixPinned: an adopt of this prefix is queued; release it once the adopt has resolved");
					return false;
				}
			}
		}
		for (const FLifecycleOpEntry& Op : RestoreOps)
		{
			if (Op.RestoreHintPrefixId == Prefix.Id && Op.Result == ESuperSLMRestoreResult::Pending)
			{
				OutError = TEXT("PrefixPinned: a restore hinted with this prefix is queued; release it once the restore has resolved");
				return false;
			}
		}
		const SuperSLMPaging::FAdmission Admission = Ledger.AdmitPrefixRelease(Entry->Node);
		if (Admission.Outcome != SuperSLMPaging::EPageAdmit::Admitted)
		{
			OutError = Admission.Outcome == SuperSLMPaging::EPageAdmit::PrefixPinned
				? FString(TEXT("PrefixPinned: an adopt, a nested build or a hinted restore of this prefix is in flight; release it once it has resolved"))
				: FString::Printf(TEXT("the page ledger did not admit the release (outcome %d)"), static_cast<int32>(Admission.Outcome));
			return false;
		}
		Entry->bReleased = true;
		Entry->Tokens.Empty();
		Entry->TurnsInFlight.Empty();
		Entry->FaultMessage.Empty();
		Entry->BeginJob.Reset();
		#if WITH_DEV_AUTOMATION_TESTS
		Entry->TurnTicks.Empty();
		#endif
		Entry->Phase = ESuperSLMPrefixPhase::Invalid;
		Entry->ReleaseTicket = Admission.Ticket;
		Entry->ReleaseHoldId = AttachPagedHold(EGpuPagedJobKind::PrefixRelease);
		JobsPostedByKind[static_cast<int32>(EGpuPagedJobKind::PrefixRelease)] += 1;
		// The release job's status rides in the begin state (the begin job is long done).
		Entry->BeginState = MakeShared<FPrefixJobState>();
		TSharedPtr<FPrefixJobState> JobState = Entry->BeginState;
		const int64 Id = Prefix.Id;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this, Id, JobState]()
		{
			FThreadPrefix* ThreadPrefix = ThreadPrefixes.Find(Id);
			if (ThreadPrefix == nullptr || ThreadPrefix->Handle == nullptr)
			{
				ThreadPrefixes.Remove(Id);
				SnapshotPoolStatsOnThread();
				return;
			}
			const FGpuStatus St = sslm_gpu_prefix_release(Ctx, ThreadPrefix->Handle);
			NoteLayer1Status(St);
			JobState->Status = St;
			JobState->Where = TEXT("sslm_gpu_prefix_release");
			if (St == FGpuStatus::SSLM_OK)
			{
				ThreadPrefixes.Remove(Id);
			}
			else
			{
				UE_LOG(LogSuperSLM, Error, TEXT("sslm_gpu_prefix_release returned %s for GPU prefix %lld; the handle is kept and released at teardown."), *GpuStatusText(St), Id);
			}
			SnapshotPoolStatsOnThread();
		};
		Entry->ReleaseJob = Enqueue(Job);
		return true;
	}

	// The prefix an adopt, a nested build or a hint may use: Ready and not released.
	const FGpuPrefixEntry* FindReadyPrefix(int64 Id) const
	{
		const FGpuPrefixEntry* Entry = Id != 0 ? Prefixes.Find(Id) : nullptr;
		return Entry != nullptr && !Entry->bReleased && Entry->Phase == ESuperSLMPrefixPhase::Ready ? Entry : nullptr;
	}

	// ------------------------------------------------------------------ pooled restores (K7)

	// Plugin paging plan §3.5 (K7): the call-time header checks of a pooled restore's Layer-1 payload,
	// and, in dedicated mode, the mode crossing by magic. Every refusal the header alone decides is
	// made here, with no handle minted and nothing queued, its error beginning with the result's
	// name and a colon (rev 1.9 ruling 1). Fields are read at the fixed 'SLM6' offsets K0 fixed (§13
	// item 1). True when the restore may be queued; Out* carry the header's budget, origin and ready
	// word for the ledger's charge and the holder's shape.
	bool CheckRestorePayloadHeader(const uint8* L1, int64 L1Size, int64 WrapperContextUsed, int64& OutBudget, int64& OutOrigin, bool& OutReady)
	{
		OutBudget = 0;
		OutOrigin = 0;
		OutReady = false;
		auto HasMagic = [L1, L1Size](const char* FourCC)
		{
			return L1Size >= 4 && FMemory::Memcmp(L1, FourCC, 4) == 0;
		};
		auto ReadLE = [L1](int32 Offset, int32 Bytes)
		{
			uint64 V = 0;
			for (int32 B = 0; B < Bytes; ++B)
			{
				V |= static_cast<uint64>(L1[Offset + B]) << (8 * B);
			}
			return V;
		};
		const bool bSlm6 = HasMagic("SLM6");
		if (!IsPooled())
		{
			if (bSlm6)
			{
				LastLifecycleRequestError = TEXT("KvModeMismatch: the payload is 'SLM6', saved by a pooled GPU subsystem (SequenceBudget > 0); this subsystem is dedicated (SequenceBudget 0) and restores 'SLM5' only");
				return false;
			}
			return true;
		}
		if (HasMagic("SLM5"))
		{
			LastLifecycleRequestError = TEXT("KvModeMismatch: the payload is 'SLM5', saved by a dedicated GPU subsystem (SequenceBudget 0); this subsystem is pooled and restores 'SLM6' only");
			return false;
		}
		if (!bSlm6)
		{
			LastLifecycleRequestError = TEXT("Malformed: the payload's magic is not one of the GPU's ('SLM5', 'SLM6')");
			return false;
		}
		constexpr int32 Slm6HeaderEnd = 148;
		if (L1Size < Slm6HeaderEnd)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the 'SLM6' payload is %lld bytes, shorter than its %d-byte header"), L1Size, Slm6HeaderEnd);
			return false;
		}
		const int64 ContextLength = static_cast<int64>(ReadLE(32, 8));
		const uint32 ReadyWord = static_cast<uint32>(ReadLE(128, 4));
		const uint32 KvMode = static_cast<uint32>(ReadLE(132, 4));
		const int64 Budget = static_cast<int64>(static_cast<int32>(static_cast<uint32>(ReadLE(136, 4))));
		const int64 Origin = static_cast<int64>(ReadLE(140, 8));
		const int64 Ce = Ledger.GetConfig().EngineContextCap > 0 ? Ledger.GetConfig().EngineContextCap : static_cast<int64>(Config.ContextCap);
		const int64 Cp = Config.ContextCap;
		constexpr uint32 KvModeBudget = 1;
		if (KvMode != KvModeBudget)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the 'SLM6' payload's kv_mode is %u, not budget (%u)"), KvMode, KvModeBudget);
			return false;
		}
		if (Budget < 1 || Budget > Ce)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the payload's budget (%lld) is outside [1, %lld], the model's cap"), Budget, Ce);
			return false;
		}
		if (Origin < 0 || Origin > Ce)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the payload's origin (%lld) is outside [0, %lld], the model's cap"), Origin, Ce);
			return false;
		}
		if (ContextLength < 0 || ContextLength > Ce)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the payload's context_length (%lld) is outside [0, %lld], the model's cap"), ContextLength, Ce);
			return false;
		}
		if (WrapperContextUsed != ContextLength)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("Malformed: the wrapper's ContextUsed (%lld) differs from the payload's context_length (%lld)"), WrapperContextUsed, ContextLength);
			return false;
		}
		const TCHAR* Field = Budget > Cp ? TEXT("budget") : (Origin > Cp ? TEXT("origin") : (ContextLength > Cp ? TEXT("context_length") : nullptr));
		if (Field != nullptr)
		{
			const int64 Figure = Budget > Cp ? Budget : (Origin > Cp ? Origin : ContextLength);
			LastLifecycleRequestError = FString::Printf(TEXT("ContextCapMismatch: the payload's %s (%lld) is above this subsystem's ContextCap (%lld); it was saved under a larger ContextCap and restores after ContextCap is raised"),
				Field, Figure, Cp);
			return false;
		}
		OutBudget = Budget;
		OutOrigin = Origin;
		OutReady = ReadyWord != 0; // the engine reads any non-zero word as ready (round-2 note 10)
		return true;
	}

	// K6 review C1, fix (b): a recycle QueueRecycle() recorded on SlotIndex whose reset the page
	// ledger has not yet admitted (PollRecycles() admits it). A ledger op of the slot's new holder,
	// and a restore's choice of slot, wait behind it, as they wait behind an open ledger job.
	bool HasUnadmittedRecycle(int32 SlotIndex) const
	{
		for (const FPendingRecycle& Recycle : PendingRecycles)
		{
			if (Recycle.SlotIndex == SlotIndex && !Recycle.bLedgerAdmitted)
			{
				return true;
			}
		}
		return false;
	}

	// A slot a pooled restore may take now: free, not withheld, with no open ledger job.
	int32 FindPooledRestoreSlot() const
	{
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			if (!Slots[I].bVended && !Slots[I].bWithheld && !Ledger.SlotHasJob(I) && !HasUnadmittedRecycle(I))
			{
				return I;
			}
		}
		return INDEX_NONE;
	}

	// True while a slot will come free without any caller acting: a restore's reservation (its job may
	// fail) or a recycle not yet applied.
	bool AnySlotWillFree() const
	{
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			if (Slots[I].bRestoreReserved || (!Slots[I].bVended && !Slots[I].bWithheld && (Ledger.SlotHasJob(I) || HasUnadmittedRecycle(I))))
			{
				return true;
			}
		}
		return false;
	}

	// Plugin paging plan §3.5 "Fit and wait" (K7): every queued pooled restore not yet arrived, in
	// arrival order. Each takes a free slot and the ledger's admission there: admitted (its job is
	// posted), waiting on H (the slot is held for it), or refused KvPagesExhausted on arrival. A
	// restore with no slot free waits, and every restore behind it waits too, while a slot will come
	// free; with none it is refused PoolExhausted, as in dedicated mode. Game thread.
	void ArrivePooledRestores()
	{
		for (int32 Index = 0; Index < RestoreOps.Num(); ++Index)
		{
			FLifecycleOpEntry& Entry = RestoreOps[Index];
			if (Entry.bSubmitted || Entry.Result != ESuperSLMRestoreResult::Pending)
			{
				continue;
			}
			if (!ArrivePooledRestore(Entry))
			{
				break;
			}
		}
	}

	// One arrival. False when the restore must wait for a slot (it stays queued, not arrived).
	bool ArrivePooledRestore(FLifecycleOpEntry& Entry)
	{
		const SslmGpuAdapterHandle* RestoreAdapterHandle = ResolveRestoreAdapterHandle(Entry.RestoreAdapterId);
		if (Entry.RestoreAdapterId != 0 && RestoreAdapterHandle == nullptr)
		{
			Entry.bSubmitted = true;
			ReleaseRestorePin(Entry);
			ResolveEntry(Entry, ESuperSLMRestoreResult::AdapterUnavailable);
			LastLifecycleRequestError = FString::Printf(TEXT("the blob was saved with adapter %s bound, and that adapter is no longer mapped on this GPU subsystem"),
				*SuperSLMRuntime::HashToHex(Entry.RestoreContents.AdapterHash));
			return true;
		}
		const int32 SlotIndex = FindPooledRestoreSlot();
		if (SlotIndex == INDEX_NONE)
		{
			if (AnySlotWillFree())
			{
				return false;
			}
			Entry.bSubmitted = true;
			ReleaseRestorePin(Entry);
			ResolveEntry(Entry, ESuperSLMRestoreResult::PoolExhausted);
			LastLifecycleRequestError = TEXT("every pooled GPU sequence is vended; a restore needs a free one");
			return true;
		}
		SuperSLMPaging::FRestoreRequest Request;
		Request.Slot = SlotIndex;
		Request.Payload = SuperSLMPaging::ERestorePayload::Budget;
		Request.Budget = Entry.RestoreBudget;
		Request.Origin = Entry.RestoreOrigin;
		if (const FGpuPrefixEntry* Hint = FindReadyPrefix(Entry.RestoreHintPrefixId))
		{
			Request.Hint = SuperSLMPaging::EHintKind::Prefix;
			Request.HintNode = Hint->Node;
		}
		else if (Entry.RestoreHintPrefixId != 0)
		{
			// A hint is never a requirement (gpu_1p0.h): one that names no Ready prefix of this
			// subsystem is dropped and the restore runs unhinted.
			UE_LOG(LogSuperSLM, Display, TEXT("GPU restore: the hint (prefix %lld) is not a Ready prefix of this subsystem; the restore runs unhinted."), Entry.RestoreHintPrefixId);
			Entry.RestoreHintPrefixId = 0;
		}
		const SuperSLMPaging::FAdmission Admission = Ledger.AdmitRestore(Request);
		Entry.bSubmitted = true;
		switch (Admission.Outcome)
		{
			case SuperSLMPaging::EPageAdmit::Admitted:
			case SuperSLMPaging::EPageAdmit::Waiting:
				Slots[SlotIndex].bVended = true; // reserved: see AdmitRestoreFront()
				Slots[SlotIndex].bRestoreReserved = true;
				Entry.RestoreSlotIndex = SlotIndex;
				Entry.LedgerTicket = Admission.Ticket;
				if (Admission.Outcome == SuperSLMPaging::EPageAdmit::Waiting)
				{
					Entry.bRestoreWaiting = true; // admitted at the apply that makes it fit (DrainRestoreResolutions())
				}
				else
				{
					StartPooledRestoreJob(Entry);
				}
				return true;
			case SuperSLMPaging::EPageAdmit::KvPagesExhausted:
				ReleaseRestorePin(Entry);
				LastLifecycleRequestError = FString::Printf(TEXT("KvPagesExhausted: the restore's charge does not fit the restore headroom (%u pages free) and the restores ahead of it do not guarantee it; re-issue it after an apply, or raise RestoreHeadroomBlocks"),
					Ledger.RegionFree(SuperSLMPaging::ERegion::H));
				ResolveEntry(Entry, ESuperSLMRestoreResult::KvPagesExhausted);
				return true;
			case SuperSLMPaging::EPageAdmit::ContextCapMismatch:
				ReleaseRestorePin(Entry);
				LastLifecycleRequestError = TEXT("ContextCapMismatch: the payload's budget or origin is above this subsystem's ContextCap");
				ResolveEntry(Entry, ESuperSLMRestoreResult::ContextCapMismatch);
				return true;
			case SuperSLMPaging::EPageAdmit::InvalidRequest:
				ReleaseRestorePin(Entry);
				LastLifecycleRequestError = TEXT("Malformed: the page ledger refused the payload's budget or origin");
				ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed);
				return true;
			case SuperSLMPaging::EPageAdmit::NotConfigured:
			case SuperSLMPaging::EPageAdmit::PrefixLimit:
			case SuperSLMPaging::EPageAdmit::ParentNotUsable:
			case SuperSLMPaging::EPageAdmit::KvModeMismatch:
			case SuperSLMPaging::EPageAdmit::PrefixPinned:
				break;
		}
		ReleaseRestorePin(Entry);
		const FString Reason = FString::Printf(TEXT("the page ledger did not admit the restore (outcome %d)"), static_cast<int32>(Admission.Outcome));
		NoteLedgerRefusal(SlotIndex, Reason);
		LastLifecycleRequestError = Reason;
		ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
		return true;
	}

	// The admitted pooled restore's job: sslm_gpu_seq_restore_into_pool (restore-first: the slot's
	// warm holder is released only once the new one is restored, bound and checked), with the hint's
	// handle. The thread slot takes the payload's shape: its limit from the header's budget and
	// origin, its armed flag from the header's ready word (§3.4 "The armed flag"). Game thread.
	void StartPooledRestoreJob(FLifecycleOpEntry& Entry)
	{
		const int32 SlotIndex = Entry.RestoreSlotIndex;
		Entry.HoldId = AttachPagedHold(EGpuPagedJobKind::Restore);
		JobsPostedByKind[static_cast<int32>(EGpuPagedJobKind::Restore)] += 1;
		Entry.Async = MakeShared<FLifecycleAsyncState>();
		TSharedPtr<FLifecycleAsyncState> Async = Entry.Async;
		BumpGeneration(SlotIndex);
		const uint32 Gen = Slots[SlotIndex].Gen;
		const int32 SchemaIndex = Entry.RestoreSchemaIndex;
		const SslmGpuAdapterHandle* RestoreAdapterHandle = ResolveRestoreAdapterHandle(Entry.RestoreAdapterId);
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Blob = MoveTemp(Entry.RestoreBlob);
		const int64 Layer1Offset = Entry.RestoreLayer1Offset;
		const int64 Layer1Size = Entry.RestoreLayer1Size;
		const int32 RestoreLastToken = Entry.RestoreLastToken;
		const bool bRestoreStopped = Entry.bRestoreStopped;
		const bool bRestorePrimed = Entry.bRestorePrimed;
		const int64 RestoreContextUsed = Entry.RestoreContents.ContextUsed;
		const int32 RestoreMaxNewTokens = Entry.RestoreContents.MaxNewTokensRemaining;
		const TArray<int32> RestoreStopTokenIds = Entry.RestoreContents.StopTokenIds;
		const int64 HintId = Entry.RestoreHintPrefixId;
		const bool bReady = Entry.bRestoreReady;
		SuperSLMPaging::FHolderShape Shape;
		Shape.Mode = SuperSLMPaging::EHolderMode::Budget;
		Shape.Budget = Entry.RestoreBudget;
		Shape.Origin = Entry.RestoreOrigin;
		const int64 LimitAfter = LedgerHolderLimit(Shape);
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Custom = [this, SlotIndex, Gen, SchemaIndex, RestoreAdapterHandle, Blob = MoveTemp(Blob), Layer1Offset, Layer1Size, Async, RestoreLastToken, bRestoreStopped,
			bRestorePrimed, RestoreContextUsed, RestoreMaxNewTokens, RestoreStopTokenIds, HintId, bReady, LimitAfter]()
		{
			SslmGpuSequenceHandle* Fresh = nullptr;
			uint32_t SharedPages = 0;
			const FThreadPrefix* HintPrefix = HintId != 0 ? ThreadPrefixes.Find(HintId) : nullptr;
#if WITH_DEV_AUTOMATION_TESTS
			const FGpuStatus Forced = TakeTestStatus(&TestNextRestoreStatus);
#else
			const FGpuStatus Forced = FGpuStatus::SSLM_OK;
#endif
			Async->RestoreStatus = Forced != FGpuStatus::SSLM_OK ? Forced
				: sslm_gpu_seq_restore_into_pool(Ctx, Model, Pool, Blob->GetData() + Layer1Offset, static_cast<size_t>(Layer1Size),
					HintPrefix != nullptr ? HintPrefix->Handle : nullptr, &Fresh, &SharedPages);
			NoteLayer1Status(Async->RestoreStatus);
			if (Async->RestoreStatus != FGpuStatus::SSLM_OK || Fresh == nullptr)
			{
				Async->RestoreWhere = TEXT("sslm_gpu_seq_restore_into_pool");
				SnapshotPoolStatsOnThread();
				return;
			}
			// Plan §2.5 row 5, as the dedicated restore: the restored binding must agree with the wrapper.
			int32_t SchemaBound = 0;
			const FGpuStatus BoundSt = SslmGpuSeqSchemaBoundForG5Bridge(Ctx, Fresh, &SchemaBound);
			NoteLayer1Status(BoundSt);
			if (BoundSt != FGpuStatus::SSLM_OK || (SchemaBound != 0) != (SchemaIndex >= 0))
			{
				Async->bSchemaCrossCheckFailed = true;
				Async->RestoreStatus = BoundSt != FGpuStatus::SSLM_OK ? BoundSt : FGpuStatus::SSLM_SEQUENCE_REJECTED;
				Async->RestoreWhere = BoundSt != FGpuStatus::SSLM_OK
					? FString(TEXT("SslmGpuSeqSchemaBoundForG5Bridge (restore cross-check)"))
					: FString::Printf(TEXT("the restore cross-check: the wrapper %s a schema, but the restored sequence %s one"),
						SchemaIndex >= 0 ? TEXT("names") : TEXT("names no"), SchemaBound != 0 ? TEXT("has") : TEXT("has no"));
				NoteLayer1Status(sslm_gpu_seq_release(Ctx, Fresh));
				SnapshotPoolStatsOnThread();
				return;
			}
			if (RestoreAdapterHandle != nullptr)
			{
				Async->RestoreStatus = sslm_gpu_seq_bind_adapter(Ctx, Fresh, RestoreAdapterHandle);
				NoteLayer1Status(Async->RestoreStatus);
				if (Async->RestoreStatus != FGpuStatus::SSLM_OK)
				{
					Async->RestoreWhere = TEXT("sslm_gpu_seq_bind_adapter (re-bind after restore)");
					NoteLayer1Status(sslm_gpu_seq_release(Ctx, Fresh));
					SnapshotPoolStatsOnThread();
					return;
				}
				Async->bAdapterBound = true;
			}
			FThreadSlot& TS = ThreadSlots[SlotIndex];
			if (TS.Handle != nullptr)
			{
				NoteLayer1Status(sslm_gpu_seq_release(Ctx, TS.Handle)); // restore-first: the warm holder goes last
			}
			TS = FThreadSlot();
			TS.Handle = Fresh;
			TS.Cap = Config.ContextCap;
			TS.KvBytes = 0; // a pooled holder's K/V is the pool's
			TS.Gen = Gen;
			TS.bPrimed = bRestorePrimed;
			TS.SchemaIndex = SchemaIndex;
			TS.BoundAdapter = RestoreAdapterHandle;
			TS.LastToken = RestoreLastToken;
			TS.bStopped = bRestoreStopped;
			TS.ContextUsed = RestoreContextUsed;
			TS.MaxNewTokens = RestoreMaxNewTokens;
			TS.StopTokenIds = RestoreStopTokenIds;
			TS.Limit = LimitAfter;
			TS.bArmed = bReady;
			Async->SharedPages = SharedPages;
			Async->WalkState = SslmGpuSeqWalkStateForG5Bridge(Fresh);
			Async->FreshHandle = Fresh;
			SnapshotPoolStatsOnThread();
		};
		Async->bPrimed = bRestorePrimed;
		Async->Job = Enqueue(Job);
	}

	// Each tick (pooled): every pooled restore whose job is done and whose apply HoldApply does not
	// hold is finalized, in arrival order; then the waiters the ledger admitted at those applies (or
	// at any other H refund) post their jobs; then the resolved entries leave the queue (their results
	// live in HandleResults). Game thread.
	void PollPooledRestores()
	{
		for (FLifecycleOpEntry& Entry : RestoreOps)
		{
			if (!Entry.bSubmitted || Entry.bRestoreWaiting || Entry.Result != ESuperSLMRestoreResult::Pending || !Entry.Async.IsValid() || !Entry.Async->Job.IsValid()
				|| !Entry.Async->Job->bDone.load(std::memory_order_acquire))
			{
				continue;
			}
			if (PagedHoldBlocks(Entry.HoldId))
			{
				continue;
			}
			FinalizeRestoreOp(Entry);
		}
		DrainRestoreResolutions();
		RestoreOps.RemoveAll([](const FLifecycleOpEntry& Entry) { return Entry.Result != ESuperSLMRestoreResult::Pending; });
		RestoreOpsFront = 0;
	}

	// The ledger's wait resolutions (§3.5 "Fit and wait"): an admitted waiter posts its job now; a
	// waiter the epoch's end resolved is refused NotConfigured, its slot reservation released (no job
	// ran on it). Game thread.
	void DrainRestoreResolutions()
	{
		if (!IsPooled())
		{
			return;
		}
		for (const SuperSLMPaging::FWaitResolution& Resolution : Ledger.DrainResolutions())
		{
			FLifecycleOpEntry* Waiter = RestoreOps.FindByPredicate([&Resolution](const FLifecycleOpEntry& Entry)
			{
				return Entry.bRestoreWaiting && Entry.LedgerTicket == Resolution.Ticket && Entry.Result == ESuperSLMRestoreResult::Pending;
			});
			if (Waiter == nullptr)
			{
				continue;
			}
			Waiter->bRestoreWaiting = false;
			if (Resolution.Outcome == SuperSLMPaging::EPageAdmit::Admitted)
			{
				StartPooledRestoreJob(*Waiter);
				continue;
			}
			Waiter->LedgerTicket = SuperSLMPaging::InvalidTicket;
			ReleaseRestorePin(*Waiter);
			if (Slots.IsValidIndex(Waiter->RestoreSlotIndex))
			{
				Slots[Waiter->RestoreSlotIndex].bVended = false;
				Slots[Waiter->RestoreSlotIndex].bRestoreReserved = false;
			}
			LastLifecycleRequestError = TEXT("NotConfigured: the restore was waiting on the restore headroom when the pooled epoch ended");
			ResolveEntry(*Waiter, ESuperSLMRestoreResult::NotConfigured);
		}
	}

	// D-SLM7758: a held schema bind reaches Layer 1 here, on the one FIFO, after every op ahead of
	// it in this sequence's log has resolved (a caller's reset included), in call order.
	void AdmitSchemaBindOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		Entry.bSubmitted = true;
		Entry.Async = MakeShared<FLifecycleAsyncState>();
		TSharedPtr<FLifecycleAsyncState> Async = Entry.Async;
		const int32 Index = Entry.BindSchema.IsNone() ? -1 : Entry.BindSchema.Index;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		FSuperSLMGpuSubsystemState* St8 = this;
		Job->Custom = [St8, SlotIndex, Index, Async]()
		{
			FThreadSlot& TS = St8->ThreadSlots[SlotIndex];
			Async->BindStatus = SslmGpuSeqSetSchemaForG5Bridge(St8->Ctx, TS.Handle, Index);
			St8->NoteLayer1Status(Async->BindStatus); // K6 review S2
			if (Async->BindStatus == FGpuStatus::SSLM_OK)
			{
				TS.SchemaIndex = Index;
			}
			// Ruling 2026-09-26: the walk after the bind (or as it stood, on a refusal), stored by
			// FinalizeSchemaBindOp().
			Async->WalkState = TS.Handle != nullptr ? SslmGpuSeqWalkStateForG5Bridge(TS.Handle) : kSslmGpuDfaWalkStateUnused;
		};
		Async->Job = Enqueue(Job);
	}

	// D-SLM7758: the held bind's completion. Applied: Schema/SchemaName now report it. Refused: the
	// holder's next generation is faulted by name at its admission (the deferred-refusal field).
	void FinalizeSchemaBindOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		if (JobWasSkipped(Entry.Async->Job))
		{
			// Round 4 (finding 1): the bind never reached Layer 1; no schema and no walk are stored.
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			LastLifecycleRequestError = LostBeforeRanText();
			return;
		}
		const FGpuStatus St = Entry.Async->BindStatus;
		if (S.Id != Entry.Owner)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed); // rule 2: no caller-owned write
			return;
		}
		S.WalkState = Entry.Async->WalkState; // ruling 2026-09-26: the game-side walk record
		if (St == FGpuStatus::SSLM_OK)
		{
			S.Schema = Entry.BindSchema;
			S.SchemaName = Entry.BindSchemaName;
			ResolveEntry(Entry, ESuperSLMRestoreResult::Success);
			return;
		}
		const FString Message = FString::Printf(TEXT("the held schema bind ('%s') was refused by Layer 1 (SslmGpuSeqSetSchemaForG5Bridge returned %s)"),
			*Entry.BindSchemaName, *GpuStatusText(St));
		S.DeferredRefusal = S.bHasDeferredRefusal ? S.DeferredRefusal + TEXT("; ") + Message : Message;
		S.bHasDeferredRefusal = true;
		LastLifecycleRequestError = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU sequence %lld: %s; its next generation is faulted by name."), S.Id, *Message);
		ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed);
	}

	void AdmitBeginGenerationOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		Entry.bSubmitted = true;
		// D-SLM7946: a generation starts only on an Idle sequence. The call refused every request
		// whose turn would not find the sequence Idle, assuming the resets queued ahead of it
		// succeed, so what reaches here on any other phase is a generation behind a reset Layer 1
		// refused, which left the sequence Faulted (PollSlotFront()); the other phases are
		// named for completeness. The generation is refused by name, ResetRequired, and nothing on
		// the sequence changes: its request, generated tokens, phase and any held refusal stay as
		// they were, so AdmitSlotFront() marks no active generation. Requiring a reset after every
		// fault is the plugin's own rule: Layer 1 requires one only after an allocation failure
		// (gpu_1p0.h:233-237), and lets a SchemaDeadEnd be retried (gpu_1p0.h:564-579). A restore
		// never clears this sequence: it restores into a new sequence.
		if (S.Phase != ESuperSLMSequencePhase::Idle)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("one generation at a time: the reset queued ahead of this generation did not leave the sequence Idle (it is %s); "
				"call RequestResetSequence() again, or return the sequence and restore a save, which gives a new sequence"), SequencePhaseText(S.Phase));
			ResolveEntry(Entry, ESuperSLMRestoreResult::ResetRequired);
			return;
		}
		// D-SLM7758: a held schema bind Layer 1 refused faults this generation by name.
		if (S.bHasDeferredRefusal)
		{
			const FString Refusal = S.DeferredRefusal;
			S.bHasDeferredRefusal = false;
			S.DeferredRefusal.Reset();
			S.Request = Entry.Request;
			S.Generated.Reset();
			FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection, Refusal);
			ResolveEntry(Entry, ESuperSLMRestoreResult::Success); // admitted; the generation itself faulted
			return;
		}
		// Plugin paging plan §3.4 "An empty prompt after adopt", rev 1.9 ruling 6 (K7): in pooled mode
		// the call accepted an empty prompt; here, at its queue head, it runs only on an armed
		// sequence (an adopter of a prefix that fed a token, or a restore of a ready payload), whose
		// first step is the finish of its already-embedded arc. An unarmed sequence faults by name,
		// before any Layer-1 call.
		const bool bEmptyPrompt = Entry.Request.PromptTokens.Num() == 0;
		if (IsPooled() && bEmptyPrompt && !S.bArmedMirror)
		{
			S.Request = Entry.Request;
			S.Generated.Reset();
			FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection,
				TEXT("the prompt is empty; a GPU sequence is primed by prefilling at least one prompt token (this sequence is not armed: no adopted prefix or restored payload left it ready)"));
			ResolveEntry(Entry, ESuperSLMRestoreResult::Success); // admitted; the generation itself faulted
			return;
		}
		// Plugin paging plan §3.4 "Activation pre-check", rev 1.9 ruling 6 (K6, a plain holder; K7
		// adds the adopter, whose mirror counts the adopted prefix's positions): in pooled mode a prompt that would run past the holder's limit is
		// refused here, at its queue head, not at the call. The handle is valid and resolves
		// Success (admitted); the sequence faults by name before any token, as the CPU's activation
		// does. KvBudgetExceeded below ContextCap; at a limit equal to ContextCap the cap outcome.
		if (IsPooled())
		{
			const int64 Limit = Ledger.SlotLimit(SlotIndex);
			const int64 PromptTokens = Entry.Request.PromptTokens.Num();
			const SuperSLMPaging::ELimitVerdict Verdict = SuperSLMPaging::CheckActivation(S.ContextUsedMirror, PromptTokens, Limit, Config.ContextCap);
			if (Verdict != SuperSLMPaging::ELimitVerdict::Allowed)
			{
				const bool bBudget = Verdict == SuperSLMPaging::ELimitVerdict::KvBudgetExceeded;
				const FString Refusal = FString::Printf(TEXT("the prompt (%lld tokens, with %lld positions already used) would run past the sequence's %s (%lld); refused at activation, before any token"),
					PromptTokens, S.ContextUsedMirror, bBudget ? TEXT("own generation limit, min(origin + SequenceBudget, ContextCap)") : TEXT("ContextCap"), Limit);
				S.Request = Entry.Request;
				S.Generated.Reset();
				FaultSlot(S, ESuperSLMDecodeOutcome::Generating,
					bBudget ? ESuperSLMGpuFaultReason::KvBudgetExceeded : ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection, Refusal);
				ResolveEntry(Entry, ESuperSLMRestoreResult::Success); // admitted; the generation itself faulted
				return;
			}
		}
		// The sequence is Idle here: the call refuses every request whose turn would not find it
		// Idle (EnqueueSlotOp()), and the check above refuses the rest. The request-shape checks
		// already ran at the call (RequestBeginGeneration()).
		S.Request = Entry.Request;
		S.Generated.Reset();
		S.Generated.Reserve(Entry.Request.MaxNewTokens);
		S.Phase = ESuperSLMSequencePhase::Prefilling;
		// K7 review S1: the limit the check above read is the one the generation's steps check.
		S.PlanBeginLimit = IsPooled() ? Ledger.SlotLimit(SlotIndex) : 0;
		S.PromptPlanned = 0;
		S.BeginWallSeconds = FPlatformTime::Seconds();
		S.FirstTokenMs = -1.0;
		if (bEmptyPrompt)
		{
			// The armed sequence's first step is finish-only, and carries the generation's limits.
			S.bPlanPrimed = true;
			S.bPlanBeginLimits = true;
		}
		// ActiveGenerationOpLogIndex is set by the caller (AdmitSlotFront), which knows this
		// entry's own OpLog index; this function only knows the entry by reference.
		ResolveEntry(Entry, ESuperSLMRestoreResult::Success); // "delivered" means admitted (header doc)
	}

	void AdmitSaveOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Save", SuperSLMGpuChannel);
		FGameSlot& S = Slots[SlotIndex];
		Entry.bSubmitted = true;
		if (S.Phase == ESuperSLMSequencePhase::Faulted)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::SaveRefused);
			LastLifecycleRequestError = TEXT("a faulted sequence cannot be saved; reset it first");
			return;
		}
		// Plan §2.5 row 6 (D-SLM7666): Layer 1's 'SLM5' blob carries the schema binding, its walk
		// and ready_for_logits, so a schema-bound sequence saves at any token boundary, advanced
		// or not; the refusal D-SLM7334 held until this re-pin is gone.
		//
		// The plugin saves only at a token boundary (plan §5): a composed token in flight is
		// carried to full depth now, fire-and-forget on the same submission-thread FIFO, so the
		// combined walk-read+save job below (enqueued after it) runs once it has completed.
		if (S.PlanLayerPos > 0)
		{
			TSharedPtr<FJob> ForceJob = MakeShared<FJob>();
			FAction A;
			A.Kind = EActionKind::ComposedSlice;
			A.Slot = SlotIndex;
			A.Gen = S.Gen;
			A.Layers = NumHiddenLayers - S.PlanLayerPos;
			A.bPrompt = S.bPlanInPrompt;
			A.bFinish = !S.bPlanInPrompt;
			const int32 Index = ForceJob->Actions.Add(MoveTemp(A));
			S.PlanLayerPos = 0;
			if (S.bPlanInPrompt)
			{
				S.bPlanInPrompt = false;
				if (S.PlanPromptIndex == S.Request.PromptTokens.Num() - 1)
				{
					PromptCompleted(S, ForceJob->Actions[Index], ForceJob, Index, S.TokenRequestTick, S.TokenRequestWall, S.TokenRequestSim);
				}
			}
			else
			{
				AddTokenEvent(S, ForceJob, Index, S.TokenRequestTick, S.TokenRequestWall, S.TokenRequestSim);
			}
			Enqueue(ForceJob);
		}

		Entry.Async = MakeShared<FLifecycleAsyncState>();
		TSharedPtr<FLifecycleAsyncState> Async = Entry.Async;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		FSuperSLMGpuSubsystemState* St8 = this;
		Job->Custom = [St8, SlotIndex, Async]()
		{
			// Runs on the submission thread, after the force-finish job above (strict FIFO order
			// on the same Queue) -- Layer 1's state is final by the time this runs.
			FThreadSlot& TS = St8->ThreadSlots[SlotIndex];
			size_t Size = 0;
			St8->NoteLayer1Status(sslm_gpu_seq_save(St8->Ctx, TS.Handle, nullptr, &Size)); // K6 review S2: the size query
			if (Size == 0)
			{
				Async->SaveStatus = FGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
				return;
			}
			Async->Layer1Bytes.SetNumUninitialized(static_cast<int64>(Size));
			Async->SaveStatus = sslm_gpu_seq_save(St8->Ctx, TS.Handle, Async->Layer1Bytes.GetData(), &Size);
			St8->NoteLayer1Status(Async->SaveStatus);
			Async->Layer1Bytes.SetNum(static_cast<int64>(Size));
			Async->bPrimed = TS.bPrimed;
			Async->bStopped = TS.bStopped;
			Async->LastToken = TS.LastToken;
			Async->ContextUsed = TS.ContextUsed;
			Async->SavedAdapter = TS.BoundAdapter;
			Async->SavedSchemaIndex = TS.SchemaIndex;
			Async->ProducedAtSave = TS.Produced;
			Async->BindTagAtSave = TS.AppliedBindTag;
		};
		Async->Job = Enqueue(Job);
	}

	// Runs on the game thread once AdmitSaveOp()'s async job has completed (FLifecycleAsyncState::
	// Job->bDone) -- assembles the wrapper blob from GAME-THREAD fields (S.Path/Request/Pending/
	// PromptPlanned/ActiveAdapterId/SchemaName/TokensIssued/Generated), unchanged from the prior
	// synchronous SaveSequence()'s own tail.
	void FinalizeSaveOp(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		FLifecycleAsyncState& Async = *Entry.Async;
		if (JobWasSkipped(Async.Job))
		{
			// Round 4 (finding 1): the save never ran; no blob is assembled or stored.
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			LastLifecycleRequestError = LostBeforeRanText();
			return;
		}
		if (S.Id != Entry.Owner)
		{
			// Rule 2: the wrapper is built from caller-owned fields, so only for the save's owner.
			// A return with ops left defers the free until they drain, so this is not expected.
			ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed);
			LastLifecycleRequestError = TEXT("the sequence that queued this save no longer holds its slot");
			return;
		}
		if (Async.SaveStatus != FGpuStatus::SSLM_OK)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::SaveRefused);
			LastLifecycleRequestError = FString::Printf(TEXT("sslm_gpu_seq_save returned %s"), *GpuStatusText(Async.SaveStatus));
			return;
		}

		struct FCarried { uint8 Kind; uint8 Fault; int32 Token; int32 Delay; };
		TArray<FCarried> Carried;
		for (const FPendingEvent& E : S.Pending)
		{
			const FActionResult& R = E.Job->Results[E.ResultIndex];
			if (R.Kind == EResultKind::Skipped || R.Kind == EResultKind::Progress)
			{
				continue;
			}
			FCarried C;
			C.Kind = static_cast<uint8>(R.Kind);
			C.Fault = static_cast<uint8>(R.Fault);
			C.Token = R.Token;
			C.Delay = static_cast<int32>(FMath::Max<int64>(1, E.ApplyTick - TickIndex));
			Carried.Add(C);
		}

		SuperSLMSaveBlob::FContents Contents;
		Contents.Backend = ESuperSLMBackend::GPU;
		Contents.Phase = S.Phase;
		Contents.SpanKind = S.Request.SpanKind;
		Contents.bReadyForLogits = Async.bPrimed;
		Contents.Layer1Tag = SuperSLMSaveBlob::CompiledLayer1Tag();
		Contents.Layer1Commit = SuperSLMSaveBlob::CompiledLayer1Commit();
		FMemory::Memcpy(Contents.ArtifactHash, ArtifactHash, 32);
		// Plan §2.5 row 24 (T-2983 N1, D-SLM7714): the tokens the thread has produced but the game
		// has not yet applied are written into this blob as carried events (above), and the
		// restore re-queues them, so the remaining budget counts only the tokens the game has
		// applied -- taken at this same finalize, so the budget and the carried set describe one
		// instant. Subtracting the thread's own count as well charged those tokens twice, and a
		// restored sequence stopped short by the carried count.
		Contents.MaxNewTokensRemaining = FMath::Max(0, S.Request.MaxNewTokens - S.Generated.Num());
		Async.ObservedAtSave = S.Generated.Num(); // diagnostic only (R-S2d)
		Contents.ContextUsed = Async.ContextUsed;
		// The thread slot's own bindings at the save's position (fold 9, D-SLM7719).
		if (Async.SavedAdapter != nullptr)
		{
			for (const TPair<int64, FAdapterEntry>& Pair : Adapters)
			{
				if (Pair.Value.Handle == Async.SavedAdapter)
				{
					Contents.bHasAdapter = true;
					FMemory::Memcpy(Contents.AdapterHash, Pair.Value.ArtifactHash, 32);
					break;
				}
			}
			if (!Contents.bHasAdapter)
			{
				ResolveEntry(Entry, ESuperSLMRestoreResult::SaveRefused);
				LastLifecycleRequestError = TEXT("the sequence's bound adapter is no longer mapped on this GPU subsystem, so the save cannot record it");
				return;
			}
		}
		for (int32 I = S.PromptPlanned; I < S.Request.PromptTokens.Num(); ++I)
		{
			Contents.PromptRemaining.Add(S.Request.PromptTokens[I]);
		}
		Contents.StopTokenIds = S.Request.StopTokenIds;
		if (Async.SavedSchemaIndex >= 0)
		{
			const superslm::SchemaEntry* SchemaEntry = Schemas.ByIndex(static_cast<size_t>(Async.SavedSchemaIndex));
			if (SchemaEntry == nullptr)
			{
				ResolveEntry(Entry, ESuperSLMRestoreResult::SaveRefused);
				LastLifecycleRequestError = FString::Printf(TEXT("the sequence's bound schema index %d is not in the configured artifact"), Async.SavedSchemaIndex);
				return;
			}
			Contents.SchemaName = UTF8_TO_TCHAR(std::string(SchemaEntry->name).c_str());
		}

		TArray<uint8> Ext;
		auto PutU8 = [&Ext](uint8 V) { Ext.Add(V); };
		auto PutU32 = [&Ext](uint32 V) { for (int32 B = 0; B < 4; ++B) { Ext.Add(static_cast<uint8>(V >> (8 * B))); } };
		auto PutU64 = [&Ext](uint64 V) { for (int32 B = 0; B < 8; ++B) { Ext.Add(static_cast<uint8>(V >> (8 * B))); } };
		PutU32(kWrapperExtMagic);
		PutU32(kWrapperExtVersion);
		PutU8(static_cast<uint8>(S.Path));
		PutU8(Async.bPrimed ? 1 : 0);
		PutU8(Async.bStopped ? 1 : 0);
		PutU8(0);
		PutU32(static_cast<uint32>(Async.LastToken));
		PutU32(static_cast<uint32>(S.TokensIssued - S.Generated.Num()));
		PutU32(static_cast<uint32>(Carried.Num()));
		for (const FCarried& C : Carried)
		{
			PutU8(C.Kind);
			PutU8(C.Fault);
			PutU8(0);
			PutU8(0);
			PutU32(static_cast<uint32>(C.Token));
			PutU32(static_cast<uint32>(C.Delay));
		}
		PutU64(static_cast<uint64>(Async.Layer1Bytes.Num()));

		// Row 19 (D-SLM7763): the header and the carried events above are this finalize's snapshot
		// (row 24); the payload copy and the Layer-1 bytes' release run on the submission thread, in
		// a job that makes no Layer-1 call. The save resolves when it finishes
		// (CompleteSaveAssembly()); until then ApplyDue() applies nothing to this slot, so the
		// snapshot still describes the slot at resolution.
		TSharedPtr<FLifecycleAsyncState> AsyncPtr = Entry.Async;
		TSharedPtr<FJob> Assemble = MakeShared<FJob>();
		Assemble->CallKind = TEXT("save assembly (no Layer-1 call)");
		Assemble->bTakesLayer1Lock = false;
		Assemble->Custom = [AsyncPtr, Contents, Ext = MoveTemp(Ext)]()
		{
			FLifecycleAsyncState& A = *AsyncPtr;
			TArray<uint8> OutBlob;
			const int64 PayloadOffset = SuperSLMSaveBlob::BeginWrite(OutBlob, Contents, Ext.Num() + A.Layer1Bytes.Num());
			FMemory::Memcpy(OutBlob.GetData() + PayloadOffset, Ext.GetData(), Ext.Num());
			FMemory::Memcpy(OutBlob.GetData() + PayloadOffset + Ext.Num(), A.Layer1Bytes.GetData(), A.Layer1Bytes.Num());
			SuperSLMSaveBlob::FinalizePayload(OutBlob, PayloadOffset, Ext.Num() + A.Layer1Bytes.Num());
			A.Layer1Bytes.Empty(); // the raw Layer-1 bytes are released here, on this thread
			A.FinishedBlob = MoveTemp(OutBlob);
			A.bAssembled = true;
		};
		S.bSaveAssembling = true;
		Async.AssembleJob = Enqueue(Assemble);
	}

	// Row 19 (D-SLM7763): the save resolves once its wrapper is assembled. The finished blob moves
	// (no copy) into the handle's result entry, where the first successful GetSaveResult() takes it.
	void CompleteSaveAssembly(int32 SlotIndex, FLifecycleOpEntry& Entry)
	{
		FGameSlot& S = Slots[SlotIndex];
		FLifecycleAsyncState& Async = *Entry.Async;
		S.bSaveAssembling = false;
		if (JobWasSkipped(Async.AssembleJob))
		{
			// Round 4: an assembly completed without running after a loss, named as the other
			// lifecycle finalizes name it.
			ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
			LastLifecycleRequestError = LostBeforeRanText();
			return;
		}
		if (!Async.bAssembled)
		{
			ResolveEntry(Entry, ESuperSLMRestoreResult::SaveRefused);
			LastLifecycleRequestError = TEXT("the GPU submission thread is unresponsive, so the save's blob was never assembled");
			return;
		}
		ResolveEntry(Entry, ESuperSLMRestoreResult::Success);
		FGpuHandleResult& Out = HandleResults.FindOrAdd(Entry.HandleId);
		RetainedResultBytes -= Out.SaveBlob.Num();
		Out.SaveBlob = MoveTemp(Async.FinishedBlob);
		RetainedResultBytes += Out.SaveBlob.Num();
		Out.bHasSaveCounts = true;
		Out.ObservedAtSave = Async.ObservedAtSave;
		Out.ProducedAtSave = Async.ProducedAtSave;
		Out.BindTagAtSave = Async.BindTagAtSave;
	}

	// Dispatches the front-of-queue entry of SlotIndex's own sequence to its kind-specific
	// admission. Called only when that entry exists and is not yet submitted.
	void AdmitSlotFront(int32 SlotIndex)
	{
		FGameSlot& S = Slots[SlotIndex];
		FLifecycleOpEntry& Entry = S.OpLog[S.OpFront];
		// Plugin paging plan §3.2 "One job per slot" (K6, K7): a ledger op (a reset, or a pooled adopt)
		// waits at the head of its queue while the slot still has a ledger job open -- the previous
		// holder's recycle, until PollRecycles() applies it. It is admitted at a later pass.
		// Plan §3.4 "Activation reads the applied shape" (rev 1.10; K7 review S1): in pooled mode a
		// BeginGeneration waits the same way, so its empty-prompt and activation checks read the shape
		// the slot's last applied job left, never a previous holder's or a job's still in flight.
		const bool bLedgerOp = Entry.Kind == ELifecycleOpKind::Reset || (Entry.Kind == ELifecycleOpKind::AdoptPrefix && Entry.AdoptPrefixId != 0);
		const bool bWaitsForSlotJob = bLedgerOp || Entry.Kind == ELifecycleOpKind::BeginGeneration;
		if (IsPooled() && bWaitsForSlotJob && (Ledger.SlotHasJob(SlotIndex) || HasUnadmittedRecycle(SlotIndex))) // K6 review C1: and a deferred recycle
		{
			return;
		}
		switch (Entry.Kind)
		{
			case ELifecycleOpKind::Reset: AdmitResetOp(SlotIndex, Entry); break;
			case ELifecycleOpKind::Save: AdmitSaveOp(SlotIndex, Entry); break;
			case ELifecycleOpKind::BeginGeneration:
				AdmitBeginGenerationOp(SlotIndex, Entry);
				if (S.Phase == ESuperSLMSequencePhase::Prefilling) // not when a deferred refusal faulted it
				{
					S.ActiveGenerationOpLogIndex = S.OpFront;
				}
				break;
			case ELifecycleOpKind::AdoptPrefix:
				if (IsPooled() && Entry.AdoptPrefixId != 0)
				{
					AdmitPooledAdoptOp(SlotIndex, Entry); // K7
				}
				else
				{
					AdmitAdoptPrefixOp(Entry);
				}
				break;
			case ELifecycleOpKind::SchemaBind: AdmitSchemaBindOp(SlotIndex, Entry); break;
			case ELifecycleOpKind::Restore: break; // subsystem-level; never reached here
		}
	}

	// Polls SlotIndex's own front-of-queue entry: advances OpFront once it is resolved. A
	// BeginGeneration entry resolves (Result != Pending) at admission but keeps occupying the
	// front for counting purposes only via ActiveGenerationOpLogIndex, not via OpFront itself --
	// OpFront still advances past it immediately, exactly like every other kind, because nothing
	// AHEAD of the NEXT entry needs to wait for the generation to finish (Reset/Save interrupt an
	// ongoing generation by design). No second BeginGeneration reaches the front while a
	// generation runs: the call refuses it unless a reset is queued ahead of it, and that reset
	// stops the generation at its admission and resolves before the BeginGeneration's turn
	// (D-SLM7946, D-SLM7948).
	void PollSlotFront(int32 SlotIndex)
	{
		FGameSlot& S = Slots[SlotIndex];
		if (!S.OpLog.IsValidIndex(S.OpFront))
		{
			return;
		}
		FLifecycleOpEntry& Entry = S.OpLog[S.OpFront];
		if (!Entry.bSubmitted)
		{
			return;
		}
		if (Entry.Result == ESuperSLMRestoreResult::Pending && Entry.Kind == ELifecycleOpKind::Save && Entry.Async.IsValid() &&
			Entry.Async->AssembleJob.IsValid())
		{
			// Row 19: the save's finalize already ran; it resolves when its assembly job finishes.
			if (Entry.Async->AssembleJob->bDone.load(std::memory_order_acquire))
			{
				CompleteSaveAssembly(SlotIndex, Entry);
			}
		}
		else if (Entry.Result == ESuperSLMRestoreResult::Pending && Entry.Async.IsValid() && Entry.Async->Job.IsValid() &&
			Entry.Async->Job->bDone.load(std::memory_order_acquire))
		{
			// Plan §3.9 (K6): HoldApply() holds this op's apply once its job is done; the entry stays
			// at the front, unresolved, until the hold is released.
			if (PagedHoldBlocks(Entry.HoldId))
			{
				return;
			}
			if (Entry.Kind == ELifecycleOpKind::Reset)
			{
				// Plan §2.5 row 16: a refused reset resolves Malformed with the status named, as the
				// CPU backend's failed reset does.
				const FLifecycleAsyncState& Async = *Entry.Async;
				const bool bSkipped = JobWasSkipped(Async.Job);
				// Plugin paging plan §3.5 (K6): the reset's page-ledger apply, with what its job did.
				ApplyLedgerReset(SlotIndex, Entry.LedgerTicket, !bSkipped && Async.bResetRan && Async.ResetStatus == FGpuStatus::SSLM_OK, Async.bNormalized);
				if (!bSkipped && Async.bResetRan && S.Id == Entry.Owner)
				{
					S.WalkState = Async.WalkState; // ruling 2026-09-26: the game-side walk record
				}
				if (bSkipped)
				{
					// Round 4 (finding 1): the reset never ran; no walk is stored.
					ResolveEntry(Entry, ESuperSLMRestoreResult::NotConfigured);
					LastLifecycleRequestError = LostBeforeRanText();
				}
				else if (Async.bResetRan && Async.ResetStatus != FGpuStatus::SSLM_OK)
				{
					// Layer 1 did not reset the sequence, so it is faulted, with its generation
					// state -- the previous generation's tokens included -- kept as AdmitResetOp()
					// left it (D-SLM7948). Its next generation is refused until a reset succeeds: at
					// the call, or ResetRequired at its turn when it was queued behind this reset.
					const FString Refusal = FString::Printf(TEXT("sslm_gpu_seq_reset returned %s; the sequence was not reset and is Faulted until a reset succeeds"),
						*GpuStatusText(Async.ResetStatus));
					if (S.Id == Entry.Owner)
					{
						FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection, Refusal);
					}
					ResolveEntry(Entry, ESuperSLMRestoreResult::Malformed);
					LastLifecycleRequestError = Refusal;
				}
				else
				{
					// D-SLM7948: the reset has taken effect, so the holder that requested it reads an
					// Idle sequence with no tokens, on the reset's new decode path.
					if (S.Id == Entry.Owner)
					{
						ClearGeneration(S);
						S.Path = Entry.NewDecodePath;
						S.ContextUsedMirror = 0; // plugin paging plan §3.4 (K6): reset to origin 0
						S.bArmedMirror = false;  // K7: a reset clears the engine's ready flag
					}
					ResolveEntry(Entry, ESuperSLMRestoreResult::Success);
				}
			}
			else if (Entry.Kind == ELifecycleOpKind::Save)
			{
				FinalizeSaveOp(SlotIndex, Entry);
			}
			else if (Entry.Kind == ELifecycleOpKind::SchemaBind)
			{
				FinalizeSchemaBindOp(SlotIndex, Entry);
			}
			else if (Entry.Kind == ELifecycleOpKind::AdoptPrefix)
			{
				FinalizeAdoptOp(SlotIndex, Entry); // K7: only a pooled adopt posts a job
			}
		}
		if (Entry.Result != ESuperSLMRestoreResult::Pending)
		{
			S.OpFront += 1;
			CompactSlotOpLog(S); // Entry is not used after this
		}
	}

	// Row 19 (D-SLM7763), as PollRestoreFront() does for the restore queue: a resolved entry keeps
	// only its result, in HandleResults, so the entries behind the front are removed and the log
	// compacted, without shrinking. While a generation runs (ActiveGenerationOpLogIndex is set)
	// nothing is removed, so the running generation's own entry, which that index points at, and
	// every entry resolved behind it (a Save or AdoptPrefix requested during the generation) stay,
	// and no index moves. A long generation with many such requests therefore grows the log by one
	// entry each, and can outgrow its MaxQueued + 2 reserve and reallocate. Complete() and
	// FaultSlot() only clear ActiveGenerationOpLogIndex (a reset's admission clears it too); this
	// function, whose one caller is PollSlotFront(), is what removes the kept entries, the first
	// time an entry on this sequence resolves after the generation has ended. Until then (and if
	// no further request is made, until the sequence is returned or re-vended, which resets the
	// log) they stay. The growth is bounded by what one generation was asked to do, and the memory
	// is kept, without shrinking, for the sequence's next requests.
	void CompactSlotOpLog(FGameSlot& S)
	{
		if (S.ActiveGenerationOpLogIndex != INDEX_NONE)
		{
			return;
		}
		const int32 Removable = S.OpFront;
		if (Removable <= 0)
		{
			return;
		}
		S.OpLog.RemoveAt(0, Removable, EAllowShrinking::No);
		S.OpFront -= Removable;
	}

	// --- Restore: the subsystem-level queue (D-SLM7457 ruling 2 -- no target sequence to collide
	// against, so it is not part of any FGameSlot::OpLog). ---

	FSuperSLMLifecycleOpHandle EnqueueRestoreOp(const TArray<uint8>& Blob, USuperSLMModel* ExpectedModel, FLifecycleOpEntry** OutEntry)
	{
		*OutEntry = nullptr;
		SuperSLMSaveBlob::FContents Contents;
		const uint8* Payload = nullptr;
		int64 PayloadBytes = 0;
		FString ParseError;
		if (!SuperSLMSaveBlob::Read(Blob, Contents, Payload, PayloadBytes, ParseError))
		{
			LastLifecycleRequestError = ParseError;
			return FSuperSLMLifecycleOpHandle(); // Malformed -- no entry queued; matches every other
				// request-time refusal's "refused before anything is queued" shape.
		}
		if (Contents.Backend != ESuperSLMBackend::GPU)
		{
			LastLifecycleRequestError = TEXT("the blob was saved by the CPU backend; a CPU blob never restores into the GPU backend");
			return FSuperSLMLifecycleOpHandle();
		}
		// Plan §2.5 row 23 (D-SLM7714): a blob saved under another Layer-1 pin is refused by name
		// before anything is queued (ESuperSLMRestoreResult::Layer1Mismatch; on this queued surface
		// the refusal is an invalid handle with the result named here). Saves do not carry across a
		// change of pin (plan §4, §12 decision 13).
		const FString CompiledTag = SuperSLMSaveBlob::CompiledLayer1Tag();
#if SUPERSLM_WITH_PREFIX_CACHE
		// Precache plan §5.8: the shipped compatibility table admits a named predecessor's saves.
		if (SuperSLMCore::CheckSaveLayer1(std::string(TCHAR_TO_UTF8(*CompiledTag)), std::string(TCHAR_TO_UTF8(*Contents.Layer1Tag)),
				std::string(TCHAR_TO_UTF8(*Contents.Layer1Commit)), Payload, PayloadBytes > 0 ? static_cast<size_t>(PayloadBytes) : 0)
			== SuperSLMCore::ESaveLayer1Check::Layer1Mismatch)
#else
		if (Contents.Layer1Tag != CompiledTag)
#endif
		{
			LastLifecycleRequestError = FString::Printf(
				TEXT("Layer1Mismatch: the blob was saved by a build pinned to Layer 1 %s, and this build is pinned to Layer 1 %s; saves do not carry across a change of pin"),
				*Contents.Layer1Tag, *CompiledTag);
			return FSuperSLMLifecycleOpHandle();
		}
		if (!bActive || bTerminalLoss.load())
		{
			LastLifecycleRequestError = TEXT("the GPU subsystem is not configured");
			return FSuperSLMLifecycleOpHandle();
		}
		uint8 ExpectedHash[32];
		if (ExpectedModel == nullptr || !SuperSLMRuntime::ReadArtifactHash(*ExpectedModel, ExpectedHash) ||
			FMemory::Memcmp(ExpectedHash, Contents.ArtifactHash, 32) != 0)
		{
			LastLifecycleRequestError = FString::Printf(TEXT("the blob was saved against artifact %s, not the expected model"), *SuperSLMRuntime::HashToHex(Contents.ArtifactHash));
			return FSuperSLMLifecycleOpHandle();
		}
		if (FMemory::Memcmp(ExpectedHash, ArtifactHash, 32) != 0)
		{
			LastLifecycleRequestError = TEXT("the expected model is not the one this GPU subsystem is configured with");
			return FSuperSLMLifecycleOpHandle();
		}

		int64 Cursor = 0;
		bool bBad = false;
		auto Need = [&](int64 N) { if (Cursor + N > PayloadBytes) { bBad = true; } return !bBad; };
		auto GetU8 = [&]() -> uint8 { return Need(1) ? Payload[Cursor++] : 0; };
		auto GetU32 = [&]() -> uint32 { uint32 V = 0; if (Need(4)) { for (int32 B = 0; B < 4; ++B) { V |= static_cast<uint32>(Payload[Cursor + B]) << (8 * B); } Cursor += 4; } return V; };
		auto GetU64 = [&]() -> uint64 { uint64 V = 0; if (Need(8)) { for (int32 B = 0; B < 8; ++B) { V |= static_cast<uint64>(Payload[Cursor + B]) << (8 * B); } Cursor += 8; } return V; };
		const uint32 Magic = GetU32();
		const uint32 Version = GetU32();
		const uint8 PathByte = GetU8();
		const bool bPrimed = GetU8() != 0;
		const bool bStopped = GetU8() != 0;
		GetU8();
		const int32 LastToken = static_cast<int32>(GetU32());
		const int32 InFlightIssued = static_cast<int32>(GetU32());
		const uint32 CarriedCount = GetU32();
		if (bBad || Magic != kWrapperExtMagic || Version != kWrapperExtVersion || PathByte > 1 || CarriedCount > 1u << 20)
		{
			LastLifecycleRequestError = TEXT("the blob's GPU section is malformed or from an unsupported plugin version");
			return FSuperSLMLifecycleOpHandle();
		}
		TArray<FLifecycleOpEntry::FRestoreCarried> Carried;
		for (uint32 I = 0; I < CarriedCount && !bBad; ++I)
		{
			FLifecycleOpEntry::FRestoreCarried C;
			C.Kind = static_cast<EResultKind>(GetU8());
			C.Fault = static_cast<ESuperSLMGpuFaultReason>(GetU8());
			GetU8();
			GetU8();
			C.Token = static_cast<int32>(GetU32());
			C.Delay = static_cast<int32>(GetU32());
			Carried.Add(C);
		}
		const uint64 Layer1Bytes = GetU64();
		if (bBad || Layer1Bytes != static_cast<uint64>(PayloadBytes - Cursor) || Layer1Bytes == 0)
		{
			LastLifecycleRequestError = TEXT("the blob's GPU section is truncated or its Layer-1 length is inconsistent");
			return FSuperSLMLifecycleOpHandle();
		}
		// Plugin paging plan §3.5 (K7): the Layer-1 payload's header, checked at the call -- the mode
		// crossing by magic in both modes, and a pooled payload's 'SLM6' fields.
		int64 HeaderBudget = 0;
		int64 HeaderOrigin = 0;
		bool bHeaderReady = false;
		if (!CheckRestorePayloadHeader(Payload + Cursor, static_cast<int64>(Layer1Bytes), Contents.ContextUsed, HeaderBudget, HeaderOrigin, bHeaderReady))
		{
			return FSuperSLMLifecycleOpHandle();
		}

		int32 RestoreSchemaIndex = -1;
		if (!Contents.SchemaName.IsEmpty())
		{
			size_t Index = 0;
			const FTCHARToUTF8 NameUtf8(*Contents.SchemaName);
			if (Schemas.ByName(std::string_view(NameUtf8.Get(), NameUtf8.Length()), &Index) == nullptr)
			{
				LastLifecycleRequestError = FString::Printf(TEXT("the blob's bound schema '%s' is not in the configured artifact"), *Contents.SchemaName);
				return FSuperSLMLifecycleOpHandle();
			}
			RestoreSchemaIndex = static_cast<int32>(Index);
		}

		FLifecycleOpEntry Entry;
		Entry.HandleId = NextLifecycleOpHandleId++;
		Entry.Kind = ELifecycleOpKind::Restore;
		Entry.GlobalOrdinal = NextGlobalRequestOrdinal++;
		Entry.RestoreLayer1Offset = (Payload - Blob.GetData()) + Cursor;
		Entry.RestoreLayer1Size = static_cast<int64>(Layer1Bytes);
		Entry.RestoreExpectedModel = ExpectedModel;
		Entry.RestoreContents = Contents;
		Entry.RestoreSchemaIndex = RestoreSchemaIndex;
		Entry.RestorePathByte = PathByte;
		Entry.bRestorePrimed = bPrimed;
		Entry.bRestoreStopped = bStopped;
		Entry.RestoreLastToken = LastToken;
		Entry.RestoreInFlightIssued = InFlightIssued;
		Entry.RestoreCarried = MoveTemp(Carried);
		Entry.bRestorePooled = IsPooled();
		Entry.RestoreBudget = HeaderBudget;
		Entry.RestoreOrigin = HeaderOrigin;
		Entry.bRestoreReady = bHeaderReady;
		if (Contents.bHasAdapter)
		{
			for (const TPair<int64, FAdapterEntry>& Pair : Adapters)
			{
				if (FMemory::Memcmp(Pair.Value.ArtifactHash, Contents.AdapterHash, 32) == 0)
				{
					Entry.RestoreAdapterId = Pair.Key;
					break;
				}
			}
			if (Entry.RestoreAdapterId == 0)
			{
				LastLifecycleRequestError = FString::Printf(TEXT("the blob was saved with adapter %s bound, and no adapter with that artifact hash is mapped on this GPU subsystem"),
					*SuperSLMRuntime::HashToHex(Contents.AdapterHash));
				return FSuperSLMLifecycleOpHandle();
			}
			// Rule 4 (T-2987 L1): pinned from here until finalize.
			Adapters[Entry.RestoreAdapterId].RestorePins += 1;
			Entry.bRestoreAdapterPinned = true;
		}

		// Row 19: the one copy, at acceptance, after every request-time check; the Layer-1 payload
		// is read in place from it. The op owns it until its job takes it (AdmitRestoreFront()).
		Entry.RestoreBlob = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(Blob);
		const int32 Index2 = RestoreOps.Add(MoveTemp(Entry));
		*OutEntry = &RestoreOps[Index2];
		FSuperSLMLifecycleOpHandle H;
		H.Id = (*OutEntry)->HandleId;
		return H;
	}

	// The RestoreOps-queue counterpart to TryAdmitImmediately() above -- admits the just-queued
	// restore now if nothing else in the subsystem-level restore queue is ahead of it.
	void TryAdmitRestoreImmediately()
	{
		if (IsPooled())
		{
			ArrivePooledRestores(); // §3.5 (K7): every restore not yet arrived, in arrival order
			return;
		}
		if (RestoreOps.IsValidIndex(RestoreOpsFront) && !RestoreOps[RestoreOpsFront].bSubmitted)
		{
			AdmitRestoreFront();
		}
	}

	// Resolved on the GAME THREAD, before the async job runs, and captured into the job BY VALUE
	// (a raw Layer-1 handle pointer, stable for the life of this Configure() session -- the same
	// stability RestoreAdapter already relied on in the prior synchronous RestoreSequence()).
	const SslmGpuAdapterHandle* ResolveRestoreAdapterHandle(int64 RestoreAdapterId) const
	{
		if (RestoreAdapterId == 0)
		{
			return nullptr;
		}
		const FAdapterEntry* AdapterEntry = Adapters.Find(RestoreAdapterId);
		return AdapterEntry != nullptr ? AdapterEntry->Handle : nullptr;
	}

	void AdmitRestoreFront()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Restore", SuperSLMGpuChannel);
		FLifecycleOpEntry& Entry = RestoreOps[RestoreOpsFront];
		Entry.bSubmitted = true;
		// Rule 4 (T-2987 L1, F4): the adapter the restore binds, re-resolved from the id pinned at
		// request time. A null re-resolve refuses the restore by name -- never a restore that skips
		// the bind and still records the adapter.
		const SslmGpuAdapterHandle* RestoreAdapterHandle = ResolveRestoreAdapterHandle(Entry.RestoreAdapterId);
		if (Entry.RestoreAdapterId != 0 && RestoreAdapterHandle == nullptr)
		{
			ReleaseRestorePin(Entry);
			ResolveEntry(Entry, ESuperSLMRestoreResult::AdapterUnavailable);
			LastLifecycleRequestError = FString::Printf(TEXT("the blob was saved with adapter %s bound, and that adapter is no longer mapped on this GPU subsystem"),
				*SuperSLMRuntime::HashToHex(Entry.RestoreContents.AdapterHash));
			return;
		}
		int32 SlotIndex = INDEX_NONE;
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			if (!Slots[I].bVended && !Slots[I].bWithheld)
			{
				SlotIndex = I;
				break;
			}
		}
		if (SlotIndex == INDEX_NONE)
		{
			ReleaseRestorePin(Entry);
			ResolveEntry(Entry, ESuperSLMRestoreResult::PoolExhausted);
			LastLifecycleRequestError = TEXT("every pooled GPU sequence is vended; a restore needs a free one");
			return;
		}
		// Reserve the slot NOW, on the game thread, so VendSequence() and GetPoolFreeCount() see
		// it as occupied for the whole window between this admission and FinalizeRestoreOp() -- a
		// restore is only finalized on a LATER tick's poll, and without this reservation
		// VendSequence() honours the identical "!bVended" predicate this loop just used and can
		// hand the same slot to a caller while the restore is still in flight (finding 2, T-2885
		// review). FinalizeRestoreOp() releases the reservation if the restore itself fails.
		Slots[SlotIndex].bVended = true;
		Entry.RestoreSlotIndex = SlotIndex;
		Entry.Async = MakeShared<FLifecycleAsyncState>();
		TSharedPtr<FLifecycleAsyncState> Async = Entry.Async;
		BumpGeneration(SlotIndex);
		const uint32 Gen = Slots[SlotIndex].Gen;
		const int32 SchemaIndex = Entry.RestoreSchemaIndex;
		// Row 19 (D-SLM7763): the job takes the op's buffer (a move, not a share) and reads the payload
		// in place; Run() releases the job's closure, and with it the blob, on the submission thread.
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Blob = MoveTemp(Entry.RestoreBlob);
		const int64 Layer1Offset = Entry.RestoreLayer1Offset;
		const int64 Layer1Size = Entry.RestoreLayer1Size;
		// GAME-THREAD-parsed facts (EnqueueRestoreOp(), request time) the restore job below
		// applies directly to the restored FThreadSlot -- captured by value exactly as
		// SchemaIndex and RestoreAdapterHandle already are, so the decode path reads the same
		// LastToken/stop set/context-used/remaining-budget the sequence had at save time instead
		// of FThreadSlot's defaults (finding 1, T-2885 review: these were previously parsed and
		// read nowhere).
		const int32 RestoreLastToken = Entry.RestoreLastToken;
		const bool bRestoreStopped = Entry.bRestoreStopped;
		const int64 RestoreContextUsed = Entry.RestoreContents.ContextUsed;
		const int32 RestoreMaxNewTokens = Entry.RestoreContents.MaxNewTokensRemaining;
		const TArray<int32> RestoreStopTokenIds = Entry.RestoreContents.StopTokenIds;
		FSuperSLMGpuSubsystemState* St8 = this;
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		// On the submission thread, after every queued job: nothing is Submitted there, so Layer
		// 1's process-wide restore precondition holds and the caller never sees SSLM_BUSY
		// (R-S2d) -- unchanged from the prior synchronous RestoreSequence()'s own reasoning. A
		// single call, no retry: the header's own invariant is that the plugin never asks Layer 1
		// a question whose answer could be SSLM_BUSY, and a retry loop that could fire would do so
		// while holding Layer1GpuLock() -- the same process-wide lock every other submitter needs
		// to clear a busy condition -- so it could only ever stall to the same failure (finding 8,
		// T-2885 review).
		Job->Custom = [St8, SlotIndex, Gen, SchemaIndex, RestoreAdapterHandle, Blob = MoveTemp(Blob), Layer1Offset, Layer1Size, Async,
			RestoreLastToken, bRestoreStopped, RestoreContextUsed, RestoreMaxNewTokens, RestoreStopTokenIds]()
		{
			SslmGpuSequenceHandle* Fresh = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
			const FGpuStatus ForcedRestoreSt = static_cast<FGpuStatus>(St8->TestNextRestoreStatus.exchange(0u, std::memory_order_acq_rel));
			Async->RestoreStatus = ForcedRestoreSt != FGpuStatus::SSLM_OK ? ForcedRestoreSt
				: sslm_gpu_seq_restore(St8->Ctx, St8->Model, Blob->GetData() + Layer1Offset, static_cast<size_t>(Layer1Size), &Fresh);
#else
			Async->RestoreStatus = sslm_gpu_seq_restore(St8->Ctx, St8->Model, Blob->GetData() + Layer1Offset, static_cast<size_t>(Layer1Size), &Fresh);
#endif
			St8->NoteLayer1Status(Async->RestoreStatus); // plugin paging plan §3.9 (K6): Layer1BusyCount()
			if (Async->RestoreStatus != FGpuStatus::SSLM_OK || Fresh == nullptr)
			{
				Async->RestoreWhere = TEXT("sslm_gpu_seq_restore");
				return;
			}
			// Plan §2.5 row 5 (D-SLM7665): 'SLM5' restores the schema binding and its walk itself,
			// and a restored sequence is not bind-eligible, so the plugin never re-binds. It checks
			// instead that the restored binding agrees with the wrapper: the one mismatch a
			// producer can emit is an 'SLM4' blob saved bound by a v1.5.0-pinned build of this
			// plugin, which restores unbound.
			int32_t SchemaBound = 0;
			const FGpuStatus BoundSt = SslmGpuSeqSchemaBoundForG5Bridge(St8->Ctx, Fresh, &SchemaBound);
			St8->NoteLayer1Status(BoundSt); // K6 review S2
			if (BoundSt != FGpuStatus::SSLM_OK || (SchemaBound != 0) != (SchemaIndex >= 0))
			{
				Async->bSchemaCrossCheckFailed = true;
				Async->RestoreStatus = BoundSt != FGpuStatus::SSLM_OK ? BoundSt : FGpuStatus::SSLM_SEQUENCE_REJECTED;
				Async->RestoreWhere = BoundSt != FGpuStatus::SSLM_OK
					? FString(TEXT("SslmGpuSeqSchemaBoundForG5Bridge (restore cross-check)"))
					: FString::Printf(TEXT("the restore cross-check: the wrapper %s a schema, but the restored sequence %s one"),
						SchemaIndex >= 0 ? TEXT("names") : TEXT("names no"), SchemaBound != 0 ? TEXT("has") : TEXT("has no"));
				St8->NoteLayer1Status(sslm_gpu_seq_release(St8->Ctx, Fresh));
				return;
			}
			if (RestoreAdapterHandle != nullptr)
			{
				// The adapter binding is not carried by the blob; bind_adapter is not subject to
				// bind eligibility, so it stays legal on a restored sequence.
				Async->RestoreStatus = sslm_gpu_seq_bind_adapter(St8->Ctx, Fresh, RestoreAdapterHandle);
				St8->NoteLayer1Status(Async->RestoreStatus);
				if (Async->RestoreStatus != FGpuStatus::SSLM_OK)
				{
					Async->RestoreWhere = TEXT("sslm_gpu_seq_bind_adapter (re-bind after restore)");
					St8->NoteLayer1Status(sslm_gpu_seq_release(St8->Ctx, Fresh));
					return;
				}
				Async->bAdapterBound = true;
			}
			FThreadSlot& TS = St8->ThreadSlots[SlotIndex];
			if (TS.Handle != nullptr)
			{
				St8->NoteLayer1Status(sslm_gpu_seq_release(St8->Ctx, TS.Handle));
			}
			TS = FThreadSlot();
			TS.Handle = Fresh;
			TS.Cap = St8->Config.ContextCap;
			TS.KvBytes = St8->bPooled ? 0 : St8->KvBytesPerSequence; // a pooled holder's K/V is the pool's (K6)
			TS.Gen = Gen;
			TS.bPrimed = Async->bPrimed; // set below from Entry's own parsed fields before Enqueue
			TS.SchemaIndex = SchemaIndex;
			TS.BoundAdapter = RestoreAdapterHandle;
			// The rest of the submission-thread state the decode path actually reads (finding 1,
			// T-2885 review) -- previously left at FThreadSlot's defaults.
			TS.LastToken = RestoreLastToken;
			TS.bStopped = bRestoreStopped;
			TS.ContextUsed = RestoreContextUsed;
			TS.MaxNewTokens = RestoreMaxNewTokens;
			TS.StopTokenIds = RestoreStopTokenIds;
			// Ruling 2026-09-26: the restored walk ('SLM5' carries it), stored by FinalizeRestoreOp().
			Async->WalkState = SslmGpuSeqWalkStateForG5Bridge(Fresh);
			Async->FreshHandle = Fresh;
		};
		// bPrimed is applied on the thread above (needed for RunOneCallToken's own bPrimed-gated
		// branch); bStopped/LastToken/ContextUsed/MaxNewTokens/StopTokenIds are applied there too,
		// by value, above. TokensIssued/Produced start at their FThreadSlot defaults (0), matching
		// FinalizeRestoreOp()'s own S.Request.MaxNewTokens = MaxNewTokensRemaining on the game
		// side: both sides count NEW tokens produced since restore against the REMAINING budget,
		// not the original one.
		Async->bPrimed = Entry.bRestorePrimed;
		Async->Job = Enqueue(Job);
		JobsPostedByKind[static_cast<int32>(EGpuPagedJobKind::Restore)] += 1; // plugin paging plan §3.9 (K6)
	}

	// Rule 4 (T-2987 L1): releases the adapter pin a restore took at request time, once the
	// restore resolves.
	void ReleaseRestorePin(FLifecycleOpEntry& Entry)
	{
		if (!Entry.bRestoreAdapterPinned)
		{
			return;
		}
		Entry.bRestoreAdapterPinned = false;
		if (FAdapterEntry* AdapterEntry = Adapters.Find(Entry.RestoreAdapterId))
		{
			AdapterEntry->RestorePins = FMath::Max(0, AdapterEntry->RestorePins - 1);
		}
	}

	// Runs once AdmitRestoreFront()'s async job has completed: applies the parsed blob contents
	// (already validated at request time, EnqueueRestoreOp()) to the reserved FGameSlot.
	void FinalizeRestoreOp(FLifecycleOpEntry& Entry)
	{
		FLifecycleAsyncState& Async = *Entry.Async;
		const int32 SlotIndex = Entry.RestoreSlotIndex;
		ReleaseRestorePin(Entry);

		auto Refuse = [this, &Entry, SlotIndex](ESuperSLMRestoreResult Result, const FString& Why)
		{
			LastLifecycleRequestError = Why;
			// Plan §2.5 row 20, H8 (T-2983 N2): admission bumped the slot's game-side generation,
			// and only a successful restore re-syncs the thread side, so a refusal that simply
			// released the reservation left the slot's generation out of step and stranded its
			// next holder (every action dropped by ActionIsLive()). Every failure path therefore
			// queues the unbinding reset on the pooled handle the failure left untouched -- which
			// re-syncs the generation -- before it releases the reservation.
			// Plugin paging plan §3.5 (K7), my K6 note 2: a pooled restore's own ledger job is applied
			// (as failed: its H charge refunded, its holder's shape kept) before the recycle, whose
			// reset the ledger admits only on a slot with no open job.
			if (Entry.LedgerTicket != SuperSLMPaging::InvalidTicket)
			{
				Ledger.ApplyRestore(Entry.LedgerTicket, false, 0u);
				Entry.LedgerTicket = SuperSLMPaging::InvalidTicket;
			}
			if (Slots.IsValidIndex(SlotIndex))
			{
				Slots[SlotIndex].bRestoreReserved = false;
				if (Thread != nullptr)
				{
					QueueRecycle(SlotIndex);
				}
				Slots[SlotIndex].bVended = false;
			}
			ResolveEntry(Entry, Result);
		};

		if (JobWasSkipped(Async.Job))
		{
			// Round 4 (finding 1): the restore never ran; its reservation is released as on every
			// refused path, and no sequence is restored.
			Refuse(ESuperSLMRestoreResult::NotConfigured, LostBeforeRanText());
			return;
		}
		if (Async.RestoreStatus != FGpuStatus::SSLM_OK)
		{
			const FString Why = Async.bSchemaCrossCheckFailed
				? FString::Printf(TEXT("%s (status %s)"), *Async.RestoreWhere, *GpuStatusText(Async.RestoreStatus))
				: FString::Printf(TEXT("%s returned %s"), *Async.RestoreWhere, *GpuStatusText(Async.RestoreStatus));
			// Row 5: a failed schema cross-check is refused Malformed by name. Out of GPU memory
			// (SSLM_GPU_ALLOCATION_FAILED) reads OutOfMemory; the mapping is one function
			// (SuperSLMGpuStatusMapping::ToRestoreFailureResult).
			// Plugin paging plan §3.7 (K6): the mapping takes the subsystem's mode. In pooled mode a
			// KV_POOL_EXHAUSTED on a restore the ledger admitted is a ledger fault too.
			if (IsPooled() && !Async.bSchemaCrossCheckFailed && Async.RestoreStatus == FGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED)
			{
				NoteLedgerFault(TEXT("the restore into the pool"));
			}
			Refuse(SuperSLMGpuStatusMapping::ToRestoreFailureResult(Async.RestoreStatus, IsPooled(), Async.bSchemaCrossCheckFailed), Why);
			return;
		}
		// SlotIndex is the slot admission already chose and reserved, not re-derived by matching
		// Async.FreshHandle against ThreadSlots (finding 2, T-2885 review) -- the restore job
		// above wrote Fresh into exactly this SlotIndex, captured by value at admission.
		if (!Slots.IsValidIndex(SlotIndex) || ThreadSlots[SlotIndex].Handle != Async.FreshHandle)
		{
			Refuse(ESuperSLMRestoreResult::Malformed, TEXT("the restored Layer-1 handle could not be matched back to its own slot"));
			return;
		}
		FGameSlot& S = Slots[SlotIndex];
		// Plugin paging plan §3.5 (K7): the pooled restore's ledger job applies with the pages the
		// hint let it share; the slot's holder takes the payload's shape (the ledger sets it here).
		if (Entry.LedgerTicket != SuperSLMPaging::InvalidTicket)
		{
			Ledger.ApplyRestore(Entry.LedgerTicket, true, Async.SharedPages);
			Entry.LedgerTicket = SuperSLMPaging::InvalidTicket;
		}
		S.bRestoreReserved = false;
		// Rule 1 (H8): the restore's holder starts from a value-reset per-user struct --
		// LayersPerSliceOverride included -- before the blob's own fields apply.
		DropPendingEvents(S); // empty on a free slot; any event is logged as dropped (ruling 2026-09-26)
		static_cast<FGameSlotUser&>(S) = FGameSlotUser();
		// §3.4 "The armed flag": the restored holder is armed exactly when the payload's ready word
		// says its last write was a prefill arc (pooled); a dedicated restore's mirror stays false.
		S.bArmedMirror = Entry.bRestorePooled && Entry.bRestoreReady;
		S.OpLog.Reset();
		S.OpFront = 0;
		S.bVended = true;
		S.bReturnPending = false;
		S.Id = NextSequenceId++;
		// Row 4: Layer 1 clears bind eligibility on restore; SetSchema() refuses by name until
		// the holder resets the restored sequence.
		S.bBindEligibleAfterQueue = false;
		S.Path = static_cast<ESuperSLMGpuDecodePath>(Entry.RestorePathByte);
		S.Phase = Entry.RestoreContents.Phase;
		S.Request.MaxNewTokens = Entry.RestoreContents.MaxNewTokensRemaining;
		S.Request.StopTokenIds = Entry.RestoreContents.StopTokenIds;
		S.Request.SpanKind = Entry.RestoreContents.SpanKind;
		S.Request.PromptTokens = Entry.RestoreContents.PromptRemaining;
		S.PromptPlanned = 0;
		S.bPlanPrimed = Entry.bRestorePrimed;
		S.TokensIssued = Entry.RestoreInFlightIssued;
		S.Schema.Index = Entry.RestoreSchemaIndex;
		S.SchemaName = Entry.RestoreContents.SchemaName;
		S.WalkState = Async.WalkState; // ruling 2026-09-26: the game-side walk record, as restored
		// Plugin paging plan §3.4, §3.9 (K6): the restored holder's ContextUsed mirror, and the slot's
		// handle generation (its Layer-1 handle was replaced).
		S.ContextUsedMirror = Entry.RestoreContents.ContextUsed;
		S.HolderGeneration += 1;
		// Rule 4 (T-2987 F4): the adapter is recorded only when the restore job bound it.
		S.ActiveAdapterId = Async.bAdapterBound ? Entry.RestoreAdapterId : 0;
		S.RequestedAdapterId = S.ActiveAdapterId;
		if (Entry.RestoreCarried.Num() > 0)
		{
			TSharedPtr<FJob> Done = MakeShared<FJob>();
			Done->bDone.store(true);
			Done->CompletedWallSeconds = FPlatformTime::Seconds();
			for (const FLifecycleOpEntry::FRestoreCarried& C : Entry.RestoreCarried)
			{
				FActionResult R;
				R.Kind = C.Kind;
				R.Fault = C.Fault;
				R.Token = C.Token;
				const int32 Index = Done->Results.Add(R);
				FPendingEvent E;
				E.Kind = C.Kind == EResultKind::PrefillOk ? EEventKind::Prefill : EEventKind::Token;
				// Round 4 (finding 4): the carried delay is clamped to K - 1, so ApplyTick never
				// decreases within a slot's Pending: a carried event applies by TickIndex + K - 1, and
				// every event planned from here on has ApplyTick >= TickIndex + K (its T is at least
				// this tick's). ApplyDue()'s in-order walk depends on that invariant.
				E.ApplyTick = TickIndex + FMath::Max(1, FMath::Min(C.Delay, Config.K - 1));
				E.RequestTick = E.ApplyTick - Config.K;
				E.Job = Done;
				E.ResultIndex = Index;
				LogEvent(S, E, true); // a carried event, rebuilt by FinalizeRestoreOp()
				S.Pending.Add(E);
			}
		}
		IdToSlot.Add(S.Id, SlotIndex);
		Entry.RestoreOutSequence.Id = S.Id;
		ResolveEntry(Entry, ESuperSLMRestoreResult::Success);
	}

	void PollRestoreFront()
	{
		if (!RestoreOps.IsValidIndex(RestoreOpsFront))
		{
			return;
		}
		FLifecycleOpEntry& Entry = RestoreOps[RestoreOpsFront];
		if (!Entry.bSubmitted)
		{
			return;
		}
		if (Entry.Result == ESuperSLMRestoreResult::Pending && Entry.Async.IsValid() && Entry.Async->Job.IsValid() &&
			Entry.Async->Job->bDone.load(std::memory_order_acquire))
		{
			FinalizeRestoreOp(Entry);
		}
		if (Entry.Result != ESuperSLMRestoreResult::Pending)
		{
			RestoreOpsFront += 1;
		}
		// Row 19 (D-SLM7763): a resolved restore keeps only its result, in HandleResults; its entry
		// is removed and the array compacted behind the front. The blob left with the job (or, for
		// a refusal before admission, through ReleaseBlobOffGameThread()).
		if (RestoreOpsFront > 0)
		{
			RestoreOps.RemoveAt(0, RestoreOpsFront, EAllowShrinking::No);
			RestoreOpsFront = 0;
		}
	}

	// Completes a ReturnSequence() the plan's own queue-drain rule deferred (§5: "the physical
	// block is not handed to a new vend ... until every operation already queued for this
	// sequence ... has drained in arrival order").
	void FinishDeferredReturn(int32 SlotIndex)
	{
		ReleaseUser(SlotIndex); // plan §2.5 row 20 rule 1 (H7)
	}

	// The admission pass, called once per tick before decode planning (Tick()): polls every
	// in-flight op for completion, then admits every newly-eligible front-of-queue entry --
	// across every slot's own queue and the subsystem-level restore queue -- in global-request-
	// ordinal order (D-SLM7428).
	void AdmitQueuedOps()
	{
		PollRecycles(); // plan §2.5 row 16: a refused recycle withholds its slot
		PollPrefixBuilds(); // plugin paging plan §3.4 (K7): begins, build turns and releases
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			PollSlotFront(SlotIndex);
		}
		if (IsPooled())
		{
			PollPooledRestores(); // §3.5 (K7): several restores may be in flight, each on its own slot
		}
		else
		{
			PollRestoreFront();
		}
		TakePoolStatsSnapshot(); // K6 review N2: the last applied paged job's pool stats

		struct FCandidate { bool bIsRestore; int32 SlotIndex; int64 Ordinal; };
		TArray<FCandidate> Candidates;
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			const FGameSlot& S = Slots[SlotIndex];
			if (S.OpLog.IsValidIndex(S.OpFront) && !S.OpLog[S.OpFront].bSubmitted)
			{
				Candidates.Add({false, SlotIndex, S.OpLog[S.OpFront].GlobalOrdinal});
			}
		}
		if (IsPooled())
		{
			// §3.5 (K7): the first restore not yet arrived takes the restore queue's place in the order.
			for (const FLifecycleOpEntry& Op : RestoreOps)
			{
				if (!Op.bSubmitted && Op.Result == ESuperSLMRestoreResult::Pending)
				{
					Candidates.Add({true, INDEX_NONE, Op.GlobalOrdinal});
					break;
				}
			}
		}
		else if (RestoreOps.IsValidIndex(RestoreOpsFront) && !RestoreOps[RestoreOpsFront].bSubmitted)
		{
			Candidates.Add({true, INDEX_NONE, RestoreOps[RestoreOpsFront].GlobalOrdinal});
		}
		Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Ordinal < B.Ordinal; });
		for (const FCandidate& C : Candidates)
		{
			if (C.bIsRestore)
			{
				if (IsPooled())
				{
					ArrivePooledRestores();
				}
				else
				{
					AdmitRestoreFront();
				}
			}
			else
			{
				AdmitSlotFront(C.SlotIndex);
			}
		}

		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			FGameSlot& S = Slots[SlotIndex];
			if (S.bReturnPending && S.OpFront >= S.OpLog.Num())
			{
				FinishDeferredReturn(SlotIndex);
			}
		}
	}

	void FaultSlot(FGameSlot& S, ESuperSLMDecodeOutcome Outcome, ESuperSLMGpuFaultReason Reason, const FString& Message)
	{
		S.Phase = ESuperSLMSequencePhase::Faulted;
		S.LastOutcome = Outcome;
		S.LastFault = Reason;
		DropPendingEvents(S);
		S.ActiveGenerationOpLogIndex = INDEX_NONE;
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU sequence %lld faulted: %s"), S.Id, *Message);
	}

	void Complete(FGameSlot& S)
	{
		S.Phase = ESuperSLMSequencePhase::Complete;
		DropPendingEvents(S);
		S.ActiveGenerationOpLogIndex = INDEX_NONE;
	}

	// Commits a bind WantsAdapterBind() planned, only on a result that echoes the requested bind's
	// tag (plan §2.5 row 20 rule 4, T-2983 finding 10). A result planned before the request
	// echoes an older tag and confirms nothing, so a swap requested after a sequence's last token
	// was planned is never reported applied while the next generation runs on the old binding. A
	// Skipped result leaves bSwapPending set, so the next planned token carries the bind again.
	void ConfirmPendingBind(FGameSlot& S, const FActionResult& R)
	{
		if (S.bPendingBindConfirm && R.BindTag == S.AwaitedBindTag)
		{
			S.ActiveAdapterId = S.PendingBindAdapterId;
			S.bSwapPending = false;
			S.bPendingBindConfirm = false;
		}
	}

	// Applies one result to its sequence. Results arrive in order per sequence.
	void ApplyResult(FGameSlot& S, const FPendingEvent& E, const FActionResult& R)
	{
		// Plugin paging plan §3.4 (K6): the slot's ContextUsed mirror, from every result whose
		// action ran (the stale-result filter already dropped the others).
		if (R.ContextUsed >= 0)
		{
			S.ContextUsedMirror = R.ContextUsed;
		}
		if (R.Armed >= 0)
		{
			S.bArmedMirror = R.Armed != 0; // K7: the armed mirror, from the same results
		}
		switch (R.Kind)
		{
			case EResultKind::Skipped:
			case EResultKind::Progress:
				return;
			case EResultKind::PrefillOk:
				ConfirmPendingBind(S, R);
				if (S.Phase == ESuperSLMSequencePhase::Prefilling)
				{
					S.Phase = ESuperSLMSequencePhase::Decoding;
				}
				return;
			case EResultKind::Token:
				ConfirmPendingBind(S, R);
				S.Phase = ESuperSLMSequencePhase::Decoding;
				S.Generated.Add(R.Token);
				S.bSchemaAccepting = R.bSchemaAccepting;
				if (R.bHasWalkState)
				{
					S.WalkState = R.WalkState; // ruling 2026-09-26: the game-side walk record
				}
				TRACE_COUNTER_INCREMENT(SuperSLMGpuTokensDelivered);
				S.LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
				if (S.FirstTokenMs < 0.0 && S.BeginWallSeconds > 0.0)
				{
					S.FirstTokenMs = (FPlatformTime::Seconds() - S.BeginWallSeconds) * 1000.0;
				}
				if (S.Generated.Num() >= S.Request.MaxNewTokens || S.Request.StopTokenIds.Contains(R.Token))
				{
					Complete(S);
				}
				return;
			case EResultKind::DeadEnd:
				S.bSchemaAccepting = R.bSchemaAccepting;
				if (R.bHasWalkState)
				{
					S.WalkState = R.WalkState; // a dead end leaves the walk where it was
				}
				FaultSlot(S, ESuperSLMDecodeOutcome::SchemaDeadEnd, ESuperSLMGpuFaultReason::None, R.Message);
				return;
			case EResultKind::Fault:
				FaultSlot(S, ESuperSLMDecodeOutcome::Generating, R.Fault, R.Message);
				return;
		}
	}

	// Ruling 2026-09-26 (plan §2.5 row 21, §5 "The T+K contract"): the examination that replaced
	// AwaitEvent(). It never waits and reads no bDone of its own: ApplyDue() calls it once per event,
	// at the tick equal to the event's own apply tick (even when an earlier event of its sequence
	// holds it back, and in save-assembling slots too), with the walk's one bDone load for that
	// event this tick and the tick's one wall reading. It is the only place lateness is counted,
	// and an event counts at most one hitch:
	//  - a token planned after its T+K tick (ApplyTick > RequestTick + K), the T+K-miss rule;
	//  - otherwise, its job not done while the wall time since its request is at least (>=) the sim
	//    time since its request: the device was given the time and did not finish.
	// An event held back only by an overdue earlier one finds its own job done and counts nothing.
	// The test event log's examination fields are written from these same arguments. Game thread.
	void ExamineEvent(const FPendingEvent& E, bool bJobDone, double TickWall)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (TestEventLog.IsValidIndex(E.LogIndex))
		{
			FSuperSLMGpuEventLogEntry& Log = TestEventLog[E.LogIndex];
			Log.bExamined = true;
			Log.bJobDoneAtExamination = bJobDone;
			Log.WallAtExamination = TickWall;
			Log.SimAtExamination = SimSeconds;
			// The unfinished tick jobs PlanAndIssue() enqueued ahead of this event's job. A done job
			// stays done, so the list is pruned of them first; every job still listed is unfinished
			// by this reading.
			TestTickJobs.RemoveAll([](const TSharedPtr<FJob>& J) { return J->bDone.load(std::memory_order_acquire); });
			const uint64 EventSeq = E.Job.IsValid() ? E.Job->Seq : 0;
			int32 Ahead = 0;
			for (const TSharedPtr<FJob>& J : TestTickJobs)
			{
				Ahead += J->Seq < EventSeq ? 1 : 0;
			}
			Log.TickJobsAheadAtExamination = Ahead;
		}
#endif
		if (E.Kind == EEventKind::Token && E.ApplyTick > E.RequestTick + Config.K)
		{
			GameHitches += 1;
			// §5.1's own GPU bookmark text ("a T+K-miss hitch"), matching the CPU backend's
			// "job late by" phrasing (SuperSLMSubsystem.cpp) on the GPU's own T+K vocabulary --
			// R-S2h's own oracle scans a real capture for this literal "token late by" substring.
			TRACE_BOOKMARK(TEXT("SuperSLM: GPU token late by %lld tick(s) (applied at tick %lld, requested at %lld, K %d)"),
				E.ApplyTick - (E.RequestTick + Config.K), E.ApplyTick, E.RequestTick, Config.K);
			return;
		}
		if (!bJobDone)
		{
			const double DeviceSeconds = TickWall - E.RequestWallSeconds;
			const double GrantedSeconds = SimSeconds - E.RequestSimSeconds;
			if (DeviceSeconds >= GrantedSeconds)
			{
				GameHitches += 1;
				TRACE_BOOKMARK(TEXT("SuperSLM: GPU device-lateness hitch (%.2f s device vs %.2f s granted)"), DeviceSeconds, GrantedSeconds);
			}
		}
	}

	// Test access only (GetEventLog()): records E's removal -- applied, or dropped when bDropped --
	// at this tick. A no-op when E has no log entry. Game thread.
	void LogEventRemoved(const FPendingEvent& E, bool bDropped, double WallSeconds)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (TestEventLog.IsValidIndex(E.LogIndex))
		{
			FSuperSLMGpuEventLogEntry& Log = TestEventLog[E.LogIndex];
			Log.RemovedTick = TickIndex;
			Log.bDropped = bDropped;
			Log.RemovedWallSeconds = WallSeconds;
		}
#else
		(void)E;
		(void)bDropped;
		(void)WallSeconds;
#endif
	}

	// Every removal of S's pending events without applying them goes through here: Complete(),
	// FaultSlot(), ClearGeneration(), ReleaseUser() and the by-value resets of the per-user struct.
	// The events are never examined after this and count nothing. Game thread.
	void DropPendingEvents(FGameSlot& S)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (S.Pending.Num() > 0 && TestEventLog.Num() > 0)
		{
			const double Now = FPlatformTime::Seconds();
			for (const FPendingEvent& E : S.Pending)
			{
				LogEventRemoved(E, true, Now);
			}
		}
#endif
		S.Pending.Reset();
	}

	// Ruling 2026-09-26: applies every due event whose job is done, in each sequence's order, and
	// never waits. It takes one wall reading (TickWall) at its start. For each vended slot,
	// save-assembling ones included, it visits the events in order while ApplyTick <= TickIndex and
	// loads each visited event's bDone exactly once this tick; that one load decides both the
	// event's examination (ExamineEvent(), for every visited event whose ApplyTick == TickIndex,
	// including events behind a stop) and whether the walk applies it. It applies only while the
	// loads read done, and only in slots that are not save-assembling, stopping at the first that
	// reads unfinished -- overdue, not applied, never waited for -- so the next tick resumes there,
	// every later event of the sequence applies after it, in order, and never before its own apply
	// tick. It counts no lateness itself. It sets every slot's bHeldThisTick, the flag Plan reads
	// instead of re-reading bDone. Game thread. Each job's bDone is read with acquire, and its
	// Results only after reading bDone true.
	void ApplyDue()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Tick.Apply", SuperSLMGpuChannel);
		const double TickWall = FPlatformTime::Seconds();
		TArray<bool, TInlineAllocator<32>> Loads;
		for (FGameSlot& S : Slots)
		{
			S.bHeldThisTick = false;
			if (!S.bVended)
			{
				continue;
			}
			// The visit: one bDone load per event, and the examinations at this tick.
			Loads.Reset();
			int32 FirstUnfinished = INDEX_NONE;
			for (const FPendingEvent& E : S.Pending)
			{
				if (!E.Job.IsValid() || E.ApplyTick > TickIndex)
				{
					break;
				}
				const bool bJobDone = E.Job->bDone.load(std::memory_order_acquire);
				if (!bJobDone && FirstUnfinished == INDEX_NONE)
				{
					FirstUnfinished = Loads.Num();
				}
				Loads.Add(bJobDone);
				if (E.ApplyTick == TickIndex)
				{
					ExamineEvent(E, bJobDone, TickWall);
				}
			}
			if (S.bSaveAssembling) // row 19: nothing applies between a save's finalize and its resolution
			{
				S.bHeldThisTick = FirstUnfinished != INDEX_NONE;
				continue;
			}
			// The apply, from the same loads: the first Loads.Num() events are exactly the visited
			// ones, and each application removes the front, so the front is always the next visited.
			for (int32 Visited = 0; Visited < Loads.Num(); ++Visited)
			{
				if (!Loads[Visited])
				{
					S.bHeldThisTick = true; // stopped at an unfinished event with ApplyTick <= TickIndex
					break;
				}
				const FPendingEvent E = S.Pending[0];
				S.Pending.RemoveAt(0, 1, EAllowShrinking::No);
				LogEventRemoved(E, false, TickWall);
				ApplyResult(S, E, E.Job->Results[E.ResultIndex]);
				if (S.Phase == ESuperSLMSequencePhase::Complete || S.Phase == ESuperSLMSequencePhase::Faulted)
				{
					break; // Complete()/FaultSlot() dropped the rest
				}
			}
		}
	}

	// Ruling 2026-09-26, back-pressure and the cap: whether Plan would start a new token for S --
	// a one-call prompt or decode token, a composed embed (PlanLayerPos == 0, prompt or decode), or
	// a primed finish-only step. A composed token in flight (PlanLayerPos > 0) is not a boundary and
	// is never held, so back-pressure never moves an in-flight token's T+K. Game-side fields only.
	static bool AtTokenBoundary(const FGameSlot& S)
	{
		return S.Path == ESuperSLMGpuDecodePath::OneCall || S.bPlanPrimed || S.PlanLayerPos == 0;
	}

	// Ruling 2026-09-26 (renamed from NextTickWouldWait(); Tick() no longer waits, so this predicts
	// no block): true when the device, not the tick, is what the next Tick() would be waiting on for
	// progress -- it would find any of
	//  - an overdue event: an event due at that tick (ApplyTick <= TickIndex + 1, the walk
	//    ApplyDue() takes) whose job the submission thread has not finished;
	//  - a vended slot whose admitted op-log front (a reset, save, save assembly or schema bind) has
	//    a job the submission thread has not finished;
	//  - OutstandingTickJobs >= K, so the global cap would bind and Plan would start no new token.
	// A loop that ticks faster than the device asks this and lets real time pass instead of ticking:
	// the stepped self-check, RunGeneration(), the headless query loop and the test access's
	// TickWhenDeviceReady(). Conservative: it does not model ApplyDue()'s early stop at a completed
	// sequence. False when the backend is inactive or already unresponsive, and when the Layer-1
	// call holding the lock has overrun its row-21 bound -- the next tick then takes the device-loss
	// path (Tick()'s CheckLockHolderOverrun()), so ticking is what makes progress. Reads only.
	// Game thread: every bDone with acquire, the holder record under Layer1LockHolderGuard().
	bool NextTickGatedOnDevice() const
	{
		if (!bActive || bUnresponsive.load())
		{
			return false;
		}
		const int64 NextTick = TickIndex + 1;
		bool bGated = OutstandingTickJobs.load() >= Config.K;
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num() && !bGated; ++SlotIndex)
		{
			const FGameSlot& S = Slots[SlotIndex];
			if (!S.bVended)
			{
				continue;
			}
			if (!S.bSaveAssembling)
			{
				for (const FPendingEvent& E : S.Pending)
				{
					if (!E.Job.IsValid() || E.ApplyTick > NextTick)
					{
						break;
					}
					if (!E.Job->bDone.load(std::memory_order_acquire))
					{
						bGated = true;
						break;
					}
				}
			}
			if (!bGated && S.OpLog.IsValidIndex(S.OpFront))
			{
				const FLifecycleOpEntry& Front = S.OpLog[S.OpFront];
				if (Front.bSubmitted && Front.Result == ESuperSLMRestoreResult::Pending && Front.Async.IsValid())
				{
					const TSharedPtr<FJob>& FrontJob = Front.Async->AssembleJob.IsValid() ? Front.Async->AssembleJob : Front.Async->Job;
					bGated = FrontJob.IsValid() && !FrontJob->bDone.load(std::memory_order_acquire);
				}
			}
			if (bGated)
			{
				break;
			}
		}
		if (!bGated)
		{
			return false;
		}
		FLayer1LockHolder Holder;
		{
			FScopeLock HolderLock(&Layer1LockHolderGuard());
			Holder = Layer1LockHolder();
		}
		return !(Holder.bHeld && FPlatformTime::Seconds() - Holder.StartSeconds > Holder.BoundSeconds);
	}

	bool WantsAdapterBind(FGameSlot& S, FAction& A)
	{
		if (!S.bSwapPending)
		{
			return false;
		}
		const FAdapterEntry* Entry = S.RequestedAdapterId != 0 ? Adapters.Find(S.RequestedAdapterId) : nullptr;
		A.bBindAdapter = true;
		A.Adapter = Entry != nullptr ? Entry->Handle : nullptr;
		A.BindTag = S.AwaitedBindTag; // rule 4: every carrier of one request carries its tag
		// Not committed yet (finding 10, T-2885 review): this action can still be dropped on the
		// submission thread (ActionIsLive() false after a generation bump, or SkipStopped() on an
		// already-stopped sequence) without Layer 1 ever seeing the bind. ApplyResult() commits
		// ActiveAdapterId and clears bSwapPending only once a result proves this action actually
		// ran; until then bSwapPending stays set so the next planning pass re-attempts the same,
		// idempotent bind request.
		S.PendingBindAdapterId = Entry != nullptr ? S.RequestedAdapterId : 0;
		S.bPendingBindConfirm = true;
		return true;
	}

	// True while a NON-generation lifecycle op (Reset/Save/Restore-shaped/AdoptPrefix) is the
	// admitted, still-running front of this sequence's own queue -- decode-tick scheduling must
	// not touch Layer 1 for this sequence while that op's own async work is in flight, or the
	// two would submit concurrently against the same sslm_gpu handle. A BeginGeneration entry
	// being the admitted front is NOT blocking here: decode IS that op's own ongoing work.
	bool HasBlockingLifecycleOp(const FGameSlot& S) const
	{
		return S.OpLog.IsValidIndex(S.OpFront) && S.OpLog[S.OpFront].bSubmitted &&
			S.OpLog[S.OpFront].Kind != ELifecycleOpKind::BeginGeneration;
	}

	bool PlanningOpen(const FGameSlot& S) const
	{
		return S.bVended && !HasBlockingLifecycleOp(S) &&
			(S.Phase == ESuperSLMSequencePhase::Prefilling || S.Phase == ESuperSLMSequencePhase::Decoding);
	}

	static bool PromptLeft(const FGameSlot& S)
	{
		return S.PromptPlanned < S.Request.PromptTokens.Num();
	}

	bool HasWork(const FGameSlot& S) const
	{
		return PlanningOpen(S) &&
			(PromptLeft(S) || S.PlanLayerPos > 0 || S.bPlanPrimed || S.TokensIssued < S.Request.MaxNewTokens);
	}

	void AddEvent(FGameSlot& S, EEventKind Kind, const TSharedPtr<FJob>& Job, int32 ResultIndex, int64 RequestTick, double RequestWall, double RequestSim)
	{
		FPendingEvent E;
		E.Kind = Kind;
		E.RequestTick = RequestTick;
		E.RequestWallSeconds = RequestWall;
		E.RequestSimSeconds = RequestSim;
		E.ApplyTick = FMath::Max(RequestTick + Config.K, TickIndex + 1);
		E.Job = Job;
		E.ResultIndex = ResultIndex;
		LogEvent(S, E, false);
		S.Pending.Add(E);
	}

	// Test access only (FSuperSLMGpuTestAccess::GetEventLog()): appends E's raw readings to the
	// event log while the logs are on, and records its index in E. A no-op otherwise. Game thread.
	void LogEvent(const FGameSlot& S, FPendingEvent& E, bool bCarried)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!bTestLogsEnabled)
		{
			return;
		}
		FSuperSLMGpuEventLogEntry Log;
		Log.SequenceId = S.Id;
		Log.bPrefill = E.Kind == EEventKind::Prefill;
		Log.bCarried = bCarried;
		Log.RequestTick = E.RequestTick;
		Log.ApplyTick = E.ApplyTick;
		Log.RequestWallSeconds = E.RequestWallSeconds;
		Log.RequestSimSeconds = E.RequestSimSeconds;
		E.LogIndex = TestEventLog.Add(Log);
#else
		(void)S;
		(void)E;
		(void)bCarried;
#endif
	}

	void AddTokenEvent(FGameSlot& S, const TSharedPtr<FJob>& Job, int32 ResultIndex, int64 RequestTick, double RequestWall, double RequestSim)
	{
		AddEvent(S, EEventKind::Token, Job, ResultIndex, RequestTick, RequestWall, RequestSim);
	}

	// Marks A as feeding prompt token PromptIndex (D-SLM7379). The prompt's first token carries
	// the generation's limits to the thread.
	void FillPrompt(const FGameSlot& S, FAction& A, int32 PromptIndex) const
	{
		A.bPrompt = true;
		A.PromptToken = S.Request.PromptTokens[PromptIndex];
		if (PromptIndex == 0)
		{
			A.bFirstPrompt = true;
			A.MaxNewTokens = S.Request.MaxNewTokens;
			A.StopTokenIds = S.Request.StopTokenIds;
			A.BeginLimit = S.PlanBeginLimit;
		}
	}

	// Plugin paging plan §3.4 "An empty prompt after adopt" (K7): an armed sequence's empty-prompt
	// generation has no first prompt token to carry its limits, so its first (finish-only) step
	// carries them (FAction::bBeginLimits).
	static void TakeBeginLimits(FGameSlot& S, FAction& A)
	{
		if (S.bPlanBeginLimits)
		{
			A.bBeginLimits = true;
			A.MaxNewTokens = S.Request.MaxNewTokens;
			A.StopTokenIds = S.Request.StopTokenIds;
			A.BeginLimit = S.PlanBeginLimit;
			S.bPlanBeginLimits = false;
		}
	}

	// The last prompt token reached full depth in action Index: the sequence is primed, and its
	// completion is the event that moves the phase to Decoding (or carries a prompt fault).
	void PromptCompleted(FGameSlot& S, FAction& A, const TSharedPtr<FJob>& Job, int32 Index, int64 RequestTick, double RequestWall, double RequestSim)
	{
		A.bLastPrompt = true;
		S.bPlanPrimed = true;
		AddEvent(S, EEventKind::Prefill, Job, Index, RequestTick, RequestWall, RequestSim);
	}

	// Plans this tick's work into one job for the submission thread.
	void PlanAndIssue()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Tick.Plan", SuperSLMGpuChannel);
		// _ALWAYS: TCounter::Set() elides a value equal to its cached one, so a constant (K, LayersPerSlice)
		// or one that settled before a capture began would record no sample in that capture. Once per
		// tick; a no-op branch when CountersChannel is off. The same holds for DueTokens and the tick's
		// HitchCount/RetainedResultBytes publish below.
		TRACE_COUNTER_SET_ALWAYS(SuperSLMGpuLayersPerTick, LayersPerTick);
		TRACE_COUNTER_SET_ALWAYS(SuperSLMGpuK, Config.K);
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->bTickJob = true;
#if WITH_DEV_AUTOMATION_TESTS
		// Test access only: the delay is fixed when the job is created (ruling 2026-09-26, item 12).
		Job->PostCallDelaySeconds = TestSubmissionDelaySeconds;
#endif
		const double Now = FPlatformTime::Seconds();

		// Ruling 2026-09-26, the global cap (finding 3): no new token starts, on either path, while
		// K or more tick jobs are outstanding, read once here. Plan enqueues at most one tick job
		// per tick, so K outstanding means a job enqueued at or before tick TickIndex - K is still
		// unfinished and the device is already late on the contract; a device that meets the
		// contract never meets the cap. With back-pressure (bHeldThisTick, set by this tick's
		// ApplyDue()), a sequence is left out of the due sets only at a token boundary
		// (AtTokenBoundary()); a composed token in flight always continues.
		const int32 OutstandingTickJobsAtPlan = OutstandingTickJobs.load();
		const bool bCapBinding = OutstandingTickJobsAtPlan >= Config.K;
		auto IsDue = [this, bCapBinding](const FGameSlot& S, ESuperSLMGpuDecodePath Path)
		{
			return S.Path == Path && HasWork(S) && !(AtTokenBoundary(S) && (S.bHeldThisTick || bCapBinding));
		};
#if WITH_DEV_AUTOMATION_TESTS
		FSuperSLMGpuPlanLogEntry* PlanLog = nullptr;
		if (bTestLogsEnabled)
		{
			// Test access only (GetPlanLog()): one entry per Plan, with the readings it decided on.
			PlanLog = &TestPlanLog.AddDefaulted_GetRef();
			PlanLog->Tick = TickIndex;
			PlanLog->OutstandingTickJobsAtPlan = OutstandingTickJobsAtPlan;
			PlanLog->bCapBinding = bCapBinding;
			for (const FGameSlot& S : Slots)
			{
				if (!S.bVended)
				{
					continue;
				}
				if (S.bHeldThisTick)
				{
					PlanLog->HeldSequenceIds.Add(S.Id);
				}
				if (S.Path == ESuperSLMGpuDecodePath::Composed && S.PlanLayerPos > 0)
				{
					PlanLog->InFlightAtPlanSequenceIds.Add(S.Id);
				}
			}
		}
		auto LogStarted = [PlanLog](int64 Id) { if (PlanLog != nullptr) { PlanLog->StartedSequenceIds.AddUnique(Id); } };
#else
		auto LogStarted = [](int64) {};
#endif

		// OneCall: one whole token per tick -- a prompt token (embed plus every layer, no finish,
		// D-SLM7379) or a decode token -- served to the due one-call sequences in rotation from
		// OneCallCursor: the first due slot index after the cursor, cyclically. A one-call sequence
		// that stays due is therefore served within BlockCount ticks, a hard bound (ruling
		// 2026-09-26, finding 14), whatever other sequences enter or leave the due set. Every
		// one-call step starts a new token, so back-pressure and the cap apply to all of them.
		//
		// Plugin paging plan §3.4 "Prefixes" (K7, UP-11): the build seat takes one more place in this
		// rotation, after every slot (index Slots.Num()). A turn there feeds one whole token of the
		// build seat's head prefix, so a build turn and a one-call token cost the same, the seat is
		// served within BlockCount + 1 ticks, and MinimumK counts it (PrepareGpuState()). The cap
		// holds the seat back like any new token.
		TArray<int32, TInlineAllocator<16>> OneCallDue;
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			if (IsDue(Slots[SlotIndex], ESuperSLMGpuDecodePath::OneCall))
			{
				OneCallDue.Add(SlotIndex);
			}
		}
		const int32 BuildSeatIndex = Slots.Num();
		FGpuPrefixEntry* BuildHead = bCapBinding ? nullptr : BuildSeatHead();
		if (BuildHead != nullptr)
		{
			OneCallDue.Add(BuildSeatIndex);
		}
		int32 OneCallChosen = INDEX_NONE;
		if (OneCallDue.Num() > 0)
		{
			OneCallChosen = OneCallDue[0];
			for (const int32 Candidate : OneCallDue)
			{
				if (Candidate > OneCallCursor)
				{
					OneCallChosen = Candidate;
					break;
				}
			}
			OneCallCursor = OneCallChosen;
		}
		if (OneCallChosen == BuildSeatIndex && BuildHead != nullptr)
		{
			PlanBuildTurn(*BuildHead, Job);
		}
		else if (OneCallChosen != INDEX_NONE)
		{
			const int32 SlotIndex = OneCallChosen;
			FGameSlot& S = Slots[SlotIndex];
			LogStarted(S.Id);
			FAction A;
			A.Slot = SlotIndex;
			A.Gen = S.Gen;
			WantsAdapterBind(S, A);
			if (PromptLeft(S))
			{
				const int32 PromptIndex = S.PromptPlanned++;
				A.Kind = EActionKind::OneCallPromptToken;
				FillPrompt(S, A, PromptIndex);
				const bool bLast = !PromptLeft(S);
				const int32 Index = Job->Actions.Add(MoveTemp(A));
				if (bLast)
				{
					PromptCompleted(S, Job->Actions[Index], Job, Index, TickIndex, Now, SimSeconds);
				}
			}
			else
			{
				A.Kind = EActionKind::OneCallToken;
				TakeBeginLimits(S, A);
				const int32 Index = Job->Actions.Add(MoveTemp(A));
				S.bPlanPrimed = false;
				S.TokensIssued += 1;
				AddTokenEvent(S, Job, Index, TickIndex, Now, SimSeconds);
			}
		}

		// Composed: the tick's layer budget, shared through the batch call's running remainder,
		// with the order rotated from ComposedCursor so no sequence starves (D-SLM7255; ruling
		// 2026-09-26, finding 14). Prompt tokens are sliced exactly like decode tokens, without a
		// finish (D-SLM7379).
		TArray<int32, TInlineAllocator<16>> Due;
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			if (IsDue(Slots[SlotIndex], ESuperSLMGpuDecodePath::Composed))
			{
				Due.Add(SlotIndex);
			}
		}
		// Every sequence this tick's Plan step actually considered issuing work for, across both
		// paths, after back-pressure and the cap removed the held ones at token boundaries -- the
		// per-tick "how many are due" figure §5.1 names (mirrors DeliveredTokens,
		// TRACE_COUNTER_INCREMENT'd once per token actually applied, in ApplyResult()).
		TRACE_COUNTER_SET_ALWAYS(SuperSLMGpuDueTokens, OneCallDue.Num() + Due.Num());
		int32 Remaining = LayersPerTick;
		int32 Rotation = 0;
		while (Rotation < Due.Num() && Due[Rotation] <= ComposedCursor)
		{
			++Rotation; // the first due slot index after the cursor; wraps to 0 when there is none
		}
		if (Rotation == Due.Num())
		{
			Rotation = 0;
		}
		bool bComposedServed = false;
		for (int32 Offset = 0; Offset < Due.Num(); ++Offset)
		{
			const int32 SlotIndex = Due[(Rotation + Offset) % Due.Num()];
			FGameSlot& S = Slots[SlotIndex];
			FAction A;
			A.Slot = SlotIndex;
			A.Gen = S.Gen;
			if (S.bPlanPrimed)
			{
				if (!bComposedServed)
				{
					bComposedServed = true;
					ComposedCursor = SlotIndex;
				}
				LogStarted(S.Id);
				A.Kind = EActionKind::ComposedFinishOnly;
				TakeBeginLimits(S, A);
				const int32 Index = Job->Actions.Add(MoveTemp(A));
				S.bPlanPrimed = false;
				S.TokensIssued += 1;
				AddTokenEvent(S, Job, Index, TickIndex, Now, SimSeconds);
				continue;
			}
			if (S.PlanLayerPos == 0 && !PromptLeft(S) && S.TokensIssued >= S.Request.MaxNewTokens)
			{
				continue;
			}
			const int32 Layers = FMath::Min3(Remaining, NumHiddenLayers - S.PlanLayerPos, LayersPerSliceFor(S));
			if (Layers <= 0)
			{
				continue;
			}
			if (!bComposedServed)
			{
				bComposedServed = true;
				ComposedCursor = SlotIndex;
			}
			A.Kind = EActionKind::ComposedSlice;
			A.Layers = Layers;
			A.bEmbed = S.PlanLayerPos == 0;
			if (A.bEmbed)
			{
				LogStarted(S.Id);
				WantsAdapterBind(S, A);
				S.TokenRequestTick = TickIndex;
				S.TokenRequestWall = Now;
				S.TokenRequestSim = SimSeconds;
				S.bPlanInPrompt = PromptLeft(S);
				if (S.bPlanInPrompt)
				{
					S.PlanPromptIndex = S.PromptPlanned++;
					FillPrompt(S, A, S.PlanPromptIndex);
				}
				else
				{
					S.TokensIssued += 1;
				}
			}
			else
			{
				A.bPrompt = S.bPlanInPrompt;
			}
			S.PlanLayerPos += Layers;
			Remaining -= Layers;
			const bool bTokenDone = S.PlanLayerPos == NumHiddenLayers;
			A.bFinish = bTokenDone && !S.bPlanInPrompt;
			const int32 Index = Job->Actions.Add(MoveTemp(A));
			if (bTokenDone)
			{
				S.PlanLayerPos = 0;
				if (S.bPlanInPrompt)
				{
					S.bPlanInPrompt = false;
					if (S.PlanPromptIndex == S.Request.PromptTokens.Num() - 1)
					{
						PromptCompleted(S, Job->Actions[Index], Job, Index, S.TokenRequestTick, S.TokenRequestWall, S.TokenRequestSim);
					}
				}
				else
				{
					AddTokenEvent(S, Job, Index, S.TokenRequestTick, S.TokenRequestWall, S.TokenRequestSim);
				}
			}
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (PlanLog != nullptr)
		{
			// Test access only (GetPlanLog()): the sequences this tick's Plan issued any action for.
			for (const FAction& Issued : Job->Actions)
			{
				if (Slots.IsValidIndex(Issued.Slot)) // a build turn has no slot (K7)
				{
					PlanLog->IssuedSequenceIds.AddUnique(Slots[Issued.Slot].Id);
				}
			}
		}
#endif
		if (Job->Actions.Num() > 0)
		{
			if (bTestHoldNextTickJob)
			{
				// Test access only (round 2, HoldNextTickJob()): the first tick job enqueued after the
				// request waits on the test-released gate after its call.
				Job->bHoldAfterCall = true;
				bTestHoldNextTickJob = false;
				TestHeldJob = Job; // round 3: HoldNextTickJob() refuses while this is outstanding
			}
			Enqueue(Job);
#if WITH_DEV_AUTOMATION_TESTS
			if (bTestLogsEnabled && !Job->bDone.load(std::memory_order_acquire))
			{
				TestTickJobs.Add(Job); // for TickJobsAheadAtExamination (ExamineEvent())
			}
#endif
		}
	}

	// On a terminal GPU failure (plan §5), which is one of two causes: a device loss confirmed by
	// probe, or a Layer-1 call that did not return within its bound (the unresponsive-thread path,
	// plan §2.5 row 21, which takes no probe). Every live sequence fails with a named outcome whose
	// message says which cause it was, the GPU backend is torn down, and IsGpuBackendActive() reads
	// false, the signal a caller falls back to the CPU backend on. The ABI carries no finer cause.
	void FallBackToCpu()
	{
		// §5.1's own device-lost GPU bookmark (one of the four named bookmarks; a forcing
		// construction per D-SLM7220, out of R-S2h's own testable scope, but real production
		// markup): fires exactly once, when either cause above is confirmed.
		TRACE_BOOKMARK(TEXT("SuperSLM: GPU backend lost (a device loss confirmed by probe, or a GPU call past its bound); the GPU backend is now inactive (USuperSLMQuery re-issues on the CPU backend)"));
		// Plan §2.5 row 21: a Layer-1 call that did not return within the bounded wait takes this
		// same path, and the fault names it.
		const FString Cause = bUnresponsive.load()
			? FString::Printf(TEXT("the GPU submission thread is unresponsive (%s)"), *UnresponsiveReason)
			: FString(TEXT("the GPU device is lost"));
		for (FGameSlot& S : Slots)
		{
			if (!S.bVended || S.Phase == ESuperSLMSequencePhase::Faulted)
			{
				continue;
			}
			if (S.Phase != ESuperSLMSequencePhase::Complete)
			{
				FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::TerminalDeviceLost,
					FString::Printf(TEXT("%s; the GPU backend is torn down and the CPU backend serves new requests"), *Cause));
				continue;
			}
			// D-SLM7948: a Complete sequence keeps its phase and tokens until its reset succeeds, so
			// one whose reset has not resolved is faulted too: that reset will not run. The tokens are
			// kept. This runs before ResolveUnresolvedAfterLoss() resolves the entry (the one caller,
			// BeginTeardownAfterLoss()). A Complete sequence with no reset pending is not faulted: its
			// generation finished.
			bool bResetPending = false;
			for (int32 Index = S.OpFront; Index < S.OpLog.Num() && !bResetPending; ++Index)
			{
				bResetPending = S.OpLog[Index].Kind == ELifecycleOpKind::Reset && S.OpLog[Index].Result == ESuperSLMRestoreResult::Pending;
			}
			if (bResetPending)
			{
				FaultSlot(S, ESuperSLMDecodeOutcome::Generating, ESuperSLMGpuFaultReason::TerminalDeviceLost,
					FString::Printf(TEXT("%s; its queued reset did not run; the GPU backend is torn down and the CPU backend serves new requests"), *Cause));
			}
		}
		UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM GPU backend: %s; the GPU backend is torn down and callers fall back to the CPU backend."),
			bUnresponsive.load() ? *Cause : TEXT("terminal SSLM_DEVICE_LOST confirmed by probe"));
		bActive = false;
	}
};

// ---------------------------------------------------------------------------------------------
// Registry of configured subsystems (schema lookup and the self-check find the GPU mapping of a
// model through it)
// ---------------------------------------------------------------------------------------------

namespace
{
	void RegisterConfigured(USuperSLMGpuSubsystem* Subsystem)
	{
		FScopeLock Lock(&ConfiguredSubsystemsLock());
		ConfiguredSubsystems().AddUnique(Subsystem);
	}

	void UnregisterConfigured(USuperSLMGpuSubsystem* Subsystem)
	{
		FScopeLock Lock(&ConfiguredSubsystemsLock());
		ConfiguredSubsystems().Remove(Subsystem);
	}

	// Plan §2.5 row 1 (D-SLM7662): the plugin's own shader directory, <plugin>/Binaries/Win64/shaders,
	// absolute -- where SuperSLMUnreal.Build.cs stages every compiled .cso and what Configure()
	// passes to Layer 1 as GpuContextConfig::shader_dir. Empty if the plugin is not found.
	FString ResolveShaderDirectory()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		FString Directory = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Binaries") / TEXT("Win64") / TEXT("shaders"));
		FPaths::NormalizeDirectoryName(Directory);
		FPaths::MakePlatformFilename(Directory);
		return Directory;
	}

	// Diagnostic only (plan §9 R-S2j, U1): DXGI's CurrentUsage of the LOCAL segment on the adapter
	// Layer 1's dispatch device holds, in bytes; -1 when it cannot be read. Read-only.
	int64 QueryLayer1LocalVideoMemoryBytes()
	{
		superslm_gpu::harness::Device& Dev = superslm_gpu::harness::GetDevice();
		if (!Dev.available || !Dev.adapter)
		{
			return -1;
		}
		Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter3;
		if (FAILED(Dev.adapter.As(&Adapter3)) || !Adapter3)
		{
			return -1;
		}
		DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
		if (FAILED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
		{
			return -1;
		}
		return static_cast<int64>(Info.CurrentUsage);
	}

	// Every shader the build lists must be present in the directory Layer 1 will be given; with
	// shader_dir set, Layer 1 never looks beside the host executable.
	bool ShaderStagingComplete(const FString& ShaderDirectory, FString& OutMissing)
	{
		TArray<FString> Names;
		FString(TEXT(SUPERSLMUNREAL_GPU_SHADER_NAMES)).ParseIntoArray(Names, TEXT(","), true);
		if (Names.Num() != SUPERSLMUNREAL_GPU_SHADER_COUNT)
		{
			OutMissing = TEXT("the build's shader list is malformed");
			return false;
		}
		TArray<FString> Missing;
		for (const FString& Name : Names)
		{
			if (!FPaths::FileExists(ShaderDirectory / (Name + TEXT(".cso"))))
			{
				Missing.Add(Name);
			}
		}
		if (Missing.Num() > 0)
		{
			OutMissing = FString::Printf(TEXT("%d of %d compiled shaders are missing from the plugin's shader directory %s (for example %s.cso); "
				"the build stages them there, so rebuild the plugin."), Missing.Num(), Names.Num(), *ShaderDirectory, *Missing[0]);
			return false;
		}
		return true;
	}

}

// ---------------------------------------------------------------------------------------------
// USuperSLMGpuSubsystem
// ---------------------------------------------------------------------------------------------

bool USuperSLMGpuSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer);
}

// Review W5: one BeginConfigure()'s hand-off from its pool-thread prepare to whoever claims the
// state it built -- the game-thread continuation, or Deinitialize() when the subsystem goes away
// first (at exit the continuation may never run).
struct FSuperSLMGpuConfigureFlight
{
	FCriticalSection Lock;
	FSuperSLMGpuSubsystemState* Prepared = nullptr; // set by the worker; taken by the first claimant
	FEventRef Done{EEventMode::ManualReset};        // triggered once the worker has set Prepared

	FSuperSLMGpuSubsystemState* Claim()
	{
		FScopeLock ScopeLock(&Lock);
		FSuperSLMGpuSubsystemState* Taken = Prepared;
		Prepared = nullptr;
		return Taken;
	}
};

namespace
{
	void DiscardGpuState(FSuperSLMGpuSubsystemState* S);
}

void USuperSLMGpuSubsystem::Deinitialize()
{
	// TearDown() first, so every continuation still queued finds its serial stale and discards.
	TearDown();
	// Review W5: a prepare still running is waited for (its device jobs wait within the load-time
	// bound, row 21), then its state is discarded here, synchronously, since the pool task that
	// would otherwise discard it may not run at exit.
	for (const TSharedPtr<FSuperSLMGpuConfigureFlight, ESPMode::ThreadSafe>& Flight : InFlightConfigures)
	{
		Flight->Done->Wait();
		DiscardGpuState(Flight->Claim());
	}
	InFlightConfigures.Empty();
	// Plugin paging plan §3.5 (K6): the subsystem is going away, so its tombstones go with it.
	LifecycleTombstones.Empty();
	Super::Deinitialize();
}

void USuperSLMGpuSubsystem::BeginDestroy()
{
	TearDown();
	Super::BeginDestroy();
}

void USuperSLMGpuSubsystem::TearDown()
{
	// A BeginConfigure() still preparing is superseded: its state is discarded when it arrives.
	++ConfigureSerial;
	bConfigurePending = false;
	UnregisterConfigured(this);
	if (State != nullptr)
	{
		// Plugin paging plan §3.5 "When a lifecycle exit ends a wait" (K6), pooled mode only: every
		// lifecycle-op handle still Pending (queued, admitted, or held by HoldApply()) is tombstoned,
		// and reads NotConfigured until the game releases it, rather than Pending for ever. So is a
		// handle a terminal loss already resolved NotConfigured, whose entry goes below. The ledger's
		// epoch ends with the state (every waiter NotConfigured), its fault count is carried over,
		// and every test hold is released. Dedicated mode is unchanged.
		if (State->IsPooled())
		{
			FSuperSLMGpuSubsystemState& S = *State;
			S.ReleaseAllPagedHolds();
			auto Tombstone = [this](const FLifecycleOpEntry& Entry)
			{
				if (Entry.Result == ESuperSLMRestoreResult::Pending && Entry.Kind != ELifecycleOpKind::SchemaBind && Entry.HandleId != 0)
				{
					LifecycleTombstones.Add(Entry.HandleId);
				}
			};
			for (const FGameSlot& Slot : S.Slots)
			{
				for (const FLifecycleOpEntry& Entry : Slot.OpLog)
				{
					Tombstone(Entry);
				}
			}
			for (const FLifecycleOpEntry& Entry : S.RestoreOps)
			{
				Tombstone(Entry);
			}
			for (const TPair<int64, FSuperSLMGpuSubsystemState::FGpuHandleResult>& Pair : S.HandleResults)
			{
				if (Pair.Value.Result == ESuperSLMRestoreResult::NotConfigured)
				{
					LifecycleTombstones.Add(Pair.Key);
				}
			}
			if (S.Ledger.IsConfigured())
			{
				S.Ledger.EndEpoch(SuperSLMPaging::EEpochEnd::Teardown);
			}
			PriorLedgerFaults += static_cast<uint64>(S.Ledger.LedgerFaults());
		}
		// Plan §2.5 row 19 (D-SLM7763): TearDown() releases every lifecycle-op handle entry, and with
		// them every save blob nobody took -- also when the state itself must be leaked (row 21),
		// since the submission thread never reads these game-thread entries.
		State->HandleResults.Empty();
		State->RetainedResultBytes = 0;
		State->Shutdown();
		if (State->bUnresponsive.load())
		{
			// Plan §2.5 row 21: the submission thread may still be inside a Layer-1 call that reads
			// this state, so it is leaked, never deleted under it (Shutdown() logged why).
		}
		else
		{
			delete State;
		}
		State = nullptr;
	}
	ConfiguredModel = nullptr;
}

USuperSLMModel* USuperSLMGpuSubsystem::GetConfiguredModel() const
{
	return State != nullptr ? ConfiguredModel.Get() : nullptr;
}

namespace
{
	// The diagnostics Configure() keeps on the subsystem whatever the outcome, carried out of
	// PrepareGpuState() so it never writes the UObject.
	struct FGpuConfigureDiagnostics
	{
		FString ShaderDirectory;
		int64 PreMapLocalVideoMemoryBytes = -1;
		int64 PostMapLocalVideoMemoryBytes = -1;
	};

	// A prepared state nobody will publish: unwound as Configure()'s own failure path unwinds one.
	void DiscardGpuState(FSuperSLMGpuSubsystemState* S)
	{
		if (S != nullptr)
		{
			S->Shutdown();
			if (!S->bUnresponsive.load())
			{
				delete S; // an unresponsive state is leaked, never deleted under its thread (row 21)
			}
		}
	}
}

// Configure()'s whole body after the teardown (fold-round ruling 1), over captured model bytes
// (SuperSLMRuntime::FModelBytes) rather than the UObject, so it runs on whichever thread calls it:
// the game thread for Configure(), a pool thread for BeginConfigure(). The artifact view load and
// per-layer marshal, the schema parse and the submission thread's creation run on the calling
// thread; the three device jobs (context, model map, pool and warm-up) run on the submission
// thread, and the calling thread waits for them within the load-time bound. ShaderDirectory is
// resolved by the caller on the game thread (the plugin manager). OutState is set only on Success.
// Plugin paging plan §3.9 (K6): PageCountPerturbation and PoolCreateStatusOverride are the Configure
// seams the caller spent for this Configure (0 when unarmed): the first is added to the plugin's
// computed page count before the post-create cross-check, the second is returned in place of the
// sslm_gpu_kv_pool_create call. Both are read in pooled mode only.
static FSuperSLMGpuConfigureReport PrepareGpuState(const SuperSLMRuntime::FModelBytes* Model, const FSuperSLMGpuRuntimeConfig& Config,
	const FString& ResolvedShaderDirectory, int32 PageCountPerturbation, uint32 PoolCreateStatusOverride,
	FGpuConfigureDiagnostics& OutDiag, FSuperSLMGpuSubsystemState*& OutState)
{
	OutState = nullptr;

	FSuperSLMGpuConfigureReport Report;
	auto Fail = [&Report](ESuperSLMGpuConfigureResult Result, const FString& Message)
	{
		Report.Result = Result;
		Report.Message = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU Configure() refused: %s"), *Message);
		return Report;
	};

	if (Model == nullptr || Model->Data == nullptr)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidModel, TEXT("the model is null or carries no artifact bytes"));
	}
	SuperSLMRuntime::FModelShape Shape;
	FString ShapeError;
	if (!SuperSLMRuntime::ReadModelShape(*Model, Shape, ShapeError))
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidModel, ShapeError);
	}
	if (Config.ContextCap <= 0 || Config.ContextCap > Shape.ContextCap)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidContextCap, FString::Printf(
			TEXT("ContextCap %lld must be positive and at most the artifact's own context_cap (%lld)"), Config.ContextCap, Shape.ContextCap));
	}
	if (Config.BlockCount <= 0)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("BlockCount %d must be positive: it is the number of GPU sequences the pool backs"), Config.BlockCount));
	}
	// Plugin paging plan §3.1, UP-15 (K6): SequenceBudget selects the mode and is bounded by the
	// plugin's own cap, ContextCap (Cp), never the model's (Ce): a budget above Cp would admit a holder
	// past every bound the plugin enforces. 0 is dedicated mode, unchanged.
	if (Config.SequenceBudget < 0 || static_cast<int64>(Config.SequenceBudget) > Config.ContextCap)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidSequenceBudget, FString::Printf(
			TEXT("SequenceBudget %d must lie in [0, ContextCap %lld]: 0 is dedicated mode, and a pooled holder's budget is bounded by the plugin's ContextCap"),
			Config.SequenceBudget, Config.ContextCap));
	}
	const bool bPooled = Config.SequenceBudget > 0;
	// UP-4: prefixes need pooled mode, so a dedicated configuration's PrefixBlockCount is ignored,
	// with a warning that names the field (plan §11).
	if (!bPooled && Config.PrefixBlockCount > 0)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU Configure(): PrefixBlockCount %d is ignored in dedicated mode (SequenceBudget 0): GPU prefixes need pooled mode."),
			Config.PrefixBlockCount);
	}
	// D-SLM7799 (plan §5 rule 1): +inf is refused as NaN already was; the negated comparison
	// refuses NaN, zero and negatives, and the finiteness clause refuses +inf. The reason
	// (D-SLM7803, T-3006 N4): the GPU's only use of TickBudgetMs is the hitch comparison
	// Busy > TickBudgetMs, which +inf would silently disable, and the refusal keeps parity with the CPU.
	if (!(Config.TickBudgetMs > 0.0) || !FMath::IsFinite(Config.TickBudgetMs))
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidTickBudget, TEXT("TickBudgetMs must be a finite, positive number of milliseconds: it is the slice budget the hitch comparison measures gpu_busy_ms against, and +inf would disable it (parity with the CPU)"));
	}
	if (Config.MaxQueuedOperationsPerSequence < 1)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidMaxQueuedOperationsPerSequence, FString::Printf(
			TEXT("MaxQueuedOperationsPerSequence %d must be at least 1: a bound below 1 refuses every Reset/Save/BeginGeneration/Adopt-Prefix/Restore request "
				"issued against a sequence, before Layer 1 is ever asked, which makes the backend unusable (D-SLM7475)"), Config.MaxQueuedOperationsPerSequence));
	}
	if (!SuperSLMFinishHook::IsValidTaskCount(Config.FinishParallelTasks))
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidFinishParallelTasks, FString::Printf(
			TEXT("FinishParallelTasks %d must lie in [0, %d]: it is the most blocks one host token finish is split into; 0 or 1 runs it serially"),
			Config.FinishParallelTasks, SuperSLMFinishHook::MaxFinishParallelTasks));
	}

	// Host-side load: the view the GPU mapping is built from, and the facts every budget derives
	// from. Layer 1's own model-map derivation of the per-layer dispatch count (any layer carrying
	// q/k norm gains: 25, otherwise 24) is repeated here through the same marshal call, so the
	// dispatch budget can be validated before any device work (gpu_1p0.cpp sslm_gpu_model_map).
	TUniquePtr<superslm::SslmModelView> View = MakeUnique<superslm::SslmModelView>();
	{
		std::string Err;
		superslm::SslmModelStatus LoadStatus;
		try
		{
			LoadStatus = superslm::SslmModel::Load(static_cast<const uint8_t*>(Model->Data),
				static_cast<size_t>(Model->Size), *View, &Err);
		}
		catch (const std::bad_alloc&)
		{
			return Fail(ESuperSLMGpuConfigureResult::InvalidModel, TEXT("out of memory while loading the artifact view"));
		}
		if (LoadStatus != superslm::SslmModelStatus::Ok)
		{
			return Fail(ESuperSLMGpuConfigureResult::InvalidModel, FString::Printf(TEXT("the artifact view did not load: %s (%s)"),
				ANSI_TO_TCHAR(superslm::SslmModelStatusName(LoadStatus)), UTF8_TO_TCHAR(Err.c_str())));
		}
	}
	const superslm::SslmModelConfig& Cfg = View->config;
	bool bHasQkNorm = false;
	for (uint32 Layer = 0; Layer < Cfg.num_hidden_layers; ++Layer)
	{
		superslm_marshal::LayerBacking Backing;
		superslm::LayerWeights Weights;
		std::string Err;
		bool bMarshalled = false;
		try
		{
			bMarshalled = superslm_marshal::MarshalLayer(*View, Layer, Cfg.num_attention_heads, Cfg.num_key_value_heads, Backing, Weights, &Err);
		}
		catch (const std::bad_alloc&)
		{
			bMarshalled = false;
			Err = "out of memory";
		}
		if (!bMarshalled)
		{
			return Fail(ESuperSLMGpuConfigureResult::InvalidModel, FString::Printf(TEXT("layer %u does not marshal for the GPU: %s"), Layer, UTF8_TO_TCHAR(Err.c_str())));
		}
		bHasQkNorm = bHasQkNorm || Weights.q_norm_gain != nullptr || Weights.k_norm_gain != nullptr;
	}

	const uint32 DispatchesPerLayer = superslm_gpu::DispatchesPerLayer(bHasQkNorm);
	const int32 NumLayers = static_cast<int32>(Cfg.num_hidden_layers);
	const int32 LayersPerSlice = FMath::Min<int32>(static_cast<int32>(Config.DispatchBudget / DispatchesPerLayer), NumLayers);
	if (LayersPerSlice < 1)
	{
		return Fail(ESuperSLMGpuConfigureResult::InvalidDispatchBudget, FString::Printf(
			TEXT("DispatchBudget %u is below one whole layer (%u dispatches per layer for this model)"), Config.DispatchBudget, DispatchesPerLayer));
	}
	// D-SLM7381: K covers the worst concurrency the configuration allows. BlockCount composed
	// sequences share LayersPerTick, so a token can take ceil(BlockCount * L / LayersPerTick)
	// ticks; one-call sequences are served one whole token per tick in rotation, so BlockCount
	// ticks. A sequence chooses its path at vend, so the floor is the larger of the two, which is
	// always the composed one (L >= LayersPerTick).
	// Plugin paging plan §3.4 "Build pace" (K7, UP-11): in pooled mode with PrefixBlockCount > 0 the
	// build seat is one more member of the one-call rotation, so the one-call term is BlockCount + 1.
	const int64 ComposedMinimumK = (static_cast<int64>(Config.BlockCount) * NumLayers + LayersPerSlice - 1) / LayersPerSlice;
	const int64 OneCallMinimumK = static_cast<int64>(Config.BlockCount) + (bPooled && Config.PrefixBlockCount > 0 ? 1 : 0);
	Report.MinimumK = static_cast<int32>(FMath::Max<int64>(ComposedMinimumK, OneCallMinimumK));
	if (Config.K < Report.MinimumK)
	{
		Report.Result = ESuperSLMGpuConfigureResult::KBelowMinimum;
		Report.Message = FString::Printf(TEXT("K %d is below the minimum %d: %d sequences sharing %d layers per tick on a %d-layer model "
			"need ceil(%d x %d / %d) = %d ticks per token (D-SLM7381)"),
			Config.K, Report.MinimumK, Config.BlockCount, LayersPerSlice, NumLayers, Config.BlockCount, NumLayers, LayersPerSlice, Report.MinimumK);
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU Configure() refused: %s"), *Report.Message);
		return Report;
	}
	// D-SLM7803 (plan §5, T-3006 N5): K's upper bound, the CPU's kMaxJobTicks. It closes the F1
	// class for every int32 sum over K (the self-check's tick ceiling is also summed in int64).
	constexpr int32 kMaxJobTicks = 1 << 20;
	if (Config.K > kMaxJobTicks)
	{
		Report.Result = ESuperSLMGpuConfigureResult::KAboveMaximum;
		Report.Message = FString::Printf(TEXT("K %d is above the maximum %d (kMaxJobTicks, 2^20): the scheduler's validated numeric domain"),
			Config.K, kMaxJobTicks);
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU Configure() refused: %s"), *Report.Message);
		return Report;
	}
	// Plan §2.5 row 1: the plugin's own shader directory, checked here and passed to Layer 1 below.
	const FString ShaderDirectory = ResolvedShaderDirectory;
	OutDiag.ShaderDirectory = ShaderDirectory; // R-S2i/R-S2j diagnostic, kept whatever the outcome
	if (ShaderDirectory.IsEmpty())
	{
		return Fail(ESuperSLMGpuConfigureResult::ShaderStagingIncomplete, TEXT("the SuperSLMUnreal plugin is not registered with the plugin manager, so its shader directory cannot be resolved"));
	}
	FString Missing;
	if (!ShaderStagingComplete(ShaderDirectory, Missing))
	{
		return Fail(ESuperSLMGpuConfigureResult::ShaderStagingIncomplete, Missing);
	}

	FSuperSLMGpuSubsystemState* S = new FSuperSLMGpuSubsystemState();
	S->Config = Config;
	S->NumHiddenLayers = NumLayers;
	S->VocabSize = static_cast<int32>(Cfg.vocab_size);
	S->ModelContextCap = Shape.ContextCap;
	S->DispatchesPerLayer = DispatchesPerLayer;
	S->LayersPerTick = LayersPerSlice;
	S->MinimumK = Report.MinimumK;
	S->ConfiguredLayersPerTick = LayersPerSlice;
	S->ConfiguredDispatchBudget = Config.DispatchBudget;
	S->ConfiguredMinimumK = Report.MinimumK;
	S->ConfiguredK = Config.K;
	SuperSLMRuntime::ReadArtifactHash(*Model, S->ArtifactHash);

	// The dead-end check's own copy of the SchemaMasks section (the view is freed below).
	if (const superslm::SslmSectionView* Section = View->Section(superslm::SslmSectionType::SchemaMasks))
	{
		S->SchemaSectionBytes.assign(Section->data, Section->data + Section->byte_size);
		std::string Err;
		if (!superslm::SchemaMasksTable::Parse(S->SchemaSectionBytes.data(), S->SchemaSectionBytes.size(), Cfg.vocab_size, S->Schemas, &Err))
		{
			delete S;
			return Fail(ESuperSLMGpuConfigureResult::InvalidModel, FString::Printf(TEXT("the SchemaMasks section does not parse: %s"), UTF8_TO_TCHAR(Err.c_str())));
		}
	}

	// Declared residency (plan §2.5 row 3, D-SLM7679 as corrected by D-SLM7683 and D-SLM7684): the
	// device-local (DEFAULT-heap) buffers Layer 1 allocates for the model, the head and each pooled
	// sequence -- a lower bound on what the driver occupies, never a ceiling. The base is the
	// packed layer weights at the exact layout the model map uploads, the two RoPE tables (each
	// its tensor's element count x 8 bytes from the artifact's rope_tables manifest, or Layer 1's
	// 8-byte placeholder when a table is absent, src/gpu/gpu_1p0.cpp sslm_gpu_model_map), and the
	// SCM1 mask pages. The head term is added only after a head-on map succeeds (below).
	const uint32 KvWidth = Cfg.num_key_value_heads * Cfg.head_dim;
	const superslm_gpu::GpuLayerLayout Layout = superslm_gpu::ComputeLayerLayout(
		static_cast<uint32>(Cfg.hidden_size), KvWidth, Cfg.num_key_value_heads, Cfg.num_attention_heads,
		static_cast<uint32>(Cfg.intermediate_size), Cfg.num_attention_heads * Cfg.head_dim);
	const superslm::SslmTensorView* RopeCos = View->rope_tables.Tensor("cos");
	const superslm::SslmTensorView* RopeSin = View->rope_tables.Tensor("sin");
	const int64 RopeBytes = (RopeCos != nullptr ? static_cast<int64>(RopeCos->elem_count) * 8 : 8)
		+ (RopeSin != nullptr ? static_cast<int64>(RopeSin->elem_count) * 8 : 8);
	S->DeclaredModelBytes = static_cast<int64>(Layout.stride) * NumLayers + RopeBytes + static_cast<int64>(S->SchemaSectionBytes.size());
	const int64 KvBytesPerSequence = static_cast<int64>(NumLayers) * Config.ContextCap * Cfg.num_key_value_heads * Cfg.head_dim * 2;
	S->KvBytesPerSequence = KvBytesPerSequence;
	// The device-local head buffers (src/gpu/gpu_1p0.cpp CreateDeviceLogitsBuffers): the head table
	// (V x H, padded to a multiple of four bytes), the H x 4 input row and the V x 8 output row.
	// The UPLOAD input row, the READBACK output row and the transient staging copy live outside
	// device-local memory and are not declared.
	const int64 HeadTableBytes = static_cast<int64>(Cfg.vocab_size) * static_cast<int64>(Cfg.hidden_size);
	const int64 DeclaredHeadBytes = FMath::Max<int64>(4, (HeadTableBytes + 3) & ~static_cast<int64>(3))
		+ static_cast<int64>(Cfg.hidden_size) * 4 + static_cast<int64>(Cfg.vocab_size) * 8;
	S->ShaderDirectory = ShaderDirectory;

	// Plugin paging plan §3.1-§3.4, UP-15 (K6): pooled mode's page geometry, ledger and pool size,
	// all host arithmetic, decided before any device work. The geometry is the engine's own
	// (ComputeGpuPageGeometry over the model's fields at the model's cap Ce, exactly as
	// sslm_gpu_kv_pool_create computes it), so B and the page bytes are never a plugin constant; the
	// pool's stats cross-check it after the create (job 3). The ledger prices every charge at Ce and
	// bounds every admission at the plugin's ContextCap (Cp), and sizes the regions with the two
	// block sizes P_blk and H_blk. The pool is S + P + H + CacheRegionPages() (0: no GPU cache).
	S->bPooled = bPooled;
	int64 WarmLimit = 0;
	if (bPooled)
	{
		const int64 EngineCap = static_cast<int64>(Cfg.context_cap);
		S->PageGeometry = superslm_gpu::kv_pool::ComputeGpuPageGeometry(Cfg.num_hidden_layers, Cfg.num_key_value_heads, Cfg.head_dim,
			static_cast<int64_t>(EngineCap), Cfg.kv_block_size);
		if (!S->PageGeometry.ok)
		{
			delete S;
			return Fail(ESuperSLMGpuConfigureResult::KvPoolRefused, FString::Printf(
				TEXT("the model's page geometry is not computable (%u layers, %u K/V heads, head_dim %u, context_cap %lld, kv_block_size %u): no pool of 0 pages (0 bytes) can be made"),
				Cfg.num_hidden_layers, Cfg.num_key_value_heads, Cfg.head_dim, EngineCap, Cfg.kv_block_size));
		}
		SuperSLMPaging::FPageLedgerConfig LedgerConfig;
		LedgerConfig.PagePositions = S->PageGeometry.page_positions;
		LedgerConfig.PageBytes = static_cast<uint64_t>(S->PageGeometry.page_bytes);
		LedgerConfig.ContextCap = Config.ContextCap;
		LedgerConfig.EngineContextCap = EngineCap;
		LedgerConfig.SequenceBudget = Config.SequenceBudget;
		LedgerConfig.BlockCount = Config.BlockCount;
		LedgerConfig.PrefixBlockCount = Config.PrefixBlockCount;
		LedgerConfig.RestoreHeadroomBlocks = Config.RestoreHeadroomBlocks;
		LedgerConfig.CacheGhostBlocks = Config.CacheGhostBlocks;
		LedgerConfig.bCacheConfigured = false;
		LedgerConfig.CacheRegionPages = FSuperSLMGpuSubsystemState::CacheRegionPages();
		const SuperSLMPaging::ELedgerConfigure LedgerResult = S->Ledger.Configure(LedgerConfig);
		FString LedgerRefusal;
		ESuperSLMGpuConfigureResult LedgerRefusalResult = ESuperSLMGpuConfigureResult::KvPoolRefused;
		switch (LedgerResult)
		{
			case SuperSLMPaging::ELedgerConfigure::Ok:
				break;
			case SuperSLMPaging::ELedgerConfigure::LegacyMode:
			case SuperSLMPaging::ELedgerConfigure::InvalidSequenceBudget:
				LedgerRefusalResult = ESuperSLMGpuConfigureResult::InvalidSequenceBudget;
				LedgerRefusal = FString::Printf(TEXT("the page ledger refused SequenceBudget %d against ContextCap %lld"), Config.SequenceBudget, Config.ContextCap);
				break;
			case SuperSLMPaging::ELedgerConfigure::InvalidGeometry:
				LedgerRefusal = FString::Printf(TEXT("the page ledger refused the pool's geometry: BlockCount %d, PrefixBlockCount %d, RestoreHeadroomBlocks %d and CacheGhostBlocks %d "
					"at %lld positions per page (%llu bytes) must be non-negative and size a pool of at most 4,294,967,295 pages"),
					Config.BlockCount, Config.PrefixBlockCount, Config.RestoreHeadroomBlocks, Config.CacheGhostBlocks,
					static_cast<int64>(S->PageGeometry.page_positions), static_cast<uint64>(S->PageGeometry.page_bytes));
				break;
		}
		if (!LedgerRefusal.IsEmpty())
		{
			delete S;
			return Fail(LedgerRefusalResult, LedgerRefusal);
		}
		S->PoolPageCount = S->Ledger.PoolPages();
		S->PoolBytes = static_cast<int64>(S->PoolPageCount) * static_cast<int64>(S->PageGeometry.page_bytes);
		WarmLimit = S->Ledger.SlotLimit(0);
	}

	S->Slots.SetNum(Config.BlockCount);
	// Each sequence's op log, sized for its queued window plus a running generation's kept entry
	// (CompactSlotOpLog()), so between generations a request does not grow it. A long generation
	// with requests resolved behind it can outgrow this reserve (see CompactSlotOpLog()).
	for (auto& Slot : S->Slots)
	{
		Slot.OpLog.Reserve(FMath::Max(Config.MaxQueuedOperationsPerSequence, 1) + 2);
	}
	S->ThreadSlots.SetNum(Config.BlockCount);
	S->LiveGen = MakeUnique<std::atomic<uint32>[]>(Config.BlockCount);
	for (int32 I = 0; I < Config.BlockCount; ++I)
	{
		S->LiveGen[I].store(0);
	}

	if (!S->StartThread())
	{
		S->StopThread();
		delete S;
		return Fail(ESuperSLMGpuConfigureResult::DeviceUnavailable, TEXT("the GPU submission thread could not be created"));
	}

	// The device work runs as three submission-thread jobs whose outputs live in shared state, so a
	// job that outlives the bounded wait (plan §2.5 row 21) never writes into this stack frame; on
	// such a timeout the state and the model view it reads are leaked, never freed under it.
	struct FDeviceOutcome
	{
		ESuperSLMGpuConfigureResult Result = ESuperSLMGpuConfigureResult::Success;
		FString Message;
		bool bHeadActive = false;
		FString HeadStatus;
		int64 PreMapLocalVideoMemoryBytes = -1;  // R-S2j, diagnostic only
		int64 PostMapLocalVideoMemoryBytes = -1;
	};
	const TSharedRef<FDeviceOutcome, ESPMode::ThreadSafe> Outcome = MakeShared<FDeviceOutcome, ESPMode::ThreadSafe>();
	superslm::SslmModelView* ViewPtr = View.Get();
	const std::string ShaderDirUtf8(TCHAR_TO_UTF8(*ShaderDirectory)); // copied into the job, alive across the call
	const bool bHeadRequested = Model->bGpuDeviceResidentHead;
	const int32 FinishParallelTasks = Config.FinishParallelTasks;

	// 1. The context, created with the plugin's own shader directory (row 1), then the host
	//    parallel-for finish hook installed on it right after creation (row 2).
	bool bJobsFinished = S->RunSync([S, Outcome, ShaderDirUtf8, FinishParallelTasks]()
	{
		GpuContextConfig ContextConfig{};
		ContextConfig.reserved = 0;
		ContextConfig.shader_dir = ShaderDirUtf8.c_str();
		FGpuStatus St = sslm_gpu_context_create(ContextConfig, &S->Ctx);
		S->NoteLayer1Status(St); // K6 review S2: every submission-thread status is counted
		if (St != FGpuStatus::SSLM_OK || S->Ctx == nullptr)
		{
			Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
			Outcome->Message = (St == FGpuStatus::SSLM_GPU_SHADER_DIR_INVALID || St == FGpuStatus::SSLM_GPU_SHADER_DIR_CONFLICT)
				? FString::Printf(TEXT("sslm_gpu_context_create refused the plugin's shader directory %s (%s)"), UTF8_TO_TCHAR(ShaderDirUtf8.c_str()), *GpuStatusText(St))
				: (St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED)
					? FString::Printf(TEXT("sslm_gpu_context_create returned %s (the device could not be set up for lack of memory; a later Configure() may succeed)"), *GpuStatusText(St))
					: FString::Printf(TEXT("sslm_gpu_context_create returned %s (no D3D12 hardware compute adapter, or SSLM_GPU_ADAPTER_INDEX names none)"), *GpuStatusText(St));
			return;
		}
		if (SuperSLMFinishHook::ShouldInstall(FinishParallelTasks))
		{
			const sslm_parallel_for Hook = SuperSLMFinishHook::Make(FinishParallelTasks, 0);
			St = sslm_gpu_context_set_host_parallel_for(S->Ctx, &Hook);
			S->NoteLayer1Status(St);
			if (St != FGpuStatus::SSLM_OK)
			{
				Outcome->Result = ESuperSLMGpuConfigureResult::InvalidFinishParallelTasks;
				Outcome->Message = FString::Printf(TEXT("sslm_gpu_context_set_host_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"),
					FinishParallelTasks, *GpuStatusText(St));
			}
		}
	}, TEXT("load-time call (context create)"), LoadTimeBoundSeconds(S->DeclaredModelBytes), S->DeclaredModelBytes); // row 21, D-SLM7762

	// 2. The model, mapped with the device-resident head unless the asset turns it off (row 3). An
	//    out-of-memory refusal of the head's buffers leaves the context usable and the map
	//    retryable without the flag: the map is retried once, on the same context, and the head is
	//    reported inactive with the reason -- never a failed backend, never a silent switch.
	if (bJobsFinished && Outcome->Result == ESuperSLMGpuConfigureResult::Success)
	{
		// Row 21 (D-SLM7762, D-SLM7764): a load-time bound from the call's worst-case bytes. With
		// the head requested, one job may run two maps -- the head-on map, then D-SLM7664's head-off
		// retry on an out-of-memory refusal -- so its bytes are head-on plus base, bounded once
		// (the floor applies to the whole call, matching row 21's table).
		const double ModelMapBoundSeconds = bHeadRequested
			? LoadTimeBoundSeconds((S->DeclaredModelBytes + DeclaredHeadBytes) + S->DeclaredModelBytes)
			: LoadTimeBoundSeconds(S->DeclaredModelBytes);
		bJobsFinished = S->RunSync([S, Outcome, ViewPtr, bHeadRequested]()
		{
			GpuResidencyConfig Residency{};
			Residency.flags = bHeadRequested ? SSLM_GPU_RESIDENCY_HEAD_ON_DEVICE : 0u;
			// R-S2j (diagnostic only): the LOCAL-segment reading immediately before the map and
			// immediately after the map that succeeded, on this thread, with nothing else submitted
			// between them by this subsystem.
			Outcome->PreMapLocalVideoMemoryBytes = QueryLayer1LocalVideoMemoryBytes();
			FGpuStatus St = sslm_gpu_model_map(S->Ctx, ViewPtr, Residency, &S->Model);
			S->NoteLayer1Status(St);
			if (bHeadRequested && St == FGpuStatus::SSLM_OK)
			{
				Outcome->bHeadActive = true;
				Outcome->HeadStatus = TEXT("on the device");
			}
			else if (bHeadRequested && St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED)
			{
				Residency.flags = 0u;
				Outcome->PreMapLocalVideoMemoryBytes = QueryLayer1LocalVideoMemoryBytes(); // before the map that succeeds
				St = sslm_gpu_model_map(S->Ctx, ViewPtr, Residency, &S->Model);
				S->NoteLayer1Status(St);
				Outcome->HeadStatus = TEXT("on the host: the model map with the device-resident head ran out of memory (SSLM_GPU_ALLOCATION_FAILED), so the model was mapped once more without it");
			}
			else if (!bHeadRequested)
			{
				Outcome->HeadStatus = TEXT("on the host: the model asset turns the device-resident head off (bGpuDeviceResidentHead)");
			}
			if (St != FGpuStatus::SSLM_OK || S->Model == nullptr)
			{
				Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
				Outcome->Message = FString::Printf(TEXT("sslm_gpu_model_map returned %s"), *GpuStatusText(St));
				return;
			}
			Outcome->PostMapLocalVideoMemoryBytes = QueryLayer1LocalVideoMemoryBytes();
		}, TEXT("load-time call (model map)"), ModelMapBoundSeconds,
			S->DeclaredModelBytes + (bHeadRequested ? DeclaredHeadBytes : 0)); // row 21, D-SLM7762
	}

	// 3. The sequence pool, the warm-up token, and the adapter for the live video-memory reading.
	//    Pooled mode (plugin paging plan §3.4, K6): the warm-up's dedicated scratch sequence is made
	//    and released first, then the page pool is created and cross-checked against the computed
	//    geometry, then the warm sequences are drawn from it with sslm_gpu_seq_create_budgeted.
	//    Dedicated mode is unchanged: its warm sequences, then the warm-up.
	if (bJobsFinished && Outcome->Result == ESuperSLMGpuConfigureResult::Success)
	{
		bJobsFinished = S->RunSync([S, Outcome, KvBytesPerSequence, PageCountPerturbation, PoolCreateStatusOverride, WarmLimit]()
		{
			// Warm-up: one token embedded and driven to full depth on a scratch sequence builds
			// every pipeline state object the decode path uses, so the first gameplay tick does not
			// pay for shader loading. It is also the configure-time proof that the staged shaders
			// load from the directory Layer 1 was given.
			auto RunWarmUp = [S]() -> FGpuStatus
			{
				SslmGpuSequenceHandle* Scratch = nullptr;
				FGpuStatus WarmSt = sslm_gpu_seq_create(S->Ctx, S->Model, 2, &Scratch);
				S->NoteLayer1Status(WarmSt);
				if (WarmSt == FGpuStatus::SSLM_OK && Scratch != nullptr)
				{
					WarmSt = sslm_gpu_seq_embed_token(S->Ctx, Scratch, 0);
					S->NoteLayer1Status(WarmSt);
					if (WarmSt == FGpuStatus::SSLM_OK)
					{
						WarmSt = sslm_decode_step_gpu(S->Ctx, Scratch, nullptr, S->FullTokenBudget());
						S->NoteLayer1Status(WarmSt);
					}
					if (WarmSt == FGpuStatus::SSLM_OK)
					{
						int32_t Ready = 0;
						FGpuStatus Drained = FGpuStatus::SSLM_OK;
						WarmSt = sslm_gpu_ready(S->Ctx, Scratch, 1, &Ready, &Drained);
						S->NoteLayer1Status(WarmSt);
						if (WarmSt == FGpuStatus::SSLM_OK)
						{
							WarmSt = Drained;
							S->NoteLayer1Status(WarmSt);
						}
					}
					S->NoteLayer1Status(sslm_gpu_seq_release(S->Ctx, Scratch));
				}
				return WarmSt;
			};

			FGpuStatus St = FGpuStatus::SSLM_OK;
			if (!S->bPooled)
			{
				for (int32 I = 0; I < S->ThreadSlots.Num(); ++I)
				{
					FThreadSlot& TS = S->ThreadSlots[I];
					St = sslm_gpu_seq_create(S->Ctx, S->Model, S->Config.ContextCap, &TS.Handle);
					S->NoteLayer1Status(St);
					if (St != FGpuStatus::SSLM_OK || TS.Handle == nullptr)
					{
						Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
						Outcome->Message = FString::Printf(TEXT("sslm_gpu_seq_create (pool sequence %d of %d, context_cap %lld) returned %s"),
							I + 1, S->ThreadSlots.Num(), S->Config.ContextCap, *GpuStatusText(St));
						return;
					}
					TS.Cap = S->Config.ContextCap;
					TS.KvBytes = KvBytesPerSequence;
				}
				St = RunWarmUp();
				if (St != FGpuStatus::SSLM_OK)
				{
					Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
					Outcome->Message = FString::Printf(TEXT("the warm-up token returned %s"), *GpuStatusText(St));
					return;
				}
			}
			else
			{
				St = RunWarmUp();
				if (St != FGpuStatus::SSLM_OK)
				{
					Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
					Outcome->Message = FString::Printf(TEXT("the warm-up token returned %s"), *GpuStatusText(St));
					return;
				}

				// The pool (§3.4, cell 5). KV_POOL_TOO_LARGE and ALLOCATION_FAILED are KvPoolRefused,
				// naming the pages, the bytes and the status; a smaller pool may be tried at once.
				const uint32 Pages = S->PoolPageCount;
				const uint64 PageBytes = static_cast<uint64>(S->PageGeometry.page_bytes);
				const uint64 Bytes = static_cast<uint64>(Pages) * PageBytes;
				St = PoolCreateStatusOverride != 0u
					? static_cast<FGpuStatus>(PoolCreateStatusOverride) // the §3.9 seam, in place of the call
					: sslm_gpu_kv_pool_create(S->Ctx, S->Model, Pages, &S->Pool);
				S->NoteLayer1Status(St);
				if (St != FGpuStatus::SSLM_OK || S->Pool == nullptr)
				{
					const bool bPoolRefusal = St == FGpuStatus::SSLM_GPU_KV_POOL_TOO_LARGE || St == FGpuStatus::SSLM_GPU_ALLOCATION_FAILED;
					Outcome->Result = bPoolRefusal ? ESuperSLMGpuConfigureResult::KvPoolRefused : ESuperSLMGpuConfigureResult::DeviceUnavailable;
					Outcome->Message = FString::Printf(TEXT("sslm_gpu_kv_pool_create refused a pool of %u pages (%llu bytes: %llu bytes per page, %lld positions per page; "
						"S %u + P %u + H %u + C %u pages), returning %s%s"),
						Pages, Bytes, PageBytes, static_cast<int64>(S->PageGeometry.page_positions),
						S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::S), S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::P),
						S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::H), FSuperSLMGpuSubsystemState::CacheRegionPages(),
						*GpuStatusText(St),
						bPoolRefusal ? TEXT("; reduce BlockCount, SequenceBudget, PrefixBlockCount or RestoreHeadroomBlocks") : TEXT(""));
					if (S->Pool != nullptr)
					{
						S->NoteLayer1Status(sslm_gpu_kv_pool_destroy(S->Ctx, S->Pool));
						S->Pool = nullptr;
					}
					return;
				}

				// The post-create cross-check (§3.4, cell 5; coverage audit G6): the pool's own page
				// size and page count must be the ones the plugin computed (with the §3.9 seam's
				// perturbation applied to the computed count). Otherwise the pool is destroyed and
				// Configure is refused, naming both figures.
				SslmGpuKvPoolStats PoolStats = {};
				PoolStats.struct_size = static_cast<uint32_t>(sizeof(PoolStats));
				St = sslm_gpu_kv_pool_stats(S->Pool, &PoolStats);
				S->NoteLayer1Status(St); // K6 review S2
				const int64 ComputedPages = static_cast<int64>(Pages) + static_cast<int64>(PageCountPerturbation);
				const int64 ComputedPositions = static_cast<int64>(S->PageGeometry.page_positions);
				if (St != FGpuStatus::SSLM_OK || static_cast<int64>(PoolStats.page_count) != ComputedPages ||
					static_cast<int64>(PoolStats.page_positions) != ComputedPositions)
				{
					const FGpuStatus DestroySt = sslm_gpu_kv_pool_destroy(S->Ctx, S->Pool);
					S->NoteLayer1Status(DestroySt);
					S->Pool = nullptr;
					Outcome->Result = ESuperSLMGpuConfigureResult::KvPoolRefused;
					Outcome->Message = FString::Printf(TEXT("the page pool disagrees with the plugin's geometry: sslm_gpu_kv_pool_stats returned %s with %u pages of %lld positions, "
						"and the plugin computed %lld pages of %lld positions (%llu bytes per page, %llu bytes) from the model's geometry; the pool was destroyed (%s)"),
						*GpuStatusText(St), PoolStats.page_count, static_cast<int64>(PoolStats.page_positions), ComputedPages, ComputedPositions,
						PageBytes, Bytes, *GpuStatusText(DestroySt));
					return;
				}
				S->LastPoolStats = PoolStats;
				S->bHasPoolSnapshot = true;

				// The warm sequences: budgeted holders, R(SequenceBudget) pages each, drawn from the
				// pool (region S). Each is the plugin's {Budget, SequenceBudget, 0} holder, whose limit
				// is min(SequenceBudget, ContextCap); the engine's own cap is the model's.
				for (int32 I = 0; I < S->ThreadSlots.Num(); ++I)
				{
					FThreadSlot& TS = S->ThreadSlots[I];
					St = sslm_gpu_seq_create_budgeted(S->Ctx, S->Model, S->Pool, S->Config.SequenceBudget, &TS.Handle);
					S->NoteLayer1Status(St);
					if (St != FGpuStatus::SSLM_OK || TS.Handle == nullptr)
					{
						Outcome->Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
						Outcome->Message = FString::Printf(TEXT("sslm_gpu_seq_create_budgeted (pool sequence %d of %d, SequenceBudget %d, %u pages) returned %s"),
							I + 1, S->ThreadSlots.Num(), S->Config.SequenceBudget, Pages, *GpuStatusText(St));
						return;
					}
					TS.Cap = S->Config.ContextCap; // Cp: the plugin's cap bounds every holder (UP-15)
					TS.KvBytes = 0;                // a pooled holder owns no K/V buffer: the pool is declared once
					TS.Limit = WarmLimit;
				}
			}

			// The adapter Layer 1 dispatches on, for the live video-memory reading (D-SLM7335).
			superslm_gpu::harness::Device& Dev = superslm_gpu::harness::GetDevice();
			if (Dev.available && Dev.adapter)
			{
				Dev.adapter.As(&S->Adapter3);
			}
		});
	}

	if (bJobsFinished)
	{
		// R-S2j (diagnostic only): the samples taken around the model map, kept whatever the outcome.
		OutDiag.PreMapLocalVideoMemoryBytes = Outcome->PreMapLocalVideoMemoryBytes;
		OutDiag.PostMapLocalVideoMemoryBytes = Outcome->PostMapLocalVideoMemoryBytes;
	}
	if (!bJobsFinished)
	{
		// Row 21: a device call did not return within the bounded wait. The state and the model
		// view a job may still be reading are leaked, never freed under it.
		const FString Why = S->UnresponsiveReason;
		S->Shutdown();
		(void)View.Release(); // leaked deliberately (row 21)
		return Fail(ESuperSLMGpuConfigureResult::DeviceUnavailable, FString::Printf(TEXT("the GPU device did not respond: %s"), *Why));
	}
	if (Outcome->Result != ESuperSLMGpuConfigureResult::Success)
	{
		S->Shutdown();
		delete S;
		return Fail(Outcome->Result, Outcome->Message);
	}
	View.Reset(); // the model handle keeps its own copies of everything it reads (gpu_1p0.cpp)

	// Row 3: the head term is declared only after the head-on map succeeded (D-SLM7681); after the
	// out-of-memory fallback no head term is added.
	S->bDeviceHeadActive = Outcome->bHeadActive;
	S->DeviceHeadStatus = Outcome->HeadStatus;
	if (S->bDeviceHeadActive)
	{
		S->DeclaredModelBytes += DeclaredHeadBytes;
	}
	else if (bHeadRequested)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("GPU Configure(): the device-resident head is %s."), *S->DeviceHeadStatus);
	}

	S->DispatchCount = 0;
	S->ThreadHitches = 0;
	S->bActive = true;
	OutState = S;

	Report.Result = ESuperSLMGpuConfigureResult::Success;
	Report.Message = FString::Printf(TEXT("GPU backend configured: %d layers, %u dispatches per layer, %d layers per tick, K %d (minimum %d), "
		"%d sequences at context_cap %lld; logits head %s; host finish %s; shaders from %s"),
		NumLayers, DispatchesPerLayer, LayersPerSlice, Config.K, Report.MinimumK, Config.BlockCount, Config.ContextCap,
		*S->DeviceHeadStatus,
		SuperSLMFinishHook::ShouldInstall(Config.FinishParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Config.FinishParallelTasks) : TEXT("serial"),
		*S->ShaderDirectory);
	if (S->bPooled)
	{
		// Plugin paging plan §3.2 (K6): the pooled shape, in the ledger's own figures.
		Report.Message += FString::Printf(TEXT("; pooled K/V: SequenceBudget %d, a pool of %u pages of %lld positions (%lld bytes, and a host mirror of the same size): "
			"S %u, P %u, H %u, C %u"),
			Config.SequenceBudget, S->PoolPageCount, static_cast<int64>(S->PageGeometry.page_positions), S->PoolBytes,
			S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::S), S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::P),
			S->Ledger.RegionCapacity(SuperSLMPaging::ERegion::H), FSuperSLMGpuSubsystemState::CacheRegionPages());
	}
	UE_LOG(LogSuperSLM, Log, TEXT("%s"), *Report.Message);
	return Report;
}

FSuperSLMGpuConfigureReport USuperSLMGpuSubsystem::Configure(USuperSLMModel* Model, const FSuperSLMGpuRuntimeConfig& Config)
{
	TearDown();
	LastConfigureShaderDirectory.Reset();
	LastPreMapLocalVideoMemoryBytes = -1;
	LastPostMapLocalVideoMemoryBytes = -1;

	// Game thread, blocking: PrepareGpuState() runs on this thread and waits here for the device
	// jobs. BeginConfigure() is the same work with PrepareGpuState() on a pool thread.
	SuperSLMRuntime::FModelBytes Bytes;
	if (Model != nullptr)
	{
		Bytes = SuperSLMRuntime::CaptureModelBytes(*Model);
	}
	FGpuConfigureDiagnostics Diag;
	FSuperSLMGpuSubsystemState* S = nullptr;
	// Plugin paging plan §3.9 (K6): the Configure seams arm the next Configure(), which spends them.
	const int32 PageCountPerturbation = NextConfigurePageCountPerturbation;
	const uint32 PoolCreateStatusOverride = NextPoolCreateStatusOverride;
	NextConfigurePageCountPerturbation = 0;
	NextPoolCreateStatusOverride = 0;
	const FSuperSLMGpuConfigureReport Report = PrepareGpuState(Model != nullptr ? &Bytes : nullptr, Config, ResolveShaderDirectory(),
		PageCountPerturbation, PoolCreateStatusOverride, Diag, S);
	LastConfigureShaderDirectory = Diag.ShaderDirectory;
	LastPreMapLocalVideoMemoryBytes = Diag.PreMapLocalVideoMemoryBytes;
	LastPostMapLocalVideoMemoryBytes = Diag.PostMapLocalVideoMemoryBytes;
	if (S != nullptr)
	{
		// Plugin paging plan §3.5 "Handle ids stay unique" (K6): the new state's handle counter starts
		// above every tombstoned id.
		S->NextLifecycleOpHandleId = FirstHandleIdAfterTombstones();
		State = S;
		ConfiguredModel = Model;
		RegisterConfigured(this);
	}
	return Report;
}

void USuperSLMGpuSubsystem::BeginConfigure(USuperSLMModel* Model, const FSuperSLMGpuRuntimeConfig& Config, TUniqueFunction<void(const FSuperSLMGpuConfigureReport&)> OnDone)
{
	check(IsInGameThread());
	// Game thread: the teardown of a prior state, the capture of the model's bytes, and the plugin
	// manager's shader directory.
	TearDown();
	LastConfigureShaderDirectory.Reset();
	LastPreMapLocalVideoMemoryBytes = -1;
	LastPostMapLocalVideoMemoryBytes = -1;
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
	const FString ShaderDirectory = ResolveShaderDirectory();
	TWeakObjectPtr<USuperSLMGpuSubsystem> WeakThis(this);
	TSharedPtr<FSuperSLMGpuConfigureFlight, ESPMode::ThreadSafe> Flight = MakeShared<FSuperSLMGpuConfigureFlight, ESPMode::ThreadSafe>();
	InFlightConfigures.Add(Flight);
	// Plugin paging plan §3.9 (K6): spent by this BeginConfigure(), as by Configure().
	const int32 PageCountPerturbation = NextConfigurePageCountPerturbation;
	const uint32 PoolCreateStatusOverride = NextPoolCreateStatusOverride;
	NextConfigurePageCountPerturbation = 0;
	NextPoolCreateStatusOverride = 0;

	// Pool thread: PrepareGpuState(). Its device jobs run on the new state's own submission thread
	// and this pool thread is the one that waits for them.
	Async(EAsyncExecution::ThreadPool,
		[WeakThis, Serial, bHasModel, Bytes = MoveTemp(Bytes), Hold = MoveTemp(Hold), Flight, Config, ShaderDirectory, PageCountPerturbation, PoolCreateStatusOverride,
			OnDone = MoveTemp(OnDone)]() mutable
		{
			FGpuConfigureDiagnostics Diag;
			FSuperSLMGpuSubsystemState* Built = nullptr;
			const FSuperSLMGpuConfigureReport Report = PrepareGpuState(bHasModel ? &Bytes : nullptr, Config, ShaderDirectory,
				PageCountPerturbation, PoolCreateStatusOverride, Diag, Built);
			{
				FScopeLock ScopeLock(&Flight->Lock);
				Flight->Prepared = Built;
			}
			Flight->Done->Trigger();

			// Game thread: publish, or discard when a later Configure()/BeginConfigure()/teardown
			// superseded this one (OnDone is then not called).
			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, Serial, Flight, Report, Diag = MoveTemp(Diag), Hold = MoveTemp(Hold), OnDone = MoveTemp(OnDone)]() mutable
				{
					// Null when Deinitialize() already claimed and discarded it (review W5).
					FSuperSLMGpuSubsystemState* S = Flight->Claim();
					USuperSLMGpuSubsystem* Self = WeakThis.Get();
					if (Self != nullptr)
					{
						Self->InFlightConfigures.Remove(Flight);
					}
					if (Self == nullptr || Self->ConfigureSerial != Serial || !Self->bConfigurePending)
					{
						if (S != nullptr)
						{
							// Shutdown() waits for the submission thread's release job: a pool thread's work.
							Async(EAsyncExecution::ThreadPool, [S]() { DiscardGpuState(S); });
						}
						Hold.Reset();
						return;
					}
					Self->bConfigurePending = false;
					Self->LastConfigureShaderDirectory = Diag.ShaderDirectory;
					Self->LastPreMapLocalVideoMemoryBytes = Diag.PreMapLocalVideoMemoryBytes;
					Self->LastPostMapLocalVideoMemoryBytes = Diag.PostMapLocalVideoMemoryBytes;
					if (S != nullptr)
					{
						S->NextLifecycleOpHandleId = Self->FirstHandleIdAfterTombstones(); // plugin paging plan §3.5 (K6)
						Self->State = S;
						Self->ConfiguredModel = Hold.IsValid() ? Hold->Get() : nullptr;
						RegisterConfigured(Self);
					}
					Hold.Reset();
					if (OnDone)
					{
						OnDone(Report);
					}
				});
		});
}

bool USuperSLMGpuSubsystem::IsGpuBackendActive() const
{
	return State != nullptr && State->bActive && !State->bTerminalLoss.load();
}

ESuperSLMGpuVendResult USuperSLMGpuSubsystem::VendSequence(FSuperSLMGpuSequence& OutSequence, ESuperSLMGpuDecodePath DecodePath)
{
	OutSequence = FSuperSLMGpuSequence();
	if (!IsGpuBackendActive())
	{
		return ESuperSLMGpuVendResult::NotConfigured;
	}
	for (int32 SlotIndex = 0; SlotIndex < State->Slots.Num(); ++SlotIndex)
	{
		FGameSlot& S = State->Slots[SlotIndex];
		if (S.bVended || S.bWithheld) // row 16: a withheld slot is never vended again
		{
			continue;
		}
		// Plan §2.5 row 20 rule 1 (H9): the new holder starts from a value-reset per-user struct --
		// WalkState included, back to kSslmGpuDfaWalkStateUnused (ruling 2026-09-26): a fresh
		// sequence has no schema, and a recycled one's queued recycle unbinds it.
		State->DropPendingEvents(S); // empty on a free slot; any event is logged as dropped
		static_cast<FGameSlotUser&>(S) = FGameSlotUser();
		S.OpLog.Reset();
		S.OpFront = 0;
		S.bReturnPending = false;
		S.bVended = true;
		S.Id = State->NextSequenceId++;
		S.Path = DecodePath;
		S.bBindEligibleAfterQueue = true; // row 4: fresh, or its recycle is queued ahead of every job of this holder
		State->IdToSlot.Add(S.Id, SlotIndex);
		OutSequence.Id = S.Id;
		return ESuperSLMGpuVendResult::Success;
	}
	TRACE_BOOKMARK(TEXT("SuperSLM: GPU pool refusal (%d of %d vended)"), State->IdToSlot.Num(), State->Slots.Num());
	return ESuperSLMGpuVendResult::PoolExhausted;
}

void USuperSLMGpuSubsystem::ReturnSequence(const FSuperSLMGpuSequence& Sequence)
{
	if (State == nullptr)
	{
		return;
	}
	int32 SlotIndex = INDEX_NONE;
	FGameSlot* S = State->FindSlot(Sequence, &SlotIndex);
	if (S == nullptr)
	{
		return;
	}
	// The plan's own queue-drain rule (§5): "the physical block is not handed to a new vend ...
	// until every operation already queued for this sequence ... has drained in arrival order."
	// A sequence handle is logically returned to the caller-facing free list at once regardless
	// (a second VendSequence() elsewhere still correctly sees every OTHER free slot; this one
	// just is not among them yet) -- the deferred free itself runs from AdmitQueuedOps() once
	// the log has fully drained.
	// D-SLM7758 (row 20's field table): the user's held schema binds that have not reached the
	// submission thread are dropped with the hold; they return no handle, and the recycle unbinds
	// whatever the sequence has bound.
	S->OpLog.RemoveAll([](const FLifecycleOpEntry& E) { return E.Kind == ELifecycleOpKind::SchemaBind && !E.bSubmitted; });
	if (S->OpLog.Num() > S->OpFront)
	{
		S->bReturnPending = true;
		return;
	}
	State->ReleaseUser(SlotIndex); // plan §2.5 row 20 rule 1 (H6)
}

ESuperSLMGpuDecodePath USuperSLMGpuSubsystem::GetDecodePath(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->Path : ESuperSLMGpuDecodePath::OneCall;
}

bool USuperSLMGpuSubsystem::SetSchema(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGpuSchemaHandle& Schema, FString& OutError)
{
	int32 SlotIndex = INDEX_NONE;
	FGameSlot* S = State != nullptr ? State->FindSlot(Sequence, &SlotIndex) : nullptr;
	if (S == nullptr || !IsGpuBackendActive())
	{
		OutError = TEXT("not a live GPU sequence handle");
		return false;
	}
	// Plan §2.5 row 4 (D-SLM7625, D-SLM7665): Layer 1 binds only a created or reset sequence that
	// has not since run a generation call or a restore. The slot knows the eligibility the
	// sequence will have once its queued work has run, and refuses by name without asking Layer 1.
	if (!S->bBindEligibleAfterQueue)
	{
		OutError = TEXT("bind needs a new or reset sequence; this one has generated or was restored; call ResetSequence() first");
		return false;
	}
	FString SchemaName;
	if (!Schema.IsNone())
	{
		const superslm::SchemaEntry* Entry = State->Schemas.ByIndex(static_cast<size_t>(Schema.Index));
		if (Entry == nullptr)
		{
			OutError = FString::Printf(TEXT("schema index %d does not exist in the configured artifact"), Schema.Index);
			return false;
		}
		SchemaName = UTF8_TO_TCHAR(std::string(Entry->name).c_str());
	}
	// Ruling 2026-09-26 (plan §2.5 row 21; D-SLM7758's held-bind path, now the only one): the bind
	// is always held as a SchemaBind entry in this sequence's op log and bound on the submission
	// thread behind every op ahead of it in call order, so this call never waits on the submission
	// FIFO. With the log empty it is admitted at once (TryAdmitImmediately()): its job is enqueued
	// now and finalized by a later tick (FinalizeSchemaBindOp()). A bind that is the submitted front
	// does not count against MaxQueuedOperationsPerSequence (ActiveWindowCount()); a bind still
	// queued behind other entries does. GetBoundSchema() lags until the bind's tick, and GetPhase()
	// stays Idle after a following RequestBeginGeneration() until then. True means the bind is accepted and ordered ahead of every
	// later request on this sequence; GetBoundSchema() reports it once it has reached Layer 1, and a
	// Layer-1 refusal faults this holder's next generation by name (the deferred-refusal field).
	// Game thread; game-side state only.
	FLifecycleOpEntry* Entry = nullptr;
	int32 QueuedSlot = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Queued = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::SchemaBind, &QueuedSlot, &Entry);
	if (Entry == nullptr || !Queued.IsValid())
	{
		OutError = State->LastLifecycleRequestError;
		return false;
	}
	Entry->BindSchema = Schema.IsNone() ? FSuperSLMGpuSchemaHandle() : Schema;
	Entry->BindSchemaName = SchemaName;
	State->TryAdmitImmediately(QueuedSlot);
	return true;
}

FSuperSLMGpuSchemaHandle USuperSLMGpuSubsystem::GetBoundSchema(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->Schema : FSuperSLMGpuSchemaHandle();
}

uint32 USuperSLMGpuSubsystem::GetSchemaWalkState(const FSuperSLMGpuSequence& Sequence) const
{
	// Ruling 2026-09-26 (plan §2.5 row 21): the game-side record (FGameSlotUser::WalkState), Layer
	// 1's walk as of the last applied result or finalized op, carried back from the submission
	// thread. Never a RunSync(). Game thread; game-side state only.
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->WalkState : kSslmGpuDfaWalkStateUnused;
}

bool USuperSLMGpuSubsystem::IsSchemaAccepting(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr && S->bSchemaAccepting;
}

bool USuperSLMGpuSubsystem::SetLayersPerSlice(const FSuperSLMGpuSequence& Sequence, int32 LayersPerSlice, FString& OutError)
{
	FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	if (S == nullptr || !IsGpuBackendActive())
	{
		OutError = TEXT("not a live GPU sequence handle");
		return false;
	}
	if (LayersPerSlice < 0)
	{
		OutError = FString::Printf(TEXT("layers per slice must be 0 (no cap) or positive, not %d"), LayersPerSlice);
		return false;
	}
	if (S->Phase == ESuperSLMSequencePhase::Prefilling || S->Phase == ESuperSLMSequencePhase::Decoding)
	{
		OutError = TEXT("the sequence is generating; set its layers per slice before BeginGeneration or after it completes");
		return false;
	}
	// Game-thread state only: LayersPerSliceFor() reads it when the tick plans a slice.
	S->LayersPerSliceOverride = LayersPerSlice;
	return true;
}

int32 USuperSLMGpuSubsystem::GetLayersPerSlice(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? State->LayersPerSliceFor(*S) : 0;
}

int32 USuperSLMGpuSubsystem::GetConfiguredK() const
{
	return State != nullptr ? State->Config.K : 0;
}

int32 USuperSLMGpuSubsystem::GetConfiguredMinimumK() const
{
	return State != nullptr ? State->MinimumK : 0;
}

bool USuperSLMGpuSubsystem::SetFixedTickLatency(int32 K, FString& OutError)
{
	if (State == nullptr || !IsGpuBackendActive())
	{
		OutError = TEXT("the GPU backend is not configured");
		return false;
	}
	// The same bounds Configure() applies (D-SLM7381 floor, D-SLM7803 kMaxJobTicks).
	constexpr int32 kMaxJobTicks = 1 << 20;
	if (K < State->MinimumK || K > kMaxJobTicks)
	{
		OutError = FString::Printf(TEXT("K %d is outside [%d, %d] (the Configure()'d minimum and kMaxJobTicks)"), K, State->MinimumK, kMaxJobTicks);
		return false;
	}
	if (K == State->Config.K && !State->bRestoreScheduleWhenDrained)
	{
		return true;
	}
	// A vended slot (bReturnPending included) may hold pending events whose ApplyTick was computed
	// at the old K; a queued restore would admit under it. Either way another query is live.
	if (!IsPoolDrainedForSchedulerChange(TEXT("K"), OutError))
	{
		return false;
	}
	// Game-thread state only: every reader of Config.K (the apply-tick schedule, the hitch check,
	// the K trace counter, the self-check's tick ceiling) runs on the game thread.
	State->Config.K = K;
	State->bRestoreScheduleWhenDrained = false; // a newer override supersedes a pending restore (R2-W3)
	return true;
}

int32 USuperSLMGpuSubsystem::GetLayersPerTick() const
{
	return State != nullptr ? State->LayersPerTick : 0;
}

bool USuperSLMGpuSubsystem::IsPoolDrainedForSchedulerChange(const TCHAR* What, FString& OutError) const
{
	for (const FGameSlot& S : State->Slots)
	{
		if (S.bVended)
		{
			OutError = FString::Printf(TEXT("a GPU sequence is still vended; %s changes only while the pool is drained"), What);
			return false;
		}
	}
	if (State->RestoreOpsFront < State->RestoreOps.Num())
	{
		OutError = FString::Printf(TEXT("a GPU restore is still queued; %s changes only while the pool is drained"), What);
		return false;
	}
	return true;
}

bool USuperSLMGpuSubsystem::SetLayersPerTick(int32 LayersPerTick, FString& OutError)
{
	if (State == nullptr || !IsGpuBackendActive())
	{
		OutError = TEXT("the GPU backend is not configured");
		return false;
	}
	if (LayersPerTick < 1)
	{
		OutError = FString::Printf(TEXT("layers per tick must be at least one whole layer, not %d"), LayersPerTick);
		return false;
	}
	const int32 Layers = FMath::Min(LayersPerTick, State->NumHiddenLayers);
	if (Layers == State->LayersPerTick && !State->bRestoreScheduleWhenDrained)
	{
		return true;
	}
	// A composed sequence mid-token was planned against the old per-tick budget, and its apply
	// ticks against the K sized from it.
	if (!IsPoolDrainedForSchedulerChange(TEXT("the tick's layer budget"), OutError))
	{
		return false;
	}
	// D-SLM7381, as Configure() computes it: BlockCount composed sequences sharing Layers per tick
	// need ceil(BlockCount x L / Layers) ticks per token; one-call sequences need BlockCount.
	const int64 ComposedMinimumK = (static_cast<int64>(State->Config.BlockCount) * State->NumHiddenLayers + Layers - 1) / Layers;
	const int64 OneCallMinimumK = static_cast<int64>(State->Config.BlockCount) + (State->IsPooled() && State->Config.PrefixBlockCount > 0 ? 1 : 0); // K7: the build seat
	const int32 NewMinimumK = static_cast<int32>(FMath::Max<int64>(ComposedMinimumK, OneCallMinimumK));
	constexpr int32 kMaxJobTicks = 1 << 20;
	if (NewMinimumK > kMaxJobTicks)
	{
		OutError = FString::Printf(TEXT("%d layers per tick needs K >= %d, above kMaxJobTicks %d"), Layers, NewMinimumK, kMaxJobTicks);
		return false;
	}
	// Game-thread state only: PlanAndIssue(), LayersPerSliceFor() and the K readers run on the game
	// thread; the submission thread receives each slice's dispatch count in its job.
	State->LayersPerTick = Layers;
	State->Config.DispatchBudget = static_cast<uint32>(Layers) * State->DispatchesPerLayer;
	State->MinimumK = NewMinimumK;
	State->Config.K = FMath::Max(State->Config.K, NewMinimumK);
	State->bRestoreScheduleWhenDrained = false; // a newer override supersedes a pending restore (R2-W3)
	return true;
}

void USuperSLMGpuSubsystem::RequestConfiguredScheduleRestore()
{
	if (State == nullptr)
	{
		return;
	}
	State->bRestoreScheduleWhenDrained = true;
	TryRestoreConfiguredSchedule();
}

void USuperSLMGpuSubsystem::TryRestoreConfiguredSchedule()
{
	if (State == nullptr || !State->bRestoreScheduleWhenDrained)
	{
		return;
	}
	// The same gate as the overrides: a live sequence's apply ticks were scheduled under the
	// override, so the restore waits for the pool to drain (a later Tick() retries).
	FString NotYet;
	if (!IsPoolDrainedForSchedulerChange(TEXT("the configured schedule"), NotYet))
	{
		return;
	}
	State->LayersPerTick = State->ConfiguredLayersPerTick;
	State->Config.DispatchBudget = State->ConfiguredDispatchBudget;
	State->MinimumK = State->ConfiguredMinimumK;
	State->Config.K = State->ConfiguredK;
	State->bRestoreScheduleWhenDrained = false;
}

bool USuperSLMGpuSubsystem::MapAdapter(const FString& AbsolutePath, USuperSLMModel& Base, FSuperSLMGpuAdapterHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMGpuAdapterHandle();
	if (!IsGpuBackendActive() || ConfiguredModel != &Base)
	{
		OutError = TEXT("the base model is not the one this GPU subsystem is configured with");
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath) || Bytes.Num() < 64)
	{
		OutError = FString::Printf(TEXT("could not read the adapter artifact '%s'"), *AbsolutePath);
		return false;
	}
	FAdapterEntry Entry;
	FMemory::Memcpy(Entry.ArtifactHash, Bytes.GetData() + 32, 32);
	Entry.ResidentBytes = Bytes.Num();
	Entry.View = std::make_unique<superslm::SslmModelView>();
	{
		std::string Err;
		superslm::SslmModelStatus LoadStatus;
		try
		{
			LoadStatus = superslm::SslmModel::Load(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), *Entry.View, &Err);
		}
		catch (const std::bad_alloc&)
		{
			OutError = TEXT("out of memory while loading the adapter artifact");
			return false;
		}
		if (LoadStatus != superslm::SslmModelStatus::Ok)
		{
			OutError = FString::Printf(TEXT("the adapter artifact did not load: %s (%s)"), ANSI_TO_TCHAR(superslm::SslmModelStatusName(LoadStatus)), UTF8_TO_TCHAR(Err.c_str()));
			return false;
		}
	}
	struct FMapOutcome
	{
		SslmGpuAdapterHandle* Handle = nullptr;
		FGpuStatus St = FGpuStatus::SSLM_OK;
	};
	const TSharedRef<FMapOutcome, ESPMode::ThreadSafe> Outcome = MakeShared<FMapOutcome, ESPMode::ThreadSafe>();
	FSuperSLMGpuSubsystemState* St8 = State;
	const superslm::SslmModelView* ViewPtr = Entry.View.get();
	if (!State->RunSync([St8, ViewPtr, Outcome]()
	{
		Outcome->St = sslm_gpu_adapter_map(St8->Ctx, St8->Model, ViewPtr, &Outcome->Handle);
		St8->NoteLayer1Status(Outcome->St); // K6 review S2
	}, TEXT("load-time call (adapter map)"), LoadTimeBoundSeconds(Entry.ResidentBytes), Entry.ResidentBytes)) // row 21, D-SLM7762
	{
		// Row 21: the view the map may still be reading is leaked, never freed under it.
		(void)Entry.View.release();
		OutError = FString::Printf(TEXT("the GPU submission thread did not respond while mapping '%s'"), *AbsolutePath);
		return false;
	}
	if (Outcome->St != FGpuStatus::SSLM_OK || Outcome->Handle == nullptr)
	{
		OutError = FString::Printf(TEXT("sslm_gpu_adapter_map returned %s for '%s'"), *GpuStatusText(Outcome->St), *AbsolutePath);
		return false;
	}
	Entry.Handle = Outcome->Handle;
	const int64 Id = State->NextAdapterId++;
	State->Adapters.Add(Id, MoveTemp(Entry));
	OutHandle.Id = Id;
	return true;
}

void USuperSLMGpuSubsystem::UnmapAdapter(const FSuperSLMGpuAdapterHandle& Adapter)
{
	// Round 3 (finding 2): after a confirmed loss the entry is kept; the teardown unmaps it.
	if (!IsGpuBackendActive())
	{
		return;
	}
	FAdapterEntry* Entry = State->Adapters.Find(Adapter.Id);
	if (Entry == nullptr || State->Thread == nullptr)
	{
		return;
	}
	for (const FGameSlot& S : State->Slots)
	{
		if (S.bVended && (S.ActiveAdapterId == Adapter.Id || (S.bSwapPending && S.RequestedAdapterId == Adapter.Id)))
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("UnmapAdapter refused: GPU sequence %lld still uses this adapter; swap it away or return the sequence first."), S.Id);
			return;
		}
	}
	// Plan §2.5 row 20 rule 4 (T-2987 L1): a queued or admitted restore that references this
	// adapter keeps it mapped until the restore resolves.
	if (Entry->RestorePins > 0)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("UnmapAdapter refused: %d queued or in-flight restore(s) reference this adapter; let them resolve first."), Entry->RestorePins);
		return;
	}
	const TSharedRef<FGpuStatus, ESPMode::ThreadSafe> St = MakeShared<FGpuStatus, ESPMode::ThreadSafe>(FGpuStatus::SSLM_OK);
	FSuperSLMGpuSubsystemState* St8 = State;
	SslmGpuAdapterHandle* Handle = Entry->Handle;
	const bool bFinished = State->RunSync([St8, Handle, St]()
	{
		for (FThreadSlot& TS : St8->ThreadSlots)
		{
			if (TS.BoundAdapter == Handle && TS.Handle != nullptr)
			{
				const FGpuStatus UnbindSt = sslm_gpu_seq_bind_adapter(St8->Ctx, TS.Handle, nullptr);
				St8->NoteLayer1Status(UnbindSt); // K6 review S2
				if (UnbindSt == FGpuStatus::SSLM_OK)
				{
					TS.BoundAdapter = nullptr;
				}
			}
		}
		*St = sslm_gpu_adapter_unmap(St8->Ctx, Handle);
		St8->NoteLayer1Status(*St);
	});
	if (!bFinished)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("UnmapAdapter: the GPU submission thread did not respond; the adapter entry is kept."));
		return;
	}
	if (*St != FGpuStatus::SSLM_OK)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("sslm_gpu_adapter_unmap refused (%s); the adapter stays mapped."), *GpuStatusText(*St));
		return;
	}
	State->Adapters.Remove(Adapter.Id);
}

void USuperSLMGpuSubsystem::RequestAdapterSwap(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGpuAdapterHandle& Adapter)
{
	// Round 3 (finding 2): a swap is a request for work on the device; refused after a loss.
	FGameSlot* S = IsGpuBackendActive() ? State->FindSlot(Sequence) : nullptr;
	if (S == nullptr)
	{
		return;
	}
	if (Adapter.IsValid() && !State->Adapters.Contains(Adapter.Id))
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("RequestAdapterSwap: adapter %lld is not mapped on this GPU subsystem."), Adapter.Id);
		return;
	}
	// Applied at the next token boundary the planner reaches (embed, one-call token, or prefill),
	// never mid-token: Layer 1 refuses a mid-token bind (SSLM_BUSY), and a composed token's
	// slices keep the adapter pinned at its embed (D-SLM7256 rule 4). Plan §2.5 row 20 rule 4
	// (T-2983 finding 10): the request mints the next per-slot bind ordinal; the action that
	// carries it echoes it back, and only a result with that tag confirms the swap. A request is
	// carried whenever it differs from the confirmed binding or an earlier request is still
	// awaiting confirmation (whose carrier may already have bound a different adapter).
	S->RequestedAdapterId = Adapter.IsValid() ? Adapter.Id : 0;
	S->AwaitedBindTag = ++S->NextBindOrdinal;
	S->bSwapPending = S->RequestedAdapterId != S->ActiveAdapterId || S->bPendingBindConfirm;
}

FSuperSLMGpuAdapterHandle USuperSLMGpuSubsystem::GetActiveAdapter(const FSuperSLMGpuSequence& Sequence) const
{
	FSuperSLMGpuAdapterHandle Handle;
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	if (S != nullptr)
	{
		Handle.Id = S->ActiveAdapterId;
	}
	return Handle;
}

ESuperSLMSequencePhase USuperSLMGpuSubsystem::GetPhase(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->Phase : ESuperSLMSequencePhase::Faulted;
}

const TArray<int32>& USuperSLMGpuSubsystem::GetGeneratedTokens(const FSuperSLMGpuSequence& Sequence) const
{
	static const TArray<int32> Empty;
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->Generated : Empty;
}

ESuperSLMDecodeOutcome USuperSLMGpuSubsystem::GetLastDecodeOutcome(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->LastOutcome : ESuperSLMDecodeOutcome::SequenceNoLongerValid;
}

ESuperSLMGpuFaultReason USuperSLMGpuSubsystem::GetLastFaultReason(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->LastFault : ESuperSLMGpuFaultReason::None;
}

bool USuperSLMGpuSubsystem::ProbeContextUsable()
{
	// Round 3 (finding 2): false after a confirmed loss, before Ctx (which ReleaseLayer1() clears on
	// the submission thread) is read.
	if (!IsGpuBackendActive())
	{
		return false;
	}
	if (State->Thread == nullptr || State->Ctx == nullptr)
	{
		return false;
	}
	const TSharedRef<bool, ESPMode::ThreadSafe> bUsable = MakeShared<bool, ESPMode::ThreadSafe>(false);
	FSuperSLMGpuSubsystemState* St8 = State;
	const bool bFinished = State->RunSync([St8, bUsable]() { *bUsable = St8->ProbeUsable(); });
	if (!bFinished || !*bUsable)
	{
		State->bTerminalLoss = true;
		return false;
	}
	return true;
}

bool USuperSLMGpuSubsystem::TickOncePerFrame(float DeltaSeconds)
{
	if (LastTickFrame == GFrameCounter)
	{
		return false;
	}
	Tick(DeltaSeconds);
	return true;
}

void USuperSLMGpuSubsystem::Tick(float DeltaSeconds)
{
	LastTickFrame = GFrameCounter; // review W6
	if (State != nullptr && State->FinishTeardownIfDone())
	{
		return; // round 3: the pending-teardown window's tick applies, admits and plans nothing
	}
	if (State == nullptr || !State->bActive)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.Tick", SuperSLMGpuChannel);
	TryRestoreConfiguredSchedule(); // R2-W3: a restore requested while the pool was still live
	const double TickStart = FPlatformTime::Seconds();
	State->TickIndex += 1;
	State->SimSeconds += FMath::Max(0.0, static_cast<double>(DeltaSeconds));

	// Ruling 2026-09-26 (plan §2.5 row 21, §5): the tick never waits on the submission thread.
	// On every tick with any job outstanding -- tick or lifecycle, so a hung bind, reset, save,
	// restore or prefill slice is covered too -- the Layer-1 call holding the process-wide lock is
	// checked against its own row-21 bound, exactly as Wait() checks it (CheckLockHolderOverrun(),
	// with no waiter job: the abandoned job is named from the holder, CurrentJobSeq or the queue
	// front). An overrun marks the thread unresponsive and sets bTerminalLoss, and the device-loss
	// path is taken at once. ApplyDue() then applies what is done and leaves an overdue event for
	// a later tick; bTerminalLoss is checked again after it (a fault it applied may have set it).
	if (!State->bUnresponsive.load() &&
		State->OutstandingTickJobs.load() + State->OutstandingOtherJobs.load() > 0)
	{
		State->CheckLockHolderOverrun(0);
	}
	if (State->bTerminalLoss.load())
	{
		State->BeginTeardownAfterLoss(); // round 2: never waits
		return;
	}
	State->ApplyDue();
	if (State->bTerminalLoss.load())
	{
		State->BeginTeardownAfterLoss(); // round 2: never waits
		return;
	}
	// The per-sequence software queue's own admission pass (D-SLM7421/D-SLM7457): polls every
	// in-flight queued op for completion, advances each queue's own front past whatever just
	// resolved, and admits the next eligible op per sequence (plus the subsystem-level restore
	// queue) in global-request-ordinal order. Runs before decode planning, mirroring "the
	// submission thread issues restores at the start of a tick, before that tick's slices" (§5)
	// generalized to every op kind this queue now carries.
	State->AdmitQueuedOps();
	{
		FScopeLock Guard(&State->TimingLock);
		State->GameTiming = State->CompletedTiming;
	}
	State->PlanAndIssue();

	// Read-only reporting of this tick's own game-thread cost -- the frame pays only issue and
	// readback (plan §5); this never feeds a decode. The cumulative hitch counters (GameHitches,
	// ThreadHitches) are re-published here so the trace counter reads correctly even on a tick
	// that raised no new hitch of its own. This is the counter's ONLY writer: the submission
	// thread's RecordGpuBusy() and ApplyDue()'s ExamineEvent() bump GameHitches/ThreadHitches but no
	// longer TRACE_COUNTER_INCREMENT it, because FCounterInt's value is a plain int64 and two
	// threads writing it raced. GameHitches is game-thread-only and ThreadHitches is atomic, so
	// this sum is the true total of every hitch recorded before this point.
	CSV_CUSTOM_STAT(SuperSLMGpu, TickMs, (FPlatformTime::Seconds() - TickStart) * 1000.0, ECsvCustomStatOp::Set);
	const int32 TotalHitches = State->GameHitches + State->ThreadHitches.load();
	CSV_CUSTOM_STAT(SuperSLMGpu, Hitches, TotalHitches, ECsvCustomStatOp::Set);
	TRACE_COUNTER_SET_ALWAYS(SuperSLMGpuHitches, TotalHitches);
	TRACE_COUNTER_SET_ALWAYS(SuperSLMGpuRetainedResultBytes, State->RetainedResultBytes);
}

FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestResetSequence(const FSuperSLMGpuSequence& Sequence, ESuperSLMGpuDecodePath NewDecodePath)
{
	if (State == nullptr)
	{
		return FSuperSLMLifecycleOpHandle();
	}
	FLifecycleOpEntry* Entry = nullptr;
	int32 SlotIndex = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::Reset, &SlotIndex, &Entry);
	if (Entry != nullptr)
	{
		Entry->NewDecodePath = NewDecodePath;
		State->Slots[SlotIndex].bBindEligibleAfterQueue = true; // row 4: a queued reset makes the sequence bind-eligible
		State->TryAdmitImmediately(SlotIndex);
	}
	return Handle;
}

FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestSaveSequence(const FSuperSLMGpuSequence& Sequence)
{
	if (State == nullptr)
	{
		return FSuperSLMLifecycleOpHandle();
	}
	FLifecycleOpEntry* Entry = nullptr;
	int32 SlotIndex = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::Save, &SlotIndex, &Entry);
	if (Entry != nullptr)
	{
		State->TryAdmitImmediately(SlotIndex);
	}
	return Handle;
}

FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestBeginGeneration(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGenerationRequest& Request)
{
	if (State == nullptr || !IsGpuBackendActive())
	{
		if (State != nullptr)
		{
			State->LastLifecycleRequestError = TEXT("not a live GPU sequence handle");
		}
		return FSuperSLMLifecycleOpHandle();
	}
	// The request SHAPE is validated here, synchronously, exactly as the prior synchronous
	// BeginGeneration() did -- none of it needs Layer 1 or the sequence's own turn in the queue.
	// Plugin paging plan §3.4 "An empty prompt after adopt" (K7, rev 1.9 ruling 6): in pooled mode an
	// empty prompt is accepted here and checked at the generation's queue-head admission, where the
	// sequence's armed flag is known (AdmitBeginGenerationOp()). Dedicated mode refuses it here.
	if (Request.PromptTokens.Num() == 0 && !State->IsPooled())
	{
		State->LastLifecycleRequestError = TEXT("the prompt is empty; a GPU sequence is primed by prefilling at least one prompt token");
		return FSuperSLMLifecycleOpHandle();
	}
	if (Request.MaxNewTokens <= 0)
	{
		State->LastLifecycleRequestError = TEXT("MaxNewTokens must be positive");
		return FSuperSLMLifecycleOpHandle();
	}
	for (const int32 Token : Request.PromptTokens)
	{
		if (Token < 0 || Token >= State->VocabSize)
		{
			State->LastLifecycleRequestError = FString::Printf(TEXT("prompt token %d is outside the vocabulary [0, %d)"), Token, State->VocabSize);
			return FSuperSLMLifecycleOpHandle();
		}
	}
	if (Request.SpanKind == ESuperSLMSpanKind::SchemaContent)
	{
		State->LastLifecycleRequestError = TEXT("a SchemaContent span is not supported on the GPU backend; feed it as a Prompt span or use the CPU backend");
		return FSuperSLMLifecycleOpHandle();
	}
	FLifecycleOpEntry* Entry = nullptr;
	int32 SlotIndex = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::BeginGeneration, &SlotIndex, &Entry);
	if (Entry != nullptr)
	{
		Entry->Request = Request;
		State->Slots[SlotIndex].bBindEligibleAfterQueue = false; // row 4: a queued generation clears eligibility
		State->TryAdmitImmediately(SlotIndex);
	}
	return Handle;
}

FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestAdoptPrefix(const FSuperSLMGpuSequence& Sequence)
{
	if (State == nullptr)
	{
		return FSuperSLMLifecycleOpHandle();
	}
	FLifecycleOpEntry* Entry = nullptr;
	int32 SlotIndex = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::AdoptPrefix, &SlotIndex, &Entry);
	if (Entry != nullptr)
	{
		State->TryAdmitImmediately(SlotIndex);
	}
	return Handle;
}

// Plugin paging plan §3.4 "Adopt", rev 1.9 ruling 2 (K6 for dedicated mode). Dedicated: queued in the
// sequence's own order like the no-prefix overload, and resolved UnsupportedOnGpu at its admission,
// with a reason naming the mode (AdmitAdoptPrefixOp()). Pooled (K7): the prefix must be Ready at the
// call (refused Malformed, with no handle, otherwise); the adopt is queued in the sequence's own order
// and admitted by the page ledger at its turn (AdmitPooledAdoptOp()).
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestAdoptPrefix(const FSuperSLMGpuSequence& Sequence, const FSuperSLMPrefix& Prefix)
{
	if (State == nullptr)
	{
		return FSuperSLMLifecycleOpHandle();
	}
	if (State->IsPooled())
	{
		if (State->FindReadyPrefix(Prefix.Id) == nullptr)
		{
			State->LastLifecycleRequestError = TEXT("Malformed: the prefix is not a Ready GPU prefix of this subsystem (unknown, released, faulted, still building, or another backend's)");
			return FSuperSLMLifecycleOpHandle();
		}
		FLifecycleOpEntry* PooledEntry = nullptr;
		int32 PooledSlotIndex = INDEX_NONE;
		const FSuperSLMLifecycleOpHandle PooledHandle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::AdoptPrefix, &PooledSlotIndex, &PooledEntry);
		if (PooledEntry != nullptr)
		{
			PooledEntry->bPrefixOverload = true;
			PooledEntry->AdoptPrefixId = Prefix.Id;
			State->TryAdmitImmediately(PooledSlotIndex);
		}
		return PooledHandle;
	}
	FLifecycleOpEntry* Entry = nullptr;
	int32 SlotIndex = INDEX_NONE;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueSlotOp(Sequence, ELifecycleOpKind::AdoptPrefix, &SlotIndex, &Entry);
	if (Entry != nullptr)
	{
		Entry->bPrefixOverload = true;
		State->TryAdmitImmediately(SlotIndex);
	}
	return Handle;
}

FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestRestoreSequence(const TArray<uint8>& Blob, USuperSLMModel* ExpectedModel)
{
	if (State == nullptr)
	{
		return FSuperSLMLifecycleOpHandle();
	}
	FLifecycleOpEntry* Entry = nullptr;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueRestoreOp(Blob, ExpectedModel, &Entry);
	if (Entry != nullptr)
	{
		State->TryAdmitRestoreImmediately();
	}
	return Handle;
}

// Plugin paging plan §3.5 (K7): the hinted restore. In pooled mode the hint names the prefix whose
// pages the restored sequence may share (sslm_gpu_seq_restore_into_pool's prefix_or_null); a hint
// that names no Ready prefix of this subsystem when the restore arrives is dropped, and the restore
// runs unhinted (a hint is never a requirement). Dedicated mode, or an invalid hint: the overload above.
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestRestoreSequence(const TArray<uint8>& Blob, USuperSLMModel* ExpectedModel, const FSuperSLMPrefix& Hint)
{
	if (State == nullptr || !State->IsPooled() || !Hint.IsValid())
	{
		return RequestRestoreSequence(Blob, ExpectedModel);
	}
	FLifecycleOpEntry* Entry = nullptr;
	const FSuperSLMLifecycleOpHandle Handle = State->EnqueueRestoreOp(Blob, ExpectedModel, &Entry);
	if (Entry != nullptr)
	{
		Entry->RestoreHintPrefixId = Hint.Id;
		State->TryAdmitRestoreImmediately();
	}
	return Handle;
}

ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetLifecycleOpResult(const FSuperSLMLifecycleOpHandle& Handle) const
{
	// Plugin paging plan §3.5 (K6): a handle a pooled teardown left Pending reads NotConfigured.
	if (Handle.IsValid() && LifecycleTombstones.Contains(Handle.Id))
	{
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State == nullptr || !Handle.IsValid())
	{
		return ESuperSLMRestoreResult::Pending;
	}
	// Row 19 (D-SLM7763): a resolved handle's result lives in HandleResults until it is released;
	// an unresolved or released handle reads Pending.
	const FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	return Entry != nullptr ? Entry->Result : ESuperSLMRestoreResult::Pending;
}

int64 USuperSLMGpuSubsystem::GetLifecycleOpResolutionOrdinal(const FSuperSLMLifecycleOpHandle& Handle) const
{
	// K6 review N3: a tombstoned handle has no resolution ordinal (it never resolved in its state).
	if (State == nullptr || !Handle.IsValid() || LifecycleTombstones.Contains(Handle.Id))
	{
		return -1;
	}
	const FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	return Entry != nullptr ? Entry->ResolutionOrdinal : -1;
}

ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetSaveResult(const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob)
{
	OutBlob.Reset();
	// Plugin paging plan §3.5 (K6): a tombstone reads NotConfigured, with no blob.
	if (Handle.IsValid() && LifecycleTombstones.Contains(Handle.Id))
	{
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State == nullptr || !Handle.IsValid())
	{
		return ESuperSLMRestoreResult::Pending;
	}
	FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	if (Entry == nullptr)
	{
		return ESuperSLMRestoreResult::Pending;
	}
	if (Entry->Result != ESuperSLMRestoreResult::Success || Entry->Kind != ELifecycleOpKind::Save)
	{
		return Entry->Result;
	}
	// Row 19 (D-SLM7763): the first successful read moves the blob out, with no copy; the entry
	// keeps only its result, and every later read says Consumed.
	if (Entry->bConsumed)
	{
		return ESuperSLMRestoreResult::Consumed;
	}
	State->RetainedResultBytes -= Entry->SaveBlob.Num();
	OutBlob = MoveTemp(Entry->SaveBlob);
	Entry->SaveBlob.Empty();
	Entry->bConsumed = true;
	return ESuperSLMRestoreResult::Success;
}

bool USuperSLMGpuSubsystem::ReleaseLifecycleOpHandle(const FSuperSLMLifecycleOpHandle& Handle)
{
	// Plugin paging plan §3.5 (K6): releasing a tombstone erases it; the handle then reads Pending.
	if (Handle.IsValid() && LifecycleTombstones.Remove(Handle.Id) > 0)
	{
		return true;
	}
	if (State == nullptr || !Handle.IsValid())
	{
		return false;
	}
	FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	if (Entry == nullptr)
	{
		return false; // unknown, already released, or still Pending (only resolved handles have an entry)
	}
	State->RetainedResultBytes -= Entry->SaveBlob.Num();
	State->HandleResults.Remove(Handle.Id);
	return true;
}

int64 USuperSLMGpuSubsystem::GetRetainedResultBytes() const
{
	return State != nullptr ? State->RetainedResultBytes : 0;
}

int32 USuperSLMGpuSubsystem::GetLifecycleHandleEntryCount() const
{
	// Plugin paging plan §3.5 (K6): tombstones count, since the game still owes their release.
	return (State != nullptr ? State->HandleResults.Num() : 0) + LifecycleTombstones.Num();
}

int32 USuperSLMGpuSubsystem::GetRestoreOpEntryCount() const
{
	return State != nullptr ? State->RestoreOps.Num() : 0;
}

bool USuperSLMGpuSubsystem::GetSaveTokenCounts(const FSuperSLMLifecycleOpHandle& Handle, int32& OutObservedAtSave, int32& OutProducedAtSave) const
{
	OutObservedAtSave = -1;
	OutProducedAtSave = -1;
	// K6 review N3: a tombstoned save produced no blob, so it has no counts.
	if (State == nullptr || !Handle.IsValid() || LifecycleTombstones.Contains(Handle.Id))
	{
		return false;
	}
	// Kept in the handle's result entry, so it reads the same before and after the blob is taken.
	const FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	if (Entry == nullptr || Entry->Kind != ELifecycleOpKind::Save || Entry->Result != ESuperSLMRestoreResult::Success || !Entry->bHasSaveCounts)
	{
		return false;
	}
	OutObservedAtSave = Entry->ObservedAtSave;
	OutProducedAtSave = Entry->ProducedAtSave;
	return true;
}

ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetRestoreResult(const FSuperSLMLifecycleOpHandle& Handle, FSuperSLMGpuSequence& OutSequence) const
{
	OutSequence = FSuperSLMGpuSequence();
	// Plugin paging plan §3.5 (K6): a tombstone reads NotConfigured, with no sequence.
	if (Handle.IsValid() && LifecycleTombstones.Contains(Handle.Id))
	{
		return ESuperSLMRestoreResult::NotConfigured;
	}
	if (State == nullptr || !Handle.IsValid())
	{
		return ESuperSLMRestoreResult::Pending;
	}
	// Row 19 (D-SLM7763): the RestoreOps entry is removed at resolution; the result stays here.
	const FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = State->HandleResults.Find(Handle.Id);
	if (Entry == nullptr)
	{
		return ESuperSLMRestoreResult::Pending;
	}
	if (Entry->Result == ESuperSLMRestoreResult::Success && Entry->Kind == ELifecycleOpKind::Restore)
	{
		OutSequence = Entry->RestoreOutSequence;
	}
	return Entry->Result;
}

int32 USuperSLMGpuSubsystem::GetPendingLifecycleOperationCount(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	if (S == nullptr)
	{
		return 0;
	}
	// Ruling 2026-09-26 (finding 9, round 2): the same quantity ActiveWindowCount() bounds, so a bind
	// that is the submitted front is left out here too.
	int32 Count = State->ActiveWindowCount(*S);
	// An ongoing generation whose OWN entry has already advanced past OpFront (its "delivered"
	// moment is admission, not completion) still counts as one in-flight operation.
	if (S->ActiveGenerationOpLogIndex != INDEX_NONE && S->ActiveGenerationOpLogIndex < S->OpFront)
	{
		Count += 1;
	}
	return Count;
}

FString USuperSLMGpuSubsystem::GetLastLifecycleRequestError() const
{
	return State != nullptr ? State->LastLifecycleRequestError : FString();
}

double USuperSLMGpuSubsystem::GetLastGpuBusyMs() const
{
	return State != nullptr ? State->LastGpuBusyMs.load() : 0.0;
}

FSuperSLMGpuTimingSnapshot USuperSLMGpuSubsystem::GetGpuTimingSnapshot() const
{
	check(IsInGameThread());
	return State != nullptr ? State->GameTiming : FSuperSLMGpuTimingSnapshot{};
}

double USuperSLMGpuSubsystem::GetLastHostFinishMs() const
{
	return State != nullptr ? State->LastHostFinishMs.load() : 0.0;
}

double USuperSLMGpuSubsystem::GetTimeToFirstTokenMs(const FSuperSLMGpuSequence& Sequence) const
{
	const FGameSlot* S = State != nullptr ? State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->FirstTokenMs : -1.0;
}

int32 USuperSLMGpuSubsystem::GetHitchCount() const
{
	return State != nullptr ? State->GameHitches + State->ThreadHitches.load() : 0;
}

int64 USuperSLMGpuSubsystem::GetGpuDispatchCount() const
{
	return State != nullptr ? State->DispatchCount.load() : 0;
}

int32 USuperSLMGpuSubsystem::GetPoolFreeCount() const
{
	if (State == nullptr)
	{
		return 0;
	}
	int32 Free = 0;
	for (const FGameSlot& S : State->Slots)
	{
		Free += (S.bVended || S.bWithheld) ? 0 : 1;
	}
	return Free;
}

int32 USuperSLMGpuSubsystem::GetPoolOccupiedCount() const
{
	if (State == nullptr)
	{
		return 0;
	}
	int32 Occupied = 0;
	for (const FGameSlot& S : State->Slots)
	{
		Occupied += S.bVended ? 1 : 0;
	}
	return Occupied;
}

int32 USuperSLMGpuSubsystem::GetMappedAdapterCount() const
{
	return State != nullptr ? State->Adapters.Num() : 0;
}

int32 USuperSLMGpuSubsystem::GetWithheldSlotCount() const
{
	if (State == nullptr)
	{
		return 0;
	}
	int32 Withheld = 0;
	for (const FGameSlot& S : State->Slots)
	{
		Withheld += (S.bWithheld && !S.bVended) ? 1 : 0;
	}
	return Withheld;
}

FString USuperSLMGpuSubsystem::GetShaderDirectory() const
{
	return LastConfigureShaderDirectory;
}

int64 USuperSLMGpuSubsystem::GetPreMapLocalVideoMemoryBytes() const
{
	return LastPreMapLocalVideoMemoryBytes;
}

int64 USuperSLMGpuSubsystem::GetPostMapLocalVideoMemoryBytes() const
{
	return LastPostMapLocalVideoMemoryBytes;
}

bool USuperSLMGpuSubsystem::IsDeviceHeadActive() const
{
	return State != nullptr && State->bDeviceHeadActive;
}

FString USuperSLMGpuSubsystem::GetDeviceHeadStatus() const
{
	return State != nullptr ? State->DeviceHeadStatus : FString();
}

int64 USuperSLMGpuSubsystem::GetDeclaredGpuResidencyBytes() const
{
	if (State == nullptr)
	{
		return 0;
	}
	int64 Bytes = State->DeclaredModelBytes;
	for (const FThreadSlot& TS : State->ThreadSlots)
	{
		Bytes += TS.KvBytes; // 0 for a pooled holder
	}
	// Plugin paging plan §3.4 (K6): a pooled subsystem's K/V is its page pool. K6 review S1, plan
	// §3.2/§3.4 rev 1.10 (K7 review S2): this figure is the device-local lower bound the header and the
	// query window promise, so it counts the pool's device buffer once (PoolPages x PageBytes); the
	// engine's host mirror of the same size is system RAM, reported in the page stats as their product.
	if (State->IsPooled())
	{
		Bytes += State->PoolBytes;
	}
	for (const TPair<int64, FAdapterEntry>& Pair : State->Adapters)
	{
		Bytes += Pair.Value.ResidentBytes;
	}
	return Bytes;
}

int64 USuperSLMGpuSubsystem::GetLocalVideoMemoryUsageBytes() const
{
	if (State == nullptr || !State->Adapter3)
	{
		return 0;
	}
	DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
	if (FAILED(State->Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
	{
		return 0;
	}
	return static_cast<int64>(Info.CurrentUsage);
}

// ---------------------------------------------------------------------------------------------
// Paged KV, pooled mode (plugin paging plan §3.2-§3.6; step K6)
// ---------------------------------------------------------------------------------------------

namespace
{
	// Plan rev 1.9 ruling 2: every prefix verb on a dedicated subsystem is UnsupportedOnGpu before
	// any other check (the tokens, a schema handle or a parent are never read), with no handle and a
	// reason naming SequenceBudget. Unconfigured is NotConfigured. In pooled mode the verb proceeds
	// (Success here), and FSuperSLMGpuSubsystemState::CreateGpuPrefix() decides it (K7).
	ESuperSLMPrefixResult RefuseGpuPrefixVerb(const FSuperSLMGpuSubsystemState* S, bool bActive, const TCHAR* Verb, FString* OutError)
	{
		if (S == nullptr || !bActive)
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("%s: the GPU subsystem is not configured"), Verb);
			}
			return ESuperSLMPrefixResult::NotConfigured;
		}
		if (!S->IsPooled())
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("%s is unsupported on a dedicated GPU subsystem: GPU prefixes need pooled mode (FSuperSLMGpuRuntimeConfig::SequenceBudget > 0)"), Verb);
			}
			return ESuperSLMPrefixResult::UnsupportedOnGpu;
		}
		return ESuperSLMPrefixResult::Success;
	}

	// The verbs' common tail: the state's verdict, its error copied out when the caller asked.
	ESuperSLMPrefixResult FinishGpuPrefixVerb(ESuperSLMPrefixResult Result, const FString& Error, FString* OutError)
	{
		if (OutError != nullptr)
		{
			*OutError = Error;
		}
		return Result;
	}
}

ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefix(const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	const ESuperSLMPrefixResult Refusal = RefuseGpuPrefixVerb(State, IsGpuBackendActive(), TEXT("CreatePrefix"), OutError);
	if (Refusal != ESuperSLMPrefixResult::Success)
	{
		return Refusal;
	}
	FString Error;
	const ESuperSLMPrefixResult Result = State->CreateGpuPrefix(Tokens, 0, -1, 0, OutPrefix, Error);
	return FinishGpuPrefixVerb(Result, Error, OutError);
}

ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefix(const TArray<int32>& Tokens, const FSuperSLMGpuSchemaHandle& Schema, ESuperSLMSpanKind SpanKind,
	FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	const ESuperSLMPrefixResult Refusal = RefuseGpuPrefixVerb(State, IsGpuBackendActive(), TEXT("CreatePrefix (schema-bound)"), OutError);
	if (Refusal != ESuperSLMPrefixResult::Success)
	{
		return Refusal;
	}
	// Plan rev 1.9 ruling 5: the schema is bound to the prefix (sslm_gpu_prefix_set_schema) before
	// its tokens are fed, and SchemaContent tokens are fed as prefill kind 1. A SchemaContent span
	// needs a schema to walk.
	if (!Schema.IsNone() && State->Schemas.ByIndex(static_cast<size_t>(Schema.Index)) == nullptr)
	{
		return FinishGpuPrefixVerb(ESuperSLMPrefixResult::Malformed,
			FString::Printf(TEXT("Malformed: schema index %d does not exist in the configured artifact"), Schema.Index), OutError);
	}
	if (SpanKind == ESuperSLMSpanKind::SchemaContent && Schema.IsNone())
	{
		return FinishGpuPrefixVerb(ESuperSLMPrefixResult::Malformed,
			TEXT("Malformed: a SchemaContent prefix needs a schema to walk; pass the schema handle, or feed the tokens as a Prompt span"), OutError);
	}
	const int32 Kind = SpanKind == ESuperSLMSpanKind::SchemaContent ? 1 : 0;
	FString Error;
	const ESuperSLMPrefixResult Result = State->CreateGpuPrefix(Tokens, 0, Schema.IsNone() ? -1 : Schema.Index, Kind, OutPrefix, Error);
	return FinishGpuPrefixVerb(Result, Error, OutError);
}

ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefixFrom(const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	const ESuperSLMPrefixResult Refusal = RefuseGpuPrefixVerb(State, IsGpuBackendActive(), TEXT("CreatePrefixFrom"), OutError);
	if (Refusal != ESuperSLMPrefixResult::Success)
	{
		return Refusal;
	}
	// An invalid parent handle (Id 0) names no prefix: the state reads it as unknown (id -1 is never
	// minted).
	FString Error;
	const ESuperSLMPrefixResult Result = State->CreateGpuPrefix(Tokens, Parent.IsValid() ? Parent.Id : -1, -1, 0, OutPrefix, Error);
	return FinishGpuPrefixVerb(Result, Error, OutError);
}

// Plugin paging plan §3.3, §3.4 (K7). Dedicated or unconfigured: there are no GPU prefixes to release.
bool USuperSLMGpuSubsystem::ReleasePrefix(const FSuperSLMPrefix& Prefix, FString& OutError)
{
	if (State == nullptr || !IsGpuBackendActive())
	{
		OutError = TEXT("the GPU subsystem is not configured");
		return false;
	}
	if (!State->IsPooled())
	{
		OutError = TEXT("ReleasePrefix is unsupported on a dedicated GPU subsystem: GPU prefixes need pooled mode (FSuperSLMGpuRuntimeConfig::SequenceBudget > 0)");
		return false;
	}
	return State->ReleaseGpuPrefix(Prefix, OutError);
}

ESuperSLMPrefixPhase USuperSLMGpuSubsystem::GetPrefixPhase(const FSuperSLMPrefix& Prefix) const
{
	if (State == nullptr || !Prefix.IsValid())
	{
		return ESuperSLMPrefixPhase::Invalid;
	}
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = State->Prefixes.Find(Prefix.Id);
	return Entry != nullptr && !Entry->bReleased ? Entry->Phase : ESuperSLMPrefixPhase::Invalid;
}

// The page ledger's count: built and building prefixes and nested children, not released.
int32 USuperSLMGpuSubsystem::GetLivePrefixCount() const
{
	return State != nullptr && State->IsPooled() ? static_cast<int32>(State->Ledger.LivePrefixCount()) : 0;
}

// Plan §3.2: the ledger's regions and counters, and the pool's stats as Configure's cross-check read
// them, all game-thread data. LedgerFaults is this subsystem's own count, kept across re-Configure()
// (PriorLedgerFaults holds the ended states' counts).
FSuperSLMKvPageStats USuperSLMGpuSubsystem::GetKvPageStats() const
{
	FSuperSLMKvPageStats Stats;
	Stats.LedgerFaults = PriorLedgerFaults + (State != nullptr ? static_cast<uint64>(State->Ledger.LedgerFaults()) : 0);
	if (State == nullptr || !State->IsPooled())
	{
		return Stats;
	}
	const FSuperSLMGpuSubsystemState& S = *State;
	const SuperSLMPaging::FPageLedgerStats L = S.Ledger.GetStats();
	auto Region = [&L](SuperSLMPaging::ERegion R)
	{
		FSuperSLMKvRegionStats Out;
		Out.CapacityPages = L.Regions[static_cast<int32>(R)].Capacity;
		Out.ChargedPages = L.Regions[static_cast<int32>(R)].Charged;
		return Out;
	};
	Stats.bPagedMode = true;
	Stats.PagePositions = static_cast<int64>(S.PageGeometry.page_positions);
	Stats.PageBytes = static_cast<int64>(S.PageGeometry.page_bytes);
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

// ---------------------------------------------------------------------------------------------
// FSuperSLMGpuSchemaLookup
// ---------------------------------------------------------------------------------------------

bool FSuperSLMGpuSchemaLookup::LookupByName(const USuperSLMModel& Model, const FString& SchemaName, FSuperSLMGpuSchemaHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMGpuSchemaHandle();
	USuperSLMGpuSubsystem* Gpu = FSuperSLMSelfCheckAccess::FindGpuSubsystemFor(Model);
	const int32 Index = Gpu != nullptr ? FSuperSLMSelfCheckAccess::LookupSchemaIndex(*Gpu, SchemaName) : -2;
	if (Index == -2)
	{
		OutError = FString::Printf(TEXT("model '%s' is not configured into an active GPU subsystem; call USuperSLMGpuSubsystem::Configure() first"), *Model.GetName());
		return false;
	}
	if (Index < 0)
	{
		OutError = FString::Printf(TEXT("schema '%s' is not compiled into model '%s'"), *SchemaName, *Model.GetName());
		return false;
	}
	OutHandle.Index = Index;
	return true;
}

// ---------------------------------------------------------------------------------------------
// FSuperSLMSelfCheckAccess
// ---------------------------------------------------------------------------------------------

USuperSLMGpuSubsystem* FSuperSLMSelfCheckAccess::FindGpuSubsystemFor(const USuperSLMModel& Model)
{
	FScopeLock Lock(&ConfiguredSubsystemsLock());
	for (USuperSLMGpuSubsystem* Gpu : ConfiguredSubsystems())
	{
		if (Gpu != nullptr && Gpu->State != nullptr && Gpu->ConfiguredModel == &Model && Gpu->IsGpuBackendActive())
		{
			return Gpu;
		}
	}
	return nullptr;
}

int32 FSuperSLMSelfCheckAccess::GetNumHiddenLayers(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->NumHiddenLayers : 0;
}

int32 FSuperSLMSelfCheckAccess::LookupSchemaIndex(const USuperSLMGpuSubsystem& Gpu, const FString& SchemaName)
{
	if (Gpu.State == nullptr || Gpu.State->Model == nullptr)
	{
		return -2;
	}
	// A host read of the model handle's own parsed SchemaMasks table, immutable after map, so it
	// is safe beside the submission thread's reads of the same table.
	return SslmGpuSchemaLookupForG5Bridge(Gpu.State->Model, TCHAR_TO_UTF8(*SchemaName));
}

bool FSuperSLMSelfCheckAccess::RunGeneration(USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuDecodePath Path, int32 LayersPerSliceOverride,
	const TArray<int32>& Prompt, const FString& SchemaName, int32 MaxNewTokens, TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.Gpu.SelfCheck", SuperSLMGpuChannel);
	OutTokens.Reset();
	bOutDeadEnd = false;
	FSuperSLMGpuSequence Seq;
	int64 MaxTicks = 0;
	if (!BeginGeneration(Gpu, Path, LayersPerSliceOverride, Prompt, SchemaName, MaxNewTokens, Seq, MaxTicks, OutError))
	{
		return false;
	}
	ON_SCOPE_EXIT { EndGeneration(Gpu, Seq); };
	// Ruling 2026-09-26: Tick() no longer waits, so this loop paces itself -- it sleeps 1 ms while
	// the device is the gate (NextTickGatedOnDevice()) and counts against MaxTicks only the ticks
	// that ran. It ends when the device finishes, or when the row-21 bound on a hung call ends the
	// gate (the next tick then takes the device-loss path and the sequence faults). Sleeping while
	// gated makes a gate that never clears (a hang outside the Layer-1 lock, which the holder check
	// cannot see) spin forever, so the loop also has a wall cap (finding 14) and fails by name.
	// Equal to the headless query runner's kHeadlessWallCapSeconds (SuperSLMQuery.cpp), which is
	// file-local there; keep the two equal.
	constexpr double kRunGenerationWallCapSeconds = 600.0;
	const double WallStart = FPlatformTime::Seconds();
	int64 TicksRun = 0;
	while (TicksRun < MaxTicks)
	{
		if (FPlatformTime::Seconds() - WallStart > kRunGenerationWallCapSeconds)
		{
			OutError = FString::Printf(TEXT("the GPU self-check sequence did not finish within its %.0f s wall cap (%lld of %lld ticks run)"),
				kRunGenerationWallCapSeconds, TicksRun, MaxTicks);
			return false;
		}
		if (NextTickWouldWaitOnDevice(Gpu))
		{
			FPlatformProcess::Sleep(0.001f);
			continue;
		}
		++TicksRun;
		const int32 Step = StepGeneration(Gpu, Seq, OutTokens, bOutDeadEnd, OutError);
		if (Step != 0)
		{
			return Step > 0;
		}
	}
	OutError = TEXT("the GPU self-check sequence did not finish");
	return false;
}

bool FSuperSLMSelfCheckAccess::BeginGeneration(USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuDecodePath Path, int32 LayersPerSliceOverride,
	const TArray<int32>& Prompt, const FString& SchemaName, int32 MaxNewTokens, FSuperSLMGpuSequence& OutSequence, int64& OutMaxTicks, FString& OutError)
{
	OutSequence = FSuperSLMGpuSequence();
	OutMaxTicks = 0;
	FSuperSLMGpuSequence Seq;
	if (Gpu.VendSequence(Seq, Path) != ESuperSLMGpuVendResult::Success)
	{
		OutError = TEXT("no GPU sequence could be vended for the self-check");
		return false;
	}
	bool bKeep = false;
	ON_SCOPE_EXIT
	{
		if (!bKeep)
		{
			Gpu.ReturnSequence(Seq);
		}
	};
	if (FGameSlot* S = Gpu.State->FindSlot(Seq))
	{
		S->LayersPerSliceOverride = LayersPerSliceOverride;
	}
	if (!SchemaName.IsEmpty())
	{
		FSuperSLMGpuSchemaHandle Schema;
		if (!FSuperSLMGpuSchemaLookup::LookupByName(*Gpu.ConfiguredModel, SchemaName, Schema, OutError) ||
			!Gpu.SetSchema(Seq, Schema, OutError))
		{
			return false;
		}
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Prompt;
	Request.MaxNewTokens = MaxNewTokens;
	if (!Gpu.RequestBeginGeneration(Seq, Request).IsValid())
	{
		OutError = Gpu.GetLastLifecycleRequestError();
		return false;
	}
	// Enough ticks for the finest slicing, a whole K of latency per token, and every token, prompt
	// tokens included: the composed path slices each prompt token like a decode token, so at one
	// layer per slice a prompt token costs NumHiddenLayers ticks too. The prompt term was missing
	// before, and a small K hid the gap only while K's own slack covered the prompt: at K = 1 (a
	// whole-token configuration) the one-layer run ran out of ticks before it finished.
	// D-SLM7803 (T-3006 N5): every term summed in int64, so no int32 sum over K or MaxNewTokens can overflow.
	OutMaxTicks = (static_cast<int64>(Prompt.Num()) + static_cast<int64>(MaxNewTokens) + 2)
		* (static_cast<int64>(Gpu.State->NumHiddenLayers) + static_cast<int64>(Gpu.State->Config.K) + 2) + 64;
	OutSequence = Seq;
	bKeep = true;
	return true;
}

int32 FSuperSLMSelfCheckAccess::StepGeneration(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError)
{
	Gpu.Tick(1.0f / 60.0f);
	const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
	if (Phase == ESuperSLMSequencePhase::Complete)
	{
		OutTokens = Gpu.GetGeneratedTokens(Seq);
		return 1;
	}
	if (Phase == ESuperSLMSequencePhase::Faulted)
	{
		OutTokens = Gpu.GetGeneratedTokens(Seq);
		if (Gpu.GetLastDecodeOutcome(Seq) == ESuperSLMDecodeOutcome::SchemaDeadEnd)
		{
			bOutDeadEnd = true;
			return 1;
		}
		OutError = TEXT("the GPU self-check sequence faulted");
		return -1;
	}
	return 0;
}

void FSuperSLMSelfCheckAccess::EndGeneration(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq)
{
	Gpu.ReturnSequence(Seq);
}

bool FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr && Gpu.State->NextTickGatedOnDevice();
}

// ---------------------------------------------------------------------------------------------
// FSuperSLMGpuTestAccess (test-only, U1 round 3; SuperSLMGpuTestAccess.h)
// ---------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS
bool FSuperSLMGpuTestAccess::TearDownWithStatuses(USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuTeardownStatuses& OutStatuses)
{
	OutStatuses = FSuperSLMGpuTeardownStatuses();
	if (Gpu.State == nullptr)
	{
		return false;
	}
	// The same first step TearDown() takes; ReleaseLayer1() records the statuses on the way.
	// TearDown() then finishes as usual (a second Shutdown() is a no-op once the thread stopped).
	Gpu.State->Shutdown();
	OutStatuses = Gpu.State->TeardownRecord;
	Gpu.TearDown();
	return true;
}

bool FSuperSLMGpuTestAccess::GetSaveBindTag(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMLifecycleOpHandle& Save, int64& OutBindTag)
{
	OutBindTag = 0;
	if (Gpu.State == nullptr || !Save.IsValid())
	{
		return false;
	}
	// Row 19 (D-SLM7763): read from the handle's result entry, which outlives the OpLog entry.
	const FSuperSLMGpuSubsystemState::FGpuHandleResult* Entry = Gpu.State->HandleResults.Find(Save.Id);
	if (Entry == nullptr || Entry->Kind != ELifecycleOpKind::Save || !Entry->bHasSaveCounts)
	{
		return false;
	}
	OutBindTag = Entry->BindTagAtSave;
	return true;
}

int64 FSuperSLMGpuTestAccess::GetAwaitedBindTag(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const FGameSlot* S = Gpu.State != nullptr ? static_cast<const FSuperSLMGpuSubsystemState*>(Gpu.State)->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->AwaitedBindTag : 0;
}

int32 FSuperSLMGpuTestAccess::SeedFreeSlotLayersPerSliceOverride(USuperSLMGpuSubsystem& Gpu, int32 LayersPerSliceOverride)
{
	FSuperSLMGpuSubsystemState* St8 = Gpu.State;
	if (St8 == nullptr || !St8->bActive || LayersPerSliceOverride < 0)
	{
		return INDEX_NONE;
	}
	for (int32 I = 0; I < St8->Slots.Num(); ++I)
	{
		FGameSlot& S = St8->Slots[I];
		if (!S.bVended && !S.bWithheld) // AdmitRestoreFront()'s own reservation predicate
		{
			S.LayersPerSliceOverride = LayersPerSliceOverride;
			return I;
		}
	}
	return INDEX_NONE;
}

bool FSuperSLMGpuTestAccess::RunGeneration(USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuDecodePath Path, int32 LayersPerSliceOverride,
	const TArray<int32>& Prompt, const FString& SchemaName, int32 MaxNewTokens, TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError)
{
	return FSuperSLMSelfCheckAccess::RunGeneration(Gpu, Path, LayersPerSliceOverride, Prompt, SchemaName, MaxNewTokens, OutTokens, bOutDeadEnd, OutError);
}

bool FSuperSLMGpuTestAccess::LoadLayer1ModelFacts(const void* ArtifactData, int64 ArtifactSize, FSuperSLMLayer1ModelFacts& OutFacts)
{
	OutFacts = FSuperSLMLayer1ModelFacts();
	if (ArtifactData == nullptr || ArtifactSize <= 0)
	{
		OutFacts.LoadError = TEXT("no artifact bytes");
		return false;
	}
	superslm::SslmModelView View;
	std::string Err;
	superslm::SslmModelStatus Status;
	try
	{
		Status = superslm::SslmModel::Load(static_cast<const uint8_t*>(ArtifactData), static_cast<size_t>(ArtifactSize), View, &Err);
	}
	catch (const std::bad_alloc&)
	{
		OutFacts.LoadError = TEXT("out of memory while loading the artifact view");
		return false;
	}
	OutFacts.LoadStatusName = ANSI_TO_TCHAR(superslm::SslmModelStatusName(Status));
	OutFacts.LoadError = UTF8_TO_TCHAR(Err.c_str());
	if (Status != superslm::SslmModelStatus::Ok)
	{
		return false;
	}
	const superslm::SslmModelConfig& C = View.config;
	OutFacts.HiddenSize = C.hidden_size;
	OutFacts.VocabSize = C.vocab_size;
	OutFacts.NumHiddenLayers = C.num_hidden_layers;
	OutFacts.NumAttentionHeads = C.num_attention_heads;
	OutFacts.NumKeyValueHeads = C.num_key_value_heads;
	OutFacts.HeadDim = C.head_dim;
	OutFacts.IntermediateSize = C.intermediate_size;
	if (const superslm::SslmTensorView* Cos = View.rope_tables.Tensor("cos"))
	{
		OutFacts.bHasRopeCos = true;
		OutFacts.RopeCosElemCount = Cos->elem_count;
	}
	if (const superslm::SslmTensorView* Sin = View.rope_tables.Tensor("sin"))
	{
		OutFacts.bHasRopeSin = true;
		OutFacts.RopeSinElemCount = Sin->elem_count;
	}
	if (const superslm::SslmSectionView* Section = View.Section(superslm::SslmSectionType::SchemaMasks))
	{
		OutFacts.bHasSchemaMasks = true;
		OutFacts.SchemaMasksByteSize = Section->byte_size;
	}
	OutFacts.bLoaded = true;
	return true;
}

FSuperSLMLayer1LayerLayout FSuperSLMGpuTestAccess::ComputeLayer1LayerLayout(uint32 HiddenSize, uint32 KvHiddenSize, uint32 NumKvHeads,
	uint32 NumAttentionHeads, uint32 IntermediateSize, uint32 QWidth)
{
	const superslm_gpu::GpuLayerLayout Layout = superslm_gpu::ComputeLayerLayout(HiddenSize, KvHiddenSize, NumKvHeads, NumAttentionHeads, IntermediateSize, QWidth);
	FSuperSLMLayer1LayerLayout Out;
	Out.Stride = Layout.stride;
	Out.Offsets.Append(Layout.off, UE_ARRAY_COUNT(Layout.off));
	return Out;
}

// Ruling 2026-09-26 (plan §2.5 row 21, implementation items 12 and 13): the slowed device, the
// event and plan logs, the outstanding-job counts, the hitch counters, the pending-event count and
// the paced tick. Game thread.
void FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(USuperSLMGpuSubsystem& Gpu, double Seconds)
{
	if (Gpu.State != nullptr)
	{
		// Copied into each tick job's PostCallDelaySeconds when PlanAndIssue() creates it, so jobs
		// already queued keep the delay they were created with.
		Gpu.State->TestSubmissionDelaySeconds = FMath::Max(0.0, Seconds);
	}
}

// Round 4 (finding 5): enabling refuses, changing nothing, while any vended slot has a pending
// event -- an event created before the log was on would be applied or examined unlogged, and the
// log's recount would no longer equal the hitch counter's increase. Disabling always succeeds.
bool FSuperSLMGpuTestAccess::SetEventLogEnabled(USuperSLMGpuSubsystem& Gpu, bool bEnabled)
{
	FSuperSLMGpuSubsystemState* St8 = Gpu.State;
	if (St8 == nullptr)
	{
		return !bEnabled;
	}
	if (bEnabled)
	{
		for (const FGameSlot& S : St8->Slots)
		{
			if (S.bVended && S.Pending.Num() > 0)
			{
				return false;
			}
		}
		St8->TestEventLog.Reset();
		St8->TestPlanLog.Reset();
		St8->TestTickJobs.Reset();
		St8->bTestHoldNextTickJob = false; // round 3: enabling the logs clears a pending hold request
		// Events planned before the logs were on carry no log index, so a stale index can never
		// point into the fresh log (a free slot's Pending, empty in practice, included).
		for (FGameSlot& S : St8->Slots)
		{
			for (FPendingEvent& E : S.Pending)
			{
				E.LogIndex = INDEX_NONE;
			}
		}
	}
	else
	{
		St8->TestTickJobs.Reset();
	}
	St8->bTestLogsEnabled = bEnabled;
	return true;
}

TArray<FSuperSLMGpuEventLogEntry> FSuperSLMGpuTestAccess::GetEventLog(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->TestEventLog : TArray<FSuperSLMGpuEventLogEntry>();
}

TArray<FSuperSLMGpuPlanLogEntry> FSuperSLMGpuTestAccess::GetPlanLog(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->TestPlanLog : TArray<FSuperSLMGpuPlanLogEntry>();
}

int32 FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->OutstandingTickJobs.load() : 0;
}

bool FSuperSLMGpuTestAccess::ReadAndDrainDebugLayer(FSuperSLMGpuDebugLayerReading& OutReading, bool bInjectWarning)
{
	check(IsInGameThread());
	OutReading = {};
	superslm_gpu::harness::Device& DebugDevice = superslm_gpu::harness::GetDevice();
	OutReading.bEnabled = DebugDevice.debug_layer_enabled;
	OutReading.bQueueAvailable = DebugDevice.debug_info_queue.Get() != nullptr;
	if (!OutReading.bEnabled || !OutReading.bQueueAvailable) { return false; }
	if (bInjectWarning)
	{
		OutReading.bWarningInjected = SUCCEEDED(DebugDevice.debug_info_queue->AddApplicationMessage(
			D3D12_MESSAGE_SEVERITY_WARNING, "SuperSLM K8 InfoQueue warning canary"));
		if (!OutReading.bWarningInjected) { return false; }
	}
	OutReading.DiscardedMessages = static_cast<int64>(DebugDevice.debug_info_queue->GetNumMessagesDiscardedByMessageCountLimit());
	OutReading.ReportableMessages = static_cast<int64>(DebugDevice.DrainDebugLayerMessages());
	return true;
}

int32 FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->OutstandingOtherJobs.load() : 0;
}

int32 FSuperSLMGpuTestAccess::GetGameHitchCount(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->GameHitches : 0;
}

int32 FSuperSLMGpuTestAccess::GetThreadHitchCount(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->ThreadHitches.load() : 0;
}

int32 FSuperSLMGpuTestAccess::GetPendingEventCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const FGameSlot* S = Gpu.State != nullptr ? Gpu.State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->Pending.Num() : 0;
}

int32 FSuperSLMGpuTestAccess::GetOpLogEntryCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const FGameSlot* S = Gpu.State != nullptr ? Gpu.State->FindSlot(Sequence) : nullptr;
	return S != nullptr ? S->OpLog.Num() : 0;
}

bool FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr && Gpu.State->NextTickGatedOnDevice(); // the lock-holder clause included
}

// Round 2 (finding 3): the test-released gate. The next tick job PlanAndIssue() enqueues waits
// after its Layer-1 call, outside the lock, until ReleaseHeldTickJob() (or any teardown) opens
// the gate. The gate is manual-reset: this resets it, so an earlier release does not pass the new
// hold through.
// Round 3 (finding 7): refuses, arming nothing, when no backend is active, a request is already
// pending, or the job it last armed is still outstanding.
bool FSuperSLMGpuTestAccess::HoldNextTickJob(USuperSLMGpuSubsystem& Gpu)
{
	FSuperSLMGpuSubsystemState* St8 = Gpu.State;
	if (!Gpu.IsGpuBackendActive() || St8->TestGateEvent == nullptr || St8->bTestHoldNextTickJob ||
		(St8->TestHeldJob.IsValid() && !St8->TestHeldJob->bDone.load(std::memory_order_acquire)))
	{
		return false;
	}
	St8->TestHeldJob.Reset();
	St8->TestGateEvent->Reset();
	St8->bTestHoldNextTickJob = true;
	return true;
}

// Opens the gate. With nothing held it changes nothing a test can see: the next
// HoldNextTickJob() resets the gate before it is used.
void FSuperSLMGpuTestAccess::ReleaseHeldTickJob(USuperSLMGpuSubsystem& Gpu)
{
	if (Gpu.State != nullptr)
	{
		Gpu.State->ReleaseTestGate();
	}
}

bool FSuperSLMGpuTestAccess::TickWhenDeviceReady(USuperSLMGpuSubsystem& Gpu, float DeltaSeconds, double TimeoutSeconds)
{
	const double Start = FPlatformTime::Seconds();
	while (FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(Gpu))
	{
		if (FPlatformTime::Seconds() - Start >= TimeoutSeconds)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	Gpu.Tick(DeltaSeconds);
	return true;
}

// One-shot Layer-1 status overrides for the next reset and the next restore the submission
// thread runs (see the header). Game thread; the submission thread takes each value once.
void FSuperSLMGpuTestAccess::SetNextResetStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status)
{
	if (Gpu.State != nullptr)
	{
		Gpu.State->TestNextResetStatus.store(static_cast<uint32>(Status), std::memory_order_release);
	}
}

void FSuperSLMGpuTestAccess::SetNextRestoreStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status)
{
	if (Gpu.State != nullptr)
	{
		Gpu.State->TestNextRestoreStatus.store(static_cast<uint32>(Status), std::memory_order_release);
	}
}

// Writes the two game-thread fields FinalizeSchemaBindOp() writes when Layer 1 refuses a held
// bind, appending as it does. Game thread.
bool FSuperSLMGpuTestAccess::SeedDeferredRefusal(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence, const FString& Message)
{
	FGameSlot* S = Gpu.State != nullptr ? Gpu.State->FindSlot(Sequence) : nullptr;
	if (S == nullptr)
	{
		return false;
	}
	S->DeferredRefusal = S->bHasDeferredRefusal ? S->DeferredRefusal + TEXT("; ") + Message : Message;
	S->bHasDeferredRefusal = true;
	return true;
}

// ---------------------------------------------------------------------------------------------
// FSuperSLMPagedGpuTestSeam (plugin paging plan §3.9): the K6 seams, then the K7 ones (prefixes, the
// armed flag, the status overrides, the adopt fault).
// ---------------------------------------------------------------------------------------------

namespace
{
	// RunOnPoolThread()'s access object: thin calls of the engine verbs on the subsystem's own
	// context, model and pool, made on the submission thread inside the seam's job. Holds what the
	// callback creates, and releases it (sequences, then prefixes, each newest first).
	class FGpuPoolThreadAccess final : public FSuperSLMGpuPoolThreadAccess
	{
	public:
		explicit FGpuPoolThreadAccess(FSuperSLMGpuSubsystemState& InState) : S(InState) {}
		virtual ~FGpuPoolThreadAccess() override { ReleaseAll(); }

		virtual int64 ModelCap() const override { return S.Ledger.GetConfig().EngineContextCap; }
		virtual int64 PagePositions() const override { return static_cast<int64>(S.PageGeometry.page_positions); }
		// The engine's stats carry no page size: the page bytes are the geometry the pool was created
		// with and cross-checked against (Configure's job 3).
		virtual int64 PageBytes() const override { return static_cast<int64>(S.PageGeometry.page_bytes); }
		virtual int64 PoolPageCount() const override
		{
			SslmGpuKvPoolStats Stats = {};
			Stats.struct_size = static_cast<uint32_t>(sizeof(Stats));
			const FGpuStatus St = sslm_gpu_kv_pool_stats(S.Pool, &Stats);
			S.NoteLayer1Status(St); // K6 review S2: the seam's calls are submission-thread calls too
			return St == FGpuStatus::SSLM_OK ? static_cast<int64>(Stats.page_count) : -1;
		}
		virtual int64 PagesForBudget(int32 Budget) const override
		{
			return static_cast<int64>(superslm_gpu::kv_pool::ReservationPages(Budget, S.PageGeometry, ModelCap()));
		}
		virtual uint32 TokenDispatches() const override { return S.FullTokenBudget(); }

		virtual SslmGpuStatus CreateBudgeted(int32 Budget) override
		{
			SslmGpuSequenceHandle* Seq = nullptr;
			const FGpuStatus St = sslm_gpu_seq_create_budgeted(S.Ctx, S.Model, S.Pool, Budget, &Seq);
			S.NoteLayer1Status(St);
			if (St == FGpuStatus::SSLM_OK && Seq != nullptr)
			{
				Sequences.Add(Seq);
			}
			return St;
		}
		virtual int32 SequenceCount() const override { return Sequences.Num(); }

		virtual SslmGpuStatus BeginPrefix(int32 Budget) override
		{
			SslmGpuPrefixHandle* Prefix = nullptr;
			const FGpuStatus St = sslm_gpu_prefix_begin(S.Ctx, S.Model, S.Pool, Budget, &Prefix);
			S.NoteLayer1Status(St);
			if (St == FGpuStatus::SSLM_OK && Prefix != nullptr)
			{
				Prefixes.Add(Prefix);
			}
			return St;
		}
		virtual int32 PrefixCount() const override { return Prefixes.Num(); }
		virtual SslmGpuStatus PrefillPrefix(int32 PrefixIndex, int32 Token, int32& OutConsumed) override
		{
			OutConsumed = 0;
			if (!Prefixes.IsValidIndex(PrefixIndex))
			{
				return FGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
			}
			const int32_t Tokens[1] = { static_cast<int32_t>(Token) };
			int32_t Consumed = 0;
			const FGpuStatus St = sslm_gpu_prefix_prefill(S.Ctx, Prefixes[PrefixIndex], Tokens, 1, 0, TokenDispatches(), &Consumed);
			S.NoteLayer1Status(St);
			OutConsumed = static_cast<int32>(Consumed);
			return St;
		}
		virtual SslmGpuStatus FreezePrefix(int32 PrefixIndex) override
		{
			if (!Prefixes.IsValidIndex(PrefixIndex))
			{
				return FGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
			}
			const FGpuStatus St = sslm_gpu_prefix_freeze(S.Ctx, Prefixes[PrefixIndex]);
			S.NoteLayer1Status(St);
			return St;
		}
		virtual SslmGpuStatus AdoptPrefix(int32 SequenceIndex, int32 PrefixIndex) override
		{
			if (!Sequences.IsValidIndex(SequenceIndex) || !Prefixes.IsValidIndex(PrefixIndex))
			{
				return FGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
			}
			const FGpuStatus St = sslm_gpu_seq_adopt_prefix(S.Ctx, Sequences[SequenceIndex], Prefixes[PrefixIndex]);
			S.NoteLayer1Status(St);
			return St;
		}
		// One call at a whole token's dispatches is one token (the bridge embeds, drives the token to
		// full depth and finishes it, or takes its ready shortcut and only finishes).
		virtual SslmGpuStatus DecodeStep(int32 SequenceIndex, int32 TokenToEmbedIfNeeded, int32& OutToken) override
		{
			OutToken = -1;
			if (!Sequences.IsValidIndex(SequenceIndex))
			{
				return FGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
			}
			int32_t Token = -1;
			const FGpuStatus St = SslmGpuSeqDecodeStepForG5Bridge(S.Ctx, Sequences[SequenceIndex], static_cast<int32_t>(TokenToEmbedIfNeeded), TokenDispatches(), &Token);
			S.NoteLayer1Status(St);
			OutToken = static_cast<int32>(Token);
			return St;
		}
		virtual int64 ContextLength(int32 SequenceIndex) const override
		{
			if (!Sequences.IsValidIndex(SequenceIndex))
			{
				return -1;
			}
			const int64_t* Length = SslmGpuSeqHandleContextLengthForBench(Sequences[SequenceIndex]);
			return Length != nullptr ? static_cast<int64>(*Length) : -1;
		}

		virtual void ReleaseAll() override
		{
			for (int32 I = Sequences.Num() - 1; I >= 0; --I)
			{
				S.NoteLayer1Status(sslm_gpu_seq_release(S.Ctx, Sequences[I]));
			}
			Sequences.Reset();
			for (int32 I = Prefixes.Num() - 1; I >= 0; --I)
			{
				S.NoteLayer1Status(sslm_gpu_prefix_release(S.Ctx, Prefixes[I]));
			}
			Prefixes.Reset();
		}

	private:
		FSuperSLMGpuSubsystemState& S;
		TArray<SslmGpuSequenceHandle*> Sequences;
		TArray<SslmGpuPrefixHandle*> Prefixes;
	};

	// The seam's request kinds and the subsystem's job kinds are the same list in the same order.
	EGpuPagedJobKind ToGpuPagedJobKind(ESuperSLMPagedRequestKind Kind)
	{
		switch (Kind)
		{
			case ESuperSLMPagedRequestKind::Reset:         return EGpuPagedJobKind::Reset;
			case ESuperSLMPagedRequestKind::Return:        return EGpuPagedJobKind::Return;
			case ESuperSLMPagedRequestKind::Adopt:         return EGpuPagedJobKind::Adopt;
			case ESuperSLMPagedRequestKind::Restore:       return EGpuPagedJobKind::Restore;
			case ESuperSLMPagedRequestKind::PrefixBuild:   return EGpuPagedJobKind::PrefixBuild;
			case ESuperSLMPagedRequestKind::NestedBuild:   return EGpuPagedJobKind::NestedBuild;
			case ESuperSLMPagedRequestKind::PrefixRelease: return EGpuPagedJobKind::PrefixRelease;
		}
		return EGpuPagedJobKind::Count;
	}

	// The warm slot a vended sequence occupies, read without the non-const FindSlot().
	int32 GpuSlotIndexOf(const FSuperSLMGpuSubsystemState* S, const FSuperSLMGpuSequence& Sequence)
	{
		const int32* Index = S != nullptr ? S->IdToSlot.Find(Sequence.Id) : nullptr;
		return Index != nullptr ? *Index : INDEX_NONE;
	}
}

// Runs Fn on the submission thread, behind every queued job (FIFO), so every job has drained except
// the applies HoldApply() holds on the game thread, whose Layer-1 work is done. A host-only state
// query identifies Submitted handles, and sslm_gpu_ready drains those first (E13). Calling ready
// on an idle handle would revoke its schema-bind eligibility (gpu_1p0.cpp), so idle handles are
// left untouched. Nothing is Submitted when Fn runs.
// K6 review S3: the seam is test-only, so it is not held to the frame-path bound, whose overrun marks
// the subsystem unresponsive (row 21) and leaks the state. Its job carries no bound the overrun check
// can reach, and the game thread waits for it up to kRunOnPoolThreadBoundSeconds itself; past that
// the seam returns false and logs an Error naming it (which fails the running test by name), and the
// subsystem stays usable once the callback returns.
// K7 review N4: Fn may capture the caller's stack, so it never runs after the seam has returned. The
// job and the seam share one state block: past the bound, a callback that has not started is
// abandoned (the job then skips it) and the seam returns false; one that has started is waited for to
// its end, however long, since returning would leave it writing a dead frame.
bool FSuperSLMPagedGpuTestSeam::RunOnPoolThread(USuperSLMGpuSubsystem& Gpu, TFunction<void(FSuperSLMGpuPoolThreadAccess&)> Fn)
{
	constexpr double kRunOnPoolThreadBoundSeconds = 120.0;
	FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !Gpu.IsGpuBackendActive() || !S->IsPooled() || !Fn || S->bUnresponsive.load())
	{
		return false;
	}
	// Held in shared state: a job that outlives the wait never reads this stack frame, and runs Fn only
	// if the seam has not abandoned it (K7 review N4).
	struct FRunOnPoolThreadShared
	{
		TFunction<void(FSuperSLMGpuPoolThreadAccess&)> Fn;
		// 0 pending, 1 running, 2 abandoned, 3 callback done, 4 quiescence failed (callback not run).
		std::atomic<int32> Stage{0};
	};
	TSharedRef<FRunOnPoolThreadShared, ESPMode::ThreadSafe> Held = MakeShared<FRunOnPoolThreadShared, ESPMode::ThreadSafe>();
	Held->Fn = MoveTemp(Fn);
	// As RunSync(): the test gate never holds this job.
	S->ReleaseTestGate();
	S->bTestHoldNextTickJob = false;
	TSharedPtr<FJob> Job = MakeShared<FJob>();
	Job->Custom = [S, Held]()
	{
		for (FThreadSlot& TS : S->ThreadSlots)
		{
			if (TS.Handle != nullptr)
			{
				// This query returns BUSY iff Submitted, and changes neither the handle nor its
				// bind gate. The query's schema-bound output is irrelevant to the state check.
				int32_t SchemaBound = 0;
				const FGpuStatus StateStatus = SslmGpuSeqSchemaBoundForG5Bridge(S->Ctx, TS.Handle, &SchemaBound);
				S->NoteLayer1Status(StateStatus);
				if (StateStatus == FGpuStatus::SSLM_BUSY)
				{
					int32_t Ready = 0;
					FGpuStatus Drained = FGpuStatus::SSLM_OK;
					const FGpuStatus ReadyStatus = sslm_gpu_ready(S->Ctx, TS.Handle, 1, &Ready, &Drained);
					S->NoteLayer1Status(ReadyStatus);
					S->NoteLayer1Status(Drained);
					if (ReadyStatus != FGpuStatus::SSLM_OK || Drained != FGpuStatus::SSLM_OK || Ready == 0)
					{
						UE_LOG(LogSuperSLM, Error, TEXT("RunOnPoolThread: cannot quiesce a Submitted handle (ready call %s, drained %s, ready %d); callback not run."),
							*GpuStatusText(ReadyStatus), *GpuStatusText(Drained), Ready);
						Held->Fn = nullptr;
						Held->Stage.store(4, std::memory_order_release);
						return;
					}
				}
				else if (StateStatus != FGpuStatus::SSLM_OK)
				{
					UE_LOG(LogSuperSLM, Error, TEXT("RunOnPoolThread: cannot read a handle's state (%s); callback not run."), *GpuStatusText(StateStatus));
					Held->Fn = nullptr;
					Held->Stage.store(4, std::memory_order_release);
					return;
				}
			}
		}
		int32 Pending = 0;
		if (!Held->Stage.compare_exchange_strong(Pending, 1, std::memory_order_acq_rel))
		{
			return; // abandoned: the seam returned, and Fn's captures may be gone
		}
		{
			FGpuPoolThreadAccess Access(*S);
			Held->Fn(Access);
			Access.ReleaseAll();
		}
		Held->Fn = nullptr; // its captures go before the seam's frame can
		Held->Stage.store(3, std::memory_order_release);
	};
	Job->CallKind = TEXT("test seam (RunOnPoolThread)");
	Job->BoundSeconds = 1.0e9; // beyond any overrun check: the seam bounds its own wait below
	S->Enqueue(Job);
	const double Deadline = FPlatformTime::Seconds() + kRunOnPoolThreadBoundSeconds;
	bool bLoggedRunning = false;
	while (!Job->bDone.load(std::memory_order_acquire))
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			int32 Pending = 0;
			if (Held->Stage.compare_exchange_strong(Pending, 2, std::memory_order_acq_rel))
			{
				UE_LOG(LogSuperSLM, Error, TEXT("FSuperSLMPagedGpuTestSeam::RunOnPoolThread: the job did not reach the callback within %.0f s; the callback is abandoned (it will not run) and the seam returns false (the subsystem is not marked unresponsive)."),
					kRunOnPoolThreadBoundSeconds);
				return false;
			}
			if (Held->Stage.load(std::memory_order_acquire) >= 3)
			{
				break; // callback finished or quiescence failed; its captures have been released
			}
			if (!bLoggedRunning)
			{
				bLoggedRunning = true;
				UE_LOG(LogSuperSLM, Error, TEXT("FSuperSLMPagedGpuTestSeam::RunOnPoolThread: the callback has run for more than %.0f s; the seam waits for it to return, since it may write the caller's frame."),
					kRunOnPoolThreadBoundSeconds);
			}
		}
		if (S->DoneEvent != nullptr)
		{
			S->DoneEvent->Wait(1);
		}
		else
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
	return !Job->bSkipped && Held->Stage.load(std::memory_order_acquire) == 3;
}

FSuperSLMPagedHold FSuperSLMPagedGpuTestSeam::HoldApply(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind)
{
	FSuperSLMPagedHold Hold;
	const EGpuPagedJobKind JobKind = ToGpuPagedJobKind(Kind);
	if (Gpu.State == nullptr || !Gpu.State->IsPooled() || JobKind == EGpuPagedJobKind::Count)
	{
		return Hold;
	}
	FGpuPagedHold& Armed = Gpu.State->PagedHolds.AddDefaulted_GetRef();
	Armed.Id = GNextGpuPagedHoldId++;
	Armed.Kind = JobKind;
	Hold.Id = Armed.Id;
	return Hold;
}

bool FSuperSLMPagedGpuTestSeam::IsHoldReached(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPagedHold& Hold)
{
	if (Gpu.State == nullptr || !Hold.IsValid())
	{
		return false;
	}
	const FGpuPagedHold* Found = Gpu.State->PagedHolds.FindByPredicate([&Hold](const FGpuPagedHold& H) { return H.Id == Hold.Id; });
	return Found != nullptr && Found->bReached && !Found->bReleased;
}

// Lets the held apply run at the next poll; an armed hold not yet attached is disarmed.
bool FSuperSLMPagedGpuTestSeam::ReleaseHold(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPagedHold& Hold)
{
	if (Gpu.State == nullptr || !Hold.IsValid())
	{
		return false;
	}
	FGpuPagedHold* Found = Gpu.State->PagedHolds.FindByPredicate([&Hold](const FGpuPagedHold& H) { return H.Id == Hold.Id; });
	if (Found == nullptr || Found->bReleased)
	{
		return false;
	}
	Found->bReleased = true;
	return true;
}

FSuperSLMPagedLedgerView FSuperSLMPagedGpuTestSeam::Ledger(const USuperSLMGpuSubsystem& Gpu)
{
	FSuperSLMPagedLedgerView View;
	const FSuperSLMGpuSubsystemState* S = Gpu.State;
	View.LedgerFaults = Gpu.PriorLedgerFaults + (S != nullptr ? static_cast<uint64>(S->Ledger.LedgerFaults()) : 0);
	if (S == nullptr || !S->IsPooled())
	{
		return View;
	}
	const SuperSLMPaging::FPageLedgerStats L = S->Ledger.GetStats();
	View.bConfigured = L.bConfigured;
	View.PoolPages = L.PoolPages;
	for (int32 Region = 0; Region < static_cast<int32>(ESuperSLMPagedRegion::Count); ++Region)
	{
		View.Capacity[Region] = L.Regions[Region].Capacity;
		View.Charged[Region] = L.Regions[Region].Charged;
	}
	View.TotalCharged = S->Ledger.TotalCharged();
	View.LivePrefixCount = static_cast<int32>(L.LivePrefixCount);
	View.GhostCount = static_cast<int32>(L.GhostCount);
	View.InFlightRestores = static_cast<int32>(L.InFlightRestores);
	View.WaitingRestores = static_cast<int32>(L.WaitingRestores);
	return View;
}

int64 FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const int32 Index = GpuSlotIndexOf(Gpu.State, Sequence);
	if (Index == INDEX_NONE || !Gpu.State->IsPooled())
	{
		return -1;
	}
	return static_cast<int64>(Gpu.State->Ledger.SlotHeadroomHold(Index));
}

bool FSuperSLMPagedGpuTestSeam::SequenceHasOpenJob(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const int32 Index = GpuSlotIndexOf(Gpu.State, Sequence);
	return Index != INDEX_NONE && Gpu.State->IsPooled() && Gpu.State->Ledger.SlotHasJob(Index);
}

int32 FSuperSLMPagedGpuTestSeam::SlotIndexOf(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	return GpuSlotIndexOf(Gpu.State, Sequence);
}

bool FSuperSLMPagedGpuTestSeam::IsSlotRecyclePending(const USuperSLMGpuSubsystem& Gpu, int32 SlotIndex)
{
	const FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !S->Slots.IsValidIndex(SlotIndex)) { return false; }
	if (S->Slots[SlotIndex].bReturnPending) { return true; }
	for (const auto& Recycle : S->PendingRecycles)
	{
		if (Recycle.SlotIndex == SlotIndex) { return true; }
	}
	return false;
}

int64 FSuperSLMPagedGpuTestSeam::SlotHolderGeneration(const USuperSLMGpuSubsystem& Gpu, int32 SlotIndex)
{
	const FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !S->Slots.IsValidIndex(SlotIndex))
	{
		return -1;
	}
	return S->Slots[SlotIndex].HolderGeneration;
}

int64 FSuperSLMPagedGpuTestSeam::SequenceContextUsed(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const int32 Index = GpuSlotIndexOf(Gpu.State, Sequence);
	return Index != INDEX_NONE ? Gpu.State->Slots[Index].ContextUsedMirror : -1;
}

bool FSuperSLMPagedGpuTestSeam::ForceUnderCharge(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedChargeKind Kind, uint32 Pages)
{
	if (Gpu.State == nullptr || !Gpu.State->IsPooled())
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
	}
	if (Charge == SuperSLMPaging::EChargeKind::Count)
	{
		return false;
	}
	Gpu.State->Ledger.SetForceUnderChargeForTest(Charge, Pages);
	return true;
#else
	(void)Kind;
	(void)Pages;
	return false;
#endif
}

int64 FSuperSLMPagedGpuTestSeam::JobsPosted(const USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind)
{
	const EGpuPagedJobKind JobKind = ToGpuPagedJobKind(Kind);
	if (Gpu.State == nullptr || JobKind == EGpuPagedJobKind::Count)
	{
		return -1;
	}
	return Gpu.State->JobsPostedByKind[static_cast<int32>(JobKind)];
}

int64 FSuperSLMPagedGpuTestSeam::Layer1BusyCount(const USuperSLMGpuSubsystem& Gpu)
{
	return Gpu.State != nullptr ? Gpu.State->Layer1BusyCount.load() : -1;
}

bool FSuperSLMPagedGpuTestSeam::SetNextConfigurePageCountPerturbation(USuperSLMGpuSubsystem& Gpu, int32 Delta)
{
	Gpu.NextConfigurePageCountPerturbation = Delta;
	return true;
}

bool FSuperSLMPagedGpuTestSeam::SetNextPoolCreateStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status)
{
	Gpu.NextPoolCreateStatusOverride = static_cast<uint32>(Status);
	return true;
}

// --- K7: the prefix and armed seams (plugin paging plan §3.9) ---

namespace
{
	// A pooled subsystem's prefix entry for Prefix, released ones included (a ghost is read through
	// its kept entry); nullptr for an unknown handle and in dedicated mode.
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* GpuSeamPrefix(const FSuperSLMGpuSubsystemState* S, const FSuperSLMPrefix& Prefix)
	{
		if (S == nullptr || !S->IsPooled() || !Prefix.IsValid())
		{
			return nullptr;
		}
		return S->Prefixes.Find(Prefix.Id);
	}
}

int64 FSuperSLMPagedGpuTestSeam::PrefixChargedPages(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
{
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	return Entry != nullptr ? static_cast<int64>(Gpu.State->Ledger.NodeChargedPages(Entry->Node)) : -1;
}

int32 FSuperSLMPagedGpuTestSeam::PrefixDependentCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
{
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	return Entry != nullptr ? static_cast<int32>(Gpu.State->Ledger.DependentCount(Entry->Node)) : -1;
}

bool FSuperSLMPagedGpuTestSeam::IsPrefixGhost(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
{
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	return Entry != nullptr && Gpu.State->Ledger.IsGhost(Entry->Node);
}

bool FSuperSLMPagedGpuTestSeam::IsPrefixReleasePending(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
{
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	return Entry != nullptr && Entry->bReleased && Entry->ReleaseTicket != SuperSLMPaging::InvalidTicket;
}

int32 FSuperSLMPagedGpuTestSeam::SequenceArmed(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence)
{
	const int32 Index = GpuSlotIndexOf(Gpu.State, Sequence);
	if (Index == INDEX_NONE || !Gpu.State->IsPooled())
	{
		return -1;
	}
	return Gpu.State->Slots[Index].bArmedMirror ? 1 : 0;
}

int64 FSuperSLMPagedGpuTestSeam::PrefixBuiltTokens(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
{
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	return Entry != nullptr ? static_cast<int64>(Entry->BuiltTokens) : -1;
}

bool FSuperSLMPagedGpuTestSeam::GetPrefixBuildTurnTicks(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix, TArray<int64>& OutTicks)
{
	OutTicks.Reset();
	const FSuperSLMGpuSubsystemState::FGpuPrefixEntry* Entry = GpuSeamPrefix(Gpu.State, Prefix);
	if (Entry == nullptr)
	{
		return false;
	}
	OutTicks = Entry->TurnTicks;
	return true;
}

bool FSuperSLMPagedGpuTestSeam::GetRetainedPrefixBookkeeping(const USuperSLMGpuSubsystem& Gpu, int32& OutEntries, uint64& OutPayloadBytes)
{
	OutEntries = 0;
	OutPayloadBytes = 0;
	if (Gpu.State == nullptr || !Gpu.State->IsPooled()) { return false; }
	OutEntries = Gpu.State->Prefixes.Num();
	for (const auto& Pair : Gpu.State->Prefixes)
	{
		const auto& Entry = Pair.Value;
		OutPayloadBytes += Entry.Tokens.GetAllocatedSize() + Entry.TurnsInFlight.GetAllocatedSize()
			+ Entry.TurnTicks.GetAllocatedSize() + Entry.FaultMessage.GetAllocatedSize();
	}
	return true;
}

bool FSuperSLMPagedGpuTestSeam::ReadCompletedLayer1Timing(USuperSLMGpuSubsystem& Gpu, double& OutBusyMs)
{
	OutBusyMs = 0.0;
	FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !Gpu.IsGpuBackendActive() || !S->IsPooled() || S->Thread == nullptr) { return false; }
	// RunSync's FIFO job takes Layer1GpuLock. Shared output stays alive if a prior job overruns
	// and the bounded wait returns before this observer executes. No engine API is called here.
	const auto Busy = MakeShared<double, ESPMode::ThreadSafe>(0.0);
	if (!S->RunSync([Busy]() { *Busy = superslm_gpu::LastCallTiming().gpu_busy_ms; }, TEXT("test timing observer"))) { return false; }
	OutBusyMs = *Busy;
	return true;
}

// The one-shot status for the next job of Kind. Return and PrefixRelease have no override: a
// recycle's statuses are the reset's (Reset), and a refused release keeps its handle, which no cell
// constructs; both read false.
bool FSuperSLMPagedGpuTestSeam::SetNextLayer1StatusOverride(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind, SslmGpuStatus Status)
{
	FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !S->IsPooled())
	{
		return false;
	}
	std::atomic<uint32>* Slot = nullptr;
	switch (Kind)
	{
		case ESuperSLMPagedRequestKind::Reset:         Slot = &S->TestNextResetStatus; break;
		case ESuperSLMPagedRequestKind::Restore:       Slot = &S->TestNextRestoreStatus; break;
		case ESuperSLMPagedRequestKind::Adopt:         Slot = &S->TestNextAdoptStatus; break;
		case ESuperSLMPagedRequestKind::PrefixBuild:   Slot = &S->TestNextPrefixBuildStatus; break;
		case ESuperSLMPagedRequestKind::NestedBuild:   Slot = &S->TestNextNestedBuildStatus; break;
		case ESuperSLMPagedRequestKind::Return:
		case ESuperSLMPagedRequestKind::PrefixRelease:
			break;
	}
	if (Slot == nullptr)
	{
		return false;
	}
	Slot->store(static_cast<uint32>(Status), std::memory_order_release);
	return true;
}

bool FSuperSLMPagedGpuTestSeam::ArmAdoptFaultAfterShare(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status)
{
	FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !S->IsPooled())
	{
		return false;
	}
	S->TestAdoptFaultAfterShare.store(static_cast<uint32>(Status), std::memory_order_release);
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

#else // SUPERSLMUNREAL_WITH_GPU == 0: the class exists for UnrealHeaderTool; no instance is created.

bool USuperSLMGpuSubsystem::ShouldCreateSubsystem(UObject* Outer) const { return false; }
void USuperSLMGpuSubsystem::Deinitialize() { Super::Deinitialize(); }
void USuperSLMGpuSubsystem::BeginDestroy() { Super::BeginDestroy(); }
void USuperSLMGpuSubsystem::TearDown() { ConfiguredModel = nullptr; }
USuperSLMModel* USuperSLMGpuSubsystem::GetConfiguredModel() const { return nullptr; }
FSuperSLMGpuConfigureReport USuperSLMGpuSubsystem::Configure(USuperSLMModel*, const FSuperSLMGpuRuntimeConfig&)
{
	FSuperSLMGpuConfigureReport Report;
	Report.Result = ESuperSLMGpuConfigureResult::DeviceUnavailable;
	Report.Message = TEXT("the GPU backend is built on Windows x64 only");
	return Report;
}
void USuperSLMGpuSubsystem::BeginConfigure(USuperSLMModel* Model, const FSuperSLMGpuRuntimeConfig& Config, TUniqueFunction<void(const FSuperSLMGpuConfigureReport&)> OnDone)
{
	const FSuperSLMGpuConfigureReport Report = Configure(Model, Config);
	if (OnDone)
	{
		OnDone(Report);
	}
}
bool USuperSLMGpuSubsystem::IsGpuBackendActive() const { return false; }
ESuperSLMGpuVendResult USuperSLMGpuSubsystem::VendSequence(FSuperSLMGpuSequence& OutSequence, ESuperSLMGpuDecodePath) { OutSequence = FSuperSLMGpuSequence(); return ESuperSLMGpuVendResult::NotConfigured; }
void USuperSLMGpuSubsystem::ReturnSequence(const FSuperSLMGpuSequence&) {}
ESuperSLMGpuDecodePath USuperSLMGpuSubsystem::GetDecodePath(const FSuperSLMGpuSequence&) const { return ESuperSLMGpuDecodePath::OneCall; }
bool USuperSLMGpuSubsystem::SetSchema(const FSuperSLMGpuSequence&, const FSuperSLMGpuSchemaHandle&, FString& OutError) { OutError = TEXT("no GPU backend"); return false; }
FSuperSLMGpuSchemaHandle USuperSLMGpuSubsystem::GetBoundSchema(const FSuperSLMGpuSequence&) const { return FSuperSLMGpuSchemaHandle(); }
uint32 USuperSLMGpuSubsystem::GetSchemaWalkState(const FSuperSLMGpuSequence&) const { return 0xFFFFFFFFu; }
bool USuperSLMGpuSubsystem::IsSchemaAccepting(const FSuperSLMGpuSequence&) const { return false; }
bool USuperSLMGpuSubsystem::SetLayersPerSlice(const FSuperSLMGpuSequence&, int32, FString& OutError) { OutError = TEXT("no GPU backend"); return false; }
int32 USuperSLMGpuSubsystem::GetLayersPerSlice(const FSuperSLMGpuSequence&) const { return 0; }
int32 USuperSLMGpuSubsystem::GetConfiguredK() const { return 0; }
int32 USuperSLMGpuSubsystem::GetConfiguredMinimumK() const { return 0; }
bool USuperSLMGpuSubsystem::SetFixedTickLatency(int32, FString& OutError) { OutError = TEXT("no GPU backend"); return false; }
int32 USuperSLMGpuSubsystem::GetLayersPerTick() const { return 0; }
bool USuperSLMGpuSubsystem::SetLayersPerTick(int32, FString& OutError) { OutError = TEXT("no GPU backend"); return false; }
void USuperSLMGpuSubsystem::RequestConfiguredScheduleRestore() {}
void USuperSLMGpuSubsystem::TryRestoreConfiguredSchedule() {}
bool USuperSLMGpuSubsystem::MapAdapter(const FString&, USuperSLMModel&, FSuperSLMGpuAdapterHandle& OutHandle, FString& OutError) { OutHandle = FSuperSLMGpuAdapterHandle(); OutError = TEXT("no GPU backend"); return false; }
void USuperSLMGpuSubsystem::UnmapAdapter(const FSuperSLMGpuAdapterHandle&) {}
void USuperSLMGpuSubsystem::RequestAdapterSwap(const FSuperSLMGpuSequence&, const FSuperSLMGpuAdapterHandle&) {}
FSuperSLMGpuAdapterHandle USuperSLMGpuSubsystem::GetActiveAdapter(const FSuperSLMGpuSequence&) const { return FSuperSLMGpuAdapterHandle(); }
ESuperSLMSequencePhase USuperSLMGpuSubsystem::GetPhase(const FSuperSLMGpuSequence&) const { return ESuperSLMSequencePhase::Faulted; }
const TArray<int32>& USuperSLMGpuSubsystem::GetGeneratedTokens(const FSuperSLMGpuSequence&) const { static const TArray<int32> Empty; return Empty; }
ESuperSLMDecodeOutcome USuperSLMGpuSubsystem::GetLastDecodeOutcome(const FSuperSLMGpuSequence&) const { return ESuperSLMDecodeOutcome::SequenceNoLongerValid; }
ESuperSLMGpuFaultReason USuperSLMGpuSubsystem::GetLastFaultReason(const FSuperSLMGpuSequence&) const { return ESuperSLMGpuFaultReason::None; }
bool USuperSLMGpuSubsystem::ProbeContextUsable() { return false; }
void USuperSLMGpuSubsystem::Tick(float) { LastTickFrame = GFrameCounter; }
bool USuperSLMGpuSubsystem::TickOncePerFrame(float) { const bool bTicked = LastTickFrame != GFrameCounter; LastTickFrame = GFrameCounter; return bTicked; }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestResetSequence(const FSuperSLMGpuSequence&, ESuperSLMGpuDecodePath) { return FSuperSLMLifecycleOpHandle(); }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestSaveSequence(const FSuperSLMGpuSequence&) { return FSuperSLMLifecycleOpHandle(); }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestBeginGeneration(const FSuperSLMGpuSequence&, const FSuperSLMGenerationRequest&) { return FSuperSLMLifecycleOpHandle(); }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestAdoptPrefix(const FSuperSLMGpuSequence&) { return FSuperSLMLifecycleOpHandle(); }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestRestoreSequence(const TArray<uint8>&, USuperSLMModel*) { return FSuperSLMLifecycleOpHandle(); }
ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetLifecycleOpResult(const FSuperSLMLifecycleOpHandle&) const { return ESuperSLMRestoreResult::Pending; }
int64 USuperSLMGpuSubsystem::GetLifecycleOpResolutionOrdinal(const FSuperSLMLifecycleOpHandle&) const { return -1; }
ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetSaveResult(const FSuperSLMLifecycleOpHandle&, TArray<uint8>& OutBlob) { OutBlob.Reset(); return ESuperSLMRestoreResult::Pending; }
bool USuperSLMGpuSubsystem::ReleaseLifecycleOpHandle(const FSuperSLMLifecycleOpHandle&) { return false; }
int64 USuperSLMGpuSubsystem::GetRetainedResultBytes() const { return 0; }
int32 USuperSLMGpuSubsystem::GetLifecycleHandleEntryCount() const { return 0; }
int32 USuperSLMGpuSubsystem::GetRestoreOpEntryCount() const { return 0; }
ESuperSLMRestoreResult USuperSLMGpuSubsystem::GetRestoreResult(const FSuperSLMLifecycleOpHandle&, FSuperSLMGpuSequence& OutSequence) const { OutSequence = FSuperSLMGpuSequence(); return ESuperSLMRestoreResult::Pending; }
int32 USuperSLMGpuSubsystem::GetPendingLifecycleOperationCount(const FSuperSLMGpuSequence&) const { return 0; }
FString USuperSLMGpuSubsystem::GetLastLifecycleRequestError() const { return FString(); }
double USuperSLMGpuSubsystem::GetLastGpuBusyMs() const { return 0.0; }
FSuperSLMGpuTimingSnapshot USuperSLMGpuSubsystem::GetGpuTimingSnapshot() const { return {}; }
double USuperSLMGpuSubsystem::GetLastHostFinishMs() const { return 0.0; }
double USuperSLMGpuSubsystem::GetTimeToFirstTokenMs(const FSuperSLMGpuSequence&) const { return -1.0; }
int32 USuperSLMGpuSubsystem::GetHitchCount() const { return 0; }
int64 USuperSLMGpuSubsystem::GetGpuDispatchCount() const { return 0; }
int32 USuperSLMGpuSubsystem::GetPoolFreeCount() const { return 0; }
int32 USuperSLMGpuSubsystem::GetPoolOccupiedCount() const { return 0; }
int32 USuperSLMGpuSubsystem::GetWithheldSlotCount() const { return 0; }
int32 USuperSLMGpuSubsystem::GetMappedAdapterCount() const { return 0; }
FString USuperSLMGpuSubsystem::GetShaderDirectory() const { return FString(); }
int64 USuperSLMGpuSubsystem::GetPreMapLocalVideoMemoryBytes() const { return -1; }
int64 USuperSLMGpuSubsystem::GetPostMapLocalVideoMemoryBytes() const { return -1; }
bool USuperSLMGpuSubsystem::GetSaveTokenCounts(const FSuperSLMLifecycleOpHandle&, int32& OutObservedAtSave, int32& OutProducedAtSave) const { OutObservedAtSave = -1; OutProducedAtSave = -1; return false; }
bool USuperSLMGpuSubsystem::IsDeviceHeadActive() const { return false; }
FString USuperSLMGpuSubsystem::GetDeviceHeadStatus() const { return FString(); }
int64 USuperSLMGpuSubsystem::GetDeclaredGpuResidencyBytes() const { return 0; }
int64 USuperSLMGpuSubsystem::GetLocalVideoMemoryUsageBytes() const { return 0; }

bool FSuperSLMGpuSchemaLookup::LookupByName(const USuperSLMModel&, const FString&, FSuperSLMGpuSchemaHandle& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMGpuSchemaHandle();
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}

USuperSLMGpuSubsystem* FSuperSLMSelfCheckAccess::FindGpuSubsystemFor(const USuperSLMModel&) { return nullptr; }
int32 FSuperSLMSelfCheckAccess::GetNumHiddenLayers(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMSelfCheckAccess::LookupSchemaIndex(const USuperSLMGpuSubsystem&, const FString&) { return -2; }
bool FSuperSLMSelfCheckAccess::RunGeneration(USuperSLMGpuSubsystem&, ESuperSLMGpuDecodePath, int32, const TArray<int32>&, const FString&, int32,
	TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError)
{
	OutTokens.Reset();
	bOutDeadEnd = false;
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}
bool FSuperSLMSelfCheckAccess::BeginGeneration(USuperSLMGpuSubsystem&, ESuperSLMGpuDecodePath, int32, const TArray<int32>&, const FString&, int32,
	FSuperSLMGpuSequence& OutSequence, int64& OutMaxTicks, FString& OutError)
{
	OutSequence = FSuperSLMGpuSequence();
	OutMaxTicks = 0;
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}
int32 FSuperSLMSelfCheckAccess::StepGeneration(USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&, TArray<int32>&, bool&, FString& OutError)
{
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return -1;
}
void FSuperSLMSelfCheckAccess::EndGeneration(USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) {}
bool FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(const USuperSLMGpuSubsystem&) { return false; }

#if WITH_DEV_AUTOMATION_TESTS
bool FSuperSLMGpuTestAccess::TearDownWithStatuses(USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuTeardownStatuses& OutStatuses) { OutStatuses = FSuperSLMGpuTeardownStatuses(); Gpu.TearDown(); return false; }
bool FSuperSLMGpuTestAccess::GetSaveBindTag(const USuperSLMGpuSubsystem&, const FSuperSLMLifecycleOpHandle&, int64& OutBindTag) { OutBindTag = 0; return false; }
int64 FSuperSLMGpuTestAccess::GetAwaitedBindTag(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return 0; }
int32 FSuperSLMGpuTestAccess::SeedFreeSlotLayersPerSliceOverride(USuperSLMGpuSubsystem&, int32) { return INDEX_NONE; }
bool FSuperSLMGpuTestAccess::RunGeneration(USuperSLMGpuSubsystem&, ESuperSLMGpuDecodePath, int32, const TArray<int32>&, const FString&, int32,
	TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError)
{
	OutTokens.Reset();
	bOutDeadEnd = false;
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}
bool FSuperSLMGpuTestAccess::LoadLayer1ModelFacts(const void*, int64, FSuperSLMLayer1ModelFacts& OutFacts)
{
	OutFacts = FSuperSLMLayer1ModelFacts();
	OutFacts.LoadError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}
FSuperSLMLayer1LayerLayout FSuperSLMGpuTestAccess::ComputeLayer1LayerLayout(uint32, uint32, uint32, uint32, uint32, uint32) { return FSuperSLMLayer1LayerLayout(); }
void FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(USuperSLMGpuSubsystem&, double) {}
bool FSuperSLMGpuTestAccess::SetEventLogEnabled(USuperSLMGpuSubsystem&, bool) { return false; }
TArray<FSuperSLMGpuEventLogEntry> FSuperSLMGpuTestAccess::GetEventLog(const USuperSLMGpuSubsystem&) { return TArray<FSuperSLMGpuEventLogEntry>(); }
TArray<FSuperSLMGpuPlanLogEntry> FSuperSLMGpuTestAccess::GetPlanLog(const USuperSLMGpuSubsystem&) { return TArray<FSuperSLMGpuPlanLogEntry>(); }
int32 FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(const USuperSLMGpuSubsystem&) { return 0; }
bool FSuperSLMGpuTestAccess::ReadAndDrainDebugLayer(FSuperSLMGpuDebugLayerReading& OutReading, bool) { OutReading = {}; return false; }
int32 FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMGpuTestAccess::GetGameHitchCount(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMGpuTestAccess::GetThreadHitchCount(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMGpuTestAccess::GetPendingEventCount(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return 0; }
int32 FSuperSLMGpuTestAccess::GetOpLogEntryCount(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return 0; }
bool FSuperSLMGpuTestAccess::TickWhenDeviceReady(USuperSLMGpuSubsystem& Gpu, float DeltaSeconds, double) { Gpu.Tick(DeltaSeconds); return true; }
bool FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(const USuperSLMGpuSubsystem&) { return false; }
bool FSuperSLMGpuTestAccess::HoldNextTickJob(USuperSLMGpuSubsystem&) { return false; }
void FSuperSLMGpuTestAccess::ReleaseHeldTickJob(USuperSLMGpuSubsystem&) {}
void FSuperSLMGpuTestAccess::SetNextResetStatusOverride(USuperSLMGpuSubsystem&, SslmGpuStatus) {}
void FSuperSLMGpuTestAccess::SetNextRestoreStatusOverride(USuperSLMGpuSubsystem&, SslmGpuStatus) {}
bool FSuperSLMGpuTestAccess::SeedDeferredRefusal(USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&, const FString&) { return false; }

// Plugin paging plan §3.9 (K6): no GPU subsystem is ever configured here, so every seam reads
// "not configured".
bool FSuperSLMPagedGpuTestSeam::RunOnPoolThread(USuperSLMGpuSubsystem&, TFunction<void(FSuperSLMGpuPoolThreadAccess&)>) { return false; }
FSuperSLMPagedHold FSuperSLMPagedGpuTestSeam::HoldApply(USuperSLMGpuSubsystem&, ESuperSLMPagedRequestKind) { return FSuperSLMPagedHold(); }
bool FSuperSLMPagedGpuTestSeam::IsHoldReached(const USuperSLMGpuSubsystem&, const FSuperSLMPagedHold&) { return false; }
bool FSuperSLMPagedGpuTestSeam::ReleaseHold(USuperSLMGpuSubsystem&, const FSuperSLMPagedHold&) { return false; }
FSuperSLMPagedLedgerView FSuperSLMPagedGpuTestSeam::Ledger(const USuperSLMGpuSubsystem&) { return FSuperSLMPagedLedgerView(); }
int64 FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return -1; }
bool FSuperSLMPagedGpuTestSeam::SequenceHasOpenJob(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return false; }
bool FSuperSLMPagedGpuTestSeam::IsSlotRecyclePending(const USuperSLMGpuSubsystem&, int32) { return false; }
int32 FSuperSLMPagedGpuTestSeam::SlotIndexOf(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return INDEX_NONE; }
int64 FSuperSLMPagedGpuTestSeam::SlotHolderGeneration(const USuperSLMGpuSubsystem&, int32) { return -1; }
int64 FSuperSLMPagedGpuTestSeam::SequenceContextUsed(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return -1; }
bool FSuperSLMPagedGpuTestSeam::ForceUnderCharge(USuperSLMGpuSubsystem&, ESuperSLMPagedChargeKind, uint32) { return false; }
int64 FSuperSLMPagedGpuTestSeam::JobsPosted(const USuperSLMGpuSubsystem&, ESuperSLMPagedRequestKind) { return -1; }
int64 FSuperSLMPagedGpuTestSeam::Layer1BusyCount(const USuperSLMGpuSubsystem&) { return -1; }
bool FSuperSLMPagedGpuTestSeam::SetNextConfigurePageCountPerturbation(USuperSLMGpuSubsystem& Gpu, int32 Delta) { Gpu.NextConfigurePageCountPerturbation = Delta; return true; }
bool FSuperSLMPagedGpuTestSeam::SetNextPoolCreateStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status) { Gpu.NextPoolCreateStatusOverride = static_cast<uint32>(Status); return true; }
int64 FSuperSLMPagedGpuTestSeam::PrefixChargedPages(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return -1; }
int32 FSuperSLMPagedGpuTestSeam::PrefixDependentCount(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return -1; }
bool FSuperSLMPagedGpuTestSeam::IsPrefixGhost(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return false; }
int32 FSuperSLMPagedGpuTestSeam::SequenceArmed(const USuperSLMGpuSubsystem&, const FSuperSLMGpuSequence&) { return -1; }
int64 FSuperSLMPagedGpuTestSeam::PrefixBuiltTokens(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return -1; }
bool FSuperSLMPagedGpuTestSeam::GetPrefixBuildTurnTicks(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&, TArray<int64>& OutTicks) { OutTicks.Reset(); return false; }
bool FSuperSLMPagedGpuTestSeam::GetRetainedPrefixBookkeeping(const USuperSLMGpuSubsystem&, int32& OutEntries, uint64& OutPayloadBytes) { OutEntries = 0; OutPayloadBytes = 0; return false; }
bool FSuperSLMPagedGpuTestSeam::ReadCompletedLayer1Timing(USuperSLMGpuSubsystem&, double& OutBusyMs) { OutBusyMs = 0.0; return false; }
bool FSuperSLMPagedGpuTestSeam::SetNextLayer1StatusOverride(USuperSLMGpuSubsystem&, ESuperSLMPagedRequestKind, SslmGpuStatus) { return false; }
bool FSuperSLMPagedGpuTestSeam::ArmAdoptFaultAfterShare(USuperSLMGpuSubsystem&, SslmGpuStatus) { return false; }
#endif // WITH_DEV_AUTOMATION_TESTS

// Plugin paging plan (K6): the pooled verbs, with no GPU backend to configure.
ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefix(const TArray<int32>&, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	if (OutError != nullptr)
	{
		*OutError = TEXT("CreatePrefix: the GPU backend is built on Windows x64 only");
	}
	return ESuperSLMPrefixResult::NotConfigured;
}
ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefix(const TArray<int32>&, const FSuperSLMGpuSchemaHandle&, ESuperSLMSpanKind, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	if (OutError != nullptr)
	{
		*OutError = TEXT("CreatePrefix: the GPU backend is built on Windows x64 only");
	}
	return ESuperSLMPrefixResult::NotConfigured;
}
ESuperSLMPrefixResult USuperSLMGpuSubsystem::CreatePrefixFrom(const FSuperSLMPrefix&, const TArray<int32>&, FSuperSLMPrefix& OutPrefix, FString* OutError)
{
	OutPrefix = FSuperSLMPrefix();
	if (OutError != nullptr)
	{
		*OutError = TEXT("CreatePrefixFrom: the GPU backend is built on Windows x64 only");
	}
	return ESuperSLMPrefixResult::NotConfigured;
}
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestAdoptPrefix(const FSuperSLMGpuSequence&, const FSuperSLMPrefix&) { return FSuperSLMLifecycleOpHandle(); }
bool USuperSLMGpuSubsystem::ReleasePrefix(const FSuperSLMPrefix&, FString& OutError)
{
	OutError = TEXT("ReleasePrefix: the GPU backend is built on Windows x64 only");
	return false;
}
ESuperSLMPrefixPhase USuperSLMGpuSubsystem::GetPrefixPhase(const FSuperSLMPrefix&) const { return ESuperSLMPrefixPhase::Invalid; }
int32 USuperSLMGpuSubsystem::GetLivePrefixCount() const { return 0; }
FSuperSLMLifecycleOpHandle USuperSLMGpuSubsystem::RequestRestoreSequence(const TArray<uint8>&, USuperSLMModel*, const FSuperSLMPrefix&) { return FSuperSLMLifecycleOpHandle(); }
FSuperSLMKvPageStats USuperSLMGpuSubsystem::GetKvPageStats() const
{
	FSuperSLMKvPageStats Stats;
	Stats.LedgerFaults = PriorLedgerFaults;
	return Stats;
}

#endif // SUPERSLMUNREAL_WITH_GPU

// Plugin paging plan §3.5 "Handle ids stay unique" (K6): the handle counter lives in the
// per-Configure state and restarts with it, so a new state starts above the highest tombstoned id,
// and a fresh operation can never take a tombstone's id. With no tombstones it starts at 1. Every
// platform (the member is declared on every platform; elsewhere there are never tombstones).
int64 USuperSLMGpuSubsystem::FirstHandleIdAfterTombstones() const
{
	int64 Highest = 0;
	for (const int64 Id : LifecycleTombstones)
	{
		Highest = FMath::Max(Highest, Id);
	}
	return Highest + 1;
}

// Outside the GPU guard: the restore's status mapping is pure host code over Layer 1's portable
// status enum, so a cell reads it on every platform, with or without a device.
#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMGpuStatusMapping.h"

ESuperSLMRestoreResult FSuperSLMGpuTestAccess::RestoreFailureResult(SslmGpuStatus Status, bool bSchemaCrossCheckFailed)
{
	return SuperSLMGpuStatusMapping::ToRestoreFailureResult(Status, bSchemaCrossCheckFailed);
}

// Plugin paging plan §3.7 (K6): the mode-aware mapping the restore itself calls, pure, on every platform.
ESuperSLMRestoreResult FSuperSLMPagedGpuTestSeam::RestoreFailureResult(SslmGpuStatus Status, bool bPooled, bool bSchemaCrossCheckFailed)
{
	return SuperSLMGpuStatusMapping::ToRestoreFailureResult(Status, bPooled, bSchemaCrossCheckFailed);
}
#endif // WITH_DEV_AUTOMATION_TESTS

#if SUPERSLM_WITH_PREFIX_CACHE
// Precache U6 wiring edit 5: the GPU prefix-cache method bodies, after the GPU guard (SuperSLMGpuSubsystemPrefixCache.h).
#include "SuperSLMGpuSubsystemPrefixCache.inl"
#endif
