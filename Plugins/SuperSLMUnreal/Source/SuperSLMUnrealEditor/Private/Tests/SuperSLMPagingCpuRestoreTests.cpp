// Plugin paging plan, CPU red suite (step K1): budget save and restore (§3.5).
//
//   SuperSLM.Paging.Cpu.Restore.Normalization          cell 1.8 (a)-(c)                    [K3]
//   SuperSLM.Paging.Cpu.Restore.TeardownStates         cell 1.11 (the CPU's states)        [K3/K6/K7]
//   SuperSLM.Paging.Cpu.Restore.NoLostState            cell 1.12                           [K3]
//   SuperSLM.Paging.Cpu.Restore.HostileHeader          cell 2.1 (the CPU's fields)         [K3]
//   SuperSLM.Paging.Cpu.Restore.TruncatedHeader        cell 2.2                            [K3]
//   SuperSLM.Paging.Cpu.Restore.ChargeFromHeader       cell 2.3 (the charge leg)           [K3]
//   SuperSLM.Paging.Cpu.Restore.ModeCrossings          cells 2.3, 9.6; §5 KvModeMismatch    [K3]
//   SuperSLM.Paging.Cpu.Restore.Origins                cell 4.5 (the CPU's legs)           [K3]
//   SuperSLM.Paging.Cpu.Restore.MidTurnRoundTrip       cells 6.3 and 9.1                   [K3]
//   SuperSLM.Paging.Cpu.Restore.HintReshares           cell 9.2                            [K3]
//   SuperSLM.Paging.Cpu.Restore.HintMismatch           cell 9.3                            [K3]
//   SuperSLM.Paging.Cpu.Restore.Ssb5Wrapper            cell 9.5 (this release's wrapper)   [K3]
//   SuperSLM.Paging.Cpu.Restore.PrePinWrapperRefused   cell 9.5 (the 1.11.0-pin wrapper)   [K3]
//   SuperSLM.Paging.Cpu.Restore.DifferentBudget        cell 9.7                            [K3]
//
// The CPU vends a fresh sequence for every restore (RestoreSequence), so "the old sequence" of a
// refused or failed restore is the warm holder of the slot it vended: it must keep its engine
// holder (FSuperSLMPagedTestSeam::SlotHolderGeneration unchanged) and generate as if no restore was
// attempted. Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// Begins Request on Seq, saves it once it has produced AtTokens tokens while still running, and
	// returns it. OutBefore is the tokens it had produced at the save (the existing mid-turn pattern
	// of the save/restore suite).
	bool SaveMidTurn(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMGenerationRequest& Request, int32 AtTokens,
		TArray<uint8>& OutBlob, TArray<int32>& OutBefore)
	{
		FString Error;
		const bool bBegun = Cpu.BeginGeneration(Seq, Request, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration (%s)"), *Error), bBegun))
		{
			return false;
		}
		bool bSaved = false;
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - Start < GenerateSeconds)
		{
			Cpu.Tick(Step);
			if (Cpu.GetGeneratedTokens(Seq).Num() >= AtTokens)
			{
				bSaved = SaveAndTake(T, Cpu, Seq, OutBlob);
				break;
			}
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		OutBefore = Cpu.GetGeneratedTokens(Seq);
		ReturnAndDrain(Cpu, Seq);
		return T.TestTrue(*FString::Printf(TEXT("saved mid-turn at %d tokens"), AtTokens), bSaved);
	}

	// Restores Blob (with Hint when valid), lets the restored generation run to its end, and appends
	// what it produced to InOutTokens.
	bool ContinueRestored(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<uint8>& Blob, USuperSLMModel* Model, const FSuperSLMPrefix& Hint,
		TArray<int32>& InOutTokens, FGeneration* OutEnd = nullptr, FSuperSLMSequence* OutSeq = nullptr)
	{
		FSuperSLMSequence Seq;
		if (!ApplyRestore(T, Cpu, Blob, Model, Seq, Hint))
		{
			return false;
		}
		auto Ended = [&Cpu, &Seq]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		};
		Drain(Cpu, Ended, GenerateSeconds);
		const bool bEnded = T.TestTrue(TEXT("the restored generation ends"), Ended());
		InOutTokens.Append(Cpu.GetGeneratedTokens(Seq));
		if (OutEnd)
		{
			OutEnd->Phase = Cpu.GetPhase(Seq);
			OutEnd->Outcome = Cpu.GetLastDecodeOutcome(Seq);
			OutEnd->Tokens = Cpu.GetGeneratedTokens(Seq);
		}
		if (OutSeq)
		{
			*OutSeq = Seq;
		}
		else
		{
			ReturnAndDrain(Cpu, Seq);
		}
		return bEnded;
	}

	TArray<int64> SlotGenerations(const USuperSLMSubsystem& Cpu, int32 BlockCount)
	{
		TArray<int64> Out;
		for (int32 Slot = 0; Slot < BlockCount; ++Slot)
		{
			Out.Add(FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot));
		}
		return Out;
	}

	// Restore-kind rows the job ledger appended in [From, To). A refused restore's recycle may post a
	// Reset, so only Restore rows count. A row the ring has already overwritten reads as one (it
	// cannot be shown absent).
	int32 RestoreJobsPosted(const USuperSLMSubsystem& Cpu, int64 From, int64 To)
	{
		int32 Count = 0;
		for (int64 Row = From; Row < To; ++Row)
		{
			const FSuperSLMWorkerJobReport* Report = Cpu.FindJobLedgerRow(Row);
			if (!Report || Report->Kind == ESuperSLMWorkerJobKind::Restore)
			{
				++Count;
			}
		}
		return Count;
	}

	// A refusal the payload header alone decides (§3.5 "Where a header refusal is carried", plan rev
	// 1.9): made at the call, by name, with no handle minted and no sequence vended; nothing drawn
	// (the ledger's total unchanged) and every slot's holder kept. "Before any job" is also graded
	// directly, two ways: a HoldStart(Restore) armed before the issue is never reached, and the job
	// ledger appends no Restore row. A refusal carried on a valid handle fails here, whatever it
	// resolves.
	bool ExpectRefusedBeforeAnyJob(FAutomationTestBase& T, FPagingRig& Rig, const TArray<uint8>& Blob, ESuperSLMRestoreResult Expected, const TCHAR* What)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		const FSuperSLMPagedLedgerView Before = LedgerOf(Cpu);
		const TArray<int64> Generations = SlotGenerations(Cpu, Rig.Config.BlockCount);
		const int64 RowsBefore = Cpu.GetJobLedgerAppendedCount();
		const FSuperSLMPagedHold Start = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::Restore);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: HoldStart(Restore) is armed"), What), Start.IsValid());
		const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: refused by name at the call (%s)"), What, *R.Error), (uint8)R.Sync, (uint8)Expected);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no handle is minted"), What), R.Handle.IsValid());
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no sequence is vended"), What), R.Sequence.IsValid());
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no restore job reached its start"), What), FSuperSLMPagedTestSeam::IsHoldReached(Cpu, Start));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the job ledger appended no Restore row"), What),
			RestoreJobsPosted(Cpu, RowsBefore, Cpu.GetJobLedgerAppendedCount()), 0);
		// Disarms the hold when it was never reached; lets a held job go on when it was.
		FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Start);
		Discard(Cpu, R);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing drawn (the ledger's total is unchanged)"), What), LedgerOf(Cpu).TotalCharged, Before.TotalCharged);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: every slot keeps its holder"), What), SlotGenerations(Cpu, Rig.Config.BlockCount) == Generations);
		return bOk;
	}

	// One payload field rewritten at the plan's §3.5 offset.
	TArray<uint8> WithField(FAutomationTestBase& T, const TArray<uint8>& Blob, int32 Offset, int32 Bytes, int64 Value)
	{
		TArray<uint8> Out = Blob;
		const bool bEdited = FSuperSLMPagedTestSeam::EditPayload(Out, [Offset, Bytes, Value](TArray<uint8>& Payload)
		{
			if (Payload.Num() >= Offset + Bytes)
			{
				PayloadHeader::WriteLE(Payload, Offset, Bytes, Value);
			}
		});
		T.TestTrue(*FString::Printf(TEXT("the payload's field at %d is rewritten"), Offset), bEdited);
		return Out;
	}

	FSuperSLMGenerationRequest MakeRequest(const TArray<int32>& Prompt, int32 MaxNew)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNew;
		return Request;
	}
}

