// Precache plan rev 1.18.8 (§5.3 item 4, §15.27): the sliced CPU cache build's host twin, cells 5.9 (a), (b) and 1.7's
// between-chunks leg. The chain's progress (Core/SuperSLMBuildSlicing.h) is driven here against a fake engine whose
// prefill folds each (position, token) pair into its state, so a build's "save" is a function of the ids and their
// positions alone, as the engine's is. The Unreal halves (the lane jobs, the ledger rows, a generation on the lane
// between chunks, the real 0.5B artifact's bytes) are the editor cells and the box's OneJobBuild leg.
//
// Written red first against a stubbed SuperSLMBuildSlicing.h (one chunk of every token left, freeze on any prefill,
// no teardown release): 5 of 9 red. Mutants (each turns a test here red, and the named box check):
//   - one job (the planner ignored, every token in one chunk)    -> build_slicing_a_2000_token_build_is_sliced; 5.9 (a)
//   - a chunk priced at the begin price                           -> build_slicing_a_2000_token_build_is_sliced; 5.9 (a)
//   - a freeze before the last chunk (OnPrefilled true early)     -> build_slicing_freezes_only_at_the_last_chunk,
//                                                                    build_slicing_is_byte_equal_to_one_job; 5.9 (b)
//   - a chunk issued while tearing down                           -> build_slicing_teardown_between_chunks; 1.7
//   - the unfrozen prefix handed out twice, or never              -> build_slicing_teardown_between_chunks; 1.7
//   - rev 1.18.10 (§15.29, code review S1): the chunk dispatched last only, so a decoding generation holds its lane
//     (LaneHeldForChunk never true)                               -> build_slicing_a_decoding_generation_gets_one_job_
//                                                                    between_chunks, build_slicing_the_hold_waits_for_a_
//                                                                    job_since_the_builds_last; 5.9 (c)'s bound

#include "harness.h"
#include "SuperSLMBuildSlicing.h"

#include <cstdint>
#include <vector>

using namespace SuperSLMCore;

namespace
{
	// CreatePrefix's planner and price, replicated from SuperSLMPrefillPlanning.h: a chunk of C tokens at depth d costs
	// C x TokenMs + PerPositionMs x (C x d + C x (C - 1) / 2), with the chunk-1 pair below 8 tokens and the batched
	// pair from 8 up; the chunk is the largest within each segment that fits the share, raised to the floor.
	struct FPrice
	{
		double TokenMs = 0.0;
		double PerPositionMs = 0.0;
	};

	class FReplicaPricer final : public IBuildChunkPricer
	{
	public:
		FReplicaPricer(double InShareMs, int32_t InMinChunk, FPrice InChunk1, FPrice InBatched)
			: ShareMs(InShareMs), MinChunk(InMinChunk), Chunk1(InChunk1), Batched(InBatched)
		{
		}

		static double PriceMs(int32_t C, int64_t D, const FPrice& P)
		{
			const double Positions = static_cast<double>(C) * static_cast<double>(D) + static_cast<double>(C) * static_cast<double>(C - 1) / 2.0;
			return static_cast<double>(C) * P.TokenMs + P.PerPositionMs * Positions;
		}

		int32_t PlanChunk(int32_t MaxChunk, int64_t StartDepth) const override
		{
			int32_t Chunk = 1;
			const int32_t Chunk1Last = MaxChunk < 7 ? MaxChunk : 7;
			for (int32_t C = 2; C <= Chunk1Last; ++C)
			{
				if (PriceMs(C, StartDepth, Chunk1) > ShareMs)
				{
					break;
				}
				Chunk = C;
			}
			for (int32_t C = 8; C <= MaxChunk; ++C)
			{
				if (PriceMs(C, StartDepth, Batched) > ShareMs)
				{
					break;
				}
				Chunk = C;
			}
			const int32_t Floor = MinChunk < MaxChunk ? MinChunk : MaxChunk;
			return Chunk > Floor ? Chunk : Floor;
		}

		double ChunkCostMs(int32_t Chunk, int64_t StartDepth) const override
		{
			return PriceMs(Chunk, StartDepth, Chunk >= 8 ? Batched : Chunk1);
		}

		double ShareMs;
		int32_t MinChunk;
		FPrice Chunk1;
		FPrice Batched;
	};

