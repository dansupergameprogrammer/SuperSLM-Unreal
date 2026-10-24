// Precache plan rev 1.17, the [U3] cells: cooked prefix sets (§5.8, §5.3 item 2, §5.9). Preprocessed out
// unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.Cooked.StaleSet             cell 2.9                                  [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.TwoNameRefused       cell 2.15, the editor build's refusal     [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.ThreeSchemaIndex2    cell 2.15, the cooked tier                [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.RebuiltAfterPin      cell 9.7                                  [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.NoEntryRecomputes    §5.8 per-producer entries, CPU now        [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.RecycledObjectId     1.3.0 review item 4: a set that reuses a collected
//                                                        set's object id leaves its registration alone
//
// The cook leg of 2.9 runs the cook hook's own function (SuperSLMPrefixSetEditor::ValidateAtCook,
// what the pre-save hook calls when the save is a cook) and asserts its warning lines; the box
// queue's one cook of the example project shows the same lines in a real cook log.
//
// A "previous pin" and "another model" are made with the build-identity seam
// (SuperSLMPrefixSetTools::FScopedBuildIdentityOverride): a set built while it is alive is stamped as
// built under that Layer-1 tag and commit, or against a flipped artifact hash, exactly as a set built
// before a re-pin or a model reimport would be. Nothing else about the build changes.
//
// Cell 2.15 needs PC-12's artifacts, built by the engine's own E2 tooling (2.13's two-name artifact
// and the three-schema twin with distinct hashes), in the directory SUPERSLM_PRECACHE_PC12_ARTIFACTS
// names: two_name.sslm and three_schema.sslm. Box queue: copy them there. The tests fail by name when
// the directory or a file is missing.
//
// The host twins of 2.9, 2.15 (cooked) and 9.7 are ci/host/prefix_cache/test_prefix_set.cpp.
// Mutants: cook validation skipped -> 2.9's warning legs red; stale bytes used (the cooked check
// skipped) -> 2.9's runtime legs red; rebuild appends rather than replaces -> 9.7 red (the stale CPU
// build is selected first); a set with no CPU build registering anything -> NoEntryRecomputes red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformMisc.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	constexpr int32 kEntryLength = 200;
	const TCHAR* const k1p13Tag = TEXT("v1.13.0");
	const TCHAR* const k1p13CandidateCommit = TEXT("002094a6da8ba33450e6fabc08503b22e35d1610");

	USuperSLMPrefixSet* NewSet(USuperSLMModel* Model)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		return Set;
	}

	FSuperSLMPrefixSetEntry SetEntry(const TCHAR* Name, const TArray<int32>& Tokens, FName Schema = NAME_None)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(Name);
		E.Tokens = Tokens;
		E.SchemaName = Schema;
		return E;
	}

	// Count token ids valid in Model's vocabulary, chosen without a tokenizer: a deterministic walk over
	// [1, VocabSize - 1], so two salts give different content of the same length. For a model with no tokenizer
	// section (PC-12's three-schema artifact, the engine's synthetic0p5-three-schema.sslm: 256-token vocabulary).
	bool VocabTokens(FAutomationTestBase& T, const USuperSLMModel& Model, int32 Count, int32 Salt, TArray<int32>& Out)
	{
		FSuperSLMModelShapeFacts Shape;
		if (!T.TestTrue(TEXT("the model's shape reads (CFG1)"), SuperSLMModelInspector::ReadShape(Model, Shape))
			|| !T.TestTrue(*FString::Printf(TEXT("the model has a vocabulary to draw from (%d)"), Shape.VocabSize), Shape.VocabSize > 2))
		{
			return false;
		}
		const int32 Span = Shape.VocabSize - 1;
		Out.Reset(Count);
		for (int32 I = 0; I < Count; ++I)
		{
			Out.Add(1 + (Salt * 131 + I * 7) % Span);
		}
		return true;
	}

	// A legacy-mode CPU subsystem with the cache on and room for several entries of kEntryLength, and one
	// 1.0.0 prefix block for the prefill twin (TwinTokens builds it with CreatePrefix). Box 2026-10-21: with
	// PrefixBlockCount 0 the twin's CreatePrefix was refused "PrefixBlockCount (0) prefixes are already live",
	// as 1.0.0's count rule says it must be, before the cell reached the cache (RebuiltAfterPin, StaleSet).
	bool CookedRig(FAutomationTestBase& T, FPagingRig& Rig)
	{
		if (!Rig.Init(T) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(T, Rig))
		{
			return false;
		}
		return ConfigureCacheLegacy(T, Rig, WithCache(MakeConfig(2, 0, 1, 1), BytesForPages(Rig.Geo, 64)));
	}

	bool Build(FAutomationTestBase& T, USuperSLMPrefixSet& Set, const TCHAR* What)
	{
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		return T.TestTrue(*FString::Printf(TEXT("%s: the set builds (%s)"), What, *Error), SuperSLMPrefixSetEditor::BuildAndLog(Set, Findings, Error));
	}

	int32 Count(const TArray<FSuperSLMPrefixSetFinding>& Findings, const TCHAR* Finding, const TCHAR* Entry)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			N += F.Finding == FName(Finding) && F.Entry == FName(Entry) ? 1 : 0;
		}
		return N;
	}

	// Requests S and expects it Ready from Source; returns the lease (returned by the caller).
	bool ServedFrom(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, ESuperSLMPrefixSource Source, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		return RequestReady(T, Cpu, S, Out, What) && ExpectSource(T, Cpu, Out, Source, What);
	}

	bool ResolvePc12Artifact(FAutomationTestBase& T, const TCHAR* File, FString& OutPath)
	{
		const FString Dir = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PRECACHE_PC12_ARTIFACTS"));
		OutPath = FPaths::Combine(Dir, File);
		return T.TestFalse(TEXT("SUPERSLM_PRECACHE_PC12_ARTIFACTS names PC-12's artifact directory"), Dir.IsEmpty())
			&& T.TestTrue(*FString::Printf(TEXT("%s exists"), *OutPath), FPaths::FileExists(OutPath));
	}
}

