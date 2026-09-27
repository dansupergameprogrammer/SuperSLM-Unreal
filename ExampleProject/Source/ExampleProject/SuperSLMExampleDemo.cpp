#include "SuperSLMExampleDemo.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "SuperSLMDetokenizer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMExampleDemo, Log, All);

namespace
{
	// Qwen2.5-Instruct's chat format. The tokenizer matches special-token text and emits its id,
	// and adds no markers of its own, so the template is written out here, as SuperSLM's own
	// tools/ask.ps1 does.
	FString ExtractionPromptText(const FString& Utterance)
	{
		return FString::Printf(TEXT("<|im_start|>system\n")
			TEXT("You work the counter of a potion shop. Record the customer's order as JSON.<|im_end|>\n")
			TEXT("<|im_start|>user\n%s<|im_end|>\n")
			TEXT("<|im_start|>assistant\n"), *Utterance);
	}

	// The reply's prompt is this head, then the extraction's own tokens, then the tail: the reply
	// is decoded after, and from, the schema-constrained span.
	const TCHAR* ReplyPromptHeadText()
	{
		return TEXT("<|im_start|>system\n")
			TEXT("You are the keeper of a small potion shop. Answer the customer in one short, friendly sentence, in character. ")
			TEXT("Their order, as recorded: ");
	}

	FString ReplyPromptTailText(const FString& Utterance)
	{
		return FString::Printf(TEXT("<|im_end|>\n")
			TEXT("<|im_start|>user\n%s<|im_end|>\n")
			TEXT("<|im_start|>assistant\n"), *Utterance);
	}

	FString DigestHex(const TArray<int32>& Tokens)
	{
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		return SuperSLMGpuDigest::DigestToHex(Digest);
	}

	double Median(TArray<double> Values)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Mid = Values.Num() / 2;
		return (Values.Num() % 2) ? Values[Mid] : 0.5 * (Values[Mid - 1] + Values[Mid]);
	}

	double MaxOf(const TArray<double>& Values)
	{
		double M = 0.0;
		for (double V : Values)
		{
			M = FMath::Max(M, V);
		}
		return M;
	}

	FString DecodeOutcomeName(ESuperSLMDecodeOutcome Outcome)
	{
		switch (Outcome)
		{
		case ESuperSLMDecodeOutcome::TokenProduced: return TEXT("Token Produced");
		case ESuperSLMDecodeOutcome::Generating: return TEXT("Generating");
		case ESuperSLMDecodeOutcome::SchemaDeadEnd: return TEXT("Schema Dead End");
		case ESuperSLMDecodeOutcome::SequenceNoLongerValid: return TEXT("Sequence No Longer Valid");
		}
		return TEXT("Unknown");
	}

	// Strips a trailing stop token for display. The digest covers the tokens as generated.
	TArray<int32> WithoutTrailingStop(const TArray<int32>& Tokens)
	{
		TArray<int32> Out = Tokens;
		if (Out.Num() > 0 && SuperSLMExample::StopTokenIds().Contains(Out.Last()))
		{
			Out.Pop();
		}
		return Out;
	}
}

namespace SuperSLMExample
{
	bool ValidatePotionShopOrder(const FString& Text, FString& OutSummary)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutSummary = TEXT("not a complete JSON object");
			return false;
		}

		struct FField
		{
			const TCHAR* Key;
			TArray<FString> Allowed; // empty: a boolean
		};
		const FField Fields[] = {
			{ TEXT("intent"), { TEXT("buy"), TEXT("sell"), TEXT("ask_price"), TEXT("haggle"), TEXT("leave") } },
			{ TEXT("item"), { TEXT("health_potion"), TEXT("mana_potion"), TEXT("rope"), TEXT("torch"), TEXT("lantern"), TEXT("none") } },
			{ TEXT("quantity"), { TEXT("one"), TEXT("two"), TEXT("three"), TEXT("several"), TEXT("none") } },
			{ TEXT("polite"), {} },
		};

		constexpr int32 FieldCount = static_cast<int32>(UE_ARRAY_COUNT(Fields));
		if (Object->Values.Num() != FieldCount)
		{
			OutSummary = FString::Printf(TEXT("%d keys, the schema has %d"), Object->Values.Num(), FieldCount);
			return false;
		}

		TArray<FString> Parts;
		int32 PreviousKeyAt = INDEX_NONE;
		for (const FField& Field : Fields)
		{
			const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field.Key);
			if (Value == nullptr || !Value->IsValid())
			{
				OutSummary = FString::Printf(TEXT("missing key \"%s\""), Field.Key);
				return false;
			}
			const int32 KeyAt = Text.Find(FString::Printf(TEXT("\"%s\""), Field.Key), ESearchCase::CaseSensitive);
			if (KeyAt <= PreviousKeyAt)
			{
				OutSummary = FString::Printf(TEXT("key \"%s\" is out of the schema's order"), Field.Key);
				return false;
			}
			PreviousKeyAt = KeyAt;

			if (Field.Allowed.Num() == 0)
			{
				if ((*Value)->Type != EJson::Boolean)
				{
					OutSummary = FString::Printf(TEXT("\"%s\" is not a boolean"), Field.Key);
					return false;
				}
				Parts.Add(FString::Printf(TEXT("%s: %s"), Field.Key, (*Value)->AsBool() ? TEXT("true") : TEXT("false")));
			}
			else
			{
				if ((*Value)->Type != EJson::String || !Field.Allowed.Contains((*Value)->AsString()))
				{
					OutSummary = FString::Printf(TEXT("\"%s\" is outside its enum"), Field.Key);
					return false;
				}
				Parts.Add(FString::Printf(TEXT("%s: %s"), Field.Key, *(*Value)->AsString()));
			}
		}
		OutSummary = FString::Join(Parts, TEXT(", "));
		return true;
	}

	FString SettingsDescription(const FSuperSLMExampleSettings& S)
	{
		const FString Budget = S.Backend == ESuperSLMExampleBackend::CPU
			? FString::Printf(TEXT("layer budget %d"), S.FrameBudgetLayers)
			: (S.IsWholeToken() ? FString(TEXT("whole token (one-call path)")) : FString::Printf(TEXT("%d layers per slice"), S.FrameBudgetLayers));
		return FString::Printf(TEXT("%s, %s, %d concurrent"),
			S.Backend == ESuperSLMExampleBackend::CPU ? TEXT("CPU") : TEXT("GPU"), *Budget, S.ConcurrentQueries);
	}

	FString VerdictName(ESuperSLMSelfCheckVerdict Verdict)
	{
		switch (Verdict)
		{
		case ESuperSLMSelfCheckVerdict::Verified: return TEXT("Verified");
		case ESuperSLMSelfCheckVerdict::Diverged: return TEXT("Diverged");
		case ESuperSLMSelfCheckVerdict::NotYetRun: return TEXT("Not Yet Run");
		}
		return TEXT("Unknown");
	}
}

