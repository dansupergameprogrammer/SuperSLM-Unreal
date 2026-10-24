#pragma once

// The prefix cache's CPU suite's shared fixture. Compiled only when SUPERSLM_WITH_PREFIX_CACHE is on.
//
// It builds on the paging fixture (SuperSLMPagingFixtures.h) and keeps its rules:
//   - Expected figures are the plan's formulas written here (region C, R(L), the budget's pages),
//     never read from the product. CacheRegionPages() is the function under test in cell 8.14 and
//     is read for messages only.
//   - Page-count verdicts use paging's fill probe (ProbeAdmits / ProbeFree / Form1ChargesCoverDrawn),
//     run behind FSuperSLMPagedTestSeam::RunOnPoolThread, which parks the lanes between jobs. The
//     cache's own diagnostics are read for the counts the cells name (evictions, builds, restores,
//     the spec-mismatch counter), never as a page verdict.
//   - "The prefill twin": the same tokens built by CreatePrefix() and adopted, then generated with
//     the same suffix. A cache lease's adopter must produce the twin's tokens exactly.

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "SuperSLMPrefixCacheTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SuperSLMPrefixCacheTestSeam.h"

namespace SuperSLMPrefixCacheFixtures
{
	using namespace SuperSLMPagingFixtures;
	using ERegion = ESuperSLMPagedRegion;

	constexpr double LeaseSeconds = BuildSeconds; // a cache request resolving: a build, a restore or a load
	constexpr int32 SuffixTokens = 4;
	constexpr int32 TwinNewTokens = 8;

	// --- Configuration -----------------------------------------------------------------------

	// A fresh directory per test under the project's Saved folder, removed by the guard. Several
	// subsystems in one process may share it (cell 3.8).
	struct FScopedCacheDirectory
	{
		FString Path;
		explicit FScopedCacheDirectory(const TCHAR* Cell)
		{
			Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SuperSLMPrefixCacheTests"), FString::Printf(TEXT("%s-%s"), Cell, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Path, true);
		}
		~FScopedCacheDirectory() { IFileManager::Get().DeleteDirectory(*Path, false, true); }

		TArray<FString> Files() const
		{
			TArray<FString> Out;
			IFileManager::Get().FindFiles(Out, *FPaths::Combine(Path, TEXT("*")), true, false);
			Out.Sort();
			return Out;
		}
		int64 TotalBytes() const
		{
			int64 Total = 0;
			for (const FString& Name : Files())
			{
				Total += FMath::Max<int64>(IFileManager::Get().FileSize(*FPaths::Combine(Path, Name)), 0);
			}
			return Total;
		}
	};

	// Adds the cache's fields to a paging configuration. BudgetBytes 0 turns the cache off.
	inline FSuperSLMRuntimeConfig WithCache(FSuperSLMRuntimeConfig C, int64 BudgetBytes, const FString& DiskDirectory = FString(), int64 DiskMaxBytes = 0)
	{
		C.PrefixCacheBudgetBytes = BudgetBytes;
		C.bPrefixDiskCacheEnabled = !DiskDirectory.IsEmpty();
		C.PrefixDiskCacheDirectory = DiskDirectory;
		C.PrefixDiskCacheMaxBytes = DiskMaxBytes;
		return C;
	}

	// §5.11 (b): region C = ceil(PrefixCacheBudgetBytes / block bytes) x P_blk, a block being
	// paging's P/A block of P_blk = Block() pages. Written here from the plan, never read.
	inline int64 ExpectedRegionC(const FCpuPagingGeometry& G, int64 BudgetBytes)
	{
		const int64 BlockBytes = G.Block() * G.PageBytes;
		return FCpuPagingGeometry::CeilDiv(BudgetBytes, BlockBytes) * G.Block();
	}

	// The budget's pages, as cells 1.4 and 8.10 subtract them: floor(budget / page size).
	inline int64 BudgetPages(const FCpuPagingGeometry& G, int64 BudgetBytes) { return BudgetBytes / G.PageBytes; }

	// A cache budget of exactly Pages pages' worth of bytes (legacy mode charges bytes).
	inline int64 BytesForPages(const FCpuPagingGeometry& G, int64 Pages) { return Pages * G.PageBytes; }

