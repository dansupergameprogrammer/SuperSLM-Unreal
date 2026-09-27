#pragma once

#include "CoreMinimal.h"
#include "SuperSLMGpuTypes.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSequenceTypes.h"
#include "superslm/gpu_1p0.h" // SslmGpuStatus, for RestoreFailureResult()

class USuperSLMGpuSubsystem;

// The statuses Layer 1 returned to the GPU subsystem's teardown (ReleaseLayer1(), plan §9 R-S1l
// (j)), by status name ("SSLM_OK", "SSLM_ADAPTER_HAS_BOUND_SEQUENCES", ...). Recorded on every
// teardown, diagnostic only; read through FSuperSLMGpuTestAccess::TearDownWithStatuses().
struct SUPERSLMUNREAL_API FSuperSLMGpuTeardownStatuses
{
	bool bRecorded = false;                // ReleaseLayer1() ran
	TArray<int32> SequenceSlots;           // pooled slot index of each live sequence released, in order
	TArray<FString> SequenceUnbindStatuses;  // sslm_gpu_seq_bind_adapter(nullptr), per entry of SequenceSlots
	TArray<FString> SequenceReleaseStatuses; // sslm_gpu_seq_release, per entry of SequenceSlots
	TArray<int64> AdapterIds;              // FSuperSLMGpuAdapterHandle ids unmapped, in order
	TArray<FString> AdapterUnmapStatuses;  // sslm_gpu_adapter_unmap, per entry of AdapterIds
	FString ModelUnmapStatus;              // empty when no model was mapped
	FString ContextDestroyStatus;          // empty when no context existed
};

// Plain values Layer 1's own superslm::SslmModel::Load reads from an artifact's bytes (U1 round 4,
// R-S2j): what an independent residency oracle needs, with no plugin arithmetic in between. Plain
// data, so a caller never constructs a Layer-1 type the runtime module does not export.
struct SUPERSLMUNREAL_API FSuperSLMLayer1ModelFacts
{
	bool bLoaded = false;          // SslmModel::Load returned SslmModelStatus::Ok
	FString LoadStatusName;        // SslmModelStatusName() of Load's status
	FString LoadError;             // Load's own error text, when it gave one
	// SslmModelView::config
	uint32 HiddenSize = 0;
	uint32 VocabSize = 0;
	uint32 NumHiddenLayers = 0;
	uint32 NumAttentionHeads = 0;
	uint32 NumKeyValueHeads = 0;
	uint32 HeadDim = 0;
	uint32 IntermediateSize = 0;
	// SslmModelView::rope_tables.Tensor("cos"/"sin")->elem_count
	bool bHasRopeCos = false;
	uint64 RopeCosElemCount = 0;
	bool bHasRopeSin = false;
	uint64 RopeSinElemCount = 0;
	// SslmModelView::Section(SslmSectionType::SchemaMasks)->byte_size
	bool bHasSchemaMasks = false;
	uint64 SchemaMasksByteSize = 0;
};

// superslm_gpu::ComputeLayerLayout's result (gpu_port.h GpuLayerLayout), as plain values.
struct SUPERSLMUNREAL_API FSuperSLMLayer1LayerLayout
{
	uint32 Stride = 0;             // GpuLayerLayout::stride
	TArray<uint32> Offsets;        // GpuLayerLayout::off[0..64]
};