FSuperSLMExampleDemo::FSuperSLMExampleDemo(USuperSLMModel& InModel, USuperSLMSubsystem& InCpu, USuperSLMGpuSubsystem* InGpu)
	: Model(InModel)
	, Cpu(InCpu)
	, Gpu(InGpu)
{
}

FSuperSLMExampleDemo::~FSuperSLMExampleDemo()
{
	// Nothing waits here. A pending BeginConfigure() callback finds AliveToken gone and does
	// nothing; destroying a stepped self-check returns the sequence it holds.
	SelfCheckRun.Reset();
	for (FQuery& Query : Queries)
	{
		ReturnSequence(Query);
	}
}

FSuperSLMRuntimeConfig FSuperSLMExampleDemo::CpuConfig()
{
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = SuperSLMExample::MaxConcurrentQueries;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = SuperSLMExample::NumHiddenLayers;
	Config.BlockCount = SuperSLMExample::MaxConcurrentQueries;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = SuperSLMExample::CpuTickBudgetMs;
	return Config;
}

int32 FSuperSLMExampleDemo::LayersFor(int32 FrameBudgetLayers)
{
	return FMath::Clamp(FrameBudgetLayers, 1, SuperSLMExample::NumHiddenLayers);
}

FSuperSLMGpuRuntimeConfig FSuperSLMExampleDemo::GpuConfigFor(const FGpuConfigRequest& Request)
{
	FSuperSLMExampleSettings Probe;
	Probe.ConcurrentQueries = Request.BlockCount;
	Probe.FrameBudgetLayers = Request.FrameBudgetLayers;

	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = SuperSLMExample::ContextCap;
	Config.BlockCount = Request.BlockCount;
	Config.DispatchBudget = static_cast<uint32>(LayersFor(Request.FrameBudgetLayers)) * SuperSLMExample::GpuDispatchesPerLayer;
	Config.K = FMath::Max(Request.RequestedK, EstimateGpuMinimumK(Probe));
	Config.TickBudgetMs = SuperSLMExample::GpuSliceBudgetMs;
	return Config;
}

void FSuperSLMExampleDemo::BeginInitialize()
{
	check(IsInGameThread());
	check(!bInitialized && PendingConfigures == 0);

	// The one artifact the example is built for: its shape is read from the model's own bytes
	// (cheap, no mapping) before anything is configured.
	FSuperSLMModelShapeFacts Shape;
	if (!SuperSLMModelInspector::ReadShape(Model, Shape))
	{
		bInitFailed = true;
		InitError = TEXT("the model's configuration section could not be read");
		return;
	}
	if (Shape.NumHiddenLayers != SuperSLMExample::NumHiddenLayers || Shape.ContextCap != SuperSLMExample::ContextCap)
	{
		bInitFailed = true;
		InitError = FString::Printf(TEXT("the example is built for a %d-layer model at context_cap %lld; this model has %d layers at context_cap %lld"),
			SuperSLMExample::NumHiddenLayers, SuperSLMExample::ContextCap, Shape.NumHiddenLayers, Shape.ContextCap);
		return;
	}

	const TWeakPtr<bool> Alive = AliveToken;
	++PendingConfigures;
	Cpu.BeginConfigure(&Model, CpuConfig(), [this, Alive](const FSuperSLMConfigureReport& Report)
	{
		if (Alive.IsValid())
		{
			OnCpuConfigured(Report);
		}
	});

	if (Gpu == nullptr)
	{
		GpuUnavailableReason = TEXT("the GPU backend is not built on this platform (Windows x64 only)");
		return;
	}
	BeginGpuConfigure(GpuConfigFor(FGpuConfigRequest()), /*bForQuery*/ false);
}

void FSuperSLMExampleDemo::OnCpuConfigured(const FSuperSLMConfigureReport& Report)
{
	// Game thread, a later frame.
	if (Report.Result != ESuperSLMConfigureResult::Success)
	{
		bInitFailed = true;
		InitError = FString::Printf(TEXT("the CPU backend refused the example's configuration (result %d): %s"),
			static_cast<int32>(Report.Result), *Report.Message);
	}
	else
	{
		FString Error;
		if (!FSuperSLMSchemaLookup::LookupByName(Model, SuperSLMExample::SchemaName(), CpuSchema, Error))
		{
			bInitFailed = true;
			InitError = FString::Printf(TEXT("the model carries no \"%s\" schema: %s"), SuperSLMExample::SchemaName(), *Error);
		}
		else
		{
			bCpuConfigured = true;
		}
	}
	FinishConfigureStep();
}

void FSuperSLMExampleDemo::BeginGpuConfigure(const FSuperSLMGpuRuntimeConfig& Config, bool bForQuery)
{
	check(Gpu != nullptr);
	bGpuConfigured = false;
	const TWeakPtr<bool> Alive = AliveToken;
	++PendingConfigures;
	Gpu->BeginConfigure(&Model, Config, [this, Alive, Config, bForQuery](const FSuperSLMGpuConfigureReport& Report)
	{
		if (Alive.IsValid())
		{
			OnGpuConfigured(Config, Report, bForQuery);
		}
	});
}

