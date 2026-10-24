#pragma once

#include "CoreMinimal.h"
#include "SuperSLMPrefillPlanning.h"
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

	// The decode mode when nothing selects one: off. Threaded decode is an option a game turns on
	// (a caller's bParallelDecode = true, SuperSLM.SetDecodeMode 1 or a saved RuntimeSettings.ini
	// bParallelDecode=1), because it also takes memory bandwidth from the rest of the game.
	constexpr bool kShippedDecodeMode = false;

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

	// The fourteenth, optional key of a saved estimate (precache U4, §5.5): the cache restore's cost per MiB.
	// A profile without it is used as before, with the term 0; a profile whose value is present but not a
	// finite, non-negative number is used with the term 0 and one Warning naming the key.
	inline const TCHAR* RestorePerMiBKey() { return TEXT("RestoreCostPerMiBMs"); }

	// The fifteenth and sixteenth, optional keys (precache plan rev 1.18.11, §15.31): the cache chunk's price pair
	// from the estimate's prefix_prefill arm. A profile without them is used as before, and the batched pair prices
	// the cache chunk; a profile with one of them, or with a value out of the domain, is used without the pair, with
	// one Warning naming the keys.
	inline const TCHAR* PrefixPrefillTokenKey() { return TEXT("PrefixPrefillTokenCostMs"); }
	inline const TCHAR* PrefixPrefillSlopeKey() { return TEXT("PrefixPrefillTokenCostPerPositionMs"); }

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
	// process's selection, else RuntimeSettings.ini, else kShippedDecodeMode and kShippedParallelTasks.
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

	// Schema fast-decode plan rev 1.3.6, §6 P1 "Where the prices come from" (cell P1.1; mutant XD9):
	// the per-C prompt prices, one pair per chunk size C = 2..7, each member resolved on its own: the
	// caller's FSuperSLMRuntimeConfig::PerChunkPromptTokenCost*Ms[C] when set; else the matching
	// saved estimate's PromptChunk<C>TokenCostMs / PromptChunk<C>TokenCostPerPositionMs when present
	// and in domain (and the saved estimate's thirteen fields were not replaced by the mode profile);
	// else the resolved chunk-1 member (PromptTokenCostMs / PromptTokenCostPerPositionMs), mutant
	// X15's figure, which is the default until B5f fills the field.
	const TCHAR* PerChunkTokenCostKey(int32 Chunk);            // "PromptChunk<C>TokenCostMs"
	const TCHAR* PerChunkTokenCostPerPositionKey(int32 Chunk); // "PromptChunk<C>TokenCostPerPositionMs"

	// The per-C table every pricing reader uses: Costs.PerChunkPromptTokenCost*Ms[2..7].
	SuperSLMPrefillPlanning::FPerChunkPromptPrices PerChunkPromptPrices(const FSuperSLMResolvedCpuConfig& Costs);

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

	// Precache plan rev 1.18.12 (§15.32): the in-situ prefix_prefill arm's write. Sets PrefixPrefillTokenKey() and
	// PrefixPrefillSlopeKey() to TokenText and SlopeText in the CalibratedCosts.ini profile Resolve() would use for
	// this artifact hash, effective decode mode and task count (the first such section), every other line kept, in
	// one atomic replace. False, with the file unchanged and OutError saying why, when the file or the profile is
	// missing (SuperSLM.EstimateCosts --write-ini writes it) or the write fails. OutHeader is the section's header.
	// The edited text is parsed again with ParseIniSections before it is written (code review S1): every other section
	// must read back unchanged and the profile with only the pair changed, or nothing is written.
	bool WriteSavedPrefixPrefillPair(const FString& HashHex, bool bEffectiveMode, int32 Tasks, const FString& TokenText, const FString& SlopeText,
		FString& OutHeader, FString& OutError);
}
