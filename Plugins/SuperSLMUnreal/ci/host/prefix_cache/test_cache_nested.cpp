// Nested cache entries (precache plan rev 1.17 §5.2 parent key, §5.4 charge rule, Ghost charges and
// Pins, §5.11 (a) Pending refunds wait on their family; step U6), on the simulator's refcount-accurate
// fill probe (CacheDrawnPages: every page some cache-owned engine handle still maps, shared pages
// counted once). Host halves of cells 1.9, 2.12 (parent key), 8.12 and 8.14's nested legs, in pages:
// B = 16, cap = 256, one page = 4,096 bytes. Releases are gated (held) wherever a leg says so.

#include "harness.h"
#include "sim.h"

#include <map>
#include <memory>
#include <set>

using namespace HostSim;

namespace
{
	struct FSeeded
	{
		FPrefixKey Key;
		std::vector<int32_t> Tokens; // every position, the parent's included (what an adopter sees)
		uint64_t Engine = 0;
	};

	struct FNestFx
	{
		int64_t Counter = 1;
		FSuperSLMPrefixCacheCore Cache{&Counter};
		FPrefixCacheConfig Config;
		std::unique_ptr<FSim> Sim;
		bool bConfigured = false;
		uint64_t NextBlob = 1;
		std::map<int64_t, FSeeded> Seeded; // lease -> what it names

		explicit FNestFx(uint64_t BudgetPages, bool bNested = true, uint64_t Slack = 16)
		{
			Config = FixtureConfig(1, false);
			Config.ContextCap = 256;
			Config.BudgetBytes = BudgetPages * kFixturePageBytes;
			Config.bNestedEntries = bNested;
			bConfigured = Cache.Configure(Config);
			Sim.reset(new FSim(Cache, Config, BudgetPages + Slack));
		}

		uint64_t Pages(uint64_t Bytes) const { return Bytes / kFixturePageBytes; }
		uint64_t Charged() const { return Pages(Cache.GetDiagnostics().ChargedBytes); }

		// The stored spec of Spec as a child of Parent's lease (or a root).
		FPrefixStoredSpec Stored(const FPrefixSpec& Spec, int64_t Parent)
		{
			FPrefixStoredSpec S = StoredOf(Spec);
			if (Parent != 0)
			{
				S.bHasParent = true;
				S.ParentKey = Seeded[Parent].Key;
			}
			return S;
		}

		// Registers a cooked wrapper for Spec (a child of Parent's lease when given), so its request is a
		// restore charged ceil(L/B) of its whole length.
		void Cook(const FPrefixSpec& Spec, int64_t Parent = 0)
		{
			const FPrefixStoredSpec S = Stored(Spec, Parent);
			const FPrefixKey Key = ComputePrefixKey(KeyInputsFor(Config.Identity, S));
			std::vector<int32_t> All = Parent != 0 ? Seeded[Parent].Tokens : std::vector<int32_t>();
			All.insert(All.end(), Spec.Tokens.begin(), Spec.Tokens.end());
			const std::vector<uint8_t> Payload = MakeFakePayload(Spec.SchemaName, All);
			const uint64_t Blob = NextBlob++;
			Sim->Cooked[Blob] = EncodePrefixWrapper(Config.Identity, Key, S, Payload.data(), Payload.size());
			Cache.RegisterCooked(Key, Blob);
		}

		int64_t Request(FPrefixSpec Spec, int64_t Parent = 0)
		{
			Spec.ParentHandle = Parent;
			int64_t Handle = 0;
			std::string Error;
			return Cache.Request(Spec, Handle, Error) == EPrefixRequestResult::Accepted ? Handle : 0;
		}

