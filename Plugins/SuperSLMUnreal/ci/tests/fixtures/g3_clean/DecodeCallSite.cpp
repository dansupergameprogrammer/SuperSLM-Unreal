// T-2226 fixture -- must-ACCEPT for ci/check_no_damped_greedy_calls.py (guard G3, §9 dim 11).
// A correct call site: mode is always SSLM_DECODE_MODE_GREEDY, matching §2.3 RULING
// D-SLM3826 (the runtime path ships greedy mode only).
#include "superslm/sslm_abi.h"

void ExampleDecodeSetup(sslm_model Model, sslm_decode_params* Out)
{
	sslm_decode_params_init(Model, SSLM_DECODE_MODE_GREEDY, /*layer_budget=*/8, Out);
}
