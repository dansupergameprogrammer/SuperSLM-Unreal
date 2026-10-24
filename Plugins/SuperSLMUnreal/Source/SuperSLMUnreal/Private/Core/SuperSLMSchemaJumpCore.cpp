// Schema jump-forward's engine-free policy (schema fast-decode plan rev 1.3.5, §6 P1 and P1-X). See
// SuperSLMSchemaJumpCore.h for the contract. C++17, standard library only, no exceptions, no RTTI.

#include "SuperSLMSchemaJumpCore.h"

#include <algorithm>

namespace SuperSLMCore
{
	namespace
	{
		constexpr uint32_t kMaxTokensPerState = 4;

		// ASCII case-insensitive equality of a NUL-terminated Text with a lower-case Word.
		bool EqualsIgnoringCase(const char* Text, const char* Word)
		{
			for (; *Word != '\0'; ++Text, ++Word)
			{
				char C = *Text;
				if (C >= 'A' && C <= 'Z')
				{
					C = static_cast<char>(C - 'A' + 'a');
				}
				if (C != *Word)
				{
					return false;
				}
			}
			return *Text == '\0';
		}

		// Adds Count observations of Token to Row under the per-state cap, by the Space-Saving rule
		// (rev 1.3.6, §6 P1-X; P1X.12 (c)): a token already kept gains the count; a new token is kept
		// while the row has room, with error 0; at a full row it replaces the kept token with the
		// lowest count (the highest id on a tie), takes that count plus Count, and records the
		// inherited part as its error. Every count reaches the total.
		void AddObservation(FDraftStateRow& Row, int32_t Token, uint64_t Count)
		{
			Row.Total += Count;
			for (FDraftKeptToken& Kept : Row.Tokens)
			{
				if (Kept.Token == Token)
				{
					Kept.Count += Count;
					return;
				}
			}
			if (Row.Tokens.size() < kMaxTokensPerState)
			{
				Row.Tokens.push_back({Token, Count, 0});
				return;
			}
			FDraftKeptToken* Weakest = nullptr;
			for (FDraftKeptToken& Kept : Row.Tokens)
			{
				if (Weakest == nullptr || Kept.Count < Weakest->Count ||
					(Kept.Count == Weakest->Count && Kept.Token > Weakest->Token))
				{
					Weakest = &Kept;
				}
			}
			const uint64_t Inherited = Weakest->Count;
			*Weakest = FDraftKeptToken{Token, Inherited + Count, Inherited};
		}

		const FDraftKeptToken* FindKept(const std::map<uint32_t, FDraftStateRow>& Rows, uint32_t State, int32_t Token)
		{
			const auto It = Rows.find(State);
			if (It == Rows.end())
			{
				return nullptr;
			}
			for (const FDraftKeptToken& Kept : It->second.Tokens)
			{
				if (Kept.Token == Token)
				{
					return &Kept;
				}
			}
			return nullptr;
		}

		// An unsigned integer of any size, little-endian 32-bit limbs: the share-product gate's exact
		// arithmetic. A draft is at most a few dozen tokens, so the numbers stay a few hundred bytes.
		class FBigNat
		{
		public:
			explicit FBigNat(uint32_t Value) { Limbs.push_back(Value); }

			void MulSmall(uint32_t Factor)
			{
				uint64_t Carry = 0;
				for (uint32_t& Limb : Limbs)
				{
					const uint64_t Product = static_cast<uint64_t>(Limb) * Factor + Carry;
					Limb = static_cast<uint32_t>(Product & 0xFFFFFFFFu);
					Carry = Product >> 32;
				}
				if (Carry != 0)
				{
					Limbs.push_back(static_cast<uint32_t>(Carry));
				}
				Trim();
			}

			void Mul(uint64_t Factor)
			{
				const uint32_t Low = static_cast<uint32_t>(Factor & 0xFFFFFFFFu);
				const uint32_t High = static_cast<uint32_t>(Factor >> 32);
				if (High == 0)
				{
					MulSmall(Low);
					return;
				}
				// this x (High x 2^32 + Low) = (this x High) shifted one limb + this x Low.
				FBigNat HighPart = *this;
				HighPart.MulSmall(High);
				HighPart.Limbs.insert(HighPart.Limbs.begin(), 0u);
				MulSmall(Low);
				Add(HighPart);
			}