// Cell 1.8 (§3.5 "Normalization", create-first).
//   (a) An 'SSB5' holder restored into budget mode becomes a budget holder at its reset: its slot's
//       holder is replaced, the probe shows R(SequenceBudget) held for it (the exact form: only S is
//       drawn), and H returns to its value before the restore (form 1).
//   (b) With the create refused (H has no room: an earlier holder's hold of 55 plus a restore held in
//       flight by HoldApply leave 0 free, with no restore waiting; applied holds alone always leave
//       R(SequenceBudget) free, plan rev 1.8), the old holder is kept and reset in place, and generates the
//       same tokens as a fresh holder. Normalization succeeds at the next reset, once H has room.
//   (c) Normalization yields (the host suite's reading): with a restore waiting on H and room for the
//       create, the un-normalized holder (a restored b = 64 holder) is reset in place and takes no H,
//       and the waiter is admitted at the apply its predicate counted on. Holders at origins 960 and
//       576 (holds 15 and 9) and a restore at origin 1,000 held in flight leave H 16 free; a waiter
//       at origin 1,000 needs 24 and counts the in-flight one's guarantee of 9.
// Mutants: normalization release-first (the refused create loses the holder: (b)'s generation counter
// and tokens); normalization taking H while a restore waits ((c)'s waiter ends short and never
// resolves).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNormalizationTest, "SuperSLM.Paging.Cpu.Restore.Normalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuNormalizationTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Ssb5, B0, B960, B576, B1000, B64Budget;
	if (!Rig.Init(*this) || !MakeSsb5Blob(*this, Rig, Ssb5) || !MakeBlobAt(*this, Rig, 512, 0, 1, B0) || !RequireAExShape(*this, Rig)
		|| !MakeBlobAt(*this, Rig, 512, 960, 2, B960) || !MakeBlobAt(*this, Rig, 512, 576, 3, B576) || !MakeBlobAt(*this, Rig, 512, 1000, 4, B1000)
		|| !MakeBlobAt(*this, Rig, 64, 0, 5, B64Budget))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	TArray<int32> Prompt, Reference;
	bool bOk = true;

	// (a)
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(2, 512, 0, 1);
		FSuperSLMSequence Holder;
		if (!ConfigureBudget(*this, Rig, C) || !MakeTokens(*this, Cpu, 8, 9, Prompt))
		{
			return false;
		}
		{
			FSuperSLMSequence Ref;
			if (!Vend(*this, Cpu, Ref) || !GenerateComplete(*this, Cpu, Ref, Prompt, 8, Reference))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Ref);
		}
		const int64 HBefore = Charged(Cpu, ERegion::H);
		if (!ApplyRestore(*this, Cpu, Ssb5, Rig.Model, Holder))
		{
			return false;
		}
		const int32 Slot = FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Holder);
		const int64 GenBefore = FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot);
		bOk &= TestEqual(TEXT("(a) the 'SSB5' holder holds P_blk - R(SB) = 55 on H"), Charged(Cpu, ERegion::H) - HBefore, G.Block() - G.R(512));
		bOk &= ResetAndWait(*this, Cpu, Holder);
		bOk &= TestTrue(TEXT("(a) the reset replaced the holder (normalization)"), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot) > GenBefore);
		bOk &= TestEqual(TEXT("(a) H returns to its value before the restore"), Charged(Cpu, ERegion::H), HBefore);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("(a) after normalization"));
		auto Rebuild = [&]()
		{
			FSuperSLMSequence S;
			return ConfigureBudget(*this, Rig, C) && ApplyRestore(*this, Cpu, Ssb5, Rig.Model, S) && ResetAndWait(*this, Cpu, S);
		};
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), ExpectedRegions(G, C).S, Rebuild, TEXT("(a) after normalization: R(SB) per slot"));
	}

	// (b)
	{
		FSuperSLMSequence Holder;
		if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)) || !ApplyRestore(*this, Cpu, Ssb5, Rig.Model, Holder))
		{
			return false;
		}
		const int32 Slot = FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Holder);
		const int64 GenBefore = FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot);
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedRestore InFlight = IssueRestore(Cpu, B0, Rig.Model);
		bOk &= TestTrue(TEXT("(b) the restore at origin 0 is held in flight"), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
		bOk &= TestEqual(TEXT("(b) H has 0 free"), Free(Cpu, ERegion::H), int64(0));
		bOk &= TestEqual(TEXT("(b) with no restore waiting (zero free comes from the hold in flight alone)"), LedgerOf(Cpu).WaitingRestores, 0);
		bOk &= ResetAndWait(*this, Cpu, Holder);
		bOk &= TestEqual(TEXT("(b) the create was refused: the old holder is kept"), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot), GenBefore);
		TArray<int32> Tokens;
		bOk &= GenerateComplete(*this, Cpu, Holder, Prompt, 8, Tokens);
		bOk &= TestEqual(TEXT("(b) the kept holder, reset in place, generates as a fresh holder"), Tokens, Reference);
		bOk &= TestTrue(TEXT("(b) the hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= TestEqual(TEXT("(b) the held restore succeeds"), (uint8)WaitOp(Cpu, InFlight.Handle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= ResetAndWait(*this, Cpu, Holder);
		bOk &= TestTrue(TEXT("(b) normalization succeeds at the next reset"), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot) > GenBefore);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("(b) after the retried normalization"));
	}

	// (c)
	{
		FSuperSLMSequence H960, H576, Unnormalized;
		if (!ConfigureBudget(*this, Rig, MakeConfig(5, 512, 0, 1)) || !ApplyRestore(*this, Cpu, B960, Rig.Model, H960)
			|| !ApplyRestore(*this, Cpu, B576, Rig.Model, H576) || !ApplyRestore(*this, Cpu, B64Budget, Rig.Model, Unnormalized))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(c) holds 15 + 9 + 0"), Charged(Cpu, ERegion::H), int64(24));
		const int32 Slot = FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Unnormalized);
		const int64 GenBefore = FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot);
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedRestore InFlight = IssueRestore(Cpu, B1000, Rig.Model);
		bOk &= TestTrue(TEXT("(c) a restore charged 24 is held in flight"), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
		bOk &= TestEqual(TEXT("(c) H has 16 free"), Free(Cpu, ERegion::H), int64(16));
		const FIssuedRestore Waiter = IssueRestore(Cpu, B1000, Rig.Model);
		Drain(Cpu, []() { return false; }, 2.0);
		bOk &= TestEqual(TEXT("(c) the second restore waits (16 + 9 >= 24)"), (uint8)OutcomeOf(Cpu, Waiter), (uint8)ESuperSLMRestoreResult::Pending);
		bOk &= TestEqual(TEXT("(c) one waiter"), LedgerOf(Cpu).WaitingRestores, 1);
		bOk &= ResetAndWait(*this, Cpu, Unnormalized);
		bOk &= TestEqual(TEXT("(c) normalization yielded: the holder was reset in place"), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot), GenBefore);
		bOk &= TestEqual(TEXT("(c) and took no H"), Free(Cpu, ERegion::H), int64(16));
		bOk &= TestTrue(TEXT("(c) the hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= TestEqual(TEXT("(c) the in-flight restore succeeds"), (uint8)WaitOp(Cpu, InFlight.Handle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("(c) the waiter is admitted at that apply and succeeds"), (uint8)WaitOp(Cpu, Waiter.Handle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("(c) after the waiter's apply"));
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 1.8"));
	return bOk;
}

// Cell 1.11 (the CPU's states; §3.5 "Fit and wait"): teardown and re-Configure in each state:
//   - prefixes with dependents (a live prefix with an adopter and a nested child, and a ghost);
//   - a queued restore (held un-run by HoldStart);
//   - a restore waiting on H.
// Each is torn down by a re-Configure. Read at once after that Configure (§3.5 "When a lifecycle
// exit ends a wait"), the queued and the waiting restore's handles read NotConfigured exactly: each
// is a tombstone. A tombstone is counted by GetLifecycleHandleEntryCount(), ReleaseLifecycleOpHandle()
// erases it (true, the count one lower), and the handle then reads Pending. For the queued restore,
// as many fresh lifecycle operations as its id are run in the new epoch: none takes the tombstoned
// id, and the tombstone still reads NotConfigured after they resolve (the handle counter starts
// above the highest tombstoned id). Afterwards nothing leaks: the ledger is S alone and the probe's
// exact form holds on the rebuilt pool. The last state is left in place for the world's teardown
// (Deinitialize), which must complete.
// Mutants: the tombstone skipped (the handle reads Pending); the handle counter restarted at 1 (a
// fresh operation takes the tombstoned id and its result shows through); the pool destroyed before
// the queued jobs are dropped; a waiter left holding its slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuTeardownStatesTest, "SuperSLM.Paging.Cpu.Restore.TeardownStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuTeardownStatesTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FSuperSLMRuntimeConfig C = MakeConfig(4, 512, 3, 1);
	const FRegions Want = ExpectedRegions(Rig.Geo, C);
	auto Rebuild = [this, &Rig, &C]() { return ConfigureBudget(*this, Rig, C); };
	// AtFirstConfigure runs right after the re-Configure that ends the state, before the probe's own
	// rebuilds.
	auto AfterReconfigure = [&](const TCHAR* State, TFunctionRef<bool()> AtFirstConfigure)
	{
		bool bClean = Rebuild();
		bClean &= AtFirstConfigure();
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		bClean &= TestEqual(*FString::Printf(TEXT("%s: after re-Configure the ledger is S alone"), State), V.TotalCharged, Want.S);
		bClean &= TestEqual(*FString::Printf(TEXT("%s: no ghost survives"), State), V.GhostCount, 0);
		bClean &= TestEqual(*FString::Printf(TEXT("%s: no waiter survives"), State), V.WaitingRestores, 0);
		bClean &= ProbeDrawnExactly(*this, Cpu, Want.Pool(), Want.S, Rebuild, *FString::Printf(TEXT("%s: the rebuilt pool"), State));
		return bClean;
	};
	// The tombstone, read in the new epoch: NotConfigured exactly; with bFreshOps, unshadowed by as
	// many fresh operations as its id; counted; erased by its release, after which it reads Pending.
	auto ResolvedNotConfigured = [&](const FIssuedRestore& R, const TCHAR* State, bool bFreshOps)
	{
		bool bTomb = TestTrue(*FString::Printf(TEXT("%s: the restore was issued with a handle"), State), R.Handle.IsValid());
		bTomb &= TestEqual(*FString::Printf(TEXT("%s: after re-Configure the handle reads NotConfigured exactly (its tombstone)"), State),
			(uint8)Cpu.GetLifecycleOpResult(R.Handle), (uint8)ESuperSLMRestoreResult::NotConfigured);
		if (bFreshOps)
		{
			FSuperSLMSequence Seq;
			if (!Vend(*this, Cpu, Seq))
			{
				return false;
			}
			const int32 Fresh = int32(FMath::Clamp<int64>(R.Handle.Id, 1, 256));
			for (int32 I = 0; I < Fresh; ++I)
			{
				FSuperSLMLifecycleOpHandle Op;
				FString Error;
				const ESuperSLMRestoreResult Queued = Cpu.ResetSequence(Seq, Op, Error);
				bTomb &= TestEqual(*FString::Printf(TEXT("%s: fresh reset %d queues (%s)"), State, I, *Error), (uint8)Queued, (uint8)ESuperSLMRestoreResult::Success);
				bTomb &= TestTrue(*FString::Printf(TEXT("%s: fresh handle %lld is not the tombstoned id %lld"), State, Op.Id, R.Handle.Id), Op.Id != R.Handle.Id);
				bTomb &= TestEqual(*FString::Printf(TEXT("%s: fresh reset %d succeeds"), State, I), (uint8)WaitOp(Cpu, Op), (uint8)ESuperSLMRestoreResult::Success);
				Cpu.ReleaseLifecycleOpHandle(Op);
			}
			ReturnAndDrain(Cpu, Seq);
			bTomb &= TestEqual(*FString::Printf(TEXT("%s: after %d fresh operations the tombstone still reads NotConfigured"), State, Fresh),
				(uint8)Cpu.GetLifecycleOpResult(R.Handle), (uint8)ESuperSLMRestoreResult::NotConfigured);
		}
		const int32 Entries = Cpu.GetLifecycleHandleEntryCount();
		bTomb &= TestTrue(*FString::Printf(TEXT("%s: the entry count (%d) includes the tombstone"), State, Entries), Entries >= 1);
		bTomb &= TestTrue(*FString::Printf(TEXT("%s: ReleaseLifecycleOpHandle erases the tombstone"), State), Cpu.ReleaseLifecycleOpHandle(R.Handle));
		bTomb &= TestEqual(*FString::Printf(TEXT("%s: the entry count is one lower"), State), Cpu.GetLifecycleHandleEntryCount(), Entries - 1);
		bTomb &= TestEqual(*FString::Printf(TEXT("%s: once released the handle reads Pending"), State),
			(uint8)Cpu.GetLifecycleOpResult(R.Handle), (uint8)ESuperSLMRestoreResult::Pending);
		return bTomb;
	};
	TArray<int32> Tokens, Child, Suffix;
	if (!Rebuild() || !MakeTokens(*this, Cpu, 1000, 1, Tokens) || !MakeTokens(*this, Cpu, 100, 2, Child) || !MakeTokens(*this, Cpu, 4, 3, Suffix))
	{
		return false;
	}
	bool bOk = true;

	// Prefixes with dependents.
	{
		FSuperSLMPrefix Live, Nested, Ghost;
		FSuperSLMSequence Adopter, GhostAdopter;
		if (!BuildPrefix(*this, Cpu, Tokens, Live) || !BuildNested(*this, Cpu, Live, Child, Nested) || !Vend(*this, Cpu, Adopter) || !AdoptAndWait(*this, Cpu, Adopter, Live)
			|| !MakeGhost(*this, Cpu, Tokens, Ghost, GhostAdopter))
		{
			return false;
		}
		bOk &= AfterReconfigure(TEXT("prefixes with dependents"), []() { return true; });
	}

	// A queued restore.
	{
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedRestore R = IssueRestore(Cpu, B1000, Rig.Model);
		bOk &= TestTrue(TEXT("queued restore: held before its Layer-1 work"), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
		bOk &= AfterReconfigure(TEXT("a queued restore"), [&]() { return ResolvedNotConfigured(R, TEXT("a queued restore"), true); });
	}

	// A restore waiting on H: two held in flight (48 of 64), a third waits.
	{
		const FSuperSLMPagedHold H1 = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		const FSuperSLMPagedHold H2 = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		IssueRestore(Cpu, B1000, Rig.Model);
		IssueRestore(Cpu, B1000, Rig.Model);
		bOk &= TestTrue(TEXT("waiting restore: two held in flight"), WaitHoldReached(Cpu, H1) && WaitHoldReached(Cpu, H2));
		const FIssuedRestore W = IssueRestore(Cpu, B1000, Rig.Model);
		Drain(Cpu, []() { return false; }, 2.0);
		bOk &= TestEqual(TEXT("waiting restore: one waiter"), LedgerOf(Cpu).WaitingRestores, 1);
		const int64 HBefore = Charged(Cpu, ERegion::H);
		bOk &= TestEqual(TEXT("waiting restore: the waiter has drawn nothing"), HBefore, int64(48));
		bOk &= AfterReconfigure(TEXT("a waiting restore"), [&]() { return ResolvedNotConfigured(W, TEXT("a waiting restore"), false); });
	}

	// Left in place for Deinitialize: a waiter behind two held restores.
	{
		FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		IssueRestore(Cpu, B1000, Rig.Model);
		IssueRestore(Cpu, B1000, Rig.Model);
		IssueRestore(Cpu, B1000, Rig.Model);
		Drain(Cpu, []() { return false; }, 2.0);
	}
	return bOk;
}

// Cell 1.12 (§3.5 "Restore-first"): no lost state. A restore is refused by the engine after
// admission, twice: the payload's body cut one byte short past its header (EditPayload), and the draw forced
// to exhaust (SetNextLayer1StatusOverride, SSLM_KV_POOL_EXHAUSTED). After each, the slot the restore
// vended keeps its warm holder (its holder generation is unchanged), and that slot's next user
// generates exactly the tokens it generated before any restore was attempted. BlockCount 1, so
// every vend is that slot. The forced exhaustion reaches an admitted request, so it is a ledger
// fault by §3.7's definition and is logged as one (a Warning containing "ledger fault"): that leg
// expects the line at least once, so the log does not fail the test.
// Mutant: CPU release-first, which replaces (or loses) the warm holder on a refused restore.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNoLostStateTest, "SuperSLM.Paging.Cpu.Restore.NoLostState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuNoLostStateTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigureBudget(*this, Rig, MakeConfig(1, 512, 0, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Prompt, Reference;
	{
		FSuperSLMSequence Ref;
		if (!MakeTokens(*this, Cpu, 8, 2, Prompt) || !Vend(*this, Cpu, Ref) || !GenerateComplete(*this, Cpu, Ref, Prompt, 16, Reference))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Ref);
	}
	TArray<uint8> Corrupt = B1000;
	// "Corrupted past its header" is read as a body the engine must refuse: one byte short of the
	// saved size (§3.5 "Exact size"). A body whose bytes are only altered may be accepted by the
	// engine, which would make the cell's refusal depend on content the plan does not name.
	const bool bEdited = FSuperSLMPagedTestSeam::EditPayload(Corrupt, [](TArray<uint8>& Payload)
	{
		Payload.SetNum(Payload.Num() - 1);
	});
	if (!TestTrue(TEXT("the payload's body is corrupted"), bEdited))
	{
		return false;
	}
	bool bOk = true;
	struct FLeg { const TCHAR* Name; const TArray<uint8>* Blob; bool bForceExhaust; };
	for (const FLeg& Leg : { FLeg{ TEXT("body corrupted past its header"), &Corrupt, false }, FLeg{ TEXT("draw forced to exhaust"), &B1000, true } })
	{
		const int64 GenBefore = FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, 0);
		if (Leg.bForceExhaust)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the status seam is set"), Leg.Name),
				FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(Cpu, ESuperSLMPagedRequestKind::Restore, SSLM_KV_POOL_EXHAUSTED));
			AddExpectedError(TEXT("ledger fault"), EAutomationExpectedErrorFlags::Contains, 0);  // 0: at least once
		}
		const FIssuedRestore R = IssueRestore(Cpu, *Leg.Blob, Rig.Model);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: admitted (the header is sound) (%s)"), Leg.Name, *R.Error), (uint8)R.Sync, (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the restore vended the one slot"), Leg.Name), FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, R.Sequence), 0);
		const ESuperSLMRestoreResult Outcome = SettleOutcome(Cpu, R);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: refused after admission (read %d)"), Leg.Name, (int32)Outcome), Leg.bForceExhaust
			? Outcome == ESuperSLMRestoreResult::KvPagesExhausted
			: Outcome == ESuperSLMRestoreResult::Malformed);
		Discard(Cpu, R);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the slot keeps its warm holder"), Leg.Name), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, 0), GenBefore);
		FSuperSLMSequence Next;
		TArray<int32> Tokens;
		bOk &= Vend(*this, Cpu, Next) && GenerateComplete(*this, Cpu, Next, Prompt, 16, Tokens);
		ReturnAndDrain(Cpu, Next);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the slot generates as if no restore was attempted"), Leg.Name), Tokens, Reference);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: H is refunded"), Leg.Name), Charged(Cpu, ERegion::H), int64(0));
	}
	return bOk;
}

// Cell 2.1 (the CPU's fields; §3.5 "Malformed input"): the payload header corrupted in each field,
// at the plan's offsets, resolves Malformed before any job, with nothing drawn and every slot's
// holder kept: an unknown magic; budget > cap; origin > cap; context_length > cap; the wrapper's
// ContextUsed differing from the payload's context_length; and a budget of 0 (the domain is
// [1, cap]). (The GPU's kv_mode, ready-word and [two-cap] legs are the GPU suite's.)
// Mutants: each field's check absent; the budget checked on [0, cap] (a zero charge).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuHostileHeaderTest, "SuperSLM.Paging.Cpu.Restore.HostileHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuHostileHeaderTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, Blob) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	FSsb6Header Header;
	if (!ReadSsb6Header(*this, Blob, Header))
	{
		return false;
	}
	const int64 Cap = Rig.Geo.Cap;
	bool bOk = true;
	bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, WithField(*this, Blob, PayloadHeader::Magic, 4, int64('S') | (int64('S') << 8) | (int64('B') << 16) | (int64('9') << 24)),
		ESuperSLMRestoreResult::Malformed, TEXT("an unknown magic"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, WithField(*this, Blob, PayloadHeader::Ssb6Budget, 4, Cap + 1), ESuperSLMRestoreResult::Malformed, TEXT("budget > cap"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, WithField(*this, Blob, PayloadHeader::Ssb6Budget, 4, 0), ESuperSLMRestoreResult::Malformed, TEXT("budget 0"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, WithField(*this, Blob, PayloadHeader::Ssb6Origin, 8, Cap + 1), ESuperSLMRestoreResult::Malformed, TEXT("origin > cap"));
	{
		// context_length > cap, with the wrapper's ContextUsed moved with it so only the range check
		// can refuse it.
		TArray<uint8> Bad = WithField(*this, Blob, PayloadHeader::SsbContextLength, 8, Cap + 1);
		bOk &= TestTrue(TEXT("the wrapper's ContextUsed follows"), FSuperSLMPagedTestSeam::SetWrapperContextUsed(Bad, Cap + 1));
		bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, Bad, ESuperSLMRestoreResult::Malformed, TEXT("context_length > cap"));
	}
	{
		TArray<uint8> Bad = Blob;
		bOk &= TestTrue(TEXT("the wrapper's ContextUsed is rewritten"), FSuperSLMPagedTestSeam::SetWrapperContextUsed(Bad, Header.ContextLength + 1));
		bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, Bad, ESuperSLMRestoreResult::Malformed, TEXT("the wrapper's ContextUsed differs from context_length"));
	}
	// Control: the unedited blob is admitted and succeeds.
	FSuperSLMSequence Seq;
	bOk &= ApplyRestore(*this, *Rig.Cpu, Blob, Rig.Model, Seq);
	return bOk;
}

