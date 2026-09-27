#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Trace/Trace.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMSubsystem.generated.h"

class USuperSLMModel;
struct FSuperSLMSubsystemState;
struct FSuperSLMCpuConfigureFlight;

// L2-S1 (the plan §4, §5, §10.2; Coverage Model §9, cells
// R-S1a-R-S1f; the red-suite record). A GameInstance subsystem
// (§4: "USuperSLMSubsystem (GameInstance subsystem) owns the KV pool and a workspace pool"),
// the ONE place the plugin's CPU-backend runtime state lives. Every method here is a C++ entry
// point; the Blueprint surface is USuperSLMBlueprintLibrary (SuperSLMBlueprintLibrary.h), which
// calls into it. The plugin's own automation tests call these methods directly and step the
// subsystem in a loop, a convention taken from a sibling plugin in the project's private
// history, rather than driving a real per-frame Tick callback.
//
// §5.1's own trace channel: costs nothing when off, toggled by name, shared by whichever
// module needs to turn it on (declared with UE_TRACE_CHANNEL_EXTERN, as a sibling plugin in the
// project's private history does). Defined once, in SuperSLMSubsystem.cpp.
//
// The C++ identifier is SuperSLMChannel because `SuperSLM` is already this module's namespace
// (SuperSLMSaveRestoreTypes.h, SuperSLMIntegrity.h, both included above) and the two cannot
// share a name. The channel's runtime name is still "SuperSLM": UE Trace strips a trailing
// "Channel" from the identifier when it registers the channel (TraceLog Channel.cpp,
// GetChannelNameLength), so UE::Trace::ToggleChannel(TEXT("SuperSLM")) and
// -trace=SuperSLM both reach it.
UE_TRACE_CHANNEL_EXTERN(SuperSLMChannel, SUPERSLMUNREAL_API);

enum class ESuperSLMVendResult : uint8
{
	Success,
	PoolExhausted, // the free list is empty -- the plugin's OWN refusal, before
	               // SSLM_KV_POOL_EXHAUSTED can ever arise (§4; R-S1f)
	NotConfigured, // Configure() has not yet succeeded
};

