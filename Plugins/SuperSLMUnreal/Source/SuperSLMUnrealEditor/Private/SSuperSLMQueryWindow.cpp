#include "SSuperSLMQueryWindow.h"

#include "AssetRegistry/AssetData.h"
#include "PropertyCustomizationHelpers.h"
#include "SSuperSLMInspectionPanel.h"
#include "Styling/CoreStyle.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SSuperSLMQueryWindow"

// L2-S3 build (plan §7 items 3, 5, 7, 8; §8). Every figure the window shows names its source and
// its backend. Nothing here runs inference: the controller's TickQuery() calls one subsystem
// Tick() per editor frame, and the subsystems run the work on their own threads.
namespace
{
	// §11's measured slack of the declared GPU residency (D-SLM7685), stated beside the figure.
	const TCHAR* const kGpuResidencySlack =
		TEXT("Declared residency is below what the device occupies. On an RTX 2080 SUPER (one process after Configure(), one pooled sequence, RoPE tables declared) ")
		TEXT("the gap, computed from recorded occupancy readings rather than measured by this window, was 0.5B 1.81% head off / 0.03% head on; 1.5B 0.39% / 0.003%. ")
		TEXT("The gap is allocation granules, the device head's extra, and the first Configure()'s warm-up workspace. ")
		TEXT("Not measured on other devices or in a process that renders; the test SuperSLM.U1.Gpu.DeclaredResidencyAgainstDxgi logs this device's declared and occupied bytes.");

	FString Mib(int64 Bytes)
	{
		return FString::Printf(TEXT("%.2f MiB (%lld B)"), static_cast<double>(Bytes) / (1024.0 * 1024.0), Bytes);
	}

	const TCHAR* VerdictName(ESuperSLMSelfCheckVerdictBP Verdict)
	{
		switch (Verdict)
		{
		case ESuperSLMSelfCheckVerdictBP::Verified: return TEXT("Verified");
		case ESuperSLMSelfCheckVerdictBP::Diverged: return TEXT("Diverged");
		default: return TEXT("Not yet run");
		}
	}

	const TCHAR* StopReasonName(const TOptional<ESuperSLMQueryStopReason>& Reason)
	{
		if (!Reason.IsSet())
		{
			return TEXT("(not classified: checkbox off)");
		}
		switch (Reason.GetValue())
		{
		case ESuperSLMQueryStopReason::Completed: return TEXT("Completed");
		case ESuperSLMQueryStopReason::SchemaRejected: return TEXT("SchemaRejected");
		default: return TEXT("BudgetExhausted");
		}
	}

	const TCHAR* PhaseName(ESuperSLMSequencePhase Phase)
	{
		switch (Phase)
		{
		case ESuperSLMSequencePhase::Prefilling: return TEXT("Prefilling");
		case ESuperSLMSequencePhase::Decoding: return TEXT("Decoding");
		case ESuperSLMSequencePhase::Complete: return TEXT("Complete");
		case ESuperSLMSequencePhase::Faulted: return TEXT("Faulted");
		default: return TEXT("Idle");
		}
	}

	TSharedRef<SWidget> Labelled(const FText& Label, const TSharedRef<SWidget>& Control)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(Label).MinDesiredWidth(150.0f)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Control
			];
	}

	TSharedRef<SWidget> Readout(const TAttribute<FText>& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.AutoWrapText(true);
	}
}

SSuperSLMQueryWindow::~SSuperSLMQueryWindow()
{
	// The host outlives this widget (it is a member), so a running query's sequences go back to
	// pools that still exist.
	if (Controller.IsValid() && Host.IsValid() && Host->IsLoaded())
	{
		Controller->CancelSelfCheck(); // returns a self-check's sequence the same way
		Controller->CancelQuery(); // returns the sequences to their pools
	}
}

