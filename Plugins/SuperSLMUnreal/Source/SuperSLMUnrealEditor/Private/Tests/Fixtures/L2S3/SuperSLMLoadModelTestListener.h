#pragma once

// Test-only (T-2818 L2-S3): receives "Load SuperSLM Model"'s Loaded/Failed pins, which are
// dynamic multicast delegates and so bind only to a UFUNCTION on a UObject. Used by
// SuperSLML2S3LoadModelNodeTests.cpp; nothing in the product references it. Outside the
// WITH_DEV_AUTOMATION_TESTS guard because UHT does not honour that guard around a UCLASS.

#include "CoreMinimal.h"
#include "SuperSLMLoadModelAsyncAction.h"
#include "UObject/Object.h"
#include "SuperSLMLoadModelTestListener.generated.h"

UCLASS(Transient)
class USuperSLMLoadModelTestListener : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnLoaded(const FSuperSLMLoadModelResultBP& InResult) { Record(true, InResult); }

	UFUNCTION()
	void OnFailed(const FSuperSLMLoadModelResultBP& InResult) { Record(false, InResult); }

	void Reset() { Calls = 0; bLoaded = false; Result = FSuperSLMLoadModelResultBP(); }

	int32 Calls = 0;          // pins fired; exactly one per load
	bool bLoaded = false;     // the last pin was Loaded
	FSuperSLMLoadModelResultBP Result;

private:
	void Record(bool bInLoaded, const FSuperSLMLoadModelResultBP& InResult)
	{
		++Calls;
		bLoaded = bInLoaded;
		Result = InResult;
	}
};
