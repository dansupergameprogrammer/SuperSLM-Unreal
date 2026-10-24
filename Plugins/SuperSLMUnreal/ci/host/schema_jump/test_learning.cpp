// What the learned table learns (plan rev 1.3.5 and rev 1.3.6, §6 P1-X "The learned draft", §17.11; cell P1X.12
// (a) to (d), host core cells, each red against the mutant named): observed forcedness, the B7
// table rebuilt from B4a's streams, the shipped-table form, the per-state cap of 4 and the tie rule.
// Leg (b), feeding, is in test_store.cpp; leg (e), the reads change nothing, is an Unreal cell
// (SuperSLM.SchemaJump.P1X.ForcednessReadsChangeNothing).
//
// Every expected value is computed here, from the plan's rule, the fixture automata, or the B7
// files read by fixture.h; never from the code under test.

#include "fixture.h"
#include "harness.h"

#include <cstdint>
#include <vector>

using namespace SuperSLMCore;
using namespace SchemaJumpFixture;

namespace
{
	// Every state on the order schema forced, so only the observations can keep a state out.
	FFakeAutomaton OrderTransitionsAllForced()
	{
		FFakeAutomaton A = OrderAutomaton();
		A.Forced = {S0, S1, S2, S3, S4, S5, S6, S7};
		return A;
	}

	FDraftTableRef LearnAll(const ISchemaAutomaton& A, const std::vector<std::vector<int32_t>>& Streams)
	{
		FDraftTableRef T;
		for (const std::vector<int32_t>& S : Streams)
		{
			T = MergeFinishedStream(T.get(), A, S.data(), S.size());
		}
		return T;
	}

	// The library's flags as reads: every state read once, forced or not.
	void ReadEveryState(FForcednessObservations& O, const FB7& B)
	{
		for (uint32_t S = 0; S < B.StateCount; ++S)
		{
			O.RecordRead(S, B.Forced.count(S) != 0);
		}
	}
}

HOST_TEST(observations_record_reads_and_drafts, "rev 1.3.5 §6 P1-X Forcedness, observed (sslm_seq_forced_tokens, cap 0)")
{
	FForcednessObservations O;
	EXPECT_EQ(O.Get(3), EForcedObservation::Unread);
	O.RecordRead(3, true);
	EXPECT_EQ(O.Get(3), EForcedObservation::Forced);
	O.RecordRead(4, false);
	EXPECT_EQ(O.Get(4), EForcedObservation::Unforced);
	O.RecordDraftedThrough(5);
	EXPECT_EQ(O.Get(5), EForcedObservation::Forced);
	EXPECT_EQ(O.Get(6), EForcedObservation::Unread);

	// The observed automaton: SCM1's transitions, the observed flags, never the transitions' own.
	const FFakeAutomaton T = OrderTransitionsAllForced();
	const FObservedSchemaAutomaton A(T, O);
	EXPECT_EQ(A.StartState(), S0);
	uint32_t Next = 99;
	EXPECT_TRUE(A.Step(S0, 100, Next));
	EXPECT_EQ(Next, S1);
	EXPECT_TRUE(!A.Step(S0, 999, Next));
	EXPECT_TRUE(A.IsForced(3));  // read forced
	EXPECT_TRUE(!A.IsForced(4)); // read unforced
	EXPECT_TRUE(A.IsForced(5));  // drafted through
	EXPECT_TRUE(!A.IsForced(6)); // never read, although the transitions call it forced
}

HOST_TEST(p1x12a_table_never_holds_a_state_read_unforced_or_never_read, "P1X.12 (a); mutant XD1")
{
	const FFakeAutomaton T = OrderTransitionsAllForced();
	FForcednessObservations O;
	O.RecordRead(S0, true);
	O.RecordRead(S1, true);
	O.RecordRead(S2, true);
	O.RecordRead(S3, false); // the choice state, read unforced
	// S4 is never read.
	const FObservedSchemaAutomaton A(T, O);
	const std::vector<int32_t> Stream = OrderStreamA(); // 100@S0 102@S1 103@S2 200@S3 104@S4
	const FDraftTableRef First = MergeFinishedStream(nullptr, A, Stream.data(), Stream.size());
	REQUIRE_TRUE(First != nullptr);
	EXPECT_EQ(First->CountOf(S0, 100), 1u);
	EXPECT_EQ(First->CountOf(S1, 102), 1u);
	EXPECT_EQ(First->CountOf(S2, 103), 1u);
	EXPECT_EQ(First->TokenCountAt(S3), 0u); // read unforced
	EXPECT_EQ(First->TokenCountAt(S4), 0u); // never read
	EXPECT_EQ(First->StateCount(), 3u);

	// S4 later lies inside a confirmed draft: now it is known forced and is learned.
	O.RecordDraftedThrough(S4);
	const FDraftTableRef Second = MergeFinishedStream(First.get(), A, Stream.data(), Stream.size());
	REQUIRE_TRUE(Second != nullptr);
	EXPECT_EQ(Second->CountOf(S4, 104), 1u);
	EXPECT_EQ(Second->TokenCountAt(S3), 0u);
	EXPECT_EQ(Second->StateCount(), 4u);
}

