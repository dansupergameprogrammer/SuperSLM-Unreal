// The kind-neutral residency core (LoRA residency plan §5.0, §5.3; precache plan §5.4). Kind 1 here
// is a test-only second kind standing in for adapters.

#include "harness.h"
#include "SuperSLMResidencyCore.h"
#include "SuperSLMPrefixCacheCore.h"

using namespace SuperSLMCore;

namespace
{
	constexpr FResidencyKind kTestKind = 1;
	constexpr FResidencyLedgerId kHost = kResidencyLedgerHostRam;

	FResidencyKey KeyOf(uint8_t N)
	{
		FResidencyKey K;
		K.Bytes[0] = N;
		K.Bytes[31] = static_cast<uint8_t>(0xFF - N);
		return K;
	}

	// Creates, charges and settles an entry: Resident, unpinned.
	FResidencyEntryId MakeResident(FSuperSLMResidencyCore& Core, FResidencyKind Kind, uint8_t N, uint64_t Bytes,
		FResidencyLedgerId Ledger = kResidencyLedgerHostRam)
	{
		const FResidencyEntryId Id = Core.CreateEntry(Kind, Ledger, KeyOf(N), true);
		if (Id == kInvalidResidencyEntry || Core.Charge(Id, Bytes) != EChargeOutcome::Charged)
		{
			return kInvalidResidencyEntry;
		}
		Core.Settle(Id, Bytes);
		return Id;
	}
}

HOST_TEST(residency_charge_evicts_lru_unpinned, "§5.4 charge rule and LRU (core)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(2, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 300);
	const FResidencyEntryId A = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	const FResidencyEntryId B = MakeResident(Core, kResidencyKindPrefix, 2, 100);
	const FResidencyEntryId C = MakeResident(Core, kResidencyKindPrefix, 3, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry && C != kInvalidResidencyEntry);
	Core.Touch(A); // order now B, C, A

	const std::vector<FResidencyEntryId> Order = Core.LruOrder(kResidencyKindPrefix, kHost);
	REQUIRE_EQ(Order.size(), static_cast<size_t>(3));
	EXPECT_EQ(Order[0], B);
	EXPECT_EQ(Order[1], C);
	EXPECT_EQ(Order[2], A);

	const FResidencyEntryId D = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(4), true);
	EXPECT_EQ(Core.Charge(D, 150), EChargeOutcome::Deferred); // B and C must go; their bytes are still held
	const std::vector<FResidencyEntryId> Evicted = Core.TakeEvicted();
	REQUIRE_EQ(Evicted.size(), static_cast<size_t>(2));
	EXPECT_EQ(Evicted[0], B);
	EXPECT_EQ(Evicted[1], C);
	EXPECT_EQ(Core.GetState(A), EResidencyState::Resident);
	EXPECT_EQ(Core.GetState(B), EResidencyState::Releasing);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 300u);
	EXPECT_EQ(Core.FindByKey(kResidencyKindPrefix, KeyOf(2)), kInvalidResidencyEntry); // a Releasing entry is not found
}

HOST_TEST(residency_release_before_reuse, "LoRA §5.3 release before reuse; precache §5.4 (ledger never exceeds budget)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 200);
	const FResidencyEntryId A = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	const FResidencyEntryId B = MakeResident(Core, kResidencyKindPrefix, 2, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry);
	const FResidencyEntryId C = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(3), true);
	const FResidencyEntryId D = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(4), true);
	REQUIRE_EQ(Core.Charge(C, 100), EChargeOutcome::Deferred);
	REQUIRE_EQ(Core.Charge(D, 100), EChargeOutcome::Deferred);
	EXPECT_EQ(Core.GetState(C), EResidencyState::Waiting);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 200u);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ReleasingBytes, 200u);
	EXPECT_EQ(Core.TakeAdmitted().empty(), true);

	REQUIRE_EQ(Core.ReleaseCompleted(A), true);
	std::vector<FResidencyEntryId> Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Admitted[0], C); // FIFO
	EXPECT_EQ(Core.GetState(C), EResidencyState::InFlight);
	EXPECT_EQ(Core.GetState(D), EResidencyState::Waiting);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 200u);
	EXPECT_EQ(Core.ReleaseCompleted(A), false); // once only

	REQUIRE_EQ(Core.ReleaseCompleted(B), true);
	Admitted = Core.TakeAdmitted();
	REQUIRE_EQ(Admitted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Admitted[0], D);
	EXPECT_LE(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 200u);
}