// Cell 2.2: a payload truncated inside its header (at 100 bytes, inside the common header, and at 162,
// inside the budget field) resolves Malformed before any job, with nothing drawn.
// Mutant: the header parser reading past the payload's end (any outcome but a named refusal).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuTruncatedHeaderTest, "SuperSLM.Paging.Cpu.Restore.TruncatedHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuTruncatedHeaderTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, Blob) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	bool bOk = true;
	for (const int32 Keep : { 100, PayloadHeader::Ssb6Budget + 2 })
	{
		TArray<uint8> Bad = Blob;
		bOk &= TestTrue(*FString::Printf(TEXT("the payload is cut to %d bytes"), Keep), FSuperSLMPagedTestSeam::EditPayload(Bad, [Keep](TArray<uint8>& Payload) { Payload.SetNum(Keep); }));
		bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, Bad, ESuperSLMRestoreResult::Malformed, *FString::Printf(TEXT("truncated at %d"), Keep));
	}
	return bOk;
}

// Cell 2.3 (the charge leg): a budget restore is charged E6's formula computed from the payload
// header (R(budget) + E(budget, origin), the fields read by the test at §3.5's offsets), and the probe
// mid-job (HoldApply) shows the draw within it (form 1). Two blobs: budget 512 at origin 1,000, and
// budget 64 at origin 0.
// Mutant: a charge computed from the wrapper or from SequenceBudget in place of the header's budget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuChargeFromHeaderTest, "SuperSLM.Paging.Cpu.Restore.ChargeFromHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuChargeFromHeaderTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000, B64;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !MakeBlobAt(*this, Rig, 64, 0, 2, B64) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	for (const TArray<uint8>* Blob : { &B1000, &B64 })
	{
		FSsb6Header Header;
		if (!ReadSsb6Header(*this, *Blob, Header))
		{
			return false;
		}
		const int64 Charge = Rig.Geo.RestoreCharge(Header.Budget, Header.Origin);
		const int64 HBefore = Charged(Cpu, ERegion::H);
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedRestore R = IssueRestore(Cpu, *Blob, Rig.Model);
		bOk &= TestTrue(TEXT("the restore is held at its apply"), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
		bOk &= TestEqual(*FString::Printf(TEXT("budget %lld, origin %lld: charged R(b) + E = %lld"), Header.Budget, Header.Origin, Charge), Charged(Cpu, ERegion::H) - HBefore, Charge);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("budget %lld, origin %lld, held at its apply"), Header.Budget, Header.Origin));
		FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
		bOk &= TestEqual(TEXT("the restore succeeds"), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Success);
		Discard(Cpu, R);
	}
	return bOk;
}

