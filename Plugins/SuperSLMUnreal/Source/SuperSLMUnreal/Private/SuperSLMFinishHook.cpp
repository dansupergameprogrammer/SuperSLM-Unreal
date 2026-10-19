#include "SuperSLMFinishHook.h"

#include "SuperSLMSubsystem.h" // SuperSLMChannel

#include "SuperSLMParallelDecodeTestAccess.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#if WITH_DEV_AUTOMATION_TESTS
#include <atomic>

namespace
{
	std::atomic<int32> GObservedMakes{ 0 };
	std::atomic<int32> GObservedZeroBitsMakes{ 0 };
	std::atomic<int32> GObservedMatvecBitsMakes{ 0 };
	std::atomic<uint32> GObservedLastBits{ 0 };
	std::atomic<int32> GObservedRuns{ 0 };
	std::atomic<bool> GObserveHook{ false };
}

void FSuperSLMParallelDecodeTestAccess::ResetHookObservations()
{
	GObserveHook.store(false, std::memory_order_release);
	GObservedMakes.store(0, std::memory_order_relaxed);
	GObservedZeroBitsMakes.store(0, std::memory_order_relaxed);
	GObservedMatvecBitsMakes.store(0, std::memory_order_relaxed);
	GObservedLastBits.store(0, std::memory_order_relaxed);
	GObservedRuns.store(0, std::memory_order_relaxed);
	GObserveHook.store(true, std::memory_order_release);
}

void FSuperSLMParallelDecodeTestAccess::StopHookObservations() { GObserveHook.store(false, std::memory_order_release); }
int32 FSuperSLMParallelDecodeTestAccess::GetMakeCount() { return GObservedMakes.load(std::memory_order_relaxed); }
int32 FSuperSLMParallelDecodeTestAccess::GetZeroBitsMakeCount() { return GObservedZeroBitsMakes.load(std::memory_order_relaxed); }
int32 FSuperSLMParallelDecodeTestAccess::GetMatvecBitsMakeCount() { return GObservedMatvecBitsMakes.load(std::memory_order_relaxed); }
uint32 FSuperSLMParallelDecodeTestAccess::GetLastMadeBits() { return GObservedLastBits.load(std::memory_order_relaxed); }
int32 FSuperSLMParallelDecodeTestAccess::GetRunCount() { return GObservedRuns.load(std::memory_order_relaxed); }
#endif

namespace
{
	// Layer 1's `run` (parallel_for.h). ParallelFor invokes the body once for each index in
	// [0, TaskCount), with the calling thread taking part, and returns only after the last body
	// invocation completes, so Task and TaskCtx are never touched after this returns. Each
	// invocation opens SuperSLM.ParallelTask (R-S1h, restated for decode-threading D4), which
	// shows a profiling user the worker and task-graph threads for finish or projection work.
	void RunFinishTasks(void* /*HostCtx*/, int32_t TaskCount, sslm_task_fn Task, void* TaskCtx)
	{
		ParallelFor(TEXT("SuperSLM.ParallelTask"), static_cast<int32>(TaskCount), /*MinBatchSize*/ 1,
			[Task, TaskCtx](int32 Index)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.ParallelTask", SuperSLMChannel);
				Task(TaskCtx, static_cast<int32_t>(Index));
			});
	}

#if WITH_DEV_AUTOMATION_TESTS
	void RunObservedFinishTasks(void* HostCtx, int32_t TaskCount, sslm_task_fn Task, void* TaskCtx)
	{
		if (GObserveHook.load(std::memory_order_acquire))
		{
			GObservedRuns.fetch_add(1, std::memory_order_relaxed);
		}
		RunFinishTasks(HostCtx, TaskCount, Task, TaskCtx);
	}
#endif
}

sslm_parallel_for SuperSLMFinishHook::Make(int32 FinishParallelTasks, uint32 Bits)
{
	sslm_parallel_for Hook;
	Hook.run = &RunFinishTasks;
	Hook.host_ctx = nullptr;
	Hook.max_tasks = static_cast<int32_t>(FinishParallelTasks);
	Hook.reserved = Bits;
#if WITH_DEV_AUTOMATION_TESTS
	if (GObserveHook.load(std::memory_order_acquire))
	{
		GObservedLastBits.store(Hook.reserved, std::memory_order_relaxed);
		GObservedMakes.fetch_add(1, std::memory_order_relaxed);
		if (Hook.reserved == 0) { GObservedZeroBitsMakes.fetch_add(1, std::memory_order_relaxed); }
		if (Hook.reserved == SSLM_PARALLEL_FOR_MATVEC) { GObservedMatvecBitsMakes.fetch_add(1, std::memory_order_relaxed); }
		Hook.run = &RunObservedFinishTasks;
	}
#endif
	return Hook;
}
