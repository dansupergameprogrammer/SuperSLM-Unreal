// T-2226 fix round 3 (routed by code review T-2241,
// the code review record, Critical finding C1). Red
// UE Automation Test for the bare cell the review found: "USuperSLMModel save -> reload must
// preserve the mapped artifact" (plan §3, §10 L2-S0 gate: "the artifact bytes are cooked into
// the package memory-mappable and aligned"; "cooked serialization at 4096 alignment").
//
// C1, quoted from the review: "Because MappedArtifactData is a raw pointer and not a
// UPROPERTY, a USuperSLMModel that is ever saved into a package and reloaded comes back with
// MappedArtifactData == nullptr and MappedArtifactSize == 0, with no error and no guard ...
// The cell that would have caught it is the cell that cannot fail" -- because no cell existed
// to catch it at all. This file is that cell.
//
// This test coordinates ONLY through the plan (§3) and the review's own quoted reproduction
// (the class name USuperSLMModel and the field names MappedArtifactData/MappedArtifactSize) --
// it does NOT read the implementation's in-progress fix to SuperSLMModel.h/SuperSLMModelImport.cpp,
// which is being actively edited for Critical 1 as this file is authored. It specifies the
// interface it needs, per this suite's standing practice
// (the red-suite record §4): an Outer/Name overload
// on FSuperSLMModelImport::ImportFromFile so the imported asset can be created inside a real,
// saveable UPackage rather than only the transient package the original C1 finding quoted
// (`NewObject<USuperSLMModel>(GetTransientPackage())`) -- mirroring
// the (Path, Outer, Name, ..., Error) import shape of a sibling plugin in the project's private
// history (not published here), so the two plugins' import entry points stay consistent.
// `GetMappedArtifactData()`/`GetMappedArtifactSize()` continue this suite's own established
// accessor-based interface (SuperSLMCookTests.cpp's CK-1), which the implementation reconciles
// against whatever the review's raw field names become on the class.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

using namespace SuperSLMImportFixtures;

// SR-1: the FEAT oracle for the C1 fix. Import the real, full valid fixture (PROVENANCE.md)
// into a real, non-transient content package; save it to disk; drop every live reference and
// force a GC pass so nothing keeps the original object resident; reload the package fresh
// from disk; assert the RELOADED object's mapped artifact is present and the same size as at
// import time.
//
// A Serialize override that writes nothing, or writes but never repopulates the runtime
// pointer on load, passes a compile check and an "the .uasset file exists on disk" check
// while still failing this test -- which is exactly the failure mode §10's own gate line
// ("cooked serialization at 4096 alignment") exists to prevent and the review found
// uncovered. The oracle is on the RELOADED object specifically (found via FindObject after
// LoadPackage, never the pre-save `Model` pointer this test also holds), because asserting on
// the original object would pass trivially regardless of whether serialization does anything
// at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMSaveReloadPreservesMappedArtifactTest,
	"SuperSLM.L2S0.Cook.SaveReloadPreservesMappedArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMSaveReloadPreservesMappedArtifactTest::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Temp/SuperSLMUnrealTests/SaveReloadModel");
	const FString AssetName = TEXT("SaveReloadModel");

	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("CreatePackage must succeed"), Package))
	{
		return false;
	}
	Package->FullyLoad();

	FSuperSLMImportDiagnostic ImportDiagnostic;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(ValidReference()), Package, FName(*AssetName), ImportDiagnostic);
	if (!TestNotNull(TEXT("import into a real (non-transient) package must succeed"), Model))
	{
		return false;
	}
	TestTrue(TEXT("Diagnostic.bAccepted"), ImportDiagnostic.bAccepted);

	const void* OriginalPtr = Model->GetMappedArtifactData();
	const int64 OriginalSize = Model->GetMappedArtifactSize();
	if (!TestNotNull(TEXT("imported asset must have a mapped artifact before save"), OriginalPtr))
	{
		return false;
	}
	TestTrue(TEXT("imported artifact must have a positive size before save"), OriginalSize > 0);

	// Save to disk -- a real .uasset file, not merely an in-memory package state.
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, Model, *PackageFileName, SaveArgs);
	if (!TestTrue(TEXT("SavePackage must succeed"), bSaved))
	{
		return false;
	}

	// Drop every reference this test holds, force a full GC pass, then reload the package
	// fresh from disk -- the oracle is on the object that comes back OUT of that reload, not
	// the one still resident from the import above. A test that never actually round-trips
	// through disk would not discriminate "never wrote anything" from "wrote it correctly."
	Model = nullptr;
	Package->ClearFlags(RF_Standalone);
	Package = nullptr;
	CollectGarbage(RF_NoFlags, true);

	UPackage* ReloadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!TestNotNull(TEXT("LoadPackage must find the package saved above"), ReloadedPackage))
	{
		return false;
	}
	USuperSLMModel* ReloadedModel = FindObject<USuperSLMModel>(ReloadedPackage, *AssetName);
	if (!TestNotNull(TEXT("the reloaded package must contain the saved USuperSLMModel"), ReloadedModel))
	{
		return false;
	}

	const void* ReloadedPtr = ReloadedModel->GetMappedArtifactData();
	const int64 ReloadedSize = ReloadedModel->GetMappedArtifactSize();
	TestNotNull(TEXT("the mapped artifact must be non-null after reload -- the review's own "
		"reproduction (C1): a raw non-UPROPERTY pointer reloads null with zero size, silently"),
		ReloadedPtr);
	TestEqual(TEXT("the mapped artifact size after reload must match the size before save"),
		ReloadedSize, OriginalSize);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