// Cells 2.3, 9.6 and §5's KvModeMismatch (the CPU's crossings, §3.5 "Mode crossings"; the magic
// decides, never a wrapper field):
//   - 'SSB6' (in a v2 wrapper whose own fields are consistent) into a legacy subsystem ->
//     KvModeMismatch at the call, with no handle minted and nothing restored (plan rev 1.9);
//   - 'SSB5' into a budget subsystem -> accepted as a whole-reserve holder;
//   - an adopt onto that un-normalized holder -> KvModeMismatch ("reset first"), the holder untouched;
//     after its reset the adopt succeeds.
// Mutants: 'SSB6' accepted in legacy mode (a budget holder in a legacy pool); an adopt onto a
// whole-reserve holder admitted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuModeCrossingsTest, "SuperSLM.Paging.Cpu.Restore.ModeCrossings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuModeCrossingsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Ssb6, Ssb5;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 200, 1, Ssb6) || !MakeSsb5Blob(*this, Rig, Ssb5))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	// The legacy subsystem (MakeSsb5Blob left it configured legacy).
	{
		// Decided by the magic alone, so refused at the call with no handle (plan rev 1.9, ruling 1).
		const int32 Live = Cpu.GetPoolOccupiedCount();
		bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, Ssb6, ESuperSLMRestoreResult::KvModeMismatch, TEXT("'SSB6' into legacy"));
		bOk &= TestEqual(TEXT("'SSB6' into legacy: no sequence left occupied"), Cpu.GetPoolOccupiedCount(), Live);
	}
	if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 1, 1)))
	{
		return false;
	}
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMSequence Holder;
	if (!MakeTokens(*this, Cpu, 100, 2, Tokens) || !BuildPrefix(*this, Cpu, Tokens, Prefix))
	{
		return false;
	}
	bOk &= ApplyRestore(*this, Cpu, Ssb5, Rig.Model, Holder);
	const int32 Slot = FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Holder);
	const int64 Gen = FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot);
	bOk &= AdoptAndWait(*this, Cpu, Holder, Prefix, ESuperSLMRestoreResult::KvModeMismatch);
	bOk &= TestEqual(TEXT("the un-normalized holder is untouched by the refused adopt"), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot), Gen);
	bOk &= ResetAndWait(*this, Cpu, Holder);
	bOk &= AdoptAndWait(*this, Cpu, Holder, Prefix);
	return bOk;
}

