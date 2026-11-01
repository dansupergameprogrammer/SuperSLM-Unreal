#pragma once

// Test-only access to the GPU subsystem's multi-row scheduling (multi-row GPU plan rev 1.3.2, slice MR6, §5.2-§5.4;
// cells P4.4, 7.12, 8.5, 9.4, ruling 40's R40.1-R40.6, ruling 42's M1, M2, R1, S1, S2, S5, ruling 43's N1, N2 and ruling 45's per-tick GPU time;
// red-suite-mr6.md).
// Compiled only under WITH_DEV_AUTOMATION_TESTS. Game thread only. It records and reads; apart from SetMultiRow (a
// configuration write) and ArmRowsFault (a one-shot injected engine status) it never changes a token.
//
// THIS FILE IS THE MR6 RED SUITE'S CONTRACT. Its bodies live in Private/SuperSLMGpuSubsystem.cpp, beside the state they
// read (as the paged and schema-jump seams' do); a build without the GPU backend has bodies where IsBuilt() is false,
// so every MR6 cell fails by name there. The implementation may add to this interface, but must not take anything away or
// change a meaning stated here.

#include "CoreMinimal.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "superslm/gpu_1p0.h" // SslmGpuStatus

#if WITH_DEV_AUTOMATION_TESTS

// What a recorded GPU action did.
enum class ESuperSLMGpuRowsActionKind : uint8
{
	OneRow = 0,      // 1.17's one-row work: a whole token (OneCall) or a slice of one row (Composed)
	PromptRows = 1,  // a slice of an embed_rows(PROMPT) chunk
	ExactRows = 2,   // a slice of an embed_rows(EXACT_DRAFT) chunk
};

// One record per GPU action a recorded sequence ran (§5.2: at most one action per sequence per tick).
struct SUPERSLMUNREAL_API FSuperSLMGpuRowsActionRecord
{
	int64 SequenceId = 0;
	int32 ActionIndex = -1;  // counts every recorded action since StartRecording(), across sequences
	int64 Tick = -1;         // the subsystem tick the action was issued on
	ESuperSLMGpuRowsActionKind Kind = ESuperSLMGpuRowsActionKind::OneRow;
	int32 Rows = 0;          // 1 for OneRow; M for a chunk's slice
	int32 FirstLayer = 0;    // the slice's first layer
	int32 Layers = 0;        // layers the action ran
	bool bOpenedChunk = false;     // this action called embed_rows
	bool bClosedChunk = false;     // this action's slice closed the chunk (its last row at full depth)
	bool bAbandonedChunk = false;  // the chunk was abandoned after this action (a save, §5.4)
	// Prompt tokens this action committed to the sequence: a one-row prompt action's token (on the action that finishes
	// it, the last slice on Composed); a chunk's M rows on the slice that closes it. 0 on a chunk's other slices, on an
	// abandoned chunk's slices, and for a produced token.
	int32 PromptTokensConsumed = 0;
	int32 AdapterId = 0;             // the adapter passed on this action's slice (0: base)
	bool bBatched = false;           // the action ran inside the batch call (Composed, MR-D14)
	// Ruling 40 (the multi-row Exact path's delivery, re-pointed from P2's host cell P2H.4): the ids this action's
	// result delivered to the sequence, in order, whenever the result is applied (one result per action). An Exact
	// close's are its accepted drafts and then its produced token (none after a dead end); a one-row action's are its
	// produced token, if any; every other slice's are empty. Filled when the result is applied, so a record read while
	// that result is pending shows none yet.
	TArray<int32> DeliveredTokens;
	// This action's finish, or its chunk's close, met a schema dead end (-2: the masked argmax had no transition).
	bool bDeadEnd = false;
	// Ruling 42 S1/S2 with ruling 42a: an Exact chunk falls back when it has no draft (an empty forced chain and no
	// table draft) or its open failed (embed_rows(EXACT_DRAFT) or forced_tokens). A chunk that falls back does no device
	// work until its close slice (its other slices record Layers = 0), and its close slice runs Off's whole-token step
	// (Rows = 1, Layers = the model's layers) and delivers there. Its slices keep Kind ExactRows; bFellBack is set on
	// the close slice only.
	bool bFellBack = false;
	// Ruling 42 S5, on an ExactRows action that opened its chunk (bOpenedChunk): the ids the forced chain gave
	// (forced_tokens, after the stop-token cut) and the drafts the chunk embedded in all (the chain's plus the learned
	// table's). -1 on every other action, and on an open that failed before forced_tokens answered.
	int32 ForcedChainCount = -1;
	int32 DraftCount = -1;
	// Ruling 59e point 1 (an Exact chunk's opening record): what the GPU's open proposed (the forced chain and any draft, the
	// ids after the pending token), what CPU Exact drafts from the same walk state (BuildDraft from the state after the
	// pending token, when that state is forced), and whether the learned table reached the GPU.
	TArray<int32> ProposedTokens;
	TArray<int32> CpuDraftTokens;
	bool bDraftTableReached = false;
};

// Ruling 42 S1, M2: the engine calls ArmRowsFault() can fail.
enum class ESuperSLMGpuRowsFaultPoint : uint8
{
	EmbedRowsPrompt = 0,      // sslm_gpu_seq_embed_rows(SSLM_GPU_ROWS_PROMPT)
	EmbedRowsExactDraft = 1,  // sslm_gpu_seq_embed_rows(SSLM_GPU_ROWS_EXACT_DRAFT)
	ForcedTokens = 2,         // sslm_gpu_seq_forced_tokens on an Exact open
};

// Ruling 45 (the suite's contract): the kind of one Layer-1 call a tick job made.
enum class ESuperSLMGpuTickCallKind : uint8
{
	OneCallStep = 0,   // a OneCall decode step (whole token, one call)
	ComposedSolo = 1,  // a Composed slice run solo (sslm_decode_step_gpu with a layer budget below full)
	Batch = 2,         // sslm_decode_step_batch_gpu (Members >= 1)
	RowsSlice = 3,     // a rows slice (multi-row) that is not an Exact close
	ExactClose = 4,    // the slice that closes an Exact chunk (its widened logits stage included)
	Other = 5,         // any other Layer-1 call the subsystem counts as a submission
	// Ruling 46a: a standalone finish-bridge call (FinishToken after Composed, the primed OneCall finish, ArmedFinish).
	// It publishes its own LastCallTiming() (the one-row device-logits finish, ruling 46) and is no submission:
	// Members = 0.
	Finish = 6,
	// Ruling 48 (E-13): a Layer-1 call of the load-time row-cost calibration (a warm-up or timed token's slice, or its
	// whole token at B = 0), Members 1. The calibration token's standalone finish, when it is its own call, is a
	// Finish record in the same tick job.
	Calibration = 7,
};

// Ruling 45 (the suite's contract): one Layer-1 call inside a tick job, in the order the job made them. The per-tick
// value must be the sum of these, never the last call's: a cell checks GpuBusyUs against the sum of CounterDeltaUs, and
// each CounterDeltaUs against what the call itself published.
struct SUPERSLMUNREAL_API FSuperSLMGpuTickCallRecord
{
	ESuperSLMGpuTickCallKind Kind = ESuperSLMGpuTickCallKind::Other;
	// Members of a Batch call; 0 for a Finish (no submission, ruling 46a); 1 for every other kind.
	int32 Members = 1;
	// The delta of superslm_gpu::CumulativeGpuBusyUs() across the call (and the result reads that publish its timing,
	// when the timing publishes there), in microseconds.
	double CounterDeltaUs = 0.0;
	// superslm_gpu::LastCallTiming().gpu_busy_ms * 1000 read once the call's timing has published: the last published
	// time, which for a Batch call is its last member's only. 0 when the call published no timing.
	double LastPublishedUs = 0.0;
	// Ruling 50: on a Calibration call, the layers its slice ran (the whole depth for a fused whole token) and the
	// calibration attempt it belongs to (0, or 1 for the retry). Ruling 51 (F7): the calibration token's own Finish
	// call carries its attempt too, with Layers 0, so a finish-only calibration tick is told from a generation's. -1 on
	// every other call. Ruling 54: 0 and 1 are phase 1's attempts (the early position); 2 marks every call of phase 2
	// (the fill to P2 and its warm-up and timed tokens), which has one attempt and no retry. Phase 2's calls ride after a
	// tick's generation actions, at most one per tick, and its first slice is not held to one layer.
	int32 Layers = -1;
	int32 CalibrationAttempt = -1;
	// Ruling 56 (2): on a Calibration call, the rows its slice ran: 1 for the calibration tokens (both phases); M for
	// phase 1's load-time warm-up of a widened row kernel the chooser can pick (ruling 57: GetWidenWarmUpRows; once each per
	// phase-1 attempt (ruling 56a: a retry adds a set, a cache hit runs none), one layer on the throwaway sequence, untimed, never over B; it carries phase 1's attempt). -1 on
	// every other call.
	int32 Rows = -1;
};

// Ruling 45/45a (MB6 10.3/10.4 on the GPU clock): one record per tick job the submission thread ran while recording.
struct SUPERSLMUNREAL_API FSuperSLMGpuTickGpuBusyRecord
{
	int64 Tick = -1;           // the subsystem tick that enqueued the job (the action records' Tick)
	// The GPU-measured busy time of the job's submissions, in microseconds: the delta of the engine's cumulative
	// counter (superslm_gpu::CumulativeGpuBusyUs(), ruling 45a) across the job. It counts every OneCall step, Composed
	// solo slice, every member of the batch call, every rows slice, the widened logits stage of an Exact close, and
	// (ruling 46) the one-row device-logits finish and the device K/V restore an Exact close submits for refused rows:
	// a standalone finish-bridge call is its own Finish record (ruling 46a).
	double GpuBusyUs = 0.0;
	// Layer-1 submissions the job made, as the subsystem counts them (a batch call counts one per member).
	int32 Submissions = 0;
	// Of those, the submissions after which the cumulative counter had not advanced: work that published no GPU
	// timing. Checked at each point the subsystem reads the timing and at the job's end, so a batch call that advanced
	// the counter counts as timed. 0 is expected (ruling 45a).
	int32 UntimedSubmissions = 0;
	// Wall-clock time of the job's Layer-1 work (the Layer-1 lock held), in microseconds: an upper bound on GPU time
	// that also counts host work and drains. Information only; nothing is graded on it (ruling 45).
	double WallUs = 0.0;
	// Ruling 45 (the suite's contract): the job's Layer-1 calls, in order. Sum of CounterDeltaUs == GpuBusyUs, and the
	// sum over calls of (Batch ? Members : Finish ? 0 : 1) == Submissions (ruling 46a).
	TArray<FSuperSLMGpuTickCallRecord> Calls;
};

// Ruling 48 (E-13): where the row cost the chooser uses (GpuRowCostMs) came from. Private/Core/
// SuperSLMGpuRowCostCalibration.h's ESource, plus Pending while a calibration has not settled.
enum class ESuperSLMGpuRowCostSource : uint8
{
	Default = 0,    // bCalibrateGpuRowCost false: 6.2 ms
	Game = 1,       // the game set GpuRowCostMs (> 0; 0 is unset, ruling 48a): calibration skipped
	Calibrated = 2, // the measured price
	FallBack = 3,   // calibration failed, or stayed out of range after its retry: 6.2 ms, with a warning
	Pending = 4,    // calibrating; the first generation waits
};

