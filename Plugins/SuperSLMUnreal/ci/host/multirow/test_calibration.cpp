// MR6 red suite, erratum E-13 (ruling 48): the load-time row-cost calibration's pure pieces
// (Source/SuperSLMUnreal/Private/Core/SuperSLMGpuRowCostCalibration.h). Every expectation is ruling 48's text worked by
// hand here, never the header's arithmetic.

#include "SuperSLMGpuRowCostCalibration.h"
#include "harness.h"

#include <cmath>
#include <limits>

using namespace SuperSLMCore::GpuRowCost;

HOST_TEST(calibration_constants_are_ruling_48s, "R48 (1 warm-up, 5 timed runs, one retry, 6.2 ms, [0.2x, 20x])")
{
	EXPECT_EQ(kDefaultRowCostUs, 6200);
	EXPECT_EQ(kSanityMinUs, 1240);
	EXPECT_EQ(kSanityMaxUs, 124000);
	EXPECT_EQ(kWarmUpRuns, 1);
	EXPECT_EQ(kTimedRuns, 5);
	EXPECT_EQ(kMaxAttempts, 2);
}

HOST_TEST(calibration_price_is_the_max_rounded_up_to_0_1_ms, "R48 (price = the max of the timed runs, rounded up to 0.1 ms)")
{
	const double A[5] = {10850.0, 10900.0, 10870.3, 10123.4, 10899.9};
	EXPECT_EQ(PriceUs(A, 5), 10900);             // the max is a multiple: 10.9 ms, not 11.0
	const double B[5] = {9000.0, 9000.0, 9000.0, 9000.0, 10900.02};
	EXPECT_EQ(PriceUs(B, 5), 11000);             // 0.02 us over a multiple rounds up
	const double C[5] = {6200.005, 6100.0, 6000.0, 5900.0, 5800.0};
	EXPECT_EQ(PriceUs(C, 5), 6200);              // within 0.01 us of a multiple: that multiple
	const double D[1] = {12345.6};
	EXPECT_EQ(PriceUs(D, 1), 12400);
	const double E[1] = {50.0};
	EXPECT_EQ(PriceUs(E, 1), 100);
	const double F[2] = {700.0, 1240.0};
	EXPECT_EQ(PriceUs(F, 2), 1300);              // 1240 is not a multiple of 100
	const double G[5] = {1.0, 2.0, 3.0, 4.0, 124000.0};
	EXPECT_EQ(PriceUs(G, 5), 124000);
}

HOST_TEST(calibration_price_of_a_failed_attempt_is_minus_one, "R48 (no runs, or a run not finite or not above 0: the attempt failed)")
{
	const double Ok[1] = {7000.0};
	EXPECT_EQ(PriceUs(Ok, 1), 7000);             // red on the stub
	EXPECT_EQ(PriceUs(Ok, 0), -1);
	EXPECT_EQ(PriceUs(nullptr, 0), -1);
	const double Zero[3] = {7000.0, 0.0, 7100.0};
	EXPECT_EQ(PriceUs(Zero, 3), -1);
	const double Neg[2] = {7000.0, -5.0};
	EXPECT_EQ(PriceUs(Neg, 2), -1);
	const double NaN[2] = {7000.0, std::numeric_limits<double>::quiet_NaN()};
	EXPECT_EQ(PriceUs(NaN, 2), -1);
	const double Inf[2] = {std::numeric_limits<double>::infinity(), 7000.0};
	EXPECT_EQ(PriceUs(Inf, 2), -1);
}

HOST_TEST(calibration_sanity_range_is_0_2x_to_20x_of_6_2_ms, "R48 (a price outside [1.24 ms, 124 ms] is noise; both ends accepted)")
{
	EXPECT_TRUE(InSanityRange(1240));
	EXPECT_TRUE(InSanityRange(6200));
	EXPECT_TRUE(InSanityRange(10900));
	EXPECT_TRUE(InSanityRange(124000));
	EXPECT_TRUE(!InSanityRange(1200));
	EXPECT_TRUE(!InSanityRange(124100));
	EXPECT_TRUE(!InSanityRange(0));
	EXPECT_TRUE(!InSanityRange(-1));
}

