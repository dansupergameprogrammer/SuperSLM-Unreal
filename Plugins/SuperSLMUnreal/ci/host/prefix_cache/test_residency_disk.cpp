// The disk part of DropPrefix(MemoryAndDisk) and Pending ops at a re-Configure() (precache plan rev 1.18-1.18.2
// §5.6 *The disk part* and *Generations and teardown*): cell 3.10 (base, (i)-(vii)) and 1.7's rev 1.18.2 leg,
// host halves.
//
// The seams (sim.h): the pool-thread hold is Sim->Held on WriteDisk, ReadDisk, TrimDisk or RemoveDiskFile (a
// held command has been taken and is running); the lane-park seam is Sim->Held on Save or Release; the build
// hold is Sim->Held on Build; the hash-completion hold is Sim->HeldHashOps / HeldHashLeases. The per-key
// overlap counter (MaxKeyDiskOverlap) and the delete/trim exclusion counter (MaxDeleteTrimOverlap) read 2
// when a command of the pair runs while the other is held running; DropDeletesRun counts drop deletes run.
//
// Red first on the residency-control stubs: every cell issues a DropPrefix or WarmPrefix, which the stub
// refuses.
//
// Host reading of 1.7's "After Configure() returns and the held write-back is released": the host core
// accepts Configure() only once its teardown has completed, and the teardown waits for every command in
// flight, the held write-back included. So each run releases its holds after BeginTeardown() and before
// Configure(), and reads the ops after Configure() returns (the red-suite record routes the reading).

#include "cache_fixture.h"

#include <cstdio>

using namespace HostSim;

namespace
{
	bool IsDropDelete(const FPrefixCommand& C, uint64_t Op) { return C.Kind == EPrefixCommandKind::RemoveDiskFile && C.OpId == Op; }

	// History index of the first command matching Pred; History.size() if none.
	template <typename TPred>
	size_t IndexOf(const FSim& Sim, TPred Pred)
	{
		for (size_t I = 0; I < Sim.History.size(); ++I)
		{
			if (Pred(Sim.History[I]))
			{
				return I;
			}
		}
		return Sim.History.size();
	}

	size_t IndexOfLast(const FSim& Sim, EPrefixCommandKind Kind, const FPrefixKey& Key)
	{
		size_t Found = Sim.History.size();
		for (size_t I = 0; I < Sim.History.size(); ++I)
		{
			if (Sim.History[I].Kind == Kind && Sim.History[I].Key == Key)
			{
				Found = I;
			}
		}
		return Found;
	}

	size_t WritesOf(const FSim& Sim, const FPrefixKey& Key)
	{
		size_t N = 0;
		for (const FPrefixCommand& C : Sim.History)
		{
			N += C.Kind == EPrefixCommandKind::WriteDisk && C.Key == Key ? 1 : 0;
		}
		return N;
	}

	uint64_t DirectoryBytes(const FSim& Sim)
	{
		uint64_t Bytes = 0;
		const std::string Prefix = Sim.Config.DiskDirectory + "/";
		for (const auto& Pair : Sim.Fs->Files)
		{
			Bytes += Pair.first.compare(0, Prefix.size(), Prefix) == 0 ? Pair.second.Data.size() : 0;
		}
		return Bytes;
	}

	// The drop's op has resolved and its disk part ran (Deleted or NoFile, never Failed or NotRequested).
	bool DiskPartRan(const FCacheFixture& F, uint64_t Op)
	{
		const FPrefixOpResult R = F.Op(Op);
		return R.Result != EPrefixOpResult::Pending && (R.Disk == EPrefixDropDisk::Deleted || R.Disk == EPrefixDropDisk::NoFile);
	}
}

// ---- 3.10 base ---------------------------------------------------------------------------------------

