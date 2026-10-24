// Deferred-refund admission in the kind-neutral core (precache plan rev 1.17 §5.11 (a); cell 8.14's
// ledger half). Units are bytes; one "page" is 100 here.

#include "harness.h"
#include "SuperSLMResidencyCore.h"

using namespace SuperSLMCore;

namespace
{
	constexpr FResidencyLedgerId kHost = kResidencyLedgerHostRam;

	FResidencyKey RKey(uint8_t N)
	{
		FResidencyKey K;
		K.Bytes[0] = N;
		K.Bytes[1] = 0x5A;
		return K;
	}

	FResidencyEntryId Resident(FSuperSLMResidencyCore& Core, uint8_t N, uint64_t Bytes)
	{
		const FResidencyEntryId Id = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(N), true);
		if (Id == kInvalidResidencyEntry || Core.Charge(Id, Bytes) != EChargeOutcome::Charged)
		{
			return kInvalidResidencyEntry;
		}
		Core.Settle(Id, Bytes);
		return Id;
	}

	FResidencyLedger Led(const FSuperSLMResidencyCore& Core) { return Core.GetLedger(kResidencyKindPrefix, kHost); }

	bool LedgerAdds(const FResidencyLedger& L)
	{
		return L.ChargedBytes == L.ResidentBytes + L.InFlightBytes + L.ReleasingBytes + L.WaitingBytes &&
			L.ChargedBytes <= L.BudgetBytes && L.ClaimedRefundBytes <= L.ReleasingBytes;
	}
}

HOST_TEST(refund_waiter_takes_free_charge_and_claims, "8.14 waiters; §5.11 (a) claimed and unclaimed refunds, the waiter's free charge")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 500);
	const FResidencyEntryId A = Resident(Core, 1, 300);
	const FResidencyEntryId B = Resident(Core, 2, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry);
	Core.AddPin(B, EResidencyPin::Lease);

	// W needs 250: 100 free, then evicts A (300) and claims 150 of it; 150 stays unclaimed.
	const FResidencyEntryId W = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(3), true);
	REQUIRE_EQ(Core.Charge(W, 250), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.GetState(W), EResidencyState::Waiting);
	EXPECT_EQ(Core.GetWaitingHeldBytes(W), 100u);
	EXPECT_EQ(Core.GetClaimedBytes(W), 150u);
	FResidencyLedger L = Led(Core);
	EXPECT_EQ(L.ChargedBytes, 500u);
	EXPECT_EQ(L.WaitingBytes, 100u);
	EXPECT_EQ(L.ReleasingBytes, 300u);
	EXPECT_EQ(L.ClaimedRefundBytes, 150u);
	EXPECT_TRUE(LedgerAdds(L));

	// X needs 150: covered by the unclaimed 150, with no eviction.
	const FResidencyEntryId X = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(4), true);
	REQUIRE_EQ(Core.Charge(X, 150), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.GetClaimedBytes(X), 150u);
	EXPECT_EQ(Core.GetWaitingHeldBytes(X), 0u);
	EXPECT_EQ(Core.TakeEvicted().size(), static_cast<size_t>(1));
	EXPECT_EQ(Core.Snapshot().Evictions, 1u);
	EXPECT_EQ(Led(Core).ClaimedRefundBytes, 300u);

	// A's release applies: both are admitted, in arrival order, with their whole peaks charged.
	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	const std::vector<FResidencyEntryId> Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(2));
	EXPECT_EQ(Admitted[0], W);
	EXPECT_EQ(Admitted[1], X);
	EXPECT_EQ(Core.GetChargedBytes(W), 250u);
	EXPECT_EQ(Core.GetChargedBytes(X), 150u);
	L = Led(Core);
	EXPECT_EQ(L.ChargedBytes, 500u);
	EXPECT_EQ(L.InFlightBytes, 400u);
	EXPECT_EQ(L.WaitingBytes, 0u);
	EXPECT_EQ(L.ReleasingBytes, 0u);
	EXPECT_TRUE(LedgerAdds(L));
	EXPECT_EQ(Core.GetArrival(W) < Core.GetArrival(X), true);
}

