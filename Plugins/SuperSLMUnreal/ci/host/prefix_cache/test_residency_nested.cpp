// Nested entries in the residency core (precache plan rev 1.17 §5.4 Ghost charges and Pins, §5.11 (a)
// Pending refunds wait on their family; step U6). Ledger-level halves of cells 1.9 and 8.14's nested
// legs: decisions, eviction lists, the apply at which each waiter is admitted, ghost and pin state.
// The physical halves (the fill probe) are in test_cache_nested.cpp. Units are pages (1 byte each).

#include "harness.h"
#include "SuperSLMResidencyCore.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace SuperSLMCore;

namespace
{
	constexpr FResidencyLedgerId kHost = kResidencyLedgerHostRam;

	struct FNested
	{
		int64_t Counter = 1;
		FSuperSLMResidencyCore Core{1, &Counter};
		uint8_t NextKey = 1;

		explicit FNested(uint64_t Budget) { Core.SetBudget(kResidencyKindPrefix, kHost, Budget); }

		FResidencyKey Key()
		{
			FResidencyKey K;
			K.Bytes[0] = NextKey++;
			return K;
		}

		// A resident entry of Charge pages, a child of Parent when given, Lease-pinned when bLeased.
		FResidencyEntryId Resident(uint64_t Charge, FResidencyEntryId Parent = kInvalidResidencyEntry, bool bLeased = false)
		{
			const FResidencyEntryId Id = Core.CreateEntry(kResidencyKindPrefix, kHost, Key(), true);
			Core.Charge(Id, Charge);
			Core.Settle(Id, Charge);
			if (Parent != kInvalidResidencyEntry)
			{
				Core.SetParent(Id, Parent);
			}
			if (bLeased)
			{
				Core.AddPin(Id, EResidencyPin::Lease);
			}
			return Id;
		}

		// A request of Peak pages: a child of Parent when given (bRequired: a build, else a restore).
		FResidencyEntryId Request(uint64_t Peak, EChargeOutcome& Out, FResidencyEntryId Parent = kInvalidResidencyEntry, bool bRequired = false)
		{
			const FResidencyEntryId Id = Core.CreateEntry(kResidencyKindPrefix, kHost, Key(), true);
			if (Parent != kInvalidResidencyEntry)
			{
				Core.SetParent(Id, Parent);
			}
			Out = Core.Charge(Id, Peak, bRequired);
			return Id;
		}

		FResidencyLedger L() const { return Core.GetLedger(kResidencyKindPrefix, kHost); }

		bool Identity() const
		{
			const FResidencyLedger Led = L();
			return Led.ChargedBytes == Led.ResidentBytes + Led.InFlightBytes + Led.ReleasingBytes + Led.WaitingBytes + Led.GhostBytes &&
				Led.ChargedBytes <= Led.BudgetBytes && Led.ClaimedRefundBytes <= Led.ReleasingBytes;
		}

		// Makes Victim a ghost: every other evictable entry is pinned while a one-page request evicts it
		// and is refused (its eviction stands), then Victim's own release applies.
		bool Ghost(FResidencyEntryId Victim, const std::vector<FResidencyEntryId>& Others)
		{
			for (FResidencyEntryId Id : Others)
			{
				Core.AddPin(Id, EResidencyPin::WorkerJob);
			}
			EChargeOutcome Out = EChargeOutcome::Charged;
			Request(1, Out);
			const std::vector<FResidencyEntryId> Evicted = Core.TakeEvicted();
			for (FResidencyEntryId Id : Others)
			{
				Core.RemovePin(Id, EResidencyPin::WorkerJob);
			}
			return Out == EChargeOutcome::BudgetExhausted && Evicted == std::vector<FResidencyEntryId>{Victim} && Core.IsGhost(Victim) &&
				Core.ReleaseCompleted(Victim) && Core.GetState(Victim) == EResidencyState::Ghost;
		}
	};
}

