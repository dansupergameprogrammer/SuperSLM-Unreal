#pragma once

#include "CoreMinimal.h"

// L2-S1 (the plan §4, §5, §10.2, §12 decision 2). The project-
// declared call shape the subsystem sizes its pools and workspaces from (§4: "Project settings
// declare sslm_config ... and the workspace is derived from it"), plus the two calibration-
// output knobs decision 2 leaves open: the Sequence Lifecycle Budget and the per-tick time
// budget R-S1b measures against.
//
// Every field is a required positive sizing input, matching sslm_config's own "all-zero is
// HOSTILE input" convention (sslm_abi.h) -- Configure() rejects a zero/negative field rather
// than silently defaulting it (R-S1f: "block_count 0 is refused at init with a named
// diagnostic").
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
	// real worker cost exceeds this is a later, reported hitch (§9 R-S1b), not a call-time
	// refusal.
	double TickBudgetMs = 0.0;

	// D-SLM7341 (plan §5): how many Layer-1 calls may run at once, each on its own workspace.
	// Worker lanes = min(MaxConcurrentCalls, BlockCount). The default of one lane keeps the SLM
	// to one worker, so it does not contend with the game's own jobs; batched decode still
	// serves every due sequence in that lane's calls. Must be at least 1.
	int32 MaxConcurrentCalls = 1;

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
	// batches -- fixed configuration fields, never a runtime measurement, and never revised by
	// the worker's actual pace.
	// Defaults (all eleven cost fields): D-SLM7817, the medians of 14 warm runs of the relevance
	// measurement (SuperSLM.U1.Relevance) on one machine, rounded up, except LayerCostMs
	// and LayerCostPerPositionMs, raised above those medians because the calibration's layer arm
	// under-prices in-job layers by about 25% at depth 1 (D-SLM7817; see T-3007).
	double LayerCostMs = 2.0;
	double FinishCostMs = 5.3;
	double PromptTokenCostMs = 36.0;

	// D-SLM7793/D-SLM7794 (plan §5 item 2, §10.3.1 item 5a.1): the depth terms. CPU attention runs
	// over context_length + 1 positions, so LayerCostMs and PromptTokenCostMs are the intercepts at
	// depth 0, and:
	//   a decode layer for a slot at depth d costs      LayerCostMs + LayerCostPerPositionMs x d;
	//   a prefill chunk of C tokens starting at depth d  C x PromptTokenCostMs
	//                                                    + PromptTokenCostPerPositionMs x (C x d + C x (C - 1) / 2).
	// The depth is the slot's ContextUsed (a prefix's own Consumed for prefix prefill). Configure()
	// refuses a non-finite or negative value; 0 (a flat cost) is accepted. Defaults: D-SLM7817's
	// measured slopes (item 5c), from the same log and rule as the intercepts above; the layer slope
	// is raised with LayerCostMs (see above, T-3007).
	double LayerCostPerPositionMs = 0.001;
	double PromptTokenCostPerPositionMs = 0.021;

	// D-SLM7422/D-SLM7430/D-SLM7444 (plan §5 item 2, §10.2 item 10, T-2805 round 9): the static
	// per-unit cost of each of the four queued lifecycle operations -- a job whose Kind is
	// Reset/Adopt/Save/Restore is priced by exactly one of these, never by the decode/prefill
	// formula above and never by the import-time Sequence Lifecycle Budget's live-probed
	// prediction (a distinct mechanism, SuperSLMSequenceLifecycleBudget.h). Defaults: D-SLM7817
	// (above), superseding D-SLM7444's figures.
	double ResetCostMs = 2.1;
	double AdoptCostMs = 2.5;
	double SaveCostMs = 2.5;
	double RestoreCostMs = 4.2;

	// D-SLM7504 (plan §5 item 2, §10.2 item 10): the static per-unit cost of each of the two
	// prefix administrative operations -- a job whose Kind is PrefixBegin/PrefixRelease is priced
	// by exactly one of these, never scaled by the prefix's own token count (a prefix's own
	// content-prefill cost is already priced per token through PromptTokenCostMs). Defaults:
	// D-SLM7817 (above), measured directly by the calibration command.
	double PrefixBeginCostMs = 2.0;
	double PrefixReleaseCostMs = 2.0;

	// D-SLM7421 (plan §5 item 3, §10.2 item 12, T-2805 round 9): how many pending entries one
	// sequence's own lifecycle/inference queue may hold before a new request against it is
	// refused with ESuperSLMRestoreResult::SequenceQueueFull, before anything is queued and before Layer
	// 1 is asked -- the only refusal a queued request can get for a busy sequence (D-SLM7418,
	// superseding D-SLM7416's outright busy refusal), with one exception: a generation request is
	// refused first, by name, unless the sequence will be Idle when its turn comes, because a
	// sequence runs one generation at a time (D-SLM7946; USuperSLMSubsystem::BeginGeneration()).
	// Must be at least 1.
	int32 MaxQueuedOperationsPerSequence = 4;

	// Plan §2.5 row 2, §5 CPU path item 7 (D-SLM7657, D-SLM7663): the most blocks one token
	// finish's logits rows are split into and run through UE's ParallelFor (Layer 1's host
	// parallel-for hook, installed on every worker lane's workspace). The worker thread that made
	// the decode call takes part and waits for the rest. At 0 or 1 no hook is installed and the
	// finish is serial. Tokens are identical at every setting. The default of 4 is Layer 1's
	// measured best on the reference box (docs/releases/1.7.0.md, "Thread guidance"), a
	// measured-on-one-box default like the per-unit costs. Valid 0-256; anything else is refused
	// at Configure() by name. Above 1 the engine's task system allocates its own per-call
	// bookkeeping on the decode path (plan §4); set 1 to remove it at the cost of a serial finish.
	int32 FinishParallelTasks = 4;
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
	// exceeds kMaxJobTicks (2^20). The message names the job kind, its bound, that K and the limit.
	// Appended last so every earlier value keeps its number.
	InvalidCostDomain,
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
};
