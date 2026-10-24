// The learned draft, D2-online (plan rev 1.3.5, §5.10.5, §6 P1-X "The learned draft"): the count
// table, the gate at a share product of 1/2, the stop at the first state the table does not hold
// (it holds only states observed forced, so this is also the unforced stop), the clip to the span
// budget, and the bound on the table. The observed-forcedness, cap and tie cells are in
// test_learning.cpp (P1X.12).
//
// The expected drafts are computed here from the plan's rule and the fixture's counts, never from
// the code under test.

#include "fixture.h"
#include "harness.h"

#include <cstdint>
#include <vector>

using namespace SuperSLMCore;
using namespace SchemaJumpFixture;

namespace
{
	FDraftTableRef Learn(const ISchemaAutomaton& A, const std::vector<std::vector<int32_t>>& Streams)
	{
		FDraftTableRef T;
		for (const std::vector<int32_t>& S : Streams)
		{
			T = MergeFinishedStream(T.get(), A, S.data(), S.size());
		}
		return T;
	}

	std::vector<int32_t> Prefix(const std::vector<int32_t>& V, size_t N)
	{
		return std::vector<int32_t>(V.begin(), V.begin() + static_cast<std::ptrdiff_t>(N));
	}
}

HOST_TEST(merge_counts_tokens_at_forced_states_only, "§6 P1-X table; Lifetime bound (forced states only)")
{
	const FFakeAutomaton A = OrderAutomaton();
	const std::vector<int32_t> S = OrderStreamA(); // 100@S0 102@S1 103@S2 200@S3 104@S4
	const FDraftTableRef T = MergeFinishedStream(nullptr, A, S.data(), S.size());
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->CountOf(S0, 100), 1u);
	EXPECT_EQ(T->CountOf(S1, 102), 1u);
	EXPECT_EQ(T->CountOf(S2, 103), 1u);
	EXPECT_EQ(T->CountOf(S4, 104), 1u);
	// S3 is a choice state: the model's choice there is never learned.
	EXPECT_EQ(T->CountOf(S3, 200), 0u);
	EXPECT_EQ(T->TokenCountAt(S3), 0u);
	EXPECT_EQ(T->StateCount(), 4u);
}

HOST_TEST(merge_builds_a_new_table_and_leaves_its_base, "§6 P1-X Concurrency: immutable snapshot, merge into a new table")
{
	const FFakeAutomaton A = OrderAutomaton();
	const std::vector<int32_t> SA = OrderStreamA();
	const std::vector<int32_t> SB = OrderStreamB();
	const FDraftTableRef Base = MergeFinishedStream(nullptr, A, SA.data(), SA.size());
	REQUIRE_TRUE(Base != nullptr);
	const FDraftTableRef Next = MergeFinishedStream(Base.get(), A, SB.data(), SB.size());
	REQUIRE_TRUE(Next != nullptr);
	EXPECT_TRUE(Next.get() != Base.get());

	// The new table holds both streams.
	EXPECT_EQ(Next->CountOf(S0, 100), 1u);
	EXPECT_EQ(Next->CountOf(S0, 101), 1u);
	EXPECT_EQ(Next->CountOf(S2, 103), 2u);
	EXPECT_EQ(Next->CountOf(S5, 105), 1u);
	EXPECT_EQ(Next->TokenCountAt(S0), 2u);

	// The base is as it was.
	EXPECT_EQ(Base->CountOf(S0, 101), 0u);
	EXPECT_EQ(Base->CountOf(S2, 103), 1u);
	EXPECT_EQ(Base->TokenCountAt(S0), 1u);
	EXPECT_EQ(Base->StateCount(), 4u);
}

HOST_TEST(merge_stops_at_the_first_inadmissible_token, "§6 P1-X table (a stream the automaton does not admit)")
{
	const FFakeAutomaton A = OrderAutomaton();
	const std::vector<int32_t> S = {100, 999, 102, 103};
	const FDraftTableRef T = MergeFinishedStream(nullptr, A, S.data(), S.size());
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->CountOf(S0, 100), 1u);
	EXPECT_EQ(T->StateCount(), 1u);
	EXPECT_EQ(T->CountOf(S1, 102), 0u);
	EXPECT_EQ(T->CountOf(S2, 103), 0u);
}

