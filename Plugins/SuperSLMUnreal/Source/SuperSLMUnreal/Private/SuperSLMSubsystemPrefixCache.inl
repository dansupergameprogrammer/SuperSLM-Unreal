// The CPU subsystem's prefix-cache glue (precache plan §5.5, §5.6, §5.10, §5.11). Included at the
// end of SuperSLMSubsystem.cpp, only with SUPERSLM_WITH_PREFIX_CACHE on, so it sees that file's own
// internals: FSuperSLMSubsystemState, the lanes, PostLedgerJob and the paging test holds. The
// declarations are in SuperSLMSubsystemPrefixCache.h.

#include "Core/SuperSLMRestorePricing.h"
#include "Core/SuperSLMSaveCompatCore.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperSLMPrefixSet.h"
#include "superslm/sha256.h"

namespace SuperSLMPrefixCacheGlue
{
	static uint64 PrefixGlueHandleBits(sslm_prefix Handle)
	{
		return static_cast<uint64>(reinterpret_cast<UPTRINT>(Handle));
	}

	static FSuperSLMPrefixLaneContext PrefixGlueLaneContext(FSuperSLMSubsystemState& S, int32 LaneIndex)
	{
		FSuperSLMPrefixLaneContext Context;
		Context.Model = S.Model;
		Context.Pool = &S.Pool;
		Context.Workspace = S.Lanes.IsValidIndex(LaneIndex) ? S.Lanes[LaneIndex].Workspace : nullptr;
		Context.MaxPrefillChunk = FMath::Max(1, S.Config.MaxPrefillChunkBudget);
		Context.bBudgetMode = S.IsBudgetMode();
		return Context;
	}

	int64 LegacyCacheBlocks(const FSuperSLMRuntimeConfig& Config, sslm_model Model)
	{
		const uint64 BlockBytes = static_cast<uint64>(sslm_kv_block_size(Model));
		if (Config.PrefixCacheBudgetBytes <= 0 || BlockBytes == 0)
		{
			return 0;
		}
		return static_cast<int64>((static_cast<uint64>(Config.PrefixCacheBudgetBytes) + BlockBytes - 1) / BlockBytes);
	}

	static uint32 PrefixGlueRegionCPages(const FSuperSLMRuntimeConfig& Config, sslm_model Model, int64 PagePositions, int64 ContextCap)
	{
		if (Config.PrefixCacheBudgetBytes <= 0)
		{
			return 0;
		}
		return SuperSLMPaging::CacheRegionPagesFor(static_cast<uint64>(Config.PrefixCacheBudgetBytes),
			static_cast<uint64>(sslm_kv_block_size(Model)), SuperSLMPaging::PagesPerBlock(PagePositions, ContextCap));
	}

	void ConfigureRegionC(const FSuperSLMRuntimeConfig& Config, sslm_model Model, int64 PagePositions, int64 ContextCap,
		SuperSLMPaging::FPageLedgerConfig& LedgerConfig)
	{
		if (Config.PrefixCacheBudgetBytes <= 0)
		{
			return; // no cache: A and C stay 0, as in the paging release
		}
		LedgerConfig.bCacheConfigured = true;
		LedgerConfig.CacheRegionPages = PrefixGlueRegionCPages(Config, Model, PagePositions, ContextCap);
	}

	// ---- Tick ----

	void Pump(FSuperSLMSubsystemState& S)
	{
		if (S.PrefixCache.Host != nullptr)
		{
			S.PrefixCache.Host->Pump();
		}
	}

	// The ledger kind, price and paging hold kind each cache lane job posts as.
	static ESuperSLMWorkerJobKind PrefixGlueJobKind(ESuperSLMPrefixLaneKind Kind)
	{
		switch (Kind)
		{
		case ESuperSLMPrefixLaneKind::Save: return ESuperSLMWorkerJobKind::Save;
		case ESuperSLMPrefixLaneKind::Release: return ESuperSLMWorkerJobKind::PrefixRelease;
		default: return ESuperSLMWorkerJobKind::PrefixBegin; // Build, Restore
		}
	}

	// A restore is priced by its payload's bytes (§5.5, R5; U4): RestoreCostMs per restore plus
	// RestoreCostPerMiBMs per MiB. The per-MiB term is 0 unless a caller or a saved estimate sets it.
	static double PrefixGlueJobCostMs(const FSuperSLMSubsystemState& S, ESuperSLMPrefixLaneKind Kind, int64 PayloadBytes)
	{
		switch (Kind)
		{
		case ESuperSLMPrefixLaneKind::Restore:
			return SuperSLMCore::RestorePlannedMs(S.Resolved.RestoreCostMs, S.Resolved.RestoreCostPerMiBMs, PayloadBytes);
		case ESuperSLMPrefixLaneKind::Save: return S.Resolved.SaveCostMs;
		case ESuperSLMPrefixLaneKind::Release: return S.Resolved.PrefixReleaseCostMs;
		default: return S.Resolved.PrefixBeginCostMs; // a build's begin job: begin and bind (its chunks are priced apart)
		}
	}

	// CreatePrefix's chunk planner and price (SuperSLMPrefillPlanning, through this file's subsystem: the same
	// share, floor and price pairs DispatchPrefixPrefill uses), for a sliced cache build's chunk jobs (§5.3 item 4,
	// rev 1.18.8).
	class FPrefixGlueChunkPricer final : public SuperSLMCore::IBuildChunkPricer
	{
	public:
		explicit FPrefixGlueChunkPricer(const FSuperSLMSubsystemState& InS) : S(InS), Prices(PricesOf(InS.Resolved)) {}

		// Rev 1.18.11 (§15.31): the cache chunk's pairs. Chunks of 8 tokens or more are planned and priced with the
		// prefix_prefill pair when the configuration resolved one, else with the batched pair (the fallback), as
		// CreatePrefix's are; chunks under 8 keep the chunk-1 pair (Core/SuperSLMCacheChunkPricing.h).
		virtual int32_t PlanChunk(int32_t MaxChunk, int64_t StartDepth) const override
		{
			return SuperSLMPrefillPlanning::PlanPrefillChunk(MaxChunk, StartDepth, S.Config.TickBudgetMs * S.Config.BudgetHeadroom, S.Config.MinPrefillChunk,
				ToPromptPrice(Prices.Chunk1), ToPromptPrice(SuperSLMCore::CacheChunkUpperPair(Prices)));
		}

		virtual double ChunkCostMs(int32_t Chunk, int64_t StartDepth) const override
		{
			return SuperSLMPrefillPlanning::PrefillChunkCostMs(Chunk, StartDepth, ToPromptPrice(Prices.Chunk1), ToPromptPrice(SuperSLMCore::CacheChunkUpperPair(Prices)));
		}

