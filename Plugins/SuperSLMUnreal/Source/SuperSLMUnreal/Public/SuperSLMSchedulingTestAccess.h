#pragma once

#include "CoreMinimal.h"

class USuperSLMSubsystem;

#if WITH_DEV_AUTOMATION_TESTS
#include "superslm/sslm_abi.h" // sslm_status, for SetNextResetStatusOverride()

// Test-only access to the CPU scheduler's numeric domain (plan §5 "The scheduler's numeric
// domain", plan §10.3.1 item 5a.3, D-SLM7799). Compiled only under WITH_DEV_AUTOMATION_TESTS; it
// never changes the guards, the limit or the horizon. Game thread only.
struct SUPERSLMUNREAL_API FSuperSLMSchedulingTestAccess
{
	// kMaxJobTicks: 2^20, the largest K Configure() admits and TryComputeK() returns.
	static int32 MaxJobTicks();

	// kTickHorizon: INT32_MAX - kMaxJobTicks. No job is planned at or past this tick.
	static int32 TickHorizon();

	// The production guards themselves (cell CheckedKAndTick). TryComputeK fails for a non-finite
	// or negative planned cost, a non-finite or non-positive budget, and ceil(cost / budget) >
	// kMaxJobTicks; otherwise OutK = max(1, ceil(cost / budget)). TryCommitTick fails when
	// TickIndex + K > INT32_MAX (or either is negative); otherwise OutTick = TickIndex + K. On
	// failure the out value is left unchanged.
	static bool TryComputeK(double PlannedJobMs, double TickBudgetMs, int32& OutK);
	static bool TryCommitTick(int32 TickIndex, int32 K, int32& OutTick);

	// Sets the configured subsystem's tick counter (cell TickHorizonFaults: horizon - 2). Returns
	// false when the subsystem is not configured. Jobs already posted keep their ticks. It does not
	// clear a horizon already reached; Configure() does.
	static bool SetTickCounter(USuperSLMSubsystem& Subsystem, int32 TickCounter);

	// The configured subsystem's tick counter (the index the next Tick() reports), or -1 when not
	// configured.
	static int32 GetTickCounter(const USuperSLMSubsystem& Subsystem);

	// The number of rows the job ledger and the tick history keep (USuperSLMSubsystem::
	// GetJobLedger(), GetTickHistory()), applied at the next Configure(). A test that reads a whole
	// run's history sets enough rows for the run before it configures; 0 restores the shipping
	// capacity. The rings run the same code at either size. Process-wide: restore it when done,
	// which FScopedReportHistoryCapacity does.
	static void SetReportHistoryCapacity(int32 Rows);
	static int32 GetReportHistoryCapacity();

	// The rows a whole-run test reads: far more than any cell here produces.
	static constexpr int32 WholeRunHistoryRows = 1 << 16;

	struct FScopedReportHistoryCapacity
	{
		explicit FScopedReportHistoryCapacity(int32 Rows = WholeRunHistoryRows) { SetReportHistoryCapacity(Rows); }
		~FScopedReportHistoryCapacity() { SetReportHistoryCapacity(0); }
		FScopedReportHistoryCapacity(const FScopedReportHistoryCapacity&) = delete;
		FScopedReportHistoryCapacity& operator=(const FScopedReportHistoryCapacity&) = delete;
	};

	// A one-shot Layer-1 status override for the next reset job, the CPU counterpart of
	// FSuperSLMGpuTestAccess::SetNextResetStatusOverride(). The next reset job the configured
	// subsystem dispatches -- of any kind: a caller's ResetSequence(), or the recycle a
	// ReturnSequence() or a refused restore queues -- returns Status instead of calling
	// sslm_seq_reset (or sslm_seq_create, on the path that recreates a sequence a failed restore
	// released), and the sequence is left as Layer 1 had it. The override is taken when that job is
	// dispatched; SSLM_OK clears it, and unset it changes nothing. A later Configure() clears it.
	// No-op when the subsystem is not configured. Game thread.
	static void SetNextResetStatusOverride(USuperSLMSubsystem& Subsystem, sslm_status Status);
};
#endif // WITH_DEV_AUTOMATION_TESTS
