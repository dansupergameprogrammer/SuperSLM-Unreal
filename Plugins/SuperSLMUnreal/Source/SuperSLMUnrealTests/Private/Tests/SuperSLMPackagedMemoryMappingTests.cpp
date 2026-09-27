// T-2226 -- L2-S0 red suite. §9 dim 9(d) (D-SLM3959), the memory-mapped cooked
// payload on Windows, plus §9 dim 9(e) (D-SLM4006), the same claim at real scale.
//
// HOSTING (T-2447, RULING D-SLM5340): every cell in this file is a ClientContext cell -- it
// makes a claim about what happens inside a PACKAGED, non-editor executable, and nothing else
// can produce the property under test. Those cells were previously hosted in
// SuperSLMUnrealEditor, which SuperSLMUnreal.uplugin declares "Type": "Editor"; UBT and the
// runtime plugin manager both restrict an Editor module to TargetType.Editor, so a packaged
// TargetType.Game executable never loaded it and `-ExecCmds="Automation RunTests
// SuperSLM.L2S0.MemoryMapping.*"` inside the staged executable had nothing to run (the staged
// round's finding R2, the build record §18.6). They live here, in
// the plugin's third module SuperSLMUnrealTests, declared "Type": "DeveloperTool" -- compiled
// and loaded exactly when bBuildDeveloperTools holds, which is the same extent as
// WITH_DEV_AUTOMATION_TESTS for a game target and additionally covers editor targets. The
// module is present in the editor, present in a packaged Development client, and cannot be
// built into a packaged Shipping client at all.
//
// The editor-path negative -- MM-0, the fresh-import-never-maps cell -- stays in
// SuperSLMUnrealEditor by the same ruling's partition rule: it needs the committed .sslm
// fixtures under the plugin's Source/ tree, it asserts a property of the editor path, and it
// already runs green there. See SuperSLMUnrealEditor/Private/Tests/SuperSLMMemoryMappingTests.cpp.
//
// NO CELL IN THIS FILE READS A FILE FROM THE PLUGIN'S Source/ TREE, and that is a requirement
// rather than a preference (RULING D-SLM5342). Staging copies a plugin's Content, Config and
// Binaries and never its Source/, so a fixture read here returns zero bytes in a packaged
// client.
//
// **Why every cell here needs a genuinely PACKAGED executable, not merely cooked content.**
// USuperSLMModel::Serialize()'s LOAD path branches on WITH_EDITOR, matching
// Engine/Private/SoundWave.cpp's own precedent exactly -- editor builds call
// ArtifactBulkData.GetCopy() (an ordinary heap copy: FBulkData::CanLoadFromDisk() requires a
// populated BulkChunkId, which only an I/O-store/pak load ever sets, never an editor-domain
// loose-file load); `!WITH_EDITOR` (packaged runtime) keeps the StealFileMapping() +
// ForceBulkDataResident() retry, the actually load-bearing memory-mapping path this suite
// cares about. **WITH_EDITOR is a COMPILE-TIME switch, not a property of which package is
// being loaded** -- loading a genuinely cooked .uasset (with its real .m.ubulk sidecar) FROM
// WITHIN THE EDITOR still takes the GetCopy() branch and IsPayloadMemoryMapped() is still
// false, because the editor build was compiled with WITH_EDITOR=1 regardless of what content
// it loads.
//
// MM-1..MM-4 (the fixture-scale packaged mapping cells) and the seeding commandlet/content
// asset they cooked from are REMOVED at T-2788 U0 (plan §10.1 item 6, fold record §4.1 dim
// 9(d), D-SLM7221/D-SLM7226 for MM-4): D-SLM7220's
// filter -- none had ever executed (no runner in this suite holds a Unreal Engine to
// cook/stage/run them) -- and the packaged-mapped-load measurement itself is consolidated into
// the one real-artifact packaged run at L2-S4 (MM-5 below) rather than kept as four separate
// fixture-scale cells at L2-S0.
//
// MM-5 (D-SLM4006, §9 dim 9(e)): the memory-mapping PRODUCT claim at REAL SCALE, the
// resolving-power cell fixture scale is below the noise floor to discriminate. Opt-in on one
// externally-configured real-artifact mapped package path (env var below), packaged
// Development on Windows only, measuring (a) IsPayloadMemoryMapped() true and (b) peak net
// heap allocated across the load as a small stated fraction of the artifact's own byte count.
// The forced-copy control arm (a same-machine same-artifact wall-clock comparison) is REMOVED
// at T-2788 U0 (plan §10.1 item 6, D-SLM7221): filter, an instrument-commissioning construction
// no shipped surface reports a claim against. Development, not Shipping (RULING D-SLM5341):
// WITH_DEV_AUTOMATION_TESTS is 0 in Shipping so an automation cell's body does not compile
// there, and AutomationController -- the module servicing `-ExecCmds="Automation RunTests …"`
// -- is loaded only under !UE_BUILD_SHIPPING, so nothing would run one if it had. Development
// costs the memory-mapping claim nothing: WITH_EDITOR is 1 only for TargetType.Editor and
// TargetType.Program, so a TargetType.Game build takes Serialize()'s packaged
// StealFileMapping() branch in every configuration. No CI runner holds a gigabyte-scale
// artifact, so this cell is LOUDLY, NAMEDLY skipped (AddWarning, never a silent pass) when its
// env var is unset -- the same established pattern as Layer 1's forced-AVX-512 axis
// (real-silicon manual run recorded separately). Per D-SLM4006, re-sequenced by D-SLM7221:
// authored at L2-S0, does not gate L2-S0 (no real artifact exists in this tree to gate
// against), and a recorded manual run is owed before L2-S4 closes -- it gates L2-S4 (the
// artifact the example actually ships), not L2-S1.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMModel.h"
#include "SuperSLMIntegrity.h"
#include "SuperSLMAllocationScope.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// MM-5 (D-SLM4006): the opt-in env var naming a REAL, externally-cooked package to
	// LoadPackage() -- never a fixture. Must be set (non-empty) for MM-5 to run its real
	// assertions; unset is this cell's own loud, named skip (see MM-5's RunTest body). The
	// paired forced-copy env var is removed at T-2788 U0 (see file header).
	const TCHAR* RealScaleMappedPackageEnvVar = TEXT("SUPERSLM_L2S0_REALSCALE_MAPPED_PACKAGE");

	// Shared by MM-5: LoadPackage() an arbitrary, externally-named package (the real-scale
	// mapped package), returning the live USuperSLMModel or nullptr with a loud failure.
	USuperSLMModel* LoadNamedPackageModel(FAutomationTestBase& Test, const FString& PackageName)
	{
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		if (!Test.TestNotNull(*FString::Printf(TEXT("LoadPackage must find the configured "
			"real-scale package '%s' -- this is a manual-run configuration error, not a "
			"plugin defect, if it fails against a genuinely staged executable"), *PackageName),
			Package))
		{
			return nullptr;
		}
		USuperSLMModel* Model = FindObject<USuperSLMModel>(Package, *FPaths::GetBaseFilename(PackageName));
		Test.TestNotNull(*FString::Printf(TEXT("'%s' must contain a USuperSLMModel named after "
			"its own package"), *PackageName), Model);
		return Model;
	}
}

