// Slice P1-X (schema fast-decode plan rev 1.3.5, §6 "P1-X", §5.10; cells §11.3 P1X.1-P1X.12): exact
// mode in the CPU subsystem, with the D2-online learned draft. Every cell runs A-EX under
// potion_shop_order. Exact mode's whole promise is "the same tokens as Off", so every cell's
// reference is the Off run, and every cell first requires span activity (RequireSpanActivity) so
// an unimplemented setting -- which runs off's loop -- cannot pass.
//
// Every cell opens with LiveGate(). The host suite
// (ci/host/schema_jump) carries the pure parts: the draft table (build, the 0.5 gate, the stops,
// the clip, the snapshot swap, the bound, the drop at unmap, the merge), observed forcedness and
// what the table learns (P1X.12 (a) to (d), against B7's d2-table.txt), the three-way setting and
// the span-after-decode loop; P1X.8's TSan half is host-only (no plugin TSan build exists).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "Tests/AutomationCommon.h"

THIRD_PARTY_INCLUDES_START
#include "superslm/sslm_abi.h"
THIRD_PARTY_INCLUDES_END

#if SUPERSLMUNREAL_WITH_GPU
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSubsystem.h"
#endif

using namespace SuperSLMSchemaJumpFixtures;

namespace
{
	constexpr ESuperSLMSchemaJumpForward kExact = ESuperSLMSchemaJumpForward::Exact;
	constexpr int32 kThreadedDraftCap = 6;

	// Off runs of the whole population, the reference every exact cell compares to.
	bool OffPopulation(FAutomationTestBase& T, const FCpuRig& Rig, int32 MaxNewTokens, TArray<FRunResult>& Out, TArray<FSuperSLMGenerationRequest>& Requests)
	{
		Out.Reset();
		Requests.Reset();
		for (const FString& P : PopulationPrompts())
		{
			FSuperSLMGenerationRequest Req;
			FRunResult R;
			if (!MakeRequest(T, Rig, P, ESuperSLMSchemaJumpForward::Off, MaxNewTokens, Req) || !RunSolo(T, Rig, Req, R)) { return false; }
			Requests.Add(Req);
			Out.Add(R);
		}
		return true;
	}

	// The population in exact mode, solo, each compared with its off run. Returns the span jobs.
	bool ExactPopulationEqualsOff(FAutomationTestBase& T, const FCpuRig& Rig, const TArray<FSuperSLMGenerationRequest>& Requests,
		const TArray<FRunResult>& Off, const TCHAR* Table, TArray<FSuperSLMSpanJobRecord>& OutSpans)
	{
		bool bOk = true;
		OutSpans.Reset();
		for (int32 I = 0; I < Requests.Num(); ++I)
		{
			FSuperSLMGenerationRequest Req = Requests[I];
			Req.SchemaJumpForward = kExact;
			FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
			FRunResult R;
			if (!RunSolo(T, Rig, Req, R)) { return false; }
			OutSpans.Append(SpanJobsOf(*Rig.Cpu, R.Sequence));
			bOk &= SameEnding(T, R, Off[I], FString::Printf(TEXT("%s table, prompt %d: exact equals off"), Table, I));
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s table, prompt %d: ForcedTokenCount equals off's (§16 Q21)"), Table, I),
				R.Stats.ForcedTokenCount, Off[I].Stats.ForcedTokenCount);
		}
		for (const FSuperSLMSpanJobRecord& S : OutSpans)
		{
			// NULL means the canonical chain (sslm_jump_options): an exact span job never passes it,
			// not even with an empty learned draft (cold start).
			bOk &= T.TestFalse(*FString::Printf(TEXT("%s table, span job %lld: the draft pointer is not NULL"), Table, S.JobId), S.bDraftNull);
			// Rev 1.3.5: an empty draft sends no span job, so every span job carries a token (XD2).
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s table, span job %lld: the draft is not empty"), Table, S.JobId), S.Draft.Num() >= 1);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s table, span job %lld: mode Exact"), Table, S.JobId), (uint8)S.Mode, (uint8)kExact);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s table, span job %lld: the draft fits chunk_budget - 1"), Table, S.JobId), S.Draft.Num() <= S.ChunkBudget - 1);
		}
		return bOk;
	}

	int32 DraftedTokens(const TArray<FSuperSLMSpanJobRecord>& Spans)
	{
		int32 N = 0;
		for (const FSuperSLMSpanJobRecord& S : Spans) { N += S.Draft.Num(); }
		return N;
	}

	TArray<TArray<int32>> Streams(const TArray<FRunResult>& Runs)
	{
		TArray<TArray<int32>> Out;
		for (const FRunResult& R : Runs) { Out.Add(R.Tokens); }
		return Out;
	}
}

