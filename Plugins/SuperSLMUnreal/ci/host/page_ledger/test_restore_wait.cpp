// Region H: restore charges and settlement, the guaranteed-return wait rule, restore bursts, and
// create-first normalization. Plugin paging plan §3.5, UP-7; cells 1.3 (restore rows), 1.8,
// 3.2 (a)-(e), 4.4, and the probe model's constructions (the plan's restore wait probe).

#include "ledger_test_support.h"
#include "restore_wait_model.h"

#include <cstdio>
#include <deque>
#include <random>

using namespace ledgertest;
using namespace SuperSLMPaging;

// Places H at an exact settled hold with nothing in flight, using filler restores in the slots
// from FirstSlot on. Each filler holds at most P_blk - R(SequenceBudget) - R(1) + R(1) pages.
static bool FillHeadroom(FWorld& W, const FShape& Sh, uint32_t Hold, int32_t FirstSlot)
{
	const uint32_t MaxHold = TPblk(Sh.B, Sh.Cap) - TR(Sh.B, Sh.Cap, Sh.SequenceBudget);
	int32_t Slot = FirstSlot;
	while (Hold > 0)
	{
		const uint32_t Take = Hold < MaxHold ? Hold : MaxHold;
		FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(Slot, 1, OriginForHold(Sh, Take)));
		if (J.Adm.Outcome != EPageAdmit::Admitted) return false;
		W.RunRestore(J);
		if (!W.ApplyRestore(J)) return false;
		Hold -= Take;
		++Slot;
	}
	return true;
}

LEDGER_TEST(restore_settle_at_apply, "§3.5 restore charge and settle at apply; 9.2/9.3 host legs (R(b) vs R(b) + E); 2.3 charge = E6's formula [K3/K7]",
            "stub admits nothing")
{
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 6, 1, 2, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	// Private: charge 95, settles to a hold of 62.
	FWorld::FRestoreJob P = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	LT_REQUIRE(P.Adm.Outcome == EPageAdmit::Admitted, Want("private restore", P.Adm.Outcome, EPageAdmit::Admitted));
	LT_EQ(P.Adm.ChargedPages, 95, "charged R(b) + E");
	W.RunRestore(P);
	{
		FSoft S;
		W.Form1(S, "private restore in flight");
		LT_REQUIRE(S.bOk, S.First);
	}
	LT_REQUIRE(W.ApplyRestore(P), "apply");
	LT_EQ(W.Ledger.SlotHeadroomHold(0), 62, "net hold without sharing: R(b) + E - R(SequenceBudget)");
	LT_EQ(W.Charged(ERegion::H), 62, "H");
	LT_REQUIRE(W.Ledger.SlotShape(0).Budget == 512 && W.Ledger.SlotShape(0).Origin == 1000, "the slot takes the saved shape");
	// Hinted and sharing at matching budget: the hold is 0, the whole 95 returns.
	FWorld::FRestoreJob Sd = W.AdmitRestore(HintedRestore(1, 512, 1000, N));
	LT_EQ(Sd.Adm.ChargedPages, 95, "a hinted restore is charged as if it does not share");
	W.RunRestore(Sd);
	LT_EQ(Sd.Shared, 62, "the pool model shares");
	LT_REQUIRE(W.ApplyRestore(Sd), "apply");
	LT_EQ(W.Ledger.SlotHeadroomHold(1), 0, "shared at matching budget holds 0");
	LT_EQ(W.Ledger.DependentCount(N), 1, "the sharing restore registers");
	// Budget above SequenceBudget: R(1024) = 65, origin 0: hold 32.
	FWorld::FRestoreJob Big = W.AdmitRestore(BudgetRestore(2, 1024, 0));
	LT_EQ(Big.Adm.ChargedPages, 65, "R(1024)");
	W.RunRestore(Big);
	LT_REQUIRE(W.ApplyRestore(Big), "apply");
	LT_EQ(W.Ledger.SlotHeadroomHold(2), 32, "hold R(1024) - R(512)");
	// Budget below: R(256) = 17 < 33: hold 0.
	FWorld::FRestoreJob Small = W.AdmitRestore(BudgetRestore(3, 256, 0));
	W.RunRestore(Small);
	LT_REQUIRE(W.ApplyRestore(Small), "apply");
	LT_EQ(W.Ledger.SlotHeadroomHold(3), 0, "a draw below R(SequenceBudget) holds nothing on H");
	// SSB5: P_blk, hold 223.
	FWorld::FRestoreJob Old = W.AdmitRestore(WholeReserveRestore(4));
	LT_EQ(Old.Adm.ChargedPages, 256, "P_blk");
	W.RunRestore(Old);
	LT_REQUIRE(W.ApplyRestore(Old), "apply");
	LT_EQ(W.Ledger.SlotHeadroomHold(4), 223, "hold P_blk - R(512)");
	LT_EQ(W.Charged(ERegion::H), 62 + 0 + 32 + 0 + 223, "H is the sum of the holds");
	FSoft S;
	W.Form1(S, "quiescent");
	LT_REQUIRE(S.bOk, S.First);
	// The private holder's reset returns its materialized pages.
	LT_REQUIRE(W.Reset(0), "reset");
	LT_EQ(W.Ledger.SlotHeadroomHold(0), 0, "E returns at the first reset");
}

// 1.3 rows on H. Hold = the fillers' settled hold, so H free = capacity - Hold with nothing in
// flight; the boundary restore must then be refused on arrival (no guarantee) or admitted.
static FSoft RestoreBoundary(const FShape& Sh, int32_t Blocks, uint32_t Hold, const FRestoreRequest& Req, bool bWantAdmit,
                             EChargeKind Kind, uint32_t Under)
{
	FSoft S;
	FWorld W(MakeConfig(Sh, 8, 1, Blocks, 1, false, 0));
	S.Expect(W.bConfigured, "Configure");
	S.Expect(FillHeadroom(W, Sh, Hold, 1), "fill H");
	W.Ledger.SetForceUnderChargeForTest(Kind, Under);
	const FAdmission A = W.Ledger.AdmitRestore(Req);
	const EPageAdmit WantA = bWantAdmit ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("boundary restore at H free " + std::to_string(W.Free(ERegion::H)), A.Outcome, WantA));
	return S;
}

LEDGER_TEST(cell_1_3_restore_without_e_boundary, "1.3 row 'restore charged without E' (form 2) [K3]",
            "stub admits nothing")
{
	const FShape Sh = GpuShape();
	const FRestoreRequest R = BudgetRestore(0, 512, 1000);
	LT_HOLDS(RestoreBoundary(Sh, 1, 256 - 94, R, false, EChargeKind::BudgetRestore, 0), "H free = R(b) + E - 1 refuses on arrival");
	LT_HOLDS(RestoreBoundary(Sh, 1, 256 - 95, R, true, EChargeKind::BudgetRestore, 0), "H free = R(b) + E admits");
	LT_KILLS(RestoreBoundary(Sh, 1, 256 - 94, R, false, EChargeKind::BudgetRestore, 62), "ForceUnderCharge(BudgetRestore, E)");
	const FShape Pk = CpuShape();
	LT_HOLDS(RestoreBoundary(Pk, 1, 64 - 23, R, false, EChargeKind::BudgetRestore, 0), "B = 64: H free 23 refuses a 24-page restore");
	LT_KILLS(RestoreBoundary(Pk, 1, 64 - 23, R, false, EChargeKind::BudgetRestore, 15), "B = 64: charged without E");
}

LEDGER_TEST(cell_1_3_ssb5_undercharge_boundary, "1.3 row 'SSB5 charged R(b)' (form 2) [K3]",
            "stub admits nothing")
{
	const FShape Sh = GpuShape();
	const FRestoreRequest R = WholeReserveRestore(0);
	LT_HOLDS(RestoreBoundary(Sh, 2, 512 - 255, R, false, EChargeKind::WholeReserveRestore, 0), "H free = P_blk - 1 refuses");
	LT_HOLDS(RestoreBoundary(Sh, 2, 512 - 256, R, true, EChargeKind::WholeReserveRestore, 0), "H free = P_blk admits");
	LT_KILLS(RestoreBoundary(Sh, 2, 512 - 255, R, false, EChargeKind::WholeReserveRestore, 256 - 33),
	         "ForceUnderCharge(WholeReserveRestore): SSB5 charged R(SequenceBudget)");
}

