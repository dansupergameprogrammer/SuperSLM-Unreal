#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

// The submission worker must make real progress between observations without publishing a
// different game-thread sample. Reading the live atomic counter/timer would fail this cell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuTimingSnapshotTest,
	"SuperSLM.U1.Gpu.TimingSnapshotStableUntilTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMGpuTimingSnapshotTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	FString Path, Reason;
	if (!TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
	FSuperSLMImportDiagnostic Diagnostic;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
	if (!TestTrue(TEXT("A-EX imports"), Model != nullptr && Diagnostic.bAccepted)) { return false; }
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("configure"), uint8(Gpu->Configure(Model, Config).Result), uint8(ESuperSLMGpuConfigureResult::Success))) { return false; }
	FSuperSLMGpuSequence Sequence;
	if (!TestEqual(TEXT("vend"), uint8(Gpu->VendSequence(Sequence, ESuperSLMGpuDecodePath::OneCall)), uint8(ESuperSLMGpuVendResult::Success))) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = { 1, 2, 3 };
	Request.MaxNewTokens = 2;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	if (!TestTrue(TEXT("generation requested"), Gpu->RequestBeginGeneration(Sequence, Request).IsValid())) { return false; }
	const int64 SubmittedBefore = Gpu->GetGpuDispatchCount();
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, 0.1);
	Gpu->Tick(1.0f / 60.0f);
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, 0.0);
	const FSuperSLMGpuTimingSnapshot Before = Gpu->GetGpuTimingSnapshot();
	bool bOk = TestTrue(TEXT("a real tick job is queued"), FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) > 0);
	// The synchronous probe queues behind the real prompt work and waits for its completion.
	bOk &= TestTrue(TEXT("worker finishes the queued work and probe"), Gpu->ProbeContextUsable());
	bOk &= TestTrue(TEXT("independent live counter confirms actual worker submissions"), Gpu->GetGpuDispatchCount() > SubmittedBefore);
	const FSuperSLMGpuTimingSnapshot After = Gpu->GetGpuTimingSnapshot();
	bOk &= TestEqual(TEXT("dispatch observation stays fixed between ticks"), After.Dispatches, Before.Dispatches);
	bOk &= TestEqual(TEXT("busy observation stays fixed between ticks"), After.GpuBusyMs, Before.GpuBusyMs);
	const double CompletedBusyMs = Gpu->GetLastGpuBusyMs();
	Gpu->Tick(1.0f / 60.0f);
	const FSuperSLMGpuTimingSnapshot Published = Gpu->GetGpuTimingSnapshot();
	bOk &= TestTrue(TEXT("next tick publishes the completed population"), Published.Dispatches > Before.Dispatches);
	bOk &= TestEqual(TEXT("published busy time belongs to that completed drain"), Published.GpuBusyMs, CompletedBusyMs);
	bOk &= TestTrue(TEXT("completed timing is finite and nonnegative"), FMath::IsFinite(Published.GpuBusyMs) && Published.GpuBusyMs >= 0.0);
	Gpu->ReturnSequence(Sequence);
	return bOk;
}

#endif
