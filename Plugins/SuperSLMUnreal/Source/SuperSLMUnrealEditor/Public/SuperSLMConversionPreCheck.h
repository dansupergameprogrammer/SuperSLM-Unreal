#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSlotGates.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate.

// L2-S3 (the plan §7 item 2; Coverage Model §9, cell R-S3d;
// the red-suite record). "A Layer-2 reader of a checkpoint's
// config.json and tokenizer files against the documented supported shape. It reports the
// derived context_cap, the KV block size and the predicted reset cost BEFORE a conversion is
// paid for. The real conversion runs Layer 1's pipeline out of process, unmodified. A pass
// means 'no known blocker'; a later converter failure fires a named 'passed pre-check, failed
// conversion' diagnostic that names the stage."
//
// The documented supported shape at v1.5.0 (D-SLM4714, D-SLM4719, D-SLM4721): a "qwen2"
// `model_type` whose declared geometry satisfies `hidden_size == num_attention_heads *
// head_dim` (`head_dim` read from an explicit config field when present, else derived as
// `hidden_size / num_attention_heads`, matching Qwen2.5's own config shape, which carries no
// explicit `head_dim` key). A `model_type` outside this set -- "qwen3" named explicitly,
// because it is the one the R-S3d fixture set exercises -- is rejected by name: at
// v1.5.0 the engine's forward pipeline has no per-head QK-norm slot and `CheckConfigGeometry`
// has not been generalized for it (D-SLM4714/D-SLM4715), so a Qwen3-family checkpoint's own
// declared geometry independently violates the same invariant this pre-check enforces
// (`hidden_size=1024`, `num_attention_heads=16`, `head_dim=128` -> 16*128 != 1024) -- the
// architecture-name rejection and the geometry rejection agree on this checkpoint. Since code
// review W3 the pre-check reports both: every blocker it finds, geometry before the
// architecture name, so a rejection by name never hides the concrete cause.
struct SUPERSLMUNREALEDITOR_API FSuperSLMConversionPreCheckReport
{
	bool bPass = false;

	// Empty when bPass. Names the stage/reason ("model_type: unsupported architecture 'qwen3'",
	// "geometry: hidden_size (1024) != num_attention_heads (16) * head_dim (128)", "missing
	// tokenizer file", ...) -- never a bare boolean, matching this project's "named diagnostic"
	// convention (SuperSLMImportDiagnostic.h). Every blocker found, "; "-separated, in check
	// order: fields, geometry, tokenizer files, context_cap, lifecycle budget, architecture (W3).
	FString BlockerReason;

	FString ModelType;
	int32 DerivedHeadDim = 0;

	// Echoes RequestedContextCap once validated against the checkpoint's own
	// max_position_embeddings (§7 item 2: "reports the derived context_cap") -- 0 when bPass is
	// false and the check never reached that stage.
	int64 DerivedContextCap = 0;

	// Present only once a real artifact's own sslm_kv_block_size is known -- BEFORE conversion,
	// this pre-check reports what THAT byte count and its predicted reset/adopt cost WOULD be
	// under the project's declared kv_precision_width, using the same law
	// SuperSLMSequenceLifecycleBudget.h's Predict() applies to a real, already-mapped model
	// (§5's linear-in-block-size scaling). 0 when bPass is false.
	int64 PredictedKvBlockSizeBytes = 0;
	double PredictedResetMs = 0.0;
	double PredictedAdoptMs = 0.0;
};

namespace SuperSLMConversionPreCheck
{
	// CheckpointDir is a local HuggingFace-snapshot-shaped directory (config.json plus
	// tokenizer files, e.g. `<HF cache>/hub/models--Qwen--.../snapshots/<rev>`).
	// SequenceLifecycleBudgetMs and BandwidthBytesPerSec feed the same Predict() law
	// SuperSLMSequenceLifecycleBudget.h uses; a caller measures BandwidthBytesPerSec once via
	// SuperSLMSequenceLifecycleBudget::MeasureHostWriteBandwidthBytesPerSec() and reuses it
	// (that header's own established convention) rather than this function re-measuring per
	// call. Never runs the real converter or maps any `.sslm` bytes -- "the real conversion runs
	// Layer 1's pipeline out of process, unmodified" (§7 item 2); this is a config.json-only
	// read.
	SUPERSLMUNREALEDITOR_API bool Run(
		const FString& CheckpointDir,
		int64 RequestedContextCap,
		double SequenceLifecycleBudgetMs,
		double BandwidthBytesPerSec,
		FSuperSLMConversionPreCheckReport& OutReport);

	// L2-S3 build: the same, predicting adopt as well -- AdoptToResetRatio from
	// SuperSLMSequenceLifecycleBudget::MeasureHostBandwidthAndAdoptRatio(), measured once by the
	// caller like the bandwidth. The form above has no ratio to apply, so it reports
	// PredictedAdoptMs 0 rather than assume one. Returns false only when the check could not run
	// at all (no readable config.json); a checkpoint with a blocker returns true with bPass false.
	SUPERSLMUNREALEDITOR_API bool Run(
		const FString& CheckpointDir,
		int64 RequestedContextCap,
		double SequenceLifecycleBudgetMs,
		double BandwidthBytesPerSec,
		double AdoptToResetRatio,
		FSuperSLMConversionPreCheckReport& OutReport);

	// Plan §7 item 2's second half: the named "passed pre-check, failed conversion" diagnostic.
	// A caller that ran the out-of-process converter after a passing pre-check hands its failing
	// stage and message here; the text names both. No L2-S3 caller runs the converter (in-editor
	// conversion is L2-S4's MCP action-tier shape), so this is the diagnostic's one definition.
	// Fold-round ruling 3 (checked: nothing in L2-S3 -- the editor module, the MCP read or action
	// tier -- runs a conversion; the MCP tier runs only the pre-check): it attaches at L2-S4's
	// authoring, wherever that tier launches Layer 1's converter out of process after a passing
	// Run() -- on a non-zero exit, with the stage the converter names. Exported, so the MCP
	// sibling can call it there.
	SUPERSLMUNREALEDITOR_API FString PassedPreCheckFailedConversion(
		const FSuperSLMConversionPreCheckReport& PassingReport, const FString& ConverterStage, const FString& ConverterMessage);
}
