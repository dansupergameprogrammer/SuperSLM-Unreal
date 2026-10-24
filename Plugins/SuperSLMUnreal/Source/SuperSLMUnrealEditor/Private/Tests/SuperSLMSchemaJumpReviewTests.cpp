// Schema fast-decode plan rev 1.3.6 (§17.12; the P1/P1-X code review, round 1): the Unreal cells the
// review's findings and the rev 1.3.6 fold need.
//
//   P1.1 price source (XD9)   SuperSLM.SchemaJump.P1.PriceSource
//   P1.9 / finding 3          SuperSLM.SchemaJump.P1.SwitchParserIsPublic
//   P1.10 (a), (b) (XD8)      SuperSLM.SchemaJump.P1.WalkTableLoad
//   P1.10 (c)                 SuperSLM.SchemaJump.P1.WalkTableLoadInFlight
//   P1.11 (a)                 SuperSLM.SchemaJump.P1.NoContinuationForJumpModes
//   P1.11 (b)                 SuperSLM.SchemaJump.P1.WalkUnknownAfterRestore
//   P1.11 (c) / finding 4 (a) SuperSLM.SchemaJump.P1.HeldRequestGoesFirst
//   P1.11 (d)                 SuperSLM.SchemaJump.P1.SpanClip
//   P1.11 (e) / finding 4 (b) SuperSLM.SchemaJump.P1.NoPromptReadsAfterFirstDecode
//   P1.11 (f) (XD11)          SuperSLM.SchemaJump.P1.ExactOverPromptOnlyPrefix
//   P1X.12 (f) (XD10)         SuperSLM.SchemaJump.P1X.StopTokenInsideDraft
//   P1.2 drain, ground truth  SuperSLM.SchemaJump.P1.DrainGroundTruth
//
// Red now: the review seam (SuperSLMSchemaJumpReviewTestAccess.h) and the public switch parser are the
// suite's stubs, the per-C configuration fields are declared but unread, and every cell that
// runs a jump-forward generation also opens with LiveGate (the verbs are stubs until the re-vendor).
// P1.11 (f)'s host half, and P1X.12 (c), (f)'s, are in ci/host/schema_jump.

