// See SuperSLMPrefixSetTools.h.

#include "SuperSLMPrefixSetTools.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Core/SuperSLMPrefixSetCore.h"
#include "SuperSLMPrefixSetBridge.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSaveBlob.h"
#include "superslm/sslm_abi.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Crc.h"

#include <atomic>

using namespace SuperSLMCore;

namespace
{
	std::string NameUtf8(FName Name)
	{
		// None is "no name", exactly as RequestPrefix() reads a spec's SchemaName.
		return Name.IsNone() ? std::string() : std::string(TCHAR_TO_UTF8(*Name.ToString()));
	}

	FString HashHex(const std::array<uint8_t, 32>& Hash)
	{
		FPrefixKey AsKey;
		AsKey.Bytes = Hash;
		return UTF8_TO_TCHAR(PrefixKeyToHex(AsKey).c_str());
	}

	bool HashFromHex(const FString& Hex, std::array<uint8_t, 32>& Out)
	{
		FPrefixKey AsKey;
		if (!PrefixKeyFromHex(std::string(TCHAR_TO_UTF8(*Hex)), AsKey))
		{
			Out.fill(0);
			return false;
		}
		Out = AsKey.Bytes;
		return true;
	}

	std::vector<FPrefixSetEntry> CoreEntries(const USuperSLMPrefixSet& Set)
	{
		return SuperSLMPrefixSetBridge::CoreEntries(Set);
	}

	// The model's artifact hash; false (and zeros) for no model or an unreadable header.
	bool ModelHashOf(const USuperSLMModel* Model, std::array<uint8_t, 32>& Out)
	{
		Out.fill(0);
		uint8 Hash[32] = {};
		if (Model == nullptr || !SuperSLMRuntime::ReadArtifactHash(*Model, Hash))
		{
			return false;
		}
		FMemory::Memcpy(Out.data(), Hash, 32);
		return true;
	}

	FPrefixSetBuild CoreBuild(const FSuperSLMPrefixSetBuild& B)
	{
		FPrefixSetBuild C;
		C.Producer.Backend = B.Backend;
		C.Producer.AdapterVendorId = static_cast<uint32_t>(B.AdapterVendorId);
		C.Producer.AdapterDeviceId = static_cast<uint32_t>(B.AdapterDeviceId);
		C.Producer.Layer1Tag = TCHAR_TO_UTF8(*B.Layer1Tag);
		C.Producer.Layer1Commit = TCHAR_TO_UTF8(*B.Layer1Commit);
		HashFromHex(B.ArtifactHash, C.Producer.ArtifactHash); // an unreadable hash reads as zeros: every entry StaleArtifact
		for (const FSuperSLMPrefixSetBuiltEntry& E : B.Entries)
		{
			FPrefixSetBuiltEntry CE;
			CE.Name = NameUtf8(E.Name);
			CE.Wrapper.assign(E.Wrapper.GetData(), E.Wrapper.GetData() + E.Wrapper.Num());
			C.Entries.push_back(MoveTemp(CE));
		}
		return C;
	}

	std::vector<FPrefixSetBuild> CoreBuilds(const USuperSLMPrefixSet& Set)
	{
		return SuperSLMPrefixSetBridge::CoreBuilds(Set);
	}

	[[maybe_unused]] FSuperSLMPrefixSetBuild UnrealBuild(const FPrefixSetBuild& C, const FDateTime& BuiltAt)
	{
		FSuperSLMPrefixSetBuild B;
		B.Backend = C.Producer.Backend;
		B.AdapterVendorId = static_cast<int32>(C.Producer.AdapterVendorId);
		B.AdapterDeviceId = static_cast<int32>(C.Producer.AdapterDeviceId);
		B.Layer1Tag = UTF8_TO_TCHAR(C.Producer.Layer1Tag.c_str());
		B.Layer1Commit = UTF8_TO_TCHAR(C.Producer.Layer1Commit.c_str());
		B.ArtifactHash = HashHex(C.Producer.ArtifactHash);
		B.BuiltAt = BuiltAt;
		for (const FPrefixSetBuiltEntry& E : C.Entries)
		{
			FSuperSLMPrefixSetBuiltEntry U;
			U.Name = E.Name.empty() ? NAME_None : FName(UTF8_TO_TCHAR(E.Name.c_str()));
			U.Wrapper = TArray<uint8>(E.Wrapper.data(), static_cast<int32>(E.Wrapper.size()));
			B.Entries.Add(MoveTemp(U));
		}
		return B;
	}

