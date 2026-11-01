// Precache plan rev 1.18, the U6 suite's GPU twins of the U2 cache cells (1.x-8.x): the GPU subsystem's cache instance
// (pooled mode, region C of the GPU page ledger). Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on; the disk-hit
// legs need SUPERSLM_WITH_GPU_PREFIX_SAVE as well (the GPU prefix save and restore verbs).
//
//   SuperSLM.PrefixCache.Gpu.Churn                    cell 1.4 (and 7.10), scaled down              [U6]
//   SuperSLM.PrefixCache.Gpu.WarmReconfigure          cell 1.5                                      [U6]
//   SuperSLM.PrefixCache.Gpu.ReconfigureInFlight      cell 1.7: build (a)(b)(c), restore legs       [U6]
//   SuperSLM.PrefixCache.Gpu.HandleDispositions       cell 2.10 (§5.10)                             [U6]
//   SuperSLM.PrefixCache.Gpu.UnknownSchema            cell 2.11                                     [U6]
//   SuperSLM.PrefixCache.Gpu.KeyComponents            cell 2.12 (tokens, schema, length prefix)     [U6]
//   SuperSLM.PrefixCache.Gpu.Coalescing               cell 3.5                                      [U6]
//   SuperSLM.PrefixCache.Gpu.EvictionVsQueuedAdopt    cell 3.6                                      [U6]
//   SuperSLM.PrefixCache.Gpu.AllPinned                cell 5.5 (and 7.10)                           [U6]
//   SuperSLM.PrefixCache.Gpu.MemoryAndLru             cell 7.13                                     [U6]
//   SuperSLM.PrefixCache.Gpu.AdoptNotReady            cell 7.21                                     [U6]
//   SuperSLM.PrefixCache.Gpu.SaveRestoreAdopter       cell 8.8                                      [U6]
//   SuperSLM.PrefixCache.Gpu.LeaseAsRestoreHint       cell 8.15                                     [U6]
//
// The CPU cells are SuperSLMPrefixCacheCpuTests.cpp's and SuperSLMPrefixCacheResidencyTests.cpp's. What differs on the GPU,
// for every cell here:
//   - The GPU cache is pooled (budget mode) only: legacy-mode legs and legacy configurations become budget mode. Region C
//     is one block (256 pages at A-EX's B = 16) at the smallest budget, so a cell that needs a small C leases a root "pad"
//     of CapC - C pages first (as SuperSLMPrefixCacheGpuNestedTests.cpp does). A pad is a ~4,000-token build (minutes on
//     the box), so only the cells whose oracle needs evictions among a few pages take one.
//   - A GPU cache ends with its configuration (SuperSLMGpuSubsystem.h). The last ended configuration's cache is kept for
//     reading: an old lease's state (Cancelled by name, as on the CPU) and the *AtLastTeardown seams (releases per
//     handle, the most of one handle, unfrozen builds released). The GPU has no teardown fill probe
//     (ArmTeardownProbe/TeardownFreePages/LastPoolDestroyStatus are CPU only), so TeardownClean's probe readings become
//     "re-Configure() succeeds, no ledger fault" plus those seams.
//   - Paging's one-shot Layer-1 status override does not reach a GPU cache build (the GPU host calls
//     sslm_gpu_prefix_begin itself); the PoolExhausted legs (3.5, 5.5) use Seam::SetNextCacheBuildStatus instead.
//   - A GPU adopt is queued (RequestAdoptPrefix returns a handle): a refusal at the call is an invalid handle with
//     GetLastLifecycleRequestError() naming it.
//   - Prefixes are one or two pages (16 or 32 tokens) wherever the oracle allows: GPU builds are slow on the box.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "Math/RandomStream.h"
#include "SuperSLMPrefixCacheTestSeam.h"

// The runtime module's GPU cache host defaults this switch (SuperSLMGpuPrefixCacheHost.h, private); this file
// reads it too, so it carries the same default.
#ifndef SUPERSLM_WITH_GPU_PREFIX_SAVE
#define SUPERSLM_WITH_GPU_PREFIX_SAVE 0
#endif

namespace
{
	using namespace SuperSLMPagingGpuFixtures;
	using Seam = FSuperSLMPrefixCacheTestSeam;
	using ERegion = ESuperSLMPagedRegion;
	using ELease = ESuperSLMPrefixLeaseState;

	struct FGpuTwin
	{
		FGpuPagingRig Rig;
		FSuperSLMGpuRuntimeConfig Config;
		FSuperSLMPrefix Pad;
		int32 Salt = 300;
		USuperSLMGpuSubsystem& Gpu() { return *Rig.Gpu; }
		int64 B() const { return Rig.Geo.B; }
	};

	FSuperSLMPrefixSpec TwSpec(const TArray<int32>& Tokens, FName Schema = NAME_None)
	{
		return SuperSLMPrefixCacheFixtures::Spec(Tokens, Schema);
	}

	int64 TwCapC(const USuperSLMGpuSubsystem& Gpu) { return LedgerOf(Gpu).CapacityOf(ERegion::C); }

