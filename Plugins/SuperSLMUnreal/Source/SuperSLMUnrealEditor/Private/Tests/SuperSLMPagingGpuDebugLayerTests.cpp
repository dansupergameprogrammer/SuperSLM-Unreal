#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "SuperSLMGpuTestAccess.h"

using namespace SuperSLMPagingGpuFixtures;

// Run last in the GPU paging tree, in a process started with SSLM_GPU_ENABLE_DEBUG_LAYER=1.
// Layer 1's device and InfoQueue live for the process, so this observes earlier cells too.
// A real queued warning must be read back: enabling a layer without observing it cannot pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuDebugLayerTest,
	"SuperSLM.Paging.Gpu.ZZDebugLayer.QueueAndTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)

bool FSuperSLMPagingGpuDebugLayerTest::RunTest(const FString& Parameters)
{
	if (!TestEqual(TEXT("debug layer explicitly requested"),
		FPlatformMisc::GetEnvironmentVariable(TEXT("SSLM_GPU_ENABLE_DEBUG_LAYER")), FString(TEXT("1")))) { return false; }
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 1))) { return false; }
	TArray<int32> Persona, Prompt, Tokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMGpuSequence Sequence;
	if (!MakeTokens(*this, *Rig.Model, 32, 1701, Persona)
		|| !MakeTokens(*this, *Rig.Model, 8, 1702, Prompt)
		|| !BuildPrefix(*this, *Rig.Gpu, Persona, Prefix)
		|| !Vend(*this, *Rig.Gpu, Sequence)
		|| !AdoptAndWait(*this, *Rig.Gpu, Sequence, Prefix)
		|| !GenerateComplete(*this, *Rig.Gpu, Sequence, Prompt, 4, Tokens)) { return false; }
	bool bOk = TestEqual(TEXT("real generation completes"), Tokens.Num(), 4);
	bOk &= TestTrue(TEXT("returned sequence drains"), ReturnAndDrain(*Rig.Gpu, Sequence));
	bOk &= ReleasePrefixAndDrain(*this, *Rig.Gpu, Prefix);
	FSuperSLMGpuTeardownStatuses Teardown;
	bOk &= TestTrue(TEXT("GPU teardown completes before reading its validation queue"),
		FSuperSLMGpuTestAccess::TearDownWithStatuses(*Rig.Gpu, Teardown));
	bOk &= TestTrue(TEXT("teardown recorded actual Layer-1 results"), Teardown.bRecorded);
	bOk &= TestEqual(TEXT("the Layer-1 context was destroyed"), Teardown.ContextDestroyStatus, FString(TEXT("SSLM_OK")));

	FSuperSLMGpuDebugLayerReading Healthy;
	bOk &= TestTrue(TEXT("enabled debug layer exposes a readable real InfoQueue"),
		FSuperSLMGpuTestAccess::ReadAndDrainDebugLayer(Healthy));
	bOk &= TestTrue(TEXT("layer is enabled"), Healthy.bEnabled);
	bOk &= TestTrue(TEXT("InfoQueue is available"), Healthy.bQueueAvailable);
	bOk &= TestEqual(TEXT("no D3D12 warnings, errors or corruption after work and teardown"), Healthy.ReportableMessages, int64(0));
	bOk &= TestEqual(TEXT("validation queue did not discard messages at its storage limit"), Healthy.DiscardedMessages, int64(0));

	FSuperSLMGpuDebugLayerReading Canary;
	bOk &= TestTrue(TEXT("warning canary is injected and read through the same queue"),
		FSuperSLMGpuTestAccess::ReadAndDrainDebugLayer(Canary, true));
	bOk &= TestTrue(TEXT("the real queue accepted the application warning"), Canary.bWarningInjected);
	bOk &= TestEqual(TEXT("the observer catches exactly the planted warning"), Canary.ReportableMessages, int64(1));
	FSuperSLMGpuDebugLayerReading Empty;
	bOk &= TestTrue(TEXT("drained queue remains readable"), FSuperSLMGpuTestAccess::ReadAndDrainDebugLayer(Empty));
	bOk &= TestEqual(TEXT("the canary was consumed, with no residual validation message"), Empty.ReportableMessages, int64(0));
	AddInfo(FString::Printf(TEXT("K8 D3D12 InfoQueue: enabled=%d available=%d workload_messages=%lld discarded_messages=%lld canary_messages=%lld residual_messages=%lld"),
		Healthy.bEnabled, Healthy.bQueueAvailable, Healthy.ReportableMessages, Healthy.DiscardedMessages, Canary.ReportableMessages, Empty.ReportableMessages));
	return bOk;
}

#endif
