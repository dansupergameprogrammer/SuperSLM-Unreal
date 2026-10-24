#pragma once

// The engine's jump-forward verbs as the plugin calls them (schema fast-decode plan rev 1.3.4, §5.3,
// §5.10.2). With SUPERSLM_WITH_SCHEMA_JUMP_FORWARD 1 the vendored engine declares them, and this
// header only includes it. With 0 (until the re-vendor at 1.15.0) this header declares them, copied
// from SuperSLM's include/superslm/sslm_abi.h and sslm_abi_functions.inc at f03a5358, and
// SuperSLMSchemaJumpAbiStubs.cpp defines RED STUBS: each returns SSLM_STATUS_NEXT_FREE, a value no
// engine path returns, and writes nothing. The plugin wiring therefore compiles and its cells fail by
// name before the re-vendor. Kept free of Unreal headers so the host suite can compile it.

// The switch, defaulted here as SuperSLMSchemaJumpTypes.h defaults it (that header pulls in
// CoreMinimal, which this one must not).
#ifndef SUPERSLM_WITH_SCHEMA_JUMP_FORWARD
#define SUPERSLM_WITH_SCHEMA_JUMP_FORWARD 0
#endif

#include "superslm/sslm_abi.h"

#if !SUPERSLM_WITH_SCHEMA_JUMP_FORWARD

#ifdef __cplusplus
extern "C" {
#endif

/* --- Copied from SuperSLM f03a5358, include/superslm/sslm_abi.h --- */
typedef enum sslm_jump_mode { SSLM_JUMP_CANONICAL = 0, SSLM_JUMP_EXACT = 1 } sslm_jump_mode;

typedef struct sslm_jump_options {
    uint32_t       struct_size;  /* sizeof(sslm_jump_options) */
    int32_t        mode;         /* sslm_jump_mode */
    const int32_t* draft;        /* EXACT only. NULL: the canonical chain from the state after the pending token */
    int32_t        draft_count;  /* EXACT only. >= 0; at most chunk_budget - 1 tokens are used */
} sslm_jump_options;

/* --- Copied from SuperSLM f03a5358, include/superslm/sslm_abi_functions.inc (without SUPERSLM_API:
 * these are the plugin's own red stubs, not imports) --- */
sslm_status sslm_seq_forced_tokens(sslm_model model, sslm_seq seq,
                                   int32_t* out_ids, int32_t cap, int32_t* out_count,
                                   int32_t* out_accepting);

sslm_status sslm_seq_jump_forward(sslm_model model, sslm_seq seq, int32_t chunk_budget,
                                  const sslm_decode_params* params, /* NULL = greedy */
                                  const sslm_jump_options* opts,     /* NULL = canonical */
                                  sslm_workspace ws,
                                  int32_t* out_forced, int32_t cap, int32_t* out_forced_count);

#ifdef __cplusplus
}
#endif

#endif // !SUPERSLM_WITH_SCHEMA_JUMP_FORWARD

// SuperSLMSchemaJumpVerbsAreReal() (SuperSLMSchemaJumpTypes.h) reads the switch for the cells.
