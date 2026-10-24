// Decode-threading plan rev 1.1, D4: plugin setting, CPU/GPU hook installation,
// real decode work, and the R-S1h scope restatement.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "../../../SuperSLMUnreal/Private/SuperSLMFinishHook.h" // signature check only; no private symbol is linked
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMParallelDecodeTestAccess.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "Trace/Detail/Channel.h"
#include "Trace/Trace.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

#include <type_traits>
#include <utility>

using namespace SuperSLML2S1Fixtures;

namespace
{
	template <typename T, typename = void>
	struct THasParallelDecodeSetting : std::false_type {};
	template <typename T>
	struct THasParallelDecodeSetting<T, std::void_t<decltype(std::declval<T&>().bParallelDecode)>> : std::true_type {};

	// T-3102 (D-SLM8146): the mode is set and read only through the shared accessors, which work on
	// the plain bool and on D4's TOptional<bool>.
	template <typename T>
	void SetParallelDecode(T& Config, bool bEnabled)
	{
		SuperSLMConfigAccess::SetMode(Config, bEnabled);
	}

	template <typename T>
	bool IsParallelDecodeOn(const T& Config)
	{
		return SuperSLMConfigAccess::GetMode(Config);
	}

	template <typename T, typename = void>
	struct THasBitsMake : std::false_type {};
	template <typename T>
	struct THasBitsMake<T, std::void_t<decltype(SuperSLMFinishHook::Make(4, std::declval<T>()))>> : std::true_type {};