	ELease TwResolve(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, double Seconds = BuildSeconds)
	{
		Pump(Gpu, [&]() { return Gpu.GetPrefixLeaseState(Lease) != ELease::Pending; }, Seconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool TwExpect(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ELease Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s resolves"), What), (uint8)TwResolve(Gpu, Lease), (uint8)Want);
	}

	bool TwRequest(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bAccepted = Gpu.RequestPrefix(Spec, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: RequestPrefix() accepts the spec (%s)"), What, *Error), bAccepted)
			&& T.TestTrue(*FString::Printf(TEXT("%s: a handle is minted"), What), Out.IsValid());
	}

	// Requests Spec, expects Ready, returns the lease unless bKeep, drains.
	bool TwLease(FAutomationTestBase& T, FGpuTwin& N, const FSuperSLMPrefixSpec& Spec, bool bKeep, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		if (!TwRequest(T, N.Gpu(), Spec, Out, What) || !TwExpect(T, N.Gpu(), Out, ELease::Ready, What))
		{
			return false;
		}
		if (!bKeep)
		{
			N.Gpu().ReturnPrefix(Out);
		}
		return DrainQuiet(N.Gpu());
	}

	bool TwPagesTokens(FAutomationTestBase& T, FGpuTwin& N, int64 Pages, TArray<int32>& Out)
	{
		return MakeTokens(T, *N.Rig.Model, int32(Pages * N.B()), N.Salt++, Out);
	}

	// (Need - 1) x B tokens: a build is charged one page above its settle, so this request is charged Need pages.
	bool TwNeedTokens(FAutomationTestBase& T, FGpuTwin& N, int64 Need, TArray<int32>& Out)
	{
		return MakeTokens(T, *N.Rig.Model, int32((Need - 1) * N.B()), N.Salt++, Out);
	}

	bool TwRoot(FAutomationTestBase& T, FGpuTwin& N, int64 Pages, bool bKeep, FSuperSLMPrefix& Out, TArray<int32>& OutTokens, const TCHAR* What)
	{
		return TwPagesTokens(T, N, Pages, OutTokens) && TwLease(T, N, TwSpec(OutTokens), bKeep, Out, What);
	}

	// Count distinct root entries of Pages pages, each Ready and returned (resident, unleased), in creation order.
	bool TwFillUnleased(FAutomationTestBase& T, FGpuTwin& N, int32 Count, int64 Pages, TArray<FSuperSLMPrefixSpec>& OutSpecs)
	{
		for (int32 I = 0; I < Count; ++I)
		{
			TArray<int32> Tokens;
			FSuperSLMPrefix Lease;
			if (!TwRoot(T, N, Pages, false, Lease, Tokens, TEXT("fill")))
			{
				return false;
			}
			OutSpecs.Add(TwSpec(Tokens));
		}
		return DrainQuiet(N.Gpu());
	}

	// The "physical" reading of the GPU cache cells: form 1 at the GPU ledger, no ledger fault, C within its capacity.
	bool TwPhysical(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		bool bOk = Form1ChargesCoverDrawn(T, Gpu, Instant);
		bOk &= NoLedgerFault(T, Gpu, Instant);
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C));
		return bOk;
	}

	// Re-Configure()s at the twin's configuration (the cache settings persist on the subsystem): the GPU cache's teardown
	// and a fresh cache, with no pad.
	bool TwReconfigure(FAutomationTestBase& T, FGpuTwin& N)
	{
		N.Pad = FSuperSLMPrefix();
		if (!ConfigurePooled(T, N.Rig, N.Config))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S = N.Gpu().GetPrefixCacheStats();
		return T.TestTrue(TEXT("the GPU cache is configured, on region C"), S.bConfigured && S.bBudgetMode);
	}

	// Leases a root pad of CapC - CPages pages, so the construction sees exactly CPages of C. CPages <= 0: no pad.
	bool TwPad(FAutomationTestBase& T, FGpuTwin& N, int64 CPages)
	{
		if (CPages <= 0)
		{
			return true;
		}
		const int64 PadPages = TwCapC(N.Gpu()) - CPages;
		if (!T.TestTrue(*FString::Printf(TEXT("region C (%lld pages) is above the construction's %lld"), TwCapC(N.Gpu()), CPages), PadPages > 0))
		{
			return false;
		}
		TArray<int32> PadTokens;
		return TwPagesTokens(T, N, PadPages, PadTokens) && TwLease(T, N, TwSpec(PadTokens), true, N.Pad, TEXT("the pad"));
	}

	// Inits the rig, configures pooled mode (2 slots, SequenceBudget 512, PrefixBlocks P_blocks, one headroom block), the
	// cache at one block of region C, and the pad.
	bool TwConfigure(FAutomationTestBase& T, FGpuTwin& N, int64 CPages, const FString& DiskDirectory = FString(), int32 PrefixBlocks = 0)
	{
		FGpuPagingRig& Rig = N.Rig;
		N.Config = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, PrefixBlocks);
		if (!Rig.Init(T) || !ConfigurePooled(T, Rig, N.Config) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		FSuperSLMGpuPrefixCacheSettings Settings;
		Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C
		Settings.bDiskEnabled = !DiskDirectory.IsEmpty();
		Settings.DiskDirectory = DiskDirectory;
		Settings.DiskMaxBytes = Settings.bDiskEnabled ? (int64(1) << 30) : 0;
		Rig.Gpu->ConfigurePrefixCache(Settings);
		return TwReconfigure(T, N) && TwPad(T, N, CPages);
	}

	// Exactly N bare ticks.
	void TwTickBare(USuperSLMGpuSubsystem& Gpu, int32 N = 4)
	{
		for (int32 I = 0; I < N; ++I)
		{
			Gpu.Tick(Step);
		}
	}

	// The GPU twin of SuperSLMPrefixCacheFixtures::SettleCacheIo(..., bDrainJobs false): ticks (paced on the device)
	// until no key hash or disk IO is in flight and no completion waits for the apply pass, on two consecutive ticks.
	bool TwSettleIo(USuperSLMGpuSubsystem& Gpu, double Seconds = OpSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		int32 Quiet = 0;
		while (Quiet < 2 && FPlatformTime::Seconds() - Start < Seconds)
		{
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu.Tick(Step);
			Quiet = Seam::IsCacheIoSettled(Gpu) ? Quiet + 1 : 0;
			FPlatformProcess::Sleep(0.0005f);
		}
		return Quiet >= 2;
	}

	FSuperSLMPrefixOpResult TwOpResolved(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixOp& Op, double Seconds = BuildSeconds)
	{
		FSuperSLMPrefixOpResult R;
		Pump(Gpu, [&]() { Gpu.GetPrefixOpResult(Op, R); return R.Result != ESuperSLMPrefixOpResult::Pending; }, Seconds);
		Gpu.GetPrefixOpResult(Op, R);
		return R;
	}

	// The prefill twin, on the GPU: adopt Lease into a fresh sequence, generate TwinNewTokens after Suffix.
	bool TwAdopterTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq) || !AdoptAndWait(T, Gpu, Seq, Lease))
		{
			return false;
		}
		const bool bOk = GenerateComplete(T, Gpu, Seq, Suffix, SuperSLMPrefixCacheFixtures::TwinNewTokens, OutTokens);
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	// The same tokens built by CreatePrefix() (region P: the config needs PrefixBlocks >= 1), adopted and generated the
	// same way; the prefix is released after.
	bool TwTwinTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMPrefix Twin;
		if (!BuildPrefix(T, Gpu, Tokens, Twin))
		{
			return false;
		}
		const bool bOk = TwAdopterTokens(T, Gpu, Twin, Suffix, OutTokens);
		return ReleasePrefixAndDrain(T, Gpu, Twin) && bOk;
	}

	bool TwEqualsTwin(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, const TArray<int32>& Twin, const TCHAR* What)
	{
		TArray<int32> Tokens;
		return TwAdopterTokens(T, Gpu, Lease, Suffix, Tokens)
			&& T.TestEqual(*FString::Printf(TEXT("%s: the adopter's tokens equal the prefill twin's"), What), Tokens, Twin);
	}

	// A GPU adopt refused at the call: no handle, GetLastLifecycleRequestError() contains Token, and nothing queued on Seq.
	bool TwAdoptRefusedAtCall(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const FSuperSLMPrefix& Lease, const TCHAR* Token,
		const TCHAR* What)
	{
		const int32 QueuedBefore = Gpu.GetPendingLifecycleOperationCount(Seq);
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestAdoptPrefix(Seq, Lease);
		const FString Error = Gpu.GetLastLifecycleRequestError();
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: RequestAdoptPrefix() is refused at the call (no handle)"), What), Handle.IsValid());
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: its error names %s (%s)"), What, Token, *Error), Error.Contains(Token));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing queued"), What), Gpu.GetPendingLifecycleOperationCount(Seq), QueuedBefore);
		if (Handle.IsValid())
		{
			WaitOp(Gpu, Handle);
			Gpu.ReleaseLifecycleOpHandle(Handle);
		}
		return bOk;
	}
}

