#include "SuperSLMQuery.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMGpuSelfCheckAccess.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMPromptResult.h"
#include "SuperSLMSubsystem.h"

// L2-S3 (plan §7 item 3, §8, §10.4; D-SLM7244, D-SLM7382, D-SLM7432); moved here from the editor
// query window's controller by code review S2. What the runner decides, in one place:
//
// - The checkbox (§10.4, D-SLM7432). On: every sequence of the query binds `prompt_result` at its
//   vend -- the bind-eligible point (§5 CPU path item 6) -- on either backend. Off: the query runs
//   UNBOUND, free decode on the typed text, as §10.4 states. No other schema is bound (review N4).
// - The prompt is the typed text, tokenized as typed (SuperSLM::TokenizeText, the same Layer-1
//   call USuperSLMSubsystem::Tokenize() makes) -- no chat template is applied.
// - Stop set: FSuperSLMQueryConfig::StopTokenIds, supplied by the caller (§8: the game supplies
//   the stop ids, because the artifact records none; review round 2, R2-W2). The editor window
//   supplies Qwen2.5-Instruct's end-of-turn and end-of-text ids. Under a schema no special id can
//   be produced (1.7.x's compile call excludes them from every state), so a constrained query ends
//   at its walk's end or at the budget.
// - Frame Budget: on the CPU, SetLayerBudget() (<= 0 selects the model's whole depth); on the
//   GPU, <= 0 selects the one-call path ("whole token"), and a positive value the composed path
//   with the TICK's budget set to that many layers (review S4: USuperSLMGpuSubsystem::
//   SetLayersPerTick()), shared by the query's N sequences through the batch call, so a tick's
//   GPU work does not grow with N. Each sequence's slice is also capped at that value
//   (SetLayersPerSlice()), which is what bounds it if the tick's budget could not be applied (W4).
// - k: the CPU's K is derived by the planner (D-SLM7407) and is not a control. On the GPU the k
//   takes effect (fold-round ruling 2; R-S3e varies k): before a GPU query vends, the runner
//   re-sets the subsystem's K to EffectiveK (USuperSLMGpuSubsystem::SetFixedTickLatency()). K and
//   the tick's layer budget are scheduler constants with no device state behind them, so setting
//   them is O(1) game-thread work, not a device reconfigure. Both are refused while any GPU
//   sequence is still vended, because a live sequence's scheduled apply ticks were computed under
//   the old values; the query then waits, ticking the GPU subsystem once per frame so deferred
//   returns drain, and vends once the pool is empty. Review W4 bounds that wait
//   (GpuDrainWaitSeconds): past it the query runs at the current schedule and the readout says so.
//   The readout shows the k (EffectiveK: RequestedK clamped to its floor), the floor (MinimumK)
//   and what the scheduler actually applied (AppliedK, AppliedLayersPerTick).
// - Concurrent queries: N sequences run the same request on the one backend; their token
//   sequences must be identical, or the query fails naming the divergence.
// - The self-check is never run by a query.
namespace
{
	constexpr float kHeadlessStepSeconds = 1.0f / 60.0f;
	constexpr double kHeadlessWallCapSeconds = 600.0; // FSuperSLMSelfCheckAccess::RunGeneration() keeps an equal cap (ruling 2026-09-26)
	constexpr int32 kMaxGpuK = 1 << 20; // USuperSLMGpuSubsystem's kMaxJobTicks (D-SLM7803)
	const TCHAR* const kPromptResultSchema = TEXT("prompt_result");

	int32 CeilDiv(int64 A, int64 B)
	{
		return B > 0 ? static_cast<int32>((A + B - 1) / B) : 0;
	}
}

static_assert(static_cast<uint8>(ESuperSLMQueryStopReason::Completed) == static_cast<uint8>(ESuperSLMQueryStopReasonBP::Completed), "ESuperSLMQueryStopReasonBP must mirror ESuperSLMQueryStopReason");
static_assert(static_cast<uint8>(ESuperSLMQueryStopReason::SchemaRejected) == static_cast<uint8>(ESuperSLMQueryStopReasonBP::SchemaRejected), "ESuperSLMQueryStopReasonBP must mirror ESuperSLMQueryStopReason");
static_assert(static_cast<uint8>(ESuperSLMQueryStopReason::BudgetExhausted) == static_cast<uint8>(ESuperSLMQueryStopReasonBP::BudgetExhausted), "ESuperSLMQueryStopReasonBP must mirror ESuperSLMQueryStopReason");

struct FSuperSLMQueryRunner::FActiveQuery
{
	FString PromptText;
	int32 MaxNewTokens = 0;
	FSuperSLMQueryConfig Config;
	ESuperSLMBackendBP Backend = ESuperSLMBackendBP::CPU; // the backend running now
	bool bMovedBackend = false;

	FSuperSLMGenerationRequest Request;
	TArray<FSuperSLMSequence> CpuSequences;
	TArray<FSuperSLMGpuSequence> GpuSequences;

	int32 Frames = 0;
	double StartSeconds = 0.0;
	int32 HitchesAtStart = 0;
	int64 DispatchesSeen = 0;
	TArray<double> GpuBusySamples;
	bool bGpuWasActive = false;
	int32 LayersPerSlice = 0; // GPU composed path; 0 on one-call and on the CPU
	int32 LayerBudget = 0;    // CPU

