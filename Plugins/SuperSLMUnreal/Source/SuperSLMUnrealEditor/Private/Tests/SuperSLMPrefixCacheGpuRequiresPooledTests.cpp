// The GPU prefix cache is pooled-only (plugin 1.4.0, precache U6; build record reading R39): it keeps GPU prefixes, which
// exist only with pooled sequences (SequenceBudget > 0). A dedicated Configure() after ConfigurePrefixCache() installs no
// cache, and every cache call refuses NotConfigured, naming PrefixCacheRequiresPooled. (In 1.3.0 this cell was
// SuperSLM.PrefixCache.Gpu.NotInRelease, when the GPU cache did not exist at all.)
//
//   SuperSLM.PrefixCache.Gpu.RequiresPooled   the calls refuse NotConfigured, naming PrefixCacheRequiresPooled; the stats read unconfigured
//
// Its sibling cells under SuperSLM.PrefixCache.Gpu.Instance and .Nested run the cache in pooled mode.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "SuperSLMGpuSubsystem.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuRequiresPooledTest, "SuperSLM.PrefixCache.Gpu.RequiresPooled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuRequiresPooledTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	SuperSLMSchemaJumpFixtures::FCpuRig Rig;
	if (!SuperSLMSchemaJumpFixtures::SetUpCpu(*this, W.GetTestWorld(), SuperSLMSchemaJumpFixtures::BaseConfig(1, 1), Rig)) { return false; }
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }

	FSuperSLMGpuPrefixCacheSettings Settings;
	Settings.BudgetBytes = int64(64) << 20;
	Gpu->ConfigurePrefixCache(Settings);
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 2;
	Config.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(SuperSLML2S2Fixtures::AExNumHiddenLayers);
	Config.K = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure"), (uint8)Gpu->Configure(Rig.Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }

	TArray<int32> Tokens;
	if (!TestTrue(TEXT("tokenize"), Rig.Cpu->Tokenize(SuperSLMSchemaJumpFixtures::PopulationPrompts()[0], Tokens))) { return false; }
	FSuperSLMPrefixSpec Spec;
	Spec.Tokens = Tokens;
	FSuperSLMPrefix Lease;
	FString Error;
	bool bOk = TestFalse(TEXT("RequestPrefix is refused"), Gpu->RequestPrefix(Spec, Lease, Error));
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal is NotConfigured, by name (%s)"), *Error), Error.StartsWith(TEXT("NotConfigured")));
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names PrefixCacheRequiresPooled (%s)"), *Error), Error.Contains(TEXT("PrefixCacheRequiresPooled")));
	bOk &= TestFalse(*FString::Printf(TEXT("the refusal no longer says the cache is planned for a later release (%s)"), *Error), Error.Contains(TEXT("planned for")));
	bOk &= TestFalse(TEXT("no lease is made"), Lease.IsValid());
	bOk &= TestFalse(TEXT("ReturnPrefix ends nothing"), Gpu->ReturnPrefix(Lease));
	bOk &= TestEqual(TEXT("GetPrefixSource reads Invalid"), (uint8)Gpu->GetPrefixSource(Lease), (uint8)ESuperSLMPrefixSource::Invalid);
	bOk &= TestEqual(TEXT("GetPrefixLeaseState reads Invalid"), (uint8)Gpu->GetPrefixLeaseState(Lease), (uint8)ESuperSLMPrefixLeaseState::Invalid);
	bOk &= TestFalse(TEXT("GetPrefixCacheStats reads unconfigured"), Gpu->GetPrefixCacheStats().bConfigured);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU
