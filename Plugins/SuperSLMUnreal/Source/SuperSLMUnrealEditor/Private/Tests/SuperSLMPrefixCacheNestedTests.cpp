// Precache plan rev 1.17, the U6 suite on the CPU cache: nested entries in budget mode (§5.4 Ghost charges and
// Pins, §5.11 (a) Pending refunds wait on their family). Preprocessed out unless
// SUPERSLM_WITH_PREFIX_CACHE is on. The GPU twins are in SuperSLMPrefixCacheGpuNestedTests.cpp.
//
//   SuperSLM.PrefixCache.Cpu.Nested.GhostCharge          1.9  parent evicted, children stay; admit to C    [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.GhostThreeLevels     1.9  P <- C <- G: evict P, then C; admit to C       [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.NullHint             8.12 nested restore, null hint: charged ceil(L/B) [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.ParentKey            2.12 the parent key is a key component             [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.WaiterPinsParent     8.14 (F1) a waiter pins its parent                 [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.GhostClaimable       8.14 (F2) (i) and (ii) an ending ghost             [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.SiblingsOutOfOrder   8.14 (R1) D1's release gated behind D2's           [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.Chain                8.14 (R1) e0 <- e1 <- e2 <- e3                     [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.ParentAfterChild     8.14 (rev 1.17) a parent evicted after its child   [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.OwnParent            8.14 (R2) own-parent restore and build             [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.Refusals             §5.6 nested-spec refusals (schema, cap, parent)    [U6]
//
// Shape. The ledger suite's rig (SuperSLMPrefixCacheCpuLedgerTests.cpp): A-EX, budget mode, zero slack
// outside C and A, C = one block of 64 pages. Each construction names its own C (7 to 12 pages): a
// leased root "pad" of CapC - C pages takes the rest, so the construction sees exactly C free. A root
// or child build is charged one page above what it settles at, so the last build of a construction
// needs one page the settled construction leaves free: each C here is the plan's C plus that one free
// page (and a request that the plan sizes to the free 0 is sized one page larger). Units
// are pages. A root of j pages has L = jB tokens; a child of a parent of p full pages with q private
// pages has a suffix of qB tokens, is charged R(qB) = q + 1 while it builds and settles at q. A nested
// restore is charged ceil(L/B) before its draw and settles at ceil(L/B) - s.
//
// Physical pages are read as the ledger suite reads them, behind the lane-park barrier: under zero
// slack, form 1 (drawn <= charged) and NoLedgerFault are the "physical never above C" reading, since a
// draw above the charges is either form 1 red, an engine exhaustion, or a LedgerFaults count.
//
// New seams this file names (declared in SuperSLMPrefixCacheTestSeam.h at U6): SetNullRestoreHints
// (8.12), SharedPagesOf (the restore's *out_shared_pages), and the stats' GhostBytes and Ghosts.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	using Seam = FSuperSLMPrefixCacheTestSeam;

	constexpr int32 kSequenceBudget = 512;

	// PrefixBlocks: region P's blocks. 0 keeps zero slack outside C and A; a cell that builds a prefill twin
	// with CreatePrefix (ParentKey) needs 1, since region P holds the 1.0.0 prefixes (box 2026-10-21: the
	// same refusal as the cooked cells', reached here once the hangs before it are fixed).
	FSuperSLMRuntimeConfig NestConfig(int32 PrefixBlocks = 0) { return MakeConfig(2, kSequenceBudget, PrefixBlocks, 0); }

	int64 CapC(const USuperSLMSubsystem& Cpu) { return LedgerOf(Cpu).CapacityOf(ERegion::C); }

	// The ledger rig, with an optional disk tier (the nested restores read their bytes from it), and
	// a leased pad so the construction sees exactly CPages of C free.
	struct FNestRig
	{
		FPagingRig Rig;
		FSuperSLMPrefix Pad;
		int32 Salt = 100;
		USuperSLMSubsystem& Cpu() { return *Rig.Cpu; }
		int64 B() const { return Rig.Geo.B; }
	};

	bool ConfigureNestRig(FAutomationTestBase& T, FNestRig& N, int64 CPages, const FString& DiskDirectory = FString(), int32 PrefixBlocks = 0)
	{
		FPagingRig& Rig = N.Rig;
		if (!Rig.Init(T) || !ConfigureBudget(T, Rig, NestConfig(PrefixBlocks)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		if (!ConfigureCacheBudget(T, Rig, WithCache(NestConfig(PrefixBlocks), Rig.Geo.PageBytes, DiskDirectory, DiskDirectory.IsEmpty() ? 0 : (int64(1) << 30))))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("budget mode: nested entries are admitted"), Stats(*Rig.Cpu).bBudgetMode))
		{
			return false;
		}
		const int64 PadPages = CapC(*Rig.Cpu) - CPages;
		if (PadPages <= 0)
		{
			return T.TestEqual(TEXT("the construction's C is the region's"), PadPages, int64(0));
		}
		TArray<int32> PadTokens;
		return MakeTokens(T, *Rig.Cpu, int32(PadPages * Rig.Geo.B), N.Salt++, PadTokens)
			&& RequestReady(T, *Rig.Cpu, Spec(PadTokens), N.Pad, TEXT("pad")) && DrainQuiet(*Rig.Cpu);
	}

	FSuperSLMPrefixSpec ChildSpec(const FSuperSLMPrefix& Parent, const TArray<int32>& Suffix)
	{
		FSuperSLMPrefixSpec S = Spec(Suffix);
		S.Parent = Parent;
		return S;
	}

	// A resident root of Pages pages; kept leased when bKeepLease.
	bool Root(FAutomationTestBase& T, FNestRig& N, int64 Pages, bool bKeepLease, FSuperSLMPrefix& Out, TArray<int32>& OutTokens, const TCHAR* What)
	{
		if (!MakeTokens(T, N.Cpu(), int32(Pages * N.B()), N.Salt++, OutTokens) || !RequestReady(T, N.Cpu(), Spec(OutTokens), Out, What))
		{
			return false;
		}
		if (!bKeepLease)
		{
			N.Cpu().ReturnPrefix(Out);
		}
		return DrainQuiet(N.Cpu());
	}

	// A resident child of Parent (a live Ready lease) with Pages private pages; the parent's lease is
	// the caller's. Kept leased when bKeepLease.
	bool Child(FAutomationTestBase& T, FNestRig& N, const FSuperSLMPrefix& Parent, int64 Pages, bool bKeepLease, FSuperSLMPrefix& Out, TArray<int32>& OutSuffix, const TCHAR* What)
	{
		if (!MakeTokens(T, N.Cpu(), int32(Pages * N.B()), N.Salt++, OutSuffix) || !RequestReady(T, N.Cpu(), ChildSpec(Parent, OutSuffix), Out, What))
		{
			return false;
		}
		if (!bKeepLease)
		{
			N.Cpu().ReturnPrefix(Out);
		}
		return DrainQuiet(N.Cpu());
	}

	// Tokens of a root build charged exactly Need pages in flight (R(L) = Need).
	bool NeedTokens(FAutomationTestBase& T, FNestRig& N, int64 Need, TArray<int32>& Out)
	{
		check(Need >= 2);
		return MakeTokens(T, N.Cpu(), int32((Need - 1) * N.B()), N.Salt++, Out);
	}

	// "Physical never above C" under zero slack (see the header).
	bool Physical(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		bool bOk = Form1ChargesCoverDrawn(T, Cpu, Instant);
		bOk &= NoLedgerFault(T, Cpu, Instant);
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C));
		return bOk;
	}

	bool IsPending(USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease)
	{
		return Cpu.GetPrefixLeaseState(Lease) == ESuperSLMPrefixLeaseState::Pending;
	}

	// Ticks, then settles the cache's pool-thread work (SettleCacheIo). Box 2026-10-21 (second run):
	// ParentAfterChild read "e1 then e0 evicted" as 0 after the fixed four ticks, because W's and X's key
	// hashes had not been applied yet. The host model applies commands synchronously.
	void Tick(USuperSLMSubsystem& Cpu, int32 Ticks = 4)
	{
		for (int32 I = 0; I < Ticks; ++I)
		{
			Cpu.Tick(Step);
		}
		SettleCacheIo(Cpu, 10.0);
	}

	// Makes the unleased Victim a ghost: every other entry is leased by the caller, the children that
	// keep it a ghost included; a request of Need pages evicts Victim and is refused; its release
	// applies. True when Victim is now a ghost of GhostPages.
	bool MakeGhost(FAutomationTestBase& T, FNestRig& N, int64 GhostPages, const TCHAR* What)
	{
		USuperSLMSubsystem& Cpu = N.Cpu();
		const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
		TArray<int32> Tokens;
		FSuperSLMPrefix Probe;
		if (!NeedTokens(T, N, GhostPages + 1, Tokens) || !Request(T, Cpu, Spec(Tokens), Probe, What))
		{
			return false;
		}
		bool bOk = ExpectLease(T, Cpu, Probe, ESuperSLMPrefixLeaseState::BudgetExhausted, What, 10.0);
		bOk &= DrainQuiet(Cpu);
		const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one eviction"), What), S1.Evictions - S0.Evictions, int64(1));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: its charge stays as a ghost"), What), S1.GhostBytes - S0.GhostBytes, GhostPages * N.Rig.Geo.PageBytes);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one more ghost"), What), S1.Ghosts - S0.Ghosts, 1);
		bOk &= Physical(T, Cpu, What);
		return bOk;
	}

	// Requests Attempts one-page roots (each charged R(B) = 2 in flight, 1 once Ready) and returns
	// each; counts the admissions. The physical reading is taken after each (in budget mode, under
	// zero slack, it is 1.4's reading: drawn never above the charges, C never above its capacity).
	bool AdmitToBudget(FAutomationTestBase& T, FNestRig& N, int32 Attempts, int32& OutAdmitted, const TCHAR* What)
	{
		bool bOk = true;
		OutAdmitted = 0;
		for (int32 I = 0; I < Attempts; ++I)
		{
			TArray<int32> Tokens;
			FSuperSLMPrefix Lease;
			if (!MakeTokens(T, N.Cpu(), int32(N.B()), N.Salt++, Tokens) || !Request(T, N.Cpu(), Spec(Tokens), Lease, What))
			{
				return false;
			}
			const ESuperSLMPrefixLeaseState State = WaitLease(N.Cpu(), Lease);
			if (State == ESuperSLMPrefixLeaseState::Ready)
			{
				++OutAdmitted;
			}
			N.Cpu().ReturnPrefix(Lease);
			bOk &= DrainQuiet(N.Cpu());
			bOk &= Physical(T, N.Cpu(), *FString::Printf(TEXT("%s, admission %d"), What, I));
		}
		return bOk;
	}
}