// Cell 4.5 (the CPU's legs, B = 64): a restore unhinted and hinted (with a rebuilt prefix of the
// saved prefix's tokens) at origins {0, B-1, B, 1000, cap-1}, budget 512. Origin 64 is page-aligned,
// with an empty tail page. Each is admitted at the default headroom (one block) and succeeds; at
// origin 4,095 the charge is 9 + 55 = 64, the whole block.
// Mutant: an H block smaller than the largest admissible charge (origin 4,095 is refused every time).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuOriginsTest, "SuperSLM.Paging.Cpu.Restore.Origins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuOriginsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int32 Origins[] = { 0, 63, 64, 1000, 4095 };
	bool bOk = true;
	for (const int32 Origin : Origins)
	{
		TArray<uint8> Blob;
		TArray<int32> PrefixTokens;
		if (!MakeBlobAt(*this, Rig, 512, Origin, Origin % 7, Blob, &PrefixTokens) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 1, 1)))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("origin %d: the charge fits one empty H block"), Origin), Rig.Geo.RestoreCharge(512, Origin) <= Rig.Geo.Block());
		FSuperSLMSequence Unhinted;
		bOk &= ApplyRestore(*this, Cpu, Blob, Rig.Model, Unhinted);
		ReturnAndDrain(Cpu, Unhinted);
		if (Origin > 0)
		{
			FSuperSLMPrefix Hint;
			FSuperSLMSequence Hinted;
			bOk &= BuildPrefix(*this, Cpu, PrefixTokens, Hint) && ApplyRestore(*this, Cpu, Blob, Rig.Model, Hinted, Hint);
			ReturnAndDrain(Cpu, Hinted);
			Cpu.ReleasePrefix(Hint);
			DrainQuiet(Cpu);
		}
		bOk &= NoLedgerFault(*this, Cpu, *FString::Printf(TEXT("origin %d"), Origin));
	}
	return bOk;
}

