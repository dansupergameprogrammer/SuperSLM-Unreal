#include "SuperSLMRuntimeRegistry.h"

#include "SuperSLMAdapterHandle.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMStatusMapping.h"

#include "HAL/CriticalSection.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

#include "superslm/artifact.h"
#include "superslm/model.h"

#include <new>
#include <string>

namespace
{
	uint32 ReadLE32(const uint8* P)
	{
		return static_cast<uint32>(P[0]) | (static_cast<uint32>(P[1]) << 8) |
			(static_cast<uint32>(P[2]) << 16) | (static_cast<uint32>(P[3]) << 24);
	}

	uint64 ReadLE64(const uint8* P)
	{
		return static_cast<uint64>(ReadLE32(P)) | (static_cast<uint64>(ReadLE32(P + 4)) << 32);
	}

	FString StatusText(sslm_status Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
	}

	struct FMappingEntry
	{
		TWeakObjectPtr<const USuperSLMModel> Owner;
		const USuperSLMModel* OwnerKey = nullptr;
		sslm_model Handle = nullptr;
		int32 RefCount = 0;
	};

	struct FAdapterEntry
	{
		sslm_adapter Adapter = nullptr;
		sslm_model Base = nullptr;
		FString SourcePath;
		uint8 ArtifactHash[32] = {};
		int32 PinCount = 0; // plan §2.5 row 20 rule 4: held requests, restores and carrying jobs
	};

	struct FSchemaNameEntry
	{
		uint8 ArtifactHash[32] = {};
		FString SchemaName;
	};

	struct FRegistry
	{
		FCriticalSection Lock;
		TArray<FMappingEntry> Mappings;
		TMap<int64, FAdapterEntry> Adapters;
		int64 NextAdapterId = 1;
		TMap<uint64, FSchemaNameEntry> SchemaNames;
		uint64 NextSchemaId = 1;
	};

	FRegistry& Registry()
	{
		static FRegistry Instance;
		return Instance;
	}

	// Caller holds the lock. Drops one reference; unmaps at zero.
	void ReleaseMappingLocked(FRegistry& R, sslm_model Mapping)
	{
		for (int32 Index = 0; Index < R.Mappings.Num(); ++Index)
		{
			FMappingEntry& Entry = R.Mappings[Index];
			if (Entry.Handle != Mapping)
			{
				continue;
			}
			if (--Entry.RefCount > 0)
			{
				return;
			}
			const sslm_status Status = sslm_model_unmap(Entry.Handle);
			if (Status != SSLM_OK)
			{
				// SSLM_MODEL_HAS_LIVE_SEQUENCES: something still holds a sequence against this
				// mapping. Keep the entry (at zero references) rather than free a handle a live
				// sequence points into; a later release retries the unmap.
				Entry.RefCount = 0;
				UE_LOG(LogSuperSLM, Error, TEXT("sslm_model_unmap refused (%s); the mapping is kept."), *StatusText(Status));
				return;
			}
			R.Mappings.RemoveAtSwap(Index);
			return;
		}
		UE_LOG(LogSuperSLM, Warning, TEXT("ReleaseModelMapping: handle is not a registered mapping."));
	}
}

namespace SuperSLMRuntime
{
	FModelBytes CaptureModelBytes(const USuperSLMModel& Model)
	{
		// No thread assertion: the USuperSLMModel& overloads below delegate here and keep the
		// calling-thread contract they always had. A load that moves to a worker captures here on
		// the game thread first.
		FModelBytes Out;
		Out.Key = &Model;
		Out.Owner = &Model;
		Out.Pin = Model.PinArtifactBytes();
		Out.Data = Out.Pin.IsValid() ? static_cast<const uint8*>(Out.Pin->GetData()) : nullptr;
		Out.Size = Out.Pin.IsValid() ? Out.Pin->GetSize() : 0;
		Out.Name = Model.GetName();
		Out.bGpuDeviceResidentHead = Model.bGpuDeviceResidentHead;
		return Out;
	}

	bool ReadArtifactHash(const USuperSLMModel& Model, uint8 OutHash[32])
	{
		return ReadArtifactHash(CaptureModelBytes(Model), OutHash);
	}

