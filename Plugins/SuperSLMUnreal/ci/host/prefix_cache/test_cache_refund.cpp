// Cell 8.14's host half (precache plan rev 1.17 §5.11 (a), (b)): the cache on a deferred refund,
// through the whole cache core and the simulator. Releases are held (the test-side batch gate), so
// every reading between an eviction decision and its release's apply is taken, never skipped.
//
// Fixture: B = 16, cap = 64, one page = 4096 bytes. A restore of L tokens is charged ceil(L/16)
// pages; a build is charged R(L) = min(ceil(L/16) + 1, 4) and settles to ceil(L/16).
//   L = 33 -> restore 3 (the plan's E at L = 2B + 1), L = 32 -> restore 2, build 3,
//   L = 16 -> restore 1, build 2,      L = 48 -> build 4.

#include "cache_fixture.h"

using namespace HostSim;

namespace
{
	constexpr uint64_t kPage = kFixturePageBytes;

	FPrefixCacheConfig PagesConfig(uint64_t Pages)
	{
		FPrefixCacheConfig C = FixtureConfig(1, true);
		C.BudgetBytes = Pages * kPage;
		return C;
	}

	// A fixture whose budget is exactly Pages, with the disk tier on (restores come from files).
	struct FRefundFixture : FCacheFixture
	{
		explicit FRefundFixture(uint64_t Pages) : FCacheFixture(1, true)
		{
			Config = PagesConfig(Pages);
			Cache.BeginTeardown();
			Sim->RunAll();
			bConfigured = Cache.IsTornDown() && Cache.Configure(Config);
			Sim->Config = Config;
		}

		// Puts a file for Spec and requests it to Ready (a disk restore of ceil(L/16) pages); the
		// lease is returned when bReturn, so the entry is evictable.
		int64_t DiskEntry(const FPrefixSpec& Spec, bool bReturn)
		{
			PutDiskFile(Spec);
			const int64_t H = RequestReady(Spec);
			if (H != 0 && bReturn)
			{
				Cache.Return(H);
			}
			return H;
		}

		bool LedgerOk() const
		{
			const FPrefixCacheDiagnostics D = Diag();
			return D.ChargedBytes == D.ResidentBytes + D.InFlightBytes + D.ReleasingBytes + D.WaitingBytes &&
				D.ChargedBytes <= D.BudgetBytes && D.ClaimedRefundBytes + D.UnclaimedRefundBytes == D.ReleasingBytes;
		}
	};
}

HOST_TEST(cache_refund_eviction_leg_reading_between_decision_and_apply, "8.14 eviction leg (host half): one reading between the eviction decision and the release's apply; admitted at the apply with its charge complete; exactly one eviction; F still a Memory hit")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1, 33);
	const FPrefixSpec Fs = FixtureSpec(2, 32);
	const FPrefixSpec G = FixtureSpec(3, 32);
	const int64_t HE = F.DiskEntry(E, false);
	REQUIRE_NE(HE, 0);
	// Two adopts of E (the subsystem's one adopt entry point opens and closes each).
	REQUIRE_NE(F.Sim->EngineHandleOf(HE), 0u);
	REQUIRE_NE(F.Sim->EngineHandleOf(HE), 0u);
	F.Cache.Return(HE);
	REQUIRE_NE(F.DiskEntry(Fs, true), 0);
	REQUIRE_EQ(F.Diag().ChargedBytes, 5 * kPage);
	REQUIRE_EQ(F.Sim->CacheDrawnPages, 5u);

	F.Sim->Held.insert(EPrefixCommandKind::Release); // the batch gate on E's release
	const uint64_t BuildsBefore = F.Sim->CountHistory(EPrefixCommandKind::Build);
	const int64_t HG = F.Request(G);
	REQUIRE_NE(HG, 0);
	F.Sim->RunAll();

	// The required reading: E is out of the map, C still carries E's 3 pages, G is a waiter that has
	// drawn nothing and has no job.
	FPrefixCacheDiagnostics D = F.Diag();
	EXPECT_EQ(D.Evictions, 1u);
	EXPECT_EQ(D.Waiters, 1u);
	EXPECT_EQ(D.ReleasingBytes, 3 * kPage);
	EXPECT_EQ(D.ClaimedRefundBytes, 3 * kPage);
	EXPECT_EQ(D.WaitingBytes, 0u);
	EXPECT_EQ(D.ChargedBytes, 5 * kPage);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), BuildsBefore);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CacheDrawnPages, 5u); // E's pages are still held
	EXPECT_EQ(F.Cache.GetLeaseState(HG), EPrefixLeaseState::Pending);
	EXPECT_TRUE(F.LedgerOk());

	// The gate opens: at the apply of E's release G is admitted with R(32) = 3 pages, and draws.
	F.Sim->Held.erase(EPrefixCommandKind::Release);
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::Release));
	D = F.Diag();
	EXPECT_EQ(D.Waiters, 0u);
	EXPECT_EQ(D.InFlightBytes, 3 * kPage);
	EXPECT_EQ(D.ChargedBytes, 5 * kPage); // the transfer leaves the total unchanged
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(HG), EPrefixLeaseState::Ready);
	const int64_t HF2 = F.RequestReady(Fs);
	EXPECT_EQ(F.Cache.GetSource(HF2), EPrefixSource::Memory);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 5u);
	EXPECT_TRUE(F.Sim->Violations.empty());
}

