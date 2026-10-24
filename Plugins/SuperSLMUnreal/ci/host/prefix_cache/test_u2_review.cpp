// The U2 code review, round 1 (code-review/review-1p14-u2-round1.md, at 79163cd69): red cells for its findings 1-4,
// 6 and 7, host halves (precache plan rev 1.18.5). Finding 5 is the host's (RegisterPrefixSet before the first
// Configure(), and the re-Configure to another model): its cells are Unreal-side
// (SuperSLMPrefixCacheResidencyTests.cpp, PrefixSetModelAcrossConfigure).
//
//   host_path_* (3.10 (iii) rev 1.18.5; findings 1, 2): the product's re-Configure() and Deinitialize() path,
//       FCacheFixture::HostReconfigure (the CPU host's ReleaseForPoolDestroy, then Abandon()), never the core's
//       BeginTeardown(). The review's probes P2a and P2b as cells, plus Deinitialize(). 1.7's host-path leg is
//       disk_host_reconfigure_resolves_every_pending_op (test_residency_disk.cpp).
//   release_skipped_* (5.14's core half, rev 1.18.5; finding 3): EPrefixEngineResult::Skipped, a RED STUB.
//   warm_build_failure_* (5.12 rev 1.18.5; finding 4): EPrefixOpResult::Failed and FPrefixOpResult::Reason, RED STUBS.
//   drop_delete_failed_line_* (5.12 / §5.9 rev 1.18.4; finding 6): FPrefixCompletion::IoError, a RED STUB.
//   touch_* (3.10 (vii) rev 1.18.5, §5.7; finding 7): the touch on the key's disk serialization; the sim counts the
//       touch in the per-key overlap counter, and a delete meeting a touch held running fails (SetTimeStamp opens
//       with read sharing only).

#include "cache_fixture.h"

#include <cstdio>

using namespace HostSim;

namespace
{
	enum class EHostEnd : uint8_t
	{
		DiskTierOff,
		OtherDirectory,
		Deinitialize,
	};

	const char* EndName(EHostEnd End)
	{
		switch (End)
		{
		case EHostEnd::DiskTierOff: return "re-Configure() with the disk tier off";
		case EHostEnd::OtherDirectory: return "re-Configure() with another directory";
		case EHostEnd::Deinitialize: return "Deinitialize()";
		}
		return "?";
	}

	// 3.10 (iii) rev 1.18.5: a MemoryAndDisk drop's delete waiting on a write-back's save that no lane has run, a
	// file already under the key, then the host path. The file is gone after the call and the op reads Deleted, its
	// memory part DropDeferred (the request's lease is live); the delete ran once, under the old configuration.
	void DeleteBehindSaveAcrossHostEnd(EHostEnd End)
	{
		std::printf("  3.10 (iii) host path: %s\n", EndName(End));
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		FSim& Sim = *F.Sim;
		const FPrefixSpec S = FixtureSpec(2);
		Sim.Held.insert(EPrefixCommandKind::Save); // the write-back's save, queued for a lane and never posted
		const int64_t H = F.RequestReady(S);
		REQUIRE_NE(H, 0);
		REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::Save), static_cast<size_t>(1));
		F.PutDiskFile(S); // another PIE instance's write, or a file whose read failed Error
		const std::string OldPath = Sim.DiskPath(FixtureKey(S));
		REQUIRE_TRUE(Sim.Fs->Exists(OldPath));
		const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(D, 0u);
		Sim.RunAll();
		REQUIRE_EQ(F.Cache.PendingDropDeletes(), 1u);
		REQUIRE_EQ(F.OpResult(D), EPrefixOpResult::Pending);

		FPrefixCacheConfig Next = F.Config;
		if (End == EHostEnd::DiskTierOff)
		{
			Next.bDiskEnabled = false;
		}
		else if (End == EHostEnd::OtherDirectory)
		{
			Next.DiskDirectory = Next.DiskDirectory + "_other";
		}
		REQUIRE_TRUE(F.HostReconfigure(End == EHostEnd::Deinitialize ? nullptr : &Next));
		// After the call: the file is gone and the op has resolved, with nothing left to run.
		EXPECT_EQ(Sim.Fs->Exists(OldPath), false);
		EXPECT_EQ(F.Cache.PendingDropDeletes(), 0u);
		const FPrefixOpResult R = F.Op(D);
		std::printf("    op %d, disk %d, file left %d, drop deletes run %u\n", static_cast<int>(R.Result), static_cast<int>(R.Disk),
			Sim.Fs->Exists(OldPath) ? 1 : 0, Sim.DropDeletesRun);
		EXPECT_EQ(R.Result, EPrefixOpResult::DropDeferred);
		EXPECT_EQ(R.Disk, EPrefixDropDisk::Deleted);
		EXPECT_EQ(Sim.DropDeletesRun, 1u);
		if (End != EHostEnd::Deinitialize)
		{
			// The new configuration never runs it again (not carried).
			Sim.Held.clear();
			Sim.RunAll();
			EXPECT_EQ(Sim.DropDeletesRun, 1u);
			EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
		}
		EXPECT_TRUE(Sim.Violations.empty());
	}
}

