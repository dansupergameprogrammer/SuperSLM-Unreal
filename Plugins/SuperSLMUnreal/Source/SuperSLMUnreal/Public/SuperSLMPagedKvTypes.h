#pragma once

#include "CoreMinimal.h"

// Paged KV (plugin paging plan §3.2, §3.6): the result and statistics carriers the opt-in
// paged-KV mode adds. Plain C++, shared by both subsystems. Additive: no existing type changes.

// The result of a prefix build that reports by name (§3.6): both backends' CreatePrefixFrom, the
// GPU CreatePrefix, and the CPU CreatePrefix overload that returns it. The CPU's existing
// bool CreatePrefix(..., FString&) is unchanged; its page refusal's message starts with the stable
// token "KvPagesExhausted:".
enum class ESuperSLMPrefixResult : uint8
{
	Success,
	NotConfigured,            // the subsystem has no configured model and pool
	PrefixLimit,              // PrefixBlockCount prefixes (and nested children) are already live
	KvPagesExhausted,         // budget/pooled mode: region P cannot take the build's charge (UP-6)
	NestedRequiresBudgetMode, // CreatePrefixFrom in legacy/dedicated mode
	ParentNotUsable,          // CreatePrefixFrom: the parent is released, not yet frozen, or unknown
	UnsupportedOnGpu,         // the GPU in dedicated mode
	Malformed,                // empty where not allowed, out-of-vocabulary, or longer than the plugin's cap
};

// One region of the page ledger, in pages.
struct SUPERSLMUNREAL_API FSuperSLMKvRegionStats
{
	int64 CapacityPages = 0;
	int64 ChargedPages = 0;
};

// GetKvPageStats() (§3.2): the ledger's regions and counters, and the engine pool snapshot taken
// by the last applied job, all game-thread data. For display, logs and pacing (a loader can read
// FreeHeadroomPages before issuing a restore). No verdict rests on it: the plan's cells grade
// page counts with the fill probe (§3.9).
struct SUPERSLMUNREAL_API FSuperSLMKvPageStats
{
	// False in legacy mode (SequenceBudget 0), where there is no ledger and every other field is 0
	// except LedgerFaults.
	bool bPagedMode = false;

	int64 PagePositions = 0; // B, read from the engine at Configure
	int64 PageBytes = 0;
	// S + P + H + A + C. On the GPU, PoolPages x PageBytes is the pool's device buffer (counted once
	// by GetDeclaredGpuResidencyBytes()) and, equally, its host mirror in system RAM (not counted
	// there; plan §3.2/§3.4 rev 1.10).
	int64 PoolPages = 0;

	FSuperSLMKvRegionStats Sequences;       // S
	FSuperSLMKvRegionStats Prefixes;        // P
	FSuperSLMKvRegionStats RestoreHeadroom; // H
	FSuperSLMKvRegionStats CacheSharing;    // A (0 in the paging release)
	FSuperSLMKvRegionStats Cache;           // C (0 in the paging release)

	int32 LivePrefixCount = 0;
	int32 GhostCount = 0;
	int32 InFlightRestores = 0;
	int32 WaitingRestores = 0;
	int64 FreeHeadroomPages = 0;

	// §3.7: the engine returned KV_POOL_EXHAUSTED to one of this plan's own requests, which correct
	// operation never reaches. Cumulative across re-Configure; survives Shipping.
	uint64 LedgerFaults = 0;

	// The engine's sslm_kv_pool_stats, as the last applied job read it on its lane. Diagnostic.
	bool bHasPoolSnapshot = false;
	int64 SnapshotPageCount = 0;
	int64 SnapshotFreePages = 0;
	int64 SnapshotSharedPages = 0;
};
