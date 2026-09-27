using UnrealBuildTool;

public class ExampleProject : ModuleRules
{
	public ExampleProject(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// The example scene is authored in code: a Slate panel over the game viewport, driven by the
		// plugin's runtime module. Only the runtime module's public API is used; nothing here reads
		// the plugin's Private/ tree or the vendored SuperSLM headers.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"Json",
			"Projects", // IPluginManager: the packaged check locates the plugin's staged shader directory
			"SuperSLMUnreal",
		});
	}
}
