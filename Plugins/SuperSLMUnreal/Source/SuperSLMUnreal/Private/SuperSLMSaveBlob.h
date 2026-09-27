#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSequenceTypes.h"

// The plugin's own save-blob wrapper around Layer 1's opaque sequence state (§4: "The plugin
// tags each blob with its backend and with the Layer-1 tag and commit, rejects a cross-backend
// restore itself, and never asserts a magic value"). The wrapper has its own magic and version;
// the Layer-1 payload inside it is carried byte-for-byte and never inspected -- its format and
// migration are Layer 1's, within one pin ('SSB5' on the CPU and 'SLM5' on the GPU at the pinned
// Layer 1, plan §2.1). A blob whose Layer-1 tag differs from CompiledLayer1Tag() is refused by
// name at restore time (plan §2.5 row 23).
//
// Layout, little-endian:
//   [0]   4  wrapper magic 'S','L','U','W'
//   [4]   4  wrapper version (2)
//   [8]   1  backend (ESuperSLMBackend)            <- kBackendTagOffset
//   [9]   1  phase at save (ESuperSLMSequencePhase)
//   [10]  1  prompt span kind (ESuperSLMSpanKind)
//   [11]  1  ready-for-logits mirror (0/1)
//   [12]  16 Layer-1 tag, NUL-padded
//   [28]  40 Layer-1 commit, NUL-padded
//   [68]  32 artifact integrity hash the payload was saved against
//   [100] 4  new tokens still allowed
//   [104] 4  pinned layer budget (0 = scheduler-chosen)
//   [108] 4  layers completed in the current token (planning mirror)
//   [112] 8  context positions used
//   [120] 1  adapter bound (0/1); 3 reserved bytes
//   [124] 32 bound adapter's artifact integrity hash (zero when none) -- Layer 1's 'SSB5'
//            saves no adapter binding, so the wrapper carries it (D-SLM7341)
//   then     i32 count + i32[count]  prompt tokens not yet prefilled
//            i32 count + i32[count]  stop token ids
//            i32 count + u8[count]   bound schema name, UTF-8 (empty = none)
//            u64 count + u8[count]   Layer-1 payload (sslm_seq_save output)
namespace SuperSLMSaveBlob
{
	constexpr int32 kBackendTagOffset = 8;

	struct FContents
	{
		ESuperSLMBackend Backend = ESuperSLMBackend::CPU;
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMSpanKind SpanKind = ESuperSLMSpanKind::Prompt;
		bool bReadyForLogits = false;
		FString Layer1Tag;
		FString Layer1Commit;
		uint8 ArtifactHash[32] = {};
		int32 MaxNewTokensRemaining = 0;
		int32 LayerBudget = 0;
		int32 LayersDoneInToken = 0;
		int64 ContextUsed = 0;
		bool bHasAdapter = false;
		uint8 AdapterHash[32] = {};
		TArray<int32> PromptRemaining;
		TArray<int32> StopTokenIds;
		FString SchemaName;
	};

	// Writes the wrapper header and fields, and reserves PayloadCapacity bytes for the Layer-1
	// payload. Returns the byte offset the payload starts at; the caller writes the payload
	// there and calls FinalizePayload with the size Layer 1 actually reported.
	int64 BeginWrite(TArray<uint8>& OutBlob, const FContents& Contents, int64 PayloadCapacity);
	void FinalizePayload(TArray<uint8>& Blob, int64 PayloadOffset, int64 PayloadBytes);

	// Parses a wrapper. False with OutError on anything malformed or truncated; on success
	// OutPayload/OutPayloadBytes point into Blob.
	bool Read(const TArray<uint8>& Blob, FContents& OutContents, const uint8*& OutPayload, int64& OutPayloadBytes, FString& OutError);

	// The Layer-1 pin this build compiled against, from ThirdParty/SuperSLM/VENDORED_VERSION.txt
	// by way of SuperSLMUnreal.Build.cs.
	FString CompiledLayer1Tag();
	FString CompiledLayer1Commit();
}
