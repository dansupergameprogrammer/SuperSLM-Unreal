#pragma once

// The kind-neutral residency core (LoRA residency plan §5.0): the cache policy the prefix cache
// (precache plan §5.4-§5.5) needs, built so the streamed-adapter cache can share it. Prefixes are its
// first and, so far, only kind.
//
// What it owns, and nothing else:
//   - ledgers, keyed by (kind, ledger id), each with a memory class, a budget and the bytes charged
//     against it. Every kind starts with one HostRam and one Vram ledger; AddLedger adds more, so a
//     kind can hold two budgets of one class (the adapters' Resident and Staged, both HostRam);
//   - the charge rule: a request is charged its PEAK before anything is drawn, the charge falls to
//     the steady figure when the result is applied, and a refused or failed request returns its
//     whole charge;
//   - LRU eviction of unpinned, unleased resident entries, within one ledger only;
//   - pins (counted per reason), leases with one handle per lease, and key lookup (coalescing);
//   - release-before-reuse: an evicted entry's charge stays on the ledger, as Releasing, until the
//     caller reports that its engine handle was actually released (LoRA residency plan §5.3, "Release
//     before reuse"). Each such charge is a PENDING REFUND.
//   - deferred-refund admission (precache plan §5.11 (a)): a request covers its peak from, in order,
//     the free charge, the UNCLAIMED parts of pending refunds (oldest eviction first), and new
//     evictions of least-recently-used unpinned entries, stopping as soon as it is covered. What it
//     takes of a pending refund is a CLAIM earmarked to it; the rest of that refund stays unclaimed
//     and any later request may claim it. Covered by the free charge alone: Charged at once.
//     Covered with claims: Waiting. A waiter takes the free charge it uses onto the ledger in its own
//     name at once, draws nothing, and is admitted (InFlight, whole peak charged) exactly when the
//     last pending refund it holds a claim on completes. Not covered: BudgetExhausted; its claims
//     return to unclaimed and the evictions it decided stand.
//   - nested entries (precache plan §5.4, §5.11 (a), step U6): an entry may name a PARENT whose full
//     pages it shares. Charging a child pins its parent (ChildOp) from the decision -- a waiter too --
//     until it settles or returns its charge; the decision touches the parent first. A child whose
//     parent is REQUIRED (a build, begin_from) never evicts that parent in its own loop; one whose
//     parent is only a hint (a restore) may, and then runs private (its parent link is cut).
//   - ghost charges: an entry evicted while a cached descendant still shares its pages keeps its whole
//     charge on the ledger as a GHOST until its last cached descendant (not only child) is evicted.
//   - pending refunds wait on their FAMILY (§5.11 (a), rev 1.17): every pending refund -- an evicted
//     entry's own charge, or an ended ghost's -- completes only when every pending release among that
//     entry's own handle and its descendants' handles has been reported, in whatever order. Until then
//     it stays claimable. A root's family is its own release.
//
// What the adapter step adds, and this core does not yet do (LoRA residency plan §5.3): a charge that
// lands in more than one ledger at once, a budget of 0 meaning unlimited, and External entries that
// hold bytes outside the charge rule.
//
// It is game-thread bookkeeping. It never calls Layer 1, never touches a file, never takes a lock:
// the game thread owns it, and work done elsewhere reaches it as calls made from the apply pass.
//
// Build constraints (it compiles inside Unreal later): C++17, standard library only, no exceptions,
// no RTTI, no dynamically initialised globals.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace SuperSLMCore
{
	// A kind is a small integer. Kind 0 is the prefix cache. Others (adapters) are added by their
	// own plans; the core does not enumerate them.
	using FResidencyKind = uint8_t;
	constexpr FResidencyKind kResidencyKindPrefix = 0;

	// Where a ledger's bytes live.
	enum class EResidencyClass : uint8_t
	{
		HostRam = 0,
		Vram = 1,
	};
	constexpr uint32_t kResidencyClassCount = 2;

	// A ledger within a kind. Each charge lands in the one ledger its entry was created in, and
	// budgets never mix across ledgers.
	using FResidencyLedgerId = uint8_t;
	constexpr FResidencyLedgerId kResidencyLedgerHostRam = 0; // every kind has it, class HostRam
	constexpr FResidencyLedgerId kResidencyLedgerVram = 1;    // every kind has it, class Vram

	// Why an entry must stay alive. Each reason is counted: two leases are two Lease pins.
	// An entry with any pin of any reason is never evicted.
	enum class EResidencyPin : uint8_t
	{
		Lease = 0,     // a caller holds a lease on it (added and removed by the lease calls below)
		Adopt = 1,     // an adopt of it is queued or running
		Save = 2,      // a save of it is queued or running
		ChildOp = 3,   // a child restore or build uses it as parent
		WorkerJob = 4, // any other worker job reads it
	};
	constexpr uint32_t kResidencyPinCount = 5;

	enum class EResidencyState : uint8_t
	{
		Unknown = 0,    // no such entry (never created, or destroyed)
		Unadmitted = 1, // created; holds no charge (never charged, or its charge was returned)
		Waiting = 2,    // a waiter: holds the free charge it took (WaitingBytes) and claims on pending refunds
		InFlight = 3,   // charged at its peak; its load or build has been or may be issued
		Resident = 4,   // charged at its steady figure; holds an engine handle
		Releasing = 5,  // evicted; its charge stays, as a pending refund (or a ghost), until ReleaseCompleted
		Ghost = 6,      // evicted and released, but a cached descendant still shares its pages: only its ghost charge remains
	};

	enum class EChargeOutcome : uint8_t
	{
		Charged = 0,         // the entry is InFlight with the whole peak charged
		Deferred = 1,        // covered only with claims on pending refunds: the entry is Waiting, and
		                     // is admitted when the last refund it claims on completes
		BudgetExhausted = 2, // not covered even by every unclaimed refund and every evictable entry:
		                     // nothing is charged, its claims are released; evictions it decided stand
	};

	enum class ELeaseStatus : uint8_t
	{
		Unknown = 0, // never minted by this core (or minted before the core was reset)
		Live = 1,    // minted and not ended
		Ended = 2,   // EndLease was called on it
	};

	struct FResidencyKey
	{
		std::array<uint8_t, 32> Bytes{};

		bool operator==(const FResidencyKey& Other) const { return Bytes == Other.Bytes; }
		bool operator!=(const FResidencyKey& Other) const { return Bytes != Other.Bytes; }
		bool operator<(const FResidencyKey& Other) const { return Bytes < Other.Bytes; }
	};

	using FResidencyEntryId = uint64_t;
	constexpr FResidencyEntryId kInvalidResidencyEntry = 0;

	// One (kind, ledger id) ledger. Invariants (precache plan §5.4, §5.11 (a), cells 7.10 and 8.14):
	//   ChargedBytes == ResidentBytes + InFlightBytes + ReleasingBytes + WaitingBytes + GhostBytes,
	//   ChargedBytes <= BudgetBytes, and ClaimedRefundBytes <= ReleasingBytes.
	// ReleasingBytes is every pending refund (ReleasingEntries counts them: an evicted entry's own, or
	// an ended ghost's); ClaimedRefundBytes is the part earmarked to waiters, and the rest is
	// unclaimed. A waiter's own charge is its WaitingBytes plus its claims. GhostBytes is the charge of
	// evicted entries whose pages cached descendants still share (precache plan §5.4, U6).
	struct FResidencyLedger
	{
		EResidencyClass Class = EResidencyClass::HostRam;
		uint64_t BudgetBytes = 0;
		uint64_t ChargedBytes = 0;
		uint64_t ResidentBytes = 0;
		uint64_t InFlightBytes = 0;
		uint64_t ReleasingBytes = 0;
		uint64_t WaitingBytes = 0;       // the free charge waiters took at their decision
		uint64_t ClaimedRefundBytes = 0; // the parts of pending refunds claimed by waiters
		uint64_t GhostBytes = 0;         // ghost charges (nested entries)
		uint32_t ResidentEntries = 0;
		uint32_t InFlightEntries = 0;
		uint32_t WaitingEntries = 0;
		uint32_t ReleasingEntries = 0;
		uint32_t GhostEntries = 0;
	};

	struct FResidencyLedgerRow
	{
		FResidencyKind Kind = 0;
		FResidencyLedgerId LedgerId = kResidencyLedgerHostRam;
		FResidencyLedger Ledger;
	};

	// The diagnostics snapshot (LoRA residency plan §5.6's FSuperSLMResidencySnapshot, core half).
	struct FResidencySnapshot
	{
		std::vector<FResidencyLedgerRow> Ledgers; // one row per (kind, ledger id), in that order
		uint64_t Evictions = 0;                   // entries moved to Releasing by the charge rule or EvictLru
		uint64_t BudgetExhausted = 0;             // Charge calls that returned BudgetExhausted
		uint64_t Deferred = 0;                    // Charge calls that returned Deferred
		uint64_t LeasesMinted = 0;
		uint64_t GhostEndings = 0;                // ghosts whose last cached descendant was evicted
	};

	// The caller's handle counter, whatever its 64-bit signed type: Unreal's int64 is long long on
	// every platform, while int64_t is long on some. Post-incremented once per handle minted.
	class FResidencyHandleCounter
	{
	public:
		template <typename TInt>
		explicit FResidencyHandleCounter(TInt* InCounter) : Counter(InCounter), Bump(&BumpAs<TInt>)
		{
			static_assert(std::is_integral<TInt>::value && std::is_signed<TInt>::value && sizeof(TInt) == 8,
				"the handle counter is a 64-bit signed integer");
		}

		int64_t Next() const { return Bump(Counter); }

	private:
		template <typename TInt>
		static int64_t BumpAs(void* InCounter)
		{
			return static_cast<int64_t>((*static_cast<TInt*>(InCounter))++);
		}

		void* Counter;
		int64_t (*Bump)(void*);
	};

	class FSuperSLMResidencyCore
	{
	public:
		// KindCount: kinds 0..KindCount-1 are valid. HandleCounter: the caller's handle counter (the
		// plugin's prefix-id counter, so a lease handle can never share an id with a handle minted
		// elsewhere from the same counter). It must outlive the core; MintLease post-increments it.
		template <typename TInt>
		FSuperSLMResidencyCore(uint32_t KindCount, TInt* HandleCounter)
			: FSuperSLMResidencyCore(KindCount, FResidencyHandleCounter(HandleCounter))
		{
		}
		FSuperSLMResidencyCore(uint32_t KindCount, const FResidencyHandleCounter& HandleCounter);
		~FSuperSLMResidencyCore();
		FSuperSLMResidencyCore(const FSuperSLMResidencyCore&) = delete;
		FSuperSLMResidencyCore& operator=(const FSuperSLMResidencyCore&) = delete;

		// Drops every entry, lease and counter; budgets return to 0. Ledgers added stay defined. Handles
		// minted before a reset are Unknown afterwards. The handle counter is the caller's and is never
		// rewound.
		void Reset();

		// Adds a ledger to a kind. False if the kind is invalid or the id is taken.
		bool AddLedger(FResidencyKind Kind, FResidencyLedgerId Ledger, EResidencyClass Class);
		void SetBudget(FResidencyKind Kind, FResidencyLedgerId Ledger, uint64_t Bytes);
		FResidencyLedger GetLedger(FResidencyKind Kind, FResidencyLedgerId Ledger) const;

		// --- Entries -------------------------------------------------------------------------------
		// Creates an Unadmitted entry whose charges land in (Kind, Ledger). A keyed entry is findable by FindByKey until it starts
		// Releasing; an unkeyed one (bKeyed false) never is. Creating a second keyed entry for a key
		// that FindByKey still finds returns kInvalidResidencyEntry.
		FResidencyEntryId CreateEntry(FResidencyKind Kind, FResidencyLedgerId Ledger, const FResidencyKey& Key, bool bKeyed);
		FResidencyEntryId FindByKey(FResidencyKind Kind, const FResidencyKey& Key) const;
		EResidencyState GetState(FResidencyEntryId Id) const;
		uint64_t GetChargedBytes(FResidencyEntryId Id) const;

		// The charge rule with deferred-refund admission (header comment). Precondition: the entry is
		// Unadmitted. Evictions are of the SAME ledger only; evicted entries move to Releasing
		// (TakeEvicted lists them). With a parent (SetParent): the parent is touched first; when it is
		// no longer Resident, or a hint-only parent (bParentRequired false) is evicted by this loop, the
		// link is cut (GetParent reads 0: the load runs private). A required parent is never evicted by
		// this loop. Charged or Deferred, the parent is ChildOp-pinned until Settle or ReturnCharge.
		EChargeOutcome Charge(FResidencyEntryId Id, uint64_t PeakBytes, bool bParentRequired = false);

		// --- Nested entries (U6) ---------------------------------------------------------------------
		// Names Parent as the entry whose full pages Id shares. Id must be Unadmitted or Resident, Parent
		// Resident in the same kind and ledger. False (nothing changes) otherwise.
		bool SetParent(FResidencyEntryId Id, FResidencyEntryId Parent);
		// The parent link, or kInvalidResidencyEntry (a root, or a link cut).
		FResidencyEntryId GetParent(FResidencyEntryId Id) const;
		// Cuts the link: the entry shares none of its parent's pages (a private restore).
		void DetachParent(FResidencyEntryId Id);
		// True while the entry holds a ghost charge (Releasing or Ghost); its amount.
		bool IsGhost(FResidencyEntryId Id) const;
		uint64_t GetGhostBytes(FResidencyEntryId Id) const;

		// The result was applied: InFlight -> Resident, charge reduced to SteadyBytes. SteadyBytes
		// above the current charge is clamped to it (a charge never grows after admission).
		void Settle(FResidencyEntryId Id, uint64_t SteadyBytes);

		// Refused or failed: InFlight or Waiting -> Unadmitted, the whole charge returned (a waiter's
		// claims go back to unclaimed). The entry may be charged again (the next tier) or destroyed.
		void ReturnCharge(FResidencyEntryId Id);

		// Destroys an Unadmitted entry. Any other state is left unchanged and false is returned.
		bool Destroy(FResidencyEntryId Id);

		// Moves the least-recently-used unpinned Resident entry of (Kind, Ledger) to Releasing (the
		// pool-exhausted evict-and-retry rule, precache plan §5.4). False if none is evictable.
		bool EvictLru(FResidencyKind Kind, FResidencyLedgerId Ledger, FResidencyEntryId& OutEvicted);

		// Teardown: moves a Resident entry to Releasing whatever its pins. False otherwise.
		bool EvictForTeardown(FResidencyEntryId Id);

		// --- Targeted eviction and marks (precache plan §5.6, rev 1.18: DropPrefix) -----------------
		// Additive and inert for a kind that never calls them: nothing else reads a mark, and no other
		// call changes behaviour.
		// Moves one named entry to Releasing when it is Resident and unpinned, an eviction no charge
		// decided: its charge becomes a pending refund, unclaimed, exactly as an LRU eviction's (ghost
		// charges included), and it is listed by TakeEvicted. It is not counted in Snapshot().Evictions
		// (the caller counts it apart). False, and nothing changes, for any other entry.
		bool EvictUnpinned(FResidencyEntryId Id);
		// A per-entry mark (a deferred drop). It pins nothing and changes no decision of this core; it
		// goes with its entry. SetMarked is false for an unknown entry.
		bool SetMarked(FResidencyEntryId Id, bool bMarked);
		bool IsMarked(FResidencyEntryId Id) const;

		// The engine handle of a Releasing entry was released, and the caller applied that completion.
		// Every pending refund whose family this was the last pending release of completes (precache
		// plan §5.11 (a)): each claimed part passes to its waiter's charge, the unclaimed rest returns to
		// free charge, and every waiter that holds no other claim is admitted (TakeAdmitted). The entry
		// is destroyed, or, while it holds a ghost charge, stays as a Ghost. False if not Releasing.
		bool ReleaseCompleted(FResidencyEntryId Id);

		// Entries moved to Releasing since the last call, in eviction order.
		std::vector<FResidencyEntryId> TakeEvicted();
		// Waiting entries that became InFlight since the last call: in arrival order within one
		// ReleaseCompleted, and in completion order across calls (GetArrival orders them).
		std::vector<FResidencyEntryId> TakeAdmitted();

		// A waiter's arrival serial (increasing per Charge that returned Deferred); 0 if it never waited.
		uint64_t GetArrival(FResidencyEntryId Id) const;
		// A Waiting entry's free charge taken at its decision, and the sum of its claims.
		uint64_t GetWaitingHeldBytes(FResidencyEntryId Id) const;
		uint64_t GetClaimedBytes(FResidencyEntryId Id) const;
		// Pending refunds of a ledger (an evicted entry's own, or an ended ghost's) not yet complete.
		uint32_t GetPendingRefundCount(FResidencyKind Kind, FResidencyLedgerId Ledger) const;

		// Marks the entry most recently used.
		void Touch(FResidencyEntryId Id);
		// Evictable entries of (Kind, Ledger) -- Resident and unpinned -- least recently used first.
		std::vector<FResidencyEntryId> LruOrder(FResidencyKind Kind, FResidencyLedgerId Ledger) const;

		// --- Pins ----------------------------------------------------------------------------------
		void AddPin(FResidencyEntryId Id, EResidencyPin Reason);
		// False (and nothing changes) when the entry has no pin of that reason.
		bool RemovePin(FResidencyEntryId Id, EResidencyPin Reason);
		uint32_t GetPinCount(FResidencyEntryId Id, EResidencyPin Reason) const;
		bool IsPinned(FResidencyEntryId Id) const;

		// --- Leases: one handle per lease ----------------------------------------------------------
		// Mints a Live lease with the next id from the caller's counter. It is attached to no entry yet.
		int64_t MintLease();
		// Attaches a Live, unattached lease to an entry and adds one Lease pin. False otherwise.
		bool AttachLease(int64_t Handle, FResidencyEntryId Id);
		// Detaches a Live lease from its entry (removing its pin) without ending it. False otherwise.
		bool DetachLease(int64_t Handle);
		// Ends a Live lease, removing its pin if attached. False for Unknown or already Ended.
		bool EndLease(int64_t Handle);
		ELeaseStatus GetLeaseStatus(int64_t Handle) const;
		// The entry a Live lease is attached to, else kInvalidResidencyEntry.
		FResidencyEntryId GetLeaseEntry(int64_t Handle) const;

		FResidencySnapshot Snapshot() const;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
