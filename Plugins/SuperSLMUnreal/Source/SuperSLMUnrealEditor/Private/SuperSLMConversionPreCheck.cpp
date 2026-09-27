#include "SuperSLMConversionPreCheck.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMJson.h"
#include "SuperSLMSequenceLifecycleBudget.h"

// L2-S3 (plan §7 item 2, D-SLM7348). The documented supported shape, read the way Layer 1's own
// converter reads it (tools/reference_pipeline/pipeline.py's config parse, tools/convert_tokenizer.py's
// tokenizer inputs at the pinned tag): the fields that parse requires, head_dim explicit or
// hidden_size / num_attention_heads, grouped-query heads that divide, and the two tokenizer files
// the tokenizer conversion opens. Review W3: every blocker found is reported, in order --
// required fields, geometry, tokenizer files, context_cap, the lifecycle budget, and the
// architecture name LAST, so a checkpoint rejected by name also shows the concrete cause behind
// the name (Qwen3-Embedding: the head_dim geometry, then the architecture and its QK-norm).
namespace
{
	// KV precision width, a fixed convention (plan §7 item 2, D-SLM7348): the converter always
	// writes kv_precision = KV_PRECISION_INT8 at the pinned tag.
	constexpr int64 kKvPrecisionBytes = 1;

	const TCHAR* const kRequiredIntegerFields[] = {
		TEXT("hidden_size"), TEXT("num_hidden_layers"), TEXT("num_attention_heads"), TEXT("num_key_value_heads"),
		TEXT("intermediate_size"), TEXT("vocab_size"), TEXT("max_position_embeddings"),
	};
	const TCHAR* const kRequiredTokenizerFiles[] = { TEXT("tokenizer.json"), TEXT("tokenizer_config.json") };

	// A blocker that invalidates the derived figures (anything before the lifecycle budget, whose
	// refusal is computed FROM those figures and keeps them).
	void ClearFigures(FSuperSLMConversionPreCheckReport& Report)
	{
		Report.DerivedContextCap = 0;
		Report.PredictedKvBlockSizeBytes = 0;
		Report.PredictedResetMs = 0.0;
		Report.PredictedAdoptMs = 0.0;
	}

	// Why a named architecture outside the supported shape cannot convert, where it is recorded
	// (D-SLM4714/D-SLM4715, D-SLM4721). Empty for a name with no recorded cause.
	const TCHAR* ArchitectureCause(const FString& ModelType)
	{
		return ModelType == TEXT("qwen3")
			? TEXT("; the engine's forward pipeline has no per-head QK-norm (q_norm/k_norm) slot in this release")
			: TEXT("");
	}
}

bool SuperSLMConversionPreCheck::Run(
	const FString& CheckpointDir,
	int64 RequestedContextCap,
	double SequenceLifecycleBudgetMs,
	double BandwidthBytesPerSec,
	FSuperSLMConversionPreCheckReport& OutReport)
{
	return Run(CheckpointDir, RequestedContextCap, SequenceLifecycleBudgetMs, BandwidthBytesPerSec, /*AdoptToResetRatio*/ 0.0, OutReport);
}

