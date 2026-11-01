#pragma once

// Multi-row GPU plan rev 1.3.2, slice MR6 (§5.1, §5.2, §5.3, §5.6; MR-D11; cell 10.6; rulings 37 and 39): the chooser.
// Plugin 1.4 prices M-row chunks from the engine's shipped cost table (ThirdParty/SuperSLM/include/superslm/
// gpu_rows_cost_table.h, engine 1.18: the merged per-M maximum arrays kGpuRowsCostRatioPerMille and
// kGpuRowsLogitsRatioPerMille for every model, ruling 39 Q11) and picks, from the configuration alone (C18), how many
// rows a chunk takes and how many layers a tick runs. It never reads a measured time, so tokens and each action's rows
// never depend on device timing.
//
// THIS FILE IS THE MR6 RED SUITE'S CONTRACT (ci/host/multirow, red-suite-mr6.md): the interface and the rules below are
// the suite's; the bodies are the implementation's (MR6). Add to the interface, never take from it or change a meaning.
//
// The rules (§5.1, integer pricing; every product in int64, every division an exact floor or ceiling):
//   - The configuration's millisecond fields are double (FSuperSLMGpuRuntimeConfig) and convert to microseconds once,
//     by the plan's pinned conversion applied to the double value (ruling 39 Q9): v = Ms x 1000 (in double),
//     n = llround(v); the result is n when |v - n| <= 0.01, else a COST rounds up (ceil v) and a BUDGET rounds down
//     (floor v). Costs: GpuRowCostMs, SceneGpuShareMs. Budgets: SchemaJumpGpuFrameBudgetMs, NominalTickMs.
//   - C(1) = CostMicros(GpuRowCostMs). C(M) = ceil(r(M) x C(1) / 1000), G(M) = ceil(g(M) x C(1) / 1000), with r and g
//     the table's per-mille ratios (index M - 1). m = GpuRowsMarginPerMille.
//   - B_eff = C(1) + Scene when the knob is 0; otherwise min(BudgetMicros(knob), BudgetMicros(NominalTickMs)).
//     Room = B_eff - Scene: the GPU time a tick may carry beyond the scene's share (ruling 39 Q1: the price rule
//     governs, so every l below is computed against Room, never against B_eff).
//   - A tick that runs l layers of an M-row chunk (M >= 2) is priced l x C(M) x (1000 + m) / (L x 1000) + Scene; the
//     tick that carries Exact's logits stage adds G(M) x (1000 + m) / 1000. M = 1 is priced at l x C(1) x (1000 + m) / (L x 1000) with the knob set (E-14), at l x C(1) / L without.
//     A tick fits when its price <= B_eff, compared exactly (cross-multiplied, no rounding).
//   - OneCall, M >= 2: l(M) is the largest l <= L whose tick fits (0: M not admissible); D(M) = ceil(L / l(M)).
//   - OneCall, M = 1 (ruling 39 Q2; erratum E-14, ruling 53): with the knob set, l(1) = min(L, max(1,
//     floor(L x Room x 1000 / (C(1) x (1000 + m))))), the margin M >= 2 carries; with the knob at 0,
//     l(1) = min(L, max(1, floor(L x Room / C(1)))) (= L: 1.17's whole token, P4.4). D(1) = ceil(L / l(1)). One layer
//     per action is always admitted, so a budget below one row (Room < C(1), Room <= 0 included) is never refused:
//     the plan is valid and bBudgetBelowOneRow says so (the plugin logs a warning; 10.4's "config invalid" is that
//     warning). With the knob at 0, Room = C(1), so l(1) = L and D(1) = 1.
//   - M_max maximizes M / D(M) over admissible M in 1..8, ties to fewer rows.
//   - Exact, OneCall only (ruling 39 Q3; on the Composed path the verification runs at this same M_exact): D_x(M) is
//     D(M) when the last slice's tick, L - (D(M) - 1) x l(M) layers, also fits the logits stage; else D(M) + 1 when
//     one layer and the logits stage fit a tick (ruling 42 R2: the engine runs the logits stage in the ready that
//     finishes the last layer, so a tick of logits alone cannot run; the plugin then cuts the last slice to the most
//     layers that fit with it); else 0. M_exact maximizes M / D_x(M) over M in 4..8 with
//     D_x > 0 (MR-D11), ties to fewer rows, whatever the maximum is (it may be <= 1.0: Exact is a setting). When no M
//     in 4..8 has D_x > 0, M_exact is 3 if D_x(3) > 0, else 2 if D_x(2) > 0 (ruling 39 Q4); else Exact runs as Off
//     with ExactReason FellBackToOff (the plugin counts ExactFellBackToOff and logs once per session).
//   - Composed: a slice of l layers of M rows uses ceil(l x C(M) x (1000 + m) / (C(1) x 1000)) one-row-layer units
//     (M = 1: l units). The chosen (M, l) maximizes the row-layers M x l per tick among slices of at most
//     LayersPerTick units, l <= L, ties to fewer rows; with the knob set, M >= 2 also needs a fitting price and M = 1
//     takes l <= l(1).
//   - Multi-row off (bGpuMultiRow false, P4.4; or an estimated table in a shipping build, N4): the one-row plan.
//     MMax = 1 with l(1) and D(1) as above, Composed (1, min(LayersPerTick, l(1) or L), that many units), nothing of
//     Exact (bExactOn false, MExact 0). bMultiRowOn is false. The fields of M >= 2 are not part of that plan.
//
//   - The finish (ruling 56 (1)): the one-row slice that ends a token also runs the logits finish. Its price, x L x 1000,
//     is (l x (C(1) - F) + F x L) x (1000 + m) with the knob set (the margin on the whole), x 1000 without: l layers
//     at (C(1) - F) / L each, plus F. F is phase 1's measured finish (the max of its timed finishes); with none measured
//     (a game override, the default, the fallback), DefaultFinishUs = ceil(g(1) x C(1) / 1000). FinishSliceLayers is
//     the largest l <= L whose price fits Room x L x 1000 (Room as above, at that C(1)); one layer is always admitted,
//     as for l(1). With the knob at 0 (Room = C(1), no margin) it is L whenever F <= C(1).
//   - Position (erratum E-14, ruling 53): ChooseRowsPlanAt(In, RowCostUs) is the same plan with C(1) = RowCostUs (a
//     whole microsecond, already a cost: no conversion) in place of CostMicros(GpuRowCostMs); every other rule above is
//     unchanged, B_eff = C(1) + Scene with the knob at 0 included. The subsystem prices each slice at the sequence's
//     position at the slice's END (an M-row chunk's last row): C(1, pos) from SuperSLMGpuRowCostCalibration.h's line,
//     the per-M ratios on top. ChooseRowsPlan(In) == ChooseRowsPlanAt(In, CostMicros(In.GpuRowCostMs)).
//
// What the chooser reports and never does:
//   - Exact runs when chosen and the model has a device-resident logits head; without one it runs as Off with the
//     reason given (7.11 (b)); with the setting off, nothing of Exact is planned (7.11 (c)).
//   - The table's provenance (N4; ruling 39 Q5): a measured table is Measured. An estimated table is EstimatedFlagged
//     in a development build (the plan is the multi-row plan, logged and traced) and EstimatedOneRow in a shipping
//     build (UE_BUILD_SHIPPING; the one-row plan, logged once). It is never reported as measured.
//   - The host finish (rulings 37, 39 Q6): report-only, the trace counter HostFinishUs (max per close). It is not an
//     input here and never part of a tick's GPU price.
//
// Build constraints (it compiles inside Unreal and standalone with g++): C++17, header-only, standard library only,
// no floating point after the conversion, no exceptions, no RTTI.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SuperSLMCore
{
	namespace GpuRows
	{
		constexpr int32_t MaxRows = 8;  // SSLM_GPU_MAX_ROWS
		constexpr int32_t MinExactRows = 4;  // MR-D11: M_exact is taken over 4..8 first (then 3, then 2: ruling 39 Q4)
		constexpr int32_t DefaultMarginPerMille = 150;  // §5.1's default m

		enum class EPath : uint8_t
		{
			OneCall = 0,
			Composed = 1,
		};

		// N4 (ruling 39 Q5): the table's provenance and what the plan does with it.
		enum class ETableStatus : uint8_t
		{
			Measured = 0,          // the vendored table says measured
			EstimatedFlagged = 1,  // estimated, development build: the multi-row plan, flagged (log + trace)
			EstimatedOneRow = 2,   // estimated, shipping build: the one-row plan (multi-row off), logged once
		};

		// 7.11 and ruling 39 Q4: why Exact runs or not.
		enum class EExactReason : uint8_t
		{
			NotChosen = 0,      // the setting is off: nothing of Exact
			Runs = 1,           // chosen, a device-resident logits head, and an M in 4..8 (or 3, or 2) fits
			NoDeviceHead = 2,   // chosen, no device logits head: runs as Off, the reason logged
			FellBackToOff = 3,  // chosen, a head, but no M in 2..8 fits the logits stage: runs as Off (ExactFellBackToOff)
		};

		namespace Detail
		{
			// |Ms x 1000| is clamped here so that every later int64 product stays in range (Configure refuses such
			// values long before; the clamp only keeps the conversion defined).
			constexpr double MaxMicros = 1.0e12;

			inline int64_t ToMicros(double Ms, bool bCost)
			{
				if (!(Ms == Ms))
				{
					return 0;  // NaN: Configure refuses it; the conversion stays defined
				}
				double V = Ms * 1000.0;
				V = V > MaxMicros ? MaxMicros : (V < -MaxMicros ? -MaxMicros : V);
				const long long N = std::llround(V);
				if (std::fabs(V - static_cast<double>(N)) <= 0.01)
				{
					return static_cast<int64_t>(N);
				}
				return bCost ? static_cast<int64_t>(std::ceil(V)) : static_cast<int64_t>(std::floor(V));
			}

			inline int64_t CeilDiv(int64_t A, int64_t B)
			{
				return A >= 0 ? (A + B - 1) / B : -((-A) / B);
			}

			inline int64_t FloorDiv(int64_t A, int64_t B)
			{
				return A >= 0 ? A / B : -((-A + B - 1) / B);
			}
		}

		// The pinned ms -> us conversion (§5.1, rev 1.3 F4; ruling 39 Q9: applied to the double value).
		inline int64_t CostMicros(double Ms)
		{
			return Detail::ToMicros(Ms, true);
		}
		inline int64_t BudgetMicros(double Ms)
		{
			return Detail::ToMicros(Ms, false);
		}

		struct FChooserInput
		{
			// The table (gpu_rows_cost_table.h's merged arrays): r(M), g(M) per-mille of C(1), index M - 1, and its
			// provenance (kGpuRowsCostTableMeasured).
			uint32_t CostRatioPerMille[MaxRows] = {};
			uint32_t LogitsRatioPerMille[MaxRows] = {};
			bool bTableMeasured = false;
			// The configuration (FSuperSLMGpuRuntimeConfig), in its own units and types.
			double GpuRowCostMs = 6.2;
			double FrameBudgetMs = 0.0;  // SchemaJumpGpuFrameBudgetMs; 0 = one row
			double SceneGpuShareMs = 0.0;
			double NominalTickMs = 16.7;
			int32_t GpuRowsMarginPerMille = DefaultMarginPerMille;  // m
			bool bGpuMultiRow = true;
			uint32_t Layers = 24;           // L
			uint32_t LayersPerTick = 4;     // Composed capacity, floor(DispatchBudget / DispatchesPerLayer)
			uint32_t DispatchesPerLayer = 24;
			EPath Path = EPath::OneCall;
			// The setting and the model.
			bool bExactChosen = false;
			bool bDeviceLogitsHead = true;
			// N4: UE_BUILD_SHIPPING.
			bool bShippingBuild = false;
		};

		struct FChooserPlan
		{
			bool bValid = false;  // false only for a stub or an input outside the types' meaning (never for a budget)
			ETableStatus TableStatus = ETableStatus::EstimatedOneRow;
			bool bMultiRowOn = false;        // false: the one-row plan (bGpuMultiRow false, or EstimatedOneRow)
			bool bBudgetBelowOneRow = false;  // Room < C(1): valid, warned (ruling 39 Q2)
			int64_t RowCostUs = 0;  // C(1)
			int64_t SceneUs = 0;
			int64_t BudgetUs = 0;   // B_eff
			int64_t CostUs[MaxRows] = {};    // C(M)
			int64_t LogitsUs[MaxRows] = {};  // G(M)
			// OneCall, per M (index M - 1): l(M) (0: not admissible), D(M), D_x(M) (0: not admissible, or M = 1).
			int32_t LayersPerSlice[MaxRows] = {};
			int32_t Ticks[MaxRows] = {};
			int32_t ExactTicks[MaxRows] = {};
			// OneCall's choice.
			int32_t MMax = 0;
			int32_t MMaxLayers = 0;  // l(M_max)
			// Composed's choice: rows, layers per slice and the units the slice uses.
			int32_t ComposedRows = 0;
			int32_t ComposedLayers = 0;
			int32_t ComposedUnits = 0;
			// Exact.
			bool bExactOn = false;
			EExactReason ExactReason = EExactReason::NotChosen;
			int32_t MExact = 0;
		};

		// Erratum E-14 (ruling 53): the plan at C(1) = RowCostUs (a whole microsecond, already a cost).
		// Run 5 (E14_3, the long-context price): ExtraPerRowUs is the part of RowCostUs that the context grew since the line's
		// anchor (C(1, pos) - C_a, 0 at the anchor). Each row attends over the whole context, so that part scales with the rows,
		// not with the table's short-context ratio: C(M) = ceil(r(M) x (C(1) - E) / 1000) + M x E for M >= 2 (C(1) unchanged,
		// and G(M) is not position-dependent). With E = 0 the plan is exactly as before.
		inline FChooserPlan ChooseRowsPlanAt(const FChooserInput& In, int64_t RowCostUs, int64_t ExtraPerRowUs = 0)
		{
			FChooserPlan P;
			const int64_t L = static_cast<int64_t>(In.Layers);
			const int64_t Lpt = static_cast<int64_t>(In.LayersPerTick);
			const int64_t m = static_cast<int64_t>(In.GpuRowsMarginPerMille);
			const int64_t C1 = RowCostUs;
			const int64_t S = CostMicros(In.SceneGpuShareMs);
			// Outside the types' meaning: no layers, no Composed capacity, a row that costs nothing, a negative scene
			// share or margin. A budget is never one of these (ruling 39 Q2).
			if (L <= 0 || L > 4096 || Lpt <= 0 || C1 <= 0 || S < 0 || m < 0 || m > 1000000)
			{
				return P;
			}
			const bool bKnob = In.FrameBudgetMs != 0.0;
			const int64_t Beff = bKnob ? std::min(BudgetMicros(In.FrameBudgetMs), BudgetMicros(In.NominalTickMs)) : C1 + S;
			const int64_t Room = Beff - S;

			P.bValid = true;
			P.TableStatus = In.bTableMeasured ? ETableStatus::Measured
				: (In.bShippingBuild ? ETableStatus::EstimatedOneRow : ETableStatus::EstimatedFlagged);
			P.bMultiRowOn = In.bGpuMultiRow && P.TableStatus != ETableStatus::EstimatedOneRow;
			P.bBudgetBelowOneRow = Room < C1;
			P.RowCostUs = C1;
			P.SceneUs = S;
			P.BudgetUs = Beff;
			for (int32_t M = 1; M <= MaxRows; ++M)
			{
				const int64_t E = M >= 2 ? std::min<int64_t>(std::max<int64_t>(0, ExtraPerRowUs), C1) : 0;
				P.CostUs[M - 1] = Detail::CeilDiv(static_cast<int64_t>(In.CostRatioPerMille[M - 1]) * (C1 - E), 1000) + static_cast<int64_t>(M) * E;
				P.LogitsUs[M - 1] = Detail::CeilDiv(static_cast<int64_t>(In.LogitsRatioPerMille[M - 1]) * C1, 1000);
			}

			// M >= 2: l layers (plus, when asked, the logits stage) fit when
			// l x C(M) x (1000 + m) [+ G(M) x (1000 + m) x L] <= Room x L x 1000.
			const int64_t Rhs = Room * L * 1000;
			auto Fits = [&](int32_t M, int64_t l, bool bLogits) -> bool {
				int64_t Lhs = l * P.CostUs[M - 1] * (1000 + m);
				if (bLogits)
				{
					Lhs += P.LogitsUs[M - 1] * (1000 + m) * L;
				}
				return Lhs <= Rhs;
			};

			// M = 1 (ruling 39 Q2): one layer per action is always admitted. With the knob set it carries the margin M >= 2
			// carries (E-14, ruling 53a); at B = 0, Room = C(1) and l(1) = L (P4.4).
			const int64_t L1 = std::min<int64_t>(L, std::max<int64_t>(1, bKnob
				? Detail::FloorDiv(L * Room * 1000, C1 * (1000 + m))
				: Detail::FloorDiv(L * Room, C1)));
			P.LayersPerSlice[0] = static_cast<int32_t>(L1);
			P.Ticks[0] = static_cast<int32_t>(Detail::CeilDiv(L, L1));

			for (int32_t M = 2; M <= MaxRows; ++M)
			{
				// The largest l <= L that fits: the price is monotone in l, so it is floor(Rhs / per-layer), clipped.
				const int64_t PerLayer = P.CostUs[M - 1] * (1000 + m);
				int64_t l = 0;
				if (Rhs >= 0)
				{
					l = PerLayer > 0 ? std::min<int64_t>(L, Rhs / PerLayer) : L;
				}
				int64_t D = 0;
				int64_t Dx = 0;
				if (l > 0)
				{
					D = Detail::CeilDiv(L, l);
					const int64_t Last = L - (D - 1) * l;
					if (Fits(M, Last, true))
					{
						Dx = D;
					}
					else if (Fits(M, 1, true))
					{
						Dx = D + 1; // ruling 42 R2: a runnable last tick, at least one layer with the logits stage
					}
				}
				P.LayersPerSlice[M - 1] = static_cast<int32_t>(l);
				P.Ticks[M - 1] = static_cast<int32_t>(D);
				P.ExactTicks[M - 1] = static_cast<int32_t>(Dx);
			}

			// Composed's one-row slice: LayersPerTick units, and with the knob set no more than l(1).
			const int64_t One = std::min<int64_t>(Lpt, bKnob ? L1 : L);
			P.ComposedRows = 1;
			P.ComposedLayers = static_cast<int32_t>(One);
			P.ComposedUnits = static_cast<int32_t>(One);

			if (!P.bMultiRowOn)
			{
				// The one-row plan (P4.4, N4): 1.17's scheduling, nothing of Exact on the multi-row path.
				P.MMax = 1;
				P.MMaxLayers = static_cast<int32_t>(L1);
				P.ExactReason = In.bExactChosen ? EExactReason::FellBackToOff : EExactReason::NotChosen;
				return P;
			}

			// M_max: the largest M / D(M), ties to fewer rows (M / D > B / D_B, cross-multiplied).
			int32_t BestM = 0;
			for (int32_t M = 1; M <= MaxRows; ++M)
			{
				if (P.LayersPerSlice[M - 1] == 0)
				{
					continue;
				}
				if (BestM == 0 || static_cast<int64_t>(M) * P.Ticks[BestM - 1] > static_cast<int64_t>(BestM) * P.Ticks[M - 1])
				{
					BestM = M;
				}
			}
			P.MMax = BestM;
			P.MMaxLayers = P.LayersPerSlice[BestM - 1];

			// Composed: the most row-layers per tick within LayersPerTick units, ties to fewer rows.
			int64_t BestRowLayers = One;
			for (int32_t M = 2; M <= MaxRows; ++M)
			{
				for (int64_t l = 1; l <= L; ++l)
				{
					const int64_t Units = Detail::CeilDiv(l * P.CostUs[M - 1] * (1000 + m), C1 * 1000);
					if (Units > Lpt)
					{
						break;  // units grow with l
					}
					if (bKnob && !Fits(M, l, false))
					{
						break;  // so does the price
					}
					if (static_cast<int64_t>(M) * l > BestRowLayers)
					{
						BestRowLayers = static_cast<int64_t>(M) * l;
						P.ComposedRows = M;
						P.ComposedLayers = static_cast<int32_t>(l);
						P.ComposedUnits = static_cast<int32_t>(Units);
					}
				}
			}

			// Exact (MR-D11; ruling 39 Q3, Q4; 7.11).
			if (!In.bExactChosen)
			{
				P.ExactReason = EExactReason::NotChosen;
			}
			else if (!In.bDeviceLogitsHead)
			{
				P.ExactReason = EExactReason::NoDeviceHead;
			}
			else
			{
				int32_t BestX = 0;
				for (int32_t M = MinExactRows; M <= MaxRows; ++M)
				{
					if (P.ExactTicks[M - 1] == 0)
					{
						continue;
					}
					if (BestX == 0 ||
						static_cast<int64_t>(M) * P.ExactTicks[BestX - 1] > static_cast<int64_t>(BestX) * P.ExactTicks[M - 1])
					{
						BestX = M;
					}
				}
				if (BestX == 0)
				{
					BestX = P.ExactTicks[2] > 0 ? 3 : (P.ExactTicks[1] > 0 ? 2 : 0);
				}
				P.bExactOn = BestX != 0;
				P.ExactReason = BestX != 0 ? EExactReason::Runs : EExactReason::FellBackToOff;
				P.MExact = BestX;
			}
			return P;
		}

		inline FChooserPlan ChooseRowsPlan(const FChooserInput& In)
		{
			return ChooseRowsPlanAt(In, CostMicros(In.GpuRowCostMs));
		}

		// Ruling 56 (1), the suite's red stubs, implemented here: the finish on the slice that ends a token.
		// F with none measured: ceil(g(1) x C(1) / 1000), g(1) = In.LogitsRatioPerMille[0].
		inline int64_t DefaultFinishUs(const FChooserInput& In, int64_t RowCostUs)
		{
			return Detail::CeilDiv(static_cast<int64_t>(In.LogitsRatioPerMille[0]) * RowCostUs, 1000);
		}

		// The price x L x 1000 of an l-layer one-row slice that ends a token, at C(1) = RowCostUs and F = FinishUs.
		inline int64_t FinishSliceScaledPrice(const FChooserInput& In, int64_t RowCostUs, int64_t FinishUs, int64_t Layers)
		{
			const int64_t L = static_cast<int64_t>(In.Layers);
			const int64_t Scale = In.FrameBudgetMs != 0.0 ? 1000 + static_cast<int64_t>(In.GpuRowsMarginPerMille) : 1000;
			return (Layers * (RowCostUs - FinishUs) + FinishUs * L) * Scale;
		}

		// The most layers (1..L) the slice that ends a token may run beside the finish within Room.
		inline int32_t FinishSliceLayers(const FChooserInput& In, int64_t RowCostUs, int64_t FinishUs)
		{
			const int64_t L = static_cast<int64_t>(In.Layers);
			if (L <= 0)
			{
				return 0;
			}
			const int64_t S = CostMicros(In.SceneGpuShareMs);
			const int64_t Beff = In.FrameBudgetMs != 0.0 ? std::min(BudgetMicros(In.FrameBudgetMs), BudgetMicros(In.NominalTickMs)) : RowCostUs + S;
			const int64_t Rhs = (Beff - S) * L * 1000;
			int64_t Best = 1;  // one layer is always admitted, as for l(1)
			for (int64_t l = 2; l <= L; ++l)
			{
				if (FinishSliceScaledPrice(In, RowCostUs, FinishUs, l) > Rhs)
				{
					break;  // the price grows with l (F <= C(1))
				}
				Best = l;
			}
			return static_cast<int32_t>(Best);
		}
	}
}
