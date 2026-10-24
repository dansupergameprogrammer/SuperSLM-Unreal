// Precache plan rev 1.17, CPU suite: the disk tier (§5.3, §5.7) and every fallback (§5.9). Preprocessed out
// unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.Disk.WarmAcrossLifetimes  cell 1.6                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.HostileFiles         cell 2.8                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.CoalescedLoadFails   cell 3.5, the failed shared load leg  [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.DeinitializeInFlight cell 3.7                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.TwoInstances         cell 3.8                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.EveryFallback        cell 5.4 (and 7.9)                    [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.CrashBeforeRename    cell 5.8                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.TrimOldestFirst      cell 7.14                             [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.WrapperEvolution     cell 9.6                              [U2]
//   SuperSLM.PrefixCache.Cpu.Disk.SpecCompare          cell 11.4 (disk and memory tiers)     [U2]
//
// The fallback names are the host-tested core's (SuperSLMCore::PrefixFallbackName): cell 5.4 asserts
// each §5.9 row's log line and its counter in FSuperSLMPrefixCacheStats::Fallbacks, keyed by that
// name. U2 logs each core log line as "PrefixCache fallback <Name>: <message>". A key miss and a
// vanished file are plain misses with no fallback counter (the core counts none); their leg asserts
// the source and the served-by-source count instead.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

#include <atomic>

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	using Seam = FSuperSLMPrefixCacheTestSeam;
	constexpr int64 kDiskMax = int64(1) << 30;

	bool DiskRig(FAutomationTestBase& T, FPagingRig& Rig, const FString& Dir, int64 DiskMax = kDiskMax)
	{
		if (!Rig.Init(T) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(T, Rig))
		{
			return false;
		}
		return ConfigureCacheBudget(T, Rig, WithCache(MakeConfig(2, 512, 1, 1), BytesForPages(Rig.Geo, 64), Dir, DiskMax));
	}

	// Builds Tokens' entry so its file is written, then re-Configures so the memory tier is cold.
	// Box 2026-10-21: this waited on DiskIoInFlight() == 0, which is true while the write-back's Save lane job
	// is still queued (the seam counts pool-thread IO only), so the re-Configure() below cancelled the
	// write-back and no file was ever written. It now waits for the write-back itself (WaitWriteBacks), when
	// the request recomputed; a Memory or Disk hit writes nothing and only lets its touch finish.
	bool WriteDiskEntry(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMPrefixSpec& S, FString& OutPath)
	{
		FSuperSLMPrefix L;
		const int64 Mark = WriteBackMark(*Rig.Cpu);
		if (!RequestReady(T, *Rig.Cpu, S, L, TEXT("disk write")))
		{
			return false;
		}
		const bool bRecomputed = Rig.Cpu->GetPrefixSource(L) == ESuperSLMPrefixSource::Recomputed;
		Rig.Cpu->ReturnPrefix(L);
		const bool bSettled = bRecomputed ? WaitWriteBacks(*Rig.Cpu, Mark) : WaitDiskQuiet(*Rig.Cpu);
		// Box 2026-10-21 run 3 failed here with nothing to say which wait: the message now names the path taken and
		// what was outstanding.
		if (!T.TestTrue(*FString::Printf(TEXT("disk write: the write-back settles (%s; %s; write-backs %lld past the mark)"),
				bRecomputed ? TEXT("recomputed: its write-back") : TEXT("a hit: the disk going quiet"), *CacheQuietReport(*Rig.Cpu),
				WriteBackMark(*Rig.Cpu) - Mark), bSettled))
		{
			return false;
		}
		OutPath = DiskFileOf(*Rig.Cpu, S);
		return T.TestTrue(*FString::Printf(TEXT("the entry's file exists (%s)"), *OutPath), IFileManager::Get().FileExists(*OutPath))
			&& ConfigureCacheBudget(T, Rig, Rig.Config);
	}

	// The request is served by recompute, its tokens equal the prefill twin's, the file is deleted,
	// and one log line names the fallback.
	bool MissThenRecompute(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMPrefixSpec& S, const FString& Path, const TCHAR* Fallback,
		const TArray<int32>& Suffix, const TArray<int32>& Twin, const TCHAR* What, bool bFileDeleted = true)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		FPagingLogCapture Log;
		const int64 Before = FallbackCount(Stats(Cpu), Fallback);
		FSuperSLMPrefix L;
		const int64 Mark = WriteBackMark(Cpu);
		bool bOk = RequestReady(T, Cpu, S, L, What);
		bOk &= ExpectSource(T, Cpu, L, ESuperSLMPrefixSource::Recomputed, What);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the %s counter rose by one"), What, Fallback), FallbackCount(Stats(Cpu), Fallback), Before + 1);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one log line names the fallback"), What), Log.CountContaining(*FString::Printf(TEXT("PrefixCache fallback %s"), Fallback)), 1);
		if (bFileDeleted)
		{
			// The recompute's write-back lands behind the bad file's delete (one key, serialized): wait for it,
			// not for DiskIoInFlight() alone (box 2026-10-21, see WriteDiskEntry).
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the recompute's write-back settles"), What), WaitWriteBacks(Cpu, Mark));
			// The bad file was deleted and the recompute wrote a fresh one under the key: after a
			// re-Configure() the key is a Disk hit on the fresh file.
			FSuperSLMPrefix Again;
			Cpu.ReturnPrefix(L);
			bOk &= ConfigureCacheBudget(T, Rig, Rig.Config) && RequestReady(T, Cpu, S, Again, What);
			bOk &= ExpectSource(T, Cpu, Again, ESuperSLMPrefixSource::Disk, *FString::Printf(TEXT("%s: the rewritten file is a hit"), What));
			L = Again;
		}
		bOk &= EqualsTwin(T, Cpu, L, Suffix, Twin, What);
		Cpu.ReturnPrefix(L);
		return bOk;
	}
}