	// CPU: this query's worker-job figures, read from the scheduler's job ledger as the jobs are
	// delivered. The ledger is a fixed ring that overwrites its oldest rows, so the query reads
	// each tick rather than once at the end. LedgerCursor is the next row not yet read.
	int64 LedgerCursor = 0;
	int64 LedgerRowsMissed = 0; // rows the ring overwrote before they were read
	double WorkerMs = 0.0;
	int32 WorkerJobs = 0;
	int32 MaxK = 0;
	int32 LateJobs = 0;
	int32 OverBudgetJobs = 0;

	// Reads the ledger rows from LedgerCursor in order. While the query runs it stops at the first
	// undelivered row, whose figures are not final yet; bFinal reads every row left, delivered or
	// not, as a scan of the whole ledger at the end would.
	void ReadCpuLedger(const USuperSLMSubsystem& Cpu, bool bFinal)
	{
		const int64 End = Cpu.GetJobLedgerAppendedCount();
		for (; LedgerCursor < End; ++LedgerCursor)
		{
			const FSuperSLMWorkerJobReport* Job = Cpu.FindJobLedgerRow(LedgerCursor);
			if (Job == nullptr)
			{
				++LedgerRowsMissed;
				continue;
			}
			if (!bFinal && Job->DeliveredAtTick < 0)
			{
				break;
			}
			// Any job serving any of this query's N sequences (review R4-N2).
			const bool bThisQuery = Job->MemberSequences.ContainsByPredicate([this](const FSuperSLMSequence& Member)
			{
				return CpuSequences.Contains(Member);
			});
			if (Job->Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || !bThisQuery)
			{
				continue;
			}
			MaxK = FMath::Max(MaxK, Job->K);
			LateJobs += Job->bHitch ? 1 : 0;
			OverBudgetJobs += Job->bWorkerOverran ? 1 : 0;
			if (Job->WorkerCallMs >= 0.0)
			{
				WorkerMs += Job->WorkerCallMs;
				++WorkerJobs;
			}
		}
	}

	// GPU: the schedule this query runs under -- the tick's layer budget (0 on one-call) and K --
	// and whether the query is waiting for the GPU pool to drain before it can apply them (nothing
	// is vended while it waits), since when, and why the last attempt was refused (W4).
	int32 GpuTargetLayersPerTick = 0;
	int32 GpuTargetK = 0;
	int32 GpuMinimumK = 0;
	bool bAwaitingGpuDrain = false;
	double DrainWaitStartSeconds = 0.0;
	FString DrainRefusal;
	bool bScheduleNotApplied = false;
	FString ScheduleMessage;
	// R2-W3: this query may have overridden the GPU backend's shared schedule, so its end asks the
	// subsystem to restore the Configure()'d one.
	bool bGpuScheduleTouched = false;
};

FSuperSLMQueryRunner::FSuperSLMQueryRunner(USuperSLMSubsystem& InCpuSubsystem, USuperSLMGpuSubsystem* InGpuSubsystem, USuperSLMModel& InModel)
	: CpuSubsystem(InCpuSubsystem)
	, GpuSubsystem(InGpuSubsystem)
	, Model(InModel)
{
}

FSuperSLMQueryRunner::~FSuperSLMQueryRunner()
{
	CancelQuery();
}

bool FSuperSLMQueryRunner::RunQuery(
	const FString& PromptText,
	int32 MaxNewTokens,
	const FSuperSLMQueryConfig& Config,
	FSuperSLMQueryReadout& OutReadout,
	FString& OutError)
{
	OutReadout = FSuperSLMQueryReadout();
	if (!BeginQuery(PromptText, MaxNewTokens, Config, OutError))
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	bool bSucceeded = false;
	for (;;)
	{
		// Plan §2.5 row 21, ruling 2026-09-26: the GPU Tick() no longer waits on the submission
		// thread, so this loop, which ticks faster than the device, paces itself: for a GPU query
		// it sleeps 1 ms instead of ticking while the device is the gate
		// (FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(), which forwards to the
		// subsystem's NextTickGatedOnDevice()). The wall cap below still bounds the whole loop.
		const bool bGpuGated = Active.IsValid() && Active->Backend == ESuperSLMBackendBP::GPU && GpuSubsystem != nullptr &&
			FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(*GpuSubsystem);
		if (bGpuGated)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		else if (TickQueryImpl(kHeadlessStepSeconds, /*bDirectTick*/ true, bSucceeded, OutReadout, OutError))
		{
			break;
		}
		if (FPlatformTime::Seconds() - Start > kHeadlessWallCapSeconds)
		{
			CancelQuery();
			OutError = FString::Printf(TEXT("the query did not stop within %.0f s"), kHeadlessWallCapSeconds);
			return false;
		}
	}
	return bSucceeded;
}

bool FSuperSLMQueryRunner::IsQueryRunning() const
{
	return Active.IsValid();
}