		// Requests Spec (a child of Parent's lease when given) and runs everything; the lease if Ready,
		// recorded for later children; 0 otherwise. bCooked: through the cooked tier, else built.
		int64_t Seed(const FPrefixSpec& Spec, int64_t Parent = 0, bool bCooked = true)
		{
			if (bCooked)
			{
				Cook(Spec, Parent);
			}
			const int64_t Handle = Request(Spec, Parent);
			Sim->RunAll();
			if (Handle == 0 || Cache.GetLeaseState(Handle) != EPrefixLeaseState::Ready)
			{
				return 0;
			}
			FSeeded& S = Seeded[Handle];
			S.Key = ComputePrefixKey(KeyInputsFor(Config.Identity, Stored(Spec, Parent)));
			S.Tokens = Parent != 0 ? Seeded[Parent].Tokens : std::vector<int32_t>();
			S.Tokens.insert(S.Tokens.end(), Spec.Tokens.begin(), Spec.Tokens.end());
			S.Engine = Sim->EngineHandleOf(Handle);
			return Handle;
		}

		// Makes the entry behind Victim's lease a ghost: Victim's lease is returned, every lease in Hold
		// pins its entry, a one-page cooked restore evicts Victim alone and is refused, and Victim's
		// release applies. Releases must be held by the caller.
		bool Ghost(int64_t Victim, int32_t ZIndex)
		{
			const uint64_t Engine = Seeded[Victim].Engine;
			const uint32_t GhostsBefore = Cache.GetDiagnostics().Ghosts;
			Cache.Return(Victim);
			const FPrefixSpec Z = FixtureSpec(ZIndex, 16);
			Cook(Z);
			const int64_t Zh = Request(Z);
			Sim->RunAll();
			return Cache.GetLeaseState(Zh) == EPrefixLeaseState::BudgetExhausted && Sim->CountQueued(EPrefixCommandKind::Release) == 1 &&
				Cache.GetDiagnostics().Ghosts == GhostsBefore + 1u && Sim->RunRelease(Engine);
		}

		bool Clean() const { return Sim->Violations.empty(); }
	};

	FPrefixSpec Spec(int32_t Index, int32_t Length) { return FixtureSpec(Index, Length); }
}

