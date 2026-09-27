#pragma once

#include "CoreMinimal.h"
#include "SuperSLMRuntimeConfig.h"

class USuperSLMModel;

// L2-S3 (the plan §7 item 1, §10.4). The model inspector's full
// readout and the adapter inspector. Every figure is either read from the artifact's own bytes
// (header, section table, CFG1, provenance) or returned by a Layer-1 size query on a mapping of
// those bytes -- the inspector computes only the Sequence Lifecycle Budget prediction (the same
// Predict() law SuperSLMSequenceLifecycleBudget.h applies at import) and the GPU device head's
// declared device-local bytes (the formula USuperSLMGpuSubsystem::Configure() declares, plan §2.5
// row 3).
//
// Off the game thread. Mapping an artifact is a whole-file SHA-256 plus a copy (model.h
// "Ownership"), the bandwidth measurement writes 128 MiB five times, and an adapter is read from
// disk; InspectModelAsync()/InspectAdapterAsync() do all of it on the thread pool, against a
// private mapping of the model's already-validated bytes, and deliver the result on the game
// thread. The model is held by a strong reference for the duration and released on the game
// thread.

// One row of the artifact's section table (docs/sslm_format.md, "Section table").
struct SUPERSLMUNREAL_API FSuperSLMSectionRow
{
	uint32 Type = 0;
	FString TypeName;   // SslmSectionType's name, or "Unknown(<n>)"
	uint32 Dtype = 0;
	int64 Offset = 0;
	int64 ByteSize = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMModelInspection
{
	bool bValid = false;
	FString Error; // why bValid is false

	// Header.
	uint32 FormatVersion = 0;
	uint32 Flags = 0;
	FString ArtifactHashHex;
	int64 FileBytes = 0;
	TArray<FSuperSLMSectionRow> Sections;
	FString ProvenanceJson;

	// CFG1.
	int32 HiddenSize = 0;
	int32 NumHiddenLayers = 0;
	int32 NumAttentionHeads = 0;
	int32 NumKeyValueHeads = 0;
	int32 HeadDim = 0;
	int32 VocabSize = 0;
	int64 ContextCap = 0;
	int32 KvPrecisionBytes = 0; // 1 (int8) or 2 (int16)

	// Schemas carried (sslm_schema_count/sslm_schema_name), in index order.
	TArray<FString> SchemaNames;

	// §7 item 1's computed footprint, for the declared shape (the FSuperSLMRuntimeConfig passed in).
	int64 WorkspaceBytes = 0;       // sslm_workspace_size
	int64 KvBlockBytes = 0;         // sslm_kv_block_size
	int64 KvPoolOverheadBytes = 0;  // sslm_kv_pool_overhead_size(BlockCount)
	int64 SeqStateBytesUpperBound = 0; // sslm_seq_state_size -- an upper bound on one save blob

	// context_cap beside the predicted reset/adopt ms against the Sequence Lifecycle Budget (§5).
	double MeasuredBandwidthBytesPerSec = 0.0;
	double PredictedResetMs = 0.0;
	double PredictedAdoptMs = 0.0;
	double SequenceLifecycleBudgetMs = 0.0;
	bool bWithinLifecycleBudget = false;

	// "DGC1: present, unused at 1.0" (§7 item 1).
	bool bHasDampedGreedyConstants = false;

	// The GPU device-head switch (§3) and, when on, the device-local bytes it declares -- a lower
	// bound, computed from vocab_size and hidden_size exactly as USuperSLMGpuSubsystem declares
	// it (the head table padded to four bytes, the H x 4 input row, the V x 8 output row).
	bool bGpuDeviceResidentHead = false;
	int64 GpuDeviceHeadDeclaredBytes = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMAdapterInspection
{
	bool bValid = false;
	FString Error;
	// sslm_adapter_map against the base's own mapping: the base check (§7 item 1). A mismatch is
	// a false bBaseMatches with Layer 1's status named in BaseCheckStatus.
	bool bBaseMatches = false;
	FString BaseCheckStatus;
	int64 ResidencyBytes = 0; // sslm_adapter_residency
	int64 FileBytes = 0;
};

// The few CFG1 facts a caller needs cheaply and on the game thread (the query window's k floor
// and its context_cap check): read from the header, the section table and CFG1 alone, with no
// mapping and no Layer-1 call.
struct SUPERSLMUNREAL_API FSuperSLMModelShapeFacts
{
	int32 HiddenSize = 0;
	int32 NumHiddenLayers = 0;
	int32 VocabSize = 0;
	int64 ContextCap = 0;
};

namespace SuperSLMModelInspector
{
	// Any thread; a read of Model's already-validated bytes. False when they carry no CFG1.
	SUPERSLMUNREAL_API bool ReadShape(const USuperSLMModel& Model, FSuperSLMModelShapeFacts& OutShape);

	// The synchronous core, callable on any thread. Data/Size are a USuperSLMModel's mapped
	// artifact bytes, which the caller keeps alive for the call. Maps them privately (never the
	// registry's shared mapping, which is game-thread API), measures host bandwidth, unmaps.
	SUPERSLMUNREAL_API FSuperSLMModelInspection InspectBytes(
		const void* Data, int64 Size, const FString& ProvenanceJson, bool bGpuDeviceResidentHead,
		const FSuperSLMRuntimeConfig& DeclaredShape);

	SUPERSLMUNREAL_API FSuperSLMAdapterInspection InspectAdapterBytes(
		const void* BaseData, int64 BaseSize, const FString& AdapterPath);

	// Game thread. Runs InspectBytes() on the thread pool and calls OnDone on the game thread.
	SUPERSLMUNREAL_API void InspectModelAsync(
		USuperSLMModel& Model, const FSuperSLMRuntimeConfig& DeclaredShape,
		TUniqueFunction<void(const FSuperSLMModelInspection&)> OnDone);

	// Game thread. Reads AdapterPath and runs InspectAdapterBytes() on the thread pool, calling
	// OnDone on the game thread.
	SUPERSLMUNREAL_API void InspectAdapterAsync(
		USuperSLMModel& Base, const FString& AdapterPath,
		TUniqueFunction<void(const FSuperSLMAdapterInspection&)> OnDone);
}
