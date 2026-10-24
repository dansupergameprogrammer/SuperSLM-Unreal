// Precache plan rev 1.17, CPU suite: the plugin cache's lifecycle, handles, memory tier, coalescing,
// threading and 1.0.0 composition (§5.2-§5.6, §5.9, §5.10). Preprocessed out unless
// SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.Churn                    cell 1.4 (and 7.10)                  [U2]
//   SuperSLM.PrefixCache.Cpu.WarmReconfigure          cell 1.5                             [U2]
//   SuperSLM.PrefixCache.Cpu.ReconfigureInFlight      cell 1.7 (and its rev 1.18.8 leg)    [U2, U5]
//   SuperSLM.PrefixCache.Cpu.HandleDispositions       cell 2.10 (§5.10)                    [U2]
//   SuperSLM.PrefixCache.Cpu.UnknownSchema            cell 2.11                            [U2]
//   SuperSLM.PrefixCache.Cpu.KeyComponents            cell 2.12 (tokens, schema, length)   [U2]
//   SuperSLM.PrefixCache.Cpu.Coalescing               cell 3.5                             [U2]
//   SuperSLM.PrefixCache.Cpu.EvictionVsQueuedAdopt    cell 3.6                             [U2]
//   SuperSLM.PrefixCache.Cpu.GameThreadOwnership      cell 3.9 (and its rev 1.18 drop leg)  [U2]
//   SuperSLM.PrefixCache.Cpu.AllPinned                cell 5.5 (and 7.10)                  [U2]
//   SuperSLM.PrefixCache.Cpu.OffGameThreadWork        cell 7.8 (and its rev 1.18.1 leg)     [U2]
//   SuperSLM.PrefixCache.Cpu.MemoryAndLru             cell 7.13                            [U2]
//   SuperSLM.PrefixCache.Cpu.SaveRestoreAdopter       cell 8.8                             [U2]
//   SuperSLM.PrefixCache.Cpu.Beside100                cell 8.10 (a), (b), (c) (and 7.10)   [U2]
//   SuperSLM.PrefixCache.Cpu.PrefixLeaseEndedOrdinal  §5.11 (b) enum order                 [U2]
//
// The disk tier's cells are in SuperSLMPrefixCacheCpuDiskTests.cpp; the ledger cell 8.14 in
// SuperSLMPrefixCacheCpuLedgerTests.cpp; the save boundary 9.11 in SuperSLMPrefixCacheSaveCompatTests.cpp.
//
// Fixture note: the plan's 8.10 names a small-cap plugin fixture (B = 64, cap 256, cap_pages = 4).
// The plugin has no such artifact; the cell is written at A-EX's geometry (B = 64, cap 4,096,
// cap_pages = 64) with the same margins (1.0.0 prefixes of L = cap, cache builds of L = 2B), which
// the cell's own text allows ("a larger divisible cap with cap_pages >= 3").

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "Math/RandomStream.h"
#include "SuperSLMBlueprintTypes.h" // ESuperSLMRestoreResultBP, the PrefixLeaseEndedOrdinal cell's mirror

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	using Seam = FSuperSLMPrefixCacheTestSeam;

	// Inits the rig, reads the geometry under a cache-off legacy configuration, then configures the
	// cache: budget mode (SequenceBudget 512) unless bLegacy. BudgetPages is the cache's budget in
	// pages' bytes; in budget mode C rounds it up to whole blocks (§5.11 (b)).
	bool CacheRig(FAutomationTestBase& T, FPagingRig& Rig, int64 BudgetPages, bool bLegacy = false, int32 BlockCount = 4, int32 PrefixBlocks = 1)
	{
		if (!Rig.Init(T) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(T, Rig) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		const int64 Budget = BytesForPages(Rig.Geo, BudgetPages);
		return bLegacy ? ConfigureCacheLegacy(T, Rig, WithCache(MakeConfig(BlockCount, 0, PrefixBlocks, 1), Budget))
					   : ConfigureCacheBudget(T, Rig, WithCache(MakeConfig(BlockCount, 512, PrefixBlocks, 1), Budget));
	}

	// The tokens of an entry resident at Pages pages.
	bool PagesTokens(FAutomationTestBase& T, FPagingRig& Rig, int64 Pages, int32 Salt, TArray<int32>& Out)
	{
		return MakeTokens(T, *Rig.Cpu, int32(Pages * Rig.Geo.B), Salt, Out);
	}
}

// Cell 1.4 (and 7.10): churn. 500 requests over 40 keys, a budget of 10 one-page entries (legacy
// mode, so the budget is exact bytes), random leases and returns; entries both recompute-built and
// restore-built (the memory tier's re-request of an evicted key with a disk file restores: the disk
// tier is on here). At every 10th tick boundary, with the lanes parked, creates totalling
// F_min = N - R_seq - floor(budget / page) are admitted, N being the pool's free reading once the cache is configured
// and before any request (legacy mode adds the cache's blocks to a pool whose other blocks the warm sequences hold:
// before them nothing is free, and the first run, 2026-10-24, read N = -1 there). A request that cannot be charged resolves
// BudgetExhausted; leased entries are never evicted; the pool ends at its initial count after
// ReturnPrefix of all and a re-Configure().
// Mutant: a build that draws without a charge -> F_min refused at some reading -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuChurnTest, "SuperSLM.PrefixCache.Cpu.Churn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPrefixCacheCpuChurnTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("churn"));
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(2, 0, 0, 1)) || !ReadGeometry(*this, Rig) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	constexpr int64 BudgetPagesN = 10;
	const int64 Budget = BytesForPages(Rig.Geo, BudgetPagesN);
	if (!ConfigureCacheLegacy(*this, Rig, WithCache(MakeConfig(2, 0, 0, 1), Budget, Dir.Path, int64(1) << 30)))
	{
		return false;
	}
	int64 Initial = -1;
	if (!InitialFree(*this, Cpu, Initial))
	{
		return false;
	}
	const int64 ReserveSeq = 0; // the probe's free reading N is taken after the warm reservations
	TArray<TArray<int32>> Keys;
	for (int32 K = 0; K < 40; ++K)
	{
		TArray<int32> Tokens;
		if (!MakeTokens(*this, Cpu, int32(Rig.Geo.B), 100 + K, Tokens))
		{
			return false;
		}
		Keys.Add(Tokens);
	}
	FRandomStream Rng(14);
	TArray<FSuperSLMPrefix> Live;
	bool bOk = true;
	int32 Exhausted = 0;
	for (int32 I = 0; I < 500; ++I)
	{
		if (Live.Num() > 0 && Rng.FRand() < 0.5f)
		{
			const int32 At = Rng.RandRange(0, Live.Num() - 1);
			bOk &= TestTrue(TEXT("ReturnPrefix() ends a live lease"), Cpu.ReturnPrefix(Live[At]));
			Live.RemoveAtSwap(At);
		}
		FSuperSLMPrefix Lease;
		if (!Request(*this, Cpu, Spec(Keys[Rng.RandRange(0, 39)]), Lease, TEXT("churn")))
		{
			return false;
		}
		const ESuperSLMPrefixLeaseState State = WaitLease(Cpu, Lease);
		if (State == ESuperSLMPrefixLeaseState::Ready)
		{
			Live.Add(Lease);
		}
		else
		{
			bOk &= TestEqual(TEXT("a request that cannot be charged resolves BudgetExhausted"), (uint8)State, (uint8)ESuperSLMPrefixLeaseState::BudgetExhausted);
			++Exhausted;
		}
		if (I % 10 == 9)
		{
			bOk &= CacheWithinBudget(*this, Cpu, Initial, ReserveSeq, 0, BudgetPagesN, *FString::Printf(TEXT("request %d"), I + 1));
		}
		for (const FSuperSLMPrefix& L : Live)
		{
			bOk &= TestEqual(TEXT("a leased entry is never evicted"), (uint8)Cpu.GetPrefixLeaseState(L), (uint8)ESuperSLMPrefixLeaseState::Ready);
		}
	}
	const FSuperSLMPrefixCacheStats S = Stats(Cpu);
	bOk &= TestTrue(TEXT("entries were recompute-built"), Served(S, ESuperSLMPrefixSource::Recomputed) > 0);
	bOk &= TestTrue(TEXT("entries were restore-built (disk)"), Served(S, ESuperSLMPrefixSource::Disk) > 0);
	AddInfo(FString::Printf(TEXT("churn: %d BudgetExhausted, %lld evictions"), Exhausted, S.Evictions));
	for (const FSuperSLMPrefix& L : Live)
	{
		Cpu.ReturnPrefix(L);
	}
	DrainQuiet(Cpu);
	bOk &= TeardownClean(*this, Rig, Initial, TEXT("after ReturnPrefix of all"));
	return bOk;
}