HOST_TEST(residency_budget_exhausted_evicts_nothing, "5.5 (core), §5.4 BudgetExhausted")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 300);
	const FResidencyEntryId A = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	const FResidencyEntryId B = MakeResident(Core, kResidencyKindPrefix, 2, 100);
	const FResidencyEntryId C = MakeResident(Core, kResidencyKindPrefix, 3, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry && B != kInvalidResidencyEntry && C != kInvalidResidencyEntry);
	Core.AddPin(A, EResidencyPin::Lease);
	Core.AddPin(B, EResidencyPin::Adopt);
	// Only C is evictable: 100 bytes can be freed, 250 are asked. Refused; C stays.
	const FResidencyEntryId D = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(4), true);
	EXPECT_EQ(Core.Charge(D, 250), EChargeOutcome::BudgetExhausted);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	EXPECT_EQ(Core.GetState(C), EResidencyState::Resident);
	EXPECT_EQ(Core.GetState(D), EResidencyState::Unadmitted);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 300u);
	// Every entry pinned: refused, nothing evicted.
	Core.AddPin(C, EResidencyPin::Save);
	EXPECT_EQ(Core.Charge(D, 1), EChargeOutcome::BudgetExhausted);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	EXPECT_EQ(Core.Snapshot().BudgetExhausted, 2u);
	// Larger than the whole budget: refused even with nothing resident to keep.
	FSuperSLMResidencyCore Empty(1, &Counter);
	Empty.SetBudget(kResidencyKindPrefix, kHost, 300);
	const FResidencyEntryId E = Empty.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(5), true);
	EXPECT_EQ(Empty.Charge(E, 301), EChargeOutcome::BudgetExhausted);
	EXPECT_EQ(Empty.Charge(E, 300), EChargeOutcome::Charged);
}

HOST_TEST(residency_settle_and_return_charge, "§5.4 (charge falls at freeze; refused or failed returns the whole charge; nested restore charged s = 0 then reduced)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 1000);
	const FResidencyEntryId A = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(1), true);
	REQUIRE_EQ(Core.Charge(A, 300), EChargeOutcome::Charged); // a build at R(L)
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).InFlightBytes, 300u);
	Core.Settle(A, 200); // freeze: ceil(L/B)
	EXPECT_EQ(Core.GetState(A), EResidencyState::Resident);
	EXPECT_EQ(Core.GetChargedBytes(A), 200u);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ResidentBytes, 200u);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).InFlightBytes, 0u);

	const FResidencyEntryId B = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(2), true);
	REQUIRE_EQ(Core.Charge(B, 400), EChargeOutcome::Charged);
	Core.ReturnCharge(B); // refused
	EXPECT_EQ(Core.GetState(B), EResidencyState::Unadmitted);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 200u);
	REQUIRE_EQ(Core.Charge(B, 500), EChargeOutcome::Charged); // next tier
	Core.Settle(B, 900); // a settle never grows a charge
	EXPECT_EQ(Core.GetChargedBytes(B), 500u);

	const FResidencyEntryId N = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(3), true);
	REQUIRE_EQ(Core.Charge(N, 300), EChargeOutcome::Charged); // nested restore at s = 0
	Core.Settle(N, 100);                                       // s pages shared
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 800u);
	EXPECT_EQ(Core.Destroy(N), false); // only an Unadmitted entry is destroyed
	EXPECT_EQ(Core.Destroy(Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(9), true)), true);
}

HOST_TEST(residency_kinds_do_not_mix, "LoRA §5.0 (per-kind budgets, not one pooled number)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(2, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 200);
	Core.SetBudget(kTestKind, kHost, 100);
	const FResidencyEntryId P1 = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	const FResidencyEntryId T1 = MakeResident(Core, kTestKind, 1, 100); // same key bytes, other kind
	REQUIRE_TRUE(P1 != kInvalidResidencyEntry && T1 != kInvalidResidencyEntry);
	EXPECT_NE(P1, T1);
	EXPECT_EQ(Core.FindByKey(kResidencyKindPrefix, KeyOf(1)), P1);
	EXPECT_EQ(Core.FindByKey(kTestKind, KeyOf(1)), T1);

	// The test kind is full; a second test-kind entry evicts the test kind's own LRU, never a prefix.
	const FResidencyEntryId T2 = Core.CreateEntry(kTestKind, kHost, KeyOf(2), true);
	EXPECT_EQ(Core.Charge(T2, 100), EChargeOutcome::Deferred);
	const std::vector<FResidencyEntryId> Evicted = Core.TakeEvicted();
	REQUIRE_EQ(Evicted.size(), static_cast<size_t>(1));
	EXPECT_EQ(Evicted[0], T1);
	EXPECT_EQ(Core.GetState(P1), EResidencyState::Resident);

	// The prefix kind has 100 bytes free: a prefix admission fits without touching the test kind,
	// although the test kind is over-subscribed. One pooled number would have refused or evicted.
	const FResidencyEntryId P2 = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(2), true);
	EXPECT_EQ(Core.Charge(P2, 100), EChargeOutcome::Charged);
	EXPECT_EQ(Core.GetLedger(kResidencyKindPrefix, kHost).ChargedBytes, 200u);
	EXPECT_EQ(Core.GetLedger(kTestKind, kHost).ChargedBytes, 100u);
	// A prefix admission past the prefix budget is refused on the prefix ledger alone.
	const FResidencyEntryId P3 = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(3), true);
	Core.AddPin(P1, EResidencyPin::Lease);
	EXPECT_EQ(Core.Charge(P3, 100), EChargeOutcome::BudgetExhausted);
	EXPECT_EQ(Core.GetState(T2), EResidencyState::Waiting);

	const FResidencySnapshot Snap = Core.Snapshot();
	EXPECT_EQ(Snap.Ledgers.size(), static_cast<size_t>(2 * 2)); // two kinds, two ledgers each
}

