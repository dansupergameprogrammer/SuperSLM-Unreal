#pragma once

// Multi-row GPU plan, erratum E-13 (plan.md §16; ruling 48): plugin 1.4 measures the GPU row cost C(1) at model load
// and uses it as GpuRowCostMs. This header holds the calibration's pure pieces: the price of an attempt, the sanity
// range, the retry rule and which row cost the chooser takes. The device work (the throwaway sequence, the warm-up,
// the timed tokens, slicing within the frame budget, once per model and adapter, the first generation waiting) is the
// subsystem's; the automation cells R48_* (SuperSLMGpuMultiRowMr6Tests.cpp) grade it.
//
// THIS FILE IS THE MR6 RED SUITE'S CONTRACT (ci/host/multirow/test_calibration.cpp, red-suite-mr6.md): the interface
// and the rules below are the suite's; the bodies are the implementation's. Add to the interface, never take from it or
// change a meaning. Builds alone under -Wconversion -Wsign-conversion -Wundef -fno-exceptions -fno-rtti.
//
// The rules (ruling 48):
//   - An attempt is 1 warm-up token, then 5 timed one-row whole-depth tokens including the logits finish, each timed
//     by the delta of the engine's cumulative GPU-busy counter, in microseconds.
//   - PriceUs: the attempt's price is the maximum of its timed runs rounded up to a multiple of 100 us (0.1 ms). A
//     maximum within 0.01 us of a multiple of 100 is that multiple (the chooser's pinned tolerance: the counter keeps
//     nanoseconds, so 10900.0 us is 10.9 ms, not 11.0). No runs, or a run that is not finite or not above 0: -1 (the
//     attempt failed).
//   - InSanityRange: a price is accepted when 0.2 x 6.2 ms <= price <= 20 x 6.2 ms, that is 1,240 us to 124,000 us,
//     both ends included. The range applies to the price (after rounding).
//   - Step: a failed attempt (price < 0; a timeout, 2 s of wall clock, is a failure: ruling 48a) falls back at once; a price in range is used; a price out of range is noise:
//     the first attempt is retried once, the second falls back.
//   - Choose: an explicit game GpuRowCostMs (> 0; 0 is unset, ruling 48a) wins and skips calibration; with bCalibrateGpuRowCost false the row cost
//     is the 6.2 ms default; otherwise the calibrated price, or the 6.2 ms default when calibration fell back.
//
// Erratum E-14 (ruling 53) in two phases (ruling 54): the price depends on position. These rules add to the ones above;
// where they differ, these govern a calibration (kTimedRuns and kWarmUpRuns stay as E-13's record).
//   - Phase 1: kWarmUpRunsPerPosition (1) warm-up and kTimedRunsPerPosition (3) timed tokens at the early position
//     (pos_a), sliced per ruling 50, priced by PriceUs: C_a. Its step is StepPhase1: E-13's Step on C_a (in range: use;
//     out of range: retry once, then fall back; a failure falls back at once). The first generation waits only for it.
//     Its line is DefaultSlopeLine(C_a, pos_a): slope s_default = 0.0015 x C_a per position (kDefaultSlopeNum /
//     kDefaultSlopeDen = 3 / 2000 of C_a), Source DefaultSlope ("Calibrated (default slope)").
//   - Phase 2, only when B > 0 (RunsPhase2): fill to P2 = LatePosition(ContextCap) = min(256, ContextCap - 8) and time 1
//     warm-up and 3 tokens there: C_b. Lowest priority, only the room a tick has left after the generations' slices,
//     no wall-clock timeout. StepPhase2: a failed late position (C_b < 0), or a measured line out of sanity
//     (LineInSanityRange: C_a in range and C(1, ContextCap) <= 124,000 us), keeps the default slope (warned once by the
//     subsystem); otherwise the measured line LineFrom(C_a, pos_a, C_b, P2) replaces it (slope max(0, ...), Source
//     Measured) and is cached. At B = 0 phase 2 is skipped and the line keeps the default slope.
//   - The line: C(1, pos) = C_a + slope x (pos - pos_a), kept as the exact rational SlopeNumUs / SlopeDenPos (the
//     numerator >= 0; a denominator <= 0 is flat) and evaluated rounded up (a cost): RowCostAtUs = C_a +
//     ceil(SlopeNumUs x (pos - pos_a) / SlopeDenPos). Before pos_a the term is negative and rounds up (toward C_a).
//   - ChooseLine: the game override is a flat line at the game's price (AnchorUs by the chooser's cost conversion,
//     GpuRows::CostMicros: 6.967 ms is 6,967 us); the default and the fallback are flat at 6,200 us; a calibrated line
//     is returned as given with Source Calibrated; the line's own Source names the phase (DefaultSlope, Measured). RowCostMs is the line's anchor in ms. The once-per-model-and-adapter cache keeps C_a and the slope.
//
// Erratum E-16 (ruling 60): every timed set (phase 1's, phase 2's) passes CheckRunSet() before it is priced: a run over
// 3 x the set's median is dropped and re-timed, at most 2 extra runs per set; the price is still PriceUs() of the runs
// kept. A set still holding such a run after its extra runs is a spread failure: that phase is retried once, and only a
// second spread failure (or a consistent cost out of range, as before) is cached.

