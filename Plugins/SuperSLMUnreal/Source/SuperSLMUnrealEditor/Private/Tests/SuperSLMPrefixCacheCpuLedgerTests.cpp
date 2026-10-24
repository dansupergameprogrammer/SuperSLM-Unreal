// Precache plan rev 1.17, CPU suite: the cache on paging's ledger, cell 8.14 (§5.11 (a) deferred-refund
// admission, §5.11 (b) region C). Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.Ledger.RegionC            8.14 region C at a non-multiple budget  [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.EvictionWindow     8.14 the eviction leg, the gated window  [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.Refused            8.14 refused, nothing evictable          [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.Waiters            8.14 two waiters, earmarked claims       [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.UnclaimedRefunds   8.14 (i) E/W/X, (ii) orphaned refund     [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.CoalesceOnWaiter   8.14 a second request for a waiting key  [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.StaleRelease       8.14 re-Configure() while a request waits [U2]
//   SuperSLM.PrefixCache.Cpu.Ledger.Legacy             8.14 legacy leg, 8.10's admission reading [U2]
//
// 8.14's nested legs are U6's and are not written here.
//
// Shape. The A-EX artifact (B = 64, a block of 64 pages; RequireAExShape). The cache budget is one
// page's bytes, which is not a multiple of the block bytes, so C = ceil(PageBytes / BlockBytes) x 64
// = 64 pages. A root build of L tokens is charged R(L) = ceil(L/B) + 1 while in flight and settles to
// ceil(L/B) once Ready, so an entry of L = jB tokens is resident at exactly j pages. Every reading is
// taken behind the lane-park barrier (paging's fill probe runs through RunOnPoolThread).
//
// Zero slack outside C and A (paging's form 3): S is charged in full at Configure (warm
// reservations), and P and H have capacity 0, so the only free charge is C's and A's. A is one
// CacheGhostBlocks block; its slack is allowed by the form, so the refund-at-decision mutant shows
// as form 1 red (drawn above charged by k - f) at the required reading, or as engine exhaustion.
//
// The one free page (box 2026-10-21). A build is charged R(L) = ceil(L/B) + 1 until it freezes, so the last
// build of a construction needs one page more than the construction settles at: a construction "filled
// exactly" by builds alone is not reachable, and the box run showed each such fill evicting its own victim
// (EvictionWindow read 3 free, UnclaimedRefunds 6 free with no eviction, CoalesceOnWaiter no waiter,
// Refused and StaleRelease never reaching their request). As the nested suite already does, each
// construction here settles with ONE page free, and each request the plan sizes against a full C is sized
// one page larger, so it takes that page and then claims exactly the refunds the plan names. The plan's
// readings are unchanged: the claims, the evictions, the waits, the admissions, form 1, and C never above
// its capacity. The host twins are ci/host/prefix_cache/test_box_regressions.cpp (A-EX geometry).
//
// Every request a leg makes builds by prefill (the disk tier is off), so "the build drew nothing" is
// read as the prefill-count diagnostic (BuildsIssued) unchanged and the paged PrefixBuild hold not
// reached; a U2 that routes cache builds through another job kind updates the hold kind here.

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

	FSuperSLMRuntimeConfig LedgerConfig()
	{
		return MakeConfig(2, kSequenceBudget, 0, 0); // CacheGhostBlocks 1: region A, one block
	}

	// Budget mode, two warm sequences, no 1.0.0 prefix region, no headroom: the only free charge is
	// C's (and A's, which the adopters fill).
	// bInit false re-configures a rig already initialized (a cell's second leg on a fresh configuration). Box
	// 2026-10-21 run 3: UnclaimedRefunds' leg (ii) called this again on the same rig, whose Init() then asked the
	// rig's FTestWorldWrapper for a second world while it still held the first ("could not create a game world").
	bool ConfigureLedgerRig(FAutomationTestBase& T, FPagingRig& Rig, bool bInit = true)
	{
		if (bInit && !Rig.Init(T))
		{
			return false;
		}
		// The page size is the engine's: read it under a cache-off configuration first.
		if (!ConfigureBudget(T, Rig, LedgerConfig()) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		const int64 Budget = Rig.Geo.PageBytes; // one page's bytes: not a multiple of the block bytes
		if (!T.TestTrue(TEXT("the budget is not a multiple of the block bytes"), Budget % (Rig.Geo.Block() * Rig.Geo.PageBytes) != 0))
		{
			return false;
		}
		return ConfigureCacheBudget(T, Rig, WithCache(LedgerConfig(), Budget));
	}

	int64 CapC(const USuperSLMSubsystem& Cpu) { return LedgerOf(Cpu).CapacityOf(ERegion::C); }

	// The tokens of a root entry resident at Pages pages (L = Pages x B).
	bool EntryTokens(FAutomationTestBase& T, FPagingRig& Rig, int64 Pages, int32 Salt, TArray<int32>& Out)
	{
		return MakeTokens(T, *Rig.Cpu, int32(Pages * Rig.Geo.B), Salt, Out);
	}

	// The tokens of a build charged exactly Need pages in flight: R(L) = Need with L = (Need - 1) x B.
	bool BuildTokens(FAutomationTestBase& T, FPagingRig& Rig, int64 Need, int32 Salt, TArray<int32>& Out)
	{
		check(Need >= 2);
		return MakeTokens(T, *Rig.Cpu, int32((Need - 1) * Rig.Geo.B), Salt, Out);
	}

	// Requests a resident entry; Ready; returned (unleased) unless bKeepLease.
	bool Resident(FAutomationTestBase& T, FPagingRig& Rig, const TArray<int32>& Tokens, bool bKeepLease, FSuperSLMPrefix& OutLease, const TCHAR* What)
	{
		if (!RequestReady(T, *Rig.Cpu, Spec(Tokens), OutLease, What))
		{
			return false;
		}
		if (!bKeepLease)
		{
			Rig.Cpu->ReturnPrefix(OutLease);
		}
		return DrainQuiet(*Rig.Cpu);
	}

	// C full (or with exactly Free pages free: the one free page, see the header), zero free charge
	// outside C and A (form 3), plus form 1.
	bool FullAndClean(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant, int64 Free = 0)
	{
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		bool bOk = Free == 0 ? T.TestEqual(*FString::Printf(TEXT("%s: C is full"), Instant), V.FreeOf(ERegion::C), int64(0))
							 : T.TestEqual(*FString::Printf(TEXT("%s: C has the construction's one free page"), Instant), V.FreeOf(ERegion::C), Free);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: zero slack outside C and A (S, P, H free 0)"), Instant),
			V.FreeOf(ERegion::S) + V.FreeOf(ERegion::P) + V.FreeOf(ERegion::H), int64(0));
		bOk &= Form1ChargesCoverDrawn(T, Cpu, Instant);
		bOk &= NoLedgerFault(T, Cpu, Instant);
		return bOk;
	}

	bool WithinC(FAutomationTestBase& T, const USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		return T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld (free charge never above C)"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C) && V.FreeOf(ERegion::C) <= V.CapacityOf(ERegion::C));
	}

	// Ticks, then settles the cache's pool-thread work (SettleCacheIo). Box 2026-10-21 (second run): the
	// fixed four ticks read the ledger before a request's key hash had been applied, so no eviction, waiter
	// or held release existed yet (EvictionWindow, UnclaimedRefunds, CoalesceOnWaiter, StaleRelease read 0
	// where 1 was expected, and Waiters hung). The host model applies commands synchronously.
	void Tick(USuperSLMSubsystem& Cpu, int32 Ticks = 4)
	{
		for (int32 I = 0; I < Ticks; ++I)
		{
			Cpu.Tick(Step);
		}
		SettleCacheIo(Cpu, 10.0);
	}
}

