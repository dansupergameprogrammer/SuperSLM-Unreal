// MR6 red suite, the chooser (multi-row GPU plan rev 1.3.2 §5.1-§5.3, §5.6, §4.7, MR-D11; cells 10.6, 7.11 (a)-(c),
// N4, P4.4's chooser half; rulings 37 and 39). Every expectation comes from chooser_ref.py (exact rationals) through grid.txt, or from the plan's own
// worked figures, never from the chooser's arithmetic. red-suite-mr6.md lists the cells and the open questions.

#include "SuperSLMGpuRowsChooser.h"
#include "grid.h"
#include "harness.h"
#include "suite_model.h"

#include <cstring>
#include <string>
#include <vector>

using namespace SuperSLMCore::GpuRows;
using namespace MultiRowHost;

namespace
{
	const std::vector<FCase>& Grid()
	{
		static std::vector<FCase> G;
		static bool Loaded = false;
		if (!Loaded)
		{
			std::string Err;
			G = LoadGrid(&Err);
			if (G.empty()) std::printf("grid: %s\n", Err.c_str());
			Loaded = true;
		}
		return G;
	}

	// The engine's MR4 table (gpu_rows_cost_table.h, estimates), the plan's default configuration.
	FChooserInput Default()
	{
		FChooserInput In;
		const uint32_t R[8] = {1000, 1275, 1565, 1855, 2142, 2428, 2714, 3000};
		const uint32_t G[8] = {250, 319, 392, 464, 536, 607, 679, 750};
		std::memcpy(In.CostRatioPerMille, R, sizeof R);
		std::memcpy(In.LogitsRatioPerMille, G, sizeof G);
		In.bTableMeasured = true;
		In.bExactChosen = true;
		return In;
	}

	bool SamePlan(const FChooserPlan& A, const FChooserPlan& B)
	{
		if (A.bValid != B.bValid || A.TableStatus != B.TableStatus || A.bMultiRowOn != B.bMultiRowOn ||
		    A.bBudgetBelowOneRow != B.bBudgetBelowOneRow || A.RowCostUs != B.RowCostUs || A.SceneUs != B.SceneUs ||
		    A.BudgetUs != B.BudgetUs || A.MMax != B.MMax || A.MMaxLayers != B.MMaxLayers || A.ComposedRows != B.ComposedRows ||
		    A.ComposedLayers != B.ComposedLayers || A.ComposedUnits != B.ComposedUnits || A.bExactOn != B.bExactOn ||
		    A.ExactReason != B.ExactReason || A.MExact != B.MExact)
			return false;
		for (int i = 0; i < MaxRows; ++i)
			if (A.CostUs[i] != B.CostUs[i] || A.LogitsUs[i] != B.LogitsUs[i] || A.LayersPerSlice[i] != B.LayersPerSlice[i] ||
			    A.Ticks[i] != B.Ticks[i] || A.ExactTicks[i] != B.ExactTicks[i])
				return false;
		return true;
	}

	// How many grid cases a plan function gets wrong; the first difference in First.
	template <typename Fn>
	int GridMisses(Fn Plan, std::string* First)
	{
		int Misses = 0;
		for (const FCase& C : Grid())
		{
			std::vector<std::string> Diffs;
			if (!PlanMatches(Plan(C.In), C.Want, &Diffs))
			{
				if (Misses == 0 && First)
					*First = "grid line " + std::to_string(C.Line) + " (" + C.Table + "): " + Diffs.front();
				++Misses;
			}
		}
		return Misses;
	}
}

// ------------------------------------------------------------------------------------------------ 10.6
HOST_TEST(chooser_grid_equals_exact_rational_reference, "10.6; §5.1-§5.3; MR-D11")
{
	REQUIRE_GT(Grid().size(), static_cast<size_t>(0));
	std::string First;
	const int Misses = GridMisses([](const FChooserInput& In) { return ChooseRowsPlan(In); }, &First);
	if (Misses) std::printf("       %d of %zu grid cases differ; first: %s\n", Misses, Grid().size(), First.c_str());
	EXPECT_EQ(Misses, 0);
}

