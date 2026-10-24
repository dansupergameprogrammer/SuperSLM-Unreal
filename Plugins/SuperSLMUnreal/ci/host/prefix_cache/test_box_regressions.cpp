// Host twins of the defects the 2026-10-21 Unreal box run found,
// at the box's geometry where the cell's arithmetic matters: A-EX, B = 64, cap = 4096, a block of 64 pages, region C
// one block of 64 pages (budget mode, through the page ledger's region-C seam) or a byte budget (legacy mode).
//
// Each construction is replayed twice where the box's reading came from the construction itself: once as the
// Unreal cell was written (asserting the reading the box logged), and once as the cell now builds it (asserting
// what the plan reads). The cause throughout: a build is charged R(L) = ceil(L/B) + 1 until it freezes, so the last
// build of a construction needs one page more than the construction settles at, and a C "filled exactly" by builds
// alone is not reachable. The cells now settle with one page free and size each request one page larger.
//
// The defects with no host half (they are Unreal glue) are listed in the box record's follow-up section: the
// release gate that never reopened (the four hangs), the teardown probe's read point, the release counts cleared
// at Configure(), paging's Layer-1 override not reaching cache lane jobs, the non-atomic rename, the disk-write
// wait, the legacy probe bound, and the cooked rig's prefix block.

#include "cache_fixture.h"
#include "harness.h"
#include "sim.h"
#include "SuperSLMPageLedgerCore.h"
#include "SuperSLMSaveCompatCore.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace HostSim;

namespace
{
	constexpr uint64_t kPage = kFixturePageBytes;
	constexpr int32_t kB = 64;      // A-EX's B
	constexpr int32_t kCap = 4096;  // A-EX's cap: a block is 64 pages
	constexpr uint32_t kC = 64;     // region C: one block (the ledger rig's budget of one page, rounded up)

	struct FAEx
	{
		int64_t Counter = 1;
		FSuperSLMPrefixCacheCore Cache{&Counter};
		FPrefixCacheConfig Config;
		SuperSLMPaging::FPageLedger Paging;
		std::unique_ptr<FSim> Sim;
		bool bConfigured = false;
		int32_t Salt = 1;

		// Budget mode on region C of CPages pages, or (bLegacy) a byte budget of CPages pages.
		explicit FAEx(uint32_t CPages = kC, bool bLegacy = false, bool bNested = false)
		{
			Config = FixtureConfig(1, false);
			Config.PageTokens = kB;
			Config.ContextCap = kCap;
			Config.BudgetBytes = static_cast<uint64_t>(CPages) * kPage;
			Config.bNestedEntries = bNested;
			bool bLedger = true;
			if (!bLegacy)
			{
				SuperSLMPaging::FPageLedgerConfig LC;
				LC.PagePositions = kB;
				LC.PageBytes = kPage;
				LC.ContextCap = kCap;
				LC.SequenceBudget = 512;
				LC.BlockCount = 2;
				LC.PrefixBlockCount = 0;
				LC.RestoreHeadroomBlocks = 0;
				LC.bCacheConfigured = true;
				LC.CacheRegionPages = CPages;
				bLedger = Paging.Configure(LC) == SuperSLMPaging::ELedgerConfigure::Ok;
				Config.BudgetBytes = 1; // ignored in budget mode: C is the budget
				Config.RegionC = &Paging;
			}
			bConfigured = bLedger && Cache.Configure(Config);
			Sim.reset(new FSim(Cache, Config, CPages + 16));
		}

		FPrefixCacheDiagnostics Diag() const { return Cache.GetDiagnostics(); }
		uint64_t Pages(uint64_t Bytes) const { return Bytes / kPage; }
		uint64_t Free() const { const FPrefixCacheDiagnostics D = Diag(); return Pages(D.BudgetBytes - D.ChargedBytes); }
		bool WithinBudget() const
		{
			const FPrefixCacheDiagnostics D = Diag();
			return D.ChargedBytes <= D.BudgetBytes && Sim->CacheDrawnPages <= Pages(D.BudgetBytes) && D.RegionChargeFaults == 0;
		}

		// A root entry resident at Pages pages (L = Pages x B).
		FPrefixSpec Entry(int64_t EntryPages) { return FixtureSpec(Salt++, static_cast<int32_t>(EntryPages * kB)); }
		// A root build charged exactly Need pages in flight (L = (Need - 1) x B).
		FPrefixSpec Need(int64_t NeedPages) { return FixtureSpec(Salt++, static_cast<int32_t>((NeedPages - 1) * kB)); }

		int64_t Request(FPrefixSpec Spec, int64_t Parent = 0)
		{
			Spec.ParentHandle = Parent;
			int64_t Handle = 0;
			std::string Error;
			return Cache.Request(Spec, Handle, Error) == EPrefixRequestResult::Accepted ? Handle : 0;
		}

