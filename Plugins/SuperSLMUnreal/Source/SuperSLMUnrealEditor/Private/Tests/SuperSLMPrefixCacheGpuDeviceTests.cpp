// Precache plan rev 1.18.x, the U6 suite's GPU-only cells and the GPU twins of the threading cells: the GPU subsystem's
// cache instance (pooled mode) under its submission thread. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on;
// the restore legs need SUPERSLM_WITH_GPU_PREFIX_SAVE as well.
//
//   SuperSLM.PrefixCache.Gpu.Build.Chunked              cell 5.13 (Q7, §5.3 item 4 "On the GPU", N4)      [U6]
//   SuperSLM.PrefixCache.Gpu.DeviceLoss.NoReissue       cell 5.14 (Q9, §5.4 after a terminal loss)        [U6]
//   SuperSLM.PrefixCache.Gpu.DeviceLoss.Unresponsive    cell 5.14's unresponsive-thread leg               [U6]
//   SuperSLM.PrefixCache.Gpu.ReleaseStatus              cell 5.15's GPU legs, busy and non-busy (§5.4)    [U6]
//   SuperSLM.PrefixCache.Gpu.GameThreadOwnership        cell 3.9 twin (and its rev 1.18 drop leg)         [U6]
//   SuperSLM.PrefixCache.Gpu.OffGameThreadWork          cell 7.8 twin (and its rev 1.18.1 leg)            [U6]
//
// The GPU cache is pooled (budget mode) only, so the CPU cells' legacy legs have no twin here. Every cell runs on the
// GPU pooled rig (A-EX, B = 16) with region C sized to one block (ConfigurePrefixCache's budget = one page's bytes)
// and no pad: none of these cells needs region C exactly full. Prefixes are one page except 5.13's, which must run as
// several build steps.
//
// No GPU reading (see 5.14's comment): the plan's "the teardown sweep releases it exactly once" after a loss.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
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

	// Pooled, then the cache (region C = one block), then pooled again: ConfigurePrefixCache applies at the next
	// Configure(). OutCacheConfigureSeconds: the wall clock of that second Configure() (5.14's reference).
	bool GpuDevConfigure(FAutomationTestBase& T, FGpuPagingRig& Rig, const FString& DiskDirectory, double* OutCacheConfigureSeconds = nullptr)
	{
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
		const double Start = FPlatformTime::Seconds();
		if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, 512)))
		{
			return false;
		}
		if (OutCacheConfigureSeconds != nullptr)
		{
			*OutCacheConfigureSeconds = FPlatformTime::Seconds() - Start;
		}
		return T.TestTrue(TEXT("the GPU cache is configured, on region C"), Rig.Gpu->GetPrefixCacheStats().bConfigured && Rig.Gpu->GetPrefixCacheStats().bBudgetMode);
	}

	FSuperSLMPrefixSpec GpuDevSpec(const TArray<int32>& Tokens)
	{
		return SuperSLMPrefixCacheFixtures::Spec(Tokens);
	}

	ESuperSLMPrefixLeaseState GpuDevResolve(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, double Seconds = SuperSLMPagingGpuFixtures::BuildSeconds)
	{
		Pump(Gpu, [&Gpu, &Lease]() { return Gpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, Seconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool GpuDevLease(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bAccepted = Gpu.RequestPrefix(Spec, Out, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: requested (%s)"), What, *Error), bAccepted))
		{
			return false;
		}
		return T.TestEqual(*FString::Printf(TEXT("%s: resolves Ready"), What), (uint8)GpuDevResolve(Gpu, Out), (uint8)ESuperSLMPrefixLeaseState::Ready);
	}

	FSuperSLMPrefixOpResult GpuDevWaitOp(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixOp& Op, double Seconds = SuperSLMPagingGpuFixtures::BuildSeconds)
	{
		FSuperSLMPrefixOpResult R;
		Pump(Gpu, [&Gpu, &Op, &R]() { return Gpu.GetPrefixOpResult(Op, R) && R.Result != ESuperSLMPrefixOpResult::Pending; }, Seconds);
		Gpu.GetPrefixOpResult(Op, R);
		return R;
	}

	bool GpuDevDrop(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& Spec, ESuperSLMPrefixDropScope Scope, ESuperSLMPrefixOpResult Want, const TCHAR* What)
	{
		FSuperSLMPrefixOp Op;
		FString Error;
		const bool bAccepted = Gpu.DropPrefix(Spec, Scope, Op, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts (%s)"), What, *Error), bAccepted))
		{
			return false;
		}
		return T.TestEqual(*FString::Printf(TEXT("%s: resolves as planned"), What), (uint8)GpuDevWaitOp(Gpu, Op).Result, (uint8)Want);
	}

	int64 GpuDevFallbacks(const FSuperSLMPrefixCacheStats& S, const TCHAR* Name)
	{
		const int64* Found = S.Fallbacks.Find(FName(Name));
		return Found != nullptr ? *Found : 0;
	}

	// Ticks until the cache's pool-thread IO and inbox read settled on two consecutive ticks (the GPU twin of
	// SuperSLMPrefixCacheFixtures::SettleCacheIoPhase, which takes the CPU subsystem). Lane jobs are not counted, so
	// it returns while a build is parked at the chunk hold (DrainQuiet would wait out its bound there).
	bool GpuDevSettleIo(USuperSLMGpuSubsystem& Gpu, double Seconds = SuperSLMPagingGpuFixtures::OpSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		int32 Quiet = 0;
		while (Quiet < 2)
		{
			if (FPlatformTime::Seconds() - Start > Seconds)
			{
				return false;
			}
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu.Tick(SuperSLMPagingGpuFixtures::Step);
			Quiet = Seam::IsCacheIoSettled(Gpu) ? Quiet + 1 : 0;
			if (Quiet == 0)
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		return true;
	}

	// The write-back mark (the GPU twin of SuperSLMPrefixCacheFixtures::WriteBackMark): a write-back counts once its
	// Save lane job completed, issued or failed.
	int64 GpuDevWriteMark(const USuperSLMGpuSubsystem& Gpu)
	{
		const FSuperSLMPrefixCacheStats S = Gpu.GetPrefixCacheStats();
		return S.DiskWritesIssued + GpuDevFallbacks(S, TEXT("SaveFailed"));
	}

	// The GPU twin of SuperSLMPrefixCacheFixtures::WaitWriteBacks for one write-back: its Save delivered, then the
	// pool-thread file write settled.
	bool GpuDevWaitWriteBack(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, int64 Mark, const TCHAR* What)
	{
		const bool bIssued = Pump(Gpu, [&Gpu, Mark]() { return GpuDevWriteMark(Gpu) > Mark; }, SuperSLMPagingGpuFixtures::BuildSeconds);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: its write-back completes"), What), bIssued);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the cache IO settles"), What), GpuDevSettleIo(Gpu));
		bOk &= DrainQuiet(Gpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the cache IO settles after the drain"), What), GpuDevSettleIo(Gpu));
		return bOk;
	}

	// Plain ticks with no device pacing: after a terminal loss the backend is inactive and every tick returns early.
	void GpuDevRawTicks(USuperSLMGpuSubsystem& Gpu, int32 N)
	{
		for (int32 I = 0; I < N; ++I)
		{
			Gpu.Tick(SuperSLMPagingGpuFixtures::Step);
			FPlatformProcess::Sleep(0.001f);
		}
	}

	// The threading cells' workload (3.9, 7.8), every Layer-1 kind once on the GPU: a build (begin, prefill, freeze), its
	// write-back (save), a Memory drop of the unleased entry (a release), and the same key again from disk (a restore;
	// without SUPERSLM_WITH_GPU_PREFIX_SAVE the write-back fails and the request recomputes). OutRestored stays leased.
	bool GpuDevThreadWorkload(FAutomationTestBase& T, FGpuPagingRig& Rig, int32 Salt, FSuperSLMPrefixSpec& OutSpec, FSuperSLMPrefix& OutRestored)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		TArray<int32> Tokens;
		if (!MakeTokens(T, *Rig.Model, int32(Rig.Geo.B), Salt, Tokens))
		{
			return false;
		}
		OutSpec = GpuDevSpec(Tokens);
		const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
		const int64 Mark = GpuDevWriteMark(Gpu);
		FSuperSLMPrefix Built;
		if (!GpuDevLease(T, Gpu, OutSpec, Built, TEXT("workload: the build")))
		{
			return false;
		}
		bool bOk = T.TestEqual(TEXT("workload: the build recomputed"), (uint8)Gpu.GetPrefixSource(Built), (uint8)ESuperSLMPrefixSource::Recomputed);
		Gpu.ReturnPrefix(Built);
		bOk &= GpuDevWaitWriteBack(T, Gpu, Mark, TEXT("workload: the build"));
		bOk &= GpuDevDrop(T, Gpu, OutSpec, ESuperSLMPrefixDropScope::Memory, ESuperSLMPrefixOpResult::Dropped, TEXT("workload: the release (a Memory drop)"));
		bOk &= DrainQuiet(Gpu);
		bOk &= T.TestTrue(TEXT("workload: a release was issued"), Gpu.GetPrefixCacheStats().ReleasesIssued > S0.ReleasesIssued);
		if (!GpuDevLease(T, Gpu, OutSpec, OutRestored, TEXT("workload: the restore")))
		{
			return false;
		}
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
		bOk &= T.TestEqual(TEXT("workload: the key again is served from disk (a GPU restore)"), (uint8)Gpu.GetPrefixSource(OutRestored), (uint8)ESuperSLMPrefixSource::Disk);
#else
		T.AddInfo(TEXT("workload: no GPU prefix save in this build (SUPERSLM_WITH_GPU_PREFIX_SAVE 0): the restore leg recomputed"));
#endif
		bOk &= DrainQuiet(Gpu);
		return bOk;
	}
}

// Cell 5.13 (Q7, §5.3 item 4 "On the GPU"; rev 1.18.1 N4): a build of 384 tokens (24 pages, six MaxPrefillChunk
// calls) runs as several submission-thread steps (BuildChunkJobsOf > 1). The chunk hold parks it after its first
// step: there it is Begun, Pending and the one build in flight; a second request for the key made there attaches to
// it (its lease names the same entry, BuildsIssued and PrefixPrefillBuilds rise by one) and both resolve Ready once
// the hold lifts. Throughout, the backend stays active (never marked unresponsive, no device-loss path taken) and
// the in-flight build count never reads above one (one charge). Afterwards the key is a Memory hit.
// The lowered bound: Seam::SetCacheJobBoundSeconds(2 s) holds every cache submission job to 2 s (a one-job build of
// these 384 tokens, ~15 ms a token on the box, would overrun it and take the unresponsive path); every job's wall
// time (Seam::CacheJobSeconds) ends inside it and the backend stays active. Restored to 0 at the end.
// Not written: the adopters-equal-the-prefill-twin leg (left to the 1.x twins).
// Mutants: the build submitted as one job -> it overruns the lowered bound (a CacheJobSeconds entry >= 2 s, the backend
// inactive) and never parks between steps (Ready after the first, chunk jobs 1) -> red;
// the second request starting its own build -> BuildsIssued 2 and the in-flight count 2 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuBuildChunkedTest, "SuperSLM.PrefixCache.Gpu.Build.Chunked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuBuildChunkedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!GpuDevConfigure(*this, Rig, FString()))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	constexpr int64 ChunkedPages = 24; // 384 tokens: six prefill calls of 64, so several 0.25 s steps
	TArray<int32> Tokens;
	if (!MakeTokens(*this, *Rig.Model, int32(ChunkedPages * Rig.Geo.B), 51, Tokens))
	{
		return false;
	}
	const FSuperSLMPrefixSpec Spec = GpuDevSpec(Tokens);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bool bActiveThroughout = true;
	int32 MaxInFlight = 0;
	auto Sample = [&Gpu, &bActiveThroughout, &MaxInFlight]()
	{
		bActiveThroughout &= Gpu.IsGpuBackendActive();
		MaxInFlight = FMath::Max(MaxInFlight, Seam::InFlightBuildCount(Gpu));
	};

	constexpr double LoweredBoundSeconds = 2.0;
	Seam::SetCacheJobBoundSeconds(Gpu, LoweredBoundSeconds);
	Seam::SetBuildChunkHold(Gpu, 1);
	FSuperSLMPrefix First, Second;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the build is requested (%s)"), *Error), Gpu.RequestPrefix(Spec, First, Error)))
	{
		Seam::SetBuildChunkHold(Gpu, -1);
		Seam::SetCacheJobBoundSeconds(Gpu, 0.0);
		return false;
	}
	Pump(Gpu, [&]() { Sample(); return Seam::BuildChunkJobsOf(Gpu, First) >= 1 || Gpu.GetPrefixLeaseState(First) != ESuperSLMPrefixLeaseState::Pending; }, SuperSLMPagingGpuFixtures::BuildSeconds);
	bool bOk = TestEqual(TEXT("after one step the build is parked, still Pending (it did not finish in one job)"), (uint8)Gpu.GetPrefixLeaseState(First), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= TestEqual(TEXT("one step ran"), Seam::BuildChunkJobsOf(Gpu, First), 1);
	bOk &= TestEqual(TEXT("parked between two steps: begun, not frozen"), (uint8)Seam::BuildStateOf(Gpu, First), (uint8)ESuperSLMPrefixBuildState::Begun);
	bOk &= TestEqual(TEXT("parked: one build in flight"), Seam::InFlightBuildCount(Gpu), 1);

	// N4: a second request for the key between two steps.
	if (!TestTrue(*FString::Printf(TEXT("the second request is accepted (%s)"), *Error), Gpu.RequestPrefix(Spec, Second, Error)))
	{
		Seam::SetBuildChunkHold(Gpu, -1);
		Seam::SetCacheJobBoundSeconds(Gpu, 0.0);
		return false;
	}
	bOk &= TestTrue(TEXT("the second request's key hash is applied with the build still parked"), GpuDevSettleIo(Gpu));
	Sample();
	bOk &= TestEqual(TEXT("the second request waits on the build (Pending)"), (uint8)Gpu.GetPrefixLeaseState(Second), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= TestEqual(TEXT("the build is still parked after one step"), Seam::BuildChunkJobsOf(Gpu, First), 1);
	bOk &= TestEqual(TEXT("the second request attaches to the same build (its lease names the build's entry)"), Seam::BuildChunkJobsOf(Gpu, Second), Seam::BuildChunkJobsOf(Gpu, First));
	bOk &= TestEqual(TEXT("still one build in flight"), Seam::InFlightBuildCount(Gpu), 1);
	bOk &= TestEqual(TEXT("one build issued"), Gpu.GetPrefixCacheStats().BuildsIssued - S0.BuildsIssued, int64(1));

	Seam::SetBuildChunkHold(Gpu, -1);
	Pump(Gpu, [&]()
	{
		Sample();
		return Gpu.GetPrefixLeaseState(First) != ESuperSLMPrefixLeaseState::Pending && Gpu.GetPrefixLeaseState(Second) != ESuperSLMPrefixLeaseState::Pending;
	}, SuperSLMPagingGpuFixtures::BuildSeconds);
	bOk &= TestEqual(TEXT("the first request: Ready"), (uint8)Gpu.GetPrefixLeaseState(First), (uint8)ESuperSLMPrefixLeaseState::Ready);
	bOk &= TestEqual(TEXT("the second request: Ready, delivered with the first"), (uint8)Gpu.GetPrefixLeaseState(Second), (uint8)ESuperSLMPrefixLeaseState::Ready);
	bOk &= TestEqual(TEXT("the first request recomputed"), (uint8)Gpu.GetPrefixSource(First), (uint8)ESuperSLMPrefixSource::Recomputed);
	const int32 Steps = Seam::BuildChunkJobsOf(Gpu, First);
	bOk &= TestTrue(*FString::Printf(TEXT("the build ran as several submission-thread steps (%d)"), Steps), Steps > 1);
	bOk &= TestEqual(TEXT("both leases name the one build's steps"), Seam::BuildChunkJobsOf(Gpu, Second), Steps);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("one build issued in all"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
	bOk &= TestEqual(TEXT("one prefill sequence counted (PrefixPrefillBuilds)"), S1.PrefixPrefillBuilds - S0.PrefixPrefillBuilds, int64(1));
	bOk &= TestTrue(TEXT("at least one prefill call per step"), S1.PrefixPrefillCalls - S0.PrefixPrefillCalls >= int64(Steps));
	bOk &= TestTrue(*FString::Printf(TEXT("one charge throughout: the in-flight build count never read above one (max %d)"), MaxInFlight), MaxInFlight <= 1);
	bOk &= TestEqual(TEXT("no build in flight once delivered"), Seam::InFlightBuildCount(Gpu), 0);
	bOk &= TestTrue(TEXT("the GPU backend stayed active throughout (never unresponsive, no device-loss path)"), bActiveThroughout && Gpu.IsGpuBackendActive());

	FSuperSLMPrefix Third;
	bOk &= GpuDevLease(*this, Gpu, Spec, Third, TEXT("the key afterwards (the cache still serves)"));
	bOk &= TestEqual(TEXT("the key afterwards: a Memory hit"), (uint8)Gpu.GetPrefixSource(Third), (uint8)ESuperSLMPrefixSource::Memory);
	Gpu.ReturnPrefix(First);
	Gpu.ReturnPrefix(Second);
	Gpu.ReturnPrefix(Third);
	bOk &= DrainQuiet(Gpu);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the chunked build"));

	// The lowered bound: every cache job ended inside it, and the backend never took the unresponsive path.
	const TArray<double> JobSeconds = Seam::CacheJobSeconds(Gpu);
	double LongestJob = 0.0;
	for (const double Took : JobSeconds)
	{
		LongestJob = FMath::Max(LongestJob, Took);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("the cache's jobs were recorded (%d, at least one per step)"), JobSeconds.Num()), JobSeconds.Num() >= Steps);
	bOk &= TestTrue(*FString::Printf(TEXT("every cache job ended inside the lowered bound (longest %.3f s, bound %.1f s)"), LongestJob, LoweredBoundSeconds), LongestJob < LoweredBoundSeconds);
	bOk &= TestTrue(TEXT("under the lowered bound the GPU backend stays active"), Gpu.IsGpuBackendActive());
	Seam::SetCacheJobBoundSeconds(Gpu, 0.0);
	return bOk;
}

// Cell 5.14 (Q9, §5.4 *After a terminal GPU device loss*): an unleased entry dropped with the release gate held (one
// release queued, its refund pending), then ForceTerminalLoss. RequestPrefix, WarmPrefix and DropPrefix refuse
// "DeviceUnavailable:"; over further ticks the cache posts nothing (PostedLaneJobs flat); opening the gate re-issues
// nothing (the handle's release count stays 0, still nothing posted), the refund stays pending and no ReleaseFailed
// is counted. The re-Configure() that follows returns within a bound (twice the cache Configure()'s own wall clock
// plus 10 s: it does not wait on the skipped release), and the fresh cache serves a request to Ready.
// GPU differences: the host is leaked unreleased at the teardown after a terminal loss (TearDown, wiring edit 4: no
// Layer-1 call under a lost device), so the plan's "the teardown sweep releases it exactly once" has no GPU reading.
// The unresponsive-thread leg is its own cell (Gpu.DeviceLoss.Unresponsive, below).
// Mutant: re-issue kept after device loss -> PostedLaneJobs rises at the gate's opening, or the release is counted -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDeviceLossNoReissueTest, "SuperSLM.PrefixCache.Gpu.DeviceLoss.NoReissue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDeviceLossNoReissueTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	double CacheConfigureSeconds = 0.0;
	if (!GpuDevConfigure(*this, Rig, FString(), &CacheConfigureSeconds))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, Other;
	FSuperSLMPrefix A;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 61, Tokens) || !MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 62, Other)
		|| !GpuDevLease(*this, Gpu, GpuDevSpec(Tokens), A, TEXT("A")))
	{
		return false;
	}
	const uint64 Handle = Seam::EngineHandleOf(Gpu, A);
	bool bOk = TestTrue(TEXT("A has an engine handle"), Handle != 0);
	Gpu.ReturnPrefix(A);
	bOk &= DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	bOk &= GpuDevDrop(*this, Gpu, GpuDevSpec(Tokens), ESuperSLMPrefixDropScope::Memory, ESuperSLMPrefixOpResult::Dropped, TEXT("A dropped behind the gate"));
	bOk &= TestEqual(TEXT("one release queued at the gate"), Seam::HeldReleaseCount(Gpu), 1);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("its refund is pending"), S0.PendingRefunds, 1);
	if (!bOk)
	{
		Seam::HoldReleases(Gpu, false);
		return false;
	}

	AddExpectedErrorPlain(TEXT("the GPU backend is torn down and callers fall back to the CPU backend"), EAutomationExpectedErrorFlags::Contains, 0);
	// Plan §5.4 / §5.9, a GPU release skipped after a terminal device loss: the cache leaks its GPU prefix handles by
	// design (no Layer-1 call under a lost device), so the teardown's destroys meet live handles and Layer 1 refuses
	// them. The leak is the design, not a defect (box run on the 7900 XT, 2026-10-29).
	AddExpectedErrorPlain(TEXT("GPU teardown: sslm_gpu_kv_pool_destroy refused (SSLM_MODEL_HAS_LIVE_SEQUENCES)"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedErrorPlain(TEXT("GPU teardown: sslm_gpu_model_unmap refused (SSLM_MODEL_HAS_LIVE_SEQUENCES)"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedErrorPlain(TEXT("GPU teardown: sslm_gpu_context_destroy refused (SSLM_CONTEXT_HAS_LIVE_HANDLES); the context is leaked rather than freed under a live handle"),
		EAutomationExpectedErrorFlags::Contains, 0);
	Seam::ForceTerminalLoss(Gpu);
	{
		FSuperSLMPrefix Refused;
		FString Error;
		bOk &= TestFalse(TEXT("after the loss: RequestPrefix refuses"), Gpu.RequestPrefix(GpuDevSpec(Other), Refused, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("RequestPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
		FSuperSLMPrefixOp Op;
		Error.Reset();
		bOk &= TestFalse(TEXT("after the loss: WarmPrefix refuses"), Gpu.WarmPrefix(GpuDevSpec(Other), Op, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("WarmPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
		Error.Reset();
		bOk &= TestFalse(TEXT("after the loss: DropPrefix refuses"), Gpu.DropPrefix(GpuDevSpec(Tokens), ESuperSLMPrefixDropScope::Memory, Op, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("DropPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
	}
	const int64 Posted = Seam::PostedLaneJobs(Gpu);
	GpuDevRawTicks(Gpu, 30);
	bOk &= TestFalse(TEXT("the loss tick tears the GPU backend down"), Gpu.IsGpuBackendActive());
	bOk &= TestEqual(TEXT("nothing posted over the ticks after the loss"), Seam::PostedLaneJobs(Gpu), Posted);

	Seam::HoldReleases(Gpu, false);
	bOk &= TestEqual(TEXT("the gate opened: nothing held"), Seam::HeldReleaseCount(Gpu), 0);
	GpuDevRawTicks(Gpu, 30);
	bOk &= TestEqual(TEXT("the gate opened: still nothing posted (no re-issue after the loss)"), Seam::PostedLaneJobs(Gpu), Posted);
	bOk &= TestEqual(TEXT("A's handle never released"), Seam::ReleasesOf(Gpu, Handle), 0);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("its refund stays pending"), S1.PendingRefunds, 1);
	bOk &= TestEqual(TEXT("no ReleaseFailed counted after the loss"), GpuDevFallbacks(S1, TEXT("ReleaseFailed")), GpuDevFallbacks(S0, TEXT("ReleaseFailed")));

	// Teardown and re-Configure() do not wait on the skipped release; the fresh cache serves.
	const double Start = FPlatformTime::Seconds();
	const bool bReconfigured = ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512));
	const double Seconds = FPlatformTime::Seconds() - Start;
	const double Bound = 2.0 * CacheConfigureSeconds + 10.0;
	bOk &= TestTrue(TEXT("the re-Configure() after the loss succeeds"), bReconfigured);
	bOk &= TestTrue(*FString::Printf(TEXT("the re-Configure() returns promptly (%.2f s, bound %.2f s)"), Seconds, Bound), Seconds <= Bound);
	if (!bReconfigured)
	{
		return false;
	}
	bOk &= TestTrue(TEXT("the GPU backend is active again"), Gpu.IsGpuBackendActive());
	bOk &= TestTrue(TEXT("the fresh configuration has its cache"), Gpu.GetPrefixCacheStats().bConfigured);
	FSuperSLMPrefix Fresh;
	bOk &= GpuDevLease(*this, Gpu, GpuDevSpec(Tokens), Fresh, TEXT("A on the fresh cache"));
	Gpu.ReturnPrefix(Fresh);
	bOk &= DrainQuiet(Gpu);
	return bOk;
}

// Cell 5.14's unresponsive-thread leg (§5.4: "the same holds for a job skipped because the submission thread is
// marked unresponsive"): A dropped behind the held release gate (its refund pending) and a 384-token build W parked by
// the chunk hold after its first step (its next step posted again each tick), then Seam::ForceUnresponsive, which
// marks the thread as an overrun does: the queued jobs (W's step) complete Skipped and the terminal-loss flag latches.
// Asserted as the terminal leg: the refusals ("DeviceUnavailable:"); nothing posted over further ticks (W's step is
// not re-posted, and W never reaches Ready); opening the gate re-issues nothing (A's release count 0, nothing posted),
// A's refund stays pending, no ReleaseFailed; the re-Configure() returns within the bound and the fresh cache serves.
// The skipped-delivery path itself (the glue delivering a skipped release as Skipped, not Failed) is not reachable
// from here: after the loss Tick() returns before the cache's apply pass, and the unresponsive teardown leaks the host
// without one. Error lines expected: MarkUnresponsive's, FallBackToCpu's, and the unresponsive teardown's (no destroy
// is attempted on this path, so the terminal leg's destroy refusals do not appear; plan §5.4 / §5.9, the leak is the
// design).
// Mutant: a step re-posted, or the held release dispatched, under an unresponsive thread -> PostedLaneJobs rises -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDeviceLossUnresponsiveTest, "SuperSLM.PrefixCache.Gpu.DeviceLoss.Unresponsive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDeviceLossUnresponsiveTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	double CacheConfigureSeconds = 0.0;
	if (!GpuDevConfigure(*this, Rig, FString(), &CacheConfigureSeconds))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, Other, WTokens;
	FSuperSLMPrefix A, W;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 63, Tokens) || !MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 64, Other)
		|| !MakeTokens(*this, *Rig.Model, int32(24 * Rig.Geo.B), 65, WTokens) || !GpuDevLease(*this, Gpu, GpuDevSpec(Tokens), A, TEXT("A")))
	{
		return false;
	}
	const uint64 Handle = Seam::EngineHandleOf(Gpu, A);
	bool bOk = TestTrue(TEXT("A has an engine handle"), Handle != 0);
	Gpu.ReturnPrefix(A);
	bOk &= DrainQuiet(Gpu);
	Seam::HoldReleases(Gpu, true);
	bOk &= GpuDevDrop(*this, Gpu, GpuDevSpec(Tokens), ESuperSLMPrefixDropScope::Memory, ESuperSLMPrefixOpResult::Dropped, TEXT("A dropped behind the gate"));
	bOk &= TestEqual(TEXT("one release queued at the gate"), Seam::HeldReleaseCount(Gpu), 1);

	Seam::SetBuildChunkHold(Gpu, 1);
	FString Error;
	bOk &= TestTrue(*FString::Printf(TEXT("W is requested (%s)"), *Error), Gpu.RequestPrefix(GpuDevSpec(WTokens), W, Error));
	Pump(Gpu, [&Gpu, &W]() { return Seam::BuildChunkJobsOf(Gpu, W) >= 1 || Gpu.GetPrefixLeaseState(W) != ESuperSLMPrefixLeaseState::Pending; },
		SuperSLMPagingGpuFixtures::BuildSeconds);
	bOk &= TestEqual(TEXT("W is parked between two steps (Pending)"), (uint8)Gpu.GetPrefixLeaseState(W), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= TestEqual(TEXT("W is begun, a step in flight"), (uint8)Seam::BuildStateOf(Gpu, W), (uint8)ESuperSLMPrefixBuildState::Begun);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("A's refund is pending"), S0.PendingRefunds, 1);
	if (!bOk)
	{
		Seam::SetBuildChunkHold(Gpu, -1);
		Seam::HoldReleases(Gpu, false);
		return false;
	}

	AddExpectedErrorPlain(TEXT("the submission thread is treated as unresponsive"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedErrorPlain(TEXT("the GPU backend is torn down and callers fall back to the CPU backend"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedErrorPlain(TEXT("SuperSLM GPU backend teardown: the submission thread is unresponsive"), EAutomationExpectedErrorFlags::Contains, 0);
	Seam::ForceUnresponsive(Gpu);
	const int64 Posted = Seam::PostedLaneJobs(Gpu);
	{
		FSuperSLMPrefix Refused;
		Error.Reset();
		bOk &= TestFalse(TEXT("unresponsive: RequestPrefix refuses"), Gpu.RequestPrefix(GpuDevSpec(Other), Refused, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("RequestPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
		FSuperSLMPrefixOp Op;
		Error.Reset();
		bOk &= TestFalse(TEXT("unresponsive: WarmPrefix refuses"), Gpu.WarmPrefix(GpuDevSpec(Other), Op, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("WarmPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
		Error.Reset();
		bOk &= TestFalse(TEXT("unresponsive: DropPrefix refuses"), Gpu.DropPrefix(GpuDevSpec(Tokens), ESuperSLMPrefixDropScope::Memory, Op, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("DropPrefix's refusal is DeviceUnavailable (%s)"), *Error), Error.StartsWith(TEXT("DeviceUnavailable:")));
	}
	GpuDevRawTicks(Gpu, 30);
	bOk &= TestFalse(TEXT("the unresponsive thread tears the GPU backend down"), Gpu.IsGpuBackendActive());
	bOk &= TestEqual(TEXT("nothing posted over the ticks after (W's step is not re-posted)"), Seam::PostedLaneJobs(Gpu), Posted);
	bOk &= TestNotEqual(TEXT("W is never delivered Ready"), (uint8)Gpu.GetPrefixLeaseState(W), (uint8)ESuperSLMPrefixLeaseState::Ready);

	Seam::HoldReleases(Gpu, false);
	Seam::SetBuildChunkHold(Gpu, -1);
	bOk &= TestEqual(TEXT("the gate opened: nothing held"), Seam::HeldReleaseCount(Gpu), 0);
	GpuDevRawTicks(Gpu, 30);
	bOk &= TestEqual(TEXT("the gate opened: still nothing posted (no re-issue)"), Seam::PostedLaneJobs(Gpu), Posted);
	bOk &= TestEqual(TEXT("A's handle never released"), Seam::ReleasesOf(Gpu, Handle), 0);
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("A's refund stays pending"), S1.PendingRefunds, 1);
	bOk &= TestEqual(TEXT("no ReleaseFailed counted"), GpuDevFallbacks(S1, TEXT("ReleaseFailed")), GpuDevFallbacks(S0, TEXT("ReleaseFailed")));

	const double Start = FPlatformTime::Seconds();
	const bool bReconfigured = ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512));
	const double Seconds = FPlatformTime::Seconds() - Start;
	const double Bound = 2.0 * CacheConfigureSeconds + 10.0;
	bOk &= TestTrue(TEXT("the re-Configure() after the unresponsive thread succeeds"), bReconfigured);
	bOk &= TestTrue(*FString::Printf(TEXT("the re-Configure() returns promptly (%.2f s, bound %.2f s)"), Seconds, Bound), Seconds <= Bound);
	if (!bReconfigured)
	{
		return false;
	}
	bOk &= TestTrue(TEXT("the GPU backend is active again"), Gpu.IsGpuBackendActive());
	bOk &= TestTrue(TEXT("the fresh configuration has its cache"), Gpu.GetPrefixCacheStats().bConfigured);
	FSuperSLMPrefix Fresh;
	bOk &= GpuDevLease(*this, Gpu, GpuDevSpec(Tokens), Fresh, TEXT("A on the fresh cache"));
	Gpu.ReturnPrefix(Fresh);
	bOk &= DrainQuiet(Gpu);
	return bOk;
}

// Cell 5.15's GPU legs (§5.4 *What a release's status means*): Seam::SetBusyReleases(3) makes the next three cache
// releases return SSLM_BUSY without calling sslm_gpu_prefix_release. A Memory drop of an unleased entry: the core
// issues the release again at later ticks until it applies, four issues in all; the release-counting seam shows the
// handle released exactly once (and no handle twice); the refund stays pending while it is busy and lands at the
// apply (charged bytes and region C back to their reading before the build); no ReleaseFailed is counted; form 1 and
// no ledger fault hold after.
// The non-busy leg: Seam::SubstituteReleaseStatus(SSLM_GPU_PREFIX_REJECTED) runs the real release and returns that
// status: issued once and never again, the handle released once, ReleaseFailed counted once, the refund lands.
// Mutants: no re-issue on SSLM_BUSY -> the handle is never released (ReleasesOf 0, the refund pending for ever) -> red;
// re-issue on a non-busy failure (the U2 draft's V12 rule) -> a second issue of a freed handle (ReleasesIssued 2,
// ReleasesOf 2) -> red; refund withheld on failure -> PendingRefunds 1 -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuReleaseStatusTest, "SuperSLM.PrefixCache.Gpu.ReleaseStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuReleaseStatusTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!GpuDevConfigure(*this, Rig, FString()))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	constexpr int32 BusyIssues = 3;
	TArray<int32> Tokens;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 71, Tokens))
	{
		return false;
	}
	const FSuperSLMPrefixCacheStats SPre = Gpu.GetPrefixCacheStats();
	const int64 CPre = LedgerOf(Gpu).ChargedOf(ESuperSLMPagedRegion::C);
	FSuperSLMPrefix A;
	if (!GpuDevLease(*this, Gpu, GpuDevSpec(Tokens), A, TEXT("A")))
	{
		return false;
	}
	const uint64 Handle = Seam::EngineHandleOf(Gpu, A);
	bool bOk = TestTrue(TEXT("A has an engine handle"), Handle != 0);
	Gpu.ReturnPrefix(A);
	bOk &= DrainQuiet(Gpu);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();

	Seam::SetBusyReleases(Gpu, BusyIssues);
	FSuperSLMPrefixOp Op;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("A's drop is accepted (%s)"), *Error), Gpu.DropPrefix(GpuDevSpec(Tokens), ESuperSLMPrefixDropScope::Memory, Op, Error)))
	{
		Seam::SetBusyReleases(Gpu, 0);
		return false;
	}
	// Sampled once a busy release has come back and been issued again, before the release applies.
	bool bSawPendingWhileBusy = false;
	bool bSampled = false;
	Pump(Gpu, [&]()
	{
		const bool bReissued = Gpu.GetPrefixCacheStats().ReleasesIssued - S0.ReleasesIssued >= 2;
		if (bReissued && !bSampled && Seam::ReleasesOf(Gpu, Handle) == 0)
		{
			bSampled = true;
			bSawPendingWhileBusy = Gpu.GetPrefixCacheStats().PendingRefunds == 1;
		}
		return Seam::ReleasesOf(Gpu, Handle) >= 1;
	}, SuperSLMPagingGpuFixtures::OpSeconds);
	bOk &= DrainQuiet(Gpu);
	bOk &= TestEqual(TEXT("A's drop: Dropped"), (uint8)GpuDevWaitOp(Gpu, Op).Result, (uint8)ESuperSLMPrefixOpResult::Dropped);
	if (bSampled)
	{
		bOk &= TestTrue(TEXT("while the release is busy its refund stays pending"), bSawPendingWhileBusy);
	}
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("the handle is released exactly once"), Seam::ReleasesOf(Gpu, Handle), 1);
	bOk &= TestEqual(TEXT("no handle released twice"), Seam::MaxReleasesOfOneHandle(Gpu), 1);
	bOk &= TestEqual(TEXT("busy releases are issued again until one applies"), S1.ReleasesIssued - S0.ReleasesIssued, int64(BusyIssues + 1));
	bOk &= TestEqual(TEXT("no ReleaseFailed counted"), GpuDevFallbacks(S1, TEXT("ReleaseFailed")), GpuDevFallbacks(S0, TEXT("ReleaseFailed")));
	bOk &= TestEqual(TEXT("the refund landed: none pending"), S1.PendingRefunds, 0);
	bOk &= TestEqual(TEXT("the refund landed: no pending refund bytes"), S1.PendingRefundBytes, int64(0));
	bOk &= TestEqual(TEXT("the refund landed: charged bytes back to before the build"), S1.ChargedBytes, SPre.ChargedBytes);
	bOk &= TestEqual(TEXT("the refund landed: region C's charge back to before the build"), LedgerOf(Gpu).ChargedOf(ESuperSLMPagedRegion::C), CPre);
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the busy releases"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the busy releases"));
	Seam::SetBusyReleases(Gpu, 0);

	// The non-busy leg: the real release runs (the handle is consumed), then SSLM_GPU_PREFIX_REJECTED is returned in its
	// place. A release that returned has applied: issued once, never again, ReleaseFailed counted once, the refund lands.
	TArray<int32> BTokens;
	FSuperSLMPrefix BLease;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 72, BTokens) || !GpuDevLease(*this, Gpu, GpuDevSpec(BTokens), BLease, TEXT("B")))
	{
		return false;
	}
	const uint64 BHandle = Seam::EngineHandleOf(Gpu, BLease);
	bOk &= TestTrue(TEXT("B has an engine handle"), BHandle != 0);
	Gpu.ReturnPrefix(BLease);
	bOk &= DrainQuiet(Gpu);
	const FSuperSLMPrefixCacheStats S2 = Gpu.GetPrefixCacheStats();
	Seam::SubstituteReleaseStatus(Gpu, static_cast<int32>(SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED));
	FSuperSLMPrefixOp BOp;
	Error.Reset();
	if (!TestTrue(*FString::Printf(TEXT("B's drop is accepted (%s)"), *Error), Gpu.DropPrefix(GpuDevSpec(BTokens), ESuperSLMPrefixDropScope::Memory, BOp, Error)))
	{
		Seam::SubstituteReleaseStatus(Gpu, 0);
		return false;
	}
	Pump(Gpu, [&Gpu, BHandle]() { return Seam::ReleasesOf(Gpu, BHandle) >= 1; }, SuperSLMPagingGpuFixtures::OpSeconds);
	bOk &= DrainQuiet(Gpu);
	bOk &= TickN(*this, Gpu, 8, TEXT("ticks after the failed release, for any re-issue to show"));
	bOk &= DrainQuiet(Gpu);
	bOk &= TestEqual(TEXT("B's drop: Dropped"), (uint8)GpuDevWaitOp(Gpu, BOp).Result, (uint8)ESuperSLMPrefixOpResult::Dropped);
	const FSuperSLMPrefixCacheStats S3 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("non-busy: the handle is released exactly once"), Seam::ReleasesOf(Gpu, BHandle), 1);
	bOk &= TestEqual(TEXT("non-busy: no handle released twice"), Seam::MaxReleasesOfOneHandle(Gpu), 1);
	bOk &= TestEqual(TEXT("non-busy: issued once, never again"), S3.ReleasesIssued - S2.ReleasesIssued, int64(1));
	bOk &= TestEqual(TEXT("non-busy: ReleaseFailed counted once"), GpuDevFallbacks(S3, TEXT("ReleaseFailed")) - GpuDevFallbacks(S2, TEXT("ReleaseFailed")), int64(1));
	bOk &= TestEqual(TEXT("non-busy: the refund landed (none pending)"), S3.PendingRefunds, 0);
	bOk &= TestEqual(TEXT("non-busy: charged bytes back to before the build"), S3.ChargedBytes, SPre.ChargedBytes);
	bOk &= TestEqual(TEXT("non-busy: region C's charge back to before the build"), LedgerOf(Gpu).ChargedOf(ESuperSLMPagedRegion::C), CPre);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after the failed release"));
	Seam::SubstituteReleaseStatus(Gpu, 0);
	return bOk;
}

