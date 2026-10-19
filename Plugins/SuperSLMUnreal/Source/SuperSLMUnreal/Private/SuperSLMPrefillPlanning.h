#pragma once

#include "CoreMinimal.h"

#include <cmath>

// Slice 1P (SuperSLM tiled-matmul plan of record, §4A.1 items 1, 2, 4 and 5; rev 11.1's
// amendments): the CPU scheduler's prefill sizing, its prompt price, the overrun rule and the
// prefill bound Configure() checks, as plain functions of plain values.
//
// Kept in one header with no engine or Layer-1 dependency so that the scheduler
// (SuperSLMSubsystem.cpp), the cost estimate (SuperSLMEstimateCostsCommand.cpp), the
// automation seams (FSuperSLMSchedulingTestAccess, FSuperSLMCalibrationTestAccess) and an
// out-of-engine replica harness can all compile the same text. Nothing here
// runs a Layer-1 call; everything here is game-thread planning arithmetic (C12).
namespace SuperSLMPrefillPlanning
{
	// The engine's tiled-GEMM threshold (SuperSLM `kTiledMinTokens`, src/matmul.cpp): a prefill call
	// of this many tokens or more reaches the register-tiled int8 kernel. The batched price applies
	// from here up, and Configure() warns when the call shape cannot reach it.
	constexpr int32 kTiledMinTokens = 8;

	// One prompt price: a chunk of C tokens starting at depth d costs
	// C x TokenMs + PerPositionMs x (C x d + C x (C - 1) / 2).
	struct FPromptPrice
	{
		double TokenMs = 0.0;
		double PerPositionMs = 0.0;
	};

	// The sum of the positions of Chunk prompt tokens starting at depth d,
	// Chunk x d + Chunk x (Chunk - 1) / 2 -- reported as PromptPositionSum.
	inline int64 PromptChunkPositionSum(int32 Chunk, int64 StartDepth)
	{
		return static_cast<int64>(Chunk) * StartDepth + static_cast<int64>(Chunk) * (Chunk - 1) / 2;
	}

	// Item 2: the pair a chunk of C tokens is priced with -- the batched pair at C >= kTiledMinTokens,
	// the chunk-1 pair below it. The model's form is the same for both.
	inline const FPromptPrice& SelectPromptPrice(int32 Chunk, const FPromptPrice& Chunk1, const FPromptPrice& Batched)
	{
		return Chunk >= kTiledMinTokens ? Batched : Chunk1;
	}

	inline double PromptPriceMs(int32 Chunk, int64 StartDepth, const FPromptPrice& Price)
	{
		return Chunk * Price.TokenMs + Price.PerPositionMs * static_cast<double>(PromptChunkPositionSum(Chunk, StartDepth));
	}

	// The planned cost of one prefill chunk, priced with the pair its own size selects.
	inline double PrefillChunkCostMs(int32 Chunk, int64 StartDepth, const FPromptPrice& Chunk1, const FPromptPrice& Batched)
	{
		return PromptPriceMs(Chunk, StartDepth, SelectPromptPrice(Chunk, Chunk1, Batched));
	}

	// Item 1: the chunk one prefill job carries. MaxChunk is already min(MaxPrefillChunkBudget, the
	// prompt tokens left). The planner takes the largest C in [1, MaxChunk] whose pair-selected price
	// fits ShareMs (1.0's rule, now over two price segments), then raises it to the floor
	// min(MinPrefillChunk, MaxChunk). Within one segment the price rises with C, so each segment is
	// scanned up from its first size and stops at its first misfit; the two segments are scanned
	// separately because the price can fall at C = kTiledMinTokens when the batched pair is cheaper.
	// With the two pairs equal and MinPrefillChunk 1 this is 1.0's PlanPrefillChunk exactly.
	inline int32 PlanPrefillChunk(int32 MaxChunk, int64 StartDepth, double ShareMs, int32 MinPrefillChunk,
		const FPromptPrice& Chunk1, const FPromptPrice& Batched)
	{
		int32 Chunk = 1;
		const int32 Chunk1Last = FMath::Min(MaxChunk, kTiledMinTokens - 1);
		for (int32 C = 2; C <= Chunk1Last; ++C)
		{
			if (PromptPriceMs(C, StartDepth, Chunk1) > ShareMs)
			{
				break;
			}
			Chunk = C;
		}
		for (int32 C = kTiledMinTokens; C <= MaxChunk; ++C)
		{
			if (PromptPriceMs(C, StartDepth, Batched) > ShareMs)
			{
				break;
			}
			Chunk = C;
		}
		return FMath::Max(Chunk, FMath::Min(MinPrefillChunk, MaxChunk));
	}

	// Item 4 (rev 11.1, audit F8): a job is a worker overrun when its own Layer-1 time exceeds its
	// own planned K ticks. K is the job's own K, never ChainedWaitTicks + K: a continuation's queue
	// wait is not worker time, and WorkerCallMs excludes it. At K = 1 this is 1.0's rule.
	inline bool IsWorkerOverrun(double WorkerCallMs, int32 K, double TickBudgetMs)
	{
		return WorkerCallMs > static_cast<double>(K) * TickBudgetMs;
	}

