#pragma once

// The page ledger: game-thread admission control for the plugin's paged-KV mode (plugin paging
// plan §3.2-§3.5, decisions UP-5, UP-6, UP-7 and UP-14).
//
// This file is engine-independent on purpose. It is C++17 and the standard library only: no
// Unreal headers, no exceptions, no RTTI, no Layer-1 call. It is pure bookkeeping. The
// subsystems read the pool geometry from the engine at Configure (on a lane or the GPU
// submission thread) and pass it in; every other input is a game-thread fact the subsystem
// already holds (a request, or a job's own result at its apply). The same ledger serves the CPU
// subsystem in budget mode and the GPU subsystem in pooled mode. Legacy/dedicated mode has no
// ledger.
//
// The host suite at Plugins/SuperSLMUnreal/ci/host/page_ledger builds this file with g++ and
// clang and grades it; Unreal compiles it unchanged inside the runtime module.
//
// ---------------------------------------------------------------------------------------------
// Vocabulary
//
//   B       positions per page, read from the engine at Configure. Never a constant: the engine
//           derives it per model (the paged-KV engine plan's per-model page size).
//   Cp, Ce  the two caps (plugin paging plan §3.2 "Two caps, two block sizes", UP-15
//           option (c)). Cp is the plugin's cap (FPageLedgerConfig::ContextCap): it bounds what
//           is admissible (budgets, origins, prefix lengths, SequenceBudget) and the holder
//           limit. Ce is the engine's cap, the model's context_cap
//           (FPageLedgerConfig::EngineContextCap): every per-request charge (R, E, a
//           whole-reserve restore) is priced at it, as the engine draws. On the CPU Cp = Ce; on
//           the GPU Ce can exceed Cp (32,768 against 4,096 at the shipped 1.5B model).
//   R(b)    min(ceil(b / B) + 1, ceil(Ce / B)) for 1 <= b <= Cp: the pages a budget holder
//           reserves.
//   E       min(floor(origin / B), ceil(Ce / B) - R(b)): the pages a private budget restore draws
//           for its prefix span ("materialized"), returned at the holder's first reset, adopt
//           or release.
//   P_blk   R(Cp) = min(ceil(Cp / B) + 1, ceil(Ce / B)): the block of regions P and A, the peak of
//           the largest admissible prefix.
//   H_blk   min(P_blk + floor(Cp / B), ceil(Ce / B)): the block of region H, the largest restore
//           charge with b <= Cp and origin <= Cp. With Cp = Ce, P_blk = H_blk = ceil(Ce / B).
//
//   Admission   the game-thread instant a request is accepted, before its job is enqueued. Every
//               charge is taken here, at the job's peak draw.
//   Apply       the game-thread instant a job's result is applied. Charges settle to the job's
//               own draw here, and every refund happens here, never earlier (charges >= drawn
//               at every instant).
//
// Regions (plugin paging plan §3.2):
//   S  BlockCount x R(SequenceBudget). Warm reservations; fixed (reset keeps them). Always fully
//      charged while configured.
//   P  PrefixBlockCount x P_blk. Live prefixes, nested children, and ghosts of released prefixes
//      that still have dependents.
//   H  RestoreHeadroomBlocks x H_blk. The part of a restore's (or a create-first
//      normalization's) draw above the slot's R(SequenceBudget).
//   A  CacheGhostBlocks x P_blk when a cache is configured, else 0. For each cache entry with at
//      least one live or pending dependent: floor(L / B) of the entry, pre-charged at the first
//      dependent's admission.
//   C  The cache's own region, charged and refunded by the cache through the narrow seam below
//      (ISuperSLMCacheRegionCharge). Its capacity is an input.
//   Pool pages = S + P + H + A + C.
//
// One job per slot: the subsystem's per-sequence queue admits a slot's operations one at a time,
// so the ledger refuses (InvalidRequest) a second admission for a slot that has an admitted,
// unapplied or waiting job.
// ---------------------------------------------------------------------------------------------