// P1X.1 The draft never changes a token. The population with an empty table, a learned table, a
// seeded table and a deliberately corrupted table: the tokens are identical, and equal Off. Rev
// 1.3.5: no exact span job passes a NULL or an empty draft, and with an empty table (cold start) no
// span job is issued; the decode job embeds the pending token. Red: verbs stubbed, seam unbuilt;
// once built, red under XE18 (a drafted token emitted unconfirmed) and XD2 (a cold-start span job).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XDraftNeverChangesATokenTest, "SuperSLM.SchemaJump.P1X.DraftNeverChangesAToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XDraftNeverChangesATokenTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }

	bool bOk = true;
	TArray<FSuperSLMSpanJobRecord> Spans;

	// Empty: the cold start. Learning is on, so the table is cleared before every prompt: each
	// prompt runs with no table, sends no span job, and equals off.
	for (int32 I = 0; I < Requests.Num(); ++I)
	{
		FSuperSLMSchemaJumpTestAccess::ClearDraftTable(*Rig.Cpu, Rig.Schema);
		bOk &= TestFalse(TEXT("empty: no table before the prompt"), FSuperSLMSchemaJumpTestAccess::HasDraftTable(*Rig.Cpu, Rig.Schema));
		TArray<FSuperSLMGenerationRequest> One = { Requests[I] };
		TArray<FRunResult> OneOff = { Off[I] };
		TArray<FSuperSLMSpanJobRecord> OneSpans;
		bOk &= ExactPopulationEqualsOff(*this, Rig, One, OneOff, TEXT("empty"), OneSpans);
		bOk &= TestEqual(*FString::Printf(TEXT("empty, prompt %d: cold start sends no span job"), I), OneSpans.Num(), 0);
	}

	// Learned: one exact pass from an empty table fills it (each finished, accepting stream feeds
	// it); the second pass drafts from what the first learned.
	FSuperSLMSchemaJumpTestAccess::ClearDraftTable(*Rig.Cpu, Rig.Schema);
	bOk &= ExactPopulationEqualsOff(*this, Rig, Requests, Off, TEXT("learning"), Spans);
	bOk &= TestTrue(TEXT("learned: the pass filled a table"), FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(*Rig.Cpu, Rig.Schema) > 0);
	bOk &= ExactPopulationEqualsOff(*this, Rig, Requests, Off, TEXT("learned"), Spans);
	bOk &= RequireSpanActivity(*this, Spans, TEXT("P1X.1 learned table"));
	bOk &= TestTrue(TEXT("learned: the drafts carried tokens"), DraftedTokens(Spans) > 0);

	// Seeded: the shipped-table path, from the off streams.
	bOk &= TestTrue(TEXT("seeded: the seed is taken"), FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off)));
	bOk &= ExactPopulationEqualsOff(*this, Rig, Requests, Off, TEXT("seeded"), Spans);
	bOk &= TestTrue(TEXT("seeded: the drafts carried tokens"), DraftedTokens(Spans) > 0);

	// Corrupted: a legal non-leader token dominates every multi-token state, so drafts go wrong there
	// and the verb rejects the rest.
	bOk &= TestTrue(TEXT("seeded again before corrupting"), FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off)));
	bOk &= TestTrue(TEXT("corrupted: the corruption is taken"), FSuperSLMSchemaJumpTestAccess::CorruptDraftTable(*Rig.Cpu, Rig.Schema));
	// Learning would repair the table between prompts, so each prompt runs on a fresh corruption.
	for (int32 I = 0; I < Requests.Num(); ++I)
	{
		FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
		FSuperSLMSchemaJumpTestAccess::CorruptDraftTable(*Rig.Cpu, Rig.Schema);
		TArray<FSuperSLMGenerationRequest> One = { Requests[I] };
		TArray<FRunResult> OneOff = { Off[I] };
		TArray<FSuperSLMSpanJobRecord> OneSpans;
		bOk &= ExactPopulationEqualsOff(*this, Rig, One, OneOff, TEXT("corrupted"), OneSpans);
		Spans.Append(OneSpans);
	}
	int32 Rejected = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans) { Rejected += (S.Draft.Num() > 0 && S.ForcedIds.Num() < 1 + S.Draft.Num()) ? 1 : 0; }
	bOk &= TestTrue(TEXT("corrupted: precondition: the verb rejected drafted tokens"), Rejected > 0);
	return bOk;
}