void FSuperSLMExampleDemo::OnGpuConfigured(const FSuperSLMGpuRuntimeConfig& Config, const FSuperSLMGpuConfigureReport& Report, bool bForQuery)
{
	// Game thread, a later frame.
	FString Error;
	if (Report.Result == ESuperSLMGpuConfigureResult::KBelowMinimum && Report.MinimumK > Config.K)
	{
		// The backend is the authority on the minimum; ask again at it.
		UE_LOG(LogSuperSLMExampleDemo, Warning, TEXT("GPU minimum K is %d, above the example's estimate %d; configuring at %d."), Report.MinimumK, Config.K, Report.MinimumK);
		FSuperSLMGpuRuntimeConfig Retry = Config;
		Retry.K = Report.MinimumK;
		BeginGpuConfigure(Retry, bForQuery);
		FinishConfigureStep();
		return;
	}
	if (Report.Result != ESuperSLMGpuConfigureResult::Success)
	{
		Error = FString::Printf(TEXT("the GPU backend refused the configuration (result %d): %s"), static_cast<int32>(Report.Result), *Report.Message);
	}
	else if (!FSuperSLMGpuSchemaLookup::LookupByName(Model, SuperSLMExample::SchemaName(), GpuSchema, Error))
	{
		Error = FString::Printf(TEXT("the GPU backend cannot resolve the \"%s\" schema: %s"), SuperSLMExample::SchemaName(), *Error);
	}
	else
	{
		// The dispatch budget was derived from the example's dispatches-per-layer figure; the
		// backend's own layers per tick says whether that figure holds for this model.
		const int32 WantedLayers = static_cast<int32>(Config.DispatchBudget / SuperSLMExample::GpuDispatchesPerLayer);
		const int32 LayersPerTick = Gpu->GetLayersPerTick();
		if (LayersPerTick != WantedLayers)
		{
			Error = FString::Printf(TEXT("the GPU backend runs %d layers per tick where the example asked for %d; the example's dispatches-per-layer figure does not fit this model"),
				LayersPerTick, WantedLayers);
		}
	}

	if (Error.IsEmpty())
	{
		bGpuConfigured = true;
		bGpuAvailable = true;
		GpuConfiguredBlockCount = Config.BlockCount;
		GpuConfiguredDispatchBudget = Config.DispatchBudget;
		GpuConfiguredK = Gpu->GetConfiguredK();
		GpuConfiguredMinimumK = Report.MinimumK;
	}
	else if (!bForQuery)
	{
		GpuUnavailableReason = Error;
	}

	if (bForQuery && Readout.State == ESuperSLMExampleRunState::ConfiguringGpu)
	{
		if (Error.IsEmpty() && !BeginRun(PendingSettings, Error))
		{
			// BeginRun() names why.
		}
		if (!Error.IsEmpty())
		{
			Readout.State = ESuperSLMExampleRunState::Failed;
			Readout.Error = Error;
			UE_LOG(LogSuperSLMExampleDemo, Warning, TEXT("Query did not start (%s): %s"), *SuperSLMExample::SettingsDescription(PendingSettings), *Error);
		}
	}
	FinishConfigureStep();
}

void FSuperSLMExampleDemo::FinishConfigureStep()
{
	--PendingConfigures;
	if (!bInitialized && PendingConfigures == 0 && !bInitFailed && bCpuConfigured)
	{
		bInitialized = true;
	}
}

bool FSuperSLMExampleDemo::NeedsGpuReconfigure(const FGpuConfigRequest& Request) const
{
	// Pool size and dispatch budget are configuration. K alone is not (SetFixedTickLatency()).
	const FSuperSLMGpuRuntimeConfig Wanted = GpuConfigFor(Request);
	return !(bGpuConfigured && Gpu != nullptr && Gpu->IsGpuBackendActive()
		&& GpuConfiguredBlockCount == Wanted.BlockCount
		&& GpuConfiguredDispatchBudget == Wanted.DispatchBudget);
}

int32 FSuperSLMExampleDemo::EstimateGpuMinimumK(const FSuperSLMExampleSettings& Settings)
{
	// ceil(BlockCount x num_hidden_layers / layers per tick), and at least BlockCount: a token of each
	// concurrent sequence must fit in K ticks.
	// The GPU backend computes the same figure at Configure() and is the authority; this estimate
	// only avoids asking it for a K it will refuse.
	const int32 N = FMath::Clamp(Settings.ConcurrentQueries, 1, SuperSLMExample::MaxConcurrentQueries);
	const int32 Layers = Settings.IsWholeToken() ? SuperSLMExample::NumHiddenLayers : LayersFor(Settings.FrameBudgetLayers);
	const int32 Composed = FMath::DivideAndRoundUp(N * SuperSLMExample::NumHiddenLayers, Layers);
	return FMath::Max(Composed, N);
}

bool FSuperSLMExampleDemo::BeginSelfCheck()
{
	check(IsInGameThread());
	if (!bInitialized || IsBusy())
	{
		return false;
	}
	// Construction is cheap game-thread work (finds the subsystems, encodes the pinned prompt,
	// reads the shape); TickFrame() steps it.
	SelfCheckRun = MakeUnique<FSuperSLMSelfCheckRun>(Model, FString());
	SelfCheckFrames = 0;
	return true;
}

void FSuperSLMExampleDemo::StepSelfCheck()
{
	++SelfCheckFrames;
	if (!SelfCheckRun->Step(SuperSLMExample::SelfCheckStepBudgetSeconds))
	{
		return;
	}
	SelfCheck = SelfCheckRun->GetReport();
	SelfCheckRun.Reset();
	bHasSelfCheck = true;
	++SelfCheckRunCount;
	LastSelfCheckFrames = SelfCheckFrames;
	if (IsCpuVerdictReadable())
	{
		UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("Self-check: CPU %s -- %s"), *SuperSLMExample::VerdictName(SelfCheck.Cpu.Verdict), *SelfCheck.Cpu.ScopeText);
	}
	else
	{
		UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("Self-check: CPU verdict recorded and withheld by the plugin."));
	}
	if (IsGpuVerdictReadable())
	{
		UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("Self-check: GPU %s -- %s"), *SuperSLMExample::VerdictName(SelfCheck.Gpu.Verdict), *SelfCheck.Gpu.ScopeText);
	}
	else
	{
		UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("Self-check: GPU verdict recorded and withheld (in 1.0 the GPU verdict is always withheld)."));
	}
}