	FSuperSLMPrefixSetFinding UnrealFinding(const FString& SetPath, const FPrefixSetFindingLine& L)
	{
		FSuperSLMPrefixSetFinding F;
		F.Finding = FName(UTF8_TO_TCHAR(PrefixSetFindingName(L.Finding)));
		F.Entry = L.EntryName.empty() ? NAME_None : FName(UTF8_TO_TCHAR(L.EntryName.c_str()));
		F.bHasProducer = L.bHasProducer;
		F.Backend = L.Backend;
		F.Line = UTF8_TO_TCHAR(FormatPrefixSetFinding(std::string(TCHAR_TO_UTF8(*SetPath)), L).c_str());
		return F;
	}

	[[maybe_unused]] FSuperSLMPrefixSetFinding OwnFinding(const FString& SetPath, const TCHAR* Finding, FName Entry, const FString& Detail)
	{
		FSuperSLMPrefixSetFinding F;
		F.Finding = FName(Finding);
		F.Entry = Entry;
		F.bHasProducer = true;
		F.Backend = 0;
		F.Line = Entry.IsNone()
			? FString::Printf(TEXT("%s: %s (%s)"), *SetPath, Finding, *Detail)
			: FString::Printf(TEXT("%s: entry '%s' [CPU]: %s (%s)"), *SetPath, *Entry.ToString(), Finding, *Detail);
		return F;
	}

	// The identity the cook ships with, and the model's shape.
	bool CurrentIdentity(const USuperSLMPrefixSet& Set, FPrefixEngineIdentity& OutIdentity, FPrefixSetModelShape& OutShape, FString& OutError)
	{
		if (Set.Model == nullptr)
		{
			OutError = FString::Printf(TEXT("%s: the prefix set names no model"), *Set.GetPathName());
			return false;
		}
		uint8 Hash[32] = {};
		if (!SuperSLMRuntime::ReadArtifactHash(*Set.Model, Hash))
		{
			OutError = FString::Printf(TEXT("%s: the model %s carries no readable artifact header"), *Set.GetPathName(), *Set.Model->GetPathName());
			return false;
		}
		SuperSLMRuntime::FModelShape Shape;
		FString ShapeError;
		if (!SuperSLMRuntime::ReadModelShape(*Set.Model, Shape, ShapeError))
		{
			OutError = FString::Printf(TEXT("%s: the model %s's shape is unreadable (%s)"), *Set.GetPathName(), *Set.Model->GetPathName(), *ShapeError);
			return false;
		}
		FMemory::Memcpy(OutIdentity.ArtifactHash.data(), Hash, 32);
		OutIdentity.Layer1Tag = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Tag());
		OutIdentity.Layer1Commit = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Commit());
		OutIdentity.Backend = 0;
		OutShape.ContextCap = Shape.ContextCap;
		OutShape.VocabSize = Shape.VocabSize;
		return true;
	}

#if WITH_DEV_AUTOMATION_TESTS
	struct FBuildOverride
	{
		FString Tag;
		FString Commit;
		bool bFlipHash = false;
	};
	TOptional<FBuildOverride> GBuildOverride;
	std::atomic<int64> GGameThreadBuildCalls{0};
#endif
}

std::vector<FPrefixSetEntry> SuperSLMPrefixSetBridge::CoreEntries(const USuperSLMPrefixSet& Set)
{
	std::vector<FPrefixSetEntry> Out;
	Out.reserve(Set.Entries.Num());
	for (const FSuperSLMPrefixSetEntry& E : Set.Entries)
	{
		FPrefixSetEntry C;
		C.Name = NameUtf8(E.Name);
		C.Tokens.assign(E.Tokens.GetData(), E.Tokens.GetData() + E.Tokens.Num());
		C.SchemaName = NameUtf8(E.SchemaName);
		C.ParentName = NameUtf8(E.ParentName);
		C.Input = E.Input == ESuperSLMPrefixEntryInput::Text ? EPrefixSetEntryInput::Text : EPrefixSetEntryInput::Tokens;
		C.Text = TCHAR_TO_UTF8(*E.Text);
		HashFromHex(E.TextHash, C.TextHash);                     // empty or unreadable reads as zeros: none
		HashFromHex(E.TokenizedModelHash, C.TokenizedModelHash);
		Out.push_back(MoveTemp(C));
	}
	return Out;
}

bool SuperSLMPrefixSetBridge::SetModelHash(const USuperSLMPrefixSet& Set, std::array<uint8_t, 32>& Out)
{
	return ModelHashOf(Set.Model, Out);
}

std::vector<FPrefixSetBuild> SuperSLMPrefixSetBridge::CoreBuilds(const USuperSLMPrefixSet& Set)
{
	std::vector<FPrefixSetBuild> Out;
	for (const FSuperSLMPrefixSetBuild& B : Set.Builds)
	{
		Out.push_back(CoreBuild(B));
	}
	return Out;
}