// 1.3 row "H refunded at apply for a private restore": after the apply, H free = 61; a 62-page
// restore is refused (the mutant returned the whole 95 at the apply, and admits it).
static FSoft EarlyRefundScenario(ELedgerMutation M, uint32_t PreHold)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 8, 1, 1, 1, false, 0), M);
	S.Expect(FillHeadroom(W, Sh, PreHold, 2), "fill H");
	FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "the private restore");
	W.RunRestore(J);
	W.ApplyRestore(J);
	const uint32_t Free = W.Free(ERegion::H);
	const FAdmission A = W.Ledger.AdmitRestore(BudgetRestore(1, 1, OriginForCharge(Sh, 62)));
	const EPageAdmit WantA = (256 - PreHold - 62) >= 62 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("a 62-page restore at H free " + std::to_string(Free), A.Outcome, WantA));
	W.Form1(S, "after the apply");
	return S;
}

LEDGER_TEST(cell_1_3_private_restore_refunded_at_apply_boundary, "1.3 row 'H refunded at apply for a private restore' (form 2) [K3]",
            "stub admits nothing")
{
	LT_HOLDS(EarlyRefundScenario(ELedgerMutation::None, 133), "H free 61 refuses a 62-page restore");
	LT_HOLDS(EarlyRefundScenario(ELedgerMutation::None, 132), "H free 62 admits it");
	LT_KILLS(EarlyRefundScenario(ELedgerMutation::PrivateRestoreRefundedAtApply, 133), "H refunded at apply for a private restore");
}

// ---- The wait rule ----

// Cell 3.2 (a) and (b) on one config: two in flight under HoldApply, a third waits on the
// guarantee, a fourth has no guarantee and is refused on arrival; the first apply (private: its
// hint fails) admits the waiter.
static FSoft WaitAB(const FShape& Sh, bool bGpu, ELedgerMutation M)
{
	FSoft S;
	const FPageLedgerConfig C = bGpu ? MakeGpuConfig(Sh, 6, 2, 1) : MakeConfig(Sh, 6, 2, 1, 1, false, 0);
	FWorld W(C, M);
	S.Expect(W.bConfigured, "Configure");
	const FNodeId Hint = W.Build(1001);  // length 1,001 against origin 1,000: the hint fails
	const uint32_t Charge = TR(Sh.B, Sh.Cap, 512) + TE(Sh.B, Sh.Cap, 512, 1000);
	FWorld::FRestoreJob J[4];
	for (int32_t I = 0; I < 4; ++I) J[I] = W.AdmitRestore(HintedRestore(I, 512, 1000, Hint));
	S.Expect(J[0].Adm.Outcome == EPageAdmit::Admitted && J[1].Adm.Outcome == EPageAdmit::Admitted, "(a) two in flight");
	S.Expect(W.Charged(ERegion::H) == 2 * Charge, WantN("(a) H holds the two charges", W.Charged(ERegion::H), 2 * Charge));
	S.Expect(J[2].Adm.Outcome == EPageAdmit::Waiting, Want("(a) the third waits on the guarantee", J[2].Adm.Outcome, EPageAdmit::Waiting));
	const FHolderShape Before = W.Ledger.SlotShape(3);
	S.Expect(J[3].Adm.Outcome == EPageAdmit::KvPagesExhausted,
	         Want("(a) the fourth has no guarantee: refused on arrival", J[3].Adm.Outcome, EPageAdmit::KvPagesExhausted));
	S.Expect(!W.Ledger.SlotHasJob(3) && W.Ledger.SlotShape(3).Budget == Before.Budget, "(a) the old sequence is untouched");
	S.Expect(W.Ledger.DrainResolutions().empty(), "(a) nothing resolves while the holds are in place");
	// (b) Release the first hold: it applies privately, and the guaranteed return admits the waiter.
	W.RunRestore(J[0]);
	S.Expect(J[0].Shared == 0, "(b) the hint fails");
	W.ApplyRestore(J[0]);
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	S.Expect(Res.size() == 1 && Res[0].Ticket == J[2].Adm.Ticket && Res[0].Outcome == EPageAdmit::Admitted,
	         "(b) the waiter is admitted at that apply");
	W.RunRestore(J[2]);
	S.Expect(J[2].NewHolder >= 0 && W.ApplyRestore(J[2]), "(b) the waiter's restore succeeds");
	W.RunRestore(J[1]);
	W.ApplyRestore(J[1]);
	S.Expect(W.Ledger.GetStats().WaitingRestores == 0 && W.Ledger.GetStats().InFlightRestores == 0, "(b) quiescent");
	W.Form1(S, "(b) end");
	return S;
}

LEDGER_TEST(cell_3_2_ab_wait_on_guarantee_cpu, "3.2 (a), (b) [K3]; 11 guaranteed-return wait predicate; §7 'waits only on guaranteed returns'",
            "stub admits nothing")
{
	LT_HOLDS(WaitAB(GpuShape(), false, ELedgerMutation::None), "B = 16 shape: 190 of 256 in flight, 66 + 33 + 33 >= 95 waits, 70 < 95 refused");
	LT_HOLDS(WaitAB(CpuShape(), false, ELedgerMutation::None), "B = 64: 48 of 64 in flight, 16 + 9 + 9 >= 24 waits, 19 < 24 refused");
	LT_KILLS(WaitAB(GpuShape(), false, ELedgerMutation::WaiterAdmittedWithoutGuarantee), "a waiter admitted with no guarantee");
}

LEDGER_TEST(cell_3_2_c_wait_on_guarantee_gpu_twin, "3.2 (c) [K7]: the GPU pooled twin of (a) and (b)",
            "stub admits nothing")
{
	LT_HOLDS(WaitAB(GpuShape(), true, ELedgerMutation::None), "GPU, B = 16");
	LT_HOLDS(WaitAB(CpuShape(), true, ELedgerMutation::None), "GPU, B = 64");
	LT_KILLS(WaitAB(GpuShape(), true, ELedgerMutation::WaiterAdmittedWithoutGuarantee), "GPU: waiter admitted with no guarantee");
}

// A burst of restores arriving at once, then applied FIFO under the ledger; the outcome per
// arrival, the waiters admitted and the short waiters (still waiting with nothing in flight).
struct FBurst
{
	std::vector<EModelOutcome> Outcomes;
	uint32_t WaitersAdmitted = 0;
	uint32_t ShortWaiters = 0;
	uint32_t ShortResolutions = 0;  // waiters resolved anything but Admitted while configured
	std::vector<uint32_t> HAfterEachApply;
	uint32_t MaxInFlight = 0;
	bool bOk = true;
	std::string Why;
};

enum class EConstruction : int
{
	Hint1001,
	Hint999,
	ByteDiffers,
	OtherProducer,
	Ssb5WithHint,
	HintShares,
	Unhinted
};

static const char* ConstructionName(EConstruction C)
{
	switch (C)
	{
	case EConstruction::Hint1001: return "(e) live prefix length 1,001 vs origin 1,000";
	case EConstruction::Hint999: return "(e) live prefix length 999 vs origin 1,000";
	case EConstruction::ByteDiffers: return "(f) one V-row byte differs";
	case EConstruction::OtherProducer: return "(f) a different producer";
	case EConstruction::Ssb5WithHint: return "(a) whole-reserve SSB5 with a live hint";
	case EConstruction::HintShares: return "control: hinted, sharing";
	case EConstruction::Unhinted: return "control: unhinted";
	}
	return "?";
}

