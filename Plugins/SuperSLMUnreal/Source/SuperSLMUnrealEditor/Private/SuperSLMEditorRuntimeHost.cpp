#include "SuperSLMEditorRuntimeHost.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMSubsystem.h"

namespace
{
	// superslm_gpu::kQkNormDispatchesPerLayer (gpu_port.h), the larger of Layer 1's two per-layer
	// dispatch counts. DispatchBudget = this x num_hidden_layers resolves to at least the model's
	// whole depth per tick on either count. That is only the load-time value: a composed-path query
	// re-sets the tick's budget to its Frame Budget (review S4, USuperSLMGpuSubsystem::
	// SetLayersPerTick(), FSuperSLMQueryRunner), so concurrent queries share it.
	constexpr uint32 kMaxDispatchesPerLayer = 25;
}

FSuperSLMEditorRuntimeHost::~FSuperSLMEditorRuntimeHost()
{
	if (UObjectInitialized() && !IsEngineExitRequested())
	{
		Unload();
	}
}

bool FSuperSLMEditorRuntimeHost::EnsureGameInstance(FString& OutError)
{
	if (GameInstance.IsValid())
	{
		return true;
	}
	if (GEngine == nullptr)
	{
		OutError = TEXT("no engine");
		return false;
	}
	// FTestWorldWrapper::CreateTestWorld(EWorldType::Game), step for step.
	UGameInstance* NewGameInstance = NewObject<UGameInstance>(GEngine);
	World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (World == nullptr)
	{
		OutError = TEXT("UWorld::CreateWorld failed");
		return false;
	}
	World->SetShouldTick(false);
	World->AddToRoot();

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.OwningGameInstance = NewGameInstance;
	World->SetGameInstance(NewGameInstance);
	WorldContext.SetCurrentWorld(World);

	GameInstance.Reset(NewGameInstance);
	NewGameInstance->Init(); // creates and registers the two subsystems
	return true;
}