		static SuperSLMCore::FCacheChunkPrices PricesOf(const FSuperSLMResolvedCpuConfig& R)
		{
			SuperSLMCore::FCacheChunkPrices Out;
			Out.Chunk1 = SuperSLMCore::FChunkPricePair{ R.PromptTokenCostMs, R.PromptTokenCostPerPositionMs };
			Out.Batched = SuperSLMCore::FChunkPricePair{ R.BatchedPromptTokenCostMs, R.BatchedPromptTokenCostPerPositionMs };
			Out.bHasPrefixPrefill = R.PrefixPrefillPriceSource != ESuperSLMPrefixPrefillPriceSource::BatchedFallback;
			Out.PrefixPrefill = SuperSLMCore::FChunkPricePair{ R.PrefixPrefillTokenCostMs, R.PrefixPrefillTokenCostPerPositionMs };
			return Out;
		}

		static SuperSLMPrefillPlanning::FPromptPrice ToPromptPrice(const SuperSLMCore::FChunkPricePair& Pair)
		{
			return SuperSLMPrefillPlanning::FPromptPrice{ Pair.TokenMs, Pair.PerPositionMs };
		}

	private:
		const FSuperSLMSubsystemState& S;
		SuperSLMCore::FCacheChunkPrices Prices;
	};

	// Rev 1.18.10 (§15.29, code review S1): the lanes held for a waiting chunk. A lane a chunk waits for records the
	// waiting build's previous job there (the smallest, when several builds wait on one lane); once the lane has
	// taken one job since, FLane::IsFreeForPlanning refuses every other job until the chunk is posted, and
	// PlanContinuations queues no continuation there. A chunk at the test seams' chunk hold does not hold its lane.
	static void PrefixGlueRefreshWaitingChunks(FSuperSLMSubsystemState& S)
	{
		for (auto& Lane : S.Lanes)
		{
			Lane.ChunkWaitingAfterJobId = 0;
		}
		FState& G = S.PrefixCache;
		if (G.Host == nullptr)
		{
			return;
		}
		for (int32 Index = 0; Index < G.Host->NumChunkJobs(); ++Index)
		{
			const FSuperSLMCpuBuildRun& Run = *G.Host->ChunkJobAt(Index).BuildRun;
#if WITH_DEV_AUTOMATION_TESTS
			if (G.Host->IsChunkHeld(Run))
			{
				continue;
			}
#endif
			if (Run.PinnedLane == INDEX_NONE || !S.Lanes.IsValidIndex(Run.PinnedLane) || Run.LastLaneJobId <= 0)
			{
				continue;
			}
			int64& Waiting = S.Lanes[Run.PinnedLane].ChunkWaitingAfterJobId;
			Waiting = Waiting == 0 ? Run.LastLaneJobId : FMath::Min(Waiting, Run.LastLaneJobId);
		}
	}

	void NoteWaitingChunks(FSuperSLMSubsystemState& S)
	{
		PrefixGlueRefreshWaitingChunks(S);
	}

	// A sliced CPU cache build's chunk jobs (§5.3 item 4, rev 1.18.8): each open build's next chunk, posted on the
	// lane its begin job ran on once that lane is free for it. Generations planned first this tick (PlanNextJobs)
	// take the lane first: other jobs run between two chunk jobs, but at most one between any two (rev 1.18.10,
	// §15.29: the lane is then held for the chunk, PrefixGlueRefreshWaitingChunks). Each chunk is planned and priced as CreatePrefix's
	// (DispatchPrefixPrefill): PlanPrefillChunk over min(MaxPrefillChunkBudget, tokens left) at the chunk's start
	// depth, and PrefillChunkCostMs at that depth. The job whose chunk reaches the build's length freezes, and carries
	// the build's HoldApply(PrefixBuild), as CreatePrefix's freezing prefill does.
	static void PrefixGlueDispatchChunkJobs(FSuperSLMSubsystemState& S, int32 TickIndex)
	{
		FState& G = S.PrefixCache;
		const FPrefixGlueChunkPricer Pricer(S);
		for (int32 Index = 0; Index < G.Host->NumChunkJobs();)
		{
			const FSuperSLMPrefixLaneJob& Waiting = G.Host->ChunkJobAt(Index);
			FSuperSLMCpuBuildRun& Run = *Waiting.BuildRun;
#if WITH_DEV_AUTOMATION_TESTS
			if (G.Host->IsChunkHeld(Run)) // the chunk hold (cell 1.7's between-chunks leg)
			{
				++Index;
				continue;
			}
#endif
			if (Run.PinnedLane == INDEX_NONE || !S.Lanes.IsValidIndex(Run.PinnedLane) || !S.Lanes[Run.PinnedLane].IsFreeForChunk(TickIndex))
			{
				++Index;
				continue;
			}
			Run.Next = Run.Slices.PlanNext(Pricer, FMath::Max(1, S.Config.MaxPrefillChunkBudget));
			TSharedRef<FSuperSLMPrefixLaneJob, ESPMode::ThreadSafe> Job = MakeShared<FSuperSLMPrefixLaneJob, ESPMode::ThreadSafe>(G.Host->TakeChunkJob(Index));
			FSuperSLMSubsystemState* StatePtr = &S;
			const FSuperSLMPrefixLaneContext Base = PrefixGlueLaneContext(S, INDEX_NONE);
			auto BuildExecute = [Job, Base, StatePtr](int32 LaneIndex) -> TFunction<void()>
			{
				FSuperSLMPrefixLaneContext Context = Base;
				Context.Workspace = StatePtr->Lanes[LaneIndex].Workspace;
				return [Job, Context]()
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixCache.BuildChunk", SuperSLMChannel);
					Job->Run(Context, Job->Completion.Get());
				};
			};
			auto Deliver = [StatePtr, Job](double)
			{
				if (StatePtr->PrefixCache.Host != nullptr)
				{
					StatePtr->PrefixCache.Host->DeliverLaneJob(*Job);
				}
			};
			if (Run.Next.bLast)
			{
				S.NextPostApplyHoldId = Run.ApplyHoldId;
				Run.ApplyHoldId = 0;
			}
			TArray<int32> CandidateLanes;
			CandidateLanes.Add(Run.PinnedLane);
			PostLedgerJob(S, Run.PinnedLane, CandidateLanes, ESuperSLMWorkerJobKind::DecodeOrPrefill, 0, Run.Next.Tokens, 0, Run.Next.PlannedMs,
				FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
			S.JobLedger.Last()->PromptPositionSum = PromptChunkPositionSum(Run.Next.Tokens, Run.Next.StartDepth);
			Run.LastLaneJobId = S.Lanes[Run.PinnedLane].LastPostedJobId;
		}
		PrefixGlueRefreshWaitingChunks(S); // a posted chunk no longer holds its lane
	}