	bool ReadArtifactHash(const FModelBytes& Model, uint8 OutHash[32])
	{
		const uint8* Data = Model.Data;
		if (Data == nullptr || Model.Size < static_cast<int64>(superslm::kHeaderBytes))
		{
			return false;
		}
		FMemory::Memcpy(OutHash, Data + 32, 32);
		return true;
	}

	FString HashToHex(const uint8 Hash[32])
	{
		return BytesToHex(Hash, 32).ToLower();
	}

	bool ReadModelShape(const USuperSLMModel& Model, FModelShape& OutShape, FString& OutError)
	{
		return ReadModelShape(CaptureModelBytes(Model), OutShape, OutError);
	}

	bool ReadModelShape(const FModelBytes& Model, FModelShape& OutShape, FString& OutError)
	{
		const uint8* Data = Model.Data;
		const int64 Size = Model.Size;
		if (Data == nullptr || Size < static_cast<int64>(superslm::kHeaderBytes))
		{
			OutError = TEXT("the model carries no artifact bytes (it was never imported or loaded)");
			return false;
		}

		// Header (64 bytes) then section_count x 40-byte rows at offset 64. The import path has
		// already validated every row (SslmArtifact::OpenFromMemory); the bounds checks here
		// guard this reader's own arithmetic, not the artifact.
		constexpr int64 RowBytes = 40;
		const uint32 SectionCount = ReadLE32(Data + 12);
		const int64 TableEnd = static_cast<int64>(superslm::kHeaderBytes) + static_cast<int64>(SectionCount) * RowBytes;
		if (TableEnd > Size)
		{
			OutError = TEXT("the artifact's section table exceeds its byte length");
			return false;
		}

		for (uint32 Row = 0; Row < SectionCount; ++Row)
		{
			const uint8* P = Data + superslm::kHeaderBytes + static_cast<int64>(Row) * RowBytes;
			if (ReadLE32(P) != static_cast<uint32>(superslm::SslmSectionType::Config))
			{
				continue;
			}
			const uint64 Offset = ReadLE64(P + 8);
			const uint64 ByteSize = ReadLE64(P + 16);
			if (Offset > static_cast<uint64>(Size) || ByteSize > static_cast<uint64>(Size) - Offset)
			{
				OutError = TEXT("the artifact's Config section lies outside its bytes");
				return false;
			}

			superslm::SslmSectionView View;
			View.type = superslm::SslmSectionType::Config;
			View.dtype = static_cast<superslm::SslmDtype>(ReadLE32(P + 4));
			View.data = Data + Offset;
			View.byte_size = ByteSize;
			View.elem_count = ReadLE64(P + 24);
			View.alignment = ReadLE32(P + 32);

			superslm::SslmModelConfig Config;
			std::string Err;
			superslm::SslmModelStatus Status;
			try
			{
				Status = superslm::ParseConfig(View, Config, &Err);
			}
			catch (const std::bad_alloc&)
			{
				OutError = TEXT("out of memory while parsing the artifact's Config section (SSLM_ALLOCATION_FAILED)");
				return false;
			}
			if (Status != superslm::SslmModelStatus::Ok)
			{
				OutError = FString::Printf(TEXT("Config section rejected: %s (%s)"),
					ANSI_TO_TCHAR(superslm::SslmModelStatusName(Status)), UTF8_TO_TCHAR(Err.c_str()));
				return false;
			}
			OutShape.NumHiddenLayers = static_cast<int32>(Config.num_hidden_layers);
			OutShape.ContextCap = static_cast<int64>(Config.context_cap);
			OutShape.VocabSize = static_cast<int32>(Config.vocab_size);
			return true;
		}

		OutError = TEXT("the artifact carries no Config section");
		return false;
	}

	sslm_model AcquireModelMapping(const USuperSLMModel& Model, FString& OutError)
	{
		return AcquireModelMapping(CaptureModelBytes(Model), OutError);
	}

