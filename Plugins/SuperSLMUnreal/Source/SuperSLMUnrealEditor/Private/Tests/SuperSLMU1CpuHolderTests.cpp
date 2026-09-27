// U1 R-S1l holder hand-off cells derived from T-2982 H1/H2 probes.
// The live build is tested directly; there is no model-switch console variable.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Containers/StringConv.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace T2982
{
	FString Toks(const TArray<int32>& T)
	{
		FString S;
		for (int32 V : T) { S += FString::Printf(TEXT("%d "), V); }
		return S;
	}

	int32 FirstDiff(const TArray<int32>& A, const TArray<int32>& B)
	{
		const int32 N = FMath::Min(A.Num(), B.Num());
		for (int32 I = 0; I < N; ++I) { if (A[I] != B[I]) { return I; } }
		return A.Num() == B.Num() ? -1 : N;
	}

	void Drain(USuperSLMSubsystem& Sub, int32 Ticks)
	{
		for (int32 I = 0; I < Ticks; ++I)
		{
			Sub.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.005f);
		}
	}

	// Runs to Complete or Faulted and returns whatever was generated either way.
	bool Run(USuperSLMSubsystem& Sub, const FSuperSLMSequence& Seq, const FSuperSLMGenerationRequest& Req, TArray<int32>& Out, FString& Why)
	{
		FString Err;
		if (!Sub.BeginGeneration(Seq, Req, Err)) { Why = Err; return false; }
		DrainTicks(Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&Sub, &Seq]()
		{
			const ESuperSLMSequencePhase P = Sub.GetPhase(Seq);
			return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted;
		}, 90.0);
		Out = Sub.GetGeneratedTokens(Seq);
		const ESuperSLMSequencePhase P = Sub.GetPhase(Seq);
		Why = P == ESuperSLMSequencePhase::Faulted ? TEXT("faulted") : (P == ESuperSLMSequencePhase::Complete ? TEXT("complete") : TEXT("did not finish"));
		return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted;
	}

	bool SaveBlob(FAutomationTestBase& T, USuperSLMSubsystem& Sub, const FSuperSLMGenerationRequest& Req, TArray<uint8>& Blob)
	{
		FSuperSLMSequence Seq;
		Sub.VendSequence(Seq);
		FString Err;
		Sub.BeginGeneration(Seq, Req, Err);
		DrainTicks(Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&Sub, &Seq]() { return Sub.GetPhase(Seq) != ESuperSLMSequencePhase::Prefilling; }, 60.0);
		FSuperSLMLifecycleOpHandle H;
		Sub.SaveSequence(Seq, H, Err);
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		DrainTicks(Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&Sub, &H, &Blob, &SaveResult]() { SaveResult = Sub.GetSaveResult(H, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 60.0);
		const bool bOk = T.TestEqual(TEXT("save"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success);
		Sub.ReturnSequence(Seq);
		Drain(Sub, 40);
		return bOk;
	}

	// Produces the hand-off: H1 = a previous user generates and returns; H2 = a restore refused for a
	// model mismatch. Either way, on return the slot is free with its recycle Reset queued and not drained.
	bool HandOff(FAutomationTestBase& T, USuperSLMSubsystem& Sub, int32 Path, const FSuperSLMGenerationRequest& PrevReq,
		const TArray<uint8>& Blob, USuperSLMModel* WrongModel, const FString& Arm)
	{
		if (Path == 1)
		{
			FSuperSLMSequence A;
			if (Sub.VendSequence(A) != ESuperSLMVendResult::Success) { T.AddError(Arm + TEXT(" user A vend failed")); return false; }
			TArray<int32> ATokens;
			FString Why;
			Run(Sub, A, PrevReq, ATokens, Why);
			Sub.ReturnSequence(A); // no drain: the recycle is queued behind this line
			return true;
		}
		FSuperSLMSequence R;
		FSuperSLMLifecycleOpHandle H;
		FString Err;
		const ESuperSLMRestoreResult Q = Sub.RestoreSequence(Blob, WrongModel, R, H, Err);
		T.TestEqual(*(Arm + TEXT(" restore queues")), (uint8)Q, (uint8)ESuperSLMRestoreResult::Success);
		DrainTicks(Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&Sub, &H]() { return Sub.GetLifecycleOpResult(H) != ESuperSLMRestoreResult::Pending; }, 60.0);
		const ESuperSLMRestoreResult Res = Sub.GetLifecycleOpResult(H);
		return T.TestEqual(*(Arm + TEXT(" restore refused ModelMismatch")), (uint8)Res, (uint8)ESuperSLMRestoreResult::ModelMismatch);
	}
}


