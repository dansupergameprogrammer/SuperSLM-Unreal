#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// T-2818 review R4-W1: the plugin's one JSON-object read. Every product site that parses JSON text
// (the completed prompt_result output, the conversion pre-check's config.json, the determinism
// self-check's reference-digest and Saved/SuperSLM/SelfCheck files) goes through this, so the
// guard below is never missing at one of them.
namespace SuperSLMJson
{
	// Parses Text as one JSON object. False, with OutObject reset and OutError saying why, for
	// anything else. Text ending in a backslash is refused before the reader runs: UE 5.8's
	// TJsonReader reads the character after a backslash without an end-of-stream check
	// (JsonReader.h, ParseStringToken), and the string archive asserts on the overrun
	// (BufferReader.h). A complete JSON object never ends in a backslash, so nothing valid is
	// refused -- a truncated file or a cut-off model output is, instead of crashing the editor.
	SUPERSLMUNREAL_API bool TryReadObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError);
}