// Cell 8.14, region C (§5.11 (b)): the pool's C equals ceil(PrefixCacheBudgetBytes / block bytes) x
// P_blk, computed in the test, at a budget that is not a multiple of the block bytes.
// Red until U2: C is 0 (the CPU subsystem sizes LedgerConfig.CacheRegionPages = 0).
// Mutant: floor for ceil -> C = 0 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuRegionCTest, "SuperSLM.PrefixCache.Cpu.Ledger.RegionC",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuRegionCTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("C is one block of 64 pages (ceil, not floor)"), CapC(*Rig.Cpu), Rig.Geo.Block());
	// A second non-multiple: one block's bytes plus one page's -> two blocks.
	const int64 Budget2 = (Rig.Geo.Block() + 1) * Rig.Geo.PageBytes;
	bOk &= ConfigureCacheBudget(*this, Rig, WithCache(LedgerConfig(), Budget2));
	bOk &= TestEqual(TEXT("one block and one page's bytes -> two blocks"), CapC(*Rig.Cpu), 2 * Rig.Geo.Block());
	bOk &= TestEqual(TEXT("the cache's budget is C's capacity in bytes"), Rig.Cpu->GetPrefixCacheStats().BudgetBytes, CapC(*Rig.Cpu) * Rig.Geo.PageBytes);
	return bOk;
}