HOST_TEST(calibration_retries_out_of_range_once_then_falls_back, "R48 (reject as noise, retry once, then fall back; a failure falls back)")
{
	EXPECT_EQ(Step(0, 10900), EStep::Use);
	EXPECT_EQ(Step(1, 10900), EStep::Use);
	EXPECT_EQ(Step(0, 1240), EStep::Use);
	EXPECT_EQ(Step(0, 124000), EStep::Use);
	EXPECT_EQ(Step(0, 500), EStep::Retry);
	EXPECT_EQ(Step(0, 200000), EStep::Retry);
	EXPECT_EQ(Step(1, 500), EStep::FallBack);
	EXPECT_EQ(Step(1, 200000), EStep::FallBack);
	EXPECT_EQ(Step(0, -1), EStep::FallBack);
	EXPECT_EQ(Step(1, -1), EStep::FallBack);
}

HOST_TEST(calibration_choice_game_value_wins_then_switch_then_measured, "R48 (an explicit game GpuRowCostMs wins; bCalibrateGpuRowCost false is 6.2 ms; fallback is 6.2 ms)")
{
	FChoice C = Choose(true, 7.3, true, 10900);
	EXPECT_EQ(C.Source, ESource::Game);
	EXPECT_TRUE(C.RowCostMs == 7.3);
	C = Choose(true, 7.3, false, -1);
	EXPECT_EQ(C.Source, ESource::Game);
	EXPECT_TRUE(C.RowCostMs == 7.3);
	C = Choose(false, 0.0, false, 10900);
	EXPECT_EQ(C.Source, ESource::Default);
	EXPECT_TRUE(C.RowCostMs == 6.2);
	C = Choose(false, 0.0, true, 10900);
	EXPECT_EQ(C.Source, ESource::Calibrated);
	EXPECT_TRUE(std::fabs(C.RowCostMs - 10.9) < 1e-12);
	C = Choose(false, 0.0, true, -1);
	EXPECT_EQ(C.Source, ESource::FallBack);
	EXPECT_TRUE(C.RowCostMs == 6.2);
}

// ------------------------------------------------------------------------------------------------ erratum E-14
// Rulings 53 and 54: the price as a line in position, in two phases. Every figure below is worked by hand from the
// rulings' text.

HOST_TEST(calibration_e14_constants_late_position_and_phase_2, "E-14 (1 + 3 tokens per position; P2 = min(256, cap - 8); s_default 0.0015 C_a; phase 2 only at B > 0)")
{
	EXPECT_EQ(kWarmUpRunsPerPosition, 1);
	EXPECT_EQ(kTimedRunsPerPosition, 3);
	EXPECT_EQ(kDefaultSlopeNum, 3);
	EXPECT_EQ(kDefaultSlopeDen, 2000);
	EXPECT_EQ(LatePosition(4096), 256);
	EXPECT_EQ(LatePosition(264), 256);
	EXPECT_EQ(LatePosition(263), 255);
	EXPECT_EQ(LatePosition(200), 192);
	EXPECT_EQ(LatePosition(16), 8);
	EXPECT_TRUE(RunsPhase2(2.0));
	EXPECT_TRUE(RunsPhase2(0.5));
	EXPECT_TRUE(!RunsPhase2(0.0));  // ruling 54: at B = 0 phase 2 is skipped
}

HOST_TEST(calibration_e14_default_slope_line, "ruling 54 (phase 1: C_a with s_default = 0.0015 x C_a per position)")
{
	// C_a = 7.0 ms at 12: s_default = 10.5 us per position.
	const FRowCostLine L = DefaultSlopeLine(7000, 12);
	EXPECT_EQ(L.AnchorUs, 7000);
	EXPECT_EQ(L.AnchorPos, 12);
	EXPECT_EQ(L.SlopeNumUs, 21000);  // 3 x 7000
	EXPECT_EQ(L.SlopeDenPos, 2000);
	EXPECT_EQ(L.Source, ELineSource::DefaultSlope);
	EXPECT_EQ(RowCostAtUs(L, 12), 7000);
	EXPECT_EQ(RowCostAtUs(L, 112), 8050);    // 10.5 x 100
	EXPECT_EQ(RowCostAtUs(L, 13), 7011);     // 10.5 rounds up to 11
	EXPECT_EQ(RowCostAtUs(L, 0), 6874);      // -126 exactly
	EXPECT_EQ(RowCostAtUs(L, 4096), 49882);  // 21000 x 4084 / 2000 = 42,882
}

