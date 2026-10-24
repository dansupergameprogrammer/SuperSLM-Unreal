// Precache U4 (plan §5.5, risk R5): the cache restore's price by bytes, and the per-MiB slope
// SuperSLM.EstimateCosts suggests (SuperSLMRestorePricing.h). The Unreal halves -- the lane job's payload size,
// the resolver's optional key, Configure()'s refusal and the estimate's restore_size arm -- are box legs (U4).

#include "harness.h"
#include "SuperSLMRestorePricing.h"

#include <cmath>
#include <limits>

using namespace SuperSLMCore;

namespace
{
	constexpr int64_t kMiB = 1024 * 1024;

	bool Near(double A, double B)
	{
		return std::fabs(A - B) <= 1e-9 * (1.0 + std::fabs(B));
	}
}

HOST_TEST(restore_price_is_fixed_plus_per_mib, "U4 R5")
{
	EXPECT_TRUE(Near(RestorePlannedMs(2.0, 0.5, 4 * kMiB), 4.0));
	EXPECT_TRUE(Near(RestorePlannedMs(2.0, 0.5, kMiB / 2), 2.25));
	EXPECT_TRUE(Near(RestorePlannedMs(1.5, 0.25, 10 * kMiB + kMiB / 4), 1.5 + 0.25 * 10.25));
}

HOST_TEST(restore_price_without_a_slope_is_the_flat_price, "U4 R5")
{
	// A profile measured before U4 (no key) resolves the term to 0: today's flat price at every size.
	EXPECT_TRUE(Near(RestorePlannedMs(4.644, 0.0, 0), 4.644));
	EXPECT_TRUE(Near(RestorePlannedMs(4.644, 0.0, 512 * kMiB), 4.644));
	// No payload (a ref with no bytes) is priced flat whatever the slope.
	EXPECT_TRUE(Near(RestorePlannedMs(4.644, 3.0, 0), 4.644));
	EXPECT_TRUE(Near(RestorePlannedMs(4.644, 3.0, -1), 4.644));
}

HOST_TEST(restore_price_never_lowered_by_a_bad_slope, "U4 R5")
{
	// Configure() refuses these; if one arrived anyway, the price stays at the fixed cost.
	EXPECT_TRUE(Near(RestorePlannedMs(3.0, -1.0, 8 * kMiB), 3.0));
	EXPECT_TRUE(Near(RestorePlannedMs(3.0, std::numeric_limits<double>::quiet_NaN(), 8 * kMiB), 3.0));
	EXPECT_TRUE(Near(RestorePlannedMs(3.0, std::numeric_limits<double>::infinity(), 8 * kMiB), 3.0));
}

HOST_TEST(per_mib_fit_leaves_out_blobs_under_one_mib, "U4 R5")
{
	// Depth 1's blob (1/64 MiB) would set a slope of 3.2 ms per MiB through the fixed cost; it is left out, and the
	// slope comes from the smallest and largest remaining readings: (13.2 - 4.10) / (24 - 6) = 0.50556.
	const double Fixed = 1.0;
	const FRestoreSizeReading R[] = {
		{1 * kMiB / 64, 1.05}, {6 * kMiB, 4.10}, {12 * kMiB, 7.30}, {18 * kMiB, 10.0}, {24 * kMiB, 13.2}};
	double Slope = -1.0;
	REQUIRE_TRUE(FitRestorePerMiB(Fixed, R, 5, Slope));
	EXPECT_TRUE(Near(Slope, (13.2 - 4.10) / 18.0));
	// The floor is 1 MiB inclusive: a reading of exactly 1 MiB is kept.
	const FRestoreSizeReading AtFloor[] = {{kMiB, 2.0}, {5 * kMiB, 4.0}};
	REQUIRE_TRUE(FitRestorePerMiB(Fixed, AtFloor, 2, Slope));
	EXPECT_TRUE(Near(Slope, 0.5));
	// Only blobs under the floor: nothing to fit.
	const FRestoreSizeReading Small[] = {{kMiB / 64, 1.05}, {kMiB - 1, 1.2}};
	EXPECT_TRUE(!FitRestorePerMiB(Fixed, Small, 2, Slope));
	EXPECT_EQ(Slope, 0.0);
}

