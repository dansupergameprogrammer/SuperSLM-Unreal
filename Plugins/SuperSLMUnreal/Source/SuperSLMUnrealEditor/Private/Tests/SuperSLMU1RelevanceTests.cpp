// Plan §10.3.1 item 5a.2: interleaved, real-artifact relevance readings for the idleness ceiling.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "SuperSLMCalibrateCostsCommand.h"
#include "SuperSLMCalibrationTestAccess.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

using namespace SuperSLML2S1Fixtures;

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMRelevance, Display, All);

namespace
{
	const FString CalibrationPath = FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini");
	// The typeperf CSVs go under the project's Saved/ folder; each run's begin line names its CSV. The path is
	// absolute because typeperf, a child process, resolves it.
	const FString RelevanceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/Relevance"));

	struct FProcess
	{
		HANDLE Handle = nullptr;
		~FProcess() { Stop(); }

		bool Start(const FString& Command, FString& Error)
		{
			TArray<TCHAR> Mutable;
			Mutable.Append(*Command, Command.Len() + 1);
			STARTUPINFOW Startup{};
			Startup.cb = sizeof(Startup);
			PROCESS_INFORMATION Child{};
			if (::CreateProcessW(nullptr, Mutable.GetData(), nullptr, nullptr, false, CREATE_NO_WINDOW,
				nullptr, nullptr, &Startup, &Child) == 0)
			{
				Error = FString::Printf(TEXT("CreateProcessW failed for %s: %lu"), *Command, ::GetLastError());
				return false;
			}
			::CloseHandle(Child.hThread);
			Handle = Child.hProcess;
			return true;
		}

		bool Running() const { return Handle && ::WaitForSingleObject(Handle, 0) == WAIT_TIMEOUT; }

		void Stop()
		{
			if (!Handle) { return; }
			if (Running()) { ::TerminateProcess(Handle, 0); }
			::WaitForSingleObject(Handle, 5000);
			::CloseHandle(Handle);
			Handle = nullptr;
		}
	};

	struct FTypeperfLogger : FProcess
	{
		bool StartCsv(const FString& Csv, FString& Error)
		{
			if (!Start(FString::Printf(TEXT("typeperf.exe \"\\Processor(_Total)\\%% Processor Time\" -si 1 -o \"%s\" -y"), *Csv), Error))
			{
				return false;
			}
			const double Deadline = FPlatformTime::Seconds() + 15.0;
			while (Running() && FPlatformTime::Seconds() < Deadline)
			{
				if (IFileManager::Get().FileSize(*Csv) > 0) { return true; }
				FPlatformProcess::Sleep(0.05f);
			}
			Error = FString::Printf(TEXT("typeperf did not produce its CSV header: %s"), *Csv);
			return false;
		}
	};

	struct FSingleThreadLoad : FProcess
	{
		FString ScriptPath;
		FString ReadyPath;
		double StartWall = 0.0;
		double StartCpu = 0.0;
		double CpuPerSecond = 0.0;
		bool bMeasured = false;
		bool bExitedEarly = false;

		~FSingleThreadLoad()
		{
			StopMeasured();
			if (!ScriptPath.IsEmpty()) { IFileManager::Get().Delete(*ScriptPath); }
			if (!ReadyPath.IsEmpty()) { IFileManager::Get().Delete(*ReadyPath); }
		}

		static bool CpuSeconds(HANDLE Process, double& Out)
		{
			FILETIME Created, Exited, Kernel, User;
			if (!Process || ::GetProcessTimes(Process, &Created, &Exited, &Kernel, &User) == 0) { return false; }
			ULARGE_INTEGER K, U;
			K.LowPart = Kernel.dwLowDateTime; K.HighPart = Kernel.dwHighDateTime;
			U.LowPart = User.dwLowDateTime; U.HighPart = User.dwHighDateTime;
			Out = static_cast<double>(K.QuadPart + U.QuadPart) / 1.0e7;
			return true;
		}

