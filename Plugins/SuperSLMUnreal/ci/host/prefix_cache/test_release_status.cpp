// Cell 5.15 (precache plan rev 1.18.1-1.18.2, §5.4 *What a release's status means*; plan review F1, audit
// G13 (a)): a release that returned has applied. Host half, through the whole cache core and the simulator.
//
// The engine consumes the handle on every non-OK release status (sslm_prefix_release always; the GPU release
// on every status but SSLM_BUSY). So:
//   - CPU leg: the release-status substitution seam (§8, FSim::ConsumeThenFailReleases) runs the real release
//     and then reports a page-module refusal (SSLM_INVALID_ARGUMENT 1, SSLM_ARTIFACT_REJECTED 4). One release
//     of the handle and never a second; ReleaseFailed logged once; the family's pending refund completes at
//     that apply, and the waiter claiming it is admitted at that apply pass; the teardown completes within a
//     watchdog bound with every handle released once (the pool destroy's SSLM_OK).
//   - GPU legs ([U6] on the box; here the core's half of them, since the core is backend-neutral): the busy-release
//     substitution seam (§8, rev 1.18.2, FSim::BusyReleases) returns SSLM_BUSY without releasing. The release is
//     issued again at a later tick, its refund pending until that issue returns, the handle released once in
//     all. A GPU release returning any other status (the consumed refcount underflow's
//     SSLM_SEQUENCE_KV_BUFFER_MISMATCH, 7) is not re-issued.
//
// Red first against the U2 draft's V12 rule (Wizard e7811590e; OnRelease unchanged at this commit): it
// re-issues every non-OK release, so the CPU and GPU non-busy legs release a freed handle a second time (the
// release-counting seam reads 2, and the simulator records "Release of an engine handle already released"),
// and hold the refund and the waiter until then. The busy leg is green on the draft: the draft re-issues
// there too, which is the rule.
//
// The seams' own must-accept and must-reject (§8) are in test_sim_seams.cpp.

#include "cache_fixture.h"

using namespace HostSim;

namespace
{
	constexpr uint64_t kPage = kFixturePageBytes;

	// Budget 5 pages, disk on. E (L = 33, a 3-page restore) is resident and unpinned; a second entry (2 pages)
	// is leased. Releases are gated. G (a 3-page build) evicts E and waits on E's refund, claiming all of it.
	struct FReleaseScene
	{
		FPagesFixture F{5, true};
		int64_t Leased = 0;
		int64_t HG = 0;
		uint64_t EHandle = 0;
		bool bReady = false;

		FReleaseScene()
		{
			const int64_t HE = F.DiskEntry(FixtureSpec(1, 33));
			if (!F.bConfigured || HE == 0)
			{
				return;
			}
			EHandle = F.Sim->EngineHandleOf(HE);
			F.Cache.Return(HE);
			Leased = F.DiskEntry(FixtureSpec(2, 32));
			if (EHandle == 0 || Leased == 0 || F.Diag().ChargedBytes != 5 * kPage)
			{
				return;
			}
			F.Sim->Held.insert(EPrefixCommandKind::Release);
			HG = F.Request(FixtureSpec(3, 32));
			F.Sim->RunAll();
			const FPrefixCacheDiagnostics D = F.Diag();
			F.Cache.TakeLog();
			bReady = HG != 0 && D.Waiters == 1 && D.ReleasingBytes == 3 * kPage && D.ClaimedRefundBytes == 3 * kPage &&
				F.Sim->CountQueued(EPrefixCommandKind::Release) == 1;
		}

		int Releases() const
		{
			const auto It = F.Sim->ReleaseCount.find(EHandle);
			return It == F.Sim->ReleaseCount.end() ? 0 : It->second;
		}
	};

	// The assertions every non-BUSY leg shares: the release returned, so it has applied.
	void ExpectAppliedAtThatPass(FReleaseScene& S)
	{
		FPrefixCacheDiagnostics D = S.F.Diag();
		EXPECT_EQ(S.Releases(), 1);
		EXPECT_EQ(D.ReleasingBytes, 0u);                                                  // the refund completed at that apply
		EXPECT_EQ(D.PendingRefunds, 0u);
		EXPECT_EQ(D.Waiters, 0u);                                                         // the waiter admitted at that apply pass
		EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
		EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(0)); // not issued again
		EXPECT_EQ(S.F.Fallbacks(EPrefixFallback::ReleaseFailed), 1u);
		EXPECT_EQ(S.F.LogLinesWith(S.F.Cache.TakeLog(), EPrefixFallback::ReleaseFailed), static_cast<size_t>(1));
		EXPECT_TRUE(S.F.LedgerOk());