void SSuperSLMQueryWindow::Construct(const FArguments& InArgs)
{
	Host = InArgs._Host;
	check(Host.IsValid());
	PickedModel = InArgs._Model;
	PromptText = TEXT("I would like to buy a health potion, please.");
	StatusText = TEXT("Pick a model and press Load.");

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8)
		[
			SNew(SVerticalBox)

			// --- The model and the backends it is loaded on ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("Model", "Model"),
					SNew(SObjectPropertyEntryBox)
					.AllowedClass(USuperSLMModel::StaticClass())
					.ObjectPath(this, &SSuperSLMQueryWindow::GetPickedModelPath)
					.OnObjectChanged(this, &SSuperSLMQueryWindow::OnModelPicked)
					.AllowClear(false)
					.DisplayThumbnail(false))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("BlockCount", "Pool size (both backends)"),
					SNew(SSpinBox<int32>)
					.MinValue(1).MaxValue(16)
					.Value_Lambda([this] { return HostSettings.BlockCount; })
					.OnValueChanged_Lambda([this](int32 V) { HostSettings.BlockCount = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("LifecycleBudget", "Sequence Lifecycle Budget (ms)"),
					SNew(SSpinBox<double>)
					.MinValue(0.1).MaxValue(1000.0)
					.Value_Lambda([this] { return HostSettings.SequenceLifecycleBudgetMs; })
					.OnValueChanged_Lambda([this](double V) { HostSettings.SequenceLifecycleBudgetMs = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return HostSettings.bConfigureGpu ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { HostSettings.bConfigureGpu = S == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("ConfigureGpu", "Configure the GPU backend"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Load", "Load (configures both backends off the game thread)"))
					.IsEnabled_Lambda([this] { return PickedModel.IsValid() && !Host->IsLoading() && !IsBusy(); })
					.OnClicked(this, &SSuperSLMQueryWindow::OnLoadClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Unload", "Unload"))
					.IsEnabled_Lambda([this] { return Host->IsLoaded() || Host->IsLoading(); })
					.OnClicked(this, &SSuperSLMQueryWindow::OnUnloadClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMQueryWindow::GetStatusText))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SSeparator)
			]

			// --- The query and the four §8 controls, plus the checkbox ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SMultiLineEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(PromptText); })
				.OnTextChanged_Lambda([this](const FText& T) { PromptText = T.ToString(); })
				.HintText(LOCTEXT("PromptHint", "Prompt (tokenized as typed; no chat template)"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("Backend", "Backend"),
					SNew(SSegmentedControl<ESuperSLMBackendBP>)
					.Value_Lambda([this] { return Config.Backend; })
					.OnValueChanged_Lambda([this](ESuperSLMBackendBP B) { Config.Backend = B; })
					+ SSegmentedControl<ESuperSLMBackendBP>::Slot(ESuperSLMBackendBP::CPU).Text(LOCTEXT("CPU", "CPU"))
					+ SSegmentedControl<ESuperSLMBackendBP>::Slot(ESuperSLMBackendBP::GPU).Text(LOCTEXT("GPU", "GPU")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("Concurrent", "Concurrent queries"),
					SNew(SSpinBox<int32>)
					.MinValue(1).MaxValue(16)
					.Value_Lambda([this] { return Config.ConcurrentQueries; })
					.OnValueChanged_Lambda([this](int32 V) { Config.ConcurrentQueries = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("FrameBudget", "Frame Budget (layers; 0 = whole token)"),
					SNew(SSpinBox<int32>)
					.MinValue(0).MaxValue(128)
					.Value_Lambda([this] { return Config.FrameBudgetLayers; })
					.OnValueChanged_Lambda([this](int32 V) { Config.FrameBudgetLayers = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("K", "k (fixed-tick latency)"),
					SNew(SSpinBox<int32>)
					.MinValue(1).MaxValue(4096)
					.Value_Lambda([this] { return Config.RequestedK; })
					.OnValueChanged_Lambda([this](int32 V) { Config.RequestedK = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				Labelled(LOCTEXT("MaxNewTokens", "Max new tokens"),
					SNew(SSpinBox<int32>)
					.MinValue(1).MaxValue(4096)
					.Value_Lambda([this] { return MaxNewTokens; })
					.OnValueChanged_Lambda([this](int32 V) { MaxNewTokens = V; }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Config.bSchemaConstrainedDecoding ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Config.bSchemaConstrainedDecoding = S == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("Checkbox", "Schema-constrained decoding (binds prompt_result)"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Run", "Run query"))
					.IsEnabled_Lambda([this] { return IsLoadedForPickedModel() && !IsBusy(); })
					.OnClicked(this, &SSuperSLMQueryWindow::OnRunQueryClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.IsEnabled_Lambda([this] { return IsBusy(); })
					.OnClicked(this, &SSuperSLMQueryWindow::OnCancelClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("SelfCheck", "Run determinism self-check"))
					.IsEnabled_Lambda([this] { return IsLoadedForPickedModel() && !IsBusy(); })
					.OnClicked(this, &SSuperSLMQueryWindow::OnSelfCheckClicked)
				]
			]

			// --- §8's readout ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("ReadoutTitle", "Readout"))
				.BodyContent()[ Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMQueryWindow::GetReadoutText)) ]
			]
			// --- §7 item 5 ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("LiveTitle", "Live inspection (read-only)"))
				.BodyContent()[ Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMQueryWindow::GetLiveText)) ]
			]
			// --- §7 item 7 ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("CostTitle", "Cost / budget"))
				.BodyContent()[ Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMQueryWindow::GetCostText)) ]
			]
			// --- §7 item 8 ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("PoolTitle", "Pooled resources"))
				.BodyContent()[ Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMQueryWindow::GetPoolText)) ]
			]
			// --- §7 items 1 and 4 ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("InspectTitle", "Model inspector and schema dry-run"))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SSuperSLMInspectionPanel)
					.Host(Host)
					.Model_Lambda([this] { return PickedModel.Get(); })
				]
			]
		]
	];
}