		bool StartLoad(FString& Error)
		{
			const FString Stem = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ScriptPath = FPaths::ProjectSavedDir() / (TEXT("SuperSLM/relevance-load-") + Stem + TEXT(".ps1"));
			ReadyPath = FPaths::ProjectSavedDir() / (TEXT("SuperSLM/relevance-load-") + Stem + TEXT(".ready"));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ScriptPath), true);
			const FString Script = TEXT(
				"$source = @'\n"
				"using System;\nusing System.Threading;\n"
				"public static class SuperSlmRelevanceLoad {\n"
				"  public static volatile int Sink = 1;\n"
				"  public static void Run(string ready) {\n"
				"    Thread t = new Thread(() => { while (true) { Sink = unchecked(Sink * 1664525 + 1013904223); } });\n"
				"    t.IsBackground = true; t.Start();\n"
				"    System.IO.File.WriteAllText(ready, \"ready\");\n"
				"    Thread.Sleep(Timeout.Infinite);\n"
				"  }\n}\n'@\n"
				"Add-Type -TypeDefinition $source\n"
				"[SuperSlmRelevanceLoad]::Run($args[0])\n");
			if (!FFileHelper::SaveStringToFile(Script, *ScriptPath))
			{
				Error = TEXT("could not write relevance load script");
				return false;
			}
			if (!Start(FString::Printf(TEXT("pwsh.exe -NoProfile -NonInteractive -File \"%s\" \"%s\""),
				*ScriptPath, *ReadyPath), Error)) { return false; }
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (Running() && FPlatformTime::Seconds() < Deadline)
			{
				if (IFileManager::Get().FileExists(*ReadyPath)) { break; }
				FPlatformProcess::Sleep(0.02f);
			}
			if (!Running() || !IFileManager::Get().FileExists(*ReadyPath) || !CpuSeconds(Handle, StartCpu))
			{
				Error = TEXT("relevance load did not reach readiness with a CPU-time handle");
				return false;
			}
			StartWall = FPlatformTime::Seconds();
			return true;
		}

		void StopMeasured()
		{
			if (!Handle) { return; }
			bExitedEarly = !Running();
			double EndCpu = 0.0;
			const double Wall = FPlatformTime::Seconds() - StartWall;
			bMeasured = StartWall > 0.0 && Wall > 0.0 && CpuSeconds(Handle, EndCpu);
			if (bMeasured) { CpuPerSecond = (EndCpu - StartCpu) / Wall; }
			Stop();
		}
	};

	struct FCalibrationFileGuard
	{
		bool bExisted = false;
		bool bArmed = false;
		TArray<uint8> Before;
		~FCalibrationFileGuard() { if (bArmed) { Restore(); } }

		bool Capture()
		{
			bExisted = IFileManager::Get().FileExists(*CalibrationPath);
			if (bExisted && !FFileHelper::LoadFileToArray(Before, *CalibrationPath)) { return false; }
			bArmed = true;
			return true;
		}

		bool Restore()
		{
			if (!bArmed) { return true; }
			const bool bChanged = bExisted
				? FFileHelper::SaveArrayToFile(Before, *CalibrationPath)
				: (!IFileManager::Get().FileExists(*CalibrationPath) || IFileManager::Get().Delete(*CalibrationPath));
			TArray<uint8> After;
			const bool bVerified = bChanged && (bExisted
				? FFileHelper::LoadFileToArray(After, *CalibrationPath) && After == Before
				: !IFileManager::Get().FileExists(*CalibrationPath));
			bArmed = !bVerified;
			return bVerified;
		}
	};

	struct FValueLog : FOutputDevice
	{
		int32 ValueLines = 0;
		FString ValueLine;
		FValueLog() { GLog->AddOutputDevice(this); }
		~FValueLog() { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category == FName(TEXT("LogSuperSLM")) && FString(Message).Contains(TEXT("SuperSLM.CalibrateCosts: values")))
			{
				++ValueLines;
				ValueLine = Message;
			}
		}
	};

	struct FOverridesGuard
	{
		~FOverridesGuard() { FSuperSLMCalibrationTestAccess::ClearOverrides(); }
	};

	const TCHAR* ConditionText(int32 Condition)
	{
		switch (Condition) { case 0: return TEXT("C"); case 1: return TEXT("L1"); case 2: return TEXT("S"); default: return TEXT("R"); }
	}

	bool RunReading(FAutomationTestBase& Test, const FString& Artifact, const TCHAR* Cell,
		int32 Block, int32 Condition, int32 Run, int32 Order)
	{
		FSuperSLMCalibrationTestAccess::ClearOverrides();
		FOverridesGuard Overrides;
		FCalibrationFileGuard File;
		if (!File.Capture()) { Test.AddError(TEXT("prior calibration file could not be read")); return false; }
		const TCHAR* Name = ConditionText(Condition);
		const FString Csv = FString::Printf(TEXT("%s/%s-%s-%d.csv"), *RelevanceDir, Cell, Name, Run);
		IFileManager::Get().MakeDirectory(*RelevanceDir, true);
		FTypeperfLogger Logger;
		FString Error;
		if (!Logger.StartCsv(Csv, Error)) { Test.AddError(Error); return false; }
		FSingleThreadLoad Load;
		bool bLoadStarted = Condition != 1;
		bool bLoadStopped = Condition != 1;
		if (Condition == 1)
		{
			FSuperSLMCalibrationTestAccess::SetPhaseHook([&](ESuperSLMCalibrationEvent Event)
			{
				if (Event == ESuperSLMCalibrationEvent::BeforePreWindow)
				{
					bLoadStarted = Load.StartLoad(Error);
				}
				else if (Event == ESuperSLMCalibrationEvent::AfterPostWindow)
				{
					Load.StopMeasured();
					bLoadStopped = true;
				}
			});
		}
		if (Condition == 2) { FSuperSLMCalibrationTestAccess::SetSamplerDisabled(true); }
		if (Condition == 3) { FSuperSLMCalibrationTestAccess::SetFinishParallelTasksOverride(1); }
		Test.AddExpectedErrorPlain(TEXT("SuperSLM.CalibrateCosts: REFUSED to write"), EAutomationExpectedErrorFlags::Contains, -1);
		UE_LOG(LogSuperSLMRelevance, Display,
			TEXT("SuperSLM.Relevance: begin cell=%s block=%d condition=%s run=%d order=%d csv=%s"),
			Cell, Block, Name, Run, Order, *Csv);
		FValueLog Values;
		const FString Command = FString::Printf(TEXT("SuperSLM.CalibrateCosts \"%s\""), *Artifact);
		const bool bInvoked = IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, nullptr);
		const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
		const SuperSLMCalibrateCostsCommand::EOutcome Outcome = SuperSLMCalibrateCostsCommand::GetLastOutcome();
		if (Condition == 1 && !bLoadStopped) { Load.StopMeasured(); }
		const bool bLoggerSurvived = Logger.Running();
		Logger.Stop();
		const bool bVoid = Condition == 1 && (!Load.bMeasured || Load.CpuPerSecond < 0.75);
		const FString LoadText = Condition == 1 && Load.bMeasured
			? FString::Printf(TEXT("%.6f"), Load.CpuPerSecond) : TEXT("-");
		UE_LOG(LogSuperSLMRelevance, Display,
			TEXT("SuperSLM.Relevance: end cell=%s block=%d condition=%s run=%d void=%d load_lp_per_s=%s"),
			Cell, Block, Name, Run, bVoid ? 1 : 0, *LoadText);
		bool bOk = bInvoked && bLoggerSurvived && Report.bWorkloadSucceeded && !Report.bSyntheticWorkload &&
			Values.ValueLines == 1 && (Condition == 2
				? Outcome == SuperSLMCalibrateCostsCommand::EOutcome::NotRun
				: Outcome == SuperSLMCalibrateCostsCommand::EOutcome::Written ||
					Outcome == SuperSLMCalibrateCostsCommand::EOutcome::RefusedNotIdle);
		for (const TCHAR* Key : { TEXT("LayerCostMs="), TEXT("LayerCostPerPositionMs="),
			TEXT("PromptTokenCostMs="), TEXT("PromptTokenCostPerPositionMs="), TEXT("FinishCostMs="),
			TEXT("ResetCostMs="), TEXT("AdoptCostMs="), TEXT("SaveCostMs="), TEXT("RestoreCostMs="),
			TEXT("PrefixBeginCostMs="), TEXT("PrefixReleaseCostMs=") })
		{
			bOk &= Values.ValueLine.Contains(Key);
		}
		bOk &= Values.ValueLine.Contains(TEXT(" FinishParallelTasks="));
		if (Condition == 3) { bOk &= Values.ValueLine.EndsWith(TEXT(" FinishParallelTasks=1")); }
		if (!bOk) { Test.AddError(TEXT("calibration command did not complete one real measurement with eleven values and the expected FinishParallelTasks setting")); }
		if (Condition == 1 && (!bLoadStarted || !bLoadStopped || Load.bExitedEarly || !Load.bMeasured))
		{
			Test.AddError(Error.IsEmpty() ? TEXT("L1 load failed during the graded span") : Error);
			bOk = false;
		}
		if (!File.Restore()) { Test.AddError(TEXT("calibration ini could not be restored byte for byte")); bOk = false; }
		return bOk;
	}

	bool RunBlock(FAutomationTestBase& Test, int32 Block, const TCHAR* Cell)
	{
		FString Artifact, Reason;
		if (!TryGetAExArtifactPath(Artifact, Reason)) { Test.AddError(Reason); return false; }
		bool bOk = true;
		for (int32 Order = 1; Order <= 3; ++Order)
		{
			const int32 Condition = (Block - 1 + Order - 1) % 3;
			bOk &= RunReading(Test, Artifact, Cell, Block, Condition, Block, Order);
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceBlock1Test,
	"SuperSLM.U1.Relevance.Block1", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceBlock1Test::RunTest(const FString& Parameters)
{
	return RunBlock(*this, 1, TEXT("SuperSLM.U1.Relevance.Block1"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceBlock2Test,
	"SuperSLM.U1.Relevance.Block2", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceBlock2Test::RunTest(const FString& Parameters)
{
	return RunBlock(*this, 2, TEXT("SuperSLM.U1.Relevance.Block2"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceBlock3Test,
	"SuperSLM.U1.Relevance.Block3", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceBlock3Test::RunTest(const FString& Parameters)
{
	return RunBlock(*this, 3, TEXT("SuperSLM.U1.Relevance.Block3"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceBlock4Test,
	"SuperSLM.U1.Relevance.Block4", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceBlock4Test::RunTest(const FString& Parameters)
{
	return RunBlock(*this, 4, TEXT("SuperSLM.U1.Relevance.Block4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceBlock5Test,
	"SuperSLM.U1.Relevance.Block5", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceBlock5Test::RunTest(const FString& Parameters)
{
	return RunBlock(*this, 5, TEXT("SuperSLM.U1.Relevance.Block5"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1RelevanceRTest,
	"SuperSLM.U1.Relevance.R", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1RelevanceRTest::RunTest(const FString& Parameters)
{
	FString Artifact, Reason;
	if (!TryGetAExArtifactPath(Artifact, Reason)) { AddError(Reason); return false; }
	bool bOk = true;
	for (int32 Run = 1; Run <= 3; ++Run)
	{
		bOk &= RunReading(*this, Artifact, TEXT("SuperSLM.U1.Relevance.R"), 5, 3, Run, 3 + Run);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
