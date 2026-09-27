#pragma once

#include "CoreMinimal.h"

// L2-S2 (the red-suite record §4). ONE bridge, inside the
// runtime module `SuperSLMUnreal` (D-SLM7337 -- the GPU backend compiles into this
// module, Win64-guarded, rather than a separate one), for Layer 1's
// superslm::ComputeTokenDigest (decode_digest.h, read at v1.5.0) -- the same "a plain
// UE-facing wrapper is the whole surface a caller needs" shape SuperSLMIntegrity.h's
// ComputeArtifactDigest already established. R-S2e's own oracle ("the composed path's
// token digest equals the one-call path's") and the determinism self-check (§7 item 11,
// SuperSLMDeterminismSelfCheck.h) both need the SAME digest function Layer 1 defines --
// never a parallel reimplementation this suite or the plugin computes on its own, which
// would let the two sides of every comparison drift from Layer 1's own definition of
// "identical."
//
// decode_digest.h's ComputeTokenDigest takes only `tokens`/`token_count`, never a
// sequence's internal state -- this wrapper's own signature mirrors that exactly, taking
// a plain TArray<int32> (UE's own generated-tokens container shape, matching
// USuperSLMSubsystem::GetGeneratedTokens()'s return type) rather than a raw pointer pair.
//
// Implementation note:
// SuperSLMGpuDigest::ComputeTokenDigest calls superslm::ComputeTokenDigest DIRECTLY --
// it needs no second wrapping and no forwarder, because `src/decode_digest.cpp` is
// already compiled into this same module via `Private/Vendored/
// SuperSLMVendored_decode_digest.cpp` (D-SLM7337, settling what T-2816 §6 left open as a
// build-wiring choice between two options; there is now only one, because there is only
// one module).
namespace SuperSLMGpuDigest
{
	SUPERSLMUNREAL_API void ComputeTokenDigest(const TArray<int32>& Tokens, uint8 OutDigest[32]);

	// Byte-for-byte hex comparison, matching the shipped reference file's own
	// hex-digest field shape (SuperSLMDeterminismSelfCheck.h) -- a small helper so no
	// test hand-rolls hex formatting for a 32-byte digest.
	SUPERSLMUNREAL_API FString DigestToHex(const uint8 Digest[32]);
}