// Cell 1.5: a warm cache re-Configure()d releases every cached handle (release-counting seam: each
// once), and the next request is a miss served from the next tier (Recomputed with the disk off).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuWarmReconfigureTest, "SuperSLM.PrefixCache.Cpu.WarmReconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuWarmReconfigureTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<FSuperSLMPrefixSpec> Specs;
	int64 Initial = -1;
	if (!InitialFree(*this, Cpu, Initial) || !FillUnleased(*this, Cpu, 3, int32(Rig.Geo.B), 1, Specs))
	{
		return false;
	}
	FSuperSLMPrefix Held;
	if (!RequestReady(*this, Cpu, Specs[0], Held, TEXT("a leased entry")))
	{
		return false;
	}
	const uint64 HeldEngine = Seam::EngineHandleOf(Cpu, Held);
	bool bOk = TeardownClean(*this, Rig, Initial, TEXT("warm re-Configure()"));
	bOk &= TestEqual(TEXT("the leased entry's engine handle was released once"), Seam::ReleasesOf(Cpu, HeldEngine), 1);
	bOk &= TestEqual(TEXT("the old lease is not a handle of this configuration"), (uint8)Cpu.GetPrefixLeaseState(Held), (uint8)ESuperSLMPrefixLeaseState::Invalid);
	FSuperSLMPrefix Next;
	bOk &= RequestReady(*this, Cpu, Specs[0], Next, TEXT("after re-Configure()"));
	bOk &= ExpectSource(*this, Cpu, Next, ESuperSLMPrefixSource::Recomputed, TEXT("a miss served by the next tier"));
	return bOk;
}

// Cell 1.7: re-Configure() while a cache build, and separately a cache restore, is (a) queued and
// (b) running on a lane: the request resolves Cancelled by name; the teardown fill probe reads the
// initial count; the pool destroy returns SSLM_OK; every engine handle released exactly once.
// Rev 1.18.8 (§15.27, §5.3 item 4): the build leg also runs between two chunk jobs of a sliced build. The chunk hold
// stops the build after its first chunk job; the re-Configure() comes while it waits. The request reads Cancelled,
// no further chunk is issued (none at the teardown's deliveries, none in the next configuration's ledger), the
// unfrozen prefix is released exactly once, and the pool destroy returns SSLM_OK. Written red first: at 15a617f01 the
// build is one job and the seam does not exist. Mutants: a chunk issued after the teardown (the delivery ignoring
// it) -> ChunkJobsIssuedInTeardown reads 1 -> red; the unfrozen prefix left live -> the pool destroy reads
// SSLM_POOL_HAS_LIVE_HANDLES and the fill probe reads short -> red; released twice -> the release seam reads 2 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuReconfigureInFlightTest, "SuperSLM.PrefixCache.Cpu.ReconfigureInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuReconfigureInFlightTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("reconfigure"));
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig))
	{
		return false;
	}
	const FSuperSLMRuntimeConfig C = WithCache(MakeConfig(2, 512, 1, 1), BytesForPages(Rig.Geo, 64), Dir.Path, int64(1) << 30);
	if (!ConfigureCacheBudget(*this, Rig, C))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens;
	if (!MakeTokens(*this, Cpu, 600, 1, Tokens))
	{
		return false;
	}
	// Warm the disk file so the restore legs restore: build once, re-Configure (memory dropped).
	{
		FSuperSLMPrefix L;
		if (!RequestReady(*this, Cpu, Spec(Tokens), L, TEXT("disk warm-up")) || !DrainQuiet(Cpu) || !ConfigureCacheBudget(*this, Rig, C))
		{
			return false;
		}
	}
	bool bOk = true;
	struct FLeg { const TCHAR* Name; bool bRestore; bool bRunning; };
	const FLeg Legs[] = {
		{ TEXT("build, queued"), false, false }, { TEXT("build, running"), false, true },
		{ TEXT("restore, queued"), true, false }, { TEXT("restore, running"), true, true } };
	for (const FLeg& Leg : Legs)
	{
		int64 Initial = -1;
		if (!InitialFree(*this, Cpu, Initial))
		{
			return false;
		}
		TArray<int32> Use = Tokens;
		if (!Leg.bRestore)
		{
			Use[0] = Use[0] == 1 ? 2 : 1; // a key with no disk file: a build
		}
		const ESuperSLMPagedRequestKind Kind = Leg.bRestore ? ESuperSLMPagedRequestKind::Restore : ESuperSLMPagedRequestKind::PrefixBuild;
		const FSuperSLMPagedHold Hold = Leg.bRunning ? FSuperSLMPagedTestSeam::HoldApply(Cpu, Kind) : FSuperSLMPagedTestSeam::HoldStart(Cpu, Kind);
		FSuperSLMPrefix L;
		if (!Request(*this, Cpu, Spec(Use), L, Leg.Name))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the job reaches its hold"), Leg.Name), WaitHoldReached(Cpu, Hold));
		bOk &= TeardownClean(*this, Rig, Initial, Leg.Name);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the request resolves by name"), Leg.Name), (uint8)Cpu.GetPrefixLeaseState(L), (uint8)ESuperSLMPrefixLeaseState::Cancelled);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: every engine handle released exactly once"), Leg.Name), Seam::MaxReleasesOfOneHandle(Cpu) <= 1, true);
	}

	// Rev 1.18.8: between two chunk jobs of a sliced build (600 tokens at MaxPrefillChunkBudget 64: ten chunk jobs).
	{
		const TCHAR* Name = TEXT("build, between chunks");
		int64 Initial = -1;
		if (!InitialFree(*this, Cpu, Initial))
		{
			return false;
		}
		TArray<int32> Use = Tokens;
		Use[1] = Use[1] == 1 ? 2 : 1; // a key with no disk file, and not the legs' above: a build
		const int64 UnfrozenBefore = Seam::UnfrozenBuildReleases(Cpu);
		const int64 IssuedBefore = Seam::ChunkJobsIssuedInTeardown(Cpu);
		Seam::SetBuildChunkHold(Cpu, 1);
		FSuperSLMPrefix L;
		if (!Request(*this, Cpu, Spec(Use), L, Name))
		{
			return false;
		}
		Drain(Cpu, [&Cpu, &L]() { return Seam::BuildChunkJobsOf(Cpu, L) >= 1; }, OpSeconds);
		for (int32 Tick = 0; Tick < 8; ++Tick)
		{
			Cpu.Tick(Step); // the build waits at the hold: no second chunk job
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: one chunk job ran, and the build waits for its next"), Name), Seam::BuildChunkJobsOf(Cpu, L), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the build is begun and unfrozen"), Name), (uint8)Seam::BuildStateOf(Cpu, L), (uint8)ESuperSLMPrefixBuildState::Begun);
		bOk &= TeardownClean(*this, Rig, Initial, Name);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the request reads Cancelled"), Name), (uint8)Cpu.GetPrefixLeaseState(L), (uint8)ESuperSLMPrefixLeaseState::Cancelled);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: no chunk issued under the teardown"), Name), Seam::ChunkJobsIssuedInTeardown(Cpu) - IssuedBefore, int64(0));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the unfrozen prefix released exactly once"), Name), Seam::UnfrozenBuildReleases(Cpu) - UnfrozenBefore, int64(1));
		bOk &= TestTrue(*FString::Printf(TEXT("%s: no handle released twice"), Name), Seam::MaxReleasesOfOneHandle(Cpu) <= 1);
		// No chunk in the next configuration: its ledger holds no prefix prefill (a DecodeOrPrefill job of no sequence).
		for (int32 Tick = 0; Tick < 8; ++Tick)
		{
			Cpu.Tick(Step);
		}
		int32 LateChunks = 0;
		for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
		{
			LateChunks += Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job.MemberSequences.IsEmpty() && Job.PromptTokens > 0 ? 1 : 0;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: no chunk job after the re-Configure()"), Name), LateChunks, 0);
	}
	return bOk;
}