static FBurst RunBurst(const FShape& Sh, bool bGpu, int32_t Blocks, EConstruction Con, int Count, ELedgerMutation M)
{
	FBurst Out;
	const FPageLedgerConfig C = bGpu ? MakeGpuConfig(Sh, Count, 1, Blocks) : MakeConfig(Sh, Count, 1, Blocks, 1, false, 0);
	FWorld W(C, M);
	if (!W.bConfigured)
	{
		Out.bOk = false;
		Out.Why = "Configure";
		return Out;
	}
	int64_t HintLen = 1000;
	if (Con == EConstruction::Hint1001) HintLen = 1001;
	if (Con == EConstruction::Hint999) HintLen = 999;
	const FNodeId Hint = W.Build(HintLen);
	const bool bBytes = !(Con == EConstruction::ByteDiffers || Con == EConstruction::OtherProducer);
	std::vector<FWorld::FRestoreJob> Jobs;
	for (int I = 0; I < Count; ++I)
	{
		FRestoreRequest R;
		if (Con == EConstruction::Ssb5WithHint)
		{
			R = WholeReserveRestore(I);
			R.Hint = EHintKind::Prefix;
			R.HintNode = Hint;
		}
		else if (Con == EConstruction::Unhinted)
		{
			R = BudgetRestore(I, 512, 1000);
		}
		else
		{
			R = HintedRestore(I, 512, 1000, Hint);
		}
		Jobs.push_back(W.AdmitRestore(R));
		const EPageAdmit A = Jobs.back().Adm.Outcome;
		Out.Outcomes.push_back(A == EPageAdmit::Admitted ? EModelOutcome::Admitted
		                                                 : (A == EPageAdmit::Waiting ? EModelOutcome::Waiting : EModelOutcome::Refused));
	}
	std::deque<size_t> InFlight;
	std::map<FTicket, size_t> WaitingByTicket;
	for (size_t I = 0; I < Jobs.size(); ++I)
	{
		if (Jobs[I].Adm.Outcome == EPageAdmit::Admitted) InFlight.push_back(I);
		if (Jobs[I].Adm.Outcome == EPageAdmit::Waiting) WaitingByTicket[Jobs[I].Adm.Ticket] = I;
	}
	Out.MaxInFlight = W.Ledger.GetStats().InFlightRestores;
	while (!InFlight.empty())
	{
		const size_t I = InFlight.front();
		InFlight.pop_front();
		W.RunRestore(Jobs[I], bBytes);
		W.ApplyRestore(Jobs[I]);
		Out.HAfterEachApply.push_back(W.Charged(ERegion::H));
		for (const FWaitResolution& R : W.Ledger.DrainResolutions())
		{
			auto It = WaitingByTicket.find(R.Ticket);
			if (It == WaitingByTicket.end()) continue;
			if (R.Outcome == EPageAdmit::Admitted)
			{
				++Out.WaitersAdmitted;
				InFlight.push_back(It->second);
			}
			else
			{
				++Out.ShortResolutions;
			}
			WaitingByTicket.erase(It);
		}
		const uint32_t Fl = W.Ledger.GetStats().InFlightRestores;
		if (Fl > Out.MaxInFlight) Out.MaxInFlight = Fl;
		FSoft S;
		W.Form1(S, "burst apply");
		if (!S.bOk && Out.bOk)
		{
			Out.bOk = false;
			Out.Why = S.First;
		}
	}
	Out.ShortWaiters = static_cast<uint32_t>(WaitingByTicket.size());
	return Out;
}

static FModelResult ModelFor(const FShape& Sh, int32_t Blocks, EConstruction Con, int Count, bool bMutant)
{
	const uint32_t Blk = TPblk(Sh.B, Sh.Cap);
	const uint32_t G = TR(Sh.B, Sh.Cap, Sh.SequenceBudget);
	FModelRestore R;
	R.Charge = (Con == EConstruction::Ssb5WithHint) ? Blk : TR(Sh.B, Sh.Cap, 512) + TE(Sh.B, Sh.Cap, 512, 1000);
	R.SharedDraw = TR(Sh.B, Sh.Cap, 512);
	R.Share = Con == EConstruction::Unhinted ? EModelShare::Unhinted
	                                          : (Con == EConstruction::HintShares ? EModelShare::HintShares : EModelShare::HintFails);
	return RunRestoreWaitModel(std::vector<FModelRestore>(static_cast<size_t>(Count), R), Blk * static_cast<uint32_t>(Blocks), G, bMutant);
}

static FSoft BurstMatchesModel(const FShape& Sh, bool bGpu, int32_t Blocks, EConstruction Con, ELedgerMutation M, bool bMutantModel)
{
	FSoft S;
	const FBurst L = RunBurst(Sh, bGpu, Blocks, Con, 10, M);
	const FModelResult Md = ModelFor(Sh, Blocks, Con, 10, bMutantModel);
	const std::string Tag = std::string(Sh.Name) + (bGpu ? " GPU " : " CPU ") + ConstructionName(Con) + " x" + std::to_string(Blocks);
	S.Expect(L.bOk, Tag + ": " + L.Why);
	S.Expect(L.Outcomes == Md.Outcomes, Tag + ": per-arrival outcomes differ from the probe model");
	S.Expect(L.WaitersAdmitted == Md.WaitersAdmitted, WantN(Tag + ": waiters admitted", L.WaitersAdmitted, Md.WaitersAdmitted));
	S.Expect(L.ShortWaiters == Md.ShortWaiters, WantN(Tag + ": short waiters", L.ShortWaiters, Md.ShortWaiters));
	S.Expect(L.ShortResolutions == 0, Tag + ": a waiter resolved other than Admitted while configured");
	return S;
}

static int Count(const std::vector<EModelOutcome>& V, EModelOutcome O)
{
	int N = 0;
	for (EModelOutcome X : V) N += X == O ? 1 : 0;
	return N;
}

LEDGER_TEST(cell_3_2_d_constructions_vs_probe_model, "3.2 (d) [K3/K7]; the probe model's five constructions and two controls, CPU and GPU, at B = 16 and B = 64",
            "stub admits nothing, so every arrival reads Refused against the model's 2/1/7")
{
	// The model alone reproduces the probe's printed figures.
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		const FModelResult Budget = ModelFor(Sh, 1, EConstruction::Hint1001, 10, false);
		LT_EQ(Count(Budget.Outcomes, EModelOutcome::Admitted), 2, std::string(Sh.Name) + " model: admitted");
		LT_EQ(Count(Budget.Outcomes, EModelOutcome::Waiting), 1, "model: waited");
		LT_EQ(Count(Budget.Outcomes, EModelOutcome::Refused), 7, "model: refused at arrival");
		LT_EQ(Budget.ShortWaiters, 0, "model: no short waiter");
		LT_EQ(ModelFor(Sh, 1, EConstruction::Hint1001, 10, true).ShortWaiters, 7, "model mutant: 7 short waiters");
		const FModelResult Old = ModelFor(Sh, 1, EConstruction::Ssb5WithHint, 10, false);
		LT_EQ(Count(Old.Outcomes, EModelOutcome::Admitted), 1, "model SSB5: admitted");
		LT_EQ(Count(Old.Outcomes, EModelOutcome::Refused), 9, "model SSB5: refused");
		LT_EQ(ModelFor(Sh, 1, EConstruction::Ssb5WithHint, 10, true).ShortWaiters, 9, "model SSB5 mutant: 9 short waiters");
	}
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		for (bool bGpu : {false, true})
		{
			for (EConstruction Con : {EConstruction::Hint1001, EConstruction::Hint999, EConstruction::ByteDiffers,
			                          EConstruction::OtherProducer, EConstruction::Ssb5WithHint, EConstruction::HintShares,
			                          EConstruction::Unhinted})
			{
				LT_HOLDS(BurstMatchesModel(Sh, bGpu, 1, Con, ELedgerMutation::None, false), "ledger == probe model");
			}
		}
	}
	// The counting-the-share mutant yields waiters that end short: 7, or 9 for SSB5, as the model's.
	for (EConstruction Con : {EConstruction::Hint1001, EConstruction::ByteDiffers, EConstruction::Ssb5WithHint})
	{
		LT_HOLDS(BurstMatchesModel(GpuShape(), false, 1, Con, ELedgerMutation::ShareCountedAsGuaranteed, true),
		         "the mutant ledger reproduces the model's mutant");
		LT_KILLS(BurstMatchesModel(GpuShape(), false, 1, Con, ELedgerMutation::ShareCountedAsGuaranteed, false),
		         "a hint's share counted as guaranteed");
	}
}

LEDGER_TEST(cell_3_2_e_sharing_burst, "3.2 (e) [K3/K7]: hints that share give the same admission counts, and every apply frees the whole charge (form 1)",
            "stub admits nothing")
{
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		const FBurst L = RunBurst(Sh, false, 1, EConstruction::HintShares, 10, ELedgerMutation::None);
		LT_REQUIRE(L.bOk, L.Why);
		LT_EQ(Count(L.Outcomes, EModelOutcome::Admitted), 2, std::string(Sh.Name) + ": admitted");
		LT_EQ(Count(L.Outcomes, EModelOutcome::Waiting), 1, "waited");
		LT_EQ(Count(L.Outcomes, EModelOutcome::Refused), 7, "refused");
		const uint32_t Charge = TR(Sh.B, Sh.Cap, 512) + TE(Sh.B, Sh.Cap, 512, 1000);
		LT_REQUIRE(L.HAfterEachApply.size() == 3, "three applies");
		// After apply 1: one in flight plus the admitted waiter; after 2: the waiter; after 3: 0.
		LT_EQ(L.HAfterEachApply[0], 2 * Charge, "apply 1 freed its whole charge (and the waiter took one)");
		LT_EQ(L.HAfterEachApply[1], Charge, "apply 2 freed its whole charge");
		LT_EQ(L.HAfterEachApply[2], 0, "apply 3 freed its whole charge: sharing at matching budget holds 0");
	}
}