	// Budget mode with the cache on. Paging's ConfigureBudget() asserts A and C are 0, which stays
	// true for a configuration with the cache off; this is its cache-on twin: S, P and H as paging
	// sizes them, C as §5.11 (b) sizes it (computed here), and the pool S + P + H + A + C.
	inline bool ConfigureCacheBudget(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMRuntimeConfig& C)
	{
		check(C.SequenceBudget > 0 && C.PrefixCacheBudgetBytes > 0);
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, C);
		if (!T.TestEqual(*FString::Printf(TEXT("budget-mode Configure() with the cache succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = C;
		if (!T.TestTrue(TEXT("budget mode (GetKvPageStats().bPagedMode)"), Rig.Cpu->GetKvPageStats().bPagedMode) || !ReadGeometry(T, Rig))
		{
			return false;
		}
		const FRegions Want = ExpectedRegions(Rig.Geo, C);
		const int64 WantC = ExpectedRegionC(Rig.Geo, C.PrefixCacheBudgetBytes);
		const FSuperSLMPagedLedgerView V = LedgerOf(*Rig.Cpu);
		bool bOk = T.TestEqual(TEXT("region S = BlockCount x R(SequenceBudget)"), V.CapacityOf(ERegion::S), Want.S);
		bOk &= T.TestEqual(TEXT("region P = PrefixBlockCount x block"), V.CapacityOf(ERegion::P), Want.P);
		bOk &= T.TestEqual(TEXT("region H = RestoreHeadroomBlocks x block"), V.CapacityOf(ERegion::H), Want.H);
		bOk &= T.TestEqual(*FString::Printf(TEXT("region C = ceil(budget / block bytes) x P_blk, computed in the test (CacheRegionPages() reads %lld)"),
			FSuperSLMPrefixCacheTestSeam::CacheRegionPages(*Rig.Cpu)), V.CapacityOf(ERegion::C), WantC);
		bOk &= T.TestEqual(TEXT("the pool is S + P + H + A + C"), V.PoolPages, Want.Pool() + V.CapacityOf(ERegion::A) + WantC);
		bOk &= T.TestTrue(TEXT("the cache reports budget mode"), Rig.Cpu->GetPrefixCacheStats().bBudgetMode);
		return bOk;
	}

	inline bool ConfigureCacheLegacy(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMRuntimeConfig& C)
	{
		check(C.SequenceBudget == 0 && C.PrefixCacheBudgetBytes > 0);
		return ConfigureLegacy(T, Rig, C) && ReadGeometry(T, Rig)
			&& T.TestFalse(TEXT("the cache reports legacy mode"), Rig.Cpu->GetPrefixCacheStats().bBudgetMode);
	}

	// --- Requests ------------------------------------------------------------------------------

	inline FSuperSLMPrefixSpec Spec(const TArray<int32>& Tokens, FName Schema = NAME_None)
	{
		FSuperSLMPrefixSpec S;
		S.Tokens = Tokens;
		S.SchemaName = Schema;
		return S;
	}

	inline bool Request(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Cpu.RequestPrefix(S, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: RequestPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: a handle is minted"), What), Out.IsValid());
	}

	// Ticks until the lease leaves Pending; returns the state it reached (Pending on timeout).
	inline ESuperSLMPrefixLeaseState WaitLease(USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease, double Seconds = LeaseSeconds)
	{
		Drain(Cpu, [&Cpu, &Lease]() { return Cpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, Seconds);
		return Cpu.GetPrefixLeaseState(Lease);
	}

	inline bool ExpectLease(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixLeaseState Expected, const TCHAR* What,
		double Seconds = LeaseSeconds)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: the lease resolves by name"), What), (uint8)WaitLease(Cpu, Lease, Seconds), (uint8)Expected);
	}

	inline bool RequestReady(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		return Request(T, Cpu, S, Out, What) && ExpectLease(T, Cpu, Out, ESuperSLMPrefixLeaseState::Ready, What);
	}

	inline bool ExpectSource(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixSource Expected, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: GetPrefixSource()"), What), (uint8)Cpu.GetPrefixSource(Lease), (uint8)Expected);
	}

	inline FSuperSLMPrefixCacheStats Stats(const USuperSLMSubsystem& Cpu) { return Cpu.GetPrefixCacheStats(); }

	inline int64 Served(const FSuperSLMPrefixCacheStats& S, ESuperSLMPrefixSource Source) { return S.ServedBySource[(int32)Source]; }

	inline int64 FallbackCount(const FSuperSLMPrefixCacheStats& S, const TCHAR* Name)
	{
		const int64* Found = S.Fallbacks.Find(FName(Name));
		return Found ? *Found : 0;
	}

	// --- Disk write-backs (box 2026-10-21) ------------------------------------------------------

	// §5.7: a Recomputed entry's write-back is a Save lane job (sslm_prefix_save), and only then a file
	// write on a pool thread. DiskIoInFlight() counts the pool-thread half alone, so it reads 0 while the
	// Save job is queued or running: "Drain until DiskIoInFlight() == 0" returned at once, before the file
	// existed (the box's eight disk-tier failures). A cell that needs a write-back landed waits on the write
	// being issued (DiskWritesIssued, raised when the Save completes) or failing (SaveFailed), then on the
	// pool-thread IO staying at 0 for consecutive ticks (a key's write runs behind its delete, so one quiet
	// tick can fall between the two).
	inline int64 WriteBackMark(const USuperSLMSubsystem& Cpu)
	{
		const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
		return S.DiskWritesIssued + FallbackCount(S, TEXT("SaveFailed"));
	}

	// Ticks until the pool-thread IO is quiet, every posted job has delivered, and the IO is quiet again: the
	// pool-thread IO and the inbox settled on consecutive ticks (counted per tick), then DrainQuiet, then the
	// same settle (a delivered job can issue IO: a build its save, a save its file write and trim). False if any
	// phase does not finish within Seconds; CacheQuietReport() says which.
	//
	// Box 2026-10-21 run 3 (Disk.EveryFallback, "disk write: the write-back settles"): this counted quiet ticks
	// through Drain(), whose predicate is evaluated twice per tick (so "three ticks" could be one and a half), read
	// DiskIoInFlight() alone (not the inbox), and read it once more after DrainQuiet's last tick, where IO that tick
	// launched was in flight. It now settles on IsCacheIoSettled() per tick, before and after the job drain.
	inline bool SettleCacheIoPhase(USuperSLMSubsystem& Cpu, double Seconds);
	inline bool WaitDiskQuiet(USuperSLMSubsystem& Cpu, double Seconds = OpSeconds)
	{
		const bool bIo = SettleCacheIoPhase(Cpu, Seconds);
		const bool bJobs = DrainQuiet(Cpu, Seconds);
		return SettleCacheIoPhase(Cpu, Seconds) && bIo && bJobs;
	}

	// What is still outstanding, for a failure message: the pool-thread IO, the inbox, and every posted job not
	// delivered (its kind and plan tick).
	inline FString CacheQuietReport(const USuperSLMSubsystem& Cpu)
	{
		FString Undelivered;
		int32 Count = 0;
		for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
		{
			if (Job.DeliveredAtTick < 0)
			{
				Undelivered += FString::Printf(TEXT("%s kind %d @%d"), Count == 0 ? TEXT("") : TEXT(","), (int32)Job.Kind, Job.PlannedAtTick);
				++Count;
			}
		}
		return FString::Printf(TEXT("pool IO in flight %d, IO and inbox settled %s, %d undelivered job(s)%s%s"),
			FSuperSLMPrefixCacheTestSeam::DiskIoInFlight(Cpu), FSuperSLMPrefixCacheTestSeam::IsCacheIoSettled(Cpu) ? TEXT("yes") : TEXT("no"),
			Count, Count > 0 ? TEXT(": ") : TEXT(""), *Undelivered);
	}

	// Waits for Count write-backs past Mark (WriteBackMark() read before the requests) to be issued or to
	// fail, then for the disk to go quiet. False on timeout.
	inline bool WaitWriteBacks(USuperSLMSubsystem& Cpu, int64 Mark, int64 Count = 1, double Seconds = BuildSeconds)
	{
		Drain(Cpu, [&Cpu, Mark, Count]() { return WriteBackMark(Cpu) >= Mark + Count; }, Seconds);
		const bool bIssued = WriteBackMark(Cpu) >= Mark + Count;
		return WaitDiskQuiet(Cpu) && bIssued;
	}

	// Ticks until the cache's pool-thread work has landed: no key hash or disk IO in flight and nothing
	// waiting for the apply pass, on two consecutive ticks; then until every posted job has delivered;
	// then settles the IO again (a delivered build issues its write-back, an applied hash its build).
	//
	// Why it exists (the second box run, 2026-10-21): a request's key hash runs on a pool thread and is
	// applied at a later Tick's apply pass, so a cell that ticks a fixed number of times and then reads
	// the ledger reads it before the build, the eviction or the waiter exists. DrainQuiet alone does not
	// cover it: before the hash lands no job is posted, so "every job delivered" is already true. The host
	// model applies every command synchronously and never sees the gap.
	//
	// Counts ticks itself rather than through Drain(): DrainTicks evaluates its predicate twice per tick,
	// which would count one quiet tick as two. Held hashes (HoldKeyHash) are parked off the counter and
	// read as settled. Bounded by Seconds per phase; false if the IO never settles. bDrainJobs false skips
	// the job phase, for a cell that holds a posted job at a gate (DrainQuiet would wait out its bound).
	inline bool SettleCacheIo(USuperSLMSubsystem& Cpu, double Seconds = OpSeconds, bool bDrainJobs = true)
	{
		const bool bFirst = SettleCacheIoPhase(Cpu, Seconds);
		if (!bDrainJobs)
		{
			return bFirst;
		}
		DrainQuiet(Cpu, Seconds);
		return SettleCacheIoPhase(Cpu, Seconds) && bFirst;
	}

	// One IO phase: ticks until the pool-thread IO and the inbox are settled on two consecutive ticks.
	inline bool SettleCacheIoPhase(USuperSLMSubsystem& Cpu, double Seconds)
	{
		const double Start = FPlatformTime::Seconds();
		int32 QuietTicks = 0;
		while (QuietTicks < 2 && FPlatformTime::Seconds() - Start < Seconds)
		{
			Cpu.Tick(Step);
			if (FSuperSLMPrefixCacheTestSeam::IsCacheIoSettled(Cpu))
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

	// --- The prefill twin ------------------------------------------------------------------------

	// Adopts Lease into a fresh sequence and generates TwinNewTokens after Suffix; returns the tokens.
	inline bool AdopterTokens(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMSequence Seq;
		if (!Vend(T, Cpu, Seq) || !AdoptAndWait(T, Cpu, Seq, Lease))
		{
			return false;
		}
		const bool bOk = GenerateComplete(T, Cpu, Seq, Suffix, TwinNewTokens, OutTokens);
		ReturnAndDrain(Cpu, Seq);
		return bOk;
	}

	// The same tokens built by CreatePrefix(), adopted, and generated the same way; the prefix is
	// released after. Computed once per (tokens, suffix) by the caller and compared by value.
	inline bool TwinTokens(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Tokens, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMPrefix Twin;
		if (!BuildPrefix(T, Cpu, Tokens, Twin))
		{
			return false;
		}
		const bool bOk = AdopterTokens(T, Cpu, Twin, Suffix, OutTokens);
		return ReleasePrefixAndDrain(T, Cpu, Twin) && bOk;
	}

	inline bool EqualsTwin(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Lease, const TArray<int32>& Suffix, const TArray<int32>& Twin, const TCHAR* What)
	{
		TArray<int32> Tokens;
		return AdopterTokens(T, Cpu, Lease, Suffix, Tokens)
			&& T.TestEqual(*FString::Printf(TEXT("%s: the adopter's tokens equal the prefill twin's"), What), Tokens, Twin);
	}

	// --- Save and restore, mid-turn ---------------------------------------------------------------
	//
	// One generation at a time: BeginGeneration() on a Complete sequence is refused ("this sequence is
	// Complete and no reset is queued after its last generation"), so a restored Complete sequence cannot
	// be continued by a second generation. Box 2026-10-21 (second run): LeaseAsRestoreHint, SaveRestoreAdopter
	// and SaveCompat.Crafted did exactly that and failed at the refusal. A continuation is a mid-turn save: the
	// restored sequence resumes the saved generation by itself. These are the paging restore suite's
	// SaveMidTurn / ContinueRestored (SuperSLMPagingCpuRestoreTests.cpp), shared here.

	inline FSuperSLMGenerationRequest GenerationRequest(const TArray<int32>& Prompt, int32 MaxNew)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNew;
		return Request;
	}

	// Begins Request on Seq, saves it once it has produced AtTokens tokens while still running, and returns
	// Seq. OutBefore is the tokens it had produced at the save.
	inline bool CacheSaveMidTurn(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMGenerationRequest& Request,
		int32 AtTokens, TArray<uint8>& OutBlob, TArray<int32>& OutBefore)
	{
		FString Error;
		const bool bBegun = Cpu.BeginGeneration(Seq, Request, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration (%s)"), *Error), bBegun))
		{
			return false;
		}
		bool bSaved = false;
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - Start < GenerateSeconds)
		{
			Cpu.Tick(Step);
			if (Cpu.GetGeneratedTokens(Seq).Num() >= AtTokens)
			{
				bSaved = SaveAndTake(T, Cpu, Seq, OutBlob);
				break;
			}
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		OutBefore = Cpu.GetGeneratedTokens(Seq);
		ReturnAndDrain(Cpu, Seq);
		return T.TestTrue(*FString::Printf(TEXT("saved mid-turn at %d tokens"), AtTokens), bSaved);
	}

	// Lets a restored sequence's resumed generation run to its end and appends what it produced to
	// InOutTokens. The sequence stays the caller's. It must produce at least one token: a save that landed at
	// Complete would otherwise pass vacuously (code review r2, N2). A restored sequence with no generation to
	// resume (Idle, nothing pending, for 60 consecutive ticks: a save taken between turns) fails here at once
	// instead of draining to GenerateSeconds (N1).
	inline bool CacheContinueRestored(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Restored, TArray<int32>& InOutTokens)
	{
		int32 IdleTicks = 0;
		auto Ended = [&Cpu, &Restored]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Restored);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		};
		const double Start = FPlatformTime::Seconds();
		while (!Ended() && IdleTicks < 60 && FPlatformTime::Seconds() - Start < GenerateSeconds)
		{
			Cpu.Tick(Step);
			const bool bIdle = Cpu.GetPhase(Restored) == ESuperSLMSequencePhase::Idle && Cpu.GetPendingLifecycleOperationCount(Restored) == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			if (!Ended())
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		if (!T.TestTrue(TEXT("the restored sequence has a generation to resume (not Idle: the save was taken mid-turn)"), IdleTicks < 60))
		{
			return false;
		}
		const bool bEnded = T.TestTrue(TEXT("the restored generation ends"), Ended());
		const TArray<int32> Produced = Cpu.GetGeneratedTokens(Restored);
		InOutTokens.Append(Produced);
		return bEnded && T.TestEqual(TEXT("the restored generation completes"), (uint8)Cpu.GetPhase(Restored), (uint8)ESuperSLMSequencePhase::Complete)
			&& T.TestTrue(*FString::Printf(TEXT("the restored sequence produced at least one token (%d)"), Produced.Num()), Produced.Num() > 0);
	}

	// --- Readings --------------------------------------------------------------------------------

	// Cell 1.4's admission form: creates totalling F_min = N - R_seq - Subtrahend - floor(budget / page)
	// must all be admitted, read behind the lane-park barrier. One-sided: it shows the cache holds at
	// most the budget's pages (with what it has in flight).
	inline bool CacheWithinBudget(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64 PoolPages, int64 ReserveSeq, int64 Subtrahend, int64 BudgetPagesFloor, const TCHAR* Instant)
	{
		const int64 FMin = PoolPages - ReserveSeq - Subtrahend - BudgetPagesFloor;
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the fixture keeps F_min >= 2 (F_min %lld)"), Instant, FMin), FMin >= 2))
		{
			return false;
		}
		const TOptional<bool> Admitted = ProbeAdmits(T, Cpu, FMin);
		return T.TestTrue(*FString::Printf(TEXT("%s: creates totalling F_min = %lld are admitted (the cache holds at most the budget's %lld pages)"), Instant, FMin, BudgetPagesFloor),
			Admitted.IsSet() && Admitted.GetValue());
	}

	// The fill probe's upper bound for a free reading. Budget mode: the ledger's pool. Legacy mode has no
	// ledger (LedgerOf().PoolPages reads 0, so a bisection bounded by it always read 2: box 2026-10-21,
	// Ledger.Legacy's "F_min -6"), so the bound is found by doubling until the probe refuses.
	inline int64 ProbeCeiling(FAutomationTestBase& T, USuperSLMSubsystem& Cpu)
	{
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		if (V.bConfigured && V.PoolPages > 0)
		{
			return V.PoolPages;
		}
		int64 Hi = 2;
		for (int32 Round = 0; Round < 24; ++Round)
		{
			const TOptional<bool> At = ProbeAdmits(T, Cpu, Hi * 2);
			if (!At.IsSet() || !At.GetValue())
			{
				return Hi * 2;
			}
			Hi *= 2;
		}
		return Hi;
	}

	// "Nothing drawn": the probe's free reading equal before and after.
	inline bool FreeUnchanged(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64 Before, const TCHAR* Instant)
	{
		int64 After = -1;
		return ProbeFree(T, Cpu, ProbeCeiling(T, Cpu), After)
			&& T.TestEqual(*FString::Printf(TEXT("%s: nothing drawn (fill probe free count)"), Instant), After, Before);
	}

	// The teardown reading (cells 1.7, 2.10): Configure() again with the teardown probe armed, so the
	// fill probe reads the initial count after every lane has stopped and before the pool is
	// destroyed, and the destroy's status is recorded.
	inline bool TeardownClean(FAutomationTestBase& T, FPagingRig& Rig, int64 InitialFree, const TCHAR* Instant)
	{
		FSuperSLMPrefixCacheTestSeam::ArmTeardownProbe(*Rig.Cpu);
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Rig.Config);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: re-Configure() succeeds (%s)"), Instant, *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the teardown fill probe reads the initial count"), Instant), FSuperSLMPrefixCacheTestSeam::TeardownFreePages(*Rig.Cpu), InitialFree);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the pool destroy returns SSLM_OK, not SSLM_POOL_HAS_LIVE_HANDLES"), Instant),
			int32(FSuperSLMPrefixCacheTestSeam::LastPoolDestroyStatus(*Rig.Cpu)), int32(SSLM_OK));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: no handle released twice (release-counting seam)"), Instant), FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandle(*Rig.Cpu) <= 1);
		return bOk;
	}

