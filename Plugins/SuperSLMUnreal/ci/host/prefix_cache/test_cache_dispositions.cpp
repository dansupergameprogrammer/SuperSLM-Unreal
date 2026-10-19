// Cache-owned handles against the existing prefix verbs (precache plan §5.10, cell 2.10).

#include "cache_fixture.h"

using namespace SuperSLMCore;
using namespace HostSim;

HOST_TEST(cache_release_prefix_refuses_cache_handles, "2.10 (ReleasePrefix on a leased and an unleased cache handle), 5.4 (§5.10 row)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t H = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(H, 0);
	F.Cache.TakeLog();
	std::string Error;
	EXPECT_EQ(F.Cache.RefuseRelease(H, Error), true); // leased
	EXPECT_EQ(Contains(Error, "owned by the prefix cache"), true);
	EXPECT_EQ(Contains(Error, "ReturnPrefix"), true);
	REQUIRE_TRUE(F.Cache.Return(H));
	Error.clear();
	EXPECT_EQ(F.Cache.RefuseRelease(H, Error), true); // unleased
	EXPECT_EQ(Contains(Error, "ReturnPrefix"), true);
	F.Sim->Held = {EPrefixCommandKind::Build};
	const int64_t Pending = F.Request(FixtureSpec(2));
	EXPECT_EQ(F.Cache.RefuseRelease(Pending, Error), true); // still pending
	const int64_t CreatePrefixHandle = F.Counter++;
	Error = "untouched";
	EXPECT_EQ(F.Cache.RefuseRelease(CreatePrefixHandle, Error), false);
	EXPECT_EQ(Error, std::string("untouched"));
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReleaseOfCacheHandle), 3u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::ReleaseOfCacheHandle), static_cast<size_t>(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0)); // nothing released
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
}

HOST_TEST(cache_adopt_of_an_ended_lease_is_refused, "2.10 (AdoptPrefix on a returned lease and on one whose entry was evicted; lease A returned while B is live)")
{
	FCacheFixture F(1, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const int64_t A = F.RequestReady(S);
	const int64_t B = F.RequestReady(S);
	REQUIRE_TRUE(A != 0 && B != 0);
	REQUIRE_TRUE(F.Cache.Return(A));
	F.Cache.TakeLog();
	uint64_t Engine = 7;
	uint64_t Token = 7;
	EXPECT_EQ(F.Cache.BeginAdopt(A, Engine, Token), EPrefixAdoptCheck::PrefixLeaseEnded); // B still live on the same entry
	EXPECT_EQ(Engine, 0u);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::AdoptOfEndedLease), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::AdoptOfEndedLease), static_cast<size_t>(1));
	const FResidencyEntryId Entry = F.Cache.Residency().GetLeaseEntry(B);
	REQUIRE_NE(Entry, kInvalidResidencyEntry);
	EXPECT_EQ(F.Cache.Residency().GetPinCount(Entry, EResidencyPin::Adopt), 0u); // nothing queued
	REQUIRE_EQ(F.Cache.BeginAdopt(B, Engine, Token), EPrefixAdoptCheck::Adoptable);
	EXPECT_NE(Engine, 0u);
	EXPECT_EQ(F.Cache.Residency().GetPinCount(Entry, EResidencyPin::Adopt), 1u);
	EXPECT_EQ(F.Cache.EndAdopt(Token), true);
	EXPECT_EQ(F.Cache.Residency().GetPinCount(Entry, EResidencyPin::Adopt), 0u);

	// The entry is then evicted: B's lease, returned, stays ended; a fresh request takes the budget.
	REQUIRE_TRUE(F.Cache.Return(B));
	EXPECT_NE(F.RequestReady(FixtureSpec(2)), 0);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Cache.BeginAdopt(B, Engine, Token), EPrefixAdoptCheck::PrefixLeaseEnded);

	// A pending lease is not adoptable yet.
	F.Sim->Held = {EPrefixCommandKind::Build};
	const int64_t P = F.Request(FixtureSpec(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.BeginAdopt(P, Engine, Token), EPrefixAdoptCheck::NotReady);
	EXPECT_EQ(F.Cache.BeginAdopt(F.Counter++, Engine, Token), EPrefixAdoptCheck::NotCacheHandle);
}

HOST_TEST(cache_return_prefix_dispositions, "2.10 (ReturnPrefix twice and on a CreatePrefix handle)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t H = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(H, 0);
	F.Cache.TakeLog();
	EXPECT_EQ(F.Cache.Return(H), true);
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ended);
	const uint32_t Resident = F.Diag().ResidentEntries;
	EXPECT_EQ(F.Cache.Return(H), false);
	EXPECT_EQ(F.Cache.Return(F.Counter++), false); // a CreatePrefix handle
	EXPECT_EQ(F.Cache.Return(0), false);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::ReturnNotOwned), 3u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::ReturnNotOwned), static_cast<size_t>(3));
	EXPECT_EQ(F.Diag().ResidentEntries, Resident); // nothing changes: the entry stays cached
	EXPECT_EQ(Resident, 1u);
}

HOST_TEST(cache_a_refused_lease_is_returned_once, "§5.10 (ReturnPrefix ends a lease resolved to a refusal)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t H = F.Request(FixtureSpec(1, 32, "missing_schema"));
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::UnknownSchema);
	EXPECT_EQ(F.Cache.Return(H), true);
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ended);
	EXPECT_EQ(F.Cache.Return(H), false);
}

