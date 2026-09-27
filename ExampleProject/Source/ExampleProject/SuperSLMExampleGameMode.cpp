#include "SuperSLMExampleGameMode.h"

#include "Engine/World.h"
#include "SuperSLMExampleScene.h"

ASuperSLMExampleGameMode::ASuperSLMExampleGameMode()
{
	DefaultPawnClass = nullptr;
}

void ASuperSLMExampleGameMode::StartPlay()
{
	Super::StartPlay();

	if (ASuperSLMExampleScene::Find(GetWorld()) == nullptr)
	{
		GetWorld()->SpawnActor<ASuperSLMExampleScene>();
	}
}
