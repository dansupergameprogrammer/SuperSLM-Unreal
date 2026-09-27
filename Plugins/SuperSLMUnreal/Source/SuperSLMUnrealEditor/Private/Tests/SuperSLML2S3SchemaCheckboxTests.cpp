// T-2818 continuation (L2-S3 red suite, 2026-09-25). R-S3g and R-S3h
// (the plan §9, §10.4, §2.5 rows 11-12; T-2853 design §5-§6,
// the design record; D-SLM7432, D-SLM7436,
// D-SLM7440, D-SLM7447, D-SLM7448, D-SLM7668). Record:
// the test record.
//
// R-S3g: with the query window's checkbox on, the answer is well-formed JSON with exactly the key
// Prompt_Result, a non-empty string, reached before the token budget, stop reason Completed --
// on the CPU and on the GPU.
// R-S3h: with MaxNewTokens below what the answer needs, the query stops BudgetExhausted, the raw
// output does not parse, accepting reads false, and the window shows the recovered,
// JSON-unescaped Prompt_Result prefix: non-empty, and a prefix of the value a second run with the
// budget raised past acceptance produces -- on the CPU and on the GPU.
//
// Both cells drive the query window's controller, the surface a user touches. Acceptance is read
// through the readout's SchemaAcceptingAtStop, whose sources the controller header fixes:
// GetStats().bSchemaAccepting on the CPU, USuperSLMGpuSubsystem::IsSchemaAccepting() on the GPU.
// The GPU accessor is pinned in both directions by the pair: R-S3g fails on one that always reads
// false, R-S3h on one that always reads true.
//
// Every "cannot answer" case fails the cell by name (D-SLM7519): A-EX absent or without the
// §10.4 rebuild, a backend that does not configure, a query that does not run, an unset
// StopReason or SchemaAcceptingAtStop, and an R-S3h reference that does not itself accept.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMQueryWindowController.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S3Fixtures;

namespace
{
	// One query, one backend, checkbox on, ConcurrentQueries 1. Frame Budget is the whole model
	// on both backends: these cells are about the answer, and R-S3e already shows no control
	// moves it within a checkbox setting.
	FSuperSLMQueryWindowConfig CheckboxOnConfig(ESuperSLMBackendBP Backend)
	{
		FSuperSLMQueryWindowConfig Config;
		Config.Backend = Backend;
		Config.FrameBudgetLayers = AExNumHiddenLayers;
		Config.ConcurrentQueries = 1;
		Config.RequestedK = 1;
		Config.bSchemaConstrainedDecoding = true;
		return Config;
	}

	const TCHAR* BackendName(ESuperSLMBackendBP Backend)
	{
		return Backend == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU");
	}