// Cell 2.10 (§5.10): every disposition of a cache handle against the existing verbs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuHandleDispositionsTest, "SuperSLM.PrefixCache.Cpu.HandleDispositions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuHandleDispositionsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	int64 Initial = -1;
	TArray<int32> T1, T2, Big;
	if (!InitialFree(*this, Cpu, Initial) || !MakeTokens(*this, Cpu, 200, 1, T1) || !MakeTokens(*this, Cpu, 200, 2, T2))
	{
		return false;
	}
	bool bOk = true;
	FString Error;
	FSuperSLMPrefix A, B2, Unleased;

	// ReleasePrefix on a leased and on an unleased cache handle: refused, nothing released.
	if (!RequestReady(*this, Cpu, Spec(T1), A, TEXT("lease A")) || !RequestReady(*this, Cpu, Spec(T1), B2, TEXT("lease B, same entry")))
	{
		return false;
	}
	const uint64 Engine = Seam::EngineHandleOf(Cpu, A);
	bOk &= TestFalse(TEXT("ReleasePrefix(leased cache handle) is refused"), Cpu.ReleasePrefix(A, Error));
	bOk &= TestTrue(TEXT("its error names ReturnPrefix"), Error.Contains(TEXT("owned by the prefix cache; call ReturnPrefix")));
	if (!RequestReady(*this, Cpu, Spec(T2), Unleased, TEXT("an entry to unlease")))
	{
		return false;
	}
	const uint64 UnleasedEngine = Seam::EngineHandleOf(Cpu, Unleased);
	Cpu.ReturnPrefix(Unleased);
	bOk &= TestFalse(TEXT("ReleasePrefix(unleased cache handle) is refused"), Cpu.ReleasePrefix(Unleased, Error));
	DrainQuiet(Cpu);
	bOk &= TestEqual(TEXT("nothing released by either refusal"), Seam::ReleasesOf(Cpu, Engine) + Seam::ReleasesOf(Cpu, UnleasedEngine), 0);

	// Lease A returned while lease B on the same entry is live: A's adopt refused, B's adopts.
	bOk &= TestTrue(TEXT("ReturnPrefix(A)"), Cpu.ReturnPrefix(A));
	bOk &= TestFalse(TEXT("ReturnPrefix(A) twice is false"), Cpu.ReturnPrefix(A));
	FSuperSLMSequence S1, S2;
	FSuperSLMLifecycleOpHandle Op;
	if (!Vend(*this, Cpu, S1) || !Vend(*this, Cpu, S2))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("AdoptPrefix(returned lease A) -> PrefixLeaseEnded"), (uint8)Cpu.AdoptPrefix(S1, A, Op, Error), (uint8)ESuperSLMRestoreResult::PrefixLeaseEnded);
	bOk &= TestFalse(TEXT("nothing queued for the refused adopt"), Op.IsValid());
	bOk &= AdoptAndWait(*this, Cpu, S2, B2);

	// AdoptPrefix on a lease whose entry was since evicted: fill past the budget with B returned.
	bOk &= TestTrue(TEXT("ReturnPrefix(B)"), Cpu.ReturnPrefix(B2));
	{
		TArray<FSuperSLMPrefixSpec> Fill;
		FillUnleased(*this, Cpu, 1, int32(LedgerOf(Cpu).CapacityOf(ERegion::C) * Rig.Geo.B - Rig.Geo.B), 50, Fill);
	}
	bOk &= TestEqual(TEXT("AdoptPrefix(lease on an evicted entry) -> PrefixLeaseEnded"), (uint8)Cpu.AdoptPrefix(S1, B2, Op, Error), (uint8)ESuperSLMRestoreResult::PrefixLeaseEnded);

	// ReturnPrefix on a CreatePrefix handle: false, nothing changes.
	FSuperSLMPrefix Made;
	if (!BuildPrefix(*this, Cpu, T2, Made))
	{
		return false;
	}
	bOk &= TestFalse(TEXT("ReturnPrefix(CreatePrefix handle) is false"), Cpu.ReturnPrefix(Made));
	bOk &= TestEqual(TEXT("the CreatePrefix handle is still Ready"), (uint8)Cpu.GetPrefixPhase(Made), (uint8)ESuperSLMPrefixPhase::Ready);
	bOk &= ReleasePrefixAndDrain(*this, Cpu, Made);
	Cpu.ReturnSequence(S1);
	Cpu.ReturnSequence(S2);

	// Each verb with a handle from before a re-Configure().
	FSuperSLMPrefix Old;
	if (!RequestReady(*this, Cpu, Spec(T1), Old, TEXT("a pre-Configure lease")))
	{
		return false;
	}
	bOk &= TeardownClean(*this, Rig, Initial, TEXT("2.10 teardown"));
	FSuperSLMSequence S3;
	if (!Vend(*this, Cpu, S3))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("AdoptPrefix(stale handle) is refused as an unknown handle"), (uint8)Cpu.AdoptPrefix(S3, Old, Op, Error), (uint8)ESuperSLMRestoreResult::Malformed);
	bOk &= TestTrue(TEXT("its error is today's"), Error.Contains(TEXT("not a live prefix handle")));
	bOk &= TestFalse(TEXT("ReleasePrefix(stale handle)"), Cpu.ReleasePrefix(Old, Error));
	bOk &= TestFalse(TEXT("ReturnPrefix(stale handle)"), Cpu.ReturnPrefix(Old));
	Cpu.ReturnSequence(S3);
	DrainQuiet(Cpu);
	bOk &= TeardownClean(*this, Rig, Initial, TEXT("2.10 end"));
	return bOk;
}

// Cell 2.11: an unknown SchemaName resolves UnknownSchema by name at a tick (never at the call); no
// cache entry and no disk file under that key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuUnknownSchemaTest, "SuperSLM.PrefixCache.Cpu.UnknownSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuUnknownSchemaTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("unknown-schema"));
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig)
		|| !ConfigureCacheBudget(*this, Rig, WithCache(MakeConfig(2, 512, 0, 1), BytesForPages(Rig.Geo, 64), Dir.Path, int64(1) << 30)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix L;
	if (!MakeTokens(*this, Cpu, 100, 1, Tokens))
	{
		return false;
	}
	const FSuperSLMPrefixSpec Unknown = Spec(Tokens, FName(TEXT("no_such_schema")));
	if (!Request(*this, Cpu, Unknown, L, TEXT("unknown schema")))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("Pending at the call (resolving a name is a Layer-1 call, on a lane)"), (uint8)Cpu.GetPrefixLeaseState(L), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= ExpectLease(*this, Cpu, L, ESuperSLMPrefixLeaseState::UnknownSchema, TEXT("unknown schema"));
	DrainQuiet(Cpu);
	bOk &= TestEqual(TEXT("no cache entry"), Stats(Cpu).ResidentEntries, 0);
	bOk &= TestEqual(TEXT("no file in the directory"), Dir.Files().Num(), 0);
	return bOk;
}

