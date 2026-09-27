#include "SuperSLMExampleScene.h"

#include "Camera/CameraActor.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SSuperSLMExampleWidget.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMExample, Log, All);

ASuperSLMExampleScene::ASuperSLMExampleScene()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

ASuperSLMExampleScene* ASuperSLMExampleScene::Find(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	for (TActorIterator<ASuperSLMExampleScene> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void ASuperSLMExampleScene::BeginPlay()
{
	Super::BeginPlay();

	BuildSet();

	if (GEngine != nullptr && GEngine->GameViewport != nullptr)
	{
		Widget = SNew(SSuperSLMExampleWidget).Scene(this);
		GEngine->GameViewport->AddViewportWidgetContent(Widget.ToSharedRef(), 10);
	}

	if (!FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExampleModel="), ModelPath))
	{
		ModelPath = GetDefaultModelPath();
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("SuperSLMExampleSkipLoad")))
	{
		LoadState = ELoadState::Skipped;
		Status = TEXT("The model was not loaded (-SuperSLMExampleSkipLoad).");
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SuperSLMExamplePackagedCheck")))
	{
		StartPackagedCheck(/*bExitWhenDone*/ true);
	}
	BeginModelLoad();
}

void ASuperSLMExampleScene::BeginModelLoad()
{
	// The engine's async loader reads the package; the completion callback runs on the game
	// thread. A cooked model's payload is memory-mapped, so the load does not copy the weights.
	LoadState = ELoadState::LoadingModel;
	Status = FString::Printf(TEXT("Loading %s..."), *ModelPath);
	const FString PackageName = FPackageName::ObjectPathToPackageName(ModelPath);
	LoadPackageAsync(PackageName, FLoadPackageAsyncDelegate::CreateWeakLambda(this,
		[this](const FName&, UPackage*, EAsyncLoadingResult::Type Result)
		{
			OnModelPackageLoaded(Result);
		}));
}

void ASuperSLMExampleScene::OnModelPackageLoaded(EAsyncLoadingResult::Type Result)
{
	// Game thread.
	if (bEnded)
	{
		return; // the load finished after the scene ended play
	}
	Model = Result == EAsyncLoadingResult::Succeeded ? Cast<USuperSLMModel>(FSoftObjectPath(ModelPath).ResolveObject()) : nullptr;
	if (Model == nullptr)
	{
		LoadState = ELoadState::Failed;
		Status = FString::Printf(TEXT("No model asset at %s. Import the release's example .sslm there (see GETTING_STARTED.md, \"The example project\")."), *ModelPath);
		UE_LOG(LogSuperSLMExample, Error, TEXT("%s"), *Status);
		return;
	}
	UGameInstance* GameInstance = GetGameInstance();
	USuperSLMSubsystem* Cpu = GameInstance ? GameInstance->GetSubsystem<USuperSLMSubsystem>() : nullptr;
	USuperSLMGpuSubsystem* Gpu = GameInstance ? GameInstance->GetSubsystem<USuperSLMGpuSubsystem>() : nullptr;
	if (Cpu == nullptr)
	{
		LoadState = ELoadState::Failed;
		Status = TEXT("The SuperSLM CPU subsystem is missing; is the SuperSLMUnreal plugin enabled?");
		UE_LOG(LogSuperSLMExample, Error, TEXT("%s"), *Status);
		return;
	}
	Demo = MakeUnique<FSuperSLMExampleDemo>(*Model, *Cpu, Gpu);
	Demo->BeginInitialize();
	LoadState = ELoadState::ConfiguringBackends;
	Status = TEXT("Configuring the CPU and GPU backends (BeginConfigure; the heavy part is off the game thread)...");
}

void ASuperSLMExampleScene::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEnded = true;
	if (Widget.IsValid() && GEngine != nullptr && GEngine->GameViewport != nullptr)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
	}
	Widget.Reset();
	if (Demo.IsValid())
	{
		// Nothing waits: a pending BeginConfigure() callback finds the demo gone and does nothing,
		// and a stepped self-check returns its sequence when it is destroyed.
		Demo.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void ASuperSLMExampleScene::BuildSet()
{
	UWorld* World = GetWorld();
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	auto SpawnMesh = [World](UStaticMesh* Mesh, const FVector& Location, const FVector& Scale)
	{
		if (Mesh == nullptr)
		{
			return;
		}
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (Actor != nullptr)
		{
			Actor->SetMobility(EComponentMobility::Movable);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->SetActorScale3D(Scale);
		}
	};

	// A floor, a counter, and a shelf of potions behind it.
	SpawnMesh(Plane, FVector(0.0, 0.0, 0.0), FVector(20.0, 20.0, 1.0));
	SpawnMesh(Cube, FVector(300.0, 0.0, 50.0), FVector(1.0, 4.0, 1.0));
	for (int32 I = 0; I < 5; ++I)
	{
		SpawnMesh(Cylinder, FVector(420.0, -160.0 + 80.0 * I, 130.0), FVector(0.3, 0.3, 0.6));
	}

	if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 500.0), FRotator(-45.0, 30.0, 0.0)))
	{
		Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		Sun->GetLightComponent()->SetIntensity(6.0f);
	}

	Camera = World->SpawnActor<ACameraActor>(FVector(-250.0, 0.0, 220.0), FRotator(-15.0, 0.0, 0.0));
	bSetBuilt = Camera != nullptr;
}

