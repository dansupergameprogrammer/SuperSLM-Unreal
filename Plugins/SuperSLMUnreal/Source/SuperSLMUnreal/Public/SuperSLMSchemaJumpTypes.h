#pragma once

// Schema jump-forward in the plugin (schema fast-decode plan rev 1.3.5, §6 slices P1 and P1-X).
//
// SUPERSLM_WITH_SCHEMA_JUMP_FORWARD says whether the vendored engine (ThirdParty/SuperSLM) carries
// the jump-forward verbs, sslm_seq_forced_tokens and the 9-argument sslm_seq_jump_forward with
// sslm_jump_options (SuperSLM 1.15.0 and later). SuperSLMUnreal.Build.cs sets it to 1 for the vendored
// 1.15.0, and the stubs compile out. At 0 (an older engine), Private/SuperSLMSchemaJumpAbi.h declares the
// verbs and the two types itself, as stubs that return SSLM_STATUS_NEXT_FREE and write nothing
// (Private/SuperSLMSchemaJumpAbiStubs.cpp), so every setting runs Off's loop and the automation cells fail
// by name. The precedent is SUPERSLM_WITH_PREFIX_CACHE.

#include "CoreMinimal.h"

#ifndef SUPERSLM_WITH_SCHEMA_JUMP_FORWARD
#define SUPERSLM_WITH_SCHEMA_JUMP_FORWARD 0
#endif

// True when the jump-forward verbs are the engine's, false when they are the stubs. Public so the
// editor module's automation cells can read it: every cell for slices P1 and P1-X checks it first, so
// a run before the re-vendor reads "the engine verbs are red stubs" rather than a token mismatch.
inline bool SuperSLMSchemaJumpVerbsAreReal() { return SUPERSLM_WITH_SCHEMA_JUMP_FORWARD != 0; }

// The setting (§6 P1-X; the ruling of 2026-10-20 16:23Z: both modes are plain options,
// default Off). Per generation, on FSuperSLMGenerationRequest::SchemaJumpForward, so one decode job
// can hold an Exact, a Canonical and an Off sequence (cell P1X.9). The GPU subsystem ignores it
// (cells P1.6, P1X.4).
//   Off       jump-forward off: today's decode, token for token.
//   Exact     exact mode (§5.10): the same tokens as Off, with drafted tokens checked in batches.
//   Canonical canonical jump-forward (§5.1-§5.6): fewer, different tokens; a new digest.
enum class ESuperSLMSchemaJumpForward : uint8
{
	Off = 0,
	Exact = 1,
	Canonical = 2,
};

// The example's scratch switch, -SuperSLMExampleJumpForward=<value> (§6 P1 and P1-X; cell P1.9; mutant
// XD7; review round 1 finding 3, a code defect against plan text). The example reads only this
// module's public API, so this is the switch's parser for it: it parses Text with the core's
// SuperSLMCore::ParseSchemaJumpSwitch, which gives that function its production caller. `0` is Off,
// `1` is Canonical (P1's on), and `off`, `exact` and `canonical` in any case are those modes: true,
// with InOutMode set. Any other value, empty or padded text included, is refused: false, with
// InOutMode unchanged. The caller logs the refusal, naming the value. Game thread or any thread.
SUPERSLMUNREAL_API bool ParseSuperSLMSchemaJumpForwardSwitch(const FString& Text, ESuperSLMSchemaJumpForward& InOutMode);
