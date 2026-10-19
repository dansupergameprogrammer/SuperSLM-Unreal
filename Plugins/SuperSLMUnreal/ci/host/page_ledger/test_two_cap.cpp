// The GPU's two caps (plugin paging plan §3.2 "Two caps, two block sizes", §3.4 "Two
// caps", UP-15 option (c); §7's [two-cap] legs of cells 1.2, 1.3, 2.1, 4.2, 4.3, 4.4, 4.5 and 5;
// the coverage re-audit's R1-B1, R1-G1, R1-G2 and N-b).
//
// The two-cap configuration is the shipped 1.5B artifact on the GPU: model cap Ce = 32,768 and
// ContextCap Cp = 4,096, B = 16. Every charge is priced at Ce, every bound and limit at Cp, and the
// blocks are P_blk = min(ceil(Cp/B) + 1, ceil(Ce/B)) = 257 (P, A) and
// H_blk = min(P_blk + floor(Cp/B), ceil(Ce/B)) = 513 (H). Figures from the plan's two-cap
// headroom probe and its output.
//
// Must-rejects, one per way of getting UP-15 wrong (each a test-hook mutant in the core):
//   RPricedAtContextCap    R at Cp: S 256 per slot, a 4,095-token prefix charged 256
//   EPricedAtContextCap    E at Cp: a b = 512 restore at origin 4,000 charged 256, not 283
//   BlocksAtContextCap     option (b): P_blk = H_blk = 256
//   BlocksAtEngineCap      option (a)'s blocks: P_blk = H_blk = 2,048 (memory only, but memory is
//                          what option (c) was chosen for)
//   ValidationAtEngineCap  budgets, origins, prefix lengths and SequenceBudget bounded at Ce
//   LimitAtEngineCap       the holder limit at Ce

#include "ledger_test_support.h"
#include "restore_wait_model.h"

using namespace ledgertest;
using namespace SuperSLMPaging;

namespace
{

// A released prefix of exactly Pages full pages kept alive as a ghost by Slot's adopt: it holds P
// pages without holding a count slot.
bool MakeGhost(FWorld& W, int32_t Slot, uint32_t Pages)
{
	const FNodeId N = W.Build(static_cast<int64_t>(Pages) * W.Cfg.PagePositions);
	if (N == InvalidNode) return false;
	if (!W.AdoptPrefix(Slot, N)) return false;
	return W.Release(N);
}

uint64_t CeilDiv(int64_t N, int64_t B) { return static_cast<uint64_t>((N + B - 1) / B); }

}  // namespace

// ---- Arithmetic --------------------------------------------------------------------------------

LEDGER_TEST(two_cap_block_arithmetic, "UP-15 (c) arithmetic; §3.2 'Two caps, two block sizes'; the probe's block rows and its every-admissible-request check",
            "red first against stub PrefixBlockPages and HeadroomBlockPages returning 0")
{
	struct FRow
	{
		int64_t B, Cp, Ce;
		uint32_t P, H;
		const char* Name;
	};
	const FRow Rows[] = {
	    {64, 4096, 4096, 64, 64, "CPU 0.5B"},
	    {64, 32768, 32768, 512, 512, "CPU 1.5B"},
	    {16, 4096, 4096, 256, 256, "GPU 0.5B, ContextCap 4,096"},
	    {16, 4096, 32768, 257, 513, "GPU 1.5B, ContextCap 4,096 (two-cap)"},
	    {16, 2048, 4096, 129, 256, "GPU 0.5B, ContextCap 2,048"},
	};
	for (const FRow& R : Rows)
	{
		LT_EQ(PrefixBlockPages(R.B, R.Cp, R.Ce), R.P, std::string(R.Name) + ": P_blk");
		LT_EQ(HeadroomBlockPages(R.B, R.Cp, R.Ce), R.H, std::string(R.Name) + ": H_blk");
	}
	LT_EQ(PrefixBlockPages(16, 4096, 2048), 0, "an engine cap below the plugin's cap is invalid");
	LT_EQ(HeadroomBlockPages(0, 4096, 32768), 0, "B < 1 is invalid");
	// The audit's B1 rows, priced at the engine's cap with the existing one-cap functions.
	LT_EQ(RestoreAdmissionCharge(16, 32768, ERestorePayload::Budget, 512, 4000), 283, "b = 512 at origin 4,000: 33 + 250");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 512, 4000), 256, "the same priced at ContextCap (the under-charge)");
	LT_EQ(ReservationPages(16, 32768, 4096), 257, "R(SequenceBudget = ContextCap) at the engine's cap");
	LT_EQ(RestoreAdmissionCharge(16, 32768, ERestorePayload::Budget, 4096, 4096), 513, "the largest admissible charge");
	LT_EQ(RestoreAdmissionCharge(16, 32768, ERestorePayload::Budget, 512, 4095), 288, "b = 512 at origin 4,095");
	// Every admissible request fits one block under (c); under (b) the probe's 530,432 page-granular
	// (b, o) pairs and 16 prefix lengths never fit (and 134,144 / 16 at ContextCap 2,048).
	struct FStuck
	{
		int64_t Cp, Ce;
		long long StuckB, PfxB;
	};
	for (const FStuck& Sx : {FStuck{4096, 32768, 530432, 16}, FStuck{2048, 4096, 134144, 16}})
	{
		const uint32_t PBlk = PrefixBlockPages(16, Sx.Cp, Sx.Ce);
		const uint32_t HBlk = HeadroomBlockPages(16, Sx.Cp, Sx.Ce);
		const uint64_t BlkB = CeilDiv(Sx.Cp, 16);
		long long StuckC = 0, StuckOptB = 0, PfxC = 0, PfxOptB = 0;
		for (int64_t Bud = 1; Bud <= Sx.Cp; ++Bud)
		{
			for (int64_t O = 0; O <= Sx.Cp; O += 16)
			{
				const uint32_t Q = RestoreAdmissionCharge(16, Sx.Ce, ERestorePayload::Budget, Bud, O);
				if (Q > HBlk) ++StuckC;
				if (Q > BlkB) ++StuckOptB;
			}
		}
		for (int64_t L = 0; L <= Sx.Cp; ++L)
		{
			const uint32_t Q = PrefixBuildCharge(16, Sx.Ce, L);
			if (Q > PBlk) ++PfxC;
			if (Q > BlkB) ++PfxOptB;
		}
		LT_EQ(StuckC, 0, "Cp " + Str(Sx.Cp) + ": restores that never fit an H block under (c)");
		LT_EQ(PfxC, 0, "Cp " + Str(Sx.Cp) + ": prefixes that never fit a P block under (c)");
		LT_EQ(StuckOptB, Sx.StuckB, "Cp " + Str(Sx.Cp) + ": must-reject: option (b)'s stuck (b, o) pairs");
		LT_EQ(PfxOptB, Sx.PfxB, "Cp " + Str(Sx.Cp) + ": must-reject: option (b)'s stuck prefix lengths");
	}
}

