// Precache plan rev 1.18, the U6 suite's GPU twins of cell 8.14's U2 legs: the GPU cache on the GPU page ledger (§5.11 (a)
// deferred-refund admission, §5.11 (b) region C), plus 8.14's nested-A leg (Q6, the U6 draft's R36) on the CPU cache.
// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on. The CPU twins of the ledger legs are in
// SuperSLMPrefixCacheCpuLedgerTests.cpp; the GPU nested-A leg (Gpu.Nested.RegionA) is in SuperSLMPrefixCacheGpuNestedTests.cpp.
//
//   SuperSLM.PrefixCache.Gpu.Ledger.RegionC            8.14 region C at a non-multiple budget   [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.EvictionWindow     8.14 the eviction leg, the gated window   [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.Refused            8.14 refused, nothing evictable           [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.Waiters            8.14 two waiters, earmarked claims        [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.UnclaimedRefunds   8.14 (i) E/W/X, (ii) orphaned refund      [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.CoalesceOnWaiter   8.14 a second request for a waiting key   [U6]
//   SuperSLM.PrefixCache.Gpu.Ledger.StaleRelease       8.14 re-Configure() while a request waits [U6]
//   SuperSLM.PrefixCache.Cpu.Nested.RegionA            8.14 nested-A (Q6): A for a nested parent  [U6]
//
// Cpu.Ledger.Legacy has no GPU twin: the GPU cache is pooled (budget mode) only, so there is no legacy mode to read.
//
// Shape. A-EX on the GPU (B = 16, a block of P_blk = 256 pages; RequireAExShape): two pooled sequences of budget 512,
// no region P, no headroom (zero slack outside C and A, paging's form 3: S is charged in full at Configure), and
// CacheGhostBlocks 1 (region A). The cache budget is one page's bytes, not a multiple of the block bytes, so C = one
// block of 256 pages. Each construction names its own C (4 to 7 pages): a leased root "pad" of CapC - C pages takes the
// rest (one ~250-page build per configuration, the slow part of each cell), so the construction sees exactly C free.
// The CPU suite's "rest, leased" is that pad. A root build of L tokens is charged R(L) = ceil(L/B) + 1 while in flight
// and settles to ceil(L/B), so each construction here settles with ONE page free (the CPU file's header, "the one free
// page") and each request the plan sizes against a full C is sized one page larger.
//
// Readings. The GPU cache's key hashes run on a pool thread and land at a later Tick(); a reading after a fixed number
// of ticks races them (the CPU box run of 2026-10-21). Every reading here is taken after SettleGpu (the GPU twin of the
// CPU fixture's SettleCacheIo: the cache IO settled on two consecutive ticks, the submission thread's jobs drained, and
// the IO settled again). A held release is not posted, so it is not outstanding work; a held build step is re-posted
// (it never blocks the submission thread), so no job drain is run while the build hold is set.
//
// GPU differences from the CPU legs, each said at its cell: EvictionWindow reads "W's build drew nothing" as no build
// issued, none in flight and none at the build hold (the GPU has no paged HoldStart); its admission is read at the
// cache's build hold (SetBuildHold). StaleRelease reads the GPU's own teardown contract: the GPU cache ends with its
// configuration, so a waiter's lease reads Invalid after the re-Configure() (not Cancelled) and no completion of the
// old configuration can reach the new cache (StaleCompletionsDropped stays 0 on the new host); the CPU's pool-destroy
// probe (ArmTeardownProbe / LastPoolDestroyStatus) is not on the GPU, and the re-Configure()'s success stands for it.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "SuperSLMPrefixCacheTestSeam.h"

namespace PCF = SuperSLMPrefixCacheFixtures;

namespace
{
	using namespace SuperSLMPagingGpuFixtures;
	using Seam = FSuperSLMPrefixCacheTestSeam;
	using ERegion = ESuperSLMPagedRegion;

	struct FGpuLedgerRig
	{
		FGpuPagingRig Rig;
		FSuperSLMPrefix Pad;
		int64 PadPages = 0;
		int32 Salt = 300;
		USuperSLMGpuSubsystem& Gpu() { return *Rig.Gpu; }
		int64 B() const { return Rig.Geo.B; }
	};

	int64 GpuLedgerCapC(const USuperSLMGpuSubsystem& Gpu) { return LedgerOf(Gpu).CapacityOf(ERegion::C); }

	// Two pooled sequences of budget 512, no region P, no headroom: the only free charge is C's and A's.
	FSuperSLMGpuRuntimeConfig GpuLedgerConfig(const FGpuPagingRig& Rig)
	{
		return MakeGpuConfig(Rig.Artifact, 2, 512, /*PrefixBlocks*/ 0, /*HeadroomBlocks*/ 0);
	}

	// Paging's P/A block in bytes: P_blk pages.
	int64 GpuLedgerBlockBytes(const FGpuPagingRig& Rig) { return Rig.Geo.Pblk() * Rig.Geo.PageBytes; }

