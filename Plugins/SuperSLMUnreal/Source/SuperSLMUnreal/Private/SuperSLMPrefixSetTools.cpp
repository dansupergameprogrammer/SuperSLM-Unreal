// See SuperSLMPrefixSetTools.h.

#include "SuperSLMPrefixSetTools.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Core/SuperSLMPrefixSetCore.h"
#include "SuperSLMPrefixSetBridge.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuPrefixSetAccess.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
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
		F.AdapterVendorId = L.AdapterVendorId;
		F.AdapterDeviceId = L.AdapterDeviceId;
		F.Line = UTF8_TO_TCHAR(FormatPrefixSetFinding(std::string(TCHAR_TO_UTF8(*SetPath)), L).c_str());
		return F;
	}

	// "CPU", "GPU 0x1002:0x744c", or "GPU" for a GPU producer whose device is not known: the producer as the core's
	// finding lines name it (FormatPrefixSetFinding).
	[[maybe_unused]] FString ProducerLabel(const FPrefixEngineIdentity& Producer)
	{
		if (Producer.Backend == 0)
		{
			return TEXT("CPU");
		}
		if (Producer.AdapterVendorId == 0 && Producer.AdapterDeviceId == 0)
		{
			return TEXT("GPU");
		}
		return FString::Printf(TEXT("GPU 0x%04x:0x%04x"), Producer.AdapterVendorId & 0xFFFFu, Producer.AdapterDeviceId & 0xFFFFu);
	}

	// A build's own finding (BuildFailed, UnknownSchema, GpuNotBuilt, AdapterIdMismatch), against Producer. With no
	// entry, a finding about the whole set: "<set>: [<producer>]: <Finding> (<detail>)".
	[[maybe_unused]] FSuperSLMPrefixSetFinding OwnFinding(const FString& SetPath, const TCHAR* Finding, FName Entry, const FString& Detail,
		const FPrefixEngineIdentity& Producer)
	{
		FSuperSLMPrefixSetFinding F;
		F.Finding = FName(Finding);
		F.Entry = Entry;
		F.bHasProducer = true;
		F.Backend = Producer.Backend;
		F.AdapterVendorId = Producer.Backend != 0 ? Producer.AdapterVendorId : 0u;
		F.AdapterDeviceId = Producer.Backend != 0 ? Producer.AdapterDeviceId : 0u;
		const FString Label = ProducerLabel(Producer);
		F.Line = Entry.IsNone()
			? FString::Printf(TEXT("%s: [%s]: %s (%s)"), *SetPath, *Label, Finding, *Detail)
			: FString::Printf(TEXT("%s: entry '%s' [%s]: %s (%s)"), *SetPath, *Entry.ToString(), *Label, Finding, *Detail);
		return F;
	}

	// The identity the cook ships with (or a build is made under), and the model's shape: this build's Layer-1 tag
	// and commit and the set's model's artifact hash, with Producer's backend and adapter ids (the CPU's: all 0).
	bool CurrentIdentity(const USuperSLMPrefixSet& Set, const FPrefixEngineIdentity& Producer, FPrefixEngineIdentity& OutIdentity,
		FPrefixSetModelShape& OutShape, FString& OutError)
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
		OutIdentity.Backend = Producer.Backend;
		OutIdentity.AdapterVendorId = Producer.Backend != 0 ? Producer.AdapterVendorId : 0u;
		OutIdentity.AdapterDeviceId = Producer.Backend != 0 ? Producer.AdapterDeviceId : 0u;
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
		uint32 AdapterVendorId = 0; // non-zero: a GPU build is stamped as this vendor's
		uint32 AdapterDeviceId = 0; // non-zero: ... and this device's
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
	if (!CurrentIdentity(Set, FPrefixEngineIdentity(), Current, Shape, OutError)) // each build is checked for its own producer
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