#include <cmath>
#include <cstdint>

namespace SuperSLMCore
{
	namespace GpuRowCost
	{
		constexpr int64_t kDefaultRowCostUs = 6200;   // 6.2 ms, the fixed default and the fallback
		constexpr int64_t kSanityMinUs = 1240;        // 0.2 x 6.2 ms
		constexpr int64_t kSanityMaxUs = 124000;      // 20 x 6.2 ms
		constexpr int32_t kWarmUpRuns = 1;
		constexpr int32_t kTimedRuns = 5;
		constexpr int32_t kMaxAttempts = 2;           // the first, and one retry

		enum class EStep : uint8_t
		{
			Use = 0,
			Retry = 1,
			FallBack = 2,
		};

		enum class ESource : uint8_t
		{
			Default = 0,    // bCalibrateGpuRowCost false: 6.2 ms
			Game = 1,       // the game set GpuRowCostMs: calibration skipped
			Calibrated = 2, // the measured price (E-14: and the measured slope, phase 2)
			FallBack = 3,   // calibration failed or stayed out of range: 6.2 ms
		};

		// Erratum E-14 (ruling 53, ruling 54): the row cost as a line in position.
		constexpr int32_t kWarmUpRunsPerPosition = 1;
		constexpr int32_t kTimedRunsPerPosition = 3;
		constexpr int64_t kLatePositionMax = 256;
		constexpr int64_t kLatePositionCapMargin = 8;
		constexpr int64_t kDefaultSlopeNum = 3;     // s_default = 3 / 2000 = 0.0015 of C_a per position (ruling 54)
		constexpr int64_t kDefaultSlopeDen = 2000;

		enum class ELineSource : uint8_t
		{
			Flat = 0,         // the game's value, the default, the fallback: slope 0
			DefaultSlope = 1, // phase 1: C_a with s_default
			Measured = 2,     // phase 2: C_a with the measured slope
		};

		struct FRowCostLine
		{
			int64_t AnchorUs = kDefaultRowCostUs; // C_a
			int64_t AnchorPos = 0;                // pos_a
			int64_t SlopeNumUs = 0;               // the slope's numerator (>= 0)
			int64_t SlopeDenPos = 1;              // its denominator, positions (> 0)
			ELineSource Source = ELineSource::Flat;
		};

		enum class EPhase2 : uint8_t
		{
			Use = 0,         // the measured line replaces the default slope
			KeepDefault = 1, // a failed late position, or a measured line out of sanity: s_default stays (warned once)
		};

		struct FChoice
		{
			ESource Source = ESource::Default;
			double RowCostMs = 0.0; // what the chooser takes as GpuRowCostMs
		};

		// E-14: Choose()'s answer with the line.
		struct FLineChoice
		{
			ESource Source = ESource::Default;
			double RowCostMs = 0.0; // C_a in ms: what the chooser takes as GpuRowCostMs
			FRowCostLine Line;
		};

		// E-14 bodies (rulings 53 and 54, the implementation).
		namespace LineDetail
		{
			// The chooser's cost conversion (SuperSLMGpuRowsChooser.h GpuRows::CostMicros, rev 1.3 F4), here so this
			// header stays standalone: v = Ms x 1000, llround when within 0.01 us, else rounded up.
			inline int64_t CostMicros(double Ms)
			{
				if (!(Ms == Ms))
				{
					return 0;
				}
				double V = Ms * 1000.0;
				V = V > 1.0e12 ? 1.0e12 : (V < -1.0e12 ? -1.0e12 : V);
				const long long N = std::llround(V);
				if (std::fabs(V - static_cast<double>(N)) <= 0.01)
				{
					return static_cast<int64_t>(N);
				}
				return static_cast<int64_t>(std::ceil(V));
			}

			inline bool PriceInRange(int64_t PriceUs)
			{
				return PriceUs >= kSanityMinUs && PriceUs <= kSanityMaxUs;
			}
		}

		inline int64_t LatePosition(int64_t ContextCap)
		{
			const int64_t P = ContextCap - kLatePositionCapMargin;
			return P < kLatePositionMax ? P : kLatePositionMax;
		}

		inline bool RunsPhase2(double FrameBudgetMs)
		{
			return FrameBudgetMs > 0.0;
		}