// Cell 2.12 (key mutation): for each key component that can vary within one subsystem, spec A then
// spec B differing only there. B's source is not Memory, B's adopter tokens equal B's prefill twin,
// and the spec-mismatch counter reads 0.
//   tokens: one token changed;
//   schema name: unbound vs A-EX's compiled schema (L2-S2's DemoSchemaName());
//   length prefix: §5.2's field order is the schema name, then the tokens. With the name's length
//     prefix dropped, (name "ab", tokens T) and (name "a", tokens T') collide when T' begins with
//     the bytes of "b" followed by T's count; a token id is 4 bytes and a name is UTF-8, so the
//     pair exists only with a schema name the model compiles. A-EX compiles one name, so no
//     colliding pair can be built at U2 and the mutant is recorded as equivalent here (the plan's
//     allowance). The leg kept is the token count's prefix: T and T + [t] stay distinct.
// Mutant: a component dropped from the key -> B is served from A's entry -> the spec compare
// catches it -> the counter reads 1 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuKeyComponentsTest, "SuperSLM.PrefixCache.Cpu.KeyComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuKeyComponentsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Base, Suffix, Twin;
	if (!MakeTokens(*this, Cpu, 300, 1, Base) || !MakeTokens(*this, Cpu, SuffixTokens, 2, Suffix))
	{
		return false;
	}
	struct FPair { const TCHAR* Name; FSuperSLMPrefixSpec A; FSuperSLMPrefixSpec B; bool bTwin; };
	TArray<int32> Longer = Base;
	Longer.Add(Base[0]);
	const FPair Pairs[] = {
		{ TEXT("tokens"), Spec(Base), Spec(WithTokenChanged(Base, 150)), true },
		{ TEXT("schema name"), Spec(Base), Spec(Base, FName(SuperSLML2S2Fixtures::DemoSchemaName())), false },
		{ TEXT("length prefix"), Spec(Base), Spec(Longer), true },
	};
	bool bOk = true;
	for (const FPair& P : Pairs)
	{
		FSuperSLMPrefix LA, LB;
		if (!RequestReady(*this, Cpu, P.A, LA, P.Name) || !RequestReady(*this, Cpu, P.B, LB, P.Name))
		{
			return false;
		}
		bOk &= TestNotEqual(*FString::Printf(TEXT("%s: B's source is not Memory"), P.Name), (uint8)Cpu.GetPrefixSource(LB), (uint8)ESuperSLMPrefixSource::Memory);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the spec-mismatch counter reads 0"), P.Name), Stats(Cpu).SpecMismatches, int64(0));
		if (P.bTwin)
		{
			bOk &= TwinTokens(*this, Cpu, P.B.Tokens, Suffix, Twin) && EqualsTwin(*this, Cpu, LB, Suffix, Twin, P.Name);
		}
		Cpu.ReturnPrefix(LA);
		Cpu.ReturnPrefix(LB);
	}
	return bOk;
}

// Cell 3.5: 16 requests for one key in one tick -> one build (counted), 16 Ready handles; also when
// the shared load fails (a corrupt disk file, then recompute: in the disk suite), when it ends
// PoolExhausted, and when the first requester returns its lease before Ready. All 16 resolve by
// name within a bounded number of ticks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuCoalescingTest, "SuperSLM.PrefixCache.Cpu.Coalescing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuCoalescingTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	auto Burst = [&](int32 Salt, bool bReturnFirst, ESuperSLMPrefixLeaseState Want, const TCHAR* What)
	{
		TArray<int32> Tokens;
		if (!MakeTokens(*this, Cpu, 400, Salt, Tokens))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
		TArray<FSuperSLMPrefix> Leases;
		for (int32 I = 0; I < 16; ++I)
		{
			FSuperSLMPrefix L;
			if (!Request(*this, Cpu, Spec(Tokens), L, What))
			{
				return false;
			}
			Leases.Add(L);
		}
		if (bReturnFirst)
		{
			TestTrue(TEXT("the first requester returns its lease before Ready"), Cpu.ReturnPrefix(Leases[0]));
		}
		bool bLeg = true;
		for (int32 I = bReturnFirst ? 1 : 0; I < 16; ++I)
		{
			bLeg &= ExpectLease(*this, Cpu, Leases[I], Want, What, BuildSeconds);
		}
		// Counted once every lease has resolved (box 2026-10-21: read straight after the requests it was 0,
		// since the build is issued only once the key's hash completes on a pool thread).
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: one build"), What), Stats(Cpu).BuildsIssued - S0.BuildsIssued, int64(1));
		if (bReturnFirst)
		{
			bLeg &= TestEqual(*FString::Printf(TEXT("%s: the returned lease reads Ended"), What), (uint8)Cpu.GetPrefixLeaseState(Leases[0]), (uint8)ESuperSLMPrefixLeaseState::Ended);
		}
		TSet<int32> Ids;
		for (const FSuperSLMPrefix& L : Leases)
		{
			Ids.Add(L.Id);
			Cpu.ReturnPrefix(L);
		}
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: 16 distinct handles"), What), Ids.Num(), 16);
		DrainQuiet(Cpu);
		return bLeg;
	};
	// PoolExhausted: the shared build's begin is told the pool is exhausted (paging's one-shot override, which
	// reaches cache builds since box 2026-10-21), and evict-and-retry finds nothing evictable, so it ends
	// PoolExhausted for all. It runs first, on an empty cache: after the other bursts their entries are
	// evictable, so the retry would evict one and build (§5.4), and the leg could not end PoolExhausted.
	FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(Cpu, ESuperSLMPagedRequestKind::PrefixBuild, SSLM_KV_POOL_EXHAUSTED);
	bOk &= Burst(3, false, ESuperSLMPrefixLeaseState::PoolExhausted, TEXT("pool exhausted"));
	bOk &= Burst(1, false, ESuperSLMPrefixLeaseState::Ready, TEXT("plain"));
	bOk &= Burst(2, true, ESuperSLMPrefixLeaseState::Ready, TEXT("first returns early"));
	return bOk;
}

