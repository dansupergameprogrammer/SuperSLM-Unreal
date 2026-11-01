#pragma once

// Shared set-up for the GPU schema-bound cells: a rig (the CPU subsystem for the tokenizer and the C17 oracle, the
// GPU subsystem, the demo schema), runs driven tick by tick, saves and restores. Written for P2's Unreal cells
// (schema fast-decode plan rev 1.4.4, §6.1); since ruling 40 (red-suite-mr6.md) retired P2's Canonical GPU core and
// those cells, its users are the MR6 cells (SuperSLMGpuMultiRowMr6Tests.cpp). P2's GPU seam is deleted in plugin 1.4;
// each MR6 cell gates on FSuperSLMGpuMultiRowTestAccess and reads the multi-row action record.

#include "CoreMinimal.h"

#if SUPERSLMUNREAL_WITH_GPU

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "Misc/AutomationTest.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMPagedGpuTestSeam.h"

namespace SuperSLMGpuSchemaJumpFixtures
{
	using SuperSLMSchemaJumpFixtures::FCpuRig;
	using SuperSLMSchemaJumpFixtures::IsPrefixOf;

	inline constexpr double kRunSeconds = 600.0;
	inline constexpr float kStep = 1.0f / 60.0f;
	inline constexpr int32 kMaxNew = 96;

	// The engine statuses the cells inject or read, by value: the vendored gpu_1p0.h until the re-vendor, plus
	// 1.17.0's two (ordinals 31 and 32, pinned by the engine's census).
	namespace Status
	{
		inline constexpr int32 Ok = 0;
		inline constexpr int32 Busy = 2;
		inline constexpr int32 DeviceLost = 8;
		inline constexpr int32 SequenceRejected = 11;
		inline constexpr int32 AllocationFailed = 16;
		inline constexpr int32 KvBudgetExceeded = 24;
		inline constexpr int32 ForcedTableAbsent = 31;
		inline constexpr int32 ContextCapExceeded = 32;
		inline constexpr int32 Unknown = 9999; // the p2_ref tool's unknown value
	}

	enum class EPath : uint8
	{
		WholeToken, // OneCall, a whole token per tick
		Sliced4,    // Composed, 4 layers per tick (P2.5, Q25 A)
	};

	inline const TCHAR* PathName(EPath P) { return P == EPath::WholeToken ? TEXT("whole token") : TEXT("4 layers per tick"); }

	inline ESuperSLMGpuDecodePath DecodePathOf(EPath P) { return P == EPath::WholeToken ? ESuperSLMGpuDecodePath::OneCall : ESuperSLMGpuDecodePath::Composed; }

	// A GPU configuration for P2's cells. SequenceBudget 0 is dedicated mode; > 0 is pooled. The frame-budget fields
	// stay at their defaults (R_max 1) unless the cell sets them.
	inline FSuperSLMGpuRuntimeConfig JumpConfig(EPath Path, int32 SequenceBudget = 0, int32 BlockCount = 4, int64 ContextCap = 4096)
	{
		FSuperSLMGpuRuntimeConfig C = SequenceBudget > 0
			? SuperSLMPagingGpuFixtures::MakeGpuConfig(SuperSLMPagingGpuFixtures::EGpuArtifact::AEx05B, BlockCount, SequenceBudget, 0, 1, ContextCap)
			: FSuperSLMGpuRuntimeConfig();
		C.ContextCap = ContextCap;
		C.BlockCount = BlockCount;
		C.TickBudgetMs = 1000.0;
		const int32 Layers = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		const int32 PerSlice = Path == EPath::WholeToken ? Layers : 4;
		C.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(PerSlice);
		// At least Configure's MinimumK on either path (BlockCount slots of a token's slices), and one more for a
		// pooled build seat.
		C.K = FMath::Max3(Layers, BlockCount + 1, (BlockCount * Layers + PerSlice - 1) / PerSlice);
		C.bCalibrateGpuRowCost = false; // ruling 51 F4: only the R48/R50/R51 cells calibrate
		return C;
	}

	struct FGpuRig
	{
		FCpuRig Cpu; // the tokenizer and the CPU oracle (C17)
		USuperSLMGpuSubsystem* Gpu = nullptr;
		FSuperSLMGpuSchemaHandle Schema;
		FSuperSLMGpuRuntimeConfig Config;
	};

