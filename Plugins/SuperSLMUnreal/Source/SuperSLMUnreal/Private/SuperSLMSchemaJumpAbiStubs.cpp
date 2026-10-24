// RED STUBS for the engine's jump-forward verbs (SuperSLMSchemaJumpAbi.h), compiled only while
// SUPERSLM_WITH_SCHEMA_JUMP_FORWARD is 0. Each returns SSLM_STATUS_NEXT_FREE, which no engine path
// returns, and writes nothing. The re-vendor at engine 1.15.0 sets the switch to 1 and this file
// compiles to nothing. Kept free of Unreal headers so the host suite can compile it.

#include "SuperSLMSchemaJumpAbi.h"

#if !SUPERSLM_WITH_SCHEMA_JUMP_FORWARD

extern "C" sslm_status sslm_seq_forced_tokens(sslm_model model, sslm_seq seq, int32_t* out_ids, int32_t cap,
	int32_t* out_count, int32_t* out_accepting)
{
	(void)model;
	(void)seq;
	(void)out_ids;
	(void)cap;
	(void)out_count;
	(void)out_accepting;
	return SSLM_STATUS_NEXT_FREE;
}

extern "C" sslm_status sslm_seq_jump_forward(sslm_model model, sslm_seq seq, int32_t chunk_budget,
	const sslm_decode_params* params, const sslm_jump_options* opts, sslm_workspace ws, int32_t* out_forced,
	int32_t cap, int32_t* out_forced_count)
{
	(void)model;
	(void)seq;
	(void)chunk_budget;
	(void)params;
	(void)opts;
	(void)ws;
	(void)out_forced;
	(void)cap;
	(void)out_forced_count;
	return SSLM_STATUS_NEXT_FREE;
}

#endif // !SUPERSLM_WITH_SCHEMA_JUMP_FORWARD