		// Requests Spec and runs everything; the lease, kept or returned. 0 unless it reached Ready.
		int64_t Resident(const FPrefixSpec& Spec, bool bKeepLease, int64_t Parent = 0)
		{
			const int64_t H = Request(Spec, Parent);
			Sim->RunAll();
			if (H == 0 || Cache.GetLeaseState(H) != EPrefixLeaseState::Ready)
			{
				return 0;
			}
			if (!bKeepLease)
			{
				Cache.Return(H);
				Sim->Pass();
			}
			return H;
		}

		EPrefixLeaseState State(int64_t H) const { return Cache.GetLeaseState(H); }

		EPrefixSource SourceOfNextRequest(const FPrefixSpec& Spec)
		{
			const int64_t H = Request(Spec);
			Sim->RunAll();
			const EPrefixSource Source = H != 0 && State(H) == EPrefixLeaseState::Ready ? Cache.GetSource(H) : EPrefixSource::None;
			if (H != 0)
			{
				Cache.Return(H);
				Sim->Pass();
			}
			return Source;
		}
	};
}

HOST_TEST(box_1021_ledger_fill_exactly_evicts_its_own_victim, "box 2026-10-21, Ledger.EvictionWindow / Refused / Waiters / UnclaimedRefunds / CoalesceOnWaiter / StaleRelease, as written: the last build of a C 'filled exactly' peaks one page over and evicts the construction's own victim (or, all leased, is refused)")
{
	// EvictionWindow as written: E (2B + 1, k = 3), F (1), the filler C - 4, all unleased.
	{
		FAEx F;
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_EQ(F.Pages(F.Diag().BudgetBytes), static_cast<uint64_t>(kC));
		REQUIRE_NE(F.Resident(FixtureSpec(F.Salt++, 2 * kB + 1), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(1), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 4), false), 0);
		EXPECT_EQ(F.Diag().Evictions, 1u);          // the filler's R = C - 3 evicted E
		EXPECT_EQ(F.Free(), 3u);                    // the box: "C is full" reads 3
	}
	// Refused as written: A 3 and B C - 3, both leased: B's own build is refused.
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(3), true), 0);
		const int64_t B = F.Request(F.Entry(kC - 3));
		F.Sim->RunAll();
		EXPECT_EQ(F.State(B), EPrefixLeaseState::BudgetExhausted); // the box: "resolves by name"
	}
	// UnclaimedRefunds (i) as written: E 6 unleased, the rest C - 6 leased: the rest's build evicts E.
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(6), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 6), true), 0);
		EXPECT_EQ(F.Free(), 6u);                    // the box: "C is full" reads 6
		const uint64_t Before = F.Diag().Evictions;
		F.Request(F.Need(4));
		F.Request(F.Need(2));
		F.Sim->RunAll();
		EXPECT_EQ(F.Diag().Evictions - Before, 0u); // the box: 0 evictions
	}
	// StaleRelease as written: E 5, the rest C - 5: W (4) fits in the free 5 and never waits.
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(5), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 5), true), 0);
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		F.Request(F.Need(4));
		F.Sim->RunAll();
		EXPECT_EQ(F.Diag().Waiters, 0u);            // the box: "W waits on E's release" 0
	}
}

HOST_TEST(box_1021_ledger_eviction_window_with_one_free_page, "8.14 eviction leg at A-EX as the cell now builds it: E 3, F 1, the filler C - 5 (one page free); W charged 4 takes the free page and claims E's 3; between the decision and E's apply C is full, one eviction, W a waiter that drew nothing; at E's apply W is admitted; F is still a Memory hit")
{
	FAEx F;
	REQUIRE_TRUE(F.bConfigured);
	REQUIRE_NE(F.Resident(FixtureSpec(F.Salt++, 2 * kB + 1), false), 0);
	const FPrefixSpec FSpec = F.Entry(1);
	REQUIRE_NE(F.Resident(FSpec, false), 0);
	REQUIRE_NE(F.Resident(F.Entry(kC - 5), false), 0);
	EXPECT_EQ(F.Diag().Evictions, 0u);
	EXPECT_EQ(F.Free(), 1u);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const uint64_t Builds = F.Diag().BuildsIssued;
	const int64_t W = F.Request(F.Need(4));
	F.Sim->RunAll();
	{
		const FPrefixCacheDiagnostics D = F.Diag();
		EXPECT_EQ(F.State(W), EPrefixLeaseState::Pending);
		EXPECT_EQ(D.Evictions, 1u);
		EXPECT_EQ(D.Waiters, 1u);
		EXPECT_EQ(D.ClaimedRefundBytes, 3 * kPage);           // all of E's refund
		EXPECT_EQ(F.Free(), 0u);                              // the window: C full (W holds the free page)
		EXPECT_EQ(F.Paging.RegionCharged(SuperSLMPaging::ERegion::C), kC); // C still carries E's k
		EXPECT_EQ(D.BuildsIssued, Builds);                    // drew nothing
		EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1));
		EXPECT_TRUE(F.WithinBudget());
	}
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.State(W), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_EQ(F.SourceOfNextRequest(FSpec), EPrefixSource::Memory);
	EXPECT_TRUE(F.WithinBudget());
}