// ---- Configure (R1-G2; cell 5) -----------------------------------------------------------------

static FSoft TwoCapConfigureScenario(ELedgerMutation M)
{
	FSoft S;
	{
		FPageLedger L;
		L.SetMutationForTest(M);
		const FPageLedgerConfig C = MakeGpuConfig(TwoCapShape(), 50, 1, 1);
		S.Expect(L.Configure(C) == ELedgerConfigure::Ok, "Configure, two caps");
		S.Expect(L.RegionCapacity(ERegion::S) == 1650, WantN("S = 50 x R(512)", L.RegionCapacity(ERegion::S), 1650));
		S.Expect(L.RegionCapacity(ERegion::P) == 257, WantN("P = 1 x P_blk", L.RegionCapacity(ERegion::P), 257));
		S.Expect(L.RegionCapacity(ERegion::H) == 513, WantN("H = 1 x H_blk", L.RegionCapacity(ERegion::H), 513));
		S.Expect(L.PoolPages() == 2420, WantN("the pool (529.4 MiB at 224 KiB pages)", L.PoolPages(), 2420));
		S.Expect(L.PoolPages() == TestPoolPages(C), WantN("the pool against the test's own figure", L.PoolPages(), TestPoolPages(C)));
	}
	{
		FPageLedger L;
		L.SetMutationForTest(M);
		const FPageLedgerConfig C = MakeConfig(TwoCapShape(), 50, 1, 1, 1, true, 0);
		S.Expect(L.Configure(C) == ELedgerConfigure::Ok, "Configure, two caps, with a cache");
		S.Expect(L.RegionCapacity(ERegion::A) == 257, WantN("A = 1 x P_blk", L.RegionCapacity(ERegion::A), 257));
	}
	{
		FPageLedger L;
		L.SetMutationForTest(M);
		const FPageLedgerConfig C = MakeGpuConfig(TwoCapHalfShape(), 50, 1, 1);
		S.Expect(L.Configure(C) == ELedgerConfigure::Ok, "Configure, 0.5B at ContextCap 2,048");
		S.Expect(L.RegionCapacity(ERegion::P) == 129 && L.RegionCapacity(ERegion::H) == 256,
		         "P_blk 129, H_blk 256 at ContextCap 2,048 (P " + Str(L.RegionCapacity(ERegion::P)) + ", H " +
		             Str(L.RegionCapacity(ERegion::H)) + ")");
		S.Expect(L.PoolPages() == 2035, WantN("the pool", L.PoolPages(), 2035));
	}
	return S;
}

LEDGER_TEST(two_cap_configure_regions, "R1-G2 (coverage re-audit): [two-cap] Configure at the 50-NPC shape, S 1,650 / P 257 / H 513, pool 2,420; §3.2; 5",
            "the core ignores EngineContextCap: it sizes P 256 and H 256, option (b)'s pool of 2,162")
{
	LT_HOLDS(TwoCapConfigureScenario(ELedgerMutation::None), "UP-15 (c) regions");
	LT_KILLS(TwoCapConfigureScenario(ELedgerMutation::BlocksAtEngineCap), "option (a)'s blocks: P_blk = 2,048 (an oversized P/A block)");
	LT_KILLS(TwoCapConfigureScenario(ELedgerMutation::BlocksAtContextCap), "option (b)'s blocks: 256");
}