void FSuperSLMExampleDemo::TickFrame(float DeltaSeconds)
{
	check(IsInGameThread());
	if (SelfCheckRun.IsValid())
	{
		// The stepped run ticks both subsystems itself.
		StepSelfCheck();
		return;
	}
	if (bCpuConfigured && !Cpu.IsConfigurePending())
	{
		Cpu.TickOncePerFrame(DeltaSeconds); // once per engine frame, whoever else ticks it
	}
	if (bGpuConfigured && Gpu != nullptr && !Gpu->IsConfigurePending())
	{
		Gpu->TickOncePerFrame(DeltaSeconds);
	}
	Tick();
}

bool FSuperSLMExampleDemo::IsGpuVerified() const
{
	// A quarantined verdict is not read at all.
	return IsGpuVerdictReadable() && SelfCheck.Gpu.Verdict == ESuperSLMSelfCheckVerdict::Verified;
}

bool FSuperSLMExampleDemo::IsBusy() const
{
	return PendingConfigures > 0
		|| SelfCheckRun.IsValid()
		|| Readout.State == ESuperSLMExampleRunState::ConfiguringGpu
		|| Readout.State == ESuperSLMExampleRunState::Running;
}

bool FSuperSLMExampleDemo::BuildPrompts(const FString& Utterance, FString& OutError)
{
	if (!Cpu.Tokenize(ExtractionPromptText(Utterance), ExtractionPromptTokens)
		|| !Cpu.Tokenize(ReplyPromptHeadText(), ReplyPromptHead)
		|| !Cpu.Tokenize(ReplyPromptTailText(Utterance), ReplyPromptTail))
	{
		OutError = TEXT("the model's tokenizer could not encode the prompt");
		return false;
	}
	return true;
}

bool FSuperSLMExampleDemo::Start(const FSuperSLMExampleSettings& InSettings, FString& OutError)
{
	if (!bInitialized)
	{
		OutError = TEXT("the backends are not configured yet");
		return false;
	}
	if (IsBusy())
	{
		OutError = TEXT("a query or backend work is already running");
		return false;
	}

	FSuperSLMExampleSettings Settings = InSettings;
	Settings.FrameBudgetLayers = FMath::Clamp(Settings.FrameBudgetLayers, 1, SuperSLMExample::NumHiddenLayers);
	Settings.ConcurrentQueries = FMath::Clamp(Settings.ConcurrentQueries, 1, SuperSLMExample::MaxConcurrentQueries);
	Settings.RequestedK = FMath::Max(Settings.RequestedK, 1);
	Settings.Utterance.TrimStartAndEndInline();
	if (Settings.Utterance.IsEmpty())
	{
		OutError = TEXT("type what the customer says first");
		return false;
	}

	if (Settings.Backend == ESuperSLMExampleBackend::GPU)
	{
		if (!bGpuAvailable)
		{
			OutError = FString::Printf(TEXT("the GPU backend is unavailable: %s"), *GpuUnavailableReason);
			return false;
		}
		FGpuConfigRequest Request;
		Request.BlockCount = Settings.ConcurrentQueries;
		Request.FrameBudgetLayers = Settings.FrameBudgetLayers;
		Request.RequestedK = Settings.RequestedK;
		const FSuperSLMGpuRuntimeConfig Wanted = GpuConfigFor(Request);
		const int32 WantedK = FMath::Max(Wanted.K, GpuConfiguredMinimumK);
		bool bReconfigure = NeedsGpuReconfigure(Request);
		if (!bReconfigure && WantedK != GpuConfiguredK)
		{
			// K alone: O(1) on the game thread, nothing mapped or allocated. The backend refuses it
			// while a sequence is still vended; reconfiguring is the fallback then.
			FString KError;
			if (Gpu->SetFixedTickLatency(WantedK, KError))
			{
				GpuConfiguredK = Gpu->GetConfiguredK();
			}
			else
			{
				UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("SetFixedTickLatency(%d) refused (%s); reconfiguring instead."), WantedK, *KError);
				bReconfigure = true;
			}
		}
		if (bReconfigure)
		{
			// BeginConfigure(): the heavy part runs on the plugin's pool threads; the query starts
			// in the callback, on the game thread, a later frame (OnGpuConfigured()).
			Readout = FSuperSLMExampleReadout();
			Readout.State = ESuperSLMExampleRunState::ConfiguringGpu;
			Readout.Settings = Settings;
			PendingSettings = Settings;
			FSuperSLMGpuRuntimeConfig Config = Wanted;
			Config.K = WantedK;
			BeginGpuConfigure(Config, /*bForQuery*/ true);
			return true;
		}
	}
	return BeginRun(Settings, OutError);
}

bool FSuperSLMExampleDemo::BeginRun(const FSuperSLMExampleSettings& Settings, FString& OutError)
{
	// Game thread, no backend work running.
	if (Settings.Backend == ESuperSLMExampleBackend::GPU && !Gpu->IsGpuBackendActive())
	{
		OutError = TEXT("the GPU backend is inactive after a device loss; choose the CPU backend");
		return false;
	}

	if (!BuildPrompts(Settings.Utterance, OutError))
	{
		return false;
	}

	Readout = FSuperSLMExampleReadout();
	Readout.State = ESuperSLMExampleRunState::Running;
	Readout.Settings = Settings;
	Readout.QueryCount = Settings.ConcurrentQueries;
	if (Settings.Backend == ESuperSLMExampleBackend::GPU)
	{
		Readout.EffectiveK = GpuConfiguredK;
		Readout.MinimumK = GpuConfiguredMinimumK;
	}

	Queries.Reset();
	Queries.SetNum(Settings.ConcurrentQueries);
	FramesElapsed = 0;
	StartSeconds = FPlatformTime::Seconds();
	CpuLedgerCursor = Cpu.GetJobLedgerAppendedCount();
	CpuTickHistoryCursor = Cpu.GetTickHistoryAppendedCount();
	CpuWorkerMs.Reset();
	CpuKs.Reset();
	CpuHitchesAtStart = Cpu.GetHitchCount();
	CpuTokensPerSecondLast = 0.0;
	GpuBusySamples.Reset();
	LastHostFinishMs = 0.0;
	if (Gpu != nullptr && Settings.Backend == ESuperSLMExampleBackend::GPU)
	{
		GpuHitchesAtStart = Gpu->GetHitchCount();
		GpuDispatchesAtStart = Gpu->GetGpuDispatchCount();
		GpuDispatchesSeen = GpuDispatchesAtStart;
	}

	for (FQuery& Query : Queries)
	{
		TryStartExtraction(Query);
	}
	return true;
}

