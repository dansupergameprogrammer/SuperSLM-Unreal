// The example's own path, run as the scene runs it: configure both backends through
// BeginConfigure(), step the determinism self-check one bounded slice per frame on the game thread,
// then answer the default query on the CPU and on the GPU, and hold each to its frame-cost bound.
// This is the path the scene takes on load; the plugin's own self-check tests do not cover the
// example's use of it.
//
// The test is one latent command advanced once per engine frame, so the engine keeps running
// between steps (BeginConfigure()'s callbacks arrive on the game thread, TickOncePerFrame() sees a
// new frame) and nothing here blocks the game thread.
//
// The frame-cost assertions read the plugin's own figures, never the example's timing:
//  - CPU: every game-thread Tick() of the query (plan and apply, USuperSLMSubsystem::GetTickHistory())
//    stays under SuperSLMExample::CpuGameThreadTickBoundMs, the plugin's own per-tick bound.
//  - GPU: the Frame Budget bounds what one tick issues. At the default Frame Budget the backend
//    issues exactly that many layers per tick (GetLayersPerTick()), and the median per-slice
//    gpu_busy_ms (GetLastGpuBusyMs(), one reading per frame that issued work) is at or under
//    SuperSLMExample::GpuSliceBudgetMs, the criterion the default slice was selected by (measured
//    headless at context depths up to about 400 tokens; the query's prompts are shorter).
//  - Self-check: no single frame of the stepped self-check (one TickFrame(), which is one Step())
//    takes SelfCheckFrameBoundMs or more. The step waits on neither backend (T-2818 nonblock), so a
//    frame costs the step budget plus one tick's overshoot and, at an arm boundary, the small
//    reference and prior-run files.
// A figure that cannot be read (no GPU backend, no slice reading, no tick history) fails the test
// by name; it is never skipped.
//
// The model is the example asset (/Game/SuperSLMExample/ExampleModel) when the project has it, or
// the .sslm named by the SUPERSLM_EXAMPLE_MODEL_FILE environment variable. With neither, the test
// fails and says so: it has no default path.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SuperSLMExampleDemo.h"
#include "SuperSLMExampleScene.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMImportDiagnostic.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMExampleTests
{
	// A limit on waiting for each phase, not a measurement: a phase not finished by then has failed.
	constexpr double PhaseWaitLimitSeconds = 600.0;

	// T-2818 nonblock: the longest one self-check frame may take. A declared bound, not a
	// measurement: SuperSLMExample::SelfCheckStepBudgetSeconds (2 ms) of ticking, one tick's
	// overshoot, and at the arm boundaries a vend and the reference and prior-run reads and writes
	// (small JSON files) fit well inside it, while a step that waits for a GPU submission job --
	// the packaged run's 1,074 ms frame -- does not. It bounds TickFrame() only, never the engine's
	// frame around it.
	constexpr double SelfCheckFrameBoundMs = 50.0;

	USuperSLMModel* LoadExampleModel(FAutomationTestBase& Test)
	{
		const FString PackagePath = FPackageName::ObjectPathToPackageName(FString(ASuperSLMExampleScene::GetDefaultModelPath()));
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			if (USuperSLMModel* Asset = LoadObject<USuperSLMModel>(nullptr, ASuperSLMExampleScene::GetDefaultModelPath()))
			{
				return Asset;
			}
		}
		const FString File = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_EXAMPLE_MODEL_FILE"));
		if (File.IsEmpty())
		{
			Test.AddError(FString::Printf(TEXT("No example model: import the release's .sslm to %s, or set SUPERSLM_EXAMPLE_MODEL_FILE to its path."),
				ASuperSLMExampleScene::GetDefaultModelPath()));
			return nullptr;
		}
		FSuperSLMImportDiagnostic Diagnostic;
		USuperSLMModel* Imported = FSuperSLMModelImport::ImportFromFile(FPaths::ConvertRelativePathToFull(File), Diagnostic);
		if (Imported == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("SUPERSLM_EXAMPLE_MODEL_FILE (%s) did not import: %s (%s)"), *File, *Diagnostic.Message, *Diagnostic.StatusName));
		}
		return Imported;
	}

	double Median(TArray<double> Values)
	{
		Values.Sort();
		const int32 Mid = Values.Num() / 2;
		return (Values.Num() % 2) ? Values[Mid] : 0.5 * (Values[Mid - 1] + Values[Mid]);
	}

	enum class EPhase : uint8
	{
		Configuring,
		SelfCheck,
		CpuQuery,
		GpuQuery,
		Done,
	};

	// Everything the per-frame command owns. Destroyed when the command finishes, which destroys
	// the demo before the world.
	struct FRun
	{
		FTestWorldWrapper World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TUniquePtr<FSuperSLMExampleDemo> Demo;
		EPhase Phase = EPhase::Configuring;
		double PhaseDeadline = 0.0;
		int32 PhaseFrames = 0;
		double MaxTickFrameMs = 0.0; // reported, not asserted: the assertions read the plugin's figures
		double MaxSelfCheckFrameMs = 0.0; // asserted against SelfCheckFrameBoundMs (T-2818 nonblock)
		double MaxCpuQueryFrameMs = 0.0; // reported; at most one CPU query frame may exceed SelfCheckFrameBoundMs
		double MaxGpuQueryFrameMs = 0.0; // reported; at most one GPU query frame may exceed SelfCheckFrameBoundMs (ruling 2026-09-26, round 3)
		int32 CpuQueryFramesOverBound = 0;
		int32 GpuQueryFramesOverBound = 0;

		~FRun()
		{
			Demo.Reset();
		}
	};

	void EnterPhase(FRun& Run, EPhase Phase)
	{
		Run.Phase = Phase;
		Run.PhaseDeadline = FPlatformTime::Seconds() + PhaseWaitLimitSeconds;
		Run.PhaseFrames = 0;
	}

	const TCHAR* PhaseName(EPhase Phase)
	{
		switch (Phase)
		{
		case EPhase::Configuring: return TEXT("configuring the backends");
		case EPhase::SelfCheck: return TEXT("the self-check");
		case EPhase::CpuQuery: return TEXT("the CPU query");
		case EPhase::GpuQuery: return TEXT("the GPU query");
		case EPhase::Done: return TEXT("done");
		}
		return TEXT("unknown");
	}

	// The query checks both backends share.
	bool CheckAnswer(FAutomationTestBase& Test, const TCHAR* Backend, const FSuperSLMExampleReadout& R)
	{
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the query is done (%s)"), Backend, *R.Error), R.State == ESuperSLMExampleRunState::Done))
		{
			return false;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: the extraction is schema-valid (%s: %s)"), Backend, *R.ExtractionText, *R.ExtractionValidation), R.bExtractionSchemaValid);
		Test.TestFalse(FString::Printf(TEXT("%s: the voiced reply is not empty"), Backend), R.ReplyText.IsEmpty());
		Test.TestTrue(FString::Printf(TEXT("%s: the answer took more than one frame"), Backend), R.FramesToAnswer > 1);
		Test.TestEqual(FString::Printf(TEXT("%s: the token digest is 64 hex characters"), Backend), R.TokenDigestHex.Len(), 64);
		Test.TestEqual(FString::Printf(TEXT("%s: every query produced the first query's digest"), Backend), R.QueriesMatchingDigest, R.QueryCount);
		return true;
	}

	void CheckCpuFrameCost(FAutomationTestBase& Test, const FSuperSLMExampleReadout& R)
	{
		if (R.CpuTicks == 0 || R.CpuMaxTickMs < 0.0)
		{
			Test.AddError(TEXT("CPU frame cost: no game-thread tick reading (USuperSLMSubsystem::GetTickHistory() has no row for this query), so the bound cannot be checked"));
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("CPU frame cost: every game-thread Tick() of the query (%d ticks, longest %.3f ms) is under the plugin's %.1f ms bound"),
			R.CpuTicks, R.CpuMaxTickMs, SuperSLMExample::CpuGameThreadTickBoundMs), R.CpuMaxTickMs < SuperSLMExample::CpuGameThreadTickBoundMs);
	}

	void CheckGpuFrameCost(FAutomationTestBase& Test, const FSuperSLMExampleReadout& R)
	{
		Test.TestEqual(TEXT("GPU frame cost: the backend issues the Frame Budget's layers per tick (GetLayersPerTick())"),
			R.GpuLayersPerTick, R.Settings.FrameBudgetLayers);
		if (R.GpuBusyMsSamples.Num() == 0)
		{
			Test.AddError(TEXT("GPU frame cost: no per-slice gpu_busy_ms reading (GetLastGpuBusyMs() never changed during the query), so the bound cannot be checked"));
			return;
		}
		double Max = 0.0;
		for (double V : R.GpuBusyMsSamples)
		{
			Max = FMath::Max(Max, V);
		}
		const double Med = Median(R.GpuBusyMsSamples);
		Test.TestTrue(FString::Printf(TEXT("GPU frame cost: median per-slice gpu_busy_ms %.3f (max %.3f, %d readings) at %d layers per slice is at or under the %.1f ms slice target"),
			Med, Max, R.GpuBusyMsSamples.Num(), R.Settings.FrameBudgetLayers, SuperSLMExample::GpuSliceBudgetMs), Med <= SuperSLMExample::GpuSliceBudgetMs);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMExampleSelfCheckPathTest,
	"SuperSLMExample.Demo.SelfCheckStepsOnGameThreadThenQueryAnswers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMExampleSelfCheckPathTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMExampleTests;

	TSharedRef<FRun> Run = MakeShared<FRun>();
	if (!Run->World.CreateTestWorld(EWorldType::Game) || Run->World.GetTestWorld()->GetGameInstance() == nullptr)
	{
		AddError(TEXT("could not create a game world with a game instance"));
		return false;
	}
	UGameInstance* GameInstance = Run->World.GetTestWorld()->GetGameInstance();
	USuperSLMSubsystem* Cpu = GameInstance->GetSubsystem<USuperSLMSubsystem>();
	USuperSLMGpuSubsystem* Gpu = GameInstance->GetSubsystem<USuperSLMGpuSubsystem>();
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu))
	{
		return false;
	}
	Run->Model.Reset(LoadExampleModel(*this));
	if (!Run->Model.IsValid())
	{
		return false;
	}

	// Configure: BeginConfigure() on both backends, completed by callbacks on later frames.
	Run->Demo = MakeUnique<FSuperSLMExampleDemo>(*Run->Model, *Cpu, Gpu);
	Run->Demo->BeginInitialize();
	TestFalse(TEXT("BeginInitialize() returns before the backends are configured (the heavy part is not on this frame)"),
		Run->Demo->IsInitialized() && !Run->Demo->HasInitializationFailed());
	EnterPhase(*Run, EPhase::Configuring);

	FAutomationTestBase* Test = this;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Run]() -> bool
	{
		FSuperSLMExampleDemo& Demo = *Run->Demo;
		if (FPlatformTime::Seconds() > Run->PhaseDeadline)
		{
			Test->AddError(FString::Printf(TEXT("%s did not finish within %.0f s"), PhaseName(Run->Phase), PhaseWaitLimitSeconds));
			return true;
		}

		// One frame, as the scene gives it.
		const double Before = FPlatformTime::Seconds();
		Demo.TickFrame(FApp::GetDeltaTime());
		const double FrameMs = (FPlatformTime::Seconds() - Before) * 1000.0;
		Run->MaxTickFrameMs = FMath::Max(Run->MaxTickFrameMs, FrameMs);
		if (Run->Phase == EPhase::SelfCheck)
		{
			Run->MaxSelfCheckFrameMs = FMath::Max(Run->MaxSelfCheckFrameMs, FrameMs);
		}
		else if (Run->Phase == EPhase::CpuQuery)
		{
			Run->MaxCpuQueryFrameMs = FMath::Max(Run->MaxCpuQueryFrameMs, FrameMs);
			Run->CpuQueryFramesOverBound += FrameMs > SelfCheckFrameBoundMs ? 1 : 0;
		}
		else if (Run->Phase == EPhase::GpuQuery)
		{
			Run->MaxGpuQueryFrameMs = FMath::Max(Run->MaxGpuQueryFrameMs, FrameMs);
			Run->GpuQueryFramesOverBound += FrameMs > SelfCheckFrameBoundMs ? 1 : 0;
		}
		++Run->PhaseFrames;

		switch (Run->Phase)
		{
		case EPhase::Configuring:
		{
			if (!Demo.IsInitialized() && !Demo.HasInitializationFailed())
			{
				return false;
			}
			if (!Test->TestTrue(FString::Printf(TEXT("initialized (%s)"), *Demo.GetInitializationError()), Demo.IsInitialized()))
			{
				return true;
			}
			// The self-check, stepped on the game thread. Under the old worker path this asserted in
			// the plugin's game-thread check and the process died; reaching the report is the first
			// assertion.
			if (!Test->TestTrue(TEXT("the self-check starts"), Demo.BeginSelfCheck()))
			{
				return true;
			}
			EnterPhase(*Run, EPhase::SelfCheck);
			return false;
		}

		case EPhase::SelfCheck:
		{
			if (Demo.IsSelfCheckRunning())
			{
				return false;
			}
			Test->TestTrue(TEXT("the self-check produced a report"), Demo.HasSelfCheckReport());
			Test->TestTrue(TEXT("the self-check ran across more than one frame (stepped, not one blocking call)"), Demo.GetLastSelfCheckFrames() > 1);
			Test->TestTrue(FString::Printf(TEXT("no self-check frame waits on a backend: the longest of %d TickFrame() calls took %.2f ms, under the %.0f ms bound"),
				Demo.GetLastSelfCheckFrames(), Run->MaxSelfCheckFrameMs, SelfCheckFrameBoundMs), Run->MaxSelfCheckFrameMs < SelfCheckFrameBoundMs);
			const FSuperSLMSelfCheckReport& Report = Demo.GetSelfCheckReport();
			Test->TestTrue(TEXT("the CPU verdict is readable exactly when the plugin has not quarantined it"), Demo.IsCpuVerdictReadable() == !Report.Cpu.bQuarantined);
			Test->TestTrue(TEXT("the GPU verdict is readable exactly when the plugin has not quarantined it"), Demo.IsGpuVerdictReadable() == !Report.Gpu.bQuarantined);
			if (Test->TestTrue(TEXT("the CPU verdict is readable"), Demo.IsCpuVerdictReadable()))
			{
				Test->TestTrue(TEXT("the CPU arm reached a verdict (not Not Yet Run)"), Report.Cpu.Verdict != ESuperSLMSelfCheckVerdict::NotYetRun);
			}
			if (Report.Gpu.bQuarantined)
			{
				Test->TestFalse(TEXT("a quarantined GPU verdict never makes a GPU run stand for the invariant"), Demo.IsGpuVerified());
			}

			FSuperSLMExampleSettings Settings; // the defaults, as the scene's Ask runs them
			Settings.Backend = ESuperSLMExampleBackend::CPU;
			FString Error;
			if (!Test->TestTrue(TEXT("the default CPU query starts"), Demo.Start(Settings, Error)))
			{
				Test->AddError(Error);
				return true;
			}
			EnterPhase(*Run, EPhase::CpuQuery);
			return false;
		}

		case EPhase::CpuQuery:
		{
			if (Demo.IsBusy())
			{
				return false;
			}
			const FSuperSLMExampleReadout& R = Demo.GetReadout();
			if (CheckAnswer(*Test, TEXT("CPU"), R))
			{
				CheckCpuFrameCost(*Test, R);
			}

			if (!Demo.IsGpuAvailable())
			{
				Test->AddError(FString::Printf(TEXT("GPU frame cost: cannot be read, the GPU backend is unavailable (%s)"), *Demo.GetGpuUnavailableReason()));
				EnterPhase(*Run, EPhase::Done);
				return true;
			}
			FSuperSLMExampleSettings Settings;
			Settings.Backend = ESuperSLMExampleBackend::GPU;
			FString Error;
			if (!Test->TestTrue(TEXT("the default GPU query starts"), Demo.Start(Settings, Error)))
			{
				Test->AddError(Error);
				return true;
			}
			EnterPhase(*Run, EPhase::GpuQuery);
			return false;
		}

		case EPhase::GpuQuery:
		{
			if (Demo.IsBusy())
			{
				return false;
			}
			const FSuperSLMExampleReadout& R = Demo.GetReadout();
			if (CheckAnswer(*Test, TEXT("GPU"), R))
			{
				CheckGpuFrameCost(*Test, R);
			}
			// Plan §2.5 row 21, "Ruling 2026-09-26": the GPU tick no longer waits on the submission
			// thread, so no GPU query frame waits on a backend either. The same
			// declared bound as the self-check's frames. Restated in round 3 (finding 10): at most one
			// query-phase frame may exceed it, so one OS preemption does not fail a correct build,
			// while a wait behind the submission queue recurs on every query and still fails. The
			// maximum and the count are reported. The CPU query phase takes the same rule.
			Test->AddInfo(FString::Printf(TEXT("query-phase TickFrame(): GPU longest %.2f ms, %d over %.0f ms; CPU longest %.2f ms, %d over %.0f ms"),
				Run->MaxGpuQueryFrameMs, Run->GpuQueryFramesOverBound, SelfCheckFrameBoundMs, Run->MaxCpuQueryFrameMs, Run->CpuQueryFramesOverBound, SelfCheckFrameBoundMs));
			Test->TestTrue(FString::Printf(TEXT("the GPU tick no longer waits: at most one of the GPU query phase's TickFrame() calls exceeds the %.0f ms bound (%d did; longest %.2f ms)"),
				SelfCheckFrameBoundMs, Run->GpuQueryFramesOverBound, Run->MaxGpuQueryFrameMs), Run->GpuQueryFramesOverBound <= 1);
			Test->TestTrue(FString::Printf(TEXT("no CPU query frame waits on a backend: at most one of the CPU query phase's TickFrame() calls exceeds the %.0f ms bound (%d did; longest %.2f ms)"),
				SelfCheckFrameBoundMs, Run->CpuQueryFramesOverBound, Run->MaxCpuQueryFrameMs), Run->CpuQueryFramesOverBound <= 1);
			Test->AddInfo(FString::Printf(TEXT("longest TickFrame() over the test: %.2f ms (reported; the assertions read the plugin's figures)"), Run->MaxTickFrameMs));
			EnterPhase(*Run, EPhase::Done);
			return true;
		}

		case EPhase::Done:
			return true;
		}
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
