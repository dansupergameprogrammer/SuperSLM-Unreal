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

	// --- Paged KV, pooled mode (plugin paging plan §3.1, UP-4, UP-15; declared at step K5, read
	// from step K6). Appended, so every earlier field keeps its place. ---

	// 0 (the default) is dedicated mode: every warm sequence owns a ContextCap-sized K/V buffer,
	// exactly as before. Above 0 is pooled mode: one sslm_gpu_kv_pool_create per subsystem, sized
	// by the page ledger's regions (§3.2), and warm sequences are
	// sslm_gpu_seq_create_budgeted(..., SequenceBudget). Configure() refuses a value below 0 or
	// above ContextCap (ESuperSLMGpuConfigureResult::InvalidSequenceBudget).
	int32 SequenceBudget = 0;

	// Pooled: the maximum number of live GPU prefixes (and nested children); region P is
	// PrefixBlockCount blocks of P_blk pages. Ignored, with a logged warning, when dedicated.
	int32 PrefixBlockCount = 0;

	// Pooled: restore headroom, region H, in blocks of H_blk pages (§3.5).
	int32 RestoreHeadroomBlocks = 1;

	// Pooled with a GPU cache: region A, in blocks of P_blk. No GPU cache exists in the paging
	// release, so it is accepted and ignored there.
	int32 CacheGhostBlocks = 1;

	// --- The GPU frame budget and multi-row scheduling (multi-row GPU plan rev 1.3.2, §5.1-§5.4, slice MR6; rulings 37
	// and 39). Appended, so every earlier field keeps its place. The chooser (Private/Core/SuperSLMGpuRowsChooser.h)
	// reads these once, at Configure(), and never a measured time, so the tokens and each action's rows never depend on
	// device timing. ---

	// The GPU work one action may put in one frame, in milliseconds. 0 (the default) means one row: GpuRowCostMs +
	// SceneGpuShareMs. Otherwise it is clamped to NominalTickMs. An action that runs l layers of an M-row chunk is priced
	// at C(M) x (1 + margin) x l / L + SceneGpuShareMs, with C(M) the vendored engine's measured cost table
	// (gpu_rows_cost_table.h) scaled by GpuRowCostMs. A budget below one row is not refused: one layer per action is
	// always admitted, and Configure() logs a warning (ruling 39 Q2).
	double SchemaJumpGpuFrameBudgetMs = 0.0;
	// One row's GPU time, in milliseconds: the cost table's M = 1 entry, C(1), one row through every layer including
	// the logits finish. 0 (the default) is unset: Configure() measures it on this GPU once per model and adapter
	// (bCalibrateGpuRowCost), or uses 6.2 ms, measured on the reference box, when calibration is off. A value above 0
	// is the game's own price: it wins and no calibration runs (erratum E-13, rulings 48 and 48a). Negative or not
	// finite is refused (InvalidSchemaJumpBudget).
	double GpuRowCostMs = 0.0;
	// The scene's share of the frame's GPU time, in milliseconds (default 0: unknown; a game that knows it sets it).
	// It comes out of every slice's budget (ruling 39 Q1).
	double SceneGpuShareMs = 0.0;
	// The nominal tick period, in milliseconds (default 16.7, 60 fps): the frame budget's maximum.
	double NominalTickMs = 16.7;
	// The margin added to every multi-row price, in per-mille of the table's cost (default 150, §5.1's m; MR-D15).
	// Valid 0-1000000; anything else is refused at Configure() (InvalidSchemaJumpBudget).
	int32 GpuRowsMarginPerMille = 150;
	// Multi-row scheduling (§5.2-§5.4; MR-D16): prompts are read up to 8 rows per pass, and Exact verifies its drafts
	// as rows on the OneCall path. False restores 1.17's one-row scheduling exactly (P4.4). Tokens are identical
	// either way.
	bool bGpuMultiRow = true;
	// Erratum E-13 (rulings 48, 48a, 50 and 51): with GpuRowCostMs unset (0), Configure() measures C(1) on this GPU -- a
	// throwaway sequence, 3 warm-up and 3 timed one-row tokens including the logits finish, priced at the max rounded
	// up to 0.1 ms -- once per model artifact, GPU adapter and logits-head placement in the process. With no frame
	// budget it runs inside Configure() on the submission thread; with SchemaJumpGpuFrameBudgetMs set it runs one slice
	// per tick -- 1 layer first, then as many layers as fit the budget at the largest per-layer cost measured so far
	// with the plan's margin, the logits finish alone when it does not fit -- and nothing is admitted or planned until
	// it settles. A failure, a timeout (per attempt: 2 s without a frame budget, 10 s with one) or a price outside
	// [1.24 ms, 124 ms] after one retry falls back to 6.2 ms with a warning, once per process; the model is never
	// refused for it (a lost device is DeviceUnavailable, as at any other Configure() step). False: 6.2 ms, no
	// calibration.
	// Erratum E-14 (rulings 53, 53a and 54): the measured price, C_a at the early position pos_a, is a line in position,
	// C(1, pos) = C_a + slope x (pos - pos_a), and every slice is priced at the position it ends at. The slope starts at
	// the default, 0.0015 x C_a per position. With a frame budget set, a second phase reads the throwaway sequence to
	// P2 = min(256, ContextCap - 8) and times 1 warm-up and 3 tokens there, only in the room each tick's generations
	// leave (no wall-clock cap); the measured slope, max(0, C_b - C_a) / (P2 - pos_a), replaces the default and is
	// cached. A failure, or a line that prices C(1, ContextCap) above 124 ms, keeps the default with one warning, and
	// is not measured again in the process. A cached measured line is re-checked under each ContextCap (ruling 55). The
	// game's value, the 6.2 ms default and the fallback are flat lines.
	// Ruling 56: the calibration also keeps F, the largest of its timed logits finishes (for the game's value, the default
	// and the fallback, ceil(g(1) x C(1) / 1000) from the cost table); with a frame budget a one-row slice that ends a
	// token is priced l x (C(1, pos) - F) / L + F with the margin. After its warm-up tokens each attempt also runs one
	// layer of each widened row kernel (2, 4 or 8 rows) the plan can pick, untimed and never over the budget (one that
	// does not fit is skipped with a log line).
	// USuperSLMGpuSubsystem::GetGpuRowCostMs() reads the row cost in use.
	bool bCalibrateGpuRowCost = true;
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
	InvalidSequenceBudget,   // plugin paging plan §3.1: SequenceBudget < 0 or > ContextCap; appended
	KvPoolRefused,           // plugin paging plan §3.1, §3.4: the pool create returned KV_POOL_TOO_LARGE or ALLOCATION_FAILED, or its stats disagree with the computed geometry; the message names the pages, the bytes and the status
	InvalidSchemaJumpBudget, // multi-row GPU plan §5.1 (MR6): a frame-budget field is negative or not finite, GpuRowCostMs or NominalTickMs is not positive, or GpuRowsMarginPerMille is outside 0-1000000; appended
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

