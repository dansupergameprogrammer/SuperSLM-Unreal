#include "SuperSLMBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"

// L2-S3 (plan §6, §10.4). Every wrapper is a forward to USuperSLMSubsystem plus a type
// conversion; none holds state, and every one runs on the game thread the subsystem itself runs
// on. The mirrored enums convert with a static_cast, which the asserts below keep honest: a
// runtime enumerator added or moved without its Blueprint mirror fails the build here.
#define SUPERSLM_BP_MIRRORS(NativeEnum, BpEnum, Value) \
	static_assert(static_cast<uint8>(NativeEnum::Value) == static_cast<uint8>(BpEnum::Value), \
		#BpEnum "::" #Value " must keep " #NativeEnum "'s ordinal");

SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, Success)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, BackendMismatch)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, ModelMismatch)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, KvMismatch)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, ResidualLost)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, Malformed)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, NotConfigured)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, PoolExhausted)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, AdapterUnavailable)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, SequenceQueueFull)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, Pending)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, UnsupportedOnGpu)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, SaveRefused)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, Layer1Mismatch)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, Consumed)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, ResetRequired)
SUPERSLM_BP_MIRRORS(ESuperSLMRestoreResult, ESuperSLMRestoreResultBP, OutOfMemory)

SUPERSLM_BP_MIRRORS(ESuperSLMDecodeOutcome, ESuperSLMDecodeOutcomeBP, TokenProduced)
SUPERSLM_BP_MIRRORS(ESuperSLMDecodeOutcome, ESuperSLMDecodeOutcomeBP, Generating)
SUPERSLM_BP_MIRRORS(ESuperSLMDecodeOutcome, ESuperSLMDecodeOutcomeBP, SchemaDeadEnd)
SUPERSLM_BP_MIRRORS(ESuperSLMDecodeOutcome, ESuperSLMDecodeOutcomeBP, SequenceNoLongerValid)

SUPERSLM_BP_MIRRORS(ESuperSLMSequencePhase, ESuperSLMSequencePhaseBP, Idle)
SUPERSLM_BP_MIRRORS(ESuperSLMSequencePhase, ESuperSLMSequencePhaseBP, Prefilling)
SUPERSLM_BP_MIRRORS(ESuperSLMSequencePhase, ESuperSLMSequencePhaseBP, Decoding)
SUPERSLM_BP_MIRRORS(ESuperSLMSequencePhase, ESuperSLMSequencePhaseBP, Complete)
SUPERSLM_BP_MIRRORS(ESuperSLMSequencePhase, ESuperSLMSequencePhaseBP, Faulted)

SUPERSLM_BP_MIRRORS(ESuperSLMSpanKind, ESuperSLMSpanKindBP, Prompt)
SUPERSLM_BP_MIRRORS(ESuperSLMSpanKind, ESuperSLMSpanKindBP, SchemaContent)

SUPERSLM_BP_MIRRORS(ESuperSLMVendResult, ESuperSLMVendResultBP, Success)
SUPERSLM_BP_MIRRORS(ESuperSLMVendResult, ESuperSLMVendResultBP, PoolExhausted)
SUPERSLM_BP_MIRRORS(ESuperSLMVendResult, ESuperSLMVendResultBP, NotConfigured)

SUPERSLM_BP_MIRRORS(ESuperSLMPrefixPhase, ESuperSLMPrefixPhaseBP, Pending)
SUPERSLM_BP_MIRRORS(ESuperSLMPrefixPhase, ESuperSLMPrefixPhaseBP, Prefilling)
SUPERSLM_BP_MIRRORS(ESuperSLMPrefixPhase, ESuperSLMPrefixPhaseBP, Ready)
SUPERSLM_BP_MIRRORS(ESuperSLMPrefixPhase, ESuperSLMPrefixPhaseBP, Faulted)
SUPERSLM_BP_MIRRORS(ESuperSLMPrefixPhase, ESuperSLMPrefixPhaseBP, Invalid)

SUPERSLM_BP_MIRRORS(ESuperSLMSelfCheckVerdict, ESuperSLMSelfCheckVerdictBP, Verified)
SUPERSLM_BP_MIRRORS(ESuperSLMSelfCheckVerdict, ESuperSLMSelfCheckVerdictBP, Diverged)
SUPERSLM_BP_MIRRORS(ESuperSLMSelfCheckVerdict, ESuperSLMSelfCheckVerdictBP, NotYetRun)

