// Cell P1.1 (plan §6 P1 "Pricing (blocking)", §16 Q8 A; mutant X15): a chunk of C = 1 is priced with
// the chunk-1 pair, C = 2..7 with the per-C table, C >= 8 with the batched pair, each with the
// per-position term at depth d; prompt chunks of 2..7 are re-priced too; Configure()'s bound uses the
// table up to C = 7.
//
// This translation unit compiles the plugin's own SuperSLMPrefillPlanning.h through shim/CoreMinimal.h,
// so the cells read the text the scheduler runs. The price oracle is the plan's formula, written out
// here.

#include "CoreMinimal.h"
#include "SuperSLMPrefillPlanning.h"

#include "harness.h"

#include <cmath>

namespace P = SuperSLMPrefillPlanning;

namespace
{
	double Oracle(int32 C, int64 Depth, const P::FPromptPrice& Pair)
	{
		const double Positions = static_cast<double>(C) * static_cast<double>(Depth) + static_cast<double>(C) * (C - 1) / 2.0;
		return C * Pair.TokenMs + Pair.PerPositionMs * Positions;
	}

	bool Near(double A, double B) { return std::fabs(A - B) <= 1e-9 * (1.0 + std::fabs(B)); }

	// The shipped Zen 2 mode-0 pairs (SuperSLMRuntimeConfig.h) and a per-C table whose entries differ
	// from both, so every selection is visible in the price. The per-token values are the box's T(M) / M
	// at one thread (plan §8.3, cell Z-box), so the price rises with C as the planner's scan assumes.
	const P::FPromptPrice kChunk1{33.583, 0.004547};
	const P::FPromptPrice kBatched{15.023, 0.004791};

	P::FPerChunkPromptPrices Table()
	{
		P::FPerChunkPromptPrices T;
		for (int32 C = 0; C <= P::kTiledMinTokens; ++C)
		{
			T.ByChunk[C] = P::FPromptPrice{-1000.0, -1.0}; // unused entries: any read of them shows
		}
		const double TokenMs[] = {0, 0, 25.15, 22.85, 22.23, 21.71, 21.42, 21.30};
		for (int32 C = 2; C <= 7; ++C)
		{
			T.ByChunk[C] = P::FPromptPrice{TokenMs[C], 0.0050 + 0.0001 * C};
		}
		return T;
	}

	P::FPerChunkPromptPrices TableEqualToChunk1()
	{
		P::FPerChunkPromptPrices T;
		for (int32 C = 0; C <= P::kTiledMinTokens; ++C)
		{
			T.ByChunk[C] = kChunk1;
		}
		return T;
	}
}

HOST_TEST(pricing_selects_chunk1_table_or_batched_by_size, "P1.1; §16 Q8 A")
{
	const P::FPerChunkPromptPrices T = Table();
	EXPECT_TRUE(&P::SelectPromptPriceWithTable(1, kChunk1, T, kBatched) == &kChunk1);
	for (int32 C = 2; C <= 7; ++C)
	{
		EXPECT_TRUE(&P::SelectPromptPriceWithTable(C, kChunk1, T, kBatched) == &T.ByChunk[C]);
	}
	const int32 Big[] = {8, 9, 64, 512};
	for (int32 C : Big)
	{
		EXPECT_TRUE(&P::SelectPromptPriceWithTable(C, kChunk1, T, kBatched) == &kBatched);
	}
}

HOST_TEST(pricing_each_chunk_size_with_its_depth_term, "P1.1: each with the per-position term at depth d")
{
	const P::FPerChunkPromptPrices T = Table();
	const int64 Depths[] = {0, 1, 41, 100, 4000};
	for (int64 D : Depths)
	{
		EXPECT_TRUE(Near(P::PrefillChunkCostMsWithTable(1, D, kChunk1, T, kBatched), Oracle(1, D, kChunk1)));
		for (int32 C = 2; C <= 7; ++C)
		{
			EXPECT_TRUE(Near(P::PrefillChunkCostMsWithTable(C, D, kChunk1, T, kBatched), Oracle(C, D, T.ByChunk[C])));
		}
		const int32 Big[] = {8, 9, 64};
		for (int32 C : Big)
		{
			EXPECT_TRUE(Near(P::PrefillChunkCostMsWithTable(C, D, kChunk1, T, kBatched), Oracle(C, D, kBatched)));
		}
	}
}

HOST_TEST(pricing_a_six_token_span_is_not_six_chunk1_tokens, "P1.1; mutant X15 (spans priced at C x 33.583)")
{
	const P::FPerChunkPromptPrices T = Table();
	const double Six = P::PrefillChunkCostMsWithTable(6, 0, kChunk1, T, kBatched);
	EXPECT_TRUE(Near(Six, 6 * 21.42 + T.ByChunk[6].PerPositionMs * 15.0));
	EXPECT_LT(Six, 150.0);
	EXPECT_TRUE(!Near(Six, Oracle(6, 0, kChunk1))); // ~201.5 ms, the price today
	for (int32 C = 2; C <= 7; ++C)
	{
		EXPECT_TRUE(!Near(P::PrefillChunkCostMsWithTable(C, 41, kChunk1, T, kBatched), Oracle(C, 41, kChunk1)));
	}
}