// P1X.2 The loop. A span job follows decode jobs only (never two in a row for one sequence), and
// carries a non-empty draft; no decode batch includes a sequence while its span job is
// outstanding; the output is append-only; ForcedTokenCount equals off's; span jobs and decode rows
// share one JobId counter. Rev 1.3.5 adopts the re-scope: "a span job follows each decode job that
// leaves a forced pending token, when the draft is non-empty" needs the walk state at each decode,
// so its full form is the host loop cells' (NextJob); here it is "span jobs run, each after a
// decode job, none overlapping a decode". Red: verbs stubbed, seam unbuilt; once built, red under
// XE18, X25's exact twin, XD2 and XD6.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XLoopTest, "SuperSLM.SchemaJump.P1X.Loop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XLoopTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);

	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(1);
	Runs[0].Request = Requests[0];
	Runs[0].Request.SchemaJumpForward = kExact;
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true)) { return false; }
	const FRunResult& Run = Runs[0].Result;
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1X.2"))) { return false; }

	bool bOk = SameEnding(*this, Run, Off[0], TEXT("P1X.2: exact equals off"));
	bOk &= CheckAppendOnly(*this, Runs[0].Snapshots, Run.Tokens, TEXT("P1X.2"));
	bOk &= TestEqual(TEXT("ForcedTokenCount equals off's"), Run.Stats.ForcedTokenCount, Off[0].Stats.ForcedTokenCount);
	bOk &= CheckSharedJobIdCounter(*this, *Rig.Cpu, Spans);
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld carries a non-NULL, non-empty draft"), S.JobId), !S.bDraftNull && S.Draft.Num() >= 1);
	}

	TArray<FSuperSLMWorkerJobReport> Decodes;
	for (const FSuperSLMWorkerJobReport& R : Rig.Cpu->GetJobLedger())
	{
		if (R.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && R.MemberSequences.Contains(Run.Sequence)) { Decodes.Add(R); }
	}
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		// The job just before it, among this sequence's span and decode jobs, is a decode job.
		int64 PrevSpan = -1;
		for (const FSuperSLMSpanJobRecord& O : Spans) { if (O.JobId < S.JobId) { PrevSpan = FMath::Max(PrevSpan, O.JobId); } }
		int64 PrevDecode = -1;
		for (const FSuperSLMWorkerJobReport& D : Decodes) { if (D.JobId < S.JobId) { PrevDecode = FMath::Max(PrevDecode, D.JobId); } }
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld follows a decode job (%lld), not a span job (%lld)"), S.JobId, PrevDecode, PrevSpan),
			PrevDecode > PrevSpan);
		// No decode job serving the sequence is planned while the span job is outstanding.
		for (const FSuperSLMWorkerJobReport& D : Decodes)
		{
			bOk &= TestFalse(*FString::Printf(TEXT("decode job %lld is not planned while span job %lld is outstanding (ticks %d..%d)"),
				D.JobId, S.JobId, S.PlannedAtTick, S.DeliveredAtTick),
				D.JobId > S.JobId && D.PlannedAtTick < S.DeliveredAtTick);
		}
	}
	Rig.Cpu->ReturnSequence(Run.Sequence);
	Settle(*Rig.Cpu);
	return bOk;
}

// P1X.3 Pricing. A span job is priced at its full drafted M (the pending token and the draft) with
// the per-C table; the decode job after it is a finish-only job. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XPricingTest, "SuperSLM.SchemaJump.P1X.Pricing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XPricingTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req = Requests[0];
	Req.SchemaJumpForward = kExact;
	FRunResult Run;
	if (!RunSolo(*this, Rig, Req, Run, false, /*bKeep*/ true)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1X.3"))) { return false; }
	const TArray<FSuperSLMWorkerJobReport> Ledger = Rig.Cpu->GetJobLedger();
	const FSuperSLMResolvedCpuConfig* Resolved = Rig.Cpu->GetResolvedConfig();
	if (!TestNotNull(TEXT("resolved config"), Resolved)) { return false; }
	bool bOk = true;
	bool bSawDraft = false;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		bSawDraft |= S.Draft.Num() > 0;
		bOk &= TestEqual(*FString::Printf(TEXT("span job %lld: priced at the full drafted M = 1 + %d"), S.JobId, S.Draft.Num()), S.PricedChunk, 1 + S.Draft.Num());
		bOk &= TestEqual(*FString::Printf(TEXT("span job %lld: PlannedJobMs is the production price at M"), S.JobId),
			S.PlannedJobMs, FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(S.PricedChunk, S.StartDepth, Rig.Config));
		// The sequence's next decode job: a finish only.
		const FSuperSLMWorkerJobReport* Next = nullptr;
		for (const FSuperSLMWorkerJobReport& D : Ledger)
		{
			if (D.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && D.JobId > S.JobId && D.MemberSequences.Contains(Run.Sequence) && (!Next || D.JobId < Next->JobId)) { Next = &D; }
		}
		if (!TestNotNull(*FString::Printf(TEXT("span job %lld: a decode job follows"), S.JobId), Next)) { bOk = false; continue; }
		bOk &= TestEqual(*FString::Printf(TEXT("decode job %lld after span job %lld: no layer pass"), Next->JobId, S.JobId), Next->DecodeLayers, 0);
		bOk &= TestEqual(*FString::Printf(TEXT("decode job %lld: one finish"), Next->JobId), Next->TokenFinishes, 1);
		bOk &= TestEqual(*FString::Printf(TEXT("decode job %lld: priced as a finish"), Next->JobId), Next->PlannedJobMs, Resolved->FinishCostMs);
	}
	bOk &= TestTrue(TEXT("precondition: a span job carried a draft"), bSawDraft);
	Rig.Cpu->ReturnSequence(Run.Sequence);
	Settle(*Rig.Cpu);
	return bOk;
}