	inline bool ConfigureGpu(FAutomationTestBase& T, FGpuRig& Rig, const FSuperSLMGpuRuntimeConfig& Config)
	{
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Cpu.Model, Config);
		if (!T.TestEqual(*FString::Printf(TEXT("GPU Configure (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = Config;
		FString Error;
		return T.TestTrue(*Error, FSuperSLMGpuSchemaLookup::LookupByName(*Rig.Cpu.Model, SuperSLML2S2Fixtures::DemoSchemaName(), Rig.Schema, Error));
	}

	inline bool SetUp(FAutomationTestBase& T, UWorld* World, const FSuperSLMGpuRuntimeConfig& Config, FGpuRig& Out)
	{
		if (!SuperSLMSchemaJumpFixtures::SetUpCpu(T, World, SuperSLMSchemaJumpFixtures::BaseConfig(4, 4), Out.Cpu))
		{
			return false;
		}
		Out.Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World);
		return T.TestNotNull(TEXT("GPU subsystem"), Out.Gpu) && ConfigureGpu(T, Out, Config);
	}

	inline bool MakeRequest(FAutomationTestBase& T, const FGpuRig& Rig, ESuperSLMSchemaJumpForward Mode, int32 MaxNewTokens, FSuperSLMGenerationRequest& Out,
		const FString& Prompt = SuperSLML2S2Fixtures::PinnedSelfCheckPrompt())
	{
		return SuperSLMSchemaJumpFixtures::MakeRequest(T, Rig.Cpu, Prompt, Mode, MaxNewTokens, Out);
	}

	// One GPU run's readings.
	struct FGpuRun
	{
		FSuperSLMGpuSequence Sequence;
		TArray<int32> Tokens;
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMDecodeOutcome Outcome = ESuperSLMDecodeOutcome::TokenProduced;
		ESuperSLMGpuFaultReason Fault = ESuperSLMGpuFaultReason::None;
		bool bAccepting = false;
		int64 ForcedTokenCount = 0;
		TArray<TArray<int32>> Snapshots; // GetGeneratedTokens() after every tick (the listener check)
		FString Error;
	};

	inline bool IsEnded(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq)
	{
		const ESuperSLMSequencePhase P = Gpu.GetPhase(Seq);
		return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted;
	}

	inline void Read(const FGpuRig& Rig, FGpuRun& Run)
	{
		Run.Phase = Rig.Gpu->GetPhase(Run.Sequence);
		Run.Tokens = Rig.Gpu->GetGeneratedTokens(Run.Sequence);
		Run.Outcome = Rig.Gpu->GetLastDecodeOutcome(Run.Sequence);
		Run.Fault = Rig.Gpu->GetLastFaultReason(Run.Sequence);
		Run.bAccepting = Rig.Gpu->IsSchemaAccepting(Run.Sequence);
		Run.ForcedTokenCount = Rig.Gpu->GetForcedTokenCount(Run.Sequence);
	}

	// Ticks until Done(), paced on the device. False on the wall-clock bound.
	inline bool Pump(USuperSLMGpuSubsystem& Gpu, TFunctionRef<bool()> Done, double Seconds = kRunSeconds, TFunctionRef<void()> AfterTick = [](){})
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done())
		{
			if (FPlatformTime::Seconds() - Start > Seconds) { return false; }
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu.Tick(kStep);
			AfterTick();
			FPlatformProcess::Sleep(0.0005f);
		}
		return true;
	}

	// Begins Request on Run.Sequence (already vended and bound) and ticks it to its end, or, with StopWhen, until
	// StopWhen() holds after a tick (the run is left mid-generation). Recording is not restarted here.
	inline bool Drive(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGenerationRequest& Request, FGpuRun& Run,
		TFunction<bool()> StopWhen = nullptr)
	{
		const FSuperSLMLifecycleOpHandle Handle = Rig.Gpu->RequestBeginGeneration(Run.Sequence, Request);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestBeginGeneration (%s)"), *Rig.Gpu->GetLastLifecycleRequestError()), Handle.IsValid())) { return false; }
		const bool bDone = Pump(*Rig.Gpu, [&]()
		{
			const ESuperSLMRestoreResult Started = Rig.Gpu->GetLifecycleOpResult(Handle);
			if (Started == ESuperSLMRestoreResult::Pending) { return false; }
			if (Started != ESuperSLMRestoreResult::Success) { Run.Error = TEXT("the begin did not resolve Success"); return true; }
			return IsEnded(*Rig.Gpu, Run.Sequence) || (StopWhen && StopWhen());
		}, kRunSeconds, [&]() { Run.Snapshots.Add(Rig.Gpu->GetGeneratedTokens(Run.Sequence)); });
		Read(Rig, Run);
		return T.TestTrue(*FString::Printf(TEXT("the run reaches its end or stop (%s)"), *Run.Error), bDone && Run.Error.IsEmpty());
	}