	// Runs one checkbox-on query and fails by name if it does not run, or if the readout does not
	// classify it (StopReason or SchemaAcceptingAtStop unset). True only when the readout can be
	// asserted against.
	bool RunClassifiedQuery(
		FAutomationTestBase& Test,
		FSuperSLMQueryWindowController& Controller,
		const FString& Prompt,
		int32 MaxNewTokens,
		ESuperSLMBackendBP Backend,
		const FString& Label,
		FSuperSLMQueryWindowReadout& OutReadout)
	{
		FString RunError;
		if (!Test.TestTrue(Label + TEXT(": query runs"),
				Controller.RunQuery(Prompt, MaxNewTokens, CheckboxOnConfig(Backend), OutReadout, RunError)))
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *Label, *RunError));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %d tokens, stop reason %s, raw output: %s"), *Label,
			OutReadout.GeneratedTokens.Num(), *StopReasonName(OutReadout.StopReason), *OutReadout.RawOutputText));
		const bool bReasonSet = Test.TestTrue(Label + TEXT(": StopReason is set (checkbox on)"), OutReadout.StopReason.IsSet());
		const bool bAcceptingSet = Test.TestTrue(Label + TEXT(": SchemaAcceptingAtStop is set (checkbox on)"),
			OutReadout.SchemaAcceptingAtStop.IsSet());
		return bReasonSet && bAcceptingSet;
	}

	// ---- R-S3g, one backend ----
	bool RunRS3gArm(FAutomationTestBase& Test, FSuperSLMQueryWindowController& Controller, ESuperSLMBackendBP Backend)
	{
		const FString Label = FString::Printf(TEXT("R-S3g %s"), BackendName(Backend));
		FSuperSLMQueryWindowReadout Readout;
		if (!RunClassifiedQuery(Test, Controller, RS3gPromptText(), RS3gMaxNewTokens, Backend, Label, Readout))
		{
			return false;
		}
		bool bOk = true;

		// The raw output parses as exactly one JSON object.
		// (Each parse runs before its assertion, so the message carries the parser's reason.)
		TSharedPtr<FJsonObject> Object;
		FString Why;
		const bool bParsed = TryParseJsonObject(Readout.RawOutputText, Object, Why);
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: raw output parses as one JSON object (%s)"), *Label, *Why), bParsed);

		// Exactly the key Prompt_Result, case-sensitive; its value a non-empty string.
		if (bParsed)
		{
			FString Value;
			const bool bField = TryReadPromptResultField(*Object, Value, Why);
			bOk &= Test.TestTrue(FString::Printf(TEXT("%s: exactly one key, Prompt_Result, holding a string (%s)"), *Label, *Why), bField);
			bOk &= Test.TestTrue(Label + TEXT(": Prompt_Result is a non-empty string"), bField && !Value.IsEmpty());
		}

		// Reached before the token budget: fewer tokens than MaxNewTokens, and at least one.
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: stopped before the budget (%d of %d tokens)"), *Label,
			Readout.GeneratedTokens.Num(), RS3gMaxNewTokens),
			Readout.GeneratedTokens.Num() > 0 && Readout.GeneratedTokens.Num() < RS3gMaxNewTokens);

		// The walk is accepting, and the window says Completed.
		bOk &= Test.TestTrue(Label + TEXT(": schema walk accepting at stop"), Readout.SchemaAcceptingAtStop.GetValue());
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: stop reason is Completed (read %s)"), *Label, *StopReasonName(Readout.StopReason)),
			Readout.StopReason.GetValue() == ESuperSLMQueryStopReason::Completed);
		return bOk;
	}

	// ---- R-S3h, one backend ----
	bool RunRS3hArm(FAutomationTestBase& Test, FSuperSLMQueryWindowController& Controller, ESuperSLMBackendBP Backend)
	{
		const FString Label = FString::Printf(TEXT("R-S3h %s"), BackendName(Backend));

		// 1. The reference: the same prompt, schema, backend and build, budget raised past
		//    acceptance. If it does not accept, the prefix oracle has nothing to compare against,
		//    and the cell fails saying so rather than comparing against a fragment.
		FSuperSLMQueryWindowReadout Reference;
		if (!RunClassifiedQuery(Test, Controller, RS3hPromptText(), RS3hReferenceMaxNewTokens, Backend,
				Label + TEXT(" reference"), Reference))
		{
			return false;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the reference run reaches Completed (read %s); without it the "
				"prefix oracle cannot be evaluated"), *Label, *StopReasonName(Reference.StopReason)),
				Reference.StopReason.GetValue() == ESuperSLMQueryStopReason::Completed))
		{
			return false;
		}
		TSharedPtr<FJsonObject> ReferenceObject;
		FString ReferenceValue, Why;
		const bool bReferenceParsed = TryParseJsonObject(Reference.RawOutputText, ReferenceObject, Why);
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the reference parses (%s)"), *Label, *Why), bReferenceParsed))
		{
			return false;
		}
		const bool bReferenceField = TryReadPromptResultField(*ReferenceObject, ReferenceValue, Why);
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the reference holds one Prompt_Result string (%s)"), *Label, *Why), bReferenceField))
		{
			return false;
		}

		// 2. Precondition on the budget (the underived RS3hTruncatedMaxNewTokens): the answer must
		//    need more tokens than the truncated run is given, or the cut never happens.
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the reference needs more than %d tokens (it used %d); a smaller "
				"budget or a longer-answer prompt is needed"), *Label, RS3hTruncatedMaxNewTokens, Reference.GeneratedTokens.Num()),
				Reference.GeneratedTokens.Num() > RS3hTruncatedMaxNewTokens))
		{
			return false;
		}

		// 3. The truncated run.
		FSuperSLMQueryWindowReadout Truncated;
		if (!RunClassifiedQuery(Test, Controller, RS3hPromptText(), RS3hTruncatedMaxNewTokens, Backend,
				Label + TEXT(" truncated"), Truncated))
		{
			return false;
		}
		bool bOk = true;

		// The budget was what stopped it: exactly MaxNewTokens tokens.
		bOk &= Test.TestEqual(Label + TEXT(": the truncated run produced exactly MaxNewTokens tokens"),
			Truncated.GeneratedTokens.Num(), RS3hTruncatedMaxNewTokens);

		// The premise of the prefix oracle (T-2853 design §6, D-SLM7447): decoding does not look at
		// the budget, so the truncated tokens are the reference's first tokens. If this fails, the
		// text comparison below is not evidence about recovery, and the message says which broke.
		TArray<int32> ReferenceHead(Reference.GeneratedTokens.GetData(),
			FMath::Min(RS3hTruncatedMaxNewTokens, Reference.GeneratedTokens.Num()));
		bOk &= Test.TestEqual(Label + TEXT(": truncated tokens are the reference's first tokens (determinism premise)"),
			Truncated.GeneratedTokens, ReferenceHead);

		// The raw output does not parse, so the boundary was really reached.
		TSharedPtr<FJsonObject> Unexpected;
		FString ParseWhy;
		bOk &= Test.TestFalse(Label + TEXT(": raw output does NOT parse as JSON"),
			TryParseJsonObject(Truncated.RawOutputText, Unexpected, ParseWhy));

		// Accepting reads false, and the window says BudgetExhausted.
		bOk &= Test.TestFalse(Label + TEXT(": schema walk not accepting at stop"), Truncated.SchemaAcceptingAtStop.GetValue());
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: stop reason is BudgetExhausted (read %s)"), *Label, *StopReasonName(Truncated.StopReason)),
			Truncated.StopReason.GetValue() == ESuperSLMQueryStopReason::BudgetExhausted);

		// The displayed text: non-empty, never the raw fragment, and a prefix of the reference's
		// unescaped value (the plan's oracle).
		bOk &= Test.TestFalse(Label + TEXT(": displayed text is non-empty"), Truncated.DisplayedText.IsEmpty());
		bOk &= Test.TestFalse(Label + TEXT(": displayed text is not the raw JSON fragment"),
			Truncated.DisplayedText.Equals(Truncated.RawOutputText, ESearchCase::CaseSensitive));
		bOk &= Test.TestTrue(FString::Printf(TEXT("%s: displayed text [%s] is a prefix of the reference value [%s]"),
			*Label, *Truncated.DisplayedText, *ReferenceValue),
			ReferenceValue.StartsWith(Truncated.DisplayedText, ESearchCase::CaseSensitive));

		// Recovery keeps everything recoverable, not merely a prefix. A one-character display
		// passes the prefix check above, so where the raw content holds no backslash (nothing to
		// unescape, no escape to cut) the display must equal the raw content exactly, up to the
		// closing quote if the cut fell between it and the brace.
		const int32 ContentStart = FindPromptResultContentStart(Truncated.RawOutputText);
		bOk &= Test.TestNotEqual(Label + TEXT(": truncated raw output opens with {\"Prompt_Result\": \""),
			ContentStart, (int32)INDEX_NONE);
		if (ContentStart != INDEX_NONE)
		{
			FString Content = Truncated.RawOutputText.Mid(ContentStart);
			int32 Backslash = INDEX_NONE;
			if (!Content.FindChar(TEXT('\\'), Backslash))
			{
				int32 ClosingQuote = INDEX_NONE;
				if (Content.FindChar(TEXT('"'), ClosingQuote))
				{
					Content.LeftInline(ClosingQuote);
				}
				bOk &= Test.TestEqual(Label + TEXT(": with no escape in the cut, the displayed text is the whole recovered content"),
					Truncated.DisplayedText, Content);
			}
			else
			{
				// Only the prefix check applies; it does test unescaping here, since an escape left
				// raw is not a prefix of the parsed reference value.
				Test.AddInfo(FString::Printf(TEXT("%s: the cut content holds an escape; unescaping is exercised by the "
					"prefix check, and the exact-content check does not apply"), *Label));
			}
		}
		return bOk;
	}

	// Sets up A-EX on both backends and builds the controller the two cells share.
	bool WithCheckboxController(FAutomationTestBase& Test, const TCHAR* CellName,
		TFunctionRef<bool(FSuperSLMQueryWindowController&)> Body)
	{
		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game))
		{
			Test.AddError(FString::Printf(TEXT("%s: could not create a game world"), CellName));
			return false;
		}
		FQueryWindowBackends Backends;
		if (!SetUpQueryWindowBackends(Test, World.GetTestWorld(), CellName, /*BlockCount*/ 1,
				/*bRequirePromptResultSchema*/ true, Backends))
		{
			return false;
		}
		FSuperSLMQueryWindowController Controller(*Backends.Cpu, Backends.Gpu, *Backends.Model, AExSchemaName());
		return Body(Controller);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3CheckboxConstrainedAnswerIsWellFormedTest,
	"SuperSLM.L2S3.QueryWindow.CheckboxConstrainedAnswerIsWellFormedJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3CheckboxConstrainedAnswerIsWellFormedTest::RunTest(const FString& Parameters)
{
	return WithCheckboxController(*this, TEXT("R-S3g"), [this](FSuperSLMQueryWindowController& Controller)
	{
		// Both arms always run, so one backend's failure never hides the other's result.
		const bool bCpu = RunRS3gArm(*this, Controller, ESuperSLMBackendBP::CPU);
		const bool bGpu = RunRS3gArm(*this, Controller, ESuperSLMBackendBP::GPU);
		return bCpu && bGpu;
	});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3CheckboxBudgetTruncationTest,
	"SuperSLM.L2S3.QueryWindow.CheckboxBudgetTruncationReportedAsBudgetExhausted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3CheckboxBudgetTruncationTest::RunTest(const FString& Parameters)
{
	return WithCheckboxController(*this, TEXT("R-S3h"), [this](FSuperSLMQueryWindowController& Controller)
	{
		const bool bCpu = RunRS3hArm(*this, Controller, ESuperSLMBackendBP::CPU);
		const bool bGpu = RunRS3hArm(*this, Controller, ESuperSLMBackendBP::GPU);
		return bCpu && bGpu;
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
