#pragma once

// The prefix cache's policy (precache plan §5.2-§5.4, §5.9, §5.10), with no Unreal and no Layer-1
// call in it. The subsystem owns one instance per configured pool, calls it from API calls and from
// Tick()'s apply pass (game thread only), executes the commands it emits, and feeds back one
// completion per command.
//
//   Request() -------------------------------> ComputeKey            (pool thread: hashing, §5.5)
//   apply(key): memory map lookup + O(tokens) spec compare on the game thread
//      hit  -> the lease is Ready at this apply (a memory hit is delivered at a tick, §5.5)
//      in flight -> the lease attaches to it (coalescing: one load or build per key)
//      miss -> cooked:  CheckCooked       (pool thread: wrapper checks over bytes in memory)
//              disk:    ReadDisk          (pool thread: read + wrapper checks)
//              then     charge ceil(L/B) - s, s = 0 for a root  -> Restore (worker lane)
//              else     charge R(L) = min(ceil(L/B) + 1, ceil(cap/B)) -> Build (worker lane:
//                       begin_budgeted(L), schema bind, prefill, freeze); at the applied freeze the
//                       charge falls to ceil(L/B)
//              recomputed and disk on -> Save (worker lane, Save-pinned) -> WriteDisk -> TrimDisk
//   evictions -> Release (worker lane); the charge stays until the Release completes. A release that
//                returned has applied, whatever its status (the engine consumes the handle), except a GPU
//                release that returned SSLM_BUSY (Busy), which is issued again (§5.4, rev 1.18.1)
//
// Issue order (§5.6, rev 1.18.1-1.18.2): Request(), Warm() and Drop() each take an issue sequence number at
// the call. A key completion is only recorded at Apply(); the ops are applied at EndApplyPass(), in issue
// order, while the head's hash has completed, after the mark applications the pass queues. The caller ends
// every apply pass with EndApplyPass(), after the pass's completions.
//
// Disk serialization (§5.6, §5.7): ReadDisk, WriteDisk and a drop's delete run one at a time per key, in the
// order the core submits them; a drop's delete also waits for any write-back save of its key that was in
// flight at its apply pass (write-back = save + file write), and never runs beside a TrimDisk.
//
// Lookup order is memory, cooked, disk, recompute; a tier that fails for any reason falls through to
// the next (§5.3). A wrapper that fails a check is a miss, the file is removed (RemoveDiskFile) or
// the cooked entry is marked stale, and one log line names the reason (§5.7). A restore the engine
// refuses (digest, producer, model) is the same: a miss, the file removed, then recompute (§5.9).
//
// The charge rule (§5.4) is FSuperSLMResidencyCore's, with the prefix kind in HostRam:
//   - a restore is charged ceil(L/B) - s pages and a build R(L) pages, times PageBytes, on the game
//     thread, BEFORE its command is emitted; least-recently-used unpinned entries are evicted to fit;
//   - an admission that cannot fit even with every unpinned entry gone resolves BudgetExhausted by
//     name, draws nothing, waits for nothing, and evicts nothing;
//   - an admission covered only with claims on pending refunds (evicted entries whose Release has not
//     yet been applied) waits, holding the free charge it took and drawing nothing, and is emitted at
//     the apply of the last Release it claims on (§5.11 (a), deferred-refund admission); it is never
//     refused for bytes that are coming back. Waiters admitted in one apply pass are emitted in
//     arrival order (TakeCommands keeps that order among them);
//   - a refused or failed restore or build returns its whole charge when its result is applied;
//   - a restore or build the pool refuses (PoolExhausted) evicts the next LRU unpinned entry and is
//     retried after that entry's Release completes, until it succeeds or nothing is evictable; then
//     it resolves PoolExhausted by name and returns its charge.
// So ChargedBytes never exceeds BudgetBytes, and every page the cache holds or may draw is charged.
//
// Pins (§5.4): a lease, a queued or running adopt (BeginAdopt..EndAdopt), a queued or running save,
// and a child's restore or build using it as parent. A pinned entry is never evicted.
//
// Nested entries (§5.4, U6; with FPrefixCacheConfig::bNestedEntries, i.e. budget mode -- otherwise a
// nested spec is refused NestedRequiresBudgetMode as before). A nested spec names a Ready lease of
// this cache as its parent; its Tokens are the tokens AFTER the parent's (begin_from's budget), its
// SchemaName is empty (a child inherits its parent's binding), and its key hashes the parent's key.
// The parent is pinned from the request through the child's tier lookups, and from the child's charge
// decision by the residency core (a waiter included) until the child's load is applied. A nested
// restore is charged s = 0, ceil(L/B) pages of its whole length L, and may evict its own parent in its
// own loop, then runs with a null hint (private). A nested build is charged R(suffix), never evicts
// its parent, and runs begin_from on it. Applied, a child settles at its private pages; a child that
// shares no page with its parent is a root from then on. An entry evicted while a cached descendant
// shares its pages leaves a ghost charge until its last cached descendant is evicted, and every
// pending refund completes only when its whole family's releases have applied (§5.11 (a)).
//
// Teardown and re-Configure (§5.4 last bullet, cells 1.5 and 1.7): BeginTeardown() refuses new
// requests, resolves every pending lease Cancelled, drops every command not yet taken, and emits no
// new work but Release and DropPayload. Every command already taken by the executor is in flight
// and must complete; an engine handle a completion delivers during teardown is released, and a
// payload ref it delivers is dropped. Only when nothing is in flight and no adopt is open does the
// core emit one Release per engine handle it holds; every Ready lease then resolves Cancelled. When
// those complete, IsTornDown() is true and Configure() may be called again. Handles from before are
// then Unknown to every call.
//
// Abandon() is the other way out (§5.11 (a), rev 1.16 L1): the subsystem has drained its lanes and is
// destroying the pool, which releases every engine handle, without waiting for the completions still
// queued. Every live lease resolves Cancelled, every record goes, and the configuration generation
// moves on. Every command carries the generation it was emitted under, and its completion echoes it:
// a completion from another generation is dropped at Apply and refunds nothing into the new ledger.
//
// Budget mode (§5.11 (b)): with FPrefixCacheConfig::RegionC set, the cache's ledger IS paging's
// region C. Its budget is C's capacity, and every change of the cache's charged bytes is mirrored
// through the region-C seam after each call, so paging's ledger reads C as it reads its own regions.

