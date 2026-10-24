#pragma once

// Everything below the switch is preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on
// (SuperSLMUnreal.Build.cs turns it on: PublicDefinitions.Add("SUPERSLM_WITH_PREFIX_CACHE=1")).
//
// The prefix cache's public types (precache plan §5.6): a request spec, the tier that served a
// lease, a lease's state by name, and the diagnostics snapshot. Additive: no existing type changes.

#include "CoreMinimal.h"
#include "SuperSLMSequenceTypes.h"

#ifndef SUPERSLM_WITH_PREFIX_CACHE
#define SUPERSLM_WITH_PREFIX_CACHE 0
#endif

// Declared whatever the switch, so USuperSLMSubsystem's reflected header can name them without a
// preprocessor block inside its UCLASS. Defined below only with the switch on; the subsystem's
// prefix-cache members are defined only then too, and nothing calls them otherwise.
struct FSuperSLMPrefixSpec;
struct FSuperSLMPrefixCacheStats;
enum class ESuperSLMPrefixSource : uint8;
enum class ESuperSLMPrefixLeaseState : uint8;
class FSuperSLMPrefixCacheHost;
struct FSuperSLMGpuPrefixCacheSettings;
struct FSuperSLMPrefixOp;
struct FSuperSLMPrefixOpResult;
enum class ESuperSLMPrefixDropScope : uint8;

#if SUPERSLM_WITH_PREFIX_CACHE

// What RequestPrefix() is asked for. Parent invalid = a root. From step U6 a valid Parent names a live
// Ready lease of the same cache: Tokens are then the suffix after the parent's tokens and SchemaName
// must be NAME_None (a child takes its parent's binding). Nested specs need budget mode; legacy mode
// refuses them NestedRequiresBudgetMode. (Before U6's build every nested spec is refused so.)
struct SUPERSLMUNREAL_API FSuperSLMPrefixSpec
{
	TArray<int32> Tokens;
	FName SchemaName; // NAME_None = no schema
	FSuperSLMPrefix Parent;
};

// The tier that served a lease (§5.6). Diagnostic only: no behaviour reads it.
enum class ESuperSLMPrefixSource : uint8
{
	Pending,    // not Ready (yet, or ever)
	Memory,
	Cooked,
	Disk,
	Recomputed,
	Invalid,    // not a cache handle of this configuration
};

// A lease's state by name (§5.9). GetPrefixPhase() reads Pending/Ready/Faulted/Invalid for the same
// handle; this says which refusal.
enum class ESuperSLMPrefixLeaseState : uint8
{
	Invalid,          // not a cache handle, or one from before a re-Configure() that it did not resolve
	Pending,
	Ready,
	BudgetExhausted,  // the charge cannot fit, even with every evictable entry and every pending refund
	PoolExhausted,    // the pool was short and nothing more was evictable
	UnknownSchema,    // SchemaName did not resolve; no entry, no file
	Cancelled,        // resolved by a re-Configure() or Deinitialize() while Pending; reads Cancelled
	                  // across the next Configure() (a tombstone, as lifecycle handles keep theirs),
	                  // and every verb still refuses it as "not a live prefix handle" (§5.10)
	Failed,           // the build failed for another reason (the log names the status)
	Ended,            // returned (ReturnPrefix)
};