HOST_TEST(cache_nested_refusals_and_legacy_mode, "§5.6 / §5.9 (U6): nested specs need budget mode; the parent must be a Ready lease of this cache; no schema of its own; within the cap")
{
	{
		FNestFx F(20, false);
		REQUIRE_TRUE(F.bConfigured);
		const int64_t P = F.Seed(Spec(1, 32));
		REQUIRE_NE(P, 0);
		FPrefixSpec Child = Spec(2, 16);
		Child.ParentHandle = P;
		int64_t Handle = 0;
		std::string Error;
		EXPECT_EQ(F.Cache.Request(Child, Handle, Error), EPrefixRequestResult::NestedRequiresBudgetMode);
	}
	FNestFx F(20);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t P = F.Seed(Spec(1, 32));
	REQUIRE_NE(P, 0);
	int64_t Handle = 0;
	std::string Error;
	FPrefixSpec Child = Spec(2, 16);
	Child.ParentHandle = 987654;                                     // not this cache's
	EXPECT_EQ(F.Cache.Request(Child, Handle, Error), EPrefixRequestResult::Malformed);
	Child.ParentHandle = P;
	Child.SchemaName = "schema_one";                                 // a child inherits its parent's binding
	EXPECT_EQ(F.Cache.Request(Child, Handle, Error), EPrefixRequestResult::Malformed);
	FPrefixSpec Long = Spec(3, 256 - 32 + 1);                        // parent + suffix over the cap
	Long.ParentHandle = P;
	EXPECT_EQ(F.Cache.Request(Long, Handle, Error), EPrefixRequestResult::Malformed);
	const int64_t Pending = F.Request(Spec(4, 32));                  // not Ready yet
	Child.SchemaName.clear();
	Child.ParentHandle = Pending;
	EXPECT_EQ(F.Cache.Request(Child, Handle, Error), EPrefixRequestResult::Malformed);
	F.Sim->RunAll();
	F.Cache.Return(P);
	Child.ParentHandle = P;                                          // ended
	EXPECT_EQ(F.Cache.Request(Child, Handle, Error), EPrefixRequestResult::Malformed);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_build_shares_parent_and_settles_private, "§5.4 (U6): a nested build is charged R(suffix), runs begin_from, settles at ceil(L/B) - floor(origin/B); a hit touches the parent")
{
	FNestFx F(20);
	const int64_t P = F.Seed(Spec(1, 40));             // 3 pages, 2 full
	REQUIRE_NE(P, 0);
	const FPrefixSpec Suffix = Spec(2, 20);
	const int64_t C = F.Seed(Suffix, P, false);        // built: R(20) = 3, then 4 - 2 = 2 private pages
	REQUIRE_NE(C, 0);
	EXPECT_EQ(F.Cache.GetSource(C), EPrefixSource::Recomputed);
	const FEngineHandle* H = F.Sim->HandleOf(C);
	REQUIRE_TRUE(H != nullptr);
	EXPECT_TRUE(H->Tokens == F.Seeded[C].Tokens);      // the adopter sees the parent's tokens, then the suffix
	EXPECT_EQ(H->SharedPages, 2u);
	EXPECT_EQ(F.Charged(), 3u + 2u);
	EXPECT_EQ(F.Sim->CacheDrawnPages, 5u);
	EXPECT_EQ(F.Sim->PeakCacheDrawnPages, 6u);         // 3 + R(20) = 3 at the child's peak
	EXPECT_EQ(F.Cache.Residency().GetPinCount(1, SuperSLMCore::EResidencyPin::ChildOp), 0u);

	// The same child spec under the same parent lease is a memory hit; as a root it is another key.
	F.Cache.Return(C);
	const int64_t Hit = F.Request(Suffix, P);
	const int64_t Root = F.Request(Suffix);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetSource(Hit), EPrefixSource::Memory);
	EXPECT_EQ(F.Cache.GetSource(Root), EPrefixSource::Recomputed);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_waiter_pins_parent_fill_probe, "8.14 [U6] (F1): C = 12; V 3, P 4, Q 3, L 2 leased; W (child of P, 3) waits on V; X (6) evicts Q and is refused; W runs against a live P")
{
	FNestFx F(12);
	const int64_t V = F.Seed(Spec(1, 48));
	const int64_t P = F.Seed(Spec(2, 64));
	const int64_t Q = F.Seed(Spec(3, 48));
	const int64_t L = F.Seed(Spec(4, 32));
	REQUIRE_TRUE(V != 0 && P != 0 && Q != 0 && L != 0);
	REQUIRE_EQ(F.Charged(), 12u);
	F.Cache.Return(V);
	F.Cache.Return(Q);
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t W = F.Request(Spec(5, 32), P);       // a build: R(32) = 3
	REQUIRE_NE(W, 0);
	F.Cache.Return(P);                                  // P is pinned only by W from here
	const int64_t X = F.Request(Spec(6, 80));          // R(80) = 6
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(2)); // V and Q, not P
	EXPECT_EQ(F.Sim->ReleaseCount[F.Seeded[P].Engine], 0);
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[V].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->ReleaseCount[F.Seeded[P].Engine], 0);  // W's begin_from ran against a live P
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 12u);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_ending_ghost_claimable_fill_probe, "8.14 [U6] (F2): C = 10, P ghost 4, D 2, E 4 leased; (i) X 6 admitted at D's apply; (ii) X 2 and Y 4 wait, physical never above C")
{
	for (int Leg = 0; Leg < 2; ++Leg)
	{
		FNestFx F(10);
		const int64_t P = F.Seed(Spec(1, 64));
		const int64_t D = F.Seed(Spec(2, 32), P);       // restore charged 6, settles at 6 - 4 = 2
		const int64_t E = F.Seed(Spec(3, 64));
		REQUIRE_TRUE(P != 0 && D != 0 && E != 0);
		REQUIRE_EQ(F.Charged(), 10u);
		F.Sim->Held = {EPrefixCommandKind::Release};
		REQUIRE_TRUE(F.Ghost(P, 90));                    // D and E leased while P is evicted
		EXPECT_EQ(F.Pages(F.Cache.GetDiagnostics().GhostBytes), 4u);
		EXPECT_EQ(F.Sim->CacheDrawnPages, 10u);          // P's 4 pages, mapped by D
		F.Cache.Return(D);
		if (Leg == 0)
		{
			const int64_t X = F.Request(Spec(4, 80));     // R = 6: D's 2 and the ghost's 4
			F.Sim->RunAll();
			EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending); // mutant "unclaimable": BudgetExhausted
			REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[D].Engine));
			F.Sim->RunAll();
			EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Ready);
		}
		else
		{
			const int64_t X = F.Request(Spec(4, 16));     // R = 2
			const int64_t Y = F.Request(Spec(5, 48));     // R = 4
			F.Sim->RunAll();
			EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending);  // mutant "refunded at D's decision": drawn now
			EXPECT_EQ(F.Cache.GetLeaseState(Y), EPrefixLeaseState::Pending);
			EXPECT_EQ(F.Cache.GetDiagnostics().Evictions, 2u);                // P, then D
			EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
			REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[D].Engine));
			F.Sim->RunAll();
			EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Ready);
			EXPECT_EQ(F.Cache.GetLeaseState(Y), EPrefixLeaseState::Ready);
		}
		EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
		EXPECT_LE(F.Charged(), 10u);
		EXPECT_TRUE(F.Clean());
	}
}

