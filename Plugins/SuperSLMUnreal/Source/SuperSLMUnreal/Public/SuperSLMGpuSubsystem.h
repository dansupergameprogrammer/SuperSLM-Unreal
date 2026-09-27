#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuTypes.h"
// Reused, backend-neutral shapes from the existing runtime module (read-only include;
// none of these three types is edited here, and none names "CPU" or carries
// a CPU-specific field -- SuperSLMSequenceTypes.h's own header comment already frames
// FSuperSLMSequence/ESuperSLMSequencePhase/etc. as the subsystem's generic vended-handle
// vocabulary). Reusing them here, rather than declaring GPU-specific duplicates, is what
// lets R-S2a directly diff USuperSLMGpuSubsystem::GetGeneratedTokens() against
// USuperSLMSubsystem::GetGeneratedTokens() with no conversion step ("tokens equal the CPU
// backend's for the same session").
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMGpuSubsystem.generated.h"

class USuperSLMModel;
struct FSuperSLMGpuSubsystemState;
struct FSuperSLMGpuConfigureFlight;

// L2-S2 (the plan §2.1, §4, §5, §7 item 11, §8, §10.3;
// Coverage Model §9, cells R-S2a-R-S2g; the red-suite record).
// A SEPARATE GameInstance subsystem from the CPU backend's USuperSLMSubsystem
// (SuperSLMSubsystem.h) -- two subsystems, not two modules: D-SLM7337 narrows
// D-SLM7230's earlier separate-module choice, because Layer 1's GPU sources call its
// CPU-core symbols, which are compiled into (and exported from no DLL by) this SAME
// module, `SuperSLMUnreal`, guarded to Win64 (`SUPERSLMUNREAL_WITH_GPU`). This
// `UCLASS` exists on every platform (UHT cannot see a platform `#if`); off Win64,
// `ShouldCreateSubsystem` returns false and every body compiles out. "Two backends
// behind one Blueprint-facing interface" (D-SLM3534) is realized at L2-S3 (§10.4),
// which unifies the two subsystems' surfaces behind Blueprint entry points; this class
// is the GPU backend's own C++ entry point for L2-S2, exactly as USuperSLMSubsystem is
// the CPU backend's.
//
// Every method here is a C++ entry point the plugin's own automation tests call
// directly, matching USuperSLMSubsystem's own established convention
// (SuperFAISSSwarmSubsystem::Step()-in-a-loop, not a real per-frame Tick callback) --
// see SuperSLML2S2Fixtures.h's own RunGpuGenerationToCompletion() helper.

