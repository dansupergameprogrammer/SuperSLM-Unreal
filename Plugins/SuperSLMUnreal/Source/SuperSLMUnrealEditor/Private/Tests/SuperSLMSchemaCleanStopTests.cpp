// Plugin 1.5.0: the clean stop at the schema's end (design note "Clean stop at the schema's end", §3 contract, §4
// cells P1-P5), against A-EX under potion_shop_order.
//
// SuperSLM reports the schema's end as -2 at SSLM_OK and keeps the walk, so the accepting reading taken after the -2
// classifies it. The plugin's rule: accepting -> Complete / SchemaComplete (a clean stop, with tokens still left in
// MaxNewTokens); not accepting -> Faulted / SchemaDeadEnd, as before.
//   P1  CPU: the pinned prompt's generation ends Complete / SchemaComplete before MaxNewTokens, accepting;
//       ComposeStopReason is Completed.
//   P2  GPU one-row (bGpuMultiRow off, whole tokens): the same, its tokens and ending equal the CPU's, and the
//       completed sequence saves and restores Complete, accepting, Completed.
//   P3  GPU multi-row, two cells. PromptRowsOneRowEnd (Off): the prompt is read by rows, but decoding is one-row, so
//       the end is a one-row finish's (ApplyResult), the site P2 covers; pinned. ExactCloseEnd (Exact, no table
//       seeded by the cell): the end is met in an Exact close, after at least one draft (ApplyJumpResult); pinned.
//   P4  GPU jump (Exact over a seeded table, multi-row on): the same, the end met in an Exact close after at least one
//       delivered draft (CloseRows -> ApplyJumpResult; pinned), and the completed sequence saves and restores.
//   P5  A -2 whose walk is NOT accepting (SuperSLM's degenerate-row seam, armed for the first finish), on the CPU and
//       on the GPU: Faulted / SchemaDeadEnd, not accepting, ComposeStopReason SchemaRejected; the GPU save is refused.
//   P6  The restore site: a sequence saved at its accepting end, with the -2 not yet applied, restores and ends
//       Complete / SchemaComplete, accepting, its tokens the CPU's.
//       GPU: the save carries the -2 as a pending event (the 'SLUG' wrapper's carried events), and the restore rebuilds
//       it; FinalizeRestoreOp() gives it the walk's accepting reading taken by the restore job. Arms save at N - b
//       tokens (N the CPU's Off count, b in 0, 1, 2, 4, 8); an arm whose save carried no -2 is skipped, and at least
//       one arm must carry it.
//       CPU: an unapplied -2 cannot be saved. A CPU save is a lifecycle op, admitted only when the sequence has no
//       undelivered job, so the job that finds the -2 is either not yet planned or already delivered. The cell takes
//       the closest state: held (the job hold, AfterDecodeDelivery) right after the delivery of token N, so the -2's
//       job is not yet planned; saved there; restored; the restored sequence's first decode is the -2. The cell turns
//       the worker continuation off: a continuation is posted behind the running job before that job's delivery sets
//       the hold (PlanContinuations does not read the hold), so with it on the -2's job is already queued at the hold.
//   P7  An Exact close's -2 whose walk is NOT accepting (FSuperSLMGpuMultiRowTestAccess::ArmExactCloseDeadEnd):
//       NoDraft, at the close's row 0 (the engine's degenerate-row seam armed right before the close; CloseRows() hands
//       on the one-row result, ApplyResult); AfterDrafts, after the close's committed drafts (the close's report
//       substituted, ApplyJumpResult). Both: Faulted / SchemaDeadEnd, not accepting, SchemaRejected, save refused.
//   P8  GPU: a save carrying an unapplied -2 whose walk is NOT accepting (the degenerate-row seam at the first finish)
//       restores, and the rebuilt -2 ends the restored sequence Faulted / SchemaDeadEnd, not accepting.
//   P9  A sequence saved AFTER its clean stop restores Complete, accepting, ComposeStopReason Completed, with no
//       token after the restore: on the CPU, and on the GPU with its CPU twin (code review finding 1).
//   P10 CPU batch: two schema-bound sequences decoded by one job, one of which ends there while the other goes on.
//       Accepting: the one ends Complete / SchemaComplete, the other ends as its solo run, tokens included. Not
//       accepting (the CPU seam armed for the batch's first finish): exactly one ends Faulted / SchemaDeadEnd, and the
//       other ends as its solo run.
//
// Not covered: a -2 at an accepting state whose mask is NOT empty (design §2, the coverage audit's note 10). A-EX's
// schemas each have one accepting state, with no legal token (the 2026-10-04 dead-end investigation's static walk), so
// no state here is accepting with a token still allowed. The plugin's rule reads only the accepting state, never the
// mask, so the claim is unproven here rather than untestable in principle: it needs a schema with such a state.
//
// The pure composition (Complete / SchemaComplete with the walk accepting -> Completed) is also in
// SuperSLML2S3StopReasonTests.cpp.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "SuperSLMPromptResult.h"
#include "SuperSLMSchemaJumpTestSeam.h"
#include "Tests/AutomationCommon.h"

#if SUPERSLMUNREAL_WITH_GPU
#include "Fixtures/SuperSLMGpuSchemaJumpFixtures.h"
#include "SuperSLMGpuMultiRowTestSeam.h"
#endif

namespace SuperSLMSchemaCleanStop
{
	namespace CJ = SuperSLMSchemaJumpFixtures;

	constexpr int32 kMaxNew = 96; // well past the pinned prompt's object (29 tokens to its end, measured)

	bool ExpectStopReason(FAutomationTestBase& T, ESuperSLMSequencePhase Phase, ESuperSLMDecodeOutcome Outcome, bool bAccepting,
		ESuperSLMQueryStopReason Expected, const FString& What)
	{
		SuperSLMPromptResult::FStopFacts Facts;
		Facts.Phase = Phase;
		Facts.LastOutcome = Outcome;
		Facts.bSchemaAccepting = bAccepting;
		ESuperSLMQueryStopReason Reason = ESuperSLMQueryStopReason::BudgetExhausted;
		const bool bComposed = SuperSLMPromptResult::ComposeStopReason(Facts, Reason);
		return T.TestTrue(*(What + TEXT(": ComposeStopReason composes")), bComposed) &&
			T.TestEqual(*FString::Printf(TEXT("%s: ComposeStopReason gives %d"), *What, (int32)Expected), (uint8)Reason, (uint8)Expected);
	}

	// The clean stop, read the same way on every backend.
	bool ExpectCleanStop(FAutomationTestBase& T, ESuperSLMSequencePhase Phase, ESuperSLMDecodeOutcome Outcome, bool bAccepting,
		int32 Tokens, int32 MaxNew, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": ends Complete")), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete);
		bOk &= T.TestEqual(*(What + TEXT(": with SchemaComplete")), (uint8)Outcome, (uint8)ESuperSLMDecodeOutcome::SchemaComplete);
		bOk &= T.TestTrue(*(What + TEXT(": the walk is accepting")), bAccepting);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: tokens are left in the allowance (%d of %d)"), *What, Tokens, MaxNew), Tokens >= 1 && Tokens < MaxNew);
		bOk &= ExpectStopReason(T, Phase, Outcome, bAccepting, ESuperSLMQueryStopReason::Completed, What);
		return bOk;
	}

	bool ExpectDeadEnd(FAutomationTestBase& T, ESuperSLMSequencePhase Phase, ESuperSLMDecodeOutcome Outcome, bool bAccepting, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": ends Faulted")), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= T.TestEqual(*(What + TEXT(": with SchemaDeadEnd")), (uint8)Outcome, (uint8)ESuperSLMDecodeOutcome::SchemaDeadEnd);
		bOk &= T.TestFalse(*(What + TEXT(": the walk is not accepting")), bAccepting);
		bOk &= ExpectStopReason(T, Phase, Outcome, bAccepting, ESuperSLMQueryStopReason::SchemaRejected, What);
		return bOk;
	}
}

using namespace SuperSLMSchemaCleanStop;