LEDGER_TEST(cell_4_4_headroom_blocks, "4.4 [K3/K7]: RestoreHeadroomBlocks 0 refuses a hinted matching restore; 1 and 3 blocks match the probe model; in flight <= floor(P_blk k / charge)",
            "stub admits nothing")
{
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		const uint32_t Charge = TR(Sh.B, Sh.Cap, 512) + TE(Sh.B, Sh.Cap, 512, 1000);
		{
			FWorld W(MakeConfig(Sh, 2, 1, 0, 1, false, 0));
			LT_REQUIRE(W.bConfigured, "Configure with 0 headroom blocks");
			const FNodeId N = W.Build(1000);
			LT_EQ(RestoreAdmissionCharge(Sh.B, Sh.Cap, ERestorePayload::Budget, 512, 1000), Charge, "the hinted restore's charge");
			const FAdmission A = W.Ledger.AdmitRestore(HintedRestore(0, 512, 1000, N));
			LT_REQUIRE(A.Outcome == EPageAdmit::KvPagesExhausted, Want("0 headroom refuses on arrival", A.Outcome, EPageAdmit::KvPagesExhausted));
		}
		for (int32_t K : {1, 3})
		{
			const FBurst L = RunBurst(Sh, false, K, EConstruction::HintShares, 10, ELedgerMutation::None);
			const FModelResult Md = ModelFor(Sh, K, EConstruction::HintShares, 10, false);
			LT_REQUIRE(L.bOk, L.Why);
			LT_REQUIRE(L.Outcomes == Md.Outcomes, std::string(Sh.Name) + " x" + std::to_string(K) + ": outcomes differ from the model");
			const uint32_t Bound = TPblk(Sh.B, Sh.Cap) * static_cast<uint32_t>(K) / Charge;
			LT_REQUIRE(L.MaxInFlight <= Bound, WantN("in flight", L.MaxInFlight, Bound));
			LT_EQ(Count(L.Outcomes, EModelOutcome::Admitted), K == 1 ? 2 : 8, "admitted at once: 2 per block at 1, 8 at 3");
		}
	}
}

// Cell 4.4, private holders per H block (form 2; the review's finding on the plan's holder figures).
// A private restore at the executed shape (budget 512, origin 1,000) is charged q and holds
// net = q - R(SequenceBudget) after its apply. The last of n holders is admitted only if
// (n - 1) * net + q <= H_blk, so a block holds floor((H_blk - q) / net) + 1, not floor(H_blk / net):
// 3 at 0.5B on both backends, 33 on the CPU at the 1.5B artifact, 7 in the GPU two-cap
// configuration. Applied one at a time (nothing in flight, so nothing is guaranteed), the next
// restore is refused on arrival; after one holder resets, the same restore is admitted.
static FSoft HoldersScenario(const FShape& Sh, bool bGpu, uint32_t WantHolders, uint32_t WantFree, ELedgerMutation M)
{
	FSoft S;
	const int32_t Slots = static_cast<int32_t>(WantHolders) + 4;
	FWorld W(bGpu ? MakeGpuConfig(Sh, Slots, 1, 1) : MakeConfig(Sh, Slots, 1, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, std::string(Sh.Name) + ": Configure");
	uint32_t Holders = 0;
	int32_t Slot = 0;
	FAdmission Next;
	for (; Slot < Slots; ++Slot)
	{
		FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(Slot, 512, 1000));
		if (J.Adm.Outcome != EPageAdmit::Admitted)
		{
			Next = J.Adm;
			break;
		}
		W.RunRestore(J);
		S.Expect(W.ApplyRestore(J), "apply");
		W.Form1(S, std::string(Sh.Name) + ": holder " + Str(Holders + 1) + " applied");
		++Holders;
	}
	S.Expect(Holders == WantHolders, WantN(std::string(Sh.Name) + ": private holders per H block, floor((H_blk - q) / net) + 1", Holders, WantHolders));
	S.Expect(Next.Outcome == EPageAdmit::KvPagesExhausted,
	         Want(std::string(Sh.Name) + ": the next restore at the executed shape, on arrival", Next.Outcome, EPageAdmit::KvPagesExhausted));
	S.Expect(W.Free(ERegion::H) == WantFree, WantN(std::string(Sh.Name) + ": H free with the holders applied", W.Free(ERegion::H), WantFree));
	S.Expect(W.Reset(0), "one holder resets");
	FWorld::FRestoreJob Again = W.AdmitRestore(BudgetRestore(Slot, 512, 1000));
	S.Expect(Again.Adm.Outcome == EPageAdmit::Admitted, Want(std::string(Sh.Name) + ": the same restore re-issued after the reset", Again.Adm.Outcome, EPageAdmit::Admitted));
	if (Again.Adm.Outcome == EPageAdmit::Admitted)
	{
		W.RunRestore(Again);
		S.Expect(W.ApplyRestore(Again), "apply");
	}
	W.Form1(S, std::string(Sh.Name) + ": end");
	return S;
}

LEDGER_TEST(cell_4_4_private_holders_per_block, "4.4 private holders per block (form 2): 3 / 3 / 33 / 7 at CPU 0.5B, GPU 0.5B, CPU 1.5B and GPU two-cap; the next restore refused on arrival (free 19, 70, 17, 79) and admitted after one reset",
            "the mutant switch HoldSettledOnePageSmaller is not implemented, so the mutant survives (the correct path already holds)")
{
	LT_HOLDS(HoldersScenario(CpuShape(), false, 3, 19, ELedgerMutation::None), "CPU 0.5B: 3 holders (45 of 64)");
	LT_HOLDS(HoldersScenario(GpuShape(), true, 3, 70, ELedgerMutation::None), "GPU 0.5B: 3 holders (186 of 256)");
	LT_HOLDS(HoldersScenario(Cpu15Shape(), false, 33, 17, ELedgerMutation::None), "CPU 1.5B: 33 holders (495 of 512)");
	LT_HOLDS(HoldersScenario(TwoCapShape(), true, 7, 79, ELedgerMutation::None), "GPU two-cap: 7 holders (434 of 513)");
	// The mutant under-counts every hold by a page: form 1 fails at the first apply on every shape,
	// and at the CPU 1.5B shape it also admits two extra restores (35 holders).
	LT_KILLS(HoldersScenario(CpuShape(), false, 3, 19, ELedgerMutation::HoldSettledOnePageSmaller), "CPU 0.5B: a hold settled one page smaller");
	LT_KILLS(HoldersScenario(GpuShape(), true, 3, 70, ELedgerMutation::HoldSettledOnePageSmaller), "GPU 0.5B: a hold settled one page smaller");
	LT_KILLS(HoldersScenario(Cpu15Shape(), false, 33, 17, ELedgerMutation::HoldSettledOnePageSmaller), "CPU 1.5B: a hold settled one page smaller");
	LT_KILLS(HoldersScenario(TwoCapShape(), true, 7, 79, ELedgerMutation::HoldSettledOnePageSmaller), "GPU two-cap: a hold settled one page smaller");
}

LEDGER_TEST(up7_default_burst_about_three_per_block, "UP-7 default: per headroom block, 2 in flight and 1 waiting; the rest refused on arrival, at B = 16 and B = 64, CPU and GPU",
            "stub admits nothing")
{
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		for (bool bGpu : {false, true})
		{
			const FBurst L = RunBurst(Sh, bGpu, 1, EConstruction::Unhinted, 10, ELedgerMutation::None);
			LT_REQUIRE(L.bOk, L.Why);
			LT_EQ(Count(L.Outcomes, EModelOutcome::Admitted), 2, std::string(Sh.Name) + ": in flight");
			LT_EQ(Count(L.Outcomes, EModelOutcome::Waiting), 1, "waiting");
			LT_EQ(Count(L.Outcomes, EModelOutcome::Refused), 7, "refused on arrival");
			LT_EQ(L.WaitersAdmitted, 1, "the waiter is admitted");
			LT_EQ(L.ShortWaiters, 0, "no waiter ends short");
		}
	}
}

