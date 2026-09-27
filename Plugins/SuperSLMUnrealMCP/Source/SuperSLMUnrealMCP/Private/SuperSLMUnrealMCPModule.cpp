#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#include "SuperSLMMCPInference.h"
#include "SuperSLMMCPWriteSink.h"
#include "SuperSLMToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

// Registers the SuperSLM toolset with the engine's ToolsetRegistry, from which the MCP plugin's
// adapter discovers it. This module never depends on the MCP
// plugin itself. Registration waits for OnPostEngineInit, because the registry's backing editor
// subsystem does not exist when the module loads.
//
// The toolset's bodies are in SuperSLMToolset.cpp. The action tier's runtime host and the read
// tier's background queue (SuperSLMMCPInference.h), then the writes waiting for an open editor
// transaction to close (SuperSLMMCPWriteSink.h), are shut down at engine pre-exit, while UObjects
// are still alive, and again at module shutdown, where it is a no-op by then.
class FSuperSLMUnrealMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			UToolsetRegistry::RegisterToolsetClass(USuperSLMToolset::StaticClass());
		});
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([]()
		{
			SuperSLMMCPInference::ShutdownHost();
			SuperSLMMCPWriteSink::ShutdownDeferredWrites();
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		if (UObjectInitialized())
		{
			SuperSLMMCPInference::ShutdownHost();
			SuperSLMMCPWriteSink::ShutdownDeferredWrites();
			UToolsetRegistry::UnregisterToolsetClass(USuperSLMToolset::StaticClass());
		}
	}

private:
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle PreExitHandle;
};

IMPLEMENT_MODULE(FSuperSLMUnrealMCPModule, SuperSLMUnrealMCP)