// Cell 1.4 (and 7.10)'s GPU twin: churn. Random requests over a key set, a region C of 10 pages (the pad leases the rest),
// one-page entries, random leases and returns, the disk tier on so an evicted key re-requested restores. Every request
// that cannot be charged resolves BudgetExhausted; a leased entry is never evicted; at every 10th request the physical
// reading holds (form 1 at the GPU ledger, no ledger fault, C within its capacity: the GPU's budget-mode form of the
// CPU's legacy F_min probe); entries were recompute-built and (with the GPU save) restore-built; after every lease is
// returned and a re-Configure(), no ledger fault.
// Scaled down from the CPU's 500 requests over 40 keys to 200 over 24 (each request is a submission-thread round trip on
// the GPU, plus the pad); the oracle is unchanged. The CPU's teardown fill probe has no GPU twin (no GPU teardown probe).
// The CPU cell carries StressFilter; this twin follows the GPU cells' ProductFilter.
// Mutant: a build that draws without a charge -> form 1 refused at some reading -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuChurnTest, "SuperSLM.PrefixCache.Gpu.Churn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuChurnTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-churn"));
	constexpr int64 BudgetPagesN = 10;
	constexpr int32 KeyCount = 24;
	constexpr int32 RequestCount = 200;
	FGpuTwin N;
	if (!TwConfigure(*this, N, BudgetPagesN, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<TArray<int32>> Keys;
	for (int32 K = 0; K < KeyCount; ++K)
	{
		TArray<int32> Tokens;
		if (!MakeTokens(*this, *N.Rig.Model, int32(N.B()), 100 + K, Tokens))
		{
			return false;
		}
		Keys.Add(Tokens);
	}
	FRandomStream Rng(14);
	TArray<FSuperSLMPrefix> Live;
	bool bOk = true;
	int32 Exhausted = 0;
	for (int32 I = 0; I < RequestCount; ++I)
	{
		if (Live.Num() > 0 && Rng.FRand() < 0.5f)
		{
			const int32 At = Rng.RandRange(0, Live.Num() - 1);
			bOk &= TestTrue(TEXT("ReturnPrefix() ends a live lease"), Gpu.ReturnPrefix(Live[At]));
			Live.RemoveAtSwap(At);
		}
		FSuperSLMPrefix Lease;
		if (!TwRequest(*this, Gpu, TwSpec(Keys[Rng.RandRange(0, KeyCount - 1)]), Lease, TEXT("churn")))
		{
			return false;
		}
		const ELease State = TwResolve(Gpu, Lease);
		if (State == ELease::Ready)
		{
			Live.Add(Lease);
		}
		else
		{
			bOk &= TestEqual(TEXT("a request that cannot be charged resolves BudgetExhausted"), (uint8)State, (uint8)ELease::BudgetExhausted);
			++Exhausted;
		}
		if (I % 10 == 9)
		{
			bOk &= TwPhysical(*this, Gpu, *FString::Printf(TEXT("request %d"), I + 1));
		}
		for (const FSuperSLMPrefix& L : Live)
		{
			bOk &= TestEqual(TEXT("a leased entry is never evicted"), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Ready);
		}
	}
	const FSuperSLMPrefixCacheStats S = Gpu.GetPrefixCacheStats();
	bOk &= TestTrue(TEXT("entries were recompute-built"), S.ServedBySource[(int32)ESuperSLMPrefixSource::Recomputed] > 0);
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	bOk &= TestTrue(TEXT("entries were restore-built (disk)"), S.ServedBySource[(int32)ESuperSLMPrefixSource::Disk] > 0);
#endif
	AddInfo(FString::Printf(TEXT("GPU churn: %d BudgetExhausted, %lld evictions, %lld disk hits"), Exhausted, S.Evictions,
		S.ServedBySource[(int32)ESuperSLMPrefixSource::Disk]));
	for (const FSuperSLMPrefix& L : Live)
	{
		Gpu.ReturnPrefix(L);
	}
	DrainQuiet(Gpu);
	bOk &= TwPhysical(*this, Gpu, TEXT("after ReturnPrefix of all"));
	bOk &= TwReconfigure(*this, N);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the closing re-Configure()"));
	return bOk;
}

// Cell 1.5's GPU twin: a warm GPU cache (three entries, one of them leased) re-Configure()d: every cached engine handle
// is released exactly once at the teardown (read from the last ended configuration's cache:
// Seam::ReleasesOfAtLastTeardown), the re-Configure() succeeds with no ledger fault, the old lease is no handle of the new
// configuration (Invalid), the new cache holds no entry, and the next request is a miss served by the next tier
// (Recomputed, the disk off). Nothing was released while the cache was warm.
// Mutants: the warm entries kept across the re-Configure() -> the next request is a Memory hit -> red; a handle left
// unreleased or released twice -> its teardown count reads 0 or 2 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuWarmReconfigureTest, "SuperSLM.PrefixCache.Gpu.WarmReconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuWarmReconfigureTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<FSuperSLMPrefixSpec> Specs;
	FSuperSLMPrefix Held;
	if (!TwFillUnleased(*this, N, 3, 1, Specs) || !TwLease(*this, N, Specs[0], true, Held, TEXT("a leased entry")))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the leased entry is a Memory hit of the warm cache"), (uint8)Gpu.GetPrefixSource(Held), (uint8)ESuperSLMPrefixSource::Memory);
	// Each cached entry's engine handle, read through a lease (the unleased ones leased and returned at once).
	TArray<uint64> Engines;
	Engines.Add(Seam::EngineHandleOf(Gpu, Held));
	for (int32 I = 1; I < Specs.Num(); ++I)
	{
		FSuperSLMPrefix Peek;
		if (!TwLease(*this, N, Specs[I], true, Peek, TEXT("an unleased entry's handle")))
		{
			return false;
		}
		Engines.Add(Seam::EngineHandleOf(Gpu, Peek));
		Gpu.ReturnPrefix(Peek);
	}
	DrainQuiet(Gpu);
	for (const uint64 Engine : Engines)
	{
		bOk &= TestTrue(TEXT("each cached entry has an engine handle"), Engine != 0);
	}
	bOk &= TestEqual(TEXT("nothing released while the cache is warm"), Seam::MaxReleasesOfOneHandle(Gpu), 0);
	bOk &= TwPhysical(*this, Gpu, TEXT("warm"));
	if (!TwReconfigure(*this, N))
	{
		return false;
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the warm re-Configure()"));
	for (int32 I = 0; I < Engines.Num(); ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("cached entry %d's engine handle (%s) was released exactly once"), I, I == 0 ? TEXT("leased") : TEXT("unleased")),
			Seam::ReleasesOfAtLastTeardown(Gpu, Engines[I]), 1);
	}
	bOk &= TestTrue(TEXT("no handle released twice at the teardown"), Seam::MaxReleasesOfOneHandleAtLastTeardown(Gpu) <= 1);
	bOk &= TestEqual(TEXT("the old lease is not a handle of this configuration"), (uint8)Gpu.GetPrefixLeaseState(Held), (uint8)ELease::Invalid);
	bOk &= TestEqual(TEXT("the new configuration's cache holds no entry"), Gpu.GetPrefixCacheStats().ResidentEntries, 0);
	FSuperSLMPrefix Next;
	bOk &= TwLease(*this, N, Specs[0], true, Next, TEXT("after re-Configure()"));
	bOk &= TestEqual(TEXT("a miss served by the next tier"), (uint8)Gpu.GetPrefixSource(Next), (uint8)ESuperSLMPrefixSource::Recomputed);
	return bOk;
}

// Cell 1.7's GPU twin: re-Configure() while a cache build, and separately a cache restore, is in flight:
//   (b) build, running: held at the build hold (begun, its reserve drawn, nothing prefilled; Seam::SetBuildHold);
//   (c) build, between two build steps (the GPU's chunk jobs, Q7): one step has prefilled and the chunk hold stops the
//       next (Seam::SetBuildChunkHold(1)); 8 ticks later still one step and the build begun and unfrozen;
//   restore, running: held at the restore hold (Seam::SetRestoreHold: the job is posted again, calling nothing);
//   (a) build, queued: charged and waiting, nothing begun: the request needs an eviction whose release is held
//       (Seam::HoldReleases), so it waits on the refund (C = 3 behind a pad, a one-page entry unleased, a three-page
//       request);
//   restore, queued: the same, for a restore of a disk-warm three-page key (C = 4 behind a pad, a two-page entry
//       unleased: the restore needs its eviction whether it is charged 3 or 4 pages).
// In each leg the re-Configure() returns (a held GPU step never blocks the submission thread: the teardown ends a
// stepped build where it is), reads no ledger fault, the request reads Cancelled by name (from the last ended
// configuration's cache), no handle was released twice at the teardown (MaxReleasesOfOneHandleAtLastTeardown), a begun
// unfrozen build was released exactly once and nothing else counted as one (UnfrozenBuildReleasesAtLastTeardown: 1 for
// (b) and (c), 0 for the rest), the next configuration posts no cache lane job of its own accord, and the same key in
// the next configuration is served by the tier below memory (Recomputed for a build: the aborted build left no entry and
// no file; Disk for a restore). The restore key's file is warmed first. Only (a) and the queued restore build pads.
// The CPU's teardown fill probe and pool destroy status have no GPU seam (ArmTeardownProbe et al. are CPU only).
// Mutants: a stepped build re-posted after the teardown began -> the re-Configure() hangs or the next configuration posts
// a lane job -> red; the begun prefix left live -> the unfrozen count reads 0 -> red; released twice -> the teardown's
// most-of-one-handle reads 2 -> red; the request left unresolved -> it does not read Cancelled -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuReconfigureInFlightTest, "SuperSLM.PrefixCache.Gpu.ReconfigureInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuReconfigureInFlightTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-reconfigure-in-flight"));
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	bool bOk = true;
	// The re-Configure() under test, and what the next configuration reads.
	auto TeardownAndAfter = [&](const FSuperSLMPrefix& L, const TArray<int32>& Tokens, const TCHAR* Name, ESuperSLMPrefixSource WantAgain, int64 WantUnfrozen)
	{
		if (!TwReconfigure(*this, N))
		{
			return false;
		}
		bool bLeg = NoLedgerFault(*this, Gpu, *FString::Printf(TEXT("%s: after the re-Configure()"), Name));
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: the request reads Cancelled by name"), Name), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Cancelled);
		bLeg &= TestTrue(*FString::Printf(TEXT("%s: no handle released twice at the teardown"), Name), Seam::MaxReleasesOfOneHandleAtLastTeardown(Gpu) <= 1);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: begun unfrozen builds released by the teardown"), Name), Seam::UnfrozenBuildReleasesAtLastTeardown(Gpu), WantUnfrozen);
		TwTickBare(Gpu, 8);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: the next configuration posts no cache lane job of its own"), Name), Seam::PostedLaneJobs(Gpu), int64(0));
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: no build in flight in the next configuration"), Name), Seam::InFlightBuildCount(Gpu), 0);
		FSuperSLMPrefix Again;
		bLeg &= TwLease(*this, N, TwSpec(Tokens), false, Again, *FString::Printf(TEXT("%s: the same key in the next configuration"), Name));
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: served by the tier below memory"), Name), (uint8)Gpu.GetPrefixSource(Again), (uint8)WantAgain);
		bLeg &= TwPhysical(*this, Gpu, Name);
		return bLeg;
	};