LEDGER_TEST(wait_guaranteed_return_capped_by_charge, "§3.5 wait predicate (ambiguity: a restore whose charge is below R(SequenceBudget) guarantees only its charge)",
            "stub admits nothing")
{
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 8, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	// In flight: 95 + 95 + 64 (each guarantees 33) and a 2-page restore (budget 1, origin 0),
	// which can return only its 2. H free is then 0.
	FWorld::FRestoreJob A = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	FWorld::FRestoreJob B = W.AdmitRestore(BudgetRestore(1, 512, 1000));
	FWorld::FRestoreJob C = W.AdmitRestore(BudgetRestore(2, 1, OriginForCharge(Sh, 64)));
	FWorld::FRestoreJob Small = W.AdmitRestore(BudgetRestore(3, 1, 0));
	LT_REQUIRE(A.Adm.Outcome == EPageAdmit::Admitted && B.Adm.Outcome == EPageAdmit::Admitted &&
	               C.Adm.Outcome == EPageAdmit::Admitted && Small.Adm.Outcome == EPageAdmit::Admitted,
	           "four in flight");
	LT_EQ(Small.Adm.ChargedPages, 2, "the small restore's charge");
	LT_EQ(W.Free(ERegion::H), 0, "H free");
	// 0 + 33 + 33 + 33 + min(2, 33) = 101 < 102: no guarantee. Counting the small restore as 33
	// would make it 132 >= 102: a waiter that ends short (the applies return only 101).
	const FAdmission D = W.Ledger.AdmitRestore(BudgetRestore(4, 1, OriginForCharge(Sh, 102)));
	LT_REQUIRE(D.Outcome == EPageAdmit::KvPagesExhausted, Want("a 102-page restore", D.Outcome, EPageAdmit::KvPagesExhausted));
	// 101 >= 101: it waits, and the applies admit it.
	FWorld::FRestoreJob E = W.AdmitRestore(BudgetRestore(4, 1, OriginForCharge(Sh, 101)));
	LT_REQUIRE(E.Adm.Outcome == EPageAdmit::Waiting, Want("a 101-page restore", E.Adm.Outcome, EPageAdmit::Waiting));
	for (FWorld::FRestoreJob* J : {&A, &B, &C, &Small})
	{
		W.RunRestore(*J);
		W.ApplyRestore(*J);
	}
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	LT_REQUIRE(Res.size() == 1 && Res[0].Ticket == E.Adm.Ticket && Res[0].Outcome == EPageAdmit::Admitted,
	           "admitted at the last apply");
}

