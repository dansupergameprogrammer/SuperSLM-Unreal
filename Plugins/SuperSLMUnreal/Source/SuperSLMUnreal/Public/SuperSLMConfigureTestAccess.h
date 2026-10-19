#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
// Test-only access to USuperSLMSubsystem::BeginConfigure()'s pool job (decode-threading D4, plan
// §10.3.1 item 5b, D-SLM8144, D-SLM8165). Compiled only under WITH_DEV_AUTOMATION_TESTS. A cell
// holds the next BeginConfigure()'s pool job, makes a selection while it is held, then releases
// it, to show that the configuration was resolved at the call and not on the pool thread.
// Game thread only.
struct SUPERSLMUNREAL_API FSuperSLMConfigureTestAccess
{
	// The next BeginConfigure() queues its pool job as usual; the job waits, before it builds
	// anything, until ReleasePoolJob() (or 120 s, so a cell that never releases cannot hang the pool).
	static void HoldNextPoolJob();

	// Lets the held pool job run. Without a held job it only clears a pending hold.
	static void ReleasePoolJob();
};
#endif // WITH_DEV_AUTOMATION_TESTS