// P1X.4 The packaged check with exact mode on: the CPU and GPU digests are equal, the GPU running
// off. The packaged run itself is the box's (B6); this editor cell is its token-level half: CPU
// exact equals GPU at the pinned self-check prompt. Red: verbs stubbed, seam unbuilt.
#if SUPERSLMUNREAL_WITH_GPU
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XCpuExactEqualsGpuTest, "SuperSLM.SchemaJump.P1X.CpuExactEqualsGpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XCpuExactEqualsGpuTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	{
		// The learned draft, seeded from this prompt's off run (ruling 4), as the other exact cells seed it.
		FSuperSLMGenerationRequest OffReq;
		FRunResult OffRun;
		if (!MakeRequest(*this, Rig, SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(), ESuperSLMSchemaJumpForward::Off, 96, OffReq) ||
			!RunSolo(*this, Rig, OffReq, OffRun)) { return false; }
		FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { OffRun.Tokens });
	}
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	if (!MakeRequest(*this, Rig, SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(), kExact, 96, Req) || !RunSolo(*this, Rig, Req, Cpu)) { return false; }
	if (!RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Cpu.Sequence), TEXT("P1X.4"))) { return false; }

	FString Error;
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 2;
	Config.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(SuperSLML2S2Fixtures::AExNumHiddenLayers);
	Config.K = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure"), (uint8)Gpu->Configure(Rig.Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuSchemaHandle Schema;
	if (!TestTrue(*Error, FSuperSLMGpuSchemaLookup::LookupByName(*Rig.Model, SuperSLML2S2Fixtures::DemoSchemaName(), Schema, Error))) { return false; }
	FSuperSLMGpuSequence Seq;
	TArray<int32> GpuTokens;
	if (!TestEqual(TEXT("GPU vend"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success) ||
		!TestTrue(*Error, Gpu->SetSchema(Seq, Schema, Error)) ||
		!TestTrue(*Error, RunGpuToSchemaEnd(*Gpu, Seq, Req, GpuTokens, kRunSeconds, Error))) { return false; }
	Gpu->ReturnSequence(Seq);
	return TestEqual(TEXT("CPU exact equals the GPU (which ignores the setting and runs off)"), Cpu.Tokens, GpuTokens);
}
#endif // SUPERSLMUNREAL_WITH_GPU

namespace
{
	// Saves Seq and reads the blob (the sequence stays vended), as the review cells do.
	bool SaveBlobP1X(FAutomationTestBase& T, const SuperSLMSchemaJumpFixtures::FCpuRig& Rig, const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob)
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		if (!T.TestEqual(*FString::Printf(TEXT("SaveSequence queues (%s)"), *Error), (uint8)Rig.Cpu->SaveSequence(Seq, Handle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { Result = Rig.Cpu->GetSaveResult(Handle, OutBlob); return Result != ESuperSLMRestoreResult::Pending; }, 60.0);
		Rig.Cpu->ReleaseLifecycleOpHandle(Handle);
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the blob has bytes"), OutBlob.Num() > 0);
	}
}

// P1X.5 MaxNewTokens. The draft is clipped to the allowance, and a truncated run is a prefix of
// off. The pending token already counts toward the generated total, so the clip applies to the
// new tokens a span adds, not to its chunk length. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XMaxNewTokensTest, "SuperSLM.SchemaJump.P1X.MaxNewTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XMaxNewTokensTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 256, Off, Requests)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req = Requests[0];
	Req.SchemaJumpForward = kExact;
	FRunResult Full;
	if (!RunSolo(*this, Rig, Req, Full)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Full.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1X.5"))) { return false; }
	// Clip inside the longest confirmed span.
	int32 Clip = -1;
	int32 Longest = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		if (S.ForcedIds.Num() >= 2 && S.ForcedIds.Num() > Longest)
		{
			Longest = S.ForcedIds.Num();
			// The pending token is already in OutputIndexBefore; the confirmed ids follow it.
			Clip = S.OutputIndexBefore + (S.ForcedIds.Num() + 1) / 2;
		}
	}
	if (!TestTrue(TEXT("precondition: a span job confirmed at least 2 tokens"), Clip > 0)) { return false; }

	// Off cut at the same MaxNewTokens, kept for its save.
	FSuperSLMGenerationRequest OffCutReq = Req;
	OffCutReq.SchemaJumpForward = ESuperSLMSchemaJumpForward::Off;
	OffCutReq.MaxNewTokens = Clip;
	FRunResult OffCut;
	TArray<uint8> OffCutBlob, CutBlob;
	if (!RunSolo(*this, Rig, OffCutReq, OffCut, /*bUnbound*/ false, /*bKeep*/ true) || !SaveBlobP1X(*this, Rig, OffCut.Sequence, OffCutBlob)) { return false; }
	Rig.Cpu->ReturnSequence(OffCut.Sequence);
	Settle(*Rig.Cpu);

	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	Req.MaxNewTokens = Clip;
	FRunResult Cut;
	if (!RunSolo(*this, Rig, Req, Cut, /*bUnbound*/ false, /*bKeep*/ true) || !SaveBlobP1X(*this, Rig, Cut.Sequence, CutBlob)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> CutSpans = SpanJobsOf(*Rig.Cpu, Cut.Sequence);
	Rig.Cpu->ReturnSequence(Cut.Sequence);
	Settle(*Rig.Cpu);
	bool bOk = TestEqual(TEXT("the clipped run emits exactly MaxNewTokens tokens"), Cut.Tokens.Num(), Clip);
	bOk &= TestTrue(TEXT("the clipped run is a prefix of off"), IsPrefixOf(Cut.Tokens, Off[0].Tokens));
	bOk &= SameEnding(*this, Cut, OffCut, TEXT("exact cut at MaxNewTokens equals off cut there"));
	bOk &= TestTrue(TEXT("the save at the cut is byte-identical to off's"), CutBlob == OffCutBlob);
	int32 Clipped = 0;
	for (const FSuperSLMSpanJobRecord& S : CutSpans)
	{
		Clipped += S.DraftLengthBeforeAllowanceClip > S.DraftLengthBeforeStopCut ? 1 : 0;
		// The pending token already counts toward the generated total (it is in OutputIndexBefore),
		// and the span chunk carries it: what must fit the allowance is the new tokens the span adds.
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld: the draft (%d) fits the allowance left (%d)"), S.JobId, S.Draft.Num(), Clip - S.OutputIndexBefore),
			S.Draft.Num() <= Clip - S.OutputIndexBefore);
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld: the new tokens it adds (%d) fit the allowance left (%d)"), S.JobId, S.ForcedIds.Num(), Clip - S.OutputIndexBefore),
			S.ForcedIds.Num() <= Clip - S.OutputIndexBefore);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("a draft was clipped to the allowance (%d span jobs)"), Clipped), Clipped >= 1);
	return bOk;
}

