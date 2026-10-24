// Slice P1 (schema fast-decode plan rev 1.3.5, §6 "P1", cells §11.3 P1.1-P1.9): canonical schema
// jump-forward in the CPU subsystem. Every cell runs with the setting Canonical
// (FSuperSLMGenerationRequest::SchemaJumpForward) against A-EX under potion_shop_order.
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
	constexpr ESuperSLMSchemaJumpForward kCanon = ESuperSLMSchemaJumpForward::Canonical;

	// A config whose span budget splits A-EX's longer key chains: MaxPrefillChunkBudget bounds a
	// span job's chunk_budget, and potion_shop_order's key chains run past 3 tokens.
	FSuperSLMRuntimeConfig SplitConfig(int32 BlockCount = 4)
	{
		FSuperSLMRuntimeConfig C = BaseConfig(BlockCount, BlockCount);
		C.MaxPrefillChunkBudget = 3;
		C.MinPrefillChunk = 1;
		return C;
	}
}

// P1.1 Pricing, the call site. A span job of C tokens is priced by the production price function
// at C from the sequence's depth (C = 1 the chunk-1 pair, 2..7 the per-C table, >= 8 the batched
// pair); the function's own values, PrefillPriceBoundMs's table and the re-pricing of 2..7-token
// prompt chunks are the host cells (test_pricing.cpp). Red: verbs stubbed, seam unbuilt; once
// built, red until the span job is priced through that function (X15 prices it C x 33.583).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1PricingTest, "SuperSLM.SchemaJump.P1.Pricing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1PricingTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Run;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Req) || !RunSolo(*this, Rig, Req, Run)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1.1"))) { return false; }
	bool bOk = true;
	bool bSawSmall = false;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("job %lld: priced at a chunk in [1, chunk_budget] (%d of %d)"), S.JobId, S.PricedChunk, S.ChunkBudget),
			S.PricedChunk >= 1 && S.PricedChunk <= S.ChunkBudget);
		bOk &= TestTrue(TEXT("the depth is recorded"), S.StartDepth >= 0);
		const double Want = FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(S.PricedChunk, S.StartDepth, Rig.Config);
		bOk &= TestEqual(*FString::Printf(TEXT("job %lld: PlannedJobMs is the production price at C = %d, depth %lld"), S.JobId, S.PricedChunk, S.StartDepth),
			S.PlannedJobMs, Want);
		bSawSmall |= S.PricedChunk >= 2 && S.PricedChunk < FSuperSLMSchedulingTestAccess::TiledMinTokens();
	}
	bOk &= TestTrue(TEXT("precondition: at least one span job priced in the per-C range 2..7"), bSawSmall);
	return bOk;
}

// P1.2 Drain. A span budget that splits a chain: no decode job serves the sequence while the
// forced chain is non-empty (out_count > 0), and the tokens equal an unsplit run's. The plan's
// reference is "a headless run"; the plugin cannot reach the engine's headless loop, so the
// reference here is the same generation with a budget that does not split (chunk invariance,
// §5.6); the headless comparison is the box's (rev 1.3.5 adopts this reading). Span jobs order
// against decode rows by JobId alone, from one counter (rev 1.3.5). Red: verbs stubbed, seam
// unbuilt; once built, red under X25 (decode at a forced state) and XD6 (separate JobId counters).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1DrainTest, "SuperSLM.SchemaJump.P1.Drain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1DrainTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Whole;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Req) || !RunSolo(*this, Rig, Req, Whole)) { return false; }

	FCpuRig Split;
	if (!SetUpCpu(*this, W.GetTestWorld(), SplitConfig(), Split)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Split.Cpu);
	FRunResult Run;
	if (!RunSolo(*this, Split, Req, Run)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Split.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1.2"))) { return false; }
	const TArray<int64> Decodes = DecodeJobIdsOf(*Split.Cpu, Run.Sequence);

	bool bOk = true;
	int32 Splits = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		if (S.ForcedChainLeftAfter <= 0) { continue; }
		++Splits;
		int64 NextSpan = TNumericLimits<int64>::Max();
		for (const FSuperSLMSpanJobRecord& O : Spans) { if (O.JobId > S.JobId) { NextSpan = FMath::Min(NextSpan, O.JobId); } }
		int64 NextDecode = TNumericLimits<int64>::Max();
		for (int64 D : Decodes) { if (D > S.JobId) { NextDecode = FMath::Min(NextDecode, D); } }
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld left %d forced tokens: the sequence's next job is a span job (%lld), not a decode job (%lld)"),
			S.JobId, S.ForcedChainLeftAfter, NextSpan, NextDecode), NextSpan < NextDecode);
	}
	bOk &= TestTrue(TEXT("precondition: the budget split at least one chain"), Splits > 0);
	bOk &= CheckSharedJobIdCounter(*this, *Split.Cpu, Spans);
	bOk &= TestEqual(TEXT("the split run's tokens equal the unsplit run's"), Run.Tokens, Whole.Tokens);
	return bOk;
}

