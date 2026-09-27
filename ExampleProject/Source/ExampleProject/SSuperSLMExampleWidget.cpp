#include "SSuperSLMExampleWidget.h"

#include "Styling/CoreStyle.h"
#include "SuperSLMExampleScene.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SuperSLMExample"

namespace
{
	FSlateFontInfo Font(float Size, const TCHAR* Typeface = TEXT("Regular"))
	{
		return FCoreStyle::GetDefaultFontStyle(Typeface, Size);
	}

	TSharedRef<SWidget> Heading(const FText& Text)
	{
		return SNew(STextBlock).Text(Text).Font(Font(12, TEXT("Bold"))).ColorAndOpacity(FLinearColor(0.95f, 0.8f, 0.45f));
	}

	TSharedRef<SWidget> Label(const FText& Text)
	{
		return SNew(SBox).WidthOverride(150.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Text).Font(Font(10))
		];
	}
}

void SSuperSLMExampleWidget::Construct(const FArguments& InArgs)
{
	Scene = InArgs._Scene;
	const FSlateBrush* Panel = FCoreStyle::Get().GetBrush("WhiteBrush");
	const FCheckBoxStyle* Radio = &FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton");

	auto Settings = [this]() -> FSuperSLMExampleSettings*
	{
		return Scene.IsValid() ? &Scene->EditSettings() : nullptr;
	};

	ChildSlot
	[
		SNew(SHorizontalBox)

		// --- Controls ---
		+ SHorizontalBox::Slot().AutoWidth().Padding(16.0f).VAlign(VAlign_Top)
		[
			SNew(SBorder).BorderImage(Panel).BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.75f)).Padding(12.0f)
			[
				SNew(SBox).WidthOverride(430.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(STextBlock).Text(LOCTEXT("Title", "SuperSLM example: the potion shop")).Font(Font(15, TEXT("Bold")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(STextBlock).AutoWrapText(true).Font(Font(9)).Text(this, &SSuperSLMExampleWidget::StatusText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock).Text(LOCTEXT("Customer", "The customer says")).Font(Font(10))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(SEditableTextBox)
						.Text(FText::FromString(Scene.IsValid() ? Scene->GetSettings().Utterance : FString()))
						.IsEnabled(this, &SSuperSLMExampleWidget::IsIdle)
						.OnTextChanged_Lambda([Settings](const FText& Text)
						{
							if (FSuperSLMExampleSettings* S = Settings())
							{
								S->Utterance = Text.ToString().Left(400);
							}
						})
						.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type Commit)
						{
							if (Commit == ETextCommit::OnEnter)
							{
								OnAsk();
							}
						})
					]

					// Backend
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()[ Label(LOCTEXT("Backend", "Backend")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
						[
							SNew(SCheckBox).Style(Radio)
							.IsEnabled(this, &SSuperSLMExampleWidget::IsIdle)
							.IsChecked_Lambda([Settings]() { const FSuperSLMExampleSettings* S = Settings(); return S && S->Backend == ESuperSLMExampleBackend::CPU ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([Settings](ECheckBoxState) { if (FSuperSLMExampleSettings* S = Settings()) { S->Backend = ESuperSLMExampleBackend::CPU; } })
							[ SNew(STextBlock).Text(LOCTEXT("Cpu", "CPU")) ]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SCheckBox).Style(Radio)
							.IsEnabled_Lambda([this]() { return IsIdle() && Scene.IsValid() && Scene->GetDemo() && Scene->GetDemo()->IsGpuAvailable(); })
							.ToolTipText_Lambda([this]() { return Scene.IsValid() && Scene->GetDemo() ? FText::FromString(Scene->GetDemo()->GetGpuUnavailableReason()) : FText(); })
							.IsChecked_Lambda([Settings]() { const FSuperSLMExampleSettings* S = Settings(); return S && S->Backend == ESuperSLMExampleBackend::GPU ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([Settings](ECheckBoxState) { if (FSuperSLMExampleSettings* S = Settings()) { S->Backend = ESuperSLMExampleBackend::GPU; } })
							[ SNew(STextBlock).Text(LOCTEXT("Gpu", "GPU (D3D12)")) ]
						]
					]

					// Thinking/Frame Budget: one control
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()[ Label(LOCTEXT("FrameBudget", "Thinking/Frame Budget")) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).WidthOverride(90.0f)
							[
								SNew(SSpinBox<int32>).MinValue(1).MaxValue(SuperSLMExample::NumHiddenLayers)
								.IsEnabled(this, &SSuperSLMExampleWidget::IsIdle)
								.Value_Lambda([Settings]() { const FSuperSLMExampleSettings* S = Settings(); return S ? S->FrameBudgetLayers : 1; })
								.OnValueChanged_Lambda([Settings](int32 V) { if (FSuperSLMExampleSettings* S = Settings()) { S->FrameBudgetLayers = V; } })
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
						[
							SNew(STextBlock).Font(Font(9)).Text(this, &SSuperSLMExampleWidget::FrameBudgetHint)
						]
					]

					// Concurrent queries
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()[ Label(LOCTEXT("Concurrent", "Concurrent queries")) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).WidthOverride(90.0f)
							[
								SNew(SSpinBox<int32>).MinValue(1).MaxValue(SuperSLMExample::MaxConcurrentQueries)
								.IsEnabled(this, &SSuperSLMExampleWidget::IsIdle)
								.Value_Lambda([Settings]() { const FSuperSLMExampleSettings* S = Settings(); return S ? S->ConcurrentQueries : 1; })
								.OnValueChanged_Lambda([Settings](int32 V) { if (FSuperSLMExampleSettings* S = Settings()) { S->ConcurrentQueries = V; } })
							]
						]
					]

					// k
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 12)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()[ Label(LOCTEXT("K", "k (GPU ticks)")) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).WidthOverride(90.0f)
							[
								SNew(SSpinBox<int32>).MinValue(1).MaxValue(256)
								.IsEnabled_Lambda([this, Settings]() { const FSuperSLMExampleSettings* S = Settings(); return IsIdle() && S && S->Backend == ESuperSLMExampleBackend::GPU; })
								.Value_Lambda([Settings]() { const FSuperSLMExampleSettings* S = Settings(); return S ? S->RequestedK : 1; })
								.OnValueChanged_Lambda([Settings](int32 V) { if (FSuperSLMExampleSettings* S = Settings()) { S->RequestedK = V; } })
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
						[
							SNew(STextBlock).Font(Font(9)).AutoWrapText(true).Text(this, &SSuperSLMExampleWidget::KHint)
						]
					]

					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SButton).Text(LOCTEXT("Ask", "Ask the shopkeeper"))
							.IsEnabled_Lambda([this]() { return Scene.IsValid() && Scene->CanAsk(); })
							.OnClicked(this, &SSuperSLMExampleWidget::OnAsk)
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton).Text(LOCTEXT("SelfCheck", "Re-run self-check"))
							.IsEnabled_Lambda([this]() { return Scene.IsValid() && Scene->CanAsk(); })
							.OnClicked(this, &SSuperSLMExampleWidget::OnSelfCheck)
						]
					]
				]
			]
		]

		// --- Readout ---
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 16, 16, 16).VAlign(VAlign_Top)
		[
			SNew(SBorder).BorderImage(Panel).BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.75f)).Padding(12.0f)
			[
				SNew(SBox).MaxDesiredHeight(900.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()[ Heading(LOCTEXT("SelfCheckHeading", "Determinism self-check (run on load)")) ]
					+ SScrollBox::Slot().Padding(0, 2, 0, 10)[ SNew(STextBlock).AutoWrapText(true).Font(Font(9)).Text(this, &SSuperSLMExampleWidget::SelfCheckText) ]
					+ SScrollBox::Slot()[ Heading(LOCTEXT("DigestHeading", "Token digest")) ]
					+ SScrollBox::Slot().Padding(0, 2, 0, 10)[ SNew(STextBlock).AutoWrapText(true).Font(Font(10)).Text(this, &SSuperSLMExampleWidget::DigestText) ]
					+ SScrollBox::Slot()[ Heading(LOCTEXT("ResultHeading", "Result")) ]
					+ SScrollBox::Slot().Padding(0, 2, 0, 10)[ SNew(STextBlock).AutoWrapText(true).Font(Font(10)).Text(this, &SSuperSLMExampleWidget::ResultText) ]
					+ SScrollBox::Slot()[ Heading(LOCTEXT("CostHeading", "Cost readout")) ]
					+ SScrollBox::Slot().Padding(0, 2, 0, 0)[ SNew(STextBlock).AutoWrapText(true).Font(Font(9)).Text(this, &SSuperSLMExampleWidget::FiguresText) ]
				]
			]
		]
	];
}

