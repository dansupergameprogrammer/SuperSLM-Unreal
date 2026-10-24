// Plan rev 1.3.6 (§17.12; the P1/P1-X code review, round 1): the host core cells the review's findings
// need, each red against the mutant or the failing input named.
//  - P1X.12 (f), review finding 5, mutant XD10: a stop token inside a confirmed draft. The span's read
//    is recorded at the state after the last confirmed id, or dropped; never at an earlier state.
//  - Review finding 6 (a code defect against text the plan already has): the shipped-table parser's
//    range check does not wrap for a single digit above a small maximum.
// P1X.12 (c)'s Space-Saving cells are in test_learning.cpp; P1.1's arm fit is in test_pricing_arm.cpp.
//
// Every expected value is computed here, from the plan's rule, the fixture automata or the B7 files
// read by fixture.h; never from the code under test.

#include "fixture.h"
#include "harness.h"

#include <cstring>
#include <string>

using namespace SuperSLMCore;
using namespace SchemaJumpFixture;

namespace
{
	bool Parses(const char* Text, FDraftTableRef& Out, std::vector<uint32_t>& Forced)
	{
		Out = nullptr;
		Forced.clear();
		return ParseShippedDraftTable(Text, std::strlen(Text), Out, &Forced);
	}
}

HOST_TEST(p1x12f_a_stop_token_inside_a_draft_walks_every_confirmed_id_first, "P1X.12 (f) rev 1.3.6; §6 P1-X The walk; mutant XD10")
{
	// A forced chain G0 -> G1 -> G2 -> G3 (G3 the unforced end). The span confirmed all three chain
	// tokens; the first confirmed id after the pending one is a stop token. The engine read after
	// all three, at G3, and found no chain.
	const FFakeAutomaton A = ChainAutomaton(3);
	FForcednessObservations Obs;
	Obs.RecordRead(0, true);
	const int32_t Confirmed[] = {ChainToken(0, 0), ChainToken(1, 0), ChainToken(2, 0)};
	const int32_t Stop[] = {ChainToken(1, 0)};
	const FSpanWalkResult R = WalkSpanResult(A, Obs, 0, Confirmed, 3, Stop, 1, /*bTailRead*/ true, /*bTailForced*/ false);
	EXPECT_TRUE(R.bWalkKnown);
	EXPECT_EQ(R.State, 3u);          // the walk went through every confirmed id
	EXPECT_EQ(R.AppendCount, 2u);    // the output stops at the stop token, inclusive
	EXPECT_TRUE(R.bReadRecorded);
	// The states inside the draft were drafted through: forced, and the read did not land there.
	EXPECT_EQ(Obs.Get(0), EForcedObservation::Forced);
	EXPECT_EQ(Obs.Get(1), EForcedObservation::Forced);
	EXPECT_EQ(Obs.Get(2), EForcedObservation::Forced);
	// The read is the engine's, at the state after the last confirmed id.
	EXPECT_EQ(Obs.Get(3), EForcedObservation::Unforced);
}

HOST_TEST(p1x12f_no_stop_token_records_the_read_at_the_last_state, "P1X.12 (f) rev 1.3.6; §6 P1-X The walk")
{
	const FFakeAutomaton A = ChainAutomaton(3);
	FForcednessObservations Obs;
	const int32_t Confirmed[] = {ChainToken(1, 2), ChainToken(2, 3)};
	const FSpanWalkResult R = WalkSpanResult(A, Obs, 1, Confirmed, 2, nullptr, 0, true, false);
	EXPECT_TRUE(R.bWalkKnown);
	EXPECT_EQ(R.State, 3u);
	EXPECT_EQ(R.AppendCount, 2u);
	EXPECT_TRUE(R.bReadRecorded);
	EXPECT_EQ(Obs.Get(1), EForcedObservation::Forced);
	EXPECT_EQ(Obs.Get(2), EForcedObservation::Forced);
	EXPECT_EQ(Obs.Get(3), EForcedObservation::Unforced);

	// No read taken: nothing recorded at the end state.
	FForcednessObservations Quiet;
	const FSpanWalkResult Q = WalkSpanResult(A, Quiet, 1, Confirmed, 2, nullptr, 0, false, false);
	EXPECT_TRUE(Q.bWalkKnown);
	EXPECT_EQ(Q.State, 3u);
	EXPECT_TRUE(!Q.bReadRecorded);
	EXPECT_EQ(Quiet.Get(3), EForcedObservation::Unread);
}

