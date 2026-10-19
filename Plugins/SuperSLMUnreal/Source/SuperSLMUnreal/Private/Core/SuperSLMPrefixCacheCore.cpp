#include "SuperSLMPrefixCacheCore.h"
#include "SuperSLMCoreBytes.h"

#include <map>
#include <utility>

namespace SuperSLMCore
{
	const char* PrefixFallbackName(EPrefixFallback Reason)
	{
		switch (Reason)
		{
		case EPrefixFallback::WrapperTruncated: return "WrapperTruncated";
		case EPrefixFallback::WrapperBadMagic: return "WrapperBadMagic";
		case EPrefixFallback::WrapperUnsupportedVersion: return "WrapperUnsupportedVersion";
		case EPrefixFallback::WrapperBackendMismatch: return "WrapperBackendMismatch";
		case EPrefixFallback::WrapperLayer1TagMismatch: return "WrapperLayer1TagMismatch";
		case EPrefixFallback::WrapperLayer1CommitMismatch: return "WrapperLayer1CommitMismatch";
		case EPrefixFallback::WrapperArtifactMismatch: return "WrapperArtifactMismatch";
		case EPrefixFallback::WrapperKeyMismatch: return "WrapperKeyMismatch";
		case EPrefixFallback::WrapperMalformed: return "WrapperMalformed";
		case EPrefixFallback::WrapperSpecMismatch: return "WrapperSpecMismatch";
		case EPrefixFallback::MemorySpecMismatch: return "MemorySpecMismatch";
		case EPrefixFallback::PayloadRefused: return "PayloadRefused";
		case EPrefixFallback::UnknownSchema: return "UnknownSchema";
		case EPrefixFallback::BudgetExhausted: return "BudgetExhausted";
		case EPrefixFallback::PoolExhausted: return "PoolExhausted";
		case EPrefixFallback::DiskReadError: return "DiskReadError";
		case EPrefixFallback::DiskWriteError: return "DiskWriteError";
		case EPrefixFallback::NestedRequiresBudgetMode: return "NestedRequiresBudgetMode";
		case EPrefixFallback::ReleaseOfCacheHandle: return "ReleaseOfCacheHandle";
		case EPrefixFallback::AdoptOfEndedLease: return "AdoptOfEndedLease";
		case EPrefixFallback::ReturnNotOwned: return "ReturnNotOwned";
		case EPrefixFallback::BuildFailed: return "BuildFailed";
		case EPrefixFallback::RestoreFailed: return "RestoreFailed";
		case EPrefixFallback::SaveFailed: return "SaveFailed";
		case EPrefixFallback::Count: break;
		}
		return "Unknown";
	}

	namespace
	{
		// The fallback a failed wrapper check is counted and logged under.
		EPrefixFallback FallbackForWrapperCheck(EPrefixWrapperCheck Check)
		{
			switch (Check)
			{
			case EPrefixWrapperCheck::Truncated: return EPrefixFallback::WrapperTruncated;
			case EPrefixWrapperCheck::BadMagic: return EPrefixFallback::WrapperBadMagic;
			case EPrefixWrapperCheck::UnsupportedVersion: return EPrefixFallback::WrapperUnsupportedVersion;
			case EPrefixWrapperCheck::BackendMismatch: return EPrefixFallback::WrapperBackendMismatch;
			case EPrefixWrapperCheck::Layer1TagMismatch: return EPrefixFallback::WrapperLayer1TagMismatch;
			case EPrefixWrapperCheck::Layer1CommitMismatch: return EPrefixFallback::WrapperLayer1CommitMismatch;
			case EPrefixWrapperCheck::ArtifactMismatch: return EPrefixFallback::WrapperArtifactMismatch;
			case EPrefixWrapperCheck::KeyMismatch: return EPrefixFallback::WrapperKeyMismatch;
			case EPrefixWrapperCheck::SpecMismatch: return EPrefixFallback::WrapperSpecMismatch;
			case EPrefixWrapperCheck::Ok:
			case EPrefixWrapperCheck::Malformed: break;
			}
			return EPrefixFallback::WrapperMalformed;
		}
	}

	struct FSuperSLMPrefixCacheCore::FImpl
	{
		// Where an entry is in §5.3's lookup. Tiers are tried in this order.
		enum class ETier : uint8_t
		{
			Cooked,
			Disk,
			Recompute,
		};

		// The prefix-specific half of one residency entry.
		struct FEntry
		{
			FPrefixKey Key;
			bool bKeyed = true;           // false: a memory-tier collision's private entry, never written back
			FPrefixStoredSpec Spec;
			int32_t Length = 0;
			ETier Tier = ETier::Cooked;    // the tier being tried, or the one that served it
			uint64_t CookedBlobId = 0;     // the cooked blob being tried
			EPrefixCommandKind PendingOp = EPrefixCommandKind::Build; // Restore or Build, once charged or waiting
			uint64_t PayloadRef = 0;       // a Restore's bytes, held while its admission waits
			uint64_t EngineHandle = 0;     // when Resident
			FResidencyEntryId RetryAfter = kInvalidResidencyEntry; // pool-exhausted: the eviction it waits for
			std::vector<int64_t> Leases;   // Live leases attached to it
		};

