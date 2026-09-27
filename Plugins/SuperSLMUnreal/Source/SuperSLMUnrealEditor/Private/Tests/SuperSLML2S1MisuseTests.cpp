// T-2805 -- L2-S1 red suite. Plan cell R-S1f (the plan §9):
// "Misconfiguration and misuse surface by name" (dims 1, 5) -- "block_count 0 is refused at
// init with a named diagnostic. Request block_count + 1 -> named refusal. Schema bind on a
// non-fresh sequence -> routed to reset-then-bind. -2 surfaced as Schema Dead End. CPU adapter
// swap mid-token -> deferred to the token boundary. Subsystem shutdown with live sequences ->
// no Layer-1 lifecycle rejection fires."
//
// Six sub-claims, six tests, all executable. Five run against A-CPU or A-AD; the fourth (schema
// dead end) runs against the tokenizer-bearing A-EX rebuild (D-SLM7339) and its own compiled
// `potion_shop_order` schema, on the maintainer's ruling on T-2805 §6 Q4 -- see that test's own
// comment for the source-verified mechanism (D-SLM3476, `src/sslm_abi.cpp`).
//
// A-CPU and A-AD's base carry no tokenizer (D-SLM7339) -- every cell against either that needs
// SOME real prompt uses the R-S1a fixture's own recorded token ids directly, never Tokenize().

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	bool ImportACpuOrFail(FAutomationTestBase& T, USuperSLMModel*& OutModel)
	{
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag);
		return T.TestNotNull(TEXT("A-CPU must import"), OutModel) && T.TestTrue(TEXT("A-CPU Diagnostic.bAccepted"), Diag.bAccepted);
	}

	// A real, valid token array with no dependency on any artifact's tokenizer -- the first
	// R-S1a reference case's own recorded prompt tokens (Tests/Fixtures/L2S1/PROVENANCE.md),
	// reused as "some real prompt" for cells testing misconfiguration/scheduling mechanics, not
	// text handling.
	bool LoadSomeRealPromptTokens(FAutomationTestBase& T, TArray<int32>& OutTokens)
	{
		TArray<FReferenceCase> Cases;
		FString LoadError;
		const bool bLoaded = LoadReferenceCases(Cases, LoadError);
		if (!T.TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), bLoaded))
		{
			return false;
		}
		OutTokens = Cases[0].PromptTokens;
		return true;
	}

	// Three-way, not a bool: "A-EX is absent" (T-2805 L23 repair: fails the cell by name via
	// AddError, never a silent skip) and "A-EX is present but failed to import" (a genuine test
	// failure) are kept distinct only so each fails the cell for its own diagnosable reason.
	enum class EAExAvailability { Absent, ImportFailed, Ready };

	EAExAvailability ImportAEx(FAutomationTestBase& T, const TCHAR* CellName, USuperSLMModel*& OutModel)
	{
		FString AExPath, Reason;
		if (!TryGetAExArtifactPath(AExPath, Reason))
		{
			T.AddError(FString::Printf(TEXT("%s: %s"), CellName, *Reason));
			return EAExAvailability::Absent;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return EAExAvailability::ImportFailed;
		}
		return EAExAvailability::Ready;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1BlockCountZeroRefusedTest,
	"SuperSLM.L2S1.Misuse.BlockCountZeroRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1BlockCountZeroRefusedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	if (!ImportACpuOrFail(*this, Model))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 0; // the mutation under test
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;

	const FSuperSLMConfigureReport Report = Subsystem->Configure(Model, Config);
	if (!TestEqual(TEXT("block_count 0 must be refused with InvalidBlockCount"),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::InvalidBlockCount))
	{
		return false;
	}
	// "refused ... with a named diagnostic" (§9 R-S1f) -- never a bare failure code.
	return TestTrue(TEXT("the report Message must be non-empty and name block_count"), Report.Message.Contains(TEXT("block_count"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1VendBeyondBlockCountRefusedTest,
	"SuperSLM.L2S1.Misuse.VendBeyondBlockCountRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1VendBeyondBlockCountRefusedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	if (!ImportACpuOrFail(*this, Model))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSequence First;
	if (!TestEqual(TEXT("the one declared handle must vend"), (uint8)Subsystem->VendSequence(First), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	FSuperSLMSequence Second;
	// "Request block_count + 1 -> named refusal" (§9 R-S1f), and "before
	// SSLM_KV_POOL_EXHAUSTED can arise" (§4) -- the plugin's own free-list refusal, never a
	// Layer-1 status leaking through.
	return TestEqual(TEXT("the (block_count + 1)th vend must report PoolExhausted"),
		(uint8)Subsystem->VendSequence(Second), (uint8)ESuperSLMVendResult::PoolExhausted);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SchemaBindOnNonFreshRoutedToResetThenBindTest,
	"SuperSLM.L2S1.Misuse.SchemaBindOnNonFreshRoutedToResetThenBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SchemaBindOnNonFreshRoutedToResetThenBindTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	if (!ImportACpuOrFail(*this, Model))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	// SSLM_SCHEMA_NONE (Opaque == 0) is always a legal bind target ON A FRESH SEQUENCE (§2.1) --
	// using it here isolates the SEQUENCE-STATE precondition this cell names from schema
	// validity, so this test needs no real compiled schema (A-CPU carries none).
	const FSuperSLMSchemaHandle NoneSchema; // Opaque == 0 by construction

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 24;
	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}
	Subsystem->Tick(1.0f / 60.0f); // now non-fresh: Prefilling or Decoding

	FString BindError;
	// "Schema bind on a non-fresh sequence -> routed to reset-then-bind" (§9 R-S1f): SetSchema
	// on a non-Idle sequence must fail and name the required remedy, never silently apply, defer
	// on its own, or crash.
	if (!TestFalse(TEXT("SetSchema on a non-fresh sequence must fail"), Subsystem->SetSchema(Seq, NoneSchema, BindError)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the failure must name reset as the remedy"), BindError.Contains(TEXT("reset"), ESearchCase::IgnoreCase)))
	{
		return false;
	}

#if SUPERSLM_WITH_L2S1_ASYNC
	// D-SLM7421/D-SLM7423 (round 9): ResetSequence() now queues and returns at once -- Success
	// here means QUEUED, never completed. Poll GetLifecycleOpResult(ResetHandle) until it drains
	// to Success (or the async worker's own committed delivery tick, whichever a real build takes
	// to settle), matching the "read after it drains" discipline every other queued op in this
	// suite already follows.
	FSuperSLMLifecycleOpHandle ResetHandle;
	const ESuperSLMRestoreResult QueueResult = Subsystem->ResetSequence(Seq, ResetHandle, BindError);
	if (!TestEqual(*FString::Printf(TEXT("ResetSequence must queue (not refuse): %s"), *BindError),
			(uint8)QueueResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	if (!TestTrue(TEXT("ResetSequence must return a valid handle"), ResetHandle.IsValid()))
	{
		return false;
	}
	// T-2805 round 10 (§14.7): FastAsPossible pacing -- a content/order cell, real wall-clock time
	// given between polls via Fixtures.h's own DrainTicks.
	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &ResetHandle]() { return Subsystem->GetLifecycleOpResult(ResetHandle) != ESuperSLMRestoreResult::Pending; },
		/*MaxWallClockSeconds*/ 30.0);
	const ESuperSLMRestoreResult ResetOutcome = Subsystem->GetLifecycleOpResult(ResetHandle);
	if (!TestEqual(TEXT("the queued reset must drain to Success"), (uint8)ResetOutcome, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
#else
	// T-2815's own synchronous form (retired once SUPERSLM_WITH_L2S1_ASYNC is turned on).
	if (!TestTrue(TEXT("ResetSequence must succeed"), Subsystem->ResetSequence(Seq, BindError)))
	{
		return false;
	}
	// The tick queue drains at most one reset per tick (§5) -- advance until Idle.
	for (int32 T = 0; T < 16 && Subsystem->GetPhase(Seq) != ESuperSLMSequencePhase::Idle; ++T)
	{
		Subsystem->Tick(1.0f / 60.0f);
	}
#endif // SUPERSLM_WITH_L2S1_ASYNC
	if (!TestEqual(TEXT("sequence must reach Idle after ResetSequence() drains"), (uint8)Subsystem->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Idle))
	{
		return false;
	}

	FString RebindError;
	return TestTrue(TEXT("SetSchema on the now-fresh (Idle) sequence must succeed"), Subsystem->SetSchema(Seq, NoneSchema, RebindError));
}

// The "-2 surfaced as Schema Dead End" clause, realized against A-EX's real compiled schema
// (`potion_shop_order`) -- on the maintainer's ruling on T-2805 §6 Q4.
//
// Verified at source before writing this test (`v1.5.0`, `src/sslm_abi.cpp`, the G5-2 masked-
// argmax/CSR-transition block, cited there as "S2/D-SLM3476"): at a bound schema's OWN accepting
// state, `ApplyMaskAndArgmax` masks every logit to INT32_MIN when that state's mask page is
// legitimately all-zero (a real, documented compiler outcome for a reachable ACCEPTING state --
// only non-accepting reachable states are proven non-empty), the lowest-index tie-break then
// selects token 0, and the DFA's CSR transition table carries no row for token 0 at that state
// (it is empty by construction) -- so `has_transition` is false and Layer 1 sets
// `out_tokens[i] = -2`, `seq->dfa_walk_state` frozen, deterministically and repeatably. This is
// therefore NOT an emergent, chance property of real model weights: ANY sequence that completes
// the potion-shop object's four fields under greedy, schema-masked decode and is then asked to
// decode ONE MORE token in the same schema-bound span reaches this state by construction, every
// time. Confirmed reachable this way; the escape hatch the suite's instructions named ("if you find
// at source that -2 cannot arise this way, stop and report it") does not apply. Needs the
// tokenizer-bearing A-EX rebuild (D-SLM7339) for its own prompt text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SchemaDeadEndSurfacedByNameTest,
	"SuperSLM.L2S1.Misuse.SchemaDeadEndSurfacedByName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SchemaDeadEndSurfacedByNameTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1f SchemaDeadEndSurfacedByName"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24; // A-EX is Qwen2.5-0.5B (§8, §12 decision 9): 24 hidden layers
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSchemaHandle Schema;
	FString SchemaError;
	const bool bSchemaFound = FSuperSLMSchemaLookup::LookupByName(*Model, AExSchemaName(), Schema, SchemaError);
	if (!TestTrue(*FString::Printf(TEXT("schema lookup '%s': %s"), AExSchemaName(), *SchemaError), bSchemaFound))
	{
		return false;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	FString BindError;
	const bool bBindOk = Subsystem->SetSchema(Seq, Schema, BindError);
	if (!TestTrue(*FString::Printf(TEXT("SetSchema: %s"), *BindError), bBindOk))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("Tokenize"), Subsystem->Tokenize(TEXT("A customer walks up to the counter."), PromptTokens)))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	// Deliberately far more tokens than the potion-shop object needs (its own compile log,
	// the build record §5, reports 148 DFA states) -- this
	// request drives decoding PAST the schema's own accepting state on purpose, never switching
	// away from the schema-bound decode this sequence is already running.
	Request.MaxNewTokens = 100;

	TArray<int32> Generated;
	FString RunError;
	const bool bCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
	const ESuperSLMDecodeOutcome LastOutcome = Subsystem->GetLastDecodeOutcome(Seq);
	Subsystem->ReturnSequence(Seq);

	// A decode path that keeps advancing dfa_walk_state on -2 instead of freezing it (the
	// pre-D-SLM3476 defect the source comment itself names: "the walk froze in place, emitting
	// token 0 forever at SSLM_OK") would run to MaxNewTokens=100 and report Complete, never
	// Faulted -- failing this first assertion.
	if (!TestFalse(TEXT("generation must dead-end before MaxNewTokens=100, never reach Complete"), bCompleted))
	{
		return false;
	}
	// FEAT oracle: the sequence's own recorded last decode outcome must be the named enum value,
	// never a bare -2 leaking through, and never a different fault (e.g. a mid-generation call
	// rejection) standing in for the schema-specific one this cell names.
	return TestEqual(TEXT("the sequence's last decode outcome must be SchemaDeadEnd (-2)"),
		(uint8)LastOutcome, (uint8)ESuperSLMDecodeOutcome::SchemaDeadEnd);
}

// A-AD's base carries no tokenizer either (D-SLM7339 names it for retokenization too, but no
// concrete rebuilt path has been given for it) -- this cell reuses the R-S1a fixture's own
// recorded ids as "some real prompt", on the documented assumption that Qwen2.5's model family
// shares one tokenizer/vocabulary across sizes, so a 0.5B-encoded id is a valid, in-vocabulary
// id for the 1.5B model too. Flagged, not hidden -- see
// the red-suite record §6 for the open question this is filed under.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1CpuAdapterSwapMidTokenDeferredTest,
	"SuperSLM.L2S1.Misuse.CpuAdapterSwapMidTokenDeferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1CpuAdapterSwapMidTokenDeferredTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base (1.5B) must import"), Base) || !TestTrue(TEXT("A-AD base Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}

	FSuperSLMAdapterHandle Adapter;
	FString AdapterError;
	const bool bAdapterImported = FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, AdapterError);
	if (!TestTrue(*FString::Printf(TEXT("adapter import: %s"), *AdapterError), bAdapterImported))
	{
		return false;
	}

	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 28; // Qwen2.5-1.5B: 28 layers (plan §1/§5)
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Base, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}

	// A small layer_budget forces several ticks WITHIN each token's own decode, giving a wide
	// window to request the swap strictly mid-token.
	Subsystem->SetLayerBudget(Seq, /*LayerBudget*/ 1);

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 4; // this test only needs to observe ONE token boundary
	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(Seq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}

	bool bRequestedMidToken = false;
	bool bStillBaseImmediatelyAfterRequest = false;
	bool bAppliedAtBoundary = false;
	int32 TokensAtRequestTime = 0;
	// T-2805 round 10 (§14.7): a real Sleep between polls (only while neither exit condition has
	// fired), wall-clock-capped -- this loop's own mid-token detection logic is not a simple
	// Done() predicate, so it stays a manual loop rather than DrainTicks.
	const double DrainStartSeconds = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - DrainStartSeconds < 60.0)
	{
		Subsystem->Tick(1.0f / 60.0f);
		const int32 CurrentTokenCount = Subsystem->GetGeneratedTokens(Seq).Num();

		if (!bRequestedMidToken && Subsystem->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding && CurrentTokenCount == 0)
		{
			// layer_budget=1 on a 28-layer model guarantees at least 27 more ticks before the
			// first token completes -- this fires strictly mid-token, never at a boundary.
			Subsystem->RequestAdapterSwap(Seq, Adapter);
			bRequestedMidToken = true;
			TokensAtRequestTime = CurrentTokenCount;
			// "deferred ... to the token boundary" (§9 R-S1f): immediately after the request,
			// the active adapter must NOT have changed yet.
			bStillBaseImmediatelyAfterRequest = !Subsystem->GetActiveAdapter(Seq).IsValid();
			FPlatformProcess::Sleep(0.001f);
			continue;
		}

		if (bRequestedMidToken && CurrentTokenCount > TokensAtRequestTime)
		{
			// A token boundary was just crossed -- the deferred swap must have applied by now.
			bAppliedAtBoundary = Subsystem->GetActiveAdapter(Seq).IsValid();
			break;
		}
		// Give the worker real wall-clock time to make progress before the next poll (§14.7).
		FPlatformProcess::Sleep(0.001f);
	}

	Subsystem->ReturnSequence(Seq);

	// FSuperSLMAdapterImport::Release() is new at this fold (T-2815 build log §4 item 4): each
	// adapter holds its 1.5 GB base-model mapping until released. Released unconditionally here
	// (not gated on the assertions below) so this test never leaks it, whatever the outcome.
	FString ReleaseError;
	FSuperSLMAdapterImport::Release(Adapter, ReleaseError);

	if (!TestTrue(TEXT("the swap must have been requested strictly mid-token"), bRequestedMidToken))
	{
		return false;
	}
	if (!TestTrue(TEXT("the active adapter must not change until the token boundary"), bStillBaseImmediatelyAfterRequest))
	{
		return false;
	}
	return TestTrue(TEXT("the deferred adapter swap must have applied by the next token boundary"), bAppliedAtBoundary);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SubsystemShutdownWithLiveSequencesTest,
	"SuperSLM.L2S1.Misuse.SubsystemShutdownWithLiveSequences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SubsystemShutdownWithLiveSequencesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	if (!ImportACpuOrFail(*this, Model))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 2;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 2;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMSequence LiveSeq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(LiveSeq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 32;
	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(LiveSeq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}
	Subsystem->Tick(1.0f / 60.0f); // LiveSeq is now mid-generation, never returned

	// "Subsystem shutdown with live sequences -> no Layer-1 lifecycle rejection fires" (§9
	// R-S1f). Deinitialize() must complete without asserting/crashing even though LiveSeq is
	// still vended and mid-generation -- Deinitialize() releases every warm sequence and unmaps
	// the model in an order Layer 1 itself accepts (§4's own ordering discipline, mirrored from
	// R-S2d's GPU teardown-order cell for the CPU backend).
	Subsystem->Deinitialize();

	// Configure() is idempotent (SuperSLMRuntimeConfig.h) and tears down any prior state before
	// rebuilding -- a clean re-Configure() after Deinitialize() proves the teardown left the
	// runtime in a valid, reusable state rather than stuck.
	FSuperSLMImportDiagnostic Diag2;
	USuperSLMModel* Model2 = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag2);
	if (!TestNotNull(TEXT("A-CPU must re-import after shutdown"), Model2))
	{
		return false;
	}
	return TestEqual(TEXT("Configure() must succeed cleanly after Deinitialize() with a live sequence outstanding"),
		(uint8)Subsystem->Configure(Model2, Config).Result, (uint8)ESuperSLMConfigureResult::Success);
}

#endif // WITH_DEV_AUTOMATION_TESTS
