#include "SuperSLMEstimateCostsCommand.h"

#include "Core/SuperSLMCacheChunkPricing.h"
#include "Core/SuperSLMRestorePricing.h"

#include "SuperSLMCalibrationTestAccess.h"
#include "SuperSLMEstimateTestAccess.h"
#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMPrefillPlanning.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMRuntimeResolve.h"
#include "SuperSLMStatusMapping.h"

#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

#include "superslm/sslm_abi.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>
#include <cmath>
#include <limits>

// Decode-threading D4 (plan §10.3.1 item 5b; D-SLM8071, D-SLM8108-8114, D-SLM8140-8147): the user
// calibration estimate, SuperSLM.EstimateCosts "<ArtifactPath.sslm>" [--write-ini].
//
// One run measures both CPU decode modes on this machine and artifact, through the same C ABI
// calls the plugin makes (sslm_prefill, greedy sslm_decode_step_v2, sslm_seq_reset/save/restore,
// sslm_seq_adopt_prefix, sslm_prefix_begin/release) against the plugin's own runtime mapping, with
// the runtime's own finish hook installed at the task count Configure() would resolve with no
// caller value. Per mode: five untimed warm-up repetitions, then twelve timed repetitions of every
// arm, each call's start and end recorded. A sampler thread takes the box's and this process's CPU
// time every 250 ms across a 2 s pre-window, both modes and a 2 s post-window; the foreign share
// is the box's busy time less this whole process's, over capacity. The build lock named by
// SUPERSLM_BUILD_LOCK is classified at every sample and reported; it never stops the run.
//
// The report is labelled "CALIBRATION ESTIMATE -- not verified": per mode and arm/depth the call
// count, the conventional median and the type-7 IQR; thirteen suggested costs per mode from the two
// existing line fits, each rounded up; the foreign load per phase; sampler coverage; the lock
// observation; and the retained per-call and per-sample CSVs. Ordinary desktop load is disclosed,
// never a reason to stop. An invalid artifact, an incomplete arm, a refused ABI call, a non-finite
// cost, a failed sampler, an interruption or a failed batched premise is a named "incomplete
// estimate" that suggests nothing. Only --write-ini writes Saved/SuperSLM/CalibratedCosts.ini, and
// only after the post-window of a complete run, in one atomic replace; Configure() then uses a
// profile whose artifact hash, effective mode and task count match. This checks that the run
// executed completely, not that the costs are accurate.
//
// Precache U4 (plan §5.5, risk R5): a fourteenth, separate measurement, kept out of the thirteen and out of
// the per-call CSV so the D4 contract above is unchanged. Per mode, after the thirteen arms' pass, the blob
// saved at each of the five depths is restored (sslm_seq_restore) five times untimed and twelve times timed; a
// blob under 1 MiB (depth 1's) is left out. RestoreCostPerMiBMs is the slope between the smallest and the largest
// remaining readings, floored at 0 (SuperSLMRestorePricing.h, plan erratum 15.26),
// reported on "restore_size" and "suggested_per_mib" lines and written after the thirteen keys; Configure()
// reads it from a matching profile and prices the prefix cache's restores by bytes.
//
// Precache U5 (plan rev 1.18.11 §15.31, cell 5.9 (d)): a fifteenth, separate measurement, the prefix_prefill arm,
// also kept out of the thirteen and the per-call CSV. Per mode, after the restore_size pass, one prefix is built
// from depth 0 in floor-sized (8-token) sslm_prefix_prefill chunks up to depth 2048 (or the cap less one chunk),
// each chunk timed: the call a sliced cache build's chunk makes. A short build is the warm-up. The readings are
// fitted to a price pair (SuperSLMCacheChunkPricing.h, FitPrefixPrefillPair), reported on "prefix_prefill" and
// "suggested_prefix_prefill" lines and written after the per-MiB key as PrefixPrefillTokenCostMs and
// PrefixPrefillTokenCostPerPositionMs. Both keys are optional to the reader: Configure() prices a cache chunk of
// 8 tokens or more with the pair when a matching profile carries it, else with the batched pair. An artifact
// whose cap holds fewer chunks than the fit's groups is a named skip that writes no pair; a refused call or a
// non-finite time is an incomplete estimate, as for every other arm.
//
// Rev 1.18.12 (§15.32): this arm is the isolated one. It times sslm_prefix_prefill with the game thread blocked,
// outside the worker lanes and away from a build's interleaved generation, and the box read its pair 1.5x low against
// a build's chunks. Its readings are also written to modeN.prefix_prefill.csv beside the per-call CSVs, as the
// isolated attribution reading. The pair a cache chunk is priced with is then the in-situ arm's, fitted from a real
// cache build's ledger rows and written over this pair (USuperSLMSubsystem::WriteInSituPrefixPrefillPrice); this
// arm's pair prices cache chunks until that runs.

namespace
{
	const TCHAR* const kPrefix = TEXT("SuperSLM.EstimateCosts: ");

	// Five untimed warm-up repetitions and twelve timed ones per arm and mode, as the T-3050d
	// per-call capture blocks the one-time profiles come from.
	constexpr int32 kWarmUpReps = 5;
	constexpr int32 kTimedReps = 12;

	// The smallest cap the depth rule accepts: 1, C/4, C/2, 3C/4 and C - 1 are distinct from 8 up.
	constexpr int64 kMinCapTokens = 8;

	// The chunk the untimed setup prefill uses to reach each depth (each call's consumed count is checked).
	constexpr int32 kSetupChunk = 64;

	constexpr double kSampleIntervalSeconds = 0.25;
	constexpr double kPreWindowSeconds = 2.0;
	constexpr double kPostWindowSeconds = 2.0;

	// Schema fast-decode plan rev 1.3.6, §6 P1 "Where the prices come from" (P1.1; mutant XD9): the
	// C = 2..7 calibration arms, beside the chunk-1 arm. Arm names, indexed by C (stable literals: the
	// per-call trace keeps the pointer).
	const TCHAR* const kSmallChunkArms[8] = { nullptr, nullptr,
		TEXT("prompt_2"), TEXT("prompt_3"), TEXT("prompt_4"), TEXT("prompt_5"), TEXT("prompt_6"), TEXT("prompt_7") };
	constexpr int32 kSmallChunkMin = 2;
	constexpr int32 kSmallChunkMax = 7;
	constexpr int32 kSmallChunkArmCount = kSmallChunkMax - kSmallChunkMin + 1;

	// A cost is a finite, positive number of milliseconds.
	bool IsValidCostMs(double Ms)
	{
		return FMath::IsFinite(Ms) && Ms > 0.0;
	}

	FString StatusName(sslm_status Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
	}

#if WITH_DEV_AUTOMATION_TESTS
	// FSuperSLMEstimateTestAccess: the fault the next run injects in mode 1.
	ESuperSLMEstimateFault GNextFault = ESuperSLMEstimateFault::None;
	TArray<FSuperSLMEstimateResetObservation> GResetObservations;
#endif

	// --- Statistics (D-SLM8112) ------------------------------------------------------------------

	// The conventional median: the middle value of an odd count, the mean of the two middle values
	// of an even count. Values is sorted in place.
	double ConventionalMedian(TArray<double>& Values)
	{
		check(Values.Num() > 0);
		Values.Sort();
		const int32 N = Values.Num();
		return (N % 2) ? Values[N / 2] : 0.5 * (Values[N / 2 - 1] + Values[N / 2]);
	}

	// Linear interpolation between order statistics at zero-based position (n - 1) x q (Hyndman-Fan
	// type 7). Sorted must be ascending.
	double Type7Quantile(const TArray<double>& Sorted, double Q)
	{
		check(Sorted.Num() > 0);
		const double P = static_cast<double>(Sorted.Num() - 1) * Q;
		const int32 Lo = static_cast<int32>(std::floor(P));
		const int32 Hi = static_cast<int32>(std::ceil(P));
		return Sorted[Lo] + (Sorted[Hi] - Sorted[Lo]) * (P - static_cast<double>(Lo));
	}

	// D-SLM8113: the smallest value with Decimals digits at or above x - 10^-(Decimals + 9),
	// computed as ceil(x * 10^d - 1e-9) / 10^d, printed with exactly Decimals digits. Printed from the
	// integer k, so the string is exact.
	FString RoundUpText(double Value, int32 Decimals)
	{
		const double Scale = std::pow(10.0, Decimals);
		const int64 K = static_cast<int64>(std::ceil(Value * Scale - 1e-9));
		const int64 Unit = static_cast<int64>(Scale + 0.5);
		const bool bNegative = K < 0;
		const int64 Abs = bNegative ? -K : K;
		FString Fraction = FString::Printf(TEXT("%lld"), Abs % Unit);
		Fraction = FString::ChrN(FMath::Max(0, Decimals - Fraction.Len()), TEXT('0')) + Fraction;
		return FString::Printf(TEXT("%s%lld.%s"), bNegative ? TEXT("-") : TEXT(""), Abs / Unit, *Fraction);
	}

	// --- The measured readings and their fits ------------------------------------------------------

	// One depth arm's reading at one depth: the conventional median of its timed calls (for the
	// batched arm the per-token median, call / 8, at position depth + 3.5).
	struct FDepthArmReading
	{
		int64 Depth = 0;
		int64 RestoredContextLength = -1;
		double MedianMs = -1.0;
		int32 Samples = 0;
		double PositionMean = -1.0; // the batched arm only: Depth + 3.5
	};

	// Slice 1P (tiled-matmul plan §4A.1 item 3, rev 11.1 audit F10): one batched reading, shared by
	// the estimate and the synthetic-timing seam (FSuperSLMCalibrationTestAccess).
	FDepthArmReading MakeBatchedReading(int64 Depth, int64 RestoredContextLength, double MedianCallMs, int32 Samples)
	{
		FDepthArmReading R;
		R.Depth = Depth;
		R.RestoredContextLength = RestoredContextLength;
		R.PositionMean = SuperSLMPrefillPlanning::BatchedReadingPosition(Depth);
		R.MedianMs = SuperSLMPrefillPlanning::BatchedReadingPerTokenMs(MedianCallMs);
		R.Samples = Samples;
		return R;
	}

	// The batched line (FitBatchedUpperEnvelope: the chord through the first reading extended back to
	// position 0, then raised) and its premise: the batched per-token intercept must not exceed the
	// chunk-1 one, or the chunk-1 pair was never an upper bound for a token inside an 8-token call.
	// A failed premise is a model-validity condition, not a load refusal.
	bool FitBatchedArm(const TArray<FDepthArmReading>& Arm, double Chunk1TokenMs, double& OutTokenMs, double& OutPerPositionMs, FString& OutError)
	{
		if (Arm.Num() == 0)
		{
			OutError = TEXT("batched prompt arm: no readings");
			return false;
		}
		TArray<double> Positions, Medians;
		for (const FDepthArmReading& R : Arm)
		{
			Positions.Add(R.PositionMean);
			Medians.Add(R.MedianMs);
		}
		SuperSLMPrefillPlanning::FitBatchedUpperEnvelope(Positions.GetData(), Medians.GetData(), Arm.Num(), OutTokenMs, OutPerPositionMs);
		if (!SuperSLMPrefillPlanning::BatchedInterceptWithinChunk1(OutTokenMs, Chunk1TokenMs))
		{
			OutError = FString::Printf(TEXT("batched prompt arm: the per-token intercept %.6f ms at chunk 8 exceeds the chunk-1 intercept %.6f ms, so the chunk-1 pair is not an upper bound for a token inside an 8-token call; no suggestion is made"),
				OutTokenMs, Chunk1TokenMs);
			return false;
		}
		return true;
	}

	// D-SLM8141: a cap under 33 skips the batched arm by name, and the measured mode's one-time pair
	// is suggested in its place -- the same values Configure() resolves for that mode.
	void BatchedArmSkipValues(bool bMode, double& OutTokenMs, double& OutPerPositionMs)
	{
		FSuperSLMResolvedCpuConfig Profile;
		SuperSLMRuntimeResolve::ApplyModeProfile(bMode, Profile);
		OutTokenMs = Profile.BatchedPromptTokenCostMs;
		OutPerPositionMs = Profile.BatchedPromptTokenCostPerPositionMs;
	}