// Cell 1.6: the disk tier warm across two subsystem lifetimes: the second's request is a Disk hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskWarmTest, "SuperSLM.PrefixCache.Cpu.Disk.WarmAcrossLifetimes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskWarmTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("warm"));
	TArray<int32> Tokens, Suffix, Twin;
	{
		FPagingRig First;
		FString Path;
		if (!DiskRig(*this, First, Dir.Path) || !MakeTokens(*this, *First.Cpu, 700, 1, Tokens) || !MakeTokens(*this, *First.Cpu, SuffixTokens, 2, Suffix)
			|| !TwinTokens(*this, *First.Cpu, Tokens, Suffix, Twin) || !WriteDiskEntry(*this, First, Spec(Tokens), Path))
		{
			return false;
		}
	}
	FPagingRig Second;
	FSuperSLMPrefix L;
	if (!DiskRig(*this, Second, Dir.Path) || !RequestReady(*this, *Second.Cpu, Spec(Tokens), L, TEXT("second lifetime")))
	{
		return false;
	}
	bool bOk = ExpectSource(*this, *Second.Cpu, L, ESuperSLMPrefixSource::Disk, TEXT("second lifetime"));
	bOk &= EqualsTwin(*this, *Second.Cpu, L, Suffix, Twin, TEXT("disk hit"));
	return bOk;
}

// Cell 2.8: truncated wrapper, other wrapper version, other Layer-1 tag, spec mismatch under an equal
// key (ForceKeyCollision), zero-length file, a foreign file in the directory, a file replaced
// between scan and read: each a miss, the file deleted, one log line, served by recompute with
// tokens equal to the prefill twin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskHostileTest, "SuperSLM.PrefixCache.Cpu.Disk.HostileFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskHostileTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("hostile"));
	FPagingRig Rig;
	TArray<int32> Tokens, Other, Suffix, Twin;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 400, 1, Tokens) || !MakeTokens(*this, *Rig.Cpu, 400, 2, Other)
		|| !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 3, Suffix) || !TwinTokens(*this, *Rig.Cpu, Tokens, Suffix, Twin))
	{
		return false;
	}
	const FSuperSLMPrefixSpec S = Spec(Tokens);
	bool bOk = true;
	struct FEdit { const TCHAR* Name; ESuperSLMDiskWrapperEdit Edit; const TCHAR* Fallback; };
	for (const FEdit& E : { FEdit{ TEXT("truncated"), ESuperSLMDiskWrapperEdit::Truncate, TEXT("WrapperTruncated") }, FEdit{ TEXT("wrapper version 2"), ESuperSLMDiskWrapperEdit::WrapperVersion2, TEXT("WrapperUnsupportedVersion") },
							FEdit{ TEXT("other Layer-1 tag"), ESuperSLMDiskWrapperEdit::OtherLayer1Tag, TEXT("WrapperLayer1TagMismatch") } })
	{
		FString Path;
		if (!WriteDiskEntry(*this, Rig, S, Path))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the edit applies"), E.Name), Seam::EditDiskWrapper(Path, E.Edit));
		bOk &= MissThenRecompute(*this, Rig, S, Path, E.Fallback, Suffix, Twin, E.Name);
	}
	// Zero-length file under the key.
	{
		FString Path;
		if (!WriteDiskEntry(*this, Rig, S, Path))
		{
			return false;
		}
		FFileHelper::SaveArrayToFile(TArray<uint8>(), *Path);
		bOk &= MissThenRecompute(*this, Rig, S, Path, TEXT("WrapperTruncated"), Suffix, Twin, TEXT("zero-length"));
	}
	// Spec mismatch under an equal key: Other's file served under Tokens' key.
	{
		FString OtherPath;
		if (!WriteDiskEntry(*this, Rig, Spec(Other), OtherPath))
		{
			return false;
		}
		// The colliding file is deleted, and the recompute's write-back then lands under the same (forced) key, so
		// the path exists again holding S's wrapper. Box 2026-10-21 second run: the cell read the path's absence,
		// which only holds before the write-back; the host twin (test_cache_tiers.cpp, cache_disk_misses_each_reason)
		// reads it as here: the bad bytes are gone, and whatever is under the key is the write-back.
		TArray<uint8> Colliding;
		FFileHelper::LoadFileToArray(Colliding, *OtherPath);
		const int64 Mark = WriteBackMark(*Rig.Cpu);
		Seam::ForceKeyCollision(*Rig.Cpu, S, Spec(Other));
		bOk &= MissThenRecompute(*this, Rig, S, OtherPath, TEXT("WrapperSpecMismatch"), Suffix, Twin, TEXT("spec mismatch"), false);
		bOk &= TestEqual(TEXT("spec mismatch: the counter reads 1"), Stats(*Rig.Cpu).SpecMismatches, int64(1));
		bOk &= TestTrue(TEXT("spec mismatch: the recompute's write-back settles"), WaitWriteBacks(*Rig.Cpu, Mark));
		TArray<uint8> After;
		const bool bPresent = FFileHelper::LoadFileToArray(After, *OtherPath);
		bOk &= TestTrue(TEXT("spec mismatch: the colliding file is deleted (the key holds the write-back, not its bytes)"), Colliding.Num() > 0 && (!bPresent || After != Colliding));
	}
	// A foreign file in the directory: ignored by the scan and the trim; nothing breaks.
	{
		const FString Foreign = FPaths::Combine(Dir.Path, TEXT("not-a-cache-file.txt"));
		FFileHelper::SaveStringToFile(TEXT("foreign"), *Foreign);
		bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config);
		FSuperSLMPrefix L;
		bOk &= RequestReady(*this, *Rig.Cpu, S, L, TEXT("foreign file present"));
		bOk &= TestTrue(TEXT("the foreign file is left alone"), IFileManager::Get().FileExists(*Foreign));
		Rig.Cpu->ReturnPrefix(L);
	}
	// A file replaced between scan and read: a miss, never a bad parse.
	{
		FString Path;
		if (!WriteDiskEntry(*this, Rig, S, Path))
		{
			return false;
		}
		Seam::SetBeforeDiskRead(*Rig.Cpu, [](const FString& P) { FFileHelper::SaveStringToFile(TEXT("replaced mid-read"), *P); });
		bOk &= MissThenRecompute(*this, Rig, S, Path, TEXT("WrapperBadMagic"), Suffix, Twin, TEXT("replaced between scan and read"));
	}
	return bOk;
}