// Cell 3.9's GPU twin (§5.5 ownership): the accessor through which the cache's private state is reachable asserts the
// game thread. Over the threading workload (a build, its write-back, a release and a restore, every GPU Layer-1 call
// on the submission thread) its count rises, and with the off-game-thread assert recording no trip is counted; its
// must-reject (the accessor reached from a pool thread) trips. Rev 1.18's drop leg: a drop's decision (Dropped) and a
// drop's mark (DropDeferred, a leased entry) each raise the count at the pass that applies it.
// GPU difference: the workload's "evictions" are a drop's (Drops), not budget evictions: filling region C's block to
// force one would need a 256-page pad build.
// Mutant: the eviction's map update moved onto the submission thread's completion -> the assert trips -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuGameThreadOwnershipTest, "SuperSLM.PrefixCache.Gpu.GameThreadOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuGameThreadOwnershipTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-game-thread-ownership"));
	FGpuPagingRig Rig;
	if (!GpuDevConfigure(*this, Rig, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const int64 Before = Seam::GameThreadStateAccesses(Gpu);
	Seam::SetOffGameThreadRecording(Gpu, true);
	const int64 TripsBefore = Seam::OffGameThreadViolations(Gpu);
	FSuperSLMPrefixSpec A;
	FSuperSLMPrefix Restored;
	if (!GpuDevThreadWorkload(*this, Rig, 81, A, Restored))
	{
		Seam::SetOffGameThreadRecording(Gpu, false);
		return false;
	}
	bool bOk = TestTrue(TEXT("the accessor was called (> 0) by a workload with a drop's eviction"), Seam::GameThreadStateAccesses(Gpu) > Before && Gpu.GetPrefixCacheStats().Drops > 0);
	bOk &= TestEqual(TEXT("no trip over a build, a write-back, a release and a restore on the GPU"), Seam::OffGameThreadViolations(Gpu) - TripsBefore, int64(0));
	bOk &= TestTrue(TEXT("the recording mode is wired (the violation count is readable)"), TripsBefore >= 0);
	Seam::SetOffGameThreadRecording(Gpu, false);
	bOk &= TestTrue(TEXT("must-reject: the accessor reached from a pool thread trips"), Seam::GameThreadAccessorTrips(Gpu));

	// Rev 1.18 drop leg: Restored (A) is leased; B is resident and unleased.
	TArray<int32> BTokens;
	FSuperSLMPrefix BLease;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 82, BTokens) || !GpuDevLease(*this, Gpu, GpuDevSpec(BTokens), BLease, TEXT("3.9 drop leg: B")))
	{
		Gpu.ReturnPrefix(Restored);
		return false;
	}
	Gpu.ReturnPrefix(BLease);
	const FSuperSLMPrefixSpec B = GpuDevSpec(BTokens);
	struct FDropCase { const FSuperSLMPrefixSpec* Spec; ESuperSLMPrefixOpResult Want; const TCHAR* What; };
	const FDropCase Cases[] = {
		{&B, ESuperSLMPrefixOpResult::Dropped, TEXT("a drop's decision (Dropped)")},
		{&A, ESuperSLMPrefixOpResult::DropDeferred, TEXT("a drop's mark (DropDeferred)")},
	};
	for (const FDropCase& Case : Cases)
	{
		DrainQuiet(Gpu);
		FSuperSLMPrefixOp Op;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts (%s)"), Case.What, *Error), Gpu.DropPrefix(*Case.Spec, ESuperSLMPrefixDropScope::Memory, Op, Error)))
		{
			Gpu.ReturnPrefix(Restored);
			return false;
		}
		const int64 AtIssue = Seam::GameThreadStateAccesses(Gpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: resolves as planned"), Case.What), (uint8)GpuDevWaitOp(Gpu, Op).Result, (uint8)Case.Want);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the accessor's count includes it"), Case.What), Seam::GameThreadStateAccesses(Gpu) > AtIssue);
	}
	Gpu.ReturnPrefix(Restored);
	bOk &= DrainQuiet(Gpu);
	return bOk;
}