FString SSuperSLMQueryWindow::GetPickedModelPath() const
{
	return PickedModel.IsValid() ? PickedModel->GetPathName() : FString();
}

void SSuperSLMQueryWindow::OnModelPicked(const FAssetData& AssetData)
{
	// Loading the asset object is the editor's own asset load (the import already validated its
	// bytes); nothing is mapped or configured until Load.
	PickedModel = Cast<USuperSLMModel>(AssetData.GetAsset());
}

bool SSuperSLMQueryWindow::IsLoadedForPickedModel() const
{
	return Controller.IsValid() && Host->IsLoaded() && !Host->IsLoading() && Host->GetModel() == PickedModel.Get();
}

bool SSuperSLMQueryWindow::IsBusy() const
{
	return Controller.IsValid() && (Controller->IsQueryRunning() || Controller->IsSelfCheckRunning());
}

FReply SSuperSLMQueryWindow::OnLoadClicked()
{
	USuperSLMModel* Model = PickedModel.Get();
	if (Model == nullptr)
	{
		return FReply::Handled();
	}
	StopQueryTimer();
	StopSelfCheckTimer();
	Controller.Reset(); // cancels a running query or self-check, returning its sequences
	bHasReadout = false;

	// Fold-round ruling 1: the load runs off the game thread (FSuperSLMEditorRuntimeHost's header
	// names each step's thread); the editor keeps rendering, and OnLoadFinished() runs on the game
	// thread when it ends.
	StatusText = FString::Printf(TEXT("Loading %s: configuring the CPU backend, then the GPU, off the game thread..."), *Model->GetName());
	const TWeakPtr<SSuperSLMQueryWindow> WeakWindow = StaticCastSharedRef<SSuperSLMQueryWindow>(AsShared());
	const TWeakObjectPtr<USuperSLMModel> WeakModel(Model);
	Host->BeginLoad(*Model, HostSettings, [WeakWindow, WeakModel](bool bLoaded, const FString& Error)
	{
		if (const TSharedPtr<SSuperSLMQueryWindow> Window = WeakWindow.Pin())
		{
			Window->OnLoadFinished(bLoaded, Error, WeakModel.Get());
		}
	});
	return FReply::Handled();
}

