#pragma once

#include "CoreMinimal.h"

// L2-S2 (the plan §5, §10.3; Coverage Model §9, cells
// R-S2a-R-S2g; the red-suite record §4 "GPU API declared").
// The GPU backend's own vended-handle and configuration shapes, distinct types from
// the CPU backend's (SuperSLMSequenceTypes.h, SuperSLMAdapterHandle.h) by design: "a
// handle from one surface is never valid on the other" (plan §2.1) is a compile-time
// property here, not a runtime check a caller could get wrong -- a CPU FSuperSLMSequence
// passed to a GPU-subsystem method is a type error, not a mismatched-backend refusal
// discovered at runtime.
//
// Plain C++ -- no UPROPERTY/UENUM(BlueprintType) anywhere in this L2-S2 API surface,
// matching L2-S1's own convention (SuperSLMSequenceTypes.h): the Blueprint surface is
// L2-S3 scope (plan §10.4), and the unified one-Blueprint-facing-interface-over-two-
// backends design (D-SLM3534) is realized there, not here. This module declares the
// GPU backend's own C++ entry points only.
//
// FSuperSLMGenerationRequest, ESuperSLMSequencePhase, ESuperSLMDecodeOutcome, and
// ESuperSLMRestoreResult (all already backend-neutral shapes -- none names "CPU" or
// carries a CPU-specific field) are REUSED from the existing runtime module's
// SuperSLMSequenceTypes.h / SuperSLMSaveRestoreTypes.h rather than re-declared here,
// so a caller comparing GPU output to CPU output (R-S2a: "tokens equal the CPU
// backend's for the same session") compares the SAME result types on both sides
// without a conversion step.

// One vended GPU sequence. Wraps the GPU subsystem's own private lookup key -- never a
// raw SslmGpuSequenceHandle*, the same "the ABI handle never crosses the module's own
// public surface" discipline FSuperSLMSequence (CPU) already holds. A DISTINCT type
// from FSuperSLMSequence: the two handle spaces are never interchangeable (plan §2.1),
// and keeping them separate types means a caller cannot mix them even by accident. 0 is
// never a valid vended handle.
struct SUPERSLMUNREAL_API FSuperSLMGpuSequence
{
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }
	bool operator==(const FSuperSLMGpuSequence& Other) const { return Id == Other.Id; }
	bool operator!=(const FSuperSLMGpuSequence& Other) const { return Id != Other.Id; }
};

// Chosen at vend or reset time and NEVER switched mid-sequence (plan §5, D-SLM7244,
// D-SLM7256 rule 3: "A sequence uses one path ... from creation or reset onward and
// never switches, because a direct finish leaves Layer 1's flag set and the bridge
// would later misread it"). USuperSLMGpuSubsystem::VendSequence()/ResetSequence() are
// the only two places this value is ever set.
enum class ESuperSLMGpuDecodePath : uint8
{
	// SslmGpuSeqDecodeStepForG5Bridge, a whole token per call (plan §5 "One-call path").
	OneCall = 0,
	// sslm_gpu_seq_embed_token -> one or more sslm_decode_step_gpu at the tick's
	// dispatch_budget -> SslmGpuSeqFinishTokenForG5Bridge (plan §5 "Composed (sliced)
	// path"), with the duplicate-KV "primed" guard (D-SLM7256 rules 1-2).
	Composed = 1,
};

enum class ESuperSLMGpuVendResult : uint8
{
	Success,
	PoolExhausted, // the GPU sequence pool's own free list is empty (mirrors
	               // ESuperSLMVendResult::PoolExhausted's CPU-side precedent, §4/R-S1f)
	NotConfigured, // Configure() has not yet succeeded
};

// Distinguishes the two SSLM_DEVICE_LOST causes gpu_1p0.h's own status-enum comment
// documents (read at v1.5.0 source: "Two distinct causes resolve to this ONE status,
// indistinguishable at the ABI") at the PLUGIN's own level, where the distinction is
// resolvable by probing the context afterward (plan §5: "probe the context once, and
// on a terminal result tear the GPU backend down and fall back to the CPU backend").
// R-S2c's own cell needs exactly this three-way split: a healthy per-call rejection
// that costs the caller nothing further, a rejection the probe confirms recoverable,
// and a confirmed-terminal one.
enum class ESuperSLMGpuFaultReason : uint8
{
	None,
	// A healthy rejection at a saturated context_cap or an out-of-domain per-sequence
	// input (SSLM_SEQUENCE_REJECTED, SSLM_GPU_ALLOCATION_FAILED -- mapped here without a probe,
	// per SuperSLM 1.8.0's contract that the device and context stay valid and only the faulted
	// sequence must be reset -- or a SSLM_DEVICE_LOST that the plugin's own probe
	// (below) confirmed does not indicate device removal) -- the device and every OTHER
	// sequence stay usable; only this sequence's own in-flight step is lost.
	RecoverablePerSequenceRejection,
	// The plugin's probe (ProbeContextUsable()) confirmed the context itself is gone
	// (GetDeviceRemovedReason() non-S_OK, or a fault the probe's own retry could not
	// clear) -- terminal for every sequence on this context. The GPU backend falls back
	// to CPU (plan §5); "no cause is reported to gameplay, because the ABI carries
	// none" -- this enum is a PLUGIN-INTERNAL diagnostic, not a gameplay-facing result.
	TerminalDeviceLost,
};

// GPU-side adapter handle -- distinct from the CPU backend's FSuperSLMAdapterHandle
// (SuperSLMAdapterHandle.h), because they wrap DIFFERENT Layer-1 ABI types
// (SslmGpuAdapterHandle* vs the CPU sslm_adapter opaque id) and neither restores from
// the other's save blob (plan §2.1: "neither save blob restores into the other").
struct SUPERSLMUNREAL_API FSuperSLMGpuAdapterHandle
{
	int64 Id = 0;
	bool IsValid() const { return Id != 0; }
};

