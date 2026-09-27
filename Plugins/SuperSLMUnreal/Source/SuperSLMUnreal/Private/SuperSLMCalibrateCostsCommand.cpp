#include "SuperSLMCalibrateCostsCommand.h"

#include "SuperSLMCalibrationTestAccess.h"
#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMStatusMapping.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "superslm/sslm_abi.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <TlHelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>

// §10.2 item 10 (plan §5 item 2, D-SLM7414/D-SLM7422/D-SLM7430/D-SLM7444/D-SLM7450): the
// calibration console command the plan names as a required §10.2 acceptance step, built here for
// the first time -- the seven measured medians this plan cites (T-2815 §11.1, D-SLM7444) were
// produced ahead of this command by the identical standalone-harness pattern, run outside the
// shipped console-command surface; this file gives the project that surface, driving the SAME
// ABI calls the plugin itself makes (never a separate harness binary) against the project's own
// artifact and box.
//
// NOT run by the build: the maintainer runs the acceptance calibration (plan §10.3.1 item 5b),
// graded by the idleness criterion D-SLM7778 states (RunUnderIdlenessEvidence, below).

namespace
{
	double Median(TArray<double>& Values)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		return Values[Values.Num() / 2];
	}

#if WITH_DEV_AUTOMATION_TESTS
	// Plan §10.3.1 item 5a.2's condition R (the must-reject): the FinishParallelTasks the next
	// runs install, set by FSuperSLMCalibrationTestAccess::SetFinishParallelTasksOverride. -1 is
	// unset, and a production run never reads anything else.
	int32 GTestFinishParallelTasksOverride = -1;
#endif

	// The FinishParallelTasks a run installs: FSuperSLMRuntimeConfig's shipped default, or the
	// automation-only override when a test has set one.
	int32 EffectiveFinishParallelTasks()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GTestFinishParallelTasksOverride >= 0)
		{
			return GTestFinishParallelTasksOverride;
		}
