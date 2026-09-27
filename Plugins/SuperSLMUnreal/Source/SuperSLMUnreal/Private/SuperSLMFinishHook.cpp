#include "SuperSLMFinishHook.h"

#include "SuperSLMSubsystem.h" // SuperSLMChannel

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// Layer 1's `run` (parallel_for.h). ParallelFor invokes the body once for each index in
	// [0, TaskCount), with the calling thread taking part, and returns only after the last body
	// invocation completes, so Task and TaskCtx are never touched after this returns. Each
	// invocation opens SuperSLM.FinishTask (plan §5.1), which is what shows a profiling user the
	// worker and task-graph threads the finish's logits actually ran on.
	void RunFinishTasks(void* /*HostCtx*/, int32_t TaskCount, sslm_task_fn Task, void* TaskCtx)
	{
		ParallelFor(TEXT("SuperSLM.FinishTask"), static_cast<int32>(TaskCount), /*MinBatchSize*/ 1,
			[Task, TaskCtx](int32 Index)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.FinishTask", SuperSLMChannel);
				Task(TaskCtx, static_cast<int32_t>(Index));
			});
	}
}

sslm_parallel_for SuperSLMFinishHook::Make(int32 FinishParallelTasks)
{
	sslm_parallel_for Hook;
	Hook.run = &RunFinishTasks;
	Hook.host_ctx = nullptr;
	Hook.max_tasks = static_cast<int32_t>(FinishParallelTasks);
	Hook.reserved = 0;
	return Hook;
}