// Cell 1.9 (§5.4 Ghost charges): nested entries in budget mode; parent P (4) evicted while its children
// D1 and D2 (1 private page each) stay; P is a ghost of its whole charge, and the cache admits new
// entries up to the budget without the probe exceeding it. C = 9: P 4, D1 1, D2 1, a leased L of 2, 1 free.
// Red until U6: a nested spec is refused NestedRequiresBudgetMode.
// Mutant: the ghost charge dropped -> the admissions take P's 4 while D1 and D2 map them -> form 1
// red (drawn above charged) or engine exhaustion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedGhostChargeTest, "SuperSLM.PrefixCache.Cpu.Nested.GhostCharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedGhostChargeTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 9))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P, D1, D2, L;
	TArray<int32> PT, D1T, D2T, LT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, D1, D1T, TEXT("D1")) || !Child(*this, N, P, 1, true, D2, D2T, TEXT("D2"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P 4 + D1 1 + D2 1 + L 2: one page free"), LedgerOf(Cpu).FreeOf(ERegion::C), int64(1));
	const uint64 D1Engine = Seam::EngineHandleOf(Cpu, D1);
	Cpu.ReturnPrefix(P);
	DrainQuiet(Cpu);
	bOk &= MakeGhost(*this, N, 4, TEXT("P evicted under its children"));
	// The children stay cached. Box 2026-10-21: this read GetPrefixSource(D1) == Memory, but D1's lease was
	// served by its own build (Recomputed), which a later eviction cannot change, and a fresh request for D1's
	// spec needs P as a live Ready lease, which it no longer is. D1 resident is: its lease still Ready, and its
	// engine handle not released (the release-counting seam).
	bOk &= TestEqual(TEXT("D1 still cached: its lease is Ready"), (uint8)Cpu.GetPrefixLeaseState(D1), (uint8)ESuperSLMPrefixLeaseState::Ready);
	bOk &= TestEqual(TEXT("D1 still cached: its handle is not released"), Seam::ReleasesOf(Cpu, D1Engine), 0);

	// Admit up to the budget: nothing is free (the ghost holds P's 4), so every root is refused while
	// D1 and D2 are leased; return them and the admissions evict D1, D2, which ends the ghost.
	int32 Admitted = 0;
	bOk &= AdmitToBudget(*this, N, 2, Admitted, TEXT("ghost held, children leased"));
	bOk &= TestEqual(TEXT("nothing admitted while the ghost and its children hold C"), Admitted, 0);
	Cpu.ReturnPrefix(D1);
	Cpu.ReturnPrefix(D2);
	DrainQuiet(Cpu);
	bOk &= AdmitToBudget(*this, N, 8, Admitted, TEXT("children returned"));
	bOk &= TestEqual(TEXT("every one-page root admitted once the children leave"), Admitted, 8);
	bOk &= TestEqual(TEXT("the ghost ended with its last descendant"), Stats(Cpu).Ghosts, 0);
	bOk &= Physical(*this, Cpu, TEXT("the end"));
	return bOk;
}

// Cell 1.9, three levels: P <- C <- G (2, 1, 1), L (2) leased, C = 7 (1 free). Evict P (a
// ghost of 2: G still maps its pages through C), then C (a ghost of 1); admit up to the budget.
// Mutant: the ghost cleared at the last CHILD -> P ends when C leaves although G maps P's pages ->
// the admissions draw over them -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedGhostThreeLevelsTest, "SuperSLM.PrefixCache.Cpu.Nested.GhostThreeLevels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedGhostThreeLevelsTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 7))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P, C, G, L;
	TArray<int32> PT, CT, GT, LT;
	if (!Root(*this, N, 2, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, C, CT, TEXT("C")) || !Child(*this, N, C, 1, true, G, GT, TEXT("G"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")))
	{
		return false;
	}
	Cpu.ReturnPrefix(P);
	DrainQuiet(Cpu);
	bool bOk = MakeGhost(*this, N, 2, TEXT("P evicted"));
	Cpu.ReturnPrefix(C);
	DrainQuiet(Cpu);
	bOk &= MakeGhost(*this, N, 1, TEXT("C evicted"));
	bOk &= TestEqual(TEXT("two ghosts: P (2) and C (1)"), Stats(Cpu).GhostBytes, 3 * N.Rig.Geo.PageBytes);
	int32 Admitted = 0;
	bOk &= AdmitToBudget(*this, N, 2, Admitted, TEXT("G leased"));
	bOk &= TestEqual(TEXT("nothing admitted while G holds both ghosts"), Admitted, 0);
	Cpu.ReturnPrefix(G);
	DrainQuiet(Cpu);
	bOk &= AdmitToBudget(*this, N, 6, Admitted, TEXT("G returned"));
	bOk &= TestEqual(TEXT("every admission fits once G leaves"), Admitted, 6);
	bOk &= TestEqual(TEXT("both ghosts ended at G's eviction"), Stats(Cpu).Ghosts, 0);
	return bOk;
}

// Cell 8.12 (§5.4 Charges): a nested restore under a hint that fails (Seam::SetNullRestoreHints) is
// charged ceil(L/B) before its draw, never exceeds the budget, and a charge that cannot fit resolves
// BudgetExhausted by name. P (4) leased; D (child, 2 private) written to disk, evicted, then requested
// again: a restore of L = 6B, charged 6, drawing 6 privately. C = 10: P 4, D's restore 6.
// Mutant: charging ceil(L/B) - floor(origin/B) = 2 before the draw -> the restore draws 6 against a
// charge of 2 -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedNullHintTest, "SuperSLM.PrefixCache.Cpu.Nested.NullHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedNullHintTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("nested-null-hint"));
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 10, Dir.Path))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P, D, Again;
	TArray<int32> PT, DT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")))
	{
		return false;
	}
	// Box 2026-10-21: the file was looked for as soon as D's jobs had delivered, which is before the
	// write-back's file write (a pool-thread command after the Save lane job) has run. Wait for it.
	const int64 Mark = WriteBackMark(Cpu);
	if (!Child(*this, N, P, 2, false, D, DT, TEXT("D")))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("D's write-back settles"), WaitWriteBacks(Cpu, Mark));
	bOk &= TestTrue(TEXT("D's write-back landed"), FPaths::FileExists(Seam::DiskFileOf(Cpu, ChildSpec(P, DT))));
	// Evict D: a root of 6 pages (R = 6 for 5B tokens) needs D's 2 plus the 4 free.
	TArray<int32> Big;
	FSuperSLMPrefix LBig;
	if (!NeedTokens(*this, N, 6, Big) || !RequestReady(*this, Cpu, Spec(Big), LBig, TEXT("evicts D")))
	{
		return false;
	}
	Cpu.ReturnPrefix(LBig);
	DrainQuiet(Cpu);

	Seam::SetNullRestoreHints(Cpu, true);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	if (!RequestReady(*this, Cpu, ChildSpec(P, DT), Again, TEXT("D restored with a null hint")))
	{
		return false;
	}
	bOk &= ExpectSource(*this, Cpu, Again, ESuperSLMPrefixSource::Disk, TEXT("D again"));
	bOk &= TestEqual(TEXT("one restore issued"), Stats(Cpu).RestoresIssued - S0.RestoresIssued, int64(1));
	bOk &= TestEqual(TEXT("the restore ran private (s = 0)"), Seam::SharedPagesOf(Cpu, Again), int64(0));
	bOk &= Physical(*this, Cpu, TEXT("after the private restore"));
	bOk &= TestEqual(TEXT("C full: P 4 + D 6 (private)"), LedgerOf(Cpu).FreeOf(ERegion::C), int64(0));

	// A second nested restore that cannot fit (everything leased): BudgetExhausted by name, no retry as
	// a build.
	FSuperSLMPrefix D2, D2Again;
	TArray<int32> D2T;
	Cpu.ReturnPrefix(Again);
	DrainQuiet(Cpu);
	if (!Child(*this, N, P, 1, false, D2, D2T, TEXT("D2")))
	{
		return false;
	}
	FSuperSLMPrefix Hold;
	TArray<int32> HoldT;
	bOk &= Root(*this, N, 5, true, Hold, HoldT, TEXT("evicts D and D2, leased"));
	const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
	if (!Request(*this, Cpu, ChildSpec(P, DT), D2Again, TEXT("D again, cannot fit")))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Cpu, D2Again, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("the charge that cannot fit"), 10.0);
	bOk &= TestEqual(TEXT("no build issued for it"), Stats(Cpu).BuildsIssued, S1.BuildsIssued);
	bOk &= Physical(*this, Cpu, TEXT("after the refusal"));
	Seam::SetNullRestoreHints(Cpu, false);
	return bOk;
}