// Cell 2.9: a cooked set with a stale tag, a stale artifact hash, and a spec edited after the build:
// the cook warns naming exactly those entries; at runtime each is served by recompute (the stale
// bytes ship and are never used), and the fresh entry is a cooked hit whose adopter equals the twin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedStaleSetTest, "SuperSLM.PrefixCache.Cpu.Cooked.StaleSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedStaleSetTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Fresh, OldTag, OldModel, Edited, Suffix, Twin;
	if (!CookedRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 1, Fresh) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 2, OldTag)
		|| !MakeTokens(*this, *Rig.Cpu, kEntryLength, 3, OldModel) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 4, Edited)
		|| !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 5, Suffix) || !TwinTokens(*this, *Rig.Cpu, Fresh, Suffix, Twin))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewSet(Rig.Model);
	Set->Entries = {SetEntry(TEXT("fresh"), Fresh), SetEntry(TEXT("old_tag"), OldTag), SetEntry(TEXT("old_model"), OldModel), SetEntry(TEXT("edited"), Edited)};
	if (!Build(*this, *Set, TEXT("current pin")) || !TestEqual(TEXT("one CPU build"), Set->Builds.Num(), 1))
	{
		return false;
	}
	// old_tag's bytes as a set built before a re-pin left them; old_model's as before a reimport.
	auto BuiltElsewhere = [this, &Rig](const FSuperSLMPrefixSetEntry& E, bool bOldPin, TArray<uint8>& OutWrapper)
	{
		USuperSLMPrefixSet* Scratch = NewSet(Rig.Model);
		Scratch->Entries = {E};
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride Override(bOldPin ? FString(k1p13Tag) : FString(), bOldPin ? FString(k1p13CandidateCommit) : FString(), !bOldPin);
		if (!Build(*this, *Scratch, bOldPin ? TEXT("previous pin") : TEXT("previous model")) || Scratch->Builds.Num() != 1 || Scratch->Builds[0].Entries.Num() != 1)
		{
			return false;
		}
		OutWrapper = Scratch->Builds[0].Entries[0].Wrapper;
		return OutWrapper.Num() > 0;
	};
	// The override stamps the tag and commit it is given (empty keeps this pin's), so the two legs
	// differ from the current build in one field each.
	bool bOk = TestTrue(TEXT("old_tag built under v1.13.0"), BuiltElsewhere(Set->Entries[1], true, Set->Builds[0].Entries[1].Wrapper));
	bOk &= TestTrue(TEXT("old_model built against a flipped artifact hash"), BuiltElsewhere(Set->Entries[2], false, Set->Builds[0].Entries[2].Wrapper));
	// The edit: one token of "edited" changed after the build (0 and 1 are in every vocabulary).
	Set->Entries[3].Tokens[17] = Set->Entries[3].Tokens[17] == 0 ? 1 : 0;
	if (!bOk)
	{
		return false;
	}

	// The cook leg: exactly three warnings, each naming its entry and why.
	{
		FPagingLogCapture Log;
		const TArray<FSuperSLMPrefixSetFinding> Findings = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
		bOk &= TestEqual(TEXT("cook: three findings"), Findings.Num(), 3);
		bOk &= TestEqual(TEXT("cook: old_tag is StaleLayer1Tag"), Count(Findings, TEXT("StaleLayer1Tag"), TEXT("old_tag")), 1);
		bOk &= TestEqual(TEXT("cook: old_model is StaleArtifact"), Count(Findings, TEXT("StaleArtifact"), TEXT("old_model")), 1);
		bOk &= TestEqual(TEXT("cook: edited is SpecEdited"), Count(Findings, TEXT("SpecEdited"), TEXT("edited")), 1);
		bOk &= TestEqual(TEXT("cook: nothing names fresh"), Log.CountContaining(TEXT("entry 'fresh'")), 0);
		for (const TCHAR* Name : {TEXT("old_tag"), TEXT("old_model"), TEXT("edited")})
		{
			bOk &= TestEqual(*FString::Printf(TEXT("cook: one warning line names '%s' and the set"), Name),
				Log.CountContaining(*FString::Printf(TEXT("%s: entry '%s' [CPU]"), *Set->GetPathName(), Name)), 1);
		}
	}

	// The runtime leg: the stale bytes ship, and every stale entry recomputes.
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	Cpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("all four entries register (staleness is the runtime's to find)"), Stats(Cpu).CookedEntriesRegistered, 4);
	FSuperSLMPrefix L;
	const int64 BuildsBefore = Stats(Cpu).BuildsIssued;
	bOk &= ServedFrom(*this, Cpu, Spec(Fresh), ESuperSLMPrefixSource::Cooked, L, TEXT("fresh"));
	bOk &= TestEqual(TEXT("fresh: no prefix prefill ran"), Stats(Cpu).BuildsIssued, BuildsBefore);
	bOk &= EqualsTwin(*this, Cpu, L, Suffix, Twin, TEXT("fresh: cooked hit"));
	Cpu.ReturnPrefix(L);
	for (const FSuperSLMPrefixSetEntry& E : {Set->Entries[1], Set->Entries[2], Set->Entries[3]})
	{
		FSuperSLMPrefix S;
		bOk &= ServedFrom(*this, Cpu, Spec(E.Tokens), ESuperSLMPrefixSource::Recomputed, S, *E.Name.ToString());
		Cpu.ReturnPrefix(S);
	}
	bOk &= TestEqual(TEXT("one cooked hit, three recomputes"), Served(Stats(Cpu), ESuperSLMPrefixSource::Cooked), int64(1));
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Cell 2.15, the editor half: the prefix-set build refuses PC-12's two-name artifact, naming the
// model, and leaves the set as it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedTwoNameTest, "SuperSLM.PrefixCache.Cpu.Cooked.TwoNameRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedTwoNameTest::RunTest(const FString& Parameters)
{
	FString Path;
	if (!ResolvePc12Artifact(*this, TEXT("two_name.sslm"), Path))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(*FString::Printf(TEXT("the two-name artifact imports (PC-12 refuses it at map, not at import): %s"), *Diag.Message), Model))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewSet(Model);
	Set->Entries = {SetEntry(TEXT("a"), {1, 2, 3, 4})};
	TArray<FSuperSLMPrefixSetFinding> Findings;
	FString Error;
	bool bOk = TestFalse(TEXT("the build is refused"), SuperSLMPrefixSetTools::BuildCpu(*Set, Findings, Error));
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the model (%s)"), *Error), Error.Contains(Model->GetPathName()));
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal carries the map status (%s)"), *Error), Error.Contains(TEXT("DuplicateSchemaNameHash")) || Error.Contains(TEXT("refused")));
	bOk &= TestEqual(TEXT("the set is unchanged: no build"), Set->Builds.Num(), 0);
	return bOk;
}