	// The chunk-1 line over (depth, median) (D-SLM7793/D-SLM7794, T-3002 W1): slope = (last median
	// - first median) / span, floored at 0; the intercept starts from the depth-1 median and is raised
	// by the largest amount any median sits above the line, so no measured median lies above it.
	void FitUpperEnvelope(const TArray<FDepthArmReading>& Arm, double& OutIntercept, double& OutSlope)
	{
		const FDepthArmReading& First = Arm[0];
		const FDepthArmReading& Last = Arm.Last();
		const double Span = static_cast<double>(Last.Depth - First.Depth);
		OutSlope = Span > 0.0 ? FMath::Max(0.0, (Last.MedianMs - First.MedianMs) / Span) : 0.0;
		OutIntercept = First.MedianMs;
		double Raise = 0.0;
		for (const FDepthArmReading& R : Arm)
		{
			Raise = FMath::Max(Raise, R.MedianMs - (OutIntercept + OutSlope * static_cast<double>(R.Depth)));
		}
		OutIntercept += Raise;
	}

	// One timed call, its times on the run's series origin (seconds). Added after the call's end
	// read, outside the measured interval.
	struct FCallTiming
	{
		const TCHAR* Arm = nullptr;
		int64 Depth = -1;
		int32 Rep = -1;
		int32 Layer = -1;
		double StartSeconds = 0.0;
		double EndSeconds = 0.0;
		double ElapsedMs() const { return (EndSeconds - StartSeconds) * 1000.0; }
	};

	struct FPerCallTrace
	{
		double Origin = 0.0;
		TArray<FCallTiming> Calls;
		void Add(const TCHAR* Arm, int64 Depth, int32 Rep, int32 Layer, double Start, double End)
		{
			Calls.Add(FCallTiming{ Arm, Depth, Rep, Layer, Start - Origin, End - Origin });
		}
	};

	// Layer 1's 'SSB5' sequence blob (src/sslm_abi.cpp, sslm_seq_save's write order at v1.9.0,
	// unchanged at v1.11.0): context_length is a little-endian int64 at byte 60. Read only to assert
	// the depth a blob this process's own sslm_seq_save just wrote carries.
	constexpr size_t kSsb5ContextLengthOffset = 60;

	bool ReadBlobContextLength(const uint8* Blob, size_t Bytes, int64& OutContextLength)
	{
		if (Bytes < kSsb5ContextLengthOffset + 8 || Blob[0] != 'S' || Blob[1] != 'S' || Blob[2] != 'B' || Blob[3] != '5')
		{
			return false;
		}
		uint64 V = 0;
		for (int32 I = 0; I < 8; ++I)
		{
			V |= static_cast<uint64>(Blob[kSsb5ContextLengthOffset + I]) << (8 * I);
		}
		OutContextLength = static_cast<int64>(V);
		return true;
	}

	// What a timed pass consults between arms and at its first call (fault seam and engine exit).
	struct FMeasureControl
	{
		// Returns true, with the reason, when the pass must stop between two arms.
		TFunction<bool(FString&)> ShouldStop;
		// FSuperSLMEstimateTestAccess::InvalidAbiResult: the first finish call's status reads as a refusal.
		bool bInjectAbiRefusal = false;
	};

	// One decode mode's measurement session: a mapping of the shared model, a two-block pool, one
	// workspace with the runtime's finish hook, one sequence, and a blob saved at every depth an arm
	// restores from. Setup() is untimed and runs before the sample series.
	//
	// Every timed call's ABI status and output state are checked before its time is kept. What each
	// arm times (src/sslm_abi.cpp):
	//  - prompt_1, per depth d and repetition: restore d's blob, then one token of sslm_prefill at
	//    chunk budget 1 (consumed must be 1): one prompt token at position d.
	//  - layer, per depth d and repetition: restore d's blob, run the finish and the embed + layer 0
	//    untimed, time each one-layer call at layer 1 .. num_hidden_layers - 2 (each must return -1),
	//    and run the last layer + finish untimed (it must produce a token).
	//  - prompt_8, per batched depth d and repetition: restore d's blob, then one 8-token sslm_prefill
	//    at chunk budget 8 (consumed must be 8): the call a chunked prefill job makes.
	//  - prompt_2 .. prompt_7 (schema fast-decode plan rev 1.3.6, P1.1), per batched depth d and
	//    repetition: restore d's blob, then one C-token sslm_prefill at chunk budget C (consumed must
	//    be C): the call a prompt chunk or a span job of C rows makes. They share the batched depths
	//    (each leaves room for 8 tokens), and are a named skip with the batched arm.
	//  - finish: reset, prefill one token untimed, time the next decode (the token finish only).
	//  - reset, save, restore, adopt (a one-token frozen prefix), prefix_begin and prefix_release.
	class FEstimateSession
	{
	public:
		~FEstimateSession() { Cleanup(); }

		int32 NumLayers = 0;
		int64 Cap = 0;
		TArray<int64> Depths;        // 1, C/4, C/2, 3C/4, C - 1
		TArray<int64> BatchedDepths; // 1, C/4, C/2, 3C/4, C - 8; empty when the batched arm is a named skip
		FString BatchedArmSkipReason;

		// The calls one pass of Reps repetitions times.
		int32 ExpectedCallCount(int32 Reps) const
		{
			return Reps * (7 + Depths.Num() * (NumLayers - 1) + BatchedDepths.Num() * (1 + kSmallChunkArmCount));
		}

