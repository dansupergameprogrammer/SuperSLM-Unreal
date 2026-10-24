// Explicit residency control (precache plan rev 1.18-1.18.3 §5.6, PC-16 (a)): WarmPrefix, DropPrefix and
// GetPrefixOpResult on the cache core, through the simulator. Host halves of cells 1.10, 2.16, 5.12, 7.8,
// 7.18, 7.19, 8.14's drop legs and 8.17. The ordering cells (3.12, 3.13) are in test_residency_order.cpp and
// the disk part (3.10, 1.7's rev 1.18.2 leg) in test_residency_disk.cpp.
//
// Red first: the verbs are stubs (SuperSLMPrefixCacheCore.cpp, "RED STUBS (U2, explicit residency
// control ...)"): Warm and Drop refuse NotConfigured with no op, GetOpResult knows no op, EndApplyPass does
// nothing. Every cell here is red on that, at its first Warm or Drop.
//
// Fixture (cache_fixture.h): B = 16, cap = 64, one page = 4096 bytes. FCacheFixture(N) budgets 2N + 1 pages;
// a build of 32 tokens is charged R(32) = 3 and settles to 2. FPagesFixture(P) budgets exactly P pages; a
// restore of L tokens is charged ceil(L/16) (L = 33: 3 pages), a build of 48 tokens R(48) = 4.
//
// "A pass" is Tick()'s apply pass: every RunOne applies one completion as one pass (sim.h), and Sim->Pass()
// is a pass with no completion, after game-thread calls such as Return.

#include "cache_fixture.h"

#include <functional>

using namespace HostSim;

namespace
{
	constexpr uint64_t kPage = kFixturePageBytes;

	// Index in History of the first command matching the predicate; History.size() if none.
	template <typename TPred>
	size_t FirstIn(const FSim& Sim, TPred Pred)
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

	// A spec far from every other in these files: FixtureSpec(10000 + I) for I distinct.
	FPrefixSpec Absent(int32_t I) { return FixtureSpec(10000 + I); }

	// Resolves Count drops of absent keys (NotCached each): Count resolved ops.
	void ResolveDrops(FCacheFixture& F, int32_t First, int32_t Count)
	{
		for (int32_t I = 0; I < Count; ++I)
		{
			F.Drop(Absent(First + I));
		}
		F.Sim->RunAll(Count * 4 + 100);
	}
}

// ---- 7.19 --------------------------------------------------------------------------------------------

HOST_TEST(residency_warm_cold_resolves_resident_after_one_build, "7.19 [U2]: WarmPrefix(S) on a cold cache resolves Resident after one build (prefill count +1), holds no lease, and the following RequestPrefix(S) is a Memory hit with no prefill")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const uint64_t Op = F.Warm(S);
	REQUIRE_NE(Op, 0u);
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Pending);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
	EXPECT_EQ(F.Diag().Warms, 1u);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Memory);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_TRUE(F.Sim->Violations.empty());
}

HOST_TEST(residency_warmed_entries_are_unpinned_and_evict_like_unleased, "7.19 [U2]: a warmed entry holds no pin; with the budget filled by warmed entries and nothing leased a request evicts them like any unleased entry (mutant: a warm that leases -> the last eviction refuses)")
{
	FCacheFixture F(2, false); // 5 pages
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t A = F.Warm(FixtureSpec(1));
	REQUIRE_NE(A, 0u);
	F.Sim->RunAll(); // settles at 2 pages, so the second warm's R(32) = 3 fits beside it
	const uint64_t B = F.Warm(FixtureSpec(2));
	REQUIRE_NE(B, 0u);
	F.Sim->RunAll();
	REQUIRE_EQ(F.OpResult(A), EPrefixOpResult::Resident);
	REQUIRE_EQ(F.OpResult(B), EPrefixOpResult::Resident);
	REQUIRE_EQ(F.Diag().ChargedBytes, 4 * kPage);
	const int64_t C = F.RequestReady(FixtureSpec(3)); // R(32) = 3: one warmed entry must go
	EXPECT_NE(C, 0);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::BudgetExhausted), 0u);
	// The second warmed entry is evictable too.
	F.Cache.Return(C);
	F.Sim->Pass();
	const int64_t D = F.RequestReady(FixtureSpec(4));
	EXPECT_NE(D, 0);
	EXPECT_EQ(F.Diag().Evictions, 2u);
	EXPECT_TRUE(F.Sim->Violations.empty());
}

HOST_TEST(residency_warm_hit_touches_the_lru, "7.19 [U2]: a warm of a resident key touches the LRU, so it survives an eviction that takes an older entry (mutant: a warm hit that does not touch the LRU -> the warmed entry is evicted first)")
{
	FCacheFixture F(2, false); // 5 pages
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec A = FixtureSpec(1);
	const FPrefixSpec B = FixtureSpec(2);
	REQUIRE_TRUE(F.Resident(A));
	REQUIRE_TRUE(F.Resident(B)); // A is the least recently used
	const uint64_t Op = F.Warm(A);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2)); // a hit: no prefill
	const int64_t C = F.RequestReady(FixtureSpec(3)); // evicts the LRU entry, now B
	REQUIRE_NE(C, 0);
	EXPECT_EQ(F.Diag().Evictions, 1u);
	F.Cache.Return(C);
	F.Sim->Pass();
	EXPECT_EQ(F.SourceOfNextRequest(A), EPrefixSource::Memory);
}

HOST_TEST(residency_warm_and_request_coalesce_in_both_orders, "7.19 [U2] (rev 1.18.1 coalescing, both orders): a request then a warm, and a warm then a request, for one key in one tick share one build (mutant: a warm that does not coalesce -> two prefills)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const uint64_t W1 = F.Warm(S);
	const int64_t H1 = F.Request(S);
	REQUIRE_TRUE(W1 != 0 && H1 != 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.OpResult(W1), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Cache.GetLeaseState(H1), EPrefixLeaseState::Ready);

	const FPrefixSpec T = FixtureSpec(2);
	const int64_t H2 = F.Request(T);
	const uint64_t W2 = F.Warm(T);
	REQUIRE_TRUE(W2 != 0 && H2 != 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));
	EXPECT_EQ(F.OpResult(W2), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Cache.GetLeaseState(H2), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Diag().ResidentEntries, 2u);
}