// Cell 3.6: an eviction racing a queued adopt of the LRU entry: the adopt pins the entry, the adopt
// succeeds, the eviction takes the next entry. LRU 3, next 3, the rest C - 7 (box 2026-10-21: C - 6 peaked
// at R = C - 5 with both others leased, so the rest was refused), one page free; the new entry (R = 3) takes
// the free page and evicts the next entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuEvictionVsAdoptTest, "SuperSLM.PrefixCache.Cpu.EvictionVsQueuedAdopt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuEvictionVsAdoptTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int64 C = LedgerOf(Cpu).CapacityOf(ERegion::C);
	TArray<int32> Lru, Next, Rest, New;
	FSuperSLMPrefix LLru, LNext, LRest, LNew;
	if (!PagesTokens(*this, Rig, 3, 1, Lru) || !PagesTokens(*this, Rig, 3, 2, Next) || !PagesTokens(*this, Rig, C - 7, 3, Rest) || !MakeTokens(*this, Cpu, int32(2 * Rig.Geo.B), 4, New))
	{
		return false;
	}
	if (!RequestReady(*this, Cpu, Spec(Lru), LLru, TEXT("LRU")) || !RequestReady(*this, Cpu, Spec(Next), LNext, TEXT("next")) || !RequestReady(*this, Cpu, Spec(Rest), LRest, TEXT("rest")))
	{
		return false;
	}
	Cpu.ReturnPrefix(LNext);
	FSuperSLMSequence Seq;
	FSuperSLMLifecycleOpHandle Op;
	FString Error;
	const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::Adopt);
	if (!Vend(*this, Cpu, Seq) || !TestEqual(TEXT("AdoptPrefix(LRU) queues"), (uint8)Cpu.AdoptPrefix(Seq, LLru, Op, Error), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	Cpu.ReturnPrefix(LLru); // the queued adopt alone pins it now
	bool bOk = WaitHoldReached(Cpu, Hold);
	if (!Request(*this, Cpu, Spec(New), LNew, TEXT("the request that evicts")))
	{
		return false;
	}
	// The eviction must be decided while the adopt is still queued. Box 2026-10-21 (second run): the hold
	// was released straight after the request, before its key hash (a pool-thread job) had been applied, so
	// the adopt ran and unpinned the LRU entry first, and the eviction took the LRU entry instead (the next
	// entry read as a Memory hit). Settle the hash with the adopt's job still held (no job drain).
	bOk &= TestTrue(TEXT("the request's key hash is applied before the adopt runs"), SettleCacheIo(Cpu, 10.0, false));
	FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
	bOk &= TestEqual(TEXT("the queued adopt succeeds"), (uint8)WaitOp(Cpu, Op), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= ExpectLease(*this, Cpu, LNew, ESuperSLMPrefixLeaseState::Ready, TEXT("the new entry"));
	FSuperSLMPrefix Probe;
	bOk &= RequestReady(*this, Cpu, Spec(Next), Probe, TEXT("the next entry again"));
	bOk &= TestNotEqual(TEXT("the eviction took the next entry (it is no Memory hit)"), (uint8)Cpu.GetPrefixSource(Probe), (uint8)ESuperSLMPrefixSource::Memory);
	ReturnAndDrain(Cpu, Seq);
	return bOk;
}

// Cell 3.9 (§5.5 ownership): the accessor through which the cache's private state is reachable
// asserts the game thread; its call count is > 0 after a workload; its must-reject (the accessor
// reached from a pool thread) trips.
// Mutant: eviction's map update moved to the pool thread -> the assert trips in the workload -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuGameThreadOwnershipTest, "SuperSLM.PrefixCache.Cpu.GameThreadOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuGameThreadOwnershipTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<FSuperSLMPrefixSpec> Specs;
	const int64 Before = Seam::GameThreadStateAccesses(Cpu);
	if (!FillUnleased(*this, Cpu, 4, int32(Rig.Geo.B * 20), 1, Specs))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the accessor was called (> 0) by a workload with evictions"), Seam::GameThreadStateAccesses(Cpu) > Before && Stats(Cpu).Evictions > 0);
	bOk &= TestTrue(TEXT("must-reject: the accessor reached from a pool thread trips"), Seam::GameThreadAccessorTrips(Cpu));

	// Rev 1.18 (suite audit G1): the drop's decision and its mark go through the same accessor, and its count
	// includes them. A drop of a resident unleased entry (decided: Dropped) and a drop of a leased one (marked:
	// DropDeferred) each raise the count at the pass that applies it. Mutant: a drop's map update moved off the
	// accessor -> it does not compile, or the count does not rise -> red.
	{
		FSuperSLMPrefix Leased;
		if (!RequestReady(*this, Cpu, Specs[0], Leased, TEXT("3.9 drop leg")))
		{
			return false;
		}
		struct FDropCase { const FSuperSLMPrefixSpec* Spec; ESuperSLMPrefixOpResult Want; const TCHAR* What; };
		const FDropCase Cases[] = {
			{&Specs.Last(), ESuperSLMPrefixOpResult::Dropped, TEXT("a drop's decision (Dropped)")},
			{&Specs[0], ESuperSLMPrefixOpResult::DropDeferred, TEXT("a drop's mark (DropDeferred)")},
		};
		for (const FDropCase& Case : Cases)
		{
			DrainQuiet(Cpu);
			FSuperSLMPrefixOp Op;
			FString Error;
			if (!TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts (%s)"), Case.What, *Error), Cpu.DropPrefix(*Case.Spec, ESuperSLMPrefixDropScope::Memory, Op, Error)))
			{
				Cpu.ReturnPrefix(Leased);
				return false;
			}
			const int64 AtIssue = Seam::GameThreadStateAccesses(Cpu);
			FSuperSLMPrefixOpResult R;
			Drain(Cpu, [&Cpu, &Op, &R]() { Cpu.GetPrefixOpResult(Op, R); return R.Result != ESuperSLMPrefixOpResult::Pending; }, LeaseSeconds);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: resolves as planned"), Case.What), (uint8)R.Result, (uint8)Case.Want);
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the accessor's count includes it"), Case.What), Seam::GameThreadStateAccesses(Cpu) > AtIssue);
		}
		Cpu.ReturnPrefix(Leased);
		DrainQuiet(Cpu);
	}
	return bOk;
}

// Cell 5.5 (and 7.10): every entry leased and one more request: BudgetExhausted, no wait, nothing
// evicted, nothing drawn (fill probe, both readings behind the barrier). PoolExhausted is reached by
// filling the pool from outside the cache (the paging probe's holders), evict-and-retry finding
// nothing evictable.
// Mutant: admission over budget when every entry is pinned -> drawn changes -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuAllPinnedTest, "SuperSLM.PrefixCache.Cpu.AllPinned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuAllPinnedTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64, /*bLegacy*/ true, 2, 1))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> A, Bt, W;
	FSuperSLMPrefix LA, LB, LW;
	// A 31 and B 32 pages (box 2026-10-21: A 32 left 32 free, and B's build peaks at R = 33, so B itself was
	// refused). One page free; W (R = 3) cannot fit with both leased.
	if (!PagesTokens(*this, Rig, 31, 1, A) || !PagesTokens(*this, Rig, 32, 2, Bt) || !MakeTokens(*this, Cpu, 100, 3, W)
		|| !RequestReady(*this, Cpu, Spec(A), LA, TEXT("A")) || !RequestReady(*this, Cpu, Spec(Bt), LB, TEXT("B")))
	{
		return false;
	}
	int64 FreeBefore = -1;
	if (!DrainQuiet(Cpu) || !ProbeFree(*this, Cpu, ProbeCeiling(*this, Cpu), FreeBefore)) // legacy: no ledger bound
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("both readings see >= 3 free pages (the spare block)"), FreeBefore >= 3);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	if (!Request(*this, Cpu, Spec(W), LW, TEXT("one more")))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Cpu, LW, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("one more"), 10.0);
	bOk &= TestEqual(TEXT("no wait"), Stats(Cpu).Waiters, 0);
	bOk &= TestEqual(TEXT("nothing evicted"), Stats(Cpu).Evictions, S0.Evictions);
	bOk &= FreeUnchanged(*this, Cpu, FreeBefore, TEXT("after BudgetExhausted"));

	// PoolExhausted: the pool short with nothing evictable. Filling the pool from outside the cache
	// is not reachable through the plugin's verbs in legacy mode (1.0.0 prefixes stop at
	// PrefixBlockCount, inside their own blocks, and the probe's holders are released when its
	// callback returns), so the leg makes the build's draw see SSLM_KV_POOL_EXHAUSTED through
	// paging's status seam (a reading for the planner).
	TArray<int32> Small;
	FSuperSLMPrefix LP;
	if (!MakeTokens(*this, Cpu, 50, 4, Small))
	{
		return false;
	}
	// B is returned, so the request's charge evicts B and is admitted at B's release; its draw then
	// meets the exhaustion, and evict-and-retry finds nothing (A is leased).
	Cpu.ReturnPrefix(LB);
	DrainQuiet(Cpu);
	FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(Cpu, ESuperSLMPagedRequestKind::PrefixBuild, SSLM_KV_POOL_EXHAUSTED);
	if (!Request(*this, Cpu, Spec(Small), LP, TEXT("pool short")))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Cpu, LP, ESuperSLMPrefixLeaseState::PoolExhausted, TEXT("pool short, nothing evictable"));
	DrainQuiet(Cpu);
	return bOk;
}