		// Every gate opens: never a second release of the consumed handle.
		S.F.Sim->Held.clear();
		S.F.Sim->RunAll();
		EXPECT_EQ(S.Releases(), 1);
		EXPECT_TRUE(S.F.Sim->Violations.empty());
		EXPECT_EQ(S.F.Cache.GetLeaseState(S.HG), EPrefixLeaseState::Ready);
		EXPECT_LE(S.F.Sim->PeakCacheDrawnPages, 5u);
		EXPECT_EQ(S.F.Fallbacks(EPrefixFallback::ReleaseFailed), 1u);                    // logged once, never a retry
		EXPECT_TRUE(S.F.LedgerOk());

		// Teardown: the pool destroy returns SSLM_OK (no live handle) within the watchdog bound.
		EXPECT_TRUE(S.F.TeardownWithin());
		EXPECT_TRUE(S.F.EveryHandleReleasedOnce());
		EXPECT_EQ(S.F.Sim->CacheDrawnPages, 0u);
		EXPECT_TRUE(S.F.Sim->Violations.empty());
	}
}

HOST_TEST(release_status_cpu_refusal_has_applied, "5.15 [U2] CPU leg: a release that returned SSLM_INVALID_ARGUMENT or SSLM_ARTIFACT_REJECTED after consuming the handle has applied: one release, ReleaseFailed once, the refund completes and the waiter is admitted at that apply, teardown within the watchdog")
{
	for (const int32_t Status : {1, 4})
	{
		FReleaseScene S;
		REQUIRE_TRUE(S.bReady);
		S.F.Sim->ConsumeThenFailReleases = 1;
		S.F.Sim->ReleaseFailStatus = Status;
		REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
		ExpectAppliedAtThatPass(S);
	}
}

HOST_TEST(release_status_gpu_busy_is_reissued_until_it_applies, "5.15 [U6 GPU leg, core half]: SSLM_BUSY (busy-release substitution seam, the handle untouched) is issued again at a later tick, its refund pending until that issue returns; the handle released exactly once in all")
{
	FReleaseScene S;
	REQUIRE_TRUE(S.bReady);
	S.F.Sim->BusyReleases = 2;
	for (uint32_t Issue = 1; Issue <= 2; ++Issue)
	{
		REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
		const FPrefixCacheDiagnostics D = S.F.Diag();
		EXPECT_EQ(S.F.Sim->BusyIssues, Issue);
		EXPECT_EQ(S.Releases(), 0);                                                       // SSLM_BUSY: nothing released
		EXPECT_EQ(D.ReleasingBytes, 3 * kPage);                                           // the refund still pending
		EXPECT_EQ(D.ClaimedRefundBytes, 3 * kPage);
		EXPECT_EQ(D.Waiters, 1u);
		EXPECT_EQ(S.F.Cache.GetLeaseState(S.HG), EPrefixLeaseState::Pending);
		EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(0));
		EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1)); // issued again, for a later tick
		EXPECT_EQ(S.F.Sim->CacheDrawnPages, 5u);                                         // E's pages are still held
		EXPECT_TRUE(S.F.LedgerOk());
	}
	// The third issue returns SSLM_OK: the refund completes there and the waiter is admitted.
	REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
	EXPECT_EQ(S.Releases(), 1);
	EXPECT_EQ(S.F.Diag().ReleasingBytes, 0u);
	EXPECT_EQ(S.F.Diag().Waiters, 0u);
	EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	S.F.Sim->Held.clear();
	S.F.Sim->RunAll();
	EXPECT_EQ(S.Releases(), 1);
	EXPECT_EQ(S.F.Cache.GetLeaseState(S.HG), EPrefixLeaseState::Ready);
	EXPECT_TRUE(S.F.Sim->Violations.empty());
	EXPECT_TRUE(S.F.TeardownWithin());
	EXPECT_TRUE(S.F.EveryHandleReleasedOnce());
	EXPECT_EQ(S.F.Sim->CacheDrawnPages, 0u);
}

HOST_TEST(release_status_gpu_other_status_is_not_reissued, "5.15 [U6 GPU leg, core half]: a GPU release returning a status other than SSLM_BUSY (SSLM_SEQUENCE_KV_BUFFER_MISMATCH, a consumed refcount underflow) has applied and is not re-issued")
{
	FReleaseScene S;
	REQUIRE_TRUE(S.bReady);
	S.F.Sim->ConsumeThenFailReleases = 1;
	S.F.Sim->ReleaseFailStatus = 7;
	REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
	ExpectAppliedAtThatPass(S);
}