void SSuperSLMQueryWindow::OnLoadFinished(bool bLoaded, const FString& Error, USuperSLMModel* Model)
{
	if (!bLoaded || Model == nullptr || Host->GetCpuSubsystem() == nullptr)
	{
		StatusText = FString::Printf(TEXT("Load failed: %s"), *Error);
		return;
	}
	// The checkbox alone decides what a query binds (review N4, SuperSLMQuery.h).
	Controller = MakeShared<FSuperSLMQueryWindowController>(*Host->GetCpuSubsystem(), Host->GetGpuSubsystem(), *Model);
	StatusText = FString::Printf(TEXT("Loaded %s. CPU backend configured. %s"), *Model->GetName(), *Host->GetGpuStatus());
}

FReply SSuperSLMQueryWindow::OnUnloadClicked()
{
	StopQueryTimer();
	StopSelfCheckTimer();
	Controller.Reset(); // returns a running query's sequences while the pools still exist
	bHasReadout = false;
	Host->Unload();
	StatusText = TEXT("Unloaded; both backends released.");
	return FReply::Handled();
}

FReply SSuperSLMQueryWindow::OnRunQueryClicked()
{
	if (!IsLoadedForPickedModel())
	{
		return FReply::Handled();
	}
	FString Error;
	if (!Controller->BeginQuery(PromptText, MaxNewTokens, Config, Error))
	{
		StatusText = FString::Printf(TEXT("Query refused: %s"), *Error);
		return FReply::Handled();
	}
	StatusText = TEXT("Running...");
	LiveTokenCountShown = -1;
	LiveReturnedText.Reset();
	LiveFedText.Reset();
	// Period 0: once per editor frame, the frame the subsystems' Tick() contract is written for.
	QueryTimer = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SSuperSLMQueryWindow::OnQueryTick));
	return FReply::Handled();
}

FReply SSuperSLMQueryWindow::OnCancelClicked()
{
	StopQueryTimer();
	StopSelfCheckTimer();
	if (Controller.IsValid())
	{
		Controller->CancelQuery();
		Controller->CancelSelfCheck();
	}
	StatusText = TEXT("Cancelled.");
	return FReply::Handled();
}

FReply SSuperSLMQueryWindow::OnSelfCheckClicked()
{
	if (!IsLoadedForPickedModel())
	{
		return FReply::Handled();
	}
	FString Error;
	if (!Controller->BeginSelfCheck(Error))
	{
		StatusText = FString::Printf(TEXT("Self-check did not run: %s"), *Error);
		return FReply::Handled();
	}
	// Fold-round ruling 1: one bounded step per editor frame (OnSelfCheckTick), never the whole run.
	StatusText = TEXT("Self-check running (32 steps x 2 granularities per backend, a few ms per frame)...");
	SelfCheckTimer = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SSuperSLMQueryWindow::OnSelfCheckTick));
	return FReply::Handled();
}

EActiveTimerReturnType SSuperSLMQueryWindow::OnSelfCheckTick(double InCurrentTime, float InDeltaTime)
{
	// The game thread's share of one frame: the subsystems' Tick() and the run's bookkeeping. The
	// inference runs on the backends' own threads meanwhile.
	constexpr double kSelfCheckStepBudgetSeconds = 0.004;
	if (!Controller.IsValid() || !Controller->IsSelfCheckRunning())
	{
		SelfCheckTimer.Reset();
		return EActiveTimerReturnType::Stop;
	}
	bool bRan = false;
	FString Error;
	if (!Controller->TickSelfCheck(kSelfCheckStepBudgetSeconds, bRan, Error))
	{
		return EActiveTimerReturnType::Continue;
	}
	SelfCheckTimer.Reset();
	StatusText = bRan
		? FString(TEXT("Self-check ran; its verdicts show beside the digest after the next query."))
		: FString::Printf(TEXT("Self-check did not run: %s"), *Error);
	return EActiveTimerReturnType::Stop;
}

void SSuperSLMQueryWindow::StopSelfCheckTimer()
{
	if (SelfCheckTimer.IsValid())
	{
		UnRegisterActiveTimer(SelfCheckTimer.ToSharedRef());
		SelfCheckTimer.Reset();
	}
}