HOST_TEST(p1x12a_no_draft_runs_through_an_unforced_or_unread_state, "P1X.12 (a): the two stops are one; mutant XD1")
{
	// A chain whose state 2 was read unforced and whose state 4 was never read: the draft from 0
	// stops at 2, and the draft from 3 stops at 4, though the streams pass through both.
	const FFakeAutomaton T = ChainAutomaton(6);
	FForcednessObservations O;
	for (uint32_t S : {0u, 1u, 3u, 5u})
	{
		O.RecordRead(S, true);
	}
	O.RecordRead(2, false);
	const FObservedSchemaAutomaton A(T, O);
	const FDraftTableRef Table = LearnAll(A, {ChainStream(6), ChainStream(6), ChainStream(6)});
	REQUIRE_TRUE(Table != nullptr);
	EXPECT_TRUE(BuildDraft(Table.get(), A, 0, Wide()) == (std::vector<int32_t>{ChainToken(0, 0), ChainToken(1, 0)}));
	EXPECT_TRUE(BuildDraft(Table.get(), A, 3, Wide()) == (std::vector<int32_t>{ChainToken(3, 0)}));
	EXPECT_TRUE(BuildDraft(Table.get(), A, 2, Wide()).empty());
	EXPECT_TRUE(BuildDraft(Table.get(), A, 4, Wide()).empty());
}

HOST_TEST(p1x12a_b4a_streams_rebuild_b7s_table_at_its_forced_states, "P1X.12 (a): B4a's 20 off streams with the library's flags equal d2-table.txt (sha256 4cc216d2...)")
{
	const FB7& B = LoadB7();
	REQUIRE_TRUE(B.bLoaded);
	REQUIRE_EQ(B.D2Text.size(), 1502u); // d2-table.txt's length; its sha256 is in README.md and fixtures/.gitattributes keeps its bytes
	REQUIRE_EQ(B.StateCount, 148u);
	REQUIRE_EQ(B.Forced.size(), 142u);

	FForcednessObservations O;
	ReadEveryState(O, B);
	const FObservedSchemaAutomaton A(B.Walked, O);
	const FDraftTableRef T = LearnAll(A, B.B4aStreams);
	REQUIRE_TRUE(T != nullptr);

	size_t ForcedRows = 0;
	for (const auto& Row : B.Rows)
	{
		const uint32_t S = Row.first;
		if (B.Forced.count(S) == 0)
		{
			// An unforced state of B7's table (a choice state: 11, 48, 105, 138): never held.
			EXPECT_EQ(T->TokenCountAt(S), 0u);
			EXPECT_EQ(T->TotalAt(S), 0u);
			continue;
		}
		++ForcedRows;
		REQUIRE_TRUE(Row.second.size() <= 4u); // B7: no forced state over 4 tokens
		EXPECT_EQ(T->TokenCountAt(S), Row.second.size());
		uint64_t Total = 0;
		for (const auto& Tc : Row.second)
		{
			EXPECT_EQ(T->CountOf(S, Tc.first), Tc.second);
			Total += Tc.second;
		}
		EXPECT_EQ(T->TotalAt(S), Total);
	}
	EXPECT_EQ(T->StateCount(), ForcedRows);
	EXPECT_EQ(ForcedRows, 60u); // 64 states seen, 4 of them unforced
}

HOST_TEST(p1x12a_drafts_on_b2s_prompt_equal_the_benchs, "P1X.12 (a): the drafts on B2's prompt equal the bench's")
{
	const FB7& B = LoadB7();
	REQUIRE_TRUE(B.bLoaded);
	FForcednessObservations O;
	ReadEveryState(O, B);
	const FObservedSchemaAutomaton A(B.Walked, O);
	const FDraftTableRef T = LearnAll(A, B.B4aStreams);
	REQUIRE_TRUE(T != nullptr);
	REQUIRE_EQ(B.B2Drafts.size(), 7u);
	size_t Drafted = 0;
	for (const FB7Draft& D : B.B2Drafts)
	{
		FDraftLimits L;
		L.MaxPrefillChunkBudget = 64;                               // the bench's budget
		L.RemainingAllowance = 64 - static_cast<int32_t>(D.Position); // max_new 64, the pending token produced
		REQUIRE_EQ(MaxDraftTokens(L), D.Cap);
		EXPECT_TRUE(BuildDraft(T.get(), A, D.State, L) == D.Draft);
		Drafted += D.Draft.size();
	}
	EXPECT_EQ(Drafted, 21u);
	// The draft through state 119 stops at 138, a choice state whose top share (13/20) passes the
	// gate: only the forced flag stops it there (§17.11's reason to keep the unforced stop).
	EXPECT_TRUE(BuildDraft(T.get(), A, 119, Wide()) == (std::vector<int32_t>{2198, 79, 14225, 665, 788}));
}