// Ruling 2026-09-26 (plan §2.5 row 21, implementation item 13; R-S2a's arms): one per
// FPendingEvent the GPU subsystem creates while logging is on, carried ones included, as
// FSuperSLMGpuTestAccess::GetEventLog() returns it. It holds the readings the examination decided
// on, not the decision: a test recounts hitches from these fields itself.
struct SUPERSLMUNREAL_API FSuperSLMGpuEventLogEntry
{
	int64  SequenceId = 0;                 // FSuperSLMGpuSequence::Id of the sequence it belongs to
	bool   bPrefill = false;               // false: a token event
	bool   bCarried = false;               // rebuilt by a restore's finalize (FinalizeRestoreOp())
	int64  RequestTick = 0;                // T: the tick the token (or the prompt's last token) was requested
	int64  ApplyTick = 0;                  // max(T + K, the tick after it was planned)
	double RequestWallSeconds = 0.0;       // FPlatformTime::Seconds() at its request
	double RequestSimSeconds = 0.0;        // the subsystem's summed DeltaSeconds at its request
	bool   bExamined = false;              // examined at ApplyTick (the fields below are set)
	bool   bJobDoneAtExamination = false;  // the one bDone load the decision used
	double WallAtExamination = 0.0;        // that tick's TickWall (one reading per tick)
	double SimAtExamination = 0.0;         // that tick's SimSeconds
	int32  TickJobsAheadAtExamination = 0; // unfinished tick jobs enqueued before its job
	int64  RemovedTick = -1;               // TickIndex when applied or dropped; -1 pending
	bool   bDropped = false;               // removed without being applied
	double RemovedWallSeconds = 0.0;       // when it was removed (the tick's TickWall when applied)
};

// One per Tick() (its Plan step) while logging is on, as FSuperSLMGpuTestAccess::GetPlanLog()
// returns it. Each array holds each sequence id at most once.
struct SUPERSLMUNREAL_API FSuperSLMGpuPlanLogEntry
{
	int64 Tick = 0;
	int32 OutstandingTickJobsAtPlan = 0;     // the reading bCapBinding used
	bool  bCapBinding = false;               // OutstandingTickJobsAtPlan >= K: no new token starts
	TArray<int64> IssuedSequenceIds;         // any action issued for the sequence
	TArray<int64> StartedSequenceIds;        // a new token started (the boundary kinds)
	TArray<int64> HeldSequenceIds;           // bHeldThisTick, whatever Plan then did
	TArray<int64> InFlightAtPlanSequenceIds; // composed PlanLayerPos > 0 at Plan's start
};

#if WITH_DEV_AUTOMATION_TESTS
// Test-only access to GPU-subsystem internals the public surface does not carry (U1 round 3).
// Read-only except TearDownWithStatuses(), which runs the subsystem's own teardown. None of it
// changes scheduling or output. Defined in SuperSLMGpuSubsystem.cpp; a friend of
// USuperSLMGpuSubsystem.
struct SUPERSLMUNREAL_API FSuperSLMGpuTestAccess
{
	// R-S1l (j): runs Gpu's own teardown (the path Deinitialize() takes) and returns the statuses
	// Layer 1 gave ReleaseLayer1(). False, with OutStatuses empty, when Gpu is not configured.
	static bool TearDownWithStatuses(USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuTeardownStatuses& OutStatuses);

	// R-S2k (vi): the adapter bind tag the submission thread had applied when a save's job ran
	// at its token boundary (FThreadSlot::AppliedBindTag; 0 = no tagged bind has run on that
	// slot). False until that save's job has run, and for any handle that is not a save.
	static bool GetSaveBindTag(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMLifecycleOpHandle& Save, int64& OutBindTag);

	// R-S2k (vi): the tag a sequence's most recent RequestAdapterSwap() minted (0 = none), the
	// one its confirmation waits for.
	static int64 GetAwaitedBindTag(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);

	// R-S1l (h): FSuperSLMSelfCheckAccess::RunGeneration, exported for the editor test module --
	// the self-check's own GPU generation seam (LayersPerSliceOverride > 0 caps the composed
	// path's layers per slice). Unchanged behaviour.
	static bool RunGeneration(USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuDecodePath Path, int32 LayersPerSliceOverride,
		const TArray<int32>& Prompt, const FString& SchemaName, int32 MaxNewTokens,
		TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError);