bool SuperSLMPrefixSetTools::ValidateForCook(const USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	OutFindings.Reset();
	FPrefixEngineIdentity Current;
	FPrefixSetModelShape Shape;
	if (!CurrentIdentity(Set, Current, Shape, OutError))
	{
		return false;
	}
	const FString Path = Set.GetPathName();
	for (const FPrefixSetFindingLine& L : ValidatePrefixSetForCook(CoreEntries(Set), CoreBuilds(Set), Current, Shape))
	{
		OutFindings.Add(UnrealFinding(Path, L));
	}
	return true;
}

bool SuperSLMPrefixSetTools::FindEntrySpec(const USuperSLMPrefixSet& Set, const USuperSLMModel* ConfiguredModel, FName Entry,
	FSuperSLMPrefixSpec& Out, FName& OutParentEntry, FName& OutReason)
{
	Out = FSuperSLMPrefixSpec();
	OutParentEntry = NAME_None;
	OutReason = NAME_None;
	// The set's model against the caller's (§5.8, rev 1.18.2): by artifact hash, for the whole set, before any entry
	// is looked at. A missing or unreadable model on either side is a mismatch (never zeros equal to zeros).
	std::array<uint8_t, 32> SetHash{};
	std::array<uint8_t, 32> ConfiguredHash{};
	const bool bSetHash = ModelHashOf(Set.Model, SetHash);
	const bool bConfiguredHash = ModelHashOf(ConfiguredModel, ConfiguredHash);
	std::vector<int32_t> Tokens;
	std::string Schema;
	std::string Parent;
	const EPrefixSetSpecResult Result = bSetHash && bConfiguredHash
		? FindPrefixSetEntrySpec(CoreEntries(Set), SetHash, ConfiguredHash, NameUtf8(Entry), Tokens, Schema, Parent)
		: EPrefixSetSpecResult::PrefixSetModelMismatch;
	if (Result != EPrefixSetSpecResult::Ok)
	{
		OutReason = FName(UTF8_TO_TCHAR(PrefixSetSpecResultName(Result)));
		return false;
	}
	Out.Tokens = TArray<int32>(Tokens.data(), static_cast<int32>(Tokens.size()));
	Out.SchemaName = Schema.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Schema.c_str()));
	OutParentEntry = Parent.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Parent.c_str()));
	return true;
}

#if WITH_EDITOR

int32 SuperSLMPrefixSetTools::CookedImage(const USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetEntry>& OutEntries, TArray<FSuperSLMPrefixSetBuild>& OutBuilds)
{
	OutEntries = Set.Entries;
	OutBuilds = Set.Builds;
	// An unreadable model reads as zeros, which no built Text entry's stored model hash is: every Text entry ships empty.
	std::array<uint8_t, 32> ModelHash{};
	ModelHashOf(Set.Model, ModelHash);
	const std::vector<size_t> Stale = PrefixSetStaleTextEntries(CoreEntries(Set), ModelHash);
	// StripPrefixSetEntriesForCook's rule (host-tested), on the asset's own types.
	for (const size_t I : Stale)
	{
		FSuperSLMPrefixSetEntry& E = OutEntries[static_cast<int32>(I)];
		E.Tokens.Reset();
		for (FSuperSLMPrefixSetBuild& B : OutBuilds)
		{
			for (FSuperSLMPrefixSetBuiltEntry& Built : B.Entries)
			{
				if (Built.Name == E.Name)
				{
					Built.Wrapper.Reset();
				}
			}
		}
	}
	return static_cast<int32>(Stale.size());
}

namespace
{
	// §5.8 *Staleness*: an edit clears a Text entry's stored ids, its hashes, and its bytes in every build.
	bool ClearTextEntry(USuperSLMPrefixSet& Set, int32 Index)
	{
		FSuperSLMPrefixSetEntry& E = Set.Entries[Index];
		bool bChanged = !E.TextHash.IsEmpty() || !E.TokenizedModelHash.IsEmpty();
		E.TextHash.Reset();
		E.TokenizedModelHash.Reset();
		if (E.Input != ESuperSLMPrefixEntryInput::Text)
		{
			return bChanged; // a Tokens entry's ids are authored, and its bytes stay keyed by them
		}
		bChanged |= E.Tokens.Num() > 0;
		E.Tokens.Reset();
		for (FSuperSLMPrefixSetBuild& B : Set.Builds)
		{
			for (FSuperSLMPrefixSetBuiltEntry& Built : B.Entries)
			{
				if (Built.Name == E.Name)
				{
					bChanged |= Built.Wrapper.Num() > 0;
					Built.Wrapper.Reset();
				}
			}
		}
		return bChanged;
	}
}