#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMSchemaJumpReviewTestAccess.h"
#include "SuperSLMSchemaJumpTestSeam.h"
#include "SuperSLMSequenceTypes.h"
#include "Tests/AutomationCommon.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SuperSLMSchemaJumpReviewTests
{
	using namespace SuperSLMSchemaJumpFixtures;

	constexpr ESuperSLMSchemaJumpForward kOff = ESuperSLMSchemaJumpForward::Off;
	constexpr ESuperSLMSchemaJumpForward kExact = ESuperSLMSchemaJumpForward::Exact;
	constexpr ESuperSLMSchemaJumpForward kCanon = ESuperSLMSchemaJumpForward::Canonical;

	bool ReviewSeamGate(FAutomationTestBase& T)
	{
		return T.TestTrue(TEXT("the rev 1.3.6 review seam is built (FSuperSLMSchemaJumpReviewTestAccess is still a stub)"),
			FSuperSLMSchemaJumpReviewTestAccess::IsBuilt());
	}

	// The plan's price form, written out here: C x TokenMs + PerPositionMs x (C x d + C x (C - 1) / 2).
	double OracleMs(int32 C, int64 Depth, double TokenMs, double PerPositionMs)
	{
		return C * TokenMs + PerPositionMs * (static_cast<double>(C) * static_cast<double>(Depth) + static_cast<double>(C) * (C - 1) / 2.0);
	}

	bool Near(double A, double B) { return FMath::Abs(A - B) <= 1e-9 * (1.0 + FMath::Abs(B)); }

	// The chunk-1 and batched pairs set explicitly, so the expected figures do not depend on a saved
	// estimate or the mode profile; and a per-C pair for each C = 2..7 that differs from C x chunk-1.
	constexpr double kChunk1TokenMs = 33.583;
	constexpr double kChunk1PerPositionMs = 0.004547;
	constexpr double kBatchedTokenMs = 15.023;
	constexpr double kBatchedPerPositionMs = 0.004791;
	double PerCTokenMs(int32 C) { return 26.0 - 0.75 * C; }
	double PerCPerPositionMs(int32 C) { return 0.0045 + 0.0001 * C; }

	FSuperSLMRuntimeConfig WithPrices(FSuperSLMRuntimeConfig Config, bool bPerC)
	{
		Config.PromptTokenCostMs = kChunk1TokenMs;
		Config.PromptTokenCostPerPositionMs = kChunk1PerPositionMs;
		Config.BatchedPromptTokenCostMs = kBatchedTokenMs;
		Config.BatchedPromptTokenCostPerPositionMs = kBatchedPerPositionMs;
		if (bPerC)
		{
			for (int32 C = 2; C <= 7; ++C)
			{
				Config.PerChunkPromptTokenCostMs[C] = PerCTokenMs(C);
				Config.PerChunkPromptTokenCostPerPositionMs[C] = PerCPerPositionMs(C);
			}
		}
		return Config;
	}

	// Saves Seq and reads the blob (the sequence stays vended).
	bool SaveBlob(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob)
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		if (!T.TestEqual(*FString::Printf(TEXT("SaveSequence queues (%s)"), *Error), (uint8)Rig.Cpu->SaveSequence(Seq, Handle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { Result = Rig.Cpu->GetSaveResult(Handle, OutBlob); return Result != ESuperSLMRestoreResult::Pending; }, 60.0);
		Rig.Cpu->ReleaseLifecycleOpHandle(Handle);
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the blob has bytes"), OutBlob.Num() > 0);
	}

	// A prompt-only prefix over PromptTokens (CreatePrefix prefills on the base model, with no schema
	// content), Ready.
	bool MakePromptOnlyPrefix(FAutomationTestBase& T, const FCpuRig& Rig, const TArray<int32>& PromptTokens, FSuperSLMPrefix& Out)
	{
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("CreatePrefix (%s)"), *Error), Rig.Cpu->CreatePrefix(PromptTokens, Out, Error))) { return false; }
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]() { return Rig.Cpu->GetPrefixPhase(Out) != ESuperSLMPrefixPhase::Pending && Rig.Cpu->GetPrefixPhase(Out) != ESuperSLMPrefixPhase::Prefilling; }, 120.0);
		return T.TestEqual(TEXT("the prefix is Ready"), (uint8)Rig.Cpu->GetPrefixPhase(Out), (uint8)ESuperSLMPrefixPhase::Ready);
	}

	// Vends, binds the schema, adopts Prefix (drained), runs a no-prompt generation in Mode to its end.
	// The sequence is kept (bKeep) or returned.
	bool RunOverPrefix(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMPrefix& Prefix, ESuperSLMSchemaJumpForward Mode,
		int32 MaxNewTokens, FRunResult& Out, bool bKeep = false)
	{
		Out = FRunResult();
		FString Error;
		if (!T.TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Out.Sequence), (uint8)ESuperSLMVendResult::Success)) { return false; }
		if (!T.TestTrue(*FString::Printf(TEXT("schema bind before adopting (%s)"), *Error), Rig.Cpu->SetSchema(Out.Sequence, Rig.Schema, Error))) { return false; }
		FSuperSLMLifecycleOpHandle Handle;
		if (!T.TestEqual(*FString::Printf(TEXT("AdoptPrefix queues (%s)"), *Error), (uint8)Rig.Cpu->AdoptPrefix(Out.Sequence, Prefix, Handle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { return Rig.Cpu->GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; }, 60.0);
		const ESuperSLMRestoreResult Adopted = Rig.Cpu->GetLifecycleOpResult(Handle);
		Rig.Cpu->ReleaseLifecycleOpHandle(Handle);
		if (!T.TestEqual(TEXT("the adoption succeeds"), (uint8)Adopted, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		FSuperSLMGenerationRequest Req;
		Req.MaxNewTokens = MaxNewTokens;
		Req.SchemaJumpForward = Mode; // no prompt: decode continues from the prefix
		RunGenerationToCompletion(*Rig.Cpu, Out.Sequence, Req, Out.Tokens, kRunSeconds, Out.Error);
		Out.Phase = Rig.Cpu->GetPhase(Out.Sequence);
		Out.Tokens = Rig.Cpu->GetGeneratedTokens(Out.Sequence);
		Out.Outcome = Rig.Cpu->GetLastDecodeOutcome(Out.Sequence);
		Out.Stats = Rig.Cpu->GetStats(Out.Sequence);
		const bool bEnded = T.TestTrue(*FString::Printf(TEXT("the run over the prefix ends (%s)"), *Out.Error),
			Out.Phase == ESuperSLMSequencePhase::Complete || Out.Phase == ESuperSLMSequencePhase::Faulted);
		if (!bKeep)
		{
			Rig.Cpu->ReturnSequence(Out.Sequence);
			Settle(*Rig.Cpu);
		}
		return bEnded;
	}

	TArray<FSuperSLMForcednessReadRecord> ReadsOf(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		TArray<FSuperSLMForcednessReadRecord> Out;
		for (const FSuperSLMForcednessReadRecord& R : FSuperSLMSchemaJumpReviewTestAccess::GetForcednessReads(Cpu))
		{
			if (R.Sequence == Seq) { Out.Add(R); }
		}
		return Out;
	}

	TArray<TArray<int32>> StreamsOf(const TArray<FRunResult>& Runs)
	{
		TArray<TArray<int32>> Out;
		for (const FRunResult& R : Runs) { Out.Add(R.Tokens); }
		return Out;
	}
}

using namespace SuperSLMSchemaJumpReviewTests;

// P1.1 (rev 1.3.6 price source; mutant XD9, review finding 2). The per-C prices come from the resolved
// configuration's field, which a caller or the C = 2..7 calibration arms fill; unset, it is the
// chunk-1 pair. Every reader of a chunk of 2..7 tokens' price reads it: the scheduler's price, the
// span jobs' planned cost, and Configure()'s prefill bound. The arms' own cells: the host cell
// pricing_small_chunk_arm_* and SuperSLM.D4.Estimate.PerChunkArms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1PriceSourceTest, "SuperSLM.SchemaJump.P1.PriceSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1PriceSourceTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	const FSuperSLMRuntimeConfig PerC = WithPrices(BaseConfig(1, 1), true);
	const FSuperSLMRuntimeConfig Unset = WithPrices(BaseConfig(1, 1), false);

	// The production price for each C, read through the scheduling seam (no model: no saved estimate).
	for (const int64 Depth : {(int64)0, (int64)100, (int64)3000})
	{
		bOk &= TestTrue(*FString::Printf(TEXT("C = 1 at depth %lld: the chunk-1 pair"), Depth),
			Near(FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(1, Depth, PerC), OracleMs(1, Depth, kChunk1TokenMs, kChunk1PerPositionMs)));
		for (int32 C = 2; C <= 7; ++C)
		{
			const double Got = FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(C, Depth, PerC);
			bOk &= TestTrue(*FString::Printf(TEXT("C = %d at depth %lld: priced from the configured per-C pair (%.6f, want %.6f)"), C, Depth, Got,
				OracleMs(C, Depth, PerCTokenMs(C), PerCPerPositionMs(C))), Near(Got, OracleMs(C, Depth, PerCTokenMs(C), PerCPerPositionMs(C))));
			bOk &= TestFalse(*FString::Printf(TEXT("C = %d at depth %lld: not C x the chunk-1 price (X15, XD9)"), C, Depth),
				Near(Got, OracleMs(C, Depth, kChunk1TokenMs, kChunk1PerPositionMs)));
			// Unset: the chunk-1 pair, the plan's default until B5f.
			bOk &= TestTrue(*FString::Printf(TEXT("C = %d at depth %lld, per-C unset: the chunk-1 pair"), C, Depth),
				Near(FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(C, Depth, Unset), OracleMs(C, Depth, kChunk1TokenMs, kChunk1PerPositionMs)));
		}
		bOk &= TestTrue(*FString::Printf(TEXT("C = 8 at depth %lld: the batched pair"), Depth),
			Near(FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(8, Depth, PerC), OracleMs(8, Depth, kBatchedTokenMs, kBatchedPerPositionMs)));
	}

	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Split = PerC;
	Split.MaxPrefillChunkBudget = 7; // spans and prompt chunks of 2..7 tokens
	if (!SetUpCpu(*this, W.GetTestWorld(), Split, Rig)) { return false; }

	// The resolved configuration carries the field.
	{
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Split);
		if (!TestTrue(TEXT("re-Configure with the per-C pairs succeeds and reports its resolution"), Report.Result == ESuperSLMConfigureResult::Success && Report.Resolved.IsSet())) { return false; }
		for (int32 C = 2; C <= 7; ++C)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("resolved PerChunkPromptTokenCostMs[%d] is the caller's"), C), Near(Report.Resolved->PerChunkPromptTokenCostMs[C], PerCTokenMs(C)));
			bOk &= TestTrue(*FString::Printf(TEXT("resolved PerChunkPromptTokenCostPerPositionMs[%d] is the caller's"), C), Near(Report.Resolved->PerChunkPromptTokenCostPerPositionMs[C], PerCPerPositionMs(C)));
		}
	}
	{
		FSuperSLMRuntimeConfig Cleared = Split;
		for (int32 C = 0; C < 8; ++C) { Cleared.PerChunkPromptTokenCostMs[C].Reset(); Cleared.PerChunkPromptTokenCostPerPositionMs[C].Reset(); }
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Cleared);
		if (!TestTrue(TEXT("Configure with the per-C pairs unset succeeds"), Report.Result == ESuperSLMConfigureResult::Success && Report.Resolved.IsSet())) { return false; }
		for (int32 C = 2; C <= 7; ++C)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("unset: resolved per-C pair %d is the chunk-1 pair"), C),
				Near(Report.Resolved->PerChunkPromptTokenCostMs[C], Report.Resolved->PromptTokenCostMs) &&
				Near(Report.Resolved->PerChunkPromptTokenCostPerPositionMs[C], Report.Resolved->PromptTokenCostPerPositionMs));
		}
	}

	// Configure()'s prefill bound reads the table up to C = 7: an unbounded C = 7 entry is refused.
	{
		FSuperSLMRuntimeConfig Huge = Split;
		Huge.PerChunkPromptTokenCostMs[7] = 1e12;
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Huge);
		bOk &= TestEqual(*FString::Printf(TEXT("a per-C pair at C = 7 whose bound implies K above the limit is refused (%s)"), *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::InvalidCostDomain);
	}

	// The scheduler: every span job of C rows is planned at the per-C price (needs the live verbs).
	if (!LiveGate(*this)) { return false; }
	if (!TestEqual(TEXT("re-Configure with the per-C pairs"), (uint8)Rig.Cpu->Configure(Rig.Model, Split).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FSuperSLMGenerationRequest Req;
	FRunResult Run;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 256, Req) || !RunSolo(*this, Rig, Req, Run)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
	if (!RequireSpanActivity(*this, Spans, TEXT("P1.1 price source"))) { return false; }
	int32 Small = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		if (S.PricedChunk < 2 || S.PricedChunk > 7) { continue; }
		++Small;
		const double Want = OracleMs(S.PricedChunk, S.StartDepth, PerCTokenMs(S.PricedChunk), PerCPerPositionMs(S.PricedChunk));
		bOk &= TestTrue(*FString::Printf(TEXT("span job %lld of %d rows at depth %lld: planned at the per-C price (%.6f, want %.6f)"),
			S.JobId, S.PricedChunk, S.StartDepth, S.PlannedJobMs, Want), Near(S.PlannedJobMs, Want));
	}
	bOk &= TestTrue(TEXT("precondition: a span job of 2..7 rows ran"), Small > 0);
	return bOk;
}

// P1.9 and review finding 3 (a code defect against plan text): the scratch switch's parser is the
// plugin's public API, so the example (which reads only the public API) can call it, and it is the
// core parser's production caller. Case-insensitive; anything else refused with the mode unchanged.
// The example's own wiring and log line: SuperSLMExample.Demo.SchemaJumpForwardSwitch and the host
// cell ci/tests/test_schema_jump_example_switch.py. Mutant XD7.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1SwitchParserIsPublicTest, "SuperSLM.SchemaJump.P1.SwitchParserIsPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1SwitchParserIsPublicTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Text; ESuperSLMSchemaJumpForward Want; };
	const FCase Accepted[] = {
		{ TEXT("0"), kOff }, { TEXT("1"), kCanon },
		{ TEXT("off"), kOff }, { TEXT("exact"), kExact }, { TEXT("canonical"), kCanon },
		{ TEXT("OFF"), kOff }, { TEXT("Exact"), kExact }, { TEXT("CaNoNiCaL"), kCanon },
	};
	bool bOk = true;
	for (const FCase& C : Accepted)
	{
		// Start from a mode other than the one wanted, so "unchanged" cannot pass.
		ESuperSLMSchemaJumpForward Mode = C.Want == kExact ? kCanon : kExact;
		bOk &= TestTrue(*FString::Printf(TEXT("'%s' is accepted"), C.Text), ParseSuperSLMSchemaJumpForwardSwitch(C.Text, Mode));
		bOk &= TestEqual(*FString::Printf(TEXT("'%s' sets %s"), C.Text, ModeName(C.Want)), (uint8)Mode, (uint8)C.Want);
	}
	const TCHAR* Refused[] = { TEXT(""), TEXT(" exact"), TEXT("exact "), TEXT("2"), TEXT("-1"), TEXT("on"), TEXT("true"), TEXT("exactly"), TEXT("canon"), TEXT("01") };
	for (const TCHAR* Text : Refused)
	{
		ESuperSLMSchemaJumpForward Mode = kExact;
		bOk &= TestFalse(*FString::Printf(TEXT("'%s' is refused"), Text), ParseSuperSLMSchemaJumpForwardSwitch(Text, Mode));
		bOk &= TestEqual(*FString::Printf(TEXT("'%s' leaves the mode as it was"), Text), (uint8)Mode, (uint8)kExact);
	}
	return bOk;
}

