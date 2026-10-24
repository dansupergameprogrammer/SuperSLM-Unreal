#include "SuperSLMResidencyCore.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace SuperSLMCore
{
	struct FSuperSLMResidencyCore::FImpl
	{
		struct FEntry
		{
			FResidencyKind Kind = 0;
			FResidencyLedgerId LedgerId = kResidencyLedgerHostRam;
			FResidencyKey Key;
			bool bKeyed = false;
			EResidencyState State = EResidencyState::Unadmitted;
			uint64_t ChargedBytes = 0;  // InFlight, Resident, Releasing
			uint64_t LastTouch = 0;     // larger is more recent
			std::array<uint32_t, kResidencyPinCount> Pins{};
			std::vector<int64_t> Leases; // Live leases attached to it

			// Waiting: the peak it is owed, the free charge it took, its arrival, and its claims
			// (pending refund's entry, bytes).
			uint64_t PeakBytes = 0;
			uint64_t WaitHeldBytes = 0;
			uint64_t Arrival = 0;
			std::vector<std::pair<uint64_t, uint64_t>> ClaimsOn; // (refund serial, bytes)

			// Nested entries: the parent whose full pages it shares (kept after its release is
			// reported while descendants' releases are still pending: the family needs the ancestry),
			// and the parent its charge decision pinned (ChildOp) until it settles or returns.
			FResidencyEntryId Parent = kInvalidResidencyEntry;
			FResidencyEntryId PinnedParent = kInvalidResidencyEntry;
			bool bGhost = false;    // holds a ghost charge (Releasing or Ghost)
			uint64_t GhostBytes = 0;
			bool bMarked = false;   // a deferred drop's mark (inert here)
		};

		// One pending refund (precache plan §5.11 (a)): an evicted entry's own charge, or an ended
		// ghost's. It completes when Family -- the pending releases among its entry's own handle and its
		// descendants' handles -- is empty, and stays claimable until then.
		struct FRefund
		{
			FResidencyKind Kind = 0;
			FResidencyLedgerId LedgerId = kResidencyLedgerHostRam;
			uint64_t Bytes = 0;
			uint64_t UnclaimedBytes = 0;
			std::vector<std::pair<FResidencyEntryId, uint64_t>> Claims; // (waiter, bytes)
			std::set<FResidencyEntryId> Family;                         // releases not yet reported
			bool bGhostRefund = false;                                  // an ended ghost's charge
		};

		struct FLease
		{
			ELeaseStatus Status = ELeaseStatus::Live;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
		};

		struct FLedgerState
		{
			FResidencyLedger Ledger;
		};

		uint32_t KindCount;
		FResidencyHandleCounter HandleCounter;
		std::map<std::pair<FResidencyKind, FResidencyLedgerId>, FLedgerState> Ledgers;
		std::map<FResidencyEntryId, FEntry> Entries;
		std::map<std::pair<FResidencyKind, FResidencyKey>, FResidencyEntryId> ByKey;
		std::map<int64_t, FLease> Leases;
		std::map<uint64_t, FRefund> Refunds; // by serial: oldest decision first
		std::vector<FResidencyEntryId> Evicted;
		std::vector<FResidencyEntryId> Admitted;
		FResidencyEntryId NextEntryId = 1; // never reset, so an id is never reused
		uint64_t TouchSerial = 0;
		uint64_t RefundSerial = 0;
		uint64_t ArrivalSerial = 0;
		uint64_t Evictions = 0;
		uint64_t BudgetExhausted = 0;
		uint64_t Deferred = 0;
		uint64_t LeasesMinted = 0;
		uint64_t GhostEndings = 0;

		FImpl(uint32_t InKindCount, const FResidencyHandleCounter& InCounter)
			: KindCount(InKindCount), HandleCounter(InCounter)
		{
			for (uint32_t Kind = 0; Kind < KindCount; ++Kind)
			{
				const FResidencyKind K = static_cast<FResidencyKind>(Kind);
				Ledgers[{K, kResidencyLedgerHostRam}].Ledger.Class = EResidencyClass::HostRam;
				Ledgers[{K, kResidencyLedgerVram}].Ledger.Class = EResidencyClass::Vram;
			}
		}

		FLedgerState* LedgerOf(FResidencyKind Kind, FResidencyLedgerId Ledger)
		{
			const auto It = Ledgers.find({Kind, Ledger});
			return It == Ledgers.end() ? nullptr : &It->second;
		}
		FLedgerState& LedgerOf(const FEntry& Entry) { return *LedgerOf(Entry.Kind, Entry.LedgerId); }

		// Uncharged budget; 0 when a lowered budget is already exceeded.
		static uint64_t FreeBytes(const FResidencyLedger& L)
		{
			return L.BudgetBytes > L.ChargedBytes ? L.BudgetBytes - L.ChargedBytes : 0;
		}

		FEntry* Find(FResidencyEntryId Id)
		{
			const auto It = Entries.find(Id);
			return It == Entries.end() ? nullptr : &It->second;
		}

		static bool IsPinned(const FEntry& Entry)
		{
			for (uint32_t Count : Entry.Pins)
			{
				if (Count != 0)
				{
					return true;
				}
			}
			return false;
		}

		// Leases attached to an entry that is going away are left Live and unattached.
		void DetachAll(FEntry& Entry)
		{
			for (int64_t Handle : Entry.Leases)
			{
				Leases[Handle].Entry = kInvalidResidencyEntry;
			}
			Entry.Leases.clear();
		}

		void Unkey(const FEntry& Entry)
		{
			if (Entry.bKeyed)
			{
				const auto It = ByKey.find({Entry.Kind, Entry.Key});
				if (It != ByKey.end())
				{
					ByKey.erase(It);
				}
			}
		}

		// Evictable entries of one ledger, least recently used first.
		std::vector<FResidencyEntryId> LruOrder(FResidencyKind Kind, FResidencyLedgerId Ledger) const
		{
			std::vector<std::pair<uint64_t, FResidencyEntryId>> Order;
			for (const auto& Pair : Entries)
			{
				const FEntry& E = Pair.second;
				if (E.Kind == Kind && E.LedgerId == Ledger && E.State == EResidencyState::Resident && !IsPinned(E))
				{
					Order.push_back({E.LastTouch, Pair.first});
				}
			}
			std::sort(Order.begin(), Order.end());
			std::vector<FResidencyEntryId> Ids;
			for (const auto& Pair : Order)
			{
				Ids.push_back(Pair.second);
			}
			return Ids;
		}

		// --- Nested entries --------------------------------------------------------------------------

		// True when Ancestor is reached walking Id's parent links (Id itself excluded).
		bool IsDescendant(FResidencyEntryId Id, FResidencyEntryId Ancestor) const
		{
			size_t Hops = 0;
			for (auto It = Entries.find(Id); It != Entries.end() && Hops <= Entries.size(); ++Hops)
			{
				const FResidencyEntryId Up = It->second.Parent;
				if (Up == kInvalidResidencyEntry)
				{
					return false;
				}
				if (Up == Ancestor)
				{
					return true;
				}
				It = Entries.find(Up);
			}
			return false;
		}

		// A Resident entry still shares Id's pages (§5.4: a grandchild holds the grandparent's too).
		bool HasCachedDescendant(FResidencyEntryId Id) const
		{
			for (const auto& Pair : Entries)
			{
				if (Pair.first != Id && Pair.second.State == EResidencyState::Resident && IsDescendant(Pair.first, Id))
				{
					return true;
				}
			}
			return false;
		}

		// Every pending release among Id's own handle and its descendants' handles.
		std::set<FResidencyEntryId> FamilyOf(FResidencyEntryId Id) const
		{
			std::set<FResidencyEntryId> Family;
			for (const auto& Pair : Entries)
			{
				if (Pair.second.State == EResidencyState::Releasing && (Pair.first == Id || IsDescendant(Pair.first, Id)))
				{
					Family.insert(Pair.first);
				}
			}
			return Family;
		}

		void AddRefund(const FEntry& Entry, uint64_t Bytes, std::set<FResidencyEntryId> Family)
		{
			FRefund& R = Refunds[++RefundSerial];
			R.Kind = Entry.Kind;
			R.LedgerId = Entry.LedgerId;
			R.Bytes = Bytes;
			R.UnclaimedBytes = Bytes;
			R.Family = std::move(Family);
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.ReleasingBytes += Bytes;
			L.ReleasingEntries += 1;
		}

		// Erases an entry; children that still name it are linked to its parent, so the ancestry a
		// pending family needs survives the release of an entry in the middle of a chain.
		void EraseEntry(FResidencyEntryId Id)
		{
			const auto It = Entries.find(Id);
			if (It == Entries.end())
			{
				return;
			}
			const FResidencyEntryId Up = It->second.Parent;
			for (auto& Pair : Entries)
			{
				if (Pair.second.Parent == Id)
				{
					Pair.second.Parent = Up;
				}
			}
			Entries.erase(It);
		}

		// The ghost of P ends (its last cached descendant was just evicted): its charge becomes a pending
		// refund of its own, whose family is P's own release (if pending) and its descendants'.
		void EndGhost(FResidencyEntryId P)
		{
			FEntry& Ghost = Entries[P];
			FResidencyLedger& L = LedgerOf(Ghost).Ledger;
			L.GhostBytes -= Ghost.GhostBytes;
			L.GhostEntries -= 1;
			const uint64_t Bytes = Ghost.GhostBytes;
			Ghost.bGhost = false;
			Ghost.GhostBytes = 0;
			++GhostEndings;
			AddRefund(Ghost, Bytes, FamilyOf(P));
			Refunds[RefundSerial].bGhostRefund = true;
			if (Ghost.State == EResidencyState::Ghost)
			{
				EraseEntry(P); // its own release was already reported: nothing of it remains
			}
		}

		void ToReleasing(FResidencyEntryId Id, FEntry& Entry)
		{
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.ResidentBytes -= Entry.ChargedBytes;
			L.ResidentEntries -= 1;
			Entry.State = EResidencyState::Releasing;
			Unkey(Entry);
			Evicted.push_back(Id);
			if (HasCachedDescendant(Id))
			{
				// §5.4: its pages stay, held by cached descendants; its whole charge stays as a ghost.
				Entry.bGhost = true;
				Entry.GhostBytes = Entry.ChargedBytes;
				L.GhostBytes += Entry.ChargedBytes;
				L.GhostEntries += 1;
				return;
			}
			AddRefund(Entry, Entry.ChargedBytes, FamilyOf(Id));
			// A descendant leaving the cache may end ghosts above it, nearest first.
			FResidencyEntryId Up = Entry.Parent;
			for (size_t Hops = 0; Up != kInvalidResidencyEntry && Hops <= Entries.size(); ++Hops)
			{
				const auto It = Entries.find(Up);
				if (It == Entries.end())
				{
					break;
				}
				const FResidencyEntryId Next = It->second.Parent;
				if (It->second.bGhost && !HasCachedDescendant(Up))
				{
					EndGhost(Up);
				}
				Up = Next;
			}
		}

		// Claims up to Lack bytes of a pending refund for Waiter; returns what it took.
		uint64_t Claim(uint64_t Serial, FResidencyEntryId Waiter, uint64_t Lack, std::vector<std::pair<uint64_t, uint64_t>>& OutClaims)
		{
			FRefund& R = Refunds[Serial];
			const uint64_t Amount = (std::min)(R.UnclaimedBytes, Lack);
			if (Amount != 0)
			{
				R.UnclaimedBytes -= Amount;
				R.Claims.push_back({Waiter, Amount});
				OutClaims.push_back({Serial, Amount});
			}
			return Amount;
		}

		void Unclaim(uint64_t Serial, FResidencyEntryId Waiter, uint64_t Amount)
		{
			const auto It = Refunds.find(Serial);
			if (It == Refunds.end())
			{
				return;
			}
			FRefund& R = It->second;
			R.UnclaimedBytes += Amount;
			for (size_t I = 0; I < R.Claims.size(); ++I)
			{
				if (R.Claims[I].first == Waiter)
				{
					R.Claims.erase(R.Claims.begin() + static_cast<std::ptrdiff_t>(I));
					break;
				}
			}
		}

		// Claims from every pending refund of the ledger with a serial above After, oldest first, until
		// Lack is covered; returns what is still lacking.
		uint64_t ClaimFrom(FResidencyKind Kind, FResidencyLedgerId Ledger, uint64_t After, FResidencyEntryId Waiter, uint64_t Lack,
			std::vector<std::pair<uint64_t, uint64_t>>& OutClaims)
		{
			std::vector<uint64_t> Serials;
			for (auto It = Refunds.upper_bound(After); It != Refunds.end(); ++It)
			{
				if (It->second.Kind == Kind && It->second.LedgerId == Ledger)
				{
					Serials.push_back(It->first);
				}
			}
			for (uint64_t Serial : Serials)
			{
				if (Lack == 0)
				{
					break;
				}
				Lack -= Claim(Serial, Waiter, Lack, OutClaims);
			}
			return Lack;
		}

		void PinParent(FEntry& Entry)
		{
			if (Entry.Parent != kInvalidResidencyEntry)
			{
				++Entries[Entry.Parent].Pins[static_cast<uint32_t>(EResidencyPin::ChildOp)];
				Entry.PinnedParent = Entry.Parent;
			}
		}

		void UnpinParent(FEntry& Entry)
		{
			if (Entry.PinnedParent == kInvalidResidencyEntry)
			{
				return;
			}
			const auto It = Entries.find(Entry.PinnedParent);
			if (It != Entries.end() && It->second.Pins[static_cast<uint32_t>(EResidencyPin::ChildOp)] != 0)
			{
				--It->second.Pins[static_cast<uint32_t>(EResidencyPin::ChildOp)];
			}
			Entry.PinnedParent = kInvalidResidencyEntry;
		}

		void ChargeNow(FEntry& Entry, uint64_t Bytes)
		{
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.ChargedBytes += Bytes;
			L.InFlightBytes += Bytes;
			L.InFlightEntries += 1;
			Entry.ChargedBytes = Bytes;
			Entry.State = EResidencyState::InFlight;
			PinParent(Entry);
		}

		// A pending refund whose family is empty completes: each claimed part passes to its waiter's
		// charge, the unclaimed rest returns to free charge. Waiters left with no claim are collected.
		void CompleteRefund(uint64_t Serial, std::vector<std::pair<uint64_t, FResidencyEntryId>>& Ready)
		{
			const FRefund R = Refunds[Serial];
			Refunds.erase(Serial);
			FResidencyLedger& L = LedgerOf(R.Kind, R.LedgerId)->Ledger;
			L.ReleasingBytes -= R.Bytes;
			L.ReleasingEntries -= 1;
			L.ChargedBytes -= R.UnclaimedBytes;
			for (const auto& Claimed : R.Claims)
			{
				FEntry& Waiter = Entries[Claimed.first];
				L.ClaimedRefundBytes -= Claimed.second;
				L.WaitingBytes += Claimed.second;
				Waiter.WaitHeldBytes += Claimed.second;
				for (size_t I = 0; I < Waiter.ClaimsOn.size(); ++I)
				{
					if (Waiter.ClaimsOn[I].first == Serial)
					{
						Waiter.ClaimsOn.erase(Waiter.ClaimsOn.begin() + static_cast<std::ptrdiff_t>(I));
						break;
					}
				}
				if (Waiter.ClaimsOn.empty())
				{
					Ready.push_back({Waiter.Arrival, Claimed.first});
				}
			}
		}

		// A waiter whose last claim has passed to it: InFlight with its whole peak charged.
		void AdmitWaiter(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.WaitingBytes -= Entry.WaitHeldBytes;
			L.WaitingEntries -= 1;
			L.InFlightBytes += Entry.PeakBytes;
			L.InFlightEntries += 1;
			Entry.ChargedBytes = Entry.PeakBytes;
			Entry.WaitHeldBytes = 0;
			Entry.State = EResidencyState::InFlight;
			Admitted.push_back(Id);
		}
	};

	FSuperSLMResidencyCore::FSuperSLMResidencyCore(uint32_t KindCount, const FResidencyHandleCounter& HandleCounter)
		: Impl(new FImpl(KindCount, HandleCounter))
	{
	}

	FSuperSLMResidencyCore::~FSuperSLMResidencyCore() = default;

	void FSuperSLMResidencyCore::Reset()
	{
		const FResidencyEntryId NextId = Impl->NextEntryId;
		std::unique_ptr<FImpl> Fresh(new FImpl(Impl->KindCount, Impl->HandleCounter));
		for (const auto& Pair : Impl->Ledgers)
		{
			Fresh->Ledgers[Pair.first].Ledger.Class = Pair.second.Ledger.Class; // added ledgers stay defined
		}
		Fresh->NextEntryId = NextId;
		Impl.swap(Fresh);
	}

	bool FSuperSLMResidencyCore::AddLedger(FResidencyKind Kind, FResidencyLedgerId Ledger, EResidencyClass Class)
	{
		if (Kind >= Impl->KindCount || Impl->LedgerOf(Kind, Ledger) != nullptr)
		{
			return false;
		}
		Impl->Ledgers[{Kind, Ledger}].Ledger.Class = Class;
		return true;
	}

	void FSuperSLMResidencyCore::SetBudget(FResidencyKind Kind, FResidencyLedgerId Ledger, uint64_t Bytes)
	{
		if (FImpl::FLedgerState* S = Impl->LedgerOf(Kind, Ledger))
		{
			S->Ledger.BudgetBytes = Bytes;
		}
	}

	FResidencyLedger FSuperSLMResidencyCore::GetLedger(FResidencyKind Kind, FResidencyLedgerId Ledger) const
	{
		const FImpl::FLedgerState* S = Impl->LedgerOf(Kind, Ledger);
		return S != nullptr ? S->Ledger : FResidencyLedger();
	}

	FResidencyEntryId FSuperSLMResidencyCore::CreateEntry(FResidencyKind Kind, FResidencyLedgerId Ledger, const FResidencyKey& Key, bool bKeyed)
	{
		if (Impl->LedgerOf(Kind, Ledger) == nullptr || (bKeyed && Impl->ByKey.count({Kind, Key}) != 0))
		{
			return kInvalidResidencyEntry;
		}
		const FResidencyEntryId Id = Impl->NextEntryId++;
		FImpl::FEntry& Entry = Impl->Entries[Id];
		Entry.Kind = Kind;
		Entry.LedgerId = Ledger;
		Entry.Key = Key;
		Entry.bKeyed = bKeyed;
		Entry.LastTouch = ++Impl->TouchSerial;
		if (bKeyed)
		{
			Impl->ByKey[{Kind, Key}] = Id;
		}
		return Id;
	}

	FResidencyEntryId FSuperSLMResidencyCore::FindByKey(FResidencyKind Kind, const FResidencyKey& Key) const
	{
		const auto It = Impl->ByKey.find({Kind, Key});
		return It == Impl->ByKey.end() ? kInvalidResidencyEntry : It->second;
	}

	EResidencyState FSuperSLMResidencyCore::GetState(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->State : EResidencyState::Unknown;
	}

	uint64_t FSuperSLMResidencyCore::GetChargedBytes(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->ChargedBytes : 0;
	}

	EChargeOutcome FSuperSLMResidencyCore::Charge(FResidencyEntryId Id, uint64_t PeakBytes, bool bParentRequired)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Unadmitted)
		{
			return EChargeOutcome::BudgetExhausted; // a broken precondition, not a refusal to count
		}
		const FResidencyKind Kind = Entry->Kind;
		const FResidencyLedgerId LedgerId = Entry->LedgerId;
		FResidencyLedger& L = Impl->LedgerOf(Kind, LedgerId)->Ledger;

		// A nested request touches its parent first (§5.4: parents age more slowly than their children).
		const FResidencyEntryId Parent = Entry->Parent;
		if (Parent != kInvalidResidencyEntry)
		{
			if (GetState(Parent) == EResidencyState::Resident)
			{
				Touch(Parent);
			}
			else
			{
				Entry->Parent = kInvalidResidencyEntry; // gone before the decision: private
			}
		}

		// 1. The free charge alone covers it: admitted now.
		const uint64_t Free = FImpl::FreeBytes(L);
		if (PeakBytes <= Free)
		{
			Impl->ChargeNow(*Entry, PeakBytes);
			return EChargeOutcome::Charged;
		}
		std::vector<std::pair<uint64_t, uint64_t>> Claims;

		// 2. Unclaimed pending refunds, oldest first.
		uint64_t Lack = Impl->ClaimFrom(Kind, LedgerId, 0, Id, PeakBytes - Free, Claims);

		// 3. New evictions, least recently used first, each claimed up to what is still lacking: the
		// eviction's own refund, then any ghost it ended. A required parent is never evicted here.
		if (Lack != 0)
		{
			for (FResidencyEntryId Candidate : Impl->LruOrder(Kind, LedgerId))
			{
				if (Lack == 0)
				{
					break;
				}
				if (Candidate == Impl->Entries[Id].Parent && bParentRequired)
				{
					continue;
				}
				const uint64_t Before = Impl->RefundSerial;
				Impl->ToReleasing(Candidate, Impl->Entries[Candidate]);
				++Impl->Evictions;
				Lack = Impl->ClaimFrom(Kind, LedgerId, Before, Id, Lack, Claims);
			}
		}

		// Not covered: refused by name. Its claims go back; the evictions it decided stand.
		if (Lack != 0)
		{
			for (const auto& Claimed : Claims)
			{
				Impl->Unclaim(Claimed.first, Id, Claimed.second);
			}
			++Impl->BudgetExhausted;
			return EChargeOutcome::BudgetExhausted;
		}

		// A hint-only parent evicted by this very loop: the load runs private (§5.4, rev 1.17 R2).
		Entry = Impl->Find(Id);
		if (Entry->Parent != kInvalidResidencyEntry && GetState(Entry->Parent) != EResidencyState::Resident)
		{
			Entry->Parent = kInvalidResidencyEntry;
		}

		// Covered with claims: a waiter. It takes the free charge now, draws nothing, and pins its
		// parent from this decision (§5.11 (a), rev 1.16 F1).
		Entry->State = EResidencyState::Waiting;
		Entry->PeakBytes = PeakBytes;
		Entry->WaitHeldBytes = Free;
		Entry->Arrival = ++Impl->ArrivalSerial;
		Entry->ClaimsOn = Claims;
		Impl->PinParent(*Entry);
		L.ChargedBytes += Free;
		L.WaitingBytes += Free;
		L.WaitingEntries += 1;
		for (const auto& Claimed : Claims)
		{
			L.ClaimedRefundBytes += Claimed.second;
		}
		++Impl->Deferred;
		return EChargeOutcome::Deferred;
	}

	bool FSuperSLMResidencyCore::SetParent(FResidencyEntryId Id, FResidencyEntryId Parent)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		const FImpl::FEntry* Up = Impl->Find(Parent);
		if (Entry == nullptr || Up == nullptr || Id == Parent || Up->State != EResidencyState::Resident ||
			(Entry->State != EResidencyState::Unadmitted && Entry->State != EResidencyState::Resident) ||
			Up->Kind != Entry->Kind || Up->LedgerId != Entry->LedgerId || Impl->IsDescendant(Parent, Id))
		{
			return false;
		}
		Entry->Parent = Parent;
		return true;
	}

	FResidencyEntryId FSuperSLMResidencyCore::GetParent(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->Parent : kInvalidResidencyEntry;
	}

	void FSuperSLMResidencyCore::DetachParent(FResidencyEntryId Id)
	{
		if (FImpl::FEntry* Entry = Impl->Find(Id))
		{
			Entry->Parent = kInvalidResidencyEntry;
		}
	}

	bool FSuperSLMResidencyCore::IsGhost(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr && Entry->bGhost;
	}

	uint64_t FSuperSLMResidencyCore::GetGhostBytes(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->GhostBytes : 0;
	}

	void FSuperSLMResidencyCore::Settle(FResidencyEntryId Id, uint64_t SteadyBytes)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::InFlight)
		{
			return;
		}
		FImpl::FLedgerState& S = Impl->LedgerOf(*Entry);
		const uint64_t Steady = (std::min)(SteadyBytes, Entry->ChargedBytes);
		S.Ledger.InFlightBytes -= Entry->ChargedBytes;
		S.Ledger.InFlightEntries -= 1;
		S.Ledger.ChargedBytes -= Entry->ChargedBytes - Steady;
		S.Ledger.ResidentBytes += Steady;
		S.Ledger.ResidentEntries += 1;
		Entry->ChargedBytes = Steady;
		Entry->State = EResidencyState::Resident;
		Entry->LastTouch = ++Impl->TouchSerial;
		Impl->UnpinParent(*Entry); // the child's job is applied: its parent pin ends with it
	}

	void FSuperSLMResidencyCore::ReturnCharge(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr)
		{
			return;
		}
		FImpl::FLedgerState& S = Impl->LedgerOf(*Entry);
		if (Entry->State == EResidencyState::Waiting)
		{
			for (const auto& Claimed : Entry->ClaimsOn)
			{
				Impl->Unclaim(Claimed.first, Id, Claimed.second);
				S.Ledger.ClaimedRefundBytes -= Claimed.second;
			}
			Entry->ClaimsOn.clear();
			S.Ledger.WaitingBytes -= Entry->WaitHeldBytes;
			S.Ledger.ChargedBytes -= Entry->WaitHeldBytes;
			S.Ledger.WaitingEntries -= 1;
			Entry->WaitHeldBytes = 0;
			Entry->PeakBytes = 0;
		}
		else if (Entry->State == EResidencyState::InFlight)
		{
			S.Ledger.InFlightBytes -= Entry->ChargedBytes;
			S.Ledger.InFlightEntries -= 1;
			S.Ledger.ChargedBytes -= Entry->ChargedBytes;
			Entry->ChargedBytes = 0;
		}
		else
		{
			return;
		}
		Impl->UnpinParent(*Entry);
		Entry->State = EResidencyState::Unadmitted;
	}

	bool FSuperSLMResidencyCore::Destroy(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Unadmitted)
		{
			return false;
		}
		Impl->Unkey(*Entry);
		Impl->DetachAll(*Entry);
		Impl->EraseEntry(Id);
		return true;
	}

	bool FSuperSLMResidencyCore::EvictLru(FResidencyKind Kind, FResidencyLedgerId Ledger, FResidencyEntryId& OutEvicted)
	{
		OutEvicted = kInvalidResidencyEntry;
		const std::vector<FResidencyEntryId> Order = Impl->LruOrder(Kind, Ledger);
		if (Order.empty())
		{
			return false;
		}
		OutEvicted = Order.front();
		Impl->ToReleasing(OutEvicted, Impl->Entries[OutEvicted]);
		++Impl->Evictions;
		return true;
	}

	bool FSuperSLMResidencyCore::EvictForTeardown(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Resident)
		{
			return false;
		}
		Impl->ToReleasing(Id, *Entry);
		return true;
	}

	bool FSuperSLMResidencyCore::EvictUnpinned(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Resident || FImpl::IsPinned(*Entry))
		{
			return false;
		}
		Entry->bMarked = false;
		Impl->ToReleasing(Id, *Entry);
		return true;
	}

	bool FSuperSLMResidencyCore::SetMarked(FResidencyEntryId Id, bool bMarked)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr)
		{
			return false;
		}
		Entry->bMarked = bMarked;
		return true;
	}

	bool FSuperSLMResidencyCore::IsMarked(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr && Entry->bMarked;
	}

	bool FSuperSLMResidencyCore::ReleaseCompleted(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Releasing)
		{
			return false;
		}
		// This release leaves every family it is in; each refund whose family is now empty completes.
		std::vector<uint64_t> Done;
		for (auto& Pair : Impl->Refunds)
		{
			Pair.second.Family.erase(Id);
			if (Pair.second.Family.empty())
			{
				Done.push_back(Pair.first);
			}
		}
		std::vector<std::pair<uint64_t, FResidencyEntryId>> Ready;
		for (uint64_t Serial : Done)
		{
			Impl->CompleteRefund(Serial, Ready);
		}
		Entry = Impl->Find(Id);
		Impl->DetachAll(*Entry);
		if (Entry->bGhost)
		{
			Entry->State = EResidencyState::Ghost; // only its ghost charge remains
		}
		else
		{
			Impl->EraseEntry(Id);
		}
		std::sort(Ready.begin(), Ready.end());
		for (const auto& Pair : Ready)
		{
			Impl->AdmitWaiter(Pair.second);
		}
		return true;
	}

	std::vector<FResidencyEntryId> FSuperSLMResidencyCore::TakeEvicted()
	{
		std::vector<FResidencyEntryId> Out;
		Out.swap(Impl->Evicted);
		return Out;
	}

	std::vector<FResidencyEntryId> FSuperSLMResidencyCore::TakeAdmitted()
	{
		std::vector<FResidencyEntryId> Out;
		Out.swap(Impl->Admitted);
		return Out;
	}

	uint64_t FSuperSLMResidencyCore::GetArrival(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->Arrival : 0;
	}

	uint64_t FSuperSLMResidencyCore::GetWaitingHeldBytes(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr && Entry->State == EResidencyState::Waiting ? Entry->WaitHeldBytes : 0;
	}

	uint64_t FSuperSLMResidencyCore::GetClaimedBytes(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		uint64_t Sum = 0;
		if (Entry != nullptr)
		{
			for (const auto& Claimed : Entry->ClaimsOn)
			{
				Sum += Claimed.second;
			}
		}
		return Sum;
	}

	uint32_t FSuperSLMResidencyCore::GetPendingRefundCount(FResidencyKind Kind, FResidencyLedgerId Ledger) const
	{
		const FImpl::FLedgerState* S = Impl->LedgerOf(Kind, Ledger);
		return S != nullptr ? S->Ledger.ReleasingEntries : 0;
	}

	void FSuperSLMResidencyCore::Touch(FResidencyEntryId Id)
	{
		if (FImpl::FEntry* Entry = Impl->Find(Id))
		{
			Entry->LastTouch = ++Impl->TouchSerial;
		}
	}

	std::vector<FResidencyEntryId> FSuperSLMResidencyCore::LruOrder(FResidencyKind Kind, FResidencyLedgerId Ledger) const
	{
		return Impl->LruOrder(Kind, Ledger);
	}

	void FSuperSLMResidencyCore::AddPin(FResidencyEntryId Id, EResidencyPin Reason)
	{
		if (FImpl::FEntry* Entry = Impl->Find(Id))
		{
			++Entry->Pins[static_cast<uint32_t>(Reason)];
		}
	}

	bool FSuperSLMResidencyCore::RemovePin(FResidencyEntryId Id, EResidencyPin Reason)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->Pins[static_cast<uint32_t>(Reason)] == 0)
		{
			return false;
		}
		--Entry->Pins[static_cast<uint32_t>(Reason)];
		return true;
	}

	uint32_t FSuperSLMResidencyCore::GetPinCount(FResidencyEntryId Id, EResidencyPin Reason) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr ? Entry->Pins[static_cast<uint32_t>(Reason)] : 0;
	}

	bool FSuperSLMResidencyCore::IsPinned(FResidencyEntryId Id) const
	{
		const FImpl::FEntry* Entry = Impl->Find(Id);
		return Entry != nullptr && FImpl::IsPinned(*Entry);
	}

	int64_t FSuperSLMResidencyCore::MintLease()
	{
		const int64_t Handle = Impl->HandleCounter.Next();
		Impl->Leases[Handle] = FImpl::FLease();
		++Impl->LeasesMinted;
		return Handle;
	}

	bool FSuperSLMResidencyCore::AttachLease(int64_t Handle, FResidencyEntryId Id)
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end() || It->second.Status != ELeaseStatus::Live || It->second.Entry != kInvalidResidencyEntry ||
			Impl->Find(Id) == nullptr)
		{
			return false;
		}
		It->second.Entry = Id;
		Impl->Entries[Id].Leases.push_back(Handle);
		AddPin(Id, EResidencyPin::Lease);
		return true;
	}

	bool FSuperSLMResidencyCore::DetachLease(int64_t Handle)
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end() || It->second.Status != ELeaseStatus::Live || It->second.Entry == kInvalidResidencyEntry)
		{
			return false;
		}
		if (FImpl::FEntry* Entry = Impl->Find(It->second.Entry))
		{
			Entry->Leases.erase(std::find(Entry->Leases.begin(), Entry->Leases.end(), Handle));
			RemovePin(It->second.Entry, EResidencyPin::Lease);
		}
		It->second.Entry = kInvalidResidencyEntry;
		return true;
	}

	bool FSuperSLMResidencyCore::EndLease(int64_t Handle)
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end() || It->second.Status != ELeaseStatus::Live)
		{
			return false;
		}
		DetachLease(Handle);
		It->second.Status = ELeaseStatus::Ended;
		return true;
	}

	ELeaseStatus FSuperSLMResidencyCore::GetLeaseStatus(int64_t Handle) const
	{
		const auto It = Impl->Leases.find(Handle);
		return It == Impl->Leases.end() ? ELeaseStatus::Unknown : It->second.Status;
	}

	FResidencyEntryId FSuperSLMResidencyCore::GetLeaseEntry(int64_t Handle) const
	{
		const auto It = Impl->Leases.find(Handle);
		return It == Impl->Leases.end() || It->second.Status != ELeaseStatus::Live ? kInvalidResidencyEntry : It->second.Entry;
	}

	FResidencySnapshot FSuperSLMResidencyCore::Snapshot() const
	{
		FResidencySnapshot Snap;
		for (const auto& Pair : Impl->Ledgers)
		{
			FResidencyLedgerRow Row;
			Row.Kind = Pair.first.first;
			Row.LedgerId = Pair.first.second;
			Row.Ledger = Pair.second.Ledger;
			Snap.Ledgers.push_back(Row);
		}
		Snap.Evictions = Impl->Evictions;
		Snap.BudgetExhausted = Impl->BudgetExhausted;
		Snap.Deferred = Impl->Deferred;
		Snap.LeasesMinted = Impl->LeasesMinted;
		Snap.GhostEndings = Impl->GhostEndings;
		return Snap;
	}
}
