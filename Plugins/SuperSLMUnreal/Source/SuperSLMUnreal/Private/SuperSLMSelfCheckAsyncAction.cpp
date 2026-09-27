#include "SuperSLMSelfCheckAsyncAction.h"

#include "SuperSLMBlueprintLibrary.h"
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMModel.h"

// Review S3. See the header. Game thread throughout: the factory and Activate() run from the
// Blueprint graph, and the core ticker fires on the game thread.

USuperSLMSelfCheckAsyncAction* USuperSLMSelfCheckAsyncAction::RunDeterminismSelfCheckAsync(
	UObject* WorldContextObject, USuperSLMModel* InModel, float InStepBudgetMs)
{
	USuperSLMSelfCheckAsyncAction* Action = NewObject<USuperSLMSelfCheckAsyncAction>();
	Action->Model = InModel;
	Action->StepBudgetMs = FMath::Clamp(InStepBudgetMs, 0.5f, 16.0f);
	// Kept alive by the game instance until Finish() calls SetReadyToDestroy(), so the run
	// survives the calling graph going away.
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void USuperSLMSelfCheckAsyncAction::Activate()
{
	check(IsInGameThread());
	if (Model == nullptr)
	{
		FSuperSLMSelfCheckReportBP Out;
		Out.ScopeText = TEXT("Not run: no model");
		Finish(Out);
		return;
	}
	// Cheap: finds the subsystems, encodes the pinned prompt, reads the shape.
	Run = MakeUnique<FSuperSLMSelfCheckRun>(*Model, FString());
	if (Run->IsDone())
	{
		Finish(USuperSLMBlueprintLibrary::ToSelfCheckReportBP(Run->GetReport()));
		return;
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &USuperSLMSelfCheckAsyncAction::OnTick));
}

bool USuperSLMSelfCheckAsyncAction::OnTick(float /*DeltaSeconds*/)
{
	if (!Run.IsValid())
	{
		TickHandle.Reset();
		return false;
	}
	if (!Run->Step(StepBudgetMs / 1000.0))
	{
		return true; // more to do; the next frame steps again
	}
	const FSuperSLMSelfCheckReportBP Out = USuperSLMBlueprintLibrary::ToSelfCheckReportBP(Run->GetReport());
	Run.Reset();
	TickHandle.Reset(); // returning false removes this ticker
	Finish(Out);
	return false;
}

void USuperSLMSelfCheckAsyncAction::Finish(const FSuperSLMSelfCheckReportBP& Report)
{
	Completed.Broadcast(Report);
	SetReadyToDestroy();
}

void USuperSLMSelfCheckAsyncAction::StopTicking()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
}

void USuperSLMSelfCheckAsyncAction::SetReadyToDestroy()
{
	StopTicking();
	// Destroying a run mid-way returns the sequence it holds (FSuperSLMSelfCheckRun).
	Run.Reset();
	Super::SetReadyToDestroy();
}

void USuperSLMSelfCheckAsyncAction::BeginDestroy()
{
	StopTicking();
	Run.Reset();
	Super::BeginDestroy();
}