// Cells 6.3 and 9.1: an 'SSB6' round trip mid-turn. An adopter of a 300-token prefix is saved after
// 20 of 48 tokens; restored unhinted, and again hinted, the continuation equals the unsaved twin.
// Then E7's count to the limit across the round trip: a plain holder at SequenceBudget 64 with an
// 8-token prompt emits 1 + (64 - 8) = 57 tokens in all, saved after 20, and stops KvBudgetExceeded.
// Mutant: the restored position taken from the wrapper (an off-by-one continuation). The E7 leg
// does not kill "the restored limit recomputed from the current shape": here SequenceBudget is the
// saved b (64), so both give the same count. Cell 9.7's b = 1,024 leg kills it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuMidTurnTest, "SuperSLM.Paging.Cpu.Restore.MidTurnRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuMidTurnTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> PrefixTokens, Suffix, Reference;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, Cpu, 300, 1, PrefixTokens) || !MakeTokens(*this, Cpu, 8, 2, Suffix) || !BuildPrefix(*this, Cpu, PrefixTokens, Prefix))
	{
		return false;
	}
	{
		FSuperSLMSequence Seq;
		if (!Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, Prefix) || !GenerateComplete(*this, Cpu, Seq, Suffix, 48, Reference))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Seq);
	}
	bool bOk = true;
	for (const bool bHinted : { false, true })
	{
		FSuperSLMSequence Live;
		TArray<uint8> Blob;
		TArray<int32> Tokens;
		if (!Vend(*this, Cpu, Live) || !AdoptAndWait(*this, Cpu, Live, Prefix) || !SaveMidTurn(*this, Cpu, Live, MakeRequest(Suffix, 48), 20, Blob, Tokens))
		{
			return false;
		}
		bOk &= ContinueRestored(*this, Cpu, Blob, Rig.Model, bHinted ? Prefix : FSuperSLMPrefix(), Tokens);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the round trip equals the unsaved twin"), bHinted ? TEXT("hinted") : TEXT("unhinted")), Tokens, Reference);
	}
	Cpu.ReleasePrefix(Prefix);
	DrainQuiet(Cpu);

	// E7's count to the limit across the round trip.
	if (!ConfigureBudget(*this, Rig, MakeConfig(2, 64, 0, 1)))
	{
		return false;
	}
	TArray<int32> Prompt, Tokens;
	TArray<uint8> Blob;
	FSuperSLMSequence Live;
	FGeneration End;
	if (!MakeTokens(*this, Cpu, 8, 3, Prompt) || !Vend(*this, Cpu, Live) || !SaveMidTurn(*this, Cpu, Live, MakeRequest(Prompt, 1000), 20, Blob, Tokens)
		|| !ContinueRestored(*this, Cpu, Blob, Rig.Model, FSuperSLMPrefix(), Tokens, &End))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("limit 64, prompt 8: 1 + (64 - 8) = 57 tokens across the round trip"), Tokens.Num(), 57);
	bOk &= TestEqual(TEXT("it stops Faulted"), (uint8)End.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("with KvBudgetExceeded"), (uint8)End.Outcome, (uint8)ESuperSLMDecodeOutcome::KvBudgetExceeded);
	return bOk;
}

// Cells 9.2 and 9.3: what a hint does after the apply, read by the probe (one-state form) on the
// restored sequence's quiescent state, with tokens equal to the unsaved twin.
//   9.2: the hint is the saved prefix (rebuilt from the same tokens): the restore re-shares and draws
//        R(b) = 9, not R(b) + E = 24.
//   9.3: the hint is another prefix of the same length (a different salt): the restore is private and
//        draws R(b) + E = 24.
namespace
{
	bool HintedDraw(FAutomationTestBase& T, FPagingRig& Rig, bool bMatching, int64 ExpectedRestoreDraw)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		if (!ConfigureBudget(T, Rig, MakeConfig(2, 512, 2, 1)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		TArray<int32> PrefixTokens, OtherTokens, Suffix, Reference;
		FSuperSLMPrefix Prefix, Other;
		if (!MakeTokens(T, Cpu, 1000, 1, PrefixTokens) || !MakeTokens(T, Cpu, 1000, 2, OtherTokens) || !MakeTokens(T, Cpu, 8, 3, Suffix)
			|| !BuildPrefix(T, Cpu, PrefixTokens, Prefix) || !BuildPrefix(T, Cpu, OtherTokens, Other))
		{
			return false;
		}
		{
			FSuperSLMSequence Seq;
			if (!Vend(T, Cpu, Seq) || !AdoptAndWait(T, Cpu, Seq, Prefix) || !GenerateComplete(T, Cpu, Seq, Suffix, 48, Reference))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Seq);
		}
		FSuperSLMSequence Live, Restored;
		TArray<uint8> Blob;
		TArray<int32> Tokens;
		if (!Vend(T, Cpu, Live) || !AdoptAndWait(T, Cpu, Live, Prefix) || !SaveMidTurn(T, Cpu, Live, MakeRequest(Suffix, 48), 20, Blob, Tokens))
		{
			return false;
		}
		// Quiescent before the restore: S, both prefixes.
		const int64 Pool = LedgerOf(Cpu).PoolPages;
		int64 FreeBefore = -1, FreeAfter = -1;
		if (!ProbeFree(T, Cpu, Pool, FreeBefore) || !ContinueRestored(T, Cpu, Blob, Rig.Model, bMatching ? Prefix : Other, Tokens, nullptr, &Restored) || !ProbeFree(T, Cpu, Pool, FreeAfter))
		{
			return false;
		}
		// The restored holder replaced a warm one (R(SB) = 9 drawn before), so its own draw is the
		// change plus 9.
		const int64 RestoreDraw = (FreeBefore - FreeAfter) + Rig.Geo.R(512);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s hint: the restored holder draws %lld"), bMatching ? TEXT("matching") : TEXT("mismatched"), ExpectedRestoreDraw),
			RestoreDraw, ExpectedRestoreDraw);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s hint: the tokens equal the unsaved twin"), bMatching ? TEXT("matching") : TEXT("mismatched")), Tokens, Reference);
		bOk &= Form1ChargesCoverDrawn(T, Cpu, bMatching ? TEXT("after the sharing restore") : TEXT("after the private restore"));
		ReturnAndDrain(Cpu, Restored);
		return bOk;
	}
}

// Cell 9.2. Mutant: the hint not passed to the engine
// (the restore never shares; it draws 24).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuHintResharesTest, "SuperSLM.Paging.Cpu.Restore.HintReshares",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuHintResharesTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	return Rig.Init(*this) && HintedDraw(*this, Rig, true, 9);
}

// Cell 9.3. Mutant: a share taken without the engine's
// identity check (the tokens differ from the twin).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuHintMismatchTest, "SuperSLM.Paging.Cpu.Restore.HintMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuHintMismatchTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	return Rig.Init(*this) && HintedDraw(*this, Rig, false, 24);
}

