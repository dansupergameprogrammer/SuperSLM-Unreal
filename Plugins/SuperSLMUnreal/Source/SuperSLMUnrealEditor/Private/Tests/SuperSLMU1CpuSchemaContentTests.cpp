// R-S1f: the last held schema value governs a queued schema-content generation.
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
    FSuperSLMU1CpuHeldSchemaContentTest,
    "SuperSLM.U1.Cpu.HeldSchemaContent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuHeldSchemaContentTest::RunTest(const FString& Parameters)
{
    // Reads the job ledger by row number across the run, so it keeps the whole run.
    FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
    FTestWorldWrapper W;
    if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
    FString Path, Error;
    if (!TestTrue(*Error, TryGetAExArtifactPath(Path, Error))) { return false; }
    FSuperSLMImportDiagnostic Diag;
    USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
    if (!TestNotNull(TEXT("A-EX"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
    USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
    if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
    FSuperSLMRuntimeConfig Config;
    Config.BlockCount = 1;
    Config.MaxSequencesPerDecodeCall = 1;
    Config.MaxPrefillChunkBudget = 64;
    Config.MaxLayerBudget = 24;
    Config.SequenceLifecycleBudgetMs = 1000.0;
    Config.TickBudgetMs = 1000.0;
    if (!TestEqual(TEXT("Configure"), (uint8)Cpu->Configure(Model, Config).Result,
        (uint8)ESuperSLMConfigureResult::Success)) { return false; }
    FSuperSLMSchemaHandle Schema;
    if (!TestTrue(*Error, FSuperSLMSchemaLookup::LookupByName(*Model, AExSchemaName(), Schema, Error))) { return false; }
    FSuperSLMGenerationRequest Request;
    if (!TestTrue(TEXT("tokenize an opening JSON object"), Cpu->Tokenize(TEXT("{"), Request.PromptTokens)) ||
        !TestTrue(TEXT("schema-content span is nonempty"), !Request.PromptTokens.IsEmpty())) { return false; }
    Request.SpanKind = ESuperSLMSpanKind::SchemaContent;
    Request.MaxNewTokens = 32;
    auto Run = [&](const FSuperSLMSequence& S, TArray<int32>& Tokens)
    {
        FString BeginError;
        if (!Cpu->BeginGeneration(S, Request, BeginError))
        {
            AddError(FString::Printf(TEXT("schema-content BeginGeneration refused: %s"), *BeginError));
            return false;
        }
        DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
            [&]() { const ESuperSLMSequencePhase P = Cpu->GetPhase(S);
                return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted; }, 90.0);
        Tokens = Cpu->GetGeneratedTokens(S);
        return true;
    };

    // Independently runnable control: an inline bind and the same forced span.
    FSuperSLMSequence ReferenceSeq;
    if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(ReferenceSeq), (uint8)ESuperSLMVendResult::Success) ||
        !TestTrue(TEXT("reference inline bind"), Cpu->SetSchema(ReferenceSeq, Schema, Error))) { return false; }
    TArray<int32> Reference;
    if (!Run(ReferenceSeq, Reference) ||
        !TestEqual(TEXT("inline schema-content reference reaches the defined dead end"), (uint8)Cpu->GetPhase(ReferenceSeq),
            (uint8)ESuperSLMSequencePhase::Faulted) ||
        !TestEqual(TEXT("inline schema-content reference reports SchemaDeadEnd"), (uint8)Cpu->GetLastDecodeOutcome(ReferenceSeq),
            (uint8)ESuperSLMDecodeOutcome::SchemaDeadEnd)) { return false; }
    Cpu->ReturnSequence(ReferenceSeq);
    for (int32 I = 0; I < 40; ++I) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.005f); }

    bool bOk = true;
    for (int32 LastHeldIsNone = 0; LastHeldIsNone < 2; ++LastHeldIsNone)
    {
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("held-run vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        FSuperSLMLifecycleOpHandle Reset;
        bOk &= TestEqual(TEXT("reset queued"), (uint8)Cpu->ResetSequence(S, Reset, Error), (uint8)ESuperSLMRestoreResult::Success);
        bOk &= TestTrue(TEXT("held schema bind accepted"), Cpu->SetSchema(S, Schema, Error));
        if (LastHeldIsNone)
        {
            bOk &= TestTrue(TEXT("held none-schema bind accepted"), Cpu->SetSchema(S, FSuperSLMSchemaHandle(), Error));
            AddExpectedError(TEXT("SSLM_SCHEMA_SPAN_UNBOUND"), EAutomationExpectedErrorFlags::Contains, 1);
        }
        const int32 LedgerStart = Cpu->GetJobLedger().Num();
        TArray<int32> Tokens;
        if (!Run(S, Tokens)) { return false; }
        if (LastHeldIsNone)
        {
            bOk &= TestEqual(TEXT("last held none makes the span fault"), (uint8)Cpu->GetPhase(S),
                (uint8)ESuperSLMSequencePhase::Faulted);
            int32 PostedGenerationJobs = 0;
            const TArray<FSuperSLMWorkerJobReport>& Ledger = Cpu->GetJobLedger();
            for (int32 I = LedgerStart; I < Ledger.Num(); ++I)
            {
                PostedGenerationJobs += Ledger[I].Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill ? 1 : 0;
            }
            bOk &= TestEqual(TEXT("unbound forced span faults before a Layer-1 generation job"), PostedGenerationJobs, 0);
        }
        else
        {
            bOk &= TestEqual(TEXT("held schema span reaches the same defined dead end"), (uint8)Cpu->GetPhase(S),
                (uint8)ESuperSLMSequencePhase::Faulted);
            bOk &= TestEqual(TEXT("held schema span reports SchemaDeadEnd"), (uint8)Cpu->GetLastDecodeOutcome(S),
                (uint8)ESuperSLMDecodeOutcome::SchemaDeadEnd);
            bOk &= TestEqual(TEXT("held bind matches the inline-bind reference"), Tokens, Reference);
        }
        Cpu->ReturnSequence(S);
        for (int32 I = 0; I < 40; ++I) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.005f); }
    }
    return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
