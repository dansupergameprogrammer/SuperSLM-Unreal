// K0R part 4: report the legacy restore cost at the brief's required 1,000 positions.
// This uses CalibrateCosts' raw ABI calls, 50-sample upper median, discarded warm-up,
// runtime mapping, finish hook, and idleness sampler. The existing command's own
// restore arm saves after reset (depth 0). This cell supplies a real depth-1000
// workload through its test hook and never writes calibration settings. It also reports the stock
// one-token adopt and empty-prefix Begin arms when another stock arm prevents a complete calibration.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

#include "SuperSLMCalibrationTestAccess.h"
#include "SuperSLMFinishHook.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMRuntimeResolve.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"
#include "UObject/StrongObjectPtr.h"
#include "superslm/sslm_abi.h"

namespace
{
	struct FK0RRestoreBench
	{
		sslm_model Mapping = nullptr;
		sslm_kv_pool Pool = nullptr;
		sslm_workspace Workspace = nullptr;
		sslm_seq Sequence = nullptr;
		sslm_prefix Prefix = nullptr;
		void* KvMemory = nullptr;
		void* WorkspaceMemory = nullptr;
		void* SaveMemory = nullptr;
		size_t StateBytes = 0;
		size_t SavedBytes = 0;

		~FK0RRestoreBench()
		{
			if (Sequence) { sslm_seq_release(Sequence); }
			if (Prefix) { sslm_prefix_release(Prefix); }
			if (Workspace) { sslm_workspace_destroy(Workspace); }
			if (Pool) { sslm_kv_pool_destroy(Pool); }
			FMemory::Free(SaveMemory);
			FMemory::Free(WorkspaceMemory);
			FMemory::Free(KvMemory);
			if (Mapping) { SuperSLMRuntime::ReleaseModelMapping(Mapping); }
		}

		bool CheckDepth(FAutomationTestBase& Test) const
		{
			sslm_seq_kv_stats_out Stats = {};
			Stats.struct_size = uint32(sizeof(Stats));
			return Test.TestEqual(TEXT("legacy sequence stats"), int32(sslm_seq_kv_stats(Sequence, &Stats)), int32(SSLM_OK))
				&& Test.TestEqual(TEXT("whole-reserve mode"), Stats.mode, uint32(0))
				&& Test.TestEqual(TEXT("actual context length"), Stats.context_length, int64(1000));
		}

		bool RestorePopulation(FAutomationTestBase& Test, TArray<double>& Samples)
		{
			Samples.Reset();
			for (int32 Rep = 0; Rep < 50; ++Rep)
			{
				// Save the unchanged depth-1000 state before each restore, as the stock
				// lifecycle arm saves its unchanged depth-0 state before each restore.
				size_t Bytes = StateBytes;
				if (!Test.TestEqual(TEXT("save before restore"), int32(sslm_seq_save(Sequence, SaveMemory, &Bytes)), int32(SSLM_OK))
					|| !Test.TestTrue(TEXT("saved byte count is bounded"), Bytes > 0 && Bytes <= StateBytes))
				{
					return false;
				}
				SavedBytes = Bytes;
				sslm_seq Restored = nullptr;
				const double Begin = FPlatformTime::Seconds();
				const sslm_status Status = sslm_seq_restore(Mapping, &Pool, SaveMemory, Bytes, &Restored);
				const double Milliseconds = (FPlatformTime::Seconds() - Begin) * 1000.0;
				if (!Test.TestEqual(TEXT("timed restore"), int32(Status), int32(SSLM_OK)) || !Restored)
				{
					if (Restored) { sslm_seq_release(Restored); }
					return false;
				}
				const sslm_status Released = sslm_seq_release(Sequence);
				Sequence = Restored;
				if (!Test.TestEqual(TEXT("release replaced sequence"), int32(Released), int32(SSLM_OK))
					|| !CheckDepth(Test) || !Test.TestTrue(TEXT("finite positive restore duration"), FMath::IsFinite(Milliseconds) && Milliseconds > 0.0))
				{
					return false;
				}
				Samples.Add(Milliseconds);
			}
			return true;
		}

