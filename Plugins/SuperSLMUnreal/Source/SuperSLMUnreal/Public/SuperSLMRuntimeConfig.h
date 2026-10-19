#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

// L2-S1 (the plan §4, §5, §10.2, §12 decision 2). The project-
// declared call shape the subsystem sizes its pools and workspaces from (§4: "Project settings
// declare sslm_config ... and the workspace is derived from it"), plus the two calibration-
// output knobs decision 2 leaves open: the Sequence Lifecycle Budget and the per-tick time
// budget R-S1b measures against.
//
// Every sizing field is a required positive input, matching sslm_config's own "all-zero is
// HOSTILE input" convention (sslm_abi.h) -- Configure() rejects a zero/negative field rather
// than silently defaulting it (R-S1f: "block_count 0 is refused at init with a named
// diagnostic").
//
// Decode-threading D4 (plan §10.3.1 item 5b, D-SLM8109): the decode mode, the task count and the
// thirteen per-unit cost fields are TOptional. Explicit means SET, never "different from a
// default": an assignment sets a field whatever its value, and a default-constructed config sets
// none of them. Configure() and BeginConfigure() resolve each field on its own, on the calling
// thread, into one FSuperSLMResolvedCpuConfig (below):
//  - bParallelDecode and FinishParallelTasks: the caller's value if set; else the value most
//    recently selected in this process by SuperSLM.SetDecodeMode or SuperSLM.SetFinishParallelTasks;
//    else the value saved in Saved/SuperSLM/RuntimeSettings.ini; else off and 4.
//  - each cost field: the caller's value if set; else a saved estimate in
//    Saved/SuperSLM/CalibratedCosts.ini (written by SuperSLM.EstimateCosts --write-ini) whose
//    artifact hash, effective decode mode and task count match; else the effective mode's one-time
//    measured profile.
// Code that read these fields reads FSuperSLMConfigureReport::Resolved, or
// USuperSLMSubsystem::GetResolvedConfig(), instead. TOptional's explicit operator bool tests
// presence, never the value, so these fields are never read in a boolean context.
struct SUPERSLMUNREAL_API FSuperSLMRuntimeConfig
{
	// sslm_config.max_batch: max sequences in one sslm_decode_step_v2 call.
	int32 MaxSequencesPerDecodeCall = 0;

	// sslm_config.max_chunk_budget: max tokens in one sslm_prefill call.
	int32 MaxPrefillChunkBudget = 0;

	// sslm_config.max_layer_budget: max layers in one sslm_decode_step_v2 call, in
	// [1, model.num_hidden_layers]. USuperSLMSubsystem::SetLayerBudget() (SuperSLMSubsystem.h)
	// chooses the actual per-call value at or under this ceiling; this field is only the
	// declared upper bound the workspace is sized to (§4).
	int32 MaxLayerBudget = 0;

	// How many sequences the KV pool backs concurrently (sslm_kv_pool_create's block_count, §4):
	// also the warm free list's own size (§5: "every sslm_seq that block_count allows is
	// created at subsystem init"). 0 is refused at Configure() with a named diagnostic (R-S1f).
	int32 BlockCount = 0;

	// §5, §12 decision 2: the ceiling the importer's predicted reset/adopt milliseconds
	// (SuperSLMSequenceLifecycleBudget.h) is checked against. A calibration output of R-S1b;
	// Configure() reports ESuperSLMConfigureResult::SequenceLifecycleBudgetExceeded rather than
	// silently accepting a model whose reset/adopt cost this project has not budgeted for
	// (§5: "fails import with a diagnostic naming context_cap as the lever").
	double SequenceLifecycleBudgetMs = 0.0;

	// D-SLM7403/D-SLM7407 (T-2805 round 9): bounds the WORKER's time per planning unit (one job),
	// never the frame and never the game thread's own Tick() cost (that bound is independent,
	// R-S1g). A job's static-cost-model PlannedJobMs is compared against this to compute
	// K = max(1, ceil(PlannedJobMs / TickBudgetMs)) at plan time (D-SLM7407) -- the delivery
	// delay in ticks, never revised after planning. Not enforced by refusing calls: a job whose
	// real worker cost exceeds its own K x TickBudgetMs is a later, reported overrun (§9 R-S1b (v);
	// slice 1P), not a call-time refusal.
	double TickBudgetMs = 0.0;