HOST_TEST(residency_warm_writes_back, "7.19 [U2]: with the disk tier on a warm writes back as a request does")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	const uint64_t Op = F.Warm(S);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Diag().DiskWritesIssued, 1u);
	EXPECT_TRUE(F.DiskFileExists(S));
}

HOST_TEST(residency_warm_follows_the_tier_order, "7.19 [U2] (rev 1.18.1 tier order): a warm on a registered cooked entry restores once and on a disk file reads it once, prefill count 0, each then a Memory hit (mutant: a warm that always recomputes -> prefill count 1)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Cooked = FixtureSpec(1);
	const FPrefixSpec OnDisk = FixtureSpec(2);
	F.PutCooked(Cooked, 77);
	F.PutDiskFile(OnDisk);
	const uint64_t A = F.Warm(Cooked);
	REQUIRE_NE(A, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(A), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::CheckCooked), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Restore), static_cast<size_t>(1));
	const uint64_t B = F.Warm(OnDisk);
	REQUIRE_NE(B, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(B), EPrefixOpResult::Resident);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ReadDisk), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Restore), static_cast<size_t>(2));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
	EXPECT_EQ(F.SourceOfNextRequest(Cooked), EPrefixSource::Memory);
	EXPECT_EQ(F.SourceOfNextRequest(OnDisk), EPrefixSource::Memory);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(0));
}

namespace
{
	// Budget 5, disk on. E (L = 33, 3 pages) resident and unpinned, a 2-page entry leased; releases gated.
	// Y (a 48-token build, 4 pages) evicts E, still falls short and is refused: E's 3-page refund is
	// pending and unclaimed (8.14 (ii)'s construction).
	struct FUnclaimedScene
	{
		FPagesFixture F{5, true};
		uint64_t EHandle = 0;
		bool bReady = false;

		FUnclaimedScene()
		{
			const int64_t HE = F.DiskEntry(FixtureSpec(1, 33));
			if (!F.bConfigured || HE == 0)
			{
				return;
			}
			EHandle = F.Sim->EngineHandleOf(HE);
			F.Cache.Return(HE);
			if (F.DiskEntry(FixtureSpec(2, 32)) == 0)
			{
				return;
			}
			F.Sim->Held.insert(EPrefixCommandKind::Release);
			const int64_t HY = F.Request(FixtureSpec(9, 48));
			F.Sim->RunAll();
			bReady = EHandle != 0 && F.Cache.GetLeaseState(HY) == EPrefixLeaseState::BudgetExhausted &&
				F.Diag().UnclaimedRefundBytes == 3 * kPage && F.Diag().Evictions == 1;
		}
	};
}

HOST_TEST(residency_warm_waits_on_an_unclaimed_refund, "7.19 [U2] (rev 1.18.1 waiting): a warm whose charge fits only by an unclaimed pending refund waits, draws nothing before that release's apply, and is admitted there (mutant: the warm refused instead)")
{
	FUnclaimedScene S;
	REQUIRE_TRUE(S.bReady);
	const size_t Builds = S.F.Sim->CountHistory(EPrefixCommandKind::Build);
	const uint64_t Drawn = S.F.Sim->CacheDrawnPages;
	const uint64_t Op = S.F.Warm(FixtureSpec(3, 32)); // R(32) = 3: only E's refund covers it
	REQUIRE_NE(Op, 0u);
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.OpResult(Op), EPrefixOpResult::Pending);
	EXPECT_EQ(S.F.Diag().Waiters, 1u);
	EXPECT_EQ(S.F.Diag().ClaimedRefundBytes, 3 * kPage);
	EXPECT_EQ(S.F.Diag().Evictions, 1u);
	EXPECT_EQ(S.F.Sim->CountHistory(EPrefixCommandKind::Build), Builds); // drew nothing
	EXPECT_EQ(S.F.Sim->CacheDrawnPages, Drawn);
	EXPECT_EQ(S.F.Fallbacks(EPrefixFallback::BudgetExhausted), 1u);       // Y's only
	REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
	EXPECT_EQ(S.F.Diag().Waiters, 0u);
	EXPECT_EQ(S.F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1)); // admitted at that apply
	S.F.Sim->Held.clear();
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.OpResult(Op), EPrefixOpResult::Resident);
	EXPECT_TRUE(S.F.LedgerOk());
	EXPECT_LE(S.F.Sim->PeakCacheDrawnPages, 5u);
}

HOST_TEST(residency_warm_attaches_to_a_waiting_request, "7.19 [U2] (rev 1.18.1 coalescing): a warm issued while a request for its key is a waiter attaches to that waiter: one build")
{
	FUnclaimedScene S;
	REQUIRE_TRUE(S.bReady);
	const size_t Builds = S.F.Sim->CountHistory(EPrefixCommandKind::Build);
	const FPrefixSpec W = FixtureSpec(3, 32);
	const int64_t H = S.F.Request(W);
	REQUIRE_NE(H, 0);
	S.F.Sim->RunAll();
	REQUIRE_EQ(S.F.Diag().Waiters, 1u);
	const uint64_t Op = S.F.Warm(W);
	REQUIRE_NE(Op, 0u);
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.Diag().Waiters, 1u);
	EXPECT_EQ(S.F.OpResult(Op), EPrefixOpResult::Pending);
	S.F.Sim->Held.clear();
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.Sim->CountHistory(EPrefixCommandKind::Build), Builds + 1);
	EXPECT_EQ(S.F.OpResult(Op), EPrefixOpResult::Resident);
	EXPECT_EQ(S.F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
}

// ---- 2.16 --------------------------------------------------------------------------------------------