HOST_TEST(host_path_drop_delete_behind_a_save_disk_tier_off, "3.10 (iii) [U2] (rev 1.18.5; U2 code review findings 1 and 2, probe P2a): a delete held at a re-Configure() on the host's path runs once and reads Deleted; then a delete waiting on a write-back's save no lane has run, a file under the key, across a re-Configure() that turns the disk tier off: the file is gone after the call, the op reads Deleted, the delete ran once under the old configuration (mutant: the delete carried into the next Configure() -> DiskTierOff, the file left)")
{
	// The held-delete leg of 3.10 (iii) (rev 1.18.1) on the host's path.
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec S = FixtureSpec(1);
		REQUIRE_TRUE(F.Resident(S));
		F.Sim->RunAll();
		F.Sim->Held.insert(EPrefixCommandKind::RemoveDiskFile);
		const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(D, 0u);
		F.Sim->RunAll();
		REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(1));
		const FPrefixCacheConfig Same = F.Config;
		REQUIRE_TRUE(F.HostReconfigure(&Same));
		F.Sim->RunAll();
		EXPECT_EQ(F.Sim->DropDeletesRun, 1u);
		EXPECT_EQ(F.DiskFileExists(S), false);
		EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
	}
	DeleteBehindSaveAcrossHostEnd(EHostEnd::DiskTierOff);
}

HOST_TEST(host_path_drop_delete_behind_a_save_other_directory, "3.10 (iii) [U2] (rev 1.18.5; U2 code review findings 1 and 2, probe P2b): a delete waiting on a write-back's save no lane has run, a file under the key, across a re-Configure() to another directory: the file is gone after the call, the op reads Deleted (mutant: the delete carried into the next Configure() -> NoFile in the new directory, the old file left)")
{
	DeleteBehindSaveAcrossHostEnd(EHostEnd::OtherDirectory);
}

HOST_TEST(host_path_drop_delete_behind_a_save_deinitialize, "3.10 (iii) [U2] (rev 1.18.5; U2 code review findings 1 and 2): a delete waiting on a write-back's save no lane has run, a file under the key, at Deinitialize(): the file is gone after the call, the op reads Deleted (mutant: the delete carried to a Configure() that never comes -> never run, the file left)")
{
	DeleteBehindSaveAcrossHostEnd(EHostEnd::Deinitialize);
}

// ---- Finding 3: a skipped release is not a release that returned ------------------------------------------------

