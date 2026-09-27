#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S1 (the plan §5, §10.2; Coverage Model §9, cells R-S1a-R-S1f;
// the red-suite record §5 "API declared"). The subsystem's own
// vended handle and the shapes its per-sequence operations take.
//
// Plain C++ -- no UPROPERTY/UENUM(BlueprintType) anywhere in this L2-S1 API surface. The
// Blueprint surface (plan §6) is explicitly L2-S3 scope (§10.4: "L2-S3 -- Blueprint, editor
// surface, inspection, MCP action tier"); §10.2's own text scopes L2-S1 to "Subsystem and CPU
// scheduling" only. Reflection is added when the Blueprint surface is actually built, not
// speculatively here.

// Mirrors superslm::sslm_span_kind (ThirdParty/SuperSLM/include/superslm/sslm_abi.h) -- §5's
// own cited call shape for sslm_prefill/sslm_prefix_prefill.
enum class ESuperSLMSpanKind : uint8
{
	Prompt = 0,        // SSLM_SPAN_PROMPT
	SchemaContent = 1, // SSLM_SPAN_SCHEMA_CONTENT
};

// Mirrors the tick loop's three sentinel outcomes (§5 "The tick loop branches on three
// sentinels", §6 "Decode status is an enum, never a sentinel number") plus ordinary token
// production. "'Negative' is never one case" (§5) -- every layer this ABI's -1/-2/-3 crosses
// names each sentinel, never passes a bare int through.
enum class ESuperSLMDecodeOutcome : uint8
{
	TokenProduced = 0,         // a token was produced this call
	Generating = 1,            // ABI sentinel -1: mid-token, re-queue and retry with the same state
	SchemaDeadEnd = 2,         // ABI sentinel -2: schema-bound walk has no legal continuation
	SequenceNoLongerValid = 3, // ABI sentinel -3: released concurrently -- never re-queue
};

// One vended sequence. Wraps the subsystem's own private lookup key -- never a raw sslm_seq --
// the same "the ABI handle never crosses the module's own public surface" discipline
// USuperSLMModel already holds for sslm_model (SuperSLMModel.h). 0 is never a valid vended
// handle: a default-constructed FSuperSLMSequence is always invalid.
struct SUPERSLMUNREAL_API FSuperSLMSequence
{
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }
	bool operator==(const FSuperSLMSequence& Other) const { return Id == Other.Id; }
	bool operator!=(const FSuperSLMSequence& Other) const { return Id != Other.Id; }
};

// A phase machine, one per vended sequence, driven forward by USuperSLMSubsystem::Tick() (§5).
// A freshly vended or just-reset sequence is Idle -- also the state SetSchema()/adapter binding
// require (§2.1: "Binding is legal only on a fresh or just-reset sequence"). BeginGeneration()
// moves Idle -> Prefilling -> Decoding; a sequence reaches Complete on MaxNewTokens or the
// model's own end-of-text token, or Faulted on SchemaDeadEnd or a rejected call.
enum class ESuperSLMSequencePhase : uint8
{
	Idle,
	Prefilling,
	Decoding,
	Complete,
	Faulted,
};

// The query window's own classification of how a schema-bound query ended (plan §10.4; T-2853
// design §5, D-SLM7440, D-SLM7449), composed on both backends from GetPhase()/
// GetLastDecodeOutcome() and the accepting reading -- GetStats().bSchemaAccepting on the CPU,
// USuperSLMGpuSubsystem::IsSchemaAccepting() on the GPU (plan §2.5 row 12). The composition
// itself is SuperSLMPromptResult::ComposeStopReason() (SuperSLMPromptResult.h). Only a
// schema-bound query is classified: an unbound one has no accepting state to read.
enum class ESuperSLMQueryStopReason : uint8
{
	Completed,       // schema_accepting == 1: the walk reached the schema's one accepting state
	SchemaRejected,  // Faulted / SchemaDeadEnd: a token had no legal continuation (grammar violation)
	BudgetExhausted, // Complete via MaxNewTokens, or Faulted / Generating via SSLM_CONTEXT_CAP_EXCEEDED,
	                 // with bSchemaAccepting == false -- the walk was still open when generation stopped
};

// The plugin-facing request for one sequence's generation (§5): the already-tokenized prompt,
// how many new tokens to produce at most, and which span kind sslm_prefill fills.
struct SUPERSLMUNREAL_API FSuperSLMGenerationRequest
{
	TArray<int32> PromptTokens;
	int32 MaxNewTokens = 0;
	ESuperSLMSpanKind SpanKind = ESuperSLMSpanKind::Prompt;

	// The model's end-of-text token ids. Generation completes on the first produced token in
	// this set, and that token is the last element of the generated tokens -- the same stop
	// contract as Layer 1's own tools/sslm_generate --stop. Supplied by the caller because
	// Layer 1 carries no end-of-text concept: the .sslm format stores no EOS id and the
	// tokenizer adds no BOS/EOS markers (tokenizer.h, sslm_generate.cpp). Qwen2.5-Instruct's
	// ids are 151645 (<|im_end|>) and 151643 (<|endoftext|>). Empty: generate MaxNewTokens.
	TArray<int32> StopTokenIds;
};

