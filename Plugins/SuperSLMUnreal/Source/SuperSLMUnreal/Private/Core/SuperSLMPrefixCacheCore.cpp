#include "SuperSLMPrefixCacheCore.h"
#include "SuperSLMCoreBytes.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
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
		case EPrefixFallback::ReleaseFailed: return "ReleaseFailed";
		case EPrefixFallback::DropDeleteFailed: return "DropDeleteFailed";
		case EPrefixFallback::PrefixSetModelMismatch: return "PrefixSetModelMismatch";
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
			// Nested (U6): the parent entry, its length (begin_from's origin), and whether this entry holds
			// the lookup pin on it (ChildOp, from the request until the charge decision).
			FResidencyEntryId ParentEntry = kInvalidResidencyEntry;
			int32_t Origin = 0;
			bool bLookupPin = false;
			// Explicit residency control (§5.6, rev 1.18). Warms attached to it (coalesced, no lease and no
			// pin); a MemoryAndDisk drop that found it in flight or waiting suppresses its write-back until a
			// later request or warm clears the mark; a mark application queued in the issue order.
			std::vector<uint64_t> Warms;
			bool bNoWriteBack = false;
			bool bMarkQueued = false;
		};

		// Request(), Warm() and Drop() each take a place in the issue order at the call; a mark application
		// takes one at the end of the pass that finds its entry unpinned (§5.6 *When a mark is applied*).
		enum class EIssueKind : uint8_t
		{
			Lease,
			Warm,
			Drop,
			MarkApp,
		};

		struct FIssueItem
		{
			EIssueKind Kind = EIssueKind::Lease;
			int64_t Lease = 0;
			uint64_t OpId = 0;
			FResidencyEntryId Entry = kInvalidResidencyEntry; // MarkApp
			bool bHashed = false;                              // its ComputeKey's completion has been applied
			FPrefixKey Key;
		};

		// A Warm or Drop op until it resolves (both parts, for a drop).
		struct FOp
		{
			bool bWarm = true;
			EPrefixDropScope Scope = EPrefixDropScope::Memory;
			FPrefixStoredSpec Spec;                              // until its key is applied
			FResidencyEntryId ParentEntry = kInvalidResidencyEntry; // a nested warm's lookup pin, until its key is applied
			FResidencyEntryId Entry = kInvalidResidencyEntry;    // a warm attached to an entry in flight or waiting
			FPrefixOpResult Result;
			bool bDeleteStarted = false;                         // a drop's disk part: the delete has been started
			FPrefixKey Key;                                      // a drop's key, once applied
		};

		// A drop's delete waiting for the write-back saves of its key that were in flight at its pass.
		struct FWaitingDelete
		{
			uint64_t OpId = 0;
			FPrefixKey Key;
			std::set<uint64_t> Saves;
		};


		// One per handle minted since Configure(), kept after the lease ends so an ended handle stays
		// told apart from one the cache never minted. Compact: the spec lives in PendingSpecs.
		struct FLease
		{
			EPrefixLeaseState State = EPrefixLeaseState::Pending;
			EPrefixSource Source = EPrefixSource::Pending;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
			bool bCancelledPending = false; // resolved Cancelled while Pending: reads Cancelled after Configure()
		};

		// A command with the entry it works for (none for ComputeKey and the file-only commands).
		struct FIssued
		{
			FPrefixCommand Command;
			FResidencyEntryId Entry = kInvalidResidencyEntry;
			uint64_t Arrival = 0; // a waiter's load: its arrival, so one pass's admissions keep that order
		};

		struct FCooked
		{
			FPrefixKey Key;
			uint64_t BlobId = 0;
			bool bStale = false;
		};

		// One key's disk commands (ReadDisk, WriteDisk, RemoveDiskFile), one at a time, in submission order.
		struct FKeyDisk
		{
			uint64_t Running = 0; // the command id emitted and not yet completed; 0 = none
			std::deque<FIssued> Queue;
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
		std::map<int64_t, FResidencyEntryId> PendingParents; // a nested lease's parent, lookup-pinned until its key is applied
		std::map<uint64_t, FResidencyEntryId> OpenAdopts; // adopt token -> entry
		std::vector<FCooked> Cooked;
		std::vector<FIssued> Outbox;
		std::map<uint64_t, FIssued> InFlight;
		uint64_t NextCommandId = 1;
		uint64_t NextWriteSerial = 1;
		uint64_t NextAdoptToken = 1;
		uint64_t Generation = 0;        // moves on at every Configure() and Abandon()
		uint64_t MirroredBytes = 0;     // budget mode: what region C holds for this cache

		// Explicit residency control (§5.6). Op ids and results outlive a configuration; the rest does not.
		std::deque<FIssueItem> IssueQueue;
		std::map<uint64_t, FOp> PendingOps;
		std::map<uint64_t, FPrefixOpResult> ResolvedOps;
		std::deque<uint64_t> ResolvedOrder;  // resolution order, oldest first; at most kPrefixOpRing
		// Leases of ended configurations that were cancelled while Pending (GetLeaseState); at most kPrefixOpRing.
		std::set<int64_t> EndedCancelledLeases;
		std::deque<int64_t> EndedCancelledOrder;
		uint64_t NextOpId = 1;               // never reset
		std::map<FPrefixKey, FKeyDisk> KeyDisks;
		std::map<uint64_t, FPrefixKey> SavesInFlight; // save command id -> its key (emitted, not yet completed)
		std::vector<FWaitingDelete> WaitingDeletes;
		bool bTrimDeferred = false;

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

		// A resolution's line also names each warm it resolves (§5.9, rev 1.18.4: the request's line, with "Warm"
		// and the op id in place of the handle, plus the key). One line per resolution, whoever waits on it.
		std::string ResolutionText(FResidencyEntryId Id) const
		{
			const auto It = Entries.find(Id);
			if (It == Entries.end())
			{
				return std::string();
			}
			std::string Text = KeyText(It->second);
			for (uint64_t Op : It->second.Warms)
			{
				Text += ", Warm op " + std::to_string(Op);
			}
			return Text;
		}

		FIssued Make(EPrefixCommandKind Kind, FResidencyEntryId Entry = kInvalidResidencyEntry)
		{
			FIssued Issued;
			Issued.Command.Id = NextCommandId++;
			Issued.Command.Kind = Kind;
			Issued.Command.Generation = Generation;
			Issued.Command.EntryId = Entry;
			Issued.Entry = Entry;
			return Issued;
		}

		FPrefixCommand& Emit(EPrefixCommandKind Kind, FResidencyEntryId Entry = kInvalidResidencyEntry)
		{
			Outbox.push_back(Make(Kind, Entry));
			return Outbox.back().Command;
		}

		// --- Per-key disk serialization (§5.6 *The disk part*, §5.7) --------------------------------

		static bool IsKeyDisk(EPrefixCommandKind Kind)
		{
			// The touch too (§5.7 rev 1.18.5): it opens the key's file, so a delete must not meet it.
			return Kind == EPrefixCommandKind::ReadDisk || Kind == EPrefixCommandKind::WriteDisk ||
				Kind == EPrefixCommandKind::RemoveDiskFile || Kind == EPrefixCommandKind::TouchDiskFile;
		}

		static bool IsDropDelete(const FPrefixCommand& C) { return C.Kind == EPrefixCommandKind::RemoveDiskFile && C.OpId != 0; }

		// Emitted and not yet completed (in the outbox or taken by the executor).
		template <typename TPred>
		bool AnyOut(TPred Pred) const
		{
			for (const FIssued& Issued : Outbox)
			{
				if (Pred(Issued.Command))
				{
					return true;
				}
			}
			for (const auto& Pair : InFlight)
			{
				if (Pred(Pair.second.Command))
				{
					return true;
				}
			}
			return false;
		}

		bool TrimOut() const
		{
			return AnyOut([](const FPrefixCommand& C) { return C.Kind == EPrefixCommandKind::TrimDisk; });
		}

		bool DropDeleteOut() const
		{
			return AnyOut([](const FPrefixCommand& C) { return IsDropDelete(C); });
		}

		// Emits the key's next command when none of the key's is out; a drop's delete also waits while a trim
		// is out.
		void PumpKey(std::map<FPrefixKey, FKeyDisk>::iterator It)
		{
			FKeyDisk& K = It->second;
			if (K.Running != 0)
			{
				return;
			}
			if (K.Queue.empty())
			{
				KeyDisks.erase(It);
				return;
			}
			if (IsDropDelete(K.Queue.front().Command) && TrimOut())
			{
				return;
			}
			K.Running = K.Queue.front().Command.Id;
			Outbox.push_back(K.Queue.front());
			K.Queue.pop_front();
		}

		void PumpAllKeys()
		{
			for (auto It = KeyDisks.begin(); It != KeyDisks.end();)
			{
				const auto Next = std::next(It);
				PumpKey(It);
				It = Next;
			}
		}

		// Queues a fully formed ReadDisk, WriteDisk or RemoveDiskFile on its key, emitting it when it is first.
		void SubmitKeyDisk(const FIssued& Issued)
		{
			KeyDisks[Issued.Command.Key].Queue.push_back(Issued);
			PumpKey(KeyDisks.find(Issued.Command.Key));
		}

		// A key's disk command completed (or was dropped before it ran): the key's next may go.
		void KeyDiskDone(const FPrefixCommand& C)
		{
			const auto It = KeyDisks.find(C.Key);
			if (It != KeyDisks.end() && It->second.Running == C.Id)
			{
				It->second.Running = 0;
			}
		}

		// A trim runs alone with respect to a drop's delete: deferred while one is out (§5.6, §5.7).
		void SubmitTrim()
		{
			if (bTearingDown)
			{
				return;
			}
			if (DropDeleteOut())
			{
				bTrimDeferred = true;
				return;
			}
			Emit(EPrefixCommandKind::TrimDisk);
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
			FIssued Issued = Make(Kind);
			Issued.Command.Key = Key;
			if (IsKeyDisk(Kind))
			{
				SubmitKeyDisk(Issued);
				return;
			}
			Outbox.push_back(Issued);
		}

		// --- Ops (§5.6) ------------------------------------------------------------------------------

		// A resolved op leaves the pending table for the ring of the last kPrefixOpRing resolved ops.
		void FinishOp(uint64_t OpId)
		{
			const auto It = PendingOps.find(OpId);
			if (It == PendingOps.end())
			{
				return;
			}
			ResolvedOps[OpId] = It->second.Result;
			ResolvedOrder.push_back(OpId);
			PendingOps.erase(It);
			while (ResolvedOrder.size() > kPrefixOpRing)
			{
				ResolvedOps.erase(ResolvedOrder.front());
				ResolvedOrder.pop_front();
			}
		}

		void ResolveWarm(uint64_t OpId, EPrefixOpResult Result)
		{
			const auto It = PendingOps.find(OpId);
			if (It == PendingOps.end() || !It->second.bWarm)
			{
				return;
			}
			It->second.Result.Result = Result;
			// §5.6 rev 1.18.5: a Failed warm names the reason its request logged; only a build fails a lease.
			It->second.Result.Reason = Result == EPrefixOpResult::Failed ? EPrefixFallback::BuildFailed : EPrefixFallback::Count;
			It->second.Entry = kInvalidResidencyEntry;
			++Diag.WarmsByResult[static_cast<uint32_t>(Result)];
			FinishOp(OpId);
		}

		// A lease state an entry resolves to, as a warm reads it (§5.6 rev 1.18.5). Failed (a build that failed for
		// any other reason) reads Failed, its Reason BuildFailed; Cancelled stays re-Configure() only.
		static EPrefixOpResult WarmResultOf(EPrefixLeaseState State)
		{
			switch (State)
			{
			case EPrefixLeaseState::BudgetExhausted: return EPrefixOpResult::BudgetExhausted;
			case EPrefixLeaseState::PoolExhausted: return EPrefixOpResult::PoolExhausted;
			case EPrefixLeaseState::UnknownSchema: return EPrefixOpResult::UnknownSchema;
			case EPrefixLeaseState::Failed: return EPrefixOpResult::Failed;
			default: break;
			}
			return EPrefixOpResult::Cancelled;
		}

		// A drop's part resolved: the op resolves once both have.
		void MaybeFinishDrop(uint64_t OpId)
		{
			const auto It = PendingOps.find(OpId);
			if (It == PendingOps.end() || It->second.bWarm)
			{
				return;
			}
			const FOp& Op = It->second;
			const bool bDiskDone = Op.Scope == EPrefixDropScope::Memory || Op.Result.Disk != EPrefixDropDisk::NotRequested;
			if (Op.Result.Result != EPrefixOpResult::Pending && bDiskDone)
			{
				FinishOp(OpId);
			}
		}

		FIssued MakeDropDelete(uint64_t OpId, const FPrefixKey& Key)
		{
			FIssued Issued = Make(EPrefixCommandKind::RemoveDiskFile);
			Issued.Command.Key = Key;
			Issued.Command.OpId = OpId;
			return Issued;
		}

		// A drop's disk part: the delete waits for every write-back save of its key out at this pass (a
		// write-back is the save and its file write), then runs on the key's serialization (§5.6).
		void StartDropDelete(uint64_t OpId, const FPrefixKey& Key)
		{
			PendingOps[OpId].bDeleteStarted = true;
			++Diag.DropDeletes;
			FWaitingDelete Waiting;
			Waiting.OpId = OpId;
			Waiting.Key = Key;
			for (const auto& Pair : SavesInFlight)
			{
				if (Pair.second == Key)
				{
					Waiting.Saves.insert(Pair.first);
				}
			}
			if (Waiting.Saves.empty())
			{
				SubmitKeyDisk(MakeDropDelete(OpId, Key));
				return;
			}
			WaitingDeletes.push_back(Waiting);
		}

		// A save completed or was dropped: a delete waiting only on it goes to its key's queue, after the
		// save's own file write (which the caller submitted first).
		void SaveDone(uint64_t SaveCommandId)
		{
			SavesInFlight.erase(SaveCommandId);
			for (size_t I = 0; I < WaitingDeletes.size();)
			{
				WaitingDeletes[I].Saves.erase(SaveCommandId);
				if (WaitingDeletes[I].Saves.empty())
				{
					const FWaitingDelete Ready = WaitingDeletes[I];
					WaitingDeletes.erase(WaitingDeletes.begin() + static_cast<std::ptrdiff_t>(I));
					SubmitKeyDisk(MakeDropDelete(Ready.OpId, Ready.Key));
					continue;
				}
				++I;
			}
		}

		void OnDropDelete(uint64_t OpId, EPrefixIoStatus Io, const std::string& IoError)
		{
			const auto It = PendingOps.find(OpId);
			if (It == PendingOps.end())
			{
				return;
			}
			FOp& Op = It->second;
			switch (Io)
			{
			case EPrefixIoStatus::Ok: Op.Result.Disk = EPrefixDropDisk::Deleted; break;
			case EPrefixIoStatus::NotFound: Op.Result.Disk = EPrefixDropDisk::NoFile; break;
			default:
				Op.Result.Disk = EPrefixDropDisk::Failed;
				// Once per op (§5.9, rev 1.18.4): the key, the file path and the file API's error.
				Fallback(EPrefixFallback::DropDeleteFailed, "DropPrefix op " + std::to_string(OpId) + ", key " + PrefixKeyToHex(Op.Key) +
					", file " + Config.DiskDirectory + "/" + PrefixDiskFileName(Op.Key) +
					", the file API refused the delete: " + (IoError.empty() ? std::string("no error reported") : IoError) +
					" (the file stays); the memory part keeps its result");
				break;
			}
			MaybeFinishDrop(OpId);
		}

		// --- Nested entries (U6) -------------------------------------------------------------------

		// The lookup pin a nested lease holds on its parent until its key is applied.
		void DropPendingParent(int64_t Handle)
		{
			const auto It = PendingParents.find(Handle);
			if (It != PendingParents.end())
			{
				Residency.RemovePin(It->second, EResidencyPin::ChildOp);
				PendingParents.erase(It);
			}
		}

		// The lookup pin a nested entry holds on its parent while it is uncharged (tier lookups, §5.4).
		void HoldLookupPin(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			const FResidencyEntryId Parent = Residency.GetParent(Id);
			if (!Entry.bLookupPin && Parent != kInvalidResidencyEntry)
			{
				Residency.AddPin(Parent, EResidencyPin::ChildOp);
				Entry.bLookupPin = true;
			}
		}

		void DropLookupPin(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			if (Entry.bLookupPin)
			{
				Residency.RemovePin(Entry.ParentEntry, EResidencyPin::ChildOp);
				Entry.bLookupPin = false;
			}
		}

		// Touching a child also touches its parent (§5.4), so parents age more slowly.
		void TouchWithAncestors(FResidencyEntryId Id)
		{
			Residency.Touch(Id);
			FResidencyEntryId Up = Residency.GetParent(Id);
			for (size_t Hops = 0; Up != kInvalidResidencyEntry && Hops < 64; ++Hops)
			{
				Residency.Touch(Up);
				Up = Residency.GetParent(Up);
			}
		}

		// --- Leases -------------------------------------------------------------------------------

		void ResolveLease(int64_t Handle, EPrefixLeaseState State, EPrefixSource Source)
		{
			FLease& Lease = Leases[Handle];
			Lease.bCancelledPending |= State == EPrefixLeaseState::Cancelled && Lease.State == EPrefixLeaseState::Pending;
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
			DropLookupPin(Id);
			FEntry& Entry = Entries[Id];
			const std::vector<int64_t> Waiting = Entry.Leases;
			for (int64_t Handle : Waiting)
			{
				DetachFromEntry(Handle);
				ResolveLease(Handle, State, EPrefixSource::Pending);
			}
			const std::vector<uint64_t> Warms = Entry.Warms;
			Entry.Warms.clear();
			for (uint64_t Op : Warms)
			{
				ResolveWarm(Op, WarmResultOf(State));
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
			const std::vector<uint64_t> Warms = Entry.Warms;
			Entry.Warms.clear();
			for (uint64_t Op : Warms)
			{
				ResolveWarm(Op, EPrefixOpResult::Resident); // resident and unpinned: a warm holds nothing
			}
			if (Source == EPrefixSource::Disk)
			{
				EmitKeyed(EPrefixCommandKind::TouchDiskFile, Entry.Key); // a hit refreshes the trim's age (§5.7)
			}
			// A MemoryAndDisk drop that found the key in flight or waiting suppressed the write-back (§5.6).
			const bool bWriteBack = !Entry.bNoWriteBack;
			Entry.bNoWriteBack = false;
			if (Source == EPrefixSource::Recomputed && Config.bDiskEnabled && Entry.bKeyed && bWriteBack)
			{
				Residency.AddPin(Id, EResidencyPin::Save);
				FPrefixCommand& Save = Emit(EPrefixCommandKind::Save, Id);
				Save.EngineHandle = EngineHandle;
				SavesInFlight[Save.Id] = Entry.Key;
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
					FIssued Read = Make(EPrefixCommandKind::ReadDisk, Id);
					Read.Command.Key = Entry.Key;
					Read.Command.Spec = Entry.Spec;
					SubmitKeyDisk(Read);
					return;
				}
				Entry.Tier = ETier::Recompute;
			}
			Entry.PendingOp = EPrefixCommandKind::Build;
			if (Entry.ParentEntry != kInvalidResidencyEntry && Residency.GetParent(Id) == kInvalidResidencyEntry)
			{
				// A nested build needs its parent (begin_from); a restore that evicted it cannot fall back to one.
				Fallback(EPrefixFallback::BuildFailed, KeyText(Entry) + ", its parent is no longer cached");
				Resolve(Id, EPrefixLeaseState::Failed);
				return;
			}
			// R(budget): a root's whole length, a child's suffix (begin_from's budget). A build never
			// evicts its own parent (§5.4, rev 1.17 R2).
			Admit(Id, BuildPagesFor(Entry.Length - Entry.Origin) * Config.PageBytes, true);
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
		void Admit(FResidencyEntryId Id, uint64_t PeakBytes, bool bParentRequired)
		{
			DropLookupPin(Id); // the decision pins the parent itself (Charged or Deferred)
			const EChargeOutcome Outcome = Residency.Charge(Id, PeakBytes, bParentRequired);
			if (Outcome == EChargeOutcome::Charged)
			{
				IssueLoad(Id);
			}
			else if (Outcome == EChargeOutcome::BudgetExhausted)
			{
				Fallback(EPrefixFallback::BudgetExhausted, ResolutionText(Id));
				Resolve(Id, EPrefixLeaseState::BudgetExhausted);
			}
			// Deferred: IssueLoad runs when the residency core admits it (release before reuse).
		}

		void IssueLoad(FResidencyEntryId Id)
		{
			FEntry& Entry = Entries[Id];
			FPrefixCommand& C = Emit(Entry.PendingOp, Id);
			C.ContextLength = Entry.Length;
			C.OriginLength = Entry.Origin;
			const FResidencyEntryId Parent = Residency.GetParent(Id);
			if (Parent != kInvalidResidencyEntry)
			{
				C.ParentEngineHandle = Entries[Parent].EngineHandle; // pinned: Resident with its handle
			}
			if (Entry.PendingOp == EPrefixCommandKind::Build)
			{
				C.ContextLength = Entry.Length - Entry.Origin; // begin_from's budget for a child
			}
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
			// Charged ceil(L/B) - s with s = 0, root or nested (§5.4, rev 1.4): whether the engine shares is
			// decided inside the restore. The engine's s is applied at Settle. A restore may evict its own
			// parent and then runs with a null hint (§5.4, rev 1.17 R2).
			Admit(Id, PagesFor(Entry.Length) * Config.PageBytes, false);
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
			Fallback(EPrefixFallback::PoolExhausted, ResolutionText(Id));
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
			HoldLookupPin(Id);
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
						Outbox.back().Arrival = Residency.GetArrival(Id);
					}
				}
			}
		}

		// --- Completions --------------------------------------------------------------------------

		// Who a key is applied for: a lease (Request) or a warm op (Warm), which runs the same path.
		struct FRequester
		{
			int64_t Lease = 0;
			uint64_t Warm = 0;
		};

		void AttachRequester(const FRequester& R, FResidencyEntryId Id)
		{
			if (R.Lease != 0)
			{
				Attach(R.Lease, Id);
				return;
			}
			Entries[Id].Warms.push_back(R.Warm);
			PendingOps[R.Warm].Entry = Id;
		}

		// A later request or warm applied to a marked entry clears the mark and restores its write-back (§5.6).
		void ClearMark(FResidencyEntryId Id)
		{
			if (Residency.IsMarked(Id))
			{
				Residency.SetMarked(Id, false);
				Entries[Id].bNoWriteBack = false;
				++Diag.MarksCleared;
			}
		}

		// The key of a lease's request: its own lookup, at its place in the issue order.
		void ApplyLeaseKey(int64_t Handle, const FPrefixKey& Key)
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
			FResidencyEntryId ParentEntry = kInvalidResidencyEntry;
			const auto ParentIt = PendingParents.find(Handle);
			if (ParentIt != PendingParents.end())
			{
				ParentEntry = ParentIt->second; // its lookup pin moves to the entry below, or is dropped
				PendingParents.erase(ParentIt);
			}
			FRequester R;
			R.Lease = Handle;
			OnKey(R, Key, std::move(Spec), ParentEntry);
		}

		void ApplyWarmKey(uint64_t OpId, const FPrefixKey& Key)
		{
			const auto It = PendingOps.find(OpId);
			if (bTearingDown || It == PendingOps.end() || It->second.Result.Result != EPrefixOpResult::Pending)
			{
				return;
			}
			FPrefixStoredSpec Spec = std::move(It->second.Spec);
			It->second.Spec = FPrefixStoredSpec();
			const FResidencyEntryId ParentEntry = It->second.ParentEntry;
			It->second.ParentEntry = kInvalidResidencyEntry;
			FRequester R;
			R.Warm = OpId;
			OnKey(R, Key, std::move(Spec), ParentEntry);
		}

		void OnKey(const FRequester& R, const FPrefixKey& Key, FPrefixStoredSpec Spec, FResidencyEntryId ParentEntry)
		{
			const FResidencyEntryId Found = Residency.FindByKey(kResidencyKindPrefix, Key);
			if (Found != kInvalidResidencyEntry)
			{
				// The memory tier's O(tokens) spec compare at the hit or attach (§5.2, rev 1.1).
				if (Entries[Found].Spec == Spec)
				{
					if (ParentEntry != kInvalidResidencyEntry)
					{
						Residency.RemovePin(ParentEntry, EResidencyPin::ChildOp);
					}
					ClearMark(Found);
					TouchWithAncestors(Found);
					if (Residency.GetState(Found) == EResidencyState::Resident)
					{
						if (R.Lease != 0)
						{
							Attach(R.Lease, Found);
							ResolveLease(R.Lease, EPrefixLeaseState::Ready, EPrefixSource::Memory);
						}
						else
						{
							ResolveWarm(R.Warm, EPrefixOpResult::Resident); // a hit: no lease, no pin
						}
						return;
					}
					AttachRequester(R, Found); // coalescing: one load or build per key
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
			if (ParentEntry != kInvalidResidencyEntry)
			{
				// Pinned since the request, so still Resident: the child continues it (§5.4, U6).
				Residency.SetParent(Id, ParentEntry);
				Entry.ParentEntry = ParentEntry;
				Entry.Origin = Entries[ParentEntry].Length;
				Entry.Length += Entry.Origin;
				Entry.bLookupPin = true;
			}
			Entry.Spec = std::move(Spec);
			Entry.Tier = ETier::Cooked;
			AttachRequester(R, Id);
			TryTier(Id);
		}

		// A drop's memory part, decided at its place in the issue order (§5.6). The entry is the one the key
		// names, matched on the whole key and spec; resident and unpinned it is evicted here (a pending refund,
		// not counted an eviction); pinned, in flight or waiting it is marked.
		void ApplyDropKey(uint64_t OpId, const FPrefixKey& Key)
		{
			const auto It = PendingOps.find(OpId);
			if (bTearingDown || It == PendingOps.end() || It->second.Result.Result != EPrefixOpResult::Pending)
			{
				return;
			}
			FOp& Op = It->second;
			Op.Key = Key;
			const FResidencyEntryId Found = Residency.FindByKey(kResidencyKindPrefix, Key);
			if (Found != kInvalidResidencyEntry && Entries[Found].Spec == Op.Spec)
			{
				if (Residency.EvictUnpinned(Found))
				{
					++Diag.Drops;
					Op.Result.Result = EPrefixOpResult::Dropped;
				}
				else
				{
					Residency.SetMarked(Found, true);
					if (Op.Scope == EPrefixDropScope::MemoryAndDisk && Residency.GetState(Found) != EResidencyState::Resident)
					{
						Entries[Found].bNoWriteBack = true; // in flight or waiting: it writes no file (§5.6)
					}
					++Diag.DropsDeferred;
					Op.Result.Result = EPrefixOpResult::DropDeferred;
				}
			}
			else
			{
				Op.Result.Result = EPrefixOpResult::NotCached;
			}
			Op.Spec = FPrefixStoredSpec();
			if (Op.Scope == EPrefixDropScope::MemoryAndDisk)
			{
				if (!Config.bDiskEnabled)
				{
					Op.Result.Disk = EPrefixDropDisk::DiskTierOff;
				}
				else
				{
					StartDropDelete(OpId, Key);
				}
			}
			MaybeFinishDrop(OpId);
		}

		// A queued mark application (step 3): it evicts only an entry still marked, resident and unpinned.
		// A cleared mark makes it void; a pinned entry stays marked and is queued again at a later pass.
		void ApplyMark(FResidencyEntryId Id)
		{
			const auto It = Entries.find(Id);
			if (It == Entries.end())
			{
				return;
			}
			It->second.bMarkQueued = false;
			if (Residency.IsMarked(Id) && Residency.EvictUnpinned(Id))
			{
				++Diag.MarksApplied;
			}
		}

		// Steps 2 and 3 of §5.6 *When a mark is applied*, at the end of an apply pass.
		void EndPass()
		{
			if (!bTearingDown)
			{
				for (auto& Pair : Entries)
				{
					const FResidencyEntryId Id = Pair.first;
					if (!Pair.second.bMarkQueued && Residency.IsMarked(Id) &&
						Residency.GetState(Id) == EResidencyState::Resident && !Residency.IsPinned(Id))
					{
						FIssueItem Item;
						Item.Kind = EIssueKind::MarkApp;
						Item.Entry = Id;
						IssueQueue.push_back(Item);
						Pair.second.bMarkQueued = true;
					}
				}
			}
			while (!IssueQueue.empty())
			{
				const FIssueItem Head = IssueQueue.front();
				if (Head.Kind != EIssueKind::MarkApp && !Head.bHashed)
				{
					break; // the head's hash is pending: everything issued after it waits
				}
				IssueQueue.pop_front();
				switch (Head.Kind)
				{
				case EIssueKind::Lease: ApplyLeaseKey(Head.Lease, Head.Key); break;
				case EIssueKind::Warm: ApplyWarmKey(Head.OpId, Head.Key); break;
				case EIssueKind::Drop: ApplyDropKey(Head.OpId, Head.Key); break;
				case EIssueKind::MarkApp: ApplyMark(Head.Entry); break;
				}
			}
		}

		// A ComputeKey completion: recorded only (step 1); the key is applied in issue order (step 3).
		void RecordKey(const FPrefixCommand& C, const FPrefixKey& Key)
		{
			for (FIssueItem& Item : IssueQueue)
			{
				const bool bMatch = C.OpId != 0
					? (Item.Kind == EIssueKind::Warm || Item.Kind == EIssueKind::Drop) && Item.OpId == C.OpId
					: Item.Kind == EIssueKind::Lease && Item.Lease == C.LeaseHandle;
				if (bMatch)
				{
					Item.bHashed = true;
					Item.Key = Key;
					return;
				}
			}
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
				if (Entry.ParentEntry != kInvalidResidencyEntry && Shared == 0)
				{
					Residency.DetachParent(Id); // restored private: it shares no page, a root from here on
				}
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
			HoldLookupPin(Id); // uncharged again: the next tier still needs the parent
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
				// At the freeze the charge falls from R(budget) to the private pages, ceil(L/B) - floor(origin/B).
				const uint64_t SharedFull = static_cast<uint64_t>(Entry.Origin) / Config.PageTokens;
				if (Entry.ParentEntry != kInvalidResidencyEntry && SharedFull == 0)
				{
					Residency.DetachParent(Id); // a parent with no full page shares nothing
				}
				BecomeReady(Id, Done.EngineHandle, EPrefixSource::Recomputed, (PagesFor(Entry.Length) - SharedFull) * Config.PageBytes);
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
				Fallback(EPrefixFallback::UnknownSchema, ResolutionText(Id) + ", schema \"" + Entry.Spec.SchemaName + "\"");
				Resolve(Id, EPrefixLeaseState::UnknownSchema);
				return;
			default:
				Fallback(EPrefixFallback::BuildFailed, KeyText(Entry) + ", status " + std::to_string(Done.EngineStatus));
				Resolve(Id, EPrefixLeaseState::Failed);
				return;
			}
		}

		void OnSave(const FIssued& Issued, const FPrefixCompletion& Done)
		{
			const FResidencyEntryId Id = Issued.Entry;
			Residency.RemovePin(Id, EResidencyPin::Save);
			if (Done.Engine != EPrefixEngineResult::Ok || Done.PayloadRef == 0)
			{
				// Skipped: the host's pool-destroy path never ran it (§5.4 rev 1.18.5), so nothing failed to log.
				if (!bTearingDown && Done.Engine != EPrefixEngineResult::Skipped)
				{
					Fallback(EPrefixFallback::SaveFailed, KeyText(Entries[Id]) + ", status " + std::to_string(Done.EngineStatus));
				}
				SaveDone(Issued.Command.Id);
				return;
			}
			if (bTearingDown)
			{
				EmitDropPayload(Done.PayloadRef);
				SaveDone(Issued.Command.Id);
				return;
			}
			const FEntry& Entry = Entries[Id];
			FIssued Write = Make(EPrefixCommandKind::WriteDisk);
			Write.Command.Key = Entry.Key;
			Write.Command.Spec = Entry.Spec;
			Write.Command.PayloadRef = Done.PayloadRef;
			Write.Command.WriteSerial = NextWriteSerial++;
			++Diag.DiskWritesIssued;
			// The save's file write goes on the key's serialization first, then any delete waiting on the save.
			SubmitKeyDisk(Write);
			SaveDone(Issued.Command.Id);
		}

		// §5.4 *What a release's status means* (rev 1.18.1): a release that returned has applied, whatever its
		// status, since the engine consumes the handle; only a GPU release that returned SSLM_BUSY (Busy) left
		// the handle untouched, and it alone is issued again, in teardown too, its refund pending meanwhile.
		void OnRelease(FResidencyEntryId Id, const FPrefixCompletion& Done)
		{
			if (Done.Engine == EPrefixEngineResult::Skipped)
			{
				// §5.4 rev 1.18.5: the submission thread skipped it, so it never reached the engine and has not
				// applied. Its refund stays pending, nothing is logged, and it is not issued again: the handle
				// stays live until the host's teardown sweep releases it. This relies on the device loss or the
				// unresponsive thread that skipped it ending the pool: a future GPU-loss recovery path that keeps the
				// configuration alive must sweep these releases (see EPrefixEngineResult::Skipped, U2 re-check R4).
				return;
			}
			const auto It = Entries.find(Id);
			if (Done.Engine == EPrefixEngineResult::Busy)
			{
				if (It != Entries.end())
				{
					Emit(EPrefixCommandKind::Release, Id).EngineHandle = It->second.EngineHandle;
					++Diag.ReleasesIssued;
					return;
				}
			}
			else if (Done.Engine != EPrefixEngineResult::Ok && It != Entries.end())
			{
				// Logged once; never retried: the handle is gone.
				Fallback(EPrefixFallback::ReleaseFailed, KeyText(It->second) + ", status " + std::to_string(Done.EngineStatus));
			}
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
			case EPrefixCommandKind::Save: OnSave(Issued, Done); break;
			case EPrefixCommandKind::Release: OnRelease(Id, Done); break;
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

		// Budget mode: region C carries exactly the cache's charged bytes after every call (§5.11 (b)).
		void MirrorRegionC()
		{
			if (Config.RegionC == nullptr)
			{
				return;
			}
			const uint64_t Charged = Residency.GetLedger(kResidencyKindPrefix, kResidencyLedgerHostRam).ChargedBytes;
			if (Charged == MirroredBytes)
			{
				return;
			}
			const SuperSLMPaging::ECacheRegionCharge Result = Charged > MirroredBytes
				? Config.RegionC->ChargeCacheRegionBytes(Charged - MirroredBytes)
				: Config.RegionC->RefundCacheRegionBytes(MirroredBytes - Charged);
			if (Result == SuperSLMPaging::ECacheRegionCharge::Ok)
			{
				MirroredBytes = Charged;
			}
			else
			{
				++Diag.RegionChargeFaults;
			}
		}

		bool IsTornDown() const
		{
			return bTearingDown && bTeardownSwept && InFlight.empty() && Outbox.empty() && Entries.empty() && KeyDisks.empty() &&
				WaitingDeletes.empty();
		}

		// RequestPrefix's synchronous checks, shared by WarmPrefix (exactly Request's) and DropPrefix (refused
		// only when malformed: no budget-mode refusal). OutParent is the nested spec's parent entry.
		EPrefixRequestResult Validate(const FPrefixSpec& Spec, bool bDrop, std::string& OutError, FResidencyEntryId& OutParent)
		{
			OutParent = kInvalidResidencyEntry;
			if (!bConfigured || bTearingDown)
			{
				OutError = "the prefix cache is not configured";
				return EPrefixRequestResult::NotConfigured;
			}
			if (Spec.Tokens.empty())
			{
				OutError = "a prefix needs at least one token";
				return EPrefixRequestResult::Malformed;
			}
			if (static_cast<int64_t>(Spec.Tokens.size()) > Config.ContextCap)
			{
				OutError = "the prefix is longer than the model's context cap";
				return EPrefixRequestResult::Malformed;
			}
			for (int32_t Token : Spec.Tokens)
			{
				if (Token < 0 || Token >= Config.VocabSize)
				{
					OutError = "token " + std::to_string(Token) + " is outside the vocabulary";
					return EPrefixRequestResult::Malformed;
				}
			}
			if (Spec.ParentHandle != 0)
			{
				if (!bDrop && !Config.bNestedEntries)
				{
					OutError = "nested prefixes need the plugin's budget mode";
					Fallback(EPrefixFallback::NestedRequiresBudgetMode, std::string());
					return EPrefixRequestResult::NestedRequiresBudgetMode;
				}
				const auto ParentIt = Leases.find(Spec.ParentHandle);
				if (ParentIt == Leases.end() || ParentIt->second.State != EPrefixLeaseState::Ready ||
					Residency.GetState(ParentIt->second.Entry) != EResidencyState::Resident)
				{
					OutError = "the parent is not a Ready prefix of this cache";
					return EPrefixRequestResult::Malformed;
				}
				if (!Spec.SchemaName.empty())
				{
					OutError = "a nested prefix inherits its parent's schema and names none of its own";
					return EPrefixRequestResult::Malformed;
				}
				const FResidencyEntryId Parent = ParentIt->second.Entry;
				if (static_cast<int64_t>(Entries[Parent].Length) + static_cast<int64_t>(Spec.Tokens.size()) > Config.ContextCap)
				{
					OutError = "the prefix and its parent are longer than the model's context cap";
					return EPrefixRequestResult::Malformed;
				}
				OutParent = Parent;
			}
			return EPrefixRequestResult::Accepted;
		}

		// The stored copy of an accepted spec (the parent's key for a nested one).
		FPrefixStoredSpec StoredSpecOf(const FPrefixSpec& Spec, FResidencyEntryId Parent)
		{
			FPrefixStoredSpec Copy;
			Copy.SchemaName = Spec.SchemaName;
			Copy.Tokens = Spec.Tokens;
			if (Parent != kInvalidResidencyEntry)
			{
				Copy.bHasParent = true;
				Copy.ParentKey = Entries[Parent].Key;
			}
			return Copy;
		}

		// Hashing is pool-thread work (§5.5): the key comes back as a completion.
		void EmitComputeKey(const FPrefixStoredSpec& Copy, int64_t Lease, uint64_t OpId)
		{
			FPrefixCommand& C = Emit(EPrefixCommandKind::ComputeKey);
			C.LeaseHandle = Lease;
			C.OpId = OpId;
			FPrefixKeyInputs& In = C.KeyInputs;
			In.ArtifactHash = Config.Identity.ArtifactHash;
			In.Layer1Tag = Config.Identity.Layer1Tag;
			In.Layer1Commit = Config.Identity.Layer1Commit;
			In.Backend = Config.Identity.Backend;
			In.AdapterVendorId = Config.Identity.AdapterVendorId;
			In.AdapterDeviceId = Config.Identity.AdapterDeviceId;
			In.SchemaName = Copy.SchemaName;
			In.Tokens = Copy.Tokens;
			In.bHasParent = Copy.bHasParent;
			In.ParentKey = Copy.ParentKey;
		}

		// An op issued: its id, its place in the issue order, its ComputeKey.
		uint64_t IssueOp(FOp Op, EIssueKind Kind)
		{
			const uint64_t OpId = NextOpId++;
			EmitComputeKey(Op.Spec, 0, OpId);
			PendingOps[OpId] = std::move(Op);
			FIssueItem Item;
			Item.Kind = Kind;
			Item.OpId = OpId;
			IssueQueue.push_back(Item);
			return OpId;
		}

		// Teardown or Abandon: every op that cannot finish resolves Cancelled. A drop whose memory part was
		// decided keeps that result while its delete is drained; Pending until it completes.
		void CancelOps()
		{
			std::vector<uint64_t> Cancel;
			for (auto& Pair : PendingOps)
			{
				FOp& Op = Pair.second;
				if (!Op.bWarm && Op.Result.Result != EPrefixOpResult::Pending)
				{
					continue; // its delete is drained, never cancelled
				}
				if (Op.ParentEntry != kInvalidResidencyEntry)
				{
					Residency.RemovePin(Op.ParentEntry, EResidencyPin::ChildOp);
					Op.ParentEntry = kInvalidResidencyEntry;
				}
				const auto E = Entries.find(Op.Entry);
				if (E != Entries.end())
				{
					std::vector<uint64_t>& Warms = E->second.Warms;
					Warms.erase(std::remove(Warms.begin(), Warms.end(), Pair.first), Warms.end());
				}
				Cancel.push_back(Pair.first);
			}
			for (uint64_t OpId : Cancel)
			{
				FOp& Op = PendingOps[OpId];
				if (Op.bWarm)
				{
					ResolveWarm(OpId, EPrefixOpResult::Cancelled);
				}
				else
				{
					Op.Result.Result = EPrefixOpResult::Cancelled;
					Op.Result.Disk = EPrefixDropDisk::NotRequested;
					FinishOp(OpId);
				}
			}
			IssueQueue.clear();
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
		uint64_t Budget = Config.BudgetBytes;
		if (Config.RegionC != nullptr)
		{
			// Budget mode: the ledger is region C's (§5.11 (b)); it starts empty, in whole pages.
			Budget = Config.RegionC->CacheRegionCapacityBytes();
			if (Config.RegionC->CacheRegionChargedBytes() != 0 || Budget % Config.PageBytes != 0)
			{
				return false;
			}
		}
		I.Residency.Reset();
		I.Residency.SetBudget(kResidencyKindPrefix, kResidencyLedgerHostRam, Budget);
		++I.Generation;
		I.MirroredBytes = 0;
		I.Config = Config;
		I.Entries.clear();
		// A request still Pending when the last configuration ended resolved Cancelled by name, and keeps reading it
		// (cell 1.7; the waiters of §5.11 (a), cell 8.14 StaleRelease). Box 2026-10-21 second run: clearing the
		// leases made it read Unknown (Invalid at the subsystem) once Configure() returned, so Cpu.ReconfigureInFlight
		// never saw the Cancelled it had resolved to. A Ready or returned lease still reads Unknown (cell 2.10).
		for (const auto& Pair : I.Leases)
		{
			if (Pair.second.bCancelledPending && Pair.second.State == EPrefixLeaseState::Cancelled &&
				I.EndedCancelledLeases.insert(Pair.first).second)
			{
				I.EndedCancelledOrder.push_back(Pair.first);
			}
		}
		while (I.EndedCancelledOrder.size() > kPrefixOpRing)
		{
			I.EndedCancelledLeases.erase(I.EndedCancelledOrder.front());
			I.EndedCancelledOrder.pop_front();
		}
		I.Leases.clear();
		I.PendingSpecs.clear();
		I.PendingParents.clear();
		I.OpenAdopts.clear();
		I.Outbox.clear();
		I.InFlight.clear();
		// Op ids and resolved results survive (§5.6 *Generations and teardown*); everything per configuration goes.
		I.IssueQueue.clear();
		I.KeyDisks.clear();
		I.SavesInFlight.clear();
		I.WaitingDeletes.clear();
		I.bTrimDeferred = false;
		I.Diag = FPrefixCacheDiagnostics();
		I.Diag.Generation = I.Generation;
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
			I.SubmitTrim(); // §5.7: trimmed at Configure()
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
		FResidencyEntryId ParentEntry = kInvalidResidencyEntry;
		const EPrefixRequestResult Check = I.Validate(Spec, false, OutError, ParentEntry);
		if (Check != EPrefixRequestResult::Accepted)
		{
			return Check;
		}

		OutHandle = I.Residency.MintLease();
		I.Leases[OutHandle] = FImpl::FLease();
		FPrefixStoredSpec& Copy = I.PendingSpecs[OutHandle];
		Copy = I.StoredSpecOf(Spec, ParentEntry);
		if (ParentEntry != kInvalidResidencyEntry)
		{
			// The parent stays cached through the child's key and tier lookups (§5.4 Pins).
			I.Residency.AddPin(ParentEntry, EResidencyPin::ChildOp);
			I.PendingParents[OutHandle] = ParentEntry;
			++I.Diag.NestedRequests;
		}
		++I.Diag.Requests;
		I.EmitComputeKey(Copy, OutHandle, 0);
		FImpl::FIssueItem Item;
		Item.Kind = FImpl::EIssueKind::Lease;
		Item.Lease = OutHandle;
		I.IssueQueue.push_back(Item); // its place in the issue order (§5.6 *What "later" means*)
		return EPrefixRequestResult::Accepted;
	}

	EPrefixRequestResult FSuperSLMPrefixCacheCore::Warm(const FPrefixSpec& Spec, uint64_t& OutOpId, std::string& OutError)
	{
		FImpl& I = *Impl;
		OutOpId = 0;
		FResidencyEntryId ParentEntry = kInvalidResidencyEntry;
		const EPrefixRequestResult Check = I.Validate(Spec, false, OutError, ParentEntry);
		if (Check != EPrefixRequestResult::Accepted)
		{
			return Check; // a refused call consumes no op id
		}
		FImpl::FOp Op;
		Op.bWarm = true;
		Op.Spec = I.StoredSpecOf(Spec, ParentEntry);
		if (ParentEntry != kInvalidResidencyEntry)
		{
			I.Residency.AddPin(ParentEntry, EResidencyPin::ChildOp); // as a nested request's lookup pin
			Op.ParentEntry = ParentEntry;
		}
		++I.Diag.Warms;
		OutOpId = I.IssueOp(std::move(Op), FImpl::EIssueKind::Warm);
		return EPrefixRequestResult::Accepted;
	}

	EPrefixRequestResult FSuperSLMPrefixCacheCore::Drop(const FPrefixSpec& Spec, EPrefixDropScope Scope, uint64_t& OutOpId, std::string& OutError)
	{
		FImpl& I = *Impl;
		OutOpId = 0;
		FResidencyEntryId ParentEntry = kInvalidResidencyEntry;
		const EPrefixRequestResult Check = I.Validate(Spec, true, OutError, ParentEntry);
		if (Check != EPrefixRequestResult::Accepted)
		{
			return Check;
		}
		FImpl::FOp Op;
		Op.bWarm = false;
		Op.Scope = Scope;
		Op.Spec = I.StoredSpecOf(Spec, ParentEntry); // the key inputs name the parent's key; nothing is pinned
		OutOpId = I.IssueOp(std::move(Op), FImpl::EIssueKind::Drop);
		return EPrefixRequestResult::Accepted;
	}

	bool FSuperSLMPrefixCacheCore::GetOpResult(uint64_t OpId, FPrefixOpResult& Out) const
	{
		const FImpl& I = *Impl;
		if (OpId == 0 || OpId >= I.NextOpId)
		{
			return false; // never issued
		}
		const auto Pending = I.PendingOps.find(OpId);
		if (Pending != I.PendingOps.end())
		{
			Out = FPrefixOpResult();
			return true;
		}
		const auto Resolved = I.ResolvedOps.find(OpId);
		if (Resolved != I.ResolvedOps.end())
		{
			Out = Resolved->second;
			return true;
		}
		Out = FPrefixOpResult();
		Out.Result = EPrefixOpResult::Expired;
		return true;
	}

	void FSuperSLMPrefixCacheCore::EndApplyPass()
	{
		FImpl& I = *Impl;
		I.EndPass();
		I.Drain();
		I.PumpAllKeys();
		I.AdvanceTeardown();
		I.MirrorRegionC();
	}

	uint32_t FSuperSLMPrefixCacheCore::PendingDropDeletes() const
	{
		uint32_t N = 0;
		for (const auto& Pair : Impl->PendingOps)
		{
			N += !Pair.second.bWarm && Pair.second.bDeleteStarted ? 1u : 0u;
		}
		return N;
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
		I.DropPendingParent(Handle);
		It->second.State = EPrefixLeaseState::Ended;
		return true;
	}

	EPrefixLeaseState FSuperSLMPrefixCacheCore::GetLeaseState(int64_t Handle) const
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end())
		{
			return Impl->EndedCancelledLeases.count(Handle) != 0 ? EPrefixLeaseState::Cancelled : EPrefixLeaseState::Unknown;
		}
		return It->second.State;
	}

	bool FSuperSLMPrefixCacheCore::GetLeaseKey(int64_t Handle, FPrefixKey& OutKey) const
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end() || It->second.State != EPrefixLeaseState::Ready)
		{
			return false;
		}
		const auto E = Impl->Entries.find(It->second.Entry);
		if (E == Impl->Entries.end() || !E->second.bKeyed)
		{
			return false;
		}
		OutKey = E->second.Key;
		return true;
	}

	uint64_t FSuperSLMPrefixCacheCore::GetLeaseEntryId(int64_t Handle) const
	{
		const auto It = Impl->Leases.find(Handle);
		if (It == Impl->Leases.end() || (It->second.State != EPrefixLeaseState::Pending && It->second.State != EPrefixLeaseState::Ready))
		{
			return 0;
		}
		return It->second.Entry;
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

	EPrefixAdoptCheck FSuperSLMPrefixCacheCore::BeginAdopt(int64_t Handle, uint64_t& OutEngineHandle, uint64_t& OutAdoptToken,
		uint64_t* OutEntryId)
	{
		FImpl& I = *Impl;
		OutEngineHandle = 0;
		OutAdoptToken = 0;
		if (OutEntryId != nullptr)
		{
			*OutEntryId = kInvalidResidencyEntry;
		}
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
		if (OutEntryId != nullptr)
		{
			*OutEntryId = Entry;
		}
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
		I.MirrorRegionC();
		return true;
	}

	bool FSuperSLMPrefixCacheCore::CheckPrefixSetModel(const std::string& SetName, const std::array<uint8_t, 32>& SetModelArtifactHash)
	{
		FImpl& I = *Impl;
		if (!I.bConfigured || SetModelArtifactHash == I.Config.Identity.ArtifactHash)
		{
			return true;
		}
		auto Hex = [](const std::array<uint8_t, 32>& Hash) {
			static const char* const kDigits = "0123456789abcdef";
			std::string Out;
			for (uint8_t B : Hash)
			{
				Out += kDigits[B >> 4];
				Out += kDigits[B & 15];
			}
			return Out;
		};
		I.Fallback(EPrefixFallback::PrefixSetModelMismatch, "set " + SetName + ": its model's artifact hash " + Hex(SetModelArtifactHash) +
			" is not the configured model's " + Hex(I.Config.Identity.ArtifactHash) + "; nothing registered");
		return false;
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
		// Waiters admitted in this pass go out in arrival order, in the slots their loads took (§5.11 (a)).
		std::vector<size_t> Slots;
		std::vector<std::pair<uint64_t, FImpl::FIssued>> Loads;
		for (size_t Index = 0; Index < I.Outbox.size(); ++Index)
		{
			if (I.Outbox[Index].Arrival != 0)
			{
				Slots.push_back(Index);
				Loads.push_back({I.Outbox[Index].Arrival, I.Outbox[Index]});
			}
		}
		std::sort(Loads.begin(), Loads.end(),
			[](const std::pair<uint64_t, FImpl::FIssued>& A, const std::pair<uint64_t, FImpl::FIssued>& B) { return A.first < B.first; });
		for (size_t K = 0; K < Slots.size(); ++K)
		{
			I.Outbox[Slots[K]] = Loads[K].second;
		}
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
		if (Completion.Generation != I.Generation)
		{
			// Posted for a configuration that has ended (§5.11 (a), rev 1.16 L1): it refunds nothing.
			++I.Diag.StaleCompletionsDropped;
			return;
		}
		const auto It = I.InFlight.find(Completion.CommandId);
		if (It == I.InFlight.end())
		{
			return;
		}
		const FImpl::FIssued Issued = It->second;
		I.InFlight.erase(It);
		const FResidencyEntryId Id = Issued.Entry;
		if (FImpl::IsKeyDisk(Issued.Command.Kind))
		{
			I.KeyDiskDone(Issued.Command); // the key's next disk command may go once this one's effects are applied
		}
		if (FImpl::IsDropDelete(Issued.Command))
		{
			I.OnDropDelete(Issued.Command.OpId, Completion.Io, Completion.IoError); // drained in teardown too, never cancelled
		}

		if (I.bTearingDown)
		{
			I.OnTeardownCompletion(Issued, Completion);
		}
		else
		{
			switch (Issued.Command.Kind)
			{
			case EPrefixCommandKind::ComputeKey: I.RecordKey(Issued.Command, Completion.Key); break; // applied in issue order
			case EPrefixCommandKind::CheckCooked: I.OnWrapperRead(Id, Completion, false); break;
			case EPrefixCommandKind::ReadDisk: I.OnWrapperRead(Id, Completion, true); break;
			case EPrefixCommandKind::Restore: I.OnRestore(Id, Completion); break;
			case EPrefixCommandKind::Build: I.OnBuild(Id, Completion); break;
			case EPrefixCommandKind::Save: I.OnSave(Issued, Completion); break;
			case EPrefixCommandKind::WriteDisk:
				if (Completion.Io == EPrefixIoStatus::Ok)
				{
					I.SubmitTrim(); // §5.7: trimmed after writes
				}
				else
				{
					I.Fallback(EPrefixFallback::DiskWriteError, "key " + PrefixKeyToHex(Issued.Command.Key));
				}
				break;
			case EPrefixCommandKind::Release: I.OnRelease(Id, Completion); break;
			default: break;
			}
		}
		// A trim deferred behind drop deletes goes once none is out.
		if (I.bTrimDeferred && !I.DropDeleteOut())
		{
			I.bTrimDeferred = false;
			I.SubmitTrim();
		}
		I.Drain();
		I.PumpAllKeys();
		I.AdvanceTeardown();
		I.MirrorRegionC();
	}

	uint32_t FSuperSLMPrefixCacheCore::InFlightCommands() const
	{
		return static_cast<uint32_t>(Impl->InFlight.size());
	}

	void FSuperSLMPrefixCacheCore::MarkTeardownBegins()
	{
		FImpl& I = *Impl;
		if (!I.bEverConfigured || I.IsTornDown())
		{
			return;
		}
		for (auto& Pair : I.Leases)
		{
			Pair.second.bCancelledPending |= Pair.second.State == EPrefixLeaseState::Pending;
		}
	}

	void FSuperSLMPrefixCacheCore::BeginTeardown()
	{
		FImpl& I = *Impl;
		if (!I.bConfigured || I.bTearingDown)
		{
			return;
		}
		I.bTearingDown = true;

		// Every pending lease and op resolves by name; the issue order is cleared (§5.6 *Generations and teardown*).
		for (auto& Pair : I.Leases)
		{
			if (Pair.second.State == EPrefixLeaseState::Pending)
			{
				I.DetachFromEntry(Pair.first);
				Pair.second.State = EPrefixLeaseState::Cancelled;
				Pair.second.bCancelledPending = true;
				I.PendingSpecs.erase(Pair.first);
				I.DropPendingParent(Pair.first);
			}
		}
		I.CancelOps();
		I.bTrimDeferred = false;

		// Commands not yet taken are dropped, except releases, payload drops and drop deletes. What they would
		// have done is undone here: a load's charge returned, a save's pin removed, held bytes dropped.
		std::vector<FImpl::FIssued> Kept;
		const std::vector<FImpl::FIssued> Outbox = I.Outbox;
		I.Outbox.clear();
		for (const FImpl::FIssued& Issued : Outbox)
		{
			const FPrefixCommand& C = Issued.Command;
			if (FImpl::IsKeyDisk(C.Kind) && !FImpl::IsDropDelete(C))
			{
				I.KeyDiskDone(C);
			}
			switch (C.Kind)
			{
			case EPrefixCommandKind::Release:
			case EPrefixCommandKind::DropPayload:
				Kept.push_back(Issued);
				break;
			case EPrefixCommandKind::RemoveDiskFile:
				if (FImpl::IsDropDelete(C))
				{
					Kept.push_back(Issued); // a drop's delete is drained, never cancelled
				}
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
				I.SaveDone(C.Id);
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

		// The keys' queued disk commands: a read's entry resolves Cancelled, a write's bytes are dropped, a
		// drop's delete stays.
		for (auto& Pair : I.KeyDisks)
		{
			std::deque<FImpl::FIssued> Stay;
			const std::deque<FImpl::FIssued> Queue = Pair.second.Queue;
			Pair.second.Queue.clear();
			for (const FImpl::FIssued& Issued : Queue)
			{
				if (FImpl::IsDropDelete(Issued.Command))
				{
					Stay.push_back(Issued);
				}
				else if (Issued.Command.Kind == EPrefixCommandKind::ReadDisk)
				{
					if (I.Entries.count(Issued.Entry) != 0)
					{
						I.Resolve(Issued.Entry, EPrefixLeaseState::Cancelled);
					}
				}
				else if (Issued.Command.Kind == EPrefixCommandKind::WriteDisk)
				{
					I.EmitDropPayload(Issued.Command.PayloadRef);
				}
			}
			Pair.second.Queue = Stay;
		}

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
		I.PumpAllKeys();
		I.AdvanceTeardown();
		I.MirrorRegionC();
	}

	void FSuperSLMPrefixCacheCore::Abandon()
	{
		FImpl& I = *Impl;
		if (!I.bEverConfigured || I.IsTornDown())
		{
			return;
		}
		for (auto& Pair : I.Leases)
		{
			if (Pair.second.State != EPrefixLeaseState::Ended)
			{
				Pair.second.bCancelledPending |= Pair.second.State == EPrefixLeaseState::Pending;
				Pair.second.State = EPrefixLeaseState::Cancelled;
				Pair.second.Entry = kInvalidResidencyEntry;
			}
		}
		// Ops resolve as at a teardown. A drop's delete is never carried into the next Configure() (§5.4 rev
		// 1.18.5): the host drains it under this configuration before calling here. One still pending did not
		// drain (the host's bounded drain ran out): its disk part reads Failed, logged once, the file left.
		for (auto& Pair : I.PendingOps)
		{
			Pair.second.Entry = kInvalidResidencyEntry;
			Pair.second.ParentEntry = kInvalidResidencyEntry; // the residency core is reset below
		}
		I.CancelOps();
		std::vector<uint64_t> Undrained;
		for (const auto& Pair : I.PendingOps)
		{
			if (!Pair.second.bWarm && Pair.second.bDeleteStarted)
			{
				Undrained.push_back(Pair.first);
			}
		}
		for (uint64_t OpId : Undrained)
		{
			FImpl::FOp& Op = I.PendingOps[OpId];
			Op.Result.Disk = EPrefixDropDisk::Failed;
			I.Fallback(EPrefixFallback::DropDeleteFailed, "DropPrefix op " + std::to_string(OpId) + ", key " + PrefixKeyToHex(Op.Key) +
				", file " + I.Config.DiskDirectory + "/" + PrefixDiskFileName(Op.Key) +
				", the delete did not drain before the configuration ended (the file stays); the memory part keeps its result");
			I.FinishOp(OpId);
		}
		I.Entries.clear();
		I.PendingSpecs.clear();
		I.PendingParents.clear();
		I.OpenAdopts.clear();
		I.Outbox.clear();
		I.InFlight.clear();
		I.KeyDisks.clear();
		I.SavesInFlight.clear();
		I.WaitingDeletes.clear();
		I.bTrimDeferred = false;
		I.Residency.Reset();
		// Region C's ledger may already have ended with the pool; a refund it refuses is not a fault here.
		if (I.Config.RegionC != nullptr && I.MirroredBytes != 0)
		{
			I.Config.RegionC->RefundCacheRegionBytes(I.MirroredBytes);
		}
		I.MirroredBytes = 0;
		++I.Generation;
		I.bTearingDown = true;
		I.bTeardownSwept = true;
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
		D.WaitingBytes = L.WaitingBytes;
		D.ClaimedRefundBytes = L.ClaimedRefundBytes;
		D.UnclaimedRefundBytes = L.ReleasingBytes - L.ClaimedRefundBytes;
		D.PendingRefunds = L.ReleasingEntries;
		D.Waiters = L.WaitingEntries;
		D.Generation = Impl->Generation;
		D.MirroredRegionBytes = Impl->MirroredBytes;
		D.ResidentEntries = L.ResidentEntries;
		D.GhostBytes = L.GhostBytes;
		D.Ghosts = L.GhostEntries;
		D.GhostEndings = Impl->Residency.Snapshot().GhostEndings;
		D.Evictions = Impl->Residency.Snapshot().Evictions;
		for (const auto& Pair : Impl->PendingSpecs)
		{
			D.SpecTokensHeld += Pair.second.Tokens.size();
		}
		for (const auto& Pair : Impl->PendingOps)
		{
			D.SpecTokensHeld += Pair.second.Spec.Tokens.size();
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