HOST_TEST(cache_nested_siblings_out_of_order_fill_probe, "8.14 [U6] (re-strike R1 siblings): P ghost 4 shared by D1, D2 (1 each), E 4 leased, C = 10; D2's release first admits nobody; physical never above C")
{
	FNestFx F(10);
	const int64_t P = F.Seed(Spec(1, 64));
	const int64_t D1 = F.Seed(Spec(2, 16), P);   // restore charged 5, settles at 1
	const int64_t D2 = F.Seed(Spec(3, 16), P);
	const int64_t E = F.Seed(Spec(4, 64));
	REQUIRE_TRUE(P != 0 && D1 != 0 && D2 != 0 && E != 0);
	REQUIRE_EQ(F.Charged(), 10u);
	F.Sim->Held = {EPrefixCommandKind::Release};
	REQUIRE_TRUE(F.Ghost(P, 90));
	F.Cache.Return(D1);
	F.Cache.Return(D2);
	const FPrefixSpec Xs = Spec(5, 16);
	F.Cook(Xs);                                   // X: a one-page restore
	const int64_t X = F.Request(Xs);
	const int64_t Y = F.Request(Spec(6, 64));     // R(64) = 5
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Cache.GetLeaseState(Y), EPrefixLeaseState::Pending);
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[D2].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(Y), EPrefixLeaseState::Pending); // mutant (rev 1.16): Y drawn here, physical 14
	EXPECT_EQ(F.Sim->CacheDrawnPages, 9u);                           // P's 4 still mapped by D1, D1 1, E 4
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[D1].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(Y), EPrefixLeaseState::Ready);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_chain_fill_probe, "8.14 [U6] (re-strike R1 chain): e0 <- e1 <- e2 <- e3 (1 page each), e1 and L (4) leased, C = 8; Q 5 refused; e3 applies before e2; Z 2 waits for e2")
{
	FNestFx F(8);
	const int64_t E0 = F.Seed(Spec(1, 16));
	const int64_t E1 = F.Seed(Spec(2, 16), E0);   // restore 2, s = 1, settles at 1
	const int64_t E2 = F.Seed(Spec(3, 16), E1);   // restore 3, s = 2
	const int64_t E3 = F.Seed(Spec(4, 16), E2);   // restore 4, s = 3
	const int64_t L = F.Seed(Spec(5, 64));
	REQUIRE_TRUE(E0 != 0 && E1 != 0 && E2 != 0 && E3 != 0 && L != 0);
	REQUIRE_EQ(F.Charged(), 8u);
	F.Cache.Return(E0);
	F.Cache.Return(E2);
	F.Cache.Return(E3);
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t Q = F.Request(Spec(6, 64));     // R = 5
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(Q), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(3)); // e0, e2, e3
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[E3].Engine));
	const int64_t Z = F.Request(Spec(7, 16));     // R = 2
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(Z), EPrefixLeaseState::Pending); // mutant: drawn now, physical 9 > 8
	EXPECT_EQ(F.Sim->CacheDrawnPages, 7u);
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[E2].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(Z), EPrefixLeaseState::Ready);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 8u);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_parent_after_child_fill_probe, "8.14 [U6] (rev 1.17 parent after child): C = 7, e1 (3, child of e0) LRU, e0 (3), L 1 leased; e0's release first admits nobody")
{
	FNestFx F(7);
	const int64_t E0 = F.Seed(Spec(1, 48));
	const int64_t E1 = F.Seed(Spec(2, 48), E0, false); // built: R(48) = 4, settles at 6 - 3 = 3
	const int64_t L = F.Seed(Spec(3, 16));
	REQUIRE_TRUE(E0 != 0 && E1 != 0 && L != 0);
	REQUIRE_EQ(F.Charged(), 7u);
	F.Cache.Return(E0);
	F.Cache.Return(E1);
	const int64_t Hit = F.Request(Spec(1, 48));         // a memory hit: e0 is now more recent than e1
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetSource(Hit), EPrefixSource::Memory);
	F.Cache.Return(Hit);
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t W = F.Request(Spec(4, 48));           // R = 4: e1's 3, then 1 of e0's
	const int64_t X = F.Request(Spec(5, 16));           // R = 2: e0's other 2
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending);
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[E0].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending); // mutant (families on ghosts only): drawn, physical 9
	EXPECT_EQ(F.Sim->CacheDrawnPages, 7u);
	REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[E1].Engine));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Ready);
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 7u);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_own_parent_restore_and_build, "8.14 [U6] (re-strike R2): C = 7, P 4, L 3 leased; a nested restore (4) evicts P and restores private; a nested build is refused and P is not released")
{
	{
		FNestFx F(7);
		const int64_t P = F.Seed(Spec(1, 56));            // 4 pages
		const int64_t L = F.Seed(Spec(2, 48));            // 3
		REQUIRE_TRUE(P != 0 && L != 0);
		const FPrefixSpec Child = Spec(3, 8);             // 64 positions: a restore charged 4
		F.Cook(Child, P);
		F.Sim->Held = {EPrefixCommandKind::Release};
		const int64_t W = F.Request(Child, P);
		REQUIRE_NE(W, 0);
		F.Cache.Return(P);
		F.Sim->RunAll();
		EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Pending);   // mutant (loop skips its parent): BudgetExhausted
		REQUIRE_TRUE(F.Sim->RunRelease(F.Seeded[P].Engine));
		F.Sim->RunAll();
		EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::Ready);
		const FEngineHandle* H = F.Sim->HandleOf(W);
		REQUIRE_TRUE(H != nullptr);
		EXPECT_EQ(H->SharedPages, 0u);                      // null hint: private
		EXPECT_LE(F.Sim->PeakCacheDrawnPages, 7u);
		EXPECT_TRUE(F.Clean());                             // no hint to a released handle
	}
	{
		FNestFx F(7);
		const int64_t P = F.Seed(Spec(1, 56));
		const int64_t L = F.Seed(Spec(2, 48));
		REQUIRE_TRUE(P != 0 && L != 0);
		F.Sim->Held = {EPrefixCommandKind::Release};
		const int64_t W = F.Request(Spec(3, 8), P);        // a build: R(8) = 2
		REQUIRE_NE(W, 0);
		F.Cache.Return(P);
		F.Sim->RunAll();
		EXPECT_EQ(F.Cache.GetLeaseState(W), EPrefixLeaseState::BudgetExhausted); // mutant (evicts its parent): waits, then begin_from on a released P
		EXPECT_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(0));
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.Sim->ReleaseCount[F.Seeded[P].Engine], 0);
		EXPECT_TRUE(F.Clean());
	}
}

