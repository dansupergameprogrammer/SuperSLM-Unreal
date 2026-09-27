#include "SuperSLMLoadModelAsyncAction.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"

// See the header. Game thread throughout: the factory and Activate() run from the Blueprint
// graph, BeginConfigure()'s OnDone arrives as a game-thread task, and the core ticker fires on
// the game thread.

namespace
{
	// superslm_gpu::kQkNormDispatchesPerLayer (gpu_port.h), the larger of Layer 1's two per-layer
	// dispatch counts, as the editor host uses it: DispatchBudget = this x num_hidden_layers is at
	// least the model's whole depth per tick on either count.
	constexpr uint32 kMaxDispatchesPerLayer = 25;

	// BeginConfigure() tears down the backend's whole state: its pool, and on the CPU its prefixes,
	// on the GPU its mapped adapters. A caller holding any of them would be left with a handle to
	// nothing, so each is refused by name before anything is torn down (review R5-N3). A configure
	// still in flight is refused too: another would supersede it, and its caller would never hear
	// back. Empty when the backend is free to reconfigure.
	FString WhyCpuBusy(const USuperSLMSubsystem& Cpu)
	{
		if (Cpu.IsConfigurePending())
		{
			return TEXT("a CPU configure is already in flight; wait for it to finish");
		}
		const int32 Vended = Cpu.GetPoolOccupiedCount();
		const int32 Prefixes = Cpu.GetLivePrefixCount();
		if (Vended > 0 || Prefixes > 0)
		{
			return FString::Printf(TEXT("the CPU backend still holds %d vended sequence(s) and %d prefix(es); return and release them first"), Vended, Prefixes);
		}
		return FString();
	}

	FString WhyGpuBusy(const USuperSLMGpuSubsystem& Gpu)
	{
		if (Gpu.IsConfigurePending())
		{
			return TEXT("a GPU configure is already in flight; wait for it to finish");
		}
		const int32 Vended = Gpu.GetPoolOccupiedCount();
		const int32 Adapters = Gpu.GetMappedAdapterCount();
		if (Vended > 0 || Adapters > 0)
		{
			return FString::Printf(TEXT("the GPU backend still holds %d vended sequence(s) and %d mapped adapter(s); return and unmap them first"), Vended, Adapters);
		}
		return FString();
	}
}