HOST_TEST(draft_takes_the_most_frequent_token_until_a_choice, "§5.10.5 D2; §6 P1-X draft; a choice state is never held, so the draft stops there")
{
	const FFakeAutomaton A = OrderAutomaton();
	// 100 three times at S0, 101 once: share 3/4. S1 and S2 are certain.
	const FDraftTableRef T = Learn(A, {OrderStreamA(), OrderStreamA(), OrderStreamA(), OrderStreamB()});
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, S0, Wide());
	// 100 (3/4), 102 (1), 103 (1): product 3/4 >= 1/2; S3 is a choice, so the draft stops there.
	const std::vector<int32_t> Expected = {100, 102, 103};
	EXPECT_TRUE(D == Expected);
	EXPECT_EQ(D.size(), 3u);

	// From S4 (forced, seen, then the unforced accepting state): one token.
	const std::vector<int32_t> D4 = BuildDraft(T.get(), A, S4, Wide());
	EXPECT_TRUE(D4 == std::vector<int32_t>{104});
}

HOST_TEST(draft_gate_stops_when_the_share_product_falls_below_half, "§5.10.5 gating >= 0.5")
{
	const FFakeAutomaton A = ChainAutomaton(5);
	// Shares at the leading token: 9/10, 3/5, 9/10, 1, 1. Products: 0.9, 0.54, 0.486 -> two tokens.
	const FDraftTableRef T = MakeSeedTable(ChainSeed({{9, 1}, {3, 2}, {9, 1}, {5}, {5}}));
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	EXPECT_TRUE(D == Prefix(ChainStream(5), 2));
	EXPECT_EQ(D.size(), 2u);

	// From state 2 the product restarts: 9/10, 1, 1 -> three tokens, to the end of the chain.
	const std::vector<int32_t> D2 = BuildDraft(T.get(), A, 2, Wide());
	const std::vector<int32_t> Tail = {ChainToken(2, 0), ChainToken(3, 0), ChainToken(4, 0)};
	EXPECT_TRUE(D2 == Tail);
}

HOST_TEST(draft_gate_admits_a_product_of_exactly_half, "§5.10.5 gating: >= 0.5 is inclusive")
{
	const FFakeAutomaton A = ChainAutomaton(4);
	// Shares: 2/4 (token 0 leads, 1 and 1 behind), then 1, then 4/5. Products: 1/2, 1/2, 2/5.
	const FDraftTableRef T = MakeSeedTable(ChainSeed({{2, 1, 1}, {7}, {4, 1}, {3}}));
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	EXPECT_TRUE(D == Prefix(ChainStream(4), 2));
	EXPECT_EQ(D.size(), 2u);
}

HOST_TEST(draft_gate_is_the_exact_product_not_a_rounded_one, "§5.10.5 gating >= 0.5, at 6/11 x 11/12 = 1/2")
{
	// The rule's product is 1/2 exactly, which the gate admits. A running double product reads
	// (6.0 / 11) * (11.0 / 12) = 0.49999999999999994 and stops one token early. Then 1/2 x 1/2 stops.
	const FFakeAutomaton A = ChainAutomaton(3);
	const FDraftTableRef T = MakeSeedTable(ChainSeed({{6, 5}, {11, 1}, {1, 1, 0, 0}}));
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	EXPECT_EQ(D.size(), 2u);
	EXPECT_TRUE(D == Prefix(ChainStream(3), 2));
}

HOST_TEST(draft_stops_at_the_first_unseen_state, "§6 P1-X: stops at the first state the table does not hold")
{
	const FFakeAutomaton A = ChainAutomaton(4);
	// States 0, 1 and 3 seen; 2 unseen. The draft does not skip over 2.
	std::vector<FDraftSeedRow> Rows = ChainSeed({{5}, {5}});
	Rows.push_back({3, ChainToken(3, 0), 5});
	const FDraftTableRef T = MakeSeedTable(Rows);
	REQUIRE_TRUE(T != nullptr);
	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	EXPECT_TRUE(D == Prefix(ChainStream(4), 2));
}