// Pin for the implementation's T-2256 round-11 remedy landing FSuperSLMAllocationScope::PeakNetAllocatedBytes()
// (D-SLM4006, §9 dim 9(e)) -- the mechanism cell MM-5's real-scale run stands on, executed here
// at toy scale where it can run every time (MM-5 itself runs only against real content, opt-in).
// Two properties are proven, one of them a MUST-REJECT against a specific wrong implementation:
// subtracting a freed PRE-scope block's size (knowable via the inner allocator's
// GetAllocationSize) would credit engine churn against this scope and drive the observed peak
// DOWN -- the one direction of error that weakens an upper-bound oracle. This cell allocates
// 16 MB before the scope, 1 MB inside it, frees the pre-scope block mid-scope, then allocates
// another 1 MB inside it: a correct instrument reports a peak of ~2 MB; the rejected design
// plateaus at ~1 MB. The 0.5 MB margin dwarfs anything a background thread can allocate through
// GMalloc inside this window, so the discrimination does not depend on scheduler quietness.
//
// Runs in BOTH contexts (T-2447, §9's M7 re-cross dimension 3): an instrument commissioned only
// where it is not used is not commissioned, and the allocator proxy
// replaces the process allocator in the packaged Development client as well as in the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMAllocationScopePeakNetOracleTest,
	"SuperSLM.L2S0.MemoryMapping.AllocationScopePeakNetOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMAllocationScopePeakNetOracleTest::RunTest(const FString& Parameters)
{
	constexpr SIZE_T PreScopeBlockSize = 16u * 1024u * 1024u;
	constexpr SIZE_T InScopeBlockSize = 1024u * 1024u;
	constexpr int64 DiscriminatingFloor = static_cast<int64>(InScopeBlockSize) +
		static_cast<int64>(InScopeBlockSize) / 2;

	void* PreScopeBlock = FMemory::Malloc(PreScopeBlockSize);
	if (!TestNotNull(TEXT("the pre-scope allocation must succeed"), PreScopeBlock))
	{
		return false;
	}

	FSuperSLMAllocationScope AllocationScope;

	void* FirstInScopeBlock = FMemory::Malloc(InScopeBlockSize);
	if (!TestNotNull(TEXT("the first in-scope allocation must succeed"), FirstInScopeBlock))
	{
		return false;
	}
	TestTrue(TEXT("after a 1 MB in-scope allocation, the peak net must already be at least "
		"1 MB -- the must-accept half: the instrument moves"),
		AllocationScope.PeakNetAllocatedBytes() >= static_cast<int64>(InScopeBlockSize));

	// THE discriminating step: a pre-scope block freed WHILE the scope is live.
	FMemory::Free(PreScopeBlock);

	void* SecondInScopeBlock = FMemory::Malloc(InScopeBlockSize);
	if (!TestNotNull(TEXT("the second in-scope allocation must succeed"), SecondInScopeBlock))
	{
		return false;
	}

	TestTrue(FString::Printf(TEXT("after two 1 MB in-scope allocations with a 16 MB PRE-scope "
		"block freed in between, the peak must be at least %lld bytes (~2 MB). A peak near "
		"1 MB means pre-scope frees were wrongly subtracted against this scope -- the exact "
		"defect this pin exists to reject"), DiscriminatingFloor),
		AllocationScope.PeakNetAllocatedBytes() >= DiscriminatingFloor);

	TestTrue(TEXT("HasAllocationAtLeast() must stay live under the new plumbing -- a 1 MB "
		"single call was observed, so the largest-single-call oracle must answer true at "
		"that threshold"),
		AllocationScope.HasAllocationAtLeast(InScopeBlockSize));

	FMemory::Free(FirstInScopeBlock);
	FMemory::Free(SecondInScopeBlock);

	return true;
}