HOST_TEST(box_1021_ledger_refused_with_one_free_page, "8.14 refused at A-EX as the cell now builds it: A 3 and B C - 4 leased (one page free); W charged 3 resolves BudgetExhausted, nothing evicted, no waiter, no build")
{
	FAEx F;
	REQUIRE_NE(F.Resident(F.Entry(3), true), 0);
	REQUIRE_NE(F.Resident(F.Entry(kC - 4), true), 0);
	EXPECT_EQ(F.Free(), 1u);
	const FPrefixCacheDiagnostics D0 = F.Diag();
	const int64_t W = F.Request(F.Need(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.State(W), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Diag().Evictions, D0.Evictions);
	EXPECT_EQ(F.Diag().Waiters, 0u);
	EXPECT_EQ(F.Diag().BuildsIssued, D0.BuildsIssued);
	EXPECT_EQ(F.Free(), 1u);
}

HOST_TEST(box_1021_ledger_waiters_with_one_free_page, "8.14 waiters at A-EX as the cell now builds it: V1 3, V2 3, the rest C - 7 leased (one page free); X1 needs 4, X2 needs 3: two evictions, two waiters, each admitted at its own victim's apply (releases one at a time); as written (the rest C - 6) the rest's build evicts V1 first")
{
	{
		FAEx F; // as written
		REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 6), true), 0);
		EXPECT_EQ(F.Diag().Evictions, 1u);
	}
	FAEx F;
	REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
	REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
	REQUIRE_NE(F.Resident(F.Entry(kC - 7), true), 0);
	EXPECT_EQ(F.Diag().Evictions, 0u);
	EXPECT_EQ(F.Free(), 1u);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t X1 = F.Request(F.Need(4));
	const int64_t X2 = F.Request(F.Need(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().Evictions, 2u);
	EXPECT_EQ(F.Diag().Waiters, 2u);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(2));
	EXPECT_TRUE(F.WithinBudget());
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::Release)); // V1's (the oldest)
	F.Sim->RunAll();
	EXPECT_EQ(F.State(X1), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.State(X2), EPrefixLeaseState::Pending);
	EXPECT_TRUE(F.WithinBudget());
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.State(X2), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Diag().Evictions, 2u);
	EXPECT_TRUE(F.WithinBudget());
}

HOST_TEST(box_1021_ledger_unclaimed_refunds_with_one_free_page, "8.14 unclaimed refunds at A-EX as the cell now builds them: (i) E 6, the rest C - 7, W 5 and X 2: one eviction, E's 6 fully claimed, both admitted at E's apply; (ii) E 3, the rest C - 4, Y 5 refused with E's eviction standing and its 3 unclaimed, Z 4 claims them and is admitted at E's apply")
{
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(6), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 7), true), 0);
		EXPECT_EQ(F.Free(), 1u);
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		const int64_t W = F.Request(F.Need(5));
		const int64_t X = F.Request(F.Need(2));
		F.Sim->RunAll();
		const FPrefixCacheDiagnostics D = F.Diag();
		EXPECT_EQ(D.Evictions, 1u);
		EXPECT_EQ(D.Waiters, 2u);
		EXPECT_EQ(D.ClaimedRefundBytes, 6 * kPage);
		EXPECT_EQ(D.UnclaimedRefundBytes, 0u);
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.State(W), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.State(X), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Diag().Evictions, 1u);
		EXPECT_TRUE(F.WithinBudget());
	}
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 4), true), 0);
		EXPECT_EQ(F.Free(), 1u);
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		const int64_t Y = F.Request(F.Need(5));
		F.Sim->RunAll();
		EXPECT_EQ(F.State(Y), EPrefixLeaseState::BudgetExhausted);
		EXPECT_EQ(F.Diag().Evictions, 1u);
		EXPECT_EQ(F.Diag().UnclaimedRefundBytes, 3 * kPage);
		const int64_t Z = F.Request(F.Need(4));
		F.Sim->RunAll();
		EXPECT_EQ(F.State(Z), EPrefixLeaseState::Pending);
		EXPECT_EQ(F.Diag().ClaimedRefundBytes, 3 * kPage);
		EXPECT_EQ(F.Diag().Evictions, 1u);
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.State(Z), EPrefixLeaseState::Ready);
		EXPECT_TRUE(F.WithinBudget());
	}
}