// P1.10 (a), (b) (rev 1.3.6; mutant XD8; review finding 1). Configure() on the game thread does no part
// of the walk table's load; with no schema-bound generation nothing loads at all, nor with only Off
// ones (the plan's ruling 4: the table loads at the first generation that is both schema-bound
// and in a jump mode other than Off); the first such generation loads it on a pool thread, from the
// section found through the mapped artifact (no whole-model load, no hash, no artifact-sized copy); a
// re-Configure() of the same model loads nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1WalkTableLoadTest, "SuperSLM.SchemaJump.P1.WalkTableLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1WalkTableLoadTest::RunTest(const FString& Parameters)
{
	if (!ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(2, 2), Rig)) { return false; }
	bool bOk = TestEqual(TEXT("(a) Configure() loads no walk table"), FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(*Rig.Cpu).Num(), 0);
	bOk &= TestFalse(TEXT("(a) the walk table is not ready after Configure()"), FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu));

	// An unbound generation, Off: still nothing.
	FSuperSLMGenerationRequest Req;
	FRunResult Run;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kOff, 8, Req) || !RunSolo(*this, Rig, Req, Run, /*bUnbound*/ true)) { return false; }
	bOk &= TestEqual(TEXT("(a) an unbound generation loads nothing"), FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(*Rig.Cpu).Num(), 0);

	// A schema-bound generation in Off: still nothing (ruling 4).
	if (!RunSolo(*this, Rig, Req, Run)) { return false; }
	bOk &= TestEqual(TEXT("(a) a schema-bound Off generation loads nothing"), FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(*Rig.Cpu).Num(), 0);
	bOk &= TestFalse(TEXT("(a) the walk table is not ready after an Off generation"), FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu));

	// The first schema-bound generation in a jump mode (Exact) starts the load.
	FSuperSLMGenerationRequest ExactReq;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 8, ExactReq) || !RunSolo(*this, Rig, ExactReq, Run)) { return false; }
	DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]() { return FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu); }, 60.0);
	const TArray<FSuperSLMSchemaWalkLoadRecord> Loads = FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(*Rig.Cpu);
	bOk &= TestEqual(TEXT("(c) the first schema-bound generation in a jump mode loads the walk table once"), Loads.Num(), 1);
	for (const FSuperSLMSchemaWalkLoadRecord& L : Loads)
	{
		bOk &= TestFalse(TEXT("(c) no part of the load ran on the game thread (XD8)"), L.bAnyOnGameThread);
		bOk &= TestFalse(TEXT("(a) no part of the load ran inside Configure()"), L.bInsideConfigure);
		bOk &= TestFalse(TEXT("no whole-model load (SslmModel::Load) ran for it"), L.bWholeModelLoad);
		bOk &= TestEqual(TEXT("the load hashed no artifact bytes"), L.ArtifactBytesHashed, (int64)0);
		bOk &= TestTrue(*FString::Printf(TEXT("precondition: the artifact has a SchemaMasks section (%lld bytes) smaller than the file (%lld)"), L.SectionBytes, L.ArtifactBytes),
			L.SectionBytes > 0 && L.SectionBytes < L.ArtifactBytes);
		bOk &= TestTrue(*FString::Printf(TEXT("the work is the section's size, never the file's: copied %lld <= section %lld"), L.BytesCopied, L.SectionBytes),
			L.BytesCopied >= 0 && L.BytesCopied <= L.SectionBytes);
		bOk &= TestTrue(TEXT("the table became ready"), L.bReady);
	}

	// (b) A re-Configure() of the same model does no artifact-sized work for the walk.
	if (!TestEqual(TEXT("re-Configure"), (uint8)Rig.Cpu->Configure(Rig.Model, Rig.Config).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
	bOk &= TestEqual(TEXT("(b) a re-Configure() loads no walk table"), FSuperSLMSchemaJumpReviewTestAccess::GetSchemaWalkLoads(*Rig.Cpu).Num(), 0);
	return bOk;
}

// P1.10 (c) (rev 1.3.6). Until the walk table is ready the walk is unknown: an exact generation makes
// no forcedness read, issues no span job, and its tokens equal off's. Once it is ready, exact mode
// drafts (with a seeded table) and still equals off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1WalkTableLoadInFlightTest, "SuperSLM.SchemaJump.P1.WalkTableLoadInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1WalkTableLoadInFlightTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this) || !ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 256, Req)) { return false; }

	FSuperSLMSchemaJumpReviewTestAccess::HoldSchemaWalkLoad(*Rig.Cpu, true);
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FRunResult Held;
	if (!RunSolo(*this, Rig, Req, Held)) { return false; }
	bool bOk = TestFalse(TEXT("precondition: the held load is not ready"), FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu));
	bOk &= TestEqual(TEXT("(c) while the load is in flight an exact generation issues no span job"), SpanJobsOf(*Rig.Cpu, Held.Sequence).Num(), 0);
	bOk &= TestEqual(TEXT("(c) and makes no forcedness read (the walk is unknown)"), ReadsOf(*Rig.Cpu, Held.Sequence).Num(), 0);
	FRunResult Off;
	if (!RunOff(*this, Rig, Req, Off)) { return false; }
	bOk &= SameEnding(*this, Held, Off, TEXT("(c) exact while the load is in flight equals off"));

	FSuperSLMSchemaJumpReviewTestAccess::HoldSchemaWalkLoad(*Rig.Cpu, false);
	DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]() { return FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu); }, 60.0);
	if (!TestTrue(TEXT("the released load becomes ready"), FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*Rig.Cpu))) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	// The Off run above ran before the table was ready, so it read no forcedness (ruling 4); the seed
	// comes from an Off run made now.
	FRunResult OffReady;
	if (!RunOff(*this, Rig, Req, OffReady)) { return false; }
	bOk &= TestEqual(TEXT("precondition: off's tokens do not depend on the table"), OffReady.Tokens, Off.Tokens);
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { OffReady.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FRunResult Ready;
	if (!RunSolo(*this, Rig, Req, Ready)) { return false; }
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Ready.Sequence), TEXT("P1.10 (c) once ready"));
	bOk &= SameEnding(*this, Ready, OffReady, TEXT("(c) exact once the table is ready equals off"));
	return bOk;
}

// P1.11 (a) (rev 1.3.6). An Exact or Canonical generation neither joins nor blocks a worker
// continuation; Off keeps its continuations.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1NoContinuationForJumpModesTest, "SuperSLM.SchemaJump.P1.NoContinuationForJumpModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1NoContinuationForJumpModesTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(3, 3);
	Config.bWorkerContinuation = true;
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	auto ContinuationRowsOf = [&](const FSuperSLMSequence& Seq)
	{
		int32 N = 0;
		for (const FSuperSLMWorkerJobReport& R : Rig.Cpu->GetJobLedger()) { N += (R.bContinuation && R.MemberSequences.Contains(Seq)) ? 1 : 0; }
		return N;
	};

	// Off alone: its continuations.
	TArray<FConcurrentRun> Solo;
	Solo.AddDefaulted(1);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kOff, 64, Solo[0].Request) || !RunConcurrently(*this, Rig, Solo, /*bKeep*/ true)) { return false; }
	const int32 OffSoloContinuations = ContinuationRowsOf(Solo[0].Result.Sequence);
	Rig.Cpu->ReturnSequence(Solo[0].Result.Sequence);
	Settle(*Rig.Cpu);
	bool bOk = TestTrue(*FString::Printf(TEXT("precondition: Off alone takes worker continuations (%d)"), OffSoloContinuations), OffSoloContinuations > 0);

	// Off beside an Exact and a Canonical generation (the exact one with a seeded table).
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Solo[0].Result.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	TArray<FConcurrentRun> Mixed;
	Mixed.AddDefaulted(3);
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kOff, 64, Mixed[0].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[1], kExact, 64, Mixed[1].Request) ||
		!MakeRequest(*this, Rig, PopulationPrompts()[2], kCanon, 64, Mixed[2].Request) ||
		!RunConcurrently(*this, Rig, Mixed, /*bKeep*/ true)) { return false; }
	bOk &= TestEqual(TEXT("(a) no worker continuation includes the Exact generation"), ContinuationRowsOf(Mixed[1].Result.Sequence), 0);
	bOk &= TestEqual(TEXT("(a) no worker continuation includes the Canonical generation"), ContinuationRowsOf(Mixed[2].Result.Sequence), 0);
	bOk &= TestTrue(*FString::Printf(TEXT("(a) Off beside them still takes continuations (%d): they do not block one"), ContinuationRowsOf(Mixed[0].Result.Sequence)),
		ContinuationRowsOf(Mixed[0].Result.Sequence) > 0);
	bOk &= TestEqual(TEXT("Off beside them equals Off alone"), Mixed[0].Result.Tokens, Solo[0].Result.Tokens);
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Mixed[1].Result.Sequence), TEXT("P1.11 (a) exact"));
	for (const FConcurrentRun& R : Mixed) { Rig.Cpu->ReturnSequence(R.Result.Sequence); }
	Settle(*Rig.Cpu);
	return bOk;
}