	// Pooled, then the cache at one page's bytes (applied at the next Configure()), then pooled again; then a leased
	// pad so the construction sees exactly CPages of C free (no pad for CPages <= 0). bInit false re-configures a rig
	// already initialized (a cell's second leg on a fresh configuration of the same world and subsystem).
	bool ConfigureGpuLedger(FAutomationTestBase& T, FGpuLedgerRig& N, int64 CPages, bool bInit = true)
	{
		FGpuPagingRig& Rig = N.Rig;
		N.Pad = FSuperSLMPrefix();
		N.PadPages = 0;
		if ((bInit && !Rig.Init(T)) || !ConfigurePooled(T, Rig, GpuLedgerConfig(Rig)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		const int64 Budget = Rig.Geo.PageBytes; // one page's bytes: not a multiple of the block bytes
		if (!T.TestTrue(TEXT("the budget is not a multiple of the block bytes"), Budget % GpuLedgerBlockBytes(Rig) != 0))
		{
			return false;
		}
		FSuperSLMGpuPrefixCacheSettings Settings;
		Settings.BudgetBytes = Budget;
		Rig.Gpu->ConfigurePrefixCache(Settings);
		if (!ConfigurePooled(T, Rig, GpuLedgerConfig(Rig)))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S = Rig.Gpu->GetPrefixCacheStats();
		if (!T.TestTrue(TEXT("the GPU cache is configured, on region C"), S.bConfigured && S.bBudgetMode))
		{
			return false;
		}
		if (CPages <= 0)
		{
			return true;
		}
		const int64 PadPages = GpuLedgerCapC(*Rig.Gpu) - CPages;
		if (PadPages <= 0)
		{
			return T.TestEqual(TEXT("the construction's C is the region's"), PadPages, int64(0));
		}
		TArray<int32> PadTokens;
		if (!MakeTokens(T, *Rig.Model, int32(PadPages * Rig.Geo.B), N.Salt++, PadTokens))
		{
			return false;
		}
		FString Error;
		const bool bRequested = Rig.Gpu->RequestPrefix(PCF::Spec(PadTokens), N.Pad, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("the pad is requested (%s)"), *Error), bRequested))
		{
			return false;
		}
		Pump(*Rig.Gpu, [&Rig, &N]() { return Rig.Gpu->GetPrefixLeaseState(N.Pad) != ESuperSLMPrefixLeaseState::Pending; }, BuildSeconds);
		if (!T.TestEqual(TEXT("the pad is Ready"), (uint8)Rig.Gpu->GetPrefixLeaseState(N.Pad), (uint8)ESuperSLMPrefixLeaseState::Ready))
		{
			return false;
		}
		N.PadPages = PadPages;
		return DrainQuiet(*Rig.Gpu);
	}

	// One IO phase: ticks (paced on the device) until the cache's pool-thread IO and its inbox are settled on two
	// consecutive ticks. Counts ticks itself: Pump re-evaluates its predicate without ticking while gated.
	bool SettleGpuPhase(USuperSLMGpuSubsystem& Gpu, double Seconds)
	{
		const double Start = FPlatformTime::Seconds();
		int32 QuietTicks = 0;
		while (QuietTicks < 2 && FPlatformTime::Seconds() - Start < Seconds)
		{
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu.Tick(Step);
			if (Seam::IsCacheIoSettled(Gpu))
			{
				++QuietTicks;
			}
			else
			{
				QuietTicks = 0;
				FPlatformProcess::Sleep(0.001f);
			}
		}
		return QuietTicks >= 2;
	}

	// The GPU twin of PCF::SettleCacheIo: IO settled, then (bDrainJobs) every submission-thread job drained, then IO
	// settled again (an applied hash posts its build, a delivered job can issue IO).
	bool SettleGpu(USuperSLMGpuSubsystem& Gpu, bool bDrainJobs = true, double Seconds = OpSeconds)
	{
		const bool bFirst = SettleGpuPhase(Gpu, Seconds);
		if (!bDrainJobs)
		{
			return bFirst;
		}
		DrainQuiet(Gpu, Seconds);
		return SettleGpuPhase(Gpu, Seconds) && bFirst;
	}

	bool Request(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Gpu.RequestPrefix(Spec, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: RequestPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: a handle is minted"), What), Out.IsValid());
	}

	ESuperSLMPrefixLeaseState WaitLease(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, double Seconds = BuildSeconds)
	{
		Pump(Gpu, [&Gpu, &Lease]() { return Gpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, Seconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool ExpectLease(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixLeaseState Want, const TCHAR* What,
		double Seconds = BuildSeconds)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: the lease resolves by name"), What), (uint8)WaitLease(Gpu, Lease, Seconds), (uint8)Want);
	}

	bool IsPending(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease)
	{
		return Gpu.GetPrefixLeaseState(Lease) == ESuperSLMPrefixLeaseState::Pending;
	}

	// The tokens of a root entry resident at Pages pages (L = Pages x B).
	bool EntryTokens(FAutomationTestBase& T, FGpuLedgerRig& N, int64 Pages, TArray<int32>& Out)
	{
		return MakeTokens(T, *N.Rig.Model, int32(Pages * N.B()), N.Salt++, Out);
	}

	// The tokens of a build charged exactly Need pages in flight: R(L) = Need with L = (Need - 1) x B.
	bool BuildTokens(FAutomationTestBase& T, FGpuLedgerRig& N, int64 Need, TArray<int32>& Out)
	{
		check(Need >= 2);
		return MakeTokens(T, *N.Rig.Model, int32((Need - 1) * N.B()), N.Salt++, Out);
	}

	// Requests a resident entry; Ready; returned (unleased) unless bKeepLease.
	bool Resident(FAutomationTestBase& T, FGpuLedgerRig& N, const TArray<int32>& Tokens, bool bKeepLease, FSuperSLMPrefix& OutLease, const TCHAR* What)
	{
		if (!Request(T, N.Gpu(), PCF::Spec(Tokens), OutLease, What) || !ExpectLease(T, N.Gpu(), OutLease, ESuperSLMPrefixLeaseState::Ready, What))
		{
			return false;
		}
		if (!bKeepLease)
		{
			N.Gpu().ReturnPrefix(OutLease);
		}
		return DrainQuiet(N.Gpu());
	}

	// C full (or with exactly Free pages free: the one free page), zero free charge outside C and A (form 3), form 1.
	bool FullAndClean(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant, int64 FreePages = 0)
	{
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		bool bOk = FreePages == 0 ? T.TestEqual(*FString::Printf(TEXT("%s: C is full"), Instant), V.FreeOf(ERegion::C), int64(0))
							 : T.TestEqual(*FString::Printf(TEXT("%s: C has the construction's one free page"), Instant), V.FreeOf(ERegion::C), FreePages);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: zero slack outside C and A (S, P, H free 0)"), Instant),
			V.FreeOf(ERegion::S) + V.FreeOf(ERegion::P) + V.FreeOf(ERegion::H), int64(0));
		bOk &= Form1ChargesCoverDrawn(T, Gpu, Instant);
		bOk &= NoLedgerFault(T, Gpu, Instant);
		return bOk;
	}

	bool WithinC(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		return T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld (free charge never above C)"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C) && V.FreeOf(ERegion::C) <= V.CapacityOf(ERegion::C));
	}
}

// Cell 8.14, region C (§5.11 (b)), GPU twin of Cpu.Ledger.RegionC: the pool's C equals ceil(BudgetBytes / block bytes) x
// P_blk, computed in the test, at budgets that are not multiples of the block bytes (one page's bytes -> one block; one
// block's bytes plus one page's -> two blocks); the cache's budget is C's capacity in bytes. No pad, no build.
// Mutant: floor for ceil -> C = 0 at one page's bytes (and one block at the second budget) -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerRegionCTest, "SuperSLM.PrefixCache.Gpu.Ledger.RegionC",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerRegionCTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	const FGpuGeometry& G = N.Rig.Geo;
	const int64 Pblk = G.Pblk();
	const int64 BlockBytes = Pblk * G.PageBytes;
	bool bOk = TestEqual(TEXT("C = ceil(one page's bytes / block bytes) x P_blk, computed in the test"), GpuLedgerCapC(Gpu), FGpuGeometry::CeilDiv(G.PageBytes, BlockBytes) * Pblk);
	bOk &= TestEqual(TEXT("C is one block of P_blk pages (ceil, not floor)"), GpuLedgerCapC(Gpu), Pblk);
	bOk &= TestEqual(TEXT("one page's bytes: the cache's budget is C's capacity in bytes"), Gpu.GetPrefixCacheStats().BudgetBytes, GpuLedgerCapC(Gpu) * G.PageBytes);

	// A second non-multiple: one block's bytes plus one page's -> two blocks.
	const int64 Budget2 = (Pblk + 1) * G.PageBytes;
	bOk &= TestTrue(TEXT("the second budget is not a multiple of the block bytes"), Budget2 % BlockBytes != 0);
	FSuperSLMGpuPrefixCacheSettings Settings;
	Settings.BudgetBytes = Budget2;
	Gpu.ConfigurePrefixCache(Settings);
	if (!ConfigurePooled(*this, N.Rig, N.Rig.Config))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("C = ceil((P_blk + 1) pages' bytes / block bytes) x P_blk, computed in the test"), GpuLedgerCapC(Gpu), FGpuGeometry::CeilDiv(Budget2, BlockBytes) * Pblk);
	bOk &= TestEqual(TEXT("one block and one page's bytes -> two blocks"), GpuLedgerCapC(Gpu), 2 * Pblk);
	bOk &= TestEqual(TEXT("the cache's budget is C's capacity in bytes"), Gpu.GetPrefixCacheStats().BudgetBytes, GpuLedgerCapC(Gpu) * G.PageBytes);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the second Configure()"));
	return bOk;
}

