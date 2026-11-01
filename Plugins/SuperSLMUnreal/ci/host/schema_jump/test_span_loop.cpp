// The span-after-decode loop's decisions (plan §6 P1 "The span job (drain rule, §5.6)", §6 P1-X "The
// loop", "Mixed modes", "Statuses"; §5.10.3's precondition; §5.4's pending token). Host halves of
// P1.2, P1.4, P1.6, P1.7, P1X.2, P1X.4, P1X.9 and P1X.10. The Unreal cells assert the same rules on
// real runs (SuperSLMSchemaJumpP1Tests.cpp, SuperSLMSchemaJumpP1XTests.cpp).

#include "harness.h"

#include "SuperSLMSchemaJumpCore.h"

using namespace SuperSLMCore;

namespace
{
	FJumpSequenceFacts Bound(ESchemaJumpMode Mode)
	{
		FJumpSequenceFacts F;
		F.Mode = Mode;
		F.bSchemaBound = true;
		F.RemainingAllowance = 64;
		return F;
	}

	// Right after the prompt: no pending token, the start state's chain ahead.
	FJumpSequenceFacts AfterPrompt(ESchemaJumpMode Mode, int32_t Chain)
	{
		FJumpSequenceFacts F = Bound(Mode);
		F.ForcedChainLength = Chain;
		return F;
	}

	// Right after a decode job produced a token: pending, the chain from the state after it. The
	// learned table holds that chain, so the exact draft is as long as the chain (rev 1.3.5: exact
	// spans only on a non-empty draft; AfterDecodeCold is the cold start).
	FJumpSequenceFacts AfterDecode(ESchemaJumpMode Mode, int32_t Chain)
	{
		FJumpSequenceFacts F = Bound(Mode);
		F.bPendingToken = true;
		F.ForcedChainLength = Chain;
		F.DraftLength = Chain;
		return F;
	}

	FJumpSequenceFacts AfterDecodeCold(ESchemaJumpMode Mode, int32_t Chain)
	{
		FJumpSequenceFacts F = AfterDecode(Mode, Chain);
		F.DraftLength = 0;
		return F;
	}

	// Right after a span job: the verb admitted rows, so the sequence is ready for logits.
	FJumpSequenceFacts AfterSpan(ESchemaJumpMode Mode, int32_t ChainLeft)
	{
		FJumpSequenceFacts F = Bound(Mode);
		F.bLastJobWasSpan = true;
		F.ForcedChainLength = ChainLeft;
		return F;
	}
}

HOST_TEST(loop_off_unbound_and_gpu_sequences_always_decode, "P1.6, P1X.4 (host half): the GPU ignores the setting; Off and unbound decode")
{
	const ESchemaJumpMode Modes[] = {ESchemaJumpMode::Off, ESchemaJumpMode::Exact};
	for (ESchemaJumpMode M : Modes)
	{
		FJumpSequenceFacts Gpu = AfterDecode(M, 4);
		Gpu.bGpu = true;
		EXPECT_EQ(NextJob(Gpu), EJumpNextJob::Decode);
		EXPECT_TRUE(MayJoinDecodeBatch(Gpu));

		FJumpSequenceFacts GpuPrompt = AfterPrompt(M, 4);
		GpuPrompt.bGpu = true;
		EXPECT_EQ(NextJob(GpuPrompt), EJumpNextJob::Decode);

		FJumpSequenceFacts Unbound = AfterDecode(M, 0);
		Unbound.bSchemaBound = false;
		EXPECT_EQ(NextJob(Unbound), EJumpNextJob::Decode);
	}
	EXPECT_EQ(NextJob(AfterDecode(ESchemaJumpMode::Off, 4)), EJumpNextJob::Decode);
	EXPECT_EQ(NextJob(AfterPrompt(ESchemaJumpMode::Off, 4)), EJumpNextJob::Decode);
}

HOST_TEST(loop_exact_takes_one_span_after_each_decode_at_a_forced_state, "P1X.2 host half; §6 P1-X loop; §5.10.3 precondition")
{
	using M = ESchemaJumpMode;
	// After the prompt there is no pending token: exact mode decodes first.
	EXPECT_EQ(NextJob(AfterPrompt(M::Exact, 3)), EJumpNextJob::Decode);
	// A decode left a pending token at a forced state: one span job.
	EXPECT_EQ(NextJob(AfterDecode(M::Exact, 3)), EJumpNextJob::Span);
	EXPECT_TRUE(!MayJoinDecodeBatch(AfterDecode(M::Exact, 3)));
	// After the span job: decode, whatever is left of the chain (no drain rule, §5.10.8).
	EXPECT_EQ(NextJob(AfterSpan(M::Exact, 2)), EJumpNextJob::Decode);
	EXPECT_EQ(NextJob(AfterSpan(M::Exact, 0)), EJumpNextJob::Decode);
	// A pending token at a choice state: decode, no span.
	EXPECT_EQ(NextJob(AfterDecode(M::Exact, 0)), EJumpNextJob::Decode);
}

