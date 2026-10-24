#pragma once

// Module-private: the asset's entries and
// builds as the host-tested core's types (defined in SuperSLMPrefixSetTools.cpp). The prefix cache host
// registers a set's entries through it.

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Core/SuperSLMPrefixSetCore.h"

#include <array>
#include <cstdint>
#include <vector>

class USuperSLMPrefixSet;

namespace SuperSLMPrefixSetBridge
{
	std::vector<SuperSLMCore::FPrefixSetBuild> CoreBuilds(const USuperSLMPrefixSet& Set);
	// The authored entries, Text entries' stored ids and hashes included (§5.8, rev 1.18).
	std::vector<SuperSLMCore::FPrefixSetEntry> CoreEntries(const USuperSLMPrefixSet& Set);
	// The set's model's artifact hash; false (zeros) for no model or an unreadable header.
	bool SetModelHash(const USuperSLMPrefixSet& Set, std::array<uint8_t, 32>& Out);
}

#endif // SUPERSLM_WITH_PREFIX_CACHE
