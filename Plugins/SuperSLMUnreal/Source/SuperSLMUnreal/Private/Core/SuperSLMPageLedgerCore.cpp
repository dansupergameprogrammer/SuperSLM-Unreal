#include "SuperSLMPageLedgerCore.h"

// The page ledger's bodies. The header is the contract; this file is the bookkeeping behind it.
//
// Shape of the state:
//   - five region counters (capacity, charged), the only figures any verdict reads;
//   - one record per sequence slot: its holder shape, its settled H hold, the one prefix node or
//     cache entry it depends on, and the ticket of its one admitted or waiting job;
//   - one record per prefix node the ledger minted (building, built, released, ghost);
//   - one record per cache entry with a region-A pre-charge (keyed by entry, never by lease);
//   - one record per admitted or waiting job, keyed by ticket. Tickets rise monotonically for the
//     ledger's whole life, so ordered iteration is arrival order, and a ticket from an ended epoch
//     can never name a job of the next one.
//
// Every charge is taken at admission and every refund at apply (plugin paging plan §3.2's charge
// rule), so the charges cover the drawn pages at every instant between the two.

#include <cstddef>
#include <map>

// Every mutant and under-charge site goes through these two macros, so that with the test hooks
// off they collapse to "no mutant" and "the full charge", and the mutation enums (which the header
// declares only with the hooks on) are never named.
#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
#define SUPERSLM_LEDGER_MUTANT(ImplPtr, Name) ((ImplPtr)->Is(ELedgerMutation::Name))
#define SUPERSLM_LEDGER_UNDERCHARGE(ImplPtr, Kind, Pages) ((ImplPtr)->Shrunk(EChargeKind::Kind, (Pages)))
#else
#define SUPERSLM_LEDGER_MUTANT(ImplPtr, Name) false
#define SUPERSLM_LEDGER_UNDERCHARGE(ImplPtr, Kind, Pages) (Pages)
#endif

