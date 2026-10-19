#pragma once

#include "CoreMinimal.h"

class USuperSLMSubsystem;

#if WITH_DEV_AUTOMATION_TESTS
#include "superslm/sslm_abi.h" // sslm_status
#include "SuperSLMSequenceTypes.h"
#include "Templates/Function.h"

// The paged-KV test seams (plugin paging plan §3.9). Compiled only under
// WITH_DEV_AUTOMATION_TESTS, so they exist in Development builds and not in Shipping. CPU
// subsystem only; the GPU subsystem's twins are declared with the GPU suite. Game thread only,
// except FSuperSLMPoolThreadAccess, which is used only inside a RunOnPoolThread() callback.
//
// The page ledger core's own hook switch, SUPERSLM_PAGE_LEDGER_TEST_HOOKS, is defined to
// WITH_DEV_AUTOMATION_TESTS in the runtime module's build rules (plan §3.9, step K2), which is
// what lets ForceUnderCharge() reach the core.

// The request kinds a hold or a status override names. Each is one queued job kind.
enum class ESuperSLMPagedRequestKind : uint8
{
	Reset,         // ResetSequence(): a reset in place or a create-first normalization
	Return,        // ReturnSequence(): the recycle that frees the slot's holder state
	Adopt,         // AdoptPrefix()
	Restore,       // RestoreSequence(), with or without a hint
	PrefixBuild,   // CreatePrefix(): the root build (sslm_prefix_begin_budgeted)
	NestedBuild,   // CreatePrefixFrom(): the nested build (sslm_prefix_begin_from)
	PrefixRelease, // ReleasePrefix()
};

// The request kinds whose admission charge ForceUnderCharge() shrinks (§3.9: the four
// charge-size mutants).
enum class ESuperSLMPagedChargeKind : uint8
{
	PrefixBuild,         // R(L)
	NestedBuild,         // R(child)
	BudgetRestore,       // R(b) + E
	WholeReserveRestore, // P_blk
};

// The page ledger's regions (§3.2).
enum class ESuperSLMPagedRegion : uint8
{
	S, // sequences
	P, // prefixes and ghosts
	H, // restore headroom
	A, // cache-entry sharing (0 in the paging release)
	C, // cache (0 in the paging release)
	Count
};

// A read-only copy of the ledger (§3.9 Ledger()): region charges, ghosts and dependents. Taken on
// the game thread; bConfigured is false in legacy mode and before Configure().
struct SUPERSLMUNREAL_API FSuperSLMPagedLedgerView
{
	bool bConfigured = false;
	int64 PoolPages = 0;
	int64 Capacity[static_cast<int32>(ESuperSLMPagedRegion::Count)] = {};
	int64 Charged[static_cast<int32>(ESuperSLMPagedRegion::Count)] = {};
	int64 TotalCharged = 0;
	int32 LivePrefixCount = 0;
	int32 GhostCount = 0;
	int32 InFlightRestores = 0;
	int32 WaitingRestores = 0;
	uint64 LedgerFaults = 0;

	int64 CapacityOf(ESuperSLMPagedRegion Region) const { return Capacity[static_cast<int32>(Region)]; }
	int64 ChargedOf(ESuperSLMPagedRegion Region) const { return Charged[static_cast<int32>(Region)]; }
	int64 FreeOf(ESuperSLMPagedRegion Region) const { return CapacityOf(Region) - ChargedOf(Region); }
};

// One armed hold. Id 0 is never a valid hold.
struct SUPERSLMUNREAL_API FSuperSLMPagedHold
{
	int64 Id = 0;
	bool IsValid() const { return Id != 0; }
};

// What a RunOnPoolThread() callback may do with the subsystem's own model and pool. The methods are
// thin calls of the engine verbs named beside each, made on the thread the callback runs on. It
// exists because the engine's symbols are compiled into the runtime module and exported from no
// shared library (SUPERSLM_API is empty), so a test module cannot call them itself; an abstract
// interface reaches them through its vtable with no link dependency. Sequences a callback creates
// are released by ReleaseAll(), and by the seam when the callback returns.
class SUPERSLMUNREAL_API FSuperSLMPoolThreadAccess
{
public:
	virtual ~FSuperSLMPoolThreadAccess() = default;

	virtual int64 PagePositions() const = 0;                 // sslm_kv_page_positions(model)
	virtual int64 PageBytes() const = 0;                     // sslm_kv_page_size(model)
	virtual int64 PagesForBudget(int32 Budget) const = 0;    // sslm_kv_pages_for_budget(model, Budget); 0 outside [1, cap]