	// The box's shape: a 16.6 ms tick at 0.8 headroom, the 0.5B's measured order of prompt prices.
	FReplicaPricer BoxPricer()
	{
		return FReplicaPricer(16.6 * 0.8, 1, FPrice{1.2, 0.0004}, FPrice{0.35, 0.00012});
	}

	constexpr double kBeginPriceMs = 2.261; // U4's reading: the one job's price
	constexpr int32_t kMaxChunkBudget = 64;

	// A fake prefix: prefill folds each (position, token) into the state, as attention makes a prefix's KV a
	// function of its tokens and their positions; "save" is the state and the position count.
	struct FFakePrefix
	{
		uint64_t State = 0xcbf29ce484222325ull;
		int32_t Positions = 0;
		bool bFrozen = false;
	};

	// One call: consumes min(Left, Chunk) tokens; refused once frozen.
	int32_t FakePrefill(FFakePrefix& P, const int32_t* Tokens, int32_t Left, int32_t Chunk)
	{
		if (P.bFrozen || Left <= 0 || Chunk <= 0)
		{
			return 0;
		}
		const int32_t N = Left < Chunk ? Left : Chunk;
		for (int32_t I = 0; I < N; ++I)
		{
			const uint64_t Pair = (static_cast<uint64_t>(static_cast<uint32_t>(P.Positions)) << 32) | static_cast<uint32_t>(Tokens[I]);
			P.State = (P.State ^ Pair) * 0x100000001b3ull;
			++P.Positions;
		}
		return N;
	}

	std::vector<int32_t> Ids(int32_t Length, int32_t Seed)
	{
		std::vector<int32_t> Out;
		for (int32_t I = 0; I < Length; ++I)
		{
			Out.push_back(5000 + ((I * 7919 + Seed) % 30000));
		}
		return Out;
	}

	// The one-job build (rev 1.18): begin, then a loop of calls of min(left, 64), then freeze. The fake engine folds each
	// (position, token) pair, so its result cannot depend on where chunk boundaries fall: the byte-equal test below
	// shows the chain carries tokens and positions correctly, and nothing more. Real chunk invariance (the engine's
	// prefill being bit-identical at every chunk size) is the packaged leg's to show (5.9 (b) on the box).
	FFakePrefix OneJobBuild(const std::vector<int32_t>& Tokens, int32_t Origin)
	{
		FFakePrefix P;
		P.Positions = Origin;
		int32_t Done = 0;
		const int32_t Length = static_cast<int32_t>(Tokens.size());
		while (Done < Length)
		{
			const int32_t Left = Length - Done;
			const int32_t Consumed = FakePrefill(P, Tokens.data() + Done, Left, Left < kMaxChunkBudget ? Left : kMaxChunkBudget);
			if (Consumed <= 0)
			{
				break;
			}
			Done += Consumed;
		}
		P.bFrozen = true;
		return P;
	}

	// The sliced build: the begin job, then one job per planned chunk, the last freezing; a "generation" (another
	// fake prefix) runs between every two chunk jobs, as other jobs may on the lane.
	struct FSlicedRun
	{
		FFakePrefix Prefix;
		std::vector<FBuildChunkPlan> Plans;
		int32_t Interleaved = 0;
		int32_t Freezes = 0;
		EBuildSliceState End = EBuildSliceState::NotBegun;
	};

	FSlicedRun SlicedBuild(const std::vector<int32_t>& Tokens, int32_t Origin, const IBuildChunkPricer& Pricer)
	{
		FSlicedRun Run;
		Run.Prefix.Positions = Origin;
		FBuildSlices Slices(static_cast<int32_t>(Tokens.size()), Origin);
		Slices.OnBegun(0x1000);
		FFakePrefix Generation;
		const std::vector<int32_t> GenerationTokens = Ids(4096, 99);
		while (Slices.WantsNextChunk(false))
		{
			if (!Run.Plans.empty())
			{
				FakePrefill(Generation, GenerationTokens.data() + Run.Interleaved % 4000, 1, 1); // another job on the lane
				++Run.Interleaved;
			}
			const FBuildChunkPlan Plan = Slices.PlanNext(Pricer, kMaxChunkBudget);
			Run.Plans.push_back(Plan);
			const int32_t Done = Slices.GetDone();
			const int32_t Left = Slices.GetLength() - Done;
			const int32_t Consumed = FakePrefill(Run.Prefix, Tokens.data() + Done, Left, Plan.Tokens);
			if (Slices.OnPrefilled(Consumed))
			{
				Run.Prefix.bFrozen = true;
				++Run.Freezes;
				Slices.OnFrozen();
			}
			else if (Slices.MadeNoProgress())
			{
				Slices.OnFailed();
			}
		}
		Run.End = Slices.GetState();
		return Run;
	}
}