namespace SuperSLMPaging
{

namespace
{

constexpr uint64_t U32Max = 0xffffffffull;

uint32_t ClampU32(uint64_t Value)
{
	return Value > U32Max ? 0xffffffffu : static_cast<uint32_t>(Value);
}

bool IsGeometry(int64_t PagePositions, int64_t ContextCap)
{
	return PagePositions >= 1 && ContextCap >= 1;
}

// ceil(N / B) for N >= 0 and B >= 1, written so that it cannot overflow near INT64_MAX.
uint64_t CeilDiv(int64_t N, int64_t B)
{
	return static_cast<uint64_t>(N / B) + ((N % B) != 0 ? 1u : 0u);
}

uint64_t PagesPerBlock64(int64_t B, int64_t Cap)
{
	return IsGeometry(B, Cap) ? CeilDiv(Cap, B) : 0;
}

uint64_t Reservation64(int64_t B, int64_t Cap, int64_t Budget)
{
	if (!IsGeometry(B, Cap) || Budget < 1 || Budget > Cap) return 0;
	const uint64_t Pages = CeilDiv(Budget, B) + 1;
	const uint64_t Blk = PagesPerBlock64(B, Cap);
	return Pages < Blk ? Pages : Blk;
}

uint32_t SaturatingSub(uint32_t A, uint32_t B)
{
	return A > B ? A - B : 0u;
}

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
ELimitMutation GLimitMutation = ELimitMutation::None;
#endif

// The limit the pre-checks compare against. Only the test hooks ever move it off Limit.
int64_t EffectiveLimit(int64_t Limit)
{
#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
	if (GLimitMutation == ELimitMutation::OneLate) return Limit + 1;
	if (GLimitMutation == ELimitMutation::OneEarly) return Limit - 1;
#endif
	return Limit;
}

ELimitVerdict LimitRefusal(int64_t Limit, int64_t ContextCap)
{
	return Limit >= ContextCap ? ELimitVerdict::ContextCapExceeded : ELimitVerdict::KvBudgetExceeded;
}

}  // namespace

// ---- Page arithmetic -------------------------------------------------------------------------

uint32_t PagesPerBlock(int64_t PagePositions, int64_t ContextCap)
{
	return ClampU32(PagesPerBlock64(PagePositions, ContextCap));
}

uint32_t ReservationPages(int64_t PagePositions, int64_t ContextCap, int64_t Budget)
{
	return ClampU32(Reservation64(PagePositions, ContextCap, Budget));
}

uint32_t FullPages(int64_t PagePositions, int64_t Length)
{
	if (PagePositions < 1 || Length < 0) return 0;
	return ClampU32(static_cast<uint64_t>(Length / PagePositions));
}

uint32_t OccupiedPages(int64_t PagePositions, int64_t Length)
{
	if (PagePositions < 1 || Length <= 0) return 0;
	return ClampU32(CeilDiv(Length, PagePositions));
}

uint32_t MaterializedPages(int64_t PagePositions, int64_t ContextCap, int64_t Budget, int64_t Origin)
{
	const uint64_t R = Reservation64(PagePositions, ContextCap, Budget);
	if (R == 0 || Origin < 0) return 0;
	const uint64_t Full = static_cast<uint64_t>(Origin / PagePositions);
	const uint64_t Room = PagesPerBlock64(PagePositions, ContextCap) - R;  // R <= P_blk
	return ClampU32(Full < Room ? Full : Room);
}

uint32_t PrefixBuildCharge(int64_t PagePositions, int64_t ContextCap, int64_t Tokens)
{
	if (Tokens < 0 || Tokens > ContextCap) return 0;
	// The engine's budget domain starts at 1, so an empty prefix is begun with budget 1 (§3.3).
	return ReservationPages(PagePositions, ContextCap, Tokens < 1 ? 1 : Tokens);
}

uint32_t NestedSettledPages(int64_t PagePositions, int64_t ParentLength, int64_t ChildTokens)
{
	if (PagePositions < 1 || ParentLength < 0 || ChildTokens < 0) return 0;
	if (ParentLength > INT64_MAX - ChildTokens) return 0;
	// The child owns every page from the parent's last full page on: its own full pages plus the
	// tail page (which, for an empty child of a mid-page parent, is the copied parent tail).
	const uint64_t AllPages = CeilDiv(ParentLength + ChildTokens, PagePositions);
	const uint64_t ParentFull = static_cast<uint64_t>(ParentLength / PagePositions);
	return ClampU32(AllPages - ParentFull);
}

uint32_t RestoreAdmissionCharge(int64_t PagePositions, int64_t ContextCap, ERestorePayload Payload, int64_t Budget,
                                int64_t Origin)
{
	if (Payload == ERestorePayload::WholeReserve) return PagesPerBlock(PagePositions, ContextCap);
	const uint32_t R = ReservationPages(PagePositions, ContextCap, Budget);
	if (R == 0 || Origin < 0 || Origin > ContextCap) return 0;
	// Charged as if it does not share: sharing is decided inside the verb by a byte compare.
	return ClampU32(static_cast<uint64_t>(R) + MaterializedPages(PagePositions, ContextCap, Budget, Origin));
}

// ---- Holder shape and the generation limit ----------------------------------------------------

int64_t HolderLimit(int64_t ContextCap, const FHolderShape& Shape)
{
	if (Shape.Mode == EHolderMode::WholeReserve) return ContextCap;
	if (Shape.Origin < 0 || Shape.Budget < 0) return 0;
	if (Shape.Origin > ContextCap - Shape.Budget) return ContextCap;
	return Shape.Origin + Shape.Budget;
}

ELimitVerdict CheckActivation(int64_t ContextUsed, int64_t PromptTokens, int64_t Limit, int64_t ContextCap)
{
	if (ContextUsed + PromptTokens > EffectiveLimit(Limit)) return LimitRefusal(Limit, ContextCap);
	return ELimitVerdict::Allowed;
}

ELimitVerdict CheckStep(int64_t ContextUsed, bool bFinishOnly, int64_t Limit, int64_t ContextCap)
{
	// E7: an embed needs a free position; a finish-only step embeds nothing, so it runs at the
	// limit itself. A holder resting ready at c therefore emits 1 + (limit - c) tokens.
	const int64_t L = EffectiveLimit(Limit);
	const bool bAllowed = bFinishOnly ? ContextUsed <= L : ContextUsed < L;
	return bAllowed ? ELimitVerdict::Allowed : LimitRefusal(Limit, ContextCap);
}

uint64_t BudgetModeLifecyclePricedBytes(int64_t PagePositions, int64_t ContextCap, int64_t SequenceBudget,
                                        uint64_t PageBytes)
{
	const uint64_t R = Reservation64(PagePositions, ContextCap, SequenceBudget);
	if (R == 0 || PageBytes == 0) return 0;
	if (R > UINT64_MAX / PageBytes) return UINT64_MAX;
	return R * PageBytes;
}

uint32_t CacheRegionPagesFor(uint64_t CacheBudgetBytes, uint64_t BlockBytes, uint32_t PagesPerBlockValue)
{
	if (CacheBudgetBytes == 0 || BlockBytes == 0) return 0;
	const uint64_t Blocks = CacheBudgetBytes / BlockBytes + ((CacheBudgetBytes % BlockBytes) != 0 ? 1u : 0u);
	if (PagesPerBlockValue != 0 && Blocks > U32Max / PagesPerBlockValue) return 0xffffffffu;
	return ClampU32(Blocks * PagesPerBlockValue);
}

// The two block sizes (plugin paging plan §3.2 "Two caps, two block sizes", UP-15 (c)). Every
// charge is priced at the engine's cap Ce, and every admissible input is bounded by the plugin's
// cap Cp, so a block only has to hold the largest charge an admissible request can make:
//   P_blk: the peak of a Cp-token prefix, R_e(Cp) = min(ceil(Cp / B) + 1, ceil(Ce / B));
//   H_blk: the largest restore, b <= Cp and origin <= Cp: R_e(Cp) + floor(Cp / B), capped at
//          ceil(Ce / B).
// With Cp = Ce both are ceil(Ce / B).
uint32_t PrefixBlockPages(int64_t PagePositions, int64_t PluginCap, int64_t EngineCap)
{
	if (!IsGeometry(PagePositions, PluginCap) || EngineCap < PluginCap) return 0;
	return ClampU32(Reservation64(PagePositions, EngineCap, PluginCap));
}

uint32_t HeadroomBlockPages(int64_t PagePositions, int64_t PluginCap, int64_t EngineCap)
{
	if (!IsGeometry(PagePositions, PluginCap) || EngineCap < PluginCap) return 0;
	const uint64_t Peak = Reservation64(PagePositions, EngineCap, PluginCap) + static_cast<uint64_t>(PluginCap / PagePositions);
	const uint64_t Whole = PagesPerBlock64(PagePositions, EngineCap);
	return ClampU32(Peak < Whole ? Peak : Whole);
}

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
void SetLimitMutationForTest(ELimitMutation Mutation)
{
	GLimitMutation = Mutation;
}
#endif

// ---- The ledger's state -------------------------------------------------------------------------

// Named, not anonymous: FPageLedger::FImpl has external linkage, and a member whose type has
// internal linkage is an error (g++'s -Wsubobject-linkage) once this file is #included into a
// unity translation unit, as Unreal's unity build may do.
namespace PageLedgerState
{

enum class EDependency : uint8_t
{
	None,
	Node,
	Entry
};

struct FSlotState
{
	FHolderShape Shape;
	uint32_t Hold = 0;  // the slot's settled H charge: its draw above R(SequenceBudget)
	EDependency Dependency = EDependency::None;
	FNodeId DependsOnNode = InvalidNode;
	FCacheEntryKey DependsOnEntry = 0;
	FTicket Job = InvalidTicket;  // the slot's one admitted or waiting job
};

struct FNodeState
{
	int64_t Length = 0;            // positions, the parent's included for a nested child
	FNodeId Parent = InvalidNode;  // nested children only
	uint32_t Charge = 0;           // P pages: the peak while building, the settled pages after
	uint32_t Dependents = 0;       // applied dependents: adopters, sharing restores, built children
	uint32_t Pins = 0;             // queued adopts, nested builds and hinted restores naming it
	bool bBuilt = false;
	bool bReleased = false;        // ReleasePrefix admitted: the count slot is free
	bool bReleaseApplied = false;  // a ghost while it still has dependents
};

struct FEntryState
{
	uint32_t PreCharge = 0;   // A pages taken at the first dependent's admission
	uint32_t Dependents = 0;  // applied dependents
	uint32_t Pending = 0;     // admitted or waiting adopts and hinted restores
};

enum class EJobKind : uint8_t
{
	PrefixBuild,
	PrefixRelease,
	Adopt,
	Restore,
	Reset
};

struct FJobState
{
	EJobKind Kind = EJobKind::PrefixBuild;
	int32_t Slot = -1;
	FNodeId Node = InvalidNode;    // build: the node built; release: the node; adopt: the target
	FNodeId Parent = InvalidNode;  // nested build: the pinned parent
	bool bEntryTarget = false;     // adopt of a cache entry
	FCacheEntryKey Entry = 0;      // adopt target, or a restore's cache-entry hint
	int64_t EntryLength = 0;
	uint32_t HCharge = 0;          // restore or normalization: its H charge
	bool bWaiting = false;         // restore: waiting on H, not yet charged there
	bool bNormalizes = false;      // reset: create-first normalization
	ERestorePayload Payload = ERestorePayload::Budget;
	int64_t Budget = 0;
	int64_t Origin = 0;
	FNodeId HintNode = InvalidNode;  // a budget restore's pinned prefix hint
	bool bHintGiven = false;         // the request carried any hint (read only by a test mutant)
	bool bRefundedEarly = false;     // test mutant: the release's refund was taken at admission
};

}  // namespace PageLedgerState

using namespace PageLedgerState;

struct FPageLedger::FImpl
{
	FPageLedgerConfig Config;
	bool bConfigured = false;

	uint32_t Capacity[static_cast<int>(ERegion::Count)] = {};
	uint32_t Charged[static_cast<int>(ERegion::Count)] = {};
	uint32_t Pool = 0;
	int64_t Cp = 0;          // the plugin's cap: bounds admissible inputs and the holder limit
	int64_t Ce = 0;          // the engine's cap: every charge is priced at it
	uint32_t WholePages = 0; // ceil(Ce / B): what a whole-reserve holder draws
	uint32_t RSb = 0;        // R(SequenceBudget), at the engine's cap

	std::vector<FSlotState> Slots;
	std::map<FNodeId, FNodeState> Nodes;
	std::map<FCacheEntryKey, FEntryState> Entries;
	std::map<FTicket, FJobState> Jobs;
	std::vector<FTicket> WaitQueue;  // waiting restores, arrival order
	std::vector<FWaitResolution> Resolutions;

	// Never reset, so tickets and nodes from an ended epoch never collide with the next one.
	FTicket NextTicket = 1;
	FNodeId NextNode = 1;
	uint64_t Faults = 0;

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
	ELedgerMutation Mutation = ELedgerMutation::None;
	uint32_t UnderCharge[static_cast<int>(EChargeKind::Count)] = {};
#endif

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
	bool Is(ELedgerMutation M) const { return Mutation == M; }
	uint32_t Shrunk(EChargeKind Kind, uint32_t Pages) const
	{
		return SaturatingSub(Pages, UnderCharge[static_cast<int>(Kind)]);
	}
#endif

	// ---- The two caps ----

