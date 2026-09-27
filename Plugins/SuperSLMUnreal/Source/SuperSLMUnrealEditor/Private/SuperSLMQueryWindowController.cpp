#include "SuperSLMQueryWindowController.h"

#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"

// L2-S3 (plan §7 item 3, §8, §10.4). Code review S2 moved the query itself -- backend choice, the
// checkbox, Frame Budget, k, concurrency and the D-SLM7382 CPU re-issue -- into the runtime's
// FSuperSLMQueryRunner (SuperSLMQuery.cpp, whose header comment states each rule). What stays
// here is the window's own: the self-check is never run by a query; RunSelfCheck() (headless) or
// BeginSelfCheck()/TickSelfCheck() (the window, one bounded step per frame) runs it, a query and
// the self-check never run together (both drive the same subsystems), and a quarantined verdict
// is never displayed (§7 item 11).
namespace
{
	// Review round 2, R2-W2: the runtime query takes its stop ids from the caller (plan §8: the game
	// supplies them). This window is the demo for Qwen2.5-Instruct artifacts, so a config that names
	// none gets that model's end-of-turn and end-of-text ids, as the window always used.
	FSuperSLMQueryWindowConfig WithWindowStopIds(const FSuperSLMQueryWindowConfig& Config)
	{
		FSuperSLMQueryWindowConfig Out = Config;
		if (Out.StopTokenIds.Num() == 0)
		{
			Out.StopTokenIds = {151645, 151643};
		}
		return Out;
	}

	ESuperSLMSelfCheckVerdictBP DisplayedVerdict(const FSuperSLMSelfCheckBackendResult& Result)
	{
		return Result.bQuarantined ? ESuperSLMSelfCheckVerdictBP::NotYetRun : static_cast<ESuperSLMSelfCheckVerdictBP>(Result.Verdict);
	}

	FString DisplayedScope(const FSuperSLMSelfCheckBackendResult& Result)
	{
		return Result.bQuarantined
			? FString(TEXT("Verdict withheld: recorded in the report, not displayed. In 1.0 the GPU verdict is always withheld."))
			: Result.ScopeText;
	}
}

FSuperSLMQueryWindowController::FSuperSLMQueryWindowController(
	USuperSLMSubsystem& InCpuSubsystem,
	USuperSLMGpuSubsystem* InGpuSubsystem,
	USuperSLMModel& InModel)
	: GpuSubsystem(InGpuSubsystem)
	, Model(InModel)
	, Runner(InCpuSubsystem, InGpuSubsystem, InModel)
{
}

FSuperSLMQueryWindowController::FSuperSLMQueryWindowController(
	USuperSLMSubsystem& InCpuSubsystem,
	USuperSLMGpuSubsystem* InGpuSubsystem,
	USuperSLMModel& InModel,
	const FString& /*InSchemaName: see the header (review N4)*/)
	: FSuperSLMQueryWindowController(InCpuSubsystem, InGpuSubsystem, InModel)
{
}

FSuperSLMQueryWindowController::~FSuperSLMQueryWindowController()
{
	SelfCheckRun.Reset(); // returns a sequence an abandoned self-check still holds
	Runner.CancelQuery();
}

bool FSuperSLMQueryWindowController::RunSelfCheck(FString& OutError)
{
	// Blocking: the self-check drives both subsystems' ticks through its reference workload
	// (32 steps x 2 granularities per backend) on the calling thread. Headless callers only; the
	// window uses BeginSelfCheck()/TickSelfCheck().
	const FSuperSLMSelfCheckReport Report = SuperSLMDeterminismSelfCheck::Run(Model, FString());
	return ApplySelfCheckReport(Report, OutError);
}

bool FSuperSLMQueryWindowController::BeginSelfCheck(FString& OutError)
{
	check(IsInGameThread());
	if (Runner.IsQueryRunning() || SelfCheckRun.IsValid())
	{
		OutError = Runner.IsQueryRunning() ? TEXT("a query is running; the self-check drives the same subsystems") : TEXT("the self-check is already running");
		return false;
	}
	SelfCheckRun = MakeUnique<FSuperSLMSelfCheckRun>(Model, FString());
	return true;
}

bool FSuperSLMQueryWindowController::TickSelfCheck(double StepBudgetSeconds, bool& bOutRan, FString& OutError)
{
	check(IsInGameThread());
	bOutRan = false;
	if (!SelfCheckRun.IsValid())
	{
		OutError = TEXT("no self-check is running");
		return true;
	}
	if (!SelfCheckRun->Step(StepBudgetSeconds))
	{
		return false;
	}
	bOutRan = ApplySelfCheckReport(SelfCheckRun->GetReport(), OutError);
	SelfCheckRun.Reset();
	return true;
}

void FSuperSLMQueryWindowController::CancelSelfCheck()
{
	SelfCheckRun.Reset();
}

bool FSuperSLMQueryWindowController::ApplySelfCheckReport(const FSuperSLMSelfCheckReport& Report, FString& OutError)
{
	CachedCpuVerdict = DisplayedVerdict(Report.Cpu);
	CachedCpuScopeText = DisplayedScope(Report.Cpu);
	CachedGpuVerdict = GpuSubsystem != nullptr ? DisplayedVerdict(Report.Gpu) : ESuperSLMSelfCheckVerdictBP::NotYetRun;
	CachedGpuScopeText = GpuSubsystem != nullptr ? DisplayedScope(Report.Gpu) : FString(TEXT("Not run: no GPU subsystem"));
	if (Report.Cpu.Verdict == ESuperSLMSelfCheckVerdict::NotYetRun)
	{
		OutError = Report.Cpu.ScopeText;
		return false;
	}
	return true;
}

void FSuperSLMQueryWindowController::FillSelfCheck(FSuperSLMQueryWindowReadout& Out) const
{
	Out.SelfCheckVerdict = Out.BackendRun == ESuperSLMBackendBP::GPU ? CachedGpuVerdict : CachedCpuVerdict;
	Out.SelfCheckScopeText = Out.BackendRun == ESuperSLMBackendBP::GPU ? CachedGpuScopeText : CachedCpuScopeText;
}

bool FSuperSLMQueryWindowController::RunQuery(
	const FString& PromptText,
	int32 MaxNewTokens,
	const FSuperSLMQueryWindowConfig& Config,
	FSuperSLMQueryWindowReadout& OutReadout,
	FString& OutError)
{
	OutReadout = FSuperSLMQueryWindowReadout();
	if (SelfCheckRun.IsValid())
	{
		OutError = TEXT("the self-check is running; it drives the same subsystems");
		return false;
	}
	const bool bSucceeded = Runner.RunQuery(PromptText, MaxNewTokens, WithWindowStopIds(Config), OutReadout, OutError);
	if (bSucceeded)
	{
		FillSelfCheck(OutReadout);
	}
	return bSucceeded;
}

bool FSuperSLMQueryWindowController::BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryWindowConfig& Config, FString& OutError)
{
	check(IsInGameThread());
	if (SelfCheckRun.IsValid())
	{
		OutError = TEXT("the self-check is running; it drives the same subsystems");
		return false;
	}
	return Runner.BeginQuery(PromptText, MaxNewTokens, WithWindowStopIds(Config), OutError);
}

bool FSuperSLMQueryWindowController::TickQuery(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMQueryWindowReadout& OutReadout, FString& OutError)
{
	const bool bStopped = Runner.TickQuery(DeltaSeconds, bOutSucceeded, OutReadout, OutError);
	if (bStopped && bOutSucceeded)
	{
		FillSelfCheck(OutReadout);
	}
	return bStopped;
}