HOST_TEST(disk_drop_behind_a_running_write_back, "3.10 [U2]: DropPrefix(MemoryAndDisk) issued while the key's write-back is running (the pool-thread hold mid-write): the write-back completes, then the delete; after the op resolves no file exists under the key and no temporary is left (mutant: the delete not serialized behind the write-back)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::WriteDisk);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
	EXPECT_TRUE(F.Cache.Return(H));
	F.Sim->Pass();
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Pending); // the disk part waits behind the write-back
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
	EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_TRUE(F.Sim->Fs->NamesIn(F.Config.DiskDirectory).empty()); // no temporary left
	EXPECT_EQ(F.Sim->MaxKeyDiskOverlap, 1u);
	EXPECT_LT(IndexOfLast(*F.Sim, EPrefixCommandKind::WriteDisk, K),
		IndexOf(*F.Sim, [D](const FPrefixCommand& C) { return IsDropDelete(C, D); }));
	EXPECT_EQ(F.Diag().DropDeletes, 1u);
}

HOST_TEST(disk_drop_during_a_build_writes_no_file, "3.10 [U2]: a drop issued while the key's build is in flight: the build completes and serves, and no file is written for it; a request admitted after the drop writes back normally")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::Build);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
	EXPECT_TRUE(DiskPartRan(F, D));
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
	EXPECT_EQ(WritesOf(*F.Sim, K), static_cast<size_t>(0));
	EXPECT_EQ(F.DiskFileExists(S), false);
	// The mark applies at the return; a request admitted after that writes back normally.
	EXPECT_TRUE(F.Cache.Return(H));
	F.Sim->Pass();
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	const int64_t H2 = F.RequestReady(S);
	REQUIRE_NE(H2, 0);
	EXPECT_EQ(F.Cache.GetSource(H2), EPrefixSource::Recomputed);
	EXPECT_EQ(WritesOf(*F.Sim, K), static_cast<size_t>(1));
	EXPECT_TRUE(F.DiskFileExists(S));
}

HOST_TEST(disk_drop_racing_a_trim, "3.10 [U2]: a drop's delete racing a trim, both held by the pool-thread hold and released in each order: one runs at a time (the exclusion counter never reads 2), and the directory ends without the key's file and within its limit")
{
	for (const bool bTrimFirst : {true, false})
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec E = FixtureSpec(1);
		const std::vector<uint8_t> Payload = MakeFakePayload(E.SchemaName, E.Tokens);
		const uint64_t FileBytes = EncodePrefixWrapper(F.Config.Identity, FixtureKey(E), StoredOf(E), Payload.data(), Payload.size()).size();
		FPrefixCacheConfig C = F.Config;
		C.DiskMaxBytes = 2 * FileBytes; // room for two files
		REQUIRE_TRUE(F.Reconfigure(C));
		F.Sim->RunAll();
		REQUIRE_TRUE(F.Resident(E));
		REQUIRE_TRUE(F.Resident(FixtureSpec(2)));
		F.Sim->RunAll();
		REQUIRE_TRUE(F.DiskFileExists(E));
		F.Sim->Held = {EPrefixCommandKind::TrimDisk, EPrefixCommandKind::RemoveDiskFile};
		uint64_t D = 0;
		if (bTrimFirst)
		{
			REQUIRE_TRUE(F.Resident(FixtureSpec(3))); // its write emits a trim, held running
			D = F.Drop(E, EPrefixDropScope::MemoryAndDisk);
			REQUIRE_NE(D, 0u);
			F.Sim->RunAll();
			EXPECT_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::TrimDisk));
			F.Sim->RunOneOfKind(EPrefixCommandKind::RemoveDiskFile);
		}
		else
		{
			D = F.Drop(E, EPrefixDropScope::MemoryAndDisk);
			REQUIRE_NE(D, 0u);
			F.Sim->RunAll(); // the delete held running
			REQUIRE_TRUE(F.Resident(FixtureSpec(3)));
			EXPECT_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::RemoveDiskFile));
			F.Sim->RunOneOfKind(EPrefixCommandKind::TrimDisk);
		}
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.Sim->MaxDeleteTrimOverlap, 1u);
		EXPECT_EQ(F.DiskFileExists(E), false);
		EXPECT_LE(DirectoryBytes(*F.Sim), C.DiskMaxBytes);
		EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
		EXPECT_TRUE(DiskPartRan(F, D));
	}
}

// ---- 3.10 rev 1.18.1 (i)-(iii) -----------------------------------------------------------------------

