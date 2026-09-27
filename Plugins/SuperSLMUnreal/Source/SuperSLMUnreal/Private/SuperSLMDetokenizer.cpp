#include "SuperSLMDetokenizer.h"

#include "SuperSLMModel.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMStatusMapping.h"

#include "superslm/sslm_abi.h"

// The mapping is the registry's shared one (SuperSLMRuntimeRegistry.h), not a private
// sslm_model_map: when a subsystem is configured with Model, or an adapter was imported against it,
// the acquire is a refcount and costs nothing; otherwise this call maps the artifact itself (a
// whole-file SHA-256 plus a copy, model.h "Ownership") and unmaps it on return. The registry is
// game-thread API, so this is a game-thread call; the detokenize itself is a table lookup.
bool SuperSLM::DetokenizeTokens(
	const USuperSLMModel& Model,
	TConstArrayView<int32> Tokens,
	FString& OutUtf8Text,
	FString& OutError)
{
	OutUtf8Text.Reset();
	if (Tokens.Num() == 0)
	{
		return true;
	}
	if (Model.GetMappedArtifactSize() == 0)
	{
		OutError = TEXT("model has no mapped artifact data");
		return false;
	}

	FString MapError;
	const sslm_model Mapping = SuperSLMRuntime::AcquireModelMapping(Model, MapError);
	if (Mapping == nullptr)
	{
		OutError = MapError;
		return false;
	}

	// A fresh {0} state is Layer 1's documented start state (sslm_abi.h). The first call sizes
	// the output: a null buffer answers SSLM_BUFFER_TOO_SMALL with the byte count and leaves the
	// state untouched. A trailing partial UTF-8 character stays in the state and is not emitted --
	// the text of a span cut mid-character ends at the last whole one.
	sslm_detok_state State = {};
	int32_t Needed = 0;
	sslm_status Status = sslm_detokenize_stream(Mapping, &State, Tokens.GetData(), Tokens.Num(), nullptr, &Needed);
	TArray<char> Utf8;
	if (Status == SSLM_BUFFER_TOO_SMALL && Needed > 0)
	{
		Utf8.SetNumUninitialized(Needed);
		int32_t Written = Needed;
		Status = sslm_detokenize_stream(Mapping, &State, Tokens.GetData(), Tokens.Num(), Utf8.GetData(), &Written);
		Utf8.SetNum(Status == SSLM_OK ? Written : 0);
	}
	else if (Status == SSLM_BUFFER_TOO_SMALL)
	{
		// Nothing to emit (every id decodes to an empty or still-partial byte string).
		Status = SSLM_OK;
	}
	SuperSLMRuntime::ReleaseModelMapping(Mapping);

	if (Status != SSLM_OK)
	{
		OutError = FString::Printf(TEXT("sslm_detokenize_stream failed (%s)%s"),
			ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)),
			Status == SSLM_ARTIFACT_REJECTED ? TEXT(": the artifact carries no tokenizer") : TEXT(""));
		return false;
	}
	OutUtf8Text = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Utf8.GetData()), Utf8.Num());
	return true;
}

bool SuperSLM::TokenizeText(
	const USuperSLMModel& Model,
	const FString& Utf8Text,
	TArray<int32>& OutTokens,
	FString& OutError)
{
	OutTokens.Reset();
	FString MapError;
	const sslm_model Mapping = SuperSLMRuntime::AcquireModelMapping(Model, MapError);
	if (Mapping == nullptr)
	{
		OutError = MapError;
		return false;
	}
	// USuperSLMSubsystem::Tokenize()'s own sizing: one guess, then the size Layer 1 reports.
	const FTCHARToUTF8 Utf8(*Utf8Text);
	int32 Count = Utf8.Length() + 16;
	OutTokens.SetNumUninitialized(Count);
	const char* Text = reinterpret_cast<const char*>(Utf8.Get());
	sslm_status Status = sslm_tokenize(Mapping, Text, OutTokens.GetData(), &Count);
	if (Status == SSLM_BUFFER_TOO_SMALL)
	{
		OutTokens.SetNumUninitialized(Count);
		Status = sslm_tokenize(Mapping, Text, OutTokens.GetData(), &Count);
	}
	SuperSLMRuntime::ReleaseModelMapping(Mapping);
	if (Status != SSLM_OK)
	{
		OutTokens.Reset();
		OutError = FString::Printf(TEXT("sslm_tokenize failed (%s)%s"),
			ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)),
			Status == SSLM_ARTIFACT_REJECTED ? TEXT(": the artifact carries no tokenizer") : TEXT(""));
		return false;
	}
	OutTokens.SetNum(Count);
	return true;
}