	// The pool's free pages at quiescence, with nothing leased: the initial count the teardown
	// readings compare against.
	inline bool InitialFree(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64& Out)
	{
		return DrainQuiet(Cpu) && ProbeFree(T, Cpu, ProbeCeiling(T, Cpu), Out);
	}

	// Fills the cache with Count distinct entries of Length tokens (salts Salt..Salt+Count-1), each
	// Ready and returned, so every one is unleased and resident. The entries are touched in
	// creation order.
	inline bool FillUnleased(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int32 Count, int32 Length, int32 Salt, TArray<FSuperSLMPrefixSpec>& OutSpecs)
	{
		for (int32 I = 0; I < Count; ++I)
		{
			TArray<int32> Tokens;
			FSuperSLMPrefix Lease;
			if (!MakeTokens(T, Cpu, Length, Salt + I, Tokens) || !RequestReady(T, Cpu, Spec(Tokens), Lease, TEXT("fill")))
			{
				return false;
			}
			Cpu.ReturnPrefix(Lease);
			OutSpecs.Add(Spec(Tokens));
		}
		return DrainQuiet(Cpu);
	}

	// A disk entry's file, by the key the cache computes (named by the seam, never by the test).
	inline FString DiskFileOf(const USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S)
	{
		return FSuperSLMPrefixCacheTestSeam::DiskFileOf(Cpu, S);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
