// T-2818 (L2-S3 suite). R-S3f, plan §9 (the plan; D-SLM7244,
// D-SLM7336, D-SLM7391), verbatim: "R-S2f's sweep (1–6 layers per slice, 200 decode tokens and a
// 200-token prompt each) run from the editor query window in an editor launched with its RHI (not
// -nullrhi), the level viewport realtime and in focus, on the 2080 SUPER. gpu_busy_ms is compared
// with R-S2f's headless figures at each setting, and the default slice is re-selected from this
// run. The load is UE's own editor rendering, not the example scene, and the report names it."
//
// Kind: measurement. The figures are reported, never gated. The cell FAILS only when it cannot
// measure what the plan names (D-SLM7519): a null RHI, no level viewport to render, a viewport that
// stops being realtime on any frame of the run, a GPU other than the 2080 SUPER, a Configure() that
// does not succeed, a prompt that is not 200 tokens, a setting that produced no sample of either
// kind, or a run that overran its wall-clock cap.
//
// Focus (round 5 follow-up, maintainer 2026-09-25): the plan's "in focus" is NOT required, because a
// cell that can never run under the machine's command-line automation is a silent non-measurement.
// What focus changes in UE 5.8 was read at source instead:
//  - UEditorEngine::Tick (Editor/UnrealEd/Private/EditorEngine.cpp:1803-1816): when
//    !FApp::HasFocus() and UEditorPerformanceSettings::bThrottleCPUWhenNotForeground, EVERY editor
//    viewport gets a "Background Process" realtime override of false, re-applied each tick -- the
//    viewports stop rendering. This path does not check GIsAutomationTesting.
//  - UEditorEngine::ShouldThrottleCPUUsage (:5305-5335), which caps the tick rate in
//    GetMaxTickRate (:2526), already returns false while GIsAutomationTesting.
// So the cell sets bThrottleCPUWhenNotForeground false for the run, which removes the only
// focus-dependent change to rendering, and restores the prior value after. It sets the level
// viewport realtime itself through AddRealtimeOverride (a temporary override, not the saved
// setting) and removes that override after. Whether the editor had focus is counted per frame and
// reported, not gated. That the plan's "in focus" is satisfied by this equivalence is the cell's
// reading of the engine source; it is recorded (record §10) for the plan, not assumed silently.
//
// Rebuilt 2026-09-25 (round 5, review S6; record §10). The previous form called RunQuery(), which
// ticks the subsystems in a loop on the game thread until the query stops: the editor drew no frame
// for the whole sweep, so "with rendering" was not true of the run it reported. It also had no
// null-RHI check, no prompt arm and no R-S2f comparison, and said it superseded R-S2f. This form:
//
//  - Drives the controller the way the editor query window does (SSuperSLMQueryWindow.cpp):
//    BeginQuery() once, then TickQuery() once per editor frame from a latent command, so the
//    editor ticks and draws between every slice. GFrameCounter is read per setting to show frames
//    advanced alongside the ticks.
//  - Refuses by name before any work under a null RHI, outside the editor, with no level viewport,
//    or on another GPU. Makes the level viewport realtime itself and turns the background
//    throttle off for the run (see "Focus" above), restoring both.
//  - Measures both arms of each setting in ONE query: a prompt of exactly 200 tokens, then 200
//    new tokens. Each gpu_busy_ms sample is classified prompt or decode by the query's phase read
//    before the tick that took it (Prefilling -> prompt, Decoding -> decode). A result applies K
//    ticks after it is planned, so the K samples after the phase turns Decoding may time a prompt
//    slice or a decode slice; they are excluded from both medians and counted. The sample stream is
//    the controller's own (a sample whenever GetGpuDispatchCount() moved), and the cell checks its
//    classified count does not exceed the readout's GpuBusyMsSamplesPerSlice.
//  - Reports R-S2f's recorded headless medians beside each setting, with their source, and says
//    "none on record" where there is none.
//  - Applies D-SLM7391's rule (the largest setting where BOTH medians are <= 2 ms) to this run
//    and reports it as this run's selection under this run's stated load. Recording it as the
//    default is a plan/record step, not something this cell can do or claim.
//
// Operator note: launch the editor normally (RHI on, not -nullrhi) with a level open, and run
// SuperSLM.L2S3.QueryWindow.GpuSliceDurationSweepWithRendering from the command line
// (-ExecCmds="Automation RunTests ...") or the Session Frontend; focus is not required. Wall time is
// roughly 400 prompt+decode tokens x ceil(24 / s) frames per setting, about 6-7 minutes at 60 fps
// (underived).
//
// Signatures used: FSuperSLMQueryWindowController::BeginQuery/TickQuery/
// GetLiveView/CancelQuery (SuperSLMQueryWindowController.h); USuperSLMGpuSubsystem::
// GetGpuDispatchCount/GetLastGpuBusyMs/GetConfiguredK/GetLayersPerTick; SuperSLM::TokenizeText
// (SuperSLMDetokenizer.h).

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "CoreGlobals.h"   // GUsingNullRHI, GFrameCounter, GIsEditor
#include "Editor.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "LevelEditorViewport.h" // GCurrentLevelEditingViewportClient
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Editor/EditorPerformanceSettings.h" // bThrottleCPUWhenNotForeground