HOST_TEST(residency_classes_do_not_mix, "LoRA §5.3 (each budget has its own ledger)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(2, &Counter);
	Core.SetBudget(kTestKind, kResidencyLedgerHostRam, 100);
	Core.SetBudget(kTestKind, kResidencyLedgerVram, 100);
	const FResidencyEntryId H = MakeResident(Core, kTestKind, 1, 100, kResidencyLedgerHostRam);
	REQUIRE_TRUE(H != kInvalidResidencyEntry);
	const FResidencyEntryId V = Core.CreateEntry(kTestKind, kResidencyLedgerVram, KeyOf(2), true);
	EXPECT_EQ(Core.Charge(V, 100), EChargeOutcome::Charged);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	EXPECT_EQ(Core.GetState(H), EResidencyState::Resident);
	EXPECT_EQ(Core.LruOrder(kTestKind, kResidencyLedgerVram).empty(), true);
	EXPECT_EQ(Core.LruOrder(kTestKind, kResidencyLedgerHostRam).size(), static_cast<size_t>(1));
}

HOST_TEST(residency_lease_handles_come_from_the_callers_counter, "§5.6 one handle per lease")
{
	int64_t Counter = 500;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 100);
	const FResidencyEntryId A = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry);
	const int64_t L1 = Core.MintLease();
	const int64_t Other = Counter++; // a CreatePrefix handle minted between two leases
	const int64_t L2 = Core.MintLease();
	EXPECT_EQ(L1, 500);
	EXPECT_EQ(Other, 501);
	EXPECT_EQ(L2, 502);
	EXPECT_EQ(Counter, 503);
	EXPECT_EQ(Core.GetLeaseStatus(Other), ELeaseStatus::Unknown);

	REQUIRE_EQ(Core.AttachLease(L1, A), true);
	REQUIRE_EQ(Core.AttachLease(L2, A), true);
	EXPECT_EQ(Core.GetPinCount(A, EResidencyPin::Lease), 2u);
	EXPECT_EQ(Core.AttachLease(L1, A), false); // already attached
	EXPECT_EQ(Core.EndLease(L1), true);
	EXPECT_EQ(Core.EndLease(L1), false);
	EXPECT_EQ(Core.GetLeaseStatus(L1), ELeaseStatus::Ended);
	EXPECT_EQ(Core.GetLeaseStatus(L2), ELeaseStatus::Live);
	EXPECT_EQ(Core.GetLeaseEntry(L2), A);
	EXPECT_EQ(Core.IsPinned(A), true);
	EXPECT_EQ(Core.DetachLease(L2), true);
	EXPECT_EQ(Core.IsPinned(A), false);
	EXPECT_EQ(Core.GetLeaseStatus(L2), ELeaseStatus::Live);
	EXPECT_EQ(Core.EndLease(Other), false);

	Core.Reset();
	EXPECT_EQ(Core.GetLeaseStatus(L2), ELeaseStatus::Unknown);
	EXPECT_EQ(Core.MintLease(), 503); // the counter is never rewound
}

HOST_TEST(residency_pins_are_counted_per_reason, "§5.4 pins (adopt, save, child op, lease)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 100);
	const FResidencyEntryId A = MakeResident(Core, kResidencyKindPrefix, 1, 100);
	REQUIRE_TRUE(A != kInvalidResidencyEntry);
	for (EResidencyPin Pin : {EResidencyPin::Adopt, EResidencyPin::Save, EResidencyPin::ChildOp, EResidencyPin::WorkerJob})
	{
		Core.AddPin(A, Pin);
		Core.AddPin(A, Pin);
		EXPECT_EQ(Core.GetPinCount(A, Pin), 2u);
		EXPECT_EQ(Core.LruOrder(kResidencyKindPrefix, kHost).empty(), true);
		FResidencyEntryId Out = 0;
		EXPECT_EQ(Core.EvictLru(kResidencyKindPrefix, kHost, Out), false);
		EXPECT_EQ(Core.RemovePin(A, Pin), true);
		EXPECT_EQ(Core.IsPinned(A), true);
		EXPECT_EQ(Core.RemovePin(A, Pin), true);
		EXPECT_EQ(Core.IsPinned(A), false);
		EXPECT_EQ(Core.RemovePin(A, Pin), false);
	}
	FResidencyEntryId Out = 0;
	EXPECT_EQ(Core.EvictLru(kResidencyKindPrefix, kHost, Out), true);
	EXPECT_EQ(Out, A);
	EXPECT_EQ(Core.GetState(A), EResidencyState::Releasing);
	EXPECT_EQ(Core.Snapshot().Evictions, 1u);
}