	// R-S1l (h), M-33 (D-SLM7769, U1 round 12): writes LayersPerSliceOverride on the slot a restore
	// admitted now would reserve -- the first slot that is neither vended nor withheld, the same
	// predicate AdmitRestoreFront() uses -- and returns that slot's index. INDEX_NONE, writing
	// nothing, when Gpu is not configured, no such slot exists, or LayersPerSliceOverride < 0.
	// Seeds the stale per-user value a restore's finalize must reset (row 20, H8). Unused, it
	// changes nothing; used, the value is read only once the slot is held again, and both ways a
	// slot is held again -- a vend and a restore's finalize -- reset it by value first.
	static int32 SeedFreeSlotLayersPerSliceOverride(USuperSLMGpuSubsystem& Gpu, int32 LayersPerSliceOverride);

	// R-S2j (U1 round 4): superslm::SslmModel::Load over the artifact bytes, called directly, and
	// the values it read. Returns bLoaded. Win64 build only (false elsewhere).
	static bool LoadLayer1ModelFacts(const void* ArtifactData, int64 ArtifactSize, FSuperSLMLayer1ModelFacts& OutFacts);

	// R-S2j (U1 round 4): superslm_gpu::ComputeLayerLayout(hidden_size, kv_hidden_size,
	// num_kv_heads, num_attention_heads, intermediate_size, q_width), called directly. Win64
	// build only (empty elsewhere).
	static FSuperSLMLayer1LayerLayout ComputeLayer1LayerLayout(uint32 HiddenSize, uint32 KvHiddenSize, uint32 NumKvHeads,
		uint32 NumAttentionHeads, uint32 IntermediateSize, uint32 QWidth);

	// --- Ruling 2026-09-26 (plan §2.5 row 21, implementation item 13): the GPU Tick() never
	// waits. For dev automation only; unused, none of it changes anything. Each acts on Gpu's
	// current configured state, so a later Configure() starts from none of it (no delay, logs off
	// and empty). Game thread.

	// Each tick job created from now on sleeps Seconds on the submission thread after its Layer-1
	// work -- after the Layer-1 lock and the holder record are released, and before its completion
	// time and bDone -- so the sleep never counts against a row-21 bound. The value is copied into
	// the job when Plan creates it, so jobs already queued keep theirs. Lifecycle and RunSync jobs
	// never sleep. 0 (the default) turns it off. No-op when Gpu is not configured.
	static void SetSubmissionDelaySeconds(USuperSLMGpuSubsystem& Gpu, double Seconds);

	// Turns the event log and the plan log on or off together. Turning them on clears both;
	// turning them off stops recording and keeps what was recorded. Both are written on the game
	// thread only. Returns false, changing nothing, when enabling while any vended sequence has a
	// pending event (round 4): enable the logs before any generation is requested, or after every
	// pending event has applied. Disabling always succeeds and returns true.
	static bool SetEventLogEnabled(USuperSLMGpuSubsystem& Gpu, bool bEnabled);

	// Every event created since the logs were last turned on, in creation order.
	static TArray<FSuperSLMGpuEventLogEntry> GetEventLog(const USuperSLMGpuSubsystem& Gpu);

	// One entry per tick whose Plan step ran while the logs were on, in tick order.
	static TArray<FSuperSLMGpuPlanLogEntry> GetPlanLog(const USuperSLMGpuSubsystem& Gpu);

	// Tick jobs (Plan's own) enqueued on the submission thread and not yet done: the global cap's
	// measure. 0 when Gpu is not configured.
	static int32 GetOutstandingTickJobCount(const USuperSLMGpuSubsystem& Gpu);

	// Every other job (lifecycle, blob release, RunSync, a save's force-finish) enqueued and not
	// yet done. 0 when Gpu is not configured.
	static int32 GetOutstandingOtherJobCount(const USuperSLMGpuSubsystem& Gpu);

	// The game-thread hitch count alone (T+K misses and device lateness, per event). 0 when Gpu is
	// not configured.
	static int32 GetGameHitchCount(const USuperSLMGpuSubsystem& Gpu);

	// The submission thread's slice-hitch count alone. 0 when Gpu is not configured.
	static int32 GetThreadHitchCount(const USuperSLMGpuSubsystem& Gpu);