		bool Setup(USuperSLMModel& Model, bool bMode, int32 ParallelTasks, FString& OutError)
		{
			SuperSLMRuntime::FModelShape Shape;
			FString ShapeError;
			if (!SuperSLMRuntime::ReadModelShape(Model, Shape, ShapeError))
			{
				OutError = FString::Printf(TEXT("could not read the model's shape: %s"), *ShapeError);
				return false;
			}
			NumLayers = Shape.NumHiddenLayers;
			if (NumLayers < 3)
			{
				OutError = FString::Printf(TEXT("the model has %d hidden layers; the layer arm needs at least 3 to time a layer that neither embeds nor finishes"), NumLayers);
				return false;
			}
			Cap = Shape.ContextCap;
			if (Cap < kMinCapTokens)
			{
				OutError = FString::Printf(TEXT("the artifact's cap C = %lld is under %lld: the depths 1, C/4, C/2, 3C/4 and C - 1 need a cap of at least %lld"), Cap, kMinCapTokens, kMinCapTokens);
				return false;
			}
			Depths = { 1, Cap / 4, Cap / 2, (3 * Cap) / 4, Cap - 1 };
			int64 BatchedDepthValues[5] = {};
			BatchedDepths.Reset();
			if (SuperSLMPrefillPlanning::BatchedArmDepths(Cap, BatchedDepthValues))
			{
				BatchedDepths.Append(BatchedDepthValues, 5);
			}
			else
			{
				BatchedArmSkipReason = FString::Printf(TEXT("the artifact's cap C = %lld is under %lld, so the depths 1, C/4, C/2, 3C/4 and C - %d do not ascend"),
					Cap, SuperSLMPrefillPlanning::kBatchedArmMinCap, SuperSLMPrefillPlanning::kBatchedArmChunk);
			}
			BlobDepths = Depths;
			for (int64 D : BatchedDepths)
			{
				BlobDepths.AddUnique(D);
			}
			BlobDepths.Sort();

			FString MapError;
			Mapping = SuperSLMRuntime::AcquireModelMapping(Model, MapError);
			if (Mapping == nullptr)
			{
				OutError = FString::Printf(TEXT("could not map the artifact: %s"), *MapError);
				return false;
			}

			// One sequence per call and one layer per decode call; the chunk ceiling serves the
			// untimed setup prefill and the batched arm.
			sslm_config CallShape;
			CallShape.max_batch = 1;
			CallShape.max_chunk_budget = kSetupChunk;
			CallShape.max_layer_budget = 1;
			CallShape.reserved = 0;

			const size_t BlockBytes = sslm_kv_block_size(Mapping);
			const size_t OverheadBytes = sslm_kv_pool_overhead_size(Mapping, kBlockCount);
			const size_t WorkspaceBytes = sslm_workspace_size(Mapping, &CallShape);
			StateBytes = sslm_seq_state_size(Mapping);
			if (BlockBytes == 0 || WorkspaceBytes == 0 || StateBytes == 0 || OverheadBytes == 0 || OverheadBytes == SIZE_MAX)
			{
				OutError = FString::Printf(TEXT("Layer 1 could not size the measurement pool/workspace (block %llu, workspace %llu, state %llu bytes)"),
					static_cast<uint64>(BlockBytes), static_cast<uint64>(WorkspaceBytes), static_cast<uint64>(StateBytes));
				return false;
			}
			KvBufferBytes = BlockBytes * kBlockCount + OverheadBytes;
			KvBuffer = FMemory::Malloc(KvBufferBytes, SSLM_ABI_ALIGNMENT_BYTES);
			WorkspaceBuffer = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
			SaveBuffer = FMemory::Malloc(StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
			if (KvBuffer == nullptr || WorkspaceBuffer == nullptr || SaveBuffer == nullptr)
			{
				OutError = TEXT("could not allocate the measurement buffers");
				return false;
			}
			sslm_status Status = sslm_kv_pool_create(Mapping, KvBuffer, KvBufferBytes, kBlockCount, &Pool);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("sslm_kv_pool_create failed (%s)"), *StatusName(Status));
				return false;
			}
			Status = sslm_workspace_create(Mapping, &CallShape, WorkspaceBuffer, WorkspaceBytes, &Workspace);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("sslm_workspace_create failed (%s)"), *StatusName(Status));
				return false;
			}
			Status = sslm_seq_create(Mapping, &Pool, &Seq);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("sslm_seq_create failed (%s)"), *StatusName(Status));
				return false;
			}
			// Plan §2.5 row 2 (D-SLM7663) and D-SLM8140: the runtime's own finish hook at the task
			// count Configure() would resolve with no caller value, with this mode's bits. At 0 or 1
			// nothing is installed, exactly as Configure() runs.
			if (SuperSLMFinishHook::ShouldInstall(ParallelTasks))
			{
				const sslm_parallel_for Hook = SuperSLMFinishHook::Make(ParallelTasks, bMode ? SSLM_PARALLEL_FOR_MATVEC : 0);
				Status = sslm_workspace_set_parallel_for(Workspace, &Hook);
				if (Status != SSLM_OK)
				{
					OutError = FString::Printf(TEXT("sslm_workspace_set_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"), ParallelTasks, *StatusName(Status));
					return false;
				}
			}
			Status = sslm_decode_params_init(Mapping, SSLM_DECODE_MODE_GREEDY, 1, &Params);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("sslm_decode_params_init(layer_budget 1) failed (%s)"), *StatusName(Status));
				return false;
			}

			// The untimed prefill to C - 1, saving a blob at each depth on the way.
			Status = sslm_seq_reset(Seq);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("setup: sslm_seq_reset failed (%s)"), *StatusName(Status));
				return false;
			}
			TArray<int32> Tokens;
			Tokens.Init(PromptToken, kSetupChunk);
			int64 Filled = 0;
			DepthBlobs.SetNum(BlobDepths.Num());
			for (int32 K = 0; K < BlobDepths.Num(); ++K)
			{
				while (Filled < BlobDepths[K])
				{
					const int32 Count = static_cast<int32>(FMath::Min<int64>(kSetupChunk, BlobDepths[K] - Filled));
					int32 Consumed = 0;
					Status = sslm_prefill(Mapping, Seq, Tokens.GetData(), Count, Count, SSLM_SPAN_PROMPT, Workspace, &Consumed);
					if (Status != SSLM_OK || Consumed <= 0 || Consumed > Count)
					{
						OutError = FString::Printf(TEXT("setup: sslm_prefill toward depth %lld returned %s and consumed %d of %d at depth %lld"),
							BlobDepths[K], *StatusName(Status), Consumed, Count, Filled);
						return false;
					}
					Filled += Consumed;
				}
				size_t PayloadBytes = StateBytes;
				Status = sslm_seq_save(Seq, SaveBuffer, &PayloadBytes);
				int64 BlobDepth = -1;
				if (Status != SSLM_OK || PayloadBytes == 0 || PayloadBytes > StateBytes ||
					!ReadBlobContextLength(static_cast<const uint8*>(SaveBuffer), PayloadBytes, BlobDepth) || BlobDepth != BlobDepths[K])
				{
					OutError = FString::Printf(TEXT("setup: the blob saved at depth %lld returned %s, %llu bytes, context_length %lld"),
						BlobDepths[K], *StatusName(Status), static_cast<uint64>(PayloadBytes), BlobDepth);
					return false;
				}
				DepthBlobs[K].SetNumUninitialized(static_cast<int32>(PayloadBytes));
				FMemory::Memcpy(DepthBlobs[K].GetData(), SaveBuffer, PayloadBytes);
			}
			return true;
		}

		// One pass of Reps repetitions of every arm, each timed call recorded in Trace.
		bool Measure(int32 Reps, FPerCallTrace& Trace, const FMeasureControl& Control, FString& OutError)
		{
			auto Fail = [&](const FString& Why)
			{
				OutError = Why;
				return false;
			};
			auto Stop = [&](const TCHAR* After) -> bool
			{
				FString Why;
				if (Control.ShouldStop && Control.ShouldStop(Why))
				{
					OutError = FString::Printf(TEXT("%s (after the %s arm)"), *Why, After);
					return true;
				}
				return false;
			};
			auto Decode = [&](int32& OutToken) -> sslm_status
			{
				OutToken = -9;
				return sslm_decode_step_v2(Mapping, &Seq, 1, &Params, Workspace, &OutToken);
			};
			sslm_status Status = SSLM_OK;

			// --- finish (depth-independent) ---
			for (int32 Rep = 0; Rep < Reps; ++Rep)
			{
				Status = sslm_seq_reset(Seq);
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("finish arm, rep %d: sslm_seq_reset failed (%s)"), Rep, *StatusName(Status)));
				}
				int32 Consumed = 0;
				Status = sslm_prefill(Mapping, Seq, &PromptToken, 1, 1, SSLM_SPAN_PROMPT, Workspace, &Consumed);
				if (Status != SSLM_OK || Consumed != 1)
				{
					return Fail(FString::Printf(TEXT("finish arm, rep %d: the untimed sslm_prefill returned %s and consumed %d of 1"), Rep, *StatusName(Status), Consumed));
				}
				int32 Token = -9;
				const double Start = FPlatformTime::Seconds();
				Status = Decode(Token);
				const double End = FPlatformTime::Seconds();
				if (Control.bInjectAbiRefusal && Rep == 0)
				{
					Status = SSLM_INVALID_ARGUMENT; // the fault seam's refused call
				}
				if (Status != SSLM_OK || Token < 0)
				{
					return Fail(FString::Printf(TEXT("finish arm, rep %d: the finish-only decode returned %s and token %d"), Rep, *StatusName(Status), Token));
				}
				Trace.Add(TEXT("finish"), 1, Rep, -1, Start, End);
			}
			if (Stop(TEXT("finish"))) { return false; }

			// --- the chunk-1 depth arms: one restore per arm per repetition ---
			for (const int64 Depth : Depths)
			{
				for (int32 Rep = 0; Rep < Reps; ++Rep)
				{
					if (!RestoreAtDepth(Depth, OutError))
					{
						return Fail(FString::Printf(TEXT("prompt_1 arm, depth %lld, rep %d: %s"), Depth, Rep, *OutError));
					}
					int32 Consumed = 0;
					const double Start = FPlatformTime::Seconds();
					Status = sslm_prefill(Mapping, Seq, &PromptToken, 1, 1, SSLM_SPAN_PROMPT, Workspace, &Consumed);
					const double End = FPlatformTime::Seconds();
					if (Status != SSLM_OK || Consumed != 1)
					{
						return Fail(FString::Printf(TEXT("prompt_1 arm, depth %lld, rep %d: sslm_prefill returned %s and consumed %d of 1"), Depth, Rep, *StatusName(Status), Consumed));
					}
					Trace.Add(TEXT("prompt_1"), Depth, Rep, -1, Start, End);
				}
				for (int32 Rep = 0; Rep < Reps; ++Rep)
				{
					if (!RestoreAtDepth(Depth, OutError))
					{
						return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d: %s"), Depth, Rep, *OutError));
					}
					int32 Token = -9;
					Status = Decode(Token); // the finish of the restored ready_for_logits residual: not timed
					if (Status != SSLM_OK || Token < 0)
					{
						return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d: the untimed finish returned %s and token %d"), Depth, Rep, *StatusName(Status), Token));
					}
					Status = Decode(Token); // embed + layer 0: not timed
					if (Status != SSLM_OK || Token != -1)
					{
						return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d: the embed call returned %s and %d (expected -1)"), Depth, Rep, *StatusName(Status), Token));
					}
					for (int32 LayerIndex = 1; LayerIndex <= NumLayers - 2; ++LayerIndex)
					{
						const double Start = FPlatformTime::Seconds();
						Status = Decode(Token);
						const double End = FPlatformTime::Seconds();
						if (Status != SSLM_OK || Token != -1)
						{
							return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d, layer %d: the one-layer call returned %s and %d (expected -1)"), Depth, Rep, LayerIndex, *StatusName(Status), Token));
						}
						Trace.Add(TEXT("layer"), Depth, Rep, LayerIndex, Start, End);
					}
					Status = Decode(Token); // the last layer and its finish: not timed
					if (Status != SSLM_OK || Token < 0)
					{
						return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d: the last layer's call returned %s and %d, so the arm's layer count (%d) does not match the model"),
							Depth, Rep, *StatusName(Status), Token, NumLayers));
					}
				}
				if (Stop(*FString::Printf(TEXT("depth-%lld chunk-1"), Depth))) { return false; }
			}

			// --- Slice 1P: the batched prompt arm ---
			if (!BatchedDepths.IsEmpty())
			{
				TArray<int32> BatchedTokens;
				BatchedTokens.Init(PromptToken, SuperSLMPrefillPlanning::kBatchedArmChunk);
				for (const int64 Depth : BatchedDepths)
				{
					for (int32 Rep = 0; Rep < Reps; ++Rep)
					{
						if (!RestoreAtDepth(Depth, OutError))
						{
							return Fail(FString::Printf(TEXT("prompt_8 arm, depth %lld, rep %d: %s"), Depth, Rep, *OutError));
						}
						int32 Consumed = 0;
						const double Start = FPlatformTime::Seconds();
						Status = sslm_prefill(Mapping, Seq, BatchedTokens.GetData(), SuperSLMPrefillPlanning::kBatchedArmChunk,
							SuperSLMPrefillPlanning::kBatchedArmChunk, SSLM_SPAN_PROMPT, Workspace, &Consumed);
						const double End = FPlatformTime::Seconds();
						if (Status != SSLM_OK || Consumed != SuperSLMPrefillPlanning::kBatchedArmChunk)
						{
							return Fail(FString::Printf(TEXT("prompt_8 arm, depth %lld, rep %d: sslm_prefill returned %s and consumed %d of %d"),
								Depth, Rep, *StatusName(Status), Consumed, SuperSLMPrefillPlanning::kBatchedArmChunk));
						}
						Trace.Add(TEXT("prompt_8"), Depth, Rep, -1, Start, End);
					}
				}
				if (Stop(TEXT("prompt_8"))) { return false; }

				// --- Schema fast-decode plan rev 1.3.6 (P1.1): the C = 2..7 arms ---
				for (int32 Chunk = kSmallChunkMin; Chunk <= kSmallChunkMax; ++Chunk)
				{
					for (const int64 Depth : BatchedDepths)
					{
						for (int32 Rep = 0; Rep < Reps; ++Rep)
						{
							if (!RestoreAtDepth(Depth, OutError))
							{
								return Fail(FString::Printf(TEXT("%s arm, depth %lld, rep %d: %s"), kSmallChunkArms[Chunk], Depth, Rep, *OutError));
							}
							int32 Consumed = 0;
							const double Start = FPlatformTime::Seconds();
							Status = sslm_prefill(Mapping, Seq, BatchedTokens.GetData(), Chunk, Chunk, SSLM_SPAN_PROMPT, Workspace, &Consumed);
							const double End = FPlatformTime::Seconds();
							if (Status != SSLM_OK || Consumed != Chunk)
							{
								return Fail(FString::Printf(TEXT("%s arm, depth %lld, rep %d: sslm_prefill returned %s and consumed %d of %d"),
									kSmallChunkArms[Chunk], Depth, Rep, *StatusName(Status), Consumed, Chunk));
							}
							Trace.Add(kSmallChunkArms[Chunk], Depth, Rep, -1, Start, End);
						}
					}
					if (Stop(kSmallChunkArms[Chunk])) { return false; }
				}
			}

			// --- reset / save / restore ---
			for (int32 Rep = 0; Rep < Reps; ++Rep)
			{
				// Paging reset releases live pages instead of clearing the old whole reserve.
				// A one-page reset can fit inside one clock tick. Price a fully populated context,
				// with its largest live page population, and verify the actual ABI state untimed.
				int64 ResetBefore = -1;
				if (!RestoreAtDepth(Cap - 1, OutError, &ResetBefore)) { return false; }
				if (ResetBefore != Cap - 1) { return Fail(TEXT("reset arm: input is not the full-context fixture")); }
				double Start = FPlatformTime::Seconds();
				Status = sslm_seq_reset(Seq);
				double End = FPlatformTime::Seconds();
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("reset arm, rep %d: sslm_seq_reset failed (%s)"), Rep, *StatusName(Status)));
				}
				Trace.Add(TEXT("reset"), -1, Rep, -1, Start, End);

				size_t PayloadBytes = StateBytes;
				Start = FPlatformTime::Seconds();
				Status = sslm_seq_save(Seq, SaveBuffer, &PayloadBytes);
				End = FPlatformTime::Seconds();
				if (Status != SSLM_OK || PayloadBytes == 0 || PayloadBytes > StateBytes)
				{
					return Fail(FString::Printf(TEXT("save arm, rep %d: sslm_seq_save returned %s with %llu payload bytes"), Rep, *StatusName(Status), static_cast<uint64>(PayloadBytes)));
				}
				Trace.Add(TEXT("save"), 0, Rep, -1, Start, End);
				int64 ResetAfter = -1;
				if (!ReadBlobContextLength(static_cast<const uint8*>(SaveBuffer), PayloadBytes, ResetAfter) || ResetAfter != 0)
				{
					return Fail(FString::Printf(TEXT("reset arm, rep %d: resetting context %lld did not leave an empty sequence"), Rep, ResetBefore));
				}
#if WITH_DEV_AUTOMATION_TESTS
				GResetObservations.Add({ ResetBefore, ResetAfter });
