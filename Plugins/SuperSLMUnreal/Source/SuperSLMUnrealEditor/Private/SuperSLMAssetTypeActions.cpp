#include "SuperSLMAssetTypeActions.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelEditorToolkit.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_SuperSLMModel"

FText FAssetTypeActions_SuperSLMModel::GetName() const
{
	return LOCTEXT("AssetTypeActions_SuperSLMModel", "SuperSLM Model");
}

FColor FAssetTypeActions_SuperSLMModel::GetTypeColor() const
{
	return FColor(88, 156, 214); // matches the plugin's own docs/README accent, chosen for
	                              // contrast against the engine's default asset palette.
}

UClass* FAssetTypeActions_SuperSLMModel::GetSupportedClass() const
{
	return USuperSLMModel::StaticClass();
}

uint32 FAssetTypeActions_SuperSLMModel::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_SuperSLMModel::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const TSharedPtr<FSuperSLMEditorRuntimeHost> SharedHost = Host.Pin();
	if (!SharedHost.IsValid())
	{
		// No host (the module is shutting down): the engine's plain property editor.
		FAssetTypeActions_Base::OpenAssetEditor(InObjects, EditWithinLevelEditor);
		return;
	}
	const EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;
	for (UObject* Object : InObjects)
	{
		if (USuperSLMModel* Model = Cast<USuperSLMModel>(Object))
		{
			const TSharedRef<FSuperSLMModelEditorToolkit> Editor = MakeShared<FSuperSLMModelEditorToolkit>();
			Editor->Initialize(Mode, EditWithinLevelEditor, Model, SharedHost.ToSharedRef());
		}
	}
}

#undef LOCTEXT_NAMESPACE
