// T-2818 (L2-S3 suite). USuperSLMQuery, the runtime module's Blueprint query (plan line 173,
// D-SLM3534: "two backends behind one Blueprint-facing interface"; §5 and D-SLM7382: the CPU
// re-issue is "the fallback a user experiences"; review round 2, R2-W1). The surface a packaged
// game uses; every other L2-S3 query cell goes through the editor controller. Record:
// the test record §12.
//
// What the cell drives, from a test world's GameInstance:
//  (a) Creation without a usable GPU. A second world whose GPU subsystem is not configured:
//      CreateQuery() succeeds on the CPU alone, and a GPU query is refused by name at BeginQuery()
//      rather than crashing or silently running on the CPU. A null model is refused by name.
//  (b) One CPU query and one GPU query, each through BeginQuery() once and TickQuery() once per
//      editor frame (latent commands, so frames draw). Each readout reports the backend it ran on,
//      bMovedBackend false and no move message: the D-SLM7382 re-issue fires only on a
//      probe-confirmed device loss, which nothing reaches without a forcing construction, so the
//      cell asserts the fallback's reporting in the state it can reach -- not moved -- and says so.
//  (c) The digest: the two Blueprint readouts and the editor controller's headless run of the
//      same prompt and config on each backend give one TokenDigestHex (plan §8's invariant; the
//      controller is now a client of the same runner, so a divergence is the Blueprint face's).
//  (d) GPU schedule restore (review R2-W3): the query runs at Frame Budget 2 and k 1 against a
//      subsystem configured at 24 layers per tick and K 24. The readout reports the schedule it ran
//      at; once its pool has drained, GetLayersPerTick() and GetConfiguredK() read their values from
//      before the query. Red until the implementation's restore lands.
//  (e) The context-cap refusal: a query whose prompt plus MaxNewTokens exceeds context_cap is
//      refused at BeginQuery() naming context_cap, with nothing left running. This is why the
//      context-cap stop branch is unreachable from a query (SuperSLML2S3StopReasonTests.cpp).
//
// Signatures used: FSuperSLMQueryConfig::
// StopTokenIds, a TArray<int32> the game supplies (review R2-W2; plan §8 "the game supplies" the
// stop ids); the restore in (d) happening by the time the GPU pool has drained. CreateQuery,
// BeginQuery, TickQuery, CancelQuery and IsQueryRunning are as declared in SuperSLMQuery.h.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Misc/App.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMQuery.h"
#include "SuperSLMQueryWindowController.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMQueryFace
{
	constexpr int32 kMaxNewTokens = 32;
	constexpr double kQueryWallSeconds = 300.0;
	constexpr double kRestoreWaitSeconds = 10.0;
	constexpr int32 kGpuFrameBudget = 2;   // differs from the fixture's 24 layers per tick
	constexpr int32 kGpuRequestedK = 1;    // clamped up to the slice floor, which differs from K 24

	FSuperSLMQueryConfig MakeConfig(ESuperSLMBackendBP Backend)
	{
		FSuperSLMQueryConfig Config;
		Config.Backend = Backend;
		Config.FrameBudgetLayers = Backend == ESuperSLMBackendBP::GPU ? kGpuFrameBudget : 0;
		Config.ConcurrentQueries = 1;
		Config.RequestedK = Backend == ESuperSLMBackendBP::GPU ? kGpuRequestedK : 1;
		Config.bSchemaConstrainedDecoding = false;
		// Assumed field (review R2-W2): the game supplies the model's stop set.
		Config.StopTokenIds = { SuperSLML2S3Fixtures::AExStopTokenImEnd, SuperSLML2S3Fixtures::AExStopTokenEndOfText };
		return Config;
	}

	struct FDrive
	{
		bool bBegun = false;
		bool bStopped = false;
		bool bSucceeded = false;
		FSuperSLMQueryReadout Readout;
		FString Error;
		double Deadline = 0.0;
	};

	struct FState
	{
		TSharedPtr<FTestWorldWrapper> World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TStrongObjectPtr<USuperSLMQuery> Query;
		TWeakObjectPtr<USuperSLMSubsystem> Cpu;
		TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
		FDrive CpuRun;
		FDrive GpuRun;
		int32 LayersPerTickBefore = 0;
		int32 KBefore = 0;
		double RestoreDeadline = 0.0;
	};

	// One latent command: Begin once, then one TickQuery per frame until the query stops.
	void EnqueueDrive(FAutomationTestBase* Test, TSharedRef<FState> State, ESuperSLMBackendBP Backend, FDrive FState::* Which,
		TFunction<void(FState&)> BeforeBegin)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, State, Backend, Which, BeforeBegin]() -> bool
		{
			FState& S = *State;
			FDrive& D = S.*Which;
			const TCHAR* Name = Backend == ESuperSLMBackendBP::GPU ? TEXT("GPU") : TEXT("CPU");
			if (!S.Query.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("%s query: no USuperSLMQuery"), Name));
				return true;
			}
			if (!D.bBegun)
			{
				D.bBegun = true;
				if (BeforeBegin)
				{
					BeforeBegin(S);
				}
				D.Deadline = FPlatformTime::Seconds() + kQueryWallSeconds;
				FString Error;
				if (!Test->TestTrue(FString::Printf(TEXT("%s query begins"), Name),
						S.Query->BeginQuery(SuperSLML2S3Fixtures::SweepPromptText(), kMaxNewTokens, MakeConfig(Backend), Error)))
				{
					Test->AddError(FString::Printf(TEXT("%s query: BeginQuery refused: %s"), Name, *Error));
					D.bStopped = true;
					return true;
				}
				return false; // let the frame draw
			}
			if (FPlatformTime::Seconds() > D.Deadline)
			{
				Test->AddError(FString::Printf(TEXT("%s query: not stopped after %.0f s"), Name, kQueryWallSeconds));
				S.Query->CancelQuery();
				D.bStopped = true;
				return true;
			}
			if (S.Query->TickQuery(FApp::GetDeltaTime(), D.bSucceeded, D.Readout, D.Error))
			{
				D.bStopped = true;
				Test->TestTrue(FString::Printf(TEXT("%s query stops with a result (%s)"), Name, *D.Error), D.bSucceeded);
				Test->TestFalse(FString::Printf(TEXT("%s query: IsQueryRunning() is false once stopped"), Name), S.Query->IsQueryRunning());
				return true;
			}
			return false;
		}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3QueryBlueprintFaceTest,
	"SuperSLM.L2S3.Query.BlueprintQueryBothBackendsMatchController",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3QueryBlueprintFaceTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMQueryFace;
	const TCHAR* CellName = TEXT("USuperSLMQuery");

	TSharedRef<FState> State = MakeShared<FState>();
	State->World = MakeShared<FTestWorldWrapper>();
	if (!State->World->CreateTestWorld(EWorldType::Game))
	{
		AddError(TEXT("USuperSLMQuery: the test game world was not created"));
		return false;
	}
	SuperSLML2S3Fixtures::FQueryWindowBackends Backends;
	if (!SuperSLML2S3Fixtures::SetUpQueryWindowBackends(*this, State->World->GetTestWorld(), CellName,
			/*BlockCount*/ 1, /*bRequirePromptResultSchema*/ false, Backends))
	{
		return false;
	}
	State->Model.Reset(Backends.Model);
	State->Cpu = Backends.Cpu;
	State->Gpu = Backends.Gpu;

	// --- (a) Creation refusals and a world with no usable GPU ---
	{
		FString Error;
		TestNull(TEXT("CreateQuery with no model is refused"), USuperSLMQuery::CreateQuery(State->World->GetTestWorld(), nullptr, Error));
		TestFalse(TEXT("the no-model refusal is named"), Error.IsEmpty());
	}
	{
		// A second GameInstance whose CPU is configured with the model and whose GPU is not. Created
		// and destroyed before the main arm runs, so its CPU subsystem does not overlap the main one's
		// queries.
		FTestWorldWrapper NoGpuWorld;
		if (TestTrue(TEXT("second game world created"), NoGpuWorld.CreateTestWorld(EWorldType::Game)))
		{
			USuperSLMSubsystem* Cpu2 = SuperSLML2S3Fixtures::GetSubsystem(NoGpuWorld.GetTestWorld());
			USuperSLMGpuSubsystem* Gpu2 = SuperSLML2S3Fixtures::GetGpuSubsystem(NoGpuWorld.GetTestWorld());
			FSuperSLMRuntimeConfig CpuConfig;
			CpuConfig.MaxSequencesPerDecodeCall = 1;
			CpuConfig.MaxPrefillChunkBudget = 512;
			CpuConfig.MaxLayerBudget = SuperSLML2S3Fixtures::AExNumHiddenLayers;
			CpuConfig.BlockCount = 1;
			CpuConfig.SequenceLifecycleBudgetMs = 100000.0;
			CpuConfig.TickBudgetMs = 100000.0;
			if (TestNotNull(TEXT("second world's CPU subsystem"), Cpu2) &&
				TestTrue(TEXT("second world's GPU subsystem is not active (the precondition)"), Gpu2 == nullptr || !Gpu2->IsGpuBackendActive()) &&
				TestEqual(TEXT("second world's CPU Configure()"), (uint8)Cpu2->Configure(Backends.Model, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success))
			{
				FString Error;
				USuperSLMQuery* CpuOnly = USuperSLMQuery::CreateQuery(NoGpuWorld.GetTestWorld(), Backends.Model, Error);
				if (TestNotNull(FString::Printf(TEXT("CreateQuery succeeds with no usable GPU (%s)"), *Error), CpuOnly))
				{
					FString GpuError;
					TestFalse(TEXT("a GPU query without a usable GPU is refused at BeginQuery"),
						CpuOnly->BeginQuery(SuperSLML2S3Fixtures::SweepPromptText(), kMaxNewTokens, MakeConfig(ESuperSLMBackendBP::GPU), GpuError));
					TestTrue(FString::Printf(TEXT("the refusal names the GPU backend (%s)"), *GpuError), GpuError.Contains(TEXT("GPU")));
					TestFalse(TEXT("nothing is left running after the refusal"), CpuOnly->IsQueryRunning());
					CpuOnly->CancelQuery();
				}
			}
		}
	}

	FString CreateError;
	State->Query.Reset(USuperSLMQuery::CreateQuery(State->World->GetTestWorld(), Backends.Model, CreateError));
	if (!TestNotNull(FString::Printf(TEXT("CreateQuery on the configured world (%s)"), *CreateError), State->Query.Get()))
	{
		return false;
	}

	// --- (b) One CPU query and one GPU query, across frames ---
	EnqueueDrive(this, State, ESuperSLMBackendBP::CPU, &FState::CpuRun, nullptr);
	EnqueueDrive(this, State, ESuperSLMBackendBP::GPU, &FState::GpuRun, [](FState& S)
	{
		if (USuperSLMGpuSubsystem* Gpu = S.Gpu.Get())
		{
			S.LayersPerTickBefore = Gpu->GetLayersPerTick();
			S.KBefore = Gpu->GetConfiguredK();
		}
	});

	// --- (d) The GPU schedule is restored once the query's pool has drained ---
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		USuperSLMGpuSubsystem* Gpu = S.Gpu.Get();
		if (Gpu == nullptr || !S.GpuRun.bSucceeded)
		{
			return true;
		}
		if (S.RestoreDeadline == 0.0)
		{
			S.RestoreDeadline = FPlatformTime::Seconds() + kRestoreWaitSeconds;
			TestEqual(TEXT("GPU readout: AppliedLayersPerTick is the query's Frame Budget"), S.GpuRun.Readout.AppliedLayersPerTick, kGpuFrameBudget);
			TestTrue(FString::Printf(TEXT("GPU readout: the query's schedule differs from the subsystem's prior one (precondition; K %d vs %d, layers %d vs %d)"),
					S.GpuRun.Readout.AppliedK, S.KBefore, S.GpuRun.Readout.AppliedLayersPerTick, S.LayersPerTickBefore),
				S.GpuRun.Readout.AppliedK != S.KBefore && S.GpuRun.Readout.AppliedLayersPerTick != S.LayersPerTickBefore);
		}
		Gpu->Tick(FApp::GetDeltaTime()); // lets deferred returns drain
		const bool bRestored = Gpu->GetLayersPerTick() == S.LayersPerTickBefore && Gpu->GetConfiguredK() == S.KBefore;
		if (bRestored)
		{
			return true;
		}
		if (FPlatformTime::Seconds() > S.RestoreDeadline)
		{
			AddError(FString::Printf(TEXT("GPU schedule not restored after the query (review R2-W3): layers per tick %d (was %d), K %d (was %d)"),
				Gpu->GetLayersPerTick(), S.LayersPerTickBefore, Gpu->GetConfiguredK(), S.KBefore));
			return true;
		}
		return false;
	}));

	// --- (b) reporting, (c) digests against the controller, (e) the context-cap refusal ---
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		const FSuperSLMQueryReadout& C = S.CpuRun.Readout;
		const FSuperSLMQueryReadout& G = S.GpuRun.Readout;
		if (S.CpuRun.bSucceeded)
		{
			TestTrue(TEXT("CPU readout: ran on the CPU"), C.BackendRun == ESuperSLMBackendBP::CPU);
			TestFalse(TEXT("CPU readout: not moved"), C.bMovedBackend);
			TestEqual(TEXT("CPU readout: digest is 64 hex chars"), C.TokenDigestHex.Len(), 64);
		}
		if (S.GpuRun.bSucceeded)
		{
			// The fallback's reporting in the state reachable here: no loss, so no move.
			TestTrue(TEXT("GPU readout: ran on the GPU (no probe-confirmed loss, so no CPU re-issue)"), G.BackendRun == ESuperSLMBackendBP::GPU);
			TestFalse(TEXT("GPU readout: bMovedBackend is false"), G.bMovedBackend);
			TestTrue(TEXT("GPU readout: no move message"), G.MovedBackendMessage.IsEmpty());
		}
		if (S.CpuRun.bSucceeded && S.GpuRun.bSucceeded)
		{
			TestEqual(TEXT("Blueprint query: GPU digest == CPU digest (plan §8)"), G.TokenDigestHex, C.TokenDigestHex);
		}

		USuperSLMSubsystem* Cpu = S.Cpu.Get();
		if (Cpu != nullptr && S.Model.IsValid())
		{
			FSuperSLMQueryWindowController Controller(*Cpu, S.Gpu.Get(), *S.Model, SuperSLML2S3Fixtures::AExSchemaName());
			for (const ESuperSLMBackendBP Backend : { ESuperSLMBackendBP::CPU, ESuperSLMBackendBP::GPU })
			{
				const bool bGpu = Backend == ESuperSLMBackendBP::GPU;
				const FDrive& Mine = bGpu ? S.GpuRun : S.CpuRun;
				FSuperSLMQueryReadout Ref;
				FString Error;
				const bool bRan = Controller.RunQuery(SuperSLML2S3Fixtures::SweepPromptText(), kMaxNewTokens, MakeConfig(Backend), Ref, Error);
				if (TestTrue(FString::Printf(TEXT("controller %s run (%s)"), bGpu ? TEXT("GPU") : TEXT("CPU"), *Error), bRan) && Mine.bSucceeded)
				{
					TestEqual(FString::Printf(TEXT("Blueprint query %s digest == the editor controller's"), bGpu ? TEXT("GPU") : TEXT("CPU")),
						Mine.Readout.TokenDigestHex, Ref.TokenDigestHex);
					TestEqual(FString::Printf(TEXT("Blueprint query %s tokens == the editor controller's"), bGpu ? TEXT("GPU") : TEXT("CPU")),
						Mine.Readout.GeneratedTokens, Ref.GeneratedTokens);
				}
			}
		}

		// (e) The context-cap refusal, on the Blueprint face.
		FSuperSLMModelShapeFacts Shape;
		if (S.Query.IsValid() && S.Model.IsValid() && TestTrue(TEXT("model shape readable"), SuperSLMModelInspector::ReadShape(*S.Model, Shape)))
		{
			FString CapError;
			TestFalse(TEXT("a query past context_cap is refused at BeginQuery"),
				S.Query->BeginQuery(SuperSLML2S3Fixtures::SweepPromptText(), static_cast<int32>(Shape.ContextCap), MakeConfig(ESuperSLMBackendBP::CPU), CapError));
			TestTrue(FString::Printf(TEXT("the refusal names context_cap (%s)"), *CapError), CapError.Contains(TEXT("context_cap")));
			TestFalse(TEXT("nothing is left running after the context_cap refusal"), S.Query->IsQueryRunning());
		}

		if (S.Query.IsValid())
		{
			S.Query->CancelQuery();
		}
		S.Query.Reset();
		S.World.Reset();
		S.Model.Reset();
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