// ---------------------------------------------------------------------------------------------
// Schema face (A-EX, potion_shop_order).
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1NewHolderSchemaBindTest,
	"SuperSLM.U1.Cpu.NewHolderSchemaBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1NewHolderSchemaBindTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FString AExPath, Reason;
	if (!TestTrue(*Reason, TryGetAExArtifactPath(AExPath, Reason))) { return false; }
	FSuperSLMImportDiagnostic D1, D2;
	USuperSLMModel* AEx = FSuperSLMModelImport::ImportFromFile(AExPath, D1);
	if (!TestNotNull(TEXT("A-EX"), AEx)) { return false; }
	USuperSLMModel* Wrong = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), D2);
	if (!TestNotNull(TEXT("A-AD base (the mismatched expected model)"), Wrong)) { return false; }

	USuperSLMSubsystem* Sub = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), Sub)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1; // every vend and every restore uses the one slot
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	const FSuperSLMConfigureReport Rep = Sub->Configure(AEx, Config);
	if (!TestEqual(*Rep.Message, (uint8)Rep.Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }

	FSuperSLMSchemaHandle Schema;
	FString SErr;
	if (!TestTrue(*SErr, FSuperSLMSchemaLookup::LookupByName(*AEx, AExSchemaName(), Schema, SErr))) { return false; }

	FSuperSLMGenerationRequest Req;
	if (!TestTrue(TEXT("Tokenize"), Sub->Tokenize(TEXT("A customer walks up to the counter."), Req.PromptTokens))) { return false; }
	Req.MaxNewTokens = 16;

	// References, each on a fully drained slot. Unconstrained, then constrained with the bind applied
	// directly (queue empty, nothing in flight: SetSchema() calls Layer 1 at once).
	TArray<int32> RefUnc, RefCon;
	FString Why;
	{
		FSuperSLMSequence S;
		Sub->VendSequence(S);
		TestTrue(*Why, T2982::Run(*Sub, S, Req, RefUnc, Why));
		Sub->ReturnSequence(S);
		T2982::Drain(*Sub, 40);
	}
	{
		FSuperSLMSequence S;
		Sub->VendSequence(S);
		FString BErr;
		TestTrue(*(TEXT("reference SetSchema: ") + BErr), Sub->SetSchema(S, Schema, BErr));
		TestTrue(*Why, T2982::Run(*Sub, S, Req, RefCon, Why));
		Sub->ReturnSequence(S);
		T2982::Drain(*Sub, 40);
	}
	AddInfo(FString::Printf(TEXT("T2982 schema REF unconstrained: %s"), *T2982::Toks(RefUnc)));
	AddInfo(FString::Printf(TEXT("T2982 schema REF constrained  : %s"), *T2982::Toks(RefCon)));
	if (!TestNotEqual(TEXT("references must differ (discrimination), or no token clause can fail"), RefUnc, RefCon)) { return false; }

	TArray<uint8> Blob;
	if (!T2982::SaveBlob(*this, *Sub, Req, Blob)) { return false; }

	bool bAll = true;
		for (int32 Path = 1; Path <= 2; ++Path)
		for (int32 bRequest = 0; bRequest <= 1; ++bRequest)
		{
			const FString Arm = FString::Printf(TEXT("[schema, H%d %s, %s]"), Path, Path == 1 ? TEXT("ReturnSequence") : TEXT("refused restore"), bRequest ? TEXT("Y") : TEXT("N"));
			if (!T2982::HandOff(*this, *Sub, Path, Req, Blob, Wrong, Arm)) { bAll = false; continue; }

			// User B: the slot's next holder, binding its own schema.
			FSuperSLMSequence B;
			if (!TestEqual(*(Arm + TEXT(" user B vends the one slot")), (uint8)Sub->VendSequence(B), (uint8)ESuperSLMVendResult::Success)) { bAll = false; continue; }
			FString BErr;
			const bool bBindAccepted = !bRequest || Sub->SetSchema(B, Schema, BErr);
			TArray<int32> BTokens;
			const bool bRan = T2982::Run(*Sub, B, Req, BTokens, Why);
			AddInfo(FString::Printf(TEXT("T2982 %s user B: SetSchema returned %d ('%s'); run %s; tokens %s; equals CONSTRAINED %d, equals UNCONSTRAINED %d, first diff vs constrained %d"),
				*Arm, bBindAccepted ? 1 : 0, *BErr, *Why, *T2982::Toks(BTokens), BTokens == RefCon ? 1 : 0, BTokens == RefUnc ? 1 : 0, T2982::FirstDiff(BTokens, RefCon)));
			bAll &= TestTrue(*(Arm + TEXT(" user B's SetSchema is accepted")), bBindAccepted);
			bAll &= TestTrue(*(Arm + TEXT(" user B's generation runs")), bRan);
			bAll &= TestEqual(*(Arm + TEXT(" user B decodes under its own requested schema state")), BTokens, bRequest ? RefCon : RefUnc);
			Sub->ReturnSequence(B);
			T2982::Drain(*Sub, 40);
		}
	return bAll;
}

