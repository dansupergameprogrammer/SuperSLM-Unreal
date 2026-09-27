#pragma once

#include "CoreMinimal.h"
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuTypes.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSequenceTypes.h"

class USuperSLMModel;
class USuperSLMSubsystem;
class USuperSLMGpuSubsystem;

// The example scene's demonstrator: one query, run on the backend and settings the scene's
// controls select, as a schema-constrained potion-shop extraction followed by a free-decode
// voiced reply, across frames. Plain C++ over the plugin's public runtime API; the scene actor
// (SuperSLMExampleScene.h) owns the objects it points at and calls TickFrame() once per frame.
//
// Threads. Every call this class makes is on the game thread, which is where the plugin's API
// lives; nothing heavy runs there, because the plugin moves the heavy part itself:
//  - Configuring a backend uses BeginConfigure(): the game thread captures the model, the mapping,
//    hashing, measurement and allocation run on the plugin's pool threads, and the result comes
//    back as a callback on the game thread a later frame.
//  - The determinism self-check is the plugin's stepped run (FSuperSLMSelfCheckRun): one Step() per
//    frame, bounded by SuperSLMExample::SelfCheckStepBudgetSeconds of game-thread bookkeeping, with
//    the inference on the plugin's worker and submission threads. While it runs it ticks both
//    subsystems itself, so TickFrame() does not.
//  - A query's inference runs on the CPU backend's worker threads or the GPU backend's submission
//    thread; the game thread ticks the subsystems (plan and apply) and reads delivered results.
//  - Detokenizing a finished answer is a table lookup on the model mapping the configured CPU
//    backend already holds.
namespace SuperSLMExample
{
	// Facts of the example artifact, Qwen2.5-0.5B-Instruct converted at context_cap 4096
	// (the release asset). The plugin's public API exposes neither the layer count nor the GPU's
	// dispatches per layer, so the example states them for the one artifact it is built for and
	// checks them: BeginInitialize() refuses a model whose shape (SuperSLMModelInspector::ReadShape)
	// has another layer count or context cap, and a GPU configuration whose layers per tick
	// (USuperSLMGpuSubsystem::GetLayersPerTick) differ from the Frame Budget it asked for is
	// refused, which is where a wrong dispatches-per-layer figure would show.
	constexpr int32 NumHiddenLayers = 24;       // the checkpoint's config.json
	constexpr uint32 GpuDispatchesPerLayer = 24; // Qwen2.5 has no QK-norm: a legacy model, 24 per layer
	constexpr int64 ContextCap = 4096;

	// Qwen2.5-Instruct's end-of-turn (<|im_end|>) and end-of-text (<|endoftext|>) ids. The
	// artifact records no end-of-text id, so the game supplies the stop set.
	inline const TArray<int32>& StopTokenIds()
	{
		static const TArray<int32> Ids = { 151645, 151643 };
		return Ids;
	}

	// The demo schema compiled into the example artifact's schema section.
	inline const TCHAR* SchemaName() { return TEXT("potion_shop_order"); }

	constexpr int32 MaxConcurrentQueries = 4;
	constexpr int32 ExtractionMaxNewTokens = 64;

	// The voiced reply's decode budget. Fixed: no control moves the answer or its digest.
	constexpr int32 ReplyMaxNewTokens = 32;

	// The GPU default slice: 4 layers. At 4, every decode- and prompt-slice median gpu_busy_ms on
	// record, headless and in the editor with its viewport rendering, was at most 1.58 ms on one
	// RTX 2080 SUPER (context up to about 400 tokens); at 5 one prompt-slice median was 2.20 ms.
	// It moves only if a headless and a rendering measurement both select another setting.
	constexpr int32 DefaultFrameBudgetLayers = 4;

	// The GPU slice budget a slice's gpu_busy_ms is compared with for the hitch count
	// (the 2 ms slice target). A comparison, never a refusal.
	constexpr double GpuSliceBudgetMs = 2.0;

	// The plugin's bound on the CPU backend's own game-thread Tick() (plan and apply bookkeeping,
	// no inference): under 1 ms per tick, set about three orders of magnitude above the
	// microsecond-scale overhead measured for that work. The example's test asserts it on every
	// tick of a query, from the plugin's tick history.
	constexpr double CpuGameThreadTickBoundMs = 1.0;

	// The CPU worker's per-job budget: one 60 Hz frame. It sizes each job; it is not the
	// game thread's cost.
	constexpr double CpuTickBudgetMs = 16.6;