// P1.11 (b) (rev 1.3.6). On a sequence that is neither fresh nor over a prompt-only prefix the plugin
// adopted, the walk is unknown: Exact and Off make no forcedness read and issue no span job, and
// Canonical still drains. The CPU input here is a restored sequence (the plugin did not build its
// contents). A CPU prefix is always prompt-only (CreatePrefix prefills tokens on the base model), so
// "after a prefix that carries schema progress" has no CPU input; see the build record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1WalkUnknownAfterRestoreTest, "SuperSLM.SchemaJump.P1.WalkUnknownAfterRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1WalkUnknownAfterRestoreTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this) || !ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(3, 3);
	Config.PrefixBlockCount = 1;
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("tokenize"), Rig.Cpu->Tokenize(PopulationPrompts()[0], PromptTokens))) { return false; }
	FSuperSLMPrefix Prefix;
	if (!MakePromptOnlyPrefix(*this, Rig, PromptTokens, Prefix)) { return false; }

	// A schema-bound sequence over the prefix, saved before any generation, then restored.
	FSuperSLMSequence Source;
	FString Error;
	if (!TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Source), (uint8)ESuperSLMVendResult::Success)) { return false; }
	if (!TestTrue(*Error, Rig.Cpu->SetSchema(Source, Rig.Schema, Error))) { return false; }
	FSuperSLMLifecycleOpHandle Adopt;
	if (!TestEqual(TEXT("adopt queues"), (uint8)Rig.Cpu->AdoptPrefix(Source, Prefix, Adopt, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]() { return Rig.Cpu->GetLifecycleOpResult(Adopt) != ESuperSLMRestoreResult::Pending; }, 60.0);
	Rig.Cpu->ReleaseLifecycleOpHandle(Adopt);
	TArray<uint8> Blob;
	if (!SaveBlob(*this, Rig, Source, Blob)) { return false; }
	Rig.Cpu->ReturnSequence(Source);
	Settle(*Rig.Cpu);

	bool bOk = true;
	TArray<int32> OffTokens;
	for (const ESuperSLMSchemaJumpForward Mode : { kOff, kExact, kCanon })
	{
		FSuperSLMSequence Seq;
		FSuperSLMLifecycleOpHandle Restore;
		if (!TestEqual(*FString::Printf(TEXT("%s: restore queues (%s)"), ModeName(Mode), *Error), (uint8)Rig.Cpu->RestoreSequence(Blob, Rig.Model, Seq, Restore, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]() { return Rig.Cpu->GetLifecycleOpResult(Restore) != ESuperSLMRestoreResult::Pending; }, 60.0);
		if (!TestEqual(TEXT("the restore succeeds"), (uint8)Rig.Cpu->GetLifecycleOpResult(Restore), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Rig.Cpu->ReleaseLifecycleOpHandle(Restore);
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		FSuperSLMGenerationRequest Req;
		Req.MaxNewTokens = 64;
		Req.SchemaJumpForward = Mode;
		TArray<int32> Tokens;
		RunGenerationToCompletion(*Rig.Cpu, Seq, Req, Tokens, kRunSeconds, Error);
		Tokens = Rig.Cpu->GetGeneratedTokens(Seq);
		bOk &= TestFalse(*FString::Printf(TEXT("(b) %s over a restored sequence: the walk is unknown"), ModeName(Mode)), FSuperSLMSchemaJumpReviewTestAccess::IsWalkKnown(*Rig.Cpu, Seq));
		const int32 Reads = ReadsOf(*Rig.Cpu, Seq).Num();
		const int32 Spans = SpanJobsOf(*Rig.Cpu, Seq).Num();
		if (Mode == kCanon)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("(b) Canonical still reads forcedness (%d): its drain needs no walk"), Reads), Reads > 0);
		}
		else
		{
			bOk &= TestEqual(*FString::Printf(TEXT("(b) %s makes no forcedness read"), ModeName(Mode)), Reads, 0);
			bOk &= TestEqual(*FString::Printf(TEXT("(b) %s issues no span job"), ModeName(Mode)), Spans, 0);
			if (Mode == kOff) { OffTokens = Tokens; }
			else { bOk &= TestEqual(TEXT("(b) Exact over a restored sequence equals Off"), Tokens, OffTokens); }
		}
		Rig.Cpu->ReturnSequence(Seq);
		Settle(*Rig.Cpu);
	}
	Rig.Cpu->ReleasePrefix(Prefix);
	return bOk;
}

// P1.11 (c) (rev 1.3.6; review finding 4 (a)). While a schema change or an adapter swap is held for the
// sequence, no span job is planned: the next job is a decode, which carries the request. The input:
// Canonical with a chunk budget that splits chains, and an adapter swap (to no adapter, which changes
// no token on the base model) requested right after a span job left its chain open, when the plain
// drain rule would plan another span job.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1HeldRequestGoesFirstTest, "SuperSLM.SchemaJump.P1.HeldRequestGoesFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1HeldRequestGoesFirstTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(1, 1);
	Config.MaxPrefillChunkBudget = 3; // splits the longer key chains
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kCanon, 256, Req)) { return false; }
	FSuperSLMSequence Seq;
	FString Error;
	if (!TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	if (!TestTrue(*Error, Rig.Cpu->SetSchema(Seq, Rig.Schema, Error))) { return false; }
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	if (!TestTrue(*Error, Rig.Cpu->BeginGeneration(Seq, Req, Error))) { return false; }

	int32 HoldTick = -1;
	const double Start = FPlatformTime::Seconds();
	while (!IsEnded(*Rig.Cpu, Seq) && FPlatformTime::Seconds() - Start < kRunSeconds)
	{
		Rig.Cpu->Tick(1.0f / 60.0f);
		if (HoldTick < 0)
		{
			for (const FSuperSLMSpanJobRecord& S : SpanJobsOf(*Rig.Cpu, Seq))
			{
				if (S.ForcedChainLeftAfter > 0)
				{
					HoldTick = FSuperSLMSchedulingTestAccess::GetTickCounter(*Rig.Cpu); // jobs planned from here on follow the request
					Rig.Cpu->RequestAdapterSwap(Seq, FSuperSLMAdapterHandle());
					break;
				}
			}
		}
		FPlatformProcess::Sleep(0.001f);
	}
	bool bOk = TestTrue(TEXT("the run ends"), IsEnded(*Rig.Cpu, Seq));
	if (!TestTrue(TEXT("precondition: a span job left its chain open, and the swap was requested then"), HoldTick >= 0)) { return false; }
	// The first job for the sequence planned at or after HoldTick.
	int64 FirstJob = TNumericLimits<int64>::Max();
	bool bFirstIsDecode = false;
	for (const FSuperSLMWorkerJobReport& R : Rig.Cpu->GetJobLedger())
	{
		if (R.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && R.MemberSequences.Contains(Seq) && R.PlannedAtTick >= HoldTick && R.JobId < FirstJob)
		{
			FirstJob = R.JobId;
			bFirstIsDecode = true;
		}
	}
	for (const FSuperSLMSpanJobRecord& S : SpanJobsOf(*Rig.Cpu, Seq))
	{
		if (S.PlannedAtTick >= HoldTick && S.JobId < FirstJob)
		{
			FirstJob = S.JobId;
			bFirstIsDecode = false;
		}
	}
	bOk &= TestTrue(TEXT("(c) the first job after the held request is a decode, which carries it, not a span job"), bFirstIsDecode);
	Rig.Cpu->ReturnSequence(Seq);
	Settle(*Rig.Cpu);
	return bOk;
}