// Cell 3.5's failed shared load: 16 requests in one tick for a key whose disk file is corrupt: one
// load, then one recompute, 16 Ready handles, all resolved within a bounded number of ticks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskCoalescedFailTest, "SuperSLM.PrefixCache.Cpu.Disk.CoalescedLoadFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskCoalescedFailTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("coalesced-fail"));
	FPagingRig Rig;
	TArray<int32> Tokens;
	FString Path;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 400, 1, Tokens) || !WriteDiskEntry(*this, Rig, Spec(Tokens), Path))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = TestTrue(TEXT("the file is corrupted"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadByte));
	const FSuperSLMPrefixCacheStats S0 = Stats(Cpu);
	TArray<FSuperSLMPrefix> Leases;
	for (int32 I = 0; I < 16; ++I)
	{
		FSuperSLMPrefix L;
		if (!Request(*this, Cpu, Spec(Tokens), L, TEXT("burst")))
		{
			return false;
		}
		Leases.Add(L);
	}
	for (const FSuperSLMPrefix& L : Leases)
	{
		bOk &= ExpectLease(*this, Cpu, L, ESuperSLMPrefixLeaseState::Ready, TEXT("burst"));
	}
	const FSuperSLMPrefixCacheStats S1 = Stats(Cpu);
	bOk &= TestEqual(TEXT("one restore attempted"), S1.RestoresIssued - S0.RestoresIssued, int64(1));
	bOk &= TestEqual(TEXT("one recompute"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
	return bOk;
}

// Cell 3.7: Deinitialize() with disk reads and writes in flight (counted > 0 at the call): no crash,
// no leaked handle, no file under a key name left partial (a temporary is removed or abandoned by
// name).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskDeinitTest, "SuperSLM.PrefixCache.Cpu.Disk.DeinitializeInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskDeinitTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("deinit"));
	TArray<FString> Keys;
	bool bOk = true;
	{
		FPagingRig Rig;
		if (!DiskRig(*this, Rig, Dir.Path))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		// Writes: several builds completing together; reads: requests for files written earlier.
		TArray<FSuperSLMPrefixSpec> Old;
		for (int32 I = 0; I < 3; ++I)
		{
			TArray<int32> T;
			FString Path;
			if (!MakeTokens(*this, Cpu, 300, 10 + I, T) || !WriteDiskEntry(*this, Rig, Spec(T), Path))
			{
				return false;
			}
			Old.Add(Spec(T));
			Keys.Add(FPaths::GetCleanFilename(Path));
		}
		for (int32 I = 0; I < 3; ++I)
		{
			TArray<int32> T;
			FSuperSLMPrefix L;
			MakeTokens(*this, Cpu, 300, 20 + I, T);
			Request(*this, Cpu, Spec(T), L, TEXT("a build whose write will be in flight"));
			Request(*this, Cpu, Old[I], L, TEXT("a disk read"));
			Keys.Add(FPaths::GetCleanFilename(DiskFileOf(Cpu, Spec(T))));
		}
		Drain(Cpu, [&Cpu]() { return Seam::DiskIoInFlight(Cpu) > 0; }, BuildSeconds);
		bOk &= TestTrue(TEXT("disk IO is in flight at the call (> 0)"), Seam::DiskIoInFlight(Cpu) > 0);
		Rig.World.DestroyTestWorld(false); // Deinitialize()
	}
	for (const FString& Name : Dir.Files())
	{
		const FString Full = FPaths::Combine(Dir.Path, Name);
		if (Keys.Contains(Name))
		{
			TArray<uint8> Bytes;
			bOk &= TestTrue(*FString::Printf(TEXT("a file under a key name (%s) is whole: it starts 'SLUP'"), *Name),
				FFileHelper::LoadFileToArray(Bytes, *Full) && Bytes.Num() > 4 && FMemory::Memcmp(Bytes.GetData(), "SLUP", 4) == 0);
		}
		else
		{
			bOk &= TestTrue(*FString::Printf(TEXT("any other file is a temporary, named as one (%s)"), *Name), Name.EndsWith(TEXT(".tmp")));
		}
	}
	return bOk;
}

