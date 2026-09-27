#pragma once

#include "CoreMinimal.h"

// L2-S2 (the plan §5, §8, §10.3, §12 decisions 1/2/4). The
// project-declared GPU call shape USuperSLMGpuSubsystem::Configure() sizes the context,
// the sequence pool, and the fixed-tick scheduler from -- the GPU-side counterpart to
// the CPU backend's FSuperSLMRuntimeConfig (SuperSLMRuntimeConfig.h), matching that
// header's own "all-zero is HOSTILE input" convention.

struct SUPERSLMUNREAL_API FSuperSLMGpuRuntimeConfig
{
	// sslm_gpu_seq_create's context_cap, shared by every sequence this context vends
	// (plan §2.1: GPU KV memory is "device-side, library-owned"). 0 is refused with a
	// named diagnostic.
	int64 ContextCap = 0;

	// How many sequences the GPU sequence pool backs concurrently -- the GPU-side
	// counterpart to FSuperSLMRuntimeConfig::BlockCount. Also the warm-pool size
	// R-S2d's 1,000 vend/return cycles exercise. 0 is refused with a named diagnostic.
	int32 BlockCount = 0;

	// The composed path's per-tick dispatch_budget (plan §5, §8's "Frame Budget" knob,
	// in Layer-1 dispatch units, never raw layers): layers issued per tick =
	// floor(DispatchBudget / DispatchesPerLayer(has_qk_norm)), capped at the layers
	// left in the current token (superslm_gpu::PlanDispatchBudgetGpu, gpu_port.h, read
	// at v1.5.0). A value below one whole layer's worth of dispatches is refused
	// (InvalidDispatchBudget) rather than silently admitted and then failing every
	// composed-path call with SSLM_DISPATCH_BUDGET_TOO_SMALL at runtime -- Configure()
	// catches it before any sequence is vended.
	uint32 DispatchBudget = 0;

	// The fixed-tick latency constant (plan §5, §12 decision 4): a token request issued
	// at sim-tick T is applied at T+K. K is one constant that the plugin never adapts to load or
	// timing. It changes only through a call, while no sequence is vended:
	// USuperSLMGpuSubsystem::SetFixedTickLatency() sets it, SetLayersPerTick() raises it to the new
	// floor when it is below, and a GPU USuperSLMQuery sets its own k and then restores the
	// Configure()'d schedule (RequestConfiguredScheduleRestore()). K covers the worst concurrency
	// this configuration allows (D-SLM7381): BlockCount composed sequences sharing the per-tick
	// layer budget need ceil(BlockCount x num_hidden_layers / layers_per_tick) ticks per
	// token (the finish runs in the job of a token's last slice), and one-call sequences,
	// served one whole token per tick in rotation, need BlockCount.
	// Configure() refuses a K below the larger of the two (KBelowMinimum) and reports it in
	// FSuperSLMGpuConfigureReport::MinimumK. A token not computed by its T+K tick although
	// the device had the sim time those ticks represent is applied on the first later tick at
	// which it exists and counted as a hitch -- R-S2a sets this default (§12 decision 4).
	int32 K = 0;

	// R-S2a/R-S2f's own "pinned per-tick budget" comparison target for the composed
	// path's measured gpu_busy_ms per slice (plan §5: "an overrun counts as a hitch").
	// Not enforced by refusing calls -- a reported comparison, matching
	// FSuperSLMRuntimeConfig::TickBudgetMs's own CPU-side precedent. Must be positive
	// (InvalidTickBudget otherwise).
	double TickBudgetMs = 0.0;