HOST_TEST(draft_is_empty_at_an_unseen_or_unforced_start_or_without_a_table, "§6 P1-X draft; cold start")
{
	const FFakeAutomaton A = OrderAutomaton();
	const FDraftTableRef T = Learn(A, {OrderStreamA()});
	REQUIRE_TRUE(T != nullptr);
	// Positive clause: the same table drafts from S0.
	REQUIRE_TRUE(!BuildDraft(T.get(), A, S0, Wide()).empty());

	EXPECT_TRUE(BuildDraft(T.get(), A, S5, Wide()).empty()); // forced, unseen
	EXPECT_TRUE(BuildDraft(T.get(), A, S3, Wide()).empty()); // a choice state
	EXPECT_TRUE(BuildDraft(T.get(), A, S7, Wide()).empty()); // accepting
	EXPECT_TRUE(BuildDraft(nullptr, A, S0, Wide()).empty()); // no table yet
}

HOST_TEST(draft_is_clipped_to_the_chunk_budget, "§6 P1-X: clipped to the span budget (chunk_budget - 1)")
{
	const FFakeAutomaton A = ChainAutomaton(12);
	const FDraftTableRef T = Learn(A, {ChainStream(12)});
	REQUIRE_TRUE(T != nullptr);
	FDraftLimits L = Wide();
	REQUIRE_EQ(BuildDraft(T.get(), A, 0, L).size(), 12u); // the table alone allows the whole chain

	const int32_t Budgets[] = {1, 2, 5, 8, 12, 13};
	for (int32_t B : Budgets)
	{
		L.MaxPrefillChunkBudget = B;
		const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, L);
		const size_t Want = static_cast<size_t>(B - 1 < 12 ? B - 1 : 12);
		EXPECT_EQ(D.size(), Want);
		EXPECT_TRUE(D == Prefix(ChainStream(12), Want));
	}
}

HOST_TEST(draft_is_clipped_to_the_remaining_allowance, "§6 P1 MaxNewTokens; P1X.5 host half")
{
	const FFakeAutomaton A = ChainAutomaton(12);
	const FDraftTableRef T = Learn(A, {ChainStream(12)});
	REQUIRE_TRUE(T != nullptr);
	FDraftLimits L = Wide();
	REQUIRE_EQ(BuildDraft(T.get(), A, 0, L).size(), 12u);

	const int32_t Allowances[] = {0, 1, 3, 11, 12, 40};
	for (int32_t R : Allowances)
	{
		L.RemainingAllowance = R;
		const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, L);
		const size_t Want = static_cast<size_t>(R < 12 ? R : 12);
		EXPECT_EQ(D.size(), Want);
		// A clipped draft is a prefix of the unclipped one, so a truncated run stays a prefix.
		EXPECT_TRUE(D == Prefix(ChainStream(12), Want));
	}
}

HOST_TEST(draft_cap_is_six_only_under_effective_threaded_decode, "P1X.7 (host half); §5.10.6")
{
	const FFakeAutomaton A = ChainAutomaton(12);
	const FDraftTableRef T = Learn(A, {ChainStream(12)});
	REQUIRE_TRUE(T != nullptr);
	FDraftLimits L = Wide();

	L.bThreadedDecodeEffective = IsThreadedDecodeEffective(true, 4);
	EXPECT_EQ(BuildDraft(T.get(), A, 0, L).size(), 6u);

	// bParallelDecode on with FinishParallelTasks 1 is not threaded: no cap (rev 1.3.1).
	L.bThreadedDecodeEffective = IsThreadedDecodeEffective(true, 1);
	EXPECT_EQ(BuildDraft(T.get(), A, 0, L).size(), 12u);

	L.bThreadedDecodeEffective = IsThreadedDecodeEffective(false, 4);
	EXPECT_EQ(BuildDraft(T.get(), A, 0, L).size(), 12u);
}