HOST_TEST(residency_ops_refuse_malformed_specs_synchronously, "2.16 [U2]: WarmPrefix and DropPrefix with each malformed spec RequestPrefix refuses (empty, out of vocabulary, over context_cap) are refused synchronously and issue no op (mutant: a warm that accepts a malformed spec)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->RunAll();
	const size_t Keys = F.Sim->CountHistory(EPrefixCommandKind::ComputeKey);
	std::vector<FPrefixSpec> Bad;
	Bad.push_back(FPrefixSpec{});
	FPrefixSpec Oov = FixtureSpec(1);
	Oov.Tokens[3] = F.Config.VocabSize;
	Bad.push_back(Oov);
	Oov.Tokens[3] = -1;
	Bad.push_back(Oov);
	Bad.push_back(FixtureSpec(1, 65)); // over cap 64
	for (const FPrefixSpec& Spec : Bad)
	{
		uint64_t Op = 99;
		std::string Error;
		int64_t Lease = 0;
		REQUIRE_EQ(F.Cache.Request(Spec, Lease, Error), EPrefixRequestResult::Malformed);
		Op = 99;
		Error.clear();
		EXPECT_EQ(F.Cache.Warm(Spec, Op, Error), EPrefixRequestResult::Malformed);
		EXPECT_EQ(Op, 0u);
		EXPECT_EQ(Error.empty(), false);
		for (EPrefixDropScope Scope : {EPrefixDropScope::Memory, EPrefixDropScope::MemoryAndDisk})
		{
			Op = 99;
			Error.clear();
			EXPECT_EQ(F.Cache.Drop(Spec, Scope, Op, Error), EPrefixRequestResult::Malformed);
			EXPECT_EQ(Op, 0u);
		}
	}
	F.Sim->Pull();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ComputeKey), Keys);
	EXPECT_EQ(F.Diag().Warms, 0u);
	// A well-formed op is accepted and issued (the control).
	EXPECT_NE(F.Warm(FixtureSpec(1, 64)), 0u);
	F.Sim->Pull();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ComputeKey), Keys + 1);
}

HOST_TEST(residency_op_result_of_an_unissued_id_is_false, "2.16 [U2]: GetPrefixOpResult on an id it never issued returns false")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	FPrefixOpResult Out;
	EXPECT_EQ(F.Cache.GetOpResult(0, Out), false);
	EXPECT_EQ(F.Cache.GetOpResult(987654321, Out), false);
	const uint64_t Op = F.Warm(FixtureSpec(1));
	REQUIRE_NE(Op, 0u);
	EXPECT_EQ(F.Cache.GetOpResult(Op, Out), true);
	EXPECT_EQ(F.Cache.GetOpResult(Op + 1000, Out), false);
}

HOST_TEST(residency_op_results_survive_reconfigure, "2.16 [U2] (rev 1.18.1, reading (b)): a resolved op from before a re-Configure() still reads its own result, and a warm cancelled by it reads Cancelled")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t Warmed = F.Warm(FixtureSpec(1));
	const uint64_t Absent0 = F.Drop(Absent(0));
	REQUIRE_TRUE(Warmed != 0 && Absent0 != 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.OpResult(Warmed), EPrefixOpResult::Resident);
	REQUIRE_EQ(F.OpResult(Absent0), EPrefixOpResult::NotCached);
	const uint64_t Held = F.Warm(FixtureSpec(2));
	REQUIRE_NE(Held, 0u);
	F.Sim->HeldHashOps.insert(Held);
	F.Sim->RunAll();
	REQUIRE_EQ(F.OpResult(Held), EPrefixOpResult::Pending);
	F.Cache.BeginTeardown();
	F.Sim->HeldHashOps.clear();
	F.Sim->RunAll();
	REQUIRE_TRUE(F.Cache.IsTornDown());
	REQUIRE_TRUE(F.Cache.Configure(F.Config));
	bool bFound = false;
	EXPECT_EQ(F.Op(Warmed, &bFound).Result, EPrefixOpResult::Resident);
	EXPECT_TRUE(bFound);
	EXPECT_EQ(F.Op(Absent0, &bFound).Result, EPrefixOpResult::NotCached);
	EXPECT_TRUE(bFound);
	EXPECT_EQ(F.Op(Held, &bFound).Result, EPrefixOpResult::Cancelled);
	EXPECT_TRUE(bFound);
	// Op ids never repeat within the core's lifetime.
	const uint64_t After = F.Warm(FixtureSpec(3));
	EXPECT_TRUE(After != Warmed && After != Absent0 && After != Held && After != 0);
}

HOST_TEST(residency_op_ring_boundary, "2.16 [U2] (rev 1.18.1): the 1,024th most recent resolved op reads its result and the 1,025th reads Expired (mutant: a ring of 1,023 -> the 1,024th reads Expired)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t First = F.Drop(Absent(0));
	REQUIRE_NE(First, 0u);
	F.Sim->RunAll();
	REQUIRE_EQ(F.OpResult(First), EPrefixOpResult::NotCached);
	const uint64_t Second = F.Drop(Absent(1));
	REQUIRE_NE(Second, 0u);
	ResolveDrops(F, 2, static_cast<int32_t>(kPrefixOpRing) - 2); // First is now the 1,024th most recent
	EXPECT_EQ(F.OpResult(First), EPrefixOpResult::NotCached);
	EXPECT_EQ(F.OpResult(Second), EPrefixOpResult::NotCached);
	ResolveDrops(F, 5000, 1);                                     // First is the 1,025th
	EXPECT_EQ(F.OpResult(First), EPrefixOpResult::Expired);
	EXPECT_EQ(F.OpResult(Second), EPrefixOpResult::NotCached);   // now the 1,024th
	bool bFound = false;
	F.Op(First, &bFound);
	EXPECT_TRUE(bFound); // an id it issued, expired, is still not "never issued"
}

HOST_TEST(residency_pending_op_never_expires, "2.16 [U2] (rev 1.18.1): an op still Pending when 1,100 later ops resolve still reads Pending")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held.insert(EPrefixCommandKind::Build);
	const uint64_t Op = F.Warm(FixtureSpec(1));
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	ResolveDrops(F, 0, 1100);
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Pending);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Resident);
}

HOST_TEST(residency_drop_matches_the_whole_key, "2.16 [U2]: DropPrefix on a spec that differs from a resident entry's in one key component (schema, a token, the length) is NotCached and the entry stays a Memory hit (mutant: a drop matched by token ids only -> the entry is evicted)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1, 32, "barter");
	REQUIRE_TRUE(F.Resident(E));
	std::vector<FPrefixSpec> Near;
	FPrefixSpec V = E;
	V.SchemaName.clear();
	Near.push_back(V);
	V.SchemaName = "trade";
	Near.push_back(V);
	V = E;
	V.Tokens[5] = (V.Tokens[5] + 1) % F.Config.VocabSize;
	Near.push_back(V);
	V = E;
	V.Tokens.pop_back();
	Near.push_back(V);
	V = E;
	V.Tokens.push_back(3);
	Near.push_back(V);
	for (const FPrefixSpec& Spec : Near)
	{
		const uint64_t Op = F.Drop(Spec, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::NotCached);
	}
	EXPECT_EQ(F.Diag().Drops, 0u);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	EXPECT_EQ(F.SourceOfNextRequest(E), EPrefixSource::Memory);
}