UCLASS()
class SUPERSLMUNREAL_API USuperSLMSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UGameInstanceSubsystem
	// Releases every warm sequence, destroys the workspace pool and the KV pool, and unmaps
	// Model -- in an order that raises no Layer-1 lifecycle rejection even with sequences still
	// vended at shutdown (R-S1f: "Subsystem shutdown with live sequences -> no Layer-1 lifecycle
	// rejection fires").
	virtual void Deinitialize() override;
	//~ End UGameInstanceSubsystem

	// Builds the KV pool, the workspace pool, and the warm free list of Config.BlockCount
	// sslm_seq handles against Model (§4; §5: "every sslm_seq that block_count allows is
	// created at subsystem init or level load and vended from a free list"). Runs the Sequence
	// Lifecycle Budget check (SuperSLMSequenceLifecycleBudget.h) against Model's own KV block
	// size BEFORE committing to the allocation -- a budget-exceeding config never allocates
	// (R-S1e). Config.BlockCount <= 0, or any call-shape field <= 0, is refused with a named
	// result and no allocation (R-S1f).
	FSuperSLMConfigureReport Configure(USuperSLMModel* Model, const FSuperSLMRuntimeConfig& Config);

	// Configure() without blocking the calling thread (fold-round ruling 1: a load must not block
	// the editor). Game thread: tears down what a prior call built, which waits for each worker
	// thread to run every job already queued on it and then joins it, so the wait is the lanes'
	// backlog, not one job (the first call has nothing to tear down and does not wait), and
	// captures Model's bytes, holding Model alive. Pool thread: everything else Configure() does
	// -- the mapping (the whole-file SHA-256 and copy), the lifecycle budget's bandwidth measurement, the KV,
	// workspace and save allocations, the lane threads and the warm sequences. Game thread again
	// (a queued task, a later frame): the state is published and OnDone receives the same report
	// Configure() would have returned. Until then the subsystem is unconfigured
	// (IsConfigurePending() is true). A later Configure(), BeginConfigure() or Deinitialize()
	// supersedes it: its state is discarded on a pool thread and OnDone is not called.
	void BeginConfigure(USuperSLMModel* Model, const FSuperSLMRuntimeConfig& Config, TUniqueFunction<void(const FSuperSLMConfigureReport&)> OnDone);
	bool IsConfigurePending() const { return bConfigurePending; }

	// --- Sequence vend/return (§4, §5) ---

	// Pulls one sslm_seq from the warm free list -- never sslm_seq_create, which runs only at
	// Configure()/Deinitialize() (§5: "create and release never run on a gameplay tick").
	// PoolExhausted once Config.BlockCount handles are all vended; a caller never reaches
	// SSLM_KV_POOL_EXHAUSTED through this path (§4).
	ESuperSLMVendResult VendSequence(FSuperSLMSequence& OutSequence);

	// Queues the sequence's release as the LAST entry of its own per-sequence queue (D-SLM7421,
	// plan §5 item 3): the physical block is not handed to a new vend, and a fresh Reset queued
	// by a new tenant does not start, until every operation already queued for this sequence --
	// including this release -- has drained in arrival order. Sequence is invalid to every OTHER
	// caller immediately, even while the underlying sslm_seq is still settling in the queue.
	void ReturnSequence(const FSuperSLMSequence& Sequence);

	// D-SLM7421/D-SLM7423 (plan §5 item 3): queues the reset and returns at once. Success means
	// QUEUED, not completed -- OutHandle is the caller's own read-back key;
	// GetLifecycleOpResult(OutHandle) reads Pending until the reset's own committed delivery
	// tick has come, then Success. SequenceQueueFull is the only synchronous refusal (this
	// sequence's own queue already holds Config.MaxQueuedOperationsPerSequence entries, §10.2
	// item 12); OutHandle is invalid then. R-S1f drives this to prove "reset-then-bind": SetSchema
	// on a non-fresh sequence fails, then ResetSequence() drained to Success + SetSchema() succeeds.
	//
	// A reset requested while a generation runs interrupts it. Until the reset takes effect, at its
	// delivery, the sequence keeps its phase and its tokens (Complete included). A reset that
	// succeeds leaves the sequence Idle with no tokens (D-SLM7949). A reset Layer 1 refuses
	// resolves Malformed and leaves the sequence Faulted, with the previous generation's tokens
	// still readable, until a reset succeeds (D-SLM7947, D-SLM7948); the fault is logged
	// (LogSuperSLM Warning "Sequence N faulted: ...", naming sslm_seq_reset and its status).
	ESuperSLMRestoreResult ResetSequence(const FSuperSLMSequence& Sequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError);

	// --- Per-sequence configuration, valid only on a fresh/reset (Idle) sequence (§2.1) ---

	// Selects the layer_budget every subsequent decode call on Sequence uses, bounded by
	// Config.MaxLayerBudget (§5: "A sequence needing a different budget goes in a different
	// call"). This is how a caller -- and R-S1a's own layer_budget in {1, 6, 24} sweep --
	// selects the value.
	void SetLayerBudget(const FSuperSLMSequence& Sequence, int32 LayerBudget);

	// Layer 1 binds a schema only on a sequence that is new or reset and has not since generated,
	// adopted a prefix or been restored (plan §2.5 row 4). When the sequence will not be in that
	// state once its queued work has run, this returns false naming why and that ResetSequence()
	// comes first -- "routed to reset-then-bind" (R-S1f) is the caller's own retry, not something
	// this call performs. Otherwise it binds at the call when nothing is queued or in flight, or
	// holds the bind and applies it at the head of this sequence's next job on the worker (plan
	// §2.5 row 20 rule 4); a held bind Layer 1 then refuses faults the sequence's generation by
	// name. GetStats()/the bound name report it once that job has delivered.
	bool SetSchema(const FSuperSLMSequence& Sequence, const FSuperSLMSchemaHandle& Schema, FString& OutError);

	// Deferred, never rejected, when Sequence is mid-token or busy (§2.3, R-S1f: "CPU adapter swap
	// mid-token -> deferred to the token boundary"): the swap is held and applied at the head of
	// this sequence's next job at a token boundary (plan §2.5 row 20 rule 4), rather than
	// surfacing SSLM_ADAPTER_SWAP_MIDTOKEN_REJECTED to the caller. A held swap keeps its adapter
	// pinned, so FSuperSLMAdapterImport::Release() is refused until it resolves. An invalid
	// (Id == 0) Adapter clears to base.
	void RequestAdapterSwap(const FSuperSLMSequence& Sequence, const FSuperSLMAdapterHandle& Adapter);
	FSuperSLMAdapterHandle GetActiveAdapter(const FSuperSLMSequence& Sequence) const;

	// --- Generation (§5) ---

	// Queues a generation, which moves the sequence to Prefilling when its turn comes (§2.1, §5).
	// A sequence runs one generation at a time (D-SLM7946): the request is accepted only when the
	// sequence will be Idle at its turn -- Idle now with nothing queued, or a reset queued (or in
	// flight) after its last generation. Each generation after a sequence's first therefore needs
	// ResetSequence() first; call it, then this, in the same frame. A generation that continues a
	// completed one is not supported (D-SLM7945).
	//
	// Returns false, queueing nothing and changing nothing on the sequence, with the reason in
	// OutError: a sequence that is not live; the tick horizon; a sequence that will not be Idle at
	// its turn (OutError says "one generation at a time" and names the phase -- Prefilling,
	// Decoding, Complete or Faulted, from a queued or in-flight restore too -- or "already queued",
	// and names ResetSequence() as the remedy); a full per-sequence queue; and a malformed request.
	// While a reset queued ahead runs the sequence still reads its old phase and tokens, so a
	// caller that queued "reset, then generate" knows the new generation has ended only when the
	// phase reads Complete or Faulted AND GetPendingLifecycleOperationCount() reads 0; a phase
	// read between ticks can miss Prefilling and Decoding altogether. If Layer 1 refuses that
	// reset, or a prefix adoption queued ahead faults the sequence, the sequence is left Faulted
	// and the generation behind it is dropped at its turn, with a LogSuperSLM Warning, without
	// starting.
	bool BeginGeneration(const FSuperSLMSequence& Sequence, const FSuperSLMGenerationRequest& Request, FString& OutError);
	ESuperSLMSequencePhase GetPhase(const FSuperSLMSequence& Sequence) const;

	// The tokens produced so far; complete and stable once GetPhase() is Complete or Faulted.
	const TArray<int32>& GetGeneratedTokens(const FSuperSLMSequence& Sequence) const;

	// The outcome of Sequence's own most recent decode step (§5, §6) -- never a bare sentinel
	// int outside this module. On a Faulted sequence it names only SchemaDeadEnd or
	// SequenceNoLongerValid and reads Generating for every other fault; the fault's reason is
	// logged (LogSuperSLM Warning "Sequence N faulted: <reason>"), not returned.
	ESuperSLMDecodeOutcome GetLastDecodeOutcome(const FSuperSLMSequence& Sequence) const;

	// The copy delivered with this sequence's most recent job that changed it (reset, adopt,
	// restore, prefill or decode), read by sslm_stats on the worker at that job's tail -- never
	// a game-thread read of a live sequence (plan §2.5 row 22). Zeroed until the first such job
	// delivers, and at every hand-off.
	FSuperSLMSequenceStats GetStats(const FSuperSLMSequence& Sequence) const;

	// Layer 1's own tokenizer (sslm_tokenize), transported unchanged. R-S1a's own oracle
	// confirms a reference case's prompt text encodes to the SAME token ids T-2783 already
	// recorded, before comparing generated output ("the input token ids are confirmed equal
	// before outputs are compared", §9 R-S1a).
	bool Tokenize(const FString& Utf8Text, TArray<int32>& OutTokens) const;

	// --- Tick (§5, D-SLM7403/D-SLM7407 -- the async worker tick, T-2805 round 9) ---

	// Two phases, in order, and NEVER a Layer-1 call in either (R-S1b (ii), R-S1g):
	//   1. Apply -- for every worker job the worker has finished, copies its result onto its
	//      sequence(s) at the first tick after it finishes (D-SLM7728), per lane in FIFO order,
	//      and updates diagnostics. A result delivered after its planned tick (PlannedAtTick + K)
	//      is counted as a hitch.
	//   2. Plan -- sizes the next job (per due sequence, and at most one queued lifecycle
	//      operation) from the static per-unit cost model (FSuperSLMRuntimeConfig) against
	//      TickBudgetMs x BudgetHeadroom, computes its K = max(1, ceil(PlannedJobMs /
	//      TickBudgetMs)), and hands the whole job to a plugin-owned worker thread as one unit.
	// All Layer-1 calls -- prefill, decode, save, restore, reset, adopt, prefix begin/release,
	// adapter bind -- run exclusively on the worker, never inline inside an API call from the
	// game thread. Tick()'s own cost is the Plan/Apply bookkeeping only, independently bounded
	// (R-S1g); GetLastTickReport() reports it. Called once per game tick in production; the
	// automation suite calls it directly in a loop.
	void Tick(float DeltaSeconds);

	// Review W6: Tick(), unless this subsystem was already ticked during the current engine frame
	// (GFrameCounter) -- by another per-frame client, a game's own per-frame call, or anything else
	// that called Tick(). Every per-frame client of one subsystem calls this instead of Tick(), so
	// together they tick it once per frame and never double its rate. True when it ticked.
	bool TickOncePerFrame(float DeltaSeconds);

	// --- Save / restore (§4, D-SLM7408/D-SLM7421 -- queued lifecycle operations, round 9) ---

	// Queues the save and returns at once, exactly like ResetSequence(). Read the blob and the
	// named result through GetSaveResult(OutHandle, ...) once it drains -- never synchronously
	// from this call.
	ESuperSLMRestoreResult SaveSequence(const FSuperSLMSequence& Sequence, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError);

	// Vends a fresh sequence and queues its restore at once; OutSequence is the vended handle
	// (valid immediately, exactly like VendSequence()), OutHandle is the read-back key for the
	// restore's own named result (GetLifecycleOpResult(OutHandle), Pending until it drains).
	// The blob's backend tag and its Layer-1 tag are checked synchronously, before anything is
	// queued (§4; a blob saved under another Layer-1 pin is refused Layer1Mismatch, naming both
	// tags, plan §2.5 row 23); ExpectedModel's own artifact hash is what Layer 1's own
	// SSLM_RESTORE_MODEL_MISMATCH (a queued, drained-later result) is checked against.
	//
	// Plan §2.5 row 19: an accepted blob is held once, in a shared immutable buffer, and never
	// copied afterwards (in Tick() or anywhere else). This overload pays exactly one copy, at the
	// call; the overload below takes the caller's array by move and pays none.
	ESuperSLMRestoreResult RestoreSequence(
		const TArray<uint8>& Blob,
		USuperSLMModel* ExpectedModel,
		FSuperSLMSequence& OutSequence,
		FSuperSLMLifecycleOpHandle& OutHandle,
		FString& OutError);
	// Takes Blob by move when the request is accepted; a refused request leaves it untouched.
	ESuperSLMRestoreResult RestoreSequence(
		TArray<uint8>&& Blob,
		USuperSLMModel* ExpectedModel,
		FSuperSLMSequence& OutSequence,
		FSuperSLMLifecycleOpHandle& OutHandle,
		FString& OutError);

	// Reads a queued Reset/Adopt/Restore's own named result: Pending until its committed
	// delivery tick has come, then the outcome (D-SLM7421, plan §5 item 3). Save's own result is
	// read through GetSaveResult() instead, since it also carries a blob.
	ESuperSLMRestoreResult GetLifecycleOpResult(const FSuperSLMLifecycleOpHandle& Handle) const;

	// Reads a queued Save's own result and blob: Pending until the save is delivered, then the
	// outcome (plan §5 item 3: "Save's blob is delivered the same way, through the same handle").
	// Plan §2.5 row 19 (D-SLM7763): the first read that returns Success MOVES the blob out to
	// OutBlob -- no copy, and the handle's entry keeps only its result. Every later read of the
	// same handle returns Consumed with OutBlob empty. OutBlob is empty on every other result.
	ESuperSLMRestoreResult GetSaveResult(const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob);

	// Plan §2.5 row 19 (D-SLM7763): releases a resolved lifecycle-op handle's entry (its result,
	// and a save blob nobody took). Returns true when an entry was erased; false for an unknown
	// or already-released handle, and for one still Pending, which is left alone. Afterwards the
	// handle reads Pending (it names nothing). TearDown() releases every entry.
	bool ReleaseLifecycleOpHandle(const FSuperSLMLifecycleOpHandle& Handle);

	// The pooled-resource monitor's retention readings (plan §7 item 8, D-SLM7763): the bytes of
	// save blobs held in handle entries and not yet taken, and the number of handle entries not
	// yet released. Both read 0 once every save is taken and every handle released.
	int64 GetRetainedResultBytes() const;
	int32 GetLifecycleHandleEntryCount() const;

	// --- Shared prefixes (D-SLM7342, plan §5 item 4) ---

	// Queues a prefix over Tokens: its pool block is drawn on the tick queue
	// (sslm_prefix_begin), it is prefilled on worker lanes one whole token or more per tick
	// (sslm_prefix_prefill), then frozen (sslm_prefix_freeze) and Ready. Refused by name when
	// PrefixBlockCount prefixes are already live (false; OutError names PrefixBlockCount), and
	// refused for an empty, out-of-vocabulary or over-context_cap token list. Prefixes prefill
	// on the base model.
	bool CreatePrefix(const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FString& OutError);
	ESuperSLMPrefixPhase GetPrefixPhase(const FSuperSLMPrefix& Prefix) const;
	bool IsPrefixReady(const FSuperSLMPrefix& Prefix) const { return GetPrefixPhase(Prefix) == ESuperSLMPrefixPhase::Ready; }

	// Queues a whole-block copy of Prefix into Sequence (sslm_seq_adopt_prefix) exactly like
	// ResetSequence() (D-SLM7421): returns at once with OutHandle, or SequenceQueueFull before anything
	// is queued. Sequence must be Idle and not yet adopted; bind a schema before adopting. The
	// copy runs once Prefix is Ready and this sequence's own queue admits it. Read the drained
	// result through GetLifecycleOpResult(OutHandle). Afterwards BeginGeneration() may pass an
	// empty prompt (decode continues from the prefix) or the per-sequence suffix to prefill
	// after it.
	ESuperSLMRestoreResult AdoptPrefix(const FSuperSLMSequence& Sequence, const FSuperSLMPrefix& Prefix, FSuperSLMLifecycleOpHandle& OutHandle, FString& OutError);

	// sslm_prefix_release. Refused while an adopt of this prefix is still queued. Sequences
	// that already adopted it are unaffected (adoption is a copy).
	bool ReleasePrefix(const FSuperSLMPrefix& Prefix, FString& OutError);
	bool ReleasePrefix(const FSuperSLMPrefix& Prefix);

	// --- Diagnostics (§5.1; §9 R-S1b, R-S1e, R-S1g, R-S1i, R-S1j) ---

	// The game thread's own most recent Tick() cost (PlanMs + ApplyMs), read off the SAME report
	// GetLastTickReport() returns (R-S1g's own bound is independent of TickBudgetMs/worker cost).
	double GetLastTickDurationMs() const;

	// The most recent reset / adopt that DRAINED (D-SLM7341): a queued operation is timed when
	// the worker runs it, so read these after it has run --
	// GetPendingLifecycleOperationCount(Sequence) reaching 0 says it has.
	double GetLastResetMs() const;
	double GetLastAdoptMs() const;

	// D-SLM7421 (plan §5 item 3): the number of operations currently queued or in flight for
	// THIS sequence (0 to Config.MaxQueuedOperationsPerSequence) -- generalized from a single
	// outstanding-operation flag to a queue depth. A coarse "everything for this sequence has
	// settled" check; a caller that needs a SPECIFIC call's own result reads it by that call's
	// own handle instead.
	int32 GetPendingLifecycleOperationCount(const FSuperSLMSequence& Sequence) const;

	// D-SLM7407/D-SLM7414 (plan §5, §9 R-S1b/R-S1i/R-S1j, T-2805 round 9): the worker jobs the
	// Plan step has committed since the last Configure() call, oldest first. Each entry's own
	// composition, static-cost-model PlannedJobMs, K, and committed/actual delivery ticks are the
	// mechanism R-S1b's five threshold-free assertions, R-S1i's cross-run delivery-tick identity
	// comparison, and R-S1j's per-request arrival-order/delivery checks all read.
	//
	// The ledger is a fixed ring created at Configure(): it keeps the most recent rows (1024 by
	// default) and overwrites the oldest, so it never grows and a request never allocates for it.
	// GetJobLedger() returns a COPY of the retained rows -- hold it, not a pointer into a
	// temporary. FindJobLedgerRow() reads one row in place. A row's number counts every job since
	// Configure(), from 0; GetJobLedgerAppendedCount() is the next row's number, and the retained
	// rows are the last GetJobLedger().Num() numbers before it.
	TArray<FSuperSLMWorkerJobReport> GetJobLedger() const;
	int64 GetJobLedgerAppendedCount() const;
	// Row Row, or null once the ring has overwritten it (or before it exists). The pointer is
	// valid until the next Tick() or Configure().
	const FSuperSLMWorkerJobReport* FindJobLedgerRow(int64 Row) const;

	// D-SLM7340 (plan §9 R-S1b): the measured costs the static cost model prices but does not
	// itself remeasure at runtime -- reported telemetry only, never fed back into Plan/K
	// (D-SLM7414: "the live p99 estimator is retained, retargeted to telemetry only").
	// The most recent prompt-prefill call's milliseconds per prompt token (sequence or prefix).
	double GetLastPrefillMsPerToken() const;
	// The most recent token-finishing step (final norm + logits + argmax): a decode call that ran
	// no layers (the first token after a prefill or adopt) measured alone, per sequence; or, for
	// a call that ran a sequence's last layers and its finish together, that call's time less
	// its layers at the measured per-layer cost.
	double GetLastFinishMs() const;
	// D-SLM7505/D-SLM7506/D-SLM7509 (plan §5.1): real tokens/second over a rolling, MEASURED
	// (never assumed) 1.0-real-second window -- (TokensFinishedTotal_now - TokensFinishedTotal_at
	// the oldest ring-buffer sample whose age is <= 1.0s) / (RealSeconds_now - RealSeconds_at that
	// sample), or the oldest sample actually retained if less than 1.0s of history exists yet.
	// Reads exactly 0.0 during a gap with no deliveries, before two samples exist, or whenever the
	// oldest in-window sample has zero age -- never the last nonzero value, never NaN. A product
	// reading first (mirrors GetLastFinishMs()/GetLastPrefillMsPerToken()'s existing shape, §7 item
	// 7's own "these are the same readings §5.1's Insights counters expose, read here without
	// opening a capture"), a test convenience second.
	double GetTokensPerSecond() const;
	// Wall-clock milliseconds from BeginGeneration() to the tick that produced the sequence's
	// first token; negative until that token exists.
	double GetTimeToFirstTokenMs(const FSuperSLMSequence& Sequence) const;
	// What the most recent Tick() did -- Plan/Apply bookkeeping only (R-S1g). Per-job
	// composition, cost, K and delivery are GetJobLedger()'s, not this report's (D-SLM7407).
	const FSuperSLMTickReport& GetLastTickReport() const;

	// Every Tick() call's own report since the last Configure() call, oldest first (T-2805 round
	// 9). R-S1g's own bound ("stays under 1 ms per tick") is a property of EVERY tick in a run, not
	// only the most recent one -- this is what that cell reads. A fixed ring like the job ledger,
	// with the same three accessors: a copy, the appended count, and one row in place.
	TArray<FSuperSLMTickReport> GetTickHistory() const;
	int64 GetTickHistoryAppendedCount() const;
	const FSuperSLMTickReport* FindTickHistoryRow(int64 Row) const;

	// D-SLM7407 (plan §5, "What counts as a hitch"): incremented for every worker job whose
	// committed delivery tick has come and the worker has not yet produced it (a late result),
	// and for every single Layer-1 call inside a job whose own measured time exceeds
	// Config.TickBudgetMs outright, reported and never hidden by K's own headroom. This is the
	// CPU path's own hitch counter, distinct from the GPU scheduler's T+k hitch count (L2-S2).
	// Never the game thread's own tick duration -- that is no longer a hitch source (D-SLM7407
	// retires the synchronous tick's frame-overrun reading; R-S1g bounds Tick() independently).
	int32 GetHitchCount() const;

	int32 GetPoolFreeCount() const;
	int32 GetPoolOccupiedCount() const;
	// Prefixes created through CreatePrefix() and not yet released. A re-Configure() tears them
	// down with the rest of the state, so the Load node refuses while any is held (review R5-N3).
	int32 GetLivePrefixCount() const;

	// Bytes the pool/workspace allocation actually reserved for the current Config -- R-S1e's
	// own cross-check: "the inspector's footprint figures equal the bytes the subsystem
	// allocates for the same sslm_config."
	int64 GetKvPoolReservedBytes() const;
	int64 GetWorkspaceReservedBytes() const;

	// The model the most recent successful Configure() mapped, or null. The determinism
	// self-check (L2-S2, SuperSLMDeterminismSelfCheck.h) reads it to confirm the CPU and GPU
	// subsystems it drives are configured with the same artifact.
	USuperSLMModel* GetConfiguredModel() const { return ConfiguredModel; }

	//~ Begin UObject
	// Tears down any runtime state Deinitialize() did not (a subsystem destroyed without its
	// collection's Deinitialize pass), so no Layer-1 handle outlives the object.
	virtual void BeginDestroy() override;
	//~ End UObject