bool FSuperSLMQueryRunner::GetLiveView(FSuperSLMQueryLiveView& OutView) const
{
	check(IsInGameThread());
	OutView = FSuperSLMQueryLiveView();
	if (!Active.IsValid())
	{
		return false;
	}
	const FActiveQuery& Q = *Active;
	OutView.Backend = Q.Backend;
	OutView.bMovedBackend = Q.bMovedBackend;
	OutView.PromptTokens = Q.Request.PromptTokens;
	OutView.bSchemaBound = Q.Config.bSchemaConstrainedDecoding;
	OutView.Frames = Q.Frames;
	OutView.bAwaitingGpuDrain = Q.bAwaitingGpuDrain;
	if (Q.Backend == ESuperSLMBackendBP::GPU && GpuSubsystem != nullptr && Q.GpuSequences.Num() > 0)
	{
		const FSuperSLMGpuSequence& Seq = Q.GpuSequences[0];
		OutView.GeneratedTokens = GpuSubsystem->GetGeneratedTokens(Seq);
		OutView.Phase = GpuSubsystem->GetPhase(Seq);
		OutView.bSchemaAccepting = OutView.bSchemaBound && GpuSubsystem->IsSchemaAccepting(Seq);
	}
	else if (Q.CpuSequences.Num() > 0)
	{
		const FSuperSLMSequence& Seq = Q.CpuSequences[0];
		const FSuperSLMSequenceStats Stats = CpuSubsystem.GetStats(Seq);
		OutView.GeneratedTokens = CpuSubsystem.GetGeneratedTokens(Seq);
		OutView.Phase = CpuSubsystem.GetPhase(Seq);
		OutView.bSchemaAccepting = OutView.bSchemaBound && Stats.bSchemaAccepting;
		OutView.ForcedTokenCount = Stats.ForcedTokenCount;
	}
	return true;
}

bool FSuperSLMQueryRunner::BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryConfig& Config, FString& OutError)
{
	check(IsInGameThread());
	if (Active.IsValid())
	{
		OutError = TEXT("a query is already running");
		return false;
	}
	if (MaxNewTokens <= 0 || Config.ConcurrentQueries <= 0)
	{
		OutError = FString::Printf(TEXT("MaxNewTokens (%d) and ConcurrentQueries (%d) must be positive"), MaxNewTokens, Config.ConcurrentQueries);
		return false;
	}
	Active = MakeUnique<FActiveQuery>();
	Active->PromptText = PromptText;
	Active->MaxNewTokens = MaxNewTokens;
	Active->Config = Config;
	if (!SuperSLM::TokenizeText(Model, PromptText, Active->Request.PromptTokens, OutError))
	{
		Active.Reset();
		return false;
	}
	if (Active->Request.PromptTokens.Num() == 0)
	{
		Active.Reset();
		OutError = TEXT("the prompt tokenizes to nothing");
		return false;
	}
	Active->Request.MaxNewTokens = MaxNewTokens;
	Active->Request.SpanKind = ESuperSLMSpanKind::Prompt;
	// R2-W2 (plan §8: "the game supplies" the stop ids, because the artifact records none): the
	// caller's set, verbatim. Empty means no stop id -- the query ends at its budget or its schema
	// walk's end.
	Active->Request.StopTokenIds = Config.StopTokenIds;
	if (!StartOnBackend(Config.Backend, OutError))
	{
		ReturnActiveSequences();
		Active.Reset();
		return false;
	}
	return true;
}