	// Item 5 (rev 11.1, audit F4): the largest planned cost a prefill job can reach, one definition.
	// With P = min(MaxPrefillChunkBudget, context_cap) and D = context_cap - 1 (a prefill position
	// never exceeds D), a chunk of n tokens ending at D costs
	//   n x TokenMs + PerPositionMs x (n x (D - n + 1) + n x (n - 1) / 2).
	// The chunk-1 pair prices only chunks below kTiledMinTokens, so it is bounded at min(P, 7)
	// tokens; the batched pair is bounded at P. Configure() refuses when either bound is not finite
	// or implies K > kMaxJobTicks. (A chunk's price rises with its size within one pair, so the bound
	// at the largest size each pair can price covers every chunk that pair prices.)
	struct FPrefillBound
	{
		double Ms = 0.0;
		bool bFinite = true;
	};

	inline FPrefillBound PrefillPriceBoundMs(double Tokens, double D, const FPromptPrice& Price)
	{
		FPrefillBound Out;
		auto Mul = [&Out](double A, double B)
		{
			const double R = A * B;
			Out.bFinite = Out.bFinite && FMath::IsFinite(R);
			return R;
		};
		auto Add = [&Out](double A, double B)
		{
			const double R = A + B;
			Out.bFinite = Out.bFinite && FMath::IsFinite(R);
			return R;
		};
		const double Positions = Add(Mul(Tokens, D - Tokens + 1.0), Mul(Tokens, Tokens - 1.0) / 2.0);
		Out.Ms = Add(Mul(Tokens, Price.TokenMs), Mul(Price.PerPositionMs, Positions));
		return Out;
	}

	// The chunk-1 pair's bound: min(P, kTiledMinTokens - 1) tokens ending at D.
	inline FPrefillBound Chunk1PrefillBoundMs(int64 P, int64 D, const FPromptPrice& Chunk1)
	{
		return PrefillPriceBoundMs(static_cast<double>(FMath::Min<int64>(P, kTiledMinTokens - 1)), static_cast<double>(D), Chunk1);
	}

	// The batched pair's bound: P tokens ending at D.
	inline FPrefillBound BatchedPrefillBoundMs(int64 P, int64 D, const FPromptPrice& Batched)
	{
		return PrefillPriceBoundMs(static_cast<double>(P), static_cast<double>(D), Batched);
	}

	// Item 3: the batched calibration arm's shape. The arm times one sslm_prefill of kBatchedArmChunk
	// tokens at chunk budget kBatchedArmChunk from each of the depths 1, C/4, C/2, 3C/4 and
	// C - kBatchedArmChunk, which ascend and are distinct only from a cap of kBatchedArmMinCap up
	// (C - 8 > 3C/4 needs C > 32). Below it the arm is a named skip, and the estimate suggests the
	// measured mode's one-time batched pair (D-SLM8141).
	constexpr int32 kBatchedArmChunk = kTiledMinTokens;
	constexpr int64 kBatchedArmMinCap = 33;

	inline bool BatchedArmDepths(int64 Cap, int64 (&OutDepths)[5])
	{
		if (Cap < kBatchedArmMinCap)
		{
			return false;
		}
		OutDepths[0] = 1;
		OutDepths[1] = Cap / 4;
		OutDepths[2] = Cap / 2;
		OutDepths[3] = (3 * Cap) / 4;
		OutDepths[4] = Cap - kBatchedArmChunk;
		return true;
	}

	// One batched reading: a call of kBatchedArmChunk tokens from depth d serves positions
	// d .. d + 7, so it is recorded at their mean, d + 3.5, with the per-token time call / 8. The
	// position is a double: the chunk-1 arm's int64 Depth cannot hold the half.
	inline double BatchedReadingPosition(int64 Depth)
	{
		return static_cast<double>(Depth) + 0.5 * static_cast<double>(kBatchedArmChunk - 1);
	}

	inline double BatchedReadingPerTokenMs(double CallMs)
	{
		return CallMs / static_cast<double>(kBatchedArmChunk);
	}

	// The batched arm's shipped line: the upper envelope of its readings, as the chunk-1 arm's
	// fit defines it, with one difference the half-position forces. Slope = (last median -
	// first median) / (the position span), floored at 0. The chord through the first reading is
	// extended back to position 0 (the batched readings start at 4.5, not at depth 1), and the
	// intercept is raised by the largest amount any reading sits above that line, so no reading lies
	// above the shipped line and the intercept is the price at position 0, as PromptTokenCostMs is.
	// Positions must ascend; N >= 1. The chunk-1 arm keeps its own fit (SuperSLMEstimateCostsCommand
	// .cpp FitUpperEnvelope, intercept from the depth-1 median), unchanged by slice 1P.
	inline void FitBatchedUpperEnvelope(const double* Positions, const double* Medians, int32 N, double& OutIntercept, double& OutSlope)
	{
		const double Span = Positions[N - 1] - Positions[0];
		OutSlope = Span > 0.0 ? FMath::Max(0.0, (Medians[N - 1] - Medians[0]) / Span) : 0.0;
		// The line through the first reading, extended back to position 0.
		OutIntercept = Medians[0] - OutSlope * Positions[0];
		double Raise = 0.0;
		for (int32 I = 0; I < N; ++I)
		{
			Raise = FMath::Max(Raise, Medians[I] - (OutIntercept + OutSlope * Positions[I]));
		}
		OutIntercept += Raise;
	}

	// Item 2's premise, checked by the arm (default 2; 9.P-8): the chunk-1 pair is an upper bound for
	// a token inside a batched call only if the batched per-token intercept does not exceed the
	// chunk-1 one. The intercepts are compared, never the slopes.
	inline bool BatchedInterceptWithinChunk1(double BatchedTokenMs, double Chunk1TokenMs)
	{
		return BatchedTokenMs <= Chunk1TokenMs;
	}
}
