// Region P (prefixes, nested children, ghosts) and region A (cache-entry pre-charges). Plugin
// paging plan §3.2 sharer registry and cache entries with sharers, §3.3, UP-6, UP-14; cells 1.3
// (prefix, nested, restored-sharer and pre-charge rows), 1.7, 2.5, 3.1, 5, 8.3, 8.4.

#include "ledger_test_support.h"

using namespace ledgertest;
using namespace SuperSLMPaging;

// A released prefix of exactly Pages full pages (Pages * B positions, no tail) kept alive as a
// ghost by Slot's adopt. It holds P pages without holding a count slot.
static bool MakeGhost(FWorld& W, int32_t Slot, uint32_t Pages)
{
	const FNodeId N = W.Build(static_cast<int64_t>(Pages) * W.Cfg.PagePositions);
	if (N == InvalidNode) return false;
	if (!W.AdoptPrefix(Slot, N)) return false;
	return W.Release(N);
}

LEDGER_TEST(prefix_charge_settles_at_freeze, "§3.3 prefixes; §3.2 charge rule; 1.2 prefix leg (charge at peak, settle at apply)",
            "stub admits nothing (AdmitPrefixBuild returns NotConfigured)")
{
	FWorld W(MakeConfig(GpuShape(), 2, 2, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	FWorld::FBuild Bd = W.AdmitRoot(1000);
	LT_REQUIRE(Bd.Adm.Outcome == EPageAdmit::Admitted, Want("admit a 1,000-token prefix", Bd.Adm.Outcome, EPageAdmit::Admitted));
	LT_EQ(Bd.Adm.ChargedPages, 64, "charged R(1000) at admission");
	LT_EQ(W.Charged(ERegion::P), 64, "P holds the peak charge before the job runs");
	LT_REQUIRE(W.Ledger.IsPrefixLive(Bd.Adm.Node), "the node is live (building) from admission");
	LT_EQ(W.Ledger.LivePrefixCount(), 1, "a building prefix holds a count slot");
	W.RunBuild(Bd, InvalidNode, 1000);
	{
		FSoft S;
		W.Form1(S, "after the build ran, before its apply");
		LT_REQUIRE(S.bOk, S.First);
	}
	LT_REQUIRE(W.ApplyBuild(Bd), "apply");
	LT_EQ(W.Charged(ERegion::P), 63, "settles to ceil(1000/16) at freeze");
	LT_EQ(W.Ledger.NodeChargedPages(Bd.Adm.Node), 63, "the node's own charge");
	// The empty prefix: begun with budget 1, charged R(1) = 2, settles to 0.
	FWorld::FBuild E = W.AdmitRoot(0);
	LT_REQUIRE(E.Adm.Outcome == EPageAdmit::Admitted, Want("admit an empty prefix", E.Adm.Outcome, EPageAdmit::Admitted));
	LT_EQ(W.Charged(ERegion::P), 63 + 2, "an empty prefix is charged R(1) = 2");
	W.RunBuild(E, InvalidNode, 0);
	LT_REQUIRE(W.ApplyBuild(E), "apply the empty build");
	LT_EQ(W.Charged(ERegion::P), 63, "and settles to 0");
	FSoft S;
	W.Form1(S, "quiescent");
	LT_REQUIRE(S.bOk, S.First);
	LT_EQ(W.Ledger.TotalCharged(), W.Pool.Drawn(), "quiescent: the ledger equals the pool model exactly");
}

LEDGER_TEST(prefix_count_and_page_refusal_up6, "UP-6; 5 KvPagesExhausted (prefix) [K2/K7]; 11 fit check",
            "stub admits nothing")
{
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	LT_REQUIRE(N != InvalidNode, "build");
	FAdmission A = W.Ledger.AdmitPrefixBuild(16);
	LT_REQUIRE(A.Outcome == EPageAdmit::PrefixLimit, Want("a second prefix past PrefixBlockCount = 1", A.Outcome, EPageAdmit::PrefixLimit));
	LT_REQUIRE(W.AdoptPrefix(0, N), "adopt");
	LT_REQUIRE(W.Release(N), "release with a dependent");
	LT_REQUIRE(W.Ledger.IsGhost(N), "a ghost while its adopter lives");
	LT_EQ(W.Ledger.LivePrefixCount(), 0, "release frees the count slot");
	LT_EQ(W.Charged(ERegion::P), 63, "the ghost keeps its whole charge");
	A = W.Ledger.AdmitPrefixBuild(3088);  // R = 194 > 193 free
	LT_REQUIRE(A.Outcome == EPageAdmit::KvPagesExhausted,
	           Want("count admits, pages refuse (UP-6)", A.Outcome, EPageAdmit::KvPagesExhausted));
	LT_EQ(W.Charged(ERegion::P), 63, "a refusal charges nothing");
	A = W.Ledger.AdmitPrefixBuild(3072);  // R = 193: exactly fits
	LT_REQUIRE(A.Outcome == EPageAdmit::Admitted, Want("exactly the free pages", A.Outcome, EPageAdmit::Admitted));
	LT_EQ(W.Free(ERegion::P), 0, "P exactly full");
}

// 1.3, row "prefix charged ceil(L/B) for R(L)": P free = R(L) - 1 refuses; a 1-page under-charge
// admits; P free = R(L) admits.
static FSoft PrefixBoundary(uint32_t FreeWanted, uint32_t UnderCharge)
{
	FSoft S;
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0));
	W.Ledger.SetForceUnderChargeForTest(EChargeKind::PrefixBuild, UnderCharge);
	S.Expect(W.bConfigured, "Configure");
	// The ghost's own build is charged under the same seam; it must still fit.
	S.Expect(MakeGhost(W, 0, 256 - FreeWanted), "ghost filler");
	S.Expect(W.Free(ERegion::P) == FreeWanted, WantN("P free", W.Free(ERegion::P), FreeWanted));
	const FAdmission A = W.Ledger.AdmitPrefixBuild(1000);
	S.Expect(A.Outcome == (FreeWanted >= 64 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted),
	         Want("boundary build of R(1000) = 64", A.Outcome, FreeWanted >= 64 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted));
	return S;
}

