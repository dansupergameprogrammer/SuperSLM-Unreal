#pragma once

// The sliced CPU cache build (precache plan rev 1.18.8, §5.3 item 4 and §15.27; cells 5.9 and 1.7). Plain C++, no
// engine and no Unreal types, so the host suite tests it (ci/host/prefix_cache, test_build_slicing.cpp).
//
// A CPU cache build is one request carried by a chain of lane jobs on one lane:
//   - the begin job: begin (budgeted, or from the parent for a nested build) and the schema bind. The build hold stays
//     in it, after begin and before any prefill, so 8.10 (c)'s reading is unchanged;
//   - one chunk job per planned chunk: one sslm_prefix_prefill call of the chunk CreatePrefix's planner gives
//     (PlanPrefillChunk over min(MaxPrefillChunkBudget, tokens left) at the chunk's start depth), priced by
//     CreatePrefix's per-chunk price (PrefillChunkCostMs, with the depth term at that start). The job whose prefill
//     reaches the build's length also freezes; no other job freezes.
// Other jobs may run on the lane between two chunk jobs, at most one between any two (LaneHeldForChunk, below). The
// chain is one request throughout (the core sees one Build command and one completion), so its charge, pins,
// coalescing, waiters and in-flight state are unchanged.
//
// FBuildSlices is a build's progress. Its lane half (OnBegun, OnPrefilled, OnFrozen, OnFailed) is written by the job
// running on the lane, and its game-thread half (PlanNext, WantsNextChunk, TakeUnfrozenHandle) is read at the job's
// dispatch and delivery: the lane's job completion orders the two, as it orders every job's output.
//
// At a re-Configure() the chunk running drains with the teardown, no further chunk is issued (WantsNextChunk is
// false while tearing down), and the unfrozen prefix is released exactly once before the pool is destroyed
// (TakeUnfrozenHandle hands it out once).

#include <cstdint>
#include <vector>

namespace SuperSLMCore
{
	// One chunk job as planned.
	struct FBuildChunkPlan
	{
		int32_t Tokens = 0;     // the chunk: what the job's one prefill call is asked to consume
		int64_t StartDepth = 0; // the chunk's first position: the parent's length plus the tokens already prefilled
		double PlannedMs = 0.0; // CreatePrefix's per-chunk price at that depth
		bool bLast = false;     // planned to reach the build's length (the job freezes when its prefill does)
	};

	// CreatePrefix's chunk planner and its price, as the subsystem resolves them (SuperSLMPrefillPlanning).
	class IBuildChunkPricer
	{
	public:
		virtual ~IBuildChunkPricer() = default;
		// The chunk for MaxChunk tokens at most (already min(MaxPrefillChunkBudget, tokens left)), at StartDepth.
		virtual int32_t PlanChunk(int32_t MaxChunk, int64_t StartDepth) const = 0;
		virtual double ChunkCostMs(int32_t Chunk, int64_t StartDepth) const = 0;
	};

	enum class EBuildSliceState : uint8_t
	{
		NotBegun,   // the begin job has not returned OK
		Prefilling, // begun (and bound): chunk jobs run
		Frozen,     // the last chunk's job froze it: the build's handle is the cache's
		Failed,     // a call failed; the job released what it had begun
		Released,   // torn down unfrozen: TakeUnfrozenHandle handed the handle out for the teardown's release
	};

	class FBuildSlices
	{
	public:
		FBuildSlices(int32_t InLength, int32_t InOriginLength)
			: Length(InLength < 0 ? 0 : InLength)
			, OriginLength(InOriginLength < 0 ? 0 : InOriginLength)
		{
		}

		int32_t GetLength() const { return Length; }
		int32_t GetDone() const { return Done; }
		int32_t GetChunkJobs() const { return ChunkJobs; }
		EBuildSliceState GetState() const { return State; }
		uint64_t GetHandle() const { return Handle; }

		// Lane, the begin job: begin returned OK (and the schema bound).
		void OnBegun(uint64_t InHandle)
		{
			if (State == EBuildSliceState::NotBegun)
			{
				Handle = InHandle;
				State = EBuildSliceState::Prefilling;
			}
		}

		// Game thread, at a chunk job's dispatch: the chunk CreatePrefix's planner gives at the build's next depth.
		FBuildChunkPlan PlanNext(const IBuildChunkPricer& Pricer, int32_t MaxChunkBudget) const
		{
			FBuildChunkPlan Plan;
			const int32_t Left = Length - Done;
			if (State != EBuildSliceState::Prefilling || Left <= 0)
			{
				return Plan;
			}
			const int32_t MaxChunk = MaxChunkBudget < 1 ? 1 : (MaxChunkBudget < Left ? MaxChunkBudget : Left);
			Plan.StartDepth = static_cast<int64_t>(OriginLength) + Done;
			int32_t Chunk = Pricer.PlanChunk(MaxChunk, Plan.StartDepth);
			Chunk = Chunk < 1 ? 1 : (Chunk > MaxChunk ? MaxChunk : Chunk);
			Plan.Tokens = Chunk;
			Plan.PlannedMs = Pricer.ChunkCostMs(Chunk, Plan.StartDepth);
			Plan.bLast = Chunk >= Left;
			return Plan;
		}

