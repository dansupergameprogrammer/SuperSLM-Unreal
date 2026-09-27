#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSequenceTypes.h"

// L2-S3 (the plan §8, §10.4; T-2853 design §5, D-SLM7432, D-SLM7440,
// D-SLM7448, D-SLM7449, D-SLM7453). The schema-constrained-decoding checkbox's own text layer:
// how a `prompt_result`-bound query ended, the completed output's JSON parse, and the truncated
// output's recovery. Pure functions over already-read facts and already-decoded text -- no
// subsystem, no model, no Layer-1 call -- so both backends and every caller (the editor query
// window, the MCP action tier) classify and display a query the same way.
namespace SuperSLMPromptResult
{
	// The one key `prompt_result`'s object carries (D-SLM7432), compared case-sensitively.
	SUPERSLMUNREAL_API const TCHAR* PromptResultKey();

	// What a schema-bound query's stop is composed from, read after its last token was applied.
	struct FStopFacts
	{
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMDecodeOutcome LastOutcome = ESuperSLMDecodeOutcome::TokenProduced;
		// GetStats().bSchemaAccepting on the CPU, USuperSLMGpuSubsystem::IsSchemaAccepting() on
		// the GPU (plan §2.5 row 12).
		bool bSchemaAccepting = false;
		// Prompt plus generated positions reached the sequence's context_cap -- the one
		// Faulted/Generating stop that is a budget rather than an error (T-2853 design §5: the
		// CPU carries SSLM_CONTEXT_CAP_EXCEEDED only in its fault text, the GPU as a
		// RecoverablePerSequenceRejection, so the caller counts positions instead of parsing text).
		bool bContextCapReached = false;
	};

	// T-2853 design §5's composition, in this order:
	//   accepting                                      -> Completed (whatever ended it: under
	//     `prompt_result` the accepting state's mask page is empty, so a completed walk's next
	//     step is a -2 and the sequence ends Faulted/SchemaDeadEnd with the walk accepting);
	//   Faulted / SchemaDeadEnd                        -> SchemaRejected;
	//   Complete (MaxNewTokens; a schema excludes every special id, so no stop id can end it)
	//     or Faulted / Generating at context_cap       -> BudgetExhausted.
	// False -- nothing composed -- while the sequence is still Idle/Prefilling/Decoding, and for a
	// Faulted stop that is none of the above (a device or call failure is a failed query, never a
	// stop reason).
	SUPERSLMUNREAL_API bool ComposeStopReason(const FStopFacts& Facts, ESuperSLMQueryStopReason& OutReason);

	// A Completed query's display (plan §10.4 "the JSON parse of Prompt_Result for display"):
	// RawOutput parses as exactly one JSON object whose one key is `Prompt_Result` (case-sensitive)
	// holding a string; OutValue is that string, unescaped. False, with OutError naming why.
	SUPERSLMUNREAL_API bool ParseCompletedOutput(const FString& RawOutput, FString& OutValue, FString& OutError);

	// Where the `Prompt_Result` string's content begins in RawOutput: after `{`, the key, `:` and
	// the opening quote, JSON whitespace allowed between them (the compiler's canonical form has
	// none; the scan does not rely on that). INDEX_NONE when RawOutput stops before the content.
	SUPERSLMUNREAL_API int32 FindPromptResultContentStart(const FString& RawOutput);

	// A BudgetExhausted (or SchemaRejected) query's display (T-2853 design §5): the content after
	// the value's literal prefix, scanned with escape state tracked -- content, after-backslash,
	// k-of-4 hex digits, awaiting-low-surrogate -- truncated to the last fully-formed character,
	// JSON-unescaped, stopped at the closing quote if one was reached. An incomplete trailing
	// escape is dropped; an unpaired surrogate, high or low, is dropped wherever it occurs.
	// Returns false (OutText empty) when RawOutput stops before the content begins.
	SUPERSLMUNREAL_API bool RecoverTruncatedOutput(const FString& RawOutput, FString& OutText);
}