// P1.3 Stop. A jump-forward extraction ends SchemaDeadEnd with the walk accepting, and
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
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Run;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 512, Req) || !RunSolo(*this, Rig, Req, Run)) { return false; }
	if (!RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Run.Sequence), TEXT("P1.3"))) { return false; }
	bool bOk = TestEqual(TEXT("the extraction ends Faulted"), (uint8)Run.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("with SchemaDeadEnd"), (uint8)Run.Outcome, (uint8)ESuperSLMDecodeOutcome::SchemaDeadEnd);
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

// P1.4 MaxNewTokens. Forced tokens count toward MaxNewTokens (§16 Q12), and a span is clipped to
// the remaining allowance. The pending token already counts toward the generated total and rides in
// the span chunk, so the clip is on the new tokens the span adds, not the chunk length. A clip in
// the middle of a chain gives exactly MaxNewTokens tokens, a prefix of the full run. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1MaxNewTokensTest, "SuperSLM.SchemaJump.P1.MaxNewTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1MaxNewTokensTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Full;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 512, Req) || !RunSolo(*this, Rig, Req, Full)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Full.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1.4"))) { return false; }

	// Clip inside the longest chain the full run admitted in one span job.
	int32 Clip = -1;
	int32 Longest = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		if (S.ForcedIds.Num() >= 2 && S.ForcedIds.Num() > Longest)
		{
			Longest = S.ForcedIds.Num();
			Clip = S.OutputIndexBefore + S.ForcedIds.Num() / 2 + (S.ForcedIds.Num() > 2 ? 1 : 0);
		}
	}
	if (!TestTrue(TEXT("precondition: a span job admitted a chain of at least 2 tokens"), Clip > 0)) { return false; }

	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	Req.MaxNewTokens = Clip;
	FRunResult Cut;
	if (!RunSolo(*this, Rig, Req, Cut)) { return false; }
	bool bOk = TestEqual(TEXT("the clipped run emits exactly MaxNewTokens tokens"), Cut.Tokens.Num(), Clip);
	bOk &= TestTrue(TEXT("the clipped run is a prefix of the full run"), IsPrefixOf(Cut.Tokens, Full.Tokens));
	bOk &= TestEqual(TEXT("the clipped run ends Complete (MaxNewTokens)"), (uint8)Cut.Phase, (uint8)ESuperSLMSequencePhase::Complete);
	for (const FSuperSLMSpanJobRecord& S : SpanJobsOf(*Rig.Cpu, Cut.Sequence))
	{
		// The pending token already counts toward the generated total (it is in OutputIndexBefore),
		// and the span chunk carries it, so the chunk length is not the measure: the new tokens the
		// span adds must fit the allowance left.
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld: the new tokens it adds (%d) fit the allowance left (%d)"), S.JobId, S.ForcedIds.Num(), Clip - S.OutputIndexBefore),
			S.ForcedIds.Num() <= Clip - S.OutputIndexBefore);
	}
	return bOk;
}

