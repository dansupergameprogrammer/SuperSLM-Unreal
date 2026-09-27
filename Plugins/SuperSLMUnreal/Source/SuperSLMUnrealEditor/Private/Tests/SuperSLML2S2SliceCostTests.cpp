// T-2816 -- L2-S2 red suite. Plan cell R-S2f (the plan
// §9): "What a GPU slice costs" -- dim 7; D-SLM7244's 2 ms target. A MEASUREMENT cell
// (§9's own kind column), not pass/fail: it REPORTS gpu_busy_ms per slice
// (LastCallTiming()) at 1-6 layers per slice; tokens per second on the composed path at
// each setting against the one-call path; host finish ms per token. It sets a
// PROVISIONAL default slice (the largest setting whose median gpu_busy_ms is <= 2 ms)
// and the minimum k -- reported here, not adjudicated: "where the plan leaves a number
// to calibration, report it rather than inventing a bound" (this suite's task scope).
//
// **Headless, no synthetic load (D-SLM7336, answering Q_R-S2f of
// the red-suite record §8).** This cell runs `-nullrhi`, and
// this test never contends the GPU queue Layer 1 submits to with any rendering work of
// its own -- the suite's own earlier synthetic busy-loop stand-in
// (`ApplySyntheticRenderingLoad`) is REMOVED, per the ruling: "a CPU busy loop contends
// no GPU queue, and a fabricated GPU load is an input no producer emits." The default
// slice this cell selects is explicitly provisional and RE-SELECTED by R-S3f
// (L2-S3, the red-suite record if filed separately; plan
// §9 R-S3f), which runs the same sweep from the editor query window with UE's own RHI
// and rendering on -- the first REAL rendering load this plugin can measure against,
// per D-SLM7336's own reasoning: the first UE rendering load the plugin can run is the
// editor with its RHI, at L2-S3, where the query window (the demonstrator this target
// exists for, D-SLM7244) is built.
//
// UPDATED per fold 8 (D-SLM7391, T-2785 fold 8; maintainer's own routed instruction,
// 2026-09-19): since prompt tokens are now fed through the SAME embed-plus-layer-slice
// loop as decode tokens (D-SLM7379), a prompt is ALSO sliced at each candidate setting
// and its own slice cost is a SEPARATE quantity from decode's -- T-2826 round 2's own
// harness measured a 200-token prompt's median slice at 1.37 ms at 4 layers/slice and
// 2.20 ms at 5, against about 1.58 ms for decode at BOTH settings, so decode alone
// cleared the old 5-layer default while the prompt did not. At EACH candidate setting
// this cell now ALSO slices a 200-token prompt and measures its own median
// gpu_busy_ms (sampled only while the sequence is Prefilling); the default-slice rule
// now requires BOTH the decode-slice AND the prompt-slice median to be <= 2 ms, which
// moves the headless provisional default to 4 layers.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	constexpr int32 kMinLayersPerSlice = 1;
	constexpr int32 kMaxLayersPerSlice = 6; // plan §10.3's own costing text: "6 settings x 200 tokens, headless"
	constexpr int32 kMeasurementTokenCount = 200;
	constexpr double kTargetGpuBusyMs = 2.0; // D-SLM7244's own target, the PROVISIONAL selection rule's bound, not a per-run assertion

	struct FSliceMeasurement
	{
		int32 LayersPerSlice = 0;
		double MedianGpuBusyMs = 0.0;
		double MedianHostFinishMs = 0.0;
		double TokensPerSecond = 0.0;
		// D-SLM7391: the SAME setting's own prompt-slice cost, a separate quantity from
		// decode's -- both must clear the 2 ms target for this setting to qualify as the
		// default.
		double MedianPromptSliceGpuBusyMs = 0.0;
	};

	bool SetUpGpu(FAutomationTestBase& T, UWorld* World, uint32 DispatchBudget, USuperSLMModel*& OutModel, USuperSLMGpuSubsystem*& OutGpu, bool bHeadOn = true)
	{
		FString AExPath, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return false;
		}
		OutGpu = GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), OutGpu))
		{
			return false;
		}
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = 4096;
		Config.BlockCount = 1;
		Config.DispatchBudget = DispatchBudget;
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		OutModel->bGpuDeviceResidentHead = bHeadOn;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(OutModel, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	// D-SLM7391: measures ONE setting's own prompt-slice cost -- a 200-token prompt
	// (kMeasurementTokenCount, matching T-2826 round 2's own harness), composed path,
	// MaxNewTokens=1 (the minimum that lets a real generation request exist; decode
	// samples are EXCLUDED by construction below). gpu_busy_ms is sampled only while
	// GetPhase() reports Prefilling -- prompt ingestion -- never after it flips to
	// Decoding, which isolates the prompt's own per-slice cost from decode's even
	// though both now share the identical embed-plus-layer-slice tick loop.
	bool MeasurePromptSliceGpuBusyMs(FAutomationTestBase& T, UWorld* World, int32 LayersPerSlice, double& OutMedianMs)
	{
		USuperSLMModel* Model = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
		if (!SetUpGpu(T, World, DispatchBudgetForLayersPerSlice(LayersPerSlice), Model, Gpu))
		{
			return false;
		}
		FSuperSLMGpuSequence Seq;
		if (!T.TestEqual(TEXT("VendSequence() (prompt-slice measurement)"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens.Init(1, kMeasurementTokenCount);
		Request.MaxNewTokens = 1;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() (prompt-slice measurement) must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
		{
			Gpu->ReturnSequence(Seq);
			return false;
		}

		TArray<double> PromptSliceSamples;
		// Unpaced (M-24's second witness, ruling 2026-09-26 as folded); a sample is taken only when
		// GetGpuDispatchCount() has moved since the last one, so a spinning tick adds no duplicates.
		int64 LastSampledDispatch = Gpu->GetGpuDispatchCount();
		const double PromptStartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - PromptStartSeconds < 180.0)
		{
			const ESuperSLMSequencePhase PhaseBeforeTick = Gpu->GetPhase(Seq);
			if (PhaseBeforeTick != ESuperSLMSequencePhase::Prefilling)
			{
				break; // prompt ingestion is over -- every later sample would be decode's, not the prompt's
			}
			Gpu->Tick(1.0f / 60.0f);
			const int64 Dispatch = Gpu->GetGpuDispatchCount();
			if (Dispatch != LastSampledDispatch)
			{
				PromptSliceSamples.Add(Gpu->GetLastGpuBusyMs());
				LastSampledDispatch = Dispatch;
			}
			FPlatformProcess::Sleep(0);
		}
		const ESuperSLMSequencePhase PromptPhase = Gpu->GetPhase(Seq);
		Gpu->ReturnSequence(Seq);
		if (!T.TestTrue(TEXT("prompt ingestion must reach Decoding or Complete before the 180-second measurement deadline"),
			PromptPhase == ESuperSLMSequencePhase::Decoding || PromptPhase == ESuperSLMSequencePhase::Complete))
		{
			return false;
		}

		if (!T.TestTrue(TEXT("at least one prompt slice must have been sampled"), PromptSliceSamples.Num() > 0))
		{
			return false;
		}
		PromptSliceSamples.Sort();
		OutMedianMs = PromptSliceSamples[PromptSliceSamples.Num() / 2];
		return true;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceCostTest,
	"SuperSLM.L2S2.SliceCost.MeasureGpuBusyMsHeadlessAndSetProvisionalDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceCostTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	AddInfo(TEXT("This cell runs headless (-nullrhi); no rendering load, synthetic or "
		"real, contends the GPU queue Layer 1 submits to. The default slice this cell "
		"selects is PROVISIONAL and re-selected by R-S3f under UE's own rendering (plan "
		"§9, D-SLM7336)."));

	TArray<FSliceMeasurement> Measurements;

	for (int32 LayersPerSlice = kMinLayersPerSlice; LayersPerSlice <= kMaxLayersPerSlice; ++LayersPerSlice)
	{
		USuperSLMModel* Model = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
		if (!SetUpGpu(*this, World, DispatchBudgetForLayersPerSlice(LayersPerSlice), Model, Gpu))
		{
			return false;
		}

		FSuperSLMGpuSequence Seq;
		if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = {1, 2, 3, 4, 5};
		Request.MaxNewTokens = kMeasurementTokenCount;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;

		Gpu->RequestBeginGeneration(Seq, Request);

		TArray<double> GpuBusySamples, HostFinishSamples;
		// Unpaced on purpose: this cell is M-24's second witness (plan §2.5 row 21, "Ruling
		// 2026-09-26"). The tick no longer waits on the device, so it can spin; a sample is taken
		// only when GetGpuDispatchCount() has moved since the last one, so a spinning tick adds
		// no duplicate readings.
		int64 LastSampledDispatch = Gpu->GetGpuDispatchCount();
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - StartSeconds < 180.0)
		{
			Gpu->Tick(1.0f / 60.0f);
			const int64 Dispatch = Gpu->GetGpuDispatchCount();
			if (Dispatch != LastSampledDispatch)
			{
				GpuBusySamples.Add(Gpu->GetLastGpuBusyMs());
				HostFinishSamples.Add(Gpu->GetLastHostFinishMs());
				LastSampledDispatch = Dispatch;
			}
			if (Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Complete || Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Faulted)
			{
				break;
			}
			FPlatformProcess::Sleep(0);
		}
		const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
		const ESuperSLMSequencePhase FinalPhase = Gpu->GetPhase(Seq);
		const TArray<int32> Tokens = Gpu->GetGeneratedTokens(Seq);
		Gpu->ReturnSequence(Seq);
		if (!TestEqual(TEXT("slice measurement must reach Complete before the 180-second deadline"),
			(uint8)FinalPhase, (uint8)ESuperSLMSequencePhase::Complete))
		{
			return false;
		}

		GpuBusySamples.Sort();
		HostFinishSamples.Sort();
		FSliceMeasurement M;
		M.LayersPerSlice = LayersPerSlice;
		M.MedianGpuBusyMs = GpuBusySamples.Num() > 0 ? GpuBusySamples[GpuBusySamples.Num() / 2] : 0.0;
		M.MedianHostFinishMs = HostFinishSamples.Num() > 0 ? HostFinishSamples[HostFinishSamples.Num() / 2] : 0.0;
		M.TokensPerSecond = ElapsedSeconds > 0.0 ? Tokens.Num() / ElapsedSeconds : 0.0;

		double MedianPromptSliceMs = 0.0;
		if (!MeasurePromptSliceGpuBusyMs(*this, World, LayersPerSlice, MedianPromptSliceMs))
		{
			return false;
		}
		M.MedianPromptSliceGpuBusyMs = MedianPromptSliceMs;
		Measurements.Add(M);

		AddInfo(FString::Printf(TEXT("headless: layers/slice=%d medianGpuBusyMs=%f medianHostFinishMs=%f tok/s=%f medianPromptSliceGpuBusyMs=%f"),
			LayersPerSlice, M.MedianGpuBusyMs, M.MedianHostFinishMs, M.TokensPerSecond, M.MedianPromptSliceGpuBusyMs));
	}

	// The one-call path's own throughput, for the "throughput cost against the
	// one-call path" comparison (plan §9 R-S2f).
	{
		USuperSLMModel* Model = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
		if (SetUpGpu(*this, World, DispatchBudgetForLayersPerSlice(AExNumHiddenLayers), Model, Gpu))
		{
			FSuperSLMGpuSequence Seq;
			if (Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall) == ESuperSLMGpuVendResult::Success)
			{
				FSuperSLMGenerationRequest Request;
				Request.PromptTokens = {1, 2, 3, 4, 5};
				Request.MaxNewTokens = kMeasurementTokenCount;
				Request.SpanKind = ESuperSLMSpanKind::Prompt;
				TArray<int32> Tokens;
				FString RunError;
				const double StartSeconds = FPlatformTime::Seconds();
				const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, /*MaxWallClockSeconds*/ 180.0, RunError);
				const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
				Gpu->ReturnSequence(Seq);
				if (!TestTrue(*FString::Printf(TEXT("one-call measurement must complete: %s"), *RunError), bCompleted))
				{
					return false;
				}
				AddInfo(FString::Printf(TEXT("one-call path (headless): tok/s=%f"), ElapsedSeconds > 0.0 ? Tokens.Num() / ElapsedSeconds : 0.0));
			}
		}
	}

	// The PROVISIONAL default-slice selection rule (plan §9 R-S2f: "the largest setting
	// whose median gpu_busy_ms is <= 2 ms") -- reported, never asserted as pass/fail,
	// because this cell is a measurement (§9's own kind column). R-S3f re-selects this
	// under UE's own rendering load (D-SLM7336); this figure is not final.
	// D-SLM7391: BOTH the decode-slice AND the prompt-slice median must clear the
	// target for a setting to qualify -- a setting that only clears decode (the old
	// rule) is not the default if its OWN prompt slices overshoot.
	int32 ProvisionalLayersPerSlice = -1;
	for (const FSliceMeasurement& M : Measurements)
	{
		if (M.MedianGpuBusyMs <= kTargetGpuBusyMs && M.MedianPromptSliceGpuBusyMs <= kTargetGpuBusyMs)
		{
			ProvisionalLayersPerSlice = FMath::Max(ProvisionalLayersPerSlice, M.LayersPerSlice);
		}
	}
	AddInfo(FString::Printf(TEXT("PROVISIONAL default-slice selection (headless, D-SLM7336/D-SLM7391): layers/slice=%d "
		"(both decode- and prompt-slice medians must clear %.1f ms; no headless setting cleared both if this reads -1; re-selected by R-S3f)"),
		ProvisionalLayersPerSlice, kTargetGpuBusyMs));

	// Structural success only -- every arm must have run and produced a completed
	// generation; the NUMBERS themselves are reported (AddInfo above), never gated.
	return Measurements.Num() == (kMaxLayersPerSlice - kMinLayersPerSlice + 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuHeadOffCostTest,
	"SuperSLM.U1.Gpu.HeadOffCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuHeadOffCostTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	for (int32 Layers = kMinLayersPerSlice; Layers <= kMaxLayersPerSlice + 1; ++Layers)
	{
		const bool bOneCall = Layers > kMaxLayersPerSlice;
		USuperSLMModel* Model = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
		if (!SetUpGpu(*this, World.GetTestWorld(),
				DispatchBudgetForLayersPerSlice(bOneCall ? AExNumHiddenLayers : Layers),
				Model, Gpu, /*bHeadOn*/ false)) { return false; }
		FSuperSLMGpuSequence Seq;
		const ESuperSLMGpuDecodePath Path = bOneCall ? ESuperSLMGpuDecodePath::OneCall : ESuperSLMGpuDecodePath::Composed;
		if (!TestEqual(TEXT("vend"), (uint8)Gpu->VendSequence(Seq, Path), (uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = {1, 2, 3, 4, 5};
		Request.MaxNewTokens = kMeasurementTokenCount;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		if (!TestTrue(TEXT("begin"), Gpu->RequestBeginGeneration(Seq, Request).IsValid())) { return false; }
		TArray<double> HostFinishSamples;
		const double Start = FPlatformTime::Seconds();
		int32 LastObserved = 0;
		// Unpaced; a sample needs a new token AND a moved GetGpuDispatchCount() (ruling 2026-09-26).
		int64 LastSampledDispatch = Gpu->GetGpuDispatchCount();
		while (FPlatformTime::Seconds() - Start < 180.0)
		{
			Gpu->Tick(1.0f / 60.0f);
			const int32 Observed = Gpu->GetGeneratedTokens(Seq).Num();
			const int64 Dispatch = Gpu->GetGpuDispatchCount();
			if (Observed > LastObserved && Dispatch != LastSampledDispatch)
			{
				HostFinishSamples.Add(Gpu->GetLastHostFinishMs());
				LastObserved = Observed;
				LastSampledDispatch = Dispatch;
			}
			const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) { break; }
			FPlatformProcess::Sleep(0);
		}
		const double Elapsed = FPlatformTime::Seconds() - Start;
		const ESuperSLMSequencePhase FinalPhase = Gpu->GetPhase(Seq);
		const int32 Count = Gpu->GetGeneratedTokens(Seq).Num();
		Gpu->ReturnSequence(Seq);
		if (!TestEqual(TEXT("head-off measurement completes"), (uint8)FinalPhase, (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		HostFinishSamples.Sort();
		const double MedianHostMs = HostFinishSamples.Num() > 0 ? HostFinishSamples[HostFinishSamples.Num() / 2] : 0.0;
		AddInfo(FString::Printf(TEXT("head off, %s %d layers/slice: %d tokens, %.3f tok/s, median host finish %.3f ms, %d finish samples"),
			bOneCall ? TEXT("one-call") : TEXT("composed"), bOneCall ? AExNumHiddenLayers : Layers,
			Count, Elapsed > 0.0 ? Count / Elapsed : 0.0, MedianHostMs, HostFinishSamples.Num()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuDeepSliceCostTest,
	"SuperSLM.U1.Gpu.DeepSliceCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuDeepSliceCostTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	constexpr int32 Depth = 3072;
	constexpr int32 DecodeTokens = 32;
	constexpr int32 LayersPerSlice = 4;
	if (!SetUpGpu(*this, World.GetTestWorld(), DispatchBudgetForLayersPerSlice(LayersPerSlice), Model, Gpu)) { return false; }
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("deep-reading vend"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed),
		(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens.Init(1, Depth);
	Request.MaxNewTokens = DecodeTokens;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	if (!TestTrue(TEXT("deep-reading begin"), Gpu->RequestBeginGeneration(Seq, Request).IsValid())) { return false; }
	TArray<double> BusySamples, FinishSamples;
	int32 LastObserved = 0;
	double DecodeStart = 0.0;
	// Unpaced; each sample stream takes a reading only when GetGpuDispatchCount() has moved since
	// its own last sample, so a spinning tick adds no duplicates (ruling 2026-09-26).
	int64 LastBusyDispatch = Gpu->GetGpuDispatchCount();
	int64 LastFinishDispatch = LastBusyDispatch;
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < 600.0)
	{
		const ESuperSLMSequencePhase Before = Gpu->GetPhase(Seq);
		if (Before == ESuperSLMSequencePhase::Decoding && DecodeStart == 0.0) { DecodeStart = FPlatformTime::Seconds(); }
		Gpu->Tick(1.0f / 60.0f);
		const int64 Dispatch = Gpu->GetGpuDispatchCount();
		if (Before == ESuperSLMSequencePhase::Decoding && Dispatch != LastBusyDispatch)
		{
			BusySamples.Add(Gpu->GetLastGpuBusyMs());
			LastBusyDispatch = Dispatch;
		}
		const int32 Observed = Gpu->GetGeneratedTokens(Seq).Num();
		if (Observed > LastObserved && Dispatch != LastFinishDispatch)
		{
			FinishSamples.Add(Gpu->GetLastHostFinishMs());
			LastObserved = Observed;
			LastFinishDispatch = Dispatch;
		}
		const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Seq);
		if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) { break; }
		FPlatformProcess::Sleep(0);
	}
	const double DecodeSeconds = DecodeStart > 0.0 ? FPlatformTime::Seconds() - DecodeStart : 0.0;
	const ESuperSLMSequencePhase FinalPhase = Gpu->GetPhase(Seq);
	const int32 Count = Gpu->GetGeneratedTokens(Seq).Num();
	Gpu->ReturnSequence(Seq);
	if (!TestEqual(TEXT("deep reading completes"), (uint8)FinalPhase, (uint8)ESuperSLMSequencePhase::Complete) ||
		!TestEqual(TEXT("deep reading delivers 32 tokens"), Count, DecodeTokens) ||
		!TestTrue(TEXT("deep reading sampled decode slices"), !BusySamples.IsEmpty()) ||
		!TestTrue(TEXT("deep reading sampled host finishes"), !FinishSamples.IsEmpty())) { return false; }
	BusySamples.Sort();
	FinishSamples.Sort();
	AddInfo(FString::Printf(TEXT("headless GPU depth >= %d, composed %d layers/slice: %d tokens, median gpu_busy_ms %.6f, median host finish %.6f ms, %.3f tok/s"),
		Depth, LayersPerSlice, Count, BusySamples[BusySamples.Num() / 2], FinishSamples[FinishSamples.Num() / 2],
		DecodeSeconds > 0.0 ? Count / DecodeSeconds : 0.0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