	// The cap that bounds an admissible input (budget, origin, prefix length, SequenceBudget).
	int64_t BoundCap() const { return SUPERSLM_LEDGER_MUTANT(this, ValidationAtEngineCap) ? Ce : Cp; }

	// R(b) and E(b, origin), priced as the engine draws them: at its own cap.
	uint32_t Reserve(int64_t Budget) const
	{
		const int64_t Cap = SUPERSLM_LEDGER_MUTANT(this, RPricedAtContextCap) ? Cp : Ce;
		return ReservationPages(Config.PagePositions, Cap, Budget);
	}
	uint32_t Materialized(int64_t Budget, int64_t Origin) const
	{
		const int64_t Cap = SUPERSLM_LEDGER_MUTANT(this, EPricedAtContextCap) ? Cp : Ce;
		return MaterializedPages(Config.PagePositions, Cap, Budget, Origin);
	}
	// A prefix or nested build's peak: R(max(Tokens, 1)) (the engine's budget domain starts at 1).
	uint32_t BuildPeak(int64_t Tokens) const { return Reserve(Tokens < 1 ? 1 : Tokens); }

	// ---- Region counters ----

	uint32_t& ChargedOf(ERegion R) { return Charged[static_cast<int>(R)]; }
	uint32_t ChargedOf(ERegion R) const { return Charged[static_cast<int>(R)]; }
	uint32_t CapacityOf(ERegion R) const { return Capacity[static_cast<int>(R)]; }
	uint32_t FreeOf(ERegion R) const { return SaturatingSub(CapacityOf(R), ChargedOf(R)); }

	void Charge(ERegion R, uint32_t Pages)
	{
		ChargedOf(R) = ClampU32(static_cast<uint64_t>(ChargedOf(R)) + Pages);
	}
	void Refund(ERegion R, uint32_t Pages)
	{
		ChargedOf(R) = SaturatingSub(ChargedOf(R), Pages);
	}

	FTicket MintTicket() { return NextTicket++; }

	bool IsSlot(int32_t Slot) const
	{
		return Slot >= 0 && static_cast<size_t>(Slot) < Slots.size();
	}
	FSlotState& SlotAt(int32_t Slot) { return Slots[static_cast<size_t>(Slot)]; }

	FNodeState* FindNode(FNodeId Id)
	{
		auto It = Nodes.find(Id);
		return It == Nodes.end() ? nullptr : &It->second;
	}
	const FNodeState* FindNode(FNodeId Id) const
	{
		auto It = Nodes.find(Id);
		return It == Nodes.end() ? nullptr : &It->second;
	}
	bool IsLiveBuilt(FNodeId Id) const
	{
		const FNodeState* N = FindNode(Id);
		return N && N->bBuilt && !N->bReleased;
	}

	FJobState* FindJob(FTicket Ticket, EJobKind Kind)
	{
		auto It = Jobs.find(Ticket);
		if (It == Jobs.end() || It->second.Kind != Kind) return nullptr;
		return &It->second;
	}

	// Ends a job: frees its slot for the next admission and forgets the ticket.
	void Retire(FTicket Ticket)
	{
		auto It = Jobs.find(Ticket);
		if (It == Jobs.end()) return;
		if (IsSlot(It->second.Slot) && SlotAt(It->second.Slot).Job == Ticket) SlotAt(It->second.Slot).Job = InvalidTicket;
		Jobs.erase(It);
	}

	uint32_t LiveCount() const
	{
		uint32_t N = 0;
		for (const auto& KV : Nodes) N += KV.second.bReleased ? 0u : 1u;
		return N;
	}

	// ---- Holds ----

	// What a slot holds on H after its materialized pages return (reset in place, adopt): a
	// budget holder keeps only its reservation above R(SequenceBudget); a whole-reserve holder
	// draws P_blk whatever it does, so its hold does not move.
	uint32_t HoldAfterUnmap(const FSlotState& S) const
	{
		if (S.Shape.Mode == EHolderMode::WholeReserve) return S.Hold;
		return SaturatingSub(Reserve(S.Shape.Budget), RSb);
	}

	void SetHold(FSlotState& S, uint32_t NewHold)
	{
		Refund(ERegion::H, S.Hold);
		Charge(ERegion::H, NewHold);
		S.Hold = NewHold;
	}

	// ---- Dependents (the sharer registry, §3.2) ----

	void FreeNode(FNodeId Id)
	{
		// A released node whose last dependent has let go returns its charge, and stops being
		// its parent's dependent; the parent may then go the same way (the cascade).
		while (Id != InvalidNode)
		{
			auto It = Nodes.find(Id);
			if (It == Nodes.end()) return;
			Refund(ERegion::P, It->second.Charge);
			const FNodeId Parent = It->second.Parent;
			Nodes.erase(It);
			Id = InvalidNode;
			FNodeState* P = FindNode(Parent);
			if (!P) return;
			P->Dependents = P->Dependents > 0 ? P->Dependents - 1 : 0;
			if (P->bReleaseApplied && (P->Dependents == 0 || SUPERSLM_LEDGER_MUTANT(this, GhostReleasedAtFirstDependent)))
			{
				Id = Parent;
			}
		}
	}

	void DropNodeDependent(FNodeId Id)
	{
		FNodeState* N = FindNode(Id);
		if (!N) return;
		N->Dependents = N->Dependents > 0 ? N->Dependents - 1 : 0;
		if (N->bReleaseApplied && (N->Dependents == 0 || SUPERSLM_LEDGER_MUTANT(this, GhostReleasedAtFirstDependent)))
		{
			FreeNode(Id);
		}
	}

	// An entry's pre-charge returns once it has neither an applied nor a pending dependent,
	// whether the entry is still cached or not (no eviction hook, §3.2).
	void MaybeRetireEntry(FCacheEntryKey Key)
	{
		auto It = Entries.find(Key);
		if (It == Entries.end()) return;
		if (It->second.Dependents != 0 || It->second.Pending != 0) return;
		Refund(ERegion::A, It->second.PreCharge);
		Entries.erase(It);
	}

	void DropEntryDependent(FCacheEntryKey Key)
	{
		auto It = Entries.find(Key);
		if (It == Entries.end()) return;
		It->second.Dependents = It->second.Dependents > 0 ? It->second.Dependents - 1 : 0;
		MaybeRetireEntry(Key);
	}

	void DropSlotDependency(FSlotState& S)
	{
		const EDependency D = S.Dependency;
		const FNodeId Node = S.DependsOnNode;
		const FCacheEntryKey Entry = S.DependsOnEntry;
		S.Dependency = EDependency::None;
		S.DependsOnNode = InvalidNode;
		S.DependsOnEntry = 0;
		if (D == EDependency::Node) DropNodeDependent(Node);
		if (D == EDependency::Entry) DropEntryDependent(Entry);
	}

	// Registers the slot on a new target and drops its previous dependency, in that order, so a
	// re-adopt of the same target never passes through zero dependents.
	void Redepend(FSlotState& S, EDependency D, FNodeId Node, FCacheEntryKey Entry, bool bDropPrevious)
	{
		if (D == EDependency::Node)
		{
			if (FNodeState* N = FindNode(Node)) N->Dependents += 1;
		}
		if (D == EDependency::Entry)
		{
			auto It = Entries.find(Entry);
			if (It != Entries.end()) It->second.Dependents += 1;
		}
		if (bDropPrevious) DropSlotDependency(S);
		S.Dependency = D;
		S.DependsOnNode = D == EDependency::Node ? Node : InvalidNode;
		S.DependsOnEntry = D == EDependency::Entry ? Entry : 0;
	}

	// ---- Region A pre-charge ----

	// The pages a first dependent of Key would pre-charge (0 when the entry already has a live
	// or pending dependent: further adopters add nothing).
	uint32_t PreChargeNeeded(FCacheEntryKey Key, int64_t Length) const
	{
		if (Entries.count(Key) != 0) return 0;
		if (SUPERSLM_LEDGER_MUTANT(this, PreChargeSkipped)) return 0;
		return SUPERSLM_LEDGER_UNDERCHARGE(this, CachePreCharge, FullPages(Config.PagePositions, Length));
	}

