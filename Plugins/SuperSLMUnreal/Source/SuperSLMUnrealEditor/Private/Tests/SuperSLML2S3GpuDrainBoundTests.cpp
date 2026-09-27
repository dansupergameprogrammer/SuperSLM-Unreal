// T-2818 (L2-S3). The GPU drain bound (plan code review W4; dev-box interactive check 13,
// which the query window cannot set up: the window holds one query at a time).
//
// A GPU query whose k or Frame Budget differs from the GPU subsystem's current schedule waits for
// the pool to drain before applying it. While another client holds a GPU sequence vended on the
// same subsystem, the pool never drains, so past the runner's bound (SetGpuDrainWaitSeconds,
// default 5 s) the query must run at the current schedule, and its readout must say the schedule
// was not applied and name the K it ran at.
//
// No forcing construction: the other client is a real sequence vended from the same
// USuperSLMGpuSubsystem with VendSequence() and held for the whole query, and the query runs the
// way the query window runs it -- BeginQuery() once, then TickQuery() once per editor frame from a
// latent command, so the wait is the product's own per-frame wait at its default bound.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Misc/App.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMQueryWindowController.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h" // FTestWorldWrapper, FFunctionLatentCommand
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMDrainBound
{
	// The runner's default bound (FSuperSLMQueryRunner::GpuDrainWaitSeconds). This cell does not
	// set it: the default is what the query window ships with.
	constexpr double kDrainBoundSeconds = 5.0;
	// "About 5 s": the wait is checked once per editor frame, so the query may vend up to a frame
	// or two past the bound; a slow editor frame under automation can run long.
	constexpr double kDrainSlackSeconds = 2.0;
	// Review round 5, R5-N1: the wait is bounded from below too, so a bound that fell to ~0 s fails.
	// The runner starts its clock inside BeginQuery(), after this cell's own start, and stops
	// waiting on the first TickQuery() at or past the bound; one frame (100 ms at a slow 10 fps)
	// covers the gap between the two clocks.
	constexpr double kOneFrameSeconds = 0.1;
	// Whole-cell cap, so a query that never stops fails the cell instead of hanging the editor.
	constexpr double kWallCapSeconds = 180.0;
	constexpr int32 kMaxNewTokens = 8;

	struct FState
	{
		TSharedPtr<FTestWorldWrapper> World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
		TUniquePtr<FSuperSLMQueryWindowController> Controller;
		FSuperSLMGpuSequence Held;
		bool bHolding = false;
		int32 PriorK = 0;
		int32 PriorLayersPerTick = 0;
		double BeginSeconds = 0.0;
		double DrainEndSeconds = -1.0; // first frame the live view no longer reads bAwaitingGpuDrain
		bool bSawAwaitingDrain = false;
	};

	// True when Text names "K <Value>" with a capital K and no further digit: the runner's message
	// also names the requested "k <n>" in lower case, which is not the K the query ran at.
	bool NamesK(const FString& Text, int32 Value)
	{
		const FString Needle = FString::Printf(TEXT("K %d"), Value);
		int32 From = 0;
		while (true)
		{
			const int32 At = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (At == INDEX_NONE)
			{
				return false;
			}
			const int32 After = At + Needle.Len();
			if (After >= Text.Len() || !FChar::IsDigit(Text[After]))
			{
				return true;
			}
			From = At + 1;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3GpuDrainBoundTest,
	"SuperSLM.L2S3.QueryWindow.GpuQueryRunsWithinDrainBoundWhileAnotherClientHoldsASequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3GpuDrainBoundTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMDrainBound;
	const TCHAR* CellName = TEXT("GPU drain bound");

	TSharedRef<FState> State = MakeShared<FState>();
	State->World = MakeShared<FTestWorldWrapper>();
	if (!State->World->CreateTestWorld(EWorldType::Game))
	{
		AddError(FString::Printf(TEXT("%s: could not create a game world"), CellName));
		return false;
	}
	// Two blocks: one for the other client's held sequence, one for the query.
	SuperSLML2S3Fixtures::FQueryWindowBackends Backends;
	if (!SuperSLML2S3Fixtures::SetUpQueryWindowBackends(
			*this, State->World->GetTestWorld(), CellName, /*BlockCount*/ 2, /*bRequirePromptResultSchema*/ false, Backends))
	{
		return false;
	}
	State->Model.Reset(Backends.Model); // held across frames; the editor collects garbage between them
	State->Gpu = Backends.Gpu;
	State->PriorK = Backends.Gpu->GetConfiguredK();
	State->PriorLayersPerTick = Backends.Gpu->GetLayersPerTick();

	// The query's schedule differs from the current one in both controls: k one above the current K
	// (above every floor, since the current K is Configure()'d at or above them), and Frame Budget
	// half the model where the tick covers all of it.
	FSuperSLMQueryWindowConfig Config;
	Config.Backend = ESuperSLMBackendBP::GPU;
	Config.ConcurrentQueries = 1;
	Config.RequestedK = State->PriorK + 1;
	Config.FrameBudgetLayers = SuperSLML2S3Fixtures::AExNumHiddenLayers / 2;
	if (!TestTrue(TEXT("construction: Frame Budget differs from the tick's current layer budget"),
			Config.FrameBudgetLayers != State->PriorLayersPerTick))
	{
		return false;
	}

	// The other client: a real sequence vended on the same GPU subsystem, held until the query ends.
	const ESuperSLMGpuVendResult Vend = Backends.Gpu->VendSequence(State->Held, ESuperSLMGpuDecodePath::Composed);
	if (!TestEqual(TEXT("construction: the other client's GPU sequence vends"), (int32)Vend, (int32)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	State->bHolding = true;

	State->Controller = MakeUnique<FSuperSLMQueryWindowController>(
		*Backends.Cpu, Backends.Gpu, *Backends.Model, SuperSLML2S3Fixtures::AExSchemaName());
	FString BeginError;
	State->BeginSeconds = FPlatformTime::Seconds();
	const bool bBegan = State->Controller->BeginQuery(SuperSLML2S3Fixtures::SweepPromptText(), kMaxNewTokens, Config, BeginError);
	if (!TestTrue(FString::Printf(TEXT("BeginQuery on the GPU while another client holds a sequence (%s)"), *BeginError), bBegan))
	{
		Backends.Gpu->ReturnSequence(State->Held);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		auto Finish = [&S]()
		{
			if (S.Controller.IsValid() && S.Controller->IsQueryRunning())
			{
				S.Controller->CancelQuery();
			}
			if (S.bHolding && S.Gpu.IsValid())
			{
				S.Gpu->ReturnSequence(S.Held);
			}
			S.bHolding = false;
			S.Controller.Reset();
			S.World.Reset();
			S.Model.Reset();
			return true;
		};

		if (!S.Gpu.IsValid())
		{
			AddError(TEXT("GPU drain bound: the GPU subsystem was destroyed mid-query."));
			return Finish();
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - S.BeginSeconds > kWallCapSeconds)
		{
			AddError(FString::Printf(TEXT("GPU drain bound: the query did not stop within %.0f s (awaiting drain: %s)."),
				kWallCapSeconds, S.DrainEndSeconds < 0.0 ? TEXT("still") : TEXT("no")));
			return Finish();
		}

		// When the query stopped waiting: the first frame its live view no longer awaits the drain.
		FSuperSLMQueryLiveView View;
		if (S.DrainEndSeconds < 0.0 && S.Controller->GetLiveView(View))
		{
			if (View.bAwaitingGpuDrain)
			{
				S.bSawAwaitingDrain = true;
			}
			else
			{
				S.DrainEndSeconds = Now;
			}
		}

		bool bSucceeded = false;
		FSuperSLMQueryWindowReadout Readout;
		FString Error;
		if (!S.Controller->TickQuery(FApp::GetDeltaTime(), bSucceeded, Readout, Error))
		{
			return false; // still running; next frame
		}
		if (S.DrainEndSeconds < 0.0)
		{
			S.DrainEndSeconds = FPlatformTime::Seconds();
		}

		if (!TestTrue(FString::Printf(TEXT("the query completes (%s)"), *Error), bSucceeded))
		{
			return Finish();
		}
		const double DrainWaitSeconds = S.DrainEndSeconds - S.BeginSeconds;
		AddInfo(FString::Printf(TEXT("GPU drain bound: waited %.2f s for the pool to drain (bound %.1f s); readout: %s"),
			DrainWaitSeconds, kDrainBoundSeconds, *Readout.ScheduleMessage));

		// Construction: the held sequence did keep the pool from draining, so the query waited.
		TestTrue(TEXT("construction: the query waited for the pool to drain (live view read bAwaitingGpuDrain)"), S.bSawAwaitingDrain);

		// The bound: the query ran within about 5 s of starting to wait, not after the holder let go.
		TestTrue(FString::Printf(TEXT("the query stops waiting within about %.0f s (waited %.2f s, allowed %.1f s)"),
			kDrainBoundSeconds, DrainWaitSeconds, kDrainBoundSeconds + kDrainSlackSeconds),
			DrainWaitSeconds <= kDrainBoundSeconds + kDrainSlackSeconds);
		TestTrue(FString::Printf(TEXT("the query waits the bound out, at least %.0f s less one frame (waited %.2f s)"),
			kDrainBoundSeconds, DrainWaitSeconds),
			DrainWaitSeconds >= kDrainBoundSeconds - kOneFrameSeconds);
		TestTrue(TEXT("the other client still holds its sequence when the query ends (it never drained)"), S.bHolding);

		// The readout: not applied, and the K it ran at named.
		TestEqual(TEXT("the query ran on the GPU"), (int32)Readout.BackendRun, (int32)ESuperSLMBackendBP::GPU);
		TestFalse(TEXT("the query was not moved to the CPU"), Readout.bMovedBackend);
		TestTrue(TEXT("the query generated tokens"), Readout.GeneratedTokens.Num() > 0);
		TestTrue(TEXT("readout: bScheduleNotApplied"), Readout.bScheduleNotApplied);
		TestTrue(FString::Printf(TEXT("readout: the message says \"not applied\" (\"%s\")"), *Readout.ScheduleMessage),
			Readout.ScheduleMessage.Contains(TEXT("not applied")));
		TestTrue(FString::Printf(TEXT("readout: the message names the K used, K %d (\"%s\")"), S.PriorK, *Readout.ScheduleMessage),
			NamesK(Readout.ScheduleMessage, S.PriorK));
		TestEqual(TEXT("readout: AppliedK is the current K, not the requested k"), Readout.AppliedK, S.PriorK);
		TestEqual(TEXT("readout: AppliedLayersPerTick is the tick's current layer budget, not Frame Budget"),
			Readout.AppliedLayersPerTick, S.PriorLayersPerTick);
		return Finish();
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