// A shared prompt prefix (D-SLM7342, plan §5 item 4): prefilled once into its own pool block,
// then adopted by any number of fresh sequences as a whole-block copy (sslm_seq_adopt_prefix).
// 0 is never a valid handle.
struct SUPERSLMUNREAL_API FSuperSLMPrefix
{
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }
	bool operator==(const FSuperSLMPrefix& Other) const { return Id == Other.Id; }
};

enum class ESuperSLMPrefixPhase : uint8
{
	Pending,    // its pool block is drawn on the tick queue (sslm_prefix_begin)
	Prefilling, // sslm_prefix_prefill on worker lanes, one whole token or more per tick
	Ready,      // frozen (sslm_prefix_freeze); AdoptPrefix can use it
	Faulted,    // a Layer-1 call rejected it; release it
	Invalid,    // not a live prefix handle
};

// D-SLM7408/D-SLM7421 (plan §5 item 3, T-2805 round 9): the opaque handle a queued lifecycle
// operation (Reset/Adopt/Save/Restore) returns at once. Queuing is not completion -- the named
// result (and, for Save, the blob) is read later through GetLifecycleOpResult()/GetSaveResult(),
// never through the call that queued it. 0 is never a valid handle.
struct SUPERSLMUNREAL_API FSuperSLMLifecycleOpHandle
{
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }
	bool operator==(const FSuperSLMLifecycleOpHandle& Other) const { return Id == Other.Id; }
	bool operator!=(const FSuperSLMLifecycleOpHandle& Other) const { return Id != Other.Id; }
};

// D-SLM7407/D-SLM7414 (plan §5, T-2805 round 9), extended to PrefixBegin/PrefixRelease by
// D-SLM7504 (plan §5 item 2, §10.2 item 10): what one worker job's own Layer-1 work IS. A job
// is either a batch of due sequences' decode/prefill calls, the tick's one admitted lifecycle
// operation, or a prefix administrative operation -- never more than one of these (plan §5 item
// 3: "a job may batch several due sequences' calls onto one worker thread"; a lifecycle or
// prefix-admin op is its own whole Layer-1 call).
enum class ESuperSLMWorkerJobKind : uint8
{
	DecodeOrPrefill,
	Reset,
	Adopt,
	Save,
	Restore,
	PrefixBegin,
	PrefixRelease,
};

// One worker job's own record (D-SLM7407/D-SLM7414/D-SLM7422/D-SLM7504, plan §5, §9
// R-S1b/R-S1i/R-S1j). Committed by the Plan step, filled in by Apply once the worker delivers.
// PlannedJobMs/K are pure functions of Kind/composition and the nine static per-unit costs
// (FSuperSLMRuntimeConfig) -- never a measurement -- fixed the instant the job is planned and
// never revised afterward.
struct SUPERSLMUNREAL_API FSuperSLMWorkerJobReport
{
	int64 JobId = 0;                                        // monotonic, assigned at Plan time; 0 is never valid
	ESuperSLMWorkerJobKind Kind = ESuperSLMWorkerJobKind::DecodeOrPrefill;

	// Composition (DecodeOrPrefill jobs only; zero on a lifecycle-op job) -- the same three terms
	// the static-cost-model formula sums (plan §5), summed across every sequence this job
	// batches: decode_layers x LayerCostMs + prompt_tokens x PromptTokenCostMs + token_finishes x
	// FinishCostMs.
	int32 DecodeLayers = 0;
	int32 PromptTokens = 0;
	int32 TokenFinishes = 0;

	// The positions the job served (plan §10.3.1 item 5a.1, D-SLM7793/D-SLM7794), written where the
	// job is assembled from the depths it serves -- never derived from its planned cost -- so R-S1b's
	// oracle can add the depth terms independently:
	//   DecodeLayerDepthSum = sum over the job's decode layers of the serving slot's ContextUsed;
	//   PromptPositionSum   = sum over the job's prompt tokens of each token's position (the slot's,
	//                         or a prefix's, Consumed count at the chunk's start, then one per token).
	// Zero on a lifecycle-op or prefix-administrative job.
	int64 DecodeLayerDepthSum = 0;
	int64 PromptPositionSum = 0;

	// The sequence a lifecycle-op job belongs to (invalid on a DecodeOrPrefill job, which may
	// batch several sequences and has no single owner; also invalid on a PrefixBegin/
	// PrefixRelease job, which belongs to a prefix, not a sequence). LifecycleOpHandle is the same handle
	// ResetSequence()/AdoptPrefix()/SaveSequence()/RestoreSequence() returned to the caller.
	FSuperSLMSequence LifecycleOpSequence;
	FSuperSLMLifecycleOpHandle LifecycleOpHandle;

