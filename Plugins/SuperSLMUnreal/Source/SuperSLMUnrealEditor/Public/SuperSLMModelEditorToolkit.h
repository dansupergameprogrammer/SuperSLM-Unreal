#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/GCObject.h"

class FSuperSLMEditorRuntimeHost;
class IDetailsView;
class SDockTab;
class USuperSLMModel;

// L2-S3 (the plan §10.4, "the asset editors"; fold-round ruling 3). The
// asset editor a USuperSLMModel opens in -- the plugin's one asset type at L2-S3 (an adapter is a
// file the inspector reads by path, not an asset). Two tabs:
//  - Details: the asset's own properties (the provenance text, read-only, and the GPU
//    device-resident-head switch, plan §3), in the engine's details view, so an edit is the
//    engine's own transacted, undoable property edit;
//  - Inspector: SSuperSLMInspectionPanel on this asset -- the model inspector's full readout (§7
//    item 1), the adapter inspector, and the schema dry-run (§7 item 4) against the editor's shared
//    runtime host, the same host the query window loads into.
// Nothing here blocks the editor: the inspector runs on the thread pool and the dry run advances
// one subsystem Tick() per frame (SSuperSLMInspectionPanel.h).
class SUPERSLMUNREALEDITOR_API FSuperSLMModelEditorToolkit : public FAssetEditorToolkit, public FGCObject
{
public:
	void Initialize(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, USuperSLMModel* InModel,
		const TSharedRef<FSuperSLMEditorRuntimeHost>& InHost);

	//~ Begin FAssetEditorToolkit
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	//~ End FAssetEditorToolkit

	//~ Begin FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End FGCObject

private:
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnInspectorTab(const FSpawnTabArgs& Args);

	TObjectPtr<USuperSLMModel> Model;
	TSharedPtr<FSuperSLMEditorRuntimeHost> Host;
	TSharedPtr<IDetailsView> DetailsView;
};
