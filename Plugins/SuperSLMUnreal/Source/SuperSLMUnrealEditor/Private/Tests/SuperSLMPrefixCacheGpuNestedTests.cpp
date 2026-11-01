// Precache plan rev 1.17, the U6 suite's GPU twins: the GPU subsystem's cache instance (pooled mode), keyed by
// adapter vendor and device, with nested entries. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on; the
// restore legs need SUPERSLM_WITH_GPU_PREFIX_SAVE as well (the GPU prefix save and restore).
//
//   SuperSLM.PrefixCache.Gpu.Instance.AdapterKey        §5.2 / PC-5 backend 1 + adapter ids in the key   [U6]
//   SuperSLM.PrefixCache.Gpu.Instance.MemoryHit         1.x twin: recompute, then a Memory hit           [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.GhostCharge         1.9 twin                                          [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.GhostThreeLevels    1.9 twin, three levels P <- C <- G                [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.NullHint            8.12 twin (restore; fits, then cannot fit)        [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.ParentKey           2.12 parent-key twin, with the twin tokens        [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.WaiterPinsParent    8.14 (F1) twin                                    [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.GhostClaimable      8.14 (F2) (i) and (ii) twin                       [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.SiblingsOutOfOrder  8.14 (R1) twin, D1's release gated behind D2's    [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.Chain               8.14 (R1) twin, e0 <- e1 <- e2 <- e3              [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.ParentAfterChild    8.14 (rev 1.17) twin                              [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.OwnParent           8.14 (R2) twin, restore and build                [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.RegionA             8.14 nested-A (Q6, R36): A for a nested parent    [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.Refusals            §5.6 nested-spec refusals twin (no legacy leg)    [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.AdoptFullLength     6.x nested: adopt at parent + suffix, twin tokens [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.AdoptCapFullLength  6.x nested: the cap check counts the whole length [U6]
//
// The constructions are the CPU file's (SuperSLMPrefixCacheNestedTests.cpp), on the GPU pooled rig
// (A-EX, B = 16): the cache's region C sized by ConfigurePrefixCache, a leased pad leaving the
// construction's C, and the same "physical" reading (form 1 at the GPU ledger, no ledger fault, C
// never above its capacity). The rest of 1.x-10.x's [U] cells' GPU twins are not written here.
//
// The prefill twin on the GPU (ParentKey, RegionA, AdoptFullLength): the whole prompt built by CreatePrefix (outside
// the cache, on region P, so those cells configure one P block), adopted by a fresh sequence and generated with the
// same suffix; a cache lease's adopter must produce its tokens. Gated readings in the cells added at U6's twin pass
// settle with SettleGpu (the GPU twin of the CPU fixture's SettleCacheIo: a request's key hash lands a Tick later).

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "Misc/FileHelper.h"
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

	struct FGpuNest
	{
		FGpuPagingRig Rig;
		FSuperSLMPrefix Pad;
		int32 Salt = 200;
		USuperSLMGpuSubsystem& Gpu() { return *Rig.Gpu; }
		int64 B() const { return Rig.Geo.B; }
	};

	int64 CapC(const USuperSLMGpuSubsystem& Gpu) { return LedgerOf(Gpu).CapacityOf(ERegion::C); }

	// CPages <= 0: no pad (the cell does not need an exact C). PrefixBlocks 1 for a cell that builds a prefill twin with
	// CreatePrefix (region P). SequenceBudget: the pooled sequences' budget (AdoptCapFullLength needs the cap to bind).
	bool ConfigureGpuNest(FAutomationTestBase& T, FGpuNest& N, int64 CPages, const FString& DiskDirectory = FString(), int32 PrefixBlocks = 0,
		int32 SequenceBudget = 512)
	{
		FGpuPagingRig& Rig = N.Rig;
		if (!Rig.Init(T) || !ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, SequenceBudget, PrefixBlocks)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		FSuperSLMGpuPrefixCacheSettings Settings;
		Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C
		Settings.bDiskEnabled = !DiskDirectory.IsEmpty();
		Settings.DiskDirectory = DiskDirectory;
		Settings.DiskMaxBytes = Settings.bDiskEnabled ? (int64(1) << 30) : 0;
		Rig.Gpu->ConfigurePrefixCache(Settings);
		if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, SequenceBudget, PrefixBlocks)))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("the GPU cache is configured, on region C"), Rig.Gpu->GetPrefixCacheStats().bConfigured && Rig.Gpu->GetPrefixCacheStats().bBudgetMode))
		{
			return false;
		}
		if (CPages <= 0)
		{
			return true;
		}
		const int64 PadPages = CapC(*Rig.Gpu) - CPages;
		if (PadPages <= 0)
		{
			return T.TestEqual(TEXT("the construction's C is the region's"), PadPages, int64(0));
		}
		TArray<int32> PadTokens;
		FString Error;
		return MakeTokens(T, *Rig.Model, int32(PadPages * Rig.Geo.B), N.Salt++, PadTokens)
			&& Rig.Gpu->RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(PadTokens), N.Pad, Error)
			&& Pump(*Rig.Gpu, [&]() { return Rig.Gpu->GetPrefixLeaseState(N.Pad) == ESuperSLMPrefixLeaseState::Ready; }, BuildSeconds);
	}

	FSuperSLMPrefixSpec ChildSpec(const FSuperSLMPrefix& Parent, const TArray<int32>& Suffix)
	{
		FSuperSLMPrefixSpec S = SuperSLMPrefixCacheFixtures::Spec(Suffix);
		S.Parent = Parent;
		return S;
	}

	ESuperSLMPrefixLeaseState Resolve(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease)
	{
		Pump(Gpu, [&]() { return Gpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, BuildSeconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool Expect(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixLeaseState Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s resolves"), What), (uint8)Resolve(Gpu, Lease), (uint8)Want);
	}

	bool Lease(FAutomationTestBase& T, FGpuNest& N, const FSuperSLMPrefixSpec& Spec, bool bKeep, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("%s requested (%s)"), What, *Error), N.Gpu().RequestPrefix(Spec, Out, Error))
			|| !Expect(T, N.Gpu(), Out, ESuperSLMPrefixLeaseState::Ready, What))
		{
			return false;
		}
		if (!bKeep)
		{
			N.Gpu().ReturnPrefix(Out);
		}
		return DrainQuiet(N.Gpu());
	}

	bool Root(FAutomationTestBase& T, FGpuNest& N, int64 Pages, bool bKeep, FSuperSLMPrefix& Out, TArray<int32>& OutTokens, const TCHAR* What)
	{
		return MakeTokens(T, *N.Rig.Model, int32(Pages * N.B()), N.Salt++, OutTokens) && Lease(T, N, SuperSLMPrefixCacheFixtures::Spec(OutTokens), bKeep, Out, What);
	}

	bool Child(FAutomationTestBase& T, FGpuNest& N, const FSuperSLMPrefix& Parent, int64 Pages, bool bKeep, FSuperSLMPrefix& Out, TArray<int32>& OutSuffix, const TCHAR* What)
	{
		return MakeTokens(T, *N.Rig.Model, int32(Pages * N.B()), N.Salt++, OutSuffix) && Lease(T, N, ChildSpec(Parent, OutSuffix), bKeep, Out, What);
	}

	bool NeedTokens(FAutomationTestBase& T, FGpuNest& N, int64 Need, TArray<int32>& Out)
	{
		return MakeTokens(T, *N.Rig.Model, int32((Need - 1) * N.B()), N.Salt++, Out);
	}

	bool Physical(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		bool bOk = Form1ChargesCoverDrawn(T, Gpu, Instant);
		bOk &= NoLedgerFault(T, Gpu, Instant);
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C));
		return bOk;
	}

	// A refused request of GhostPages + 1 that evicts the one unleased entry: a ghost of GhostPages.
	bool MakeGhost(FAutomationTestBase& T, FGpuNest& N, int64 GhostPages, const TCHAR* What)
	{
		const FSuperSLMPrefixCacheStats S0 = N.Gpu().GetPrefixCacheStats();
		TArray<int32> Tokens;
		FSuperSLMPrefix Probe;
		FString Error;
		if (!NeedTokens(T, N, GhostPages + 1, Tokens) || !N.Gpu().RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(Tokens), Probe, Error))
		{
			return false;
		}
		bool bOk = Expect(T, N.Gpu(), Probe, ESuperSLMPrefixLeaseState::BudgetExhausted, What);
		bOk &= DrainQuiet(N.Gpu());
		const FSuperSLMPrefixCacheStats S1 = N.Gpu().GetPrefixCacheStats();
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: a ghost of its charge"), What), S1.GhostBytes - S0.GhostBytes, GhostPages * N.Rig.Geo.PageBytes);
		bOk &= Physical(T, N.Gpu(), What);
		return bOk;
	}

	void TickN(USuperSLMGpuSubsystem& Gpu, int32 N = 4)
	{
		for (int32 I = 0; I < N; ++I)
		{
			Gpu.Tick(Step);
		}
	}

	bool IsPending(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Probe)
	{
		return Gpu.GetPrefixLeaseState(Probe) == ESuperSLMPrefixLeaseState::Pending;
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

	// The GPU twin of SuperSLMPrefixCacheFixtures::SettleCacheIo: IO settled, every submission-thread job drained (a
	// held release is not posted, so it is not outstanding), IO settled again.
	bool SettleGpu(USuperSLMGpuSubsystem& Gpu, double Seconds = OpSeconds)
	{
		const bool bFirst = SettleGpuPhase(Gpu, Seconds);
		DrainQuiet(Gpu, Seconds);
		return SettleGpuPhase(Gpu, Seconds) && bFirst;
	}

	// Requests Attempts one-page roots (each charged R(B) = 2 in flight, 1 once Ready) and returns each; counts the
	// admissions; the physical reading after each (the CPU nested suite's AdmitToBudget).
	bool AdmitToBudget(FAutomationTestBase& T, FGpuNest& N, int32 Attempts, int32& OutAdmitted, const TCHAR* What)
	{
		bool bOk = true;
		OutAdmitted = 0;
		for (int32 I = 0; I < Attempts; ++I)
		{
			TArray<int32> Tokens;
			FSuperSLMPrefix Probe;
			FString Error;
			if (!MakeTokens(T, *N.Rig.Model, int32(N.B()), N.Salt++, Tokens))
			{
				return false;
			}
			const bool bRequested = N.Gpu().RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(Tokens), Probe, Error);
			if (!T.TestTrue(*FString::Printf(TEXT("%s: requested (%s)"), What, *Error), bRequested))
			{
				return false;
			}
			if (Resolve(N.Gpu(), Probe) == ESuperSLMPrefixLeaseState::Ready)
			{
				++OutAdmitted;
			}
			N.Gpu().ReturnPrefix(Probe);
			bOk &= DrainQuiet(N.Gpu());
			bOk &= Physical(T, N.Gpu(), *FString::Printf(TEXT("%s, admission %d"), What, I));
		}
		return bOk;
	}

	// Adopts Lease into a fresh sequence and generates TwinNewTokens after Suffix; returns the tokens.
	bool GpuAdopterTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Leased, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq) || !AdoptAndWait(T, Gpu, Seq, Leased))
		{
			return false;
		}
		const bool bOk = GenerateComplete(T, Gpu, Seq, Suffix, SuperSLMPrefixCacheFixtures::TwinNewTokens, OutTokens);
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	// The prefill twin: Tokens built by CreatePrefix (region P, outside the cache), adopted and generated the same way;
	// the prefix is released after.
	bool GpuTwinTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMPrefix Twin;
		if (!BuildPrefix(T, Gpu, Tokens, Twin))
		{
			return false;
		}
		const bool bOk = GpuAdopterTokens(T, Gpu, Twin, Suffix, OutTokens);
		return ReleasePrefixAndDrain(T, Gpu, Twin) && bOk;
	}

	bool GpuEqualsTwin(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Leased, const TArray<int32>& Suffix, const TArray<int32>& Twin, const TCHAR* What)
	{
		TArray<int32> Tokens;
		return GpuAdopterTokens(T, Gpu, Leased, Suffix, Tokens)
			&& T.TestEqual(*FString::Printf(TEXT("%s: the adopter's tokens equal the prefill twin's"), What), Tokens, Twin);
	}
}

