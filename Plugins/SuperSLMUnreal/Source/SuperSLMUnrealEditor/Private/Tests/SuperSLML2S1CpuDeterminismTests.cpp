// T-2805 -- L2-S1 red suite. Plan cell R-S1a (the plan §9):
// "The plugin does not perturb Layer 1's CPU bits, at any frame budget, with tracing on, on a
// recycled handle" -- dims 6, 7, 8.
//
// Every test in this file drives the SAME real artifact (A-CPU, T-2783's own
// qwen2.5-0.5b-instruct.sslm) through USuperSLMSubsystem's real tick loop over the SAME 20
// stride-sampled prompts, and asserts the plugin's own generated tokens are IDENTICAL, id for
// id, to Layer 1's own `sslm_generate` recorded output at v1.5.0 (D-SLM7229) -- never a value
// the plugin itself computes as its own reference (the reference is independent of what it
// grades). Six arms, matching the plan's own costing text (§10.2:
// "over six arms (three budgets, trace on, recycled, baseline)"):
//   1. Baseline    -- no SetLayerBudget() call at all; whatever the subsystem defaults to.
//   2. LayerBudget1  -- explicit layer_budget = 1 (finest CPU slicing this ABI allows).
//   3. LayerBudget6  -- explicit layer_budget = 6.
//   4. LayerBudget24 -- explicit layer_budget = 24 (= num_hidden_layers, i.e. one whole token
//      per decode call, no slicing) -- a DIFFERENT code path from Baseline even if the
//      subsystem's own default happens to equal 24, because this arm calls SetLayerBudget()
//      explicitly and Baseline never does; a defect specific to either path is caught by
//      keeping both as separate tests rather than treating one as redundant with the other.
//   5. TraceOn     -- layer_budget 24, the `SuperSLM` trace channel toggled on for the duration
//      (§5.1: "Instrumentation is read-only. It never feeds a decode ... a perturbation shows
//      up as a mismatch").
//   6. RecycledHandles -- layer_budget 24, a KV pool of only 3 sequences serving 20 prompts, so
//      every handle is vended, returned, and re-vended multiple times within the run.
//
// Feeds the fixture's own recorded `promptTokens` directly (never `Tokenize()`): A-CPU carries
// no tokenizer section, and D-SLM7339 rules that R-S1a keeps feeding recorded token ids anyway,
// because its own claim is that the plugin does not perturb Layer 1's bits, not text handling.
// R-S1a's own "input token ids confirmed" language is satisfied by the fixture's own dual
// sourcing (Tests/Fixtures/L2S1/PROVENANCE.md: `promptTokens` and `expectedOutputTokens` both
// come from Layer 1's own recorded run, independent of this suite).
//
// Uses an explicit FTestWorldWrapper (Engine/Public/Tests/AutomationCommon.h) rather than
// scanning for an already-running world: a headless test run (UnrealEditor-Cmd with
// -ExecCmds="Automation RunTests ...", as in CONTRIBUTING.md) has no PIE, and a UGameInstanceSubsystem exists only on a world whose context carries a real
// UGameInstance (T-2815 build log D1, reasoned from `PlayLevel.cpp` and confirmed against
// `AutomationCommon.cpp`'s own `FTestWorldWrapper::CreateTestWorld`, which is exactly this
// engine's own utility for this situation). `EWorldType::Game` is the one case that constructs
// and `Init()`s a `UGameInstance` and assigns it to the new world context's `OwningGameInstance`.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "Trace/Trace.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	// Shared setup: import A-CPU fresh and (re)configure the subsystem. Configure() is
	// idempotent (SuperSLMRuntimeConfig.h), so re-running this once per arm/test function is the
	// documented way six arms share one GameInstanceSubsystem instance.
	bool SetUpArm(FAutomationTestBase& T, UWorld* World, int32 BlockCount, int32 FinishParallelTasks, USuperSLMModel*& OutModel, USuperSLMSubsystem*& OutSubsystem)
	{
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag);
		if (!T.TestNotNull(TEXT("A-CPU must import (is SUPERSLM_HF_CACHE set, with superslm_artifacts/ under it, on this box?)"), OutModel))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("A-CPU Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return false;
		}

		OutSubsystem = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem must be reachable from the test world's GameInstance"), OutSubsystem))
		{
			return false;
		}

		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;   // every fixture prompt is well under this (longest: 24 tokens)
		Config.MaxLayerBudget = ACpuNumHiddenLayers;
		Config.BlockCount = BlockCount;
		Config.FinishParallelTasks = FinishParallelTasks;
		Config.SequenceLifecycleBudgetMs = 1000.0; // generous; R-S1e is where this is calibrated for real
		Config.TickBudgetMs = 1000.0;

		const FSuperSLMConfigureReport Report = OutSubsystem->Configure(OutModel, Config);
		return T.TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// Runs all 20 reference cases through one arm and asserts token-identical output.
	// LayerBudgetOverride < 0 means "do not call SetLayerBudget at all" (the Baseline arm).
	bool RunArm(FAutomationTestBase& T, UWorld* World, int32 BlockCount, int32 LayerBudgetOverride, int32 FinishParallelTasks = 4)
	{
		USuperSLMModel* Model = nullptr;
		USuperSLMSubsystem* Subsystem = nullptr;
		if (!SetUpArm(T, World, BlockCount, FinishParallelTasks, Model, Subsystem))
		{
			return false;
		}

		TArray<FReferenceCase> Cases;
		FString LoadError;
		const bool bLoaded = LoadReferenceCases(Cases, LoadError);
		if (!T.TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), bLoaded))
		{
			return false;
		}

		int32 MismatchCount = 0;
		for (const FReferenceCase& Case : Cases)
		{
			ESuperSLMVendResult VendResult = ESuperSLMVendResult::NotConfigured;
			FSuperSLMSequence Seq;
			VendResult = Subsystem->VendSequence(Seq);
			if (!T.TestEqual(*FString::Printf(TEXT("%s: VendSequence()"), *Case.Id), (uint8)VendResult, (uint8)ESuperSLMVendResult::Success))
			{
				++MismatchCount;
				continue;
			}

			if (LayerBudgetOverride > 0)
			{
				Subsystem->SetLayerBudget(Seq, LayerBudgetOverride);
			}

			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Case.PromptTokens;
			Request.MaxNewTokens = 48;
			Request.SpanKind = ESuperSLMSpanKind::Prompt;
			// T-2783's own reference run used `sslm_generate --stop 151645,151643`
			// (read from the run's own scoring script, T-2815 build log D3) -- 19 of the 20 fixture
			// cases end on 151645 with the stop token included; only `open21` runs the full 48.
			// Without this the plugin correctly generates 48 tokens and mismatches those 19.
			Request.StopTokenIds = {151645, 151643};

			TArray<int32> GeneratedTokens;
			FString RunError;
			// 48 tokens at layer_budget=1 is the slowest arm this file runs; 200 ticks is a wide
			// margin over the natural per-token tick count at any budget this file exercises.
			const bool bCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, GeneratedTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
			Subsystem->ReturnSequence(Seq);

			if (!T.TestTrue(*FString::Printf(TEXT("%s: generation must complete (%s)"), *Case.Id, *RunError), bCompleted))
			{
				++MismatchCount;
				continue;
			}

			// FEAT oracle: token-identical to Layer 1's OWN recorded sslm_generate output
			// (D-SLM7229) -- not a value this suite or the plugin computes. A plugin that
			// perturbs even one bit along the decode path (a wrong layer_budget slice boundary,
			// a stale KV row from a recycled handle, tracing feeding back into the decode)
			// changes at least one token id here.
			if (!T.TestEqual(*FString::Printf(TEXT("%s: generated tokens must equal sslm_generate's recorded output"), *Case.Id),
					GeneratedTokens, Case.ExpectedOutputTokens))
			{
				++MismatchCount;
			}
		}

		return MismatchCount == 0;
	}
}