	// D-SLM7341 (plan §5): how many Layer-1 calls may run at once, each on its own workspace.
	// Worker lanes = min(MaxConcurrentCalls, BlockCount). The default of one lane keeps the SLM
	// to one worker, so it does not contend with the game's own jobs; batched decode still
	// serves every due sequence in that lane's calls. Must be at least 1.
	int32 MaxConcurrentCalls = 1;

	// 1.1 worker continuation (plan §5 item 1a). When true, a generation whose worker lane has no
	// other work waiting gets its next prefill or decode job queued behind the running one, so the
	// worker goes straight from one job to the next instead of idling until the next Tick() posts
	// it. Tokens are identical either way: the continuation is composed exactly as the next tick
	// would compose it, and one that finds its generation already over at its head is skipped
	// without a Layer-1 call. False restores 1.0's one-job-at-a-time hand-off.
	bool bWorkerContinuation = true;

	// Decode-threading D4: split CPU one-row decode projections through the host parallel-for
	// hook (SSLM_PARALLEL_FOR_MATVEC). The bit rides on the finish hook, which is installed only
	// when FinishParallelTasks resolves above 1, so the EFFECTIVE mode is
	// bParallelDecode && FinishParallelTasks > 1, and that is the mode the cost profiles follow.
	// Unset resolves as described above; the shipped mode is off. Tokens are identical either way.
	TOptional<bool> bParallelDecode;

	// D-SLM7342 (plan §5 item 4): extra KV pool blocks reserved for shared prefixes
	// (CreatePrefix). The pool holds BlockCount + PrefixBlockCount blocks. 0 = no prefixes.
	int32 PrefixBlockCount = 0;

	// D-SLM7341 (plan §5): the share of TickBudgetMs the Plan step packs one worker job's own
	// composition into (D-SLM7414: "the Plan step picks the largest composition whose planned
	// cost fits TickBudgetMs x BudgetHeadroom using these fixed costs"). A composition whose
	// smallest legal unit alone exceeds the share (e.g. one whole prompt token) still runs --
	// the floor the scheduler cannot go below -- and earns a correspondingly larger K instead of
	// being refused. Provisional default under plan §12 decision 2, calibrated by R-S1b. Must lie
	// in (0, 1].
	double BudgetHeadroom = 0.7;

	// D-SLM7414 (plan §5 item 2, T-2805 round 9): the static per-unit cost model's three
	// decode/prefill terms. `PlannedJobMs = decode_layers x LayerCostMs + prompt_tokens x
	// PromptTokenCostMs + token_finishes x FinishCostMs`, summed across every sequence one job
	// batches -- fixed configuration values, never a runtime measurement, and never revised by
	// the worker's actual pace.
	//
	// Unset cost fields resolve per field (above). The one-time measured profiles (plan §10.3.1
	// item 5b's table) are reasonable measured starting defaults from six per-call capture blocks
	// on one Zen 2 desktop at FinishParallelTasks 4, not certified costs or accuracy bounds; a
	// project measures its own with SuperSLM.EstimateCosts. Mode 0 / mode 1 (milliseconds, slopes
	// in milliseconds per position):
	//   LayerCostMs 1.397 / 0.906            LayerCostPerPositionMs 0.000186 / 0.000254
	//   PromptTokenCostMs 33.583 / 22.108    PromptTokenCostPerPositionMs 0.004547 / 0.005209
	//   BatchedPromptTokenCostMs 15.023 / 15.682  BatchedPromptTokenCostPerPositionMs 0.004791 / 0.004437
	//   FinishCostMs 5.836 / 5.929           ResetCostMs 2.369 / 2.397
	//   AdoptCostMs 2.648 / 2.731            SaveCostMs 2.691 / 2.744
	//   RestoreCostMs 4.644 / 4.766          PrefixBeginCostMs 2.261 / 2.425
	//   PrefixReleaseCostMs 2.247 / 2.440
	// Budget mode replaces only the three unset lifecycle defaults (Adopt/Restore/PrefixBegin):
	// actual artifact cap <= 4096: .009/3.576/.001 (mode 0), .008/3.558/.001 (mode 1);
	// larger cap: .020/90.795/.002 (mode 0), .020/90.442/.002 (mode 1), all milliseconds.
	// These K4 estimates came from full-cap 0.5B/1.5B measurements at four finish tasks;
	// matching saved estimates and explicit caller fields still override them individually.
	TOptional<double> LayerCostMs;
	TOptional<double> FinishCostMs;
	TOptional<double> PromptTokenCostMs;