#endif

				sslm_seq Restored = nullptr;
				Start = FPlatformTime::Seconds();
				Status = sslm_seq_restore(Mapping, &Pool, SaveBuffer, PayloadBytes, &Restored);
				End = FPlatformTime::Seconds();
				if (Status != SSLM_OK || Restored == nullptr)
				{
					return Fail(FString::Printf(TEXT("restore arm, rep %d: sslm_seq_restore failed (%s)"), Rep, *StatusName(Status)));
				}
				Trace.Add(TEXT("restore"), 0, Rep, -1, Start, End);
				Status = sslm_seq_release(Seq);
				Seq = Restored;
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("restore arm, rep %d: releasing the replaced sequence failed (%s)"), Rep, *StatusName(Status)));
				}
			}
			if (Stop(TEXT("reset, save and restore"))) { return false; }

			// --- adopt: a one-token frozen prefix, adopted by a freshly reset sequence each rep ---
			Status = sslm_prefix_begin(Mapping, &Pool, &Prefix);
			if (Status != SSLM_OK || Prefix == nullptr)
			{
				Prefix = nullptr;
				return Fail(FString::Printf(TEXT("adopt arm: sslm_prefix_begin failed (%s)"), *StatusName(Status)));
			}
			{
				int32 Consumed = 0;
				Status = sslm_prefix_prefill(Mapping, Prefix, &PromptToken, 1, 1, SSLM_SPAN_PROMPT, Workspace, &Consumed);
				if (Status != SSLM_OK || Consumed != 1)
				{
					return Fail(FString::Printf(TEXT("adopt arm: sslm_prefix_prefill returned %s and consumed %d of 1"), *StatusName(Status), Consumed));
				}
				Status = sslm_prefix_freeze(Prefix);
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm: sslm_prefix_freeze failed (%s)"), *StatusName(Status)));
				}
			}
			for (int32 Rep = 0; Rep < Reps; ++Rep)
			{
				Status = sslm_seq_reset(Seq);
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm, rep %d: sslm_seq_reset failed (%s)"), Rep, *StatusName(Status)));
				}
				const double Start = FPlatformTime::Seconds();
				Status = sslm_seq_adopt_prefix(Seq, Prefix);
				const double End = FPlatformTime::Seconds();
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm, rep %d: sslm_seq_adopt_prefix failed (%s)"), Rep, *StatusName(Status)));
				}
				Trace.Add(TEXT("adopt"), 1, Rep, -1, Start, End);
			}
			{
				const sslm_prefix Held = Prefix;
				Prefix = nullptr;
				Status = sslm_prefix_release(Held);
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm: releasing its prefix failed (%s)"), *StatusName(Status)));
				}
			}
			if (Stop(TEXT("adopt"))) { return false; }

			// --- prefix_begin / prefix_release: the two calls a prefix's admin jobs make ---
			for (int32 Rep = 0; Rep < Reps; ++Rep)
			{
				sslm_prefix RepPrefix = nullptr;
				double Start = FPlatformTime::Seconds();
				Status = sslm_prefix_begin(Mapping, &Pool, &RepPrefix);
				double End = FPlatformTime::Seconds();
				if (Status != SSLM_OK || RepPrefix == nullptr)
				{
					return Fail(FString::Printf(TEXT("prefix_begin arm, rep %d: sslm_prefix_begin failed (%s)"), Rep, *StatusName(Status)));
				}
				Trace.Add(TEXT("prefix_begin"), -1, Rep, -1, Start, End);
				Start = FPlatformTime::Seconds();
				Status = sslm_prefix_release(RepPrefix);
				End = FPlatformTime::Seconds();
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("prefix_release arm, rep %d: sslm_prefix_release failed (%s)"), Rep, *StatusName(Status)));
				}
				Trace.Add(TEXT("prefix_release"), -1, Rep, -1, Start, End);
			}
			return true;
		}

		// Precache U4: one pass of Reps timed restores of each depth's blob (Depths), each recorded in Trace under the
		// arm "restore_size" at its depth. The blob's bytes are BlobBytesAt(depth). A blob under 1 MiB is not
		// measured (IsSizedDepth): the fit leaves it out (review S2).
		bool MeasureRestoreSizes(int32 Reps, FPerCallTrace& Trace, FString& OutError)
		{
			for (const int64 Depth : Depths)
			{
				const int32 K = BlobDepths.IndexOfByKey(Depth);
				if (K == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("restore_size: no blob was saved at depth %lld"), Depth);
					return false;
				}
				if (!IsSizedDepth(Depth))
				{
					continue;
				}
				const TArray<uint8>& Blob = DepthBlobs[K];
				for (int32 Rep = 0; Rep < Reps; ++Rep)
				{
					sslm_seq Restored = nullptr;
					const double Start = FPlatformTime::Seconds();
					sslm_status Status = sslm_seq_restore(Mapping, &Pool, Blob.GetData(), static_cast<size_t>(Blob.Num()), &Restored);
					const double End = FPlatformTime::Seconds();
					if (Status != SSLM_OK || Restored == nullptr)
					{
						OutError = FString::Printf(TEXT("restore_size arm, depth %lld, rep %d: sslm_seq_restore failed (%s)"), Depth, Rep, *StatusName(Status));
						return false;
					}
					Trace.Add(TEXT("restore_size"), Depth, Rep, -1, Start, End);
					Status = sslm_seq_release(Seq);
					Seq = Restored;
					if (Status != SSLM_OK)
					{
						OutError = FString::Printf(TEXT("restore_size arm, depth %lld, rep %d: releasing the replaced sequence failed (%s)"), Depth, Rep, *StatusName(Status));
						return false;
					}
				}
			}
			return true;
		}

		// Precache U5 (§15.31): the prefix_prefill arm's depth, a whole number of floor chunks; 0 when the cap holds
		// fewer chunks than the fit's groups (the named skip).
		int64 PrefixPrefillArmDepth() const
		{
			const int64 Chunk = SuperSLMCore::kCacheChunkTiledMinTokens;
			const int64 Depth = (FMath::Min<int64>(SuperSLMCore::kPrefixPrefillArmMaxDepth, Cap - Chunk) / Chunk) * Chunk;
			return Depth / Chunk >= SuperSLMCore::kPrefixPrefillFitGroups ? Depth : 0;
		}

		// Precache U5 (§15.31): one prefix built from depth 0 to Depth in floor-sized sslm_prefix_prefill chunks
		// (consumed must be the chunk), each chunk timed into Out, then released. The prefix takes the pool's spare
		// block, as the adopt arm's does.
		bool MeasurePrefixPrefill(int64 Depth, TArray<SuperSLMCore::FPrefixPrefillReading>& Out, FString& OutError)
		{
			const int32 Chunk = SuperSLMCore::kCacheChunkTiledMinTokens;
			Out.Reset();
			sslm_status Status = sslm_prefix_begin(Mapping, &Pool, &Prefix);
			if (Status != SSLM_OK || Prefix == nullptr)
			{
				Prefix = nullptr;
				OutError = FString::Printf(TEXT("prefix_prefill arm: sslm_prefix_begin failed (%s)"), *StatusName(Status));
				return false;
			}
			TArray<int32> Tokens;
			Tokens.Init(PromptToken, Chunk);
			for (int64 D = 0; D + Chunk <= Depth; D += Chunk)
			{
				int32 Consumed = 0;
				const double Start = FPlatformTime::Seconds();
				Status = sslm_prefix_prefill(Mapping, Prefix, Tokens.GetData(), Chunk, Chunk, SSLM_SPAN_PROMPT, Workspace, &Consumed);
				const double End = FPlatformTime::Seconds();
				if (Status != SSLM_OK || Consumed != Chunk)
				{
					OutError = FString::Printf(TEXT("prefix_prefill arm, depth %lld: sslm_prefix_prefill returned %s and consumed %d of %d"),
						D, *StatusName(Status), Consumed, Chunk);
					return false;
				}
				Out.Add(SuperSLMCore::FPrefixPrefillReading{ D, Chunk, (End - Start) * 1000.0 });
			}
			const sslm_prefix Held = Prefix;
			Prefix = nullptr;
			Status = sslm_prefix_release(Held);
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("prefix_prefill arm: releasing its prefix failed (%s)"), *StatusName(Status));
				return false;
			}
			return true;
		}

		int64 BlobBytesAt(int64 Depth) const
		{
			const int32 K = BlobDepths.IndexOfByKey(Depth);
			return K == INDEX_NONE ? 0 : static_cast<int64>(DepthBlobs[K].Num());
		}

		// The restore_size arm's depths: those whose blob is at least the fit's floor.
		bool IsSizedDepth(int64 Depth) const
		{
			return BlobBytesAt(Depth) >= SuperSLMCore::kRestoreFitMinBytes;
		}

	private:
		// Restores the depth's blob into a fresh sequence, releases the one it replaces, and asserts
		// the restored context_length by saving it back (Layer 1 exposes no getter).
		bool RestoreAtDepth(int64 Depth, FString& OutError, int64* OutObservedDepth = nullptr)
		{
			const int32 K = BlobDepths.IndexOfByKey(Depth);
			if (K == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("no blob was saved at depth %lld"), Depth);
				return false;
			}
			const TArray<uint8>& Blob = DepthBlobs[K];
			sslm_seq Restored = nullptr;
			sslm_status Status = sslm_seq_restore(Mapping, &Pool, Blob.GetData(), static_cast<size_t>(Blob.Num()), &Restored);
			if (Status != SSLM_OK || Restored == nullptr)
			{
				OutError = FString::Printf(TEXT("sslm_seq_restore of the depth-%lld blob failed (%s)"), BlobDepths[K], *StatusName(Status));
				return false;
			}
			Status = sslm_seq_release(Seq);
			Seq = Restored;
			if (Status != SSLM_OK)
			{
				OutError = FString::Printf(TEXT("releasing the replaced sequence failed (%s)"), *StatusName(Status));
				return false;
			}
			size_t PayloadBytes = StateBytes;
			Status = sslm_seq_save(Seq, SaveBuffer, &PayloadBytes);
			int64 ContextLength = -1;
			if (Status != SSLM_OK || !ReadBlobContextLength(static_cast<const uint8*>(SaveBuffer), PayloadBytes, ContextLength) || ContextLength != BlobDepths[K])
			{
				OutError = FString::Printf(TEXT("the restored sequence's context_length reads %lld (save %s), not the depth %lld"),
					ContextLength, *StatusName(Status), BlobDepths[K]);
				return false;
			}
			if (OutObservedDepth != nullptr) { *OutObservedDepth = ContextLength; }
			return true;
		}

		void Cleanup()
		{
			if (Prefix != nullptr) { sslm_prefix_release(Prefix); Prefix = nullptr; }
			if (Seq != nullptr) { sslm_seq_release(Seq); Seq = nullptr; }
			if (Workspace != nullptr) { sslm_workspace_destroy(Workspace); Workspace = nullptr; }
			if (Pool != nullptr) { sslm_kv_pool_destroy(Pool); Pool = nullptr; }
			FMemory::Free(SaveBuffer); SaveBuffer = nullptr;
			FMemory::Free(WorkspaceBuffer); WorkspaceBuffer = nullptr;
			FMemory::Free(KvBuffer); KvBuffer = nullptr;
			if (Mapping != nullptr) { SuperSLMRuntime::ReleaseModelMapping(Mapping); Mapping = nullptr; }
		}

		static constexpr uint32 kBlockCount = 2; // the live sequence, and one spare for restore and prefixes
		sslm_model Mapping = nullptr;
		TArray<int64> BlobDepths; // Depths and BatchedDepths, ascending and distinct; DepthBlobs follows it
		TArray<TArray<uint8>> DepthBlobs;
		size_t StateBytes = 0;
		size_t KvBufferBytes = 0;
		void* KvBuffer = nullptr;
		void* WorkspaceBuffer = nullptr;
		void* SaveBuffer = nullptr;
		sslm_kv_pool Pool = nullptr;
		sslm_workspace Workspace = nullptr;
		sslm_seq Seq = nullptr;
		sslm_prefix Prefix = nullptr;
		sslm_decode_params Params = {};
		int32 PromptToken = 0; // token id 0 is always in-vocabulary for a real artifact
	};

	// --- The foreign-load sample series -------------------------------------------------------------

	enum class EEstimatePhase : uint8
	{
		Pre,
		WarmUp,
		Workload,
		Post,
	};

	const TCHAR* PhaseName(EEstimatePhase Phase)
	{
		switch (Phase)
		{
			case EEstimatePhase::Pre: return TEXT("pre");
			case EEstimatePhase::WarmUp: return TEXT("warm-up");
			case EEstimatePhase::Workload: return TEXT("workload");
			case EEstimatePhase::Post: return TEXT("post");
		}
		return TEXT("?");
	}

	// The phase and mode the run is in, as one value so a sample never reads one without the other.
	int32 PackState(EEstimatePhase Phase, int32 Mode) { return (static_cast<int32>(Phase) << 8) | (Mode + 1); }
	EEstimatePhase StatePhase(int32 State) { return static_cast<EEstimatePhase>(State >> 8); }
	int32 StateMode(int32 State) { return (State & 0xFF) - 1; }

	// Plan §10.3.1 item 5b's build-lock observation (D-SLM7788, D-SLM8142), at every sample. A build
	// wrapper holds the lock named by SUPERSLM_BUILD_LOCK as a directory, writes a fresh GUID as the
	// first line of owner.txt inside it, and passes it to this process as SSU_BUILD_LOCK_TOKEN.
	// absent: nothing at the path. own: a directory, a non-empty token, and owner.txt's first line
	// equal to it. foreign: anything else. not-configured: SUPERSLM_BUILD_LOCK is unset. Observed and
	// reported; it never stops the estimate.
	enum class ELockState : uint8
	{
		Absent,
		Own,
		Foreign,
		NotConfigured,
	};

	const TCHAR* LockStateName(ELockState State)
	{
		switch (State)
		{
			case ELockState::Absent: return TEXT("absent");
			case ELockState::Own: return TEXT("own");
			case ELockState::Foreign: return TEXT("foreign");
			case ELockState::NotConfigured: return TEXT("not-configured");
		}
		return TEXT("foreign");
	}

	FString BuildLockPath()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_BUILD_LOCK")).TrimStartAndEnd();
	}

	ELockState ClassifyBuildLock(const FString& LockPath, const FString& Token)
	{
		if (LockPath.IsEmpty())
		{
			return ELockState::NotConfigured;
		}
		IFileManager& Files = IFileManager::Get();
		if (!Files.DirectoryExists(*LockPath))
		{
			return Files.FileExists(*LockPath) ? ELockState::Foreign : ELockState::Absent;
		}
		const FString OwnerFile = FPaths::Combine(LockPath, TEXT("owner.txt"));
		FString Contents;
		if (Token.IsEmpty() || !Files.FileExists(*OwnerFile) || !FFileHelper::LoadFileToString(Contents, *OwnerFile))
		{
			return ELockState::Foreign;
		}
		FString FirstLine;
		if (!Contents.Split(TEXT("\n"), &FirstLine, nullptr))
		{
			FirstLine = Contents;
		}
		FirstLine.TrimStartAndEndInline(); // drops the CR of a CRLF line and any padding
		return !FirstLine.IsEmpty() && FirstLine.Equals(Token, ESearchCase::IgnoreCase) ? ELockState::Own : ELockState::Foreign;
	}

	// One 250 ms sample. Shares are percent of the box's total capacity over the sample (every
	// logical processor): GetSystemTimes for the box, GetProcessTimes for this whole process.
	struct FLoadSample
	{
		double StartSeconds = 0.0; // on the run's series origin
		double EndSeconds = 0.0;
		EEstimatePhase Phase = EEstimatePhase::Pre; // the phase at the sample's end
		int32 Mode = -1;                            // the mode at the sample's end; -1 outside both
		double BoxSharePercent = 0.0;               // (kernel + user - idle) / (kernel + user)
		double OwnSharePercent = 0.0;               // this process's kernel + user / capacity
		double ForeignSharePercent = 0.0;           // max(0, box busy - own) / capacity
		ELockState LockState = ELockState::Absent;
	};