#undef SUPERSLM_BP_MIRRORS

namespace
{
	ESuperSLMRestoreResultBP ToBP(ESuperSLMRestoreResult Result)
	{
		return static_cast<ESuperSLMRestoreResultBP>(Result);
	}

	const TCHAR* NoSubsystem()
	{
		return TEXT("no SuperSLM subsystem");
	}
}

USuperSLMSubsystem* USuperSLMBlueprintLibrary::GetSuperSLMSubsystem(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine != nullptr
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	const UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	return GameInstance != nullptr ? GameInstance->GetSubsystem<USuperSLMSubsystem>() : nullptr;
}

ESuperSLMVendResultBP USuperSLMBlueprintLibrary::VendSequence(USuperSLMSubsystem* Subsystem, FSuperSLMSequenceBP& OutSequence)
{
	OutSequence = FSuperSLMSequenceBP();
	if (Subsystem == nullptr)
	{
		return ESuperSLMVendResultBP::NotConfigured;
	}
	FSuperSLMSequence Native;
	const ESuperSLMVendResult Result = Subsystem->VendSequence(Native);
	OutSequence = FSuperSLMSequenceBP(Native);
	return static_cast<ESuperSLMVendResultBP>(Result);
}

void USuperSLMBlueprintLibrary::ReturnSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	if (Subsystem != nullptr)
	{
		Subsystem->ReturnSequence(Sequence.ToNative());
	}
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::ResetSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
	FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandleBP();
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return ESuperSLMRestoreResultBP::NotConfigured;
	}
	FSuperSLMLifecycleOpHandle Handle;
	const ESuperSLMRestoreResult Result = Subsystem->ResetSequence(Sequence.ToNative(), Handle, OutError);
	OutHandle = FSuperSLMLifecycleOpHandleBP(Handle);
	return ToBP(Result);
}

void USuperSLMBlueprintLibrary::SetLayerBudget(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence, int32 LayerBudget)
{
	if (Subsystem != nullptr)
	{
		Subsystem->SetLayerBudget(Sequence.ToNative(), LayerBudget);
	}
}

bool USuperSLMBlueprintLibrary::SetSchemaByName(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
	const USuperSLMModel* Model, const FString& SchemaName, FString& OutError)
{
	if (Subsystem == nullptr || Model == nullptr)
	{
		OutError = Subsystem == nullptr ? NoSubsystem() : TEXT("no model");
		return false;
	}
	FSuperSLMSchemaHandle Schema;
	if (!FSuperSLMSchemaLookup::LookupByName(*Model, SchemaName, Schema, OutError))
	{
		return false;
	}
	return Subsystem->SetSchema(Sequence.ToNative(), Schema, OutError);
}

void USuperSLMBlueprintLibrary::RequestAdapterSwap(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence, int64 AdapterId)
{
	if (Subsystem != nullptr)
	{
		FSuperSLMAdapterHandle Adapter;
		Adapter.Id = AdapterId;
		Subsystem->RequestAdapterSwap(Sequence.ToNative(), Adapter);
	}
}

int64 USuperSLMBlueprintLibrary::GetActiveAdapter(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	return Subsystem != nullptr ? Subsystem->GetActiveAdapter(Sequence.ToNative()).Id : 0;
}

bool USuperSLMBlueprintLibrary::BeginGeneration(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
	const FSuperSLMGenerationRequestBP& Request, FString& OutError)
{
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return false;
	}
	FSuperSLMGenerationRequest Native;
	Native.PromptTokens = Request.PromptTokens;
	Native.MaxNewTokens = Request.MaxNewTokens;
	Native.SpanKind = static_cast<ESuperSLMSpanKind>(Request.SpanKind);
	Native.StopTokenIds = Request.StopTokenIds;
	return Subsystem->BeginGeneration(Sequence.ToNative(), Native, OutError);
}

ESuperSLMSequencePhaseBP USuperSLMBlueprintLibrary::GetPhase(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	return Subsystem != nullptr
		? static_cast<ESuperSLMSequencePhaseBP>(Subsystem->GetPhase(Sequence.ToNative()))
		: ESuperSLMSequencePhaseBP::Faulted;
}

