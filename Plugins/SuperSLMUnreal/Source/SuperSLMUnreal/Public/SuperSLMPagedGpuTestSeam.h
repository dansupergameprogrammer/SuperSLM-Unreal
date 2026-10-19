#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSaveRestoreTypes.h"

class USuperSLMGpuSubsystem;

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMGpuTypes.h"
#include "SuperSLMPagedTestSeam.h" // FSuperSLMPagedLedgerView, FSuperSLMPagedHold, the request and charge kinds
#include "SuperSLMSequenceTypes.h"
#include "Templates/Function.h"
#include "superslm/gpu_1p0.h" // SslmGpuStatus

// The GPU subsystem's paged-KV test seams (plugin paging plan §3.9; steps K5 to K7). The twins of
// FSuperSLMPagedTestSeam (SuperSLMPagedTestSeam.h), which is CPU only. Compiled only under
// WITH_DEV_AUTOMATION_TESTS, so they exist in Development builds and not in Shipping. Game thread
// only, except FSuperSLMGpuPoolThreadAccess, which is used only inside a RunOnPoolThread() callback.
//
// The differences from the CPU seam, and why:
//   - No HoldStart(). §3.9: the GPU's single FIFO submission thread orders a free before any draw
//     enqueued after it, so the enqueue window cannot open there (cell 1.5 is CPU only).
//   - The access object reaches the GPU verbs, not the CPU ones, and adds the engine-only twin of
//     cell 10.4 (b): a prefix built and a pooled sequence decoded with no plugin code in between.
//   - SequenceContextUsed() and SequenceArmed() read the slot's mirror (§3.4 "The armed flag"),
//     which cells 1.6, 9.4, 10.4 and 10.6 grade against the engine's own figures.
//   - ArmAdoptFaultAfterShare() reproduces §3.4's documented post-state of an adopt whose tail-page
//     upload fails after the share (the engine's throw is unreachable from the plugin).
//   - The Configure seams (page-count perturbation, pool-create status) arm the NEXT Configure(),
//     so they are held on the subsystem object, not in the per-Configure state.
//   - RestoreFailureResult() is the mode-aware twin of FSuperSLMGpuTestAccess::RestoreFailureResult()
//     (§3.7: K0R's arms in dedicated mode, §3.7's table in pooled mode). Pure, on every platform.

// What a RunOnPoolThread() callback may do with the GPU subsystem's own model, context and pool.
// Each method is a thin call of the engine verb named beside it, made on the submission thread
// under the Layer-1 lock with every job drained and sslm_gpu_ready called first (§3.9, E13). It
// exists for the same reason as the CPU's FSuperSLMPoolThreadAccess: the engine's symbols are
// exported from no shared library, so a test module reaches them through this vtable. Sequences and
// prefixes a callback creates are released by ReleaseAll(), and by the seam when the callback
// returns (sequences before prefixes).
class SUPERSLMUNREAL_API FSuperSLMGpuPoolThreadAccess
{
public:
	virtual ~FSuperSLMGpuPoolThreadAccess() = default;

	// The engine's own figures.
	virtual int64 ModelCap() const = 0;                   // the model's context_cap (Ce)
	virtual int64 PagePositions() const = 0;              // sslm_gpu_kv_pool_stats: page_positions (B)
	virtual int64 PageBytes() const = 0;                  // the pool's page size: ComputeGpuPageGeometry's page_bytes (SslmGpuKvPoolStats has no page-size field)
	virtual int64 PoolPageCount() const = 0;              // sslm_gpu_kv_pool_stats: page count
	virtual int64 PagesForBudget(int32 Budget) const = 0; // the engine's reservation for Budget (ReservationPages); 0 outside [1, Ce]
	virtual uint32 TokenDispatches() const = 0;           // one whole token: DispatchesPerLayer(has_qk_norm) x num_hidden_layers

	// The fill probe (§3.9): sslm_gpu_seq_create_budgeted(ctx, model, pool, Budget, &seq). The
	// sequence is held until ReleaseAll().
	virtual SslmGpuStatus CreateBudgeted(int32 Budget) = 0;
	virtual int32 SequenceCount() const = 0;