bool FSuperSLMQueryRunner::StartOnBackend(ESuperSLMBackendBP Backend, FString& OutError)
{
	FActiveQuery& Q = *Active;
	Q.Backend = Backend;
	Q.CpuSequences.Reset();
	Q.GpuSequences.Reset();
	Q.GpuBusySamples.Reset();
	Q.Frames = 0;
	Q.StartSeconds = FPlatformTime::Seconds();

	FSuperSLMModelShapeFacts Shape;
	if (!SuperSLMModelInspector::ReadShape(Model, Shape) || Shape.NumHiddenLayers <= 0)
	{
		OutError = TEXT("the model carries no readable CFG1 section");
		return false;
	}
	if (static_cast<int64>(Q.Request.PromptTokens.Num()) + Q.MaxNewTokens > Shape.ContextCap)
	{
		OutError = FString::Printf(TEXT("the prompt (%d tokens) plus MaxNewTokens (%d) exceeds context_cap (%lld)"),
			Q.Request.PromptTokens.Num(), Q.MaxNewTokens, Shape.ContextCap);
		return false;
	}
	const bool bCheckbox = Q.Config.bSchemaConstrainedDecoding;

	if (Backend == ESuperSLMBackendBP::GPU)
	{
		if (GpuSubsystem == nullptr || !GpuSubsystem->IsGpuBackendActive())
		{
			OutError = TEXT("the GPU backend is not configured on this box (no GPU subsystem, or Configure() did not succeed)");
			return false;
		}
		if (bCheckbox && GpuSchemaHandle.IsNone() &&
			!FSuperSLMGpuSchemaLookup::LookupByName(Model, kPromptResultSchema, GpuSchemaHandle, OutError))
		{
			OutError = FString::Printf(TEXT("'%s' does not resolve on the GPU backend; schema-constrained queries need this schema compiled into the model. %s"), kPromptResultSchema, *OutError);
			return false;
		}
		// Review S4: Frame Budget is the tick's layer budget on the composed path; the one-call
		// path leaves the tick's budget as it is (it serves one whole token per tick).
		Q.GpuTargetLayersPerTick = Q.Config.FrameBudgetLayers <= 0 ? 0 : FMath::Min(Q.Config.FrameBudgetLayers, Shape.NumHiddenLayers);
		FString Refusal;
		if (!TryApplyGpuSchedule(Shape, Refusal))
		{
			if (Q.GpuTargetK > kMaxGpuK)
			{
				OutError = Refusal;
				return false;
			}
			// In range, so the refusal is a GPU sequence still vended: wait for the pool to drain
			// (TickQuery), never change the schedule under a live query.
			Q.bAwaitingGpuDrain = true;
			Q.DrainWaitStartSeconds = FPlatformTime::Seconds();
			Q.DrainRefusal = Refusal;
			return true;
		}
		return VendOnGpu(Shape, OutError);
	}

	if (bCheckbox && CpuSchemaHandle.IsNone() &&
		!FSuperSLMSchemaLookup::LookupByName(Model, kPromptResultSchema, CpuSchemaHandle, OutError))
	{
		OutError = FString::Printf(TEXT("'%s' does not resolve on the CPU backend; schema-constrained queries need this schema compiled into the model. %s"), kPromptResultSchema, *OutError);
		return false;
	}
	if (CpuSubsystem.GetConfiguredModel() != &Model)
	{
		OutError = TEXT("the CPU subsystem is not configured with this model");
		return false;
	}
	// The ledger rows this query reads start here, before its sequences are vended.
	Q.LedgerCursor = CpuSubsystem.GetJobLedgerAppendedCount();
	Q.LedgerRowsMissed = 0;
	Q.WorkerMs = 0.0;
	Q.WorkerJobs = 0;
	Q.MaxK = 0;
	Q.LateJobs = 0;
	Q.OverBudgetJobs = 0;
	Q.LayerBudget = Q.Config.FrameBudgetLayers <= 0 ? Shape.NumHiddenLayers : FMath::Min(Q.Config.FrameBudgetLayers, Shape.NumHiddenLayers);
	for (int32 I = 0; I < Q.Config.ConcurrentQueries; ++I)
	{
		FSuperSLMSequence Seq;
		const ESuperSLMVendResult Vend = CpuSubsystem.VendSequence(Seq);
		if (Vend != ESuperSLMVendResult::Success)
		{
			OutError = FString::Printf(TEXT("CPU VendSequence refused query %d of %d (%s)"), I + 1, Q.Config.ConcurrentQueries,
				Vend == ESuperSLMVendResult::PoolExhausted ? TEXT("pool exhausted: raise BlockCount") : TEXT("not configured"));
			return false;
		}
		Q.CpuSequences.Add(Seq);
		CpuSubsystem.SetLayerBudget(Seq, Q.LayerBudget);
		if (bCheckbox && !CpuSubsystem.SetSchema(Seq, CpuSchemaHandle, OutError))
		{
			return false;
		}
		if (!CpuSubsystem.BeginGeneration(Seq, Q.Request, OutError))
		{
			return false;
		}
	}
	Q.HitchesAtStart = CpuSubsystem.GetHitchCount();
	return true;
}

bool FSuperSLMQueryRunner::TryApplyGpuSchedule(const FSuperSLMModelShapeFacts& Shape, FString& OutRefusal)
{
	FActiveQuery& Q = *Active;
	Q.bGpuScheduleTouched = true;
	// The tick's layer budget first: it moves the subsystem's own K floor (D-SLM7381).
	if (Q.GpuTargetLayersPerTick > 0 && !GpuSubsystem->SetLayersPerTick(Q.GpuTargetLayersPerTick, OutRefusal))
	{
		return false;
	}
	// The k floor for this query, the same figure FinishQuery() reports: one token at s layers per
	// slice needs ceil(L / s) of its own slices, and N sequences share the tick's LayersPerTick, so
	// each round of N slices takes ceil(N * s / LayersPerTick) ticks; one-call serves one whole
	// token per tick in rotation, N. Never below the subsystem's floor, which
	// SetFixedTickLatency() enforces.
	const int32 N = Q.Config.ConcurrentQueries;
	const int32 LayersPerTick = FMath::Max(1, GpuSubsystem->GetLayersPerTick());
	const int32 Slice = Q.Config.FrameBudgetLayers <= 0 ? 0 : FMath::Min3(Q.Config.FrameBudgetLayers, Shape.NumHiddenLayers, LayersPerTick);
	const int32 WindowFloor = Slice > 0
		? CeilDiv(Shape.NumHiddenLayers, Slice) * FMath::Max(1, CeilDiv(static_cast<int64>(N) * Slice, LayersPerTick))
		: N;
	Q.GpuMinimumK = FMath::Max(WindowFloor, GpuSubsystem->GetConfiguredMinimumK());
	Q.GpuTargetK = FMath::Max(Q.Config.RequestedK, Q.GpuMinimumK);
	if (Q.GpuTargetK > kMaxGpuK)
	{
		OutRefusal = FString::Printf(TEXT("k %d is above the GPU scheduler's maximum %d (kMaxJobTicks)"), Q.GpuTargetK, kMaxGpuK);
		return false;
	}
	return GpuSubsystem->SetFixedTickLatency(Q.GpuTargetK, OutRefusal);
}

