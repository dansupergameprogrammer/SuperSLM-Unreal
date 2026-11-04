// Slice P1 (schema fast-decode plan rev 1.3.5, §6 "P1", cells §11.3 P1.1-P1.9): schema jump-forward in the CPU
// subsystem, against A-EX under potion_shop_order. P1 was written for Canonical, which plugin 1.4 removes (E-11;
// ruling 40, red-suite-mr6.md). Under ruling 23's rule the Canonical-only cells are retired: P1.1 Pricing, P1.2
// Drain, P1.4 MaxNewTokens, P1.5 Listeners and P1.8 Cancel, whose shared halves are Exact's own cells (P1X.3,
// P1X.2, P1X.5, P1X.2 and P1X.6). P1.3, P1.7 and P1.9 are re-pointed at Exact over a table seeded from the Off
// population (as P1X seeds it); P1.6 keeps its Off and Exact legs.
//
// Every cell opens with LiveGate(), so with the engine verbs stubbed or the seam unbuilt each fails by
// name ("the engine verbs are red stubs", "the seam is not built").
// The host suite (ci/host/schema_jump) carries the pure parts of P1.1 (the pricing functions,
// mutant X15) and of P1.2 (the drain rule, mutant X25); these cells check the call sites.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "SuperSLMPromptResult.h"
#include "Tests/AutomationCommon.h"

#if SUPERSLMUNREAL_WITH_GPU
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSubsystem.h"
#endif

using namespace SuperSLMSchemaJumpFixtures;

namespace
{
	constexpr ESuperSLMSchemaJumpForward kExact = ESuperSLMSchemaJumpForward::Exact;

	// Exact drafts only from a learned table. The walk table is loaded (the plan's ruling 4), the population runs Off,
	// and its streams seed the draft table, as the P1X cells seed it. A cell calls this after Configure() and before it
	// starts recording.
	bool SeedFromOff(FAutomationTestBase& T, const FCpuRig& Rig)
	{
		if (!LoadWalkTableForSeeding(T, Rig)) { return false; }
		TArray<TArray<int32>> Streams;
		for (const FString& P : PopulationPrompts())
		{
			FSuperSLMGenerationRequest Req;
			FRunResult R;
			if (!MakeRequest(T, Rig, P, ESuperSLMSchemaJumpForward::Off, 96, Req) || !RunSolo(T, Rig, Req, R)) { return false; }
			Streams.Add(R.Tokens);
		}
		return T.TestTrue(TEXT("the draft table is seeded from the Off population"),
			FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams));
	}
}