HOST_TEST(build_slicing_a_2000_token_build_is_sliced, "5.9 (a)")
{
	const FReplicaPricer Pricer = BoxPricer();
	const std::vector<FBuildChunkPlan> Plans = PlanBuildChunks(2000, 0, Pricer, kMaxChunkBudget);
	REQUIRE_TRUE(Plans.size() > 1);
	int64_t Sum = 0;
	int64_t Depth = 0;
	for (size_t I = 0; I < Plans.size(); ++I)
	{
		const FBuildChunkPlan& P = Plans[I];
		EXPECT_TRUE(P.Tokens >= 1 && P.Tokens <= kMaxChunkBudget);
		EXPECT_EQ(P.StartDepth, Depth); // in order, contiguous
		// Priced by the chunk planner's own price at the chunk's depth, never the begin's flat price, and within the slice.
		EXPECT_TRUE(P.PlannedMs == Pricer.ChunkCostMs(P.Tokens, P.StartDepth));
		EXPECT_TRUE(P.PlannedMs <= Pricer.ShareMs);
		EXPECT_TRUE(P.PlannedMs != kBeginPriceMs);
		EXPECT_EQ(P.bLast, I + 1 == Plans.size());
		Sum += P.Tokens;
		Depth += P.Tokens;
	}
	EXPECT_EQ(Sum, int64_t(2000));
	// The depth term: a later chunk is never larger than an earlier one under one price pair.
	EXPECT_TRUE(Plans.back().Tokens <= Plans.front().Tokens);
}

HOST_TEST(build_slicing_a_nested_build_starts_after_its_parent, "5.9 (a)")
{
	const FReplicaPricer Pricer = BoxPricer();
	const std::vector<FBuildChunkPlan> Plans = PlanBuildChunks(500, 300, Pricer, kMaxChunkBudget);
	REQUIRE_TRUE(!Plans.empty());
	EXPECT_EQ(Plans.front().StartDepth, int64_t(300));
	EXPECT_TRUE(Plans.front().PlannedMs == Pricer.ChunkCostMs(Plans.front().Tokens, 300));
	int64_t Sum = 0;
	for (const FBuildChunkPlan& P : Plans)
	{
		EXPECT_EQ(P.StartDepth, int64_t(300) + Sum);
		Sum += P.Tokens;
	}
	EXPECT_EQ(Sum, int64_t(500));
}

HOST_TEST(build_slicing_the_chunk_is_clamped_to_the_budget_and_the_tokens_left, "5.9 (a)")
{
	// A planner that answers out of range is clamped to [1, min(budget, left)].
	class FWild final : public IBuildChunkPricer
	{
	public:
		int32_t Answer = 0;
		int32_t PlanChunk(int32_t, int64_t) const override { return Answer; }
		double ChunkCostMs(int32_t Chunk, int64_t) const override { return static_cast<double>(Chunk); }
	};
	FWild Wild;
	FBuildSlices Slices(10, 0);
	Slices.OnBegun(7);
	Wild.Answer = 0;
	EXPECT_EQ(Slices.PlanNext(Wild, 64).Tokens, 1);
	Wild.Answer = 1000;
	EXPECT_EQ(Slices.PlanNext(Wild, 64).Tokens, 10);
	EXPECT_TRUE(Slices.PlanNext(Wild, 64).bLast);
	EXPECT_EQ(Slices.PlanNext(Wild, 4).Tokens, 4);
	EXPECT_TRUE(!Slices.PlanNext(Wild, 4).bLast);
	// Nothing is planned before begin.
	FBuildSlices NotBegun(10, 0);
	EXPECT_EQ(NotBegun.PlanNext(Wild, 64).Tokens, 0);
}