	inline const TCHAR* DefaultUtterance() { return TEXT("Hello! Could I get two health potions, please?"); }

	// The self-check's game-thread work per frame. A chosen setting, not a measurement: the
	// inference itself runs on the plugin's threads; this bounds the ticking and bookkeeping.
	constexpr double SelfCheckStepBudgetSeconds = 0.002;

	// The GPU backend's compute shaders at this plugin version, staged loose under the plugin's
	// Binaries/Win64/shaders; logits_site is the one the logits head loads at map time.
	constexpr int32 ExpectedGpuShaderCount = 35;
	inline const TCHAR* LogitsSiteShaderFile() { return TEXT("logits_site.cso"); }
}

enum class ESuperSLMExampleBackend : uint8
{
	CPU,
	GPU,
};

// The scene's controls: Thinking/Frame Budget, Backend, concurrent queries and k. There is no
// schema checkbox: the example scene always runs the schema-constrained extraction, then the
// free-decode reply. No control moves the structured result or its token digest; they move only
// frames-to-answer and the cost figures.
struct FSuperSLMExampleSettings
{
	ESuperSLMExampleBackend Backend = ESuperSLMExampleBackend::CPU;

	// Thinking/Frame Budget, one control: layers per slice, 1 to NumHiddenLayers. On the CPU it is
	// the sequence's layer_budget. On the GPU it sets the composed path's dispatch_budget in whole
	// layers per slice, and the top position (NumHiddenLayers) selects the one-call path, labelled
	// "whole token".
	int32 FrameBudgetLayers = SuperSLMExample::DefaultFrameBudgetLayers;

	int32 ConcurrentQueries = 1;

	// The GPU's fixed-tick latency. Clamped to at least the minimum the GPU backend computes for
	// the current Frame Budget and concurrency. The CPU backend has no fixed K: its K is per job,
	// and the readout shows it.
	int32 RequestedK = 1;

	FString Utterance = SuperSLMExample::DefaultUtterance();

	bool IsWholeToken() const { return FrameBudgetLayers >= SuperSLMExample::NumHiddenLayers; }
};

enum class ESuperSLMExampleRunState : uint8
{
	Idle,
	ConfiguringGpu, // a control changed the GPU backend's configuration; its BeginConfigure() is pending
	Running,
	Done,
	Failed,
};

// One labelled figure of the readout: every figure names the call it came from.
struct FSuperSLMExampleFigure
{
	FString Label;
	FString Value;
	FString Source;
};

struct FSuperSLMExampleReadout
{
	ESuperSLMExampleRunState State = ESuperSLMExampleRunState::Idle;
	FString Error;

	FSuperSLMExampleSettings Settings; // the settings the run used

	// The structured result, as the model wrote it and as validated against the demo schema.
	FString ExtractionText;
	bool bExtractionSchemaValid = false;
	FString ExtractionValidation; // the validator's finding, or the parsed fields
	FString ReplyText;

	// SuperSLMGpuDigest::ComputeTokenDigest over the extraction tokens followed by the reply
	// tokens, of the first query. The other concurrent queries are compared with it.
	FString TokenDigestHex;
	int32 QueriesMatchingDigest = 0;
	int32 QueryCount = 0;

	TArray<int32> ExtractionTokens;
	TArray<int32> ReplyTokens;

	int32 FramesToAnswer = 0;
	double WallMsToAnswer = 0.0;

	// Backend facts the packaged check reads directly.
	bool bGpuDeviceHeadActive = false;
	FString GpuDeviceHeadStatus;
	FString GpuShaderDirectory;
	int64 GpuDispatches = 0;
	TArray<double> GpuBusyMsSamples;
	double HostFinishMs = 0.0;
	int32 EffectiveK = 0;
	int32 MinimumK = 0;

	// The frame-cost readings, both the plugin's own. GPU: the layers one tick issues
	// (USuperSLMGpuSubsystem::GetLayersPerTick()), which the Frame Budget sets. CPU: the longest
	// game-thread Tick() of this run and how many ticks it covers (GetTickHistory() DurationMs).
	int32 GpuLayersPerTick = 0;
	double CpuMaxTickMs = -1.0;
	int32 CpuTicks = 0;

	TArray<FSuperSLMExampleFigure> Figures;
};

class FSuperSLMExampleDemo
{
public:
	FSuperSLMExampleDemo(USuperSLMModel& InModel, USuperSLMSubsystem& InCpu, USuperSLMGpuSubsystem* InGpu);
	~FSuperSLMExampleDemo();

