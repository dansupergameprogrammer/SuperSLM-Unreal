// Precache plan rev 1.17, the U6 suite's GPU twins: the GPU subsystem's cache instance (pooled mode), keyed by
// adapter vendor and device, with nested entries. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on; the
// restore legs need SUPERSLM_WITH_GPU_PREFIX_SAVE as well (the GPU prefix save and restore, unreleased in Layer 1
// 1.15.0). Red until the GPU subsystem installs its cache host (SuperSLMGpuSubsystemPrefixCache.h, edits 2-4).
//
//   SuperSLM.PrefixCache.Gpu.Instance.AdapterKey        §5.2 / PC-5 backend 1 + adapter ids in the key   [U6]
//   SuperSLM.PrefixCache.Gpu.Instance.MemoryHit         1.x twin: recompute, then a Memory hit           [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.GhostCharge         1.9 twin                                          [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.NullHint            8.12 twin (restore)                               [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.ParentKey           2.12 parent-key twin                              [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.WaiterPinsParent    8.14 (F1) twin                                    [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.GhostClaimable      8.14 (F2) (i) twin                                [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.OwnParent           8.14 (R2) twin, restore and build                [U6]
//
// The constructions are the CPU file's (SuperSLMPrefixCacheNestedTests.cpp), on the GPU pooled rig
// (A-EX, B = 16): the cache's region C sized by ConfigurePrefixCache, a leased pad leaving the
// construction's C, and the same "physical" reading (form 1 at the GPU ledger, no ledger fault, C
// never above its capacity). The rest of 1.x-10.x's [U] cells' GPU twins are not written here.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "RHIGlobals.h"
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

	bool ConfigureGpuNest(FAutomationTestBase& T, FGpuNest& N, int64 CPages, const FString& DiskDirectory = FString())
	{
		FGpuPagingRig& Rig = N.Rig;
		if (!Rig.Init(T) || !ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, 512)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		FSuperSLMGpuPrefixCacheSettings Settings;
		Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C
		Settings.bDiskEnabled = !DiskDirectory.IsEmpty();
		Settings.DiskDirectory = DiskDirectory;
		Settings.DiskMaxBytes = Settings.bDiskEnabled ? (int64(1) << 30) : 0;
		Rig.Gpu->ConfigurePrefixCache(Settings);
		if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, 512)))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("the GPU cache is configured, on region C"), Rig.Gpu->GetPrefixCacheStats().bConfigured && Rig.Gpu->GetPrefixCacheStats().bBudgetMode))
		{
			return false;
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
}

// §5.2 / PC-5: the GPU instance keys by backend 1 and the adapter's vendor and device ids, so the same
// spec names a different disk file on the CPU and on the GPU, and the GPU's identity is this adapter's.
// Red until U6: the GPU subsystem has no cache instance.
// Mutant: the GPU instance configured with the CPU identity (backend 0) -> the two files are equal.
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
	bOk &= TestEqual(TEXT("vendor: this adapter's"), Vendor, static_cast<uint32>(GRHIVendorId));
	bOk &= TestEqual(TEXT("device: this adapter's"), Device, static_cast<uint32>(GRHIDeviceId));
	TArray<int32> Tokens;
	if (!MakeTokens(*this, *N.Rig.Model, int32(N.B()), 1, Tokens))
	{
		return false;
	}
	const FString GpuFile = Seam::DiskFileOf(N.Gpu(), SuperSLMPrefixCacheFixtures::Spec(Tokens));
	bOk &= TestFalse(TEXT("the GPU file is named"), GpuFile.IsEmpty());
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
	bool bOk = Lease(*this, N, ChildSpec(P, DT), true, Again, TEXT("D restored, null hint"));
	bOk &= TestEqual(TEXT("from disk"), (uint8)Gpu.GetPrefixSource(Again), (uint8)ESuperSLMPrefixSource::Disk);
	bOk &= TestEqual(TEXT("private (s = 0)"), Seam::SharedPagesOf(Gpu, Again), int64(0));
	bOk &= Physical(*this, Gpu, TEXT("after the private restore"));
	Seam::SetNullRestoreHints(Gpu, false);
	return bOk;
#endif
}

// Cell 2.12's parent-key twin: equal suffixes under P1 and P2 are different keys and files; the
// second is not a Memory hit of the first. Mutant: the parent key not hashed -> a Memory hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedParentKeyTest, "SuperSLM.PrefixCache.Gpu.Nested.ParentKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedParentKeyTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-nested-parent-key"));
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 16, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P1, P2, A, Bl;
	TArray<int32> P1T, P2T, Suffix;
	if (!Root(*this, N, 2, true, P1, P1T, TEXT("P1")) || !Root(*this, N, 2, true, P2, P2T, TEXT("P2")) || !MakeTokens(*this, *N.Rig.Model, int32(N.B()), 7, Suffix))
	{
		return false;
	}
	bool bOk = Lease(*this, N, ChildSpec(P1, Suffix), true, A, TEXT("A under P1"));
	bOk &= Lease(*this, N, ChildSpec(P2, Suffix), true, Bl, TEXT("B under P2"));
	bOk &= TestNotEqual(TEXT("B is not a Memory hit of A"), (uint8)Gpu.GetPrefixSource(Bl), (uint8)ESuperSLMPrefixSource::Memory);
	bOk &= TestNotEqual(TEXT("different files"), Seam::DiskFileOf(Gpu, ChildSpec(P1, Suffix)), Seam::DiskFileOf(Gpu, ChildSpec(P2, Suffix)));
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

// Cell 8.14 (F2) (i) twin: C = 11; P (4) a ghost under D (2), E (4) leased; X (6) evicts D, claims
// the free 1 and 5 of D's 2 and the ghost's 4, waits, and is admitted at D's apply. Mutant: the
// ghost unclaimable -> X refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedGhostClaimableTest, "SuperSLM.PrefixCache.Gpu.Nested.GhostClaimable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedGhostClaimableTest::RunTest(const FString& Parameters)
{
	FGpuNest N;
	if (!ConfigureGpuNest(*this, N, 11))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = N.Gpu();
	FSuperSLMPrefix P, D, E, X;
	TArray<int32> PT, DT, ET, XT;
	if (!Root(*this, N, 4, true, P, PT, TEXT("P")) || !Child(*this, N, P, 2, true, D, DT, TEXT("D")) || !Root(*this, N, 4, true, E, ET, TEXT("E")) || !NeedTokens(*this, N, 6, XT))
	{
		return false;
	}
	Gpu.ReturnPrefix(P);
	DrainQuiet(Gpu);
	bool bOk = MakeGhost(*this, N, 4, TEXT("P"));
	Gpu.ReturnPrefix(D);
	DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	FString Error;
	bOk &= Gpu.RequestPrefix(SuperSLMPrefixCacheFixtures::Spec(XT), X, Error);
	TickN(Gpu);
	bOk &= TestEqual(TEXT("X waits on D's refund"), (uint8)Gpu.GetPrefixLeaseState(X), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= Physical(*this, Gpu, TEXT("before D's apply"));
	Seam::HoldReleases(Gpu, false);
	bOk &= Expect(*this, Gpu, X, ESuperSLMPrefixLeaseState::Ready, TEXT("X at D's apply"));
	bOk &= Physical(*this, Gpu, TEXT("after D's apply"));
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

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