private:
	// Test-only (WITH_DEV_AUTOMATION_TESTS): FSuperSLMSchedulingTestAccess sets the tick counter
	// (plan §10.3.1 item 5a.3, D-SLM7799) and the report rings' capacity.
	// SuperSLMSchedulingTestAccess.h.
	friend struct FSuperSLMSchedulingTestAccess;

	// Every Layer-1 handle, buffer, slot, and scheduler figure (SuperSLMSubsystem.cpp). Owned
	// here and freed by Deinitialize()/BeginDestroy(); held by pointer so this reflected header
	// includes no Layer-1 header.
	FSuperSLMSubsystemState* State = nullptr;

	/** The configured model, referenced so it is not collected while its mapping serves this subsystem's sequences. */
	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> ConfiguredModel;

	// BeginConfigure()'s supersession check: TearDown() increments it, so a pending prepare whose
	// serial is no longer current is discarded. Game thread.
	// The GFrameCounter of the last Tick() (review W6); TickOncePerFrame() skips while it is current.
	uint64 LastTickFrame = MAX_uint64;

	uint32 ConfigureSerial = 0;
	bool bConfigurePending = false;

	// Review W5: every BeginConfigure() whose pool-thread prepare has not yet been claimed by its
	// game-thread continuation. Deinitialize() waits for each and frees what it built, so a load
	// in flight at exit neither leaks its state nor leaves its lane threads running into module
	// unload. Game thread.
	TArray<TSharedPtr<FSuperSLMCpuConfigureFlight, ESPMode::ThreadSafe>> InFlightConfigures;

	void TearDown();
};