HOST_TEST(cache_refund_refused_with_nothing_evictable_and_nothing_pending, "8.14 refused; 5.5: every entry leased, no eviction pending -> BudgetExhausted, nothing drawn")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), false), 0);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), false), 0);
	const uint64_t Drawn = F.Sim->CacheDrawnPages;
	const int64_t H = F.Request(FixtureSpec(3, 32));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Diag().Evictions, 0u);
	EXPECT_EQ(F.Diag().Waiters, 0u);
	EXPECT_EQ(F.Sim->CacheDrawnPages, Drawn);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::BudgetExhausted), 1u);
}

HOST_TEST(cache_refund_two_waiters_claim_their_own_victims, "8.14 waiters: free charge 0, two victims of equal charge, each need equal to it -> two evictions, each admitted at its own victim's apply")
{
	FRefundFixture F(4);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 32), true), 0); // V1, 2 pages, LRU
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), true), 0); // V2, 2 pages
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t W1 = F.Request(FixtureSpec(3, 16)); // build, 2 pages
	const int64_t W2 = F.Request(FixtureSpec(4, 16));
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().Evictions, 2u);
	EXPECT_EQ(F.Diag().Waiters, 2u);
	EXPECT_EQ(F.Diag().UnclaimedRefundBytes, 0u);
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::Release)); // V1's
	EXPECT_EQ(F.Diag().Waiters, 1u);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_LE(F.Diag().ChargedBytes, 4 * kPage);
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::Release)); // V2's
	EXPECT_EQ(F.Diag().Waiters, 0u);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(W1), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(W2), EPrefixLeaseState::Ready);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 4u);
}

HOST_TEST(cache_refund_waiters_admitted_in_one_pass_are_enqueued_in_arrival_order, "§5.11 (a): two waiters completed in the same pass are enqueued in arrival order")
{
	FRefundFixture F(4);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 32), true), 0);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), true), 0);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const FPrefixSpec S1 = FixtureSpec(3, 16);
	const FPrefixSpec S2 = FixtureSpec(4, 16);
	F.Request(S1); // arrives first, claims V1
	F.Request(S2); // claims V2
	F.Sim->RunAll();
	REQUIRE_EQ(F.Diag().Waiters, 2u);
	// One apply pass applies V2's release, then V1's; the commands are taken after the pass.
	F.Sim->bAutoPull = false;
	std::vector<FPrefixCommand> Releases;
	for (const FPrefixCommand& C : F.Sim->Queue)
	{
		if (C.Kind == EPrefixCommandKind::Release)
		{
			Releases.push_back(C);
		}
	}
	REQUIRE_EQ(Releases.size(), static_cast<size_t>(2));
	FPrefixCompletion Done2;
	FPrefixCompletion Done1;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done1)); // V1's command (queued first)
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Done2));
	F.Cache.Apply(Done2);
	F.Cache.Apply(Done1);
	std::vector<std::vector<int32_t>> BuildTokens;
	for (const FPrefixCommand& C : F.Cache.TakeCommands())
	{
		if (C.Kind == EPrefixCommandKind::Build)
		{
			BuildTokens.push_back(C.Spec.Tokens);
		}
	}
	REQUIRE_EQ(BuildTokens.size(), static_cast<size_t>(2));
	EXPECT_TRUE(BuildTokens[0] == S1.Tokens);
	EXPECT_TRUE(BuildTokens[1] == S2.Tokens);
}