// Cell 3.8: two subsystem instances in one process sharing one directory write the same key and
// trim at once: one valid file per key, no partial file, and a read racing a rename is a miss, never
// a bad parse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskTwoInstancesTest, "SuperSLM.PrefixCache.Cpu.Disk.TwoInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskTwoInstancesTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("two-instances"));
	FPagingRig A, B;
	if (!DiskRig(*this, A, Dir.Path, 4 * 1024 * 1024) || !DiskRig(*this, B, Dir.Path, 4 * 1024 * 1024))
	{
		return false;
	}
	bool bOk = true;
	TArray<TArray<int32>> Keys;
	TArray<FSuperSLMPrefix> LeasesA, LeasesB;
	const int64 MarkA = WriteBackMark(*A.Cpu), MarkB = WriteBackMark(*B.Cpu);
	for (int32 I = 0; I < 6; ++I)
	{
		TArray<int32> T;
		if (!MakeTokens(*this, *A.Cpu, 500, 30 + I, T))
		{
			return false;
		}
		Keys.Add(T);
		FSuperSLMPrefix LA, LB;
		Request(*this, *A.Cpu, Spec(T), LA, TEXT("instance A"));
		Request(*this, *B.Cpu, Spec(T), LB, TEXT("instance B"));
		LeasesA.Add(LA);
		LeasesB.Add(LB);
	}
	// Box 2026-10-21: "settled" was DiskIoInFlight() == 0 with nothing in flight, which holds while the
	// write-backs' Save jobs are still queued. Settled now means every lease resolved, and each instance's
	// write-backs (one per entry it recomputed; a Disk hit on the other's file writes nothing) issued or
	// failed, with the pool-thread IO quiet for three ticks running.
	auto Resolved = [](USuperSLMSubsystem& Cpu, const TArray<FSuperSLMPrefix>& Leases, int64& OutRecomputed)
	{
		OutRecomputed = 0;
		for (const FSuperSLMPrefix& L : Leases)
		{
			if (Cpu.GetPrefixLeaseState(L) == ESuperSLMPrefixLeaseState::Pending)
			{
				return false;
			}
			OutRecomputed += Cpu.GetPrefixSource(L) == ESuperSLMPrefixSource::Recomputed ? 1 : 0;
		}
		return true;
	};
	int32 QuietTicks = 0;
	auto BothQuiet = [&]()
	{
		int64 NA = 0, NB = 0;
		const bool bDone = Resolved(*A.Cpu, LeasesA, NA) && Resolved(*B.Cpu, LeasesB, NB)
			&& WriteBackMark(*A.Cpu) >= MarkA + NA && WriteBackMark(*B.Cpu) >= MarkB + NB
			&& Seam::DiskIoInFlight(*A.Cpu) == 0 && Seam::DiskIoInFlight(*B.Cpu) == 0
			&& A.Cpu->GetPrefixCacheStats().InFlightBytes == 0 && B.Cpu->GetPrefixCacheStats().InFlightBytes == 0;
		QuietTicks = bDone ? QuietTicks + 1 : 0;
		return QuietTicks >= 3;
	};
	// Box 2026-10-21 run 3 ("both instances settle" false): the bound was simulated time (BuildSeconds / Step =
	// 54,000 tick pairs) on an unpaced loop, which can spin through it in a few wall seconds while twelve 500-token
	// builds are still running on the lanes (run 2 happened to finish in time). The bound is now wall-clock
	// BuildSeconds, and the loop yields a millisecond to the workers between tick pairs, as DrainTicks does
	// (paging fixture's §14.7 note). The predicate is evaluated once per tick pair.
	const double SettleStart = FPlatformTime::Seconds();
	while (!BothQuiet() && FPlatformTime::Seconds() - SettleStart < BuildSeconds)
	{
		A.Cpu->Tick(Step);
		B.Cpu->Tick(Step);
		FPlatformProcess::Sleep(0.001f);
	}
	bOk &= TestTrue(*FString::Printf(TEXT("both instances settle (%.1f s; A: %s; B: %s)"), FPlatformTime::Seconds() - SettleStart,
		*CacheQuietReport(*A.Cpu), *CacheQuietReport(*B.Cpu)), QuietTicks >= 3);
	for (const FSuperSLMPrefix& L : LeasesA) { A.Cpu->ReturnPrefix(L); }
	for (const FSuperSLMPrefix& L : LeasesB) { B.Cpu->ReturnPrefix(L); }
	TSet<FString> Seen;
	for (const FString& Name : Dir.Files())
	{
		TArray<uint8> Bytes;
		bOk &= TestFalse(*FString::Printf(TEXT("no temporary left (%s)"), *Name), Name.EndsWith(TEXT(".tmp")));
		bOk &= TestTrue(*FString::Printf(TEXT("each file is whole (%s)"), *Name),
			FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Dir.Path, Name)) && Bytes.Num() > 4 && FMemory::Memcmp(Bytes.GetData(), "SLUP", 4) == 0);
		bOk &= TestFalse(TEXT("one file per key"), Seen.Contains(Name));
		Seen.Add(Name);
	}
	bOk &= TestTrue(TEXT("the shared directory is within its limit"), Dir.TotalBytes() <= 4 * 1024 * 1024);
	// A read racing a rename: instance B's read meets A's rename mid-read; it is a miss.
	// Box 2026-10-21 second run: the hook moved Keys[0]'s file with IFileManager::Move, which logs at Error when the
	// move fails ("Error moving file ... .slup"), and the directory's trim (4 MiB over six keys) may already have
	// removed the oldest key's file. The leg now races the first key whose file is still there, moves it through
	// the platform file (no log), and asserts the move happened, so the race is witnessed rather than assumed.
	int32 Raced = INDEX_NONE;
	for (int32 I = 0; I < Keys.Num() && Raced == INDEX_NONE; ++I)
	{
		Raced = IFileManager::Get().FileExists(*DiskFileOf(*B.Cpu, Spec(Keys[I]))) ? I : INDEX_NONE;
	}
	if (!TestTrue(TEXT("a key's file is left to race (the trim keeps the newest)"), Raced != INDEX_NONE))
	{
		return false;
	}
	FPagingLogCapture Log;
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bMoved = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Seam::SetBeforeDiskRead(*B.Cpu, [bMoved](const FString& Path)
	{
		bMoved->store(FPlatformFileManager::Get().GetPlatformFile().MoveFile(*(Path + TEXT(".moving")), *Path));
	});
	FSuperSLMPrefix L;
	bOk &= ConfigureCacheBudget(*this, B, B.Config) && RequestReady(*this, *B.Cpu, Spec(Keys[Raced]), L, TEXT("read racing a rename"));
	bOk &= TestTrue(TEXT("the rename raced the read (the file moved away before it was opened)"), bMoved->load());
	bOk &= TestNotEqual(TEXT("the raced read is a miss"), (uint8)B.Cpu->GetPrefixSource(L), (uint8)ESuperSLMPrefixSource::Disk);
	for (const TCHAR* Bad : { TEXT("WrapperTruncated"), TEXT("WrapperBadMagic"), TEXT("WrapperMalformed") })
	{
		bOk &= TestEqual(*FString::Printf(TEXT("never a bad parse (%s)"), Bad), Log.CountContaining(*FString::Printf(TEXT("PrefixCache fallback %s"), Bad)), 0);
	}
	return bOk;
}

