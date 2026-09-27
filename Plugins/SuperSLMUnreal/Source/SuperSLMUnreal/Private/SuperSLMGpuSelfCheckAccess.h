#pragma once

#include "CoreMinimal.h"
#include "SuperSLMGpuTypes.h"

class USuperSLMGpuSubsystem;
class USuperSLMModel;

// L2-S2 (plan §7 item 11). The determinism self-check drives the GPU subsystem through the few
// internals its workload needs and the public surface does not carry: finding the subsystem a
// model is configured into, and running one sequence at a granularity finer than the
// subsystem's configured per-tick budget (the composed path at one layer per slice). Defined in
// SuperSLMGpuSubsystem.cpp; declared a friend of USuperSLMGpuSubsystem.
struct FSuperSLMSelfCheckAccess
{
	// The active GPU subsystem whose most recent successful Configure() mapped Model, or null.
	static USuperSLMGpuSubsystem* FindGpuSubsystemFor(const USuperSLMModel& Model);

	// The model's own decoder layer count on the GPU mapping (0 if not configured).
	static int32 GetNumHiddenLayers(const USuperSLMGpuSubsystem& Gpu);

	// SslmGpuSchemaLookupForG5Bridge against Gpu's own mapped model handle: the schema's index,
	// -1 on no match, or -2 when Gpu holds no mapped model. Also backs
	// FSuperSLMGpuSchemaLookup::LookupByName, which is not a friend of the subsystem.
	static int32 LookupSchemaIndex(const USuperSLMGpuSubsystem& Gpu, const FString& SchemaName);

	// Vends one sequence on Path, binds SchemaName (empty: none), generates up to MaxNewTokens
	// greedy tokens from Prompt, and returns it. LayersPerSliceOverride > 0 caps the composed
	// path's layers per slice for this sequence below the configured per-tick budget.
	// bOutDeadEnd reports a schema dead end (the GPU twin of the CPU's -2), which ends the
	// workload exactly as it ends the CPU's. False with OutError when the run could not complete.
	static bool RunGeneration(
		USuperSLMGpuSubsystem& Gpu,
		ESuperSLMGpuDecodePath Path,
		int32 LayersPerSliceOverride,
		const TArray<int32>& Prompt,
		const FString& SchemaName,
		int32 MaxNewTokens,
		TArray<int32>& OutTokens,
		bool& bOutDeadEnd,
		FString& OutError);

	// Fold-round ruling 1: RunGeneration() in steps, so the self-check can advance by a bounded
	// slice of ticks per editor frame instead of holding the game thread for the whole run.
	// BeginGeneration() vends, caps the slicing, binds SchemaName and requests the generation; on
	// false nothing is left vended. OutMaxTicks is RunGeneration()'s own tick bound for the run.
	// StepGeneration() ticks Gpu once: 0 while running, 1 when finished (OutTokens and bOutDeadEnd
	// set as RunGeneration() sets them), -1 on a fault (OutError). EndGeneration() returns the
	// sequence. RunGeneration() is exactly these three in a loop. Game thread.
	static bool BeginGeneration(
		USuperSLMGpuSubsystem& Gpu,
		ESuperSLMGpuDecodePath Path,
		int32 LayersPerSliceOverride,
		const TArray<int32>& Prompt,
		const FString& SchemaName,
		int32 MaxNewTokens,
		FSuperSLMGpuSequence& OutSequence,
		int64& OutMaxTicks,
		FString& OutError);
	static int32 StepGeneration(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence, TArray<int32>& OutTokens, bool& bOutDeadEnd, FString& OutError);
	static void EndGeneration(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Sequence);

	// T-2818 nonblock, restated by the ruling of 2026-09-26 (plan §2.5 row 21): forwards to the
	// subsystem's NextTickGatedOnDevice(). Tick() no longer waits; this is true when the device is
	// the gate for the next tick -- an event due by then whose job the submission thread has not
	// finished, a vended sequence whose admitted queued op (reset, save, bind) has an unfinished
	// job, or K or more tick jobs outstanding so the cap would bind -- so a tick taken now would
	// make no progress. The stepped self-check asks this before
	// each StepGeneration() and, on true, returns to its caller so real time passes; RunGeneration(),
	// the headless query loop and the test access's TickWhenDeviceReady() sleep 1 ms instead. False
	// when the next tick is what makes progress, including when it takes the device-loss path (the
	// backend already unresponsive, or the Layer-1 call's bound overrun). Reads only. Game thread.
	static bool NextTickWouldWaitOnDevice(const USuperSLMGpuSubsystem& Gpu);
};
