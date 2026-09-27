#pragma once

#include "CoreMinimal.h"
#include "Serialization/BulkData.h"
#include "UObject/Object.h"
#include "SuperSLMModel.generated.h"

// L2-S0 (the plan §3, §9 dim 9(d), §10; D-SLM3959). Wraps a
// validated `.sslm` artifact.
//
// Constructed only through FSuperSLMModelImport::ImportFromFile (SuperSLMModelImport.h); a
// default-constructed instance carries no mapped data. GetMappedArtifactData()'s value and
// ownership DIFFER BY PATH (D-SLM3959, ruling T-2249 review N6 -- the plan's own §3 sentence
// is now delivered rather than narrowed):
//   - **Fresh import (editor, every platform):** a CookedArtifactAlignment-aligned COPY of
//     the artifact's bytes, owned by FMemory::Malloc. Unchanged from before this ruling.
//   - **Loaded from a package saved in the editor (every platform), or a cooked package on
//     Linux/macOS-ARM (the engine offers no mapped payload there):** the same
//     FMemory::Malloc'd aligned copy, populated by Serialize()'s own bulk-data read.
//   - **Loaded from a COOKED package on Windows:** a genuine memory-mapped view --
//     `ArtifactBulkData.StealFileMapping()` yields an `FOwnedBulkDataPtr` wrapping the mapped
//     region, and GetMappedArtifactData() returns `FOwnedBulkDataPtr::GetPointer()` with NO
//     heap copy of the artifact's bytes at all. `IsPayloadMemoryMapped()` reports which case
//     obtains for the current instance.
// Two calls to GetMappedArtifactData() return the SAME pointer within one loaded instance's
// lifetime in every case -- an in-place view of that instance's own owned data (a copy on
// most paths, a real OS-level mapping on the one path the engine supports it), never a
// re-copy or re-map per call.
//
// Serialize() carries the artifact payload through save/load via `FByteBulkData`
// (`ArtifactBulkData`), requesting `BULKDATA_MemoryMappedPayload` +
// `BULKDATA_Force_NOT_InlinePayload` on every save; the engine's own cooking pipeline honours
// that request only when cooking for a platform reporting
// `ETargetPlatformFeatures::MemoryMappedFiles` (Windows, at UE 5.8) and falls back to an
// ordinary bulk-data payload everywhere else -- this plugin makes no per-platform branching
// decision of its own (D-SLM3959).
//
// L2-S3 code review S5: the bytes behind GetMappedArtifactData() are owned by a thread-safe
// shared FSuperSLMArtifactBytes rather than by the UObject directly. An off-game-thread reader
// (an async configure's model map, an async inspect) takes a pin with PinArtifactBytes() on the
// game thread, and a reload through Serialize() or a BeginDestroy() while it runs drops only
// the asset's own reference: the bytes the worker is reading are freed when its pin is.

// One artifact's bytes and whatever frees them: an FMemory::Malloc'd aligned copy (import, and
// an editor or non-mapped load), or the FOwnedBulkDataPtr a cooked load stole (a real OS
// mapping on Windows). Immutable once built; freed by the destructor on whichever thread drops
// the last pin (FOwnedBulkDataPtr's destructor and FMemory::Free are both thread-agnostic).
class SUPERSLMUNREAL_API FSuperSLMArtifactBytes
{
public:
	// Takes ownership of an FMemory::Malloc'd buffer.
	FSuperSLMArtifactBytes(void* InHeapData, int64 InSize);
	// Takes ownership of a StealFileMapping() result.
	FSuperSLMArtifactBytes(TUniquePtr<FOwnedBulkDataPtr> InOwned, int64 InSize);
	~FSuperSLMArtifactBytes();

	FSuperSLMArtifactBytes(const FSuperSLMArtifactBytes&) = delete;
	FSuperSLMArtifactBytes& operator=(const FSuperSLMArtifactBytes&) = delete;

	const void* GetData() const { return Data; }
	int64 GetSize() const { return Size; }
	bool IsMemoryMapped() const { return Owned.IsValid() && Owned->IsDataMemoryMapped(); }

private:
	void* HeapData = nullptr;
	TUniquePtr<FOwnedBulkDataPtr> Owned;
	const void* Data = nullptr;
	int64 Size = 0;
};
using FSuperSLMArtifactBytesPin = TSharedPtr<const FSuperSLMArtifactBytes, ESPMode::ThreadSafe>;

UCLASS()
class SUPERSLMUNREAL_API USuperSLMModel : public UObject
{
	GENERATED_BODY()

public:
	// The cooked artifact's mapped base pointer is aligned to this many bytes -- the format's
	// own maximum declared section alignment ceiling (docs/sslm_format.md, Load-bearing choice
	// 4). Shared between the import path (which allocates it) and Serialize() (which
	// re-allocates it identically on load), so the two can never drift to different constants.
	static constexpr int64 CookedArtifactAlignment = 4096;

