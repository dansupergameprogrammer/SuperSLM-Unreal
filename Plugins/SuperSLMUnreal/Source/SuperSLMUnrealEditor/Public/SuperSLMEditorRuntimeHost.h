#pragma once

#include "CoreMinimal.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMModel.h"
#include "SuperSLMRuntimeConfig.h"
#include "UObject/StrongObjectPtr.h"

class UGameInstance;
class USuperSLMGpuSubsystem;
class USuperSLMSubsystem;
class UWorld;
struct FSuperSLMModelShapeFacts;

// L2-S3 (plan §7 items 3, 5, 7, 8; §8). Where the editor's query window and inspection panel get
// their two backends. Both are UGameInstanceSubsystems (SuperSLMSubsystem.h,
// SuperSLMGpuSubsystem.h), and the editor has no GameInstance outside Play-In-Editor, so the host
// builds one: a transient Game world with its own world context and an initialized GameInstance,
// exactly the way FTestWorldWrapper::CreateTestWorld() does (Engine/Private/Tests/
// AutomationCommon.cpp), torn down the way DestroyTestWorld() does. The world never ticks and
// spawns nothing; it exists so the GameInstance's subsystem collection creates and registers the
// two subsystems. Because they are registered, the determinism self-check will drive them
// (SuperSLMDeterminismSelfCheck.h refuses a subsystem its GameInstance does not know).
//
// Configure() on each subsystem is that subsystem's own game-thread API: it builds the KV and
// workspace pools and the warm sequence list, and on the GPU creates the context and maps the
// model onto the device. The host calls it once per load, never per query or per frame; it is
// the one blocking step the editor surfaces take, and the window labels its button as such.
// Everything after it -- inference, prefill, finish -- runs on the subsystems' own worker and
// submission threads, driven one bounded Tick() per editor frame.
//
// Fold-round ruling 1 supersedes "the one blocking step": the load no longer blocks. BeginLoad()
// runs each subsystem's BeginConfigure(), whose heavy work runs off the game thread. By thread:
//  - game thread, in BeginLoad(): the shape read (the CFG1 header), the GameInstance's creation on
//    first use, each subsystem's teardown of a prior load and the capture of the model's bytes;
//  - pool thread: the CPU configure (the mapping's whole-file SHA-256 and copy, the lifecycle
//    bandwidth measurement, the KV/workspace/save allocations, the lane threads, the warm
//    sequences), then the GPU configure's host side (the artifact view, the per-layer marshal, the
//    schema parse);
//  - the GPU's submission thread: the context, the model map and the pool and warm-up token,
//    which that pool thread waits for;
//  - game thread, in later frames: each subsystem's publish, the host's next step (the GPU after
//    the CPU; a KBelowMinimum retry), and OnDone.
// The CPU is configured first and the GPU after it, so a CPU refusal fails the load before any
// device work, as before.
struct SUPERSLMUNREALEDITOR_API FSuperSLMEditorHostSettings
{
	// The pool size on both backends: the most concurrent queries the window can run.
	int32 BlockCount = 4;
	// The project's Sequence Lifecycle Budget (plan §5); Configure() refuses a model whose
	// predicted reset/adopt cost exceeds it.
	double SequenceLifecycleBudgetMs = 16.0;
	// The CPU worker's per-job budget (D-SLM7407), from which the planner derives K.
	double CpuTickBudgetMs = 8.0;
	// The GPU's pinned per-slice comparison target (plan §8: the default slice is the largest
	// whose median GPU time per slice is at most 2 ms).
	double GpuTickBudgetMs = 2.0;
	// Whether to configure the GPU backend at all.
	bool bConfigureGpu = true;
};

class SUPERSLMUNREALEDITOR_API FSuperSLMEditorRuntimeHost : public TSharedFromThis<FSuperSLMEditorRuntimeHost>
{
public:
	FSuperSLMEditorRuntimeHost() = default;
	~FSuperSLMEditorRuntimeHost();

	FSuperSLMEditorRuntimeHost(const FSuperSLMEditorRuntimeHost&) = delete;
	FSuperSLMEditorRuntimeHost& operator=(const FSuperSLMEditorRuntimeHost&) = delete;

	// Game thread; returns at once (see above). Builds the GameInstance on first use, then
	// configures the CPU subsystem with Model and, when Settings.bConfigureGpu, the GPU subsystem,
	// both off the game thread. OnDone runs on the game thread when the load ends: false with the
	// error when the CPU refused (or the load could not start); true otherwise. A GPU refusal does
	// not fail the load -- the host keeps the CPU backend and GetGpuStatus() names the GPU's
	// result, so the window can show why the GPU is absent. A later BeginLoad() or Unload()
	// supersedes a load in flight: its OnDone is not called. The host must be held by a TSharedPtr.
	void BeginLoad(USuperSLMModel& Model, const FSuperSLMEditorHostSettings& Settings, TUniqueFunction<void(bool bLoaded, const FString& Error)> OnDone);
	bool IsLoading() const { return bLoading; }

	// Game thread. Shuts the GameInstance down (each subsystem's Deinitialize() returns its
	// sequences and releases its Layer-1 handles) and destroys the world.
	void Unload();

	bool IsLoaded() const { return LoadedModel.IsValid(); }
	USuperSLMModel* GetModel() const { return LoadedModel.Get(); }
	USuperSLMSubsystem* GetCpuSubsystem() const;
	// Null unless the GPU backend is configured, active, and configured with the loaded model.
	USuperSLMGpuSubsystem* GetGpuSubsystem() const;
	const FString& GetGpuStatus() const { return GpuStatus; }
	const FSuperSLMEditorHostSettings& GetSettings() const { return Settings; }

	// Incremented by every Load() and Unload(). A sequence vended from the host is valid only while
	// the serial it was vended under is current: a re-Configure() rebuilds the pool under it.
	uint32 GetLoadSerial() const { return LoadSerial; }

	// The CPU configuration Load() applies for Shape under Settings -- also the declared shape the
	// model inspector's footprint figures are computed for (plan §7 item 1), so the two agree.
	static FSuperSLMRuntimeConfig MakeCpuConfig(const FSuperSLMModelShapeFacts& Shape, const FSuperSLMEditorHostSettings& Settings);

private:
	bool EnsureGameInstance(FString& OutError);
	void BeginGpuConfigure(uint32 Serial, const FSuperSLMGpuRuntimeConfig& GpuConfig, bool bRetryAtMinimumK);
	void FinishLoad(uint32 Serial, bool bLoaded, const FString& Error);

	TUniqueFunction<void(bool, const FString&)> PendingOnDone;
	bool bLoading = false;

	UWorld* World = nullptr; // rooted while it exists, as FTestWorldWrapper roots its world
	TStrongObjectPtr<UGameInstance> GameInstance;
	TStrongObjectPtr<USuperSLMModel> LoadedModel;
	FSuperSLMEditorHostSettings Settings;
	FString GpuStatus;
	uint32 LoadSerial = 0;
};
