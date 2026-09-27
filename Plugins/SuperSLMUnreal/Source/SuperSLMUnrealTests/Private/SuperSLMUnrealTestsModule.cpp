#include "Modules/ModuleManager.h"

// T-2447 (D-SLM5340). This module carries no startup or shutdown behaviour: its entire
// content is the automation cells under Private/Tests, which register themselves through
// IMPLEMENT_SIMPLE_AUTOMATION_TEST at static initialisation when the module's binary loads.
// FDefaultModuleImpl supplies the InitializeModule export every UE module must export
// (T-2232's own finding, where an absent export left a module built but never loadable).
IMPLEMENT_MODULE(FDefaultModuleImpl, SuperSLMUnrealTests)