// P1 CPU: the schema's end with tokens left is a clean stop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP1CpuTest, "SuperSLM.SchemaCleanStop.P1.Cpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP1CpuTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	SuperSLMSchemaCleanStop::CJ::FCpuRig Rig;
	if (!SuperSLMSchemaCleanStop::CJ::SetUpCpu(*this, W.GetTestWorld(), SuperSLMSchemaCleanStop::CJ::BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	SuperSLMSchemaCleanStop::CJ::FRunResult Run;
	if (!SuperSLMSchemaCleanStop::CJ::MakeRequest(*this, Rig, SuperSLMSchemaCleanStop::CJ::PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req) ||
		!SuperSLMSchemaCleanStop::CJ::RunSolo(*this, Rig, Req, Run)) { return false; }
	return SuperSLMSchemaCleanStop::ExpectCleanStop(*this, Run.Phase, Run.Outcome, Run.Stats.bSchemaAccepting, Run.Tokens.Num(), SuperSLMSchemaCleanStop::kMaxNew, TEXT("P1 CPU"));
}

// P5 CPU: a -2 whose walk is not accepting stays a fault.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP5CpuTest, "SuperSLM.SchemaCleanStop.P5.CpuDeadEndStaysFaulted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP5CpuTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	SuperSLMSchemaCleanStop::CJ::FCpuRig Rig;
	if (!SuperSLMSchemaCleanStop::CJ::SetUpCpu(*this, W.GetTestWorld(), SuperSLMSchemaCleanStop::CJ::BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::CJ::MakeRequest(*this, Rig, SuperSLMSchemaCleanStop::CJ::PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	// The first schema-bound finish of this generation sees the degenerate row: a -2 at the schema's start state, which
	// is not accepting (an empty document is not a potion order).
	FSuperSLMSchemaJumpTestAccess::ArmCpuDegenerateFinishRow();
	SuperSLMSchemaCleanStop::CJ::FRunResult Run;
	if (!SuperSLMSchemaCleanStop::CJ::RunSolo(*this, Rig, Req, Run)) { return false; }
	bool bOk = SuperSLMSchemaCleanStop::ExpectDeadEnd(*this, Run.Phase, Run.Outcome, Run.Stats.bSchemaAccepting, TEXT("P5 CPU"));
	bOk &= TestEqual(TEXT("P5 CPU: the dead end is at the first finish (no token produced)"), Run.Tokens.Num(), 0);
	return bOk;
}

// P6 CPU, the restore site: saved right after token N's delivery (the -2's job not yet planned), restored, ended
// cleanly. The literal case, a -2 found but not applied at the save, cannot be built on the CPU (see the file's head).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP6CpuTest, "SuperSLM.SchemaCleanStop.P6.CpuRestoreAtAcceptingEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP6CpuTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	SuperSLMSchemaCleanStop::CJ::FCpuRig Rig;
	// No worker continuation (1.0's one-job hand-off). With it on, the next decode job is posted behind the running one
	// before the running one is delivered, and PlanContinuations does not read the job hold: at the hold after token N
	// the -2's job is already queued, and the save, admitted only once it is delivered, finds the sequence Complete.
	// Tokens are the same either way (FSuperSLMRuntimeConfig::bWorkerContinuation).
	FSuperSLMRuntimeConfig Config = SuperSLMSchemaCleanStop::CJ::BaseConfig(1, 1);
	Config.bWorkerContinuation = false;
	if (!SuperSLMSchemaCleanStop::CJ::SetUpCpu(*this, W.GetTestWorld(), Config, Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	SuperSLMSchemaCleanStop::CJ::FRunResult Off;
	if (!SuperSLMSchemaCleanStop::CJ::MakeRequest(*this, Rig, SuperSLMSchemaCleanStop::CJ::PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req) ||
		!SuperSLMSchemaCleanStop::CJ::RunSolo(*this, Rig, Req, Off)) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the unsaved run ends Complete / SchemaComplete (%d tokens)"), Off.Tokens.Num()),
		Off.Phase == ESuperSLMSequencePhase::Complete && Off.Outcome == ESuperSLMDecodeOutcome::SchemaComplete && Off.Tokens.Num() >= 1)) { return false; }
	const int32 N = Off.Tokens.Num();

	// Held after every decode delivery; released until token N has been delivered.
	FSuperSLMSequence Seq;
	FString Error;
	if (!TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	if (!TestTrue(*Error, Rig.Cpu->SetSchema(Seq, Rig.Schema, Error))) { Rig.Cpu->ReturnSequence(Seq); return false; }
	FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, Seq, ESuperSLMJobHoldPoint::AfterDecodeDelivery);
	if (!TestTrue(*Error, Rig.Cpu->BeginGeneration(Seq, Req, Error))) { Rig.Cpu->ReturnSequence(Seq); return false; }
	bool bHeldAtEnd = false;
	const double Start = FPlatformTime::Seconds();
	while (!SuperSLMSchemaCleanStop::CJ::IsEnded(*Rig.Cpu, Seq) && FPlatformTime::Seconds() - Start < SuperSLMSchemaCleanStop::CJ::kRunSeconds)
	{
		Rig.Cpu->Tick(1.0f / 60.0f);
		if (FSuperSLMSchemaJumpTestAccess::IsJobHeld(*Rig.Cpu, Seq))
		{
			if (Rig.Cpu->GetGeneratedTokens(Seq).Num() >= N) { bHeldAtEnd = true; break; }
			FSuperSLMSchemaJumpTestAccess::ReleaseJobHold(*Rig.Cpu, Seq);
		}
		FPlatformProcess::Sleep(0.001f);
	}
	const TArray<int32> Before = Rig.Cpu->GetGeneratedTokens(Seq);
	if (!TestTrue(*FString::Printf(TEXT("precondition: held right after token %d's delivery, still Decoding (%d tokens)"), N, Before.Num()),
		bHeldAtEnd && Before.Num() == N && Rig.Cpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding))
	{
		Rig.Cpu->ReturnSequence(Seq);
		return false;
	}

	// The save runs while the sequence is held (the hold stops decode planning, not lifecycle ops).
	FSuperSLMLifecycleOpHandle SaveHandle;
	TArray<uint8> Blob;
	if (!TestEqual(*FString::Printf(TEXT("SaveSequence queues (%s)"), *Error), (uint8)Rig.Cpu->SaveSequence(Seq, SaveHandle, Error), (uint8)ESuperSLMRestoreResult::Success))
	{
		Rig.Cpu->ReturnSequence(Seq);
		return false;
	}
	ESuperSLMRestoreResult Saved = ESuperSLMRestoreResult::Pending;
	SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[&]() { Saved = Rig.Cpu->GetSaveResult(SaveHandle, Blob); return Saved != ESuperSLMRestoreResult::Pending; }, 60.0);
	Rig.Cpu->ReleaseLifecycleOpHandle(SaveHandle);
	bool bOk = TestEqual(TEXT("P6 CPU: the save succeeds"), (uint8)Saved, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestTrue(TEXT("P6 CPU: at the save the sequence is still Decoding, its last outcome a produced token (the -2 not yet found)"),
		Rig.Cpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding && Rig.Cpu->GetLastDecodeOutcome(Seq) == ESuperSLMDecodeOutcome::TokenProduced &&
		Rig.Cpu->GetGeneratedTokens(Seq).Num() == N);
	Rig.Cpu->ReturnSequence(Seq); // ends the hold
	SuperSLMSchemaCleanStop::CJ::Settle(*Rig.Cpu);
	if (!bOk) { return false; }

	FSuperSLMSequence Restored;
	FSuperSLMLifecycleOpHandle RestoreHandle;
	if (!TestEqual(*FString::Printf(TEXT("RestoreSequence queues (%s)"), *Error), (uint8)Rig.Cpu->RestoreSequence(Blob, Rig.Model, Restored, RestoreHandle, Error),
		(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[&]() { return Rig.Cpu->GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Pending; }, 60.0);
	const ESuperSLMRestoreResult RestoreResult = Rig.Cpu->GetLifecycleOpResult(RestoreHandle);
	Rig.Cpu->ReleaseLifecycleOpHandle(RestoreHandle);
	if (!TestEqual(TEXT("P6 CPU: the restore succeeds"), (uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success)) { Rig.Cpu->ReturnSequence(Restored); return false; }
	SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[&]() { return SuperSLMSchemaCleanStop::CJ::IsEnded(*Rig.Cpu, Restored); }, SuperSLMSchemaCleanStop::CJ::kRunSeconds);
	bOk &= TestTrue(TEXT("P6 CPU: the restored sequence ends"), SuperSLMSchemaCleanStop::CJ::IsEnded(*Rig.Cpu, Restored));
	const TArray<int32> Tail = Rig.Cpu->GetGeneratedTokens(Restored);
	TArray<int32> Joined = Before;
	Joined.Append(Tail);
	bOk &= SuperSLMSchemaCleanStop::ExpectCleanStop(*this, Rig.Cpu->GetPhase(Restored), Rig.Cpu->GetLastDecodeOutcome(Restored),
		Rig.Cpu->GetStats(Restored).bSchemaAccepting, Joined.Num(), SuperSLMSchemaCleanStop::kMaxNew, TEXT("P6 CPU"));
	bOk &= TestEqual(TEXT("P6 CPU: the restored sequence's first decode is the -2 (no token after the restore)"), Tail.Num(), 0);
	bOk &= TestEqual(TEXT("P6 CPU: saved + continued tokens equal the unsaved run's"), Joined, Off.Tokens);
	Rig.Cpu->ReturnSequence(Restored);
	SuperSLMSchemaCleanStop::CJ::Settle(*Rig.Cpu);
	return bOk;
}

namespace SuperSLMSchemaCleanStop
{
	// Saves Seq, which ended at its schema's end cleanly, restores the blob, and reads the restored sequence: Complete,
	// accepting, ComposeStopReason Completed, and no token after the restore (P9; code review finding 1, coverage audit
	// gap 7). Seq is returned once saved, so a one-block configuration has room for the restore.
	bool CheckCpuRestoreAfterCleanStop(FAutomationTestBase& T, const CJ::FCpuRig& Rig, const FSuperSLMSequence& Seq, const FString& What)
	{
		FString Error;
		FSuperSLMLifecycleOpHandle SaveHandle;
		TArray<uint8> Blob;
		if (!T.TestEqual(*FString::Printf(TEXT("%s: SaveSequence queues (%s)"), *What, *Error), (uint8)Rig.Cpu->SaveSequence(Seq, SaveHandle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { Rig.Cpu->ReturnSequence(Seq); CJ::Settle(*Rig.Cpu); return false; }
		ESuperSLMRestoreResult Saved = ESuperSLMRestoreResult::Pending;
		SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { Saved = Rig.Cpu->GetSaveResult(SaveHandle, Blob); return Saved != ESuperSLMRestoreResult::Pending; }, 60.0);
		Rig.Cpu->ReleaseLifecycleOpHandle(SaveHandle);
		Rig.Cpu->ReturnSequence(Seq);
		CJ::Settle(*Rig.Cpu);
		if (!T.TestEqual(*(What + TEXT(": the save after the clean stop succeeds")), (uint8)Saved, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		FSuperSLMSequence Restored;
		FSuperSLMLifecycleOpHandle RestoreHandle;
		if (!T.TestEqual(*FString::Printf(TEXT("%s: RestoreSequence queues (%s)"), *What, *Error), (uint8)Rig.Cpu->RestoreSequence(Blob, Rig.Model, Restored, RestoreHandle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		SuperSLML2S1Fixtures::DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { return Rig.Cpu->GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Pending; }, 60.0);
		const ESuperSLMRestoreResult RestoreResult = Rig.Cpu->GetLifecycleOpResult(RestoreHandle);
		Rig.Cpu->ReleaseLifecycleOpHandle(RestoreHandle);
		bool bOk = T.TestEqual(*(What + TEXT(": the restore succeeds")), (uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success);
		if (bOk)
		{
			CJ::Settle(*Rig.Cpu); // ticks: nothing may happen to a restored completed sequence
			const ESuperSLMSequencePhase Phase = Rig.Cpu->GetPhase(Restored);
			const ESuperSLMDecodeOutcome Outcome = Rig.Cpu->GetLastDecodeOutcome(Restored);
			const bool bAccepting = Rig.Cpu->GetStats(Restored).bSchemaAccepting;
			T.AddInfo(FString::Printf(TEXT("%s: the restored sequence's last decode outcome is %d"), *What, (int32)Outcome));
			bOk &= T.TestEqual(*(What + TEXT(": the restored sequence is Complete")), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete);
			bOk &= T.TestTrue(*(What + TEXT(": the restored walk is accepting")), bAccepting);
			bOk &= ExpectStopReason(T, Phase, Outcome, bAccepting, ESuperSLMQueryStopReason::Completed, What + TEXT(", restored"));
			bOk &= T.TestEqual(*(What + TEXT(": no token after the restore")), Rig.Cpu->GetGeneratedTokens(Restored).Num(), 0);
		}
		Rig.Cpu->ReturnSequence(Restored);
		CJ::Settle(*Rig.Cpu);
		return bOk;
	}

	// The last decode job that served Ending (the one that delivered its last outcome) also served Other.
	bool LastDecodeJobServedBoth(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Ending, const FSuperSLMSequence& Other)
	{
		int64 Last = -1;
		for (const int64 Job : CJ::DecodeJobIdsOf(Cpu, Ending)) { Last = FMath::Max(Last, Job); }
		if (Last < 0) { return false; }
		for (const FSuperSLMWorkerJobReport& R : Cpu.GetJobLedger())
		{
			if (R.JobId == Last) { return R.MemberSequences.Contains(Other); }
		}
		return false;
	}

	// P10: begins A and B on two bound sequences in one tick, holds each before its next decode until both are held,
	// then releases both at once, so the next decode job serves them together (P1.7's gather). With bArmSeam the CPU
	// degenerate-row seam is armed at that release, so the batch's first schema-bound finish is a -2 whose walk is not
	// accepting. Both run to their ends; Out holds the readings, the ledger's included, taken before the sequences are
	// returned.
	struct FPairRun
	{
		CJ::FRunResult A, B;
		bool bOneJobServedBoth = false; // some decode job served both
		bool bAEndedBesideB = false;    // A's last decode job also served B
		bool bBEndedBesideA = false;    // B's last decode job also served A
	};

	bool RunPairBatched(FAutomationTestBase& T, const CJ::FCpuRig& Rig, const FSuperSLMGenerationRequest& ReqA, const FSuperSLMGenerationRequest& ReqB,
		bool bArmSeam, FPairRun& Out)
	{
		Out = FPairRun();
		FString Error;
		CJ::FRunResult* Runs[2] = { &Out.A, &Out.B };
		const FSuperSLMGenerationRequest* Reqs[2] = { &ReqA, &ReqB };
		for (CJ::FRunResult* R : Runs)
		{
			if (!T.TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(R->Sequence), (uint8)ESuperSLMVendResult::Success)) { return false; }
			if (!T.TestTrue(TEXT("schema bind"), Rig.Cpu->SetSchema(R->Sequence, Rig.Schema, Error))) { return false; }
		}
		for (int32 I = 0; I < 2; ++I)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration (%s)"), *Error), Rig.Cpu->BeginGeneration(Runs[I]->Sequence, *Reqs[I], Error))) { return false; }
			FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, Runs[I]->Sequence, ESuperSLMJobHoldPoint::BeforeDecode);
		}
		bool bGathering = true;
		const double Start = FPlatformTime::Seconds();
		auto BothEnded = [&]() { return CJ::IsEnded(*Rig.Cpu, Out.A.Sequence) && CJ::IsEnded(*Rig.Cpu, Out.B.Sequence); };
		while (!BothEnded() && FPlatformTime::Seconds() - Start < CJ::kRunSeconds)
		{
			Rig.Cpu->Tick(1.0f / 60.0f);
			if (bGathering && FSuperSLMSchemaJumpTestAccess::IsJobHeld(*Rig.Cpu, Out.A.Sequence) && FSuperSLMSchemaJumpTestAccess::IsJobHeld(*Rig.Cpu, Out.B.Sequence))
			{
				if (bArmSeam)
				{
					FSuperSLMSchemaJumpTestAccess::ArmCpuDegenerateFinishRow();
				}
				for (CJ::FRunResult* R : Runs) { FSuperSLMSchemaJumpTestAccess::SetJobHold(*Rig.Cpu, R->Sequence, ESuperSLMJobHoldPoint::None); }
				bGathering = false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		bool bOk = T.TestFalse(TEXT("both sequences were held before a decode and released together"), bGathering);
		bOk &= T.TestTrue(TEXT("both sequences end"), BothEnded());
		for (CJ::FRunResult* R : Runs)
		{
			R->Phase = Rig.Cpu->GetPhase(R->Sequence);
			R->Tokens = Rig.Cpu->GetGeneratedTokens(R->Sequence);
			R->Outcome = Rig.Cpu->GetLastDecodeOutcome(R->Sequence);
			R->Stats = Rig.Cpu->GetStats(R->Sequence);
		}
		Out.bOneJobServedBoth = CJ::SomeDecodeJobServedAll(*Rig.Cpu, { Out.A.Sequence, Out.B.Sequence });
		Out.bAEndedBesideB = LastDecodeJobServedBoth(*Rig.Cpu, Out.A.Sequence, Out.B.Sequence);
		Out.bBEndedBesideA = LastDecodeJobServedBoth(*Rig.Cpu, Out.B.Sequence, Out.A.Sequence);
		for (CJ::FRunResult* R : Runs) { Rig.Cpu->ReturnSequence(R->Sequence); }
		CJ::Settle(*Rig.Cpu);
		return bOk;
	}
}

// P9 CPU: a sequence saved after its clean stop restores Complete, accepting, Completed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP9CpuTest, "SuperSLM.SchemaCleanStop.P9.CpuRestoreAfterCleanStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP9CpuTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	SuperSLMSchemaCleanStop::CJ::FCpuRig Rig;
	if (!SuperSLMSchemaCleanStop::CJ::SetUpCpu(*this, W.GetTestWorld(), SuperSLMSchemaCleanStop::CJ::BaseConfig(1, 1), Rig)) { return false; }
	FSuperSLMGenerationRequest Req;
	SuperSLMSchemaCleanStop::CJ::FRunResult Run;
	if (!SuperSLMSchemaCleanStop::CJ::MakeRequest(*this, Rig, SuperSLMSchemaCleanStop::CJ::PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req) ||
		!SuperSLMSchemaCleanStop::CJ::RunSolo(*this, Rig, Req, Run, /*bUnbound*/ false, /*bKeep*/ true)) { Rig.Cpu->ReturnSequence(Run.Sequence); return false; }
	bool bOk = SuperSLMSchemaCleanStop::ExpectCleanStop(*this, Run.Phase, Run.Outcome, Run.Stats.bSchemaAccepting, Run.Tokens.Num(), SuperSLMSchemaCleanStop::kMaxNew, TEXT("P9 CPU"));
	bOk &= SuperSLMSchemaCleanStop::CheckCpuRestoreAfterCleanStop(*this, Rig, Run.Sequence, TEXT("P9 CPU")); // returns Run.Sequence
	return bOk;
}

// P10 CPU batch: one of two sequences in one decode job ends there; the other goes on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP10CpuBatchTest, "SuperSLM.SchemaCleanStop.P10.CpuBatchOneEnds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP10CpuBatchTest::RunTest(const FString& Parameters)
{
	namespace CS = SuperSLMSchemaCleanStop;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	CS::CJ::FCpuRig Rig;
	if (!CS::CJ::SetUpCpu(*this, W.GetTestWorld(), CS::CJ::BaseConfig(4, 4), Rig)) { return false; }
	// A ends first; B is the first population prompt whose solo run ends cleanly at least two tokens after A's.
	FSuperSLMGenerationRequest ReqA, ReqB;
	CS::CJ::FRunResult SoloA, SoloB;
	if (!CS::CJ::MakeRequest(*this, Rig, CS::CJ::PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, CS::kMaxNew, ReqA) ||
		!CS::CJ::RunSolo(*this, Rig, ReqA, SoloA)) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: A's solo run ends Complete / SchemaComplete (%d tokens)"), SoloA.Tokens.Num()),
		SoloA.Phase == ESuperSLMSequencePhase::Complete && SoloA.Outcome == ESuperSLMDecodeOutcome::SchemaComplete)) { return false; }
	bool bFoundB = false;
	for (int32 I = 1; I < CS::CJ::PopulationPrompts().Num() && !bFoundB; ++I)
	{
		if (!CS::CJ::MakeRequest(*this, Rig, CS::CJ::PopulationPrompts()[I], ESuperSLMSchemaJumpForward::Off, CS::kMaxNew, ReqB) ||
			!CS::CJ::RunSolo(*this, Rig, ReqB, SoloB)) { return false; }
		bFoundB = SoloB.Phase == ESuperSLMSequencePhase::Complete && SoloB.Outcome == ESuperSLMDecodeOutcome::SchemaComplete &&
			SoloB.Tokens.Num() >= SoloA.Tokens.Num() + 2;
	}
	if (!TestTrue(*FString::Printf(TEXT("precondition: a population prompt ends cleanly at least two tokens after A's %d"), SoloA.Tokens.Num()), bFoundB)) { return false; }

	// Accepting: A ends in a decode job that also serves B, and B goes on to its own end.
	CS::FPairRun Pair;
	bool bOk = CS::RunPairBatched(*this, Rig, ReqA, ReqB, /*bArmSeam*/ false, Pair);
	bOk &= TestTrue(TEXT("P10 accepting: precondition: one decode job served both"), Pair.bOneJobServedBoth);
	bOk &= TestTrue(TEXT("P10 accepting: A's ending decode job also served B (one ends, one goes on)"), Pair.bAEndedBesideB);
	bOk &= CS::ExpectCleanStop(*this, Pair.A.Phase, Pair.A.Outcome, Pair.A.Stats.bSchemaAccepting, Pair.A.Tokens.Num(), CS::kMaxNew, TEXT("P10 accepting, A"));
	bOk &= TestEqual(TEXT("P10 accepting, A: tokens equal its solo run's"), Pair.A.Tokens, SoloA.Tokens);
	bOk &= CS::ExpectCleanStop(*this, Pair.B.Phase, Pair.B.Outcome, Pair.B.Stats.bSchemaAccepting, Pair.B.Tokens.Num(), CS::kMaxNew, TEXT("P10 accepting, B"));
	bOk &= TestEqual(TEXT("P10 accepting, B: tokens equal its solo run's"), Pair.B.Tokens, SoloB.Tokens);

	// Not accepting: the batch's first schema-bound finish is the degenerate row. Exactly one member dead-ends there; the
	// other goes on to its solo run's end.
	if (!CS::RunPairBatched(*this, Rig, ReqA, ReqB, /*bArmSeam*/ true, Pair)) { return false; }
	bOk &= TestTrue(TEXT("P10 dead end: precondition: one decode job served both"), Pair.bOneJobServedBoth);
	const bool bAFaulted = Pair.A.Phase == ESuperSLMSequencePhase::Faulted;
	const bool bBFaulted = Pair.B.Phase == ESuperSLMSequencePhase::Faulted;
	if (!TestTrue(*FString::Printf(TEXT("P10 dead end: exactly one member faulted (A %d, B %d)"), (int32)bAFaulted, (int32)bBFaulted), bAFaulted != bBFaulted)) { return false; }
	const CS::CJ::FRunResult& Dead = bAFaulted ? Pair.A : Pair.B;
	const CS::CJ::FRunResult& Live = bAFaulted ? Pair.B : Pair.A;
	const CS::CJ::FRunResult& DeadSolo = bAFaulted ? SoloA : SoloB;
	const CS::CJ::FRunResult& LiveSolo = bAFaulted ? SoloB : SoloA;
	const FString D = bAFaulted ? TEXT("P10 dead end, A") : TEXT("P10 dead end, B");
	const FString L = bAFaulted ? TEXT("P10 dead end, B (goes on)") : TEXT("P10 dead end, A (goes on)");
	bOk &= TestTrue(*(D + TEXT(": its ending decode job also served the other member")), bAFaulted ? Pair.bAEndedBesideB : Pair.bBEndedBesideA);
	bOk &= CS::ExpectDeadEnd(*this, Dead.Phase, Dead.Outcome, Dead.Stats.bSchemaAccepting, D);
	bOk &= TestTrue(*FString::Printf(TEXT("%s: its %d tokens are a strict prefix of its solo run's %d"), *D, Dead.Tokens.Num(), DeadSolo.Tokens.Num()),
		Dead.Tokens.Num() < DeadSolo.Tokens.Num() && CS::CJ::IsPrefixOf(Dead.Tokens, DeadSolo.Tokens));
	bOk &= CS::ExpectCleanStop(*this, Live.Phase, Live.Outcome, Live.Stats.bSchemaAccepting, Live.Tokens.Num(), CS::kMaxNew, L);
	bOk &= TestEqual(*(L + TEXT(": tokens equal its solo run's")), Live.Tokens, LiveSolo.Tokens);
	return bOk;
}

#if SUPERSLMUNREAL_WITH_GPU
namespace SuperSLMSchemaCleanStop
{
	namespace GJ = SuperSLMGpuSchemaJumpFixtures;
	using FMr = FSuperSLMGpuMultiRowTestAccess;

	struct FGpuCell
	{
		FTestWorldWrapper W;
		GJ::FGpuRig Rig;
		bool Open(FAutomationTestBase& T, bool bMultiRow)
		{
			if (!T.TestTrue(TEXT("the multi-row test seam is built"), FMr::IsBuilt())) { return false; }
			FSuperSLMGpuRuntimeConfig C = GJ::JumpConfig(GJ::EPath::WholeToken);
			C.bGpuMultiRow = bMultiRow;
			return W.CreateTestWorld(EWorldType::Game) && GJ::SetUp(T, W.GetTestWorld(), C, Rig);
		}
	};

	int32 CountKind(const TArray<FSuperSLMGpuRowsActionRecord>& Acts, ESuperSLMGpuRowsActionKind Kind)
	{
		int32 N = 0;
		for (const FSuperSLMGpuRowsActionRecord& A : Acts) { N += A.Kind == Kind ? 1 : 0; }
		return N;
	}

	TArray<FSuperSLMGpuRowsActionRecord> ActionsOf(const USuperSLMGpuSubsystem& Gpu, int64 SequenceId)
	{
		TArray<FSuperSLMGpuRowsActionRecord> Out;
		for (const FSuperSLMGpuRowsActionRecord& A : FMr::GetActions(Gpu))
		{
			if (A.SequenceId == SequenceId) { Out.Add(A); }
		}
		return Out;
	}

	// Runs Req on a fresh bound OneCall sequence, recording its actions; the sequence is kept.
	bool RunKept(FAutomationTestBase& T, FGpuCell& C, const FSuperSLMGenerationRequest& Req, GJ::FGpuRun& Run, TArray<FSuperSLMGpuRowsActionRecord>& Acts)
	{
		Run = GJ::FGpuRun();
		if (!GJ::Vend(T, C.Rig, GJ::EPath::WholeToken, Run)) { return false; }
		FMr::StartRecording(*C.Rig.Gpu);
		const bool bOk = GJ::Drive(T, C.Rig, Req, Run);
		Acts = ActionsOf(*C.Rig.Gpu, Run.Sequence.Id);
		return bOk;
	}

	// Whether a save carried an unapplied -2: a carried event of kind DeadEnd (3, the GPU subsystem's EResultKind) in
	// the v1 wrapper ('SLUG'; the carried count is the u32 at +20; events are 12 bytes from +24: kind, fault, 0, 0,
	// token, delay; GpuJumpCore::EncodeWrapperExt()).
	bool CarriesSchemaEnd(const TArray<uint8>& Blob)
	{
		constexpr uint8 kDeadEndKind = 3;
		const int32 At = GJ::FindWrapperExt(Blob);
		if (At == INDEX_NONE || At + 24 > Blob.Num()) { return false; }
		const int32 Count = static_cast<int32>(Blob[At + 20] | (Blob[At + 21] << 8) | (Blob[At + 22] << 16) | (Blob[At + 23] << 24));
		for (int32 I = 0; I < Count && At + 24 + 12 * I < Blob.Num(); ++I)
		{
			if (Blob[At + 24 + 12 * I] == kDeadEndKind) { return true; }
		}
		return false;
	}

	// The last recorded action whose result met a -2 (the action that ended the sequence), or null.
	const FSuperSLMGpuRowsActionRecord* LastDeadEnd(const TArray<FSuperSLMGpuRowsActionRecord>& Acts)
	{
		const FSuperSLMGpuRowsActionRecord* End = nullptr;
		for (const FSuperSLMGpuRowsActionRecord& A : Acts) { if (A.bDeadEnd) { End = &A; } }
		return End;
	}

	// The -2 was met in an Exact close (not a chunk that fell back to Off's one-row step), and which plugin branch took
	// it: after at least one delivered draft, ApplyJumpResult (JumpEnd DeadEnd); with none, the close's row 0, which
	// CloseRows() hands on as the one-row result, ApplyResult.
	bool ExpectEndInExactClose(FAutomationTestBase& T, const FSuperSLMGpuRowsActionRecord* End, bool bAfterDrafts, const FString& What)
	{
		const FString Seen = End == nullptr ? FString(TEXT("no action met a -2"))
			: FString::Printf(TEXT("kind %d, closed %d, fell back %d, %d drafts delivered"), (int32)End->Kind, (int32)End->bClosedChunk, (int32)End->bFellBack, End->DeliveredTokens.Num());
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the -2 was met in an Exact close, not a fallen-back step (%s)"), *What, *Seen),
			End != nullptr && End->Kind == ESuperSLMGpuRowsActionKind::ExactRows && End->bClosedChunk && !End->bFellBack);
		if (End != nullptr)
		{
			bOk &= bAfterDrafts
				? T.TestTrue(*FString::Printf(TEXT("%s: the close delivered drafts before the -2 (ApplyJumpResult; %d)"), *What, End->DeliveredTokens.Num()), End->DeliveredTokens.Num() >= 1)
				: T.TestEqual(*(What + TEXT(": the close's row 0 met the -2, no draft delivered (CloseRows -> ApplyResult)")), End->DeliveredTokens.Num(), 0);
		}
		return bOk;
	}

	// Restores Blob, a save taken after a clean stop, and reads the restored sequence: Complete, accepting (code review
	// finding 1: FinalizeRestoreOp() sets it from the restore job's reading), ComposeStopReason Completed, and no token
	// after the restore, over a few more ticks.
	bool CheckGpuRestoreAfterCleanStop(FAutomationTestBase& T, FGpuCell& C, const TArray<uint8>& Blob, const FString& What)
	{
		GJ::FGpuRun After;
		if (!T.TestEqual(*(What + TEXT(": the restore of the save after the clean stop succeeds")), (uint8)GJ::Restore(C.Rig, Blob, After.Sequence),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		int32 Ticks = 0;
		GJ::Pump(*C.Rig.Gpu, [&]() { return Ticks >= 8; }, 60.0, [&]() { ++Ticks; });
		GJ::Read(C.Rig, After);
		T.AddInfo(FString::Printf(TEXT("%s: the restored sequence's last decode outcome is %d"), *What, (int32)After.Outcome));
		bool bOk = T.TestEqual(*(What + TEXT(": the restored sequence is Complete")), (uint8)After.Phase, (uint8)ESuperSLMSequencePhase::Complete);
		bOk &= T.TestTrue(*(What + TEXT(": the restored walk reads accepting (IsSchemaAccepting)")), After.bAccepting);
		bOk &= ExpectStopReason(T, After.Phase, After.Outcome, After.bAccepting, ESuperSLMQueryStopReason::Completed, What + TEXT(", restored"));
		bOk &= T.TestEqual(*(What + TEXT(": no token after the restore")), After.Tokens.Num(), 0);
		GJ::ReturnAndSettle(C.Rig, After.Sequence);
		return bOk;
	}

	// The clean stop on a GPU path: the ending, the CPU's run of the same request (tokens and ending), a save, and that
	// save's restore.
	bool CheckGpuCleanStop(FAutomationTestBase& T, FGpuCell& C, const FSuperSLMGenerationRequest& Req, const GJ::FGpuRun& Run, const FString& What)
	{
		bool bOk = ExpectCleanStop(T, Run.Phase, Run.Outcome, Run.bAccepting, Run.Tokens.Num(), Req.MaxNewTokens, What);
		CJ::FRunResult Cpu;
		if (GJ::RunCpu(T, C.Rig, Req, ESuperSLMSchemaJumpForward::Off, Cpu))
		{
			bOk &= GJ::SameAsCpu(T, Run, Cpu, What);
		}
		else
		{
			bOk = false;
		}
		// A cleanly completed GPU sequence saves like any other completed one (the save refusal is for Faulted only), and
		// restores as it was saved.
		TArray<uint8> Blob;
		if (!GJ::Save(T, C.Rig, Run.Sequence, Blob)) { return false; }
		bOk &= CheckGpuRestoreAfterCleanStop(T, C, Blob, What);
		return bOk;
	}

	// P7: an Exact close's -2 whose walk is not accepting, at Where (ArmExactCloseDeadEnd). Exact over a table seeded from
	// the CPU's Off run, as P4, so every draft agrees and the first close commits all its rows.
	bool CheckExactCloseDeadEnd(FAutomationTestBase& T, ESuperSLMGpuExactCloseDeadEnd Where, const FString& What)
	{
		if (!CJ::LiveGate(T)) { return false; }
		FGpuCell C;
		if (!C.Open(T, /*bMultiRow*/ true)) { return false; }
		FSuperSLMGenerationRequest Req;
		if (!GJ::MakeRequest(T, C.Rig, ESuperSLMSchemaJumpForward::Exact, kMaxNew, Req)) { return false; }
		CJ::FRunResult Off;
		if (!CJ::LoadWalkTableForSeeding(T, C.Rig.Cpu) || !GJ::RunCpu(T, C.Rig, Req, ESuperSLMSchemaJumpForward::Off, Off) ||
			!T.TestTrue(*(What + TEXT(": the draft table is seeded from the Off run")),
				FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*C.Rig.Cpu.Cpu, C.Rig.Cpu.Schema, { Off.Tokens }))) { return false; }
		GJ::FGpuRun Run;
		if (!GJ::Vend(T, C.Rig, GJ::EPath::WholeToken, Run)) { return false; }
		if (!T.TestTrue(*(What + TEXT(": the Exact-close dead end is armed")), FMr::ArmExactCloseDeadEnd(*C.Rig.Gpu, Where)))
		{
			GJ::ReturnAndSettle(C.Rig, Run.Sequence);
			return false;
		}
		FMr::StartRecording(*C.Rig.Gpu);
		if (!GJ::Drive(T, C.Rig, Req, Run)) { GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
		const TArray<FSuperSLMGpuRowsActionRecord> Acts = ActionsOf(*C.Rig.Gpu, Run.Sequence.Id);
		bool bOk = ExpectDeadEnd(T, Run.Phase, Run.Outcome, Run.bAccepting, What);
		bOk &= ExpectEndInExactClose(T, LastDeadEnd(Acts), Where == ESuperSLMGpuExactCloseDeadEnd::AfterDrafts, What);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: its %d tokens are a strict prefix of the CPU's Off run's %d (the dead end came mid-document)"), *What, Run.Tokens.Num(), Off.Tokens.Num()),
			Run.Tokens.Num() < Off.Tokens.Num() && CJ::IsPrefixOf(Run.Tokens, Off.Tokens));
		const FSuperSLMLifecycleOpHandle Save = C.Rig.Gpu->RequestSaveSequence(Run.Sequence);
		TArray<uint8> Blob;
		const ESuperSLMRestoreResult SaveResult = Save.IsValid()
			? SuperSLML2S2Fixtures::DriveSaveToResolution(*C.Rig.Gpu, Save, Blob, 120.0)
			: ESuperSLMRestoreResult::SaveRefused;
		bOk &= T.TestEqual(*(What + TEXT(": a faulted sequence's save is refused")), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::SaveRefused);
		GJ::ReturnAndSettle(C.Rig, Run.Sequence);
		return bOk;
	}
}

// P2 GPU one-row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP2GpuOneRowTest, "SuperSLM.SchemaCleanStop.P2.GpuOneRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP2GpuOneRowTest::RunTest(const FString& Parameters)
{
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ false)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	SuperSLMSchemaCleanStop::GJ::FGpuRun Run;
	TArray<FSuperSLMGpuRowsActionRecord> Acts;
	if (!SuperSLMSchemaCleanStop::RunKept(*this, C, Req, Run, Acts)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = TestEqual(TEXT("P2: one-row work only (no chunk)"), SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::PromptRows) + SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::ExactRows), 0);
	bOk &= SuperSLMSchemaCleanStop::CheckGpuCleanStop(*this, C, Req, Run, TEXT("P2 GPU one-row"));
	SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	return bOk;
}

// P3 GPU multi-row, Off: the prompt is read by rows, and decoding is one-row, so the end is a one-row finish's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP3GpuMultiRowTest, "SuperSLM.SchemaCleanStop.P3.GpuMultiRowPromptRowsOneRowEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP3GpuMultiRowTest::RunTest(const FString& Parameters)
{
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ true)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	SuperSLMSchemaCleanStop::GJ::FGpuRun Run;
	TArray<FSuperSLMGpuRowsActionRecord> Acts;
	if (!SuperSLMSchemaCleanStop::RunKept(*this, C, Req, Run, Acts)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = TestTrue(*FString::Printf(TEXT("P3: the prompt was read by rows (%d PromptRows actions)"), SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::PromptRows)),
		SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::PromptRows) > 0);
	const FSuperSLMGpuRowsActionRecord* End = SuperSLMSchemaCleanStop::LastDeadEnd(Acts);
	bOk &= TestTrue(TEXT("P3: the end (-2) is a one-row finish's (ApplyResult): with Exact off, decoding is one-row"),
		End != nullptr && End->Kind == ESuperSLMGpuRowsActionKind::OneRow);
	bOk &= SuperSLMSchemaCleanStop::CheckGpuCleanStop(*this, C, Req, Run, TEXT("P3 GPU multi-row, prompt rows"));
	SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	return bOk;
}

// P3 GPU multi-row, Exact with no table seeded by the cell: the end is met in an Exact close, after its drafts. The
// last value of potion_shop_order's object is a choice, so a finish produces it; the closing brace that follows it is a
// forced chain, so the chunk after the value drafts it, and the -2 is that draft row's finish (reasoned from the
// schema's shape, not measured here: the cell pins it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP3GpuExactCloseEndTest, "SuperSLM.SchemaCleanStop.P3.GpuMultiRowExactCloseEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP3GpuExactCloseEndTest::RunTest(const FString& Parameters)
{
	if (!SuperSLMSchemaCleanStop::CJ::LiveGate(*this)) { return false; }
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ true)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Exact, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	SuperSLMSchemaCleanStop::GJ::FGpuRun Run;
	TArray<FSuperSLMGpuRowsActionRecord> Acts;
	if (!SuperSLMSchemaCleanStop::RunKept(*this, C, Req, Run, Acts)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = TestTrue(*FString::Printf(TEXT("P3 Exact: Exact chunks ran (%d ExactRows actions)"), SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::ExactRows)),
		SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::ExactRows) > 0);
	bOk &= SuperSLMSchemaCleanStop::ExpectEndInExactClose(*this, SuperSLMSchemaCleanStop::LastDeadEnd(Acts), /*bAfterDrafts*/ true, TEXT("P3 Exact"));
	bOk &= SuperSLMSchemaCleanStop::CheckGpuCleanStop(*this, C, Req, Run, TEXT("P3 GPU multi-row, Exact"));
	SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	return bOk;
}

// P4 GPU jump (Exact): the end is met in an Exact close.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP4GpuJumpTest, "SuperSLM.SchemaCleanStop.P4.GpuJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP4GpuJumpTest::RunTest(const FString& Parameters)
{
	if (!SuperSLMSchemaCleanStop::CJ::LiveGate(*this)) { return false; }
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ true)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Exact, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	// Exact drafts from the learned table: seed it from the CPU's Off run of the same request.
	SuperSLMSchemaCleanStop::CJ::FRunResult Off;
	if (!SuperSLMSchemaCleanStop::CJ::LoadWalkTableForSeeding(*this, C.Rig.Cpu) || !SuperSLMSchemaCleanStop::GJ::RunCpu(*this, C.Rig, Req, ESuperSLMSchemaJumpForward::Off, Off) ||
		!TestTrue(TEXT("P4: the draft table is seeded from the Off run"),
			FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*C.Rig.Cpu.Cpu, C.Rig.Cpu.Schema, { Off.Tokens }))) { return false; }
	SuperSLMSchemaCleanStop::GJ::FGpuRun Run;
	TArray<FSuperSLMGpuRowsActionRecord> Acts;
	if (!SuperSLMSchemaCleanStop::RunKept(*this, C, Req, Run, Acts)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = TestTrue(*FString::Printf(TEXT("P4: Exact chunks ran (%d ExactRows actions)"), SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::ExactRows)),
		SuperSLMSchemaCleanStop::CountKind(Acts, ESuperSLMGpuRowsActionKind::ExactRows) > 0);
	// Pinned: the end is the Exact close's, after at least one delivered draft (ApplyJumpResult, JumpEnd DeadEnd), not a
	// close whose row 0 dead-ended (CloseRows -> ApplyResult) and not a fallen-back step's one-row finish. A row-0 -2
	// at an accepting walk cannot occur (an accepting walk has no forced chain, so no chunk opens there); the fallen-back
	// step would be the one-row site.
	bOk &= SuperSLMSchemaCleanStop::ExpectEndInExactClose(*this, SuperSLMSchemaCleanStop::LastDeadEnd(Acts), /*bAfterDrafts*/ true, TEXT("P4"));
	bOk &= SuperSLMSchemaCleanStop::CheckGpuCleanStop(*this, C, Req, Run, TEXT("P4 GPU jump"));
	SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	return bOk;
}