		// One per handle minted since Configure(), kept after the lease ends so an ended handle stays
		// told apart from one the cache never minted. Compact: the spec lives in PendingSpecs.
		struct FLease
		{
			EPrefixLeaseState State = EPrefixLeaseState::Pending;
			EPrefixSource Source = EPrefixSource::Pending;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
		};

		// A command with the entry it works for (none for ComputeKey and the file-only commands).
		struct FIssued
		{
			FPrefixCommand Command;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
		};

		struct FCooked
		{
			FPrefixKey Key;
			uint64_t BlobId = 0;
			bool bStale = false;
		};

		explicit FImpl(const FResidencyHandleCounter& Counter) : Residency(1, Counter) {}

		FSuperSLMResidencyCore Residency;
		FPrefixCacheConfig Config;
		bool bEverConfigured = false;
		bool bConfigured = false;
		bool bTearingDown = false;
		bool bTeardownSwept = false;

		std::map<FResidencyEntryId, FEntry> Entries;
		std::map<int64_t, FLease> Leases;
		std::map<int64_t, FPrefixStoredSpec> PendingSpecs; // a lease's spec copy, until its key is applied
		std::map<uint64_t, FResidencyEntryId> OpenAdopts; // adopt token -> entry
		std::vector<FCooked> Cooked;
		std::vector<FIssued> Outbox;
		std::map<uint64_t, FIssued> InFlight;
		uint64_t NextCommandId = 1;
		uint64_t NextWriteSerial = 1;
		uint64_t NextAdoptToken = 1;

		FPrefixCacheDiagnostics Diag;
		std::vector<FPrefixLogLine> Log;

		// --- Small helpers ------------------------------------------------------------------------

		uint64_t PagesFor(int32_t Length) const { return CoreBytes::CeilDiv(static_cast<uint64_t>(Length), Config.PageTokens); }

		// R(L) = min(ceil(L/B) + 1, ceil(cap/B)): begin_budgeted's peak draw (§5.4).
		uint64_t BuildPagesFor(int32_t Length) const
		{
			const uint64_t CapPages = CoreBytes::CeilDiv(static_cast<uint64_t>(Config.ContextCap), Config.PageTokens);
			const uint64_t Pages = PagesFor(Length) + 1;
			return Pages < CapPages ? Pages : CapPages;
		}

		void Fallback(EPrefixFallback Reason, const std::string& Detail)
		{
			++Diag.Fallbacks[static_cast<uint32_t>(Reason)];
			FPrefixLogLine Line;
			Line.Reason = Reason;
			Line.Message = std::string("Prefix cache: ") + PrefixFallbackName(Reason) + (Detail.empty() ? std::string() : ": " + Detail);
			Log.push_back(Line);
		}

		std::string KeyText(const FEntry& Entry) const { return "key " + PrefixKeyToHex(Entry.Key); }

		FPrefixCommand& Emit(EPrefixCommandKind Kind, FResidencyEntryId Entry = kInvalidResidencyEntry)
		{
			FIssued Issued;
			Issued.Command.Id = NextCommandId++;
			Issued.Command.Kind = Kind;
			Issued.Entry = Entry;
			Outbox.push_back(Issued);
			return Outbox.back().Command;
		}

		void EmitDropPayload(uint64_t PayloadRef)
		{
			if (PayloadRef != 0)
			{
				Emit(EPrefixCommandKind::DropPayload).PayloadRef = PayloadRef;
			}
		}

		void EmitKeyed(EPrefixCommandKind Kind, const FPrefixKey& Key)
		{
			Emit(Kind).Key = Key;
		}

		// --- Leases -------------------------------------------------------------------------------

		void ResolveLease(int64_t Handle, EPrefixLeaseState State, EPrefixSource Source)
		{
			FLease& Lease = Leases[Handle];
			Lease.State = State;
			if (State == EPrefixLeaseState::Ready)
			{
				Lease.Source = Source;
				++Diag.ServedBySource[static_cast<uint32_t>(Source)];
			}
		}

		void DetachFromEntry(int64_t Handle)
		{
			FLease& Lease = Leases[Handle];
			const auto It = Entries.find(Lease.Entry);
			if (It != Entries.end())
			{
				std::vector<int64_t>& List = It->second.Leases;
				for (size_t I = 0; I < List.size(); ++I)
				{
					if (List[I] == Handle)
					{
						List.erase(List.begin() + static_cast<std::ptrdiff_t>(I));
						break;
					}
				}
			}
			Residency.DetachLease(Handle);
			Lease.Entry = kInvalidResidencyEntry;
		}

		void Attach(int64_t Handle, FResidencyEntryId Id)
		{
			Residency.AttachLease(Handle, Id);
			Leases[Handle].Entry = Id;
			Entries[Id].Leases.push_back(Handle);
		}

		// --- Entries ------------------------------------------------------------------------------