HOST_TEST(residency_nested_parent_link_and_refusals, "§5.4 Pins (U6): SetParent's preconditions; a charged child pins its parent until applied or returned")
{
	FNested N(20);
	const FResidencyEntryId P = N.Resident(4);
	const FResidencyEntryId Other = N.Core.CreateEntry(kResidencyKindPrefix, kHost, N.Key(), true);
	EXPECT_TRUE(!N.Core.SetParent(Other, Other));                  // itself
	EXPECT_TRUE(!N.Core.SetParent(Other, 9999));                   // unknown
	const FResidencyEntryId Unadmitted = N.Core.CreateEntry(kResidencyKindPrefix, kHost, N.Key(), true);
	EXPECT_TRUE(!N.Core.SetParent(Other, Unadmitted));             // a parent that is not Resident
	REQUIRE_TRUE(N.Core.SetParent(Other, P));
	EXPECT_EQ(N.Core.GetParent(Other), P);

	// Charged now: P is ChildOp-pinned until the child settles.
	EXPECT_EQ(N.Core.Charge(Other, 3, true), EChargeOutcome::Charged);
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 1u);
	EXPECT_TRUE(!N.Core.SetParent(Other, P));                      // a child in flight takes no new parent
	N.Core.Settle(Other, 2);
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 0u);
	EXPECT_EQ(N.Core.GetParent(Other), P);                         // the link outlives the job

	// Returned: the pin goes with the charge.
	EChargeOutcome Out = EChargeOutcome::BudgetExhausted;
	const FResidencyEntryId C2 = N.Request(3, Out, P, true);
	EXPECT_EQ(Out, EChargeOutcome::Charged);
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 1u);
	N.Core.ReturnCharge(C2);
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 0u);
	N.Core.DetachParent(Other);
	EXPECT_EQ(N.Core.GetParent(Other), kInvalidResidencyEntry);
	EXPECT_TRUE(N.Identity());
}

HOST_TEST(residency_nested_ghost_until_last_descendant, "1.9 [U6] (ledger): a parent evicted while a child stays is a ghost of its whole charge until its last cached descendant leaves; three levels")
{
	// Two levels: C = 10, P (4) with child D (2), E (4) leased.
	{
		FNested N(10);
		const FResidencyEntryId P = N.Resident(4);
		const FResidencyEntryId D = N.Resident(2, P);
		N.Resident(4, kInvalidResidencyEntry, true);
		REQUIRE_TRUE(N.Ghost(P, {D}));
		EXPECT_TRUE(N.Core.IsGhost(P));
		EXPECT_EQ(N.Core.GetGhostBytes(P), 4u);
		EXPECT_EQ(N.L().GhostBytes, 4u);
		EXPECT_EQ(N.L().GhostEntries, 1u);
		EXPECT_EQ(N.L().ReleasingBytes, 0u);   // a ghost is not a pending refund: nothing to claim yet
		EXPECT_EQ(N.L().ChargedBytes, 10u);    // ghost 4 + D 2 + E 4
		EXPECT_TRUE(N.Identity());

		// Nothing is free while the ghost stands: X (6) evicts D, which ends the ghost.
		EChargeOutcome Out = EChargeOutcome::Charged;
		const FResidencyEntryId X = N.Request(6, Out);
		EXPECT_EQ(Out, EChargeOutcome::Deferred);
		EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{D});
		EXPECT_TRUE(!N.Core.IsGhost(P));                    // D was P's last cached descendant
		EXPECT_EQ(N.Core.Snapshot().GhostEndings, 1u);
		EXPECT_EQ(N.L().ReleasingBytes, 6u);                // D's 2 and the ended ghost's 4, both pending
		EXPECT_EQ(N.Core.GetClaimedBytes(X), 6u);
		EXPECT_TRUE(N.Core.ReleaseCompleted(D));
		EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{X});
		EXPECT_EQ(N.Core.GetState(P), EResidencyState::Unknown); // the ghost is gone with its last refund
		EXPECT_EQ(N.L().GhostBytes, 0u);
		EXPECT_TRUE(N.Identity());
	}
	// Three levels: P (2) ⊂ Q (2) ⊂ G (2), L (2) leased; C = 8. Evict P, then Q: P's ghost stays (G,
	// a grandchild, still holds its pages); evicting G ends both.
	{
		FNested N(8);
		const FResidencyEntryId P = N.Resident(2);
		const FResidencyEntryId Q = N.Resident(2, P);
		const FResidencyEntryId G = N.Resident(2, Q);
		N.Resident(2, kInvalidResidencyEntry, true);
		REQUIRE_TRUE(N.Ghost(P, {Q, G}));
		REQUIRE_TRUE(N.Ghost(Q, {G}));
		EXPECT_TRUE(N.Core.IsGhost(P));          // mutant "cleared at the last child": P's ghost ends here
		EXPECT_TRUE(N.Core.IsGhost(Q));
		EXPECT_EQ(N.L().GhostBytes, 4u);
		EXPECT_EQ(N.L().ChargedBytes, 8u);
		EChargeOutcome Out = EChargeOutcome::Charged;
		const FResidencyEntryId X = N.Request(6, Out);
		EXPECT_EQ(Out, EChargeOutcome::Deferred);
		EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{G});
		EXPECT_EQ(N.Core.Snapshot().GhostEndings, 2u);
		EXPECT_EQ(N.L().GhostBytes, 0u);
		EXPECT_TRUE(N.Core.ReleaseCompleted(G));
		EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{X});
		EXPECT_TRUE(N.Identity());
	}
}