// Cell 2.12, the parent key [U6]: two nested specs with equal suffix tokens under different parents
// are different keys (different disk files, the second not a Memory hit, its adopter tokens equal its
// own prefill twin). Mutant: the parent key not hashed -> the second is a Memory hit of the first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedParentKeyTest, "SuperSLM.PrefixCache.Cpu.Nested.ParentKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedParentKeyTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("nested-parent-key"));
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 16, Dir.Path, /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P1, P2, A, Bl;
	TArray<int32> P1T, P2T, Suffix, Twin, TwinSuffix;
	if (!Root(*this, N, 2, true, P1, P1T, TEXT("P1")) || !Root(*this, N, 2, true, P2, P2T, TEXT("P2")) || !MakeTokens(*this, Cpu, int32(N.B()), 7, Suffix)
		|| !MakeTokens(*this, Cpu, SuffixTokens, 8, TwinSuffix))
	{
		return false;
	}
	bool bOk = RequestReady(*this, Cpu, ChildSpec(P1, Suffix), A, TEXT("A under P1"));
	bOk &= RequestReady(*this, Cpu, ChildSpec(P2, Suffix), Bl, TEXT("B under P2"));
	bOk &= TestNotEqual(TEXT("B is not a Memory hit of A"), (uint8)Cpu.GetPrefixSource(Bl), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestNotEqual(TEXT("different disk files"), Seam::DiskFileOf(Cpu, ChildSpec(P1, Suffix)), Seam::DiskFileOf(Cpu, ChildSpec(P2, Suffix)));
	TArray<int32> Whole = P2T;
	Whole.Append(Suffix);
	bOk &= TwinTokens(*this, Cpu, Whole, TwinSuffix, Twin);
	bOk &= EqualsTwin(*this, Cpu, Bl, TwinSuffix, Twin, TEXT("B's adopter"));
	return bOk;
}