LEDGER_TEST(two_cap_configure_engine_cap_field, "UP-15: the engine-cap input; 0 means 'equal to ContextCap' (the CPU); below ContextCap is refused",
            "the core ignores EngineContextCap, so a cap below ContextCap is accepted")
{
	FPageLedgerConfig C = MakeGpuConfig(GpuShape(), 4, 1, 1);
	FPageLedger A;
	LT_REQUIRE(A.Configure(C) == ELedgerConfigure::Ok, "EngineContextCap 0");
	C.EngineContextCap = 4096;
	FPageLedger B;
	LT_REQUIRE(B.Configure(C) == ELedgerConfigure::Ok, "EngineContextCap = ContextCap");
	for (int R = 0; R < static_cast<int>(ERegion::Count); ++R)
	{
		LT_EQ(A.RegionCapacity(static_cast<ERegion>(R)), B.RegionCapacity(static_cast<ERegion>(R)), "0 and an equal cap size the same regions");
	}
	LT_EQ(B.PoolPages(), 4 * 33 + 256 + 256, "one cap: blocks of ceil(cap / B)");
	C.EngineContextCap = 4095;
	FPageLedger L;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidGeometry, "an engine cap below ContextCap is InvalidGeometry");
	LT_REQUIRE(!L.IsConfigured(), "and leaves no ledger");
}

// ---- 1.2 [two-cap]: zero slack (form 3) --------------------------------------------------------

// Kind 1: warm creates at SequenceBudget = ContextCap (R = 257, not 256). The pool is sized from
// the ledger, as the subsystem's pool create is, and the only region is S.
static FSoft TwoCapWarmCreates(ELedgerMutation M)
{
	FSoft S;
	FShape Sh = TwoCapShape();
	Sh.SequenceBudget = 4096;
	FWorld W(MakeGpuConfig(Sh, 4, 0, 0), M, FWorld::ePoolFromLedger);
	S.Expect(W.bConfigured, "Configure at SequenceBudget = ContextCap");
	S.Expect(W.Ledger.RegionCapacity(ERegion::S) == 4 * 257, WantN("S = 4 x R(4,096) at the engine's cap", W.Ledger.RegionCapacity(ERegion::S), 4 * 257));
	for (int32_t I = 0; I < 4; ++I) S.Expect(W.SlotHolder[static_cast<size_t>(I)] >= 0, "warm create " + Str(I) + " draws 257");
	W.Form1(S, "after the warm creates");
	return S;
}

// Kind 2: a prefix of ContextCap - 1 tokens (R = 257) with P exactly its charge, graded through the
// build's in-flight window (the job has begun and drawn its peak; it has not frozen or applied).
static FSoft TwoCapPrefixZeroSlack(ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 1, 1, 0), M, FWorld::ePoolFromLedger);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(W.Free(ERegion::P) == 257, WantN("P free", W.Free(ERegion::P), 257));
	const FAdmission A = W.Ledger.AdmitPrefixBuild(4095);
	S.Expect(A.Outcome == EPageAdmit::Admitted, Want("a 4,095-token prefix", A.Outcome, EPageAdmit::Admitted));
	if (A.Outcome != EPageAdmit::Admitted) return S;
	S.Expect(A.ChargedPages == 257, WantN("charged R(4,095) at the engine's cap", A.ChargedPages, 257));
	const int H = W.Pool.PrefixBegin(4095, EPageTag::Prefix);
	S.Expect(H >= 0, "the begin draws its peak");
	W.Form1(S, "in flight, at the peak draw");
	W.Pool.Freeze(H);
	W.NodeHolder[A.Node] = H;
	S.Expect(W.Ledger.ApplyPrefixBuild(A.Ticket, true), "apply");
	S.Expect(W.Charged(ERegion::P) == 256, WantN("settles to ceil(4,095 / 16)", W.Charged(ERegion::P), 256));
	W.Form1(S, "quiescent");
	S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), WantN("quiescent: ledger equals the pool model", W.Ledger.TotalCharged(), W.Pool.Drawn()));
	return S;
}

LEDGER_TEST(two_cap_cell_1_2_zero_slack, "1.2 [two-cap] (form 3): warm creates at SequenceBudget = ContextCap, and a ContextCap - 1 prefix; R1-B1's in-flight alternative",
            "the core prices R at ContextCap: S is 4 x 256 and the fourth warm create exhausts the pool; the prefix is charged 256 against a 257-page draw")
{
	LT_HOLDS(TwoCapWarmCreates(ELedgerMutation::None), "warm creates fit S exactly");
	LT_HOLDS(TwoCapPrefixZeroSlack(ELedgerMutation::None), "the 4,095-token prefix fits P exactly");
	LT_KILLS(TwoCapWarmCreates(ELedgerMutation::RPricedAtContextCap), "R at ContextCap: S under-sized, a warm create exhausts");
	LT_KILLS(TwoCapPrefixZeroSlack(ELedgerMutation::RPricedAtContextCap), "R at ContextCap: charge 256 < drawn 257 in flight");
	LT_KILLS(TwoCapPrefixZeroSlack(ELedgerMutation::BlocksAtContextCap), "option (b): P_blk 256 refuses it");
}