// Cell 5.4 (and 7.9): each §5.9 row exercised: the named result or recompute, the log line, the
// counter for that reason raised by one. Rows owned by other cells are re-run here only for their
// counters: 2.8 (wrapper), 2.10 (handles), 2.11 (schema), 5.5 (budget, pool).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskEveryFallbackTest, "SuperSLM.PrefixCache.Cpu.Disk.EveryFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskEveryFallbackTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("fallbacks"));
	FPagingRig Rig;
	TArray<int32> Tokens, Suffix, Twin;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 400, 1, Tokens) || !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 2, Suffix)
		|| !TwinTokens(*this, *Rig.Cpu, Tokens, Suffix, Twin))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FSuperSLMPrefixSpec S = Spec(Tokens);
	bool bOk = true;
	FString Path;

	// Key: a different spec is a plain miss, recomputed (no fallback counter: the core counts none).
	{
		const int64 Before = Served(Stats(Cpu), ESuperSLMPrefixSource::Recomputed);
		FSuperSLMPrefix L;
		bOk &= RequestReady(*this, Cpu, S, L, TEXT("key miss"));
		bOk &= ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("key miss"));
		bOk &= TestEqual(TEXT("key miss: served by recompute, counted once"), Served(Stats(Cpu), ESuperSLMPrefixSource::Recomputed), Before + 1);
		Cpu.ReturnPrefix(L);
	}
	// Wrapper rejected: 2.8's rows (one re-run for the counter).
	bOk &= WriteDiskEntry(*this, Rig, S, Path) && TestTrue(TEXT("wrapper rejected: the file is truncated"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::Truncate))
		&& MissThenRecompute(*this, Rig, S, Path, TEXT("WrapperTruncated"), Suffix, Twin, TEXT("wrapper rejected"));
	// Payload corrupted: the SPX1 digest (SSLM_INVALID_ARGUMENT) -> miss, file deleted.
	bOk &= WriteDiskEntry(*this, Rig, S, Path) && TestTrue(TEXT("payload corrupted: a payload byte is flipped"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadByte))
		&& MissThenRecompute(*this, Rig, S, Path, TEXT("PayloadRefused"), Suffix, Twin, TEXT("payload corrupted"));
	// Another producer: the engine's named status (SSLM_RESTORE_PRODUCER_MISMATCH, 30) -> miss; the log
	// line carries the status number.
	{
		// Box 2026-10-21 second run: the edit seam returned false for this case, which failed the cell with no
		// message (and "30" matched elsewhere in the log). Each step now asserts by name.
		FPagingLogCapture Log;
		bOk &= WriteDiskEntry(*this, Rig, S, Path)
			&& TestTrue(TEXT("another producer: the SPX1 producer field is rewritten and the digest resealed"), Seam::EditDiskWrapper(Path, ESuperSLMDiskWrapperEdit::PayloadProducer))
			&& MissThenRecompute(*this, Rig, S, Path, TEXT("PayloadRefused"), Suffix, Twin, TEXT("another producer"));
		// One line carries both (code review r2, N5): "PrefixCache fallback PayloadRefused: ..., status 30".
		bOk &= TestTrue(TEXT("the producer refusal's log line names status 30 (SSLM_RESTORE_PRODUCER_MISMATCH)"), Log.CountLinesContainingAll({ TEXT("PayloadRefused"), TEXT("status 30") }) > 0);
	}
	// Disk read error: an IO failure on the read -> miss, logged.
	if (WriteDiskEntry(*this, Rig, S, Path))
	{
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::FailRead);
		bOk &= MissThenRecompute(*this, Rig, S, Path, TEXT("DiskReadError"), Suffix, Twin, TEXT("disk read error"), false);
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::None);
	}
	// A file that vanished between scan and read: a plain miss, no counter.
	if (WriteDiskEntry(*this, Rig, S, Path))
	{
		Seam::SetBeforeDiskRead(Cpu, [](const FString& P) { IFileManager::Get().Delete(*P); });
		FSuperSLMPrefix L;
		const int64 Mark = WriteBackMark(Cpu);
		bOk &= RequestReady(*this, Cpu, S, L, TEXT("vanished")) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("vanished"));
		// The recompute writes S back (§5.7). Box 2026-10-21 runs 3 and 4: the leg did not wait for that write-back,
		// so it landed inside the next leg's FailTemporaryWrite window (the fault is a standing switch, not one-shot)
		// and failed too: DiskWriteError +2, two lines, for S and for the next leg's key. Wait for it here.
		bOk &= TestTrue(TEXT("vanished: the recompute's write-back settles"), WaitWriteBacks(Cpu, Mark));
		Cpu.ReturnPrefix(L);
	}
	// Disk write error: no write-back, logged; the request itself is served.
	{
		TArray<int32> T;
		MakeTokens(*this, Cpu, 300, 9, T);
		FPagingLogCapture Log;
		// The fault applies to every file write while it is set: nothing else may be writing when it is raised.
		bOk &= TestTrue(*FString::Printf(TEXT("disk write error: no other write-back is in flight when the fault is set (%s)"), *CacheQuietReport(Cpu)), WaitDiskQuiet(Cpu));
		const int64 Before = FallbackCount(Stats(Cpu), TEXT("DiskWriteError"));
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::FailTemporaryWrite);
		FSuperSLMPrefix L;
		const int64 Mark = WriteBackMark(Cpu);
		bOk &= RequestReady(*this, Cpu, Spec(T), L, TEXT("disk write error"));
		bOk &= TestTrue(TEXT("disk write error: the write-back ran (and failed)"), WaitWriteBacks(Cpu, Mark));
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::None);
		bOk &= TestEqual(TEXT("DiskWriteError counter +1"), FallbackCount(Stats(Cpu), TEXT("DiskWriteError")), Before + 1);
		bOk &= TestEqual(TEXT("DiskWriteError logged once"), Log.CountContaining(TEXT("PrefixCache fallback DiskWriteError")), 1);
		bOk &= TestFalse(TEXT("no file under the key"), IFileManager::Get().FileExists(*DiskFileOf(Cpu, Spec(T))));
		Cpu.ReturnPrefix(L);
	}
	// Unknown schema, by name.
	{
		const int64 Before = FallbackCount(Stats(Cpu), TEXT("UnknownSchema"));
		FSuperSLMPrefix L;
		bOk &= Request(*this, Cpu, Spec(Tokens, FName(TEXT("no_such_schema"))), L, TEXT("unknown schema"))
			&& ExpectLease(*this, Cpu, L, ESuperSLMPrefixLeaseState::UnknownSchema, TEXT("unknown schema"));
		bOk &= TestEqual(TEXT("UnknownSchema counter +1"), FallbackCount(Stats(Cpu), TEXT("UnknownSchema")), Before + 1);
	}
	// A cache-owned handle to ReleasePrefix; an ended lease to AdoptPrefix.
	{
		FSuperSLMPrefix L;
		FString Error;
		const int64 Owned = FallbackCount(Stats(Cpu), TEXT("ReleaseOfCacheHandle"));
		const int64 Ended = FallbackCount(Stats(Cpu), TEXT("AdoptOfEndedLease"));
		bOk &= RequestReady(*this, Cpu, S, L, TEXT("handles"));
		bOk &= TestFalse(TEXT("ReleasePrefix(cache handle) refused"), Cpu.ReleasePrefix(L, Error));
		Cpu.ReturnPrefix(L);
		FSuperSLMSequence Seq;
		FSuperSLMLifecycleOpHandle Op;
		bOk &= Vend(*this, Cpu, Seq);
		bOk &= TestEqual(TEXT("AdoptPrefix(ended lease)"), (uint8)Cpu.AdoptPrefix(Seq, L, Op, Error), (uint8)ESuperSLMRestoreResult::PrefixLeaseEnded);
		Cpu.ReturnSequence(Seq);
		bOk &= TestEqual(TEXT("ReleaseOfCacheHandle counter +1"), FallbackCount(Stats(Cpu), TEXT("ReleaseOfCacheHandle")), Owned + 1);
		bOk &= TestEqual(TEXT("AdoptOfEndedLease counter +1"), FallbackCount(Stats(Cpu), TEXT("AdoptOfEndedLease")), Ended + 1);
	}
	// NestedRequiresBudgetMode. The row (§5.9; cells 5.12 and 1.x) reads "a nested spec before U6"; U6 lifted it for
	// budget mode, where a nested spec is a nested entry (§5.4; FPrefixCacheConfig::bNestedEntries = budget mode, the
	// Cpu.Nested cells), and kept it for legacy mode (Nested.Refusals). Box 2026-10-21 run 5: this leg was written
	// before U6 and ran on DiskRig's budget configuration, where the nested spec is (correctly) accepted. The
	// refusal and its counter are now read in legacy mode, and budget mode's acceptance is asserted beside it.
	{
		// Budget mode (this rig): a nested spec is accepted as a nested entry.
		FSuperSLMPrefix Parent, Child;
		FString Error;
		const int64 BeforeBudget = FallbackCount(Stats(Cpu), TEXT("NestedRequiresBudgetMode"));
		bOk &= RequestReady(*this, Cpu, S, Parent, TEXT("parent (budget mode)"));
		FSuperSLMPrefixSpec Nested = Spec(Suffix);
		Nested.Parent = Parent;
		bOk &= TestTrue(*FString::Printf(TEXT("budget mode: a nested spec is accepted (%s)"), *Error), Cpu.RequestPrefix(Nested, Child, Error));
		bOk &= TestEqual(TEXT("budget mode: no NestedRequiresBudgetMode"), FallbackCount(Stats(Cpu), TEXT("NestedRequiresBudgetMode")), BeforeBudget);
		WaitLease(Cpu, Child);
		Cpu.ReturnPrefix(Child);
		Cpu.ReturnPrefix(Parent);
		bOk &= WaitDiskQuiet(Cpu);

		// Legacy mode, same directory: refused NestedRequiresBudgetMode at the call, by name, counted once.
		const FSuperSLMRuntimeConfig BudgetConfig = Rig.Config;
		if (!ConfigureCacheLegacy(*this, Rig, WithCache(MakeConfig(2, 0, 0, 1), BytesForPages(Rig.Geo, 64), Dir.Path, kDiskMax)))
		{
			return false;
		}
		// A parent with no file on disk (a legacy build, not a restore of the budget configuration's save).
		FSuperSLMPrefix LegacyParent, LegacyChild;
		TArray<int32> LegacyTokens;
		Error.Reset();
		const int64 Before = FallbackCount(Stats(Cpu), TEXT("NestedRequiresBudgetMode"));
		bOk &= MakeTokens(*this, Cpu, 300, 91, LegacyTokens) && RequestReady(*this, Cpu, Spec(LegacyTokens), LegacyParent, TEXT("parent (legacy mode)"));
		FSuperSLMPrefixSpec LegacyNested = Spec(Suffix);
		LegacyNested.Parent = LegacyParent;
		bOk &= TestFalse(TEXT("legacy mode: a nested spec is refused"), Cpu.RequestPrefix(LegacyNested, LegacyChild, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("legacy mode: by name (%s)"), *Error), Error.Contains(TEXT("NestedRequiresBudgetMode")));
		bOk &= TestEqual(TEXT("NestedRequiresBudgetMode counter +1"), FallbackCount(Stats(Cpu), TEXT("NestedRequiresBudgetMode")), Before + 1);
		Cpu.ReturnPrefix(LegacyParent);
		bOk &= WaitDiskQuiet(Cpu);
		// Back to the cell's budget configuration for the next leg.
		if (!ConfigureCacheBudget(*this, Rig, BudgetConfig))
		{
			return false;
		}
	}
	// BudgetExhausted and PoolExhausted: their counters (the cells are 5.5 and 8.14). Box 2026-10-21: the
	// override is one-shot per kind (setting it twice arms it once), and §5.4's evict-and-retry consumes it on
	// the first begin and builds on the retry while anything is evictable. PoolExhausted by name is "nothing
	// evictable", so the leg runs on a cold memory tier (a re-Configure()), where the one override ends it.
	{
		bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config);
		const int64 Before = FallbackCount(Stats(Cpu), TEXT("PoolExhausted"));
		TArray<int32> T;
		FSuperSLMPrefix L;
		MakeTokens(*this, Cpu, 200, 77, T);
		FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(Cpu, ESuperSLMPagedRequestKind::PrefixBuild, SSLM_KV_POOL_EXHAUSTED);
		bOk &= Request(*this, Cpu, Spec(T), L, TEXT("pool exhausted")) && ExpectLease(*this, Cpu, L, ESuperSLMPrefixLeaseState::PoolExhausted, TEXT("pool exhausted"));
		bOk &= TestEqual(TEXT("PoolExhausted counter +1"), FallbackCount(Stats(Cpu), TEXT("PoolExhausted")), Before + 1);
	}
	return bOk;
}