	// Starts configuring both backends through BeginConfigure() (the CPU backend, the demo schema,
	// and the GPU backend at the default settings when one exists). The callbacks complete it on
	// later frames. A GPU backend that cannot configure is reported by GetGpuUnavailableReason()
	// and does not fail initialization.
	void BeginInitialize();
	bool IsInitialized() const { return bInitialized; }
	bool HasInitializationFailed() const { return bInitFailed; }
	const FString& GetInitializationError() const { return InitError; }

	// Once per frame on the game thread: steps a running self-check, or ticks the configured
	// subsystems, then advances any query.
	void TickFrame(float DeltaSeconds);

	bool IsGpuAvailable() const { return bGpuAvailable; }
	const FString& GetGpuUnavailableReason() const { return GpuUnavailableReason; }

	// The minimum K the GPU backend computed at its most recent successful configuration, and the
	// estimate for a given setting before it is configured.
	int32 GetGpuMinimumK() const { return GpuConfiguredMinimumK; }
	static int32 EstimateGpuMinimumK(const FSuperSLMExampleSettings& Settings);

	// Starts the determinism self-check against the model as configured on both backends: the
	// plugin's stepped run, advanced one bounded Step() per frame by TickFrame(). Refused while a
	// query or a configuration is in progress.
	bool BeginSelfCheck();
	bool IsSelfCheckRunning() const { return SelfCheckRun.IsValid(); }
	bool HasSelfCheckReport() const { return bHasSelfCheck; }
	int32 GetSelfCheckRunCount() const { return SelfCheckRunCount; }
	int32 GetLastSelfCheckFrames() const { return LastSelfCheckFrames; }

	// The report. A backend's verdict is read only when the plugin has not quarantined it
	// (FSuperSLMSelfCheckBackendResult::bQuarantined): a quarantined verdict is recorded and never
	// shown or used. Callers go through IsCpuVerdictReadable() / IsGpuVerdictReadable().
	const FSuperSLMSelfCheckReport& GetSelfCheckReport() const { return SelfCheck; }
	bool IsCpuVerdictReadable() const { return bHasSelfCheck && !SelfCheck.Cpu.bQuarantined; }
	bool IsGpuVerdictReadable() const { return bHasSelfCheck && !SelfCheck.Gpu.bQuarantined; }

	// Starts a query. Refused while one or any backend work is running. On the GPU backend, a
	// change of Frame Budget or concurrency reconfigures the backend first (its dispatch budget and
	// pool size are configuration) through BeginConfigure(), and the query starts in its callback;
	// a change of K alone is SetFixedTickLatency(), which is immediate.
	bool Start(const FSuperSLMExampleSettings& Settings, FString& OutError);

	// A query, a configuration or the self-check is in progress.
	bool IsBusy() const;
	const FSuperSLMExampleReadout& GetReadout() const { return Readout; }

	// The digest the first run at this utterance produced on a backend whose result the invariant
	// may rest on: the CPU, or a GPU whose released verdict is Verified. Empty until such a run
	// exists.
	bool GetInvariantBaseline(const FSuperSLMExampleSettings& Settings, FString& OutDigest, FString& OutDescription) const;

	// Whether a GPU run may stand for the invariant: only when the plugin has not quarantined the
	// GPU verdict and it reads Verified. While it is quarantined this is false and the verdict is
	// not read.
	bool IsGpuVerified() const;

private:
	enum class EStage : uint8
	{
		WaitingForExtractionSequence,
		Extracting,
		WaitingForReplySequence,
		Replying,
		Finished,
		Failed,
	};

	struct FQuery
	{
		EStage Stage = EStage::WaitingForExtractionSequence;
		FSuperSLMSequence CpuSequence;
		FSuperSLMGpuSequence GpuSequence;
		FSuperSLMLifecycleOpHandle GpuBeginHandle;
		TArray<int32> ExtractionTokens;
		TArray<int32> ReplyTokens;
		FString Error;
		double ExtractionTimeToFirstTokenMs = -1.0;
		double ReplyTimeToFirstTokenMs = -1.0;
		bool bExtractionSchemaAccepting = false;
	};

	// A GPU configuration as the example derives it from the controls.
	struct FGpuConfigRequest
	{
		int32 BlockCount = 1;
		int32 FrameBudgetLayers = SuperSLMExample::DefaultFrameBudgetLayers;
		int32 RequestedK = 1;
	};

	USuperSLMModel& Model;
	USuperSLMSubsystem& Cpu;
	USuperSLMGpuSubsystem* Gpu = nullptr;