// ---- 7.8 (host half) ---------------------------------------------------------------------------------

HOST_TEST(residency_ops_hash_nothing_on_the_game_thread, "7.8 [U2] (host half, rev 1.18.1 audit G12): WarmPrefix and DropPrefix hash nothing and decide nothing on the call: each emits one ComputeKey carrying its op id, and the op stays Pending until that completion (mutant: the key hashed inside DropPrefix to decide synchronously)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1, 32, "barter");
	const FPrefixSpec W = FixtureSpec(2);
	REQUIRE_TRUE(F.Resident(E));
	F.Sim->RunAll();
	F.Sim->Pull();
	REQUIRE_TRUE(F.Sim->Queue.empty());
	const uint64_t OpW = F.Warm(W);
	const uint64_t OpD = F.Drop(E);
	REQUIRE_TRUE(OpW != 0 && OpD != 0 && OpW != OpD);
	// Nothing decided on the call: the entry is still resident, nothing released, both ops Pending.
	EXPECT_EQ(F.OpResult(OpW), EPrefixOpResult::Pending);
	EXPECT_EQ(F.OpResult(OpD), EPrefixOpResult::Pending);
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
	EXPECT_EQ(F.Diag().Drops, 0u);
	F.Sim->Pull();
	REQUIRE_EQ(F.Sim->Queue.size(), static_cast<size_t>(2));
	const FPrefixCommand& CW = F.Sim->Queue[0];
	const FPrefixCommand& CD = F.Sim->Queue[1];
	EXPECT_EQ(CW.Kind, EPrefixCommandKind::ComputeKey);
	EXPECT_EQ(CW.OpId, OpW);
	EXPECT_EQ(CW.LeaseHandle, 0);
	EXPECT_EQ(CW.KeyInputs.Tokens == W.Tokens, true);
	EXPECT_EQ(CD.Kind, EPrefixCommandKind::ComputeKey);
	EXPECT_EQ(CD.OpId, OpD);
	EXPECT_EQ(CD.LeaseHandle, 0);
	EXPECT_EQ(CD.KeyInputs.Tokens == E.Tokens, true);
	EXPECT_EQ(CD.KeyInputs.SchemaName, E.SchemaName);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(0));
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(OpW), EPrefixOpResult::Resident);
	EXPECT_EQ(F.OpResult(OpD), EPrefixOpResult::Dropped);
}

// ---- 5.12 --------------------------------------------------------------------------------------------

HOST_TEST(residency_warm_fallbacks, "5.12 [U2]: a warm with every entry leased and a charge that cannot fit resolves BudgetExhausted, nothing drawn; an unknown schema UnknownSchema, no entry, no file; a nested spec before U6 is refused NestedRequiresBudgetMode synchronously; with the pool filled from outside (5.5's construction) PoolExhausted")
{
	{
		FCacheFixture F(2, false);
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_NE(F.RequestReady(FixtureSpec(1)), 0);
		REQUIRE_NE(F.RequestReady(FixtureSpec(2)), 0); // both leased
		const uint64_t Drawn = F.Sim->CacheDrawnPages;
		const uint64_t Op = F.Warm(FixtureSpec(3));
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::BudgetExhausted);
		EXPECT_EQ(F.Sim->CacheDrawnPages, Drawn);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));
		EXPECT_EQ(F.Diag().Evictions, 0u);
	}
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec S = FixtureSpec(5, 32, "missing_schema");
		const uint64_t Op = F.Warm(S);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::UnknownSchema);
		EXPECT_EQ(F.Diag().ResidentEntries, 0u);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0));
		EXPECT_EQ(F.DiskFileExists(S), false);
	}
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const int64_t Parent = F.RequestReady(FixtureSpec(1));
		REQUIRE_NE(Parent, 0);
		FPrefixSpec Nested = FixtureSpec(2, 16);
		Nested.ParentHandle = Parent;
		uint64_t Op = 99;
		std::string Error;
		EXPECT_EQ(F.Cache.Warm(Nested, Op, Error), EPrefixRequestResult::NestedRequiresBudgetMode);
		EXPECT_EQ(Op, 0u);
	}
	{
		FCacheFixture F(4, false, 0);
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_NE(F.RequestReady(FixtureSpec(1)), 0);
		REQUIRE_NE(F.RequestReady(FixtureSpec(2)), 0);
		F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 2;
		const uint64_t Op = F.Warm(FixtureSpec(3));
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::PoolExhausted);
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::PoolExhausted), 1u);
	}
}

HOST_TEST(residency_drop_disk_part_outcomes, "5.12 [U2]: DropPrefix(MemoryAndDisk) with the disk tier off applies the memory part and reads DiskTierOff; on a key with no file NoFile; with a delete the file API refuses Failed, one log line; DropPrefix(Memory) reads NotRequested; on a key not resident with a disk file NotCached and Deleted, the file gone")
{
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec A = FixtureSpec(1);
		REQUIRE_TRUE(F.Resident(A));
		const uint64_t Op = F.Drop(A, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::DiskTierOff);
		EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	}
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	{
		const FPrefixSpec B = FixtureSpec(2);
		REQUIRE_TRUE(F.Resident(B));
		F.Sim->RunAll();
		REQUIRE_TRUE(F.DiskFileExists(B));
		const uint64_t Op = F.Drop(B, EPrefixDropScope::Memory);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::NotRequested);
		EXPECT_TRUE(F.DiskFileExists(B));
	}
	{
		const FPrefixSpec C = FixtureSpec(3);
		REQUIRE_TRUE(F.Resident(C));
		F.Sim->RunAll();
		F.Sim->Fs->Files.erase(F.Sim->DiskPath(FixtureKey(C)));
		const uint64_t Op = F.Drop(C, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::NoFile);
	}
	{
		const FPrefixSpec D = FixtureSpec(4);
		REQUIRE_TRUE(F.Resident(D));
		F.Sim->RunAll();
		F.Cache.TakeLog();
		F.Sim->Fs->bFailRemoves = true;
		const uint64_t Op = F.Drop(D, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		F.Sim->Fs->bFailRemoves = false;
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::Failed);
		EXPECT_EQ(F.Cache.TakeLog().size(), static_cast<size_t>(1));
		EXPECT_TRUE(F.DiskFileExists(D));
	}
	{
		const FPrefixSpec E = FixtureSpec(5);
		F.PutDiskFile(E); // a stale file, nothing resident
		const uint64_t Op = F.Drop(E, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::NotCached);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::Deleted);
		EXPECT_EQ(F.DiskFileExists(E), false);
	}
	EXPECT_EQ(F.Diag().Drops, 3u);
	EXPECT_EQ(F.Diag().Evictions, 0u); // drops are counted apart
}