HOST_TEST(residency_nested_waiter_pins_its_parent, "8.14 [U6] (F1, ledger): C = 12, V 3, P 4, Q 3, L 2 leased; W (child of P, 3) waits on V and pins P; X (6) evicts Q and is refused")
{
	FNested N(12);
	const FResidencyEntryId V = N.Resident(3);
	const FResidencyEntryId P = N.Resident(4);
	const FResidencyEntryId Q = N.Resident(3);
	N.Resident(2, kInvalidResidencyEntry, true);
	EChargeOutcome Out = EChargeOutcome::Charged;
	const FResidencyEntryId W = N.Request(3, Out, P, true);
	EXPECT_EQ(Out, EChargeOutcome::Deferred);
	EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{V});
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 1u); // from the decision
	N.Request(6, Out);
	EXPECT_EQ(Out, EChargeOutcome::BudgetExhausted);                // as the approved rule refuses it
	EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{Q}); // mutant "no pin while waiting": Q, P
	EXPECT_EQ(N.Core.GetState(P), EResidencyState::Resident);
	EXPECT_TRUE(N.Core.ReleaseCompleted(V));
	EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{W});
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 1u); // the pin passed to the job
	N.Core.Settle(W, 3);
	EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 0u);
	EXPECT_TRUE(N.Identity());
}

HOST_TEST(residency_nested_ending_ghost_is_claimable, "8.14 [U6] (F2, ledger): C = 10, P ghost 4, D 2, E 4 leased; (i) X 6 claims D's 2 and the ghost's 4; (ii) X 2 and Y 4 both wait")
{
	// (i)
	{
		FNested N(10);
		const FResidencyEntryId P = N.Resident(4);
		const FResidencyEntryId D = N.Resident(2, P);
		N.Resident(4, kInvalidResidencyEntry, true);
		REQUIRE_TRUE(N.Ghost(P, {D}));
		EChargeOutcome Out = EChargeOutcome::Charged;
		const FResidencyEntryId X = N.Request(6, Out);
		EXPECT_EQ(Out, EChargeOutcome::Deferred);              // mutant "ghost held, unclaimable": refused
		EXPECT_EQ(N.Core.GetClaimedBytes(X), 6u);
		EXPECT_TRUE(N.Core.ReleaseCompleted(D));
		EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{X});
		EXPECT_EQ(N.Core.GetChargedBytes(X), 6u);
	}
	// (ii)
	{
		FNested N(10);
		const FResidencyEntryId P = N.Resident(4);
		const FResidencyEntryId D = N.Resident(2, P);
		N.Resident(4, kInvalidResidencyEntry, true);
		REQUIRE_TRUE(N.Ghost(P, {D}));
		EChargeOutcome Ox = EChargeOutcome::Charged;
		EChargeOutcome Oy = EChargeOutcome::Charged;
		const FResidencyEntryId X = N.Request(2, Ox);   // mutant "ghost refunded at D's decision": Charged at once
		const FResidencyEntryId Y = N.Request(4, Oy);
		EXPECT_EQ(Ox, EChargeOutcome::Deferred);
		EXPECT_EQ(Oy, EChargeOutcome::Deferred);
		EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{D}); // one eviction
		EXPECT_EQ(N.Core.GetClaimedBytes(X), 2u);
		EXPECT_EQ(N.Core.GetClaimedBytes(Y), 4u);
		EXPECT_EQ(N.L().ChargedBytes, 10u);
		EXPECT_TRUE(N.Core.ReleaseCompleted(D));
		EXPECT_TRUE(N.Core.TakeAdmitted() == (std::vector<FResidencyEntryId>{X, Y})); // arrival order
		EXPECT_TRUE(N.Identity());
	}
}