	sslm_model AcquireModelMapping(const FModelBytes& Model, FString& OutError)
	{
		FRegistry& R = Registry();

		// Keyed by object identity AND liveness: a destroyed model whose address was reused by a
		// new object must not inherit the old object's mapping. The weak pointers are compared by
		// index and serial number, never dereferenced, so a worker may call this; a reused address
		// carries a new serial number.
		auto TakeExistingLocked = [&R, &Model]() -> sslm_model
		{
			for (FMappingEntry& Entry : R.Mappings)
			{
				if (Entry.OwnerKey == Model.Key && Entry.Owner.HasSameIndexAndSerialNumber(Model.Owner))
				{
					++Entry.RefCount;
					return Entry.Handle;
				}
			}
			return nullptr;
		};

		{
			FScopeLock ScopeLock(&R.Lock);
			if (const sslm_model Existing = TakeExistingLocked())
			{
				return Existing;
			}
		}

		const void* Data = Model.Data;
		const int64 Size = Model.Size;
		if (Data == nullptr || Size <= 0)
		{
			OutError = TEXT("the model carries no artifact bytes (it was never imported or loaded)");
			return nullptr;
		}

		// sslm_model_map validates and indexes the whole artifact, which is seconds of work on a
		// large model. It runs outside the registry lock so every other registry call (resolve,
		// pin, release, another model's map) proceeds meanwhile; the result is published under
		// the lock below.
		sslm_model Handle = nullptr;
		const sslm_status Status = sslm_model_map(Data, static_cast<size_t>(Size), &Handle);
		if (Status != SSLM_OK)
		{
			OutError = FString::Printf(TEXT("sslm_model_map rejected the model (%s)"), *StatusText(Status));
			return nullptr;
		}

		sslm_model Duplicate = nullptr;
		sslm_model Result = nullptr;
		{
			FScopeLock ScopeLock(&R.Lock);
			// Two callers can map the same model concurrently. The first to publish wins; the
			// loser takes a reference on the winner and unmaps its own handle, which no sequence
			// has seen.
			if (const sslm_model Winner = TakeExistingLocked())
			{
				Duplicate = Handle;
				Result = Winner;
			}
			else
			{
				FMappingEntry Entry;
				Entry.Owner = Model.Owner;
				Entry.OwnerKey = Model.Key;
				Entry.Handle = Handle;
				Entry.RefCount = 1;
				R.Mappings.Add(Entry);
				Result = Handle;
			}
		}

		if (Duplicate != nullptr)
		{
			const sslm_status UnmapStatus = sslm_model_unmap(Duplicate);
			if (UnmapStatus != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("sslm_model_unmap of a duplicate mapping refused (%s)."), *StatusText(UnmapStatus));
			}
		}
		return Result;
	}

	void ReleaseModelMapping(sslm_model Mapping)
	{
		if (Mapping == nullptr)
		{
			return;
		}
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		ReleaseMappingLocked(R, Mapping);
	}

	int64 RegisterAdapter(const FString& AbsolutePath, const USuperSLMModel& Base, FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath))
		{
			OutError = FString::Printf(TEXT("could not read adapter file: %s"), *AbsolutePath);
			return 0;
		}

		FString MapError;
		const sslm_model BaseMapping = AcquireModelMapping(Base, MapError);
		if (BaseMapping == nullptr)
		{
			OutError = FString::Printf(TEXT("base model could not be mapped: %s"), *MapError);
			return 0;
		}

		// sslm_adapter_map loads the adapter into a view that owns a copy of its bytes (model.h
		// "Ownership"), so the file buffer is released when this function returns.
		sslm_adapter Adapter = nullptr;
		const sslm_status Status = sslm_adapter_map(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), BaseMapping, &Adapter);
		if (Status != SSLM_OK)
		{
			ReleaseModelMapping(BaseMapping);
			OutError = (Status == SSLM_ADAPTER_MODEL_MISMATCH)
				? FString::Printf(TEXT("adapter %s was built for a different base model than %s (SSLM_ADAPTER_MODEL_MISMATCH)"), *AbsolutePath, *Base.GetName())
				: FString::Printf(TEXT("sslm_adapter_map rejected %s (%s)"), *AbsolutePath, *StatusText(Status));
			return 0;
		}

		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		const int64 Id = R.NextAdapterId++;
		FAdapterEntry Entry;
		Entry.Adapter = Adapter;
		Entry.Base = BaseMapping;
		Entry.SourcePath = AbsolutePath;
		// sslm_adapter_map accepted the file, so its 64-byte header (integrity hash at 32) exists.
		FMemory::Memcpy(Entry.ArtifactHash, Bytes.GetData() + 32, 32);
		R.Adapters.Add(Id, Entry);
		return Id;
	}

	bool ResolveAdapter(int64 AdapterId, FResolvedAdapter& OutAdapter)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		if (const FAdapterEntry* Entry = R.Adapters.Find(AdapterId))
		{
			OutAdapter.Adapter = Entry->Adapter;
			OutAdapter.Base = Entry->Base;
			FMemory::Memcpy(OutAdapter.ArtifactHash, Entry->ArtifactHash, 32);
			return true;
		}
		return false;
	}

	bool PinAdapter(int64 AdapterId, FResolvedAdapter& OutAdapter)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		if (FAdapterEntry* Entry = R.Adapters.Find(AdapterId))
		{
			++Entry->PinCount;
			OutAdapter.Adapter = Entry->Adapter;
			OutAdapter.Base = Entry->Base;
			FMemory::Memcpy(OutAdapter.ArtifactHash, Entry->ArtifactHash, 32);
			return true;
		}
		return false;
	}

	void UnpinAdapter(int64 AdapterId)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		if (FAdapterEntry* Entry = R.Adapters.Find(AdapterId))
		{
			if (Entry->PinCount > 0)
			{
				--Entry->PinCount;
			}
			else
			{
				UE_LOG(LogSuperSLM, Error, TEXT("UnpinAdapter: adapter %lld was not pinned; a pin was released twice."), AdapterId);
			}
		}
	}

	int64 FindAdapterByHash(const uint8 ArtifactHash[32], sslm_model Base)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		for (const TPair<int64, FAdapterEntry>& Pair : R.Adapters)
		{
			if (Pair.Value.Base == Base && FMemory::Memcmp(Pair.Value.ArtifactHash, ArtifactHash, 32) == 0)
			{
				return Pair.Key;
			}
		}
		return 0;
	}

	bool UnregisterAdapter(int64 AdapterId, FString& OutError)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		FAdapterEntry* Entry = R.Adapters.Find(AdapterId);
		if (Entry == nullptr)
		{
			OutError = TEXT("unknown adapter handle");
			return false;
		}
		if (Entry->PinCount > 0)
		{
			OutError = FString::Printf(
				TEXT("the adapter is held by %d pending request(s) or restore(s) that have not yet taken effect; let them resolve, or return the sequences that made them, first"),
				Entry->PinCount);
			return false;
		}
		const sslm_status Status = sslm_adapter_release(Entry->Adapter);
		if (Status != SSLM_OK)
		{
			// SSLM_ADAPTER_HAS_LIVE_SEQUENCES: a sequence still has it bound.
			OutError = FString::Printf(TEXT("sslm_adapter_release refused (%s); swap every sequence off this adapter first"), *StatusText(Status));
			return false;
		}
		const sslm_model Base = Entry->Base;
		R.Adapters.Remove(AdapterId);
		ReleaseMappingLocked(R, Base);
		return true;
	}

	uint64 RegisterSchemaName(const uint8 ArtifactHash[32], const FString& SchemaName)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		for (const TPair<uint64, FSchemaNameEntry>& Pair : R.SchemaNames)
		{
			if (Pair.Value.SchemaName == SchemaName && FMemory::Memcmp(Pair.Value.ArtifactHash, ArtifactHash, 32) == 0)
			{
				return Pair.Key;
			}
		}
		const uint64 Id = R.NextSchemaId++;
		FSchemaNameEntry Entry;
		FMemory::Memcpy(Entry.ArtifactHash, ArtifactHash, 32);
		Entry.SchemaName = SchemaName;
		R.SchemaNames.Add(Id, Entry);
		return Id;
	}

	bool ResolveSchemaName(uint64 SchemaId, uint8 OutArtifactHash[32], FString& OutSchemaName)
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		if (const FSchemaNameEntry* Entry = R.SchemaNames.Find(SchemaId))
		{
			FMemory::Memcpy(OutArtifactHash, Entry->ArtifactHash, 32);
			OutSchemaName = Entry->SchemaName;
			return true;
		}
		return false;
	}

	void ShutdownRegistry()
	{
		FRegistry& R = Registry();
		FScopeLock ScopeLock(&R.Lock);
		for (TPair<int64, FAdapterEntry>& Pair : R.Adapters)
		{
			const sslm_status Status = sslm_adapter_release(Pair.Value.Adapter);
			if (Status != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Shutdown: sslm_adapter_release refused for %s (%s)."), *Pair.Value.SourcePath, *StatusText(Status));
				continue;
			}
			ReleaseMappingLocked(R, Pair.Value.Base);
		}
		R.Adapters.Reset();
		for (const FMappingEntry& Entry : R.Mappings)
		{
			const sslm_status Status = sslm_model_unmap(Entry.Handle);
			if (Status != SSLM_OK)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Shutdown: sslm_model_unmap refused (%s) with %d reference(s) outstanding."), *StatusText(Status), Entry.RefCount);
			}
		}
		R.Mappings.Reset();
		R.SchemaNames.Reset();
	}
}