// ---- 1.3 [two-cap] rows (form 2) ---------------------------------------------------------------

// R1-B1: a prefix of ContextCap - 1 tokens with P free 256 (refused) or 257 (admitted). Ghosts
// place P exactly while freeing the count slots.
static FSoft TwoCapPrefixBoundary(uint32_t FreeWanted, ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 2, 2, 1), M);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(MakeGhost(W, 0, 256), "ghost filler of 256 pages");
	S.Expect(MakeGhost(W, 1, 514 - 256 - FreeWanted), "ghost filler to the boundary");
	S.Expect(W.Free(ERegion::P) == FreeWanted, WantN("P free", W.Free(ERegion::P), FreeWanted));
	const FAdmission A = W.Ledger.AdmitPrefixBuild(4095);
	const EPageAdmit WantA = FreeWanted >= 257 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("a 4,095-token prefix (R = 257) at P free " + Str(FreeWanted), A.Outcome, WantA));
	return S;
}

LEDGER_TEST(two_cap_cell_1_3_prefix_r_boundary, "1.3 [two-cap] row 'R priced at ContextCap' (R1-B1, coverage re-audit): P free 256 refuses, 257 admits",
            "the core sizes P 256 per block (ceil(ContextCap / B)): the ghosts cannot place the boundary, and the prefix is charged 256")
{
	LT_HOLDS(TwoCapPrefixBoundary(256, ELedgerMutation::None), "P free 256 refuses the 257-page prefix");
	LT_HOLDS(TwoCapPrefixBoundary(257, ELedgerMutation::None), "P free 257 admits it");
	LT_KILLS(TwoCapPrefixBoundary(256, ELedgerMutation::RPricedAtContextCap), "R at ContextCap: the prefix charged 256 is admitted");
}

// N-b: H free 282 of 513 is placed by an applied private hold of 231 (b = 512, origin 3,696:
// E = 231, draw 264, hold 264 - 33). Nothing is in flight, so nothing is guaranteed and the
// 283-page restore is refused on arrival, not made to wait.
static FSoft TwoCapEBoundary(int64_t FillerOrigin, ELedgerMutation M, bool bWantAdmit)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 4, 1, 1), M);
	S.Expect(W.bConfigured, "Configure");
	FWorld::FRestoreJob F = W.AdmitRestore(BudgetRestore(1, 512, FillerOrigin));
	S.Expect(F.Adm.Outcome == EPageAdmit::Admitted, "the private filler restore");
	W.RunRestore(F);
	S.Expect(W.ApplyRestore(F), "the filler applies: its hold is settled, nothing is in flight");
	const uint32_t Free = W.Free(ERegion::H);
	FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, 512, 4000));
	const EPageAdmit WantA = bWantAdmit ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(J.Adm.Outcome == WantA, Want("b = 512 at origin 4,000 (charge 283) at H free " + Str(Free), J.Adm.Outcome, WantA));
	if (J.Adm.Outcome == EPageAdmit::Admitted)
	{
		S.Expect(J.Adm.ChargedPages == 283, WantN("charged R(512) + E at the engine's cap", J.Adm.ChargedPages, 283));
		W.RunRestoreDraw(J);
		W.Form1(S, "the boundary restore drawn");
		W.RunRestoreRelease(J);
		S.Expect(W.ApplyRestore(J), "apply");
		W.Form1(S, "after the apply");
	}
	return S;
}

LEDGER_TEST(two_cap_cell_1_3_e_boundary, "1.3 [two-cap] row 'E priced at ContextCap' (form 2): 282 refuses, 283 admits; N-b (an applied private hold of 231)",
            "the core prices E at ContextCap (charge 256, not 283) and sizes H 256, so the filler and the boundary differ")
{
	LT_HOLDS(TwoCapEBoundary(3696, ELedgerMutation::None, false), "H free 282 refuses on arrival");
	LT_HOLDS(TwoCapEBoundary(3680, ELedgerMutation::None, true), "H free 283 admits, and the engine draw fits");
	LT_KILLS(TwoCapEBoundary(3696, ELedgerMutation::EPricedAtContextCap, false), "E at ContextCap: charged 256, admitted");
}

// ---- 2.1, 2.6 and 5 [two-cap]: the plugin's cap bounds every input --------------------------------
//
// A restore's budget or origin above the engine's cap is malformed (the ledger's InvalidRequest,
// the subsystem's Malformed). Above ContextCap and within the engine's cap it is a valid save taken
// under a larger ContextCap: ContextCapMismatch (plan §3.5, §3.6). A prefix length above ContextCap
// is InvalidRequest (the subsystem's Malformed): there is no save to be valid. Nothing is charged
// for any of them.

