#pragma once

#include "CoreMinimal.h"

// Decode-threading D4 (plan §10.3.1 item 5b): the "SuperSLM.EstimateCosts" console command. See
// SuperSLMEstimateCostsCommand.cpp. Registration happens at static-init time (FAutoConsoleCommand);
// Register() exists only so the module can force the implementation file to link.
namespace SuperSLMEstimateCostsCommand
{
	void Register();
}
