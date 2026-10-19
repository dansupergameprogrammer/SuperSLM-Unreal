// Plugin paging plan, CPU red suite (step K1): restore admission, the guaranteed-return wait and
// headroom (§3.5 "Fit and wait").
//
//   SuperSLM.Paging.Cpu.Wait.NoGuaranteeRefused        cell 3.2 (a) and (b)                 [K3]
//   SuperSLM.Paging.Cpu.Wait.StrikeConstructions       cell 3.2 (d)                         [K3]
//   SuperSLM.Paging.Cpu.Wait.SharingBurst              cell 3.2 (e)                         [K3]
//   SuperSLM.Paging.Cpu.Wait.CappedGuarantee           cell 3.2 (f)                         [K3]
//   SuperSLM.Paging.Cpu.Wait.Headroom                  cell 4.4 (the 0.5B legs)             [K3]
//
// Every figure is A-EX's (B = 64, block 64, R(512) = 9): a restore at budget 512 and origin 1,000
// is charged 24 and guarantees min(24, 9) = 9. The first restores of each burst are held at their
// apply (HoldApply), so "on arrival" and "at the apply" are separate instants. The 1.5B legs of
// 4.4 are in the K4 figures file. Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// The plan's admission rule (§3.5), run by the test on a burst of equal restores arriving at
	// once: fit when no restore waits ahead and the charge fits; else wait when free(H) + the
	// guarantees ahead - the waiters' charges ahead >= the charge; else refuse on arrival.
	struct FBurstCounts
	{
		int32 Admitted = 0;
		int32 Waited = 0;
		int32 Refused = 0;
	};

	FBurstCounts BurstModel(int64 HFree, int64 Charge, int64 Guarantee, int32 Arrivals)
	{
		FBurstCounts Out;
		for (int32 I = 0; I < Arrivals; ++I)
		{
			if (Out.Waited == 0 && Charge <= HFree)
			{
				++Out.Admitted;
				HFree -= Charge;
			}
			else if (HFree + Guarantee * (Out.Admitted + Out.Waited) - Charge * Out.Waited >= Charge)
			{
				++Out.Waited;
			}
			else
			{
				++Out.Refused;
			}
		}
		return Out;
	}

	// Issues Arrivals restores in one game-thread instant, with the first Held admissions held at
	// their apply. Returns the issued restores and their holds.
	void IssueBurst(USuperSLMSubsystem& Cpu, const TArray<uint8>& Blob, USuperSLMModel* Model, const FSuperSLMPrefix& Hint, int32 Arrivals, int32 Held,
		TArray<FIssuedRestore>& OutRestores, TArray<FSuperSLMPagedHold>& OutHolds)
	{
		for (int32 I = 0; I < Held; ++I)
		{
			OutHolds.Add(FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore));
		}
		for (int32 I = 0; I < Arrivals; ++I)
		{
			OutRestores.Add(IssueRestore(Cpu, Blob, Model, Hint));
		}
	}

	// Lets the burst settle with its holds in place: every refusal on arrival resolves; admitted
	// restores reach their holds; waiters stay Pending.
	FBurstCounts ObserveBurst(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<FIssuedRestore>& Restores, const TArray<FSuperSLMPagedHold>& Holds, int32 ExpectHeld)
	{
		for (int32 I = 0; I < ExpectHeld && I < Holds.Num(); ++I)
		{
			T.TestTrue(*FString::Printf(TEXT("hold %d is reached"), I), WaitHoldReached(Cpu, Holds[I]));
		}
		Drain(Cpu, []() { return false; }, 5.0);
		FBurstCounts Out;
		for (const FIssuedRestore& R : Restores)
		{
			// A refusal on arrival is carried on the handle, never at the call (plan rev 1.9, ruling 1).
			const bool bRefusedOnArrival = R.Sync == ESuperSLMRestoreResult::Success
				&& Cpu.GetLifecycleOpResult(R.Handle) == ESuperSLMRestoreResult::KvPagesExhausted;
			Out.Refused += bRefusedOnArrival ? 1 : 0;
		}
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		Out.Admitted = V.InFlightRestores;
		Out.Waited = V.WaitingRestores;
		return Out;
	}

	bool CountsMatch(FAutomationTestBase& T, const FBurstCounts& Got, const FBurstCounts& Want, const TCHAR* What)
	{
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: admitted (in flight)"), What), Got.Admitted, Want.Admitted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: waiting"), What), Got.Waited, Want.Waited);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: refused on arrival"), What), Got.Refused, Want.Refused);
		return bOk;
	}

	// Releases every hold, then requires every restore not refused on arrival to succeed: no waiter
	// ends short. Every restored sequence is still live afterwards (DiscardAll returns them), so a
	// cell can read the holds the applies kept.
	bool ReleaseAndSettle(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<FIssuedRestore>& Restores, const TArray<FSuperSLMPagedHold>& Holds, const TCHAR* What)
	{
		TArray<ESuperSLMRestoreResult> Before;
		for (const FIssuedRestore& R : Restores)
		{
			Before.Add(OutcomeOf(Cpu, R));
		}
		for (const FSuperSLMPagedHold& Hold : Holds)
		{
			FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
		}
		bool bOk = true;
		for (int32 I = 0; I < Restores.Num(); ++I)
		{
			if (Before[I] != ESuperSLMRestoreResult::Pending)
			{
				continue;
			}
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: restore %d, admitted or waiting, succeeds (no waiter ends KvPagesExhausted)"), What, I),
				(uint8)WaitOp(Cpu, Restores[I].Handle), (uint8)ESuperSLMRestoreResult::Success);
		}
		return bOk;
	}

	void DiscardAll(USuperSLMSubsystem& Cpu, const TArray<FIssuedRestore>& Restores)
	{
		for (const FIssuedRestore& R : Restores)
		{
			Discard(Cpu, R);
		}
	}

	bool ReleaseAndFinish(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<FIssuedRestore>& Restores, const TArray<FSuperSLMPagedHold>& Holds, const TCHAR* What)
	{
		const bool bOk = ReleaseAndSettle(T, Cpu, Restores, Holds, What);
		DiscardAll(Cpu, Restores);
		return bOk;
	}

	// The blob every budget construction restores: an adopter of a 1,000-token prefix (salt 1) at
	// budget 512, so origin 1,000 and charge 24. OutPrefixTokens are the prefix's tokens.
	bool MakeBurstBlob(FAutomationTestBase& T, FPagingRig& Rig, TArray<uint8>& OutBlob, TArray<int32>& OutPrefixTokens)
	{
		return MakeBlobAt(T, Rig, 512, 1000, 1, OutBlob, &OutPrefixTokens) && RequireAExShape(T, Rig)
			&& T.TestEqual(TEXT("the burst's restores are charged 24"), Rig.Geo.RestoreCharge(512, 1000), int64(24));
	}
}