bool SSuperSLMExampleWidget::IsIdle() const
{
	return Scene.IsValid() && (Scene->CanAsk() || Scene->GetLoadState() == ASuperSLMExampleScene::ELoadState::Failed);
}

FText SSuperSLMExampleWidget::StatusText() const
{
	if (!Scene.IsValid())
	{
		return FText();
	}
	FString Text = Scene->GetStatus();
	if (const FSuperSLMExampleDemo* Demo = Scene->GetDemo())
	{
		const FSuperSLMExampleReadout& R = Demo->GetReadout();
		if (Demo->IsSelfCheckRunning())
		{
			Text = TEXT("Running the determinism self-check on both backends (stepped, a slice per frame)...");
		}
		else if (R.State == ESuperSLMExampleRunState::ConfiguringGpu)
		{
			Text = FString::Printf(TEXT("Configuring the GPU backend for %s (BeginConfigure; the heavy part is off the game thread)..."), *SuperSLMExample::SettingsDescription(R.Settings));
		}
		else if (R.State == ESuperSLMExampleRunState::Running)
		{
			Text = FString::Printf(TEXT("Running (%s)..."), *SuperSLMExample::SettingsDescription(R.Settings));
		}
		else if (R.State == ESuperSLMExampleRunState::Failed)
		{
			Text = FString::Printf(TEXT("The query failed: %s"), *R.Error);
		}
		if (Demo->IsInitialized() && !Demo->IsGpuAvailable())
		{
			Text += FString::Printf(TEXT("\nGPU backend unavailable: %s"), *Demo->GetGpuUnavailableReason());
		}
	}
	if (!LastAskError.IsEmpty())
	{
		Text += TEXT("\n") + LastAskError;
	}
	return FText::FromString(Text);
}