#include "SuperSLMPrefixCacheKey.h"
#include "SuperSLMPrefixCacheWrapper.h"
#include "SuperSLMPrefixDiskCache.h"
#include "SuperSLMResidencyCore.h"
#include "SuperSLMPageLedgerCore.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SuperSLMCore
{
	struct FPrefixCacheConfig
	{
		FPrefixEngineIdentity Identity;
		uint64_t BudgetBytes = 0;      // PrefixCacheBudgetBytes
		uint64_t PageBytes = 0;        // sslm_kv_page_size
		uint32_t PageTokens = 16;      // B, positions per page
		int64_t ContextCap = 0;        // the model's context_cap
		int32_t VocabSize = 0;
		bool bDiskEnabled = false;     // bPrefixDiskCacheEnabled (default false)
		std::string DiskDirectory;     // PrefixDiskCacheDirectory
		uint64_t DiskMaxBytes = 0;     // PrefixDiskCacheMaxBytes
		uint64_t InstanceId = 0;       // unique per subsystem instance; names temporaries
		// Budget mode: paging's region-C seam. When set, BudgetBytes is ignored and the budget is the
		// seam's capacity; the seam must read 0 charged at Configure, and its capacity must be whole
		// pages of PageBytes. Not owned; it must outlive the configuration.
		SuperSLMPaging::ISuperSLMCacheRegionCharge* RegionC = nullptr;
		// Nested entries (U6). The subsystem sets it in budget mode (CPU: with RegionC; GPU: pooled
		// sequences); false keeps refusing nested specs NestedRequiresBudgetMode.
		bool bNestedEntries = false;
	};

	// The request a caller makes (FSuperSLMPrefixSpec). ParentHandle 0 = a root prefix; otherwise a Ready
	// lease of this cache, Tokens are the tokens after the parent's, and SchemaName is empty.
	struct FPrefixSpec
	{
		std::vector<int32_t> Tokens;
		std::string SchemaName;
		int64_t ParentHandle = 0;
	};

	enum class EPrefixRequestResult : uint8_t
	{
		Accepted = 0,                 // a lease handle was minted; it resolves at a later apply
		NotConfigured = 1,            // never configured, or tearing down
		Malformed = 2,                // empty, a token outside [0, VocabSize), or longer than ContextCap (with the
		                              // parent's, when nested); a nested spec whose parent is not a Ready lease
		                              // of this cache, or that names a schema of its own
		NestedRequiresBudgetMode = 3, // ParentHandle != 0
	};

	enum class EPrefixSource : uint8_t
	{
		None = 0,       // not a handle this cache minted since its last Configure
		Pending = 1,
		Memory = 2,
		Cooked = 3,
		Disk = 4,
		Recomputed = 5,
	};

	enum class EPrefixLeaseState : uint8_t
	{
		Unknown = 0,          // not minted by this cache since its last Configure
		Pending = 1,
		Ready = 2,
		BudgetExhausted = 3,
		PoolExhausted = 4,
		UnknownSchema = 5,
		Cancelled = 6,        // resolved by teardown: still Pending, or Ready until the teardown released its handle
		Failed = 7,           // the build failed for any other reason
		Ended = 8,            // returned
	};

	enum class EPrefixAdoptCheck : uint8_t
	{
		Adoptable = 0,        // OutEngineHandle is the entry's handle; the entry is Adopt-pinned until EndAdopt
		NotReady = 1,         // a live lease that is still Pending or resolved to a refusal
		PrefixLeaseEnded = 2, // a cache handle whose lease ended (returned, or its entry since evicted)
		NotCacheHandle = 3,   // not minted by this cache since its last Configure: the caller's own path decides
	};

	// Every named fallback and refusal (§5.9, §5.10). Each raises its own counter by one and writes
	// one log line whose message contains PrefixFallbackName(Reason) and, for PayloadRefused, the
	// engine's status number in decimal (FPrefixCompletion::EngineStatus).
	enum class EPrefixFallback : uint8_t
	{
		WrapperTruncated = 0,
		WrapperBadMagic,
		WrapperUnsupportedVersion,
		WrapperBackendMismatch,
		WrapperLayer1TagMismatch,
		WrapperLayer1CommitMismatch,
		WrapperArtifactMismatch,
		WrapperKeyMismatch,
		WrapperMalformed,
		WrapperSpecMismatch,
		MemorySpecMismatch,       // the memory tier's compare at a hit (rev 1.1)
		PayloadRefused,           // the engine refused the restore (digest, producer, model, ...)
		UnknownSchema,
		BudgetExhausted,
		PoolExhausted,
		DiskReadError,
		DiskWriteError,
		NestedRequiresBudgetMode,
		ReleaseOfCacheHandle,     // ReleasePrefix given a cache handle
		AdoptOfEndedLease,        // AdoptPrefix given an ended lease: PrefixLeaseEnded
		ReturnNotOwned,           // ReturnPrefix given a handle it ends no lease on
		BuildFailed,              // a build ended Failed (not PoolExhausted or UnknownSchema): Failed by name
		RestoreFailed,            // a restore ended Failed, an engine failure that does not judge the bytes: a miss
		SaveFailed,               // a save ended Failed: no write-back
		ReleaseFailed,            // a release returned a status other than OK and (GPU) SSLM_BUSY: the engine consumed the
		                          // handle, so the release HAS applied and its refund advances as for OK; logged once,
		                          // never retried (§5.4 *What a release's status means*, rev 1.18.1)
		DropDeleteFailed,         // a drop's disk part: the file API refused the delete (Disk == Failed, §5.6); logged
		                          // once per op with the key, the file path and the file API's error (§5.9, rev 1.18.4)
		PrefixSetModelMismatch,   // a prefix set whose model is not the configured model (§5.8): nothing registered, no
		                          // spec; logged with the set's name and both models' artifact hashes (§5.9, rev 1.18.4)
		// Reason codes (§5.9, rev 1.18.4): a warm's refusals count under the request's own reasons (BudgetExhausted,
		// PoolExhausted, UnknownSchema; the line names "Warm" and the op id, and the key). A deferred drop is not a
		// fallback: it counts only in DropsDeferred (and MarksApplied when its mark applies), and logs nothing.
		Count
	};
	constexpr uint32_t kPrefixFallbackCount = static_cast<uint32_t>(EPrefixFallback::Count);

	const char* PrefixFallbackName(EPrefixFallback Reason);

	struct FPrefixLogLine
	{
		EPrefixFallback Reason = EPrefixFallback::Count;
		std::string Message;
	};

	enum class EPrefixCommandKind : uint8_t
	{
		ComputeKey = 0,     // pool thread: ComputePrefixKey(KeyInputs)
		CheckCooked = 1,    // pool thread: CheckPrefixWrapper on cooked blob CookedBlobId against Key and Spec
		ReadDisk = 2,       // pool thread: ReadPrefixFile(Key) then CheckPrefixWrapper against Key and Spec
		Restore = 3,        // worker lane: sslm_prefix_restore of PayloadRef (L = ContextLength), hint ParentEngineHandle (0 = none)
		Build = 4,          // worker lane: root: begin_budgeted(ContextLength), bind Spec.SchemaName, prefill Spec.Tokens, freeze;
		                    // nested: begin_from(ParentEngineHandle, ContextLength), prefill Spec.Tokens, freeze
		Save = 5,           // worker lane: sslm_prefix_save(EngineHandle)
		WriteDisk = 6,      // pool thread: EncodePrefixWrapper(Key, Spec, PayloadRef) then WritePrefixFileAtomically(WriteSerial)
		RemoveDiskFile = 7, // pool thread: remove the file for Key
		TouchDiskFile = 8,  // pool thread: set the file for Key's modification time to now (at least every disk hit, §5.7)
		TrimDisk = 9,       // pool thread: TrimPrefixDiskDirectory(DiskMaxBytes)
		Release = 10,       // worker lane: sslm_prefix_release(EngineHandle)
		DropPayload = 11,   // the executor frees PayloadRef; nothing else
	};

	// Payload refs are the executor's (bytes read, checked or saved off the game thread). Each ref a
	// completion delivers is named by exactly one later command: the Restore or WriteDisk that uses
	// it, or a DropPayload when the core does not use it (a refused charge, a teardown).
	struct FPrefixCommand
	{
		uint64_t Id = 0;                  // unique per Configure; echoed by its completion
		EPrefixCommandKind Kind = EPrefixCommandKind::ComputeKey;
		int64_t LeaseHandle = 0;          // ComputeKey
		FPrefixKeyInputs KeyInputs;       // ComputeKey
		FPrefixKey Key;                   // CheckCooked, ReadDisk, WriteDisk, RemoveDiskFile, TouchDiskFile
		FPrefixStoredSpec Spec;           // CheckCooked, ReadDisk (expected), Build, WriteDisk
		uint64_t CookedBlobId = 0;        // CheckCooked
		uint64_t PayloadRef = 0;          // Restore, WriteDisk, DropPayload
		uint64_t EngineHandle = 0;        // Save, Release
		int32_t ContextLength = 0;        // Restore: L (the whole length); Build: the budget (L of a root, the suffix of a child)
		uint64_t ParentEngineHandle = 0;  // Restore: the sharing hint (0 = private); Build: the parent of a nested build
		int32_t OriginLength = 0;         // Restore, Build: the parent's length (0 for a root)
		uint64_t WriteSerial = 0;         // WriteDisk: unique per write within this instance
		uint64_t Generation = 0;          // the configuration generation it was emitted under; echoed
		uint64_t OpId = 0;                // ComputeKey of a WarmPrefix or DropPrefix (LeaseHandle 0); RemoveDiskFile of a
		                                  // drop's disk part (§5.6, rev 1.18); 0 otherwise
		uint64_t EntryId = 0;             // the residency id of the entry the command works for (0: none). Never
		                                  // reused; the executor's test seams key a build's progress on it (§8)
	};

	enum class EPrefixEngineResult : uint8_t
	{
		Ok = 0,
		PoolExhausted = 1,  // SSLM_KV_POOL_EXHAUSTED: nothing was drawn
		UnknownSchema = 2,  // Build: the schema name did not resolve
		Refused = 3,        // Restore: the engine refused the blob (digest, producer, model...): a miss
		Failed = 4,         // anything else
		Busy = 5,           // Release, GPU only: SSLM_BUSY. The handle is untouched (the engine's SSLM_BUSY contract), so the
		                    // release has not applied: it is issued again at a later tick and its refund stays pending
		                    // until a later issue returns anything else (§5.4 *What a release's status means*, rev 1.18.1)
		// The job never reached the engine (U2 code review findings 1 and 3, rev 1.18.5). A Release, GPU only:
		// the submission thread skipped it (a terminal device loss, or the thread marked unresponsive), so the
		// handle is still live (§5.4 *a skipped release is not a release that returned*). Not applied: its refund
		// stays pending, nothing is refunded, no ReleaseFailed is logged, and it is NOT issued again; the handle
		// stays in the host's live-handle set until teardown's sweep releases it. A Save: a write-back no lane ran
		// before the pool went (the host's pool-destroy drain, §5.4), so it writes nothing and logs no SaveFailed.
		// RELIES ON THE POOL ENDING (U2 re-check R4): a skipped release is never retried, so its entry, its pending
		// refund and its live handle are cleared only by the teardown that a terminal device loss or an
		// unresponsive submission thread brings (ReleaseForPoolDestroy's sweep, then Abandon()). A future GPU-loss
		// recovery path that keeps the pool, or the cache's configuration, alive must sweep these releases itself
		// (release or forget each skipped handle and complete its refund); otherwise they stay pending for good.
		Skipped = 6,
	};

	// Exactly one per command, fed to Apply() in Tick()'s apply pass.
	struct FPrefixCompletion
	{
		uint64_t CommandId = 0;
		FPrefixKey Key;                                     // ComputeKey
		EPrefixIoStatus Io = EPrefixIoStatus::Ok;           // ReadDisk, WriteDisk
		EPrefixWrapperCheck Wrapper = EPrefixWrapperCheck::Ok; // CheckCooked; ReadDisk when Io is Ok
		EPrefixEngineResult Engine = EPrefixEngineResult::Ok;  // Restore, Build, Save
		uint64_t PayloadRef = 0;                            // CheckCooked / ReadDisk / Save when Ok
		uint64_t EngineHandle = 0;                          // Restore / Build when Ok
		uint32_t SharedPages = 0;                           // Restore when Ok: s, read as bookkeeping
		int32_t EngineStatus = 0;                           // the raw sslm_status, for the log line
		uint64_t Generation = 0;                            // the command's Generation, echoed
		// U2 code review finding 6; §5.9 rev 1.18.4: the file API's error for a RemoveDiskFile whose Io is
		// Error (the platform's error code and text). A drop's DropDeleteFailed line carries it.
		std::string IoError;
	};

	struct FPrefixCacheDiagnostics
	{
		uint64_t BudgetBytes = 0;
		uint64_t ChargedBytes = 0;
		uint64_t ResidentBytes = 0;
		uint64_t InFlightBytes = 0;
		uint64_t ReleasingBytes = 0;                        // pending refunds (§5.11 (a))
		uint64_t WaitingBytes = 0;                          // free charge taken by waiters
		uint64_t ClaimedRefundBytes = 0;                    // pending refunds claimed by waiters
		uint64_t UnclaimedRefundBytes = 0;                  // pending refunds claimed by nobody
		uint32_t PendingRefunds = 0;
		uint32_t Waiters = 0;
		uint32_t ResidentEntries = 0;
		uint64_t Requests = 0;
		uint64_t ServedBySource[6] = {};                    // indexed by EPrefixSource
		uint64_t Evictions = 0;
		uint64_t RestoresIssued = 0;
		uint64_t BuildsIssued = 0;
		uint64_t ReleasesIssued = 0;
		uint64_t DiskWritesIssued = 0;
		uint64_t SpecMismatches = 0;                        // memory-tier and wrapper spec mismatches together
		uint64_t SpecTokensHeld = 0;                        // tokens of every spec the cache keeps a copy of
		uint64_t Fallbacks[kPrefixFallbackCount] = {};
		uint64_t Generation = 0;                            // the current configuration generation
		uint64_t StaleCompletionsDropped = 0;               // completions from another generation
		uint64_t RegionChargeFaults = 0;                    // budget mode: the seam refused a mirror
		uint64_t MirroredRegionBytes = 0;                   // budget mode: bytes charged on region C
		uint64_t GhostBytes = 0;                            // ghost charges (nested entries, §5.4)
		uint32_t Ghosts = 0;
		uint64_t GhostEndings = 0;
		uint64_t NestedRequests = 0;
		// Explicit residency control (§5.6, rev 1.18): drops are counted apart from evictions.
		uint64_t Warms = 0;
		uint64_t Drops = 0;                                 // drops decided Dropped (evicted at their apply pass)
		uint64_t DropsDeferred = 0;                         // drops decided DropDeferred (the key marked)
		uint64_t MarksApplied = 0;                          // marks applied later (§5.6 *When a mark is applied*)
		uint64_t MarksCleared = 0;                          // marks cleared by a later request or warm
		uint64_t DropDeletes = 0;                           // a drop's disk part: deletes issued
		uint64_t WarmsByResult[11] = {};                    // resolved warms, indexed by EPrefixOpResult
	};

	// Explicit residency control (§5.6, rev 1.18, PC-16 (a)).
	enum class EPrefixDropScope : uint8_t
	{
		Memory = 0,
		MemoryAndDisk = 1,
	};

	enum class EPrefixOpResult : uint8_t
	{
		Pending = 0,          // until both parts of the op resolve; a Pending op never expires
		Resident = 1,         // a warm: any hit, or a completed load or build
		Dropped = 2,          // a drop: resident and unpinned at its apply pass, evicted there
		DropDeferred = 3,     // a drop: pinned, or its key in flight or waiting: the key is marked
		NotCached = 4,        // a drop: not resident, not in flight and not waiting
		BudgetExhausted = 5,  // a warm: the request's named refusals
		PoolExhausted = 6,
		UnknownSchema = 7,
		Cancelled = 8,        // a warm Pending at a re-Configure; a drop whose hash was pending then
		Expired = 9,          // a resolved op older than the last kPrefixOpRing resolved ops
		// U2 code review finding 4; §5.6 rev 1.18.5: a warm whose build failed; Reason carries the fallback reason
		// its request logs (BuildFailed for an engine failure). Appended last (the append rule). Cancelled stays
		// re-Configure() only.
		Failed = 10,
	};

	enum class EPrefixDropDisk : uint8_t
	{
		NotRequested = 0,     // a Memory drop, or a drop cancelled before its disk part ran
		Deleted = 1,
		NoFile = 2,
		DiskTierOff = 3,
		Failed = 4,           // the file API refused the delete (logged once)
	};

	struct FPrefixOpResult
	{
		EPrefixOpResult Result = EPrefixOpResult::Pending;
		EPrefixDropDisk Disk = EPrefixDropDisk::NotRequested;
		EPrefixFallback Reason = EPrefixFallback::Count; // meaningful only when Result is Failed (finding 4)
	};

	// The ring of resolved ops whose results stay readable (§5.6: the last 1,024).
	constexpr uint32_t kPrefixOpRing = 1024;

	class FSuperSLMPrefixCacheCore
	{
	public:
		// PrefixIdCounter: the plugin's prefix-id counter (GNextPrefixId). Lease handles are minted from
		// it, so a cache handle never shares an id with a CreatePrefix handle. It must outlive the core.
		template <typename TInt>
		explicit FSuperSLMPrefixCacheCore(TInt* PrefixIdCounter) : FSuperSLMPrefixCacheCore(FResidencyHandleCounter(PrefixIdCounter))
		{
		}
		explicit FSuperSLMPrefixCacheCore(const FResidencyHandleCounter& PrefixIdCounter);
		~FSuperSLMPrefixCacheCore();
		FSuperSLMPrefixCacheCore(const FSuperSLMPrefixCacheCore&) = delete;
		FSuperSLMPrefixCacheCore& operator=(const FSuperSLMPrefixCacheCore&) = delete;

		// Valid when never configured or IsTornDown(); false otherwise. Starts from an empty cache with
		// every counter at zero, and emits TrimDisk when the disk tier is on (§5.7: trimmed at Configure()).
		bool Configure(const FPrefixCacheConfig& Config);
		bool IsConfigured() const;

		// RequestPrefix. Copies the spec, mints one lease handle, emits ComputeKey. Hashes nothing.
		EPrefixRequestResult Request(const FPrefixSpec& Spec, int64_t& OutHandle, std::string& OutError);

		// ReturnPrefix. True when it ends a lease (Pending, Ready, or resolved to a refusal); the entry
		// stays cached until evicted. False, with one ReturnNotOwned log line, for anything else.
		bool Return(int64_t Handle);

		// A lease of an ended configuration reads Unknown, except one that was still Pending when that
		// configuration ended: it resolved Cancelled by name (cell 1.7, §5.11 (a)'s waiters), and reads Cancelled
		// afterwards too, as a warm op does (§5.6 *Generations and teardown*), until kPrefixOpRing later such leases
		// drop it. Every other verb treats it as a handle of no configuration (cell 2.10).
		EPrefixLeaseState GetLeaseState(int64_t Handle) const;
		// The key of the entry a Ready lease holds (a nested spec's parent key, as the host's disk-file
		// seam needs it). False for any other handle.
		bool GetLeaseKey(int64_t Handle, FPrefixKey& OutKey) const;
		// The residency id of the entry a live lease is attached to (Pending once its key is applied, or Ready);
		// 0 for any other handle. The id a Build or Restore for that entry carries in FPrefixCommand::EntryId.
		uint64_t GetLeaseEntryId(int64_t Handle) const;
		EPrefixSource GetSource(int64_t Handle) const;

		// ReleasePrefix's check (§5.10). True when Handle is any handle this cache minted since its last
		// Configure, leased or not: the caller must refuse, and OutError says "owned by the prefix
		// cache; call ReturnPrefix". Raises ReleaseOfCacheHandle. False: not ours, OutError untouched.
		bool RefuseRelease(int64_t Handle, std::string& OutError);

		// AdoptPrefix's cache lookup (§5.6, §5.10). On Adoptable the entry is Adopt-pinned and
		// OutAdoptToken identifies that pin for EndAdopt. PrefixLeaseEnded raises AdoptOfEndedLease.
		// Any other result sets both outputs to 0. It does not touch the entry's LRU position: only a
		// request (its hit or its attach) and an entry becoming Ready do. OutEntryId, when given, is set
		// to the entry's residency id on Adoptable (else 0): one per entry, never reused, so it is the
		// paging ledger's cache-entry key (an engine handle's address can be reused once released).
		EPrefixAdoptCheck BeginAdopt(int64_t Handle, uint64_t& OutEngineHandle, uint64_t& OutAdoptToken,
			uint64_t* OutEntryId = nullptr);

		// Explicit residency control (§5.6, rev 1.18). Both are game-thread calls that hash nothing: each
		// takes an issue sequence number from the counter Request() takes it from, emits ComputeKey with
		// OpId set, and resolves at a later apply pass, in issue order (*What "later" means*).
		// WarmPrefix: a request that takes no lease. Refused synchronously exactly where Request() is (and
		// then OutOpId is 0); otherwise Accepted, OutOpId names the op, and it runs Request()'s path unchanged
		// (tiers, charge rule, waiters, coalescing, write-back); resolved, the entry is resident and unpinned.
		EPrefixRequestResult Warm(const FPrefixSpec& Spec, uint64_t& OutOpId, std::string& OutError);
		// DropPrefix: refused synchronously only for a malformed spec (or NotConfigured). The memory part is
		// decided at the apply pass that receives the key: Dropped, DropDeferred (marked) or NotCached; the disk
		// part (MemoryAndDisk) deletes the key's file on the key's serialization, after any write-back or read.
		EPrefixRequestResult Drop(const FPrefixSpec& Spec, EPrefixDropScope Scope, uint64_t& OutOpId, std::string& OutError);
		// GetPrefixOpResult. False for an id this core never issued. Op ids never repeat within the core's
		// lifetime; a resolved op keeps its result across a re-Configure until it leaves the ring, then reads
		// Expired; a Pending op never expires.
		bool GetOpResult(uint64_t OpId, FPrefixOpResult& Out) const;
		// The end of Tick()'s apply pass, after every completion of the pass has been applied (step 1): steps 2
		// and 3 of §5.6 *When a mark is applied* (mark applications queued after every op issued before this
		// pass, then the issue-ordered queue drained while its head's hash has completed).
		void EndApplyPass();
		// The adopt drained (ran or was cancelled). False for an unknown or already-ended token.
		bool EndAdopt(uint64_t AdoptToken);

		// The set's model against the configured model (§5.8 *The set's model against the subsystem's*; §5.9 rev 1.18.4).
		// False, raising PrefixSetModelMismatch once with SetName and both artifact hashes in its line, when the cache
		// is configured and SetModelArtifactHash is not its identity's: the caller registers nothing. True otherwise
		// (never configured: there is no configured model to refuse against yet).
		bool CheckPrefixSetModel(const std::string& SetName, const std::array<uint8_t, 32>& SetModelArtifactHash);

		// The cooked tier (§5.3 item 2; sets themselves are U3). Key is the wrapper's key field.
		void RegisterCooked(const FPrefixKey& Key, uint64_t CookedBlobId);
		void UnregisterCooked(uint64_t CookedBlobId);
		bool IsCookedStale(uint64_t CookedBlobId) const;

		// Commands emitted since the last call, in emission order. Taking a command puts it in flight.
		std::vector<FPrefixCommand> TakeCommands();
		// Applies one completion. A completion for an id not in flight is ignored.
		void Apply(const FPrefixCompletion& Completion);
		uint32_t InFlightCommands() const;

		void BeginTeardown();
		// The executor's teardown has begun (the host calls it before it delivers anything: the CPU subsystem's
		// teardown delivers every finished lane job, and the host's sweep applies its IO, before Abandon()). Every
		// lease Pending now is the configuration's queued or running request: whatever a completion delivered
		// during the teardown resolves it to, Abandon() resolves it Cancelled and it reads Cancelled after the
		// next Configure() (§5.6, cell 1.7's running legs; code review r2 B1). A lease already Ready, Ended or
		// resolved is untouched (cell 2.10). Nothing if not configured; idempotent.
		void MarkTeardownBegins();
		bool IsTornDown() const;
		// The executor has drained every lane and the pool is being destroyed (every engine handle is
		// released with it). Ends the configuration at once: every lease not Ended resolves Cancelled,
		// every record and command is forgotten, and later completions are stale. The executor frees
		// every payload ref it still holds. Then IsTornDown() is true. Nothing if never configured.
		void Abandon();

		// Drops whose delete has not completed (queued, waiting on a write-back, or in flight). A re-Configure
		// drains them before Abandon() (§5.4: a drop's delete is drained, never cancelled); any still pending at
		// Abandon() are carried and issued at the next Configure().
		uint32_t PendingDropDeletes() const;

		FPrefixCacheDiagnostics GetDiagnostics() const;
		// Log lines written since the last call.
		std::vector<FPrefixLogLine> TakeLog();

		// The kind-neutral core this cache keeps its entries, ledger, pins and leases in.
		const FSuperSLMResidencyCore& Residency() const;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
