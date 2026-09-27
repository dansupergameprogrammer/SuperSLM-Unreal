#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "SuperSLMBlueprintTypes.h"
// Complete, not forward-declared (review round 2, R2-F1): UHT emits this class's destructor into
// SuperSLMSelfCheckAsyncAction.gen.cpp, which sees only this header, and that destructor deletes
// the TUniquePtr<FSuperSLMSelfCheckRun> below -- an incomplete type there is C4150, an error in UE.
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMSelfCheckAsyncAction.generated.h"

class USuperSLMModel;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperSLMSelfCheckCompletedBP, const FSuperSLMSelfCheckReportBP&, Report);

// L2-S3 code review S3 (plan §6, §7 item 11, §8). The Blueprint route to the determinism
// self-check: a latent node that steps an FSuperSLMSelfCheckRun once per frame on the core
// ticker, each step bounded by StepBudgetMs of game-thread work, and fires Completed with the
// per-backend verdicts (USuperSLMBlueprintLibrary::ToSelfCheckReportBP) when the run is done.
// The inference runs where it always does -- the CPU backend's worker threads and the GPU
// backend's submission thread -- so the game thread never waits on it.
//
// Model must be configured on both backends, the run's own precondition. The run drives both
// subsystems' Tick() itself while it is in flight; a caller that also ticks them each frame
// should not start one while its own queries run.

/**
 * Runs the determinism self-check a bounded slice per frame and fires Completed with each
 * backend's verdict. The model must be configured on both backends. In 1.0 the GPU verdict is
 * always withheld and reads Not Yet Run.
 */
UCLASS()
class SUPERSLMUNREAL_API USuperSLMSelfCheckAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Starts the self-check on Model. StepBudgetMs bounds each frame's share of game-thread work (clamped to 0.5-16 ms). A null model completes at once with "Not run: no model". */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Run Determinism Self-Check"))
	static USuperSLMSelfCheckAsyncAction* RunDeterminismSelfCheckAsync(UObject* WorldContextObject, USuperSLMModel* Model, float StepBudgetMs = 4.0f);

	UPROPERTY(BlueprintAssignable, Category = "SuperSLM|Diagnostics")
	FSuperSLMSelfCheckCompletedBP Completed;

	//~ Begin UBlueprintAsyncActionBase interface
	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;
	//~ End UBlueprintAsyncActionBase interface

	//~ Begin UObject interface
	virtual void BeginDestroy() override;
	//~ End UObject interface

private:
	bool OnTick(float DeltaSeconds);
	void Finish(const FSuperSLMSelfCheckReportBP& Report);
	void StopTicking();

	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> Model;

	float StepBudgetMs = 4.0f;
	TUniquePtr<FSuperSLMSelfCheckRun> Run;
	FTSTicker::FDelegateHandle TickHandle;
};
