// T-2226 -- L2-S0 red suite. §9 dim 9 (Persistence) and §3 (cooked serialization),
// the L2-S0 gate line "a `.sslm` cooks and maps in place on every platform".
//
// REWRITTEN in the T-2226 fix round routed by code review T-2241
// (the code review record, finding S7): "The
// cooked-alignment cell cannot fail; the suite nonetheless reports no absent cell." Read at
// source (SuperSLMModel.cpp's Serialize(), SuperSLMModelImport.cpp's ImportFromFileInternal,
// both real and landed as of T-2227 fix round 2):
//
//   - The pointer comes from `FMemory::Malloc(Size, CookedArtifactAlignment)` on BOTH the
//     import path and the Serialize()-on-load path. Alignment is an argument to the
//     allocator call, not a derived property of anything the plugin serializes or of
//     anything in the `.sslm` file's own bytes -- there is no code path through the plugin's
//     real import or serialization logic that can produce a MISALIGNED pointer without
//     corrupting UE's own allocator contract, which is out of scope for a plugin-level test.
//     CONFIRMED AT SOURCE, not merely asserted: a search for `CookedArtifactAlignment` finds
//     exactly two call sites, both `FMemory::Malloc(..., CookedArtifactAlignment)`, in
//     SuperSLMModelImport.cpp and SuperSLMModel.cpp's Serialize() -- no third path exists.
//   - `GetMappedArtifactData()` is `return MappedArtifactData;` -- a member access. Two calls
//     returning the same pointer is guaranteed by that shape for any implementation that
//     compiles, unless the accessor itself is rewritten to reallocate per call (a bug in the
//     accessor, not in cooking/serialization).
//
// Per the review's own routing (to the coverage work and the tests, not to the
// implementation) and the T-2226 fix-round instruction that follows it: the violation this
// cell needs cannot be constructed through the plugin's real serialization path for the
// ALIGNMENT half specifically -- filed BARE below (CK-1, unchanged assertions, comment
// rewritten to state this plainly rather than claim a mutation that does not exist). The
// property "cooking" actually promises and that IS falsifiable through the real path --
// content fidelity, byte-for-byte, through both import AND a real save/reload round trip --
// is added as CK-2, the genuine FEAT oracle this cell was missing.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"

#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

using namespace SuperSLMImportFixtures;

// CK-1 (BARE, per T-2241 review S7 and the routing instruction that follows it -- kept, not
// removed, because both properties are true and worth stating; neither is claimed as
// discriminating). The pointer is non-null and 4096-aligned, and two accessor calls return
// the same pointer.
//
// NOT A MUTATION-PROVEN CELL: read the file header above for why. Alignment is guaranteed by
// `FMemory::Malloc`'s own contract given `CookedArtifactAlignment` as an argument -- no
// real-path mutation can violate it without corrupting the allocator itself. Pointer identity
// follows from `GetMappedArtifactData()` being a plain member-access one-liner. This cell is
// filed bare rather than dressed up with an invented mutation, because
// "a claim no test can fail is unverified" cuts both ways --
// asserting the true thing plainly beats asserting it while implying a falsifiability that
// is not real).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMCookAlignmentTest,
	"SuperSLM.L2S0.Cook.AlignedAndMappedInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMCookAlignmentTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(ValidReference()), Diag);
	if (!TestNotNull(TEXT("import must succeed before cook alignment can be checked"), Model))
	{
		return false;
	}

	const void* FirstPtr = Model->GetMappedArtifactData();
	TestNotNull(TEXT("GetMappedArtifactData must return a real pointer once imported"), FirstPtr);

	const uint64 Address = reinterpret_cast<uint64>(FirstPtr);
	TestTrue(TEXT("mapped base pointer must be 4096-aligned "
		"(docs/sslm_format.md Load-bearing choice 4: max declared section alignment) -- "
		"a property of FMemory::Malloc's own contract, stated here, not claimed as this "
		"cell's discriminating oracle (see file header, T-2241 review S7)"),
		(Address % 4096) == 0);

	const void* SecondPtr = Model->GetMappedArtifactData();
	TestEqual(TEXT("a second access must return the SAME pointer (member access, not a "
		"per-call reallocation) -- stated here, not claimed as this cell's discriminating "
		"oracle (see file header, T-2241 review S7)"),
		SecondPtr, FirstPtr);

	return true;
}