			// -1, 0 or 1 as this is below, equal to or above Other.
			int Compare(const FBigNat& Other) const
			{
				if (Limbs.size() != Other.Limbs.size())
				{
					return Limbs.size() < Other.Limbs.size() ? -1 : 1;
				}
				for (size_t I = Limbs.size(); I-- > 0;)
				{
					if (Limbs[I] != Other.Limbs[I])
					{
						return Limbs[I] < Other.Limbs[I] ? -1 : 1;
					}
				}
				return 0;
			}

		private:
			void Add(const FBigNat& Other)
			{
				if (Limbs.size() < Other.Limbs.size())
				{
					Limbs.resize(Other.Limbs.size(), 0u);
				}
				uint64_t Carry = 0;
				for (size_t I = 0; I < Limbs.size(); ++I)
				{
					const uint64_t Sum = static_cast<uint64_t>(Limbs[I]) + (I < Other.Limbs.size() ? Other.Limbs[I] : 0u) + Carry;
					Limbs[I] = static_cast<uint32_t>(Sum & 0xFFFFFFFFu);
					Carry = Sum >> 32;
				}
				if (Carry != 0)
				{
					Limbs.push_back(static_cast<uint32_t>(Carry));
				}
				Trim();
			}

			void Trim()
			{
				while (Limbs.size() > 1 && Limbs.back() == 0)
				{
					Limbs.pop_back();
				}
			}

			std::vector<uint32_t> Limbs;
		};

		// A minimal cursor over the shipped table's text (no exceptions, no locale).
		struct FTextCursor
		{
			const char* Pos;
			const char* End;

			bool AtLineEnd() const { return Pos >= End || *Pos == '\n' || *Pos == '\r'; }

			void SkipBlanks()
			{
				while (Pos < End && (*Pos == ' ' || *Pos == '\t'))
				{
					++Pos;
				}
			}

			void NextLine()
			{
				while (Pos < End && *Pos != '\n')
				{
					++Pos;
				}
				if (Pos < End)
				{
					++Pos;
				}
			}

			// The next word on this line (up to a blank or the line end).
			bool Word(const char*& OutBegin, size_t& OutLength)
			{
				SkipBlanks();
				OutBegin = Pos;
				while (Pos < End && *Pos != ' ' && *Pos != '\t' && *Pos != '\n' && *Pos != '\r')
				{
					++Pos;
				}
				OutLength = static_cast<size_t>(Pos - OutBegin);
				return OutLength > 0;
			}
		};

		bool WordIs(const char* Begin, size_t Length, const char* Literal)
		{
			size_t I = 0;
			for (; I < Length && Literal[I] != '\0'; ++I)
			{
				if (Begin[I] != Literal[I])
				{
					return false;
				}
			}
			return I == Length && Literal[I] == '\0';
		}

		// Decimal digits only, no sign, no overflow past Max.
		bool ParseUnsigned(const char* Begin, size_t Length, uint64_t Max, uint64_t& Out)
		{
			if (Length == 0)
			{
				return false;
			}
			uint64_t Value = 0;
			for (size_t I = 0; I < Length; ++I)
			{
				const char C = Begin[I];
				if (C < '0' || C > '9')
				{
					return false;
				}
				const uint64_t Digit = static_cast<uint64_t>(C - '0');
				// Digit > Max first: Max - Digit would wrap (review finding 6).
				if (Digit > Max || Value > (Max - Digit) / 10)
				{
					return false;
				}
				Value = Value * 10 + Digit;
			}
			Out = Value;
			return true;
		}
	}

	// --- The setting ---