// Cell 8.14 (F1): a waiter pins its parent. C = 13; LRU V (3), P (4), Q (3); L (2) leased; free 1. W,
// a child of P charged 3 (suffix 2B), takes the free 1, evicts V, claims 2 and waits; P is pinned from that decision. X,
// in the same tick, a root charged 6: Q is the only evictable entry, so X evicts Q and is refused. At
// V's apply W is admitted and its begin_from runs against a live P (the release-counting seam).
// Mutant: no pin while waiting -> X also evicts P and waits; W's job starts against a released P.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedWaiterPinsParentTest, "SuperSLM.PrefixCache.Cpu.Nested.WaiterPinsParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedWaiterPinsParentTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 13))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix V, P, Q, L, PLease, W, X;
	TArray<int32> VT, PT, QT, LT, WSuffix, XT;
	if (!Root(*this, N, 3, false, V, VT, TEXT("V")) || !Root(*this, N, 4, true, PLease, PT, TEXT("P")) || !Root(*this, N, 3, false, Q, QT, TEXT("Q"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")) || !MakeTokens(*this, Cpu, int32(2 * N.B()), N.Salt++, WSuffix) || !NeedTokens(*this, N, 6, XT))
	{
		return false;
	}
	bool bOk = Physical(*this, Cpu, TEXT("C full"));
	const uint64 PHandle = Seam::EngineHandleOf(Cpu, PLease);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);

	Seam::HoldReleases(Cpu, true);
	// W names P through PLease (a live Ready lease), which it returns once W is decided: from then on
	// only W's pin keeps P from the LRU.
	bOk &= Request(*this, Cpu, ChildSpec(PLease, WSuffix), W, TEXT("W"));
	bOk &= Request(*this, Cpu, Spec(XT), X, TEXT("X"));
	Tick(Cpu);
	Cpu.ReturnPrefix(PLease);
	Tick(Cpu);
	bOk &= TestTrue(TEXT("W waits"), IsPending(Cpu, W));
	bOk &= ExpectLease(*this, Cpu, X, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("X, as the approved rule refuses it"), 10.0);
	bOk &= TestEqual(TEXT("two evictions: V (W's) and Q (X's), never P"), Stats(Cpu).Evictions - S0.Evictions, int64(2));
	bOk &= TestEqual(TEXT("two releases held: V and Q"), Seam::HeldReleaseCount(Cpu), 2);
	bOk &= Physical(*this, Cpu, TEXT("decided, not applied"));

	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W, admitted at V's apply"));
	bOk &= TestEqual(TEXT("P never released (its handle is W's begin_from parent)"), Seam::ReleasesOf(Cpu, PHandle), 0);
	bOk &= Physical(*this, Cpu, TEXT("W built"));
	return bOk;
}

// Cell 8.14 (F2): an ending ghost is claimable. C = 11 (1 free); P (4) evicted while its child D (2
// private) stays, so P is a ghost of 4, and P's release applies; E (4) leased.
//   (i)  X needs 6: the free 1, then it evicts D and claims 5 of D's 2 and the ghost's 4, waits, and
//        is admitted at D's apply (the ghost unclaimable leaves it 3: refused).
//   (ii) a fresh run: X needs 2 (the free 1 and 1 of D's), Y needs 4 (D's other 1 and 3 of the
//        ghost's), one tick; both wait on D's refund; physical never above C
//        before D's apply; zero LedgerFaults.
// Mutants: the ghost refunded at D's decision -> (ii) X admitted at once and draws while D maps P's
// pages -> form 1 red; the ghost unclaimable -> (i) X refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedGhostClaimableTest, "SuperSLM.PrefixCache.Cpu.Nested.GhostClaimable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedGhostClaimableTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	for (int32 Leg = 0; Leg < 2; ++Leg)
	{
		FNestRig N;
		if (!ConfigureNestRig(*this, N, 11))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = N.Cpu();
		FSuperSLMPrefix P, D, E;
		TArray<int32> PT, DT, ET;
		if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 2, true, D, DT, TEXT("D")) || !Root(*this, N, 4, true, E, ET, TEXT("E")))
		{
			return false;
		}
		Cpu.ReturnPrefix(P);
		DrainQuiet(Cpu);
		// D and E leased: a 1-page request evicts P (a ghost of 4) and is refused.
		bOk &= MakeGhost(*this, N, 4, TEXT("P"));
		Cpu.ReturnPrefix(D);
		DrainQuiet(Cpu);
		Seam::HoldReleases(Cpu, true);
		if (Leg == 0)
		{
			FSuperSLMPrefix X;
			TArray<int32> XT;
			bOk &= NeedTokens(*this, N, 6, XT) && Request(*this, Cpu, Spec(XT), X, TEXT("(i) X"));
			Tick(Cpu);
			bOk &= TestTrue(TEXT("(i) X waits on D's refund (D's 2 and the ghost's 4)"), IsPending(Cpu, X));
			bOk &= Physical(*this, Cpu, TEXT("(i) before D's apply"));
			Seam::HoldReleases(Cpu, false);
			bOk &= ExpectLease(*this, Cpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) X at D's apply"));
		}
		else
		{
			FSuperSLMPrefix X, Y;
			TArray<int32> XT, YT;
			bOk &= NeedTokens(*this, N, 2, XT) && NeedTokens(*this, N, 4, YT);
			bOk &= Request(*this, Cpu, Spec(XT), X, TEXT("(ii) X")) && Request(*this, Cpu, Spec(YT), Y, TEXT("(ii) Y"));
			Tick(Cpu);
			bOk &= TestTrue(TEXT("(ii) X waits"), IsPending(Cpu, X));
			bOk &= TestTrue(TEXT("(ii) Y waits"), IsPending(Cpu, Y));
			bOk &= TestEqual(TEXT("(ii) one eviction (D)"), Seam::HeldReleaseCount(Cpu), 1);
			bOk &= Physical(*this, Cpu, TEXT("(ii) before D's apply"));
			Seam::HoldReleases(Cpu, false);
			bOk &= ExpectLease(*this, Cpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) X"));
			bOk &= ExpectLease(*this, Cpu, Y, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) Y"));
		}
		bOk &= Physical(*this, Cpu, TEXT("after D's apply"));
	}
	return bOk;
}