// P5 GPU: a -2 whose walk is not accepting stays a fault, and the faulted sequence cannot be saved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP5GpuTest, "SuperSLM.SchemaCleanStop.P5.GpuDeadEndStaysFaulted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP5GpuTest::RunTest(const FString& Parameters)
{
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ false)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	SuperSLMSchemaCleanStop::GJ::FGpuRun Run;
	if (!SuperSLMSchemaCleanStop::GJ::Vend(*this, C.Rig, SuperSLMSchemaCleanStop::GJ::EPath::WholeToken, Run)) { return false; }
	// Armed after Configure (no calibration: JumpConfig turns it off) and the vend, so the first finish the GPU runs is
	// this generation's first token, at the schema's start state, which is not accepting.
	if (!TestTrue(TEXT("P5 GPU: the degenerate-row seam is armed"), FSuperSLMSchemaJumpTestAccess::ArmGpuDegenerateFinishRow()))
	{
		SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
		return false;
	}
	if (!SuperSLMSchemaCleanStop::GJ::Drive(*this, C.Rig, Req, Run)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = SuperSLMSchemaCleanStop::ExpectDeadEnd(*this, Run.Phase, Run.Outcome, Run.bAccepting, TEXT("P5 GPU"));
	bOk &= TestEqual(TEXT("P5 GPU: the dead end is at the first finish (no token produced)"), Run.Tokens.Num(), 0);
	const FSuperSLMLifecycleOpHandle Save = C.Rig.Gpu->RequestSaveSequence(Run.Sequence);
	TArray<uint8> Blob;
	const ESuperSLMRestoreResult SaveResult = Save.IsValid()
		? SuperSLML2S2Fixtures::DriveSaveToResolution(*C.Rig.Gpu, Save, Blob, 120.0)
		: ESuperSLMRestoreResult::SaveRefused;
	bOk &= TestEqual(TEXT("P5 GPU: a faulted sequence's save is refused"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::SaveRefused);
	SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	return bOk;
}

// P6 GPU, the restore site: a save that carries the unapplied -2 restores, and the rebuilt -2 ends the sequence cleanly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP6GpuTest, "SuperSLM.SchemaCleanStop.P6.GpuRestoreCarriedEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP6GpuTest::RunTest(const FString& Parameters)
{
	SuperSLMSchemaCleanStop::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ false)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!SuperSLMSchemaCleanStop::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, SuperSLMSchemaCleanStop::kMaxNew, Req)) { return false; }
	SuperSLMSchemaCleanStop::CJ::FRunResult Off;
	if (!SuperSLMSchemaCleanStop::GJ::RunCpu(*this, C.Rig, Req, ESuperSLMSchemaJumpForward::Off, Off)) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the CPU's run ends Complete / SchemaComplete (%d tokens)"), Off.Tokens.Num()),
		Off.Phase == ESuperSLMSequencePhase::Complete && Off.Outcome == ESuperSLMDecodeOutcome::SchemaComplete && Off.Tokens.Num() >= 1)) { return false; }
	const int32 N = Off.Tokens.Num();
	bool bOk = true;
	int32 Carried = 0;
	for (const int32 Back : { 0, 1, 2, 4, 8 })
	{
		const int32 At = FMath::Max(1, N - Back);
		const FString Arm = FString::Printf(TEXT("P6 GPU, saved at %d of %d tokens"), At, N);
		SuperSLMSchemaCleanStop::GJ::FGpuRun Before;
		if (!SuperSLMSchemaCleanStop::GJ::Vend(*this, C.Rig, SuperSLMSchemaCleanStop::GJ::EPath::WholeToken, Before)) { return false; }
		if (!SuperSLMSchemaCleanStop::GJ::Drive(*this, C.Rig, Req, Before, [&]() { return C.Rig.Gpu->GetGeneratedTokens(Before.Sequence).Num() >= At; }))
		{
			SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
			return false;
		}
		if (SuperSLMSchemaCleanStop::GJ::IsEnded(*C.Rig.Gpu, Before.Sequence))
		{
			AddInfo(*(Arm + TEXT(": the run ended before the save; arm skipped")));
			SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
			continue;
		}
		TArray<uint8> Blob;
		if (!SuperSLMSchemaCleanStop::GJ::Save(*this, C.Rig, Before.Sequence, Blob)) { SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Before.Sequence); return false; }
		const TArray<int32> TokensBefore = C.Rig.Gpu->GetGeneratedTokens(Before.Sequence);
		SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
		if (!SuperSLMSchemaCleanStop::CarriesSchemaEnd(Blob))
		{
			AddInfo(*(Arm + TEXT(": the save carried no unapplied -2; arm skipped")));
			continue;
		}
		++Carried;
		SuperSLMSchemaCleanStop::GJ::FGpuRun After;
		if (!TestEqual(*(Arm + TEXT(": the restore succeeds")), (uint8)SuperSLMSchemaCleanStop::GJ::Restore(C.Rig, Blob, After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		const bool bEnded = SuperSLMSchemaCleanStop::GJ::Pump(*C.Rig.Gpu, [&]() { return SuperSLMSchemaCleanStop::GJ::IsEnded(*C.Rig.Gpu, After.Sequence); });
		SuperSLMSchemaCleanStop::GJ::Read(C.Rig, After);
		bOk &= TestTrue(*(Arm + TEXT(": the restored sequence ends")), bEnded);
		TArray<int32> Joined = TokensBefore;
		Joined.Append(After.Tokens);
		bOk &= SuperSLMSchemaCleanStop::ExpectCleanStop(*this, After.Phase, After.Outcome, After.bAccepting, Joined.Num(), Req.MaxNewTokens, Arm);
		bOk &= TestEqual(*(Arm + TEXT(": saved + continued tokens equal the CPU's")), Joined, Off.Tokens);
		SuperSLMSchemaCleanStop::GJ::ReturnAndSettle(C.Rig, After.Sequence);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: an arm's save carried the unapplied -2 (%d arms)"), Carried), Carried > 0);
	return bOk;
}

// P7 GPU, an Exact close's -2 at its row 0, the walk not accepting: no draft delivered, a fault.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP7NoDraftTest, "SuperSLM.SchemaCleanStop.P7.GpuExactCloseDeadEndNoDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP7NoDraftTest::RunTest(const FString& Parameters)
{
	return SuperSLMSchemaCleanStop::CheckExactCloseDeadEnd(*this, ESuperSLMGpuExactCloseDeadEnd::FirstRow, TEXT("P7 no draft"));
}

// P7 GPU, an Exact close's -2 after its committed drafts, the walk not accepting: the drafts delivered, then a fault.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP7AfterDraftsTest, "SuperSLM.SchemaCleanStop.P7.GpuExactCloseDeadEndAfterDrafts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP7AfterDraftsTest::RunTest(const FString& Parameters)
{
	return SuperSLMSchemaCleanStop::CheckExactCloseDeadEnd(*this, ESuperSLMGpuExactCloseDeadEnd::AfterDrafts, TEXT("P7 after drafts"));
}

// P8 GPU, the restore site with the walk not accepting: a save carrying the unapplied -2 of the degenerate-row seam
// (armed for the generation's first finish, at the schema's start state) restores, and the rebuilt -2 is a fault.
// Arms save at P + e ticks after the begin (P the prompt's length, e in 1, 2, 4, 8, 12, 16): an arm whose save carried
// no -2 is skipped, and at least one arm must carry it. Each saved run is driven on to its own end, so the seam is
// spent by it and never by a later cell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP8GpuTest, "SuperSLM.SchemaCleanStop.P8.GpuRestoreCarriedDeadEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP8GpuTest::RunTest(const FString& Parameters)
{
	namespace CS = SuperSLMSchemaCleanStop;
	CS::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ false)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!CS::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, CS::kMaxNew, Req)) { return false; }
	const int32 P = Req.PromptTokens.Num();
	bool bOk = true;
	int32 Carried = 0;
	for (const int32 Extra : { 1, 2, 4, 8, 12, 16 })
	{
		const FString Arm = FString::Printf(TEXT("P8 GPU, saved %d ticks after the begin"), P + Extra);
		CS::GJ::FGpuRun Before;
		if (!CS::GJ::Vend(*this, C.Rig, CS::GJ::EPath::WholeToken, Before)) { return false; }
		if (!TestTrue(*(Arm + TEXT(": the degenerate-row seam is armed")), FSuperSLMSchemaJumpTestAccess::ArmGpuDegenerateFinishRow()))
		{
			CS::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
			return false;
		}
		// Drive() records one snapshot per tick, so their count is the ticks since the begin was requested.
		if (!CS::GJ::Drive(*this, C.Rig, Req, Before, [&]() { return Before.Snapshots.Num() >= P + Extra; }))
		{
			CS::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
			return false;
		}
		// The save is admitted a tick or more after the request, so the -2 can apply first and fault the sequence; a
		// refusal for that reason skips the arm, as an end before the save does.
		TArray<uint8> Blob;
		bool bSaved = false;
		if (!CS::GJ::IsEnded(*C.Rig.Gpu, Before.Sequence))
		{
			const FSuperSLMLifecycleOpHandle Save = C.Rig.Gpu->RequestSaveSequence(Before.Sequence);
			const ESuperSLMRestoreResult SaveResult = Save.IsValid()
				? SuperSLML2S2Fixtures::DriveSaveToResolution(*C.Rig.Gpu, Save, Blob, 120.0)
				: ESuperSLMRestoreResult::SaveRefused;
			bSaved = SaveResult == ESuperSLMRestoreResult::Success && Blob.Num() > 0;
			if (!bSaved && !(SaveResult == ESuperSLMRestoreResult::SaveRefused && C.Rig.Gpu->GetPhase(Before.Sequence) == ESuperSLMSequencePhase::Faulted))
			{
				bOk &= TestEqual(*(Arm + TEXT(": the save succeeds, or is refused only because the -2 applied first")), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success);
			}
		}
		// The saved run goes on to its end, the -2 at the seam: the unsaved twin of what the restore must give.
		const bool bBeforeEnded = CS::GJ::Pump(*C.Rig.Gpu, [&]() { return CS::GJ::IsEnded(*C.Rig.Gpu, Before.Sequence); });
		CS::GJ::Read(C.Rig, Before);
		bOk &= TestTrue(*(Arm + TEXT(": the saved run ends")), bBeforeEnded);
		bOk &= CS::ExpectDeadEnd(*this, Before.Phase, Before.Outcome, Before.bAccepting, Arm + TEXT(", the saved run"));
		bOk &= TestEqual(*(Arm + TEXT(": the saved run's dead end is at its first finish (no token)")), Before.Tokens.Num(), 0);
		CS::GJ::ReturnAndSettle(C.Rig, Before.Sequence);
		if (!bSaved || !CS::CarriesSchemaEnd(Blob))
		{
			AddInfo(Arm + (bSaved ? TEXT(": the save carried no unapplied -2; arm skipped") : TEXT(": the run ended before the save was taken; arm skipped")));
			continue;
		}
		++Carried;
		CS::GJ::FGpuRun After;
		if (!TestEqual(*(Arm + TEXT(": the restore succeeds")), (uint8)CS::GJ::Restore(C.Rig, Blob, After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		const bool bEnded = CS::GJ::Pump(*C.Rig.Gpu, [&]() { return CS::GJ::IsEnded(*C.Rig.Gpu, After.Sequence); });
		CS::GJ::Read(C.Rig, After);
		bOk &= TestTrue(*(Arm + TEXT(": the restored sequence ends")), bEnded);
		bOk &= CS::ExpectDeadEnd(*this, After.Phase, After.Outcome, After.bAccepting, Arm + TEXT(", restored"));
		bOk &= TestEqual(*(Arm + TEXT(": no token, before the save or after the restore")), After.Tokens.Num(), 0);
		CS::GJ::ReturnAndSettle(C.Rig, After.Sequence);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: an arm's save carried the unapplied -2 (%d arms)"), Carried), Carried > 0);
	return bOk;
}

// P9 GPU: a sequence saved after its clean stop restores Complete, accepting, Completed (code review finding 1), and
// so does the CPU's run of the same request: the two backends agree on the restored stop reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSchemaCleanStopP9GpuTest, "SuperSLM.SchemaCleanStop.P9.GpuRestoreAfterCleanStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSchemaCleanStopP9GpuTest::RunTest(const FString& Parameters)
{
	namespace CS = SuperSLMSchemaCleanStop;
	CS::FGpuCell C;
	if (!C.Open(*this, /*bMultiRow*/ false)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!CS::GJ::MakeRequest(*this, C.Rig, ESuperSLMSchemaJumpForward::Off, CS::kMaxNew, Req)) { return false; }
	CS::GJ::FGpuRun Run;
	TArray<FSuperSLMGpuRowsActionRecord> Acts;
	if (!CS::RunKept(*this, C, Req, Run, Acts)) { CS::GJ::ReturnAndSettle(C.Rig, Run.Sequence); return false; }
	bool bOk = CS::ExpectCleanStop(*this, Run.Phase, Run.Outcome, Run.bAccepting, Run.Tokens.Num(), Req.MaxNewTokens, TEXT("P9 GPU"));
	TArray<uint8> Blob;
	const bool bSaved = CS::GJ::Save(*this, C.Rig, Run.Sequence, Blob);
	CS::GJ::ReturnAndSettle(C.Rig, Run.Sequence);
	if (!bSaved) { return false; }
	bOk &= CS::CheckGpuRestoreAfterCleanStop(*this, C, Blob, TEXT("P9 GPU"));
	// The CPU twin: the same request, saved after its clean stop and restored.
	CS::CJ::FRunResult Cpu;
	FSuperSLMGenerationRequest CpuReq = Req;
	CpuReq.SchemaJumpForward = ESuperSLMSchemaJumpForward::Off;
	if (!CS::CJ::RunSolo(*this, C.Rig.Cpu, CpuReq, Cpu, /*bUnbound*/ false, /*bKeep*/ true)) { C.Rig.Cpu.Cpu->ReturnSequence(Cpu.Sequence); return false; }
	bOk &= CS::ExpectCleanStop(*this, Cpu.Phase, Cpu.Outcome, Cpu.Stats.bSchemaAccepting, Cpu.Tokens.Num(), CpuReq.MaxNewTokens, TEXT("P9 GPU's CPU twin"));
	bOk &= CS::CheckCpuRestoreAfterCleanStop(*this, C.Rig.Cpu, Cpu.Sequence, TEXT("P9 GPU's CPU twin")); // returns Cpu.Sequence
	return bOk;
}
#endif // SUPERSLMUNREAL_WITH_GPU

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