#include <cstdint>
#include <vector>

// Test hooks (mutation switches and the plan's ForceUnderCharge seam, plugin paging plan §3.9).
// The host suite defines this to 1. Unreal maps it to WITH_DEV_AUTOMATION_TESTS in
// SuperSLMUnreal.Build.cs, so Development automation reaches the hooks and Shipping
// compiles them out. Standalone consumers keep the default below unless they opt in.
#ifndef SUPERSLM_PAGE_LEDGER_TEST_HOOKS
#define SUPERSLM_PAGE_LEDGER_TEST_HOOKS 0
#endif

namespace SuperSLMPaging
{

// ---- Page arithmetic (plugin paging plan §2 rows E1, E2, E6; §3.3; §3.5) ---------------------
// Every function returns 0 for an invalid geometry (B < 1 or cap < 1) unless it says otherwise.
// These take one cap, the engine's. With two caps the ledger prices every charge by
// calling them with Ce, and bounds admissible inputs at Cp itself, before any charge.

// ceil(Cap / B). A cap the page size does not divide has a partly empty last page.
uint32_t PagesPerBlock(int64_t PagePositions, int64_t ContextCap);

// R(Budget) = min(ceil(Budget / B) + 1, P_blk); 0 for a budget outside [1, Cap].
uint32_t ReservationPages(int64_t PagePositions, int64_t ContextCap, int64_t Budget);

// floor(Length / B): the full pages a budget adopter of a frozen prefix of this length shares.
// 0 for Length < 0.
uint32_t FullPages(int64_t PagePositions, int64_t Length);

// ceil(Length / B): the pages a frozen root prefix of this length holds. 0 for Length <= 0.
uint32_t OccupiedPages(int64_t PagePositions, int64_t Length);

// E = min(floor(Origin / B), P_blk - R(Budget)); 0 for an invalid budget or Origin < 0.
uint32_t MaterializedPages(int64_t PagePositions, int64_t ContextCap, int64_t Budget, int64_t Origin);

// A prefix build's admission charge: R(max(Tokens, 1)) (§3.3 "Empty prefixes": the engine's
// budget domain starts at 1). Used for a root prefix (Tokens = its length) and a nested child
// (Tokens = the child's own tokens). 0 for Tokens < 0 or Tokens > Cap.
uint32_t PrefixBuildCharge(int64_t PagePositions, int64_t ContextCap, int64_t Tokens);

// The pages a frozen nested child holds privately: ceil((ParentLength + ChildTokens) / B) -
// floor(ParentLength / B) (its own full pages plus its tail page; the parent's full pages are
// shared). With ParentLength = 0 this is OccupiedPages(ChildTokens).
uint32_t NestedSettledPages(int64_t PagePositions, int64_t ParentLength, int64_t ChildTokens);

// What a restore payload declares (read from the Layer-1 payload header, never the wrapper).
enum class ERestorePayload : uint8_t
{
	Budget,       // CPU 'SSB6' with kv_mode = budget, or GPU 'SLM6'
	WholeReserve  // CPU 'SSB5' (or older), or a whole_reserve 'SSB6'
};

// A restore's admission charge, assuming no sharing (§3.5 "Restore charge"): R(b) + E for a
// budget payload, P_blk for a whole-reserve one. 0 for a budget payload whose budget is outside
// [1, Cap] or whose origin is outside [0, Cap].
uint32_t RestoreAdmissionCharge(int64_t PagePositions, int64_t ContextCap, ERestorePayload Payload,
                                int64_t Budget, int64_t Origin);

// The two block sizes (UP-15 (c)). PluginCap is Cp, EngineCap is Ce. Both return 0
// for an invalid geometry (B < 1, a cap < 1) or EngineCap < PluginCap.
//   PrefixBlockPages   = min(ceil(Cp / B) + 1, ceil(Ce / B))            (P_blk, for P and A)
//   HeadroomBlockPages = min(PrefixBlockPages + floor(Cp / B), ceil(Ce / B))  (H_blk, for H)
uint32_t PrefixBlockPages(int64_t PagePositions, int64_t PluginCap, int64_t EngineCap);
uint32_t HeadroomBlockPages(int64_t PagePositions, int64_t PluginCap, int64_t EngineCap);

// ---- Holder shape and the generation limit (§3.3 "Generation limit", §3.4) --------------------

enum class EHolderMode : uint8_t
{
	Budget,
	WholeReserve
};

struct FHolderShape
{
	EHolderMode Mode = EHolderMode::Budget;
	int64_t Budget = 0;   // the holder's own budget (a whole-reserve holder: the cap)
	int64_t Origin = 0;   // 0 after create or reset; the prefix length after adopt; saved origin
	                      // after a budget restore
};

// limit = min(Origin + Budget, Cap) for a budget holder; Cap for a whole-reserve one.
int64_t HolderLimit(int64_t ContextCap, const FHolderShape& Shape);

enum class ELimitVerdict : uint8_t
{
	Allowed,
	KvBudgetExceeded,   // the holder's own limit is below the cap
	ContextCapExceeded  // the limit is the cap: the existing cap outcome fires
};

// Activation pre-check (§3.3, §3.4 "Activation pre-check"): refused when
// ContextUsed + PromptTokens > Limit.
ELimitVerdict CheckActivation(int64_t ContextUsed, int64_t PromptTokens, int64_t Limit, int64_t ContextCap);

// Per-step pre-check (engine token law E7). An embed is allowed while ContextUsed < Limit; a
// finish-only step (primed or armed: it embeds nothing) is allowed at ContextUsed <= Limit. So a
// holder resting ready at c emits exactly 1 + (Limit - c) tokens, then stops by name.
ELimitVerdict CheckStep(int64_t ContextUsed, bool bFinishOnly, int64_t Limit, int64_t ContextCap);

// Budget-mode lifecycle pricing (§3.3 "Lifecycle pricing", cell 7.1): a create or reset is
// priced at R(SequenceBudget) x PageBytes. 0 for an invalid budget.
uint64_t BudgetModeLifecyclePricedBytes(int64_t PagePositions, int64_t ContextCap, int64_t SequenceBudget,
                                        uint64_t PageBytes);

// Region C's size as precache states it: ceil(CacheBudgetBytes / BlockBytes) x P_blk, where
// BlockBytes is the engine's legacy block size (sslm_kv_block_size). 0 when either is 0.
uint32_t CacheRegionPagesFor(uint64_t CacheBudgetBytes, uint64_t BlockBytes, uint32_t PagesPerBlockValue);

// ---- The ledger --------------------------------------------------------------------------------

struct FPageLedgerConfig
{
	int64_t PagePositions = 0;  // B, from the engine at Configure
	uint64_t PageBytes = 0;     // bytes per page, from the engine at Configure
	int64_t ContextCap = 0;        // Cp: the plugin's cap (GPU: the configured ContextCap)
	int64_t EngineContextCap = 0;  // Ce: the engine's cap, the model's context_cap. 0 means "equal
	                               // to ContextCap" (the CPU, where the two are one). Below
	                               // ContextCap is InvalidGeometry.
	int32_t SequenceBudget = 0;
	int32_t BlockCount = 0;
	int32_t PrefixBlockCount = 0;
	int32_t RestoreHeadroomBlocks = 1;
	int32_t CacheGhostBlocks = 1;
	bool bCacheConfigured = false;   // regions A and C are 0 without a cache (GPU: until one exists)
	uint32_t CacheRegionPages = 0;   // region C's capacity; ignored without a cache
};

enum class ELedgerConfigure : uint8_t
{
	Ok,
	LegacyMode,             // SequenceBudget == 0: no ledger; the ledger stays unconfigured
	InvalidSequenceBudget,  // SequenceBudget < 0 or > ContextCap (Cp, never the engine's cap)
	InvalidGeometry         // B < 1, cap < 1, EngineContextCap set below ContextCap,
	                        // PageBytes == 0, a negative count, or a pool above UINT32_MAX pages
};

enum class ERegion : uint8_t
{
	S,
	P,
	H,
	A,
	C,
	Count
};

enum class EPageAdmit : uint8_t
{
	Admitted,
	Waiting,           // restores only: will resolve Admitted (or NotConfigured at epoch end)
	KvPagesExhausted,  // the charge does not fit its region (on arrival, for a restore)
	PrefixLimit,       // PrefixBlockCount live prefixes already (UP-6's count rule)
	ParentNotUsable,   // nested build: the parent is unknown, released, or not yet built
	KvModeMismatch,    // adopt onto a whole-reserve holder that has not been normalized
	PrefixPinned,      // ReleasePrefix while an adopt, begin_from or hinted restore of it is queued
	NotConfigured,
	InvalidRequest,    // out-of-range argument, unknown slot/node, or the slot already has a job
	ContextCapMismatch // restores only: a budget payload whose budget or origin is above the
	                   // plugin's cap Cp but within the engine's cap Ce. A valid save taken under a
	                   // larger ContextCap, not a corrupt one (plugin paging plan §3.5, §3.6). Never
	                   // returned when Cp = Ce.
};

using FTicket = uint64_t;          // 0 is never a valid ticket
using FNodeId = uint64_t;          // a prefix node the ledger minted; 0 is never valid
using FCacheEntryKey = uint64_t;   // the cache entry's identity, one per entry (never per lease);
                                   // 0 is never valid. The caller must not reuse a key while
                                   // CacheEntryDependents(key) or the entry's pre-charge is live.

constexpr FTicket InvalidTicket = 0;
constexpr FNodeId InvalidNode = 0;

struct FAdmission
{
	EPageAdmit Outcome = EPageAdmit::NotConfigured;
	FTicket Ticket = InvalidTicket;  // set for Admitted and Waiting
	FNodeId Node = InvalidNode;      // prefix builds: the node this build creates
	uint32_t ChargedPages = 0;       // pages charged at this admission, all regions (a waiting
	                                 // restore: only a region-A pre-charge taken at its arrival)
	bool bNormalizes = false;        // resets: true when this reset is a create-first normalization
};

enum class EEpochEnd : uint8_t
{
	Reconfigure,
	Teardown,
	Destroyed,
	DeviceLost
};

// What a reset job did, reported at its apply (ApplyReset).
enum class EResetResult : uint8_t
{
	Normalized,
	ResetInPlace,
	Refused
};

struct FWaitResolution
{
	FTicket Ticket = InvalidTicket;
	EPageAdmit Outcome = EPageAdmit::NotConfigured;  // Admitted, or NotConfigured at an epoch end
};

enum class EHintKind : uint8_t
{
	None,
	Prefix,     // a live, frozen prefix node of this ledger
	CacheEntry  // a cache entry (keyed by entry, not by lease)
};

struct FRestoreRequest
{
	int32_t Slot = -1;
	ERestorePayload Payload = ERestorePayload::Budget;
	int64_t Budget = 0;   // budget payloads: the saved budget
	int64_t Origin = 0;   // budget payloads: the saved origin
	EHintKind Hint = EHintKind::None;
	FNodeId HintNode = InvalidNode;
	FCacheEntryKey HintEntry = 0;
	int64_t HintEntryLength = 0;  // the cache entry's length in positions
};

struct FRegionStats
{
	uint32_t Capacity = 0;
	uint32_t Charged = 0;
};

// GetKvPageStats' ledger half. For display and logs, never for verdicts.
struct FPageLedgerStats
{
	bool bConfigured = false;
	uint32_t PoolPages = 0;
	FRegionStats Regions[static_cast<int>(ERegion::Count)];
	uint32_t LivePrefixCount = 0;     // built or building, not released (the count rule's figure)
	uint32_t GhostCount = 0;          // released nodes still charged for their dependents
	uint32_t InFlightRestores = 0;    // admitted, not yet applied
	uint32_t WaitingRestores = 0;
	uint32_t FreeHeadroomPages = 0;   // H capacity - H charged
	uint64_t LedgerFaults = 0;
};

// The narrow seam for region C (the cache's region). When paged mode is on, the cache's
// kind-neutral residency core charges its prefix kind here instead of keeping a second count:
// the prefix kind's charged bytes ARE region C's charge. Bytes must be whole pages.
enum class ECacheRegionCharge : uint8_t
{
	Ok,
	Exhausted,      // would exceed region C: nothing charged; the cache evicts and retries
	NotConfigured,
	InvalidRequest  // not a whole number of pages, or a refund above the charge
};

class ISuperSLMCacheRegionCharge
{
public:
	virtual ECacheRegionCharge ChargeCacheRegionBytes(uint64_t Bytes) = 0;
	virtual ECacheRegionCharge RefundCacheRegionBytes(uint64_t Bytes) = 0;
	virtual uint64_t CacheRegionChargedBytes() const = 0;
	virtual uint64_t CacheRegionCapacityBytes() const = 0;

protected:
	~ISuperSLMCacheRegionCharge() = default;
};

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
// Mutants the plan's cells must kill (§7 cells 1.1-1.11, 3.2, 8.3, 8.4, 11.2). Each is a
// one-line departure from the contract above; the host suite runs every one and asserts the cell
// that names it fails.
enum class ELedgerMutation : uint8_t
{
	None,
	RestoredSharerNotRegistered,        // a sharing restore does not register as a dependent
	PreChargeSkipped,                   // the region-A pre-charge is never taken
	PrivateRestoreRefundedAtApply,      // a private restore's whole H charge returns at its apply
	RefundAtEnqueue,                    // reset/release refunds taken at admission, not apply
	FailedRestoreKeepsCharge,           // a failed restore never refunds H
	GhostReleasedAtFirstDependent,      // a ghost is refunded when its first dependent lets go
	DependencyKeptAtReadopt,            // re-adopt does not drop the previous dependency
	ShareCountedAsGuaranteed,           // the wait predicate counts a hinted restore's whole charge
	WaiterAdmittedWithoutGuarantee,     // every restore that does not fit waits
	WaitersSurviveEpochEnd,             // an epoch end leaves waiters unresolved
	LedgerFaultNotCounted,              // NoteEngineExhaustion does not increment the counter
	NormalizationUncharged,             // a create-first normalization takes no H charge
	// The two caps (UP-15 (c)) and the coverage re-audit's rows (plugin paging plan §7):
	NormalizationTakesHeadroomWhileWaiting,  // a normalization charges H while a restore waits
	FlatGuarantee,                      // a restore guarantees R(SequenceBudget), not
	                                    // min(charge, R(SequenceBudget))
	EmptyChildSettledToZero,            // an empty nested child settles to 0, not its tail page
	RPricedAtContextCap,                // every R (S, prefix and nested charges, a restore's R(b))
	                                    // priced at Cp instead of Ce; block sizes unchanged
	EPricedAtContextCap,                // a restore's E priced at Cp instead of Ce
	BlocksAtContextCap,                 // UP-15 option (b): P_blk = H_blk = ceil(Cp / B)
	BlocksAtEngineCap,                  // UP-15 option (a)'s blocks: P_blk = H_blk = ceil(Ce / B)
	ValidationAtEngineCap,              // budgets, origins, prefix lengths and SequenceBudget
	                                    // bounded by Ce instead of Cp
	LimitAtEngineCap,                   // SlotLimit = min(origin + budget, Ce) instead of Cp
	// The review's findings on the plan's holder figures and its lesser cases (§7 cells 2.1, 2.6,
	// 4.4):
	HoldSettledOnePageSmaller,          // a private restore's hold settles to draw - R(SB) - 1
	BudgetDomainFromZero,               // a budget payload's budget domain checked as [0, Cp]: budget 0
	                                    // passes to a zero R
	ContextCapMismatchReportedAsInvalid // a field above Cp and within Ce reported InvalidRequest
	                                    // (the subsystem's Malformed) instead of ContextCapMismatch
};

// The request kinds the plan's ForceUnderCharge seam shrinks (§3.9).
enum class EChargeKind : uint8_t
{
	PrefixBuild,
	NestedBuild,
	BudgetRestore,
	WholeReserveRestore,
	Normalization,
	CachePreCharge,
	Count
};

// The two pre-check mutants of cell 4.2 (process-wide; host tests only).
enum class ELimitMutation : uint8_t
{
	None,
	OneLate,   // the pre-check's < read as <= : the limit taken as Limit + 1 (one token more)
	OneEarly   // the limit taken as Limit - 1 (one token fewer)
};
void SetLimitMutationForTest(ELimitMutation Mutation);
#endif

class FPageLedger final : public ISuperSLMCacheRegionCharge
{
public:
	FPageLedger();
	~FPageLedger();
	FPageLedger(const FPageLedger&) = delete;
	FPageLedger& operator=(const FPageLedger&) = delete;