// Cell 2.15, the cooked tier: on the three-schema model with distinct hashes, a spec bound to the
// index-2 schema is served from the cooked tier, and its adopter's tokens equal the recompute's.
// The row is about selecting the index-2 schema in a cooked set; tokenization is not under test. Box 2026-10-21
// (run at ab7f223c3): PC-12's three_schema.sslm is the engine's synthetic fixture (synthetic0p5-three-schema.sslm),
// which has no tokenizer section and a 256-token vocabulary, so MakeTokens' text could not tokenize. The cell
// now draws token ids from the model's own vocabulary (VocabTokens).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedThreeSchemaTest, "SuperSLM.PrefixCache.Cpu.Cooked.ThreeSchemaIndex2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedThreeSchemaTest::RunTest(const FString& Parameters)
{
	FString Path;
	if (!ResolvePc12Artifact(*this, TEXT("three_schema.sslm"), Path))
	{
		return false;
	}
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	Rig.Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	if (!TestNotNull(*FString::Printf(TEXT("the three-schema artifact imports (%s)"), *Diag.Message), Rig.Model)
		|| !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig)
		|| !ConfigureCacheLegacy(*this, Rig, WithCache(MakeConfig(2, 0, 1, 1), BytesForPages(Rig.Geo, 64)))) // a prefix block for the twin, as CookedRig
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FName Index2(TEXT("schema_two")); // the twin's third schema (e2 tooling's naming)
	TArray<int32> Tokens, Suffix;
	if (!VocabTokens(*this, *Rig.Model, 64, 1, Tokens) || !VocabTokens(*this, *Rig.Model, SuffixTokens, 2, Suffix))
	{
		return false;
	}
	// The recompute's operands first, before any set is registered.
	FSuperSLMPrefix Recomputed;
	TArray<int32> RecomputeTokens;
	if (!ServedFrom(*this, Cpu, Spec(Tokens, Index2), ESuperSLMPrefixSource::Recomputed, Recomputed, TEXT("recompute"))
		|| !AdopterTokens(*this, Cpu, Recomputed, Suffix, RecomputeTokens))
	{
		return false;
	}
	Cpu.ReturnPrefix(Recomputed);

	USuperSLMPrefixSet* Set = NewSet(Rig.Model);
	Set->Entries = {SetEntry(TEXT("index2"), Tokens, Index2)};
	if (!Build(*this, *Set, TEXT("three-schema")) || !ConfigureCacheLegacy(*this, Rig, Rig.Config)) // a cold memory tier
	{
		return false;
	}
	Cpu.RegisterPrefixSet(Set);
	FSuperSLMPrefix L;
	bool bOk = ServedFrom(*this, Cpu, Spec(Tokens, Index2), ESuperSLMPrefixSource::Cooked, L, TEXT("index-2 schema"));
	bOk &= EqualsTwin(*this, Cpu, L, Suffix, RecomputeTokens, TEXT("index-2 schema: the adopter equals the recompute's"));
	Cpu.ReturnPrefix(L);
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Cell 9.7: a set built under the previous pin is all recompute and warns at cook; rebuilt under this
// pin it validates clean and serves cooked hits again, with no prefix prefill.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedRebuiltTest, "SuperSLM.PrefixCache.Cpu.Cooked.RebuiltAfterPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedRebuiltTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> A, B, Suffix, TwinA;
	if (!CookedRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 1, A) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 2, B)
		|| !MakeTokens(*this, *Rig.Cpu, SuffixTokens, 3, Suffix) || !TwinTokens(*this, *Rig.Cpu, A, Suffix, TwinA))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMPrefixSet* Set = NewSet(Rig.Model);
	Set->Entries = {SetEntry(TEXT("a"), A), SetEntry(TEXT("b"), B)};
	{
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OldPin(k1p13Tag, k1p13CandidateCommit, false);
		if (!Build(*this, *Set, TEXT("previous pin")))
		{
			return false;
		}
	}
	const TArray<FSuperSLMPrefixSetFinding> Before = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
	bool bOk = TestEqual(TEXT("before: a is StaleLayer1Tag"), Count(Before, TEXT("StaleLayer1Tag"), TEXT("a")), 1);
	bOk &= TestEqual(TEXT("before: b is StaleLayer1Tag"), Count(Before, TEXT("StaleLayer1Tag"), TEXT("b")), 1);
	Cpu.RegisterPrefixSet(Set);
	for (const TArray<int32>& Tokens : {A, B})
	{
		FSuperSLMPrefix L;
		bOk &= ServedFrom(*this, Cpu, Spec(Tokens), ESuperSLMPrefixSource::Recomputed, L, TEXT("before the rebuild"));
		Cpu.ReturnPrefix(L);
	}

	// The rebuild replaces the CPU build; the set is registered again on a cold memory tier.
	if (!Build(*this, *Set, TEXT("this pin")) || !TestEqual(TEXT("the rebuild replaced the CPU build"), Set->Builds.Num(), 1)
		|| !ConfigureCacheLegacy(*this, Rig, Rig.Config))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("after: the set validates clean"), SuperSLMPrefixSetEditor::ValidateAtCook(*Set).Num(), 0);
	Cpu.RegisterPrefixSet(Set);
	const int64 BuildsBefore = Stats(Cpu).BuildsIssued;
	FSuperSLMPrefix LA, LB;
	bOk &= ServedFrom(*this, Cpu, Spec(A), ESuperSLMPrefixSource::Cooked, LA, TEXT("after: a"));
	bOk &= ServedFrom(*this, Cpu, Spec(B), ESuperSLMPrefixSource::Cooked, LB, TEXT("after: b"));
	bOk &= TestEqual(TEXT("after: no prefix prefill ran"), Stats(Cpu).BuildsIssued, BuildsBefore);
	bOk &= EqualsTwin(*this, Cpu, LA, Suffix, TwinA, TEXT("after: a"));
	Cpu.ReturnPrefix(LA);
	Cpu.ReturnPrefix(LB);
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// §5.8's per-producer rule on the CPU now (U7 states it for GPU devices): a set with no CPU build, and
// an entry its build left empty, register nothing; those requests recompute.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedNoEntryTest, "SuperSLM.PrefixCache.Cpu.Cooked.NoEntryRecomputes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedNoEntryTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> A, B;
	if (!CookedRig(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 1, A) || !MakeTokens(*this, *Rig.Cpu, kEntryLength, 2, B))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMPrefixSet* Set = NewSet(Rig.Model);
	Set->Entries = {SetEntry(TEXT("a"), A), SetEntry(TEXT("b"), B)};
	if (!Build(*this, *Set, TEXT("cpu")) || !TestEqual(TEXT("one build"), Set->Builds.Num(), 1))
	{
		return false;
	}

	// The same bytes stamped as one GPU device's build: no CPU build, nothing registers.
	USuperSLMPrefixSet* GpuOnly = NewSet(Rig.Model);
	GpuOnly->Entries = Set->Entries;
	GpuOnly->Builds = Set->Builds;
	GpuOnly->Builds[0].Backend = 1;
	GpuOnly->Builds[0].AdapterVendorId = 0x10DE;
	GpuOnly->Builds[0].AdapterDeviceId = 0x1E84;
	Cpu.RegisterPrefixSet(GpuOnly);
	bool bOk = TestEqual(TEXT("a set with no CPU build registers nothing"), Stats(Cpu).CookedEntriesRegistered, 0);
	FSuperSLMPrefix L;
	bOk &= ServedFrom(*this, Cpu, Spec(A), ESuperSLMPrefixSource::Recomputed, L, TEXT("no CPU build"));
	Cpu.ReturnPrefix(L);
	Cpu.UnregisterPrefixSet(GpuOnly);
	const TArray<FSuperSLMPrefixSetFinding> Cook = SuperSLMPrefixSetEditor::ValidateAtCook(*GpuOnly);
	bOk &= TestEqual(TEXT("cook: a and b NotBuilt for the CPU"), Count(Cook, TEXT("NotBuilt"), TEXT("a")) + Count(Cook, TEXT("NotBuilt"), TEXT("b")), 2);

	// An entry the build left empty: the other registers and hits; this one recomputes.
	if (!ConfigureCacheLegacy(*this, Rig, Rig.Config))
	{
		return false;
	}
	Set->Builds[0].Entries[1].Wrapper.Reset();
	Cpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("one entry registers"), Stats(Cpu).CookedEntriesRegistered, 1);
	FSuperSLMPrefix LA, LB;
	bOk &= ServedFrom(*this, Cpu, Spec(A), ESuperSLMPrefixSource::Cooked, LA, TEXT("a: built"));
	bOk &= ServedFrom(*this, Cpu, Spec(B), ESuperSLMPrefixSource::Recomputed, LB, TEXT("b: left empty"));
	Cpu.ReturnPrefix(LA);
	Cpu.ReturnPrefix(LB);
	Cpu.UnregisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("cook: b NotBuilt"), Count(SuperSLMPrefixSetEditor::ValidateAtCook(*Set), TEXT("NotBuilt"), TEXT("b")), 1);
	return bOk;
}

