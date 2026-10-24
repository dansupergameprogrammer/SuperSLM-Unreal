#pragma once

// The cache chunk's price (precache plan rev 1.18.11, §15.31; cell 5.9 (d)). Plain C++, no engine and no Unreal
// types, so the host suite tests it (ci/host/prefix_cache, test_cache_chunk_pricing.cpp).
//
// A sliced CPU cache build's chunk is one sslm_prefix_prefill call of the floor chunk (MinPrefillChunk, 8 by
// default), from depth 0 up. Until rev 1.18.11 it was priced with the batched prompt pair, which
// SuperSLM.EstimateCosts measures on sslm_prefill of a sequence at depths spread across the cap: not the call it
// prices. The box read about 200 ms per 8-token chunk against a price of about 120-200 ms.
//
// So the estimate gains a prefix_prefill arm. It builds one prefix from depth 0 in floor-sized
// sslm_prefix_prefill chunks up to kPrefixPrefillArmMaxDepth and times each chunk (FPrefixPrefillReading). The
// readings are fitted to a price pair of the prompt price's form (FitPrefixPrefillPair, below):
//   a chunk of C tokens from depth d costs  C x TokenMs + PerPositionMs x (C x d + C x (C - 1) / 2).
// The pair is written to CalibratedCosts.ini under two optional keys (PrefixPrefillTokenCostMs and
// PrefixPrefillTokenCostPerPositionMs), as RestoreCostPerMiBMs is, and a caller may set it on the configuration.
// The cache chunk's planner and price read it for chunks of kTiledMinTokens or more in place of the batched pair;
// when it is absent they fall back to the batched pair (CacheChunkPair). Chunks under kTiledMinTokens keep the
// chunk-1 pair. CreatePrefix's prefill is not a cache chunk and keeps the batched pair.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SuperSLMCore
{
	// The engine's tiled-GEMM threshold, where the batched pair starts (SuperSLMPrefillPlanning::kTiledMinTokens).
	constexpr int32_t kCacheChunkTiledMinTokens = 8;

	// The prefix_prefill arm: floor-sized chunks from depth 0 up to this depth (or the artifact's cap less one
	// chunk, if that is less), grouped into kPrefixPrefillFitGroups consecutive groups for the fit.
	constexpr int64_t kPrefixPrefillArmMaxDepth = 2048;
	constexpr int32_t kPrefixPrefillFitGroups = 8;

	struct FChunkPricePair
	{
		double TokenMs = 0.0;
		double PerPositionMs = 0.0;
	};

	// A pair a chunk may be priced with: a finite, positive per-token cost and a finite, non-negative slope.
	inline bool IsValidChunkPricePair(const FChunkPricePair& Pair)
	{
		return std::isfinite(Pair.TokenMs) && Pair.TokenMs > 0.0 && std::isfinite(Pair.PerPositionMs) && Pair.PerPositionMs >= 0.0;
	}

	// The planned cost of Chunk tokens from StartDepth with one pair.
	inline double ChunkPairPriceMs(int32_t Chunk, int64_t StartDepth, const FChunkPricePair& Pair)
	{
		const int64_t PositionSum = static_cast<int64_t>(Chunk) * StartDepth + static_cast<int64_t>(Chunk) * (Chunk - 1) / 2;
		return Chunk * Pair.TokenMs + Pair.PerPositionMs * static_cast<double>(PositionSum);
	}

	// The prices a cache chunk can be planned with.
	struct FCacheChunkPrices
	{
		FChunkPricePair Chunk1;
		FChunkPricePair Batched;
		bool bHasPrefixPrefill = false; // the calibrated (or the caller's) prefix_prefill pair is present
		FChunkPricePair PrefixPrefill;
	};

	// The pair a cache chunk of Chunk tokens is priced with: the chunk-1 pair below kCacheChunkTiledMinTokens; at or
	// above it the prefix_prefill pair when present, else the batched pair (the fallback).
	inline const FChunkPricePair& CacheChunkPair(int32_t Chunk, const FCacheChunkPrices& Prices)
	{
		if (Chunk < kCacheChunkTiledMinTokens)
		{
			return Prices.Chunk1;
		}
		return Prices.bHasPrefixPrefill ? Prices.PrefixPrefill : Prices.Batched;
	}

	// The pair the cache's planner uses in place of the batched one (the chunk-1 pair is unchanged).
	inline const FChunkPricePair& CacheChunkUpperPair(const FCacheChunkPrices& Prices)
	{
		return Prices.bHasPrefixPrefill ? Prices.PrefixPrefill : Prices.Batched;
	}

	// Where the prefix_prefill pair comes from at Configure(): the caller's (both fields set), else a used saved
	// estimate's (both keys present and valid), else none (the batched fallback). One caller field without the
	// other, or a caller pair out of the domain, is the caller's error: Configure() refuses it (OutCallerError).
	// A saved pair with one key, or out of the domain, is not used (OutSavedIgnored), and the batched pair prices.
	enum class EPrefixPrefillPriceSource : uint8_t
	{
		BatchedFallback,
		Caller,
		SavedEstimate,
	};

	struct FPrefixPrefillPriceInputs
	{
		bool bCallerTokenSet = false;
		bool bCallerSlopeSet = false;
		FChunkPricePair Caller;
		bool bSavedUsed = false;      // the saved profile is used at all (its artifact, mode and tasks match)
		bool bSavedTokenSet = false;
		bool bSavedSlopeSet = false;
		FChunkPricePair Saved;
	};

	struct FPrefixPrefillPrice
	{
		EPrefixPrefillPriceSource Source = EPrefixPrefillPriceSource::BatchedFallback;
		FChunkPricePair Pair;
		bool bCallerError = false;
		bool bSavedIgnored = false;
	};

	inline FPrefixPrefillPrice ResolvePrefixPrefillPrice(const FPrefixPrefillPriceInputs& In)
	{
		FPrefixPrefillPrice Out;
		if (In.bCallerTokenSet || In.bCallerSlopeSet)
		{
			if (!In.bCallerTokenSet || !In.bCallerSlopeSet || !IsValidChunkPricePair(In.Caller))
			{
				Out.bCallerError = true;
				return Out;
			}
			Out.Source = EPrefixPrefillPriceSource::Caller;
			Out.Pair = In.Caller;
			return Out;
		}
		if (In.bSavedUsed && (In.bSavedTokenSet || In.bSavedSlopeSet))
		{
			if (In.bSavedTokenSet && In.bSavedSlopeSet && IsValidChunkPricePair(In.Saved))
			{
				Out.Source = EPrefixPrefillPriceSource::SavedEstimate;
				Out.Pair = In.Saved;
				return Out;
			}
			Out.bSavedIgnored = true;
		}
		return Out;
	}

	// One timed chunk of the prefix_prefill arm.
	struct FPrefixPrefillReading
	{
		int64_t StartDepth = 0;
		int32_t Tokens = 0;
		double Ms = 0.0;
	};

	// The arm's pair: the readings, in depth order, are cut into Groups consecutive groups; each group gives the
	// median position of its tokens (d + (C - 1) / 2) and the median ms per token. The pair is the upper envelope of
	// the group medians: the slope of the chord from the first group to the last (floored at 0), and the smallest
	// intercept that puts every group median on or under the line. A chunk priced with it is then priced at or
	// above the median chunk of its depth range, which is what 5.9 (d)'s grade reads. False when there are fewer
	// readings than groups, fewer than 2 groups, or any reading is not a finite positive time of at least one token;
	// the estimate then writes no pair and the batched pair keeps pricing.
	inline bool FitPrefixPrefillPair(const FPrefixPrefillReading* Readings, size_t Count, int32_t Groups, FChunkPricePair& Out)
	{
		Out = FChunkPricePair{};
		if (Readings == nullptr || Groups < 2 || Count < static_cast<size_t>(Groups))
		{
			return false;
		}
		for (size_t I = 0; I < Count; ++I)
		{
			if (Readings[I].Tokens <= 0 || !std::isfinite(Readings[I].Ms) || !(Readings[I].Ms > 0.0) || Readings[I].StartDepth < 0)
			{
				return false;
			}
		}
		auto Median = [](std::vector<double> V)
		{
			std::sort(V.begin(), V.end());
			const size_t N = V.size();
			return N % 2 == 1 ? V[N / 2] : 0.5 * (V[N / 2 - 1] + V[N / 2]);
		};
		std::vector<double> X, Y;
		for (int32_t G = 0; G < Groups; ++G)
		{
			const size_t Begin = Count * static_cast<size_t>(G) / static_cast<size_t>(Groups);
			const size_t End = Count * static_cast<size_t>(G + 1) / static_cast<size_t>(Groups);
			std::vector<double> Positions, PerToken;
			for (size_t I = Begin; I < End; ++I)
			{
				const FPrefixPrefillReading& R = Readings[I];
				Positions.push_back(static_cast<double>(R.StartDepth) + 0.5 * static_cast<double>(R.Tokens - 1));
				PerToken.push_back(R.Ms / static_cast<double>(R.Tokens));
			}
			X.push_back(Median(Positions));
			Y.push_back(Median(PerToken));
		}
		const double Span = X.back() - X.front();
		double Slope = Span > 0.0 ? (Y.back() - Y.front()) / Span : 0.0;
		if (!std::isfinite(Slope) || Slope < 0.0)
		{
			Slope = 0.0;
		}
		double Intercept = Y.front() - Slope * X.front();
		for (size_t G = 0; G < X.size(); ++G)
		{
			Intercept = std::max(Intercept, Y[G] - Slope * X[G]);
		}
		Out.TokenMs = Intercept;
		Out.PerPositionMs = Slope;
		return IsValidChunkPricePair(Out);
	}
}
