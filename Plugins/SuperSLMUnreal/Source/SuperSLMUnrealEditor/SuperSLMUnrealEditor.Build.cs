using System.IO;
using UnrealBuildTool;

// L2-S0 (the plan §10). The editor module: the import factory, the
// Content Browser asset identity, and the automation cells under Private/Tests that require an
// editor-only module (UnrealEd, AssetTools) or a file under the plugin's Source/ tree. (The
// MMapReference seeding commandlet this module once hosted was deleted at T-2788 U0, plan
// §10.1 item 6, D-SLM7221/D-SLM7226, alongside the MM-1..4 cells it fed.)
//
// It is not the plugin's only test host. T-2447 (RULING D-SLM5340) added a third module,
// SuperSLMUnrealTests, declared "Type": "DeveloperTool", which hosts every cell whose stated
// context is ClientContext — a packaged TargetType.Game executable never loads an
// "Type": "Editor" module, so those cells had no host in the artifact they make a claim
// about. The partition rule, so a future cell knows its own home without re-deciding it: a
// cell lives HERE if it requires an editor-only module or a file under the plugin's Source/
// tree; otherwise it lives in SuperSLMUnrealTests.
public class SuperSLMUnrealEditor : ModuleRules
{
	public SuperSLMUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		// Layer 1 forbids implicit FP contraction: Layer 1's own scalar/kernel-tier
		// bit-equality depends on exact mul-then-add rounding (SuperSLM's own design, §6/§13).
		// This module never compiles Layer-1 kernels itself, but the CookAlignment test
		// links against the vendored artifact.h/model.h view types, so the same precise-FP
		// contract is stated here rather than assumed inherited from the runtime module.
		FPSemantics = FPSemanticsMode.Precise;

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "SuperSLM", "include"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AssetTools",
			"SuperSLMUnreal",
			"Slate",     // L2-S3 build: SSuperSLMQueryWindow.h / SSuperSLMInspectionPanel.h are Slate widgets
			"SlateCore", // (the red suite's record §5 named these for the query window's Slate view)
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects", // IPluginManager, to resolve Tests/Fixtures/SSLM/*.sslm by plugin-relative path
			"Json",     // T-2805 (L2-S1 red suite): SuperSLML2S1Fixtures.h parses the committed
			            // Tests/Fixtures/L2S1/l2s1_r_s1a_reference.json extract of T-2783's own
			            // frozen prompts and Layer-1-recorded outputs (D-SLM7229).
			"TraceLog", // T-2805 (L2-S1 red suite, T-2815 build log D4): SuperSLML2S1CpuDeterminismTests.cpp's
			            // TraceOn arm calls UE::Trace::ToggleChannel. UBT links a module only
			            // against its DIRECT dependencies -- Core depends on TraceLog publicly,
			            // but that does not put ToggleChannel on this module's own link line
			            // (measured: LNK2019, T-2815 build log). Adding it to the RUNTIME module
			            // instead does not help either (measured, then reverted, same log) --
			            // this Editor module is the one that calls it, so this is where it goes.
			"TraceServices",  // T-2816 round 8 (D-SLM7502): SuperSLML2S2InsightsTests.cpp's rebuilt
			                  // oracle reads a captured .utrace through
			                  // TraceServices::IAnalysisService/ITimingProfilerProvider/
			                  // ICounterProvider/IBookmarkProvider (real trace analysis),
			                  // replacing the withdrawn raw-byte scan whole.
			"TraceAnalysis",  // TraceServices' own public dependency (TraceServices.Build.cs).
			                  // UBT links a module only against its DIRECT dependencies -- the
			                  // same reason TraceLog is listed explicitly above rather than relied
			                  // on transitively through Core.
			"InputCore",              // L2-S3 build: the Slate widgets' input types (SButton, SSpinBox).
			"PropertyEditor",         // L2-S3 build: SObjectPropertyEntryBox, the query window's model picker.
			"WorkspaceMenuStructure", // L2-S3 build: the query window tab's menu group.
			"RHI",                    // L2-S3: R-S3f's null-RHI refusal reads GUsingNullRHI (GRHIGlobals).
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("AutomationController");
		}
	}
}