TArray<int32> USuperSLMBlueprintLibrary::GetGeneratedTokens(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	return Subsystem != nullptr ? Subsystem->GetGeneratedTokens(Sequence.ToNative()) : TArray<int32>();
}

ESuperSLMDecodeOutcomeBP USuperSLMBlueprintLibrary::GetLastDecodeOutcome(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	return Subsystem != nullptr
		? static_cast<ESuperSLMDecodeOutcomeBP>(Subsystem->GetLastDecodeOutcome(Sequence.ToNative()))
		: ESuperSLMDecodeOutcomeBP::SequenceNoLongerValid;
}

FSuperSLMSequenceStatsBP USuperSLMBlueprintLibrary::GetStats(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence)
{
	FSuperSLMSequenceStatsBP Out;
	if (Subsystem != nullptr)
	{
		const FSuperSLMSequenceStats Stats = Subsystem->GetStats(Sequence.ToNative());
		Out.DecodeStepCeiling = Stats.DecodeStepCeiling;
		Out.DecodeStepActual = Stats.DecodeStepActual;
		Out.ForcedTokenCount = Stats.ForcedTokenCount;
		Out.KvBlocksResident = Stats.KvBlocksResident;
		Out.bSchemaAccepting = Stats.bSchemaAccepting;
	}
	return Out;
}

bool USuperSLMBlueprintLibrary::Tokenize(const USuperSLMSubsystem* Subsystem, const FString& Utf8Text, TArray<int32>& OutTokens)
{
	OutTokens.Reset();
	return Subsystem != nullptr && Subsystem->Tokenize(Utf8Text, OutTokens);
}

bool USuperSLMBlueprintLibrary::Detokenize(const USuperSLMModel* Model, const TArray<int32>& Tokens, FString& OutText)
{
	OutText.Reset();
	if (Model == nullptr)
	{
		return false;
	}
	FString Error;
	if (!SuperSLM::DetokenizeTokens(*Model, Tokens, OutText, Error))
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("Detokenize: %s"), *Error);
		return false;
	}
	return true;
}

void USuperSLMBlueprintLibrary::Tick(USuperSLMSubsystem* Subsystem, float DeltaSeconds)
{
	if (Subsystem != nullptr)
	{
		Subsystem->Tick(DeltaSeconds);
	}
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::SaveSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
	FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandleBP();
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return ESuperSLMRestoreResultBP::NotConfigured;
	}
	FSuperSLMLifecycleOpHandle Handle;
	const ESuperSLMRestoreResult Result = Subsystem->SaveSequence(Sequence.ToNative(), Handle, OutError);
	OutHandle = FSuperSLMLifecycleOpHandleBP(Handle);
	return ToBP(Result);
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::RestoreSequence(USuperSLMSubsystem* Subsystem, const TArray<uint8>& Blob,
	USuperSLMModel* ExpectedModel, FSuperSLMSequenceBP& OutSequence, FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError)
{
	OutSequence = FSuperSLMSequenceBP();
	OutHandle = FSuperSLMLifecycleOpHandleBP();
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return ESuperSLMRestoreResultBP::NotConfigured;
	}
	FSuperSLMSequence Sequence;
	FSuperSLMLifecycleOpHandle Handle;
	const ESuperSLMRestoreResult Result = Subsystem->RestoreSequence(Blob, ExpectedModel, Sequence, Handle, OutError);
	OutSequence = FSuperSLMSequenceBP(Sequence);
	OutHandle = FSuperSLMLifecycleOpHandleBP(Handle);
	return ToBP(Result);
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::GetLifecycleOpResult(const USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle)
{
	return Subsystem != nullptr ? ToBP(Subsystem->GetLifecycleOpResult(Handle.ToNative())) : ESuperSLMRestoreResultBP::NotConfigured;
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::GetSaveResult(USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle,
	TArray<uint8>& OutBlob)
{
	OutBlob.Reset();
	return Subsystem != nullptr ? ToBP(Subsystem->GetSaveResult(Handle.ToNative(), OutBlob)) : ESuperSLMRestoreResultBP::NotConfigured;
}

bool USuperSLMBlueprintLibrary::ReleaseLifecycleOpHandle(USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle)
{
	return Subsystem != nullptr && Subsystem->ReleaseLifecycleOpHandle(Handle.ToNative());
}

bool USuperSLMBlueprintLibrary::CreatePrefix(USuperSLMSubsystem* Subsystem, const TArray<int32>& Tokens, FSuperSLMPrefixBP& OutPrefix, FString& OutError)
{
	OutPrefix = FSuperSLMPrefixBP();
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return false;
	}
	FSuperSLMPrefix Prefix;
	const bool bOk = Subsystem->CreatePrefix(Tokens, Prefix, OutError);
	OutPrefix = FSuperSLMPrefixBP(Prefix);
	return bOk;
}

