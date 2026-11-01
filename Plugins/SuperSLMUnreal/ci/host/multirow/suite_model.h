#pragma once

// The suite's own model of the chooser, with the plan's mutants as switches (10.6's vitality, before the chooser
// exists): MX13 (the chooser ignores B), MX13b (an M-row layer billed as one unit) and MX22 (computed in float32).
// The unmutated model must equal grid.txt case for case (it is a second, C++ reading of the plan's text, checked
// against chooser_ref.py's), and every mutant must differ from grid.txt somewhere: so the grid can see each mutant
// before the implementation's chooser is written. Once the chooser exists, mutants.sh applies the same three edits to it.
// This file is the suite's, never the plugin's.

#include "SuperSLMGpuRowsChooser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MultiRowHost
{
	enum class EMutant : uint8_t
	{
		None,
		MX13,   // the chooser ignores B: every tick is priced against NominalTickMs
		MX13b,  // an M-row layer billed as one unit (Composed: units = l)
		MX22,   // l computed in float32 from the millisecond inputs
	};

	inline int64_t ModelToUs(double Ms, bool bCost)
	{
		const double V = Ms * 1000.0;
		const long long N = std::llround(V);
		if (std::fabs(V - static_cast<double>(N)) <= 0.01) return N;
		return bCost ? static_cast<int64_t>(std::ceil(V)) : static_cast<int64_t>(std::floor(V));
	}

	inline SuperSLMCore::GpuRows::FChooserPlan ModelPlan(const SuperSLMCore::GpuRows::FChooserInput& In, EMutant Mu)
	{
		using namespace SuperSLMCore::GpuRows;
		FChooserPlan P;
		const int64_t L = In.Layers, m = In.GpuRowsMarginPerMille, Lpt = In.LayersPerTick;
		const bool bKnob = In.FrameBudgetMs != 0.0;
		const int64_t C1 = ModelToUs(In.GpuRowCostMs, true), S = ModelToUs(In.SceneGpuShareMs, true);
		const int64_t Beff = bKnob ? std::min(ModelToUs(In.FrameBudgetMs, false), ModelToUs(In.NominalTickMs, false)) : C1 + S;
		const int64_t B = Mu == EMutant::MX13 ? ModelToUs(In.NominalTickMs, false) : Beff;  // MX13 prices against the tick
		const int64_t Room = B - S;
		P.bValid = true;
		P.TableStatus = In.bTableMeasured ? ETableStatus::Measured : In.bShippingBuild ? ETableStatus::EstimatedOneRow : ETableStatus::EstimatedFlagged;
		P.bMultiRowOn = In.bGpuMultiRow && P.TableStatus != ETableStatus::EstimatedOneRow;
		P.bBudgetBelowOneRow = Beff - S < C1;
		P.RowCostUs = C1;
		P.SceneUs = S;
		P.BudgetUs = Beff;
		for (int M = 1; M <= MaxRows; ++M)
		{
			P.CostUs[M - 1] = (static_cast<int64_t>(In.CostRatioPerMille[M - 1]) * C1 + 999) / 1000;
			P.LogitsUs[M - 1] = (static_cast<int64_t>(In.LogitsRatioPerMille[M - 1]) * C1 + 999) / 1000;
		}
		// l x C(M) x (1000 + m) <= Room x L x 1000 (M >= 2), plus the logits stage when asked.
		auto Fits = [&](int M, int64_t l, bool bLogits) {
			int64_t Lhs = l * P.CostUs[M - 1] * (1000 + m);
			if (bLogits) Lhs += P.LogitsUs[M - 1] * (1000 + m) * L;
			return Lhs <= Room * L * 1000;
		};
		// E-14 (ruling 53): with the knob set, l(1) carries the margin; with it at 0, l(1) = L (1.17's whole token).
		const int64_t L1 = Room <= 0 ? 1
			: bKnob ? std::min<int64_t>(L, std::max<int64_t>(1, L * Room * 1000 / (C1 * (1000 + m))))
			: std::min<int64_t>(L, std::max<int64_t>(1, L * Room / C1));
		P.LayersPerSlice[0] = static_cast<int32_t>(L1);
		P.Ticks[0] = static_cast<int32_t>((L + L1 - 1) / L1);
		for (int M = 2; M <= MaxRows; ++M)
		{
			int64_t l = 0, D = 0, Dx = 0;
			if (Mu == EMutant::MX22)
			{
				const float Rms = static_cast<float>(Room) / 1000.0f, Cms = static_cast<float>(P.CostUs[M - 1]) / 1000.0f;
				const float Lf = static_cast<float>(L) * Rms / (Cms * (1.0f + static_cast<float>(m) / 1000.0f));
				l = std::max<int64_t>(0, std::min<int64_t>(L, static_cast<int64_t>(std::floor(Lf))));
			}
			else
				while (l < L && Fits(M, l + 1, false)) ++l;
			D = l > 0 ? (L + l - 1) / l : 0;
			if (l > 0)
			{
				const int64_t Last = L - (D - 1) * l;
				if (Fits(M, Last, true)) Dx = D;
				else if (Fits(M, 1, true)) Dx = D + 1;  // ruling 42 R2: one layer plus logits, never logits alone
			}
			P.LayersPerSlice[M - 1] = static_cast<int32_t>(l);
			P.Ticks[M - 1] = static_cast<int32_t>(D);
			P.ExactTicks[M - 1] = static_cast<int32_t>(Dx);
		}
		const int64_t One = std::min<int64_t>(Lpt, bKnob ? L1 : L);
		P.ComposedRows = 1;
		P.ComposedLayers = static_cast<int32_t>(One);
		P.ComposedUnits = static_cast<int32_t>(One);
		if (!P.bMultiRowOn)
		{
			P.MMax = 1;
			P.MMaxLayers = static_cast<int32_t>(L1);
			P.ExactReason = In.bExactChosen ? EExactReason::FellBackToOff : EExactReason::NotChosen;  // the reason: not graded
			return P;
		}
		int BestM = 0;
		for (int M = 1; M <= MaxRows; ++M)
		{
			if (P.LayersPerSlice[M - 1] == 0) continue;
			if (BestM == 0 || static_cast<int64_t>(M) * P.Ticks[BestM - 1] > static_cast<int64_t>(BestM) * P.Ticks[M - 1]) BestM = M;
		}
		P.MMax = BestM;
		P.MMaxLayers = P.LayersPerSlice[BestM - 1];
		int64_t BestRl = One;
		for (int M = 2; M <= MaxRows; ++M)
			for (int64_t l = 1; l <= L; ++l)
			{
				const int64_t Units = Mu == EMutant::MX13b ? l : (l * P.CostUs[M - 1] * (1000 + m) + C1 * 1000 - 1) / (C1 * 1000);
				if (Units > Lpt) continue;
				if (bKnob && !Fits(M, l, false)) continue;
				if (M * l > BestRl)
				{
					BestRl = M * l;
					P.ComposedRows = M;
					P.ComposedLayers = static_cast<int32_t>(l);
					P.ComposedUnits = static_cast<int32_t>(Units);
				}
			}
		if (!In.bExactChosen) P.ExactReason = EExactReason::NotChosen;
		else if (!In.bDeviceLogitsHead) P.ExactReason = EExactReason::NoDeviceHead;
		else
		{
			int BestX = 0;
			for (int M = MinExactRows; M <= MaxRows; ++M)
			{
				if (P.ExactTicks[M - 1] == 0) continue;
				if (BestX == 0 || static_cast<int64_t>(M) * P.ExactTicks[BestX - 1] > static_cast<int64_t>(BestX) * P.ExactTicks[M - 1]) BestX = M;
			}
			if (BestX == 0) BestX = P.ExactTicks[2] > 0 ? 3 : P.ExactTicks[1] > 0 ? 2 : 0;
			P.bExactOn = BestX != 0;
			P.ExactReason = BestX ? EExactReason::Runs : EExactReason::FellBackToOff;
			P.MExact = BestX;
		}
		return P;
	}
}
