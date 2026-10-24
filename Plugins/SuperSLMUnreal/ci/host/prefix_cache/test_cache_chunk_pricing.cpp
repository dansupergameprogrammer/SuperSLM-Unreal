// Precache plan rev 1.18.11 (§15.31): the cache chunk's price. Core/SuperSLMCacheChunkPricing.h: the prefix_prefill
// pair prices a cache chunk of 8 tokens or more when present, and the batched pair when it is absent (the fallback);
// where the pair comes from at Configure(); and the estimate's fit of the prefix_prefill arm's readings.
//
// Written red first against a stubbed header (no pair ever present: CacheChunkPair always the batched pair,
// ResolvePrefixPrefillPrice always the fallback, FitPrefixPrefillPair always false). Mutants:
//   - the pair ignored (always batched)            -> cache_chunk_pricing_the_prefix_pair_prices_a_floor_chunk
//   - the pair used when absent                    -> cache_chunk_pricing_falls_back_to_the_batched_pair
//   - a saved pair winning over the caller's       -> cache_chunk_pricing_the_caller_pair_wins
//   - one caller field accepted alone              -> cache_chunk_pricing_one_caller_field_is_refused
//   - a fit under the group medians                -> cache_chunk_pricing_the_fit_is_an_upper_envelope

#include "harness.h"
#include "SuperSLMCacheChunkPricing.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace SuperSLMCore;

namespace
{
	FCacheChunkPrices Prices(bool bPrefix)
	{
		FCacheChunkPrices P;
		P.Chunk1 = FChunkPricePair{ 33.583, 0.004547 };
		P.Batched = FChunkPricePair{ 15.023, 0.004791 };
		P.bHasPrefixPrefill = bPrefix;
		P.PrefixPrefill = FChunkPricePair{ 25.0, 0.006 };
		return P;
	}

	bool Near(double A, double B)
	{
		return std::fabs(A - B) <= 1e-9 * (std::fabs(B) > 1.0 ? std::fabs(B) : 1.0);
	}

	// A prefix_prefill arm's readings: 8-token chunks from depth 0 to Depth, each costing the pair's price, with
	// an optional deterministic spread.
	std::vector<FPrefixPrefillReading> ArmReadings(FChunkPricePair Pair, int64_t Depth, double Spread)
	{
		std::vector<FPrefixPrefillReading> Out;
		int32_t I = 0;
		for (int64_t D = 0; D + 8 <= Depth; D += 8, ++I)
		{
			const double Wobble = Spread * static_cast<double>((I * 37) % 11 - 5) / 5.0;
			Out.push_back(FPrefixPrefillReading{ D, 8, ChunkPairPriceMs(8, D, Pair) * (1.0 + Wobble) });
		}
		return Out;
	}
}

HOST_TEST(cache_chunk_pricing_the_prefix_pair_prices_a_floor_chunk, "5.9 (d)")
{
	const FCacheChunkPrices P = Prices(true);
	EXPECT_TRUE(Near(CacheChunkPair(8, P).TokenMs, 25.0));
	EXPECT_TRUE(Near(CacheChunkPair(64, P).PerPositionMs, 0.006));
	EXPECT_TRUE(Near(CacheChunkUpperPair(P).TokenMs, 25.0));
	EXPECT_TRUE(Near(ChunkPairPriceMs(8, 1000, CacheChunkPair(8, P)), 8 * 25.0 + 0.006 * (8.0 * 1000 + 28)));
}

HOST_TEST(cache_chunk_pricing_falls_back_to_the_batched_pair, "5.9 (d)")
{
	const FCacheChunkPrices P = Prices(false);
	EXPECT_TRUE(Near(CacheChunkPair(8, P).TokenMs, 15.023));
	EXPECT_TRUE(Near(CacheChunkPair(2000, P).PerPositionMs, 0.004791));
	EXPECT_TRUE(Near(CacheChunkUpperPair(P).TokenMs, 15.023));
}

HOST_TEST(cache_chunk_pricing_below_8_tokens_keeps_the_chunk1_pair, "5.9 (d)")
{
	EXPECT_TRUE(Near(CacheChunkPair(7, Prices(true)).TokenMs, 33.583));
	EXPECT_TRUE(Near(CacheChunkPair(1, Prices(false)).TokenMs, 33.583));
}

HOST_TEST(cache_chunk_pricing_resolves_none_to_the_fallback, "5.9 (d)")
{
	FPrefixPrefillPriceInputs In;
	FPrefixPrefillPrice R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::BatchedFallback));
	EXPECT_TRUE(!R.bCallerError && !R.bSavedIgnored);
	// A saved profile that is not used (another artifact, mode or task count) gives no pair either.
	In.bSavedTokenSet = In.bSavedSlopeSet = true;
	In.Saved = FChunkPricePair{ 25.0, 0.006 };
	R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::BatchedFallback));
}

HOST_TEST(cache_chunk_pricing_a_used_saved_pair_prices, "5.9 (d)")
{
	FPrefixPrefillPriceInputs In;
	In.bSavedUsed = In.bSavedTokenSet = In.bSavedSlopeSet = true;
	In.Saved = FChunkPricePair{ 25.0, 0.006 };
	const FPrefixPrefillPrice R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::SavedEstimate));
	EXPECT_TRUE(Near(R.Pair.TokenMs, 25.0));
}