USuperSLMLoadModelAsyncAction* USuperSLMLoadModelAsyncAction::LoadSuperSLMModelAsync(UObject* WorldContextObject, USuperSLMModel* InModel,
	bool bInConfigureGpu, int32 InPoolSize, float InCpuTickBudgetMs, float InGpuTickBudgetMs, float InSequenceLifecycleBudgetMs, int32 InPrefixBlockCount)
{
	USuperSLMLoadModelAsyncAction* Action = NewObject<USuperSLMLoadModelAsyncAction>();
	Action->Model = InModel;
	Action->WorldContext = WorldContextObject;
	Action->bConfigureGpu = bInConfigureGpu;
	// Out-of-range figures go to Configure() as given, which refuses them by name (R-S1f).
	Action->PoolSize = InPoolSize;
	Action->PrefixBlockCount = InPrefixBlockCount;
	Action->CpuTickBudgetMs = InCpuTickBudgetMs;
	Action->GpuTickBudgetMs = InGpuTickBudgetMs;
	Action->SequenceLifecycleBudgetMs = InSequenceLifecycleBudgetMs;
	// Kept alive by the game instance until Finish() calls SetReadyToDestroy().
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void USuperSLMLoadModelAsyncAction::Activate()
{
	check(IsInGameThread());
	if (Model == nullptr)
	{
		Finish(false, TEXT("Load SuperSLM Model: no model"));
		return;
	}
	UWorld* World = GEngine != nullptr ? GEngine->GetWorldFromContextObject(WorldContext.Get(), EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	USuperSLMSubsystem* CpuSubsystem = GameInstance != nullptr ? GameInstance->GetSubsystem<USuperSLMSubsystem>() : nullptr;
	if (CpuSubsystem == nullptr)
	{
		Finish(false, TEXT("Load SuperSLM Model: no GameInstance with a USuperSLMSubsystem (the backends are GameInstance subsystems)"));
		return;
	}
	USuperSLMGpuSubsystem* GpuSubsystem = GameInstance->GetSubsystem<USuperSLMGpuSubsystem>();
	FSuperSLMModelShapeFacts Shape;
	if (!SuperSLMModelInspector::ReadShape(*Model, Shape) || Shape.NumHiddenLayers <= 0 || Shape.ContextCap <= 0)
	{
		Finish(false, FString::Printf(TEXT("Load SuperSLM Model: %s carries no readable CFG1 section"), *Model->GetName()));
		return;
	}
	// Refused before anything is torn down (WhyCpuBusy/WhyGpuBusy). The GPU is checked only when
	// it will be reconfigured, and again just before it is (OnCpuConfigured).
	FString WhyBusy = WhyCpuBusy(*CpuSubsystem);
	if (WhyBusy.IsEmpty() && bConfigureGpu && GpuSubsystem != nullptr)
	{
		WhyBusy = WhyGpuBusy(*GpuSubsystem);
	}
	if (!WhyBusy.IsEmpty())
	{
		Finish(false, FString::Printf(TEXT("Load SuperSLM Model refused: %s"), *WhyBusy));
		return;
	}

	Cpu = CpuSubsystem;
	Gpu = GpuSubsystem;
	NumHiddenLayers = Shape.NumHiddenLayers;
	ContextCap = Shape.ContextCap;

	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = PoolSize;
	Config.MaxPrefillChunkBudget = 512;
	Config.MaxLayerBudget = Shape.NumHiddenLayers;
	Config.BlockCount = PoolSize;
	Config.PrefixBlockCount = PrefixBlockCount;
	Config.SequenceLifecycleBudgetMs = SequenceLifecycleBudgetMs;
	Config.TickBudgetMs = CpuTickBudgetMs;

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USuperSLMLoadModelAsyncAction::OnTick));
	const TSharedRef<bool, ESPMode::ThreadSafe> Token = MakeShared<bool, ESPMode::ThreadSafe>(true);
	PendingToken = Token;
	TWeakObjectPtr<USuperSLMLoadModelAsyncAction> WeakThis(this);
	CpuSubsystem->BeginConfigure(Model, Config, [WeakThis, Token](const FSuperSLMConfigureReport& Report)
	{
		if (USuperSLMLoadModelAsyncAction* Self = WeakThis.Get())
		{
			Self->OnCpuConfigured(Report);
		}
	});
}

void USuperSLMLoadModelAsyncAction::OnCpuConfigured(const FSuperSLMConfigureReport& Report)
{
	if (bFinished)
	{
		return;
	}
	if (Report.Result != ESuperSLMConfigureResult::Success)
	{
		Finish(false, FString::Printf(TEXT("Load SuperSLM Model: the CPU backend refused the configuration (result %d): %s"), static_cast<int32>(Report.Result), *Report.Message));
		return;
	}
	bCpuConfigured = true;
	if (!bConfigureGpu)
	{
		Finish(true, TEXT("GPU backend not asked for"));
		return;
	}
	if (!Gpu.IsValid())
	{
		Finish(true, TEXT("GPU backend unavailable: the GameInstance has no USuperSLMGpuSubsystem"));
		return;
	}
	// Review R5-W1: the GPU stayed configured with its previous model through the CPU phase, so a
	// sequence may have been vended on it (or an adapter mapped, or a configure started) since
	// Activate(). Checked again here, before BeginGpu()'s teardown; on a hit the GPU is left as it
	// is and the load finishes with the CPU only, naming why.
	const FString WhyGpu = WhyGpuBusy(*Gpu);
	if (!WhyGpu.IsEmpty())
	{
		Finish(true, FString::Printf(TEXT("GPU backend not reconfigured: %s"), *WhyGpu));
		return;
	}
	// D-SLM7381's floor at LayersPerTick = the whole depth is PoolSize; Configure() is the
	// authority on it, and a KBelowMinimum is retried once at the floor it names, as the editor
	// host does.
	BeginGpu(FMath::Max(1, PoolSize), /*bRetryAtMinimumK*/ true);
}