// §5.2 / PC-5: the GPU instance keys by backend 1 and the adapter's vendor and device ids, so the same
// spec names a different disk file on the CPU and on the GPU, and the GPU's identity is this adapter's.
// "This adapter" is the one Layer 1 runs on, read
// back from a real GPU save: the SPX1 header of the entry's write-back (backend at offset 4, vendor at 20, device at
// 24), not the RHI's adapter (GRHIVendorId, GRHIDeviceId), which names another GPU under -nullrhi or on a machine
// whose RHI and Layer 1 use different adapters.
// Mutants: the GPU instance configured with the CPU identity (backend 0) -> the two files are equal; keyed by the
// RHI's ids where they differ from Layer 1's -> the identity's ids differ from the save's header.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuAdapterKeyTest, "SuperSLM.PrefixCache.Gpu.Instance.AdapterKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuAdapterKeyTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-adapter-key"));
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 8, Dir.Path))
	{
		return false;
	}
	uint32 Vendor = 0, Device = 0;
	bool bOk = TestTrue(TEXT("the instance reports its adapter ids"), Seam::AdapterIdsOf(N.Gpu(), Vendor, Device));
	// A real GPU save: one entry recomputed and written back (sslm_gpu_prefix_save on the submission thread, then the
	// file), whose SPX1 header names the producer adapter the engine itself saw.
	const int64 WritesBefore = N.Gpu().GetPrefixCacheStats().DiskWritesIssued;
	FSuperSLMPrefix Saved;
	TArray<int32> Tokens;
	if (!Root(*this, N, 1, false, Saved, Tokens, TEXT("the saved entry")))
	{
		return false;
	}
	Pump(N.Gpu(), [&N, WritesBefore]() { return N.Gpu().GetPrefixCacheStats().DiskWritesIssued > WritesBefore; }, BuildSeconds);
	bOk &= TestTrue(TEXT("the entry's write-back is issued"), N.Gpu().GetPrefixCacheStats().DiskWritesIssued > WritesBefore);
	bOk &= TestTrue(TEXT("the write-back lands (IO settled)"), SettleGpu(N.Gpu()));
	const FString GpuFile = Seam::DiskFileOf(N.Gpu(), SuperSLMPrefixCacheFixtures::Spec(Tokens));
	bOk &= TestFalse(TEXT("the GPU file is named"), GpuFile.IsEmpty());
	TArray<uint8> Wrapper;
	uint32 SavedBackend = 0, SavedVendor = 0, SavedDevice = 0;
	bOk &= TestTrue(*FString::Printf(TEXT("the save's file reads (%s)"), *GpuFile), FFileHelper::LoadFileToArray(Wrapper, *GpuFile));
	bOk &= TestTrue(TEXT("the save's payload is an SPX1 blob"), Seam::ReadWrapperPayloadProducer(Wrapper, SavedBackend, SavedVendor, SavedDevice));
	bOk &= TestEqual(TEXT("the save's backend is the GPU's (1)"), SavedBackend, 1u);
	bOk &= TestEqual(TEXT("vendor: the adapter Layer 1 saved on (the SPX1 header's, offset 20)"), Vendor, SavedVendor);
	bOk &= TestEqual(TEXT("device: the adapter Layer 1 saved on (the SPX1 header's, offset 24)"), Device, SavedDevice);
	bOk &= TestNotEqual(TEXT("another adapter's ids name another file"), GpuFile, Seam::DiskFileOfWithAdapter(N.Gpu(), SuperSLMPrefixCacheFixtures::Spec(Tokens), Vendor ^ 1u, Device));
	bOk &= TestNotEqual(TEXT("another device's ids name another file"), GpuFile, Seam::DiskFileOfWithAdapter(N.Gpu(), SuperSLMPrefixCacheFixtures::Spec(Tokens), Vendor, Device ^ 1u));
	return bOk;
}