#endif
		return FSuperSLMRuntimeConfig().FinishParallelTasks;
	}

	// Plan §2.5 row 2 (D-SLM7663): installs the runtime's own finish hook on a calibration
	// workspace, at FSuperSLMRuntimeConfig's shipped default FinishParallelTasks -- the setting
	// the runtime runs by default and plan §10.3.1 item 5b calibrates -- or, in an automation
	// build, at a test's override (item 5a.2's R). At 0 or 1 nothing is installed, exactly as the
	// runtime does. OutFinishParallelTasks is the value the run used; the values line logs it.
	bool InstallFinishHook(sslm_workspace Workspace, int32& OutFinishParallelTasks, FString& OutError)
	{
		const int32 FinishParallelTasks = EffectiveFinishParallelTasks();
		OutFinishParallelTasks = FinishParallelTasks;
		if (!SuperSLMFinishHook::IsValidTaskCount(FinishParallelTasks))
		{
			OutError = FString::Printf(TEXT("FinishParallelTasks %d is outside [0, %d]"), FinishParallelTasks, SuperSLMFinishHook::MaxFinishParallelTasks);
			return false;
		}
		if (!SuperSLMFinishHook::ShouldInstall(FinishParallelTasks))
		{
			return true;
		}
		const sslm_parallel_for Hook = SuperSLMFinishHook::Make(FinishParallelTasks);
		const sslm_status Status = sslm_workspace_set_parallel_for(Workspace, &Hook);
		if (Status != SSLM_OK)
		{
			OutError = FString::Printf(TEXT("sslm_workspace_set_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"),
				FinishParallelTasks, ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
			return false;
		}
		return true;
	}


	// D-SLM7444/D-SLM7450: a fixed process-name list is kept as a DIAGNOSTIC only, never
	// required evidence of idleness on its own (§10.2 item 10's own correction).
	void LogNamedProcessDiagnostic()
	{
		static const TCHAR* Named[] = { TEXT("UnrealEditor.exe"), TEXT("UnrealEditor-Cmd.exe"), TEXT("cl.exe"), TEXT("link.exe"), TEXT("dotnet.exe"), TEXT("UnrealBuildTool.exe") };
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: named-process diagnostic list (not required evidence on its own): %s, %s, %s, %s, %s, %s -- check these manually if the CPU-utilisation gate below fails."),
			Named[0], Named[1], Named[2], Named[3], Named[4], Named[5]);
	}

	// One depth arm's reading (plan §10.3.1 item 5a.1, D-SLM7793/D-SLM7794).
	struct FDepthArmReading
	{
		int64 Depth = 0;                  // the context_length the arm timed at
		int64 RestoredContextLength = -1; // what the restored blob's context_length read (asserted == Depth)
		double MedianMs = -1.0;
		int32 Samples = 0;
	};

	// The eleven values (plan §5 item 2), plus the per-depth readings they were fitted from.
	struct FMeasuredCosts
	{
		// Fitted from the five depth arms: the intercepts at depth 0 and the per-position slopes.
		double LayerCostMs = -1.0;
		double LayerCostPerPositionMs = -1.0;
		double PromptTokenCostMs = -1.0;
		double PromptTokenCostPerPositionMs = -1.0;
		// Depth-independent (D-SLM7793: the finish, and the lifecycle and prefix operations, which
		// copy or draw a whole block).
		double FinishCostMs = -1.0;
		double ResetCostMs = -1.0;
		double AdoptCostMs = -1.0;
		double SaveCostMs = -1.0;
		double RestoreCostMs = -1.0;
		double PrefixBeginCostMs = -1.0;
		double PrefixReleaseCostMs = -1.0;

		int64 CapTokens = 0;
		// The FinishParallelTasks the run installed (InstallFinishHook), logged on the values line
		// so item 5a.2's analysis can confirm R ran at 1. -1 until Setup installs the hook.
		int32 FinishParallelTasks = -1;
		TArray<FDepthArmReading> LayerArm;  // one per depth, in depth order
		TArray<FDepthArmReading> PromptArm; // one per depth, in depth order
	};

	// Samples per arm (D-SLM7444's 50-repetition pattern). Each depth of the layer arm records the
	// pure layers of every repetition, so it holds kReps x (num_hidden_layers - 2) samples.
	constexpr int32 kReps = 50;

	// The smallest cap the depth rule accepts: 1, C/4, C/2, 3C/4 and C - 1 are distinct from 8 up.
	constexpr int64 kMinCapTokens = 8;

	// The chunk the untimed setup prefill uses to reach each depth (a ceiling; each call's consumed
	// count is checked).
	constexpr int32 kSetupChunk = 64;

	// T-2999 S1: a cost is accepted only when it is a finite, positive number of milliseconds.
	bool IsValidCostMs(double Ms)
	{
		return FMath::IsFinite(Ms) && Ms > 0.0;
	}

	FString StatusName(sslm_status Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
	}

	// Layer 1's 'SSB5' sequence blob (src/sslm_abi.cpp, sslm_seq_save's write order at v1.9.0):
	// magic 4, model hash 32, kv_precision 4, schema_name_hash 8, dfa_walk_state 4,
	// adapter_binding_id 8, then context_length as a little-endian int64 at byte 60. 'SSB5' appends
	// its four per-site saturation counts at bytes 124-155, so this offset is 'SSB4''s. Read here only
	// to assert the depth a blob carries; no other field is interpreted. Only 'SSB5' is accepted:
	// both callers read a blob this process's own sslm_seq_save has just written, which at the pin
	// is always 'SSB5', so any other magic means the compiled Layer 1 is not the pinned one.
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

	// The shipped line for one depth arm (plan §5 item 2, D-SLM7793/D-SLM7794, T-3002 W1): the upper
	// envelope of its medians. Slope = (median at the last depth - median at the first) / (the depth
	// span), floored at 0. The intercept starts from the median at the first depth (depth 1), and is
	// raised by the largest amount any depth's median sits above that line, so no measured median
	// lies above the shipped line.
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

	// Drives the same C ABI calls the plugin itself makes (sslm_prefill, greedy sslm_decode_step_v2,
	// sslm_seq_reset, sslm_seq_adopt_prefix, sslm_seq_save, sslm_seq_restore, sslm_prefix_begin/
	// _release) against the project's own imported artifact, through the plugin's own runtime mapping
	// (SuperSLMRuntime::AcquireModelMapping).
	//
	// Setup() runs BEFORE the idleness pre-window, as untimed setup (plan §10.3.1 item 5a.1, T-3002
	// W2): it maps the model, reads the cap C, takes the depths 1, C/4, C/2, 3C/4 and C - 1 (a cap
	// under 8 is refused by name), prefills one sequence to C - 1, and saves a blob at each depth on
	// the way, asserting each blob's context_length. Measure() is the graded workload: only timed arms.
	//
	// Every timed call's ABI status and output state are checked before its duration is kept, and
	// every arm must reach its full sample population; any failure is a named measurement failure
	// (T-2999 S1). What each sample times, read at Layer 1 (src/sslm_abi.cpp):
	//  - Prompt arm, per depth d and repetition: restore d's blob (context_length d, ready_for_logits
	//    set by its prefill), assert context_length == d, then time sslm_prefill of one token at
	//    chunk budget 1, requiring consumed == 1: one prompt token at position d.
	//  - Layer arm, per depth d and repetition: restore d's blob, assert context_length == d, run the
	//    finish untimed (the ready_for_logits shortcut, which must produce a token), then the embed +
	//    layer 0 call untimed (must return -1), then time each one-layer call at layer_index
	//    1 .. num_hidden_layers - 2 (each must return -1; none embeds or finishes), and run the last
	//    layer + finish untimed (must produce a token, which confirms the layer count). The produced
	//    token sits at position d, so every timed layer attends over position d.
	//  - Finish: reset, prefill one token untimed, then time the next decode, which with
	//    ready_for_logits set runs only the token finish; kept only when it produced a token.
	//  - Reset / save / restore / adopt / prefix begin / prefix release as T-2999 built them.
	// No cost is derived by subtracting one distribution from another.
	class FCalibrationSession
	{
	public:
		~FCalibrationSession() { Cleanup(); }

		bool Setup(const FString& ArtifactPath, FMeasuredCosts& OutCosts, FString& OutError)
		{
			FSuperSLMImportDiagnostic Diag;
			Model = FSuperSLMModelImport::ImportFromFile(ArtifactPath, Diag);
			if (Model == nullptr || !Diag.bAccepted)
			{
				OutError = FString::Printf(TEXT("could not import '%s' (%s)"), *ArtifactPath, *Diag.Message);
				return false;
			}
			SuperSLMRuntime::FModelShape Shape;
			FString ShapeError;
			if (!SuperSLMRuntime::ReadModelShape(*Model, Shape, ShapeError))
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
			const int64 Cap = Shape.ContextCap;
			OutCosts.CapTokens = Cap;
			if (Cap < kMinCapTokens)
			{
				OutError = FString::Printf(TEXT("the artifact's cap C = %lld is under %lld: the depths 1, C/4, C/2, 3C/4 and C - 1 need a cap of at least %lld"), Cap, kMinCapTokens, kMinCapTokens);
				return false;
			}
			Depths = { 1, Cap / 4, Cap / 2, (3 * Cap) / 4, Cap - 1 };
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: cap C = %lld; depths %lld, %lld, %lld, %lld, %lld."),
				Cap, Depths[0], Depths[1], Depths[2], Depths[3], Depths[4]);

			FString MapError;
			Mapping = SuperSLMRuntime::AcquireModelMapping(*Model, MapError);
			if (Mapping == nullptr)
			{
				OutError = FString::Printf(TEXT("could not map '%s': %s"), *ArtifactPath, *MapError);
				return false;
			}

			// One sequence per call and one layer per decode call, inside Layer 1's domain
			// (1 <= max_layer_budget <= num_hidden_layers). The chunk ceiling serves the untimed setup
			// prefill; every timed prefill uses chunk budget 1.
			sslm_config CallShape;
			CallShape.max_batch = 1;
			CallShape.max_chunk_budget = kSetupChunk;
			CallShape.max_layer_budget = 1;
			CallShape.reserved = 0;

			BlockBytes = sslm_kv_block_size(Mapping);
			const size_t OverheadBytes = sslm_kv_pool_overhead_size(Mapping, kBlockCount);
			const size_t WorkspaceBytes = sslm_workspace_size(Mapping, &CallShape);
			StateBytes = sslm_seq_state_size(Mapping);
			if (BlockBytes == 0 || WorkspaceBytes == 0 || StateBytes == 0 || OverheadBytes == 0 || OverheadBytes == SIZE_MAX)
			{
				OutError = FString::Printf(TEXT("Layer 1 could not size the calibration pool/workspace (block %llu, workspace %llu, state %llu bytes)"),
					static_cast<uint64>(BlockBytes), static_cast<uint64>(WorkspaceBytes), static_cast<uint64>(StateBytes));
				return false;
			}
			KvBufferBytes = BlockBytes * kBlockCount + OverheadBytes;
			KvBuffer = FMemory::Malloc(KvBufferBytes, SSLM_ABI_ALIGNMENT_BYTES);
			WorkspaceBuffer = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
			SaveBuffer = FMemory::Malloc(StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
			if (KvBuffer == nullptr || WorkspaceBuffer == nullptr || SaveBuffer == nullptr)
			{
				OutError = TEXT("could not allocate the calibration buffers");
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
			// Plan §2.5 row 2 (D-SLM7663): the identical finish hook the runtime installs, at the
			// shipped default FinishParallelTasks, so FinishCostMs measures the finish the runtime runs.
			FString HookError;
			if (!InstallFinishHook(Workspace, OutCosts.FinishParallelTasks, HookError))
			{
				OutError = HookError;
				return false;
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
			DepthBlobs.SetNum(Depths.Num());
			for (int32 K = 0; K < Depths.Num(); ++K)
			{
				while (Filled < Depths[K])
				{
					const int32 Count = static_cast<int32>(FMath::Min<int64>(kSetupChunk, Depths[K] - Filled));
					int32 Consumed = 0;
					Status = sslm_prefill(Mapping, Seq, Tokens.GetData(), Count, Count, SSLM_SPAN_PROMPT, Workspace, &Consumed);
					if (Status != SSLM_OK || Consumed <= 0 || Consumed > Count)
					{
						OutError = FString::Printf(TEXT("setup: sslm_prefill toward depth %lld returned %s and consumed %d of %d at depth %lld"),
							Depths[K], *StatusName(Status), Consumed, Count, Filled);
						return false;
					}
					Filled += Consumed;
				}
				size_t PayloadBytes = StateBytes;
				Status = sslm_seq_save(Seq, SaveBuffer, &PayloadBytes);
				int64 BlobDepth = -1;
				if (Status != SSLM_OK || PayloadBytes == 0 || PayloadBytes > StateBytes ||
					!ReadBlobContextLength(static_cast<const uint8*>(SaveBuffer), PayloadBytes, BlobDepth) || BlobDepth != Depths[K])
				{
					OutError = FString::Printf(TEXT("setup: the blob saved at depth %lld returned %s, %llu bytes, context_length %lld"),
						Depths[K], *StatusName(Status), static_cast<uint64>(PayloadBytes), BlobDepth);
					return false;
				}
				DepthBlobs[K].SetNumUninitialized(static_cast<int32>(PayloadBytes));
				FMemory::Memcpy(DepthBlobs[K].GetData(), SaveBuffer, PayloadBytes);
			}
			return true;
		}

		bool Measure(FMeasuredCosts& OutCosts, FString& OutError)
		{
			auto Fail = [&](const FString& Why)
			{
				OutError = Why;
				return false;
			};
			auto Decode = [&](int32& OutToken) -> sslm_status
			{
				OutToken = -9;
				return sslm_decode_step_v2(Mapping, &Seq, 1, &Params, Workspace, &OutToken);
			};
			sslm_status Status = SSLM_OK;

			// --- Finish (depth-independent) ---
			TArray<double> FinishSamples;
			for (int32 Rep = 0; Rep < kReps; ++Rep)
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
				const double FinishMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK || Token < 0)
				{
					return Fail(FString::Printf(TEXT("finish arm, rep %d: the finish-only decode returned %s and token %d"), Rep, *StatusName(Status), Token));
				}
				FinishSamples.Add(FinishMs);
			}

			// --- The depth arms: one restore per arm per repetition (T-3002 W3, N3) ---
			OutCosts.PromptArm.SetNum(Depths.Num());
			OutCosts.LayerArm.SetNum(Depths.Num());
			for (int32 K = 0; K < Depths.Num(); ++K)
			{
				const int64 Depth = Depths[K];
				TArray<double> PromptSamples, LayerSamples;
				int64 PromptRestored = -1, LayerRestored = -1;
				for (int32 Rep = 0; Rep < kReps; ++Rep)
				{
					if (!RestoreAtDepth(K, PromptRestored, OutError))
					{
						OutError = FString::Printf(TEXT("prompt arm, depth %lld, rep %d: %s"), Depth, Rep, *OutError);
						return false;
					}
					int32 Consumed = 0;
					const double Start = FPlatformTime::Seconds();
					Status = sslm_prefill(Mapping, Seq, &PromptToken, 1, 1, SSLM_SPAN_PROMPT, Workspace, &Consumed);
					const double PromptMs = (FPlatformTime::Seconds() - Start) * 1000.0;
					if (Status != SSLM_OK || Consumed != 1)
					{
						return Fail(FString::Printf(TEXT("prompt arm, depth %lld, rep %d: sslm_prefill returned %s and consumed %d of 1"), Depth, Rep, *StatusName(Status), Consumed));
					}
					PromptSamples.Add(PromptMs);
				}
				for (int32 Rep = 0; Rep < kReps; ++Rep)
				{
					if (!RestoreAtDepth(K, LayerRestored, OutError))
					{
						OutError = FString::Printf(TEXT("layer arm, depth %lld, rep %d: %s"), Depth, Rep, *OutError);
						return false;
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
						const double LayerMs = (FPlatformTime::Seconds() - Start) * 1000.0;
						if (Status != SSLM_OK || Token != -1)
						{
							return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d, layer %d: the one-layer call returned %s and %d (expected -1)"), Depth, Rep, LayerIndex, *StatusName(Status), Token));
						}
						LayerSamples.Add(LayerMs);
					}
					Status = Decode(Token); // the last layer and its finish: not timed
					if (Status != SSLM_OK || Token < 0)
					{
						return Fail(FString::Printf(TEXT("layer arm, depth %lld, rep %d: the last layer's call returned %s and %d, so the arm's layer count (%d) does not match the model"),
							Depth, Rep, *StatusName(Status), Token, NumLayers));
					}
				}
				if (PromptSamples.Num() != kReps || LayerSamples.Num() != kReps * (NumLayers - 2))
				{
					return Fail(FString::Printf(TEXT("depth %lld: %d of %d prompt samples and %d of %d layer samples"),
						Depth, PromptSamples.Num(), kReps, LayerSamples.Num(), kReps * (NumLayers - 2)));
				}
				FDepthArmReading& P = OutCosts.PromptArm[K];
				P.Depth = Depth;
				P.RestoredContextLength = PromptRestored;
				P.Samples = PromptSamples.Num();
				P.MedianMs = Median(PromptSamples);
				FDepthArmReading& L = OutCosts.LayerArm[K];
				L.Depth = Depth;
				L.RestoredContextLength = LayerRestored;
				L.Samples = LayerSamples.Num();
				L.MedianMs = Median(LayerSamples);
				if (!IsValidCostMs(P.MedianMs) || !IsValidCostMs(L.MedianMs))
				{
					return Fail(FString::Printf(TEXT("depth %lld: a median is not a finite, positive cost (prompt %.9f ms, layer %.9f ms)"), Depth, P.MedianMs, L.MedianMs));
				}
			}

			// --- Reset / Save / Restore (D-SLM7444 pattern) ---
			TArray<double> ResetSamples, SaveSamples, RestoreSamples;
			for (int32 Rep = 0; Rep < kReps; ++Rep)
			{
				double Start = FPlatformTime::Seconds();
				Status = sslm_seq_reset(Seq);
				const double ResetMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("reset arm, rep %d: sslm_seq_reset failed (%s)"), Rep, *StatusName(Status)));
				}
				ResetSamples.Add(ResetMs);

				size_t PayloadBytes = StateBytes;
				Start = FPlatformTime::Seconds();
				Status = sslm_seq_save(Seq, SaveBuffer, &PayloadBytes);
				const double SaveMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK || PayloadBytes == 0 || PayloadBytes > StateBytes)
				{
					return Fail(FString::Printf(TEXT("save arm, rep %d: sslm_seq_save returned %s with %llu payload bytes"), Rep, *StatusName(Status), static_cast<uint64>(PayloadBytes)));
				}
				SaveSamples.Add(SaveMs);

				sslm_seq Restored = nullptr;
				Start = FPlatformTime::Seconds();
				Status = sslm_seq_restore(Mapping, &Pool, SaveBuffer, PayloadBytes, &Restored);
				const double RestoreMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK || Restored == nullptr)
				{
					return Fail(FString::Printf(TEXT("restore arm, rep %d: sslm_seq_restore failed (%s)"), Rep, *StatusName(Status)));
				}
				RestoreSamples.Add(RestoreMs);
				Status = sslm_seq_release(Seq);
				Seq = Restored;
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("restore arm, rep %d: releasing the replaced sequence failed (%s)"), Rep, *StatusName(Status)));
				}
			}

			// --- Adopt: a one-token frozen prefix, adopted by a freshly reset sequence each rep ---
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
			TArray<double> AdoptSamples;
			for (int32 Rep = 0; Rep < kReps; ++Rep)
			{
				Status = sslm_seq_reset(Seq);
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm, rep %d: sslm_seq_reset failed (%s)"), Rep, *StatusName(Status)));
				}
				const double Start = FPlatformTime::Seconds();
				Status = sslm_seq_adopt_prefix(Seq, Prefix);
				const double AdoptMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("adopt arm, rep %d: sslm_seq_adopt_prefix failed (%s)"), Rep, *StatusName(Status)));
				}
				AdoptSamples.Add(AdoptMs);
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

			// --- PrefixBegin / PrefixRelease: the two calls DispatchPrefixAdmin makes ---
			TArray<double> PrefixBeginSamples, PrefixReleaseSamples;
			for (int32 Rep = 0; Rep < kReps; ++Rep)
			{
				sslm_prefix RepPrefix = nullptr;
				double Start = FPlatformTime::Seconds();
				Status = sslm_prefix_begin(Mapping, &Pool, &RepPrefix);
				const double BeginMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK || RepPrefix == nullptr)
				{
					return Fail(FString::Printf(TEXT("prefix-begin arm, rep %d: sslm_prefix_begin failed (%s)"), Rep, *StatusName(Status)));
				}
				PrefixBeginSamples.Add(BeginMs);
				Start = FPlatformTime::Seconds();
				Status = sslm_prefix_release(RepPrefix);
				const double ReleaseMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Status != SSLM_OK)
				{
					return Fail(FString::Printf(TEXT("prefix-release arm, rep %d: sslm_prefix_release failed (%s)"), Rep, *StatusName(Status)));
				}
				PrefixReleaseSamples.Add(ReleaseMs);
			}

			// T-2999 S1: every depth-independent arm's full population, and every median finite and
			// positive.
			struct FArm { const TCHAR* Name; TArray<double>* Samples; double* Out; };
			FArm Arms[] = {
				{ TEXT("FinishCostMs"), &FinishSamples, &OutCosts.FinishCostMs },
				{ TEXT("ResetCostMs"), &ResetSamples, &OutCosts.ResetCostMs },
				{ TEXT("AdoptCostMs"), &AdoptSamples, &OutCosts.AdoptCostMs },
				{ TEXT("SaveCostMs"), &SaveSamples, &OutCosts.SaveCostMs },
				{ TEXT("RestoreCostMs"), &RestoreSamples, &OutCosts.RestoreCostMs },
				{ TEXT("PrefixBeginCostMs"), &PrefixBeginSamples, &OutCosts.PrefixBeginCostMs },
				{ TEXT("PrefixReleaseCostMs"), &PrefixReleaseSamples, &OutCosts.PrefixReleaseCostMs },
			};
			for (FArm& Arm : Arms)
			{
				if (Arm.Samples->Num() != kReps)
				{
					return Fail(FString::Printf(TEXT("%s has %d of its %d required samples"), Arm.Name, Arm.Samples->Num(), kReps));
				}
				*Arm.Out = Median(*Arm.Samples);
				if (!IsValidCostMs(*Arm.Out))
				{
					return Fail(FString::Printf(TEXT("%s's median %.9f ms is not a finite, positive cost"), Arm.Name, *Arm.Out));
				}
			}
			// The two depth lines (D-SLM7793/D-SLM7794): intercept and slope, the upper envelope.
			FitUpperEnvelope(OutCosts.LayerArm, OutCosts.LayerCostMs, OutCosts.LayerCostPerPositionMs);
			FitUpperEnvelope(OutCosts.PromptArm, OutCosts.PromptTokenCostMs, OutCosts.PromptTokenCostPerPositionMs);
			return true;
		}

	private:
		// Restores depth K's blob into a fresh sequence, releases the one it replaces, and asserts the
		// restored context_length by saving it back and reading the blob (Layer 1 exposes no getter).
		bool RestoreAtDepth(int32 K, int64& OutRestoredContextLength, FString& OutError)
		{
			const TArray<uint8>& Blob = DepthBlobs[K];
			sslm_seq Restored = nullptr;
			sslm_status Status = sslm_seq_restore(Mapping, &Pool, Blob.GetData(), static_cast<size_t>(Blob.Num()), &Restored);
			if (Status != SSLM_OK || Restored == nullptr)
			{
				OutError = FString::Printf(TEXT("sslm_seq_restore of the depth-%lld blob failed (%s)"), Depths[K], *StatusName(Status));
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
			if (Status != SSLM_OK || !ReadBlobContextLength(static_cast<const uint8*>(SaveBuffer), PayloadBytes, ContextLength) || ContextLength != Depths[K])
			{
				OutError = FString::Printf(TEXT("the restored sequence's context_length reads %lld (save %s), not the depth %lld"),
					ContextLength, *StatusName(Status), Depths[K]);
				return false;
			}
			OutRestoredContextLength = ContextLength;
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
		USuperSLMModel* Model = nullptr;
		sslm_model Mapping = nullptr;
		int32 NumLayers = 0;
		TArray<int64> Depths;
		TArray<TArray<uint8>> DepthBlobs;
		size_t BlockBytes = 0;
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

	// ---------------------------------------------------------------------------------------------
	// Plan §10.3.1 item 5b (PD-1 ruled (A), D-SLM7777; the criterion stated by D-SLM7778): the
	// idleness evidence. One dedicated sampler thread takes a sample every 250 ms across a 2 s
	// pre-window, the whole workload and a 2 s post-window. Each sample reads GetSystemTimes for the
	// box and GetProcessTimes for this process; what is graded is the FOREIGN share (box busy time
	// less this process's CPU time, over the box's capacity, clamped at 0), so the calibration's own
	// game-thread and finish-hook worker load never makes the box read contended. The ceiling is
	// 50/N percent of capacity (N = logical processors, read at run time), graded on the mean of every
	// four consecutive samples (a sliding 1 s window) anywhere in the series. The build lock is
	// classified at every sample (absent, own or foreign; only foreign refuses, D-SLM7788). The verdict, and any write, come only after the post-window's last
	// sample. Residual stated by item 5b and not closed: load inside this process that is not the
	// calibration (editor ticking, asset or shader-map work) is subtracted with it; each sample's
	// own-process share is logged so the record shows how much there was.
	// ---------------------------------------------------------------------------------------------

	constexpr double kSampleIntervalSeconds = 0.25;
	constexpr double kPreWindowSeconds = 2.0;
	constexpr double kPostWindowSeconds = 2.0;
	constexpr int32 kWindowSamples = 4;
	constexpr double kCeilingLogicalProcessorFraction = 0.5; // half of one logical processor

	const TCHAR* PhaseName(ESuperSLMCalibrationPhase Phase)
	{
		switch (Phase)
		{
			case ESuperSLMCalibrationPhase::PreWindow: return TEXT("pre-window");
			case ESuperSLMCalibrationPhase::Workload: return TEXT("workload");
			case ESuperSLMCalibrationPhase::PostWindow: return TEXT("post-window");
		}
		return TEXT("?");
	}

	// The most recent run's report (game thread). Production reads it only to log it.
	FSuperSLMCalibrationIdlenessReport GLastIdlenessReport;

#if WITH_DEV_AUTOMATION_TESTS
	// Test-only overrides (FSuperSLMCalibrationTestAccess). Absent from a build without automation
	// tests; empty otherwise unless a test sets them.
	TFunction<void(ESuperSLMCalibrationEvent)> GTestPhaseHook;
	TFunction<bool(FString&)> GTestSyntheticWorkload;
	// Plan §10.3.1 item 5a.2's condition S (D-SLM7793): the run with no sampler thread.
	bool GTestSamplerDisabled = false;
#endif

	void FirePhaseHook(ESuperSLMCalibrationEvent Event)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GTestPhaseHook)
		{
			GTestPhaseHook(Event);
		}
#else
		(void)Event;
#endif
	}

	// Plan §10.3.1 item 5b's build-lock check (D-SLM7444, owner marker D-SLM7788), classified at every
	// sample on the sampler thread. The contract with a build wrapper: the wrapper holds the build
	// lock (BuildLockPath()) as a DIRECTORY, writes a fresh GUID as the first line of owner.txt
	// inside it, and passes the same GUID to this process as SSU_BUILD_LOCK_TOKEN. Absent: nothing
	// at the path, file or directory. Own: a directory, a non-empty token, and owner.txt's first
	// line equal to it. Foreign: anything else. Only Foreign refuses. (An earlier build used
	// IFileManager::FileExists, false for a directory, so it read "absent" while the lock was held.)
	// The lock's path comes from SUPERSLM_BUILD_LOCK, which a machine whose builds take such a lock
	// sets. Unset,
	// the build-lock check is not configured: every sample reads NotConfigured, the evidence says
	// "build-lock check not configured", and the lock never refuses a run.
	FString BuildLockPath()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_BUILD_LOCK")).TrimStartAndEnd();
	}

	// RefusedForeignLock's verdict text, set at the start of each run from the configured path. With
	// SUPERSLM_BUILD_LOCK set to the box's lock, it reads what it did when the path was fixed. The
	// command runs on the game thread only.
	FString GRefusedForeignLockText = TEXT("REFUSED: the build lock was held by a foreign owner");

	const TCHAR* LockStateName(ESuperSLMBuildLockState State)
	{
		switch (State)
		{
			case ESuperSLMBuildLockState::Absent: return TEXT("absent");
			case ESuperSLMBuildLockState::Own: return TEXT("own");
			case ESuperSLMBuildLockState::Foreign: return TEXT("foreign");
			case ESuperSLMBuildLockState::NotConfigured: return TEXT("check not configured");
		}
		return TEXT("foreign");
	}

	ESuperSLMBuildLockState ClassifyBuildLock(const FString& LockPath, const FString& Token, FString& OutForeignReason)
	{
		OutForeignReason.Reset();
		if (LockPath.IsEmpty())
		{
			return ESuperSLMBuildLockState::NotConfigured;
		}
		const FString OwnerFile = FPaths::Combine(LockPath, TEXT("owner.txt"));
		IFileManager& Files = IFileManager::Get();
		const bool bDirectory = Files.DirectoryExists(*LockPath);
		if (!bDirectory)
		{
			if (!Files.FileExists(*LockPath))
			{
				return ESuperSLMBuildLockState::Absent;
			}
			OutForeignReason = TEXT("a file, not a directory, is at the lock path");
			return ESuperSLMBuildLockState::Foreign;
		}
		if (Token.IsEmpty())
		{
			OutForeignReason = TEXT("this process has no SSU_BUILD_LOCK_TOKEN");
			return ESuperSLMBuildLockState::Foreign;
		}
		if (!Files.FileExists(*OwnerFile))
		{
			OutForeignReason = TEXT("the lock has no owner.txt");
			return ESuperSLMBuildLockState::Foreign;
		}
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *OwnerFile))
		{
			OutForeignReason = TEXT("owner.txt could not be read");
			return ESuperSLMBuildLockState::Foreign;
		}
		FString FirstLine;
		if (!Contents.Split(TEXT("\n"), &FirstLine, nullptr))
		{
			FirstLine = Contents;
		}
		FirstLine.TrimStartAndEndInline(); // drops the CR of a CRLF line and any padding
		if (FirstLine.IsEmpty())
		{
			OutForeignReason = TEXT("owner.txt's first line is empty");
			return ESuperSLMBuildLockState::Foreign;
		}
		if (!FirstLine.Equals(Token, ESearchCase::IgnoreCase))
		{
			OutForeignReason = TEXT("owner.txt names a different token");
			return ESuperSLMBuildLockState::Foreign;
		}
		return ESuperSLMBuildLockState::Own;
	}

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
		uint64 Process = 0; // this process's kernel + user
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

	// Per-process CPU time of every other process, for the refusal diagnostic only (the process
	// list never grades anything, D-SLM7450). Processes this user cannot open are skipped.
	struct FProcessSnapshot
	{
		TMap<uint32, uint64> CpuTime; // pid -> kernel + user, 100 ns
		// Why a process could not be read (D-SLM7788: a failed enumeration is logged with its error,
		// never left as a silent empty list). 0 when the snapshot itself succeeded.
		uint32 SnapshotError = 0;         // GetLastError() of CreateToolhelp32Snapshot/Process32FirstW
		int32 Enumerated = 0;             // processes listed, this one and pid 0 excluded
		TArray<FString> Unreadable;       // "name (pid N): OpenProcess|GetProcessTimes error E"
	};

	FProcessSnapshot SnapshotProcesses(TMap<uint32, FString>& InOutNames)
	{
		FProcessSnapshot Snap;
		const DWORD OwnPid = ::GetCurrentProcessId();
		HANDLE Tool = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (Tool == INVALID_HANDLE_VALUE)
		{
			Snap.SnapshotError = ::GetLastError();
			return Snap;
		}
		PROCESSENTRY32W Entry;
		Entry.dwSize = sizeof(Entry);
		if (::Process32FirstW(Tool, &Entry) == 0)
		{
			Snap.SnapshotError = ::GetLastError();
			::CloseHandle(Tool);
			return Snap;
		}
		do
		{
			if (Entry.th32ProcessID == OwnPid || Entry.th32ProcessID == 0)
			{
				continue;
			}
			++Snap.Enumerated;
			HANDLE Proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, Entry.th32ProcessID);
			if (Proc == nullptr)
			{
				Snap.Unreadable.Add(FString::Printf(TEXT("%s (pid %u): OpenProcess error %u"), Entry.szExeFile, Entry.th32ProcessID, ::GetLastError()));
				continue;
			}
			FILETIME Creation, Exit, Kernel, User;
			if (::GetProcessTimes(Proc, &Creation, &Exit, &Kernel, &User) != 0)
			{
				Snap.CpuTime.Add(Entry.th32ProcessID, FileTimeToU64(Kernel) + FileTimeToU64(User));
				if (!InOutNames.Contains(Entry.th32ProcessID))
				{
					InOutNames.Add(Entry.th32ProcessID, FString(Entry.szExeFile));
				}
			}
			else
			{
				Snap.Unreadable.Add(FString::Printf(TEXT("%s (pid %u): GetProcessTimes error %u"), Entry.szExeFile, Entry.th32ProcessID, ::GetLastError()));
			}
			::CloseHandle(Proc);
		}
		while (::Process32NextW(Tool, &Entry) != 0);
		::CloseHandle(Tool);
		return Snap;
	}