	// D-SLM7793/D-SLM7794 (plan §5 item 2, §10.3.1 item 5a.1): the depth terms. CPU attention runs
	// over context_length + 1 positions, so LayerCostMs and PromptTokenCostMs are the intercepts at
	// depth 0, and:
	//   a decode layer for a slot at depth d costs      LayerCostMs + LayerCostPerPositionMs x d;
	//   a prefill chunk of C tokens starting at depth d  C x PromptTokenCostMs
	//                                                    + PromptTokenCostPerPositionMs x (C x d + C x (C - 1) / 2).
	// The depth is the slot's ContextUsed (a prefix's own Consumed for prefix prefill). Configure()
	// refuses a non-finite or negative value; 0 (a flat cost) is accepted.
	TOptional<double> LayerCostPerPositionMs;
	TOptional<double> PromptTokenCostPerPositionMs;

	// Slice 1P (SuperSLM tiled-matmul plan §4A.1 item 1): the fewest prompt tokens one prefill job
	// carries, while the prompt has that many left and MaxPrefillChunkBudget allows it. The Plan step
	// still grows a chunk to the largest size whose price fits TickBudgetMs x BudgetHeadroom, then
	// raises it to min(MinPrefillChunk, MaxPrefillChunkBudget, tokens left); a tail shorter than that
	// goes as one shorter job. A chunk that exceeds the share earns a larger K, as a one-token chunk
	// already does. Default 8: the engine's tiled-GEMM threshold (`kTiledMinTokens`), where a prefill
	// call reaches the register-tiled int8 kernel. 1 restores 1.0's prefill exactly (one token per
	// job whenever one token alone exceeds the share). Tokens are identical at every setting. Must be
	// at least 1; Configure() warns, and does not refuse, below 8.
	int32 MinPrefillChunk = 8;

	// Slice 1P (plan §4A.1 item 2): the prompt price of a prefill chunk of 8 tokens or more, in the
	// same form as the chunk-1 pair above: a chunk of C >= 8 tokens starting at depth d costs
	//   C x BatchedPromptTokenCostMs + BatchedPromptTokenCostPerPositionMs x (C x d + C x (C - 1) / 2);
	// a chunk below 8 keeps PromptTokenCostMs and PromptTokenCostPerPositionMs. SuperSLM.EstimateCosts
	// measures this pair with a second prompt arm timed at chunk 8. Configure() refuses a non-finite
	// or non-positive BatchedPromptTokenCostMs and a non-finite or negative slope, by name.
	TOptional<double> BatchedPromptTokenCostMs;
	TOptional<double> BatchedPromptTokenCostPerPositionMs;

	// D-SLM7422/D-SLM7430/D-SLM7444 (plan §5 item 2, §10.2 item 10, T-2805 round 9): the static
	// per-unit cost of each of the four queued lifecycle operations -- a job whose Kind is
	// Reset/Adopt/Save/Restore is priced by exactly one of these, never by the decode/prefill
	// formula above and never by the import-time Sequence Lifecycle Budget's live-probed
	// prediction (a distinct mechanism, SuperSLMSequenceLifecycleBudget.h).
	TOptional<double> ResetCostMs;
	TOptional<double> AdoptCostMs;
	TOptional<double> SaveCostMs;
	TOptional<double> RestoreCostMs;