void SuperSLMPrefixSetTools::OnEntryTextEdited(USuperSLMPrefixSet& Set, int32 Index)
{
	if (Set.Entries.IsValidIndex(Index) && ClearTextEntry(Set, Index))
	{
		Set.MarkPackageDirty();
	}
}

void SuperSLMPrefixSetTools::OnModelEdited(USuperSLMPrefixSet& Set)
{
	bool bChanged = false;
	for (int32 I = 0; I < Set.Entries.Num(); ++I)
	{
		if (Set.Entries[I].Input == ESuperSLMPrefixEntryInput::Text)
		{
			bChanged |= ClearTextEntry(Set, I);
		}
	}
	if (bChanged)
	{
		Set.MarkPackageDirty();
	}
}

#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
namespace
{
	// One Layer-1 context for the whole build: the model's shared mapping, a page pool sized for the
	// largest entry's peak R(L), and one workspace. Released in reverse order.
	struct FBuildContext
	{
		sslm_model Model = nullptr;
		void* KvBuffer = nullptr;
		sslm_kv_pool Pool = nullptr;
		void* WorkspaceBuffer = nullptr;
		sslm_workspace Workspace = nullptr;
		int32 ChunkBudget = 64;

		~FBuildContext()
		{
			if (Workspace != nullptr)
			{
				sslm_workspace_destroy(Workspace);
			}
			FMemory::Free(WorkspaceBuffer);
			if (Pool != nullptr && sslm_kv_pool_destroy(Pool) != SSLM_OK)
			{
				KvBuffer = nullptr; // a live handle: leak rather than free under it (as the subsystem does)
			}
			FMemory::Free(KvBuffer);
			if (Model != nullptr)
			{
				SuperSLMRuntime::ReleaseModelMapping(Model);
			}
		}
	};

	// One root entry: begin_budgeted(L), schema bind, prefill, freeze, save, release. Empty on failure,
	// with OutFinding/OutDetail naming why.
	// Cell 3.11's counter: prefill, freeze and save calls made on the game thread (the background action makes none).
	void CountIfGameThread()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (IsInGameThread())
		{
			GGameThreadBuildCalls.fetch_add(1);
		}
#endif
	}

	TArray<uint8> BuildOneEntry(FBuildContext& C, const FPrefixSetEntry& E, const TCHAR*& OutFinding, FString& OutDetail)
	{
		TArray<uint8> Payload;
		const int32 Length = static_cast<int32>(E.Tokens.size());
		sslm_prefix Prefix = nullptr;
		sslm_status Status = sslm_prefix_begin_budgeted(C.Model, &C.Pool, Length, &Prefix);
		if (Status == SSLM_OK && !E.SchemaName.empty())
		{
			sslm_schema Bound = SSLM_SCHEMA_NONE;
			Status = sslm_schema_lookup(C.Model, E.SchemaName.c_str(), &Bound);
			if (Status == SSLM_SCHEMA_NOT_FOUND)
			{
				OutFinding = TEXT("UnknownSchema");
			}
			if (Status == SSLM_OK)
			{
				Status = sslm_prefix_set_schema(Prefix, Bound);
			}
		}
		int32 Done = 0;
		while (Status == SSLM_OK && Done < Length)
		{
			const int32 Left = Length - Done;
			int32 Consumed = 0;
			CountIfGameThread();
			Status = sslm_prefix_prefill(C.Model, Prefix, E.Tokens.data() + Done, Left, FMath::Min(Left, C.ChunkBudget), SSLM_SPAN_PROMPT, C.Workspace, &Consumed);
			Done += Consumed;
			if (Status == SSLM_OK && Consumed <= 0)
			{
				Status = SSLM_INVALID_ARGUMENT; // no progress: never loop
			}
		}
		if (Status == SSLM_OK)
		{
			CountIfGameThread();
			Status = sslm_prefix_freeze(Prefix);
		}
		if (Status == SSLM_OK)
		{
			size_t Size = 0;
			CountIfGameThread();
			Status = sslm_prefix_save(Prefix, nullptr, &Size);
			if (Status == SSLM_BUFFER_TOO_SMALL)
			{
				Payload.SetNumUninitialized(static_cast<int32>(Size));
				CountIfGameThread();
				Status = sslm_prefix_save(Prefix, Payload.GetData(), &Size);
				Payload.SetNum(static_cast<int32>(Size));
			}
		}
		if (Prefix != nullptr)
		{
			sslm_prefix_release(Prefix);
		}
		if (Status != SSLM_OK)
		{
			Payload.Reset();
			if (OutFinding == nullptr)
			{
				OutFinding = TEXT("BuildFailed");
			}
			OutDetail = FString::Printf(TEXT("sslm status %d; left empty, the runtime recomputes it"), static_cast<int32>(Status));
		}
		return Payload;
	}
}
#endif // SSLM_HAS_PREFIX_SAVE_ABI

