using UnrealBuildTool;

// T-2447 (D-SLM5340; the plan §9 dim 9(d), §9's M7 re-cross, §10
// L2-S0). The plugin's third module: it hosts every automation cell whose stated context is
// ClientContext -- §9 dim 9(e)'s MM-5 (§9 dim 9(d)'s MM-1..MM-4 were removed at T-2788 U0,
// fold record §4.1 dim 9(d), D-SLM7221/D-SLM7226 for MM-4) -- together with the
// test-only allocator utility those cells measure through and that utility's own oracle cell.
//
// Declared "Type": "DeveloperTool" in SuperSLMUnreal.uplugin. SuperSLMUnrealEditor is
// "Type": "Editor", which UBT and the runtime plugin manager both restrict to
// TargetType.Editor, so a packaged TargetType.Game executable never loads it and had nothing
// to run. DeveloperTool is compiled and loaded exactly when bBuildDeveloperTools holds, whose
// own default is `Type == Editor || Type == Program || (Configuration != Test && Configuration
// != Shipping)` -- the same extent as WITH_DEV_AUTOMATION_TESTS, so this module is present in
// the editor and in a packaged Development client and cannot be built into a packaged
// Shipping client by any path, rather than by a rule someone remembers.
//
// The partition rule this module's contents follow (D-SLM5340, stated so a future cell knows
// its own home without re-deciding it): a cell lives in SuperSLMUnrealEditor if it requires an
// editor-only module (UnrealEd, AssetTools) OR a file under the plugin's Source/ tree;
// otherwise it lives here. Staging copies a plugin's Content, Config and Binaries and never
// its Source/, so a cell hosted here that read a committed fixture would read nothing in a
// packaged client (D-SLM5342).
public class SuperSLMUnrealTests : ModuleRules
{
	public SuperSLMUnrealTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		// §9 dim 8 (M7 re-cross): this dependency set contains NO editor-only module, and that
		// is the property the packaged link depends on -- an UnrealEd or AssetTools edge added
		// here would compile in the editor and fail the packaged build. The runtime module
		// SuperSLMUnreal exports everything these cells consume: USuperSLMModel's accessors
		// and SuperSLM::ComputeArtifactDigest (SUPERSLMUNREAL_API), so no vendored Layer-1
		// header or include path is needed here and no second copy of vendored code is
		// compiled.
		//
		// AutomationController is deliberately NOT a dependency: Launch.Build.cs already
		// carries it and LaunchEngineLoop dynamically loads it in every non-Shipping
		// configuration, so the packaged automation framework is present without this module
		// widening its own dependency set to get it.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SuperSLMUnreal",
		});
	}
}
