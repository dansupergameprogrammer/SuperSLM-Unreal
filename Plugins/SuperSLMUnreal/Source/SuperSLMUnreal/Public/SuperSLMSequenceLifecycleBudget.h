#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;

// L2-S1 (the plan §5 "An import-time Sequence Lifecycle Budget on
// the axis the cost scales on"; §12 decision 2; D-SLM3554, D-SLM3548, T-2167). Reset/adopt cost
// is linear in sslm_kv_block_size, which is linear in context_cap and fixed at conversion (§5)
// -- so the prediction needs only the model's own KV block size and a measured host bandwidth,
// never a live subsystem or a pool. This keeps the check callable from the IMPORT path (before
// any USuperSLMSubsystem exists) as well as from the subsystem's own Configure() -- R-S1e
// imports A-EX under two Sequence Lifecycle Budget settings and separately checks the
// subsystem's own report agrees with the import-time one.
struct SUPERSLMUNREAL_API FSuperSLMSequenceLifecycleReport
{
	int64 KvBlockSizeBytes = 0;
	double MeasuredBandwidthBytesPerSec = 0.0;
	double PredictedResetMs = 0.0;
	double PredictedAdoptMs = 0.0;
	bool bWithinBudget = false;
};

namespace SuperSLMSequenceLifecycleBudget
{
	// A real, measured whole-KV-block write bandwidth on THIS box, bytes/sec -- never a shipped
	// constant (§5: "the measured host bandwidth"). Re-measured, not cached across engine
	// versions or hardware. Declared here rather than folded into Predict() below so a caller
	// (the importer, the subsystem, a test) can measure once and pass the same figure into
	// several Predict() calls without re-measuring per model.
	SUPERSLMUNREAL_API double MeasureHostWriteBandwidthBytesPerSec();

	// L2-S3 build (plan §7 items 1-2): the adopt-to-reset ratio CheckModel() applies -- the
	// measured whole-block write bandwidth over the measured copy bandwidth, from one pass of the
	// same measurement -- for a caller (the conversion pre-check) that predicts adopt as well as
	// reset without a mapped model. Measures once per call, like the function above; a caller
	// measures once and reuses both.
	SUPERSLMUNREAL_API void MeasureHostBandwidthAndAdoptRatio(double& OutWriteBytesPerSec, double& OutAdoptToResetRatio);

	// Derived, not executed, exactly as §5 states its own worked example (26.24 ms / 448 MiB
	// linearly rescaled): PredictedResetMs = KvBlockSizeBytes / BandwidthBytesPerSec * 1000.
	//
	// Adopt's own ratio to reset is a MEASURED quantity, not a scaling law the plan states in
	// closed form: D-SLM3548 / T-2167 measured 54.22 ms against 26.24 ms at the same block size
	// at v1.2.0, a ~2.07x ratio. R-S1b re-measures this ratio at v1.5.0 rather than this
	// function assuming the old figure still holds (an API choice recorded in
	// the red-suite record §6 -- the plan states reset's own linear
	// law explicitly and never states adopt's, only a measured ratio at the prior pin), so the
	// ratio Predict() applies is an explicit input, never a hard-coded constant.
	SUPERSLMUNREAL_API FSuperSLMSequenceLifecycleReport Predict(
		int64 KvBlockSizeBytes,
		double BandwidthBytesPerSec,
		double AdoptToResetRatio,
		double BudgetMs);

	// The import-time gate itself (§5: "fails import with a diagnostic naming context_cap as
	// the lever"; R-S1e: "A-EX imported under two Sequence Lifecycle Budget settings ... import
	// fails ... then passes"). Reads Model's own KV block size (sslm_kv_block_size's formula,
	// §2.1 -- num_hidden_layers, context_cap, num_key_value_heads, head_dim, kv_precision_width
	// -- via a transient sslm_model_map/unmap on Model's already-validated bytes, never a second
	// parse of the artifact), measures bandwidth, and predicts against BudgetMs. The caller (the
	// import pipeline, or a test standing in for it before that pipeline wires this in) turns a
	// !bWithinBudget report into a rejected import naming context_cap as the lever; this
	// function itself never touches FSuperSLMImportDiagnostic, so it works independently of
	// exactly where in the import pipeline the wiring lands.
	SUPERSLMUNREAL_API FSuperSLMSequenceLifecycleReport CheckModel(const USuperSLMModel& Model, double BudgetMs);
}