HOST_TEST(residency_drop_never_reaches_the_cooked_tier, "5.12 [U2]: a drop on a cooked entry's key leaves the set registered, and the next request is a Cooked hit (mutant: the drop's disk part marks the set entry stale -> the Cooked hit is lost)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	F.PutCooked(S, 41);
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	REQUIRE_EQ(F.Cache.GetSource(H), EPrefixSource::Cooked);
	F.Cache.Return(H);
	F.Sim->Pass();
	const uint64_t Op = F.Drop(S, EPrefixDropScope::MemoryAndDisk);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
	EXPECT_EQ(F.Cache.IsCookedStale(41), false);
	EXPECT_EQ(F.SourceOfNextRequest(S), EPrefixSource::Cooked);
}

// ---- 7.18 --------------------------------------------------------------------------------------------

HOST_TEST(residency_drop_of_a_leased_entry_never_invalidates_a_lease, "7.18 [U2]: with two leases on E and an adopt of it open, DropPrefix(E) is DropDeferred; both leases adopt the twin's tokens and E is unreleased; after the adopt drains and lease 1 returns E stays; after lease 2 returns E is evicted at that pass (marks applied +1), its refund pending until its release applies (mutant m1: a drop that releases a pinned entry)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1);
	const int64_t L1 = F.RequestReady(E);
	const int64_t L2 = F.RequestReady(E);
	REQUIRE_TRUE(L1 != 0 && L2 != 0);
	uint64_t EHandle = 0;
	uint64_t AdoptToken = 0;
	REQUIRE_EQ(F.Cache.BeginAdopt(L1, EHandle, AdoptToken), EPrefixAdoptCheck::Adoptable); // the queued adopt
	const uint64_t Op = F.Drop(E);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::DropDeferred);
	EXPECT_EQ(F.Diag().DropsDeferred, 1u);
	// Every lease stays valid and adopts the twin's tokens; E is not released.
	for (int64_t L : {L1, L2})
	{
		const FEngineHandle* Handle = F.Sim->HandleOf(L);
		REQUIRE_TRUE(Handle != nullptr);
		EXPECT_EQ(Handle->Tokens == E.Tokens, true);
		EXPECT_EQ(Handle->bReleased, false);
	}
	EXPECT_EQ(F.Sim->ReleaseCount.count(EHandle), static_cast<size_t>(0));
	EXPECT_TRUE(F.Cache.EndAdopt(AdoptToken));
	F.Sim->Pass();
	EXPECT_TRUE(F.Cache.Return(L1));
	F.Sim->Pass();
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	EXPECT_EQ(F.Diag().MarksApplied, 0u);
	// The last pin goes: E is evicted at that pass (no op of this fixture has a pending hash, rev 1.18.3 m3).
	F.Sim->Held.insert(EPrefixCommandKind::Release);
	EXPECT_TRUE(F.Cache.Return(L2));
	F.Sim->Pass();
	EXPECT_EQ(F.Diag().MarksApplied, 1u);
	EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Diag().ReleasingBytes, 2 * kPage); // its refund pending until its release applies
	REQUIRE_TRUE(F.Sim->RunRelease(EHandle));
	EXPECT_EQ(F.Diag().ReleasingBytes, 0u);
	EXPECT_EQ(F.Sim->ReleaseCount[EHandle], 1);
	EXPECT_TRUE(F.Sim->Violations.empty());
}

HOST_TEST(residency_drop_during_a_build_serves_it_then_applies, "7.18 [U2]: a drop on a key whose build is in flight is DropDeferred; the build completes and serves its requester, and the mark applies when that lease is returned")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec E = FixtureSpec(1);
	F.Sim->Held.insert(EPrefixCommandKind::Build);
	const int64_t H = F.Request(E);
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	const uint64_t Op = F.Drop(E);
	REQUIRE_NE(Op, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::DropDeferred);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
	EXPECT_EQ(F.Diag().MarksApplied, 0u);
	EXPECT_TRUE(F.Cache.Return(H));
	F.Sim->Pass();
	EXPECT_EQ(F.Diag().MarksApplied, 1u);
	EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(1));
}

HOST_TEST(residency_a_later_request_clears_the_mark, "7.18 [U2]: a RequestPrefix for E between the drop and the last return clears the mark and E stays resident after every return; so does one issued after the last return and before that frame's pass with its hash held past the pass (3.13's T1 run) (mutant: the mark not cleared by a later request)")
{
	for (int Leg = 0; Leg < 2; ++Leg)
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec E = FixtureSpec(1);
		const int64_t L1 = F.RequestReady(E);
		REQUIRE_NE(L1, 0);
		const uint64_t Op = F.Drop(E);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		REQUIRE_EQ(F.OpResult(Op), EPrefixOpResult::DropDeferred);
		int64_t L2 = 0;
		if (Leg == 0)
		{
			L2 = F.RequestReady(E); // between the drop and the last return
			REQUIRE_NE(L2, 0);
			EXPECT_EQ(F.Cache.GetSource(L2), EPrefixSource::Memory);
			EXPECT_TRUE(F.Cache.Return(L1));
			F.Sim->Pass();
			EXPECT_TRUE(F.Cache.Return(L2));
			F.Sim->Pass();
		}
		else
		{
			EXPECT_TRUE(F.Cache.Return(L1)); // the last return ...
			L2 = F.Request(E);               // ... and, in the same frame, the request
			REQUIRE_NE(L2, 0);
			F.Sim->HeldHashLeases.insert(L2);
			F.Sim->Pass();                   // that frame's pass, the request's hash still held
			EXPECT_EQ(F.Diag().MarksApplied, 0u);
			REQUIRE_TRUE(F.Sim->RunHashOfLease(L2));
			F.Sim->RunAll();
			EXPECT_EQ(F.Cache.GetSource(L2), EPrefixSource::Memory);
			EXPECT_TRUE(F.Cache.Return(L2));
			F.Sim->Pass();
		}
		F.Sim->RunAll();
		EXPECT_EQ(F.Diag().ResidentEntries, 1u);
		EXPECT_EQ(F.Diag().MarksCleared, 1u);
		EXPECT_EQ(F.Diag().MarksApplied, 0u);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	}
}