// Cell 8.14, the eviction leg (§5.11 (a)). E has L = 2B + 1 (k = 3 pages in C; its two adopters
// pre-charge A its f = 2 full pages). C is filled exactly: E (LRU), then F (1 page), then a filler,
// all unleased, settling with one page free (see the header: the filler is C - 5, not C - 4). A build
// charged 4 (L = 3B) takes the free page and fits only with E's refund of 3. E's release is held at the
// batch gate; the required reading is taken between the eviction decision and the release's apply.
// Red until U2: no cache, no region C, no deferred refund.
// Mutants: C refunded at the decision (the build draws while E's private page is held: physical
// exceeds the charges by k - f = 1 under zero slack -> engine exhaustion, LedgerFaults or form 1
// red); AdoptPrefix bypassing the one adopt entry point (A never pre-charged -> the same); the loop
// ignoring the request's pending refunds (evicts F too -> two evictions, F not a Memory hit).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuEvictionWindowTest, "SuperSLM.PrefixCache.Cpu.Ledger.EvictionWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuEvictionWindowTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int64 B = Rig.Geo.B;
	TArray<int32> ETokens, FTokens, FillTokens, WTokens, Suffix;
	if (!MakeTokens(*this, Cpu, int32(2 * B + 1), 1, ETokens) || !EntryTokens(*this, Rig, 1, 2, FTokens)
		|| !EntryTokens(*this, Rig, CapC(Cpu) - 3 - 1 - 1, 3, FillTokens) || !BuildTokens(*this, Rig, 4, 4, WTokens) || !MakeTokens(*this, Cpu, SuffixTokens, 5, Suffix))
	{
		return false;
	}

	// E, adopted twice through AdoptPrefix (the one adopt entry point), then returned.
	FSuperSLMPrefix E, F, Fill, W;
	FSuperSLMSequence Adopter[2];
	if (!RequestReady(*this, Cpu, Spec(ETokens), E, TEXT("E")))
	{
		return false;
	}
	for (FSuperSLMSequence& S : Adopter)
	{
		if (!Vend(*this, Cpu, S) || !AdoptAndWait(*this, Cpu, S, E))
		{
			return false;
		}
	}
	bool bOk = TestEqual(TEXT("E is charged k = 3 pages of C"), Charged(Cpu, ERegion::C), int64(3));
	bOk &= TestEqual(TEXT("E's adopters pre-charge A its f = 2 full pages"), Charged(Cpu, ERegion::A), int64(2));
	Cpu.ReturnPrefix(E);
	if (!DrainQuiet(Cpu) || !Resident(*this, Rig, FTokens, false, F, TEXT("F")) || !Resident(*this, Rig, FillTokens, false, Fill, TEXT("filler")))
	{
		return false;
	}
	bOk &= FullAndClean(*this, Cpu, TEXT("C filled (E 3, F 1, the filler C - 5)"), 1);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);

	// The request, with E's release held and the build's start held.
	Seam::HoldReleases(Cpu, true);
	const FSuperSLMPagedHold BuildHold = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::PrefixBuild);
	if (!Request(*this, Cpu, Spec(WTokens), W, TEXT("W")))
	{
		return false;
	}
	Tick(Cpu);
	DrainQuiet(Cpu, 5.0);

	// The required reading: between E's eviction decision and its release's apply.
	{
		const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
		const TCHAR* Instant = TEXT("the window (decided, not applied)");
		const bool bWindow = TestEqual(TEXT("the window exists: E's release is held at the gate"), Seam::HeldReleaseCount(Cpu), 1);
		if (!bWindow)
		{
			AddError(TEXT("8.14 requires the reading between the eviction decision and the release's apply; a later reading cannot substitute"));
			Seam::HoldReleases(Cpu, false);
			return false;
		}
		bOk &= TestEqual(TEXT("window: exactly one eviction decided"), S1.Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("window: E is out of the map"), S1.ResidentEntries, S0.ResidentEntries - 1);
		bOk &= TestEqual(TEXT("window: C still carries E's k = 3"), Charged(Cpu, ERegion::C), CapC(Cpu));
		bOk &= TestEqual(TEXT("window: W waits"), (uint8)Cpu.GetPrefixLeaseState(W), (uint8)ESuperSLMPrefixLeaseState::Pending);
		bOk &= TestEqual(TEXT("window: one waiter"), S1.Waiters, 1);
		bOk &= TestEqual(TEXT("window: W claims all of E's refund"), S1.ClaimedRefundBytes, 3 * Rig.Geo.PageBytes);
		bOk &= TestFalse(TEXT("window: W's begin job is not dispatched (drew nothing)"), FSuperSLMPagedTestSeam::IsHoldReached(Cpu, BuildHold));
		bOk &= TestEqual(TEXT("window: no build issued"), S1.BuildsIssued, S0.BuildsIssued);
		bOk &= FullAndClean(*this, Cpu, Instant);
	}

	// Open the gate: at E's apply W is admitted with its charge complete, and held at its start.
	Seam::HoldReleases(Cpu, false);
	bOk &= TestTrue(TEXT("W's build reaches its start hold (admitted at E's apply)"), WaitHoldReached(Cpu, BuildHold));
	{
		const FSuperSLMPrefixCacheStats S2 = Stats(Cpu);
		bOk &= TestEqual(TEXT("admitted: the ledger total is unchanged by the transfer"), Charged(Cpu, ERegion::C), CapC(Cpu));
		bOk &= TestEqual(TEXT("admitted: W's charge is its R(L) = 4"), S2.InFlightBytes, Rig.Geo.R(WTokens.Num()) * Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("admitted: no refund pending"), S2.PendingRefunds, 0);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("admitted, before the draw"));
	}
	FSuperSLMPagedTestSeam::ReleaseHold(Cpu, BuildHold);
	bOk &= ExpectLease(*this, Cpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W"));
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("W drawn"));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("W drawn"));

	// Exactly one eviction, and F is still a Memory hit.
	const FSuperSLMPrefixCacheStats S3 = Stats(Cpu);
	bOk &= TestEqual(TEXT("exactly one eviction (the eviction diagnostic)"), S3.Evictions - S0.Evictions, int64(1));
	FSuperSLMPrefix F2;
	bOk &= RequestReady(*this, Cpu, Spec(FTokens), F2, TEXT("F again"));
	bOk &= ExpectSource(*this, Cpu, F2, ESuperSLMPrefixSource::Memory, TEXT("F after W's admission"));
	bOk &= TestEqual(TEXT("F's hit issues no build"), Stats(Cpu).BuildsIssued, S3.BuildsIssued);

	// E's adopters keep generating after E's release (A holds their pages).
	TArray<int32> Tokens;
	for (const FSuperSLMSequence& S : Adopter)
	{
		bOk &= GenerateComplete(*this, Cpu, S, Suffix, TwinNewTokens, Tokens);
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the end"));
	return bOk;
}