HOST_TEST(refund_a_request_the_free_charge_covers_is_admitted_beside_a_waiter, "§5.11 (a) admitted now, waiting, or refused (no queue order between requests)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 500);
	const FResidencyEntryId B = Resident(Core, 1, 100);
	REQUIRE_TRUE(B != kInvalidResidencyEntry);
	Core.AddPin(B, EResidencyPin::Lease);
	const FResidencyEntryId C = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(2), true);
	REQUIRE_EQ(Core.Charge(C, 150), EChargeOutcome::Charged); // a build in flight at its peak
	const FResidencyEntryId A = Resident(Core, 3, 200);
	REQUIRE_TRUE(A != kInvalidResidencyEntry);
	// free 50. W needs 250: takes 50, evicts A and claims 200.
	const FResidencyEntryId W = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(4), true);
	REQUIRE_EQ(Core.Charge(W, 250), EChargeOutcome::Deferred);
	// C freezes: its charge falls to 50, so 100 is free. The waiter is not admitted by that; it waits
	// for the refund it claims on.
	Core.Settle(C, 50);
	EXPECT_EQ(Core.TakeAdmitted().empty(), true);
	EXPECT_EQ(Core.GetState(W), EResidencyState::Waiting);
	// X needs 100: the free charge covers it, so it is admitted now, not queued behind W.
	const FResidencyEntryId X = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(5), true);
	EXPECT_EQ(Core.Charge(X, 100), EChargeOutcome::Charged);
	EXPECT_TRUE(LedgerAdds(Led(Core)));
	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	const std::vector<FResidencyEntryId> Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Admitted[0], W);
	EXPECT_EQ(Led(Core).ChargedBytes, 500u);
}

HOST_TEST(refund_a_waiter_is_admitted_at_its_last_claimed_refund, "§5.11 (a) admitted at the apply of the last pending refund it claims on; §5.5")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 400);
	const FResidencyEntryId A = Resident(Core, 1, 200);
	const FResidencyEntryId B = Resident(Core, 2, 200);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry);
	const FResidencyEntryId W = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(3), true);
	REQUIRE_EQ(Core.Charge(W, 300), EChargeOutcome::Deferred); // A and B both evicted; 100 of B unclaimed
	EXPECT_EQ(Core.TakeEvicted().size(), static_cast<size_t>(2));
	EXPECT_EQ(Led(Core).ClaimedRefundBytes, 300u);
	REQUIRE_EQ(Core.ReleaseCompleted(B), true);
	EXPECT_EQ(Core.TakeAdmitted().empty(), true); // A's refund is still pending
	EXPECT_EQ(Core.GetWaitingHeldBytes(W), 100u); // B's claimed 100 passed to W
	EXPECT_EQ(Led(Core).ChargedBytes, 300u);      // B's unclaimed 100 returned to free charge
	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	const std::vector<FResidencyEntryId> Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Core.GetState(W), EResidencyState::InFlight);
	EXPECT_EQ(Core.GetChargedBytes(W), 300u);
	EXPECT_TRUE(LedgerAdds(Led(Core)));
}