bool FSuperSLMQueryRunner::VendOnGpu(const FSuperSLMModelShapeFacts& Shape, FString& OutError)
{
	// The schedule is applied (or, past the W4 bound, knowingly not); frames and wall time count
	// from here, so a drain wait is not charged to frames-to-answer.
	FActiveQuery& Q = *Active;
	const bool bCheckbox = Q.Config.bSchemaConstrainedDecoding;
	Q.bAwaitingGpuDrain = false;
	Q.Frames = 0;
	Q.StartSeconds = FPlatformTime::Seconds();
	const ESuperSLMGpuDecodePath Path = Q.Config.FrameBudgetLayers <= 0 ? ESuperSLMGpuDecodePath::OneCall : ESuperSLMGpuDecodePath::Composed;
	for (int32 I = 0; I < Q.Config.ConcurrentQueries; ++I)
	{
		FSuperSLMGpuSequence Seq;
		const ESuperSLMGpuVendResult Vend = GpuSubsystem->VendSequence(Seq, Path);
		if (Vend != ESuperSLMGpuVendResult::Success)
		{
			OutError = FString::Printf(TEXT("GPU VendSequence refused query %d of %d (%s)"), I + 1, Q.Config.ConcurrentQueries,
				Vend == ESuperSLMGpuVendResult::PoolExhausted ? TEXT("pool exhausted: raise BlockCount") : TEXT("not configured"));
			return false;
		}
		Q.GpuSequences.Add(Seq);
		if (Path == ESuperSLMGpuDecodePath::Composed &&
			!GpuSubsystem->SetLayersPerSlice(Seq, FMath::Min(Q.Config.FrameBudgetLayers, Shape.NumHiddenLayers), OutError))
		{
			return false;
		}
		if (bCheckbox && !GpuSubsystem->SetSchema(Seq, GpuSchemaHandle, OutError))
		{
			return false;
		}
		if (!GpuSubsystem->RequestBeginGeneration(Seq, Q.Request).IsValid())
		{
			OutError = FString::Printf(TEXT("GPU RequestBeginGeneration refused: %s"), *GpuSubsystem->GetLastLifecycleRequestError());
			return false;
		}
	}
	Q.LayersPerSlice = Path == ESuperSLMGpuDecodePath::Composed ? GpuSubsystem->GetLayersPerSlice(Q.GpuSequences[0]) : 0;
	Q.bGpuWasActive = true;
	Q.HitchesAtStart = GpuSubsystem->GetHitchCount();
	Q.DispatchesSeen = GpuSubsystem->GetGpuDispatchCount();
	return true;
}

void FSuperSLMQueryRunner::ReturnActiveSequences()
{
	if (!Active.IsValid())
	{
		return;
	}
	for (const FSuperSLMSequence& Seq : Active->CpuSequences)
	{
		CpuSubsystem.ReturnSequence(Seq);
	}
	if (GpuSubsystem != nullptr)
	{
		for (const FSuperSLMGpuSequence& Seq : Active->GpuSequences)
		{
			GpuSubsystem->ReturnSequence(Seq);
		}
	}
	Active->CpuSequences.Reset();
	Active->GpuSequences.Reset();
}

void FSuperSLMQueryRunner::CancelQuery()
{
	ReturnActiveSequences();
	if (Active.IsValid() && Active->bGpuScheduleTouched && GpuSubsystem != nullptr)
	{
		// R2-W3: LayersPerTick and K are shared by every client of the GPU backend; put the
		// Configure()'d schedule back once this query's returns have drained.
		GpuSubsystem->RequestConfiguredScheduleRestore();
	}
	Active.Reset();
}

void FSuperSLMQueryRunner::Abandon()
{
	Active.Reset();
}

bool FSuperSLMQueryRunner::TickQuery(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMQueryReadout& OutReadout, FString& OutError)
{
	return TickQueryImpl(DeltaSeconds, /*bDirectTick*/ false, bOutSucceeded, OutReadout, OutError);
}

