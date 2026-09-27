#pragma once

#include "CoreMinimal.h"

// T-2256 round 11 fix (first build of round 10's MM-5): the ONE bridge across the module boundary
// for Layer 1's own integrity hash. MM-5 (D-SLM4006, section 9 dim 9(e)) reports the
// real-scale artifact's SHA-256 alongside its measurements, and D-SLM4006 requires that hash
// to be THE artifact-integrity semantics -- Layer 1's own superslm::Sha256Hash -- never a
// parallel implementation. The vendored tree is pinned and unaltered (D-SLM3812), so its
// headers carry no export annotations and the function links only inside the module that
// compiles the vendored sources (this one); MM-5 lives in the automation-test module, whose
// link saw LNK2019. This exported forwarder gives sibling modules access without compiling a
// second copy of vendored code or editing the pinned tree.
//
// Throws std::bad_alloc exactly per Layer 1's documented throw contract (S-HARDEN-7): callers
// inside exception-disabled modules must treat an escape as fatal, identical to every other
// Layer 1 entry point this plugin already consumes.
namespace SuperSLM
{
	SUPERSLMUNREAL_API void ComputeArtifactDigest(const uint8* Data, int64 SizeBytes, uint8 OutDigest[32]);
}