// The 1.x root path's twin: a request recomputes (sslm_gpu_prefix_begin, prefill, freeze), Ready; the
// same spec again is a Memory hit with no second build; Physical throughout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuMemoryHitTest, "SuperSLM.PrefixCache.Gpu.Instance.MemoryHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuMemoryHitTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 8))
	{
		return false;
	}
	FSuperSLMPrefix A, A2;
	TArray<int32> AT;
	if (!Root(*this, N, 2, false, A, AT, TEXT("A")))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("A recomputed"), (uint8)N.Gpu().GetPrefixSource(A), (uint8)ESuperSLMPrefixSource::Recomputed);
	const int64 Builds = N.Gpu().GetPrefixCacheStats().BuildsIssued;
	bOk &= Lease(*this, N, SuperSLMPrefixCacheFixtures::Spec(AT), true, A2, TEXT("A again"));
	bOk &= TestEqual(TEXT("A again: Memory"), (uint8)N.Gpu().GetPrefixSource(A2), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestEqual(TEXT("no second build"), N.Gpu().GetPrefixCacheStats().BuildsIssued, Builds);
	bOk &= Physical(*this, N.Gpu(), TEXT("after the hit"));
	return bOk;
}

// Cell 1.9's GPU twin: C = 9; P (4) leased with children D1, D2 (1 private each, begin_from), L (2)
// leased; P returned and evicted under its children (a ghost of 4); nothing admitted while the
// children are leased; every admission fits once they leave; Physical throughout.
// Mutant: the ghost charge dropped -> form 1 red at the GPU ledger.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedGhostChargeTest, "SuperSLM.PrefixCache.Gpu.Nested.GhostCharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedGhostChargeTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 9))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, D1, D2, L;
	TArray<int32> PT, D1T, D2T, LT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, D1, D1T, TEXT("D1")) || !Child(*this, N, P, 1, true, D2, D2T, TEXT("D2"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")))
	{
		return false;
	}
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bool bOk = MakeGhost(*this, N, 4, TEXT("P under its children"));
	TArray<int32> XT;
	FSuperSLMPrefix X;
	FString Error;
	bOk &= NeedTokens(*this, N, 2, XT) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error);
	bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("a root while the ghost and its children hold C"));
	Gpu.ReturnPrefix(D1);
	Gpu.ReturnPrefix(D2);
	DrainQuiet(Gpu);
	for (int32 I = 0; I < 6; ++I)
	{
		FSuperSLMPrefix Y;
		TArray<int32> YT;
		bOk &= Root(*this, N, 1, false, Y, YT, TEXT("an admission once the children leave"));
		bOk &= Physical(*this, Gpu, TEXT("admitting"));
	}
	bOk &= TestEqual(TEXT("the ghost ended with its last descendant"), Gpu.GetPrefixCacheStats().Ghosts, 0);
	return bOk;
}

