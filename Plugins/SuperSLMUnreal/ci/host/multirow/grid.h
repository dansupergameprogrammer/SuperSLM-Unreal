#pragma once

// The 10.6 grid (grid.txt, written by chooser_ref.py): each case's chooser input and the reference's expected plan.

#include "SuperSLMGpuRowsChooser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MultiRowHost
{
	struct FExpected
	{
		int32_t Status = 0, MultiRowOn = 0, BelowOneRow = 0;
		int64_t RowCostUs = 0, SceneUs = 0, BudgetUs = 0;
		int64_t CostUs[8] = {}, LogitsUs[8] = {};
		int32_t LayersPerSlice[8] = {}, Ticks[8] = {}, ExactTicks[8] = {};
		int32_t MMax = 0, MMaxLayers = 0, ComposedRows = 0, ComposedLayers = 0, ComposedUnits = 0;
		int32_t ExactOn = 0, ExactReason = 0, MExact = 0;  // ExactReason -1: not graded (Exact chosen under the one-row plan)
	};

	struct FCase
	{
		std::string Table;
		SuperSLMCore::GpuRows::FChooserInput In;
		FExpected Want;
		int Line = 0;
	};

	// Reads GRID_PATH (a compile definition). Empty on a read or parse failure (the caller requires a non-empty grid).
	std::vector<FCase> LoadGrid(std::string* Error);

	// Compares a plan with the expectation; appends one line per differing field to Diffs. Returns true when equal. A
	// one-row plan (MultiRowOn 0) is graded on the fields that plan has: M = 1's, M_max, Composed and Exact's.
	bool PlanMatches(const SuperSLMCore::GpuRows::FChooserPlan& Plan, const FExpected& Want, std::vector<std::string>* Diffs);
}