void SSuperSLMQueryWindow::StopQueryTimer()
{
	if (QueryTimer.IsValid())
	{
		UnRegisterActiveTimer(QueryTimer.ToSharedRef());
		QueryTimer.Reset();
	}
}

EActiveTimerReturnType SSuperSLMQueryWindow::OnQueryTick(double InCurrentTime, float InDeltaTime)
{
	if (!Controller.IsValid() || !Controller->IsQueryRunning())
	{
		QueryTimer.Reset();
		return EActiveTimerReturnType::Stop;
	}
	bool bSucceeded = false;
	FSuperSLMQueryWindowReadout QueryReadout;
	FString Error;
	if (Controller->TickQuery(InDeltaTime, bSucceeded, QueryReadout, Error))
	{
		QueryTimer.Reset();
		if (bSucceeded)
		{
			LastReadout = MoveTemp(QueryReadout);
			bHasReadout = true;
			StatusText = LastReadout.bMovedBackend ? LastReadout.MovedBackendMessage : FString(TEXT("Done."));
		}
		else
		{
			StatusText = FString::Printf(TEXT("Query failed: %s"), *Error);
		}
		return EActiveTimerReturnType::Stop;
	}

	// The live panel's text, detokenized only when a token has arrived since the last frame.
	FSuperSLMQueryLiveView View;
	if (Controller->GetLiveView(View) && View.GeneratedTokens.Num() != LiveTokenCountShown && Host->GetModel() != nullptr)
	{
		LiveTokenCountShown = View.GeneratedTokens.Num();
		FString Ignored;
		if (LiveFedText.IsEmpty())
		{
			SuperSLM::DetokenizeTokens(*Host->GetModel(), View.PromptTokens, LiveFedText, Ignored);
		}
		SuperSLM::DetokenizeTokens(*Host->GetModel(), View.GeneratedTokens, LiveReturnedText, Ignored);
	}
	return EActiveTimerReturnType::Continue;
}

FText SSuperSLMQueryWindow::GetStatusText() const
{
	return FText::FromString(StatusText);
}

FText SSuperSLMQueryWindow::GetReadoutText() const
{
	if (!bHasReadout)
	{
		return LOCTEXT("NoReadout", "No query has finished yet.");
	}
	const FSuperSLMQueryWindowReadout& R = LastReadout;
	TStringBuilder<2048> B;
	B.Appendf(TEXT("Backend run:            %s%s\n"), R.BackendRun == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU"),
		R.bMovedBackend ? TEXT(" (moved from the GPU after a device loss)") : TEXT(""));
	B.Appendf(TEXT("Answer:                 %s\n"), *R.DisplayedText);
	B.Appendf(TEXT("Stop reason:            %s\n"), StopReasonName(R.StopReason));
	B.Appendf(TEXT("Schema accepting @stop: %s\n"), R.SchemaAcceptingAtStop.IsSet() ? (R.SchemaAcceptingAtStop.GetValue() ? TEXT("yes") : TEXT("no")) : TEXT("(not read: checkbox off)"));
	B.Appendf(TEXT("Structured result:      %s\n"), R.ResultJson.IsEmpty() ? TEXT("(none)") : *R.ResultJson);
	B.Appendf(TEXT("Raw output:             %s\n"), *R.RawOutputText);
	B.Appendf(TEXT("Generated tokens:       %d\n"), R.GeneratedTokens.Num());
	B.Appendf(TEXT("Token digest (SHA-256, decode_digest.h): %s\n"), *R.TokenDigestHex);
	B.Appendf(TEXT("Self-check (backend run): %s -- %s\n"), VerdictName(R.SelfCheckVerdict), *R.SelfCheckScopeText);
	B.Appendf(TEXT("Frames to answer:       %d\n"), R.FramesToAnswer);
	B.Appendf(TEXT("k: requested %d, floor %d, effective %d; applied by the scheduler %d\n"), Config.RequestedK, R.MinimumK, R.EffectiveK, R.AppliedK);
	if (R.bScheduleNotApplied)
	{
		B.Appendf(TEXT("%s\n"), *R.ScheduleMessage); // review W4
	}
	return FText::FromString(FString(B.ToView()));
}