LEDGER_TEST(cell_1_3_prefix_undercharge_boundary, "1.3 row 'prefix charged ceil(L/B) for R(L)' (form 2) [K2]",
            "stub admits nothing, so neither the filler nor the boundary leg resolves")
{
	LT_HOLDS(PrefixBoundary(63, 0), "correct build at P free = R(L) - 1 refuses");
	LT_HOLDS(PrefixBoundary(64, 0), "second leg at P free = R(L) admits");
	LT_KILLS(PrefixBoundary(63, 1), "ForceUnderCharge(PrefixBuild, 1)");
}

// 1.3, row "nested charged 0": P free = R(child) - 1.
static FSoft NestedBoundary(uint32_t FreeWanted, uint32_t UnderCharge)
{
	FSoft S;
	FWorld W(MakeConfig(GpuShape(), 3, 2, 1, 1, false, 0));
	S.Expect(W.bConfigured, "Configure");
	const FNodeId Parent = W.Build(1000);
	S.Expect(Parent != InvalidNode, "parent");
	const uint32_t Fill = 512 - 63 - FreeWanted;
	S.Expect(MakeGhost(W, 0, 256), "ghost filler 1");
	S.Expect(MakeGhost(W, 1, Fill - 256), "ghost filler 2");
	S.Expect(W.Free(ERegion::P) == FreeWanted, WantN("P free", W.Free(ERegion::P), FreeWanted));
	W.Ledger.SetForceUnderChargeForTest(EChargeKind::NestedBuild, UnderCharge);
	const FAdmission A = W.Ledger.AdmitNestedPrefixBuild(Parent, 200);
	const EPageAdmit WantA = FreeWanted >= 14 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("nested build of R(200) = 14", A.Outcome, WantA));
	return S;
}

LEDGER_TEST(cell_1_3_nested_undercharge_boundary, "1.3 row 'nested charged 0' (form 2) [K3/K7]",
            "stub admits nothing")
{
	LT_HOLDS(NestedBoundary(13, 0), "correct build at P free = R(child) - 1 refuses");
	LT_HOLDS(NestedBoundary(14, 0), "second leg at P free = R(child) admits");
	LT_KILLS(NestedBoundary(13, 14), "ForceUnderCharge(NestedBuild, R(child)): nested charged 0");
}

