#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate. No type
// here is reflected any longer (review S2 moved them to SuperSLMQuery.h), so no generated header.

#include "SuperSLMQuery.h"

class FSuperSLMSelfCheckRun;
class USuperSLMModel;
class USuperSLMSubsystem;
class USuperSLMGpuSubsystem;
struct FSuperSLMSelfCheckReport;

// L2-S3 (the plan §7 item 3, §8; D-SLM7244; Coverage Model §9, cell
// R-S3e; the red-suite record). The demonstrator's TESTABLE core --
// the plain-C++ controller `SSuperSLMQueryWindow` (SSuperSLMQueryWindow.h, the Slate widget)
// wraps. Separated so R-S3e's sweep runs headless in an automation test, matching this
// project's own "the editor toolchain works, and headless automation is how the plugin's UI
// logic is proven" posture.
//
// Code review S2: the "two backends behind one Blueprint-facing interface" (D-SLM3534) seam --
// backend choice and the D-SLM7382 CPU re-issue -- was this controller's body, in the editor
// module, so a packaged game had neither. It now lives in the runtime module as
// FSuperSLMQueryRunner, with USuperSLMQuery as its Blueprint face (SuperSLMQuery.h), and this
// controller is a client of it: every query call forwards to its runner. What the controller
// adds is the window's own: the determinism self-check, run on demand, whose cached verdict and
// scope it writes into each readout (§8: "The example runs the self-check on load and shows its
// verdict and scope beside the digest").
//
// The window's config, readout and live view are the runtime's types, under the names this
// editor surface and its tests have always used.
using FSuperSLMQueryWindowConfig = FSuperSLMQueryConfig;
using FSuperSLMQueryWindowReadout = FSuperSLMQueryReadout;

// The query window's controller: one Model, one prompt; RunQuery() drives the full generation to
// completion under Config and fills OutReadout. Owns no persistent UE object state beyond what the
// two subsystems already track -- a fresh FSuperSLMQueryWindowController is cheap to construct per
// query, matching the Slate widget's own per-click-driven usage.
//
// GpuSubsystem is nullable: a caller (or R-S3e's own test) on a box where the GPU backend has
// not been Configure()'d successfully (FSuperSLMGpuConfigureReport::Result != Success --
// DeviceUnavailable or ShaderStagingIncomplete, SuperSLMGpuRuntimeConfig.h) passes nullptr, and
// RunQuery() with Config.Backend == GPU then fails with a named OutError rather than crashing --
// "Nothing is refused" (§8) describes a DIVERGING device the self-check catches, never an
// ABSENT backend.
class SUPERSLMUNREALEDITOR_API FSuperSLMQueryWindowController
{
public:
	FSuperSLMQueryWindowController(
		USuperSLMSubsystem& InCpuSubsystem,
		USuperSLMGpuSubsystem* InGpuSubsystem,
		USuperSLMModel& InModel);

	// Review N4: the schema a query binds is decided by its checkbox alone -- on, `prompt_result`;
	// off, none (FSuperSLMQueryConfig::bSchemaConstrainedDecoding). A constructor-time schema name
	// has no part in that rule, so this overload ignores InSchemaName. It stays only so existing
	// callers (the L2-S3 test cells) compile; new code uses the three-argument form.
	FSuperSLMQueryWindowController(
		USuperSLMSubsystem& InCpuSubsystem,
		USuperSLMGpuSubsystem* InGpuSubsystem,
		USuperSLMModel& InModel,
		const FString& InSchemaName);

	// Runs SuperSLMDeterminismSelfCheck::Run() (SuperSLMDeterminismSelfCheck.h, T-2816) once and
	// caches its per-backend verdict/scope for every readout this controller instance fills
	// ("The example runs the self-check on load and shows its verdict and scope beside the
	// digest", §8). Idempotent; a second call re-runs and overwrites the cached verdicts.
	// Requires Model to already be Configure()'d against BOTH subsystems (that function's own
	// documented precondition) -- when GpuSubsystem is nullptr, only the CPU verdict is
	// populated and the GPU verdict stays NotYetRun. Blocking: headless callers only.
	bool RunSelfCheck(FString& OutError);

	// The same self-check without blocking (fold-round ruling 1): BeginSelfCheck() starts an
	// FSuperSLMSelfCheckRun (SuperSLMDeterminismSelfCheck.h) and TickSelfCheck(), called once per
	// editor frame, advances it by at most StepBudgetSeconds of game-thread work. TickSelfCheck()
	// returns true once it has stopped: bOutRan and OutError are then RunSelfCheck()'s return
	// value and error, and the cached verdicts are updated exactly as RunSelfCheck() updates them.
	// Refused while a query runs (both drive the same subsystems' Tick()). Game thread.
	bool BeginSelfCheck(FString& OutError);
	bool TickSelfCheck(double StepBudgetSeconds, bool& bOutRan, FString& OutError);
	bool IsSelfCheckRunning() const { return SelfCheckRun.IsValid(); }
	void CancelSelfCheck();

	// FSuperSLMQueryRunner::RunQuery() (SuperSLMQuery.h), plus the cached self-check verdict for
	// the backend the answer came from. Headless: it ticks the backend directly in a loop. Here and in
	// BeginQuery(), a Config with no StopTokenIds gets Qwen2.5-Instruct's {151645, 151643} (R2-W2).
	bool RunQuery(
		const FString& PromptText,
		int32 MaxNewTokens,
		const FSuperSLMQueryWindowConfig& Config,
		FSuperSLMQueryWindowReadout& OutReadout,
		FString& OutError);

	// --- The same query, one frame at a time ---
	//
	// FSuperSLMQueryRunner's per-frame form: the Slate window calls BeginQuery() once and
	// TickQuery() once per editor frame, which shares one subsystem Tick() per frame with every
	// other per-frame client (review W6). Refused while the self-check runs. Game thread.
	bool BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryWindowConfig& Config, FString& OutError);
	bool TickQuery(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMQueryWindowReadout& OutReadout, FString& OutError);
	bool IsQueryRunning() const { return Runner.IsQueryRunning(); }
	bool GetLiveView(FSuperSLMQueryLiveView& OutView) const { return Runner.GetLiveView(OutView); }
	void CancelQuery() { Runner.CancelQuery(); }

	~FSuperSLMQueryWindowController();

private:
	TUniquePtr<FSuperSLMSelfCheckRun> SelfCheckRun;

	bool ApplySelfCheckReport(const FSuperSLMSelfCheckReport& Report, FString& OutError);
	void FillSelfCheck(FSuperSLMQueryWindowReadout& OutReadout) const;

	USuperSLMGpuSubsystem* GpuSubsystem = nullptr;
	USuperSLMModel& Model;
	FSuperSLMQueryRunner Runner;
	ESuperSLMSelfCheckVerdictBP CachedCpuVerdict = ESuperSLMSelfCheckVerdictBP::NotYetRun;
	ESuperSLMSelfCheckVerdictBP CachedGpuVerdict = ESuperSLMSelfCheckVerdictBP::NotYetRun;
	FString CachedCpuScopeText;
	FString CachedGpuScopeText;
};
