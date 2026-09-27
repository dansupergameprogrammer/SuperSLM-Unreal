#include "SSuperSLMInspectionPanel.h"

#include "Styling/CoreStyle.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SSuperSLMInspectionPanel"

namespace
{
	FString Mib(int64 Bytes)
	{
		return FString::Printf(TEXT("%.2f MiB (%lld B)"), static_cast<double>(Bytes) / (1024.0 * 1024.0), Bytes);
	}

	TSharedRef<SWidget> Readout(const TAttribute<FText>& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.AutoWrapText(true);
	}

	const TCHAR* StopReasonName(const TOptional<ESuperSLMQueryStopReason>& Reason)
	{
		if (!Reason.IsSet())
		{
			return TEXT("(unclassified)");
		}
		switch (Reason.GetValue())
		{
		case ESuperSLMQueryStopReason::Completed: return TEXT("Completed");
		case ESuperSLMQueryStopReason::SchemaRejected: return TEXT("SchemaRejected");
		default: return TEXT("BudgetExhausted");
		}
	}
}

SSuperSLMInspectionPanel::~SSuperSLMInspectionPanel()
{
	// The host is a member declared before DryRun, so it outlives the scratch sequence's return;
	// a sequence from an earlier load went back with its pool and is only forgotten.
	if (DryRun.IsValid() && !IsDryRunSequenceCurrent())
	{
		DryRun->Abandon();
	}
}

bool SSuperSLMInspectionPanel::IsDryRunSequenceCurrent() const
{
	return Host.IsValid() && Host->IsLoaded() && Host->GetLoadSerial() == DryRunLoadSerial;
}

void SSuperSLMInspectionPanel::Construct(const FArguments& InArgs)
{
	Host = InArgs._Host;
	check(Host.IsValid());
	Model = InArgs._Model;

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- §7 item 1: the model inspector ---
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SButton)
			.Text(LOCTEXT("Inspect", "Inspect model"))
			.IsEnabled_Lambda([this] { return Model.Get() != nullptr && !bInspecting; })
			.OnClicked(this, &SSuperSLMInspectionPanel::OnInspectClicked)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMInspectionPanel::GetInspectionText))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
		[
			SNew(SSeparator)
		]

		// --- §7 item 1: the adapter inspector ---
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 4, 0)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("AdapterHint", "Absolute path to an adapter file"))
				.Text_Lambda([this] { return FText::FromString(AdapterPath); })
				.OnTextChanged_Lambda([this](const FText& T) { AdapterPath = T.ToString(); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("InspectAdapter", "Inspect adapter"))
				.IsEnabled_Lambda([this] { return Model.Get() != nullptr && !AdapterPath.IsEmpty() && !bInspectingAdapter; })
				.OnClicked(this, &SSuperSLMInspectionPanel::OnInspectAdapterClicked)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMInspectionPanel::GetAdapterText))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
		[
			SNew(SSeparator)
		]

		// --- §7 item 4: the schema dry-run ---
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("SchemaHint", "Schema name (one the inspector lists)"))
			.Text_Lambda([this] { return FText::FromString(DryRunSchemaName); })
			.OnTextChanged_Lambda([this](const FText& T) { DryRunSchemaName = T.ToString(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("DryRunPromptHint", "Prompt for the scratch sequence"))
			.Text_Lambda([this] { return FText::FromString(DryRunPrompt); })
			.OnTextChanged_Lambda([this](const FText& T) { DryRunPrompt = T.ToString(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("DryRunMax", "Max new tokens"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpinBox<int32>)
				.MinValue(1).MaxValue(4096)
				.Value_Lambda([this] { return DryRunMaxNewTokens; })
				.OnValueChanged_Lambda([this](int32 V) { DryRunMaxNewTokens = V; })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("DryRun", "Dry-run on a scratch CPU sequence"))
				.IsEnabled(this, &SSuperSLMInspectionPanel::CanDryRun)
				.OnClicked(this, &SSuperSLMInspectionPanel::OnDryRunClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("DryRunCancel", "Cancel"))
				.IsEnabled_Lambda([this] { return DryRun.IsValid() && DryRun->IsRunning(); })
				.OnClicked(this, &SSuperSLMInspectionPanel::OnDryRunCancelClicked)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			Readout(TAttribute<FText>::CreateSP(this, &SSuperSLMInspectionPanel::GetDryRunText))
		]
	];
}

FReply SSuperSLMInspectionPanel::OnInspectClicked()
{
	USuperSLMModel* Target = Model.Get();
	FSuperSLMModelShapeFacts Shape;
	if (Target == nullptr || !SuperSLMModelInspector::ReadShape(*Target, Shape))
	{
		Inspection.Reset();
		InspectedModelName = TEXT("(the model carries no readable CFG1 section)");
		return FReply::Handled();
	}
	bInspecting = true;
	InspectedModelName = Target->GetName();
	// The footprint is computed for the shape the query window's Load would configure.
	const FSuperSLMRuntimeConfig Declared = FSuperSLMEditorRuntimeHost::MakeCpuConfig(Shape, Host->GetSettings());
	TWeakPtr<SSuperSLMInspectionPanel> Weak = StaticCastSharedRef<SSuperSLMInspectionPanel>(AsShared());
	SuperSLMModelInspector::InspectModelAsync(*Target, Declared,
		[Weak](const FSuperSLMModelInspection& Result)
		{
			if (TSharedPtr<SSuperSLMInspectionPanel> Panel = Weak.Pin())
			{
				Panel->Inspection = Result;
				Panel->bInspecting = false;
				if (Result.SchemaNames.Num() > 0 && !Result.SchemaNames.Contains(Panel->DryRunSchemaName))
				{
					Panel->DryRunSchemaName = Result.SchemaNames[0];
				}
			}
		});
	return FReply::Handled();
}

FReply SSuperSLMInspectionPanel::OnInspectAdapterClicked()
{
	USuperSLMModel* Base = Model.Get();
	if (Base == nullptr)
	{
		return FReply::Handled();
	}
	bInspectingAdapter = true;
	TWeakPtr<SSuperSLMInspectionPanel> Weak = StaticCastSharedRef<SSuperSLMInspectionPanel>(AsShared());
	SuperSLMModelInspector::InspectAdapterAsync(*Base, AdapterPath,
		[Weak](const FSuperSLMAdapterInspection& Result)
		{
			if (TSharedPtr<SSuperSLMInspectionPanel> Panel = Weak.Pin())
			{
				Panel->AdapterInspection = Result;
				Panel->bInspectingAdapter = false;
			}
		});
	return FReply::Handled();
}

bool SSuperSLMInspectionPanel::CanDryRun() const
{
	return Model.Get() != nullptr && Host->IsLoaded() && Host->GetModel() == Model.Get() &&
		!(DryRun.IsValid() && DryRun->IsRunning());
}

FReply SSuperSLMInspectionPanel::OnDryRunClicked()
{
	if (!CanDryRun())
	{
		return FReply::Handled();
	}
	DryRun = MakeUnique<FSuperSLMSchemaDryRun>(*Host->GetCpuSubsystem(), *Host->GetModel());
	DryRunLoadSerial = Host->GetLoadSerial();
	DryRunReport.Reset();
	FString Error;
	if (!DryRun->Begin(DryRunSchemaName, DryRunPrompt, DryRunMaxNewTokens, Error))
	{
		DryRunStatus = FString::Printf(TEXT("Dry run refused: %s"), *Error);
		DryRun.Reset();
		return FReply::Handled();
	}
	DryRunStatus = TEXT("Running...");
	DryRunTimer = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SSuperSLMInspectionPanel::OnDryRunTick));
	return FReply::Handled();
}