HOST_TEST(cache_refund_unclaimed_E_W_X, "8.14 unclaimed refunds (i) E/W/X: W evicts E and claims 2, X claims the unclaimed 1; both admitted at E's apply; one eviction")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), true), 0);  // E, 3 pages, the only unleased entry
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), false), 0); // leased, 2 pages
	const FPrefixSpec X = FixtureSpec(4, 16);
	F.PutDiskFile(X); // X restores: 1 page
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t HW = F.Request(FixtureSpec(3, 16)); // W builds: 2 pages
	const int64_t HX = F.Request(X);
	F.Sim->RunAll();
	FPrefixCacheDiagnostics D = F.Diag();
	EXPECT_EQ(D.Evictions, 1u);
	EXPECT_EQ(D.Waiters, 2u);
	EXPECT_EQ(D.ClaimedRefundBytes, 3 * kPage);
	EXPECT_EQ(D.UnclaimedRefundBytes, 0u);
	EXPECT_TRUE(F.LedgerOk());
	F.Sim->Held.clear();
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::Release));
	EXPECT_EQ(F.Diag().Waiters, 0u);
	EXPECT_EQ(F.Diag().InFlightBytes, 3 * kPage);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(HW), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(HX), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(HX), EPrefixSource::Disk);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 5u);
}

HOST_TEST(cache_refund_orphaned_refund_is_claimed_by_a_later_request, "8.14 unclaimed refunds (ii): Y evicts E, falls short, BudgetExhausted; E's eviction stands; Z claims E's refund and is admitted at E's apply")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), true), 0);  // E, 3 pages
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), false), 0); // leased, 2 pages
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t HY = F.Request(FixtureSpec(3, 48)); // build, 4 pages
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(HY), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_EQ(F.Diag().UnclaimedRefundBytes, 3 * kPage);
	const int64_t HZ = F.Request(FixtureSpec(4, 32)); // build, 3 pages
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().Waiters, 1u);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(HZ), EPrefixLeaseState::Ready);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 5u);
}

HOST_TEST(cache_refund_a_request_for_a_waiting_key_attaches, "8.14 / §5.11 (a) coalescing: a second request for a waiting key attaches to the waiter; one build")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), true), 0);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), true), 0);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const FPrefixSpec G = FixtureSpec(3, 32);
	const int64_t H1 = F.Request(G);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Diag().Waiters, 1u);
	const uint64_t Builds = F.Diag().BuildsIssued;
	const int64_t H2 = F.Request(G);
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().Waiters, 1u);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().BuildsIssued, Builds + 1);
	EXPECT_EQ(F.Cache.GetLeaseState(H1), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Ready);
}

HOST_TEST(cache_refund_reconfigure_while_waiting_drops_the_stale_release, "8.14 re-Configure leg; §5.11 (a) rev 1.16 L1: the waiter resolves by name, nothing drawn; the release's completion arrives after Configure() and is dropped, refunding nothing")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), true), 0);
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), true), 0);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t HG = F.Request(FixtureSpec(3, 32));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Diag().Waiters, 1u);
	const uint64_t Gen = F.Diag().Generation;
	// The lanes drain: E's release job runs, and its completion is queued, not applied.
	FPrefixCompletion Stale;
	REQUIRE_TRUE(F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Stale));
	F.Cache.Abandon();
	F.Sim->DestroyPool();
	EXPECT_EQ(F.Cache.GetLeaseState(HG), EPrefixLeaseState::Cancelled);
	EXPECT_TRUE(F.Cache.IsTornDown());
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
	REQUIRE_TRUE(F.Cache.Configure(F.Config));
	EXPECT_NE(F.Diag().Generation, Gen);
	// The rebuilt ledger holds one new entry of 2 pages.
	REQUIRE_NE(F.DiskEntry(FixtureSpec(5, 32), false), 0);
	REQUIRE_EQ(F.Diag().ChargedBytes, 2 * kPage);
	F.Cache.Apply(Stale); // the next apply pass
	const FPrefixCacheDiagnostics D = F.Diag();
	EXPECT_EQ(D.StaleCompletionsDropped, 1u);
	EXPECT_EQ(D.ChargedBytes, 2 * kPage);
	EXPECT_EQ(D.ReleasingBytes, 0u);
	EXPECT_LE(D.BudgetBytes - D.ChargedBytes, 5 * kPage); // free charge never above C
	EXPECT_TRUE(F.LedgerOk());
}