FText SSuperSLMQueryWindow::GetLiveText() const
{
	FSuperSLMQueryLiveView View;
	if (!Controller.IsValid() || !Controller->GetLiveView(View))
	{
		return LOCTEXT("NoLive", "No query is running.");
	}
	TStringBuilder<2048> B;
	B.Appendf(TEXT("Backend: %s%s   phase: %s   frames: %d\n"), View.Backend == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU"),
		View.bMovedBackend ? TEXT(" (moved)") : TEXT(""), PhaseName(View.Phase), View.Frames);
	B.Appendf(TEXT("Context fed (%d tokens): %s\n"), View.PromptTokens.Num(), *LiveFedText);
	B.Appendf(TEXT("Tokens returned (%d): %s\n"), View.GeneratedTokens.Num(), *LiveReturnedText);
	if (View.bSchemaBound)
	{
		B.Appendf(TEXT("Schema: prompt_result bound; accepting: %s"), View.bSchemaAccepting ? TEXT("yes") : TEXT("no"));
		if (View.ForcedTokenCount >= 0)
		{
			B.Appendf(TEXT("; forced tokens: %lld (CPU GetStats)"), View.ForcedTokenCount);
		}
		B.Append(TEXT("\n"));
	}
	else
	{
		B.Append(TEXT("Schema: none bound (checkbox off)\n"));
	}
	return FText::FromString(FString(B.ToView()));
}