// Cell 8.14, refused: every entry leased and no eviction pending -> BudgetExhausted, nothing drawn. A 3 and
// B C - 4 (B's build peaks at C - 3, which fits beside A; the box's C - 3 peaked one page over and was itself
// refused), one page free, W charged 3.
// Red until U2. Mutant: admission over budget (a draw without a charge) -> drawn changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuRefusedTest, "SuperSLM.PrefixCache.Cpu.Ledger.Refused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuRefusedTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> A, Bt, W;
	FSuperSLMPrefix LA, LB, LW;
	if (!EntryTokens(*this, Rig, 3, 1, A) || !EntryTokens(*this, Rig, CapC(Cpu) - 4, 2, Bt) || !BuildTokens(*this, Rig, 3, 3, W)
		|| !Resident(*this, Rig, A, true, LA, TEXT("A")) || !Resident(*this, Rig, Bt, true, LB, TEXT("B")))
	{
		return false;
	}
	bool bOk = FullAndClean(*this, Cpu, TEXT("every entry leased"), 1);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	if (!Request(*this, Cpu, Spec(W), LW, TEXT("W")))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Cpu, LW, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("W"), 10.0);
	const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
	bOk &= TestEqual(TEXT("nothing evicted"), S1.Evictions, S0.Evictions);
	bOk &= TestEqual(TEXT("no waiter"), S1.Waiters, 0);
	bOk &= TestEqual(TEXT("no build issued"), S1.BuildsIssued, S0.BuildsIssued);
	bOk &= FullAndClean(*this, Cpu, TEXT("after the refusal (nothing drawn)"), 1);
	return bOk;
}