HOST_TEST(disk_attach_after_a_drop_restores_write_back, "3.10 (i) [U2] (rev 1.18.1, F9): a request issued after the drop that attaches to the build in flight is served, and that build's write-back is restored: the file exists after it completes, written after the delete (mutant: the attach leaves write-back suppressed)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::Build);
	const int64_t H0 = F.Request(S);
	REQUIRE_NE(H0, 0);
	F.Sim->RunAll();
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	const int64_t H1 = F.Request(S); // attaches to the build in flight
	REQUIRE_NE(H1, 0);
	F.Sim->RunAll();
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H0), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(H1), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Diag().MarksCleared, 1u);
	EXPECT_TRUE(DiskPartRan(F, D));
	EXPECT_TRUE(F.DiskFileExists(S));
	EXPECT_GT(IndexOfLast(*F.Sim, EPrefixCommandKind::WriteDisk, K),
		IndexOf(*F.Sim, [D](const FPrefixCommand& C) { return IsDropDelete(C, D); }));
}

HOST_TEST(disk_held_delete_reads_pending, "3.10 (ii) [U2] (rev 1.18.1): while the delete is held the op reads Pending; after its release Disk == Deleted")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	REQUIRE_TRUE(F.Resident(S));
	F.Sim->RunAll();
	REQUIRE_TRUE(F.DiskFileExists(S));
	F.Sim->Held.insert(EPrefixCommandKind::RemoveDiskFile);
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(1));
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Pending);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
	EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
	EXPECT_EQ(F.DiskFileExists(S), false);
}

HOST_TEST(disk_delete_held_at_reconfigure_runs_once, "3.10 (iii) [U2] (rev 1.18.1): a delete held at a re-Configure(), then released: the file is gone, the exclusion counter shows one delete ran, and the op reads Deleted after (mutant: the delete re-run after Configure() -> two deletes counted)")
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
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	F.Sim->Held.clear();
	F.Sim->RunAll();
	REQUIRE_TRUE(F.Cache.IsTornDown());
	REQUIRE_TRUE(F.Cache.Configure(F.Config));
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->DropDeletesRun, 1u);
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
	EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
}

// ---- 3.10 rev 1.18.2 (iv)-(vii) ----------------------------------------------------------------------

namespace
{
	// Budget 5, disk on: E (L = 33, 3 pages) resident and unpinned, a 2-page entry leased, releases gated
	// behind the lane-park seam. A request for S (a 3-page build, no file) evicts E and waits on its refund.
	struct FWaiterScene
	{
		FPagesFixture F{5, true};
		FPrefixSpec S = FixtureSpec(3, 32);
		uint64_t EHandle = 0;
		int64_t H = 0;
		bool bReady = false;

		FWaiterScene()
		{
			const int64_t HE = F.DiskEntry(FixtureSpec(1, 33));
			if (!F.bConfigured || HE == 0)
			{
				return;
			}
			EHandle = F.Sim->EngineHandleOf(HE);
			F.Cache.Return(HE);
			F.Sim->Pass();
			if (F.DiskEntry(FixtureSpec(2, 32)) == 0)
			{
				return;
			}
			F.Sim->Held.insert(EPrefixCommandKind::Release);
			H = F.Request(S);
			F.Sim->RunAll();
			bReady = EHandle != 0 && H != 0 && F.Diag().Waiters == 1;
		}
	};
}

