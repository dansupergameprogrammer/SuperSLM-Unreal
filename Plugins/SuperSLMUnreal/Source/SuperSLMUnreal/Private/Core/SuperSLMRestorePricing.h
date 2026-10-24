#pragma once

// The restore's price by bytes (precache plan §5.5, risk R5; step U4). Plain C++, no engine and no
// Unreal types, so the host suite tests it (ci/host/prefix_cache, test_restore_pricing.cpp).
//
// A cache restore is priced as one fixed cost per restore plus a cost per MiB of its payload:
//   planned ms = RestoreCostMs + RestoreCostPerMiBMs x (payload bytes / 2^20)
// RestoreCostPerMiBMs 0 (the default, and every profile measured before U4) is today's flat price.
//
// SuperSLM.EstimateCosts measures the term. Its existing restore arm restores an empty sequence's blob, which is
// the fixed cost (RestoreCostMs, unchanged). Its per-MiB arm restores the blob saved at each depth the other
// arms use, leaving out any blob under 1 MiB (kRestoreFitMinBytes: depth 1's blob is a few dozen KiB, so its
// median noise over its size would set the slope). The slope is fitted from the smallest and the largest of the
// remaining readings:
//   max(0, (median_largest - median_smallest) / (MiB_largest - MiB_smallest))
// the incremental cost per MiB across the measured range, floored at 0. With one remaining size it is the slope of
// that size's slowest reading through the fixed cost, max(0, (median - RestoreCostMs) / MiB). Plan erratum
// 15.26 records the choice: a two-point fit over the large blobs, not the steepest reading, which an upper
// envelope over every depth would take and which overprices every large restore.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace SuperSLMCore
{
	constexpr double kBytesPerMiB = 1024.0 * 1024.0;

	// The planned cost of one restore. A non-finite or negative input is the caller's to refuse
	// (Configure() refuses both); here it returns the fixed cost unchanged so a bad slope never lowers it.
	inline double RestorePlannedMs(double PerRestoreMs, double PerMiBMs, int64_t PayloadBytes)
	{
		if (!(PerMiBMs > 0.0) || !std::isfinite(PerMiBMs) || PayloadBytes <= 0)
		{
			return PerRestoreMs;
		}
		return PerRestoreMs + PerMiBMs * (static_cast<double>(PayloadBytes) / kBytesPerMiB);
	}

	struct FRestoreSizeReading
	{
		int64_t PayloadBytes = 0;
		double MedianMs = 0.0;
	};

	// Blobs under this size are left out of the fit (their median noise over their size would set the slope).
	constexpr int64_t kRestoreFitMinBytes = 1024 * 1024;

	// The per-MiB slope (above). False, with OutSlope 0, when no reading is at or above kRestoreFitMinBytes or any
	// input is non-finite: the estimate then suggests nothing for the term.
	inline bool FitRestorePerMiB(double PerRestoreMs, const FRestoreSizeReading* Readings, size_t Count, double& OutSlope)
	{
		OutSlope = 0.0;
		if (!std::isfinite(PerRestoreMs) || Readings == nullptr)
		{
			return false;
		}
		const FRestoreSizeReading* Smallest = nullptr;
		const FRestoreSizeReading* Largest = nullptr;
		for (size_t I = 0; I < Count; ++I)
		{
			const FRestoreSizeReading& R = Readings[I];
			if (!std::isfinite(R.MedianMs))
			{
				return false;
			}
			if (R.PayloadBytes < kRestoreFitMinBytes)
			{
				continue;
			}
			if (Smallest == nullptr || R.PayloadBytes < Smallest->PayloadBytes)
			{
				Smallest = &R;
			}
			if (Largest == nullptr || R.PayloadBytes > Largest->PayloadBytes)
			{
				Largest = &R;
			}
		}
		if (Smallest == nullptr)
		{
			return false;
		}
		double Slope = 0.0;
		if (Largest->PayloadBytes > Smallest->PayloadBytes)
		{
			Slope = (Largest->MedianMs - Smallest->MedianMs)
				/ (static_cast<double>(Largest->PayloadBytes - Smallest->PayloadBytes) / kBytesPerMiB);
		}
		else
		{
			// One size: its slowest reading's slope through the fixed cost.
			double Median = Largest->MedianMs;
			for (size_t I = 0; I < Count; ++I)
			{
				if (Readings[I].PayloadBytes == Largest->PayloadBytes && Readings[I].MedianMs > Median)
				{
					Median = Readings[I].MedianMs;
				}
			}
			Slope = (Median - PerRestoreMs) / (static_cast<double>(Largest->PayloadBytes) / kBytesPerMiB);
		}
		if (!std::isfinite(Slope))
		{
			return false;
		}
		OutSlope = Slope > 0.0 ? Slope : 0.0;
		return true;
	}
}