HOST_TEST(p1x12f_an_id_the_walk_does_not_admit_drops_the_read, "P1X.12 (f) rev 1.3.6: the read is dropped when the walk cannot reach the engine's state")
{
	const FFakeAutomaton A = ChainAutomaton(3);
	FForcednessObservations Obs;
	const int32_t Confirmed[] = {ChainToken(0, 0), 9999}; // 9999 is not legal at G1
	const FSpanWalkResult R = WalkSpanResult(A, Obs, 0, Confirmed, 2, nullptr, 0, true, false);
	EXPECT_TRUE(!R.bWalkKnown);
	EXPECT_TRUE(!R.bReadRecorded);
	// The admitted id before it was walked: G0 was drafted through.
	EXPECT_EQ(Obs.Get(0), EForcedObservation::Forced);
	// The unforced read lands nowhere: no state is marked Unforced.
	for (uint32_t S = 0; S <= 3; ++S)
	{
		EXPECT_NE(Obs.Get(S), EForcedObservation::Unforced);
	}
}

HOST_TEST(p1x12f_b7_stop_tokens_never_mark_a_forced_state_unforced, "P1X.12 (f) rev 1.3.6 on B7 (the review's stop_probe: 4 of 4 flipped); mutant XD10")
{
	const FB7& B = LoadB7();
	REQUIRE_TRUE(B.bLoaded);
	size_t Spans = 0;
	for (const FB7Draft& D : B.B2Drafts)
	{
		if (D.Confirmed < 2)
		{
			continue;
		}
		++Spans;
		// The observations before the span: every state visited so far read with the library flag.
		FForcednessObservations Obs;
		uint32_t St = 0;
		for (size_t I = 0; I < D.Position; ++I)
		{
			Obs.RecordRead(St, B.Forced.count(St) != 0);
			uint32_t N = 0;
			REQUIRE_TRUE(B.Walked.Step(St, B.B2Stream[I], N));
			St = N;
		}
		Obs.RecordRead(St, B.Forced.count(St) != 0);
		// The engine's state after every confirmed id, and what its read reports there.
		uint32_t Engine = St;
		std::vector<uint32_t> Inside;
		for (size_t K = 0; K < D.Confirmed; ++K)
		{
			Inside.push_back(Engine);
			uint32_t N = 0;
			REQUIRE_TRUE(B.Walked.Step(Engine, D.Draft[K], N));
			Engine = N;
		}
		const bool bTailForced = B.Forced.count(Engine) != 0;
		// The first confirmed id is a stop token (the review's input).
		const int32_t Stop[] = {D.Draft[0]};
		const FSpanWalkResult R = WalkSpanResult(B.Walked, Obs, St, D.Draft.data(), D.Confirmed, Stop, 1, true, bTailForced);
		EXPECT_TRUE(R.bWalkKnown);
		EXPECT_EQ(R.State, Engine);
		EXPECT_EQ(R.AppendCount, 1u);
		// Every state inside the confirmed draft is a library-forced state, and stays observed Forced.
		for (const uint32_t S : Inside)
		{
			EXPECT_TRUE(B.Forced.count(S) != 0);
			EXPECT_EQ(Obs.Get(S), EForcedObservation::Forced);
		}
		// The read, if recorded, is at the engine's state and agrees with the library there.
		if (R.bReadRecorded)
		{
			EXPECT_EQ(Obs.Get(Engine), bTailForced ? EForcedObservation::Forced : EForcedObservation::Unforced);
		}
	}
	EXPECT_EQ(Spans, 4u); // B2's spans with two or more confirmed ids (positions 3, 9, 15 and 21)
}

