#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
// Test-only tokenize-call counter (precache plan §8, cell 7.20; rev 1.18.1, plan review F14, audit G6). Counts every
// sslm_tokenize call the plugin makes, at both of its sites: SuperSLM::TokenizeText (SuperSLMDetokenizer.cpp, the
// prefix-set build's tokenize pass and any other stateless caller) and USuperSLMSubsystem::Tokenize
// (SuperSLMSubsystem.cpp). A retry after SSLM_BUFFER_TOO_SMALL is a second call and counts. Cell 7.20 reads 0 across
// Configure(), RegisterPrefixSet, FindEntrySpec, the request and the adopt (the runtime never tokenizes), and > 0 at
// the editor build; its must-reject makes one call through each site and reads each. Compiled only under
// WITH_DEV_AUTOMATION_TESTS (Development, not Shipping). Thread-safe counters; the sites are game-thread calls.
namespace SuperSLMTokenizeTestSeam
{
	enum class ESite : uint8
	{
		TokenizeText = 0,      // SuperSLM::TokenizeText
		SubsystemTokenize = 1, // USuperSLMSubsystem::Tokenize
	};

	// Calls at Site since the process started (or the last Reset()).
	SUPERSLMUNREAL_API int64 Calls(ESite Site);
	// Calls at both sites.
	SUPERSLMUNREAL_API int64 TotalCalls();
	SUPERSLMUNREAL_API void Reset();

	// The sites' own hook: one sslm_tokenize call at Site.
	SUPERSLMUNREAL_API void CountCall(ESite Site);
}
#endif // WITH_DEV_AUTOMATION_TESTS