// Cell 9.5 (this release's wrapper): a v2 wrapper around an 'SSB5' payload, written by this release
// in legacy mode mid-turn, restores unchanged in legacy mode (the continuation equals the unsaved
// twin), and in budget mode with the 'SSB5' charge (P_blk on H, read under HoldApply) and its
// normalization at the next reset (H returns to 0).
// Mutants: 'SSB5' charged R(b) in budget mode; no normalization (H stays held).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuSsb5WrapperTest, "SuperSLM.Paging.Cpu.Restore.Ssb5Wrapper",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuSsb5WrapperTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(2, 0, 0, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Prompt, Reference, Before;
	TArray<uint8> Blob;
	FSuperSLMSequence Ref, Live;
	if (!MakeTokens(*this, Cpu, 8, 1, Prompt) || !Vend(*this, Cpu, Ref) || !GenerateComplete(*this, Cpu, Ref, Prompt, 48, Reference))
	{
		return false;
	}
	ReturnAndDrain(Cpu, Ref);
	if (!Vend(*this, Cpu, Live) || !SaveMidTurn(*this, Cpu, Live, MakeRequest(Prompt, 48), 20, Blob, Before))
	{
		return false;
	}
	TArray<uint8> Payload;
	bool bOk = ReadPayload(*this, Blob, Payload) && TestTrue(TEXT("legacy mode writes 'SSB5'"), PayloadHeader::MagicIs(Payload, "SSB5"));
	{
		TArray<int32> Tokens = Before;
		bOk &= ContinueRestored(*this, Cpu, Blob, Rig.Model, FSuperSLMPrefix(), Tokens);
		bOk &= TestEqual(TEXT("legacy: the round trip equals the unsaved twin"), Tokens, Reference);
	}
	if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
	const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model);
	bOk &= TestTrue(TEXT("budget: the 'SSB5' restore is held at its apply"), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
	bOk &= TestEqual(TEXT("budget: charged P_blk on H"), Charged(Cpu, ERegion::H), Rig.Geo.Block());
	FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
	bOk &= TestEqual(TEXT("budget: the restore succeeds"), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Success);
	Drain(Cpu, [&Cpu, &R]() { const ESuperSLMSequencePhase P = Cpu.GetPhase(R.Sequence); return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted; }, GenerateSeconds);
	TArray<int32> Tokens = Before;
	Tokens.Append(Cpu.GetGeneratedTokens(R.Sequence));
	bOk &= TestEqual(TEXT("budget: the 'SSB5' round trip equals the unsaved twin"), Tokens, Reference);
	bOk &= ResetAndWait(*this, Cpu, R.Sequence);
	bOk &= TestEqual(TEXT("budget: normalized at the reset, H is empty"), Charged(Cpu, ERegion::H), int64(0));
	return bOk;
}

// Cell 9.5 (the pre-pin wrapper): a wrapper written by a build on the 1.11.0 pin is refused
// Layer1Mismatch in legacy and in budget mode, with nothing drawn. The blob is K0R's box fixture
// (the 'SSB5' wrapper captured from the pre-re-vendor build), named by SUPERSLM_PAGING_PREPIN_SSB5.
// Mutant: a version check skipped in budget mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuPrePinWrapperTest, "SuperSLM.Paging.Cpu.Restore.PrePinWrapperRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuPrePinWrapperTest::RunTest(const FString& Parameters)
{
	const FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PAGING_PREPIN_SSB5"));
	TArray<uint8> Blob;
	if (!TestTrue(*FString::Printf(TEXT("K0R's pre-pin 'SSB5' wrapper fixture is readable (SUPERSLM_PAGING_PREPIN_SSB5 = '%s')"), *Path),
			!Path.IsEmpty() && FFileHelper::LoadFileToArray(Blob, *Path)))
	{
		return false;
	}
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(2, 0, 0, 1)))
	{
		return false;
	}
	FSuperSLMSequence Seq;
	bool bOk = TestEqual(TEXT("legacy: Layer1Mismatch"), (uint8)RestoreAndWait(*this, *Rig.Cpu, Blob, Rig.Model, Seq), (uint8)ESuperSLMRestoreResult::Layer1Mismatch);
	if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	bOk &= ExpectRefusedBeforeAnyJob(*this, Rig, Blob, ESuperSLMRestoreResult::Layer1Mismatch, TEXT("budget"));
	return bOk;
}

// Cell 9.7 (§3.3 "Generation limit"): a restored holder whose budget differs from SequenceBudget
// (512): b = 256 and b = 1,024, each a plain holder saved mid-turn and restored, then generated to
// its stop. The emitted count after the restore is E7's for the saved limit, min(origin + b, cap) =
// b: 1 + (b - c) with c the payload's context_length (ready at the save), with no early stop and no
// spurious KvBudgetExceeded before it; the stop is KvBudgetExceeded.
// Mutant: the restored limit taken from SequenceBudget (or recomputed from the current shape):
// b = 1,024 stops early at 512, which kills it. The b = 256 leg does not discriminate: the mutant
// runs on past 256, but the engine enforces the saved limit by itself, so the count and the stop
// read the same. It stays as a correctness leg for a budget below SequenceBudget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuDifferentBudgetTest, "SuperSLM.Paging.Cpu.Restore.DifferentBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuDifferentBudgetTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	for (const int32 SavedBudget : { 256, 1024 })
	{
		TArray<int32> Prompt, Before;
		TArray<uint8> Blob;
		FSuperSLMSequence Live;
		if (!ConfigureBudget(*this, Rig, MakeConfig(1, SavedBudget, 0, 1)) || !MakeTokens(*this, Cpu, 8, 1, Prompt) || !Vend(*this, Cpu, Live)
			|| !SaveMidTurn(*this, Cpu, Live, MakeRequest(Prompt, 4096), 20, Blob, Before))
		{
			return false;
		}
		FSsb6Header Header;
		TArray<uint8> Payload;
		if (!ReadSsb6Header(*this, Blob, Header) || !ReadPayload(*this, Blob, Payload))
		{
			return false;
		}
		const int64 Ready = PayloadHeader::ReadLE(Payload, 120, 4) != 0 ? 1 : 0;
		const int64 Limit = FMath::Min<int64>(Header.Origin + Header.Budget, Rig.Geo.Cap);
		const int64 Expected = Ready + (Limit - Header.ContextLength);
		if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 2)))
		{
			return false;
		}
		TArray<int32> After;
		FGeneration End;
		bOk &= ContinueRestored(*this, Cpu, Blob, Rig.Model, FSuperSLMPrefix(), After, &End);
		bOk &= TestEqual(*FString::Printf(TEXT("b = %d: the saved budget is in the header"), SavedBudget), Header.Budget, int64(SavedBudget));
		bOk &= TestEqual(*FString::Printf(TEXT("b = %d: E7's count for the saved limit %lld from c = %lld"), SavedBudget, Limit, Header.ContextLength), int64(After.Num()), Expected);
		bOk &= TestEqual(*FString::Printf(TEXT("b = %d: the stop is Faulted"), SavedBudget), (uint8)End.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= TestEqual(*FString::Printf(TEXT("b = %d: KvBudgetExceeded"), SavedBudget), (uint8)End.Outcome, (uint8)ESuperSLMDecodeOutcome::KvBudgetExceeded);
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 9.7"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
