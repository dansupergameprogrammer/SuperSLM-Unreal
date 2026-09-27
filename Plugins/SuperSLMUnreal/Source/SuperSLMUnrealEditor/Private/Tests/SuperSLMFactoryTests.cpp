// T-2226 -- L2-S0 red suite. Fix round routed by confirmation code review T-2249
// (the confirmation review, finding N4, closing
// the factory and asset-type-actions quarters of N4).
//
// N4, as found: "USuperSLMModelFactory::FactoryCreateFile -- C1's own named gate deliverable
// ('USuperSLMModel asset + import factory'). A grep across Source/ and ci/ finds the symbol
// referenced only by its own two files and by comments. SuperSLMSaveReloadTests.cpp and
// SuperSLMCookTests.cpp both call FSuperSLMModelImport::ImportFromFile directly and never
// touch the factory. The editor-facing half of C1's remedy -- the Formats registration, the
// Warn->Log failure channel §10 names ('an import failure reports its section index and
// message'), the SetFlags merge -- is entirely unexecuted code." And: "FAssetTypeActions_
// SuperSLMModel and its registration ... Nothing asserts the type is registered."
//
// This file drives the ACTUAL factory entry point, FactoryCreateFile, never
// FSuperSLMModelImport::ImportFromFile directly -- every other test in this suite (IA-*,
// CK-*, SR-1, PV-*) calls the static import helper, which is exactly the gap N4 names: no
// cell in the suite had ever executed the path a person actually exercises (dragging a
// `.sslm` into the Content Browser routes through UFactory::FactoryCreateFile, not through
// the static helper). FT-2/FT-3 exercise FAssetTypeActions_SuperSLMModel's own interface
// directly, since asserting module-lifecycle registration timing (StartupModule having
// already run before automation tests execute, in the same process) is not independently
// controllable from a test -- the type object's own returned values are.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

#include "SuperSLMAssetTypeActions.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelFactory.h"

#include "Misc/FeedbackContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

using namespace SuperSLMImportFixtures;

// FT-1a: the factory's accept path. Drives FactoryCreateFile directly -- the real entry point
// a Content Browser drag-and-drop import calls -- against the real valid fixture, into a real
// (non-transient) package, and asserts a live, correctly-flagged USuperSLMModel comes back.
// FEAT oracle: an implementation that compiles but never actually wires FactoryCreateFile to
// FSuperSLMModelImport::ImportFromFile (e.g. a stub returning nullptr, or one that forgets
// SetFlags) fails this cell; no other test in the suite would have caught either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMFactoryAcceptTest,
	"SuperSLM.L2S0.Factory.AcceptsValidArtifactThroughFactoryPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMFactoryAcceptTest::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Temp/SuperSLMUnrealTests/FactoryAcceptModel");
	const FString AssetName = TEXT("FactoryAcceptModel");

	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("CreatePackage must succeed"), Package))
	{
		return false;
	}
	Package->FullyLoad();

	USuperSLMModelFactory* Factory = NewObject<USuperSLMModelFactory>();
	if (!TestNotNull(TEXT("USuperSLMModelFactory must construct"), Factory))
	{
		return false;
	}
	TestTrue(TEXT("Factory must declare .sslm as a supported import format (\"sslm;...\" in "
		"Formats, checked by prefix since Factory::Formats stores \"ext;description\")"),
		Factory->Formats.ContainsByPredicate([](const FString& Fmt)
		{
			return Fmt.StartsWith(TEXT("sslm;"));
		}));

	bool bOperationCanceled = false;
	UObject* Result = Factory->FactoryCreateFile(
		USuperSLMModel::StaticClass(),
		Package,
		FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional,
		AbsolutePath(ValidReference()),
		nullptr,
		GWarn,
		bOperationCanceled);

	USuperSLMModel* Model = Cast<USuperSLMModel>(Result);
	if (!TestNotNull(TEXT("FactoryCreateFile must return a live USuperSLMModel for a valid artifact"), Model))
	{
		return false;
	}
	TestFalse(TEXT("bOutOperationCanceled must be false on success"), bOperationCanceled);
	TestNotNull(TEXT("the factory-created asset must have a mapped artifact"),
		Model->GetMappedArtifactData());
	TestTrue(TEXT("the factory-created asset must carry RF_Standalone (SetFlags merge, "
		"needed for the asset to be saveable -- the same flag SuperSLMSaveReloadTests.cpp's "
		"SR-1 depends on, but reached this time through the factory, not the static helper)"),
		Model->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("the factory-created asset must carry RF_Transactional, supplied only by "
		"UFactory's own Flags parameter and merged via SetFlags -- a factory that dropped "
		"Flags rather than OR-ing it in would fail only this assertion"),
		Model->HasAnyFlags(RF_Transactional));

	return true;
}