// Cell 8.14 (re-strike R1): siblings, releases out of order. C = 11 here (the plan's 10, plus one free
// page: a root build is charged at least R(B) = 2, so the plan's "X needs 1" is X charged 2 with one
// page free); P (4) shared by D1 and D2 (1 private each); E (4) leased. P becomes a ghost. X needs 2:
// takes the free 1, evicts D1, claims 1, waits. Y needs 5: evicts D2
// (ending the ghost), claims D2's 1 and the ghost's 4, waits. D1's release is gated behind D2's: at
// D2's apply nobody is admitted and physical stays within C; at D1's apply both are.
// Mutant: the ghost refunded at the ending descendant's apply -> Y admitted at D2's apply, drawing 5
// while D1 maps P's 4 -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedSiblingsTest, "SuperSLM.PrefixCache.Cpu.Nested.SiblingsOutOfOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedSiblingsTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 11))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P, D1, D2, E, X, Y;
	TArray<int32> PT, D1T, D2T, ET, XT, YT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, D1, D1T, TEXT("D1")) || !Child(*this, N, P, 1, true, D2, D2T, TEXT("D2"))
		|| !Root(*this, N, 4, true, E, ET, TEXT("E")) || !NeedTokens(*this, N, 2, XT) || !NeedTokens(*this, N, 5, YT))
	{
		return false;
	}
	Cpu.ReturnPrefix(P);
	DrainQuiet(Cpu);
	bool bOk = MakeGhost(*this, N, 4, TEXT("P"));
	Cpu.ReturnPrefix(D1);
	Cpu.ReturnPrefix(D2);
	DrainQuiet(Cpu);
	Seam::HoldReleases(Cpu, true);
	bOk &= Request(*this, Cpu, Spec(XT), X, TEXT("X")) && Request(*this, Cpu, Spec(YT), Y, TEXT("Y"));
	Tick(Cpu);
	bOk &= TestTrue(TEXT("X waits"), IsPending(Cpu, X));
	bOk &= TestTrue(TEXT("Y waits"), IsPending(Cpu, Y));
	bOk &= TestEqual(TEXT("D1 and D2 evicted, releases held in that order"), Seam::HeldReleaseCount(Cpu), 2);
	// D2's release (index 1) first.
	bOk &= TestTrue(TEXT("D2's release dispatched"), Seam::DispatchHeldRelease(Cpu, 1));
	Tick(Cpu);
	bOk &= TestTrue(TEXT("at D2's apply, X still waits"), IsPending(Cpu, X));
	bOk &= TestTrue(TEXT("at D2's apply, Y still waits (the ghost's family holds D1)"), IsPending(Cpu, Y));
	bOk &= Physical(*this, Cpu, TEXT("D2 applied, D1 held"));
	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("X at D1's apply"));
	bOk &= ExpectLease(*this, Cpu, Y, ESuperSLMPrefixLeaseState::Ready, TEXT("Y at D1's apply"));
	bOk &= Physical(*this, Cpu, TEXT("both admitted"));
	return bOk;
}