	// ---- Epoch ----

	// Sizes the regions and charges S in full (the warm sequences). Every slot starts as a
	// normalized budget holder {Budget, SequenceBudget, 0} with no dependency. Configuring a
	// configured ledger first ends its epoch as EEpochEnd::Reconfigure. A refused Configure leaves
	// the ledger unconfigured.
	ELedgerConfigure Configure(const FPageLedgerConfig& Config);

	// Re-Configure, teardown, destruction or terminal device loss (§0 "Waits", cell 1.11): every
	// waiting restore resolves NotConfigured with nothing charged for it, every charge is dropped
	// (the pool goes with it), and the ledger is unconfigured. Tickets from the ended epoch are
	// stale: their Apply calls return false and change nothing. LedgerFaults is kept.
	void EndEpoch(EEpochEnd Reason);

	bool IsConfigured() const;
	const FPageLedgerConfig& GetConfig() const;

	// ---- Regions ----

	uint32_t PoolPages() const;  // S + P + H + A + C
	uint32_t RegionCapacity(ERegion Region) const;
	uint32_t RegionCharged(ERegion Region) const;
	uint32_t RegionFree(ERegion Region) const;
	uint32_t TotalCharged() const;
	FPageLedgerStats GetStats() const;

	// Wait resolutions produced since the last drain, in the order they happened: Admitted when an
	// apply (or any H refund) admits the waiter at the head of the queue, NotConfigured at an
	// epoch end. The subsystem enqueues each admitted restore's job when it drains this.
	std::vector<FWaitResolution> DrainResolutions();