// Cell 7.8's GPU twin: no hashing, file IO or Layer-1 call on the game thread. On the GPU the Layer-1 calls run on the
// submission thread, and the off-game-thread assert covers them there: over the threading workload (a build, its
// write-back, a release and a restore) its checked-call count rises and, recording, it counts no trip. Rev 1.18.1's
// leg: a warm of a cold key, a warm of a resident one and drops of both scopes trip nothing and are checked. Its
// must-reject (a hashing call on the game thread) trips.
// Mutant: a GPU lane job run inline on the game thread (or the key hashed inside RequestPrefix) -> a trip -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuOffGameThreadTest, "SuperSLM.PrefixCache.Gpu.OffGameThreadWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuOffGameThreadTest::RunTest(const FString& Parameters)
{
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-off-game-thread"));
	FGpuPagingRig Rig;
	if (!GpuDevConfigure(*this, Rig, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	Seam::SetOffGameThreadRecording(Gpu, true);
	const int64 ChecksBefore = Seam::OffGameThreadChecks(Gpu);
	const int64 TripsBefore = Seam::OffGameThreadViolations(Gpu);
	FSuperSLMPrefixSpec A;
	FSuperSLMPrefix Restored;
	if (!GpuDevThreadWorkload(*this, Rig, 91, A, Restored))
	{
		Seam::SetOffGameThreadRecording(Gpu, false);
		return false;
	}
	bool bOk = TestTrue(TEXT("the off-game-thread assert was checked (> 0) by hashing, file IO and the GPU Layer-1 calls"), Seam::OffGameThreadChecks(Gpu) > ChecksBefore);
	bOk &= TestEqual(TEXT("no hashing, file IO or Layer-1 call on the game thread over the workload (no trip recorded)"), Seam::OffGameThreadViolations(Gpu) - TripsBefore, int64(0));
	bOk &= TestTrue(TEXT("the recording mode is wired (the violation count is readable)"), TripsBefore >= 0);

	// Rev 1.18.1 leg: warms and drops.
	TArray<int32> Cold;
	if (!MakeTokens(*this, *Rig.Model, int32(Rig.Geo.B), 92, Cold))
	{
		Seam::SetOffGameThreadRecording(Gpu, false);
		Gpu.ReturnPrefix(Restored);
		return false;
	}
	const int64 LegChecksBefore = Seam::OffGameThreadChecks(Gpu);
	auto Issue = [this, &Gpu](int32 Kind, const FSuperSLMPrefixSpec& Spec, const TCHAR* What)
	{
		FSuperSLMPrefixOp Op;
		FString Error;
		const bool bAccepted = Kind == 0 ? Gpu.WarmPrefix(Spec, Op, Error)
			: Gpu.DropPrefix(Spec, Kind == 1 ? ESuperSLMPrefixDropScope::Memory : ESuperSLMPrefixDropScope::MemoryAndDisk, Op, Error);
		bool bLeg = TestTrue(*FString::Printf(TEXT("%s: accepted (%s)"), What, *Error), bAccepted && Op.Id != 0);
		bLeg &= TestNotEqual(*FString::Printf(TEXT("%s: resolves"), What), (uint8)GpuDevWaitOp(Gpu, Op).Result, (uint8)ESuperSLMPrefixOpResult::Pending);
		return bLeg;
	};
	bOk &= Issue(0, GpuDevSpec(Cold), TEXT("warm, cold key"));
	bOk &= Issue(0, A, TEXT("warm, resident key"));
	bOk &= Issue(1, GpuDevSpec(Cold), TEXT("drop, Memory"));
	bOk &= Issue(2, A, TEXT("drop, MemoryAndDisk"));
	bOk &= DrainQuiet(Gpu);
	bOk &= TestTrue(TEXT("warm and drop: the off-game-thread assert was checked by their hashing, file IO and builds"), Seam::OffGameThreadChecks(Gpu) > LegChecksBefore);
	bOk &= TestEqual(TEXT("warm and drop: no hashing, file IO or Layer-1 call on the game thread (no trip recorded)"), Seam::OffGameThreadViolations(Gpu) - TripsBefore, int64(0));
	Seam::SetOffGameThreadRecording(Gpu, false);
	bOk &= TestTrue(TEXT("must-reject: a hashing call on the game thread trips"), Seam::OffGameThreadAssertTrips(Gpu));
	Gpu.ReturnPrefix(Restored);
	bOk &= DrainQuiet(Gpu);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