HOST_TEST(calibration_e14_line_prices_at_position_rounded_up, "E-14 (the measured line: C_a + max(0, slope) x (pos - pos_a), a cost: rounded up)")
{
	// C_a = 7.0 ms at 12, C_b = 8.2 ms at 256: slope 1200 / 244 us per position.
	const FRowCostLine L = LineFrom(7000, 12, 8200, 256);
	EXPECT_EQ(L.AnchorUs, 7000);
	EXPECT_EQ(L.AnchorPos, 12);
	EXPECT_EQ(L.SlopeNumUs, 1200);
	EXPECT_EQ(L.SlopeDenPos, 244);
	EXPECT_EQ(L.Source, ELineSource::Measured);
	EXPECT_EQ(RowCostAtUs(L, 12), 7000);
	EXPECT_EQ(RowCostAtUs(L, 256), 8200);
	EXPECT_EQ(RowCostAtUs(L, 134), 7600);    // 1200 x 122 / 244 = 600 exactly
	EXPECT_EQ(RowCostAtUs(L, 13), 7005);     // 4.918... rounds up to 5
	EXPECT_EQ(RowCostAtUs(L, 4096), 27086);  // 1200 x 4084 / 244 = 20,085.2... -> 20,086
	EXPECT_EQ(RowCostAtUs(L, 0), 6941);      // before pos_a: -59.01... rounds up to -59
}

HOST_TEST(calibration_e14_negative_slope_and_flat_lines, "E-14 (max(0, slope); the override and the fallback are flat)")
{
	const FRowCostLine Down = LineFrom(8000, 12, 7500, 256);  // a late price below the early one: slope 0
	EXPECT_EQ(Down.SlopeNumUs, 0);
	EXPECT_EQ(Down.Source, ELineSource::Measured);
	EXPECT_EQ(RowCostAtUs(Down, 12), 8000);
	EXPECT_EQ(RowCostAtUs(Down, 4096), 8000);
	EXPECT_EQ(RowCostAtUs(Down, 0), 8000);
	const FRowCostLine Same = LineFrom(7000, 12, 9000, 12);   // no distance between the positions: flat
	EXPECT_EQ(RowCostAtUs(Same, 4096), 7000);
	const FRowCostLine F = FlatLine(6200);
	EXPECT_EQ(F.AnchorUs, 6200);
	EXPECT_EQ(F.SlopeNumUs, 0);
	EXPECT_EQ(F.Source, ELineSource::Flat);
	EXPECT_EQ(RowCostAtUs(F, 0), 6200);
	EXPECT_EQ(RowCostAtUs(F, 4096), 6200);
}

HOST_TEST(calibration_e14_sanity_on_c_a_and_at_the_cap, "E-14 (C_a in [1.24, 124] ms and C(1, cap) <= 20 x 6.2 ms)")
{
	EXPECT_TRUE(LineInSanityRange(LineFrom(7000, 12, 8200, 256), 4096));    // 27,086 at the cap
	EXPECT_TRUE(!LineInSanityRange(LineFrom(7000, 12, 60000, 256), 4096));  // 7000 + ceil(53000 x 4084 / 244) > 124,000
	EXPECT_TRUE(LineInSanityRange(LineFrom(7000, 12, 60000, 256), 264));    // 61,738 at 264
	EXPECT_TRUE(!LineInSanityRange(LineFrom(1000, 12, 1200, 256), 4096));   // C_a under 1,240
	EXPECT_TRUE(LineInSanityRange(LineFrom(1240, 12, 1240, 256), 4096));
	EXPECT_TRUE(!LineInSanityRange(LineFrom(124100, 12, 124100, 256), 4096));
	EXPECT_TRUE(LineInSanityRange(FlatLine(124000), 4096));
}

