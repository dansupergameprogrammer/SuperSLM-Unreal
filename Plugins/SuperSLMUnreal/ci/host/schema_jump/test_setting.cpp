// The three-way setting (plan §6 P1, P1-X; the rulings of 2026-10-20 16:23Z and 16:24Z)
// and the threaded-decode predicate that caps exact-mode drafts (§5.10.6, G24, cell P1X.7).

#include "fixture.h"
#include "harness.h"

using namespace SuperSLMCore;

namespace
{
	// A bound sequence whose next job, under jump-forward, would be a span: a pending token at a
	// forced state. Only the mode decides whether a span is taken.
	FJumpSequenceFacts PendingAtForcedState()
	{
		FJumpSequenceFacts F;
		F.bSchemaBound = true;
		F.bPendingToken = true;
		F.ForcedChainLength = 3;
		F.RemainingAllowance = 32;
		F.DraftLength = 3; // a learned draft exists, so exact mode would span too
		return F;
	}
}

HOST_TEST(setting_default_is_off_and_takes_no_span, "§6 P1-X setting; 16:23Z default Off")
{
	EXPECT_EQ(ESchemaJumpMode{}, ESchemaJumpMode::Off);
	EXPECT_EQ(static_cast<int>(ESchemaJumpMode::Off), 0);

	// A default-constructed fact set is Off, and Off decodes where a mode would take a span.
	FJumpSequenceFacts F = PendingAtForcedState();
	EXPECT_EQ(FJumpSequenceFacts{}.Mode, ESchemaJumpMode::Off);
	EXPECT_EQ(F.Mode, ESchemaJumpMode::Off);
	EXPECT_EQ(NextJob(F), EJumpNextJob::Decode);

	// The same facts with the option set take a span, so the default is what keeps it off.
	F.Mode = ESchemaJumpMode::Exact;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Span);
	F.Mode = ESchemaJumpMode::Canonical;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Span);
}

HOST_TEST(setting_three_values_are_distinct, "§6 P1-X setting")
{
	EXPECT_NE(ESchemaJumpMode::Off, ESchemaJumpMode::Exact);
	EXPECT_NE(ESchemaJumpMode::Off, ESchemaJumpMode::Canonical);
	EXPECT_NE(ESchemaJumpMode::Exact, ESchemaJumpMode::Canonical);

	// Each value reaches the loop as itself: after the prompt (no pending token, a forced chain from
	// the start state), canonical spans and exact decodes (§5.10.3 precondition).
	FJumpSequenceFacts F;
	F.bSchemaBound = true;
	F.ForcedChainLength = 3;
	F.RemainingAllowance = 32;
	F.Mode = ESchemaJumpMode::Canonical;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Span);
	F.Mode = ESchemaJumpMode::Exact;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Decode);
	F.Mode = ESchemaJumpMode::Off;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Decode);
}

HOST_TEST(switch_parses_off_exact_canonical, "P1.9; §6 P1 (0|1) and P1-X (off|exact|canonical, any case); mutant XD7")
{
	struct FCase
	{
		const char* Text;
		ESchemaJumpMode Want;
	};
	const FCase Cases[] = {
		{"0", ESchemaJumpMode::Off}, {"1", ESchemaJumpMode::Canonical},
		{"off", ESchemaJumpMode::Off}, {"exact", ESchemaJumpMode::Exact}, {"canonical", ESchemaJumpMode::Canonical},
		{"OFF", ESchemaJumpMode::Off}, {"Exact", ESchemaJumpMode::Exact}, {"CaNoNiCaL", ESchemaJumpMode::Canonical},
		{"eXACT", ESchemaJumpMode::Exact}, {"Off", ESchemaJumpMode::Off},
	};
	for (const FCase& C : Cases)
	{
		// Start from a mode the case does not want, so a parser that leaves it cannot pass.
		ESchemaJumpMode M = C.Want == ESchemaJumpMode::Exact ? ESchemaJumpMode::Canonical : ESchemaJumpMode::Exact;
		EXPECT_TRUE(ParseSchemaJumpSwitch(C.Text, M));
		EXPECT_EQ(M, C.Want);
	}
}

HOST_TEST(switch_refuses_other_values_and_leaves_the_mode, "P1.9; §6 P1-X: any other value refused, mode unchanged; mutant XD7")
{
	// Positive clause first, so an always-false parser cannot pass this cell.
	ESchemaJumpMode M = ESchemaJumpMode::Off;
	REQUIRE_TRUE(ParseSchemaJumpSwitch("exact", M));

	const char* const Others[] = {"", " ", "on", "true", "2", "-1", "01", "00", "1 ", " 0", "exactly", "canonical ",
		" off", "offf", "ex act", "o", "exac", "canon", "\toff", "off\n"};
	const ESchemaJumpMode Starts[] = {ESchemaJumpMode::Off, ESchemaJumpMode::Exact, ESchemaJumpMode::Canonical};
	for (const char* Text : Others)
	{
		for (ESchemaJumpMode Start : Starts)
		{
			M = Start;
			EXPECT_TRUE(!ParseSchemaJumpSwitch(Text, M));
			EXPECT_EQ(M, Start);
		}
	}
	M = ESchemaJumpMode::Canonical;
	EXPECT_TRUE(!ParseSchemaJumpSwitch(nullptr, M));
	EXPECT_EQ(M, ESchemaJumpMode::Canonical);
}

HOST_TEST(threaded_decode_is_effective_only_with_more_than_one_task, "G24; §5.10.6; P1X.7 predicate")
{
	EXPECT_TRUE(IsThreadedDecodeEffective(true, 2));
	EXPECT_TRUE(IsThreadedDecodeEffective(true, 4));
	EXPECT_TRUE(IsThreadedDecodeEffective(true, 256));
	EXPECT_TRUE(!IsThreadedDecodeEffective(true, 1));
	EXPECT_TRUE(!IsThreadedDecodeEffective(true, 0));
	EXPECT_TRUE(!IsThreadedDecodeEffective(false, 4));
	EXPECT_TRUE(!IsThreadedDecodeEffective(false, 1));
}

HOST_TEST(threaded_draft_cap_is_six, "§5.10.6 the 6-token cap; P1X.7")
{
	EXPECT_EQ(kThreadedExactDraftCap, 6);
	FDraftLimits L = SchemaJumpFixture::Wide();
	L.bThreadedDecodeEffective = true;
	EXPECT_EQ(MaxDraftTokens(L), 6);
	L.bThreadedDecodeEffective = false;
	EXPECT_EQ(MaxDraftTokens(L), 63);
}