		// The entry is gone without a resident handle: every lease on it resolves to State, its charge is
		// returned, and it is destroyed.
		void Resolve(FResidencyEntryId Id, EPrefixLeaseState State)
		{
			FEntry& Entry = Entries[Id];
			const std::vector<int64_t> Waiting = Entry.Leases;
			for (int64_t Handle : Waiting)
			{
				DetachFromEntry(Handle);
				ResolveLease(Handle, State, EPrefixSource::Pending);
			}
			EmitDropPayload(Entry.PayloadRef);
			Residency.ReturnCharge(Id);
			Residency.Destroy(Id);
			Entries.erase(Id);
		}

		void BecomeReady(FResidencyEntryId Id, uint64_t EngineHandle, EPrefixSource Source, uint64_t SteadyBytes)
		{
			FEntry& Entry = Entries[Id];
			Residency.Settle(Id, SteadyBytes);
			Entry.EngineHandle = EngineHandle;
			if (bTearingDown)
			{
				return; // held only until the teardown sweep releases it
			}
			for (int64_t Handle : Entry.Leases)
			{
				ResolveLease(Handle, EPrefixLeaseState::Ready, Source);
			}
			if (Source == EPrefixSource::Disk)
			{
				EmitKeyed(EPrefixCommandKind::TouchDiskFile, Entry.Key); // a hit refreshes the trim's age (§5.7)
			}
			if (Source == EPrefixSource::Recomputed && Config.bDiskEnabled && Entry.bKeyed)
			{
				Residency.AddPin(Id, EResidencyPin::Save);
				Emit(EPrefixCommandKind::Save, Id).EngineHandle = EngineHandle;
			}
		}

		// Tries the entry's current tier, then each later one (§5.3).
		void TryTier(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			if (Entry.Tier == ETier::Cooked)
			{
				for (const FCooked& Blob : Cooked)
				{
					if (!Blob.bStale && Blob.Key == Entry.Key)
					{
						Entry.CookedBlobId = Blob.BlobId;
						FPrefixCommand& C = Emit(EPrefixCommandKind::CheckCooked, Id);
						C.Key = Entry.Key;
						C.Spec = Entry.Spec;
						C.CookedBlobId = Blob.BlobId;
						return;
					}
				}
				Entry.Tier = ETier::Disk;
			}
			if (Entry.Tier == ETier::Disk)
			{
				if (Config.bDiskEnabled)
				{
					FPrefixCommand& C = Emit(EPrefixCommandKind::ReadDisk, Id);
					C.Key = Entry.Key;
					C.Spec = Entry.Spec;
					return;
				}
				Entry.Tier = ETier::Recompute;
			}
			Entry.PendingOp = EPrefixCommandKind::Build;
			Admit(Id, BuildPagesFor(Entry.Length) * Config.PageBytes);
		}

		void NextTier(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			Entry.Tier = Entry.Tier == ETier::Cooked ? ETier::Disk : ETier::Recompute;
			TryTier(Id);
		}

		// A cooked or disk tier that failed: the cooked blob is marked stale or the file removed (§5.7).
		// A collision's unkeyed entry changes nothing stored under the key: those bytes are the keyed
		// entry's to judge.
		void TierMissed(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			if (Entry.bKeyed && Entry.Tier == ETier::Cooked)
			{
				for (FCooked& Blob : Cooked)
				{
					if (Blob.BlobId == Entry.CookedBlobId)
					{
						Blob.bStale = true;
					}
				}
			}
			else if (Entry.bKeyed)
			{
				EmitKeyed(EPrefixCommandKind::RemoveDiskFile, Entry.Key);
			}
			NextTier(Id);
		}

		// Charges the peak, on the game thread, before the restore or build is emitted (§5.4).
		void Admit(FResidencyEntryId Id, uint64_t PeakBytes)
		{
			const EChargeOutcome Outcome = Residency.Charge(Id, PeakBytes);
			if (Outcome == EChargeOutcome::Charged)
			{
				IssueLoad(Id);
			}
			else if (Outcome == EChargeOutcome::BudgetExhausted)
			{
				Fallback(EPrefixFallback::BudgetExhausted, KeyText(Entries[Id]));
				Resolve(Id, EPrefixLeaseState::BudgetExhausted);
			}
			// Deferred: IssueLoad runs when the residency core admits it (release before reuse).
		}

		void IssueLoad(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			FPrefixCommand& C = Emit(Entry.PendingOp, Id);
			C.ContextLength = Entry.Length;
			if (Entry.PendingOp == EPrefixCommandKind::Restore)
			{
				C.PayloadRef = Entry.PayloadRef;
				Entry.PayloadRef = 0; // the Restore owns the bytes now
				++Diag.RestoresIssued;
			}
			else
			{
				C.Spec = Entry.Spec;
				++Diag.BuildsIssued;
			}
		}

		void StartRestore(FResidencyEntryId Id, uint64_t PayloadRef)
		{
			FEntry& Entry = Entries[Id];
			Entry.PendingOp = EPrefixCommandKind::Restore;
			Entry.PayloadRef = PayloadRef;
			// A root restore: ceil(L/B) - s with s = 0. The engine's s is applied at Settle.
			Admit(Id, PagesFor(Entry.Length) * Config.PageBytes);
		}