HOST_TEST(disk_drop_of_a_waiter_writes_no_file, "3.10 (iv) [U2] (rev 1.18.2, the strike's s3): the request is a waiter under deferred-refund admission at the drop's apply pass (it draws nothing, no job queued); after the drop's op resolves it is admitted and served, and no file exists under the key (mutant: the waiter's write-back not suppressed)")
{
	FWaiterScene W;
	REQUIRE_TRUE(W.bReady);
	const FPrefixKey K = FixtureKey(W.S);
	const size_t Builds = W.F.Sim->CountHistory(EPrefixCommandKind::Build);
	const uint64_t D = W.F.Drop(W.S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	W.F.Sim->RunAll();
	EXPECT_EQ(W.F.OpResult(D), EPrefixOpResult::DropDeferred);
	EXPECT_TRUE(DiskPartRan(W.F, D));
	EXPECT_EQ(W.F.Sim->CountHistory(EPrefixCommandKind::Build), Builds);
	REQUIRE_TRUE(W.F.Sim->RunRelease(W.EHandle));
	W.F.Sim->Held.clear();
	W.F.Sim->RunAll();
	EXPECT_EQ(W.F.Cache.GetLeaseState(W.H), EPrefixLeaseState::Ready);
	EXPECT_EQ(WritesOf(*W.F.Sim, K), static_cast<size_t>(0));
	EXPECT_EQ(W.F.DiskFileExists(W.S), false);
}

HOST_TEST(disk_attach_to_a_dropped_waiter_restores_write_back, "3.10 (iv) [U2] attach (rev 1.18.2): a request that attaches after the drop to the waiter restores write-back as (i) does: the file exists, written after the delete")
{
	FWaiterScene W;
	REQUIRE_TRUE(W.bReady);
	const FPrefixKey K = FixtureKey(W.S);
	const uint64_t D = W.F.Drop(W.S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	W.F.Sim->RunAll();
	REQUIRE_EQ(W.F.OpResult(D), EPrefixOpResult::DropDeferred);
	const int64_t H2 = W.F.Request(W.S); // attaches to the waiter
	REQUIRE_NE(H2, 0);
	W.F.Sim->RunAll();
	EXPECT_EQ(W.F.Diag().Waiters, 1u);
	REQUIRE_TRUE(W.F.Sim->RunRelease(W.EHandle));
	W.F.Sim->Held.clear();
	W.F.Sim->RunAll();
	EXPECT_EQ(W.F.Cache.GetLeaseState(W.H), EPrefixLeaseState::Ready);
	EXPECT_EQ(W.F.Cache.GetLeaseState(H2), EPrefixLeaseState::Ready);
	EXPECT_EQ(W.F.Diag().MarksCleared, 1u);
	EXPECT_TRUE(W.F.DiskFileExists(W.S));
	EXPECT_GT(IndexOfLast(*W.F.Sim, EPrefixCommandKind::WriteDisk, K),
		IndexOf(*W.F.Sim, [D](const FPrefixCommand& C) { return IsDropDelete(C, D); }));
}

HOST_TEST(disk_drop_behind_a_parked_save, "3.10 (v) [U2] (rev 1.18.2): the request's build has applied and its save is queued on a worker lane the lane-park seam holds, no file write issued yet: the save, then its file write, then the delete run in that order on the key's serialization, and no file exists (mutant: write-back read as the file write only -> the save's later write lands after the delete)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::Save);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Save), static_cast<size_t>(1));
	REQUIRE_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0));
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Pending); // the delete waits for the write-back
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	const size_t Save = IndexOf(*F.Sim, [](const FPrefixCommand& C) { return C.Kind == EPrefixCommandKind::Save; });
	const size_t Write = IndexOfLast(*F.Sim, EPrefixCommandKind::WriteDisk, K);
	const size_t Delete = IndexOf(*F.Sim, [D](const FPrefixCommand& C) { return IsDropDelete(C, D); });
	EXPECT_LT(Save, Write);
	EXPECT_LT(Write, Delete);
	EXPECT_LT(Delete, F.Sim->History.size());
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
	EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
}

HOST_TEST(disk_drop_of_a_build_waiting_to_retry, "3.10 (vi) [U2] (rev 1.18.2): the request's build was refused PoolExhausted and is waiting to retry, the pool short of its draw with one evictable entry whose release the lane-park seam holds: the build retries and serves, and no file exists (mutant: a retry wait not counted in flight -> a file)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->PoolPages = 4; // one resident entry (2) leaves 2: short of a build's R(32) = 3
	REQUIRE_TRUE(F.Resident(FixtureSpec(2)));
	F.Sim->RunAll();
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1)); // the retry waits on it
	REQUIRE_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Pending);
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
	EXPECT_EQ(WritesOf(*F.Sim, K), static_cast<size_t>(0));
	EXPECT_EQ(F.DiskFileExists(S), false);
	EXPECT_TRUE(DiskPartRan(F, D));
}

