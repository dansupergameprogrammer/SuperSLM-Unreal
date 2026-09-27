using UnrealBuildTool;

// SuperSLM's MCP toolset, a sibling plugin in SuperFAISSUnrealMCP's shape: disabled by default,
// with a hard dependency on the Experimental ToolsetRegistry engine plugin, which is absent from
// stock distributions. Leaving the plugin disabled is how it compiles out on a stock engine. A
// preprocessor stub is not possible: UHT forbids reflected types inside preprocessor blocks, and
// the toolset class must be reflected.
public class SuperSLMUnrealMCP : ModuleRules
{
	public SuperSLMUnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"SuperSLMUnreal",
			"SuperSLMUnrealEditor",
			"ToolsetRegistry",
			"UnrealEd",
		});

		// Read only by the automation test (Private/Tests), which creates and saves its table
		// through the asset registry. No production source uses it.
		PrivateDependencyModuleNames.Add("AssetRegistry");
	}
}