HOST_TEST(per_mib_fit_uses_the_smallest_and_largest_not_the_steepest_pair, "U4 R5")
{
	// A middle reading far above the line between the ends does not move the slope (an envelope would take it),
	// and the readings' order does not matter.
	const FRestoreSizeReading R[] = {{16 * kMiB, 30.0}, {32 * kMiB, 17.0}, {2 * kMiB, 3.0}};
	double Slope = 0.0;
	REQUIRE_TRUE(FitRestorePerMiB(1.0, R, 3, Slope));
	EXPECT_TRUE(Near(Slope, (17.0 - 3.0) / 30.0));
}

HOST_TEST(per_mib_fit_of_one_size_runs_through_the_fixed_cost, "U4 R5")
{
	double Slope = 0.0;
	const FRestoreSizeReading One[] = {{4 * kMiB, 3.0}};
	REQUIRE_TRUE(FitRestorePerMiB(1.0, One, 1, Slope));
	EXPECT_TRUE(Near(Slope, 0.5));
	const FRestoreSizeReading Same[] = {{4 * kMiB, 2.0}, {4 * kMiB, 3.0}};
	REQUIRE_TRUE(FitRestorePerMiB(1.0, Same, 2, Slope));
	EXPECT_TRUE(Near(Slope, 0.5));
}

HOST_TEST(per_mib_fit_is_zero_when_every_reading_is_below_the_fixed_cost, "U4 R5")
{
	// Review N1: every median under RestoreCostMs. The two-point slope is negative, and so is each reading's slope
	// through the fixed cost; the fit floors both at 0, never negative.
	const FRestoreSizeReading Falling[] = {{2 * kMiB, 1.8}, {16 * kMiB, 1.5}};
	double Slope = -1.0;
	REQUIRE_TRUE(FitRestorePerMiB(2.0, Falling, 2, Slope));
	EXPECT_EQ(Slope, 0.0);
	const FRestoreSizeReading One[] = {{8 * kMiB, 1.5}};
	Slope = -1.0;
	REQUIRE_TRUE(FitRestorePerMiB(2.0, One, 1, Slope));
	EXPECT_EQ(Slope, 0.0);
}

HOST_TEST(per_mib_fit_of_a_clean_line_recovers_its_slope, "U4 R5")
{
	const FRestoreSizeReading R[] = {{2 * kMiB, 1.0 + 0.75 * 2}, {8 * kMiB, 1.0 + 0.75 * 8}, {32 * kMiB, 1.0 + 0.75 * 32}};
	double Slope = 0.0;
	REQUIRE_TRUE(FitRestorePerMiB(1.0, R, 3, Slope));
	EXPECT_TRUE(Near(Slope, 0.75));
}

HOST_TEST(per_mib_fit_is_zero_when_size_costs_nothing, "U4 R5")
{
	// Every reading at or below the fixed cost: a flat price already covers them, the slope is 0, not negative.
	const FRestoreSizeReading R[] = {{4 * kMiB, 2.0}, {16 * kMiB, 1.9}};
	double Slope = -1.0;
	REQUIRE_TRUE(FitRestorePerMiB(2.0, R, 2, Slope));
	EXPECT_EQ(Slope, 0.0);
}

HOST_TEST(per_mib_fit_refuses_what_it_cannot_measure, "U4 R5")
{
	double Slope = -1.0;
	// No reading with bytes: nothing to fit (the estimate reports an incomplete arm).
	const FRestoreSizeReading Empty[] = {{0, 3.0}};
	EXPECT_TRUE(!FitRestorePerMiB(1.0, Empty, 1, Slope));
	EXPECT_EQ(Slope, 0.0);
	EXPECT_TRUE(!FitRestorePerMiB(1.0, nullptr, 0, Slope));
	// A non-finite median or fixed cost.
	const FRestoreSizeReading Bad[] = {{kMiB, std::numeric_limits<double>::quiet_NaN()}};
	EXPECT_TRUE(!FitRestorePerMiB(1.0, Bad, 1, Slope));
	const FRestoreSizeReading Good[] = {{kMiB, 2.0}};
	EXPECT_TRUE(!FitRestorePerMiB(std::numeric_limits<double>::infinity(), Good, 1, Slope));
	EXPECT_EQ(Slope, 0.0);
}
