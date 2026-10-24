// The rev 1.18.1-1.18.2 simulator seams (precache plan §8), each with its must-accept and its must-reject,
// graded on the seam alone: commands are put straight into the executor's queue and their completions read
// with ExecuteOnly, so no cache-core behaviour decides any of these (they are green on any core).
//
//   - release-status substitution (5.15): runs the real release, then reports a chosen non-OK status;
//     must-reject: the rev 1.18 seam that skips the release (FailReleases) reads 0 releases;
//   - GPU busy-release substitution (5.15 [U6]): SSLM_BUSY without releasing, then the real release; must-reject:
//     a seam that releases before reporting SSLM_BUSY reads a release at the first issue and a second at the
//     re-issue;
//   - hash-completion hold (3.12, 3.13, 1.7): must-reject: an op whose hash is held is never applied;
//   - per-key overlap counter (3.10 (vii)): must-reject: a forced overlap of a read and a delete of one key reads 2;
//   - exclusion counter over a drop's delete and the trim (3.10): must-reject: a forced overlap reads 2;
//   - file-delete refusal (5.12): the delete fails and the file stays.

#include "cache_fixture.h"

using namespace HostSim;

namespace
{
	constexpr uint64_t kSyntheticId = 1ull << 60;

	// A live engine handle the simulator owns, mapping Pages fresh pages, outside any cache record.
	uint64_t AddHandle(FSim& Sim, uint64_t Pages)
	{
		FEngineHandle H;
		for (uint64_t I = 0; I < Pages; ++I)
		{
			const uint64_t Page = Sim.NextPageId++;
			++Sim.PageRefs[Page];
			H.PageIds.push_back(Page);
		}
		H.Pages = Pages;
		const uint64_t Id = Sim.NextEngineHandle++;
		Sim.Handles[Id] = H;
		Sim.CacheDrawnPages = Sim.PageRefs.size();
		return Id;
	}

	void Push(FSim& Sim, EPrefixCommandKind Kind, const FPrefixKey& Key, uint64_t EngineHandle = 0, uint64_t OpId = 0)
	{
		static uint64_t Next = kSyntheticId;
		FPrefixCommand C;
		C.Id = Next++;
		C.Kind = Kind;
		C.Key = Key;
		C.EngineHandle = EngineHandle;
		C.OpId = OpId;
		Sim.Queue.push_back(C);
	}
}

HOST_TEST(seam_release_status_substitution, "§8 (rev 1.18.1, 5.15): the release-status substitution seam consumes the handle, then reports the chosen status (must-accept); the rev 1.18 seam that skips the release is refused by the release-counting seam reading 0 (must-reject)")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	// Must-accept: one release counted, the handle and its pages gone, the completion Failed with the status.
	const uint64_t A = AddHandle(*F.Sim, 3);
	F.Sim->ConsumeThenFailReleases = 1;
	F.Sim->ReleaseFailStatus = 4;
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, A);
	FPrefixCompletion Done;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(Done.Engine, EPrefixEngineResult::Failed);
	EXPECT_EQ(Done.EngineStatus, 4);
	EXPECT_EQ(F.Sim->ReleaseCount[A], 1);
	EXPECT_TRUE(F.Sim->Handles[A].bReleased);
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	EXPECT_EQ(F.Sim->ConsumeThenFailReleases, 0u);
	// The next release is the plain one.
	const uint64_t B = AddHandle(*F.Sim, 1);
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, B);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(Done.Engine, EPrefixEngineResult::Ok);
	EXPECT_EQ(F.Sim->ReleaseCount[B], 1);

	// Must-reject: the withdrawn seam reports Failed and releases nothing, which no engine release does.
	const uint64_t C = AddHandle(*F.Sim, 2);
	F.Sim->FailReleases = 1;
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, C);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(Done.Engine, EPrefixEngineResult::Failed);
	EXPECT_EQ(F.Sim->ReleaseCount.count(C), static_cast<size_t>(0)); // 0 releases: refused as the engine's model
	EXPECT_EQ(F.Sim->Handles[C].bReleased, false);
	EXPECT_TRUE(F.Sim->Violations.empty());
}

