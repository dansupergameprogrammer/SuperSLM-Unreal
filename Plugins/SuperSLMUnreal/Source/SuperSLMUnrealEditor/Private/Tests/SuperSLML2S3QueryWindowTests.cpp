// T-2818 (L2-S3 red suite). R-S3e (the plan §9; D-SLM7244;
// the checkbox arm D-SLM7432, D-SLM7445, D-SLM7668): "A-EX in the editor window: one query swept
// across Backend {CPU, GPU}, concurrent queries {1, 4}, three Frame Budget positions, and two k
// values, run once with the checkbox off and once with it on -> one token digest across the
// sweep, per checkbox setting -- on the 2080 SUPER (GPU rows) and on the CPU -- never compared
// between the off digest and the on digest."
//
// Two test functions, one per checkbox setting, sharing one sweep (RunSweepForCheckboxSetting).
// Each function owns its own reference digest, so no code path can compare the off digest with
// the on digest: the two settings' outputs differ by design (D-SLM7432, plan §8).
//
// Rewritten 2026-09-25 (the test record §2) to add the
// checkbox-on arm. The off arm changed with it, in four ways, each recorded there: it uses
// FTestWorldWrapper and the world-scoped subsystem accessors (the no-argument calls no longer
// compile); an unavailable GPU fails the cell instead of warning (D-SLM7519); the GPU
// configuration is one the GPU subsystem accepts (DispatchBudget 4 is below one layer's 24
// dispatches, so Configure() refused it and the GPU rows never ran); and each row generates 48
// tokens, the figure §10.4's cost line uses.
//
// Round 5 (2026-09-25, record §10): the k assertion is replaced (review W1), and the GPU is
// configured at the whole model per tick (fixture default), so the Frame Budget axis is not capped
// at 1 layer on the GPU rows.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMQueryWindowController.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

namespace
{
	// Tokens each sweep row generates (plan §10.4: "24 runs of 48 tokens").
	constexpr int32 SweepMaxNewTokens = 48;

	// Plan §8's control list: 3 Frame Budget positions x 2 concurrent-query values x 2 k values
	// = 12 rows per backend, every one asserted against the same digest.
	TArray<FSuperSLMQueryWindowConfig> SweepConfigs(ESuperSLMBackendBP Backend, bool bCheckbox)
	{
		TArray<FSuperSLMQueryWindowConfig> Configs;
		const int32 Layers = SuperSLML2S3Fixtures::AExNumHiddenLayers;
		for (const int32 FrameBudget : {1, Layers / 2, Layers})
		{
			for (const int32 Concurrent : {1, 4})
			{
				for (const int32 K : {1, 4})
				{
					FSuperSLMQueryWindowConfig Config;
					Config.Backend = Backend;
					Config.FrameBudgetLayers = FrameBudget;
					Config.ConcurrentQueries = Concurrent;
					Config.RequestedK = K;
					Config.bSchemaConstrainedDecoding = bCheckbox;
					Configs.Add(Config);
				}
			}
		}
		return Configs;
	}

	FString RowLabel(const FSuperSLMQueryWindowConfig& Row)
	{
		return FString::Printf(TEXT("%s row (checkbox=%s, frame=%d, concurrent=%d, k=%d)"),
			Row.Backend == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU"),
			Row.bSchemaConstrainedDecoding ? TEXT("on") : TEXT("off"),
			Row.FrameBudgetLayers, Row.ConcurrentQueries, Row.RequestedK);
	}