HOST_TEST(box_1021_ledger_coalesce_and_stale_release_with_one_free_page, "8.14 at A-EX as the cells now build them: CoalesceOnWaiter (E 3, the rest C - 4, W 4 twice: one waiter, one build, one eviction) and StaleRelease's wait (E 5, the rest C - 6, W 5 claims 4 of E's 5 and waits)")
{
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 4), true), 0);
		const FPrefixCacheDiagnostics D0 = F.Diag();
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		const FPrefixSpec WSpec = F.Need(4);
		const int64_t W1 = F.Request(WSpec);
		F.Sim->RunAll();
		const int64_t W2 = F.Request(WSpec);
		F.Sim->RunAll();
		EXPECT_EQ(F.Diag().Waiters, 1u);
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.State(W1), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.State(W2), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Diag().BuildsIssued - D0.BuildsIssued, 1u);
		EXPECT_EQ(F.Diag().Evictions - D0.Evictions, 1u);
	}
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(5), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(kC - 6), true), 0);
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		const int64_t W = F.Request(F.Need(5));
		F.Sim->RunAll();
		EXPECT_EQ(F.Diag().Waiters, 1u);
		EXPECT_EQ(F.Diag().ClaimedRefundBytes, 4 * kPage);
		EXPECT_EQ(F.State(W), EPrefixLeaseState::Pending);
	}
}

HOST_TEST(box_1021_ledger_legacy_with_one_free_page, "8.14 legacy leg at A-EX as the cell now builds it: a byte budget of 8 pages, E 3, the rest 4 leased (one page free), W 4 waits on E and is admitted at its apply, the cache's draw never above the budget; as written (the rest 5) the rest's build evicts E")
{
	{
		FAEx F(8, /*bLegacy*/ true);
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
		REQUIRE_NE(F.Resident(F.Entry(5), true), 0);
		EXPECT_EQ(F.Diag().Evictions, 1u);
	}
	FAEx F(8, true);
	REQUIRE_NE(F.Resident(F.Entry(3), false), 0);
	REQUIRE_NE(F.Resident(F.Entry(4), true), 0);
	EXPECT_EQ(F.Free(), 1u);
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	const int64_t W = F.Request(F.Need(4));
	F.Sim->RunAll();
	EXPECT_EQ(F.State(W), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Diag().Waiters, 1u);
	EXPECT_TRUE(F.WithinBudget());
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.State(W), EPrefixLeaseState::Ready);
	EXPECT_TRUE(F.WithinBudget());
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 8u);
}

HOST_TEST(box_1021_all_pinned_and_adopt_not_ready_fill, "5.5 and 7.21 (N3) at A-EX, legacy budget of 64 pages: as written (A 32, B 32, both leased) B's own build is refused; as the cells now build it (A 31, B 32) both are Ready with one page free and the 100-token request resolves BudgetExhausted, nothing evicted")
{
	{
		FAEx F(64, true);
		REQUIRE_NE(F.Resident(F.Entry(32), true), 0);
		const int64_t B = F.Request(F.Entry(32));
		F.Sim->RunAll();
		EXPECT_EQ(F.State(B), EPrefixLeaseState::BudgetExhausted);
	}
	FAEx F(64, true);
	REQUIRE_NE(F.Resident(F.Entry(31), true), 0);
	REQUIRE_NE(F.Resident(F.Entry(32), true), 0);
	EXPECT_EQ(F.Free(), 1u);
	const uint64_t Evictions = F.Diag().Evictions;
	const int64_t W = F.Request(FixtureSpec(F.Salt++, 100));
	F.Sim->RunAll();
	EXPECT_EQ(F.State(W), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Diag().Evictions, Evictions);
	EXPECT_EQ(F.Diag().Waiters, 0u);
}

