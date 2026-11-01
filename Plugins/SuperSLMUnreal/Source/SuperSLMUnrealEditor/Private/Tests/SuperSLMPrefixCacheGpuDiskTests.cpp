// Precache plan rev 1.17, the U6 suite's GPU twins of the disk tier (§5.3, §5.7) and every fallback (§5.9): the
// GPU subsystem's cache instance (pooled mode), its disk files keyed by backend 1 and the adapter's vendor and
// device. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on. Every cell here writes disk files through the
// GPU write-back (sslm_gpu_prefix_save) and serves them through sslm_gpu_prefix_restore, so every cell needs
// SUPERSLM_WITH_GPU_PREFIX_SAVE as well (on for Win64 in SuperSLMUnreal.Build.cs) and fails by name without it.
//
//   SuperSLM.PrefixCache.Gpu.Disk.WarmAcrossLifetimes  cell 1.6 twin                                         [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.HostileFiles         cell 2.8 twin, plus the GPU's own: another adapter's
//                                                      key, and a CPU-backend wrapper under the GPU key       [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.CoalescedLoadFails   cell 3.5 twin, the failed shared load leg             [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.DeinitializeInFlight cell 3.7 twin                                         [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.TwoInstances         cell 3.8 twin (two GPU subsystems, two worlds)        [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.EveryFallback        cell 5.4 (and 7.9) twin, with §5.9's GPU producer row [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.CrashBeforeRename    cell 5.8 twin                                         [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.TrimOldestFirst      cell 7.14 twin                                        [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.WrapperEvolution     cell 9.6 twin                                         [U6]
//   SuperSLM.PrefixCache.Gpu.Disk.SpecCompare          cell 11.4 twin (disk and memory tiers)                [U6]
//
// The constructions are the CPU file's (SuperSLMPrefixCacheCpuDiskTests.cpp) on the GPU pooled rig (A-EX, B = 16,
// two slots of budget 512, region C one block), with every prefix one page (16 tokens): the GPU builds are slow on
// the box, and no assertion here needs a longer prefix. The differences from the CPU cells, each said in its cell:
//   - The GPU cache is pooled (budget mode) only: there is no legacy leg.
//   - The GPU cache ends with its configuration (SuperSLMGpuSubsystem.h): a re-Configure() makes a new instance, so
//     "a cold memory tier" is a re-Configure(), its statistics restart at 0, and the seams that are "cleared at
//     Configure()" (the collision, the disk fault, the before-read hook) are always set after it.
//   - The prefill twin. The CPU's twin is a CreatePrefix() build adopted and generated; on the GPU that is not the
//     oracle (the per-token feed and the cache's chunked prefill may differ numerically: UP-13, cell 10.5, as
//     SuperSLMPagingGpuPrefixTests.cpp says). The GPU twin is the cache's own recomputed lease, adopted and generated
//     before its write-back: a disk hit restores the bytes that build saved, and a recompute repeats that build, so
//     either one's adopter must produce the twin's tokens exactly.
//
// The fallback names are the host-tested core's (SuperSLMCore::PrefixFallbackName), logged by the shared host as
// "PrefixCache fallback <Name>: <message>".

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "SuperSLMPrefixCacheTestSeam.h"

#include <atomic>

// The runtime module's GPU cache host defaults this switch (SuperSLMGpuPrefixCacheHost.h, private); this file
// reads it too, so it carries the same default.
#ifndef SUPERSLM_WITH_GPU_PREFIX_SAVE
#define SUPERSLM_WITH_GPU_PREFIX_SAVE 0
#endif

namespace
{
	using namespace SuperSLMPagingGpuFixtures;
	using Seam = FSuperSLMPrefixCacheTestSeam;

	// Named here, not read unqualified: in a unity build another file's `using namespace SuperSLMPrefixCacheFixtures`
	// makes the CPU fixture's Step/OpSeconds/BuildSeconds visible beside the GPU fixture's.
	constexpr int64 kGpuDiskMax = int64(1) << 30;
	constexpr double kGpuDiskBuildSeconds = SuperSLMPagingGpuFixtures::BuildSeconds;
	constexpr double kGpuDiskOpSeconds = SuperSLMPagingGpuFixtures::OpSeconds;
	constexpr float kGpuDiskStep = SuperSLMPagingGpuFixtures::Step;
	constexpr int32 kGpuDiskSuffixTokens = 4;
	constexpr int32 kGpuDiskTwinNewTokens = 8;

	FSuperSLMPrefixSpec GpuDiskSpec(const TArray<int32>& Tokens, FName Schema = NAME_None)
	{
		return SuperSLMPrefixCacheFixtures::Spec(Tokens, Schema);
	}

	// --- The rig ---------------------------------------------------------------------------------

	struct FGpuDiskRig
	{
		FGpuPagingRig Rig;
		FSuperSLMGpuPrefixCacheSettings Settings;
		USuperSLMGpuSubsystem& Gpu() const { return *Rig.Gpu; }
	};

	// ConfigurePrefixCache(Settings), then a pooled Configure() at the rig's configuration: a new cache instance, its
	// memory tier cold, its statistics and seams reset.
	bool GpuDiskReconfigure(FAutomationTestBase& T, FGpuDiskRig& R)
	{
		R.Rig.Gpu->ConfigurePrefixCache(R.Settings);
		if (!ConfigurePooled(T, R.Rig, R.Rig.Config))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S = R.Rig.Gpu->GetPrefixCacheStats();
		return T.TestTrue(TEXT("the GPU cache is configured, on region C"), S.bConfigured && S.bBudgetMode);
	}

	// The A-EX pooled rig (2 slots, budget 512), with a cache of one page's bytes (region C one block) and its disk
	// tier in Dir.
	bool GpuDiskRigInit(FAutomationTestBase& T, FGpuDiskRig& R, const FString& Dir, int64 DiskMax = kGpuDiskMax)
	{
		if (!R.Rig.Init(T) || !ConfigurePooled(T, R.Rig, MakeGpuConfig(R.Rig.Artifact, 2, 512)) || !RequireAExShape(T, R.Rig))
		{
			return false;
		}
		R.Settings.BudgetBytes = R.Rig.Geo.PageBytes;
		R.Settings.bDiskEnabled = true;
		R.Settings.DiskDirectory = Dir;
		R.Settings.DiskMaxBytes = DiskMax;
		return GpuDiskReconfigure(T, R);
	}

	// One page of tokens (16 at B = 16).
	bool GpuDiskTokens(FAutomationTestBase& T, const FGpuDiskRig& R, int32 Salt, TArray<int32>& Out)
	{
		return MakeTokens(T, *R.Rig.Model, int32(R.Rig.Geo.B), Salt, Out);
	}

	// --- Requests ------------------------------------------------------------------------------

