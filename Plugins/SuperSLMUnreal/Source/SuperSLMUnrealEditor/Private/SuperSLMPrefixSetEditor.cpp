// See SuperSLMPrefixSetEditor.h.

#include "SuperSLMPrefixSetEditor.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "ContentBrowserMenuContexts.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IAssetTools.h"
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
			// In the background (§5.8, cell 3.11): the action's progress notification reports the outcome.
			FString Error;
			if (!SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Start(*Set, Error).IsValid())
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
			LOCTEXT("BuildPrefixesTip", "Prefill each entry against the set's model on this editor's Layer 1, save it, and store the bytes the cooked tier serves."),
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

TSharedPtr<SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction> SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Start(
	USuperSLMPrefixSet& Set, FString& OutError, int32 TestPauseAfterEntries)
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
	Action->LastProgress = SuperSLMPrefixSetTools::GetBuildProgress(*Job);

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
	}), 0.1f);
	GActiveBuilds.Add(Action);
	Action->UpdateNotification(Action->LastProgress);
	return Action;
}

SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::~FSuperSLMPrefixSetBuildAction()
{
	if (!bFinished && Job.IsValid())
	{
		SuperSLMPrefixSetTools::CancelBuild(*Job);
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
	LastProgress = SuperSLMPrefixSetTools::GetBuildProgress(*Job);
	UpdateNotification(LastProgress);
	if (LastProgress.bWorkDone)
	{
		Finish();
	}
}

void SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::Cancel()
{
	if (!bFinished && Job.IsValid())
	{
		SuperSLMPrefixSetTools::CancelBuild(*Job);
	}
}

SuperSLMPrefixSetTools::FBuildProgress SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction::GetProgress() const
{
	return !bFinished && Job.IsValid() ? SuperSLMPrefixSetTools::GetBuildProgress(*Job) : LastProgress;
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
	if (IsValid(Target))
	{
		Result = SuperSLMPrefixSetTools::FinishBuildCpu(*Target, *Job, Findings, Error);
	}
	else
	{
		Result = SuperSLMPrefixSetTools::EBuildFinish::Refused;
		Error = FString::Printf(TEXT("%s: the set was deleted while its build ran; nothing was stored"), *SetName);
	}
	LastProgress = SuperSLMPrefixSetTools::GetBuildProgress(*Job);
	bFinished = true;
	Job.Reset();
	Set.Reset();

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