HOST_TEST(shipped_table_loads_only_its_forced_states, "rev 1.3.5 §6 P1-X: a shipped seed carries the library's flags; only its forced states are loaded")
{
	const FB7& B = LoadB7();
	REQUIRE_TRUE(B.bLoaded);
	FDraftTableRef Shipped;
	std::vector<uint32_t> Forced;
	REQUIRE_TRUE(ParseShippedDraftTable(B.D2Text.data(), B.D2Text.size(), Shipped, &Forced));
	REQUIRE_TRUE(Shipped != nullptr);
	EXPECT_EQ(Forced.size(), B.Forced.size());
	EXPECT_TRUE(std::set<uint32_t>(Forced.begin(), Forced.end()) == B.Forced);
	size_t ForcedRows = 0;
	for (const auto& Row : B.Rows)
	{
		if (B.Forced.count(Row.first) == 0)
		{
			EXPECT_EQ(Shipped->TokenCountAt(Row.first), 0u);
			continue;
		}
		++ForcedRows;
		for (const auto& Tc : Row.second)
		{
			EXPECT_EQ(Shipped->CountOf(Row.first, Tc.first), Tc.second);
		}
	}
	EXPECT_EQ(Shipped->StateCount(), ForcedRows);

	// Not the form: refused.
	const char Bad[] = "SFD-DRAFT-TABLE/2 schema x\nstates 3\n";
	FDraftTableRef Out = Shipped;
	EXPECT_TRUE(!ParseShippedDraftTable(Bad, sizeof(Bad) - 1, Out, nullptr));
	EXPECT_TRUE(Out == nullptr);
}

HOST_TEST(p1x12c_a_newcomer_at_a_full_state_evicts_the_lowest_count_and_inherits_it, "P1X.12 (c) rev 1.3.6 (Space-Saving); mutant XD4 (rev 1.3.5's keep-the-first-four rule; a newcomer that takes no error)")
{
	EXPECT_EQ(FDraftTable::MaxTokensPerState(), 4u);
	const FFakeAutomaton A = ChainAutomaton(2);
	// State 0: k0 x3, k1, k2, k3 once each: total 6, share 3/6 = 1/2, so k0 is drafted.
	FDraftTableRef T = MakeSeedTable(ChainSeed({{3, 1, 1, 1}, {5}}));
	REQUIRE_TRUE(T != nullptr);
	REQUIRE_EQ(T->TokenCountAt(0), 4u);
	REQUIRE_EQ(T->TotalAt(0), 6u);
	REQUIRE_EQ(BuildDraft(T.get(), A, 0, Wide()).size(), 2u);
	EXPECT_EQ(T->ErrorOf(0, ChainToken(0, 0)), 0u); // a seed row's token carries no error
	EXPECT_EQ(T->GuaranteedCountOf(0, ChainToken(0, 0)), 3u);

	// A fifth distinct token arrives at the full state (a stream that leaves the chain there). The
	// lowest count is 1, held by k1, k2 and k3; on that tie the highest id, k3, goes. The newcomer
	// takes 1 + 1 = 2 with error 1, so its guaranteed count is 1.
	FFakeAutomaton Wider = A;
	Wider.Edge(0, 4242, 1);
	const int32_t Fifth[] = {4242};
	const FDraftTableRef After = MergeFinishedStream(T.get(), Wider, Fifth, 1);
	REQUIRE_TRUE(After != nullptr);
	EXPECT_EQ(After->TokenCountAt(0), 4u);                       // still four kept
	EXPECT_EQ(After->CountOf(0, 4242), 2u);                      // the newcomer is kept, at the evicted count + 1
	EXPECT_EQ(After->ErrorOf(0, 4242), 1u);                      // the inherited part is its error
	EXPECT_EQ(After->GuaranteedCountOf(0, 4242), 1u);
	EXPECT_EQ(After->CountOf(0, ChainToken(0, 3)), 0u);          // k3 (the highest id of the tie) evicted
	EXPECT_EQ(After->CountOf(0, ChainToken(0, 1)), 1u);          // k1 and k2 kept
	EXPECT_EQ(After->CountOf(0, ChainToken(0, 2)), 1u);
	EXPECT_EQ(After->GuaranteedCountOf(0, ChainToken(0, 0)), 3u); // the leader untouched
	EXPECT_EQ(After->TotalAt(0), 7u);                            // every arrival is counted
	// The leader's share fell to 3/7 < 1/2: the draft from 0 is now empty.
	EXPECT_TRUE(BuildDraft(After.get(), Wider, 0, Wide()).empty());
}

