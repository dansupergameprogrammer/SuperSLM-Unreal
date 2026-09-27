#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class FSuperSLMEditorRuntimeHost;

// T-2241 review C1: asset-type registration §10 L2-S0's gate names alongside the import
// factory. Without this, USuperSLMModel has no Content Browser identity -- no icon color, no
// category, and no entry in the "Create Asset" / import-type resolution the factory above
// otherwise reaches only by class match.
class FAssetTypeActions_SuperSLMModel : public FAssetTypeActions_Base
{
public:
	// L2-S3 (fold-round ruling 3): the model opens in FSuperSLMModelEditorToolkit
	// (SuperSLMModelEditorToolkit.h), whose inspector tab dry-runs against the editor module's
	// shared runtime host. Held weakly: the module owns the host.
	FAssetTypeActions_SuperSLMModel() = default;
	explicit FAssetTypeActions_SuperSLMModel(const TWeakPtr<FSuperSLMEditorRuntimeHost>& InHost) : Host(InHost) {}

	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	using FAssetTypeActions_Base::OpenAssetEditor;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;

private:
	TWeakPtr<FSuperSLMEditorRuntimeHost> Host;
};