HOST_TEST(pricing_with_a_chunk1_table_equals_todays_price, "P1.1 (the table changes only C = 2..7, and only by its values)")
{
	const P::FPerChunkPromptPrices T = TableEqualToChunk1();
	const int64 Depths[] = {0, 7, 300};
	for (int64 D : Depths)
	{
		for (int32 C = 1; C <= 70; ++C)
		{
			EXPECT_TRUE(Near(P::PrefillChunkCostMsWithTable(C, D, kChunk1, T, kBatched), P::PrefillChunkCostMs(C, D, kChunk1, kBatched)));
		}
	}
}

HOST_TEST(pricing_prompt_chunks_of_two_to_seven_are_repriced, "P1.1: prompt chunks of 2..7 are re-priced too (§16 Q8)")
{
	const P::FPerChunkPromptPrices T = Table();
	// A 100 ms share fits 2 chunk-1 tokens (67.2 ms) but not 3 (100.7 ms). With the table, 4 tokens
	// cost ~88.9 ms and 5 cost ~108.6 ms at depth 0, so the planner reaches 4.
	EXPECT_EQ(P::PlanPrefillChunk(7, 0, 100.0, 1, kChunk1, kBatched), 2);
	EXPECT_EQ(P::PlanPrefillChunkWithTable(7, 0, 100.0, 1, kChunk1, T, kBatched), 4);
	// The MinPrefillChunk floor still applies.
	EXPECT_EQ(P::PlanPrefillChunkWithTable(20, 0, 1.0, 8, kChunk1, T, kBatched), 8);
	// With the table equal to the chunk-1 pair, the planner is today's planner exactly.
	const P::FPerChunkPromptPrices Same = TableEqualToChunk1();
	const double Shares[] = {10.0, 40.0, 70.0, 101.0, 150.0, 400.0};
	const int32 Mins[] = {1, 4, 8};
	for (double Share : Shares)
	{
		for (int32 Min : Mins)
		{
			for (int32 MaxChunk = 1; MaxChunk <= 20; ++MaxChunk)
			{
				EXPECT_EQ(P::PlanPrefillChunkWithTable(MaxChunk, 37, Share, Min, kChunk1, Same, kBatched),
					P::PlanPrefillChunk(MaxChunk, 37, Share, Min, kChunk1, kBatched));
			}
		}
	}
}

HOST_TEST(pricing_bound_uses_the_table_up_to_seven, "P1.1: PrefillPriceBoundMs uses the table up to C = 7")
{
	P::FPerChunkPromptPrices T = Table();
	T.ByChunk[5] = P::FPromptPrice{40.0, 0.02}; // the largest bound at the cap's end
	const int64 D = 4095;
	double Want = 0.0;
	for (int32 C = 1; C <= 7; ++C)
	{
		const P::FPromptPrice& Pair = C == 1 ? kChunk1 : T.ByChunk[C];
		const P::FPrefillBound B = P::PrefillPriceBoundMs(static_cast<double>(C), static_cast<double>(D), Pair);
		Want = B.Ms > Want ? B.Ms : Want;
	}
	const P::FPrefillBound Got = P::SmallChunkPrefillBoundMsWithTable(4096, D, kChunk1, T);
	EXPECT_TRUE(Got.bFinite);
	EXPECT_TRUE(Near(Got.Ms, Want));
	EXPECT_TRUE(Near(Got.Ms, P::PrefillPriceBoundMs(5.0, static_cast<double>(D), T.ByChunk[5]).Ms));

	// P = 3: only C = 1..3 can be priced, so entry 5 does not count.
	double Want3 = 0.0;
	for (int32 C = 1; C <= 3; ++C)
	{
		const P::FPromptPrice& Pair = C == 1 ? kChunk1 : T.ByChunk[C];
		const P::FPrefillBound B = P::PrefillPriceBoundMs(static_cast<double>(C), static_cast<double>(D), Pair);
		Want3 = B.Ms > Want3 ? B.Ms : Want3;
	}
	EXPECT_TRUE(Near(P::SmallChunkPrefillBoundMsWithTable(3, D, kChunk1, T).Ms, Want3));
}

HOST_TEST(pricing_bound_is_not_finite_when_a_reachable_entry_is_not, "P1.1 bound; the scheduler's numeric domain (Configure() refuses a non-finite bound)")
{
	P::FPerChunkPromptPrices T = Table();
	T.ByChunk[4] = P::FPromptPrice{INFINITY, 0.0};
	EXPECT_TRUE(!P::SmallChunkPrefillBoundMsWithTable(4096, 4095, kChunk1, T).bFinite);
	EXPECT_TRUE(!P::SmallChunkPrefillBoundMsWithTable(4, 4095, kChunk1, T).bFinite);
	// Unreachable at P = 3.
	const P::FPrefillBound Three = P::SmallChunkPrefillBoundMsWithTable(3, 4095, kChunk1, T);
	EXPECT_TRUE(Three.bFinite);
	EXPECT_GT(Three.Ms, 0.0);
}