// CK-2: the genuine FEAT oracle "cooking" promises and CK-1 could not provide -- content
// fidelity, byte-for-byte, through import AND through a real save/reload round trip.
//
// Oracle: the imported artifact's MappedArtifactData bytes match the source .sslm FILE's own
// raw bytes (loaded independently via FFileHelper, never through the import path itself, so
// the reference side of the comparison is not derived from the code under test -- the
// reference must not share its inputs with the thing it grades); the same equality holds again
// on a RELOADED object after a real UPackage save + LoadPackage round trip (mirroring
// SuperSLMSaveReloadTests.cpp's SR-1 mechanism, extended from "size matches" to "every byte
// matches").
//
// MUTATION (the concrete violation this cell exists to catch, confirmed constructible through
// the real serialization path -- USuperSLMModel::Serialize(), SuperSLMModel.cpp): change the
// payload archive call from
//     Ar.Serialize(MappedArtifactData, static_cast<int64>(MappedArtifactSize));
// to a call that writes/reads fewer bytes than MappedArtifactSize -- e.g.
//     Ar.Serialize(MappedArtifactData, static_cast<int64>(MappedArtifactSize / 2));
// -- or to remove the payload Ar.Serialize(...) call entirely (leaving only the earlier
// `Ar << MappedArtifactSize` line). Either mutation still allocates a correctly-sized,
// correctly-aligned buffer on load (CK-1's assertions and SR-1's size assertion all still
// PASS), but the second half of the reloaded buffer is uninitialized/stale FMemory::Malloc
// content rather than the original artifact's bytes -- CK-2's post-reload memcmp is the ONLY
// assertion in this suite that would catch it. This is exactly the "cell that cannot fail"
// gap T-2241 review S7 named: CK-1 alone could not have distinguished a correctly-cooked
// artifact from a truncated one.
//
// NOT EXECUTED HERE: this test needs a full UE engine build (package save/load, garbage
// collection) that no toolchain in this authoring environment can provide -- confirmed
// red/unexecuted by construction, matching every other C++ Automation Test in this suite
// (SuperSLMImportTests.cpp, SuperSLMSaveReloadTests.cpp). The implementation's own re-run of
// `SuperSLM.L2S0.*` is what confirms it, and confirms the mutation above actually flips it,
// for real.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMCookContentFidelityTest,
	"SuperSLM.L2S0.Cook.ContentFidelityThroughSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMCookContentFidelityTest::RunTest(const FString& Parameters)
{
	// Independent reference: the source .sslm file's own raw bytes, read directly -- never
	// through FSuperSLMModelImport, so this is not the code under test grading itself.
	TArray<uint8> SourceBytes;
	if (!TestTrue(TEXT("must be able to read the source fixture file independently"),
		FFileHelper::LoadFileToArray(SourceBytes, *AbsolutePath(ValidReference()))))
	{
		return false;
	}

	const FString PackageName = TEXT("/Temp/SuperSLMUnrealTests/CookContentFidelityModel");
	const FString AssetName = TEXT("CookContentFidelityModel");

	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("CreatePackage must succeed"), Package))
	{
		return false;
	}
	Package->FullyLoad();

	FSuperSLMImportDiagnostic ImportDiagnostic;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(ValidReference()), Package, FName(*AssetName), ImportDiagnostic);
	if (!TestNotNull(TEXT("import into a real package must succeed"), Model))
	{
		return false;
	}

	// Content fidelity immediately after import -- before any save/reload is involved, so a
	// failure here isolates to the import path's copy, not to Serialize().
	const void* ImportedPtr = Model->GetMappedArtifactData();
	const int64 ImportedSize = Model->GetMappedArtifactSize();
	if (!TestNotNull(TEXT("imported artifact must have mapped data"), ImportedPtr))
	{
		return false;
	}
	TestEqual(TEXT("imported size must match the source file's own byte count"),
		ImportedSize, static_cast<int64>(SourceBytes.Num()));
	TestTrue(TEXT("imported bytes must match the source file byte-for-byte (post-import)"),
		FMemory::Memcmp(ImportedPtr, SourceBytes.GetData(), SourceBytes.Num()) == 0);

	// Save to disk, drop every reference, force GC, reload fresh -- the oracle below is on
	// the RELOADED object, never the still-resident `Model` pointer (see
	// SuperSLMSaveReloadTests.cpp's SR-1 for the same discipline and its own rationale).
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, Model, *PackageFileName, SaveArgs);
	if (!TestTrue(TEXT("SavePackage must succeed"), bSaved))
	{
		return false;
	}

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
	if (!TestNotNull(TEXT("reloaded artifact must have mapped data"), ReloadedPtr))
	{
		return false;
	}
	TestEqual(TEXT("reloaded size must match the source file's own byte count"),
		ReloadedSize, static_cast<int64>(SourceBytes.Num()));
	TestTrue(TEXT("reloaded bytes must match the source file byte-for-byte -- the mutation "
		"named in this file's own header (truncating or dropping the payload "
		"Ar.Serialize(...) call in USuperSLMModel::Serialize()) fails ONLY this assertion, "
		"which is the cell T-2241 review S7 found missing"),
		FMemory::Memcmp(ReloadedPtr, SourceBytes.GetData(), SourceBytes.Num()) == 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
