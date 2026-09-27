// U1 R-S1f: restored and still-restoring CPU handles require reset before schema bind.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMSlotGates.h"
#if SUPERSLM_WITH_L2S1_ASYNC
#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
using namespace SuperSLML2S1Fixtures;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuRestoredSchemaBindRequiresResetTest,
    "SuperSLM.U1.Cpu.RestoredSchemaBindRequiresReset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuRestoredSchemaBindRequiresResetTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper World;
    if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
    FString Path, Reason;
    if (!TestTrue(*Reason, TryGetAExArtifactPath(Path, Reason))) { return false; }
    FSuperSLMImportDiagnostic Diag;
    USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
    if (!TestNotNull(TEXT("A-EX"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
    USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
    if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
    FSuperSLMRuntimeConfig Config;
    Config.BlockCount = 2;
    Config.MaxSequencesPerDecodeCall = 1;
    Config.MaxPrefillChunkBudget = 64;
    Config.MaxLayerBudget = 24;
    Config.SequenceLifecycleBudgetMs = 1000.0;
    Config.TickBudgetMs = 1000.0;
    if (!TestEqual(TEXT("Configure"), (uint8)Cpu->Configure(Model, Config).Result,
            (uint8)ESuperSLMConfigureResult::Success)) { return false; }
    FSuperSLMSequence Source;
    if (!TestEqual(TEXT("source vend"), (uint8)Cpu->VendSequence(Source), (uint8)ESuperSLMVendResult::Success)) { return false; }
    FSuperSLMGenerationRequest Request;
    if (!TestTrue(TEXT("Tokenize"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
    Request.MaxNewTokens = 1;
    TArray<int32> Tokens;
    FString Error;
    if (!TestTrue(*Error, RunGenerationToCompletion(*Cpu, Source, Request, Tokens, 60.0, Error))) { return false; }
    FSuperSLMLifecycleOpHandle SaveHandle;
    if (!TestEqual(TEXT("save queues"), (uint8)Cpu->SaveSequence(Source, SaveHandle, Error),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    TArray<uint8> Blob;
    ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
    DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
        [Cpu, &SaveHandle, &Blob, &SaveResult]() { SaveResult = Cpu->GetSaveResult(SaveHandle, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 60.0);
    if (!TestEqual(TEXT("save succeeds"), (uint8)SaveResult,
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    Cpu->ReturnSequence(Source);
    FSuperSLMSequence Restored;
    FSuperSLMLifecycleOpHandle RestoreHandle;
    if (!TestEqual(TEXT("restore queues"), (uint8)Cpu->RestoreSequence(Blob, Model, Restored, RestoreHandle, Error),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    const FSuperSLMSchemaHandle NoneSchema;
    FString BindError;
    bool bOk = TestFalse(TEXT("schema bind while restore is queued is refused"), Cpu->SetSchema(Restored, NoneSchema, BindError));
    bOk &= TestTrue(TEXT("queued-restore refusal names reset-then-bind route"), BindError.Contains(TEXT("reset"), ESearchCase::IgnoreCase));
    DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
        [Cpu, &RestoreHandle]() { return Cpu->GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Pending; }, 60.0);
    if (!TestEqual(TEXT("restore succeeds"), (uint8)Cpu->GetLifecycleOpResult(RestoreHandle),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    BindError.Empty();
    bOk &= TestFalse(TEXT("schema bind on a restored sequence is refused"), Cpu->SetSchema(Restored, NoneSchema, BindError));
    bOk &= TestTrue(TEXT("restored-state refusal names reset-then-bind route"), BindError.Contains(TEXT("reset"), ESearchCase::IgnoreCase));
    FSuperSLMLifecycleOpHandle ResetHandle;
    if (!TestEqual(TEXT("reset queues"), (uint8)Cpu->ResetSequence(Restored, ResetHandle, Error),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
        [Cpu, &ResetHandle]() { return Cpu->GetLifecycleOpResult(ResetHandle) != ESuperSLMRestoreResult::Pending; }, 60.0);
    if (!TestEqual(TEXT("reset succeeds"), (uint8)Cpu->GetLifecycleOpResult(ResetHandle),
            (uint8)ESuperSLMRestoreResult::Success)) { return false; }
    bOk &= TestTrue(TEXT("schema bind succeeds after reset"), Cpu->SetSchema(Restored, NoneSchema, BindError));
    Cpu->ReturnSequence(Restored);
    return bOk;
}
#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