#endif

	// The sampler thread. Every kSampleIntervalSeconds it closes one sample, tagged with the phase
	// current at the sample's end, and keeps a per-process snapshot for the refusal diagnostic. On
	// Stop() it closes one final (shorter) sample, so the post-window runs to its end.
	class FIdlenessSampler : public FRunnable
	{
	public:
		FIdlenessSampler(const std::atomic<uint8>& InPhase, double InOrigin, const FString& InLockPath, const FString& InLockToken)
			: Phase(InPhase), Origin(InOrigin), LockPath(InLockPath), LockToken(InLockToken) {}

		bool Start()
		{
#if PLATFORM_WINDOWS
			StopEvent = FPlatformProcess::GetSynchEventFromPool(true);
			Thread = FRunnableThread::Create(this, TEXT("SuperSLM Calibration Idleness Sampler"), 0, TPri_AboveNormal);
			return Thread != nullptr;
#else
			return false;
#endif
		}

		// Stops the thread after its final sample and joins it. The series is complete on return.
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
			InitialSnapshot = SnapshotProcesses(Names);
			bool bStopping = false;
			while (!bStopping)
			{
				bStopping = StopEvent->Wait(static_cast<uint32>(kSampleIntervalSeconds * 1000.0));
				const FTimesReading Now = ReadTimes();
				if (!Now.bOk)
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
				FSuperSLMIdlenessSample Sample;
				Sample.EndSeconds = FPlatformTime::Seconds() - Origin;
				Sample.Phase = static_cast<ESuperSLMCalibrationPhase>(Phase.load(std::memory_order_acquire));
				if (Capacity > 0)
				{
					Sample.BoxSharePercent = 100.0 * static_cast<double>(Busy) / static_cast<double>(Capacity);
					Sample.OwnSharePercent = 100.0 * static_cast<double>(Own) / static_cast<double>(Capacity);
					Sample.ForeignSharePercent = Busy > Own ? 100.0 * static_cast<double>(Busy - Own) / static_cast<double>(Capacity) : 0.0;
				}
				Sample.LockState = ClassifyBuildLock(LockPath, LockToken, Sample.LockForeignReason);
				Samples.Add(Sample);
				Snapshots.Add(SnapshotProcesses(Names));
				Prev = Now;
			}
#endif
			return 0;
		}

		// Written only by the sampler thread; read only after StopAndJoin().
		TArray<FSuperSLMIdlenessSample> Samples;
		bool bFailed = false;
