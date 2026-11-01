// See SuperSLMPrefixSetEditor.h.

#include "SuperSLMPrefixSetEditor.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "ContentBrowserMenuContexts.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IAssetTools.h"
#include "Misc/Optional.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMPrefixSet.h"
#include "ToolMenus.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "SuperSLMPrefixSetEditor"

DEFINE_LOG_CATEGORY(LogSuperSLMPrefixSetCook);

namespace
{
	class FAssetTypeActions_SuperSLMPrefixSet final : public FAssetTypeActions_Base
	{
	public:
		virtual FText GetName() const override { return LOCTEXT("PrefixSetName", "SuperSLM Prefix Set"); }
		virtual FColor GetTypeColor() const override { return FColor(88, 156, 214); }
		virtual UClass* GetSupportedClass() const override { return USuperSLMPrefixSet::StaticClass(); }
		virtual uint32 GetCategories() override { return EAssetTypeCategories::Misc; }
	};

	TSharedPtr<FAssetTypeActions_SuperSLMPrefixSet> GActions;
#if WITH_DEV_AUTOMATION_TESTS
	TOptional<FString> GNextGpuLegUnavailable; // SetNextGpuLegUnavailable's one-shot reason
#endif
	// The running background builds, game thread only. An action leaves it when it finishes.
	TArray<TSharedPtr<SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction>> GActiveBuilds;
	FDelegateHandle GPreSaveHandle;
	FDelegateHandle GMenusHandle;

	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext Context)
	{
		if (!Context.IsCooking())
		{
			return;
		}
		if (const USuperSLMPrefixSet* Set = Cast<USuperSLMPrefixSet>(Object))
		{
			SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
		}
	}

	void Notify(const FText& Text, bool bOk)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 6.0f;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	void BuildSelected(const FToolMenuContext& MenuContext)
	{
		const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
		if (Context == nullptr)
		{
			return;
		}
		for (USuperSLMPrefixSet* Set : Context->LoadSelectedObjects<USuperSLMPrefixSet>())
		{
			// In the background (§5.8, cell 3.11): the action's progress notification reports the outcome. The CPU
			// entries and this editor's GPU's (step U7).
			FString Error;
			if (!SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Start(*Set, Error, /*TestPauseAfterEntries*/ -1, /*bWithGpu*/ true).IsValid())
			{
				Notify(FText::Format(LOCTEXT("Refused", "Build refused: {0}"), FText::FromString(Error)), false);
			}
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(TEXT("SuperSLMPrefixSetEditor"));
		UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(USuperSLMPrefixSet::StaticClass());
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
		Section.AddMenuEntry(TEXT("SuperSLMBuildPrefixes"),
			LOCTEXT("BuildPrefixes", "Build Prefixes"),
			LOCTEXT("BuildPrefixesTip", "Prefill each entry against the set's model on this editor's Layer 1, on the CPU and on this editor's GPU, save it, and store the bytes the cooked tier serves."),
			FSlateIcon(),
			FToolMenuExecuteAction::CreateStatic(&BuildSelected));
	}
}

void SuperSLMPrefixSetEditor::Register()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	GActions = MakeShared<FAssetTypeActions_SuperSLMPrefixSet>();
	AssetTools.RegisterAssetTypeActions(GActions.ToSharedRef());
	GPreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&OnObjectPreSave);
	GMenusHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
}

void SuperSLMPrefixSetEditor::Unregister()
{
	FSuperSLMPrefixSetBuildAction::CancelAll();
	FCoreUObjectDelegates::OnObjectPreSave.Remove(GPreSaveHandle);
	UToolMenus::UnRegisterStartupCallback(GMenusHandle);
	UToolMenus::UnregisterOwner(TEXT("SuperSLMPrefixSetEditor"));
	if (GActions.IsValid() && FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get().UnregisterAssetTypeActions(GActions.ToSharedRef());
	}
	GActions.Reset();
}