	inline bool Vend(FAutomationTestBase& T, const FGpuRig& Rig, EPath Path, FGpuRun& Run, bool bUnbound = false)
	{
		FString Error;
		if (!T.TestEqual(TEXT("GPU vend"), (uint8)Rig.Gpu->VendSequence(Run.Sequence, DecodePathOf(Path)), (uint8)ESuperSLMGpuVendResult::Success)) { return false; }
		return bUnbound || T.TestTrue(*FString::Printf(TEXT("GPU schema bind (%s)"), *Error), Rig.Gpu->SetSchema(Run.Sequence, Rig.Schema, Error));
	}

	inline void ReturnAndSettle(const FGpuRig& Rig, const FSuperSLMGpuSequence& Seq)
	{
		Rig.Gpu->ReturnSequence(Seq);
		SuperSLMPagingGpuFixtures::DrainQuiet(*Rig.Gpu);
	}

	// The CPU's run of the same request in Mode (the C17 oracle).
	inline bool RunCpu(FAutomationTestBase& T, const FGpuRig& Rig, FSuperSLMGenerationRequest Request, ESuperSLMSchemaJumpForward Mode,
		SuperSLMSchemaJumpFixtures::FRunResult& Out, bool bUnbound = false)
	{
		Request.SchemaJumpForward = Mode;
		return SuperSLMSchemaJumpFixtures::RunSolo(T, Rig.Cpu, Request, Out, bUnbound);
	}

	// The ending of a GPU run against the CPU's: tokens, phase, outcome.
	inline bool SameAsCpu(FAutomationTestBase& T, const FGpuRun& Gpu, const SuperSLMSchemaJumpFixtures::FRunResult& Cpu, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": tokens equal the CPU's")), Gpu.Tokens, Cpu.Tokens);
		bOk &= T.TestEqual(*(What + TEXT(": phase equals the CPU's")), (uint8)Gpu.Phase, (uint8)Cpu.Phase);
		bOk &= T.TestEqual(*(What + TEXT(": last decode outcome equals the CPU's")), (uint8)Gpu.Outcome, (uint8)Cpu.Outcome);
		return bOk;
	}

	inline bool SameGpuEnding(FAutomationTestBase& T, const FGpuRun& A, const FGpuRun& B, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": tokens")), A.Tokens, B.Tokens);
		bOk &= T.TestEqual(*(What + TEXT(": phase")), (uint8)A.Phase, (uint8)B.Phase);
		bOk &= T.TestEqual(*(What + TEXT(": last decode outcome")), (uint8)A.Outcome, (uint8)B.Outcome);
		bOk &= T.TestEqual(*(What + TEXT(": ForcedTokenCount")), A.ForcedTokenCount, B.ForcedTokenCount);
		return bOk;
	}

	// Saves Seq, waits, and returns the blob.
	inline bool Save(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuSequence& Seq, TArray<uint8>& OutBlob)
	{
		const FSuperSLMLifecycleOpHandle Handle = Rig.Gpu->RequestSaveSequence(Seq);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestSaveSequence (%s)"), *Rig.Gpu->GetLastLifecycleRequestError()), Handle.IsValid())) { return false; }
		const ESuperSLMRestoreResult Result = SuperSLML2S2Fixtures::DriveSaveToResolution(*Rig.Gpu, Handle, OutBlob, 120.0);
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the blob has bytes"), OutBlob.Num() > 0);
	}

	inline ESuperSLMRestoreResult Restore(const FGpuRig& Rig, const TArray<uint8>& Blob, FSuperSLMGpuSequence& OutSeq)
	{
		const FSuperSLMLifecycleOpHandle Handle = Rig.Gpu->RequestRestoreSequence(Blob, Rig.Cpu.Model);
		if (!Handle.IsValid()) { return ESuperSLMRestoreResult::Pending; }
		return SuperSLML2S2Fixtures::DriveRestoreToResolution(*Rig.Gpu, Handle, OutSeq, 120.0);
	}

	// The GPU section's wrapper in a blob: the offset of its magic ('S','L','U','G', 0x47554c53 little-endian), or
	// INDEX_NONE. Its version is the u32 after it: 1, the only version plugin 1.4 writes (rulings 39 Q8 and 40).
	inline int32 FindWrapperExt(const TArray<uint8>& Blob)
	{
		for (int32 I = 0; I + 12 <= Blob.Num(); ++I)
		{
			if (Blob[I] == 'S' && Blob[I + 1] == 'L' && Blob[I + 2] == 'U' && Blob[I + 3] == 'G') { return I; }
		}
		return INDEX_NONE;
	}

	inline int32 WrapperVersion(const TArray<uint8>& Blob)
	{
		const int32 At = FindWrapperExt(Blob);
		return At == INDEX_NONE ? -1 : static_cast<int32>(Blob[At + 4] | (Blob[At + 5] << 8) | (Blob[At + 6] << 16) | (Blob[At + 7] << 24));
	}

}

#endif // SUPERSLMUNREAL_WITH_GPU