LEDGER_TEST(nested_charge_and_settle, "§3.3 nested prefixes; 10.3 host leg (child holds only its private pages) [K3/K7]",
            "stub admits nothing")
{
	FWorld W(MakeConfig(GpuShape(), 2, 3, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId Parent = W.Build(1000);
	LT_REQUIRE(Parent != InvalidNode, "parent");
	FWorld::FBuild Bd = W.AdmitChild(Parent, 200);
	LT_REQUIRE(Bd.Adm.Outcome == EPageAdmit::Admitted, Want("child", Bd.Adm.Outcome, EPageAdmit::Admitted));
	LT_EQ(W.Charged(ERegion::P), 63 + 14, "child charged R(200) = 14 at admission");
	W.RunBuild(Bd, Parent, 200);
	LT_REQUIRE(W.ApplyBuild(Bd), "apply");
	LT_EQ(W.Ledger.NodeChargedPages(Bd.Adm.Node), 13, "child settles to its 13 private pages");
	LT_EQ(W.Ledger.DependentCount(Parent), 1, "the child is its parent's dependent");
	LT_EQ(W.Ledger.LivePrefixCount(), 2, "a child counts against PrefixBlockCount");
	FWorld::FBuild Empty = W.AdmitChild(Parent, 0);
	LT_REQUIRE(Empty.Adm.Outcome == EPageAdmit::Admitted, Want("empty child", Empty.Adm.Outcome, EPageAdmit::Admitted));
	LT_EQ(Empty.Adm.ChargedPages, 2, "an empty child is begun with budget 1: R(1) = 2");
	W.RunBuild(Empty, Parent, 0);
	LT_REQUIRE(W.ApplyBuild(Empty), "apply the empty child");
	LT_EQ(W.Ledger.NodeChargedPages(Empty.Adm.Node), 1, "an empty child of a mid-page parent keeps the copied tail page");
	FSoft S;
	W.Form1(S, "quiescent");
	LT_REQUIRE(S.bOk, S.First);
	LT_EQ(W.Ledger.TotalCharged(), W.Pool.Drawn(), "quiescent equality");
}

// Cell 4.3's empty nested child (coverage audit N2; the plan's host-suite item for B = 64 legs): on parents of length B
// and B + 1 it settles to 0 and 1 private pages, as the pool model draws, at both backends' B.
static FSoft EmptyChildScenario(const FShape& Sh, int64_t ParentLen, ELedgerMutation M)
{
	FSoft S;
	FWorld W(MakeConfig(Sh, 2, 2, 1, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	const FNodeId Parent = W.Build(ParentLen);
	S.Expect(Parent != InvalidNode, "the parent");
	FWorld::FBuild Bd = W.AdmitChild(Parent, 0);
	S.Expect(Bd.Adm.Outcome == EPageAdmit::Admitted, Want("an empty child", Bd.Adm.Outcome, EPageAdmit::Admitted));
	if (Bd.Adm.Outcome != EPageAdmit::Admitted) return S;
	S.Expect(Bd.Adm.ChargedPages == TR(Sh.B, Sh.Cap, 1), WantN("charged R(1)", Bd.Adm.ChargedPages, TR(Sh.B, Sh.Cap, 1)));
	W.RunBuild(Bd, Parent, 0);
	W.ApplyBuild(Bd);
	const uint32_t Want0 = (ParentLen % Sh.B) != 0 ? 1u : 0u;
	S.Expect(W.Ledger.NodeChargedPages(Bd.Adm.Node) == Want0,
	         WantN(std::string(Sh.Name) + ", parent " + Str(ParentLen) + ": the empty child's settled pages", W.Ledger.NodeChargedPages(Bd.Adm.Node), Want0));
	W.Form1(S, std::string(Sh.Name) + ", parent " + Str(ParentLen) + ": quiescent");
	S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), WantN("the ledger equals the pool model", W.Ledger.TotalCharged(), W.Pool.Drawn()));
	return S;
}

LEDGER_TEST(cell_4_3_empty_child_on_b_and_b_plus_1, "4.3 host leg (coverage audit N2): an empty nested child on parents of length B and B + 1 settles to 0 and 1 private pages, at the GPU's B = 16 and the CPU's B = 64 (form 1)",
            "red first against a core without the EmptyChildSettledToZero switch: the mutant survived (the correct path already held)")
{
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		LT_HOLDS(EmptyChildScenario(Sh, Sh.B, ELedgerMutation::None), std::string(Sh.Name) + ": parent of length B");
		LT_HOLDS(EmptyChildScenario(Sh, Sh.B + 1, ELedgerMutation::None), std::string(Sh.Name) + ": parent of length B + 1");
		LT_KILLS(EmptyChildScenario(Sh, Sh.B + 1, ELedgerMutation::EmptyChildSettledToZero), std::string(Sh.Name) + ": an empty child settled to 0 (charge < drawn at B + 1)");
	}
}

LEDGER_TEST(cell_2_5_nested_parent_not_usable, "2.5 [K3/K7]: released, unfrozen or foreign parent -> ParentNotUsable",
            "stub answers NotConfigured")
{
	FWorld W(MakeConfig(GpuShape(), 2, 4, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	FAdmission A = W.Ledger.AdmitNestedPrefixBuild(987654321, 10);
	LT_REQUIRE(A.Outcome == EPageAdmit::ParentNotUsable, Want("a foreign (unknown) parent", A.Outcome, EPageAdmit::ParentNotUsable));
	FWorld::FBuild Building = W.AdmitRoot(100);
	LT_REQUIRE(Building.Adm.Outcome == EPageAdmit::Admitted, "a parent still building");
	A = W.Ledger.AdmitNestedPrefixBuild(Building.Adm.Node, 10);
	LT_REQUIRE(A.Outcome == EPageAdmit::ParentNotUsable, Want("an unfrozen parent", A.Outcome, EPageAdmit::ParentNotUsable));
	const FNodeId Released = W.Build(100);
	LT_REQUIRE(Released != InvalidNode && W.Release(Released), "build and release");
	A = W.Ledger.AdmitNestedPrefixBuild(Released, 10);
	LT_REQUIRE(A.Outcome == EPageAdmit::ParentNotUsable, Want("a released parent", A.Outcome, EPageAdmit::ParentNotUsable));
	const FNodeId Ghost = W.Build(160);
	LT_REQUIRE(Ghost != InvalidNode && W.AdoptPrefix(0, Ghost) && W.Release(Ghost), "a ghost");
	A = W.Ledger.AdmitNestedPrefixBuild(Ghost, 10);
	LT_REQUIRE(A.Outcome == EPageAdmit::ParentNotUsable, Want("a ghost parent", A.Outcome, EPageAdmit::ParentNotUsable));
	const FNodeId Live = W.Build(100);
	A = W.Ledger.AdmitNestedPrefixBuild(Live, 4000);
	LT_REQUIRE(A.Outcome == EPageAdmit::InvalidRequest, Want("parent + child past the cap", A.Outcome, EPageAdmit::InvalidRequest));
}

LEDGER_TEST(cell_3_1_release_pinned_while_queued, "3.1 [K2]; §3.3 concurrency (adopt, begin_from, hinted restore incl. a waiting one)",
            "stub answers NotConfigured for every admission")
{
	FWorld W(MakeConfig(GpuShape(), 6, 3, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	LT_REQUIRE(N != InvalidNode, "build");
	// A queued adopt pins.
	FAdmission Ad = W.Ledger.AdmitAdoptPrefix(0, N);
	LT_REQUIRE(Ad.Outcome == EPageAdmit::Admitted, "adopt admitted");
	FAdmission R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::PrefixPinned, Want("release while an adopt is queued", R.Outcome, EPageAdmit::PrefixPinned));
	W.Pool.Adopt(W.SlotHolder[0], W.NodeHolder[N]);
	LT_REQUIRE(W.Ledger.ApplyAdopt(Ad.Ticket, true), "apply the adopt");
	// A queued begin_from pins.
	FWorld::FBuild Child = W.AdmitChild(N, 50);
	LT_REQUIRE(Child.Adm.Outcome == EPageAdmit::Admitted, "child admitted");
	R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::PrefixPinned, Want("release while a begin_from is queued", R.Outcome, EPageAdmit::PrefixPinned));
	W.RunBuild(Child, N, 50);
	LT_REQUIRE(W.ApplyBuild(Child), "apply the child");
	// A queued hinted restore pins.
	FWorld::FRestoreJob J = W.AdmitRestore(HintedRestore(1, 512, 1000, N));
	LT_REQUIRE(J.Adm.Outcome == EPageAdmit::Admitted, "hinted restore admitted");
	R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::PrefixPinned, Want("release while a hinted restore is queued", R.Outcome, EPageAdmit::PrefixPinned));
	// A second in flight, and a third waiting on the guarantee, also hinted on N.
	FWorld::FRestoreJob J2 = W.AdmitRestore(HintedRestore(2, 512, 1000, N));
	FWorld::FRestoreJob J3 = W.AdmitRestore(HintedRestore(3, 512, 1000, N));
	LT_REQUIRE(J2.Adm.Outcome == EPageAdmit::Admitted && J3.Adm.Outcome == EPageAdmit::Waiting, "two in flight and one waiting");
	W.RunRestore(J);
	LT_REQUIRE(W.ApplyRestore(J), "apply the first");
	W.RunRestore(J2);
	LT_REQUIRE(W.ApplyRestore(J2), "apply the second");
	LT_REQUIRE(W.Ledger.GetStats().WaitingRestores == 0, "the sharing applies admit the waiter");
	R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::PrefixPinned, Want("release while the admitted waiter is queued", R.Outcome, EPageAdmit::PrefixPinned));
	W.RunRestore(J3);
	LT_REQUIRE(W.ApplyRestore(J3), "apply the third");
	// Nothing queued: the release succeeds.
	R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::Admitted, Want("release after every apply", R.Outcome, EPageAdmit::Admitted));
}