void ASuperSLMExampleScene::ConfigureInput()
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller == nullptr || !Widget.IsValid())
	{
		return;
	}
	// No pawn is spawned (the game mode's DefaultPawnClass is null), so the controller keeps
	// whatever view target it is given.
	if (Camera != nullptr)
	{
		Controller->bAutoManageActiveCameraTarget = false;
		Controller->SetViewTarget(Camera);
	}
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(Widget);
	Mode.SetHideCursorDuringCapture(false);
	Controller->SetInputMode(Mode);
	Controller->SetShowMouseCursor(true);
	bInputConfigured = true;
}

bool ASuperSLMExampleScene::CanAsk() const
{
	return LoadState == ELoadState::Ready && Demo.IsValid() && !Demo->IsBusy();
}

bool ASuperSLMExampleScene::Ask(FString& OutError)
{
	if (!CanAsk())
	{
		OutError = LoadState == ELoadState::Ready ? FString(TEXT("a query or the self-check is running")) : Status;
		return false;
	}
	return Demo->Start(Settings, OutError);
}

bool ASuperSLMExampleScene::RequestSelfCheck()
{
	if (!CanAsk() || !Demo->BeginSelfCheck())
	{
		return false;
	}
	LoadState = ELoadState::RunningSelfCheck;
	Status = TEXT("Running the determinism self-check on both backends (stepped, a slice per frame)...");
	return true;
}

void ASuperSLMExampleScene::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bInputConfigured)
	{
		ConfigureInput();
	}

	if (Demo.IsValid())
	{
		// Steps a running self-check, or ticks the configured subsystems (bookkeeping: plan and
		// apply; the inference runs on the plugin's own threads) and advances any query.
		Demo->TickFrame(DeltaSeconds);
		AdvanceLoad();
	}

	AdvancePackagedCheck();
}

void ASuperSLMExampleScene::AdvanceLoad()
{
	switch (LoadState)
	{
	case ELoadState::ConfiguringBackends:
		if (Demo->HasInitializationFailed())
		{
			LoadState = ELoadState::Failed;
			Status = Demo->GetInitializationError();
			UE_LOG(LogSuperSLMExample, Error, TEXT("%s"), *Status);
			return;
		}
		if (!Demo->IsInitialized())
		{
			return;
		}
		UE_LOG(LogSuperSLMExample, Display, TEXT("Loaded %s: %lld bytes, memory-mapped: %s. GPU backend: %s."),
			*ModelPath, Model->GetMappedArtifactSize(), Model->IsPayloadMemoryMapped() ? TEXT("yes") : TEXT("no"),
			Demo->IsGpuAvailable() ? TEXT("configured") : *Demo->GetGpuUnavailableReason());
		// The self-check runs on load.
		if (Demo->BeginSelfCheck())
		{
			LoadState = ELoadState::RunningSelfCheck;
			Status = TEXT("Running the determinism self-check on both backends (stepped, a slice per frame)...");
		}
		else
		{
			LoadState = ELoadState::Ready;
			Status = TEXT("Ready. The self-check could not start.");
		}
		return;

	case ELoadState::RunningSelfCheck:
		if (!Demo->IsSelfCheckRunning())
		{
			LoadState = ELoadState::Ready;
			Status = TEXT("Ready.");
		}
		return;

	default:
		return;
	}
}