// MM-5 (D-SLM4006, §9 dim 9(e)): the real-scale product claim. FEAT oracle when its one
// env var is configured; a loud, named, logged skip (never a silent pass, never a hard
// fail) when it is not -- no CI runner in this suite has ever held a gigabyte-scale
// artifact (same stated ceiling as every other cook-only cell in this file). A recorded
// manual run against real content is owed before L2-S4 closes, not L2-S1 (D-SLM4006's
// original sequencing, re-sequenced by D-SLM7221).
// The forced-copy control arm is removed at T-2788 U0 (file header, D-SLM7221).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMMemoryMappingRealScaleArtifactTest,
	"SuperSLM.L2S0.MemoryMapping.RealScaleArtifactOptIn",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMMemoryMappingRealScaleArtifactTest::RunTest(const FString& Parameters)
{
	const FString MappedPackageName = FPlatformMisc::GetEnvironmentVariable(RealScaleMappedPackageEnvVar);

	if (MappedPackageName.IsEmpty())
	{
		AddWarning(FString::Printf(TEXT("MM-5 (D-SLM4006) SKIPPED: opt-in real-scale artifact "
			"env var not set (%s). This is the stated, loud skip this cell is designed "
			"to take in every environment that does not hold a gigabyte-scale test artifact -- "
			"NOT a silent pass. A recorded manual run against real content is owed before "
			"L2-S4 closes (D-SLM4006, re-sequenced by D-SLM7221)."), RealScaleMappedPackageEnvVar));
		return true;
	}

	FSuperSLMAllocationScope AllocationScope;
	const double MappedStartSeconds = FPlatformTime::Seconds();
	USuperSLMModel* MappedModel = LoadNamedPackageModel(*this, MappedPackageName);
	const double MappedElapsedSeconds = FPlatformTime::Seconds() - MappedStartSeconds;
	if (!MappedModel)
	{
		return false;
	}

	const int64 ArtifactByteCount = MappedModel->GetMappedArtifactSize();
	if (!TestTrue(FString::Printf(TEXT("the loaded model must report a strictly positive "
		"artifact size before that size can serve as the heap-copy comparison's own "
		"denominator -- observed %lld bytes. A non-positive size makes the peak-fraction "
		"check below default to 0.0, which is unconditionally under the 5%% bound and "
		"discriminates nothing (T-2799 M5)"),
		ArtifactByteCount), ArtifactByteCount > 0))
	{
		return false;
	}

	uint8 IntegrityHash[32] = { 0 };
	if (const void* Data = MappedModel->GetMappedArtifactData())
	{
		SuperSLM::ComputeArtifactDigest(static_cast<const uint8*>(Data), static_cast<int64>(ArtifactByteCount), IntegrityHash);
	}
	FString IntegrityHashHex;
	for (uint8 Byte : IntegrityHash)
	{
		IntegrityHashHex += FString::Printf(TEXT("%02x"), Byte);
	}

	TestTrue(TEXT("a real-scale cooked Windows load must report IsPayloadMemoryMapped() true "
		"-- the product claim restated at the scale fixture-scale mechanism checks cannot "
		"discriminate"),
		MappedModel->IsPayloadMemoryMapped());

	// Peak net heap allocated (Malloc/Realloc minus Free, high-water mark) must stay a small
	// fraction of the artifact's own byte count -- a mapped view holds bookkeeping only, not
	// a heap-resident copy of the payload. 5% is a stated, generous bound (headroom for
	// legitimate non-payload package-load allocation), not a tight regression trap. The
	// strict-positivity gate above guarantees ArtifactByteCount > 0 here, so this division
	// never degenerates to the vacuous 0.0/false-pass the gate exists to rule out.
	const int64 PeakNetAllocatedBytes = AllocationScope.PeakNetAllocatedBytes();
	const double PeakFractionOfArtifact =
		static_cast<double>(PeakNetAllocatedBytes) / static_cast<double>(ArtifactByteCount);
	TestTrue(FString::Printf(TEXT("peak net heap allocated across a real-scale mapped load "
		"(%lld bytes) must stay under 5%% of the artifact's own byte count (%lld bytes, "
		"fraction observed: %f) -- a mapped view is not a heap copy, restated where fixture "
		"scale is below the noise floor to prove it"),
		PeakNetAllocatedBytes, ArtifactByteCount, PeakFractionOfArtifact),
		PeakFractionOfArtifact < 0.05);

	AddInfo(FString::Printf(TEXT("MM-5 real-scale report: artifact=%s bytes=%lld sha256=%s "
		"mapped_load_seconds=%f peak_net_allocated_bytes=%lld peak_fraction_of_artifact=%f"),
		*MappedPackageName, ArtifactByteCount, *IntegrityHashHex, MappedElapsedSeconds,
		PeakNetAllocatedBytes, PeakFractionOfArtifact));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