	bool GpuDiskRequest(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Gpu.RequestPrefix(S, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: RequestPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: a handle is minted"), What), Out.IsValid());
	}

	ESuperSLMPrefixLeaseState GpuDiskWaitLease(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, double Seconds = kGpuDiskBuildSeconds)
	{
		Pump(Gpu, [&Gpu, &Lease]() { return Gpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, Seconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool GpuDiskExpectLease(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixLeaseState Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: the lease resolves by name"), What), (uint8)GpuDiskWaitLease(Gpu, Lease), (uint8)Want);
	}

	bool GpuDiskReady(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		return GpuDiskRequest(T, Gpu, S, Out, What) && GpuDiskExpectLease(T, Gpu, Out, ESuperSLMPrefixLeaseState::Ready, What);
	}

	bool GpuDiskExpectSource(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixSource Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: GetPrefixSource()"), What), (uint8)Gpu.GetPrefixSource(Lease), (uint8)Want);
	}

	int64 GpuDiskFallbacks(const FSuperSLMPrefixCacheStats& S, const TCHAR* Name)
	{
		const int64* Found = S.Fallbacks.Find(FName(Name));
		return Found ? *Found : 0;
	}

	int64 GpuDiskServed(const FSuperSLMPrefixCacheStats& S, ESuperSLMPrefixSource Source) { return S.ServedBySource[(int32)Source]; }

	// --- Disk write-backs: the CPU fixture's WriteBackMark / WaitWriteBacks / WaitDiskQuiet, for the GPU ------------
	//
	// SuperSLMPrefixCacheFixtures' versions take a USuperSLMSubsystem; these are their GPU twins. A write-back is a Save
	// lane job (sslm_gpu_prefix_save, on the submission thread), then a file write on a pool thread: a cell waits for the
	// write being issued (DiskWritesIssued) or failing (SaveFailed), then for the pool-thread IO and inbox to settle on
	// consecutive ticks, the submission-thread jobs to drain, and the IO to settle again. Ticks are paced on the device
	// (Pump's rule).

	int64 GpuDiskWriteBackMark(const USuperSLMGpuSubsystem& Gpu)
	{
		const FSuperSLMPrefixCacheStats S = Gpu.GetPrefixCacheStats();
		return S.DiskWritesIssued + GpuDiskFallbacks(S, TEXT("SaveFailed"));
	}

	bool GpuDiskSettleIoPhase(USuperSLMGpuSubsystem& Gpu, double Seconds)
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
			Gpu.Tick(kGpuDiskStep);
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

	bool GpuDiskWaitQuiet(USuperSLMGpuSubsystem& Gpu, double Seconds = kGpuDiskOpSeconds)
	{
		const bool bIo = GpuDiskSettleIoPhase(Gpu, Seconds);
		const bool bJobs = DrainQuiet(Gpu, Seconds);
		return GpuDiskSettleIoPhase(Gpu, Seconds) && bIo && bJobs;
	}

	bool GpuDiskWaitWriteBacks(USuperSLMGpuSubsystem& Gpu, int64 Mark, int64 Count = 1, double Seconds = kGpuDiskBuildSeconds)
	{
		Pump(Gpu, [&Gpu, Mark, Count]() { return GpuDiskWriteBackMark(Gpu) >= Mark + Count; }, Seconds);
		const bool bIssued = GpuDiskWriteBackMark(Gpu) >= Mark + Count;
		return GpuDiskWaitQuiet(Gpu) && bIssued;
	}

	// What is still outstanding, for a failure message.
	FString GpuDiskQuietReport(const USuperSLMGpuSubsystem& Gpu)
	{
		return FString::Printf(TEXT("pool IO in flight %d, IO and inbox settled %s, submission-thread jobs outstanding %d tick + %d other, cache lane jobs posted %lld"),
			Seam::DiskIoInFlight(Gpu), Seam::IsCacheIoSettled(Gpu) ? TEXT("yes") : TEXT("no"), FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(Gpu),
			FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(Gpu), Seam::PostedLaneJobs(Gpu));
	}

	// --- The GPU twin (see the file header) ---------------------------------------------------------

	// Adopts Lease into a fresh pooled sequence, feeds Suffix and generates kGpuDiskTwinNewTokens; returns the tokens.
	bool GpuDiskAdopterTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq))
		{
			return false;
		}
		const bool bOk = AdoptAndWait(T, Gpu, Seq, Lease) && GenerateComplete(T, Gpu, Seq, Suffix, kGpuDiskTwinNewTokens, OutTokens);
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	bool GpuDiskEqualsTwin(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, const TArray<int32>& Twin, const TCHAR* What)
	{
		TArray<int32> Tokens;
		return GpuDiskAdopterTokens(T, Gpu, Lease, Suffix, Tokens)
			&& T.TestEqual(*FString::Printf(TEXT("%s: the adopter's tokens equal the twin's (the recomputed lease's adopter)"), What), Tokens, Twin);
	}

	// --- Disk entries ----------------------------------------------------------------------------

	// Builds S's entry so its file is written (a recompute: its write-back), or lets a hit's touch finish; with OutTwin,
	// the recomputed lease's adopter tokens after Suffix are the twin. Then re-Configures, so the memory tier is cold.
	bool GpuDiskWriteEntry(FAutomationTestBase& T, FGpuDiskRig& R, const FSuperSLMPrefixSpec& S, FString& OutPath, const TArray<int32>* Suffix = nullptr,
		TArray<int32>* OutTwin = nullptr)
	{
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		if (!GpuDiskReady(T, Gpu, S, L, TEXT("disk write")))
		{
			return false;
		}
		const bool bRecomputed = Gpu.GetPrefixSource(L) == ESuperSLMPrefixSource::Recomputed;
		bool bOk = true;
		if (OutTwin != nullptr && Suffix != nullptr)
		{
			bOk &= T.TestTrue(TEXT("the twin is a recomputed lease's adopter"), bRecomputed) && GpuDiskAdopterTokens(T, Gpu, L, *Suffix, *OutTwin);
		}
		Gpu.ReturnPrefix(L);
		const bool bSettled = bRecomputed ? GpuDiskWaitWriteBacks(Gpu, Mark) : GpuDiskWaitQuiet(Gpu);
		if (!T.TestTrue(*FString::Printf(TEXT("disk write: the write-back settles (%s; %s; write-backs %lld past the mark)"),
				bRecomputed ? TEXT("recomputed: its write-back") : TEXT("a hit: the disk going quiet"), *GpuDiskQuietReport(Gpu), GpuDiskWriteBackMark(Gpu) - Mark), bSettled))
		{
			return false;
		}
		OutPath = Seam::DiskFileOf(Gpu, S);
		return bOk && T.TestTrue(*FString::Printf(TEXT("the entry's file exists (%s)"), *OutPath), !OutPath.IsEmpty() && IFileManager::Get().FileExists(*OutPath))
			&& GpuDiskReconfigure(T, R);
	}

	// The CPU cell's MissThenRecompute: the request is served by recompute, the fallback's counter rises by one, one log
	// line names it; with bFileDeleted, the recompute's write-back replaced the bad file, so after a re-Configure() the
	// key is a Disk hit; the served lease's adopter equals the twin.
	bool GpuDiskMissThenRecompute(FAutomationTestBase& T, FGpuDiskRig& R, const FSuperSLMPrefixSpec& S, const TCHAR* Fallback, const TArray<int32>& Suffix,
		const TArray<int32>& Twin, const TCHAR* What, bool bFileDeleted = true)
	{
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		FGpuPagingLogCapture Log;
		const int64 Before = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), Fallback);
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		bool bOk = GpuDiskReady(T, Gpu, S, L, What);
		bOk &= GpuDiskExpectSource(T, Gpu, L, ESuperSLMPrefixSource::Recomputed, What);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the %s counter rose by one"), What, Fallback), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), Fallback), Before + 1);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one log line names the fallback"), What), Log.CountContaining(*FString::Printf(TEXT("PrefixCache fallback %s"), Fallback)), 1);
		if (bFileDeleted)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the recompute's write-back settles (%s)"), What, *GpuDiskQuietReport(Gpu)), GpuDiskWaitWriteBacks(Gpu, Mark));
			FSuperSLMPrefix Again;
			Gpu.ReturnPrefix(L);
			bOk &= GpuDiskReconfigure(T, R) && GpuDiskReady(T, Gpu, S, Again, What);
			bOk &= GpuDiskExpectSource(T, Gpu, Again, ESuperSLMPrefixSource::Disk, *FString::Printf(TEXT("%s: the rewritten file is a hit"), What));
			L = Again;
		}
		bOk &= GpuDiskEqualsTwin(T, Gpu, L, Suffix, Twin, What);
		Gpu.ReturnPrefix(L);
		return bOk;
	}

	// A CPU instance's wrapper (backend 0) for the same tokens: a CPU budget-mode rig with its own disk directory writes
	// the entry; its file's bytes are returned. The CPU fixture's helpers, by their own namespaces.
	bool GpuDiskCpuBackendWrapper(FAutomationTestBase& T, const TArray<int32>& Tokens, TArray<uint8>& OutBytes)
	{
		SuperSLMPrefixCacheFixtures::FScopedCacheDirectory CpuDir(TEXT("gpu-hostile-cpu-backend"));
		SuperSLMPagingFixtures::FPagingRig Cpu;
		if (!Cpu.Init(T) || !SuperSLMPagingFixtures::ConfigureLegacy(T, Cpu, SuperSLMPagingFixtures::MakeConfig(1, 0, 0, 1)) || !SuperSLMPagingFixtures::ReadGeometry(T, Cpu)
			|| !SuperSLMPrefixCacheFixtures::ConfigureCacheBudget(T, Cpu, SuperSLMPrefixCacheFixtures::WithCache(SuperSLMPagingFixtures::MakeConfig(2, 512, 1, 1),
				SuperSLMPrefixCacheFixtures::BytesForPages(Cpu.Geo, 64), CpuDir.Path, kGpuDiskMax)))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = GpuDiskSpec(Tokens);
		FSuperSLMPrefix L;
		const int64 Mark = SuperSLMPrefixCacheFixtures::WriteBackMark(*Cpu.Cpu);
		if (!SuperSLMPrefixCacheFixtures::RequestReady(T, *Cpu.Cpu, S, L, TEXT("the CPU instance's entry")))
		{
			return false;
		}
		Cpu.Cpu->ReturnPrefix(L);
		if (!T.TestTrue(TEXT("the CPU instance's write-back lands"), SuperSLMPrefixCacheFixtures::WaitWriteBacks(*Cpu.Cpu, Mark)))
		{
			return false;
		}
		const FString Path = SuperSLMPrefixCacheFixtures::DiskFileOf(*Cpu.Cpu, S);
		return T.TestTrue(*FString::Printf(TEXT("the CPU instance's file is a whole wrapper (%s)"), *Path),
			FFileHelper::LoadFileToArray(OutBytes, *Path) && OutBytes.Num() > 4 && FMemory::Memcmp(OutBytes.GetData(), "SLUP", 4) == 0);
	}

	bool GpuDiskIsWholeWrapper(const FString& Path)
	{
		TArray<uint8> Bytes;
		return FFileHelper::LoadFileToArray(Bytes, *Path) && Bytes.Num() > 4 && FMemory::Memcmp(Bytes.GetData(), "SLUP", 4) == 0;
	}
}