HOST_TEST(chooser_price_never_exceeds_budget, "10.6 (the chosen plan's integer price <= B_eff in every tick, Exact's logits tick included; ruling 39 Q2's one layer excepted)")
{
	for (const FCase& C : Grid())
	{
		const FChooserPlan P = ChooseRowsPlan(C.In);
		REQUIRE_TRUE(P.bValid);
		const int64_t L = C.In.Layers, m = C.In.GpuRowsMarginPerMille, Room = P.BudgetUs - P.SceneUs;
		// M = 1: l(1) x C(1) <= Room x L, unless l(1) is the one layer always admitted (ruling 39 Q2); with the knob
		// set, with the margin (E-14, ruling 53): l(1) x C(1) x (1000 + m) <= Room x L x 1000.
		if (P.LayersPerSlice[0] > 1) EXPECT_LE(static_cast<int64_t>(P.LayersPerSlice[0]) * P.RowCostUs, Room * L);
		if (P.LayersPerSlice[0] > 1 && C.In.FrameBudgetMs != 0.0)
			EXPECT_LE(static_cast<int64_t>(P.LayersPerSlice[0]) * P.RowCostUs * (1000 + m), Room * L * 1000);
		EXPECT_GE(P.LayersPerSlice[0], 1);
		if (!P.bMultiRowOn) continue;
		for (int M = 2; M <= MaxRows; ++M)
		{
			const int64_t l = P.LayersPerSlice[M - 1];
			if (l == 0) continue;
			EXPECT_LE(l * P.CostUs[M - 1] * (1000 + m), Room * L * 1000);
			const int32_t Dx = P.ExactTicks[M - 1];
			if (Dx == P.Ticks[M - 1] && Dx > 0)  // the logits stage rides the last slice's tick
				EXPECT_LE((L - (Dx - 1) * l) * P.CostUs[M - 1] * (1000 + m) + P.LogitsUs[M - 1] * (1000 + m) * L, Room * L * 1000);
			else if (Dx > 0)  // ruling 42 R2: the last slice shrinks to k >= 1 layers with the logits stage, so one layer + logits fits
			{
				EXPECT_EQ(Dx, P.Ticks[M - 1] + 1);
				EXPECT_LE(P.CostUs[M - 1] * (1000 + m) + P.LogitsUs[M - 1] * (1000 + m) * L, Room * L * 1000);
			}
		}
		if (C.In.FrameBudgetMs != 0.0 && P.ComposedRows > 1)
			EXPECT_LE(static_cast<int64_t>(P.ComposedLayers) * P.CostUs[P.ComposedRows - 1] * (1000 + m), Room * L * 1000);
		EXPECT_LE(P.ComposedUnits, static_cast<int32_t>(C.In.LayersPerTick));
	}
}

HOST_TEST(chooser_float_inputs_convert_by_the_pinned_rule, "10.6 (rev 1.3 F4, applied to the double value: ruling 39 Q9)")
{
	EXPECT_EQ(CostMicros(6.2), 6200);
	EXPECT_EQ(CostMicros(7.9), 7900);
	EXPECT_EQ(CostMicros(11.5), 11500);
	EXPECT_EQ(CostMicros(18.6), 18600);
	EXPECT_EQ(BudgetMicros(6.2), 6200);
	EXPECT_EQ(BudgetMicros(18.6), 18600);
	EXPECT_EQ(BudgetMicros(6.2005), 6200);  // B rounds down
	EXPECT_EQ(CostMicros(6.2005), 6201);    // a cost rounds up
	EXPECT_EQ(BudgetMicros(16.7), 16700);
	// A value configured as f32 and widened (an older project's) converts the same.
	EXPECT_EQ(CostMicros(static_cast<double>(6.2f)), 6200);
	EXPECT_EQ(BudgetMicros(static_cast<double>(18.6f)), 18600);
}