#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	// The restore legs' key: three pages, built once and written back (its memory entry ends at (b)'s teardown).
	TArray<int32> RestoreKey;
	{
		FSuperSLMPrefix K;
		const int64 Mark = Gpu.GetPrefixCacheStats().DiskWritesIssued;
		if (!TwPagesTokens(*this, N, 3, RestoreKey) || !TwLease(*this, N, TwSpec(RestoreKey), false, K, TEXT("the restore legs' key")))
		{
			return false;
		}
		Pump(Gpu, [&Gpu, Mark]() { return Gpu.GetPrefixCacheStats().DiskWritesIssued > Mark; }, BuildSeconds);
		TwSettleIo(Gpu);
		DrainQuiet(Gpu);
		TwSettleIo(Gpu);
		if (!TestTrue(TEXT("the restore legs' key is on disk"), IFileManager::Get().FileExists(*Seam::DiskFileOf(Gpu, TwSpec(RestoreKey)))))
		{
			return false;
		}
	}
#endif

	// (b) running: held at the build hold.
	{
		const TCHAR* Name = TEXT("(b) build, running (held after its begin)");
		TArray<int32> Tokens;
		if (!TwPagesTokens(*this, N, 2, Tokens))
		{
			return false;
		}
		Seam::SetBuildHold(Gpu, true);
		FSuperSLMPrefix L;
		if (!TwRequest(*this, Gpu, TwSpec(Tokens), L, Name))
		{
			Seam::SetBuildHold(Gpu, false);
			return false;
		}
		Pump(Gpu, [&Gpu]() { return Seam::HeldBuildCount(Gpu) == 1; }, OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the build stops at the build hold"), Name), Seam::HeldBuildCount(Gpu), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the build is begun and unfrozen"), Name), (uint8)Seam::BuildStateOf(Gpu, L), (uint8)ESuperSLMPrefixBuildState::Begun);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the lease is Pending"), Name), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Pending);
		bOk &= TeardownAndAfter(L, Tokens, Name, ESuperSLMPrefixSource::Recomputed, 1);
	}

	// (c) between two build steps: 6 pages (96 tokens) take more than one step at the box's prefill rate.
	{
		const TCHAR* Name = TEXT("(c) build, between two steps");
		TArray<int32> Tokens;
		if (!TwPagesTokens(*this, N, 6, Tokens))
		{
			return false;
		}
		Seam::SetBuildChunkHold(Gpu, 1);
		FSuperSLMPrefix L;
		if (!TwRequest(*this, Gpu, TwSpec(Tokens), L, Name))
		{
			Seam::SetBuildChunkHold(Gpu, -1);
			return false;
		}
		Pump(Gpu, [&Gpu, &L]() { return Seam::BuildChunkJobsOf(Gpu, L) >= 1; }, BuildSeconds);
		TwTickBare(Gpu, 8); // the build waits at the chunk hold: no second step
		bOk &= TestEqual(*FString::Printf(TEXT("%s: one step prefilled, and the build waits for its next"), Name), Seam::BuildChunkJobsOf(Gpu, L), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the build is begun and unfrozen"), Name), (uint8)Seam::BuildStateOf(Gpu, L), (uint8)ESuperSLMPrefixBuildState::Begun);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the lease is Pending"), Name), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Pending);
		bOk &= TeardownAndAfter(L, Tokens, Name, ESuperSLMPrefixSource::Recomputed, 1);
	}

#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	// Restore, running: held at the restore hold.
	{
		const TCHAR* Name = TEXT("restore, running (held at the restore hold)");
		const int64 RestoresBefore = Gpu.GetPrefixCacheStats().RestoresIssued;
		Seam::SetRestoreHold(Gpu, true);
		FSuperSLMPrefix L;
		if (!TwRequest(*this, Gpu, TwSpec(RestoreKey), L, Name))
		{
			Seam::SetRestoreHold(Gpu, false);
			return false;
		}
		Pump(Gpu, [&Gpu]() { return Seam::HeldRestoreCount(Gpu) == 1; }, OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the restore stops at the restore hold"), Name), Seam::HeldRestoreCount(Gpu), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: one restore issued"), Name), Gpu.GetPrefixCacheStats().RestoresIssued - RestoresBefore, int64(1));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the lease is Pending"), Name), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Pending);
		bOk &= TeardownAndAfter(L, RestoreKey, Name, ESuperSLMPrefixSource::Disk, 0);
	}
#endif

	// (a) queued: charged and waiting on a held release, nothing begun.
	{
		const TCHAR* Name = TEXT("(a) build, queued (waiting on a held release)");
		FSuperSLMPrefix V, X;
		TArray<int32> VT, XT;
		if (!TwPad(*this, N, 3) || !TwRoot(*this, N, 1, false, V, VT, TEXT("V (unleased, the victim)")) || !TwNeedTokens(*this, N, 3, XT))
		{
			return false;
		}
		Seam::HoldReleases(Gpu, true);
		if (!TwRequest(*this, Gpu, TwSpec(XT), X, Name))
		{
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		Pump(Gpu, [&Gpu]() { return Gpu.GetPrefixCacheStats().Waiters == 1; }, OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the request waits on V's refund"), Name), Gpu.GetPrefixCacheStats().Waiters, 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: V's release is held"), Name), Seam::HeldReleaseCount(Gpu), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the lease is Pending"), Name), (uint8)Gpu.GetPrefixLeaseState(X), (uint8)ELease::Pending);
		const ESuperSLMPrefixBuildState BuildState = Seam::BuildStateOf(Gpu, X);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: nothing begun (build state %d)"), Name, (int32)BuildState),
			BuildState != ESuperSLMPrefixBuildState::Begun && BuildState != ESuperSLMPrefixBuildState::Frozen);
		bOk &= TwPhysical(*this, Gpu, *FString::Printf(TEXT("%s: before the re-Configure()"), Name));
		bOk &= TeardownAndAfter(X, XT, Name, ESuperSLMPrefixSource::Recomputed, 0);
	}

#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	// Restore, queued: the disk-warm key's restore waits on a held release.
	{
		const TCHAR* Name = TEXT("restore, queued (waiting on a held release)");
		FSuperSLMPrefix V, R;
		TArray<int32> VT;
		if (!TwPad(*this, N, 4) || !TwRoot(*this, N, 2, false, V, VT, TEXT("V (two pages, unleased, the victim)")))
		{
			return false;
		}
		const int64 RestoresBefore = Gpu.GetPrefixCacheStats().RestoresIssued;
		Seam::HoldReleases(Gpu, true);
		if (!TwRequest(*this, Gpu, TwSpec(RestoreKey), R, Name))
		{
			Seam::HoldReleases(Gpu, false);
			return false;
		}
		Pump(Gpu, [&Gpu]() { return Gpu.GetPrefixCacheStats().Waiters == 1; }, OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the restore waits on V's refund"), Name), Gpu.GetPrefixCacheStats().Waiters, 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: V's release is held"), Name), Seam::HeldReleaseCount(Gpu), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the lease is Pending"), Name), (uint8)Gpu.GetPrefixLeaseState(R), (uint8)ELease::Pending);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: no restore issued yet"), Name), Gpu.GetPrefixCacheStats().RestoresIssued, RestoresBefore);
		bOk &= TwPhysical(*this, Gpu, *FString::Printf(TEXT("%s: before the re-Configure()"), Name));
		bOk &= TeardownAndAfter(R, RestoreKey, Name, ESuperSLMPrefixSource::Disk, 0);
	}
#endif
	return bOk;
}