	// Takes the pre-charge (when this is the first dependent) and registers one pending dependent.
	void AddPendingEntry(FCacheEntryKey Key, uint32_t PreCharge)
	{
		auto It = Entries.find(Key);
		if (It == Entries.end())
		{
			FEntryState E;
			E.PreCharge = PreCharge;
			It = Entries.emplace(Key, E).first;
			Charge(ERegion::A, PreCharge);
		}
		It->second.Pending += 1;
	}

	void DropPendingEntry(FCacheEntryKey Key)
	{
		auto It = Entries.find(Key);
		if (It == Entries.end()) return;
		It->second.Pending = It->second.Pending > 0 ? It->second.Pending - 1 : 0;
	}

	// ---- The wait rule (§3.5) ----

	// What an admitted restore is certain to return to H at its apply: its old holder's
	// reservation, capped by its own charge (it cannot return more than it holds). A share is
	// never counted.
	uint32_t Guaranteed(const FJobState& J) const
	{
		// Nested, not "MUTANT && cond": with the test hooks off the macro is the literal false, and MSVC's /W4 raises
		// C4127 on "false && cond" (it exempts a bare literal condition).
		if (SUPERSLM_LEDGER_MUTANT(this, ShareCountedAsGuaranteed))
		{
			if (J.bHintGiven) return J.HCharge;
		}
		if (SUPERSLM_LEDGER_MUTANT(this, FlatGuarantee)) return RSb;
		return J.HCharge < RSb ? J.HCharge : RSb;
	}

	bool AnyWaiting() const { return !WaitQueue.empty(); }

	// free(H) + sum(guaranteed of the restores ahead) - sum(charges of the waiters ahead) >= c.
	// Every restore job in the table arrived before this one.
	bool WaitIsGuaranteed(uint32_t HCharge) const
	{
		int64_t Promise = FreeOf(ERegion::H);
		for (const auto& KV : Jobs)
		{
			const FJobState& J = KV.second;
			if (J.Kind != EJobKind::Restore) continue;
			Promise += Guaranteed(J);
			if (J.bWaiting) Promise -= J.HCharge;
		}
		return Promise >= static_cast<int64_t>(HCharge);
	}

	// Admits waiters in arrival order while the head fits. Called after every H refund.
	void AdmitWaiters()
	{
		size_t Taken = 0;
		while (Taken < WaitQueue.size())
		{
			FJobState* J = FindJob(WaitQueue[Taken], EJobKind::Restore);
			if (J == nullptr || !J->bWaiting)
			{
				++Taken;  // not expected; skip a stale entry rather than stall the queue
				continue;
			}
			if (J->HCharge > FreeOf(ERegion::H)) break;
			Charge(ERegion::H, J->HCharge);
			J->bWaiting = false;
			FWaitResolution R;
			R.Ticket = WaitQueue[Taken];
			R.Outcome = EPageAdmit::Admitted;
			Resolutions.push_back(R);
			++Taken;
		}
		WaitQueue.erase(WaitQueue.begin(), WaitQueue.begin() + static_cast<std::ptrdiff_t>(Taken));
	}

	// ---- Epoch ----