HOST_TEST(box_1021_eviction_vs_queued_adopt_fill, "3.6 at A-EX: as written (LRU 3 and next 3 leased, the rest C - 6) the rest is refused; as the cell now builds it (the rest C - 7) it is Ready with one page free, and with the LRU entry pinned (a lease stands in for the queued adopt's pin) the 2B request evicts the next entry, which is then no Memory hit")
{
	{
		FAEx F;
		REQUIRE_NE(F.Resident(F.Entry(3), true), 0);
		REQUIRE_NE(F.Resident(F.Entry(3), true), 0);
		const int64_t Rest = F.Request(F.Entry(kC - 6));
		F.Sim->RunAll();
		EXPECT_EQ(F.State(Rest), EPrefixLeaseState::BudgetExhausted);
	}
	FAEx F;
	const int64_t Lru = F.Resident(F.Entry(3), true);
	const FPrefixSpec NextSpec = F.Entry(3);
	const int64_t Next = F.Resident(NextSpec, true);
	REQUIRE_NE(Lru, 0);
	REQUIRE_NE(Next, 0);
	REQUIRE_NE(F.Resident(F.Entry(kC - 7), true), 0);
	EXPECT_EQ(F.Free(), 1u);
	F.Cache.Return(Next);
	F.Sim->Pass();
	const int64_t New = F.Request(FixtureSpec(F.Salt++, 2 * kB));
	F.Sim->RunAll();
	EXPECT_EQ(F.State(New), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_NE(F.SourceOfNextRequest(NextSpec), EPrefixSource::Memory);
}

HOST_TEST(box_1021_coalesced_build_is_counted_after_the_hash, "3.5 (box 2026-10-21, Cpu.Coalescing 'one build' read 0): 16 requests for one key issue no build until the key's hash completes; after it, exactly one")
{
	FAEx F;
	const FPrefixSpec Spec = FixtureSpec(F.Salt++, 400);
	std::vector<int64_t> Leases;
	for (int I = 0; I < 16; ++I)
	{
		Leases.push_back(F.Request(Spec));
	}
	F.Sim->Pull();
	EXPECT_EQ(F.Diag().BuildsIssued, 0u); // read at the call: the hash is still a pool-thread command
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().BuildsIssued, 1u);
	for (int64_t L : Leases)
	{
		EXPECT_EQ(F.State(L), EPrefixLeaseState::Ready);
	}
}

HOST_TEST(box_1021_mark_applied_is_not_an_eviction, "7.18 / 11.7 (box 2026-10-21, Cpu.ShippingGuards read Evictions + 1 and got 0): a drop of a leased entry is DropDeferred; once the lease returns the mark applies: MarksApplied rises by one and the eviction diagnostic does not")
{
	FPagesFixture F(8, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1, 32);
	const int64_t L = F.RequestReady(E);
	REQUIRE_NE(L, 0);
	const uint64_t Op = F.Drop(E);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::DropDeferred);
	const FPrefixCacheDiagnostics D0 = F.Diag();
	F.Cache.Return(L);
	F.Sim->Pass();
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().MarksApplied, D0.MarksApplied + 1);
	EXPECT_EQ(F.Diag().Evictions, D0.Evictions);
	EXPECT_NE(F.SourceOfNextRequest(E), EPrefixSource::Memory);
}

HOST_TEST(box_1021_nested_child_lease_is_served_by_its_build, "1.9 (box 2026-10-21, Nested.GhostCharge 'D1 still a Memory hit'): a child built under P is served Recomputed, and a later eviction of P does not change its lease's source; so the cell reads D1's residency from its lease and the release-counting seam instead")
{
	FAEx F(16, false, /*bNested*/ true);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t P = F.Resident(F.Entry(4), true);
	REQUIRE_NE(P, 0);
	const int64_t D1 = F.Resident(F.Entry(1), true, P);
	REQUIRE_NE(D1, 0);
	EXPECT_EQ(F.Cache.GetSource(D1), EPrefixSource::Recomputed);
	EXPECT_NE(F.Cache.GetSource(D1), EPrefixSource::Memory);
}