// Cell 5.8: the fault seam between the disk write and the rename: no file under the key name; the
// next request recomputes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskCrashTest, "SuperSLM.PrefixCache.Cpu.Disk.CrashBeforeRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskCrashTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("crash"));
	FPagingRig Rig;
	TArray<int32> Tokens;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 300, 1, Tokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	for (const ESuperSLMPrefixDiskFault Fault : { ESuperSLMPrefixDiskFault::CrashBeforeRename, ESuperSLMPrefixDiskFault::FailRename })
	{
		Seam::SetDiskFault(Cpu, Fault);
		FSuperSLMPrefix L;
		const int64 Mark = WriteBackMark(Cpu);
		bOk &= RequestReady(*this, Cpu, Spec(Tokens), L, TEXT("faulted write"));
		Cpu.ReturnPrefix(L);
		// Box 2026-10-21: wait for the faulted write-back itself; DiskIoInFlight() == 0 held while its Save job
		// was queued, so "no file" passed before the write had run at all.
		bOk &= TestTrue(TEXT("the faulted write-back ran"), WaitWriteBacks(Cpu, Mark));
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::None);
		bOk &= TestFalse(TEXT("no file under the key name"), IFileManager::Get().FileExists(*DiskFileOf(Cpu, Spec(Tokens))));
		FSuperSLMPrefix Next;
		bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config);
		const int64 NextMark = WriteBackMark(Cpu);
		bOk &= RequestReady(*this, Cpu, Spec(Tokens), Next, TEXT("next request"));
		bOk &= ExpectSource(*this, Cpu, Next, ESuperSLMPrefixSource::Recomputed, TEXT("the next request recomputes"));
		Cpu.ReturnPrefix(Next);
		// Its (unfaulted) write-back lands before the file is removed for the next fault.
		bOk &= TestTrue(TEXT("the next request's write-back settles"), WaitWriteBacks(Cpu, NextMark));
		IFileManager::Get().Delete(*DiskFileOf(Cpu, Spec(Tokens)));
		bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config);
	}
	return bOk;
}

