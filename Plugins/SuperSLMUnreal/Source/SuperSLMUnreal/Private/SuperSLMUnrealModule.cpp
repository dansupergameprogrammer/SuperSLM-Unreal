#include "SuperSLMUnrealModule.h"
#include "SuperSLMCalibrateCostsCommand.h"
#include "SuperSLMEstimateCostsCommand.h"
#include "SuperSLMLog.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMStatusMapping.h"

DEFINE_LOG_CATEGORY(LogSuperSLM);

void FSuperSLMUnrealModule::StartupModule()
{
	// T-2241 review C2 (guard G5): forces SuperSLMStatusMapping.h's switch to be a real,
	// exercised part of this module's startup path -- see that header's own comment.
	SuperSLMStatusMapping::VerifyEveryStatusHasDiagnosticText();

	// Retain the legacy regression instrument without registering its retired public commands.
	SuperSLMCalibrateCostsCommand::Register();
	SuperSLMEstimateCostsCommand::Register();
}

void FSuperSLMUnrealModule::ShutdownModule()
{
	// Adapters first (each holds a reference on its base model's mapping), then any mapping
	// still held. Subsystems have released their own references in Deinitialize() by now.
	SuperSLMRuntime::ShutdownRegistry();
}

IMPLEMENT_MODULE(FSuperSLMUnrealModule, SuperSLMUnreal)