	// ---- Prefixes (§3.3; region P; UP-6) ----

	// A root prefix build of Tokens positions (0 allowed: begun with budget 1). Checks in order:
	// NotConfigured, InvalidRequest (Tokens outside [0, Cp]), PrefixLimit (LivePrefixCount >=
	// PrefixBlockCount), KvPagesExhausted (P free < PrefixBuildCharge). Admitted mints the node.
	FAdmission AdmitPrefixBuild(int64_t Tokens);

	// A nested build (`begin_from`). Parent must be a live, built (frozen) node:
	// else ParentNotUsable. Parent length + ChildTokens > Cp is InvalidRequest. Then the count and
	// page checks as above, charging PrefixBuildCharge(ChildTokens). The parent is pinned from
	// admission until this build applies.
	FAdmission AdmitNestedPrefixBuild(FNodeId Parent, int64_t ChildTokens);

	// The build job's apply. Success settles the node's P charge to its own pages (OccupiedPages
	// for a root, NestedSettledPages for a child) and, for a child, registers it as a dependent of
	// its parent. Failure refunds the whole charge, frees the count slot and forgets the node.
	bool ApplyPrefixBuild(FTicket Ticket, bool bSucceeded);

	// ReleasePrefix. PrefixPinned while an adopt, a nested build or a hinted restore (admitted or
	// waiting) names the node. Admitted frees the node's count slot at once; its pages stay
	// charged until ApplyPrefixRelease, and after that as a ghost until its last dependent's job
	// applies (children cascade: a grandchild holds both ancestors' ghosts).
	FAdmission AdmitPrefixRelease(FNodeId Node);
	bool ApplyPrefixRelease(FTicket Ticket);