	static bool PrefixGlueHoldKind(ESuperSLMPrefixLaneKind Kind, EPagedJobKind& Out)
	{
		switch (Kind)
		{
		case ESuperSLMPrefixLaneKind::Build: Out = EPagedJobKind::PrefixBuild; return true;
		case ESuperSLMPrefixLaneKind::Restore: Out = EPagedJobKind::Restore; return true;
		case ESuperSLMPrefixLaneKind::Release: Out = EPagedJobKind::PrefixRelease; return true;
		default: return false;
		}
	}

	void DispatchLaneJobs(FSuperSLMSubsystemState& S, int32 TickIndex)
	{
		FState& G = S.PrefixCache;
		if (G.Host == nullptr)
		{
			return;
		}
		PrefixGlueDispatchChunkJobs(S, TickIndex); // §5.3 item 4, rev 1.18.8: a build's chunks on its own lane
		for (;;)
		{
			if (!G.Parked.IsSet())
			{
				if (!G.Host->HasLaneJob())
				{
					return;
				}
				G.Parked.Emplace(G.Host->TakeLaneJob());
				EPagedJobKind HoldKind;
				const bool bHolds = PrefixGlueHoldKind(G.Parked->Kind, HoldKind);
				G.ParkedStartHoldId = bHolds ? S.TakeArmedHold(HoldKind, /*bApply*/ false) : 0;
				G.ParkedApplyHoldId = bHolds ? S.TakeArmedHold(HoldKind, /*bApply*/ true) : 0;
#if WITH_DEV_AUTOMATION_TESTS
				// Paging's one-shot Layer-1 override reaches the cache's own Layer-1 jobs too: a cache build is a
				// PrefixBuild, a cache restore a Restore (box 2026-10-21: before this, an override set for a cache
				// build stayed armed and no cache request could end PoolExhausted; Cpu.Coalescing, Cpu.AllPinned,
				// Disk.EveryFallback). A release takes none: §5.4's release statuses have their own seam (5.15).
				if (bHolds && G.Parked->Kind != ESuperSLMPrefixLaneKind::Release)
				{
					G.Parked->ForcedLayer1Status = static_cast<int32>(S.TakeLayer1Override(HoldKind));
				}
#endif
			}
			// Paging plan §3.9: a job parked at its HoldStart() hold is not posted.
			if (S.IsHoldBlocking(G.ParkedStartHoldId))
			{
				S.MarkHoldReached(G.ParkedStartHoldId);
				return;
			}
			TArray<int32> CandidateLanes;
			for (int32 L = 0; L < S.Lanes.Num(); ++L)
			{
				if (S.Lanes[L].IsFreeForPlanning(TickIndex))
				{
					CandidateLanes.Add(L);
				}
			}
			if (CandidateLanes.IsEmpty())
			{
				return;
			}

			TSharedRef<FSuperSLMPrefixLaneJob, ESPMode::ThreadSafe> Job = MakeShared<FSuperSLMPrefixLaneJob, ESPMode::ThreadSafe>(MoveTemp(G.Parked.GetValue()));
			G.Parked.Reset();
			const ESuperSLMPrefixLaneKind Kind = Job->Kind;
			const FSuperSLMPrefixLaneContext Base = PrefixGlueLaneContext(S, INDEX_NONE);
			FSuperSLMSubsystemState* StatePtr = &S;

			auto BuildExecute = [Job, Base, StatePtr](int32 LaneIndex) -> TFunction<void()>
			{
				FSuperSLMPrefixLaneContext Context = Base;
				Context.Workspace = StatePtr->Lanes[LaneIndex].Workspace;
#if WITH_DEV_AUTOMATION_TESTS
				Context.ForcedLayer1Status = Job->ForcedLayer1Status;
				// The build hold (§8): a Build posted while it is armed counts on its lane from this post until it
				// passes the hold, so nothing is planned behind it and the barrier reads the lane as parked.
				FSuperSLMCpuWorkerRunnable* Runnable = StatePtr->Lanes[LaneIndex].Runnable;
				FSuperSLMPrefixCacheHost* Host = StatePtr->PrefixCache.Host;
				if (Runnable != nullptr && Host != nullptr && Job->Kind == ESuperSLMPrefixLaneKind::Build && !Job->bBuildChunk && Host->IsBuildHoldArmed())
				{
					Context.bBuildHoldCounted = true;
					Context.LaneBuildHolds = &Runnable->TestBuildHolds;
					Context.LaneStopRequested = &Runnable->bStopRequested;
					Runnable->TestBuildHolds.fetch_add(1);
				}
#endif
				return [Job, Context]()
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PrefixCache", SuperSLMChannel);
					Job->Run(Context, Job->Completion.Get());
				};
			};
			auto Deliver = [StatePtr, Job](double)
			{
				if (StatePtr->PrefixCache.Host != nullptr)
				{
					StatePtr->PrefixCache.Host->DeliverLaneJob(*Job);
				}
			};

			// PostLedgerJob clamps and logs a schedule that does not fit, as it does for every kind. A sliced build's
			// HoldApply(PrefixBuild) goes with the job that freezes it (PrefixGlueDispatchChunkJobs), as CreatePrefix's
			// does; its begin job carries it only when the build has no token to prefill (the begin job freezes).
			const bool bSliced = Job->BuildRun.IsValid() && Job->BuildRun->Tokens.Num() > 0;
			S.NextPostApplyHoldId = bSliced ? 0 : G.ParkedApplyHoldId;
			if (bSliced)
			{
				Job->BuildRun->ApplyHoldId = G.ParkedApplyHoldId;
			}
			G.ParkedStartHoldId = 0;
			G.ParkedApplyHoldId = 0;
			int32 PinnedLane = INDEX_NONE;
			PostLedgerJob(S, PinnedLane, CandidateLanes, PrefixGlueJobKind(Kind), 0, 0, 0, PrefixGlueJobCostMs(S, Kind, Job->PayloadBytes),
				FSuperSLMSequence{}, FSuperSLMLifecycleOpHandle{}, TickIndex, BuildExecute, MoveTemp(Deliver));
			if (Job->BuildRun.IsValid())
			{
				Job->BuildRun->PinnedLane = PinnedLane; // the build's chunk jobs run on its begin job's lane, in order
				Job->BuildRun->LastLaneJobId = S.Lanes[PinnedLane].LastPostedJobId; // the begin job (§15.29)
			}
		}
	}

	// ---- Teardown (§5.11 (a) L1) ----

	// Code review r2 B1: the lane delivery below the lane stop applies a running build's or restore's result to
	// its lease (Ready) before BeforeHandleRelease abandons the core, so the lease was not carried as Cancelled
	// across the Configure(). Marked here, first, the result is discarded as §5.6 says.
	void TeardownBegins(FSuperSLMSubsystemState& S)
	{
		if (S.PrefixCache.Host != nullptr)
		{
			S.PrefixCache.Host->MarkTeardownBegins();
		}
	}

	void BeforeHandleRelease(FSuperSLMSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		// A shadow entry stands in for a cache lease; the engine handle is the cache's, so the
		// subsystem's release loop must not see it.
		for (const TPair<int64, FState::FAdoptShadow>& Pair : G.Shadows)
		{
			S.Prefixes.Remove(Pair.Key);
		}
		G.Shadows.Empty();
		G.LeaseLength.Empty();
		if (G.Host == nullptr)
		{
			return;
		}
		if (G.Parked.IsSet())
		{
			G.Host->PutBackLaneJob(MoveTemp(G.Parked.GetValue()));
			G.Parked.Reset();
		}
		G.Host->ReleaseForPoolDestroy(PrefixGlueLaneContext(S, 0));
#if WITH_DEV_AUTOMATION_TESTS
		// The teardown probe (cells 1.7, 2.10): the pool's free pages after every lane has stopped and the cache
		// has released every handle it holds, and before the subsystem releases its own sequences and prefixes.
		// That is the state the cells' initial count is read in (InitialFree: at quiescence, nothing leased, the
		// warm sequences holding their reservations), so equal readings mean the cache leaked nothing. Box
		// 2026-10-21: read in BeforePoolDestroy, after the subsystem had freed the warm sequences, it read the
		// initial count plus their reservations (292 = 256 + 4 x R(512) and 274 = 256 + 2 x R(512)).
		if (G.Host->Test.bTeardownProbeArmed && S.Pool != nullptr)
		{
			sslm_kv_pool_stats_out Raw = {};
			Raw.struct_size = static_cast<uint32_t>(sizeof(sslm_kv_pool_stats_out));
			G.Host->Test.TeardownFreePages = sslm_kv_pool_stats(S.Pool, &Raw) == SSLM_OK ? static_cast<int64>(Raw.free_pages) : -1;
			G.Host->Test.bTeardownProbeArmed = false;
		}
#endif
	}

	void BeforePoolDestroy(FSuperSLMSubsystemState& S)
	{
		// The teardown probe moved to BeforeHandleRelease (box 2026-10-21); kept as the pool-destroy hook.
		(void)S;
	}

	void AfterPoolDestroy(FSuperSLMSubsystemState& S, sslm_status Status)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (S.PrefixCache.Host != nullptr)
		{
			S.PrefixCache.Host->Test.LastPoolDestroyStatus = static_cast<int32>(Status);
		}