#if PLATFORM_WINDOWS
		FProcessSnapshot InitialSnapshot;
		TArray<FProcessSnapshot> Snapshots; // Snapshots[i] taken at Samples[i]'s end
		TMap<uint32, FString> Names;
#endif

	private:
		const std::atomic<uint8>& Phase;
		double Origin = 0.0; // the command's own series origin, so sample times match its phase log lines
		FString LockPath;    // SUPERSLM_BUILD_LOCK as read when the run began (empty: the check is not configured)
		FString LockToken;   // SSU_BUILD_LOCK_TOKEN as this process inherited it (empty when absent)
		FRunnableThread* Thread = nullptr;
		FEvent* StopEvent = nullptr;
	};

	// Grades the series by item 5b's criterion: the build lock first (any sample), then every
	// sliding window of kWindowSamples consecutive samples against the ceiling.
	void GradeSeries(FSuperSLMCalibrationIdlenessReport& Report)
	{
		for (int32 I = 0; I < Report.Samples.Num(); ++I)
		{
			if (Report.Samples[I].LockState == ESuperSLMBuildLockState::Foreign)
			{
				Report.FirstForeignLockSample = I;
				break;
			}
		}
		for (int32 I = 0; I + kWindowSamples <= Report.Samples.Num(); ++I)
		{
			double Sum = 0.0;
			for (int32 J = 0; J < kWindowSamples; ++J)
			{
				Sum += Report.Samples[I + J].ForeignSharePercent;
			}
			const double Mean = Sum / kWindowSamples;
			if (Mean > Report.CeilingPercent)
			{
				Report.FirstRefusingWindow = I;
				Report.FirstRefusingWindowFirstPhase = Report.Samples[I].Phase;
				Report.FirstRefusingWindowLastPhase = Report.Samples[I + kWindowSamples - 1].Phase;
				Report.FirstRefusingWindowMeanPercent = Mean;
				break;
			}
		}
		if (Report.FirstForeignLockSample >= 0)
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::RefusedForeignLock;
		}
		else if (Report.FirstRefusingWindow >= 0)
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::RefusedForeignLoad;
		}
		else if (Report.Samples.Num() < kWindowSamples)
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::SamplerUnavailable; // too short a series to grade
		}
		else
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::Met;
		}
	}

	// On a refusal: the foreign processes whose CPU time grew most over the refusing window
	// (diagnostic only; the record names the load instead of guessing it).
	void LogTopForeignGrowth(const FIdlenessSampler& Sampler, const FSuperSLMCalibrationIdlenessReport& Report)
	{
#if PLATFORM_WINDOWS
		if (Report.FirstRefusingWindow < 0)
		{
			return;
		}
		const int32 First = Report.FirstRefusingWindow;
		if (Sampler.Snapshots.Num() < First + kWindowSamples)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: foreign processes: no process snapshot covers the refusing window (%d snapshots for samples %d-%d)."),
				Sampler.Snapshots.Num(), First, First + kWindowSamples - 1);
			return;
		}
		const FProcessSnapshot& Before = First == 0 ? Sampler.InitialSnapshot : Sampler.Snapshots[First - 1];
		const FProcessSnapshot& After = Sampler.Snapshots[First + kWindowSamples - 1];
		// D-SLM7788: a failed enumeration is logged with its Win32 error, never left as a bare heading.
		for (const FProcessSnapshot* Snap : { &Before, &After })
		{
			if (Snap->SnapshotError != 0)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: foreign processes: process enumeration failed at the refusing window's %s (CreateToolhelp32Snapshot/Process32FirstW error %u)."),
					Snap == &Before ? TEXT("start") : TEXT("end"), Snap->SnapshotError);
				return;
			}
		}
		const double WindowSeconds = Report.Samples[First + kWindowSamples - 1].EndSeconds - (First == 0 ? 0.0 : Report.Samples[First - 1].EndSeconds);
		TArray<TPair<uint64, uint32>> Growth;
		for (const TPair<uint32, uint64>& Pair : After.CpuTime)
		{
			const uint64* Prior = Before.CpuTime.Find(Pair.Key);
			const uint64 Grew = Prior != nullptr ? (Pair.Value > *Prior ? Pair.Value - *Prior : 0) : Pair.Value;
			if (Grew > 0)
			{
				Growth.Add(TPair<uint64, uint32>(Grew, Pair.Key));
			}
		}
		Growth.Sort([](const TPair<uint64, uint32>& A, const TPair<uint64, uint32>& B) { return A.Key > B.Key; });
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: foreign processes whose CPU time grew most over the refusing window (%.2f s; diagnostic only):"), WindowSeconds);
		for (int32 I = 0; I < FMath::Min(5, Growth.Num()); ++I)
		{
			const FString* Name = Sampler.Names.Find(Growth[I].Value);
			const double CpuSeconds = static_cast<double>(Growth[I].Key) / 1.0e7;
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts:   %s (pid %u): %.3f CPU-s, %.2f logical processors over the window"),
				Name != nullptr ? **Name : TEXT("?"), Growth[I].Value, CpuSeconds, WindowSeconds > 0.0 ? CpuSeconds / WindowSeconds : 0.0);
		}
		// The processes this user could not read at the window's end. When none that could be read
		// grew, the load is among these (a protected or elevated process, such as a VM's memory
		// process), so they are named with the error, never left out.
		if (Growth.Num() == 0)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts:   no readable process's CPU time grew over the window (%d enumerated, %d readable, %d unreadable); the load is in a process this user cannot read:"),
				After.Enumerated, After.CpuTime.Num(), After.Unreadable.Num());
			for (const FString& Line : After.Unreadable)
			{
				UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts:   unreadable: %s"), *Line);
			}
		}
		else if (After.Unreadable.Num() > 0)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts:   (%d of %d processes could not be read, so their growth is not in this list; the first: %s)"),
				After.Unreadable.Num(), After.Enumerated, *After.Unreadable[0]);
		}