bool FSuperSLMQueryRunner::TickQueryImpl(float DeltaSeconds, bool bDirectTick, bool& bOutSucceeded, FSuperSLMQueryReadout& OutReadout, FString& OutError)
{
	check(IsInGameThread());
	bOutSucceeded = false;
	if (!Active.IsValid())
	{
		OutError = TEXT("no query is running");
		return true;
	}
	// Review W6: the per-frame form shares one subsystem Tick() per engine frame with every other
	// per-frame client; the headless loop ticks directly.
	auto TickGpu = [this, bDirectTick, DeltaSeconds]()
	{
		if (bDirectTick)
		{
			GpuSubsystem->Tick(DeltaSeconds);
		}
		else
		{
			GpuSubsystem->TickOncePerFrame(DeltaSeconds);
		}
	};
	auto TickCpu = [this, bDirectTick, DeltaSeconds]()
	{
		if (bDirectTick)
		{
			CpuSubsystem.Tick(DeltaSeconds);
		}
		else
		{
			CpuSubsystem.TickOncePerFrame(DeltaSeconds);
		}
	};

	FActiveQuery& Q = *Active;
	bool bAllStopped = true;
	if (Q.Backend == ESuperSLMBackendBP::GPU && Q.bAwaitingGpuDrain)
	{
		// Waiting to apply this query's schedule: tick so deferred returns drain, then retry.
		// Nothing of this query is vended yet, so a loss here re-runs on the CPU like any other.
		TickGpu();
		if (!GpuSubsystem->IsGpuBackendActive())
		{
			Q.bMovedBackend = true;
			Q.bAwaitingGpuDrain = false;
			if (!StartOnBackend(ESuperSLMBackendBP::CPU, OutError))
			{
				OutError = FString::Printf(TEXT("the GPU backend was lost before the query started and the CPU re-run could not start: %s"), *OutError);
				CancelQuery();
				return true;
			}
			return false;
		}
		FSuperSLMModelShapeFacts Shape;
		SuperSLMModelInspector::ReadShape(Model, Shape);
		FString Refusal;
		const bool bApplied = TryApplyGpuSchedule(Shape, Refusal);
		if (!bApplied)
		{
			Q.DrainRefusal = Refusal;
			if (FPlatformTime::Seconds() - Q.DrainWaitStartSeconds < GpuDrainWaitSeconds)
			{
				return false;
			}
			// Review W4: another holder keeps the pool from draining. Run at the current schedule and
			// say so, rather than wait without bound.
			Q.bScheduleNotApplied = true;
			Q.ScheduleMessage = FString::Printf(TEXT("k %d / Frame Budget %d not applied after %.1f s waiting for the GPU pool to drain (%s); "
				"the query ran at the current K %d and %d layers per tick."),
				Q.GpuTargetK, Q.Config.FrameBudgetLayers, GpuDrainWaitSeconds, *Q.DrainRefusal,
				GpuSubsystem->GetConfiguredK(), GpuSubsystem->GetLayersPerTick());
		}
		if (!VendOnGpu(Shape, OutError))
		{
			CancelQuery();
			return true;
		}
		return false;
	}
	if (Q.Backend == ESuperSLMBackendBP::GPU)
	{
		TickGpu();
		Q.Frames += 1;

		// D-SLM7382: a probe-confirmed loss latches IsGpuBackendActive() false. The query is
		// re-issued on the CPU from its original request; the GPU's partial answer is dropped.
		bool bTerminalLoss = Q.bGpuWasActive && !GpuSubsystem->IsGpuBackendActive();
		for (const FSuperSLMGpuSequence& Seq : Q.GpuSequences)
		{
			bTerminalLoss |= GpuSubsystem->GetLastFaultReason(Seq) == ESuperSLMGpuFaultReason::TerminalDeviceLost;
		}
		if (bTerminalLoss)
		{
			Q.GpuSequences.Reset(); // the backend tore its pool down; there is nothing to return
			Q.bMovedBackend = true;
			if (!StartOnBackend(ESuperSLMBackendBP::CPU, OutError))
			{
				OutError = FString::Printf(TEXT("the GPU backend was lost mid-query and the CPU re-run could not start: %s"), *OutError);
				CancelQuery();
				return true;
			}
			return false;
		}

		// Plan §9 R-S3f: a slice's own gpu_busy_ms, read whenever this tick issued GPU work.
		const int64 Dispatches = GpuSubsystem->GetGpuDispatchCount();
		if (Dispatches != Q.DispatchesSeen)
		{
			Q.DispatchesSeen = Dispatches;
			Q.GpuBusySamples.Add(GpuSubsystem->GetLastGpuBusyMs());
		}
		for (const FSuperSLMGpuSequence& Seq : Q.GpuSequences)
		{
			const ESuperSLMSequencePhase Phase = GpuSubsystem->GetPhase(Seq);
			bAllStopped &= Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}
	}
	else
	{
		TickCpu();
		Q.Frames += 1;
		Q.ReadCpuLedger(CpuSubsystem, /*bFinal*/ false);
		for (const FSuperSLMSequence& Seq : Q.CpuSequences)
		{
			const ESuperSLMSequencePhase Phase = CpuSubsystem.GetPhase(Seq);
			bAllStopped &= Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}
	}
	if (!bAllStopped)
	{
		return false;
	}
	OutReadout = FSuperSLMQueryReadout();
	bOutSucceeded = FinishQuery(OutReadout, OutError);
	CancelQuery(); // returns the sequences
	return true;
}