// P1.3 Stop (ruling 40: re-pointed from Canonical at Exact over a seeded table). An Exact extraction ends
// Complete / SchemaComplete with the walk accepting (plugin 1.5.0: the schema's end is a clean stop), and
// ComposeStopReason gives Completed (§6 "Stop reporting": the final finish returns -2 with no layer
// pass). The example project's IsSequenceTerminal is outside the plugin; it reads the same two
// facts, which this cell pins (rev 1.3.5 adopts this reading). Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1StopTest, "SuperSLM.SchemaJump.P1.Stop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1StopTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig) || !SeedFromOff(*this, Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Run;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 512, Req) || !RunSolo(*this, Rig, Req, Run)) { return false; }
	if (!RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Run.Sequence), TEXT("P1.3"))) { return false; }
	bool bOk = TestEqual(TEXT("the extraction ends Complete"), (uint8)Run.Phase, (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= TestEqual(TEXT("with SchemaComplete"), (uint8)Run.Outcome, (uint8)ESuperSLMDecodeOutcome::SchemaComplete);
	bOk &= TestTrue(*FString::Printf(TEXT("before MaxNewTokens (%d of %d tokens)"), Run.Tokens.Num(), Req.MaxNewTokens), Run.Tokens.Num() < Req.MaxNewTokens);
	bOk &= TestTrue(TEXT("with the walk accepting"), Run.Stats.bSchemaAccepting);
	SuperSLMPromptResult::FStopFacts Facts;
	Facts.Phase = Run.Phase;
	Facts.LastOutcome = Run.Outcome;
	Facts.bSchemaAccepting = Run.Stats.bSchemaAccepting;
	ESuperSLMQueryStopReason Reason = ESuperSLMQueryStopReason::SchemaRejected;
	bOk &= TestTrue(TEXT("ComposeStopReason composes"), SuperSLMPromptResult::ComposeStopReason(Facts, Reason));
	bOk &= TestEqual(TEXT("ComposeStopReason gives Completed"), (uint8)Reason, (uint8)ESuperSLMQueryStopReason::Completed);
	return bOk;
}

// P1.6 GPU untouched. P1 has no GPU verb: a GPU sequence with the setting on is bit-equal to the
// setting off. Ruling 40: the Canonical leg is retired; GPU Exact (multi-row in plugin 1.4, MR6) still gives Off's
// tokens. This holds before P1 too; it is red only through LiveGate, and guards P1's change
// to FSuperSLMGenerationRequest against reaching the GPU path.
#if SUPERSLMUNREAL_WITH_GPU
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1GpuUntouchedTest, "SuperSLM.SchemaJump.P1.GpuUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1GpuUntouchedTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Error;
	if (!TestTrue(*Error, SuperSLML2S2Fixtures::TryGetAExArtifactPath(Path, Error))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(TEXT("A-EX"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(W.GetTestWorld());
	USuperSLMSubsystem* Cpu = SuperSLML2S2Fixtures::GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu) || !TestNotNull(TEXT("CPU subsystem (tokenizer)"), Cpu)) { return false; }
	if (!TestEqual(TEXT("CPU Configure (tokenizer)"), (uint8)Cpu->Configure(Model, BaseConfig(1, 1)).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 2;
	Config.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(SuperSLML2S2Fixtures::AExNumHiddenLayers);
	Config.K = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuSchemaHandle Schema;
	if (!TestTrue(*Error, FSuperSLMGpuSchemaLookup::LookupByName(*Model, SuperSLML2S2Fixtures::DemoSchemaName(), Schema, Error))) { return false; }

	TArray<int32> Out[2];
	const ESuperSLMSchemaJumpForward Modes[2] = { ESuperSLMSchemaJumpForward::Off, ESuperSLMSchemaJumpForward::Exact };
	for (int32 I = 0; I < 2; ++I)
	{
		FSuperSLMGenerationRequest Req;
		if (!TestTrue(TEXT("tokenize"), Cpu->Tokenize(SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(), Req.PromptTokens))) { return false; }
		Req.MaxNewTokens = 96;
		Req.SchemaJumpForward = Modes[I];
		FSuperSLMGpuSequence Seq;
		if (!TestEqual(TEXT("GPU vend"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success) ||
			!TestTrue(*Error, Gpu->SetSchema(Seq, Schema, Error))) { return false; }
		if (!TestTrue(*FString::Printf(TEXT("GPU run %s (%s)"), ModeName(Modes[I]), *Error),
			RunGpuToSchemaEnd(*Gpu, Seq, Req, Out[I], kRunSeconds, Error))) { return false; }
		Gpu->ReturnSequence(Seq);
	}
	bool bOk = TestTrue(TEXT("precondition: the GPU run produced tokens"), Out[0].Num() > 0);
	bOk &= TestEqual(TEXT("GPU, Exact: bit-equal to Off (P1X.4's half)"), Out[1], Out[0]);
	return bOk;
}
#endif // SUPERSLMUNREAL_WITH_GPU

// P1.7 Mixed batch (ruling 40: the jump-forward member is Exact over a seeded table, not Canonical). One decode job
// holds an Exact sequence, a jump-forward-off sequence and an unbound sequence; each equals its solo run. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1MixedBatchTest, "SuperSLM.SchemaJump.P1.MixedBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1MixedBatchTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(4, 3), Rig) || !SeedFromOff(*this, Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(3);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 96, Runs[0].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[1], ESuperSLMSchemaJumpForward::Off, 96, Runs[1].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[2], ESuperSLMSchemaJumpForward::Off, 48, Runs[2].Request)) { return false; }
	Runs[2].bUnbound = true;
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true, /*bGatherAtDecode*/ true)) { return false; }
	bool bOk = RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[0].Result.Sequence), TEXT("P1.7 Exact sequence"));
	bOk &= TestTrue(TEXT("precondition: one decode job served all three sequences"),
		SomeDecodeJobServedAll(*Rig.Cpu, { Runs[0].Result.Sequence, Runs[1].Result.Sequence, Runs[2].Result.Sequence }));
	for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	const TCHAR* Names[3] = { TEXT("Exact"), TEXT("off"), TEXT("unbound") };
	for (int32 I = 0; I < 3; ++I)
	{
		FRunResult Solo;
		if (!RunSolo(*this, Rig, Runs[I].Request, Solo, Runs[I].bUnbound)) { return false; }
		bOk &= SameEnding(*this, Runs[I].Result, Solo, FString::Printf(TEXT("P1.7 %s sequence equals its solo run"), Names[I]));
	}
	return bOk;
}