// P1.11 (d) (rev 1.3.6). A span's budget never exceeds the context cap or the slot limit (less the
// context used), the largest prefill chunk, or the allowance. Budget mode, so the slot limit binds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1SpanClipTest, "SuperSLM.SchemaJump.P1.SpanClip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1SpanClipTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	bool bOk = true;
	TArray<FRunResult> OffRuns;
	for (const ESuperSLMSchemaJumpForward Mode : { kCanon, kExact })
	{
		for (const int32 ChunkBudget : { 3, 7 })
		{
			FSuperSLMGenerationRequest Req;
			if (!MakeRequest(*this, Rig, PopulationPrompts()[0], Mode, 20, Req)) { return false; }
			FSuperSLMRuntimeConfig C = Rig.Config;
			C.MaxPrefillChunkBudget = ChunkBudget;
			C.SequenceBudget = Req.PromptTokens.Num() + 16; // the slot limit binds before the allowance runs out
			if (!TestEqual(TEXT("Configure (budget mode)"), (uint8)Rig.Cpu->Configure(Rig.Model, C).Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
			if (Mode == kExact)
			{
				if (!LoadWalkTableForSeeding(*this, Rig)) { return false; } // after this leg's Configure(), which drops the table (O6)
				FRunResult Off;
				if (!RunOff(*this, Rig, Req, Off)) { return false; }
				FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Off.Tokens });
			}
			FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
			FRunResult Run;
			if (!RunSolo(*this, Rig, Req, Run)) { return false; }
			const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Run.Sequence);
			bOk &= RequireSpanActivity(*this, Spans, *FString::Printf(TEXT("P1.11 (d) %s at chunk %d"), ModeName(Mode), ChunkBudget));
			const int64 SlotLimit = C.SequenceBudget; // origin 0: no adopted prefix
			for (const FSuperSLMSpanJobRecord& S : Spans)
			{
				const FString What = FString::Printf(TEXT("%s, chunk %d, span job %lld"), ModeName(Mode), ChunkBudget, S.JobId);
				bOk &= TestTrue(*(What + TEXT(": within the largest prefill chunk")), S.ChunkBudget >= 1 && S.ChunkBudget <= ChunkBudget);
				bOk &= TestTrue(*(What + FString::Printf(TEXT(": within the slot limit less the context used (%lld + %d <= %lld)"), S.StartDepth, S.ChunkBudget, SlotLimit)),
					S.StartDepth + S.ChunkBudget <= SlotLimit);
				bOk &= TestTrue(*(What + FString::Printf(TEXT(": its new tokens (%d) fit the allowance left (%d)"), S.ForcedIds.Num(), Req.MaxNewTokens - S.OutputIndexBefore)),
					S.ForcedIds.Num() <= Req.MaxNewTokens - S.OutputIndexBefore);
			}
		}
	}
	return bOk;
}

// P1.11 (e) (rev 1.3.6; review finding 4 (b)). A generation with no prompt (over an adopted prefix) has
// no prefill job to read after: its first forcedness read follows its first decode. In Canonical
// mode its first token is therefore the model's choice; after it, no decode is planned with a chain
// open.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1NoPromptReadsAfterFirstDecodeTest, "SuperSLM.SchemaJump.P1.NoPromptReadsAfterFirstDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1NoPromptReadsAfterFirstDecodeTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this) || !ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(2, 2);
	Config.PrefixBlockCount = 1;
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("tokenize"), Rig.Cpu->Tokenize(PopulationPrompts()[0], PromptTokens))) { return false; }
	FSuperSLMPrefix Prefix;
	if (!MakePromptOnlyPrefix(*this, Rig, PromptTokens, Prefix)) { return false; }
	bool bOk = true;
	for (const ESuperSLMSchemaJumpForward Mode : { kCanon, kExact, kOff })
	{
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		const int32 OpenBefore = FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain(*Rig.Cpu);
		FRunResult Run;
		if (!RunOverPrefix(*this, Rig, Prefix, Mode, 64, Run, /*bKeep*/ true)) { return false; }
		const TArray<int64> Decodes = DecodeJobIdsOf(*Rig.Cpu, Run.Sequence);
		const TArray<FSuperSLMForcednessReadRecord> Reads = ReadsOf(*Rig.Cpu, Run.Sequence);
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: decodes ran (%d) and reads were made (%d)"), ModeName(Mode), Decodes.Num(), Reads.Num()),
			Decodes.Num() > 0 && Reads.Num() > 0)) { return false; }
		int64 FirstDecode = TNumericLimits<int64>::Max();
		for (const int64 Id : Decodes) { FirstDecode = FMath::Min(FirstDecode, Id); }
		bOk &= TestEqual(*FString::Printf(TEXT("(e) %s with no prompt: the first read follows the first decode"), ModeName(Mode)), Reads[0].AfterJobId, FirstDecode);
		if (Mode == kCanon)
		{
			const int32 Open = FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain(*Rig.Cpu) - OpenBefore;
			bOk &= TestTrue(*FString::Printf(TEXT("(e) Canonical with no prompt: only the first decode can be planned with a chain open (%d)"), Open), Open >= 0 && Open <= 1);
		}
		Rig.Cpu->ReturnSequence(Run.Sequence);
		Settle(*Rig.Cpu);
	}
	Rig.Cpu->ReleasePrefix(Prefix);
	return bOk;
}