// Cell 7.14: write A, B and C in that order, serve a Disk hit on A, then write past
// PrefixDiskCacheMaxBytes: after the write, and after a re-Configure(), the directory is within the
// limit, A survives, and B is the first removed.
// Mutant: no time refresh on a hit -> A removed first -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskTrimTest, "SuperSLM.PrefixCache.Cpu.Disk.TrimOldestFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskTrimTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("trim"));
	FPagingRig Rig;
	if (!DiskRig(*this, Rig, Dir.Path))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> TA, TB, TC, TD;
	FString PA, PB, PC, PD;
	if (!MakeTokens(*this, Cpu, 300, 1, TA) || !MakeTokens(*this, Cpu, 300, 2, TB) || !MakeTokens(*this, Cpu, 300, 3, TC) || !MakeTokens(*this, Cpu, 300, 4, TD)
		|| !WriteDiskEntry(*this, Rig, Spec(TA), PA))
	{
		return false;
	}
	const int64 OneFile = IFileManager::Get().FileSize(*PA);
	// The limit holds three files and not four.
	if (!ConfigureCacheBudget(*this, Rig, WithCache(Rig.Config, Rig.Config.PrefixCacheBudgetBytes, Dir.Path, 3 * OneFile + OneFile / 2)))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f); // distinct modification times on coarse file systems
	if (!WriteDiskEntry(*this, Rig, Spec(TB), PB))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f);
	if (!WriteDiskEntry(*this, Rig, Spec(TC), PC))
	{
		return false;
	}
	FPlatformProcess::Sleep(1.1f);
	FSuperSLMPrefix HitA;
	bool bOk = RequestReady(*this, Cpu, Spec(TA), HitA, TEXT("A")) && ExpectSource(*this, Cpu, HitA, ESuperSLMPrefixSource::Disk, TEXT("A's disk hit"));
	Cpu.ReturnPrefix(HitA);
	bOk &= TestTrue(TEXT("A's touch settles"), WaitDiskQuiet(Cpu));
	FPlatformProcess::Sleep(1.1f);
	bOk &= WriteDiskEntry(*this, Rig, Spec(TD), PD);
	bOk &= TestTrue(TEXT("the trim settles"), WaitDiskQuiet(Cpu));
	auto Check = [&](const TCHAR* Instant)
	{
		bool b = TestTrue(*FString::Printf(TEXT("%s: within the limit"), Instant), Dir.TotalBytes() <= Rig.Config.PrefixDiskCacheMaxBytes);
		b &= TestTrue(*FString::Printf(TEXT("%s: A survives (its hit refreshed it)"), Instant), IFileManager::Get().FileExists(*PA));
		b &= TestFalse(*FString::Printf(TEXT("%s: B is the first removed"), Instant), IFileManager::Get().FileExists(*PB));
		b &= TestTrue(*FString::Printf(TEXT("%s: C survives"), Instant), IFileManager::Get().FileExists(*PC));
		return b;
	};
	bOk &= Check(TEXT("after the write"));
	bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config);
	bOk &= TestTrue(TEXT("the trim after re-Configure() settles"), WaitDiskQuiet(Cpu));
	bOk &= Check(TEXT("after re-Configure()"));
	return bOk;
}

