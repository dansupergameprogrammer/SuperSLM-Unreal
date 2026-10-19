// Lifetime and windows: cycles, zero-slack exhaustion per request kind, the in-flight and enqueue
// windows, post-admission failures, epoch ends, the ledger-fault detector, and the seeded random
// workload with "charges never exceed the pool" (and charges >= drawn) checked after every step.
// Plugin paging plan §3.2, §3.5, §3.9; cells 1.1, 1.2, 1.4, 1.5, 1.6, 1.9, 1.11, 11.2.

#include "ledger_test_support.h"

#include <deque>
#include <random>

using namespace ledgertest;
using namespace SuperSLMPaging;

static FPageLedgerConfig CpuOrGpu(bool bGpu, const FShape& Sh, int32_t BlockCount, int32_t Pbc, int32_t Rhb)
{
	return bGpu ? MakeGpuConfig(Sh, BlockCount, Pbc, Rhb) : MakeConfig(Sh, BlockCount, Pbc, Rhb, 1, false, 0);
}

// ---- 1.1: 1,000 cycles on every slot ----

static FSoft CyclesScenario(bool bGpu, ELedgerMutation M, int Cycles)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(CpuOrGpu(bGpu, Sh, 4, 2, 1), M);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t PostConfigure = W.Ledger.TotalCharged();
	const FNodeId A = W.Build(1000);
	const FNodeId B = W.Build(500);
	S.Expect(A != InvalidNode && B != InvalidNode, "two prefixes");
	for (int C = 0; C < Cycles && S.bOk; ++C)
	{
		for (int32_t Slot = 0; Slot < 4; ++Slot)
		{
			// vend -> adopt -> (generate) -> reset -> re-adopt -> adopt another (re-adopt drops the
			// previous dependency) -> return (reset).
			S.Expect(W.AdoptPrefix(Slot, A), "adopt");
			S.Expect(W.Reset(Slot), "reset");
			S.Expect(W.AdoptPrefix(Slot, A), "re-adopt");
			S.Expect(W.AdoptPrefix(Slot, B), "re-adopt onto another prefix");
			S.Expect(W.Reset(Slot), "return");
		}
	}
	S.Expect(W.Ledger.DependentCount(A) == 0 && W.Ledger.DependentCount(B) == 0, "no dependency outlives its cycle");
	S.Expect(W.Release(A) && W.Release(B), "release both prefixes");
	S.Expect(W.Ledger.TotalCharged() == PostConfigure,
	         WantN("the ledger equals its post-Configure value", W.Ledger.TotalCharged(), PostConfigure));
	S.Expect(W.Pool.Drawn() == PostConfigure, WantN("the pool model equals it", W.Pool.Drawn(), PostConfigure));
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(cell_1_1_lifecycle_cycles, "1.1 [K2] (form 1 at the end; a dependency never dropped at re-adopt leaks a charge)",
            "stub admits nothing")
{
	LT_HOLDS(CyclesScenario(false, ELedgerMutation::None, 1000), "1,000 cycles on every slot");
	LT_KILLS(CyclesScenario(false, ELedgerMutation::DependencyKeptAtReadopt, 3), "dependency kept at re-adopt");
}

// ---- 1.2: zero-slack exhaustion, each request kind in turn (form 3) ----

static FSoft ZeroSlackKind(bool bGpu, int Kind)
{
	FSoft S;
	const FShape Sh = GpuShape();
	auto Quiesce = [&](FWorld& W, const char* What) {
		W.Form1(S, std::string(What) + " admitted and applied");
		S.Expect(W.Pool.Exhaustions() == 0, std::string(What) + ": zero engine exhaustion");
		S.Expect(W.Ledger.LedgerFaults() == 0, std::string(What) + ": LedgerFaults 0");
	};
	switch (Kind)
	{
	case 0:  // prefix build: P free = R(1000) = 64 exactly (a 192-page ghost), H sized 0
	{
		FWorld W(CpuOrGpu(bGpu, Sh, 2, 1, 0));
		const FNodeId G = W.Build(192 * 16);
		S.Expect(G && W.AdoptPrefix(0, G) && W.Release(G), "ghost filler");
		S.Expect(W.Free(ERegion::P) == 64 && W.Pool.Free() == 64, "the pool's only free pages are P's 64");
		const FNodeId N = W.Build(1000);
		S.Expect(N != InvalidNode, "prefix build admitted at zero slack");
		Quiesce(W, "prefix build");
		S.Expect(W.Reset(0), "drain the ghost");
		S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), "quiescent equality");
		break;
	}
	case 1:  // nested build: P free = R(200) = 14
	{
		FWorld W(CpuOrGpu(bGpu, Sh, 3, 2, 0));
		const FNodeId Parent = W.Build(1000);
		const FNodeId G1 = W.Build(256 * 16);
		S.Expect(G1 && W.AdoptPrefix(0, G1) && W.Release(G1), "ghost 1");
		const FNodeId G2 = W.Build(179 * 16);
		S.Expect(G2 && W.AdoptPrefix(1, G2) && W.Release(G2), "ghost 2");
		S.Expect(W.Pool.Free() == 14, WantN("pool free", W.Pool.Free(), 14));
		S.Expect(W.Build(200, Parent) != InvalidNode, "nested build admitted at zero slack");
		Quiesce(W, "nested build");
		S.Expect(W.Reset(0) && W.Reset(1), "drain the ghosts");
		S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), "quiescent equality");
		break;
	}
	case 2:  // unhinted restore: H free = 95 exactly, P sized 0
	case 3:  // hinted restore that does not share (prefix length 1,001 vs origin 1,000), P drawn to its bound
	{
		const bool bHinted = Kind == 3;
		FWorld W(CpuOrGpu(bGpu, Sh, 4, bHinted ? 2 : 0, 1));
		FNodeId Hint = InvalidNode;
		if (bHinted)
		{
			// P = 512: the hint (63), a 193-page ghost, then a whole-cap prefix (256, no build gap).
			Hint = W.Build(1001);
			const FNodeId G = W.Build(193 * 16);
			S.Expect(Hint && G && W.AdoptPrefix(3, G) && W.Release(G), "hint and ghost");
			S.Expect(W.Build(4096) != InvalidNode, "a whole-cap prefix");
			S.Expect(W.Free(ERegion::P) == 0, "P drawn to its bound");
		}
		// Fill H to free 95 with a settled 161-page hold.
		FWorld::FRestoreJob F = W.AdmitRestore(BudgetRestore(1, 1, OriginForHold(Sh, 161)));
		W.RunRestore(F);
		W.ApplyRestore(F);
		const uint32_t Before = W.Pool.Free();
		S.Expect(Before == 95, WantN("the pool's only free pages are H's", Before, 95));
		FWorld::FRestoreJob J = W.AdmitRestore(bHinted ? HintedRestore(0, 512, 1000, Hint) : BudgetRestore(0, 512, 1000));
		S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "restore admitted at H free = its charge");
		S.Expect(W.Free(ERegion::H) == 0, "H exactly consumed");
		W.RunRestore(J);
		S.Expect(J.NewHolder >= 0 && J.Shared == 0, "the draw succeeds, privately");
		W.ApplyRestore(J);
		Quiesce(W, bHinted ? "hinted restore (no share)" : "unhinted restore");
		S.Expect(W.Pool.Free() == Before - 62, "the pool keeps exactly the 62 held");
		if (bHinted) S.Expect(W.Reset(3), "drain the ghost");
		S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), "quiescent equality");
		break;
	}
	case 4:  // SSB5 into budget: H = P_blk exactly
	{
		FWorld W(CpuOrGpu(bGpu, Sh, 2, 0, 1));
		S.Expect(W.Pool.Free() == 256, "the pool's only free pages are H's");
		FWorld::FRestoreJob J = W.AdmitRestore(WholeReserveRestore(0));
		S.Expect(J.Adm.Outcome == EPageAdmit::Admitted, "SSB5 admitted at zero slack");
		W.RunRestore(J);
		W.ApplyRestore(J);
		Quiesce(W, "SSB5");
		S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), "quiescent equality");
		break;
	}
	case 5:  // the first adopt of a cache entry, then its eviction with adopters (CPU only: A is 0 on the GPU)
	{
		FWorld W(MakeConfig(Sh, 3, 0, 0, 1, true, 256));
		S.Expect(W.BuildEntry(1, 4096), "a whole-cap entry fills C");
		S.Expect(W.Free(ERegion::A) == 256, "A free = floor(L/B)");
		S.Expect(W.AdoptEntry(0, 1) == EPageAdmit::Admitted && W.AdoptEntry(1, 1) == EPageAdmit::Admitted, "adopters");
		W.EvictEntry(1);
		S.Expect(W.BuildEntry(2, 4096), "the cache rebuilds into C with zero slack");
		Quiesce(W, "cache entry adopt and eviction");
		S.Expect(W.Reset(0) && W.Reset(1), "the adopters let go");
		S.Expect(W.Free(ERegion::A) == 256, "A free");
		S.Expect(W.Ledger.TotalCharged() == W.Pool.Drawn(), "quiescent equality");
		break;
	}
	default:
		break;
	}
	return S;
}

