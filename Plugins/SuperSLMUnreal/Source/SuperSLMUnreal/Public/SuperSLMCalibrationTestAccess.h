#pragma once

#include "CoreMinimal.h"
#include "SuperSLMCalibrateCostsCommand.h"

// Plan §10.3.1 item 5b (D-SLM7778): the calibration command's idleness evidence, as plain values.
// The command fills one FSuperSLMCalibrationIdlenessReport per run. Production code reads it only
// to log it; tests read it through FSuperSLMCalibrationTestAccess (below).

// The three phases of the one sample series (item 5b "Sampling: one series across the whole run").
enum class ESuperSLMCalibrationPhase : uint8
{
	PreWindow,  // the 2 s before the workload, nothing of it running
	Workload,   // the whole nine-cost measurement (or, under a test, its synthetic substitute)
	PostWindow, // the 2 s after the workload ends
};

// The points at which the command calls a test's phase hook (FSuperSLMCalibrationTestAccess).
enum class ESuperSLMCalibrationEvent : uint8
{
	BeforePreWindow, // before the sampler starts: nothing of the series has been sampled yet
	WorkloadStart,   // the phase has just become Workload and its log line is written; the workload has not begun
	WorkloadEnd,     // the workload has returned, the phase has just become PostWindow, and its log line is written
	AfterPostWindow, // the sampler has stopped after the post-window's last sample; the verdict is not yet logged
};

// What the idleness criterion concluded.
enum class ESuperSLMIdlenessVerdict : uint8
{
	NotGraded,          // the command did not reach grading (no artifact argument)
	Met,                // no window above the ceiling, and no foreign build lock at any sample
	RefusedForeignLoad, // some four-sample window's mean foreign share exceeded the ceiling
	RefusedForeignLock, // the build lock was held by a foreign owner at some sample (checked first; D-SLM7788)
	SamplerUnavailable, // the platform has no GetSystemTimes/GetProcessTimes reading: refused
};

// Plan §10.3.1 item 5b's build-lock states (D-SLM7788), classified at every sample.
enum class ESuperSLMBuildLockState : uint8
{
	Absent,  // nothing exists at the build lock's path, file or directory
	Own,     // a directory; SSU_BUILD_LOCK_TOKEN is non-empty in this process; owner.txt's first line equals it
	Foreign, // anything else: a file at the path, no marker, an unreadable marker, no token, or a different token
	NotConfigured, // SUPERSLM_BUILD_LOCK is unset: no lock is checked, and the lock never refuses
};

// One 250 ms sample. Shares are percent of the box's total capacity over the sample (every
// logical processor), from GetSystemTimes for the box and GetProcessTimes for this process.
struct SUPERSLMUNREAL_API FSuperSLMIdlenessSample
{
	double EndSeconds = 0.0;    // the sample's end, seconds since the sampler started
	ESuperSLMCalibrationPhase Phase = ESuperSLMCalibrationPhase::PreWindow; // the phase at the sample's end
	double BoxSharePercent = 0.0;     // (kernel + user - idle) / (kernel + user)
	double OwnSharePercent = 0.0;     // this process's kernel + user time / capacity
	double ForeignSharePercent = 0.0; // (box busy - own) / capacity, a negative difference read as 0
	ESuperSLMBuildLockState LockState = ESuperSLMBuildLockState::Absent; // classified when the sample was taken
	FString LockForeignReason;        // why the lock read Foreign; empty otherwise
};

