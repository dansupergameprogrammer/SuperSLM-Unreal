#pragma once

// Test-only access to the CPU subsystem's schema jump-forward (schema fast-decode plan rev 1.3.5,
// §6 slices P1 and P1-X; cells §11.3 P1.1-P1.9 and P1X.1-P1X.12). Compiled only under
// WITH_DEV_AUTOMATION_TESTS. Game thread only. It records and seeds; it never changes a token.
//
// Written ahead of the implementation; the declarations are the contract the
// cells are written against, and the implementer may widen them but not narrow them. The bodies
// live in Private/SuperSLMSubsystem.cpp, beside the state they read (as the scheduling and paged
// seams' do); IsBuilt() reads true there. Every cell still reads SuperSLMSchemaJumpVerbsAreReal()
// first, so a run before the engine re-vendor fails by name ("the engine verbs are red stubs").

#include "CoreMinimal.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSchemaJumpTypes.h"
#include "SuperSLMSequenceTypes.h"

class USuperSLMSubsystem;

#if WITH_DEV_AUTOMATION_TESTS

// One span job: one sslm_seq_jump_forward call (§6: "one call per job"). Recorded at plan time and
// completed at delivery.
struct SUPERSLMUNREAL_API FSuperSLMSpanJobRecord
{
	// Drawn from the same monotonic counter as FSuperSLMWorkerJobReport::JobId (rev 1.3.5: one
	// counter), so span jobs and ledger rows order against each other by JobId alone and never share
	// one (cells P1.2, P1X.2; mutant XD6).
	int64 JobId = 0;
	FSuperSLMSequence Sequence;
	ESuperSLMSchemaJumpForward Mode = ESuperSLMSchemaJumpForward::Off;
	int32 PlannedAtTick = -1;
	int32 DeliveredAtTick = -1;

	// The call's arguments. bDraftNull is opts.draft == NULL (or opts itself NULL, which means
	// canonical); Draft is the draft passed, empty when bDraftNull. Rev 1.3.5: an Exact span job
	// always passes a non-NULL draft of at least one token (cold start sends no span job, P1X.1).
	int32 ChunkBudget = 0;
	bool bDraftNull = true;
	TArray<int32> Draft;
	// Exact mode: the length of the draft the table gave with the whole allowance (before the planner's clip to one
	// less, so a span never admits the generation's last allowed token), and its length after that clip and before
	// the cut at the first stop token. Draft.Num() is after both. -1 for a canonical job.
	int32 DraftLengthBeforeAllowanceClip = -1;
	int32 DraftLengthBeforeStopCut = -1;

	// The job's planned cost (§6 "Pricing": a span job is priced at its full drafted M with the
	// per-C table), the M it was priced at, and the depth (the sequence's context length) it was
	// priced from.
	double PlannedJobMs = 0.0;
	int32 PricedChunk = 0;
	int64 StartDepth = -1;

	// The call's result: its status (an sslm_status as int32; SSLM_OK = 0), the ids it returned in
	// out_forced, and the forced chain still left after it (sslm_seq_forced_tokens' out_count read
	// at delivery; > 0 means the chain was split by the budget, cell P1.2).
	int32 Status = -1;
	TArray<int32> ForcedIds;
	int32 ForcedChainLeftAfter = 0;

	// GetGeneratedTokens(Sequence).Num() when the job was delivered, before its ids were appended.
	int32 OutputIndexBefore = -1;

	// The draft table snapshot the job read: a counter that changes on every swap (cell P1X.8).
	int64 DraftSnapshotVersion = -1;
};

// Where a held sequence stops (FSuperSLMSchemaJumpTestAccess::SetJobHold).
enum class ESuperSLMJobHoldPoint : uint8
{
	None,              // no hold
	AfterSpanDelivery, // after each span job of the sequence is delivered, before its next job is planned
	BeforeDecode,      // when the sequence would next join a decode job, before that job is planned
};

struct SUPERSLMUNREAL_API FSuperSLMSchemaJumpTestAccess
{
	// True: the seam is built (its bodies are in Private/SuperSLMSubsystem.cpp); the cells read it first.
	static bool IsBuilt();

	// Starts recording span jobs on the configured subsystem, clearing earlier records. A later
	// Configure() clears the records and stops recording.
	static void StartRecording(USuperSLMSubsystem& Subsystem);
	static TArray<FSuperSLMSpanJobRecord> GetSpanJobs(const USuperSLMSubsystem& Subsystem);