#if PLATFORM_WINDOWS
	uint64 FileTimeToU64(const FILETIME& Ft)
	{
		ULARGE_INTEGER Li;
		Li.LowPart = Ft.dwLowDateTime;
		Li.HighPart = Ft.dwHighDateTime;
		return Li.QuadPart;
	}

	// One reading of the box's and this process's cumulative CPU times, in 100 ns units.
	struct FTimesReading
	{
		uint64 Idle = 0;
		uint64 Kernel = 0; // GetSystemTimes' kernel time INCLUDES idle time
		uint64 User = 0;
		uint64 Process = 0;
		bool bOk = false;
	};

	FTimesReading ReadTimes()
	{
		FTimesReading R;
		FILETIME Idle, Kernel, User, Creation, Exit, PKernel, PUser;
		if (::GetSystemTimes(&Idle, &Kernel, &User) == 0 ||
			::GetProcessTimes(::GetCurrentProcess(), &Creation, &Exit, &PKernel, &PUser) == 0)
		{
			return R;
		}
		R.Idle = FileTimeToU64(Idle);
		R.Kernel = FileTimeToU64(Kernel);
		R.User = FileTimeToU64(User);
		R.Process = FileTimeToU64(PKernel) + FileTimeToU64(PUser);
		R.bOk = true;
		return R;
	}