		// §5.4 "the pool is the hard limit": evict the next LRU unpinned entry and retry once its release
		// completes, or resolve PoolExhausted when nothing is evictable.
		void PoolExhausted(FResidencyEntryId Id)
		{
			FResidencyEntryId Victim = kInvalidResidencyEntry;
			if (Residency.EvictLru(kResidencyKindPrefix, kResidencyLedgerHostRam, Victim))
			{
				Entries[Id].RetryAfter = Victim;
				return;
			}
			Fallback(EPrefixFallback::PoolExhausted, KeyText(Entries[Id]));
			Resolve(Id, EPrefixLeaseState::PoolExhausted);
		}

		void Retry(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			Entry.RetryAfter = kInvalidResidencyEntry;
			if (Entry.PendingOp == EPrefixCommandKind::Build)
			{
				IssueLoad(Id); // still charged at its peak
				return;
			}
			// The refused restore consumed its bytes: read the same tier again, and charge again on success.
			Residency.ReturnCharge(Id);
			TryTier(Id);
		}

		// Turns the residency core's evictions into Release commands and its deferred admissions into loads.
		void Drain()
		{
			for (;;)
			{
				const std::vector<FResidencyEntryId> Evicted = Residency.TakeEvicted();
				const std::vector<FResidencyEntryId> Admitted = Residency.TakeAdmitted();
				if (Evicted.empty() && Admitted.empty())
				{
					return;
				}
				for (FResidencyEntryId Id : Evicted)
				{
					const auto It = Entries.find(Id);
					if (It != Entries.end())
					{
						Emit(EPrefixCommandKind::Release, Id).EngineHandle = It->second.EngineHandle;
						++Diag.ReleasesIssued;
					}
				}
				for (FResidencyEntryId Id : Admitted)
				{
					if (Entries.count(Id) == 0)
					{
						continue; // resolved since it was admitted
					}
					if (bTearingDown)
					{
						Resolve(Id, EPrefixLeaseState::Cancelled);
					}
					else
					{
						IssueLoad(Id);
					}
				}
			}
		}

		// --- Completions --------------------------------------------------------------------------

		void OnKey(int64_t Handle, const FPrefixKey& Key)
		{
			const auto LeaseIt = Leases.find(Handle);
			if (bTearingDown || LeaseIt == Leases.end() || LeaseIt->second.State != EPrefixLeaseState::Pending)
			{
				return; // cancelled or returned while its key was computed
			}
			// The lease's copy of the spec is needed only until here.
			FPrefixStoredSpec Spec;
			Spec = std::move(PendingSpecs[Handle]);
			PendingSpecs.erase(Handle);
			const FResidencyEntryId Found = Residency.FindByKey(kResidencyKindPrefix, Key);
			if (Found != kInvalidResidencyEntry)
			{
				// The memory tier's O(tokens) spec compare at the hit or attach (§5.2, rev 1.1).
				if (Entries[Found].Spec == Spec)
				{
					Attach(Handle, Found);
					Residency.Touch(Found);
					if (Residency.GetState(Found) == EResidencyState::Resident)
					{
						ResolveLease(Handle, EPrefixLeaseState::Ready, EPrefixSource::Memory);
					}
					return;
				}
				++Diag.SpecMismatches;
				Fallback(EPrefixFallback::MemorySpecMismatch, "key " + PrefixKeyToHex(Key));
			}

			// A miss. A collision gets a private, unkeyed entry: it tries every tier like any miss, but is
			// never written back, so the colliding key's entry and file are left alone.
			const bool bKeyed = Found == kInvalidResidencyEntry;
			const FResidencyEntryId Id = Residency.CreateEntry(kResidencyKindPrefix, kResidencyLedgerHostRam, Key, bKeyed);
			FEntry& Entry = Entries[Id];
			Entry.Key = Key;
			Entry.bKeyed = bKeyed;
			Entry.Length = static_cast<int32_t>(Spec.Tokens.size());
			Entry.Spec = std::move(Spec);
			Entry.Tier = ETier::Cooked;
			Attach(Handle, Id);
			TryTier(Id);
		}

		void OnWrapperRead(FResidencyEntryId Id, const FPrefixCompletion& Done, bool bDisk)
		{
			if (bDisk && Done.Io != EPrefixIoStatus::Ok)
			{
				if (Done.Io == EPrefixIoStatus::Error)
				{
					Fallback(EPrefixFallback::DiskReadError, KeyText(Entries[Id])); // a miss; nothing is deleted
				}
				NextTier(Id);
				return;
			}
			if (Done.Wrapper != EPrefixWrapperCheck::Ok)
			{
				if (Done.Wrapper == EPrefixWrapperCheck::SpecMismatch)
				{
					++Diag.SpecMismatches;
				}
				Fallback(FallbackForWrapperCheck(Done.Wrapper), KeyText(Entries[Id]));
				TierMissed(Id);
				return;
			}
			StartRestore(Id, Done.PayloadRef);
		}

