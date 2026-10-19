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
//   evictions -> Release (worker lane); the charge stays until the Release completes
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
//   - an admission that fits once already-evicted entries finish releasing waits for those releases
//     and is then emitted (release before reuse); it is never refused for bytes that are coming back;
//   - a refused or failed restore or build returns its whole charge when its result is applied;
//   - a restore or build the pool refuses (PoolExhausted) evicts the next LRU unpinned entry and is
//     retried after that entry's Release completes, until it succeeds or nothing is evictable; then
//     it resolves PoolExhausted by name and returns its charge.
// So ChargedBytes never exceeds BudgetBytes, and every page the cache holds or may draw is charged.
//
// Pins (§5.4): a lease, a queued or running adopt (BeginAdopt..EndAdopt), and a queued or running
// save. A pinned entry is never evicted. Nested specs are refused NestedRequiresBudgetMode (U2 builds
// no nested entry), so no ghost charge arises here.
//
// Teardown and re-Configure (§5.4 last bullet, cells 1.5 and 1.7): BeginTeardown() refuses new
// requests, resolves every pending lease Cancelled, drops every command not yet taken, and emits no
// new work but Release and DropPayload. Every command already taken by the executor is in flight
// and must complete; an engine handle a completion delivers during teardown is released, and a
// payload ref it delivers is dropped. Only when nothing is in flight and no adopt is open does the
// core emit one Release per engine handle it holds; every Ready lease then resolves Cancelled. When
// those complete, IsTornDown() is true and Configure() may be called again. Handles from before are
// then Unknown to every call.

#include "SuperSLMPrefixCacheKey.h"
#include "SuperSLMPrefixCacheWrapper.h"
#include "SuperSLMPrefixDiskCache.h"
#include "SuperSLMResidencyCore.h"

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
	};

	// The request a caller makes (FSuperSLMPrefixSpec). ParentHandle 0 = a root prefix.
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
		Malformed = 2,                // empty, a token outside [0, VocabSize), or longer than ContextCap
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
		Restore = 3,        // worker lane: sslm_prefix_restore of PayloadRef (L = ContextLength)
		Build = 4,          // worker lane: begin_budgeted(ContextLength), bind Spec.SchemaName, prefill Spec.Tokens, freeze
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
		int32_t ContextLength = 0;        // Restore, Build: L
		uint64_t WriteSerial = 0;         // WriteDisk: unique per write within this instance
	};

	enum class EPrefixEngineResult : uint8_t
	{
		Ok = 0,
		PoolExhausted = 1,  // SSLM_KV_POOL_EXHAUSTED: nothing was drawn
		UnknownSchema = 2,  // Build: the schema name did not resolve
		Refused = 3,        // Restore: the engine refused the blob (digest, producer, model...): a miss
		Failed = 4,         // anything else
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
	};

	struct FPrefixCacheDiagnostics
	{
		uint64_t BudgetBytes = 0;
		uint64_t ChargedBytes = 0;
		uint64_t ResidentBytes = 0;
		uint64_t InFlightBytes = 0;
		uint64_t ReleasingBytes = 0;
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
	};

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

		EPrefixLeaseState GetLeaseState(int64_t Handle) const;
		EPrefixSource GetSource(int64_t Handle) const;

		// ReleasePrefix's check (§5.10). True when Handle is any handle this cache minted since its last
		// Configure, leased or not: the caller must refuse, and OutError says "owned by the prefix
		// cache; call ReturnPrefix". Raises ReleaseOfCacheHandle. False: not ours, OutError untouched.
		bool RefuseRelease(int64_t Handle, std::string& OutError);

		// AdoptPrefix's cache lookup (§5.6, §5.10). On Adoptable the entry is Adopt-pinned and
		// OutAdoptToken identifies that pin for EndAdopt. PrefixLeaseEnded raises AdoptOfEndedLease.
		// Any other result sets both outputs to 0. It does not touch the entry's LRU position: only a
		// request (its hit or its attach) and an entry becoming Ready do.
		EPrefixAdoptCheck BeginAdopt(int64_t Handle, uint64_t& OutEngineHandle, uint64_t& OutAdoptToken);
		// The adopt drained (ran or was cancelled). False for an unknown or already-ended token.
		bool EndAdopt(uint64_t AdoptToken);

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
		bool IsTornDown() const;

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