// Cell 8.14, waiters (§5.11 (a); rev 1.15 re-check §4). Two unleased victims V1, V2 (LRU first) of 3 pages
// each, the rest C - 7 leased, one page free (see the header); two requests for distinct keys in one tick:
// X1 needs 4 (the free page and V1's 3), X2 needs exactly 3 (V2's 3, equal to the first victim's charge).
// Each claims its own victim (two evictions); each is admitted at its own victim's apply; C never
// exceeds its capacity. Releases are dispatched one at a time.
// Mutant: refunds not earmarked (both count V1's) -> the second stops without evicting -> one
// eviction -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuWaitersTest, "SuperSLM.PrefixCache.Cpu.Ledger.Waiters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuWaitersTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> V1, V2, Rest, X1, X2;
	FSuperSLMPrefix LV1, LV2, LRest, R1, R2;
	if (!EntryTokens(*this, Rig, 3, 1, V1) || !EntryTokens(*this, Rig, 3, 2, V2) || !EntryTokens(*this, Rig, CapC(Cpu) - 7, 3, Rest)
		|| !BuildTokens(*this, Rig, 4, 4, X1) || !BuildTokens(*this, Rig, 3, 5, X2)
		|| !Resident(*this, Rig, V1, false, LV1, TEXT("V1")) || !Resident(*this, Rig, V2, false, LV2, TEXT("V2")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("rest")))
	{
		return false;
	}
	bool bOk = FullAndClean(*this, Cpu, TEXT("C full but one page, two victims"), 1);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	Seam::HoldReleases(Cpu, true);
	if (!Request(*this, Cpu, Spec(X1), R1, TEXT("X1")) || !Request(*this, Cpu, Spec(X2), R2, TEXT("X2")))
	{
		return false;
	}
	Tick(Cpu);
	bOk &= TestEqual(TEXT("two evictions (each request claims its own victim)"), Stats(Cpu).Evictions - S0.Evictions, int64(2));
	const bool bTwoHeld = TestEqual(TEXT("two releases held"), Seam::HeldReleaseCount(Cpu), 2);
	bOk &= TestEqual(TEXT("two waiters"), Stats(Cpu).Waiters, 2);
	bOk &= WithinC(*this, Cpu, TEXT("both waiting"));
	if (!bTwoHeld)
	{
		// Box 2026-10-21 (second run): with no release held, X1 can never leave Pending under the gate,
		// and the drain below ran to its 900 s bound (the hang). Open the gate and stop.
		Seam::HoldReleases(Cpu, false);
		DrainQuiet(Cpu, 10.0);
		return false;
	}

	bOk &= TestTrue(TEXT("V1's release dispatched"), Seam::DispatchHeldRelease(Cpu, 0));
	Drain(Cpu, [&Cpu, &R1]() { return Cpu.GetPrefixLeaseState(R1) != ESuperSLMPrefixLeaseState::Pending; }, LeaseSeconds);
	SettleCacheIo(Cpu, 10.0);
	bOk &= TestEqual(TEXT("X1 is admitted at V1's apply (and builds)"), (uint8)Cpu.GetPrefixLeaseState(R1), (uint8)ESuperSLMPrefixLeaseState::Ready);
	bOk &= TestEqual(TEXT("X2 still waits on V2's release"), (uint8)Cpu.GetPrefixLeaseState(R2), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= WithinC(*this, Cpu, TEXT("after V1's apply"));
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after V1's apply"));

	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, R2, ESuperSLMPrefixLeaseState::Ready, TEXT("X2 at V2's apply"));
	bOk &= WithinC(*this, Cpu, TEXT("both admitted"));
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("both admitted"));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("both admitted"));
	bOk &= TestEqual(TEXT("still exactly two evictions"), Stats(Cpu).Evictions - S0.Evictions, int64(2));
	return bOk;
}