		bool RestoreSeed(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("depth-1000 seed exists"), SavedBytes > 0)) { return false; }
			sslm_seq Restored = nullptr;
			if (!Test.TestEqual(TEXT("untimed depth-1000 seed restore"), int32(sslm_seq_restore(Mapping, &Pool, SaveMemory, SavedBytes, &Restored)), int32(SSLM_OK)) || !Restored)
			{
				if (Restored) { sslm_seq_release(Restored); }
				return false;
			}
			const sslm_status Status = sslm_seq_release(Sequence);
			Sequence = Restored;
			return Test.TestEqual(TEXT("release replaced seed sequence"), int32(Status), int32(SSLM_OK)) && CheckDepth(Test);
		}

		bool LifecyclePopulation(FAutomationTestBase& Test, TArray<double>& Adopt, TArray<double>& Begin)
		{
			Adopt.Reset();
			Begin.Reset();
			if (!Test.TestEqual(TEXT("one-token prefix begin"), int32(sslm_prefix_begin(Mapping, &Pool, &Prefix)), int32(SSLM_OK)) || !Prefix) { return false; }
			const int32 Token = 0;
			int32 Consumed = 0;
			if (!Test.TestEqual(TEXT("one-token prefix prefill"), int32(sslm_prefix_prefill(Mapping, Prefix, &Token, 1, 1, SSLM_SPAN_PROMPT, Workspace, &Consumed)), int32(SSLM_OK))
				|| !Test.TestEqual(TEXT("one-token prefix consumed"), Consumed, 1)
				|| !Test.TestEqual(TEXT("one-token prefix freeze"), int32(sslm_prefix_freeze(Prefix)), int32(SSLM_OK))) { return false; }
			for (int32 Rep = 0; Rep < 50; ++Rep)
			{
				if (!Test.TestEqual(TEXT("untimed reset before adopt"), int32(sslm_seq_reset(Sequence)), int32(SSLM_OK))) { return false; }
				const double Start = FPlatformTime::Seconds();
				const sslm_status Status = sslm_seq_adopt_prefix(Sequence, Prefix);
				const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (!Test.TestEqual(TEXT("timed one-token adopt"), int32(Status), int32(SSLM_OK))
					|| !Test.TestTrue(TEXT("finite nonnegative adopt duration"), FMath::IsFinite(Ms) && Ms >= 0.0)) { return false; }
				Adopt.Add(Ms);
			}
			sslm_seq_kv_stats_out AdoptedStats = {};
			AdoptedStats.struct_size = uint32(sizeof(AdoptedStats));
			if (!Test.TestEqual(TEXT("adopted sequence stats"), int32(sslm_seq_kv_stats(Sequence, &AdoptedStats)), int32(SSLM_OK))
				|| !Test.TestEqual(TEXT("actual adopted context length"), AdoptedStats.context_length, int64(1))) { return false; }
			if (!Test.TestEqual(TEXT("release adopted prefix"), int32(sslm_prefix_release(Prefix)), int32(SSLM_OK))) { return false; }
			Prefix = nullptr;
			for (int32 Rep = 0; Rep < 50; ++Rep)
			{
				const double Start = FPlatformTime::Seconds();
				const sslm_status Status = sslm_prefix_begin(Mapping, &Pool, &Prefix);
				const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (!Test.TestEqual(TEXT("timed empty prefix begin"), int32(Status), int32(SSLM_OK)) || !Prefix
					|| !Test.TestTrue(TEXT("finite nonnegative prefix begin duration"), FMath::IsFinite(Ms) && Ms >= 0.0)) { return false; }
				Begin.Add(Ms);
				if (!Test.TestEqual(TEXT("untimed empty prefix release"), int32(sslm_prefix_release(Prefix)), int32(SSLM_OK))) { return false; }
				Prefix = nullptr;
			}
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMK0RRestorePricingTest,
	"SuperSLM.K0R.LegacyPricing.Restore1000", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMK0RRestorePricingTest::RunTest(const FString& Parameters)
{
	FSuperSLMLegacyCalibrationScope CommandScope;
	const FString Artifact = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S1_AEX_PATH"));
	if (!TestTrue(TEXT("A-EX path names an existing artifact"), !Artifact.IsEmpty() && IFileManager::Get().FileExists(*Artifact))) { return false; }
	FSuperSLMImportDiagnostic Diagnostic;
	TStrongObjectPtr<USuperSLMModel> Model(FSuperSLMModelImport::ImportFromFile(Artifact, Diagnostic));
	if (!TestTrue(TEXT("A-EX imports"), Model.IsValid() && Diagnostic.bAccepted)) { return false; }
	FK0RRestoreBench Bench;
	FString Error;
	Bench.Mapping = SuperSLMRuntime::AcquireModelMapping(*Model.Get(), Error);
	if (!TestNotNull(*FString::Printf(TEXT("model maps (%s)"), *Error), Bench.Mapping)) { return false; }
	const sslm_config Shape = { 1, 64, 1, 0 };
	const size_t BlockBytes = sslm_kv_block_size(Bench.Mapping);
	const size_t Overhead = sslm_kv_pool_overhead_size(Bench.Mapping, 2);
	const size_t WorkspaceBytes = sslm_workspace_size(Bench.Mapping, &Shape);
	Bench.StateBytes = sslm_seq_state_size(Bench.Mapping);
	if (!TestTrue(TEXT("positive bounded ABI sizes"), BlockBytes > 0 && BlockBytes <= SIZE_MAX / 2
		&& Overhead > 0 && Overhead < SIZE_MAX - BlockBytes * 2 && WorkspaceBytes > 0 && Bench.StateBytes > 0)) { return false; }
	const size_t KvBytes = BlockBytes * 2 + Overhead;
	Bench.KvMemory = FMemory::Malloc(KvBytes, SSLM_ABI_ALIGNMENT_BYTES);
	Bench.WorkspaceMemory = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
	Bench.SaveMemory = FMemory::Malloc(Bench.StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
	if (!TestTrue(TEXT("benchmark buffers allocated"), Bench.KvMemory && Bench.WorkspaceMemory && Bench.SaveMemory)) { return false; }
	if (!TestEqual(TEXT("legacy pool create"), int32(sslm_kv_pool_create(Bench.Mapping, Bench.KvMemory, KvBytes, 2, &Bench.Pool)), int32(SSLM_OK))
		|| !TestEqual(TEXT("workspace create"), int32(sslm_workspace_create(Bench.Mapping, &Shape, Bench.WorkspaceMemory, WorkspaceBytes, &Bench.Workspace)), int32(SSLM_OK))) { return false; }
	FSuperSLMResolvedCpuConfig Defaults;
	SuperSLMRuntimeResolve::ApplyModeProfile(false, Defaults);
	const int32 Tasks = Defaults.ParallelTasks;
	if (SuperSLMFinishHook::ShouldInstall(Tasks))
	{
		const sslm_parallel_for Hook = SuperSLMFinishHook::Make(Tasks, 0);
		if (!TestEqual(TEXT("legacy default finish hook"), int32(sslm_workspace_set_parallel_for(Bench.Workspace, &Hook)), int32(SSLM_OK))) { return false; }
	}
	if (!TestEqual(TEXT("legacy sequence create"), int32(sslm_seq_create(Bench.Mapping, &Bench.Pool, &Bench.Sequence)), int32(SSLM_OK))) { return false; }
	TArray<int32> Prompt;
	Prompt.Init(0, 1000); // the stock calibration's in-vocabulary setup token
	for (int32 Done = 0; Done < Prompt.Num();)
	{
		int32 Consumed = 0;
		const sslm_status Status = sslm_prefill(Bench.Mapping, Bench.Sequence, Prompt.GetData() + Done,
			Prompt.Num() - Done, 64, SSLM_SPAN_PROMPT, Bench.Workspace, &Consumed);
		if (!TestEqual(TEXT("depth-1000 setup prefill"), int32(Status), int32(SSLM_OK))
			|| !TestTrue(TEXT("setup makes bounded progress"), Consumed > 0 && Consumed <= Prompt.Num() - Done)) { return false; }
		Done += Consumed;
	}
	if (!Bench.CheckDepth(*this)) { return false; }
	TArray<double> WarmUp;
	if (!Bench.RestorePopulation(*this, WarmUp)) { return false; }
	TArray<double> WarmAdopt, WarmBegin;
	if (!Bench.LifecyclePopulation(*this, WarmAdopt, WarmBegin) || !Bench.RestoreSeed(*this)) { return false; }
	FSuperSLMCalibrationTestAccess::ClearOverrides();
	struct FOverridesGuard { ~FOverridesGuard() { FSuperSLMCalibrationTestAccess::ClearOverrides(); } } Overrides;
	TArray<double> Samples;
	TArray<double> AdoptSamples, BeginSamples;
	FSuperSLMCalibrationTestAccess::SetSyntheticWorkload([&](FString& OutError)
	{
		const bool bOk = Bench.RestorePopulation(*this, Samples) && Bench.LifecyclePopulation(*this, AdoptSamples, BeginSamples);
		if (!bOk) { OutError = TEXT("legacy lifecycle population failed"); }
		return bOk;
	});
	AddExpectedErrorPlain(TEXT("SuperSLM.CalibrateCosts: REFUSED to write"), EAutomationExpectedErrorFlags::Contains, -1);
	const FString Command = FString::Printf(TEXT("SuperSLM.CalibrateCosts \"%s\" 0"), *Artifact);
	const bool bInvoked = IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, nullptr);
	const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
	if (!TestTrue(TEXT("real restore workload completes inside the stock sampler"), bInvoked && Report.bWorkloadSucceeded
		&& Report.bSyntheticWorkload && !Report.bSamplerDisabled && Report.Samples.Num() > 0)
		|| !TestEqual(TEXT("full restore population"), Samples.Num(), 50)
		|| !TestEqual(TEXT("full adopt population"), AdoptSamples.Num(), 50)
		|| !TestEqual(TEXT("full prefix begin population"), BeginSamples.Num(), 50)) { return false; }
	Samples.Sort();
	AdoptSamples.Sort();
	BeginSamples.Sort();
	AddInfo(FString::Printf(TEXT("K0R legacy restore: depth=1000 mode=whole_reserve block_bytes=%llu warmup=50 samples=50 median_ms=%.6f shipped_RestoreCostMs=%.6f FinishParallelTasks=%d decode_mode=0 idleness_verdict=%d foreign_ceiling_percent=%.6f; report only, no settings written"),
		uint64(BlockBytes), Samples[Samples.Num() / 2], Defaults.RestoreCostMs, Tasks, int32(Report.Verdict), Report.CeilingPercent));
	AddInfo(FString::Printf(TEXT("K0R legacy lifecycle: adopt_prefix_depth=1 prefix_begin_depth=0 warmup=50 samples=50 adopt_median_ms=%.9f shipped_AdoptCostMs=%.6f prefix_begin_median_ms=%.9f shipped_PrefixBeginCostMs=%.6f timer_ms_per_cycle=%.9f; zero readings are unresolved by the timer, report only, no settings written"),
		AdoptSamples[25], Defaults.AdoptCostMs, BeginSamples[25], Defaults.PrefixBeginCostMs, FPlatformTime::GetSecondsPerCycle64() * 1000.0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