#else
		(void)Sampler;
		(void)Report;
#endif
	}

	// N for the ceiling (D-SLM7793, T-3001 F4): every logical processor GetSystemTimes' capacity sums
	// over -- GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) -- never the UE count, which -corelimit
	// caps. Both are recorded; the UE count only for the log line when they differ.
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

	// Runs Workload inside the one sample series and fills Report. OnGraded runs after the series is
	// graded and before any verdict detail is logged, so the command logs its medians ahead of the
	// verdict (plan §10.3.1 item 5a.1). Returns only after the post-window's last sample.
	void RunUnderIdlenessEvidence(TFunctionRef<void()> Workload, FSuperSLMCalibrationIdlenessReport& Report,
		TFunctionRef<void(const FSuperSLMCalibrationIdlenessReport&)> OnGraded)
	{
		Report.LogicalProcessors = BoxLogicalProcessors();
		Report.UeLogicalProcessors = FPlatformMisc::NumberOfCoresIncludingHyperthreads();
		Report.CeilingPercent = 100.0 * kCeilingLogicalProcessorFraction / static_cast<double>(Report.LogicalProcessors);
		if (Report.UeLogicalProcessors != Report.LogicalProcessors)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: N = %d from GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); FPlatformMisc::NumberOfCoresIncludingHyperthreads() reads %d."),
				Report.LogicalProcessors, Report.UeLogicalProcessors);
		}
		const FString LockPath = BuildLockPath();
		Report.bLockCheckConfigured = !LockPath.IsEmpty();
		GRefusedForeignLockText = Report.bLockCheckConfigured
			? FString::Printf(TEXT("REFUSED: %s was held by a foreign owner"), *LockPath)
			: FString(TEXT("REFUSED: the build lock was held by a foreign owner"));
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: idleness criterion: foreign CPU share, mean of every %d consecutive %.0f ms samples, must not exceed %.4f%% (50/N, N = %d logical processors); %s."),
			kWindowSamples, kSampleIntervalSeconds * 1000.0, Report.CeilingPercent, Report.LogicalProcessors,
			Report.bLockCheckConfigured
				? *FString::Printf(TEXT("%s never held by a foreign owner at any sample"), *LockPath)
				: TEXT("build-lock check not configured (SUPERSLM_BUILD_LOCK is unset), so the lock does not refuse"));

		// D-SLM7788: the token the build wrapper passed down, read once; a hand-issued command has none,
		// and then every lock present reads foreign.
		const FString LockToken = FPlatformMisc::GetEnvironmentVariable(TEXT("SSU_BUILD_LOCK_TOKEN")).TrimStartAndEnd();
		Report.bHadLockToken = !LockToken.IsEmpty();
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: SSU_BUILD_LOCK_TOKEN %s."),
			Report.bHadLockToken ? *FString::Printf(TEXT("is %s"), *LockToken) : TEXT("is not set: any lock present reads foreign"));

		bool bSamplerDisabled = false;
