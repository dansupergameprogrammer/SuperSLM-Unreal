#include "SuperSLMModelEditorToolkit.h"

#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SSuperSLMInspectionPanel.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMModel.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SScrollBox.h"

#define LOCTEXT_NAMESPACE "SuperSLMModelEditorToolkit"

// The layout and the details view follow the engine's own single-asset toolkits
// (FUsdAssetCacheAssetEditorToolkit, USDClassesEditor), with a second tab for the inspector.
namespace
{
	const FName kSuperSLMModelEditorAppId(TEXT("SuperSLMModelEditor"));
	const FName kDetailsTabId(TEXT("SuperSLMModelEditor_Details"));
	const FName kInspectorTabId(TEXT("SuperSLMModelEditor_Inspector"));
}

void FSuperSLMModelEditorToolkit::Initialize(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, USuperSLMModel* InModel,
	const TSharedRef<FSuperSLMEditorRuntimeHost>& InHost)
{
	Model = InModel;
	Host = InHost;

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(InModel);

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("Standalone_SuperSLMModelEditor_v1")
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.35f)->AddTab(kDetailsTabId, ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.65f)->AddTab(kInspectorTabId, ETabState::OpenedTab)
			)
		);

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;
	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, kSuperSLMModelEditorAppId, Layout,
		bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, InModel);
}

FName FSuperSLMModelEditorToolkit::GetToolkitFName() const
{
	return kSuperSLMModelEditorAppId;
}

FText FSuperSLMModelEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("AppLabel", "SuperSLM Model Editor");
}

FString FSuperSLMModelEditorToolkit::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "SuperSLM Model ").ToString();
}

FLinearColor FSuperSLMModelEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor(FColor(88, 156, 214)); // FAssetTypeActions_SuperSLMModel's own type color
}

void FSuperSLMModelEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "SuperSLM Model Editor"));
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(kDetailsTabId, FOnSpawnTab::CreateSP(this, &FSuperSLMModelEditorToolkit::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
	InTabManager->RegisterTabSpawner(kInspectorTabId, FOnSpawnTab::CreateSP(this, &FSuperSLMModelEditorToolkit::SpawnInspectorTab))
		.SetDisplayName(LOCTEXT("InspectorTab", "Inspector"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FSuperSLMModelEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(kDetailsTabId);
	InTabManager->UnregisterTabSpawner(kInspectorTabId);
}

TSharedRef<SDockTab> FSuperSLMModelEditorToolkit::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId().TabType == kDetailsTabId);
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTab", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FSuperSLMModelEditorToolkit::SpawnInspectorTab(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId().TabType == kInspectorTabId);
	// The panel reads the model through an attribute; a weak pointer, so a deleted asset reads
	// null and the panel's buttons disable rather than touch a dead object.
	const TWeakObjectPtr<USuperSLMModel> WeakModel(Model);
	return SNew(SDockTab)
		.Label(LOCTEXT("InspectorTab", "Inspector"))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(8)
			[
				SNew(SSuperSLMInspectionPanel)
				.Host(Host)
				.Model_Lambda([WeakModel] { return WeakModel.Get(); })
			]
		];
}

void FSuperSLMModelEditorToolkit::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Model);
}

FString FSuperSLMModelEditorToolkit::GetReferencerName() const
{
	return TEXT("FSuperSLMModelEditorToolkit");
}

#undef LOCTEXT_NAMESPACE
