// Precache plan rev 1.18.7, §5.8 *Entries defined by text* (PC-16 (b)); the [U3] cells 2.17, 2.18, 7.20 and 9.12.
// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on. Written red first: at d72a2e920 none of it compiles (no Input, Text, TextHash, TokenizedModelHash, FindEntrySpec, CookedImage,
// change hooks or tokenize counter existed), and the host twins (ci/host/prefix_cache/test_prefix_set_text.cpp)
// failed 7 of 8 against stubs of the new core rules.
//
//   SuperSLM.PrefixCache.Cpu.Text.Edges            2.17: per-entry failure (empty text, over context_cap), the
//                                                  edit and model clears, cook TextChanged from a stale text
//                                                  hash, the nested suffix                              [U3]
//   SuperSLM.PrefixCache.Cpu.Text.NoTokenizer      2.17: an artifact with no tokenizer section fails the Text
//                                                  entry by name, keeps its bytes; the Tokens entry builds  [U3]
//   SuperSLM.PrefixCache.Cpu.Text.ModelHashStale   2.17: one Text and one Tokens entry, the model hash stale:
//                                                  TextChanged (not StaleArtifact), skipped at registration;
//                                                  the Tokens entry follows 2.9                         [U3]
//   SuperSLM.PrefixCache.Cpu.Text.ModelMismatch    2.18: FindEntrySpec and RegisterPrefixSet on a mixed set
//                                                  against another configured model                     [U3]
//   SuperSLM.PrefixCache.Cpu.Text.KeyedByIds       7.20: equal ids, equal keys, a Cooked hit through
//                                                  FindEntrySpec, no tokenize call at runtime           [U3]
//   SuperSLM.PrefixCache.Cpu.Text.Persistence      9.12: an asset saved before the text fields loads as
//                                                  Tokens; a Text set survives save, reload and cook    [U3]
//   SuperSLM.PrefixCache.Cpu.Text.CookSerialize    2.17: a stale set serialized through a cooking archive
//                                                  writes the cleared copy and keeps the object's own    [U3]
//
// Fixtures. NoTokenizer uses PC-12's three-schema artifact (SUPERSLM_PRECACHE_PC12_ARTIFACTS, three_schema.sslm:
// the engine's synthetic fixture, which has no tokenizer section), as the cooked cells do. ModelMismatch uses the
// shipped 1.5B artifact as model B, as the U2 review cells do. Persistence's old-asset leg needs a prefix set saved
// by the U3 class (before the text fields): SUPERSLM_PRECACHE_U3_SET_ASSET names its .uasset file (the U4 box run
// keeps one, README "The Text cells"); the leg fails by name when it is missing.
//
// The cook legs run what a cook runs: the pre-save hook's function (SuperSLMPrefixSetEditor::ValidateAtCook) and
// the cooked package's copy (SuperSLMPrefixSetTools::CookedImage, which USuperSLMPrefixSet::Serialize writes when
// the archive is cooking). The box's packaged run cooks a mixed set for real (u4.ps1, the set and residency pieces).
//
// Mutants: a failing entry that clears its own bytes, or blocks the others' store -> Edges red; the edit hook not
// clearing -> Edges red; a Text entry taking 2.9's stale-artifact path -> ModelHashStale red (StaleArtifact named,
// registered, served by recompute from stale ids); tokenize the whole text then split -> Edges' nested leg red; no
// model check in FindEntrySpec, or one on Text entries only -> ModelMismatch red; the runtime re-tokenizing through
// either site -> KeyedByIds red on the counter; the build prepending a token -> KeyedByIds red on the ids; the new
// fields serialized with no default (an old asset loads as Text) -> Persistence red; USuperSLMPrefixSet::Serialize
// not swapping the cooked copy in, or not swapping it back -> CookSerialize red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/ArchiveSavePackageData.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"
#include "SuperSLMTokenizeTestSeam.h"
#include "UObject/ArchiveCookContext.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/Linker.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace SuperSLMPrefixSetTextTests
{
	const TCHAR* const kGreeting = TEXT("The lighthouse keeper of Saltmere kept a ledger of every ship that passed.");
	const TCHAR* const kFarewell = TEXT("Mind the reef on your way out, and give my regards to the pilot.");
	// The nested fixture: "keep" | "er of ..." merges into " keeper" when tokenized whole, so
	// tokenize(parent ‖ suffix) ≠ tokenize(parent) ‖ tokenize(suffix). Asserted in the test, not assumed.
	const TCHAR* const kNestedParent = TEXT("The lighthouse keep");
	const TCHAR* const kNestedSuffix = TEXT("er of Saltmere smiled.");

	USuperSLMPrefixSet* NewTextSet(USuperSLMModel* Model)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		return Set;
	}

	FSuperSLMPrefixSetEntry TextEntry(const TCHAR* Name, const FString& Text, FName Parent = NAME_None)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(Name);
		E.Input = ESuperSLMPrefixEntryInput::Text;
		E.Text = Text;
		E.ParentName = Parent;
		return E;
	}

	FSuperSLMPrefixSetEntry TokensEntry(const TCHAR* Name, const TArray<int32>& Tokens)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(Name);
		E.Tokens = Tokens;
		return E;
	}

	bool TextRig(FAutomationTestBase& T, FPagingRig& Rig)
	{
		if (!Rig.Init(T) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(T, Rig))
		{
			return false;
		}
		return ConfigureCacheLegacy(T, Rig, WithCache(MakeConfig(2, 0, 1, 1), BytesForPages(Rig.Geo, 64)));
	}

	bool BuildTextSet(FAutomationTestBase& T, USuperSLMPrefixSet& Set, const TCHAR* What, TArray<FSuperSLMPrefixSetFinding>* OutFindings = nullptr)
	{
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		const bool bBuilt = SuperSLMPrefixSetEditor::BuildAndLog(Set, Findings, Error);
		if (OutFindings != nullptr)
		{
			*OutFindings = Findings;
		}
		return T.TestTrue(*FString::Printf(TEXT("%s: the set builds (%s)"), What, *Error), bBuilt);
	}

	int32 CountFinding(const TArray<FSuperSLMPrefixSetFinding>& Findings, const TCHAR* Finding, const TCHAR* Entry)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			N += F.Finding == FName(Finding) && F.Entry == FName(Entry) ? 1 : 0;
		}
		return N;
	}

	const FSuperSLMPrefixSetBuiltEntry* CpuBytes(const USuperSLMPrefixSet& Set, FName Entry)
	{
		for (const FSuperSLMPrefixSetBuild& B : Set.Builds)
		{
			if (B.Backend != 0)
			{
				continue;
			}
			for (const FSuperSLMPrefixSetBuiltEntry& E : B.Entries)
			{
				if (E.Name == Entry)
				{
					return &E;
				}
			}
		}
		return nullptr;
	}

	TArray<uint8> BytesOf(const USuperSLMPrefixSet& Set, FName Entry)
	{
		const FSuperSLMPrefixSetBuiltEntry* E = CpuBytes(Set, Entry);
		return E != nullptr ? E->Wrapper : TArray<uint8>();
	}

	int32 IndexOf(const USuperSLMPrefixSet& Set, FName Entry)
	{
		return Set.Entries.IndexOfByPredicate([Entry](const FSuperSLMPrefixSetEntry& E) { return E.Name == Entry; });
	}

	FName SpecReason(const USuperSLMPrefixSet& Set, const USuperSLMModel* Configured, FName Entry)
	{
		FSuperSLMPrefixSpec Out;
		FName Parent;
		FName Reason;
		SuperSLMPrefixSetTools::FindEntrySpec(Set, Configured, Entry, Out, Parent, Reason);
		return Reason;
	}

	// The details panel's edit of Entries[Index].Text (or .Input): the hook the editor runs after the change.
	void EditEntryProperty(USuperSLMPrefixSet& Set, int32 Index, FName Property)
	{
		FProperty* EntriesProp = FindFProperty<FProperty>(USuperSLMPrefixSet::StaticClass(), GET_MEMBER_NAME_CHECKED(USuperSLMPrefixSet, Entries));
		FProperty* Leaf = FindFProperty<FProperty>(FSuperSLMPrefixSetEntry::StaticStruct(), Property);
		FPropertyChangedEvent Event(Leaf, EPropertyChangeType::ValueSet);
		Event.SetActiveMemberProperty(EntriesProp);
		TArray<TMap<FString, int32>> Indices;
		Indices.AddDefaulted();
		Indices[0].Add(EntriesProp->GetName(), Index);
		Event.SetArrayIndexPerObject(Indices);
		Event.ObjectIteratorIndex = 0;
		Set.PostEditChangeProperty(Event);
	}

	void EditModel(USuperSLMPrefixSet& Set, USuperSLMModel* Model)
	{
		Set.Model = Model;
		FProperty* ModelProp = FindFProperty<FProperty>(USuperSLMPrefixSet::StaticClass(), GET_MEMBER_NAME_CHECKED(USuperSLMPrefixSet, Model));
		FPropertyChangedEvent Event(ModelProp, EPropertyChangeType::ValueSet);
		Set.PostEditChangeProperty(Event);
	}

	bool ResolveEnvFile(FAutomationTestBase& T, const TCHAR* Variable, const TCHAR* File, FString& OutPath)
	{
		const FString Dir = FPlatformMisc::GetEnvironmentVariable(Variable);
		OutPath = File != nullptr ? FPaths::Combine(Dir, File) : Dir;
		return T.TestFalse(*FString::Printf(TEXT("%s is set"), Variable), Dir.IsEmpty())
			&& T.TestTrue(*FString::Printf(TEXT("%s exists"), *OutPath), FPaths::FileExists(OutPath));
	}

	USuperSLMModel* ImportModelB(FAutomationTestBase& T)
	{
		FString Path, Reason;
		if (!ResolveArtifact(EPagingArtifact::Shipped15B, Path, Reason))
		{
			T.AddError(Reason);
			return nullptr;
		}
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		return T.TestNotNull(TEXT("model B imports"), Model) && T.TestTrue(TEXT("model B is accepted"), Diag.bAccepted) ? Model : nullptr;
	}

	// Saves Object's package to its file, drops it, collects garbage and loads it again; returns the reloaded
	// object of the same name (SuperSLMCookTests.cpp's discipline: the oracle is on the reloaded object).
	template <typename T>
	T* SaveAndReload(FAutomationTestBase& Test, T* Object, const FString& PackageName, const FString& AssetName)
	{
		UPackage* Package = Object->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!Test.TestTrue(*FString::Printf(TEXT("%s saves"), *PackageName), UPackage::SavePackage(Package, Object, *File, Args)))
		{
			return nullptr;
		}
		// Every object in the package is dropped (a model imported into it as well), so the reload reads the file.
		ForEachObjectWithPackage(Package, [](UObject* O) { O->ClearFlags(RF_Standalone); return true; });
		Package->ClearFlags(RF_Standalone);
		CollectGarbage(RF_NoFlags, true);
		UPackage* Reloaded = LoadPackage(nullptr, *PackageName, LOAD_None);
		return Test.TestNotNull(*FString::Printf(TEXT("%s reloads"), *PackageName), Reloaded) ? FindObject<T>(Reloaded, *AssetName) : nullptr;
	}

	// Unloads a fixture package (its loader released, its objects collected) and deletes its file, so a second run
	// in the same editor session starts from nothing.
	void DropFixturePackage(const FString& PackageName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			ForEachObjectWithPackage(Package, [](UObject* O) { O->ClearFlags(RF_Standalone); return true; });
			Package->ClearFlags(RF_Standalone);
			ResetLoaders(Package);
		}
		CollectGarbage(RF_NoFlags, true);
		IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), false, true, true);
	}

	// What the cook's save does to the set (§5.8 *Staleness*): Serialize through a saving archive that carries a cook
	// context for the running platform, as SavePackage's cooking archive does.
	TArray<uint8> SerializeForCook(USuperSLMPrefixSet& Set)
	{
		TArray<uint8> Bytes;
		ITargetPlatformManagerModule& Platforms = FModuleManager::LoadModuleChecked<ITargetPlatformManagerModule>(TEXT("TargetPlatform"));
		const ITargetPlatform* Platform = Platforms.GetRunningTargetPlatform();
		FObjectSaveContextData SaveData;
		FObjectSavePackageSerializeContext SaveContext(SaveData);
		FArchiveCookContext CookContext(nullptr, UE::Cook::ECookType::ByTheBook, UE::Cook::ECookingDLC::No, Platform, nullptr);
		FArchiveSavePackageData PackageData(SaveContext, Platform, &CookContext);
		FObjectWriter Writer(Bytes);
		Writer.SetSavePackageData(&PackageData);
		Set.Serialize(Writer);
		return Bytes;
	}

	bool SameEntries(const TArray<FSuperSLMPrefixSetEntry>& A, const TArray<FSuperSLMPrefixSetEntry>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 I = 0; I < A.Num(); ++I)
		{
			if (A[I].Name != B[I].Name || A[I].Input != B[I].Input || A[I].Text != B[I].Text || A[I].Tokens != B[I].Tokens
				|| A[I].TextHash != B[I].TextHash || A[I].TokenizedModelHash != B[I].TokenizedModelHash || A[I].SchemaName != B[I].SchemaName)
			{
				return false;
			}
		}
		return true;
	}

	bool SameBuilds(const TArray<FSuperSLMPrefixSetBuild>& A, const TArray<FSuperSLMPrefixSetBuild>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 I = 0; I < A.Num(); ++I)
		{
			if (A[I].Backend != B[I].Backend || A[I].Entries.Num() != B[I].Entries.Num())
			{
				return false;
			}
			for (int32 J = 0; J < A[I].Entries.Num(); ++J)
			{
				if (A[I].Entries[J].Name != B[I].Entries[J].Name || A[I].Entries[J].Wrapper != B[I].Entries[J].Wrapper)
				{
					return false;
				}
			}
		}
		return true;
	}
}

