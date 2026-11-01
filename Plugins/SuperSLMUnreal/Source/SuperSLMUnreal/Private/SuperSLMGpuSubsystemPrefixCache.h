#pragma once

// Included only by
// SuperSLMGpuSubsystem.cpp, and only with SUPERSLM_WITH_PREFIX_CACHE on.
//
// The GPU subsystem's prefix-cache glue (precache plan §5.5, §5.6, §5.11; step U6): the hooks
// SuperSLMGpuSubsystem.cpp calls at its existing sites, over FSuperSLMGpuPrefixCacheHost. Bodies in
// SuperSLMGpuSubsystemPrefixCache.inl, included at the end of SuperSLMGpuSubsystem.cpp so they see
// FSuperSLMGpuSubsystemState (its Ctx, Model, Pool, Ledger, Schemas, Enqueue and RunSync).
//
// Wired (precache U6): SuperSLMGpuSubsystem.cpp carries the edits below, each under #if SUPERSLM_WITH_PREFIX_CACHE.
// A host is installed for a pooled configuration with a budget; dedicated mode with a budget refuses
// PrefixCacheRequiresPooled (R39) and leaves no host, so the cache methods refuse NotConfigured naming that reason.
//   1. FSuperSLMGpuSubsystemState gains `SuperSLMGpuPrefixCacheGlue::FState PrefixCache;`, and
//      CacheRegionPages() becomes a per-configuration value (0 when no cache, or dedicated mode):
//      RegionCPagesFor(Config, PageGeometry), sized as the CPU's region C (one ledger block per
//      started block of budget bytes). The pool becomes S + P + H + C pages, and the ledger's config
//      sets bCacheConfigured and CacheRegionPages from it.
//   2. Configure(), after the pool and ledger exist: Install(*State, Host, Settings).
//   3. Tick(), in the apply pass after the job deliveries: Pump(*State); in the plan pass:
//      DispatchLaneJobs(*State) (the cache's lane jobs are FJobs on the submission thread, posted
//      after the tick job, so a cache job never delays a frame-path call it was not ahead of).
//   4. Teardown, after the submission thread has drained and before any prefix release and the pool
//      destroy: BeforePoolDestroy(*State).
//   5. USuperSLMGpuSubsystem's prefix-cache methods (declared in SuperSLMGpuSubsystem.h) are defined
//      in the .inl and refuse NotConfigured while no cache is installed. Include the .inl at the very
//      end of the file, AFTER `#endif // SUPERSLMUNREAL_WITH_GPU`: with SUPERSLMUNREAL_WITH_GPU 0 it
//      defines the six methods as refusals and the GPU test seams as inert, so a non-Win64 build with
//      the switch on still links (the editor's GPU cells call them on every platform).
//   6. ReleasePrefix() refuses a cache handle (RefuseRelease), and AdoptPrefix()/the hinted restore
//      accept a Ready lease through BeginAdopt/EndAdopt, as the CPU glue does (StopCacheAdopt).
//   7. (Precache U7.) Configure() and BeginConfigure()'s continuation call ReRegisterPrefixSets() after Install(): the
//      cooked sets RegisterPrefixSet() kept on the subsystem are registered with the new configuration's host, whose
//      identity (backend 1, Layer 1's adapter's vendor and device) selects the build.

#include "CoreMinimal.h"
#include "SuperSLMGpuPrefixCacheHost.h"

#if SUPERSLMUNREAL_WITH_GPU

struct FSuperSLMGpuSubsystemState;