#else
		(void)Status;
#endif
		S.PrefixCache.Host = nullptr;
	}

	void NoteLegacyBegin(FSuperSLMSubsystemState& S, sslm_prefix Handle)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (S.PrefixCache.Host != nullptr && Handle != nullptr)
		{
			S.PrefixCache.Host->Test.LegacyLive.Add(PrefixGlueHandleBits(Handle));
			S.PrefixCache.Host->MarkHandleMinted(PrefixGlueHandleBits(Handle));
		}
#else
		(void)S;
		(void)Handle;
#endif
	}

	void NoteRelease(FSuperSLMSubsystemState& S, sslm_prefix Handle)
	{
		if (S.PrefixCache.Host != nullptr && Handle != nullptr)
		{
			S.PrefixCache.Host->NoteRelease(Handle);
		}
	}

	// ---- Adopt (§5.6, §5.11 (b)) ----

	bool StopCacheAdopt(FSuperSLMSubsystemState& S, int64 PrefixId, ESuperSLMRestoreResult& OutResult, FString& OutError)
	{
		FState& G = S.PrefixCache;
		if (G.Host == nullptr)
		{
			return false;
		}
		FSuperSLMPrefix Lease;
		Lease.Id = PrefixId;
		sslm_prefix Handle = nullptr;
		uint64 Token = 0;
		uint64 EntryId = 0;
		switch (G.Host->BeginAdopt(Lease, Handle, Token, EntryId, OutError))
		{
		case FSuperSLMPrefixCacheHost::EAdoptLookup::NotCacheHandle:
			return false; // the subsystem's own handle, or a stale one: the existing checks decide
		case FSuperSLMPrefixCacheHost::EAdoptLookup::NotReady:
			OutResult = ESuperSLMRestoreResult::Malformed;
			return true;
		case FSuperSLMPrefixCacheHost::EAdoptLookup::LeaseEnded:
			OutResult = ESuperSLMRestoreResult::PrefixLeaseEnded;
			return true;
		default:
			break;
		}
		FState::FAdoptShadow& Shadow = G.Shadows.FindOrAdd(PrefixId);
		Shadow.Tokens.Add(Token);
		Shadow.EntryKey = EntryId;
		if (!S.Prefixes.Contains(PrefixId))
		{
			FPrefixEntry& Entry = S.Prefixes.Add(PrefixId);
			Entry.Handle = Handle;
			Entry.Consumed = G.LeaseLength.FindRef(PrefixId);
			Entry.Phase = ESuperSLMPrefixPhase::Ready;
			Entry.bLedgerBuilt = S.IsBudgetMode(); // admitted through AdmitCacheAdopt, never a ledger node
		}
		return false;
	}

	bool AdmitCacheAdopt(FSuperSLMSubsystemState& S, int64 PrefixId, int32 SlotIndex, SuperSLMPaging::FAdmission& OutAdmission)
	{
		const FState::FAdoptShadow* Shadow = S.PrefixCache.Shadows.Find(PrefixId);
		if (Shadow == nullptr)
		{
			return false;
		}
		OutAdmission = S.Ledger.AdmitAdoptCacheEntry(SlotIndex, Shadow->EntryKey, S.PrefixCache.LeaseLength.FindRef(PrefixId));
		return true;
	}

	void OnAdoptDelivered(FSuperSLMSubsystemState& S, int64 PrefixId)
	{
		FState& G = S.PrefixCache;
		FState::FAdoptShadow* Shadow = G.Shadows.Find(PrefixId);
		if (Shadow == nullptr)
		{
			return;
		}
		if (Shadow->Tokens.Num() > 0)
		{
			const uint64 Token = Shadow->Tokens[0];
			Shadow->Tokens.RemoveAt(0);
			if (G.Host != nullptr)
			{
				G.Host->EndAdopt(Token);
			}
		}
		if (Shadow->Tokens.Num() == 0)
		{
			S.Prefixes.Remove(PrefixId);
			G.Shadows.Remove(PrefixId);
		}
	}

	int32 ShadowCount(const FSuperSLMSubsystemState& S)
	{
		return S.PrefixCache.Shadows.Num();
	}

	bool IsShadow(const FSuperSLMSubsystemState& S, int64 PrefixId)
	{
		return S.PrefixCache.Shadows.Contains(PrefixId);
	}

	// ---- A leased cache handle as a restore hint (§5.11 (b) *Adopt and hint*, rev 1.18, PC-15 Q5; cell 8.15) ----

	bool PinCacheRestoreHint(FSuperSLMSubsystemState& S, int64 PrefixId, FCacheRestoreHint& Out)
	{
		Out = FCacheRestoreHint();
		FState& G = S.PrefixCache;
		if (G.Host == nullptr)
		{
			return false;
		}
		FSuperSLMPrefix Lease;
		Lease.Id = PrefixId;
		sslm_prefix Handle = nullptr;
		uint64 Token = 0;
		uint64 EntryId = 0;
		FString Ignored;
		// A lease that is not Ready, or ended, is no hint: the restore is private. Checked first, so a hint is never
		// counted as an adopt of an ended lease (BeginAdopt raises AdoptOfEndedLease for one).
		if (G.Host->GetLeaseState(Lease) != ESuperSLMPrefixLeaseState::Ready)
		{
			return false;
		}
		// The adopt pin: the entry stays cached, its engine handle live, until the restore delivers or is refused,
		// whatever the lease does meanwhile.
		if (G.Host->BeginAdopt(Lease, Handle, Token, EntryId, Ignored) != FSuperSLMPrefixCacheHost::EAdoptLookup::Adoptable)
		{
			return false;
		}
		Out.Token = Token;
		Out.Handle = Handle;
		Out.EntryKey = EntryId;
		Out.Length = G.LeaseLength.FindRef(PrefixId);
		return true;
	}

	void UnpinCacheRestoreHint(FSuperSLMSubsystemState& S, uint64 Token)
	{
		if (Token != 0 && S.PrefixCache.Host != nullptr)
		{
			S.PrefixCache.Host->EndAdopt(Token);
		}
	}

	void NoteSequenceRestoreApplied(FSuperSLMSubsystemState& S, uint32 SharedPages)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (S.PrefixCache.Host != nullptr)
		{
			S.PrefixCache.Host->Test.LastRestoreSharedPages = static_cast<int64>(SharedPages);
		}