/** The GPU (D3D12) inference backend, one per GameInstance. Windows x64 only; elsewhere it is not created. */
UCLASS()
class SUPERSLMUNREAL_API USuperSLMGpuSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UGameInstanceSubsystem
	// Enforced teardown order (plan §9 R-S2d: "shutdown follows the enforced order
	// (unbind -> adapter unmap -> model unmap -> context destroy) with no refusal"),
	// even with sequences still vended -- every bound adapter is unbound from every
	// sequence, every mapped adapter is unmapped, the model is unmapped
	// (SSLM_MODEL_HAS_LIVE_ADAPTERS never fires because adapters were already unmapped
	// in step 2), and the context is destroyed (SSLM_CONTEXT_HAS_LIVE_HANDLES never
	// fires because every sequence was already released).
	virtual void Deinitialize() override;
	//~ End UGameInstanceSubsystem

	// Verifies every compiled shader the build lists is staged in the plugin's own
	// Binaries/Win64/shaders (ShaderStagingIncomplete otherwise), creates the GPU context with
	// that directory, absolute, as GpuContextConfig::shader_dir (plan §2.5 row 1; a refused
	// directory fails as DeviceUnavailable with the directory and status named), installs the
	// host parallel-for finish hook at Config.FinishParallelTasks (row 2), maps Model with the
	// device-resident head unless Model->bGpuDeviceResidentHead is off -- falling back to the
	// host head once, reported by name, on an out-of-memory refusal (row 3) -- and validates
	// Config.K against the floor for BlockCount concurrent sequences
	// (D-SLM7381; SuperSLMGpuRuntimeConfig.h, K) -- KBelowMinimum, never a silent clamp, if
	// Config.K is too small. A ContextCap/BlockCount/DispatchBudget <= 0 (or resolving to
	// fewer than one whole layer, for DispatchBudget), and a MaxQueuedOperationsPerSequence < 1
	// (InvalidMaxQueuedOperationsPerSequence, D-SLM7475 -- a bound below 1 would refuse every
	// queued lifecycle op), are each refused with a named result and no device allocation
	// (mirrors USuperSLMSubsystem::Configure()'s own "a budget-exceeding config never
	// allocates" discipline, SuperSLMSubsystem.h).
	// Idempotent, matching USuperSLMSubsystem::Configure()'s own convention: re-calling
	// tears down any prior context/pool and rebuilds.
	FSuperSLMGpuConfigureReport Configure(USuperSLMModel* Model, const FSuperSLMGpuRuntimeConfig& Config);

	// Configure() without blocking the calling thread (fold-round ruling 1: a load must not block
	// the editor). Game thread: tears down a prior state, which queues a teardown job behind every
	// job already on the submission thread, waits for all of them to run and then joins the
	// thread, so the wait is the queued backlog, not one job; if that thread is already
	// unresponsive, it is leaked instead, with no wait and no join, and if it stops responding
	// during the wait, the wait lasts until the stuck call overruns its time bound and the thread
	// is then leaked (the first call has nothing to tear down and does not wait), captures Model's bytes (holding Model alive) and resolves the plugin's shader directory. Pool thread: the artifact view load, the
	// per-layer marshal, the schema parse and the submission thread's creation, then the wait on
	// the three device jobs, which run on that submission thread as they do under Configure().
	// Game thread again (a queued task, a later frame): the state is published, registered and
	// OnDone receives the report Configure() would have returned. Until then the backend is
	// inactive (IsConfigurePending() is true). A later Configure(), BeginConfigure() or
	// Deinitialize() supersedes it: its state is shut down on a pool thread and OnDone is not
	// called. Without the GPU build it runs Configure()'s refusal and calls OnDone at once.
	void BeginConfigure(USuperSLMModel* Model, const FSuperSLMGpuRuntimeConfig& Config, TUniqueFunction<void(const FSuperSLMGpuConfigureReport&)> OnDone);
	bool IsConfigurePending() const { return bConfigurePending; }

	// True once Configure() has succeeded and no confirmed-terminal device loss (plan
	// §5: "on a terminal result tear the GPU backend down and fall back to the CPU
	// backend") has occurred since. False after Configure() fails, after
	// Deinitialize(), or after a terminal SSLM_DEVICE_LOST (R-S2c: "the probe finds the
	// context usable, and the next tick runs on the GPU" is the case where this STAYS
	// true through a recoverable rejection).
	bool IsGpuBackendActive() const;

	// Plan §2.5 row 1 (D-SLM7662): the absolute shader directory the most recent Configure()
	// resolved and, if it got that far, passed to Layer 1 (<plugin>/Binaries/Win64/shaders). Kept
	// whether or not that Configure() succeeded; empty before the first call or when the plugin
	// could not be resolved. Read by the cost readout, R-S2i and R-S2j.
	FString GetShaderDirectory() const;

	// Diagnostic only (plan §9 R-S2j, U1): DXGI's LOCAL-segment CurrentUsage on Layer 1's dispatch
	// adapter, sampled on the submission thread immediately before and immediately after the
	// sslm_gpu_model_map that succeeded in the most recent Configure() (after an out-of-memory
	// fallback, around the retried map). -1 when not sampled (before the first Configure(), a
	// failure before the map, or an unreadable adapter). Kept whatever that Configure()'s outcome.
	int64 GetPreMapLocalVideoMemoryBytes() const;
	int64 GetPostMapLocalVideoMemoryBytes() const;

	// Plan §2.5 row 3 (D-SLM7664): whether the configured model is mapped with the
	// device-resident head, and where its logits run with the reason when not on the device (the
	// asset turned it off, or an out-of-memory map fell back to the host). Never a silent switch.
	bool IsDeviceHeadActive() const;
	FString GetDeviceHeadStatus() const;

	// --- Sequence vend/return/reset (§5) ---

	// Vends one GPU sequence from the warm pool with DecodePath fixed for this
	// sequence's lifetime until the next ResetSequence() (§5, D-SLM7256 rule 3: "never
	// switches").
	ESuperSLMGpuVendResult VendSequence(FSuperSLMGpuSequence& OutSequence, ESuperSLMGpuDecodePath DecodePath);
	void ReturnSequence(const FSuperSLMGpuSequence& Sequence);

	ESuperSLMGpuDecodePath GetDecodePath(const FSuperSLMGpuSequence& Sequence) const;

	// --- Schema binding (§2.1; R-S2g) ---

	// Never waits on the submission thread (plan §2.5 row 21, ruling 2026-09-26). The bind is
	// always held as an entry in the sequence's per-sequence queue and reaches Layer 1 on the
	// submission thread behind every op and job already queued for it, in call order (D-SLM7758's
	// held bind, now the only path). A bind that is the submitted front does not fill the window:
	// it is left out of MaxQueuedOperationsPerSequence's count and of
	// GetPendingLifecycleOperationCount(), so a RequestBeginGeneration() after it is admitted to the
	// queue even at a bound of 1; a bind still queued behind other entries counts as an ordinary
	// entry.
	// True means the bind is accepted and ordered ahead of every later request on this sequence,
	// so a RequestBeginGeneration() issued next runs with it. Because the bind is held, that
	// request does not admit at its call: GetPhase() stays Idle, not Prefilling, until the tick
	// after the bind's job finishes. False names each refusal the game
	// side can see: the sequence is not live; it is not bind-eligible -- Layer 1 binds only a
	// created or reset sequence that has not since generated or been restored (bind eligibility,
	// 1.6.0; plan §2.5 row 4), so this names that ResetSequence() comes first, without asking
	// Layer 1, matching USuperSLMSubsystem::SetSchema()'s CPU-side "routed to reset-then-bind"
	// convention; the schema is unknown; or the queue is full. GetBoundSchema() reports the schema
	// once the bind has reached Layer 1 (the tick that finalizes it), so it lags this call, and a
	// Layer-1 refusal faults this sequence's next generation by name.
	bool SetSchema(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGpuSchemaHandle& Schema, FString& OutError);
	FSuperSLMGpuSchemaHandle GetBoundSchema(const FSuperSLMGpuSequence& Sequence) const;

	// SslmGpuSeqWalkStateForG5Bridge's own return (gpu_1p0.h), unchanged -- the plugin does not
	// recompute it -- as of the last applied result or finalized op: the submission thread reads it
	// after each token's finish and after the bind, reset and restore jobs, and the game side
	// stores it when that result applies (T+K) or that op finalizes. It never waits on the
	// submission thread (plan §2.5 row 21, ruling 2026-09-26). 0 means "at the schema's own start
	// state" (a fresh bind or a reset sequence); kSslmGpuDfaWalkStateUnused (0xFFFFFFFF) means no
	// schema is bound, and is what a newly vended sequence and an invalid handle read. Since the
	// re-pin to 1.7.0 a save carries the walk ('SLM5'), so a restored sequence resumes at the walk
	// state it was saved at. After a restore the value is the restore job's reading -- the walk
	// after every carried token -- so it may lead the applied tokens by the carried ones until they
	// apply (carried results store nothing). A refused bind stores Layer 1's unchanged walk.
	uint32 GetSchemaWalkState(const FSuperSLMGpuSequence& Sequence) const;

	// Plan §2.5 row 12, §10.4: whether Sequence's schema walk was accepting after its most recent
	// token was finished -- Layer 1's SslmGpuSeqSchemaAcceptingForG5Bridge, read on the submission
	// thread right after that token's finish (the bridge reports pre-finish membership until then)
	// and carried back with the token's result, so this never blocks. It reads the reading of the
	// last result APPLIED (T+K), the same one GetGeneratedTokens() reflects; a schema dead end
	// leaves the walk where it was, so its reading is the last token's. False when unbound, before
	// the current generation's first token, and for an invalid handle.
	bool IsSchemaAccepting(const FSuperSLMGpuSequence& Sequence) const;

	// --- Per-sequence slicing (plan §8 Frame Budget; §10.4 query window) ---

	// Caps Sequence's composed-path layers per slice at LayersPerSlice, never above the
	// Configure()'d DispatchBudget's own LayersPerTick (the per-tick budget is shared, §5); 0
	// clears the cap. Only while Sequence is not generating (Idle, Complete or Faulted), so a
	// token's slicing never changes mid-token; false naming why otherwise. Moves only
	// frames-to-answer and the cost figures, never a token (R-S2e). A vend or return resets it.
	bool SetLayersPerSlice(const FSuperSLMGpuSequence& Sequence, int32 LayersPerSlice, FString& OutError);

	// The layers per slice Sequence's composed-path tokens are sliced at now (the cap above,
	// bounded by LayersPerTick); 0 for an invalid handle.
	int32 GetLayersPerSlice(const FSuperSLMGpuSequence& Sequence) const;

	// The Configure()'d fixed-tick latency K and the floor Configure() computed for it
	// (FSuperSLMGpuConfigureReport::MinimumK); 0 when not configured.
	int32 GetConfiguredK() const;
	int32 GetConfiguredMinimumK() const;

	// Re-sets the fixed-tick latency K on a configured subsystem without a re-Configure(), so a
	// query can run at a K other than the Configure()'d one (§10.4 query window; the R-S3e sweep
	// varies k). K is a scheduler constant only -- it sets each result's apply tick and the hitch
	// check, and no device state reads it -- so this is O(1) game-thread work: nothing is mapped,
	// allocated or submitted. Bounded exactly as Configure() bounds it ([GetConfiguredMinimumK(),
	// 2^20], D-SLM7381/D-SLM7803). Refused while any sequence is vended (a deferred return
	// included) or any restore is queued, since a live sequence's already-scheduled apply ticks
	// were computed at the old K; a caller serializes by waiting for the pool to drain. False
	// naming why otherwise.
	bool SetFixedTickLatency(int32 K, FString& OutError);

	// The whole layers one tick's DispatchBudget covers on the composed path, shared by every due
	// composed sequence (§5); 0 when not configured.
	int32 GetLayersPerTick() const;

	// L2-S3 code review S4 (plan §8: Frame Budget "sets the composed path's dispatch_budget, in
	// whole layers per slice", and concurrent queries "share the tick's budget" through the batch
	// call). Re-sets the tick's composed-path budget to LayersPerTick whole layers (capped at the
	// model's depth; DispatchBudget becomes LayersPerTick x the model's dispatches per layer)
	// without a re-Configure(), so Frame Budget bounds what ONE tick issues whatever the number of
	// concurrent sequences. Like K, this is a scheduler constant with no device state behind it:
	// O(1) game-thread work. It moves D-SLM7381's floor, which is recomputed exactly as Configure()
	// computes it (GetConfiguredMinimumK()); a K below the new floor is raised to it
	// (GetConfiguredK()). Refused, like SetFixedTickLatency(), while any sequence is vended or any
	// restore is queued. False naming why otherwise.
	bool SetLayersPerTick(int32 LayersPerTick, FString& OutError);

	// Review round 2, R2-W3. SetLayersPerTick() and SetFixedTickLatency() change a schedule every
	// client of this subsystem shares. A client that overrode it calls this when its sequences are
	// returned: the Configure()'d LayersPerTick, DispatchBudget, floor and K come back at once if the
	// pool is drained, else at the first Tick() that finds it drained (the overrides' own gate). A
	// later override before then cancels the pending restore. Game thread.
	// It restores the Configure()'d values, not whatever was set before the override: a client's
	// own SetFixedTickLatency()/SetLayersPerTick() made before a query is replaced by the configured
	// value when that query's restore lands (review round 3, R3-N1). Re-apply it after the query.
	void RequestConfiguredScheduleRestore();

	// --- Adapter mapping and swap (§2.1) ---

	// sslm_gpu_adapter_map against Base (already mapped via a prior Configure()) --
	// SSLM_ADAPTER_MODEL_MISMATCH surfaces as a false return naming the mismatch,
	// matching FSuperSLMAdapterImport::ImportFromFile's own CPU-side shape
	// (SuperSLMAdapterHandle.h).
	// Waits on the GPU submission queue; do not call on the frame path (plan §2.5 row 21, ruling
	// 2026-09-26, round 2). The same holds for UnmapAdapter() below.
	bool MapAdapter(const FString& AbsolutePath, USuperSLMModel& Base, FSuperSLMGpuAdapterHandle& OutHandle, FString& OutError);
	// Waits on the GPU submission queue; do not call on the frame path. A no-op, keeping the
	// mapping, once IsGpuBackendActive() is false (round 3): teardown unmaps it.
	void UnmapAdapter(const FSuperSLMGpuAdapterHandle& Adapter);

	// Refused (SSLM_BUSY, gpu_1p0.h) while Sequence is genuinely mid-token; the
	// PLUGIN's own composed-path pin (D-SLM7256 rule 4: "the adapter passed to every
	// slice of one token is the one pinned at that token's embed") means a swap
	// requested mid-token is deferred to the next token boundary, mirroring
	// USuperSLMSubsystem::RequestAdapterSwap()'s own CPU-side deferral convention
	// (SuperSLMSubsystem.h) rather than surfacing SSLM_BUSY to the caller. An invalid
	// (Id == 0) Adapter clears to base. GetActiveAdapter() reports the swap only once a token
	// result produced after the bind has been applied (plan §2.5 row 20 rule 4). Ignored once
	// IsGpuBackendActive() is false (round 3).
	void RequestAdapterSwap(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGpuAdapterHandle& Adapter);
	FSuperSLMGpuAdapterHandle GetActiveAdapter(const FSuperSLMGpuSequence& Sequence) const;

	// --- Generation (§5, §8) ---
	//
	// BeginGeneration is one of the per-sequence software queue's five request kinds
	// (D-SLM7421: "Requests -- Reset, Adopt Prefix, Save, Restore, and a decode/prefill
	// submission ... run in strict arrival order, one in flight at a time"); it is issued
	// through RequestBeginGeneration() below, not a synchronous entry point here.

	ESuperSLMSequencePhase GetPhase(const FSuperSLMGpuSequence& Sequence) const;
	const TArray<int32>& GetGeneratedTokens(const FSuperSLMGpuSequence& Sequence) const;
	// On a Faulted sequence: SchemaDeadEnd for a schema dead end, Generating for every other
	// fault. The fault's reason is logged (LogSuperSLM Warning "GPU sequence N faulted: <reason>"),
	// and GetLastFaultReason() below gives its category.
	ESuperSLMDecodeOutcome GetLastDecodeOutcome(const FSuperSLMGpuSequence& Sequence) const;

	// Non-None only immediately after a fault -- see ESuperSLMGpuFaultReason
	// (SuperSLMGpuTypes.h). R-S2c's own cell asserts this is
	// RecoverablePerSequenceRejection (never TerminalDeviceLost) for its saturated-cap
	// construction.
	ESuperSLMGpuFaultReason GetLastFaultReason(const FSuperSLMGpuSequence& Sequence) const;

	// Probes the context (plan §5: "probe the context once") and reports whether it still
	// answers -- the plugin's own disambiguation of gpu_1p0.h's documented SSLM_DEVICE_LOST
	// ambiguity. NOT side-effect-free: it creates a scratch sequence, embeds a token, runs one
	// full layer, drains and releases it -- a real GPU submission that moves
	// GetGpuDispatchCount() (finding 7, T-2885 review). Under SuperSLM 1.8.0's contract any of
	// those steps may instead fail with SSLM_GPU_ALLOCATION_FAILED, which states the device and
	// the context stay valid: the probe then stops early and returns true (recoverable) provided
	// the device does not report removal. If that happens at the scratch sequence's creation, the
	// probe returns true having submitted nothing and without moving GetGpuDispatchCount(); that
	// outcome is inconclusive about the submission list and is re-probed on the next
	// SSLM_DEVICE_LOST. A device whose setup ran out of memory also reads true (it is set up
	// again by the next call). A false return also latches
	// bTerminalLoss, tearing the GPU backend down on the next Tick() -- a caller that probes a
	// transiently unavailable context loses the backend permanently, so this is not a query to
	// call speculatively. Called by the plugin itself after every SSLM_DEVICE_LOST; also
	// exposed here so R-S2c's own test can assert the probe's result directly rather than only
	// its downstream effect (IsGpuBackendActive() staying true).
	// Waits on the GPU submission queue; do not call on the frame path (plan §2.5 row 21, ruling
	// 2026-09-26, round 2). False, without probing, once IsGpuBackendActive() is false (round 3).
	bool ProbeContextUsable();

	// --- Tick (§5) ---

	// Drives every due sequence's own next slice: Composed-path sequences share
	// Config.DispatchBudget's worth of layers per tick, through the batch call
	// (sslm_decode_step_batch_gpu) when more than one is due, with the array order ROTATED
	// each tick so no sequence starves (§5); OneCall-path sequences are served one whole
	// token per tick in rotation (SslmGpuSeqDecodeStepForG5Bridge, or embed plus a
	// full-depth layer call for a prompt token, D-SLM7379). Drains queued restores at the START of the tick,
	// before any slice (§5: "the submission thread issues restores at the start of a
	// tick, before that tick's slices"), and applies each token's result at exactly T+K when the
	// device has finished it. Never waits on the submission thread (plan §2.5 row 21, ruling
	// 2026-09-26): an event whose T+K tick has come but whose job has not finished is not applied;
	// it applies on the first later tick at which its job has finished, and every later event of
	// its sequence applies after it, in order, never before its own T+K tick. A hitch is counted
	// per event, each examined once at its own apply tick (GetHitchCount()): a token planned after
	// its T+K tick, or an unfinished job whose device was given at least the sim time since its
	// request; one held back only by an earlier overdue event counts nothing, so one late job
	// shared by N sequences counts N. New tokens are held under back-pressure and the cap: a
	// sequence this tick's apply left holding an overdue event starts no new token, and no new
	// token starts on either path while K or more tick jobs are outstanding; a composed token
	// already in flight always continues. On every tick with any job outstanding, a Layer-1 call
	// past its row-21 bound takes the terminal device-loss path. The tick that confirms a terminal
	// loss tears down asynchronously; it never waits: every live sequence faults by name and
	// IsGpuBackendActive() reads false from that tick, queued work completes Skipped without
	// touching the device, and a later Tick() (or TearDown()) finishes releasing it. Results are
	// never reordered.
	// Updates every diagnostics accessor below.
	void Tick(float DeltaSeconds);

	// Review W6: Tick(), unless this subsystem was already ticked during the current engine frame
	// (GFrameCounter) -- by another per-frame client, a game's own per-frame call, or anything else
	// that called Tick(). Every per-frame client of one subsystem calls this instead of Tick(), so
	// together they tick it once per frame and never double its rate. True when it ticked.
	bool TickOncePerFrame(float DeltaSeconds);

	// --- The per-sequence software queue (accepted async-tick plan, D-SLM7418/D-SLM7421/
	// D-SLM7424; D-SLM7457's ruling: this queue is the ONLY lifecycle API the GPU subsystem
	// exposes -- Save/Restore/Reset/BeginGeneration's prior synchronous forms are removed;
	// there is no parallel synchronous API) ---
	//
	// Built against the current GPU backend, T-2826 round 5, 2026-09-19 (plan §5 GPU path /
	// §10.3 item 8; R-S2d's collision arm). A request against a sequence already Submitted or
	// already holding a queued operation is appended to that sequence's own queue instead of
	// being forwarded to Layer 1, and drains in arrival order once each entry ahead of it
	// completes (plan §5: "never a block on worker progress" -- D-SLM7402). Every Request*
	// call below returns its own opaque FSuperSLMLifecycleOpHandle AT ONCE, whether the
	// operation starts this tick or waits; the caller reads the result back later through
	// GetLifecycleOpResult()/GetSaveResult()/GetRestoreResult(), never through a blocking wait
	// inside the call that queued it. A request against a sequence already holding
	// Config.MaxQueuedOperationsPerSequence queued entries is refused AT ONCE, before anything
	// is queued and before Layer 1 is asked: the returned handle is invalid (IsValid() ==
	// false) and GetLifecycleOpResult() on it is undefined -- a caller checks IsValid() on the
	// RETURNED handle itself to learn whether the request was admitted, matching
	// ESuperSLMGpuVendResult's own "check the result, not a side channel" convention elsewhere
	// on this class. A busy sequence is otherwise no reason to refuse, with one exception: a
	// sequence runs one generation at a time (D-SLM7946), so RequestBeginGeneration() is refused
	// at the call, the same way (invalid handle, GetLastLifecycleRequestError() naming the rule),
	// unless the sequence will be Idle when the request's turn comes. That refusal comes before
	// the bound's. No call reachable through this queue ever surfaces Layer 1's own SSLM_BUSY
	// to the caller -- structurally, the same reason sslm_gpu_seq_restore already never does
	// (plan §5): the plugin never asks Layer 1 a question whose answer could be SSLM_BUSY.
	//
	// Cross-sequence admission order (D-SLM7428, mirroring the CPU backend's own rule, §5 CPU
	// path item 3): when more than one sequence has an eligible, not-yet-submitted operation at
	// the front of its own queue in the same tick's admission pass, the one admitted first is
	// the eligible sequence whose operation carries the lowest global request ordinal -- a
	// single counter incremented once per Request*() call, read at that call and never revised
	// afterward. This is a pure function of request arrival order, never container iteration
	// order, a hash, or a pointer value.
	//
	// RequestResetSequence() and RequestSaveSequence() carry the same contracts the prior
	// synchronous ResetSequence()/SaveSequence() did -- just queued rather than blocking, so
	// they take the same arguments, minus the OutError the queued shape has no synchronous
	// return path for. A reset requested while a generation runs interrupts it: from the reset's
	// admission no token of that generation is planned or applied. Until the reset takes effect
	// the sequence keeps its phase and tokens (Complete included), so a call that needs a sequence
	// that is not generating -- SetLayersPerSlice(), which refuses "the sequence is generating" --
	// is refused while a reset requested mid-run is still running; drive the reset's handle to
	// resolution first. A reset that succeeds clears the generation state (Idle, no tokens) and
	// applies NewDecodePath. A reset Layer 1 refuses resolves Malformed, the status named in
	// GetLastLifecycleRequestError() (plan §2.5 row 16), and leaves the sequence Faulted with the
	// previous generation's tokens still readable (D-SLM7948), so its next generation is refused
	// until a reset succeeds.
	//
	// Emits Layer 1's 'SLM5' blob (sslm_gpu_seq_save, 1.6.0) tagged with ESuperSLMBackend::GPU
	// and the Layer-1 pin (mirrors USuperSLMSubsystem::SaveSequence()'s own CPU-side tagging
	// convention, SuperSLMSubsystem.h), plus the plugin's OWN "primed" bit (D-SLM7256 rule 2)
	// and the bound schema's name. 'SLM5' carries the schema binding, its walk and
	// ready_for_logits, so a sequence saves at any token boundary, schema-bound or not, advanced
	// or not (plan §2.5 row 6, D-SLM7666); a composed token in flight is first carried to its
	// boundary. Only a Faulted sequence resolves SaveRefused. The remaining token budget the blob
	// records counts the tokens already applied to the game (row 24), the tokens the thread has
	// produced beyond them travelling as carried results.
	FSuperSLMLifecycleOpHandle RequestResetSequence(const FSuperSLMGpuSequence& Sequence, ESuperSLMGpuDecodePath NewDecodePath);
	FSuperSLMLifecycleOpHandle RequestSaveSequence(const FSuperSLMGpuSequence& Sequence);

	// RequestBeginGeneration() queues a generation on the sequence's own queue, behind whatever is
	// already queued or in flight, like Reset/Save. A sequence runs one generation at a time
	// (D-SLM7946): the request is accepted only when the sequence will be Idle when its turn
	// comes -- Idle now with nothing queued, or a reset queued (or admitted and not yet resolved)
	// after its last generation. Each generation after a sequence's first therefore needs
	// RequestResetSequence() first; call it, then this, in the same frame. A generation that
	// continues a completed one is not supported (D-SLM7945).
	//
	// Refused at the call, with nothing queued and nothing on the sequence changed (invalid
	// handle; GetLastLifecycleRequestError() names the reason): a sequence that is not live; a
	// sequence that will not be Idle at its turn (the error says "one generation at a time" and
	// names the phase -- Prefilling, Decoding, Complete or Faulted -- or "already queued", and names
	// RequestResetSequence() as the remedy; for Faulted, also returning the sequence and restoring
	// a save, which gives a new sequence); a full per-sequence queue; and a malformed request (the
	// request SHAPE: prompt non-empty, MaxNewTokens positive, every prompt token inside the
	// vocabulary, SpanKind::SchemaContent refused on this backend -- none of it needs Layer 1 or
	// the sequence's own turn).
	//
	// "Delivered" for this op means ADMITTED -- GetLifecycleOpResult() resolves to Success once
	// the queued request has actually STARTED (the sequence has left Idle for the NEW request);
	// the generation then continues to Complete/Faulted, read through GetPhase()/
	// GetGeneratedTokens(). While a reset queued ahead of it runs, the sequence still reads its old
	// phase and tokens, so a caller that queued "reset, then generate" reads the new generation
	// once this handle reads Success. The request resolves ResetRequired at its turn, leaving the
	// sequence unchanged, only when a reset queued ahead of it was refused by Layer 1, which left
	// the sequence Faulted; GetLastLifecycleRequestError() then says the reset did not leave it
	// Idle. Requiring a reset after every fault is the plugin's own rule: Layer 1 requires one only
	// after an allocation failure (gpu_1p0.h:233-237). A generation that ended in SchemaDeadEnd is
	// Faulted too -- also the plugin's rule, since Layer 1 lets a dead end be retried
	// (gpu_1p0.h:564-579). A restore never clears a faulted sequence: it gives a new one.
	FSuperSLMLifecycleOpHandle RequestBeginGeneration(const FSuperSLMGpuSequence& Sequence, const FSuperSLMGenerationRequest& Request);

	// D-SLM7457 ruling (1): Layer 1's GPU ABI has no prefix-adopt verb of any kind (v1.5.0, and
	// unchanged at the 1.7.0 and 1.9.0 pins)
	// (grepped ThirdParty/SuperSLM/include/superslm/gpu_1p0.h and gpu_port.h for "prefix";
	// sslm_seq_adopt_prefix, sslm_abi_functions_g5_comparable.inc:79, takes an sslm_seq, the
	// CPU-only handle type). This request queues and admits in the sequence's own strict
	// arrival order like every other op, but its own admission never calls Layer 1: it
	// resolves directly to ESuperSLMRestoreResult::UnsupportedOnGpu, a named,
	// backend-unsupported result -- "not a queue case" in the sense that nothing here is ever
	// forwarded to the submission thread, never in the sense that it skips the queue's own
	// ordering and capacity accounting.
	FSuperSLMLifecycleOpHandle RequestAdoptPrefix(const FSuperSLMGpuSequence& Sequence);

	// D-SLM7457 ruling (2): Restore creates a NEW sequence handle from a blob -- it has no
	// existing vended handle to collide against, so it is not part of any sequence's
	// per-sequence FIFO (the plan's own collision list drops it for exactly this reason). It
	// still must never block the calling thread (D-SLM7402), so it is queued at the SUBSYSTEM
	// level instead: admitted in global-request-ordinal order against every other queued
	// subsystem-level restore, interleaved with per-sequence admissions on the same shared
	// submission thread. ExpectedModel's own artifact hash is checked exactly as
	// USuperSLMSubsystem::RestoreSequence()'s CPU-side counterpart already does
	// (SuperSLMSubsystem.h) -- synchronously, at the call that queues it, so a malformed blob
	// or a model mismatch is knowable without waiting a tick (GetRestoreResult() on the
	// returned handle still reads Pending until the next tick's admission pass reports it,
	// keeping one uniform "poll until not Pending" caller shape across every op kind).
	// Internally tick-queued the same way the prior synchronous form was (plan §5: "Restore is
	// refused while any sequence in the process is Submitted, so the submission thread issues
	// restores at the start of a tick") -- the caller never observes SSLM_BUSY. 'SLM5' restores
	// the schema binding and its walk (plan §2.5 row 5); the restored sequence is checked to
	// agree with the wrapper's schema name and refused Malformed by name otherwise, and it is not
	// bind-eligible until reset. A blob saved under another Layer-1 pin is refused at request
	// time with nothing queued, and GetLastLifecycleRequestError() names Layer1Mismatch and both
	// tags (row 23). A restore pins its adapter from the request until it resolves, so
	// UnmapAdapter() is refused meanwhile. The blob is copied once, when the request is accepted,
	// and never after (row 19). Reuses ESuperSLMRestoreResult for every case Layer 1 or the blob
	// wrapper can produce (BackendMismatch, ModelMismatch, KvMismatch, Malformed, NotConfigured,
	// PoolExhausted, AdapterUnavailable), and OutOfMemory when Layer 1 ran out of GPU memory
	// (SSLM_GPU_ALLOCATION_FAILED): the save is intact and the restore may be retried later. A lost
	// device still reads Malformed. ResidualLost is CPU-only and never returned here.
	FSuperSLMLifecycleOpHandle RequestRestoreSequence(const TArray<uint8>& Blob, USuperSLMModel* ExpectedModel);

	// Pending until the operation's own committed tick has come; then the named result
	// (ESuperSLMRestoreResult, SuperSLMSaveRestoreTypes.h) -- Success, or whichever named
	// rejection applies to that op kind (SequenceQueueFull is read from the HANDLE's own
	// validity instead, never from this accessor -- see the queue-refusal paragraph above).
	// Shared by RequestResetSequence()/RequestAdoptPrefix()/RequestBeginGeneration().
	ESuperSLMRestoreResult GetLifecycleOpResult(const FSuperSLMLifecycleOpHandle& Handle) const;

	// The resolution-order ordinal (maintainer ruling superseding D-SLM7480, T-2826 round 8):
	// -1 while the handle is invalid or its own op is still Pending; otherwise the value assigned
	// the instant that op's result was first published, strictly increasing in the order ops
	// actually resolve -- ACROSS every sequence this subsystem vends and the subsystem-level
	// restore queue, so it also orders two ops on DIFFERENT sequences relative to each other, not
	// only two ops on the same one. Two ops that resolve on the same tick still carry two distinct
	// ordinals in their true resolution order (D-SLM7418/D-SLM7421's own "arrival order" promise
	// is about ORDER, not about which tick each op lands on), which is what this accessor exists
	// to make directly observable instead of inferring order from tick indices, as
	// SuperSLM.L2S2.Lifetime.SameSequenceCollisionQueues now does. Read-only telemetry: it never
	// feeds a decode or the admission schedule -- nothing in this subsystem ever branches on it.
	int64 GetLifecycleOpResolutionOrdinal(const FSuperSLMLifecycleOpHandle& Handle) const;

	// Save's own blob is delivered through the same handle (plan §5): Pending until the save
	// resolves, then the result. Plan §2.5 row 19 (D-SLM7763): the blob is assembled off the game
	// thread, and the first read that returns Success MOVES it out to OutBlob -- no copy, and the
	// handle's entry keeps only its result. Every later read of the same handle returns Consumed
	// with OutBlob empty. OutBlob is empty on every other result.
	ESuperSLMRestoreResult GetSaveResult(const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob);

	// Plan §2.5 row 19 (D-SLM7763): releases a resolved lifecycle-op handle's entry (its result,
	// and a save blob nobody took). Returns true when an entry was erased; false for an unknown or
	// already-released handle, and for one still Pending, which is left alone. Afterwards the
	// handle reads Pending (it names nothing). TearDown() releases every entry.
	bool ReleaseLifecycleOpHandle(const FSuperSLMLifecycleOpHandle& Handle);

	// The pooled-resource monitor's retention readings (plan §7 item 8, D-SLM7763): the bytes of
	// save blobs held in handle entries and not yet taken; the number of resolved handle entries
	// not yet released; and the number of restore entries still queued or in flight (a resolved
	// restore's entry is removed, so this reads 0 once every restore has resolved).
	int64 GetRetainedResultBytes() const;
	int32 GetLifecycleHandleEntryCount() const;
	int32 GetRestoreOpEntryCount() const;

	// Diagnostic only (plan §9 R-S2d, U1): for a save that resolved Success, the generation's token
	// counts at the save's position -- OutObservedAtSave, the tokens already applied to the game
	// (GetGeneratedTokens().Num() at the save's finalize), and OutProducedAtSave, the tokens the
	// submission thread had produced when the save job ran. The difference travels in the blob as
	// carried results. False (both -1) for any other handle or result.
	bool GetSaveTokenCounts(const FSuperSLMLifecycleOpHandle& Handle, int32& OutObservedAtSave, int32& OutProducedAtSave) const;

	// Restore's own new sequence handle is delivered through the same handle mechanism: Pending
	// until the restore's own committed tick has come, then the result, with OutSequence
	// populated only on Success.
	ESuperSLMRestoreResult GetRestoreResult(const FSuperSLMLifecycleOpHandle& Handle, FSuperSLMGpuSequence& OutSequence) const;

	// The number of operations currently queued OR in flight for Sequence (0 to
	// Config.MaxQueuedOperationsPerSequence, plus one more if a job is genuinely Submitted) --
	// leaving out a held schema bind that is the submitted front, as the queue bound does (ruling
	// 2026-09-26, round 2) --
	// mirrors USuperSLMSubsystem::GetPendingLifecycleOperationCount()'s own CPU-side shape
	// (SuperSLMSubsystem.h), applied here to the GPU backend's own queue.
	int32 GetPendingLifecycleOperationCount(const FSuperSLMGpuSequence& Sequence) const;

	// The human-readable reason the MOST RECENT Request*() call on this subsystem was refused
	// or (for a Save admission that resolved SaveRefused) the reason that admission refused --
	// empty when the most recent relevant call had no refusal to report. The queued Request*
	// calls return only a handle (or, for GetSaveResult, a handle's own named enum), with no
	// OutError parameter the way the prior synchronous calls had, because a request-time
	// refusal and an admission-time refusal happen at two different, caller-invisible moments;
	// this accessor is the one place the descriptive text for either surfaces. Read it
	// immediately after observing the refusal (an invalid returned handle, or a resolved
	// non-Success/non-Pending result) -- like GetLastFaultReason() above, it names the most
	// recent event and is not itself queued or historied.
	FString GetLastLifecycleRequestError() const;

	// --- Diagnostics (§5.1, §7 item 7; R-S2a, R-S2c, R-S2d, R-S2f) ---

	// Transports superslm_gpu::LastCallTiming().gpu_busy_ms unchanged (gpu_port.h, read
	// on the submission thread right after the most recent drain, plan §7 item 7) --
	// the plugin does not recompute or average this. R-S2f reads it per slice, with and
	// without the example scene rendering.
	double GetLastGpuBusyMs() const;

	// The wall time of SslmGpuSeqFinishTokenForG5Bridge on the submission thread, once per
	// token: final norm, the logits row, narrowing, mask and argmax (plan §5, §5.1; R-S2f
	// measures it here). With the device-resident head (the default) the logits run as their own
	// synchronous device submission inside this call, which gpu_busy_ms does not see, so this
	// figure bounds that dispatch from above; with the head on the host, the logits run through
	// the ParallelFor finish hook.
	double GetLastHostFinishMs() const;

	// Incremented once per late event, at its own apply tick (a token planned after its T+K tick,
	// or else a job not finished although the device was given at least the sim time since its
	// request; Tick() above), OR per composed-path slice whose own measured
	// gpu_busy_ms exceeds Config.TickBudgetMs (plan §5: "an overrun is counted as a
	// hitch") -- the GPU scheduler's own hitch counter, distinct from
	// USuperSLMSubsystem::GetHitchCount()'s CPU-path counter (SuperSLMSubsystem.h).
	int32 GetHitchCount() const;

	// Cumulative count of real GPU submissions issued since the last Configure() --
	// R-S2c's own cross-check that a refused prompt costs "zero GPU calls" (this counter
	// must be unchanged across that refusal).
	int64 GetGpuDispatchCount() const;

	int32 GetPoolFreeCount() const;
	int32 GetPoolOccupiedCount() const;

	// Plan §2.5 row 16 (D-SLM7682): pooled sequences withheld from vending because Layer 1 refused
	// the reset or unbind that recycles them; each is logged by name and stays withheld until the
	// subsystem is torn down. The pooled-resource monitor's count (plan §7 item 8).
	int32 GetWithheldSlotCount() const;

	// Adapters mapped through MapAdapter() and not yet unmapped. A re-Configure() tears them down
	// with the rest of the state, so the Load node refuses while any is held (review R5-N3).
	int32 GetMappedAdapterCount() const;

	// The plugin's own DECLARED GPU residency accounting: the device-local (DEFAULT-heap)
	// buffers Layer 1 allocates for the model (the packed layer weights, the two RoPE tables and
	// the SCM1 mask pages), the device-resident head while it is active, and every pooled
	// sequence's K/V buffer, plus the mapped adapters -- a lower bound on what the driver
	// occupies, never a ceiling (plan §2.5 row 3, D-SLM7679/D-SLM7683/D-SLM7684), and never a
	// live device memory query. D-SLM7335
	// (answering Q_R-S2d, T-2816 §8) settles that this figure is a MECHANISM CHECK
	// only -- an internal-consistency proxy that would catch the plugin's own
	// accounting drifting from its own arithmetic, never the evidence for R-S2d's
	// user-facing "VRAM is flat" claim. GetLocalVideoMemoryUsageBytes() below is that
	// evidence.
	int64 GetDeclaredGpuResidencyBytes() const;

	// The LIVE evidence for R-S2d's "VRAM is flat across long play" claim (D-SLM7335).
	// Reads `IDXGIAdapter3::QueryVideoMemoryInfo(0 /* node */, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
	// &Info)` and returns `Info.CurrentUsage` in bytes, on the SAME DXGI adapter this
	// subsystem's own `sslm_gpu_context_create` selected (the plugin already holds that
	// adapter's `IDXGIAdapter3` from its own device-creation path -- this accessor reads
	// through it, it does not re-enumerate or guess which adapter Layer 1 chose,
	// unlike a caller outside the plugin, which has no way to know). This is a real
	// device-memory reading, independent of the plugin's own arithmetic in
	// GetDeclaredGpuResidencyBytes() above -- a leak in Layer 1's own allocations or in
	// a plugin GPU call this subsystem does not account for is visible here and
	// invisible there (a reference must not share its inputs with the thing it grades). R-S2d samples this after 10 warm-up cycles and after
	// cycle 1,000 and requires the growth to be under one A-EX KV block (24 MiB).
	int64 GetLocalVideoMemoryUsageBytes() const;

	// Wall-clock milliseconds from BeginGeneration() to the tick that applied Sequence's first
	// token; negative until that token is applied. Prompts are fed a token at a time in layer
	// slices (D-SLM7379), so a long prompt costs ticks, and this is where that cost is reported
	// (plan §9 R-S2a, R-S2c).
	double GetTimeToFirstTokenMs(const FSuperSLMGpuSequence& Sequence) const;

	// The model the most recent successful Configure() mapped, or null.
	USuperSLMModel* GetConfiguredModel() const;

	//~ Begin USubsystem
	// False off Win64 (SUPERSLMUNREAL_WITH_GPU == 0): the class exists on every platform
	// because UnrealHeaderTool cannot see a platform #if (plan §10.3 item 4), but no instance
	// is created where the GPU backend is compiled out.
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem

	//~ Begin UObject
	// Tears down any runtime state Deinitialize() did not.
	virtual void BeginDestroy() override;
	//~ End UObject