HOST_TEST(cache_nested_restore_null_hint_charged_whole, "8.12 [U6]: a nested restore under a hint that fails (seam: null hint) is charged ceil(L/B) before the draw; the fill probe stays within the budget; the next one is BudgetExhausted")
{
	FNestFx F(10);
	const int64_t P = F.Seed(Spec(1, 64));               // 4, kept leased
	REQUIRE_NE(P, 0);
	F.Sim->bNullHints = true;
	const int64_t C1 = F.Seed(Spec(2, 16), P);            // 80 positions: charged 5, restored private (5)
	REQUIRE_NE(C1, 0);
	EXPECT_EQ(F.Sim->HandleOf(C1)->SharedPages, 0u);
	EXPECT_EQ(F.Charged(), 9u);
	const FPrefixSpec Second = Spec(3, 16);
	F.Cook(Second, P);
	const int64_t C2 = F.Request(Second, P);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(C2), EPrefixLeaseState::BudgetExhausted); // mutant (charge 5 - 4 = 1): admitted, physical 14
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
	EXPECT_LE(F.Charged(), 10u);
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_ghost_charge_fill_probe, "1.9 [U6]: a parent evicted while its child stays keeps a ghost charge; new entries are admitted only up to the budget; three levels P ⊂ Q ⊂ G")
{
	{
		FNestFx F(10);
		const int64_t P = F.Seed(Spec(1, 64));
		const int64_t D = F.Seed(Spec(2, 32), P);          // 2 private
		const int64_t Fill = F.Seed(Spec(3, 64));          // 4, leased while P is evicted
		REQUIRE_TRUE(P != 0 && D != 0 && Fill != 0);
		F.Sim->Held = {EPrefixCommandKind::Release};
		REQUIRE_TRUE(F.Ghost(P, 90));
		F.Sim->Held.clear();
		F.Cache.Return(Fill);                               // evictable: the new entries take its 4
		std::vector<int64_t> Admitted;
		for (int32_t I = 0; I < 4; ++I)
		{
			const int64_t A = F.Request(Spec(10 + I, 32));  // R = 3, settles at 2; leases kept (pinned)
			F.Sim->RunAll();
			if (F.Cache.GetLeaseState(A) == EPrefixLeaseState::Ready)
			{
				Admitted.push_back(A);
			}
			EXPECT_LE(F.Sim->CacheDrawnPages, 10u);
		}
		// ghost 4 + D 2, then A1 (peak 3) takes Fill's 4 and settles at 2: 8, and A2's peak 3 no longer fits
		// (mutant "ghost dropped": three admitted, physical above 10).
		EXPECT_EQ(Admitted.size(), static_cast<size_t>(1));
		EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
		EXPECT_TRUE(F.Clean());
	}
	{
		FNestFx F(10);
		const int64_t P = F.Seed(Spec(1, 32));              // 2
		const int64_t Q = F.Seed(Spec(2, 32), P);           // restore 4, settles at 2
		const int64_t G = F.Seed(Spec(3, 32), Q);           // restore 6, settles at 2
		const int64_t Fill = F.Seed(Spec(4, 64));           // 4
		REQUIRE_TRUE(P != 0 && Q != 0 && G != 0 && Fill != 0);
		F.Sim->Held = {EPrefixCommandKind::Release};
		REQUIRE_TRUE(F.Ghost(P, 90));
		REQUIRE_TRUE(F.Ghost(Q, 91));                       // P's ghost stands: G, a grandchild, holds its pages
		EXPECT_EQ(F.Cache.GetDiagnostics().Ghosts, 2u);
		F.Sim->Held.clear();
		F.Cache.Return(Fill);
		size_t Admitted = 0;
		for (int32_t I = 0; I < 4; ++I)
		{
			const int64_t A = F.Request(Spec(10 + I, 32));
			F.Sim->RunAll();
			Admitted += F.Cache.GetLeaseState(A) == EPrefixLeaseState::Ready;
		}
		EXPECT_EQ(Admitted, static_cast<size_t>(1));         // ghosts 2 + 2, G 2, A1 2 (mutant "cleared at the last child": red above)
		EXPECT_LE(F.Sim->PeakCacheDrawnPages, 10u);
		EXPECT_TRUE(F.Clean());
	}
}