	bool IsPrefixLive(FNodeId Node) const;       // built or building, not released
	bool IsGhost(FNodeId Node) const;            // released and still charged
	uint32_t NodeChargedPages(FNodeId Node) const;
	uint32_t DependentCount(FNodeId Node) const;  // applied dependents only
	uint32_t LivePrefixCount() const;
	uint32_t GhostCount() const;

	// ---- Adopt (§3.3; UP-14) ----

	// Adopt of a plain prefix node: draws nothing, so charges nothing. The node must be live and
	// built (else InvalidRequest). KvModeMismatch for a whole-reserve slot. The node is pinned
	// until the apply.
	FAdmission AdmitAdoptPrefix(int32_t Slot, FNodeId Node);

	// Adopt of a cache entry of EntryLength positions. When the entry has no live or pending
	// dependent, FullPages(EntryLength) is pre-charged to A at this admission; when A cannot take
	// it, KvPagesExhausted with nothing charged (UP-14). Further adopters add nothing.
	FAdmission AdmitAdoptCacheEntry(int32_t Slot, FCacheEntryKey Entry, int64_t EntryLength);

	// The adopt job's apply. Success: the slot's previous dependency is dropped (which may refund
	// a ghost or an A pre-charge), the slot registers on the new target, its materialized pages
	// return (its H hold falls to max(0, R(b) - R(SequenceBudget))), and its origin becomes the
	// target's length. Failure: the slot is unchanged; a pre-charge with no dependent left is
	// refunded.
	bool ApplyAdopt(FTicket Ticket, bool bSucceeded);