// Cell 2.10 (§5.10)'s GPU twin: every disposition of a GPU cache handle against the GPU's prefix verbs. C = 3 behind a
// pad; one-page entries; PrefixBlocks 1 so CreatePrefix() has region P.
//   ReleasePrefix() of a leased and of an unleased cache handle: refused naming ReturnPrefix, nothing released;
//   lease A returned while lease B on the same entry lives: ReturnPrefix(A) twice is false, A's adopt is refused at the
//     call (PrefixLeaseEnded, nothing queued), B's adopts;
//   an adopt of a lease whose entry was since evicted (and returned): refused PrefixLeaseEnded;
//   ReturnPrefix() of a CreatePrefix() handle: false, the handle still Ready, then released as today;
//   each verb with a lease from before a re-Configure(): the adopt refused Malformed (no handle of this configuration),
//     ReleasePrefix() and ReturnPrefix() false.
// The CPU's teardown readings (fill probe, pool destroy status) have no GPU seam: the closing reading is no ledger fault.
// Mutant: ReleasePrefix() releasing a cache handle -> the release count reads 1 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuHandleDispositionsTest, "SuperSLM.PrefixCache.Gpu.HandleDispositions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuHandleDispositionsTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 3, FString(), /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> T1, T2;
	if (!TwPagesTokens(*this, N, 1, T1) || !TwPagesTokens(*this, N, 1, T2))
	{
		return false;
	}
	bool bOk = true;
	FString Error;
	FSuperSLMPrefix A, B2, Unleased;

	// ReleasePrefix on a leased and on an unleased cache handle: refused, nothing released.
	if (!TwLease(*this, N, TwSpec(T1), true, A, TEXT("lease A")) || !TwLease(*this, N, TwSpec(T1), true, B2, TEXT("lease B, same entry")))
	{
		return false;
	}
	const uint64 Engine = Seam::EngineHandleOf(Gpu, A);
	bOk &= TestFalse(TEXT("ReleasePrefix(leased cache handle) is refused"), Gpu.ReleasePrefix(A, Error));
	bOk &= TestTrue(*FString::Printf(TEXT("its error names ReturnPrefix (%s)"), *Error), Error.Contains(TEXT("owned by the prefix cache; call ReturnPrefix")));
	if (!TwLease(*this, N, TwSpec(T2), true, Unleased, TEXT("an entry to unlease")))
	{
		return false;
	}
	const uint64 UnleasedEngine = Seam::EngineHandleOf(Gpu, Unleased);
	Gpu.ReturnPrefix(Unleased);
	Error.Reset();
	bOk &= TestFalse(TEXT("ReleasePrefix(unleased cache handle) is refused"), Gpu.ReleasePrefix(Unleased, Error));
	DrainQuiet(Gpu);
	bOk &= TestEqual(TEXT("nothing released by either refusal"), Seam::ReleasesOf(Gpu, Engine) + Seam::ReleasesOf(Gpu, UnleasedEngine), 0);

	// Lease A returned while lease B on the same entry is live: A's adopt refused, B's adopts.
	bOk &= TestTrue(TEXT("ReturnPrefix(A)"), Gpu.ReturnPrefix(A));
	bOk &= TestFalse(TEXT("ReturnPrefix(A) twice is false"), Gpu.ReturnPrefix(A));
	FSuperSLMGpuSequence S1, S2;
	if (!Vend(*this, Gpu, S1) || !Vend(*this, Gpu, S2))
	{
		return false;
	}
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, S1, A, TEXT("PrefixLeaseEnded"), TEXT("adopt of returned lease A"));
	bOk &= AdoptAndWait(*this, Gpu, S2, B2);

	// An adopt of a lease whose entry was since evicted: B returned, then a three-page request evicts both entries.
	bOk &= TestTrue(TEXT("ReturnPrefix(B)"), Gpu.ReturnPrefix(B2));
	{
		TArray<int32> FillT;
		FSuperSLMPrefix Fill;
		bOk &= TwNeedTokens(*this, N, 3, FillT) && TwLease(*this, N, TwSpec(FillT), false, Fill, TEXT("fill past the budget"));
	}
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, S1, B2, TEXT("PrefixLeaseEnded"), TEXT("adopt of a lease on an evicted entry"));

	// ReturnPrefix on a CreatePrefix handle: false, nothing changes.
	FSuperSLMPrefix Made;
	if (!BuildPrefix(*this, Gpu, T2, Made))
	{
		return false;
	}
	bOk &= TestFalse(TEXT("ReturnPrefix(CreatePrefix handle) is false"), Gpu.ReturnPrefix(Made));
	bOk &= TestEqual(TEXT("the CreatePrefix handle is still Ready"), (uint8)Gpu.GetPrefixPhase(Made), (uint8)ESuperSLMPrefixPhase::Ready);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Made);
	ReturnAndDrain(Gpu, S1);
	ReturnAndDrain(Gpu, S2);
	bOk &= TwPhysical(*this, Gpu, TEXT("2.10 before the re-Configure()"));

	// Each verb with a handle from before a re-Configure().
	FSuperSLMPrefix Old;
	if (!TwLease(*this, N, TwSpec(T1), true, Old, TEXT("a pre-Configure lease")) || !TwReconfigure(*this, N))
	{
		return false;
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("2.10 after the re-Configure()"));
	FSuperSLMGpuSequence S3;
	if (!Vend(*this, Gpu, S3))
	{
		return false;
	}
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, S3, Old, TEXT("Malformed"), TEXT("adopt of a stale handle (no handle of this configuration)"));
	Error.Reset();
	bOk &= TestFalse(TEXT("ReleasePrefix(stale handle)"), Gpu.ReleasePrefix(Old, Error));
	bOk &= TestFalse(TEXT("ReturnPrefix(stale handle)"), Gpu.ReturnPrefix(Old));
	ReturnAndDrain(Gpu, S3);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("2.10 end"));
	return bOk;
}

// Cell 2.11's GPU twin: an unknown SchemaName resolves UnknownSchema by name at a tick (never at the call: on the GPU the
// name resolves on the submission thread at the build's begin); no cache entry and no disk file under that key.
// Mutant: the unknown schema built unbound -> Ready, an entry and a file -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuUnknownSchemaTest, "SuperSLM.PrefixCache.Gpu.UnknownSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuUnknownSchemaTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-unknown-schema"));
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> Tokens;
	FSuperSLMPrefix L;
	if (!TwPagesTokens(*this, N, 1, Tokens) || !TwRequest(*this, Gpu, TwSpec(Tokens, FName(TEXT("no_such_schema"))), L, TEXT("unknown schema")))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("Pending at the call (resolving a name is a Layer-1 matter, on the submission thread)"), (uint8)Gpu.GetPrefixLeaseState(L), (uint8)ELease::Pending);
	bOk &= TwExpect(*this, Gpu, L, ELease::UnknownSchema, TEXT("unknown schema"));
	DrainQuiet(Gpu);
	TwSettleIo(Gpu);
	bOk &= TestEqual(TEXT("no cache entry"), Gpu.GetPrefixCacheStats().ResidentEntries, 0);
	bOk &= TestEqual(TEXT("no file in the directory"), Dir.Files().Num(), 0);
	bOk &= TwPhysical(*this, Gpu, TEXT("after UnknownSchema"));
	return bOk;
}

// Cell 2.12 (key mutation)'s GPU twin: for each key component that can vary within one subsystem, spec A then spec B
// differing only there. B's source is not Memory, B's adopter tokens equal B's prefill twin (CreatePrefix() of B's
// tokens, adopted and generated the same way), and the spec-mismatch counter reads 0.
//   tokens: one token changed; schema name: unbound vs A-EX's compiled schema (no twin, as on the CPU); length prefix:
//   the name's prefix is an equivalent mutant at A-EX's one compiled name (the CPU cell's reasoning), so the leg kept is
//   the token count's prefix: T and T + [t] stay distinct.
// Mutant: a component dropped from the key -> B served from A's entry -> the spec compare catches it -> the counter
// reads 1 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuKeyComponentsTest, "SuperSLM.PrefixCache.Gpu.KeyComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuKeyComponentsTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0, FString(), /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> Base, Suffix;
	if (!TwPagesTokens(*this, N, 2, Base) || !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 2, Suffix))
	{
		return false;
	}
	struct FPair { const TCHAR* Name; FSuperSLMPrefixSpec A; FSuperSLMPrefixSpec B; bool bTwin; };
	TArray<int32> Longer = Base;
	Longer.Add(Base[0]);
	const FPair Pairs[] = {
		{ TEXT("tokens"), TwSpec(Base), TwSpec(WithTokenChanged(Base, 20)), true },
		{ TEXT("schema name"), TwSpec(Base), TwSpec(Base, FName(SuperSLML2S2Fixtures::DemoSchemaName())), false },
		{ TEXT("length prefix"), TwSpec(Base), TwSpec(Longer), true },
	};
	bool bOk = true;
	for (const FPair& P : Pairs)
	{
		FSuperSLMPrefix LA, LB;
		if (!TwLease(*this, N, P.A, true, LA, P.Name) || !TwLease(*this, N, P.B, true, LB, P.Name))
		{
			return false;
		}
		bOk &= TestNotEqual(*FString::Printf(TEXT("%s: B's source is not Memory"), P.Name), (uint8)Gpu.GetPrefixSource(LB), (uint8)ESuperSLMPrefixSource::Memory);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the spec-mismatch counter reads 0"), P.Name), Gpu.GetPrefixCacheStats().SpecMismatches, int64(0));
		if (P.bTwin)
		{
			TArray<int32> Twin;
			bOk &= TwTwinTokens(*this, Gpu, P.B.Tokens, Suffix, Twin) && TwEqualsTwin(*this, Gpu, LB, Suffix, Twin, P.Name);
		}
		Gpu.ReturnPrefix(LA);
		Gpu.ReturnPrefix(LB);
		DrainQuiet(Gpu);
	}
	return bOk;
}