// P1.11 (f) (rev 1.3.6 ruling; mutant XD11). An exact generation over an adopted
// prompt-only prefix, with a learned table, starts its walk at state 0: it reads forcedness, issues
// span jobs with non-empty drafts, and its tokens equal off's over the same prefix, digest and blob.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1ExactOverPromptOnlyPrefixTest, "SuperSLM.SchemaJump.P1.ExactOverPromptOnlyPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1ExactOverPromptOnlyPrefixTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this) || !ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(2, 2);
	Config.PrefixBlockCount = 1;
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("tokenize"), Rig.Cpu->Tokenize(PopulationPrompts()[0], PromptTokens))) { return false; }
	FSuperSLMPrefix Prefix;
	if (!MakePromptOnlyPrefix(*this, Rig, PromptTokens, Prefix)) { return false; }

	// The cut, by construction: the learned table is the whole off stream over the prefix, walked from state 0;
	// an exact probe finds its longest confirmed span, and the cut lands inside it, so the planner must clip that
	// span's draft to the allowance (the span never admits the generation's last allowed token).
	FRunResult WholeOff, Probe;
	if (!RunOverPrefix(*this, Rig, Prefix, kOff, 96, WholeOff)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { WholeOff.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	if (!RunOverPrefix(*this, Rig, Prefix, kExact, 96, Probe, /*bKeep*/ true)) { return false; }
	int32 kMaxNew = -1;
	int32 Longest = 0;
	for (const FSuperSLMSpanJobRecord& S : SpanJobsOf(*Rig.Cpu, Probe.Sequence))
	{
		if (S.ForcedIds.Num() >= 2 && S.ForcedIds.Num() > Longest)
		{
			Longest = S.ForcedIds.Num();
			kMaxNew = S.OutputIndexBefore + (S.ForcedIds.Num() + 1) / 2;
		}
	}
	Rig.Cpu->ReturnSequence(Probe.Sequence);
	Settle(*Rig.Cpu);
	if (!TestTrue(TEXT("precondition: the probe confirmed a span of 2 or more tokens to cut inside"), kMaxNew > 0)) { return false; }

	FRunResult Off, Exact;
	TArray<uint8> OffBlob, ExactBlob;
	if (!RunOverPrefix(*this, Rig, Prefix, kOff, kMaxNew, Off, /*bKeep*/ true) || !SaveBlob(*this, Rig, Off.Sequence, OffBlob)) { return false; }
	Rig.Cpu->ReturnSequence(Off.Sequence);
	Settle(*Rig.Cpu);
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { WholeOff.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	if (!RunOverPrefix(*this, Rig, Prefix, kExact, kMaxNew, Exact, /*bKeep*/ true)) { return false; }
	bool bOk = TestTrue(TEXT("(f) over a prompt-only prefix the walk is known"), FSuperSLMSchemaJumpReviewTestAccess::IsWalkKnown(*Rig.Cpu, Exact.Sequence));
	const TArray<FSuperSLMForcednessReadRecord> Reads = ReadsOf(*Rig.Cpu, Exact.Sequence);
	bOk &= TestTrue(*FString::Printf(TEXT("(f) it reads forcedness (%d)"), Reads.Num()), Reads.Num() > 0);
	int32 RecordedAtZeroOrAfter = 0;
	for (const FSuperSLMForcednessReadRecord& R : Reads) { RecordedAtZeroOrAfter += R.bRecordedAtWalkState ? 1 : 0; }
	bOk &= TestTrue(TEXT("(f) its reads are recorded against walk states (the walk started at state 0)"), RecordedAtZeroOrAfter > 0);
	const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Exact.Sequence);
	bOk &= RequireSpanActivity(*this, Spans, TEXT("P1.11 (f)"));
	int32 Clipped = 0;
	for (const FSuperSLMSpanJobRecord& S : Spans)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("(f) span job %lld carries a non-empty draft"), S.JobId), !S.bDraftNull && S.Draft.Num() >= 1);
		Clipped += S.DraftLengthBeforeAllowanceClip > S.DraftLengthBeforeStopCut ? 1 : 0;
	}
	bOk &= TestTrue(*FString::Printf(TEXT("(f) a draft was clipped at the cut (MaxNewTokens %d): %d span jobs"), kMaxNew, Clipped), Clipped >= 1);
	bOk &= SameEnding(*this, Exact, Off, TEXT("(f) exact over the prefix equals off over it"));
	bOk &= SaveBlob(*this, Rig, Exact.Sequence, ExactBlob);
	bOk &= TestTrue(TEXT("(f) the saved blobs are byte-identical"), ExactBlob == OffBlob);
	Rig.Cpu->ReturnSequence(Exact.Sequence);
	Settle(*Rig.Cpu);
	Rig.Cpu->ReleasePrefix(Prefix);
	return bOk;
}

// P1X.12 (f) (rev 1.3.6; mutant XD10; review finding 5), the Unreal half. A stop token inside a
// confirmed draft: the span's read is recorded at the engine's state or dropped, so the table never
// holds a state observed Unforced, and exact still equals off with the same stop tokens. The input
// takes as the stop token the first confirmed id of a span job that confirmed two or more.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XStopTokenInsideDraftTest, "SuperSLM.SchemaJump.P1X.StopTokenInsideDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XStopTokenInsideDraftTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 256, Req)) { return false; }
	FRunResult Off;
	if (!RunOff(*this, Rig, Req, Off)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Off.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FRunResult Learn;
	if (!RunSolo(*this, Rig, Req, Learn)) { return false; }
	int32 StopToken = INDEX_NONE;
	for (const FSuperSLMSpanJobRecord& S : SpanJobsOf(*Rig.Cpu, Learn.Sequence))
	{
		if (S.ForcedIds.Num() >= 2) { StopToken = S.ForcedIds[0]; break; }
	}
	if (!TestTrue(TEXT("precondition: a span job confirmed two or more ids"), StopToken != INDEX_NONE)) { return false; }

	FSuperSLMGenerationRequest Stopped = Req;
	Stopped.StopTokenIds.Add(StopToken);
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FRunResult Run, OffStopped;
	if (!RunSolo(*this, Rig, Stopped, Run) || !RunOff(*this, Rig, Stopped, OffStopped)) { return false; }
	bool bOk = TestTrue(TEXT("(f) every state the table holds is still observed forced (no read recorded at the stop's state)"),
		FSuperSLMSchemaJumpTestAccess::IsEveryDraftTableStateObservedForced(*Rig.Cpu, Rig.Schema));
	bOk &= SameEnding(*this, Run, OffStopped, TEXT("(f) exact with the stop token equals off with it"));
	return bOk;
}