	FSuperSLMSchemaHandle CpuSchema;
	FSuperSLMGpuSchemaHandle GpuSchema;

	bool bGpuAvailable = false;
	FString GpuUnavailableReason;
	bool bGpuConfigured = false;
	int32 GpuConfiguredBlockCount = 0;
	uint32 GpuConfiguredDispatchBudget = 0;
	int32 GpuConfiguredK = 0;
	int32 GpuConfiguredMinimumK = 0;

	bool bInitialized = false;
	bool bInitFailed = false;
	FString InitError;
	bool bCpuConfigured = false;
	int32 PendingConfigures = 0; // BeginConfigure() calls whose callback has not come yet

	// Callbacks from BeginConfigure() capture a weak reference to this, so a callback that arrives
	// after the demo is gone does nothing.
	TSharedRef<bool> AliveToken = MakeShared<bool>(true);

	FSuperSLMExampleSettings PendingSettings; // the query that waits on a GPU reconfiguration
	TUniquePtr<FSuperSLMSelfCheckRun> SelfCheckRun;
	int32 SelfCheckFrames = 0;
	int32 LastSelfCheckFrames = 0;

	bool bHasSelfCheck = false;
	int32 SelfCheckRunCount = 0;
	FSuperSLMSelfCheckReport SelfCheck;

	FSuperSLMExampleReadout Readout;
	TArray<FQuery> Queries;
	TArray<int32> ExtractionPromptTokens;
	TArray<int32> ReplyPromptHead;
	TArray<int32> ReplyPromptTail;

	int32 FramesElapsed = 0;
	double StartSeconds = 0.0;
	// The next CPU job-ledger and tick-history rows this run has not read. The plugin keeps both
	// in fixed rings that overwrite their oldest rows, so the run reads them every frame
	// (ReadCpuHistory()) rather than once at the end.
	int64 CpuLedgerCursor = 0;
	int64 CpuTickHistoryCursor = 0;
	TArray<double> CpuWorkerMs;
	TArray<double> CpuKs;
	int32 CpuHitchesAtStart = 0;
	int32 GpuHitchesAtStart = 0;
	int64 GpuDispatchesAtStart = 0;
	int64 GpuDispatchesSeen = 0;
	double CpuTokensPerSecondLast = 0.0;
	TArray<double> GpuBusySamples;
	double LastHostFinishMs = 0.0;

	// Baselines for the invariant: key = utterance.
	struct FBaseline
	{
		FString Digest;
		FString Description;
	};
	TMap<FString, FBaseline> Baselines;

	static FSuperSLMRuntimeConfig CpuConfig();
	static FSuperSLMGpuRuntimeConfig GpuConfigFor(const FGpuConfigRequest& Request);
	static int32 LayersFor(int32 FrameBudgetLayers);

	void BeginGpuConfigure(const FSuperSLMGpuRuntimeConfig& Config, bool bForQuery);
	void OnCpuConfigured(const FSuperSLMConfigureReport& Report);
	void OnGpuConfigured(const FSuperSLMGpuRuntimeConfig& Config, const FSuperSLMGpuConfigureReport& Report, bool bForQuery);
	void FinishConfigureStep();
	void StepSelfCheck();
	bool NeedsGpuReconfigure(const FGpuConfigRequest& Request) const;
	bool BeginRun(const FSuperSLMExampleSettings& Settings, FString& OutError);
	bool BuildPrompts(const FString& Utterance, FString& OutError);
	void Tick();

	bool TryStartExtraction(FQuery& Query);
	bool TryStartReply(FQuery& Query);
	void PollQuery(FQuery& Query);
	bool IsSequenceTerminal(const FQuery& Query, bool& bOutSchemaComplete, FString& OutWhy) const;
	const TArray<int32>& GeneratedTokens(const FQuery& Query) const;
	void ReturnSequence(FQuery& Query);

	void SampleCostFigures();
	void ReadCpuHistory(bool bFinal);
	void FinishRun();
	void Fail(const FString& Why);

	static FString BaselineKey(const FSuperSLMExampleSettings& Settings);
};

namespace SuperSLMExample
{
	// Checks the extraction against the demo schema: one JSON object whose keys are
	// intent, item, quantity and polite, in that order, each value inside its declared set. On
	// success OutSummary lists the fields; on failure it names the first violation.
	bool ValidatePotionShopOrder(const FString& Text, FString& OutSummary);

	FString SettingsDescription(const FSuperSLMExampleSettings& Settings);
	FString VerdictName(ESuperSLMSelfCheckVerdict Verdict);
}