namespace
{
	// What a build's result is checked against at its finish: the authored entries and the model, as they were
	// when the build began. A set edited while its background build ran stores nothing (§5.8: one replacement,
	// of the entries the build was made from).
	uint32 EntriesFingerprint(const USuperSLMPrefixSet& Set)
	{
		FString Blob = Set.Model != nullptr ? Set.Model->GetPathName() : FString();
		for (const FSuperSLMPrefixSetEntry& E : Set.Entries)
		{
			Blob += FString::Printf(TEXT("|%s|%s|%s|%d|%s|%s|%s|"), *E.Name.ToString(), *E.SchemaName.ToString(), *E.ParentName.ToString(),
				static_cast<int32>(E.Input), *E.Text, *E.TextHash, *E.TokenizedModelHash);
			for (const int32 Id : E.Tokens)
			{
				Blob.AppendInt(Id);
				Blob.AppendChar(TEXT(','));
			}
		}
		return FCrc::StrCrc32(*Blob);
	}
}

// One CPU build in three phases (§5.8 *The build runs in the background*, PC-15 Q9; cell 3.11): Begin on the game
// thread (identity, the tokenize pass, the manifest, the model's mapping, the pool and workspace), the entries on
// any one thread (prefill, freeze and save per entry; cancellable between entries), and Finish on the game thread
// (one replacement of the set's CPU build, or nothing on a cancel).
struct SuperSLMPrefixSetTools::FPrefixSetBuildJob
{
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
	TUniquePtr<FBuildContext> Context; // created and released on the game thread; used by the entries' thread between
#endif
	FString Path;
	FPrefixEngineIdentity Producer;
	FPrefixSetTokenizePass Tokenized;
	std::vector<bool> Buildable;
	std::vector<bool> Kept;                    // the entries' thread sets its own failures; read at Finish
	std::vector<std::vector<uint8_t>> Payloads;
	TArray<FName> EntryNames;
	TArray<FSuperSLMPrefixSetFinding> BeginFindings;
	TArray<FSuperSLMPrefixSetFinding> EntryFindings; // the entries' thread's; read at Finish, after bWorkDone
	uint32 Fingerprint = 0;
	int32 EntriesTotal = 0;
	std::atomic<int32> EntriesDone{0};
	std::atomic<bool> bCancel{false};
	std::atomic<bool> bWorkDone{false};
	std::atomic<bool> bPaused{false};
	std::atomic<bool> bResume{false};
	int32 PauseAfterEntries = -1;              // test seam (cell 3.11's cancel leg); -1 = none
	bool bFinished = false;
};

TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> SuperSLMPrefixSetTools::BeginBuildCpu(USuperSLMPrefixSet& Set, FString& OutError)
{
	check(IsInGameThread());
	TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> JobPtr = MakeShared<FPrefixSetBuildJob, ESPMode::ThreadSafe>();
	FPrefixSetBuildJob& Job = *JobPtr;
	Job.Path = Set.GetPathName();
	const FString& Path = Job.Path;
	FPrefixEngineIdentity& Producer = Job.Producer;
	FPrefixSetModelShape Shape;
	if (!CurrentIdentity(Set, Producer, Shape, OutError))
	{
		return nullptr;
	}
#if !defined(SSLM_HAS_PREFIX_SAVE_ABI)
	OutError = FString::Printf(TEXT("%s: this build's Layer 1 (%s) has no prefix save; cooked prefix sets need 1.14.0"), *Path, *SuperSLMSaveBlob::CompiledLayer1Tag());
	return nullptr;
#else
	// The tokenize pass (§5.8 *Tokenized at build*): here, on the game thread (the registry is game-thread API),
	// before the model is mapped for the build and before any prefill. Each Text entry's own text, nothing
	// prepended (a nested entry's suffix alone), against the set's model. A failing entry keeps its previous ids
	// and bytes and is reported by name; the others build.
	Job.Tokenized = TokenizePrefixSetText(CoreEntries(Set), Shape, Producer.ArtifactHash,
		[&Set](const std::string& Text, std::vector<int32_t>& OutIds, std::string& OutTokenizeError)
		{
			TArray<int32> Ids;
			FString Error;
			const bool bOk = SuperSLM::TokenizeText(*Set.Model, FString(UTF8_TO_TCHAR(Text.c_str())), Ids, Error);
			OutIds.assign(Ids.GetData(), Ids.GetData() + Ids.Num());
			OutTokenizeError = TCHAR_TO_UTF8(*Error);
			return bOk;
		});
	const FPrefixSetTokenizePass& Tokenized = Job.Tokenized;
	for (const FPrefixSetFindingLine& L : Tokenized.Lines)
	{
		Job.BeginFindings.Add(UnrealFinding(Path, L));
	}
	const std::vector<FPrefixSetEntry>& Entries = Tokenized.Entries;
	std::vector<bool>& Buildable = Job.Buildable;
	for (const FPrefixSetFindingLine& L : CheckPrefixSetManifest(Entries, Shape, &Buildable))
	{
		bool bFailedEntry = false; // reported once, by the tokenize pass
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			bFailedEntry |= Tokenized.Failed[I] && Entries[I].Name == L.EntryName;
		}
		if (!bFailedEntry)
		{
			Job.BeginFindings.Add(UnrealFinding(Path, L));
		}
	}
	// Kept[i]: entry i failed (at the tokenize pass, or below in its prefill, freeze or save) and keeps its bytes.
	Job.Kept = Tokenized.Failed;
	std::vector<bool>& Kept = Job.Kept;
	for (size_t I = 0; I < Entries.size(); ++I)
	{
		Buildable[I] = Buildable[I] && !Kept[I];
	}

	// The model is mapped here; a model the engine refuses at map (PC-12's two-name artifact, cell
	// 2.15) refuses the whole build by name, and the set is left as it was.
	Job.Context = MakeUnique<FBuildContext>();
	FBuildContext& C = *Job.Context;
	FString MapError;
	C.Model = SuperSLMRuntime::AcquireModelMapping(*Set.Model, MapError);
	if (C.Model == nullptr)
	{
		OutError = FString::Printf(TEXT("%s: the model %s is refused (%s); nothing was built"), *Path, *Set.Model->GetPathName(), *MapError);
		return nullptr;
	}

	// A page pool for the largest buildable entry's peak R(L) = min(ceil(L/B) + 1, ceil(cap/B)).
	const int64 B = sslm_kv_page_positions(C.Model);
	const size_t PageBytes = sslm_kv_page_size(C.Model);
	int64 PeakPages = 0;
	for (size_t I = 0; I < Entries.size(); ++I)
	{
		if (Buildable[I])
		{
			const int64 L = static_cast<int64>(Entries[I].Tokens.size());
			PeakPages = FMath::Max(PeakPages, FMath::Min((L + B - 1) / B + 1, (Shape.ContextCap + B - 1) / B));
		}
	}
	if (PeakPages > 0)
	{
		if (B <= 0 || PageBytes == 0)
		{
			OutError = FString::Printf(TEXT("%s: Layer 1 reports no page geometry for %s"), *Path, *Set.Model->GetPathName());
			return nullptr;
		}
		const uint32 Pages = static_cast<uint32>(PeakPages);
		const size_t KvBytes = PageBytes * Pages + sslm_kv_page_pool_overhead_size(C.Model, Pages);
		C.KvBuffer = FMemory::Malloc(KvBytes, SSLM_ABI_ALIGNMENT_BYTES);
		sslm_status Status = C.KvBuffer != nullptr ? sslm_kv_page_pool_create(C.Model, C.KvBuffer, KvBytes, Pages, &C.Pool) : SSLM_ALLOCATION_FAILED;
		// One prefill sequence at a time, every layer per call, 64-token chunks (the cache's own builds
		// chunk at the lane's MaxPrefillChunk the same way).
		SuperSLMRuntime::FModelShape Layers;
		FString LayersError;
		SuperSLMRuntime::ReadModelShape(*Set.Model, Layers, LayersError);
		sslm_config Shape1;
		Shape1.max_batch = 1;
		Shape1.max_chunk_budget = 64;
		Shape1.max_layer_budget = FMath::Max(Layers.NumHiddenLayers, 1);
		Shape1.reserved = 0;
		C.ChunkBudget = Shape1.max_chunk_budget;
		const size_t WorkspaceBytes = Status == SSLM_OK ? sslm_workspace_size(C.Model, &Shape1) : 0;
		if (Status == SSLM_OK && WorkspaceBytes != 0)
		{
			C.WorkspaceBuffer = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
			Status = C.WorkspaceBuffer != nullptr ? sslm_workspace_create(C.Model, &Shape1, C.WorkspaceBuffer, WorkspaceBytes, &C.Workspace) : SSLM_ALLOCATION_FAILED;
		}
		if (Status != SSLM_OK || C.Workspace == nullptr)
		{
			OutError = FString::Printf(TEXT("%s: could not create the build's pool or workspace for %s (sslm status %d)"), *Path, *Set.Model->GetPathName(), static_cast<int32>(Status));
			return nullptr;
		}
	}

	Job.Payloads.assign(Entries.size(), std::vector<uint8_t>());
	for (const FSuperSLMPrefixSetEntry& E : Set.Entries)
	{
		Job.EntryNames.Add(E.Name);
	}
	for (size_t I = 0; I < Entries.size(); ++I)
	{
		Job.EntriesTotal += Buildable[I] ? 1 : 0;
	}
	Job.Fingerprint = EntriesFingerprint(Set);
	return JobPtr;