#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMQueryWindowController.h"
#include "SuperSLMSubsystem.h"
#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Tests/AutomationCommon.h" // FTestWorldWrapper, FFunctionLatentCommand

namespace SuperSLMRS3f
{
	constexpr int32 kMinLayersPerSlice = 1;
	constexpr int32 kMaxLayersPerSlice = 6;       // R-S2f's own sweep width
	constexpr int32 kPromptTokens = 200;          // plan §9 R-S3f / R-S2f: "a 200-token prompt"
	constexpr int32 kDecodeTokens = 200;          // plan §9 R-S3f / R-S2f: "200 decode tokens"
	constexpr double kTargetMs = 2.0;             // D-SLM7244's target, D-SLM7391's selection bound
	constexpr double kWallCapSeconds = 1800.0;    // the whole sweep; ~7 min expected (underived)
	constexpr const TCHAR* kRequiredGpu = TEXT("2080 SUPER");

	// R-S2f's headless figures on record: the build record, the R-S2f
	// preview at v1.5.0 (-nullrhi, 200 tokens). Decode: the "gpu_busy_ms per composed slice" table.
	// Prompt: the "Per-prompt-token GPU time, 200-token prompt" table, which recorded 4 and 5 layers
	// per slice only. A negative entry is "none on record", never zero.
	constexpr double kRS2fDecodeMedianMs[kMaxLayersPerSlice] = { 0.348, 0.866, 1.022, 1.581, 1.574, 2.324 };
	constexpr double kRS2fPromptMedianMs[kMaxLayersPerSlice] = { -1.0, -1.0, -1.0, 1.368, 2.204, -1.0 };
	constexpr const TCHAR* kRS2fSource = TEXT("the build record (R-S2f preview, v1.5.0, headless)");

	double Median(TArray<double> Samples)
	{
		check(Samples.Num() > 0); // callers check first: an empty row has no median (D-SLM7519)
		Samples.Sort();
		return Samples[Samples.Num() / 2];
	}

	FString Recorded(double Ms)
	{
		return Ms < 0.0 ? FString(TEXT("none on record")) : FString::Printf(TEXT("%.3f"), Ms);
	}

	// Exactly kPromptTokens tokens of plain prose under A-EX's tokenizer, built by appending one
	// word at a time and re-tokenizing. False, naming why, when the count cannot land exactly.
	bool BuildPromptOfExactTokenCount(const USuperSLMModel& Model, int32 Target, FString& OutText, FString& OutError)
	{
		static const TCHAR* Words[] = {
			TEXT("The"), TEXT("old"), TEXT("shop"), TEXT("keeper"), TEXT("counts"), TEXT("each"), TEXT("potion"),
			TEXT("on"), TEXT("the"), TEXT("shelf"), TEXT("and"), TEXT("writes"), TEXT("the"), TEXT("number"),
			TEXT("in"), TEXT("a"), TEXT("book"), TEXT("before"), TEXT("the"), TEXT("door"), TEXT("opens"), TEXT(".") };
		FString Text;
		TArray<int32> Tokens;
		for (int32 i = 0; i < Target * 4; ++i)
		{
			const TCHAR* Word = Words[i % UE_ARRAY_COUNT(Words)];
			const FString Candidate = (Text.IsEmpty() || FCString::Strcmp(Word, TEXT(".")) == 0) ? Text + Word : Text + TEXT(" ") + Word;
			if (!SuperSLM::TokenizeText(Model, Candidate, Tokens, OutError))
			{
				return false;
			}
			if (Tokens.Num() == Target)
			{
				OutText = Candidate;
				return true;
			}
			if (Tokens.Num() > Target)
			{
				OutError = FString::Printf(TEXT("appending '%s' moved the prompt from under %d tokens to %d"), Word, Target, Tokens.Num());
				return false;
			}
			Text = Candidate;
		}
		OutError = FString::Printf(TEXT("the prompt never reached %d tokens"), Target);
		return false;
	}