HOST_TEST(disk_attach_to_a_dropped_retry_restores_write_back, "3.10 (vi) [U2] attach (rev 1.18.1 (i) on (vi)'s construction; suite audit G3): a request issued after the drop resolves attaches to the build waiting to retry, and restores write-back as (i) does: after it serves, the file exists, written after the delete (mutant: the attach leaves write-back suppressed -> no file)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->PoolPages = 4; // one resident entry (2) leaves 2: short of a build's R(32) = 3
	REQUIRE_TRUE(F.Resident(FixtureSpec(2)));
	F.Sim->RunAll();
	const FPrefixSpec S = FixtureSpec(1);
	const FPrefixKey K = FixtureKey(S);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1)); // the retry waits on it
	REQUIRE_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Pending);
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	REQUIRE_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred); // the drop has resolved
	const size_t BuildsBefore = F.Sim->CountHistory(EPrefixCommandKind::Build);
	const int64_t H2 = F.Request(S); // issued after the drop resolved: attaches to the retry
	REQUIRE_NE(H2, 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Pending);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build) - BuildsBefore, static_cast<size_t>(1)); // one retry served both
	EXPECT_EQ(F.Diag().MarksCleared, 1u);
	EXPECT_TRUE(F.DiskFileExists(S));
	EXPECT_GT(IndexOfLast(*F.Sim, EPrefixCommandKind::WriteDisk, K),
		IndexOf(*F.Sim, [D](const FPrefixCommand& C) { return IsDropDelete(C, D); }));
}

HOST_TEST(disk_drop_behind_a_held_read, "3.10 (vii) [U2] (rev 1.18.2): a disk read of the key held mid-read: the read completes and serves, then the delete runs: Disk == Deleted, never Failed, and the per-key overlap counter over read, write-back and delete never reads 2 (mutant: the delete not serialized with the read)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	F.PutDiskFile(S);
	F.Sim->Held.insert(EPrefixCommandKind::ReadDisk);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::ReadDisk), static_cast<size_t>(1));
	const uint64_t D = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(D, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Pending);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Disk);
	EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
	EXPECT_EQ(F.Op(D).Disk, EPrefixDropDisk::Deleted);
	EXPECT_EQ(F.Sim->MaxKeyDiskOverlap, 1u);
	EXPECT_EQ(F.DiskFileExists(S), false);
}

// ---- 1.7, rev 1.18.2 leg -----------------------------------------------------------------------------

namespace
{
	enum class EPendingState : uint8_t
	{
		WarmHashHeld,
		WarmDiskReadHeld,
		WarmBuildQueued,
		WarmBuildRunning,
		WarmWaiter,
		DropHashHeld,
		DropDeleteBehindWriteBack,
		// Rev 1.18.5 (U2 code review findings 1 and 2), the host path only: a MemoryAndDisk drop's delete waiting on
		// a write-back's SAVE that no lane has run (a lane job never posted), a file already under the key.
		DropDeleteBehindSave,
	};

	constexpr EPendingState kPendingStates[] = {EPendingState::WarmHashHeld, EPendingState::WarmDiskReadHeld,
		EPendingState::WarmBuildQueued, EPendingState::WarmBuildRunning, EPendingState::WarmWaiter,
		EPendingState::DropHashHeld, EPendingState::DropDeleteBehindWriteBack};

	const char* StateName(EPendingState S)
	{
		switch (S)
		{
		case EPendingState::WarmHashHeld: return "warm, hash held";
		case EPendingState::WarmDiskReadHeld: return "warm, disk read held";
		case EPendingState::WarmBuildQueued: return "warm, build queued";
		case EPendingState::WarmBuildRunning: return "warm, build running";
		case EPendingState::WarmWaiter: return "warm, a waiter";
		case EPendingState::DropHashHeld: return "drop, hash held";
		case EPendingState::DropDeleteBehindWriteBack: return "MemoryAndDisk drop, delete queued behind a held write-back";
		case EPendingState::DropDeleteBehindSave: return "MemoryAndDisk drop, delete waiting on a save no lane has run";
		}
		return "?";
	}
}

