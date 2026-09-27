// T-2448 -- pin for the T-2447 module-hosting remedy. Design record:
// the test record.
//
// This cell lives in SuperSLMUnrealEditor deliberately, and the placement IS the pin. The
// property under test is that the plugin's third module, SuperSLMUnrealTests, is loaded and its
// cells are registered with the automation framework; a cell asking that question from INSIDE
// that module cannot fail, because a module that did not load contributes no cell to fail. The
// question has to be asked from a module that loads independently.
//
// **The defect this pin is shaped against (finding R2,
// the build record §18.6).** MM-1..MM-5 were hosted in an
// "Type": "Editor" module, which a packaged TargetType.Game executable never loads, so
// `-ExecCmds="Automation RunTests SuperSLM.L2S0.MemoryMapping.*"` inside the staged executable
// matched nothing, ran nothing, and reported no failures. Nothing anywhere went red. T-2447's
// remedy moved those cells into a "DeveloperTool" module -- and the identical failure recurs
// silently if that module's entry is dropped from SuperSLMUnreal.uplugin, if its LoadingPhase
// stops loading it, or if its IMPLEMENT_MODULE export is lost: the source still declares the
// cells, the suite count quietly falls, and nothing names the loss.
//
// It also satisfies the ruling's own partition rule for living here: it requires nothing from
// the Source/ tree and nothing editor-only, but it must not be hosted by the module it is
// asking about.
//
// Mutation: remove the SuperSLMUnrealTests entry from SuperSLMUnreal.uplugin's Modules list.
// The binary stays on disk and the source stays unchanged; the module is simply never loaded,
// which is precisely the shape that today goes unnoticed.
//
// EDITOR-ONLY BY CONSTRUCTION: it can only assert what the EDITOR loaded. No hosted CI runs
// this plugin at 1.0 (D-SLM7233): an earlier packaged CI job that asserted the same property in
// a packaged client was removed at T-2788 U0.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Modules/ModuleManager.h"

namespace
{
	const TCHAR* TestsModuleName = TEXT("SuperSLMUnrealTests");

	// Every automation cell SuperSLMUnrealTests hosts, by CLASS name -- which is the key
	// FAutomationTestFramework registers under (IMPLEMENT_SIMPLE_AUTOMATION_TEST constructs its
	// instance with TEXT(#TClass), not with the pretty name).
	//
	// This list is written out rather than discovered, and it is kept honest structurally rather
	// than by anyone remembering to update it: ci/tests/test_m7_module_hosting.py's
	// test_l_hosted_cell_list_matches_the_module_s_real_cells parses this array and the module's
	// own IMPLEMENT_SIMPLE_AUTOMATION_TEST declarations and fails in BOTH directions -- a cell
	// added to the module and not listed here, and a name listed here that no longer exists.
	// MM-1..MM-4 (WindowsCookedLoadIsMapped, InlinePayloadLeftSetFailsToMap,
	// MappedPayloadFlagRemovedFailsToMap, NonWindowsCookedLoadIsNotMapped) removed at T-2788
	// U0 (plan §10.1 item 6, D-SLM7221/D-SLM7226) -- none had ever executed.
	// InstrumentDoesNotContaminateItsOwnOracle and DegenerateArtifactSizeDefeatsTheOracle
	// removed at T-2802 (O5, D-SLM7305) -- both exercised only HasAllocationAtLeast(), which no
	// retained product cell reads after U0.
	const TCHAR* HostedTestClassNames[] =
	{
		TEXT("FSuperSLMAllocationScopePeakNetOracleTest"),
		TEXT("FSuperSLMMemoryMappingRealScaleArtifactTest"),
		TEXT("FSuperSLMAllocationScopeArmResetsEveryCounterTest"),
		TEXT("FSuperSLMAllocationScopeSelfAdoptionRefusedStateIsReachableTest"),
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMUnrealTestsModuleIsLoadedAndItsCellsAreRegisteredTest,
	"SuperSLM.L2S0.Hosting.TestsModuleIsLoadedAndItsCellsAreRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMUnrealTestsModuleIsLoadedAndItsCellsAreRegisteredTest::RunTest(const FString& Parameters)
{
	// (a) The module itself. A DeveloperTool module at LoadingPhase Default is loaded by the
	// plugin manager wherever bBuildDeveloperTools holds, which includes every editor target.
	const bool bModuleLoaded = FModuleManager::Get().IsModuleLoaded(FName(TestsModuleName));
	TestTrue(FString::Printf(TEXT("the plugin's '%s' module must be loaded -- it hosts every "
		"ClientContext memory-mapping cell and the allocator instrument they measure through, "
		"and a module that is declared in source but never loaded contributes no cell that can "
		"fail (finding R2)"), TestsModuleName),
		bModuleLoaded);

	if (!bModuleLoaded)
	{
		// The registrations below cannot be present if the binary never loaded; reporting ten
		// consequential failures would bury the one cause.
		return false;
	}

	// (b) The cells. Loading the binary is what runs their static registration, so this is the
	// property the automation filter actually depends on -- and it is the one R2's defect broke.
	int32 RegisteredCount = 0;
	for (const TCHAR* TestClassName : HostedTestClassNames)
	{
		const bool bRegistered = FAutomationTestFramework::Get().ContainsTest(FString(TestClassName));
		TestTrue(FString::Printf(TEXT("'%s' must be registered with the automation framework -- "
			"an automation filter that matches no cell reports no failures, which is how the "
			"staged round's packaged run reported success while executing none of them"),
			TestClassName), bRegistered);
		RegisteredCount += bRegistered ? 1 : 0;
	}

	TestEqual(TEXT("every cell this module is expected to host must be registered"),
		RegisteredCount, static_cast<int32>(UE_ARRAY_COUNT(HostedTestClassNames)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