	// --- The D2-online draft table (§5.10.5, §6 P1-X), per (the configured model, Schema) ---

	// Whether a table exists for Schema under the configured model, how many walk states it holds,
	// and the longest token list at any one state. The bound (cell P1X.11): every held state is one
	// observed forced (IsEveryDraftTableStateObservedForced), so StateCount is at most
	// GetSchemaForcedStateCount, and MaxTokensAtAnyState is at most MaxTokensPerState(), which is 4.
	static bool HasDraftTable(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);
	static int32 GetDraftTableStateCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);
	static int32 GetDraftTableMaxTokensAtAnyState(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);
	// The schema's forced states, from SCM1 and the library's flags as the subsystem observed them
	// (rev 1.3.5: the states read forced or drafted through so far).
	static int32 GetSchemaForcedStateCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);
	// True when every state the table holds was observed forced (read with out_count > 0 or drafted
	// through); false when one was read unforced or never observed (mutant XD1), or no table exists.
	static bool IsEveryDraftTableStateObservedForced(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);
	static int32 MaxTokensPerState();

	// Incremented on every snapshot swap for Schema (cell P1X.8); -1 when no table exists.
	static int64 GetDraftSnapshotVersion(const USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);

	// Seeds Schema's table as the shipped-table path would (§16 Q20), from whole token streams
	// walked from the schema's start state, counting only at states with the library's forced flag
	// (rev 1.3.5: a shipped seed carries the flags, and only its forced states are loaded).
	// Replaces any table. False when not configured.
	static bool SeedDraftTableFromStreams(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema,
		const TArray<TArray<int32>>& Streams);

	// Deliberately corrupts Schema's table (cell P1X.1): at every held state with two or more legal
	// tokens, a legal token other than the row's leader is given a count that dominates the row, so
	// a draft through such a state is wrong there and the verb rejects it. (A token the walk does not
	// admit would only end the draft early, rev 1.3.5, and test nothing.) False when no table exists.
	static bool CorruptDraftTable(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);

	// Drops Schema's table (an empty table is the cold start).
	static void ClearDraftTable(USuperSLMSubsystem& Subsystem, const FSuperSLMSchemaHandle& Schema);

	// --- Forcedness reads (rev 1.3.5, cell P1X.12 (e)) ---

	// The sslm_seq_forced_tokens (cap 0) reads the subsystem has made since StartRecording(): one
	// before the first decode job and one after every decode and span job of each schema-bound Off
	// or Exact generation.
	static int64 GetForcednessReadCount(const USuperSLMSubsystem& Subsystem);

	// Turns the reads off (and the learning that needs them) until set back on or the next
	// Configure(). Exists only so P1X.12 (e) can show that the reads change nothing.
	static void SetForcednessReadsEnabled(USuperSLMSubsystem& Subsystem, bool bEnabled);

	// --- Failure injection (cell P1X.10). The plan says "through the engine seams"; the plugin
	// cannot reach the engine's, so the plugin-side override stands in for them. Precedent:
	// FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride. ---

	// The next span job the configured subsystem dispatches returns Status instead of calling
	// sslm_seq_jump_forward, with out_forced_count 0 and the sequence as Layer 1 had it. Taken when
	// that job is dispatched; SSLM_OK (0) clears it. A later Configure() clears it.
	static void SetNextSpanStatusOverride(USuperSLMSubsystem& Subsystem, int32 Status);

	// The job hold, modelled on the precache build hold and lane park: a test holds a vended sequence at Point,
	// and while it is held the planner plans no job for it, so a window that exists only inside one tick (the gap
	// after a span job, three sequences at one decode) stays open between ticks. Nothing else changes: a released
	// sequence goes on exactly as it would have. SetJobHold arms Point (None removes the hold); IsJobHeld reads
	// whether the sequence is stopped there now; ReleaseJobHold lets it go on until it reaches Point again.
	// Returning the sequence ends its hold. A later Configure() clears every hold.
	static void SetJobHold(USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence, ESuperSLMJobHoldPoint Point);
	static bool IsJobHeld(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);
	static void ReleaseJobHold(USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);
};

#endif // WITH_DEV_AUTOMATION_TESTS