// Cell 7.8: no hashing, file IO or Layer-1 call on the game thread -> the new off-game-thread assert:
// its checked-call count > 0 after a workload, and its must-reject (a hashing call made on the game
// thread) trips.
// Mutant: key hashing moved into RequestPrefix -> the assert trips in the workload -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuOffGameThreadTest, "SuperSLM.PrefixCache.Cpu.OffGameThreadWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuOffGameThreadTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("off-game-thread"));
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig)
		|| !ConfigureCacheBudget(*this, Rig, WithCache(MakeConfig(2, 512, 0, 1), BytesForPages(Rig.Geo, 64), Dir.Path, int64(1) << 30)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<FSuperSLMPrefixSpec> Specs;
	const int64 Before = Seam::OffGameThreadChecks(Cpu);
	if (!FillUnleased(*this, Cpu, 3, 300, 1, Specs))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the off-game-thread assert was checked (> 0) by hashing, file IO and Layer-1 calls"), Seam::OffGameThreadChecks(Cpu) > Before);
	bOk &= TestTrue(TEXT("must-reject: a hashing call on the game thread trips"), Seam::OffGameThreadAssertTrips(Cpu));

	// Rev 1.18.1 (audit G12; suite audit G1): the assert also covers WarmPrefix and DropPrefix. With the assert
	// recording, a warm of a cold key, a warm of a resident one, and drops of both scopes (each hashing its key,
	// the MemoryAndDisk drop deleting a file) trip nothing, and the assert was checked by them. Mutant: the key
	// hashed inside DropPrefix to decide synchronously -> one trip recorded -> red.
	{
		TArray<int32> Cold;
		if (!MakeTokens(*this, Cpu, 300, 9, Cold))
		{
			return false;
		}
		Seam::SetOffGameThreadRecording(Cpu, true);
		const int64 ChecksBefore = Seam::OffGameThreadChecks(Cpu);
		const int64 TripsBefore = Seam::OffGameThreadViolations(Cpu);
		TArray<FSuperSLMPrefixOp> Ops;
		auto Issue = [this, &Cpu, &Ops](bool bOk, const FSuperSLMPrefixOp& Op, const FString& Error, const TCHAR* What)
		{
			TestTrue(*FString::Printf(TEXT("%s: accepted (%s)"), What, *Error), bOk && Op.Id != 0);
			Ops.Add(Op);
		};
		{
			FSuperSLMPrefixOp Op; FString Error;
			const bool bAccepted = Cpu.WarmPrefix(Spec(Cold), Op, Error);
			Issue(bAccepted, Op, Error, TEXT("warm, cold key"));
		}
		{
			FSuperSLMPrefixOp Op; FString Error;
			const bool bAccepted = Cpu.WarmPrefix(Specs[0], Op, Error);
			Issue(bAccepted, Op, Error, TEXT("warm, resident key"));
		}
		{
			FSuperSLMPrefixOp Op; FString Error;
			const bool bAccepted = Cpu.DropPrefix(Specs[1], ESuperSLMPrefixDropScope::Memory, Op, Error);
			Issue(bAccepted, Op, Error, TEXT("drop, Memory"));
		}
		{
			FSuperSLMPrefixOp Op; FString Error;
			const bool bAccepted = Cpu.DropPrefix(Specs[2], ESuperSLMPrefixDropScope::MemoryAndDisk, Op, Error);
			Issue(bAccepted, Op, Error, TEXT("drop, MemoryAndDisk"));
		}
		Drain(Cpu, [&Cpu, &Ops]()
		{
			for (const FSuperSLMPrefixOp& Op : Ops)
			{
				FSuperSLMPrefixOpResult R;
				if (!Cpu.GetPrefixOpResult(Op, R) || R.Result == ESuperSLMPrefixOpResult::Pending)
				{
					return false;
				}
			}
			return true;
		}, LeaseSeconds);
		bOk &= TestTrue(TEXT("warm and drop: the off-game-thread assert was checked by their hashing and file IO"), Seam::OffGameThreadChecks(Cpu) > ChecksBefore);
		bOk &= TestEqual(TEXT("warm and drop: no hashing, file IO or Layer-1 call on the game thread (no trip recorded)"), Seam::OffGameThreadViolations(Cpu) - TripsBefore, int64(0));
		bOk &= TestTrue(TEXT("warm and drop: the recording mode is wired (the violation count is readable)"), TripsBefore >= 0);
		Seam::SetOffGameThreadRecording(Cpu, false);
	}
	return bOk;
}

// Cell 7.13: memory-hit oracle and LRU oracle.
// Mutants: evict the most recent -> red; evict in insertion order -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuMemoryAndLruTest, "SuperSLM.PrefixCache.Cpu.MemoryAndLru",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuMemoryAndLruTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens, Suffix, Twin;
	if (!MakeTokens(*this, Cpu, 500, 1, Tokens) || !MakeTokens(*this, Cpu, SuffixTokens, 2, Suffix) || !TwinTokens(*this, Cpu, Tokens, Suffix, Twin))
	{
		return false;
	}
	// Memory hit.
	FSuperSLMPrefix First, Hit;
	if (!RequestReady(*this, Cpu, Spec(Tokens), First, TEXT("first")))
	{
		return false;
	}
	Cpu.ReturnPrefix(First);
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	bool bOk = RequestReady(*this, Cpu, Spec(Tokens), Hit, TEXT("again"));
	bOk &= ExpectSource(*this, Cpu, Hit, ESuperSLMPrefixSource::Memory, TEXT("again"));
	bOk &= TestEqual(TEXT("no restore issued"), Stats(Cpu).RestoresIssued, S0.RestoresIssued);
	bOk &= TestEqual(TEXT("no prefill issued"), Stats(Cpu).BuildsIssued, S0.BuildsIssued);
	bOk &= EqualsTwin(*this, Cpu, Hit, Suffix, Twin, TEXT("memory hit"));
	Cpu.ReturnPrefix(Hit);

	// LRU: re-Configure, fill C with four 16-page entries (insertion order 0..3), touch in the order
	// 2, 0, 3, 1 (neither insertion nor its reverse), admit one more: the evicted key is 2.
	if (!ConfigureCacheBudget(*this, Rig, Rig.Config))
	{
		return false;
	}
	const int64 Quarter = LedgerOf(Cpu).CapacityOf(ERegion::C) / 4;
	TArray<FSuperSLMPrefixSpec> Specs;
	if (!FillUnleased(*this, Cpu, 4, int32(Quarter * Rig.Geo.B), 10, Specs))
	{
		return false;
	}
	for (int32 I : { 2, 0, 3, 1 })
	{
		FSuperSLMPrefix Touch;
		bOk &= RequestReady(*this, Cpu, Specs[I], Touch, TEXT("touch"));
		Cpu.ReturnPrefix(Touch);
	}
	TArray<int32> More;
	FSuperSLMPrefix LMore;
	if (!MakeTokens(*this, Cpu, int32(Rig.Geo.B), 99, More) || !RequestReady(*this, Cpu, Spec(More), LMore, TEXT("one more")))
	{
		return false;
	}
	Cpu.ReturnPrefix(LMore);
	for (int32 I = 0; I < 4; ++I)
	{
		FSuperSLMPrefix Probe;
		bOk &= RequestReady(*this, Cpu, Specs[I], Probe, TEXT("probe"));
		const bool bMemory = Cpu.GetPrefixSource(Probe) == ESuperSLMPrefixSource::Memory;
		bOk &= TestEqual(*FString::Printf(TEXT("entry %d (touched %s) is %s"), I, I == 2 ? TEXT("least recently") : TEXT("later"), I == 2 ? TEXT("evicted") : TEXT("resident")), bMemory, I != 2);
		Cpu.ReturnPrefix(Probe);
		if (I == 2)
		{
			break; // re-requesting the evicted key evicts another; the rest were read before it
		}
	}
	return bOk;
}