HOST_TEST(release_skipped_by_the_submission_thread_stays_pending, "5.14 core half [U2 core; U6] (rev 1.18.5; U2 code review finding 3; §5.4 a skipped release is not a release that returned): a release that returned Failed has applied (refund complete, ReleaseFailed once, the handle consumed); a release the submission thread skipped (Skipped) has not: its refund stays pending, nothing is refunded, no ReleaseFailed, it is not issued again over later passes, the engine handle stays live, and teardown's sweep releases it exactly once (mutant: a skipped job delivered as a returned Failed -> refunded at once, ReleaseFailed logged)")
{
	// Returned Failed: applied (5.15's rule), the contrast.
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec S = FixtureSpec(1);
		const int64_t H = F.RequestReady(S);
		REQUIRE_NE(H, 0);
		const uint64_t E = F.Sim->EngineHandleOf(H);
		REQUIRE_TRUE(F.Cache.Return(H));
		F.Sim->Pass();
		F.Sim->ConsumeThenFailReleases = 1;
		REQUIRE_NE(F.Drop(S), 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.Sim->ReleaseCount[E], 1);
		EXPECT_EQ(F.Diag().ReleasingBytes, 0u);
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReleaseFailed), 1u);
	}
	// Skipped: not applied.
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	FSim& Sim = *F.Sim;
	const FPrefixSpec S = FixtureSpec(1);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	const uint64_t E = Sim.EngineHandleOf(H);
	REQUIRE_TRUE(F.Cache.Return(H));
	Sim.Pass();
	const uint64_t Charged = F.Diag().ChargedBytes;
	REQUIRE_NE(Charged, 0u);
	Sim.SkipReleases = 1000; // every release from here is skipped (a terminal device loss)
	const uint64_t D = F.Drop(S);
	REQUIRE_NE(D, 0u);
	Sim.RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
	const size_t Issued = Sim.CountHistory(EPrefixCommandKind::Release);
	EXPECT_EQ(Issued, static_cast<size_t>(1));
	EXPECT_EQ(Sim.SkippedReleases, 1u);
	FPrefixCacheDiagnostics Diag = F.Diag();
	std::printf("  skipped release: charged %llu -> %llu, releasing %llu, ReleaseFailed %llu, issued %zu\n",
		static_cast<unsigned long long>(Charged), static_cast<unsigned long long>(Diag.ChargedBytes),
		static_cast<unsigned long long>(Diag.ReleasingBytes),
		static_cast<unsigned long long>(F.Fallbacks(EPrefixFallback::ReleaseFailed)), Issued);
	EXPECT_EQ(Diag.ChargedBytes, Charged);     // nothing refunded
	EXPECT_EQ(Diag.ReleasingBytes, Charged);   // the refund stays pending
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReleaseFailed), 0u);
	EXPECT_EQ(Sim.ReleaseCount[E], 0);         // the handle never reached the engine
	EXPECT_TRUE(!Sim.Handles[E].bReleased);
	// Later passes issue nothing more (no re-issue spin).
	for (int I = 0; I < 8; ++I)
	{
		Sim.Pass();
		Sim.RunAll();
	}
	EXPECT_EQ(Sim.CountHistory(EPrefixCommandKind::Release), Issued);
	// Teardown's sweep releases the live handle exactly once (the host path's step 3), without waiting on it.
	Sim.SkipReleases = 0;
	REQUIRE_TRUE(F.HostReconfigure(nullptr));
	EXPECT_EQ(Sim.ReleaseCount[E], 1);
	EXPECT_TRUE(F.EveryHandleReleasedOnce());
	EXPECT_TRUE(Sim.Violations.empty());
}

// ---- Finding 4: a warm whose build fails reads Failed, not Cancelled -------------------------------------------

HOST_TEST(warm_build_failure_reads_failed_with_its_reason, "5.12 [U2] (rev 1.18.5; U2 code review finding 4, probe P3; §5.6 GetPrefixOpResult): a warm whose build fails (the builds-fail seam) reads Failed, with Reason == BuildFailed, one BuildFailed log line carrying the key and BuildFailed's counter up by exactly one; it does not read Cancelled, which stays re-Configure() only (mutant: a failed build mapped to Cancelled -> red)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->bFailBuilds = true;
	const FPrefixSpec S = FixtureSpec(3);
	F.Cache.TakeLog();
	const uint64_t Before = F.Fallbacks(EPrefixFallback::BuildFailed);
	const uint64_t W = F.Warm(S);
	REQUIRE_NE(W, 0u);
	F.Sim->RunAll();
	const FPrefixOpResult R = F.Op(W);
	std::printf("  warm on a failed build: result %d (Failed = %d, Cancelled = %d), reason %d\n", static_cast<int>(R.Result),
		static_cast<int>(EPrefixOpResult::Failed), static_cast<int>(EPrefixOpResult::Cancelled), static_cast<int>(R.Reason));
	EXPECT_EQ(R.Result, EPrefixOpResult::Failed);
	EXPECT_EQ(R.Reason, EPrefixFallback::BuildFailed);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::BuildFailed) - Before, 1u);
	const std::vector<FPrefixLogLine> Log = F.Cache.TakeLog();
	EXPECT_EQ(F.LogLinesWith(Log, EPrefixFallback::BuildFailed), static_cast<size_t>(1));
	bool bKeyed = false;
	for (const FPrefixLogLine& Line : Log)
	{
		bKeyed |= Line.Reason == EPrefixFallback::BuildFailed && Contains(Line.Message, PrefixKeyToHex(FixtureKey(S)));
	}
	EXPECT_TRUE(bKeyed);
}

// ---- Finding 6: the DropDeleteFailed line carries the file API's error -----------------------------------------