FText SSuperSLMQueryWindow::GetCostText() const
{
	TStringBuilder<2048> B;
	const USuperSLMSubsystem* Cpu = Host->IsLoaded() ? Host->GetCpuSubsystem() : nullptr;
	const USuperSLMGpuSubsystem* Gpu = Host->IsLoaded() ? Host->GetGpuSubsystem() : nullptr;
	if (bHasReadout)
	{
		const FSuperSLMQueryWindowReadout& R = LastReadout;
		if (R.BackendRun == ESuperSLMBackendBP::GPU)
		{
			// D-SLM7379: on the composed path the prompt's prefill is sliced like decode, so its
			// time spans many ticks; only the one-call path submits it at once.
			B.Appendf(TEXT("[GPU] prefill (time to first token): %.3f ms (%s, plugin clock)\n"), R.PromptTimeMs,
				R.AppliedLayersPerTick <= 0 ? TEXT("one call") : TEXT("sliced like decode, across ticks"));
			B.Appendf(TEXT("[GPU] GPU ms per slice (last): %.3f ms (Layer 1 timestamps, gpu_busy_ms)\n"), R.GpuMsPerSliceMs);
			B.Appendf(TEXT("[GPU] slices sampled: %d\n"), R.GpuBusyMsSamplesPerSlice.Num());
			B.Appendf(TEXT("[GPU] per-frame headroom: %.3f ms (pinned slice budget %.3f ms - last slice)\n"),
				Host->GetSettings().GpuTickBudgetMs - R.GpuMsPerSliceMs, Host->GetSettings().GpuTickBudgetMs);
			B.Appendf(TEXT("[GPU] host finish ms per token: %.3f ms (the finish call's wall time, including the device-head dispatch)\n"), R.HostFinishMsPerTokenMs);
			B.Appendf(TEXT("[GPU] dispatch shape: %s\n"), R.AppliedLayersPerTick <= 0 ? TEXT("one call per token") : *FString::Printf(TEXT("%d layers per tick, shared by the query's sequences"), R.AppliedLayersPerTick));
			B.Appendf(TEXT("[GPU] K in ticks: %d (the window's k, applied before the vend)\n"), R.AppliedK);
		}
		else
		{
			B.Appendf(TEXT("[CPU] worker ms per job: %.3f ms (plugin clock, FSuperSLMWorkerJobReport::WorkerCallMs)\n"), R.WorkerMsPerJobMs);
			B.Appendf(TEXT("[CPU] K in ticks: %d (planner-derived)\n"), R.AppliedK);
			B.Appendf(TEXT("[CPU] layer budget: %s\n"), Config.FrameBudgetLayers <= 0 ? TEXT("whole depth") : *FString::Printf(TEXT("%d layers"), Config.FrameBudgetLayers));
		}
		B.Appendf(TEXT("[%s] tokens per second: %.2f\n"), R.BackendRun == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU"), R.TokensPerSecond);
		if (R.BackendRun == ESuperSLMBackendBP::GPU)
		{
			B.Appendf(TEXT("[GPU] hitches this query: %d\n"), R.HitchCount);
		}
		else
		{
			// Worker-side counts, not game-thread time (plan §5 "What counts as a hitch").
			B.Appendf(TEXT("[CPU] worker hitches this query: %d (%d late past K x budget, %d over the %.1f ms budget; not game-thread time)\n"),
				R.HitchCount, R.CpuLateJobs, R.CpuOverBudgetJobs, Host->GetSettings().CpuTickBudgetMs);
		}
	}
	if (Cpu != nullptr)
	{
		B.Appendf(TEXT("[CPU] last Tick(): %.3f ms; tokens per second (GetTokensPerSecond): %.2f; finish parallel tasks: %d\n"),
			Cpu->GetLastTickDurationMs(), Cpu->GetTokensPerSecond(), FSuperSLMRuntimeConfig().FinishParallelTasks);
	}
	if (Gpu != nullptr)
	{
		B.Appendf(TEXT("[GPU] head: %s; finish parallel tasks: %d\n"), *Gpu->GetDeviceHeadStatus(), FSuperSLMGpuRuntimeConfig().FinishParallelTasks);
		B.Appendf(TEXT("[GPU] declared residency: %s -- device-local buffers (lower bound)\n"), *Mib(Gpu->GetDeclaredGpuResidencyBytes()));
	}
	if (B.Len() == 0)
	{
		return LOCTEXT("NoCost", "Load a model and run a query.");
	}
	return FText::FromString(FString(B.ToView()));
}

FText SSuperSLMQueryWindow::GetPoolText() const
{
	const USuperSLMSubsystem* Cpu = Host->IsLoaded() ? Host->GetCpuSubsystem() : nullptr;
	const USuperSLMGpuSubsystem* Gpu = Host->IsLoaded() ? Host->GetGpuSubsystem() : nullptr;
	if (Cpu == nullptr)
	{
		return LOCTEXT("NoPool", "Load a model to see its pools.");
	}
	TStringBuilder<2048> B;
	B.Appendf(TEXT("[CPU] sequences: %d occupied, %d free\n"), Cpu->GetPoolOccupiedCount(), Cpu->GetPoolFreeCount());
	B.Appendf(TEXT("[CPU] KV pool reserved: %s\n"), *Mib(Cpu->GetKvPoolReservedBytes()));
	B.Appendf(TEXT("[CPU] workspace reserved: %s\n"), *Mib(Cpu->GetWorkspaceReservedBytes()));
	if (Gpu == nullptr)
	{
		B.Appendf(TEXT("[GPU] %s\n"), *Host->GetGpuStatus());
	}
	else
	{
		B.Appendf(TEXT("[GPU] sequences: %d occupied, %d free, %d withheld\n"), Gpu->GetPoolOccupiedCount(), Gpu->GetPoolFreeCount(), Gpu->GetWithheldSlotCount());
		B.Appendf(TEXT("[GPU] declared residency: %s -- device-local buffers (lower bound)\n"), *Mib(Gpu->GetDeclaredGpuResidencyBytes()));
		B.Appendf(TEXT("[GPU] %s\n"), kGpuResidencySlack);
		const int64 Occupancy = Gpu->GetLocalVideoMemoryUsageBytes();
		if (Occupancy >= 0)
		{
			B.Appendf(TEXT("[GPU] process local video memory in use (DXGI CurrentUsage, the whole process): %s\n"), *Mib(Occupancy));
		}
	}
	return FText::FromString(FString(B.ToView()));
}

#undef LOCTEXT_NAMESPACE
