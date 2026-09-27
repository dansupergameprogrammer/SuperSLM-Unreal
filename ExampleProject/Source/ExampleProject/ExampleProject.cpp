#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "SuperSLMExampleScene.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ExampleProject, "ExampleProject");

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMExampleCommands, Log, All);

// Console commands for the example scene. They work in a packaged Development build, from the
// console or from -ExecCmds.
namespace
{
	ASuperSLMExampleScene* SceneOrWarn(UWorld* World)
	{
		ASuperSLMExampleScene* Scene = ASuperSLMExampleScene::Find(World);
		if (Scene == nullptr)
		{
			UE_LOG(LogSuperSLMExampleCommands, Warning, TEXT("No example scene in this world; start the game with the example's game mode."));
		}
		return Scene;
	}

	FAutoConsoleCommandWithWorldAndArgs GAskCommand(
		TEXT("SuperSLMExample.Ask"),
		TEXT("Runs the example query with the panel's current settings. Any arguments replace what the customer says."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (ASuperSLMExampleScene* Scene = SceneOrWarn(World))
			{
				if (Args.Num() > 0)
				{
					Scene->EditSettings().Utterance = FString::Join(Args, TEXT(" "));
				}
				FString Error;
				if (!Scene->Ask(Error))
				{
					UE_LOG(LogSuperSLMExampleCommands, Warning, TEXT("SuperSLMExample.Ask: %s"), *Error);
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GSelfCheckCommand(
		TEXT("SuperSLMExample.SelfCheck"),
		TEXT("Re-runs the determinism self-check on both backends, stepped a bounded slice per frame on the game thread, and logs its report (in 1.0 the GPU verdict is always withheld)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASuperSLMExampleScene* Scene = SceneOrWarn(World))
			{
				if (!Scene->RequestSelfCheck())
				{
					UE_LOG(LogSuperSLMExampleCommands, Warning, TEXT("SuperSLMExample.SelfCheck: the scene is not ready, or a query or backend work is running."));
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GPackagedCheckCommand(
		TEXT("SuperSLMExample.PackagedCheck"),
		TEXT("Runs the packaged check at the default settings (self-check console command, CPU query, GPU query) and writes Saved/SuperSLMExample/packaged-check-report.json. Pass 'exit' to quit with status 0 on pass, 1 on fail."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (ASuperSLMExampleScene* Scene = SceneOrWarn(World))
			{
				Scene->StartPackagedCheck(Args.Contains(TEXT("exit")));
			}
		}));
}