		void OnRestore(FResidencyEntryId Id, const FPrefixCompletion& Done)
		{
			FEntry& Entry = Entries[Id];
			if (Done.Engine == EPrefixEngineResult::Ok)
			{
				const uint64_t Pages = PagesFor(Entry.Length);
				const uint64_t Shared = Done.SharedPages < Pages ? Done.SharedPages : Pages;
				BecomeReady(Id, Done.EngineHandle, Entry.Tier == ETier::Cooked ? EPrefixSource::Cooked : EPrefixSource::Disk,
					(Pages - Shared) * Config.PageBytes);
				return;
			}
			if (bTearingDown)
			{
				Resolve(Id, EPrefixLeaseState::Cancelled);
				return;
			}
			if (Done.Engine == EPrefixEngineResult::PoolExhausted)
			{
				PoolExhausted(Id);
				return;
			}
			Residency.ReturnCharge(Id);
			const std::string Detail = KeyText(Entry) + ", status " + std::to_string(Done.EngineStatus);
			if (Done.Engine == EPrefixEngineResult::Refused)
			{
				// The engine refused the bytes (digest, producer, model, ...): a miss, and the bytes go (§5.9).
				Fallback(EPrefixFallback::PayloadRefused, Detail);
				TierMissed(Id);
				return;
			}
			// Any other engine failure says nothing about the bytes: a miss that keeps them.
			Fallback(EPrefixFallback::RestoreFailed, Detail);
			NextTier(Id);
		}

		void OnBuild(FResidencyEntryId Id, const FPrefixCompletion& Done)
		{
			FEntry& Entry = Entries[Id];
			if (Done.Engine == EPrefixEngineResult::Ok)
			{
				// At the freeze the charge falls from R(L) to ceil(L/B).
				BecomeReady(Id, Done.EngineHandle, EPrefixSource::Recomputed, PagesFor(Entry.Length) * Config.PageBytes);
				return;
			}
			if (bTearingDown)
			{
				Resolve(Id, EPrefixLeaseState::Cancelled);
				return;
			}
			switch (Done.Engine)
			{
			case EPrefixEngineResult::PoolExhausted:
				PoolExhausted(Id);
				return;
			case EPrefixEngineResult::UnknownSchema:
				Fallback(EPrefixFallback::UnknownSchema, KeyText(Entry) + ", schema \"" + Entry.Spec.SchemaName + "\"");
				Resolve(Id, EPrefixLeaseState::UnknownSchema);
				return;
			default:
				Fallback(EPrefixFallback::BuildFailed, KeyText(Entry) + ", status " + std::to_string(Done.EngineStatus));
				Resolve(Id, EPrefixLeaseState::Failed);
				return;
			}
		}

		void OnSave(FResidencyEntryId Id, const FPrefixCompletion& Done)
		{
			Residency.RemovePin(Id, EResidencyPin::Save);
			if (Done.Engine != EPrefixEngineResult::Ok || Done.PayloadRef == 0)
			{
				if (!bTearingDown)
				{
					Fallback(EPrefixFallback::SaveFailed, KeyText(Entries[Id]) + ", status " + std::to_string(Done.EngineStatus));
				}
				return;
			}
			if (bTearingDown)
			{
				EmitDropPayload(Done.PayloadRef);
				return;
			}
			const FEntry& Entry = Entries[Id];
			FPrefixCommand& C = Emit(EPrefixCommandKind::WriteDisk);
			C.Key = Entry.Key;
			C.Spec = Entry.Spec;
			C.PayloadRef = Done.PayloadRef;
			C.WriteSerial = NextWriteSerial++;
			++Diag.DiskWritesIssued;
		}

		void OnRelease(FResidencyEntryId Id)
		{
			Residency.ReleaseCompleted(Id);
			Entries.erase(Id);
			if (bTearingDown)
			{
				return;
			}
			for (auto& Pair : Entries)
			{
				if (Pair.second.RetryAfter == Id)
				{
					Retry(Pair.first);
					break;
				}
			}
		}

		// While tearing down, every completion only finishes what it started.
		void OnTeardownCompletion(const FIssued& Issued, const FPrefixCompletion& Done)
		{
			const FResidencyEntryId Id = Issued.Entry;
			switch (Issued.Command.Kind)
			{
			case EPrefixCommandKind::CheckCooked:
			case EPrefixCommandKind::ReadDisk:
				EmitDropPayload(Done.PayloadRef);
				Resolve(Id, EPrefixLeaseState::Cancelled);
				break;
			case EPrefixCommandKind::Restore: OnRestore(Id, Done); break;
			case EPrefixCommandKind::Build: OnBuild(Id, Done); break;
			case EPrefixCommandKind::Save: OnSave(Id, Done); break;
			case EPrefixCommandKind::Release: OnRelease(Id); break;
			default: break;
			}
		}

		// --- Teardown -----------------------------------------------------------------------------

		// Once nothing is in flight and no adopt is open, one Release per engine handle held (§5.4).
		void AdvanceTeardown()
		{
			if (!bTearingDown || bTeardownSwept || !InFlight.empty() || !OpenAdopts.empty())
			{
				return;
			}
			bTeardownSwept = true;
			// A Ready lease's handle is about to be released: it resolves Cancelled, and may still be returned.
			for (auto& Pair : Leases)
			{
				if (Pair.second.State == EPrefixLeaseState::Ready)
				{
					DetachFromEntry(Pair.first);
					Pair.second.State = EPrefixLeaseState::Cancelled;
				}
			}
			for (const auto& Pair : Entries)
			{
				Residency.EvictForTeardown(Pair.first);
			}
			Drain();
		}