#else
		(void)S;
		(void)SharedPages;
#endif
	}

#if WITH_DEV_AUTOMATION_TESTS
	int32 HeldBuildsForBarrier(const FSuperSLMSubsystemState& S)
	{
		return S.PrefixCache.Host != nullptr ? S.PrefixCache.Host->Test.HeldBuilds.load() : 0;
	}
#endif

	bool RefuseRelease(FSuperSLMSubsystemState& S, const FSuperSLMPrefix& Prefix, FString& OutError)
	{
		return S.PrefixCache.Host != nullptr && S.PrefixCache.Host->RefuseRelease(Prefix, OutError);
	}

	bool AcceptSaveLayer1(const FString& WrapperTag, const FString& WrapperCommit, const uint8* Payload, int64 PayloadBytes)
	{
		const SuperSLMCore::ESaveLayer1Check Check = SuperSLMCore::CheckSaveLayer1(
			std::string(TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Tag())), std::string(TCHAR_TO_UTF8(*WrapperTag)),
			std::string(TCHAR_TO_UTF8(*WrapperCommit)), Payload, PayloadBytes > 0 ? static_cast<size_t>(PayloadBytes) : 0);
		return Check != SuperSLMCore::ESaveLayer1Check::Layer1Mismatch;
	}
}

// ---- The subsystem's prefix-cache API (§5.6) ----

void USuperSLMSubsystem::InstallPrefixCache()
{
	if (State == nullptr)
	{
		return;
	}
	State->PrefixCache.Host = nullptr;
	if (State->Config.PrefixCacheBudgetBytes <= 0)
	{
		return; // no cache this configuration; a host kept from an earlier one stays abandoned
	}
	if (PrefixCacheHost == nullptr)
	{
		PrefixCacheHost = new FSuperSLMPrefixCacheHost(&GNextPrefixId);
	}

	SuperSLMCore::FPrefixCacheConfig CacheConfig;
	FMemory::Memcpy(CacheConfig.Identity.ArtifactHash.data(), State->ArtifactHash, 32);
	CacheConfig.Identity.Layer1Tag = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Tag());
	CacheConfig.Identity.Layer1Commit = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Commit());
	CacheConfig.Identity.Backend = 0; // CPU
	CacheConfig.BudgetBytes = static_cast<uint64>(State->Config.PrefixCacheBudgetBytes);
	CacheConfig.PageBytes = static_cast<uint64>(sslm_kv_page_size(State->Model));
	CacheConfig.PageTokens = static_cast<uint32>(sslm_kv_page_positions(State->Model));
	CacheConfig.ContextCap = State->Shape.ContextCap;
	CacheConfig.VocabSize = State->Shape.VocabSize;
	CacheConfig.bDiskEnabled = State->Config.bPrefixDiskCacheEnabled;
	const FString Directory = State->Config.PrefixDiskCacheDirectory.IsEmpty()
		? FPaths::ProjectSavedDir() / TEXT("SuperSLM/PrefixCache")
		: State->Config.PrefixDiskCacheDirectory;
	CacheConfig.DiskDirectory = TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Directory));
	CacheConfig.DiskMaxBytes = static_cast<uint64>(FMath::Max<int64>(0, State->Config.PrefixDiskCacheMaxBytes));
	// Unique among live subsystems in this process, and across processes sharing the directory.
	CacheConfig.InstanceId = (static_cast<uint64>(FPlatformProcess::GetCurrentProcessId()) << 40)
		^ static_cast<uint64>(reinterpret_cast<UPTRINT>(PrefixCacheHost));
	CacheConfig.RegionC = State->IsBudgetMode() ? &State->Ledger : nullptr;
	// Nested entries (U6) need budget mode: begin_from and a restore's re-share are page-table verbs.
	CacheConfig.bNestedEntries = State->IsBudgetMode();
	State->PrefixCache.RegionCPages = State->IsBudgetMode()
		? SuperSLMPrefixCacheGlue::PrefixGlueRegionCPages(State->Config, State->Model, State->PagePositions, State->Shape.ContextCap)
		: 0u;

	FString Error;
	if (!PrefixCacheHost->Configure(CacheConfig, Error))
	{
		UE_LOG(LogSuperSLM, Error, TEXT("PrefixCache: Configure() refused (%s); this configuration runs without a cache."), *Error);
		return;
	}
	State->PrefixCache.Host = PrefixCacheHost;
}