#endif
}

void SuperSLMPrefixSetTools::RunBuildEntries(FPrefixSetBuildJob& Job)
{
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
	const std::vector<FPrefixSetEntry>& Entries = Job.Tokenized.Entries;
	for (size_t I = 0; I < Entries.size() && Job.Context.IsValid(); ++I)
	{
		if (!Job.Buildable[I])
		{
			continue;
		}
		// The test pause (cell 3.11), one-shot: it holds once, when EntriesDone reaches PauseAfterEntries, until a resume
		// or a cancel. bResume is never cleared, and EntriesDone only grows, so it never holds a second time.
		if (Job.PauseAfterEntries >= 0 && Job.EntriesDone.load() == Job.PauseAfterEntries && !Job.bCancel.load())
		{
			Job.bPaused.store(true);
			while (!Job.bCancel.load() && !Job.bResume.load())
			{
				FPlatformProcess::Sleep(0.001f);
			}
			Job.bPaused.store(false);
		}
		if (Job.bCancel.load())
		{
			break; // between entries: the entry in hand finishes, no later one starts
		}
		const TCHAR* Finding = nullptr;
		FString Detail;
		const TArray<uint8> Payload = BuildOneEntry(*Job.Context, Entries[I], Finding, Detail);
		if (Payload.Num() == 0)
		{
			Job.EntryFindings.Add(OwnFinding(Job.Path, Finding, Job.EntryNames[static_cast<int32>(I)], Detail + TEXT("; it keeps its previous bytes")));
			Job.Kept[I] = true;
		}
		else
		{
			Job.Payloads[I].assign(Payload.GetData(), Payload.GetData() + Payload.Num());
		}
		Job.EntriesDone.fetch_add(1);
	}
#endif
	Job.bWorkDone.store(true);
}

void SuperSLMPrefixSetTools::CancelBuild(FPrefixSetBuildJob& Job)
{
	Job.bCancel.store(true);
}

SuperSLMPrefixSetTools::FBuildProgress SuperSLMPrefixSetTools::GetBuildProgress(const FPrefixSetBuildJob& Job)
{
	FBuildProgress P;
	P.EntriesTotal = Job.EntriesTotal;
	P.EntriesDone = Job.EntriesDone.load();
	P.bCancelRequested = Job.bCancel.load();
	P.bPaused = Job.bPaused.load();
	P.bWorkDone = Job.bWorkDone.load();
	return P;
}

SuperSLMPrefixSetTools::EBuildFinish SuperSLMPrefixSetTools::FinishBuildCpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& Job,
	TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	check(IsInGameThread());
	OutFindings = Job.BeginFindings;
	if (!Job.bWorkDone.load() || Job.bFinished)
	{
		OutError = FString::Printf(TEXT("%s: the build's entries have not finished (or it was already finished)"), *Job.Path);
		return EBuildFinish::Refused;
	}
	Job.bFinished = true;
	OutFindings.Append(Job.EntryFindings);
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
	Job.Context.Reset(); // the model's mapping is registry state: released here, on the game thread
#endif
	const FString& Path = Job.Path;
	if (Job.bCancel.load())
	{
		OutError = FString::Printf(TEXT("%s: the build was cancelled after %d of %d entries; the set keeps its previous build"), *Path, Job.EntriesDone.load(), Job.EntriesTotal);
		return EBuildFinish::Cancelled;
	}
	if (EntriesFingerprint(Set) != Job.Fingerprint || Set.GetPathName() != Path)
	{
		OutError = FString::Printf(TEXT("%s: the set or its model was edited while the build ran; nothing was stored (build it again)"), *Path);
		return EBuildFinish::Refused;
	}
	const std::vector<FPrefixSetEntry>& Entries = Job.Tokenized.Entries;
	const std::vector<bool>& Kept = Job.Kept;
	FPrefixEngineIdentity Producer = Job.Producer;
