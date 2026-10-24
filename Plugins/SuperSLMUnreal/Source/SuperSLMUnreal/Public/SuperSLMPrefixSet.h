#pragma once

// A cooked prefix set (precache plan §5.8): the model it is for, the entries a designer authors
// (by token ids or by text, rev 1.18), and, per producer (backend, and on the GPU the adapter's vendor and device), the
// built 'SLUP' wrapper bytes of each entry with the Layer-1 identity they were built under. The
// editor's Build Prefixes action and the SuperSLMBuildPrefixes commandlet fill Builds; the cook
// validates them (a warning naming each stale entry); a subsystem registers the set with its cache's
// cooked tier (USuperSLMSubsystem::RegisterPrefixSet). The rules are host-tested in
// Private/Core/SuperSLMPrefixSetCore.h; SuperSLMPrefixSetTools.h is the Unreal-side bridge.
//
// Entries defined by text (§5.8 *Entries defined by text*; PC-16 (b); cells 2.17, 2.18, 7.20, 9.12): the Input
// kind and Text, the ids and hashes the build stores beside the text, FindEntrySpec, the editor's change hooks and
// the cook's copy. The token ids stay the runtime key: the runtime never tokenizes.

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SuperSLMPrefixSet.generated.h"

class USuperSLMModel;
struct FSuperSLMPrefixSpec;

// How an entry is authored (§5.8, rev 1.18). Tokens is the default, and what every asset saved before this
// field existed loads as (cell 9.12).
UENUM(BlueprintType)
enum class ESuperSLMPrefixEntryInput : uint8
{
	Tokens,
	Text,
};

// One authored entry. ParentName names another entry of the set (a nested entry): the cache builds
// none until budget-mode nesting (plan step U6), so the build leaves it empty and says so.
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMPrefixSetEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix")
	FName Name;

	// Tokens: the ids are authored. Text: the build tokenizes Text against the set's model and stores the ids.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix")
	ESuperSLMPrefixEntryInput Input = ESuperSLMPrefixEntryInput::Tokens;

	// A Text entry's text, tokenized at build with nothing prepended or appended. A nested entry's text is its
	// suffix alone (tokenized alone, so a boundary a whole-text tokenize would merge is not merged).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix", meta = (MultiLine = "true", EditCondition = "Input == ESuperSLMPrefixEntryInput::Text", EditConditionHides))
	FString Text;

	// Token ids, as CreatePrefix() and RequestPrefix() take them: the runtime key. A Tokens entry's are authored;
	// a Text entry's are what its last successful build tokenized (empty until then, and after an edit to its
	// Text or the set's Model clears them).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix", meta = (EditCondition = "Input == ESuperSLMPrefixEntryInput::Tokens"))
	TArray<int32> Tokens;

	// A Text entry's provenance, stored by the build beside its ids: the SHA-256 of Text, and the model's
	// artifact hash, when the ids were tokenized (64 hex digits each; empty = none). A mismatch at cook is
	// TextChanged: the entry ships with no ids and no bytes.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prefix|Built")
	FString TextHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Prefix|Built")
	FString TokenizedModelHash;

	// None = no schema.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix")
	FName SchemaName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix")
	FName ParentName;
};

// One entry's built bytes: a whole 'SLUP' wrapper, or empty (left empty by the build).
USTRUCT()
struct SUPERSLMUNREAL_API FSuperSLMPrefixSetBuiltEntry
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Prefix")
	FName Name;

	UPROPERTY()
	TArray<uint8> Wrapper;
};

// One producer's build. A rebuild for the same producer replaces it.
USTRUCT()
struct SUPERSLMUNREAL_API FSuperSLMPrefixSetBuild
{
	GENERATED_BODY()

	// 0 CPU, 1 GPU.
	UPROPERTY(VisibleAnywhere, Category = "Build")
	uint8 Backend = 0;

	// The GPU adapter's PCI vendor and device ids (0 on the CPU).
	UPROPERTY(VisibleAnywhere, Category = "Build")
	int32 AdapterVendorId = 0;

	UPROPERTY(VisibleAnywhere, Category = "Build")
	int32 AdapterDeviceId = 0;

	UPROPERTY(VisibleAnywhere, Category = "Build")
	FString Layer1Tag;

	UPROPERTY(VisibleAnywhere, Category = "Build")
	FString Layer1Commit;

	// The model's artifact integrity hash, 64 hex digits.
	UPROPERTY(VisibleAnywhere, Category = "Build")
	FString ArtifactHash;

	UPROPERTY(VisibleAnywhere, Category = "Build")
	FDateTime BuiltAt;

	UPROPERTY(VisibleAnywhere, Category = "Build")
	TArray<FSuperSLMPrefixSetBuiltEntry> Entries;
};

UCLASS(BlueprintType)
class SUPERSLMUNREAL_API USuperSLMPrefixSet : public UDataAsset
{
	GENERATED_BODY()

public:
	// The model the entries are built against. A runtime configured with another model finds no
	// entry (the key carries the model's artifact hash) and recomputes.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix Set")
	TObjectPtr<USuperSLMModel> Model;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prefix Set")
	TArray<FSuperSLMPrefixSetEntry> Entries;

	// Filled by Build Prefixes / -run=SuperSLMBuildPrefixes; never edited by hand.
	UPROPERTY(VisibleAnywhere, Category = "Prefix Set|Built")
	TArray<FSuperSLMPrefixSetBuild> Builds;

	// How a game requests an entry (§5.8 *How a game requests it*, rev 1.18.3). ConfiguredModel is the
	// subsystem's GetConfiguredModel() (USuperSLMSubsystem and USuperSLMGpuSubsystem both have it). Out gets the
	// entry's stored ids and schema name for RequestPrefix or WarmPrefix; Out.Parent is left invalid, and
	// OutParentEntry names the parent entry (NAME_None for a root): the caller requests the parent itself and
	// sets Out.Parent to that lease. False, with nothing returned, for a set whose model is not ConfiguredModel
	// (PrefixSetModelMismatch, every entry), an unknown entry, an entry with no stored ids, and a Text entry
	// whose text or model changed since its build (TextChanged). Never tokenizes. Game thread.
	bool FindEntrySpec(const USuperSLMModel* ConfiguredModel, FName Entry, FSuperSLMPrefixSpec& Out, FName& OutParentEntry) const;

	// A cooked package carries every TextChanged entry with no ids and no bytes (SuperSLMPrefixSetTools::
	// CookedImage); the source asset is not modified.
	virtual void Serialize(FArchive& Ar) override;

#if WITH_EDITOR
	// An edit to an entry's Text or Input, or to Model, clears the affected Text entries' stored ids, hashes and
	// bytes (SuperSLMPrefixSetTools::OnEntryTextEdited, OnModelEdited).
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};