// P1X.6 Cancel or release between a span job and its decode job: no crash, and the sequence
// returns to the pool clean. Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XCancelTest, "SuperSLM.SchemaJump.P1X.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XCancelTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	FSuperSLMGenerationRequest Off;
	FRunResult Fresh;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[1], ESuperSLMSchemaJumpForward::Off, 64, Off) || !RunSolo(*this, Rig, Off, Fresh)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Fresh.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 96, Req)) { return false; }
	FSuperSLMSequence Seq;
	FString Error;
	if (!TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success) ||
		!TestTrue(*Error, Rig.Cpu->SetSchema(Seq, Rig.Schema, Error)) ||
		!TestTrue(*Error, Rig.Cpu->BeginGeneration(Seq, Req, Error))) { return false; }
	// Hold after each span delivery (the job hold): the sequence's decode job is then not planned until the release
	// below, so the gap between them stays open between ticks.
	FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, Seq, ESuperSLMJobHoldPoint::AfterSpanDelivery);
	// Tick until a span job has delivered and no decode job for the sequence has been planned since.
	bool bBetween = false;
	const double Start = FPlatformTime::Seconds();
	while (!bBetween && !IsEnded(*Rig.Cpu, Seq) && FPlatformTime::Seconds() - Start < kRunSeconds)
	{
		Rig.Cpu->Tick(1.0f / 60.0f);
		const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Seq);
		if (Spans.Num() > 0 && Spans.Last().DeliveredAtTick >= 0)
		{
			const TArray<int64> Decodes = DecodeJobIdsOf(*Rig.Cpu, Seq);
			bBetween = Decodes.Num() == 0 || Decodes.Last() < Spans.Last().JobId;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("precondition: reached the gap between a span job and its decode job"), bBetween)) { return false; }
	Rig.Cpu->ReturnSequence(Seq);
	Settle(*Rig.Cpu);
	FRunResult After;
	if (!RunSolo(*this, Rig, Off, After)) { return false; }
	return SameEnding(*this, After, Fresh, TEXT("P1X.6: after the release, a generation on the pool equals a fresh one"));
}

// P1X.7 Threaded decode. With the effective decode mode threaded (bParallelDecode on and
// FinishParallelTasks > 1) drafts are capped at 6 tokens; with bParallelDecode on and
// FinishParallelTasks = 1 they are not (rev 1.3.1, review L7). Tokens equal off either way. Red:
// verbs stubbed, seam unbuilt; once built, red under "cap keyed on bParallelDecode alone".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XThreadedDecodeCapTest, "SuperSLM.SchemaJump.P1X.ThreadedDecodeCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XThreadedDecodeCapTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	bool bOk = true;
	for (int32 Tasks : { 4, 1 })
	{
		FSuperSLMRuntimeConfig C = BaseConfig(1, 1);
		C.bParallelDecode = true;
		C.FinishParallelTasks = Tasks;
		FCpuRig Rig;
		if (!SetUpCpu(*this, W.GetTestWorld(), C, Rig)) { return false; }
		if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
		const FSuperSLMResolvedCpuConfig* Resolved = Rig.Cpu->GetResolvedConfig();
		if (!TestNotNull(TEXT("resolved config"), Resolved)) { return false; }
		bOk &= TestEqual(*FString::Printf(TEXT("FinishParallelTasks %d: the effective mode"), Tasks), Resolved->bDecodeModeEffective, Tasks > 1);
		TArray<FRunResult> Off;
		TArray<FSuperSLMGenerationRequest> Requests;
		if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
		FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
		TArray<FSuperSLMSpanJobRecord> Spans;
		bOk &= ExactPopulationEqualsOff(*this, Rig, Requests, Off, Tasks > 1 ? TEXT("threaded") : TEXT("bParallelDecode, one task"), Spans);
		bOk &= RequireSpanActivity(*this, Spans, TEXT("P1X.7"));
		int32 Longest = 0;
		for (const FSuperSLMSpanJobRecord& S : Spans) { Longest = FMath::Max(Longest, S.Draft.Num()); }
		if (Tasks > 1)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("threaded: every draft is at most %d tokens (longest %d)"), kThreadedDraftCap, Longest), Longest <= kThreadedDraftCap);
		}
		else
		{
			bOk &= TestTrue(*FString::Printf(TEXT("effective mode serial: some draft exceeds %d tokens (longest %d), so the cap is not applied"), kThreadedDraftCap, Longest),
				Longest > kThreadedDraftCap);
		}
	}
	return bOk;
}