// Cell 1.6's GPU twin: the disk tier warm across two GPU subsystem lifetimes on the same adapter. The key carries
// backend 1 and the adapter's ids, so the second lifetime names the same file and its request is a Disk hit whose
// adopter equals the first lifetime's recomputed lease's.
// Mutant: an instance-specific value in the key (or the write-back skipped) -> the second lifetime recomputes -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskWarmTest, "SuperSLM.PrefixCache.Gpu.Disk.WarmAcrossLifetimes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskWarmTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-warm"));
	TArray<int32> Tokens, Suffix, Twin;
	FString FirstPath;
	{
		FGpuDiskRig First;
		if (!GpuDiskRigInit(*this, First, Dir.Path) || !GpuDiskTokens(*this, First, 1, Tokens) || !MakeTokens(*this, *First.Rig.Model, kGpuDiskSuffixTokens, 2, Suffix)
			|| !GpuDiskWriteEntry(*this, First, GpuDiskSpec(Tokens), FirstPath, &Suffix, &Twin))
		{
			return false;
		}
	}
	FGpuDiskRig Second;
	FSuperSLMPrefix L;
	if (!GpuDiskRigInit(*this, Second, Dir.Path))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the second lifetime on the same adapter names the same file"), Seam::DiskFileOf(Second.Gpu(), GpuDiskSpec(Tokens)), FirstPath);
	if (!GpuDiskReady(*this, Second.Gpu(), GpuDiskSpec(Tokens), L, TEXT("second lifetime")))
	{
		return false;
	}
	bOk &= GpuDiskExpectSource(*this, Second.Gpu(), L, ESuperSLMPrefixSource::Disk, TEXT("second lifetime"));
	bOk &= GpuDiskEqualsTwin(*this, Second.Gpu(), L, Suffix, Twin, TEXT("disk hit"));
	Second.Gpu().ReturnPrefix(L);
	return bOk;
#endif
}

// Cell 2.8's GPU twin: truncated wrapper, other wrapper version, other Layer-1 tag, spec mismatch under an equal key
// (ForceKeyCollision's GPU overload), zero-length file, a foreign file in the directory, a file replaced between scan
// and read: each a miss, the file deleted, one log line, served by recompute with the twin's tokens. The GPU's own two:
// this adapter's file moved under another adapter's key (DiskFileOfWithAdapter) is never read here (a recompute; the
// other key's file is left alone), and a CPU instance's wrapper (backend 0) under the GPU key is a miss by name,
// WrapperBackendMismatch, deleted and recomputed.
// Mutants: a wrapper check skipped -> served from the bad file (or a bad parse) -> red; the backend byte not compared ->
// the CPU wrapper reaches the engine -> PayloadRefused, not WrapperBackendMismatch -> red; the adapter ids left out
// of the key -> DiskFileOfWithAdapter names this adapter's own path -> its TestNotEqual is red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskHostileTest, "SuperSLM.PrefixCache.Gpu.Disk.HostileFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskHostileTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-hostile"));
	FGpuDiskRig R;
	TArray<int32> Tokens, Other, Suffix, Twin;
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Tokens) || !GpuDiskTokens(*this, R, 2, Other)
		|| !MakeTokens(*this, *R.Rig.Model, kGpuDiskSuffixTokens, 3, Suffix))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	const FSuperSLMPrefixSpec S = GpuDiskSpec(Tokens);
	bool bOk = true;
	// The twin: S's first build, adopted before its write-back.
	{
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path, &Suffix, &Twin))
		{
			return false;
		}
	}
	struct FEdit { const TCHAR* Name; ESuperSLMDiskWrapperEdit Edit; const TCHAR* Fallback; };
	for (const FEdit& E : { FEdit{ TEXT("truncated"), ESuperSLMDiskWrapperEdit::Truncate, TEXT("WrapperTruncated") },
							FEdit{ TEXT("wrapper version 2"), ESuperSLMDiskWrapperEdit::WrapperVersion2, TEXT("WrapperUnsupportedVersion") },
							FEdit{ TEXT("other Layer-1 tag"), ESuperSLMDiskWrapperEdit::OtherLayer1Tag, TEXT("WrapperLayer1TagMismatch") } })
	{
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the edit applies"), E.Name), Seam::EditDiskWrapper(Path, E.Edit));
		bOk &= GpuDiskMissThenRecompute(*this, R, S, E.Fallback, Suffix, Twin, E.Name);
	}
	// Zero-length file under the key.
	{
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("zero-length: the file is emptied"), FFileHelper::SaveArrayToFile(TArray<uint8>(), *Path));
		bOk &= GpuDiskMissThenRecompute(*this, R, S, TEXT("WrapperTruncated"), Suffix, Twin, TEXT("zero-length"));
	}
	// Spec mismatch under an equal key: Other's file served under S's key. The colliding file is deleted and the
	// recompute's write-back then lands under the same (forced) key, so the path holds S's wrapper afterwards, not
	// the colliding bytes (the CPU cell's reading).
	{
		FString OtherPath;
		if (!GpuDiskWriteEntry(*this, R, GpuDiskSpec(Other), OtherPath))
		{
			return false;
		}
		TArray<uint8> Colliding;
		FFileHelper::LoadFileToArray(Colliding, *OtherPath);
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		Seam::ForceKeyCollision(Gpu, S, GpuDiskSpec(Other)); // after the re-Configure() in GpuDiskWriteEntry, which clears it
		bOk &= GpuDiskMissThenRecompute(*this, R, S, TEXT("WrapperSpecMismatch"), Suffix, Twin, TEXT("spec mismatch"), false);
		bOk &= TestEqual(TEXT("spec mismatch: the counter reads 1 (this configuration's)"), Gpu.GetPrefixCacheStats().SpecMismatches, int64(1));
		bOk &= TestTrue(TEXT("spec mismatch: the recompute's write-back settles"), GpuDiskWaitWriteBacks(Gpu, Mark));
		TArray<uint8> After;
		const bool bPresent = FFileHelper::LoadFileToArray(After, *OtherPath);
		bOk &= TestTrue(TEXT("spec mismatch: the colliding file is deleted (the key holds the write-back, not its bytes)"), Colliding.Num() > 0 && (!bPresent || After != Colliding));
	}
	// A foreign file in the directory: ignored by the scan and the trim; nothing breaks.
	{
		const FString Foreign = FPaths::Combine(Dir.Path, TEXT("not-a-cache-file.txt"));
		FFileHelper::SaveStringToFile(TEXT("foreign"), *Foreign);
		bOk &= GpuDiskReconfigure(*this, R);
		FSuperSLMPrefix L;
		bOk &= GpuDiskReady(*this, Gpu, S, L, TEXT("foreign file present"));
		bOk &= TestTrue(TEXT("the foreign file is left alone"), IFileManager::Get().FileExists(*Foreign));
		Gpu.ReturnPrefix(L);
		bOk &= GpuDiskWaitQuiet(Gpu);
	}
	// A file replaced between scan and read: a miss, never a bad parse.
	{
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path))
		{
			return false;
		}
		Seam::SetBeforeDiskRead(Gpu, [](const FString& P) { FFileHelper::SaveStringToFile(TEXT("replaced mid-read"), *P); });
		bOk &= GpuDiskMissThenRecompute(*this, R, S, TEXT("WrapperBadMagic"), Suffix, Twin, TEXT("replaced between scan and read"));
	}
	// GPU only: this adapter's file moved under another adapter's key. This instance never reads that path (its key
	// carries this adapter's ids): S is recomputed, and the other key's file is not this instance's to delete.
	{
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path))
		{
			return false;
		}
		uint32 Vendor = 0, Device = 0;
		bOk &= TestTrue(TEXT("another adapter: the instance reports its adapter ids"), Seam::AdapterIdsOf(Gpu, Vendor, Device));
		const FString OtherAdapter = Seam::DiskFileOfWithAdapter(Gpu, S, Vendor ^ 1u, Device);
		if (!TestNotEqual(TEXT("another adapter: its key names another file"), OtherAdapter, Path)
			|| !TestTrue(TEXT("another adapter: this adapter's file now sits under that adapter's key"), IFileManager::Get().Move(*OtherAdapter, *Path)))
		{
			return false;
		}
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		bOk &= GpuDiskReady(*this, Gpu, S, L, TEXT("another adapter's file"));
		bOk &= GpuDiskExpectSource(*this, Gpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("another adapter's file is never this adapter's hit"));
		bOk &= TestTrue(TEXT("another adapter: its file is left in place"), IFileManager::Get().FileExists(*OtherAdapter));
		bOk &= TestTrue(TEXT("another adapter: the recompute's write-back settles"), GpuDiskWaitWriteBacks(Gpu, Mark));
		bOk &= GpuDiskEqualsTwin(*this, Gpu, L, Suffix, Twin, TEXT("another adapter's file"));
		Gpu.ReturnPrefix(L);
		IFileManager::Get().Delete(*OtherAdapter);
	}
	// GPU only: a CPU instance's wrapper (backend 0) for the same spec under the GPU key: WrapperBackendMismatch.
	{
		TArray<uint8> CpuWrapper;
		if (!GpuDiskCpuBackendWrapper(*this, Tokens, CpuWrapper))
		{
			return false;
		}
		FString Path;
		if (!GpuDiskWriteEntry(*this, R, S, Path))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("CPU backend: the CPU instance's wrapper is put under the GPU key"), FFileHelper::SaveArrayToFile(CpuWrapper, *Path));
		bOk &= GpuDiskMissThenRecompute(*this, R, S, TEXT("WrapperBackendMismatch"), Suffix, Twin, TEXT("a CPU-backend wrapper"));
	}
	return bOk;