HOST_TEST(build_slicing_is_byte_equal_to_one_job, "5.9 (b)")
{
	const FReplicaPricer Pricer = BoxPricer();
	for (const int32_t Length : {1, 63, 64, 65, 300, 2000})
	{
		for (const int32_t Origin : {0, 128})
		{
			const std::vector<int32_t> Tokens = Ids(Length, Length + Origin);
			const FFakePrefix One = OneJobBuild(Tokens, Origin);
			const FSlicedRun Sliced = SlicedBuild(Tokens, Origin, Pricer);
			EXPECT_EQ(Sliced.End, EBuildSliceState::Frozen);
			EXPECT_EQ(Sliced.Freezes, 1);
			EXPECT_EQ(Sliced.Prefix.State, One.State);
			EXPECT_EQ(Sliced.Prefix.Positions, One.Positions);
			// Interleaved: another job ran between every two chunk jobs (5.9 (c)'s shape; the box grades it on the lane).
			EXPECT_EQ(Sliced.Interleaved, static_cast<int32_t>(Sliced.Plans.size()) - 1);
		}
	}
}

HOST_TEST(build_slicing_freezes_only_at_the_last_chunk, "5.9 (b)")
{
	FBuildSlices Slices(100, 0);
	Slices.OnBegun(42);
	EXPECT_TRUE(!Slices.OnPrefilled(40));
	Slices.OnFrozen(); // a freeze before the last chunk is not taken
	EXPECT_EQ(Slices.GetState(), EBuildSliceState::Prefilling);
	EXPECT_TRUE(!Slices.IsFinished());
	EXPECT_TRUE(Slices.WantsNextChunk(false));
	EXPECT_TRUE(!Slices.OnPrefilled(59));
	EXPECT_TRUE(Slices.OnPrefilled(1));
	Slices.OnFrozen();
	EXPECT_EQ(Slices.GetState(), EBuildSliceState::Frozen);
	EXPECT_TRUE(Slices.IsFinished());
	EXPECT_TRUE(!Slices.WantsNextChunk(false));
	EXPECT_EQ(Slices.GetChunkJobs(), 3);
	EXPECT_EQ(Slices.GetHandle(), uint64_t(42));
}

HOST_TEST(build_slicing_a_call_without_progress_fails_the_build, "5.9")
{
	FBuildSlices Slices(100, 0);
	Slices.OnBegun(42);
	EXPECT_TRUE(!Slices.OnPrefilled(0));
	EXPECT_TRUE(Slices.MadeNoProgress());
	EXPECT_EQ(Slices.GetDone(), 0);
	FBuildSlices Over(10, 0);
	Over.OnBegun(43);
	EXPECT_TRUE(!Over.OnPrefilled(11)); // more than was left: never counted as done
	EXPECT_TRUE(Over.MadeNoProgress());
	Over.OnFailed();
	EXPECT_EQ(Over.GetState(), EBuildSliceState::Failed);
	EXPECT_TRUE(Over.IsFinished());
	EXPECT_EQ(Over.TakeUnfrozenHandle(), uint64_t(0)); // the job released it
}

HOST_TEST(build_slicing_teardown_between_chunks, "1.7")
{
	const FReplicaPricer Pricer = BoxPricer();
	FBuildSlices Slices(2000, 0);
	Slices.OnBegun(0xbeef);
	for (int32_t Chunk = 0; Chunk < 2; ++Chunk)
	{
		const FBuildChunkPlan Plan = Slices.PlanNext(Pricer, kMaxChunkBudget);
		EXPECT_TRUE(!Slices.OnPrefilled(Plan.Tokens));
	}
	REQUIRE_TRUE(Slices.WantsNextChunk(false));
	// The re-Configure(): the chunk delivered under the teardown issues no further chunk.
	EXPECT_TRUE(!Slices.WantsNextChunk(true));
	EXPECT_TRUE(!Slices.IsFinished()); // no completion: the core resolves the request Cancelled
	// The unfrozen prefix is released exactly once.
	EXPECT_EQ(Slices.TakeUnfrozenHandle(), uint64_t(0xbeef));
	EXPECT_EQ(Slices.TakeUnfrozenHandle(), uint64_t(0));
	EXPECT_EQ(Slices.GetState(), EBuildSliceState::Released);
	// Nothing after it: no plan, no chunk, no freeze.
	EXPECT_EQ(Slices.PlanNext(Pricer, kMaxChunkBudget).Tokens, 0);
	EXPECT_TRUE(!Slices.WantsNextChunk(false));
	EXPECT_TRUE(!Slices.OnPrefilled(10));
	Slices.OnFrozen();
	EXPECT_EQ(Slices.GetState(), EBuildSliceState::Released);
}