LEDGER_TEST(cell_3_1_release_pinned_by_waiting_restore, "3.1 [K2/K3]: a hinted restore still waiting pins its prefix",
            "stub answers NotConfigured")
{
	FWorld W(MakeConfig(GpuShape(), 6, 2, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId Other = W.Build(1001);
	const FNodeId N = W.Build(1000);
	LT_REQUIRE(Other != InvalidNode && N != InvalidNode, "build");
	FWorld::FRestoreJob A1 = W.AdmitRestore(HintedRestore(0, 512, 1000, Other));
	FWorld::FRestoreJob A2 = W.AdmitRestore(HintedRestore(1, 512, 1000, Other));
	FWorld::FRestoreJob Wt = W.AdmitRestore(HintedRestore(2, 512, 1000, N));
	LT_REQUIRE(A1.Adm.Outcome == EPageAdmit::Admitted && A2.Adm.Outcome == EPageAdmit::Admitted, "two in flight");
	LT_REQUIRE(Wt.Adm.Outcome == EPageAdmit::Waiting, Want("the third waits", Wt.Adm.Outcome, EPageAdmit::Waiting));
	const FAdmission R = W.Ledger.AdmitPrefixRelease(N);
	LT_REQUIRE(R.Outcome == EPageAdmit::PrefixPinned, Want("release while a hinted restore waits", R.Outcome, EPageAdmit::PrefixPinned));
}

// Cell 1.7: the overcount (P charge minus prefix pages drawn) is 0 <= overcount <= ghosts at
// every step, and 0 after the last dependent resets.
static void GhostStep(FWorld& W, FSoft& S, const std::string& At)
{
	W.Form1(S, At);
	const long long Over = static_cast<long long>(W.Charged(ERegion::P)) - W.Pool.DrawnByTag(EPageTag::Prefix);
	S.Expect(Over >= 0, At + ": overcount " + std::to_string(Over) + " < 0");
	S.Expect(Over <= static_cast<long long>(W.Ledger.GhostCount()),
	         At + ": overcount " + std::to_string(Over) + " > ghosts " + std::to_string(W.Ledger.GhostCount()));
}

static FSoft GhostScenario(ELedgerMutation M)
{
	FSoft S;
	// (a) A released prefix with 3 adopters.
	{
		FWorld W(MakeConfig(GpuShape(), 4, 1, 1, 1, false, 0), M);
		S.Expect(W.bConfigured, "Configure");
		const FNodeId N = W.Build(1000);
		for (int32_t Slot = 0; Slot < 3; ++Slot) S.Expect(W.AdoptPrefix(Slot, N), "adopt");
		GhostStep(W, S, "(a) adopted");
		S.Expect(W.Release(N), "release");
		S.Expect(W.Ledger.IsGhost(N), "(a) ghost");
		GhostStep(W, S, "(a) released");
		for (int32_t Slot = 0; Slot < 3; ++Slot)
		{
			S.Expect(W.Reset(Slot), "reset");
			GhostStep(W, S, "(a) after reset " + std::to_string(Slot));
		}
		S.Expect(W.Charged(ERegion::P) == 0 && W.Ledger.GhostCount() == 0, "(a) the ghost drains at the last dependent");
	}
	// (b) begin_from children after their parent's release.
	{
		FWorld W(MakeConfig(GpuShape(), 2, 3, 1, 1, false, 0), M);
		const FNodeId P = W.Build(1000);
		const FNodeId C1 = W.Build(100, P);
		const FNodeId C2 = W.Build(300, P);
		S.Expect(P && C1 && C2, "(b) build parent and two children");
		S.Expect(W.Release(P), "(b) release the parent");
		GhostStep(W, S, "(b) parent released");
		S.Expect(W.Release(C1), "(b) release child 1");
		GhostStep(W, S, "(b) child 1 released");
		S.Expect(W.Ledger.IsGhost(P), "(b) the parent stays a ghost while child 2 lives");
		S.Expect(W.Release(C2), "(b) release child 2");
		GhostStep(W, S, "(b) child 2 released");
		S.Expect(W.Charged(ERegion::P) == 0, "(b) everything returns");
	}
	// (c) Sequences restored with sharing.
	{
		FWorld W(MakeConfig(GpuShape(), 4, 1, 2, 1, false, 0), M);
		const FNodeId N = W.Build(1000);
		for (int32_t Slot = 0; Slot < 2; ++Slot)
		{
			FWorld::FRestoreJob J = W.AdmitRestore(HintedRestore(Slot, 512, 1000, N));
			S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "(c) restore admitted");
			W.RunRestore(J);
			S.Expect(J.Shared == 62, "(c) the pool model shares 62");
			W.ApplyRestore(J);
		}
		S.Expect(W.Ledger.DependentCount(N) == 2, "(c) two restored sharers registered");
		S.Expect(W.Release(N), "(c) release");
		GhostStep(W, S, "(c) released");
		S.Expect(W.Reset(0), "(c) reset 0");
		GhostStep(W, S, "(c) reset 0");
		S.Expect(W.Reset(1), "(c) reset 1");
		GhostStep(W, S, "(c) reset 1");
		S.Expect(W.Charged(ERegion::P) == 0, "(c) drained");
	}
	// (d) A grandchild holding its grandparent.
	{
		FWorld W(MakeConfig(GpuShape(), 2, 3, 1, 1, false, 0), M);
		const FNodeId G = W.Build(1000);
		const FNodeId C = W.Build(100, G);
		const FNodeId GC = W.Build(100, C);
		S.Expect(G && C && GC, "(d) three generations");
		S.Expect(W.AdoptPrefix(0, GC), "(d) adopt the grandchild");
		S.Expect(W.Release(G) && W.Release(C) && W.Release(GC), "(d) release all three");
		S.Expect(W.Ledger.GhostCount() == 3, "(d) three ghosts");
		GhostStep(W, S, "(d) all released");
		S.Expect(W.Reset(0), "(d) reset the adopter");
		GhostStep(W, S, "(d) adopter reset");
		S.Expect(W.Charged(ERegion::P) == 0 && W.Ledger.GhostCount() == 0, "(d) the cascade drains");
	}
	return S;
}

LEDGER_TEST(cell_1_7_ghosts, "1.7 [K3] (form 1, four cases); §3.2 sharer registry; §7 'a ghost lasts until the last dependent'",
            "stub admits nothing")
{
	LT_HOLDS(GhostScenario(ELedgerMutation::None), "ghost overcount bounds");
	LT_KILLS(GhostScenario(ELedgerMutation::GhostReleasedAtFirstDependent), "ghost released at the first dependent");
}

// 1.3, row "restored sharer not registered": release the prefix, then P free = its ghost - 1.
static FSoft RestoredSharerBoundary(ELedgerMutation M, int64_t NewLength)
{
	FSoft S;
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0), M);
	const FNodeId N = W.Build(1000);
	FWorld::FRestoreJob J = W.AdmitRestore(HintedRestore(0, 512, 1000, N));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "restore admitted");
	W.RunRestore(J);
	S.Expect(J.Shared == 62, "shares");
	W.ApplyRestore(J);
	S.Expect(W.Release(N), "release");
	S.Expect(W.Free(ERegion::P) == 193, WantN("P free with the ghost held", W.Free(ERegion::P), 193));
	const FAdmission A = W.Ledger.AdmitPrefixBuild(NewLength);
	const EPageAdmit WantA = TR(16, 4096, NewLength) <= 193 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(A.Outcome == WantA, Want("a new prefix at the boundary", A.Outcome, WantA));
	return S;
}