// One build in three phases (§5.8 *The build runs in the background*, PC-15 Q9; cell 3.11). The CPU's: Begin on the game
// thread (identity, the tokenize pass, the manifest, the model's mapping, the pool and workspace), the entries on any
// one thread (prefill, freeze and save per entry; cancellable between entries), and Finish on the game thread (one
// replacement of the set's CPU build, or nothing on a cancel). The GPU's (step U7): Begin on the game thread (the GPU
// cache's identity, the tokenize pass, the manifest), the entries one at a time through the GPU subsystem's own cache
// (PumpBuildGpu, game thread; every GPU call on the submission thread), and the same Finish for this device's build.
struct SuperSLMPrefixSetTools::FPrefixSetBuildJob
{
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
	TUniquePtr<FBuildContext> Context; // created and released on the game thread; used by the entries' thread between
#endif
	FString Path;
	FPrefixEngineIdentity Producer;            // the real producer: the payloads' SPX1 headers are checked against it
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
	// The GPU build's state (game thread only): the subsystem it runs through, the next entry to request, and the
	// entry in flight (its lease of the GPU cache).
	bool bGpu = false;
	TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
	size_t NextEntry = 0;
	bool bLeased = false;
	size_t LeaseIndex = 0;
	FSuperSLMPrefix Lease;
};