HOST_TEST(cache_nested_parent_key_is_in_the_key, "2.12 [U6] (parent key): a child of P1 then the same suffix under P2: not a Memory hit, the twin's tokens, no spec mismatch")
{
	FNestFx F(30);
	const int64_t P1 = F.Seed(Spec(1, 32));
	const int64_t P2 = F.Seed(Spec(2, 32));
	REQUIRE_TRUE(P1 != 0 && P2 != 0);
	const FPrefixSpec Suffix = Spec(3, 16);
	const int64_t A = F.Seed(Suffix, P1, false);
	const int64_t B = F.Seed(Suffix, P2, false);
	REQUIRE_TRUE(A != 0 && B != 0);
	EXPECT_EQ(F.Cache.GetSource(B), EPrefixSource::Recomputed);   // mutant (parent key dropped): a spec mismatch
	EXPECT_EQ(F.Cache.GetDiagnostics().SpecMismatches, 0u);
	const FEngineHandle* H = F.Sim->HandleOf(B);
	REQUIRE_TRUE(H != nullptr);
	EXPECT_TRUE(H->Tokens == F.Seeded[B].Tokens);                 // P2's tokens, then the suffix
	// The key a Ready lease holds (the host's disk-file seam reads a nested spec's parent key so).
	FPrefixKey KA, KB, KP;
	REQUIRE_TRUE(F.Cache.GetLeaseKey(A, KA) && F.Cache.GetLeaseKey(B, KB) && F.Cache.GetLeaseKey(P1, KP));
	EXPECT_TRUE(KA.Bytes == F.Seeded[A].Key.Bytes);
	EXPECT_TRUE(KB.Bytes == F.Seeded[B].Key.Bytes);
	EXPECT_TRUE(KA.Bytes != KB.Bytes);
	EXPECT_TRUE(!F.Cache.GetLeaseKey(999999, KA));                // not a lease
	F.Cache.Return(B);
	EXPECT_TRUE(!F.Cache.GetLeaseKey(B, KB));                     // ended
	EXPECT_TRUE(F.Clean());
}