	void Clear()
	{
		bConfigured = false;
		for (int I = 0; I < static_cast<int>(ERegion::Count); ++I)
		{
			Capacity[I] = 0;
			Charged[I] = 0;
		}
		Pool = 0;
		Cp = 0;
		Ce = 0;
		WholePages = 0;
		RSb = 0;
		Slots.clear();
		Nodes.clear();
		Entries.clear();
		Jobs.clear();
		WaitQueue.clear();
	}
};

FPageLedger::FPageLedger() : Impl(new FImpl()) {}

FPageLedger::~FPageLedger()
{
	delete Impl;
}

// ---- Epoch ------------------------------------------------------------------------------------

ELedgerConfigure FPageLedger::Configure(const FPageLedgerConfig& Config)
{
	if (Impl->bConfigured) EndEpoch(EEpochEnd::Reconfigure);
	Impl->Config = Config;

	if (Config.SequenceBudget == 0) return ELedgerConfigure::LegacyMode;
	if (Config.SequenceBudget < 0) return ELedgerConfigure::InvalidSequenceBudget;
	// The engine's cap: 0 means "equal to ContextCap" (the CPU, where the two are one). A model
	// whose cap is below the game's ContextCap cannot serve it, so that is a geometry refusal.
	const int64_t Cp = Config.ContextCap;
	const int64_t Ce = Config.EngineContextCap == 0 ? Cp : Config.EngineContextCap;
	if (!IsGeometry(Config.PagePositions, Cp) || Ce < Cp || Config.PageBytes == 0 || Config.BlockCount < 0 ||
	    Config.PrefixBlockCount < 0 || Config.RestoreHeadroomBlocks < 0 || Config.CacheGhostBlocks < 0)
	{
		return ELedgerConfigure::InvalidGeometry;
	}
	Impl->Cp = Cp;
	Impl->Ce = Ce;
	// SequenceBudget is an admissible input, so the plugin's cap bounds it (never the engine's).
	if (Config.SequenceBudget > Impl->BoundCap()) return ELedgerConfigure::InvalidSequenceBudget;

	// UP-15 (c): the P/A block and the H block, each the largest charge an admissible request can
	// make. The test hooks can swap in option (b)'s or option (a)'s blocks.
	uint64_t PBlk = PrefixBlockPages(Config.PagePositions, Cp, Ce);
	uint64_t HBlk = HeadroomBlockPages(Config.PagePositions, Cp, Ce);
	if (SUPERSLM_LEDGER_MUTANT(Impl, BlocksAtContextCap)) PBlk = HBlk = PagesPerBlock64(Config.PagePositions, Cp);
	if (SUPERSLM_LEDGER_MUTANT(Impl, BlocksAtEngineCap)) PBlk = HBlk = PagesPerBlock64(Config.PagePositions, Ce);
	const uint64_t Whole = PagesPerBlock64(Config.PagePositions, Ce);
	const uint64_t RSb = Impl->Reserve(Config.SequenceBudget);
	if (Whole > U32Max) return ELedgerConfigure::InvalidGeometry;  // PBlk, HBlk and R are all <= Whole

	// Every factor is below 2^32, so each product fits in 64 bits.
	const uint64_t S = static_cast<uint64_t>(Config.BlockCount) * RSb;
	const uint64_t P = static_cast<uint64_t>(Config.PrefixBlockCount) * PBlk;
	const uint64_t H = static_cast<uint64_t>(Config.RestoreHeadroomBlocks) * HBlk;
	const uint64_t A = Config.bCacheConfigured ? static_cast<uint64_t>(Config.CacheGhostBlocks) * PBlk : 0;
	const uint64_t C = Config.bCacheConfigured ? Config.CacheRegionPages : 0;
	const uint64_t Pool = S + P + H + A + C;
	if (Pool > U32Max) return ELedgerConfigure::InvalidGeometry;
	// Region C is spoken to in bytes; its bytes must be representable.
	if (C != 0 && C > UINT64_MAX / Config.PageBytes) return ELedgerConfigure::InvalidGeometry;

	Impl->WholePages = static_cast<uint32_t>(Whole);
	Impl->RSb = static_cast<uint32_t>(RSb);
	Impl->Capacity[static_cast<int>(ERegion::S)] = static_cast<uint32_t>(S);
	Impl->Capacity[static_cast<int>(ERegion::P)] = static_cast<uint32_t>(P);
	Impl->Capacity[static_cast<int>(ERegion::H)] = static_cast<uint32_t>(H);
	Impl->Capacity[static_cast<int>(ERegion::A)] = static_cast<uint32_t>(A);
	Impl->Capacity[static_cast<int>(ERegion::C)] = static_cast<uint32_t>(C);
	Impl->Pool = static_cast<uint32_t>(Pool);

	// The warm sequences: every slot a normalized budget holder, S charged in full.
	FSlotState Fresh;
	Fresh.Shape.Mode = EHolderMode::Budget;
	Fresh.Shape.Budget = Config.SequenceBudget;
	Fresh.Shape.Origin = 0;
	Impl->Slots.assign(static_cast<size_t>(Config.BlockCount), Fresh);
	Impl->Charged[static_cast<int>(ERegion::S)] = static_cast<uint32_t>(S);
	Impl->bConfigured = true;
	return ELedgerConfigure::Ok;
}

void FPageLedger::EndEpoch(EEpochEnd Reason)
{
	(void)Reason;  // every lifecycle exit ends the epoch the same way (§0 "Waits")
	if (!Impl->bConfigured) return;
	// A waiter admitted but not yet drained never had its job enqueued: it ends unserved too.
	for (FWaitResolution& R : Impl->Resolutions)
	{
		if (R.Outcome == EPageAdmit::Admitted) R.Outcome = EPageAdmit::NotConfigured;
	}
	if (!SUPERSLM_LEDGER_MUTANT(Impl, WaitersSurviveEpochEnd))
	{
		for (FTicket T : Impl->WaitQueue)
		{
			FWaitResolution R;
			R.Ticket = T;
			R.Outcome = EPageAdmit::NotConfigured;
			Impl->Resolutions.push_back(R);
		}
	}
	// Every charge goes with the pool; LedgerFaults and the ticket counter are kept.
	Impl->Clear();
}

bool FPageLedger::IsConfigured() const
{
	return Impl->bConfigured;
}

const FPageLedgerConfig& FPageLedger::GetConfig() const
{
	return Impl->Config;
}

// ---- Regions ------------------------------------------------------------------------------------

uint32_t FPageLedger::PoolPages() const
{
	return Impl->Pool;
}

uint32_t FPageLedger::RegionCapacity(ERegion Region) const
{
	if (Region >= ERegion::Count) return 0;
	return Impl->CapacityOf(Region);
}

uint32_t FPageLedger::RegionCharged(ERegion Region) const
{
	if (Region >= ERegion::Count) return 0;
	return Impl->ChargedOf(Region);
}

uint32_t FPageLedger::RegionFree(ERegion Region) const
{
	if (Region >= ERegion::Count) return 0;
	return Impl->FreeOf(Region);
}

uint32_t FPageLedger::TotalCharged() const
{
	uint64_t Sum = 0;
	for (int I = 0; I < static_cast<int>(ERegion::Count); ++I) Sum += Impl->Charged[I];
	return ClampU32(Sum);
}

FPageLedgerStats FPageLedger::GetStats() const
{
	FPageLedgerStats St;
	St.bConfigured = Impl->bConfigured;
	St.PoolPages = Impl->Pool;
	for (int I = 0; I < static_cast<int>(ERegion::Count); ++I)
	{
		St.Regions[I].Capacity = Impl->Capacity[I];
		St.Regions[I].Charged = Impl->Charged[I];
	}
	St.LivePrefixCount = Impl->LiveCount();
	St.GhostCount = GhostCount();
	for (const auto& KV : Impl->Jobs)
	{
		if (KV.second.Kind != EJobKind::Restore) continue;
		if (KV.second.bWaiting) ++St.WaitingRestores;
		else ++St.InFlightRestores;
	}
	St.FreeHeadroomPages = Impl->FreeOf(ERegion::H);
	St.LedgerFaults = Impl->Faults;
	return St;
}

std::vector<FWaitResolution> FPageLedger::DrainResolutions()
{
	std::vector<FWaitResolution> Out;
	Out.swap(Impl->Resolutions);
	return Out;
}

// ---- Prefixes -----------------------------------------------------------------------------------

FAdmission FPageLedger::AdmitPrefixBuild(int64_t Tokens)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	const FPageLedgerConfig& C = Impl->Config;
	if (Tokens < 0 || Tokens > Impl->BoundCap())
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	if (Impl->LiveCount() >= static_cast<uint32_t>(C.PrefixBlockCount))
	{
		Adm.Outcome = EPageAdmit::PrefixLimit;
		return Adm;
	}
	const uint32_t Charge = SUPERSLM_LEDGER_UNDERCHARGE(Impl, PrefixBuild, Impl->BuildPeak(Tokens));
	if (Charge > Impl->FreeOf(ERegion::P))
	{
		Adm.Outcome = EPageAdmit::KvPagesExhausted;
		return Adm;
	}
	FNodeState N;
	N.Length = Tokens;
	N.Charge = Charge;
	const FNodeId Id = Impl->NextNode++;
	Impl->Nodes.emplace(Id, N);
	Impl->Charge(ERegion::P, Charge);

	FJobState J;
	J.Kind = EJobKind::PrefixBuild;
	J.Node = Id;
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);

	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.Node = Id;
	Adm.ChargedPages = Charge;
	return Adm;
}

FAdmission FPageLedger::AdmitNestedPrefixBuild(FNodeId Parent, int64_t ChildTokens)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	const FPageLedgerConfig& C = Impl->Config;
	if (!Impl->IsLiveBuilt(Parent))
	{
		Adm.Outcome = EPageAdmit::ParentNotUsable;
		return Adm;
	}
	const int64_t ParentLength = Impl->FindNode(Parent)->Length;
	if (ChildTokens < 0 || ChildTokens > Impl->BoundCap() - ParentLength)
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	if (Impl->LiveCount() >= static_cast<uint32_t>(C.PrefixBlockCount))
	{
		Adm.Outcome = EPageAdmit::PrefixLimit;
		return Adm;
	}
	const uint32_t Charge =
	    SUPERSLM_LEDGER_UNDERCHARGE(Impl, NestedBuild, Impl->BuildPeak(ChildTokens));
	if (Charge > Impl->FreeOf(ERegion::P))
	{
		Adm.Outcome = EPageAdmit::KvPagesExhausted;
		return Adm;
	}
	FNodeState N;
	N.Length = ParentLength + ChildTokens;
	N.Parent = Parent;
	N.Charge = Charge;
	const FNodeId Id = Impl->NextNode++;
	Impl->Nodes.emplace(Id, N);
	Impl->Charge(ERegion::P, Charge);
	Impl->FindNode(Parent)->Pins += 1;  // pinned from admission until begin_from applies

	FJobState J;
	J.Kind = EJobKind::PrefixBuild;
	J.Node = Id;
	J.Parent = Parent;
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);

	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.Node = Id;
	Adm.ChargedPages = Charge;
	return Adm;
}

bool FPageLedger::ApplyPrefixBuild(FTicket Ticket, bool bSucceeded)
{
	FJobState* J = Impl->FindJob(Ticket, EJobKind::PrefixBuild);
	if (J == nullptr) return false;
	const FNodeId Id = J->Node;
	const FNodeId Parent = J->Parent;
	Impl->Retire(Ticket);

	FNodeState* P = Impl->FindNode(Parent);
	if (P && P->Pins > 0) P->Pins -= 1;
	FNodeState* N = Impl->FindNode(Id);
	if (N == nullptr) return true;

	if (!bSucceeded)
	{
		// The whole charge returns; the count slot and the node go with it.
		Impl->Refund(ERegion::P, N->Charge);
		Impl->Nodes.erase(Id);
		return true;
	}
	// Settle to the node's own pages at freeze: a root holds ceil(L/B); a child holds its pages
	// past the parent's full pages, and becomes its parent's dependent.
	const int64_t B = Impl->Config.PagePositions;
	uint32_t Settled = OccupiedPages(B, N->Length);
	if (P)
	{
		Settled = NestedSettledPages(B, P->Length, N->Length - P->Length);
		if (SUPERSLM_LEDGER_MUTANT(Impl, EmptyChildSettledToZero)) // nested for MSVC C4127, as in Guaranteed()
		{
			if (N->Length == P->Length) Settled = 0;
		}
		P->Dependents += 1;
	}
	Impl->Refund(ERegion::P, N->Charge);
	Impl->Charge(ERegion::P, Settled);
	N->Charge = Settled;
	N->bBuilt = true;
	return true;
}

