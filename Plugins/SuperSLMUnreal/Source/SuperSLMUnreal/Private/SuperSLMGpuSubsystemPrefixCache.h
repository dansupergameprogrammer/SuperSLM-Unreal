#pragma once

// Included only by
// SuperSLMGpuSubsystem.cpp, and only with SUPERSLM_WITH_PREFIX_CACHE on.
//
// The GPU subsystem's prefix-cache glue (precache plan §5.5, §5.6, §5.11; step U6): the hooks
// SuperSLMGpuSubsystem.cpp calls at its existing sites, over FSuperSLMGpuPrefixCacheHost. Bodies in
// SuperSLMGpuSubsystemPrefixCache.inl, included at the end of SuperSLMGpuSubsystem.cpp so they see
// FSuperSLMGpuSubsystemState (its Ctx, Model, Pool, Ledger, Schemas, Enqueue and RunSync).
//
// Partly wired: SuperSLMGpuSubsystem.cpp has edits 1 (the state member) and 5 (the .inl) only, so no host is
// installed and the GPU cache methods refuse NotConfigured. Edits 2-4 connect it. Each is under
// #if SUPERSLM_WITH_PREFIX_CACHE:
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
		uint32 RegionCPages = 0;
		FSuperSLMGpuPrefixCacheSettings Settings; // ConfigurePrefixCache()'s, kept across Configure()
	};

	// Region C in pages for a pooled configuration: ceil(budget bytes / block bytes) blocks of
	// ceil(ContextCap / B) pages each; 0 for no cache or dedicated mode.
	uint32 RegionCPagesFor(int64 BudgetBytes, int64 PageBytes, int64 PagePositions, int64 ContextCap);

	// Configure()'s cache step: identity (backend 1, the adapter's ids), region C, nested entries.
	bool Install(FSuperSLMGpuSubsystemState& S, int64* PrefixIdCounter, const FSuperSLMGpuPrefixCacheSettings& Settings, FString& OutError);
	void Pump(FSuperSLMGpuSubsystemState& S);
	void DispatchLaneJobs(FSuperSLMGpuSubsystemState& S);
	void BeforePoolDestroy(FSuperSLMGpuSubsystemState& S);
}

#endif // SUPERSLMUNREAL_WITH_GPU