using namespace SuperSLMPrefixSetTextTests;

// Cell 2.17: Text entries at the edges, through the editor build, the change hook and the cook.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextEdgesTest, "SuperSLM.PrefixCache.Cpu.Text.Edges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextEdgesTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Plain;
	if (!TextRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 64, 1, Plain))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	FSuperSLMModelShapeFacts Shape;
	if (!TestTrue(TEXT("the model's shape reads"), SuperSLMModelInspector::ReadShape(*Rig.Model, Shape)) || !TestTrue(TEXT("a context cap"), Shape.ContextCap > 0))
	{
		return false;
	}
	// A text that tokenizes past context_cap: the greeting repeated until the subsystem's tokenizer says so.
	FString Long;
	{
		TArray<int32> Ids;
		while (Ids.Num() <= Shape.ContextCap)
		{
			for (int32 I = 0; I < 64; ++I)
			{
				Long += kGreeting;
				Long += TEXT(" ");
			}
			if (!TestTrue(TEXT("the long text tokenizes"), Cpu.Tokenize(Long, Ids)))
			{
				return false;
			}
		}
	}

	// The fixture: every entry built once with known bytes (empty and long as ordinary texts first).
	USuperSLMPrefixSet* Set = NewTextSet(Rig.Model);
	Set->Entries = {TokensEntry(TEXT("plain"), Plain), TextEntry(TEXT("greeting"), kGreeting), TextEntry(TEXT("empty"), kFarewell), TextEntry(TEXT("long"), kFarewell)};
	Set->Entries[3].Text = FString(kFarewell) + TEXT(" Twice.");
	if (!BuildTextSet(*this, *Set, TEXT("first build")))
	{
		return false;
	}
	const TArray<uint8> EmptyBefore = BytesOf(*Set, TEXT("empty"));
	const TArray<uint8> LongBefore = BytesOf(*Set, TEXT("long"));
	const TArray<int32> EmptyIdsBefore = Set->Entries[2].Tokens;
	const TArray<int32> LongIdsBefore = Set->Entries[3].Tokens;
	const TArray<uint8> GreetingBefore = BytesOf(*Set, TEXT("greeting"));
	bool bOk = TestTrue(TEXT("first build: every entry has bytes"), EmptyBefore.Num() > 0 && LongBefore.Num() > 0 && GreetingBefore.Num() > 0 && BytesOf(*Set, TEXT("plain")).Num() > 0);
	bOk &= TestTrue(TEXT("first build: a Text entry stores its ids and hashes"), Set->Entries[1].Tokens.Num() > 0 && Set->Entries[1].TextHash.Len() == 64 && Set->Entries[1].TokenizedModelHash.Len() == 64);

	// Per-entry failure: empty text and text past the cap fail by name and keep their own ids and bytes; the others
	// build and are stored (greeting's text changed, so its bytes are new).
	Set->Entries[1].Text = FString(kGreeting) + TEXT(" Again.");
	Set->Entries[2].Text.Reset();
	Set->Entries[3].Text = Long;
	TArray<FSuperSLMPrefixSetFinding> Findings;
	bOk &= BuildTextSet(*this, *Set, TEXT("rebuild with two failing entries"), &Findings);
	bOk &= TestEqual(TEXT("empty: EmptyTokens, by name"), CountFinding(Findings, TEXT("EmptyTokens"), TEXT("empty")), 1);
	bOk &= TestEqual(TEXT("long: LongerThanContextCap, by name"), CountFinding(Findings, TEXT("LongerThanContextCap"), TEXT("long")), 1);
	bOk &= TestTrue(TEXT("empty keeps its bytes, byte-equal"), BytesOf(*Set, TEXT("empty")) == EmptyBefore);
	bOk &= TestTrue(TEXT("long keeps its bytes, byte-equal"), BytesOf(*Set, TEXT("long")) == LongBefore);
	bOk &= TestTrue(TEXT("empty keeps its ids"), Set->Entries[2].Tokens == EmptyIdsBefore);
	bOk &= TestTrue(TEXT("long keeps its ids"), Set->Entries[3].Tokens == LongIdsBefore);
	TArray<int32> GreetingIds;
	bOk &= TestTrue(TEXT("the new greeting tokenizes"), Cpu.Tokenize(Set->Entries[1].Text, GreetingIds));
	bOk &= TestTrue(TEXT("greeting is rebuilt: new ids"), Set->Entries[1].Tokens == GreetingIds);
	bOk &= TestTrue(TEXT("greeting is rebuilt: new bytes"), BytesOf(*Set, TEXT("greeting")).Num() > 0 && BytesOf(*Set, TEXT("greeting")) != GreetingBefore);
	bOk &= TestTrue(TEXT("plain still has bytes"), BytesOf(*Set, TEXT("plain")).Num() > 0);

	// The editor's change hooks: an edit to greeting's Text clears its ids and bytes; plain is untouched.
	EditEntryProperty(*Set, 1, GET_MEMBER_NAME_CHECKED(FSuperSLMPrefixSetEntry, Text));
	bOk &= TestEqual(TEXT("edit: greeting's ids cleared"), Set->Entries[1].Tokens.Num(), 0);
	bOk &= TestTrue(TEXT("edit: greeting's hashes cleared"), Set->Entries[1].TextHash.IsEmpty() && Set->Entries[1].TokenizedModelHash.IsEmpty());
	bOk &= TestEqual(TEXT("edit: greeting's bytes cleared"), BytesOf(*Set, TEXT("greeting")).Num(), 0);
	bOk &= TestTrue(TEXT("edit: plain untouched"), Set->Entries[0].Tokens == Plain && BytesOf(*Set, TEXT("plain")).Num() > 0);
	// A model change clears every Text entry's ids and bytes (the same model put back still counts as a change).
	bOk &= BuildTextSet(*this, *Set, TEXT("rebuild before the model change"));
	EditModel(*Set, Rig.Model);
	for (const int32 I : {1, 2, 3})
	{
		bOk &= TestEqual(*FString::Printf(TEXT("model change: %s's ids cleared"), *Set->Entries[I].Name.ToString()), Set->Entries[I].Tokens.Num(), 0);
		bOk &= TestEqual(*FString::Printf(TEXT("model change: %s's bytes cleared"), *Set->Entries[I].Name.ToString()), BytesOf(*Set, Set->Entries[I].Name).Num(), 0);
	}
	bOk &= TestTrue(TEXT("model change: the Tokens entry keeps its ids"), Set->Entries[0].Tokens == Plain);

	// The cook: greeting rebuilt, then its stored text hash edited outside the change hook (which would clear it).
	Set->Entries[2].Text = kFarewell;
	Set->Entries[3].Text = FString(kFarewell) + TEXT(" Twice.");
	bOk &= BuildTextSet(*this, *Set, TEXT("rebuild before the cook"));
	Set->Entries[1].TextHash = FString::ChrN(64, TEXT('0'));
	const TArray<FSuperSLMPrefixSetEntry> SourceEntries = Set->Entries;
	const TArray<FSuperSLMPrefixSetBuild> SourceBuilds = Set->Builds;
	{
		FPagingLogCapture Log;
		const TArray<FSuperSLMPrefixSetFinding> Cook = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
		bOk &= TestEqual(TEXT("cook: greeting is TextChanged"), CountFinding(Cook, TEXT("TextChanged"), TEXT("greeting")), 1);
		bOk &= TestEqual(TEXT("cook: one finding"), Cook.Num(), 1);
		bOk &= TestEqual(TEXT("cook: one warning line names greeting and the set"),
			Log.CountContaining(*FString::Printf(TEXT("%s: entry 'greeting'"), *Set->GetPathName())), 1);
	}
	TArray<FSuperSLMPrefixSetEntry> CookedEntries;
	TArray<FSuperSLMPrefixSetBuild> CookedBuilds;
	bOk &= TestEqual(TEXT("cooked copy: one entry cleared"), SuperSLMPrefixSetTools::CookedImage(*Set, CookedEntries, CookedBuilds), 1);
	bOk &= TestEqual(TEXT("cooked copy: greeting has no ids"), CookedEntries[1].Tokens.Num(), 0);
	bOk &= TestTrue(TEXT("cooked copy: greeting has no bytes"), CookedBuilds.Num() == 1 && CookedBuilds[0].Entries[1].Wrapper.Num() == 0);
	bOk &= TestTrue(TEXT("cooked copy: every other entry as the source"), CookedEntries[0].Tokens == SourceEntries[0].Tokens && CookedBuilds[0].Entries[0].Wrapper == SourceBuilds[0].Entries[0].Wrapper
		&& CookedBuilds[0].Entries[2].Wrapper == SourceBuilds[0].Entries[2].Wrapper);
	bOk &= TestTrue(TEXT("the source asset is unchanged by the cook"), Set->Entries[1].Tokens == SourceEntries[1].Tokens && Set->Builds[0].Entries[1].Wrapper == SourceBuilds[0].Entries[1].Wrapper);
	bOk &= TestEqual(TEXT("runtime: FindEntrySpec refuses greeting TextChanged"), SpecReason(*Set, Cpu.GetConfiguredModel(), TEXT("greeting")).ToString(), FString(TEXT("TextChanged")));
	{
		FSuperSLMPrefixSpec Out;
		FName Parent;
		bOk &= TestFalse(TEXT("runtime: the member refuses it too"), Set->FindEntrySpec(Cpu.GetConfiguredModel(), TEXT("greeting"), Out, Parent));
		bOk &= TestEqual(TEXT("runtime: nothing returned"), Out.Tokens.Num(), 0);
	}
	Cpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("runtime: greeting is not registered (three of four)"), Stats(Cpu).CookedEntriesRegistered, 3);
	Cpu.UnregisterPrefixSet(Set);

	// The nested suffix: tokenized alone, nothing prepended.
	TArray<int32> ParentIds, SuffixIds, WholeIds;
	if (!Cpu.Tokenize(kNestedParent, ParentIds) || !Cpu.Tokenize(kNestedSuffix, SuffixIds) || !Cpu.Tokenize(FString(kNestedParent) + kNestedSuffix, WholeIds))
	{
		return TestTrue(TEXT("the nested fixture tokenizes"), false);
	}
	TArray<int32> Joined = ParentIds;
	Joined.Append(SuffixIds);
	if (!TestTrue(TEXT("the nested fixture: tokenize(parent + suffix) != tokenize(parent) + tokenize(suffix)"), WholeIds != Joined))
	{
		return false;
	}
	USuperSLMPrefixSet* Nested = NewTextSet(Rig.Model);
	Nested->Entries = {TextEntry(TEXT("parent"), kNestedParent), TextEntry(TEXT("child"), kNestedSuffix, TEXT("parent"))};
	TArray<FSuperSLMPrefixSetFinding> NestedFindings;
	bOk &= BuildTextSet(*this, *Nested, TEXT("nested"), &NestedFindings);
	bOk &= TestTrue(TEXT("nested: the child's ids are its suffix's tokenization alone"), Nested->Entries[1].Tokens == SuffixIds);
	bOk &= TestTrue(TEXT("nested: the parent's ids are its own text's"), Nested->Entries[0].Tokens == ParentIds);
	bOk &= TestEqual(TEXT("nested: the child builds nothing before U6, by name"), CountFinding(NestedFindings, TEXT("NestedRequiresBudgetMode"), TEXT("child")), 1);
	{
		FSuperSLMPrefixSpec Out;
		FName ParentEntry;
		bOk &= TestTrue(TEXT("nested: FindEntrySpec returns the child's stored ids"), Nested->FindEntrySpec(Cpu.GetConfiguredModel(), TEXT("child"), Out, ParentEntry) && Out.Tokens == SuffixIds);
		bOk &= TestEqual(TEXT("nested: OutParentEntry names the parent"), ParentEntry.ToString(), FString(TEXT("parent")));
		bOk &= TestFalse(TEXT("nested: Out.Parent is left invalid"), Out.Parent.IsValid());
		bOk &= TestTrue(TEXT("root: OutParentEntry is None"), Nested->FindEntrySpec(Cpu.GetConfiguredModel(), TEXT("parent"), Out, ParentEntry) && ParentEntry.IsNone());
	}
	return bOk;
}