HOST_TEST(chooser_knife_edge_c8_three_rows_m0, "10.6 knife edge (C(8) = 3 x C(1), m = 0: l = 8, D = 3); MX22")
{
	FChooserInput In = Default();
	In.GpuRowsMarginPerMille = 0;
	const FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_EQ(P.CostUs[7], 3 * P.RowCostUs);
	EXPECT_EQ(P.LayersPerSlice[7], 8);
	EXPECT_EQ(P.Ticks[7], 3);
	EXPECT_EQ(P.MMax, 8);
}

HOST_TEST(chooser_section_4_7_table, "10.6 (§4.7's rows: l / D_x at m = 0, 150 and gamma = 0, 0.25; ruling 42 R2: 8 x 6 at m = 150, gamma 0.25 is D_x 0)")
{
	// Ruling 42 R2: the plan's D_x = 5 at (8, 150, 0.25) counted a logits-only tick the engine cannot run; one layer plus
	// logits (891.25 + 5,347.5 us) is over 6,200, so that M has no Exact schedule.
	// C(M) = COST's spike figures; g(M) = gamma x r(M) per-mille.
	struct Row { int M; uint32_t m; bool bG; int l, Dx; };
	const Row Rows[] = {{2, 0, false, 18, 2}, {4, 0, false, 12, 2}, {8, 0, false, 8, 3}, {2, 0, true, 18, 2}, {4, 0, true, 12, 3}, {8, 0, true, 8, 4},
	                    {2, 150, false, 16, 2}, {4, 150, false, 11, 3}, {8, 150, false, 6, 4}, {2, 150, true, 16, 2}, {4, 150, true, 11, 3}, {8, 150, true, 6, 0}};
	for (const Row& R : Rows)
	{
		FChooserInput In = Default();
		const uint32_t Rr[8] = {1000, 1275, 1855, 1855, 3000, 3000, 3000, 3000};
		const uint32_t G25[8] = {250, 319, 464, 464, 750, 750, 750, 750};
		std::memcpy(In.CostRatioPerMille, Rr, sizeof Rr);
		if (R.bG) std::memcpy(In.LogitsRatioPerMille, G25, sizeof G25);
		else std::memset(In.LogitsRatioPerMille, 0, sizeof In.LogitsRatioPerMille);
		In.GpuRowsMarginPerMille = R.m;
		const FChooserPlan P = ChooseRowsPlan(In);
		EXPECT_EQ(P.LayersPerSlice[R.M - 1], R.l);
		EXPECT_EQ(P.ExactTicks[R.M - 1], R.Dx);
	}
}

HOST_TEST(chooser_section_5_2_and_5_3_worked_figures, "10.6 (§5.2: M_max 8, D 4 at m = 150; D(8) 3 at m = 0. §5.3: 8x1 at 4 units; the m = 0 tie to 4x2)")
{
	FChooserInput In = Default();
	const uint32_t Rr[8] = {1000, 1275, 1855, 1855, 3000, 3000, 3000, 3000};
	std::memcpy(In.CostRatioPerMille, Rr, sizeof Rr);
	FChooserPlan P = ChooseRowsPlan(In);
	EXPECT_EQ(P.MMax, 8);
	EXPECT_EQ(P.Ticks[7], 4);
	EXPECT_EQ(P.ComposedRows, 8);
	EXPECT_EQ(P.ComposedLayers, 1);
	EXPECT_EQ(P.ComposedUnits, 4);
	In.GpuRowsMarginPerMille = 0;
	P = ChooseRowsPlan(In);
	EXPECT_EQ(P.Ticks[7], 3);
	EXPECT_EQ(P.ComposedRows, 4);  // 8 row-layers either way: the tie goes to fewer rows
	EXPECT_EQ(P.ComposedLayers, 2);
	EXPECT_EQ(P.ComposedUnits, 4);
}