FAdmission FPageLedger::AdmitPrefixRelease(FNodeId Node)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	FNodeState* N = Impl->FindNode(Node);
	if (N == nullptr || N->bReleased)
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	// A queued job naming the node pins it; so does its own build until that applies.
	if (N->Pins > 0 || !N->bBuilt)
	{
		Adm.Outcome = EPageAdmit::PrefixPinned;
		return Adm;
	}
	N->bReleased = true;  // the count slot is free from here

	FJobState J;
	J.Kind = EJobKind::PrefixRelease;
	J.Node = Node;
	if (SUPERSLM_LEDGER_MUTANT(Impl, RefundAtEnqueue)) // nested for MSVC C4127, as in Guaranteed()
	{
		if (N->Dependents == 0)
		{
			Impl->Refund(ERegion::P, N->Charge);
			N->Charge = 0;
			J.bRefundedEarly = true;
		}
	}
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);
	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.Node = Node;
	return Adm;
}

bool FPageLedger::ApplyPrefixRelease(FTicket Ticket)
{
	FJobState* J = Impl->FindJob(Ticket, EJobKind::PrefixRelease);
	if (J == nullptr) return false;
	const FNodeId Id = J->Node;
	Impl->Retire(Ticket);
	FNodeState* N = Impl->FindNode(Id);
	if (N == nullptr) return true;
	N->bReleaseApplied = true;
	// No dependent: the pages return now. Otherwise the node stays charged as a ghost until its
	// last dependent's job applies.
	if (N->Dependents == 0) Impl->FreeNode(Id);
	return true;
}

bool FPageLedger::IsPrefixLive(FNodeId Node) const
{
	const FNodeState* N = Impl->FindNode(Node);
	return N != nullptr && !N->bReleased;
}

bool FPageLedger::IsGhost(FNodeId Node) const
{
	const FNodeState* N = Impl->FindNode(Node);
	return N != nullptr && N->bReleaseApplied;
}

uint32_t FPageLedger::NodeChargedPages(FNodeId Node) const
{
	const FNodeState* N = Impl->FindNode(Node);
	return N ? N->Charge : 0;
}

uint32_t FPageLedger::DependentCount(FNodeId Node) const
{
	const FNodeState* N = Impl->FindNode(Node);
	return N ? N->Dependents : 0;
}

uint32_t FPageLedger::LivePrefixCount() const
{
	return Impl->LiveCount();
}

uint32_t FPageLedger::GhostCount() const
{
	uint32_t N = 0;
	for (const auto& KV : Impl->Nodes) N += KV.second.bReleaseApplied ? 1u : 0u;
	return N;
}

// ---- Adopt --------------------------------------------------------------------------------------

FAdmission FPageLedger::AdmitAdoptPrefix(int32_t Slot, FNodeId Node)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	if (!Impl->IsSlot(Slot) || Impl->SlotAt(Slot).Job != InvalidTicket || !Impl->IsLiveBuilt(Node))
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	if (Impl->SlotAt(Slot).Shape.Mode == EHolderMode::WholeReserve)
	{
		Adm.Outcome = EPageAdmit::KvModeMismatch;  // "reset first"
		return Adm;
	}
	Impl->FindNode(Node)->Pins += 1;
	FJobState J;
	J.Kind = EJobKind::Adopt;
	J.Slot = Slot;
	J.Node = Node;
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);
	Impl->SlotAt(Slot).Job = T;
	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	return Adm;
}

FAdmission FPageLedger::AdmitAdoptCacheEntry(int32_t Slot, FCacheEntryKey Entry, int64_t EntryLength)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	if (!Impl->IsSlot(Slot) || Impl->SlotAt(Slot).Job != InvalidTicket || Entry == 0 || EntryLength < 0 ||
	    EntryLength > Impl->BoundCap())
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	if (Impl->SlotAt(Slot).Shape.Mode == EHolderMode::WholeReserve)
	{
		Adm.Outcome = EPageAdmit::KvModeMismatch;
		return Adm;
	}
	// Charge before sharing: the first dependent pre-charges the entry's full pages to A.
	const uint32_t PreCharge = Impl->PreChargeNeeded(Entry, EntryLength);
	if (PreCharge > Impl->FreeOf(ERegion::A))
	{
		Adm.Outcome = EPageAdmit::KvPagesExhausted;  // UP-14: the sequence is untouched
		return Adm;
	}
	Impl->AddPendingEntry(Entry, PreCharge);
	FJobState J;
	J.Kind = EJobKind::Adopt;
	J.Slot = Slot;
	J.bEntryTarget = true;
	J.Entry = Entry;
	J.EntryLength = EntryLength;
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);
	Impl->SlotAt(Slot).Job = T;
	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.ChargedPages = PreCharge;
	return Adm;
}

bool FPageLedger::ApplyAdopt(FTicket Ticket, bool bSucceeded)
{
	FJobState* Jp = Impl->FindJob(Ticket, EJobKind::Adopt);
	if (Jp == nullptr) return false;
	const FJobState J = *Jp;
	Impl->Retire(Ticket);

	if (J.bEntryTarget) Impl->DropPendingEntry(J.Entry);
	else if (FNodeState* N = Impl->FindNode(J.Node))
	{
		if (N->Pins > 0) N->Pins -= 1;
	}

	if (bSucceeded && Impl->IsSlot(J.Slot))
	{
		FSlotState& S = Impl->SlotAt(J.Slot);
		const bool bDrop = !SUPERSLM_LEDGER_MUTANT(Impl, DependencyKeptAtReadopt);
		if (J.bEntryTarget)
		{
			Impl->Redepend(S, EDependency::Entry, InvalidNode, J.Entry, bDrop);
			S.Shape.Origin = J.EntryLength;
		}
		else
		{
			const FNodeState* N = Impl->FindNode(J.Node);
			Impl->Redepend(S, EDependency::Node, J.Node, 0, bDrop);
			if (N) S.Shape.Origin = N->Length;
		}
		// The adopt unmaps first: the holder's materialized pages return. Its own budget is
		// kept (the engine's adopt moves only the origin).
		Impl->SetHold(S, Impl->HoldAfterUnmap(S));
	}
	// A pre-charge with no dependent left (a lone failed first adopt) returns here.
	if (J.bEntryTarget) Impl->MaybeRetireEntry(J.Entry);
	Impl->AdmitWaiters();
	return true;
}

uint32_t FPageLedger::CacheEntryDependents(FCacheEntryKey Entry) const
{
	auto It = Impl->Entries.find(Entry);
	return It == Impl->Entries.end() ? 0 : It->second.Dependents;
}

bool FPageLedger::IsCacheEntryPreCharged(FCacheEntryKey Entry) const
{
	return Impl->Entries.count(Entry) != 0;
}

// ---- Restore ------------------------------------------------------------------------------------

