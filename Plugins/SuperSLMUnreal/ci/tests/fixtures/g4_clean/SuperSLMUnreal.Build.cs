// T-2226 fixture -- must-ACCEPT for ci/check_no_forced_simd_tier.py (guard G4, §9 dim 11).
// No SUPERSLM_FORCE_* public/private definition -- Layer 1's runtime CPUID dispatch among
// SSE2/AVX2/AVX-512 is left alone, which is the property the plugin's cross-tier
// determinism claim depends on (D-SLM3833).
using UnrealBuildTool;

public class SuperSLMUnreal : ModuleRules
{
	public SuperSLMUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