bool SuperSLMConversionPreCheck::Run(
	const FString& CheckpointDir,
	int64 RequestedContextCap,
	double SequenceLifecycleBudgetMs,
	double BandwidthBytesPerSec,
	double AdoptToResetRatio,
	FSuperSLMConversionPreCheckReport& OutReport)
{
	OutReport = FSuperSLMConversionPreCheckReport();
	const FString ConfigPath = FPaths::Combine(CheckpointDir, TEXT("config.json"));
	FString ConfigText;
	if (!FFileHelper::LoadFileToString(ConfigText, *ConfigPath))
	{
		OutReport.BlockerReason = FString::Printf(TEXT("config.json: not readable at '%s'"), *ConfigPath);
		return false;
	}
	// R4-W1: a config.json ending in a backslash (a truncated download) is a blocker, not a crash.
	TSharedPtr<FJsonObject> Config;
	FString ReadError;
	if (!SuperSLMJson::TryReadObject(ConfigText, Config, ReadError))
	{
		OutReport.BlockerReason = FString::Printf(TEXT("config.json: not a JSON object (%s)"), *ReadError);
		return true; // the check ran; it found a blocker
	}

	// Review W3: every blocker, in order; the report names them all.
	TArray<FString> Blockers;
	bool bFiguresValid = true;

	TMap<FString, int64> Fields;
	for (const TCHAR* Name : kRequiredIntegerFields)
	{
		int64 Value = 0;
		if (!Config->TryGetNumberField(Name, Value) || Value <= 0)
		{
			Blockers.Add(FString::Printf(TEXT("config.json: '%s' is absent or not a positive integer"), Name));
			continue;
		}
		Fields.Add(Name, Value);
	}
	const int64 HiddenSize = Fields.FindRef(TEXT("hidden_size"));
	const int64 Heads = Fields.FindRef(TEXT("num_attention_heads"));
	const int64 KvHeads = Fields.FindRef(TEXT("num_key_value_heads"));
	const int64 Layers = Fields.FindRef(TEXT("num_hidden_layers"));
	const int64 MaxPositions = Fields.FindRef(TEXT("max_position_embeddings"));

	// Geometry: head_dim explicit, else hidden_size / num_attention_heads (the converter's own
	// derivation), and hidden_size == num_attention_heads * head_dim. Checked only over the fields
	// that are present, so a missing field is reported once, above, not again as geometry.
	int64 HeadDim = 0;
	const bool bGeometryFields = HiddenSize > 0 && Heads > 0 && KvHeads > 0;
	if (Config->HasField(TEXT("head_dim")))
	{
		if (!Config->TryGetNumberField(TEXT("head_dim"), HeadDim) || HeadDim <= 0)
		{
			Blockers.Add(TEXT("geometry: 'head_dim' is present but not a positive integer"));
			HeadDim = 0;
		}
	}
	else if (bGeometryFields)
	{
		if (HiddenSize % Heads != 0)
		{
			Blockers.Add(FString::Printf(TEXT("geometry: hidden_size (%lld) is not a multiple of num_attention_heads (%lld) and no head_dim is given"), HiddenSize, Heads));
		}
		else
		{
			HeadDim = HiddenSize / Heads;
		}
	}
	OutReport.DerivedHeadDim = static_cast<int32>(HeadDim);
	if (bGeometryFields && HeadDim > 0)
	{
		if (Heads * HeadDim != HiddenSize)
		{
			Blockers.Add(FString::Printf(TEXT("geometry: hidden_size (%lld) != num_attention_heads (%lld) * head_dim (%lld)"), HiddenSize, Heads, HeadDim));
		}
		if (HeadDim % 2 != 0)
		{
			Blockers.Add(FString::Printf(TEXT("geometry: head_dim (%lld) is odd; RoPE needs it even"), HeadDim));
		}
	}
	if (bGeometryFields && Heads % KvHeads != 0)
	{
		Blockers.Add(FString::Printf(TEXT("geometry: num_attention_heads (%lld) is not a whole multiple of num_key_value_heads (%lld)"), Heads, KvHeads));
	}

	for (const TCHAR* File : kRequiredTokenizerFiles)
	{
		if (!IFileManager::Get().FileExists(*FPaths::Combine(CheckpointDir, File)))
		{
			Blockers.Add(FString::Printf(TEXT("missing tokenizer file: %s"), File));
		}
	}

	if (MaxPositions > 0 && (RequestedContextCap <= 0 || RequestedContextCap > MaxPositions))
	{
		Blockers.Add(FString::Printf(TEXT("context_cap: %lld is outside (0, max_position_embeddings = %lld]"), RequestedContextCap, MaxPositions));
	}

	// The figures need the whole geometry and a valid context_cap; with any blocker so far they
	// are not computed (0, as before).
	if (Blockers.Num() == 0 && Layers > 0 && HeadDim > 0)
	{
		OutReport.DerivedContextCap = RequestedContextCap;

		// sslm_kv_block_size's own formula (plan §2.1): num_hidden_layers x context_cap x
		// num_key_value_heads x head_dim x 2 (K and V) x kv_precision_width.
		OutReport.PredictedKvBlockSizeBytes = Layers * RequestedContextCap * KvHeads * HeadDim * 2 * kKvPrecisionBytes;
		if (BandwidthBytesPerSec > 0.0)
		{
			OutReport.PredictedResetMs = static_cast<double>(OutReport.PredictedKvBlockSizeBytes) / BandwidthBytesPerSec * 1000.0;
		}
		if (AdoptToResetRatio > 0.0)
		{
			const FSuperSLMSequenceLifecycleReport Lifecycle = SuperSLMSequenceLifecycleBudget::Predict(
				OutReport.PredictedKvBlockSizeBytes, BandwidthBytesPerSec, AdoptToResetRatio, SequenceLifecycleBudgetMs);
			OutReport.PredictedResetMs = Lifecycle.PredictedResetMs;
			OutReport.PredictedAdoptMs = Lifecycle.PredictedAdoptMs;
		}
		// A predicted cost over the project's budget is the import-time gate's refusal (plan §5); it
		// is reported as a blocker here, before the conversion is paid for, naming context_cap. The
		// figures it was computed from stay in the report.
		if (SequenceLifecycleBudgetMs > 0.0 &&
			(OutReport.PredictedResetMs > SequenceLifecycleBudgetMs || OutReport.PredictedAdoptMs > SequenceLifecycleBudgetMs))
		{
			Blockers.Add(FString::Printf(TEXT("sequence lifecycle budget: predicted reset %.2f ms / adopt %.2f ms exceed %.2f ms at context_cap %lld; lower context_cap"),
				OutReport.PredictedResetMs, OutReport.PredictedAdoptMs, SequenceLifecycleBudgetMs, RequestedContextCap));
		}
	}
	else
	{
		bFiguresValid = false;
	}

	// The architecture name last (W3): at the pinned tag only "qwen2" is the documented supported
	// shape, and a rejection by name follows the concrete blockers above rather than hiding them.
	if (!Config->TryGetStringField(TEXT("model_type"), OutReport.ModelType))
	{
		Blockers.Add(TEXT("model_type: absent from config.json"));
		bFiguresValid = false;
	}
	else if (OutReport.ModelType != TEXT("qwen2"))
	{
		Blockers.Add(FString::Printf(TEXT("model_type: unsupported architecture '%s' (the supported shape is 'qwen2')%s"),
			*OutReport.ModelType, ArchitectureCause(OutReport.ModelType)));
		bFiguresValid = false;
	}

	if (Blockers.Num() > 0)
	{
		OutReport.bPass = false;
		OutReport.BlockerReason = FString::Join(Blockers, TEXT("; "));
		if (!bFiguresValid)
		{
			ClearFigures(OutReport);
		}
		return true; // the check ran; it found blockers
	}
	OutReport.bPass = true;
	return true;
}

FString SuperSLMConversionPreCheck::PassedPreCheckFailedConversion(
	const FSuperSLMConversionPreCheckReport& PassingReport, const FString& ConverterStage, const FString& ConverterMessage)
{
	return FString::Printf(TEXT("passed pre-check, failed conversion: stage '%s' (%s). The pre-check found no known blocker for model_type '%s' at context_cap %lld."),
		*ConverterStage, *ConverterMessage, *PassingReport.ModelType, PassingReport.DerivedContextCap);
}