// Ruling 48 (E-13): the row cost the subsystem settled on at its last Configure(), and the calibration behind it.
struct SUPERSLMUNREAL_API FSuperSLMGpuRowCostRecord
{
	ESuperSLMGpuRowCostSource Source = ESuperSLMGpuRowCostSource::Pending;
	// The row cost in use, in milliseconds (the value the chooser was given as GpuRowCostMs).
	double RowCostMs = 0.0;
	// The chooser plan's C(1), in microseconds (FChooserPlan::RowCostUs). -1 while Pending.
	int64 PlanRowCostUs = -1;
	// Attempts the calibration made for this model and adapter (1, or 2 after a retry; 0 when it did not run or the
	// value came from the once-per-model-and-adapter cache).
	int32 Attempts = 0;
	// Of the last attempt: its warm-up tokens, and each timed token's counter delta in microseconds, in order. Ruling 56
	// (2): phase 1 runs 3 warm-up tokens (it is still priced at the max of its 3 timed runs).
	int32 WarmUpRuns = 0;
	TArray<double> TimedRunsUs;
	// The GPU adapter the context runs on, as the log line names it.
	FString AdapterName;
	// Erratum E-14 (ruling 53): the price is a line in position, C(1, pos) = C_a + max(0, slope) x (pos - pos_a).
	// RowCostMs above is C_a. WarmUpRuns and TimedRunsUs above are the early position's (1 and 3 runs); the late
	// position's follow. The game value, the default and the fallback are flat: slope 0, positions -1. A cache hit
	// gives the same line (both numbers are cached). -1 / 0 while Pending.
	// Ruling 54: Source is Calibrated once phase 1 settles. WarmUpRuns and TimedRunsUs are phase 1's, and
	// AnchorPosition is phase 1's last timed position. Until phase 2 settles (and for good at B = 0, or after phase 2
	// failed) SlopeUsPerPosition is s_default = 0.0015 x C_a and the late fields are -1 / -1 / 0 / empty; once it
	// settles they are phase 2's and the slope is the measured one. A cache hit with a measured slope runs nothing; a
	// cache hit with the default slope at B > 0 runs phase 2 again.
	int64 AnchorPosition = -1;        // pos_a, where C_a was timed
	int64 LatePosition = -1;          // P2 = min(256, ContextCap - 8)
	int64 LateRowCostUs = -1;         // C_b, PriceUs() of the late position's timed runs
	double SlopeUsPerPosition = 0.0;  // max(0, C_b - C_a) / (P2 - pos_a), in us per position
	int32 LateWarmUpRuns = 0;
	TArray<double> LateTimedRunsUs;
	// Ruling 54: the slope in use is phase 2's measured one (false: s_default, or a flat line).
	bool bSlopeMeasured = false;
	// Ruling 54: phase 2 is pending or running (B > 0, phase 1 settled Calibrated, phase 2 not yet settled, failed or
	// dropped). Never true at B = 0.
	bool bLatePending = false;
	// Ruling 56 (1): F, the logits finish's price in microseconds: phase 1's measured finish (the max of its timed
	// finishes, bFinishMeasured), cached with the line; with none measured (the game's value, the default, the
	// fallback) ceil(g(1) x C(1) / 1000) from the cost table, at C_a. -1 while Pending.
	int64 FinishUs = -1;
	// Erratum E-16 (ruling 60): the timed runs the outlier rule dropped (over 3 x their set's median), each phase's in
	// order, and the spread failures it counted (a set still holding such a run after its 2 extra runs). TimedRunsUs and
	// LateTimedRunsUs above are the runs kept.
	TArray<double> DroppedRunsUs;
	TArray<double> LateDroppedRunsUs;
	int32 SpreadFailures = 0;
	int32 LateSpreadFailures = 0;
	bool bFinishMeasured = false;
};

