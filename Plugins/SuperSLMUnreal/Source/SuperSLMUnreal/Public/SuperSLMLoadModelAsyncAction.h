#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "SuperSLMLoadModelAsyncAction.generated.h"

class USuperSLMModel;
class USuperSLMSubsystem;
class USuperSLMGpuSubsystem;
struct FSuperSLMConfigureReport;
struct FSuperSLMGpuConfigureReport;

/** What Load SuperSLM Model reports on either pin. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMLoadModelResultBP
{
	GENERATED_BODY()

	// D-SLM7382: a GPU query that loses its device re-runs on the CPU.

	/** The CPU backend is configured with the model. Every query needs it: Create Query requires it, and a GPU query that loses its device re-runs there. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Load")
	bool bCpuConfigured = false;

	/** The GPU backend is active with the model. False when it was not asked for, or refused. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Load")
	bool bGpuBackendActive = false;

	/** On Failed, why, naming the refusal. On Loaded, the GPU's state when it was asked for. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM|Load")
	FString Message;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperSLMLoadModelDoneBP, const FSuperSLMLoadModelResultBP&, Result);

// Box-session product gap (T-2818 box record §9 items 14-15; maintainer ruling,
// pending entry 19): 1.0's one Blueprint-facing interface (D-SLM3534, plan §6) includes loading a
// model, so a Blueprint-only game can configure both backends. A latent node over
// USuperSLMSubsystem::BeginConfigure() and then USuperSLMGpuSubsystem::BeginConfigure(), in that
// order as the editor host does, so a CPU refusal fails the load before any device work. Every
// heavy step runs where BeginConfigure() puts it (pool threads and the GPU submission thread);
// the game thread does the teardown, the byte capture and the publish, never the load.
//
// Loaded fires when the CPU backend is configured; the GPU's outcome is in the result (a GPU
// refusal leaves the CPU usable, matching the editor host). Failed fires, naming why, when:
// there is no model or no GameInstance; the model has no readable shape; a backend about to be
// reconfigured still holds a vended sequence, a CPU prefix or a mapped GPU adapter, or is
// mid-configure (refused before anything is torn down: a reconfigure destroys them all); the CPU
// refuses; or a later configure or the subsystem's shutdown superseded this one. The GPU is
// checked again after the CPU phase, just before its own teardown: if something was vended or
// mapped on it meanwhile, it is left untouched and Loaded fires with the CPU only, naming why.

/**
 * Loads a model into the CPU backend and, when asked, the GPU backend, off the game thread.
 * Loaded fires when the CPU backend is configured, with the GPU's outcome in the result; Failed
 * fires, naming why, when the load is refused.
 */
UCLASS()
class SUPERSLMUNREAL_API USuperSLMLoadModelAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	// PoolSize is the most concurrent sequences on each backend (BlockCount). CpuTickBudgetMs is
	// the CPU worker's per-job budget, from which the planner derives K (D-SLM7407); GpuTickBudgetMs
	// the GPU's per-slice target. SequenceLifecycleBudgetMs is the project's Sequence Lifecycle
	// Budget (plan §5), which the CPU configure checks the model's predicted reset/adopt against.
	// PrefixBlockCount is how many CPU shared prefixes (CreatePrefix) can be live at once: the CPU
	// KV pool reserves that many blocks beyond PoolSize (D-SLM7342). It defaults to 0 (no prefixes),
	// matching FSuperSLMRuntimeConfig, so a load reserves no extra KV block unless asked: set it >0
	// to use shared prefixes. Negative, or PoolSize + PrefixBlockCount past INT32_MAX, is refused by
	// name by Configure(). The GPU config has no prefix blocks, so it applies to the CPU only.

	/**
	 * Loads Model. PoolSize: the most concurrent sequences on each backend. CpuTickBudgetMs: the
	 * CPU worker's time budget per job. GpuTickBudgetMs: the GPU's time target per slice.
	 * SequenceLifecycleBudgetMs: the budget the model's predicted reset and adopt costs are
	 * checked against. PrefixBlockCount: how many CPU shared prefixes can be live at once (0, the
	 * default, reserves none).
	 */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Load",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Load SuperSLM Model"))
	static USuperSLMLoadModelAsyncAction* LoadSuperSLMModelAsync(UObject* WorldContextObject, USuperSLMModel* Model,
		bool bConfigureGpu = true, int32 PoolSize = 4, float CpuTickBudgetMs = 16.6f, float GpuTickBudgetMs = 2.0f,
		float SequenceLifecycleBudgetMs = 16.0f, int32 PrefixBlockCount = 0);

	UPROPERTY(BlueprintAssignable, Category = "SuperSLM|Load")
	FSuperSLMLoadModelDoneBP Loaded;

	UPROPERTY(BlueprintAssignable, Category = "SuperSLM|Load")
	FSuperSLMLoadModelDoneBP Failed;

	//~ Begin UBlueprintAsyncActionBase interface
	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;
	//~ End UBlueprintAsyncActionBase interface

	//~ Begin UObject interface
	virtual void BeginDestroy() override;
	//~ End UObject interface

private:
	void OnCpuConfigured(const FSuperSLMConfigureReport& Report);
	void BeginGpu(int32 K, bool bRetryAtMinimumK);
	void OnGpuConfigured(const FSuperSLMGpuConfigureReport& Report, int32 K, bool bRetryAtMinimumK);
	// Watches for a superseded BeginConfigure(), whose OnDone is never called.
	bool OnTick(float DeltaSeconds);
	void Finish(bool bLoaded, const FString& Message);
	void StopTicking();

	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> Model;
	TWeakObjectPtr<UObject> WorldContext;
	TWeakObjectPtr<USuperSLMSubsystem> Cpu;
	TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;

	bool bConfigureGpu = true;
	int32 PoolSize = 4;
	int32 PrefixBlockCount = 0;
	double CpuTickBudgetMs = 16.6;
	double GpuTickBudgetMs = 2.0;
	double SequenceLifecycleBudgetMs = 16.0;
	int32 NumHiddenLayers = 0;
	int64 ContextCap = 0;

	bool bCpuConfigured = false;
	bool bFinished = false;
	// Held only by the pending BeginConfigure()'s OnDone: expired with no callback means the
	// subsystem discarded that configure (a later one, or Deinitialize(), superseded it).
	TWeakPtr<bool, ESPMode::ThreadSafe> PendingToken;
	FTSTicker::FDelegateHandle TickHandle;
};