HOST_TEST(build_slicing_nothing_to_release_unless_begun_and_unfrozen, "1.7")
{
	FBuildSlices NotBegun(10, 0);
	EXPECT_EQ(NotBegun.TakeUnfrozenHandle(), uint64_t(0));
	EXPECT_EQ(NotBegun.GetState(), EBuildSliceState::NotBegun);
	FBuildSlices Frozen(10, 0);
	Frozen.OnBegun(9);
	EXPECT_TRUE(Frozen.OnPrefilled(10));
	Frozen.OnFrozen();
	EXPECT_EQ(Frozen.TakeUnfrozenHandle(), uint64_t(0)); // the cache's handle: released by the teardown sweep, not here
	EXPECT_EQ(Frozen.GetHandle(), uint64_t(9));
	FBuildSlices BeginFailed(10, 0);
	BeginFailed.OnFailed();
	EXPECT_EQ(BeginFailed.GetState(), EBuildSliceState::Failed);
	EXPECT_EQ(BeginFailed.TakeUnfrozenHandle(), uint64_t(0));
}

HOST_TEST(build_slicing_a_one_token_build_is_one_chunk_job, "5.9 (a)")
{
	const FReplicaPricer Pricer = BoxPricer();
	const std::vector<FBuildChunkPlan> Plans = PlanBuildChunks(1, 0, Pricer, kMaxChunkBudget);
	REQUIRE_EQ(Plans.size(), size_t(1));
	EXPECT_EQ(Plans[0].Tokens, 1);
	EXPECT_TRUE(Plans[0].bLast);
	EXPECT_TRUE(PlanBuildChunks(0, 0, Pricer, kMaxChunkBudget).empty());
}

// Code review S1 (plan rev 1.18.10, §15.29): a generation that keeps decoding on the build's lane must not hold the
// build's next chunk back for its whole length. The lane below replicates the CPU tick's order on one lane (Apply
// delivers each job at its committed tick; then PlanNextJobs, PlanContinuations, and the cache's chunk dispatch last,
// as SuperSLMSubsystem.cpp's Tick runs them) around the production rule, LaneHeldForChunk: the subsystem's
// FLane::IsFreeForPlanning and PlanContinuations both refuse a lane it holds, and only the chunk is posted there.
namespace
{
	enum class ESimJob : uint8_t { Begin, Chunk, Generation };

	struct FSimLaneRun
	{
		bool bBuildDone = false;
		int32_t Ticks = 0;
		int32_t ChunkJobs = 0;
		int32_t MaxGenerationJobsBetweenBuildJobs = 0; // posted on the lane between two consecutive build jobs
		int32_t GenerationJobsDuringBuild = 0;
	};