#if WITH_DEV_AUTOMATION_TESTS
	if (GBuildOverride.IsSet())
	{
		if (!GBuildOverride->Tag.IsEmpty())
		{
			Producer.Layer1Tag = TCHAR_TO_UTF8(*GBuildOverride->Tag);
		}
		if (!GBuildOverride->Commit.IsEmpty())
		{
			Producer.Layer1Commit = TCHAR_TO_UTF8(*GBuildOverride->Commit);
		}
		if (GBuildOverride->bFlipHash)
		{
			Producer.ArtifactHash[0] ^= 0xFF;
		}
	}
#endif
	// ReplacePrefixSetBuild's rule (host-tested): this producer's earlier builds go, the others stay
	// with their own BuiltAt, and the new build goes last.
	FPrefixSetBuild Built = AssemblePrefixSetBuild(Entries, Producer, Job.Payloads);
	KeepPreviousPrefixSetBytes(Built, CoreBuilds(Set), Kept); // per-entry failure: one replacement at the end
	TArray<FSuperSLMPrefixSetBuild> Out;
	for (const FSuperSLMPrefixSetBuild& Old : Set.Builds)
	{
		if (!PrefixSetBuildServes(CoreBuild(Old).Producer, Built.Producer))
		{
			Out.Add(Old);
		}
	}
	Out.Add(UnrealBuild(Built, FDateTime::UtcNow()));
	Set.Modify();
	Set.Builds = MoveTemp(Out);
	// Each tokenized Text entry's new ids and hashes, beside its text. A failing entry (at the tokenize pass, or in
	// its prefill, freeze or save) keeps its own, so its kept bytes stay keyed by its kept ids.
	for (int32 I = 0; I < Set.Entries.Num(); ++I)
	{
		FSuperSLMPrefixSetEntry& E = Set.Entries[I];
		const FPrefixSetEntry& T = Entries[static_cast<size_t>(I)];
		if (E.Input == ESuperSLMPrefixEntryInput::Text && !Kept[static_cast<size_t>(I)])
		{
			E.Tokens = TArray<int32>(T.Tokens.data(), static_cast<int32>(T.Tokens.size()));
			E.TextHash = HashHex(T.TextHash);
			E.TokenizedModelHash = HashHex(T.TokenizedModelHash);
		}
	}
	Set.MarkPackageDirty();

	// GPU entries: built per certified device at plan step U7 (the GPU save verb is 1.15.0's). Reported not built,
	// never as a failure, and the set's GPU builds are kept as they were (rev 1.18.1, cell 3.11).
	if (Set.Entries.Num() > 0)
	{
		OutFindings.Add(OwnFinding(Path, TEXT("GpuNotBuilt"), NAME_None,
			TEXT("GPU entries are built per device from plan step U7; a GPU subsystem recomputes every entry until then")));
	}
	return EBuildFinish::Stored;
}

bool SuperSLMPrefixSetTools::BuildCpu(USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	// The commandlet's build (and the cells' synchronous one): the background action's three phases, with the
	// entries run here, so its output is the action's byte for byte (cell 3.11).
	OutFindings.Reset();
	TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> Job = BeginBuildCpu(Set, OutError);
	if (!Job.IsValid())
	{
		return false; // the caller logs the error; the set is unchanged
	}
	RunBuildEntries(*Job);
	return FinishBuildCpu(Set, *Job, OutFindings, OutError) == EBuildFinish::Stored; // the caller logs the findings
}

#if WITH_DEV_AUTOMATION_TESTS
void SuperSLMPrefixSetTools::SetBuildPauseAfterEntries(FPrefixSetBuildJob& Job, int32 Entries)
{
	Job.PauseAfterEntries = Entries;
}

void SuperSLMPrefixSetTools::ResumeBuild(FPrefixSetBuildJob& Job)
{
	Job.bResume.store(true);
}

int64 SuperSLMPrefixSetTools::GameThreadBuildCalls()
{
	return GGameThreadBuildCalls.load();
}

void SuperSLMPrefixSetTools::ResetGameThreadBuildCalls()
{
	GGameThreadBuildCalls.store(0);
}
#endif

#endif // WITH_EDITOR

#if WITH_DEV_AUTOMATION_TESTS
SuperSLMPrefixSetTools::FScopedBuildIdentityOverride::FScopedBuildIdentityOverride(const FString& Layer1Tag, const FString& Layer1Commit, bool bFlipArtifactHash)
{
	GBuildOverride = FBuildOverride{Layer1Tag, Layer1Commit, bFlipArtifactHash};
}

SuperSLMPrefixSetTools::FScopedBuildIdentityOverride::~FScopedBuildIdentityOverride()
{
	GBuildOverride.Reset();
}
#endif

#endif // SUPERSLM_WITH_PREFIX_CACHE
