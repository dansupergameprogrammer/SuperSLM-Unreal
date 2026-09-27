// T-2226 fixture -- must-REJECT for ci/check_no_forced_simd_tier.py (guard G4, §9 dim 11).
// The mutation §10 L2-S0's gate names: a planted SUPERSLM_FORCE_AVX2_MATMUL definition,
// which would pin the plugin off Layer 1's runtime CPUID dispatch and break the cross-tier
// bit-identity determinism claim the plugin inherits (D-SLM3833). This file must never
// appear in the plugin's real source tree; it exists only so the checker has a genuine
// violation to find.
using UnrealBuildTool;

public class SuperSLMUnreal : ModuleRules
{
	public SuperSLMUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PublicDefinitions.Add("SUPERSLM_FORCE_AVX2_MATMUL=1");
	}
}