HOST_TEST(seam_busy_release_substitution, "§8 (rev 1.18.2, 5.15 [U6]): the busy-release seam returns SSLM_BUSY without releasing for N issues, then the real release runs (must-accept); a seam that releases before SSLM_BUSY reads a release at the first issue and a second at the re-issue (must-reject)")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t A = AddHandle(*F.Sim, 2);
	F.Sim->BusyReleases = 2;
	FPrefixCompletion Done;
	for (int Issue = 1; Issue <= 2; ++Issue)
	{
		Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, A);
		REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
		EXPECT_EQ(Done.Engine, EPrefixEngineResult::Busy);
		EXPECT_EQ(Done.EngineStatus, 2);
		EXPECT_EQ(F.Sim->ReleaseCount.count(A), static_cast<size_t>(0));
		EXPECT_EQ(F.Sim->Handles[A].bReleased, false);
		EXPECT_EQ(F.Sim->CacheDrawnPages, 2u);
	}
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, A);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(Done.Engine, EPrefixEngineResult::Ok);
	EXPECT_EQ(F.Sim->ReleaseCount[A], 1);
	EXPECT_EQ(F.Sim->BusyIssues, 2u);
	EXPECT_TRUE(F.Sim->Violations.empty());

	// Must-reject: release first, then report SSLM_BUSY. The re-issue the BUSY contract calls for then
	// releases a freed handle: the release-counting seam reads 1 at the first issue and 2 at the second.
	const uint64_t B = AddHandle(*F.Sim, 1);
	F.Sim->BusyReleases = 1;
	F.Sim->bBusyAfterRelease = true;
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, B);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(Done.Engine, EPrefixEngineResult::Busy);
	EXPECT_EQ(F.Sim->ReleaseCount[B], 1);
	Push(*F.Sim, EPrefixCommandKind::Release, FPrefixKey{}, B);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done));
	EXPECT_EQ(F.Sim->ReleaseCount[B], 2);
	EXPECT_EQ(F.Sim->Violations.size(), static_cast<size_t>(1)); // refused: a second release of a freed handle
}

HOST_TEST(seam_hash_completion_hold, "§8 (rev 1.18.1, 3.12): with the hash-completion hold set, a lease whose hash is held is never applied by RunAll (must-reject); RunHashOfLease applies it (must-accept)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->HeldHashLeases.insert(H);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::ComputeKey), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
	// Another request's hash is not held by it: its hash runs, and only H's stays queued. The request itself
	// still waits, behind H in issue order (§5.6 What "later" means: an op whose hash has completed waits
	// behind any earlier-issued op whose hash has not).
	const int64_t H2 = F.Request(FixtureSpec(2));
	REQUIRE_NE(H2, 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::ComputeKey), static_cast<size_t>(1));
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Pending);
	REQUIRE_TRUE(F.Sim->RunHashOfLease(H));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Ready);
}

HOST_TEST(seam_per_key_overlap_counter, "§8 (rev 1.18.2, 3.10 (vii)): the per-key overlap counter reads 1 for a delete alone (must-accept) and 2 for a forced overlap of a read and a delete of one key, which then fails as on Windows (must-reject)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.PutDiskFile(S);
	F.Sim->Pull();
	F.Sim->Queue.clear(); // the Configure trim
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 77);
	FPrefixCompletion Done;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(F.Sim->MaxKeyDiskOverlap, 1u);
	EXPECT_EQ(Done.Io, EPrefixIoStatus::Ok);
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_EQ(F.Sim->DropDeletesRun, 1u);

	F.PutDiskFile(S);
	F.Sim->Held.insert(EPrefixCommandKind::ReadDisk);
	Push(*F.Sim, EPrefixCommandKind::ReadDisk, K);   // held: running, mid-read
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 78);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(F.Sim->MaxKeyDiskOverlap, 2u);
	EXPECT_EQ(Done.Io, EPrefixIoStatus::Error); // a delete meeting a file open for reading
	EXPECT_EQ(F.DiskFileExists(S), true);
}

HOST_TEST(seam_delete_trim_exclusion_counter, "§8 (rev 1.18.1 audit N4, 3.10): the exclusion counter over a drop's delete and the trim reads 1 for either alone (must-accept) and 2 for a forced overlap (must-reject)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixKey K = FixtureKey(FixtureSpec(1));
	F.Sim->Pull();
	F.Sim->Queue.clear();
	Push(*F.Sim, EPrefixCommandKind::TrimDisk, K);
	FPrefixCompletion Done;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::TrimDisk, Done));
	EXPECT_EQ(F.Sim->MaxDeleteTrimOverlap, 1u);
	// A wrapper-miss removal (no op) is not a drop's delete and never counts.
	F.Sim->Held.insert(EPrefixCommandKind::TrimDisk);
	Push(*F.Sim, EPrefixCommandKind::TrimDisk, K);
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 0);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(F.Sim->MaxDeleteTrimOverlap, 1u);
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 9);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(F.Sim->MaxDeleteTrimOverlap, 2u);
}

HOST_TEST(seam_file_delete_refusal, "§8 (rev 1.18, 5.12): with the file-delete refusal seam a delete of a present file fails and the file stays (must-reject); without it the delete removes the file (must-accept); a missing file is NotFound either way")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Pull();
	F.Sim->Queue.clear();
	F.PutDiskFile(S);
	F.Sim->Fs->bFailRemoves = true;
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 5);
	FPrefixCompletion Done;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(Done.Io, EPrefixIoStatus::Error);
	EXPECT_EQ(F.DiskFileExists(S), true);
	F.Sim->Fs->bFailRemoves = false;
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 6);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(Done.Io, EPrefixIoStatus::Ok);
	EXPECT_EQ(F.DiskFileExists(S), false);
	Push(*F.Sim, EPrefixCommandKind::RemoveDiskFile, K, 0, 7);
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::RemoveDiskFile, Done));
	EXPECT_EQ(Done.Io, EPrefixIoStatus::NotFound);
}