LEDGER_TEST(cell_1_3_restored_sharer_not_registered_boundary, "1.3 row 'restored sharer not registered' (form 2) [K3]",
            "stub admits nothing")
{
	LT_HOLDS(RestoredSharerBoundary(ELedgerMutation::None, 3088), "R = 194 at P free 193 refuses");
	LT_HOLDS(RestoredSharerBoundary(ELedgerMutation::None, 3072), "R = 193 at P free 193 admits");
	LT_KILLS(RestoredSharerBoundary(ELedgerMutation::RestoredSharerNotRegistered, 3088), "restored sharer not registered");
}

// ---- Region A ----

static FPageLedgerConfig CacheConfig(int32_t BlockCount, uint32_t CPages)
{
	return MakeConfig(GpuShape(), BlockCount, 1, 1, 1, true, CPages);
}

// 8.4 / UP-14, and 1.3 row "A pre-charge skipped": A holds X's pre-charge; the first adopt of
// a new entry Y refuses at A free = floor(L/B) - 1 and admits at A free = floor(L/B).
static FSoft AFullScenario(ELedgerMutation M, int64_t XLength)
{
	FSoft S;
	FWorld W(CacheConfig(4, 1024), M);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(W.BuildEntry(1, XLength), "entry X in C");
	S.Expect(W.BuildEntry(2, 1000), "entry Y in C");
	S.Expect(W.AdoptEntry(0, 1) == EPageAdmit::Admitted, "first adopt of X");
	const uint32_t XFull = TFull(16, XLength);
	S.Expect(W.Charged(ERegion::A) == XFull, WantN("A holds X's floor(L/B)", W.Charged(ERegion::A), XFull));
	const uint32_t FreeA = W.Free(ERegion::A);
	const FShape Sh = GpuShape();
	const FHolderShape Before = W.Ledger.SlotShape(1);
	const EPageAdmit Y = W.AdoptEntry(1, 2);
	const EPageAdmit WantY = FreeA >= 62 ? EPageAdmit::Admitted : EPageAdmit::KvPagesExhausted;
	S.Expect(Y == WantY, Want("first adopt of Y at A free " + std::to_string(FreeA), Y, WantY));
	if (WantY == EPageAdmit::KvPagesExhausted)
	{
		const FHolderShape After = W.Ledger.SlotShape(1);
		S.Expect(After.Origin == Before.Origin && After.Budget == Before.Budget && !W.Ledger.SlotHasJob(1),
		         "the refused sequence is untouched");
		S.Expect(W.Charged(ERegion::A) == XFull, "a refused pre-charge charges nothing");
		S.Expect(W.Ledger.CacheEntryDependents(2) == 0 && !W.Ledger.IsCacheEntryPreCharged(2), "Y has no dependent");
		// An adopt of an already-adopted entry still succeeds and adds nothing.
		S.Expect(W.AdoptEntry(2, 1) == EPageAdmit::Admitted, "a second adopter of X");
		S.Expect(W.Charged(ERegion::A) == XFull, "further adopters add nothing");
		S.Expect(W.Ledger.CacheEntryDependents(1) == 2, "X has two dependents");
		// After X's dependents reset, the refused adopt succeeds when re-issued.
		S.Expect(W.Reset(0) && W.Reset(2), "reset X's adopters");
		S.Expect(W.Charged(ERegion::A) == 0, "A returns at the last dependent");
		S.Expect(W.AdoptEntry(1, 2) == EPageAdmit::Admitted, "the re-issued adopt of Y");
		S.Expect(W.Charged(ERegion::A) == 62, "Y's pre-charge");
	}
	W.Form1(S, "end");
	(void)Sh;
	return S;
}