// 1.3.0 review item 4. Registrations were keyed by the set's object id, which Unreal hands to a new object once the
// old one is collected (level streaming does this): a new set then replaced the old set's registration, silently.
// They are keyed by the set's path name. Set A registers and is collected; set B gets A's object id and registers;
// A's entry stays registered and is served Cooked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCookedRecycledIdTest, "SuperSLM.PrefixCache.Cpu.Cooked.RecycledObjectId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCookedRecycledIdTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!CookedRig(*this, Rig)) { return false; }
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> TokensA, TokensB;
	if (!MakeTokens(*this, Cpu, kEntryLength, 41, TokensA) || !MakeTokens(*this, Cpu, kEntryLength, 42, TokensB)) { return false; }
	uint32 OldId = 0;
	{
		USuperSLMPrefixSet* A = NewSet(Rig.Model);
		A->Entries = {SetEntry(TEXT("a"), TokensA)};
		if (!Build(*this, *A, TEXT("set A"))) { return false; }
		Cpu.RegisterPrefixSet(A);
		OldId = A->GetUniqueID();
		A->MarkAsGarbage();
	}
	const int32 Registered = Stats(Cpu).CookedEntriesRegistered;
	if (!TestEqual(TEXT("set A's entry registers"), Registered, 1)) { return false; }
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	// A new set that takes A's freed object id. The ones that do not are kept alive, so the freed id is not handed
	// to something else in between.
	USuperSLMPrefixSet* B = nullptr;
	TArray<TStrongObjectPtr<USuperSLMPrefixSet>> Others;
	for (int32 I = 0; I < 4096 && B == nullptr; ++I)
	{
		USuperSLMPrefixSet* Candidate = NewSet(Rig.Model);
		if (Candidate->GetUniqueID() == OldId) { B = Candidate; }
		else { Others.Emplace(Candidate); }
	}
	if (!TestNotNull(*FString::Printf(TEXT("precondition: a new prefix set reuses the collected set's object id %u"), OldId), B)) { return false; }
	TStrongObjectPtr<USuperSLMPrefixSet> KeepB(B);
	B->Entries = {SetEntry(TEXT("b"), TokensB)};
	if (!Build(*this, *B, TEXT("set B"))) { return false; }
	Cpu.RegisterPrefixSet(B);
	bool bOk = TestEqual(TEXT("set A's entry stays registered when set B reuses A's object id"), Stats(Cpu).CookedEntriesRegistered, Registered + 1);
	FSuperSLMPrefix L;
	bOk &= ServedFrom(*this, Cpu, Spec(TokensA), ESuperSLMPrefixSource::Cooked, L, TEXT("set A's entry, after B registered"));
	Cpu.ReturnPrefix(L);
	bOk &= ServedFrom(*this, Cpu, Spec(TokensB), ESuperSLMPrefixSource::Cooked, L, TEXT("set B's entry"));
	Cpu.ReturnPrefix(L);
	Cpu.UnregisterPrefixSet(B);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