bool FSuperSLMExampleDemo::TryStartExtraction(FQuery& Query)
{
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = ExtractionPromptTokens;
	Request.MaxNewTokens = SuperSLMExample::ExtractionMaxNewTokens;
	Request.StopTokenIds = SuperSLMExample::StopTokenIds();
	FString Error;

	if (Readout.Settings.Backend == ESuperSLMExampleBackend::CPU)
	{
		FSuperSLMSequence Sequence;
		const ESuperSLMVendResult Vend = Cpu.VendSequence(Sequence);
		if (Vend == ESuperSLMVendResult::PoolExhausted)
		{
			return false; // a returned sequence is still settling; try again next frame
		}
		if (Vend != ESuperSLMVendResult::Success)
		{
			Query.Stage = EStage::Failed;
			Query.Error = TEXT("the CPU backend is not configured");
			return false;
		}
		Query.CpuSequence = Sequence;
		Cpu.SetLayerBudget(Sequence, Readout.Settings.FrameBudgetLayers);
		if (!Cpu.SetSchema(Sequence, CpuSchema, Error) || !Cpu.BeginGeneration(Sequence, Request, Error))
		{
			Query.Stage = EStage::Failed;
			Query.Error = Error;
			return false;
		}
	}
	else
	{
		FSuperSLMGpuSequence Sequence;
		const ESuperSLMGpuDecodePath Path = Readout.Settings.IsWholeToken() ? ESuperSLMGpuDecodePath::OneCall : ESuperSLMGpuDecodePath::Composed;
		const ESuperSLMGpuVendResult Vend = Gpu->VendSequence(Sequence, Path);
		if (Vend == ESuperSLMGpuVendResult::PoolExhausted)
		{
			return false;
		}
		if (Vend != ESuperSLMGpuVendResult::Success)
		{
			Query.Stage = EStage::Failed;
			Query.Error = TEXT("the GPU backend is not configured");
			return false;
		}
		Query.GpuSequence = Sequence;
		if (!Gpu->SetSchema(Sequence, GpuSchema, Error))
		{
			Query.Stage = EStage::Failed;
			Query.Error = Error;
			return false;
		}
		Query.GpuBeginHandle = Gpu->RequestBeginGeneration(Sequence, Request);
		if (!Query.GpuBeginHandle.IsValid())
		{
			Query.Stage = EStage::Failed;
			Query.Error = Gpu->GetLastLifecycleRequestError();
			return false;
		}
	}
	Query.Stage = EStage::Extracting;
	return true;
}

bool FSuperSLMExampleDemo::TryStartReply(FQuery& Query)
{
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = ReplyPromptHead;
	Request.PromptTokens.Append(Query.ExtractionTokens);
	Request.PromptTokens.Append(ReplyPromptTail);
	Request.MaxNewTokens = SuperSLMExample::ReplyMaxNewTokens;
	Request.StopTokenIds = SuperSLMExample::StopTokenIds();
	FString Error;

	if (Readout.Settings.Backend == ESuperSLMExampleBackend::CPU)
	{
		FSuperSLMSequence Sequence;
		const ESuperSLMVendResult Vend = Cpu.VendSequence(Sequence);
		if (Vend == ESuperSLMVendResult::PoolExhausted)
		{
			return false;
		}
		if (Vend != ESuperSLMVendResult::Success)
		{
			Query.Stage = EStage::Failed;
			Query.Error = TEXT("the CPU backend is not configured");
			return false;
		}
		Query.CpuSequence = Sequence;
		Cpu.SetLayerBudget(Sequence, Readout.Settings.FrameBudgetLayers);
		if (!Cpu.BeginGeneration(Sequence, Request, Error))
		{
			Query.Stage = EStage::Failed;
			Query.Error = Error;
			return false;
		}
	}
	else
	{
		FSuperSLMGpuSequence Sequence;
		const ESuperSLMGpuDecodePath Path = Readout.Settings.IsWholeToken() ? ESuperSLMGpuDecodePath::OneCall : ESuperSLMGpuDecodePath::Composed;
		const ESuperSLMGpuVendResult Vend = Gpu->VendSequence(Sequence, Path);
		if (Vend == ESuperSLMGpuVendResult::PoolExhausted)
		{
			return false;
		}
		if (Vend != ESuperSLMGpuVendResult::Success)
		{
			Query.Stage = EStage::Failed;
			Query.Error = TEXT("the GPU backend is not configured");
			return false;
		}
		Query.GpuSequence = Sequence;
		Query.GpuBeginHandle = Gpu->RequestBeginGeneration(Sequence, Request);
		if (!Query.GpuBeginHandle.IsValid())
		{
			Query.Stage = EStage::Failed;
			Query.Error = Gpu->GetLastLifecycleRequestError();
			return false;
		}
	}
	Query.Stage = EStage::Replying;
	return true;
}

const TArray<int32>& FSuperSLMExampleDemo::GeneratedTokens(const FQuery& Query) const
{
	return Readout.Settings.Backend == ESuperSLMExampleBackend::CPU
		? Cpu.GetGeneratedTokens(Query.CpuSequence)
		: Gpu->GetGeneratedTokens(Query.GpuSequence);
}