#endif
}

// Cell 3.5's failed shared load, GPU twin: 16 requests in one tick for a key whose disk file is corrupt (a payload
// byte flipped: sslm_gpu_prefix_restore's digest refuses it): one restore, then one recompute, 16 Ready handles,
// all resolved within a bounded time. Then 3.5's PoolExhausted leg: on a cold memory tier, the next build's begin
// forced to SSLM_GPU_KV_POOL_EXHAUSTED (SetNextCacheBuildStatus), 16 requests for a new key in one tick: one build,
// all 16 resolve PoolExhausted by name.
// Mutant: each coalesced waiter issuing its own load or build -> RestoresIssued or BuildsIssued above 1 -> red; a
// waiter left Pending when the shared build ends PoolExhausted -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskCoalescedFailTest, "SuperSLM.PrefixCache.Gpu.Disk.CoalescedLoadFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskCoalescedFailTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-coalesced-fail"));
	FGpuDiskRig R;
	TArray<int32> Tokens;
	FString Path;
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Tokens) || !GpuDiskWriteEntry(*this, R, GpuDiskSpec(Tokens), Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	bool bOk = TestTrue(TEXT("the file is corrupted"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadByte));
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	TArray<FSuperSLMPrefix> Leases;
	for (int32 I = 0; I < 16; ++I)
	{
		FSuperSLMPrefix L;
		if (!GpuDiskRequest(*this, Gpu, GpuDiskSpec(Tokens), L, TEXT("burst")))
		{
			return false;
		}
		Leases.Add(L);
	}
	for (const FSuperSLMPrefix& L : Leases)
	{
		bOk &= GpuDiskExpectLease(*this, Gpu, L, ESuperSLMPrefixLeaseState::Ready, TEXT("burst"));
	}
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("one restore attempted"), S1.RestoresIssued - S0.RestoresIssued, int64(1));
	bOk &= TestEqual(TEXT("one recompute"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
	for (const FSuperSLMPrefix& L : Leases)
	{
		Gpu.ReturnPrefix(L);
	}
	// The PoolExhausted leg: a cold memory tier, so nothing is evictable and the one-shot ends the shared build.
	{
		bOk &= GpuDiskReconfigure(*this, R);
		TArray<int32> Fresh;
		if (!GpuDiskTokens(*this, R, 77, Fresh))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats P0 = Gpu.GetPrefixCacheStats();
		Seam::SetNextCacheBuildStatus(Gpu, static_cast<int32>(SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
		TArray<FSuperSLMPrefix> Burst;
		for (int32 I = 0; I < 16; ++I)
		{
			FSuperSLMPrefix L;
			if (!GpuDiskRequest(*this, Gpu, GpuDiskSpec(Fresh), L, TEXT("pool-exhausted burst")))
			{
				return false;
			}
			Burst.Add(L);
		}
		for (const FSuperSLMPrefix& L : Burst)
		{
			bOk &= GpuDiskExpectLease(*this, Gpu, L, ESuperSLMPrefixLeaseState::PoolExhausted, TEXT("pool-exhausted burst"));
		}
		bOk &= TestEqual(TEXT("pool-exhausted burst: one build"), Gpu.GetPrefixCacheStats().BuildsIssued - P0.BuildsIssued, int64(1));
	}
	return bOk;
#endif
}

// Cell 3.7's GPU twin: Deinitialize() with disk reads and writes in flight: no crash, no leaked handle, no file under a
// key name left partial (a temporary is removed or abandoned by name). Stronger than the CPU cell's "> 0 at the call":
// the GPU overloads of HoldDiskReads and HoldWriteBacks (both yield to teardown) park a read before it opens its file
// and a write-back with its temporary written and not renamed, and the call is made once both are parked.
// Mutant: teardown not waiting for pool-thread IO (or a write straight to the key name) -> a crash, or a key file
// that is not a whole 'SLUP' wrapper -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskDeinitTest, "SuperSLM.PrefixCache.Gpu.Disk.DeinitializeInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskDeinitTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-deinit"));
	TArray<FString> Keys;
	bool bOk = true;
	{
		FGpuDiskRig R;
		if (!GpuDiskRigInit(*this, R, Dir.Path))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		// Reads: requests for files written earlier; writes: builds whose write-backs follow.
		TArray<FSuperSLMPrefixSpec> Old;
		for (int32 I = 0; I < 3; ++I)
		{
			TArray<int32> T;
			FString Path;
			if (!GpuDiskTokens(*this, R, 10 + I, T) || !GpuDiskWriteEntry(*this, R, GpuDiskSpec(T), Path))
			{
				return false;
			}
			Old.Add(GpuDiskSpec(T));
			Keys.Add(FPaths::GetCleanFilename(Path));
		}
		// The holds are set after the last re-Configure() (which starts them disarmed). The write-backs are parked first,
		// then the reads are issued under their own hold: box 2026-10-29, with both holds armed before any request, no
		// write-back ever reached its hold within the build bound.
		Seam::HoldWriteBacks(Gpu, true);
		for (int32 I = 0; I < 3; ++I)
		{
			TArray<int32> T;
			FSuperSLMPrefix LB;
			if (!GpuDiskTokens(*this, R, 20 + I, T))
			{
				return false;
			}
			bOk &= GpuDiskRequest(*this, Gpu, GpuDiskSpec(T), LB, TEXT("a build whose write will be in flight"));
			Keys.Add(FPaths::GetCleanFilename(Seam::DiskFileOf(Gpu, GpuDiskSpec(T))));
		}
		Pump(Gpu, [&Gpu]() { return Seam::HeldWriteBackCount(Gpu) > 0; }, 5 * kGpuDiskOpSeconds);
		Seam::HoldDiskReads(Gpu, true);
		for (int32 I = 0; I < 3; ++I)
		{
			FSuperSLMPrefix LR;
			bOk &= GpuDiskRequest(*this, Gpu, Old[I], LR, TEXT("a disk read"));
		}
		Pump(Gpu, [&Gpu]() { return Seam::HeldDiskReadCount(Gpu) > 0; }, kGpuDiskOpSeconds);
		bOk &= TestTrue(*FString::Printf(TEXT("a disk read is parked in flight (%d)"), Seam::HeldDiskReadCount(Gpu)), Seam::HeldDiskReadCount(Gpu) > 0);
		bOk &= TestTrue(*FString::Printf(TEXT("a write-back is parked mid-write (%d)"), Seam::HeldWriteBackCount(Gpu)), Seam::HeldWriteBackCount(Gpu) > 0);
		bOk &= TestTrue(*FString::Printf(TEXT("disk IO is in flight at the call (> 0; %s)"), *GpuDiskQuietReport(Gpu)), Seam::DiskIoInFlight(Gpu) > 0);
		R.Rig.World.DestroyTestWorld(false); // Deinitialize()
	}
	for (const FString& Name : Dir.Files())
	{
		const FString Full = FPaths::Combine(Dir.Path, Name);
		if (Keys.Contains(Name))
		{
			bOk &= TestTrue(*FString::Printf(TEXT("a file under a key name (%s) is whole: it starts 'SLUP'"), *Name), GpuDiskIsWholeWrapper(Full));
		}
		else
		{
			bOk &= TestTrue(*FString::Printf(TEXT("any other file is a temporary, named as one (%s)"), *Name), Name.EndsWith(TEXT(".tmp")));
		}
	}
	return bOk;
#endif
}

// Cell 3.8's GPU twin: two GPU subsystems (two worlds, two game instances, one adapter) share one directory, write the
// same keys and trim at once: one valid file per key, no partial file, the directory within its limit, and a read
// racing a rename is a miss, never a bad parse. The limit is three and a half of this run's files (one probe entry
// measures a file), so the trim runs with one-page prefixes (the CPU cell's 4 MiB over 500-token files).
// Mutant: a write straight to the key name (no temporary and rename) -> a partial file or a bad parse -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskTwoInstancesTest, "SuperSLM.PrefixCache.Gpu.Disk.TwoInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskTwoInstancesTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-two-instances"));
	FGpuDiskRig A, B;
	if (!GpuDiskRigInit(*this, A, Dir.Path) || !GpuDiskRigInit(*this, B, Dir.Path))
	{
		return false;
	}
	// One file's size, from a probe entry (removed), sets the shared limit for both instances.
	int64 Limit = 0;
	{
		TArray<int32> Probe;
		FString ProbePath;
		if (!GpuDiskTokens(*this, A, 99, Probe) || !GpuDiskWriteEntry(*this, A, GpuDiskSpec(Probe), ProbePath))
		{
			return false;
		}
		const int64 OneFile = IFileManager::Get().FileSize(*ProbePath);
		IFileManager::Get().Delete(*ProbePath);
		Limit = 3 * OneFile + OneFile / 2;
		A.Settings.DiskMaxBytes = Limit;
		B.Settings.DiskMaxBytes = Limit;
		if (!TestTrue(TEXT("a file has a size"), OneFile > 0) || !GpuDiskReconfigure(*this, A) || !GpuDiskReconfigure(*this, B))
		{
			return false;
		}
	}
	USuperSLMGpuSubsystem& GA = A.Gpu();
	USuperSLMGpuSubsystem& GB = B.Gpu();
	bool bOk = true;
	TArray<TArray<int32>> Keys;
	TArray<FSuperSLMPrefix> LeasesA, LeasesB;
	const int64 MarkA = GpuDiskWriteBackMark(GA), MarkB = GpuDiskWriteBackMark(GB);
	for (int32 I = 0; I < 6; ++I)
	{
		TArray<int32> T;
		if (!GpuDiskTokens(*this, A, 30 + I, T))
		{
			return false;
		}
		Keys.Add(T);
		FSuperSLMPrefix LA, LB;
		bOk &= GpuDiskRequest(*this, GA, GpuDiskSpec(T), LA, TEXT("instance A"));
		bOk &= GpuDiskRequest(*this, GB, GpuDiskSpec(T), LB, TEXT("instance B"));
		LeasesA.Add(LA);
		LeasesB.Add(LB);
	}
	// Settled: every lease resolved, each instance's write-backs (one per entry it recomputed) issued or failed, the
	// pool-thread IO quiet and nothing in flight, for three consecutive tick pairs.
	auto Resolved = [](USuperSLMGpuSubsystem& Gpu, const TArray<FSuperSLMPrefix>& Leases, int64& OutRecomputed)
	{
		OutRecomputed = 0;
		for (const FSuperSLMPrefix& L : Leases)
		{
			if (Gpu.GetPrefixLeaseState(L) == ESuperSLMPrefixLeaseState::Pending)
			{
				return false;
			}
			OutRecomputed += Gpu.GetPrefixSource(L) == ESuperSLMPrefixSource::Recomputed ? 1 : 0;
		}
		return true;
	};
	int32 QuietTicks = 0;
	auto BothQuiet = [&]()
	{
		int64 NA = 0, NB = 0;
		const bool bDone = Resolved(GA, LeasesA, NA) && Resolved(GB, LeasesB, NB)
			&& GpuDiskWriteBackMark(GA) >= MarkA + NA && GpuDiskWriteBackMark(GB) >= MarkB + NB
			&& Seam::DiskIoInFlight(GA) == 0 && Seam::DiskIoInFlight(GB) == 0
			&& GA.GetPrefixCacheStats().InFlightBytes == 0 && GB.GetPrefixCacheStats().InFlightBytes == 0;
		QuietTicks = bDone ? QuietTicks + 1 : 0;
		return QuietTicks >= 3;
	};
	auto PacedTick = [](USuperSLMGpuSubsystem& Gpu)
	{
		if (!FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
		{
			Gpu.Tick(kGpuDiskStep);
		}
	};
	const double SettleStart = FPlatformTime::Seconds();
	while (!BothQuiet() && FPlatformTime::Seconds() - SettleStart < kGpuDiskBuildSeconds)
	{
		PacedTick(GA);
		PacedTick(GB);
		FPlatformProcess::Sleep(0.001f);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("both instances settle (%.1f s; A: %s; B: %s)"), FPlatformTime::Seconds() - SettleStart,
		*GpuDiskQuietReport(GA), *GpuDiskQuietReport(GB)), QuietTicks >= 3);
	for (const FSuperSLMPrefix& L : LeasesA) { GA.ReturnPrefix(L); }
	for (const FSuperSLMPrefix& L : LeasesB) { GB.ReturnPrefix(L); }
	bOk &= GpuDiskWaitQuiet(GA) && GpuDiskWaitQuiet(GB);
	TSet<FString> Seen;
	for (const FString& Name : Dir.Files())
	{
		bOk &= TestFalse(*FString::Printf(TEXT("no temporary left (%s)"), *Name), Name.EndsWith(TEXT(".tmp")));
		bOk &= TestTrue(*FString::Printf(TEXT("each file is whole (%s)"), *Name), GpuDiskIsWholeWrapper(FPaths::Combine(Dir.Path, Name)));
		bOk &= TestFalse(TEXT("one file per key"), Seen.Contains(Name));
		Seen.Add(Name);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("the shared directory is within its limit (%lld <= %lld)"), Dir.TotalBytes(), Limit), Dir.TotalBytes() <= Limit);
	// A read racing a rename: instance B's read meets a rename of its file mid-read; it is a miss. B is re-Configured
	// first (a cold memory tier), then the hook is set (a re-Configure() makes a new instance, without it).
	FGpuPagingLogCapture Log;
	if (!GpuDiskReconfigure(*this, B))
	{
		return false;
	}
	int32 Raced = INDEX_NONE;
	for (int32 I = 0; I < Keys.Num() && Raced == INDEX_NONE; ++I)
	{
		Raced = IFileManager::Get().FileExists(*Seam::DiskFileOf(GB, GpuDiskSpec(Keys[I]))) ? I : INDEX_NONE;
	}
	if (!TestTrue(TEXT("a key's file is left to race (the trim keeps the newest)"), Raced != INDEX_NONE))
	{
		return false;
	}
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bMoved = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Seam::SetBeforeDiskRead(GB, [bMoved](const FString& Path)
	{
		bMoved->store(FPlatformFileManager::Get().GetPlatformFile().MoveFile(*(Path + TEXT(".moving")), *Path));
	});
	FSuperSLMPrefix L;
	bOk &= GpuDiskReady(*this, GB, GpuDiskSpec(Keys[Raced]), L, TEXT("read racing a rename"));
	bOk &= TestTrue(TEXT("the rename raced the read (the file moved away before it was opened)"), bMoved->load());
	bOk &= TestNotEqual(TEXT("the raced read is a miss"), (uint8)GB.GetPrefixSource(L), (uint8)ESuperSLMPrefixSource::Disk);
	for (const TCHAR* Bad : { TEXT("WrapperTruncated"), TEXT("WrapperBadMagic"), TEXT("WrapperMalformed") })
	{
		bOk &= TestEqual(*FString::Printf(TEXT("never a bad parse (%s)"), Bad), Log.CountContaining(*FString::Printf(TEXT("PrefixCache fallback %s"), Bad)), 0);
	}
	GB.ReturnPrefix(L);
	return bOk;
#endif
}