HOST_TEST(refund_refused_request_keeps_its_evictions_and_its_refund_is_claimable, "8.14 (ii) orphaned refund; §5.11 (a) refused: claims released, evictions stand")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 600);
	const FResidencyEntryId E = Resident(Core, 1, 300);
	const FResidencyEntryId K = Resident(Core, 2, 300);
	REQUIRE_TRUE(E != kInvalidResidencyEntry && K != kInvalidResidencyEntry);
	Core.AddPin(K, EResidencyPin::Lease);
	const FResidencyEntryId Y = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(3), true);
	EXPECT_EQ(Core.Charge(Y, 400), EChargeOutcome::BudgetExhausted);
	const std::vector<FResidencyEntryId> Evicted = Core.TakeEvicted();
	REQUIRE_EQ(Evicted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Evicted[0], E);
	EXPECT_EQ(Core.GetState(E), EResidencyState::Releasing);
	EXPECT_EQ(Core.GetState(Y), EResidencyState::Unadmitted);
	EXPECT_EQ(Led(Core).ClaimedRefundBytes, 0u);
	EXPECT_EQ(Led(Core).ChargedBytes, 600u);
	const FResidencyEntryId Z = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(4), true);
	EXPECT_EQ(Core.Charge(Z, 300), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.GetClaimedBytes(Z), 300u);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	REQUIRE_EQ(Core.ReleaseCompleted(E), true);
	const std::vector<FResidencyEntryId> Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Admitted[0], Z);
	EXPECT_EQ(Led(Core).ChargedBytes, 600u);
	EXPECT_EQ(Core.Snapshot().Evictions, 1u);
}

HOST_TEST(refund_a_returned_waiter_releases_its_claims_and_its_free_charge, "§5.11 (a) re-Configure and teardown: a waiter's partial charge leaves; claims return to unclaimed")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 400);
	const FResidencyEntryId A = Resident(Core, 1, 300);
	REQUIRE_TRUE(A != kInvalidResidencyEntry);
	const FResidencyEntryId W = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(2), true);
	REQUIRE_EQ(Core.Charge(W, 250), EChargeOutcome::Deferred); // 100 free taken, 150 of A claimed
	Core.ReturnCharge(W);
	EXPECT_EQ(Core.GetState(W), EResidencyState::Unadmitted);
	FResidencyLedger L = Led(Core);
	EXPECT_EQ(L.WaitingBytes, 0u);
	EXPECT_EQ(L.ClaimedRefundBytes, 0u);
	EXPECT_EQ(L.ChargedBytes, 300u);
	EXPECT_TRUE(LedgerAdds(L));
	// The whole of A's refund is claimable again.
	const FResidencyEntryId X = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(3), true);
	EXPECT_EQ(Core.Charge(X, 400), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.GetClaimedBytes(X), 300u);
	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	EXPECT_EQ(Core.TakeAdmitted().size(), static_cast<size_t>(1));
	EXPECT_EQ(Core.GetState(W), EResidencyState::Unadmitted); // never admitted after its return
}

HOST_TEST(refund_unclaimed_before_new_evictions_oldest_first, "§5.11 (a) cover order: free, unclaimed oldest eviction first, then new evictions; never evicts while unclaimed refunds cover it")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 600);
	const FResidencyEntryId A = Resident(Core, 1, 200);
	const FResidencyEntryId B = Resident(Core, 2, 200);
	const FResidencyEntryId C = Resident(Core, 3, 200);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry && C != kInvalidResidencyEntry);
	FResidencyEntryId Evicted = kInvalidResidencyEntry;
	REQUIRE_EQ(Core.EvictLru(kResidencyKindPrefix, kHost, Evicted), true); // A: an unclaimed pending refund
	EXPECT_EQ(Evicted, A);
	REQUIRE_EQ(Core.EvictLru(kResidencyKindPrefix, kHost, Evicted), true); // B
	EXPECT_EQ(Evicted, B);
	Core.TakeEvicted();
	// W needs 300: A's 200 (the oldest), then 100 of B's; C stays.
	const FResidencyEntryId W = Core.CreateEntry(kResidencyKindPrefix, kHost, RKey(4), true);
	REQUIRE_EQ(Core.Charge(W, 300), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	EXPECT_EQ(Core.GetState(C), EResidencyState::Resident);
	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	EXPECT_EQ(Core.TakeAdmitted().empty(), true); // it also claims on B
	REQUIRE_EQ(Core.ReleaseCompleted(B), true);
	EXPECT_EQ(Core.TakeAdmitted().size(), static_cast<size_t>(1));
	EXPECT_EQ(Led(Core).ChargedBytes, 500u); // C 200 + W 300; B's unclaimed 100 came back
}