	// D-SLM7504 (plan §5 item 2, §10.2 item 10): the static per-unit cost of each of the two
	// prefix administrative operations -- a job whose Kind is PrefixBegin/PrefixRelease is priced
	// by exactly one of these, never scaled by the prefix's own token count (a prefix's own
	// content-prefill cost is already priced per token through PromptTokenCostMs).
	TOptional<double> PrefixBeginCostMs;
	TOptional<double> PrefixReleaseCostMs;

	// D-SLM7421 (plan §5 item 3, §10.2 item 12, T-2805 round 9): how many pending entries one
	// sequence's own lifecycle/inference queue may hold before a new request against it is
	// refused with ESuperSLMRestoreResult::SequenceQueueFull, before anything is queued and before Layer
	// 1 is asked -- the only refusal a queued request can get for a busy sequence (D-SLM7418,
	// superseding D-SLM7416's outright busy refusal), with one exception: a generation request is
	// refused first, by name, unless the sequence will be Idle when its turn comes, because a
	// sequence runs one generation at a time (D-SLM7946; USuperSLMSubsystem::BeginGeneration()).
	// Must be at least 1.
	int32 MaxQueuedOperationsPerSequence = 4;

	// Plan §2.5 row 2 and decode-threading D4: the most tasks one token finish, one decode
	// projection, or one prefill GEMM is split into through UE's ParallelFor (Layer 1's host
	// parallel-for hook, installed on every worker lane's workspace). The calling worker takes
	// part and waits for the rest. Decode projections use the hook when bParallelDecode is on;
	// prefill GEMM threading has its own later setting. At 0 or 1 no hook is installed, so decode
	// mode 1 runs as mode 0. Tokens are identical at every setting. Unset resolves as described
	// above; the shipped value of 4 is Layer 1's measured best on the reference box
	// (docs/releases/1.7.0.md, "Thread guidance"), a measured-on-one-box default like the per-unit
	// costs. Valid 0-256; anything else is refused at Configure() by name. Above 1 the engine's
	// task system allocates its own per-call bookkeeping on the decode path (plan §4); set 1 to
	// remove it at the cost of a serial finish.
	TOptional<int32> FinishParallelTasks;

	// --- Paged KV (plugin paging plan §3.1, decision UP-3). Appended, so every earlier field keeps
	// its meaning and default. ---

	// 0 (the default) is legacy mode: 1.0.x/1.1.x behaviour, no page ledger, and the pool is
	// sslm_kv_pool_create with BlockCount + PrefixBlockCount blocks. Above 0 is budget mode: warm
	// sequences are sslm_seq_create_budgeted(..., SequenceBudget), prefixes are budget mode, and
	// the pool is sslm_kv_page_pool_create sized by the page ledger's regions (plan §3.2).
	// Configure() refuses a value below 0 or above the model's context_cap
	// (ESuperSLMConfigureResult::InvalidSequenceBudget).
	int32 SequenceBudget = 0;

	// Budget mode only: restore headroom, in whole blocks of the page ledger's H block (plan §3.5).
	// 0 refuses every restore.
	int32 RestoreHeadroomBlocks = 1;

	// Budget mode with a prefix cache configured: region A, in blocks (plan §3.2). The paging
	// release has no cache, so this is accepted and ignored.
	int32 CacheGhostBlocks = 1;
};

// Which source supplied a resolved configuration's cost fields (plan §10.3.1 item 5b, D-SLM8165).
// The Configure() line prints it as caller, saved-estimate or mode-default.
enum class ESuperSLMCostProfile : uint8
{
	Caller,        // the caller set all thirteen cost fields; no profile was consulted
	SavedEstimate, // the unset fields came from a matching SuperSLM.EstimateCosts profile in CalibratedCosts.ini
	ModeDefault,   // the unset fields came from the effective decode mode's one-time measured profile
};