// Cell 3.2 (a) and (b). Two restores in flight (48 of 64; their hint, a 999-token prefix, does not
// share), a third admitted as a waiter (16 + 9 + 9 >= 24). (a) A fourth has 16 + 9 + 9 + 9 - 24 = 19
// < 24: refused KvPagesExhausted on arrival, with every slot's holder kept. (b) The first hold is
// released: that restore applies privately, its guaranteed 9 lets the waiter fit (16 + 9 >= 24), and
// the waiter is admitted at that apply and succeeds.
// Mutant: a waiter admitted with no guarantee (the fourth waits and never resolves while the holds
// are in place: it fails the on-arrival bound).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNoGuaranteeTest, "SuperSLM.Paging.Cpu.Wait.NoGuaranteeRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuNoGuaranteeTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	TArray<int32> PrefixTokens;
	if (!Rig.Init(*this) || !MakeBurstBlob(*this, Rig, Blob, PrefixTokens) || !ConfigureBudget(*this, Rig, MakeConfig(6, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	FSuperSLMPrefix Hint;
	TArray<int32> Short = PrefixTokens;
	Short.SetNum(999);
	if (!BuildPrefix(*this, Cpu, Short, Hint))
	{
		return false;
	}
	const FSuperSLMPagedHold H1 = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
	const FSuperSLMPagedHold H2 = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
	const FIssuedRestore R1 = IssueRestore(Cpu, Blob, Rig.Model, Hint);
	const FIssuedRestore R2 = IssueRestore(Cpu, Blob, Rig.Model, Hint);
	bool bOk = TestTrue(TEXT("two restores are held in flight"), WaitHoldReached(Cpu, H1) && WaitHoldReached(Cpu, H2));
	bOk &= TestEqual(TEXT("48 of 64 charged"), Charged(Cpu, ERegion::H), int64(48));
	const FIssuedRestore W = IssueRestore(Cpu, Blob, Rig.Model, Hint);
	Drain(Cpu, []() { return false; }, 2.0);
	bOk &= TestEqual(TEXT("the third waits"), (uint8)OutcomeOf(Cpu, W), (uint8)ESuperSLMRestoreResult::Pending);
	bOk &= TestEqual(TEXT("one waiter"), LedgerOf(Cpu).WaitingRestores, 1);
	TArray<int64> Generations;
	for (int32 Slot = 0; Slot < 6; ++Slot)
	{
		Generations.Add(FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot));
	}
	// (a)
	const FIssuedRestore Fourth = IssueRestore(Cpu, Blob, Rig.Model, Hint);
	bOk &= ExpectRefusedOnArrival(*this, Cpu, Fourth, int64(48), TEXT("(a) the fourth: 19 < 24"));
	Discard(Cpu, Fourth);
	for (int32 Slot = 0; Slot < 6; ++Slot)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("(a) slot %d keeps its holder"), Slot), FSuperSLMPagedTestSeam::SlotHolderGeneration(Cpu, Slot), Generations[Slot]);
	}
	// (b)
	bOk &= TestTrue(TEXT("(b) the first hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, H1));
	bOk &= TestEqual(TEXT("(b) the first restore succeeds"), (uint8)WaitOp(Cpu, R1.Handle), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("(b) it applied privately: its hold is 15"), FSuperSLMPagedTestSeam::SequenceHeadroomHold(Cpu, R1.Sequence), int64(15));
	bOk &= TestEqual(TEXT("(b) the waiter is admitted at that apply and succeeds"), (uint8)WaitOp(Cpu, W.Handle), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestTrue(TEXT("(b) the second hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, H2));
	bOk &= TestEqual(TEXT("(b) the second restore succeeds"), (uint8)WaitOp(Cpu, R2.Handle), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("(b) after every apply"));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 3.2 (a), (b)"));
	return bOk;
}

// Cell 3.2 (d): the strike's constructions, each 10 restores at once at the executed shape (budget
// 512, origin 1,000; one headroom block). The hint is a live prefix of 1,001 tokens, or of 999,
// against origin 1,000; a prefix with one token differing in the full-page span (position 0); a
// prefix differing at the last full-page position (959), the reading taken for "a prefix from a
// different producer" (see the K1 record); or a whole-reserve 'SSB5' blob with a live hint. Expected
// by the plan's model (computed here): each budget construction admits 2, holds 1 waiting that is
// admitted, and refuses 7 on arrival; the 'SSB5' construction admits 1 and refuses 9. No waiter ever
// resolves KvPagesExhausted.
// Mutant: a hint's share counted as guaranteed (it yields waiters that end short: 7, or 9 for 'SSB5').
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuStrikeTest, "SuperSLM.Paging.Cpu.Wait.StrikeConstructions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuStrikeTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob, Ssb5;
	TArray<int32> PrefixTokens;
	if (!Rig.Init(*this) || !MakeSsb5Blob(*this, Rig, Ssb5) || !MakeBurstBlob(*this, Rig, Blob, PrefixTokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	TArray<int32> Longer = PrefixTokens;
	Longer.Add(PrefixTokens[0]);
	TArray<int32> Shorter = PrefixTokens;
	Shorter.SetNum(999);
	struct FConstruction { const TCHAR* Name; TArray<int32> HintTokens; bool bSsb5; };
	const FConstruction Constructions[] = {
		{ TEXT("hint of 1,001 tokens"), Longer, false },
		{ TEXT("hint of 999 tokens"), Shorter, false },
		{ TEXT("one token differing at position 0"), WithTokenChanged(PrefixTokens, 0), false },
		{ TEXT("one token differing at position 959 (a different producer)"), WithTokenChanged(PrefixTokens, 959), false },
		{ TEXT("'SSB5' blob with a live hint"), PrefixTokens, true },
	};
	bool bOk = true;
	for (const FConstruction& C : Constructions)
	{
		FSuperSLMPrefix Hint;
		if (!ConfigureBudget(*this, Rig, MakeConfig(10, 512, 1, 1)) || !BuildPrefix(*this, Cpu, C.HintTokens, Hint))
		{
			return false;
		}
		const int64 Charge = C.bSsb5 ? G.Block() : G.RestoreCharge(512, 1000);
		const FBurstCounts Want = BurstModel(G.Block(), Charge, G.Guarantee(Charge, 512), 10);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the model gives the plan's counts"), C.Name), C.bSsb5
			? (Want.Admitted == 1 && Want.Waited == 0 && Want.Refused == 9)
			: (Want.Admitted == 2 && Want.Waited == 1 && Want.Refused == 7));
		TArray<FIssuedRestore> Restores;
		TArray<FSuperSLMPagedHold> Holds;
		IssueBurst(Cpu, C.bSsb5 ? Ssb5 : Blob, Rig.Model, Hint, 10, Want.Admitted, Restores, Holds);
		bOk &= CountsMatch(*this, ObserveBurst(*this, Cpu, Restores, Holds, Want.Admitted), Want, C.Name);
		bOk &= ReleaseAndFinish(*this, Cpu, Restores, Holds, C.Name);
		bOk &= NoLedgerFault(*this, Cpu, C.Name);
	}
	return bOk;
}

// Cell 3.2 (e): the same burst with hints that do share (the saved prefix, rebuilt from its tokens):
// the same counts (2 admitted, 1 waited, 7 refused; the rule does not depend on sharing), and every
// apply frees its whole charge of 24 (a sharing apply at matching budget holds 0 on H), by the
// ledger and by form 1 after every apply.
// Mutant: a share counted as guaranteed (more waiters); a sharing apply that keeps a hold.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuSharingBurstTest, "SuperSLM.Paging.Cpu.Wait.SharingBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuSharingBurstTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	TArray<int32> PrefixTokens;
	FSuperSLMPrefix Hint;
	if (!Rig.Init(*this) || !MakeBurstBlob(*this, Rig, Blob, PrefixTokens) || !ConfigureBudget(*this, Rig, MakeConfig(10, 512, 1, 1))
		|| !BuildPrefix(*this, *Rig.Cpu, PrefixTokens, Hint))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FBurstCounts Want = BurstModel(Rig.Geo.Block(), 24, 9, 10);
	TArray<FIssuedRestore> Restores;
	TArray<FSuperSLMPagedHold> Holds;
	IssueBurst(Cpu, Blob, Rig.Model, Hint, 10, Want.Admitted, Restores, Holds);
	bool bOk = CountsMatch(*this, ObserveBurst(*this, Cpu, Restores, Holds, Want.Admitted), Want, TEXT("sharing burst"));
	bOk &= ReleaseAndSettle(*this, Cpu, Restores, Holds, TEXT("sharing burst"));
	// Read before any sequence is returned: a return refunds a hold, which would hide one kept.
	bOk &= TestEqual(TEXT("every sharing apply freed its whole charge: H is empty with every holder live"), Charged(Cpu, ERegion::H), int64(0));
	for (int32 I = 0; I < Restores.Num(); ++I)
	{
		if (OutcomeOf(Cpu, Restores[I]) == ESuperSLMRestoreResult::Success)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("sharing burst: restore %d keeps no headroom hold"), I),
				FSuperSLMPagedTestSeam::SequenceHeadroomHold(Cpu, Restores[I].Sequence), int64(0));
		}
	}
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after the sharing burst, every holder live"));
	DiscardAll(Cpu, Restores);
	bOk &= TestEqual(TEXT("H is still empty once every holder is returned"), Charged(Cpu, ERegion::H), int64(0));
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after the sharing burst"));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 3.2 (e)"));
	return bOk;
}

