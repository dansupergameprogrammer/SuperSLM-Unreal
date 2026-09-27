// U1 R-S2g(iv),(v),(vii),(x) and R-S2k(i): durable adversarial constructions.
// T-2983 P1b, P2, P3 and P4; each reads a real A-EX or A-AD artifact.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	FString Join(const TArray<int32>& A)
	{
		FString S;
		for (int32 V : A) { S += FString::Printf(TEXT("%d,"), V); }
		return S;
	}

	bool ProbeSetUpGpu(FAutomationTestBase& T, UWorld* World, USuperSLMModel* Model, int32 BlockCount, int32 K, int64 ContextCap, USuperSLMGpuSubsystem*& OutGpu)
	{
		OutGpu = GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("GPU subsystem"), OutGpu)) { return false; }
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = ContextCap;
		Config.BlockCount = BlockCount;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
		Config.K = K;
		Config.TickBudgetMs = 1000.0;
		const FSuperSLMGpuConfigureReport Report = OutGpu->Configure(Model, Config);
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	USuperSLMModel* ImportAEx(FAutomationTestBase& T)
	{
		FString Path, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX present (%s)"), *Reason), TryGetAExArtifactPath(Path, Reason))) { return nullptr; }
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* M = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		return (M != nullptr && Diag.bAccepted) ? M : nullptr;
	}
}