bool USuperSLMSubsystem::RequestPrefix(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& OutPrefix, FString& OutError)
{
	OutPrefix = FSuperSLMPrefix();
	FSuperSLMPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host : nullptr;
	if (Host == nullptr)
	{
		OutError = TEXT("the prefix cache is not configured (the subsystem is not configured, or PrefixCacheBudgetBytes is 0)");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage;
		return false;
	}
	if (!Host->Request(Spec, OutPrefix, OutError))
	{
		return false;
	}
	// The lease's whole length: a nested spec's Tokens are only the suffix after its parent lease, and the engine's
	// nested prefix holds parent plus suffix. The adopt (context, origin) and the ledger's pre-charge read this, so it
	// is the parent lease's own whole length plus the suffix (the parent is a live Ready lease: Request checked it).
	const int32 ParentLength = Spec.Parent.IsValid() ? State->PrefixCache.LeaseLength.FindRef(Spec.Parent.Id) : 0;
	State->PrefixCache.LeaseLength.Add(OutPrefix.Id, ParentLength + Spec.Tokens.Num());
	return true;
}

bool USuperSLMSubsystem::ReturnPrefix(const FSuperSLMPrefix& Prefix)
{
	FSuperSLMPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host : nullptr;
	if (Host == nullptr || !Host->Return(Prefix))
	{
		return false;
	}
	if (!State->PrefixCache.Shadows.Contains(Prefix.Id))
	{
		State->PrefixCache.LeaseLength.Remove(Prefix.Id);
	}
	return true;
}

// The queries read the host itself, not this configuration's pointer to it: a lease from an ended
// configuration reads Cancelled (§5.11 (a)), not Invalid.
ESuperSLMPrefixSource USuperSLMSubsystem::GetPrefixSource(const FSuperSLMPrefix& Prefix) const
{
	return PrefixCacheHost != nullptr ? PrefixCacheHost->GetSource(Prefix) : ESuperSLMPrefixSource::Invalid;
}

ESuperSLMPrefixLeaseState USuperSLMSubsystem::GetPrefixLeaseState(const FSuperSLMPrefix& Prefix) const
{
	return PrefixCacheHost != nullptr ? PrefixCacheHost->GetLeaseState(Prefix) : ESuperSLMPrefixLeaseState::Invalid;
}

FSuperSLMPrefixCacheStats USuperSLMSubsystem::GetPrefixCacheStats() const
{
	return PrefixCacheHost != nullptr ? PrefixCacheHost->GetStats() : FSuperSLMPrefixCacheStats();
}

// Explicit residency control (§5.6, rev 1.18, PC-16 (a)). The verbs need a configured cache, as RequestPrefix
// does; the op query reads the host itself, whose op table outlives each configuration (a resolved op keeps its
// result across a re-Configure()).
bool USuperSLMSubsystem::WarmPrefix(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	FSuperSLMPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host : nullptr;
	if (Host == nullptr)
	{
		OutError = TEXT("the prefix cache is not configured (the subsystem is not configured, or PrefixCacheBudgetBytes is 0)");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage;
		return false;
	}
	return Host->Warm(Spec, OutOp, OutError);
}

bool USuperSLMSubsystem::DropPrefix(const FSuperSLMPrefixSpec& Spec, ESuperSLMPrefixDropScope Scope, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	FSuperSLMPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host : nullptr;
	if (Host == nullptr)
	{
		OutError = TEXT("the prefix cache is not configured (the subsystem is not configured, or PrefixCacheBudgetBytes is 0)");
		return false;
	}
	if (State->bTickHorizonReached)
	{
		OutError = State->TickHorizonMessage;
		return false;
	}
	return Host->Drop(Spec, Scope, OutOp, OutError);
}

bool USuperSLMSubsystem::GetPrefixOpResult(const FSuperSLMPrefixOp& Op, FSuperSLMPrefixOpResult& Out) const
{
	Out = FSuperSLMPrefixOpResult();
	return PrefixCacheHost != nullptr && PrefixCacheHost->GetOpResult(Op, Out);
}

void USuperSLMSubsystem::RegisterPrefixSet(USuperSLMPrefixSet* Set)
{
	if (Set == nullptr)
	{
		return;
	}
	// The host outlives each configuration (it is created here when no cache has been configured
	// yet), so a set registered before Configure() serves the first configuration with a cache.
	if (PrefixCacheHost == nullptr)
	{
		PrefixCacheHost = new FSuperSLMPrefixCacheHost(&GNextPrefixId);
	}
	const bool bChecked = PrefixCacheHost->IsCoreConfigured();
	const int32 Registered = PrefixCacheHost->RegisterCookedSet(*Set);
	if (bChecked)
	{
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache: registered %d cooked entr%s from %s"), Registered, Registered == 1 ? TEXT("y") : TEXT("ies"), *Set->GetPathName());
	}
	else
	{
		// No model yet to check the set against (§5.8, rev 1.18.5): the next Configure() checks it, and a mismatch
		// is logged then, once, by the set's name. This line leaves the name out so that one line is the only one.
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache: registered %d cooked entr%s before Configure(); the set's model is checked then"), Registered, Registered == 1 ? TEXT("y") : TEXT("ies"));
	}
}

void USuperSLMSubsystem::UnregisterPrefixSet(USuperSLMPrefixSet* Set)
{
	if (Set != nullptr && PrefixCacheHost != nullptr)
	{
		PrefixCacheHost->UnregisterCookedSet(*Set);
	}
}

// ---- The test seam (SuperSLMPrefixCacheTestSeam.h) ----
// USuperSLMSubsystem::PrefixCacheHost is private: only the seam's own members (a friend) read it.

#if WITH_DEV_AUTOMATION_TESTS

