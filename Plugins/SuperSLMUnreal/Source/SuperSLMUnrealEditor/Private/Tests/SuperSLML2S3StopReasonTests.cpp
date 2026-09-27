// T-2818 (L2-S3 suite). The context-cap branch of a schema-constrained query's stop
// (plan §10.4; T-2853 design §5; review N1, re-raised in round 2 as open). Record:
// the test record §12.
//
// Why a unit cell. The query runner refuses a prompt whose tokens plus MaxNewTokens exceed the
// model's context_cap before it vends (SuperSLMQuery.cpp StartOnBackend), so no query through the
// window, USuperSLMQuery or the MCP tier can stop at the cap: the branch is reachable only by an
// API caller composing its own facts (SuperSLMSchemaDryRun.cpp does). The refusal itself is
// asserted by SuperSLML2S3QueryBlueprintFaceTests.cpp. This cell drives the two pure functions
// the branch consists of: SuperSLMPromptResult::ComposeStopReason() with FStopFacts, and the
// display a BudgetExhausted stop gets, RecoverTruncatedOutput().
//
// Every case is paired with the case that differs from it only in the fact under test, so a
// composition that ignored bContextCapReached, or read it where the design says it does not
// apply, fails a named case.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "SuperSLMPromptResult.h"
#include "SuperSLMSequenceTypes.h"

namespace
{
	using SuperSLMPromptResult::FStopFacts;

	FStopFacts Facts(ESuperSLMSequencePhase Phase, ESuperSLMDecodeOutcome Outcome, bool bAccepting, bool bCapReached)
	{
		FStopFacts F;
		F.Phase = Phase;
		F.LastOutcome = Outcome;
		F.bSchemaAccepting = bAccepting;
		F.bContextCapReached = bCapReached;
		return F;
	}

	const TCHAR* ReasonName(ESuperSLMQueryStopReason R)
	{
		switch (R)
		{
		case ESuperSLMQueryStopReason::Completed: return TEXT("Completed");
		case ESuperSLMQueryStopReason::SchemaRejected: return TEXT("SchemaRejected");
		case ESuperSLMQueryStopReason::BudgetExhausted: return TEXT("BudgetExhausted");
		}
		return TEXT("?");
	}

	void ExpectComposed(FAutomationTestBase& T, const TCHAR* Case, const FStopFacts& F, ESuperSLMQueryStopReason Expected)
	{
		ESuperSLMQueryStopReason Got = ESuperSLMQueryStopReason::Completed;
		const bool bComposed = SuperSLMPromptResult::ComposeStopReason(F, Got);
		T.TestTrue(FString::Printf(TEXT("%s: a stop reason is composed"), Case), bComposed);
		if (bComposed)
		{
			T.TestTrue(FString::Printf(TEXT("%s: %s (got %s)"), Case, ReasonName(Expected), ReasonName(Got)), Got == Expected);
		}
	}