HOST_TEST(disk_reconfigure_resolves_every_pending_op, "1.7 [U2] (rev 1.18.2, lesser plane 1; §5.6 Generations and teardown): re-Configure() with an op held in each Pending state (warm: hash held, disk read held, build queued, build running, a waiter; drop: hash held; MemoryAndDisk drop: delete queued behind a held write-back): no op reads Pending after; each warm Cancelled; the held-hash drop Cancelled with NotRequested; the queued delete ran once and reads Deleted; a warm's discarded build left no entry and no file (mutant: any one state left unresolved -> that op reads Pending after 1,100 later ops resolve)")
{
	for (const EPendingState State : kPendingStates)
	{
		const bool bWaiter = State == EPendingState::WarmWaiter;
		std::unique_ptr<FCacheFixture> Owned(bWaiter ? static_cast<FCacheFixture*>(new FPagesFixture(5, true)) : new FCacheFixture(4, true));
		FCacheFixture& F = *Owned;
		REQUIRE_TRUE(F.bConfigured);
		FSim& Sim = *F.Sim;
		const FPrefixSpec S = FixtureSpec(1, 32);
		uint64_t Op = 0;
		switch (State)
		{
		case EPendingState::WarmHashHeld:
			Op = F.Warm(S);
			Sim.HeldHashOps.insert(Op);
			Sim.RunAll();
			break;
		case EPendingState::WarmDiskReadHeld:
			F.PutDiskFile(S);
			Sim.Held.insert(EPrefixCommandKind::ReadDisk);
			Op = F.Warm(S);
			Sim.RunAll();
			break;
		case EPendingState::WarmBuildQueued:
			Op = F.Warm(S);
			Sim.Pull();
			REQUIRE_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::ComputeKey));
			Sim.bAutoPull = false; // the build its disk miss emits stays queued, not yet taken by a lane
			REQUIRE_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::ReadDisk));
			REQUIRE_EQ(Sim.CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
			break;
		case EPendingState::WarmBuildRunning:
			Sim.Held.insert(EPrefixCommandKind::Build);
			Op = F.Warm(S);
			Sim.RunAll();
			break;
		case EPendingState::WarmWaiter:
		{
			FPagesFixture& P = static_cast<FPagesFixture&>(F);
			const int64_t HE = P.DiskEntry(FixtureSpec(5, 33));
			REQUIRE_NE(HE, 0);
			P.Cache.Return(HE);
			REQUIRE_NE(P.DiskEntry(FixtureSpec(6, 32)), 0);
			Sim.Held.insert(EPrefixCommandKind::Release);
			Op = F.Warm(S);
			Sim.RunAll();
			REQUIRE_EQ(F.Diag().Waiters, 1u);
			break;
		}
		case EPendingState::DropHashHeld:
			REQUIRE_TRUE(F.Resident(S));
			Sim.RunAll();
			Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
			Sim.HeldHashOps.insert(Op);
			Sim.RunAll();
			break;
		case EPendingState::DropDeleteBehindWriteBack:
			Sim.Held.insert(EPrefixCommandKind::WriteDisk);
			REQUIRE_TRUE(F.Resident(S));
			Sim.Pass();
			Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
			Sim.RunAll();
			REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
			break;
		case EPendingState::DropDeleteBehindSave: // the host-path leg's state only (below)
			break;
		}
		REQUIRE_NE(Op, 0u);
		REQUIRE_EQ(F.OpResult(Op), EPrefixOpResult::Pending);

		// The re-Configure(): teardown, the holds released (host reading, see the file comment), Configure().
		F.Cache.BeginTeardown();
		if (State != EPendingState::DropDeleteBehindWriteBack)
		{
			EXPECT_NE(F.OpResult(Op), EPrefixOpResult::Pending); // resolved during it
		}
		Sim.bAutoPull = true;
		Sim.Held.clear();
		Sim.HeldHashOps.clear();
		Sim.HeldHashLeases.clear();
		Sim.RunAll();
		// "Left no entry" is carried here: IsTornDown() requires the entry map empty (suite audit G5).
		REQUIRE_TRUE(F.Cache.IsTornDown());
		// 1.7's base operands (rev 1.2) at the teardown point (suite audit G5): every engine handle the
		// run created, the discarded build's included, released exactly once, and the fill probe at its
		// initial count.
		EXPECT_TRUE(F.EveryHandleReleasedOnce());
		EXPECT_EQ(Sim.CacheDrawnPages, 0u);
		REQUIRE_TRUE(F.Cache.Configure(F.Config));
		Sim.RunAll();

		const FPrefixOpResult R = F.Op(Op);
		EXPECT_NE(R.Result, EPrefixOpResult::Pending);
		switch (State)
		{
		case EPendingState::DropHashHeld:
			EXPECT_EQ(R.Result, EPrefixOpResult::Cancelled);
			EXPECT_EQ(R.Disk, EPrefixDropDisk::NotRequested);
			EXPECT_TRUE(F.DiskFileExists(S)); // neither part ran
			break;
		case EPendingState::DropDeleteBehindWriteBack:
			EXPECT_EQ(R.Result, EPrefixOpResult::Dropped); // the memory part's result kept
			EXPECT_EQ(R.Disk, EPrefixDropDisk::Deleted);
			EXPECT_EQ(Sim.DropDeletesRun, 1u);
			EXPECT_EQ(Sim.MaxKeyDiskOverlap, 1u);
			EXPECT_EQ(F.DiskFileExists(S), false);
			break;
		default:
			EXPECT_EQ(R.Result, EPrefixOpResult::Cancelled);
			if (State != EPendingState::WarmDiskReadHeld)
			{
				EXPECT_EQ(F.DiskFileExists(S), false); // a discarded build writes nothing
			}
			break;
		}
		EXPECT_TRUE(Sim.Violations.empty());
		// 1,100 later ops resolve: a resolved op has left the ring (Expired); an unresolved one would read Pending.
		for (int32_t I = 0; I < 1100; ++I)
		{
			F.Drop(FixtureSpec(20000 + I));
		}
		Sim.RunAll(10000);
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Expired);
		(void)StateName(State);
	}
}