FReply SSuperSLMInspectionPanel::OnDryRunCancelClicked()
{
	if (DryRunTimer.IsValid())
	{
		UnRegisterActiveTimer(DryRunTimer.ToSharedRef());
		DryRunTimer.Reset();
	}
	if (DryRun.IsValid() && !IsDryRunSequenceCurrent())
	{
		DryRun->Abandon();
	}
	DryRun.Reset(); // returns the scratch sequence
	DryRunStatus = TEXT("Cancelled.");
	return FReply::Handled();
}

EActiveTimerReturnType SSuperSLMInspectionPanel::OnDryRunTick(double InCurrentTime, float InDeltaTime)
{
	if (!DryRun.IsValid() || !IsDryRunSequenceCurrent())
	{
		if (DryRun.IsValid())
		{
			DryRun->Abandon(); // the reload returned it with the pool
		}
		DryRun.Reset();
		DryRunTimer.Reset();
		DryRunStatus = TEXT("The model was reloaded under the dry run; it was stopped.");
		return EActiveTimerReturnType::Stop;
	}
	bool bSucceeded = false;
	FSuperSLMSchemaDryRunReport Report;
	FString Error;
	if (!DryRun->Tick(InDeltaTime, bSucceeded, Report, Error))
	{
		return EActiveTimerReturnType::Continue;
	}
	DryRunTimer.Reset();
	DryRun.Reset();
	if (bSucceeded)
	{
		DryRunReport = MoveTemp(Report);
		DryRunStatus = TEXT("Done.");
	}
	else
	{
		DryRunStatus = FString::Printf(TEXT("Dry run failed: %s"), *Error);
	}
	return EActiveTimerReturnType::Stop;
}