// Cell 8.14, the eviction leg (§5.11 (a)), GPU twin of Cpu.Ledger.EvictionWindow. C = 5. E has L = 2B + 1 (k = 3 pages in
// C; its two adopters, through RequestAdoptPrefix, pre-charge A its f = 2 full pages); E (LRU), then F (1 page), both
// unleased, one page free. A build W charged 4 (L = 3B) takes the free page and fits only with E's refund of 3. E's
// release is held at the batch gate and the cache's build hold is set; the required reading is taken between the
// eviction decision and the release's apply: one eviction, E out of the map, C still full, W waiting with all of E's
// refund claimed, and no build issued, in flight or at the hold (the GPU has no paged HoldStart). At E's apply W is
// admitted with its charge complete (read at the build hold: C unchanged, W in flight at R(L) = 4, no refund pending,
// form 1), then built; exactly one eviction; F still a Memory hit; E's adopters keep generating (A holds their pages).
// Mutants: C refunded at the decision (W draws while E's private page is held: k - f = 1 page above the charges under
// zero slack -> form 1 red, LedgerFaults or engine exhaustion); the adopt bypassing the one adopt entry point (A never
// pre-charged -> the same); the loop ignoring the request's pending refunds (evicts F too -> two evictions, F not Memory).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerEvictionWindowTest, "SuperSLM.PrefixCache.Gpu.Ledger.EvictionWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerEvictionWindowTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 5))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	const int64 B = N.B();
	const int64 PageBytes = N.Rig.Geo.PageBytes;
	TArray<int32> ETokens, FTokens, WTokens, Suffix;
	if (!MakeTokens(*this, *N.Rig.Model, int32(2 * B + 1), N.Salt++, ETokens) || !EntryTokens(*this, N, 1, FTokens) || !BuildTokens(*this, N, 4, WTokens)
		|| !MakeTokens(*this, *N.Rig.Model, PCF::SuffixTokens, N.Salt++, Suffix))
	{
		return false;
	}
	const int64 C0 = Charged(Gpu, ERegion::C); // the pad
	const int64 A0 = Charged(Gpu, ERegion::A);

	// E, adopted twice through RequestAdoptPrefix (the one adopt entry point), then returned.
	FSuperSLMPrefix E, F, W, F2;
	FSuperSLMGpuSequence Adopter[2];
	if (!Resident(*this, N, ETokens, true, E, TEXT("E")))
	{
		return false;
	}
	for (FSuperSLMGpuSequence& S : Adopter)
	{
		if (!Vend(*this, Gpu, S) || !AdoptAndWait(*this, Gpu, S, E))
		{
			return false;
		}
	}
	bool bOk = TestEqual(TEXT("E is charged k = 3 pages of C"), Charged(Gpu, ERegion::C) - C0, int64(3));
	bOk &= TestEqual(TEXT("E's adopters pre-charge A its f = 2 full pages"), Charged(Gpu, ERegion::A) - A0, int64(2));
	Gpu.ReturnPrefix(E);
	if (!DrainQuiet(Gpu) || !Resident(*this, N, FTokens, false, F, TEXT("F")))
	{
		return false;
	}
	bOk &= FullAndClean(*this, Gpu, TEXT("C filled (E 3, F 1)"), 1);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();

	// The request, with E's release held and the cache's builds held at their first step.
	Seam::HoldReleases(Gpu, true);
	Seam::SetBuildHold(Gpu, true);
	if (!Request(*this, Gpu, PCF::Spec(WTokens), W, TEXT("W")))
	{
		Seam::SetBuildHold(Gpu, false);
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	SettleGpu(Gpu, true, 10.0);

	// The required reading: between E's eviction decision and its release's apply.
	{
		const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
		const TCHAR* Instant = TEXT("the window (decided, not applied)");
		const bool bWindow = TestEqual(TEXT("the window exists: E's release is held at the gate"), Seam::HeldReleaseCount(Gpu), 1);
		if (!bWindow)
		{
			AddError(TEXT("8.14 requires the reading between the eviction decision and the release's apply; a later reading cannot substitute"));
			Seam::SetBuildHold(Gpu, false);
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		bOk &= TestEqual(TEXT("window: exactly one eviction decided"), S1.Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("window: E is out of the map"), S1.ResidentEntries, S0.ResidentEntries - 1);
		bOk &= TestEqual(TEXT("window: C still carries E's k = 3 (C full)"), Charged(Gpu, ERegion::C), GpuLedgerCapC(Gpu));
		bOk &= TestTrue(TEXT("window: W waits"), IsPending(Gpu, W));
		bOk &= TestEqual(TEXT("window: one waiter"), S1.Waiters, 1);
		bOk &= TestEqual(TEXT("window: W claims all of E's refund"), S1.ClaimedRefundBytes, 3 * PageBytes);
		bOk &= TestEqual(TEXT("window: W's build is not at the build hold (drew nothing)"), Seam::HeldBuildCount(Gpu), 0);
		bOk &= TestEqual(TEXT("window: no build in flight"), Seam::InFlightBuildCount(Gpu), 0);
		bOk &= TestEqual(TEXT("window: no build issued"), S1.BuildsIssued, S0.BuildsIssued);
		bOk &= FullAndClean(*this, Gpu, Instant);
	}

	// Open the gate: at E's apply W is admitted with its charge complete, and stops at the build hold.
	Seam::HoldReleases(Gpu, false);
	Pump(Gpu, [&Gpu, &W]() { return Seam::HeldBuildCount(Gpu) >= 1 || !IsPending(Gpu, W); }, OpSeconds);
	bOk &= TestEqual(TEXT("W's build reaches the build hold (admitted at E's apply)"), Seam::HeldBuildCount(Gpu), 1);
	{
		const FSuperSLMPrefixCacheStats S2 = Gpu.GetPrefixCacheStats();
		bOk &= TestEqual(TEXT("admitted: the ledger total is unchanged by the transfer"), Charged(Gpu, ERegion::C), GpuLedgerCapC(Gpu));
		bOk &= TestEqual(TEXT("admitted: W's charge is its R(L) = 4"), S2.InFlightBytes, N.Rig.Geo.R(WTokens.Num()) * PageBytes);
		bOk &= TestEqual(TEXT("admitted: no refund pending"), S2.PendingRefunds, 0);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("admitted, at the build hold"));
	}
	Seam::SetBuildHold(Gpu, false);
	bOk &= ExpectLease(*this, Gpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("W drawn"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("W drawn"));

	// Exactly one eviction, and F is still a Memory hit.
	const FSuperSLMPrefixCacheStats S3 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("exactly one eviction (the eviction diagnostic)"), S3.Evictions - S0.Evictions, int64(1));
	bOk &= Resident(*this, N, FTokens, true, F2, TEXT("F again"));
	bOk &= TestEqual(TEXT("F after W's admission: Memory"), (uint8)Gpu.GetPrefixSource(F2), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestEqual(TEXT("F's hit issues no build"), Gpu.GetPrefixCacheStats().BuildsIssued, S3.BuildsIssued);

	// E's adopters keep generating after E's release (A holds their pages).
	for (const FSuperSLMGpuSequence& S : Adopter)
	{
		TArray<int32> Tokens;
		bOk &= GenerateComplete(*this, Gpu, S, Suffix, PCF::TwinNewTokens, Tokens);
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("the end"));
	for (const FSuperSLMGpuSequence& S : Adopter)
	{
		ReturnAndDrain(Gpu, S);
	}
	return bOk;
}

// Cell 8.14, refused, GPU twin of Cpu.Ledger.Refused: every entry leased and no eviction pending -> BudgetExhausted,
// nothing drawn. C = 5: A 3 and B 1 leased, one page free, W charged 3: nothing evicted, no waiter, no build.
// Mutant: admission over budget (a draw without a charge) -> form 1 red / C above its capacity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerRefusedTest, "SuperSLM.PrefixCache.Gpu.Ledger.Refused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerRefusedTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 5))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> A, Bt, W;
	FSuperSLMPrefix LA, LB, LW;
	if (!EntryTokens(*this, N, 3, A) || !EntryTokens(*this, N, 1, Bt) || !BuildTokens(*this, N, 3, W)
		|| !Resident(*this, N, A, true, LA, TEXT("A")) || !Resident(*this, N, Bt, true, LB, TEXT("B")))
	{
		return false;
	}
	bool bOk = FullAndClean(*this, Gpu, TEXT("every entry leased"), 1);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	if (!Request(*this, Gpu, PCF::Spec(W), LW, TEXT("W")))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Gpu, LW, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("W"), OpSeconds);
	SettleGpu(Gpu);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("nothing evicted"), S1.Evictions, S0.Evictions);
	bOk &= TestEqual(TEXT("no waiter"), S1.Waiters, 0);
	bOk &= TestEqual(TEXT("no build issued"), S1.BuildsIssued, S0.BuildsIssued);
	bOk &= FullAndClean(*this, Gpu, TEXT("after the refusal (nothing drawn)"), 1);
	return bOk;
}