// The CPU configuration Configure() and BeginConfigure() resolved from an FSuperSLMRuntimeConfig,
// the process selection and the two Saved/SuperSLM INI files, on the calling thread, before any
// pool work is queued (D-SLM8144, D-SLM8165). Plain values: the worker lanes, the hook install,
// pricing, the Configure() line and the report consume only this. The mode and task members are
// deliberately not named like the config's.
struct SUPERSLMUNREAL_API FSuperSLMResolvedCpuConfig
{
	// The decode mode the resolution chose, and the mode the lanes run: the matvec bit rides on
	// the finish hook, so the effective mode is bDecodeModeRequested && ParallelTasks > 1 (D-SLM8166).
	bool bDecodeModeRequested = false;
	bool bDecodeModeEffective = false;

	// The finish hook's task count, 0-256 (0 or 1 installs no hook).
	int32 ParallelTasks = 4;

	// The thirteen per-unit costs, under the config's names (FSuperSLMRuntimeConfig).
	double LayerCostMs = 0.0;
	double LayerCostPerPositionMs = 0.0;
	double PromptTokenCostMs = 0.0;
	double PromptTokenCostPerPositionMs = 0.0;
	double BatchedPromptTokenCostMs = 0.0;
	double BatchedPromptTokenCostPerPositionMs = 0.0;
	double FinishCostMs = 0.0;
	double ResetCostMs = 0.0;
	double AdoptCostMs = 0.0;
	double SaveCostMs = 0.0;
	double RestoreCostMs = 0.0;
	double PrefixBeginCostMs = 0.0;
	double PrefixReleaseCostMs = 0.0;

	// Which source supplied the cost fields the caller did not set, and how many the caller set
	// (0-13). CostProfile reads Caller only when CallerCostFields is 13.
	ESuperSLMCostProfile CostProfile = ESuperSLMCostProfile::ModeDefault;
	int32 CallerCostFields = 0;
};

enum class ESuperSLMConfigureResult : uint8
{
	Success,
	InvalidBlockCount,                // BlockCount <= 0 (§4, R-S1f)
	InvalidCallShape,                 // MaxSequencesPerDecodeCall / MaxPrefillChunkBudget / MaxLayerBudget <= 0
	SequenceLifecycleBudgetExceeded,  // §5, §12 decision 2; R-S1e
	InvalidModel,                     // null model, no artifact bytes, or sslm_model_map rejected it
	AllocationFailed,                 // a pool/workspace buffer or a warm sslm_seq could not be created
	// Plan §5 "The scheduler's numeric domain" (D-SLM7799, T-3004 F1): some job this configuration
	// can plan has a non-finite largest planned cost, or one whose K = ceil(cost / TickBudgetMs)
	// exceeds kMaxJobTicks (2^20). The message names the job kind, the cost fields it is priced
	// from, its bound, that K and the limit. Appended last so every earlier value keeps its number.
	InvalidCostDomain,
	// Plugin paging plan §3.1: SequenceBudget below 0 or above the model's context_cap. Appended
	// last so every earlier value keeps its number.
	InvalidSequenceBudget,
};

// Configure() is idempotent, not one-shot: calling it again tears down any pool/workspace/warm
// free list a prior call built (releasing every previously-vended FSuperSLMSequence, the same
// release path Deinitialize() takes) and rebuilds from Model/Config. This is what lets one
// GameInstanceSubsystem instance -- which the editor's automation run keeps alive across every
// test function in one pass, since nothing else tears it down between them -- serve R-S1a's six
// independently-configured arms without a manual Deinitialize() call between each.

struct SUPERSLMUNREAL_API FSuperSLMConfigureReport
{
	ESuperSLMConfigureResult Result = ESuperSLMConfigureResult::InvalidCallShape;

	// Names the offending field/lever on failure -- the runtime-configure counterpart to the
	// import-time diagnostic FSuperSLMSequenceLifecycleReport carries
	// (SuperSLMSequenceLifecycleBudget.h).
	FString Message;

	// The configuration this call resolved (D-SLM8165), set whenever resolution ran, a refused
	// configuration included: read what was configured here, not from the struct that was passed.
	TOptional<FSuperSLMResolvedCpuConfig> Resolved;
};