// Cell 1.9, three levels, GPU twin of Cpu.Nested.GhostThreeLevels: C = 7; P <- Mid <- G (2, 1, 1 pages, each child
// begin_from its parent), L (2) leased, 1 free. P evicted (a ghost of 2: G still maps its pages through Mid), then Mid
// (a ghost of 1); both ghosts held (3 pages); nothing admitted while G is leased; every admission fits once G leaves,
// and both ghosts end at G's eviction; Physical after each admission.
// Mutant: the ghost cleared at the last CHILD -> P ends when Mid leaves although G maps P's pages -> the admissions draw
// over them -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedGhostThreeLevelsTest, "SuperSLM.PrefixCache.Gpu.Nested.GhostThreeLevels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedGhostThreeLevelsTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 7))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, Mid, G, L;
	TArray<int32> PT, MidT, GT, LT;
	if (!Root(*this, N, 2, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, Mid, MidT, TEXT("Mid (child of P)")) || !Child(*this, N, Mid, 1, true, G, GT, TEXT("G (child of Mid)"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")))
	{
		return false;
	}
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bool bOk = MakeGhost(*this, N, 2, TEXT("P evicted"));
	Gpu.ReturnPrefix(Mid);
	DrainQuiet(Gpu);
	bOk &= MakeGhost(*this, N, 1, TEXT("Mid evicted"));
	bOk &= TestEqual(TEXT("two ghosts: P (2) and Mid (1), 3 pages"), Gpu.GetPrefixCacheStats().GhostBytes, 3 * N.Rig.Geo.PageBytes);
	bOk &= TestEqual(TEXT("two ghosts"), Gpu.GetPrefixCacheStats().Ghosts, 2);
	int32 Admitted = 0;
	bOk &= AdmitToBudget(*this, N, 2, Admitted, TEXT("G leased"));
	bOk &= TestEqual(TEXT("nothing admitted while G holds both ghosts"), Admitted, 0);
	Gpu.ReturnPrefix(G);
	DrainQuiet(Gpu);
	bOk &= AdmitToBudget(*this, N, 6, Admitted, TEXT("G returned"));
	bOk &= TestEqual(TEXT("every admission fits once G leaves"), Admitted, 6);
	bOk &= TestEqual(TEXT("both ghosts ended at G's eviction"), Gpu.GetPrefixCacheStats().Ghosts, 0);
	return bOk;
}

// Cell 8.12's GPU twin: C = 10; P (4) leased; D (child, 2 private) written back, evicted, requested
// again under Seam::SetNullRestoreHints: sslm_gpu_prefix_restore with a null hint (s = 0), charged
// ceil(L/B) = 6 before the draw; Physical; the same restore when it cannot fit -> BudgetExhausted.
// Mutant: charged ceil(L/B) - floor(origin/B) -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedNullHintTest, "SuperSLM.PrefixCache.Gpu.Nested.NullHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedNullHintTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-nested-null-hint"));
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 10, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, D, Big, Again;
	TArray<int32> PT, DT, BigT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 2, false, D, DT, TEXT("D")) || !NeedTokens(*this, N, 6, BigT)
		|| !Lease(*this, N, SuperSLMPrefixCacheFixtures::Spec(BigT), false, Big, TEXT("evicts D")))
	{
		return false;
	}
	Seam::SetNullRestoreHints(Gpu, true);
	const FSuperSLMPrefixCacheStats SR = Gpu.GetPrefixCacheStats();
	bool bOk = Lease(*this, N, ChildSpec(P, DT), true, Again, TEXT("D restored, null hint"));
	bOk &= TestEqual(TEXT("from disk"), (uint8)Gpu.GetPrefixSource(Again), (uint8)ESuperSLMPrefixSource::Disk);
	bOk &= TestEqual(TEXT("one restore issued"), Gpu.GetPrefixCacheStats().RestoresIssued - SR.RestoresIssued, int64(1));
	bOk &= TestEqual(TEXT("private (s = 0)"), Seam::SharedPagesOf(Gpu, Again), int64(0));
	bOk &= Physical(*this, Gpu, TEXT("after the private restore"));
	bOk &= TestEqual(TEXT("C full: P 4 + D 6 (private)"), LedgerOf(Gpu).FreeOf(ERegion::C), int64(0));

	// The same restore when it cannot fit (the CPU twin's second leg): D's restored entry returned, D2 (a child of P, 1
	// private page) evicts it, a leased root of 5 evicts D2, leaving P 4 + 5 leased and 1 free. D requested again (a
	// null-hint restore charged 6) resolves BudgetExhausted by name, with no retry as a build.
	FSuperSLMPrefix D2, Hold, DAgain;
	TArray<int32> D2T, HoldT;
	Gpu.ReturnPrefix(Again);
	DrainQuiet(Gpu);
	if (!Child(*this, N, P, 1, false, D2, D2T, TEXT("D2 (evicts D)")) || !Root(*this, N, 5, true, Hold, HoldT, TEXT("a root of 5 (evicts D2), leased")))
	{
		Seam::SetNullRestoreHints(Gpu, false);
		return false;
	}
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	FString Error;
	const bool bRequested = Gpu.RequestPrefix(ChildSpec(P, DT), DAgain, Error);
	bOk &= TestTrue(*FString::Printf(TEXT("D requested again (%s)"), *Error), bRequested);
	if (bRequested)
	{
		bOk &= Expect(*this, Gpu, DAgain, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("D again, a restore charged 6 that cannot fit"));
	}
	bOk &= TestEqual(TEXT("no build issued for it"), Gpu.GetPrefixCacheStats().BuildsIssued, S1.BuildsIssued);
	bOk &= Physical(*this, Gpu, TEXT("after the refusal"));
	Seam::SetNullRestoreHints(Gpu, false);
	return bOk;
#endif
}

// Cell 2.12's parent-key twin: equal suffixes under P1 and P2 are different keys and files; the
// second is not a Memory hit of the first; B's adopter's tokens equal its own prefill twin's (P2's
// tokens plus the suffix, built by CreatePrefix, adopted, the same generation suffix). No pad: the
// cell needs no exact C. One region-P block for the twin. Mutant: the parent key not hashed -> a
// Memory hit (of A, under P1: B's adopter then decodes P1's continuation, not its twin's).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedParentKeyTest, "SuperSLM.PrefixCache.Gpu.Nested.ParentKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedParentKeyTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-nested-parent-key"));
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 0, Dir.Path, /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P1, P2, A, Bl;
	TArray<int32> P1T, P2T, Suffix, TwinSuffix, Twin;
	if (!Root(*this, N, 2, true, P1, P1T, TEXT("P1")) || !Root(*this, N, 2, true, P2, P2T, TEXT("P2")) || !MakeTokens(*this, *N.Rig.Model, int32(N.B()), 7, Suffix)
		|| !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 8, TwinSuffix))
	{
		return false;
	}
	bool bOk = Lease(*this, N, ChildSpec(P1, Suffix), true, A, TEXT("A under P1"));
	bOk &= Lease(*this, N, ChildSpec(P2, Suffix), true, Bl, TEXT("B under P2"));
	bOk &= TestNotEqual(TEXT("B is not a Memory hit of A"), (uint8)Gpu.GetPrefixSource(Bl), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestNotEqual(TEXT("different files"), Seam::DiskFileOf(Gpu, ChildSpec(P1, Suffix)), Seam::DiskFileOf(Gpu, ChildSpec(P2, Suffix)));
	TArray<int32> Whole = P2T;
	Whole.Append(Suffix);
	bOk &= GpuTwinTokens(*this, Gpu, Whole, TwinSuffix, Twin);
	bOk &= GpuEqualsTwin(*this, Gpu, Bl, TwinSuffix, Twin, TEXT("B's adopter"));
	return bOk;
}

// Cell 8.14 (F1) twin: C = 13; V (3), P (4, named by a lease returned after the request), Q (3), L (2)
// leased; W (child of P, 3) waits on V and pins P; X (6) evicts Q and is refused; at V's apply W's
// begin_from runs against a live P (P never released). Mutant: no pin while waiting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedWaiterPinsParentTest, "SuperSLM.PrefixCache.Gpu.Nested.WaiterPinsParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedWaiterPinsParentTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 13))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix V, P, Q, L, W, X;
	TArray<int32> VT, PT, QT, LT, WS, XT;
	if (!Root(*this, N, 3, false, V, VT, TEXT("V")) || !Root(*this, N, 4, true, P, PT, TEXT("P")) || !Root(*this, N, 3, false, Q, QT, TEXT("Q"))
		|| !Root(*this, N, 2, true, L, LT, TEXT("L")) || !MakeTokens(*this, *N.Rig.Model, int32(2 * N.B()), N.Salt++, WS) || !NeedTokens(*this, N, 6, XT))
	{
		return false;
	}
	const uint64 PHandle = Seam::EngineHandleOf(Gpu, P);
	Seam::HoldReleases(Gpu, true);
	FString Error;
	bool bOk = Gpu.RequestPrefix(ChildSpec(P, WS), W, Error) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error);
	TickN(Gpu);
	Gpu.ReturnPrefix(P);
	TickN(Gpu);
	bOk &= TestEqual(TEXT("W waits"), (uint8)Gpu.GetPrefixLeaseState(W), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("X"));
	bOk &= TestEqual(TEXT("V and Q evicted, never P"), Seam::HeldReleaseCount(Gpu), 2);
	bOk &= Physical(*this, Gpu, TEXT("decided, not applied"));
	Seam::HoldReleases(Gpu, false);
	bOk &= Expect(*this, Gpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W at V's apply"));
	bOk &= TestEqual(TEXT("P never released"), Seam::ReleasesOf(Gpu, PHandle), 0);
	bOk &= Physical(*this, Gpu, TEXT("W built"));
	return bOk;
}