namespace SuperSLMPrefixCacheGlue
{
	// The wrapper's variable fields, walked to the payload (SuperSLMPrefixCacheWrapper.h layout).
	static bool PrefixGlueWrapperPayload(const TArray<uint8>& Bytes, int64& OutOffset, int64& OutBytes)
	{
		int64 At = static_cast<int64>(SuperSLMCore::kPrefixWrapperFixedBytes);
		auto ReadU32 = [&Bytes, &At](uint32& Out) -> bool
		{
			if (At + 4 > Bytes.Num()) { return false; }
			FMemory::Memcpy(&Out, Bytes.GetData() + At, 4);
			At += 4;
			return true;
		};
		uint32 SchemaBytes = 0;
		uint32 TokenCount = 0;
		if (!ReadU32(SchemaBytes)) { return false; }
		At += SchemaBytes;
		if (!ReadU32(TokenCount)) { return false; }
		At += static_cast<int64>(TokenCount) * 4;
		if (At + 1 > Bytes.Num()) { return false; }
		At += Bytes[At] == 1 ? 33 : 1;
		if (At + 8 > Bytes.Num()) { return false; }
		uint64 PayloadBytes = 0;
		FMemory::Memcpy(&PayloadBytes, Bytes.GetData() + At, 8);
		At += 8;
		if (At + static_cast<int64>(PayloadBytes) != Bytes.Num()) { return false; }
		OutOffset = At;
		OutBytes = static_cast<int64>(PayloadBytes);
		return true;
	}
}

void FSuperSLMPrefixCacheTestSeam::HoldReleases(USuperSLMSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->SetHoldReleases(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldReleaseCount(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.HeldReleases.Num() : 0;
}

bool FSuperSLMPrefixCacheTestSeam::DispatchHeldRelease(USuperSLMSubsystem& Subsystem, int32 Index)
{
	FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	if (Host == nullptr || !Host->Test.HeldReleases.IsValidIndex(Index))
	{
		return false;
	}
	Host->DispatchHeldRelease(Index);
	return true;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandle(const USuperSLMSubsystem& Subsystem)
{
	int32 Max = 0;
	if (const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		for (const TPair<uint64, int32>& Pair : Host->Test.ReleasesByHandle) { Max = FMath::Max(Max, Pair.Value); }
	}
	return Max;
}

int32 FSuperSLMPrefixCacheTestSeam::ReleasesOf(const USuperSLMSubsystem& Subsystem, uint64 EngineHandle)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.ReleasesByHandle.FindRef(EngineHandle) : 0;
}

uint64 FSuperSLMPrefixCacheTestSeam::EngineHandleOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->EngineHandleOf(Lease) : 0;
}

void FSuperSLMPrefixCacheTestSeam::SetNullRestoreHints(USuperSLMSubsystem& Subsystem, bool bNull)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.bNullRestoreHints = bNull; }
}

int64 FSuperSLMPrefixCacheTestSeam::SharedPagesOf(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	if (Host == nullptr)
	{
		return -1;
	}
	const uint64 Engine = Host->EngineHandleOf(Lease);
	const int64* Shared = Engine != 0 ? Host->Test.SharedPagesByHandle.Find(Engine) : nullptr;
	return Shared != nullptr ? *Shared : -1;
}

void FSuperSLMPrefixCacheTestSeam::ForceKeyCollision(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, const FSuperSLMPrefixSpec& CollideWith)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		FSuperSLMPrefixCacheHost::FTestState::FCollision& C = Host->Test.Collisions.AddDefaulted_GetRef();
		C.FromTokens = Spec.Tokens;
		C.FromSchema = Spec.SchemaName.IsNone() ? FString() : Spec.SchemaName.ToString();
		C.ToTokens = CollideWith.Tokens;
		C.ToSchema = CollideWith.SchemaName.IsNone() ? FString() : CollideWith.SchemaName.ToString();
	}
}

void FSuperSLMPrefixCacheTestSeam::SetDiskFault(USuperSLMSubsystem& Subsystem, ESuperSLMPrefixDiskFault Fault)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.DiskFault.store(static_cast<uint8>(Fault)); }
}

bool FSuperSLMPrefixCacheTestSeam::EditDiskWrapper(const FString& Path, ESuperSLMDiskWrapperEdit Edit)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < static_cast<int32>(SuperSLMCore::kPrefixWrapperFixedBytes))
	{
		return false;
	}
	switch (Edit)
	{
	case ESuperSLMDiskWrapperEdit::Truncate:
		Bytes.SetNum(Bytes.Num() / 2);
		break;
	case ESuperSLMDiskWrapperEdit::WrapperVersion0:
	case ESuperSLMDiskWrapperEdit::WrapperVersion2:
	{
		const uint32 Version = Edit == ESuperSLMDiskWrapperEdit::WrapperVersion0 ? 0u : 2u;
		FMemory::Memcpy(Bytes.GetData() + 4, &Version, 4);
		break;
	}
	case ESuperSLMDiskWrapperEdit::OtherLayer1Tag:
	{
		static const char Other[SuperSLMCore::kPrefixWrapperTagBytes] = "0.0.0-other";
		FMemory::Memcpy(Bytes.GetData() + 12, Other, SuperSLMCore::kPrefixWrapperTagBytes);
		break;
	}
	case ESuperSLMDiskWrapperEdit::PayloadByte:
	{
		int64 Offset = 0;
		int64 Size = 0;
		if (!SuperSLMPrefixCacheGlue::PrefixGlueWrapperPayload(Bytes, Offset, Size) || Size < 2)
		{
			return false;
		}
		Bytes[Offset + Size / 2] ^= 0x5A;
		break;
	}
	case ESuperSLMDiskWrapperEdit::PayloadProducer:
	{
		// Another producer: the SPX1 header's engine patch field (offset 16) moved by one and the digest (offset 28,
		// SHA-256 of the blob with its own field zeroed) recomputed, so the restore passes the digest and backend
		// checks and is refused at the producer check, SSLM_RESTORE_PRODUCER_MISMATCH (Layer 1's src/spx1_format.h).
		// Box 2026-10-21 second run: this case returned false (it was left to be written on the box), so
		// Disk.EveryFallback's producer leg failed silently, with no assertion message.
		constexpr int64 kSpx1Header = 184, kPatchAt = 16, kDigestAt = 28;
		int64 Offset = 0;
		int64 Size = 0;
		if (!SuperSLMPrefixCacheGlue::PrefixGlueWrapperPayload(Bytes, Offset, Size) || Size < kSpx1Header
			|| FMemory::Memcmp(Bytes.GetData() + Offset, "SPX1", 4) != 0)
		{
			return false;
		}
		uint8* Spx1 = Bytes.GetData() + Offset;
		uint32 Patch = 0;
		FMemory::Memcpy(&Patch, Spx1 + kPatchAt, 4);
		Patch += 1;
		FMemory::Memcpy(Spx1 + kPatchAt, &Patch, 4);
		FMemory::Memzero(Spx1 + kDigestAt, 32);
		uint8 Digest[32] = {};
		superslm::Sha256Hash(Spx1, static_cast<size_t>(Size), Digest);
		FMemory::Memcpy(Spx1 + kDigestAt, Digest, 32);
		break;
	}
	default:
		return false;
	}
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