FAdmission FPageLedger::AdmitRestore(const FRestoreRequest& Request)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	if (!Impl->IsSlot(Request.Slot) || Impl->SlotAt(Request.Slot).Job != InvalidTicket)
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	const bool bBudget = Request.Payload == ERestorePayload::Budget;
	// A budget outside [1, Ce] or an origin outside [0, Ce] is malformed: no engine could have taken
	// that save. Otherwise a budget or origin above the plugin's cap is a valid save taken under a
	// larger ContextCap. Both are refused before any charge, and the malformed outcome wins. The
	// charge itself is the engine's draw: R(b) + E at its own cap, or ceil(Ce / B) for a whole-reserve
	// payload.
	if (bBudget)
	{
		const int64_t MinBudget = SUPERSLM_LEDGER_MUTANT(Impl, BudgetDomainFromZero) ? 0 : 1;
		if (Request.Budget < MinBudget || Request.Budget > Impl->Ce || Request.Origin < 0 || Request.Origin > Impl->Ce)
		{
			Adm.Outcome = EPageAdmit::InvalidRequest;
			return Adm;
		}
		if (Request.Budget > Impl->BoundCap() || Request.Origin > Impl->BoundCap())
		{
			Adm.Outcome = SUPERSLM_LEDGER_MUTANT(Impl, ContextCapMismatchReportedAsInvalid) ? EPageAdmit::InvalidRequest
			                                                                                 : EPageAdmit::ContextCapMismatch;
			return Adm;
		}
	}
	uint32_t HCharge = bBudget ? ClampU32(static_cast<uint64_t>(Impl->Reserve(Request.Budget)) +
	                                      Impl->Materialized(Request.Budget, Request.Origin))
	                           : Impl->WholePages;
	HCharge = (bBudget ? SUPERSLM_LEDGER_UNDERCHARGE(Impl, BudgetRestore, HCharge)
	                  : SUPERSLM_LEDGER_UNDERCHARGE(Impl, WholeReserveRestore, HCharge));

	// The hint. The engine ignores a hint on a whole-reserve payload, and so does the ledger.
	const bool bPrefixHint = bBudget && Request.Hint == EHintKind::Prefix;
	const bool bEntryHint = bBudget && Request.Hint == EHintKind::CacheEntry;
	if (bPrefixHint && !Impl->IsLiveBuilt(Request.HintNode))
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	if (bEntryHint && (Request.HintEntry == 0 || Request.HintEntryLength < 0 || Request.HintEntryLength > Impl->BoundCap()))
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	// A cache-entry hint may share, so it pre-charges like a first adopt, at arrival.
	const uint32_t PreCharge = bEntryHint ? Impl->PreChargeNeeded(Request.HintEntry, Request.HintEntryLength) : 0;
	if (PreCharge > Impl->FreeOf(ERegion::A))
	{
		Adm.Outcome = EPageAdmit::KvPagesExhausted;
		return Adm;
	}

	// Fit, wait, or refuse (§3.5), in arrival order.
	bool bWait = false;
	if (Impl->AnyWaiting() || HCharge > Impl->FreeOf(ERegion::H))
	{
		if (!SUPERSLM_LEDGER_MUTANT(Impl, WaiterAdmittedWithoutGuarantee) && !Impl->WaitIsGuaranteed(HCharge))
		{
			Adm.Outcome = EPageAdmit::KvPagesExhausted;  // on arrival: the old sequence is untouched
			return Adm;
		}
		bWait = true;
	}

	FJobState J;
	J.Kind = EJobKind::Restore;
	J.Slot = Request.Slot;
	J.Payload = Request.Payload;
	J.Budget = Request.Budget;
	J.Origin = Request.Origin;
	J.HCharge = HCharge;
	J.bWaiting = bWait;
	J.bHintGiven = Request.Hint != EHintKind::None;
	if (bPrefixHint)
	{
		J.HintNode = Request.HintNode;
		Impl->FindNode(Request.HintNode)->Pins += 1;  // pinned while queued or waiting
	}
	if (bEntryHint)
	{
		J.Entry = Request.HintEntry;
		J.EntryLength = Request.HintEntryLength;
		Impl->AddPendingEntry(Request.HintEntry, PreCharge);
	}
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);
	Impl->SlotAt(Request.Slot).Job = T;
	if (bWait)
	{
		Impl->WaitQueue.push_back(T);
	}
	else
	{
		Impl->Charge(ERegion::H, HCharge);
	}
	Adm.Outcome = bWait ? EPageAdmit::Waiting : EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.ChargedPages = PreCharge + (bWait ? 0u : HCharge);
	return Adm;
}

bool FPageLedger::ApplyRestore(FTicket Ticket, bool bSucceeded, uint32_t SharedPages)
{
	FJobState* Jp = Impl->FindJob(Ticket, EJobKind::Restore);
	if (Jp == nullptr || Jp->bWaiting) return false;  // a waiting restore has no job to apply
	const FJobState J = *Jp;
	Impl->Retire(Ticket);

	if (J.HintNode != InvalidNode)
	{
		if (FNodeState* N = Impl->FindNode(J.HintNode))
		{
			if (N->Pins > 0) N->Pins -= 1;
		}
	}
	if (J.Entry != 0) Impl->DropPendingEntry(J.Entry);

	const FPageLedgerConfig& C = Impl->Config;
	if (bSucceeded && Impl->IsSlot(J.Slot))
	{
		FSlotState& S = Impl->SlotAt(J.Slot);
		const bool bWhole = J.Payload == ERestorePayload::WholeReserve;
		const bool bShared = !bWhole && SharedPages > 0 && (J.HintNode != InvalidNode || J.Entry != 0);
		const uint32_t R = Impl->Reserve(J.Budget);
		uint32_t Draw = Impl->WholePages;
		if (!bWhole) Draw = bShared ? R : R + Impl->Materialized(J.Budget, J.Origin);
		uint32_t NewHold = SaturatingSub(Draw, Impl->RSb);
		if (!bShared && SUPERSLM_LEDGER_MUTANT(Impl, PrivateRestoreRefundedAtApply)) NewHold = 0;
		if (!bShared && SUPERSLM_LEDGER_MUTANT(Impl, HoldSettledOnePageSmaller)) NewHold = SaturatingSub(NewHold, 1);

		// The admission charge and the old holder's hold both return; the new holder's hold is
		// charged. The old holder was released (restore-first), so its dependency goes too.
		Impl->Refund(ERegion::H, J.HCharge);
		Impl->SetHold(S, NewHold);
		const bool bRegister = bShared && !SUPERSLM_LEDGER_MUTANT(Impl, RestoredSharerNotRegistered);
		if (bRegister && J.HintNode != InvalidNode)
		{
			Impl->Redepend(S, EDependency::Node, J.HintNode, 0, true);
		}
		else if (bRegister && J.Entry != 0)
		{
			Impl->Redepend(S, EDependency::Entry, InvalidNode, J.Entry, true);
		}
		else
		{
			Impl->DropSlotDependency(S);
		}
		S.Shape.Mode = bWhole ? EHolderMode::WholeReserve : EHolderMode::Budget;
		S.Shape.Budget = bWhole ? C.ContextCap : J.Budget;
		S.Shape.Origin = bWhole ? 0 : J.Origin;
	}
	else if (!SUPERSLM_LEDGER_MUTANT(Impl, FailedRestoreKeepsCharge))
	{
		// A refused or failed restore leaves the old sequence as it was and returns its charge.
		Impl->Refund(ERegion::H, J.HCharge);
	}
	if (J.Entry != 0) Impl->MaybeRetireEntry(J.Entry);
	Impl->AdmitWaiters();
	return true;
}

// ---- Reset and create-first normalization -------------------------------------------------------