// --- P1b: the same claim with the game loop paced at a real 60 Hz frame (Sleep per Tick), so the
// submission thread keeps up and the save resolves while issued tokens are produced but not yet
// applied (T+K). ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuCarriedTokensAtSavePaced, "SuperSLM.U1.Gpu.CarriedTokensAtSavePaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuCarriedTokensAtSavePaced::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = ImportAEx(*this);
	if (!TestNotNull(TEXT("A-EX import"), Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Model, 1, AExNumHiddenLayers, 4096, Gpu)) { return false; }

	FSuperSLMGenerationRequest Request;
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU tokenizer subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.BlockCount = 1;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU tokenizer configure"), (uint8)Cpu->Configure(Model, CpuConfig).Result,
		(uint8)ESuperSLMConfigureResult::Success) ||
		!TestTrue(TEXT("natural prompt tokenizes"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 40;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> CpuReference;
	{
		FSuperSLMSequence CpuSeq;
		FString Error;
		if (!TestEqual(TEXT("CPU reference vend"), (uint8)Cpu->VendSequence(CpuSeq),
			(uint8)ESuperSLMVendResult::Success) ||
			!TestTrue(TEXT("CPU reference begins"), Cpu->BeginGeneration(CpuSeq, Request, Error))) { return false; }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (Cpu->GetPhase(CpuSeq) != ESuperSLMSequencePhase::Complete &&
			Cpu->GetPhase(CpuSeq) != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() < Deadline)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestEqual(TEXT("CPU natural-prompt reference completes"), (uint8)Cpu->GetPhase(CpuSeq),
			(uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		CpuReference = Cpu->GetGeneratedTokens(CpuSeq);
		Cpu->ReturnSequence(CpuSeq);
	}

	TArray<int32> Reference;
	{
		FSuperSLMGpuSequence Ref;
		Gpu->VendSequence(Ref, ESuperSLMGpuDecodePath::OneCall);
		FString E;
		if (!TestTrue(*FString::Printf(TEXT("reference completes (%s)"), *E), RunGpuGenerationToCompletion(*Gpu, Ref, Request, Reference, 60.0, E))) { return false; }
		Gpu->ReturnSequence(Ref);
	}
	if (!TestEqual(TEXT("GPU natural-prompt reference matches CPU"), Reference, CpuReference)) { return false; }

	FSuperSLMGpuSequence Seq;
	Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	Gpu->RequestBeginGeneration(Seq, Request);
	constexpr double MaxWaitForFirstTokenSeconds = 120.0;
	const double WaitStartSeconds = FPlatformTime::Seconds();
	while (Gpu->GetGeneratedTokens(Seq).IsEmpty() &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Complete &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Faulted &&
		FPlatformTime::Seconds() - WaitStartSeconds < MaxWaitForFirstTokenSeconds)
	{
		Gpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("paced save follows a delivered decode token"),
		!Gpu->GetGeneratedTokens(Seq).IsEmpty() && Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding)) { return false; }
	const int32 ObservedAtRequest = Gpu->GetGeneratedTokens(Seq).Num();
	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const ESuperSLMRestoreResult SaveResult = DriveSaveToResolution(*Gpu, SaveHandle, Blob);
	const TArray<int32> Pre = Gpu->GetGeneratedTokens(Seq);
	if (!TestEqual(TEXT("save Success"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	int32 ObservedAtSave = -1, ProducedAtSave = -1;
	if (!TestTrue(TEXT("save exposes delivered and produced counts"),
		Gpu->GetSaveTokenCounts(SaveHandle, ObservedAtSave, ProducedAtSave))) { return false; }
	if (!TestEqual(TEXT("save's observed count equals delivered prefix"), ObservedAtSave, Pre.Num()) ||
		!TestTrue(TEXT("save resolves while produced tokens remain in flight"), ObservedAtSave < ProducedAtSave)) { return false; }
	TSet<int32> DistinctTail;
	for (int32 I = Pre.Num(); I < Reference.Num(); ++I) { DistinctTail.Add(Reference[I]); }
	if (!TestTrue(TEXT("the reference has eight distinct tokens after the save point"), DistinctTail.Num() >= 8)) { return false; }
	Gpu->ReturnSequence(Seq);

	FSuperSLMGpuSequence Restored;
	const ESuperSLMRestoreResult RestoreResult = DriveRestoreToResolution(*Gpu, Gpu->RequestRestoreSequence(Blob, Model), Restored);
	if (!TestEqual(TEXT("restore Success"), (uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	TArray<int32> Tail;
	FString E;
	const bool bDone = RunGpuGenerationToCompletion(*Gpu, Restored, Request, Tail, 60.0, E, /*bAlreadyInProgress*/ true);
	const ESuperSLMSequencePhase PhaseAfter = Gpu->GetPhase(Restored);
	Gpu->ReturnSequence(Restored);

	TArray<int32> Concat = Pre;
	Concat.Append(Tail);
	AddInfo(FString::Printf(TEXT("[probe P1b] Reference (%d): [%s]"), Reference.Num(), *Join(Reference)));
	AddInfo(FString::Printf(TEXT("[probe P1b] observed at save request %d; PreSave observed at save resolution (%d): [%s]"), ObservedAtRequest, Pre.Num(), *Join(Pre)));
	AddInfo(FString::Printf(TEXT("[probe P1b] RestoredTail (%d): [%s]; restored phase %d; completed=%d"), Tail.Num(), *Join(Tail), (int32)PhaseAfter, bDone ? 1 : 0));
	const bool bCompleted = TestTrue(*FString::Printf(TEXT("restored completes (%s)"), *E), bDone);
	return bCompleted && TestEqual(TEXT("delivered tokens at save plus restored tail equal the unsaved reference"), Concat, Reference);
}

// --- P2: a restore's reserved slot is not vendable while in flight, and restoring into slot 0 keeps
// the declared residency. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuRestoreReservation, "SuperSLM.U1.Gpu.RestoreReservationAndSlot0Residency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuRestoreReservation::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = ImportAEx(*this);
	if (!TestNotNull(TEXT("A-EX import"), Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Model, 1, AExNumHiddenLayers, 4096, Gpu)) { return false; }

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Seq;
	Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	Gpu->RequestBeginGeneration(Seq, Request);
	for (int32 T = 0; T < 6; ++T) { if (!PacedTick(*this, *Gpu, TEXT("P2 position before save"))) { break; } }
	TArray<uint8> Blob;
	TestEqual(TEXT("save Success"), (uint8)DriveSaveToResolution(*Gpu, Gpu->RequestSaveSequence(Seq), Blob), (uint8)ESuperSLMRestoreResult::Success);
	Gpu->ReturnSequence(Seq);
	for (int32 T = 0; T < 4; ++T) { if (!PacedTick(*this, *Gpu, TEXT("P2 drain after return"))) { break; } FPlatformProcess::Sleep(1.0f / 60.0f); }

	const int64 ResidencyBefore = Gpu->GetDeclaredGpuResidencyBytes();
	const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(Blob, Model);
	const ESuperSLMRestoreResult Immediate = Gpu->GetLifecycleOpResult(H);
	FSuperSLMGpuSequence Intruder;
	const ESuperSLMGpuVendResult VendDuring = Gpu->VendSequence(Intruder, ESuperSLMGpuDecodePath::OneCall);
	const int32 FreeDuring = Gpu->GetPoolFreeCount();
	FSuperSLMGpuSequence Restored;
	const ESuperSLMRestoreResult R = DriveRestoreToResolution(*Gpu, H, Restored);
	const int64 ResidencyAfter = Gpu->GetDeclaredGpuResidencyBytes();
	AddInfo(FString::Printf(TEXT("[probe P2] restore result at request=%d, vend during=%d, free during=%d, final restore=%d, residency before=%lld after=%lld"),
		(int32)Immediate, (int32)VendDuring, FreeDuring, (int32)R, ResidencyBefore, ResidencyAfter));
	bool bOk = TestEqual(TEXT("[probe P2] restore still pending at request time (otherwise the window was not exercised)"), (uint8)Immediate, (uint8)ESuperSLMRestoreResult::Pending);
	bOk &= TestEqual(TEXT("[probe P2] vend during an in-flight restore must be refused"), (uint8)VendDuring, (uint8)ESuperSLMGpuVendResult::PoolExhausted);
	bOk &= TestEqual(TEXT("[probe P2] pool free count during restore"), FreeDuring, 0);
	bOk &= TestEqual(TEXT("[probe P2] restore Success"), (uint8)R, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("[probe P2] declared residency unchanged by a restore into slot 0"), ResidencyAfter, ResidencyBefore);
	if (Intruder.IsValid()) { Gpu->ReturnSequence(Intruder); }
	Gpu->ReturnSequence(Restored);
	return bOk;
}

// --- P4: after a restore that Layer 1 refuses (one corrupted byte of the saved Layer-1 section, a
// production input: a damaged save file), the slot the refusal released serves the next vend. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuVendAfterFailedRestore, "SuperSLM.U1.Gpu.VendAfterFailedRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuVendAfterFailedRestore::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = ImportAEx(*this);
	if (!TestNotNull(TEXT("A-EX import"), Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Model, 1, AExNumHiddenLayers, 4096, Gpu)) { return false; }

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Seq;
	Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	Gpu->RequestBeginGeneration(Seq, Request);
	for (int32 T = 0; T < 6; ++T) { if (!PacedTick(*this, *Gpu, TEXT("P4 position before save"))) { break; } }
	TArray<uint8> Blob;
	TestEqual(TEXT("save Success"), (uint8)DriveSaveToResolution(*Gpu, Gpu->RequestSaveSequence(Seq), Blob), (uint8)ESuperSLMRestoreResult::Success);
	Gpu->ReturnSequence(Seq);
	for (int32 T = 0; T < 4; ++T) { if (!PacedTick(*this, *Gpu, TEXT("P4 drain after return"))) { break; } FPlatformProcess::Sleep(1.0f / 60.0f); }

	int32 MagicAt = INDEX_NONE;
	for (int32 I = 0; I + 3 < Blob.Num(); ++I)
	{
		if (Blob[I] == 0x53 && Blob[I + 1] == 0x4C && Blob[I + 2] == 0x4D && Blob[I + 3] == 0x35) { MagicAt = I; break; }
	}
	if (!TestTrue(TEXT("the Layer-1 'SLM5' header is present in the blob"), MagicAt != INDEX_NONE)) { return false; }
	Blob[MagicAt] ^= 0xFF;

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(Blob, Model);
	const bool bAccepted = H.IsValid();
	const ESuperSLMRestoreResult R = DriveRestoreToResolution(*Gpu, H, Restored);
	const FString RestoreError = Gpu->GetLastLifecycleRequestError();
	const int32 FreeAfter = Gpu->GetPoolFreeCount();

	FSuperSLMGpuSequence Next;
	const ESuperSLMGpuVendResult V = Gpu->VendSequence(Next, ESuperSLMGpuDecodePath::OneCall);
	TArray<int32> Tokens;
	FString E;
	const bool bDone = RunGpuGenerationToCompletion(*Gpu, Next, Request, Tokens, 15.0, E);
	const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Next);
	AddInfo(FString::Printf(TEXT("[probe P4] magic at %d of %d; request accepted=%d; restore result=%d (%s); free after=%d; vend=%d; next generation completed=%d (%s), phase=%d, tokens=%d, fault reason=%d"),
		MagicAt, Blob.Num(), bAccepted ? 1 : 0, (int32)R, *RestoreError, FreeAfter, (int32)V, bDone ? 1 : 0, *E, (int32)Phase, Tokens.Num(), (int32)Gpu->GetLastFaultReason(Next)));
	bool bOk = TestTrue(TEXT("[probe P4] the corrupted restore is refused (otherwise the failure path was not exercised)"), bAccepted && R != ESuperSLMRestoreResult::Success && R != ESuperSLMRestoreResult::Pending);
	bOk &= TestEqual(TEXT("[probe P4] the refused restore releases its slot"), FreeAfter, 1);
	bOk &= TestTrue(TEXT("[probe P4] the next sequence vended into that slot completes an ordinary generation"), bDone);
	if (Next.IsValid()) { Gpu->ReturnSequence(Next); }
	return bOk;
}

// --- P3: an adapter bind carried only by an action the thread skips (sequence already stopped) is
// reported active afterwards, and the next generation runs without it. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuAdapterBindConfirmedByAction, "SuperSLM.U1.Gpu.AdapterBindConfirmedByAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuAdapterBindConfirmedByAction::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	const FString BasePath = SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-instruct.sslm"));
	const FString AdapterPath = SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-shopkeeper-lora-v2-t2102-runtime.sslm"));
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(BasePath, Diag);
	if (!TestNotNull(TEXT("A-AD base import"), Base) || !TestTrue(TEXT("A-AD accepted"), Diag.bAccepted)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Base, 1, 28, 1024, Gpu)) { return false; }
	FSuperSLMGpuAdapterHandle GA;
	FString MapError;
	if (!TestTrue(*FString::Printf(TEXT("GPU MapAdapter (%s)"), *MapError), Gpu->MapAdapter(AdapterPath, *Base, GA, MapError))) { return false; }

	const TArray<int32> Prompt = {40, 1035, 311, 3695, 264, 2820, 60108, 13};
	FSuperSLMGenerationRequest Plain;
	Plain.PromptTokens = Prompt;
	Plain.MaxNewTokens = 12;
	Plain.SpanKind = ESuperSLMSpanKind::Prompt;

	auto RunFresh = [&](bool bWithAdapter, TArray<int32>& Out) -> bool
	{
		FSuperSLMGpuSequence S;
		Gpu->VendSequence(S, ESuperSLMGpuDecodePath::OneCall);
		if (bWithAdapter) { Gpu->RequestAdapterSwap(S, GA); }
		FString E;
		const bool b = RunGpuGenerationToCompletion(*Gpu, S, Plain, Out, 120.0, E);
		Gpu->ReturnSequence(S);
		return b;
	};
	TArray<int32> RefBase, RefAdapter;
	TestTrue(TEXT("RefBase completes"), RunFresh(false, RefBase));
	TestTrue(TEXT("RefAdapter completes"), RunFresh(true, RefAdapter));
	AddInfo(FString::Printf(TEXT("[probe P3] RefBase (%d): [%s]"), RefBase.Num(), *Join(RefBase)));
	AddInfo(FString::Printf(TEXT("[probe P3] RefAdapter (%d): [%s]"), RefAdapter.Num(), *Join(RefAdapter)));

	int32 K = INDEX_NONE;
	for (int32 I = 1; I < RefBase.Num(); ++I)
	{
		bool bSeen = false;
		for (int32 J = 0; J < I; ++J) { bSeen |= RefBase[J] == RefBase[I]; }
		if (!bSeen) { K = I; break; }
	}
	if (!TestTrue(TEXT("a stop token at index >= 1 exists"), K != INDEX_NONE)) { return false; }

	FSuperSLMGenerationRequest Stopping = Plain;
	Stopping.StopTokenIds = {RefBase[K]};
	FSuperSLMGpuSequence Seq;
	Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
	Gpu->RequestBeginGeneration(Seq, Stopping);
	// One tick per prompt token, then K+1 decode ticks: token K (the stop token) is planned on the last.
	for (int32 T = 0; T < Prompt.Num() + K + 1; ++T) { if (!PacedTick(*this, *Gpu, TEXT("P3 position at the stop token"))) { break; } }
	Gpu->RequestAdapterSwap(Seq, GA);
	const int64 ActiveRightAfterRequest = Gpu->GetActiveAdapter(Seq).Id;
	TArray<int32> First;
	FString E1;
	const bool bFirst = RunGpuGenerationToCompletion(*Gpu, Seq, Stopping, First, 120.0, E1, /*bAlreadyInProgress*/ true);
	const int64 ActiveAfterComplete = Gpu->GetActiveAdapter(Seq).Id;
	AddInfo(FString::Printf(TEXT("[probe P3] stop index %d token %d; first generation (%d): [%s] complete=%d (%s); active adapter right after request=%lld, after complete=%lld, mapped=%lld"),
		K, RefBase[K], First.Num(), *Join(First), bFirst ? 1 : 0, *E1, ActiveRightAfterRequest, ActiveAfterComplete, GA.Id));

	TestEqual(TEXT("[probe P3] reset Success"), (uint8)DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall)), (uint8)ESuperSLMRestoreResult::Success);
	TArray<int32> Second;
	FString E2;
	const bool bSecond = RunGpuGenerationToCompletion(*Gpu, Seq, Plain, Second, 120.0, E2);
	AddInfo(FString::Printf(TEXT("[probe P3] second generation (%d): [%s] complete=%d (%s); active adapter=%lld; equals RefBase=%d equals RefAdapter=%d"),
		Second.Num(), *Join(Second), bSecond ? 1 : 0, *E2, Gpu->GetActiveAdapter(Seq).Id, Second == RefBase ? 1 : 0, Second == RefAdapter ? 1 : 0));
	const int64 ActiveAfterSecond = Gpu->GetActiveAdapter(Seq).Id;
	Gpu->ReturnSequence(Seq);

	bool bOk = TestTrue(TEXT("the adapter changes this prompt's output"), RefBase != RefAdapter);
	bOk &= TestTrue(TEXT("the stop-token construction completed"), bFirst && First.Num() == K + 1);
	bOk &= TestTrue(TEXT("the swap stays pending at request time"), ActiveRightAfterRequest != GA.Id);
	bOk &= TestTrue(TEXT("the next generation completes"), bSecond);
	bOk &= TestEqual(TEXT("the next generation reports the adapter active"), ActiveAfterSecond, GA.Id);
	bOk &= TestEqual(TEXT("the next generation runs under the reported adapter"), Second, RefAdapter);
	Gpu->UnmapAdapter(GA);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuLayer1PinMismatchRefusedTest,
	"SuperSLM.U1.Gpu.Layer1PinMismatchRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuLayer1PinMismatchRefusedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = ImportAEx(*this);
	if (!TestNotNull(TEXT("A-EX import"), Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Model, 1, AExNumHiddenLayers, 4096, Gpu)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	TArray<int32> Tokens;
	FString Error;
	if (!TestTrue(*Error, RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, 60.0, Error))) { return false; }
	TArray<uint8> Blob;
	if (!TestEqual(TEXT("save succeeds"), (uint8)DriveSaveToResolution(*Gpu, Gpu->RequestSaveSequence(Seq), Blob),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
	Gpu->ReturnSequence(Seq);
	for (int32 I = 0; I < 4; ++I) { if (!PacedTick(*this, *Gpu, TEXT("pin mismatch drain after return"))) { break; } FPlatformProcess::Sleep(1.0f / 60.0f); }
	if (!TestTrue(TEXT("the saved wrapper carries a nonempty Layer-1 tag"), Blob.Num() >= 28 && Blob[12] != 0)) { return false; }
	Blob[12] = Blob[12] == 'v' ? 'x' : 'v';
	const int32 FreeBefore = Gpu->GetPoolFreeCount();
	const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(Blob, Model);
	bool bOk = TestFalse(TEXT("a blob from another Layer-1 pin is refused before queuing"), H.IsValid());
	bOk &= TestTrue(TEXT("pin mismatch is reported by name"), Gpu->GetLastLifecycleRequestError().Contains(TEXT("Layer1Mismatch")));
	bOk &= TestEqual(TEXT("pin mismatch reserves no slot"), Gpu->GetPoolFreeCount(), FreeBefore);
	return bOk;
}

// --- R-S2g arm (x), a restore across a smaller K (plan section 9 R-S2g; section 2.5 row 21's ruling,
// round 5, finding 1; kills M-66). A-EX, OneCall, BlockCount 1, Configure() at K1 = 24, both logs
// enabled before anything is vended. A save through DriveSaveToResolution() resolves with at least
// 10 produced tokens not yet observed, so at least one carried delay is >= K2 + 1 = 9 (one token per
// tick, delays distinct). The pool is then drained and SetFixedTickLatency(8) makes K2 = 8, and the
// blob is restored and run to Complete with gated ticks. The carried-delay clamp keeps every carried
// event's apply tick at most r + K2 - 1, ahead of the first fresh token's r + K2. Without it (M-66)
// the first fresh token sits behind a carried event due as late as r + 23, the walk stops before it,
// and it is applied without ever being examined, or applied out of creation order. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuRestoreAcrossSmallerK, "SuperSLM.U1.Gpu.RestoreAcrossSmallerK",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuRestoreAcrossSmallerK::RunTest(const FString& Parameters)
{
	constexpr int32 kK1 = 24;
	constexpr int32 kK2 = 8;
	constexpr int32 kMinCarriedTokens = 10; // produced - observed at the save, so a carried delay >= K2 + 1
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = ImportAEx(*this);
	if (!TestNotNull(TEXT("A-EX import"), Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!ProbeSetUpGpu(*this, W.GetTestWorld(), Model, 1, kK1, 4096, Gpu)) { return false; }
	// Both logs on before anything is vended; SetEventLogEnabled(true) refuses while an event is pending.
	if (!TestTrue(TEXT("the logs are enabled while no event is pending"), FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, true))) { return false; }

	FSuperSLMGenerationRequest Request;
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU tokenizer subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.BlockCount = 1;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU tokenizer configure"), (uint8)Cpu->Configure(Model, CpuConfig).Result,
		(uint8)ESuperSLMConfigureResult::Success) ||
		!TestTrue(TEXT("natural prompt tokenizes"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 40;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// The unsaved reference, at K1.
	TArray<int32> Reference;
	{
		FSuperSLMGpuSequence Ref;
		if (!TestEqual(TEXT("reference vend"), (uint8)Gpu->VendSequence(Ref, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		FString E;
		const bool bRefDone = RunGpuGenerationToCompletion(*Gpu, Ref, Request, Reference, 60.0, E);
		Gpu->ReturnSequence(Ref);
		if (!TestTrue(*FString::Printf(TEXT("reference completes (%s)"), *E), bRefDone)) { return false; }
	}

	// Generate at K1 and save with produced tokens in flight.
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall),
		(uint8)ESuperSLMGpuVendResult::Success)) { return false; }
	if (!TestTrue(TEXT("RequestBeginGeneration()"), Gpu->RequestBeginGeneration(Seq, Request).IsValid())) { Gpu->ReturnSequence(Seq); return false; }
	const double WaitStartSeconds = FPlatformTime::Seconds();
	while (Gpu->GetGeneratedTokens(Seq).IsEmpty() &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Complete &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Faulted &&
		FPlatformTime::Seconds() - WaitStartSeconds < 120.0)
	{
		Gpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("the save follows a delivered decode token"),
		!Gpu->GetGeneratedTokens(Seq).IsEmpty() && Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding)) { Gpu->ReturnSequence(Seq); return false; }
	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const ESuperSLMRestoreResult SaveResult = DriveSaveToResolution(*Gpu, SaveHandle, Blob);
	const TArray<int32> Pre = Gpu->GetGeneratedTokens(Seq);
	int32 ObservedAtSave = -1, ProducedAtSave = -1;
	const bool bCounts = SaveResult == ESuperSLMRestoreResult::Success && Gpu->GetSaveTokenCounts(SaveHandle, ObservedAtSave, ProducedAtSave);
	Gpu->ReturnSequence(Seq);
	if (!TestEqual(TEXT("save Success"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success) ||
		!TestTrue(TEXT("save exposes delivered and produced counts"), bCounts)) { return false; }
	AddInfo(FString::Printf(TEXT("save: observed %d, produced %d, delivered prefix %d"), ObservedAtSave, ProducedAtSave, Pre.Num()));
	if (!TestTrue(*FString::Printf(TEXT("precondition: produced - observed >= %d at the save (%d)"), kMinCarriedTokens, ProducedAtSave - ObservedAtSave),
		ProducedAtSave - ObservedAtSave >= kMinCarriedTokens)) { return false; }

	// Drain the pool, then K2 = 8.
	bool bLatencySet = false;
	FString LatencyError;
	const double DrainStartSeconds = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - DrainStartSeconds < 60.0)
	{
		if (Gpu->GetPoolOccupiedCount() == 0 && Gpu->SetFixedTickLatency(kK2, LatencyError))
		{
			bLatencySet = true;
			break;
		}
		if (NextTickGatedOnDevice(*Gpu))
		{
			FPlatformProcess::Sleep(0.001f);
			continue;
		}
		Gpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(1.0f / 60.0f);
	}
	if (!TestTrue(*FString::Printf(TEXT("the pool drains and SetFixedTickLatency(%d) succeeds (%s)"), kK2, *LatencyError), bLatencySet)) { return false; }

	// Restore at K2 and run to Complete with gated ticks.
	FSuperSLMGpuSequence Restored;
	const ESuperSLMRestoreResult RestoreResult = DriveRestoreToResolution(*Gpu, Gpu->RequestRestoreSequence(Blob, Model), Restored);
	if (!TestEqual(TEXT("restore Success"), (uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	TArray<int32> Tail;
	FString RunError;
	const bool bDone = RunGpuGenerationToCompletion(*Gpu, Restored, Request, Tail, 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Restored);

	const TArray<FSuperSLMGpuEventLogEntry> Log = FSuperSLMGpuTestAccess::GetEventLog(*Gpu);
	FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);

	// Every applied entry was examined.
	int32 AppliedUnexamined = 0;
	int32 FirstAppliedUnexamined = INDEX_NONE;
	// In creation order per SequenceId, ApplyTick never decreases.
	TMap<int64, int64> LastApplyTickBySequence;
	int32 ApplyTickDecreases = 0;
	int32 FirstApplyTickDecrease = INDEX_NONE;
	for (int32 Index = 0; Index < Log.Num(); ++Index)
	{
		const FSuperSLMGpuEventLogEntry& Entry = Log[Index];
		if (Entry.RemovedTick >= 0 && !Entry.bDropped && !Entry.bExamined && AppliedUnexamined++ == 0)
		{
			FirstAppliedUnexamined = Index;
		}
		const int64* Last = LastApplyTickBySequence.Find(Entry.SequenceId);
		if (Last != nullptr && Entry.ApplyTick < *Last && ApplyTickDecreases++ == 0)
		{
			FirstApplyTickDecrease = Index;
		}
		LastApplyTickBySequence.Add(Entry.SequenceId, Last != nullptr ? FMath::Max(*Last, Entry.ApplyTick) : Entry.ApplyTick);
	}
	TArray<int32> Concat = Pre;
	Concat.Append(Tail);
	AddInfo(FString::Printf(TEXT("arm (x): %d log entries; reference %d tokens, prefix %d + continuation %d"), Log.Num(), Reference.Num(), Pre.Num(), Tail.Num()));

	bool bOk = TestTrue(*FString::Printf(TEXT("the restored sequence completes (%s)"), *RunError), bDone);
	bOk &= TestTrue(TEXT("the event log recorded the run's events"), Log.Num() > 0);
	bOk &= TestEqual(*FString::Printf(TEXT("every event-log entry with RemovedTick >= 0 and not dropped has bExamined true (first not: entry %d)"), FirstAppliedUnexamined),
		AppliedUnexamined, 0);
	bOk &= TestEqual(*FString::Printf(TEXT("in creation order per SequenceId, ApplyTick never decreases (first decrease: entry %d)"), FirstApplyTickDecrease),
		ApplyTickDecreases, 0);
	bOk &= TestEqual(TEXT("the saved prefix plus the continuation equals the unsaved reference"), Concat, Reference);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
