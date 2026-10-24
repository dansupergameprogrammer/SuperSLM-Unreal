#pragma once

// Schema jump-forward in the plugin (schema fast-decode plan rev 1.3.5, §6 slices P1 and P1-X;
// §5.6, §5.10.5, §5.10.6, §5.10.8; §17.11): the policy that needs no engine.
//
// What it owns:
//   - the three-way setting, Off | Exact | Canonical, default Off (the ruling of
//     2026-10-20 16:23Z: both modes are plain options), and the example's scratch switch values;
//   - the effective threaded-decode predicate and the exact-mode draft cap it implies (§5.10.6,
//     P1X.7; threaded decode is an opt-in, 16:24Z);
//   - the learned draft, D2-online (§5.10.5, §6 P1-X): a per-(model, schema) count table, walk
//     state -> token -> count, filled from finished generations that ended accepting (Off and
//     Exact; never Canonical), learning only at states observed forced, at most 4 tokens per state
//     with every token counted in the state's total, read as an immutable snapshot, replaced by
//     pointer on the game thread, dropped at model unmap; and the draft it yields, gated at a share
//     product of 0.5, the lowest token id on a tie, stopped at the first state the table does not
//     hold, clipped to the span budget;
//   - forcedness as the plugin observes it (rev 1.3.5): sslm_seq_forced_tokens (cap 0) read before
//     the first decode job and after every decode and span job, recorded per walk state;
//   - the span-after-decode loop's decisions (§6 P1 drain rule, P1-X loop): which job a sequence
//     takes next, whether it may join a decode batch, and the canonical span's chunk budget.
//
// It never calls Layer 1. The automaton it walks is supplied by the caller (ISchemaAutomaton): the
// transitions are SCM1's, which the subsystem reads itself (the ABI has no walk-state query), and
// the forced flag is the one the subsystem observed (FObservedSchemaAutomaton).
//
// Build constraints (it compiles inside Unreal later): C++17, standard library only, no exceptions,
// no RTTI, no dynamically initialised globals.
//
// The declarations below are the interface the host suite (ci/host/schema_jump/) tests;
// SuperSLMSchemaJumpCore.cpp implements them, and SuperSLMSubsystem.cpp is the one caller.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace SuperSLMCore
{
	// --- The setting (§6 P1-X) ---

	// SchemaJumpForward. Value-initialised, it is Off.
	enum class ESchemaJumpMode : uint8_t
	{
		Off = 0,
		Exact = 1,
		Canonical = 2,
	};

	// The example's scratch switch value (§6 P1, P1-X; cell P1.9): -SuperSLMExampleJumpForward=0|1
	// from P1 (0 Off, 1 Canonical) and off|exact|canonical from P1-X, the names in any case.
	// Returns true and sets OutMode for one of those. Returns false and leaves OutMode unchanged for
	// anything else: empty or padded text, another number, a null Text. The caller logs the refusal.
	bool ParseSchemaJumpSwitch(const char* Text, ESchemaJumpMode& OutMode);

	// --- Threaded decode (G24, §5.10.6) ---

	// The effective decode mode: bParallelDecode && FinishParallelTasks > 1.
	bool IsThreadedDecodeEffective(bool bParallelDecodeRequested, int32_t FinishParallelTasks);

	// Under effective threaded decode, exact-mode drafts are capped at this many tokens, so every
	// pass is M <= 7 and decomposes under the engine's Q6 C split (§5.10.6, P1X.7).
	constexpr int32_t kThreadedExactDraftCap = 6;

	// --- The automaton the draft walks ---

	// A schema's token DFA as the draft sees it. StartState and Step are SCM1's transitions. IsForced is
	// the OBSERVED flag (rev 1.3.5, §6 P1-X "Forcedness, observed"): true only for a state the
	// subsystem read as forced through sslm_seq_forced_tokens (cap 0, out_count > 0) or drafted
	// through with a confirmed draft; false for a state read unforced and for a state never read.
	// The subsystem's implementation is FObservedSchemaAutomaton.
	class ISchemaAutomaton
	{
	public:
		virtual ~ISchemaAutomaton() = default;
		// The walk state before the first generated token.
		virtual uint32_t StartState() const = 0;
		// True when Token is legal at State; OutNext is then the state after it.
		virtual bool Step(uint32_t State, int32_t Token, uint32_t& OutNext) const = 0;
		// True when State was observed forced (above).
		virtual bool IsForced(uint32_t State) const = 0;
	};

	// --- Forcedness, observed (rev 1.3.5) ---

	enum class EForcedObservation : uint8_t
	{
		Unread = 0,   // never read, never drafted through
		Unforced = 1, // read: sslm_seq_forced_tokens reported out_count == 0
		Forced = 2,   // read with out_count > 0, or inside a confirmed draft
	};

	// The forced flags the subsystem has observed for one (model, schema), per walk state. Game thread.
	class FForcednessObservations
	{
	public:
		// One sslm_seq_forced_tokens read (cap 0) at State: forced when out_count > 0.
		void RecordRead(uint32_t State, bool bForced);
		// State lay inside a draft the verb confirmed: the table held it, so it is forced.
		void RecordDraftedThrough(uint32_t State);
		EForcedObservation Get(uint32_t State) const;

		std::map<uint32_t, EForcedObservation> Flags;
	};

	// SCM1's transitions (Transitions.StartState/Step) with the observed flags (IsForced is
	// Observed.Get(State) == Forced; Transitions.IsForced is never consulted). Holds references.
	class FObservedSchemaAutomaton final : public ISchemaAutomaton
	{
	public:
		FObservedSchemaAutomaton(const ISchemaAutomaton& InTransitions, const FForcednessObservations& InObserved);
		uint32_t StartState() const override;
		bool Step(uint32_t State, int32_t Token, uint32_t& OutNext) const override;
		bool IsForced(uint32_t State) const override;

	private:
		const ISchemaAutomaton& Transitions;
		const FForcednessObservations& Observed;
	};

	// --- The learned draft table (D2-online, §5.10.5) ---

	// One row of a shipped seed table (§16 Q20: "optionally seeded from a shipped table").
	struct FDraftSeedRow
	{
		uint32_t State = 0;
		int32_t Token = 0;
		uint32_t Count = 0;
	};

	// One kept token: its count and, under the Space-Saving rule (rev 1.3.6), the part of that count
	// it inherited when it displaced another token (Error <= Count).
	struct FDraftKeptToken
	{
		int32_t Token = 0;
		uint64_t Count = 0;
		uint64_t Error = 0;
	};

	// One state's row: the tokens kept (at most MaxTokensPerState()) and the total of every token
	// observed there, kept or not.
	struct FDraftStateRow
	{
		std::vector<FDraftKeptToken> Tokens;
		uint64_t Total = 0;
	};

	// Walk state -> token -> count. Immutable once built: a merge builds a new table.
	class FDraftTable
	{
	public:
		// The cap on the tokens kept per state: 4 (rev 1.3.5). Rev 1.3.6: a token arriving at a full
		// state evicts by the Space-Saving rule (ErrorOf below), and every arrival adds to the
		// state's total (cell P1X.12 (c)).
		static uint32_t MaxTokensPerState();

		// States the table holds (each with at least one kept token).
		size_t StateCount() const;
		// Distinct tokens kept at State; 0 when State is not held.
		size_t TokenCountAt(uint32_t State) const;
		// The count of Token at State; 0 when either is not held.
		uint64_t CountOf(uint32_t State, int32_t Token) const;
		// Every token counted at State, kept or not; 0 when State is not held.
		uint64_t TotalAt(uint32_t State) const;

		// Rev 1.3.6 (review O1; §6 P1-X cap; P1X.12 (c); XD4), the Space-Saving rule. A token that
		// arrives at a full state replaces the kept token with the lowest count (the highest id on a
		// tie), takes that count plus one, and records the inherited part as its error. ErrorOf is
		// that error (0 for a token that never displaced one, and for a seed row's token);
		// GuaranteedCountOf is CountOf - ErrorOf. The draft and the share read the guaranteed count.
		// Both are 0 when State or Token is not held.
		uint64_t ErrorOf(uint32_t State, int32_t Token) const;
		uint64_t GuaranteedCountOf(uint32_t State, int32_t Token) const;

		// Internal representation, read only through the accessors above by every caller but the
		// builder functions below.
		std::map<uint32_t, FDraftStateRow> Rows;
	};

	using FDraftTableRef = std::shared_ptr<const FDraftTable>;

	// A new table: Base's counts (Base may be null) plus one finished stream, walked from
	// Automaton.StartState(). A token is counted only at a state Automaton.IsForced reports (the
	// observed flag), so the table never holds a state read unforced or never read (XD1). The walk
	// stops at the first token the automaton does not admit; tokens before it are counted.
	FDraftTableRef MergeFinishedStream(const FDraftTable* Base, const ISchemaAutomaton& Automaton,
		const int32_t* Tokens, size_t TokenCount);

	// A table holding the seed rows, each taken as a forced state's (the caller filters). Rows for
	// one (state, token) add. Rev 1.3.6 (the plan's ruling on a seed with more than 4 tokens
	// at a state): a state keeps its 4 highest counts, ties going to the lowest token id, each with
	// error 0; the dropped counts still go into the state's total.
	FDraftTableRef MakeSeedTable(const std::vector<FDraftSeedRow>& Rows);

	// A shipped table in the bench's text form, SFD-DRAFT-TABLE/1 (SuperSLM
	// tools/schema_fast_decode_bench.cpp, `drafttable`): a header line, "states N", "forced s s ...",
	// then "S state token:count ..." rows. Only the rows of states on the forced line are loaded (rev
	// 1.3.5: "only its forced states are loaded"), under the per-state cap. OutForcedStates (may be
	// null) receives the forced line. False, with Out null, for text not in that form.
	bool ParseShippedDraftTable(const char* Text, size_t Length, FDraftTableRef& Out,
		std::vector<uint32_t>* OutForcedStates);

	// What clips a draft (§6 P1-X; §5.10.6; P1X.5).
	struct FDraftLimits
	{
		// sslm_config.max_chunk_budget: the verb's chunk_budget is 1 + the draft's length, so a draft
		// is at most this minus 1.
		int32_t MaxPrefillChunkBudget = 0;
		// Output tokens the generation may still produce after the pending token (MaxNewTokens less
		// every token produced so far, the pending one included). Confirmed tokens are output. The
		// subsystem passes one less (its BuildDraft call): a span never admits the generation's last token,
		// so a MaxNewTokens cut leaves it pending, as off's decode does, and saves off's state (P1.11 (f)).
		int32_t RemainingAllowance = 0;
		// IsThreadedDecodeEffective(...): caps the draft at kThreadedExactDraftCap.
		bool bThreadedDecodeEffective = false;
	};

	// The most tokens a draft may hold under Limits: min(MaxPrefillChunkBudget - 1,
	// RemainingAllowance, kThreadedExactDraftCap when threaded), and never below 0.
	int32_t MaxDraftTokens(const FDraftLimits& Limits);

	// The D2-online draft from State, the walk state after the pending token. At each step: stop if
	// Table does not hold the state (it holds only states observed forced, so this is also the
	// unforced stop, rev 1.3.5); take the state's kept token with the highest guaranteed count (rev
	// 1.3.6: GuaranteedCountOf), the lowest token id on a tie (XD5); stop unless the running product
	// of the drafted tokens' shares (guaranteed count / TotalAt) stays
	// >= 1/2, computed exactly; append it and step with Automaton. Clipped to MaxDraftTokens(Limits).
	// A null Table drafts nothing. An empty draft sends no span job (cold start, P1X.1).
	std::vector<int32_t> BuildDraft(const FDraftTable* Table, const ISchemaAutomaton& Automaton,
		uint32_t State, const FDraftLimits& Limits);

	// --- A span's result on the tracked walk (rev 1.3.6, §6 P1-X "The walk"; P1X.12 (f); XD10) ---

	// What WalkSpanResult did.
	struct FSpanWalkResult
	{
		// False when a confirmed id is not admitted where the walk stands: the walk is then unknown
		// for the rest of the generation, and the span's read is dropped.
		bool bWalkKnown = false;
		// The walk state after the last confirmed id (State unchanged when bWalkKnown is false).
		uint32_t State = 0;
		// The confirmed ids the output takes: up to and including the first one in StopTokens, or
		// all Count of them.
		size_t AppendCount = 0;
		// Whether the span's read was recorded (at State, never at an earlier state).
		bool bReadRecorded = false;
	};

	// A span job's confirmed ids (the verb's out_forced) applied to a generation's tracked walk from
	// State. Every confirmed id is walked first, and the state each one leaves is recorded as drafted
	// through, before any stop-token check: the engine committed all Count ids. Only then is the
	// output cut at the first id in StopTokens. The span's read (bTailRead; bTailForced is whether it
	// reported a chain) was taken by the engine after all Count ids, so it is recorded at the state
	// after the last one; when the walk cannot reach that state it is dropped, never recorded at
	// another state (review finding 5: recording it where a stop token broke the loop marks a
	// forced state Unforced).
	FSpanWalkResult WalkSpanResult(const ISchemaAutomaton& Automaton, FForcednessObservations& Observations, uint32_t State,
		const int32_t* Confirmed, size_t Count, const int32_t* StopTokens, size_t StopCount, bool bTailRead, bool bTailForced);

	// --- The per-(model, schema) store: snapshots, merge on completion, lifetime (§6 P1-X) ---

	struct FDraftTableKey
	{
		uint64_t Model = 0;  // the mapped model's identity; a remap is a new identity or a drop
		uint32_t Schema = 0; // the schema's index in the model's SCM1 section
	};

	// Game-thread object. A span job takes Snapshot() when it is planned and reads only that
	// snapshot, on any thread, for its whole life; a later merge never changes it.
	class FDraftTableStore
	{
	public:
		// The current table for Key; null when there is none.
		FDraftTableRef Snapshot(const FDraftTableKey& Key) const;

		// Installs a shipped table for Key; later merges add to it.
		void Seed(const FDraftTableKey& Key, FDraftTableRef Table);

		// A CPU generation under Key's schema has ended: merge its stream into a new table and swap it
		// in by pointer. Only the model's own, finished output is learned (rev 1.3.5): an Off or Exact
		// generation that ended with the walk accepting feeds the table; a Canonical one never does
		// (its forced tokens are not the model's), nor does one that did not end accepting (cut by
		// MaxNewTokens, a fault, a cancel or a release). The GPU never calls this.
		void OnGenerationFinished(const FDraftTableKey& Key, ESchemaJumpMode Mode, bool bEndedAccepting,
			const ISchemaAutomaton& Automaton, const int32_t* Tokens, size_t TokenCount);

		// The model is unmapped: every table keyed by it is dropped (§6 P1-X Lifetime).
		void DropModel(uint64_t Model);

		// Tables held, all keys.
		size_t TableCount() const;

		std::map<std::pair<uint64_t, uint32_t>, FDraftTableRef> Tables;
	};

	// --- The span-after-decode loop (§6 P1 drain rule; §6 P1-X loop) ---

	enum class EJumpNextJob : uint8_t
	{
		Decode = 0, // may join a decode batch
		Span = 1,   // a span job: one sslm_seq_jump_forward call
		Wait = 2,   // a span job is outstanding: neither
		End = 3,    // the allowance is spent: no job
	};

	// What the scheduler knows about one sequence after its last applied job.
	struct FJumpSequenceFacts
	{
		ESchemaJumpMode Mode = ESchemaJumpMode::Off;
		bool bGpu = false;
		bool bSchemaBound = false;
		// A decode produced a token not yet embedded (§5.4).
		bool bPendingToken = false;
		// sslm_seq_forced_tokens' out_count at the current walk state, read at the last job's tail.
		int32_t ForcedChainLength = 0;
		// The last applied job was a span job.
		bool bLastJobWasSpan = false;
		// The last span job's verb returned a status other than SSLM_OK.
		bool bLastSpanFailed = false;
		// A span job has been planned and not yet applied.
		bool bSpanJobOutstanding = false;
		// MaxNewTokens less the tokens produced so far.
		int32_t RemainingAllowance = 0;
		// Exact only: the learned draft's length from the state after the pending token, after
		// clipping (BuildDraft(...).size()). Exact takes a span job only when it is > 0 (rev 1.3.5
		// cold start: no span job passes a NULL or an empty draft, XD2).
		int32_t DraftLength = 0;
	};

	EJumpNextJob NextJob(const FJumpSequenceFacts& Facts);

	// True exactly when NextJob(Facts) is Decode.
	bool MayJoinDecodeBatch(const FJumpSequenceFacts& Facts);

	// A canonical span job's chunk_budget: the pending token (if any) plus forced tokens, clipped
	// to MaxPrefillChunkBudget and to the allowance (forced tokens count toward MaxNewTokens, §16
	// Q12). 0 when no forced token fits.
	int32_t CanonicalSpanChunkBudget(const FJumpSequenceFacts& Facts, int32_t MaxPrefillChunkBudget);
}