namespace SuperSLMGpuPrefixCacheGlue
{
	struct FState
	{
		// Owned: the GPU cache instance; null when none is configured.
		TUniquePtr<FSuperSLMGpuPrefixCacheHost> Host;
		// Lane jobs posted to the submission thread and not yet delivered, oldest first. Delivered in
		// order, on the game thread, once their FJob reads done.
		struct FPosted
		{
			FSuperSLMPrefixLaneJob Job;
			TSharedPtr<void> Ticket; // the subsystem's FJob, type-erased here
		};
		TArray<FPosted> Posted;
		int64 PostedCount = 0; // lane jobs posted since Install() (every step of a build counts; a test reading)
#if WITH_DEV_AUTOMATION_TESTS
		// Cell 5.13's readings: a bound in place of the frame-path bound for cache jobs (0: the default), and each cache
		// job's wall time on the submission thread, appended there.
		double TestJobBoundSeconds = 0.0;
		FCriticalSection TestJobSecondsGuard;
		TArray<double> TestJobSeconds;
		// Cell 5.12's PoolExhausted row (FSuperSLMPrefixCacheTestSeam::ShortGpuPool): sequences created on the pool outside
		// the cache and the ledger, held until RestoreGpuPool or the teardown (BeforePoolDestroy releases them first).
		// Touched only on the submission thread.
		TArray<SslmGpuSequenceHandle*> TestPoolShortHolders;
#endif
		uint32 RegionCPages = 0;
		FSuperSLMGpuPrefixCacheSettings Settings; // ConfigurePrefixCache()'s, kept across Configure()
		FString NotConfiguredReason; // why Install() refused this configuration (RequestPrefix()'s NotConfigured names it)
		// Each live lease's length (its parent's length plus its own tokens), as the CPU glue's: an adopt of the lease is
		// admitted against its entry for that many positions (AdmitAdoptCacheEntry). Keyed by the lease's id.
		TMap<int64, int32> LeaseLength;
	};

	// Region C in pages for a pooled configuration: ceil(budget bytes / block bytes) blocks of
	// ceil(ContextCap / B) pages each; 0 for no cache or dedicated mode.
	uint32 RegionCPagesFor(int64 BudgetBytes, int64 PageBytes, int64 PagePositions, int64 ContextCap);

	// The next residency op id any GPU cache host may issue (game thread; defined in SuperSLMGpuSubsystemPrefixCache.inl):
	// raised as each host ends and carried into the next, so op ids never repeat within the subsystem's lifetime.
	extern uint64 GGpuPrefixNextOpId;

	// Configure()'s cache step: identity (backend 1, the adapter's ids), region C, nested entries.
	bool Install(FSuperSLMGpuSubsystemState& S, int64* PrefixIdCounter, const FSuperSLMGpuPrefixCacheSettings& Settings, FString& OutError);
	// bAbortSteps (the teardown's): a build still owing steps (Q7) is ended where it is and delivered failed.
	void Pump(FSuperSLMGpuSubsystemState& S, bool bAbortSteps = false);
	void DispatchLaneJobs(FSuperSLMGpuSubsystemState& S);
	// Returns the host, its handles released, for the subsystem to keep as its retired host (queries only).
	TUniquePtr<FSuperSLMGpuPrefixCacheHost> BeforePoolDestroy(FSuperSLMGpuSubsystemState& S);

	// Edit 6 (§5.6, §5.11 (b)): a cache lease adopted through RequestAdoptPrefix(Sequence, Prefix), as the CPU glue's
	// StopCacheAdopt. Adoptable pins the entry (its engine handle stays live) until EndCacheAdopt(Token), which the adopt's
	// resolution calls, whatever its outcome.
	enum class ECacheAdopt : uint8 { NotCacheHandle, Adoptable, NotReady, LeaseEnded };
	struct FCacheAdopt
	{
		uint64 Token = 0;
		uint64 EntryKey = 0;  // the ledger's cache-entry key (the entry's residency id)
		uint64 Engine = 0;    // the entry's SslmGpuPrefixHandle
		int64 Length = 0;     // the lease's length (LeaseLength)
	};
	ECacheAdopt BeginCacheAdopt(FSuperSLMGpuSubsystemState& S, int64 PrefixId, FCacheAdopt& Out, FString& OutError);
	void EndCacheAdopt(FSuperSLMGpuSubsystemState& S, uint64 Token);
	// ReleasePrefix()'s refusal of a handle the cache minted (ReturnPrefix() ends a lease).
	bool RefuseRelease(FSuperSLMGpuSubsystemState& S, const FSuperSLMPrefix& Prefix, FString& OutError);
}

#endif // SUPERSLMUNREAL_WITH_GPU
