#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && PLATFORM_WINDOWS

#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace SuperSLMPagingLifecyclePricing
{
	constexpr int32 Repetitions = 50;
	constexpr int32 Blocks = 3;
	constexpr int32 PrefixLength = 1000;
	constexpr int32 AdoptBudget = 512;
	constexpr int32 Tasks = 4;

	struct FCpuReading
	{
		uint64 Total = 0, Busy = 0, Own = 0;
		bool bValid = false;
	};

	uint64 Ticks(const FILETIME& Value) { return (uint64(Value.dwHighDateTime) << 32) | Value.dwLowDateTime; }
	FCpuReading ReadCpu()
	{
		FILETIME Idle, Kernel, User, Created, Exited, OwnKernel, OwnUser;
		FCpuReading Result;
		Result.bValid = GetSystemTimes(&Idle, &Kernel, &User) && GetProcessTimes(GetCurrentProcess(), &Created, &Exited, &OwnKernel, &OwnUser);
		if (Result.bValid)
		{
			Result.Total = Ticks(Kernel) + Ticks(User);
			Result.Busy = Result.Total - Ticks(Idle);
			Result.Own = Ticks(OwnKernel) + Ticks(OwnUser);
		}
		return Result;
	}

	struct FBench
	{
		sslm_model Model = nullptr;
		sslm_kv_pool Pool = nullptr;
		sslm_workspace Workspace = nullptr;
		sslm_seq Source = nullptr, Target = nullptr, Restored = nullptr;
		sslm_prefix Persona = nullptr, Begun = nullptr;
		void* PoolMemory = nullptr;
		void* WorkspaceMemory = nullptr;
		void* SaveMemory = nullptr;
		size_t SavedBytes = 0;
		int32 Cap = 0;

		~FBench()
		{
			if (Restored) { sslm_seq_release(Restored); }
			if (Target) { sslm_seq_release(Target); }
			if (Source) { sslm_seq_release(Source); }
			if (Begun) { sslm_prefix_release(Begun); }
			if (Persona) { sslm_prefix_release(Persona); }
			if (Workspace) { sslm_workspace_destroy(Workspace); }
			if (Pool) { sslm_kv_pool_destroy(Pool); }
			FMemory::Free(SaveMemory);
			FMemory::Free(WorkspaceMemory);
			FMemory::Free(PoolMemory);
			if (Model) { SuperSLMRuntime::ReleaseModelMapping(Model); }
		}

		bool Ok(FAutomationTestBase& T, const TCHAR* Operation, sslm_status Status)
		{
			return T.TestEqual(Operation, int32(Status), int32(SSLM_OK));
		}

		bool State(FAutomationTestBase& T, sslm_seq Sequence, int64 Depth, int64 Origin, int32 Budget, sslm_seq_kv_stats_out* OutObserved = nullptr)
		{
			sslm_seq_kv_stats_out Stats = {};
			Stats.struct_size = sizeof(Stats);
			const bool bValid = Ok(T, TEXT("actual ABI sequence stats"), sslm_seq_kv_stats(Sequence, &Stats))
				&& T.TestEqual(TEXT("actual budget mode"), Stats.mode, uint32(1))
				&& T.TestEqual(TEXT("actual context length"), Stats.context_length, Depth)
				&& T.TestEqual(TEXT("actual origin"), Stats.origin, Origin)
				&& T.TestEqual(TEXT("actual budget"), Stats.budget, Budget)
				&& T.TestEqual(TEXT("actual limit"), Stats.limit, FMath::Min<int64>(Origin + Budget, Cap));
			if (OutObserved) { *OutObserved = Stats; }
			return bValid;
		}

		bool Prepare(FAutomationTestBase& T, const USuperSLMModel& Asset, int32 ExpectedCap, int32 Mode)
		{
			SuperSLMRuntime::FModelShape Shape;
			FString Error;
			if (!T.TestTrue(TEXT("artifact shape reads"), SuperSLMRuntime::ReadModelShape(Asset, Shape, Error))
				|| !T.TestEqual(TEXT("real artifact cap"), Shape.ContextCap, int64(ExpectedCap))
				|| !T.TestEqual(TEXT("real artifact layer count"), Shape.NumHiddenLayers, ExpectedCap == 4096 ? 24 : 28)) { return false; }
			Cap = ExpectedCap;
			Model = SuperSLMRuntime::AcquireModelMapping(Asset, Error);
			if (!T.TestNotNull(*FString::Printf(TEXT("model maps (%s)"), *Error), Model)) { return false; }
			const int64 PagePositions = sslm_kv_page_positions(Model);
			const size_t PageBytes = sslm_kv_page_size(Model);
			if (!T.TestTrue(TEXT("positive real page geometry"), PagePositions > 0 && PageBytes > 0)) { return false; }
			const uint32 Pages = uint32(4 * ((int64(Cap) + PagePositions - 1) / PagePositions));
			const size_t Overhead = sslm_kv_page_pool_overhead_size(Model, Pages);
			if (!T.TestTrue(TEXT("bounded page-pool allocation"), Overhead > 0 && Overhead < SIZE_MAX && PageBytes <= (SIZE_MAX - Overhead) / Pages)) { return false; }
			const size_t PoolBytes = size_t(Pages) * PageBytes + Overhead;
			const sslm_config Config = { 1, 64, 1, 0 };
			const size_t WorkspaceBytes = sslm_workspace_size(Model, &Config);
			const size_t StateBytes = sslm_seq_state_size(Model);
			if (!T.TestTrue(TEXT("positive workspace/save sizes"), WorkspaceBytes > 0 && StateBytes > 0)) { return false; }
			PoolMemory = FMemory::Malloc(PoolBytes, SSLM_ABI_ALIGNMENT_BYTES);
			WorkspaceMemory = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
			SaveMemory = FMemory::Malloc(StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
			if (!T.TestTrue(TEXT("benchmark allocations succeed"), PoolMemory && WorkspaceMemory && SaveMemory)
				|| !Ok(T, TEXT("real page pool"), sslm_kv_page_pool_create(Model, PoolMemory, PoolBytes, Pages, &Pool))
				|| !Ok(T, TEXT("workspace"), sslm_workspace_create(Model, &Config, WorkspaceMemory, WorkspaceBytes, &Workspace))) { return false; }
			const sslm_parallel_for Hook = SuperSLMFinishHook::Make(Tasks, Mode ? SSLM_PARALLEL_FOR_MATVEC : 0);
			if (!Ok(T, TEXT("same mode/task hook as runtime"), sslm_workspace_set_parallel_for(Workspace, &Hook))
				|| !Ok(T, TEXT("budgeted persona"), sslm_prefix_begin_budgeted(Model, &Pool, PrefixLength, &Persona))) { return false; }
			TArray<int32> Tokens;
			Tokens.Init(0, Cap); // the calibration command's in-vocabulary setup token, at real size
			for (int32 Done = 0; Done < PrefixLength;)
			{
				int32 Used = 0;
				if (!Ok(T, TEXT("real persona prefill"), sslm_prefix_prefill(Model, Persona, Tokens.GetData() + Done,
					PrefixLength - Done, 64, SSLM_SPAN_PROMPT, Workspace, &Used))
					|| !T.TestTrue(TEXT("persona prefill progresses"), Used > 0 && Used <= PrefixLength - Done)) { return false; }
				Done += Used;
			}
			if (!Ok(T, TEXT("freeze real persona"), sslm_prefix_freeze(Persona))
				|| !Ok(T, TEXT("full-budget source"), sslm_seq_create_budgeted(Model, &Pool, Cap, &Source))
				|| !Ok(T, TEXT("adopt target"), sslm_seq_create_budgeted(Model, &Pool, AdoptBudget, &Target))) { return false; }
			int32 NextProgress = 2048;
			for (int32 Done = 0; Done < Cap;)
			{
				int32 Used = 0;
				if (!Ok(T, TEXT("real full-cap prefill"), sslm_prefill(Model, Source, Tokens.GetData() + Done,
					Cap - Done, 64, SSLM_SPAN_PROMPT, Workspace, &Used))
					|| !T.TestTrue(TEXT("full-cap prefill progresses"), Used > 0 && Used <= Cap - Done)) { return false; }
				Done += Used;
				if (Done >= NextProgress || Done == Cap)
				{
					UE_LOG(LogSuperSLM, Display, TEXT("K4 lifecycle preparation: mode=%d context=%d/%d"), Mode, Done, Cap);
					NextProgress = Done + 2048;
				}
			}
			SavedBytes = StateBytes;
			return State(T, Source, Cap, 0, Cap)
				&& Ok(T, TEXT("save actual full-cap budget state"), sslm_seq_save(Source, SaveMemory, &SavedBytes))
				&& T.TestTrue(TEXT("full-cap save is bounded"), SavedBytes > 0 && SavedBytes <= StateBytes);
		}

		bool Measure(FAutomationTestBase& T, int32 Arm, double& Milliseconds, sslm_seq_kv_stats_out& Observed)
		{
			if (!Ok(T, TEXT("prepare empty target outside timer"), sslm_seq_reset(Target)) || !State(T, Target, 0, 0, AdoptBudget)) { return false; }
			const double Start = FPlatformTime::Seconds();
			const sslm_status Status = Arm == 0 ? sslm_seq_adopt_prefix(Target, Persona)
				: Arm == 1 ? sslm_seq_restore(Model, &Pool, SaveMemory, SavedBytes, &Restored)
				: sslm_prefix_begin_budgeted(Model, &Pool, Cap, &Begun);
			Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
			if (!Ok(T, TEXT("timed budget lifecycle call"), Status)) { return false; }
			if (Arm == 0) { return State(T, Target, PrefixLength, PrefixLength, AdoptBudget, &Observed); }
			if (Arm == 1)
			{
				const bool bValid = State(T, Restored, Cap, 0, Cap, &Observed);
				const bool bReleased = Ok(T, TEXT("release restored holder outside timer"), sslm_seq_release(Restored));
				if (bReleased) { Restored = nullptr; }
				return bValid && bReleased;
			}
			// Observe the newly begun prefix through its real frozen empty state and a real adopt.
			// Neither freeze, adopt, state read nor release is inside the Begin timer.
			if (!T.TestNotNull(TEXT("begin creates a real prefix"), Begun)
				|| !Ok(T, TEXT("freeze empty begun prefix"), sslm_prefix_freeze(Begun))
				|| !Ok(T, TEXT("observe empty prefix by adopt"), sslm_seq_adopt_prefix(Target, Begun))
				|| !State(T, Target, 0, 0, AdoptBudget, &Observed)
				|| !Ok(T, TEXT("drop observer outside timer"), sslm_seq_reset(Target))) { return false; }
			const bool bReleased = Ok(T, TEXT("release begun prefix outside timer"), sslm_prefix_release(Begun));
			if (bReleased) { Begun = nullptr; }
			return bReleased;
		}
	};

	bool Run(FAutomationTestBase& T, const TCHAR* EnvironmentName, int32 ExpectedCap)
	{
		const FString Artifact = FPlatformMisc::GetEnvironmentVariable(EnvironmentName);
		if (!T.TestTrue(TEXT("real artifact exists"), !Artifact.IsEmpty() && IFileManager::Get().FileExists(*Artifact))) { return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		TStrongObjectPtr<USuperSLMModel> Asset(FSuperSLMModelImport::ImportFromFile(Artifact, Diagnostic));
		if (!T.TestTrue(TEXT("real artifact imports"), Asset.IsValid() && Diagnostic.bAccepted)) { return false; }
		uint8 Hash[32];
		if (!T.TestTrue(TEXT("artifact identity reads"), SuperSLMRuntime::ReadArtifactHash(*Asset.Get(), Hash))) { return false; }
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("SuperSLMPagingPrices") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!T.TestTrue(TEXT("unique evidence directory"), IFileManager::Get().MakeDirectory(*Directory, true))) { return false; }
		UE_LOG(LogSuperSLM, Display, TEXT("K4 lifecycle evidence directory: %s; artifact=%s hash=%s expected_cap=%d"),
			*Directory, *Artifact, *SuperSLMRuntime::HashToHex(Hash), ExpectedCap);
		FString Csv(TEXT("block,mode,arm,phase,rep,elapsed_ms,cap,call_budget,observed_context,observed_origin,observed_budget,observed_limit\n"));
		const TCHAR* Arms[] = { TEXT("adopt_1000"), TEXT("restore_full"), TEXT("prefix_begin_full_budget") };
		const FCpuReading CpuBefore = ReadCpu();
		const double RunStart = FPlatformTime::Seconds();
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FBench Bench;
			if (!Bench.Prepare(T, *Asset.Get(), ExpectedCap, Mode)) { return false; }
			for (int32 Block = 0; Block < Blocks; ++Block)
			{
				for (int32 Arm = 0; Arm < 3; ++Arm)
				{
					TArray<double> Samples;
					for (int32 Phase = 0; Phase < 2; ++Phase)
					{
						for (int32 Rep = 0; Rep < Repetitions; ++Rep)
						{
							double Milliseconds = 0;
							sslm_seq_kv_stats_out Observed = {};
							if (!Bench.Measure(T, Arm, Milliseconds, Observed)) { return false; }
							Csv += FString::Printf(TEXT("%d,%d,%s,%s,%d,%.9f,%d,%d,%lld,%lld,%d,%lld\n"),
								Block, Mode, Arms[Arm], Phase ? TEXT("timed") : TEXT("warmup"), Rep, Milliseconds, ExpectedCap,
								Arm == 0 ? AdoptBudget : ExpectedCap, Observed.context_length, Observed.origin, Observed.budget, Observed.limit);
							if (!T.TestTrue(*FString::Printf(TEXT("mode %d arm %s phase %d rep %d: raw %.9f ms is finite and positive"),
								Mode, Arms[Arm], Phase, Rep, Milliseconds), FMath::IsFinite(Milliseconds) && Milliseconds > 0.0))
							{
								FFileHelper::SaveStringToFile(Csv, *(Directory / TEXT("calls.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
								return false;
							}
							if (Phase) { Samples.Add(Milliseconds); }
						}
					}
					Samples.Sort();
					T.AddInfo(FString::Printf(TEXT("K4 lifecycle estimate, not verified: artifact=%s block=%d mode=%d tasks=4 arm=%s warmup=50 timed=50 median_ms=%.9f"),
						*SuperSLMRuntime::HashToHex(Hash), Block, Mode, Arms[Arm], (Samples[24] + Samples[25]) / 2.0));
					if (!T.TestTrue(TEXT("raw samples checkpointed"), FFileHelper::SaveStringToFile(Csv, *(Directory / TEXT("calls.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) { return false; }
				}
			}
		}
		const FCpuReading CpuAfter = ReadCpu();
		if (!T.TestTrue(TEXT("whole-run CPU counters are valid and monotonic"), CpuBefore.bValid && CpuAfter.bValid
			&& CpuAfter.Total > CpuBefore.Total && CpuAfter.Busy >= CpuBefore.Busy && CpuAfter.Own >= CpuBefore.Own)) { return false; }
		const double Total = double(CpuAfter.Total - CpuBefore.Total);
		const double Box = 100.0 * double(CpuAfter.Busy - CpuBefore.Busy) / Total;
		const double Own = 100.0 * double(CpuAfter.Own - CpuBefore.Own) / Total;
		TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
		Meta->SetStringField(TEXT("artifact_path"), FPaths::GetCleanFilename(Artifact)); // the file name: a capture records no machine path
		Meta->SetStringField(TEXT("artifact_hash"), SuperSLMRuntime::HashToHex(Hash));
		Meta->SetNumberField(TEXT("cap"), ExpectedCap);
		Meta->SetNumberField(TEXT("tasks"), Tasks);
		Meta->SetNumberField(TEXT("blocks_per_mode"), Blocks);
		Meta->SetStringField(TEXT("block_scope"), TEXT("Three sequential measurement blocks per mode in one process; actual full-context setup is prepared once per mode. No independence or accuracy bound is claimed."));
		Meta->SetNumberField(TEXT("warmup_per_arm_mode"), Repetitions);
		Meta->SetNumberField(TEXT("timed_per_arm_mode"), Repetitions);
		Meta->SetNumberField(TEXT("timer_ms_per_cycle"), FPlatformTime::GetSecondsPerCycle64() * 1000.0);
		Meta->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - RunStart);
		Meta->SetNumberField(TEXT("whole_run_box_busy_pct"), Box);
		Meta->SetNumberField(TEXT("whole_run_own_pct"), Own);
		Meta->SetNumberField(TEXT("whole_run_foreign_pct"), FMath::Max(0.0, Box - Own));
		Meta->SetStringField(TEXT("load_scope"), TEXT("Whole-run interval averages including actual full-context preparation; not per-call load, not an idle-box verdict or an accuracy bound."));
		Meta->SetStringField(TEXT("observations"), TEXT("observed_* are actual ABI sequence stats: the adopter, restored sequence, or an adopter of the newly frozen empty Begin prefix. call_budget is the target's pre-existing adopt budget or the restore/Begin budget."));
		FString Json;
		if (!T.TestTrue(TEXT("metadata serializes"), FJsonSerializer::Serialize(Meta, TJsonWriterFactory<>::Create(&Json)))
			|| !T.TestTrue(TEXT("metadata retained"), FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("metadata.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) { return false; }
		T.AddInfo(FString::Printf(TEXT("K4 lifecycle evidence: %s; 1800 raw calls, actual full-cap input, whole-run foreign %.6f%%; no settings written"), *Directory, FMath::Max(0.0, Box - Own)));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLifecycleAExTest, "SuperSLM.Paging.Cpu.Figures.LifecyclePrices.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingLifecycleAExTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLifecyclePricing::Run(*this, TEXT("SUPERSLM_L2S1_AEX_PATH"), 4096);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLifecycle15BTest, "SuperSLM.Paging.Cpu.Figures.LifecyclePrices.Shipped15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingLifecycle15BTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLifecyclePricing::Run(*this, TEXT("SUPERSLM_PAGING_1P5B_PATH"), 32768);
}

#endif