HOST_TEST(max_draft_tokens_takes_the_smallest_limit, "§6 P1-X clip; P1X.5; P1X.7")
{
	FDraftLimits L;
	L.MaxPrefillChunkBudget = 64;
	L.RemainingAllowance = 64;
	EXPECT_EQ(MaxDraftTokens(L), 63);
	L.RemainingAllowance = 10;
	EXPECT_EQ(MaxDraftTokens(L), 10);
	L.bThreadedDecodeEffective = true;
	EXPECT_EQ(MaxDraftTokens(L), 6);
	L.RemainingAllowance = 2;
	EXPECT_EQ(MaxDraftTokens(L), 2);
	L.MaxPrefillChunkBudget = 1;
	EXPECT_EQ(MaxDraftTokens(L), 0);
	L.MaxPrefillChunkBudget = 64;
	L.RemainingAllowance = 0;
	EXPECT_EQ(MaxDraftTokens(L), 0);
	L.RemainingAllowance = -3;
	EXPECT_EQ(MaxDraftTokens(L), 0);
	L.RemainingAllowance = 9;
	L.MaxPrefillChunkBudget = 0;
	EXPECT_EQ(MaxDraftTokens(L), 0);
}

HOST_TEST(draft_from_a_corrupted_table_is_bounded_and_deterministic, "P1X.1 (host half: the corrupted-table arm cannot break the loop)")
{
	const FFakeAutomaton A = ChainAutomaton(6);
	// State 2's leading token is one the automaton does not admit there (a corrupted or stale row).
	std::vector<FDraftSeedRow> Rows = ChainSeed({{5}, {5}});
	Rows.push_back({2, 424242, 5});
	Rows.push_back({3, ChainToken(3, 0), 5});
	const FDraftTableRef T = MakeSeedTable(Rows);
	REQUIRE_TRUE(T != nullptr);
	REQUIRE_EQ(T->CountOf(2, 424242), 5u);

	const std::vector<int32_t> D = BuildDraft(T.get(), A, 0, Wide());
	// Nothing past the inadmissible token can be drafted: the draft is a prefix of
	// [t0, t1, 424242].
	const std::vector<int32_t> Bound = {ChainToken(0, 0), ChainToken(1, 0), 424242};
	REQUIRE_TRUE(D.size() >= 2u);
	EXPECT_TRUE(D.size() <= 3u);
	EXPECT_TRUE(D == Prefix(Bound, D.size()));
	EXPECT_TRUE(BuildDraft(T.get(), A, 0, Wide()) == D);
}

HOST_TEST(table_keeps_a_capped_token_list_and_the_leader, "§6 P1-X Lifetime: at most 4 tokens per state, the leader kept (rev 1.3.6: under Space-Saving eviction too); P1X.11 host half")
{
	FFakeAutomaton A;
	A.Start = 0;
	A.Forced = {0};
	for (int32_t Tok = 0; Tok < 600; ++Tok)
	{
		A.Edge(0, Tok, 1); // state 1: unforced end
	}
	const uint32_t Cap = FDraftTable::MaxTokensPerState();
	REQUIRE_EQ(Cap, 4u);

	// 600 distinct rare tokens, interleaved with token 7 twice as often as all of them together.
	FDraftTableRef T;
	for (int32_t Tok = 0; Tok < 600; ++Tok)
	{
		const int32_t Rare[] = {Tok};
		T = MergeFinishedStream(T.get(), A, Rare, 1);
		const int32_t Lead[] = {7};
		T = MergeFinishedStream(T.get(), A, Lead, 1);
		T = MergeFinishedStream(T.get(), A, Lead, 1);
		REQUIRE_TRUE(T != nullptr);
		EXPECT_LE(T->TokenCountAt(0), static_cast<size_t>(Cap));
	}
	EXPECT_EQ(T->StateCount(), 1u);
	EXPECT_EQ(T->TotalAt(0), 1800u); // every token counted, kept or not
	// The leader is still the draft (D2 is the state's most frequent token).
	EXPECT_TRUE(BuildDraft(T.get(), A, 0, Wide()) == std::vector<int32_t>{7});
}
