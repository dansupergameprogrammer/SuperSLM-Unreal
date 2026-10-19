#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
// Test-only faults for SuperSLM.EstimateCosts (decode-threading D4, plan §10.3.1 item 5b, the
// report contract's item 12 with D-SLM8147's BatchedPremise). Compiled only under
// WITH_DEV_AUTOMATION_TESTS. Each fault is consumed by the next run and fires in mode 1, after
// mode 1's finish hook is installed and mode 0 has completed, on the production path: the run is a
// named "incomplete estimate", suggests nothing in either mode, and writes nothing. Game thread only.
enum class ESuperSLMEstimateFault : uint8
{
	None,
	TruncateArm,          // one timed call of a mode-1 arm is dropped before the arm is checked
	InvalidAbiResult,     // a mode-1 timed ABI call's status is replaced by a refusal
	NonfiniteCost,        // a mode-1 timed call's duration is replaced by NaN
	SamplerFailure,       // the foreign-load sampler fails during mode 1's workload
	InterruptDuringMode1, // the run is interrupted between two mode-1 arms
	BatchedPremise,       // each mode-1 prompt_8 per-token median is raised above mode 1's chunk-1 intercept before the fit
};

// Actual context lengths read back from ABI saves before/after each warm-up or timed reset.
struct FSuperSLMEstimateResetObservation
{
	int64 Before = -1;
	int64 After = -1;
};

struct SUPERSLMUNREAL_API FSuperSLMEstimateTestAccess
{
	static void SetNextFault(ESuperSLMEstimateFault Fault);
	static void ClearOverrides();
	static const TArray<FSuperSLMEstimateResetObservation>& GetResetObservations();
};
#endif // WITH_DEV_AUTOMATION_TESTS