TArray<FSuperSLMPrefixSetFinding> SuperSLMPrefixSetEditor::ValidateAtCook(const USuperSLMPrefixSet& Set)
{
	TArray<FSuperSLMPrefixSetFinding> Findings;
	FString Error;
	if (!SuperSLMPrefixSetTools::ValidateForCook(Set, Findings, Error))
	{
		// Unreadable as a whole: every request recomputes, so the cook still succeeds.
		UE_LOG(LogSuperSLMPrefixSetCook, Warning, TEXT("PrefixSet cook: %s"), *Error);
		return Findings;
	}
	for (const FSuperSLMPrefixSetFinding& F : Findings)
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Warning, TEXT("PrefixSet cook: %s"), *F.Line);
	}
	return Findings;
}

bool SuperSLMPrefixSetEditor::BuildAndLog(USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError)
{
	const bool bBuilt = SuperSLMPrefixSetTools::BuildCpu(Set, OutFindings, OutError);
	LogFindings(OutFindings);
	if (!bBuilt)
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Error, TEXT("PrefixSet build refused: %s"), *OutError);
	}
	return bBuilt;
}

void SuperSLMPrefixSetEditor::LogFindings(const TArray<FSuperSLMPrefixSetFinding>& Findings)
{
	for (const FSuperSLMPrefixSetFinding& F : Findings)
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Warning, TEXT("PrefixSet build: %s"), *F.Line);
	}
}

// ---- The GPU half (step U7) ----

#if WITH_DEV_AUTOMATION_TESTS
void SuperSLMPrefixSetEditor::SetNextGpuLegUnavailable(const FString& Reason)
{
	GNextGpuLegUnavailable = Reason;
}
#endif

bool SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::CreateGameInstance(FString& OutError)
{
	if (GEngine == nullptr)
	{
		OutError = TEXT("no engine");
		return false;
	}
	// FTestWorldWrapper::CreateTestWorld(EWorldType::Game), step for step, as the query window's runtime host does: a
	// world nothing ticks, and a game instance whose Init() creates its subsystems.
	UGameInstance* NewGameInstance = NewObject<UGameInstance>(GEngine);
	World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (World == nullptr)
	{
		OutError = TEXT("UWorld::CreateWorld failed");
		return false;
	}
	World->SetShouldTick(false);
	World->AddToRoot();
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.OwningGameInstance = NewGameInstance;
	World->SetGameInstance(NewGameInstance);
	WorldContext.SetCurrentWorld(World);
	GameInstance.Reset(NewGameInstance);
	NewGameInstance->Init();
	return true;
}

SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::~FSuperSLMPrefixSetGpuLeg()
{
	Job.Reset(); // an entry in flight is the cache's lease: the teardown below releases it
	if (World == nullptr || GEngine == nullptr || !UObjectInitialized() || IsEngineExitRequested())
	{
		GameInstance.Reset();
		return;
	}
	// FTestWorldWrapper::DestroyTestWorld(), step for step: the GPU subsystem's Deinitialize() tears its state down.
	World->RemoveFromRoot();
	if (UGameInstance* Instance = World->GetGameInstance())
	{
		Instance->Shutdown();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	World = nullptr;
	GameInstance.Reset();
}

TUniquePtr<SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg> SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::Start(USuperSLMPrefixSet& Set, FString& OutReason)
{
	check(IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
	if (GNextGpuLegUnavailable.IsSet())
	{
		OutReason = GNextGpuLegUnavailable.GetValue();
		GNextGpuLegUnavailable.Reset();
		return nullptr;
	}
#endif
	if (SuperSLMPrefixSetTools::AnyGpuSubsystemConfigured())
	{
		OutReason = TEXT("a GPU subsystem is already configured in this editor (Play In Editor, or the SuperSLM query window); the GPU entries are built when none is, so no second GPU context is made beside it");
		return nullptr;
	}
	FSuperSLMGpuRuntimeConfig Config;
	FSuperSLMGpuPrefixCacheSettings CacheSettings;
	if (!SuperSLMPrefixSetTools::MakeGpuBuildConfig(Set, Config, CacheSettings, OutReason))
	{
		return nullptr;
	}
	TUniquePtr<FSuperSLMPrefixSetGpuLeg> Leg(new FSuperSLMPrefixSetGpuLeg());
	if (!Leg->CreateGameInstance(OutReason))
	{
		return nullptr;
	}
	USuperSLMGpuSubsystem* Subsystem = Leg->GameInstance->GetSubsystem<USuperSLMGpuSubsystem>();
	if (Subsystem == nullptr)
	{
		OutReason = TEXT("this editor has no GPU backend (the GPU subsystem is built on Windows x64 only)");
		return nullptr;
	}
	Subsystem->ConfigurePrefixCache(CacheSettings);
	FSuperSLMGpuConfigureReport Report = Subsystem->Configure(Set.Model.Get(), Config);
	if (Report.Result == ESuperSLMGpuConfigureResult::KBelowMinimum && Report.MinimumK > Config.K)
	{
		Config.K = Report.MinimumK; // Configure() is the authority on the floor; once, at the value it names
		Report = Subsystem->Configure(Set.Model.Get(), Config);
	}
	if (Report.Result != ESuperSLMGpuConfigureResult::Success)
	{
		OutReason = FString::Printf(TEXT("the GPU did not configure (%s)"), *Report.Message);
		return nullptr;
	}
	Leg->Job = SuperSLMPrefixSetTools::BeginBuildGpu(Set, *Subsystem, OutReason);
	if (!Leg->Job.IsValid())
	{
		return nullptr;
	}
	Leg->Gpu = Subsystem;
	return Leg;
}

bool SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::Pump()
{
	if (!Job.IsValid())
	{
		return false;
	}
	USuperSLMGpuSubsystem* Subsystem = Gpu.Get();
	if (Subsystem == nullptr)
	{
		// The game instance went away under the build: what is left of it is not built.
		SuperSLMPrefixSetTools::CancelBuild(*Job);
		return false;
	}
	return SuperSLMPrefixSetTools::PumpBuildGpu(*Job, *Subsystem);
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::Cancel()
{
	if (Job.IsValid())
	{
		SuperSLMPrefixSetTools::CancelBuild(*Job);
	}
}

bool SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::IsWorkDone() const
{
	return !Job.IsValid() || SuperSLMPrefixSetTools::GetBuildProgress(*Job).bWorkDone;
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::GetDevice(uint32& OutVendorId, uint32& OutDeviceId) const
{
	uint8 Backend = 0;
	OutVendorId = 0;
	OutDeviceId = 0;
	if (Job.IsValid())
	{
		SuperSLMPrefixSetTools::GetBuildProducer(*Job, Backend, OutVendorId, OutDeviceId);
	}
}

// ---- The action ----

TSharedPtr<SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction> SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Start(
	USuperSLMPrefixSet& Set, FString& OutError, int32 TestPauseAfterEntries, bool bWithGpu)
{
	check(IsInGameThread());
	for (const TSharedPtr<FSuperSLMPrefixSetBuildAction>& Running : GActiveBuilds)
	{
		if (Running->Set.Get() == &Set)
		{
			OutError = FString::Printf(TEXT("%s: a build of this set is already running"), *Set.GetPathName());
			UE_LOG(LogSuperSLMPrefixSetCook, Error, TEXT("PrefixSet build refused: %s"), *OutError);
			return nullptr;
		}
	}
	// The first phase, here on the game thread: the tokenize pass, the manifest, the model's mapping. No prefill.
	TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> Job = SuperSLMPrefixSetTools::BeginBuildCpu(Set, OutError);
	if (!Job.IsValid())
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Error, TEXT("PrefixSet build refused: %s"), *OutError);
		return nullptr;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (TestPauseAfterEntries >= 0)
	{
		SuperSLMPrefixSetTools::SetBuildPauseAfterEntries(*Job, TestPauseAfterEntries);
	}
#endif
	TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = MakeShareable(new FSuperSLMPrefixSetBuildAction());
	Action->Set.Reset(&Set);
	Action->SetName = Set.GetName();
	Action->Job = Job;
	Action->bBuildGpu = bWithGpu;
	if (bWithGpu)
	{
		// The GPU half, begun here too, so both builds start from the same set (FinishBuildCpuAndGpu checks it). Its
		// entries run on the GPU subsystem's submission thread; Pump() requests and ticks.
		Action->GpuLeg = FSuperSLMPrefixSetGpuLeg::Start(Set, Action->GpuNotBuiltReason);
		if (!Action->GpuLeg.IsValid())
		{
			UE_LOG(LogSuperSLMPrefixSetCook, Display, TEXT("PrefixSet build: %s: the GPU entries are not built (%s); the CPU entries are"), *Set.GetPathName(), *Action->GpuNotBuiltReason);
		}
	}
	Action->LastProgress = Action->GetProgress();

	// The progress dialog: a notification that stays until the build finishes, with a Cancel button.
	if (FSlateApplication::IsInitialized())
	{
		const TWeakPtr<FSuperSLMPrefixSetBuildAction> Weak = Action;
		FNotificationInfo Info(FText::Format(LOCTEXT("Building", "Building prefixes: {0}"), FText::FromString(Action->SetName)));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 6.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("CancelBuild", "Cancel"),
			LOCTEXT("CancelBuildTip", "Stop after the entry in hand; the set keeps its previous build."),
			FSimpleDelegate::CreateLambda([Weak]()
			{
				if (const TSharedPtr<FSuperSLMPrefixSetBuildAction> A = Weak.Pin())
				{
					A->Cancel();
				}
			}),
			SNotificationItem::CS_Pending));
		Action->Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Action->Notification.IsValid())
		{
			Action->Notification->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}

	// The entries, on a worker thread: prefill, freeze and save per entry, never on the game thread.
	Action->Worker = Async(EAsyncExecution::Thread, [Job]() { SuperSLMPrefixSetTools::RunBuildEntries(*Job); });

	const TWeakPtr<FSuperSLMPrefixSetBuildAction> WeakTick = Action;
	Action->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakTick](float)
	{
		const TSharedPtr<FSuperSLMPrefixSetBuildAction> A = WeakTick.Pin();
		if (!A.IsValid())
		{
			return false;
		}
		A->Pump();
		return !A->IsFinished();
	}), Action->GpuLeg.IsValid() ? 0.0f : 0.1f); // the GPU half moves on ticks: every frame while it runs
	GActiveBuilds.Add(Action);
	Action->UpdateNotification(Action->LastProgress);
	return Action;
}

SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::~FSuperSLMPrefixSetBuildAction()
{
	if (!bFinished && Job.IsValid())
	{
		SuperSLMPrefixSetTools::CancelBuild(*Job);
		if (GpuLeg.IsValid())
		{
			GpuLeg->Cancel();
		}
		if (Worker.IsValid())
		{
			Worker.Wait();
		}
		if (IsInGameThread())
		{
			Finish();
		}
		// Off the game thread (not expected): the job is dropped unfinished and the set is untouched.
	}
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Pump()
{
	check(IsInGameThread());
	if (bFinished || !Job.IsValid())
	{
		return;
	}
	if (GpuLeg.IsValid() && !GpuLeg->IsWorkDone())
	{
		GpuLeg->Pump();
	}
	LastProgress = GetProgress();
	UpdateNotification(LastProgress);
	// Finished once both halves are done; on a cancel, once the CPU half is (nothing is stored either way).
	const bool bCpuDone = SuperSLMPrefixSetTools::GetBuildProgress(*Job).bWorkDone;
	if (bCpuDone && (LastProgress.bWorkDone || LastProgress.bCancelRequested))
	{
		Finish();
	}
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Cancel()
{
	if (!bFinished && Job.IsValid())
	{
		SuperSLMPrefixSetTools::CancelBuild(*Job);
		if (GpuLeg.IsValid())
		{
			GpuLeg->Cancel();
		}
	}
}

SuperSLMPrefixSetTools::FBuildProgress SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::GetProgress() const
{
	if (bFinished || !Job.IsValid())
	{
		return LastProgress;
	}
	SuperSLMPrefixSetTools::FBuildProgress P = SuperSLMPrefixSetTools::GetBuildProgress(*Job);
	if (GpuLeg.IsValid())
	{
		// Both halves' entries: the CPU's and this GPU's.
		const SuperSLMPrefixSetTools::FBuildProgress G = SuperSLMPrefixSetTools::GetBuildProgress(GpuLeg->GetJob());
		P.EntriesTotal += G.EntriesTotal;
		P.EntriesDone += G.EntriesDone;
		P.bWorkDone = P.bWorkDone && G.bWorkDone;
		P.bCancelRequested = P.bCancelRequested || G.bCancelRequested;
	}
	return P;
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::CancelAll()
{
	check(IsInGameThread());
	TArray<TSharedPtr<FSuperSLMPrefixSetBuildAction>> Running = MoveTemp(GActiveBuilds);
	GActiveBuilds.Reset();
	for (const TSharedPtr<FSuperSLMPrefixSetBuildAction>& A : Running)
	{
		A->Cancel();
		if (A->Worker.IsValid())
		{
			A->Worker.Wait();
		}
		A->Pump();
	}
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Finish()
{
	check(IsInGameThread() && !bFinished && Job.IsValid());
	if (Worker.IsValid())
	{
		Worker.Wait(); // the entries' thread has set bWorkDone and is returning
	}
	USuperSLMPrefixSet* Target = Set.Get();
	if (IsValid(Target) && bBuildGpu)
	{
		// One Modify() for both builds. A GPU half that has not finished (a cancel came first) is not passed: the CPU
		// job reads Cancelled then, and nothing is stored.
		SuperSLMPrefixSetTools::FPrefixSetBuildJob* GpuJob = GpuLeg.IsValid() && GpuLeg->IsWorkDone() ? &GpuLeg->GetJob() : nullptr;
		if (GpuLeg.IsValid() && GpuJob == nullptr)
		{
			GpuNotBuiltReason = TEXT("the build was cancelled before the GPU entries finished");
		}
		Result = SuperSLMPrefixSetTools::FinishBuildCpuAndGpu(*Target, *Job, GpuJob, GpuNotBuiltReason, Findings, Error, bGpuStored);
	}
	else if (IsValid(Target))
	{
		Result = SuperSLMPrefixSetTools::FinishBuildCpu(*Target, *Job, Findings, Error);
	}
	else
	{
		Result = SuperSLMPrefixSetTools::EBuildFinish::Refused;
		Error = FString::Printf(TEXT("%s: the set was deleted while its build ran; nothing was stored"), *SetName);
	}
	LastProgress = GetProgress();
	bFinished = true;
	Job.Reset();
	Set.Reset();
	GpuLeg.Reset(); // the GPU half's game instance is torn down here

	LogFindings(Findings);
	if (Result == SuperSLMPrefixSetTools::EBuildFinish::Cancelled)
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Warning, TEXT("PrefixSet build cancelled: %s"), *Error);
	}
	else if (Result == SuperSLMPrefixSetTools::EBuildFinish::Refused)
	{
		UE_LOG(LogSuperSLMPrefixSetCook, Error, TEXT("PrefixSet build refused: %s"), *Error);
	}
	if (Notification.IsValid())
	{
		const bool bStored = Result == SuperSLMPrefixSetTools::EBuildFinish::Stored;
		Notification->SetText(bStored
			? FText::Format(LOCTEXT("Built", "Built {0} ({1} finding(s); see the Output Log)"), FText::FromString(SetName), Findings.Num())
			: Result == SuperSLMPrefixSetTools::EBuildFinish::Cancelled
				? FText::Format(LOCTEXT("Cancelled", "Cancelled {0}; it keeps its previous build"), FText::FromString(SetName))
				: FText::Format(LOCTEXT("RefusedAtFinish", "Build refused: {0}"), FText::FromString(Error)));
		Notification->SetCompletionState(bStored ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		Notification->ExpireAndFadeout();
		Notification.Reset();
	}
	// Leaves the registry last: the caller (the ticker, a cell, CancelAll) holds its own reference.
	const FSuperSLMPrefixSetBuildAction* Self = this;
	GActiveBuilds.RemoveAll([Self](const TSharedPtr<FSuperSLMPrefixSetBuildAction>& A) { return A.Get() == Self; });
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::UpdateNotification(const SuperSLMPrefixSetTools::FBuildProgress& Progress)
{
	if (Notification.IsValid())
	{
		Notification->SetText(FText::Format(Progress.bCancelRequested
				? LOCTEXT("Cancelling", "Cancelling {0}: {1} of {2} entries built")
				: LOCTEXT("Progress", "Building prefixes: {0}: {1} of {2} entries"),
			FText::FromString(SetName), Progress.EntriesDone, Progress.EntriesTotal));
	}
}

#undef LOCTEXT_NAMESPACE

#endif // SUPERSLM_WITH_PREFIX_CACHE