bool FSuperSLMExampleDemo::IsSequenceTerminal(const FQuery& Query, bool& bOutSchemaComplete, FString& OutWhy) const
{
	bOutSchemaComplete = false;
	const bool bCpu = Readout.Settings.Backend == ESuperSLMExampleBackend::CPU;
	const ESuperSLMSequencePhase Phase = bCpu ? Cpu.GetPhase(Query.CpuSequence) : Gpu->GetPhase(Query.GpuSequence);
	if (Phase == ESuperSLMSequencePhase::Complete)
	{
		return true;
	}
	if (Phase != ESuperSLMSequencePhase::Faulted)
	{
		return false;
	}
	const ESuperSLMDecodeOutcome Outcome = bCpu ? Cpu.GetLastDecodeOutcome(Query.CpuSequence) : Gpu->GetLastDecodeOutcome(Query.GpuSequence);
	// A schema-bound walk that has emitted its last legal token ends in Schema Dead End: the
	// schema is complete. The JSON validator decides whether it completed or stopped short.
	bOutSchemaComplete = Outcome == ESuperSLMDecodeOutcome::SchemaDeadEnd;
	OutWhy = FString::Printf(TEXT("the sequence faulted (%s)"), *DecodeOutcomeName(Outcome));
	if (!bCpu && Gpu->GetLastFaultReason(Query.GpuSequence) == ESuperSLMGpuFaultReason::TerminalDeviceLost)
	{
		OutWhy += TEXT("; the GPU device was lost");
	}
	return true;
}

void FSuperSLMExampleDemo::ReturnSequence(FQuery& Query)
{
	if (Query.CpuSequence.IsValid())
	{
		Cpu.ReturnSequence(Query.CpuSequence);
		Query.CpuSequence = FSuperSLMSequence();
	}
	if (Query.GpuSequence.IsValid() && Gpu != nullptr)
	{
		if (Query.GpuBeginHandle.IsValid() && Gpu->GetLifecycleOpResult(Query.GpuBeginHandle) != ESuperSLMRestoreResult::Pending)
		{
			Gpu->ReleaseLifecycleOpHandle(Query.GpuBeginHandle);
		}
		Query.GpuBeginHandle = FSuperSLMLifecycleOpHandle();
		Gpu->ReturnSequence(Query.GpuSequence);
		Query.GpuSequence = FSuperSLMGpuSequence();
	}
}

void FSuperSLMExampleDemo::PollQuery(FQuery& Query)
{
	const bool bCpu = Readout.Settings.Backend == ESuperSLMExampleBackend::CPU;
	switch (Query.Stage)
	{
	case EStage::WaitingForExtractionSequence:
		TryStartExtraction(Query);
		return;

	case EStage::WaitingForReplySequence:
		TryStartReply(Query);
		return;

	case EStage::Extracting:
	{
		bool bSchemaComplete = false;
		FString Why;
		if (!IsSequenceTerminal(Query, bSchemaComplete, Why))
		{
			return;
		}
		const bool bCompleted = bCpu ? Cpu.GetPhase(Query.CpuSequence) == ESuperSLMSequencePhase::Complete
			: Gpu->GetPhase(Query.GpuSequence) == ESuperSLMSequencePhase::Complete;
		if (!bCompleted && !bSchemaComplete)
		{
			Query.Stage = EStage::Failed;
			Query.Error = FString::Printf(TEXT("extraction: %s"), *Why);
			return;
		}
		Query.ExtractionTokens = GeneratedTokens(Query);
		Query.ExtractionTimeToFirstTokenMs = bCpu ? Cpu.GetTimeToFirstTokenMs(Query.CpuSequence) : Gpu->GetTimeToFirstTokenMs(Query.GpuSequence);
		if (bCpu)
		{
			Query.bExtractionSchemaAccepting = Cpu.GetStats(Query.CpuSequence).bSchemaAccepting;
		}
		ReturnSequence(Query);
		Query.Stage = EStage::WaitingForReplySequence;
		TryStartReply(Query);
		return;
	}

	case EStage::Replying:
	{
		bool bSchemaComplete = false;
		FString Why;
		if (!IsSequenceTerminal(Query, bSchemaComplete, Why))
		{
			return;
		}
		const bool bCompleted = bCpu ? Cpu.GetPhase(Query.CpuSequence) == ESuperSLMSequencePhase::Complete
			: Gpu->GetPhase(Query.GpuSequence) == ESuperSLMSequencePhase::Complete;
		if (!bCompleted)
		{
			Query.Stage = EStage::Failed;
			Query.Error = FString::Printf(TEXT("reply: %s"), *Why);
			return;
		}
		Query.ReplyTokens = GeneratedTokens(Query);
		Query.ReplyTimeToFirstTokenMs = bCpu ? Cpu.GetTimeToFirstTokenMs(Query.CpuSequence) : Gpu->GetTimeToFirstTokenMs(Query.GpuSequence);
		ReturnSequence(Query);
		Query.Stage = EStage::Finished;
		return;
	}

	case EStage::Finished:
	case EStage::Failed:
		return;
	}
}

void FSuperSLMExampleDemo::SampleCostFigures()
{
	if (Readout.Settings.Backend == ESuperSLMExampleBackend::CPU)
	{
		const double Tps = Cpu.GetTokensPerSecond();
		if (Tps > 0.0)
		{
			CpuTokensPerSecondLast = Tps;
		}
		ReadCpuHistory(/*bFinal*/ false);
		return;
	}
	// GetLastGpuBusyMs() reads the most recent drain. One sample is taken per frame in which the
	// backend issued work, so several drains inside one frame contribute their last.
	const int64 Dispatches = Gpu->GetGpuDispatchCount();
	if (Dispatches != GpuDispatchesSeen)
	{
		GpuDispatchesSeen = Dispatches;
		GpuBusySamples.Add(Gpu->GetLastGpuBusyMs());
		LastHostFinishMs = Gpu->GetLastHostFinishMs();
	}
}