HOST_TEST(cache_handles_from_before_a_reconfigure_are_unknown, "2.10 (each verb with a handle from before a re-Configure)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t Leased = F.RequestReady(FixtureSpec(1));
	const int64_t Returned = F.RequestReady(FixtureSpec(2));
	REQUIRE_TRUE(Leased != 0 && Returned != 0);
	REQUIRE_TRUE(F.Cache.Return(Returned));
	REQUIRE_TRUE(F.Reconfigure(F.Config));
	for (int64_t Old : {Leased, Returned})
	{
		std::string Error = "untouched";
		uint64_t Engine = 0;
		uint64_t Token = 0;
		EXPECT_EQ(F.Cache.Return(Old), false);
		EXPECT_EQ(F.Cache.BeginAdopt(Old, Engine, Token), EPrefixAdoptCheck::NotCacheHandle);
		EXPECT_EQ(F.Cache.RefuseRelease(Old, Error), false); // the subsystem's own "not a live prefix handle" decides
		EXPECT_EQ(F.Cache.GetLeaseState(Old), EPrefixLeaseState::Unknown);
		EXPECT_EQ(F.Cache.GetSource(Old), EPrefixSource::None);
	}
	const int64_t Fresh = F.RequestReady(FixtureSpec(1));
	EXPECT_GT(Fresh, Returned); // the counter is never rewound
	EXPECT_EQ(F.Cache.GetSource(Fresh), EPrefixSource::Recomputed);
}

HOST_TEST(cache_teardown_releases_every_engine_handle_exactly_once, "2.10 (release-counting seam), 1.5")
{
	FCacheFixture F(3, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	const int64_t B = F.RequestReady(FixtureSpec(2));
	const int64_t C = F.RequestReady(FixtureSpec(3));
	REQUIRE_TRUE(A != 0 && B != 0 && C != 0);
	F.Cache.Return(B);
	F.Cache.Return(C);
	EXPECT_NE(F.RequestReady(FixtureSpec(4)), 0); // evicts B
	int64_t Probe = 0;
	std::string Error;
	EXPECT_EQ(F.Cache.Request(FixtureSpec(5), Probe, Error), EPrefixRequestResult::Accepted);
	F.Cache.BeginTeardown();
	EXPECT_EQ(F.Cache.GetLeaseState(Probe), EPrefixLeaseState::Cancelled);
	EXPECT_EQ(F.Cache.Request(FixtureSpec(6), Probe, Error), EPrefixRequestResult::NotConfigured);
	EXPECT_EQ(F.Cache.Configure(F.Config), false); // not torn down yet
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.IsTornDown(), true);
	REQUIRE_EQ(F.Sim->Handles.size(), static_cast<size_t>(4));
	for (const auto& Pair : F.Sim->Handles)
	{
		EXPECT_EQ(F.Sim->ReleaseCount[Pair.first], 1);
	}
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
	EXPECT_EQ(F.Cache.Configure(F.Config), true);
	const int64_t Again = F.RequestReady(FixtureSpec(1));
	EXPECT_NE(Again, 0);
	EXPECT_NE(F.Cache.GetSource(Again), EPrefixSource::Memory); // the next request is a miss
}

HOST_TEST(cache_ended_leases_hold_no_spec, "§5.6 (RequestPrefix copies the spec; a lease keeps it only until its key arrives)")
{
	// Memory hits on one 4,096-token prefix, each lease returned. Only the entry keeps the spec.
	int64_t Counter = 1;
	FSuperSLMPrefixCacheCore Cache(&Counter);
	FPrefixCacheConfig Config = FixtureConfig(1, false);
	Config.ContextCap = 8192;
	Config.BudgetBytes = (8192 / 16 + 1) * Config.PageBytes;
	REQUIRE_TRUE(Cache.Configure(Config));
	FSim Sim(Cache, Config, 1 << 20);
	const FPrefixSpec Spec = FixtureSpec(1, 4096);
	std::vector<int64_t> Handles;
	std::string Error;
	for (int I = 0; I < 200; ++I)
	{
		int64_t H = 0;
		REQUIRE_EQ(Cache.Request(Spec, H, Error), EPrefixRequestResult::Accepted);
		EXPECT_EQ(Cache.GetDiagnostics().SpecTokensHeld, I == 0 ? 4096u : 8192u); // the pending copy, and the entry's
		Sim.RunAll();
		REQUIRE_EQ(Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
		EXPECT_EQ(Cache.GetDiagnostics().SpecTokensHeld, 4096u); // a Ready lease holds no copy
		REQUIRE_TRUE(Cache.Return(H));
		Handles.push_back(H);
	}
	EXPECT_EQ(Cache.GetDiagnostics().SpecTokensHeld, 4096u);
	// The compact record still tells an ended handle from one the cache never minted.
	uint64_t Engine = 0;
	uint64_t Token = 0;
	EXPECT_EQ(Cache.BeginAdopt(Handles[0], Engine, Token), EPrefixAdoptCheck::PrefixLeaseEnded);
	EXPECT_EQ(Cache.BeginAdopt(Counter++, Engine, Token), EPrefixAdoptCheck::NotCacheHandle);
	EXPECT_EQ(Cache.GetLeaseState(Handles[199]), EPrefixLeaseState::Ended);
	EXPECT_EQ(Cache.GetSource(Handles[199]), EPrefixSource::Memory);
}