	// One lane, one build of Length tokens (begin at tick 0), and a generation that starts at tick 1 and never ends:
	// each of its jobs is planned the moment it may be. bAlternate false runs the lane without the hold (the chunk
	// dispatched last only), the hazard S1 names.
	FSimLaneRun RunSimLane(int32_t Length, bool bAlternate, int32_t MaxTicks)
	{
		struct FPending { int64_t Id; int32_t Committed; ESimJob Kind; };
		std::vector<FPending> Pending;
		int64_t NextJobId = 1;
		int64_t LaneLastPostedJobId = 0;
		const FReplicaPricer Pricer = BoxPricer();
		FBuildSlices Slices(Length, 0);
		FBuildChunkPlan Running;
		bool bChunkWaiting = false;
		int64_t BuildLastJobId = 0;
		int32_t GenerationUndelivered = 0;
		int32_t GenerationSinceBuildJob = 0;
		FSimLaneRun Out;

		auto Post = [&](ESimJob Kind, int32_t Committed)
		{
			const int64_t Id = NextJobId++;
			Pending.push_back(FPending{ Id, Committed, Kind });
			LaneLastPostedJobId = Id;
			if (Kind == ESimJob::Generation)
			{
				++GenerationUndelivered;
				++GenerationSinceBuildJob;
				Out.GenerationJobsDuringBuild += Out.bBuildDone ? 0 : 1;
			}
			else
			{
				BuildLastJobId = Id;
				Out.MaxGenerationJobsBetweenBuildJobs = GenerationSinceBuildJob > Out.MaxGenerationJobsBetweenBuildJobs ? GenerationSinceBuildJob : Out.MaxGenerationJobsBetweenBuildJobs;
				GenerationSinceBuildJob = 0;
			}
		};
		auto Held = [&]()
		{
			return bAlternate && LaneHeldForChunk(LaneLastPostedJobId, bChunkWaiting ? BuildLastJobId : 0);
		};
		auto FreeOnSchedule = [&](int32_t Tick) { return Pending.empty() || Tick >= Pending.back().Committed; };

		Post(ESimJob::Begin, 1);
		for (int32_t Tick = 1; Tick <= MaxTicks && !Out.bBuildDone; ++Tick)
		{
			Out.Ticks = Tick;
			// Apply: the lane's jobs in order, each at its committed tick (no overrun).
			while (!Pending.empty() && Pending.front().Committed <= Tick)
			{
				const FPending Done = Pending.front();
				Pending.erase(Pending.begin());
				if (Done.Kind == ESimJob::Generation)
				{
					--GenerationUndelivered;
				}
				else if (Done.Kind == ESimJob::Begin)
				{
					Slices.OnBegun(7);
					bChunkWaiting = true;
				}
				else if (Slices.OnPrefilled(Running.Tokens))
				{
					Slices.OnFrozen();
					Out.bBuildDone = true;
				}
				else
				{
					bChunkWaiting = Slices.WantsNextChunk(false);
				}
			}
			// PlanNextJobs: the generation's next job, when it has none undelivered and the lane is free for planning.
			if (GenerationUndelivered == 0 && !Held() && FreeOnSchedule(Tick))
			{
				Post(ESimJob::Generation, Tick + 1);
			}
			// PlanContinuations: one more behind the generation's running job, the lane's only pending job.
			if (GenerationUndelivered == 1 && Pending.size() == 1 && Pending.front().Kind == ESimJob::Generation && !Held())
			{
				Post(ESimJob::Generation, Pending.front().Committed + 1);
			}
			// The cache's chunk dispatch, last: the waiting chunk, when the lane is free for it.
			if (bChunkWaiting && FreeOnSchedule(Tick))
			{
				Running = Slices.PlanNext(Pricer, kMaxChunkBudget);
				const int32_t K = static_cast<int32_t>(Running.PlannedMs / 16.6) + 1;
				Post(ESimJob::Chunk, Tick + K);
				++Out.ChunkJobs;
				bChunkWaiting = false;
			}
		}
		return Out;
	}
}

HOST_TEST(build_slicing_the_hold_waits_for_a_job_since_the_builds_last, "5.9 (c)")
{
	EXPECT_TRUE(!LaneHeldForChunk(10, 0));  // no chunk waits
	EXPECT_TRUE(!LaneHeldForChunk(10, 10)); // the build's own job is the lane's last: other jobs may still go first
	EXPECT_TRUE(LaneHeldForChunk(11, 10));  // one job has gone since: the chunk is next
	EXPECT_TRUE(!LaneHeldForChunk(9, 10));
}

HOST_TEST(build_slicing_a_decoding_generation_gets_one_job_between_chunks, "5.9 (c)")
{
	// Mutant: the chunk dispatched last only (LaneHeldForChunk never true): the generation keeps the lane through its
	// continuations, the build never finishes, and this test turns red.
	const FSimLaneRun Run = RunSimLane(640, /*bAlternate*/ true, 20000);
	REQUIRE_TRUE(Run.bBuildDone);
	EXPECT_TRUE(Run.ChunkJobs > 1);
	EXPECT_EQ(Run.MaxGenerationJobsBetweenBuildJobs, 1); // the generation still runs between chunks, one job at a time
	EXPECT_TRUE(Run.GenerationJobsDuringBuild >= Run.ChunkJobs - 1);
}

HOST_TEST(build_slicing_without_the_hold_a_decoding_generation_starves_the_build, "5.9 (c)")
{
	// The hazard itself, so the test above is known to cover it: chunks dispatched last only never get the lane back.
	const FSimLaneRun Run = RunSimLane(640, /*bAlternate*/ false, 20000);
	EXPECT_TRUE(!Run.bBuildDone);
	EXPECT_EQ(Run.ChunkJobs, 0);
}