// Dogfood (review of 70d42cd6c3..6f30cb535b, item 3): a stop token inside a confirmed draft leaves the same
// resumable state as off with the same stop token, so the saved blobs are byte-identical (P1.11 (f)'s rule, at a
// stop instead of a MaxNewTokens cut). Two cases, chosen from the learning run's span jobs: the stop at a draft's
// first id, and in its middle. Each stop is the token's first occurrence in the stream, and a span job admits
// tokens before it, so the stopped run reaches that draft. Each case asserts that the stopped run took a span
// job, that no draft it passed carried the stop token (the planner cuts the draft before it), that it stops at
// the token, that it equals off with the same stop token, and that the saved blobs are byte-identical. In the
// middle case it also asserts the cut itself: a span job whose draft the planner cut ends exactly at the stop's
// position. (At a draft's first id the cut leaves no draft, so a decode writes the stop token and no span job
// starts there.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1XStopTokenInsideDraftStateTest, "SuperSLM.SchemaJump.P1X.StopTokenInsideDraftState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1XStopTokenInsideDraftStateTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), BaseConfig(1, 1), Rig)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], kExact, 256, Req)) { return false; }
	FRunResult Off;
	if (!RunOff(*this, Rig, Req, Off)) { return false; }
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Off.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	FRunResult Learn;
	if (!RunSolo(*this, Rig, Req, Learn)) { return false; }
	const TArray<FSuperSLMSpanJobRecord> LearnSpans = SpanJobsOf(*Rig.Cpu, Learn.Sequence);

	// The stop: id 0 of a span job's confirmed ids, or one strictly inside them (bMiddle), at the token's first
	// occurrence in the stream, with a span job admitting tokens before it (an earlier one, or in the middle case
	// this one). Returns the token and sets its stream position, or returns INDEX_NONE.
	auto ChooseStop = [&](bool bMiddle, int32& OutPosition) -> int32
	{
		for (const FSuperSLMSpanJobRecord& S : LearnSpans)
		{
			if (S.OutputIndexBefore < 0 || S.ForcedIds.Num() < (bMiddle ? 3 : 2)) { continue; }
			const int32 Offset = bMiddle ? S.ForcedIds.Num() / 2 : 0;
			const int32 Token = S.ForcedIds[Offset];
			const int32 Position = S.OutputIndexBefore + Offset;
			if (Learn.Tokens.IndexOfByKey(Token) != Position) { continue; }
			bool bSpanBefore = bMiddle;
			for (const FSuperSLMSpanJobRecord& E : LearnSpans)
			{
				bSpanBefore |= E.JobId < S.JobId && E.ForcedIds.Num() > 0 && E.OutputIndexBefore >= 0 && E.OutputIndexBefore < Position;
			}
			if (bSpanBefore) { OutPosition = Position; return Token; }
		}
		return INDEX_NONE;
	};

	bool bOk = true;
	for (const bool bMiddle : { false, true })
	{
		const TCHAR* Case = bMiddle ? TEXT("stop in a draft's middle") : TEXT("stop at a draft's first id");
		int32 StopPosition = INDEX_NONE;
		const int32 StopToken = ChooseStop(bMiddle, StopPosition);
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: the learning run has such a span job (%d span jobs)"), Case, LearnSpans.Num()),
			StopToken != INDEX_NONE)) { return false; }

		FSuperSLMGenerationRequest Stopped = Req;
		Stopped.StopTokenIds.Add(StopToken);
		FSuperSLMGenerationRequest OffStopped = Stopped;
		OffStopped.SchemaJumpForward = kOff;
		FRunResult Exact, OffRun;
		TArray<uint8> ExactBlob, OffBlob;
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		if (!RunSolo(*this, Rig, Stopped, Exact, /*bUnbound*/ false, /*bKeep*/ true) || !SaveBlob(*this, Rig, Exact.Sequence, ExactBlob)) { return false; }
		const TArray<FSuperSLMSpanJobRecord> Spans = SpanJobsOf(*Rig.Cpu, Exact.Sequence);
		Rig.Cpu->ReturnSequence(Exact.Sequence);
		Settle(*Rig.Cpu);
		if (!RunSolo(*this, Rig, OffStopped, OffRun, /*bUnbound*/ false, /*bKeep*/ true) || !SaveBlob(*this, Rig, OffRun.Sequence, OffBlob)) { return false; }
		Rig.Cpu->ReturnSequence(OffRun.Sequence);
		Settle(*Rig.Cpu);

		bOk &= RequireSpanActivity(*this, Spans, Case);
		for (const FSuperSLMSpanJobRecord& S : Spans)
		{
			bOk &= TestFalse(*FString::Printf(TEXT("%s: span job %lld's draft does not carry the stop token %d"), Case, S.JobId, StopToken),
				S.Draft.Contains(StopToken));
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the run stops at the stop token (position %d)"), Case, StopPosition), Exact.Tokens.Num(), StopPosition + 1);
		int32 CutAtStop = 0;
		for (const FSuperSLMSpanJobRecord& S : Spans)
		{
			CutAtStop += S.DraftLengthBeforeStopCut > S.Draft.Num() && S.OutputIndexBefore + S.Draft.Num() == StopPosition ? 1 : 0;
		}
		if (bMiddle)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("%s: one span job's draft was cut at the stop's position %d"), Case, StopPosition), CutAtStop, 1);
		}
		else
		{
			for (const FSuperSLMSpanJobRecord& S : Spans)
			{
				bOk &= TestNotEqual(*FString::Printf(TEXT("%s: no span job starts at the stop (span job %lld)"), Case, S.JobId), S.OutputIndexBefore, StopPosition);
			}
		}
		bOk &= SameEnding(*this, Exact, OffRun, *FString::Printf(TEXT("%s: exact equals off with the same stop token"), Case));
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the saved blobs are byte-identical"), Case), ExactBlob == OffBlob);
	}
	return bOk;
}

// P1.2's drain rule against ground truth (review finding 4): with a chunk budget that splits chains
// and no held request, no decode job is planned for a Canonical sequence while the engine reports a
// chain (the seam reads sslm_seq_forced_tokens itself before each such decode).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaJumpP1DrainGroundTruthTest, "SuperSLM.SchemaJump.P1.DrainGroundTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaJumpP1DrainGroundTruthTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this) || !ReviewSeamGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	FSuperSLMRuntimeConfig Config = BaseConfig(1, 1);
	Config.MaxPrefillChunkBudget = 3;
	if (!SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	bool bOk = true;
	for (const FString& Prompt : PopulationPrompts())
	{
		FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
		const int32 Before = FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain(*Rig.Cpu);
		FSuperSLMGenerationRequest Req;
		FRunResult Run;
		if (!MakeRequest(*this, Rig, Prompt, kCanon, 256, Req) || !RunSolo(*this, Rig, Req, Run)) { return false; }
		bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Run.Sequence), *Prompt);
		bOk &= TestEqual(*FString::Printf(TEXT("'%s': no decode planned with a chain open"), *Prompt),
			FSuperSLMSchemaJumpReviewTestAccess::GetCanonicalDecodesPlannedWithOpenChain(*Rig.Cpu) - Before, 0);
		bOk &= TestTrue(TEXT("the seam's counter is live"), Before >= 0);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
