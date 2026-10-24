#pragma once

#include "SuperSLMSaveRestoreTypes.h"
#include "superslm/gpu_1p0.h"

// L2-S2 (plan §10.3 item 7): the GPU status mapping is an exhaustive switch over SslmGpuStatus in
// guard G5's form (SuperSLMStatusMapping.h). Layer 1 publishes no X-macro list for the GPU enum,
// so every enumerator has its own written arm; there is no default arm, and the unhandled-
// enumerator diagnostics are promoted to errors for this switch only, so a Layer-1 pin that
// appends a GPU status fails this plugin's compile until the new value is named here.
namespace SuperSLMGpuStatusMapping
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(error : 4062) // switch of enum with no default label, an enumerator unhandled
#pragma warning(error : 4061) // enumerator unhandled in a switch that has a default
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif

	inline const char* ToDiagnosticText(SslmGpuStatus Status)
	{
		switch (Status)
		{
			case SslmGpuStatus::SSLM_OK: return "SSLM_OK";
			case SslmGpuStatus::SSLM_DISPATCH_BUDGET_TOO_SMALL: return "SSLM_DISPATCH_BUDGET_TOO_SMALL";
			case SslmGpuStatus::SSLM_BUSY: return "SSLM_BUSY";
			case SslmGpuStatus::SSLM_CONTEXT_HAS_LIVE_HANDLES: return "SSLM_CONTEXT_HAS_LIVE_HANDLES";
			case SslmGpuStatus::SSLM_MODEL_HAS_LIVE_SEQUENCES: return "SSLM_MODEL_HAS_LIVE_SEQUENCES";
			case SslmGpuStatus::SSLM_ADAPTER_MODEL_MISMATCH: return "SSLM_ADAPTER_MODEL_MISMATCH";
			case SslmGpuStatus::SSLM_ADAPTER_BASE_HASH_MISMATCH: return "SSLM_ADAPTER_BASE_HASH_MISMATCH";
			case SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH: return "SSLM_SEQUENCE_KV_BUFFER_MISMATCH";
			case SslmGpuStatus::SSLM_DEVICE_LOST: return "SSLM_DEVICE_LOST";
			case SslmGpuStatus::SSLM_BATCH_BUDGET_EXHAUSTED: return "SSLM_BATCH_BUDGET_EXHAUSTED";
			case SslmGpuStatus::SSLM_TOKEN_ID_OUT_OF_RANGE: return "SSLM_TOKEN_ID_OUT_OF_RANGE";
			case SslmGpuStatus::SSLM_SEQUENCE_REJECTED: return "SSLM_SEQUENCE_REJECTED";
			case SslmGpuStatus::SSLM_RESTORE_MODEL_MISMATCH: return "SSLM_RESTORE_MODEL_MISMATCH";
			case SslmGpuStatus::SSLM_MODEL_HAS_LIVE_ADAPTERS: return "SSLM_MODEL_HAS_LIVE_ADAPTERS";
			case SslmGpuStatus::SSLM_ADAPTER_HAS_BOUND_SEQUENCES: return "SSLM_ADAPTER_HAS_BOUND_SEQUENCES";
			case SslmGpuStatus::SSLM_GPU_SHADER_BINARY_STALE: return "SSLM_GPU_SHADER_BINARY_STALE";
			case SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED: return "SSLM_GPU_ALLOCATION_FAILED";
			// Ordinals 17-23, appended at 1.6.0 and 1.7.0 (plan §2.5 row 9). The two shader-directory
			// statuses and SSLM_GPU_RESIDENCY_FLAGS_INVALID arise only in Configure();
			// SSLM_GPU_PARALLEL_FOR_INCOMPLETE is unreachable with the plugin's ParallelFor hook, and
			// if it ever fires the sequence faults and a reset recovers it.
			case SslmGpuStatus::SSLM_OUTPUT_BUFFER_TOO_SMALL: return "SSLM_OUTPUT_BUFFER_TOO_SMALL";
			case SslmGpuStatus::SSLM_PREFILL_HIDDEN_UNAVAILABLE: return "SSLM_PREFILL_HIDDEN_UNAVAILABLE";
			case SslmGpuStatus::SSLM_GPU_SHADER_DIR_INVALID: return "SSLM_GPU_SHADER_DIR_INVALID";
			case SslmGpuStatus::SSLM_GPU_SHADER_DIR_CONFLICT: return "SSLM_GPU_SHADER_DIR_CONFLICT";
			case SslmGpuStatus::SSLM_GPU_PARALLEL_FOR_INVALID: return "SSLM_GPU_PARALLEL_FOR_INVALID";
			case SslmGpuStatus::SSLM_GPU_PARALLEL_FOR_INCOMPLETE: return "SSLM_GPU_PARALLEL_FOR_INCOMPLETE";
			case SslmGpuStatus::SSLM_GPU_RESIDENCY_FLAGS_INVALID: return "SSLM_GPU_RESIDENCY_FLAGS_INVALID";
			// Ordinals 24-29, appended at 1.13.0 for paged K/V (gpu_1p0.h; docs/api.md "Paged KV memory on
			// the GPU"). Every one arises only on a page-pool verb (pool create, budgeted create, prefix
			// verbs, restore into pool) except SSLM_GPU_HOLDER_MODE_MISMATCH, which sslm_gpu_seq_restore also
			// returns for a pooled sequence's 'SLM6' blob. A plugin that never makes a pool sees none of
			// them on its own calls.
			case SslmGpuStatus::SSLM_GPU_KV_BUDGET_EXCEEDED: return "SSLM_GPU_KV_BUDGET_EXCEEDED";
			case SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED: return "SSLM_GPU_KV_POOL_EXHAUSTED";
			case SslmGpuStatus::SSLM_GPU_KV_POOL_TOO_LARGE: return "SSLM_GPU_KV_POOL_TOO_LARGE";
			case SslmGpuStatus::SSLM_GPU_HOLDER_MODE_MISMATCH: return "SSLM_GPU_HOLDER_MODE_MISMATCH";
			case SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED: return "SSLM_GPU_PREFIX_REJECTED";
			case SslmGpuStatus::SSLM_GPU_PREFIX_SCHEMA_MISMATCH: return "SSLM_GPU_PREFIX_SCHEMA_MISMATCH";
#if defined(SUPERSLM_WITH_PREFIX_CACHE) && SUPERSLM_WITH_PREFIX_CACHE
			// Ordinal 30 (the prefix save ABI): a blob from another adapter or engine version. The switch-on
			// build runs only on the engine re-pin that carries it, so the arm goes with the switch.
			case SslmGpuStatus::SSLM_RESTORE_PRODUCER_MISMATCH: return "SSLM_RESTORE_PRODUCER_MISMATCH";
#endif
		}
		// After the switch, not a default label inside it (see SuperSLMStatusMapping.h).
		return "SSLM_GPU_STATUS_UNKNOWN";
	}

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

	// The result a GPU restore resolves when its Layer-1 step failed with Status (anything but
	// SSLM_OK). The one place this mapping lives: FinalizeRestoreOp() calls it, and
	// FSuperSLMGpuTestAccess::RestoreFailureResult() exposes it to a cell that has no device.
	// bSchemaCrossCheckFailed is the plugin's own schema cross-check after the restore, which
	// reads Malformed whatever status it carries. SSLM_GPU_ALLOCATION_FAILED is Layer 1 running
	// out of GPU memory, which is retryable, so it reads OutOfMemory. A lost device
	// (SSLM_DEVICE_LOST) and every other status keep the default arm's Malformed.
	// Paged K/V (1.13.0) adds two arms. SSLM_GPU_HOLDER_MODE_MISMATCH is what sslm_gpu_seq_restore
	// now returns for a pooled sequence's 'SLM6' blob; 1.12.0 and earlier refused that blob with
	// SSLM_SEQUENCE_KV_BUFFER_MISMATCH (docs/api.md), so it reads KvMismatch, the result it had
	// before the pin. SSLM_GPU_KV_POOL_EXHAUSTED is a restore into a page pool too short for the
	// blob, nothing drawn and retryable once pages are returned: the same shape as
	// SSLM_GPU_ALLOCATION_FAILED, so it reads OutOfMemory. The other four (budget exceeded, pool too
	// large, prefix rejected, prefix schema mismatch) are not restore statuses and keep Malformed.
	inline ESuperSLMRestoreResult ToRestoreFailureResult(SslmGpuStatus Status, bool bSchemaCrossCheckFailed)
	{
		if (bSchemaCrossCheckFailed)
		{
			return ESuperSLMRestoreResult::Malformed;
		}
		switch (Status)
		{
			case SslmGpuStatus::SSLM_RESTORE_MODEL_MISMATCH: return ESuperSLMRestoreResult::ModelMismatch;
			case SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH: return ESuperSLMRestoreResult::KvMismatch;
			case SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED: return ESuperSLMRestoreResult::OutOfMemory;
			case SslmGpuStatus::SSLM_GPU_HOLDER_MODE_MISMATCH: return ESuperSLMRestoreResult::KvMismatch;
			case SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED: return ESuperSLMRestoreResult::OutOfMemory;
			default: return ESuperSLMRestoreResult::Malformed;
		}
	}

	// Plugin paging plan §3.7 (step K6): the mapping by mode. Dedicated mode (bPooled false) is the
	// function above, unchanged (cell 8.1's legacy identity). Pooled mode reads §3.7's table for the
	// three statuses that differ: SSLM_GPU_KV_POOL_EXHAUSTED is KvPagesExhausted (the caller also
	// counts it as a ledger fault, since an admitted restore was charged for it),
	// SSLM_GPU_HOLDER_MODE_MISMATCH is KvModeMismatch (a dedicated-mode blob offered to a pool), and
	// SSLM_SEQUENCE_KV_BUFFER_MISMATCH is Malformed (§5: a pooled restore's handles are the
	// plugin's own, so that status can only mean a malformed payload). Every other status, and a
	// failed schema cross-check, reads as in dedicated mode.
	inline ESuperSLMRestoreResult ToRestoreFailureResult(SslmGpuStatus Status, bool bPooled, bool bSchemaCrossCheckFailed)
	{
		if (bPooled && !bSchemaCrossCheckFailed)
		{
			if (Status == SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED)
			{
				return ESuperSLMRestoreResult::KvPagesExhausted;
			}
			if (Status == SslmGpuStatus::SSLM_GPU_HOLDER_MODE_MISMATCH)
			{
				return ESuperSLMRestoreResult::KvModeMismatch;
			}
			if (Status == SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH)
			{
				return ESuperSLMRestoreResult::Malformed;
			}
		}
		return ToRestoreFailureResult(Status, bSchemaCrossCheckFailed);
	}
}