#if WITH_DEV_AUTOMATION_TESTS
		// Plan §10.3.1 item 5a.2's condition S: the same run, phases and windows, with no sampler.
		bSamplerDisabled = GTestSamplerDisabled;
#endif
		Report.bSamplerDisabled = bSamplerDisabled;

		FirePhaseHook(ESuperSLMCalibrationEvent::BeforePreWindow);
		std::atomic<uint8> Phase{static_cast<uint8>(ESuperSLMCalibrationPhase::PreWindow)};
		const double Origin = FPlatformTime::Seconds();
		FIdlenessSampler Sampler(Phase, Origin, LockPath, LockToken);
		const bool bSampling = bSamplerDisabled ? false : Sampler.Start();
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: phase pre-window started at %.3f s%s."), 0.0,
			bSamplerDisabled ? TEXT(" (sampler disabled by the test seam)") : TEXT(""));
		FPlatformProcess::Sleep(static_cast<float>(kPreWindowSeconds));

		Phase.store(static_cast<uint8>(ESuperSLMCalibrationPhase::Workload), std::memory_order_release);
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: workload-start at %.3f s (phase workload)."), FPlatformTime::Seconds() - Origin);
		FirePhaseHook(ESuperSLMCalibrationEvent::WorkloadStart);
		Workload();
		Phase.store(static_cast<uint8>(ESuperSLMCalibrationPhase::PostWindow), std::memory_order_release);
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: workload-end at %.3f s (phase post-window)."), FPlatformTime::Seconds() - Origin);
		FirePhaseHook(ESuperSLMCalibrationEvent::WorkloadEnd);
		FPlatformProcess::Sleep(static_cast<float>(kPostWindowSeconds));
		Sampler.StopAndJoin();
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: post-window ended at %.3f s; the series is complete."), FPlatformTime::Seconds() - Origin);
		FirePhaseHook(ESuperSLMCalibrationEvent::AfterPostWindow);

		if (bSamplerDisabled)
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::NotGraded;
			OnGraded(Report);
			return;
		}
		Report.Samples = Sampler.Samples;
		for (int32 I = 0; I < Report.Samples.Num(); ++I)
		{
			const FSuperSLMIdlenessSample& S = Report.Samples[I];
			// The lock-state format the constructions assert on (build log §20): "build lock absent.",
			// "build lock own." or "build lock foreign (<reason>).".
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: sample %d at %.3f s, %s: box %.4f%%, own %.4f%%, foreign %.4f%%, build lock %s%s."),
				I, S.EndSeconds, PhaseName(S.Phase), S.BoxSharePercent, S.OwnSharePercent, S.ForeignSharePercent,
				LockStateName(S.LockState),
				S.LockState == ESuperSLMBuildLockState::Foreign ? *FString::Printf(TEXT(" (%s)"), *S.LockForeignReason) : TEXT(""));
		}
		if (!bSampling || Sampler.bFailed)
		{
			Report.Verdict = ESuperSLMIdlenessVerdict::SamplerUnavailable;
		}
		else
		{
			GradeSeries(Report);
		}
		OnGraded(Report);
		if (Report.Verdict == ESuperSLMIdlenessVerdict::SamplerUnavailable)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: idleness sampler unavailable on this platform or failed; the run is refused."));
			return;
		}
		if (Report.FirstForeignLockSample >= 0)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: %s held by a foreign owner at sample %d (%s): %s."),
				*LockPath, Report.FirstForeignLockSample, PhaseName(Report.Samples[Report.FirstForeignLockSample].Phase),
				*Report.Samples[Report.FirstForeignLockSample].LockForeignReason);
		}
		if (Report.FirstRefusingWindow >= 0)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: first refusing window: samples %d-%d (%s to %s), mean foreign share %.4f%% against the %.4f%% ceiling."),
				Report.FirstRefusingWindow, Report.FirstRefusingWindow + kWindowSamples - 1,
				PhaseName(Report.FirstRefusingWindowFirstPhase), PhaseName(Report.FirstRefusingWindowLastPhase),
				Report.FirstRefusingWindowMeanPercent, Report.CeilingPercent);
			LogTopForeignGrowth(Sampler, Report);
		}
	}

	const TCHAR* VerdictText(ESuperSLMIdlenessVerdict Verdict)
	{
		switch (Verdict)
		{
			case ESuperSLMIdlenessVerdict::Met: return TEXT("MEETS the criterion");
			case ESuperSLMIdlenessVerdict::RefusedForeignLock: return *GRefusedForeignLockText;
			case ESuperSLMIdlenessVerdict::RefusedForeignLoad: return TEXT("REFUSED: a window's foreign share exceeded the ceiling");
			case ESuperSLMIdlenessVerdict::SamplerUnavailable: return TEXT("REFUSED: the sampler could not grade the run");
			case ESuperSLMIdlenessVerdict::NotGraded: return TEXT("not graded: the sampler was disabled");
		}
		return TEXT("REFUSED");
	}

	// Plan §10.3.1 item 5a.1 (T-3003 F40): every completed measurement's per-depth medians and eleven
	// values, logged before the idleness verdict, labelled when the run is not decision-bearing.
	// Logging writes nothing to Saved/. The formats are in build log §22.
	void LogMeasuredValues(const FMeasuredCosts& Costs, const FString& Label)
	{
		for (const FDepthArmReading& R : Costs.LayerArm)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: layer arm timed at depth %lld (restored context_length %lld): median %.6f ms over %d samples%s."),
				R.Depth, R.RestoredContextLength, R.MedianMs, R.Samples, *Label);
		}
		for (const FDepthArmReading& R : Costs.PromptArm)
		{
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: prompt arm timed at depth %lld (restored context_length %lld): median %.6f ms over %d samples%s."),
				R.Depth, R.RestoredContextLength, R.MedianMs, R.Samples, *Label);
		}
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: values%s: LayerCostMs=%.6f LayerCostPerPositionMs=%.9f PromptTokenCostMs=%.6f PromptTokenCostPerPositionMs=%.9f FinishCostMs=%.6f ResetCostMs=%.6f AdoptCostMs=%.6f SaveCostMs=%.6f RestoreCostMs=%.6f PrefixBeginCostMs=%.6f PrefixReleaseCostMs=%.6f FinishParallelTasks=%d"),
			*Label, Costs.LayerCostMs, Costs.LayerCostPerPositionMs, Costs.PromptTokenCostMs, Costs.PromptTokenCostPerPositionMs,
			Costs.FinishCostMs, Costs.ResetCostMs, Costs.AdoptCostMs, Costs.SaveCostMs, Costs.RestoreCostMs,
			Costs.PrefixBeginCostMs, Costs.PrefixReleaseCostMs, Costs.FinishParallelTasks);
	}

	// Diagnostic only: the most recent run's outcome, read by SuperSLMCalibrateCostsCommand::
	// GetLastOutcome(). Game-thread (console commands run there).
	SuperSLMCalibrateCostsCommand::EOutcome GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::NotRun;

	void RunCalibrateCostsCommand(const TArray<FString>& Args)
	{
		GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::MeasurementFailed;
		GLastIdlenessReport = FSuperSLMCalibrationIdlenessReport();
		if (Args.Num() < 1)
		{
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts <ArtifactPath.sslm> -- measures the eleven static per-unit scheduling costs and writes them into Saved/SuperSLM/CalibratedCosts.ini, only when the machine is otherwise idle."));
			return;
		}
		const FString ArtifactPath = Args[0];

		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: starting against '%s'."), *ArtifactPath);
		LogNamedProcessDiagnostic();

		FMeasuredCosts Costs;
		FString Error;
		bool bRanOk = false;
		double RunSeconds = 0.0;
		bool bSynthetic = false;
#if WITH_DEV_AUTOMATION_TESTS
		// FSuperSLMCalibrationTestAccess::SetSyntheticWorkload(): a test's short substitute runs in
		// the measurement's place, graded by the same criterion, and never written.
		const TFunction<bool(FString&)> Synthetic = GTestSyntheticWorkload;
		bSynthetic = static_cast<bool>(Synthetic);
#endif
		FSuperSLMCalibrationIdlenessReport& Report = GLastIdlenessReport;
		Report.bSyntheticWorkload = bSynthetic;

		// Plan §10.3.1 item 5a.1 (T-3002 W2): the prefill to depth and the depth blobs are untimed
		// setup, run before the pre-window, so the graded series spans only the timed arms.
		FCalibrationSession Session;
		if (!bSynthetic)
		{
			const double SetupStart = FPlatformTime::Seconds();
			const bool bSetupOk = Session.Setup(ArtifactPath, Costs, Error);
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: setup (the prefill to each depth and its blob) took %.2f s, before the pre-window."),
				FPlatformTime::Seconds() - SetupStart);
			if (!bSetupOk)
			{
				UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: FAILED -- measurement setup failed (%s); nothing is written."), *Error);
				return;
			}

			// Plan §10.3.1 item 5a.1 (D-SLM7816): the warm-up pass. The whole timed workload, every
			// arm at every depth, runs once after the depth setup and before the pre-window, untimed
			// and discarded, so no run -- the first in a fresh process included -- ships cold-start
			// costs. The sampler starts only in RunUnderIdlenessEvidence below, after this returns, so
			// no timed sample is taken before the warm-up ends. N counts the pass's timed calls (the
			// samples a measurement takes), all discarded.
			FMeasuredCosts WarmUp;
			FString WarmUpError;
			if (!Session.Measure(WarmUp, WarmUpError))
			{
				UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: FAILED -- the warm-up pass failed (%s); nothing is written."), *WarmUpError);
				return;
			}
			int64 WarmUpCalls = static_cast<int64>(7) * kReps; // finish, reset, save, restore, adopt, prefix begin, prefix release
			for (const FDepthArmReading& R : WarmUp.PromptArm) { WarmUpCalls += R.Samples; }
			for (const FDepthArmReading& R : WarmUp.LayerArm) { WarmUpCalls += R.Samples; }
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: warm-up pass: %lld calls, discarded."), WarmUpCalls);
		}
		else
		{
			// A synthetic substitute has no arms, depths or setup to warm (D-SLM7816 defines the pass
			// as the timed workload after the depth setup), and running a construction's substitute
			// twice would plant its load or lock change outside the graded window.
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: warm-up pass skipped: synthetic workload (no measurement arms)."));
		}

		// Plan §10.3.1 item 5b (D-SLM7778): the workload runs inside the one sample series; the
		// verdict is graded only after the post-window's last sample.
		RunUnderIdlenessEvidence([&]()
		{
			const double RunStart = FPlatformTime::Seconds();
#if WITH_DEV_AUTOMATION_TESTS
			if (bSynthetic)
			{
				bRanOk = Synthetic(Error);
			}
			else
#endif
			{
				bRanOk = Session.Measure(Costs, Error);
			}
			RunSeconds = FPlatformTime::Seconds() - RunStart;
		}, Report, [&](const FSuperSLMCalibrationIdlenessReport& Graded)
		{
			// Every completed measurement's medians, before the verdict (T-3003 F40).
			if (!bSynthetic && bRanOk)
			{
				FString Label;
				if (Graded.bSamplerDisabled)
				{
					Label = TEXT(" [sampler disabled: no idleness verdict]");
				}
				else if (Graded.Verdict != ESuperSLMIdlenessVerdict::Met)
				{
					Label = FString::Printf(TEXT(" [not decision-bearing: %s]"), VerdictText(Graded.Verdict));
				}
				LogMeasuredValues(Costs, Label);
			}
		});
		Report.bWorkloadSucceeded = bRanOk;

		const bool bIdleEvidenceOk = Report.Verdict == ESuperSLMIdlenessVerdict::Met;
		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: idleness evidence -- %d samples, ceiling %.4f%% (N = %d) -- %s."),
			Report.Samples.Num(), Report.CeilingPercent, Report.LogicalProcessors, VerdictText(Report.Verdict));

		if (!bRanOk)
		{
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: FAILED -- measurement failed (%s); nothing is written."), *Error);
			return;
		}
		if (Report.bSamplerDisabled)
		{
			// Item 5a.2's condition S (test seam only): the values are logged, never written.
			GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::NotRun;
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: sampler disabled: no idleness verdict; nothing is written."));
			return;
		}

		const FString OutPath = FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini");
		// Plan §10.3.1 items 5b and 6b: medians from a run that does not meet the criterion are never
		// written -- the file's defaults are what every job's K is sized from, so a contended reading
		// must not reach it. The refusal is by name, and any existing file is left untouched.
		if (!bIdleEvidenceOk)
		{
			GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::RefusedNotIdle;
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: REFUSED to write %s: the idleness criterion is not met, so these medians are not written. Re-run with the machine otherwise idle and the build lock absent."), *OutPath);
			return;
		}
		if (bSynthetic)
		{
			// A test's synthetic workload measures nothing; it is graded, never written.
			GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::NotRun;
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: the idleness criterion is met by a synthetic (test) workload; nothing is written."));
			return;
		}
		// T-2999 S1: the file is never handed a cost the runtime's Configure() would refuse.
		{
			const TPair<const TCHAR*, double> Written[] = {
				{ TEXT("LayerCostMs"), Costs.LayerCostMs }, { TEXT("FinishCostMs"), Costs.FinishCostMs },
				{ TEXT("PromptTokenCostMs"), Costs.PromptTokenCostMs }, { TEXT("ResetCostMs"), Costs.ResetCostMs },
				{ TEXT("AdoptCostMs"), Costs.AdoptCostMs }, { TEXT("SaveCostMs"), Costs.SaveCostMs },
				{ TEXT("RestoreCostMs"), Costs.RestoreCostMs }, { TEXT("PrefixBeginCostMs"), Costs.PrefixBeginCostMs },
				{ TEXT("PrefixReleaseCostMs"), Costs.PrefixReleaseCostMs },
			};
			for (const TPair<const TCHAR*, double>& Cost : Written)
			{
				if (!IsValidCostMs(Cost.Value))
				{
					GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::MeasurementFailed;
					UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: FAILED -- measurement failed (%s = %.9f ms is not a finite, positive cost); nothing is written."), Cost.Key, Cost.Value);
					return;
				}
			}
			const TPair<const TCHAR*, double> Slopes[] = {
				{ TEXT("LayerCostPerPositionMs"), Costs.LayerCostPerPositionMs },
				{ TEXT("PromptTokenCostPerPositionMs"), Costs.PromptTokenCostPerPositionMs },
			};
			for (const TPair<const TCHAR*, double>& Slope : Slopes)
			{
				if (!FMath::IsFinite(Slope.Value) || Slope.Value < 0.0)
				{
					GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::MeasurementFailed;
					UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: FAILED -- measurement failed (%s = %.9f ms is not a finite, non-negative slope); nothing is written."), Slope.Key, Slope.Value);
					return;
				}
			}
		}

		UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: measured in %.2f s."), RunSeconds);

		FString DepthLines;
		for (int32 K = 0; K < Costs.LayerArm.Num(); ++K)
		{
			DepthLines += FString::Printf(TEXT("LayerMedianMsAtDepth%lld=%.6f\nPromptTokenMedianMsAtDepth%lld=%.6f\n"),
				Costs.LayerArm[K].Depth, Costs.LayerArm[K].MedianMs, Costs.PromptArm[K].Depth, Costs.PromptArm[K].MedianMs);
		}
		const FString OutText = FString::Printf(TEXT(
			"; SuperSLM.CalibrateCosts output -- the plan §5 item 2, §10.2 item 10, §10.3.1 items 5a.1 and 5b\n"
			"; Artifact: %s\n"
			"; Idleness criterion (D-SLM7778) met: %d samples, ceiling %.4f%% (N = %d)\n"
			"; Cap C = %lld; the layer and prompt arms were timed at depths 1, C/4, C/2, 3C/4 and C - 1 (D-SLM7793/D-SLM7794)\n"
			"[SuperSLM.CalibratedCosts]\n"
			"LayerCostMs=%.6f\n"
			"LayerCostPerPositionMs=%.9f\n"
			"FinishCostMs=%.6f\n"
			"PromptTokenCostMs=%.6f\n"
			"PromptTokenCostPerPositionMs=%.9f\n"
			"ResetCostMs=%.6f\n"
			"AdoptCostMs=%.6f\n"
			"SaveCostMs=%.6f\n"
			"RestoreCostMs=%.6f\n"
			"PrefixBeginCostMs=%.6f\n"
			"PrefixReleaseCostMs=%.6f\n"
			"[SuperSLM.CalibratedCosts.Depths]\n"
			"CapTokens=%lld\n"
			"%s"),
			*ArtifactPath, Report.Samples.Num(), Report.CeilingPercent, Report.LogicalProcessors, Costs.CapTokens,
			Costs.LayerCostMs, Costs.LayerCostPerPositionMs, Costs.FinishCostMs,
			Costs.PromptTokenCostMs, Costs.PromptTokenCostPerPositionMs,
			Costs.ResetCostMs, Costs.AdoptCostMs, Costs.SaveCostMs, Costs.RestoreCostMs,
			Costs.PrefixBeginCostMs, Costs.PrefixReleaseCostMs, Costs.CapTokens, *DepthLines);
		if (FFileHelper::SaveStringToFile(OutText, *OutPath))
		{
			GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::Written;
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.CalibrateCosts: wrote %s. A project reads this file (or its own equivalent config surface) and copies these eleven values into FSuperSLMRuntimeConfig before Configure()."), *OutPath);
		}
		else
		{
			GLastOutcome = SuperSLMCalibrateCostsCommand::EOutcome::WriteFailed;
			UE_LOG(LogSuperSLM, Error, TEXT("SuperSLM.CalibrateCosts: could not write %s."), *OutPath);
		}
	}

	static FAutoConsoleCommand GSuperSLMCalibrateCostsCommand(
		TEXT("SuperSLM.CalibrateCosts"),
		TEXT("SuperSLM.CalibrateCosts <ArtifactPath.sslm> -- measures the eleven static per-unit scheduling costs (the layer and prompt arms at five depths from the artifact's cap) on this machine against the given artifact, and writes them to Saved/SuperSLM/CalibratedCosts.ini only when the idleness criterion is met: the foreign CPU share, sampled every 250 ms across a 2 s pre-window, the workload and a 2 s post-window, stays at or under 50/N percent in every 1 s window, and the build lock (SUPERSLM_BUILD_LOCK, when set) is never held by a foreign owner. Editor and packaged."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunCalibrateCostsCommand));
}

