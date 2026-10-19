// Re-Configure and teardown with work in flight, and the disk tier across lifetimes
// (precache plan §5.4 last bullet, §5.7; cells 1.6, 1.7, 3.7).

#include "cache_fixture.h"

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	bool EveryHandleReleasedOnce(const FSim& Sim)
	{
		for (const auto& Pair : Sim.Handles)
		{
			const auto It = Sim.ReleaseCount.find(Pair.first);
			if (It == Sim.ReleaseCount.end() || It->second != 1)
			{
				return false;
			}
		}
		return !Sim.Handles.empty();
	}
}

HOST_TEST(cache_teardown_waits_for_a_running_restore, "1.7 (restore running on a worker lane)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t Resident = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(Resident, 0);
	const FPrefixSpec S = FixtureSpec(2);
	F.PutDiskFile(S);
	F.Sim->Held = {EPrefixCommandKind::Restore};
	const int64_t H = F.Request(S);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Restore), static_cast<size_t>(1)); // taken: in flight
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Cancelled); // resolves by name
	F.Sim->RunAll();
	EXPECT_GE(F.Cache.InFlightCommands(), 1u);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0)); // nothing released under it
	EXPECT_EQ(F.Cache.IsTornDown(), false);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->Handles.size(), static_cast<size_t>(2)); // the resident entry and the restored one
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_teardown_drops_a_queued_restore, "1.7 (restore queued, not yet on a lane)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(2);
	F.PutDiskFile(S);
	const int64_t H = F.Request(S);
	REQUIRE_NE(H, 0);
	F.Sim->Pull();
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::ComputeKey));
	F.Sim->bAutoPull = false;
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::ReadDisk)); // its completion makes the core emit Restore
	F.Cache.BeginTeardown();                                          // before the executor takes it
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Cancelled);
	F.Sim->bAutoPull = true;
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Restore), static_cast<size_t>(0)); // never issued
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::DropPayload), static_cast<size_t>(1)); // its payload freed
	EXPECT_EQ(F.Sim->Payloads.empty(), true);
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
}

HOST_TEST(cache_teardown_waits_for_a_running_build, "1.7 (build running on a worker lane)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::Build};
	const int64_t H = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Cancelled);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), false);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	REQUIRE_EQ(F.Sim->Handles.size(), static_cast<size_t>(1));
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true); // the build's handle, delivered during teardown, released once
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	EXPECT_EQ(F.Cache.Configure(F.Config), true);
}

HOST_TEST(cache_teardown_with_disk_reads_and_writes_in_flight, "3.7 (host half: bookkeeping and files)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::WriteDisk};
	const FPrefixSpec Written = FixtureSpec(1);
	const int64_t W = F.Request(Written);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Ready);
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
	const FPrefixSpec Read = FixtureSpec(2);
	F.PutDiskFile(Read);
	F.Sim->Held.insert(EPrefixCommandKind::ReadDisk);
	const int64_t R = F.Request(Read);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::ReadDisk), static_cast<size_t>(1));
	EXPECT_GE(F.Cache.InFlightCommands(), 2u); // counted > 0 at the call
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(R), EPrefixLeaseState::Cancelled);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Restore), static_cast<size_t>(0)); // the read was not restored
	EXPECT_EQ(F.Sim->Payloads.empty(), true);                                          // and its bytes were dropped
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	for (const std::string& Name : F.Sim->Fs->NamesIn("cache"))
	{
		EXPECT_EQ(IsPrefixDiskFileName(Name, nullptr), true); // no partial file, no temporary
	}
	const auto It = F.Sim->Fs->Files.find(F.Sim->DiskPath(FixtureKey(Written)));
	if (It != F.Sim->Fs->Files.end())
	{
		size_t Off = 0;
		size_t N = 0;
		EXPECT_EQ(CheckPrefixWrapper(It->second.Data.data(), It->second.Data.size(), F.Config.Identity, FixtureKey(Written),
			StoredOf(Written), Off, N), EPrefixWrapperCheck::Ok);
	}
}

