// Precache plan rev 1.18.12 (§15.32): the in-situ prefix_prefill arm. Core/SuperSLMInSituPrefixPrefill.h: a ledger
// chunk row as a reading, the fit of a build's rows in depth order, the attribution median, the estimate's rounding,
// and the rewrite of the two keys in one section of CalibratedCosts.ini.
//
// Written red first against a stubbed header (no row ever a reading, no fit, a median of 0, an empty rounding, the
// text never rewritten). Mutants:
//   - the start depth read as the position sum / C (the triangle term kept)  -> insitu_a_chunk_row_is_a_reading
//   - rows under 8 tokens kept                                              -> insitu_a_row_the_pair_does_not_price_is_refused
//   - the rows fitted in ledger order, not depth order                      -> insitu_the_fit_reads_rows_in_depth_order
//   - the first matching header rewritten (another artifact's profile)      -> insitu_only_the_indexed_section_is_rewritten
//   - an old key left beside the new one                                    -> insitu_the_keys_are_replaced_not_duplicated
//   - lines broken on '\n' alone (code review S1, red at 0ac0c077b)          -> insitu_a_lone_cr_breaks_a_line_as_the_parser_does

#include "harness.h"
#include "SuperSLMInSituPrefixPrefill.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace SuperSLMCore;

namespace
{
	int64_t PositionSum(int64_t Depth, int32_t C)
	{
		return static_cast<int64_t>(C) * Depth + static_cast<int64_t>(C) * (C - 1) / 2;
	}

	const std::vector<std::pair<std::string, std::string>> kPair = {
		{ "PrefixPrefillTokenCostMs", "30.731" }, { "PrefixPrefillTokenCostPerPositionMs", "0.004700" } };

	const char* const kIni =
		"; SuperSLM.EstimateCosts output\n"
		"[SuperSLM.EstimatedCosts.Mode0]\n"
		"bParallelDecode=0\n"
		"ArtifactHash=AAAA\n"
		"FinishParallelTasks=4\n"
		"RestoreCostPerMiBMs=0.136000\n"
		"PrefixPrefillTokenCostMs=15.349\n"
		"PrefixPrefillTokenCostPerPositionMs=0.004666\n"
		"[SuperSLM.EstimatedCosts.Mode1]\n"
		"bParallelDecode=1\n"
		"ArtifactHash=AAAA\n"
		"FinishParallelTasks=4\n"
		"PrefixPrefillTokenCostMs=15.360\n"
		"PrefixPrefillTokenCostPerPositionMs=0.004677\n"
		"\n"
		"[SuperSLM.EstimatedCosts.Mode0]\n"
		"bParallelDecode=0\n"
		"ArtifactHash=BBBB\n"
		"FinishParallelTasks=4\n"
		"PrefixPrefillTokenCostMs=11.000\n"
		"PrefixPrefillTokenCostPerPositionMs=0.001000\n";
}

HOST_TEST(insitu_a_chunk_row_is_a_reading, "5.9 (d) §15.32")
{
	FPrefixPrefillReading R;
	REQUIRE_TRUE(ChunkRowReading(8, PositionSum(0, 8), 245.5, R));
	EXPECT_EQ(R.StartDepth, 0);
	EXPECT_EQ(R.Tokens, 8);
	EXPECT_TRUE(R.Ms == 245.5);
	REQUIRE_TRUE(ChunkRowReading(8, PositionSum(1992, 8), 300.0, R));
	EXPECT_EQ(R.StartDepth, 1992);
	REQUIRE_TRUE(ChunkRowReading(64, PositionSum(128, 64), 900.0, R));
	EXPECT_EQ(R.StartDepth, 128);
	EXPECT_EQ(R.Tokens, 64);
}