bool ASuperSLMExampleScene::StartPackagedCheck(bool bExitWhenDone)
{
	if (PackagedCheckStep != EPackagedCheckStep::Off)
	{
		return false;
	}
	PackagedCheckRuns.Reset();
	bPackagedCheckSelfCheckCommandRan = false;
	bPackagedCheckSelfCheckReported = false;
	bPackagedCheckExitWhenDone = bExitWhenDone;
	PackagedCheckStep = EPackagedCheckStep::WaitingForReady;
	UE_LOG(LogSuperSLMExample, Display, TEXT("SuperSLMExample.PackagedCheck: started"));
	return true;
}

void ASuperSLMExampleScene::AdvancePackagedCheck()
{
	if (PackagedCheckStep == EPackagedCheckStep::Off)
	{
		return;
	}
	if (LoadState == ELoadState::Failed || LoadState == ELoadState::Skipped)
	{
		FinishPackagedCheck();
		return;
	}
	if (LoadState != ELoadState::Ready || !Demo.IsValid() || Demo->IsBusy())
	{
		return;
	}

	FSuperSLMExampleSettings Defaults; // the check runs at the default settings
	FString Error;
	switch (PackagedCheckStep)
	{
	case EPackagedCheckStep::WaitingForReady:
		// The self-check already ran on load; the check runs it again through its console command, the
		// entry point a packaged build offers.
		PackagedCheckStep = EPackagedCheckStep::SelfCheck;
		PackagedCheckSelfCheckRunsBefore = Demo->GetSelfCheckRunCount();
		if (!IConsoleManager::Get().ProcessUserConsoleInput(TEXT("SuperSLMExample.SelfCheck"), *GLog, GetWorld())
			|| LoadState != ELoadState::RunningSelfCheck)
		{
			UE_LOG(LogSuperSLMExample, Error, TEXT("SuperSLMExample.PackagedCheck: the SuperSLMExample.SelfCheck console command did not run"));
			bPackagedCheckSelfCheckCommandRan = false;
		}
		else
		{
			bPackagedCheckSelfCheckCommandRan = true;
		}
		return;

	case EPackagedCheckStep::SelfCheck:
		// The console command's own run has come back with a report.
		bPackagedCheckSelfCheckReported = Demo->GetSelfCheckRunCount() > PackagedCheckSelfCheckRunsBefore;
		Defaults.Backend = ESuperSLMExampleBackend::CPU;
		Settings = Defaults;
		if (!Demo->Start(Defaults, Error))
		{
			UE_LOG(LogSuperSLMExample, Error, TEXT("SuperSLMExample.PackagedCheck: the CPU query did not start: %s"), *Error);
			FinishPackagedCheck();
			return;
		}
		PackagedCheckStep = EPackagedCheckStep::Cpu;
		return;

	case EPackagedCheckStep::Cpu:
		PackagedCheckRuns.Add(Demo->GetReadout());
		Defaults.Backend = ESuperSLMExampleBackend::GPU;
		Settings = Defaults;
		if (!Demo->Start(Defaults, Error))
		{
			UE_LOG(LogSuperSLMExample, Error, TEXT("SuperSLMExample.PackagedCheck: the GPU query did not start: %s"), *Error);
			FinishPackagedCheck();
			return;
		}
		PackagedCheckStep = EPackagedCheckStep::Gpu;
		return;

	case EPackagedCheckStep::Gpu:
		PackagedCheckRuns.Add(Demo->GetReadout());
		FinishPackagedCheck();
		return;

	default:
		return;
	}
}