	bool ParseSchemaJumpSwitch(const char* Text, ESchemaJumpMode& OutMode)
	{
		if (Text == nullptr)
		{
			return false;
		}
		if (Text[0] == '0' && Text[1] == '\0')
		{
			OutMode = ESchemaJumpMode::Off;
			return true;
		}
		if (Text[0] == '1' && Text[1] == '\0')
		{
			OutMode = ESchemaJumpMode::Canonical;
			return true;
		}
		if (EqualsIgnoringCase(Text, "off"))
		{
			OutMode = ESchemaJumpMode::Off;
			return true;
		}
		if (EqualsIgnoringCase(Text, "exact"))
		{
			OutMode = ESchemaJumpMode::Exact;
			return true;
		}
		if (EqualsIgnoringCase(Text, "canonical"))
		{
			OutMode = ESchemaJumpMode::Canonical;
			return true;
		}
		return false;
	}

	bool IsThreadedDecodeEffective(bool bParallelDecodeRequested, int32_t FinishParallelTasks)
	{
		return bParallelDecodeRequested && FinishParallelTasks > 1;
	}

	// --- Forcedness, observed ---

	void FForcednessObservations::RecordRead(uint32_t State, bool bForced)
	{
		Flags[State] = bForced ? EForcedObservation::Forced : EForcedObservation::Unforced;
	}

	void FForcednessObservations::RecordDraftedThrough(uint32_t State)
	{
		Flags[State] = EForcedObservation::Forced;
	}

	EForcedObservation FForcednessObservations::Get(uint32_t State) const
	{
		const auto It = Flags.find(State);
		return It == Flags.end() ? EForcedObservation::Unread : It->second;
	}

	FObservedSchemaAutomaton::FObservedSchemaAutomaton(const ISchemaAutomaton& InTransitions, const FForcednessObservations& InObserved)
		: Transitions(InTransitions), Observed(InObserved)
	{
	}

	uint32_t FObservedSchemaAutomaton::StartState() const
	{
		return Transitions.StartState();
	}

	bool FObservedSchemaAutomaton::Step(uint32_t State, int32_t Token, uint32_t& OutNext) const
	{
		return Transitions.Step(State, Token, OutNext);
	}

	bool FObservedSchemaAutomaton::IsForced(uint32_t State) const
	{
		return Observed.Get(State) == EForcedObservation::Forced;
	}

	// --- The table ---

	uint32_t FDraftTable::MaxTokensPerState()
	{
		return kMaxTokensPerState;
	}

	size_t FDraftTable::StateCount() const
	{
		size_t Count = 0;
		for (const auto& Row : Rows)
		{
			Count += Row.second.Tokens.empty() ? 0u : 1u;
		}
		return Count;
	}

	size_t FDraftTable::TokenCountAt(uint32_t State) const
	{
		const auto It = Rows.find(State);
		return It == Rows.end() ? 0u : It->second.Tokens.size();
	}

	uint64_t FDraftTable::CountOf(uint32_t State, int32_t Token) const
	{
		const FDraftKeptToken* Kept = FindKept(Rows, State, Token);
		return Kept == nullptr ? 0u : Kept->Count;
	}

	uint64_t FDraftTable::ErrorOf(uint32_t State, int32_t Token) const
	{
		const FDraftKeptToken* Kept = FindKept(Rows, State, Token);
		return Kept == nullptr ? 0u : Kept->Error;
	}

	uint64_t FDraftTable::GuaranteedCountOf(uint32_t State, int32_t Token) const
	{
		const FDraftKeptToken* Kept = FindKept(Rows, State, Token);
		return Kept == nullptr || Kept->Error > Kept->Count ? 0u : Kept->Count - Kept->Error;
	}

	uint64_t FDraftTable::TotalAt(uint32_t State) const
	{
		const auto It = Rows.find(State);
		return It == Rows.end() || It->second.Tokens.empty() ? 0u : It->second.Total;
	}

	FDraftTableRef MergeFinishedStream(const FDraftTable* Base, const ISchemaAutomaton& Automaton,
		const int32_t* Tokens, size_t TokenCount)
	{
		std::shared_ptr<FDraftTable> Next = std::make_shared<FDraftTable>();
		if (Base != nullptr)
		{
			Next->Rows = Base->Rows;
		}
		uint32_t State = Automaton.StartState();
		for (size_t I = 0; I < TokenCount && Tokens != nullptr; ++I)
		{
			uint32_t After = 0;
			if (!Automaton.Step(State, Tokens[I], After))
			{
				break;
			}
			if (Automaton.IsForced(State))
			{
				AddObservation(Next->Rows[State], Tokens[I], 1);
			}
			State = After;
		}
		return Next;
	}