ESuperSLMPrefixPhaseBP USuperSLMBlueprintLibrary::GetPrefixPhase(const USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix)
{
	return Subsystem != nullptr
		? static_cast<ESuperSLMPrefixPhaseBP>(Subsystem->GetPrefixPhase(Prefix.ToNative()))
		: ESuperSLMPrefixPhaseBP::Invalid;
}

bool USuperSLMBlueprintLibrary::IsPrefixReady(const USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix)
{
	return Subsystem != nullptr && Subsystem->IsPrefixReady(Prefix.ToNative());
}

ESuperSLMRestoreResultBP USuperSLMBlueprintLibrary::AdoptPrefix(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
	const FSuperSLMPrefixBP& Prefix, FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError)
{
	OutHandle = FSuperSLMLifecycleOpHandleBP();
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return ESuperSLMRestoreResultBP::NotConfigured;
	}
	FSuperSLMLifecycleOpHandle Handle;
	const ESuperSLMRestoreResult Result = Subsystem->AdoptPrefix(Sequence.ToNative(), Prefix.ToNative(), Handle, OutError);
	OutHandle = FSuperSLMLifecycleOpHandleBP(Handle);
	return ToBP(Result);
}

bool USuperSLMBlueprintLibrary::ReleasePrefix(USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix, FString& OutError)
{
	if (Subsystem == nullptr)
	{
		OutError = NoSubsystem();
		return false;
	}
	return Subsystem->ReleasePrefix(Prefix.ToNative(), OutError);
}

FSuperSLMSelfCheckReportBP USuperSLMBlueprintLibrary::ToSelfCheckReportBP(const FSuperSLMSelfCheckReport& Report)
{
	FSuperSLMSelfCheckReportBP Out;
	auto Displayed = [](const FSuperSLMSelfCheckBackendResult& Backend)
	{
		return Backend.bQuarantined ? ESuperSLMSelfCheckVerdictBP::NotYetRun : static_cast<ESuperSLMSelfCheckVerdictBP>(Backend.Verdict);
	};
	Out.CpuVerdict = Displayed(Report.Cpu);
	Out.GpuVerdict = Displayed(Report.Gpu);
	// One scope string per report on this struct: each backend's own text, labelled, and a
	// withheld verdict's text replaced by the statement that it is withheld (recorded, never
	// displayed -- the text names the verdict, so it is withheld with it).
	auto Scope = [](const TCHAR* Label, const FSuperSLMSelfCheckBackendResult& Backend)
	{
		return Backend.bQuarantined
			? FString::Printf(TEXT("%s: verdict withheld (recorded in the report, not displayed; in 1.0 the GPU verdict is always withheld)"), Label)
			: FString::Printf(TEXT("%s: %s"), Label, *Backend.ScopeText);
	};
	Out.ScopeText = Scope(TEXT("CPU"), Report.Cpu) + TEXT("\n") + Scope(TEXT("GPU"), Report.Gpu);
	return Out;
}

double USuperSLMBlueprintLibrary::GetLastTickDurationMs(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetLastTickDurationMs() : 0.0;
}

int32 USuperSLMBlueprintLibrary::GetHitchCount(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetHitchCount() : 0;
}

int32 USuperSLMBlueprintLibrary::GetPoolFreeCount(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetPoolFreeCount() : 0;
}

int32 USuperSLMBlueprintLibrary::GetPoolOccupiedCount(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetPoolOccupiedCount() : 0;
}

int64 USuperSLMBlueprintLibrary::GetKvPoolReservedBytes(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetKvPoolReservedBytes() : 0;
}

int64 USuperSLMBlueprintLibrary::GetWorkspaceReservedBytes(const USuperSLMSubsystem* Subsystem)
{
	return Subsystem != nullptr ? Subsystem->GetWorkspaceReservedBytes() : 0;
}