// Cell 8.14 (F2) twin, both legs: C = 11 (1 free); P (4) a ghost under D (2 private), whose release
// applies; E (4) leased.
//   (i)  X needs 6: the free 1, then it evicts D and claims 5 of D's 2 and the ghost's 4, waits, and
//        is admitted at D's apply.
//   (ii) a fresh run: X needs 2 (the free 1 and 1 of D's), Y needs 4 (D's other 1 and 3 of the
//        ghost's), one settle; both wait on D's refund (one held release); physical never above C
//        before D's apply; both admitted at it.
// Mutants: the ghost refunded at D's decision -> (ii) X admitted at once and draws while D maps P's
// pages -> form 1 red; the ghost unclaimable -> (i) X refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedGhostClaimableTest, "SuperSLM.PrefixCache.Gpu.Nested.GhostClaimable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedGhostClaimableTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	for (int32 Leg = 0; Leg < 2; ++Leg)
	{
		FGpuNest N;
		if (!ConfigureGpuNest(*this, N, 11))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = N.Gpu();
		FSuperSLMPrefix P, D, E;
		TArray<int32> PT, DT, ET;
		if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 2, true, D, DT, TEXT("D")) || !Root(*this, N, 4, true, E, ET, TEXT("E")))
		{
			return false;
		}
		Gpu.ReturnPrefix(P);
		DrainQuiet(Gpu);
		bOk &= MakeGhost(*this, N, 4, TEXT("P"));
		Gpu.ReturnPrefix(D);
		DrainQuiet(Gpu);
		Seam::HoldReleases(Gpu, true);
		FString Error;
		if (Leg == 0)
		{
			FSuperSLMPrefix X;
			TArray<int32> XT;
			bOk &= NeedTokens(*this, N, 6, XT) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error);
			SettleGpu(Gpu);
			bOk &= TestTrue(TEXT("(i) X waits on D's refund (D's 2 and the ghost's 4)"), IsPending(Gpu, X));
			bOk &= Physical(*this, Gpu, TEXT("(i) before D's apply"));
			Seam::HoldReleases(Gpu, false);
			bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("(i) X at D's apply"));
		}
		else
		{
			FSuperSLMPrefix X, Y;
			TArray<int32> XT, YT;
			bOk &= NeedTokens(*this, N, 2, XT) && NeedTokens(*this, N, 4, YT);
			bOk &= Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(YT), Y, Error);
			SettleGpu(Gpu);
			bOk &= TestTrue(TEXT("(ii) X waits"), IsPending(Gpu, X));
			bOk &= TestTrue(TEXT("(ii) Y waits"), IsPending(Gpu, Y));
			bOk &= TestEqual(TEXT("(ii) one eviction (D)"), Seam::HeldReleaseCount(Gpu), 1);
			bOk &= Physical(*this, Gpu, TEXT("(ii) before D's apply"));
			Seam::HoldReleases(Gpu, false);
			bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) X"));
			bOk &= Expect(*this, Gpu, Y, ESuperSLMPrefixLeaseState::Ready, TEXT("(ii) Y"));
		}
		bOk &= Physical(*this, Gpu, TEXT("after D's apply"));
	}
	return bOk;
}

// Cell 8.14 (re-strike R1) twin, siblings with releases out of order: C = 11 (the plan's 10 plus
// one free page); P (4) shared by D1 and D2 (1 private each); E (4) leased. P becomes a ghost. X
// needs 2: the free 1, evicts D1, claims 1, waits. Y needs 5: evicts D2 (ending the ghost), claims
// D2's 1 and the ghost's 4, waits. D1's release is gated behind D2's (Seam::DispatchHeldRelease of
// index 1): at D2's apply nobody is admitted and physical stays within C; at D1's apply both are.
// Mutant: the ghost refunded at the ending descendant's apply -> Y admitted at D2's apply, drawing
// 5 while D1 maps P's 4 -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedSiblingsTest, "SuperSLM.PrefixCache.Gpu.Nested.SiblingsOutOfOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedSiblingsTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 11))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, D1, D2, E, X, Y;
	TArray<int32> PT, D1T, D2T, ET, XT, YT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 1, true, D1, D1T, TEXT("D1")) || !Child(*this, N, P, 1, true, D2, D2T, TEXT("D2"))
		|| !Root(*this, N, 4, true, E, ET, TEXT("E")) || !NeedTokens(*this, N, 2, XT) || !NeedTokens(*this, N, 5, YT))
	{
		return false;
	}
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bool bOk = MakeGhost(*this, N, 4, TEXT("P"));
	Gpu.ReturnPrefix(D1);
	Gpu.ReturnPrefix(D2);
	DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	FString Error;
	bOk &= Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(YT), Y, Error);
	SettleGpu(Gpu);
	bOk &= TestTrue(TEXT("X waits"), IsPending(Gpu, X));
	bOk &= TestTrue(TEXT("Y waits"), IsPending(Gpu, Y));
	if (!TestEqual(TEXT("D1 and D2 evicted, releases held in that order"), Seam::HeldReleaseCount(Gpu), 2))
	{
		Seam::HoldReleases(Gpu, false);
		DrainQuiet(Gpu, 10.0);
		return false;
	}
	// D2's release (index 1) first.
	bOk &= TestTrue(TEXT("D2's release dispatched"), Seam::DispatchHeldRelease(Gpu, 1));
	SettleGpu(Gpu);
	bOk &= TestTrue(TEXT("at D2's apply, X still waits"), IsPending(Gpu, X));
	bOk &= TestTrue(TEXT("at D2's apply, Y still waits (the ghost's family holds D1)"), IsPending(Gpu, Y));
	bOk &= Physical(*this, Gpu, TEXT("D2 applied, D1 held"));
	Seam::HoldReleases(Gpu, false);
	bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("X at D1's apply"));
	bOk &= Expect(*this, Gpu, Y, ESuperSLMPrefixLeaseState::Ready, TEXT("Y at D1's apply"));
	bOk &= Physical(*this, Gpu, TEXT("both admitted"));
	return bOk;
}