	FDraftTableRef MakeSeedTable(const std::vector<FDraftSeedRow>& Rows)
	{
		// Every (state, token) count first, so the cap sees the seed's whole state at once.
		std::map<uint32_t, std::map<int32_t, uint64_t>> Counts;
		for (const FDraftSeedRow& Row : Rows)
		{
			if (Row.Count > 0)
			{
				Counts[Row.State][Row.Token] += Row.Count;
			}
		}
		std::shared_ptr<FDraftTable> Table = std::make_shared<FDraftTable>();
		for (const auto& State : Counts)
		{
			std::vector<FDraftKeptToken> All;
			FDraftStateRow& Row = Table->Rows[State.first];
			for (const auto& TokenCount : State.second)
			{
				All.push_back({TokenCount.first, TokenCount.second, 0});
				Row.Total += TokenCount.second;
			}
			// The highest counts first, the lowest id on a tie; keep the first four.
			std::sort(All.begin(), All.end(), [](const FDraftKeptToken& A, const FDraftKeptToken& B)
			{
				return A.Count != B.Count ? A.Count > B.Count : A.Token < B.Token;
			});
			if (All.size() > kMaxTokensPerState)
			{
				All.resize(kMaxTokensPerState);
			}
			Row.Tokens = All;
		}
		return Table;
	}

	bool ParseShippedDraftTable(const char* Text, size_t Length, FDraftTableRef& Out,
		std::vector<uint32_t>* OutForcedStates)
	{
		Out = nullptr;
		if (Text == nullptr)
		{
			return false;
		}
		FTextCursor Cursor{Text, Text + Length};
		const char* W = nullptr;
		size_t WL = 0;

		// The header: the form's name, then free text to the line end.
		if (!Cursor.Word(W, WL) || !WordIs(W, WL, "SFD-DRAFT-TABLE/1"))
		{
			return false;
		}
		Cursor.NextLine();

		bool bHaveStates = false;
		bool bHaveForced = false;
		uint64_t StateCount = 0;
		std::vector<uint32_t> Forced;
		std::vector<FDraftSeedRow> Rows;
		while (Cursor.Pos < Cursor.End)
		{
			if (!Cursor.Word(W, WL))
			{
				Cursor.NextLine(); // a blank line
				continue;
			}
			if (WordIs(W, WL, "states"))
			{
				if (bHaveStates || !Cursor.Word(W, WL) || !ParseUnsigned(W, WL, 0xFFFFFFFFull, StateCount))
				{
					return false;
				}
				bHaveStates = true;
			}
			else if (WordIs(W, WL, "forced"))
			{
				if (!bHaveStates || bHaveForced)
				{
					return false;
				}
				bHaveForced = true;
				while (Cursor.Word(W, WL))
				{
					uint64_t S = 0;
					if (!ParseUnsigned(W, WL, StateCount == 0 ? 0 : StateCount - 1, S) || StateCount == 0)
					{
						return false;
					}
					Forced.push_back(static_cast<uint32_t>(S));
				}
			}
			else if (WordIs(W, WL, "S"))
			{
				uint64_t S = 0;
				if (!bHaveForced || !Cursor.Word(W, WL) || StateCount == 0 || !ParseUnsigned(W, WL, StateCount - 1, S))
				{
					return false;
				}
				while (Cursor.Word(W, WL))
				{
					size_t Colon = 0;
					while (Colon < WL && W[Colon] != ':')
					{
						++Colon;
					}
					uint64_t Token = 0;
					uint64_t Count = 0;
					if (Colon == WL || !ParseUnsigned(W, Colon, 0x7FFFFFFFull, Token) ||
						!ParseUnsigned(W + Colon + 1, WL - Colon - 1, 0xFFFFFFFFull, Count))
					{
						return false;
					}
					Rows.push_back({static_cast<uint32_t>(S), static_cast<int32_t>(Token), static_cast<uint32_t>(Count)});
				}
			}
			else
			{
				return false;
			}
			Cursor.SkipBlanks();
			if (!Cursor.AtLineEnd())
			{
				return false;
			}
			Cursor.NextLine();
		}
		if (!bHaveForced)
		{
			return false;
		}

		// Only the rows of states on the forced line are loaded.
		std::vector<uint32_t> SortedForced = Forced;
		std::sort(SortedForced.begin(), SortedForced.end());
		std::vector<FDraftSeedRow> ForcedRows;
		for (const FDraftSeedRow& Row : Rows)
		{
			if (std::binary_search(SortedForced.begin(), SortedForced.end(), Row.State))
			{
				ForcedRows.push_back(Row);
			}
		}
		Out = MakeSeedTable(ForcedRows);
		if (OutForcedStates != nullptr)
		{
			*OutForcedStates = Forced;
		}
		return true;
	}