// ---------------------------------------------------------------------------------------------
// Adapter face (A-AD 1.5B base + shopkeeper adapter).
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1NewHolderAdapterRequestTest,
	"SuperSLM.U1.Cpu.NewHolderAdapterRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1NewHolderAdapterRequestTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic D1, D2;
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), D1);
	if (!TestNotNull(TEXT("A-AD base"), Base)) { return false; }
	FString AExPath, Reason;
	if (!TestTrue(*Reason, TryGetAExArtifactPath(AExPath, Reason))) { return false; }
	USuperSLMModel* Wrong = FSuperSLMModelImport::ImportFromFile(AExPath, D2);
	if (!TestNotNull(TEXT("A-EX (the mismatched expected model)"), Wrong)) { return false; }
	FSuperSLMAdapterHandle Adapter;
	FString AErr;
	if (!TestTrue(*AErr, FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, AErr))) { return false; }

	USuperSLMSubsystem* Sub = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), Sub)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 28;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0;
	const FSuperSLMConfigureReport Rep = Sub->Configure(Base, Config);
	if (!TestEqual(*Rep.Message, (uint8)Rep.Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }

	TArray<FReferenceCase> Cases;
	FString LErr;
	if (!TestTrue(*LErr, LoadReferenceCases(Cases, LErr))) { return false; }
	FSuperSLMGenerationRequest Req;
	Req.MaxNewTokens = 32;

	// References on a drained slot: base, and the adapter requested on an idle, empty-queued sequence
	// (applied at once, RequestAdapterSwap's own immediate branch). First recorded prompt that separates them.
	TArray<int32> RefBase, RefAd;
	int32 Chosen = INDEX_NONE;
	FString Why;
	for (int32 C = 0; C < Cases.Num() && Chosen == INDEX_NONE; ++C)
	{
		Req.PromptTokens = Cases[C].PromptTokens;
		{
			FSuperSLMSequence S;
			Sub->VendSequence(S);
			T2982::Run(*Sub, S, Req, RefBase, Why);
			Sub->ReturnSequence(S);
			T2982::Drain(*Sub, 40);
		}
		{
			FSuperSLMSequence S;
			Sub->VendSequence(S);
			Sub->RequestAdapterSwap(S, Adapter);
			TestTrue(TEXT("reference adapter bound at once"), Sub->GetActiveAdapter(S).IsValid());
			T2982::Run(*Sub, S, Req, RefAd, Why);
			Sub->ReturnSequence(S);
			T2982::Drain(*Sub, 40);
		}
		if (RefBase != RefAd) { Chosen = C; }
	}
	AddInfo(FString::Printf(TEXT("T2982 adapter discriminating case %d; REF base: %s"), Chosen, *T2982::Toks(RefBase)));
	AddInfo(FString::Printf(TEXT("T2982 adapter REF adapter: %s"), *T2982::Toks(RefAd)));
	if (!TestTrue(TEXT("a recorded prompt separates base from adapter (discrimination)"), Chosen != INDEX_NONE)) { return false; }

	TArray<uint8> Blob;
	if (!T2982::SaveBlob(*this, *Sub, Req, Blob)) { return false; }

	bool bAll = true;
		for (int32 Path = 1; Path <= 2; ++Path)
		for (int32 bRequest = 0; bRequest <= 1; ++bRequest)
		{
			const FString Arm = FString::Printf(TEXT("[adapter, H%d %s, %s]"), Path, Path == 1 ? TEXT("ReturnSequence") : TEXT("refused restore"), bRequest ? TEXT("Y") : TEXT("N"));
			if (!T2982::HandOff(*this, *Sub, Path, Req, Blob, Wrong, Arm)) { bAll = false; continue; }

			FSuperSLMSequence B;
			if (!TestEqual(*(Arm + TEXT(" user B vends the one slot")), (uint8)Sub->VendSequence(B), (uint8)ESuperSLMVendResult::Success)) { bAll = false; continue; }
			if (bRequest) { Sub->RequestAdapterSwap(B, Adapter); }
			TArray<int32> BTokens;
			const bool bRan = T2982::Run(*Sub, B, Req, BTokens, Why);
			const bool bActiveAfter = Sub->GetActiveAdapter(B).IsValid();
			AddInfo(FString::Printf(TEXT("T2982 %s user B: run %s; adapter active after run %d; tokens %s; equals ADAPTER %d, equals BASE %d, first diff vs adapter %d"),
				*Arm, *Why, bActiveAfter ? 1 : 0, *T2982::Toks(BTokens), BTokens == RefAd ? 1 : 0, BTokens == RefBase ? 1 : 0, T2982::FirstDiff(BTokens, RefAd)));
			bAll &= TestTrue(*(Arm + TEXT(" user B's generation runs")), bRan);
			bAll &= TestEqual(*(Arm + TEXT(" user B decodes under its own requested adapter state")), BTokens, bRequest ? RefAd : RefBase);
			Sub->ReturnSequence(B);
			T2982::Drain(*Sub, 40);
		}
	return bAll;
}