// Cell 8.14 (re-strike R1) twin, a chain: C = 9 (1 free); e0 <- e1 <- e2 <- e3 (1 page each), e1 and
// L (4) leased; LRU e0, e2, e3. Q needing 5 evicts e0 (a ghost), e2 (a ghost), e3 (ending e2's
// ghost) and is refused. e3's release applies, then e0's; e2's own stays held. Z (3: the free 1, e3's
// 1, and e2's ghost's 1) then waits on e2's own release; physical never above C; Z is admitted at
// e2's apply. Mutant: the ghost refunded at e3's apply -> Z admitted at once, drawing 3 while e2's
// handle maps its page -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedChainTest, "SuperSLM.PrefixCache.Gpu.Nested.Chain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedChainTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 9))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix E0, E1, E2, E3, L, Q, Z;
	TArray<int32> T0, T1, T2, T3, LT, QT, ZT;
	if (!Root(*this, N, 1, true, E0, T0, TEXT("e0")) || !Child(*this, N, E0, 1, true, E1, T1, TEXT("e1")) || !Child(*this, N, E1, 1, true, E2, T2, TEXT("e2"))
		|| !Child(*this, N, E2, 1, true, E3, T3, TEXT("e3")) || !Root(*this, N, 4, true, L, LT, TEXT("L")) || !NeedTokens(*this, N, 5, QT) || !NeedTokens(*this, N, 3, ZT))
	{
		return false;
	}
	// LRU order e0, e2, e3: return them in that order (e1 and L stay leased).
	Gpu.ReturnPrefix(E0);
	Gpu.ReturnPrefix(E2);
	Gpu.ReturnPrefix(E3);
	DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	FString Error;
	bool bOk = TestTrue(TEXT("Q (5) requested"), Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(QT), Q, Error));
	bOk &= Expect(*this, Gpu, Q, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("Q"));
	SettleGpu(Gpu);
	if (!TestEqual(TEXT("e0, e2 and e3 evicted, releases held"), Seam::HeldReleaseCount(Gpu), 3))
	{
		Seam::HoldReleases(Gpu, false);
		DrainQuiet(Gpu, 10.0);
		return false;
	}
	// e3's release (index 2) first, then e0's (index 0); e2's own stays held.
	bOk &= TestTrue(TEXT("e3 dispatched"), Seam::DispatchHeldRelease(Gpu, 2));
	bOk &= TestTrue(TEXT("e0 dispatched"), Seam::DispatchHeldRelease(Gpu, 0));
	SettleGpu(Gpu);
	bOk &= TestTrue(TEXT("Z (3) requested"), Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(ZT), Z, Error));
	SettleGpu(Gpu);
	bOk &= TestTrue(TEXT("Z waits on e2's own release"), IsPending(Gpu, Z));
	bOk &= Physical(*this, Gpu, TEXT("e2 held"));
	Seam::HoldReleases(Gpu, false);
	bOk &= Expect(*this, Gpu, Z, ESuperSLMPrefixLeaseState::Ready, TEXT("Z at e2's apply"));
	bOk &= Physical(*this, Gpu, TEXT("Z admitted"));
	return bOk;
}

// Cell 8.14 (rev 1.17) twin, a parent evicted after its child, not a ghost: C = 8 (1 free); e1 (3,
// child of e0) LRU, then e0 (3); L (1) leased. W needs 5: the free 1, evicts e1 (3), then e0 (no
// ghost, but e1's handle still maps its pages; W claims 1, 2 unclaimed). X needs 2 and claims e0's 2.
// e0's release applies first (index 1): nobody is admitted; at e1's apply both are. Mutant: family
// sets on ghosts only -> X admitted at e0's apply -> form 1 red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedParentAfterChildTest, "SuperSLM.PrefixCache.Gpu.Nested.ParentAfterChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedParentAfterChildTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 8))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix E0, E1, L, W, X;
	TArray<int32> T0, T1, LT, WT, XT;
	if (!Root(*this, N, 1, true, L, LT, TEXT("L")) || !Root(*this, N, 3, true, E0, T0, TEXT("e0")) || !Child(*this, N, E0, 3, false, E1, T1, TEXT("e1"))
		|| !NeedTokens(*this, N, 5, WT) || !NeedTokens(*this, N, 2, XT))
	{
		return false;
	}
	Gpu.ReturnPrefix(E0); // e0 is touched after e1: e1 is LRU
	DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	FString Error;
	bool bOk = Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(WT), W, Error) && Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error);
	bOk &= TestTrue(TEXT("W and X requested"), bOk);
	SettleGpu(Gpu);
	if (!TestEqual(TEXT("e1 then e0 evicted"), Seam::HeldReleaseCount(Gpu), 2))
	{
		Seam::HoldReleases(Gpu, false);
		DrainQuiet(Gpu, 10.0);
		return false;
	}
	bOk &= TestEqual(TEXT("e0 is no ghost"), Gpu.GetPrefixCacheStats().Ghosts, 0);
	bOk &= TestTrue(TEXT("e0's release dispatched first"), Seam::DispatchHeldRelease(Gpu, 1));
	SettleGpu(Gpu);
	bOk &= TestTrue(TEXT("at e0's apply W still waits"), IsPending(Gpu, W));
	bOk &= TestTrue(TEXT("at e0's apply X still waits (e0's family holds e1)"), IsPending(Gpu, X));
	bOk &= Physical(*this, Gpu, TEXT("e0 applied, e1 held"));
	Seam::HoldReleases(Gpu, false);
	bOk &= Expect(*this, Gpu, W, ESuperSLMPrefixLeaseState::Ready, TEXT("W at e1's apply"));
	bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("X at e1's apply"));
	bOk &= Physical(*this, Gpu, TEXT("both admitted"));
	return bOk;
}

