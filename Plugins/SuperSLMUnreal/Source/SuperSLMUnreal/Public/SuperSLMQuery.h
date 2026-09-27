#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SuperSLMBlueprintTypes.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuTypes.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMQuery.generated.h"

class USuperSLMGpuSubsystem;
class USuperSLMModel;
class USuperSLMSubsystem;
struct FSuperSLMModelShapeFacts;

// L2-S3 code review S2 (the plan line 173, D-SLM3534: "two backends
// behind one Blueprint-facing interface"; §5: "At L2-S3 the unified Blueprint-facing interface ...
// re-issues the affected queries on the CPU backend. That is the fallback a user experiences";
// D-SLM7382). The unified query, in the RUNTIME module so a packaged game has it:
//
// - FSuperSLMQueryRunner (plain C++) is where a query's backend is chosen and where a
//   probe-confirmed GPU loss re-issues the query on the CPU. It was the editor query window's
//   controller body (FSuperSLMQueryWindowController, SuperSLMUnrealEditor), moved here unchanged
//   in behaviour apart from the review's S4 and W4 fixes; that controller is now a client of it.
// - USuperSLMQuery (UObject) is the Blueprint face of one runner: create it for a model, Begin a
//   query, call Tick once per frame, read the readout when it stops.
//
// The CPU backend (USuperSLMSubsystem) and the GPU backend (USuperSLMGpuSubsystem) are two
// separate GameInstance subsystems with two separate handle types (FSuperSLMSequence vs
// FSuperSLMGpuSequence, SuperSLMGpuTypes.h: "a handle from one surface is never valid on the
// other"); neither knows the other exists, and the runner is the one place a query's backend is
// decided.
//
// §8's invariant: "moving any control changes only frames-to-answer and the cost figures; the
// structured result and its token digest do not move." Every control below is one of the four
// D-SLM7244 names: Thinking/Frame Budget, Backend, Concurrent queries, k.

// ESuperSLMQueryStopReason (SuperSLMSequenceTypes.h, plain C++ by that header's own rule), mirrored
// for Blueprint; the ordinals are asserted equal in SuperSLMQuery.cpp.

/** How a schema-constrained query ended: Completed, SchemaRejected, or BudgetExhausted. */
UENUM(BlueprintType)
enum class ESuperSLMQueryStopReasonBP : uint8
{
	Completed,
	SchemaRejected,
	BudgetExhausted,
};

// One control setting (§8).