	uint32_t CacheEntryDependents(FCacheEntryKey Entry) const;  // applied dependents only
	bool IsCacheEntryPreCharged(FCacheEntryKey Entry) const;

	// ---- Restore (§3.5; UP-7) ----

	// Restore-first into Slot. Before any charge, a budget payload's fields are checked against the
	// two caps (§3.5 "Malformed input" and "ContextCapMismatch"): a budget outside [1, Ce] or an
	// origin outside [0, Ce] is InvalidRequest (the subsystem names it Malformed); otherwise a
	// budget or origin above Cp is ContextCapMismatch. Nothing is charged for either.
	// Charge = RestoreAdmissionCharge at Ce on H (plus, for a budget payload
	// hinted with a cache entry that has no live or pending dependent, the entry's pre-charge on
	// A, taken at arrival; KvPagesExhausted if A cannot take it). Fit: admitted when the H charge
	// fits and no restore is waiting. Otherwise it waits only if
	//     free(H) + sum(guaranteed returns of the restores ahead, in flight or waiting)
	//             - sum(H charges of the waiters ahead) >= its H charge,
	// where a restore's guaranteed return is min(its H charge, R(SequenceBudget)) (its old holder's
	// reservation; a share is never counted); else KvPagesExhausted on arrival. A hint pins its
	// prefix node while the restore is queued or waiting. A hint on a whole-reserve payload is
	// ignored (the engine ignores it).
	FAdmission AdmitRestore(const FRestoreRequest& Request);