// P1.5 Listeners. Each forced token reaches the output once, in order, and ForcedTokenCount equals
// the forced tokens the span jobs admitted. Read through GetGeneratedTokens() tick by tick: the CPU
// subsystem has no listener API, and rev 1.3.5 adopts this reading. Red: verbs stubbed, seam
// unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1ListenersTest, "SuperSLM.SchemaJump.P1.Listeners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1ListenersTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), SplitConfig(), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(1);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Runs[0].Request) || !RunConcurrently(*this, Rig, Runs)) { return false; }
	const FRunResult& Run = Runs[0].Result;
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1.5"))) { return false; }
	bool bOk = CheckAppendOnly(*this, Runs[0].Snapshots, Run.Tokens, TEXT("P1.5"));
	int64 Admitted = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		Admitted += S.ForcedIds.Num();
		// A canonical span job's out_forced are its forced ids, after the pending token it admitted.
		const int32 At = S.OutputIndexBefore;
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld: its ids lie inside the output"), S.JobId), At >= 0 && At + S.ForcedIds.Num() <= Run.Tokens.Num());
		if (At >= 0 && At + S.ForcedIds.Num() <= Run.Tokens.Num())
		{
			bOk &= TestEqual(*FString::Printf(TEXT("span job %lld: its ids appear once, in order, at output index %d"), S.JobId, At),
				TArray<int32>(Run.Tokens.GetData() + At, S.ForcedIds.Num()), S.ForcedIds);
		}
	}
	bOk &= TestEqual(TEXT("ForcedTokenCount equals the forced tokens the span jobs admitted"), Run.Stats.ForcedTokenCount, Admitted);
	return bOk;
}

// P1.6 GPU untouched. P1 has no GPU verb: a GPU sequence with the setting on is bit-equal to the
// setting off. This holds before P1 too; it is red only through LiveGate, and guards P1's change
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

	TArray<int32> Out[3];
	const ESuperSLMSchemaJumpForward Modes[3] = { ESuperSLMSchemaJumpForward::Off, ESuperSLMSchemaJumpForward::Canonical, ESuperSLMSchemaJumpForward::Exact };
	for (int32 I = 0; I < 3; ++I)
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
	bOk &= TestEqual(TEXT("GPU, Canonical: bit-equal to Off"), Out[1], Out[0]);
	bOk &= TestEqual(TEXT("GPU, Exact: bit-equal to Off (P1X.4's half)"), Out[2], Out[0]);
	return bOk;
}
#endif // SUPERSLMUNREAL_WITH_GPU

// P1.7 Mixed batch. One decode job holds a jump-forward sequence (drained), a jump-forward-off
// sequence and an unbound sequence; each equals its solo run. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1MixedBatchTest, "SuperSLM.SchemaJump.P1.MixedBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1MixedBatchTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(4, 3), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(3);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Runs[0].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[1], ESuperSLMSchemaJumpForward::Off, 96, Runs[1].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[2], ESuperSLMSchemaJumpForward::Off, 48, Runs[2].Request)) { return false; }
	Runs[2].bUnbound = true;
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true, /*bGatherAtDecode*/ true)) { return false; }
	bool bOk = RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[0].Result.Sequence), TEXT("P1.7 jump-forward sequence"));
	bOk &= TestTrue(TEXT("precondition: one decode job served all three sequences"),
		SomeDecodeJobServedAll(*Rig.Cpu, { Runs[0].Result.Sequence, Runs[1].Result.Sequence, Runs[2].Result.Sequence }));
	for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	const TCHAR* Names[3] = { TEXT("jump-forward"), TEXT("off"), TEXT("unbound") };
	for (int32 I = 0; I < 3; ++I)
	{
		FRunResult Solo;
		if (!RunSolo(*this, Rig, Runs[I].Request, Solo, Runs[I].bUnbound)) { return false; }
		bOk &= SameEnding(*this, Runs[I].Result, Solo, FString::Printf(TEXT("P1.7 %s sequence equals its solo run"), Names[I]));
	}
	return bOk;
}