	// sslm_seq_create_budgeted(model, &pool, Budget, &seq). The sequence is held until ReleaseAll().
	virtual sslm_status CreateBudgeted(int32 Budget) = 0;
	// sslm_seq_create(model, &pool, &seq): a no-budget (whole-reserve) holder, for legacy pools.
	virtual sslm_status CreateWholeReserve() = 0;
	virtual int32 HeldCount() const = 0;
	// sslm_seq_release on every sequence this callback created, newest first.
	virtual void ReleaseAll() = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMPagedTestSeam
{
	// Runs Fn on a CPU lane with the subsystem quiesced: no job in flight except jobs held by
	// HoldApply() (their Layer-1 work is done) or HoldStart() (their Layer-1 work has not started;
	// a held job never blocks Fn, whichever thread it is held on). Blocks the game thread until Fn
	// returns. The fill probe (§3.9) is test code run through this seam. Works in legacy mode too,
	// on the legacy pool. False, with Fn not run, when the subsystem is not configured.
	static bool RunOnPoolThread(USuperSLMSubsystem& Subsystem, TFunction<void(FSuperSLMPoolThreadAccess&)> Fn);

	// Arms one hold on the next job of Kind admitted after this call (each call arms one more,
	// taken in admission order). HoldStart holds the job after its admission charge is taken and
	// before its Layer-1 work, so the window between enqueue and the engine's free or draw can be
	// read (CPU only, plan cell 1.5). HoldApply lets the job finish its Layer-1 work, then holds its
	// apply. An invalid hold when the subsystem is not configured. Configure() and Deinitialize()
	// release every hold and disarm every armed one before they tear the pool down (cell 1.11).
	static FSuperSLMPagedHold HoldStart(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind);
	static FSuperSLMPagedHold HoldApply(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind);

	// True once a job has reached Hold's hold point: it is held there.
	static bool IsHoldReached(const USuperSLMSubsystem& Subsystem, const FSuperSLMPagedHold& Hold);

	// Lets the held job continue (an armed hold not yet reached is disarmed). False for an unknown
	// or already released hold.
	static bool ReleaseHold(USuperSLMSubsystem& Subsystem, const FSuperSLMPagedHold& Hold);

	// The ledger (§3.9 Ledger()), and its per-node and per-slot readings. A prefix handle keeps
	// naming its node after ReleasePrefix() for as long as the node is a ghost. -1 (or false) for
	// an unknown handle, and in legacy mode.
	static FSuperSLMPagedLedgerView Ledger(const USuperSLMSubsystem& Subsystem);
	static int64 PrefixChargedPages(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix);
	static int32 PrefixDependentCount(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix);
	static bool IsPrefixGhost(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Prefix);
	static int64 SequenceHeadroomHold(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);
	static bool SequenceHasOpenJob(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);

	// The warm slot a vended sequence occupies (INDEX_NONE when not vended), and a counter that
	// rises each time that slot's engine holder is replaced (create, restore, normalization). A
	// refused restore leaves it unchanged (restore-first, cell 1.12).
	static int32 SlotIndexOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);
	static int64 SlotHolderGeneration(const USuperSLMSubsystem& Subsystem, int32 SlotIndex);

	// §3.9 ForceUnderCharge(): shrinks Kind's admission charge by Pages (floored at 0); 0 turns it
	// off. Off by default, and cleared by Configure(). False when the subsystem is not in budget
	// mode.
	static bool ForceUnderCharge(USuperSLMSubsystem& Subsystem, ESuperSLMPagedChargeKind Kind, uint32 Pages);

	// A one-shot Layer-1 status for the next job of Kind: that job returns Status in place of its
	// Layer-1 call (a failed begin_from, cell 1.6; a restore whose draw exhausts, cell 1.12), and the
	// engine is left as the call would have left it on that refusal. The twin of
	// FSuperSLMSchedulingTestAccess::SetNextResetStatusOverride() for every paged request kind.
	// False when the subsystem is not configured.
	static bool SetNextLayer1StatusOverride(USuperSLMSubsystem& Subsystem, ESuperSLMPagedRequestKind Kind, sslm_status Status);

	// Payload edits for the hostile-input cells (2.1, 2.2, 1.6, 1.12). The wrapper's layout is this
	// module's detail, so a test never encodes it: EditPayload extracts the Layer-1 payload from a
	// wrapped blob, lets Edit change it (its header offsets are the plan's §3.5 table, which the
	// test states), and writes it back with the wrapper's recorded payload size updated and every
	// other wrapper field unchanged. SetWrapperContextUsed rewrites the wrapper's ContextUsed
	// field only. False for a blob that is not a well-formed wrapper.
	static bool EditPayload(TArray<uint8>& WrappedBlob, TFunctionRef<void(TArray<uint8>& Payload)> Edit);
	static bool SetWrapperContextUsed(TArray<uint8>& WrappedBlob, int64 ContextUsed);
};

#endif // WITH_DEV_AUTOMATION_TESTS