		inline FRowCostLine FlatLine(int64_t RowCostUs)
		{
			return FRowCostLine{RowCostUs, 0, 0, 1, ELineSource::Flat};
		}

		inline FRowCostLine DefaultSlopeLine(int64_t AnchorUs, int64_t AnchorPos)
		{
			return FRowCostLine{AnchorUs, AnchorPos, kDefaultSlopeNum * (AnchorUs > 0 ? AnchorUs : 0), kDefaultSlopeDen, ELineSource::DefaultSlope};
		}

		inline FRowCostLine LineFrom(int64_t AnchorUs, int64_t AnchorPos, int64_t LateUs, int64_t LatePos)
		{
			const int64_t Den = LatePos - AnchorPos;
			if (Den <= 0)
			{
				return FRowCostLine{AnchorUs, AnchorPos, 0, 1, ELineSource::Measured}; // no distance between the positions: flat
			}
			const int64_t Num = LateUs - AnchorUs;
			return FRowCostLine{AnchorUs, AnchorPos, Num > 0 ? Num : 0, Den, ELineSource::Measured};
		}

		inline int64_t RowCostAtUs(const FRowCostLine& Line, int64_t Pos)
		{
			if (Line.SlopeNumUs <= 0 || Line.SlopeDenPos <= 0)
			{
				return Line.AnchorUs;
			}
			const int64_t A = Line.SlopeNumUs * (Pos - Line.AnchorPos);
			const int64_t B = Line.SlopeDenPos;
			// Rounded up (a cost); before pos_a the term is negative and rounds up toward 0.
			const int64_t Term = A >= 0 ? (A + B - 1) / B : -((-A) / B);
			return Line.AnchorUs + Term;
		}

		inline bool LineInSanityRange(const FRowCostLine& Line, int64_t ContextCap)
		{
			return LineDetail::PriceInRange(Line.AnchorUs) && RowCostAtUs(Line, ContextCap) <= kSanityMaxUs;
		}

		// Phase 1. Attempt: 0, or 1 for the retry. AnchorUs: PriceUs() at pos_a (-1: the attempt failed).
		inline EStep StepPhase1(int32_t Attempt, int64_t AnchorUs)
		{
			if (AnchorUs < 0)
			{
				return EStep::FallBack; // a failed attempt is not retried (ruling 48a)
			}
			if (LineDetail::PriceInRange(AnchorUs))
			{
				return EStep::Use;
			}
			return Attempt + 1 < kMaxAttempts ? EStep::Retry : EStep::FallBack;
		}

		// Phase 2 (one attempt). LateUs: PriceUs() at P2 (-1: the late position failed).
		inline EPhase2 StepPhase2(int64_t AnchorUs, int64_t AnchorPos, int64_t LateUs, int64_t LatePos, int64_t ContextCap)
		{
			if (AnchorUs < 0 || LateUs < 0)
			{
				return EPhase2::KeepDefault;
			}
			return LineInSanityRange(LineFrom(AnchorUs, AnchorPos, LateUs, LatePos), ContextCap) ? EPhase2::Use : EPhase2::KeepDefault;
		}

		// Erratum E-16 (ruling 60): a timed set's outlier check, in phase 1 and phase 2. A run over kOutlierFactor x the
		// set's median is dropped and one replacement run is timed for it, at most kMaxOutlierExtraRuns extra runs per set;
		// the price stays the max of the runs kept (ruling 48). A set that still holds such a run once its extra runs are
		// spent is a spread failure (not a cost failure): retried once, and cached only on the second.
		constexpr int32_t kOutlierFactor = 3;
		constexpr int32_t kMaxOutlierExtraRuns = 2;

		enum class ERunSet : uint8_t
		{
			Price = 0,   // no run over the factor: price the set (PriceUs())
			Replace = 1, // drop DropMask's runs and time Drops more
			Spread = 2,  // a run over the factor with no extra run left: a spread failure
		};

		struct FRunSetCheck
		{
			ERunSet Step = ERunSet::Price;
			uint32_t DropMask = 0; // bit i: run i is over the factor
			int32_t Drops = 0;
			double MedianUs = 0.0;
		};

		// The median of Count runs (the mean of the middle two for an even count); 0 for none. At most 32 runs.
		inline double MedianUs(const double* RunsUs, int32_t Count)
		{
			if (RunsUs == nullptr || Count <= 0 || Count > 32)
			{
				return 0.0;
			}
			double Sorted[32];
			for (int32_t I = 0; I < Count; ++I)
			{
				double V = RunsUs[I];
				int32_t J = I;
				for (; J > 0 && Sorted[J - 1] > V; --J)
				{
					Sorted[J] = Sorted[J - 1];
				}
				Sorted[J] = V;
			}
			return (Count & 1) != 0 ? Sorted[Count / 2] : 0.5 * (Sorted[Count / 2 - 1] + Sorted[Count / 2]);
		}