struct SUPERSLMUNREAL_API FSuperSLMCalibrationIdlenessReport
{
	ESuperSLMIdlenessVerdict Verdict = ESuperSLMIdlenessVerdict::NotGraded;
	int32 LogicalProcessors = 0;    // N: GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) at run time (D-SLM7793, T-3001 F4)
	int32 UeLogicalProcessors = 0;  // FPlatformMisc::NumberOfCoresIncludingHyperthreads(), which -corelimit caps; logged when it differs
	double CeilingPercent = 0.0;    // 50 / LogicalProcessors
	TArray<FSuperSLMIdlenessSample> Samples;
	// The first refusing four-sample window: the index of its first sample, and the phases of its
	// first and last samples. -1 when no window refused on foreign share.
	int32 FirstRefusingWindow = -1;
	ESuperSLMCalibrationPhase FirstRefusingWindowFirstPhase = ESuperSLMCalibrationPhase::PreWindow;
	ESuperSLMCalibrationPhase FirstRefusingWindowLastPhase = ESuperSLMCalibrationPhase::PreWindow;
	double FirstRefusingWindowMeanPercent = 0.0;
	// The first sample at which the build lock read Foreign, or -1 (an Own lock never refuses).
	int32 FirstForeignLockSample = -1;
	// Whether this process had a non-empty SSU_BUILD_LOCK_TOKEN when the run began.
	bool bHadLockToken = false;
	// Whether SUPERSLM_BUILD_LOCK named a lock path when the run began (false: "build-lock check not configured").
	bool bLockCheckConfigured = false;
	bool bSyntheticWorkload = false; // the workload was a test's substitute (nothing is ever written)
	bool bSamplerDisabled = false;   // the test seam disabled the sampler (item 5a.2's condition S): no verdict, nothing written
	bool bWorkloadSucceeded = false; // the workload (real or synthetic) reported success
};

#if WITH_DEV_AUTOMATION_TESTS
// Test-only access to the calibration command's idleness instrument (plan §10.3.1 items 5b and 6b,
// D-SLM7778: the four commissioning constructions). Compiled only under WITH_DEV_AUTOMATION_TESTS;
// it never changes the criterion, the ceiling or the sampling. Each setting is consumed by the
// next run of the command and cleared by ClearOverrides(). Game thread only.
struct SUPERSLMUNREAL_API FSuperSLMCalibrationTestAccess
{
	// (a) Called synchronously on the command's thread at each ESuperSLMCalibrationEvent, so a
	// separate-process load can be started or stopped at a phase boundary. The hook's own time is
	// part of the phase it runs in.
	static void SetPhaseHook(TFunction<void(ESuperSLMCalibrationEvent)> Hook);

	// (c) Replaces the nine-cost measurement with Workload for the next run. It runs in the
	// Workload phase exactly where the measurement would, and returns success (OutError on
	// failure). A synthetic run is graded by the same criterion, and never writes: it ends
	// RefusedNotIdle when refused, MeasurementFailed when the workload failed, and otherwise leaves
	// GetLastOutcome() at NotRun with the report's Verdict reading Met. Starting or stopping a load
	// inside the workload is done by the substitute itself.
	static void SetSyntheticWorkload(TFunction<bool(FString& OutError)> Workload);

	// Plan §10.3.1 item 5a.2's condition S (D-SLM7793): the next run takes no samples. Phases,
	// windows and the workload are unchanged; the command logs its eleven values labelled
	// "sampler disabled: no idleness verdict", writes nothing, and leaves GetLastOutcome() at NotRun
	// (MeasurementFailed if the measurement failed). The report's Verdict reads NotGraded.
	static void SetSamplerDisabled(bool bDisabled);

	// Plan §10.3.1 item 5a.2's condition R, the must-reject (D-SLM7793, D-SLM7794): the runs that
	// follow install the finish hook at FinishParallelTasks instead of FSuperSLMRuntimeConfig's
	// shipped default (0 or 1 installs nothing, so the finish runs serially). A value outside
	// [0, SSLM_PARALLEL_FOR_MAX_TASKS] fails the measurement by name. The values line carries the
	// effective value as FinishParallelTasks=<n>. Cleared by ClearOverrides(); a production run
	// always uses the default.
	static void SetFinishParallelTasksOverride(int32 FinishParallelTasks);

	// Clears every override.
	static void ClearOverrides();

	// (b) The most recent run's idleness report: outcome is SuperSLMCalibrateCostsCommand::
	// GetLastOutcome(); verdict, ceiling, first refusing window (index and phases) and the
	// per-sample series are here.
	static const FSuperSLMCalibrationIdlenessReport& GetLastIdlenessReport();
};
#endif // WITH_DEV_AUTOMATION_TESTS