void ASuperSLMExampleScene::FinishPackagedCheck()
{
	PackagedCheckStep = EPackagedCheckStep::Off;

	struct FCheck
	{
		FString Name;
		bool bPass;
		FString Detail;
	};
	TArray<FCheck> Checks;
	auto AddCheck = [&Checks](const FString& Name, bool bPass, const FString& Detail)
	{
		Checks.Add({ Name, bPass, Detail });
	};

	const FSuperSLMExampleReadout* CpuRun = nullptr;
	const FSuperSLMExampleReadout* GpuRun = nullptr;
	for (const FSuperSLMExampleReadout& Run : PackagedCheckRuns)
	{
		(Run.Settings.Backend == ESuperSLMExampleBackend::CPU ? CpuRun : GpuRun) = &Run;
	}

	// The checks assert what the packaged example must show, clause by clause, and nothing more:
	//  (a) a schema-valid extraction plus a voiced reply, across frames, output read;
	//  (b) the self-check's packaged console command runs and reports;
	//  (c) the GPU backend dispatches with the device head on, which proves the GPU shaders
	//      (logits_site among them) were staged loose under the packaged plugin's
	//      Binaries/Win64/shaders and found there;
	//  (d) it reports gpu_busy_ms per slice and the host finish ms, at the default setting, with
	//      the example scene rendering.
	// "pre." checks are the preconditions the clauses are read under.
	AddCheck(TEXT("pre.packaged_build"), FPlatformProperties::RequiresCookedData(), LexToString(FApp::GetBuildConfiguration()));
	AddCheck(TEXT("pre.model_loaded"), LoadState == ELoadState::Ready, Status);

	// (a), on each backend the example runs.
	auto CheckRun = [&AddCheck](const TCHAR* Backend, const FSuperSLMExampleReadout* Run)
	{
		if (Run == nullptr)
		{
			AddCheck(FString::Printf(TEXT("a.%s.ran"), Backend), false, TEXT("the query did not run"));
			return;
		}
		AddCheck(FString::Printf(TEXT("a.%s.completed"), Backend), Run->State == ESuperSLMExampleRunState::Done, Run->Error);
		AddCheck(FString::Printf(TEXT("a.%s.extraction_schema_valid"), Backend), Run->bExtractionSchemaValid,
			FString::Printf(TEXT("%s -> %s"), *Run->ExtractionText, *Run->ExtractionValidation));
		AddCheck(FString::Printf(TEXT("a.%s.reply_voiced_and_read"), Backend), !Run->ReplyText.IsEmpty(), Run->ReplyText);
		AddCheck(FString::Printf(TEXT("a.%s.across_frames"), Backend), Run->FramesToAnswer > 1,
			FString::Printf(TEXT("%d frames, %.0f ms"), Run->FramesToAnswer, Run->WallMsToAnswer));
	};
	CheckRun(TEXT("cpu"), CpuRun);
	CheckRun(TEXT("gpu"), GpuRun);

	// (b): the command ran, and its own run came back with a report. A report is the device, the
	// pin, the plugin version and the model hash, with a verdict (Verified or Diverged, not Not
	// Yet Run) on every arm that may be read. Which arms may be read is the plugin's call
	// (FSuperSLMSelfCheckBackendResult::bQuarantined): in 1.0 the GPU verdict is always withheld,
	// so the plugin records it and this check neither reads nor asserts it. The CPU arm must be
	// readable.
	AddCheck(TEXT("b.self_check_console_command_ran"), bPackagedCheckSelfCheckCommandRan, TEXT("SuperSLMExample.SelfCheck"));
	{
		const bool bHasReport = Demo.IsValid() && bPackagedCheckSelfCheckReported;
		const FSuperSLMSelfCheckReport* R = bHasReport ? &Demo->GetSelfCheckReport() : nullptr;
		const bool bIdentified = R != nullptr && !R->DeviceLabel.IsEmpty() && !R->LayerOneTagAndCommit.IsEmpty()
			&& !R->PluginVersion.IsEmpty() && !R->ArtifactHash.IsEmpty();
		const bool bCpuVerdict = R != nullptr && Demo->IsCpuVerdictReadable() && R->Cpu.Verdict != ESuperSLMSelfCheckVerdict::NotYetRun;
		bool bGpuVerdict = true;
		FString GpuDetail = TEXT("GPU verdict recorded and withheld (always withheld in 1.0), not asserted");
		if (R != nullptr && Demo->IsGpuVerdictReadable())
		{
			bGpuVerdict = R->Gpu.Verdict != ESuperSLMSelfCheckVerdict::NotYetRun;
			GpuDetail = FString::Printf(TEXT("GPU %s -- %s"), *SuperSLMExample::VerdictName(R->Gpu.Verdict), *R->Gpu.ScopeText);
		}
		AddCheck(TEXT("b.self_check_reported"), bIdentified && bCpuVerdict && bGpuVerdict,
			R == nullptr ? FString(TEXT("the command's run produced no report"))
				: FString::Printf(TEXT("CPU %s -- %s; %s; device %s; %s; plugin %s; model %s"),
					*SuperSLMExample::VerdictName(R->Cpu.Verdict), *R->Cpu.ScopeText, *GpuDetail,
					*R->DeviceLabel, *R->LayerOneTagAndCommit, *R->PluginVersion, *R->ArtifactHash));
	}

	if (GpuRun == nullptr)
	{
		AddCheck(TEXT("c.gpu_ran"), false, TEXT("the GPU query did not run"));
	}
	else
	{
		// (c)
		AddCheck(TEXT("c.gpu_dispatched_with_device_head_on"), GpuRun->GpuDispatches > 0 && GpuRun->bGpuDeviceHeadActive,
			FString::Printf(TEXT("%lld dispatches; logits head %s"), GpuRun->GpuDispatches, *GpuRun->GpuDeviceHeadStatus));

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		FString Expected = Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Binaries") / TEXT("Win64") / TEXT("shaders")) : FString();
		FString Actual = GpuRun->GpuShaderDirectory;
		FPaths::NormalizeDirectoryName(Expected);
		FPaths::NormalizeDirectoryName(Actual);
		TArray<FString> Shaders;
		if (!Actual.IsEmpty())
		{
			IFileManager::Get().FindFiles(Shaders, *(Actual / TEXT("*.cso")), /*Files*/ true, /*Directories*/ false);
		}
		const bool bLogitsSite = Shaders.ContainsByPredicate([](const FString& Name) { return Name.Equals(SuperSLMExample::LogitsSiteShaderFile(), ESearchCase::IgnoreCase); });
		AddCheck(TEXT("c.shaders_found_under_packaged_plugin"),
			!Actual.IsEmpty() && !FPaths::IsRelative(Actual) && Actual.Equals(Expected, ESearchCase::IgnoreCase)
				&& Shaders.Num() == SuperSLMExample::ExpectedGpuShaderCount && bLogitsSite,
			FString::Printf(TEXT("shader_dir %s (plugin's %s); %d .cso files, %d expected; %s %s"),
				*Actual, *Expected, Shaders.Num(), SuperSLMExample::ExpectedGpuShaderCount,
				SuperSLMExample::LogitsSiteShaderFile(), bLogitsSite ? TEXT("present") : TEXT("missing")));

		// (d)
		bool bSamplesValid = GpuRun->GpuBusyMsSamples.Num() > 0;
		for (double V : GpuRun->GpuBusyMsSamples)
		{
			bSamplesValid &= FMath::IsFinite(V) && V >= 0.0;
		}
		AddCheck(TEXT("d.gpu_busy_ms_per_slice_reported"), bSamplesValid,
			FString::Printf(TEXT("%d samples, one per frame that issued GPU work"), GpuRun->GpuBusyMsSamples.Num()));
		AddCheck(TEXT("d.host_finish_ms_reported"), GpuRun->HostFinishMs > 0.0, FString::Printf(TEXT("%.3f ms"), GpuRun->HostFinishMs));
		const FSuperSLMExampleSettings Defaults;
		AddCheck(TEXT("d.at_default_setting"), GpuRun->Settings.FrameBudgetLayers == Defaults.FrameBudgetLayers
				&& GpuRun->Settings.ConcurrentQueries == Defaults.ConcurrentQueries && GpuRun->Settings.RequestedK == Defaults.RequestedK,
			SuperSLMExample::SettingsDescription(GpuRun->Settings));
		AddCheck(TEXT("d.scene_rendering"), FApp::CanEverRender() && bSetBuilt && GEngine != nullptr && GEngine->GameViewport != nullptr,
			FApp::CanEverRender() ? TEXT("rendering, example set built, game viewport present") : TEXT("this process cannot render (-nullrhi or a server)"));
	}

	bool bPass = true;
	TArray<TSharedPtr<FJsonValue>> CheckValues;
	for (const FCheck& C : Checks)
	{
		bPass &= C.bPass;
		UE_LOG(LogSuperSLMExample, Display, TEXT("SuperSLMExample.PackagedCheck: %s %s -- %s"), C.bPass ? TEXT("PASS") : TEXT("FAIL"), *C.Name, *C.Detail);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), C.Name);
		O->SetBoolField(TEXT("pass"), C.bPass);
		O->SetStringField(TEXT("detail"), C.Detail);
		CheckValues.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("check"), TEXT("packaged example"));
	Root->SetBoolField(TEXT("pass"), bPass);
	Root->SetStringField(TEXT("build_configuration"), LexToString(FApp::GetBuildConfiguration()));
	Root->SetBoolField(TEXT("cooked"), FPlatformProperties::RequiresCookedData());
	Root->SetStringField(TEXT("model_path"), ModelPath);
	if (Model != nullptr)
	{
		Root->SetNumberField(TEXT("model_bytes"), static_cast<double>(Model->GetMappedArtifactSize()));
		Root->SetBoolField(TEXT("model_memory_mapped"), Model->IsPayloadMemoryMapped());
	}
	if (Demo.IsValid() && Demo->HasSelfCheckReport())
	{
		const FSuperSLMSelfCheckReport& R = Demo->GetSelfCheckReport();
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		if (Demo->IsCpuVerdictReadable())
		{
			S->SetStringField(TEXT("cpu_verdict"), SuperSLMExample::VerdictName(R.Cpu.Verdict));
			S->SetStringField(TEXT("cpu_scope"), R.Cpu.ScopeText);
		}
		else
		{
			S->SetStringField(TEXT("cpu_verdict"), TEXT("withheld by the plugin"));
		}
		if (Demo->IsGpuVerdictReadable())
		{
			S->SetStringField(TEXT("gpu_verdict"), SuperSLMExample::VerdictName(R.Gpu.Verdict));
			S->SetStringField(TEXT("gpu_scope"), R.Gpu.ScopeText);
		}
		else
		{
			S->SetStringField(TEXT("gpu_verdict"), TEXT("withheld (in 1.0 the GPU verdict is always withheld)"));
		}
		S->SetStringField(TEXT("device"), R.DeviceLabel);
		S->SetStringField(TEXT("layer1"), R.LayerOneTagAndCommit);
		S->SetStringField(TEXT("plugin_version"), R.PluginVersion);
		S->SetStringField(TEXT("artifact_hash"), R.ArtifactHash);
		Root->SetObjectField(TEXT("self_check"), S);
	}
	TArray<TSharedPtr<FJsonValue>> RunValues;
	for (const FSuperSLMExampleReadout& Run : PackagedCheckRuns)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("settings"), SuperSLMExample::SettingsDescription(Run.Settings));
		O->SetStringField(TEXT("token_digest"), Run.TokenDigestHex);
		O->SetStringField(TEXT("extraction"), Run.ExtractionText);
		O->SetStringField(TEXT("reply"), Run.ReplyText);
		O->SetNumberField(TEXT("frames_to_answer"), Run.FramesToAnswer);
		O->SetNumberField(TEXT("wall_ms_to_answer"), Run.WallMsToAnswer);
		TArray<TSharedPtr<FJsonValue>> Samples;
		for (double V : Run.GpuBusyMsSamples)
		{
			Samples.Add(MakeShared<FJsonValueNumber>(V));
		}
		O->SetArrayField(TEXT("gpu_busy_ms_samples"), Samples);
		TArray<TSharedPtr<FJsonValue>> Figures;
		for (const FSuperSLMExampleFigure& F : Run.Figures)
		{
			TSharedRef<FJsonObject> FO = MakeShared<FJsonObject>();
			FO->SetStringField(TEXT("label"), F.Label);
			FO->SetStringField(TEXT("value"), F.Value);
			FO->SetStringField(TEXT("source"), F.Source);
			Figures.Add(MakeShared<FJsonValueObject>(FO));
		}
		O->SetArrayField(TEXT("figures"), Figures);
		RunValues.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("runs"), RunValues);
	Root->SetArrayField(TEXT("checks"), CheckValues);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	const FString ReportPath = FPaths::ProjectSavedDir() / TEXT("SuperSLMExample") / TEXT("packaged-check-report.json");
	FFileHelper::SaveStringToFile(Text, *ReportPath);
	UE_LOG(LogSuperSLMExample, Display, TEXT("SuperSLMExample.PackagedCheck: %s; report %s"), bPass ? TEXT("PASS") : TEXT("FAIL"), *FPaths::ConvertRelativePathToFull(ReportPath));

	if (bPackagedCheckExitWhenDone)
	{
		FPlatformMisc::RequestExitWithStatus(false, bPass ? 0 : 1, TEXT("SuperSLMExample.PackagedCheck"));
	}
}
