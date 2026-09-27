#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate.

class USuperSLMModel;

// L2-S3 (the plan §6: "tokenize and detokenize"; Coverage Model §9,
// cells R-S3b/R-S3e; the red-suite record §4 "API choices made",
// item 2). Layer 1's own `sslm_detokenize_stream` (sslm_abi_functions.inc) takes an
// `sslm_model` and a caller-owned `sslm_detok_state*` whose all-zero value IS a valid start
// state ("{0} IS A VALID start state ... pending_count == 0 means 'no partial UTF-8 tail yet'",
// sslm_abi.h) -- unlike Tokenize (USuperSLMSubsystem::Tokenize), detokenizing a complete,
// already-generated token array needs no state carried ACROSS calls, so this is a stateless,
// subsystem-independent utility: map Model transiently (the same "transient
// sslm_model_map/unmap on Model's already-validated bytes" pattern
// SuperSLMSequenceLifecycleBudget::CheckModel already uses), feed the whole array through one
// fresh state, unmap.
//
// Not a USuperSLMSubsystem method: detokenizing a COMPLETE span needs no per-sequence state
// (see SuperSLMBlueprintLibrary.h's own Detokenize wrapper, which is this function's only
// caller in the plugin's declared surface) and this keeps the primitive usable directly by
// the MCP action tier's write path (SuperSLMToolset.h, sibling plugin) without a live
// USuperSLMSubsystem sequence.
namespace SuperSLM
{
	// Utf8Text is empty (never null) on failure; OutError names the rejection ("model has no
	// mapped artifact data", or Layer 1's own sslm_status name via SuperSLMStatusMapping.h).
	// Tokens.Num() == 0 succeeds with an empty string (matching sslm_detokenize_stream's own
	// n == 0 no-op contract).
	SUPERSLMUNREAL_API bool DetokenizeTokens(
		const USuperSLMModel& Model,
		TConstArrayView<int32> Tokens,
		FString& OutUtf8Text,
		FString& OutError);

	// L2-S3 build (2026-09-25): the same stateless shape for sslm_tokenize, so a caller that holds
	// only a GPU subsystem -- the query window's GPU rows, R-S3f's sweep -- turns typed text into
	// tokens without a configured USuperSLMSubsystem (the GPU subsystem carries no tokenizer
	// entry point, SuperSLMGpuSubsystem.h). Byte-identical to USuperSLMSubsystem::Tokenize(): the
	// same Layer-1 call on the same artifact. Game thread (the registry is game-thread API).
	SUPERSLMUNREAL_API bool TokenizeText(
		const USuperSLMModel& Model,
		const FString& Utf8Text,
		TArray<int32>& OutTokens,
		FString& OutError);
}