// Cell 8.14, unclaimed refunds (rev 1.15, re-check R2). Both legs with releases gated.
// (i) E/W/X: C full, E the only unleased entry. W evicts E, claims part of E's refund and waits; X in
// the same tick claims the unclaimed rest and waits with no further eviction; at E's apply both are
// admitted; one eviction. The plan's E = 3, W = 2, X = 1 is scaled x2 here (E = 6, W = 4, X = 2):
// a plugin root build is charged R(L) >= 2, so no build needs 1 page (a reading for the planner). With the
// construction's one free page (the header), W needs 5: the free page and 4 of E's 6.
// (ii) Orphaned refund: E (3) the only unleased entry, one page free. Y needs 5 (the plan's 4, plus the
// free page): evicts E, still short, resolves BudgetExhausted; E's eviction stands, its refund unclaimed. Z
// needs 4 (the plan's 3, plus the free page) before E's apply: claims E's refund, waits, is admitted at E's
// apply.
// Mutant: a loop that cannot claim unclaimed refunds (rev 1.14's rule) -> X refused in (i), Z in (ii).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuUnclaimedRefundsTest, "SuperSLM.PrefixCache.Cpu.Ledger.UnclaimedRefunds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuUnclaimedRefundsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;

	// (i) E/W/X.
	{
		TArray<int32> E, Rest, W, X;
		FSuperSLMPrefix LE, LRest, LW, LX;
		if (!EntryTokens(*this, Rig, 6, 1, E) || !EntryTokens(*this, Rig, CapC(Cpu) - 7, 2, Rest) || !BuildTokens(*this, Rig, 5, 3, W) || !BuildTokens(*this, Rig, 2, 4, X)
			|| !Resident(*this, Rig, E, false, LE, TEXT("(i) E")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("(i) rest")))
		{
			return false;
		}
		bOk &= FullAndClean(*this, Cpu, TEXT("(i) C full but one page"), 1);
		const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
		Seam::HoldReleases(Cpu, true);
		if (!Request(*this, Cpu, Spec(W), LW, TEXT("(i) W")) || !Request(*this, Cpu, Spec(X), LX, TEXT("(i) X")))
		{
			return false;
		}
		Tick(Cpu);
		const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
		bOk &= TestEqual(TEXT("(i) exactly one eviction (E)"), S1.Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("(i) two waiters"), S1.Waiters, 2);
		bOk &= TestEqual(TEXT("(i) E's refund fully claimed (W 4, X 2)"), S1.ClaimedRefundBytes, 6 * Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("(i) nothing unclaimed"), S1.UnclaimedRefundBytes, int64(0));
		bOk &= WithinC(*this, Cpu, TEXT("(i) waiting"));
		Seam::HoldReleases(Cpu, false);
		bOk &= ExpectLease(*this, Cpu, LW, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) W at E's apply"));
		bOk &= ExpectLease(*this, Cpu, LX, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) X at E's apply"));
		bOk &= TestEqual(TEXT("(i) still one eviction"), Stats(Cpu).Evictions - S0.Evictions, int64(1));
		bOk &= WithinC(*this, Cpu, TEXT("(i) admitted"));
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(i)"));
	}

	// (ii) Orphaned refund, on a fresh configuration of the same rig (its world and subsystem).
	if (!ConfigureLedgerRig(*this, Rig, /*bInit*/ false))
	{
		return false;
	}
	{
		TArray<int32> E, Rest, Y, Z;
		FSuperSLMPrefix LE, LRest, LY, LZ;
		if (!EntryTokens(*this, Rig, 3, 11, E) || !EntryTokens(*this, Rig, CapC(Cpu) - 4, 12, Rest) || !BuildTokens(*this, Rig, 5, 13, Y) || !BuildTokens(*this, Rig, 4, 14, Z)
			|| !Resident(*this, Rig, E, false, LE, TEXT("(ii) E")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("(ii) rest")))
		{
			return false;
		}
		bOk &= FullAndClean(*this, Cpu, TEXT("(ii) C full but one page"), 1);
		const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
		Seam::HoldReleases(Cpu, true);
		if (!Request(*this, Cpu, Spec(Y), LY, TEXT("(ii) Y")))
		{
			return false;
		}
		bOk &= ExpectLease(*this, Cpu, LY, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("(ii) Y"), 10.0);
		bOk &= TestEqual(TEXT("(ii) E's eviction stands"), Stats(Cpu).Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("(ii) E's refund is unclaimed"), Stats(Cpu).UnclaimedRefundBytes, 3 * Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("(ii) E's release is still held"), Seam::HeldReleaseCount(Cpu), 1);
		if (!Request(*this, Cpu, Spec(Z), LZ, TEXT("(ii) Z")))
		{
			return false;
		}
		Tick(Cpu);
		bOk &= TestEqual(TEXT("(ii) Z waits on E's refund (not refused)"), (uint8)Cpu.GetPrefixLeaseState(LZ), (uint8)ESuperSLMPrefixLeaseState::Pending);
		bOk &= TestEqual(TEXT("(ii) Z claims E's refund"), Stats(Cpu).ClaimedRefundBytes, 3 * Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("(ii) no further eviction"), Stats(Cpu).Evictions - S0.Evictions, int64(1));
		Seam::HoldReleases(Cpu, false);
		bOk &= ExpectLease(*this, Cpu, LZ, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) Z at E's apply"));
		bOk &= WithinC(*this, Cpu, TEXT("(ii) admitted"));
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(ii)"));
	}
	return bOk;
}