FText SSuperSLMInspectionPanel::GetInspectionText() const
{
	if (bInspecting)
	{
		return LOCTEXT("Inspecting", "Inspecting (off the game thread)...");
	}
	if (!Inspection.IsSet())
	{
		return FText::FromString(InspectedModelName.IsEmpty() ? FString(TEXT("Not inspected.")) : InspectedModelName);
	}
	const FSuperSLMModelInspection& I = Inspection.GetValue();
	if (!I.bValid)
	{
		return FText::FromString(FString::Printf(TEXT("%s: %s"), *InspectedModelName, *I.Error));
	}
	TStringBuilder<4096> B;
	B.Appendf(TEXT("%s -- format version %u, flags 0x%08x, %s\n"), *InspectedModelName, I.FormatVersion, I.Flags, *Mib(I.FileBytes));
	B.Appendf(TEXT("artifact hash: %s\n"), *I.ArtifactHashHex);
	B.Append(TEXT("sections:\n"));
	for (const FSuperSLMSectionRow& Row : I.Sections)
	{
		B.Appendf(TEXT("  %s  dtype %u  offset %lld  %s\n"), *Row.TypeName, Row.Dtype, Row.Offset, *Mib(Row.ByteSize));
	}
	B.Appendf(TEXT("CFG1: hidden %d, layers %d, heads %d, kv heads %d, head dim %d, vocab %d, context_cap %lld, KV precision %d B\n"),
		I.HiddenSize, I.NumHiddenLayers, I.NumAttentionHeads, I.NumKeyValueHeads, I.HeadDim, I.VocabSize, I.ContextCap, I.KvPrecisionBytes);
	B.Appendf(TEXT("schemas carried: %s\n"), I.SchemaNames.Num() > 0 ? *FString::Join(I.SchemaNames, TEXT(", ")) : TEXT("(none)"));
	B.Appendf(TEXT("footprint for the declared shape (Layer 1 size queries):\n"));
	B.Appendf(TEXT("  workspace (sslm_workspace_size): %s\n"), *Mib(I.WorkspaceBytes));
	B.Appendf(TEXT("  KV block (sslm_kv_block_size): %s\n"), *Mib(I.KvBlockBytes));
	B.Appendf(TEXT("  KV pool overhead (sslm_kv_pool_overhead_size): %s\n"), *Mib(I.KvPoolOverheadBytes));
	B.Appendf(TEXT("  sequence state (sslm_seq_state_size, an upper bound on one save): %s\n"), *Mib(I.SeqStateBytesUpperBound));
	B.Appendf(TEXT("context_cap %lld: predicted reset %.3f ms, adopt %.3f ms against the Sequence Lifecycle Budget %.3f ms -- %s (measured host bandwidth %.2f GB/s)\n"),
		I.ContextCap, I.PredictedResetMs, I.PredictedAdoptMs, I.SequenceLifecycleBudgetMs,
		I.bWithinLifecycleBudget ? TEXT("within") : TEXT("EXCEEDS; lower context_cap"), I.MeasuredBandwidthBytesPerSec / 1.0e9);
	B.Appendf(TEXT("DGC1: %s\n"), I.bHasDampedGreedyConstants ? TEXT("present, unused at 1.0") : TEXT("absent"));
	if (I.bGpuDeviceResidentHead)
	{
		B.Appendf(TEXT("GPU device-resident head: on -- declares %s of device-local buffers (lower bound)\n"), *Mib(I.GpuDeviceHeadDeclaredBytes));
	}
	else
	{
		B.Append(TEXT("GPU device-resident head: off (logits run on the host)\n"));
	}
	B.Appendf(TEXT("provenance: %s\n"), *I.ProvenanceJson);
	return FText::FromString(FString(B.ToView()));
}

FText SSuperSLMInspectionPanel::GetAdapterText() const
{
	if (bInspectingAdapter)
	{
		return LOCTEXT("InspectingAdapter", "Inspecting adapter (off the game thread)...");
	}
	if (!AdapterInspection.IsSet())
	{
		return LOCTEXT("NoAdapter", "No adapter inspected.");
	}
	const FSuperSLMAdapterInspection& A = AdapterInspection.GetValue();
	if (!A.bValid)
	{
		return FText::FromString(FString::Printf(TEXT("Adapter: %s"), *A.Error));
	}
	return FText::FromString(FString::Printf(TEXT("Adapter: %s; base check: %s (%s); residency (sslm_adapter_residency): %s"),
		*Mib(A.FileBytes), A.bBaseMatches ? TEXT("matches") : TEXT("does NOT match"), *A.BaseCheckStatus, *Mib(A.ResidencyBytes)));
}

FText SSuperSLMInspectionPanel::GetDryRunText() const
{
	TStringBuilder<2048> B;
	B.Append(DryRunStatus);
	B.Append(TEXT("\n"));
	if (!Host->IsLoaded() || Host->GetModel() != Model.Get())
	{
		B.Append(TEXT("The dry run needs the model loaded (the query window's Load).\n"));
	}
	if (DryRunReport.IsSet())
	{
		const FSuperSLMSchemaDryRunReport& R = DryRunReport.GetValue();
		B.Appendf(TEXT("schema: %s\n"), *R.SchemaName);
		B.Appendf(TEXT("schema_accepting: %d\n"), R.bSchemaAccepting ? 1 : 0);
		B.Appendf(TEXT("forced_token_count: %lld\n"), R.ForcedTokenCount);
		B.Appendf(TEXT("-2 (dead end): %s\n"), R.bDeadEnd ? TEXT("yes") : TEXT("no"));
		B.Appendf(TEXT("stop reason: %s\n"), StopReasonName(R.StopReason));
		B.Appendf(TEXT("tokens: %d in %d frames\n"), R.GeneratedTokens.Num(), R.Frames);
		B.Appendf(TEXT("text: %s\n"), *R.Text);
	}
	return FText::FromString(FString(B.ToView()));
}

#undef LOCTEXT_NAMESPACE