HOST_TEST(box_1021_save_tag_longer_than_its_field, "8.8 and 8.15 (box 2026-10-21, Cpu.SaveRestoreAdopter and Cpu.LeaseAsRestoreHint refused): the save wrapper keeps 16 bytes of the Layer-1 tag, so a vendored tag longer than that never equals itself after a save (Layer1Mismatch at every restore); a tag that fits is SamePin. Build.cs now refuses the long tag")
{
	std::vector<uint8_t> Payload(64, 0);
	std::memcpy(Payload.data(), "SSB6", 4);
	const std::string Commit = "5c30458f00000000000000000000000000000000";
	const std::string Long = "(none: box build of a working branch)";
	const std::string Stored = Long.substr(0, 16); // SuperSLMSaveBlob.cpp's kTagBytes
	EXPECT_EQ(SuperSLMCore::CheckSaveLayer1(Long, Stored, Commit, Payload.data(), Payload.size()), SuperSLMCore::ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(SuperSLMCore::CheckSaveLayer1("v1.14.0", "v1.14.0", Commit, Payload.data(), Payload.size()), SuperSLMCore::ESaveLayer1Check::SamePin);
	// SaveCompat.Crafted's expectation holds only under the table's compiled tag.
	const std::string Prev = "v1.13.1", PrevCommit = "d697927f6d5ce5fa73ecbb8660020a425518b923";
	std::vector<uint8_t> Ssb5(64, 0);
	std::memcpy(Ssb5.data(), "SSB5", 4);
	EXPECT_EQ(SuperSLMCore::CheckSaveLayer1("v1.14.0", Prev, PrevCommit, Ssb5.data(), Ssb5.size()), SuperSLMCore::ESaveLayer1Check::CompatibilityEntry);
	EXPECT_EQ(SuperSLMCore::CheckSaveLayer1(Long, Prev, PrevCommit, Ssb5.data(), Ssb5.size()), SuperSLMCore::ESaveLayer1Check::Layer1Mismatch);
}

// ---- The box fixes' code review (code-review/review-box-fixes-2026-10-21.md), findings 1 and 2 ----
// Both are host-side (SuperSLMPrefixCacheHost.cpp), which the suite does not compile; FSim models the host's
// bookkeeping (sim.h, "The CPU host's lane-side bookkeeping"), and each cell runs the construction twice: the host as
// it is now, and the host as it was at afe45536d (the must-reject leg), which must show the defect.

namespace
{
	// Finding 1. A build posted under the armed build hold and forced by paging's Layer-1 override: the lane's
	// build-hold count returns to 0 (the lane plans again) only if the forced path passes the hold.
	int32_t LaneHoldsAfterForcedBuild(bool bPreFix, EPrefixLeaseState& OutState, uint32_t& OutForced)
	{
		FCacheFixture F(2, false);
		F.Sim->bBuildHoldArmed = true;
		F.Sim->bForceNextBuild = true;
		F.Sim->bForcedBuildSkipsHold = bPreFix;
		const int64_t L = F.Request(FixtureSpec(1, kFixtureLength));
		F.Sim->RunAll();
		OutState = L != 0 ? F.Cache.GetLeaseState(L) : EPrefixLeaseState::Unknown;
		OutForced = F.Sim->ForcedBuilds;
		return F.Sim->LaneBuildHolds;
	}

	// Finding 2. Entry A is resident at handle X. A's release and B's build are in flight together; the release
	// runs first and frees X, the build is given X, and the build is delivered first (the Apply pass delivers lanes
	// in index order, and the build's lane is the lower one). Returns whether X is in the host's live set after both
	// deliveries; OutMisses and OutDoubles are the teardown sweep's differences from the engine.
	struct FReuseReading
	{
		bool bConstructed = false;
		bool bLiveAfter = false;
		uint32_t Misses = 0;
		uint32_t Doubles = 0;
		bool bReleasedOnce = false;
		EPrefixLeaseState BState = EPrefixLeaseState::Unknown;
	};

	FReuseReading AddressReuse(bool bPreFix)
	{
		FReuseReading R;
		FCacheFixture F(2, false); // 5 pages: A (2) releasing beside B's build at its peak R(32) = 3
		F.Sim->bHostLiveAtDelivery = bPreFix;
		const FPrefixSpec A = FixtureSpec(1, kFixtureLength);
		const FPrefixSpec B = FixtureSpec(2, kFixtureLength);
		const int64_t LA = F.RequestReady(A);
		if (LA == 0)
		{
			return R;
		}
		const uint64_t X = F.Sim->EngineHandleOf(LA);
		F.Cache.Return(LA);
		F.Sim->Pass();
		F.Sim->bReuseReleasedHandles = true;
		F.Sim->Held = {EPrefixCommandKind::Release, EPrefixCommandKind::Build};
		const uint64_t Op = F.Drop(A);
		const int64_t LB = F.Request(B);
		F.Sim->RunAll(); // the hash, the drop's mark; the release and the build stay queued
		F.Sim->Held.clear();
		FPrefixCommand Rel, Build;
		FPrefixCompletion RelDone, BuildDone;
		if (Op == 0 || LB == 0 || X == 0 || F.Sim->CountQueued(EPrefixCommandKind::Release) != 1 ||
			F.Sim->CountQueued(EPrefixCommandKind::Build) != 1 ||
			!F.Sim->ExecuteOnly(EPrefixCommandKind::Release, Rel, RelDone) ||
			!F.Sim->ExecuteOnly(EPrefixCommandKind::Build, Build, BuildDone) ||
			Rel.EngineHandle != X || BuildDone.EngineHandle != X)
		{
			return R;
		}
		R.bConstructed = true;
		F.Sim->DeliverAsPass(Build, BuildDone);
		F.Sim->DeliverAsPass(Rel, RelDone);
		F.Sim->RunAll();
		R.BState = F.Cache.GetLeaseState(LB);
		R.bLiveAfter = F.Sim->HostLive.count(X) != 0;
		F.Cache.Return(LB);
		F.Sim->Pass();
		F.HostReconfigure(nullptr);
		R.Misses = F.Sim->HostSweepMisses;
		R.Doubles = F.Sim->HostSweepDoubles;
		R.bReleasedOnce = F.EveryHandleReleasedOnce();
		return R;
	}
}

HOST_TEST(box_review_1021_forced_build_passes_the_build_hold, "review finding 1 (7.21 / 8.10 (c) with the paging override): a build posted under the armed build hold and forced by the Layer-1 override completes with the forced status and gives its lane's hold count back; the pre-fix host's forced path returned before PassBuildHold and left the lane counted as held")
{
	EPrefixLeaseState State = EPrefixLeaseState::Unknown;
	uint32_t Forced = 0;
	EXPECT_EQ(LaneHoldsAfterForcedBuild(false, State, Forced), 0);
	EXPECT_EQ(Forced, 1u);
	EXPECT_EQ(State, EPrefixLeaseState::PoolExhausted); // the forced status, resolved
	// Must-reject: the forced path that skips the pass leaks one hold for the rest of the configuration.
	EXPECT_EQ(LaneHoldsAfterForcedBuild(true, State, Forced), 1);
	EXPECT_EQ(Forced, 1u);
}

HOST_TEST(box_review_1021_release_leaves_the_live_set_at_issue, "review finding 2: a release frees address X, a build in flight beside it is given X and is delivered first; the handle leaves the host's live set when its release is issued, so the new handle at X stays live and the teardown sweep releases it; the pre-fix host removed it at the release's delivery, taking the new handle with it (the sweep misses it: SSLM_POOL_HAS_LIVE_HANDLES)")
{
	const FReuseReading Now = AddressReuse(false);
	REQUIRE_TRUE(Now.bConstructed);
	EXPECT_EQ(Now.BState, EPrefixLeaseState::Ready);
	EXPECT_TRUE(Now.bLiveAfter);
	EXPECT_EQ(Now.Misses, 0u);
	EXPECT_EQ(Now.Doubles, 0u);
	EXPECT_TRUE(Now.bReleasedOnce);
	// Must-reject: removal at delivery.
	const FReuseReading Pre = AddressReuse(true);
	REQUIRE_TRUE(Pre.bConstructed);
	EXPECT_TRUE(!Pre.bLiveAfter);
	EXPECT_EQ(Pre.Misses, 1u);
}

// Box 2026-10-21, second run (Cpu.ReconfigureInFlight "build, queued: the request resolves by name", and
// Ledger.StaleRelease's "W resolves by name"): a request still Pending when its configuration ends resolves Cancelled
// by name and reads Cancelled after the next Configure() returns; the cells read it there. The core cleared every
// lease at Configure(), so it read Unknown (Invalid at the subsystem). No host cell read a lease after a
// re-Configure() before this one: the model's divergence was a missing reading, not a different behaviour.
// Both teardown paths: the CPU host's (HostReconfigure: Abandon()) and the core's own (BeginTeardown()).
HOST_TEST(box_r2_1021_queued_request_reads_cancelled_after_reconfigure, "1.7 / 8.14 (box 2026-10-21 second run, Cpu.ReconfigureInFlight and Ledger.StaleRelease): a request Pending at a re-Configure() reads Cancelled after it, on both teardown paths and after a further re-Configure(); a Ready lease of the ended configuration still reads Unknown (2.10), and no verb treats the cancelled handle as live")
{
	for (const bool bHostPath : {true, false})
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const int64_t Ready = F.RequestReady(FixtureSpec(1));
		const int64_t Queued = F.Request(FixtureSpec(2)); // its key hash is queued, never run
		REQUIRE_TRUE(Ready != 0 && Queued != 0);
		EXPECT_EQ(F.Cache.GetLeaseState(Queued), EPrefixLeaseState::Pending);
		const FPrefixCacheConfig Same = F.Config;
		if (bHostPath)
		{
			REQUIRE_TRUE(F.HostReconfigure(&Same));
		}
		else
		{
			REQUIRE_TRUE(F.Reconfigure(Same));
		}
		EXPECT_EQ(F.Cache.GetLeaseState(Queued), EPrefixLeaseState::Cancelled);
		EXPECT_EQ(F.Cache.GetLeaseState(Ready), EPrefixLeaseState::Unknown);
		uint64_t Engine = 0;
		uint64_t Token = 0;
		std::string Error;
		EXPECT_EQ(F.Cache.Return(Queued), false);
		EXPECT_EQ(F.Cache.BeginAdopt(Queued, Engine, Token), EPrefixAdoptCheck::NotCacheHandle);
		EXPECT_EQ(F.Cache.RefuseRelease(Queued, Error), false);
		EXPECT_EQ(F.Cache.GetSource(Queued), EPrefixSource::None);
		REQUIRE_TRUE(F.Reconfigure(Same));
		EXPECT_EQ(F.Cache.GetLeaseState(Queued), EPrefixLeaseState::Cancelled);
		const int64_t Fresh = F.RequestReady(FixtureSpec(2));
		EXPECT_GT(Fresh, Queued); // the counter is never rewound: a cancelled handle is never reissued
		EXPECT_EQ(F.Cache.GetLeaseState(Fresh), EPrefixLeaseState::Ready);
	}
}