void FSuperSLMPrefixCacheTestSeam::SetBeforeDiskRead(USuperSLMSubsystem& Subsystem, TFunction<void(const FString& Path)> Hook)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		FScopeLock Lock(&Host->Test.HookGuard);
		Host->Test.BeforeDiskRead = MoveTemp(Hook);
	}
}

int32 FSuperSLMPrefixCacheTestSeam::DiskIoInFlight(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->DiskIoInFlight() : 0;
}

bool FSuperSLMPrefixCacheTestSeam::IsCacheIoSettled(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host == nullptr || Host->IsIoSettled();
}

int64 FSuperSLMPrefixCacheTestSeam::GameThreadStateAccesses(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.GameThreadStateAccesses.load() : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadChecks(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.OffGameThreadChecks.load() : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::CacheRegionPages(const USuperSLMSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? static_cast<int64>(Subsystem.State->PrefixCache.RegionCPages) : 0;
}

FString FSuperSLMPrefixCacheTestSeam::DiskFileOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->DiskFileOf(Spec) : FString();
}

void FSuperSLMPrefixCacheTestSeam::ArmTeardownProbe(USuperSLMSubsystem& Subsystem)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		Host->Test.bTeardownProbeArmed = true;
		Host->Test.TeardownFreePages = -1;
		Host->Test.LastPoolDestroyStatus = MIN_int32;
	}
}

int64 FSuperSLMPrefixCacheTestSeam::TeardownFreePages(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.TeardownFreePages : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::LastPoolDestroyStatus(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.LastPoolDestroyStatus : MIN_int32;
}

int32 FSuperSLMPrefixCacheTestSeam::LegacyPrefixLive(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.LegacyLive.Num() : 0;
}

bool FSuperSLMPrefixCacheTestSeam::SetWrapperLayer1(TArray<uint8>& WrappedBlob, const FString& Tag, const FString& Commit)
{
	SuperSLMSaveBlob::FContents Contents;
	const uint8* Payload = nullptr;
	int64 PayloadBytes = 0;
	FString Error;
	if (!SuperSLMSaveBlob::Read(WrappedBlob, Contents, Payload, PayloadBytes, Error))
	{
		return false;
	}
	const TArray<uint8> PayloadCopy(Payload, static_cast<int32>(PayloadBytes));
	Contents.Layer1Tag = Tag;
	Contents.Layer1Commit = Commit;
	TArray<uint8> Rewritten;
	const int64 Offset = SuperSLMSaveBlob::BeginWrite(Rewritten, Contents, PayloadBytes);
	FMemory::Memcpy(Rewritten.GetData() + Offset, PayloadCopy.GetData(), PayloadBytes);
	SuperSLMSaveBlob::FinalizePayload(Rewritten, Offset, PayloadBytes);
	WrappedBlob = MoveTemp(Rewritten);
	return true;
}

// The rev 1.18 seams (plan §8; suite audit G1, G2). Each reads its "not built" value (-1, Unknown) only when the
// subsystem has no cache host.
void FSuperSLMPrefixCacheTestSeam::SetBuildHold(USuperSLMSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.bBuildHold.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldBuildCount(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.HeldBuilds.load() : -1;
}

ESuperSLMPrefixBuildState FSuperSLMPrefixCacheTestSeam::BuildStateOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->BuildStateOf(Lease) : ESuperSLMPrefixBuildState::Unknown;
}

void FSuperSLMPrefixCacheTestSeam::SetBuildChunkHold(USuperSLMSubsystem& Subsystem, int32 AfterChunkJobs)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.ChunkHoldAfter.store(AfterChunkJobs < 0 ? -1 : AfterChunkJobs); }
}

int32 FSuperSLMPrefixCacheTestSeam::BuildChunkJobsOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->ChunkJobsOf(Lease) : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::ChunkJobsIssuedInTeardown(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.ChunkJobsIssuedInTeardown : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::UnfrozenBuildReleases(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.UnfrozenBuildReleases : 0;
}

int32 FSuperSLMPrefixCacheTestSeam::InFlightBuildCount(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->InFlightBuildCount() : -1;
}

void FSuperSLMPrefixCacheTestSeam::HoldWriteBacks(USuperSLMSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.bHoldWriteBacks.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldWriteBackCount(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.HeldWriteBacks.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxDiskExclusion(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.MaxDiskExclusion.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::SetOffGameThreadRecording(USuperSLMSubsystem& Subsystem, bool bRecord)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost) { Host->Test.bRecordOffGameThread.store(bRecord); }
}

int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadViolations(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.OffGameThreadViolations.load() : -1;
}

int64 FSuperSLMPrefixCacheTestSeam::LastRestoreSharedPages(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.LastRestoreSharedPages : -1;
}

bool FSuperSLMPrefixCacheTestSeam::OffGameThreadAssertTrips(USuperSLMSubsystem& Subsystem)
{
	FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr && Host->OffGameThreadAssertTrips();
}

bool FSuperSLMPrefixCacheTestSeam::GameThreadAccessorTrips(USuperSLMSubsystem& Subsystem)
{
	FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr && Host->GameThreadAccessorTrips();
}

// The §8 seams the U2 code review found missing (finding 2). Each reads "not built" (-1) with no cache host.
void FSuperSLMPrefixCacheTestSeam::HoldDiskReads(USuperSLMSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		Host->Test.bHoldDiskReads.store(bHold);
	}
}

int32 FSuperSLMPrefixCacheTestSeam::HeldDiskReadCount(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.HeldDiskReads.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxKeyDiskOverlap(const USuperSLMSubsystem& Subsystem)
{
	const FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost;
	return Host != nullptr ? Host->Test.MaxKeyDiskOverlap.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::SetDeleteRefusal(USuperSLMSubsystem& Subsystem, bool bRefuse)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		Host->Test.bRefuseDeletes.store(bRefuse);
	}
}

void FSuperSLMPrefixCacheTestSeam::HoldKeyHash(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, bool bHold)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		Host->SetHashHold(Spec.Tokens, Spec.SchemaName.IsNone() ? FString() : Spec.SchemaName.ToString(), bHold);
	}
}

void FSuperSLMPrefixCacheTestSeam::SubstituteReleaseStatus(USuperSLMSubsystem& Subsystem, sslm_status Status)
{
	if (FSuperSLMPrefixCacheHost* Host = Subsystem.PrefixCacheHost)
	{
		Host->Test.SubstitutedReleaseStatus.store(static_cast<int32>(Status));
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
