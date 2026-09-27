#pragma once

#include "CoreMinimal.h"

// L2-S0 (the plan §3, §10; the L2-S0 red-suite
// record §4 "Expected interface"). The result of one import attempt.
//
// Fields mirror `superslm::SslmError{code, section_index, message}` (ThirdParty/SuperSLM/
// include/superslm/artifact.h) via `superslm::SslmStatusName(code)` -- this struct routes
// that struct through, it does not invent a second taxonomy (§10 L2-S0 gate: "an import
// failure reports its section index and message rather than a bare status").
struct SUPERSLMUNREAL_API FSuperSLMImportDiagnostic
{
	bool bAccepted = false;

	// Mirrors superslm::SslmError::section_index. INDEX_NONE (superslm::kNoSection) when the
	// rejection did not name a specific section row, or on acceptance.
	int32 SectionIndex = INDEX_NONE;

	// Mirrors superslm::SslmError::message, or the plugin's own product-facing rewrite for a
	// cause whose remedy differs from what Layer 1's own message states (§3, v1.2.0 cell (a):
	// an unknown artifact `flags` bit is reported as "requires a newer SuperSLM" rather than
	// Layer 1's own raw "flags has unknown bit(s) set" text).
	FString Message;

	// Mirrors superslm::SslmStatusName(code) at the artifact stage -- e.g. "BadMagic",
	// "FileSizeMismatch", "IntegrityMismatch", "SectionOverlap", "BadHeader" -- and
	// superslm::SslmModelStatusName(code) at the model-load stage -- e.g. "SectionTooShort".
	// Empty string is never a valid value; an accepted import reports "Ok". ONE EXCEPTION,
	// documented rather than silent (T-2249 confirmation review N7): an allocation failure
	// (std::bad_alloc, Layer 1's public C++ throw contract) reports "SSLM_ALLOCATION_FAILED"
	// -- the C ABI's own enumerator spelling, not a C++-taxonomy name, because
	// superslm::SslmStatus (the artifact-stage enum) declares no AllocationFailed
	// enumerator at all (verified at v1.5.0:include/superslm/artifact.h) and
	// superslm::SslmModelStatus's OOM path is unreached before this arm can fire -- there is
	// no faithful C++-side name this value could mirror, so it names the ABI status the
	// underlying cause maps to instead of inventing one.
	FString StatusName;
};