// P1X.8 Concurrency. Several sequences under one schema finish at once on worker threads: the
// table snapshot swaps once per finished generation, every span job read a snapshot that existed,
// and the tokens equal off's. The TSan half is the host cell StoreSnapshotUnderWorkers (no plugin
// TSan build exists). Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XConcurrencyTest, "SuperSLM.SchemaJump.P1X.Concurrency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XConcurrencyTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	const int32 N = PopulationPrompts().Num();
	FSuperSLMRuntimeConfig C = BaseConfig(N, 1);
	C.MaxConcurrentCalls = 4;
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), C, Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	const int64 Before = FSuperSLMSchemaJumpTestAccess::GetDraftSnapshotVersion(*Rig.Cpu, Rig.Schema);
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(N);
	for (int32 I = 0; I < N; ++I) { Runs[I].Request = Requests[I]; Runs[I].Request.SchemaJumpForward = kExact; }
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true)) { return false; }
	const int64 After = FSuperSLMSchemaJumpTestAccess::GetDraftSnapshotVersion(*Rig.Cpu, Rig.Schema);
	bool bOk = TestTrue(TEXT("a seeded table has a version"), Before >= 0);
	bOk &= TestEqual(TEXT("one snapshot swap per finished generation"), After - Before, (int64)N);
	TSet<uint32> Threads;
	for (const FSuperSLMWorkerJobReport& R : Rig.Cpu->GetJobLedger()) { if (R.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill) { Threads.Add(R.WorkerThreadId); } }
	bOk &= TestTrue(TEXT("precondition: decode jobs ran on more than one worker thread"), Threads.Num() > 1);
	for (int32 I = 0; I < N; ++I)
	{
		const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Runs[I].Result.Sequence);
		bOk &= RequireSpanActivity(*this, Spans, *FString::Printf(TEXT("P1X.8 sequence %d"), I));
		for (const FSuperSLMSpanJobRecord& S : Spans)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("span job %lld read a snapshot that existed (%lld in [%lld, %lld))"), S.JobId, S.DraftSnapshotVersion, Before, After),
				S.DraftSnapshotVersion >= Before && S.DraftSnapshotVersion < After);
		}
		bOk &= SameEnding(*this, Runs[I].Result, Off[I], FString::Printf(TEXT("P1X.8 sequence %d equals off"), I));
		Rig.Cpu->ReturnSequence(Runs[I].Result.Sequence);
	}
	Settle(*Rig.Cpu);
	return bOk;
}

// P1X.9 Composition. One decode job holds an Exact sequence (between span jobs), a Canonical
// sequence (drained) and an Off one; each equals its solo run (P1.7's analog). Red: verbs stubbed,
// seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XCompositionTest, "SuperSLM.SchemaJump.P1X.Composition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XCompositionTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(4, 3), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<FRunResult> Off;
	TArray<FSuperSLMGenerationRequest> Requests;
	if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	const ESuperSLMSchemaJumpForward Modes[3] = { kExact, ESuperSLMSchemaJumpForward::Canonical, ESuperSLMSchemaJumpForward::Off };
	TArray<FConcurrentRun> Runs;
	Runs.AddDefaulted(3);
	for (int32 I = 0; I < 3; ++I) { Runs[I].Request = Requests[I]; Runs[I].Request.SchemaJumpForward = Modes[I]; }
	if (!RunConcurrently(*this, Rig, Runs, /*bKeep*/ true, /*bGatherAtDecode*/ true)) { return false; }
	bool bOk = RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[0].Result.Sequence), TEXT("P1X.9 exact"));
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Runs[1].Result.Sequence), TEXT("P1X.9 canonical"));
	bOk &= TestTrue(TEXT("precondition: one decode job served all three"),
		SomeDecodeJobServedAll(*Rig.Cpu, { Runs[0].Result.Sequence, Runs[1].Result.Sequence, Runs[2].Result.Sequence }));
	for (const FConcurrentRun& R : Runs) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	bOk &= SameEnding(*this, Runs[0].Result, Off[0], TEXT("P1X.9 exact equals its solo run (off's)"));
	bOk &= SameEnding(*this, Runs[2].Result, Off[2], TEXT("P1X.9 off equals its solo run"));
	FRunResult CanonSolo;
	if (!RunSolo(*this, Rig, Runs[1].Request, CanonSolo)) { return false; }
	bOk &= SameEnding(*this, Runs[1].Result, CanonSolo, TEXT("P1X.9 canonical equals its solo run"));
	return bOk;
}