namespace
{
	enum class ETeardownPath : uint8_t { Host, Core };

	// Cell 1.7's running legs (code review r2, B1). A build or restore is running on a lane when Configure() is called
	// again. On the CPU host's path, the subsystem's teardown marks the core first (DestroyCpuStateContents ->
	// TeardownBegins), then stops the lanes and delivers the finished job, which applies to the lease (Ready), then
	// sweeps and abandons. On the core's own path (BeginTeardown), the lease is cancelled before the job delivers.
	// bMark false replays the pre-fix host order (the delivery before any mark). Returns the lease's state after
	// Configure() returns, and the state the delivery left it in (OutAtSweep).
	EPrefixLeaseState RunningAtReconfigure(EPrefixCommandKind Kind, ETeardownPath Path, bool bMark, EPrefixLeaseState& OutAtSweep, bool& bOutConstructed)
	{
		bOutConstructed = false;
		OutAtSweep = EPrefixLeaseState::Unknown;
		const bool bRestore = Kind == EPrefixCommandKind::Restore;
		FCacheFixture F(4, bRestore);
		if (!F.bConfigured)
		{
			return EPrefixLeaseState::Unknown;
		}
		const FPrefixSpec S = FixtureSpec(2);
		if (bRestore)
		{
			F.PutDiskFile(S);
		}
		const int64_t Lease = F.Request(S);
		F.Sim->Held.insert(Kind);
		F.Sim->RunAll(); // the hash (and the disk read) applied; the lane job taken and running
		if (Lease == 0 || F.Cache.GetLeaseState(Lease) != EPrefixLeaseState::Pending || F.Sim->CountQueued(Kind) != 1)
		{
			return EPrefixLeaseState::Unknown;
		}
		bOutConstructed = true;
		const FPrefixCacheConfig Same = F.Config;
		F.Sim->Held.clear();
		if (Path == ETeardownPath::Core)
		{
			if (!F.Reconfigure(Same)) // BeginTeardown, then the job drains
			{
				bOutConstructed = false;
			}
			OutAtSweep = EPrefixLeaseState::Cancelled;
			return F.Cache.GetLeaseState(Lease);
		}
		if (bMark)
		{
			F.Cache.MarkTeardownBegins();
		}
		F.Sim->bAutoPull = false;
		if (!F.Sim->RunOneOfKind(Kind)) // the subsystem's lane delivery
		{
			bOutConstructed = false;
		}
		OutAtSweep = F.Cache.GetLeaseState(Lease);
		if (!F.HostReconfigure(&Same))
		{
			bOutConstructed = false;
		}
		return F.Cache.GetLeaseState(Lease);
	}
}