void FSuperSLMExampleDemo::ReadCpuHistory(bool bFinal)
{
	// The game thread's own cost of this run: every Tick() since Ask. A tick's row is final once
	// written.
	for (const int64 End = Cpu.GetTickHistoryAppendedCount(); CpuTickHistoryCursor < End; ++CpuTickHistoryCursor)
	{
		if (const FSuperSLMTickReport* TickRow = Cpu.FindTickHistoryRow(CpuTickHistoryCursor))
		{
			Readout.CpuMaxTickMs = FMath::Max(Readout.CpuMaxTickMs, TickRow->DurationMs);
			++Readout.CpuTicks;
		}
	}

	// This run's worker jobs: every decode or prefill job in the ledger since Ask, read once it is
	// delivered, when its figures are final. Only this run's sequences are vended while it runs,
	// so those are its jobs; a job that batches several of them is one row, counted once. While
	// the run goes on, reading stops at the first job not yet delivered; the final read takes the
	// delivered jobs after it too.
	for (const int64 End = Cpu.GetJobLedgerAppendedCount(); CpuLedgerCursor < End; ++CpuLedgerCursor)
	{
		const FSuperSLMWorkerJobReport* Job = Cpu.FindJobLedgerRow(CpuLedgerCursor);
		if (Job == nullptr)
		{
			continue; // overwritten before this run read it
		}
		if (Job->DeliveredAtTick < 0)
		{
			if (!bFinal)
			{
				break;
			}
			continue;
		}
		if (Job->Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job->WorkerCallMs >= 0.0)
		{
			CpuWorkerMs.Add(Job->WorkerCallMs);
			CpuKs.Add(static_cast<double>(Job->K));
		}
	}
}

void FSuperSLMExampleDemo::Tick()
{
	if (Readout.State == ESuperSLMExampleRunState::Running)
	{
		++FramesElapsed;
		SampleCostFigures();

		bool bAllFinished = true;
		for (FQuery& Query : Queries)
		{
			PollQuery(Query);
			if (Query.Stage == EStage::Failed)
			{
				Fail(Query.Error);
				return;
			}
			bAllFinished &= Query.Stage == EStage::Finished;
		}
		if (bAllFinished)
		{
			FinishRun();
		}
	}
}

void FSuperSLMExampleDemo::Fail(const FString& Why)
{
	for (FQuery& Query : Queries)
	{
		ReturnSequence(Query);
	}
	Readout.State = ESuperSLMExampleRunState::Failed;
	Readout.Error = Why;
	UE_LOG(LogSuperSLMExampleDemo, Warning, TEXT("Query failed (%s): %s"), *SuperSLMExample::SettingsDescription(Readout.Settings), *Why);
}