// Arm 1: Baseline -- no SetLayerBudget() call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismBaselineTest,
	"SuperSLM.L2S1.CpuDeterminism.Baseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismBaselineTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1, /*LayerBudgetOverride*/ -1);
}

// Arm 2: layer_budget = 1 (finest CPU slicing).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismLayerBudget1Test,
	"SuperSLM.L2S1.CpuDeterminism.LayerBudget1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismLayerBudget1Test::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1, /*LayerBudgetOverride*/ 1);
}

// Arm 3: layer_budget = 6.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismLayerBudget6Test,
	"SuperSLM.L2S1.CpuDeterminism.LayerBudget6",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismLayerBudget6Test::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1, /*LayerBudgetOverride*/ 6);
}

// Arm 4: layer_budget = 24 (= num_hidden_layers, one whole token per call, no slicing).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismLayerBudget24Test,
	"SuperSLM.L2S1.CpuDeterminism.LayerBudget24",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismLayerBudget24Test::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1, /*LayerBudgetOverride*/ ACpuNumHiddenLayers);
}

// Arm 5: the SuperSLM trace channel toggled on for the run (§5.1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismTraceOnTest,
	"SuperSLM.L2S1.CpuDeterminism.TraceOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismTraceOnTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	// The C++ channel identifier is `SuperSLMChannel` (T-2815 build log §4 item 5, a namespace
	// collision fix); UE Trace strips a trailing "Channel" when registering, so the RUNTIME name
	// this string toggles is still "SuperSLM" -- unaffected by that rename.
	const bool bWasEnabled = UE::Trace::ToggleChannel(TEXT("SuperSLM"), true);
	const bool bResult = RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1, /*LayerBudgetOverride*/ ACpuNumHiddenLayers);
	if (!bWasEnabled)
	{
		UE::Trace::ToggleChannel(TEXT("SuperSLM"), false);
	}
	return bResult;
}

// Arm 6: a 3-sequence pool serving 20 prompts -- every handle is vended, returned, and
// re-vended several times within the run (RunArm's own per-case Vend/Return already exercises
// this once BlockCount is small enough to force reuse across the 20 cases).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismRecycledHandlesTest,
	"SuperSLM.L2S1.CpuDeterminism.RecycledHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismRecycledHandlesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 3, /*LayerBudgetOverride*/ ACpuNumHiddenLayers);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuDeterminismSerialFinishTest,
	"SuperSLM.L2S1.CpuDeterminism.SerialFinish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuDeterminismSerialFinishTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	return RunArm(*this, TestWorldWrapper.GetTestWorld(), /*BlockCount*/ 1,
		/*LayerBudgetOverride*/ ACpuNumHiddenLayers, /*FinishParallelTasks*/ 1);
}

#endif // WITH_DEV_AUTOMATION_TESTS