static FSoft TwoCapOriginBound(ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 4, 2, 1), M);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t Before = W.Ledger.TotalCharged();
	FAdmission A = W.Ledger.AdmitRestore(BudgetRestore(0, 512, 4500));
	S.Expect(A.Outcome == EPageAdmit::ContextCapMismatch,
	         Want("origin 4,500: above ContextCap, within the model's cap", A.Outcome, EPageAdmit::ContextCapMismatch));
	A = W.Ledger.AdmitRestore(BudgetRestore(0, 512, 40000));
	S.Expect(A.Outcome == EPageAdmit::InvalidRequest, Want("origin 40,000: above the model's cap (Malformed)", A.Outcome, EPageAdmit::InvalidRequest));
	S.Expect(W.Ledger.TotalCharged() == Before && !W.Ledger.SlotHasJob(0), "nothing charged, the slot untouched");
	A = W.Ledger.AdmitRestore(BudgetRestore(0, 512, 4096));
	S.Expect(A.Outcome == EPageAdmit::Admitted, Want("origin = ContextCap is admissible", A.Outcome, EPageAdmit::Admitted));
	return S;
}

LEDGER_TEST(cell_2_1_two_cap_origin_bound, "2.1 [two-cap]: an SLM6 origin of 4,500 -> ContextCapMismatch; 40,000 -> Malformed (the ledger's InvalidRequest); nothing charged",
            "the core reports every field above ContextCap InvalidRequest: it has no ContextCapMismatch outcome")
{
	LT_HOLDS(TwoCapOriginBound(ELedgerMutation::None), "the two caps tell a mismatch from a corrupt save");
	LT_KILLS(TwoCapOriginBound(ELedgerMutation::ValidationAtEngineCap), "validation at the model's cap admits origin 4,500");
	LT_KILLS(TwoCapOriginBound(ELedgerMutation::ContextCapMismatchReportedAsInvalid), "the mismatch reported Malformed");
}

static FSoft TwoCapBudgetBound(ELedgerMutation M)
{
	FSoft S;
	{
		FPageLedger L;
		L.SetMutationForTest(M);
		FPageLedgerConfig C = MakeGpuConfig(TwoCapShape(), 4, 2, 1);
		C.SequenceBudget = 4097;
		S.Expect(L.Configure(C) == ELedgerConfigure::InvalidSequenceBudget, "5: SequenceBudget = ContextCap + 1 (the model's cap would admit it)");
		C.SequenceBudget = 4096;
		S.Expect(L.Configure(C) == ELedgerConfigure::Ok, "5: SequenceBudget = ContextCap");
		S.Expect(L.RegionCapacity(ERegion::S) == 4 * 257, WantN("S at R(4,096) = 257", L.RegionCapacity(ERegion::S), 4 * 257));
	}
	FWorld W(MakeGpuConfig(TwoCapShape(), 4, 2, 1), M);
	S.Expect(W.bConfigured, "Configure, PrefixBlockCount 2");
	const uint32_t Before = W.Ledger.TotalCharged();
	FAdmission A = W.Ledger.AdmitRestore(BudgetRestore(1, 4097, 0));
	S.Expect(A.Outcome == EPageAdmit::ContextCapMismatch, Want("an SLM6 budget of 4,097", A.Outcome, EPageAdmit::ContextCapMismatch));
	A = W.Ledger.AdmitRestore(BudgetRestore(1, 40000, 0));
	S.Expect(A.Outcome == EPageAdmit::InvalidRequest, Want("a budget of 40,000, above the model's cap", A.Outcome, EPageAdmit::InvalidRequest));
	S.Expect(W.Ledger.TotalCharged() == Before, "nothing charged");
	return S;
}

LEDGER_TEST(cell_2_6_two_cap_budget_bound, "2.6 [two-cap] and 5 [two-cap]: an SLM6 budget of 4,097 -> ContextCapMismatch; above the model's cap -> Malformed; SequenceBudget ContextCap + 1 -> InvalidSequenceBudget",
            "the core reports a budget above ContextCap InvalidRequest: it has no ContextCapMismatch outcome")
{
	LT_HOLDS(TwoCapBudgetBound(ELedgerMutation::None), "budgets bounded at ContextCap");
	LT_KILLS(TwoCapBudgetBound(ELedgerMutation::ValidationAtEngineCap), "validation at the model's cap");
	LT_KILLS(TwoCapBudgetBound(ELedgerMutation::ContextCapMismatchReportedAsInvalid), "the mismatch reported Malformed");
}

static FSoft TwoCapPrefixLengthBound(ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 4, 2, 1), M);
	S.Expect(W.bConfigured, "Configure, PrefixBlockCount 2");
	const uint32_t Before = W.Ledger.TotalCharged();
	FAdmission A = W.Ledger.AdmitPrefixBuild(4097);
	S.Expect(A.Outcome == EPageAdmit::InvalidRequest, Want("a prefix of ContextCap + 1 tokens (R = 258 would fit P = 514)", A.Outcome, EPageAdmit::InvalidRequest));
	S.Expect(W.Ledger.TotalCharged() == Before, "nothing charged");
	const FNodeId Parent = W.Build(4000);
	S.Expect(Parent != InvalidNode, "a 4,000-token parent");
	const uint32_t Mid = W.Ledger.TotalCharged();
	A = W.Ledger.AdmitNestedPrefixBuild(Parent, 200);
	S.Expect(A.Outcome == EPageAdmit::InvalidRequest, Want("parent + child = 4,200, past ContextCap", A.Outcome, EPageAdmit::InvalidRequest));
	S.Expect(W.Ledger.TotalCharged() == Mid, "nothing charged");
	A = W.Ledger.AdmitNestedPrefixBuild(Parent, 96);
	S.Expect(A.Outcome == EPageAdmit::Admitted, Want("parent + child = ContextCap is admissible", A.Outcome, EPageAdmit::Admitted));
	return S;
}

