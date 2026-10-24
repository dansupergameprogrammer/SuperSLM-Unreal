// Cell P1.1's price source (plan rev 1.3.6, §6 P1 "Where the prices come from", review finding 2;
// mutant XD9): the C = 2..7 calibration arm's fit, SuperSLMPrefillPlanning::FitSmallChunkArm. B5f runs
// this arm and stores its result in the resolved configuration's per-C field (the Unreal cell
// SuperSLM.SchemaJump.P1.PriceSource covers the field and the scheduler). A table that can only ever
// hold the chunk-1 price is X15; an arm that cannot recover a known price is XD9.
//
// Compiles the plugin's own SuperSLMPrefillPlanning.h through shim/CoreMinimal.h, as test_pricing.cpp
// does. The oracle is the plan's price form, written out here.

#include "CoreMinimal.h"
#include "SuperSLMPrefillPlanning.h"

#include "harness.h"

#include <cmath>
#include <limits>

namespace P = SuperSLMPrefillPlanning;

namespace
{
	double OracleMs(int32 C, int64 Depth, const P::FPromptPrice& Pair)
	{
		return C * Pair.TokenMs + Pair.PerPositionMs * (static_cast<double>(C) * static_cast<double>(Depth) + static_cast<double>(C) * (C - 1) / 2.0);
	}

	bool Near(double A, double B) { return std::fabs(A - B) <= 1e-9 * (1.0 + std::fabs(B)); }

	// A distinct pair per C, none equal to the chunk-1 pair, the price falling per token as C grows.
	P::FPromptPrice PairFor(int32 C) { return P::FPromptPrice{26.0 - 0.75 * C, 0.0045 + 0.0001 * C}; }
	const P::FPromptPrice kChunk1{33.583, 0.004547};
	const int64 kDepths[] = {1, 1024, 2048, 3072, 4088};
}

HOST_TEST(pricing_small_chunk_arm_recovers_a_known_pair_for_each_c, "P1.1 rev 1.3.6 price source: the C = 2..7 arms; mutant XD9")
{
	for (int32 C = 2; C <= 7; ++C)
	{
		const P::FPromptPrice Want = PairFor(C);
		double Medians[5];
		for (int32 I = 0; I < 5; ++I) { Medians[I] = OracleMs(C, kDepths[I], Want); }
		P::FPromptPrice Got{-1.0, -1.0};
		EXPECT_TRUE(P::FitSmallChunkArm(C, kDepths, Medians, 5, Got));
		EXPECT_TRUE(Near(Got.TokenMs, Want.TokenMs));
		EXPECT_TRUE(Near(Got.PerPositionMs, Want.PerPositionMs));
		EXPECT_TRUE(!Near(Got.TokenMs, kChunk1.TokenMs)); // not X15's chunk-1 figure
		// The recovered pair prices a chunk of C exactly as the readings did.
		for (int32 I = 0; I < 5; ++I) { EXPECT_TRUE(Near(P::PromptPriceMs(C, kDepths[I], Got), Medians[I])); }
	}
}

HOST_TEST(pricing_small_chunk_arm_ships_the_upper_envelope, "P1.1 rev 1.3.6: the arm's pair, like the chunk-1 and batched arms', has no reading above it")
{
	for (int32 C = 2; C <= 7; ++C)
	{
		const P::FPromptPrice Base = PairFor(C);
		double Medians[5];
		const double Noise[] = {0.0, 0.9, -0.4, 1.7, 0.2}; // ms per call, some above the line
		for (int32 I = 0; I < 5; ++I) { Medians[I] = OracleMs(C, kDepths[I], Base) + Noise[I]; }
		P::FPromptPrice Got{-1.0, -1.0};
		REQUIRE_TRUE(P::FitSmallChunkArm(C, kDepths, Medians, 5, Got));
		EXPECT_GE(Got.PerPositionMs, 0.0);
		for (int32 I = 0; I < 5; ++I)
		{
			const double Position = static_cast<double>(kDepths[I]) + 0.5 * (C - 1);
			const double PerToken = Medians[I] / C;
			EXPECT_LE(PerToken, Got.TokenMs + Got.PerPositionMs * Position + 1e-9);
		}
	}
}

HOST_TEST(pricing_small_chunk_arm_refuses_what_it_cannot_fit, "P1.1 rev 1.3.6: the arm's domain (C in 2..7; ascending depths; finite, positive readings)")
{
	double Medians[5];
	for (int32 I = 0; I < 5; ++I) { Medians[I] = OracleMs(4, kDepths[I], PairFor(4)); }
	const P::FPromptPrice Untouched{-7.0, -7.0};
	auto Refused = [&](int32 C, const int64* Depths, const double* M, int32 N)
	{
		P::FPromptPrice Out = Untouched;
		return !P::FitSmallChunkArm(C, Depths, M, N, Out) && Out.TokenMs == Untouched.TokenMs && Out.PerPositionMs == Untouched.PerPositionMs;
	};
	EXPECT_TRUE(Refused(1, kDepths, Medians, 5));  // C = 1 is the chunk-1 arm's
	EXPECT_TRUE(Refused(8, kDepths, Medians, 5));  // C >= 8 is the batched arm's
	EXPECT_TRUE(Refused(4, kDepths, Medians, 0));
	const int64 Descending[] = {4088, 3072, 2048, 1024, 1};
	EXPECT_TRUE(Refused(4, Descending, Medians, 5));
	double Bad[5];
	for (int32 I = 0; I < 5; ++I) { Bad[I] = Medians[I]; }
	Bad[2] = 0.0;
	EXPECT_TRUE(Refused(4, kDepths, Bad, 5));
	Bad[2] = std::numeric_limits<double>::quiet_NaN();
	EXPECT_TRUE(Refused(4, kDepths, Bad, 5));
	// Control: the same readings at C = 4 fit.
	P::FPromptPrice Out = Untouched;
	EXPECT_TRUE(P::FitSmallChunkArm(4, kDepths, Medians, 5, Out));
}