// P1X.10 Failure. Near the context cap, near the K/V budget, and with each non-OK status the verb
// can return: the tokens equal off's, or the generation ends exactly as off's does. Rev 1.3.5
// names the route and the set: the plugin cannot reach the engine's seams, so each status is
// injected by the plugin-side override of the next span job's status (SetNextSpanStatusOverride),
// and the set holds the verb's own statuses and, as defence, the limit statuses it never returns.
// Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XFailureTest, "SuperSLM.SchemaJump.P1X.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XFailureTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	bool bOk = true;

	// (a) Each non-OK status the verb can return, injected at the first span job.
	{
		FCpuRig Rig;
		if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
		if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
		TArray<FRunResult> Off;
		TArray<FSuperSLMGenerationRequest> Requests;
		if (!OffPopulation(*this, Rig, 96, Off, Requests)) { return false; }
		const sslm_status Statuses[] = { SSLM_INVALID_ARGUMENT, SSLM_BUFFER_TOO_SMALL, SSLM_ARTIFACT_REJECTED, SSLM_KV_POOL_EXHAUSTED,
			SSLM_CONTEXT_CAP_EXCEEDED, SSLM_KV_BUDGET_EXCEEDED, SSLM_ALLOCATION_FAILED, SSLM_NUMERIC_STEP_REFUSED };
		for (sslm_status Status : Statuses)
		{
			FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, Streams(Off));
			FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
			FSuperSLMSchemaJumpTestAccess::SetNextSpanStatusOverride(*Rig.Cpu, (int32)Status);
			FSuperSLMGenerationRequest Req = Requests[0];
			Req.SchemaJumpForward = kExact;
			FRunResult R;
			if (!RunSolo(*this, Rig, Req, R)) { return false; }
			const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, R.Sequence);
			bOk &= TestTrue(*FString::Printf(TEXT("status %d: the override reached a span job"), (int32)Status),
				Spans.Num() > 0 && Spans[0].Status == (int32)Status);
			bOk &= SameEnding(*this, R, Off[0], FString::Printf(TEXT("status %d: the generation ends as off's"), (int32)Status));
		}
	}

	// (b) Near the K/V budget: a budget holder whose limit falls inside the generation.
	{
		FSuperSLMRuntimeConfig C = BaseConfig(1, 1);
		FCpuRig Probe;
		if (!SetUpCpu(*this, W.GetTestWorld(), C, Probe)) { return false; }
		FSuperSLMGenerationRequest Req;
		if (!MakeRequest(*this, Probe, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, 96, Req)) { return false; }
		C.SequenceBudget = Req.PromptTokens.Num() + 24;
		FCpuRig Rig;
		if (!SetUpCpu(*this, W.GetTestWorld(), C, Rig)) { return false; }
		if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
		FRunResult OffRun;
		if (!RunSolo(*this, Rig, Req, OffRun)) { return false; }
		bOk &= TestEqual(TEXT("K/V budget: precondition: off stops at the budget"), (uint8)OffRun.Outcome, (uint8)ESuperSLMDecodeOutcome::KvBudgetExceeded);
		FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { OffRun.Tokens });
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		Req.SchemaJumpForward = kExact;
		FRunResult R;
		if (!RunSolo(*this, Rig, Req, R)) { return false; }
		bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, R.Sequence), TEXT("P1X.10 K/V budget"));
		bOk &= SameEnding(*this, R, OffRun, TEXT("K/V budget: exact ends as off's"));
	}

	// (c) Near the context cap: a prompt that leaves 24 positions.
	{
		FCpuRig Rig;
		if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
		if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
		FSuperSLMGenerationRequest Req;
		if (!MakeRequest(*this, Rig, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, 96, Req)) { return false; }
		constexpr int32 kCap = 4096;
		const TArray<int32> Unit = Req.PromptTokens;
		TArray<int32> Long;
		while (Long.Num() + Unit.Num() <= kCap - 24) { Long.Append(Unit); }
		Req.PromptTokens = Long;
		FRunResult OffRun;
		if (!RunSolo(*this, Rig, Req, OffRun)) { return false; }
		FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { OffRun.Tokens });
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		Req.SchemaJumpForward = kExact;
		FRunResult R;
		if (!RunSolo(*this, Rig, Req, R)) { return false; }
		bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, R.Sequence), TEXT("P1X.10 context cap"));
		bOk &= SameEnding(*this, R, OffRun, TEXT("context cap: exact ends as off's"));
	}
	return bOk;
}