HOST_TEST(residency_drop_of_a_waiting_key, "7.18 [U2] (rev 1.18.1 audit G12): a drop of a key whose request is a waiter reads DropDeferred; the waiter is admitted and served, and the mark applies at its lease's return")
{
	FUnclaimedScene S;
	REQUIRE_TRUE(S.bReady);
	const FPrefixSpec W = FixtureSpec(3, 32);
	const int64_t H = S.F.Request(W);
	REQUIRE_NE(H, 0);
	S.F.Sim->RunAll();
	REQUIRE_EQ(S.F.Diag().Waiters, 1u);
	const uint64_t Op = S.F.Drop(W);
	REQUIRE_NE(Op, 0u);
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.OpResult(Op), EPrefixOpResult::DropDeferred);
	EXPECT_EQ(S.F.Diag().Waiters, 1u);
	REQUIRE_TRUE(S.F.Sim->RunRelease(S.EHandle));
	S.F.Sim->Held.clear();
	S.F.Sim->RunAll();
	EXPECT_EQ(S.F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(S.F.Diag().MarksApplied, 0u);
	EXPECT_TRUE(S.F.Cache.Return(H));
	S.F.Sim->Pass();
	EXPECT_EQ(S.F.Diag().MarksApplied, 1u);
	S.F.Sim->RunAll();
	EXPECT_TRUE(S.F.LedgerOk());
	EXPECT_TRUE(S.F.Sim->Violations.empty());
}

// ---- 8.14 drop legs (host half) ----------------------------------------------------------------------

namespace
{
	// 8.14's E: L = 2B + 1 = 33, k = 3 pages, adopted twice, every lease returned; a 2-page entry leased; C
	// full (5); E's release gated. Returns E's engine handle, 0 on a failed setup.
	uint64_t DropScene(FPagesFixture& F)
	{
		const int64_t HE = F.DiskEntry(FixtureSpec(1, 33));
		if (!F.bConfigured || HE == 0)
		{
			return 0;
		}
		const uint64_t EHandle = F.Sim->EngineHandleOf(HE);
		if (EHandle == 0 || F.Sim->EngineHandleOf(HE) != EHandle) // two adopts
		{
			return 0;
		}
		F.Cache.Return(HE);
		F.Sim->Pass();
		return F.DiskEntry(FixtureSpec(2, 32)) != 0 ? EHandle : 0;
	}

	// The drop leg on F with C full: the readings shared by the legacy and the budget-mode legs.
	void DropLeg(FPagesFixture& F, uint64_t EHandle, const std::function<bool()>& Form1)
	{
		REQUIRE_EQ(F.Diag().ChargedBytes, F.Diag().BudgetBytes);
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		const uint64_t Op = F.Drop(FixtureSpec(1, 33));
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		REQUIRE_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		// The required reading between the drop's decision and its release's apply.
		FPrefixCacheDiagnostics D = F.Diag();
		EXPECT_EQ(D.ResidentEntries, 1u);            // E is out of the map
		EXPECT_EQ(D.ReleasingBytes, 3 * kPage);      // C still carries E's k
		EXPECT_EQ(D.UnclaimedRefundBytes, 3 * kPage); // no request claims it
		EXPECT_EQ(D.ChargedBytes, D.BudgetBytes);
		EXPECT_EQ(D.Drops, 1u);
		EXPECT_EQ(D.Evictions, 0u);                  // drops are counted apart
		EXPECT_EQ(F.Sim->CacheDrawnPages, 5u);       // E's pages still held
		EXPECT_TRUE(F.LedgerOk());
		EXPECT_TRUE(Form1());
		// A request in that window whose charge fits only with E's refund claims it and waits.
		const size_t Builds = F.Sim->CountHistory(EPrefixCommandKind::Build);
		const int64_t HG = F.Request(FixtureSpec(3, 32));
		REQUIRE_NE(HG, 0);
		F.Sim->RunAll();
		D = F.Diag();
		EXPECT_EQ(D.Waiters, 1u);
		EXPECT_EQ(D.ClaimedRefundBytes, 3 * kPage);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), Builds); // begin job not dispatched
		EXPECT_EQ(F.Sim->CacheDrawnPages, 5u);
		EXPECT_TRUE(F.LedgerOk());
		EXPECT_TRUE(Form1());
		// At E's apply it is admitted with its charge complete.
		REQUIRE_TRUE(F.Sim->RunRelease(EHandle));
		D = F.Diag();
		EXPECT_EQ(D.Waiters, 0u);
		EXPECT_EQ(D.InFlightBytes, 3 * kPage);
		EXPECT_EQ(D.ChargedBytes, D.BudgetBytes);
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.Cache.GetLeaseState(HG), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Diag().Evictions, 0u);
		EXPECT_LE(F.Sim->PeakCacheDrawnPages, 5u);
		EXPECT_TRUE(F.Sim->Violations.empty());
		EXPECT_TRUE(Form1());
	}
}

HOST_TEST(residency_drop_is_an_unclaimed_pending_refund_legacy, "8.14 [U2] drop leg, legacy mode (rev 1.18.1 audit G12; the model's m2): DropPrefix(E) is Dropped, E out of the map, its k = 3 still charged and unclaimed at the reading between the decision and the release's apply; a request in that window claims it, waits, draws nothing, and is admitted at E's apply; the eviction diagnostic unchanged (mutant: refund at the decision -> admitted at once, drawing while E's pages are held)")
{
	FPagesFixture F(5, true);
	const uint64_t EHandle = DropScene(F);
	REQUIRE_NE(EHandle, 0u);
	DropLeg(F, EHandle, [&F]() { return F.Sim->CacheDrawnPages <= F.Diag().ChargedBytes / kPage; });
}

