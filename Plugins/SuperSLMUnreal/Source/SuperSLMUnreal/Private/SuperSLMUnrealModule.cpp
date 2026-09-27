#include "SuperSLMUnrealModule.h"
#include "SuperSLMCalibrateCostsCommand.h"
#include "SuperSLMLog.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMStatusMapping.h"

DEFINE_LOG_CATEGORY(LogSuperSLM);

void FSuperSLMUnrealModule::StartupModule()
{
	// T-2241 review C2 (guard G5): forces SuperSLMStatusMapping.h's switch to be a real,
	// exercised part of this module's startup path -- see that header's own comment.
	SuperSLMStatusMapping::VerifyEveryStatusHasDiagnosticText();

	// §10.2 item 10: forces SuperSLMCalibrateCostsCommand.cpp's static FAutoConsoleCommand to
	// link (an otherwise-unreferenced translation unit is a candidate for the linker to strip).
	SuperSLMCalibrateCostsCommand::Register();
}

void FSuperSLMUnrealModule::ShutdownModule()
{
	// Adapters first (each holds a reference on its base model's mapping), then any mapping
	// still held. Subsystems have released their own references in Deinitialize() by now.
	SuperSLMRuntime::ShutdownRegistry();
}

IMPLEMENT_MODULE(FSuperSLMUnrealModule, SuperSLMUnreal)