	// Cell 10.4 (b)'s engine-only twin. Prefix and sequence indices count the prefixes and
	// sequences this callback created, from 0, in creation order.
	virtual SslmGpuStatus BeginPrefix(int32 Budget) = 0;                                  // sslm_gpu_prefix_begin(ctx, model, pool, Budget, &prefix)
	virtual int32 PrefixCount() const = 0;
	virtual SslmGpuStatus PrefillPrefix(int32 PrefixIndex, int32 Token, int32& OutConsumed) = 0; // sslm_gpu_prefix_prefill(kind 0, &Token, count 1, TokenDispatches())
	virtual SslmGpuStatus FreezePrefix(int32 PrefixIndex) = 0;                            // sslm_gpu_prefix_freeze
	virtual SslmGpuStatus AdoptPrefix(int32 SequenceIndex, int32 PrefixIndex) = 0;        // sslm_gpu_seq_adopt_prefix
	// SslmGpuSeqDecodeStepForG5Bridge(ctx, seq, TokenToEmbedIfNeeded, TokenDispatches(), &out),
	// repeated while the bridge reports a token still in progress, so one call is one token. The
	// bridge's own ready shortcut decides whether TokenToEmbedIfNeeded is embedded.
	virtual SslmGpuStatus DecodeStep(int32 SequenceIndex, int32 TokenToEmbedIfNeeded, int32& OutToken) = 0;
	// The engine's context_length for the sequence (SslmGpuSeqHandleContextLengthForBench); -1 when
	// that accessor is not reachable from this build (§13 item 13, K0's box leg (a)).
	virtual int64 ContextLength(int32 SequenceIndex) const = 0;

