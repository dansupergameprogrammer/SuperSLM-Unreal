// A faulted GPU sequence refuses a new generation until it is reset. gpu_1p0.h:233-237 requires a
// reset (or a restore, which gives a new sequence) before a faulted sequence's next generation;
// the plugin applies that rule to every fault.
//
// A sequence runs one generation at a time: RequestBeginGeneration() is refused at the call --
// an invalid handle, GetLastLifecycleRequestError() naming the rule -- unless the sequence will be
// Idle when the request's turn comes. A Faulted sequence with nothing queued will not be, so the
// request is refused at the call, nothing is queued, and nothing on the sequence changes.
//
// Red where the plugin has no call-time check (before the one-generation rule): the request is
// accepted and admitted, so "the generation on a faulted sequence is refused at the call (invalid
// handle)" reads a valid handle, and the cell returns there.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMJson.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// Small enough that a prompt over it faults in about a hundred one-call ticks; large enough
	// that the pinned prompt plus the anchor's 32 tokens fits (checked below, not assumed).
	constexpr int64 kFaultedSlotContextCap = 128;
	// Over the cap by 8, so the embed after position 128 is refused (D-SLM7379's per-embed check).
	constexpr int32 kOverCapPromptTokens = 136;
	// The anchor's own --max-new 32 (T-2826 build log §3.1; SliceInvariance's AgainstSslmGenerateAnchor).
	constexpr int32 kAnchorMaxNewTokens = 32;

	// The shipped reference file's CPU entry: Layer 1's own sslm_generate tokens for the pinned
	// prompt, and their digest. Either CPU entry carries the same tokens (the layer budget does not
	// change CPU bits), so the first one with tokens is taken, as AgainstSslmGenerateAnchor does.
	bool LoadShippedAnchor(FAutomationTestBase& T, TArray<int32>& OutTokens, FString& OutDigestHex)
	{
		FString RefPath, RefReason;
		if (!T.TestTrue(*FString::Printf(TEXT("the shipped reference file must exist (%s)"), *RefReason), TryGetReferenceDigestFilePath(RefPath, RefReason)))
		{
			return false;
		}
		FString RefJson;
		if (!T.TestTrue(*FString::Printf(TEXT("reading '%s' must succeed"), *RefPath), FFileHelper::LoadFileToString(RefJson, *RefPath)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		FString ReadError;
		if (!T.TestTrue(*FString::Printf(TEXT("the reference file must be a JSON object (%s)"), *ReadError), SuperSLMJson::TryReadObject(RefJson, Root, ReadError) && Root.IsValid()))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!T.TestTrue(TEXT("the reference file must carry an 'entries' array"), Root->TryGetArrayField(TEXT("entries"), Entries) && Entries != nullptr))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			if (!EntryValue.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
			if (!Entry.IsValid())
			{
				continue;
			}
			FString Backend;
			const TArray<TSharedPtr<FJsonValue>>* Tokens = nullptr;
			if (!Entry->TryGetStringField(TEXT("backend"), Backend) || Backend != TEXT("CPU") ||
				!Entry->TryGetArrayField(TEXT("tokens"), Tokens) || Tokens == nullptr || Tokens->Num() == 0 ||
				!Entry->TryGetStringField(TEXT("tokenDigestHex"), OutDigestHex))
			{
				continue;
			}
			OutTokens.Reset();
			for (const TSharedPtr<FJsonValue>& TokenValue : *Tokens)
			{
				OutTokens.Add(static_cast<int32>(TokenValue->AsNumber()));
			}
			return true;
		}
		return T.TestTrue(TEXT("a CPU entry with 'tokens' and 'tokenDigestHex' must be found in the reference file"), false);
	}

	FString DigestHexOf(const TArray<int32>& Tokens)
	{
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		return SuperSLMGpuDigest::DigestToHex(Digest);
	}

	// One-call generation of the pinned prompt on a fresh sequence to completion; the sequence is
	// returned before this returns.
	bool RunPinnedPromptOnFreshSequence(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGenerationRequest& Request, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!T.TestEqual(TEXT("VendSequence() (control)"), (uint8)Gpu.VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString RunError;
		const bool bCompleted = RunGpuGenerationToCompletion(Gpu, Seq, Request, OutTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
		Gpu.ReturnSequence(Seq);
		return T.TestTrue(*FString::Printf(TEXT("the control generation must complete (%s)"), *RunError), bCompleted);
	}
}

// Fault a slot with a prompt over the context cap. The next generation on it is refused at the
// call, names the rule, and changes nothing on the slot. Reset, then generate the reference prompt:
// it is admitted, and its digest matches the shipped reference.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuFaultedSlotRefusesGenerationUntilResetTest,
	"SuperSLM.U1.Gpu.FaultedSlotRefusesGenerationUntilReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuFaultedSlotRefusesGenerationUntilResetTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	// --- The reference: the shipped sslm_generate anchor for the pinned prompt ---
	TArray<int32> AnchorTokens;
	FString AnchorDigestHex;
	if (!LoadShippedAnchor(*this, AnchorTokens, AnchorDigestHex))
	{
		return false;
	}

	// --- Configure both backends: the GPU under test, and the CPU for the tokenizer ---
	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = kFaultedSlotContextCap;
	Config.BlockCount = 2;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // one-call path; a whole token's worth
	Config.K = AExNumHiddenLayers; // one-call MinimumK = BlockCount (2); generous, K is not this cell's subject
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = GetSubsystem(World);
	if (!TestNotNull(TEXT("a CPU subsystem must be reachable to tokenize the pinned prompt"), Cpu))
	{
		return false;
	}
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.BlockCount = 1;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU Configure()"), (uint8)Cpu->Configure(Model, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest PinnedRequest;
	if (!TestTrue(TEXT("Tokenize() of the pinned prompt must succeed"), Cpu->Tokenize(PinnedSelfCheckPrompt(), PinnedRequest.PromptTokens)))
	{
		return false;
	}
	PinnedRequest.MaxNewTokens = kAnchorMaxNewTokens;
	PinnedRequest.SpanKind = ESuperSLMSpanKind::Prompt;
	if (!TestTrue(*FString::Printf(TEXT("the pinned prompt (%d tokens) plus %d new tokens must fit the cell's context cap (%lld)"),
			PinnedRequest.PromptTokens.Num(), kAnchorMaxNewTokens, kFaultedSlotContextCap),
			PinnedRequest.PromptTokens.Num() + kAnchorMaxNewTokens <= kFaultedSlotContextCap))
	{
		return false;
	}

	// --- Control: the reference prompt on a fresh sequence in this same configuration. It
	// separates "this cap changes the tokens" (the control misses the anchor too) from "the reset
	// did not restore the slot" (only the post-reset run misses). ---
	TArray<int32> ControlTokens;
	if (!RunPinnedPromptOnFreshSequence(*this, *Gpu, PinnedRequest, ControlTokens))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("control: a fresh sequence's tokens equal the shipped sslm_generate anchor at this context cap"), ControlTokens, AnchorTokens);

	// --- Fault the slot: a prompt over the context cap (the construction of
	// SuperSLM.L2S2.RecoverableRejection.LongPromptRecoversAtContextCap) ---
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest OverCapRequest;
	OverCapRequest.PromptTokens.Init(1, kOverCapPromptTokens);
	OverCapRequest.MaxNewTokens = 8;
	OverCapRequest.SpanKind = ESuperSLMSpanKind::Prompt;
	const FSuperSLMLifecycleOpHandle OverCapBegin = Gpu->RequestBeginGeneration(Seq, OverCapRequest);
	if (!TestTrue(*FString::Printf(TEXT("the over-cap generation is accepted (no upfront ceiling refusal, D-SLM7379) (%s)"), *Gpu->GetLastLifecycleRequestError()),
			OverCapBegin.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	const double FaultStart = FPlatformTime::Seconds();
	while (Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() - FaultStart < 60.0)
	{
		if (NextTickGatedOnDevice(*Gpu))
		{
			FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
			continue;
		}
		Gpu->Tick(1.0f / 60.0f);
	}
	const ESuperSLMGpuFaultReason FaultReason = Gpu->GetLastFaultReason(Seq);
	if (!TestEqual(TEXT("the over-cap prompt faults the sequence"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted) ||
		!TestEqual(TEXT("the context-cap fault is the recoverable per-sequence rejection"), (uint8)FaultReason, (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	const TArray<int32> GeneratedAtFault = Gpu->GetGeneratedTokens(Seq);
	const ESuperSLMDecodeOutcome OutcomeAtFault = Gpu->GetLastDecodeOutcome(Seq);

	// --- The claim: the next generation on the faulted slot, with no reset between and nothing
	// queued, is refused at the call by name and changes nothing on the slot. ---
	const FSuperSLMLifecycleOpHandle RefusedBegin = Gpu->RequestBeginGeneration(Seq, PinnedRequest);
	const FString RefusalText = Gpu->GetLastLifecycleRequestError();
	AddInfo(FString::Printf(TEXT("begin on the faulted slot: handle %lld, phase %d, error '%s'"),
		RefusedBegin.Id, (int32)Gpu->GetPhase(Seq), *RefusalText));
	if (!TestFalse(TEXT("the generation on a faulted sequence is refused at the call (invalid handle)"), RefusedBegin.IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		return false;
	}
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the rule, the phase Faulted, the reset and the restore (got: '%s')"), *RefusalText),
		RefusalText.Contains(TEXT("one generation at a time")) &&
		RefusalText.Contains(TEXT("Faulted")) &&
		RefusalText.Contains(TEXT("RequestResetSequence")) &&
		RefusalText.Contains(TEXT("restore")));
	bOk &= TestFalse(*FString::Printf(TEXT("the refusal does not read as the queue bound (got: '%s')"), *RefusalText),
		RefusalText.Contains(TEXT("configured bound")) || RefusalText.Contains(TEXT("already full")) || RefusalText.Contains(TEXT("SSLM_SEQUENCE_QUEUE_FULL")));
	bOk &= TestEqual(TEXT("the refused generation leaves the phase Faulted"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the refused generation leaves the generated tokens as they were"), Gpu->GetGeneratedTokens(Seq), GeneratedAtFault);
	bOk &= TestEqual(TEXT("the refused generation leaves the last decode outcome as it was"), (uint8)Gpu->GetLastDecodeOutcome(Seq), (uint8)OutcomeAtFault);
	bOk &= TestEqual(TEXT("the refused generation leaves the fault reason as it was"), (uint8)Gpu->GetLastFaultReason(Seq), (uint8)FaultReason);
	bOk &= TestEqual(TEXT("the refused generation queues nothing (pending count)"), Gpu->GetPendingLifecycleOperationCount(Seq), 0);

	// A few ticks later nothing has started either: a refused request must not be admitted late.
	for (int32 T = 0; T < 8; ++T)
	{
		if (!PacedTick(*this, *Gpu, TEXT("after the refused generation")))
		{
			break;
		}
	}
	bOk &= TestEqual(TEXT("eight ticks after the refusal the phase is still Faulted"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("eight ticks after the refusal the generated tokens are unchanged"), Gpu->GetGeneratedTokens(Seq), GeneratedAtFault);
	bOk &= TestEqual(TEXT("eight ticks after the refusal the last decode outcome is unchanged"), (uint8)Gpu->GetLastDecodeOutcome(Seq), (uint8)OutcomeAtFault);
	bOk &= TestEqual(TEXT("eight ticks after the refusal the fault reason is unchanged"), (uint8)Gpu->GetLastFaultReason(Seq), (uint8)FaultReason);
	bOk &= TestEqual(TEXT("eight ticks after the refusal nothing is pending"), Gpu->GetPendingLifecycleOperationCount(Seq), 0);

	// --- Reset, then the reference prompt: admitted, and its digest matches the reference ---
	const ESuperSLMRestoreResult ResetResult = DriveLifecycleOpToResolution(*Gpu, Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall));
	bOk &= TestEqual(TEXT("the reset of the faulted slot succeeds"), (uint8)ResetResult, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the reset returns the slot to Idle"), (uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Idle);

	const FSuperSLMLifecycleOpHandle AdmittedBegin = Gpu->RequestBeginGeneration(Seq, PinnedRequest);
	bOk &= TestTrue(*FString::Printf(TEXT("the generation after the reset is queued (%s)"), *Gpu->GetLastLifecycleRequestError()), AdmittedBegin.IsValid());
	const ESuperSLMRestoreResult AdmittedResult = DriveLifecycleOpToResolution(*Gpu, AdmittedBegin);
	bOk &= TestEqual(TEXT("the generation after the reset is admitted (Success)"), (uint8)AdmittedResult, (uint8)ESuperSLMRestoreResult::Success);

	TArray<int32> PostResetTokens;
	FString RunError;
	const bool bCompleted = AdmittedBegin.IsValid() &&
		RunGpuGenerationToCompletion(*Gpu, Seq, PinnedRequest, PostResetTokens, /*MaxWallClockSeconds*/ 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Seq);
	bOk &= TestTrue(*FString::Printf(TEXT("the generation after the reset completes (%s)"), *RunError), bCompleted);

	const FString PostResetDigestHex = DigestHexOf(PostResetTokens);
	AddInfo(FString::Printf(TEXT("post-reset digest %s, anchor digest %s, control digest %s (%d tokens)"),
		*PostResetDigestHex, *AnchorDigestHex, *DigestHexOf(ControlTokens), PostResetTokens.Num()));
	bOk &= TestTrue(*FString::Printf(TEXT("the post-reset generation's digest matches the shipped reference (%s vs %s)"), *PostResetDigestHex, *AnchorDigestHex),
		PostResetDigestHex.Equals(AnchorDigestHex, ESearchCase::IgnoreCase));
	bOk &= TestEqual(TEXT("the post-reset generation's tokens equal the control's"), PostResetTokens, ControlTokens);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
