// T-2818 (L2-S3 red suite). R-S3a (the plan §9): "The
// runtime-LoRA switch changes the answer (dim 10, D-SLM27) -- A-AD: the same prompt with the
// base, then the adapter re-pointed -> different in-character output, reproducible under each;
// the plugin's measured re-point time reported. Covers CPU and GPU paths."

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMGpuTypes.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Tests/AutomationCommon.h" // FTestWorldWrapper (L2-S3 mechanical repair, 2026-09-25)

namespace
{
	// A-AD's own base model + adapter (plan §9), matching R-S1f.5's own
	// FSuperSLMAdapterImport::ImportFromFile usage pattern
	// (the red-suite record §2, R-S1f.5).
	bool LoadAAd(USuperSLMModel*& OutBase, FSuperSLMAdapterHandle& OutAdapter, FString& OutError)
	{
		FSuperSLMImportDiagnostic Diag;
		OutBase = FSuperSLMModelImport::ImportFromFile(SuperSLML2S3Fixtures::AAdBaseModelPath(), Diag);
		if (OutBase == nullptr)
		{
			OutError = FString::Printf(TEXT("A-AD base import failed: %s"), *Diag.Message);
			return false;
		}
		return FSuperSLMAdapterImport::ImportFromFile(
			SuperSLML2S3Fixtures::AAdAdapterPath(), *OutBase, OutAdapter, OutError);
	}

	// One 32-token generation with Adapter applied BEFORE BeginGeneration (an Idle sequence has
	// no pending token boundary, so the swap is expected to be live by the time generation
	// starts -- R-S1f.5's own "deferred to the token boundary" contract is about a swap
	// requested STRICTLY MID-TOKEN; requesting on a fresh/Idle sequence has no mid-token state
	// to defer past). Measures the swap's own wall-clock cost (§9: "the plugin's measured
	// re-point time reported") around the request plus the one Tick() that would apply a
	// deferred swap, so the measurement is honest whichever timing the implementation implements.
	bool RunWithAdapter(
		USuperSLMSubsystem& Subsystem,
		const FSuperSLMSequence& Seq,
		const FSuperSLMAdapterHandle& Adapter, // IsValid() == false selects base (clears to base)
		const TArray<int32>& PromptTokens,
		TArray<int32>& OutTokens,
		double& OutSwapRequestMs,
		double& OutFollowingTickMs,
		FString& OutError)
	{
		// L2-S3 mechanical repair (2026-09-25): ResetSequence() is queued since L2-S1's
		// async rework (SuperSLMSubsystem.h, D-SLM7421); drive its handle to resolution.
		FString ResetError;
		FSuperSLMLifecycleOpHandle ResetHandle;
		if (Subsystem.ResetSequence(Seq, ResetHandle, ResetError) != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("ResetSequence failed: %s"), *ResetError);
			return false;
		}
		ESuperSLMRestoreResult ResetResult = Subsystem.GetLifecycleOpResult(ResetHandle);
		const double ResetStart = FPlatformTime::Seconds();
		while (ResetResult == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - ResetStart < 60.0)
		{
			Subsystem.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
			ResetResult = Subsystem.GetLifecycleOpResult(ResetHandle);
		}
		Subsystem.ReleaseLifecycleOpHandle(ResetHandle);
		if (ResetResult != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("ResetSequence resolved to %d, not Success"), (int32)ResetResult);
			return false;
		}

		// Round 5 (review W2, record §10): the re-point request is timed alone; the tick that lets a
		// deferred swap apply is timed separately, so the reported re-point is not a whole Tick().
		const double SwapStart = FPlatformTime::Seconds();
		Subsystem.RequestAdapterSwap(Seq, Adapter);
		OutSwapRequestMs = (FPlatformTime::Seconds() - SwapStart) * 1000.0;
		const double TickStart = FPlatformTime::Seconds();
		Subsystem.Tick(1.0f / 60.0f); // lets a deferred swap apply if the sequence is Idle
		OutFollowingTickMs = (FPlatformTime::Seconds() - TickStart) * 1000.0;

		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = 32;
		return SuperSLML2S3Fixtures::RunGenerationToCompletion(
			Subsystem, Seq, Request, OutTokens, /*MaxWallClockSeconds*/ 60.0, OutError);
	}
}