// Cell 8.14 (R2) twin: C = 7; P (3) unpinned, L (3) leased, 1 free. A nested restore of a child of P
// (from disk, charged 4) evicts P and runs private; a nested build of the same shape is refused and
// P is not released. Mutants: the restore's loop skips its parent; the build's loop evicts it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedOwnParentTest, "SuperSLM.PrefixCache.Gpu.Nested.OwnParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedOwnParentTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	for (int32 Leg = 0; Leg < 2; ++Leg)
	{
		const bool bRestore = Leg == 0;
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
		if (bRestore)
		{
			continue; // the restore leg needs the 1.15.0 GPU restore
		}
#endif
		SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-nested-own-parent"));
		FGpuNest N;
		if (!ConfigureGpuNest(*this, N, 7, Dir.Path))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = N.Gpu();
		FSuperSLMPrefix P, D, Fill, L, Again;
		TArray<int32> PT, DT, FillT, LT;
		if (!Root(*this, N, 3, true, P, PT, TEXT("P")))
		{
			return false;
		}
		if (bRestore)
		{
			if (!Child(*this, N, P, 1, false, D, DT, TEXT("D, written back")) || !Root(*this, N, 3, false, Fill, FillT, TEXT("fill (evicts D)")))
			{
				return false;
			}
		}
		else if (!MakeTokens(*this, *N.Rig.Model, int32(3 * N.B()), N.Salt++, DT))
		{
			return false;
		}
		if (!Root(*this, N, 3, true, L, LT, TEXT("L")))
		{
			return false;
		}
		const uint64 PHandle = Seam::EngineHandleOf(Gpu, P);
		FString Error;
		if (!Gpu.RequestPrefix(ChildSpec(P, DT), Again, Error))
		{
			return false;
		}
		Gpu.ReturnPrefix(P);
		if (bRestore)
		{
			bOk &= Expect(*this, Gpu, Again, ESuperSLMPrefixLeaseState::Ready, TEXT("the restore"));
			bOk &= TestEqual(TEXT("private (s = 0)"), Seam::SharedPagesOf(Gpu, Again), int64(0));
		}
		else
		{
			bOk &= Expect(*this, Gpu, Again, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("the build"));
			bOk &= TestEqual(TEXT("P not released"), Seam::ReleasesOf(Gpu, PHandle), 0);
		}
		bOk &= Physical(*this, Gpu, bRestore ? TEXT("after the restore") : TEXT("after the refusal"));
	}
	return bOk;
}

// Cell 8.14, nested-A (rev 1.18, PC-15 Q6; the U6 draft's R36), GPU twin of Cpu.Nested.RegionA (in
// SuperSLMPrefixCacheGpuLedgerTests.cpp). C = 4: P (2 full pages, leased) with cached child D (1
// private page, leased), one page free; one region-P block for the twin. A budget sequence adopts D
// through RequestAdoptPrefix: A is pre-charged with D's 3 full pages, P's 2 included. P is returned,
// evicted under D (a ghost of 2 on C) and its release applies: form 1 at that reading counts P's
// pages on both C (the ghost) and A (the adopter's dependency). D is evicted by X (needs 4: the free
// 1, D's 1 and the ghost's 2) and its release applies: no ghost is left (C refunded it), A still
// carries the 3 pages, Physical holds, and the adopter still decodes its prefill twin's tokens
// (CreatePrefix of P + D's tokens, adopted, the same suffix).
// Mutant: A not pre-charged for the parent's pages through the child -> after the refund the adopter
// holds uncharged pages -> form 1 red (or A reads 1, not 3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedRegionATest, "SuperSLM.PrefixCache.Gpu.Nested.RegionA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedRegionATest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 4, FString(), /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	const int64 CPad = Charged(Gpu, ERegion::C);
	const int64 A0 = Charged(Gpu, ERegion::A);
	FSuperSLMPrefix P, D, X;
	TArray<int32> PT, DT, XT, Suffix;
	if (!Root(*this, N, 2, true, P, PT, TEXT("P (2 full pages)")) || !Child(*this, N, P, 1, true, D, DT, TEXT("D (child of P, 1 private page)"))
		|| !NeedTokens(*this, N, 4, XT) || !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, N.Salt++, Suffix))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P 2 + D 1 on C, one page free"), Charged(Gpu, ERegion::C) - CPad, int64(3));

	// A budget sequence adopts D: paging pre-charges A with D's full pages, P's included.
	FSuperSLMGpuSequence Adopter;
	if (!Vend(*this, Gpu, Adopter) || !AdoptAndWait(*this, Gpu, Adopter, D))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the adopter pre-charges A with D's 3 full pages (P's 2 included)"), Charged(Gpu, ERegion::A) - A0, int64(3));

	// P evicted under D (a ghost on C); its release applies (MakeGhost drains).
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bOk &= MakeGhost(*this, N, 2, TEXT("P evicted under D"));
	{
		const TCHAR* Instant = TEXT("the required reading (P a ghost, its release applied)");
		bOk &= TestEqual(TEXT("one ghost (P)"), Gpu.GetPrefixCacheStats().Ghosts, 1);
		bOk &= TestEqual(TEXT("C counts P's pages (the ghost 2) and D's 1"), Charged(Gpu, ERegion::C) - CPad, int64(3));
		bOk &= TestEqual(TEXT("A counts P's pages through the adopter's dependency on D (3)"), Charged(Gpu, ERegion::A) - A0, int64(3));
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, Instant);
		bOk &= NoLedgerFault(*this, Gpu, Instant);
	}

	// D evicted by X; D's apply ends the ghost and admits X.
	Gpu.ReturnPrefix(D);
	DrainQuiet(Gpu);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bOk &= Lease(*this, N, SuperSLMPrefixCacheFixtures::Spec(XT), true, X, TEXT("X (needs 4: evicts D)"));
	SettleGpu(Gpu);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("one eviction (D)"), S1.Evictions - S0.Evictions, int64(1));
	bOk &= TestEqual(TEXT("C has refunded the ghost (no ghost)"), S1.Ghosts, 0);
	bOk &= TestEqual(TEXT("C has refunded the ghost (no ghost bytes)"), S1.GhostBytes, int64(0));
	bOk &= TestEqual(TEXT("A still carries the adopter's pages (3)"), Charged(Gpu, ERegion::A) - A0, int64(3));
	bOk &= Physical(*this, Gpu, TEXT("after D's eviction and release"));

	// The adopter still decodes its twin's tokens.
	TArray<int32> Tokens, Twin;
	bOk &= GenerateComplete(*this, Gpu, Adopter, Suffix, SuperSLMPrefixCacheFixtures::TwinNewTokens, Tokens);
	ReturnAndDrain(Gpu, Adopter);
	TArray<int32> Whole = PT;
	Whole.Append(DT);
	bOk &= GpuTwinTokens(*this, Gpu, Whole, Suffix, Twin);
	bOk &= TestEqual(TEXT("the adopter's tokens equal the prefill twin's (P + D's tokens, CreatePrefix)"), Tokens, Twin);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("the end"));
	return bOk;
}

// §5.6 at U6, GPU twin of Cpu.Nested.Refusals: a nested spec naming a schema of its own, a parent's
// length plus the suffix above ContextCap, and a parent that is not a live Ready lease (an ended
// one) are refused at the request (RequestPrefix false). No pad (no build but P's). The CPU cell's
// legacy leg (NestedRequiresBudgetMode) is N/A: the GPU cache has no legacy mode.
// Mutant: a check dropped -> that request accepted -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedRefusalsTest, "SuperSLM.PrefixCache.Gpu.Nested.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedRefusalsTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, Out;
	TArray<int32> PT, Suffix, TooLong;
	FString Error;
	if (!Root(*this, N, 2, true, P, PT, TEXT("P")) || !MakeTokens(*this, *N.Rig.Model, int32(N.B()), N.Salt++, Suffix)
		|| !MakeTokens(*this, *N.Rig.Model, int32(N.Rig.Geo.Cp) - PT.Num() + 1, N.Salt++, TooLong))
	{
		return false;
	}
	FSuperSLMPrefixSpec WithSchema = ChildSpec(P, Suffix);
	WithSchema.SchemaName = FName(TEXT("any"));
	bool bOk = TestFalse(TEXT("a nested spec naming a schema is refused"), Gpu.RequestPrefix(WithSchema, Out, Error));
	bOk &= TestFalse(TEXT("parent length plus suffix above ContextCap is refused"), Gpu.RequestPrefix(ChildSpec(P, TooLong), Out, Error));
	const FSuperSLMPrefix Ended = P;
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bOk &= TestFalse(TEXT("an ended lease is no parent"), Gpu.RequestPrefix(ChildSpec(Ended, Suffix), Out, Error));
	return bOk;
}