namespace
{
	// Both builds' first phase after the identity, on the game thread: the tokenize pass (§5.8 *Tokenized at build*; the
	// registry is game-thread API), before any prefill, then the manifest. Each Text entry's own text, nothing prepended
	// (a nested entry's suffix alone), against the set's model. A failing entry keeps its previous ids and bytes and is
	// reported by name; the others build. Kept[i]: entry i failed at the tokenize pass (and keeps its bytes).
	void PrepareBuildEntries(USuperSLMPrefixSet& Set, const FPrefixSetModelShape& Shape, SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job)
	{
		const FString& Path = Job.Path;
		Job.Tokenized = TokenizePrefixSetText(CoreEntries(Set), Shape, Job.Producer.ArtifactHash,
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
		// Kept[i]: entry i failed (at the tokenize pass, or later in its build or save) and keeps its bytes.
		Job.Kept = Tokenized.Failed;
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			Buildable[I] = Buildable[I] && !Job.Kept[I];
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
	}
}

TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> SuperSLMPrefixSetTools::BeginBuildCpu(USuperSLMPrefixSet& Set, FString& OutError)
{
	check(IsInGameThread());
	TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> JobPtr = MakeShared<FPrefixSetBuildJob, ESPMode::ThreadSafe>();
	FPrefixSetBuildJob& Job = *JobPtr;
	Job.Path = Set.GetPathName();
	const FString& Path = Job.Path;
	FPrefixSetModelShape Shape;
	if (!CurrentIdentity(Set, FPrefixEngineIdentity(), Job.Producer, Shape, OutError))
	{
		return nullptr;
	}
#if !defined(SSLM_HAS_PREFIX_SAVE_ABI)
	OutError = FString::Printf(TEXT("%s: this build's Layer 1 (%s) has no prefix save; cooked prefix sets need 1.14.0"), *Path, *SuperSLMSaveBlob::CompiledLayer1Tag());
	return nullptr;
#else
	// The tokenize pass and the manifest, here on the game thread, before the model is mapped for the build and
	// before any prefill.
	PrepareBuildEntries(Set, Shape, Job);
	const std::vector<FPrefixSetEntry>& Entries = Job.Tokenized.Entries;
	const std::vector<bool>& Buildable = Job.Buildable;

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
			Job.EntryFindings.Add(OwnFinding(Job.Path, Finding, Job.EntryNames[static_cast<int32>(I)], Detail + TEXT("; it keeps its previous bytes"), Job.Producer));
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

namespace
{
	// The producer a job's build is stored under: the real one, with the test seam's stamp applied (a previous pin,
	// another model, or, for a GPU build, another device).
	FPrefixEngineIdentity StampedProducer(const SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job)
	{
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
			if (Producer.Backend != 0 && GBuildOverride->AdapterVendorId != 0)
			{
				Producer.AdapterVendorId = GBuildOverride->AdapterVendorId;
			}
			if (Producer.Backend != 0 && GBuildOverride->AdapterDeviceId != 0)
			{
				Producer.AdapterDeviceId = GBuildOverride->AdapterDeviceId;
			}
		}
#endif
		return Producer;
	}

	// The common head of every finish, on the game thread: the job's own findings (its first phase's unless
	// bBeginFindings is false, then its entries'), the build's mapping released, and the outcomes that store
	// nothing: not done or finished twice, cancelled, or the set edited while it ran. True when the job may be
	// stored; otherwise OutResult and OutError say why.
	bool BeginFinish(USuperSLMPrefixSet& Set, SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job, bool bBeginFindings,
		TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError, SuperSLMPrefixSetTools::EBuildFinish& OutResult)
	{
		using SuperSLMPrefixSetTools::EBuildFinish;
		if (bBeginFindings)
		{
			OutFindings.Append(Job.BeginFindings);
		}
		if (!Job.bWorkDone.load() || Job.bFinished)
		{
			OutError = FString::Printf(TEXT("%s: the build's entries have not finished (or it was already finished)"), *Job.Path);
			OutResult = EBuildFinish::Refused;
			return false;
		}
		Job.bFinished = true;
		OutFindings.Append(Job.EntryFindings);
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
		Job.Context.Reset(); // the model's mapping is registry state: released here, on the game thread
#endif
		if (Job.bCancel.load())
		{
			OutError = FString::Printf(TEXT("%s: the build was cancelled after %d of %d entries; the set keeps its previous build"), *Job.Path,
				Job.EntriesDone.load(), Job.EntriesTotal);
			OutResult = EBuildFinish::Cancelled;
			return false;
		}
		if (EntriesFingerprint(Set) != Job.Fingerprint || Set.GetPathName() != Job.Path)
		{
			OutError = FString::Printf(TEXT("%s: the set or its model was edited while the build ran; nothing was stored (build it again)"), *Job.Path);
			OutResult = EBuildFinish::Refused;
			return false;
		}
		return true;
	}

	// A GPU build's check before it is stored (§5.8, PC-5): every payload's SPX1 header names the producer the build
	// ran as (backend 1, its adapter's vendor and device). False, with an AdapterIdMismatch finding, otherwise.
	bool GpuPayloadsFromProducer(const SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
	{
		size_t Index = 0;
		uint32_t Backend = 0;
		uint32_t Vendor = 0;
		uint32_t Device = 0;
		if (PrefixSetPayloadsFromProducer(Job.Payloads, Job.Producer, Index, Backend, Vendor, Device))
		{
			return true;
		}
		const FName Entry = Job.EntryNames.IsValidIndex(static_cast<int32>(Index)) ? Job.EntryNames[static_cast<int32>(Index)] : NAME_None;
		const FString Detail = FString::Printf(TEXT("its SPX1 payload names backend %u, adapter 0x%04x:0x%04x, not this build's GPU; the GPU part is not stored"),
			Backend, Vendor & 0xFFFFu, Device & 0xFFFFu);
		OutFindings.Add(OwnFinding(Job.Path, TEXT("AdapterIdMismatch"), Entry, Detail, Job.Producer));
		OutError = FString::Printf(TEXT("%s: entry '%s': %s"), *Job.Path, *Entry.ToString(), *Detail);
		return false;
	}

	// What a finish stores, staged on copies of the set's builds and entries and written in one Modify().
	struct FStagedSet
	{
		TArray<FSuperSLMPrefixSetBuild> Builds;
		TArray<FSuperSLMPrefixSetEntry> Entries;
		bool bEntriesTokenized = false;
	};

	// One job's build into Staged: its producer's earlier builds go (ReplacePrefixSetBuild's rule, host-tested), the
	// others stay with their own BuiltAt, and the new build goes last, every entry with Kept[i] set keeping the bytes
	// that producer's previous build stored under its name (KeepPreviousPrefixSetBytes; one replacement at the end).
	// The first job staged also writes each tokenized Text entry's new ids and hashes beside its text; an entry kept
	// keeps its own, so its kept bytes stay keyed by its kept ids.
	void StageBuild(const USuperSLMPrefixSet& Set, const SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job, const std::vector<bool>& Kept, FStagedSet& Staged)
	{
		const std::vector<FPrefixSetEntry>& Entries = Job.Tokenized.Entries;
		FPrefixSetBuild Built = AssemblePrefixSetBuild(Entries, StampedProducer(Job), Job.Payloads);
		KeepPreviousPrefixSetBytes(Built, CoreBuilds(Set), Kept);
		TArray<FSuperSLMPrefixSetBuild> Out;
		for (const FSuperSLMPrefixSetBuild& Old : Staged.Builds)
		{
			if (!PrefixSetBuildServes(CoreBuild(Old).Producer, Built.Producer))
			{
				Out.Add(Old);
			}
		}
		Out.Add(UnrealBuild(Built, FDateTime::UtcNow()));
		Staged.Builds = MoveTemp(Out);
		if (Staged.bEntriesTokenized)
		{
			return;
		}
		Staged.bEntriesTokenized = true;
		for (int32 I = 0; I < Staged.Entries.Num() && static_cast<size_t>(I) < Entries.size(); ++I)
		{
			FSuperSLMPrefixSetEntry& E = Staged.Entries[I];
			const FPrefixSetEntry& T = Entries[static_cast<size_t>(I)];
			if (E.Input == ESuperSLMPrefixEntryInput::Text && !Kept[static_cast<size_t>(I)])
			{
				E.Tokens = TArray<int32>(T.Tokens.data(), static_cast<int32>(T.Tokens.size()));
				E.TextHash = HashHex(T.TextHash);
				E.TokenizedModelHash = HashHex(T.TokenizedModelHash);
			}
		}
	}

	void StoreStaged(USuperSLMPrefixSet& Set, FStagedSet& Staged)
	{
		Set.Modify();
		Set.Builds = MoveTemp(Staged.Builds);
		Set.Entries = MoveTemp(Staged.Entries);
		Set.MarkPackageDirty();
	}

	FPrefixEngineIdentity AnyGpuProducer()
	{
		FPrefixEngineIdentity Gpu;
		Gpu.Backend = 1; // its device unknown: the finding reads [GPU]
		return Gpu;
	}

	const TCHAR* LeaseStateName(ESuperSLMPrefixLeaseState State)
	{
		switch (State)
		{
		case ESuperSLMPrefixLeaseState::Invalid: return TEXT("Invalid");
		case ESuperSLMPrefixLeaseState::Pending: return TEXT("Pending");
		case ESuperSLMPrefixLeaseState::Ready: return TEXT("Ready");
		case ESuperSLMPrefixLeaseState::BudgetExhausted: return TEXT("BudgetExhausted");
		case ESuperSLMPrefixLeaseState::PoolExhausted: return TEXT("PoolExhausted");
		case ESuperSLMPrefixLeaseState::UnknownSchema: return TEXT("UnknownSchema");
		case ESuperSLMPrefixLeaseState::Cancelled: return TEXT("Cancelled");
		case ESuperSLMPrefixLeaseState::Failed: return TEXT("Failed");
		case ESuperSLMPrefixLeaseState::Ended: return TEXT("Ended");
		}
		return TEXT("Unknown");
	}
}

SuperSLMPrefixSetTools::EBuildFinish SuperSLMPrefixSetTools::FinishBuildCpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& Job,
	TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	check(IsInGameThread());
	OutFindings.Reset();
	EBuildFinish Result = EBuildFinish::Refused;
	if (!BeginFinish(Set, Job, /*bBeginFindings*/ true, OutFindings, OutError, Result))
	{
		return Result;
	}
	FStagedSet Staged{Set.Builds, Set.Entries};
	StageBuild(Set, Job, Job.Kept, Staged);
	StoreStaged(Set, Staged);
	// A CPU build alone makes no GPU entries: the set's GPU builds are kept as they were, and reported not built,
	// never as a failure (rev 1.18.1, cell 3.11). The Build Prefixes action and the commandlet build this machine's
	// GPU's entries beside it (FinishBuildCpuAndGpu).
	if (Set.Entries.Num() > 0)
	{
		OutFindings.Add(OwnFinding(Job.Path, TEXT("GpuNotBuilt"), NAME_None,
			TEXT("this build made the CPU entries only; the Build Prefixes action and the SuperSLMBuildPrefixes commandlet build the GPU entries of the GPU they run on, and a GPU with no build of its own recomputes every entry"),
			AnyGpuProducer()));
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

// ---- The GPU build, per device (§5.8, PC-5; step U7) ----

TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> SuperSLMPrefixSetTools::BeginBuildGpu(USuperSLMPrefixSet& Set,
	USuperSLMGpuSubsystem& Gpu, FString& OutError)
{
	check(IsInGameThread());
	TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> JobPtr = MakeShared<FPrefixSetBuildJob, ESPMode::ThreadSafe>();
	FPrefixSetBuildJob& Job = *JobPtr;
	Job.Path = Set.GetPathName();
	Job.bGpu = true;
	const FString& Path = Job.Path;
	// The GPU cache the entries are built through: its identity is the producer (backend 1, the vendor and device of
	// the adapter Layer 1 runs on). Cold for this build: no disk tier and no cooked set, so every entry is built on
	// this device here (a Memory hit is an entry this configuration already built on it).
	FPrefixEngineIdentity CacheIdentity;
	bool bDisk = false;
	int32 Cooked = 0;
	FString Why;
	if (!FSuperSLMGpuPrefixSetAccess::ReadBuildCache(Gpu, CacheIdentity, bDisk, Cooked, Why))
	{
		OutError = FString::Printf(TEXT("%s: the GPU entries need a GPU subsystem configured pooled with a prefix cache (%s)"), *Path, *Why);
		return nullptr;
	}
	if (bDisk)
	{
		OutError = FString::Printf(TEXT("%s: the GPU subsystem's prefix cache has its disk tier on; the GPU build needs it off, so every entry is built rather than read back"), *Path);
		return nullptr;
	}
	if (Cooked > 0)
	{
		OutError = FString::Printf(TEXT("%s: the GPU subsystem's prefix cache has %d cooked entr%s registered; the GPU build needs none, so every entry is built rather than restored"),
			*Path, Cooked, Cooked == 1 ? TEXT("y") : TEXT("ies"));
		return nullptr;
	}
	FPrefixSetModelShape Shape;
	if (!CurrentIdentity(Set, CacheIdentity, Job.Producer, Shape, OutError))
	{
		return nullptr;
	}
	if (Job.Producer.ArtifactHash != CacheIdentity.ArtifactHash || Gpu.GetConfiguredModel() == nullptr)
	{
		OutError = FString::Printf(TEXT("%s: the GPU subsystem is configured with %s, not the set's model %s (PrefixSetModelMismatch); nothing was built"), *Path,
			Gpu.GetConfiguredModel() != nullptr ? *Gpu.GetConfiguredModel()->GetPathName() : TEXT("no model"), *Set.Model->GetPathName());
		return nullptr;
	}
	Job.Producer = CacheIdentity; // the key the GPU cache computes for each entry is the one the build stores
	PrepareBuildEntries(Set, Shape, Job);
	Job.Gpu = &Gpu;
	return JobPtr;
}

bool SuperSLMPrefixSetTools::PumpBuildGpu(FPrefixSetBuildJob& Job, USuperSLMGpuSubsystem& Gpu, float DeltaSeconds)
{
	check(IsInGameThread());
	if (!Job.bGpu || Job.bWorkDone.load())
	{
		return false;
	}
	const std::vector<FPrefixSetEntry>& Entries = Job.Tokenized.Entries;
	if (Job.Gpu.Get() != &Gpu)
	{
		// Not the subsystem the build began on: every entry left keeps its previous bytes.
		Job.EntryFindings.Add(OwnFinding(Job.Path, TEXT("BuildFailed"), NAME_None,
			TEXT("PumpBuildGpu was given another GPU subsystem than BeginBuildGpu's; the entries left keep their previous bytes"), Job.Producer));
		for (size_t I = Job.NextEntry; I < Entries.size(); ++I)
		{
			Job.Kept[I] = Job.Kept[I] || Job.Buildable[I];
		}
		Job.bWorkDone.store(true);
		return false;
	}
	Gpu.Tick(DeltaSeconds); // the request's key hash, its build steps and their deliveries all move on ticks

	if (Job.bLeased)
	{
		const ESuperSLMPrefixLeaseState LeaseState = Gpu.GetPrefixLeaseState(Job.Lease);
		if (LeaseState == ESuperSLMPrefixLeaseState::Pending)
		{
			return true;
		}
		const size_t I = Job.LeaseIndex;
		TArray<uint8> Payload;
		FString Why;
		const TCHAR* Finding = TEXT("BuildFailed");
		if (LeaseState == ESuperSLMPrefixLeaseState::Ready)
		{
			bool bOnGameThread = false;
			if (!FSuperSLMGpuPrefixSetAccess::SaveLease(Gpu, Job.Lease, Payload, bOnGameThread, Why))
			{
				Payload.Reset();
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (bOnGameThread)
			{
				GGameThreadBuildCalls.fetch_add(1); // cell 3.11's counter: a GPU save on the game thread (never expected)
			}
#endif
		}
		else
		{
			Finding = LeaseState == ESuperSLMPrefixLeaseState::UnknownSchema ? TEXT("UnknownSchema") : TEXT("BuildFailed");
			Why = FString::Printf(TEXT("the GPU cache's lease resolved %s"), LeaseStateName(LeaseState));
		}
		Gpu.ReturnPrefix(Job.Lease);
		Job.Lease = FSuperSLMPrefix();
		Job.bLeased = false;
		if (Payload.Num() == 0)
		{
			Job.EntryFindings.Add(OwnFinding(Job.Path, Finding, Job.EntryNames[static_cast<int32>(I)], Why + TEXT("; it keeps its previous bytes"), Job.Producer));
			Job.Kept[I] = true;
		}
		else
		{
			Job.Payloads[I].assign(Payload.GetData(), Payload.GetData() + Payload.Num());
		}
		Job.EntriesDone.fetch_add(1);
		Job.NextEntry = I + 1;
	}

	// Between entries: a cancel ends the build here (the entry in hand has finished); the test pause holds here.
	if (Job.bCancel.load())
	{
		Job.bPaused.store(false);
		Job.bWorkDone.store(true);
		return false;
	}
	size_t Next = Job.NextEntry;
	while (Next < Entries.size() && !Job.Buildable[Next])
	{
		++Next;
	}
	Job.NextEntry = Next;
	if (Next >= Entries.size())
	{
		Job.bWorkDone.store(true);
		return false;
	}
	if (Job.PauseAfterEntries >= 0 && Job.EntriesDone.load() == Job.PauseAfterEntries && !Job.bResume.load())
	{
		Job.bPaused.store(true);
		return true;
	}
	Job.bPaused.store(false);
	const FPrefixSetEntry& E = Entries[Next];
	FSuperSLMPrefixSpec Spec;
	Spec.Tokens = TArray<int32>(E.Tokens.data(), static_cast<int32>(E.Tokens.size()));
	Spec.SchemaName = E.SchemaName.empty() ? NAME_None : FName(UTF8_TO_TCHAR(E.SchemaName.c_str()));
	FString RequestError;
	if (!Gpu.RequestPrefix(Spec, Job.Lease, RequestError))
	{
		Job.Lease = FSuperSLMPrefix();
		Job.EntryFindings.Add(OwnFinding(Job.Path, TEXT("BuildFailed"), Job.EntryNames[static_cast<int32>(Next)],
			FString::Printf(TEXT("the GPU cache refused its request (%s); it keeps its previous bytes"), *RequestError), Job.Producer));
		Job.Kept[Next] = true;
		Job.EntriesDone.fetch_add(1);
		Job.NextEntry = Next + 1;
		return true;
	}
	Job.bLeased = true;
	Job.LeaseIndex = Next;
	return true;
}

SuperSLMPrefixSetTools::EBuildFinish SuperSLMPrefixSetTools::FinishBuildGpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& Job,
	TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	check(IsInGameThread());
	OutFindings.Reset();
	EBuildFinish Result = EBuildFinish::Refused;
	if (!Job.bGpu)
	{
		OutError = FString::Printf(TEXT("%s: FinishBuildGpu was given a CPU build"), *Job.Path);
		return EBuildFinish::Refused;
	}
	if (!BeginFinish(Set, Job, /*bBeginFindings*/ true, OutFindings, OutError, Result))
	{
		return Result;
	}
	if (!GpuPayloadsFromProducer(Job, OutFindings, OutError))
	{
		return EBuildFinish::Refused;
	}
	FStagedSet Staged{Set.Builds, Set.Entries};
	StageBuild(Set, Job, Job.Kept, Staged);
	StoreStaged(Set, Staged);
	return EBuildFinish::Stored;
}

SuperSLMPrefixSetTools::EBuildFinish SuperSLMPrefixSetTools::FinishBuildCpuAndGpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& CpuJob, FPrefixSetBuildJob* GpuJob,
	const FString& GpuNotBuiltReason, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError, bool& bOutGpuStored)
{
	check(IsInGameThread());
	OutFindings.Reset();
	bOutGpuStored = false;
	EBuildFinish CpuResult = EBuildFinish::Refused;
	const bool bCpu = BeginFinish(Set, CpuJob, /*bBeginFindings*/ true, OutFindings, OutError, CpuResult);
	// The GPU job is finished whatever the CPU's outcome. Its first phase's findings are the CPU's (the same tokenize
	// pass and manifest), so only its entries' are reported.
	TArray<FSuperSLMPrefixSetFinding> GpuFindings;
	FString GpuError;
	EBuildFinish GpuResult = EBuildFinish::Refused;
	const bool bGpu = GpuJob != nullptr && GpuJob->bGpu && BeginFinish(Set, *GpuJob, /*bBeginFindings*/ false, GpuFindings, GpuError, GpuResult);
	if (!bCpu)
	{
		OutFindings.Append(GpuFindings);
		return CpuResult;
	}
	if (GpuJob != nullptr && !bGpu && GpuResult == EBuildFinish::Cancelled)
	{
		// One build, one cancel: nothing is stored.
		OutFindings.Append(GpuFindings);
		OutError = GpuError;
		return EBuildFinish::Cancelled;
	}
	// A Text entry's ids are written once, for both builds, so its bytes in each stay keyed by the ids it keeps: an
	// entry either build failed keeps its previous ids and its previous bytes in both.
	std::vector<bool> CpuKept = CpuJob.Kept;
	std::vector<bool> GpuKept = bGpu ? GpuJob->Kept : std::vector<bool>();
	if (bGpu)
	{
		for (int32 I = 0; I < Set.Entries.Num() && static_cast<size_t>(I) < CpuKept.size() && static_cast<size_t>(I) < GpuKept.size(); ++I)
		{
			if (Set.Entries[I].Input == ESuperSLMPrefixEntryInput::Text)
			{
				const bool bKept = CpuKept[static_cast<size_t>(I)] || GpuKept[static_cast<size_t>(I)];
				CpuKept[static_cast<size_t>(I)] = bKept;
				GpuKept[static_cast<size_t>(I)] = bKept;
			}
		}
	}
	FStagedSet Staged{Set.Builds, Set.Entries};
	StageBuild(Set, CpuJob, CpuKept, Staged);
	if (bGpu)
	{
		if (GpuPayloadsFromProducer(*GpuJob, GpuFindings, GpuError))
		{
			StageBuild(Set, *GpuJob, GpuKept, Staged);
			bOutGpuStored = true;
		}
	}
	else
	{
		const FString Reason = GpuJob != nullptr ? GpuError : GpuNotBuiltReason;
		GpuFindings.Add(OwnFinding(CpuJob.Path, TEXT("GpuNotBuilt"), NAME_None,
			FString::Printf(TEXT("%s; the set's GPU builds are kept as they were"), Reason.IsEmpty() ? TEXT("the GPU part did not run") : *Reason), AnyGpuProducer()));
	}
	OutFindings.Append(GpuFindings);
	StoreStaged(Set, Staged);
	return EBuildFinish::Stored;
}

void SuperSLMPrefixSetTools::GetBuildProducer(const FPrefixSetBuildJob& Job, uint8& OutBackend, uint32& OutVendorId, uint32& OutDeviceId)
{
	OutBackend = Job.Producer.Backend;
	OutVendorId = Job.Producer.AdapterVendorId;
	OutDeviceId = Job.Producer.AdapterDeviceId;
}

bool SuperSLMPrefixSetTools::MakeGpuBuildConfig(const USuperSLMPrefixSet& Set, FSuperSLMGpuRuntimeConfig& OutConfig,
	FSuperSLMGpuPrefixCacheSettings& OutCacheSettings, FString& OutError)
{
	OutConfig = FSuperSLMGpuRuntimeConfig();
	OutCacheSettings = FSuperSLMGpuPrefixCacheSettings();
	if (Set.Model == nullptr)
	{
		OutError = FString::Printf(TEXT("%s: the prefix set names no model"), *Set.GetPathName());
		return false;
	}
	SuperSLMRuntime::FModelShape Shape;
	FString ShapeError;
	if (!SuperSLMRuntime::ReadModelShape(*Set.Model, Shape, ShapeError) || Shape.ContextCap <= 0 || Shape.NumHiddenLayers <= 0)
	{
		OutError = FString::Printf(TEXT("%s: the model %s's shape is unreadable (%s)"), *Set.GetPathName(), *Set.Model->GetPathName(), *ShapeError);
		return false;
	}
	// superslm_gpu::kQkNormDispatchesPerLayer (gpu_port.h), the larger of Layer 1's two per-layer dispatch counts: a
	// budget of this x the depth is a whole token per tick on either (SuperSLMEditorRuntimeHost's load-time value).
	constexpr uint32 kMaxDispatchesPerLayer = 25;
	OutConfig.ContextCap = Shape.ContextCap; // the manifest admits entries up to the model's cap
	OutConfig.BlockCount = 1;
	OutConfig.DispatchBudget = kMaxDispatchesPerLayer * static_cast<uint32>(Shape.NumHiddenLayers);
	OutConfig.K = 2; // above the one-sequence floor; a KBelowMinimum names the floor and the caller configures again at it
	OutConfig.TickBudgetMs = 1000.0;
	OutConfig.SequenceBudget = static_cast<int32>(FMath::Min<int64>(Shape.ContextCap, 64)); // pooled: GPU prefixes exist only there
	OutConfig.PrefixBlockCount = 0;
	OutConfig.RestoreHeadroomBlocks = 1;
	OutConfig.CacheGhostBlocks = 1;
	OutConfig.bCalibrateGpuRowCost = false;
	// Any budget above 0 sizes region C in whole blocks of ceil(ContextCap / B) pages (RegionCPagesFor), so one byte
	// is one block: it holds the largest entry the manifest admits (at most the model's cap) at its peak charge.
	OutCacheSettings.BudgetBytes = 1;
	OutCacheSettings.bDiskEnabled = false;
	OutCacheSettings.DiskMaxBytes = 0;
	return true;
}

bool SuperSLMPrefixSetTools::AnyGpuSubsystemConfigured()
{
	return FSuperSLMGpuPrefixSetAccess::ConfiguredSubsystemCount() > 0;
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
SuperSLMPrefixSetTools::FScopedBuildIdentityOverride::FScopedBuildIdentityOverride(const FString& Layer1Tag, const FString& Layer1Commit, bool bFlipArtifactHash,
	uint32 AdapterVendorId, uint32 AdapterDeviceId)
{
	GBuildOverride = FBuildOverride{Layer1Tag, Layer1Commit, bFlipArtifactHash, AdapterVendorId, AdapterDeviceId};
}

SuperSLMPrefixSetTools::FScopedBuildIdentityOverride::~FScopedBuildIdentityOverride()
{
	GBuildOverride.Reset();
}
#endif

#endif // SUPERSLM_WITH_PREFIX_CACHE