HOST_TEST(residency_nested_siblings_released_out_of_order, "8.14 [U6] (re-strike R1, ledger): P ghost 4 shared by D1, D2 (1 each), E 4 leased; X 1 waits on D1, Y 5 on D2 and the ghost; D2's release first admits nobody")
{
	FNested N(10);
	const FResidencyEntryId P = N.Resident(4);
	const FResidencyEntryId D1 = N.Resident(1, P);
	const FResidencyEntryId D2 = N.Resident(1, P);
	N.Resident(4, kInvalidResidencyEntry, true);
	REQUIRE_TRUE(N.Ghost(P, {D1, D2}));
	EChargeOutcome Ox = EChargeOutcome::Charged;
	EChargeOutcome Oy = EChargeOutcome::Charged;
	const FResidencyEntryId X = N.Request(1, Ox);
	const FResidencyEntryId Y = N.Request(5, Oy);
	EXPECT_EQ(Ox, EChargeOutcome::Deferred);
	EXPECT_EQ(Oy, EChargeOutcome::Deferred);
	EXPECT_TRUE(N.Core.TakeEvicted() == (std::vector<FResidencyEntryId>{D1, D2}));
	EXPECT_EQ(N.Core.GetClaimedBytes(Y), 5u);
	EXPECT_TRUE(N.Core.ReleaseCompleted(D2));
	EXPECT_TRUE(N.Core.TakeAdmitted().empty());   // the ghost's family still holds D1 (mutant, rev 1.16: Y here)
	EXPECT_TRUE(N.Core.ReleaseCompleted(D1));
	EXPECT_TRUE(N.Core.TakeAdmitted() == (std::vector<FResidencyEntryId>{X, Y}));
	EXPECT_TRUE(N.Identity());
}

HOST_TEST(residency_nested_chain_ghost_waits_on_its_own_release, "8.14 [U6] (re-strike R1 chain, ledger): e0 <- e1 <- e2 <- e3 (1 each), e1 and L (4) leased, C = 8; Q 5 refused; Z 2 waits on e2's own release")
{
	FNested N(8);
	N.Resident(4, kInvalidResidencyEntry, true);
	const FResidencyEntryId E0 = N.Resident(1);
	const FResidencyEntryId E1 = N.Resident(1, E0, true);
	const FResidencyEntryId E2 = N.Resident(1, E1);
	const FResidencyEntryId E3 = N.Resident(1, E2);
	EChargeOutcome Out = EChargeOutcome::Charged;
	N.Request(5, Out, kInvalidResidencyEntry, true);
	EXPECT_EQ(Out, EChargeOutcome::BudgetExhausted);
	EXPECT_TRUE(N.Core.TakeEvicted() == (std::vector<FResidencyEntryId>{E0, E2, E3}));
	EXPECT_TRUE(N.Core.IsGhost(E0));      // e1 is still cached
	EXPECT_TRUE(!N.Core.IsGhost(E2));     // e3's eviction ended it
	EXPECT_EQ(N.L().GhostBytes, 1u);
	EXPECT_TRUE(N.Core.ReleaseCompleted(E3));
	EXPECT_EQ(N.L().ChargedBytes, 7u);    // e3's own page returned; e2's ghost still waits on e2's release
	const FResidencyEntryId Z = N.Request(2, Out);
	EXPECT_EQ(Out, EChargeOutcome::Deferred);   // mutant (ghost refunded at e3's apply): Charged at once
	EXPECT_TRUE(N.Core.ReleaseCompleted(E2));
	EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{Z});
	EXPECT_TRUE(N.Identity());
}