// Cell 8.14 (re-strike R1): a chain. C = 9 (1 free); e0 <- e1 <- e2 <- e3 (1 page each), e1 and L (4)
// leased; LRU e0, e2, e3. A request needing 5 evicts e0 (a ghost), e2 (a ghost), e3 (ending e2's ghost)
// and is refused. e3's release applies before e2's own. Z (3: the free 1, e3's 1, and e2's ghost's 1)
// then waits on e2's own release; physical never above C. Mutant: the ghost refunded at e3's apply ->
// Z admitted at once, drawing 3 while e2's handle maps its page -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedChainTest, "SuperSLM.PrefixCache.Cpu.Nested.Chain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedChainTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 9))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix E0, E1, E2, E3, L, Q, Z;
	TArray<int32> T0, T1, T2, T3, LT, QT, ZT;
	if (!Root(*this, N, 1, true, E0, T0, TEXT("e0")) || !Child(*this, N, E0, 1, true, E1, T1, TEXT("e1")) || !Child(*this, N, E1, 1, true, E2, T2, TEXT("e2"))
		|| !Child(*this, N, E2, 1, true, E3, T3, TEXT("e3")) || !Root(*this, N, 4, true, L, LT, TEXT("L")) || !NeedTokens(*this, N, 5, QT) || !NeedTokens(*this, N, 3, ZT))
	{
		return false;
	}
	// LRU order e0, e2, e3: touch them in that order, then return their leases (e1 and L stay).
	Cpu.ReturnPrefix(E0);
	Cpu.ReturnPrefix(E2);
	Cpu.ReturnPrefix(E3);
	DrainQuiet(Cpu);
	Seam::HoldReleases(Cpu, true);
	bool bOk = Request(*this, Cpu, Spec(QT), Q, TEXT("Q (5)"));
	bOk &= ExpectLease(*this, Cpu, Q, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("Q"), 10.0);
	bOk &= TestEqual(TEXT("e0, e2 and e3 evicted, releases held"), Seam::HeldReleaseCount(Cpu), 3);
	// e3's release (index 2) first, then e0's (index 0); e2's own stays held.
	bOk &= TestTrue(TEXT("e3 dispatched"), Seam::DispatchHeldRelease(Cpu, 2));
	bOk &= TestTrue(TEXT("e0 dispatched"), Seam::DispatchHeldRelease(Cpu, 0));
	Tick(Cpu);
	bOk &= Request(*this, Cpu, Spec(ZT), Z, TEXT("Z (3)"));
	Tick(Cpu);
	bOk &= TestTrue(TEXT("Z waits on e2's own release"), IsPending(Cpu, Z));
	bOk &= Physical(*this, Cpu, TEXT("e2 held"));
	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, Z, ESuperSLMPrefixLeaseState::Ready, TEXT("Z at e2's apply"));
	bOk &= Physical(*this, Cpu, TEXT("Z admitted"));
	return bOk;
}

