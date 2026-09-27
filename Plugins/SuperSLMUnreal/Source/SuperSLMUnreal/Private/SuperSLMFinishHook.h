#pragma once

#include "CoreMinimal.h"

#include "superslm/parallel_for.h"

// Layer 1's host parallel-for hook (1.7.0; the plan §2.5 row 2, §5 CPU
// path item 7, D-SLM7657, D-SLM7663). Only the token finish's logits step reads it: its rows are
// split into at most FinishParallelTasks blocks and handed to one plugin-owned `run`, which calls
// UE's ParallelFor. The calling thread (a CPU worker lane, the GPU submission thread, or the
// calibration command) takes part and waits for the rest, so the hook meets Layer 1's contract:
// every index in [0, task_count) is invoked exactly once, and `run` returns only after the last
// invocation has returned (Engine/Source/Runtime/Core/Public/Async/ParallelFor.h). Tokens are
// identical at every setting and with no hook.
//
// One definition serves the CPU workspaces, the GPU context and the calibration command, so
// FinishCostMs is measured on the same finish the runtime runs.
namespace SuperSLMFinishHook
{
	// The accepted range of FinishParallelTasks on both runtime configs: 0 to Layer 1's own
	// SSLM_PARALLEL_FOR_MAX_TASKS (256).
	constexpr int32 MaxFinishParallelTasks = SSLM_PARALLEL_FOR_MAX_TASKS;
	inline bool IsValidTaskCount(int32 FinishParallelTasks)
	{
		return FinishParallelTasks >= 0 && FinishParallelTasks <= MaxFinishParallelTasks;
	}

	// At 0 or 1 the plugin installs no hook and the finish is serial.
	inline bool ShouldInstall(int32 FinishParallelTasks)
	{
		return FinishParallelTasks > 1;
	}

	// The hook the plugin installs: run = the ParallelFor bridge, host_ctx = null, max_tasks =
	// FinishParallelTasks. Holds no state; Layer 1 copies it into the workspace or context.
	sslm_parallel_for Make(int32 FinishParallelTasks);
}