LEDGER_TEST(cell_2_6_two_cap_prefix_length_bound, "2.6 [two-cap]: CreatePrefix with ContextCap + 1 tokens and CreatePrefixFrom past ContextCap -> Malformed (the ledger's InvalidRequest), at PrefixBlockCount 2",
            "none: green on arrival (the core already bounds prefix lengths at ContextCap)")
{
	LT_HOLDS(TwoCapPrefixLengthBound(ELedgerMutation::None), "prefix lengths bounded at ContextCap");
	LT_KILLS(TwoCapPrefixLengthBound(ELedgerMutation::ValidationAtEngineCap), "the bound at the model's cap admits a 4,097-token prefix");
}

// Budget 0 is outside the budget domain [1, cap] (plan §3.5; the review's lesser case 3), in an
// SSB6 (the CPU, one cap) and an SLM6 (the GPU, two caps). Origin 0, so that the mutant's charge is
// exactly 0.
static FSoft BudgetZero(const FShape& Sh, bool bGpu, ELedgerMutation M)
{
	FSoft S;
	FWorld W(bGpu ? MakeGpuConfig(Sh, 4, 1, 1) : MakeConfig(Sh, 4, 1, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t Before = W.Ledger.TotalCharged();
	const FAdmission A = W.Ledger.AdmitRestore(BudgetRestore(0, 0, 0));
	S.Expect(A.Outcome == EPageAdmit::InvalidRequest, Want(std::string(Sh.Name) + ": a budget of 0", A.Outcome, EPageAdmit::InvalidRequest));
	S.Expect(W.Ledger.TotalCharged() == Before && !W.Ledger.SlotHasJob(0), "nothing charged, the slot untouched");
	return S;
}

LEDGER_TEST(cell_2_1_budget_zero, "2.1: a budget of 0 in an SSB6 (CPU) and an SLM6 (GPU, two caps) header -> Malformed (the ledger's InvalidRequest); the budget domain is [1, cap]",
            "the mutant switch BudgetDomainFromZero is not implemented, so the mutant survives (the correct path already holds)")
{
	LT_HOLDS(BudgetZero(CpuShape(), false, ELedgerMutation::None), "SSB6");
	LT_HOLDS(BudgetZero(TwoCapShape(), true, ELedgerMutation::None), "SLM6");
	LT_KILLS(BudgetZero(CpuShape(), false, ELedgerMutation::BudgetDomainFromZero), "SSB6: the [0, cap] check passes budget 0 to a zero charge");
	LT_KILLS(BudgetZero(TwoCapShape(), true, ELedgerMutation::BudgetDomainFromZero), "SLM6: the [0, cap] check");
}

// ---- 4.2 [two-cap]: the limit is ContextCap's --------------------------------------------------

static FSoft TwoCapLimit(ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 3, 1, 1), M);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(W.Ledger.SlotLimit(2) == 512, WantN("a fresh holder: min(0 + 512, 4,096)", W.Ledger.SlotLimit(2), 512));
	FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, 512, 4000));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, Want("b = 512 at origin 4,000", J.Adm.Outcome, EPageAdmit::Admitted));
	W.RunRestore(J);
	S.Expect(W.ApplyRestore(J), "apply");
	const int64_t Limit = W.Ledger.SlotLimit(0);
	S.Expect(Limit == 4096, WantN("origin + b = 4,512 is capped at ContextCap", Limit, 4096));
	S.Expect(CheckStep(4096, false, Limit, 4096) == ELimitVerdict::ContextCapExceeded, "the next embed at ContextCap is the cap outcome");
	S.Expect(CheckStep(4095, false, Limit, 4096) == ELimitVerdict::Allowed, "one below is allowed: from 4,000 ready, 97 tokens");
	const FNodeId N = W.Build(4000);
	S.Expect(N != InvalidNode && W.AdoptPrefix(1, N), "an adopter of a 4,000-token prefix");
	S.Expect(W.Ledger.SlotLimit(1) == 4096, WantN("the adopter's limit", W.Ledger.SlotLimit(1), 4096));
	return S;
}