	// sslm_gpu_seq_release on every sequence this callback created, newest first, then
	// sslm_gpu_prefix_release on every prefix it created, newest first.
	virtual void ReleaseAll() = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMPagedGpuTestSeam
{
	// Runs Fn on the submission thread with the subsystem quiesced: every job drained except those
	// held by HoldApply() (their Layer-1 work is done), and sslm_gpu_ready called for Submitted
	// handles. Idle handles are left untouched so their schema-bind eligibility is preserved. Blocks the game
	// thread until Fn returns. Pooled mode only: false, with Fn not run, when the subsystem is not
	// configured or is dedicated (a dedicated subsystem has no pool to probe).
	static bool RunOnPoolThread(USuperSLMGpuSubsystem& Gpu, TFunction<void(FSuperSLMGpuPoolThreadAccess&)> Fn);

	// Arms one hold on the next job of Kind admitted after this call (each call arms one more, taken
	// in admission order): the job finishes its Layer-1 work, then its apply is held until
	// ReleaseHold(). An invalid hold when the subsystem is not pooled. Configure() and
	// Deinitialize() release every hold and disarm every armed one before they tear the pool down
	// (cell 1.11).
	static FSuperSLMPagedHold HoldApply(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind);
	static bool IsHoldReached(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPagedHold& Hold);
	static bool ReleaseHold(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPagedHold& Hold);

	// The ledger and its per-node and per-slot readings, as on the CPU. -1 (or false) for an
	// unknown handle, and in dedicated mode.
	static FSuperSLMPagedLedgerView Ledger(const USuperSLMGpuSubsystem& Gpu);
	static int64 PrefixChargedPages(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix);
	static int32 PrefixDependentCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix);
	static bool IsPrefixGhost(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix);
	// A release admitted but not yet applied on the game thread, including after its worker job
	// finished. False for an unknown prefix, one not released, or a release whose apply completed.
	static bool IsPrefixReleasePending(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix);
	static int64 SequenceHeadroomHold(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	static bool SequenceHasOpenJob(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	// A logical return or recycle still awaiting its game-thread apply on this physical slot.
	// Read-only driving observation; dependency/page assertions grade the applied result separately.
	static bool IsSlotRecyclePending(const USuperSLMGpuSubsystem& Gpu, int32 SlotIndex);
	static int32 SlotIndexOf(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	static int64 SlotHolderGeneration(const USuperSLMGpuSubsystem& Gpu, int32 SlotIndex);

	// The slot's mirror (§3.4): ContextUsed, and bL1ReadyArmed (1 armed, 0 not). -1 for a sequence
	// that is not live, and in dedicated mode for the armed flag.
	static int64 SequenceContextUsed(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	static int32 SequenceArmed(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);

	// The tokens a prefix's build has fed so far (one per build turn, §3.4 UP-11); -1 for an unknown
	// handle. And the GPU ticks at which each build turn of Prefix ran, in order (cell 3.4's
	// cadence; the sequences' own come from FSuperSLMGpuTestAccess::GetPlanLog()). False for an
	// unknown handle.
	static int64 PrefixBuiltTokens(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix);
	static bool GetPrefixBuildTurnTicks(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix, TArray<int64>& OutTicks);
	// Read-only retained map population and variable-capacity token/turn/fault buffers.
	static bool GetRetainedPrefixBookkeeping(const USuperSLMGpuSubsystem& Gpu, int32& OutEntries, uint64& OutPayloadBytes);
	// FIFO observer under the Layer-1 lock: reads the core timing without submitting, draining,
	// counting work, or publishing a plugin timing sample.
	static bool ReadCompletedLayer1Timing(USuperSLMGpuSubsystem& Gpu, double& OutBusyMs);

	// §3.9 ForceUnderCharge(), as on the CPU. False when the subsystem is not pooled.
	static bool ForceUnderCharge(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedChargeKind Kind, uint32 Pages);

	// A one-shot Layer-1 status for the next job of Kind, as on the CPU: that job returns Status in
	// place of its Layer-1 call, and the engine is left as the call would have left it on that
	// refusal (cells 1.6, 1.12 and 11.2's GPU legs). False when the subsystem is not pooled.
	static bool SetNextLayer1StatusOverride(USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind, SslmGpuStatus Status);

	// §3.4 "A fault after the share": the next adopt runs the real sslm_gpu_seq_adopt_prefix, then
	// resets the sequence through the engine, then reports Status as the adopt's result (cells 1.6
	// and 10.6). False when the subsystem is not pooled.
	static bool ArmAdoptFaultAfterShare(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status);

	// The number of submission-thread jobs of Kind posted since the last Configure(): what "refused
	// before any job" is graded against (K1 coverage audit B2). -1 when the subsystem is not
	// configured.
	static int64 JobsPosted(const USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind);

	// The SSLM_BUSY returns any Layer-1 call of this subsystem has seen since the last Configure()
	// (cell 3.3); -1 when the subsystem is not configured.
	static int64 Layer1BusyCount(const USuperSLMGpuSubsystem& Gpu);

	// Arm the NEXT Configure(), and are spent by it. The geometry cross-check's seam (§3.4, cell 5):
	// the page count the plugin computed is perturbed by Delta before it is compared with the pool's
	// stats. The pool-create seam: sslm_gpu_kv_pool_create returns Status in place of its call
	// (cell 5's ALLOCATION_FAILED leg, which no adapter reaches on demand). Return true when armed.
	static bool SetNextConfigurePageCountPerturbation(USuperSLMGpuSubsystem& Gpu, int32 Delta);
	static bool SetNextPoolCreateStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status);

	// The mode-aware restore mapping (§3.7): with bPooled false, K0R's dedicated arms
	// (HOLDER_MODE_MISMATCH -> KvMismatch, KV_POOL_EXHAUSTED -> OutOfMemory); with bPooled true,
	// §3.7's (KvModeMismatch, KvPagesExhausted). Every other status reads as
	// FSuperSLMGpuTestAccess::RestoreFailureResult() does. Pure: no device, every platform.
	static ESuperSLMRestoreResult RestoreFailureResult(SslmGpuStatus Status, bool bPooled, bool bSchemaCrossCheckFailed);
};

#endif // WITH_DEV_AUTOMATION_TESTS