/** A query's settings: backend, frame budget, concurrent queries, k, schema-constrained decoding and stop ids. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMQueryConfig
{
	GENERATED_BODY()

	/** The backend the query runs on. A GPU query whose device is lost re-runs on the CPU. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	ESuperSLMBackendBP Backend = ESuperSLMBackendBP::CPU;

	// CPU: USuperSLMSubsystem::SetLayerBudget's own value, in [1, model.num_hidden_layers]; <= 0
	// selects the whole depth. GPU (review S4; plan §8: Frame Budget "sets the composed path's own
	// dispatch_budget, in whole layers per slice", and concurrent queries "share the tick's
	// budget"): a positive value selects ESuperSLMGpuDecodePath::Composed and sets the TICK's
	// budget to that many layers (USuperSLMGpuSubsystem::SetLayersPerTick()), which every due
	// sequence shares through the batch call, so one tick issues at most that many layers whatever
	// ConcurrentQueries is; <= 0 selects ESuperSLMGpuDecodePath::OneCall ("whole token", §8).

	/**
	 * Frame Budget, in whole transformer layers. CPU: the layers each worker job runs for a
	 * sequence, from 1 to the model's layer count. GPU: the layers one tick issues, shared by the
	 * query's sequences. 0 or less: whole tokens (on the GPU, one whole token per tick).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	int32 FrameBudgetLayers = 0;

	// 1 to N (§8). > 1 shares the GPU's per-tick budget through the batch call with rotated
	// order (§5) -- on CPU, N sequences simply tick concurrently through the same subsystem. The
	// runner vends N sequences on the selected backend, runs the SAME prompt on each, and fails
	// the query naming the divergence unless their token sequences are identical (R-S1d/R-S2a
	// already prove concurrent-vs-serial equivalence per backend; this control re-exercises it
	// at the query's own level).

	/**
	 * How many copies of the query run at once, each on its own sequence with the same prompt.
	 * The query fails, naming the divergence, unless every copy produces the same tokens.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	int32 ConcurrentQueries = 1;

	// The fixed-tick latency (§8) -- clamped to at least the ticks one sliced token needs at
	// FrameBudgetLayers (FSuperSLMQueryReadout::MinimumK reports the clamp). On the GPU it is
	// applied (USuperSLMGpuSubsystem::SetFixedTickLatency()) and never falls below the
	// subsystem's own floor (GetConfiguredMinimumK()). On the CPU k is not a control: the
	// planner derives K per job (D-SLM7407, D-SLM7409), and the readout reports that K.

	/**
	 * k, the fixed number of ticks between asking for a token and applying it. Raised to at least
	 * what one token needs at this Frame Budget (the readout's MinimumK). GPU only: on the CPU the
	 * scheduler derives k for each job, and the readout reports it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	int32 RequestedK = 1;

	// The schema-constrained-decoding checkbox (plan §8, §10.4; D-SLM7432, D-SLM7445, D-SLM7668).
	// true: every sequence of the query binds A-EX's `prompt_result` schema at its vend (the
	// bind-eligible point, plan §5 CPU path item 6; on the GPU through
	// USuperSLMGpuSubsystem::SetSchema()), and its output is {"Prompt_Result": <free text>}. The
	// readout's StopReason and SchemaAcceptingAtStop are set exactly when this is true. false: the
	// query runs unbound -- free decode on the typed text (§10.4). No other schema is ever bound by
	// a query: `prompt_result` is the only schema this surface binds (review N4).

	/**
	 * Schema-constrained decoding. On: every sequence binds the model's prompt_result schema, and
	 * the output is {"Prompt_Result": <text>}; the readout then reports how the query stopped. Off:
	 * free decoding of the typed text.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	bool bSchemaConstrainedDecoding = false;

	// The ids that end generation (review round 2, R2-W2). Plan §8: the game supplies them, because
	// the artifact records none.

	/**
	 * Token ids that end generation. The model file records none, so the game supplies them: for
	 * Qwen2.5-Instruct, 151645 and 151643 (end of turn, end of text), which the editor query window
	 * passes. Empty: the query ends at MaxNewTokens or when the schema is complete.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM|Query")
	TArray<int32> StopTokenIds;
};

// §8's own readout list, each figure labelled with its source.

/** What a finished query reports: its answer, its token digest, and its cost figures, each from the backend's own reading. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMQueryReadout
{
	GENERATED_BODY()

	/** Schema-constrained and Completed: the raw {"Prompt_Result": ...} JSON the model wrote. Empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString ResultJson;

	/** Unconstrained: the generated text. Empty for a schema-constrained query. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString VoicedReply;

	// superslm::ComputeTokenDigest (decode_digest.h). §8/R-S3e's own invariant is checked by string
	// equality on this field across every swept config.

	/** SHA-256 of the full generated token sequence, as 64 hex characters. On the CPU backend the same request gives the same digest at every other setting. A GPU digest can differ from the CPU's, per device. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString TokenDigestHex;

	/** Frames from the start of the query to its answer. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 FramesToAnswer = 0;

	// D-SLM7379; R2-N1.

	/** Time to first token, in milliseconds: the prompt's prefill, sliced like decode on the GPU's composed path. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	double PromptTimeMs = 0.0;

	/** GPU: the last slice's GPU time, in milliseconds (USuperSLMGpuSubsystem::GetLastGpuBusyMs()). 0 on the CPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	double GpuMsPerSliceMs = 0.0;

	/** GPU: the CPU time of the last token's finish, in milliseconds (USuperSLMGpuSubsystem::GetLastHostFinishMs()). 0 on the CPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	double HostFinishMsPerTokenMs = 0.0;

	/** Tokens per second over the query. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	double TokensPerSecond = 0.0;

	/** The backend's hitch count over the query, including other clients' work on the same backend. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 HitchCount = 0;

	// CPU only (0 on the GPU): HitchCount's two worker-side sources on the decode and prefill jobs
	// of any of this query's sequences, from the job ledger (plan §5 "What counts as a hitch"). Neither is game-thread
	// time: a late job is one delivered after its planned tick when the worker had K x TickBudgetMs
	// of wall time and still needed more (bHitch); an over-budget job is one whose own worker call
	// measured over TickBudgetMs (bWorkerOverran). HitchCount is the subsystem-wide delta over the
	// query, so it can exceed their sum by lifecycle-op jobs, and by other clients' jobs on the
	// shared subsystem, that were delivered while the query ran.

	/** CPU: this query's jobs the worker delivered late, after K x TickBudgetMs of wall time. Not game-thread time. 0 on the GPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 CpuLateJobs = 0;

	/** CPU: this query's jobs whose worker call alone took longer than TickBudgetMs. Not game-thread time. 0 on the GPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 CpuOverBudgetJobs = 0;

	// Coverage Model §9, cell R-S3f. On the one-call path one slice is the whole token, so
	// GpuMsPerSliceMs alone is that one sample.

	/** GPU composed path: every slice's GPU time during the query, in order. Empty on the CPU and on the GPU's one-call path. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	TArray<double> GpuBusyMsSamplesPerSlice;

	/** RequestedK after it was raised to MinimumK. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 EffectiveK = 0;

	/** The least k this query could run at (GPU: never below the backend's configured minimum). */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 MinimumK = 0;

	/** Filled by a caller that ran the determinism self-check; the query itself never runs it and leaves Not Yet Run. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	ESuperSLMSelfCheckVerdictBP SelfCheckVerdict = ESuperSLMSelfCheckVerdictBP::NotYetRun;

	/** The self-check's scope text, filled with SelfCheckVerdict. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString SelfCheckScopeText;

	// R-S3g reads its count against MaxNewTokens; R-S3h checks the truncated run's tokens are a
	// prefix of the full run's.

	/** Every token the query generated, in order (the first copy's, when ConcurrentQueries is more than 1). */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	TArray<int32> GeneratedTokens;

	// R-S3g parses it, and R-S3h asserts it does NOT parse.

	/** GeneratedTokens as text, with nothing stripped or parsed. Schema-constrained: the raw JSON the model wrote. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString RawOutputText;

	// BudgetExhausted recovery: escape-state tracked, surrogate pairs handled, JSON-unescaped, an
	// incomplete trailing escape dropped (T-2853 design §5, D-SLM7440, D-SLM7448, D-SLM7453).

	/**
	 * The answer to show. Schema-constrained: the Prompt_Result text when the query completed, or
	 * the part of it written before the token budget ran out; never a raw JSON fragment.
	 * Unconstrained: the generated text.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString DisplayedText;

	// How a schema-constrained query ended (ESuperSLMQueryStopReason, SuperSLMSequenceTypes.h,
	// plan §10.4), composed from phase, last decode outcome and SchemaAcceptingAtStop. Set exactly
	// when Config.bSchemaConstrainedDecoding is true; unset otherwise. Unset is how the readout
	// says "not classified", so no enumerator doubles as a default (D-SLM7519).
	TOptional<ESuperSLMQueryStopReason> StopReason;

	// Whether the schema walk was accepting when the query stopped: GetStats(Seq).bSchemaAccepting
	// on the CPU, USuperSLMGpuSubsystem::IsSchemaAccepting() on the GPU (plan §2.5 row 12), read
	// after the query's last token was applied. Set exactly when Config.bSchemaConstrainedDecoding
	// is true; unset means it was never read, not false.
	TOptional<bool> SchemaAcceptingAtStop;

	// The two optionals above, for Blueprint (which has no optional).

	/** True when StopReasonBP applies: the query was schema-constrained. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	bool bHasStopReason = false;

	/** How a schema-constrained query ended. Read only when bHasStopReason is true. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	ESuperSLMQueryStopReasonBP StopReasonBP = ESuperSLMQueryStopReasonBP::Completed;

	/** True when bSchemaAcceptingAtStopBP applies: the query was schema-constrained. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	bool bHasSchemaAcceptingAtStop = false;

	/** Whether the schema was complete when the query stopped. Read only when bHasSchemaAcceptingAtStop is true. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	bool bSchemaAcceptingAtStopBP = false;

	/** The backend the answer came from. Differs from the requested backend only when the GPU was lost and the query re-ran on the CPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	ESuperSLMBackendBP BackendRun = ESuperSLMBackendBP::CPU;

	// D-SLM7382: the GPU subsystem reported IsGpuBackendActive() false after a probe-confirmed
	// loss, so the query was re-issued on the CPU from its original request.

	/** The GPU device was lost, so the query re-ran on the CPU from its original request. Every figure is the CPU run's. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	bool bMovedBackend = false;

	/** When bMovedBackend is true, what happened. It claims nothing about the GPU's partial answer. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString MovedBackendMessage;

	// On the GPU, the subsystem's K when the query vended (GetConfiguredK()); on the CPU, the
	// largest K the planner derived for this query's own jobs (D-SLM7407, D-SLM7409).

	/** The k the scheduler actually applied. GPU: EffectiveK unless bScheduleNotApplied. CPU: the largest k the scheduler derived for this query's jobs. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 AppliedK = 0;

	/** GPU composed path: the layers per tick the query ran under, which is Frame Budget unless bScheduleNotApplied. 0 on the one-call path and on the CPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	int32 AppliedLayersPerTick = 0;

	// Review W4: a GPU query whose k or Frame Budget differs from the subsystem's current schedule
	// waits for the GPU pool to drain before applying it.

	/**
	 * GPU: another client kept a GPU sequence past the wait for the pool to drain, so the query ran
	 * at the backend's current schedule instead of its own k and Frame Budget. AppliedK and
	 * AppliedLayersPerTick are what it ran at.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	bool bScheduleNotApplied = false;

	/** When bScheduleNotApplied is true, why. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	FString ScheduleMessage;

	// D-SLM7409; FSuperSLMWorkerJobReport::WorkerCallMs.

	/** CPU: the mean worker time of this query's decode and prefill jobs, in milliseconds. 0 on the GPU. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Query")
	double WorkerMsPerJobMs = 0.0;
};

// Plan §7 item 5, the live inspection panel: what the running query has been fed and has returned
// so far, and its schema state, read from the backend's own accessors on the game thread. Read
// only; reading it changes nothing. The first of the query's N sequences (TokenDigestHex covers
// identity across the N once it stops).
struct SUPERSLMUNREAL_API FSuperSLMQueryLiveView
{
	ESuperSLMBackendBP Backend = ESuperSLMBackendBP::CPU;
	bool bMovedBackend = false;
	TArray<int32> PromptTokens;      // the context fed (the typed text, tokenized)
	TArray<int32> GeneratedTokens;   // the tokens returned so far (GetGeneratedTokens())
	ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
	bool bSchemaBound = false;       // the checkbox bound prompt_result
	bool bSchemaAccepting = false;   // CPU: GetStats().bSchemaAccepting; GPU: IsSchemaAccepting()
	int64 ForcedTokenCount = -1;     // CPU: GetStats().ForcedTokenCount; -1 on the GPU, which reports none
	int32 Frames = 0;
	bool bAwaitingGpuDrain = false;  // review W4: waiting for the GPU pool to drain before it vends
};

// One query at a time over one Model and the two backends. GpuSubsystem is nullable: on a box
// where the GPU backend was not Configure()'d successfully the caller passes nullptr, and a query
// with Config.Backend == GPU then fails with a named OutError rather than crashing -- "Nothing is
// refused" (§8) describes a DIVERGING device the self-check catches, never an ABSENT backend.
//
// Ticking (review W6). TickQuery() is the per-frame form: it ticks the query's backend through
// TickOncePerFrame(), so every per-frame client of one subsystem -- two queries, a query and the
// editor's schema dry run, a game that ticks the subsystem itself -- shares ONE subsystem Tick()
// per engine frame, and none doubles the tick rate. RunQuery() is the headless form: it drives
// the subsystem's Tick() directly in a loop until the query stops, for automation and
// commandlets; a live game or editor must not call it. On the GPU backend the loop sleeps 1 ms
// instead of ticking while the device is the gate, since the GPU Tick() never waits on the device
// (plan §2.5 row 21, ruling 2026-09-26); a 600 s wall cap bounds it.
//
// The runner holds plain references to the subsystems and the model; the caller keeps them alive
// for the runner's lifetime, or calls Abandon() once they are gone. Game thread only.
class SUPERSLMUNREAL_API FSuperSLMQueryRunner
{
public:
	FSuperSLMQueryRunner(USuperSLMSubsystem& InCpuSubsystem, USuperSLMGpuSubsystem* InGpuSubsystem, USuperSLMModel& InModel);
	~FSuperSLMQueryRunner();

	FSuperSLMQueryRunner(const FSuperSLMQueryRunner&) = delete;
	FSuperSLMQueryRunner& operator=(const FSuperSLMQueryRunner&) = delete;

	// Runs one query under Config to completion (see the class comment); OutReadout's
	// TokenDigestHex/ResultJson/VoicedReply are the invariant R-S3e checks across every swept
	// Config. MaxNewTokens is the query's token budget: a schema-constrained query that reaches it
	// with the walk still open is BudgetExhausted, not a failure, and returns true. Returns false
	// with a named OutError when the query cannot run -- including, with the checkbox on, when
	// `prompt_result` does not resolve on the selected backend (an A-EX without the §10.4 rebuild)
	// -- never a readout that looks like a result.
	bool RunQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryConfig& Config,
		FSuperSLMQueryReadout& OutReadout, FString& OutError);

	// Vends Config.ConcurrentQueries sequences on the selected backend, binds `prompt_result` when
	// the checkbox is on, and submits the prompt. False, with OutError, when the query cannot
	// start; nothing is left vended then.
	bool BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryConfig& Config, FString& OutError);

	// Once per frame. Returns true once the query has stopped: bOutSucceeded then says whether
	// OutReadout is a result (RunQuery()'s true) or OutError names why not. False while it runs.
	bool TickQuery(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMQueryReadout& OutReadout, FString& OutError);

	bool IsQueryRunning() const;

	// False when no query is running.
	bool GetLiveView(FSuperSLMQueryLiveView& OutView) const;

	// Returns the running query's sequences to their pools without a readout.
	void CancelQuery();

	// Forgets the running query without returning its sequences: for when a subsystem was
	// re-Configure()d or shut down under it, which returned every sequence with its pool.
	void Abandon();

	// Review W4: how long a GPU query waits for the pool to drain before it runs at the current
	// schedule (FSuperSLMQueryReadout::bScheduleNotApplied). Default 5 s.
	void SetGpuDrainWaitSeconds(double Seconds) { GpuDrainWaitSeconds = FMath::Max(0.0, Seconds); }

private:
	struct FActiveQuery;
	TUniquePtr<FActiveQuery> Active;

	bool TickQueryImpl(float DeltaSeconds, bool bDirectTick, bool& bOutSucceeded, FSuperSLMQueryReadout& OutReadout, FString& OutError);
	bool StartOnBackend(ESuperSLMBackendBP Backend, FString& OutError);
	bool TryApplyGpuSchedule(const FSuperSLMModelShapeFacts& Shape, FString& OutRefusal);
	bool VendOnGpu(const FSuperSLMModelShapeFacts& Shape, FString& OutError);
	void ReturnActiveSequences();
	bool FinishQuery(FSuperSLMQueryReadout& OutReadout, FString& OutError);

	USuperSLMSubsystem& CpuSubsystem;
	USuperSLMGpuSubsystem* GpuSubsystem = nullptr;
	USuperSLMModel& Model;
	FSuperSLMSchemaHandle CpuSchemaHandle;
	FSuperSLMGpuSchemaHandle GpuSchemaHandle;
	double GpuDrainWaitSeconds = 5.0;
};

// The Blueprint face of FSuperSLMQueryRunner (review S2): one query at a time over one model on
// either backend, with the D-SLM7382 CPU re-issue on a GPU loss. Create it with CreateQuery(),
// call BeginQuery(), then TickQuery() once per frame until it reports the query stopped. The GPU
// backend is used only when the GameInstance's USuperSLMGpuSubsystem is active and configured with
// the same model; otherwise a GPU query fails naming why. Game thread.
//
// A GPU query re-schedules the whole GPU backend while it runs (review round 2, R2-W3): Frame
// Budget and k set the subsystem's shared LayersPerTick and K, and every other client's sequences
// run under them. When the query ends its runner asks the subsystem to restore the Configure()'d
// schedule, which happens as soon as the pool is drained
// (USuperSLMGpuSubsystem::RequestConfiguredScheduleRestore()). While another client holds a GPU
// sequence the query does not change the schedule at all: it waits, then runs at the current one
// (FSuperSLMQueryReadout::bScheduleNotApplied).

/**
 * One query at a time over one model, on either backend. Create it with Create Query, call Begin
 * Query, then Tick Query once per frame until it reports the query stopped. A GPU query whose
 * device is lost re-runs on the CPU. Game thread only.
 */
