// U1 R-S1g: the game thread's Plan+Apply cost while a real 1.5B restore is dispatched.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuRestoreTickBoundTest,
    "SuperSLM.U1.Cpu.RestoreTickBound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuRestoreTickBoundTest::RunTest(const FString& Parameters)
{
    // Reads the tick history by row number across the run, so it keeps the whole run.
    FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
    FTestWorldWrapper World;
    if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
    FSuperSLMImportDiagnostic Diag;
    USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
    if (!TestNotNull(TEXT("A-AD 1.5B import"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
    USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
    if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
    FSuperSLMRuntimeConfig Config;
    Config.BlockCount = 1;
    Config.MaxSequencesPerDecodeCall = 1;
    Config.MaxPrefillChunkBudget = 64;
    Config.MaxLayerBudget = 28;
    Config.SequenceLifecycleBudgetMs = 1000.0;
    Config.TickBudgetMs = 16.6;
    if (!TestEqual(TEXT("Configure"), (uint8)Cpu->Configure(Model, Config).Result,
            (uint8)ESuperSLMConfigureResult::Success)) { return false; }
    FSuperSLMSequence Seq;
    if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
    FSuperSLMGenerationRequest Request;
    Request.PromptTokens = {1, 2, 3};
    Request.MaxNewTokens = 1;
    TArray<int32> Tokens;
    FString Error;
    if (!TestTrue(*Error, RunGenerationToCompletion(*Cpu, Seq, Request, Tokens, 120.0, Error))) { return false; }
    FSuperSLMLifecycleOpHandle SaveHandle;
    if (!TestEqual(TEXT("save queues"), (uint8)Cpu->SaveSequence(Seq, SaveHandle, Error),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    TArray<uint8> Blob;
    ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
    DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::RealTime,
        [Cpu, &SaveHandle, &Blob, &SaveResult]() { SaveResult = Cpu->GetSaveResult(SaveHandle, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 120.0);
    if (!TestEqual(TEXT("save completes"), (uint8)SaveResult,
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    Cpu->ReturnSequence(Seq);
    for (int32 I = 0; I < 8; ++I) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(1.0f / 60.0f); }

    FSuperSLMSequence Restored;
    FSuperSLMLifecycleOpHandle RestoreHandle;
    const int32 HistoryStart = Cpu->GetTickHistory().Num();
    if (!TestEqual(TEXT("restore queues"), (uint8)Cpu->RestoreSequence(Blob, Model, Restored, RestoreHandle, Error),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    const double Deadline = FPlatformTime::Seconds() + 120.0;
    while (Cpu->GetLifecycleOpResult(RestoreHandle) == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() < Deadline)
    {
        Cpu->Tick(1.0f / 60.0f);
        FPlatformProcess::Sleep(1.0f / 60.0f);
    }
    if (!TestEqual(TEXT("restore completes"), (uint8)Cpu->GetLifecycleOpResult(RestoreHandle),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    const TArray<FSuperSLMTickReport>& History = Cpu->GetTickHistory();
    bool bOk = TestTrue(TEXT("restore dispatch produced tick reports"), History.Num() > HistoryStart);
    double Worst = 0.0;
    for (int32 I = HistoryStart; I < History.Num(); ++I)
    {
        const FSuperSLMTickReport& Tick = History[I];
        Worst = FMath::Max(Worst, Tick.PlanMs + Tick.ApplyMs);
        bOk &= TestTrue(*FString::Printf(TEXT("restore tick %d Plan+Apply %.6f ms stays below 1 ms"), Tick.TickIndex, Tick.PlanMs + Tick.ApplyMs),
            Tick.PlanMs + Tick.ApplyMs < 1.0);
    }
    AddInfo(FString::Printf(TEXT("1.5B restore: %d game-thread ticks, worst Plan+Apply %.6f ms"), History.Num() - HistoryStart, Worst));
    Cpu->ReturnSequence(Restored);
    return bOk;
}
#endif // WITH_DEV_AUTOMATION_TESTS