HOST_TEST(box_r2_1021_running_build_and_restore_read_cancelled_after_reconfigure, "1.7 running legs / §5.6 (code review r2 B1): a build or a restore running on a lane at a re-Configure() reads Cancelled after it, on the CPU host's path (marked at the teardown's entry, then the lane delivery applies it Ready, then the sweep) and on the core's BeginTeardown path; the pre-fix order (delivered before any mark) reads Unknown")
{
	for (const EPrefixCommandKind Kind : {EPrefixCommandKind::Build, EPrefixCommandKind::Restore})
	{
		for (const ETeardownPath Path : {ETeardownPath::Host, ETeardownPath::Core})
		{
			EPrefixLeaseState AtSweep = EPrefixLeaseState::Unknown;
			bool bConstructed = false;
			const EPrefixLeaseState After = RunningAtReconfigure(Kind, Path, true, AtSweep, bConstructed);
			REQUIRE_TRUE(bConstructed);
			if (Path == ETeardownPath::Host)
			{
				EXPECT_EQ(AtSweep, EPrefixLeaseState::Ready); // the delivery applied it: the case B1 names
			}
			EXPECT_EQ(After, EPrefixLeaseState::Cancelled);
		}
		// Must-reject: the delivery before the mark (3100a49a0's order) loses the lease.
		EPrefixLeaseState AtSweep = EPrefixLeaseState::Unknown;
		bool bConstructed = false;
		EXPECT_EQ(RunningAtReconfigure(Kind, ETeardownPath::Host, false, AtSweep, bConstructed), EPrefixLeaseState::Unknown);
		REQUIRE_TRUE(bConstructed);
	}
}