void FSuperSLMEditorRuntimeHost::Unload()
{
	++LoadSerial; // a load in flight is superseded; the subsystems' teardown below discards its state
	bLoading = false;
	PendingOnDone = nullptr;
	LoadedModel.Reset();
	GpuStatus.Reset();
	if (World == nullptr)
	{
		GameInstance.Reset();
		return;
	}
	// FTestWorldWrapper::DestroyTestWorld(), step for step.
	World->RemoveFromRoot();
	if (UGameInstance* Instance = World->GetGameInstance())
	{
		Instance->Shutdown(); // each subsystem's Deinitialize()
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	World = nullptr;
	GameInstance.Reset();
}

USuperSLMSubsystem* FSuperSLMEditorRuntimeHost::GetCpuSubsystem() const
{
	return GameInstance.IsValid() ? GameInstance->GetSubsystem<USuperSLMSubsystem>() : nullptr;
}

USuperSLMGpuSubsystem* FSuperSLMEditorRuntimeHost::GetGpuSubsystem() const
{
	USuperSLMGpuSubsystem* Gpu = GameInstance.IsValid() ? GameInstance->GetSubsystem<USuperSLMGpuSubsystem>() : nullptr;
	// Only a GPU configured with the loaded model: a prior load's GPU state is not the loaded one's
	// (a load with the GPU turned off leaves it configured with the previous model).
	return (Gpu != nullptr && Gpu->IsGpuBackendActive() && LoadedModel.IsValid() && Gpu->GetConfiguredModel() == LoadedModel.Get()) ? Gpu : nullptr;
}

FSuperSLMRuntimeConfig FSuperSLMEditorRuntimeHost::MakeCpuConfig(const FSuperSLMModelShapeFacts& Shape, const FSuperSLMEditorHostSettings& InSettings)
{
	const int32 BlockCount = FMath::Max(1, InSettings.BlockCount);
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = BlockCount;
	CpuConfig.MaxPrefillChunkBudget = 512;
	CpuConfig.MaxLayerBudget = Shape.NumHiddenLayers;
	CpuConfig.BlockCount = BlockCount;
	CpuConfig.SequenceLifecycleBudgetMs = InSettings.SequenceLifecycleBudgetMs;
	CpuConfig.TickBudgetMs = InSettings.CpuTickBudgetMs;
	return CpuConfig;
}

void FSuperSLMEditorRuntimeHost::BeginLoad(USuperSLMModel& Model, const FSuperSLMEditorHostSettings& InSettings, TUniqueFunction<void(bool bLoaded, const FString& Error)> OnDone)
{
	check(IsInGameThread());
	const uint32 Serial = ++LoadSerial;
	Settings = InSettings;
	LoadedModel.Reset();
	GpuStatus.Reset();
	PendingOnDone = MoveTemp(OnDone);
	bLoading = true;

	FSuperSLMModelShapeFacts Shape;
	if (!SuperSLMModelInspector::ReadShape(Model, Shape) || Shape.NumHiddenLayers <= 0 || Shape.ContextCap <= 0)
	{
		FinishLoad(Serial, false, TEXT("the model carries no readable CFG1 section"));
		return;
	}
	FString Error;
	if (!EnsureGameInstance(Error))
	{
		FinishLoad(Serial, false, Error);
		return;
	}
	USuperSLMSubsystem* Cpu = GameInstance->GetSubsystem<USuperSLMSubsystem>();
	if (Cpu == nullptr)
	{
		FinishLoad(Serial, false, TEXT("the host GameInstance created no USuperSLMSubsystem"));
		return;
	}
	const int32 BlockCount = FMath::Max(1, Settings.BlockCount);
	FSuperSLMGpuRuntimeConfig GpuConfig;
	GpuConfig.ContextCap = Shape.ContextCap;
	GpuConfig.BlockCount = BlockCount;
	GpuConfig.DispatchBudget = kMaxDispatchesPerLayer * static_cast<uint32>(Shape.NumHiddenLayers);
	// D-SLM7381's floor at LayersPerTick = the whole depth: max(ceil(BlockCount x L / L), BlockCount).
	// Configure() is the authority on it; a KBelowMinimum names the true floor, and the host
	// re-Configure()s once at that value rather than guess.
	GpuConfig.K = BlockCount;
	GpuConfig.TickBudgetMs = Settings.GpuTickBudgetMs;

	TWeakPtr<FSuperSLMEditorRuntimeHost> WeakHost = AsShared();
	TWeakObjectPtr<USuperSLMModel> WeakModel(&Model);
	Cpu->BeginConfigure(&Model, MakeCpuConfig(Shape, Settings), [WeakHost, WeakModel, Serial, GpuConfig](const FSuperSLMConfigureReport& CpuReport)
	{
		// Game thread: the CPU configure has published.
		const TSharedPtr<FSuperSLMEditorRuntimeHost> Host = WeakHost.Pin();
		if (!Host.IsValid() || Host->LoadSerial != Serial)
		{
			return;
		}
		if (CpuReport.Result != ESuperSLMConfigureResult::Success)
		{
			Host->FinishLoad(Serial, false, FString::Printf(TEXT("CPU Configure() refused (%d): %s"), static_cast<int32>(CpuReport.Result), *CpuReport.Message));
			return;
		}
		Host->LoadedModel.Reset(WeakModel.Get());
		if (!Host->Settings.bConfigureGpu)
		{
			Host->GpuStatus = TEXT("GPU backend not configured (turned off in the window)");
			Host->FinishLoad(Serial, true, FString());
			return;
		}
		Host->BeginGpuConfigure(Serial, GpuConfig, /*bRetryAtMinimumK*/ true);
	});
}

void FSuperSLMEditorRuntimeHost::BeginGpuConfigure(uint32 Serial, const FSuperSLMGpuRuntimeConfig& GpuConfig, bool bRetryAtMinimumK)
{
	USuperSLMGpuSubsystem* Gpu = GameInstance.IsValid() ? GameInstance->GetSubsystem<USuperSLMGpuSubsystem>() : nullptr;
	if (Gpu == nullptr || !LoadedModel.IsValid())
	{
		GpuStatus = TEXT("the host GameInstance created no USuperSLMGpuSubsystem");
		FinishLoad(Serial, true, FString());
		return;
	}
	TWeakPtr<FSuperSLMEditorRuntimeHost> WeakHost = AsShared();
	Gpu->BeginConfigure(LoadedModel.Get(), GpuConfig, [WeakHost, Serial, GpuConfig, bRetryAtMinimumK](const FSuperSLMGpuConfigureReport& GpuReport)
	{
		// Game thread: the GPU configure has published (or refused).
		const TSharedPtr<FSuperSLMEditorRuntimeHost> Host = WeakHost.Pin();
		if (!Host.IsValid() || Host->LoadSerial != Serial)
		{
			return;
		}
		if (bRetryAtMinimumK && GpuReport.Result == ESuperSLMGpuConfigureResult::KBelowMinimum && GpuReport.MinimumK > GpuConfig.K)
		{
			FSuperSLMGpuRuntimeConfig Retry = GpuConfig;
			Retry.K = GpuReport.MinimumK;
			Host->BeginGpuConfigure(Serial, Retry, /*bRetryAtMinimumK*/ false);
			return;
		}
		USuperSLMGpuSubsystem* Configured = Host->GetGpuSubsystem();
		if (GpuReport.Result != ESuperSLMGpuConfigureResult::Success || Configured == nullptr)
		{
			Host->GpuStatus = FString::Printf(TEXT("GPU backend unavailable: Configure() refused (%d): %s"), static_cast<int32>(GpuReport.Result), *GpuReport.Message);
		}
		else
		{
			Host->GpuStatus = FString::Printf(TEXT("GPU backend active: K = %d, %d layers per tick; head: %s"),
				Configured->GetConfiguredK(), Configured->GetLayersPerTick(), *Configured->GetDeviceHeadStatus());
		}
		Host->FinishLoad(Serial, true, FString());
	});
}

void FSuperSLMEditorRuntimeHost::FinishLoad(uint32 Serial, bool bLoaded, const FString& Error)
{
	if (Serial != LoadSerial)
	{
		return;
	}
	bLoading = false;
	if (!bLoaded)
	{
		LoadedModel.Reset();
	}
	TUniqueFunction<void(bool, const FString&)> OnDone = MoveTemp(PendingOnDone);
	PendingOnDone = nullptr;
	if (OnDone)
	{
		OnDone(bLoaded, Error);
	}
}
