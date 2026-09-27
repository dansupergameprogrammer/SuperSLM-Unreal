#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate.

#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMQueryWindowController.h"
#include "Widgets/SCompoundWidget.h"

class USuperSLMModel;
struct FAssetData;

// L2-S3 (the plan §7 item 3, §8; D-SLM7244). The editor query
// window's Slate presentation -- a thin view over FSuperSLMQueryWindowController
// (SuperSLMQueryWindowController.h), which carries every behavior R-S3e proves. This widget is
// NOT itself a coverage-model cell (no R-S3 row names Slate rendering); it exists so the
// demonstrator D-SLM7244 names ("the demonstrator also ships as an editor query window") has a
// real dockable-tab home, per the plan's scope ("the demonstrator's editor query
// window" is named in scope). The release review (plan §10.5) is what screenshots this widget;
// the automation tests assert nothing about it directly.
//
// L2-S3 build: the window also carries the live inspection panel (§7 item 5), the cost / budget
// readout (item 7) and the pooled-resource monitor (item 8), and hosts SSuperSLMInspectionPanel
// (items 1 and 4). Its backends come from an FSuperSLMEditorRuntimeHost
// (SuperSLMEditorRuntimeHost.h), which the Load button configures -- off the game thread since
// the fold round (ruling 1), so nothing the window does blocks the editor. A query never blocks
// the editor: Run calls the controller's BeginQuery() and an active timer calls TickQuery() once
// per editor frame until the query stops; the self-check runs the same way (BeginSelfCheck(),
// then TickSelfCheck() once per frame).
class SUPERSLMUNREALEDITOR_API SSuperSLMQueryWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperSLMQueryWindow)
		: _Model(nullptr)
	{}
		// Required: where the two backends live. Shared with the editor module, which owns it.
		SLATE_ARGUMENT(TSharedPtr<FSuperSLMEditorRuntimeHost>, Host)
		// Optional: the model the picker starts on.
		SLATE_ARGUMENT(USuperSLMModel*, Model)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSuperSLMQueryWindow() override;

private:
	TSharedPtr<FSuperSLMEditorRuntimeHost> Host;
	TSharedPtr<FSuperSLMQueryWindowController> Controller;
	TWeakObjectPtr<USuperSLMModel> PickedModel;
	TSharedPtr<FActiveTimerHandle> QueryTimer;
	TSharedPtr<FActiveTimerHandle> SelfCheckTimer;

	FSuperSLMEditorHostSettings HostSettings;
	FString PromptText;
	int32 MaxNewTokens = 64;
	FSuperSLMQueryWindowConfig Config;
	FSuperSLMQueryWindowReadout LastReadout;
	bool bHasReadout = false;
	FString StatusText;

	// The live panel's detokenized text, refreshed only when the token count moves.
	int32 LiveTokenCountShown = -1;
	FString LiveReturnedText;
	FString LiveFedText;

	FString GetPickedModelPath() const;
	void OnModelPicked(const FAssetData& AssetData);
	bool IsLoadedForPickedModel() const;
	bool IsBusy() const; // a query or the self-check is running

	FReply OnLoadClicked();
	void OnLoadFinished(bool bLoaded, const FString& Error, USuperSLMModel* Model);
	FReply OnUnloadClicked();
	FReply OnRunQueryClicked();
	FReply OnCancelClicked();
	FReply OnSelfCheckClicked();
	EActiveTimerReturnType OnQueryTick(double InCurrentTime, float InDeltaTime);
	void StopQueryTimer();
	EActiveTimerReturnType OnSelfCheckTick(double InCurrentTime, float InDeltaTime);
	void StopSelfCheckTimer();

	FText GetStatusText() const;
	FText GetReadoutText() const;
	FText GetLiveText() const;
	FText GetCostText() const;
	FText GetPoolText() const;
};