// Seeded random restore streams with interleaved arrivals, applies and resets: every wait ends in
// admission while configured, no resolution other than Admitted, Form 1 after every step.
static FSoft RandomWaits(uint32_t Seed, ELedgerMutation M, uint32_t* OutShortWaiters)
{
	FSoft S;
	std::mt19937 Rng(Seed);
	const FShape Sh = (Seed % 2) ? GpuShape() : CpuShape();
	const int32_t Slots = 16;
	FWorld W(MakeConfig(Sh, Slots, 2, 1 + static_cast<int32_t>(Seed % 3), 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	const FNodeId Exact = W.Build(1000);
	const FNodeId Off = W.Build(1001);
	std::vector<FWorld::FRestoreJob> Jobs(static_cast<size_t>(Slots));
	std::vector<int> State(static_cast<size_t>(Slots), 0);  // 0 idle, 1 in flight, 2 waiting
	std::map<FTicket, int32_t> WaitingSlot;
	uint32_t Short = 0;
	auto Drain = [&]() {
		for (const FWaitResolution& R : W.Ledger.DrainResolutions())
		{
			auto It = WaitingSlot.find(R.Ticket);
			S.Expect(It != WaitingSlot.end(), "a resolution for an unknown ticket");
			if (It == WaitingSlot.end()) continue;
			S.Expect(R.Outcome == EPageAdmit::Admitted, "a wait resolved other than Admitted while configured");
			State[static_cast<size_t>(It->second)] = R.Outcome == EPageAdmit::Admitted ? 1 : 0;
			WaitingSlot.erase(It);
		}
	};
	for (int Step = 0; Step < 400 && S.bOk; ++Step)
	{
		const int32_t Slot = static_cast<int32_t>(Rng() % Slots);
		const int Op = static_cast<int>(Rng() % 10);
		if (State[static_cast<size_t>(Slot)] == 0 && Op < 5)
		{
			FRestoreRequest R;
			const int Kind = static_cast<int>(Rng() % 6);
			const int64_t Budget = 1 + static_cast<int64_t>(Rng() % static_cast<uint32_t>(Sh.Cap));
			const int64_t Origin = static_cast<int64_t>(Rng() % static_cast<uint32_t>(Sh.Cap + 1));
			if (Kind == 0) R = WholeReserveRestore(Slot);
			else if (Kind == 1) R = BudgetRestore(Slot, Budget, Origin);
			else if (Kind == 2) R = HintedRestore(Slot, Sh.SequenceBudget, 1000, Exact);
			else if (Kind == 3) R = HintedRestore(Slot, Sh.SequenceBudget, 1000, Off);
			else if (Kind == 4) R = HintedRestore(Slot, Budget, 1000, Exact);
			else R = BudgetRestore(Slot, Sh.SequenceBudget, 1000);
			Jobs[static_cast<size_t>(Slot)] = W.AdmitRestore(R);
			const EPageAdmit A = Jobs[static_cast<size_t>(Slot)].Adm.Outcome;
			if (A == EPageAdmit::Admitted) State[static_cast<size_t>(Slot)] = 1;
			if (A == EPageAdmit::Waiting)
			{
				State[static_cast<size_t>(Slot)] = 2;
				WaitingSlot[Jobs[static_cast<size_t>(Slot)].Adm.Ticket] = Slot;
			}
		}
		else if (State[static_cast<size_t>(Slot)] == 1 && Op < 8)
		{
			FWorld::FRestoreJob& J = Jobs[static_cast<size_t>(Slot)];
			if ((Rng() % 20) == 0)
			{
				W.Ledger.ApplyRestore(J.Adm.Ticket, false, 0);  // an engine-refused blob
			}
			else
			{
				W.RunRestore(J, (Rng() % 2) == 0);
				W.ApplyRestore(J);
			}
			State[static_cast<size_t>(Slot)] = 0;
			Drain();
		}
		else if (State[static_cast<size_t>(Slot)] == 0)
		{
			W.Reset(Slot);
			Drain();
		}
		W.Form1(S, "random waits step " + std::to_string(Step));
	}
	// Drain: apply everything in flight until nothing is left.
	for (int Round = 0; Round < 1000; ++Round)
	{
		bool bAny = false;
		for (int32_t Slot = 0; Slot < Slots; ++Slot)
		{
			if (State[static_cast<size_t>(Slot)] != 1) continue;
			bAny = true;
			W.RunRestore(Jobs[static_cast<size_t>(Slot)]);
			W.ApplyRestore(Jobs[static_cast<size_t>(Slot)]);
			State[static_cast<size_t>(Slot)] = 0;
			Drain();
		}
		if (!bAny) break;
	}
	Short = static_cast<uint32_t>(WaitingSlot.size());
	S.Expect(Short == 0, std::to_string(Short) + " waiter(s) never admitted with nothing in flight");
	if (OutShortWaiters) *OutShortWaiters = Short;
	return S;
}

LEDGER_TEST(wait_every_wait_ends_in_admission_random, "§0 'Waits'; §7 'every wait ends in admission while configured'; 3.2 (d) generalized: 60 seeded streams",
            "stub admits nothing, so no stream resolves")
{
	uint32_t Admitting = 0;
	for (uint32_t Seed = 1; Seed <= 60; ++Seed)
	{
		LT_HOLDS(RandomWaits(Seed, ELedgerMutation::None, nullptr), "seed " + std::to_string(Seed));
		++Admitting;
	}
	// The counting-the-share mutant strands a waiter on some seed.
	bool bKilled = false;
	for (uint32_t Seed = 1; Seed <= 60 && !bKilled; ++Seed)
	{
		uint32_t Short = 0;
		const FSoft S = RandomWaits(Seed, ELedgerMutation::ShareCountedAsGuaranteed, &Short);
		bKilled = !S.bOk;
	}
	LT_REQUIRE(bKilled, "mutant survived 60 seeds: a hint's share counted as guaranteed");
	LT_REQUIRE(Admitting == 60, "all seeds ran");
}

LEDGER_TEST(probe_model_random_cross_check, "the probe model ported: seeded bursts of mixed restores, ledger vs generalized model, per arrival",
            "stub admits nothing")
{
	for (uint32_t Seed = 100; Seed < 160; ++Seed)
	{
		std::mt19937 Rng(Seed);
		const FShape Sh = (Seed % 2) ? GpuShape() : CpuShape();
		const int32_t Blocks = 1 + static_cast<int32_t>(Rng() % 3);
		const int Count = 6 + static_cast<int>(Rng() % 10);
		FWorld W(MakeConfig(Sh, Count, 1, Blocks, 1, false, 0));
		LT_REQUIRE(W.bConfigured, "Configure");
		const FNodeId Hint = W.Build(1000);
		std::vector<FModelRestore> Model;
		std::vector<FWorld::FRestoreJob> Jobs;
		std::vector<bool> Bytes;
		for (int I = 0; I < Count; ++I)
		{
			const int Kind = static_cast<int>(Rng() % 4);
			const int64_t Budget = Sh.SequenceBudget + static_cast<int64_t>(Rng() % 1024);  // >= SequenceBudget: c >= G
			FRestoreRequest R = Kind == 0 ? WholeReserveRestore(I)
			                              : (Kind == 1 ? BudgetRestore(I, Budget, 1000) : HintedRestore(I, Budget, 1000, Hint));
			const bool bMatch = Kind == 3;
			FModelRestore M;
			M.Charge = Kind == 0 ? TPblk(Sh.B, Sh.Cap) : TR(Sh.B, Sh.Cap, Budget) + TE(Sh.B, Sh.Cap, Budget, 1000);
			M.SharedDraw = TR(Sh.B, Sh.Cap, Budget);
			M.Share = Kind <= 1 ? EModelShare::Unhinted : (bMatch ? EModelShare::HintShares : EModelShare::HintFails);
			if (Kind == 0) M.Share = EModelShare::Unhinted;
			Model.push_back(M);
			Bytes.push_back(bMatch);
			Jobs.push_back(W.AdmitRestore(R));
		}
		const FModelResult Md = RunRestoreWaitModel(Model, TPblk(Sh.B, Sh.Cap) * static_cast<uint32_t>(Blocks),
		                                            TR(Sh.B, Sh.Cap, Sh.SequenceBudget), false);
		for (int I = 0; I < Count; ++I)
		{
			const EPageAdmit A = Jobs[static_cast<size_t>(I)].Adm.Outcome;
			const EModelOutcome O = A == EPageAdmit::Admitted ? EModelOutcome::Admitted
			                                                  : (A == EPageAdmit::Waiting ? EModelOutcome::Waiting : EModelOutcome::Refused);
			LT_REQUIRE(O == Md.Outcomes[static_cast<size_t>(I)],
			           "seed " + std::to_string(Seed) + " arrival " + std::to_string(I) + ": ledger " + AdmitName(A) + " vs model");
		}
		// FIFO applies, as the model runs them.
		std::deque<int> InFlight;
		std::map<FTicket, int> Waiting;
		for (int I = 0; I < Count; ++I)
		{
			if (Jobs[static_cast<size_t>(I)].Adm.Outcome == EPageAdmit::Admitted) InFlight.push_back(I);
			if (Jobs[static_cast<size_t>(I)].Adm.Outcome == EPageAdmit::Waiting) Waiting[Jobs[static_cast<size_t>(I)].Adm.Ticket] = I;
		}
		uint32_t Admitted = 0;
		while (!InFlight.empty())
		{
			const int I = InFlight.front();
			InFlight.pop_front();
			W.RunRestore(Jobs[static_cast<size_t>(I)], Bytes[static_cast<size_t>(I)]);
			W.ApplyRestore(Jobs[static_cast<size_t>(I)]);
			for (const FWaitResolution& R : W.Ledger.DrainResolutions())
			{
				auto It = Waiting.find(R.Ticket);
				if (It == Waiting.end() || R.Outcome != EPageAdmit::Admitted) continue;
				InFlight.push_back(It->second);
				Waiting.erase(It);
				++Admitted;
			}
		}
		LT_EQ(Admitted, Md.WaitersAdmitted, "seed " + std::to_string(Seed) + ": waiters admitted");
		LT_EQ(Waiting.size(), Md.ShortWaiters, "seed " + std::to_string(Seed) + ": short waiters");
	}
}

// ---- Normalization ----

// Cell 1.8. P is filled and H holds only the SSB5 holder, so the pool has no slack outside H.
// bSqueeze: a 2-page restore in flight leaves H 31 free, below R(SequenceBudget), so the reset
// is in place, and normalization succeeds at the next reset once H has room. Otherwise the reset
// normalizes, and a 33-page restore arriving behind it must be refused: the mutant that does not
// charge the create admits it, and its draw then starves the create (engine exhaustion).
static FSoft NormalizationScenario(ELedgerMutation M, bool bSqueeze)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 6, 1, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(W.Build(4096) != InvalidNode, "P drawn to its bound");
	FWorld::FRestoreJob J = W.AdmitRestore(WholeReserveRestore(0));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "SSB5 into budget mode");
	W.RunRestore(J);
	W.ApplyRestore(J);
	S.Expect(W.Ledger.SlotHeadroomHold(0) == 223, "hold P_blk - R(512)");
	const uint32_t HBefore = W.Charged(ERegion::H);
	FWorld::FRestoreJob Squeeze;
	if (bSqueeze)
	{
		Squeeze = W.AdmitRestore(BudgetRestore(1, 1, 0));
		S.Expect(Squeeze.Adm.Outcome == EPageAdmit::Admitted && W.Free(ERegion::H) == 31, "H free 31");
	}
	FWorld::FResetJob R = W.AdmitReset(0);
	S.Expect(R.Adm.Outcome == EPageAdmit::Admitted, "reset admitted");
	S.Expect(R.Adm.bNormalizes == !bSqueeze, bSqueeze ? "no room: a reset in place" : "a create-first normalization");
	if (!bSqueeze)
	{
		S.Expect(W.Charged(ERegion::H) == HBefore + 33, "the create is charged R(SequenceBudget) on H at admission");
		FWorld::FRestoreJob Late = W.AdmitRestore(BudgetRestore(2, 1, OriginForCharge(Sh, 33)));
		S.Expect(Late.Adm.Outcome == EPageAdmit::KvPagesExhausted,
		         Want("a 33-page restore behind the normalization", Late.Adm.Outcome, EPageAdmit::KvPagesExhausted));
		// Two lanes: the late restore draws, the normalization's create runs, then the restore
		// releases its old holder.
		if (Late.Adm.Outcome == EPageAdmit::Admitted) W.RunRestoreDraw(Late);
		W.RunReset(R);
		W.Form1(S, "normalization run, before its apply");
		if (Late.Adm.Outcome == EPageAdmit::Admitted) W.RunRestoreRelease(Late);
		W.ApplyReset(R);
		if (Late.Adm.Outcome == EPageAdmit::Admitted) W.ApplyRestore(Late);
		S.Expect(W.Ledger.SlotShape(0).Mode == EHolderMode::Budget && W.Ledger.SlotShape(0).Budget == 512, "normalized");
		S.Expect(W.Charged(ERegion::H) == 0, "H returns to its value before the restore");
		S.Expect(W.Pool.DrawnByTag(EPageTag::Seq) == 6 * 33, "the pool model shows R(SequenceBudget) held per slot");
	}
	else
	{
		W.RunReset(R);
		W.ApplyReset(R);
		S.Expect(W.Ledger.SlotShape(0).Mode == EHolderMode::WholeReserve, "the old holder is kept, reset in place");
		S.Expect(W.Ledger.SlotHeadroomHold(0) == 223, "its hold is unchanged");
		W.RunRestore(Squeeze);
		W.ApplyRestore(Squeeze);
		FWorld::FResetJob R2 = W.AdmitReset(0);
		S.Expect(R2.Adm.bNormalizes, "normalization retried at the next reset once H has room");
		W.RunReset(R2);
		W.ApplyReset(R2);
		S.Expect(W.Ledger.SlotShape(0).Mode == EHolderMode::Budget, "normalized on retry");
		S.Expect(W.Charged(ERegion::H) == 0, "H empty");
	}
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(cell_1_8_normalization_create_first, "1.8 [K3]; §3.5 normalization (create-first); §7 'no outcome loses a live sequence'",
            "stub admits nothing")
{
	LT_HOLDS(NormalizationScenario(ELedgerMutation::None, false), "room: normalized, H returns");
	LT_HOLDS(NormalizationScenario(ELedgerMutation::None, true), "H free 31: kept and reset in place, then normalized on retry");
	LT_KILLS(NormalizationScenario(ELedgerMutation::NormalizationUncharged, false), "normalization's create uncharged");
}

// Cell 1.8's yield leg, rebuilt so it can fail (the plan's host-suite item for it; coverage audit G3), on
// the CPU at B = 64 (H = 64). Slot 0 holds a budget-1,024 holder (R = 17, hold 8) that a reset
// would normalize; slot 1 an applied hold of 16; slot 2 a restore in flight (charge 24,
// guarantee 9). H free is 16: room for the create (9). A 24-page waiter fits only on the
// guarantee: 16 + 9 >= 24. The correct build resets slot 0 in place and takes no H, so the
// in-flight apply admits the waiter. The mutant normalizes, takes 9, and leaves the waiter short
// at that apply (7 + 9 < 24).
static FSoft YieldScenario(ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = CpuShape();
	FWorld W(MakeConfig(Sh, 6, 1, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	FWorld::FRestoreJob Big = W.AdmitRestore(BudgetRestore(0, 1024, 0));
	W.RunRestore(Big);
	S.Expect(Big.Adm.Outcome == EPageAdmit::Admitted && W.ApplyRestore(Big), "a budget-1,024 holder in slot 0");
	S.Expect(W.Ledger.SlotHeadroomHold(0) == 8, WantN("its hold R(1024) - R(512)", W.Ledger.SlotHeadroomHold(0), 8));
	S.Expect(FillHeadroom(W, Sh, 16, 1), "an applied hold of 16 in slot 1");
	FWorld::FRestoreJob InFlight = W.AdmitRestore(BudgetRestore(2, 512, 1000));
	S.Expect(InFlight.Adm.Outcome == EPageAdmit::Admitted, "a 24-page restore in flight (guarantee 9)");
	S.Expect(W.Free(ERegion::H) == 16, WantN("H free", W.Free(ERegion::H), 16));
	FWorld::FRestoreJob Waiter = W.AdmitRestore(BudgetRestore(3, 512, 1000));
	S.Expect(Waiter.Adm.Outcome == EPageAdmit::Waiting, Want("a 24-page waiter: 16 + 9 >= 24", Waiter.Adm.Outcome, EPageAdmit::Waiting));
	const uint32_t HBefore = W.Charged(ERegion::H);
	FWorld::FResetJob R = W.AdmitReset(0);
	S.Expect(R.Adm.Outcome == EPageAdmit::Admitted, "the reset is admitted");
	S.Expect(!R.Adm.bNormalizes, "with a restore waiting and room for the create, the reset is in place");
	S.Expect(W.Charged(ERegion::H) == HBefore, WantN("and takes no H", W.Charged(ERegion::H), HBefore));
	W.RunReset(R);
	W.RunRestore(InFlight);
	W.ApplyRestore(InFlight);
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	S.Expect(Res.size() == 1 && Res[0].Ticket == Waiter.Adm.Ticket && Res[0].Outcome == EPageAdmit::Admitted,
	         "the waiter is admitted at the apply its predicate counted on");
	W.ApplyReset(R);
	W.RunRestore(Waiter);
	S.Expect(Waiter.NewHolder >= 0 && W.ApplyRestore(Waiter), "the waiter's restore succeeds");
	W.Form1(S, "end");
	S.Expect(W.Ledger.SlotShape(0).Budget == 1024, "slot 0 kept its holder (normalization retried at its next reset)");
	return S;
}

LEDGER_TEST(normalization_yields_to_waiting_restores, "1.8 'Normalization yields' (host leg, rebuilt so it can fail: free 16, one restore in flight guaranteeing 9, a 24-page waiter); §3.5 Normalization; §7 row 'normalization taking H while a restore waits'",
            "red first against a core without the NormalizationTakesHeadroomWhileWaiting switch: the mutant survived (the correct path already held)")
{
	LT_HOLDS(YieldScenario(ELedgerMutation::None), "the reset is in place and the waiter is admitted");
	LT_KILLS(YieldScenario(ELedgerMutation::NormalizationTakesHeadroomWhileWaiting), "normalization taking H while a restore waits");
}

// Cell 3.2 (f) (coverage audit G2): the guarantee is capped by the charge, on the CPU
// at the 0.5B shape. In flight: a 24-page restore (guarantee 9) and a small one, b = 64 at origin
// 0, charged 2 (guarantee 2, not 9). H free 38. Each waiter runs on its own rebuild (N-a, coverage
// re-audit, N-a): a 49-page waiter queued ahead would make the 50 refused under the mutant
// too.
static FSoft GuaranteeCapScenario(uint32_t WaiterCharge, ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = CpuShape();
	FWorld W(MakeConfig(Sh, 4, 1, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	FWorld::FRestoreJob A = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	FWorld::FRestoreJob Small = W.AdmitRestore(BudgetRestore(1, 64, 0));
	S.Expect(A.Adm.Outcome == EPageAdmit::Admitted && A.Adm.ChargedPages == 24, "a 24-page restore in flight");
	S.Expect(Small.Adm.Outcome == EPageAdmit::Admitted && Small.Adm.ChargedPages == 2, "b = 64 at origin 0, charged 2, in flight");
	S.Expect(W.Free(ERegion::H) == 38, WantN("H free", W.Free(ERegion::H), 38));
	const int64_t Origin = static_cast<int64_t>(WaiterCharge - 9) * Sh.B;  // b = 512: R 9, E = origin / 64
	FWorld::FRestoreJob Wt = W.AdmitRestore(BudgetRestore(2, 512, Origin));
	const EPageAdmit WantA = WaiterCharge <= 49 ? EPageAdmit::Waiting : EPageAdmit::KvPagesExhausted;
	S.Expect(Wt.Adm.Outcome == WantA, Want("a " + Str(WaiterCharge) + "-page restore (b = 512, origin " + Str(Origin) + "): 38 + 9 + 2 = 49", Wt.Adm.Outcome, WantA));
	W.RunRestore(A);
	W.ApplyRestore(A);
	W.RunRestore(Small);
	W.ApplyRestore(Small);
	S.Expect(W.Ledger.SlotHeadroomHold(1) == 0, WantN("the small restore's hold: max(0, 2 - 9)", W.Ledger.SlotHeadroomHold(1), 0));
	W.Form1(S, "after both applies");
	bool bAdmitted = false;
	for (const FWaitResolution& R : W.Ledger.DrainResolutions())
	{
		if (R.Ticket == Wt.Adm.Ticket && R.Outcome == EPageAdmit::Admitted) bAdmitted = true;
	}
	if (Wt.Adm.Outcome == EPageAdmit::Waiting)
	{
		S.Expect(bAdmitted, "the waiter is admitted when both apply (no waiter ends short)");
		if (bAdmitted)
		{
			W.RunRestore(Wt);
			S.Expect(Wt.NewHolder >= 0 && W.ApplyRestore(Wt), "the waiter's restore succeeds");
			W.Form1(S, "end");
		}
	}
	return S;
}

LEDGER_TEST(cell_3_2_f_guarantee_capped_by_charge, "3.2 (f) (coverage audit G2): CPU pair, 49 waits, 50 refused on arrival; each on its own rebuild (N-a); form 1 and a hold of 0 after the small apply (N3)",
            "red first against a core without the FlatGuarantee switch: the mutant survived (the correct path already held)")
{
	LT_HOLDS(GuaranteeCapScenario(49, ELedgerMutation::None), "49 waits and is admitted");
	LT_HOLDS(GuaranteeCapScenario(50, ELedgerMutation::None), "50 is refused on arrival");
	LT_KILLS(GuaranteeCapScenario(50, ELedgerMutation::FlatGuarantee), "the flat R(SequenceBudget) guarantee admits the 50 as a waiter that ends short");
}

// Cell 1.3's early-refund row at the CPU's B = 64 (the plan's host-suite item for B = 64 legs): after a private restore's
// apply (hold 15), H free 14 refuses a 15-page restore and H free 15 admits it.
static FSoft EarlyRefundB64(ELedgerMutation M, uint32_t PreHold)
{
	FSoft S;
	const FShape Sh = CpuShape();
	FWorld W(MakeConfig(Sh, 8, 1, 1, 1, false, 0), M);
	S.Expect(FillHeadroom(W, Sh, PreHold, 2), "fill H");
	FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "the private restore");
	W.RunRestore(J);
	W.ApplyRestore(J);
	const uint32_t Free = W.Free(ERegion::H);
	const FAdmission A = W.Ledger.AdmitRestore(BudgetRestore(1, 1, OriginForCharge(Sh, 15)));
	const EPageAdmit WantA = (64 - PreHold - 15) >= 15 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("a 15-page restore at H free " + Str(Free), A.Outcome, WantA));
	W.Form1(S, "after the apply");
	return S;
}

LEDGER_TEST(cell_1_3_early_refund_boundary_b64, "1.3 row 'H refunded at apply for a private restore' at the CPU's B = 64 (14 / 15)",
            "none: green on arrival (the row's mutant switch exists and the core already settles the hold)")
{
	LT_HOLDS(EarlyRefundB64(ELedgerMutation::None, 35), "H free 14 refuses a 15-page restore");
	LT_HOLDS(EarlyRefundB64(ELedgerMutation::None, 34), "H free 15 admits it");
	LT_KILLS(EarlyRefundB64(ELedgerMutation::PrivateRestoreRefundedAtApply, 35), "H refunded at apply for a private restore");
}

// §3.5 "admitted at the apply that makes it fit": the apply that frees the room need not be a
// restore's. A slot's reset or adopt returns its materialized pages to H, and the waiter must be
// admitted at that apply, not at some later restore apply.
static FSoft WaiterRetestScenario(bool bAdopt)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 6, 1, 1, 1, false, 0));
	S.Expect(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	FWorld::FRestoreJob P = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	W.RunRestore(P);
	W.ApplyRestore(P);
	S.Expect(W.Ledger.SlotHeadroomHold(0) == 62, "slot 0 holds its 62 materialized pages");
	FWorld::FRestoreJob J1 = W.AdmitRestore(BudgetRestore(1, 512, 1000));
	FWorld::FRestoreJob J2 = W.AdmitRestore(BudgetRestore(2, 512, 1000));
	S.Expect(J1.Adm.Outcome == EPageAdmit::Admitted && J2.Adm.Outcome == EPageAdmit::Admitted && W.Free(ERegion::H) == 4,
	         "two in flight, H free 4");
	// 4 + 33 + 33 = 70 >= 40: it waits on the in-flight guarantees.
	FWorld::FRestoreJob Wt = W.AdmitRestore(BudgetRestore(3, 1, OriginForCharge(Sh, 40)));
	S.Expect(Wt.Adm.Outcome == EPageAdmit::Waiting, Want("a 40-page restore", Wt.Adm.Outcome, EPageAdmit::Waiting));
	S.Expect(W.Ledger.DrainResolutions().empty(), "nothing resolved yet");
	// Slot 0's reset (or re-adopt) returns its 62 materialized pages at its apply: H free 66.
	S.Expect(bAdopt ? W.AdoptPrefix(0, N) : W.Reset(0), bAdopt ? "adopt slot 0" : "reset slot 0");
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	S.Expect(Res.size() == 1 && Res[0].Ticket == Wt.Adm.Ticket && Res[0].Outcome == EPageAdmit::Admitted,
	         bAdopt ? "the waiter is admitted at the adopt's apply" : "the waiter is admitted at the reset's apply");
	W.Form1(S, "after the freeing apply");
	for (FWorld::FRestoreJob* J : {&J1, &J2, &Wt})
	{
		W.RunRestore(*J);
		W.ApplyRestore(*J);
	}
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(wait_retested_at_reset_and_adopt_apply, "§3.5 fit and wait ('admitted at the apply that makes it fit'); 3.2 [K3]: a reset or adopt that returns materialized pages admits the waiter at its own apply",
            "a waiter is re-tested only at restore applies")
{
	LT_HOLDS(WaiterRetestScenario(false), "reset");
	LT_HOLDS(WaiterRetestScenario(true), "adopt");
}

// ApplyReset's three outcomes on a normalization. Slot 0 is a private budget-1,024 holder
// (hold 94: R(1,024) = 65 + E 62 - 33); slot 1 shares a 1,000-token prefix at budget 1,024
// (hold 32, a registered dependent). Both are off-shape, so their resets normalize.
static FSoft ResetOutcomeScenario(EResetResult Want0)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 4, 1, 1, 1, false, 0));
	S.Expect(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	FWorld::FRestoreJob P = W.AdmitRestore(BudgetRestore(0, 1024, 1000));
	W.RunRestore(P);
	W.ApplyRestore(P);
	FWorld::FRestoreJob Sd = W.AdmitRestore(HintedRestore(1, 1024, 1000, N));
	W.RunRestore(Sd);
	W.ApplyRestore(Sd);
	S.Expect(W.Ledger.SlotHeadroomHold(0) == 94 && W.Ledger.SlotHeadroomHold(1) == 32 && W.Ledger.DependentCount(N) == 1,
	         "holds 94 and 32; slot 1 depends on the prefix");
	const bool bCreateRefused = Want0 != EResetResult::Normalized;
	const bool bResetRefused = Want0 == EResetResult::Refused;
	for (int32_t Slot : {0, 1})
	{
		const uint32_t HBefore = W.Charged(ERegion::H);
		FWorld::FResetJob R = W.AdmitReset(Slot);
		S.Expect(R.Adm.bNormalizes && W.Charged(ERegion::H) == HBefore + 33, "a create-first normalization, charged 33");
		W.RunReset(R, bCreateRefused, bResetRefused);
		S.Expect(R.Result == Want0, "the job reports the outcome under test");
		S.Expect(W.ApplyReset(R), "apply");
		const FHolderShape Shape = W.Ledger.SlotShape(Slot);
		const uint32_t Hold = W.Ledger.SlotHeadroomHold(Slot);
		switch (Want0)
		{
		case EResetResult::Normalized:
			S.Expect(Shape.Budget == 512 && Shape.Origin == 0 && Hold == 0, "normalized, hold 0");
			break;
		case EResetResult::ResetInPlace:
			// Create refused, the old holder reset as it is: E returns, the dependency goes.
			S.Expect(Shape.Budget == 1024 && Shape.Origin == 0 && Hold == 32, "kept and reset: hold R(1,024) - 33");
			break;
		case EResetResult::Refused:
			// Both refused: nothing ran, nothing returns but the create's charge.
			S.Expect(Shape.Budget == 1024 && Shape.Origin == 1000 && Hold == (Slot == 0 ? 94u : 32u), "unchanged");
			break;
		}
		S.Expect(W.Charged(ERegion::H) == HBefore - (Slot == 0 && Want0 != EResetResult::Refused ? 62u : 0u) -
		                                       (Want0 == EResetResult::Normalized ? 32u : 0u),
		         WantN("H after the apply", W.Charged(ERegion::H), HBefore));
		W.Form1(S, "after the apply");
	}
	S.Expect(W.Ledger.DependentCount(N) == (Want0 == EResetResult::Refused ? 1u : 0u),
	         "the dependency is dropped only by a reset that ran");
	return S;
}

LEDGER_TEST(normalization_apply_outcomes, "§3.5 normalization (create-first): created / create refused and reset in place / both refused; 1.8 [K3]",
            "a single bool cannot tell 'create refused, reset ran' from 'both refused'")
{
	LT_HOLDS(ResetOutcomeScenario(EResetResult::Normalized), "Normalized");
	LT_HOLDS(ResetOutcomeScenario(EResetResult::ResetInPlace), "create refused, reset in place");
	LT_HOLDS(ResetOutcomeScenario(EResetResult::Refused), "both refused");
	// A reset in place cannot report Normalized: refused, and the job stays open.
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FAdmission A = W.Ledger.AdmitReset(0);
	LT_REQUIRE(A.Outcome == EPageAdmit::Admitted && !A.bNormalizes, "a reset in place");
	LT_REQUIRE(!W.Ledger.ApplyReset(A.Ticket, EResetResult::Normalized), "Normalized on an in-place reset is refused");
	LT_REQUIRE(W.Ledger.SlotHasJob(0), "the job stays open");
	LT_REQUIRE(W.Ledger.ApplyReset(A.Ticket, EResetResult::ResetInPlace), "then applies as what it was");
}