namespace SuperSLMCalibrateCostsCommand
{
	void Register()
	{
		// FAutoConsoleCommand above self-registers at static-init time; this function exists so
		// SuperSLMUnrealModule.cpp can force this translation unit to link (a console command
		// defined only in an otherwise-unreferenced .cpp can be stripped by the linker).
	}

	EOutcome GetLastOutcome()
	{
		return GLastOutcome;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FSuperSLMCalibrationTestAccess::SetPhaseHook(TFunction<void(ESuperSLMCalibrationEvent)> Hook)
{
	GTestPhaseHook = MoveTemp(Hook);
}

void FSuperSLMCalibrationTestAccess::SetSyntheticWorkload(TFunction<bool(FString& OutError)> Workload)
{
	GTestSyntheticWorkload = MoveTemp(Workload);
}

void FSuperSLMCalibrationTestAccess::SetSamplerDisabled(bool bDisabled)
{
	GTestSamplerDisabled = bDisabled;
}

void FSuperSLMCalibrationTestAccess::SetFinishParallelTasksOverride(int32 FinishParallelTasks)
{
	GTestFinishParallelTasksOverride = FinishParallelTasks;
}

void FSuperSLMCalibrationTestAccess::ClearOverrides()
{
	GTestPhaseHook.Reset();
	GTestSyntheticWorkload.Reset();
	GTestSamplerDisabled = false;
	GTestFinishParallelTasksOverride = -1;
}

const FSuperSLMCalibrationIdlenessReport& FSuperSLMCalibrationTestAccess::GetLastIdlenessReport()
{
	return GLastIdlenessReport;
}
#endif // WITH_DEV_AUTOMATION_TESTS