// Cell 8.14 (rev 1.17): a parent evicted after its child, not a ghost. C = 8 (1 free); e1 (3, child
// of e0) LRU, then e0 (3); L (1) leased. W needs 5: the free 1, evicts e1 (3), then e0 (no ghost, but
// e1's handle still maps its pages; W claims 1, 2 unclaimed). X needs 2 and claims e0's 2. e0's release applies first:
// nobody is admitted; at e1's apply both are. Mutant: family sets on ghosts only -> X admitted at
// e0's apply -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedParentAfterChildTest, "SuperSLM.PrefixCache.Cpu.Nested.ParentAfterChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedParentAfterChildTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 8))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix E0, E1, L, W, X;
	TArray<int32> T0, T1, LT, WT, XT;
	if (!Root(*this, N, 1, true, L, LT, TEXT("L")) || !Root(*this, N, 3, true, E0, T0, TEXT("e0")) || !Child(*this, N, E0, 3, false, E1, T1, TEXT("e1"))
		|| !NeedTokens(*this, N, 5, WT) || !NeedTokens(*this, N, 2, XT))
	{
		return false;
	}
	Cpu.ReturnPrefix(E0); // e0 is touched after e1: e1 is LRU
	DrainQuiet(Cpu);
	Seam::HoldReleases(Cpu, true);
	bool bOk = Request(*this, Cpu, Spec(WT), W, TEXT("W (5)")) && Request(*this, Cpu, Spec(XT), X, TEXT("X (2)"));
	Tick(Cpu);
	bOk &= TestEqual(TEXT("e1 then e0 evicted"), Seam::HeldReleaseCount(Cpu), 2);
	bOk &= TestEqual(TEXT("e0 is no ghost"), Stats(Cpu).Ghosts, 0);
	bOk &= TestTrue(TEXT("e0's release dispatched first"), Seam::DispatchHeldRelease(Cpu, 1));
	Tick(Cpu);
	bOk &= TestTrue(TEXT("at e0's apply W still waits"), IsPending(Cpu, W));
	bOk &= TestTrue(TEXT("at e0's apply X still waits (e0's family holds e1)"), IsPending(Cpu, X));
	bOk &= Physical(*this, Cpu, TEXT("e0 applied, e1 held"));
	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W at e1's apply"));
	bOk &= ExpectLease(*this, Cpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("X at e1's apply"));
	bOk &= Physical(*this, Cpu, TEXT("both admitted"));
	return bOk;
}

