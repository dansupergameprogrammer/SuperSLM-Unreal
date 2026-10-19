#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
// Observes the hook value actually constructed for Configure() and its real run invocations.
// Counts are reset only by a test, after the preceding workload has drained.
struct SUPERSLMUNREAL_API FSuperSLMParallelDecodeTestAccess
{
	static void ResetHookObservations();
	static void StopHookObservations();
	static int32 GetMakeCount();
	static int32 GetZeroBitsMakeCount();
	static int32 GetMatvecBitsMakeCount();
	static uint32 GetLastMadeBits();
	static int32 GetRunCount();
};
#endif
