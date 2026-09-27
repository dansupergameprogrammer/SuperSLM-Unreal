// T-2226 fixture -- must-REJECT for ci/check_no_damped_greedy_calls.py (guard G3, §9 dim 11).
// The mutation §10 L2-S0's gate names: a call site passing SSLM_DECODE_MODE_DAMPED_GREEDY,
// which would silently violate §2.3 RULING D-SLM3826 and, more concretely, break §4's
// caller-owned-memory promise (damped greedy grows a content-dependent anti-LM state Layer 1
// does not represent as caller workspace). This file must never appear in the plugin's real
// source tree; it exists only so the checker has a genuine violation to find.
#include "superslm/sslm_abi.h"

void ExampleDecodeSetup(sslm_model Model, sslm_decode_params* Out)
{
	sslm_decode_params_init(Model, SSLM_DECODE_MODE_DAMPED_GREEDY, /*layer_budget=*/8, Out);
}