bool FSuperSLMQueryRunner::FinishQuery(FSuperSLMQueryReadout& Out, FString& OutError)
{
	FActiveQuery& Q = *Active;
	const bool bGpu = Q.Backend == ESuperSLMBackendBP::GPU;
	const bool bCheckbox = Q.Config.bSchemaConstrainedDecoding;
	const double WallSeconds = FPlatformTime::Seconds() - Q.StartSeconds;
	Out.BackendRun = Q.Backend;
	Out.bMovedBackend = Q.bMovedBackend;
	if (Q.bMovedBackend)
	{
		Out.MovedBackendMessage = TEXT("The GPU backend was lost (probe-confirmed) during this query, so the query was re-run from its "
			"original request on the CPU. Nothing of the GPU's partial answer is carried over; every figure is the CPU run's.");
	}
	FSuperSLMModelShapeFacts Shape;
	SuperSLMModelInspector::ReadShape(Model, Shape);

	// Each sequence's stop, and identity across the N (§8: concurrency moves no token).
	const int32 N = bGpu ? Q.GpuSequences.Num() : Q.CpuSequences.Num();
	FString FirstDigest;
	SuperSLMPromptResult::FStopFacts Facts;
	for (int32 I = 0; I < N; ++I)
	{
		const TArray<int32>& Tokens = bGpu ? GpuSubsystem->GetGeneratedTokens(Q.GpuSequences[I]) : CpuSubsystem.GetGeneratedTokens(Q.CpuSequences[I]);
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		const FString Hex = SuperSLMGpuDigest::DigestToHex(Digest);
		if (I == 0)
		{
			FirstDigest = Hex;
			Out.GeneratedTokens = Tokens;
			Facts.Phase = bGpu ? GpuSubsystem->GetPhase(Q.GpuSequences[0]) : CpuSubsystem.GetPhase(Q.CpuSequences[0]);
			Facts.LastOutcome = bGpu ? GpuSubsystem->GetLastDecodeOutcome(Q.GpuSequences[0]) : CpuSubsystem.GetLastDecodeOutcome(Q.CpuSequences[0]);
			Facts.bSchemaAccepting = bGpu ? GpuSubsystem->IsSchemaAccepting(Q.GpuSequences[0]) : CpuSubsystem.GetStats(Q.CpuSequences[0]).bSchemaAccepting;
			Facts.bContextCapReached = Shape.ContextCap > 0 && static_cast<int64>(Q.Request.PromptTokens.Num()) + Tokens.Num() >= Shape.ContextCap;
		}
		else if (Hex != FirstDigest)
		{
			OutError = FString::Printf(TEXT("concurrent query %d of %d produced a different token sequence (digest %s, first query %s)"), I + 1, N, *Hex, *FirstDigest);
			return false;
		}
	}
	Out.TokenDigestHex = FirstDigest;

	if (bCheckbox)
	{
		ESuperSLMQueryStopReason Reason;
		if (!SuperSLMPromptResult::ComposeStopReason(Facts, Reason))
		{
			OutError = FString::Printf(TEXT("the query faulted (last outcome %d%s) and is not a classified stop"), static_cast<int32>(Facts.LastOutcome),
				bGpu ? *FString::Printf(TEXT(", GPU fault %d"), static_cast<int32>(GpuSubsystem->GetLastFaultReason(Q.GpuSequences[0]))) : TEXT(""));
			return false;
		}
		Out.StopReason = Reason;
		Out.SchemaAcceptingAtStop = Facts.bSchemaAccepting;
		Out.bHasStopReason = true;
		Out.StopReasonBP = static_cast<ESuperSLMQueryStopReasonBP>(Reason);
		Out.bHasSchemaAcceptingAtStop = true;
		Out.bSchemaAcceptingAtStopBP = Facts.bSchemaAccepting;
	}
	else if (Facts.Phase != ESuperSLMSequencePhase::Complete)
	{
		OutError = FString::Printf(TEXT("the query faulted (last outcome %d)"), static_cast<int32>(Facts.LastOutcome));
		return false;
	}

	if (!SuperSLM::DetokenizeTokens(Model, Out.GeneratedTokens, Out.RawOutputText, OutError))
	{
		return false;
	}
	if (bCheckbox)
	{
		FString Parsed, ParseError;
		if (Out.StopReason.GetValue() == ESuperSLMQueryStopReason::Completed &&
			SuperSLMPromptResult::ParseCompletedOutput(Out.RawOutputText, Parsed, ParseError))
		{
			Out.DisplayedText = Parsed;
			Out.ResultJson = Out.RawOutputText;
		}
		else
		{
			// BudgetExhausted or SchemaRejected -- or a Completed output the parser refused, which
			// the display never shows raw either (T-2853 design §5).
			SuperSLMPromptResult::RecoverTruncatedOutput(Out.RawOutputText, Out.DisplayedText);
		}
	}
	else
	{
		Out.DisplayedText = Out.RawOutputText;
		Out.VoicedReply = Out.RawOutputText;
	}

	// Cost figures, each from its own source (§8).
	Out.FramesToAnswer = Q.Frames;
	const int32 L = Shape.NumHiddenLayers;
	if (bGpu)
	{
		Out.PromptTimeMs = GpuSubsystem->GetTimeToFirstTokenMs(Q.GpuSequences[0]);
		Out.HostFinishMsPerTokenMs = GpuSubsystem->GetLastHostFinishMs();
		Out.GpuBusyMsSamplesPerSlice = Q.LayersPerSlice > 0 ? Q.GpuBusySamples : TArray<double>();
		Out.GpuMsPerSliceMs = Q.GpuBusySamples.Num() > 0 ? Q.GpuBusySamples.Last() : 0.0;
		Out.HitchCount = GpuSubsystem->GetHitchCount() - Q.HitchesAtStart;
		// The GPU subsystem keeps no throughput figure; this is the query's own tokens over its own
		// wall time, begin to stop.
		Out.TokensPerSecond = WallSeconds > 0.0 ? static_cast<double>(Out.GeneratedTokens.Num() * N) / WallSeconds : 0.0;
		// One token at LayersPerSlice layers per slice needs ceil(L / s) of its own slices; N
		// sequences share the tick's LayersPerTick, so each round of N slices takes
		// ceil(N * s / LayersPerTick) ticks. One-call: one whole token per tick in rotation, N.
		// The scheduler's own floor is USuperSLMGpuSubsystem::GetConfiguredMinimumK().
		const int32 LayersPerTick = FMath::Max(1, GpuSubsystem->GetLayersPerTick());
		Out.MinimumK = Q.LayersPerSlice > 0
			? CeilDiv(L, Q.LayersPerSlice) * FMath::Max(1, CeilDiv(static_cast<int64>(N) * Q.LayersPerSlice, LayersPerTick))
			: N;
		// SetFixedTickLatency() cannot go below the subsystem's floor, so neither can the query's.
		Out.MinimumK = FMath::Max(Out.MinimumK, GpuSubsystem->GetConfiguredMinimumK());
		// The schedule the scheduler ran this query under: TryApplyGpuSchedule() applied it before
		// the vend and nothing changes it while a sequence is vended, so these equal the targets
		// unless the W4 bound ran the query at the current schedule instead.
		Out.AppliedK = GpuSubsystem->GetConfiguredK();
		Out.AppliedLayersPerTick = Q.LayersPerSlice > 0 ? GpuSubsystem->GetLayersPerTick() : 0;
		Out.bScheduleNotApplied = Q.bScheduleNotApplied;
		Out.ScheduleMessage = Q.ScheduleMessage;
	}
	else
	{
		Out.PromptTimeMs = CpuSubsystem.GetTimeToFirstTokenMs(Q.CpuSequences[0]);
		Out.TokensPerSecond = CpuSubsystem.GetTokensPerSecond();
		Out.HitchCount = CpuSubsystem.GetHitchCount() - Q.HitchesAtStart;
		// This query's own jobs in the ledger -- the planner's K for each, its worker wall time and
		// its two hitch sources -- read as they were delivered, and now the rest.
		Q.ReadCpuLedger(CpuSubsystem, /*bFinal*/ true);
		if (Q.LedgerRowsMissed > 0)
		{
			UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM query: the job figures leave out %lld job(s) the ledger overwrote before the query read them."), Q.LedgerRowsMissed);
		}
		Out.WorkerMsPerJobMs = Q.WorkerJobs > 0 ? Q.WorkerMs / Q.WorkerJobs : 0.0;
		Out.CpuLateJobs = Q.LateJobs;
		Out.CpuOverBudgetJobs = Q.OverBudgetJobs;
		// The CPU's K is the planner's (D-SLM7407): the floor is what it derived, and the k control
		// cannot lower it.
		Out.MinimumK = FMath::Max(1, Q.MaxK);
		Out.AppliedK = Out.MinimumK;
	}
	Out.EffectiveK = FMath::Max(Q.Config.RequestedK, Out.MinimumK);
	return true;
}