	void ExpectNotComposed(FAutomationTestBase& T, const TCHAR* Case, const FStopFacts& F)
	{
		ESuperSLMQueryStopReason Got = ESuperSLMQueryStopReason::Completed;
		T.TestFalse(FString::Printf(TEXT("%s: no stop reason is composed (a failed query, never a stop)"), Case),
			SuperSLMPromptResult::ComposeStopReason(F, Got));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3ContextCapStopReasonTest,
	"SuperSLM.L2S3.PromptResult.ContextCapStopIsBudgetExhausted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3ContextCapStopReasonTest::RunTest(const FString& Parameters)
{
	using P = ESuperSLMSequencePhase;
	using O = ESuperSLMDecodeOutcome;
	using R = ESuperSLMQueryStopReason;

	// The branch: a Faulted stop mid-token (Generating) with the positions at context_cap is a
	// budget, not an error (the CPU carries the cap only in its fault text; the GPU reports a
	// recoverable per-sequence rejection).
	ExpectComposed(*this, TEXT("Faulted/Generating at context_cap, walk open"),
		Facts(P::Faulted, O::Generating, /*accepting*/ false, /*cap*/ true), R::BudgetExhausted);
	// Its pair: the same stop below the cap is a device or call failure.
	ExpectNotComposed(*this, TEXT("Faulted/Generating below context_cap, walk open"),
		Facts(P::Faulted, O::Generating, false, false));

	// Precedence around the branch (design §5's order: accepting, then dead end, then budget).
	ExpectComposed(*this, TEXT("Faulted/Generating at context_cap, walk accepting"),
		Facts(P::Faulted, O::Generating, true, true), R::Completed);
	ExpectComposed(*this, TEXT("Faulted/SchemaDeadEnd at context_cap"),
		Facts(P::Faulted, O::SchemaDeadEnd, false, true), R::SchemaRejected);

	// The cap is read only on a Faulted/Generating stop: another fault at the cap is still a failure.
	ExpectNotComposed(*this, TEXT("Faulted/SequenceNoLongerValid at context_cap"),
		Facts(P::Faulted, O::SequenceNoLongerValid, false, true));
	ExpectNotComposed(*this, TEXT("Faulted/TokenProduced at context_cap"),
		Facts(P::Faulted, O::TokenProduced, false, true));

	// A running sequence composes nothing, at the cap or not.
	ExpectNotComposed(*this, TEXT("Decoding at context_cap"), Facts(P::Decoding, O::Generating, false, true));
	ExpectNotComposed(*this, TEXT("Prefilling at context_cap"), Facts(P::Prefilling, O::Generating, false, true));

	// The MaxNewTokens budget is BudgetExhausted with or without the cap.
	ExpectComposed(*this, TEXT("Complete at context_cap, walk open"), Facts(P::Complete, O::TokenProduced, false, true), R::BudgetExhausted);
	ExpectComposed(*this, TEXT("Complete below context_cap, walk open"), Facts(P::Complete, O::TokenProduced, false, false), R::BudgetExhausted);

	// The display a context-cap stop gets is the BudgetExhausted display: the recovered,
	// unescaped prefix of the value, never the raw fragment, with an incomplete trailing escape
	// dropped. The raw texts are what a cut at the cap leaves.
	struct FCut { const TCHAR* Raw; const TCHAR* Expected; };
	const FCut Cuts[] = {
		{ TEXT("{\"Prompt_Result\": \"Two health potions, that will be"), TEXT("Two health potions, that will be") },
		{ TEXT("{\"Prompt_Result\":\"She said \\\"hello"), TEXT("She said \"hello") },
		{ TEXT("{\"Prompt_Result\":\"Line one\\nLine two\\u00"), TEXT("Line one\nLine two") },
		{ TEXT("{\"Prompt_Result\":\"Ends on a backslash\\"), TEXT("Ends on a backslash") },
	};
	for (const FCut& Cut : Cuts)
	{
		FString Shown;
		const bool bRecovered = SuperSLMPromptResult::RecoverTruncatedOutput(Cut.Raw, Shown);
		TestTrue(FString::Printf(TEXT("a context-cap cut is recovered: %s"), Cut.Raw), bRecovered);
		TestEqual(FString::Printf(TEXT("the recovered text for %s"), Cut.Raw), Shown, FString(Cut.Expected));
		FString Parsed, ParseError;
		TestFalse(FString::Printf(TEXT("the cut does not parse as a completed answer: %s"), Cut.Raw),
			SuperSLMPromptResult::ParseCompletedOutput(Cut.Raw, Parsed, ParseError));
	}
	// A cut before the value's content begins has nothing to show.
	FString Nothing;
	TestFalse(TEXT("a cut inside the key recovers nothing"), SuperSLMPromptResult::RecoverTruncatedOutput(TEXT("{\"Prompt_Res"), Nothing));
	TestTrue(TEXT("a cut inside the key shows empty text"), Nothing.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