// P1.8 Cancel. Release between two span jobs of one chain: no crash, and the sequence returns to
// the pool clean (the next generation on the recycled slot equals a fresh one). Red: verbs
// stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1CancelTest, "SuperSLM.SchemaJump.P1.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1CancelTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	// One slot, so the released sequence's slot is the one the next generation runs on.
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), SplitConfig(1), Rig)) { return false; }
	FSuperSLMGenerationRequest Off;
	FRunResult Fresh;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[1], ESuperSLMSchemaJumpForward::Off, 64, Off) || !RunSolo(*this, Rig, Off, Fresh)) { return false; }

	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Req)) { return false; }
	FSuperSLMSequence Seq;
	FString Error;
	if (!TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success) ||
		!TestTrue(*Error, Rig.Cpu->SetSchema(Seq, Rig.Schema, Error)) ||
		!TestTrue(*Error, Rig.Cpu->BeginGeneration(Seq, Req, Error))) { return false; }
	// Hold after each span delivery (the job hold), so the gap stays open between ticks; a hold whose span left
	// no chain is released, and the release below lands between a span job and the chain's next one.
	FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, Seq, ESuperSLMJobHoldPoint::AfterSpanDelivery);
	// Tick until a span job has delivered with chain left over: the release lands between it and
	// the chain's next span job.
	bool bBetween = false;
	const double Start = FPlatformTime::Seconds();
	while (!bBetween && !IsEnded(*Rig.Cpu, Seq) && FPlatformTime::Seconds() - Start < kRunSeconds)
	{
		Rig.Cpu->Tick(1.0f / 60.0f);
		const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Seq);
		if (Spans.Num() > 0 && Spans.Last().DeliveredAtTick >= 0 && Spans.Last().ForcedChainLeftAfter > 0) { bBetween = true; }
		if (!bBetween && FSuperSLMSchemaJumpTestAccess::IsJobHeld(*Rig.Cpu, Seq)) { FSuperSLMSchemaJumpTestAccess::ReleaseJobHold(*Rig.Cpu, Seq); }
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("precondition: reached the gap between two span jobs of one chain"), bBetween)) { return false; }
	Rig.Cpu->ReturnSequence(Seq);
	Settle(*Rig.Cpu);
	FRunResult After;
	if (!RunSolo(*this, Rig, Off, After)) { return false; }
	return SameEnding(*this, After, Fresh, TEXT("P1.8: after the release, a generation on the pool equals a fresh one"));
}

// P1.9 The setting and the switch (rev 1.3.5). The setting is read per generation from
// FSuperSLMGenerationRequest, default Off: two generations in one decode job with different
// settings each follow their own. The switch's values, case and refusals are host cells
// (switch_parses_off_exact_canonical, switch_refuses_other_values_and_leaves_the_mode; mutant
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
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(4, 2), Rig)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	// The same prompt twice, one Canonical and one Off, begun together.
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(2);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 96, Runs[0].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, 96, Runs[1].Request)) { return false; }
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true)) { return false; }
	bOk &= TestTrue(TEXT("precondition: one decode job served both"),
		SomeDecodeJobServedAll(*Rig.Cpu, { Runs[0].Result.Sequence, Runs[1].Result.Sequence }));
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[0].Result.Sequence), TEXT("P1.9 canonical generation"));
	bOk &= TestEqual(TEXT("the Off generation ran no span job"), SpanJobsOf(*Rig.Cpu, Runs[1].Result.Sequence).Num(), 0);
	for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	FRunResult OffSolo;
	if (!RunSolo(*this, Rig, Runs[1].Request, OffSolo)) { return false; }
	bOk &= SameEnding(*this, Runs[1].Result, OffSolo, TEXT("P1.9: the Off generation equals its solo run"));
	FRunResult CanonSolo;
	if (!RunSolo(*this, Rig, Runs[0].Request, CanonSolo)) { return false; }
	bOk &= SameEnding(*this, Runs[0].Result, CanonSolo, TEXT("P1.9: the Canonical generation equals its solo run"));
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
