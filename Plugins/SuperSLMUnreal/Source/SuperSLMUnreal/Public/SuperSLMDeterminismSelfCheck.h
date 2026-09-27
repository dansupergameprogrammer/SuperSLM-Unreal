#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;

// L2-S2 (the plan §7 item 11, "Determinism self-check --
// the user-runnable device test", D-SLM3857 as re-formed by D-SLM7228; commissioning
// D-SLM7223; Coverage Model §9 R-S2b). This header declares the computation every entry point
// calls: the editor's query window, the Run Determinism Self-Check Blueprint node (which a game,
// including a packaged build, uses), and FSuperSLMSelfCheckRun / Run() from C++. The self-check
// has no console command; the plugin's one console command is SuperSLM.CalibrateCosts, which
// measures scheduling costs and is unrelated.
//
// Runs the pinned reference workload (the example artifact, a pinned prompt, no schema bound,
// greedy decoding, 32 decode steps) on each backend at two granularities
// of that backend's own parameter (CPU: layer_budget in {1, num_hidden_layers}; GPU:
// the composed path at one layer per slice, plus the one-call path at a whole token),
// computes SuperSLMGpuDigest::ComputeTokenDigest per (backend, granularity), and
// compares: (1) every GPU digest against THIS MACHINE's own CPU digests -- the value
// that sets the GPU arm's Verified/Diverged verdict; (2) the CPU digest against a
// reference digest file shipped for the artifact, produced by Layer 1's OWN
// sslm_generate at the pin -- the value that sets the CPU arm's verdict (plan §7 item
// 6: "asks whether the plugin perturbs Layer 1's bits... never share a verdict" with
// the GPU arm); (3) the plugin's own prior run, keyed (artifact hash, pin, plugin
// version, backend, device label) -- reported changed/unchanged, never a verdict.

enum class ESuperSLMSelfCheckVerdict : uint8
{
	Verified,
	Diverged,
	NotYetRun,
};

struct SUPERSLMUNREAL_API FSuperSLMSelfCheckBackendResult
{
	ESuperSLMSelfCheckVerdict Verdict = ESuperSLMSelfCheckVerdict::NotYetRun;

	// "GPU tokens identical to the CPU reference on the reference workload (32 steps x
	// 2 granularities); final-logit digest unavailable at Layer 1 <tag>", the tag being the
	// compiled pin (SUPERSLM_LAYER1_TAG) -- plan §7 item 11's own worked example of what this
	// field carries; the GPU arm's text also names its logits head and that the GPU verdict
	// is withheld (in 1.0 it always is). Set on every verdict,
	// including NotYetRun (states why -- e.g. "reference file unavailable at this
	// pin").
	FString ScopeText;

	// -1 if no divergence was found. Otherwise the first (decode step, granularity
	// index) pair whose digest disagreed with this backend's own comparison target --
	// plan §7 item 11: "the report names ... the first diverging step and
	// granularity."
	int32 FirstDivergingStep = -1;
	int32 FirstDivergingGranularityIndex = -1;

	// True only for the CPU arm, and only when comparison (2)'s reference file was
	// actually found and read (plan §7 item 11: "It exists only for artifacts the
	// plugin ships a reference for, and the report names it absent otherwise"). Always
	// false for the GPU arm, whose own comparison (1) never depends on a shipped file.
	bool bReferenceFileAvailable = false;

	// Comparison (3): "changed / unchanged, never pass" against this backend's own
	// prior run keyed by (artifact hash, pin, plugin version, backend, device label).
	// Distinct from Verdict, which never depends on this comparison.
	bool bChangedFromPriorRun = false;
	bool bHadPriorRun = false;

	// Plan §7 item 11: a withheld (quarantined) verdict is recorded in the report but never
	// displayed, and never read by §6, §8 or the cost readout. Set by the self-check with the
	// scope text that says so; in 1.0 the GPU arm's verdict is always withheld. The Blueprint
	// library and the query window read this flag rather than the text. Showing the GPU verdict
	// is a change here and to that text, in a later release.
	bool bQuarantined = false;
};