LEDGER_TEST(cell_1_2_zero_slack_each_kind, "1.2 [K2/K3] (form 3): prefix, nested, unhinted restore, hinted no-share restore, SSB5, first cache-entry adopt, eviction with adopters",
            "stub admits nothing")
{
	for (int Kind = 0; Kind <= 5; ++Kind)
	{
		LT_HOLDS(ZeroSlackKind(false, Kind), "kind " + std::to_string(Kind));
	}
}

// ---- 1.4: the in-flight window after the draw (HoldApply) ----

static FSoft InFlightScenario(bool bGpu)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(CpuOrGpu(bGpu, Sh, 6, 3, 2));
	S.Expect(W.bConfigured, "Configure");
	// Prefix build held at apply.
	FWorld::FBuild Bd = W.AdmitRoot(1000);
	W.RunBuild(Bd, InvalidNode, 1000);
	W.Form1(S, "prefix build held at apply");
	S.Expect(W.Charged(ERegion::P) == 64 && W.Pool.DrawnByTag(EPageTag::Prefix) == 63, "charge 64 covers the 63 drawn");
	W.ApplyBuild(Bd);
	// Nested build held.
	FWorld::FBuild Ch = W.AdmitChild(Bd.Adm.Node, 200);
	W.RunBuild(Ch, Bd.Adm.Node, 200);
	W.Form1(S, "nested build held at apply");
	W.ApplyBuild(Ch);
	// Restores held: private, sharing, SSB5.
	FWorld::FRestoreJob P = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	W.RunRestore(P);
	W.Form1(S, "private restore held at apply");
	FWorld::FRestoreJob Sh2 = W.AdmitRestore(HintedRestore(1, 512, 1000, Bd.Adm.Node));
	W.RunRestore(Sh2);
	W.Form1(S, "sharing restore held at apply");
	FWorld::FRestoreJob Old = W.AdmitRestore(WholeReserveRestore(2));
	W.RunRestore(Old);
	W.Form1(S, "SSB5 restore held at apply");
	W.ApplyRestore(P);
	W.ApplyRestore(Sh2);
	W.ApplyRestore(Old);
	// Normalization held.
	FWorld::FResetJob N = W.AdmitReset(2);
	S.Expect(N.Adm.bNormalizes, "a normalization");
	W.RunReset(N);
	W.Form1(S, "normalization held at apply");
	W.ApplyReset(N);
	W.Form1(S, "quiescent");
	return S;
}

LEDGER_TEST(cell_1_4_inflight_window, "1.4 [K2] (form 1 under HoldApply on each drawing kind)",
            "stub admits nothing")
{
	LT_HOLDS(InFlightScenario(false), "drawn <= charged during each hold");
}

// ---- 1.5: the enqueue window before the free (HoldStart; CPU only) ----