	// The per-sequence software queue's own capacity (accepted async-tick plan, T-2816
	// 2026-09-19 round; plan §5 GPU path / §10.3 item 8: "shared MaxQueuedOperationsPerSequence
	// config field, default 4" -- "shared" with the CPU backend's own mirror field, which is a
	// SEPARATE, concurrently-authored ticket's own responsibility and not declared here, this
	// suite's own established "the two fixture files are not coupled" precedent applied to
	// config fields). A Reset/Adopt-Prefix/Save/Restore/second-decode request against a
	// sequence already holding this many queued (not counting the one currently Submitted)
	// operations is refused at once with ESuperSLMRestoreResult::SequenceQueueFull, before
	// anything is queued and before Layer 1 is asked. Declared at 4 here -- the plan's own
	// shipped default (plan §5, §10.3 item 8) -- NOT 0 (T-2826 round 6, D-SLM7475 corrects the
	// round-5 declaration). This struct's own "all-zero is HOSTILE input" convention (header
	// comment above) governs a field whose zero is an obviously-invalid input Configure()
	// refuses outright; it does not transfer to a field whose own zero Configure() would
	// otherwise silently ACCEPT while it makes the whole backend unusable
	// (ActiveWindowCount() >= 0 is always true, so a bound of 0 refuses the very first Reset/
	// Save/BeginGeneration/Adopt-Prefix/Restore issued against any sequence -- every GPU
	// lifecycle-op call fails with SequenceQueueFull before Layer 1 is ever asked). Configure()
	// rejects a bound below 1 by name (InvalidMaxQueuedOperationsPerSequence,
	// SuperSLMGpuSubsystem.cpp), matching this struct's own established pattern for every other
	// field whose zero is unusable rather than merely small (ContextCap, BlockCount,
	// DispatchBudget, TickBudgetMs). A test that wants a smaller, self-contained bound still
	// sets it explicitly, matching every other field in this struct --
	// SuperSLML2S2LifetimeTests.cpp's own SameSequenceCollisionQueues test does exactly this
	// (bound 3), documented there as that test's own deliberate choice.
	int32 MaxQueuedOperationsPerSequence = 4;

	// Plan §2.5 row 2, §5 GPU path (D-SLM7657, D-SLM7663): the most blocks a token finish's
	// logits rows are split into and run through UE's ParallelFor (Layer 1's host parallel-for
	// hook, installed on the GPU context), with the submission thread taking part. Used only for
	// a model mapped WITHOUT the device-resident head (USuperSLMModel::bGpuDeviceResidentHead off,
	// or its out-of-memory fallback); with the head on the device the hook is unused. At 0 or 1
	// no hook is installed and the host finish is serial. Tokens are identical at every setting.
	// Default 4, Layer 1's measured best on the reference box; valid 0-256, anything else is
	// refused at Configure() (InvalidFinishParallelTasks).
	int32 FinishParallelTasks = 4;
};

enum class ESuperSLMGpuConfigureResult : uint8
{
	Success,
	InvalidContextCap,       // ContextCap <= 0
	InvalidBlockCount,       // BlockCount <= 0
	InvalidDispatchBudget,   // DispatchBudget resolves to fewer than one whole layer
	KBelowMinimum,           // K is below the computed floor; Message/MinimumK name it
	DeviceUnavailable,       // sslm_gpu_context_create or sslm_gpu_model_map failed, or the shader
	                         // directory was refused (SSLM_GPU_SHADER_DIR_INVALID/_CONFLICT, named)
	ShaderStagingIncomplete, // a compiled shader the build lists is missing from the plugin's own
	                         // Binaries/Win64/shaders (plan §2.5 row 1)
	InvalidModel,            // null, never imported, or its artifact does not load/marshal on this backend
	InvalidTickBudget,       // TickBudgetMs <= 0: no slice budget to compare gpu_busy_ms against
	InvalidMaxQueuedOperationsPerSequence, // MaxQueuedOperationsPerSequence < 1: a bound of 0 refuses every queued op
	InvalidFinishParallelTasks, // FinishParallelTasks outside [0, 256], or Layer 1 refused the hook (named)
	KAboveMaximum,           // K above kMaxJobTicks (2^20), plan §5 (D-SLM7803); appended last so earlier values keep their numbers
};

struct SUPERSLMUNREAL_API FSuperSLMGpuConfigureReport
{
	ESuperSLMGpuConfigureResult Result = ESuperSLMGpuConfigureResult::InvalidContextCap;

	// Names the offending field/lever on failure -- the GPU-side counterpart to
	// FSuperSLMConfigureReport::Message (SuperSLMRuntimeConfig.h).
	FString Message;

	// The floor Configure() computed for K against this Config's own BlockCount and
	// DispatchBudget (D-SLM7381), populated on EVERY successful Configure() (not only on
	// KBelowMinimum failure) so a caller -- and R-S2a's own test -- can read the
	// computed minimum directly rather than re-deriving it (plan §5: "the minimum is
	// shown").
	int32 MinimumK = 0;
};