FAdmission FPageLedger::AdmitReset(int32_t Slot)
{
	FAdmission Adm;
	if (!Impl->bConfigured) return Adm;
	if (!Impl->IsSlot(Slot) || Impl->SlotAt(Slot).Job != InvalidTicket)
	{
		Adm.Outcome = EPageAdmit::InvalidRequest;
		return Adm;
	}
	FSlotState& S = Impl->SlotAt(Slot);
	const bool bOffShape = S.Shape.Mode != EHolderMode::Budget || S.Shape.Budget != Impl->Config.SequenceBudget;
	// A normalization creates the new holder before releasing the old one, so it needs
	// R(SequenceBudget) of H. It yields to waiting restores: it takes no H while one waits.
	const bool bYield = Impl->AnyWaiting() && !SUPERSLM_LEDGER_MUTANT(Impl, NormalizationTakesHeadroomWhileWaiting);
	const bool bNormalize = bOffShape && !bYield && Impl->FreeOf(ERegion::H) >= Impl->RSb;

	FJobState J;
	J.Kind = EJobKind::Reset;
	J.Slot = Slot;
	J.bNormalizes = bNormalize;
	if (bNormalize)
	{
		J.HCharge = SUPERSLM_LEDGER_MUTANT(Impl, NormalizationUncharged) ? 0u
		                                                              : SUPERSLM_LEDGER_UNDERCHARGE(Impl, Normalization, Impl->RSb);
		Impl->Charge(ERegion::H, J.HCharge);
	}
	if (SUPERSLM_LEDGER_MUTANT(Impl, RefundAtEnqueue))
	{
		// The mutant takes the reset's refunds before the job has freed anything.
		Impl->DropSlotDependency(S);
		Impl->SetHold(S, bNormalize ? 0u : Impl->HoldAfterUnmap(S));
		Impl->AdmitWaiters();
	}
	const FTicket T = Impl->MintTicket();
	Impl->Jobs.emplace(T, J);
	S.Job = T;
	Adm.Outcome = EPageAdmit::Admitted;
	Adm.Ticket = T;
	Adm.ChargedPages = J.HCharge;
	Adm.bNormalizes = bNormalize;
	return Adm;
}

bool FPageLedger::ApplyReset(FTicket Ticket, EResetResult Result)
{
	FJobState* Jp = Impl->FindJob(Ticket, EJobKind::Reset);
	if (Jp == nullptr) return false;
	// Only a normalization can report Normalized; an in-place job that claims it is refused whole.
	if (Result == EResetResult::Normalized && !Jp->bNormalizes) return false;
	const FJobState J = *Jp;
	Impl->Retire(Ticket);
	if (!Impl->IsSlot(J.Slot)) return true;
	FSlotState& S = Impl->SlotAt(J.Slot);

	// A normalization's create charge returns whatever the job did: its new holder either replaced
	// the old one (and the slot's reservation in S now covers it) or was never drawn.
	if (J.bNormalizes) Impl->Refund(ERegion::H, J.HCharge);

	switch (Result)
	{
	case EResetResult::Normalized:
		// The new budgeted holder replaced the old one: the old holder's whole hold returns.
		Impl->DropSlotDependency(S);
		Impl->SetHold(S, 0);
		S.Shape.Mode = EHolderMode::Budget;
		S.Shape.Budget = Impl->Config.SequenceBudget;
		S.Shape.Origin = 0;
		break;
	case EResetResult::ResetInPlace:
		// The holder was reset as it is (for a normalization: its create was refused, so the old
		// holder is kept and normalization is retried at its next reset). Shared references go
		// and a budget holder's materialized pages return.
		Impl->DropSlotDependency(S);
		Impl->SetHold(S, Impl->HoldAfterUnmap(S));
		S.Shape.Origin = 0;
		break;
	case EResetResult::Refused:
		// Nothing ran, so nothing was freed and nothing else returns.
		break;
	}
	Impl->AdmitWaiters();
	return true;
}

int64_t FPageLedger::SlotLimit(int32_t Slot) const
{
	if (!Impl->bConfigured || !Impl->IsSlot(Slot)) return 0;
	// The holder limit is the plugin's: a holder whose origin + budget runs past ContextCap stops at
	// ContextCap with the cap outcome, even when the engine's cap is larger (§3.4 "Two caps").
	const int64_t Cap = SUPERSLM_LEDGER_MUTANT(Impl, LimitAtEngineCap) ? Impl->Ce : Impl->Cp;
	return HolderLimit(Cap, Impl->Slots[static_cast<size_t>(Slot)].Shape);
}

FHolderShape FPageLedger::SlotShape(int32_t Slot) const
{
	if (!Impl->IsSlot(Slot)) return FHolderShape();
	return Impl->Slots[static_cast<size_t>(Slot)].Shape;
}

uint32_t FPageLedger::SlotHeadroomHold(int32_t Slot) const
{
	if (!Impl->IsSlot(Slot)) return 0;
	return Impl->Slots[static_cast<size_t>(Slot)].Hold;
}

bool FPageLedger::SlotHasJob(int32_t Slot) const
{
	if (!Impl->IsSlot(Slot)) return false;
	return Impl->Slots[static_cast<size_t>(Slot)].Job != InvalidTicket;
}

// ---- Region C -----------------------------------------------------------------------------------

ECacheRegionCharge FPageLedger::ChargeCacheRegionBytes(uint64_t Bytes)
{
	if (!Impl->bConfigured) return ECacheRegionCharge::NotConfigured;
	const uint64_t PageBytes = Impl->Config.PageBytes;
	if (Bytes % PageBytes != 0) return ECacheRegionCharge::InvalidRequest;
	const uint64_t Pages = Bytes / PageBytes;
	if (Pages > Impl->FreeOf(ERegion::C)) return ECacheRegionCharge::Exhausted;
	Impl->Charge(ERegion::C, static_cast<uint32_t>(Pages));
	return ECacheRegionCharge::Ok;
}

ECacheRegionCharge FPageLedger::RefundCacheRegionBytes(uint64_t Bytes)
{
	if (!Impl->bConfigured) return ECacheRegionCharge::NotConfigured;
	const uint64_t PageBytes = Impl->Config.PageBytes;
	if (Bytes % PageBytes != 0) return ECacheRegionCharge::InvalidRequest;
	const uint64_t Pages = Bytes / PageBytes;
	if (Pages > Impl->ChargedOf(ERegion::C)) return ECacheRegionCharge::InvalidRequest;
	Impl->Refund(ERegion::C, static_cast<uint32_t>(Pages));
	return ECacheRegionCharge::Ok;
}

uint64_t FPageLedger::CacheRegionChargedBytes() const
{
	if (!Impl->bConfigured) return 0;
	return static_cast<uint64_t>(Impl->ChargedOf(ERegion::C)) * Impl->Config.PageBytes;
}

uint64_t FPageLedger::CacheRegionCapacityBytes() const
{
	if (!Impl->bConfigured) return 0;
	return static_cast<uint64_t>(Impl->CapacityOf(ERegion::C)) * Impl->Config.PageBytes;
}

// ---- The ledger-fault detector ------------------------------------------------------------------

void FPageLedger::NoteEngineExhaustion()
{
	if (SUPERSLM_LEDGER_MUTANT(Impl, LedgerFaultNotCounted)) return;
	++Impl->Faults;
}

uint64_t FPageLedger::LedgerFaults() const
{
	return Impl->Faults;
}

bool FPageLedger::ChargesWithinPool() const
{
	uint64_t Sum = 0;
	for (int I = 0; I < static_cast<int>(ERegion::Count); ++I)
	{
		if (Impl->Charged[I] > Impl->Capacity[I]) return false;
		Sum += Impl->Charged[I];
	}
	return Sum <= Impl->Pool;
}

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
void FPageLedger::SetMutationForTest(ELedgerMutation Mutation)
{
	Impl->Mutation = Mutation;
}

void FPageLedger::SetForceUnderChargeForTest(EChargeKind Kind, uint32_t Pages)
{
	if (Kind >= EChargeKind::Count) return;
	Impl->UnderCharge[static_cast<int>(Kind)] = Pages;
}
#endif

}  // namespace SuperSLMPaging

// File-local: a unity build must not carry them into the next file.
#undef SUPERSLM_LEDGER_MUTANT
#undef SUPERSLM_LEDGER_UNDERCHARGE