HOST_TEST(cache_disk_tier_warm_across_two_lifetimes, "1.6")
{
	FMemFs SharedFs;
	const FPrefixSpec Spec = FixtureSpec(1, 32, "quest");
	{
		FCacheFixture First(4, true);
		REQUIRE_TRUE(First.bConfigured);
		First.Sim->Fs = &SharedFs;
		REQUIRE_NE(First.RequestReady(Spec), 0);
		First.Cache.BeginTeardown();
		First.Sim->RunAll();
		REQUIRE_TRUE(First.Cache.IsTornDown());
	}
	FCacheFixture Second(4, true);
	REQUIRE_TRUE(Second.bConfigured);
	Second.Sim->Fs = &SharedFs;
	const int64_t H = Second.RequestReady(Spec);
	REQUIRE_NE(H, 0);
	EXPECT_EQ(Second.Cache.GetSource(H), EPrefixSource::Disk);
	const FEngineHandle* Handle = Second.Sim->HandleOf(H);
	REQUIRE_TRUE(Handle != nullptr);
	EXPECT_EQ(Handle->Tokens == Spec.Tokens, true);
	EXPECT_EQ(Handle->Schema, Spec.SchemaName);
	EXPECT_EQ(Second.Diag().BuildsIssued, 0u);
}

HOST_TEST(cache_teardown_waits_for_an_open_adopt, "1.7 (an adopt open at teardown), 3.6 (an adopt pins its entry)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	uint64_t Engine = 0;
	uint64_t Token = 0;
	REQUIRE_EQ(F.Cache.BeginAdopt(A, Engine, Token), EPrefixAdoptCheck::Adoptable);
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), false);
	EXPECT_EQ(F.Sim->Handles[Engine].bReleased, false); // not released under the adopt
	EXPECT_EQ(F.Cache.EndAdopt(Token), true);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_teardown_with_a_save_in_flight, "1.7 (a save running at teardown), 3.7 (host half: no write-back after teardown)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::Save};
	const int64_t A = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetLeaseState(A), EPrefixLeaseState::Ready);
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Save), static_cast<size_t>(1));
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), false);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0)); // not released under the save
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0));
	EXPECT_EQ(F.Sim->Payloads.empty(), true); // the saved bytes were dropped
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_teardown_cancels_a_deferred_admission, "1.7 (an admission waiting on a release at teardown)")
{
	FCacheFixture F(1, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	REQUIRE_TRUE(F.Cache.Return(A));
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t B = F.Request(FixtureSpec(2));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::Pending);
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::Cancelled);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1)); // B never built
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Diag().ChargedBytes, 0u);
}

HOST_TEST(cache_teardown_cancels_a_pool_retry, "1.7 (a build waiting to retry after PoolExhausted at teardown)")
{
	FCacheFixture F(4, false, 0);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	F.Cache.Return(A);
	F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 2; // a build needs 3
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t C = F.Request(FixtureSpec(3));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1));
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::Cancelled);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2)); // A, and C's refused try; no retry
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_build_landing_in_teardown_is_not_saved, "1.7 (a build running at teardown, disk on), 3.7 (host half)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::Build};
	F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	F.Cache.BeginTeardown();
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Save), static_cast<size_t>(0));
	EXPECT_EQ(EveryHandleReleasedOnce(*F.Sim), true);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_teardown_sweep_ends_ready_leases_by_name, "1.5 (teardown releases every handle), 2.10 (no verb reaches a released handle)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Cache.GetLeaseState(A), EPrefixLeaseState::Cancelled); // its handle was released
	uint64_t Engine = 0;
	uint64_t Token = 0;
	EXPECT_EQ(F.Cache.BeginAdopt(A, Engine, Token), EPrefixAdoptCheck::NotReady);
	EXPECT_EQ(F.Cache.Return(A), true); // the caller may still return it, once
	EXPECT_EQ(F.Cache.Return(A), false);
}