// Cell 5.4 (and 7.9), GPU twin: each §5.9 row the GPU instance can reach, with its named result or recompute, its log
// line and its counter raised by one. The GPU rows: "blob from another producer (engine version, GPU vendor or device)"
// is two legs, another engine version (PayloadProducer) and another GPU (PayloadAdapter), each refused
// SSLM_RESTORE_PRODUCER_MISMATCH by sslm_gpu_prefix_restore, its log line naming the status. "Pool short" is the
// PoolExhausted leg (SetNextCacheBuildStatus, on a cold memory tier).
// Not here: the legacy-mode NestedRequiresBudgetMode refusal (the GPU cache is pooled only; budget mode's acceptance
// is kept); "a GPU release skipped after a terminal device loss", which is cell 5.14's (device loss) row by §5.9's
// own note.
// Mutant: any row not logged or not counted under its reason -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskEveryFallbackTest, "SuperSLM.PrefixCache.Gpu.Disk.EveryFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskEveryFallbackTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-fallbacks"));
	FGpuDiskRig R;
	TArray<int32> Tokens, Suffix, Twin;
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Tokens) || !MakeTokens(*this, *R.Rig.Model, kGpuDiskSuffixTokens, 2, Suffix))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	const FSuperSLMPrefixSpec S = GpuDiskSpec(Tokens);
	bool bOk = true;
	FString Path;

	// Key: a different spec is a plain miss, recomputed (no fallback counter: the core counts none). Its lease's
	// adopter is the twin.
	{
		const int64 Before = GpuDiskServed(Gpu.GetPrefixCacheStats(), ESuperSLMPrefixSource::Recomputed);
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		if (!GpuDiskReady(*this, Gpu, S, L, TEXT("key miss")))
		{
			return false;
		}
		bOk &= GpuDiskExpectSource(*this, Gpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("key miss"));
		bOk &= TestEqual(TEXT("key miss: served by recompute, counted once"), GpuDiskServed(Gpu.GetPrefixCacheStats(), ESuperSLMPrefixSource::Recomputed), Before + 1);
		bOk &= GpuDiskAdopterTokens(*this, Gpu, L, Suffix, Twin);
		Gpu.ReturnPrefix(L);
		bOk &= TestTrue(TEXT("key miss: its write-back settles"), GpuDiskWaitWriteBacks(Gpu, Mark));
	}
	// Wrapper rejected: 2.8's rows (one re-run for the counter).
	bOk &= GpuDiskWriteEntry(*this, R, S, Path) && TestTrue(TEXT("wrapper rejected: the file is truncated"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::Truncate))
		&& GpuDiskMissThenRecompute(*this, R, S, TEXT("WrapperTruncated"), Suffix, Twin, TEXT("wrapper rejected"));
	// Payload corrupted: the SPX1 digest (sslm_gpu_prefix_restore: SSLM_SEQUENCE_KV_BUFFER_MISMATCH) -> miss, file deleted.
	bOk &= GpuDiskWriteEntry(*this, R, S, Path) && TestTrue(TEXT("payload corrupted: a payload byte is flipped"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadByte))
		&& GpuDiskMissThenRecompute(*this, R, S, TEXT("PayloadRefused"), Suffix, Twin, TEXT("payload corrupted"));
	// Another producer (§5.9's GPU row): the SPX1 producer identity changed and the digest resealed -> the engine's
	// named status, SSLM_RESTORE_PRODUCER_MISMATCH -> miss; one log line carries the reason and the status number.
	{
		FGpuPagingLogCapture Log;
		bOk &= GpuDiskWriteEntry(*this, R, S, Path)
			&& TestTrue(TEXT("another producer: the SPX1 producer field is rewritten and the digest resealed"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadProducer))
			&& GpuDiskMissThenRecompute(*this, R, S, TEXT("PayloadRefused"), Suffix, Twin, TEXT("another producer"));
		const FString Status = FString::Printf(TEXT("status %d"), static_cast<int32>(SslmGpuStatus::SSLM_RESTORE_PRODUCER_MISMATCH));
		bOk &= TestTrue(*FString::Printf(TEXT("the producer refusal's log line names %s (SSLM_RESTORE_PRODUCER_MISMATCH)"), *Status),
			Log.CountContainingBoth(TEXT("PayloadRefused"), *Status) > 0);
	}
	// Another GPU's blob (§5.9's "GPU vendor or device"): the SPX1 producer adapter (vendor, device) changed and the
	// digest resealed (PayloadAdapter) -> SSLM_RESTORE_PRODUCER_MISMATCH -> miss, file deleted, the status in its line.
	{
		FGpuPagingLogCapture Log;
		bOk &= GpuDiskWriteEntry(*this, R, S, Path)
			&& TestTrue(TEXT("another GPU: the SPX1 adapter fields are rewritten and the digest resealed"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadAdapter))
			&& GpuDiskMissThenRecompute(*this, R, S, TEXT("PayloadRefused"), Suffix, Twin, TEXT("another GPU's blob"));
		const FString Status = FString::Printf(TEXT("status %d"), static_cast<int32>(SslmGpuStatus::SSLM_RESTORE_PRODUCER_MISMATCH));
		bOk &= TestTrue(*FString::Printf(TEXT("another GPU's blob: the log line names %s (SSLM_RESTORE_PRODUCER_MISMATCH)"), *Status),
			Log.CountContainingBoth(TEXT("PayloadRefused"), *Status) > 0);
	}
	// Disk read error: an IO failure on the read -> miss, logged (the file stays).
	if (GpuDiskWriteEntry(*this, R, S, Path))
	{
		Seam::SetDiskFault(Gpu, ESuperSLMPrefixDiskFault::FailRead);
		bOk &= GpuDiskMissThenRecompute(*this, R, S, TEXT("DiskReadError"), Suffix, Twin, TEXT("disk read error"), false);
		Seam::SetDiskFault(Gpu, ESuperSLMPrefixDiskFault::None);
		bOk &= TestTrue(TEXT("disk read error: the recompute's write-back settles"), GpuDiskWaitQuiet(Gpu));
	}
	else
	{
		bOk = false;
	}
	// A file that vanished between scan and read: a plain miss, no counter.
	if (GpuDiskWriteEntry(*this, R, S, Path))
	{
		Seam::SetBeforeDiskRead(Gpu, [](const FString& P) { IFileManager::Get().Delete(*P); });
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		bOk &= GpuDiskReady(*this, Gpu, S, L, TEXT("vanished")) && GpuDiskExpectSource(*this, Gpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("vanished"));
		// The recompute writes S back (§5.7): wait for it, so it cannot land inside the next leg's write fault.
		bOk &= TestTrue(TEXT("vanished: the recompute's write-back settles"), GpuDiskWaitWriteBacks(Gpu, Mark));
		Gpu.ReturnPrefix(L);
	}
	else
	{
		bOk = false;
	}
	// Disk write error: no write-back, logged; the request itself is served.
	{
		TArray<int32> T;
		GpuDiskTokens(*this, R, 9, T);
		FGpuPagingLogCapture Log;
		// The fault applies to every file write while it is set: nothing else may be writing when it is raised.
		bOk &= TestTrue(*FString::Printf(TEXT("disk write error: no other write-back is in flight when the fault is set (%s)"), *GpuDiskQuietReport(Gpu)), GpuDiskWaitQuiet(Gpu));
		const int64 Before = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("DiskWriteError"));
		Seam::SetDiskFault(Gpu, ESuperSLMPrefixDiskFault::FailTemporaryWrite);
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(T), L, TEXT("disk write error"));
		bOk &= TestTrue(TEXT("disk write error: the write-back ran (and failed)"), GpuDiskWaitWriteBacks(Gpu, Mark));
		Seam::SetDiskFault(Gpu, ESuperSLMPrefixDiskFault::None);
		bOk &= TestEqual(TEXT("DiskWriteError counter +1"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("DiskWriteError")), Before + 1);
		bOk &= TestEqual(TEXT("DiskWriteError logged once"), Log.CountContaining(TEXT("PrefixCache fallback DiskWriteError")), 1);
		bOk &= TestFalse(TEXT("no file under the key"), IFileManager::Get().FileExists(*Seam::DiskFileOf(Gpu, GpuDiskSpec(T))));
		Gpu.ReturnPrefix(L);
	}
	// Unknown schema, by name (the GPU build's schema lookup, before any binding call).
	{
		const int64 Before = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("UnknownSchema"));
		FSuperSLMPrefix L;
		bOk &= GpuDiskRequest(*this, Gpu, GpuDiskSpec(Tokens, FName(TEXT("no_such_schema"))), L, TEXT("unknown schema"))
			&& GpuDiskExpectLease(*this, Gpu, L, ESuperSLMPrefixLeaseState::UnknownSchema, TEXT("unknown schema"));
		bOk &= TestEqual(TEXT("UnknownSchema counter +1"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("UnknownSchema")), Before + 1);
		bOk &= TestFalse(TEXT("unknown schema: no file"), IFileManager::Get().FileExists(*Seam::DiskFileOf(Gpu, GpuDiskSpec(Tokens, FName(TEXT("no_such_schema"))))));
	}
	// A cache-owned handle to ReleasePrefix; an ended lease to RequestAdoptPrefix (refused at the call, PrefixLeaseEnded).
	{
		FSuperSLMPrefix L;
		FString Error;
		const int64 Owned = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("ReleaseOfCacheHandle"));
		const int64 Ended = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("AdoptOfEndedLease"));
		bOk &= GpuDiskReady(*this, Gpu, S, L, TEXT("handles"));
		bOk &= TestFalse(TEXT("ReleasePrefix(cache handle) refused"), Gpu.ReleasePrefix(L, Error));
		Gpu.ReturnPrefix(L);
		FSuperSLMGpuSequence Seq;
		if (Vend(*this, Gpu, Seq))
		{
			const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestAdoptPrefix(Seq, L);
			bOk &= TestFalse(TEXT("RequestAdoptPrefix(ended lease) is refused at the call"), Handle.IsValid());
			bOk &= TestTrue(*FString::Printf(TEXT("by name: PrefixLeaseEnded (%s)"), *Gpu.GetLastLifecycleRequestError()), Gpu.GetLastLifecycleRequestError().Contains(TEXT("PrefixLeaseEnded")));
			if (Handle.IsValid())
			{
				Gpu.ReleaseLifecycleOpHandle(Handle);
			}
			ReturnAndDrain(Gpu, Seq);
		}
		else
		{
			bOk = false;
		}
		bOk &= TestEqual(TEXT("ReleaseOfCacheHandle counter +1"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("ReleaseOfCacheHandle")), Owned + 1);
		bOk &= TestEqual(TEXT("AdoptOfEndedLease counter +1"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("AdoptOfEndedLease")), Ended + 1);
	}
	// NestedRequiresBudgetMode: the GPU cache is budget mode (pooled) only, so a nested spec is accepted as a nested
	// entry and the reason is never counted. The legacy-mode refusal has no GPU configuration.
	{
		FSuperSLMPrefix Parent, Child;
		FString Error;
		const int64 Before = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("NestedRequiresBudgetMode"));
		bOk &= GpuDiskReady(*this, Gpu, S, Parent, TEXT("parent"));
		FSuperSLMPrefixSpec Nested = GpuDiskSpec(Suffix);
		Nested.Parent = Parent;
		bOk &= TestTrue(*FString::Printf(TEXT("a nested spec is accepted (%s)"), *Error), Gpu.RequestPrefix(Nested, Child, Error));
		bOk &= TestEqual(TEXT("no NestedRequiresBudgetMode"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("NestedRequiresBudgetMode")), Before);
		GpuDiskWaitLease(Gpu, Child);
		Gpu.ReturnPrefix(Child);
		Gpu.ReturnPrefix(Parent);
		bOk &= GpuDiskWaitQuiet(Gpu);
	}
	// Pool short (§5.4: evict and retry, then PoolExhausted): SetNextCacheBuildStatus makes the next build's begin return
	// SSLM_GPU_KV_POOL_EXHAUSTED without calling it. On a cold memory tier (a re-Configure()) nothing is evictable, so the
	// one-shot ends the request by name (a resident entry would be evicted and the retry would build).
	{
		bOk &= GpuDiskReconfigure(*this, R);
		FGpuPagingLogCapture Log;
		const int64 Before = GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("PoolExhausted"));
		TArray<int32> T;
		FSuperSLMPrefix L;
		bOk &= GpuDiskTokens(*this, R, 77, T);
		Seam::SetNextCacheBuildStatus(Gpu, static_cast<int32>(SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
		bOk &= GpuDiskRequest(*this, Gpu, GpuDiskSpec(T), L, TEXT("pool exhausted"))
			&& GpuDiskExpectLease(*this, Gpu, L, ESuperSLMPrefixLeaseState::PoolExhausted, TEXT("pool exhausted"));
		bOk &= TestEqual(TEXT("PoolExhausted counter +1"), GpuDiskFallbacks(Gpu.GetPrefixCacheStats(), TEXT("PoolExhausted")), Before + 1);
		bOk &= TestEqual(TEXT("PoolExhausted logged once"), Log.CountContaining(TEXT("PrefixCache fallback PoolExhausted")), 1);
	}
	return bOk;
#endif
}