namespace
{
	// The prompt both arms feed. A-AD's 1.5B base carries no tokenizer sections (D-SLM7339), so
	// Tokenize() fails against it -- the box run of 2026-09-25 failed both arms there. The arms feed
	// R-S1a's first recorded case's prompt ids instead (Tests/Fixtures/L2S1/l2s1_r_s1a_reference.json),
	// as the other A-AD cells do (SuperSLMU1CpuHolderTests.cpp). They were tokenized by the Qwen2.5
	// tokenizer, which the 0.5B and the 1.5B share, so they are valid ids for A-AD's base.
	bool RecordedAdapterPromptTokens(FAutomationTestBase& Test, TArray<int32>& OutTokens)
	{
		TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
		FString Error;
		// Review round 4, R4-N3: load first, so the message carries the filled-in reason.
		const bool bLoaded = SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error);
		if (!Test.TestTrue(FString::Printf(TEXT("R-S1a recorded cases load (%s)"), *Error), bLoaded) ||
			!Test.TestTrue(TEXT("at least one recorded case"), Cases.Num() > 0))
		{
			return false;
		}
		OutTokens = Cases[0].PromptTokens;
		return Test.TestTrue(TEXT("recorded prompt non-empty"), OutTokens.Num() > 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3AdapterSwitchCpuTest,
	"SuperSLM.L2S3.AdapterSwitch.CpuBaseVsAdapterDiffersAndReproduces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3AdapterSwitchCpuTest::RunTest(const FString& Parameters)
{
	USuperSLMModel* Base = nullptr;
	FSuperSLMAdapterHandle Adapter;
	FString LoadError;
	if (!TestTrue(TEXT("A-AD loads"), LoadAAd(Base, Adapter, LoadError)))
	{
		AddError(LoadError);
		return true;
	}
	if (!TestTrue(TEXT("adapter handle valid"), Adapter.IsValid()))
	{
		return true;
	}

	// L2-S3 mechanical repair (2026-09-25): the subsystem accessors take the test's own
	// FTestWorldWrapper world (T-2815 build log D1).
	FTestWorldWrapper World;
	if (!TestTrue(TEXT("game world created"), World.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	USuperSLMSubsystem* Subsystem = SuperSLML2S3Fixtures::GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 512;
	Config.MaxLayerBudget = 28; // Qwen2.5-1.5B-Instruct's own num_hidden_layers (plan §9 A-AD)
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 100000.0;
	const FSuperSLMConfigureReport ConfigureReport = Subsystem->Configure(Base, Config);
	if (!TestEqual(TEXT("configure ok"), (uint8)ConfigureReport.Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		AddError(ConfigureReport.Message);
		return true;
	}

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return true;
	}

	TArray<int32> PromptTokens;
	if (!RecordedAdapterPromptTokens(*this, PromptTokens))
	{
		return false;
	}

	// --- Base arm, twice (reproducibility) ---
	TArray<int32> BaseTokensA, BaseTokensB;
	double SwapMsUnused = 0.0, TickMsUnused = 0.0;
	FString RunError;
	if (!TestTrue(TEXT("base run A"),
			RunWithAdapter(*Subsystem, Seq, FSuperSLMAdapterHandle{}, PromptTokens, BaseTokensA, SwapMsUnused, TickMsUnused, RunError)))
	{
		AddError(RunError);
		return true;
	}
	if (!TestTrue(TEXT("base run B"),
			RunWithAdapter(*Subsystem, Seq, FSuperSLMAdapterHandle{}, PromptTokens, BaseTokensB, SwapMsUnused, TickMsUnused, RunError)))
	{
		AddError(RunError);
		return true;
	}
	TestEqual(TEXT("base arm reproduces"), BaseTokensA, BaseTokensB);

	// --- Adapter arm, twice (reproducibility), with the swap's own wall-clock reported ---
	TArray<int32> AdapterTokensA, AdapterTokensB;
	double SwapMsA = 0.0, SwapMsB = 0.0, TickMsA = 0.0, TickMsB = 0.0;
	if (!TestTrue(TEXT("adapter run A"),
			RunWithAdapter(*Subsystem, Seq, Adapter, PromptTokens, AdapterTokensA, SwapMsA, TickMsA, RunError)))
	{
		AddError(RunError);
		return true;
	}
	if (!TestTrue(TEXT("adapter run B"),
			RunWithAdapter(*Subsystem, Seq, Adapter, PromptTokens, AdapterTokensB, SwapMsB, TickMsB, RunError)))
	{
		AddError(RunError);
		return true;
	}
	TestEqual(TEXT("adapter arm reproduces"), AdapterTokensA, AdapterTokensB);
	AddInfo(FString::Printf(TEXT("CPU adapter re-point: RequestAdapterSwap() %.3f ms / %.3f ms (runs A/B); the following Tick(), timed separately: %.3f ms / %.3f ms"),
		SwapMsA, SwapMsB, TickMsA, TickMsB));

	// --- The achievement claim itself: base and adapter differ (FEAT, real generation) ---
	TestFalse(TEXT("base and adapter outputs differ"), BaseTokensA == AdapterTokensA);

	Subsystem->ReturnSequence(Seq);
	return true;
}

namespace
{
	// One 32-token GPU generation with Adapter applied before BeginGeneration, mirroring
	// RunWithAdapter's own CPU-side shape but against USuperSLMGpuSubsystem's own distinct
	// handle/config types (SuperSLMGpuSubsystem.h, T-2816).
	bool RunWithAdapterGpu(
		USuperSLMGpuSubsystem& GpuSubsystem,
		const FSuperSLMGpuSequence& Seq,
		const FSuperSLMGpuAdapterHandle& Adapter, // IsValid() == false selects base
		const TArray<int32>& PromptTokens,
		TArray<int32>& OutTokens,
		double& OutSwapRequestMs,
		double& OutFollowingTickMs,
		FString& OutError)
	{
		// L2-S3 mechanical repair (2026-09-25): the GPU lifecycle API is the per-sequence
		// queue only (D-SLM7457); RequestResetSequence() driven to resolution by L2-S2's helper.
		const ESuperSLMRestoreResult ResetResult = SuperSLML2S2Fixtures::DriveLifecycleOpToResolution(
			GpuSubsystem, GpuSubsystem.RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall));
		if (ResetResult != ESuperSLMRestoreResult::Success)
		{
			OutError = FString::Printf(TEXT("GPU RequestResetSequence resolved to %d (%s)"),
				(int32)ResetResult, *GpuSubsystem.GetLastLifecycleRequestError());
			return false;
		}

		// Timed as on the CPU (review W2): the request alone, then the following tick.
		const double SwapStart = FPlatformTime::Seconds();
		GpuSubsystem.RequestAdapterSwap(Seq, Adapter);
		OutSwapRequestMs = (FPlatformTime::Seconds() - SwapStart) * 1000.0;
		const double TickStart = FPlatformTime::Seconds();
		GpuSubsystem.Tick(1.0f / 60.0f); // lets a deferred swap apply if the sequence is Idle
		OutFollowingTickMs = (FPlatformTime::Seconds() - TickStart) * 1000.0;

		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = 32;
		if (!GpuSubsystem.RequestBeginGeneration(Seq, Request).IsValid()) // mechanical repair, as above
		{
			OutError = FString::Printf(TEXT("GPU RequestBeginGeneration failed: %s"), *GpuSubsystem.GetLastLifecycleRequestError());
			return false;
		}

		constexpr float StepSeconds = 1.0f / 60.0f;
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - StartSeconds < 60.0)
		{
			GpuSubsystem.Tick(StepSeconds);
			const ESuperSLMSequencePhase Phase = GpuSubsystem.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete)
			{
				OutTokens = GpuSubsystem.GetGeneratedTokens(Seq);
				return true;
			}
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				OutError = TEXT("GPU sequence faulted before completion");
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		OutError = TEXT("GPU generation did not reach Complete/Faulted within 60 seconds");
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3AdapterSwitchGpuTest,
	"SuperSLM.L2S3.AdapterSwitch.GpuBaseVsAdapterDiffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3AdapterSwitchGpuTest::RunTest(const FString& Parameters)
{
	// §9 R-S3a: "Covers CPU and GPU paths." Against USuperSLMGpuSubsystem (SuperSLMGpuSubsystem.h,
	// T-2816's own L2-S2 declaration, committed to this same worktree -- read, not guessed).
	// L2-S3 mechanical repair (2026-09-25): world-scoped accessors, as in the CPU arm.
	FTestWorldWrapper World;
	if (!TestTrue(TEXT("game world created"), World.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	USuperSLMGpuSubsystem* GpuSubsystem = SuperSLML2S3Fixtures::GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), GpuSubsystem))
	{
		return true;
	}

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(SuperSLML2S3Fixtures::AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base imports"), Base))
	{
		AddError(Diag.Message);
		return true;
	}

	FSuperSLMGpuRuntimeConfig GpuConfig;
	GpuConfig.ContextCap = 4096;
	GpuConfig.BlockCount = 1;
	// One whole layer per composed slice, in Layer-1 dispatch units. The earlier value, 1, was one
	// DISPATCH, below one layer's 24, so Configure() refused it (InvalidDispatchBudget) and the arm
	// failed on configuration, never reaching the adapter. The sequence below runs the one-call
	// path; Configure() still validates the composed budget. (2026-09-25, record §9.)
	GpuConfig.DispatchBudget = SuperSLML2S3Fixtures::DispatchBudgetForLayers(1, /*bHasQkNorm*/ false); // Qwen2.5-1.5B: legacy, no QK norm
	// K must be at least the larger of the two paths' floors (SuperSLMGpuRuntimeConfig.h): composed
	// at 1 layer per tick needs num_hidden_layers ticks per token (28 for Qwen2.5-1.5B, from its
	// config.json; confirm on the machine), one-call needs BlockCount (1). The earlier K = 8 is
	// below 28 and was refused KBelowMinimum. 64 clears the floor with margin.
	GpuConfig.K = 64;
	GpuConfig.TickBudgetMs = 100000.0;
	const FSuperSLMGpuConfigureReport ConfigureReport = GpuSubsystem->Configure(Base, GpuConfig);
	if (ConfigureReport.Result == ESuperSLMGpuConfigureResult::DeviceUnavailable ||
		ConfigureReport.Result == ESuperSLMGpuConfigureResult::ShaderStagingIncomplete)
	{
		// D-SLM7519: §9 R-S3a claims the GPU path, so an unusable device fails the arm by name
		// rather than warning and passing.
		AddError(FString::Printf(
			TEXT("R-S3a GPU arm cannot run: GPU Configure() reported %d (%s) -- no usable GPU "
				"device or incomplete shader staging on this box."),
			(int32)ConfigureReport.Result, *ConfigureReport.Message));
		return false;
	}
	if (!TestEqual(TEXT("GPU configure ok"), (uint8)ConfigureReport.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		AddError(ConfigureReport.Message);
		return true;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("GPU vend"), (uint8)GpuSubsystem->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::Success))
	{
		return true;
	}

	FSuperSLMGpuAdapterHandle Adapter;
	FString MapError;
	if (!TestTrue(TEXT("GPU adapter maps"),
			GpuSubsystem->MapAdapter(SuperSLML2S3Fixtures::AAdAdapterPath(), *Base, Adapter, MapError)))
	{
		AddError(MapError);
		return true;
	}

	TArray<int32> PromptTokens;
	if (!RecordedAdapterPromptTokens(*this, PromptTokens))
	{
		return false;
	}

	// Round 5 (review W2, record §10): each arm runs twice, so "reproducible under each" (plan §9
	// R-S3a) is checked on the GPU as on the CPU, and the re-point is timed apart from the tick.
	TArray<int32> BaseTokensA, BaseTokensB, AdapterTokensA, AdapterTokensB;
	double SwapMs[4] = {}, TickMs[4] = {};
	FString RunError;
	struct FGpuRun { const TCHAR* Label; bool bAdapter; TArray<int32>* Out; };
	const FGpuRun Runs[4] = {
		{ TEXT("gpu base run A"), false, &BaseTokensA },
		{ TEXT("gpu base run B"), false, &BaseTokensB },
		{ TEXT("gpu adapter run A"), true, &AdapterTokensA },
		{ TEXT("gpu adapter run B"), true, &AdapterTokensB },
	};
	for (int32 i = 0; i < 4; ++i)
	{
		if (!TestTrue(Runs[i].Label,
				RunWithAdapterGpu(*GpuSubsystem, Seq, Runs[i].bAdapter ? Adapter : FSuperSLMGpuAdapterHandle{}, PromptTokens,
					*Runs[i].Out, SwapMs[i], TickMs[i], RunError)))
		{
			AddError(RunError);
			return false;
		}
	}
	TestEqual(TEXT("GPU base arm reproduces"), BaseTokensA, BaseTokensB);
	TestEqual(TEXT("GPU adapter arm reproduces"), AdapterTokensA, AdapterTokensB);
	AddInfo(FString::Printf(TEXT("GPU adapter re-point: RequestAdapterSwap() %.3f ms / %.3f ms (runs A/B); the following Tick(), timed separately: %.3f ms / %.3f ms"),
		SwapMs[2], SwapMs[3], TickMs[2], TickMs[3]));
	TestFalse(TEXT("GPU base and adapter outputs differ"), BaseTokensA == AdapterTokensA);

	GpuSubsystem->UnmapAdapter(Adapter);
	GpuSubsystem->ReturnSequence(Seq);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