// The suite's own model equals the reference, and each of the plan's chooser mutants is visible on the grid.
HOST_TEST(chooser_suite_model_equals_reference, "10.6 (a second reading of the plan agrees with chooser_ref.py)")
{
	REQUIRE_GT(Grid().size(), static_cast<size_t>(0));
	std::string First;
	EXPECT_EQ(GridMisses([](const FChooserInput& In) { return ModelPlan(In, EMutant::None); }, &First), 0);
	if (!First.empty()) std::printf("       %s\n", First.c_str());
}
HOST_TEST(chooser_grid_kills_mx13_mx13b_mx22, "10.6 vitality: MX13, MX13b, MX22")
{
	REQUIRE_GT(Grid().size(), static_cast<size_t>(0));
	for (EMutant Mu : {EMutant::MX13, EMutant::MX13b, EMutant::MX22})
	{
		const int Misses = GridMisses([Mu](const FChooserInput& In) { return ModelPlan(In, Mu); }, nullptr);
		std::printf("       mutant %d: %d grid cases differ\n", static_cast<int>(Mu), Misses);
		EXPECT_GT(Misses, 0);
	}
}

// ------------------------------------------------------------------------------------------------ 7.11
HOST_TEST(exact_setting_runs_where_m_over_dx_is_at_most_one, "7.11 (a); MR-D11")
{
	FChooserInput In = Default();
	// m = 700 at one row's budget: under ruling 42 R2 only M = 4 in 4..8 has a runnable Exact schedule, D_x = 5
	// (chooser_ref: D_x = 0,3,4,5,0,0,0,0; M = 5 needs a logits-only tick, which the engine cannot run). So no fitting M
	// makes more than one token per tick, and Exact still runs, at M_exact = 4.
	// m = 2000 fits no M in 4..8 at all: that is Q4, not 7.11 (a).
	In.GpuRowsMarginPerMille = 700;
	const FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_TRUE(P.bExactOn);
	EXPECT_EQ(P.ExactReason, EExactReason::Runs);
	REQUIRE_GE(P.MExact, MinExactRows);
	for (int M = MinExactRows; M <= MaxRows; ++M)
		if (P.ExactTicks[M - 1] > 0) EXPECT_LE(M, P.ExactTicks[M - 1]);  // the case is the one 7.11 (a) names
	EXPECT_EQ(P.ExactTicks[3], 5);
	EXPECT_EQ(P.ExactTicks[4], 0);
	EXPECT_EQ(P.MExact, 4);
}

HOST_TEST(exact_setting_without_device_head_runs_as_off, "7.11 (b); ruling 26 Q2")
{
	FChooserInput In = Default();
	In.bDeviceLogitsHead = false;
	const FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_TRUE(!P.bExactOn);
	EXPECT_EQ(P.ExactReason, EExactReason::NoDeviceHead);
	EXPECT_EQ(P.MExact, 0);
	// The rest of the plan is the setting-off plan's.
	In.bExactChosen = false;
	FChooserPlan Off = ChooseRowsPlan(In);
	Off.ExactReason = EExactReason::NoDeviceHead;
	EXPECT_TRUE(SamePlan(P, Off));
}

HOST_TEST(exact_setting_off_plans_nothing_of_exact, "7.11 (c)")
{
	FChooserInput In = Default();
	In.bExactChosen = false;
	const FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_TRUE(!P.bExactOn);
	EXPECT_EQ(P.ExactReason, EExactReason::NotChosen);
	EXPECT_EQ(P.MExact, 0);
	// Turning Exact on changes nothing but the Exact fields (M_max and the Composed choice are Off's).
	In.bExactChosen = true;
	const FChooserPlan On = ChooseRowsPlan(In);
	EXPECT_EQ(On.MMax, P.MMax);
	EXPECT_EQ(On.ComposedRows, P.ComposedRows);
	EXPECT_EQ(On.ComposedLayers, P.ComposedLayers);
}