// Cell 5.8's GPU twin: the fault seam between the disk write and the rename (CrashBeforeRename, then FailRename): no
// file under the key name; after a re-Configure() the next request recomputes.
// Mutant: the write made in place under the key name (no temporary) -> a file under the key after the fault -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskCrashTest, "SuperSLM.PrefixCache.Gpu.Disk.CrashBeforeRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskCrashTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-crash"));
	FGpuDiskRig R;
	TArray<int32> Tokens;
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Tokens))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	bool bOk = true;
	for (const ESuperSLMPrefixDiskFault Fault : { ESuperSLMPrefixDiskFault::CrashBeforeRename, ESuperSLMPrefixDiskFault::FailRename })
	{
		Seam::SetDiskFault(Gpu, Fault);
		FSuperSLMPrefix L;
		const int64 Mark = GpuDiskWriteBackMark(Gpu);
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(Tokens), L, TEXT("faulted write"));
		Gpu.ReturnPrefix(L);
		bOk &= TestTrue(TEXT("the faulted write-back ran"), GpuDiskWaitWriteBacks(Gpu, Mark));
		Seam::SetDiskFault(Gpu, ESuperSLMPrefixDiskFault::None);
		bOk &= TestFalse(TEXT("no file under the key name"), IFileManager::Get().FileExists(*Seam::DiskFileOf(Gpu, GpuDiskSpec(Tokens))));
		FSuperSLMPrefix Next;
		bOk &= GpuDiskReconfigure(*this, R);
		const int64 NextMark = GpuDiskWriteBackMark(Gpu);
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(Tokens), Next, TEXT("next request"));
		bOk &= GpuDiskExpectSource(*this, Gpu, Next, ESuperSLMPrefixSource::Recomputed, TEXT("the next request recomputes"));
		Gpu.ReturnPrefix(Next);
		// Its (unfaulted) write-back lands before the file is removed for the next fault.
		bOk &= TestTrue(TEXT("the next request's write-back settles"), GpuDiskWaitWriteBacks(Gpu, NextMark));
		IFileManager::Get().Delete(*Seam::DiskFileOf(Gpu, GpuDiskSpec(Tokens)));
		bOk &= GpuDiskReconfigure(*this, R);
	}
	return bOk;