// Cell 8.14, waiters (§5.11 (a); rev 1.15 re-check §4), GPU twin of Cpu.Ledger.Waiters. C = 7: two unleased victims V1,
// V2 (LRU first) of 3 pages each, one page free; two requests for distinct keys in one tick: X1 needs 4 (the free page
// and V1's 3), X2 needs exactly 3 (V2's 3). Each claims its own victim (two evictions, two held releases, two waiters);
// releases are dispatched one at a time (Seam::DispatchHeldRelease): X1 is admitted at V1's apply while X2 still
// waits, X2 at V2's; C never exceeds its capacity. Mutant: refunds not earmarked (both count V1's) -> the second stops
// without evicting -> one eviction -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerWaitersTest, "SuperSLM.PrefixCache.Gpu.Ledger.Waiters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerWaitersTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 7))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> V1, V2, X1, X2;
	FSuperSLMPrefix LV1, LV2, R1, R2;
	if (!EntryTokens(*this, N, 3, V1) || !EntryTokens(*this, N, 3, V2) || !BuildTokens(*this, N, 4, X1) || !BuildTokens(*this, N, 3, X2)
		|| !Resident(*this, N, V1, false, LV1, TEXT("V1")) || !Resident(*this, N, V2, false, LV2, TEXT("V2")))
	{
		return false;
	}
	bool bOk = FullAndClean(*this, Gpu, TEXT("C full but one page, two victims"), 1);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	Seam::HoldReleases(Gpu, true);
	if (!Request(*this, Gpu, PCF::Spec(X1), R1, TEXT("X1")) || !Request(*this, Gpu, PCF::Spec(X2), R2, TEXT("X2")))
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	SettleGpu(Gpu);
	bOk &= TestEqual(TEXT("two evictions (each request claims its own victim)"), Gpu.GetPrefixCacheStats().Evictions - S0.Evictions, int64(2));
	const bool bTwoHeld = TestEqual(TEXT("two releases held"), Seam::HeldReleaseCount(Gpu), 2);
	bOk &= TestEqual(TEXT("two waiters"), Gpu.GetPrefixCacheStats().Waiters, 2);
	bOk &= WithinC(*this, Gpu, TEXT("both waiting"));
	if (!bTwoHeld)
	{
		// With no release held, X1 can never leave Pending under the gate: open it and stop (the CPU box's hang).
		Seam::HoldReleases(Gpu, false);
		DrainQuiet(Gpu, 10.0);
		return false;
	}

	bOk &= TestTrue(TEXT("V1's release dispatched"), Seam::DispatchHeldRelease(Gpu, 0));
	WaitLease(Gpu, R1);
	SettleGpu(Gpu);
	bOk &= TestEqual(TEXT("X1 is admitted at V1's apply (and builds)"), (uint8)Gpu.GetPrefixLeaseState(R1), (uint8)ESuperSLMPrefixLeaseState::Ready);
	bOk &= TestTrue(TEXT("X2 still waits on V2's release"), IsPending(Gpu, R2));
	bOk &= WithinC(*this, Gpu, TEXT("after V1's apply"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after V1's apply"));

	Seam::HoldReleases(Gpu, false);
	bOk &= ExpectLease(*this, Gpu, R2, ESuperSLMPrefixLeaseState::Ready, TEXT("X2 at V2's apply"));
	bOk &= WithinC(*this, Gpu, TEXT("both admitted"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("both admitted"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("both admitted"));
	bOk &= TestEqual(TEXT("still exactly two evictions"), Gpu.GetPrefixCacheStats().Evictions - S0.Evictions, int64(2));
	return bOk;
}

// Cell 8.14, unclaimed refunds (rev 1.15, re-check R2), GPU twin of Cpu.Ledger.UnclaimedRefunds. Both legs with releases
// gated.
// (i) E/W/X, C = 7: E (6) the only unleased entry, one page free. W needs 5 (the free page and 4 of E's 6): evicts E,
//     claims and waits; X needs 2, in the same tick, claims the unclaimed rest with no further eviction and waits; at
//     E's apply both are admitted; one eviction. (The plan's E 3 / W 2 / X 1 scaled x2: a root build is charged >= 2.)
// (ii) Orphaned refund, a fresh configuration of the same rig, C = 4: E (3) the only unleased entry, one page free. Y
//     needs 5: evicts E, still short, resolves BudgetExhausted; E's eviction stands, its refund unclaimed. Z needs 4
//     before E's apply: claims E's refund, waits, is admitted at E's apply.
// Mutant: a loop that cannot claim unclaimed refunds (rev 1.14's rule) -> X refused in (i), Z in (ii).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerUnclaimedRefundsTest, "SuperSLM.PrefixCache.Gpu.Ledger.UnclaimedRefunds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerUnclaimedRefundsTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 7))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	const int64 PageBytes = N.Rig.Geo.PageBytes;
	bool bOk = true;

	// (i) E/W/X.
	{
		TArray<int32> E, W, X;
		FSuperSLMPrefix LE, LW, LX;
		if (!EntryTokens(*this, N, 6, E) || !BuildTokens(*this, N, 5, W) || !BuildTokens(*this, N, 2, X) || !Resident(*this, N, E, false, LE, TEXT("(i) E")))
		{
			return false;
		}
		bOk &= FullAndClean(*this, Gpu, TEXT("(i) C full but one page"), 1);
		const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
		Seam::HoldReleases(Gpu, true);
		if (!Request(*this, Gpu, PCF::Spec(W), LW, TEXT("(i) W")) || !Request(*this, Gpu, PCF::Spec(X), LX, TEXT("(i) X")))
		{
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		SettleGpu(Gpu);
		const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
		bOk &= TestEqual(TEXT("(i) exactly one eviction (E)"), S1.Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("(i) two waiters"), S1.Waiters, 2);
		bOk &= TestEqual(TEXT("(i) E's refund fully claimed (W 4, X 2)"), S1.ClaimedRefundBytes, 6 * PageBytes);
		bOk &= TestEqual(TEXT("(i) nothing unclaimed"), S1.UnclaimedRefundBytes, int64(0));
		bOk &= WithinC(*this, Gpu, TEXT("(i) waiting"));
		Seam::HoldReleases(Gpu, false);
		bOk &= ExpectLease(*this, Gpu, LW, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) W at E's apply"));
		bOk &= ExpectLease(*this, Gpu, LX, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) X at E's apply"));
		bOk &= TestEqual(TEXT("(i) still one eviction"), Gpu.GetPrefixCacheStats().Evictions - S0.Evictions, int64(1));
		bOk &= WithinC(*this, Gpu, TEXT("(i) admitted"));
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(i)"));
	}

	// (ii) Orphaned refund, on a fresh configuration of the same rig (its world and subsystem).
	if (!ConfigureGpuLedger(*this, N, 4, /*bInit*/ false))
	{
		return false;
	}
	{
		TArray<int32> E, Y, Z;
		FSuperSLMPrefix LE, LY, LZ;
		if (!EntryTokens(*this, N, 3, E) || !BuildTokens(*this, N, 5, Y) || !BuildTokens(*this, N, 4, Z) || !Resident(*this, N, E, false, LE, TEXT("(ii) E")))
		{
			return false;
		}
		bOk &= FullAndClean(*this, Gpu, TEXT("(ii) C full but one page"), 1);
		const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
		Seam::HoldReleases(Gpu, true);
		if (!Request(*this, Gpu, PCF::Spec(Y), LY, TEXT("(ii) Y")))
		{
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		bOk &= ExpectLease(*this, Gpu, LY, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("(ii) Y"), OpSeconds);
		SettleGpu(Gpu);
		bOk &= TestEqual(TEXT("(ii) E's eviction stands"), Gpu.GetPrefixCacheStats().Evictions - S0.Evictions, int64(1));
		bOk &= TestEqual(TEXT("(ii) E's refund is unclaimed"), Gpu.GetPrefixCacheStats().UnclaimedRefundBytes, 3 * PageBytes);
		bOk &= TestEqual(TEXT("(ii) E's release is still held"), Seam::HeldReleaseCount(Gpu), 1);
		if (!Request(*this, Gpu, PCF::Spec(Z), LZ, TEXT("(ii) Z")))
		{
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		SettleGpu(Gpu);
		bOk &= TestTrue(TEXT("(ii) Z waits on E's refund (not refused)"), IsPending(Gpu, LZ));
		bOk &= TestEqual(TEXT("(ii) Z claims E's refund"), Gpu.GetPrefixCacheStats().ClaimedRefundBytes, 3 * PageBytes);
		bOk &= TestEqual(TEXT("(ii) no further eviction"), Gpu.GetPrefixCacheStats().Evictions - S0.Evictions, int64(1));
		Seam::HoldReleases(Gpu, false);
		bOk &= ExpectLease(*this, Gpu, LZ, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) Z at E's apply"));
		bOk &= WithinC(*this, Gpu, TEXT("(ii) admitted"));
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(ii)"));
	}
	return bOk;
}

// Cell 8.14, a second request for a waiting key attaches to the waiter, GPU twin of Cpu.Ledger.CoalesceOnWaiter: one
// build (BuildsIssued up by one), both handles Ready at the apply, one eviction. C = 4: E 3 unleased, one page free; W
// needs 4, so it takes the free page, evicts E and waits; the second request for W's spec, a settle later, attaches.
// Mutant: a coalesced request charged again -> a second eviction or a second build -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerCoalesceOnWaiterTest, "SuperSLM.PrefixCache.Gpu.Ledger.CoalesceOnWaiter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerCoalesceOnWaiterTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 4))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> E, W;
	FSuperSLMPrefix LE, W1, W2;
	if (!EntryTokens(*this, N, 3, E) || !BuildTokens(*this, N, 4, W) || !Resident(*this, N, E, false, LE, TEXT("E")))
	{
		return false;
	}
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	Seam::HoldReleases(Gpu, true);
	if (!Request(*this, Gpu, PCF::Spec(W), W1, TEXT("W first")))
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	SettleGpu(Gpu);
	if (!Request(*this, Gpu, PCF::Spec(W), W2, TEXT("W second")))
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	// The second request's hash lands a tick later too: settle before reading the waiter count.
	SettleGpu(Gpu);
	bool bOk = TestFalse(TEXT("two leases are two handles"), W1 == W2);
	bOk &= TestEqual(TEXT("one waiter"), Gpu.GetPrefixCacheStats().Waiters, 1);
	Seam::HoldReleases(Gpu, false);
	bOk &= ExpectLease(*this, Gpu, W1, ESuperSLMPrefixLeaseState::Ready, TEXT("W first"));
	bOk &= ExpectLease(*this, Gpu, W2, ESuperSLMPrefixLeaseState::Ready, TEXT("W second"));
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("one build (BuildsIssued up by one)"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
	bOk &= TestEqual(TEXT("one eviction"), S1.Evictions - S0.Evictions, int64(1));
	bOk &= WithinC(*this, Gpu, TEXT("the end"));
	return bOk;
}

// Cell 8.14, re-Configure() while a request waits (rev 1.16 strike L1), GPU twin of Cpu.Ledger.StaleRelease. C = 6: E 5
// unleased, one page free; W needs 5 (the free page and 4 of E's 5), evicts E and waits with E's release held. A
// re-Configure() runs with the release still gated: it succeeds (the GPU has no pool-destroy probe; the Configure()'s
// result stands for the CPU's SSLM_OK reading), W's lease reads Invalid (the GPU cache ends with its configuration:
// lease states do not outlive a TearDown(), SuperSLMGpuSubsystem.h), nothing of the old configuration reaches the new
// cache (no held release, no stale completion applied: StaleCompletionsDropped 0 on the new host), and the rebuilt
// ledger holds exactly the new configuration's charges (C charged 0, the cache's own ledger empty), free charge never
// above C, form 1, no ledger fault.
// Mutant: a completion of the old configuration applied to the rebuilt ledger -> free charge above C, a LedgerFaults
// count, or C charged above 0 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLedgerStaleReleaseTest, "SuperSLM.PrefixCache.Gpu.Ledger.StaleRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLedgerStaleReleaseTest::RunTest(const FString& Parameters)
{
	FGpuLedgerRig N;
	if (!ConfigureGpuLedger(*this, N, 6))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> E, W;
	FSuperSLMPrefix LE, LW;
	if (!EntryTokens(*this, N, 5, E) || !BuildTokens(*this, N, 5, W) || !Resident(*this, N, E, false, LE, TEXT("E")))
	{
		return false;
	}
	Seam::HoldReleases(Gpu, true);
	if (!Request(*this, Gpu, PCF::Spec(W), LW, TEXT("W")))
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	SettleGpu(Gpu);
	bool bOk = TestEqual(TEXT("W waits on E's release"), Gpu.GetPrefixCacheStats().Waiters, 1);
	bOk &= TestEqual(TEXT("E's release is held"), Seam::HeldReleaseCount(Gpu), 1);

	// The teardown runs with the release still gated; the same configuration (cache settings included) is applied again.
	if (!ConfigurePooled(*this, N.Rig, N.Rig.Config))
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}
	Seam::HoldReleases(Gpu, false); // the new host's gate: open (it should never have been closed there)
	bOk &= TestTrue(TEXT("the rebuilt cache is configured, on region C"), Gpu.GetPrefixCacheStats().bConfigured && Gpu.GetPrefixCacheStats().bBudgetMode);
	// As the CPU's: a lease the teardown cancelled while Pending reads Cancelled by name (§5.11 (a); the GPU subsystem
	// keeps the last ended configuration's cache for this reading).
	bOk &= TestEqual(TEXT("W's lease reads Cancelled after the teardown"), (uint8)Gpu.GetPrefixLeaseState(LW), (uint8)ESuperSLMPrefixLeaseState::Cancelled);
	SettleGpu(Gpu);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("no release of the old configuration is held by the new cache"), Seam::HeldReleaseCount(Gpu), 0);
	bOk &= TestEqual(TEXT("no completion of the old configuration reached the new cache"), S1.StaleCompletionsDropped, int64(0));
	bOk &= TestEqual(TEXT("the rebuilt ledger holds only the new configuration's charges (C charged 0)"), Charged(Gpu, ERegion::C), int64(0));
	bOk &= TestEqual(TEXT("the cache's own ledger is empty"), S1.ChargedBytes, int64(0));
	bOk &= TestEqual(TEXT("no waiter"), S1.Waiters, 0);
	bOk &= WithinC(*this, Gpu, TEXT("after the re-Configure()"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the re-Configure()"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the re-Configure()"));
	return bOk;
}

// ---- 8.14's nested-A leg on the CPU cache (Q6, the U6 draft's R36) ----------------------------------------------------
//
// The CPU nested suite's rig (SuperSLMPrefixCacheNestedTests.cpp's FNestRig: A-EX on the CPU, budget mode, a leased pad
// leaving the construction's C) with one region-P block, which CreatePrefix (the prefill twin) needs. Its helpers live in
// a named namespace and call the CPU fixtures fully qualified (PCF::), so nothing here meets the GPU names above.

namespace SuperSLMPrefixCacheCpuRegionATests
{
	using ERegion = ESuperSLMPagedRegion;
	using Seam = FSuperSLMPrefixCacheTestSeam;

	struct FCpuRegionARig
	{
		PCF::FPagingRig Rig;
		FSuperSLMPrefix Pad;
		int32 Salt = 400;
		USuperSLMSubsystem& Cpu() { return *Rig.Cpu; }
		int64 B() const { return Rig.Geo.B; }
	};

	FSuperSLMRuntimeConfig CpuRegionAConfig() { return PCF::MakeConfig(2, 512, /*PrefixBlocks*/ 1, /*HeadroomBlocks*/ 0); }

	bool ConfigureCpuRegionA(FAutomationTestBase& T, FCpuRegionARig& N, int64 CPages)
	{
		PCF::FPagingRig& Rig = N.Rig;
		if (!Rig.Init(T) || !PCF::ConfigureBudget(T, Rig, CpuRegionAConfig()) || !PCF::RequireAExShape(T, Rig))
		{
			return false;
		}
		if (!PCF::ConfigureCacheBudget(T, Rig, PCF::WithCache(CpuRegionAConfig(), Rig.Geo.PageBytes)))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("budget mode: nested entries are admitted"), PCF::Stats(*Rig.Cpu).bBudgetMode))
		{
			return false;
		}
		const int64 PadPages = PCF::LedgerOf(*Rig.Cpu).CapacityOf(ERegion::C) - CPages;
		if (PadPages <= 0)
		{
			return T.TestEqual(TEXT("the construction's C is the region's"), PadPages, int64(0));
		}
		TArray<int32> PadTokens;
		return PCF::MakeTokens(T, *Rig.Cpu, int32(PadPages * Rig.Geo.B), N.Salt++, PadTokens)
			&& PCF::RequestReady(T, *Rig.Cpu, PCF::Spec(PadTokens), N.Pad, TEXT("pad")) && PCF::DrainQuiet(*Rig.Cpu);
	}

	FSuperSLMPrefixSpec CpuChildSpec(const FSuperSLMPrefix& Parent, const TArray<int32>& Suffix)
	{
		FSuperSLMPrefixSpec S = PCF::Spec(Suffix);
		S.Parent = Parent;
		return S;
	}

	bool CpuEntry(FAutomationTestBase& T, FCpuRegionARig& N, const FSuperSLMPrefixSpec& Spec, bool bKeepLease, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		if (!PCF::RequestReady(T, N.Cpu(), Spec, Out, What))
		{
			return false;
		}
		if (!bKeepLease)
		{
			N.Cpu().ReturnPrefix(Out);
		}
		return PCF::DrainQuiet(N.Cpu());
	}

	bool CpuPhysical(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		bool bOk = PCF::Form1ChargesCoverDrawn(T, Cpu, Instant);
		bOk &= PCF::NoLedgerFault(T, Cpu, Instant);
		const FSuperSLMPagedLedgerView V = PCF::LedgerOf(Cpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C));
		return bOk;
	}

	// The unleased Victim made a ghost (the CPU nested suite's MakeGhost): every other entry is leased; a request of
	// GhostPages + 1 evicts Victim and is refused; its release applies.
	bool CpuMakeGhost(FAutomationTestBase& T, FCpuRegionARig& N, int64 GhostPages, const TCHAR* What)
	{
		USuperSLMSubsystem& Cpu = N.Cpu();
		const FSuperSLMPrefixCacheStats S0 = PCF::Stats(Cpu);
		TArray<int32> Tokens;
		FSuperSLMPrefix Probe;
		if (!PCF::MakeTokens(T, Cpu, int32(GhostPages * N.B()), N.Salt++, Tokens) || !PCF::Request(T, Cpu, PCF::Spec(Tokens), Probe, What))
		{
			return false;
		}
		bool bOk = PCF::ExpectLease(T, Cpu, Probe, ESuperSLMPrefixLeaseState::BudgetExhausted, What, 10.0);
		bOk &= PCF::DrainQuiet(Cpu);
		const FSuperSLMPrefixCacheStats S1 = PCF::Stats(Cpu);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one eviction"), What), S1.Evictions - S0.Evictions, int64(1));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: its charge stays as a ghost"), What), S1.GhostBytes - S0.GhostBytes, GhostPages * N.Rig.Geo.PageBytes);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one more ghost"), What), S1.Ghosts - S0.Ghosts, 1);
		bOk &= CpuPhysical(T, Cpu, What);
		return bOk;
	}

	bool Run(FAutomationTestBase& T)
	{
		FCpuRegionARig N;
		if (!ConfigureCpuRegionA(T, N, 4))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = N.Cpu();
		const int64 B = N.B();
		const int64 CPad = PCF::Charged(Cpu, ERegion::C);
		const int64 A0 = PCF::Charged(Cpu, ERegion::A);
		FSuperSLMPrefix P, D, X;
		TArray<int32> PT, DT, XT, Suffix;
		if (!PCF::MakeTokens(T, Cpu, int32(2 * B), N.Salt++, PT) || !PCF::MakeTokens(T, Cpu, int32(B), N.Salt++, DT)
			|| !PCF::MakeTokens(T, Cpu, int32(3 * B), N.Salt++, XT) || !PCF::MakeTokens(T, Cpu, PCF::SuffixTokens, N.Salt++, Suffix))
		{
			return false;
		}
		if (!CpuEntry(T, N, PCF::Spec(PT), true, P, TEXT("P (2 full pages)")) || !CpuEntry(T, N, CpuChildSpec(P, DT), true, D, TEXT("D (child of P, 1 private page)")))
		{
			return false;
		}
		bool bOk = T.TestEqual(TEXT("P 2 + D 1 on C, one page free"), PCF::Charged(Cpu, ERegion::C) - CPad, int64(3));

		// A budget sequence adopts D through AdoptPrefix: paging pre-charges A with D's full pages, P's included.
		FSuperSLMSequence Adopter;
		if (!PCF::Vend(T, Cpu, Adopter) || !PCF::AdoptAndWait(T, Cpu, Adopter, D))
		{
			return false;
		}
		bOk &= T.TestEqual(TEXT("the adopter pre-charges A with D's 3 full pages (P's 2 included)"), PCF::Charged(Cpu, ERegion::A) - A0, int64(3));

		// P evicted under D (a ghost on C); its release applies.
		Cpu.ReturnPrefix(P);
		PCF::DrainQuiet(Cpu);
		bOk &= CpuMakeGhost(T, N, 2, TEXT("P evicted under D"));
		{
			const TCHAR* Instant = TEXT("the required reading (P a ghost, its release applied)");
			bOk &= T.TestEqual(TEXT("C counts P's pages (the ghost 2) and D's 1"), PCF::Charged(Cpu, ERegion::C) - CPad, int64(3));
			bOk &= T.TestEqual(TEXT("A counts P's pages through the adopter's dependency on D (3)"), PCF::Charged(Cpu, ERegion::A) - A0, int64(3));
			bOk &= PCF::Form1ChargesCoverDrawn(T, Cpu, Instant);
			bOk &= PCF::NoLedgerFault(T, Cpu, Instant);
		}

		// D evicted: X needs 4 (the free 1, D's 1 and the ghost's 2), admitted at D's apply, which ends the ghost.
		Cpu.ReturnPrefix(D);
		PCF::DrainQuiet(Cpu);
		const FSuperSLMPrefixCacheStats S0 = PCF::Stats(Cpu);
		bOk &= CpuEntry(T, N, PCF::Spec(XT), true, X, TEXT("X (needs 4: evicts D)"));
		PCF::SettleCacheIo(Cpu, 10.0);
		const FSuperSLMPrefixCacheStats S1 = PCF::Stats(Cpu);
		bOk &= T.TestEqual(TEXT("one eviction (D)"), S1.Evictions - S0.Evictions, int64(1));
		bOk &= T.TestEqual(TEXT("C has refunded the ghost (no ghost)"), S1.Ghosts, 0);
		bOk &= T.TestEqual(TEXT("C has refunded the ghost (no ghost bytes)"), S1.GhostBytes, int64(0));
		bOk &= T.TestEqual(TEXT("A still carries the adopter's pages (3)"), PCF::Charged(Cpu, ERegion::A) - A0, int64(3));
		bOk &= CpuPhysical(T, Cpu, TEXT("after D's eviction and release"));

		// The adopter still decodes its twin's tokens.
		TArray<int32> Tokens, Twin;
		bOk &= PCF::GenerateComplete(T, Cpu, Adopter, Suffix, PCF::TwinNewTokens, Tokens);
		PCF::ReturnAndDrain(Cpu, Adopter);
		TArray<int32> Whole = PT;
		Whole.Append(DT);
		bOk &= PCF::TwinTokens(T, Cpu, Whole, Suffix, Twin);
		bOk &= T.TestEqual(TEXT("the adopter's tokens equal the prefill twin's (P + D's tokens, CreatePrefix)"), Tokens, Twin);
		bOk &= PCF::NoLedgerFault(T, Cpu, TEXT("the end"));
		return bOk;
	}
}

// Cell 8.14, nested-A (rev 1.18, PC-15 Q6; the U6 draft's R36), on the CPU cache. C = 4: P (2 full pages, leased) with
// cached child D (1 private page, leased), one page free. A budget sequence adopts D through AdoptPrefix: A is
// pre-charged with D's 3 full pages, P's 2 included. P is returned, evicted under D (a ghost of 2 on C) and its release
// applies: form 1 at that reading counts P's pages on both C (the ghost) and A (the adopter's dependency). D is then
// evicted by X (needs 4) and its release applies: C has refunded the ghost (no ghost), A still carries the 3 pages, form
// 1 holds, and the adopter still decodes its prefill twin's tokens (CreatePrefix of P + D's tokens, adopted, the same
// suffix). The GPU twin is Gpu.Nested.RegionA (SuperSLMPrefixCacheGpuNestedTests.cpp).
// Mutant: A not pre-charged for the parent's pages through the child -> after the refund the adopter holds uncharged
// pages -> form 1 red (or A reads 1, not 3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedRegionATest, "SuperSLM.PrefixCache.Cpu.Nested.RegionA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedRegionATest::RunTest(const FString& Parameters)
{
	return SuperSLMPrefixCacheCpuRegionATests::Run(*this);
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