	// One checkbox setting's whole sweep: 12 CPU rows, then 12 GPU rows, all against ONE digest --
	// the first CPU row's. Returns true only when all 24 rows ran and every digest matched.
	//
	// Per row, besides the digest, a precondition that the checkbox did what this setting says,
	// read from that row's own output and never from the other setting's:
	//   on:  the raw output opens with the schema's literal prefix {"Prompt_Result": " and
	//        StopReason and SchemaAcceptingAtStop are set;
	//   off: the raw output does not open with that prefix, and StopReason is unset.
	// Without these, a controller that ignored the checkbox would pass both arms: each arm's
	// digest would still be constant across its own sweep.
	bool RunSweepForCheckboxSetting(FAutomationTestBase& Test, bool bCheckbox)
	{
		const TCHAR* CellName = bCheckbox ? TEXT("R-S3e checkbox on") : TEXT("R-S3e checkbox off");
		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game))
		{
			Test.AddError(FString::Printf(TEXT("%s: could not create a game world"), CellName));
			return false;
		}
		SuperSLML2S3Fixtures::FQueryWindowBackends Backends;
		if (!SuperSLML2S3Fixtures::SetUpQueryWindowBackends(
				Test, World.GetTestWorld(), CellName, /*BlockCount*/ 4, /*bRequirePromptResultSchema*/ bCheckbox, Backends))
		{
			return false;
		}

		FSuperSLMQueryWindowController Controller(
			*Backends.Cpu, Backends.Gpu, *Backends.Model, SuperSLML2S3Fixtures::AExSchemaName());
		FString SelfCheckError;
		if (!Test.TestTrue(TEXT("self-check runs"), Controller.RunSelfCheck(SelfCheckError)))
		{
			Test.AddError(SelfCheckError);
		}

		TArray<FSuperSLMQueryWindowConfig> Rows = SweepConfigs(ESuperSLMBackendBP::CPU, bCheckbox);
		Rows.Append(SweepConfigs(ESuperSLMBackendBP::GPU, bCheckbox));

		FString ReferenceDigestHex;
		int32 Failures = 0;
		for (const FSuperSLMQueryWindowConfig& Row : Rows)
		{
			const FString Label = RowLabel(Row);
			FSuperSLMQueryWindowReadout Readout;
			FString RunError;
			if (!Test.TestTrue(Label + TEXT(" runs"),
					Controller.RunQuery(SuperSLML2S3Fixtures::SweepPromptText(), SweepMaxNewTokens, Row, Readout, RunError)))
			{
				Test.AddError(RunError);
				++Failures;
				continue;
			}

			const bool bHasLiteralPrefix =
				SuperSLML2S3Fixtures::FindPromptResultContentStart(Readout.RawOutputText) != INDEX_NONE;
			if (bCheckbox)
			{
				Failures += !Test.TestTrue(Label + TEXT(": raw output opens with {\"Prompt_Result\": \" (checkbox took effect)"),
					bHasLiteralPrefix);
				Failures += !Test.TestTrue(Label + TEXT(": StopReason is set"), Readout.StopReason.IsSet());
				Failures += !Test.TestTrue(Label + TEXT(": SchemaAcceptingAtStop is set"), Readout.SchemaAcceptingAtStop.IsSet());
			}
			else
			{
				Failures += !Test.TestFalse(Label + TEXT(": raw output does not open with {\"Prompt_Result\": \" (checkbox is off)"),
					bHasLiteralPrefix);
				Failures += !Test.TestFalse(Label + TEXT(": StopReason is unset"), Readout.StopReason.IsSet());
			}
			if (bHasLiteralPrefix != bCheckbox)
			{
				Test.AddInfo(FString::Printf(TEXT("%s raw output: %s"), *Label, *Readout.RawOutputText));
			}

			// k (§8: "clamped to at least the ticks one sliced token needs, and the minimum is shown").
			// Round 5 (review W1, record §10): the former `EffectiveK >= MinimumK` could not fail,
			// since the controller computes EffectiveK as that very max. What can fail, on the GPU:
			// the clamp is RequestedK raised to MinimumK and no further, and the K the scheduler
			// actually runs at -- the readout's AppliedK, read before the query's schedule restore -- is
			// EffectiveK. On the CPU k is not a control (D-SLM7409: AppliedK
			// there is the largest K the planner derived for the query's own jobs), so no k claim
			// is asserted on CPU rows; they still vary RequestedK so the digest is shown invariant
			// to it.
			if (Row.Backend == ESuperSLMBackendBP::GPU)
			{
				// Review round 2, N3: the clamp itself (EffectiveK == max(RequestedK, MinimumK)) is the
				// runner's own formula on the runner's own MinimumK and cannot fail, so it is not
				// asserted; the two checks below compare it with what the scheduler ran at.
				Failures += !Test.TestEqual(Label + TEXT(": AppliedK == EffectiveK"), Readout.AppliedK, Readout.EffectiveK);
				// Review round 3, R3-S1: GetConfiguredK() is not read after the query. The query's end
				// restores the Configure()'d schedule (R2-W3), so it reads the configured K by then;
				// AppliedK is read in FinishQuery() before that restore, while the query still runs.
			}

			if (ReferenceDigestHex.IsEmpty())
			{
				// The first row is the CPU reference. A 64-hex-char digest, not merely non-empty:
				// an empty or placeholder string would make every later equality meaningless.
				ReferenceDigestHex = Readout.TokenDigestHex;
				if (!Test.TestEqual(Label + TEXT(": reference digest is 64 hex chars"), ReferenceDigestHex.Len(), 64))
				{
					return false;
				}
				continue;
			}
			Failures += !Test.TestEqual(Label + TEXT(" matches this setting's reference digest"),
				Readout.TokenDigestHex, ReferenceDigestHex);
		}
		Test.AddInfo(FString::Printf(TEXT("%s: digest %s over %d rows"), CellName, *ReferenceDigestHex, Rows.Num()));
		return Failures == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3QueryWindowInvariantTest,
	"SuperSLM.L2S3.QueryWindow.InvariantHoldsAcrossEveryControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3QueryWindowInvariantTest::RunTest(const FString& Parameters)
{
	return RunSweepForCheckboxSetting(*this, /*bCheckbox*/ false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3QueryWindowInvariantCheckboxOnTest,
	"SuperSLM.L2S3.QueryWindow.InvariantHoldsAcrossEveryControlCheckboxOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3QueryWindowInvariantCheckboxOnTest::RunTest(const FString& Parameters)
{
	return RunSweepForCheckboxSetting(*this, /*bCheckbox*/ true);
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