#endif
}

// Cell 7.14's GPU twin: write A, B and C in that order, serve a Disk hit on A, then write D past DiskMaxBytes (three
// and a half files): after the write, and after a re-Configure(), the directory is within the limit, A survives, and B
// is the first removed.
// Mutant: no time refresh on a hit -> A removed first -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskTrimTest, "SuperSLM.PrefixCache.Gpu.Disk.TrimOldestFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskTrimTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-trim"));
	FGpuDiskRig R;
	if (!GpuDiskRigInit(*this, R, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	TArray<int32> TA, TB, TC, TD;
	FString PA, PB, PC, PD;
	if (!GpuDiskTokens(*this, R, 1, TA) || !GpuDiskTokens(*this, R, 2, TB) || !GpuDiskTokens(*this, R, 3, TC) || !GpuDiskTokens(*this, R, 4, TD)
		|| !GpuDiskWriteEntry(*this, R, GpuDiskSpec(TA), PA))
	{
		return false;
	}
	const int64 OneFile = IFileManager::Get().FileSize(*PA);
	// The limit holds three files and not four.
	R.Settings.DiskMaxBytes = 3 * OneFile + OneFile / 2;
	if (!TestTrue(TEXT("a file has a size"), OneFile > 0) || !GpuDiskReconfigure(*this, R))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f); // distinct modification times on coarse file systems
	if (!GpuDiskWriteEntry(*this, R, GpuDiskSpec(TB), PB))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f);
	if (!GpuDiskWriteEntry(*this, R, GpuDiskSpec(TC), PC))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f);
	FSuperSLMPrefix HitA;
	bool bOk = GpuDiskReady(*this, Gpu, GpuDiskSpec(TA), HitA, TEXT("A")) && GpuDiskExpectSource(*this, Gpu, HitA, ESuperSLMPrefixSource::Disk, TEXT("A's disk hit"));
	Gpu.ReturnPrefix(HitA);
	bOk &= TestTrue(TEXT("A's touch settles"), GpuDiskWaitQuiet(Gpu));
	FPlatformProcess::Sleep(1.1f);
	bOk &= GpuDiskWriteEntry(*this, R, GpuDiskSpec(TD), PD);
	bOk &= TestTrue(TEXT("the trim settles"), GpuDiskWaitQuiet(Gpu));
	auto Check = [&](const TCHAR* Instant)
	{
		bool b = TestTrue(*FString::Printf(TEXT("%s: within the limit (%lld <= %lld)"), Instant, Dir.TotalBytes(), R.Settings.DiskMaxBytes), Dir.TotalBytes() <= R.Settings.DiskMaxBytes);
		b &= TestTrue(*FString::Printf(TEXT("%s: A survives (its hit refreshed it)"), Instant), IFileManager::Get().FileExists(*PA));
		b &= TestFalse(*FString::Printf(TEXT("%s: B is the first removed"), Instant), IFileManager::Get().FileExists(*PB));
		b &= TestTrue(*FString::Printf(TEXT("%s: C survives"), Instant), IFileManager::Get().FileExists(*PC));
		return b;
	};
	bOk &= Check(TEXT("after the write"));
	bOk &= GpuDiskReconfigure(*this, R);
	bOk &= TestTrue(TEXT("the trim after re-Configure() settles"), GpuDiskWaitQuiet(Gpu));
	bOk &= Check(TEXT("after re-Configure()"));
	return bOk;