HOST_TEST(residency_nested_parent_after_child_waits_on_the_child, "8.14 [U6] (rev 1.17 parent after child, ledger): C = 7, e1 (3, child of e0) LRU, e0 (3), L 1 leased; e0's release first admits nobody")
{
	FNested N(7);
	const FResidencyEntryId E0 = N.Resident(3);
	const FResidencyEntryId E1 = N.Resident(3, E0);
	N.Resident(1, kInvalidResidencyEntry, true);
	N.Core.Touch(E0);
	EChargeOutcome Ow = EChargeOutcome::Charged;
	EChargeOutcome Ox = EChargeOutcome::Charged;
	const FResidencyEntryId W = N.Request(4, Ow);
	const FResidencyEntryId X = N.Request(2, Ox);
	EXPECT_EQ(Ow, EChargeOutcome::Deferred);
	EXPECT_EQ(Ox, EChargeOutcome::Deferred);
	EXPECT_TRUE(N.Core.TakeEvicted() == (std::vector<FResidencyEntryId>{E1, E0}));
	EXPECT_TRUE(!N.Core.IsGhost(E0));     // e1 is no longer cached: no ghost
	EXPECT_TRUE(N.Core.ReleaseCompleted(E0));
	EXPECT_TRUE(N.Core.TakeAdmitted().empty()); // e0's pages are still mapped by e1's handle (mutant: X here)
	EXPECT_TRUE(N.Core.ReleaseCompleted(E1));
	EXPECT_TRUE(N.Core.TakeAdmitted() == (std::vector<FResidencyEntryId>{W, X}));
	EXPECT_TRUE(N.Identity());
}

HOST_TEST(residency_nested_own_parent_restore_evicts_build_refused, "8.14 [U6] (re-strike R2, ledger): C = 7, P 4, L 3 leased; a nested restore (4) evicts P and runs private; a nested build is refused and P stays")
{
	{
		FNested N(7);
		const FResidencyEntryId P = N.Resident(4);
		N.Resident(3, kInvalidResidencyEntry, true);
		EChargeOutcome Out = EChargeOutcome::Charged;
		const FResidencyEntryId W = N.Request(4, Out, P, false);
		EXPECT_EQ(Out, EChargeOutcome::Deferred);   // mutant (restore's loop skips its parent): refused
		EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{P});
		EXPECT_EQ(N.Core.GetParent(W), kInvalidResidencyEntry); // null hint: private
		EXPECT_TRUE(N.Core.ReleaseCompleted(P));
		EXPECT_TRUE(N.Core.TakeAdmitted() == std::vector<FResidencyEntryId>{W});
	}
	{
		FNested N(7);
		const FResidencyEntryId P = N.Resident(4);
		N.Resident(3, kInvalidResidencyEntry, true);
		EChargeOutcome Out = EChargeOutcome::Charged;
		const FResidencyEntryId W = N.Request(2, Out, P, true);
		EXPECT_EQ(Out, EChargeOutcome::BudgetExhausted); // mutant (build's loop evicts its parent): Deferred
		EXPECT_TRUE(N.Core.TakeEvicted().empty());
		EXPECT_EQ(N.Core.GetState(P), EResidencyState::Resident);
		EXPECT_EQ(N.Core.GetPinCount(P, EResidencyPin::ChildOp), 0u);
		EXPECT_TRUE(N.Core.Destroy(W));
		EXPECT_TRUE(N.Identity());
	}
}