HOST_TEST(calibration_e14_phase_1_step, "ruling 54 (phase 1 is E-13's step on C_a: in range used, out of range retried once, a failure falls back)")
{
	EXPECT_EQ(StepPhase1(0, 7000), EStep::Use);
	EXPECT_EQ(StepPhase1(1, 7000), EStep::Use);
	EXPECT_EQ(StepPhase1(0, 1240), EStep::Use);
	EXPECT_EQ(StepPhase1(0, 124000), EStep::Use);
	EXPECT_EQ(StepPhase1(0, 500), EStep::Retry);
	EXPECT_EQ(StepPhase1(1, 500), EStep::FallBack);
	EXPECT_EQ(StepPhase1(0, 200000), EStep::Retry);
	EXPECT_EQ(StepPhase1(1, 200000), EStep::FallBack);
	EXPECT_EQ(StepPhase1(0, -1), EStep::FallBack);
}

HOST_TEST(calibration_e14_phase_2_step, "ruling 54 (phase 2: the measured line, or the default slope kept on a failure or a line out of sanity)")
{
	EXPECT_EQ(StepPhase2(7000, 12, 8200, 256, 4096), EPhase2::Use);
	EXPECT_EQ(StepPhase2(8000, 12, 7500, 256, 4096), EPhase2::Use);          // slope 0 is a measured slope
	EXPECT_EQ(StepPhase2(7000, 12, -1, 256, 4096), EPhase2::KeepDefault);    // the late position failed
	EXPECT_EQ(StepPhase2(7000, 12, 60000, 256, 4096), EPhase2::KeepDefault); // too steep at the cap
	EXPECT_EQ(StepPhase2(7000, 12, 60000, 256, 264), EPhase2::Use);          // the same line within a small cap
}

HOST_TEST(calibration_e14_choose_line, "E-14 (game and fallback flat; a calibrated line kept as given, Calibrated, the line naming the phase)")
{
	const FRowCostLine Phase1 = DefaultSlopeLine(7000, 12);
	const FRowCostLine Phase2 = LineFrom(7000, 12, 8200, 256);
	FLineChoice C = ChooseLine(true, 6.967, true, true, Phase2);
	EXPECT_EQ(C.Source, ESource::Game);
	EXPECT_TRUE(C.RowCostMs == 6.967);
	EXPECT_EQ(C.Line.AnchorUs, 6967);
	EXPECT_EQ(C.Line.SlopeNumUs, 0);
	EXPECT_EQ(C.Line.Source, ELineSource::Flat);
	C = ChooseLine(false, 0.0, false, true, Phase2);
	EXPECT_EQ(C.Source, ESource::Default);
	EXPECT_EQ(C.Line.AnchorUs, 6200);
	EXPECT_EQ(C.Line.SlopeNumUs, 0);
	C = ChooseLine(false, 0.0, true, false, Phase2);
	EXPECT_EQ(C.Source, ESource::FallBack);
	EXPECT_TRUE(C.RowCostMs == 6.2);
	EXPECT_EQ(C.Line.AnchorUs, 6200);
	EXPECT_EQ(C.Line.SlopeNumUs, 0);
	C = ChooseLine(false, 0.0, true, true, Phase1);
	EXPECT_EQ(C.Source, ESource::Calibrated);
	EXPECT_EQ(C.Line.Source, ELineSource::DefaultSlope);
	EXPECT_TRUE(std::fabs(C.RowCostMs - 7.0) < 1e-12);
	EXPECT_EQ(RowCostAtUs(C.Line, 112), 8050);
	C = ChooseLine(false, 0.0, true, true, Phase2);
	EXPECT_EQ(C.Source, ESource::Calibrated);
	EXPECT_EQ(C.Line.Source, ELineSource::Measured);
	EXPECT_TRUE(std::fabs(C.RowCostMs - 7.0) < 1e-12);
	EXPECT_EQ(C.Line.AnchorPos, 12);
	EXPECT_EQ(RowCostAtUs(C.Line, 256), 8200);
}