LEDGER_TEST(cell_8_4_a_full_up14, "8.4 [K3]; UP-14; 5 KvPagesExhausted (first adopt of a cache entry, A full); 11 A pre-charge refusal",
            "stub admits nothing and the seam charges nothing")
{
	LT_HOLDS(AFullScenario(ELedgerMutation::None, 3120), "A free = floor(L/B) - 1 = 61 refuses; the rest of 8.4");
	LT_HOLDS(AFullScenario(ELedgerMutation::None, 3104), "A free = floor(L/B) = 62 admits");
}

LEDGER_TEST(cell_1_3_precharge_skipped_boundary, "1.3 row 'A pre-charge skipped' (form 2) [K2]; §7 'shared pages charged before the first share'",
            "stub admits nothing")
{
	LT_HOLDS(AFullScenario(ELedgerMutation::None, 3120), "correct build refuses at A free 61");
	LT_KILLS(AFullScenario(ELedgerMutation::PreChargeSkipped, 3120), "A pre-charge skipped");
}

LEDGER_TEST(adopt_precharge_once_per_entry, "§3.2 keyed by the entry (two leases, one entry); further adopters add nothing; pending adopters [K2]",
            "stub admits nothing")
{
	FWorld W(CacheConfig(4, 1024));
	LT_REQUIRE(W.bConfigured && W.BuildEntry(7, 1000), "entry");
	// Two leases of one entry are one key: two adopts admitted before either applies.
	const FAdmission A1 = W.Ledger.AdmitAdoptCacheEntry(0, 7, 1000);
	const FAdmission A2 = W.Ledger.AdmitAdoptCacheEntry(1, 7, 1000);
	LT_REQUIRE(A1.Outcome == EPageAdmit::Admitted && A2.Outcome == EPageAdmit::Admitted, "both admitted");
	LT_EQ(A1.ChargedPages, 62, "the first pre-charges floor(1000/16)");
	LT_EQ(A2.ChargedPages, 0, "the second (pending behind it) adds nothing");
	LT_EQ(W.Charged(ERegion::A), 62, "one pre-charge");
	// The first fails at the engine; the second still depends, so the pre-charge stays.
	LT_REQUIRE(W.Ledger.ApplyAdopt(A1.Ticket, false), "apply a failed adopt");
	LT_EQ(W.Charged(ERegion::A), 62, "kept for the pending adopter");
	W.Pool.Adopt(W.SlotHolder[1], W.EntryHolder[7]);
	LT_REQUIRE(W.Ledger.ApplyAdopt(A2.Ticket, true), "apply the second");
	LT_EQ(W.Ledger.CacheEntryDependents(7), 1, "one dependent");
	// A lone failed adopt refunds its pre-charge at its apply.
	LT_REQUIRE(W.BuildEntry(8, 500), "entry 8");
	const FAdmission A3 = W.Ledger.AdmitAdoptCacheEntry(2, 8, 500);
	LT_EQ(W.Charged(ERegion::A), 62 + 31, "entry 8 pre-charged");
	LT_REQUIRE(W.Ledger.ApplyAdopt(A3.Ticket, false), "apply its failure");
	LT_EQ(W.Charged(ERegion::A), 62, "refunded: no dependent left");
	// Re-adopt from entry 7 to a plain prefix drops 7's dependency at the apply.
	const FNodeId N = W.Build(100);
	LT_REQUIRE(W.AdoptPrefix(1, N), "re-adopt onto a plain prefix");
	LT_EQ(W.Charged(ERegion::A), 0, "the re-adopt's apply refunds 7's pre-charge");
}

