// R-S1l(b): a slot returned with an adapter bound and a partial token must unbind on recycle.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMSlotGates.h"
#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuMidTokenAdapterHandoffTest,
    "SuperSLM.U1.Cpu.MidTokenAdapterHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuMidTokenAdapterHandoffTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper W;
    if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
    FSuperSLMImportDiagnostic Diag;
    USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
    if (!TestNotNull(TEXT("A-AD base"), Base) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
    FSuperSLMAdapterHandle Adapter;
    FString Error;
    if (!TestTrue(*Error, FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, Error))) { return false; }
    USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
    if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
    FSuperSLMRuntimeConfig Config;
    Config.BlockCount = 1;
    Config.MaxSequencesPerDecodeCall = 1;
    Config.MaxPrefillChunkBudget = 64;
    Config.MaxLayerBudget = 28;
    Config.SequenceLifecycleBudgetMs = 1000.0;
    Config.TickBudgetMs = 1000.0;
    if (!TestEqual(TEXT("Configure"), (uint8)Cpu->Configure(Base, Config).Result,
        (uint8)ESuperSLMConfigureResult::Success)) { return false; }
    TArray<FReferenceCase> Cases;
    if (!TestTrue(*Error, LoadReferenceCases(Cases, Error)) ||
        !TestTrue(TEXT("A-AD reference case 0 exists"), Cases.Num() > 0)) { return false; }
    FSuperSLMGenerationRequest Request;
    Request.PromptTokens = Cases[0].PromptTokens;
    Request.MaxNewTokens = 32;
    auto DrainRecycle = [&]()
    {
        for (int32 I = 0; I < 40; ++I) { Cpu->Tick(1.0f / 60.0f); FPlatformProcess::Sleep(0.005f); }
    };
    auto RunToComplete = [&](const FSuperSLMSequence& S, TArray<int32>& Tokens)
    {
        FString BeginError;
        if (!Cpu->BeginGeneration(S, Request, BeginError))
        {
            AddError(FString::Printf(TEXT("BeginGeneration refused: %s"), *BeginError));
            return false;
        }
        DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
            [&]() { const ESuperSLMSequencePhase P = Cpu->GetPhase(S);
                return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted; }, 90.0);
        Tokens = Cpu->GetGeneratedTokens(S);
        return Cpu->GetPhase(S) == ESuperSLMSequencePhase::Complete;
    };
    TArray<int32> BaseReference, AdapterReference;
    for (int32 Bind = 0; Bind < 2; ++Bind)
    {
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (Bind) { Cpu->RequestAdapterSwap(S, Adapter); }
        if (!TestTrue(TEXT("reference completes"), RunToComplete(S, Bind ? AdapterReference : BaseReference))) { return false; }
        Cpu->ReturnSequence(S);
        DrainRecycle();
    }
    if (!TestNotEqual(TEXT("case 0 distinguishes base from the adapter"), BaseReference, AdapterReference)) { return false; }

    bool bOk = true;
    for (int32 NewHolderRequestsAdapter = 0; NewHolderRequestsAdapter < 2; ++NewHolderRequestsAdapter)
    {
        FSuperSLMSequence Old;
        if (!TestEqual(TEXT("old holder vend"), (uint8)Cpu->VendSequence(Old), (uint8)ESuperSLMVendResult::Success)) { return false; }
        Cpu->RequestAdapterSwap(Old, Adapter);
        Cpu->SetLayerBudget(Old, 1);
        if (!TestTrue(TEXT("old holder adapter active"), Cpu->GetActiveAdapter(Old).IsValid())) { return false; }
        if (!TestTrue(TEXT("old generation begins"), Cpu->BeginGeneration(Old, Request, Error))) { return false; }
        int32 FirstTokenTick = INDEX_NONE;
        bool bDeliveredMidTokenLayer = false;
        const double Deadline = FPlatformTime::Seconds() + 90.0;
        while (FPlatformTime::Seconds() < Deadline && !bDeliveredMidTokenLayer)
        {
            Cpu->Tick(1.0f / 60.0f);
            if (FirstTokenTick == INDEX_NONE && Cpu->GetGeneratedTokens(Old).Num() > 0)
            {
                FirstTokenTick = Cpu->GetLastTickReport().TickIndex;
            }
            if (FirstTokenTick != INDEX_NONE)
            {
                for (const FSuperSLMWorkerJobReport& Job : Cpu->GetJobLedger())
                {
                    if (Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill &&
                        Job.PlannedAtTick >= FirstTokenTick && Job.DeliveredAtTick >= 0 &&
                        Job.DecodeLayers == 1 && Job.TokenFinishes == 0)
                    {
                        bDeliveredMidTokenLayer = true;
                        break;
                    }
                }
            }
            if (!bDeliveredMidTokenLayer) { FPlatformProcess::Sleep(0.001f); }
        }
        if (!TestTrue(TEXT("the old holder returned after a delivered one-layer mid-token job"),
            bDeliveredMidTokenLayer && Cpu->GetPhase(Old) == ESuperSLMSequencePhase::Decoding)) { return false; }
        Cpu->ReturnSequence(Old); // recycle queued; no intervening Tick
        FSuperSLMSequence New;
        if (!TestEqual(TEXT("new holder vends the same slot"), (uint8)Cpu->VendSequence(New),
            (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (NewHolderRequestsAdapter) { Cpu->RequestAdapterSwap(New, Adapter); }
        TArray<int32> Tokens;
        bOk &= TestTrue(TEXT("new holder completes"), RunToComplete(New, Tokens));
        bOk &= TestEqual(TEXT("new holder sees only its requested adapter state"), Tokens,
            NewHolderRequestsAdapter ? AdapterReference : BaseReference);
        Cpu->ReturnSequence(New);
        DrainRecycle();
        FString ReleaseError;
        bOk &= TestTrue(TEXT("adapter import releases after the new holder drains"),
            FSuperSLMAdapterImport::Release(Adapter, ReleaseError));
        if (NewHolderRequestsAdapter == 0 && bOk)
        {
            bOk &= TestTrue(TEXT("adapter reimports for the Y arm"),
                FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, ReleaseError));
        }
    }
    return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