static FSoft EnqueueWindowScenario(ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = GpuShape();
	// A reset held before its run.
	{
		FWorld W(MakeConfig(Sh, 6, 1, 1, 1, false, 0), M);
		FWorld::FRestoreJob P = W.AdmitRestore(BudgetRestore(0, 512, 1000));
		W.RunRestore(P);
		W.ApplyRestore(P);
		// H free 194 -> 40 with a settled 154-page filler.
		FWorld::FRestoreJob F = W.AdmitRestore(BudgetRestore(1, 1, OriginForHold(Sh, 154)));
		W.RunRestore(F);
		W.ApplyRestore(F);
		S.Expect(W.Free(ERegion::H) == 40, WantN("H free", W.Free(ERegion::H), 40));
		FWorld::FResetJob R = W.AdmitReset(0);  // held by HoldStart: not run
		S.Expect(W.Charged(ERegion::H) == 62 + 154, "(form 1) the freeing slot's 62 is still charged");
		W.Form1(S, "reset held before its run");
		const FAdmission Boundary = W.Ledger.AdmitRestore(BudgetRestore(2, 1, OriginForCharge(Sh, 102)));
		S.Expect(Boundary.Outcome == EPageAdmit::KvPagesExhausted,
		         Want("(form 2) a restore sized to the pages being freed", Boundary.Outcome, EPageAdmit::KvPagesExhausted));
		W.RunReset(R);
		W.ApplyReset(R);
		S.Expect(W.Free(ERegion::H) == 102, "after the apply, the 62 returns");
	}
	// A prefix release held before its run.
	{
		FWorld W(MakeConfig(Sh, 2, 1, 1, 1, false, 0), M);
		const FNodeId A = W.Build(4096);
		S.Expect(A != InvalidNode && W.Free(ERegion::P) == 0, "P full (a whole-cap prefix has no build gap)");
		const FAdmission Rel = W.Ledger.AdmitPrefixRelease(A);  // held by HoldStart
		S.Expect(Rel.Outcome == EPageAdmit::Admitted, "release admitted");
		S.Expect(W.Charged(ERegion::P) == 256, "(form 1) the releasing prefix is still charged");
		W.Form1(S, "release held before its run");
		const FAdmission Boundary = W.Ledger.AdmitPrefixBuild(4096);  // R = 256 = the pages being freed
		S.Expect(Boundary.Outcome == EPageAdmit::KvPagesExhausted,
		         Want("(form 2) a prefix sized to the pages being freed", Boundary.Outcome, EPageAdmit::KvPagesExhausted));
		if (W.NodeHolder.count(A))
		{
			W.Pool.Release(W.NodeHolder[A]);
			W.NodeHolder.erase(A);
		}
		W.Ledger.ApplyPrefixRelease(Rel.Ticket);
		S.Expect(W.Free(ERegion::P) == 256, "after the apply, the 256 return");
	}
	return S;
}

LEDGER_TEST(cell_1_5_enqueue_window, "1.5 [K2] (CPU only; forms 1 and 2 under HoldStart on a reset and a release)",
            "stub admits nothing")
{
	LT_HOLDS(EnqueueWindowScenario(ELedgerMutation::None), "the charge stays until the apply");
	LT_KILLS(EnqueueWindowScenario(ELedgerMutation::RefundAtEnqueue), "the refund taken at enqueue");
}

// ---- 1.6: post-admission failures refund exactly ----