// R-S1c: a save reads binding state at its own worker turn. The two request
// orders have different intended blobs even though both use a queued recycle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuHeldSchemaSaveOrderTest,
    "SuperSLM.U1.Cpu.HeldSchemaSaveOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuHeldSchemaSaveOrderTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper W;
    if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
    FString Path, Reason;
    if (!TestTrue(*Reason, TryGetAExArtifactPath(Path, Reason))) { return false; }
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
    FString Error;
    if (!TestTrue(*Error, FSuperSLMSchemaLookup::LookupByName(*Model, AExSchemaName(), Schema, Error))) { return false; }
    FSuperSLMGenerationRequest Request;
    if (!TestTrue(TEXT("Tokenize"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
    Request.MaxNewTokens = 16;

    TArray<int32> Unconstrained, Constrained;
    for (int32 Bind = 0; Bind <= 1; ++Bind)
    {
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (Bind && !TestTrue(*Error, Cpu->SetSchema(S, Schema, Error))) { return false; }
        TArray<int32>& Out = Bind ? Constrained : Unconstrained;
        if (!TestTrue(*Error, T2982::Run(*Cpu, S, Request, Out, Error))) { return false; }
        Cpu->ReturnSequence(S);
        T2982::Drain(*Cpu, 40);
    }
    if (!TestNotEqual(TEXT("binding must change the reference"), Constrained, Unconstrained)) { return false; }

    bool bOk = true;
    for (int32 BindBeforeSave = 0; BindBeforeSave <= 1; ++BindBeforeSave)
    {
        FSuperSLMSequence Prior;
        if (!TestEqual(TEXT("prior vend"), (uint8)Cpu->VendSequence(Prior), (uint8)ESuperSLMVendResult::Success)) { return false; }
        Cpu->ReturnSequence(Prior); // queue a recycle; no Tick before the next holder's requests
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("new holder vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        // Keep an owner reset ahead of the save. The bind below must be carried by
        // the save itself, so its post-time schema mirror is not the save's schema.
        FSuperSLMLifecycleOpHandle ResetHandle;
        if (!TestEqual(TEXT("owner reset queues ahead of held bind"),
                (uint8)Cpu->ResetSequence(S, ResetHandle, Error),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
        if (BindBeforeSave && !TestTrue(TEXT("held SetSchema before save accepted"), Cpu->SetSchema(S, Schema, Error))) { return false; }
        FSuperSLMLifecycleOpHandle SaveHandle;
        if (!TestEqual(TEXT("save queues"), (uint8)Cpu->SaveSequence(S, SaveHandle, Error),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
        if (!BindBeforeSave && !TestTrue(TEXT("SetSchema after queued save accepted"), Cpu->SetSchema(S, Schema, Error))) { return false; }
        TArray<uint8> Blob;
        ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
        DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
            [Cpu, &SaveHandle, &Blob, &SaveResult]() { SaveResult = Cpu->GetSaveResult(SaveHandle, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 60.0);
        if (!TestEqual(TEXT("save resolves"), (uint8)SaveResult,
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
        int32 Layer1Offset = INDEX_NONE;
        for (int32 I = 0; I + 3 < Blob.Num(); ++I)
        {
            if (Blob[I] == 'S' && Blob[I + 1] == 'S' && Blob[I + 2] == 'B' && Blob[I + 3] == '5')
            { Layer1Offset = I; break; }
        }
        if (!TestTrue(TEXT("save contains Layer-1 payload"), Layer1Offset != INDEX_NONE)) { return false; }
        const FTCHARToUTF8 SchemaUtf8(AExSchemaName());
        bool bWrapperNamesSchema = false;
        for (int32 I = 0; I + SchemaUtf8.Length() <= Layer1Offset; ++I)
        {
            if (FMemory::Memcmp(Blob.GetData() + I, SchemaUtf8.Get(), SchemaUtf8.Length()) == 0)
            { bWrapperNamesSchema = true; break; }
        }
        bOk &= TestEqual(TEXT("save wrapper names the schema bound at the save ordinal"),
            bWrapperNamesSchema, BindBeforeSave != 0);
        Cpu->ReturnSequence(S);
        T2982::Drain(*Cpu, 40);
        FSuperSLMSequence Restored;
        FSuperSLMLifecycleOpHandle RestoreHandle;
        if (!TestEqual(TEXT("restore queues"), (uint8)Cpu->RestoreSequence(Blob, Model, Restored, RestoreHandle, Error),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
        DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
            [Cpu, &RestoreHandle]() { return Cpu->GetLifecycleOpResult(RestoreHandle) != ESuperSLMRestoreResult::Pending; }, 60.0);
        if (!TestEqual(TEXT("restore resolves"), (uint8)Cpu->GetLifecycleOpResult(RestoreHandle),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
        TArray<int32> Tokens;
        bOk &= TestTrue(TEXT("restored generation runs"), T2982::Run(*Cpu, Restored, Request, Tokens, Error));
        bOk &= TestEqual(BindBeforeSave ? TEXT("bind-before-save blob is constrained") : TEXT("save-before-bind blob is unconstrained"),
            Tokens, BindBeforeSave ? Constrained : Unconstrained);
        Cpu->ReturnSequence(Restored);
        T2982::Drain(*Cpu, 40);
    }
    return bOk;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuInlineAfterHeldSchemaTest,
    "SuperSLM.U1.Cpu.InlineAfterHeldSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuInlineAfterHeldSchemaTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper W;
    if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
    FString Path, Reason;
    if (!TestTrue(*Reason, TryGetAExArtifactPath(Path, Reason))) { return false; }
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
    FString Error;
    if (!TestTrue(*Error, FSuperSLMSchemaLookup::LookupByName(*Model, AExSchemaName(), Schema, Error))) { return false; }
    FSuperSLMGenerationRequest Request;
    if (!TestTrue(TEXT("Tokenize"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
    Request.MaxNewTokens = 16;
    FSuperSLMSequence ReferenceSeq;
    if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(ReferenceSeq), (uint8)ESuperSLMVendResult::Success)) { return false; }
    TArray<int32> Reference;
    if (!TestTrue(*Error, T2982::Run(*Cpu, ReferenceSeq, Request, Reference, Error))) { return false; }
    Cpu->ReturnSequence(ReferenceSeq);
    T2982::Drain(*Cpu, 40);

    FSuperSLMSequence Prior;
    if (!TestEqual(TEXT("prior vend"), (uint8)Cpu->VendSequence(Prior), (uint8)ESuperSLMVendResult::Success)) { return false; }
    Cpu->ReturnSequence(Prior); // queued recycle: the incoming holder's first bind is held
    FSuperSLMSequence Incoming;
    if (!TestEqual(TEXT("incoming vend"), (uint8)Cpu->VendSequence(Incoming), (uint8)ESuperSLMVendResult::Success)) { return false; }
    if (!TestTrue(TEXT("held schema bind accepted"), Cpu->SetSchema(Incoming, Schema, Error))) { return false; }
    T2982::Drain(*Cpu, 40); // recycle drains, but no incoming-holder job has consumed the held bind
    const FSuperSLMSchemaHandle NoneSchema;
    if (!TestTrue(TEXT("later none-schema bind accepted"), Cpu->SetSchema(Incoming, NoneSchema, Error))) { return false; }
    TArray<int32> Tokens;
    const bool bRan = T2982::Run(*Cpu, Incoming, Request, Tokens, Error);
    Cpu->ReturnSequence(Incoming);
    bool bOk = TestTrue(TEXT("generation after the two binds runs"), bRan);
    bOk &= TestEqual(TEXT("the last schema request wins before generation"), Tokens, Reference);
    return bOk;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuHeldAdapterPinLifecycleTest,
    "SuperSLM.U1.Cpu.HeldAdapterPinLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuHeldAdapterPinLifecycleTest::RunTest(const FString& Parameters)
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
    if (!TestTrue(*Error, LoadReferenceCases(Cases, Error))) { return false; }
    FSuperSLMGenerationRequest Request;
    Request.MaxNewTokens = 32;
    TArray<int32> BaseTokens, AdapterTokens;
    int32 DistinguishingCase = INDEX_NONE;
    for (int32 C = 0; C < Cases.Num() && DistinguishingCase == INDEX_NONE; ++C)
    {
        Request.PromptTokens = Cases[C].PromptTokens;
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("base reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (!TestTrue(*Error, T2982::Run(*Cpu, S, Request, BaseTokens, Error)) ||
            !TestEqual(TEXT("base reference completes"), (uint8)Cpu->GetPhase(S), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
        Cpu->ReturnSequence(S);
        T2982::Drain(*Cpu, 40);
        if (!TestEqual(TEXT("adapter reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        Cpu->RequestAdapterSwap(S, Adapter);
        if (!TestTrue(*Error, T2982::Run(*Cpu, S, Request, AdapterTokens, Error)) ||
            !TestEqual(TEXT("adapter reference completes"), (uint8)Cpu->GetPhase(S), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
        Cpu->ReturnSequence(S);
        T2982::Drain(*Cpu, 40);
        if (BaseTokens != AdapterTokens) { DistinguishingCase = C; }
    }
    if (!TestTrue(TEXT("a recorded prompt distinguishes base from adapter"), DistinguishingCase != INDEX_NONE)) { return false; }

    FSuperSLMSequence Prior;
    if (!TestEqual(TEXT("prior vend"), (uint8)Cpu->VendSequence(Prior), (uint8)ESuperSLMVendResult::Success)) { return false; }
    Cpu->ReturnSequence(Prior); // recycle queued, no Tick
    FSuperSLMSequence Incoming;
    if (!TestEqual(TEXT("incoming vend"), (uint8)Cpu->VendSequence(Incoming), (uint8)ESuperSLMVendResult::Success)) { return false; }
    Cpu->RequestAdapterSwap(Incoming, Adapter); // held by the incoming holder
    FString ReleaseError;
    bool bOk = TestFalse(TEXT("release is refused while a held request pins the adapter"),
        FSuperSLMAdapterImport::Release(Adapter, ReleaseError));
    bOk &= TestTrue(TEXT("held-release refusal names the pending hold"),
        ReleaseError.Contains(TEXT("held"), ESearchCase::IgnoreCase));
    TArray<int32> IncomingTokens;
    bOk &= TestTrue(TEXT("incoming generation runs"), T2982::Run(*Cpu, Incoming, Request, IncomingTokens, Error));
    bOk &= TestEqual(TEXT("incoming generation completes"), (uint8)Cpu->GetPhase(Incoming), (uint8)ESuperSLMSequencePhase::Complete);
    bOk &= TestEqual(TEXT("the held adapter applies before the first generated token"), IncomingTokens, AdapterTokens);
    Cpu->ReturnSequence(Incoming);
    T2982::Drain(*Cpu, 40);
    bOk &= TestTrue(TEXT("release succeeds after the holder and recycle drain"),
        FSuperSLMAdapterImport::Release(Adapter, ReleaseError));
    if (!bOk) { return false; }

    // Drop path: the new holder returns before its carrying job starts. Its held pin
    // must be released by ReleaseUser(), even though no generation used the adapter.
    if (!TestTrue(*Error, FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, Error))) { return false; }
    if (!TestEqual(TEXT("drop-path prior vend"), (uint8)Cpu->VendSequence(Prior), (uint8)ESuperSLMVendResult::Success)) { return false; }
    Cpu->ReturnSequence(Prior);
    if (!TestEqual(TEXT("drop-path incoming vend"), (uint8)Cpu->VendSequence(Incoming), (uint8)ESuperSLMVendResult::Success)) { return false; }
    Cpu->RequestAdapterSwap(Incoming, Adapter);
    Cpu->ReturnSequence(Incoming);
    T2982::Drain(*Cpu, 40);
    return TestTrue(TEXT("drop path releases the held adapter pin"), FSuperSLMAdapterImport::Release(Adapter, ReleaseError));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuperSLMU1CpuPriorSchemaDoesNotLeakTest,
    "SuperSLM.U1.Cpu.PriorSchemaDoesNotLeak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuPriorSchemaDoesNotLeakTest::RunTest(const FString& Parameters)
{
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
    if (!TestTrue(TEXT("Tokenize"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
    Request.MaxNewTokens = 32;
    TArray<int32> UnboundReference, BoundReference;
    for (int32 Arm = 0; Arm < 2; ++Arm)
    {
        FSuperSLMSequence S;
        if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(S), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (Arm == 1 && !TestTrue(TEXT("reference schema bind"), Cpu->SetSchema(S, Schema, Error))) { return false; }
        TArray<int32>& Tokens = Arm == 0 ? UnboundReference : BoundReference;
        if (!TestTrue(*Error, T2982::Run(*Cpu, S, Request, Tokens, Error)) ||
            !TestEqual(TEXT("reference complete"), (uint8)Cpu->GetPhase(S), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
        Cpu->ReturnSequence(S);
        T2982::Drain(*Cpu, 40);
    }
    if (!TestNotEqual(TEXT("schema reference distinguishes the two holders"), UnboundReference, BoundReference)) { return false; }
    FSuperSLMGenerationRequest OldRequest;
    if (!TestTrue(TEXT("old holder schema-content tokenizes"), Cpu->Tokenize(TEXT("{"), OldRequest.PromptTokens)) ||
        !TestTrue(TEXT("old holder schema-content span is nonempty"), !OldRequest.PromptTokens.IsEmpty())) { return false; }
    OldRequest.SpanKind = ESuperSLMSpanKind::SchemaContent;
    OldRequest.MaxNewTokens = 16;
    bool bOk = true;
    for (int32 NewHolderRequestsSchema = 0; NewHolderRequestsSchema < 2; ++NewHolderRequestsSchema)
    {
        FSuperSLMSequence Old;
        if (!TestEqual(TEXT("old holder vend"), (uint8)Cpu->VendSequence(Old), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (!TestTrue(TEXT("old holder schema bind"), Cpu->SetSchema(Old, Schema, Error))) { return false; }
        TArray<int32> OldTokens;
        if (!TestTrue(*Error, T2982::Run(*Cpu, Old, OldRequest, OldTokens, Error)) ||
            !TestEqual(TEXT("old holder complete"), (uint8)Cpu->GetPhase(Old), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
        const FSuperSLMSequenceStats OldStats = Cpu->GetStats(Old);
        bOk &= TestTrue(TEXT("old holder exposes schema state before return"),
            OldStats.ForcedTokenCount > 0 || OldStats.bSchemaAccepting);
        Cpu->ReturnSequence(Old); // no Tick: the new holder is admitted behind the recycle
        FSuperSLMSequence New;
        if (!TestEqual(TEXT("new holder vend"), (uint8)Cpu->VendSequence(New), (uint8)ESuperSLMVendResult::Success)) { return false; }
        const FSuperSLMSequenceStats NewStats = Cpu->GetStats(New);
        bOk &= TestFalse(TEXT("new holder cannot see old schema acceptance"), NewStats.bSchemaAccepting);
        bOk &= TestEqual(TEXT("new holder cannot see old forced-token count"), NewStats.ForcedTokenCount, int64(0));
        if (NewHolderRequestsSchema)
        {
            bOk &= TestTrue(TEXT("new holder schema bind accepted"), Cpu->SetSchema(New, Schema, Error));
        }
        TArray<int32> NewTokens;
        bOk &= TestTrue(TEXT("new holder generation runs"), T2982::Run(*Cpu, New, Request, NewTokens, Error));
        bOk &= TestEqual(TEXT("new holder generation completes"), (uint8)Cpu->GetPhase(New), (uint8)ESuperSLMSequencePhase::Complete);
        bOk &= TestEqual(TEXT("new holder gets only its own schema state"), NewTokens,
            NewHolderRequestsSchema ? BoundReference : UnboundReference);
        Cpu->ReturnSequence(New);
        T2982::Drain(*Cpu, 40);
    }
    return bOk;
}

// R-S1l(c): an adopt and generation queued by the old holder cannot be
// delivered to the new holder of the same physical slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuQueuedGenerationHandoffTest,
    "SuperSLM.U1.Cpu.QueuedGenerationHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuQueuedGenerationHandoffTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper W;
    if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
    FString Path, Error;
    if (!TestTrue(TEXT("A-EX artifact"), TryGetAExArtifactPath(Path, Error))) { return false; }
    FSuperSLMImportDiagnostic Diag;
    USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
    if (!TestNotNull(TEXT("A-EX model"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted)) { return false; }
    USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
    if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
    FSuperSLMRuntimeConfig Config;
    Config.BlockCount = 1;
    Config.PrefixBlockCount = 1;
    Config.MaxSequencesPerDecodeCall = 1;
    Config.MaxPrefillChunkBudget = 64;
    Config.MaxLayerBudget = 24;
    Config.SequenceLifecycleBudgetMs = 1000.0;
    Config.TickBudgetMs = 1000.0;
    if (!TestEqual(TEXT("configure"), (uint8)Cpu->Configure(Model, Config).Result,
        (uint8)ESuperSLMConfigureResult::Success)) { return false; }
    FSuperSLMSchemaHandle Schema;
    if (!TestTrue(TEXT("schema lookup"), FSuperSLMSchemaLookup::LookupByName(*Model, AExSchemaName(), Schema, Error))) { return false; }
    FSuperSLMGenerationRequest Prompt1, Prompt2;
    if (!TestTrue(TEXT("prompt 1 tokenize"), Cpu->Tokenize(TEXT("The first customer asks for tea."), Prompt1.PromptTokens)) ||
        !TestTrue(TEXT("prompt 2 tokenize"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Prompt2.PromptTokens))) { return false; }
    Prompt1.MaxNewTokens = 16;
    Prompt2.MaxNewTokens = 16;
    TArray<int32> References[2];
    for (int32 Bind = 0; Bind < 2; ++Bind)
    {
        FSuperSLMSequence ReferenceSeq;
        if (!TestEqual(TEXT("reference vend"), (uint8)Cpu->VendSequence(ReferenceSeq), (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (Bind && !TestTrue(TEXT("reference bind"), Cpu->SetSchema(ReferenceSeq, Schema, Error))) { return false; }
        if (!TestTrue(TEXT("reference generation"), T2982::Run(*Cpu, ReferenceSeq, Prompt2, References[Bind], Error)) ||
            !TestEqual(TEXT("reference complete"), (uint8)Cpu->GetPhase(ReferenceSeq), (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
        Cpu->ReturnSequence(ReferenceSeq);
        T2982::Drain(*Cpu, 40);
    }
    if (!TestNotEqual(TEXT("prompt 2 references discriminate"), References[0], References[1])) { return false; }
    FSuperSLMPrefix Prefix;
    if (!TestTrue(TEXT("create prefix"), Cpu->CreatePrefix(Prompt1.PromptTokens, Prefix, Error))) { return false; }
    DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
        [Cpu, &Prefix]() { return Cpu->IsPrefixReady(Prefix); }, 60.0);
    if (!TestTrue(TEXT("prefix ready"), Cpu->IsPrefixReady(Prefix))) { return false; }
    bool bOk = true;
    for (int32 Ask = 0; Ask < 2; ++Ask)
    {
        FSuperSLMSequence Old;
        if (!TestEqual(TEXT("old holder vend"), (uint8)Cpu->VendSequence(Old), (uint8)ESuperSLMVendResult::Success)) { return false; }
        FSuperSLMLifecycleOpHandle Adopt;
        bOk &= TestEqual(TEXT("old holder adopt queues"), (uint8)Cpu->AdoptPrefix(Old, Prefix, Adopt, Error),
            (uint8)ESuperSLMRestoreResult::Success);
        FSuperSLMGenerationRequest OldRequest = Prompt1;
        OldRequest.PromptTokens.Reset();
        bOk &= TestTrue(TEXT("old holder generation queues"), Cpu->BeginGeneration(Old, OldRequest, Error));
        bOk &= TestEqual(TEXT("old generation has not delivered"), Cpu->GetGeneratedTokens(Old).Num(), 0);
        Cpu->ReturnSequence(Old); // no Tick between queued work, return, and new vend
        FSuperSLMSequence Incoming;
        if (!TestEqual(TEXT("new holder vends before old queue drains"), (uint8)Cpu->VendSequence(Incoming),
            (uint8)ESuperSLMVendResult::Success)) { return false; }
        if (Ask) { bOk &= TestTrue(TEXT("new holder bind accepted"), Cpu->SetSchema(Incoming, Schema, Error)); }
        TArray<int32> Tokens;
        bOk &= TestTrue(TEXT("new holder generation runs"), T2982::Run(*Cpu, Incoming, Prompt2, Tokens, Error));
        bOk &= TestEqual(TEXT("new holder completes"), (uint8)Cpu->GetPhase(Incoming), (uint8)ESuperSLMSequencePhase::Complete);
        bOk &= TestEqual(TEXT("only prompt 2 reaches new holder"), Tokens, References[Ask]);
        Cpu->ReturnSequence(Incoming);
        T2982::Drain(*Cpu, 40);
    }
    bOk &= TestTrue(TEXT("prefix release"), Cpu->ReleasePrefix(Prefix, Error));
    return bOk;
}

namespace U1CpuRestoredHandoff
{
    bool DriveRestore(USuperSLMSubsystem& Cpu, const FSuperSLMLifecycleOpHandle& Handle)
    {
        DrainTicks(Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
            [&Cpu, &Handle]() { return Cpu.GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; }, 120.0);
        return Cpu.GetLifecycleOpResult(Handle) == ESuperSLMRestoreResult::Success;
    }

    bool Run(FAutomationTestBase& T, bool bQueuedRecycle)
    {
        FTestWorldWrapper W;
        if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
        FSuperSLMImportDiagnostic Diag;
        USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
        if (!T.TestNotNull(TEXT("A-AD model"), Model) || !T.TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
        USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
        if (!T.TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
        FSuperSLMRuntimeConfig Config;
        Config.BlockCount = 1;
        Config.MaxSequencesPerDecodeCall = 1;
        Config.MaxPrefillChunkBudget = 64;
        Config.MaxLayerBudget = 28;
        Config.SequenceLifecycleBudgetMs = 1000.0;
        Config.TickBudgetMs = 1000.0;
        if (!T.TestEqual(TEXT("configure"), (uint8)Cpu->Configure(Model, Config).Result,
            (uint8)ESuperSLMConfigureResult::Success)) { return false; }
        FSuperSLMAdapterHandle Adapter;
        FString Error;
        if (!T.TestTrue(TEXT("adapter import"), FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Model, Adapter, Error))) { return false; }
        TArray<FReferenceCase> Cases;
        if (!T.TestTrue(TEXT("reference cases"), LoadReferenceCases(Cases, Error)) ||
            !T.TestTrue(TEXT("case 0"), Cases.Num() > 0)) { return false; }
        FSuperSLMGenerationRequest Request;
        Request.PromptTokens = Cases[0].PromptTokens;
        Request.MaxNewTokens = 32;
        Request.SpanKind = ESuperSLMSpanKind::Prompt;
        bool bOk = true;
        TArray<int32> Totals[2];
        for (int32 Ask = 0; Ask < 2; ++Ask)
        {
            FSuperSLMSequence Source;
            if (!T.TestEqual(TEXT("source vend"), (uint8)Cpu->VendSequence(Source), (uint8)ESuperSLMVendResult::Success)) { return false; }
            if (!T.TestTrue(TEXT("source generation queues"), Cpu->BeginGeneration(Source, Request, Error))) { return false; }
            DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
                [Cpu, &Source]() { return Cpu->GetGeneratedTokens(Source).Num() >= 8; }, 120.0);
            if (!T.TestTrue(TEXT("save point reached in decode"), Cpu->GetGeneratedTokens(Source).Num() >= 8 &&
                Cpu->GetPhase(Source) == ESuperSLMSequencePhase::Decoding)) { return false; }
            FSuperSLMLifecycleOpHandle Save;
            if (!T.TestEqual(TEXT("save queues"), (uint8)Cpu->SaveSequence(Source, Save, Error),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
            if (Ask) { Cpu->RequestAdapterSwap(Source, Adapter); }
            TArray<uint8> Blob;
            ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
            DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
                [Cpu, &Save, &Blob, &SaveResult]() { SaveResult = Cpu->GetSaveResult(Save, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 120.0);
            if (!T.TestEqual(TEXT("save succeeds"), (uint8)SaveResult,
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
            const TArray<int32> Prefix = Cpu->GetGeneratedTokens(Source);
            DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
                [Cpu, &Source]() { return Cpu->GetPhase(Source) == ESuperSLMSequencePhase::Complete ||
                    Cpu->GetPhase(Source) == ESuperSLMSequencePhase::Faulted; }, 120.0);
            bOk &= T.TestEqual(TEXT("unsaved generation completes"), (uint8)Cpu->GetPhase(Source),
                (uint8)ESuperSLMSequencePhase::Complete);
            Totals[Ask] = Cpu->GetGeneratedTokens(Source);
            bOk &= T.TestTrue(TEXT("unsaved total retains save prefix"), Totals[Ask].Num() >= Prefix.Num());
            Cpu->ReturnSequence(Source);
            T2982::Drain(*Cpu, 40);

            if (bQueuedRecycle)
            {
                FSuperSLMSequence Previous;
                if (!T.TestEqual(TEXT("previous holder vends"), (uint8)Cpu->VendSequence(Previous),
                    (uint8)ESuperSLMVendResult::Success)) { return false; }
                TArray<int32> PreviousTokens;
                if (!T.TestTrue(TEXT("previous holder generates"), T2982::Run(*Cpu, Previous, Request, PreviousTokens, Error))) { return false; }
                Cpu->ReturnSequence(Previous); // restore reserves the slot behind this recycle
            }
            FSuperSLMSequence Restored;
            FSuperSLMLifecycleOpHandle Restore;
            if (!T.TestEqual(TEXT("restore reserves the slot"),
                (uint8)Cpu->RestoreSequence(Blob, Model, Restored, Restore, Error),
                (uint8)ESuperSLMRestoreResult::Success)) { return false; }
            if (Ask) { Cpu->RequestAdapterSwap(Restored, Adapter); }
            if (!T.TestTrue(TEXT("restore resolves"), DriveRestore(*Cpu, Restore))) { return false; }
            DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
                [Cpu, &Restored]() { return Cpu->GetGeneratedTokens(Restored).Num() >= 1 ||
                    Cpu->GetPhase(Restored) == ESuperSLMSequencePhase::Faulted; }, 120.0);
            bOk &= T.TestTrue(TEXT("restored continuation delivers its first token"),
                Cpu->GetGeneratedTokens(Restored).Num() >= 1);
            bOk &= T.TestEqual(TEXT("restored report matches request after the carrying job delivers"),
                Cpu->GetActiveAdapter(Restored).Id, Ask ? Adapter.Id : int64(0));
            DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
                [Cpu, &Restored]() { return Cpu->GetPhase(Restored) == ESuperSLMSequencePhase::Complete ||
                    Cpu->GetPhase(Restored) == ESuperSLMSequencePhase::Faulted; }, 120.0);
            bOk &= T.TestEqual(TEXT("restored continuation completes"), (uint8)Cpu->GetPhase(Restored),
                (uint8)ESuperSLMSequencePhase::Complete);
            TArray<int32> Total = Prefix;
            Total.Append(Cpu->GetGeneratedTokens(Restored));
            bOk &= T.TestEqual(TEXT("restored continuation equals unsaved same-point switch"), Total, Totals[Ask]);
            Cpu->ReturnSequence(Restored);
            T2982::Drain(*Cpu, 40);
        }
        bOk &= T.TestNotEqual(TEXT("base and same-point adapter references discriminate"), Totals[0], Totals[1]);
        bOk &= T.TestTrue(TEXT("adapter release after both arms"), FSuperSLMAdapterImport::Release(Adapter, Error));
        return bOk;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuSuccessfulRestoreHandoffTest,
    "SuperSLM.U1.Cpu.SuccessfulRestoreHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuSuccessfulRestoreHandoffTest::RunTest(const FString& Parameters)
{
    return U1CpuRestoredHandoff::Run(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuRestoreBehindRecycleTest,
    "SuperSLM.U1.Cpu.RestoreBehindRecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuRestoreBehindRecycleTest::RunTest(const FString& Parameters)
{
    return U1CpuRestoredHandoff::Run(*this, true);
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
