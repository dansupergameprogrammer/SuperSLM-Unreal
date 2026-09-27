#include "Modules/ModuleManager.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "SuperSLMAssetTypeActions.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "SSuperSLMQueryWindow.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SuperSLMUnrealEditor"

// L2-S3 (plan §7 item 3, §8): the query window's dockable tab, under Tools > Miscellaneous. The
// module owns the runtime host the window loads models into, so a model stays configured while
// the tab is closed and reopened; the window's Unload releases it, and module shutdown does.
namespace
{
	const FName kSuperSLMQueryWindowTabId(TEXT("SuperSLMQueryWindow"));
}

// T-2241 review C1: this module now carries real editor startup/shutdown behaviour --
// registering FAssetTypeActions_SuperSLMModel with FAssetToolsModule so USuperSLMModel has a
// Content Browser identity (icon color, category, import-type resolution alongside
// USuperSLMModelFactory) -- so FDefaultModuleImpl (T-2232's fix for the missing
// InitializeModule export, superseded here) is no longer the correct implementation. This
// module also still hosts the Private/Tests automation suite, unchanged.
class FSuperSLMUnrealEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The host first: the model's asset editor (fold-round ruling 3) dry-runs against it.
		RuntimeHost = MakeShared<FSuperSLMEditorRuntimeHost>();

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		TSharedRef<FAssetTypeActions_SuperSLMModel> Actions = MakeShared<FAssetTypeActions_SuperSLMModel>(RuntimeHost);
		AssetTools.RegisterAssetTypeActions(Actions);
		RegisteredAssetTypeActions = Actions;

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kSuperSLMQueryWindowTabId,
				FOnSpawnTab::CreateRaw(this, &FSuperSLMUnrealEditorModule::SpawnQueryWindowTab))
			.SetDisplayName(LOCTEXT("QueryWindowTab", "SuperSLM Query Window"))
			.SetTooltipText(LOCTEXT("QueryWindowTabTip", "Run a model on either backend, inspect it, and dry-run its schemas."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());
	}

	virtual void ShutdownModule() override
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kSuperSLMQueryWindowTabId);
		}
		RuntimeHost.Reset(); // an open window keeps its own reference; the last one unloads
		if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			if (TSharedPtr<FAssetTypeActions_SuperSLMModel> Actions = RegisteredAssetTypeActions.Pin())
			{
				AssetTools.UnregisterAssetTypeActions(Actions.ToSharedRef());
			}
		}
		RegisteredAssetTypeActions.Reset();
	}

private:
	TWeakPtr<FAssetTypeActions_SuperSLMModel> RegisteredAssetTypeActions;
	TSharedPtr<FSuperSLMEditorRuntimeHost> RuntimeHost;

	TSharedRef<SDockTab> SpawnQueryWindowTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SSuperSLMQueryWindow).Host(RuntimeHost)
			];
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSuperSLMUnrealEditorModule, SuperSLMUnrealEditor)