	FSuperSLMRuntimeConfig CpuConfig(bool bParallelDecode)
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 8;
		Config.MaxLayerBudget = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		Config.BlockCount = 2;
		Config.MaxConcurrentCalls = 2;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		SuperSLMConfigAccess::SetTasks(Config, 4);
		SetParallelDecode(Config, bParallelDecode);
		return Config;
	}

	bool ImportAExForD4(FAutomationTestBase& T, USuperSLMModel*& OutModel)
	{
		FString Path, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(Path, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diagnostic;
		OutModel = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		return T.TestNotNull(TEXT("A-EX imports"), OutModel)
			&& T.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted);
	}

	struct FDecodeArm
	{
		int32 Makes = 0;
		int32 ZeroBitsMakes = 0;
		int32 MatvecBitsMakes = 0;
		uint32 Bits = 0;
		int32 HookRuns = 0;
		int32 TokenFinishes = 0;
		int32 DecodeJobs = 0;
		int32 PrefillJobs = 0;
		int32 OneTokenPrefillJobs = 0;
		TArray<int32> Tokens;
	};

	struct FHookProbeSession
	{
		FHookProbeSession() { FSuperSLMParallelDecodeTestAccess::ResetHookObservations(); }
		~FHookProbeSession() { FSuperSLMParallelDecodeTestAccess::StopHookObservations(); }
	};

	bool RunCpuArm(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model,
		bool bParallelDecode, FDecodeArm& Out)
	{
		FHookProbeSession Probe;
		const FSuperSLMRuntimeConfig Config = CpuConfig(bParallelDecode);
		const FSuperSLMConfigureReport Report = Cpu.Configure(&Model, Config);
		if (!T.TestEqual(*FString::Printf(TEXT("CPU Configure (%s): %s"), bParallelDecode ? TEXT("on") : TEXT("off"), *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		Out.Makes = FSuperSLMParallelDecodeTestAccess::GetMakeCount();
		Out.ZeroBitsMakes = FSuperSLMParallelDecodeTestAccess::GetZeroBitsMakeCount();
		Out.MatvecBitsMakes = FSuperSLMParallelDecodeTestAccess::GetMatvecBitsMakeCount();
		Out.Bits = FSuperSLMParallelDecodeTestAccess::GetLastMadeBits();

		FSuperSLMSequence Seq;
		if (!T.TestEqual(TEXT("CPU VendSequence"), (uint8)Cpu.VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = { 1, 2, 3, 4, 5, 6, 7, 8 };
		Request.MaxNewTokens = 8;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		FString Error;
		const bool bCompleted = RunGenerationToCompletion(Cpu, Seq, Request, Out.Tokens, 120.0, Error);
		Out.HookRuns = FSuperSLMParallelDecodeTestAccess::GetRunCount();
		for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
		{
			if (Job.DeliveredAtTick < 0 || Job.bSkippedContinuation) { continue; }
			Out.TokenFinishes += Job.TokenFinishes;
			Out.DecodeJobs += Job.DecodeLayers > 0 && Job.PromptTokens == 0 ? 1 : 0;
			Out.PrefillJobs += Job.PromptTokens > 0 ? 1 : 0;
			Out.OneTokenPrefillJobs += Job.PromptTokens == 1 ? 1 : 0;
		}
		Cpu.ReturnSequence(Seq);
		return T.TestTrue(*FString::Printf(TEXT("CPU generation completed (%s)"), *Error), bCompleted);
	}

	struct FParallelTaskReading
	{
		bool bAnswered = false;
		FString Error;
		int32 ProbeEvents = 0;
		int32 PluginEvents = 0;
		int32 OnTaskThreads = 0;
	};

	FParallelTaskReading ReadParallelTaskCapture(const FString& Path, bool bSuperSLMChannelOn)
	{
		using namespace TraceServices;
		FParallelTaskReading R;
		ITraceServicesModule& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
		const TSharedPtr<IAnalysisService> Service = Module.GetAnalysisService();
		if (!Service.IsValid()) { R.Error = TEXT("TraceServices analysis service absent"); return R; }
		const TSharedPtr<const IAnalysisSession> Session = Service->StartAnalysis(*Path);
		if (!Session.IsValid()) { R.Error = TEXT("StartAnalysis returned no session"); return R; }
		const double Deadline = FPlatformTime::Seconds() + 60.0;
		while (!Session->IsAnalysisComplete() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.05f);
		}
		if (!Session->IsAnalysisComplete()) { R.Error = TEXT("trace analysis timed out"); return R; }
		FAnalysisSessionReadScope ReadScope(*Session);
		const ITimingProfilerProvider* Timing = ReadTimingProfilerProvider(*Session);
		const IThreadProvider* Threads = Session->ReadProvider<IThreadProvider>(GetThreadProviderName());
		if (!Timing || !Threads || Session->GetDurationSeconds() <= 0.0)
		{
			R.Error = TEXT("trace has no readable timing/thread data");
			return R;
		}
		TMap<uint32, FString> TimerNames;
		TMap<uint32, FString> ThreadNames;
		TSet<uint32> PlanThreads;
		TArray<uint32> PluginEventThreads;
		{
			const ITimingProfilerTimerReader& Timers = Timing->GetTimerReader();
			for (uint32 Id = 0; Id < Timers.GetTimerCount(); ++Id)
			{
				const FTimingProfilerTimer* Timer = Timers.GetTimer(Id);
				if (Timer && Timer->Name)
				{
					const FString Name(Timer->Name);
					if (Name == TEXT("D4.TraceProbe") || Name == TEXT("SuperSLM.ParallelTask")
						|| Name == TEXT("SuperSLM.Tick.Plan"))
					{
						TimerNames.Add(Id, Name);
					}
				}
			}
		}
		Threads->EnumerateThreads([&](const FThreadInfo& Thread)
		{
			ThreadNames.Add(Thread.Id, Thread.Name ? FString(Thread.Name) : FString());
			uint32 TimelineIndex = 0;
			if (!Timing->GetCpuThreadTimelineIndex(Thread.Id, TimelineIndex)) { return; }
			TArray<uint32> Stack;
			Timing->ReadTimeline(TimelineIndex, [&](const ITimingProfilerTimeline& Timeline)
			{
				Timeline.EnumerateEvents(0.0, Session->GetDurationSeconds(),
					[&](double, double, uint32 Depth, const FTimingProfilerEvent& Event)
					{
						const int32 D = static_cast<int32>(Depth);
						Stack.SetNum(D + 1, EAllowShrinking::No);
						Stack[D] = Event.TimerIndex;
						const FString* Name = TimerNames.Find(Event.TimerIndex);
						if (Name && *Name == TEXT("D4.TraceProbe")) { ++R.ProbeEvents; }
						if (Name && *Name == TEXT("SuperSLM.Tick.Plan")) { PlanThreads.Add(Thread.Id); }
						if (Name && *Name == TEXT("SuperSLM.ParallelTask") && D > 0)
						{
							const FString* Parent = TimerNames.Find(Stack[D - 1]);
							if (Parent && *Parent == TEXT("SuperSLM.ParallelTask"))
							{
								++R.PluginEvents; // the inner scope, not ParallelFor's same-named executor
								PluginEventThreads.Add(Thread.Id);
							}
						}
						return EEventEnumerate::Continue;
					});
			});
		});
		if (bSuperSLMChannelOn && PlanThreads.Num() != 1)
		{
			R.Error = FString::Printf(TEXT("Tick.Plan identifies %d game threads, expected one"), PlanThreads.Num());
			return R;
		}
		if (bSuperSLMChannelOn)
		{
			const uint32 GameThread = *PlanThreads.CreateConstIterator();
			for (const uint32 ThreadId : PluginEventThreads)
			{
				const FString* Name = ThreadNames.Find(ThreadId);
				if (ThreadId != GameThread && Name && !Name->IsEmpty()
					&& !Name->StartsWith(TEXT("SuperSLM CPU Worker ")))
				{
					++R.OnTaskThreads;
				}
			}
		}
		R.bAnswered = true;
		return R;
	}

	bool CaptureParallelTaskArm(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model,
		bool bSuperSLMChannelOn, FParallelTaskReading& Out)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("SuperSLMD4Scratch"), bSuperSLMChannelOn ? TEXT("parallel_task_on.utrace") : TEXT("parallel_task_off.utrace")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		IFileManager::Get().Delete(*Path, false, true);
		UE::Trace::FChannel* PriorCpuChannel = UE::Trace::FindChannel(TEXT("Cpu"));
		UE::Trace::FChannel* SslmChannel = UE::Trace::FindChannel(TEXT("SuperSLM"));
		UE::Trace::FChannel* MemChannel = UE::Trace::FindChannel(TEXT("MemAlloc"));
		const bool bPriorCpu = PriorCpuChannel && PriorCpuChannel->IsEnabled();
		const bool bPriorSslm = SslmChannel && SslmChannel->IsEnabled();
		const bool bPriorMem = MemChannel && MemChannel->IsEnabled();
		UE::Trace::ToggleChannel(TEXT("Cpu"), true);
		UE::Trace::ToggleChannel(TEXT("SuperSLM"), bSuperSLMChannelOn);
		UE::Trace::ToggleChannel(TEXT("MemAlloc"), false);
		auto RestoreChannels = [&]()
		{
			UE::Trace::ToggleChannel(TEXT("Cpu"), bPriorCpu);
			UE::Trace::ToggleChannel(TEXT("SuperSLM"), bPriorSslm);
			UE::Trace::ToggleChannel(TEXT("MemAlloc"), bPriorMem);
		};
		FTraceAuxiliary::FOptions Options;
		Options.bTruncateFile = true;
		Options.bExcludeTail = true;
		const bool bStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *Path,
			bSuperSLMChannelOn ? TEXT("cpu,SuperSLM") : TEXT("cpu"), &Options);
		if (!T.TestTrue(TEXT("D4 trace starts"), bStarted)) { RestoreChannels(); return false; }
		for (int32 I = 0; I < 8; ++I)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("D4.TraceProbe");
		}
		FDecodeArm Arm;
		const bool bRunOk = RunCpuArm(T, Cpu, Model, true, Arm);
		TRACE_CPUPROFILER_EVENT_FLUSH();
		FTraceAuxiliary::Stop();
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (UE::Trace::IsTracing() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		RestoreChannels();
		if (!T.TestFalse(TEXT("D4 trace closes before analysis"), UE::Trace::IsTracing()) || !bRunOk)
		{
			return false;
		}
		if (!T.TestTrue(TEXT("precondition: the capture ran token-finish hook work"), Arm.HookRuns > 0 && Arm.TokenFinishes > 0))
		{
			return false;
		}
		Out = ReadParallelTaskCapture(Path, bSuperSLMChannelOn);
		T.AddInfo(FString::Printf(TEXT("D4 trace %s: %s; probe=%d nested ParallelTask=%d task-thread=%d"),
			*Path, *Out.Error, Out.ProbeEvents, Out.PluginEvents, Out.OnTaskThreads));
		return T.TestTrue(*FString::Printf(TEXT("D4 trace readable: %s"), *Out.Error), Out.bAnswered);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMParallelDecodeSettingAndMakeTest,
	"SuperSLM.D4.SettingAndMake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMParallelDecodeSettingAndMakeTest::RunTest(const FString& Parameters)
{
	const FSuperSLMRuntimeConfig Default;
	bool bOk = TestTrue(TEXT("CPU FSuperSLMRuntimeConfig declares bParallelDecode"),
		THasParallelDecodeSetting<FSuperSLMRuntimeConfig>::value);
	// The fixture oracle only; the runtime's own resolved fallback is asserted, and compared with this
	// oracle, by SuperSLM.D4.Settings.ModeAppliesAtNextConfigure's shipped-mode arm.
	bOk &= TestFalse(TEXT("the expected-resolution oracle reads an unset bParallelDecode as off"), IsParallelDecodeOn(Default));
	bOk &= TestTrue(TEXT("SuperSLMFinishHook::Make accepts Tasks and uint32 Bits"), THasBitsMake<uint32>::value);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMParallelDecodeCpuInstallAndRunTest,
	"SuperSLM.D4.CpuInstallAndDecodeRuns", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMParallelDecodeCpuInstallAndRunTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (!ImportAExForD4(*this, Model)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }

	FDecodeArm Off, On;
	if (!RunCpuArm(*this, *Cpu, *Model, false, Off) || !RunCpuArm(*this, *Cpu, *Model, true, On))
	{
		return false;
	}
	const int32 OffMatvecRuns = Off.HookRuns - Off.TokenFinishes;
	const int32 OnMatvecRuns = On.HookRuns - On.TokenFinishes;
	AddInfo(FString::Printf(TEXT("D4 CPU: off makes=%d zero=%d matvec=%d bits=%u runs=%d finishes=%d decode jobs=%d prefill jobs=%d one-token prefill=%d; on makes=%d zero=%d matvec=%d bits=%u runs=%d finishes=%d decode jobs=%d prefill jobs=%d one-token prefill=%d"),
		Off.Makes, Off.ZeroBitsMakes, Off.MatvecBitsMakes, Off.Bits, Off.HookRuns, Off.TokenFinishes, Off.DecodeJobs, Off.PrefillJobs, Off.OneTokenPrefillJobs,
		On.Makes, On.ZeroBitsMakes, On.MatvecBitsMakes, On.Bits, On.HookRuns, On.TokenFinishes, On.DecodeJobs, On.PrefillJobs, On.OneTokenPrefillJobs));
	bool bOk = TestEqual(TEXT("the two CPU worker lanes each install a hook with the setting off"), Off.Makes, 2);
	bOk &= TestEqual(TEXT("the two CPU worker lanes each install a hook with the setting on"), On.Makes, 2);
	bOk &= TestEqual(TEXT("CPU Make reserved is zero with bParallelDecode off"), Off.Bits, (uint32)0);
	bOk &= TestEqual(TEXT("CPU Make reserved is SSLM_PARALLEL_FOR_MATVEC with bParallelDecode on"), On.Bits, (uint32)SSLM_PARALLEL_FOR_MATVEC);
	bOk &= TestEqual(TEXT("both CPU lanes make zero-bit hooks with bParallelDecode off"), Off.ZeroBitsMakes, 2);
	bOk &= TestEqual(TEXT("both CPU lanes make matvec-bit hooks with bParallelDecode on"), On.MatvecBitsMakes, 2);
	bOk &= TestTrue(TEXT("precondition: both arms delivered decode jobs"), Off.DecodeJobs > 0 && On.DecodeJobs > 0);
	bOk &= TestTrue(TEXT("precondition: both arms delivered a prefill job"), Off.PrefillJobs > 0 && On.PrefillJobs > 0);
	bOk &= TestEqual(TEXT("precondition: off arm has no one-token prefill job that could thread"), Off.OneTokenPrefillJobs, 0);
	bOk &= TestEqual(TEXT("precondition: on arm has no one-token prefill job that could thread"), On.OneTokenPrefillJobs, 0);
	bOk &= TestTrue(TEXT("precondition: both arms called the finish hook"), Off.TokenFinishes > 0 && On.TokenFinishes > 0);
	bOk &= TestEqual(TEXT("with the setting off, hook runs minus delivered token finishes is zero matvec runs"), OffMatvecRuns, 0);
	bOk &= TestTrue(TEXT("with the setting on, hook runs minus delivered token finishes is positive matvec runs"), OnMatvecRuns > 0);
	bOk &= TestEqual(TEXT("threading preserves generated tokens"), On.Tokens, Off.Tokens);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMParallelDecodeGpuInstallTest,
	"SuperSLM.D4.GpuInstallAndConfigure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMParallelDecodeGpuInstallTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (!ImportAExForD4(*this, Model)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu) || !TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }

	bool bOk = true;
	for (const bool bParallelDecode : { false, true })
	{
		const FSuperSLMRuntimeConfig CpuCfg = CpuConfig(bParallelDecode);
		bOk &= TestEqual(TEXT("requested CPU decode setting is represented in the config"),
			IsParallelDecodeOn(CpuCfg), bParallelDecode);
		const FSuperSLMConfigureReport CpuReport = Cpu->Configure(Model, CpuCfg);
		if (!TestEqual(TEXT("CPU Configure before GPU Configure"), (uint8)CpuReport.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		FSuperSLMGpuRuntimeConfig GpuCfg;
		GpuCfg.ContextCap = 4096;
		GpuCfg.BlockCount = 1;
		GpuCfg.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(SuperSLML2S2Fixtures::AExNumHiddenLayers);
		GpuCfg.K = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		GpuCfg.TickBudgetMs = 1000.0;
		GpuCfg.FinishParallelTasks = 4;
		FHookProbeSession Probe;
		const FSuperSLMGpuConfigureReport GpuReport = Gpu->Configure(Model, GpuCfg);
		bOk &= TestEqual(*FString::Printf(TEXT("GPU Configure succeeds with CPU setting %s: %s"),
			bParallelDecode ? TEXT("on") : TEXT("off"), *GpuReport.Message),
			(uint8)GpuReport.Result, (uint8)ESuperSLMGpuConfigureResult::Success);
		bOk &= TestEqual(TEXT("GPU Configure constructs one hook"), FSuperSLMParallelDecodeTestAccess::GetMakeCount(), 1);
		bOk &= TestEqual(TEXT("GPU hook uses zero bits (M12)"), FSuperSLMParallelDecodeTestAccess::GetZeroBitsMakeCount(), 1);
		bOk &= TestEqual(TEXT("GPU install passes zero bits, independent of CPU bParallelDecode (M12)"),
			FSuperSLMParallelDecodeTestAccess::GetLastMadeBits(), (uint32)0);
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMParallelDecodeParallelTaskScopeTest,
	"SuperSLM.D4.ParallelTaskScope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMParallelDecodeParallelTaskScopeTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (!ImportAExForD4(*this, Model)) { return false; }
	USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
	FParallelTaskReading On, Off;
	if (!CaptureParallelTaskArm(*this, *Cpu, *Model, true, On)
		|| !CaptureParallelTaskArm(*this, *Cpu, *Model, false, Off))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the captured CPU config has bParallelDecode on"), IsParallelDecodeOn(CpuConfig(true)));
	bOk &= TestEqual(TEXT("channel-on reader sees eight own CPU probes"), On.ProbeEvents, 8);
	bOk &= TestEqual(TEXT("channel-off reader remains live and sees eight own CPU probes"), Off.ProbeEvents, 8);
	bOk &= TestTrue(TEXT("SuperSLM.ParallelTask has a plugin-owned nested scope with bParallelDecode on"), On.PluginEvents > 0);
	bOk &= TestTrue(TEXT("SuperSLM.ParallelTask executes on an engine task thread"), On.OnTaskThreads > 0);
	bOk &= TestEqual(TEXT("SuperSLM.ParallelTask plugin scope is absent with SuperSLMChannel off"), Off.PluginEvents, 0);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