UCLASS(BlueprintType)
class SUPERSLMUNREAL_API USuperSLMQuery : public UObject
{
	GENERATED_BODY()

public:
	/** Finds the two backends in this world's GameInstance. Null, with OutError, when the model is null, there is no GameInstance, or the CPU backend is not configured with Model. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Query", meta = (WorldContext = "WorldContextObject"))
	static USuperSLMQuery* CreateQuery(UObject* WorldContextObject, USuperSLMModel* Model, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Query")
	bool BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryConfig& Config, FString& OutError);

	/** Call once per frame. True once the query has stopped: bSucceeded then says whether Readout is a result, or OutError names why not. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Query")
	bool TickQuery(float DeltaSeconds, bool& bSucceeded, FSuperSLMQueryReadout& Readout, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Query")
	void CancelQuery();

	UFUNCTION(BlueprintPure, Category = "SuperSLM|Query")
	bool IsQueryRunning() const;

	//~ Begin UObject interface
	virtual void BeginDestroy() override;
	//~ End UObject interface

private:
	// False (and the runner abandoned) once a subsystem or the model is gone.
	bool EnsureAlive(FString& OutError);

	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> Model;

	TWeakObjectPtr<USuperSLMSubsystem> Cpu;
	TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
	TUniquePtr<FSuperSLMQueryRunner> Runner;
};