		bool IsTornDown() const
		{
			return bTearingDown && bTeardownSwept && InFlight.empty() && Outbox.empty() && Entries.empty();
		}
	};

	FSuperSLMPrefixCacheCore::FSuperSLMPrefixCacheCore(const FResidencyHandleCounter& PrefixIdCounter) : Impl(new FImpl(PrefixIdCounter)) {}
	FSuperSLMPrefixCacheCore::~FSuperSLMPrefixCacheCore() = default;

	bool FSuperSLMPrefixCacheCore::Configure(const FPrefixCacheConfig& Config)
	{
		FImpl& I = *Impl;
		if (I.bEverConfigured && !I.IsTornDown())
		{
			return false;
		}
		if (Config.PageBytes == 0 || Config.PageTokens == 0 || Config.ContextCap <= 0 || Config.VocabSize <= 0)
		{
			return false;
		}
		I.Residency.Reset();
		I.Residency.SetBudget(kResidencyKindPrefix, kResidencyLedgerHostRam, Config.BudgetBytes);
		I.Config = Config;
		I.Entries.clear();
		I.Leases.clear();
		I.PendingSpecs.clear();
		I.OpenAdopts.clear();
		I.Outbox.clear();
		I.InFlight.clear();
		I.Diag = FPrefixCacheDiagnostics();
		I.Log.clear();
		// Cooked sets stay registered; a new identity may make a stale blob valid again.
		for (FImpl::FCooked& Blob : I.Cooked)
		{
			Blob.bStale = false;
		}
		I.bEverConfigured = true;
		I.bConfigured = true;
		I.bTearingDown = false;
		I.bTeardownSwept = false;
		if (Config.bDiskEnabled)
		{
			I.Emit(EPrefixCommandKind::TrimDisk); // §5.7: trimmed at Configure()
		}
		return true;
	}

	bool FSuperSLMPrefixCacheCore::IsConfigured() const
	{
		return Impl->bConfigured && !Impl->bTearingDown;
	}

	EPrefixRequestResult FSuperSLMPrefixCacheCore::Request(const FPrefixSpec& Spec, int64_t& OutHandle, std::string& OutError)
	{
		FImpl& I = *Impl;
		OutHandle = 0;
		if (!IsConfigured())
		{
			OutError = "the prefix cache is not configured";
			return EPrefixRequestResult::NotConfigured;
		}
		if (Spec.Tokens.empty())
		{
			OutError = "a prefix needs at least one token";
			return EPrefixRequestResult::Malformed;
		}
		if (static_cast<int64_t>(Spec.Tokens.size()) > I.Config.ContextCap)
		{
			OutError = "the prefix is longer than the model's context cap";
			return EPrefixRequestResult::Malformed;
		}
		for (int32_t Token : Spec.Tokens)
		{
			if (Token < 0 || Token >= I.Config.VocabSize)
			{
				OutError = "token " + std::to_string(Token) + " is outside the vocabulary";
				return EPrefixRequestResult::Malformed;
			}
		}
		if (Spec.ParentHandle != 0)
		{
			OutError = "nested prefixes need the plugin's budget mode";
			I.Fallback(EPrefixFallback::NestedRequiresBudgetMode, std::string());
			return EPrefixRequestResult::NestedRequiresBudgetMode;
		}

		OutHandle = I.Residency.MintLease();
		I.Leases[OutHandle] = FImpl::FLease();
		FPrefixStoredSpec& Copy = I.PendingSpecs[OutHandle];
		Copy.SchemaName = Spec.SchemaName;
		Copy.Tokens = Spec.Tokens;
		++I.Diag.Requests;

		// Hashing is pool-thread work (§5.5): the key comes back as a completion.
		FPrefixCommand& C = I.Emit(EPrefixCommandKind::ComputeKey);
		C.LeaseHandle = OutHandle;
		FPrefixKeyInputs& In = C.KeyInputs;
		In.ArtifactHash = I.Config.Identity.ArtifactHash;
		In.Layer1Tag = I.Config.Identity.Layer1Tag;
		In.Layer1Commit = I.Config.Identity.Layer1Commit;
		In.Backend = I.Config.Identity.Backend;
		In.AdapterVendorId = I.Config.Identity.AdapterVendorId;
		In.AdapterDeviceId = I.Config.Identity.AdapterDeviceId;
		In.SchemaName = Spec.SchemaName;
		In.Tokens = Spec.Tokens;
		return EPrefixRequestResult::Accepted;
	}

	bool FSuperSLMPrefixCacheCore::Return(int64_t Handle)
	{
		FImpl& I = *Impl;
		const auto It = I.Leases.find(Handle);
		if (It == I.Leases.end() || It->second.State == EPrefixLeaseState::Ended)
		{
			I.Fallback(EPrefixFallback::ReturnNotOwned, "handle " + std::to_string(Handle));
			return false;
		}
		I.DetachFromEntry(Handle);
		I.Residency.EndLease(Handle);
		I.PendingSpecs.erase(Handle);
		It->second.State = EPrefixLeaseState::Ended;
		return true;
	}

