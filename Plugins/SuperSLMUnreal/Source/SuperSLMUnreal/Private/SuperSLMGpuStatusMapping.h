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
			default: return ESuperSLMRestoreResult::Malformed;
		}
	}
}