FText SSuperSLMExampleWidget::SelfCheckText() const
{
	const FSuperSLMExampleDemo* Demo = Scene.IsValid() ? Scene->GetDemo() : nullptr;
	if (Demo == nullptr || !Demo->HasSelfCheckReport())
	{
		return Demo != nullptr && Demo->IsSelfCheckRunning() ? LOCTEXT("SelfCheckRunning", "Running...") : LOCTEXT("SelfCheckPending", "Not run yet.");
	}
	// The report's own words: the scope travels with the verdict.
	const FSuperSLMSelfCheckReport& R = Demo->GetSelfCheckReport();
	// A verdict is read only when the plugin has not quarantined it.
	FString Text;
	if (Demo->IsCpuVerdictReadable())
	{
		Text = FString::Printf(TEXT("CPU: %s. %s"), *SuperSLMExample::VerdictName(R.Cpu.Verdict), *R.Cpu.ScopeText);
		if (R.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Diverged)
		{
			Text += FString::Printf(TEXT(" First divergence: step %d, layer budget setting %d."), R.Cpu.FirstDivergingStep, R.Cpu.FirstDivergingGranularityIndex);
		}
	}
	else
	{
		Text = TEXT("CPU: withheld. The plugin has withheld the CPU verdict.");
	}
	if (Demo->IsGpuVerdictReadable())
	{
		Text += FString::Printf(TEXT("\nGPU: %s. %s"), *SuperSLMExample::VerdictName(R.Gpu.Verdict), *R.Gpu.ScopeText);
		if (R.Gpu.Verdict == ESuperSLMSelfCheckVerdict::Diverged)
		{
			Text += FString::Printf(TEXT(" First divergence: step %d, granularity %d."), R.Gpu.FirstDivergingStep, R.Gpu.FirstDivergingGranularityIndex);
		}
	}
	else
	{
		// Withheld: not read, so not shown.
		Text += TEXT("\nGPU: withheld. In 1.0 the GPU verdict is always withheld: it is recorded in the report but not shown. A later release shows it.");
	}
	Text += FString::Printf(TEXT("\nDevice %s; SuperSLM %s; plugin %s; model %s"), *R.DeviceLabel, *R.LayerOneTagAndCommit, *R.PluginVersion, *R.ArtifactHash.Left(16));
	if (!Demo->IsGpuVerified())
	{
		Text += TEXT("\nThe invariant below is demonstrated on the CPU backend. GPU runs still report their cost; nothing is refused.");
	}
	return FText::FromString(Text);
}