	EPrefixLeaseState FSuperSLMPrefixCacheCore::GetLeaseState(int64_t Handle) const
	{
		const auto It = Impl->Leases.find(Handle);
		return It == Impl->Leases.end() ? EPrefixLeaseState::Unknown : It->second.State;
	}

	EPrefixSource FSuperSLMPrefixCacheCore::GetSource(int64_t Handle) const
	{
		// The tier that served it once Ready (kept after it ends); Pending until then, or if it never was.
		const auto It = Impl->Leases.find(Handle);
		return It == Impl->Leases.end() ? EPrefixSource::None : It->second.Source;
	}

	bool FSuperSLMPrefixCacheCore::RefuseRelease(int64_t Handle, std::string& OutError)
	{
		if (Impl->Leases.count(Handle) == 0)
		{
			return false;
		}
		OutError = "owned by the prefix cache; call ReturnPrefix";
		Impl->Fallback(EPrefixFallback::ReleaseOfCacheHandle, "handle " + std::to_string(Handle));
		return true;
	}

	EPrefixAdoptCheck FSuperSLMPrefixCacheCore::BeginAdopt(int64_t Handle, uint64_t& OutEngineHandle, uint64_t& OutAdoptToken)
	{
		FImpl& I = *Impl;
		OutEngineHandle = 0;
		OutAdoptToken = 0;
		const auto It = I.Leases.find(Handle);
		if (It == I.Leases.end())
		{
			return EPrefixAdoptCheck::NotCacheHandle;
		}
		if (It->second.State == EPrefixLeaseState::Ended)
		{
			I.Fallback(EPrefixFallback::AdoptOfEndedLease, "handle " + std::to_string(Handle));
			return EPrefixAdoptCheck::PrefixLeaseEnded;
		}
		if (It->second.State != EPrefixLeaseState::Ready || I.bTearingDown)
		{
			return EPrefixAdoptCheck::NotReady;
		}
		const FResidencyEntryId Entry = It->second.Entry;
		I.Residency.AddPin(Entry, EResidencyPin::Adopt);
		OutEngineHandle = I.Entries[Entry].EngineHandle;
		OutAdoptToken = I.NextAdoptToken++;
		I.OpenAdopts[OutAdoptToken] = Entry;
		return EPrefixAdoptCheck::Adoptable;
	}

	bool FSuperSLMPrefixCacheCore::EndAdopt(uint64_t AdoptToken)
	{
		FImpl& I = *Impl;
		const auto It = I.OpenAdopts.find(AdoptToken);
		if (It == I.OpenAdopts.end())
		{
			return false;
		}
		I.Residency.RemovePin(It->second, EResidencyPin::Adopt);
		I.OpenAdopts.erase(It);
		I.AdvanceTeardown();
		return true;
	}

	void FSuperSLMPrefixCacheCore::RegisterCooked(const FPrefixKey& Key, uint64_t CookedBlobId)
	{
		FImpl::FCooked Blob;
		Blob.Key = Key;
		Blob.BlobId = CookedBlobId;
		Impl->Cooked.push_back(Blob);
	}

	void FSuperSLMPrefixCacheCore::UnregisterCooked(uint64_t CookedBlobId)
	{
		std::vector<FImpl::FCooked>& Cooked = Impl->Cooked;
		for (size_t I = Cooked.size(); I-- > 0;)
		{
			if (Cooked[I].BlobId == CookedBlobId)
			{
				Cooked.erase(Cooked.begin() + static_cast<std::ptrdiff_t>(I));
			}
		}
	}

	bool FSuperSLMPrefixCacheCore::IsCookedStale(uint64_t CookedBlobId) const
	{
		for (const FImpl::FCooked& Blob : Impl->Cooked)
		{
			if (Blob.BlobId == CookedBlobId)
			{
				return Blob.bStale;
			}
		}
		return false;
	}

	std::vector<FPrefixCommand> FSuperSLMPrefixCacheCore::TakeCommands()
	{
		FImpl& I = *Impl;
		std::vector<FPrefixCommand> Taken;
		Taken.reserve(I.Outbox.size());
		for (const FImpl::FIssued& Issued : I.Outbox)
		{
			Taken.push_back(Issued.Command);
			I.InFlight[Issued.Command.Id] = Issued;
		}
		I.Outbox.clear();
		return Taken;
	}

	void FSuperSLMPrefixCacheCore::Apply(const FPrefixCompletion& Completion)
	{
		FImpl& I = *Impl;
		const auto It = I.InFlight.find(Completion.CommandId);
		if (It == I.InFlight.end())
		{
			return;
		}
		const FImpl::FIssued Issued = It->second;
		I.InFlight.erase(It);
		const FResidencyEntryId Id = Issued.Entry;

		if (I.bTearingDown)
		{
			I.OnTeardownCompletion(Issued, Completion);
		}
		else
		{
			switch (Issued.Command.Kind)
			{
			case EPrefixCommandKind::ComputeKey: I.OnKey(Issued.Command.LeaseHandle, Completion.Key); break;
			case EPrefixCommandKind::CheckCooked: I.OnWrapperRead(Id, Completion, false); break;
			case EPrefixCommandKind::ReadDisk: I.OnWrapperRead(Id, Completion, true); break;
			case EPrefixCommandKind::Restore: I.OnRestore(Id, Completion); break;
			case EPrefixCommandKind::Build: I.OnBuild(Id, Completion); break;
			case EPrefixCommandKind::Save: I.OnSave(Id, Completion); break;
			case EPrefixCommandKind::WriteDisk:
				if (Completion.Io == EPrefixIoStatus::Ok)
				{
					I.Emit(EPrefixCommandKind::TrimDisk); // §5.7: trimmed after writes
				}
				else
				{
					I.Fallback(EPrefixFallback::DiskWriteError, "key " + PrefixKeyToHex(Issued.Command.Key));
				}
				break;
			case EPrefixCommandKind::Release: I.OnRelease(Id); break;
			default: break;
			}
		}
		I.Drain();
		I.AdvanceTeardown();
	}