// --- FSuperSLMSchemaLookup (SuperSLMSchemaHandle.h) ---

bool FSuperSLMSchemaLookup::LookupByName(
	const USuperSLMModel& Model,
	const FString& SchemaName,
	FSuperSLMSchemaHandle& OutHandle,
	FString& OutError)
{
	OutHandle = FSuperSLMSchemaHandle();

	uint8 Hash[32];
	if (!SuperSLMRuntime::ReadArtifactHash(Model, Hash))
	{
		OutError = FString::Printf(TEXT("model %s carries no artifact bytes"), *Model.GetName());
		return false;
	}

	FString MapError;
	const sslm_model Mapping = SuperSLMRuntime::AcquireModelMapping(Model, MapError);
	if (Mapping == nullptr)
	{
		OutError = MapError;
		return false;
	}

	sslm_schema Schema = SSLM_SCHEMA_NONE;
	const sslm_status Status = sslm_schema_lookup(Mapping, TCHAR_TO_UTF8(*SchemaName), &Schema);
	SuperSLMRuntime::ReleaseModelMapping(Mapping);

	if (Status != SSLM_OK)
	{
		OutError = (Status == SSLM_SCHEMA_NOT_FOUND)
			? FString::Printf(TEXT("schema '%s' is not compiled into model %s (SSLM_SCHEMA_NOT_FOUND)"), *SchemaName, *Model.GetName())
			: FString::Printf(TEXT("sslm_schema_lookup failed for '%s' in model %s (%s)"), *SchemaName, *Model.GetName(),
				ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
		return false;
	}

	OutHandle.Opaque = SuperSLMRuntime::RegisterSchemaName(Hash, SchemaName);
	return true;
}

// --- FSuperSLMAdapterImport (SuperSLMAdapterHandle.h) ---

bool FSuperSLMAdapterImport::ImportFromFile(
	const FString& AbsolutePath,
	const USuperSLMModel& Base,
	FSuperSLMAdapterHandle& OutHandle,
	FString& OutError)
{
	OutHandle = FSuperSLMAdapterHandle();
	const int64 Id = SuperSLMRuntime::RegisterAdapter(AbsolutePath, Base, OutError);
	if (Id == 0)
	{
		return false;
	}
	OutHandle.Id = Id;
	return true;
}

bool FSuperSLMAdapterImport::Release(FSuperSLMAdapterHandle& Handle, FString& OutError)
{
	if (!Handle.IsValid())
	{
		OutError = TEXT("invalid adapter handle");
		return false;
	}
	if (!SuperSLMRuntime::UnregisterAdapter(Handle.Id, OutError))
	{
		return false;
	}
	Handle = FSuperSLMAdapterHandle();
	return true;
}