#endif
}

// Cell 9.6's GPU twin: wrapper versions 0 and 2 are misses (WrapperUnsupportedVersion); a blob built under another
// Layer-1 pin is a miss (WrapperLayer1TagMismatch); each recomputed with the twin's tokens.
// Mutant: the version check accepting 0 or 2 -> a hit (or a bad parse) -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskWrapperEvolutionTest, "SuperSLM.PrefixCache.Gpu.Disk.WrapperEvolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskWrapperEvolutionTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-evolution"));
	FGpuDiskRig R;
	TArray<int32> Tokens, Suffix, Twin;
	FString TwinPath;
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Tokens) || !MakeTokens(*this, *R.Rig.Model, kGpuDiskSuffixTokens, 2, Suffix)
		|| !GpuDiskWriteEntry(*this, R, GpuDiskSpec(Tokens), TwinPath, &Suffix, &Twin))
	{
		return false;
	}
	bool bOk = true;
	for (const ESuperSLMDiskWrapperEdit Edit : { ESuperSLMDiskWrapperEdit::WrapperVersion0, ESuperSLMDiskWrapperEdit::WrapperVersion2, ESuperSLMDiskWrapperEdit::OtherLayer1Tag })
	{
		FString Path;
		bOk &= GpuDiskWriteEntry(*this, R, GpuDiskSpec(Tokens), Path) && TestTrue(TEXT("the edit applies"), Seam::EditDiskWrapper(Path, Edit))
			&& GpuDiskMissThenRecompute(*this, R, GpuDiskSpec(Tokens),
				Edit == ESuperSLMDiskWrapperEdit::OtherLayer1Tag ? TEXT("WrapperLayer1TagMismatch") : TEXT("WrapperUnsupportedVersion"), Suffix, Twin, TEXT("wrapper evolution"));
	}
	return bOk;
#endif
}

// Cell 11.4's GPU twin: the spec compare on load. With the compare removed, the forced-collision case (the GPU overload
// of ForceKeyCollision) serves the wrong prefix and this goes red; the same for the memory tier's compare, with the
// collision forced by the same seam. Each colliding request is not served from the colliding entry or file, and its
// adopter equals Mine's twin.
// Mutants: the disk tier's spec compare removed -> a Disk hit on Other's file -> red; the memory tier's -> a Memory
// hit on Other's entry -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuDiskSpecCompareTest, "SuperSLM.PrefixCache.Gpu.Disk.SpecCompare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuDiskSpecCompareTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the 1.15.0 GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
	return false;
#else
	SuperSLMPrefixCacheFixtures::FScopedCacheDirectory Dir(TEXT("gpu-spec-compare"));
	FGpuDiskRig R;
	TArray<int32> Mine, Other, Suffix, Twin;
	FString MinePath;
	// Mine's twin first (its build's adopter); GpuDiskWriteEntry re-Configures after it, so the memory leg starts cold.
	if (!GpuDiskRigInit(*this, R, Dir.Path) || !GpuDiskTokens(*this, R, 1, Mine) || !GpuDiskTokens(*this, R, 2, Other)
		|| !MakeTokens(*this, *R.Rig.Model, kGpuDiskSuffixTokens, 3, Suffix) || !GpuDiskWriteEntry(*this, R, GpuDiskSpec(Mine), MinePath, &Suffix, &Twin))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = R.Gpu();
	bool bOk = true;
	// Memory tier: Other resident; Mine forced onto Other's key.
	{
		FSuperSLMPrefix LO, LM;
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(Other), LO, TEXT("memory: other"));
		Gpu.ReturnPrefix(LO);
		Seam::ForceKeyCollision(Gpu, GpuDiskSpec(Mine), GpuDiskSpec(Other));
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(Mine), LM, TEXT("memory: mine, colliding"));
		bOk &= TestNotEqual(TEXT("memory: not served from the colliding entry"), (uint8)Gpu.GetPrefixSource(LM), (uint8)ESuperSLMPrefixSource::Memory);
		bOk &= GpuDiskEqualsTwin(*this, Gpu, LM, Suffix, Twin, TEXT("memory collision"));
		Gpu.ReturnPrefix(LM);
		bOk &= GpuDiskWaitQuiet(Gpu);
	}
	// Disk tier: Other's file under Mine's key (the collision set after GpuDiskWriteEntry's re-Configure()).
	{
		FString OtherPath;
		bOk &= GpuDiskReconfigure(*this, R) && GpuDiskWriteEntry(*this, R, GpuDiskSpec(Other), OtherPath);
		Seam::ForceKeyCollision(Gpu, GpuDiskSpec(Mine), GpuDiskSpec(Other));
		FSuperSLMPrefix LM;
		bOk &= GpuDiskReady(*this, Gpu, GpuDiskSpec(Mine), LM, TEXT("disk: mine, colliding"));
		bOk &= TestNotEqual(TEXT("disk: not served from the colliding file"), (uint8)Gpu.GetPrefixSource(LM), (uint8)ESuperSLMPrefixSource::Disk);
		bOk &= GpuDiskEqualsTwin(*this, Gpu, LM, Suffix, Twin, TEXT("disk collision"));
		Gpu.ReturnPrefix(LM);
	}
	return bOk;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
