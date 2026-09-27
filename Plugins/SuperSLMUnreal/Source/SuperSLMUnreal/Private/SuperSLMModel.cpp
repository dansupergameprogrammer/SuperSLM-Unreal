#include "SuperSLMModel.h"
#include "HAL/UnrealMemory.h"

// D-SLM3959 (T-2249 review N6): the cooked payload is carried through Serialize() as bulk
// data rather than an inline raw byte span, so the engine's own cooking pipeline can honour a
// memory-mapped payload on the one platform it supports (Windows) and fall back to an
// ordinary bulk-data payload everywhere else -- see SuperSLMModel.h's own header comment for
// the full per-path breakdown.

FSuperSLMArtifactBytes::FSuperSLMArtifactBytes(void* InHeapData, int64 InSize)
	: HeapData(InHeapData)
	, Data(InHeapData)
	, Size((InHeapData != nullptr) ? InSize : 0)
{
}

FSuperSLMArtifactBytes::FSuperSLMArtifactBytes(TUniquePtr<FOwnedBulkDataPtr> InOwned, int64 InSize)
	: Owned(MoveTemp(InOwned))
{
	Data = Owned.IsValid() ? Owned->GetPointer() : nullptr;
	Size = (Data != nullptr) ? InSize : 0;
}

FSuperSLMArtifactBytes::~FSuperSLMArtifactBytes()
{
	// An owned FOwnedBulkDataPtr frees or unmaps in its own destructor; a heap copy is ours.
	if (HeapData != nullptr)
	{
		FMemory::Free(HeapData);
	}
}

void USuperSLMModel::SetArtifactBytes(TSharedPtr<FSuperSLMArtifactBytes, ESPMode::ThreadSafe> Bytes)
{
	// Review S5: dropping this asset's reference frees the previous bytes only when no
	// PinArtifactBytes() holder remains.
	ArtifactBytes = MoveTemp(Bytes);
	MappedArtifactData = ArtifactBytes.IsValid() ? const_cast<void*>(ArtifactBytes->GetData()) : nullptr;
	MappedArtifactSize = ArtifactBytes.IsValid() ? ArtifactBytes->GetSize() : 0;
	if (MappedArtifactData == nullptr)
	{
		ArtifactBytes.Reset();
		MappedArtifactSize = 0;
	}
}

