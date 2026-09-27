// D-SLM7939, behaviour 2 (plan §10.5.1 steps 3c and 6b): a GPU restore that ran out of GPU memory reads OutOfMemory, not
// Malformed. sslm_gpu_seq_restore returns SSLM_GPU_ALLOCATION_FAILED for it, which is retryable
// (gpu_1p0.h:408-415); before the fix the restore switch sent it to Malformed through its default
// arm.
//
// The fix: the restore's status-to-result mapping is one function,
// SuperSLMGpuStatusMapping::ToRestoreFailureResult(SslmGpuStatus, bool bSchemaCrossCheckFailed),
// which FinalizeRestoreOp() calls and FSuperSLMGpuTestAccess::RestoreFailureResult() exposes, so
// this cell checks it without a device. SSLM_GPU_ALLOCATION_FAILED -> OutOfMemory is added; every
// existing arm is unchanged, and a failed schema cross-check still reads Malformed.
//
// Red first at the fix's first commit (the values and the seam, behaviour unchanged): there the
// function carries only the old arms, so the SSLM_GPU_ALLOCATION_FAILED row reads Malformed (5)
// against OutOfMemory (16). Every other row is green there. Its mutant sends
// SSLM_GPU_ALLOCATION_FAILED back to the default arm, with the same red.
//
// The second cell, RefusedRestoreReadsMappedResult, pins the call site: a real restore that Layer 1
// refuses reads the mapping's result. Layer 1 cannot be made to run out of GPU memory on demand, so
// it uses the refusal a damaged save gives: one corrupted byte of the blob's Layer-1 'SLM5' magic,
// which Layer 1 refuses SSLM_SEQUENCE_KV_BUFFER_MISMATCH (the malformed-blob status, gpu_1p0.h), and
// the mapping reads KvMismatch. A restore path that bypassed the mapping with a fixed Malformed would
// read Malformed. It is green before and after the fix; its red is that bypass.
//
// The third cell, RestoreAllocationFailureReadsOutOfMemory, runs the claim end to end: a real
// restore of an intact save, with SetNextRestoreStatusOverride() making Layer 1's restore return
// SSLM_GPU_ALLOCATION_FAILED, resolves OutOfMemory through FinalizeRestoreOp()'s real failure path.
// With the fix's first commit's mapping (no SSLM_GPU_ALLOCATION_FAILED arm) it reads Malformed.
// The override is one-shot, so the same blob then restores Success.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "Tests/AutomationCommon.h"

