#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SuperSLMExampleGameMode.generated.h"

// The project's default game mode (Config/DefaultEngine.ini). It spawns no pawn and places the
// example scene (SuperSLMExampleScene.h) into whichever map is open, so the example runs on the
// engine's own empty map and the repository carries no map binary.
UCLASS()
class ASuperSLMExampleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASuperSLMExampleGameMode();

	virtual void StartPlay() override;
};