// Cell 3.2 (f): the guarantee is capped by the charge. Two restores held in flight: one charged 24
// (guarantee 9) and a small one at budget 64, origin 0, charged 2 (guarantee 2, not 9). H has 38
// free. Each leg is a fresh rebuild of that state.
//   - A restore charged 49 (budget 512, origin 2,560) waits: 38 + 9 + 2 = 49. After the small one's
//     apply its H hold is max(0, 2 - 9) = 0 and form 1 holds; the 49 is admitted when both have
//     applied, and succeeds.
//   - A restore charged 50 (origin 2,624) is refused KvPagesExhausted on arrival: 49 < 50.
// Mutant: the flat R(SequenceBudget) guarantee (38 + 9 + 9 = 56 admits the 50 as a waiter, which
// ends short).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuCappedGuaranteeTest, "SuperSLM.Paging.Cpu.Wait.CappedGuarantee",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuCappedGuaranteeTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000, B64, B2560, B2624;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !RequireAExShape(*this, Rig) || !MakeBlobAt(*this, Rig, 64, 0, 2, B64)
		|| !MakeBlobAt(*this, Rig, 512, 2560, 3, B2560) || !MakeBlobAt(*this, Rig, 512, 2624, 4, B2624))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	bool bOk = TestEqual(TEXT("charges: 24, 2, 49, 50"), FString::Printf(TEXT("%lld %lld %lld %lld"), G.RestoreCharge(512, 1000), G.RestoreCharge(64, 0),
		G.RestoreCharge(512, 2560), G.RestoreCharge(512, 2624)), FString(TEXT("24 2 49 50")));
	FIssuedRestore Big, Small;
	FSuperSLMPagedHold HBig, HSmall;
	auto Build = [&]()
	{
		if (!ConfigureBudget(*this, Rig, MakeConfig(3, 512, 0, 1)))
		{
			return false;
		}
		HBig = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		HSmall = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
		Big = IssueRestore(Cpu, B1000, Rig.Model);
		Small = IssueRestore(Cpu, B64, Rig.Model);
		return TestTrue(TEXT("both are held in flight"), WaitHoldReached(Cpu, HBig) && WaitHoldReached(Cpu, HSmall))
			&& TestEqual(TEXT("H has 38 free"), Free(Cpu, ERegion::H), int64(38));
	};

	// The 49 waits and is admitted.
	if (!Build())
	{
		return false;
	}
	{
		const FIssuedRestore W = IssueRestore(Cpu, B2560, Rig.Model);
		Drain(Cpu, []() { return false; }, 2.0);
		bOk &= TestEqual(TEXT("the 49 waits (38 + 9 + 2 = 49)"), (uint8)OutcomeOf(Cpu, W), (uint8)ESuperSLMRestoreResult::Pending);
		bOk &= TestTrue(TEXT("the small restore's hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, HSmall));
		bOk &= TestEqual(TEXT("the small restore succeeds"), (uint8)WaitOp(Cpu, Small.Handle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("its H hold is max(0, 2 - 9) = 0"), FSuperSLMPagedTestSeam::SequenceHeadroomHold(Cpu, Small.Sequence), int64(0));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after the small restore's apply"));
		bOk &= TestTrue(TEXT("the big restore's hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, HBig));
		bOk &= TestEqual(TEXT("the big restore succeeds"), (uint8)WaitOp(Cpu, Big.Handle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("the 49 is admitted and succeeds"), (uint8)WaitOp(Cpu, W.Handle), (uint8)ESuperSLMRestoreResult::Success);
	}

	// The 50 is refused, on a fresh rebuild.
	if (!Build())
	{
		return false;
	}
	{
		const FIssuedRestore R = IssueRestore(Cpu, B2624, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Cpu, R, Charged(Cpu, ERegion::H), TEXT("the 50: 49 < 50"));
		FSuperSLMPagedTestSeam::ReleaseHold(Cpu, HBig);
		FSuperSLMPagedTestSeam::ReleaseHold(Cpu, HSmall);
		DrainQuiet(Cpu);
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 3.2 (f)"));
	return bOk;
}

// Cell 4.4 (the 0.5B legs; §3.5 "What the hint buys"):
//   - RestoreHeadroomBlocks = 0 with a hinted restore at matching budget: refused KvPagesExhausted on
//     arrival (its admission charge is 24 > 0).
//   - A burst of 10 hinted restores with 1 and with 3 blocks: the in-flight count, read from the
//     ledger under HoldApply, is at most floor(64k / 24) (2 or 8), with H charged 24 per restore in
//     flight, and the admitted, waited and refused counts match the plan's model for that k
//     (2/1/7 and 8/2/0).
//   - Private holders per block (form 2): at one block, private restores applied one at a time are
//     admitted 3 times; the fourth is refused on arrival (19 < 24); after one holder resets the same
//     restore is admitted. Form 1 after every apply.
// Mutant: a hold settled one page smaller (14). At 0.5B it does not change the holder count (22 < 24
// still refuses the fourth), so it is killed here by form 1 after each apply (charge < drawn); the
// 1.5B leg (K4 figures file) kills it by count (35 holders, not 33).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuHeadroomTest, "SuperSLM.Paging.Cpu.Wait.Headroom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuHeadroomTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	TArray<int32> PrefixTokens;
	if (!Rig.Init(*this) || !MakeBurstBlob(*this, Rig, Blob, PrefixTokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	bool bOk = true;

	// No headroom.
	{
		FSuperSLMPrefix Hint;
		if (!ConfigureBudget(*this, Rig, MakeConfig(2, 512, 1, 0)) || !BuildPrefix(*this, Cpu, PrefixTokens, Hint))
		{
			return false;
		}
		const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model, Hint);
		bOk &= ExpectRefusedOnArrival(*this, Cpu, R, int64(0), TEXT("no headroom: a hinted restore charged 24"));
		Discard(Cpu, R);
	}

	// Bursts at 1 and 3 blocks.
	for (const int32 Blocks : { 1, 3 })
	{
		FSuperSLMPrefix Hint;
		if (!ConfigureBudget(*this, Rig, MakeConfig(10, 512, 1, Blocks)) || !BuildPrefix(*this, Cpu, PrefixTokens, Hint))
		{
			return false;
		}
		const FBurstCounts Want = BurstModel(G.Block() * Blocks, 24, 9, 10);
		const int32 Bound = int32((G.Block() * Blocks) / 24);
		bOk &= TestTrue(*FString::Printf(TEXT("%d block(s): the model's in-flight count is within floor(64k / 24) = %d"), Blocks, Bound), Want.Admitted <= Bound);
		bOk &= TestTrue(*FString::Printf(TEXT("%d block(s): the model gives the plan's counts"), Blocks), Blocks == 1
			? (Want.Admitted == 2 && Want.Waited == 1 && Want.Refused == 7)
			: (Want.Admitted == 8 && Want.Waited == 2 && Want.Refused == 0));
		TArray<FIssuedRestore> Restores;
		TArray<FSuperSLMPagedHold> Holds;
		IssueBurst(Cpu, Blob, Rig.Model, Hint, 10, Want.Admitted, Restores, Holds);
		const FBurstCounts Got = ObserveBurst(*this, Cpu, Restores, Holds, Want.Admitted);
		bOk &= CountsMatch(*this, Got, Want, *FString::Printf(TEXT("%d block(s)"), Blocks));
		bOk &= TestEqual(*FString::Printf(TEXT("%d block(s): H charged 24 per restore in flight"), Blocks), Charged(Cpu, ERegion::H), int64(24) * Got.Admitted);
		bOk &= ReleaseAndFinish(*this, Cpu, Restores, Holds, *FString::Printf(TEXT("%d block(s)"), Blocks));
	}

	// Private holders per block.
	{
		if (!ConfigureBudget(*this, Rig, MakeConfig(5, 512, 0, 1)))
		{
			return false;
		}
		TArray<FSuperSLMSequence> Holders;
		for (int32 I = 0; I < 3; ++I)
		{
			FSuperSLMSequence S;
			if (!ApplyRestore(*this, Cpu, Blob, Rig.Model, S))
			{
				return false;
			}
			Holders.Add(S);
			bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("private holder %d applied"), I + 1));
		}
		bOk &= TestEqual(TEXT("three holds of 15 leave 19 free"), Free(Cpu, ERegion::H), int64(19));
		{
			const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model);
			bOk &= ExpectRefusedOnArrival(*this, Cpu, R, int64(45), TEXT("the fourth private restore: 19 < 24"));
			Discard(Cpu, R);
		}
		bOk &= ResetAndWait(*this, Cpu, Holders[0]);
		FSuperSLMSequence Again;
		bOk &= ApplyRestore(*this, Cpu, Blob, Rig.Model, Again);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("the re-issued restore applied"));
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 4.4"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