// Cell 3.5's GPU twin: 16 requests for one key in one tick -> one build (counted once every lease resolved), 16 Ready
// handles, 16 distinct; also when the first requester returns its lease before Ready (it reads Ended, the other 15
// Ready), and when the shared build ends PoolExhausted (Seam::SetNextCacheBuildStatus: the build's begin reads
// SSLM_GPU_KV_POOL_EXHAUSTED; run first, on an empty cache, so evict-and-retry finds nothing evictable and all 16 read
// PoolExhausted). All resolve by name within the bound. The coalesced failed load is cell 3.5's disk half
// (Gpu.Disk.CoalescedLoadFails), not this file's. The 16 keys' hashes finish off the game thread, so they are held and
// let go together, to apply in one pass: applied across two, a failed build can resolve before the later requests join
// it, and those start a build of their own, as they should.
// Mutant: a second build for a key already in flight -> the build count reads 2 or more -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCoalescingTest, "SuperSLM.PrefixCache.Gpu.Coalescing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCoalescingTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	auto Burst = [&](bool bReturnFirst, ELease Want, const TCHAR* What)
	{
		TArray<int32> Tokens;
		if (!TwPagesTokens(*this, N, 2, Tokens))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
		TArray<FSuperSLMPrefix> Leases;
		Seam::HoldKeyHash(Gpu, TwSpec(Tokens), true);
		for (int32 I = 0; I < 16; ++I)
		{
			FSuperSLMPrefix L;
			if (!TwRequest(*this, Gpu, TwSpec(Tokens), L, What))
			{
				Seam::HoldKeyHash(Gpu, TwSpec(Tokens), false);
				return false;
			}
			Leases.Add(L);
		}
		bool bLeg = TestTrue(*FString::Printf(TEXT("%s: the 16 hashes are held"), What), TwSettleIo(Gpu, 10.0));
		Seam::HoldKeyHash(Gpu, TwSpec(Tokens), false);
		if (bReturnFirst)
		{
			bLeg &= TestTrue(*FString::Printf(TEXT("%s: the first requester returns its lease before Ready"), What), Gpu.ReturnPrefix(Leases[0]));
		}
		for (int32 I = bReturnFirst ? 1 : 0; I < 16; ++I)
		{
			bLeg &= TwExpect(*this, Gpu, Leases[I], Want, What);
		}
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: one build"), What), Gpu.GetPrefixCacheStats().BuildsIssued - S0.BuildsIssued, int64(1));
		if (bReturnFirst)
		{
			bLeg &= TestEqual(*FString::Printf(TEXT("%s: the returned lease reads Ended"), What), (uint8)Gpu.GetPrefixLeaseState(Leases[0]), (uint8)ELease::Ended);
		}
		TSet<int64> Ids;
		for (const FSuperSLMPrefix& L : Leases)
		{
			Ids.Add(L.Id);
			Gpu.ReturnPrefix(L);
		}
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: 16 distinct handles"), What), Ids.Num(), 16);
		DrainQuiet(Gpu);
		bLeg &= TwPhysical(*this, Gpu, What);
		return bLeg;
	};
	Seam::SetNextCacheBuildStatus(Gpu, int32(SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
	bool bOk = Burst(false, ELease::PoolExhausted, TEXT("pool exhausted"));
	bOk &= Burst(false, ELease::Ready, TEXT("plain"));
	bOk &= Burst(true, ELease::Ready, TEXT("first returns early"));
	return bOk;
}

// Cell 3.6's GPU twin: an eviction racing a queued adopt of the LRU entry. C = 3 behind a pad: the LRU entry (1 page,
// leased), the next entry (1 page, returned), one page free. The LRU lease is adopted (RequestAdoptPrefix, its apply held
// by paging's HoldApply(Adopt): the GPU has no HoldStart) and then returned, so the queued adopt alone pins the entry; a
// new request charged 2 pages needs one eviction and takes the next entry. The adopt succeeds, the new entry is Ready, and
// the next entry re-requested is no Memory hit. The new request's key hash is settled before the adopt's apply is let go
// (the CPU cell's box finding).
// Mutant: the queued adopt does not pin -> the eviction takes the LRU entry -> the next entry is a Memory hit -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuEvictionVsAdoptTest, "SuperSLM.PrefixCache.Gpu.EvictionVsQueuedAdopt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuEvictionVsAdoptTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 3))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> Lru, Next, New;
	FSuperSLMPrefix LLru, LNext, LNew;
	if (!TwRoot(*this, N, 1, true, LLru, Lru, TEXT("LRU")) || !TwRoot(*this, N, 1, true, LNext, Next, TEXT("next")) || !TwNeedTokens(*this, N, 2, New))
	{
		return false;
	}
	Gpu.ReturnPrefix(LNext);
	DrainQuiet(Gpu);
	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}
	const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Adopt);
	const FSuperSLMLifecycleOpHandle Op = Gpu.RequestAdoptPrefix(Seq, LLru);
	if (!TestTrue(*FString::Printf(TEXT("RequestAdoptPrefix(LRU) queues (%s)"), *Gpu.GetLastLifecycleRequestError()), Op.IsValid()))
	{
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		return false;
	}
	Gpu.ReturnPrefix(LLru); // the queued adopt alone pins it now
	bool bOk = TestTrue(TEXT("the adopt is held at its apply"), Hold.IsValid() && WaitHoldReached(Gpu, Hold));
	if (!TwRequest(*this, Gpu, TwSpec(New), LNew, TEXT("the request that evicts")))
	{
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		return false;
	}
	bOk &= TestTrue(TEXT("the request's key hash is applied before the adopt resolves"), TwSettleIo(Gpu, 10.0));
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
	bOk &= TestEqual(TEXT("the queued adopt succeeds"), (uint8)WaitOp(Gpu, Op), (uint8)ESuperSLMRestoreResult::Success);
	Gpu.ReleaseLifecycleOpHandle(Op);
	bOk &= TwExpect(*this, Gpu, LNew, ELease::Ready, TEXT("the new entry"));
	DrainQuiet(Gpu);
	FSuperSLMPrefix Probe;
	bOk &= TwLease(*this, N, TwSpec(Next), true, Probe, TEXT("the next entry again"));
	bOk &= TestNotEqual(TEXT("the eviction took the next entry (it is no Memory hit)"), (uint8)Gpu.GetPrefixSource(Probe), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TwPhysical(*this, Gpu, TEXT("after the race"));
	ReturnAndDrain(Gpu, Seq);
	return bOk;
}