HOST_TEST(cache_chunk_pricing_a_partial_or_bad_saved_pair_falls_back, "5.9 (d)")
{
	FPrefixPrefillPriceInputs In;
	In.bSavedUsed = In.bSavedTokenSet = true; // the slope key missing
	In.Saved = FChunkPricePair{ 25.0, 0.006 };
	FPrefixPrefillPrice R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::BatchedFallback));
	EXPECT_TRUE(R.bSavedIgnored);
	In.bSavedSlopeSet = true;
	In.Saved = FChunkPricePair{ 0.0, 0.006 }; // not a positive cost
	R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::BatchedFallback));
	EXPECT_TRUE(R.bSavedIgnored);
	In.Saved = FChunkPricePair{ 25.0, std::nan("") };
	R = ResolvePrefixPrefillPrice(In);
	EXPECT_TRUE(R.bSavedIgnored);
}

HOST_TEST(cache_chunk_pricing_the_caller_pair_wins, "5.9 (d)")
{
	FPrefixPrefillPriceInputs In;
	In.bCallerTokenSet = In.bCallerSlopeSet = true;
	In.Caller = FChunkPricePair{ 30.0, 0.0 };
	In.bSavedUsed = In.bSavedTokenSet = In.bSavedSlopeSet = true;
	In.Saved = FChunkPricePair{ 25.0, 0.006 };
	const FPrefixPrefillPrice R = ResolvePrefixPrefillPrice(In);
	EXPECT_EQ(static_cast<int>(R.Source), static_cast<int>(EPrefixPrefillPriceSource::Caller));
	EXPECT_TRUE(Near(R.Pair.TokenMs, 30.0));
	EXPECT_TRUE(!R.bCallerError);
}

HOST_TEST(cache_chunk_pricing_one_caller_field_is_refused, "5.9 (d)")
{
	FPrefixPrefillPriceInputs In;
	In.bCallerTokenSet = true;
	In.Caller = FChunkPricePair{ 30.0, 0.0 };
	EXPECT_TRUE(ResolvePrefixPrefillPrice(In).bCallerError);
	In.bCallerTokenSet = false;
	In.bCallerSlopeSet = true;
	EXPECT_TRUE(ResolvePrefixPrefillPrice(In).bCallerError);
	In.bCallerTokenSet = true;
	In.Caller = FChunkPricePair{ 30.0, -0.001 }; // a negative slope
	EXPECT_TRUE(ResolvePrefixPrefillPrice(In).bCallerError);
}

HOST_TEST(cache_chunk_pricing_the_fit_recovers_an_exact_pair, "5.9 (d)")
{
	const FChunkPricePair Truth{ 25.0, 0.006 };
	const std::vector<FPrefixPrefillReading> R = ArmReadings(Truth, kPrefixPrefillArmMaxDepth, 0.0);
	FChunkPricePair Fit;
	REQUIRE_TRUE(FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit));
	EXPECT_TRUE(std::fabs(Fit.TokenMs - Truth.TokenMs) < 1e-9);
	EXPECT_TRUE(std::fabs(Fit.PerPositionMs - Truth.PerPositionMs) < 1e-12);
}

HOST_TEST(cache_chunk_pricing_the_fit_is_an_upper_envelope, "5.9 (d)")
{
	// With a spread, the line is on or above every group's median, so at least half the chunks of every group are
	// priced at or above their time: the median chunk does not overrun a price from this fit.
	const std::vector<FPrefixPrefillReading> R = ArmReadings(FChunkPricePair{ 25.0, 0.006 }, kPrefixPrefillArmMaxDepth, 0.15);
	FChunkPricePair Fit;
	REQUIRE_TRUE(FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit));
	size_t Under = 0;
	for (const FPrefixPrefillReading& X : R)
	{
		Under += ChunkPairPriceMs(X.Tokens, X.StartDepth, Fit) >= X.Ms ? 1 : 0;
	}
	EXPECT_TRUE(Under * 2 >= R.size());
}

HOST_TEST(cache_chunk_pricing_the_fit_refuses_what_it_cannot_price, "5.9 (d)")
{
	FChunkPricePair Fit;
	std::vector<FPrefixPrefillReading> R = ArmReadings(FChunkPricePair{ 25.0, 0.006 }, 40, 0.0); // 5 readings
	EXPECT_TRUE(!FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit)); // fewer than the groups
	R = ArmReadings(FChunkPricePair{ 25.0, 0.006 }, 512, 0.0);
	R[3].Ms = std::nan("");
	EXPECT_TRUE(!FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit));
	R[3].Ms = 0.0;
	EXPECT_TRUE(!FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit));
	EXPECT_TRUE(!FitPrefixPrefillPair(nullptr, 0, kPrefixPrefillFitGroups, Fit));
}

HOST_TEST(cache_chunk_pricing_a_falling_arm_fits_a_flat_slope, "5.9 (d)")
{
	// A slope below 0 is floored: the price never falls with depth.
	std::vector<FPrefixPrefillReading> R;
	for (int64_t D = 0; D < 512; D += 8)
	{
		R.push_back(FPrefixPrefillReading{ D, 8, 200.0 - static_cast<double>(D) * 0.01 });
	}
	FChunkPricePair Fit;
	REQUIRE_TRUE(FitPrefixPrefillPair(R.data(), R.size(), kPrefixPrefillFitGroups, Fit));
	EXPECT_TRUE(Fit.PerPositionMs == 0.0);
	// Flat at the highest group median: the first group's (its median chunk starts at depth 28, 199.72 ms / 8).
	EXPECT_TRUE(Fit.TokenMs >= (200.0 - 28.0 * 0.01) / 8.0 - 1e-9);
	EXPECT_TRUE(Fit.TokenMs <= 200.0 / 8.0);
}