// Cell 8.8: a sequence adopted from a cached prefix saves and restores as today: the restored
// continuation equals the unsaved twin's.
// Box 2026-10-21 (second run): the cell saved a Complete sequence and began a second generation on the
// restored one, which one-generation-at-a-time refuses. It now saves mid-turn (at 20 of 48 tokens) and lets
// the restored sequence resume its generation, the paging restore suite's pattern. The prefix is 450 tokens
// so prefix, suffix and 48 new tokens fit the sequence budget of 512.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuSaveRestoreAdopterTest, "SuperSLM.PrefixCache.Cpu.SaveRestoreAdopter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuSaveRestoreAdopterTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CacheRig(*this, Rig, 64))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens, Suffix, Before, Reference;
	FSuperSLMPrefix Lease;
	constexpr int32 kNew = 48;
	constexpr int32 kSaveAt = 20;
	if (!MakeTokens(*this, Cpu, 450, 1, Tokens) || !MakeTokens(*this, Cpu, SuffixTokens, 2, Suffix) || !RequestReady(*this, Cpu, Spec(Tokens), Lease, TEXT("lease")))
	{
		return false;
	}
	// The unsaved twin: adopt, generate kNew after the suffix in one generation.
	FSuperSLMSequence Twin, Seq, Restored;
	if (!Vend(*this, Cpu, Twin) || !AdoptAndWait(*this, Cpu, Twin, Lease) || !GenerateComplete(*this, Cpu, Twin, Suffix, kNew, Reference))
	{
		return false;
	}
	ReturnAndDrain(Cpu, Twin);
	TArray<uint8> Blob;
	if (!Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, Lease) || !CacheSaveMidTurn(*this, Cpu, Seq, GenerationRequest(Suffix, kNew), kSaveAt, Blob, Before))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the restore succeeds"), (uint8)RestoreAndWait(*this, Cpu, Blob, Rig.Model, Restored, Lease), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= CacheContinueRestored(*this, Cpu, Restored, Before);
	bOk &= TestEqual(TEXT("save/restore of a cache adopter continues as the unsaved twin"), Before, Reference);
	ReturnAndDrain(Cpu, Restored);
	return bOk;
}