void USuperSLMLoadModelAsyncAction::BeginGpu(int32 K, bool bRetryAtMinimumK)
{
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = ContextCap;
	Config.BlockCount = PoolSize;
	Config.DispatchBudget = kMaxDispatchesPerLayer * static_cast<uint32>(NumHiddenLayers);
	Config.K = K;
	Config.TickBudgetMs = GpuTickBudgetMs;

	const TSharedRef<bool, ESPMode::ThreadSafe> Token = MakeShared<bool, ESPMode::ThreadSafe>(true);
	PendingToken = Token;
	TWeakObjectPtr<USuperSLMLoadModelAsyncAction> WeakThis(this);
	// Without the GPU build this calls OnDone at once with Configure()'s refusal.
	Gpu->BeginConfigure(Model, Config, [WeakThis, Token, K, bRetryAtMinimumK](const FSuperSLMGpuConfigureReport& Report)
	{
		if (USuperSLMLoadModelAsyncAction* Self = WeakThis.Get())
		{
			Self->OnGpuConfigured(Report, K, bRetryAtMinimumK);
		}
	});
}

void USuperSLMLoadModelAsyncAction::OnGpuConfigured(const FSuperSLMGpuConfigureReport& Report, int32 K, bool bRetryAtMinimumK)
{
	if (bFinished)
	{
		return;
	}
	if (bRetryAtMinimumK && Report.Result == ESuperSLMGpuConfigureResult::KBelowMinimum && Report.MinimumK > K && Gpu.IsValid())
	{
		BeginGpu(Report.MinimumK, /*bRetryAtMinimumK*/ false);
		return;
	}
	if (Report.Result != ESuperSLMGpuConfigureResult::Success || !Gpu.IsValid() || !Gpu->IsGpuBackendActive())
	{
		Finish(true, FString::Printf(TEXT("GPU backend unavailable: Configure() refused (result %d): %s"), static_cast<int32>(Report.Result), *Report.Message));
		return;
	}
	Finish(true, FString::Printf(TEXT("GPU backend active: K = %d, %d layers per tick"), Gpu->GetConfiguredK(), Gpu->GetLayersPerTick()));
}

bool USuperSLMLoadModelAsyncAction::OnTick(float /*DeltaSeconds*/)
{
	if (bFinished)
	{
		TickHandle.Reset();
		return false;
	}
	if (!PendingToken.IsValid())
	{
		// The pending BeginConfigure()'s OnDone was destroyed without being called.
		TickHandle.Reset(); // returning false removes this ticker
		Finish(bCpuConfigured, bCpuConfigured
			? FString(TEXT("GPU backend unavailable: its configure was superseded by a later configure or the subsystem's shutdown"))
			: FString(TEXT("Load SuperSLM Model: the CPU configure was superseded by a later configure or the subsystem's shutdown")));
		return false;
	}
	return true;
}

void USuperSLMLoadModelAsyncAction::Finish(bool bLoaded, const FString& Message)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	FSuperSLMLoadModelResultBP Result;
	const USuperSLMSubsystem* CpuSubsystem = Cpu.Get();
	const USuperSLMGpuSubsystem* GpuSubsystem = Gpu.Get();
	Result.bCpuConfigured = bCpuConfigured && CpuSubsystem != nullptr && CpuSubsystem->GetConfiguredModel() == Model;
	Result.bGpuBackendActive = bLoaded && bConfigureGpu && GpuSubsystem != nullptr && GpuSubsystem->IsGpuBackendActive() && GpuSubsystem->GetConfiguredModel() == Model;
	Result.Message = Message;
	if (bLoaded)
	{
		Loaded.Broadcast(Result);
	}
	else
	{
		Failed.Broadcast(Result);
	}
	SetReadyToDestroy();
}

void USuperSLMLoadModelAsyncAction::StopTicking()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
}

void USuperSLMLoadModelAsyncAction::SetReadyToDestroy()
{
	StopTicking();
	Super::SetReadyToDestroy();
}

void USuperSLMLoadModelAsyncAction::BeginDestroy()
{
	StopTicking();
	Super::BeginDestroy();
}