// ---------------------------------------------------------------------------------------------
// USuperSLMQuery -- the Blueprint face
// ---------------------------------------------------------------------------------------------

USuperSLMQuery* USuperSLMQuery::CreateQuery(UObject* WorldContextObject, USuperSLMModel* InModel, FString& OutError)
{
	check(IsInGameThread());
	if (InModel == nullptr)
	{
		OutError = TEXT("no model");
		return nullptr;
	}
	UWorld* World = GEngine != nullptr ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	if (GameInstance == nullptr)
	{
		OutError = TEXT("no GameInstance: the two backends are GameInstance subsystems");
		return nullptr;
	}
	USuperSLMSubsystem* CpuSubsystem = GameInstance->GetSubsystem<USuperSLMSubsystem>();
	if (CpuSubsystem == nullptr || CpuSubsystem->GetConfiguredModel() != InModel)
	{
		OutError = TEXT("the CPU subsystem is not configured with this model");
		return nullptr;
	}
	// The GPU only when it is active with the same model; otherwise a GPU query fails naming why,
	// and a loss later re-runs on the CPU (D-SLM7382).
	USuperSLMGpuSubsystem* GpuSubsystem = GameInstance->GetSubsystem<USuperSLMGpuSubsystem>();
	if (GpuSubsystem != nullptr && (!GpuSubsystem->IsGpuBackendActive() || GpuSubsystem->GetConfiguredModel() != InModel))
	{
		GpuSubsystem = nullptr;
	}
	USuperSLMQuery* Query = NewObject<USuperSLMQuery>(GameInstance);
	Query->Model = InModel;
	Query->Cpu = CpuSubsystem;
	Query->Gpu = GpuSubsystem;
	Query->Runner = MakeUnique<FSuperSLMQueryRunner>(*CpuSubsystem, GpuSubsystem, *InModel);
	return Query;
}

bool USuperSLMQuery::EnsureAlive(FString& OutError)
{
	const bool bGpuGone = Gpu.IsExplicitlyNull() ? false : !Gpu.IsValid();
	if (Runner.IsValid() && Cpu.IsValid() && !bGpuGone && Model != nullptr)
	{
		return true;
	}
	if (Runner.IsValid())
	{
		Runner->Abandon(); // the pools went with the subsystems
		Runner.Reset();
	}
	OutError = TEXT("the backends this query was created for are gone; create a new query");
	return false;
}

bool USuperSLMQuery::BeginQuery(const FString& PromptText, int32 MaxNewTokens, const FSuperSLMQueryConfig& Config, FString& OutError)
{
	return EnsureAlive(OutError) && Runner->BeginQuery(PromptText, MaxNewTokens, Config, OutError);
}

bool USuperSLMQuery::TickQuery(float DeltaSeconds, bool& bSucceeded, FSuperSLMQueryReadout& Readout, FString& OutError)
{
	bSucceeded = false;
	if (!EnsureAlive(OutError))
	{
		return true;
	}
	return Runner->TickQuery(DeltaSeconds, bSucceeded, Readout, OutError);
}

void USuperSLMQuery::CancelQuery()
{
	FString Ignored;
	if (EnsureAlive(Ignored))
	{
		Runner->CancelQuery();
	}
}

bool USuperSLMQuery::IsQueryRunning() const
{
	return Runner.IsValid() && Runner->IsQueryRunning();
}

void USuperSLMQuery::BeginDestroy()
{
	if (Runner.IsValid())
	{
		// Return the sequences while the subsystems live; otherwise their pools already went.
		const bool bGpuGone = Gpu.IsExplicitlyNull() ? false : !Gpu.IsValid();
		if (Cpu.IsValid() && !bGpuGone)
		{
			Runner->CancelQuery();
		}
		else
		{
			Runner->Abandon();
		}
		Runner.Reset();
	}
	Super::BeginDestroy();
}