	// The sequence's pending (unapplied) event count, or 0 for a sequence that is not live.
	static int32 GetPendingEventCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);

	// The number of entries in the sequence's per-sequence request log (OpLog), resolved entries
	// not yet compacted included, or 0 for a sequence that is not live. Lets a cell see that the
	// log is compacted behind its front rather than growing while the sequence is held.
	static int32 GetOpLogEntryCount(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);

	// Sleeps 1 ms at a time while the device is the gate for the next tick
	// (NextTickGatedOnDevice(): an event due by then with an unfinished job, a vended sequence's
	// admitted queued op with an unfinished job, or K or more tick jobs outstanding so the cap
	// would bind), then calls Gpu.Tick(DeltaSeconds) once and returns true. Returns false without
	// ticking when the gate has not opened within TimeoutSeconds.
	static bool TickWhenDeviceReady(USuperSLMGpuSubsystem& Gpu, float DeltaSeconds, double TimeoutSeconds);

	// --- Round 2 (ruling 2026-09-26, round 2, item 13). Game thread.

	// NextTickGatedOnDevice(), the lock-holder clause included: true when the device, not the tick,
	// is what the next Tick() would wait on for progress. False when Gpu is not configured. A loop
	// bounded by a wall deadline sleeps while this is true instead of ticking.
	static bool IsNextTickGatedOnDevice(const USuperSLMGpuSubsystem& Gpu);

	// The next tick job created waits after its Layer-1 call -- outside the Layer-1 lock and the
	// holder record, after any SetSubmissionDelaySeconds() sleep, before it reads done -- until
	// ReleaseHeldTickJob() or any teardown releases it. The gate never counts against a row-21
	// bound. A RunSync() (MapAdapter(), UnmapAdapter(), ProbeContextUsable(), teardown) releases
	// it first, so the gate never holds one. Returns true when armed; false, arming nothing, when
	// no GPU backend is active, a request is already pending, or the job it last armed is still
	// outstanding (round 3). Shutdown(), a re-Configure(), SetEventLogEnabled(true) and (round 5)
	// every RunSync() clear a pending request: a hold armed before a RunSync is cancelled, not
	// passed through unheld, and the caller re-arms.
	static bool HoldNextTickJob(USuperSLMGpuSubsystem& Gpu);

	// Releases the held tick job; a no-op when nothing is held.
	static void ReleaseHeldTickJob(USuperSLMGpuSubsystem& Gpu);

	// The result a GPU restore resolves when its Layer-1 step failed with Status
	// (SuperSLMGpuStatusMapping::ToRestoreFailureResult(), the one function the restore itself
	// calls). bSchemaCrossCheckFailed is the plugin's own schema cross-check after the restore.
	// Pure: needs no device and no configured subsystem, on every platform.
	static ESuperSLMRestoreResult RestoreFailureResult(SslmGpuStatus Status, bool bSchemaCrossCheckFailed);

	// --- One-shot Layer-1 status overrides. Game thread. Each is taken once, by the next call of
	// its kind on the submission thread, with an atomic exchange back to SSLM_OK; SSLM_OK clears
	// it. Unset, nothing changes. No-op when Gpu has no state, and on a build without the GPU.

	// The next sslm_gpu_seq_reset the submission thread runs returns Status instead of calling
	// Layer 1 (the sequence is then left as Layer 1 had it). That is the next reset of ANY kind:
	// a caller's RequestResetSequence(), or the unbinding reset of a return or a refused restore.
	static void SetNextResetStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status);

	// The next sslm_gpu_seq_restore the submission thread runs returns Status, with no sequence,
	// instead of calling Layer 1, so a real restore takes its failure path end to end
	// (FinalizeRestoreOp()'s refusal and recycle). SSLM_GPU_ALLOCATION_FAILED gives OutOfMemory.
	static void SetNextRestoreStatusOverride(USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status);

	// Seeds the deferred refusal a held schema bind leaves when Layer 1 refuses it: the sequence's
	// next admitted generation is faulted by name with Message. Appends to a refusal already held,
	// as the bind path does. False, changing nothing, when Sequence is not a live GPU sequence.
	static bool SeedDeferredRefusal(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence, const FString& Message);
};
#endif