// GetPrefixCacheStats() (§5.6 diagnostics). Game-thread data; bytes and counts since Configure().
struct SUPERSLMUNREAL_API FSuperSLMPrefixCacheStats
{
	bool bConfigured = false;
	bool bBudgetMode = false;      // the cache's ledger is the page ledger's region C (§5.11 (b))
	int64 BudgetBytes = 0;         // budget mode: region C's capacity in bytes
	int64 ChargedBytes = 0;
	int64 ResidentBytes = 0;
	int64 InFlightBytes = 0;
	int64 PendingRefundBytes = 0;  // evicted entries whose release has not been applied
	int64 ClaimedRefundBytes = 0;
	int64 UnclaimedRefundBytes = 0;
	int64 WaitingBytes = 0;        // free charge held by waiting requests
	int32 ResidentEntries = 0;
	int32 Waiters = 0;
	int32 PendingRefunds = 0;
	int64 Requests = 0;
	int64 Evictions = 0;
	int64 RestoresIssued = 0;
	int64 BuildsIssued = 0;
	int64 ReleasesIssued = 0;
	int64 DiskWritesIssued = 0;
	int64 SpecMismatches = 0;
	int64 ServedBySource[6] = {};  // indexed by ESuperSLMPrefixSource
	TMap<FName, int64> Fallbacks;  // by the fallback's name, as its log line names it
	int64 StaleCompletionsDropped = 0;
	int32 CookedEntriesRegistered = 0; // entries of registered prefix sets in the cooked tier (step U3)
	int64 RegionChargeFaults = 0;
	// Nested entries (step U6, §5.4): evicted parents whose pages cached descendants still map keep
	// their charge as ghosts until their last cached descendant leaves.
	int64 GhostBytes = 0;
	int32 Ghosts = 0;
	int64 GhostEndings = 0;
	int64 NestedRequests = 0;
	// Explicit residency control (§5.6, rev 1.18): drops are counted apart from evictions.
	int64 Warms = 0;
	int64 Drops = 0;          // decided Dropped: evicted at their apply pass
	int64 DropsDeferred = 0;  // decided DropDeferred: the key marked
	int64 MarksApplied = 0;   // marks applied later (§5.6 *When a mark is applied*)
	int64 MarksCleared = 0;   // marks cleared by a later request or warm
	int64 DropDeletes = 0;    // MemoryAndDisk drops: deletes issued
	// Prefix prefills (precache U4, plan §15.26): the cache's own count of the prefill work its builds ran on the
	// worker lanes, in every build configuration (Development and Shipping included). A packaged game has no
	// engine test counter (sslm_test_only_prefix_prefill_call_count is a test-only engine build), so the
	// packaged cells read these instead. They count the cache's builds only: a CreatePrefix() prefill, which the
	// engine's counter also saw, is not counted. Counted at the lane, after the call returns, so a build that a
	// seam forced, or that failed before its first prefill, adds nothing. Cumulative since the cache host was
	// created, so read them as differences. On the CPU the host outlives a re-Configure(), so the counts carry
	// across it; on the GPU the host belongs to its configuration, so a GPU re-Configure() starts both from 0.
	int64 PrefixPrefillCalls = 0;  // sslm_prefix_prefill calls (one per chunk)
	int64 PrefixPrefillBuilds = 0; // builds that made at least one of them (one per recomputed prefix)
};

// Explicit residency control (§5.6, rev 1.18, PC-16 (a)): WarmPrefix() and DropPrefix() return an op at once;
// it resolves at a later tick (the key is hashed on a pool thread, §5.5). GetPrefixOpResult() reads it.
enum class ESuperSLMPrefixDropScope : uint8
{
	Memory,
	MemoryAndDisk,
};

enum class ESuperSLMPrefixOpResult : uint8
{
	Pending,          // until both parts of the op resolve; a Pending op never expires
	Resident,         // a warm: any hit, or a completed load or build
	Dropped,          // a drop: resident and unpinned at its apply pass, evicted there
	DropDeferred,     // a drop: pinned, or its key in flight or waiting: the key is marked
	NotCached,        // a drop: not resident, not in flight and not waiting
	BudgetExhausted,  // a warm: the request's named refusals
	PoolExhausted,
	UnknownSchema,
	Cancelled,        // a warm still Pending at a re-Configure(); a drop whose hash was pending then
	Expired,          // a resolved op older than the last 1,024 resolved ops
	// A warm whose build failed (U2 code review finding 4; plan §5.6 rev 1.18.5); Reason names the fallback its
	// request logs (BuildFailed). Appended last; Cancelled stays re-Configure() only.
	Failed,
};

enum class ESuperSLMPrefixDropDisk : uint8
{
	NotRequested,     // a Memory drop, or a drop cancelled before its disk part ran
	Deleted,
	NoFile,
	DiskTierOff,
	Failed,           // the file API refused the delete (logged once, DropDeleteFailed)
};

// An op handle. Ids never repeat within the subsystem's lifetime; 0 = none.
struct SUPERSLMUNREAL_API FSuperSLMPrefixOp
{
	int64 Id = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMPrefixOpResult
{
	ESuperSLMPrefixOpResult Result = ESuperSLMPrefixOpResult::Pending;
	ESuperSLMPrefixDropDisk Disk = ESuperSLMPrefixDropDisk::NotRequested;
	FName Reason;     // Result == Failed: the fallback's name, as its log line names it (finding 4)
};

// The GPU subsystem's cache settings (step U6): USuperSLMGpuSubsystem::ConfigurePrefixCache(), applied
// at the next Configure(). The GPU's runtime config carries no cache fields, so its reflected struct is
// unchanged by the switch. Pooled mode only; BudgetBytes 0 = no cache.
struct SUPERSLMUNREAL_API FSuperSLMGpuPrefixCacheSettings
{
	int64 BudgetBytes = 0;
	bool bDiskEnabled = false;
	FString DiskDirectory;   // empty: <ProjectSaved>/SuperSLM/PrefixCacheGpu
	int64 DiskMaxBytes = 0;
};

#endif // SUPERSLM_WITH_PREFIX_CACHE