// Cell 5.5 (and 7.10)'s GPU twin: every entry leased (A and B, one page each, in a full region C of one block) and one
// more request charged 255 pages: it could fit an empty C but not beside A and B, and nothing is evictable, so it resolves
// BudgetExhausted with no wait, nothing evicted and nothing drawn (the fill probe's free reading equal before and after).
// Budget mode (the CPU cell runs legacy mode, which the GPU has not); the charge-fits-empty construction keeps the cell
// pad-free.
// PoolExhausted: with A and B still leased (nothing evictable), the next cache build's begin reads
// SSLM_GPU_KV_POOL_EXHAUSTED (Seam::SetNextCacheBuildStatus) and evict-and-retry finds nothing, so a one-page request
// resolves PoolExhausted. The CPU returns B first, its request evicting B to be admitted; in a one-block C the request
// is admitted without evicting, so B stays leased here instead, or the retry would evict B and build.
// Mutant: admission over budget when every entry is pinned -> the request is admitted and draws -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuAllPinnedTest, "SuperSLM.PrefixCache.Gpu.AllPinned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuAllPinnedTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> At, Bt, W;
	FSuperSLMPrefix LA, LB, LW;
	const int64 CapC = TwCapC(Gpu);
	if (!TwRoot(*this, N, 1, true, LA, At, TEXT("A")) || !TwRoot(*this, N, 1, true, LB, Bt, TEXT("B")) || !TwNeedTokens(*this, N, CapC - 1, W))
	{
		return false;
	}
	int64 FreeBefore = -1;
	if (!DrainQuiet(Gpu) || !ProbeFree(*this, Gpu, LedgerOf(Gpu).PoolPages, FreeBefore))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the free reading sees >= 3 pages"), FreeBefore >= 3);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	if (!TwRequest(*this, Gpu, TwSpec(W), LW, TEXT("one more")))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("one more: BudgetExhausted"), (uint8)TwResolve(Gpu, LW, OpSeconds), (uint8)ELease::BudgetExhausted);
	bOk &= TestEqual(TEXT("no wait"), Gpu.GetPrefixCacheStats().Waiters, 0);
	bOk &= TestEqual(TEXT("nothing evicted"), Gpu.GetPrefixCacheStats().Evictions, S0.Evictions);
	int64 FreeAfter = -1;
	bOk &= DrainQuiet(Gpu) && ProbeFree(*this, Gpu, LedgerOf(Gpu).PoolPages, FreeAfter);
	bOk &= TestEqual(TEXT("after BudgetExhausted: nothing drawn (fill probe free count)"), FreeAfter, FreeBefore);
	bOk &= TwPhysical(*this, Gpu, TEXT("after BudgetExhausted"));
	Gpu.ReturnPrefix(LW);
	DrainQuiet(Gpu);

	// PoolExhausted: the pool short with nothing evictable.
	TArray<int32> Small;
	FSuperSLMPrefix LP;
	if (!TwPagesTokens(*this, N, 1, Small))
	{
		return false;
	}
	Seam::SetNextCacheBuildStatus(Gpu, int32(SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
	if (!TwRequest(*this, Gpu, TwSpec(Small), LP, TEXT("pool short")))
	{
		return false;
	}
	bOk &= TwExpect(*this, Gpu, LP, ELease::PoolExhausted, TEXT("pool short, nothing evictable"));
	Gpu.ReturnPrefix(LP);
	DrainQuiet(Gpu);
	bOk &= TwPhysical(*this, Gpu, TEXT("after PoolExhausted"));
	Gpu.ReturnPrefix(LA);
	Gpu.ReturnPrefix(LB);
	DrainQuiet(Gpu);
	return bOk;
}

// Cell 7.13's GPU twin: the memory-hit oracle and the LRU oracle. C = 6 behind a pad; one-page entries; PrefixBlocks 1
// for the prefill twin.
//   Memory hit: an entry built and returned, requested again: Memory, no restore and no build issued, and the adopter's
//   tokens equal the prefill twin's.
//   LRU: four more entries (insertion order First, 0, 1, 2, 3; one page free), touched in the order 2, First, 0, 3, 1
//   (neither insertion nor its reverse), one more admitted (charged 2: one eviction): entry 2 is evicted, First, 0, 1
//   and 3 stay resident (the CPU cell re-Configures between the halves; here the memory-hit entry joins the touch order).
// Mutants: evict the most recent -> red; evict in insertion order -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuMemoryAndLruTest, "SuperSLM.PrefixCache.Gpu.MemoryAndLru",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuMemoryAndLruTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 6, FString(), /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> Tokens, Suffix, Twin;
	FSuperSLMPrefix First, Hit;
	if (!TwPagesTokens(*this, N, 1, Tokens) || !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 2, Suffix)
		|| !TwTwinTokens(*this, Gpu, Tokens, Suffix, Twin) || !TwLease(*this, N, TwSpec(Tokens), false, First, TEXT("first")))
	{
		return false;
	}
	// Memory hit.
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bool bOk = TwLease(*this, N, TwSpec(Tokens), true, Hit, TEXT("again"));
	bOk &= TestEqual(TEXT("again: Memory"), (uint8)Gpu.GetPrefixSource(Hit), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestEqual(TEXT("no restore issued"), Gpu.GetPrefixCacheStats().RestoresIssued, S0.RestoresIssued);
	bOk &= TestEqual(TEXT("no prefill issued"), Gpu.GetPrefixCacheStats().BuildsIssued, S0.BuildsIssued);
	bOk &= TwEqualsTwin(*this, Gpu, Hit, Suffix, Twin, TEXT("memory hit"));
	Gpu.ReturnPrefix(Hit);
	DrainQuiet(Gpu);

	// LRU.
	TArray<FSuperSLMPrefixSpec> Specs;
	if (!TwFillUnleased(*this, N, 4, 1, Specs))
	{
		return false;
	}
	const FSuperSLMPrefixSpec FirstSpec = TwSpec(Tokens);
	const FSuperSLMPrefixSpec* TouchOrder[] = { &Specs[2], &FirstSpec, &Specs[0], &Specs[3], &Specs[1] };
	for (const FSuperSLMPrefixSpec* S : TouchOrder)
	{
		FSuperSLMPrefix Touch;
		bOk &= TwLease(*this, N, *S, false, Touch, TEXT("touch"));
		bOk &= TestEqual(TEXT("touch: Memory"), (uint8)Gpu.GetPrefixSource(Touch), (uint8)ESuperSLMPrefixSource::Memory);
	}
	TArray<int32> More;
	FSuperSLMPrefix LMore;
	if (!TwPagesTokens(*this, N, 1, More) || !TwLease(*this, N, TwSpec(More), false, LMore, TEXT("one more")))
	{
		return false;
	}
	struct FProbe { const FSuperSLMPrefixSpec* Spec; const TCHAR* Name; bool bResident; };
	const FProbe Probes[] = {
		{ &FirstSpec, TEXT("First (touched second)"), true },
		{ &Specs[0], TEXT("entry 0 (touched third)"), true },
		{ &Specs[1], TEXT("entry 1 (touched last)"), true },
		{ &Specs[3], TEXT("entry 3 (touched fourth)"), true },
		{ &Specs[2], TEXT("entry 2 (touched least recently)"), false }, // last: re-requesting it evicts another
	};
	for (const FProbe& P : Probes)
	{
		FSuperSLMPrefix L;
		bOk &= TwLease(*this, N, *P.Spec, false, L, TEXT("probe"));
		const bool bMemory = Gpu.GetPrefixSource(L) == ESuperSLMPrefixSource::Memory;
		bOk &= TestEqual(*FString::Printf(TEXT("%s is %s"), P.Name, P.bResident ? TEXT("resident") : TEXT("evicted")), bMemory, P.bResident);
	}
	bOk &= TwPhysical(*this, Gpu, TEXT("after the LRU probes"));
	return bOk;
}

// Cell 7.21's GPU twin: RequestAdoptPrefix() on a lease not yet Ready is refused at the call (no handle) with a Malformed
// "not Ready" error, nothing queued (the sequence's pending count unchanged), and the same lease adopts once Ready. The
// lease is held Pending by the GPU build hold (Seam::SetBuildHold: the build stops after its begin). Rev 1.18.1 (N3): a
// live lease whose request resolved BudgetExhausted and was not returned reads the same; the BudgetExhausted request is
// charged 255 pages beside the held 2-page lease in a one-block C, so no pad is needed.
// Mutant: the adopt queued before Ready -> a handle and the pending count rises -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuAdoptNotReadyTest, "SuperSLM.PrefixCache.Gpu.AdoptNotReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuAdoptNotReadyTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	TArray<int32> T0;
	FSuperSLMPrefix Lease;
	FSuperSLMGpuSequence Seq;
	if (!TwPagesTokens(*this, N, 2, T0) || !Vend(*this, Gpu, Seq))
	{
		return false;
	}
	// A lease whose build is held at the build hold.
	Seam::SetBuildHold(Gpu, true);
	if (!TwRequest(*this, Gpu, TwSpec(T0), Lease, TEXT("held build")))
	{
		Seam::SetBuildHold(Gpu, false);
		return false;
	}
	Pump(Gpu, [&Gpu]() { return Seam::HeldBuildCount(Gpu) == 1; }, OpSeconds);
	bool bOk = TestEqual(TEXT("held build: the build stops at the build hold"), Seam::HeldBuildCount(Gpu), 1);
	bOk &= TestEqual(TEXT("held build: the lease is Pending"), (uint8)Gpu.GetPrefixLeaseState(Lease), (uint8)ELease::Pending);
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, Seq, Lease, TEXT("Malformed"), TEXT("held build"));
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, Seq, Lease, TEXT("not Ready"), TEXT("held build, again"));
	Seam::SetBuildHold(Gpu, false);
	bOk &= TwExpect(*this, Gpu, Lease, ELease::Ready, TEXT("after the hold"));
	bOk &= AdoptAndWait(*this, Gpu, Seq, Lease);
	ReturnAndDrain(Gpu, Seq);

	// Rev 1.18.1 (N3): a live lease that resolved BudgetExhausted, not returned (Lease stays leased beside it).
	TArray<int32> W;
	FSuperSLMPrefix LW;
	FSuperSLMGpuSequence Seq2;
	if (!TwNeedTokens(*this, N, TwCapC(Gpu) - 1, W) || !TwRequest(*this, Gpu, TwSpec(W), LW, TEXT("one more")) || !Vend(*this, Gpu, Seq2))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("one more: BudgetExhausted"), (uint8)TwResolve(Gpu, LW, OpSeconds), (uint8)ELease::BudgetExhausted);
	bOk &= TwAdoptRefusedAtCall(*this, Gpu, Seq2, LW, TEXT("not Ready"), TEXT("BudgetExhausted lease"));
	ReturnAndDrain(Gpu, Seq2);
	Gpu.ReturnPrefix(LW);
	Gpu.ReturnPrefix(Lease);
	DrainQuiet(Gpu);
	bOk &= TwPhysical(*this, Gpu, TEXT("7.21 end"));
	return bOk;
}