// P1.9 The setting and the switch (rev 1.3.5; ruling 40: the jump-forward member is Exact over a seeded table). The setting is read per generation from
// FSuperSLMGenerationRequest, default Off: two generations in one decode job with different
// settings each follow their own. The switch's values, case and refusals are host cells
// (switch_parses_off_and_exact, switch_refuses_canonical_and_falls_back_to_off,
// switch_refuses_other_values_and_leaves_the_mode; mutant
// XD7); the switch itself lives in the example project. "With SUPERSLM_WITH_SCHEMA_JUMP_FORWARD =
// 0 the jump-forward cells are gated off" is LiveGate itself. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1SettingPerGenerationTest, "SuperSLM.SchemaJump.P1.SettingPerGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1SettingPerGenerationTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	bool bOk = TestEqual(TEXT("a request's setting defaults to Off"), (uint8)FSuperSLMGenerationRequest().SchemaJumpForward,
		(uint8)ESuperSLMSchemaJumpForward::Off);
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(4, 2), Rig) || !SeedFromOff(*this, Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	// The same prompt twice, one Exact and one Off, begun together.
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(2);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 96, Runs[0].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, 96, Runs[1].Request)) { return false; }
	// Gathered at decode, as P1.7: without the hold, whether the two land in one decode job is tick timing.
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true, /*bGatherAtDecode*/ true)) { return false; }
	bOk &= TestTrue(TEXT("precondition: one decode job served both"),
		SomeDecodeJobServedAll(*Rig.Cpu, { Runs[0].Result.Sequence, Runs[1].Result.Sequence }));
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[0].Result.Sequence), TEXT("P1.9 Exact generation"));
	bOk &= TestEqual(TEXT("the Off generation ran no span job"), SpanJobsOf(*Rig.Cpu, Runs[1].Result.Sequence).Num(), 0);
	for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	FRunResult OffSolo;
	if (!RunSolo(*this, Rig, Runs[1].Request, OffSolo)) { return false; }
	bOk &= SameEnding(*this, Runs[1].Result, OffSolo, TEXT("P1.9: the Off generation equals its solo run"));
	FRunResult ExactSolo;
	if (!RunSolo(*this, Rig, Runs[0].Request, ExactSolo)) { return false; }
	bOk &= SameEnding(*this, Runs[0].Result, ExactSolo, TEXT("P1.9: the Exact generation equals its solo run"));
	bOk &= SameEnding(*this, Runs[0].Result, OffSolo, TEXT("P1.9: the Exact generation equals Off's"));
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