HOST_TEST(shipped_table_range_check_does_not_wrap_for_a_single_digit, "Review finding 6 (ParseUnsigned's guard wraps when Max < 9); §16 Q20 shipped seed")
{
	FDraftTableRef T;
	std::vector<uint32_t> F;
	// The review's inputs: a single digit above a maximum below 9 was accepted.
	EXPECT_TRUE(!Parses("SFD-DRAFT-TABLE/1\nstates 3\nforced 0 9\nS 9 5:2\n", T, F));   // state 9 of 3
	EXPECT_TRUE(T == nullptr);
	EXPECT_TRUE(!Parses("SFD-DRAFT-TABLE/1\nstates 1\nforced 7\nS 7 5:2\n", T, F));     // state 7 of 1
	EXPECT_TRUE(!Parses("SFD-DRAFT-TABLE/1\nstates 9\nforced 9\nS 9 5:2\n", T, F));     // state 9 of 9 (max 8)
	EXPECT_TRUE(!Parses("SFD-DRAFT-TABLE/1\nstates 3\nforced 0\nS 0 5:2\nS 9 6:1\n", T, F)); // a row's state 9 of 3
	// Two digits already took the correct branch; keep it so.
	EXPECT_TRUE(!Parses("SFD-DRAFT-TABLE/1\nstates 10\nforced 10\n", T, F));
	// Controls: the largest legal state is accepted, single digit and two digits.
	REQUIRE_TRUE(Parses("SFD-DRAFT-TABLE/1\nstates 10\nforced 9\nS 9 5:2\n", T, F));
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->CountOf(9, 5), 2u);
	REQUIRE_TRUE(Parses("SFD-DRAFT-TABLE/1\nstates 3\nforced 2\nS 2 5:2\n", T, F));
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->CountOf(2, 5), 2u);
}

HOST_TEST(p1x12c_a_seed_over_the_cap_keeps_the_four_highest_counts, "P1X.12 (c) rev 1.3.6 seed leg (the plan's ruling): the 4 highest counts, ties to the lowest id, error 0, dropped counts in the total; mutant: first 4 in file order, or the total without the dropped counts")
{
	// Six tokens at forced state 0, in an order where the first four in file order are not the
	// four highest: 15:1, 14:3, 13:3, 10:5, 12:3, 11:3. Highest first, lowest id on a tie: 10 (5),
	// then 11, 12, 13 (3 each); 14 (3, the highest id of the tie) and 15 (1) are dropped.
	FDraftTableRef T;
	std::vector<uint32_t> F;
	REQUIRE_TRUE(Parses("SFD-DRAFT-TABLE/1\nstates 2\nforced 0\nS 0 15:1 14:3 13:3 10:5 12:3 11:3\n", T, F));
	REQUIRE_TRUE(T != nullptr);
	EXPECT_EQ(T->TokenCountAt(0), 4u);
	EXPECT_EQ(T->CountOf(0, 10), 5u);
	EXPECT_EQ(T->CountOf(0, 11), 3u);
	EXPECT_EQ(T->CountOf(0, 12), 3u);
	EXPECT_EQ(T->CountOf(0, 13), 3u);
	EXPECT_EQ(T->CountOf(0, 14), 0u);
	EXPECT_EQ(T->CountOf(0, 15), 0u);
	for (const int32_t K : {10, 11, 12, 13})
	{
		EXPECT_EQ(T->ErrorOf(0, K), 0u);
		EXPECT_EQ(T->GuaranteedCountOf(0, K), T->CountOf(0, K));
	}
	EXPECT_EQ(T->TotalAt(0), 18u); // 1 + 3 + 3 + 5 + 3 + 3: the dropped counts stay in the total

	// The same rule through MakeSeedTable directly.
	const std::vector<FDraftSeedRow> Rows = {{0, 15, 1}, {0, 14, 3}, {0, 13, 3}, {0, 10, 5}, {0, 12, 3}, {0, 11, 3}};
	const FDraftTableRef M = MakeSeedTable(Rows);
	REQUIRE_TRUE(M != nullptr);
	EXPECT_EQ(M->TokenCountAt(0), 4u);
	EXPECT_EQ(M->CountOf(0, 14), 0u);
	EXPECT_EQ(M->CountOf(0, 11), 3u);
	EXPECT_EQ(M->TotalAt(0), 18u);
}