void FSuperSLMExampleDemo::FinishRun()
{
	const bool bCpu = Readout.Settings.Backend == ESuperSLMExampleBackend::CPU;
	const FQuery& First = Queries[0];

	Readout.FramesToAnswer = FramesElapsed;
	Readout.WallMsToAnswer = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	Readout.ExtractionTokens = First.ExtractionTokens;
	Readout.ReplyTokens = First.ReplyTokens;

	TArray<int32> All = First.ExtractionTokens;
	All.Append(First.ReplyTokens);
	Readout.TokenDigestHex = DigestHex(All);
	int32 TotalTokens = 0;
	for (const FQuery& Query : Queries)
	{
		TArray<int32> Tokens = Query.ExtractionTokens;
		Tokens.Append(Query.ReplyTokens);
		TotalTokens += Tokens.Num();
		Readout.QueriesMatchingDigest += DigestHex(Tokens) == Readout.TokenDigestHex ? 1 : 0;
	}

	TArray<FSuperSLMExampleFigure>& F = Readout.Figures;
	F.Add({ TEXT("Frames to answer"), FString::FromInt(Readout.FramesToAnswer), TEXT("example: frames from Ask to the last query's final token") });
	F.Add({ TEXT("Wall time to answer"), FString::Printf(TEXT("%.0f ms"), Readout.WallMsToAnswer), TEXT("example: FPlatformTime from Ask to the last query's final token") });
	F.Add({ TEXT("Prompt time, extraction"), FString::Printf(TEXT("%.1f ms"), First.ExtractionTimeToFirstTokenMs),
		TEXT("GetTimeToFirstTokenMs(): generation start to first token, prompt included") });
	F.Add({ TEXT("Prompt time, reply"), FString::Printf(TEXT("%.1f ms"), First.ReplyTimeToFirstTokenMs),
		TEXT("GetTimeToFirstTokenMs(): generation start to first token, prompt included") });

	if (bCpu)
	{
		// This run's worker jobs and ticks, read every frame and now the rest (ReadCpuHistory()).
		ReadCpuHistory(/*bFinal*/ true);
		F.Add({ TEXT("Worker ms per job (median / max)"), FString::Printf(TEXT("%.2f / %.2f over %d jobs"), Median(CpuWorkerMs), MaxOf(CpuWorkerMs), CpuWorkerMs.Num()),
			TEXT("USuperSLMSubsystem::GetJobLedger(): WorkerCallMs of this run's decode and prefill jobs") });
		F.Add({ TEXT("K in ticks (median / max)"), FString::Printf(TEXT("%.0f / %.0f"), Median(CpuKs), MaxOf(CpuKs)),
			TEXT("USuperSLMSubsystem::GetJobLedger(): K of the same jobs") });
		F.Add({ TEXT("Finish ms per token"), FString::Printf(TEXT("%.2f"), Cpu.GetLastFinishMs()), TEXT("USuperSLMSubsystem::GetLastFinishMs()") });
		F.Add({ TEXT("Prefill ms per prompt token"), FString::Printf(TEXT("%.2f"), Cpu.GetLastPrefillMsPerToken()), TEXT("USuperSLMSubsystem::GetLastPrefillMsPerToken()") });
		F.Add({ TEXT("Tokens per second"), FString::Printf(TEXT("%.1f"), CpuTokensPerSecondLast), TEXT("USuperSLMSubsystem::GetTokensPerSecond(), 1 s window, last nonzero reading") });
		F.Add({ TEXT("Hitches"), FString::FromInt(Cpu.GetHitchCount() - CpuHitchesAtStart), TEXT("USuperSLMSubsystem::GetHitchCount() over this run") });
		F.Add({ TEXT("Schema accepting after extraction"), First.bExtractionSchemaAccepting ? TEXT("yes") : TEXT("no"), TEXT("USuperSLMSubsystem::GetStats() (sslm_stats)") });
		Readout.HostFinishMs = Cpu.GetLastFinishMs();

		// The game thread's own cost of this run, from the plugin's tick history (ReadCpuHistory()).
		F.Add({ TEXT("Game-thread tick ms (max)"), FString::Printf(TEXT("%.3f over %d ticks"), Readout.CpuMaxTickMs, Readout.CpuTicks),
			TEXT("USuperSLMSubsystem::GetTickHistory(): DurationMs (plan + apply) of this run's ticks") });
	}
	else
	{
		Readout.GpuBusyMsSamples = GpuBusySamples;
		Readout.HostFinishMs = LastHostFinishMs;
		Readout.GpuDispatches = Gpu->GetGpuDispatchCount() - GpuDispatchesAtStart;
		Readout.bGpuDeviceHeadActive = Gpu->IsDeviceHeadActive();
		Readout.GpuDeviceHeadStatus = Gpu->GetDeviceHeadStatus();
		Readout.GpuShaderDirectory = Gpu->GetShaderDirectory();
		Readout.GpuLayersPerTick = Gpu->GetLayersPerTick();
		const double WallSeconds = Readout.WallMsToAnswer / 1000.0;

		F.Add({ TEXT("Path"), Readout.Settings.IsWholeToken() ? FString(TEXT("one-call, whole token")) : FString::Printf(TEXT("composed, %d layers per slice"), Readout.Settings.FrameBudgetLayers),
			TEXT("the Frame Budget control") });
		F.Add({ TEXT("GPU ms per slice (median / max)"), FString::Printf(TEXT("%.2f / %.2f over %d samples"), Median(GpuBusySamples), MaxOf(GpuBusySamples), GpuBusySamples.Num()),
			TEXT("USuperSLMGpuSubsystem::GetLastGpuBusyMs() (SuperSLM's gpu_busy_ms), one sample per frame that issued GPU work") });
		F.Add({ TEXT("Host finish ms per token"), FString::Printf(TEXT("%.2f"), LastHostFinishMs), TEXT("USuperSLMGpuSubsystem::GetLastHostFinishMs(), the finish call's wall time") });
		F.Add({ TEXT("Tokens per second"), FString::Printf(TEXT("%.1f"), WallSeconds > 0.0 ? TotalTokens / WallSeconds : 0.0),
			TEXT("example: tokens generated by every query / wall seconds from Ask, prompts included") });
		F.Add({ TEXT("Hitches"), FString::FromInt(Gpu->GetHitchCount() - GpuHitchesAtStart), TEXT("USuperSLMGpuSubsystem::GetHitchCount() over this run (T+K misses, and slices whose gpu_busy_ms span is over 2 ms; under rendering that span can include the renderer's GPU time)") });
		F.Add({ TEXT("K (effective / minimum)"), FString::Printf(TEXT("%d / %d"), Readout.EffectiveK, Readout.MinimumK), TEXT("USuperSLMGpuSubsystem::Configure() report") });
		F.Add({ TEXT("Logits head"), Readout.GpuDeviceHeadStatus, TEXT("USuperSLMGpuSubsystem::GetDeviceHeadStatus()") });
		F.Add({ TEXT("GPU dispatches this run"), FString::Printf(TEXT("%lld"), Readout.GpuDispatches), TEXT("USuperSLMGpuSubsystem::GetGpuDispatchCount()") });
	}

	// Detokenizing is game-thread API (it resolves the model through the plugin's registry). The
	// configured CPU backend already holds the model's mapping, so this is a lookup over a few
	// dozen tokens, not a mapping.
	FString ExtractionText, ReplyText, Error;
	if (!SuperSLM::DetokenizeTokens(Model, First.ExtractionTokens, ExtractionText, Error)
		|| !SuperSLM::DetokenizeTokens(Model, WithoutTrailingStop(First.ReplyTokens), ReplyText, Error))
	{
		Fail(FString::Printf(TEXT("detokenizing the answer failed: %s"), *Error));
		return;
	}
	Readout.ExtractionText = ExtractionText;
	Readout.ReplyText = ReplyText.TrimStartAndEnd();
	Readout.bExtractionSchemaValid = SuperSLMExample::ValidatePotionShopOrder(Readout.ExtractionText, Readout.ExtractionValidation);
	Readout.State = ESuperSLMExampleRunState::Done;

	const bool bEligible = bCpu || IsGpuVerified();
	if (bEligible && Readout.bExtractionSchemaValid && Readout.QueriesMatchingDigest == Readout.QueryCount)
	{
		const FString Key = BaselineKey(Readout.Settings);
		if (!Baselines.Contains(Key))
		{
			FBaseline Baseline;
			Baseline.Digest = Readout.TokenDigestHex;
			Baseline.Description = SuperSLMExample::SettingsDescription(Readout.Settings);
			Baselines.Add(Key, MoveTemp(Baseline));
		}
	}

	UE_LOG(LogSuperSLMExampleDemo, Display, TEXT("Query done (%s): %d frames, digest %s, %d/%d queries identical, extraction %s [%s], reply \"%s\""),
		*SuperSLMExample::SettingsDescription(Readout.Settings), Readout.FramesToAnswer, *Readout.TokenDigestHex,
		Readout.QueriesMatchingDigest, Readout.QueryCount, *Readout.ExtractionText,
		Readout.bExtractionSchemaValid ? TEXT("schema-valid") : *Readout.ExtractionValidation, *Readout.ReplyText);
}

FString FSuperSLMExampleDemo::BaselineKey(const FSuperSLMExampleSettings& Settings)
{
	FString Utterance = Settings.Utterance;
	Utterance.TrimStartAndEndInline();
	return Utterance;
}

bool FSuperSLMExampleDemo::GetInvariantBaseline(const FSuperSLMExampleSettings& Settings, FString& OutDigest, FString& OutDescription) const
{
	if (const FBaseline* Found = Baselines.Find(BaselineKey(Settings)))
	{
		OutDigest = Found->Digest;
		OutDescription = Found->Description;
		return true;
	}
	return false;
}