// Layer 1's status enum (vendored C++), for the enumerators the mapping takes.
THIRD_PARTY_INCLUDES_START
#include "superslm/gpu_1p0.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	struct FRestoreStatusRow
	{
		SslmGpuStatus Status;
		const TCHAR* StatusName;
		ESuperSLMRestoreResult Expected;
		bool bSchemaCrossCheckFailed = false;
	};

	bool CheckStatus(FAutomationTestBase& T, const FRestoreStatusRow& Row)
	{
		const ESuperSLMRestoreResult Actual = FSuperSLMGpuTestAccess::RestoreFailureResult(Row.Status, Row.bSchemaCrossCheckFailed);
		return T.TestEqual(*FString::Printf(TEXT("a restore that returned %s%s reads result %d"), Row.StatusName,
				Row.bSchemaCrossCheckFailed ? TEXT(" with a failed schema cross-check") : TEXT(""), (int32)(uint8)Row.Expected),
			(int32)(uint8)Actual, (int32)(uint8)Row.Expected);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuRestoreOutOfMemoryReadsOutOfMemoryTest,
	"SuperSLM.U1.Gpu.RestoreOutOfMemoryReadsOutOfMemory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRestoreOutOfMemoryReadsOutOfMemoryTest::RunTest(const FString& Parameters)
{
	// The new values' numbers (the record §10, "The new result values"): appended after Consumed,
	// ResetRequired then OutOfMemory, so every earlier value keeps its number.
	bool bOk = TestEqual(TEXT("ResetRequired is appended directly after Consumed"),
		(int32)(uint8)ESuperSLMRestoreResult::ResetRequired, (int32)(uint8)ESuperSLMRestoreResult::Consumed + 1);
	bOk &= TestEqual(TEXT("OutOfMemory follows ResetRequired"),
		(int32)(uint8)ESuperSLMRestoreResult::OutOfMemory, (int32)(uint8)ESuperSLMRestoreResult::ResetRequired + 1);

	// The claim: SSLM_GPU_ALLOCATION_FAILED reads OutOfMemory.
	bOk &= CheckStatus(*this, {SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED, TEXT("SSLM_GPU_ALLOCATION_FAILED"), ESuperSLMRestoreResult::OutOfMemory});

	// Every other non-OK status keeps the result it had before the fix: the two named arms, and Malformed
	// through the default arm. SSLM_DEVICE_LOST stays Malformed (the record §10, "Not changed": a
	// lost device during restore keeps today's result). The list is every SslmGpuStatus enumerator
	// but SSLM_OK in the vendored gpu_1p0.h at this pin; SSLM_OK never reaches the mapping.
	static const FRestoreStatusRow ExistingRows[] = {
		{SslmGpuStatus::SSLM_RESTORE_MODEL_MISMATCH,      TEXT("SSLM_RESTORE_MODEL_MISMATCH"),      ESuperSLMRestoreResult::ModelMismatch},
		{SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH, TEXT("SSLM_SEQUENCE_KV_BUFFER_MISMATCH"), ESuperSLMRestoreResult::KvMismatch},
		{SslmGpuStatus::SSLM_DEVICE_LOST,                 TEXT("SSLM_DEVICE_LOST"),                 ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_DISPATCH_BUDGET_TOO_SMALL,   TEXT("SSLM_DISPATCH_BUDGET_TOO_SMALL"),   ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_BUSY,                        TEXT("SSLM_BUSY"),                        ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_CONTEXT_HAS_LIVE_HANDLES,    TEXT("SSLM_CONTEXT_HAS_LIVE_HANDLES"),    ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_MODEL_HAS_LIVE_SEQUENCES,    TEXT("SSLM_MODEL_HAS_LIVE_SEQUENCES"),    ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_ADAPTER_MODEL_MISMATCH,      TEXT("SSLM_ADAPTER_MODEL_MISMATCH"),      ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_ADAPTER_BASE_HASH_MISMATCH,  TEXT("SSLM_ADAPTER_BASE_HASH_MISMATCH"),  ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_BATCH_BUDGET_EXHAUSTED,      TEXT("SSLM_BATCH_BUDGET_EXHAUSTED"),      ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_TOKEN_ID_OUT_OF_RANGE,       TEXT("SSLM_TOKEN_ID_OUT_OF_RANGE"),       ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_SEQUENCE_REJECTED,           TEXT("SSLM_SEQUENCE_REJECTED"),           ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_MODEL_HAS_LIVE_ADAPTERS,     TEXT("SSLM_MODEL_HAS_LIVE_ADAPTERS"),     ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_ADAPTER_HAS_BOUND_SEQUENCES, TEXT("SSLM_ADAPTER_HAS_BOUND_SEQUENCES"), ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_SHADER_BINARY_STALE,     TEXT("SSLM_GPU_SHADER_BINARY_STALE"),     ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_OUTPUT_BUFFER_TOO_SMALL,     TEXT("SSLM_OUTPUT_BUFFER_TOO_SMALL"),     ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_PREFILL_HIDDEN_UNAVAILABLE,  TEXT("SSLM_PREFILL_HIDDEN_UNAVAILABLE"),  ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_SHADER_DIR_INVALID,      TEXT("SSLM_GPU_SHADER_DIR_INVALID"),      ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_SHADER_DIR_CONFLICT,     TEXT("SSLM_GPU_SHADER_DIR_CONFLICT"),     ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_PARALLEL_FOR_INVALID,    TEXT("SSLM_GPU_PARALLEL_FOR_INVALID"),    ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_PARALLEL_FOR_INCOMPLETE, TEXT("SSLM_GPU_PARALLEL_FOR_INCOMPLETE"), ESuperSLMRestoreResult::Malformed},
		{SslmGpuStatus::SSLM_GPU_RESIDENCY_FLAGS_INVALID, TEXT("SSLM_GPU_RESIDENCY_FLAGS_INVALID"), ESuperSLMRestoreResult::Malformed},
	};
	for (const FRestoreStatusRow& Row : ExistingRows)
	{
		bOk &= CheckStatus(*this, Row);
	}

	// A failed schema cross-check (plan §2.5 row 5) reads Malformed whatever status it carries,
	// the out-of-memory status included.
	bOk &= CheckStatus(*this, {SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED, TEXT("SSLM_GPU_ALLOCATION_FAILED"), ESuperSLMRestoreResult::Malformed, true});
	bOk &= CheckStatus(*this, {SslmGpuStatus::SSLM_RESTORE_MODEL_MISMATCH, TEXT("SSLM_RESTORE_MODEL_MISMATCH"), ESuperSLMRestoreResult::Malformed, true});
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuRefusedRestoreReadsMappedResultTest,
	"SuperSLM.U1.Gpu.RefusedRestoreReadsMappedResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRefusedRestoreReadsMappedResultTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLML2S2Fixtures;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
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
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	// A real save, as SuperSLM.U1.Gpu.VendAfterFailedRestore takes it.
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	Gpu->RequestBeginGeneration(Seq, Request);
	for (int32 T = 0; T < 6; ++T)
	{
		PacedTick(*this, *Gpu, TEXT("position before the save"));
	}
	TArray<uint8> Blob;
	const ESuperSLMRestoreResult SaveResult = DriveSaveToResolution(*Gpu, Gpu->RequestSaveSequence(Seq), Blob);
	Gpu->ReturnSequence(Seq);
	for (int32 T = 0; T < 4; ++T)
	{
		PacedTick(*this, *Gpu, TEXT("drain after the return"));
	}
	if (!TestEqual(TEXT("the save succeeds"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	int32 MagicAt = INDEX_NONE;
	for (int32 I = 0; I + 3 < Blob.Num(); ++I)
	{
		if (Blob[I] == 0x53 && Blob[I + 1] == 0x4C && Blob[I + 2] == 0x4D && Blob[I + 3] == 0x35) // 'SLM5'
		{
			MagicAt = I;
			break;
		}
	}
	if (!TestTrue(TEXT("the Layer-1 'SLM5' header is present in the blob"), MagicAt != INDEX_NONE))
	{
		return false;
	}
	Blob[MagicAt] ^= 0xFF;

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(Blob, Model);
	const ESuperSLMRestoreResult Result = DriveRestoreToResolution(*Gpu, H, Restored);
	const FString Error = Gpu->GetLastLifecycleRequestError();
	if (Restored.IsValid())
	{
		Gpu->ReturnSequence(Restored);
	}
	const ESuperSLMRestoreResult Mapped = FSuperSLMGpuTestAccess::RestoreFailureResult(SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH, false);
	AddInfo(FString::Printf(TEXT("refused restore: result %d, mapping of SSLM_SEQUENCE_KV_BUFFER_MISMATCH %d, error '%s'"), (int32)Result, (int32)Mapped, *Error));
	bool bOk = TestTrue(TEXT("the restore request is accepted (the wrapper is intact; Layer 1 refuses)"), H.IsValid());
	bOk &= TestTrue(*FString::Printf(TEXT("Layer 1 refused with SSLM_SEQUENCE_KV_BUFFER_MISMATCH, as the error names (got: '%s')"), *Error),
		Error.Contains(TEXT("SSLM_SEQUENCE_KV_BUFFER_MISMATCH")));
	bOk &= TestEqual(TEXT("the refused restore reads the mapping's result for that status (KvMismatch), not a fixed Malformed"),
		(int32)(uint8)Result, (int32)(uint8)Mapped);
	bOk &= TestEqual(TEXT("the mapping's result for that status is KvMismatch"), (int32)(uint8)Mapped, (int32)(uint8)ESuperSLMRestoreResult::KvMismatch);
	return bOk;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuRestoreAllocationFailureReadsOutOfMemoryTest,
	"SuperSLM.U1.Gpu.RestoreAllocationFailureReadsOutOfMemory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuRestoreAllocationFailureReadsOutOfMemoryTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLML2S2Fixtures;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
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
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	// An intact save, taken as RefusedRestoreReadsMappedResult takes it.
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	Gpu->RequestBeginGeneration(Seq, Request);
	for (int32 T = 0; T < 6; ++T)
	{
		PacedTick(*this, *Gpu, TEXT("position before the save"));
	}
	TArray<uint8> Blob;
	const ESuperSLMRestoreResult SaveResult = DriveSaveToResolution(*Gpu, Gpu->RequestSaveSequence(Seq), Blob);
	Gpu->ReturnSequence(Seq);
	for (int32 T = 0; T < 4; ++T)
	{
		PacedTick(*this, *Gpu, TEXT("drain after the return"));
	}
	if (!TestEqual(TEXT("the save succeeds"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	// Armed just before the restore it is meant for; the next sslm_gpu_seq_restore returns it.
	FSuperSLMGpuTestAccess::SetNextRestoreStatusOverride(*Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED);
	FSuperSLMGpuSequence Refused;
	const FSuperSLMLifecycleOpHandle H = Gpu->RequestRestoreSequence(Blob, Model);
	const ESuperSLMRestoreResult Result = DriveRestoreToResolution(*Gpu, H, Refused);
	const FString Error = Gpu->GetLastLifecycleRequestError();
	if (Refused.IsValid())
	{
		Gpu->ReturnSequence(Refused);
	}
	AddInfo(FString::Printf(TEXT("restore out of GPU memory: result %d, error '%s'"), (int32)Result, *Error));
	bool bOk = TestTrue(TEXT("the restore request is accepted"), H.IsValid());
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names SSLM_GPU_ALLOCATION_FAILED (got: '%s')"), *Error),
		Error.Contains(TEXT("SSLM_GPU_ALLOCATION_FAILED")));
	bOk &= TestEqual(TEXT("a restore that ran out of GPU memory reads OutOfMemory end to end, not Malformed"),
		(int32)(uint8)Result, (int32)(uint8)ESuperSLMRestoreResult::OutOfMemory);
	bOk &= TestFalse(TEXT("the refused restore gives no sequence"), Refused.IsValid());

	// The override was one-shot, and the blob is intact: the same restore now succeeds.
	FSuperSLMGpuSequence Restored;
	const ESuperSLMRestoreResult Retry = DriveRestoreToResolution(*Gpu, Gpu->RequestRestoreSequence(Blob, Model), Restored);
	bOk &= TestEqual(TEXT("the same blob restores Success once the override is spent (out of memory is retryable)"),
		(uint8)Retry, (uint8)ESuperSLMRestoreResult::Success);
	if (Restored.IsValid())
	{
		Gpu->ReturnSequence(Restored);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