	// The level viewport the cell renders: the current one when set, else the first perspective
	// level viewport client. Null when the editor has none.
	FLevelEditorViewportClient* FindLevelViewportClient()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (GCurrentLevelEditingViewportClient != nullptr && Clients.Contains(GCurrentLevelEditingViewportClient))
		{
			return GCurrentLevelEditingViewportClient;
		}
		for (FLevelEditorViewportClient* Client : Clients)
		{
			if (Client != nullptr && Client->IsPerspective())
			{
				return Client;
			}
		}
		return nullptr;
	}

	// The refusals, read before any work. Empty when none applies; otherwise each by name.
	FString RefusalsBeforeWork()
	{
		TArray<FString> Failures;
		if (GUsingNullRHI || !FApp::CanEverRender())
		{
			Failures.Add(TEXT("the editor is running under the null RHI (-nullrhi); R-S3f measures with UE rendering and R-S2f is the headless cell"));
		}
		if (!GIsEditor || GEditor == nullptr)
		{
			Failures.Add(TEXT("this is not an editor session; the plan runs R-S3f from the editor query window"));
		}
		else if (FindLevelViewportClient() == nullptr)
		{
			Failures.Add(TEXT("the editor has no perspective level viewport to render"));
		}
		return FString::Join(Failures, TEXT("; "));
	}

	const FText& RealtimeOverrideName()
	{
		static const FText Name = FText::FromString(TEXT("SuperSLM R-S3f measurement"));
		return Name;
	}

	struct FRow
	{
		int32 LayersPerSlice = 0;
		TArray<double> PromptSamples;
		TArray<double> DecodeSamples;
		int32 BoundarySamplesExcluded = 0;
		int32 OtherPhaseSamples = 0;      // taken while the phase read neither Prefilling nor Decoding
		int32 ReadoutSampleCount = -1;
		int32 GeneratedTokens = 0;
		int32 Ticks = 0;
		uint64 FramesAtStart = 0;
		uint64 FramesAtEnd = 0;
		int32 FramesOutOfCondition = 0;   // the viewport was gone or not realtime
		FString FirstConditionFailure;
		int32 FramesWithoutFocus = 0;     // reported only; the throttle that focus drives is off
		bool bFinished = false;
		bool bSucceeded = false;
	};

	struct FState
	{
		TSharedPtr<FTestWorldWrapper> World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TWeakObjectPtr<USuperSLMSubsystem> Cpu;
		TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
		TUniquePtr<FSuperSLMQueryWindowController> Controller;
		FString PromptText;
		FString RhiName;
		FString GpuBrand;

		TArray<FRow> Rows;
		int32 RowIndex = -1;           // -1: the next row has not begun
		int64 DispatchesSeen = 0;
		ESuperSLMSequencePhase LastPhase = ESuperSLMSequencePhase::Idle;
		int32 TicksSinceDecoding = -1; // -1 until this row's phase first reads Decoding
		int32 KForRow = 0;
		double StartSeconds = 0.0;

		// What the cell changed in the editor, restored by Finish.
		FLevelEditorViewportClient* ViewportClient = nullptr;
		bool bRealtimeOverrideAdded = false;
		bool bPriorThrottleWhenNotForeground = true;
		bool bThrottleChanged = false;
	};

	bool ViewportClientStillExists(const FLevelEditorViewportClient* Client)
	{
		return Client != nullptr && GEditor != nullptr && GEditor->GetLevelViewportClients().Contains(Client);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3GpuSliceSweepTest,
	"SuperSLM.L2S3.QueryWindow.GpuSliceDurationSweepWithRendering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3GpuSliceSweepTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMRS3f;

	// --- Refusals, before any work ---
	const FString ConditionFailures = RefusalsBeforeWork();
	if (!ConditionFailures.IsEmpty())
	{
		AddError(FString::Printf(TEXT("R-S3f refuses to run: %s."), *ConditionFailures));
		return false;
	}
	const FString GpuBrand = FPlatformMisc::GetPrimaryGPUBrand();
	if (!GpuBrand.Contains(kRequiredGpu))
	{
		AddError(FString::Printf(TEXT("R-S3f refuses to run: the primary GPU is '%s'; the plan measures on the %s, and R-S2f's figures it is compared with were taken there."),
			*GpuBrand, kRequiredGpu));
		return false;
	}

	TSharedRef<FState> State = MakeShared<FState>();
	State->RhiName = FApp::GetGraphicsRHI();
	State->GpuBrand = GpuBrand;

	FString AExPath, AExReason;
	if (!SuperSLML2S3Fixtures::TryGetAExArtifactPath(AExPath, AExReason))
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: %s"), *AExReason));
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (Model == nullptr || !Diag.bAccepted)
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: A-EX import: %s"), *Diag.Message));
		return false;
	}
	State->Model.Reset(Model); // held across frames; the editor collects garbage between them

	State->World = MakeShared<FTestWorldWrapper>();
	if (!State->World->CreateTestWorld(EWorldType::Game))
	{
		AddError(TEXT("R-S3f cannot run: the test game world was not created."));
		return false;
	}
	USuperSLMSubsystem* Cpu = SuperSLML2S3Fixtures::GetSubsystem(State->World->GetTestWorld());
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S3Fixtures::GetGpuSubsystem(State->World->GetTestWorld());
	if (Cpu == nullptr || Gpu == nullptr)
	{
		AddError(TEXT("R-S3f cannot run: a SuperSLM subsystem is unreachable on the test world's game instance."));
		return false;
	}
	State->Cpu = Cpu;
	State->Gpu = Gpu;

	// The controller's constructor takes the CPU subsystem by reference and the query path checks it
	// is configured with this model, so both backends are configured, as the query window does.
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 512;
	CpuConfig.MaxLayerBudget = SuperSLML2S3Fixtures::AExNumHiddenLayers;
	CpuConfig.BlockCount = 1;
	CpuConfig.SequenceLifecycleBudgetMs = 100000.0;
	CpuConfig.TickBudgetMs = 100000.0;
	const FSuperSLMConfigureReport CpuReport = Cpu->Configure(Model, CpuConfig);
	if (CpuReport.Result != ESuperSLMConfigureResult::Success)
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: CPU Configure() reported %d."), (int32)CpuReport.Result));
		return false;
	}

	FSuperSLMGpuRuntimeConfig GpuConfig;
	GpuConfig.ContextCap = 4096;
	GpuConfig.BlockCount = 1;
	// The controller caps each sequence's slice at GetLayersPerTick(), so the tick must cover the
	// widest setting: 6 layers in Layer-1 dispatches (6 x DispatchesPerLayer, 144 on Qwen2.5).
	GpuConfig.DispatchBudget = SuperSLML2S3Fixtures::DispatchBudgetForLayers(kMaxLayersPerSlice, SuperSLML2S2Fixtures::AExHasQkNorm);
	GpuConfig.K = SuperSLML2S3Fixtures::AExNumHiddenLayers; // >= composed floor ceil(24/6) and one-call floor 1
	GpuConfig.TickBudgetMs = 100000.0;
	const FSuperSLMGpuConfigureReport GpuReport = Gpu->Configure(Model, GpuConfig);
	if (GpuReport.Result != ESuperSLMGpuConfigureResult::Success)
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: GPU Configure() reported %d (%s)."), (int32)GpuReport.Result, *GpuReport.Message));
		return false;
	}
	if (Gpu->GetLayersPerTick() < kMaxLayersPerSlice)
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: the GPU tick covers %d layers, below the sweep's widest setting %d, so the wider settings would be capped and report a narrower slice under their name."),
			Gpu->GetLayersPerTick(), kMaxLayersPerSlice));
		return false;
	}

	FString PromptError;
	if (!BuildPromptOfExactTokenCount(*Model, kPromptTokens, State->PromptText, PromptError))
	{
		AddError(FString::Printf(TEXT("R-S3f cannot run: no %d-token prompt: %s"), kPromptTokens, *PromptError));
		return false;
	}

	State->Controller = MakeUnique<FSuperSLMQueryWindowController>(*Cpu, Gpu, *Model, SuperSLML2S3Fixtures::AExSchemaName());

	// --- The rendering condition, set by the cell (every fallible setup step is above) ---
	// Background throttle off, so losing focus does not stop viewport rendering (EditorEngine.cpp
	// :1803-1816), and the level viewport realtime through a temporary override. Both restored.
	UEditorPerformanceSettings* Perf = GetMutableDefault<UEditorPerformanceSettings>();
	State->bPriorThrottleWhenNotForeground = Perf->bThrottleCPUWhenNotForeground != 0;
	Perf->bThrottleCPUWhenNotForeground = false;
	State->bThrottleChanged = true;
	State->ViewportClient = FindLevelViewportClient();
	State->ViewportClient->AddRealtimeOverride(true, RealtimeOverrideName());
	State->bRealtimeOverrideAdded = true;
	AddInfo(FString::Printf(TEXT("R-S3f set for the run: level viewport realtime (temporary override), 'Use Less CPU when in Background' off (was %s). Both are restored when the cell ends."),
		State->bPriorThrottleWhenNotForeground ? TEXT("on") : TEXT("off")));

	State->StartSeconds = FPlatformTime::Seconds();

	AddInfo(FString::Printf(TEXT("R-S3f load: UE's own editor rendering (the level viewport, realtime, RHI '%s', GPU '%s'), not the example scene. A-EX, one sequence, %d-token prompt, %d new tokens per setting, checkbox off."),
		*State->RhiName, *State->GpuBrand, kPromptTokens, kDecodeTokens));

	// One TickQuery() per editor frame. Returns true when the sweep is over (finished or failed).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		auto Finish = [&S]()
		{
			if (S.Controller.IsValid() && S.Controller->IsQueryRunning())
			{
				S.Controller->CancelQuery();
			}
			S.Controller.Reset();
			S.World.Reset();
			S.Model.Reset();
			if (S.bRealtimeOverrideAdded && ViewportClientStillExists(S.ViewportClient))
			{
				S.ViewportClient->RemoveRealtimeOverride(RealtimeOverrideName(), /*bCheckMissingOverride*/ false);
			}
			S.bRealtimeOverrideAdded = false;
			if (S.bThrottleChanged)
			{
				GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = S.bPriorThrottleWhenNotForeground;
				S.bThrottleChanged = false;
			}
			return true;
		};

		USuperSLMGpuSubsystem* Gpu = S.Gpu.Get();
		if (Gpu == nullptr || !S.Cpu.IsValid())
		{
			AddError(TEXT("R-S3f: a subsystem was destroyed mid-sweep."));
			return Finish();
		}
		if (FPlatformTime::Seconds() - S.StartSeconds > kWallCapSeconds)
		{
			AddError(FString::Printf(TEXT("R-S3f: the sweep overran its %.0f s wall-clock cap at layers/slice=%d; nothing after that setting was measured."),
				kWallCapSeconds, S.RowIndex >= 0 ? S.Rows[S.RowIndex].LayersPerSlice : -1));
			return Finish();
		}

		// Begin the next setting.
		if (S.RowIndex < 0 || S.Rows[S.RowIndex].bFinished)
		{
			const int32 Next = S.Rows.Num() + kMinLayersPerSlice;
			if (Next > kMaxLayersPerSlice)
			{
				return Finish();
			}
			FRow& Row = S.Rows.AddDefaulted_GetRef();
			S.RowIndex = S.Rows.Num() - 1;
			Row.LayersPerSlice = Next;
			Row.FramesAtStart = GFrameCounter;

			FSuperSLMQueryWindowConfig Config;
			Config.Backend = ESuperSLMBackendBP::GPU;
			Config.FrameBudgetLayers = Next;
			Config.ConcurrentQueries = 1;
			Config.RequestedK = SuperSLML2S3Fixtures::AExNumHiddenLayers;
			FString Error;
			if (!S.Controller->BeginQuery(S.PromptText, kDecodeTokens, Config, Error))
			{
				AddError(FString::Printf(TEXT("R-S3f: layers/slice=%d did not start: %s"), Next, *Error));
				return Finish();
			}
			S.DispatchesSeen = Gpu->GetGpuDispatchCount();
			S.LastPhase = ESuperSLMSequencePhase::Idle;
			S.TicksSinceDecoding = -1;
			S.KForRow = 0; // read once the controller has applied the query's k
			return false;  // let this frame draw
		}

		FRow& Row = S.Rows[S.RowIndex];

		// The rendering condition, every frame of the measurement: the viewport still exists and is
		// still realtime (another system's override could turn it off). Focus is counted, not gated.
		FString Condition;
		if (!ViewportClientStillExists(S.ViewportClient))
		{
			Condition = TEXT("the level viewport the cell set realtime was closed");
		}
		else if (!S.ViewportClient->IsRealtime())
		{
			Condition = TEXT("the level viewport stopped being realtime (another realtime override took precedence)");
		}
		if (!FApp::HasFocus())
		{
			Row.FramesWithoutFocus += 1;
		}
		if (!Condition.IsEmpty())
		{
			Row.FramesOutOfCondition += 1;
			if (Row.FirstConditionFailure.IsEmpty())
			{
				Row.FirstConditionFailure = Condition;
			}
		}

		// The phase BEFORE this tick classifies whatever slice this tick samples.
		FSuperSLMQueryLiveView View;
		const bool bHaveView = S.Controller->GetLiveView(View);
		const ESuperSLMSequencePhase PhaseBefore = bHaveView ? View.Phase : S.LastPhase;
		if (bHaveView)
		{
			if (View.PromptTokens.Num() != kPromptTokens)
			{
				AddError(FString::Printf(TEXT("R-S3f: layers/slice=%d fed %d prompt tokens, not %d."), Row.LayersPerSlice, View.PromptTokens.Num(), kPromptTokens));
				return Finish();
			}
			if (View.bMovedBackend)
			{
				AddError(FString::Printf(TEXT("R-S3f: layers/slice=%d lost the GPU backend and moved to the CPU; nothing after that is a GPU figure."), Row.LayersPerSlice));
				return Finish();
			}
			Row.GeneratedTokens = View.GeneratedTokens.Num();
		}
		if (S.KForRow == 0 && Gpu->GetConfiguredK() > 0)
		{
			S.KForRow = Gpu->GetConfiguredK();
		}
		if (PhaseBefore == ESuperSLMSequencePhase::Decoding && S.TicksSinceDecoding < 0)
		{
			S.TicksSinceDecoding = 0;
		}

		bool bSucceeded = false;
		FSuperSLMQueryWindowReadout Readout;
		FString Error;
		const bool bStopped = S.Controller->TickQuery(FApp::GetDeltaTime(), bSucceeded, Readout, Error);
		Row.Ticks += 1;

		const int64 Dispatches = Gpu->GetGpuDispatchCount();
		if (Dispatches != S.DispatchesSeen)
		{
			S.DispatchesSeen = Dispatches;
			const double Ms = Gpu->GetLastGpuBusyMs();
			if (PhaseBefore == ESuperSLMSequencePhase::Prefilling)
			{
				Row.PromptSamples.Add(Ms);
			}
			else if (PhaseBefore == ESuperSLMSequencePhase::Decoding)
			{
				if (S.TicksSinceDecoding < S.KForRow)
				{
					Row.BoundarySamplesExcluded += 1; // may time the last prompt slices (K-tick latency)
				}
				else
				{
					Row.DecodeSamples.Add(Ms);
				}
			}
			else
			{
				Row.OtherPhaseSamples += 1;
			}
		}
		if (S.TicksSinceDecoding >= 0)
		{
			S.TicksSinceDecoding += 1;
		}
		S.LastPhase = PhaseBefore;

		if (!bStopped)
		{
			return false; // let this frame draw
		}

		Row.bFinished = true;
		Row.bSucceeded = bSucceeded;
		Row.FramesAtEnd = GFrameCounter;
		if (!bSucceeded)
		{
			AddError(FString::Printf(TEXT("R-S3f: layers/slice=%d stopped without a result: %s"), Row.LayersPerSlice, *Error));
			return Finish();
		}
		Row.ReadoutSampleCount = Readout.GpuBusyMsSamplesPerSlice.Num();

		// The controller starts counting at its vend; this cell starts at BeginQuery(), which can be
		// earlier when the vend waits for the previous row's returns to drain. Samples taken then read
		// a phase other than Prefilling/Decoding and land in OtherPhaseSamples, so only the classified
		// bins are held to the controller's count.
		const int32 Classified = Row.PromptSamples.Num() + Row.DecodeSamples.Num() + Row.BoundarySamplesExcluded;
		TestTrue(FString::Printf(TEXT("R-S3f layers/slice=%d: the classified samples (%d) are within the controller's own (%d readout GpuBusyMsSamplesPerSlice)"),
				Row.LayersPerSlice, Classified, Row.ReadoutSampleCount),
			Classified <= Row.ReadoutSampleCount);
		TestTrue(FString::Printf(TEXT("R-S3f layers/slice=%d produced prompt-slice samples"), Row.LayersPerSlice), Row.PromptSamples.Num() > 0);
		TestTrue(FString::Printf(TEXT("R-S3f layers/slice=%d produced decode-slice samples"), Row.LayersPerSlice), Row.DecodeSamples.Num() > 0);
		TestTrue(FString::Printf(TEXT("R-S3f layers/slice=%d: frames advanced while it ran (the editor drew between slices)"), Row.LayersPerSlice),
			Row.FramesAtEnd > Row.FramesAtStart);
		if (Row.FramesOutOfCondition > 0)
		{
			AddError(FString::Printf(TEXT("R-S3f layers/slice=%d: the plan's rendering condition did not hold on %d of %d frames (first: %s); its figures are not the plan's measurement."),
				Row.LayersPerSlice, Row.FramesOutOfCondition, Row.Ticks, *Row.FirstConditionFailure));
		}
		if (Row.GeneratedTokens < kDecodeTokens)
		{
			AddInfo(FString::Printf(TEXT("R-S3f layers/slice=%d: a stop token ended the query at %d new tokens, below the plan's %d; the decode median is over those."),
				Row.LayersPerSlice, Row.GeneratedTokens, kDecodeTokens));
		}

		const int32 i = Row.LayersPerSlice - 1;
		AddInfo(FString::Printf(
			TEXT("R-S3f layers/slice=%d | prompt slices: n=%d median %s ms (R-S2f headless: %s) | decode slices: n=%d median %s ms (R-S2f headless: %s) | excluded at the prompt/decode boundary: %d (K=%d) | other phase: %d | new tokens %d | ticks %d, editor frames %llu, frames without editor focus %d (throttle off) | AppliedK %d"),
			Row.LayersPerSlice,
			Row.PromptSamples.Num(), Row.PromptSamples.Num() > 0 ? *FString::Printf(TEXT("%.3f"), Median(Row.PromptSamples)) : TEXT("n/a"), *Recorded(kRS2fPromptMedianMs[i]),
			Row.DecodeSamples.Num(), Row.DecodeSamples.Num() > 0 ? *FString::Printf(TEXT("%.3f"), Median(Row.DecodeSamples)) : TEXT("n/a"), *Recorded(kRS2fDecodeMedianMs[i]),
			Row.BoundarySamplesExcluded, S.KForRow, Row.OtherPhaseSamples, Row.GeneratedTokens, Row.Ticks,
			(unsigned long long)(Row.FramesAtEnd - Row.FramesAtStart), Row.FramesWithoutFocus, Readout.AppliedK));

		if (Row.LayersPerSlice < kMaxLayersPerSlice)
		{
			return false; // the next setting begins on the next frame
		}

		// --- The selection, D-SLM7391, applied to this run only ---
		bool bAllMeasured = S.Rows.Num() == (kMaxLayersPerSlice - kMinLayersPerSlice + 1);
		int32 Selected = -1;
		for (const FRow& R : S.Rows)
		{
			if (!R.bSucceeded || R.PromptSamples.Num() == 0 || R.DecodeSamples.Num() == 0 || R.FramesOutOfCondition > 0)
			{
				bAllMeasured = false;
				continue;
			}
			if (Median(R.PromptSamples) <= kTargetMs && Median(R.DecodeSamples) <= kTargetMs)
			{
				Selected = FMath::Max(Selected, R.LayersPerSlice);
			}
		}
		if (bAllMeasured)
		{
			AddInfo(FString::Printf(TEXT("R-S3f selection, this run (D-SLM7391: the largest setting where both the prompt-slice and decode-slice medians are <= %.1f ms): %s. "
				"Load: UE's editor rendering (RHI '%s', GPU '%s'), not the example scene. R-S2f's headless figures are from %s; under that rule its recorded figures give 4 "
				"(prompt at 5 is %.3f ms). This cell reports; recording the default is a plan step."),
				kTargetMs, Selected > 0 ? *FString::Printf(TEXT("%d layers per slice"), Selected) : TEXT("no setting cleared the target"),
				*S.RhiName, *S.GpuBrand, kRS2fSource, kRS2fPromptMedianMs[4]));
		}
		else
		{
			AddError(TEXT("R-S3f: not every setting was measured under the plan's condition, so no selection is reported."));
		}
		return Finish();
	}));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