HOST_TEST(residency_key_lookup_and_unkeyed_entries, "§5.4 coalescing (one entry per key)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(1, &Counter);
	Core.SetBudget(kResidencyKindPrefix, kHost, 1000);
	const FResidencyEntryId A = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(1), true);
	REQUIRE_NE(A, kInvalidResidencyEntry);
	EXPECT_EQ(Core.FindByKey(kResidencyKindPrefix, KeyOf(1)), A); // findable while Unadmitted / in flight
	EXPECT_EQ(Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(1), true), kInvalidResidencyEntry);
	const FResidencyEntryId U = Core.CreateEntry(kResidencyKindPrefix, kHost, KeyOf(1), false);
	EXPECT_NE(U, kInvalidResidencyEntry);
	EXPECT_NE(U, A);
	EXPECT_EQ(Core.FindByKey(kResidencyKindPrefix, KeyOf(1)), A);
	EXPECT_EQ(Core.GetState(U), EResidencyState::Unadmitted);
	EXPECT_EQ(Core.GetState(9999), EResidencyState::Unknown);
}

HOST_TEST(residency_and_cache_take_a_long_long_counter, "§5.6 one handle per lease (Unreal's int64 is long long on every platform)")
{
	// The plugin's prefix-id counter is an Unreal int64, which is long long even where int64_t is long.
	long long Counter = 40;
	FSuperSLMResidencyCore Core(1, &Counter);
	EXPECT_EQ(Core.MintLease(), 40);
	EXPECT_EQ(Counter, 41LL);
	FSuperSLMPrefixCacheCore Cache(&Counter);
	EXPECT_EQ(Cache.IsConfigured(), false);
}

HOST_TEST(residency_two_ledgers_of_one_class_do_not_mix, "LoRA §5.3 (every charge names the ledger it lands in; Resident and Staged are both HostRam)")
{
	int64_t Counter = 1;
	FSuperSLMResidencyCore Core(2, &Counter);
	constexpr FResidencyLedgerId kStaged = 2;
	REQUIRE_EQ(Core.AddLedger(kTestKind, kStaged, EResidencyClass::HostRam), true);
	EXPECT_EQ(Core.AddLedger(kTestKind, kStaged, EResidencyClass::HostRam), false); // the id is taken
	EXPECT_EQ(Core.AddLedger(2, 5, EResidencyClass::HostRam), false);               // no such kind
	EXPECT_EQ(Core.GetLedger(kTestKind, kStaged).Class, EResidencyClass::HostRam);
	EXPECT_EQ(Core.GetLedger(kTestKind, kResidencyLedgerVram).Class, EResidencyClass::Vram);
	Core.SetBudget(kTestKind, kResidencyLedgerHostRam, 100);
	Core.SetBudget(kTestKind, kStaged, 100);
	const FResidencyEntryId Resident = MakeResident(Core, kTestKind, 1, 100, kResidencyLedgerHostRam);
	REQUIRE_TRUE(Resident != kInvalidResidencyEntry);
	// The Staged ledger has its own 100 bytes: charged, and the Resident ledger's entry is not evicted.
	const FResidencyEntryId Staged = Core.CreateEntry(kTestKind, kStaged, KeyOf(2), true);
	EXPECT_EQ(Core.Charge(Staged, 100), EChargeOutcome::Charged);
	EXPECT_EQ(Core.TakeEvicted().empty(), true);
	EXPECT_EQ(Core.GetLedger(kTestKind, kResidencyLedgerHostRam).ChargedBytes, 100u);
	EXPECT_EQ(Core.GetLedger(kTestKind, kStaged).ChargedBytes, 100u);
	EXPECT_EQ(Core.LruOrder(kTestKind, kStaged).empty(), true);
	// A reset keeps the ledger defined, with its budget back to 0.
	Core.Reset();
	EXPECT_EQ(Core.GetLedger(kTestKind, kStaged).Class, EResidencyClass::HostRam);
	EXPECT_EQ(Core.GetLedger(kTestKind, kStaged).BudgetBytes, 0u);
	EXPECT_EQ(Core.Snapshot().Ledgers.size(), static_cast<size_t>(5));
}