	uint32_t FSuperSLMPrefixCacheCore::InFlightCommands() const
	{
		return static_cast<uint32_t>(Impl->InFlight.size());
	}

	void FSuperSLMPrefixCacheCore::BeginTeardown()
	{
		FImpl& I = *Impl;
		if (!I.bConfigured || I.bTearingDown)
		{
			return;
		}
		I.bTearingDown = true;

		// Every pending lease resolves by name.
		for (auto& Pair : I.Leases)
		{
			if (Pair.second.State == EPrefixLeaseState::Pending)
			{
				I.DetachFromEntry(Pair.first);
				Pair.second.State = EPrefixLeaseState::Cancelled;
				I.PendingSpecs.erase(Pair.first);
			}
		}

		// Commands not yet taken are dropped, except releases and payload drops. What they would have
		// done is undone here: a load's charge returned, a save's pin removed, held bytes dropped.
		std::vector<FImpl::FIssued> Kept;
		const std::vector<FImpl::FIssued> Outbox = I.Outbox;
		I.Outbox.clear();
		for (const FImpl::FIssued& Issued : Outbox)
		{
			const FPrefixCommand& C = Issued.Command;
			switch (C.Kind)
			{
			case EPrefixCommandKind::Release:
			case EPrefixCommandKind::DropPayload:
				Kept.push_back(Issued);
				break;
			case EPrefixCommandKind::Restore:
				I.EmitDropPayload(C.PayloadRef);
				I.Resolve(Issued.Entry, EPrefixLeaseState::Cancelled);
				break;
			case EPrefixCommandKind::Build:
			case EPrefixCommandKind::CheckCooked:
			case EPrefixCommandKind::ReadDisk:
				I.Resolve(Issued.Entry, EPrefixLeaseState::Cancelled);
				break;
			case EPrefixCommandKind::Save:
				I.Residency.RemovePin(Issued.Entry, EResidencyPin::Save);
				break;
			case EPrefixCommandKind::WriteDisk:
				I.EmitDropPayload(C.PayloadRef);
				break;
			default:
				break;
			}
		}
		Kept.insert(Kept.end(), I.Outbox.begin(), I.Outbox.end());
		I.Outbox = Kept;

		// Entries with no command of their own in flight: waiting for admission, or for a pool retry.
		std::vector<FResidencyEntryId> Idle;
		for (const auto& Pair : I.Entries)
		{
			const EResidencyState State = I.Residency.GetState(Pair.first);
			if (State == EResidencyState::Waiting || Pair.second.RetryAfter != kInvalidResidencyEntry)
			{
				Idle.push_back(Pair.first);
			}
		}
		for (FResidencyEntryId Id : Idle)
		{
			I.Resolve(Id, EPrefixLeaseState::Cancelled);
		}
		I.Drain();
		I.AdvanceTeardown();
	}

	bool FSuperSLMPrefixCacheCore::IsTornDown() const
	{
		return Impl->IsTornDown();
	}

	FPrefixCacheDiagnostics FSuperSLMPrefixCacheCore::GetDiagnostics() const
	{
		FPrefixCacheDiagnostics D = Impl->Diag;
		const FResidencyLedger L = Impl->Residency.GetLedger(kResidencyKindPrefix, kResidencyLedgerHostRam);
		D.BudgetBytes = L.BudgetBytes;
		D.ChargedBytes = L.ChargedBytes;
		D.ResidentBytes = L.ResidentBytes;
		D.InFlightBytes = L.InFlightBytes;
		D.ReleasingBytes = L.ReleasingBytes;
		D.ResidentEntries = L.ResidentEntries;
		D.Evictions = Impl->Residency.Snapshot().Evictions;
		for (const auto& Pair : Impl->PendingSpecs)
		{
			D.SpecTokensHeld += Pair.second.Tokens.size();
		}
		for (const auto& Pair : Impl->Entries)
		{
			D.SpecTokensHeld += Pair.second.Spec.Tokens.size();
		}
		return D;
	}

	std::vector<FPrefixLogLine> FSuperSLMPrefixCacheCore::TakeLog()
	{
		std::vector<FPrefixLogLine> Out;
		Out.swap(Impl->Log);
		return Out;
	}

	const FSuperSLMResidencyCore& FSuperSLMPrefixCacheCore::Residency() const
	{
		return Impl->Residency;
	}
}