	// --- The draft ---

	int32_t MaxDraftTokens(const FDraftLimits& Limits)
	{
		int32_t Max = Limits.MaxPrefillChunkBudget - 1;
		Max = std::min(Max, Limits.RemainingAllowance);
		if (Limits.bThreadedDecodeEffective)
		{
			Max = std::min(Max, kThreadedExactDraftCap);
		}
		return std::max(Max, 0);
	}

	std::vector<int32_t> BuildDraft(const FDraftTable* Table, const ISchemaAutomaton& Automaton,
		uint32_t State, const FDraftLimits& Limits)
	{
		std::vector<int32_t> Draft;
		if (Table == nullptr)
		{
			return Draft;
		}
		const size_t Max = static_cast<size_t>(MaxDraftTokens(Limits));
		// The running product of shares is Numerator / Denominator; the gate holds while
		// 2 x Numerator >= Denominator, compared exactly.
		FBigNat Numerator(1);
		FBigNat Denominator(1);
		uint32_t Current = State;
		while (Draft.size() < Max)
		{
			const auto It = Table->Rows.find(Current);
			if (It == Table->Rows.end() || It->second.Tokens.empty() || It->second.Total == 0)
			{
				break; // a state the table does not hold: unseen, or never observed forced
			}
			// The highest guaranteed count (count - error, rev 1.3.6), the lowest id on a tie.
			const FDraftKeptToken* Leader = nullptr;
			uint64_t LeaderGuaranteed = 0;
			for (const FDraftKeptToken& Kept : It->second.Tokens)
			{
				const uint64_t Guaranteed = Kept.Error > Kept.Count ? 0u : Kept.Count - Kept.Error;
				if (Leader == nullptr || Guaranteed > LeaderGuaranteed ||
					(Guaranteed == LeaderGuaranteed && Kept.Token < Leader->Token))
				{
					Leader = &Kept;
					LeaderGuaranteed = Guaranteed;
				}
			}
			Numerator.Mul(LeaderGuaranteed);
			Denominator.Mul(It->second.Total);
			FBigNat Twice = Numerator;
			Twice.MulSmall(2);
			if (Twice.Compare(Denominator) < 0)
			{
				break;
			}
			uint32_t Next = 0;
			if (!Automaton.Step(Current, Leader->Token, Next))
			{
				break; // a stale or corrupted row: never draft a token the automaton refuses
			}
			Draft.push_back(Leader->Token);
			Current = Next;
		}
		return Draft;
	}

	// --- The store ---

	FDraftTableRef FDraftTableStore::Snapshot(const FDraftTableKey& Key) const
	{
		const auto It = Tables.find({Key.Model, Key.Schema});
		return It == Tables.end() ? nullptr : It->second;
	}

	void FDraftTableStore::Seed(const FDraftTableKey& Key, FDraftTableRef Table)
	{
		if (Table == nullptr)
		{
			Tables.erase({Key.Model, Key.Schema});
			return;
		}
		Tables[{Key.Model, Key.Schema}] = std::move(Table);
	}

	void FDraftTableStore::OnGenerationFinished(const FDraftTableKey& Key, ESchemaJumpMode Mode, bool bEndedAccepting,
		const ISchemaAutomaton& Automaton, const int32_t* Tokens, size_t TokenCount)
	{
		if (Mode == ESchemaJumpMode::Canonical || !bEndedAccepting)
		{
			return;
		}
		FDraftTableRef& Slot = Tables[{Key.Model, Key.Schema}];
		Slot = MergeFinishedStream(Slot.get(), Automaton, Tokens, TokenCount);
	}

