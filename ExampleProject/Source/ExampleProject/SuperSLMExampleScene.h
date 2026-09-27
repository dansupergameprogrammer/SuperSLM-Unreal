#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"
#include "SuperSLMExampleDemo.h"
#include "SuperSLMExampleScene.generated.h"

class ACameraActor;
class SSuperSLMExampleWidget;
class USuperSLMModel;

// The example scene, authored in code: this actor builds a small lit set, puts the demonstrator's
// panel over the game viewport, loads the example model asset, configures both backends, runs
// the determinism self-check, and then ticks both backends once per frame while the panel drives
// queries. The game mode (SuperSLMExampleGameMode.h) spawns it into whatever map is open, so the
// project needs no map of its own.
//
// Which thread each step runs on:
//   model package load     the engine's async loader; the completion callback on the game thread
//   backend configuration  BeginConfigure(): the game thread captures the model, the plugin's pool
//                          threads map, hash, measure and allocate, and the callback comes back on
//                          the game thread (FSuperSLMExampleDemo::BeginInitialize)
//   self-check             the plugin's stepped run on the game thread, one bounded Step() per
//                          frame, its inference on the plugin's own threads (BeginSelfCheck)
//   GPU reconfiguration    BeginConfigure() again when a control changes the GPU's configuration;
//                          a change of k alone is SetFixedTickLatency(), O(1)
//   queries                the plugin's worker and submission threads; the game thread only
//                          ticks the subsystems (bookkeeping) and reads results
//   detokenizing           the game thread: a lookup on the mapping the CPU backend holds
//   panel, readout, report the game thread
// While a query runs, nothing on the game thread waits for another thread. Reconfiguring does:
// BeginConfigure() first tears down the previous GPU configuration on the game thread: it waits for
// the submission thread to run every job already queued on it and the teardown, then joins it (a
// thread already unresponsive is leaked instead, with no wait and no join; one that stops
// responding during the wait is leaked once its stuck call overruns its time bound).
//
// Command-line switches (a packaged build reads them the same way):
//   -SuperSLMExampleModel=<object path>   load this model asset instead of the default; an object
//                                         path, /Game/Dir/Name.Name, not the package path alone
//   -SuperSLMExampleSkipLoad              load nothing; for the plugin's packaged memory-mapping test,
//                                         which must be the first to load the model package
//   -SuperSLMExamplePackagedCheck         run the packaged check after the self-check, write its report, exit
UCLASS()
class ASuperSLMExampleScene : public AActor
{
	GENERATED_BODY()

public:
	// Where the example model asset is expected: the release's .sslm imported to this path.
	static const TCHAR* GetDefaultModelPath() { return TEXT("/Game/SuperSLMExample/ExampleModel.ExampleModel"); }

	enum class ELoadState : uint8
	{
		Starting,
		LoadingModel,        // async package load
		ConfiguringBackends, // BeginConfigure() pending
		RunningSelfCheck,    // stepped per frame
		Ready,
		Skipped,
		Failed,
	};

	ASuperSLMExampleScene();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	static ASuperSLMExampleScene* Find(UWorld* World);

	ELoadState GetLoadState() const { return LoadState; }
	const FString& GetStatus() const { return Status; }

	FSuperSLMExampleSettings& EditSettings() { return Settings; }
	const FSuperSLMExampleSettings& GetSettings() const { return Settings; }

	// Null until the model is loaded and both backends are configured.
	const FSuperSLMExampleDemo* GetDemo() const { return Demo.Get(); }

	bool CanAsk() const;
	bool Ask(FString& OutError);

	// Re-runs the self-check between queries, stepped a bounded slice per frame; the scene keeps
	// rendering and the panel shows it running until the report comes back.
	bool RequestSelfCheck();

	// The packaged check, at the default settings: the self-check through its console command, then
	// the query on the CPU and on the GPU, each with its output read. Each check it asserts is one
	// clause of what the packaged example must show (see FinishPackagedCheck()). Writes
	// Saved/SuperSLMExample/packaged-check-report.json. With bExitWhenDone the process exits with
	// status 0 when every check passed and 1 otherwise.
	bool StartPackagedCheck(bool bExitWhenDone);

private:
	enum class EPackagedCheckStep : uint8
	{
		Off,
		WaitingForReady,
		SelfCheck,
		Cpu,
		Gpu,
	};

	UPROPERTY(Transient)
	TObjectPtr<USuperSLMModel> Model;

	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> Camera;

	TUniquePtr<FSuperSLMExampleDemo> Demo;
	TSharedPtr<SSuperSLMExampleWidget> Widget;

	FSuperSLMExampleSettings Settings;
	ELoadState LoadState = ELoadState::Starting;
	FString Status;
	FString ModelPath;
	bool bInputConfigured = false;
	bool bSetBuilt = false;
	bool bEnded = false;

	EPackagedCheckStep PackagedCheckStep = EPackagedCheckStep::Off;
	bool bPackagedCheckExitWhenDone = false;
	bool bPackagedCheckSelfCheckCommandRan = false;
	int32 PackagedCheckSelfCheckRunsBefore = 0;
	bool bPackagedCheckSelfCheckReported = false;
	TArray<FSuperSLMExampleReadout> PackagedCheckRuns;

	void BuildSet();
	void ConfigureInput();
	void BeginModelLoad();
	void OnModelPackageLoaded(EAsyncLoadingResult::Type Result);
	void AdvanceLoad();
	void AdvancePackagedCheck();
	void FinishPackagedCheck();
};
