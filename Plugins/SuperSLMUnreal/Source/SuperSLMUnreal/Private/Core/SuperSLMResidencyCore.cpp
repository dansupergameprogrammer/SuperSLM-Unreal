#include "SuperSLMResidencyCore.h"

#include <algorithm>
#include <deque>
#include <map>

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
			uint64_t WaitingBytes = 0;  // Waiting: the peak it is owed
			uint64_t LastTouch = 0;     // larger is more recent
			std::array<uint32_t, kResidencyPinCount> Pins{};
			std::vector<int64_t> Leases; // Live leases attached to it
		};

		struct FLease
		{
			ELeaseStatus Status = ELeaseStatus::Live;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
		};

		struct FLedgerState
		{
			FResidencyLedger Ledger;
			std::deque<FResidencyEntryId> Waiting; // FIFO
			uint64_t OwedBytes = 0;                // the Waiting entries' peaks
		};

		uint32_t KindCount;
		FResidencyHandleCounter HandleCounter;
		std::map<std::pair<FResidencyKind, FResidencyLedgerId>, FLedgerState> Ledgers;
		std::map<FResidencyEntryId, FEntry> Entries;
		std::map<std::pair<FResidencyKind, FResidencyKey>, FResidencyEntryId> ByKey;
		std::map<int64_t, FLease> Leases;
		std::vector<FResidencyEntryId> Evicted;
		std::vector<FResidencyEntryId> Admitted;
		FResidencyEntryId NextEntryId = 1; // never reset, so an id is never reused
		uint64_t TouchSerial = 0;
		uint64_t Evictions = 0;
		uint64_t BudgetExhausted = 0;
		uint64_t Deferred = 0;
		uint64_t LeasesMinted = 0;

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

		void ToReleasing(FResidencyEntryId Id, FEntry& Entry)
		{
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.ResidentBytes -= Entry.ChargedBytes;
			L.ResidentEntries -= 1;
			L.ReleasingBytes += Entry.ChargedBytes;
			L.ReleasingEntries += 1;
			Entry.State = EResidencyState::Releasing;
			Unkey(Entry);
			Evicted.push_back(Id);
		}

		void ChargeNow(FEntry& Entry, uint64_t Bytes)
		{
			FResidencyLedger& L = LedgerOf(Entry).Ledger;
			L.ChargedBytes += Bytes;
			L.InFlightBytes += Bytes;
			L.InFlightEntries += 1;
			Entry.ChargedBytes = Bytes;
			Entry.State = EResidencyState::InFlight;
		}

		// Admits Waiting entries, FIFO, while the head fits.
		void AdmitWaiting(FLedgerState& S)
		{
			while (!S.Waiting.empty())
			{
				const FResidencyEntryId Id = S.Waiting.front();
				FEntry& Entry = Entries[Id];
				if (Entry.WaitingBytes > FreeBytes(S.Ledger))
				{
					return;
				}
				S.Waiting.pop_front();
				S.OwedBytes -= Entry.WaitingBytes;
				S.Ledger.WaitingEntries -= 1;
				const uint64_t Bytes = Entry.WaitingBytes;
				Entry.WaitingBytes = 0;
				ChargeNow(Entry, Bytes);
				Admitted.push_back(Id);
			}
		}

		void RemoveWaiting(FLedgerState& S, FResidencyEntryId Id, FEntry& Entry)
		{
			S.Waiting.erase(std::find(S.Waiting.begin(), S.Waiting.end(), Id));
			S.OwedBytes -= Entry.WaitingBytes;
			S.Ledger.WaitingEntries -= 1;
			Entry.WaitingBytes = 0;
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
			Impl->AdmitWaiting(*S);
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

	EChargeOutcome FSuperSLMResidencyCore::Charge(FResidencyEntryId Id, uint64_t PeakBytes)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Unadmitted)
		{
			return EChargeOutcome::BudgetExhausted; // a broken precondition, not a refusal to count
		}
		FImpl::FLedgerState& S = Impl->LedgerOf(*Entry);
		FResidencyLedger& L = S.Ledger;

		// Fits now, and no earlier admission is waiting ahead of it.
		if (S.Waiting.empty() && PeakBytes <= FImpl::FreeBytes(L))
		{
			Impl->ChargeNow(*Entry, PeakBytes);
			return EChargeOutcome::Charged;
		}

		// What it can have once every Releasing byte has come back and every earlier Waiting admission
		// has been served. Resident and InFlight bytes stay unless evicted.
		const uint64_t Held = L.ResidentBytes + L.InFlightBytes + S.OwedBytes;
		const uint64_t Available = L.BudgetBytes > Held ? L.BudgetBytes - Held : 0;
		std::vector<FResidencyEntryId> ToEvict;
		if (PeakBytes > Available)
		{
			// Evict least-recently-used unpinned entries, but only if that makes it fit.
			uint64_t Freed = 0;
			for (FResidencyEntryId Candidate : Impl->LruOrder(Entry->Kind, Entry->LedgerId))
			{
				if (Available + Freed >= PeakBytes)
				{
					break;
				}
				ToEvict.push_back(Candidate);
				Freed += Impl->Entries[Candidate].ChargedBytes;
			}
			if (Available + Freed < PeakBytes)
			{
				++Impl->BudgetExhausted;
				return EChargeOutcome::BudgetExhausted;
			}
		}
		for (FResidencyEntryId Victim : ToEvict)
		{
			Impl->ToReleasing(Victim, Impl->Entries[Victim]);
			++Impl->Evictions;
		}

		// Evicted bytes stay charged until their release completes, so it waits for them.
		Entry->State = EResidencyState::Waiting;
		Entry->WaitingBytes = PeakBytes;
		S.Waiting.push_back(Id);
		S.OwedBytes += PeakBytes;
		L.WaitingEntries += 1;
		++Impl->Deferred;
		return EChargeOutcome::Deferred;
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
		Impl->AdmitWaiting(S);
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
			Impl->RemoveWaiting(S, Id, *Entry);
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
		Entry->State = EResidencyState::Unadmitted;
		Impl->AdmitWaiting(S);
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
		Impl->Entries.erase(Id);
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

	bool FSuperSLMResidencyCore::ReleaseCompleted(FResidencyEntryId Id)
	{
		FImpl::FEntry* Entry = Impl->Find(Id);
		if (Entry == nullptr || Entry->State != EResidencyState::Releasing)
		{
			return false;
		}
		FImpl::FLedgerState& S = Impl->LedgerOf(*Entry);
		S.Ledger.ReleasingBytes -= Entry->ChargedBytes;
		S.Ledger.ReleasingEntries -= 1;
		S.Ledger.ChargedBytes -= Entry->ChargedBytes;
		Impl->DetachAll(*Entry);
		Impl->Entries.erase(Id);
		Impl->AdmitWaiting(S);
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
		return Snap;
	}
}