// Cell 8.10 (and 7.10): 1.0.0's behaviour beside a working cache, legacy mode, three phases.
// F_min(t) = N - R_seq - P10(t) - floor(budget / page), P10(t) = cap_pages x live(t) (every 1.0.0
// prefix has L = cap, so frozen and unfrozen hold the same), live(t) read behind the barrier.
// None of 1.0.0's calls is refused under its cap.
// Rev 1.18 / 1.18.1 (PC-15 Q7, plan review F7, audit G15; suite audit G2): a CPU build is one lane job, so
// phase (c)'s drawn-and-unfrozen window is read through the build hold (§8). K = 8 builds on K + 1 lanes
// (MaxConcurrentCalls = BlockCount = K + 1, so lanes = Clamp(MaxConcurrentCalls, 1, BlockCount) = K + 1):
// the K builds stop at the hold on K lanes; phase (c)'s CreatePrefix calls and the warm-slot reset run on
// the free lane and complete; then one required reading has K builds drawn and zero frozen, witnessed by
// each build's state and the ledger's in-flight count, with P10 > 0. If the window is absent the cell
// fails. The budget is K x R(2B) = 3K pages, so the ledger sits at the budget, to the page, in the window.
// Mutants: a build that draws without a charge -> F_min refused -> red; a build charged ceil(L/B) = 2
// rather than R(L) = 3 -> K pages over at the window's reading -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuBeside100Test, "SuperSLM.PrefixCache.Cpu.Beside100",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPrefixCacheCpuBeside100Test::RunTest(const FString& Parameters)
{
	constexpr int32 K = 8;
	constexpr int32 Blocks = K + 1;
	constexpr int32 PrefixBlocks = 2;
	auto Config = [](int32 BlockCount, int32 Prefixes)
	{
		FSuperSLMRuntimeConfig C = MakeConfig(BlockCount, 0, Prefixes, 1);
		C.MaxConcurrentCalls = BlockCount; // K + 1 lanes (rev 1.18.1)
		return C;
	};
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, Config(Blocks, PrefixBlocks)) || !ReadGeometry(*this, Rig) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int64 CapPages = Rig.Geo.Block();
	const int64 BudgetPagesN = K * 3; // K in-flight cache builds of L = 2B (R = 3)
	if (!ConfigureCacheLegacy(*this, Rig, WithCache(Config(Blocks, PrefixBlocks), BytesForPages(Rig.Geo, BudgetPagesN))))
	{
		return false;
	}
	int64 N = -1;
	if (!InitialFree(*this, Cpu, N))
	{
		return false;
	}
	auto Reading = [&](const TCHAR* Instant)
	{
		const int64 P10 = CapPages * Seam::LegacyPrefixLive(Cpu);
		return CacheWithinBudget(*this, Cpu, N, 0, P10, BudgetPagesN, Instant);
	};
	bool bOk = true;
	TArray<int32> CapTokens;
	if (!MakeTokens(*this, Cpu, int32(Rig.Geo.Cap), 1, CapTokens))
	{
		return false;
	}

	// (a) Static: the cache at its budget less one entry: 3K/2 - 1 entries of L = 2B, each leased, settled to 2 pages,
	// 3K - 2 pages. One more cannot be built under 3K: a build is charged R(2B) = 3 pages until it settles (the
	// first run, 2026-10-24, filled 3K/2 and its last build resolved BudgetExhausted). The reading is one-sided,
	// and (b)'s one more request is still BudgetExhausted.
	TArray<FSuperSLMPrefixSpec> Specs;
	TArray<FSuperSLMPrefix> Leases;
	for (int32 I = 0; I < K * 3 / 2 - 1; ++I)
	{
		TArray<int32> T;
		FSuperSLMPrefix L;
		if (!MakeTokens(*this, Cpu, int32(2 * Rig.Geo.B), 10 + I, T) || !RequestReady(*this, Cpu, Spec(T), L, TEXT("(a) fill")))
		{
			return false;
		}
		Leases.Add(L);
		Specs.Add(Spec(T));
	}
	bOk &= Reading(TEXT("(a) cache at budget"));
	TArray<FSuperSLMPrefix> Old;
	TArray<FSuperSLMSequence> Warm;
	for (int32 I = 0; I < PrefixBlocks; ++I)
	{
		FSuperSLMPrefix P;
		if (!BuildPrefix(*this, Cpu, WithTokenChanged(CapTokens, I), P))
		{
			return false;
		}
		Old.Add(P);
		bOk &= Reading(TEXT("(a) a 1.0.0 prefix built"));
	}
	{
		FSuperSLMPrefix Over;
		FString Error;
		bOk &= TestFalse(TEXT("(a) CreatePrefix past PrefixBlockCount is refused as today"), Cpu.CreatePrefix(WithTokenChanged(CapTokens, 99), Over, Error));
		bOk &= TestTrue(TEXT("(a) by name"), Error.Contains(TEXT("PrefixBlockCount")));
	}
	for (int32 I = 0; I < Blocks; ++I)
	{
		FSuperSLMSequence S;
		if (!Vend(*this, Cpu, S) || !AdoptAndWait(*this, Cpu, S, Old[I % Old.Num()]))
		{
			return false;
		}
		Warm.Add(S);
	}
	bOk &= Reading(TEXT("(a) warm sequences adopted"));

	// (b) Every entry leased and one more: BudgetExhausted; then (a)'s calls all admitted.
	{
		TArray<int32> T;
		FSuperSLMPrefix L;
		if (!MakeTokens(*this, Cpu, int32(2 * Rig.Geo.B), 50, T) || !Request(*this, Cpu, Spec(T), L, TEXT("(b) one more")))
		{
			return false;
		}
		bOk &= ExpectLease(*this, Cpu, L, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("(b) one more"), 10.0);
		for (const FSuperSLMSequence& S : Warm)
		{
			bOk &= ResetAndWait(*this, Cpu, S);
		}
		bOk &= Reading(TEXT("(b)"));
	}

	// (c) K requests for distinct uncached keys in one tick (no coalescing), each stopped at the build hold after
	// its begin; while they are held, CreatePrefix up to PrefixBlockCount and reset a released warm slot on the
	// free lane; then the required reading, with K builds drawn and none frozen.
	for (const FSuperSLMPrefix& L : Leases)
	{
		Cpu.ReturnPrefix(L);
	}
	for (const FSuperSLMPrefix& P : Old)
	{
		ReleasePrefixAndDrain(*this, Cpu, P);
	}
	bOk &= Reading(TEXT("(c) before the burst"));
	Seam::SetBuildHold(Cpu, true);
	TArray<FSuperSLMPrefix> Burst;
	for (int32 I = 0; I < K; ++I)
	{
		TArray<int32> T;
		FSuperSLMPrefix L;
		if (!MakeTokens(*this, Cpu, int32(2 * Rig.Geo.B), 70 + I, T) || !Request(*this, Cpu, Spec(T), L, TEXT("(c) burst")))
		{
			Seam::SetBuildHold(Cpu, false);
			return false;
		}
		Burst.Add(L);
	}
	Drain(Cpu, [&Cpu]() { return Seam::HeldBuildCount(Cpu) == K; }, LeaseSeconds * K);
	bOk &= TestEqual(TEXT("(c) the K builds reach the build hold on K lanes"), Seam::HeldBuildCount(Cpu), K);
	TArray<FSuperSLMPrefix> Old2;
	for (int32 I = 0; I < PrefixBlocks; ++I)
	{
		FSuperSLMPrefix P;
		FString Error;
		bOk &= TestTrue(*FString::Printf(TEXT("(c) CreatePrefix %d admitted under its cap (%s)"), I, *Error), Cpu.CreatePrefix(WithTokenChanged(CapTokens, 10 + I), P, Error));
		Old2.Add(P);
	}
	for (const FSuperSLMPrefix& P : Old2)
	{
		bOk &= WaitPrefixReady(*this, Cpu, P); // on the free lane, while the K builds are held
	}
	Cpu.ReturnSequence(Warm[0]);
	FSuperSLMSequence Again;
	bOk &= Vend(*this, Cpu, Again) && ResetAndWait(*this, Cpu, Again);
	{
		// The required reading: K builds drawn and zero frozen, P10 > 0.
		int32 Begun = 0, Frozen = 0;
		for (const FSuperSLMPrefix& L : Burst)
		{
			const ESuperSLMPrefixBuildState B = Seam::BuildStateOf(Cpu, L);
			Begun += B == ESuperSLMPrefixBuildState::Begun ? 1 : 0;
			Frozen += B == ESuperSLMPrefixBuildState::Frozen ? 1 : 0;
		}
		const bool bWindow = TestEqual(TEXT("(c) window: every build has begun (drawn its reserve)"), Begun, K)
			& TestEqual(TEXT("(c) window: no build has frozen"), Frozen, 0)
			& TestEqual(TEXT("(c) window: the ledger's in-flight count reads K builds"), Seam::InFlightBuildCount(Cpu), K)
			& TestTrue(TEXT("(c) window: 1.0.0 prefixes are live (P10 > 0)"), Seam::LegacyPrefixLive(Cpu) > 0)
			& TestEqual(TEXT("(c) window: still K builds at the hold after the free lane's calls"), Seam::HeldBuildCount(Cpu), K);
		bOk &= bWindow;
		bOk &= TestTrue(TEXT("(c) the K-drawn, zero-frozen window was present (a later reading cannot substitute)"), bWindow);
		bOk &= Reading(TEXT("(c) K builds drawn, none frozen"));
	}
	Seam::SetBuildHold(Cpu, false);
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Cpu.Tick(Step);
		if (Tick % 10 == 9)
		{
			bOk &= Reading(*FString::Printf(TEXT("(c) tick %d"), Tick + 1));
		}
	}
	for (const FSuperSLMPrefix& L : Burst)
	{
		const ESuperSLMPrefixLeaseState S = WaitLease(Cpu, L);
		bOk &= TestTrue(TEXT("(c) each request resolves Ready or BudgetExhausted by name"), S == ESuperSLMPrefixLeaseState::Ready || S == ESuperSLMPrefixLeaseState::BudgetExhausted);
	}
	bOk &= Reading(TEXT("(c) the end"));
	return bOk;
}

// §5.11 (b) enum order: PrefixLeaseEnded is appended after paging's KvPagesExhausted, KvModeMismatch
// and ContextCapMismatch, still last, with its Blueprint mirror.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuLeaseEndedOrdinalTest, "SuperSLM.PrefixCache.Cpu.PrefixLeaseEndedOrdinal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuLeaseEndedOrdinalTest::RunTest(const FString& Parameters)
{
	bool bOk = TestEqual(TEXT("PrefixLeaseEnded follows ContextCapMismatch"), int32(ESuperSLMRestoreResult::PrefixLeaseEnded), int32(ESuperSLMRestoreResult::ContextCapMismatch) + 1);
	bOk &= TestEqual(TEXT("ContextCapMismatch follows KvModeMismatch"), int32(ESuperSLMRestoreResult::ContextCapMismatch), int32(ESuperSLMRestoreResult::KvModeMismatch) + 1);
	bOk &= TestEqual(TEXT("KvModeMismatch follows KvPagesExhausted"), int32(ESuperSLMRestoreResult::KvModeMismatch), int32(ESuperSLMRestoreResult::KvPagesExhausted) + 1);
	const UEnum* Mirror = StaticEnum<ESuperSLMRestoreResultBP>();
	bOk &= TestNotNull(TEXT("the Blueprint mirror exists"), Mirror);
	if (Mirror)
	{
		bOk &= TestEqual(TEXT("the mirror carries PrefixLeaseEnded at the same value"),
			Mirror->GetValueByNameString(TEXT("PrefixLeaseEnded")), int64(ESuperSLMRestoreResult::PrefixLeaseEnded));
		bOk &= TestEqual(TEXT("PrefixLeaseEnded is last in the mirror"), Mirror->GetMaxEnumValue() - 1, int64(ESuperSLMRestoreResult::PrefixLeaseEnded));
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