	// Diagnostic only (plan §9 R-S1d, U1): every sequence this job served, as held at post -- each
	// member of a DecodeOrPrefill batch, the one sequence of a prompt prefill, and a lifecycle op's
	// sequence. Empty for a prefix job and for a recycle (whose slot has no holder). Lets a test
	// check which sequences a job was posted for, e.g. that none is posted after its -3.
	TArray<FSuperSLMSequence> MemberSequences;

	double PlannedJobMs = 0.0;         // the static-cost-model figure for Kind/composition (plan §5)
	int32 K = 0;                       // max(1, ceil(PlannedJobMs / TickBudgetMs)) at plan time (D-SLM7407)
	int32 PlannedAtTick = 0;           // the game-thread tick number Plan committed this job on
	int32 CommittedDeliveryTick = 0;   // PlannedAtTick + K; never revised after planning

	int32 DeliveredAtTick = -1;        // -1 until Apply actually delivers this job's result(s); may
	                                    // exceed CommittedDeliveryTick without being a hitch (below)
	                                    // but is never earlier and results are never reordered (R-S1b (iv))
	double WorkerCallMs = -1.0;        // the worker's own measured wall-clock time inside this job's
	                                    // Layer-1 call(s); -1 until measured
	bool bWorkerOverran = false;       // a Layer-1 call inside this job measured over TickBudgetMs,
	                                    // reported on this job regardless of K's own headroom (R-S1b (v))
	uint32 WorkerThreadId = 0;         // the OS thread id that actually ran this job's Layer-1 call(s)
	                                    // (R-S1b (ii)/R-S1g: never the game thread's own id)
	double WorkerSpanMs = -1.0;        // real wall-clock time from the job's post (its planning tick)
	                                    // to the worker finishing it, queue wait behind earlier jobs
	                                    // included; -1 until delivered. bHitch's wall-time half reads
	                                    // this (below), so a test applies the same rule (review R4-S1)

	// D-SLM7407 ("What counts as a hitch", plan §5): true exactly when DeliveredAtTick >
	// CommittedDeliveryTick AND WorkerSpanMs > K x TickBudgetMs -- the worker was left at least that
	// many ticks' worth of real wall-clock time to run it and still needed more (a
	// harness ticking faster than real time does not blame the worker for its own pace, T-2826
	// §4 Q4's accepted reading for the GPU path, mirrored here). GetHitchCount() sums bHitch across
	// the ledger (R-S1b (iii)) plus every bWorkerOverran (R-S1b (v)) -- late-by-tick-count ALONE
	// (DeliveredAtTick > CommittedDeliveryTick with bHitch false) is not itself a violation of
	// anything: R-S1b (iv) only requires never early and never reordered, which this allows.
	bool bHitch = false;
};

// The game thread's own Tick() report (plan §5, §5.1; §9 R-S1g, T-2805 round 9). Plan and Apply
// bookkeeping only -- never a Layer-1 call (R-S1b (ii), R-S1g). Retired at D-SLM7407: the old
// per-tick DecodeLayers/PrefillTokens/TokensFinished/PlannedMs/OverrunMs/floor fields, which
// described a call that ran ON the game thread and no longer does. Per-job composition, static
// cost, K and delivery now live on FSuperSLMWorkerJobReport (GetJobLedger()), which is a per-JOB
// property, not a per-tick one, since a job can take several ticks to deliver.
struct SUPERSLMUNREAL_API FSuperSLMTickReport
{
	int32 TickIndex = 0;              // this subsystem's own monotonic tick counter, since Configure()
	double PlanMs = 0.0;               // the game thread's own time inside this tick's Plan step
	double ApplyMs = 0.0;              // the game thread's own time inside this tick's Apply step
	double DurationMs = 0.0;           // PlanMs + ApplyMs -- R-S1g's own bound; never a worker/Layer-1 cost
	int32 JobsPlannedThisTick = 0;     // 0 or 1 per lane (plan §5: "hand the whole job ... as one unit"); jobs actually posted (D-SLM7803, T-3006 O1)
	int32 JobsDeliveredThisTick = 0;   // results Apply delivered this tick, of any job kind
	int32 LateJobsThisTick = 0;        // of JobsDeliveredThisTick, how many were hitches by bHitch's
	                                    // plan §5 rule (late by tick AND WorkerSpanMs > K x TickBudgetMs)
};

// Mirrors sslm_stats_out (sslm_abi.h) one-for-one -- the plugin transports Layer 1's own
// per-sequence stats through, it does not recompute them. R-S1e: "the stat group's counters
// equal the subsystem's own ledger after R-S1b."
struct SUPERSLMUNREAL_API FSuperSLMSequenceStats
{
	int64 DecodeStepCeiling = 0;
	int64 DecodeStepActual = 0;
	int64 ForcedTokenCount = 0;
	int32 KvBlocksResident = 0;
	bool bSchemaAccepting = false;
};