	void FDraftTableStore::DropModel(uint64_t Model)
	{
		for (auto It = Tables.begin(); It != Tables.end();)
		{
			if (It->first.first == Model)
			{
				It = Tables.erase(It);
			}
			else
			{
				++It;
			}
		}
	}

	size_t FDraftTableStore::TableCount() const
	{
		return Tables.size();
	}

	// --- The loop ---

	EJumpNextJob NextJob(const FJumpSequenceFacts& Facts)
	{
		if (Facts.RemainingAllowance <= 0)
		{
			return EJumpNextJob::End;
		}
		if (Facts.bSpanJobOutstanding)
		{
			return EJumpNextJob::Wait;
		}
		if (Facts.bGpu || !Facts.bSchemaBound || Facts.Mode == ESchemaJumpMode::Off)
		{
			return EJumpNextJob::Decode;
		}
		if (Facts.Mode == ESchemaJumpMode::Canonical)
		{
			// The drain rule (§5.6): span until the chain reports 0 or a call returns non-OK.
			if (Facts.bLastJobWasSpan && Facts.bLastSpanFailed)
			{
				return EJumpNextJob::Decode;
			}
			return Facts.ForcedChainLength > 0 ? EJumpNextJob::Span : EJumpNextJob::Decode;
		}
		// Exact (§6 P1-X loop): one span job after a decode job that left a pending token at a forced
		// state, when the learned draft is non-empty; never two in a row.
		if (Facts.bLastJobWasSpan)
		{
			return EJumpNextJob::Decode;
		}
		return Facts.bPendingToken && Facts.ForcedChainLength > 0 && Facts.DraftLength > 0 ? EJumpNextJob::Span
			: EJumpNextJob::Decode;
	}

	bool MayJoinDecodeBatch(const FJumpSequenceFacts& Facts)
	{
		return NextJob(Facts) == EJumpNextJob::Decode;
	}

	int32_t CanonicalSpanChunkBudget(const FJumpSequenceFacts& Facts, int32_t MaxPrefillChunkBudget)
	{
		const int32_t Pending = Facts.bPendingToken ? 1 : 0;
		const int32_t Forced = std::max(0, std::min(Facts.ForcedChainLength, Facts.RemainingAllowance));
		const int32_t Budget = std::min(Pending + Forced, MaxPrefillChunkBudget);
		return Budget - Pending > 0 ? Budget : 0;
	}

	// --- A span's result on the tracked walk (rev 1.3.6, §6 P1-X "The walk"; P1X.12 (f); XD10) ---

	FSpanWalkResult WalkSpanResult(const ISchemaAutomaton& Automaton, FForcednessObservations& Observations, uint32_t State,
		const int32_t* Confirmed, size_t Count, const int32_t* StopTokens, size_t StopCount, bool bTailRead, bool bTailForced)
	{
		FSpanWalkResult Out;
		Out.State = State;
		// The output's cut: through the first stop token, or every confirmed id.
		Out.AppendCount = Count;
		for (size_t I = 0; I < Count && Confirmed != nullptr; ++I)
		{
			bool bStop = false;
			for (size_t K = 0; K < StopCount && StopTokens != nullptr; ++K)
			{
				bStop = bStop || StopTokens[K] == Confirmed[I];
			}
			if (bStop)
			{
				Out.AppendCount = I + 1;
				break;
			}
		}
		// The walk goes through every confirmed id before the cut matters: the engine committed them all.
		uint32_t Current = State;
		for (size_t I = 0; I < Count && Confirmed != nullptr; ++I)
		{
			uint32_t Next = 0;
			if (!Automaton.Step(Current, Confirmed[I], Next))
			{
				return Out; // the walk cannot reach the engine's state: unknown, and the read is dropped
			}
			Observations.RecordDraftedThrough(Current);
			Current = Next;
		}
		Out.bWalkKnown = true;
		Out.State = Current;
		if (bTailRead)
		{
			Observations.RecordRead(Current, bTailForced);
			Out.bReadRecorded = true;
		}
		return Out;
	}
}
