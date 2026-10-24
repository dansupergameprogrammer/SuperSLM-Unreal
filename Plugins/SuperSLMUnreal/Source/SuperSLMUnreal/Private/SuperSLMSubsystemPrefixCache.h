#pragma once

// Included only by
// SuperSLMSubsystem.cpp, and only with SUPERSLM_WITH_PREFIX_CACHE on.
//
// The CPU subsystem's prefix-cache glue (precache plan §5.5, §5.6, §5.11): the per-configuration
// state the subsystem state carries, and the hooks SuperSLMSubsystem.cpp calls at its existing
// sites. Their bodies live in SuperSLMSubsystemPrefixCache.inl, included at the end of
// SuperSLMSubsystem.cpp, so they see the file's own internals (the lanes, PostLedgerJob, the holds).

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheHost.h"
#include "Core/SuperSLMPageLedgerCore.h"
#include "superslm/sslm_abi.h"

struct FSuperSLMSubsystemState;
struct FSuperSLMRuntimeConfig;

namespace SuperSLMPrefixCacheGlue
{
	// One per subsystem state (one configuration).
	struct FState
	{
		// Not owned: USuperSLMSubsystem::PrefixCacheHost, set by InstallPrefixCache() when the state
		// is published. Null for a state that was never published, and when no cache is configured.
		FSuperSLMPrefixCacheHost* Host = nullptr;

		// The lane job taken from the host and parked at its HoldStart() hold, with its holds.
		TOptional<FSuperSLMPrefixLaneJob> Parked;
		int64 ParkedStartHoldId = 0;
		int64 ParkedApplyHoldId = 0;

		// Cache leases adopted through AdoptPrefix(): the subsystem prefix entry that stands in for
		// the lease while adopts of it are queued (it never owns the engine handle), the adopt tokens
		// the host issued, one per queued adopt, and the lease's length for the ledger's
		// AdmitAdoptCacheEntry. Keyed by the lease's handle id.
		struct FAdoptShadow
		{
			TArray<uint64> Tokens;
			uint64 EntryKey = 0; // the paging ledger's cache-entry key: the entry's residency id, one per
			                     // entry (never per lease) and never reused, unlike the handle's address
		};
		TMap<int64, FAdoptShadow> Shadows;
		TMap<int64, int32> LeaseLength;

		// Region C's pages in budget mode (0 in legacy mode), for the test seam.
		uint32 RegionCPages = 0;
	};

	// Legacy mode: the blocks the pool gains for the cache, ceil(budget / block bytes).
	int64 LegacyCacheBlocks(const FSuperSLMRuntimeConfig& Config, sslm_model Model);
	// Budget mode: the ledger's region C, configured from the budget.
	void ConfigureRegionC(const FSuperSLMRuntimeConfig& Config, sslm_model Model, int64 PagePositions, int64 ContextCap,
		SuperSLMPaging::FPageLedgerConfig& LedgerConfig);

	// Tick: applies arrived completions (the apply pass), then posts lane jobs (the plan pass).
	void Pump(FSuperSLMSubsystemState& S);

	// Tick, the plan pass's first step (precache plan rev 1.18.10, §15.29): marks each lane a sliced cache build's next
	// chunk waits for (FLane::ChunkWaitingAfterJobId), so the lane alternates between the chunk and other jobs.
	void NoteWaitingChunks(FSuperSLMSubsystemState& S);
	void DispatchLaneJobs(FSuperSLMSubsystemState& S, int32 TickIndex);

	// Teardown (DestroyCpuStateContents): at its entry, before anything is delivered (TeardownBegins);
	// after the lanes are delivered and before any handle is released; then just before, and just after,
	// the pool destroy.
	void TeardownBegins(FSuperSLMSubsystemState& S);
	void BeforeHandleRelease(FSuperSLMSubsystemState& S);
	void BeforePoolDestroy(FSuperSLMSubsystemState& S);
	void AfterPoolDestroy(FSuperSLMSubsystemState& S, sslm_status Status);

	// The subsystem's own prefix lifecycle, reported to the release-counting seam.
	void NoteLegacyBegin(FSuperSLMSubsystemState& S, sslm_prefix Handle);
	void NoteRelease(FSuperSLMSubsystemState& S, sslm_prefix Handle);

	// AdoptPrefix(): true when Prefix is a cache lease the call must stop on (OutResult set); false
	// to continue into the subsystem's one adopt path (a shadow entry now stands in for an
	// Adoptable lease, or the handle is not the cache's).
	bool StopCacheAdopt(FSuperSLMSubsystemState& S, int64 PrefixId, ESuperSLMRestoreResult& OutResult, FString& OutError);
	// The adopt's ledger admission: true when PrefixId is a cache lease (OutAdmission set).
	bool AdmitCacheAdopt(FSuperSLMSubsystemState& S, int64 PrefixId, int32 SlotIndex, SuperSLMPaging::FAdmission& OutAdmission);
	// The adopt's delivery: ends one adopt of a cache lease; the shadow goes with the last.
	void OnAdoptDelivered(FSuperSLMSubsystemState& S, int64 PrefixId);

	// A shadow entry (a cache lease standing in State->Prefixes while adopts of it are queued) is not
	// a 1.0.0 prefix: it never counts against PrefixBlockCount (precache plan §5.4, PC-11) and is
	// never a restore hint (its LedgerNode is none, and only the cache's pins keep its handle alive).
	int32 ShadowCount(const FSuperSLMSubsystemState& S);
	bool IsShadow(const FSuperSLMSubsystemState& S, int64 PrefixId);

	// A leased cache handle as a restore hint (§5.11 (b) *Adopt and hint*, rev 1.18, PC-15 Q5; cell 8.15). QueueRestore()
	// pins a Ready lease's entry with an adopt token for the restore's whole life (true, Out set); the ledger's admission
	// takes it as an EHintKind::CacheEntry hint, so paging's A pre-charge covers the shared pages; the job passes its
	// engine handle to sslm_seq_restore_shared. The pin ends when the restore delivers or is refused. False: no hint
	// (not a cache lease, not Ready, or ended), and the restore is private.
	struct FCacheRestoreHint
	{
		uint64 Token = 0;
		sslm_prefix Handle = nullptr;
		uint64 EntryKey = 0;
		int64 Length = 0;
	};
	bool PinCacheRestoreHint(FSuperSLMSubsystemState& S, int64 PrefixId, FCacheRestoreHint& Out);
	void UnpinCacheRestoreHint(FSuperSLMSubsystemState& S, uint64 Token);
	// Every applied sequence restore's *out_shared_pages, for the test seam (cell 8.15).
	void NoteSequenceRestoreApplied(FSuperSLMSubsystemState& S, uint32 SharedPages);
#if WITH_DEV_AUTOMATION_TESTS
	// The build jobs stopped at the build hold now: the lane-park barrier counts them as parked (§8).
	int32 HeldBuildsForBarrier(const FSuperSLMSubsystemState& S);
#endif

	// ReleasePrefix(): true (refuse) for a handle the cache minted.
	bool RefuseRelease(FSuperSLMSubsystemState& S, const FSuperSLMPrefix& Prefix, FString& OutError);

	// QueueRestore()'s Layer-1 check, through the shipped compatibility table (§5.8): true to accept.
	bool AcceptSaveLayer1(const FString& WrapperTag, const FString& WrapperCommit, const uint8* Payload, int64 PayloadBytes);
}