// Cell 2.17, the no-tokenizer leg: an artifact with no tokenizer section (TokenizeText's SSLM_ARTIFACT_REJECTED
// branch) fails the Text entry by name; it keeps its previous ids and bytes; the Tokens entry builds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextNoTokenizerTest, "SuperSLM.PrefixCache.Cpu.Text.NoTokenizer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextNoTokenizerTest::RunTest(const FString& Parameters)
{
	FString Path;
	if (!ResolveEnvFile(*this, TEXT("SUPERSLM_PRECACHE_PC12_ARTIFACTS"), TEXT("three_schema.sslm"), Path))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	FSuperSLMModelShapeFacts Shape;
	if (!TestNotNull(*FString::Printf(TEXT("the three-schema artifact imports (%s)"), *Diag.Message), Model)
		|| !TestTrue(TEXT("its shape reads"), SuperSLMModelInspector::ReadShape(*Model, Shape)) || !TestTrue(TEXT("a vocabulary"), Shape.VocabSize > 2))
	{
		return false;
	}
	TArray<int32> A, B;
	for (int32 I = 0; I < 32; ++I)
	{
		A.Add(1 + (131 + I * 7) % (Shape.VocabSize - 1));
		B.Add(1 + (262 + I * 7) % (Shape.VocabSize - 1));
	}
	// The fixture: "text" built once with known bytes (as ids, the only way this artifact builds anything), then
	// switched to Text input outside the change hook, as a set re-pointed at this model would carry it.
	USuperSLMPrefixSet* Set = NewTextSet(Model);
	Set->Entries = {TokensEntry(TEXT("text"), A), TokensEntry(TEXT("tokens"), B)};
	if (!BuildTextSet(*this, *Set, TEXT("first build")))
	{
		return false;
	}
	const TArray<uint8> TextBefore = BytesOf(*Set, TEXT("text"));
	Set->Entries[0].Input = ESuperSLMPrefixEntryInput::Text;
	Set->Entries[0].Text = kGreeting;
	Set->Entries[1].Tokens.Swap(0, 1); // the Tokens entry changes, so its rebuild is visible
	const TArray<uint8> TokensBefore = BytesOf(*Set, TEXT("tokens"));
	TArray<FSuperSLMPrefixSetFinding> Findings;
	bool bOk = BuildTextSet(*this, *Set, TEXT("rebuild"), &Findings);
	bOk &= TestEqual(TEXT("text: TokenizeFailed, by name"), CountFinding(Findings, TEXT("TokenizeFailed"), TEXT("text")), 1);
	const FSuperSLMPrefixSetFinding* Failed = Findings.FindByPredicate([](const FSuperSLMPrefixSetFinding& F) { return F.Finding == FName(TEXT("TokenizeFailed")); });
	bOk &= TestTrue(TEXT("the finding says the artifact has no tokenizer"), Failed != nullptr && Failed->Line.Contains(TEXT("no tokenizer")));
	bOk &= TestTrue(TEXT("text keeps its ids"), Set->Entries[0].Tokens == A);
	bOk &= TestTrue(TEXT("text keeps its bytes, byte-equal"), TextBefore.Num() > 0 && BytesOf(*Set, TEXT("text")) == TextBefore);
	bOk &= TestTrue(TEXT("tokens builds: new bytes"), BytesOf(*Set, TEXT("tokens")).Num() > 0 && BytesOf(*Set, TEXT("tokens")) != TokensBefore);
	return bOk;
}