static FSoft FailureScenario(bool bGpu, ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(bGpu ? MakeGpuConfig(Sh, 4, 2, 1) : MakeConfig(Sh, 4, 2, 1, 1, true, 512), M);
	S.Expect(W.bConfigured, "Configure");
	const FNodeId Parent = W.Build(1000);
	const uint32_t Base = W.Ledger.TotalCharged();
	const FHolderShape Shape0 = W.Ledger.SlotShape(0);
	// An engine-refused blob (or, on the GPU, HOLDER_MODE_MISMATCH forced by a seam).
	const FAdmission R = W.Ledger.AdmitRestore(BudgetRestore(0, 512, 1000));
	S.Expect(R.Outcome == EPageAdmit::Admitted && W.Ledger.TotalCharged() == Base + 95, "restore admitted, 95 charged");
	W.Ledger.ApplyRestore(R.Ticket, false, 0);
	S.Expect(W.Ledger.TotalCharged() == Base, WantN("a failed restore returns its whole charge", W.Ledger.TotalCharged(), Base));
	S.Expect(W.Ledger.SlotShape(0).Budget == Shape0.Budget && W.Ledger.SlotShape(0).Origin == Shape0.Origin && !W.Ledger.SlotHasJob(0),
	         "the slot is unchanged");
	// A failed begin_from (the parent released by a seam before the job ran).
	const FAdmission C = W.Ledger.AdmitNestedPrefixBuild(Parent, 200);
	S.Expect(C.Outcome == EPageAdmit::Admitted, "child admitted");
	W.Ledger.ApplyPrefixBuild(C.Ticket, false);
	S.Expect(W.Ledger.TotalCharged() == Base, "a failed begin_from returns its whole charge");
	S.Expect(W.Ledger.LivePrefixCount() == 1 && W.Ledger.DependentCount(Parent) == 0, "its count slot and dependency are gone");
	// A failed adopt changes nothing; a failed first adopt of a cache entry refunds its pre-charge.
	const FAdmission A = W.Ledger.AdmitAdoptPrefix(1, Parent);
	W.Ledger.ApplyAdopt(A.Ticket, false);
	S.Expect(W.Ledger.DependentCount(Parent) == 0 && W.Ledger.TotalCharged() == Base, "a failed adopt changes nothing");
	if (!bGpu)
	{
		S.Expect(W.BuildEntry(9, 1000), "entry");
		const uint32_t WithEntry = W.Ledger.TotalCharged();
		const FAdmission E = W.Ledger.AdmitAdoptCacheEntry(1, 9, 1000);
		W.Ledger.ApplyAdopt(E.Ticket, false);
		S.Expect(W.Ledger.TotalCharged() == WithEntry, "a failed first adopt refunds its pre-charge");
	}
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(cell_1_6_failures_refund_exactly, "1.6 [K2/K3] (form 1: the ledger returns to its pre-admission value)",
            "stub admits nothing")
{
	LT_HOLDS(FailureScenario(false, ELedgerMutation::None), "CPU");
	LT_KILLS(FailureScenario(false, ELedgerMutation::FailedRestoreKeepsCharge), "a failed restore never refunds H");
}

LEDGER_TEST(cell_1_9_gpu_twins, "1.9 [K6]: the GPU pooled twins of 1.1, 1.2, 1.4 and 1.6 (ledger legs)",
            "stub admits nothing")
{
	LT_HOLDS(CyclesScenario(true, ELedgerMutation::None, 200), "1.1 GPU twin");
	for (int Kind = 0; Kind <= 4; ++Kind) LT_HOLDS(ZeroSlackKind(true, Kind), "1.2 GPU twin kind " + std::to_string(Kind));
	LT_HOLDS(InFlightScenario(true), "1.4 GPU twin");
	LT_HOLDS(FailureScenario(true, ELedgerMutation::None), "1.6 GPU twin (HOLDER_MODE_MISMATCH forced)");
	LT_KILLS(FailureScenario(true, ELedgerMutation::FailedRestoreKeepsCharge), "1.6 GPU twin mutant");
}

// ---- 1.11: teardown, re-Configure, destruction, device loss ----

static FSoft EpochEndScenario(EEpochEnd Reason, bool bGpu, ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = GpuShape();
	const FPageLedgerConfig Cfg = CpuOrGpu(bGpu, Sh, 6, 3, 1);
	FWorld W(Cfg, M);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t PostConfigure = W.Ledger.TotalCharged();
	// Prefixes with dependents.
	const FNodeId N = W.Build(1000);
	S.Expect(W.AdoptPrefix(0, N), "a dependent");
	// A queued restore, two in flight, and one waiting on H.
	FWorld::FRestoreJob J[3];
	for (int I = 0; I < 3; ++I) J[I] = W.AdmitRestore(BudgetRestore(1 + I, 512, 1000));
	S.Expect(J[2].Adm.Outcome == EPageAdmit::Waiting, "a restore waiting on H");
	// A build mid-flight (on the GPU: mid-rotation).
	FWorld::FBuild Bd = W.AdmitRoot(500);
	S.Expect(Bd.Adm.Outcome == EPageAdmit::Admitted, "a build in flight");
	W.Ledger.DrainResolutions();
	if (Reason == EEpochEnd::Reconfigure)
	{
		S.Expect(W.Ledger.Configure(Cfg) == ELedgerConfigure::Ok, "re-Configure");
	}
	else
	{
		W.Ledger.EndEpoch(Reason);
		S.Expect(!W.Ledger.IsConfigured(), "unconfigured");
		S.Expect(W.Ledger.TotalCharged() == 0 && W.Ledger.PoolPages() == 0, "every charge dropped with the pool");
		S.Expect(W.Ledger.AdmitPrefixBuild(10).Outcome == EPageAdmit::NotConfigured, "no admission without a ledger");
	}
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	S.Expect(Res.size() == 1 && Res[0].Ticket == J[2].Adm.Ticket && Res[0].Outcome == EPageAdmit::NotConfigured,
	         "the waiting restore resolves NotConfigured");
	// Stale tickets from the ended epoch change nothing.
	S.Expect(!W.Ledger.ApplyRestore(J[0].Adm.Ticket, true, 0), "a stale restore apply is refused");
	S.Expect(!W.Ledger.ApplyPrefixBuild(Bd.Adm.Ticket, true), "a stale build apply is refused (its charge went with the pool)");
	if (Reason != EEpochEnd::Reconfigure)
	{
		S.Expect(W.Ledger.Configure(Cfg) == ELedgerConfigure::Ok, "the next Configure");
	}
	S.Expect(W.Ledger.TotalCharged() == PostConfigure, WantN("the rebuilt ledger equals a fresh Configure", W.Ledger.TotalCharged(), PostConfigure));
	S.Expect(W.Ledger.LivePrefixCount() == 0 && W.Ledger.GhostCount() == 0, "nothing carried over");
	S.Expect(W.Ledger.GetStats().WaitingRestores == 0 && W.Ledger.GetStats().InFlightRestores == 0, "no restore carried over");
	S.Expect(W.Ledger.SlotShape(1).Budget == Sh.SequenceBudget && !W.Ledger.SlotHasJob(1), "slots fresh");
	return S;
}

// A waiter admitted at an apply but not yet drained: its job was never enqueued, so the epoch end
// must resolve it NotConfigured (never Admitted), once, with nothing drawn for it.
static FSoft UndrainedAdmissionScenario(EEpochEnd Reason, bool bGpu)
{
	FSoft S;
	const FShape Sh = GpuShape();
	const FPageLedgerConfig Cfg = CpuOrGpu(bGpu, Sh, 6, 3, 1);
	FWorld W(Cfg);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t PostConfigure = W.Ledger.TotalCharged();
	FWorld::FRestoreJob J[3];
	for (int I = 0; I < 3; ++I) J[I] = W.AdmitRestore(BudgetRestore(1 + I, 512, 1000));
	S.Expect(J[0].Adm.Outcome == EPageAdmit::Admitted && J[1].Adm.Outcome == EPageAdmit::Admitted &&
	             J[2].Adm.Outcome == EPageAdmit::Waiting,
	         "admit, admit, wait");
	W.RunRestore(J[0]);
	W.ApplyRestore(J[0]);
	S.Expect(W.Ledger.GetStats().WaitingRestores == 0 && W.Ledger.GetStats().InFlightRestores == 2,
	         "the apply admitted the waiter (not yet drained)");
	if (Reason == EEpochEnd::Reconfigure)
	{
		S.Expect(W.Ledger.Configure(Cfg) == ELedgerConfigure::Ok, "re-Configure");
	}
	else
	{
		W.Ledger.EndEpoch(Reason);
		S.Expect(W.Ledger.Configure(Cfg) == ELedgerConfigure::Ok, "the next Configure");
	}
	const std::vector<FWaitResolution> Res = W.Ledger.DrainResolutions();
	S.Expect(Res.size() == 1 && Res[0].Ticket == J[2].Adm.Ticket && Res[0].Outcome == EPageAdmit::NotConfigured,
	         "the undrained admission resolves NotConfigured, once");
	S.Expect(!W.Ledger.ApplyRestore(J[2].Adm.Ticket, true, 0), "its ticket is stale");
	S.Expect(W.Ledger.TotalCharged() == PostConfigure && W.Ledger.GetStats().InFlightRestores == 0,
	         WantN("nothing charged for it in the new epoch", W.Ledger.TotalCharged(), PostConfigure));
	return S;
}

// ---- Cell 3.5: one job per slot, admitted at the head of the queue (§3.2; coverage audit G1) ----

// A host model of the subsystem's per-sequence queue (MaxQueuedOperationsPerSequence = 4). The
// subsystem is Unreal code; this driver stands in for it so that the ledger's half of the rule
// (a second admission on a busy slot is refused) and the driver's half (admit at the head) are
// graded together. Its two mutants are the plan's: admission at queue time, and a queue of 1.
enum class EQueueModel
{
	AdmitAtHead,
	AdmitAtQueueTime,
	NarrowedToOne
};

struct FQueuedOp
{
	enum EKind
	{
		Reset,
		Restore,
		Adopt
	} Kind = Reset;
	FRestoreRequest Req;
	FNodeId Node = InvalidNode;
	FAdmission Adm;
	bool bAdmitted = false;
};

static FAdmission AdmitOp(FWorld& W, int32_t Slot, const FQueuedOp& Op)
{
	switch (Op.Kind)
	{
	case FQueuedOp::Reset: return W.Ledger.AdmitReset(Slot);
	case FQueuedOp::Restore: return W.Ledger.AdmitRestore(Op.Req);
	case FQueuedOp::Adopt: return W.Ledger.AdmitAdoptPrefix(Slot, Op.Node);
	}
	return FAdmission{};
}

static FSoft QueueScenario(EQueueModel Model)
{
	FSoft S;
	const FShape Sh = CpuShape();
	FWorld W(MakeConfig(Sh, 4, 1, 1, 1, false, 0));
	S.Expect(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	S.Expect(N != InvalidNode, "a prefix to adopt");
	// Another slot's restore, held in flight (HoldApply): H free 40.
	FWorld::FRestoreJob Other = W.AdmitRestore(BudgetRestore(1, 512, 1000));
	S.Expect(Other.Adm.Outcome == EPageAdmit::Admitted && W.Free(ERegion::H) == 40, "H free 40 under another slot's hold");
	// Slot 0 queues reset -> restore (charge 55: 40 + 9 < 55, so it cannot fit) -> adopt -> restore (24).
	std::vector<FQueuedOp> Ops(4);
	Ops[0].Kind = FQueuedOp::Reset;
	Ops[1].Kind = FQueuedOp::Restore;
	Ops[1].Req = BudgetRestore(0, 512, 46 * Sh.B);
	Ops[2].Kind = FQueuedOp::Adopt;
	Ops[2].Node = N;
	Ops[3].Kind = FQueuedOp::Restore;
	Ops[3].Req = BudgetRestore(0, 512, 1000);
	const size_t Depth = Model == EQueueModel::NarrowedToOne ? 1 : 4;
	const uint32_t ChargedBefore = W.Ledger.TotalCharged();
	std::deque<FQueuedOp> Queue;
	for (FQueuedOp& Op : Ops)
	{
		const bool bQueued = Queue.size() < Depth;
		S.Expect(bQueued, "nothing is refused at queue time (the queue keeps its depth of 4)");
		if (!bQueued) return S;
		if (Model == EQueueModel::AdmitAtQueueTime)
		{
			Op.Adm = AdmitOp(W, 0, Op);
			Op.bAdmitted = true;
			S.Expect(Op.Adm.Outcome != EPageAdmit::InvalidRequest && Op.Adm.Outcome != EPageAdmit::KvPagesExhausted,
			         std::string("nothing is refused at queue time: got ") + AdmitName(Op.Adm.Outcome));
		}
		Queue.push_back(Op);
	}
	if (Model == EQueueModel::AdmitAtHead)
	{
		S.Expect(W.Ledger.TotalCharged() == ChargedBefore && !W.Ledger.SlotHasJob(0), "the ledger shows no charge for a queued request");
	}
	std::vector<EPageAdmit> AtHead;
	while (!Queue.empty())
	{
		FQueuedOp Op = Queue.front();
		Queue.pop_front();
		if (!Op.bAdmitted) Op.Adm = AdmitOp(W, 0, Op);
		AtHead.push_back(Op.Adm.Outcome);
		if (Op.Adm.Outcome != EPageAdmit::Admitted) continue;
		// While the head job is open, a second admission on the slot is refused and charges nothing.
		const uint32_t Open = W.Ledger.TotalCharged();
		const FAdmission Second = W.Ledger.AdmitReset(0);
		S.Expect(Second.Outcome == EPageAdmit::InvalidRequest && W.Ledger.TotalCharged() == Open,
		         Want("a second admission on the busy slot", Second.Outcome, EPageAdmit::InvalidRequest));
		if (Op.Kind == FQueuedOp::Reset)
		{
			FWorld::FResetJob J;
			J.Slot = 0;
			J.Adm = Op.Adm;
			W.RunReset(J);
			W.ApplyReset(J);
		}
		else if (Op.Kind == FQueuedOp::Adopt)
		{
			W.Pool.Adopt(W.SlotHolder[0], W.NodeHolder[N]);
			W.Ledger.ApplyAdopt(Op.Adm.Ticket, true);
		}
		else
		{
			FWorld::FRestoreJob J;
			J.Req = Op.Req;
			J.Adm = Op.Adm;
			W.RunRestore(J);
			W.ApplyRestore(J);
		}
		W.Form1(S, "after a head job's apply");
	}
	const std::vector<EPageAdmit> Want4 = {EPageAdmit::Admitted, EPageAdmit::KvPagesExhausted, EPageAdmit::Admitted, EPageAdmit::Admitted};
	S.Expect(AtHead == Want4, "at the head: reset admitted, the 55-page restore refused KvPagesExhausted, then the adopt and the 24-page restore run");
	W.RunRestore(Other);
	W.ApplyRestore(Other);
	W.Form1(S, "end");
	return S;
}

LEDGER_TEST(cell_3_5_one_job_per_slot, "3.5 [K2/K6] host leg (coverage audit G1): a 4-deep queue on one slot (reset, restore, adopt, restore) admitted at its head; a second admission on a busy slot is refused; a waiting restore keeps its slot busy",
            "none: green on arrival (the core already refuses a second admission on a busy slot, InvalidRequest; the mutants are the driver's)")
{
	LT_HOLDS(QueueScenario(EQueueModel::AdmitAtHead), "admission at the head of the queue");
	LT_KILLS(QueueScenario(EQueueModel::AdmitAtQueueTime), "admission at queue time (the second queued call is refused)");
	LT_KILLS(QueueScenario(EQueueModel::NarrowedToOne), "the queue narrowed to 1");
	// A waiting restore is an open job: the slot stays busy until it resolves.
	FWorld W(MakeConfig(CpuShape(), 4, 1, 1, 1, false, 0));
	LT_REQUIRE(W.bConfigured, "Configure");
	const FNodeId N = W.Build(1000);
	FWorld::FRestoreJob A = W.AdmitRestore(BudgetRestore(0, 512, 1000));
	FWorld::FRestoreJob B = W.AdmitRestore(BudgetRestore(1, 512, 1000));
	FWorld::FRestoreJob C = W.AdmitRestore(BudgetRestore(2, 512, 1000));
	LT_REQUIRE(C.Adm.Outcome == EPageAdmit::Waiting, Want("the third waits (16 + 9 + 9 >= 24)", C.Adm.Outcome, EPageAdmit::Waiting));
	const uint32_t Before = W.Ledger.TotalCharged();
	LT_REQUIRE(W.Ledger.AdmitReset(2).Outcome == EPageAdmit::InvalidRequest, "a reset behind the waiting restore is not admitted");
	LT_REQUIRE(W.Ledger.AdmitAdoptPrefix(2, N).Outcome == EPageAdmit::InvalidRequest, "nor an adopt");
	LT_EQ(W.Ledger.TotalCharged(), Before, "nothing charged");
	W.RunRestore(A);
	W.ApplyRestore(A);
	LT_REQUIRE(W.Ledger.DrainResolutions().size() == 1, "the waiter is admitted");
	W.RunRestore(C);
	LT_REQUIRE(W.ApplyRestore(C), "and applies");
	LT_REQUIRE(W.Ledger.AdmitReset(2).Outcome == EPageAdmit::Admitted, "the slot takes its next job after the apply");
	(void)B;
}

LEDGER_TEST(cell_1_11_epoch_end_resolves_waits, "1.11 [K2/K6]; §0 'Waits'; §7 'teardown, re-Configure and terminal loss resolve it NotConfigured'",
            "stub admits nothing and resolves nothing")
{
	for (EEpochEnd R : {EEpochEnd::Reconfigure, EEpochEnd::Teardown, EEpochEnd::Destroyed, EEpochEnd::DeviceLost})
	{
		LT_HOLDS(EpochEndScenario(R, false, ELedgerMutation::None), "CPU epoch end " + std::to_string(static_cast<int>(R)));
		LT_HOLDS(EpochEndScenario(R, true, ELedgerMutation::None), "GPU epoch end " + std::to_string(static_cast<int>(R)));
	}
	LT_KILLS(EpochEndScenario(EEpochEnd::Teardown, false, ELedgerMutation::WaitersSurviveEpochEnd), "waiters survive the epoch end");
	for (EEpochEnd R : {EEpochEnd::Reconfigure, EEpochEnd::Teardown, EEpochEnd::Destroyed, EEpochEnd::DeviceLost})
	{
		LT_HOLDS(UndrainedAdmissionScenario(R, false), "CPU undrained admission at epoch end " + std::to_string(static_cast<int>(R)));
		LT_HOLDS(UndrainedAdmissionScenario(R, true), "GPU undrained admission at epoch end " + std::to_string(static_cast<int>(R)));
	}
}

// ---- 11.2: the ledger-fault detector ----

static FSoft LedgerFaultScenario(ELedgerMutation M)
{
	FSoft S;
	const FShape Sh = GpuShape();
	FWorld W(MakeConfig(Sh, 2, 1, 0, 1, false, 0), M);
	S.Expect(W.bConfigured, "Configure");
	// Zero slack: H and A sized 0, a 193-page ghost in P, so the pool's only free pages are P's 63.
	const FNodeId G = W.Build(193 * 16);
	S.Expect(G && W.AdoptPrefix(0, G) && W.Release(G), "ghost filler");
	S.Expect(W.Pool.Free() == 63 && W.Free(ERegion::P) == 63, "the pool and P agree: 63 free");
	// One ForceUnderCharge set: R(1000) = 64 charged 63 is admitted, and the engine refuses its draw.
	W.Ledger.SetForceUnderChargeForTest(EChargeKind::PrefixBuild, 1);
	FWorld::FBuild Bd = W.AdmitRoot(1000);
	S.Expect(Bd.Adm.Outcome == EPageAdmit::Admitted, "the under-charge admits a request the pool cannot serve");
	W.RunBuild(Bd, InvalidNode, 1000);
	S.Expect(Bd.Holder < 0, "the engine model refuses it: KV_POOL_EXHAUSTED");
	if (Bd.Holder < 0) W.Ledger.NoteEngineExhaustion();
	W.ApplyBuild(Bd);
	S.Expect(W.Ledger.LedgerFaults() == 1, WantN("LedgerFaults", static_cast<long long>(W.Ledger.LedgerFaults()), 1));
	S.Expect(W.Ledger.GetStats().LedgerFaults == 1, "GetKvPageStats carries it");
	W.Ledger.EndEpoch(EEpochEnd::Teardown);
	S.Expect(W.Ledger.LedgerFaults() == 1, "the counter survives the epoch end");
	return S;
}

LEDGER_TEST(cell_11_2_ledger_fault_detector, "11.2 [K2]; §3.7 LedgerFaults (zero slack, one ForceUnderCharge set)",
            "stub counts nothing")
{
	LT_HOLDS(LedgerFaultScenario(ELedgerMutation::None), "the detector trips");
	LT_KILLS(LedgerFaultScenario(ELedgerMutation::LedgerFaultNotCounted), "the counter increment removed");
}

// ---- The seeded random workload: charges never exceed the pool, charges >= drawn ----

namespace
{
struct FJob
{
	enum class EKind
	{
		Build,
		Release,
		Adopt,
		AdoptEntry,
		Restore,
		Reset
	} Kind;
	FWorld::FBuild Bd;
	FNodeId Parent = InvalidNode;
	int64_t Tokens = 0;
	FNodeId Node = InvalidNode;
	FCacheEntryKey Entry = 0;
	int32_t Slot = -1;
	FTicket Ticket = InvalidTicket;
	FWorld::FRestoreJob Rj;
	FWorld::FResetJob Reset;
	int Phase = 0;  // 0 admitted, 1 drawn (restores: new holder drawn, old not yet released), 2 ran
	bool bFail = false;
};
}  // namespace

static FSoft RandomWorkload(uint32_t Seed, ELedgerMutation M, bool bGpu)
{
	FSoft S;
	std::mt19937 Rng(Seed);
	auto Pick = [&](uint32_t N) { return N == 0 ? 0u : static_cast<uint32_t>(Rng() % N); };
	const FShape Sh = (Seed % 2) ? GpuShape() : CpuShape();
	const int32_t Slots = 10;
	const FPageLedgerConfig Cfg = bGpu ? MakeGpuConfig(Sh, Slots, 3, 2) : MakeConfig(Sh, Slots, 3, 2, 1, true, 4 * TPblk(Sh.B, Sh.Cap));
	FWorld W(Cfg, M);
	S.Expect(W.bConfigured, "Configure");
	const uint32_t PostConfigure = W.Ledger.TotalCharged();
	std::vector<FJob> Jobs;
	std::map<FTicket, size_t> Waiting;  // ticket -> index in Jobs (restores)
	std::vector<FNodeId> Built;
	std::vector<FCacheEntryKey> Entries;
	FCacheEntryKey NextKey = 1;
	auto SlotBusy = [&](int32_t Slot) { return W.Ledger.SlotHasJob(Slot); };
	auto EntryPinned = [&](FCacheEntryKey K) {
		for (const FJob& J : Jobs)
		{
			if (J.Kind == FJob::EKind::AdoptEntry && J.Entry == K) return true;
			if (J.Kind == FJob::EKind::Restore && J.Rj.Req.Hint == EHintKind::CacheEntry && J.Rj.Req.HintEntry == K) return true;
		}
		for (const auto& KV : Waiting)
		{
			const FJob& J = Jobs[KV.second];
			if (J.Rj.Req.Hint == EHintKind::CacheEntry && J.Rj.Req.HintEntry == K) return true;
		}
		return false;
	};
	auto DrainResolutions = [&]() {
		for (const FWaitResolution& R : W.Ledger.DrainResolutions())
		{
			auto It = Waiting.find(R.Ticket);
			S.Expect(It != Waiting.end(), "a resolution for an unknown ticket");
			if (It == Waiting.end()) continue;
			S.Expect(R.Outcome == EPageAdmit::Admitted, "a wait resolved other than Admitted while configured");
			Jobs[It->second].Phase = 0;
			Waiting.erase(It);
		}
	};
	auto LiveBuilt = [&]() {
		std::vector<FNodeId> Out;
		for (FNodeId N : Built)
			if (W.Ledger.IsPrefixLive(N) && W.NodeHolder.count(N)) Out.push_back(N);
		return Out;
	};
	auto AdvanceJob = [&](size_t I) {
		FJob& J = Jobs[I];
		switch (J.Kind)
		{
		case FJob::EKind::Build:
			if (J.Phase == 0)
			{
				if (!J.bFail) W.RunBuild(J.Bd, J.Parent, J.Tokens);
				J.Phase = 2;
				return false;
			}
			if (!J.bFail && J.Bd.Holder >= 0) W.NodeHolder[J.Bd.Adm.Node] = J.Bd.Holder;
			W.Ledger.ApplyPrefixBuild(J.Ticket, !J.bFail && J.Bd.Holder >= 0);
			if (!J.bFail && J.Bd.Holder >= 0) Built.push_back(J.Bd.Adm.Node);
			return true;
		case FJob::EKind::Release:
			if (J.Phase == 0)
			{
				if (W.NodeHolder.count(J.Node))
				{
					W.Pool.Release(W.NodeHolder[J.Node]);
					W.NodeHolder.erase(J.Node);
				}
				J.Phase = 2;
				return false;
			}
			W.Ledger.ApplyPrefixRelease(J.Ticket);
			return true;
		case FJob::EKind::Adopt:
		case FJob::EKind::AdoptEntry:
			if (J.Phase == 0)
			{
				if (!J.bFail)
				{
					const int Target = J.Kind == FJob::EKind::Adopt ? (W.NodeHolder.count(J.Node) ? W.NodeHolder[J.Node] : -1)
					                                                : (W.EntryHolder.count(J.Entry) ? W.EntryHolder[J.Entry] : -1);
					W.Pool.Adopt(W.SlotHolder[static_cast<size_t>(J.Slot)], Target);
				}
				J.Phase = 2;
				return false;
			}
			W.Ledger.ApplyAdopt(J.Ticket, !J.bFail);
			return true;
		case FJob::EKind::Restore:
			if (J.Phase == 0)
			{
				if (!J.bFail) W.RunRestoreDraw(J.Rj, Pick(2) == 0);
				J.Phase = 1;
				return false;
			}
			if (J.Phase == 1)
			{
				if (!J.bFail) W.RunRestoreRelease(J.Rj);
				J.Phase = 2;
				return false;
			}
			if (J.bFail) W.Ledger.ApplyRestore(J.Ticket, false, 0);
			else W.ApplyRestore(J.Rj);
			return true;
		case FJob::EKind::Reset:
			if (J.Phase == 0)
			{
				W.RunReset(J.Reset);
				J.Phase = 2;
				return false;
			}
			W.ApplyReset(J.Reset);
			return true;
		}
		return true;
	};
	auto Step = [&](size_t I) {
		if (AdvanceJob(I))
		{
			Jobs.erase(Jobs.begin() + static_cast<long>(I));
			// Re-index waiting entries after the erase.
			for (auto& KV : Waiting)
				if (KV.second > I) --KV.second;
			DrainResolutions();
		}
	};
	auto Runnable = [&]() {
		std::vector<size_t> Out;
		for (size_t I = 0; I < Jobs.size(); ++I)
		{
			bool bWaiting = false;
			for (const auto& KV : Waiting)
				if (KV.second == I) bWaiting = true;
			if (!bWaiting) Out.push_back(I);
		}
		return Out;
	};
	for (int StepNo = 0; StepNo < 300 && S.bOk; ++StepNo)
	{
		const uint32_t Op = Pick(100);
		const int32_t Slot = static_cast<int32_t>(Pick(Slots));
		if (Op < 40)
		{
			const std::vector<size_t> R = Runnable();
			if (!R.empty()) Step(R[Pick(static_cast<uint32_t>(R.size()))]);
		}
		else if (Op < 48)
		{
			FJob J;
			J.Kind = FJob::EKind::Build;
			const std::vector<FNodeId> Live = LiveBuilt();
			J.Parent = (!Live.empty() && Pick(3) == 0) ? Live[Pick(static_cast<uint32_t>(Live.size()))] : InvalidNode;
			const int64_t ParentLen = J.Parent ? W.Pool.Length(W.NodeHolder[J.Parent]) : 0;
			J.Tokens = static_cast<int64_t>(Pick(static_cast<uint32_t>(Sh.Cap - ParentLen + 1)));
			J.Bd = J.Parent ? W.AdmitChild(J.Parent, J.Tokens) : W.AdmitRoot(J.Tokens);
			J.Ticket = J.Bd.Adm.Ticket;
			J.bFail = Pick(25) == 0;
			if (J.Bd.Adm.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
		}
		else if (Op < 54)
		{
			const std::vector<FNodeId> Live = LiveBuilt();
			if (!Live.empty())
			{
				FJob J;
				J.Kind = FJob::EKind::Release;
				J.Node = Live[Pick(static_cast<uint32_t>(Live.size()))];
				const FAdmission A = W.Ledger.AdmitPrefixRelease(J.Node);
				J.Ticket = A.Ticket;
				if (A.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
			}
		}
		else if (Op < 64 && !SlotBusy(Slot))
		{
			const std::vector<FNodeId> Live = LiveBuilt();
			const bool bEntry = !bGpu && !Entries.empty() && Pick(2) == 0;
			FJob J;
			J.Slot = Slot;
			J.bFail = Pick(25) == 0;
			if (bEntry)
			{
				J.Kind = FJob::EKind::AdoptEntry;
				J.Entry = Entries[Pick(static_cast<uint32_t>(Entries.size()))];
				const FAdmission A = W.Ledger.AdmitAdoptCacheEntry(Slot, J.Entry, W.EntryLength[J.Entry]);
				J.Ticket = A.Ticket;
				if (A.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
			}
			else if (!Live.empty())
			{
				J.Kind = FJob::EKind::Adopt;
				J.Node = Live[Pick(static_cast<uint32_t>(Live.size()))];
				const FAdmission A = W.Ledger.AdmitAdoptPrefix(Slot, J.Node);
				J.Ticket = A.Ticket;
				if (A.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
			}
		}
		else if (Op < 80 && !SlotBusy(Slot))
		{
			FJob J;
			J.Kind = FJob::EKind::Restore;
			J.Slot = Slot;
			J.bFail = Pick(25) == 0;
			const uint32_t K = Pick(5);
			const int64_t Budget = 1 + static_cast<int64_t>(Pick(static_cast<uint32_t>(Sh.Cap)));
			const std::vector<FNodeId> Live = LiveBuilt();
			FRestoreRequest R = BudgetRestore(Slot, Budget, static_cast<int64_t>(Pick(static_cast<uint32_t>(Sh.Cap + 1))));
			if (K == 0) R = WholeReserveRestore(Slot);
			if (K == 1 && !Live.empty())
			{
				const FNodeId H = Live[Pick(static_cast<uint32_t>(Live.size()))];
				R = HintedRestore(Slot, Pick(2) ? Sh.SequenceBudget : Budget, W.Pool.Length(W.NodeHolder[H]), H);
			}
			if (K == 2 && !bGpu && !Entries.empty())
			{
				const FCacheEntryKey E = Entries[Pick(static_cast<uint32_t>(Entries.size()))];
				R = EntryHintedRestore(Slot, Sh.SequenceBudget, W.EntryLength[E], E, W.EntryLength[E]);
			}
			J.Rj = W.AdmitRestore(R);
			J.Ticket = J.Rj.Adm.Ticket;
			if (J.Rj.Adm.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
			if (J.Rj.Adm.Outcome == EPageAdmit::Waiting)
			{
				Jobs.push_back(J);
				Waiting[J.Ticket] = Jobs.size() - 1;
			}
		}
		else if (Op < 90 && !SlotBusy(Slot))
		{
			FJob J;
			J.Kind = FJob::EKind::Reset;
			J.Slot = Slot;
			J.Reset = W.AdmitReset(Slot);
			J.Ticket = J.Reset.Adm.Ticket;
			if (J.Reset.Adm.Outcome == EPageAdmit::Admitted) Jobs.push_back(J);
		}
		else if (Op < 95 && !bGpu)
		{
			const int64_t L = static_cast<int64_t>(Pick(static_cast<uint32_t>(Sh.Cap + 1)));
			if (W.BuildEntry(NextKey, L)) Entries.push_back(NextKey);
			++NextKey;
		}
		else if (!bGpu && !Entries.empty())
		{
			const size_t I = Pick(static_cast<uint32_t>(Entries.size()));
			if (!EntryPinned(Entries[I]))
			{
				W.EvictEntry(Entries[I]);
				Entries.erase(Entries.begin() + static_cast<long>(I));
			}
		}
		W.Form1(S, "seed " + std::to_string(Seed) + " step " + std::to_string(StepNo));
	}
	// Drain: run every job to its apply; every wait must end in admission.
	for (int Guard = 0; Guard < 20000 && S.bOk; ++Guard)
	{
		const std::vector<size_t> R = Runnable();
		if (R.empty()) break;
		Step(R.front());
		W.Form1(S, "seed " + std::to_string(Seed) + " drain");
	}
	S.Expect(Waiting.empty(), "seed " + std::to_string(Seed) + ": a wait did not end in admission");
	if (!S.bOk) return S;
	// Quiesce: reset every slot (twice, so a normalization deferred once completes), release every
	// prefix, evict every entry. The ledger must return to its post-Configure value, equal to the pool.
	for (int Pass = 0; Pass < 3; ++Pass)
		for (int32_t Slot = 0; Slot < Slots; ++Slot) W.Reset(Slot);
	for (int Pass = 0; Pass < 4; ++Pass)
		for (FNodeId N : LiveBuilt()) W.Release(N);
	for (FCacheEntryKey K : Entries) W.EvictEntry(K);
	W.Form1(S, "seed " + std::to_string(Seed) + " quiescent");
	S.Expect(W.Ledger.TotalCharged() == PostConfigure,
	         WantN("seed " + std::to_string(Seed) + ": quiescent ledger vs post-Configure", W.Ledger.TotalCharged(), PostConfigure));
	S.Expect(W.Pool.Drawn() == PostConfigure, WantN("seed " + std::to_string(Seed) + ": quiescent pool", W.Pool.Drawn(), PostConfigure));
	return S;
}

LEDGER_TEST(invariant_random_workload, "§0/§7 'the charges never exceed the pool' and 'charges >= drawn at every instant': seeded random workload, invariants after every step, CPU (with cache) and GPU",
            "stub admits nothing, so the post-Configure ledger reads 0 against the pool's S")
{
	for (uint32_t Seed = 1; Seed <= 40; ++Seed)
	{
		LT_HOLDS(RandomWorkload(Seed, ELedgerMutation::None, false), "CPU seed " + std::to_string(Seed));
		LT_HOLDS(RandomWorkload(Seed, ELedgerMutation::None, true), "GPU seed " + std::to_string(Seed));
	}
	// Each unsafe mutant breaks an invariant on some seed.
	for (ELedgerMutation Mut : {ELedgerMutation::PreChargeSkipped, ELedgerMutation::GhostReleasedAtFirstDependent,
	                            ELedgerMutation::PrivateRestoreRefundedAtApply, ELedgerMutation::RefundAtEnqueue,
	                            ELedgerMutation::RestoredSharerNotRegistered, ELedgerMutation::DependencyKeptAtReadopt,
	                            ELedgerMutation::FailedRestoreKeepsCharge})
	{
		bool bKilled = false;
		for (uint32_t Seed = 1; Seed <= 40 && !bKilled; ++Seed) bKilled = !RandomWorkload(Seed, Mut, false).bOk;
		LT_REQUIRE(bKilled, "mutant " + std::to_string(static_cast<int>(Mut)) + " survived 40 seeds of the random workload");
	}
}