HOST_TEST(residency_drop_is_an_unclaimed_pending_refund_budget_mode, "8.14 [U2] drop leg, budget mode (region C on paging's ledger, §5.11 (b)): as the legacy leg, with region C mirroring every charge, zero LedgerFaults, and the fill probe within C's charge at every reading")
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
	LC.CacheRegionPages = 5;
	REQUIRE_TRUE(Paging.Configure(LC) == ELedgerConfigure::Ok);
	REQUIRE_EQ(Paging.RegionCapacity(ERegion::C), 5u);
	FPagesFixture F(5, true);
	REQUIRE_TRUE(F.bConfigured);
	FPrefixCacheConfig C = F.Config;
	C.RegionC = &Paging;
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	REQUIRE_TRUE(F.Cache.Configure(C));
	F.Config = C;
	F.Sim->Config = C;
	REQUIRE_EQ(F.Diag().BudgetBytes, 5 * kPage);
	const uint64_t EHandle = DropScene(F);
	REQUIRE_NE(EHandle, 0u);
	DropLeg(F, EHandle, [&]() {
		return Paging.CacheRegionChargedBytes() == F.Diag().ChargedBytes && Paging.LedgerFaults() == 0 &&
			F.Diag().RegionChargeFaults == 0 && F.Sim->CacheDrawnPages <= Paging.RegionCharged(ERegion::C);
	});
}

// ---- 8.17 --------------------------------------------------------------------------------------------

HOST_TEST(residency_key_recreated_while_its_earlier_instance_is_pending, "8.17 [U2]: E's release gated, DropPrefix(E) then RequestPrefix(E): the request builds a new instance (waiting on E's own refund, and in a fresh run admitted at once); the ledger holds the old pending k plus the new charge; at the old release's apply exactly k refunds and the new instance stays a Memory hit (mutant: instances keyed by key -> the new charge overwrites the pending one)")
{
	for (const uint64_t Budget : {5ull, 7ull}) // 5: waits on E's own refund; 7: admitted at once
	{
		FPagesFixture F(Budget, false);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec E = FixtureSpec(1, 32);
		const int64_t HE = F.RequestReady(E); // a build settling at 2 pages
		REQUIRE_NE(HE, 0);
		const uint64_t EHandle = F.Sim->EngineHandleOf(HE);
		F.Cache.Return(HE);
		F.Sim->Pass();
		REQUIRE_NE(F.RequestReady(FixtureSpec(2, 32)), 0); // 2 pages, leased
		F.Sim->Held.insert(EPrefixCommandKind::Release);
		// The new instance's build is held too, so the reading below is taken with its charge in flight: at
		// budget 7 it is admitted at once and would otherwise complete inside RunAll and settle at 2 pages.
		F.Sim->Held.insert(EPrefixCommandKind::Build);
		const uint64_t Op = F.Drop(E);
		const int64_t H = F.Request(E);
		REQUIRE_TRUE(Op != 0 && H != 0);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		// The required reading: the old pending k = 2 plus the new instance's charge R(32) = 3.
		FPrefixCacheDiagnostics D = F.Diag();
		EXPECT_EQ(D.ReleasingBytes, 2 * kPage);
		EXPECT_EQ(D.ChargedBytes, (2 + 2 + 3) * kPage - (Budget == 5 ? 2 * kPage : 0)); // a waiter holds only the free charge
		EXPECT_EQ(D.Waiters, Budget == 5 ? 1u : 0u);
		EXPECT_EQ(D.WaitingBytes + D.InFlightBytes, (Budget == 5 ? 1 : 3) * kPage);
		EXPECT_TRUE(F.LedgerOk());
		EXPECT_LE(F.Sim->CacheDrawnPages, D.ChargedBytes / kPage); // physical within the charges
		REQUIRE_TRUE(F.Sim->RunRelease(EHandle));
		EXPECT_EQ(F.Diag().ReleasingBytes, 0u);
		EXPECT_EQ(F.Sim->ReleaseCount[EHandle], 1);
		F.Sim->Held.clear();
		F.Sim->RunAll();
		D = F.Diag();
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
		EXPECT_EQ(D.ChargedBytes, 4 * kPage); // exactly k refunded: two resident entries of 2
		EXPECT_EQ(D.ResidentEntries, 2u);
		EXPECT_TRUE(F.LedgerOk());
		EXPECT_EQ(F.Sim->CacheDrawnPages, 4u);
		F.Cache.Return(H);
		F.Sim->Pass();
		EXPECT_EQ(F.SourceOfNextRequest(E), EPrefixSource::Memory);
		EXPECT_TRUE(F.Sim->Violations.empty());
	}
}

// ---- 1.10 --------------------------------------------------------------------------------------------

HOST_TEST(residency_churn_warm_request_adopt_return_drop, "1.10 [U2]: 1,000 cycles of warm -> request -> adopt -> return -> drop over mixed lengths and keys, drops of leased and unleased entries interleaved; at the end, after teardown, the pool is at its initial count, no handle released twice and none left unreleased, and the ledger reads 0 (mutants: a leased drop released at once and again at its return; a dropped charge never refunded)")
{
	FCacheFixture F(3, false); // 7 pages
	REQUIRE_TRUE(F.bConfigured);
	const int32_t Lengths[] = {16, 20, 32, 33, 48};
	uint32_t X = 12345;
	auto Next = [&X](uint32_t N) {
		X = X * 1664525u + 1013904223u;
		return (X >> 8) % N;
	};
	uint64_t Dropped = 0;
	uint64_t Deferred = 0;
	for (int Cycle = 0; Cycle < 1000; ++Cycle)
	{
		const int32_t Key = static_cast<int32_t>(Next(12));
		const FPrefixSpec S = FixtureSpec(Key, Lengths[Key % 5]);
		REQUIRE_NE(F.Warm(S), 0u);
		F.Sim->RunAll();
		const int64_t H = F.Request(S);
		REQUIRE_NE(H, 0);
		F.Sim->RunAll();
		if (F.Cache.GetLeaseState(H) == EPrefixLeaseState::Ready)
		{
			REQUIRE_NE(F.Sim->EngineHandleOf(H), 0u); // the adopt
		}
		const bool bLeasedDrop = Next(2) == 0;
		uint64_t Op = 0;
		if (bLeasedDrop)
		{
			Op = F.Drop(S);
			F.Sim->RunAll();
			F.Cache.Return(H);
			F.Sim->Pass();
		}
		else
		{
			F.Cache.Return(H);
			F.Sim->Pass();
			Op = F.Drop(S);
		}
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		const EPrefixOpResult R = F.OpResult(Op);
		Dropped += R == EPrefixOpResult::Dropped ? 1 : 0;
		Deferred += R == EPrefixOpResult::DropDeferred ? 1 : 0;
		REQUIRE_TRUE(F.Sim->Violations.empty());
		const FPrefixCacheDiagnostics D = F.Diag();
		REQUIRE_TRUE(D.ChargedBytes <= D.BudgetBytes);
		REQUIRE_TRUE(F.Sim->CacheDrawnPages <= D.ChargedBytes / kFixturePageBytes);
	}
	EXPECT_GT(Dropped, 0u);
	EXPECT_GT(Deferred, 0u);
	EXPECT_TRUE(F.TeardownWithin());
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u); // the pool at its initial count
	EXPECT_TRUE(F.EveryHandleReleasedOnce());
	EXPECT_EQ(F.Diag().ChargedBytes, 0u);
	EXPECT_TRUE(F.Sim->Violations.empty());
}

