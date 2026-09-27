#pragma once

// Two compile-time switches the plugin's sources and tests read. Both are always 1 in 1.0.
//
// SUPERSLM_WITH_L2S3 guards the Blueprint-surface test files (plain .cpp files, which
// UnrealHeaderTool does not parse) in their `#if WITH_DEV_AUTOMATION_TESTS && ...` guards. It
// guards no reflected declaration: UnrealHeaderTool rejects a USTRUCT/UCLASS inside an #if it does
// not know.
#ifndef SUPERSLM_WITH_L2S3
#define SUPERSLM_WITH_L2S3 1
#endif

// SUPERSLM_WITH_L2S1_ASYNC selects the CPU subsystem's queued API (handle-returning
// ResetSequence/AdoptPrefix/SaveSequence/RestoreSequence, the per-job ledger and the per-tick
// history; D-SLM7403/D-SLM7407/D-SLM7414/D-SLM7421). It is the only shape the plugin declares;
// the earlier synchronous shape was removed (D-SLM7457). The GPU subsystem's queued API
// (SuperSLMGpuSubsystem.h: the Request* calls and their result reads) needs no switch: it is the
// only lifecycle API the GPU subsystem exposes (D-SLM7418/D-SLM7421/D-SLM7424/D-SLM7457).
#ifndef SUPERSLM_WITH_L2S1_ASYNC
#define SUPERSLM_WITH_L2S1_ASYNC 1
#endif
