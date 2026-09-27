// A GPU call that waits on the submission queue (RunSync: Configure(), adapter map and unmap, the
// probe) waits for as long as the jobs ahead of it take to finish. Queue time is progress, not a
// hang: only a Layer-1 call that overruns its own bound, timed from the moment it takes the lock,
// may end the wait and take the device-loss path. A whole-wait deadline measured from the call's
// enqueue would turn a slow but healthy queue into a lost device.
//
// RunSyncWaitsOutSlowQueuedTickJob: one sequence is generating, and each tick job is slowed by
// the test-access submission delay. The delay is slept after the job's Layer-1 work, outside the
// lock and the holder record, so no Layer-1 call is ever over its bound. One tick queues a tick
// job carrying a delay of kSubmissionDelaySeconds; ProbeContextUsable() is then queued behind
// it, so its wait lasts at least that long, which is well over the 10 s frame-path bound. It must:
//   - return true (the context is usable, and the wait ended because the job finished);
//   - have waited longer than the frame-path bound (the precondition that the cell tests
//     anything: a shorter wait would pass under a whole-wait deadline too);
//   - leave the backend active.
// A whole-wait deadline of 10 s ends the wait before the job finishes, marks the submission
// thread unresponsive and returns false, so every one of these fails, and the error it logs
// fails the cell as well.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// The frame-path bound a Layer-1 call is timed against (the product's kFramePathBoundSeconds).
	constexpr double kFramePathBoundSecondsForCell = 10.0;
	// The delay each tick job carries: half the bound again, so the probe's wait is well past the
	// bound even after the time between the tick and the probe is taken off it.
	constexpr double kSubmissionDelaySeconds = 15.0;
	// The probe's wait must be at least this long for the cell to test anything.
	constexpr double kMinProbeWaitSeconds = kFramePathBoundSecondsForCell + 1.0;
	constexpr double kDrainDeadlineSeconds = 60.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuRunSyncWaitsOutSlowQueuedTickJobTest,
	"SuperSLM.U1.Gpu.RunSyncWaitsOutSlowQueuedTickJob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRunSyncWaitsOutSlowQueuedTickJobTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // one-call path
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	const FSuperSLMLifecycleOpHandle Begin = Gpu->RequestBeginGeneration(Seq, Request);
	bool bOk = TestTrue(*FString::Printf(TEXT("the generation is requested (%s)"), *Gpu->GetLastLifecycleRequestError()), Begin.IsValid());

	// One tick: it admits the generation if the request did not, and queues a tick job carrying
	// the delay for its prompt.
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, kSubmissionDelaySeconds);
	Gpu->Tick(1.0f / 60.0f);
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, 0.0); // the job keeps the delay it was created with
	bOk &= TestTrue(TEXT("precondition: the slowed tick queued a tick job, still outstanding"), FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) >= 1);

	// The probe queues behind that job and waits for it.
	bool bProbeUsable = false;
	double ProbeWaitSeconds = 0.0;
	if (bOk)
	{
		const double ProbeStart = FPlatformTime::Seconds();
		bProbeUsable = Gpu->ProbeContextUsable();
		ProbeWaitSeconds = FPlatformTime::Seconds() - ProbeStart;
	}
	AddInfo(FString::Printf(TEXT("the probe waited %.3f s behind a tick job delayed %.1f s"), ProbeWaitSeconds, kSubmissionDelaySeconds));
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: the probe waited longer than the %.0f s frame-path bound (%.3f s)"),
		kFramePathBoundSecondsForCell, ProbeWaitSeconds), ProbeWaitSeconds >= kMinProbeWaitSeconds);
	bOk &= TestTrue(TEXT("a call queued behind a slow but healthy job returns true: the wait ends when the job finishes"), bProbeUsable);
	bOk &= TestTrue(TEXT("the backend is still active after the wait"), Gpu->IsGpuBackendActive());

	// Drain with no delay, then a tick: still active, so no device-loss path was latched.
	const double DrainStart = FPlatformTime::Seconds();
	while (FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) + FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(*Gpu) > 0
		&& FPlatformTime::Seconds() - DrainStart < kDrainDeadlineSeconds)
	{
		TickWhenDeviceReady(*Gpu);
	}
	TickWhenDeviceReady(*Gpu);
	bOk &= TestTrue(TEXT("the backend is still active on the tick after the wait"), Gpu->IsGpuBackendActive());
	Gpu->ReturnSequence(Seq);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