LEDGER_TEST(two_cap_cell_4_2_limit_at_context_cap, "4.2 [two-cap]: a holder whose origin + b exceeds ContextCap stops at ContextCap with the cap outcome; §3.4 'Two caps'",
            "red first against a stub SlotLimit returning 0")
{
	LT_HOLDS(TwoCapLimit(ELedgerMutation::None), "limit at ContextCap");
	LT_KILLS(TwoCapLimit(ELedgerMutation::LimitAtEngineCap), "the limit at the model's cap emits past ContextCap");
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure, one cap");
	FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	W.RunRestore(J);
	LT_REQUIRE(W.ApplyRestore(J), "apply");
	LT_EQ(W.Ledger.SlotLimit(0), 1512, "one cap: min(origin + b, cap)");
	LT_EQ(W.Ledger.SlotLimit(7), 0, "an unknown slot");
}

// ---- 4.3 [two-cap]: the cap - 1 prefix at PrefixBlockCount 1 -----------------------------------

static FSoft TwoCapCapMinusOnePrefix(ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeGpuConfig(TwoCapShape(), 1, 1, 1), M);
	S.Expect(W.bConfigured, "Configure, PrefixBlockCount 1");
	FWorld::FBuild Bd = W.AdmitRoot(4095);
	S.Expect(Bd.Adm.Outcome == EPageAdmit::Admitted, Want("a 4,095-token prefix (R = 257)", Bd.Adm.Outcome, EPageAdmit::Admitted));
	if (Bd.Adm.Outcome != EPageAdmit::Admitted) return S;
	S.Expect(Bd.Adm.ChargedPages == 257, WantN("charged R(4,095) at the engine's cap", Bd.Adm.ChargedPages, 257));
	Bd.Holder = W.Pool.PrefixBegin(4095, EPageTag::Prefix);
	W.Form1(S, "begun: the peak draw of 257 in flight");
	if (Bd.Holder >= 0) W.Pool.Freeze(Bd.Holder);
	W.Form1(S, "frozen, before the apply");
	S.Expect(W.ApplyBuild(Bd), "apply");
	S.Expect(W.Ledger.NodeChargedPages(Bd.Adm.Node) == 256, WantN("settles to 256", W.Ledger.NodeChargedPages(Bd.Adm.Node), 256));
	S.Expect(W.AdoptPrefix(0, Bd.Adm.Node), "adopted");
	W.Form1(S, "adopted");
	return S;
}

LEDGER_TEST(two_cap_cell_4_3_cap_minus_one_prefix, "4.3 [two-cap]: the cap - 1 prefix (4,095 tokens, R = 257) is admitted at PrefixBlockCount = 1",
            "the core prices R at ContextCap: the prefix is charged 256 against its 257-page peak draw")
{
	LT_HOLDS(TwoCapCapMinusOnePrefix(ELedgerMutation::None), "P_blk 257 admits it");
	LT_KILLS(TwoCapCapMinusOnePrefix(ELedgerMutation::BlocksAtContextCap), "option (b): a P block of 256 refuses it");
}

// ---- 4.4 [two-cap]: the burst against H_blk 513 ------------------------------------------------

struct FTwoCapBurst
{
	std::vector<EModelOutcome> Outcomes;
	uint32_t MaxInFlight = 0;
	uint32_t ShortWaiters = 0;
	FSoft S;
};

static FTwoCapBurst TwoCapBurst(int32_t Blocks, ELedgerMutation M)
{
	FTwoCapBurst Out;
	FWorld W(MakeGpuConfig(TwoCapShape(), 10, 1, Blocks), M);
	Out.S.Expect(W.bConfigured, "Configure");
	const FNodeId Hint = W.Build(1000);
	Out.S.Expect(Hint != InvalidNode, "the hint prefix");
	std::vector<FWorld::FRestoreJob> Jobs;
	for (int32_t I = 0; I < 10; ++I)
	{
		Jobs.push_back(W.AdmitRestore(HintedRestore(I, 512, 1000, Hint)));
		const EPageAdmit A = Jobs.back().Adm.Outcome;
		Out.Outcomes.push_back(A == EPageAdmit::Admitted ? EModelOutcome::Admitted
		                                                 : (A == EPageAdmit::Waiting ? EModelOutcome::Waiting : EModelOutcome::Refused));
	}
	Out.MaxInFlight = W.Ledger.GetStats().InFlightRestores;
	std::vector<size_t> Queue;
	std::map<FTicket, size_t> Waiting;
	for (size_t I = 0; I < Jobs.size(); ++I)
	{
		if (Jobs[I].Adm.Outcome == EPageAdmit::Admitted) Queue.push_back(I);
		if (Jobs[I].Adm.Outcome == EPageAdmit::Waiting) Waiting[Jobs[I].Adm.Ticket] = I;
	}
	for (size_t Q = 0; Q < Queue.size(); ++Q)
	{
		FWorld::FRestoreJob& J = Jobs[Queue[Q]];
		W.RunRestore(J);
		W.ApplyRestore(J);
		for (const FWaitResolution& R : W.Ledger.DrainResolutions())
		{
			auto It = Waiting.find(R.Ticket);
			if (It == Waiting.end()) continue;
			if (R.Outcome == EPageAdmit::Admitted) Queue.push_back(It->second);
			Waiting.erase(It);
		}
		const uint32_t Fl = W.Ledger.GetStats().InFlightRestores;
		if (Fl > Out.MaxInFlight) Out.MaxInFlight = Fl;
		W.Form1(Out.S, "burst apply");
	}
	Out.ShortWaiters = static_cast<uint32_t>(Waiting.size());
	return Out;
}