HOST_TEST(p1x12c_the_real_token_is_never_locked_out, "P1X.12 (c) rev 1.3.6: the review's probe (four one-off tokens, then the real token 1,000 times); mutant XD4 (rev 1.3.5's rule locks it out)")
{
	FFakeAutomaton A = ChainAutomaton(1);
	A.Edge(0, 1004, 1); // a fifth legal token at G0
	FDraftTableRef T;
	for (const int32_t OneOff : {ChainToken(0, 1), ChainToken(0, 2), ChainToken(0, 3), 1004})
	{
		const int32_t S[] = {OneOff};
		T = MergeFinishedStream(T.get(), A, S, 1);
	}
	REQUIRE_TRUE(T != nullptr);
	REQUIRE_EQ(T->TokenCountAt(0), 4u);
	for (int I = 0; I < 1000; ++I)
	{
		const int32_t S[] = {ChainToken(0, 0)};
		T = MergeFinishedStream(T.get(), A, S, 1);
	}
	REQUIRE_TRUE(T != nullptr);
	// Its first arrival evicted the highest id of the four tied at 1 (1004) and inherited 1.
	EXPECT_EQ(T->CountOf(0, 1004), 0u);
	EXPECT_EQ(T->CountOf(0, ChainToken(0, 0)), 1001u);
	EXPECT_EQ(T->ErrorOf(0, ChainToken(0, 0)), 1u);
	EXPECT_EQ(T->GuaranteedCountOf(0, ChainToken(0, 0)), 1000u);
	EXPECT_EQ(T->TotalAt(0), 1004u);
	EXPECT_LE(T->TokenCountAt(0), 4u);
	// It is drafted: guaranteed share 1000 / 1004.
	EXPECT_TRUE(BuildDraft(T.get(), A, 0, Wide()) == std::vector<int32_t>{ChainToken(0, 0)});
}

HOST_TEST(p1x12c_the_share_is_the_guaranteed_count_over_the_total, "P1X.12 (c) rev 1.3.6: a newcomer's share is never above its guaranteed count over the total; mutant XD4 (a newcomer that takes no error)")
{
	FFakeAutomaton A = ChainAutomaton(1);
	A.Edge(0, 1004, 1);
	FDraftTableRef T;
	auto Arrive = [&](int32_t Token)
	{
		const int32_t S[] = {Token};
		T = MergeFinishedStream(T.get(), A, S, 1);
	};
	for (int32_t K = 0; K < 4; ++K) { Arrive(ChainToken(0, K)); }
	// 1004 arrives three times: the first evicts 1003 (the highest id tied at 1) and takes 2 with
	// error 1; then 3, then 4. Raw share 4/7 would pass the gate; the guaranteed share 3/7 does not.
	for (int I = 0; I < 3; ++I) { Arrive(1004); }
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->CountOf(0, 1004), 4u);
	EXPECT_EQ(T->ErrorOf(0, 1004), 1u);
	EXPECT_EQ(T->GuaranteedCountOf(0, 1004), 3u);
	EXPECT_EQ(T->TotalAt(0), 7u);
	EXPECT_TRUE(BuildDraft(T.get(), A, 0, Wide()).empty());
	// One more: guaranteed 4 of 8, exactly 1/2, which the gate admits.
	Arrive(1004);
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->GuaranteedCountOf(0, 1004), 4u);
	EXPECT_EQ(T->TotalAt(0), 8u);
	EXPECT_TRUE(BuildDraft(T.get(), A, 0, Wide()) == std::vector<int32_t>{1004});
}

HOST_TEST(p1x12d_a_tie_drafts_the_lowest_token_id, "P1X.12 (d): lowest id on a tie; mutant XD5 (the highest id)")
{
	const FFakeAutomaton A = ChainAutomaton(2);
	// The higher id is inserted first, so "first seen" and "highest id" both fail.
	const FDraftTableRef T = MakeSeedTable({{0, ChainToken(0, 3), 2}, {0, ChainToken(0, 1), 2}, {1, ChainToken(1, 0), 4}});
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	EXPECT_TRUE(D == (std::vector<int32_t>{ChainToken(0, 1), ChainToken(1, 0)}));

	// No tie: the leader, whatever its id.
	const FDraftTableRef U = MakeSeedTable({{0, ChainToken(0, 1), 2}, {0, ChainToken(0, 3), 3}});
	REQUIRE_TRUE(U != nullptr);
	const std::vector<int32_t> E = BuildDraft(U.get(), A, 0, Wide());
	REQUIRE_TRUE(!E.empty());
	EXPECT_EQ(E.front(), ChainToken(0, 3));
}