private:
	friend struct FSuperSLMSelfCheckAccess;
	friend struct FSuperSLMGpuTestAccess; // test-only access (SuperSLMGpuTestAccess.h, U1 round 3)

	// Every Layer-1 GPU handle, the submission thread, the slot table and the scheduler
	// figures (SuperSLMGpuSubsystem.cpp). Held by pointer so this reflected header includes no
	// Layer-1 or D3D12 header.
	// The GFrameCounter of the last Tick() (review W6); TickOncePerFrame() skips while it is current.
	uint64 LastTickFrame = MAX_uint64;

	FSuperSLMGpuSubsystemState* State = nullptr;

	// SetFixedTickLatency()'s and SetLayersPerTick()'s shared gate: no sequence vended (a deferred
	// return included) and no restore queued. What names the constant in OutError. Game thread.
	bool IsPoolDrainedForSchedulerChange(const TCHAR* What, FString& OutError) const;
	// Applies a pending RequestConfiguredScheduleRestore() once the pool is drained. Game thread.
	void TryRestoreConfiguredSchedule();

	/** The configured model, referenced so it is not collected while its GPU mapping serves this subsystem's sequences. */
	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> ConfiguredModel;

	// Diagnostics kept on the subsystem itself, so they outlive a Configure() that failed (the state
	// is discarded then): GetShaderDirectory() and the R-S2j map samples.
	FString LastConfigureShaderDirectory;
	int64 LastPreMapLocalVideoMemoryBytes = -1;
	int64 LastPostMapLocalVideoMemoryBytes = -1;

	// BeginConfigure()'s supersession check: TearDown() increments it, so a pending prepare whose
	// serial is no longer current is discarded. Game thread.
	uint32 ConfigureSerial = 0;
	bool bConfigurePending = false;

	// Review W5: every BeginConfigure() whose pool-thread prepare has not yet been claimed by its
	// game-thread continuation. Deinitialize() waits for each and discards what it built, so a
	// load in flight at exit neither leaks its state nor leaves its submission thread running
	// into module unload. Game thread.
	TArray<TSharedPtr<FSuperSLMGpuConfigureFlight, ESPMode::ThreadSafe>> InFlightConfigures;

	void TearDown();
};