// Cell 8.8's GPU twin: a pooled sequence adopted from a cached prefix saves ('SLM6') and restores as today: the restored
// sequence's generation equals the unsaved twin's. The restore is hinted with the live lease. The blob is saved between
// the adopt and the first generation (the GPU paging restore suite's HintSharing pattern), not mid-turn as the CPU cell
// does, since the restored GPU sequence then runs the same one generation as the twin.
// Mutant: the adopter's position or KV not carried by the save -> the continuation differs -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuSaveRestoreAdopterTest, "SuperSLM.PrefixCache.Gpu.SaveRestoreAdopter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuSaveRestoreAdopterTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	constexpr int32 kNew = 8;
	TArray<int32> Tokens, Suffix, Reference, Got;
	FSuperSLMPrefix Lease;
	if (!TwPagesTokens(*this, N, 2, Tokens) || !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 2, Suffix)
		|| !TwLease(*this, N, TwSpec(Tokens), true, Lease, TEXT("lease")))
	{
		return false;
	}
	// The unsaved twin: adopt, then kNew tokens after the suffix in one generation.
	FSuperSLMGpuSequence Twin, Seq, Restored;
	if (!Vend(*this, Gpu, Twin) || !AdoptAndWait(*this, Gpu, Twin, Lease) || !GenerateComplete(*this, Gpu, Twin, Suffix, kNew, Reference))
	{
		return false;
	}
	ReturnAndDrain(Gpu, Twin);
	TArray<uint8> Blob;
	if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, Lease) || !SaveAndTake(*this, Gpu, Seq, Blob))
	{
		return false;
	}
	ReturnAndDrain(Gpu, Seq);
	bool bOk = TestEqual(TEXT("the restore succeeds"), (uint8)RestoreAndWait(*this, Gpu, Blob, N.Rig.Model, Restored, Lease), (uint8)ESuperSLMRestoreResult::Success);
	if (!Restored.IsValid())
	{
		return false;
	}
	bOk &= GenerateComplete(*this, Gpu, Restored, Suffix, kNew, Got);
	bOk &= TestEqual(TEXT("save/restore of a cache adopter continues as the unsaved twin"), Got, Reference);
	ReturnAndDrain(Gpu, Restored);
	bOk &= TwPhysical(*this, Gpu, TEXT("after the restore"));
	return bOk;
}

// Cell 8.15's GPU twin: parent entry P (origin 40 tokens: floor(origin / B) = 2 full pages) leased; a pooled restore of a
// child's blob (a holder that adopted P, saved before its first generation) hinted with P's lease shares floor(origin / B)
// of P's pages: read as the GPU paging suite reads a sharing restore (Restore.HintSharing) -- the restored holder's H hold
// is 0, where a private restore holds E = floor(origin / B), and the fill probe's draw is floor(origin / B) smaller than
// the private restore's. P stays pinned by its lease for the restore (a DropPrefix issued while the restore is held at its
// apply reads DropDeferred); A is pre-charged for the shared pages (form 1 clean); the restored child's generation equals
// the unsaved twin's. With P's lease returned (and the deferred drop applied) the same restore runs private.
// The CPU reads *out_shared_pages through LastRestoreSharedPages, which the GPU has not; the H hold and the probe are the
// GPU's readings of it.
// Mutants: the hint ignored -> the H hold reads E and the draws are equal -> red; the hint wired but A not pre-charged ->
// form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuLeaseAsRestoreHintTest, "SuperSLM.PrefixCache.Gpu.LeaseAsRestoreHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuLeaseAsRestoreHintTest::RunTest(const FString& Parameters)
{
	FGpuTwin N;
	if (!TwConfigure(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	const FGpuGeometry& G = N.Rig.Geo;
	constexpr int32 Origin = 40;
	constexpr int32 Budget = 512; // the configuration's SequenceBudget, the blob's saved budget
	constexpr int32 kNew = 8;
	TArray<int32> P, Suffix, Twin;
	FSuperSLMPrefix LP;
	if (!MakeTokens(*this, *N.Rig.Model, Origin, N.Salt++, P) || !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 2, Suffix)
		|| !TwLease(*this, N, TwSpec(P), true, LP, TEXT("P")))
	{
		return false;
	}
	const int64 S = Origin / G.B; // floor(origin / B), written from the plan
	// The unsaved twin: adopt P, then kNew tokens after the suffix in one generation.
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, LP) || !GenerateComplete(*this, Gpu, Seq, Suffix, kNew, Twin))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Seq);
	}
	// The child's blob: a holder that adopted P, saved before its first generation.
	TArray<uint8> Blob;
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, LP) || !SaveAndTake(*this, Gpu, Seq, Blob))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Seq);
	}
	const int64 Pool = LedgerOf(Gpu).PoolPages;

	// One restore of the child's blob with Hint, measured by the probe and the holder's H hold; the restored holder's
	// generation compared with the twin's.
	auto HintedRestore = [&](const FSuperSLMPrefix& Hint, bool bCheckPin, int64& OutDraw, int64& OutHold, const TCHAR* What)
	{
		int64 FreeBefore = -1, FreeAfter = -1;
		if (!DrainQuiet(Gpu) || !ProbeFree(*this, Gpu, Pool, FreeBefore))
		{
			return false;
		}
		bool bLeg = true;
		FIssuedGpuRestore R;
		if (bCheckPin)
		{
			const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
			R = IssueRestore(Gpu, Blob, N.Rig.Model, Hint);
			bLeg &= TestTrue(*FString::Printf(TEXT("%s: the restore is held at its apply (%s)"), What, *R.CallError), Hold.IsValid() && WaitHoldReached(Gpu, Hold));
			FSuperSLMPrefixOp Op;
			FString Error;
			const bool bAccepted = Gpu.DropPrefix(TwSpec(P), ESuperSLMPrefixDropScope::Memory, Op, Error);
			bLeg &= TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts (%s)"), What, *Error), bAccepted);
			bLeg &= TestEqual(*FString::Printf(TEXT("%s: P stays pinned by its lease for the restore (a drop is deferred)"), What),
				(uint8)TwOpResolved(Gpu, Op).Result, (uint8)ESuperSLMPrefixOpResult::DropDeferred);
			FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		}
		else
		{
			R = IssueRestore(Gpu, Blob, N.Rig.Model, Hint);
		}
		FSuperSLMGpuSequence Seq;
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: the restore succeeds (%s)"), What, *R.CallError), (uint8)SettleOutcome(Gpu, R, Seq), (uint8)ESuperSLMRestoreResult::Success);
		if (R.Handle.IsValid())
		{
			Gpu.ReleaseLifecycleOpHandle(R.Handle);
		}
		if (!Seq.IsValid())
		{
			return false;
		}
		OutHold = FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(Gpu, Seq);
		bLeg &= DrainQuiet(Gpu) && ProbeFree(*this, Gpu, Pool, FreeAfter);
		OutDraw = FreeBefore - FreeAfter;
		bLeg &= Form1ChargesCoverDrawn(*this, Gpu, *FString::Printf(TEXT("%s: A pre-charged for the shared pages"), What));
		bLeg &= NoLedgerFault(*this, Gpu, What);
		TArray<int32> Tokens;
		bLeg &= GenerateComplete(*this, Gpu, Seq, Suffix, kNew, Tokens);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: the restored child's generation equals the twin's"), What), Tokens, Twin);
		ReturnAndDrain(Gpu, Seq);
		return bLeg;
	};

	int64 SharedDraw = -1, SharedHold = -1, PrivateDraw = -1, PrivateHold = -1;
	bool bOk = HintedRestore(LP, true, SharedDraw, SharedHold, TEXT("P's live lease as the hint"));
	bOk &= TestEqual(TEXT("live lease: the holder's H hold is 0 (it shares floor(origin / B) pages)"), SharedHold, G.HeadroomHold(G.R(Budget), Budget));
	Gpu.ReturnPrefix(LP);
	DrainQuiet(Gpu);
	bOk &= HintedRestore(LP, false, PrivateDraw, PrivateHold, TEXT("P's lease returned"));
	bOk &= TestEqual(TEXT("returned lease: the restore runs private (H hold E = floor(origin / B))"), PrivateHold, G.HeadroomHold(G.RestoreCharge(Budget, Origin), Budget));
	bOk &= TestEqual(TEXT("E is floor(origin / B) at this geometry"), G.E(Budget, Origin), S);
	bOk &= TestEqual(*FString::Printf(TEXT("the sharing restore draws floor(origin / B) = %lld fewer pages than the private one (%lld vs %lld)"), S, SharedDraw, PrivateDraw),
		PrivateDraw - SharedDraw, S);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