		// ExtraRunsUsed: the replacement runs this set has timed already. A run that is not finite or not above 0 leaves
		// the set to PriceUs() (which fails it, as before).
		inline FRunSetCheck CheckRunSet(const double* RunsUs, int32_t Count, int32_t ExtraRunsUsed)
		{
			FRunSetCheck Out;
			if (RunsUs == nullptr || Count <= 0 || Count > 32)
			{
				return Out;
			}
			for (int32_t I = 0; I < Count; ++I)
			{
				if (!std::isfinite(RunsUs[I]) || !(RunsUs[I] > 0.0))
				{
					return Out;
				}
			}
			Out.MedianUs = MedianUs(RunsUs, Count);
			const double Limit = static_cast<double>(kOutlierFactor) * Out.MedianUs;
			for (int32_t I = 0; I < Count; ++I)
			{
				if (RunsUs[I] > Limit)
				{
					Out.DropMask |= 1u << static_cast<uint32_t>(I);
					++Out.Drops;
				}
			}
			if (Out.Drops > 0)
			{
				Out.Step = ExtraRunsUsed + Out.Drops <= kMaxOutlierExtraRuns ? ERunSet::Replace : ERunSet::Spread;
			}
			return Out;
		}

		// bCalibratedOk false: calibration fell back (Calibrated is not read).
		inline FLineChoice ChooseLine(bool bGameSetRowCost, double GameRowCostMs, bool bCalibrate, bool bCalibratedOk, const FRowCostLine& Calibrated)
		{
			const double DefaultMs = static_cast<double>(kDefaultRowCostUs) / 1000.0;
			if (bGameSetRowCost)
			{
				return FLineChoice{ESource::Game, GameRowCostMs, FlatLine(LineDetail::CostMicros(GameRowCostMs))};
			}
			if (!bCalibrate)
			{
				return FLineChoice{ESource::Default, DefaultMs, FlatLine(kDefaultRowCostUs)};
			}
			if (bCalibratedOk && Calibrated.AnchorUs > 0)
			{
				return FLineChoice{ESource::Calibrated, static_cast<double>(Calibrated.AnchorUs) / 1000.0, Calibrated};
			}
			return FLineChoice{ESource::FallBack, DefaultMs, FlatLine(kDefaultRowCostUs)};
		}

		// The bodies (the implementation, ruling 48 and 48a).
		inline int64_t PriceUs(const double* RunsUs, int32_t Count)
		{
			if (RunsUs == nullptr || Count <= 0)
			{
				return -1;
			}
			double MaxUs = 0.0;
			for (int32_t I = 0; I < Count; ++I)
			{
				const double R = RunsUs[I];
				if (!std::isfinite(R) || !(R > 0.0)) // not finite, or not above 0: the attempt failed
				{
					return -1;
				}
				MaxUs = R > MaxUs ? R : MaxUs;
			}
			// Within 0.01 us of a multiple of 100 us is that multiple; otherwise round up to the next multiple.
			const double Near = std::floor(MaxUs / 100.0 + 0.5) * 100.0;
			const double Hundreds = std::fabs(MaxUs - Near) <= 0.01 ? Near / 100.0 : std::ceil(MaxUs / 100.0);
			return static_cast<int64_t>(Hundreds) * 100;
		}

		inline bool InSanityRange(int64_t PriceUs)
		{
			return PriceUs >= kSanityMinUs && PriceUs <= kSanityMaxUs;
		}

		// Attempt: 0 for the first attempt, 1 for the retry. PriceUs: PriceUs()'s result (-1: the attempt failed).
		inline EStep Step(int32_t Attempt, int64_t PriceUs)
		{
			if (PriceUs < 0)
			{
				return EStep::FallBack; // a failed attempt is not retried (ruling 48a)
			}
			if (InSanityRange(PriceUs))
			{
				return EStep::Use;
			}
			return Attempt + 1 < kMaxAttempts ? EStep::Retry : EStep::FallBack;
		}

		// CalibratedPriceUs: the used price, or -1 when calibration fell back (read only when it ran).
		inline FChoice Choose(bool bGameSetRowCost, double GameRowCostMs, bool bCalibrate, int64_t CalibratedPriceUs)
		{
			const double DefaultMs = static_cast<double>(kDefaultRowCostUs) / 1000.0;
			if (bGameSetRowCost)
			{
				return FChoice{ESource::Game, GameRowCostMs};
			}
			if (!bCalibrate)
			{
				return FChoice{ESource::Default, DefaultMs};
			}
			if (CalibratedPriceUs > 0)
			{
				return FChoice{ESource::Calibrated, static_cast<double>(CalibratedPriceUs) / 1000.0};
			}
			return FChoice{ESource::FallBack, DefaultMs};
		}
	}
}