HOST_TEST(cache_refund_region_c_mirrors_every_charge, "8.14 region C (host half); §5.11 (b): budget mode charges and refunds region C through the seam; paging's ledger reads C as the cache's ledger at every reading")
{
	using namespace SuperSLMPaging;
	FPageLedger Paging;
	FPageLedgerConfig LC;
	LC.PagePositions = 16;
	LC.PageBytes = kPage;
	LC.ContextCap = 64;
	LC.SequenceBudget = 16;
	LC.BlockCount = 1;
	LC.PrefixBlockCount = 1;
	LC.bCacheConfigured = true;
	// C = ceil(budget / block bytes) x P_blk, computed here: a budget of 1.5 blocks of 4 pages.
	const uint64_t BlockBytes = 4 * kPage;
	const uint64_t BudgetBytes = BlockBytes + BlockBytes / 2;
	const uint32_t ExpectedC = static_cast<uint32_t>(((BudgetBytes + BlockBytes - 1) / BlockBytes) * 4);
	LC.CacheRegionPages = CacheRegionPagesFor(BudgetBytes, BlockBytes, 4);
	REQUIRE_EQ(LC.CacheRegionPages, ExpectedC);
	REQUIRE_TRUE(Paging.Configure(LC) == ELedgerConfigure::Ok);
	REQUIRE_EQ(Paging.RegionCapacity(ERegion::C), 8u);

	FRefundFixture F(5);
	FPrefixCacheConfig C = F.Config;
	C.BudgetBytes = 1; // ignored in budget mode
	C.RegionC = &Paging;
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	REQUIRE_TRUE(F.Cache.Configure(C));
	F.Config = C;
	F.Sim->Config = C;
	REQUIRE_EQ(F.Diag().BudgetBytes, 8 * kPage);

	auto Mirrored = [&]() {
		return Paging.CacheRegionChargedBytes() == F.Diag().ChargedBytes && F.Diag().RegionChargeFaults == 0 &&
			Paging.GetStats().LedgerFaults == 0;
	};
	REQUIRE_NE(F.DiskEntry(FixtureSpec(1, 33), true), 0); // 3
	EXPECT_TRUE(Mirrored());
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), true), 0); // 2
	REQUIRE_NE(F.DiskEntry(FixtureSpec(3, 48), true), 0); // 3
	EXPECT_TRUE(Mirrored());
	EXPECT_EQ(Paging.RegionCharged(ERegion::C), 8u);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t H = F.Request(FixtureSpec(4, 32)); // build, 3 pages: evicts the 33-token entry
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().Waiters, 1u);
	EXPECT_TRUE(Mirrored());
	EXPECT_EQ(Paging.RegionCharged(ERegion::C), 8u); // the evicted entry's 3 pages are still C's
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_TRUE(Mirrored());
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	EXPECT_TRUE(F.Cache.IsTornDown());
	EXPECT_EQ(Paging.CacheRegionChargedBytes(), 0u);
}

HOST_TEST(cache_refund_failed_release_has_applied_and_is_not_reissued, "§5.4 *What a release's status means* (rev 1.18.1, plan review F1; 5.15) / §5.11 (a): a release that returned a failure has consumed its handle and has applied: its family's refund completes at that apply, the waiter is admitted there, ReleaseFailed is logged once, and the release is never issued again")
{
	FRefundFixture F(5);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1, 33);
	const int64_t HE = F.DiskEntry(E, false); // E: 3 pages
	REQUIRE_NE(HE, 0);
	const uint64_t EHandle = F.Sim->EngineHandleOf(HE);
	REQUIRE_NE(EHandle, 0u);
	F.Cache.Return(HE);                                    // evictable
	REQUIRE_NE(F.DiskEntry(FixtureSpec(2, 32), false), 0); // 2 pages, leased
	REQUIRE_EQ(F.Diag().ChargedBytes, 5 * kPage);

	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t HG = F.Request(FixtureSpec(3, 32)); // build: 3 pages, evicts E
	REQUIRE_NE(HG, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Diag().Waiters, 1u);
	F.Cache.TakeLog();

	// The engine's failing release (the release-status substitution seam): the handle is consumed, then a
	// page-module refusal is returned. The release has applied: E's refund completes, the waiter is admitted.
	F.Sim->ConsumeThenFailReleases = 1;
	F.Sim->ReleaseFailStatus = 1;
	REQUIRE_TRUE(F.Sim->RunRelease(EHandle));
	FPrefixCacheDiagnostics D = F.Diag();
	EXPECT_EQ(F.Sim->ReleaseCount[EHandle], 1);
	EXPECT_EQ(D.Waiters, 0u);
	EXPECT_EQ(D.ReleasingBytes, 0u);
	EXPECT_EQ(D.ClaimedRefundBytes, 0u);
	EXPECT_EQ(D.ChargedBytes, 5 * kPage); // the transfer leaves the total unchanged
	EXPECT_EQ(F.Sim->CacheDrawnPages, 2u); // E's pages went with the consumed handle
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(0)); // not issued again
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));   // admitted at that apply
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReleaseFailed), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::ReleaseFailed), static_cast<size_t>(1));
	EXPECT_TRUE(F.LedgerOk());

	// Every gate opens: the consumed handle is never released a second time.
	F.Sim->Held.erase(EPrefixCommandKind::Release);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->ReleaseCount[EHandle], 1);
	EXPECT_EQ(F.Cache.GetLeaseState(HG), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReleaseFailed), 1u);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 5u);
	EXPECT_TRUE(F.Sim->Violations.empty());
	EXPECT_TRUE(F.LedgerOk());
}
