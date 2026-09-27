#include "SuperSLMSequenceLifecycleBudget.h"

#include "SuperSLMLifecycleMeasure.h"
#include "SuperSLMModel.h"
#include "SuperSLMRuntimeRegistry.h"

#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"

#include "superslm/sslm_abi.h"

namespace
{
	// The measurement buffer is larger than any last-level cache this plugin targets (the
	// 7950X3D's 96 MiB V-Cache CCD is the largest on the project's hardware map), so the figure
	// is DRAM bandwidth -- the conservative reading for a KV block, which a reset reaches after
	// the decode that last wrote it may have been evicted. A block that happens to stay cached
	// resets faster than predicted, never slower.
	constexpr SIZE_T kMeasureBytes = 128ull * 1024 * 1024;
	constexpr int32 kTimedPasses = 5;

	double Median(TArray<double>& Values)
	{
		Values.Sort();
		return Values[Values.Num() / 2];
	}
}

namespace SuperSLMLifecycleMeasure
{
	FBandwidths MeasureHostBandwidths()
	{
		FBandwidths Out;

		// Write bandwidth: memset over the whole buffer, which is exactly what sslm_seq_reset
		// does to a KV block (src/sslm_abi.cpp: `std::memset(seq->kv_block, 0, seq->block_size)`).
		uint8* Write = static_cast<uint8*>(FMemory::Malloc(kMeasureBytes, SSLM_ABI_ALIGNMENT_BYTES));
		if (Write == nullptr)
		{
			return Out;
		}
		FMemory::Memset(Write, 0x5A, kMeasureBytes); // first touch commits the pages; untimed
		TArray<double> WriteSeconds;
		for (int32 Pass = 0; Pass < kTimedPasses; ++Pass)
		{
			const double Start = FPlatformTime::Seconds();
			FMemory::Memset(Write, static_cast<uint8>(Pass), kMeasureBytes);
			WriteSeconds.Add(FPlatformTime::Seconds() - Start);
		}

		// Copy bandwidth: memcpy of half the buffer onto the other half, the shape of
		// sslm_seq_adopt_prefix's copy-on-adopt (a whole-block copy from the frozen prefix's
		// block into the adopting sequence's block, src/sslm_abi.cpp).
		const SIZE_T Half = kMeasureBytes / 2;
		TArray<double> CopySeconds;
		for (int32 Pass = 0; Pass < kTimedPasses; ++Pass)
		{
			const double Start = FPlatformTime::Seconds();
			FMemory::Memcpy(Write + Half, Write, Half);
			CopySeconds.Add(FPlatformTime::Seconds() - Start);
		}
		FMemory::Free(Write);

		const double WriteMedian = Median(WriteSeconds);
		const double CopyMedian = Median(CopySeconds);
		if (WriteMedian > 0.0)
		{
			Out.WriteBytesPerSec = static_cast<double>(kMeasureBytes) / WriteMedian;
		}
		if (CopyMedian > 0.0)
		{
			Out.CopyBytesPerSec = static_cast<double>(Half) / CopyMedian;
		}
		return Out;
	}
}

namespace SuperSLMSequenceLifecycleBudget
{
	double MeasureHostWriteBandwidthBytesPerSec()
	{
		return SuperSLMLifecycleMeasure::MeasureHostBandwidths().WriteBytesPerSec;
	}

	void MeasureHostBandwidthAndAdoptRatio(double& OutWriteBytesPerSec, double& OutAdoptToResetRatio)
	{
		const SuperSLMLifecycleMeasure::FBandwidths Bw = SuperSLMLifecycleMeasure::MeasureHostBandwidths();
		OutWriteBytesPerSec = Bw.WriteBytesPerSec;
		OutAdoptToResetRatio = (Bw.CopyBytesPerSec > 0.0) ? Bw.WriteBytesPerSec / Bw.CopyBytesPerSec : 0.0;
	}

	FSuperSLMSequenceLifecycleReport Predict(
		int64 KvBlockSizeBytes,
		double BandwidthBytesPerSec,
		double AdoptToResetRatio,
		double BudgetMs)
	{
		FSuperSLMSequenceLifecycleReport Report;
		Report.KvBlockSizeBytes = KvBlockSizeBytes;
		Report.MeasuredBandwidthBytesPerSec = BandwidthBytesPerSec;
		if (KvBlockSizeBytes <= 0 || BandwidthBytesPerSec <= 0.0 || AdoptToResetRatio <= 0.0)
		{
			// No prediction can be made, so nothing is within budget.
			Report.bWithinBudget = false;
			return Report;
		}
		Report.PredictedResetMs = static_cast<double>(KvBlockSizeBytes) / BandwidthBytesPerSec * 1000.0;
		Report.PredictedAdoptMs = Report.PredictedResetMs * AdoptToResetRatio;
		// §5: import fails "when the prediction exceeds the budget" -- both lifecycle writes the
		// plan prices (reset and adopt) must fit.
		Report.bWithinBudget = Report.PredictedResetMs <= BudgetMs && Report.PredictedAdoptMs <= BudgetMs;
		return Report;
	}

	FSuperSLMSequenceLifecycleReport CheckModel(const USuperSLMModel& Model, double BudgetMs)
	{
		FSuperSLMSequenceLifecycleReport Report;

		// Layer 1's own sizing verb on the model's shared mapping (mapped transiently when no
		// subsystem holds it), so this figure and the subsystem's pool allocation come from the
		// same function.
		FString MapError;
		const sslm_model Mapping = SuperSLMRuntime::AcquireModelMapping(Model, MapError);
		if (Mapping == nullptr)
		{
			return Report;
		}
		const int64 BlockBytes = static_cast<int64>(sslm_kv_block_size(Mapping));
		SuperSLMRuntime::ReleaseModelMapping(Mapping);

		// Adopt's cost relative to reset is measured on this box (copy vs write bandwidth over
		// the same byte count), never carried over from the v1.2.0 figure.
		const SuperSLMLifecycleMeasure::FBandwidths Bw = SuperSLMLifecycleMeasure::MeasureHostBandwidths();
		const double Ratio = (Bw.CopyBytesPerSec > 0.0) ? Bw.WriteBytesPerSec / Bw.CopyBytesPerSec : 0.0;
		return Predict(BlockBytes, Bw.WriteBytesPerSec, Ratio, BudgetMs);
	}
}
