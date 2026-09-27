#pragma once

#include "CoreMinimal.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMSchemaDryRun.h"
#include "Widgets/SCompoundWidget.h"

class USuperSLMModel;

// L2-S3 (plan §7 item 1, the model and adapter inspector; §7 item 4, the schema tool's inspect and
// dry-run half). Read-only. The inspector's work -- a private mapping of the artifact, the Layer-1
// size queries and the host bandwidth measurement -- runs on the thread pool
// (SuperSLMModelInspector::InspectModelAsync()), and its result arrives on the game thread. The
// dry run vends one scratch sequence on the host's CPU subsystem and advances one subsystem Tick()
// per editor frame (FSuperSLMSchemaDryRun), so neither blocks the editor.
class SUPERSLMUNREALEDITOR_API SSuperSLMInspectionPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperSLMInspectionPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FSuperSLMEditorRuntimeHost>, Host)
		// The model to inspect: the query window's picked model.
		SLATE_ATTRIBUTE(USuperSLMModel*, Model)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSuperSLMInspectionPanel() override;

private:
	TSharedPtr<FSuperSLMEditorRuntimeHost> Host;
	TAttribute<USuperSLMModel*> Model;

	bool bInspecting = false;
	TOptional<FSuperSLMModelInspection> Inspection;
	FString InspectedModelName;

	FString AdapterPath;
	bool bInspectingAdapter = false;
	TOptional<FSuperSLMAdapterInspection> AdapterInspection;

	FString DryRunSchemaName = TEXT("potion_shop_order");
	FString DryRunPrompt = TEXT("I would like to buy a health potion, please.");
	int32 DryRunMaxNewTokens = 64;
	TUniquePtr<FSuperSLMSchemaDryRun> DryRun;
	uint32 DryRunLoadSerial = 0; // the host's load serial the scratch sequence was vended under
	TOptional<FSuperSLMSchemaDryRunReport> DryRunReport;
	FString DryRunStatus;
	TSharedPtr<FActiveTimerHandle> DryRunTimer;

	FReply OnInspectClicked();
	FReply OnInspectAdapterClicked();
	FReply OnDryRunClicked();
	FReply OnDryRunCancelClicked();
	EActiveTimerReturnType OnDryRunTick(double InCurrentTime, float InDeltaTime);
	bool CanDryRun() const;
	bool IsDryRunSequenceCurrent() const;

	FText GetInspectionText() const;
	FText GetAdapterText() const;
	FText GetDryRunText() const;
};