		// Lane, a chunk job: its prefill call returned OK having consumed Consumed tokens. True when every token is now
		// prefilled, so this job freezes. A call that consumed nothing (or more than was left) is a failure: false, and
		// the caller fails the build (never loops, never freezes a short build).
		bool OnPrefilled(int32_t Consumed)
		{
			if (State != EBuildSliceState::Prefilling || Consumed <= 0 || Consumed > Length - Done)
			{
				bNoProgress = true;
				return false;
			}
			Done += Consumed;
			++ChunkJobs;
			return Done == Length;
		}

		// A call that made no progress (OnPrefilled returned false without completing).
		bool MadeNoProgress() const { return bNoProgress; }

		// Lane: the freeze returned OK.
		void OnFrozen()
		{
			if (State == EBuildSliceState::Prefilling && Done == Length)
			{
				State = EBuildSliceState::Frozen;
			}
		}

		// Lane: a call failed and the job released what it had begun (or begin itself failed).
		void OnFailed()
		{
			if (State == EBuildSliceState::NotBegun || State == EBuildSliceState::Prefilling)
			{
				State = EBuildSliceState::Failed;
				Handle = 0;
			}
		}

		// Game thread, at a job's delivery: the build has its one completion (frozen, or failed).
		bool IsFinished() const { return State == EBuildSliceState::Frozen || State == EBuildSliceState::Failed; }

		// Game thread, at a job's delivery: a further chunk job is issued. Never while tearing down.
		bool WantsNextChunk(bool bTearingDown) const
		{
			return !bTearingDown && State == EBuildSliceState::Prefilling && Done < Length;
		}

		// Teardown: the begun, unfrozen handle the pool destroy must release first; 0 when there is none. Once: the
		// build is then Released and a second call returns 0.
		uint64_t TakeUnfrozenHandle()
		{
			if (State != EBuildSliceState::Prefilling)
			{
				return 0;
			}
			const uint64_t Out = Handle;
			Handle = 0;
			State = EBuildSliceState::Released;
			return Out;
		}

	private:
		int32_t Length = 0;
		int32_t OriginLength = 0;
		int32_t Done = 0;
		int32_t ChunkJobs = 0;
		uint64_t Handle = 0;
		EBuildSliceState State = EBuildSliceState::NotBegun;
		bool bNoProgress = false;
	};

	// Code review S1 (plan rev 1.18.10, §15.29): the alternation on a build's lane. A generation that keeps decoding on
	// the lane would otherwise keep it busy for its whole length (its jobs are planned before the chunk each tick, and
	// its continuations queue behind the job running), and hold the build's next chunk back as long. So once a chunk
	// is waiting (the build's previous job, its begin or its last chunk, has delivered) and the lane has taken one job
	// since that previous job, the lane takes no other new job until the chunk is posted: the chunk goes ahead of
	// further generation jobs, and between two chunks of a build at most one other job is posted on its lane.
	//   LaneLastPostedJobId     the id of the last job posted on the lane (any kind);
	//   ChunkWaitingAfterJobId  the id of the build's previous job on the lane while its next chunk waits; 0 when no
	//                           chunk waits there.
	// True when the lane is held for the chunk: no job but the chunk may be posted on it.
	inline bool LaneHeldForChunk(int64_t LaneLastPostedJobId, int64_t ChunkWaitingAfterJobId)
	{
		return ChunkWaitingAfterJobId > 0 && LaneLastPostedJobId > ChunkWaitingAfterJobId;
	}

	// The chunks a build of Length tokens is planned as when every call consumes its whole chunk: the schedule the
	// chain follows, in order. For the cells and the box's ledger check.
	inline std::vector<FBuildChunkPlan> PlanBuildChunks(int32_t Length, int32_t OriginLength, const IBuildChunkPricer& Pricer, int32_t MaxChunkBudget)
	{
		std::vector<FBuildChunkPlan> Out;
		FBuildSlices Slices(Length, OriginLength);
		Slices.OnBegun(1);
		while (Slices.GetState() == EBuildSliceState::Prefilling && Slices.GetDone() < Slices.GetLength())
		{
			const FBuildChunkPlan Plan = Slices.PlanNext(Pricer, MaxChunkBudget);
			Out.push_back(Plan);
			if (Slices.OnPrefilled(Plan.Tokens))
			{
				break;
			}
		}
		return Out;
	}
}