HOST_TEST(disk_host_reconfigure_resolves_every_pending_op, "1.7 [U2] (rev 1.18.5, U2 code review findings 1 and 2): the same re-Configure() with an op held in each Pending state, through the host's real path (ReleaseForPoolDestroy then Abandon(), then Configure() and its first Pump; never BeginTeardown(), which the CPU host does not call), plus the state that path adds: a MemoryAndDisk drop's delete waiting on a write-back's save no lane has run, a file already under the key. No op reads Pending after; each warm Cancelled; the held-hash drop Cancelled with NotRequested; each delete drained under the old configuration, run once, reading Deleted, the file gone; every engine handle released once and the fill probe at 0 (mutant: the delete waiting on the save carried into the next Configure() -> it reads Pending through the teardown and the file is left)")
{
	constexpr EPendingState kHostStates[] = {EPendingState::WarmHashHeld, EPendingState::WarmDiskReadHeld,
		EPendingState::WarmBuildQueued, EPendingState::WarmBuildRunning, EPendingState::WarmWaiter,
		EPendingState::DropHashHeld, EPendingState::DropDeleteBehindWriteBack, EPendingState::DropDeleteBehindSave};
	for (const EPendingState State : kHostStates)
	{
		std::printf("  1.7 host path: %s\n", StateName(State));
		const bool bWaiter = State == EPendingState::WarmWaiter;
		std::unique_ptr<FCacheFixture> Owned(bWaiter ? static_cast<FCacheFixture*>(new FPagesFixture(5, true)) : new FCacheFixture(4, true));
		FCacheFixture& F = *Owned;
		REQUIRE_TRUE(F.bConfigured);
		FSim& Sim = *F.Sim;
		const FPrefixSpec S = FixtureSpec(1, 32);
		uint64_t Op = 0;
		switch (State)
		{
		case EPendingState::WarmHashHeld:
			Op = F.Warm(S);
			Sim.HeldHashOps.insert(Op);
			Sim.RunAll();
			break;
		case EPendingState::WarmDiskReadHeld:
			F.PutDiskFile(S);
			Sim.Held.insert(EPrefixCommandKind::ReadDisk);
			Op = F.Warm(S);
			Sim.RunAll();
			break;
		case EPendingState::WarmBuildQueued:
			Op = F.Warm(S);
			Sim.Pull();
			REQUIRE_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::ComputeKey));
			Sim.bAutoPull = false;
			REQUIRE_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::ReadDisk));
			REQUIRE_EQ(Sim.CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
			Sim.bAutoPull = true;
			break;
		case EPendingState::WarmBuildRunning:
			Sim.Held.insert(EPrefixCommandKind::Build);
			Op = F.Warm(S);
			Sim.RunAll();
			break;
		case EPendingState::WarmWaiter:
		{
			FPagesFixture& P = static_cast<FPagesFixture&>(F);
			const int64_t HE = P.DiskEntry(FixtureSpec(5, 33));
			REQUIRE_NE(HE, 0);
			P.Cache.Return(HE);
			REQUIRE_NE(P.DiskEntry(FixtureSpec(6, 32)), 0);
			Sim.Held.insert(EPrefixCommandKind::Release);
			Op = F.Warm(S);
			Sim.RunAll();
			REQUIRE_EQ(F.Diag().Waiters, 1u);
			break;
		}
		case EPendingState::DropHashHeld:
			REQUIRE_TRUE(F.Resident(S));
			Sim.RunAll();
			Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
			Sim.HeldHashOps.insert(Op);
			Sim.RunAll();
			break;
		case EPendingState::DropDeleteBehindWriteBack:
			Sim.Held.insert(EPrefixCommandKind::WriteDisk);
			REQUIRE_TRUE(F.Resident(S));
			Sim.Pass();
			Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
			Sim.RunAll();
			REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
			break;
		case EPendingState::DropDeleteBehindSave:
			Sim.Held.insert(EPrefixCommandKind::Save); // queued for a lane, not posted
			REQUIRE_NE(F.RequestReady(S), 0);
			REQUIRE_EQ(Sim.CountQueued(EPrefixCommandKind::Save), static_cast<size_t>(1));
			F.PutDiskFile(S); // a file already under the key (another PIE instance, or an unreadable file)
			Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
			Sim.RunAll();
			REQUIRE_EQ(F.Cache.PendingDropDeletes(), 1u);
			break;
		}
		REQUIRE_NE(Op, 0u);
		REQUIRE_EQ(F.OpResult(Op), EPrefixOpResult::Pending);

		const FPrefixCacheConfig Same = F.Config;
		REQUIRE_TRUE(F.HostReconfigure(&Same));
		EXPECT_TRUE(F.EveryHandleReleasedOnce());
		EXPECT_EQ(Sim.CacheDrawnPages, 0u);
		const bool bDelete = State == EPendingState::DropDeleteBehindWriteBack || State == EPendingState::DropDeleteBehindSave;
		if (bDelete)
		{
			// Drained under the old configuration, before Configure() returned: nothing left to run after it.
			EXPECT_EQ(Sim.DropDeletesRun, 1u);
			EXPECT_EQ(F.DiskFileExists(S), false);
		}
		Sim.Held.clear();
		Sim.HeldHashOps.clear(); // the held hash completes now, under the old generation: stale
		Sim.HeldHashLeases.clear();
		Sim.RunAll();

		const FPrefixOpResult R = F.Op(Op);
		EXPECT_NE(R.Result, EPrefixOpResult::Pending);
		switch (State)
		{
		case EPendingState::DropHashHeld:
			EXPECT_EQ(R.Result, EPrefixOpResult::Cancelled);
			EXPECT_EQ(R.Disk, EPrefixDropDisk::NotRequested);
			EXPECT_TRUE(F.DiskFileExists(S));
			break;
		case EPendingState::DropDeleteBehindWriteBack:
		case EPendingState::DropDeleteBehindSave:
			EXPECT_EQ(R.Result, State == EPendingState::DropDeleteBehindSave ? EPrefixOpResult::DropDeferred : EPrefixOpResult::Dropped);
			EXPECT_EQ(R.Disk, EPrefixDropDisk::Deleted);
			EXPECT_EQ(Sim.DropDeletesRun, 1u); // not re-run after Configure()
			EXPECT_EQ(F.DiskFileExists(S), false);
			EXPECT_LE(Sim.MaxKeyDiskOverlap, 1u);
			break;
		default:
			EXPECT_EQ(R.Result, EPrefixOpResult::Cancelled);
			if (State != EPendingState::WarmDiskReadHeld)
			{
				EXPECT_EQ(F.DiskFileExists(S), false);
			}
			break;
		}
		EXPECT_TRUE(Sim.Violations.empty());
	}
}