HOST_TEST(insitu_a_row_the_pair_does_not_price_is_refused, "5.9 (d) §15.32")
{
	FPrefixPrefillReading R;
	EXPECT_TRUE(!ChunkRowReading(7, PositionSum(0, 7), 100.0, R));      // the chunk-1 pair prices it
	EXPECT_TRUE(!ChunkRowReading(0, 0, 100.0, R));
	EXPECT_TRUE(!ChunkRowReading(8, PositionSum(16, 8), -1.0, R));      // not measured
	EXPECT_TRUE(!ChunkRowReading(8, PositionSum(16, 8), std::nan(""), R));
	EXPECT_TRUE(!ChunkRowReading(8, PositionSum(16, 8) + 3, 100.0, R)); // not 8 consecutive positions
	EXPECT_TRUE(!ChunkRowReading(8, 10, 100.0, R));                     // below a start depth of 0
}

HOST_TEST(insitu_the_fit_reads_rows_in_depth_order, "5.9 (d) §15.32")
{
	// A rising arm in depth order, handed over reversed (the ledger may interleave other rows; only the start
	// depth orders a reading). Read in the order given, the chord would fall and the slope floor at 0.
	const FChunkPricePair Truth{ 30.0, 0.005 };
	std::vector<FPrefixPrefillReading> R;
	for (int64_t D = 2040; D >= 0; D -= 8)
	{
		R.push_back(FPrefixPrefillReading{ D, 8, ChunkPairPriceMs(8, D, Truth) });
	}
	FChunkPricePair Fit;
	REQUIRE_TRUE(FitInSituPrefixPrefillPair(R, Fit));
	EXPECT_TRUE(std::fabs(Fit.TokenMs - Truth.TokenMs) < 1e-9);
	EXPECT_TRUE(std::fabs(Fit.PerPositionMs - Truth.PerPositionMs) < 1e-12);
	std::vector<FPrefixPrefillReading> Few(R.begin(), R.begin() + 7);
	EXPECT_TRUE(!FitInSituPrefixPrefillPair(Few, Fit)); // fewer rows than the fit's groups
}

HOST_TEST(insitu_the_attribution_median_is_ms_per_token, "5.9 (d) §15.32")
{
	const FPrefixPrefillReading R[] = { { 0, 8, 240.0 }, { 8, 8, 248.0 }, { 16, 8, 256.0 }, { 24, 8, 1000.0 } };
	EXPECT_TRUE(MedianMsPerToken(R, 4) == 0.5 * (31.0 + 32.0));
	EXPECT_TRUE(MedianMsPerToken(R, 3) == 31.0);
	EXPECT_TRUE(MedianMsPerToken(nullptr, 0) == 0.0);
}

HOST_TEST(insitu_the_pair_is_rounded_up_as_the_estimate_rounds, "5.9 (d) §15.32")
{
	EXPECT_EQ(RoundUpDecimalText(30.7301, 3), std::string("30.731"));
	EXPECT_EQ(RoundUpDecimalText(30.731, 3), std::string("30.731"));
	EXPECT_EQ(RoundUpDecimalText(0.0046661, 6), std::string("0.004667"));
	EXPECT_EQ(RoundUpDecimalText(0.0, 6), std::string("0.000000"));
	EXPECT_EQ(RoundUpDecimalText(12.0, 3), std::string("12.000"));
}

HOST_TEST(insitu_only_the_indexed_section_is_rewritten, "5.9 (d) §15.32")
{
	bool bFound = false;
	const std::string Out = SetIniSectionKeys(kIni, 2, kPair, bFound);
	REQUIRE_TRUE(bFound);
	// The third section (artifact BBBB) carries the new pair; the first two keep theirs.
	EXPECT_TRUE(Out.find("ArtifactHash=BBBB\nFinishParallelTasks=4\nPrefixPrefillTokenCostMs=30.731\nPrefixPrefillTokenCostPerPositionMs=0.004700\n") != std::string::npos);
	EXPECT_TRUE(Out.find("PrefixPrefillTokenCostMs=15.349\n") != std::string::npos);
	EXPECT_TRUE(Out.find("PrefixPrefillTokenCostMs=15.360\n") != std::string::npos);
	EXPECT_TRUE(Out.find("11.000") == std::string::npos);
	EXPECT_TRUE(Out.find("; SuperSLM.EstimateCosts output\n") == 0);
}