#endif

	// The sampler thread. Every kSampleIntervalSeconds it closes one sample tagged with the state at
	// its end; on StopAndJoin() it closes one final, shorter sample.
	class FLoadSampler : public FRunnable
	{
	public:
		FLoadSampler(const std::atomic<int32>& InState, double InOrigin, const FString& InLockPath, const FString& InLockToken, const std::atomic<bool>& InFailNow)
			: State(InState), FailNow(InFailNow), Origin(InOrigin), LockPath(InLockPath), LockToken(InLockToken) {}

		bool Start()
		{
#if PLATFORM_WINDOWS
			StopEvent = FPlatformProcess::GetSynchEventFromPool(true);
			Thread = FRunnableThread::Create(this, TEXT("SuperSLM Estimate Load Sampler"), 0, TPri_AboveNormal);
			return Thread != nullptr;
#else
			return false;
#endif
		}

		void StopAndJoin()
		{
			if (Thread != nullptr)
			{
				StopEvent->Trigger();
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
			if (StopEvent != nullptr)
			{
				FPlatformProcess::ReturnSynchEventToPool(StopEvent);
				StopEvent = nullptr;
			}
		}

		virtual uint32 Run() override
		{
#if PLATFORM_WINDOWS
			FTimesReading Prev = ReadTimes();
			if (!Prev.bOk)
			{
				bFailed = true;
				return 0;
			}
			double PrevSeconds = FPlatformTime::Seconds() - Origin;
			bool bStopping = false;
			while (!bStopping)
			{
				bStopping = StopEvent->Wait(static_cast<uint32>(kSampleIntervalSeconds * 1000.0));
				const FTimesReading Now = ReadTimes();
				if (!Now.bOk || FailNow.load(std::memory_order_acquire))
				{
					bFailed = true;
					return 0;
				}
				const uint64 Kernel = Now.Kernel - Prev.Kernel;
				const uint64 User = Now.User - Prev.User;
				const uint64 Idle = Now.Idle - Prev.Idle;
				const uint64 Capacity = Kernel + User;
				const uint64 Busy = Capacity > Idle ? Capacity - Idle : 0;
				const uint64 Own = Now.Process - Prev.Process;
				FLoadSample Sample;
				Sample.StartSeconds = PrevSeconds;
				Sample.EndSeconds = FPlatformTime::Seconds() - Origin;
				const int32 Packed = State.load(std::memory_order_acquire);
				Sample.Phase = StatePhase(Packed);
				Sample.Mode = StateMode(Packed);
				if (Capacity > 0)
				{
					Sample.BoxSharePercent = 100.0 * static_cast<double>(Busy) / static_cast<double>(Capacity);
					Sample.OwnSharePercent = 100.0 * static_cast<double>(Own) / static_cast<double>(Capacity);
					Sample.ForeignSharePercent = Busy > Own ? 100.0 * static_cast<double>(Busy - Own) / static_cast<double>(Capacity) : 0.0;
				}
				Sample.LockState = ClassifyBuildLock(LockPath, LockToken);
				Samples.Add(Sample);
				Prev = Now;
				PrevSeconds = Sample.EndSeconds;
			}
#endif
			return 0;
		}

		// Written only by the sampler thread; read only after StopAndJoin().
		TArray<FLoadSample> Samples;
		bool bFailed = false;

	private:
		const std::atomic<int32>& State;
		const std::atomic<bool>& FailNow;
		double Origin = 0.0;
		FString LockPath;
		FString LockToken;
		FRunnableThread* Thread = nullptr;
		FEvent* StopEvent = nullptr;
	};

	// N for the own-share floor: every logical processor GetSystemTimes' capacity sums over.
	int32 BoxLogicalProcessors()
	{
#if PLATFORM_WINDOWS
		const DWORD Count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
		if (Count > 0)
		{
			return static_cast<int32>(Count);
		}
#endif
		return FMath::Max(1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	}

	// --- One mode's reduction ------------------------------------------------------------------------

	struct FArmRow
	{
		FString Arm;
		int64 Depth = 0;
		int32 Calls = 0;
		double MedianMs = 0.0;
		double IqrMs = 0.0;
	};

	struct FModeResult
	{
		int32 Mode = 0;
		FPerCallTrace Trace;
		TArray<FArmRow> Rows;
		FSuperSLMResolvedCpuConfig Suggested;          // the thirteen exact suggestions, and the per-C pairs
		TArray<FString> SuggestedText;                 // in SuperSLMRuntimeResolve::CostFields() order, rounded up
		// Rev 1.3.6 (P1.1): the C = 2..7 pairs, rounded up as the chunk-1 pair is, indexed by C.
		FString PerChunkTokenText[8];
		FString PerChunkPerPositionText[8];
		double TimedSeconds = 0.0;
		// Precache U4: the restore_size calls, their per-depth readings and the per-MiB suggestion.
		FPerCallTrace RestoreSizeTrace;
		TArray<SuperSLMCore::FRestoreSizeReading> RestoreSizes;
		TArray<int64> RestoreSizeDepths;
		FString PerMiBText;                            // rounded up to 6 decimals
		// Precache U5 (§15.31): the prefix_prefill arm's readings and its pair; no pair on the named skip.
		TArray<SuperSLMCore::FPrefixPrefillReading> PrefixPrefillReadings;
		bool bPrefixPrefillPair = false;
		FString PrefixPrefillSkipReason;
		FString PrefixPrefillTokenText;                // rounded up to 3 decimals
		FString PrefixPrefillSlopeText;                // rounded up to 6 decimals
	};

	// The arm/depth groups one mode times, and how many calls each must hold.
	TArray<TPair<FString, int64>> ExpectedGroups(const FEstimateSession& Session)
	{
		TArray<TPair<FString, int64>> Out = {
			{ TEXT("finish"), 1 }, { TEXT("reset"), -1 }, { TEXT("save"), 0 }, { TEXT("restore"), 0 },
			{ TEXT("adopt"), 1 }, { TEXT("prefix_begin"), -1 }, { TEXT("prefix_release"), -1 } };
		for (const int64 D : Session.Depths)
		{
			Out.Add({ TEXT("prompt_1"), D });
			Out.Add({ TEXT("layer"), D });
		}
		for (const int64 D : Session.BatchedDepths)
		{
			Out.Add({ TEXT("prompt_8"), D });
		}
		for (int32 C = kSmallChunkMin; C <= kSmallChunkMax; ++C)
		{
			for (const int64 D : Session.BatchedDepths)
			{
				Out.Add({ kSmallChunkArms[C], D });
			}
		}
		return Out;
	}

	int32 ExpectedCallsFor(const FString& Arm, const FEstimateSession& Session)
	{
		return Arm == TEXT("layer") ? kTimedReps * (Session.NumLayers - 2) : kTimedReps;
	}

	// Rows, statistics and the thirteen suggestions of one mode's timed calls. Returns false, with
	// the reason, on an incomplete arm, a non-finite cost or a failed batched premise.
	bool ReduceMode(FModeResult& Result, const FEstimateSession& Session, bool bRaiseBatchedForFault, FString& OutError)
	{
		TMap<FString, TArray<double>> Groups;
		for (const FCallTiming& Call : Result.Trace.Calls)
		{
			Groups.FindOrAdd(FString::Printf(TEXT("%s@%lld"), Call.Arm, Call.Depth)).Add(Call.ElapsedMs());
		}
		TMap<FString, double> Medians;
		for (const TPair<FString, int64>& Group : ExpectedGroups(Session))
		{
			const FString Key = FString::Printf(TEXT("%s@%lld"), *Group.Key, Group.Value);
			TArray<double>* Values = Groups.Find(Key);
			const int32 Want = ExpectedCallsFor(Group.Key, Session);
			const int32 Got = Values ? Values->Num() : 0;
			if (Got != Want)
			{
				OutError = FString::Printf(TEXT("mode %d %s holds %d of its %d timed calls (an incomplete arm)"), Result.Mode, *Key, Got, Want);
				return false;
			}
			for (const double Ms : *Values)
			{
				if (!IsValidCostMs(Ms))
				{
					OutError = FString::Printf(TEXT("mode %d %s has a call of %f ms, not a finite, positive cost"), Result.Mode, *Key, Ms);
					return false;
				}
			}
			FArmRow Row;
			Row.Arm = Group.Key;
			Row.Depth = Group.Value;
			Row.Calls = Got;
			Row.MedianMs = ConventionalMedian(*Values); // sorts Values
			Row.IqrMs = Type7Quantile(*Values, 0.75) - Type7Quantile(*Values, 0.25);
			if (!IsValidCostMs(Row.MedianMs) || !FMath::IsFinite(Row.IqrMs))
			{
				OutError = FString::Printf(TEXT("mode %d %s: median %f ms, IQR %f ms is not a finite cost"), Result.Mode, *Key, Row.MedianMs, Row.IqrMs);
				return false;
			}
			Medians.Add(Key, Row.MedianMs);
			Result.Rows.Add(Row);
		}

		FSuperSLMResolvedCpuConfig& S = Result.Suggested;
		auto MedianOf = [&Medians](const TCHAR* Arm, int64 Depth) { return Medians.FindChecked(FString::Printf(TEXT("%s@%lld"), Arm, Depth)); };
		S.FinishCostMs = MedianOf(TEXT("finish"), 1);
		S.ResetCostMs = MedianOf(TEXT("reset"), -1);
		S.SaveCostMs = MedianOf(TEXT("save"), 0);
		S.RestoreCostMs = MedianOf(TEXT("restore"), 0);
		S.AdoptCostMs = MedianOf(TEXT("adopt"), 1);
		S.PrefixBeginCostMs = MedianOf(TEXT("prefix_begin"), -1);
		S.PrefixReleaseCostMs = MedianOf(TEXT("prefix_release"), -1);
		TArray<FDepthArmReading> LayerArm, PromptArm, BatchedArm;
		for (const int64 D : Session.Depths)
		{
			FDepthArmReading L;
			L.Depth = D;
			L.MedianMs = MedianOf(TEXT("layer"), D);
			LayerArm.Add(L);
			FDepthArmReading P;
			P.Depth = D;
			P.MedianMs = MedianOf(TEXT("prompt_1"), D);
			PromptArm.Add(P);
		}
		FitUpperEnvelope(LayerArm, S.LayerCostMs, S.LayerCostPerPositionMs);
		FitUpperEnvelope(PromptArm, S.PromptTokenCostMs, S.PromptTokenCostPerPositionMs);
		if (Session.BatchedDepths.IsEmpty())
		{
			BatchedArmSkipValues(Result.Mode == 1, S.BatchedPromptTokenCostMs, S.BatchedPromptTokenCostPerPositionMs);
			UE_LOG(LogSuperSLM, Display, TEXT("%sbatched mode=%d skipped: %s; the mode's one-time pair is suggested."), kPrefix, Result.Mode, *Session.BatchedArmSkipReason);
		}
		else
		{
			for (const int64 D : Session.BatchedDepths)
			{
				FDepthArmReading R = MakeBatchedReading(D, D, MedianOf(TEXT("prompt_8"), D), kTimedReps);
				if (bRaiseBatchedForFault)
				{
					// FSuperSLMEstimateTestAccess::BatchedPremise: above the chunk-1 intercept, before the fit.
					R.MedianMs = FMath::Max(R.MedianMs, S.PromptTokenCostMs) + 1.0;
				}
				BatchedArm.Add(R);
			}
			if (!FitBatchedArm(BatchedArm, S.PromptTokenCostMs, S.BatchedPromptTokenCostMs, S.BatchedPromptTokenCostPerPositionMs, OutError))
			{
				return false;
			}
		}

		// Rev 1.3.6 (P1.1): the C = 2..7 pairs from their arms (FitSmallChunkArm, the upper envelope
		// at each chunk's mean position). With the batched arm skipped they are skipped too, and the
		// chunk-1 pair just fitted is suggested in their place: the plan's default until B5f.
		for (int32 C = kSmallChunkMin; C <= kSmallChunkMax; ++C)
		{
			SuperSLMPrefillPlanning::FPromptPrice Pair{ S.PromptTokenCostMs, S.PromptTokenCostPerPositionMs };
			if (!Session.BatchedDepths.IsEmpty())
			{
				TArray<double> CallMedians;
				for (const int64 D : Session.BatchedDepths)
				{
					CallMedians.Add(MedianOf(kSmallChunkArms[C], D));
				}
				if (!SuperSLMPrefillPlanning::FitSmallChunkArm(C, Session.BatchedDepths.GetData(), CallMedians.GetData(), CallMedians.Num(), Pair))
				{
					OutError = FString::Printf(TEXT("mode %d %s arm: its readings could not be fitted"), Result.Mode, kSmallChunkArms[C]);
					return false;
				}
			}
			if (!IsValidCostMs(Pair.TokenMs) || !FMath::IsFinite(Pair.PerPositionMs) || Pair.PerPositionMs < 0.0)
			{
				OutError = FString::Printf(TEXT("mode %d suggested PromptChunk%d pair (%f, %f) is not a finite, positive cost and a finite, non-negative slope"),
					Result.Mode, C, Pair.TokenMs, Pair.PerPositionMs);
				return false;
			}
			S.PerChunkPromptTokenCostMs[C] = Pair.TokenMs;
			S.PerChunkPromptTokenCostPerPositionMs[C] = Pair.PerPositionMs;
			Result.PerChunkTokenText[C] = RoundUpText(Pair.TokenMs, 3);
			Result.PerChunkPerPositionText[C] = RoundUpText(Pair.PerPositionMs, 6);
		}

		for (const SuperSLMRuntimeResolve::FCostField& Field : SuperSLMRuntimeResolve::CostFields())
		{
			const double Value = S.*(Field.Member);
			if (!SuperSLMRuntimeResolve::IsInFieldDomain(Field, Value))
			{
				OutError = FString::Printf(TEXT("mode %d suggested %s = %f is not a finite, %s"), Result.Mode, Field.Name, Value,
					Field.bPerPosition ? TEXT("non-negative slope") : TEXT("positive cost"));
				return false;
			}
			Result.SuggestedText.Add(RoundUpText(Value, Field.bPerPosition ? 6 : 3));
		}

		// Precache U4: the per-MiB term, the slope between the smallest and largest blobs of at least 1 MiB.
		TMap<int64, TArray<double>> SizeGroups;
		for (const FCallTiming& Call : Result.RestoreSizeTrace.Calls)
		{
			SizeGroups.FindOrAdd(Call.Depth).Add(Call.ElapsedMs());
		}
		Result.RestoreSizes.Reset();
		Result.RestoreSizeDepths.Reset();
		for (const int64 D : Session.Depths)
		{
			if (!Session.IsSizedDepth(D))
			{
				continue; // under 1 MiB: not measured, not fitted
			}
			TArray<double>* Values = SizeGroups.Find(D);
			const int32 Got = Values ? Values->Num() : 0;
			if (Got != kTimedReps)
			{
				OutError = FString::Printf(TEXT("mode %d restore_size@%lld holds %d of its %d timed calls (an incomplete arm)"), Result.Mode, D, Got, kTimedReps);
				return false;
			}
			for (const double Ms : *Values)
			{
				if (!IsValidCostMs(Ms))
				{
					OutError = FString::Printf(TEXT("mode %d restore_size@%lld has a call of %f ms, not a finite, positive cost"), Result.Mode, D, Ms);
					return false;
				}
			}
			SuperSLMCore::FRestoreSizeReading Reading;
			Reading.PayloadBytes = Session.BlobBytesAt(D);
			Reading.MedianMs = ConventionalMedian(*Values);
			Result.RestoreSizes.Add(Reading);
			Result.RestoreSizeDepths.Add(D);
		}
		if (!SuperSLMCore::FitRestorePerMiB(S.RestoreCostMs, Result.RestoreSizes.GetData(), static_cast<size_t>(Result.RestoreSizes.Num()), S.RestoreCostPerMiBMs))
		{
			OutError = FString::Printf(TEXT("mode %d: the restore_size readings give no per-MiB slope (no blob of at least 1 MiB, or a median was not finite)"), Result.Mode);
			return false;
		}
		Result.PerMiBText = RoundUpText(S.RestoreCostPerMiBMs, 6);

		// Precache U5 (§15.31): the prefix_prefill pair, from the arm's chunks (a named skip writes none).
		Result.bPrefixPrefillPair = false;
		if (Session.PrefixPrefillArmDepth() == 0)
		{
			Result.PrefixPrefillSkipReason = FString::Printf(TEXT("the cap %lld holds fewer than %d chunks of %d tokens"),
				Session.Cap, SuperSLMCore::kPrefixPrefillFitGroups, SuperSLMCore::kCacheChunkTiledMinTokens);
			return true;
		}
		const int64 ExpectedChunks = Session.PrefixPrefillArmDepth() / SuperSLMCore::kCacheChunkTiledMinTokens;
		if (Result.PrefixPrefillReadings.Num() != ExpectedChunks)
		{
			OutError = FString::Printf(TEXT("mode %d prefix_prefill holds %d of its %lld chunks (an incomplete arm)"), Result.Mode, Result.PrefixPrefillReadings.Num(), ExpectedChunks);
			return false;
		}
		for (const SuperSLMCore::FPrefixPrefillReading& R : Result.PrefixPrefillReadings)
		{
			if (!IsValidCostMs(R.Ms))
			{
				OutError = FString::Printf(TEXT("mode %d prefix_prefill@%lld has a call of %f ms, not a finite, positive cost"), Result.Mode, R.StartDepth, R.Ms);
				return false;
			}
		}
		SuperSLMCore::FChunkPricePair Pair;
		if (!SuperSLMCore::FitPrefixPrefillPair(Result.PrefixPrefillReadings.GetData(), static_cast<size_t>(Result.PrefixPrefillReadings.Num()),
				SuperSLMCore::kPrefixPrefillFitGroups, Pair))
		{
			OutError = FString::Printf(TEXT("mode %d: the prefix_prefill readings give no price pair"), Result.Mode);
			return false;
		}
		Result.bPrefixPrefillPair = true;
		Result.PrefixPrefillTokenText = RoundUpText(Pair.TokenMs, 3);
		Result.PrefixPrefillSlopeText = RoundUpText(Pair.PerPositionMs, 6);
		return true;
	}

	// Median, type-7 p95 and max of a set of shares.
	struct FLoadSummary
	{
		int32 Samples = 0;
		double Median = 0.0, P95 = 0.0, Max = 0.0, OwnMedian = 0.0;
	};

	FLoadSummary Summarize(const TArray<FLoadSample>& Samples, EEstimatePhase Phase, int32 Mode)
	{
		TArray<double> Foreign, Own;
		for (const FLoadSample& S : Samples)
		{
			if (S.Phase == Phase && (Mode < 0 || S.Mode == Mode))
			{
				Foreign.Add(S.ForeignSharePercent);
				Own.Add(S.OwnSharePercent);
			}
		}
		FLoadSummary Out;
		Out.Samples = Foreign.Num();
		if (Out.Samples > 0)
		{
			Out.Median = ConventionalMedian(Foreign);
			Out.P95 = Type7Quantile(Foreign, 0.95);
			Out.Max = Foreign.Last();
			Out.OwnMedian = ConventionalMedian(Own);
		}
		return Out;
	}

	void LogLoad(const TCHAR* Phase, int32 Mode, const FLoadSummary& L)
	{
		UE_LOG(LogSuperSLM, Display, TEXT("%sload phase=%s%s samples=%d median_pct=%.6f p95_pct=%.6f max_pct=%.6f own_median_pct=%.6f"),
			kPrefix, Phase, Mode >= 0 ? *FString::Printf(TEXT(" mode=%d"), Mode) : TEXT(""), L.Samples, L.Median, L.P95, L.Max, L.OwnMedian);
	}

	// The calls CSV row for each timed call, with the overlap-weighted foreign share of the samples it
	// spans (T-3050d's t3050d-per-call-v1 columns). Returns the calls under 99.9 % sample coverage.
	int32 WriteCallsCsv(const FString& Path, const FString& Block, const FModeResult& Result, const TArray<FLoadSample>& Samples, bool& bOutSaved)
	{
		FString Csv = TEXT("call_id,block,mode,arm,depth,rep,layer,start_s,end_s,elapsed_ms,foreign_pct,foreign_peak_pct,foreign_coverage,first_sample,last_sample\n");
		int32 Uncovered = 0;
		for (int32 I = 0; I < Result.Trace.Calls.Num(); ++I)
		{
			const FCallTiming& C = Result.Trace.Calls[I];
			double Covered = 0.0, Weighted = 0.0, Peak = 0.0;
			int32 First = -1, Last = -1;
			for (int32 J = 0; J < Samples.Num(); ++J)
			{
				const FLoadSample& S = Samples[J];
				const double Overlap = FMath::Max(0.0, FMath::Min(C.EndSeconds, S.EndSeconds) - FMath::Max(C.StartSeconds, S.StartSeconds));
				if (Overlap > 0.0)
				{
					if (First < 0) { First = J; }
					Last = J;
					Covered += Overlap;
					Weighted += Overlap * S.ForeignSharePercent;
					Peak = FMath::Max(Peak, S.ForeignSharePercent);
				}
			}
			const double Duration = C.EndSeconds - C.StartSeconds;
			const double Coverage = Duration > 0.0 ? Covered / Duration : 0.0;
			if (Coverage < 0.999) { ++Uncovered; }
			Csv += FString::Printf(TEXT("%d,%s,%d,%s,%lld,%d,%d,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%d,%d\n"),
				I, *Block, Result.Mode, C.Arm, C.Depth, C.Rep, C.Layer, C.StartSeconds, C.EndSeconds, C.ElapsedMs(),
				Covered > 0.0 ? Weighted / Covered : -1.0, Peak, Coverage, First, Last);
		}
		bOutSaved = FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Uncovered;
	}

	// Precache plan rev 1.18.12 (§15.32): the isolated prefix_prefill arm's chunks, one row each, for the attribution
	// reading the in-situ arm is compared with (an empty arm, the named skip, writes the header only).
	bool WritePrefixPrefillCsv(const FString& Path, const FString& Block, const FModeResult& Result)
	{
		FString Csv = TEXT("block,mode,start_depth,tokens,ms\n");
		for (const SuperSLMCore::FPrefixPrefillReading& R : Result.PrefixPrefillReadings)
		{
			Csv += FString::Printf(TEXT("%s,%d,%lld,%d,%.6f\n"), *Block, Result.Mode, static_cast<long long>(R.StartDepth), R.Tokens, R.Ms);
		}
		return FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteSamplesCsv(const FString& Path, const FString& Block, const TArray<FLoadSample>& Samples)
	{
		FString Csv = TEXT("sample_id,block,mode,start_s,end_s,phase,box_pct,own_pct,foreign_pct,lock_state\n");
		for (int32 I = 0; I < Samples.Num(); ++I)
		{
			const FLoadSample& S = Samples[I];
			Csv += FString::Printf(TEXT("%d,%s,%d,%.9f,%.9f,%s,%.6f,%.6f,%.6f,%s\n"),
				I, *Block, S.Mode, S.StartSeconds, S.EndSeconds, PhaseName(S.Phase), S.BoxSharePercent,
				S.OwnSharePercent, S.ForeignSharePercent, LockStateName(S.LockState));
		}
		return FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString PluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		FString Version = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown"));
		Version.ReplaceCharInline(TEXT(' '), TEXT('_'));
		return Version.IsEmpty() ? FString(TEXT("unknown")) : Version;
	}

	void LogUsage()
	{
		UE_LOG(LogSuperSLM, Error, TEXT("Usage: SuperSLM.EstimateCosts \"<ArtifactPath.sslm>\" [--write-ini] -- measures both CPU decode modes on this machine and prints a CALIBRATION ESTIMATE (not verified) of the thirteen per-unit costs; only --write-ini saves them to Saved/SuperSLM/CalibratedCosts.ini, which the next CPU Configure() reads for a matching artifact, decode mode and task count."));
	}

	void RunEstimateCostsCommand(const TArray<FString>& Args)
	{
		// The argument list: one artifact path, then at most --write-ini.
		bool bWriteIni = false;
		if (Args.Num() < 1 || Args.Num() > 2 || Args[0].StartsWith(TEXT("--")))
		{
			LogUsage();
			return;
		}
		if (Args.Num() == 2)
		{
			if (Args[1] != TEXT("--write-ini"))
			{
				LogUsage();
				return;
			}
			bWriteIni = true;
		}
		const FString ArtifactPath = Args[0];
		const double RunStart = FPlatformTime::Seconds();

#if WITH_DEV_AUTOMATION_TESTS
		const ESuperSLMEstimateFault Fault = GNextFault;
		GNextFault = ESuperSLMEstimateFault::None;
		GResetObservations.Reset();
#endif

		// The artifact is checked before the run opens: one that cannot be imported is a named
		// incomplete estimate with no report, so a log's report lines always belong to a run that
		// measured something.
		FSuperSLMImportDiagnostic Diag;
		TStrongObjectPtr<USuperSLMModel> Model(FSuperSLMModelImport::ImportFromFile(ArtifactPath, Diag));
		uint8 Hash[32] = {};
		if (!Model.IsValid() || !Diag.bAccepted || !SuperSLMRuntime::ReadArtifactHash(*Model.Get(), Hash))
		{
			UE_LOG(LogSuperSLM, Error, TEXT("%sincomplete estimate: the artifact '%s' could not be imported (%s); no run was started and nothing is written."),
				kPrefix, *ArtifactPath, *Diag.Message);
			return;
		}

		UE_LOG(LogSuperSLM, Display, TEXT("%sCALIBRATION ESTIMATE — not verified"), kPrefix);
		auto Incomplete = [&](const FString& Reason)
		{
			UE_LOG(LogSuperSLM, Error, TEXT("%sincomplete estimate: %s"), kPrefix, *Reason);
			UE_LOG(LogSuperSLM, Display, TEXT("%srun status=incomplete elapsed_s=%.3f"), kPrefix, FPlatformTime::Seconds() - RunStart);
		};

		// D-SLM8140: the task count Configure() would resolve with no caller value.
		bool bUnusedMode = false;
		int32 ParallelTasks = SuperSLMRuntimeResolve::kShippedParallelTasks;
		SuperSLMRuntimeResolve::ResolveUncalledSettings(bUnusedMode, ParallelTasks);
		const FString HashHex = SuperSLMRuntime::HashToHex(Hash);
		const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		UE_LOG(LogSuperSLM, Display, TEXT("%srun id=%s artifact_hash=%s FinishParallelTasks=%d engine=%s plugin=%s artifact=\"%s\""),
			kPrefix, *RunId, *HashHex, ParallelTasks, *FEngineVersion::Current().ToString(EVersionComponent::Patch), *PluginVersion(), *ArtifactPath);

		// Untimed setup of both modes, before the sample series.
		FEstimateSession Sessions[2];
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("%sprogress mode=%d phase=setup"), kPrefix, Mode);
			FString Error;
			if (!Sessions[Mode].Setup(*Model.Get(), Mode == 1, ParallelTasks, Error))
			{
				Incomplete(FString::Printf(TEXT("mode %d setup: %s"), Mode, *Error));
				return;
			}
		}
		UE_LOG(LogSuperSLM, Display, TEXT("%ssetup cap=%lld layers=%d depths=%s batched_depths=%s timed_calls_per_mode=%d"),
			kPrefix, Sessions[0].Cap, Sessions[0].NumLayers,
			*FString::JoinBy(Sessions[0].Depths, TEXT(","), [](int64 D) { return FString::Printf(TEXT("%lld"), D); }),
			Sessions[0].BatchedDepths.IsEmpty() ? TEXT("skipped") : *FString::JoinBy(Sessions[0].BatchedDepths, TEXT(","), [](int64 D) { return FString::Printf(TEXT("%lld"), D); }),
			Sessions[0].ExpectedCallCount(kTimedReps));

		// The one sample series: pre-window, each mode's warm-up and timed workload, post-window.
		std::atomic<int32> State{ PackState(EEstimatePhase::Pre, -1) };
		std::atomic<bool> SamplerFailNow{ false };
		const double Origin = FPlatformTime::Seconds();
		FLoadSampler Sampler(State, Origin, BuildLockPath(), FPlatformMisc::GetEnvironmentVariable(TEXT("SSU_BUILD_LOCK_TOKEN")).TrimStartAndEnd(), SamplerFailNow);
		if (!Sampler.Start())
		{
			Incomplete(TEXT("the foreign-load sampler could not start on this platform"));
			return;
		}
		UE_LOG(LogSuperSLM, Display, TEXT("%sprogress phase=pre seconds=%.1f"), kPrefix, kPreWindowSeconds);
		FPlatformProcess::Sleep(static_cast<float>(kPreWindowSeconds));

		FModeResult Results[2];
		FString MeasureError;
		bool bMeasured = true;
		for (int32 Mode = 0; Mode < 2 && bMeasured; ++Mode)
		{
			FEstimateSession& Session = Sessions[Mode];
#if WITH_DEV_AUTOMATION_TESTS
			const bool bFaultMode = Mode == 1;
#endif
			bool bInterruptNow = false;
			FMeasureControl Control;
			Control.ShouldStop = [&bInterruptNow, Mode](FString& OutWhy)
			{
				if (IsEngineExitRequested())
				{
					OutWhy = FString::Printf(TEXT("interrupted during mode %d: the engine is exiting"), Mode);
					return true;
				}
				if (bInterruptNow)
				{
					OutWhy = FString::Printf(TEXT("interrupted during mode %d"), Mode);
					return true;
				}
				return false;
			};

			State.store(PackState(EEstimatePhase::WarmUp, Mode), std::memory_order_release);
			FPerCallTrace WarmUp;
			WarmUp.Origin = Origin;
			WarmUp.Calls.Reserve(Session.ExpectedCallCount(kWarmUpReps));
			if (!Session.Measure(kWarmUpReps, WarmUp, Control, MeasureError))
			{
				MeasureError = FString::Printf(TEXT("mode %d warm-up: %s"), Mode, *MeasureError);
				bMeasured = false;
				break;
			}
			{
				// Precache U4: the restore_size pass's own warm-up; its calls are not the thirteen's, so they are not
				// counted on the warm-up line.
				FPerCallTrace SizeWarmUp;
				SizeWarmUp.Origin = Origin;
				if (!Session.MeasureRestoreSizes(kWarmUpReps, SizeWarmUp, MeasureError))
				{
					MeasureError = FString::Printf(TEXT("mode %d warm-up: %s"), Mode, *MeasureError);
					bMeasured = false;
					break;
				}
				// Precache U5 (§15.31): the prefix_prefill arm's warm-up, a short build of the fit's group count of chunks.
				TArray<SuperSLMCore::FPrefixPrefillReading> PrefillWarmUp;
				if (Session.PrefixPrefillArmDepth() > 0
					&& !Session.MeasurePrefixPrefill(static_cast<int64>(SuperSLMCore::kPrefixPrefillFitGroups) * SuperSLMCore::kCacheChunkTiledMinTokens, PrefillWarmUp, MeasureError))
				{
					MeasureError = FString::Printf(TEXT("mode %d warm-up: %s"), Mode, *MeasureError);
					bMeasured = false;
					break;
				}
			}
			UE_LOG(LogSuperSLM, Display, TEXT("%sprogress mode=%d phase=warm-up reps=%d calls=%d"), kPrefix, Mode, kWarmUpReps, WarmUp.Calls.Num());

			// The fault seam fires here, in mode 1, after its hook was installed and mode 0 completed.
#if WITH_DEV_AUTOMATION_TESTS
			if (bFaultMode)
			{
				SamplerFailNow.store(Fault == ESuperSLMEstimateFault::SamplerFailure, std::memory_order_release);
				bInterruptNow = Fault == ESuperSLMEstimateFault::InterruptDuringMode1;
				Control.bInjectAbiRefusal = Fault == ESuperSLMEstimateFault::InvalidAbiResult;
			}
#endif
			UE_LOG(LogSuperSLM, Display, TEXT("%sprogress mode=%d phase=timed reps=%d expected_calls=%d"), kPrefix, Mode, kTimedReps, Session.ExpectedCallCount(kTimedReps));
			State.store(PackState(EEstimatePhase::Workload, Mode), std::memory_order_release);
			FModeResult& Result = Results[Mode];
			Result.Mode = Mode;
			Result.Trace.Origin = Origin;
			Result.Trace.Calls.Reserve(Session.ExpectedCallCount(kTimedReps));
			const double TimedStart = FPlatformTime::Seconds();
			if (!Session.Measure(kTimedReps, Result.Trace, Control, MeasureError))
			{
				MeasureError = FString::Printf(TEXT("mode %d: %s"), Mode, *MeasureError);
				bMeasured = false;
				break;
			}
			Result.TimedSeconds = FPlatformTime::Seconds() - TimedStart;
			Result.RestoreSizeTrace.Origin = Origin;
			if (!Session.MeasureRestoreSizes(kTimedReps, Result.RestoreSizeTrace, MeasureError))
			{
				MeasureError = FString::Printf(TEXT("mode %d: %s"), Mode, *MeasureError);
				bMeasured = false;
				break;
			}
			if (Session.PrefixPrefillArmDepth() > 0
				&& !Session.MeasurePrefixPrefill(Session.PrefixPrefillArmDepth(), Result.PrefixPrefillReadings, MeasureError))
			{
				MeasureError = FString::Printf(TEXT("mode %d: %s"), Mode, *MeasureError);
				bMeasured = false;
				break;
			}
			UE_LOG(LogSuperSLM, Display, TEXT("%sprogress mode=%d phase=done calls=%d elapsed_s=%.3f"), kPrefix, Mode, Result.Trace.Calls.Num(), Result.TimedSeconds);
#if WITH_DEV_AUTOMATION_TESTS
			if (bFaultMode && Fault == ESuperSLMEstimateFault::TruncateArm && Result.Trace.Calls.Num() > 0)
			{
				Result.Trace.Calls.RemoveAt(Result.Trace.Calls.Num() - 1); // the last call of the last arm
			}
			if (bFaultMode && Fault == ESuperSLMEstimateFault::NonfiniteCost && Result.Trace.Calls.Num() > 0)
			{
				Result.Trace.Calls[0].EndSeconds = std::numeric_limits<double>::quiet_NaN();
			}
#endif
		}
		State.store(PackState(EEstimatePhase::Post, -1), std::memory_order_release);
		UE_LOG(LogSuperSLM, Display, TEXT("%sprogress phase=post seconds=%.1f"), kPrefix, kPostWindowSeconds);
		FPlatformProcess::Sleep(static_cast<float>(kPostWindowSeconds));
		Sampler.StopAndJoin();

		if (!bMeasured)
		{
			Incomplete(MeasureError);
			return;
		}
		if (Sampler.bFailed || Sampler.Samples.Num() == 0)
		{
			Incomplete(TEXT("the foreign-load sampler failed during the run, so the load the estimate ran under is unknown"));
			return;
		}
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FString ReduceError;
			bool bRaise = false;
#if WITH_DEV_AUTOMATION_TESTS
			bRaise = Mode == 1 && Fault == ESuperSLMEstimateFault::BatchedPremise;
#endif
			if (!ReduceMode(Results[Mode], Sessions[Mode], bRaise, ReduceError))
			{
				Incomplete(ReduceError);
				return;
			}
		}

		// Diagnostics, independent of --write-ini: the per-call and per-sample series.
		const FString FilesDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/Estimates") / RunId);
		IFileManager::Get().MakeDirectory(*FilesDir, true);
		int32 Uncovered = 0;
		bool bFilesSaved = true;
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			bool bSaved = false;
			Uncovered += WriteCallsCsv(FilesDir / FString::Printf(TEXT("mode%d.calls.csv"), Mode), FString::Printf(TEXT("%s-m%d"), *RunId, Mode), Results[Mode], Sampler.Samples, bSaved);
			bFilesSaved &= bSaved;
			bFilesSaved &= WritePrefixPrefillCsv(FilesDir / FString::Printf(TEXT("mode%d.prefix_prefill.csv"), Mode), FString::Printf(TEXT("%s-m%d"), *RunId, Mode), Results[Mode]);
		}
		bFilesSaved &= WriteSamplesCsv(FilesDir / TEXT("samples.csv"), RunId, Sampler.Samples);
		if (!bFilesSaved)
		{
			Incomplete(FString::Printf(TEXT("the per-call and per-sample files could not be written to %s"), *FilesDir));
			return;
		}

		// The report.
		for (const FModeResult& Result : Results)
		{
			for (const FArmRow& Row : Result.Rows)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%srow mode=%d arm=%s depth=%lld calls=%d median_ms=%.6f iqr_ms=%.6f"),
					kPrefix, Result.Mode, *Row.Arm, Row.Depth, Row.Calls, Row.MedianMs, Row.IqrMs);
			}
			const TConstArrayView<SuperSLMRuntimeResolve::FCostField> Fields = SuperSLMRuntimeResolve::CostFields();
			for (int32 I = 0; I < Fields.Num(); ++I)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%ssuggested mode=%d %s=%s"), kPrefix, Result.Mode, Fields[I].Name, *Result.SuggestedText[I]);
			}
			// Rev 1.3.6 (P1.1): the per-C pairs, on their own lines, so the thirteen-field contract stands.
			for (int32 C = kSmallChunkMin; C <= kSmallChunkMax; ++C)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%sper_chunk mode=%d chunk=%d %s=%s %s=%s"), kPrefix, Result.Mode, C,
					SuperSLMRuntimeResolve::PerChunkTokenCostKey(C), *Result.PerChunkTokenText[C],
					SuperSLMRuntimeResolve::PerChunkTokenCostPerPositionKey(C), *Result.PerChunkPerPositionText[C]);
			}
			// Precache U4: the restore-by-size readings and the per-MiB term, on lines of their own.
			for (int32 I = 0; I < Result.RestoreSizes.Num(); ++I)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%srestore_size mode=%d depth=%lld bytes=%lld mib=%.6f calls=%d median_ms=%.6f"),
					kPrefix, Result.Mode, Result.RestoreSizeDepths[I], Result.RestoreSizes[I].PayloadBytes,
					static_cast<double>(Result.RestoreSizes[I].PayloadBytes) / SuperSLMCore::kBytesPerMiB, kTimedReps, Result.RestoreSizes[I].MedianMs);
			}
			UE_LOG(LogSuperSLM, Display, TEXT("%ssuggested_per_mib mode=%d %s=%s"), kPrefix, Result.Mode, SuperSLMRuntimeResolve::RestorePerMiBKey(), *Result.PerMiBText);
			// Precache U5 (§15.31): the prefix_prefill arm, one line per fit group's span, and its pair.
			if (!Result.bPrefixPrefillPair)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%ssuggested_prefix_prefill mode=%d skipped=\"%s\" (the batched pair prices cache chunks)"), kPrefix, Result.Mode, *Result.PrefixPrefillSkipReason);
				continue;
			}
			{
				const int32 N = Result.PrefixPrefillReadings.Num();
				for (int32 G = 0; G < SuperSLMCore::kPrefixPrefillFitGroups; ++G)
				{
					const int32 Begin = static_cast<int32>(static_cast<int64>(N) * G / SuperSLMCore::kPrefixPrefillFitGroups);
					const int32 End = static_cast<int32>(static_cast<int64>(N) * (G + 1) / SuperSLMCore::kPrefixPrefillFitGroups);
					TArray<double> Ms;
					for (int32 I = Begin; I < End; ++I)
					{
						Ms.Add(Result.PrefixPrefillReadings[I].Ms);
					}
					UE_LOG(LogSuperSLM, Display, TEXT("%sprefix_prefill mode=%d depths=%lld-%lld chunk=%d calls=%d median_ms=%.6f"),
						kPrefix, Result.Mode, Result.PrefixPrefillReadings[Begin].StartDepth,
						Result.PrefixPrefillReadings[End - 1].StartDepth + Result.PrefixPrefillReadings[End - 1].Tokens,
						SuperSLMCore::kCacheChunkTiledMinTokens, End - Begin, ConventionalMedian(Ms));
				}
			}
			UE_LOG(LogSuperSLM, Display, TEXT("%ssuggested_prefix_prefill mode=%d %s=%s %s=%s"), kPrefix, Result.Mode,
				SuperSLMRuntimeResolve::PrefixPrefillTokenKey(), *Result.PrefixPrefillTokenText,
				SuperSLMRuntimeResolve::PrefixPrefillSlopeKey(), *Result.PrefixPrefillSlopeText);
		}
		LogLoad(TEXT("pre"), -1, Summarize(Sampler.Samples, EEstimatePhase::Pre, -1));
		LogLoad(TEXT("workload"), 0, Summarize(Sampler.Samples, EEstimatePhase::Workload, 0));
		LogLoad(TEXT("workload"), 1, Summarize(Sampler.Samples, EEstimatePhase::Workload, 1));
		LogLoad(TEXT("post"), -1, Summarize(Sampler.Samples, EEstimatePhase::Post, -1));
		int32 LockCounts[4] = {};
		for (const FLoadSample& S : Sampler.Samples)
		{
			++LockCounts[static_cast<int32>(S.LockState)];
		}
		const ELockState Reported = LockCounts[(int32)ELockState::Foreign] > 0 ? ELockState::Foreign
			: LockCounts[(int32)ELockState::Own] > 0 ? ELockState::Own
			: LockCounts[(int32)ELockState::Absent] > 0 ? ELockState::Absent
			: ELockState::NotConfigured;
		UE_LOG(LogSuperSLM, Display, TEXT("%scoverage uncovered_calls=%d samples=%d logical_processors=%d"), kPrefix, Uncovered, Sampler.Samples.Num(), BoxLogicalProcessors());
		UE_LOG(LogSuperSLM, Display, TEXT("%slock state=%s samples_absent=%d samples_own=%d samples_foreign=%d samples_not_configured=%d"),
			kPrefix, LockStateName(Reported), LockCounts[(int32)ELockState::Absent], LockCounts[(int32)ELockState::Own],
			LockCounts[(int32)ELockState::Foreign], LockCounts[(int32)ELockState::NotConfigured]);
		UE_LOG(LogSuperSLM, Display, TEXT("%sfiles %s"), kPrefix, *FilesDir);
		UE_LOG(LogSuperSLM, Display, TEXT("%snote reset_input_context_length=%lld reset_output_context_length=0; restoring and verifying the input fixture are outside the timed reset call."), kPrefix, Sessions[0].Cap - 1);
		UE_LOG(LogSuperSLM, Display, TEXT("%snote iqr_ms is the within-run spread of serial, autocorrelated calls, not a confidence interval."), kPrefix);
		UE_LOG(LogSuperSLM, Display, TEXT("%snote foreign shares are 250 ms interval averages of whole-box CPU time less this process's own, not per-call isolation; desktop load is disclosed here and never stops the estimate."), kPrefix);
		UE_LOG(LogSuperSLM, Display, TEXT("%snote suggestions are rounded up to 3 decimals (ms) or 6 (per position); this checks that the run executed completely, not that the costs are accurate."), kPrefix);
		UE_LOG(LogSuperSLM, Display, TEXT("%srun status=complete elapsed_s=%.3f"), kPrefix, FPlatformTime::Seconds() - RunStart);

		if (!bWriteIni)
		{
			return;
		}
		// The opt-in write, after the post-window of a complete run: both profiles, then every prior profile
		// for another artifact or task count, in one replace. The new profiles come first because Configure()
		// reads the first match.
		FString Text = FString::Printf(TEXT(
			"; SuperSLM.EstimateCosts output: CALIBRATION ESTIMATE - not verified.\n"
			"; Measured costs for this machine and artifact, rounded up; not certified costs or accuracy bounds.\n"
			"; The next CPU Configure() reads a profile only when its ArtifactHash, bParallelDecode (the effective\n"
			"; decode mode) and FinishParallelTasks match; a caller's explicit cost fields win field by field.\n"
			"; Run %s, artifact %s; workload foreign load median mode 0 %.3f%%, mode 1 %.3f%%.\n"),
			*RunId, *ArtifactPath, Summarize(Sampler.Samples, EEstimatePhase::Workload, 0).Median, Summarize(Sampler.Samples, EEstimatePhase::Workload, 1).Median);
		for (const FModeResult& Result : Results)
		{
			Text += FString::Printf(TEXT("[SuperSLM.EstimatedCosts.Mode%d]\nbParallelDecode=%d\nArtifactHash=%s\nFinishParallelTasks=%d\n"),
				Result.Mode, Result.Mode, *HashHex, ParallelTasks);
			const TConstArrayView<SuperSLMRuntimeResolve::FCostField> Fields = SuperSLMRuntimeResolve::CostFields();
			for (int32 I = 0; I < Fields.Num(); ++I)
			{
				Text += FString::Printf(TEXT("%s=%s\n"), Fields[I].Name, *Result.SuggestedText[I]);
			}
			// Precache U4: the cache restore's per-MiB term, after the thirteen (optional to the reader).
			Text += FString::Printf(TEXT("%s=%s\n"), SuperSLMRuntimeResolve::RestorePerMiBKey(), *Result.PerMiBText);
			for (int32 C = kSmallChunkMin; C <= kSmallChunkMax; ++C)
			{
				Text += FString::Printf(TEXT("%s=%s\n%s=%s\n"), SuperSLMRuntimeResolve::PerChunkTokenCostKey(C), *Result.PerChunkTokenText[C],
					SuperSLMRuntimeResolve::PerChunkTokenCostPerPositionKey(C), *Result.PerChunkPerPositionText[C]);
			}
			// Precache U5 (§15.31): the cache chunk's prefix_prefill pair, optional to the reader; none on a skip.
			if (Result.bPrefixPrefillPair)
			{
				Text += FString::Printf(TEXT("%s=%s\n%s=%s\n"), SuperSLMRuntimeResolve::PrefixPrefillTokenKey(), *Result.PrefixPrefillTokenText,
					SuperSLMRuntimeResolve::PrefixPrefillSlopeKey(), *Result.PrefixPrefillSlopeText);
			}
		}
		const FString OutPath = SuperSLMRuntimeResolve::CalibratedCostsPath();
		FString Existing;
		if (FPaths::FileExists(OutPath) && FFileHelper::LoadFileToString(Existing, *OutPath))
		{
			Text += SuperSLMRuntimeResolve::KeptEstimateProfiles(Existing, HashHex, ParallelTasks);
		}
		FString WriteError;
		if (SuperSLMRuntimeResolve::WriteTextFileAtomically(OutPath, Text, WriteError))
		{
			UE_LOG(LogSuperSLM, Display, TEXT("%swrote %s"), kPrefix, *OutPath);
		}
		else
		{
			UE_LOG(LogSuperSLM, Error, TEXT("%scould not write %s (%s); its prior bytes are unchanged."), kPrefix, *OutPath, *WriteError);
		}
	}

	static FAutoConsoleCommand GSuperSLMEstimateCostsCommand(
		TEXT("SuperSLM.EstimateCosts"),
		TEXT("SuperSLM.EstimateCosts \"<ArtifactPath.sslm>\" [--write-ini] -- measures both CPU decode modes on this machine (about 5-7 minutes for a 0.5B artifact) and prints a CALIBRATION ESTIMATE (not verified): per-arm medians and spread, thirteen suggested costs per mode, and the foreign CPU load it ran under. Only --write-ini saves the suggestions to Saved/SuperSLM/CalibratedCosts.ini, which the next CPU Configure() reads for a matching artifact, decode mode and task count. Editor and packaged."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunEstimateCostsCommand));
}

namespace SuperSLMEstimateCostsCommand
{
	void Register()
	{
		// FAutoConsoleCommand above self-registers at static-init time; this function exists so
		// SuperSLMUnrealModule.cpp can force this translation unit to link.
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FSuperSLMEstimateTestAccess::SetNextFault(ESuperSLMEstimateFault Fault)
{
	GNextFault = Fault;
}

void FSuperSLMEstimateTestAccess::ClearOverrides()
{
	GNextFault = ESuperSLMEstimateFault::None;
}

const TArray<FSuperSLMEstimateResetObservation>& FSuperSLMEstimateTestAccess::GetResetObservations()
{
	check(IsInGameThread());
	return GResetObservations;
}

#endif // WITH_DEV_AUTOMATION_TESTS