HOST_TEST(residency_nested_ghost_held_by_a_grandchild, "1.9 [U6] three levels: a ghost stays while any cached descendant (not only a child) shares its pages")
{
	// G(4) <- C(2) <- GC(2), and G <- C2(2): the budget is full.
	FNested N(10);
	const FResidencyEntryId G = N.Resident(4);
	const FResidencyEntryId C = N.Resident(2, G);
	const FResidencyEntryId GC = N.Resident(2, C);
	const FResidencyEntryId C2 = N.Resident(2, G);
	REQUIRE_TRUE(N.Ghost(G, {C, GC, C2}));
	REQUIRE_TRUE(N.Ghost(C, {GC, C2}));
	EXPECT_EQ(N.L().GhostEntries, 2u);

	// C2 leaves: G has no cached child now, but its grandchild GC still maps its pages.
	N.Core.AddPin(GC, EResidencyPin::WorkerJob);
	EChargeOutcome Out = EChargeOutcome::Charged;
	N.Request(1, Out);
	EXPECT_TRUE(N.Core.TakeEvicted() == std::vector<FResidencyEntryId>{C2});
	EXPECT_TRUE(N.Core.IsGhost(G));
	EXPECT_TRUE(N.Core.IsGhost(C));
	EXPECT_EQ(N.L().GhostBytes, 6u);
	EXPECT_TRUE(N.Identity());
	N.Core.RemovePin(GC, EResidencyPin::WorkerJob);
}