	// The restore job's apply. SharedPages is the job's own out_shared_pages. Success: the old
	// holder's dependency is dropped, the slot takes the payload's shape, its H hold settles to
	// max(0, draw - R(SequenceBudget)) where draw = R(b) when SharedPages > 0, R(b) + E when not,
	// and ceil(Ce / B) for a whole-reserve payload; with SharedPages > 0 the slot registers on the hint.
	// A cache pre-charge for a restore that did not share, with no dependent left, is refunded.
	// Failure: the whole charge returns and the slot is unchanged. Either way, waiters that now fit
	// are admitted in arrival order.
	bool ApplyRestore(FTicket Ticket, bool bSucceeded, uint32_t SharedPages);

	// ---- Reset and create-first normalization (§3.5 "Normalization") ----

	// A reset of Slot. When the slot's shape differs from the configured one (whole-reserve, or a
	// budget other than SequenceBudget) and H has R(SequenceBudget) free and no restore is
	// waiting, the reset is a create-first normalization (bNormalizes, R(SequenceBudget) charged
	// to H). Otherwise it is a reset in place (no charge).
	FAdmission AdmitReset(int32_t Slot);

	// The reset job's apply, with what the job actually did (one outcome type for both paths, so a
	// normalization's two failure shapes cannot be confused):
	//   Normalized    normalization only: the create succeeded and the old holder was released.
	//                 The slot becomes {Budget, SequenceBudget, 0}; its whole H hold and the
	//                 normalization charge return. Reported for a reset in place, it is refused:
	//                 the apply returns false and changes nothing (the job stays open).
	//   ResetInPlace  the holder was reset as it is: a reset in place that ran, or a normalization
	//                 whose create was refused and which then reset the old holder. The dependency is
	//                 dropped, the origin becomes 0, a budget holder's materialized pages return, and
	//                 a normalization's charge returns (normalization is retried at the next reset).
	//   Refused       nothing ran: the engine refused the reset (and, for a normalization, the create
	//                 too). The slot is unchanged; only a normalization's charge returns.
	bool ApplyReset(FTicket Ticket, EResetResult Result);

