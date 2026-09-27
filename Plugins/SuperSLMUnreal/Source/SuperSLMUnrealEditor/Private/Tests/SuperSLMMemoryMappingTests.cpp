// T-2226 -- L2-S0 red suite. §9 dim 9(d) (D-SLM3959), the editor half of the
// memory-mapping claim.
//
// THIS FILE HOLDS ONE CELL: MM-0, the EDITOR-PATH NEGATIVE. The four cooked-load cells
// (MM-1..MM-4) lived in the plugin's third module, SuperSLMUnrealTests, and were removed at
// T-2788 U0 (fold record §4.1 dim 9(d), D-SLM7221/D-SLM7226 for MM-4). The real-scale cell
// (MM-5) still lives there -- see
// Source/SuperSLMUnrealTests/Private/Tests/SuperSLMPackagedMemoryMappingTests.cpp.
//
// **Why the split (T-2447, RULING D-SLM5340).** Those cells are ClientContext cells: each
// makes a claim about what happens inside a packaged, non-editor executable. This module
// is declared "Type": "Editor" in SuperSLMUnreal.uplugin, which UBT and the runtime plugin
// manager both restrict to TargetType.Editor, so a packaged TargetType.Game executable never
// loaded it and the automation run inside the staged executable had nothing to execute (the
// staged round's finding R2, the build record §18.6). The ruling's
// partition rule: a cell lives HERE if it requires an editor-only module (UnrealEd,
// AssetTools) or a file under the plugin's Source/ tree; otherwise it lives in
// SuperSLMUnrealTests. MM-0 satisfies the rule on both counts -- it reads the committed .sslm
// fixture and it asserts a property of the editor import path -- and it already runs green
// here.
//
// **The production fix the implementation landed while retargeting these cells** (round 5,
// the build record §14): USuperSLMModel::Serialize()'s LOAD path
// branches on WITH_EDITOR, matching Engine/Private/SoundWave.cpp's own precedent exactly --
// editor builds call ArtifactBulkData.GetCopy() (an ordinary heap copy:
// FBulkData::CanLoadFromDisk() requires a populated BulkChunkId, which only an
// I/O-store/pak load ever sets, never an editor-domain loose-file load); `!WITH_EDITOR`
// (packaged runtime) keeps the StealFileMapping() + ForceBulkDataResident() retry, the
// actually load-bearing memory-mapping path. **WITH_EDITOR is a COMPILE-TIME switch, not a
// property of which package is being loaded** -- loading a genuinely cooked .uasset (with its
// real .m.ubulk sidecar) FROM WITHIN THE EDITOR still takes the GetCopy() branch and
// IsPayloadMemoryMapped() is still false, because the editor build was compiled with
// WITH_EDITOR=1 regardless of what content it loads. That is the fact behind the split above,
// and the reason no editor-context cell can stand in for the packaged ones.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"

using namespace SuperSLMImportFixtures;

// MM-0: the EDITOR-path negative, and the one cell of this group that genuinely runs under the
// normal editor automation pass. SuperSLMModel.h's own documented contract (confirmed by the
// the WITH_EDITOR production fix, see file header): a fresh ImportFromFile ALWAYS
// produces a heap copy via FMemory::Malloc, on every platform, and never holds a mapped view.
// This is a real, distinct claim from the cooked-load property MM-1 tested -- not a stand-in
// for it -- so it earns its own cell rather than being folded into CK-1's alignment/identity
// checks (SuperSLMCookTests.cpp).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMMemoryMappingEditorImportNeverMapsTest,
	"SuperSLM.L2S0.MemoryMapping.EditorImportNeverMaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMMemoryMappingEditorImportNeverMapsTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(ValidReference()), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}

	TestFalse(TEXT("a fresh ImportFromFile must report IsPayloadMemoryMapped() false on every "
		"platform -- this IS the documented contract (SuperSLMModel.h), not an oversight: "
		"only a Serialize()-driven load of a packaged (non-editor) build can produce a mapped "
		"view"),
		Model->IsPayloadMemoryMapped());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