// P1X.11 Lifetime (speed only). The table stays bounded over a long population (rev 1.3.5: only
// states observed forced, each with at most 4 tokens), and a model remap (a re-Configure) starts
// from no table. Red: verbs stubbed, seam unbuilt; once built, red under "no drop at unmap", an
// unbounded table, and XD1 (a held state not observed forced).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XLifetimeTest, "SuperSLM.SchemaJump.P1X.Lifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XLifetimeTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; } // ruling 4: the first generation's walk is known
	// GetSchemaForcedStateCount is the states read forced, or drafted through, so far (the seam
	// header's definition): an unseeded table has observed none right after Configure, so the count
	// is read after each finished generation, and "> 0" is asserted after the first.
	const int32 PerState = FSuperSLMSchemaJumpTestAccess::MaxTokensPerState();
	bool bOk = TestEqual(TEXT("the per-state cap is 4 (rev 1.3.5)"), PerState, 4);
	bool bFirst = true;
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	// A long population: every prompt, several rounds, each merged into the table at completion.
	constexpr int32 kRounds = 5;
	int32 Spans = 0;
	for (int32 Round = 0; Round < kRounds; ++Round)
	{
		for (const FString& P : PopulationPrompts())
		{
			FSuperSLMGenerationRequest Req;
			FRunResult R;
			if (!MakeRequest(*this, Rig, P, kExact, 96, Req) || !RunSolo(*this, Rig, Req, R)) { return false; }
			Spans += SpanJobsOf(*Rig.Cpu, R.Sequence).Num();
			const int32 ForcedStates = FSuperSLMSchemaJumpTestAccess::GetSchemaForcedStateCount(*Rig.Cpu, Rig.Schema);
			if (bFirst)
			{
				bOk &= TestTrue(TEXT("after the first finished generation, forced states have been observed"), ForcedStates > 0);
				bFirst = false;
			}
			bOk &= TestTrue(*FString::Printf(TEXT("round %d: states (%d) at most the schema's forced states (%d)"), Round,
				FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(*Rig.Cpu, Rig.Schema), ForcedStates),
				FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(*Rig.Cpu, Rig.Schema) <= ForcedStates);
			bOk &= TestTrue(*FString::Printf(TEXT("round %d: tokens per state (%d) at most the cap (%d)"), Round,
				FSuperSLMSchemaJumpTestAccess::GetDraftTableMaxTokensAtAnyState(*Rig.Cpu, Rig.Schema), PerState),
				FSuperSLMSchemaJumpTestAccess::GetDraftTableMaxTokensAtAnyState(*Rig.Cpu, Rig.Schema) <= PerState);
		}
	}
	bOk &= TestTrue(TEXT("precondition: span jobs ran"), Spans > 0);
	bOk &= TestTrue(TEXT("precondition: the population filled a table"), FSuperSLMSchemaJumpTestAccess::HasDraftTable(*Rig.Cpu, Rig.Schema));
	bOk &= TestTrue(TEXT("precondition: the subsystem read forcedness"), FSuperSLMSchemaJumpTestAccess::GetForcednessReadCount(*Rig.Cpu) > 0);
	bOk &= TestTrue(TEXT("every held state was observed forced"), FSuperSLMSchemaJumpTestAccess::IsEveryDraftTableStateObservedForced(*Rig.Cpu, Rig.Schema));
	// Remap: Configure() again unmaps and maps the model.
	if (!TestEqual(TEXT("re-Configure"), (uint8)Rig.Cpu->Configure(Rig.Model, Rig.Config).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FString Error;
	FSuperSLMSchemaHandle Schema;
	if (!TestTrue(*Error, FSuperSLMSchemaLookup::LookupByName(*Rig.Model, AExSchemaName(), Schema, Error))) { return false; }
	bOk &= TestFalse(TEXT("a remap starts from no table"), FSuperSLMSchemaJumpTestAccess::HasDraftTable(*Rig.Cpu, Schema));
	bOk &= TestEqual(TEXT("a remap's table holds no states"), FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(*Rig.Cpu, Schema), 0);
	return bOk;
}

// P1X.12 (e) The reads change nothing (rev 1.3.5). An off generation with the forcedness reads
// (sslm_seq_forced_tokens, cap 0, before the first decode job and after every job) equals one
// without them: the tokens, and the saved blob. Legs (a) to (d) are host cells (test_learning.cpp,
// test_store.cpp). Red: verbs stubbed, seam unbuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XForcednessReadsChangeNothingTest, "SuperSLM.SchemaJump.P1X.ForcednessReadsChangeNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XForcednessReadsChangeNothingTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	// Cut by MaxNewTokens mid-walk, so the sequence is Complete and can be saved.
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, 16, Req)) { return false; }

	auto RunAndSave = [&](bool bReads, FRunResult& Out, TArray<uint8>& OutBlob, int64& OutReads)
	{
		FSuperSLMSchemaJumpTestAccess::SetForcednessReadsEnabled(*Rig.Cpu, bReads);
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		if (!RunSolo(*this, Rig, Req, Out, false, /*bKeep*/ true)) { return false; }
		OutReads = FSuperSLMSchemaJumpTestAccess::GetForcednessReadCount(*Rig.Cpu);
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		if (!TestEqual(*FString::Printf(TEXT("SaveSequence queues (%s)"), *Error), (uint8)Rig.Cpu->SaveSequence(Out.Sequence, Handle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { Result = Rig.Cpu->GetSaveResult(Handle, OutBlob); return Result != ESuperSLMRestoreResult::Pending; }, 60.0);
		Rig.Cpu->ReleaseLifecycleOpHandle(Handle);
		Rig.Cpu->ReturnSequence(Out.Sequence);
		Settle(*Rig.Cpu);
		return TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && TestTrue(TEXT("the blob has bytes"), OutBlob.Num() > 0);
	};
	FRunResult With, Without;
	TArray<uint8> BlobWith, BlobWithout;
	int64 ReadsWith = 0, ReadsWithout = 0;
	if (!RunAndSave(true, With, BlobWith, ReadsWith) || !RunAndSave(false, Without, BlobWithout, ReadsWithout)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SetForcednessReadsEnabled(*Rig.Cpu, true);
	bool bOk = TestTrue(*FString::Printf(TEXT("precondition: the run with reads read forcedness (%lld)"), ReadsWith), ReadsWith > 0);
	bOk &= TestEqual(TEXT("precondition: the run without reads read nothing"), ReadsWithout, (int64)0);
	bOk &= SameEnding(*this, With, Without, TEXT("P1X.12 (e): with and without the reads"));
	bOk &= TestTrue(TEXT("P1X.12 (e): the saved blobs are byte-identical"), BlobWith == BlobWithout);
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