	FHolderShape SlotShape(int32_t Slot) const;
	// The slot holder's generation limit, HolderLimit(ContextCap, SlotShape(Slot)): bounded by the
	// plugin's cap Cp, never the engine's (§3.4 "Two caps"; cell 4.2 [two-cap]). The
	// subsystem's activation and per-step pre-checks read it. 0 for an unknown slot or when
	// unconfigured.
	int64_t SlotLimit(int32_t Slot) const;
	uint32_t SlotHeadroomHold(int32_t Slot) const;  // the slot's settled H hold
	bool SlotHasJob(int32_t Slot) const;

	// ---- Region C (ISuperSLMCacheRegionCharge) ----

	ECacheRegionCharge ChargeCacheRegionBytes(uint64_t Bytes) override;
	ECacheRegionCharge RefundCacheRegionBytes(uint64_t Bytes) override;
	uint64_t CacheRegionChargedBytes() const override;
	uint64_t CacheRegionCapacityBytes() const override;

	// ---- The ledger-fault detector (§3.7, cell 11.2) ----

	// The subsystem calls this when the engine returns KV_POOL_EXHAUSTED to one of this plan's
	// requests. The counter survives Shipping and epoch ends.
	void NoteEngineExhaustion();
	uint64_t LedgerFaults() const;

	// Charges per region never exceed capacity and their sum never exceeds the pool.
	bool ChargesWithinPool() const;

#if SUPERSLM_PAGE_LEDGER_TEST_HOOKS
	void SetMutationForTest(ELedgerMutation Mutation);
	// Shrinks one request kind's admission charge by Pages (floored at 0). Off (0) by default.
	void SetForceUnderChargeForTest(EChargeKind Kind, uint32_t Pages);
#endif

private:
	struct FImpl;
	FImpl* Impl;
};

}  // namespace SuperSLMPaging
