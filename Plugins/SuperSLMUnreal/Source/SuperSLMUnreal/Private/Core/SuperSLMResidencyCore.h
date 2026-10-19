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
//     before reuse"). An admission that fits only once those bytes come back is Deferred, not refused.
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
		Waiting = 2,    // admission deferred until Releasing bytes return; holds no charge yet
		InFlight = 3,   // charged at its peak; its load or build has been or may be issued
		Resident = 4,   // charged at its steady figure; holds an engine handle
		Releasing = 5,  // evicted; its charge stays until ReleaseCompleted
	};

	enum class EChargeOutcome : uint8_t
	{
		Charged = 0,         // the entry is InFlight with the whole peak charged
		Deferred = 1,        // evictions were started (or were already releasing) that make it fit;
		                     // the entry is Waiting and is charged, FIFO, as releases complete
		BudgetExhausted = 2, // cannot fit even with every unpinned entry evicted and every release
		                     // complete; NOTHING is evicted and nothing is charged
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

	// One (kind, ledger id) ledger. Invariant (precache plan §5.4, cell 7.10):
	//   ChargedBytes == ResidentBytes + InFlightBytes + ReleasingBytes  and  ChargedBytes <= BudgetBytes.
	struct FResidencyLedger
	{
		EResidencyClass Class = EResidencyClass::HostRam;
		uint64_t BudgetBytes = 0;
		uint64_t ChargedBytes = 0;
		uint64_t ResidentBytes = 0;
		uint64_t InFlightBytes = 0;
		uint64_t ReleasingBytes = 0;
		uint32_t ResidentEntries = 0;
		uint32_t InFlightEntries = 0;
		uint32_t WaitingEntries = 0;
		uint32_t ReleasingEntries = 0;
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

		// The charge rule. Precondition: the entry is Unadmitted. Evicts least-recently-used
		// unpinned Resident entries of the SAME ledger until PeakBytes fits, counting bytes
		// already Releasing as returning, less what earlier Waiting admissions of that ledger are
		// owed (they are admitted first, FIFO). Evicted entries move to Releasing (TakeEvicted lists
		// them). If even evicting every unpinned Resident entry would not make it fit, nothing is
		// evicted and the result is BudgetExhausted.
		EChargeOutcome Charge(FResidencyEntryId Id, uint64_t PeakBytes);

		// The result was applied: InFlight -> Resident, charge reduced to SteadyBytes. SteadyBytes
		// above the current charge is clamped to it (a charge never grows after admission).
		void Settle(FResidencyEntryId Id, uint64_t SteadyBytes);

		// Refused or failed: InFlight or Waiting -> Unadmitted, the whole charge returned. The entry
		// may be charged again (the next tier) or destroyed.
		void ReturnCharge(FResidencyEntryId Id);

		// Destroys an Unadmitted entry. Any other state is left unchanged and false is returned.
		bool Destroy(FResidencyEntryId Id);

		// Moves the least-recently-used unpinned Resident entry of (Kind, Ledger) to Releasing (the
		// pool-exhausted evict-and-retry rule, precache plan §5.4). False if none is evictable.
		bool EvictLru(FResidencyKind Kind, FResidencyLedgerId Ledger, FResidencyEntryId& OutEvicted);

		// Teardown: moves a Resident entry to Releasing whatever its pins. False otherwise.
		bool EvictForTeardown(FResidencyEntryId Id);

		// The engine handle of a Releasing entry was released: its charge is returned, the entry is
		// destroyed, and Waiting admissions are retried in FIFO order (TakeAdmitted lists those that
		// were charged). False if the entry was not Releasing.
		bool ReleaseCompleted(FResidencyEntryId Id);

		// Entries moved to Releasing since the last call, in eviction order.
		std::vector<FResidencyEntryId> TakeEvicted();
		// Waiting entries that became InFlight since the last call, in admission order.
		std::vector<FResidencyEntryId> TakeAdmitted();

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