struct SUPERSLMUNREAL_API FSuperSLMSelfCheckReport
{
	FSuperSLMSelfCheckBackendResult Cpu;
	FSuperSLMSelfCheckBackendResult Gpu;

	// The platform's own device label (plan §7 item 11: "the platform's, not Layer 1's
	// pick, which cannot be queried" -- no GetGpuDeviceIdentity-style verb exists at
	// the pinned Layer 1, v1.5.0, 1.7.0 or 1.9.0, plan §2.1).
	FString DeviceLabel;
	FString LayerOneTagAndCommit;
	FString PluginVersion;
	FString ArtifactHash;
};

// Fold-round ruling 1 (the self-check must not block the editor): the same computation as a run
// that advances in steps. Construct it on the game thread (it finds the subsystems, encodes the
// pinned prompt and reads the shape -- cheap), then call Step() once per editor frame until it
// returns true. Each Step() is game-thread work bounded by StepBudgetSeconds: the subsystems'
// Tick(), vend and return, and at the arm boundaries the reference file read and the prior-run
// record (small files). The inference runs on the CPU backend's worker threads and the GPU
// backend's submission thread, as under Run(). No Step() waits on either: when the next tick would
// wait for the CPU worker or for a GPU submission job, Step() returns false with
// IsWaitingOnWorker() true instead of ticking (T-2818 nonblock). Run() is this run stepped without
// a budget, so the two produce the same report. Destroying a run mid-way returns the sequence it
// holds.
class SUPERSLMUNREAL_API FSuperSLMSelfCheckRun
{
public:
	FSuperSLMSelfCheckRun(USuperSLMModel& Model, const FString& ReferenceDigestFilePathOverride);
	~FSuperSLMSelfCheckRun();

	FSuperSLMSelfCheckRun(const FSuperSLMSelfCheckRun&) = delete;
	FSuperSLMSelfCheckRun& operator=(const FSuperSLMSelfCheckRun&) = delete;

	// Advances the run; true once it has finished and GetReport() is final.
	bool Step(double StepBudgetSeconds);
	bool IsDone() const;
	// True when the last Step() stopped because the CPU worker or the GPU device, not the schedule,
	// is what the next tick waits on: real time must pass before stepping again helps.
	bool IsWaitingOnWorker() const;
	const FSuperSLMSelfCheckReport& GetReport() const;

#if WITH_DEV_AUTOMATION_TESTS
	// RunWithGpuTokenPerturbation()'s commissioning seam; set before the first Step().
	void SetGpuTokenPerturbation(int32 GranularityIndex, int32 StepIndex);
#endif

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

namespace SuperSLMDeterminismSelfCheck
{
	// Model must already be Configure()'d against BOTH a CPU subsystem
	// (USuperSLMSubsystem) and a GPU subsystem (USuperSLMGpuSubsystem) -- this function drives
	// both, it does not Configure()
	// them itself, matching the plugin's general convention: the seam is
	// declared here, and the caller wires the call sites.
	//
	// ReferenceDigestFilePathOverride: empty selects the shipped default path for
	// Model's own artifact; non-empty is used verbatim. This is the SAME seam R-S2b's
	// must-reject construction needs (the red-suite record
	// §3): pointing this parameter at a copy of the reference file with one digest
	// entry corrupted drives THE SAME comparison code path (2) a genuinely corrupted
	// shipped file would, which is what makes the construction "one the production
	// path can produce" (D-SLM7223) rather than a mocked comparator.
	SUPERSLMUNREAL_API FSuperSLMSelfCheckReport Run(USuperSLMModel& Model, const FString& ReferenceDigestFilePathOverride);

#if WITH_DEV_AUTOMATION_TESTS
	// Commissioning seam: changes one completed GPU token before comparison (1). The
	// production Run path always calls the same comparison with no perturbation.
	SUPERSLMUNREAL_API FSuperSLMSelfCheckReport RunWithGpuTokenPerturbation(
		USuperSLMModel& Model, const FString& ReferenceDigestFilePathOverride,
		int32 GranularityIndex, int32 StepIndex);
#endif
}