FText SSuperSLMExampleWidget::DigestText() const
{
	const FSuperSLMExampleDemo* Demo = Scene.IsValid() ? Scene->GetDemo() : nullptr;
	if (Demo == nullptr)
	{
		return FText();
	}
	const FSuperSLMExampleReadout& R = Demo->GetReadout();
	if (R.State != ESuperSLMExampleRunState::Done)
	{
		return LOCTEXT("NoDigest", "Ask a question. Then, on the CPU backend, move Thinking/Frame Budget, concurrent queries or k and ask again: only the frames and the cost change. Switching Backend can change the answer.");
	}
	FString Text = FString::Printf(TEXT("%s\n%d of %d concurrent queries produced this digest."), *R.TokenDigestHex, R.QueriesMatchingDigest, R.QueryCount);

	const bool bRunCounts = R.Settings.Backend == ESuperSLMExampleBackend::CPU || Demo->IsGpuVerified();
	FString BaselineDigest;
	FString BaselineDescription;
	if (!bRunCounts)
	{
		Text += Demo->IsGpuVerdictReadable()
			? TEXT("\nThis run is on a GPU the self-check did not verify, so it is not compared with the invariant's baseline.")
			: TEXT("\nThe GPU verdict is withheld, so GPU runs are not compared with the invariant's baseline.");
	}
	else if (Demo->GetInvariantBaseline(R.Settings, BaselineDigest, BaselineDescription))
	{
		Text += BaselineDigest == R.TokenDigestHex
			? FString::Printf(TEXT("\nUnchanged: identical to the first run at this prompt (%s)."), *BaselineDescription)
			: FString::Printf(TEXT("\nDIFFERS from the first run at this prompt (%s): %s"), *BaselineDescription, *BaselineDigest);
	}
	return FText::FromString(Text);
}

FText SSuperSLMExampleWidget::ResultText() const
{
	const FSuperSLMExampleDemo* Demo = Scene.IsValid() ? Scene->GetDemo() : nullptr;
	if (Demo == nullptr || Demo->GetReadout().State != ESuperSLMExampleRunState::Done)
	{
		return FText();
	}
	const FSuperSLMExampleReadout& R = Demo->GetReadout();
	return FText::FromString(FString::Printf(TEXT("Order (schema-constrained, %s): %s\n%s\n\nShopkeeper (free decode): %s"),
		R.bExtractionSchemaValid ? TEXT("valid") : TEXT("NOT valid"), *R.ExtractionValidation, *R.ExtractionText, *R.ReplyText));
}

FText SSuperSLMExampleWidget::FiguresText() const
{
	const FSuperSLMExampleDemo* Demo = Scene.IsValid() ? Scene->GetDemo() : nullptr;
	if (Demo == nullptr || Demo->GetReadout().Figures.Num() == 0)
	{
		return FText();
	}
	const FSuperSLMExampleReadout& R = Demo->GetReadout();
	FString Text = SuperSLMExample::SettingsDescription(R.Settings);
	for (const FSuperSLMExampleFigure& F : R.Figures)
	{
		Text += FString::Printf(TEXT("\n%s: %s    [%s]"), *F.Label, *F.Value, *F.Source);
	}
	return FText::FromString(Text);
}

FText SSuperSLMExampleWidget::FrameBudgetHint() const
{
	if (!Scene.IsValid())
	{
		return FText();
	}
	const FSuperSLMExampleSettings& S = Scene->GetSettings();
	if (S.Backend == ESuperSLMExampleBackend::CPU)
	{
		return FText::FromString(FString::Printf(TEXT("layers per worker call (layer_budget)%s"), S.IsWholeToken() ? TEXT(", whole token") : TEXT("")));
	}
	return S.IsWholeToken() ? LOCTEXT("WholeToken", "whole token (one-call path)") : LOCTEXT("PerSlice", "layers per GPU slice (composed path)");
}

FText SSuperSLMExampleWidget::KHint() const
{
	if (!Scene.IsValid())
	{
		return FText();
	}
	const FSuperSLMExampleSettings& S = Scene->GetSettings();
	if (S.Backend == ESuperSLMExampleBackend::CPU)
	{
		return LOCTEXT("KCpu", "GPU only. The CPU backend sets K per job; the readout shows it.");
	}
	const int32 Minimum = FSuperSLMExampleDemo::EstimateGpuMinimumK(S);
	return FText::FromString(FString::Printf(TEXT("minimum %d at this Frame Budget and concurrency; runs at %d"), Minimum, FMath::Max(Minimum, S.RequestedK)));
}

FReply SSuperSLMExampleWidget::OnAsk()
{
	LastAskError.Reset();
	FString Error;
	if (Scene.IsValid() && !Scene->Ask(Error))
	{
		LastAskError = Error;
	}
	return FReply::Handled();
}

FReply SSuperSLMExampleWidget::OnSelfCheck()
{
	LastAskError.Reset();
	if (Scene.IsValid())
	{
		Scene->RequestSelfCheck();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
