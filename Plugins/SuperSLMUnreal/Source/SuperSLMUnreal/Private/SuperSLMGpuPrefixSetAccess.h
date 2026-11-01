#pragma once

// Module-private; preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
// What a cooked prefix set's GPU build (SuperSLMPrefixSetTools::BeginBuildGpu, PumpBuildGpu; precache plan §5.8,
// step U7) reads of a GPU subsystem's private state, and the one call it makes through it: a friend of
// USuperSLMGpuSubsystem, defined in SuperSLMGpuSubsystemPrefixCache.inl (inert where the GPU backend is not built).
// Game thread only.

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Core/SuperSLMPrefixCacheKey.h"

class USuperSLMGpuSubsystem;

struct FSuperSLMGpuPrefixSetAccess
{
	// The GPU cache a build runs through: its engine identity (backend 1, the adapter's vendor and device, the Layer-1
	// pin, the configured model's artifact hash), whether its disk tier is on, and how many cooked entries are
	// registered with it. False, with OutError, when Gpu has no GPU prefix cache, its device was lost, or this build
	// has no GPU prefix save.
	static bool ReadBuildCache(const USuperSLMGpuSubsystem& Gpu, SuperSLMCore::FPrefixEngineIdentity& OutIdentity, bool& bOutDiskEnabled,
		int32& OutCookedEntries, FString& OutError);

	// Saves the prefix a Ready lease of Gpu's cache holds: its entry pinned (BeginCacheAdopt), sslm_gpu_prefix_save on
	// the submission thread with two-call sizing (RunSync), the pin released (EndCacheAdopt). OutPayload is the SPX1
	// blob. bOutOnGameThread reads whether the save ran on the game thread (it never should; cell 3.11's counter).
	// False, with OutError and OutPayload empty, for a lease that is not Ready, a save the engine refused, or a
	// submission thread that did not run it.
	static bool SaveLease(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, TArray<uint8>& OutPayload, bool& bOutOnGameThread, FString& OutError);

	// How many GPU subsystems in the process are configured now.
	static int32 ConfiguredSubsystemCount();
};

#endif // SUPERSLM_WITH_PREFIX_CACHE