HOST_TEST(insitu_the_keys_are_replaced_not_duplicated, "5.9 (d) §15.32")
{
	bool bFound = false;
	const std::string Out = SetIniSectionKeys(kIni, 1, kPair, bFound);
	REQUIRE_TRUE(bFound);
	// Mode1's pair is replaced in place of its old lines, before the blank line that ends the section.
	EXPECT_TRUE(Out.find("FinishParallelTasks=4\nPrefixPrefillTokenCostMs=30.731\nPrefixPrefillTokenCostPerPositionMs=0.004700\n\n[SuperSLM.EstimatedCosts.Mode0]") != std::string::npos);
	EXPECT_TRUE(Out.find("15.360") == std::string::npos);
	size_t Count = 0;
	for (size_t P = Out.find("PrefixPrefillTokenCostMs="); P != std::string::npos; P = Out.find("PrefixPrefillTokenCostMs=", P + 1))
	{
		++Count;
	}
	EXPECT_EQ(Count, static_cast<size_t>(3));
}

HOST_TEST(insitu_a_section_without_the_keys_gains_them, "5.9 (d) §15.32")
{
	bool bFound = false;
	const std::string Out = SetIniSectionKeys("[S]\nA=1\r\n; note\nB=2\n", 0, kPair, bFound);
	REQUIRE_TRUE(bFound);
	EXPECT_EQ(Out, std::string("[S]\nA=1\n; note\nB=2\nPrefixPrefillTokenCostMs=30.731\nPrefixPrefillTokenCostPerPositionMs=0.004700\n"));
}

HOST_TEST(insitu_no_such_section_leaves_the_text, "5.9 (d) §15.32")
{
	bool bFound = true;
	EXPECT_EQ(SetIniSectionKeys(kIni, 3, kPair, bFound), std::string(kIni));
	EXPECT_TRUE(!bFound);
	EXPECT_EQ(SetIniSectionKeys("; only a comment\n", 0, kPair, bFound), std::string("; only a comment\n"));
	EXPECT_TRUE(!bFound);
}

HOST_TEST(insitu_a_lone_cr_breaks_a_line_as_the_parser_does, "5.9 (d) §15.32")
{
	// Code review S1: SuperSLMRuntimeResolve::ParseIniSections (ParseIntoArrayLines) also breaks a line on a lone '\r'
	// and on '\r\n', so the section index it chose must name the same section here. "[X]" is the second section, "[T]"
	// the third; read on '\n' alone, "k=1\r[X]" would be one line and index 2 would name "[Other]".
	bool bFound = false;
	const std::string Out = SetIniSectionKeys("[P0]\nk=1\r[X]\r\n[T]\nA=1\n[Other]\nB=2\n", 2, kPair, bFound);
	REQUIRE_TRUE(bFound);
	EXPECT_EQ(Out, std::string("[P0]\nk=1\n[X]\n[T]\nA=1\nPrefixPrefillTokenCostMs=30.731\nPrefixPrefillTokenCostPerPositionMs=0.004700\n[Other]\nB=2\n"));
	// A header led by a vertical tab or form feed is a header, as TrimStartAndEnd reads it.
	const std::string Led = SetIniSectionKeys("[A]\n\v[B]\nC=1\n", 1, kPair, bFound);
	REQUIRE_TRUE(bFound);
	EXPECT_EQ(Led, std::string("[A]\n\v[B]\nC=1\nPrefixPrefillTokenCostMs=30.731\nPrefixPrefillTokenCostPerPositionMs=0.004700\n"));
}