// Cell 8.14, a second request for a waiting key attaches to the waiter: one build (the prefill-count
// seam up by one), both handles Ready at the apply. E 3, the rest C - 4, one page free (the header); W
// needs 4, so it takes the free page, evicts E and waits.
// Mutant: a coalesced request charged again -> a second eviction or a second build -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuCoalesceOnWaiterTest, "SuperSLM.PrefixCache.Cpu.Ledger.CoalesceOnWaiter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuCoalesceOnWaiterTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> E, Rest, W;
	FSuperSLMPrefix LE, LRest, W1, W2;
	if (!EntryTokens(*this, Rig, 3, 1, E) || !EntryTokens(*this, Rig, CapC(Cpu) - 4, 2, Rest) || !BuildTokens(*this, Rig, 4, 3, W)
		|| !Resident(*this, Rig, E, false, LE, TEXT("E")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("rest")))
	{
		return false;
	}
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	Seam::HoldReleases(Cpu, true);
	if (!Request(*this, Cpu, Spec(W), W1, TEXT("W first")))
	{
		return false;
	}
	Tick(Cpu);
	if (!Request(*this, Cpu, Spec(W), W2, TEXT("W second")))
	{
		return false;
	}
	// The second request's hash lands a tick later too: settle before reading the waiter count.
	SettleCacheIo(Cpu, 10.0);
	bool bOk = TestNotEqual(TEXT("two leases are two handles"), W1, W2);
	bOk &= TestEqual(TEXT("one waiter"), Stats(Cpu).Waiters, 1);
	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, W1, ESuperSLMPrefixLeaseState::Ready, TEXT("W first"));
	bOk &= ExpectLease(*this, Cpu, W2, ESuperSLMPrefixLeaseState::Ready, TEXT("W second"));
	const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
	bOk &= TestEqual(TEXT("one build (the prefill count up by one)"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
	bOk &= TestEqual(TEXT("one eviction"), S1.Evictions - S0.Evictions, int64(1));
	bOk &= WithinC(*this, Cpu, TEXT("the end"));
	return bOk;
}

// Cell 8.14, re-Configure() while a request waits (rev 1.16 strike L1). The waiter resolves
// Cancelled by name as 1.7's queued request does, nothing is drawn, the pool destroy returns SSLM_OK.
// The waited-on release completes during the teardown and its completion reaches the rebuilt cache
// after the Configure(): at the next apply pass it is dropped (stale generation), and the rebuilt
// ledger, read after that pass, holds exactly the new configuration's charges (C charged 0), with
// free charge never above C.
// E 5, the rest C - 6, one page free (the header); W needs 5: the free page and 4 of E's 5 (the strike's
// "W claiming 4").
// Mutant: a stale completion applied -> free charge above C -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuStaleReleaseTest, "SuperSLM.PrefixCache.Cpu.Ledger.StaleRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuStaleReleaseTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!ConfigureLedgerRig(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> E, Rest, W;
	FSuperSLMPrefix LE, LRest, LW;
	if (!EntryTokens(*this, Rig, 5, 1, E) || !EntryTokens(*this, Rig, CapC(Cpu) - 6, 2, Rest) || !BuildTokens(*this, Rig, 5, 3, W)
		|| !Resident(*this, Rig, E, false, LE, TEXT("E")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("rest")))
	{
		return false;
	}
	Seam::HoldReleases(Cpu, true);
	if (!Request(*this, Cpu, Spec(W), LW, TEXT("W")))
	{
		return false;
	}
	Tick(Cpu);
	bool bOk = TestEqual(TEXT("W waits on E's release"), Stats(Cpu).Waiters, 1);

	// The teardown runs with the release still gated; Configure() opens every gate before the
	// pool is destroyed, so E's release runs in the teardown.
	Seam::ArmTeardownProbe(Cpu);
	if (!ConfigureCacheBudget(*this, Rig, Rig.Config))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the pool destroy returns SSLM_OK"), Seam::LastPoolDestroyStatus(Cpu), int32(SSLM_OK));
	bOk &= TestEqual(TEXT("W resolves by name"), (uint8)Cpu.GetPrefixLeaseState(LW), (uint8)ESuperSLMPrefixLeaseState::Cancelled);
	DrainQuiet(Cpu);
	const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
	bOk &= TestEqual(TEXT("the stale release completion was dropped at the apply pass"), S1.StaleCompletionsDropped, int64(1));
	bOk &= TestEqual(TEXT("the rebuilt ledger holds only the new configuration's charges (C charged 0)"), Charged(Cpu, ERegion::C), int64(0));
	bOk &= TestEqual(TEXT("the cache's own ledger is empty"), S1.ChargedBytes, int64(0));
	bOk &= WithinC(*this, Cpu, TEXT("after the stale completion"));
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after the stale completion"));
	return bOk;
}

