// U1 R-S1l(d): refused-restore recycle clears the previous holder.
// Derived from the T-2973 review probe against the live implementation.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	FString Toks(const TArray<int32>& T)
	{
		FString S;
		for (int32 V : T) { S += FString::Printf(TEXT("%d "), V); }
		return S;
	}

	void Drain(USuperSLMSubsystem& Sub, int32 Ticks)
	{
		for (int32 I = 0; I < Ticks; ++I)
		{
			Sub.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.005f);
		}
	}

	bool GenerateUnbound(FAutomationTestBase& T, USuperSLMSubsystem& Sub, const FSuperSLMGenerationRequest& Req, TArray<int32>& Out, bool& bAdapterReported)
	{
		FSuperSLMSequence Seq;
		if (Sub.VendSequence(Seq) != ESuperSLMVendResult::Success) { T.AddError(TEXT("vend failed")); return false; }
		bAdapterReported = Sub.GetActiveAdapter(Seq).IsValid();
		FString Err;
		const bool bOk = RunGenerationToCompletion(Sub, Seq, Req, Out, 20000, Err);
		Sub.ReturnSequence(Seq);
		Drain(Sub, 40);
		if (!bOk) { T.AddError(FString::Printf(TEXT("unbound generation: %s"), *Err)); }
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1RefusedRestoreAdapterLeakTest,
	"SuperSLM.U1.Cpu.RefusedRestoreAdapterLeak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1RefusedRestoreAdapterLeakTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base"), Base)) { return false; }
	FString AExPath, Reason;
	if (!TestTrue(*Reason, TryGetAExArtifactPath(AExPath, Reason))) { return false; }
	FSuperSLMImportDiagnostic Diag2;
	USuperSLMModel* Other = FSuperSLMModelImport::ImportFromFile(AExPath, Diag2);
	if (!TestNotNull(TEXT("A-EX (the mismatched expected model)"), Other)) { return false; }

	FSuperSLMAdapterHandle Adapter;
	FString AErr;
	if (!TestTrue(*AErr, FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, AErr))) { return false; }

	USuperSLMSubsystem* Sub = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), Sub)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1; // every vend and every restore reuses the one slot
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0;
	const FSuperSLMConfigureReport Rep = Sub->Configure(Base, Config);
	if (!TestEqual(*Rep.Message, (uint8)Rep.Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }

	TArray<FReferenceCase> Cases;
	FString LErr;
	if (!TestTrue(*LErr, LoadReferenceCases(Cases, LErr))) { return false; }
	FSuperSLMGenerationRequest Req;
	Req.MaxNewTokens = 32;

	// References: base model, and under the adapter. Scan the recorded prompts for the first one on which
	// they differ, or no token clause can fail.
	TArray<int32> BaseTokens, AdapterTokens;
	int32 Chosen = INDEX_NONE;
	for (int32 C = 0; C < Cases.Num() && Chosen == INDEX_NONE; ++C)
	{
		Req.PromptTokens = Cases[C].PromptTokens;
		bool bDummy = false;
		if (!GenerateUnbound(*this, *Sub, Req, BaseTokens, bDummy)) { return false; }
		FSuperSLMSequence Seq;
		Sub->VendSequence(Seq);
		Sub->RequestAdapterSwap(Seq, Adapter);
		Drain(*Sub, 40);
		TestTrue(TEXT("reference adapter bound"), Sub->GetActiveAdapter(Seq).IsValid());
		FString Err;
		TestTrue(*Err, RunGenerationToCompletion(*Sub, Seq, Req, AdapterTokens, 20000, Err));
		Sub->ReturnSequence(Seq);
		Drain(*Sub, 40);
		AddInfo(FString::Printf(TEXT("T2973 case %d BASE    : %s"), C, *Toks(BaseTokens)));
		AddInfo(FString::Printf(TEXT("T2973 case %d ADAPTER : %s"), C, *Toks(AdapterTokens)));
		if (BaseTokens != AdapterTokens) { Chosen = C; }
	}
	AddInfo(FString::Printf(TEXT("T2973 discriminating case: %d of %d"), Chosen, Cases.Num()));
	const bool bTokensDiscriminate = Chosen != INDEX_NONE;
	if (!bTokensDiscriminate) { AddWarning(TEXT("T2973: no recorded prompt separates base from adapter at 32 tokens; token clause is uninformative")); }

	// A CPU save blob from this base model (valid, CPU-tagged; restored against A-EX it mismatches).
	TArray<uint8> Blob;
	{
		FSuperSLMSequence Seq;
		Sub->VendSequence(Seq);
		FString Err;
		Sub->BeginGeneration(Seq, Req, Err);
		DrainTicks(*Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Sub, &Seq]() { return Sub->GetPhase(Seq) != ESuperSLMSequencePhase::Prefilling; }, 60.0);
		FSuperSLMLifecycleOpHandle H;
		Sub->SaveSequence(Seq, H, Err);
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		DrainTicks(*Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Sub, &H, &Blob, &SaveResult]() { SaveResult = Sub->GetSaveResult(H, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 60.0);
		if (!TestEqual(TEXT("save"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Sub->ReturnSequence(Seq);
		Drain(*Sub, 40);
	}

	bool bAll = true;
		for (int32 bRequestAdapter = 0; bRequestAdapter <= 1; ++bRequestAdapter)
		{
			const FString Arm = bRequestAdapter ? TEXT("adapter requested during refused restore") : TEXT("control: no request");

			FSuperSLMSequence R;
			FSuperSLMLifecycleOpHandle H;
			FString Err;
			const ESuperSLMRestoreResult Q = Sub->RestoreSequence(Blob, Other, R, H, Err);
			TestEqual(*FString::Printf(TEXT("%s restore queues"), *Arm), (uint8)Q, (uint8)ESuperSLMRestoreResult::Success);
			if (bRequestAdapter)
			{
				Sub->RequestAdapterSwap(R, Adapter); // a caller setting its NPC's adapter on the restored handle
			}
			DrainTicks(*Sub, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
				[Sub, &H]() { return Sub->GetLifecycleOpResult(H) != ESuperSLMRestoreResult::Pending; }, 60.0);
			const ESuperSLMRestoreResult Res = Sub->GetLifecycleOpResult(H);
			AddInfo(FString::Printf(TEXT("T2973 %s restore result %d (ModelMismatch = %d)"), *Arm, (int32)Res, (int32)ESuperSLMRestoreResult::ModelMismatch));
			TestEqual(*FString::Printf(TEXT("%s restore refused ModelMismatch"), *Arm), (uint8)Res, (uint8)ESuperSLMRestoreResult::ModelMismatch);
			Drain(*Sub, 40); // the refusal's recycle Reset runs and delivers

			// The next user: vends, binds nothing, generates.
			FSuperSLMSequence U2;
			Sub->VendSequence(U2);
			const bool bReported = Sub->GetActiveAdapter(U2).IsValid();
			TArray<int32> U2Tokens;
			TestTrue(*Err, RunGenerationToCompletion(*Sub, U2, Req, U2Tokens, 20000, Err));
			// While U2 (who never requested an adapter) holds the slot, is the adapter releasable?
			FSuperSLMAdapterHandle Probe = Adapter;
			FString RelErr;
			const bool bReleasable = [&]() -> bool
			{
				// Do not actually release on success: re-import cost aside, later arms need it. Ask Layer 1's
				// own count via a release attempt only when it would be refused; otherwise report "free".
				// (Release is attempted for real; on success the adapter is re-imported below.)
				return FSuperSLMAdapterImport::Release(Probe, RelErr);
			}();
			if (bReleasable)
			{
				FString IErr;
				FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, IErr);
			}
			AddInfo(FString::Printf(TEXT("T2973 %s next user: adapter reported %d, tokens %s, equals BASE %d, equals ADAPTER %d, adapter releasable %d (%s)"),
				*Arm, bReported ? 1 : 0, *Toks(U2Tokens), U2Tokens == BaseTokens ? 1 : 0, U2Tokens == AdapterTokens ? 1 : 0, bReleasable ? 1 : 0, *RelErr));
			bAll &= TestFalse(*FString::Printf(TEXT("%s next user must not report an adapter it never requested"), *Arm), bReported);
			if (bTokensDiscriminate) { bAll &= TestEqual(*FString::Printf(TEXT("%s next user's tokens must equal the base model's"), *Arm), U2Tokens, BaseTokens); }
			bAll &= TestTrue(*FString::Printf(TEXT("%s adapter must be releasable while nobody requested it"), *Arm), bReleasable);
			Sub->ReturnSequence(U2);
			Drain(*Sub, 40);
		}
	return bAll;
}

// R-S1l(i): unlike ModelMismatch, this refusal comes from Layer 1 after the
// worker has tried the restore. The recycled slot must not re-apply the
// restoring holder's adapter to the next holder on its create path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuWorkerRefusalHandoffTest,
	"SuperSLM.U1.Cpu.WorkerRefusalHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuWorkerRefusalHandoffTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base"), Model) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	FSuperSLMAdapterHandle Adapter;
	FString Error;
	if (!TestTrue(TEXT("adapter imports"), FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Model, Adapter, Error))) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 28;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("configure"), (uint8)Cpu->Configure(Model, Config).Result,
		(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	TArray<FReferenceCase> Cases;
	if (!TestTrue(TEXT("reference prompts load"), LoadReferenceCases(Cases, Error)) ||
		!TestTrue(TEXT("at least one prompt"), Cases.Num() > 0)) { return false; }
	FSuperSLMGenerationRequest Req;
	Req.MaxNewTokens = 32;
	TArray<int32> Reference[2];
	int32 Chosen = INDEX_NONE;
	for (int32 I = 0; I < Cases.Num() && Chosen == INDEX_NONE; ++I)
	{
		Req.PromptTokens = Cases[I].PromptTokens;
		for (int32 Bind = 0; Bind < 2; ++Bind)
		{
			FSuperSLMSequence Seq;
			if (Cpu->VendSequence(Seq) != ESuperSLMVendResult::Success) { return false; }
			if (Bind) { Cpu->RequestAdapterSwap(Seq, Adapter); }
			if (!TestTrue(TEXT("reference completes"), RunGenerationToCompletion(*Cpu, Seq, Req,
				Reference[Bind], 20000, Error))) { return false; }
			Cpu->ReturnSequence(Seq);
			Drain(*Cpu, 40);
		}
		if (Reference[0] != Reference[1]) { Chosen = I; }
	}
	if (!TestTrue(TEXT("base/adapter references discriminate"), Chosen != INDEX_NONE)) { return false; }

	FSuperSLMSequence Source;
	if (Cpu->VendSequence(Source) != ESuperSLMVendResult::Success) { return false; }
	if (!TestTrue(TEXT("source generation queues"), Cpu->BeginGeneration(Source, Req, Error))) { return false; }
	DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Cpu, &Source]() { return Cpu->GetPhase(Source) == ESuperSLMSequencePhase::Decoding; }, 60.0);
	if (!TestEqual(TEXT("source reaches Decoding"), (uint8)Cpu->GetPhase(Source),
		(uint8)ESuperSLMSequencePhase::Decoding)) { return false; }
	FSuperSLMLifecycleOpHandle Save;
	if (!TestEqual(TEXT("source save queues"), (uint8)Cpu->SaveSequence(Source, Save, Error),
		(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	TArray<uint8> Blob;
	ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
	DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Cpu, &Save, &Blob, &SaveResult]() { SaveResult = Cpu->GetSaveResult(Save, Blob); return SaveResult != ESuperSLMRestoreResult::Pending; }, 60.0);
	if (!TestEqual(TEXT("source save succeeds"), (uint8)SaveResult,
		(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	Cpu->ReturnSequence(Source);
	Drain(*Cpu, 40);
	int32 Layer1Header = INDEX_NONE;
	for (int32 I = 0; I + 3 < Blob.Num(); ++I)
	{
		if (Blob[I] == 'S' && Blob[I + 1] == 'S' && Blob[I + 2] == 'B' && Blob[I + 3] == '5')
		{ Layer1Header = I; break; }
	}
	if (!TestTrue(TEXT("saved blob contains Layer-1 header"), Layer1Header != INDEX_NONE)) { return false; }
	Blob[Layer1Header] ^= 0xff;

	bool bOk = true;
	for (int32 Ask = 0; Ask < 2; ++Ask)
	{
		FSuperSLMSequence Restoring;
		FSuperSLMLifecycleOpHandle Restore;
		if (!TestEqual(TEXT("corrupt restore queues for worker refusal"),
			(uint8)Cpu->RestoreSequence(Blob, Model, Restoring, Restore, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		Cpu->RequestAdapterSwap(Restoring, Adapter);
		DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[Cpu, &Restore]() { return Cpu->GetLifecycleOpResult(Restore) != ESuperSLMRestoreResult::Pending; }, 60.0);
		const ESuperSLMRestoreResult Refusal = Cpu->GetLifecycleOpResult(Restore);
		bOk &= TestTrue(TEXT("worker refuses the Layer-1 payload, not the model"),
			Refusal == ESuperSLMRestoreResult::KvMismatch || Refusal == ESuperSLMRestoreResult::Malformed);
		FSuperSLMSequence Next;
		if (!TestEqual(TEXT("new holder vends before recycle drain"), (uint8)Cpu->VendSequence(Next),
			(uint8)ESuperSLMVendResult::Success)) { return false; }
		if (Ask) { Cpu->RequestAdapterSwap(Next, Adapter); }
		TArray<int32> Actual;
		bOk &= TestTrue(TEXT("new holder completes"), RunGenerationToCompletion(*Cpu, Next, Req, Actual, 20000, Error));
		bOk &= TestEqual(TEXT("new holder uses only its own adapter request"), Actual, Reference[Ask]);
		bOk &= TestEqual(TEXT("reported adapter matches new holder"), Cpu->GetActiveAdapter(Next).IsValid(), Ask == 1);
		Cpu->ReturnSequence(Next);
		Drain(*Cpu, 40);
	}
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
