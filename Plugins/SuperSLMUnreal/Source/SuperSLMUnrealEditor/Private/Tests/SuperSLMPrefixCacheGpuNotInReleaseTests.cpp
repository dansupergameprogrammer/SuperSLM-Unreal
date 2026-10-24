// The GPU prefix cache in plugin 1.3.0 (review round 2, item d): USuperSLMGpuSubsystem declares the cache's calls for
// 1.4.0, and in 1.3.0 every one of them refuses NotConfigured, by name, even after ConfigurePrefixCache() and a GPU
// Configure(). The documented behaviour (CHANGELOG 1.3.0 Known issues, ARCHITECTURE.md "On the GPU").
//
//   SuperSLM.PrefixCache.Gpu.NotInRelease   the calls refuse NotConfigured and name 1.4.0; the stats read unconfigured
//
// Its sibling cells under SuperSLM.PrefixCache.Gpu.Instance and .Nested are the U6 twins owed at plugin 1.4.0; they
// are red in 1.3.0 by design.

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNotInReleaseTest, "SuperSLM.PrefixCache.Gpu.NotInRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNotInReleaseTest::RunTest(const FString& Parameters)
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
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal says the GPU prefix cache is not in this release, and names 1.4.0 (%s)"), *Error),
		Error.Contains(TEXT("1.4.0")));
	bOk &= TestFalse(TEXT("no lease is made"), Lease.IsValid());
	bOk &= TestFalse(TEXT("ReturnPrefix ends nothing"), Gpu->ReturnPrefix(Lease));
	bOk &= TestEqual(TEXT("GetPrefixSource reads Invalid"), (uint8)Gpu->GetPrefixSource(Lease), (uint8)ESuperSLMPrefixSource::Invalid);
	bOk &= TestEqual(TEXT("GetPrefixLeaseState reads Invalid"), (uint8)Gpu->GetPrefixLeaseState(Lease), (uint8)ESuperSLMPrefixLeaseState::Invalid);
	bOk &= TestFalse(TEXT("GetPrefixCacheStats reads unconfigured"), Gpu->GetPrefixCacheStats().bConfigured);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU
