#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S1 (the plan §4: "Save/restore hands the game opaque bytes
// bound to the artifact hash ... The plugin tags each blob with its backend and with the
// Layer-1 tag and commit, rejects a cross-backend restore itself, and never asserts a magic
// value"). CPU-only at L2-S1 (R-S1c); the GPU value exists in the enum now so the wrapper's own
// tag format does not change shape when L2-S2 adds the GPU backend (§9's own note on this exact
// cell: "GPU arm L2-S2").
enum class ESuperSLMBackend : uint8
{
	CPU = 0,
	GPU = 1,
};

// Every case R-S1c names, as one closed result set -- never a bare bool, so "restore failed"
// always says which of Layer 1's own rejection (surfaced by name, §4) or the plugin's own
// backend-tag mismatch (never reaching Layer 1 at all) it was.
//
// D-SLM7423 (T-2805 round 9): this is the ONE shared result type Restore, Save, Reset and Adopt
// Prefix all return -- none of the four owns a distinct type. D-SLM7421/D-SLM7408: every one of
// them is now a QUEUED lifecycle operation, so this enum gains two values that only the QUEUED
// shape needs: Pending (GetLifecycleOpResult()/GetSaveResult()'s own "not drained yet" value) and
// SequenceQueueFull (SSLM_SEQUENCE_QUEUE_FULL, the one synchronous refusal a caller can still get
// back from the call that queues -- before anything is queued and before Layer 1 is asked, plan
// §5 item 3). Named to match the GPU backend's own declaration (maintainer ruling D-SLM7457), so both
// backends share one surface.
// The GPU backend adds two further values of its own (UnsupportedOnGpu, SaveRefused; T-2826
// round 5, D-SLM7457), appended after the shared queued values so no shared value moves.
enum class ESuperSLMRestoreResult : uint8
{
	Success,
	BackendMismatch, // the plugin's OWN tag check (§4); refused before sslm_seq_restore is called
	ModelMismatch,   // SSLM_RESTORE_MODEL_MISMATCH
	KvMismatch,      // SSLM_RESTORE_KV_MISMATCH
	ResidualLost,    // SSLM_RESTORE_RESIDUAL_LOST (§2.3 change #1; legacy 'SSB2'/'SSB3' blobs only)
	Malformed,       // any other rejection (truncated/garbage blob, unrecognized magic)
	NotConfigured,   // the subsystem has no configured model and pool to restore into
	PoolExhausted,   // every warm sequence is vended; a restore needs a free one (§4)
	AdapterUnavailable, // the blob was saved with an adapter bound, and no adapter with that
	                    // artifact hash is mapped against the configured model (D-SLM7341)
	SequenceQueueFull, // SSLM_SEQUENCE_QUEUE_FULL -- this sequence's own queue already holds
	                   // MaxQueuedOperationsPerSequence entries (D-SLM7421/D-SLM7429, plan §5 item 3)
	Pending,         // GetLifecycleOpResult()/GetSaveResult(): queued, not yet drained (D-SLM7421)

	// --- Added by T-2826 round 5's GPU per-sequence queue build, 2026-09-19 ---

	// D-SLM7457 ruling (1): the GPU backend has no Layer-1 prefix-adopt verb at v1.5.0
	// (grepped gpu_1p0.h/gpu_port.h; sslm_seq_adopt_prefix takes the CPU-only sslm_seq
	// handle type) -- RequestAdoptPrefix() queues and admits in strict per-sequence
	// arrival order like every other op, but its own admission never calls Layer 1 at
	// all: it resolves directly to this named, backend-unsupported result. Distinct from
	// SequenceQueueFull (refused before queueing, at request time) -- this is a normal
	// admission that queued, waited its turn, and then resolved to "there is nothing this
	// backend can do." No cell in this round's Coverage Model exercises this admitted
	// path (AdoptPrefix is used only as the collision arm's refused-by-capacity fifth
	// request); flagged in the round's own handoff as production behavior no cell pins.
	UnsupportedOnGpu,

	// A queued GPU SaveSequence (RequestSaveSequence) declined to produce a blob because the
	// sequence was Faulted. Since the re-pin to Layer 1 1.7.0 the GPU 'SLM5' blob carries the
	// schema binding and its walk, so a schema-bound sequence saves at any token boundary
	// (plan §2.5 row 6, D-SLM7666). GetLastLifecycleRequestError() carries the human-readable
	// reason; the sequence itself is left untouched by a SaveRefused result.
	SaveRefused,

	// --- Added at U1, the re-pin to Layer 1 1.7.0 (plan §2.5 row 23, D-SLM7714) ---

	// The blob's wrapper names a Layer-1 tag other than the one this build compiled
	// (SuperSLMSaveBlob::CompiledLayer1Tag()). Refused at request time on both backends,
	// before anything is queued; the message names both tags. Saves do not carry across a
	// change of the plugin's Layer-1 pin (plan §4, §12 decision 13). Appended last so every
	// earlier value keeps its number.
	Layer1Mismatch,

	// --- Added at U1 after the first full run (plan §2.5 row 19, D-SLM7763) ---

	// GetSaveResult() on a handle whose blob the first successful read already moved out to the
	// caller: the save succeeded, and no bytes are returned again (the blob is the caller's now).
	// Appended last so every earlier value keeps its number; L2-S3's Blueprint mirror gains it.
	Consumed,

	// --- Added for 1.0: two behaviours Layer 1 requires of the GPU backend (gpu_1p0.h). Both are
	// appended last so every earlier value keeps its number; the Blueprint mirror gains both, in
	// this order. The CPU backend never returns either.

	// What a GPU RequestBeginGeneration() reads at its turn when the sequence is not Idle then,
	// which only a reset queued ahead of it and refused by Layer 1 can cause: the refused reset
	// left the sequence Faulted (a sequence runs one generation at a time, D-SLM7946). Nothing on
	// the sequence is changed; it needs a reset that succeeds, or to be returned and a save
	// restored, which gives a new sequence (a restore never clears the faulted one). A request
	// refused at the call gets no handle, so it never reads this. Requiring a reset after every
	// fault is the plugin's own rule: Layer 1 requires one only after an allocation failure
	// (gpu_1p0.h:233-237), and lets a SchemaDeadEnd be retried (gpu_1p0.h:564-579).
	ResetRequired,

	// GPU restore: Layer 1 ran out of GPU memory (SSLM_GPU_ALLOCATION_FAILED); the save is intact
	// and the restore may be retried later.
	OutOfMemory,
};

namespace SuperSLM
{
	// Test-only support (declared here because R-S1c needs it and no other consumer should):
	// overwrites an already-saved, well-formed blob's OWN backend tag in place. R-S1c's cross-
	// backend refusal cell ("A GPU-tagged blob offered to the CPU restore -> refused by the
	// plugin's backend tag") needs a well-formed-but-differently-tagged blob to offer
	// RestoreSequence(), and L2-S1 has no real GPU backend to save one from (L2-S2). This lets
	// the test construct that input without knowing the wrapper's exact byte layout, which is
	// this module's own implementation detail, never something a test should encode by hand.
	// No production code path calls this. Namespaced with ComputeArtifactDigest
	// (SuperSLMIntegrity.h) -- the module's existing home for narrow, test-serving bridges.
	SUPERSLMUNREAL_API bool DebugOverwriteBackendTag(TArray<uint8>& Blob, ESuperSLMBackend NewBackend);
}