void USuperSLMModel::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	// UPROPERTY fields (ProvenanceJson) are handled by the reflection system via
	// Super::Serialize(); MappedArtifactSize is no longer separately archived -- ArtifactBulkData's
	// own serialization records the payload's length, and GetBulkDataSize() after a load IS
	// MappedArtifactSize (set below).

	if (Ar.IsSaving())
	{
		// Populate the bulk data from whatever this instance currently holds -- a fresh
		// import's own aligned copy, or a prior load's own view -- before handing it to the
		// engine's save pipeline. Lock/Realloc/Unlock is the documented bulk-data write
		// sequence (matches the engine's own precedent, e.g.
		// FCompressedAnimSequence::SerializeCompressedData's OptionalBulk usage).
		const int64 SourceSize = (MappedArtifactData != nullptr) ? MappedArtifactSize : 0;
		uint8* Dest = static_cast<uint8*>(ArtifactBulkData.Lock(LOCK_READ_WRITE));
		Dest = static_cast<uint8*>(ArtifactBulkData.Realloc(SourceSize));
		if (SourceSize > 0)
		{
			FMemory::Memcpy(Dest, MappedArtifactData, static_cast<SIZE_T>(SourceSize));
		}
		ArtifactBulkData.Unlock();
	}

	// BULKDATA_MemoryMappedPayload + BULKDATA_Force_NOT_InlinePayload are the save-time
	// REQUEST (SuperSLMModel.h: "the mmap flag is silently ignored if the payload is also
	// inline" -- BulkData.h:133-138); SerializeWithFlags ORs them in for this save only and
	// restores the prior flags afterward, so the request is made fresh on every save rather
	// than accumulating. bAttemptFileMapping is gated on Ar.IsLoading() -- the engine asserts
	// `!bAttemptFileMapping || Ar.IsLoading()` (BulkData.cpp:1223, hit and confirmed at the
	// bench: passing true unconditionally crashes every SAVE) -- true only on a LOAD is what
	// lets it take the mapped path when the saved payload actually was cooked as one; on an
	// ordinary (non-mapped) payload the flag is simply unused. Whether the request is
	// actually honoured (a real OS mapping) or falls back to an ordinary bulk-data payload is
	// the engine's own cook-time decision, gated on ETargetPlatformFeatures::MemoryMappedFiles
	// -- this plugin makes no per-platform branch of its own (D-SLM3959).
	ArtifactBulkData.SerializeWithFlags(
		Ar, this, BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload, /*bAttemptFileMapping=*/Ar.IsLoading());

	if (Ar.IsLoading())
	{
		// Release this instance's reference to whatever it held before replacing it below. A
		// worker that pinned the previous bytes (review S5) keeps them until its pin drops.
		SetArtifactBytes(nullptr);

		const int64 BulkSize = ArtifactBulkData.GetBulkDataSize();
		if (BulkSize > 0)
		{
#if WITH_EDITOR
			// Editor-context loads -- including the cook commandlet's own load of the SOURCE
			// .uasset while producing a cooked package -- must NOT go through
			// StealFileMapping()/ForceBulkDataResident(): that retry pair is a !WITH_EDITOR-only
			// pattern in the engine's own precedent (SoundWave.cpp's InitAudioResource has this
			// exact retry only inside its `#else` of `#if WITH_EDITOR`). In editor,
			// FBulkData::CanLoadFromDisk() is `AttachedAr != nullptr || BulkChunkId.IsValid()`
			// (BulkData.cpp:781), and BulkChunkId is only ever populated by an I/O-store/pak
			// load, never an editor-domain loose-file load. Calling
			// StealFileMapping()/ForceBulkDataResident() here does not crash, but it hits
			// FBulkData::TryLoadDataIntoMemory's `CanLoadFromDisk() == false` branch
			// (BulkData.cpp:1665) and logs LogSerialization Error "Attempting to load a
			// BulkData object that cannot be loaded from disk" -- confirmed at the bench
			// (T-2227 fix round 5, D-SLM3959 cook proof): a `-run=Cook` commandlet cooking this
			// asset failed with exactly that error and exactly one fewer cooked package than
			// expected; isolating the asset out of Content/ made the error disappear (19 vs 20
			// unique-error count, 500 vs 501 cooked packages), and restoring it reproduced the
			// error again. GetCopy(nullptr, true) is the engine's own editor-safe substitute
			// (SoundWave.cpp's `#if WITH_EDITOR` branch): it swaps out the buffer
			// SerializeWithFlags() already loaded via AttachedAr with zero extra copy when
			// available, or falls back to TryLoadDataIntoMemory's own AttachedAr-based
			// synchronous read otherwise (the branch immediately above CanLoadFromDisk(), which
			// DOES succeed here since AttachedAr is still valid at this point in Serialize()).
			// The result is always an ordinary FMemory-owned heap copy, never a real OS mapping
			// -- correct, because editor domain never produces one; a genuine mapped load only
			// exists in a packaged (!WITH_EDITOR) runtime reading a cooked payload, which is
			// what the #else branch below is for.
			void* CopyPtr = nullptr;
			ArtifactBulkData.GetCopy(&CopyPtr, /*bDiscardInternalCopy=*/true);
			SetArtifactBytes(MakeShared<FSuperSLMArtifactBytes, ESPMode::ThreadSafe>(CopyPtr, BulkSize));
#else
			// Packaged runtime: StealFileMapping() -- the engine's own documented consumption
			// pattern for a bulk-data payload that MAY be memory-mapped (e.g.
			// USoundWave::InitAudioResource's own !WITH_EDITOR branch). It returns an
			// FOwnedBulkDataPtr regardless of whether the payload actually ended up mapped or
			// merely allocated, so GetPointer() is the single call site this class needs for
			// both outcomes, and FOwnedBulkDataPtr::IsDataMemoryMapped() (surfaced via
			// USuperSLMModel::IsPayloadMemoryMapped()) is what distinguishes them.
			TUniquePtr<FOwnedBulkDataPtr> Stolen(ArtifactBulkData.StealFileMapping());
			const void* Ptr = Stolen.IsValid() ? Stolen->GetPointer() : nullptr;
			if (Ptr == nullptr)
			{
				// Async-loading race (the bulk data's payload has not streamed in yet) -- force
				// it resident synchronously and retry once, matching
				// USoundWave::InitAudioResource's own fallback for the identical race. Here (and
				// only here, !WITH_EDITOR) BulkChunkId is populated by the I/O store load, so
				// CanLoadFromDisk() is true and this retry is safe.
				ArtifactBulkData.ForceBulkDataResident();
				Stolen.Reset(ArtifactBulkData.StealFileMapping());
			}
			SetArtifactBytes(MakeShared<FSuperSLMArtifactBytes, ESPMode::ThreadSafe>(MoveTemp(Stolen), BulkSize));
#endif
		}
	}
}

void USuperSLMModel::BeginDestroy()
{
	// Review S5: drops this asset's reference only; a pinned worker's bytes outlive it.
	SetArtifactBytes(nullptr);
	Super::BeginDestroy();
}