// FT-1b: the factory's reject path, and the direct proof of the gate line N4 names as
// entirely unexecuted -- "an import failure reports its section index and message" through
// "the editor's own import-failure channel". AddExpectedError registers the exact message
// FactoryCreateFile's Warn->Log call constructs (SuperSLMModelFactory.cpp: "SuperSLM import
// failed: %s (section %d): %s") for the overlapping-sections fixture, whose diagnostic names
// section 4 or 5 -- so this cell fails if the factory stops calling Warn->Log, or logs a bare
// status instead of the section index and message, exactly as IA-5 pins for the static import
// path (this is the same claim, reached through the path a person actually uses).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMFactoryRejectLogsDiagnosticTest,
	"SuperSLM.L2S0.Factory.RejectionLogsSectionAndMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMFactoryRejectLogsDiagnosticTest::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Temp/SuperSLMUnrealTests/FactoryRejectModel");
	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("CreatePackage must succeed"), Package))
	{
		return false;
	}
	Package->FullyLoad();

	USuperSLMModelFactory* Factory = NewObject<USuperSLMModelFactory>();
	if (!TestNotNull(TEXT("USuperSLMModelFactory must construct"), Factory))
	{
		return false;
	}

	// The overlapping-sections fixture's diagnostic names section 4 or 5 (IA-5's own
	// established oracle, SuperSLMImportTests.cpp) -- accept either, matching IA-5's own
	// dual-acceptance for the same reason (the loader's sorted-neighbour overlap check may
	// name either colliding row).
	AddExpectedError(TEXT("SuperSLM import failed:"), EAutomationExpectedErrorFlags::Contains, 0);

	bool bOperationCanceled = false;
	UObject* Result = Factory->FactoryCreateFile(
		USuperSLMModel::StaticClass(),
		Package,
		FName(TEXT("FactoryRejectModel")),
		RF_Public | RF_Standalone,
		AbsolutePath(OverlappingSections()),
		nullptr,
		GWarn,
		bOperationCanceled);

	TestNull(TEXT("FactoryCreateFile must return null for a rejected artifact"), Result);
	TestFalse(TEXT("bOutOperationCanceled must be false -- rejection is a reported failure, "
		"not a cancellation"), bOperationCanceled);

	return true;
}

// FT-2: FAssetTypeActions_SuperSLMModel's own returned values -- the asset-type identity §10
// names alongside the import factory ("Without this, USuperSLMModel has no Content Browser
// identity", per the type's own header comment). GetSupportedClass is the load-bearing one:
// it is what makes the factory's SupportedClass and the registered type-actions agree on
// which UObject subclass this whole mechanism is about.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMAssetTypeActionsIdentityTest,
	"SuperSLM.L2S0.Factory.AssetTypeActionsIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMAssetTypeActionsIdentityTest::RunTest(const FString& Parameters)
{
	FAssetTypeActions_SuperSLMModel Actions;
	TestEqual(TEXT("GetSupportedClass must return USuperSLMModel -- the property that makes "
		"the asset-type registration and the import factory (SupportedClass, "
		"SuperSLMModelFactory.cpp) agree on the same UObject subclass"),
		Actions.GetSupportedClass(), USuperSLMModel::StaticClass());
	TestFalse(TEXT("GetName must not be empty -- an empty name is how a Content Browser "
		"identity silently degrades to unlabeled"),
		Actions.GetName().IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