HOST_TEST(drop_delete_failed_line_carries_the_file_api_error, "5.12 / §5.9 [U2] (rev 1.18.4; U2 code review finding 6): a drop delete the file API refuses logs one DropDeleteFailed line carrying the key, the file path and the file API's error (the completion's IoError: the refusal's, and a sharing violation's); Disk == Failed (mutant: the line's fixed text without the error -> red)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(4);
	REQUIRE_TRUE(F.Resident(S));
	F.Sim->RunAll();
	F.Cache.TakeLog();
	F.Sim->Fs->bFailRemoves = true;
	const uint64_t Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	F.Sim->Fs->bFailRemoves = false;
	EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::Failed);
	const std::vector<FPrefixLogLine> Log = F.Cache.TakeLog();
	EXPECT_EQ(F.LogLinesWith(Log, EPrefixFallback::DropDeleteFailed), static_cast<size_t>(1));
	for (const FPrefixLogLine& Line : Log)
	{
		if (Line.Reason != EPrefixFallback::DropDeleteFailed)
		{
			continue;
		}
		std::printf("  DropDeleteFailed line: %s\n", Line.Message.c_str());
		EXPECT_TRUE(Contains(Line.Message, PrefixKeyToHex(FixtureKey(S))));
		EXPECT_TRUE(Contains(Line.Message, F.Sim->DiskPath(FixtureKey(S))));
		EXPECT_TRUE(Contains(Line.Message, F.Sim->RemoveRefusalError));
	}
}

// ---- Finding 7: the touch on the key's disk serialization --------------------------------------------------------

HOST_TEST(touch_and_drop_delete_never_leave_together, "3.10 (vii) / §5.7 [U2] (rev 1.18.5; U2 code review finding 7, probe P1): a disk hit's TouchDiskFile and a drop's RemoveDiskFile of the same key never leave the core in one TakeCommands batch: the delete waits for the touch's completion, as for the key's read, write-back and delete (mutant: the touch left off the key's serialization -> both in one batch)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	FSim& Sim = *F.Sim;
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey Key = FixtureKey(S);
	F.PutDiskFile(S);
	Sim.Held.insert(EPrefixCommandKind::Restore);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	Sim.RunAll(); // hash, read; the restore waits on a lane
	REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::Restore), static_cast<size_t>(1));
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	Sim.bAutoPull = false;
	Sim.Pull();
	// One apply pass with two completions: the restore and the drop's hash (the host's Pump applies the whole inbox).
	FPrefixCompletion Restore, Hash;
	REQUIRE_TRUE(Sim.ExecuteOnly(EPrefixCommandKind::Restore, Restore));
	REQUIRE_TRUE(Sim.ExecuteOnly(EPrefixCommandKind::ComputeKey, Hash));
	F.Cache.Apply(Restore);
	F.Cache.Apply(Hash);
	F.Cache.EndApplyPass();
	bool bTouch = false, bDelete = false;
	for (const FPrefixCommand& C : F.Cache.TakeCommands())
	{
		bTouch |= C.Kind == EPrefixCommandKind::TouchDiskFile && C.Key == Key;
		bDelete |= C.Kind == EPrefixCommandKind::RemoveDiskFile && C.Key == Key && C.OpId == D;
	}
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	std::printf("  one batch: TouchDiskFile %d, the drop's RemoveDiskFile %d\n", bTouch ? 1 : 0, bDelete ? 1 : 0);
	EXPECT_TRUE(bTouch);
	EXPECT_TRUE(!bDelete);
}

HOST_TEST(touch_held_then_drop_delete_runs_after_it, "3.10 (vii) [U2] (rev 1.18.5; U2 code review finding 7; §5.7): a disk hit's touch held in place of the read: the touch completes, then the delete runs; Disk == Deleted, the file gone, and the per-key overlap counter, now over the touch too, never reads 2 (mutant: the touch left off the key's serialization -> the counter reads 2 and the delete fails as on Windows, Disk == Failed with the file left)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	FSim& Sim = *F.Sim;
	const FPrefixSpec S = FixtureSpec(1);
	F.PutDiskFile(S);
	Sim.Held.insert(EPrefixCommandKind::TouchDiskFile);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	REQUIRE_EQ(F.Cache.GetSource(H), EPrefixSource::Disk);
	REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::TouchDiskFile), static_cast<size_t>(1));
	REQUIRE_TRUE(F.Cache.Return(H));
	Sim.Pass();
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	Sim.RunAll();
	Sim.Held.clear();
	Sim.RunAll();
	const FPrefixOpResult R = F.Op(D);
	std::printf("  touch held: op %d, disk %d, overlap %u, file left %d\n", static_cast<int>(R.Result), static_cast<int>(R.Disk),
		Sim.MaxKeyDiskOverlap, F.DiskFileExists(S) ? 1 : 0);
	EXPECT_EQ(R.Disk, EPrefixDropDisk::Deleted);
	EXPECT_LE(Sim.MaxKeyDiskOverlap, 1u);
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_TRUE(Sim.Violations.empty());
}