// ------------------------------------------------------------------------------------------------ N4 (ruling 39 Q5), P4.4
HOST_TEST(table_provenance_never_reports_an_estimate_as_measured, "N4 (ruling 39 Q5)")
{
	FChooserInput In = Default();
	In.bTableMeasured = true;
	const FChooserPlan Meas = ChooseRowsPlan(In);
	EXPECT_EQ(Meas.TableStatus, ETableStatus::Measured);
	EXPECT_TRUE(Meas.bMultiRowOn);
	In.bTableMeasured = false;
	In.bShippingBuild = false;
	const FChooserPlan Dev = ChooseRowsPlan(In);
	EXPECT_TRUE(Dev.bValid);
	EXPECT_EQ(Dev.TableStatus, ETableStatus::EstimatedFlagged);  // a development build runs multi-row, and says so
	EXPECT_TRUE(Dev.bMultiRowOn);
	EXPECT_EQ(Dev.MMax, Meas.MMax);  // the flag changes nothing of the plan
	In.bShippingBuild = true;
	const FChooserPlan Ship = ChooseRowsPlan(In);
	EXPECT_TRUE(Ship.bValid);
	EXPECT_EQ(Ship.TableStatus, ETableStatus::EstimatedOneRow);  // a shipping build falls back to one-row scheduling
	EXPECT_TRUE(!Ship.bMultiRowOn);
	EXPECT_EQ(Ship.MMax, 1);
	EXPECT_EQ(Ship.ComposedRows, 1);
	EXPECT_TRUE(!Ship.bExactOn);
	EXPECT_EQ(Ship.MExact, 0);
}

HOST_TEST(multirow_switch_off_is_the_one_row_plan, "P4.4's chooser half (bGpuMultiRow = false: 1.17's scheduling; ruling 39 Q9)")
{
	for (double B : {0.0, 2.0, 11.5, 16.7})
		for (uint32_t Lpt : {1u, 4u, 8u})
		{
			FChooserInput In = Default();
			In.FrameBudgetMs = B;
			In.LayersPerTick = Lpt;
			In.bGpuMultiRow = false;
			const FChooserPlan P = ChooseRowsPlan(In);
			REQUIRE_TRUE(P.bValid);
			EXPECT_TRUE(!P.bMultiRowOn);
			EXPECT_EQ(P.TableStatus, ETableStatus::Measured);
			EXPECT_EQ(P.MMax, 1);
			EXPECT_EQ(P.MMaxLayers, P.LayersPerSlice[0]);
			EXPECT_EQ(P.ComposedRows, 1);
			EXPECT_EQ(P.ComposedLayers, P.ComposedUnits);  // one-row layers are one unit each
			EXPECT_LE(P.ComposedLayers, static_cast<int32_t>(Lpt));
			EXPECT_TRUE(!P.bExactOn);
			// Knob 0: the whole one-row token per OneCall action, as 1.17.
			if (B == 0.0) EXPECT_EQ(P.Ticks[0], 1);
		}
}

// ------------------------------------------------------------------------------------------------ ruling 39 Q1, Q2, Q4
HOST_TEST(scene_share_comes_out_of_every_slice, "ruling 39 Q1 (l(M) against B_eff - SceneGpuShareMs)")
{
	FChooserInput In = Default();
	In.GpuRowsMarginPerMille = 0;
	In.FrameBudgetMs = 11.5;
	const FChooserPlan NoScene = ChooseRowsPlan(In);
	In.SceneGpuShareMs = 1.5;
	const FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	// C(4) = ceil(1855 x 6200 / 1000) = 11,501 us; Room = 11,500 - 1,500 = 10,000 us: l(4) = floor(24 x 10,000 / 11,501) = 20.
	EXPECT_EQ(P.CostUs[3], 11501);
	EXPECT_EQ(P.LayersPerSlice[3], 20);
	EXPECT_EQ(NoScene.LayersPerSlice[3], 23);  // floor(24 x 11,500 / 11,501)
	EXPECT_EQ(P.BudgetUs, 11500);  // B_eff itself is unchanged; the scene is priced inside it
}