	// nullptr until an import or a load has populated this asset.
	const void* GetMappedArtifactData() const { return MappedArtifactData; }

	// Total byte length of the mapped artifact, 0 until populated.
	int64 GetMappedArtifactSize() const { return MappedArtifactSize; }

	// Review S5. A shared reference to the bytes GetMappedArtifactData() points at, null until
	// populated. Game thread. Hold it for as long as any other thread reads those bytes: while it
	// is held, a reload or destruction of this asset cannot free them.
	FSuperSLMArtifactBytesPin PinArtifactBytes() const { return ArtifactBytes; }

	// Raw text of the artifact's embedded `Provenance` section (SslmSectionType::Provenance,
	// type 1 -- a JSON blob per docs/sslm_format.md's section-types table, "carrying
	// checkpoint name, license id, source hash", §3), surfaced into the asset's own Details
	// panel exactly as written by the artifact -- this plugin does not parse or re-derive the
	// individual fields, it transports Layer 1's own text unchanged. Empty if the artifact
	// carries no Provenance section.

	/** The model file's embedded provenance record (a JSON text: checkpoint name, license id, source hash), exactly as written. Empty if the file carries none. */
	UPROPERTY(VisibleAnywhere, Category = "SuperSLM|Provenance")
	FString ProvenanceJson;

	// Plan §2.5 row 3, §3 (D-SLM7645, D-SLM7664): whether the GPU backend maps this model with
	// Layer 1's device-resident output head (SSLM_GPU_RESIDENCY_HEAD_ON_DEVICE), so each token's
	// finish computes its logits row on the device instead of the host. A property of this asset,
	// not of the .sslm bytes: the same artifact maps with the head on for one asset and off for
	// another, and the artifact hash does not change. Default on, so existing assets load with
	// it on. The CPU backend never reads it. Tokens are identical either way; the head costs VRAM
	// once per mapped model (vocab_size x hidden_size bytes plus two small rows), and an
	// out-of-memory refusal of those buffers falls back to the host head once, reported by name
	// (USuperSLMGpuSubsystem::IsDeviceHeadActive()/GetDeviceHeadStatus()).

	/**
	 * GPU backend: compute each token's output logits on the GPU instead of the CPU. Tokens are
	 * identical either way. Costs GPU memory once per loaded model (vocabulary size x hidden size
	 * bytes, plus two small rows); if that memory cannot be allocated, the backend uses the CPU
	 * path and says so. The CPU backend ignores it.
	 */
	UPROPERTY(EditAnywhere, Category = "SuperSLM|GPU")
	bool bGpuDeviceResidentHead = true;

	// True only when GetMappedArtifactData() is a genuine OS-level memory mapping (a cooked
	// Windows load that took StealFileMapping() and got a real IMappedFileHandle back, per
	// FOwnedBulkDataPtr::IsDataMemoryMapped()) -- false for a fresh import, an editor-saved
	// reload, and a cooked Linux/macOS-ARM load, all of which hold an ordinary heap copy
	// (D-SLM3959: cooked memory mapping is Windows-only at UE 5.8).
	bool IsPayloadMemoryMapped() const
	{
		return ArtifactBytes.IsValid() && ArtifactBytes->IsMemoryMapped();
	}

	//~ Begin UObject interface
	virtual void Serialize(FArchive& Ar) override;
	virtual void BeginDestroy() override;
	//~ End UObject interface

private:
	friend class FSuperSLMModelImport;

	// The artifact's mapped view: ArtifactBytes' data, cached here so the accessors stay
	// plain member reads. Null exactly when ArtifactBytes is.
	void* MappedArtifactData = nullptr;
	int64 MappedArtifactSize = 0;

	// The bulk-data object Serialize() reads/writes through. Not a UPROPERTY -- hand-
	// serialized in Serialize(), matching every other UE asset type that carries a
	// memory-mappable payload (Textures, SoundWaves, Meshes).
	FByteBulkData ArtifactBulkData;

	// The owner of MappedArtifactData (review S5): the import path's own FMemory::Malloc'd copy,
	// or a Serialize()-driven load's GetCopy() buffer or StealFileMapping() result. Shared with
	// every outstanding PinArtifactBytes() holder, so replacing or dropping it here frees the
	// bytes only once no pin remains.
	TSharedPtr<FSuperSLMArtifactBytes, ESPMode::ThreadSafe> ArtifactBytes;

	// Installs Bytes (may be null) as this asset's artifact, dropping this asset's reference to
	// the previous one.
	void SetArtifactBytes(TSharedPtr<FSuperSLMArtifactBytes, ESPMode::ThreadSafe> Bytes);
};