struct SUPERSLMUNREAL_API FSuperSLMGpuMultiRowTestAccess
{
	// True: the seam's bodies are built (the red stubs return false).
	static bool IsBuilt();
	// Sets FSuperSLMGpuRuntimeConfig::bGpuMultiRow (ruling 39 Q9) on a configuration before Configure(). False when the
	// field is not there (the stub).
	static bool SetMultiRow(FSuperSLMGpuRuntimeConfig& Config, bool bOn);
	// Starts (and clears) the action record.
	static void StartRecording(USuperSLMGpuSubsystem& Gpu);
	static TArray<FSuperSLMGpuRowsActionRecord> GetActions(const USuperSLMGpuSubsystem& Gpu);
	// A chunk is open on the sequence: planned and not closed or abandoned (embedded, or under ruling 42a a chunk that
	// falls back and has not reached its close slice).
	static bool IsChunkOpen(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	// The plan the subsystem's chooser made at Configure (SuperSLMCore::GpuRows): M_max, l(M_max), the Composed rows and
	// layers, and whether multi-row is on. False before a Configure (or the stub).
	static bool GetRowsPlan(const USuperSLMGpuSubsystem& Gpu, int32& OutMMax, int32& OutMMaxLayers, int32& OutComposedRows, int32& OutComposedLayers, bool& OutMultiRowOn);
	// Ruling 40 (the save carry, re-pointed from P2H.4's gpu_restore_issued_count): the ids in the sequence's results
	// that are issued and not yet applied, which a save now would carry (one carried event per id). -1 for a sequence
	// that is not live (and the stub).
	static int32 PendingResultIds(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	// Ruling 42 S1, M2: one shot. The next call at Point that the subsystem makes (any sequence) returns Status in
	// place of the engine call, and the engine is left as that call leaves it on that refusal (nothing embedded,
	// nothing read). False when the seam is not built.
	static bool ArmRowsFault(USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuRowsFaultPoint Point, SslmGpuStatus Status);
	// Ruling 42 S1/S2 (ruling 42a): the Exact chunks that fell back since StartRecording(), by cause. The same counts
	// feed the trace counter SuperSLM/GPU/ExactStepFellBack. False when the seam is not built.
	static bool GetExactFallbacks(const USuperSLMGpuSubsystem& Gpu, int32& OutNoDraft, int32& OutOpenFailed);
	// Ruling 42 M2: the adapter is mapped (MapAdapter() and no UnmapAdapter() that went through). False when the seam is
	// not built.
	static bool IsAdapterMapped(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuAdapterHandle& Adapter);
	// Ruling 43 N2: how many times any Layer-1 call the subsystem made (decode steps and the batch call included) returned
	// Status since StartRecording(). -1 when the seam is not built.
	static int32 GetLayer1StatusCount(const USuperSLMGpuSubsystem& Gpu, SslmGpuStatus Status);
	// Ruling 45/45a: the per-tick GPU-busy records of the tick jobs completed since StartRecording(), in the order they
	// ran (a tick with no tick job has no record). Read on the game thread after the jobs are done (e.g. after a
	// drain). False when the seam is not built (OutRecords empty).
	static bool GetTickGpuBusyUs(const USuperSLMGpuSubsystem& Gpu, TArray<FSuperSLMGpuTickGpuBusyRecord>& OutRecords);
	// Ruling 48: Configure() does not clear the action or tick records; only StartRecording() does, so a cell can record
	// the calibration a Configure() starts.

	// --- Ruling 48 (E-13): the load-time row-cost calibration. ---
	// Sets FSuperSLMGpuRuntimeConfig::bCalibrateGpuRowCost (default true). False when the field is not there (the stub).
	static bool SetCalibrateGpuRowCost(FSuperSLMGpuRuntimeConfig& Config, bool bOn);
	// The row cost of the last Configure(). False when the seam is not built.
	static bool GetRowCost(const USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuRowCostRecord& Out);
	// A calibration is running (Source Pending). Ruling 54: phase 1 only; false while phase 2 runs.
	static bool IsRowCostCalibrating(const USuperSLMGpuSubsystem& Gpu);
	// The public getter's value (USuperSLMGpuSubsystem::GetGpuRowCostMs(): the row cost in use, in ms). -1 when the
	// seam is not built.
	static double GetPublicGpuRowCostMs(const USuperSLMGpuSubsystem& Gpu);
	// One shot per entry, for the next calibration's attempts in order: an entry above 0 replaces that attempt's
	// measured maximum, in microseconds, before the price is taken (the timed runs still run); 0 leaves the attempt as
	// measured; below 0 makes the attempt fail as an engine failure would. False when the seam is not built.
	static bool ArmRowCostCalibration(USuperSLMGpuSubsystem& Gpu, const TArray<double>& AttemptMaxUs);
	// Ruling 48a (4): the next calibration's first attempt times out, as if the 2 s wall-clock limit had passed (no
	// retry follows a timeout). False when the seam is not built.
	static bool ArmRowCostCalibrationTimeout(USuperSLMGpuSubsystem& Gpu);
	// Ruling 48a (3): the cache key is the artifact's content hash and the GPU adapter's LUID (never a LoRA adapter),
	// so the same artifact imported again hits it.
	// Forgets every calibration done in this process (the once-per-model-and-adapter cache) and zeroes the count.
	// Ruling 54: it forgets the once-per-model-and-adapter phase-2 warning too, and drops a pending phase 2.
	static void ForgetRowCostCalibrations();
	// Ruling 60 (5): adds DelayUs to the measured time of chosen timed runs of a phase (Phase 1 or 2): bit i of RunMask is
	// the phase's i-th timed run in each state configured from now on (replacement runs and retries counted). Stays armed
	// until called again with RunMask 0. False when the seam is not built.
	static bool ArmRowCostRunDelay(int32 Phase, uint32 RunMask, double DelayUs);
	// Calibrations run (to a used price or a fallback) since the last ForgetRowCostCalibrations(). -1 when the seam is
	// not built. Ruling 54: counts phase 1 settling; phase 2 adds nothing.
	static int32 GetRowCostCalibrationCount();

	// --- Erratum E-14 (ruling 53): the row cost by position. ---
	// ArmRowCostCalibration() above (E-13) now arms both positions of an attempt with the same maximum (a flat line).
	// This arms them apart: entry i of each array is attempt i's maximum at that position, with
	// ArmRowCostCalibration()'s meaning per entry (> 0 replaces, 0 measures, < 0 fails). False when the seam is not built.
	// Ruling 54: EarlyMaxUs arms phase 1's attempts in order; LateMaxUs[0] arms phase 2 (its one attempt; later
	// entries are unused). ArmRowCostCalibration() arms phase 1's attempts and phase 2 with the maximum of the
	// attempt phase 1 used, so a phase 2 it arms measures slope 0.
	static bool ArmRowCostCalibrationPositions(USuperSLMGpuSubsystem& Gpu, const TArray<double>& EarlyMaxUs, const TArray<double>& LateMaxUs);
	// C(1, Position) as the subsystem prices a slice that ends at Position, in us. -1 when the seam is not built or
	// before a settled Configure().
	static int64 GetRowCostAtUs(const USuperSLMGpuSubsystem& Gpu, int64 Position);
	// l(1) for a one-row slice that ends at Position (the chooser's plan at C(1, Position)). -1 when the seam is not
	// built or before a settled Configure().
	static int32 GetOneRowLayersAt(const USuperSLMGpuSubsystem& Gpu, int64 Position);
	// Run 5 (ruling 59c point 3): the widened kernel widths (bit M, M in 2, 4, 8) Configure()'s own warm-up ran -- the warm-up
	// that builds every width at load, so a width phase 1 skips under the half-room rule (59.2) is still warm. False when the
	// seam is not built or before a Configure().
	static bool GetConfigureWidenedMask(const USuperSLMGpuSubsystem& Gpu, uint32& OutMask);
	// Ruling 59d point 4: the first field two GPU save blobs differ in, by name (the plugin wrapper's, the GPU extension's,
	// a Layer-1 header field, or 'Layer-1 body at +N'), with both values as text; empty when equal. OutInFlightA/B are the
	// extensions' InFlightIssued (point 1). Live state, as GetLiveSaveBytes. False when either blob does not read.
	static bool DescribeSaveDiff(const USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& A, const TArray<uint8>& B, FString& OutField, FString& OutValues,
		int32& OutInFlightA, int32& OutInFlightB);
	// Ruling 59d point 2: a GPU save's wrapper ContextUsed (the positions the save holds). False when the blob does not read.
	static bool GetSaveContextUsed(const TArray<uint8>& Blob, int64& OutContextUsed);
	// Ruling 59d point 6 (E14_5): a sequence's planning counts -- tokens issued (in flight included), the Exact drafts its
	// open chunks may still commit (ExactExtraBound, E-14) and whether an Exact close's result is awaited. False when absent.
	static bool GetPlanCounts(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence, int32& OutTokensIssued, int64& OutExactExtraBound, bool& bOutAwaitClose);
	// Ruling 59d point 6 (E14_5): one sample per Exact close applied while recording, taken right after it applies (the
	// planner held the sequence for it, so nothing else of it is in flight): the sequence, its tokens generated, its priced
	// next-row position and the Exact bound left. False when the seam is not built.
	static bool GetExactApplySamples(const USuperSLMGpuSubsystem& Gpu, int64 SequenceId, TArray<FIntVector4>& OutGeneratedNextPosBoundOpen);
	// Ruling 55 (F1): the position the subsystem prices the sequence's next row at (E-14: the position a one-row slice
	// begun now ends at). For a sequence begun at position 0, once its first token is produced, with no chunk open and
	// no result pending (PendingResultIds() 0), it is the prompt's length + the tokens produced - 1: the committed
	// rows, an Exact chunk's delivered drafts counted once. -1 when the seam is not built or the sequence is not live.
	static int64 GetNextRowPosition(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);
	// Ruling 56 (1): the layers a one-row slice that ends a token, ending at Position, may run beside the finish: the
	// largest l <= L with (l x (C(1, Position) - F) + F x L) x (1000 + m) <= Room x L x 1000 (x 1000 at B = 0), at least
	// 1 (SuperSLMGpuRowsChooser.h's FinishSliceLayers). -1 when the seam is not built or before a settled Configure().
	static int32 GetFinishSliceLayersAt(const USuperSLMGpuSubsystem& Gpu, int64 Position);
	// Ruling 56 (5): a save "equals" another in its live state. OutLive is Blob with the K/V of every position at or
	// past the saved sequence's length dropped: every header, the wrapper and its carried events, the metadata, the
	// tail, the residual and the K/V of positions [0, length) are kept, in order. A pooled blob that already holds only
	// [0, length) is returned whole. False (OutLive empty) when the blob is not one this build wrote, or the seam is not
	// built.
	static bool GetLiveSaveBytes(const USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, TArray<uint8>& OutLive);
	// Ruling 56 (6): one shot. The next Exact open (any sequence) takes the no-draft path, as if its forced chain were
	// empty and no draft table were loaded: it drafts nothing, falls back (ruling 42a) and is counted a no-draft
	// fallback (GetExactFallbacks). False when the seam is not built.
	static bool ArmNoDraftStep(USuperSLMGpuSubsystem& Gpu);
	// Ruling 57 (F1, F5), 57a: the widths phase 1's load-time warm-up of the widened row kernels takes at the last
	// Configure(): exactly the chooser's candidate widths (WidenCandidateMask()), ascending, each in 2..8; possibly
	// empty, and empty with multi-row off. Each width, in each phase-1 attempt, either runs once (a Calibration call of
	// one layer at M rows) or is skipped with a Display line naming it ("GPU GpuRowCost load-time warm-up for ...: M =
	// <M> skipped"); the smallest width runs in every attempt.
	// False when the seam is not built or before a Configure().
	static bool GetWidenWarmUpRows(const USuperSLMGpuSubsystem& Gpu, TArray<int32>& OutRows);
};

#endif // WITH_DEV_AUTOMATION_TESTS