// Cell 9.6: wrapper versions 0 and 2 are misses; a blob built under another Layer-1 pin is a miss.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuDiskWrapperEvolutionTest, "SuperSLM.PrefixCache.Cpu.Disk.WrapperEvolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuDiskWrapperEvolutionTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("evolution"));
	FPagingRig Rig;
	TArray<int32> Tokens, Suffix, Twin;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 300, 1, Tokens) || !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 2, Suffix)
		|| !TwinTokens(*this, *Rig.Cpu, Tokens, Suffix, Twin))
	{
		return false;
	}
	bool bOk = true;
	for (const ESuperSLMDiskWrapperEdit Edit : { ESuperSLMDiskWrapperEdit::WrapperVersion0, ESuperSLMDiskWrapperEdit::WrapperVersion2, ESuperSLMDiskWrapperEdit::OtherLayer1Tag })
	{
		FString Path;
		bOk &= WriteDiskEntry(*this, Rig, Spec(Tokens), Path) && Seam::EditDiskWrapper(Path, Edit)
			&& MissThenRecompute(*this, Rig, Spec(Tokens), Path,
				Edit == ESuperSLMDiskWrapperEdit::OtherLayer1Tag ? TEXT("WrapperLayer1TagMismatch") : TEXT("WrapperUnsupportedVersion"), Suffix, Twin, TEXT("wrapper evolution"));
	}
	return bOk;
}

// Cell 11.4: the spec compare on load. With the compare removed, 2.8's forced-collision case
// restores the wrong prefix and this goes red; the same for the memory tier's compare, with the
// collision forced by the same seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuSpecCompareTest, "SuperSLM.PrefixCache.Cpu.Disk.SpecCompare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuSpecCompareTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("spec-compare"));
	FPagingRig Rig;
	TArray<int32> Mine, Other, Suffix, Twin;
	if (!DiskRig(*this, Rig, Dir.Path) || !MakeTokens(*this, *Rig.Cpu, 300, 1, Mine) || !MakeTokens(*this, *Rig.Cpu, 300, 2, Other)
		|| !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 3, Suffix) || !TwinTokens(*this, *Rig.Cpu, Mine, Suffix, Twin))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;
	// Memory tier: Other resident; Mine forced onto Other's key.
	{
		FSuperSLMPrefix LO, LM;
		bOk &= RequestReady(*this, Cpu, Spec(Other), LO, TEXT("memory: other"));
		Cpu.ReturnPrefix(LO);
		Seam::ForceKeyCollision(Cpu, Spec(Mine), Spec(Other));
		bOk &= RequestReady(*this, Cpu, Spec(Mine), LM, TEXT("memory: mine, colliding"));
		bOk &= TestNotEqual(TEXT("memory: not served from the colliding entry"), (uint8)Cpu.GetPrefixSource(LM), (uint8)ESuperSLMPrefixSource::Memory);
		bOk &= EqualsTwin(*this, Cpu, LM, Suffix, Twin, TEXT("memory collision"));
		Cpu.ReturnPrefix(LM);
	}
	// Disk tier: Other's file under Mine's key.
	{
		FString OtherPath;
		bOk &= ConfigureCacheBudget(*this, Rig, Rig.Config) && WriteDiskEntry(*this, Rig, Spec(Other), OtherPath);
		Seam::ForceKeyCollision(Cpu, Spec(Mine), Spec(Other));
		FSuperSLMPrefix LM;
		bOk &= RequestReady(*this, Cpu, Spec(Mine), LM, TEXT("disk: mine, colliding"));
		bOk &= TestNotEqual(TEXT("disk: not served from the colliding file"), (uint8)Cpu.GetPrefixSource(LM), (uint8)ESuperSLMPrefixSource::Disk);
		bOk &= EqualsTwin(*this, Cpu, LM, Suffix, Twin, TEXT("disk collision"));
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