// Cell 8.14 (re-strike R2): the own-parent reading. C = 7; P (3) unpinned, L (3) leased, 1 free.
//   restore: a nested restore of a child of P charged 4 (L = 4B, from disk) evicts P, is admitted,
//            and runs with a null hint (shared pages 0). Mutant: the restore's loop skips its parent
//            -> refused -> red.
//   build:   a nested build of the same shape is refused BudgetExhausted; P stays resident and is
//            not released (the release-counting seam). Mutant: the build's loop evicts its parent ->
//            begin_from against a released P -> red on the seam.
// The child is named through a lease of P taken for the request and returned before the decision
// (a ChildOp lookup pin holds P from the request until the decision; P's own lease does not).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedOwnParentTest, "SuperSLM.PrefixCache.Cpu.Nested.OwnParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedOwnParentTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	for (int32 Leg = 0; Leg < 2; ++Leg)
	{
		const bool bRestore = Leg == 0;
		FScopedCacheDirectory Dir(TEXT("nested-own-parent"));
		FNestRig N;
		if (!ConfigureNestRig(*this, N, 7, Dir.Path))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = N.Cpu();
		FSuperSLMPrefix P, D, L, Again;
		TArray<int32> PT, DT, LT;
		// The restore leg seeds D's file first (a child of 0 private pages is no entry: D has 1 private
		// page over P's 3 full ones, L = 4B, so its restore is charged 4).
		if (!Root(*this, N, 3, true, P, PT, TEXT("P")))
		{
			return false;
		}
		if (bRestore)
		{
			if (!Child(*this, N, P, 1, false, D, DT, TEXT("D, written back")))
			{
				return false;
			}
			// Evict D: lease P and fill the rest.
			FSuperSLMPrefix Fill;
			TArray<int32> FillT;
			if (!Root(*this, N, 3, false, Fill, FillT, TEXT("fill (evicts D; L's build evicts it in turn)")))
			{
				return false;
			}
		}
		else if (!MakeTokens(*this, Cpu, int32(3 * N.B()), N.Salt++, DT)) // a build of R(3B) = 4
		{
			return false;
		}
		if (!Root(*this, N, 3, true, L, LT, TEXT("L")))
		{
			return false;
		}
		const uint64 PHandle = Seam::EngineHandleOf(Cpu, P);
		const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
		if (!Request(*this, Cpu, ChildSpec(P, DT), Again, bRestore ? TEXT("nested restore") : TEXT("nested build")))
		{
			return false;
		}
		Cpu.ReturnPrefix(P); // only the lookup pin holds P now
		if (bRestore)
		{
			bOk &= ExpectLease(*this, Cpu, Again, ESuperSLMPrefixLeaseState::Ready, TEXT("the restore, admitted after evicting its parent"));
			bOk &= ExpectSource(*this, Cpu, Again, ESuperSLMPrefixSource::Disk, TEXT("the restore"));
			bOk &= TestEqual(TEXT("the restore ran private (shared pages 0)"), Seam::SharedPagesOf(Cpu, Again), int64(0));
			bOk &= TestEqual(TEXT("P was evicted"), Stats(Cpu).Evictions - S0.Evictions, int64(1));
		}
		else
		{
			bOk &= ExpectLease(*this, Cpu, Again, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("the build, which never evicts its parent"), 10.0);
			bOk &= TestEqual(TEXT("nothing evicted"), Stats(Cpu).Evictions, S0.Evictions);
			bOk &= TestEqual(TEXT("P not released"), Seam::ReleasesOf(Cpu, PHandle), 0);
		}
		bOk &= Physical(*this, Cpu, bRestore ? TEXT("after the restore") : TEXT("after the refusal"));
	}
	return bOk;
}

// §5.6 at U6: the nested-spec refusals. A parent that is not a live Ready lease, a nested spec naming a
// schema of its own, and a parent's length plus the suffix above the cap are refused at the request
// (no lease); NestedRequiresBudgetMode stays for legacy mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedRefusalsTest, "SuperSLM.PrefixCache.Cpu.Nested.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedRefusalsTest::RunTest(const FString& Parameters)
{
	FNestRig N;
	if (!ConfigureNestRig(*this, N, 8))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = N.Cpu();
	FSuperSLMPrefix P, Out;
	TArray<int32> PT, Suffix, TooLong;
	FString Error;
	if (!Root(*this, N, 2, true, P, PT, TEXT("P")) || !MakeTokens(*this, Cpu, int32(N.B()), N.Salt++, Suffix)
		|| !MakeTokens(*this, Cpu, int32(N.Rig.Geo.Cap) - PT.Num() + 1, N.Salt++, TooLong))
	{
		return false;
	}
	FSuperSLMPrefixSpec WithSchema = ChildSpec(P, Suffix);
	WithSchema.SchemaName = FName(TEXT("any"));
	bool bOk = TestFalse(TEXT("a nested spec naming a schema is refused"), Cpu.RequestPrefix(WithSchema, Out, Error));
	bOk &= TestFalse(TEXT("parent length plus suffix above the cap is refused"), Cpu.RequestPrefix(ChildSpec(P, TooLong), Out, Error));
	FSuperSLMPrefix Ended = P;
	Cpu.ReturnPrefix(P);
	bOk &= TestFalse(TEXT("an ended lease is no parent"), Cpu.RequestPrefix(ChildSpec(Ended, Suffix), Out, Error));

	// Legacy mode keeps the refusal by name. Legacy mode is SequenceBudget 0 (the fixture checks it): the
	// box's second run crashed the editor here on that check, passing kSequenceBudget. The budget is the
	// Ledger.Legacy cell's 8 pages, room for the 1-page parent at any build charge.
	bOk &= ConfigureCacheLegacy(*this, N.Rig, WithCache(MakeConfig(2, 0, 0, 1), BytesForPages(N.Rig.Geo, 8)));
	FSuperSLMPrefix LP;
	TArray<int32> LPT;
	bOk &= Root(*this, N, 1, true, LP, LPT, TEXT("legacy parent"));
	bOk &= TestFalse(TEXT("legacy: refused"), Cpu.RequestPrefix(ChildSpec(LP, Suffix), Out, Error));
	bOk &= TestTrue(TEXT("legacy: named NestedRequiresBudgetMode"), Error.Contains(TEXT("NestedRequiresBudgetMode")));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