// Cell 8.14, legacy mode: the same eviction-then-build sequence keeps 8.10's admission reading (the
// cache's pages never above the budget's, F_min admitted at every reading).
// E 3, the rest 4, one page free (the header), W needs 4. Box 2026-10-21: the probe's initial reading was
// bounded by LedgerOf().PoolPages, which is 0 in legacy mode, so it read 2 (F_min -6); InitialFree now
// bounds it by ProbeCeiling().
// Red until U2. Mutant: refund at the decision -> the build draws early -> F_min refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuLegacyLedgerTest, "SuperSLM.PrefixCache.Cpu.Ledger.Legacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuLegacyLedgerTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(2, 0, 0, 1)) || !ReadGeometry(*this, Rig) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	const int64 BudgetPagesN = 8;
	const int64 Budget = BytesForPages(Rig.Geo, BudgetPagesN);
	if (!ConfigureCacheLegacy(*this, Rig, WithCache(MakeConfig(2, 0, 0, 1), Budget)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	int64 FreeAtStart = -1;
	if (!InitialFree(*this, Cpu, FreeAtStart))
	{
		return false;
	}
	const int64 Pool = FreeAtStart; // nothing but the warm reservations is drawn at the start
	const int64 ReserveSeq = 0;     // the probe's reading already excludes the warm reservations
	TArray<int32> E, Rest, W;
	FSuperSLMPrefix LE, LRest, LW;
	if (!EntryTokens(*this, Rig, 3, 1, E) || !EntryTokens(*this, Rig, BudgetPagesN - 4, 2, Rest) || !BuildTokens(*this, Rig, 4, 3, W)
		|| !Resident(*this, Rig, E, false, LE, TEXT("E")) || !Resident(*this, Rig, Rest, true, LRest, TEXT("rest")))
	{
		return false;
	}
	bool bOk = CacheWithinBudget(*this, Cpu, Pool, ReserveSeq, 0, BudgetPagesN, TEXT("full"));
	Seam::HoldReleases(Cpu, true);
	if (!Request(*this, Cpu, Spec(W), LW, TEXT("W")))
	{
		return false;
	}
	Tick(Cpu);
	bOk &= CacheWithinBudget(*this, Cpu, Pool, ReserveSeq, 0, BudgetPagesN, TEXT("the window"));
	Seam::HoldReleases(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, LW, ESuperSLMPrefixLeaseState::Ready, TEXT("W"));
	bOk &= CacheWithinBudget(*this, Cpu, Pool, ReserveSeq, 0, BudgetPagesN, TEXT("W built"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