HOST_TEST(cache_nested_teardown_releases_every_handle_once, "1.7 / 2.10 [U6]: teardown with nested entries, a ghost and a waiter: every handle released once, the pool destroy sees no live handle")
{
	FNestFx F(10);
	const int64_t P = F.Seed(Spec(1, 64));
	const int64_t D = F.Seed(Spec(2, 32), P);
	const int64_t E = F.Seed(Spec(3, 64));
	REQUIRE_TRUE(P != 0 && D != 0 && E != 0);
	F.Sim->Held = {EPrefixCommandKind::Release};
	REQUIRE_TRUE(F.Ghost(P, 90));
	F.Cache.Return(D);
	const int64_t X = F.Request(Spec(4, 16));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Pending);
	F.Sim->Held.clear();
	F.Cache.BeginTeardown();
	F.Sim->RunAll();
	EXPECT_TRUE(F.Cache.IsTornDown());
	EXPECT_EQ(F.Cache.GetLeaseState(X), EPrefixLeaseState::Cancelled);
	for (const auto& Pair : F.Sim->ReleaseCount)
	{
		EXPECT_EQ(Pair.second, 1);
	}
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	EXPECT_EQ(F.Cache.Residency().GetLedger(0, 0).ChargedBytes, 0u);
	EXPECT_TRUE(F.Clean());
}