HOST_TEST(loop_exact_cold_start_sends_no_span_job, "P1X.1 (rev 1.3.5 cold start): an empty draft sends no span job; mutant XD2")
{
	// A pending token at a forced state, and no learned draft from the state after it: the decode
	// job embeds the pending token. A span job here would pass a NULL or an empty draft.
	const FJumpSequenceFacts Cold = AfterDecodeCold(ESchemaJumpMode::Exact, 3);
	EXPECT_EQ(NextJob(Cold), EJumpNextJob::Decode);
	EXPECT_TRUE(MayJoinDecodeBatch(Cold));
	// Positive clause: one drafted token is enough.
	FJumpSequenceFacts One = Cold;
	One.DraftLength = 1;
	EXPECT_EQ(NextJob(One), EJumpNextJob::Span);
}

HOST_TEST(loop_exact_never_takes_two_spans_in_a_row, "P1X.2 host half: one span job per decode job (a no-op span leaves the token pending)")
{
	// The verb was a no-op (an empty draft, or h = 0): the token is still pending at a forced state,
	// and the last job was a span. The next job is the decode, not another span.
	FJumpSequenceFacts F = AfterDecode(ESchemaJumpMode::Exact, 3);
	REQUIRE_EQ(NextJob(F), EJumpNextJob::Span);
	F.bLastJobWasSpan = true;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Decode);
	EXPECT_TRUE(MayJoinDecodeBatch(F));
}

HOST_TEST(loop_exact_span_failure_goes_to_the_decode, "P1X.10 host half: a non-OK verb status ends the span job; the decode follows")
{
	FJumpSequenceFacts F = AfterDecode(ESchemaJumpMode::Exact, 3);
	REQUIRE_EQ(NextJob(F), EJumpNextJob::Span);
	F.bLastJobWasSpan = true;
	F.bLastSpanFailed = true;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Decode);
}

HOST_TEST(loop_outstanding_span_job_keeps_the_sequence_out_of_decode_batches, "P1.2, P1X.2 host half: no decode batch includes a sequence with a span job outstanding")
{
	const ESchemaJumpMode Modes[] = {ESchemaJumpMode::Exact};
	for (ESchemaJumpMode M : Modes)
	{
		FJumpSequenceFacts F = AfterDecode(M, 3);
		REQUIRE_EQ(NextJob(F), EJumpNextJob::Span);
		F.bSpanJobOutstanding = true;
		EXPECT_EQ(NextJob(F), EJumpNextJob::Wait);
		EXPECT_TRUE(!MayJoinDecodeBatch(F));
		F.ForcedChainLength = 0;
		EXPECT_EQ(NextJob(F), EJumpNextJob::Wait);
	}
}

HOST_TEST(loop_mixed_modes_share_one_decode_batch, "P1X.9, P1.7 host half: Exact between spans, Off and unbound in one decode job (plugin 1.4: no Canonical)")
{
	const FJumpSequenceFacts ExactBetween = AfterSpan(ESchemaJumpMode::Exact, 3);
	const FJumpSequenceFacts Off = AfterDecode(ESchemaJumpMode::Off, 3);
	FJumpSequenceFacts Unbound = AfterDecode(ESchemaJumpMode::Exact, 0);
	Unbound.bSchemaBound = false;
	EXPECT_TRUE(MayJoinDecodeBatch(ExactBetween));
	EXPECT_TRUE(MayJoinDecodeBatch(Off));
	EXPECT_TRUE(MayJoinDecodeBatch(Unbound));

	// And the one that must stay out of the same batch.
	EXPECT_TRUE(!MayJoinDecodeBatch(AfterDecode(ESchemaJumpMode::Exact, 2)));
}

HOST_TEST(loop_spent_allowance_ends_every_mode, "§6 P1 MaxNewTokens; P1.4, P1X.5 host half")
{
	const ESchemaJumpMode Modes[] = {ESchemaJumpMode::Off, ESchemaJumpMode::Exact};
	for (ESchemaJumpMode M : Modes)
	{
		FJumpSequenceFacts F = AfterDecode(M, 4);
		F.RemainingAllowance = 0;
		EXPECT_EQ(NextJob(F), EJumpNextJob::End);
		EXPECT_TRUE(!MayJoinDecodeBatch(F));
	}
	// Positive clause: one more token allowed, and the same facts keep going.
	FJumpSequenceFacts F = AfterDecode(ESchemaJumpMode::Exact, 4);
	F.RemainingAllowance = 1;
	EXPECT_EQ(NextJob(F), EJumpNextJob::Span);
}