HOST_TEST(residency_nested_random_runs_quiesce, "1.9, 8.14 [U6] (ledger, randomized): nested restores and builds, waiters, leases, private settles and releases in random order; the identity and the budget hold, no InFlight or Waiting child has a parent that is not Resident, and once every job applies and every entry is torn down nothing is charged, ghosted, pinned ChildOp or left")
{
	for (uint32_t Seed = 1; Seed <= 400; ++Seed)
	{
		std::mt19937 Rng(Seed);
		auto Pick = [&Rng](size_t N) { return static_cast<size_t>(Rng() % static_cast<uint32_t>(N)); };
		FNested N(6 + Rng() % 10);
		std::vector<FResidencyEntryId> All, InFlight, Waiters, Leased;
		auto InState = [&](EResidencyState S)
		{
			std::vector<FResidencyEntryId> Out;
			for (FResidencyEntryId Id : All)
			{
				if (N.Core.GetState(Id) == S)
				{
					Out.push_back(Id);
				}
			}
			return Out;
		};
		auto Take = [&]()
		{
			for (FResidencyEntryId Id : N.Core.TakeAdmitted())
			{
				Waiters.erase(std::remove(Waiters.begin(), Waiters.end(), Id), Waiters.end());
				InFlight.push_back(Id);
			}
			N.Core.TakeEvicted();
		};
		auto Check = [&]()
		{
			REQUIRE_TRUE(N.Identity());
			for (FResidencyEntryId Id : All)
			{
				const EResidencyState S = N.Core.GetState(Id);
				const FResidencyEntryId Up = N.Core.GetParent(Id);
				if ((S == EResidencyState::InFlight || S == EResidencyState::Waiting) && Up != kInvalidResidencyEntry)
				{
					REQUIRE_EQ(N.Core.GetState(Up), EResidencyState::Resident);
				}
			}
		};
		for (int Step = 0; Step < 200; ++Step)
		{
			const uint32_t Op = Rng() % 10;
			if (Op <= 3)
			{
				const std::vector<FResidencyEntryId> Res = InState(EResidencyState::Resident);
				const FResidencyEntryId Parent = !Res.empty() && Rng() % 2 == 0 ? Res[Pick(Res.size())] : kInvalidResidencyEntry;
				EChargeOutcome Out = EChargeOutcome::Charged;
				const FResidencyEntryId Id = N.Request(1 + Rng() % 5, Out, Parent, Rng() % 2 == 0);
				All.push_back(Id);
				if (Out == EChargeOutcome::Charged)
				{
					InFlight.push_back(Id);
				}
				else if (Out == EChargeOutcome::Deferred)
				{
					Waiters.push_back(Id);
				}
				else
				{
					EXPECT_TRUE(N.Core.Destroy(Id));
				}
			}
			else if (Op <= 5 && !InFlight.empty())
			{
				const size_t I = Pick(InFlight.size());
				const FResidencyEntryId Id = InFlight[I];
				InFlight.erase(InFlight.begin() + static_cast<std::ptrdiff_t>(I));
				if (Rng() % 5 == 0)
				{
					N.Core.ReturnCharge(Id); // the job failed
					EXPECT_TRUE(N.Core.Destroy(Id));
				}
				else
				{
					const uint64_t Charge = N.Core.GetChargedBytes(Id);
					if (N.Core.GetParent(Id) != kInvalidResidencyEntry && Rng() % 3 == 0)
					{
						N.Core.DetachParent(Id); // restored private (s = 0)
					}
					N.Core.Settle(Id, Charge - (Charge > 1 ? Rng() % 2 : 0));
				}
			}
			else if (Op <= 7)
			{
				const std::vector<FResidencyEntryId> Rel = InState(EResidencyState::Releasing);
				if (!Rel.empty())
				{
					EXPECT_TRUE(N.Core.ReleaseCompleted(Rel[Pick(Rel.size())]));
				}
			}
			else if (Op == 8 && !Waiters.empty())
			{
				const size_t I = Pick(Waiters.size());
				const FResidencyEntryId Id = Waiters[I];
				Waiters.erase(Waiters.begin() + static_cast<std::ptrdiff_t>(I));
				N.Core.ReturnCharge(Id); // cancelled while waiting
				EXPECT_TRUE(N.Core.Destroy(Id));
			}
			else if (Op == 9)
			{
				const std::vector<FResidencyEntryId> Res = InState(EResidencyState::Resident);
				if (!Res.empty() && Rng() % 2 == 0)
				{
					Leased.push_back(Res[Pick(Res.size())]);
					N.Core.AddPin(Leased.back(), EResidencyPin::Lease);
				}
				else if (!Leased.empty())
				{
					const size_t I = Pick(Leased.size());
					EXPECT_TRUE(N.Core.RemovePin(Leased[I], EResidencyPin::Lease));
					Leased.erase(Leased.begin() + static_cast<std::ptrdiff_t>(I));
				}
			}
			Take();
			Check();
		}

		// Every job applies and every pending release applies (in random order): no waiter is left.
		for (int Round = 0; Round < 10000 && (!InFlight.empty() || !Waiters.empty() || !InState(EResidencyState::Releasing).empty()); ++Round)
		{
			for (FResidencyEntryId Id : InFlight)
			{
				N.Core.Settle(Id, N.Core.GetChargedBytes(Id));
			}
			InFlight.clear();
			const std::vector<FResidencyEntryId> Rel = InState(EResidencyState::Releasing);
			if (!Rel.empty())
			{
				N.Core.ReleaseCompleted(Rel[Pick(Rel.size())]);
			}
			Take();
			Check();
		}
		REQUIRE_TRUE(Waiters.empty() && InFlight.empty());
		for (FResidencyEntryId Id : InState(EResidencyState::Resident))
		{
			EXPECT_EQ(N.Core.GetPinCount(Id, EResidencyPin::ChildOp), 0u);
		}

		// Teardown: every resident entry goes whatever its pins, then every release applies.
		for (FResidencyEntryId Id : InState(EResidencyState::Resident))
		{
			EXPECT_TRUE(N.Core.EvictForTeardown(Id));
		}
		N.Core.TakeEvicted();
		for (std::vector<FResidencyEntryId> Rel = InState(EResidencyState::Releasing); !Rel.empty(); Rel = InState(EResidencyState::Releasing))
		{
			EXPECT_TRUE(N.Core.ReleaseCompleted(Rel[Pick(Rel.size())]));
		}
		const FResidencyLedger L = N.L();
		EXPECT_EQ(L.ChargedBytes, 0u);
		EXPECT_EQ(L.GhostBytes, 0u);
		EXPECT_EQ(L.GhostEntries, 0u);
		EXPECT_EQ(L.ReleasingBytes, 0u);
		EXPECT_EQ(L.ReleasingEntries, 0u);
		EXPECT_EQ(N.Core.GetPendingRefundCount(kResidencyKindPrefix, kHost), 0u);
		for (FResidencyEntryId Id : All)
		{
			const EResidencyState S = N.Core.GetState(Id);
			EXPECT_TRUE(S == EResidencyState::Unknown);
		}
	}
}