// Cell 2.17, the model-hash leg: one Text and one Tokens entry, the set's model hash stale. The Text entry is
// TextChanged (no ids, no bytes, refused, skipped at registration); the Tokens entry follows 2.9 (StaleArtifact,
// its bytes ship, recomputed at runtime). Mutant: the Text entry takes 2.9's path -> StaleArtifact named, the
// entry registered and served by recompute from stale ids -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextModelHashTest, "SuperSLM.PrefixCache.Cpu.Text.ModelHashStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextModelHashTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Plain;
	if (!TextRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 64, 2, Plain))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMPrefixSet* Set = NewTextSet(Rig.Model);
	Set->Entries = {TextEntry(TEXT("text"), kGreeting), TokensEntry(TEXT("tokens"), Plain)};
	{
		// Both entries' bytes as a build against another model left them (2.9's seam) ...
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OtherModel(FString(), FString(), true);
		if (!BuildTextSet(*this, *Set, TEXT("another model")))
		{
			return false;
		}
	}
	// ... and the Text entry's stored model hash made stale the same way, outside the change hook.
	FString& Stored = Set->Entries[0].TokenizedModelHash;
	if (!TestEqual(TEXT("the Text entry stored a model hash"), Stored.Len(), 64))
	{
		return false;
	}
	Stored[0] = Stored[0] == TEXT('0') ? TEXT('1') : TEXT('0');

	const TArray<FSuperSLMPrefixSetFinding> Cook = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
	bool bOk = TestEqual(TEXT("cook: text is TextChanged"), CountFinding(Cook, TEXT("TextChanged"), TEXT("text")), 1);
	bOk &= TestEqual(TEXT("cook: text is not StaleArtifact (the mutant)"), CountFinding(Cook, TEXT("StaleArtifact"), TEXT("text")), 0);
	bOk &= TestEqual(TEXT("cook: tokens is StaleArtifact (2.9)"), CountFinding(Cook, TEXT("StaleArtifact"), TEXT("tokens")), 1);
	TArray<FSuperSLMPrefixSetEntry> CookedEntries;
	TArray<FSuperSLMPrefixSetBuild> CookedBuilds;
	bOk &= TestEqual(TEXT("cooked copy: one entry cleared"), SuperSLMPrefixSetTools::CookedImage(*Set, CookedEntries, CookedBuilds), 1);
	bOk &= TestTrue(TEXT("cooked copy: text has no ids and no bytes"), CookedEntries[0].Tokens.Num() == 0 && CookedBuilds[0].Entries[0].Wrapper.Num() == 0);
	bOk &= TestTrue(TEXT("cooked copy: tokens' stale bytes ship"), CookedBuilds[0].Entries[1].Wrapper.Num() > 0);
	bOk &= TestEqual(TEXT("runtime: FindEntrySpec refuses text"), SpecReason(*Set, Cpu.GetConfiguredModel(), TEXT("text")).ToString(), FString(TEXT("TextChanged")));
	bOk &= TestTrue(TEXT("runtime: FindEntrySpec returns tokens"), SpecReason(*Set, Cpu.GetConfiguredModel(), TEXT("tokens")).IsNone());
	Cpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("runtime: RegisterPrefixSet skips text (one registered)"), Stats(Cpu).CookedEntriesRegistered, 1);
	FSuperSLMPrefix L;
	bOk &= RequestReady(*this, Cpu, Spec(Plain), L, TEXT("tokens")) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("tokens: stale bytes refused, recomputed (2.9)"));
	Cpu.ReturnPrefix(L);
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Cell 2.18: a set built for model A holds one Text entry and one Tokens entry; the subsystem is configured with
// model B. RegisterPrefixSet refuses the set (PrefixSetModelMismatch, logged once) and FindEntrySpec refuses both
// entries with the same reason; after a re-Configure() with A and a fresh registration both are found and served
// Cooked; a set registered under A is unregistered by a re-Configure() with B.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextModelMismatchTest, "SuperSLM.PrefixCache.Cpu.Text.ModelMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextModelMismatchTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Plain;
	if (!TextRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 64, 3, Plain))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMModel* const ModelA = Rig.Model;
	USuperSLMModel* const ModelB = ImportModelB(*this);
	if (ModelB == nullptr)
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewTextSet(ModelA);
	Set->Entries = {TextEntry(TEXT("text"), kGreeting), TokensEntry(TEXT("tokens"), Plain)};
	if (!BuildTextSet(*this, *Set, TEXT("model A")))
	{
		return false;
	}
	const TArray<int32> TextIds = Set->Entries[0].Tokens;
	const FSuperSLMRuntimeConfig Config = Rig.Config;

	bool bOk = true;
	{
		const FSuperSLMConfigureReport Report = Cpu.Configure(ModelB, Config);
		bOk &= TestEqual(*FString::Printf(TEXT("Configure(model B) succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
		FPagingLogCapture Log;
		const int64 Before = FallbackCount(Stats(Cpu), TEXT("PrefixSetModelMismatch"));
		Cpu.RegisterPrefixSet(Set);
		bOk &= TestEqual(TEXT("model B: nothing registers"), Stats(Cpu).CookedEntriesRegistered, 0);
		bOk &= TestEqual(TEXT("model B: PrefixSetModelMismatch once"), FallbackCount(Stats(Cpu), TEXT("PrefixSetModelMismatch")) - Before, int64(1));
		// The "registered 0 cooked entries from <set>" line names the set too, so count the mismatch line itself.
		bOk &= TestEqual(TEXT("model B: one mismatch line naming the set"), Log.CountLinesContainingAll({*Set->GetPathName(), TEXT("PrefixSetModelMismatch")}), 1);
		for (const TCHAR* Entry : {TEXT("text"), TEXT("tokens")})
		{
			// The mutant "the check on Text entries only" returns the Tokens entry here.
			bOk &= TestEqual(*FString::Printf(TEXT("model B: FindEntrySpec refuses %s"), Entry), SpecReason(*Set, Cpu.GetConfiguredModel(), Entry).ToString(), FString(TEXT("PrefixSetModelMismatch")));
			FSuperSLMPrefixSpec Out;
			FName Parent;
			bOk &= TestFalse(*FString::Printf(TEXT("model B: the member refuses %s"), Entry), Set->FindEntrySpec(Cpu.GetConfiguredModel(), Entry, Out, Parent));
		}
		// A request for either entry's ids under B gets no Cooked source (the ids may not even be B's vocabulary).
		for (const TArray<int32>& Ids : {TextIds, Plain})
		{
			FSuperSLMPrefix L;
			FString Error;
			if (Cpu.RequestPrefix(Spec(Ids), L, Error))
			{
				WaitLease(Cpu, L, BuildSeconds);
				bOk &= TestNotEqual(TEXT("model B: no Cooked source"), (uint8)Cpu.GetPrefixSource(L), (uint8)ESuperSLMPrefixSource::Cooked);
				Cpu.ReturnPrefix(L);
			}
		}
	}
	{
		const FSuperSLMConfigureReport Report = Cpu.Configure(ModelA, Config);
		bOk &= TestEqual(*FString::Printf(TEXT("Configure(model A) succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
		Cpu.RegisterPrefixSet(Set);
		bOk &= TestEqual(TEXT("model A: both register"), Stats(Cpu).CookedEntriesRegistered, 2);
		for (const TCHAR* Entry : {TEXT("text"), TEXT("tokens")})
		{
			FSuperSLMPrefixSpec Out;
			FName Parent;
			FSuperSLMPrefix L;
			bOk &= TestTrue(*FString::Printf(TEXT("model A: FindEntrySpec finds %s"), Entry), Set->FindEntrySpec(Cpu.GetConfiguredModel(), Entry, Out, Parent));
			bOk &= RequestReady(*this, Cpu, Out, L, Entry) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, Entry);
			Cpu.ReturnPrefix(L);
		}
	}
	{
		// Registered under A; a re-Configure() with B unregisters it.
		const FSuperSLMConfigureReport Report = Cpu.Configure(ModelB, Config);
		bOk &= TestEqual(*FString::Printf(TEXT("Configure(model B) again succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
		bOk &= TestEqual(TEXT("model B again: the set is unregistered"), Stats(Cpu).CookedEntriesRegistered, 0);
	}
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Cell 7.20: "the runtime never tokenizes, and a text entry is keyed by its stored ids". A Text entry and a Tokens
// entry whose ids are the text's tokenization: equal keys (byte-equal wrappers: one disk file, one cooked key), the
// Text entry a Cooked hit through FindEntrySpec, and the tokenize counter 0 across Configure(), RegisterPrefixSet,
// FindEntrySpec, the request and the adopt, > 0 at the build. The built ids equal USuperSLMSubsystem::Tokenize's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextKeyedByIdsTest, "SuperSLM.PrefixCache.Cpu.Text.KeyedByIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextKeyedByIdsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Ids, Suffix, Twin;
	// The subsystem is configured with the set's own model asset, under which both sites map the same artifact.
	if (!TextRig(*this, Rig) || !TestTrue(TEXT("the text tokenizes (USuperSLMSubsystem::Tokenize)"), Rig.Cpu->Tokenize(kGreeting, Ids))
		|| !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 4, Suffix) || !TwinTokens(*this, *Rig.Cpu, Ids, Suffix, Twin))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMPrefixSet* Set = NewTextSet(Rig.Model);
	Set->Entries = {TextEntry(TEXT("text"), kGreeting), TokensEntry(TEXT("tokens"), Ids)};
	const int64 BuildCallsBefore = SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::TokenizeText);
	if (!BuildTextSet(*this, *Set, TEXT("text and tokens")))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the build tokenized (the counter's positive clause)"), SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::TokenizeText) > BuildCallsBefore);
	bOk &= TestTrue(TEXT("the built ids equal USuperSLMSubsystem::Tokenize's (nothing prepended)"), Set->Entries[0].Tokens == Ids);
	bOk &= TestTrue(TEXT("equal ids, equal keys: the two wrappers are byte-equal"), BytesOf(*Set, TEXT("text")).Num() > 0 && BytesOf(*Set, TEXT("text")) == BytesOf(*Set, TEXT("tokens")));

	// The runtime: no tokenize call anywhere from Configure() to the adopt.
	SuperSLMTokenizeTestSeam::Reset();
	if (!ConfigureCacheLegacy(*this, Rig, Rig.Config))
	{
		return false;
	}
	Cpu.RegisterPrefixSet(Set);
	FSuperSLMPrefixSpec Out;
	FName Parent;
	bOk &= TestTrue(TEXT("FindEntrySpec finds the Text entry"), Set->FindEntrySpec(Cpu.GetConfiguredModel(), TEXT("text"), Out, Parent));
	FSuperSLMPrefix L;
	bOk &= RequestReady(*this, Cpu, Out, L, TEXT("text")) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, TEXT("text: a Cooked hit"));
	bOk &= EqualsTwin(*this, Cpu, L, Suffix, Twin, TEXT("text: the adopter equals the twin"));
	Cpu.ReturnPrefix(L);
	bOk &= TestEqual(TEXT("TokenizeText calls from Configure() to the adopt"), SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::TokenizeText), int64(0));
	bOk &= TestEqual(TEXT("USuperSLMSubsystem::Tokenize calls from Configure() to the adopt"), SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::SubsystemTokenize), int64(0));
	Cpu.UnregisterPrefixSet(Set);

	// The counter's must-reject: one call through each site reads on that site.
	SuperSLMTokenizeTestSeam::Reset();
	TArray<int32> Scratch;
	FString Error;
	Cpu.Tokenize(TEXT("a"), Scratch);
	bOk &= TestTrue(TEXT("must-reject: the subsystem site counts"), SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::SubsystemTokenize) >= 1
		&& SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::TokenizeText) == 0);
	SuperSLM::TokenizeText(*Rig.Model, TEXT("a"), Scratch, Error);
	bOk &= TestTrue(TEXT("must-reject: the TokenizeText site counts"), SuperSLMTokenizeTestSeam::Calls(SuperSLMTokenizeTestSeam::ESite::TokenizeText) >= 1);
	return bOk;
}

// Cell 9.12: Prefix Set persistence. An asset saved before the text fields existed loads with every entry Input ==
// Tokens and its ids and bytes unchanged, and serves as before; a set with Text entries survives save, reload and
// cook with its Text, ids, text hash and model hash unchanged, and its cooked entries are hits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextPersistenceTest, "SuperSLM.PrefixCache.Cpu.Text.Persistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextPersistenceTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Plain;
	if (!TextRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 64, 5, Plain))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;

	// The old asset: copied into this project under a fixture path and loaded.
	FString OldFile;
	if (ResolveEnvFile(*this, TEXT("SUPERSLM_PRECACHE_U3_SET_ASSET"), nullptr, OldFile))
	{
		const FString OldPackage = TEXT("/Game/SuperSLMTestFixtures/U3SetBeforeText");
		const FString OldTarget = FPackageName::LongPackageNameToFilename(OldPackage, FPackageName::GetAssetPackageExtension());
		bOk &= TestEqual(TEXT("the old asset is copied in"), IFileManager::Get().Copy(*OldTarget, *OldFile), (uint32)COPY_OK);
		UPackage* Package = LoadPackage(nullptr, *OldPackage, LOAD_None);
		USuperSLMPrefixSet* Old = nullptr;
		if (Package != nullptr)
		{
			ForEachObjectWithOuter(Package, [&Old](UObject* Object) { Old = Old != nullptr ? Old : Cast<USuperSLMPrefixSet>(Object); }, EGetObjectsFlags::None);
		}
		if (TestNotNull(TEXT("the old asset loads as a prefix set"), Old))
		{
			bOk &= TestTrue(TEXT("the old asset has entries"), Old->Entries.Num() > 0);
			for (const FSuperSLMPrefixSetEntry& E : Old->Entries)
			{
				const FString Name = E.Name.ToString();
				bOk &= TestEqual(*FString::Printf(TEXT("old %s: Input == Tokens"), *Name), (uint8)E.Input, (uint8)ESuperSLMPrefixEntryInput::Tokens);
				bOk &= TestTrue(*FString::Printf(TEXT("old %s: no text, no hashes"), *Name), E.Text.IsEmpty() && E.TextHash.IsEmpty() && E.TokenizedModelHash.IsEmpty());
				bOk &= TestTrue(*FString::Printf(TEXT("old %s: its ids"), *Name), E.Tokens.Num() > 0);
			}
			// Its ids and bytes unchanged: every wrapper's stored spec still equals its entry's ids, and nothing is
			// TextChanged (a pin change since may leave stale tags, which are not this cell's).
			const TArray<FSuperSLMPrefixSetFinding> Cook = SuperSLMPrefixSetEditor::ValidateAtCook(*Old);
			for (const FSuperSLMPrefixSetFinding& F : Cook)
			{
				for (const TCHAR* Bad : {TEXT("SpecEdited"), TEXT("WrapperInvalid"), TEXT("TextChanged"), TEXT("EmptyTokens"), TEXT("Orphaned")})
				{
					bOk &= TestFalse(*FString::Printf(TEXT("old asset: no %s (%s)"), Bad, *F.Line), F.Finding == FName(Bad));
				}
			}
			// Serves as before: the rig configured with the old asset's own model, every entry the cook found clean is a
			// cooked hit.
			if (Old->Model != nullptr)
			{
				const FSuperSLMConfigureReport Report = Cpu.Configure(Old->Model, Rig.Config);
				bOk &= TestEqual(*FString::Printf(TEXT("Configure(the old asset's model %s) succeeds (%s)"), *Old->Model->GetPathName(), *Report.Message),
					(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
				int32 Served = 0;
				int32 Clean = 0;
				Cpu.RegisterPrefixSet(Old);
				for (const FSuperSLMPrefixSetEntry& E : Old->Entries)
				{
					const bool bClean = !Cook.ContainsByPredicate([&E](const FSuperSLMPrefixSetFinding& F) { return F.Entry == E.Name; });
					FSuperSLMPrefixSpec Out;
					FName Parent;
					FSuperSLMPrefix L;
					Clean += bClean ? 1 : 0;
					if (bClean && Old->FindEntrySpec(Cpu.GetConfiguredModel(), E.Name, Out, Parent))
					{
						bOk &= RequestReady(*this, Cpu, Out, L, *E.Name.ToString()) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, *E.Name.ToString());
						Cpu.ReturnPrefix(L);
						++Served;
					}
				}
				Cpu.UnregisterPrefixSet(Old);
				// Every entry the cook found clean is found and served; a pin change since the asset was saved can leave
				// none clean, which is named rather than passed silently.
				bOk &= TestEqual(TEXT("the old asset: every clean entry is found and served Cooked"), Served, Clean);
				if (Clean == 0)
				{
					AddInfo(TEXT("the old asset has no clean entry under this build (its builds are another pin's); nothing to serve"));
				}
			}
			else
			{
				AddInfo(TEXT("the old asset's model does not load in this project; its serve leg is skipped"));
			}
		}
		// Back to the rig's model, and the copied fixture gone, so a second run copies onto nothing.
		Old = nullptr;
		bOk &= ConfigureCacheLegacy(*this, Rig, Rig.Config);
		DropFixturePackage(OldPackage);
	}
	else
	{
		bOk = false;
	}

	// A Text set in a real package: built, saved, reloaded, cooked.
	const FString PackageName = TEXT("/Game/SuperSLMTestFixtures/TextSetPersistence");
	const FString AssetName = TEXT("TextSetPersistence");
	UPackage* Package = CreatePackage(*PackageName);
	USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(Package, *AssetName, RF_Public | RF_Standalone);
	// The set's model is saved with it: the same artifact imported into the set's package (the rig's own import is
	// transient, and a reference to it does not save). FindEntrySpec and registration compare models by artifact
	// hash, so the rig, configured with Rig.Model, matches it.
	{
		FString ModelPath, Reason;
		if (!TestTrue(*FString::Printf(TEXT("the rig's artifact resolves (%s)"), *Reason), ResolveArtifact(Rig.Artifact, ModelPath, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		Set->Model = FSuperSLMModelImport::ImportFromFile(ModelPath, Package, TEXT("Model"), Diag);
		if (!TestNotNull(TEXT("the model imports into the set's package"), Set->Model.Get()) || !TestTrue(TEXT("the package's model is accepted"), Diag.bAccepted))
		{
			return false;
		}
	}
	Set->Entries = {TextEntry(TEXT("text"), kGreeting), TokensEntry(TEXT("tokens"), Plain)};
	if (!BuildTextSet(*this, *Set, TEXT("text set")))
	{
		return false;
	}
	const TArray<FSuperSLMPrefixSetEntry> Before = Set->Entries;
	const TArray<uint8> TextBytes = BytesOf(*Set, TEXT("text"));
	USuperSLMPrefixSet* Reloaded = SaveAndReload(*this, Set, PackageName, AssetName);
	Set = nullptr;
	if (!TestNotNull(TEXT("the Text set reloads"), Reloaded) || !TestNotNull(TEXT("the reloaded set's model reloads with it"), Reloaded->Model.Get()))
	{
		return false;
	}
	const FSuperSLMPrefixSetEntry& R = Reloaded->Entries[0];
	bOk &= TestEqual(TEXT("reloaded: Input"), (uint8)R.Input, (uint8)ESuperSLMPrefixEntryInput::Text);
	bOk &= TestEqual(TEXT("reloaded: Text"), R.Text, Before[0].Text);
	bOk &= TestTrue(TEXT("reloaded: ids"), R.Tokens == Before[0].Tokens);
	bOk &= TestEqual(TEXT("reloaded: text hash"), R.TextHash, Before[0].TextHash);
	bOk &= TestEqual(TEXT("reloaded: model hash"), R.TokenizedModelHash, Before[0].TokenizedModelHash);
	bOk &= TestTrue(TEXT("reloaded: bytes"), BytesOf(*Reloaded, TEXT("text")) == TextBytes);
	bOk &= TestEqual(TEXT("reloaded: Input of the Tokens entry"), (uint8)Reloaded->Entries[1].Input, (uint8)ESuperSLMPrefixEntryInput::Tokens);
	// The cook: clean, and the cooked copy is the set itself.
	bOk &= TestEqual(TEXT("cook: clean"), SuperSLMPrefixSetEditor::ValidateAtCook(*Reloaded).Num(), 0);
	TArray<FSuperSLMPrefixSetEntry> CookedEntries;
	TArray<FSuperSLMPrefixSetBuild> CookedBuilds;
	bOk &= TestEqual(TEXT("cook: nothing cleared"), SuperSLMPrefixSetTools::CookedImage(*Reloaded, CookedEntries, CookedBuilds), 0);
	bOk &= TestTrue(TEXT("cook: the cooked Text entry is the source's"), CookedEntries[0].Text == R.Text && CookedEntries[0].Tokens == R.Tokens && CookedEntries[0].TextHash == R.TextHash);
	// Its cooked entries are hits.
	if (!ConfigureCacheLegacy(*this, Rig, Rig.Config))
	{
		return false;
	}
	Cpu.RegisterPrefixSet(Reloaded);
	for (const TCHAR* Entry : {TEXT("text"), TEXT("tokens")})
	{
		FSuperSLMPrefixSpec Out;
		FName Parent;
		FSuperSLMPrefix L;
		bOk &= TestTrue(*FString::Printf(TEXT("reloaded: FindEntrySpec finds %s"), Entry), Reloaded->FindEntrySpec(Cpu.GetConfiguredModel(), Entry, Out, Parent));
		bOk &= RequestReady(*this, Cpu, Out, L, Entry) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, Entry);
		Cpu.ReturnPrefix(L);
	}
	Cpu.UnregisterPrefixSet(Reloaded);
	Reloaded = nullptr;
	DropFixturePackage(PackageName);
	return bOk;
}

// Cell 2.17, the cooked package's copy as the cook writes it: a set whose Text entry is stale, serialized through a
// saving archive that carries a cook context, writes that entry with no ids and no bytes, and leaves the object's own
// Entries and Builds as they were (the source asset unchanged). The same set through a plain save writes them whole.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetTextCookSerializeTest, "SuperSLM.PrefixCache.Cpu.Text.CookSerialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetTextCookSerializeTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Plain;
	if (!TextRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 64, 6, Plain))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewTextSet(Rig.Model);
	Set->Entries = {TextEntry(TEXT("text"), kGreeting), TokensEntry(TEXT("tokens"), Plain)};
	if (!BuildTextSet(*this, *Set, TEXT("cook serialize")))
	{
		return false;
	}
	const TArray<int32> TextIds = Set->Entries[0].Tokens;
	const TArray<uint8> TextBytes = BytesOf(*Set, TEXT("text"));
	const TArray<uint8> TokensBytes = BytesOf(*Set, TEXT("tokens"));
	// Stale: the stored text hash no longer matches the text (edited outside the change hook, which would clear it).
	Set->Entries[0].TextHash = FString::ChrN(64, TEXT('0'));
	const TArray<FSuperSLMPrefixSetEntry> EntriesBefore = Set->Entries;
	const TArray<FSuperSLMPrefixSetBuild> BuildsBefore = Set->Builds;
	{
		TArray<FSuperSLMPrefixSetEntry> E;
		TArray<FSuperSLMPrefixSetBuild> B;
		if (!TestEqual(TEXT("precondition: the cooked copy clears one entry"), SuperSLMPrefixSetTools::CookedImage(*Set, E, B), 1)
			|| !TestTrue(TEXT("precondition: the text entry has ids and bytes"), TextIds.Num() > 0 && TextBytes.Num() > 0 && TokensBytes.Num() > 0))
		{
			return false;
		}
	}

	const TArray<uint8> Cooked = SerializeForCook(*Set);
	bool bOk = TestTrue(TEXT("the cook's save writes bytes"), Cooked.Num() > 0);
	// The swap back: the object keeps what it had.
	bOk &= TestTrue(TEXT("after the cook's save: the object's Entries are unchanged"), SameEntries(Set->Entries, EntriesBefore));
	bOk &= TestTrue(TEXT("after the cook's save: the object's Builds are unchanged"), SameBuilds(Set->Builds, BuildsBefore));
	// The written copy: the stale entry with no ids and no bytes, the Tokens entry whole.
	USuperSLMPrefixSet* Read = NewTextSet(nullptr);
	{
		FObjectReader Reader(Cooked);
		Read->Serialize(Reader);
	}
	const int32 TextAt = IndexOf(*Read, TEXT("text"));
	const int32 TokensAt = IndexOf(*Read, TEXT("tokens"));
	if (!TestTrue(TEXT("the cooked copy has both entries"), TextAt != INDEX_NONE && TokensAt != INDEX_NONE))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("cooked copy: the stale Text entry has no ids"), Read->Entries[TextAt].Tokens.Num(), 0);
	bOk &= TestEqual(TEXT("cooked copy: the stale Text entry has no bytes"), BytesOf(*Read, TEXT("text")).Num(), 0);
	bOk &= TestTrue(TEXT("cooked copy: the Tokens entry keeps its ids"), Read->Entries[TokensAt].Tokens == Plain);
	bOk &= TestTrue(TEXT("cooked copy: the Tokens entry keeps its bytes"), BytesOf(*Read, TEXT("tokens")) == TokensBytes);

	// Must-reject: a plain (non-cook) save of the same set writes the stale entry whole.
	TArray<uint8> Plainly;
	{
		FObjectWriter Writer(Plainly);
		Set->Serialize(Writer);
	}
	USuperSLMPrefixSet* ReadPlain = NewTextSet(nullptr);
	{
		FObjectReader Reader(Plainly);
		ReadPlain->Serialize(Reader);
	}
	const int32 PlainTextAt = IndexOf(*ReadPlain, TEXT("text"));
	bOk &= TestTrue(TEXT("must-reject: a plain save keeps the stale entry's ids"), PlainTextAt != INDEX_NONE && ReadPlain->Entries[PlainTextAt].Tokens == TextIds);
	bOk &= TestTrue(TEXT("must-reject: a plain save keeps the stale entry's bytes"), BytesOf(*ReadPlain, TEXT("text")) == TextBytes);
	bOk &= TestTrue(TEXT("after the plain save: the object is unchanged"), SameEntries(Set->Entries, EntriesBefore) && SameBuilds(Set->Builds, BuildsBefore));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