static uint32_t CountOf(const std::vector<EModelOutcome>& V, EModelOutcome O)
{
	uint32_t N = 0;
	for (EModelOutcome X : V) N += X == O ? 1u : 0u;
	return N;
}

static FSoft TwoCapBurstMatches(int32_t Blocks, ELedgerMutation M)
{
	const FTwoCapBurst L = TwoCapBurst(Blocks, M);
	FSoft S = L.S;
	FModelRestore R;
	R.Charge = 95;
	R.SharedDraw = 33;
	R.Share = EModelShare::HintShares;
	const uint32_t H = 513u * static_cast<uint32_t>(Blocks);
	const FModelResult Md = RunRestoreWaitModel(std::vector<FModelRestore>(10, R), H, 33, false);
	S.Expect(L.Outcomes == Md.Outcomes, "x" + Str(Blocks) + ": outcomes differ from the probe model at H = " + Str(H));
	S.Expect(L.MaxInFlight <= H / 95, WantN("in flight <= floor(513 k / 95)", L.MaxInFlight, H / 95));
	S.Expect(L.ShortWaiters == 0, "no waiter ends short");
	if (Blocks == 1)
	{
		S.Expect(CountOf(L.Outcomes, EModelOutcome::Admitted) == 5 && CountOf(L.Outcomes, EModelOutcome::Waiting) == 2 &&
		             CountOf(L.Outcomes, EModelOutcome::Refused) == 3,
		         "one block: 5 admitted, 2 waiting, 3 refused (got " + Str(CountOf(L.Outcomes, EModelOutcome::Admitted)) + "/" +
		             Str(CountOf(L.Outcomes, EModelOutcome::Waiting)) + "/" + Str(CountOf(L.Outcomes, EModelOutcome::Refused)) + ")");
	}
	else
	{
		S.Expect(CountOf(L.Outcomes, EModelOutcome::Admitted) == 10, "three blocks (at most 16 in flight): all 10 admitted");
	}
	return S;
}

LEDGER_TEST(two_cap_cell_4_4_headroom_burst, "4.4 [two-cap]: H_blk 513; a burst of 10 hinted restores, 5 / 2 / 3 at one block; in flight <= floor(513 k / 95) (5 or 16)",
            "the core sizes H at 256: 2 admitted, 1 waiting, 7 refused")
{
	LT_HOLDS(TwoCapBurstMatches(1, ELedgerMutation::None), "one block");
	LT_HOLDS(TwoCapBurstMatches(3, ELedgerMutation::None), "three blocks");
	LT_KILLS(TwoCapBurstMatches(1, ELedgerMutation::BlocksAtContextCap), "option (b): H block 256 (2 / 1 / 7)");
	LT_KILLS(TwoCapBurstMatches(1, ELedgerMutation::BlocksAtEngineCap), "option (a)'s H block 2,048 (10 / 0 / 0)");
}

// ---- 4.5 [two-cap]: the largest admissible restores fit an empty H ------------------------------

static FSoft TwoCapLargestRestores(ELedgerMutation M)
{
	FSoft S;
	struct FCase
	{
		int64_t Budget, Origin;
		uint32_t Charge;
	};
	for (const FCase& C : {FCase{512, 4095, 288}, FCase{4096, 4096, 513}})
	{
		FWorld W(MakeGpuConfig(TwoCapShape(), 2, 1, 1), M);
		S.Expect(W.bConfigured, "Configure, RestoreHeadroomBlocks 1");
		FWorld::FRestoreJob J = W.AdmitRestore(BudgetRestore(0, C.Budget, C.Origin));
		const std::string What = "b = " + Str(C.Budget) + " at origin " + Str(C.Origin);
		S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, Want(What + " into an empty H", J.Adm.Outcome, EPageAdmit::Admitted));
		if (J.Adm.Outcome != EPageAdmit::Admitted) continue;
		S.Expect(J.Adm.ChargedPages == C.Charge, WantN(What + ": charge", J.Adm.ChargedPages, C.Charge));
		W.RunRestoreDraw(J);
		W.Form1(S, What + ": drawn");
		W.RunRestoreRelease(J);
		S.Expect(J.NewHolder >= 0 && W.ApplyRestore(J), What + ": succeeds");
		W.Form1(S, What + ": applied");
	}
	return S;
}

LEDGER_TEST(two_cap_cell_4_5_largest_restores, "4.5 [two-cap]: origin 4,095 with b = 512 (charge 288), and origin and budget 4,096 (charge 513), each admitted into an empty H at one block",
            "the core prices at ContextCap (charges 256 and 256) and the engine model then draws past the charge")
{
	LT_HOLDS(TwoCapLargestRestores(ELedgerMutation::None), "every admissible restore fits one H block");
	LT_KILLS(TwoCapLargestRestores(ELedgerMutation::BlocksAtContextCap), "option (b)'s H block of 256 refuses both, every time");
}