// The 6.x nested case (plugin 1.3.0 review round, blocking item 1), GPU twin of
// Cpu.Nested.AdoptFullLength: a root lease (20 tokens) and a nested child (12 more), the child
// adopted through RequestAdoptPrefix: the sequence's context after the adopt is parent + suffix
// (32), it is armed, its generation completes, the position oracle (cell 10.4 (a): the 'SLM6'
// header's context_length) reads L + s + n - 1 at the whole L, and its tokens equal the whole-prompt
// prefill twin's (CreatePrefix of the 32 tokens, adopted, the same suffix). The CPU cell's Exact
// leg (schema-bound skip-ahead) has no GPU twin here: the GPU cache's child specs carry no schema of
// their own and this rig binds none. No pad; one region-P block for the twin.
// Mutant: the lease's length read as the suffix alone -> context 12 -> red (and the oracle red).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedAdoptFullLengthTest, "SuperSLM.PrefixCache.Gpu.Nested.AdoptFullLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedAdoptFullLengthTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 0, FString(), /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	constexpr int32 RootLength = 20;
	constexpr int32 SuffixLength = 12;
	const int64 WholeLength = RootLength + SuffixLength;
	TArray<int32> RootT, SuffixT, GenSuffix;
	FSuperSLMPrefix RootLease, ChildLease;
	if (!MakeTokens(*this, *N.Rig.Model, RootLength, N.Salt++, RootT) || !MakeTokens(*this, *N.Rig.Model, SuffixLength, N.Salt++, SuffixT)
		|| !MakeTokens(*this, *N.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, N.Salt++, GenSuffix)
		|| !Lease(*this, N, SuperSLMPrefixCacheFixtures::Spec(RootT), true, RootLease, TEXT("the root lease"))
		|| !Lease(*this, N, ChildSpec(RootLease, SuffixT), true, ChildLease, TEXT("the nested lease")))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, ChildLease))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("after the nested adopt the context is parent + suffix"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), WholeLength);
	bOk &= TestEqual(TEXT("armed after the adopt"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);
	TArray<int32> Tokens, Twin;
	bOk &= GenerateComplete(*this, Gpu, Seq, GenSuffix, SuperSLMPrefixCacheFixtures::TwinNewTokens, Tokens);
	bOk &= PositionOracle(*this, Gpu, Seq, WholeLength + GenSuffix.Num() + SuperSLMPrefixCacheFixtures::TwinNewTokens - 1, TEXT("the nested adopter"));
	ReturnAndDrain(Gpu, Seq);
	TArray<int32> Whole = RootT;
	Whole.Append(SuffixT);
	bOk &= GpuTwinTokens(*this, Gpu, Whole, GenSuffix, Twin);
	bOk &= TestEqual(TEXT("the nested adopter's tokens equal the whole-prompt prefill twin's"), Tokens, Twin);
	Gpu.ReturnPrefix(ChildLease);
	Gpu.ReturnPrefix(RootLease);
	DrainQuiet(Gpu);
	return bOk;
}

// The 6.x nested case, GPU twin of Cpu.Nested.AdoptCapFullLength: the activation check after a
// nested adopt counts the whole length. SequenceBudget = ContextCap, so the adopter's limit is
// ContextCap (origin + budget runs past it) whatever the origin. A prompt of ContextCap - (parent +
// suffix) + 1 tokens, which would fit after the suffix alone, is refused at its activation (§3.4
// "Activation pre-check"): accepted at the call, the sequence faults with the cap outcome
// (RecoverablePerSequenceRejection) before any generated token. No pad, no region P.
// Mutant: the adopter's context read as the suffix alone -> the prompt fits -> it prefills and
// generates -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedAdoptCapFullLengthTest, "SuperSLM.PrefixCache.Gpu.Nested.AdoptCapFullLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedAdoptCapFullLengthTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 0, FString(), /*PrefixBlocks*/ 0, /*SequenceBudget*/ int32(PluginCap)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	constexpr int32 RootLength = 20;
	constexpr int32 SuffixLength = 12;
	const int64 WholeLength = RootLength + SuffixLength;
	const int64 Cap = N.Rig.Geo.Cp;
	if (!TestTrue(TEXT("the cap is known and above the nested length"), Cap > WholeLength)
		|| !TestTrue(TEXT("the adopter's limit is ContextCap at either origin (SequenceBudget = ContextCap)"), int64(N.Rig.Config.SequenceBudget) >= Cap))
	{
		return false;
	}
	TArray<int32> RootT, SuffixT;
	FSuperSLMPrefix RootLease, ChildLease;
	if (!MakeTokens(*this, *N.Rig.Model, RootLength, N.Salt++, RootT) || !MakeTokens(*this, *N.Rig.Model, SuffixLength, N.Salt++, SuffixT)
		|| !Lease(*this, N, SuperSLMPrefixCacheFixtures::Spec(RootT), true, RootLease, TEXT("the root lease"))
		|| !Lease(*this, N, ChildSpec(RootLease, SuffixT), true, ChildLease, TEXT("the nested lease")))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, ChildLease))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("after the nested adopt the context is parent + suffix"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), WholeLength);
	// One token past the cap after parent + suffix; it would fit after the suffix alone (RootLength >= 1).
	const int32 PromptCount = int32(Cap - WholeLength + 1);
	TArray<int32> Prompt;
	Prompt.Init(RootT[0], PromptCount);
	FGpuGeneration G;
	bOk &= Generate(*this, Gpu, Seq, Prompt, 1, G);
	AddInfo(FString::Printf(TEXT("the over-cap prompt (%d tokens after %lld): phase %d, fault %s, %d tokens; %s"),
		PromptCount, WholeLength, (int32)G.Phase, FaultName(G.Fault), G.Tokens.Num(), *G.Error));
	bOk &= TestFalse(TEXT("the prompt is accepted at the call (refused at its activation)"), G.bRefusedAtCall);
	bOk &= TestEqual(TEXT("the over-cap prompt is refused: the sequence faults"), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the cap outcome (RecoverablePerSequenceRejection)"), (uint8)G.Fault, (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection);
	bOk &= TestEqual(TEXT("the refusal comes before any generated token"), G.Tokens.Num(), 0);
	ReturnAndDrain(Gpu, Seq);
	Gpu.ReturnPrefix(ChildLease);
	Gpu.ReturnPrefix(RootLease);
	DrainQuiet(Gpu);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
