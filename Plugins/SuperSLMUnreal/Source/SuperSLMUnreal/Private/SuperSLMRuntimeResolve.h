#pragma once

#include "CoreMinimal.h"
#include "SuperSLMRuntimeConfig.h"

namespace SuperSLMRuntime { struct FModelBytes; }

// Decode-threading D4 (plan §10.3.1 item 5b; D-SLM8109, D-SLM8144, D-SLM8145, D-SLM8165-8167): the
// one place an FSuperSLMRuntimeConfig's optional decode mode, task count and thirteen cost fields
// are read. Resolve() runs on the calling thread inside Configure() and BeginConfigure(), before
// any pool work is queued, and returns one plain FSuperSLMResolvedCpuConfig that the worker lanes,
// the hook install, pricing, the Configure() line and the report consume.
//
// It also owns the two process-wide inputs resolution reads:
//  - the process selection made by SuperSLM.SetDecodeMode and SuperSLM.SetFinishParallelTasks
//    (registered here), and its --save file, Saved/SuperSLM/RuntimeSettings.ini;
//  - the saved estimates SuperSLM.EstimateCosts --write-ini writes, Saved/SuperSLM/CalibratedCosts.ini.
// Both files are read at each resolution, so a selection or save made after a call applies to the
// next configuration. Game thread.
namespace SuperSLMRuntimeResolve
{
	// Plan §5 "The scheduler's numeric domain" (D-SLM7799): the largest K any job may have.
	constexpr int32 kMaxJobTicks = 1 << 20;

	// The task count when nothing selects one: Layer 1's measured best on the reference box.
	constexpr int32 kShippedParallelTasks = 4;

	// The thirteen cost fields, in the order the plan's table lists them, each with its member of
	// the resolved configuration and whether it is a per-position slope (0 accepted) or a
	// millisecond cost (positive).
	struct FCostField
	{
		const TCHAR* Name;
		double FSuperSLMResolvedCpuConfig::* Member;
		bool bPerPosition;
	};
	constexpr int32 kCostFieldCount = 13;
	TConstArrayView<FCostField> CostFields();

	// A cost value is in its field's domain: a finite, positive millisecond cost, or a finite,
	// non-negative per-position slope.
	bool IsInFieldDomain(const FCostField& Field, double Value);

	// The effective decode mode's one-time measured profile (plan §10.3.1 item 5b's table): writes
	// all thirteen cost members of Out.
	void ApplyModeProfile(bool bEffectiveMode, FSuperSLMResolvedCpuConfig& Out);

	// Saved/SuperSLM/RuntimeSettings.ini and Saved/SuperSLM/CalibratedCosts.ini, as absolute paths.
	FString RuntimeSettingsPath();
	FString CalibratedCostsPath();

	// The mode and task count a configuration resolves when the caller sets neither: this
	// process's selection, else RuntimeSettings.ini, else off and kShippedParallelTasks.
	void ResolveUncalledSettings(bool& OutDecodeModeRequested, int32& OutParallelTasks);

	// Resolves Config. Model supplies the artifact hash a saved estimate must match and the
	// context cap the composed cost-domain check needs; with no model no saved estimate is read.
	// Logs one Warning naming CalibratedCosts.ini when a matching saved estimate is not used.
	FSuperSLMResolvedCpuConfig Resolve(const FSuperSLMRuntimeConfig& Config, const SuperSLMRuntime::FModelBytes* Model);

	// D-SLM7799 rule 3 (plan §5 "The scheduler's numeric domain"): every job kind's largest
	// planned cost under these costs, Shape's BlockCount, MaxLayerBudget, MaxPrefillChunkBudget and
	// TickBudgetMs, and the model's context cap, is finite and implies K <= kMaxJobTicks. On failure
	// OutError names the job kind, the cost fields it is priced from, its bound, K and the limit.
	bool CheckCostDomain(const FSuperSLMRuntimeConfig& Shape, const FSuperSLMResolvedCpuConfig& Costs, int64 ContextCap, FString& OutError);

	// "caller", "saved-estimate" or "mode-default".
	const TCHAR* CostProfileName(ESuperSLMCostProfile Profile);

	// The Configure() line's text (plan item 5b "Report"): "SuperSLM: CPU Configure: bParallelDecode=<effective 0|1>
	// FinishParallelTasks=<n> cost profile=<name> caller_cost_fields=<0..13>".
	FString ConfigureLine(const FSuperSLMResolvedCpuConfig& Resolved);

	// Writes Text (UTF-8, no BOM) to a temporary sibling of Path and moves it over Path in one
	// replace, so Path holds either its prior bytes or all of Text. On failure Path keeps its prior
	// bytes, the temporary is removed, and OutError says why.
	bool WriteTextFileAtomically(const FString& Path, const FString& Text, FString& OutError);

	// One "[Section]" of an INI-shaped file: its header line and its key=value lines, keys and
	// values trimmed; lines starting ';' are comments.
	struct FIniSection
	{
		FString Header;
		TMap<FString, FString> Keys;
	};
	TArray<FIniSection> ParseIniSections(const FString& Text);

	// The saved-estimate profiles of ExistingText (a prior CalibratedCosts.ini) that SuperSLM.EstimateCosts
	// --write-ini keeps: each section whose ArtifactHash, bParallelDecode and FinishParallelTasks labels
	// parse and whose artifact hash or task count differs from HashHex and Tasks, as its header and
	// key=value lines, each ending "\n". Same-key profiles (this run rewrites both modes), comments,
	// blank lines and sections without those labels are dropped.
	FString KeptEstimateProfiles(const FString& ExistingText, const FString& HashHex, int32 Tasks);

	// A decimal number written as a whole string (exponent forms included), finite.
	bool ParseFiniteDouble(const FString& Text, double& OutValue);
}