HOST_TEST(budget_below_one_row_is_valid_and_slices_one_row, "ruling 39 Q2 (10.4's 'config invalid' is a warning)")
{
	FChooserInput In = Default();
	In.FrameBudgetMs = 2.0;  // below C(1) = 6,200 us
	FChooserPlan P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_TRUE(P.bBudgetBelowOneRow);
	EXPECT_EQ(P.LayersPerSlice[0], 6);  // E-14 (ruling 53): floor(24 x 2,000 x 1,000 / (6,200 x 1,150)); 7 before the margin
	EXPECT_EQ(P.Ticks[0], 4);
	// Below the scene's own share: one layer per action, never refused.
	In.FrameBudgetMs = 1.0;
	In.SceneGpuShareMs = 1.5;
	P = ChooseRowsPlan(In);
	REQUIRE_TRUE(P.bValid);
	EXPECT_TRUE(P.bBudgetBelowOneRow);
	EXPECT_EQ(P.LayersPerSlice[0], 1);
	EXPECT_EQ(P.Ticks[0], 24);
	EXPECT_EQ(P.MMax, 1);  // no M >= 2 has room
	EXPECT_EQ(P.ComposedRows, 1);
	EXPECT_EQ(P.ComposedLayers, 1);
	// At or above one row it is not flagged.
	In.SceneGpuShareMs = 0.0;
	In.FrameBudgetMs = 6.2;
	EXPECT_TRUE(!ChooseRowsPlan(In).bBudgetBelowOneRow);
	In.FrameBudgetMs = 0.0;
	EXPECT_TRUE(!ChooseRowsPlan(In).bBudgetBelowOneRow);
}

HOST_TEST(exact_falls_to_three_then_two_rows_then_off, "ruling 39 Q4 (ExactFellBackToOff)")
{
	FChooserInput In = Default();
	In.GpuRowsMarginPerMille = 2000;  // at one row's budget no M in 4..8 fits the logits stage
	const FChooserPlan P = ChooseRowsPlan(In), R = ModelPlan(In, EMutant::None);
	REQUIRE_TRUE(P.bValid);
	for (int M = MinExactRows; M <= MaxRows; ++M) EXPECT_EQ(R.ExactTicks[M - 1], 0);  // the case is the one Q4 names
	EXPECT_EQ(P.MExact, R.MExact);
	EXPECT_EQ(P.ExactReason, R.ExactReason);
	EXPECT_TRUE(P.MExact == 3 || P.MExact == 2 || (P.MExact == 0 && P.ExactReason == EExactReason::FellBackToOff && !P.bExactOn));
	// Nothing fits at all (B below the scene): Off, with the reason.
	In.FrameBudgetMs = 1.0;
	In.SceneGpuShareMs = 1.5;
	const FChooserPlan Q = ChooseRowsPlan(In);
	REQUIRE_TRUE(Q.bValid);
	EXPECT_TRUE(!Q.bExactOn);
	EXPECT_EQ(Q.ExactReason, EExactReason::FellBackToOff);
	EXPECT_EQ(Q.MExact, 0);
}

// ------------------------------------------------------------------------------------------------ erratum E-14
HOST_TEST(chooser_e14_plan_at_the_configured_price_is_the_plan, "E-14 (ChooseRowsPlanAt(In, CostMicros(GpuRowCostMs)) == the grid)")
{
	REQUIRE_GT(Grid().size(), static_cast<size_t>(0));
	std::string First;
	const int Misses = GridMisses([](const FChooserInput& In) { return ChooseRowsPlanAt(In, CostMicros(In.GpuRowCostMs)); }, &First);
	if (Misses) std::printf("       %d of %zu grid cases differ; first: %s\n", Misses, Grid().size(), First.c_str());
	EXPECT_EQ(Misses, 0);
}