LEDGER_TEST(adopt_mode_mismatch_whole_reserve, "5 KvModeMismatch (CPU adopt onto an un-normalized holder) [K3]; §3.3 adopt",
            "stub admits nothing")
{
	FWorld W(MakeConfig(GpuShape(), 2, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	FWorld::FRestoreJob J = W.AdmitRestore(WholeReserveRestore(0));
	LT_REQUIRE(J.Adm.Outcome == EPageAdmit::Admitted, Want("SSB5 into budget mode", J.Adm.Outcome, EPageAdmit::Admitted));
	W.RunRestore(J);
	LT_REQUIRE(W.ApplyRestore(J), "apply");
	LT_REQUIRE(W.Ledger.SlotShape(0).Mode == EHolderMode::WholeReserve, "a whole-reserve holder");
	const uint32_t Before = W.Ledger.TotalCharged();
	const FAdmission A = W.Ledger.AdmitAdoptPrefix(0, N);
	LT_REQUIRE(A.Outcome == EPageAdmit::KvModeMismatch, Want("adopt onto it", A.Outcome, EPageAdmit::KvModeMismatch));
	LT_EQ(W.Ledger.TotalCharged(), Before, "nothing charged");
	LT_REQUIRE(W.Reset(0), "the normalizing reset");
	LT_REQUIRE(W.Ledger.SlotShape(0).Mode == EHolderMode::Budget, "normalized");
	LT_REQUIRE(W.AdoptPrefix(0, N), "adopt after normalization");
}

LEDGER_TEST(cache_hint_restore_precharge, "§3.2 'a hinted restore that names a cache entry ... pre-charges it'; refunded at apply when it does not share [K3]",
            "stub admits nothing")
{
	FWorld W(CacheConfig(4, 1024));
	LT_REQUIRE(W.bConfigured && W.BuildEntry(5, 1000), "entry");
	FWorld::FRestoreJob J = W.AdmitRestore(EntryHintedRestore(0, 512, 1000, 5, 1000));
	LT_REQUIRE(J.Adm.Outcome == EPageAdmit::Admitted, "hinted restore admitted");
	LT_EQ(W.Charged(ERegion::A), 62, "pre-charged at admission: it may share");
	LT_EQ(W.Charged(ERegion::H), 95, "H charged as if it does not");
	W.RunRestore(J, false);  // a byte differs: private
	LT_REQUIRE(W.ApplyRestore(J), "apply");
	LT_EQ(W.Charged(ERegion::A), 0, "refunded at the apply: it did not share");
	FWorld::FRestoreJob K = W.AdmitRestore(EntryHintedRestore(1, 512, 1000, 5, 1000));
	W.RunRestore(K, true);
	LT_REQUIRE(K.Shared == 62 && W.ApplyRestore(K), "a sharing restore");
	LT_EQ(W.Charged(ERegion::A), 62, "kept: the restored sequence is a dependent");
	LT_EQ(W.Ledger.CacheEntryDependents(5), 1, "registered");
	LT_REQUIRE(W.Reset(1), "reset");
	LT_EQ(W.Charged(ERegion::A), 0, "refunded at the last dependent's reset");
	// A full: a cache-hinted budget restore that cannot take the pre-charge is refused.
	LT_REQUIRE(W.BuildEntry(6, 4096), "a whole-cap entry");
	LT_REQUIRE(W.AdoptEntry(2, 6) == EPageAdmit::Admitted, "fills A");
	const FWorld::FRestoreJob R = W.AdmitRestore(EntryHintedRestore(3, 512, 1000, 5, 1000));
	LT_REQUIRE(R.Adm.Outcome == EPageAdmit::KvPagesExhausted, Want("A cannot take the pre-charge", R.Adm.Outcome, EPageAdmit::KvPagesExhausted));
	LT_EQ(W.Charged(ERegion::H), 62, "nothing charged to H for the refused restore (slot 0's settled hold only)");
}

// 8.3: a cache entry evicted while adopters hold it, with zero slack outside C and A (form 3).
static FSoft EvictionScenario(ELedgerMutation M)
{
	FSoft S;
	// S = 5 x 33, P = 0, H = 256, A = 256, C = 512: the pool is exactly the regions.
	FWorld W(MakeConfig(GpuShape(), 5, 0, 1, 1, true, 512), M);
	S.Expect(W.bConfigured, "Configure");
	S.Expect(W.BuildEntry(1, 4096) && W.BuildEntry(2, 4096), "two whole-cap entries fill C");
	S.Expect(W.AdoptEntry(0, 1) == EPageAdmit::Admitted, "first adopter of entry 1");
	S.Expect(W.Charged(ERegion::A) == 256, "the first adopter pre-charges floor(L/B) = 256");
	S.Expect(W.AdoptEntry(1, 1) == EPageAdmit::Admitted && W.AdoptEntry(2, 1) == EPageAdmit::Admitted, "two more adopters");
	S.Expect(W.Charged(ERegion::A) == 256, "the other two add nothing");
	W.Form1(S, "three adopters");
	// A is full: the first adopt of entry 2 is refused (the mutant admits it).
	W.AdoptEntry(3, 2);
	// A restore of this plan's, admitted before the eviction and held before its run.
	FWorld::FRestoreJob J = W.AdmitRestore(WholeReserveRestore(4));
	S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "a restore admitted before the eviction");
	W.EvictEntry(1);
	W.EvictEntry(2);
	W.Form1(S, "after both evictions");
	// The cache builds into C up to its budget.
	S.Expect(W.BuildEntry(3, 4096), "the cache rebuilds entry 3");
	S.Expect(W.BuildEntry(4, 4096), "the cache rebuilds entry 4");
	W.Form1(S, "C full again");
	W.RunRestore(J);
	S.Expect(J.NewHolder >= 0, "the held restore runs and succeeds");
	W.ApplyRestore(J);
	W.Form1(S, "restore applied");
	for (int32_t Slot = 0; Slot < 3; ++Slot) S.Expect(W.Reset(Slot), "reset an adopter");
	S.Expect(W.Free(ERegion::A) == 256, "after the three resets, A is free");
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(cell_8_3_eviction_with_adopters_zero_slack, "8.3 [K3] host leg (form 3 and form 1); §7 'charges never exceed the pool'",
            "stub admits nothing and the seam charges nothing")
{
	LT_HOLDS(EvictionScenario(ELedgerMutation::None), "zero engine exhaustion through eviction and rebuild");
	LT_KILLS(EvictionScenario(ELedgerMutation::PreChargeSkipped), "pre-charge skipped (exhausts under zero slack)");
}