namespace
{
	// 5.4's form for one row (suite audit G4): exactly one log line, carrying the row's reason name and
	// the key, and that reason's counter raised by exactly one with every other reason's unchanged.
	void ExpectRowForm(const FCacheFixture& F, const std::vector<FPrefixLogLine>& Log, const uint64_t (&Before)[kPrefixFallbackCount],
		EPrefixFallback Reason, const FPrefixKey& Key)
	{
		EXPECT_EQ(Log.size(), static_cast<size_t>(1));
		EXPECT_EQ(F.LogLinesWith(Log, Reason), static_cast<size_t>(1));
		const std::string Hex = PrefixKeyToHex(Key);
		bool bKeyed = false;
		for (const FPrefixLogLine& Line : Log)
		{
			bKeyed |= Line.Message.find(Hex) != std::string::npos;
		}
		EXPECT_TRUE(bKeyed);
		for (uint32_t R = 0; R < kPrefixFallbackCount; ++R)
		{
			EXPECT_EQ(F.Diag().Fallbacks[R] - Before[R], R == static_cast<uint32_t>(Reason) ? 1u : 0u);
		}
	}

	void Snapshot(const FCacheFixture& F, uint64_t (&Out)[kPrefixFallbackCount])
	{
		for (uint32_t R = 0; R < kPrefixFallbackCount; ++R)
		{
			Out[R] = F.Diag().Fallbacks[R];
		}
	}
}

HOST_TEST(residency_warm_rows_have_the_5_4_form, "5.12 / 5.4 [U2] (rev 1.18.4, §5.9 Reason codes; suite audit G4): each warm row of §5.9 (BudgetExhausted, UnknownSchema, PoolExhausted) gives its named result, one log line carrying the reason and the key, and that reason's counter raised by one, every other reason's unchanged (mutants: a row logged under another reason; a counter not raised)")
{
	uint64_t Before[kPrefixFallbackCount] = {};
	{
		FCacheFixture F(2, false);
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_NE(F.RequestReady(FixtureSpec(1)), 0);
		REQUIRE_NE(F.RequestReady(FixtureSpec(2)), 0); // both leased
		F.Cache.TakeLog();
		Snapshot(F, Before);
		const uint64_t Op = F.Warm(FixtureSpec(3));
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::BudgetExhausted);
		ExpectRowForm(F, F.Cache.TakeLog(), Before, EPrefixFallback::BudgetExhausted, FixtureKey(FixtureSpec(3)));
	}
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec S = FixtureSpec(5, 32, "missing_schema");
		F.Cache.TakeLog();
		Snapshot(F, Before);
		const uint64_t Op = F.Warm(S);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::UnknownSchema);
		ExpectRowForm(F, F.Cache.TakeLog(), Before, EPrefixFallback::UnknownSchema, FixtureKey(S));
	}
	{
		FCacheFixture F(4, false, 0);
		REQUIRE_TRUE(F.bConfigured);
		REQUIRE_NE(F.RequestReady(FixtureSpec(1)), 0);
		REQUIRE_NE(F.RequestReady(FixtureSpec(2)), 0);
		F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 2;
		F.Cache.TakeLog();
		Snapshot(F, Before);
		const uint64_t Op = F.Warm(FixtureSpec(3));
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::PoolExhausted);
		ExpectRowForm(F, F.Cache.TakeLog(), Before, EPrefixFallback::PoolExhausted, FixtureKey(FixtureSpec(3)));
	}
}

HOST_TEST(residency_drop_rows_have_the_5_4_form, "5.12 / 5.4 [U2] (rev 1.18.4, §5.9 Reason codes; suite audit G4): a refused drop delete reads Disk == Failed, its memory part's result kept, with one log line carrying DropDeleteFailed and the key, and DropDeleteFailed's counter raised by exactly one, every other reason's unchanged; a deferred drop raises the drop diagnostics' Deferred by one, logs nothing and leaves every reason counter unchanged (mutants: logged under another reason; the counter not raised)")
{
	uint64_t Before[kPrefixFallbackCount] = {};
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	{
		const FPrefixSpec A = FixtureSpec(1);
		const int64_t H = F.RequestReady(A); // leased: pinned
		REQUIRE_NE(H, 0);
		F.Sim->RunAll();
		F.Cache.TakeLog();
		Snapshot(F, Before);
		const uint64_t Deferred = F.Diag().DropsDeferred;
		const size_t Releases = F.Sim->CountHistory(EPrefixCommandKind::Release);
		const uint64_t Op = F.Drop(A, EPrefixDropScope::Memory);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::DropDeferred);
		EXPECT_EQ(F.Diag().DropsDeferred - Deferred, 1u);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), Releases);
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Cache.TakeLog().size(), static_cast<size_t>(0));
		for (uint32_t R = 0; R < kPrefixFallbackCount; ++R)
		{
			EXPECT_EQ(F.Diag().Fallbacks[R], Before[R]);
		}
		F.Cache.Return(H);
		F.Sim->RunAll();
	}
	{
		const FPrefixSpec D = FixtureSpec(4);
		REQUIRE_TRUE(F.Resident(D));
		F.Sim->RunAll();
		F.Cache.TakeLog();
		Snapshot(F, Before);
		F.Sim->Fs->bFailRemoves = true;
		const uint64_t Op = F.Drop(D, EPrefixDropScope::MemoryAndDisk);
		REQUIRE_NE(Op, 0u);
		F.Sim->RunAll();
		F.Sim->Fs->bFailRemoves = false;
		EXPECT_EQ(F.OpResult(Op), EPrefixOpResult::Dropped);
		EXPECT_EQ(F.Op(Op).Disk, EPrefixDropDisk::Failed);
		ExpectRowForm(F, F.Cache.TakeLog(), Before, EPrefixFallback::DropDeleteFailed, FixtureKey(D));
	}
}
