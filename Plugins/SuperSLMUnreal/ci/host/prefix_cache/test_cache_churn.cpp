// Cache churn under the charge rule (precache plan §5.4; cells 1.4 and 7.10).

#include "cache_fixture.h"

#include <random>

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	// std::mt19937's sequence is fixed by the standard; the distributions are not, so draw by hand.
	uint32_t Draw(std::mt19937& Rng, uint32_t Bound)
	{
		return static_cast<uint32_t>(Rng() % Bound);
	}
}

HOST_TEST(cache_churn_keeps_every_invariant, "1.4, 7.10 (host half: the fake pool stands in for the fill probe)")
{
	// 500 requests over 40 keys, a budget of 10 entries, random leases and returns, work left in
	// flight at random, the disk tier on so evicted entries come back restore-built.
	FCacheFixture F(10, true, 16);
	REQUIRE_TRUE(F.bConfigured);
	std::mt19937 Rng(0x5EED1234u);
	const uint64_t BudgetPages = F.BudgetPages();
	std::vector<int64_t> Live;
	std::vector<int64_t> All;
	std::map<uint64_t, std::set<int64_t>> LeasesOnEngine; // engine handle -> live Ready leases that adopted it
	int Requests = 0;
	int Steps = 0;
	bool bBroken = false;
	auto CheckInvariants = [&](const char* Where) {
		const FPrefixCacheDiagnostics D = F.Diag();
		const uint64_t Drawn = F.Sim->CacheDrawnPages * F.Config.PageBytes;
		if (D.ChargedBytes > F.Config.BudgetBytes || Drawn > D.ChargedBytes || F.Sim->CacheDrawnPages > BudgetPages ||
			D.ChargedBytes != D.ResidentBytes + D.InFlightBytes + D.ReleasingBytes || !F.Sim->Violations.empty())
		{
			if (!bBroken)
			{
				HostTest::Fail(__FILE__, __LINE__, std::string("invariant broken ") + Where + " at step " + std::to_string(Steps) +
					": charged=" + std::to_string(D.ChargedBytes) + " budget=" + std::to_string(F.Config.BudgetBytes) +
					" drawn=" + std::to_string(Drawn) + " resident+inflight+releasing=" +
					std::to_string(D.ResidentBytes + D.InFlightBytes + D.ReleasingBytes) +
					(F.Sim->Violations.empty() ? std::string() : " violation: " + F.Sim->Violations.front()));
			}
			bBroken = true;
		}
	};
	// A released engine handle must never be one a live lease still adopts from (pinned entries are never evicted).
	size_t ReleasesSeen = 0;
	auto CheckReleases = [&]() {
		const std::vector<uint64_t> Released = F.Released();
		for (; ReleasesSeen < Released.size(); ++ReleasesSeen)
		{
			const auto It = LeasesOnEngine.find(Released[ReleasesSeen]);
			if (It != LeasesOnEngine.end() && !It->second.empty() && !bBroken)
			{
				HostTest::Fail(__FILE__, __LINE__, "a leased entry was evicted at step " + std::to_string(Steps));
				bBroken = true;
			}
		}
	};

	while (Requests < 500 && !bBroken)
	{
		++Steps;
		const uint32_t Action = Draw(Rng, 4);
		if (Action <= 1)
		{
			const int64_t H = F.Request(FixtureSpec(static_cast<int32_t>(Draw(Rng, 40))));
			REQUIRE_NE(H, 0);
			Live.push_back(H);
			All.push_back(H);
			++Requests;
		}
		else if (Action == 2 && !Live.empty())
		{
			const size_t At = Draw(Rng, static_cast<uint32_t>(Live.size()));
			const int64_t H = Live[At];
			for (auto& Pair : LeasesOnEngine) { Pair.second.erase(H); }
			F.Cache.Return(H);
			Live.erase(Live.begin() + static_cast<long>(At));
		}
		const uint32_t Run = Draw(Rng, 5);
		for (uint32_t I = 0; I < Run && F.Sim->RunOne(); ++I)
		{
			CheckInvariants("after a completion");
			CheckReleases();
		}
		// Learn which engine handle each newly Ready lease adopts from.
		for (int64_t H : Live)
		{
			if (F.Cache.GetLeaseState(H) == EPrefixLeaseState::Ready)
			{
				const uint64_t Engine = F.Sim->EngineHandleOf(H);
				if (Engine != 0) { LeasesOnEngine[Engine].insert(H); }
			}
		}
		CheckInvariants("at a step boundary");
		// Too many live leases pin everything; return the oldest few so the churn keeps moving.
		while (Live.size() > 8)
		{
			for (auto& Pair : LeasesOnEngine) { Pair.second.erase(Live.front()); }
			F.Cache.Return(Live.front());
			Live.erase(Live.begin());
		}
	}
	REQUIRE_EQ(Requests, 500);

	F.Sim->RunAll();
	CheckInvariants("after the drain");
	CheckReleases();
	size_t Resolved = 0;
	size_t Exhausted = 0;
	for (int64_t H : All)
	{
		const EPrefixLeaseState State = F.Cache.GetLeaseState(H);
		Resolved += State != EPrefixLeaseState::Pending && State != EPrefixLeaseState::Unknown ? 1 : 0;
		Exhausted += State == EPrefixLeaseState::BudgetExhausted ? 1 : 0;
	}
	EXPECT_EQ(Resolved, All.size()); // every request resolved by name
	EXPECT_GT(F.Diag().BuildsIssued, 0u);
	EXPECT_GT(F.Diag().RestoresIssued, 0u); // both recompute-built and restore-built entries
	EXPECT_GT(F.Diag().Evictions, 0u);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, BudgetPages);

	for (int64_t H : Live) { F.Cache.Return(H); }
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.IsTornDown(), true);
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u); // the pool ends at its initial count
	for (const auto& Pair : F.Sim->Handles)
	{
		if (F.Sim->ReleaseCount[Pair.first] != 1)
		{
			HostTest::Fail(__FILE__, __LINE__, "engine handle " + std::to_string(Pair.first) + " released " +
				std::to_string(F.Sim->ReleaseCount[Pair.first]) + " times");
			break;
		}
	}
	EXPECT_EQ(F.Sim->Violations.empty(), true);
	(void)Exhausted;
}