HOST_TEST(chooser_e14_plan_at_a_position_price, "E-14 (a slice priced at its end position: the plan at C(1, pos), as if GpuRowCostMs were that price)")
{
	int Differ = 0;
	for (const FCase& C : Grid())
	{
		for (const int64_t Us : {int64_t{6967}, int64_t{8200}, int64_t{27086}})
		{
			FChooserInput At = C.In;
			At.GpuRowCostMs = static_cast<double>(Us) / 1000.0;
			if (!SamePlan(ChooseRowsPlanAt(C.In, Us), ModelPlan(At, EMutant::None))) ++Differ;
		}
	}
	EXPECT_EQ(Differ, 0);
	// Worked by hand: B = 2 ms, m = 150, L = 24, no scene: l(1) = floor(48,000,000 / (C x 1.15)).
	FChooserInput In = Default();
	In.FrameBudgetMs = 2.0;
	In.GpuRowsMarginPerMille = 150;
	EXPECT_EQ(ChooseRowsPlanAt(In, 6200).LayersPerSlice[0], 6);
	EXPECT_EQ(ChooseRowsPlanAt(In, 6967).LayersPerSlice[0], 5);
	EXPECT_EQ(ChooseRowsPlanAt(In, 8200).LayersPerSlice[0], 5);
	EXPECT_EQ(ChooseRowsPlanAt(In, 9000).LayersPerSlice[0], 4);
	EXPECT_EQ(ChooseRowsPlanAt(In, 5000).LayersPerSlice[0], 8);
	// The knob at 0: the whole token at any price.
	In.FrameBudgetMs = 0.0;
	EXPECT_EQ(ChooseRowsPlanAt(In, 27086).LayersPerSlice[0], 24);
}

HOST_TEST(chooser_finish_on_the_slice_that_ends_a_token, "ruling 56 (1) (l x (C - F) / L + F, the margin on the whole; F = g(1) x C(1) / 1000 when not measured)")
{
	// Worked by hand. The measured table's g(1) = 48 per mille; B = 2 ms, m = 150, L = 24, no scene: Room x L x 1000 =
	// 48,000,000.
	FChooserInput In = Default();
	In.LogitsRatioPerMille[0] = 48;
	In.FrameBudgetMs = 2.0;
	In.GpuRowsMarginPerMille = 150;
	// F with none measured: ceil(48 x C / 1000).
	EXPECT_EQ(DefaultFinishUs(In, 7000), 336);   // exact
	EXPECT_EQ(DefaultFinishUs(In, 6200), 298);   // 297.6 rounds up
	EXPECT_EQ(DefaultFinishUs(In, 7300), 351);   // 350.4 rounds up
	// C = 7,000, F = 400: l = 4 is (4 x 6,600 + 400 x 24) x 1,150 = 41,400,000; l = 5 is 48,990,000.
	EXPECT_EQ(FinishSliceScaledPrice(In, 7000, 400, 4), 41400000);
	EXPECT_EQ(FinishSliceScaledPrice(In, 7000, 400, 5), 48990000);
	EXPECT_EQ(FinishSliceLayers(In, 7000, 400), 4);
	EXPECT_EQ(ChooseRowsPlanAt(In, 7000).LayersPerSlice[0], 5);  // a slice that does not end a token: l(1)
	// C = 6,200 and 7,300 with F their default (298, 351): 5 and 4 (l(1) 6 and 5).
	EXPECT_EQ(FinishSliceLayers(In, 6200, 298), 5);
	EXPECT_EQ(FinishSliceLayers(In, 7300, 351), 4);
	// The finish alone nearly fills the room: one layer is always admitted.
	EXPECT_EQ(FinishSliceLayers(In, 10900, 1500), 1);
	// Exactly at the room: (l x (C - F) + F x L) x 1,150 = 48,000,000 fits. C = 2,300, F = 100: l x 2,200 x 1,150 +
	// 2,760,000 <= 48,000,000 gives l <= 17.88: 17 (and 24 is the cap).
	EXPECT_EQ(FinishSliceLayers(In, 2300, 100), 17);
	// The knob at 0: no margin, Room = C(1): the whole token.
	In.FrameBudgetMs = 0.0;
	EXPECT_EQ(FinishSliceScaledPrice(In, 7000, 400, 4), 36000000);
	EXPECT_EQ(FinishSliceLayers(In, 7000, 400), 24);
	EXPECT_EQ(FinishSliceLayers(In, 27086, 1300), 24);
}
