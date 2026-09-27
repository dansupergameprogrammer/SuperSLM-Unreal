// Plan §10.3.1 item 5b: six commissioning constructions for the calibration idleness check.
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

namespace
{
	const FString OutputPath = FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini");
	// The build lock the command checks: SUPERSLM_BUILD_LOCK, the lock directory a build wrapper holds
	// (SuperSLMCalibrateCostsCommand.cpp states the contract). The lock cells need it set; unset, Read() fails and names the variable.
	FString LockPath() { return FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_BUILD_LOCK")).TrimStartAndEnd(); }
	FString OwnerPath() { return FPaths::Combine(LockPath(), TEXT("owner.txt")); }
	const TCHAR* const CostKeys[] = { TEXT("LayerCostMs"), TEXT("LayerCostPerPositionMs"),
		TEXT("PromptTokenCostMs"), TEXT("PromptTokenCostPerPositionMs"), TEXT("FinishCostMs"),
		TEXT("ResetCostMs"), TEXT("AdoptCostMs"), TEXT("SaveCostMs"), TEXT("RestoreCostMs"),
		TEXT("PrefixBeginCostMs"), TEXT("PrefixReleaseCostMs") };

	class FCalibrationLog : public FOutputDevice
	{
	public:
		TArray<FString> Lines;
		FCalibrationLog() { GLog->AddOutputDevice(this); }
		~FCalibrationLog() { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category == FName(TEXT("LogSuperSLM"))) { Lines.Add(V); }
		}
	};

	const FString* FindLog(const FCalibrationLog& Log, const FString& Needle)
	{
		for (const FString& Line : Log.Lines) { if (Line.Contains(Needle)) { return &Line; } }
		return nullptr;
	}

	bool ReadLogNumber(const FString& Line, const FString& Prefix, double& Out)
	{
		const int32 At = Line.Find(Prefix);
		if (At == INDEX_NONE) { return false; }
		const TCHAR* Number = *Line + At + Prefix.Len();
		if (!FChar::IsDigit(*Number) && *Number != '-' && *Number != '+') { return false; }
		Out = FCString::Atod(Number);
		return FMath::IsFinite(Out);
	}

	bool CheckValueLog(FAutomationTestBase& Test, const FCalibrationLog& Log, bool bRefused)
	{
		const FString* Line = FindLog(Log, bRefused ? TEXT("SuperSLM.CalibrateCosts: values [not decision-bearing:")
			: TEXT("SuperSLM.CalibrateCosts: values: "));
		if (!Test.TestNotNull(TEXT("all measured values logged before verdict"), Line)) { return false; }
		bool bOk = true;
		for (const TCHAR* Key : CostKeys)
		{
			double Value = 0.0;
			bOk &= Test.TestTrue(*FString::Printf(TEXT("logged %s is finite and has the required sign"), Key),
				ReadLogNumber(*Line, FString(Key) + TEXT("="), Value) &&
				(FString(Key).Contains(TEXT("PerPosition")) ? Value >= 0.0 : Value > 0.0));
		}
		const FString* Verdict = FindLog(Log, TEXT("SuperSLM.CalibrateCosts: idleness evidence --"));
		if (Test.TestNotNull(TEXT("idleness verdict logged"), Verdict))
		{
			const int32 ValueIndex = Log.Lines.IndexOfByKey(*Line);
			const int32 VerdictIndex = Log.Lines.IndexOfByKey(*Verdict);
			bOk &= Test.TestTrue(TEXT("medians precede the idleness verdict"), ValueIndex >= 0 && ValueIndex < VerdictIndex);
		}
		else { bOk = false; }
		return bOk;
	}

	bool CheckDepthLogs(FAutomationTestBase& Test, const FCalibrationLog& Log, int64 Cap,
		bool bRequireTrend, bool bRefused)
	{
		const int64 Depths[] = { 1, Cap / 4, Cap / 2, 3 * Cap / 4, Cap - 1 };
		const FString* CapLine = FindLog(Log, TEXT("SuperSLM.CalibrateCosts: cap C = "));
		bool bOk = Test.TestNotNull(TEXT("cap and depths logged"), CapLine);
		if (CapLine)
		{
			const FString Expected = FString::Printf(TEXT("cap C = %lld; depths %lld, %lld, %lld, %lld, %lld."),
				Cap, Depths[0], Depths[1], Depths[2], Depths[3], Depths[4]);
			bOk &= Test.TestTrue(TEXT("logged depths follow cap rule"), CapLine->Contains(Expected));
		}
		for (const TCHAR* Arm : { TEXT("layer"), TEXT("prompt") })
		{
			double First = 0.0, Last = 0.0;
			for (int32 I = 0; I < 5; ++I)
			{
				const FString Prefix = FString::Printf(TEXT("%s arm timed at depth %lld (restored context_length %lld): median "),
					Arm, Depths[I], Depths[I]);
				int32 Count = 0;
				double Median = 0.0;
				for (const FString& Line : Log.Lines)
				{
					if (Line.Contains(Prefix) && ReadLogNumber(Line, Prefix, Median))
					{
						++Count;
						if (bRefused)
						{
							bOk &= Test.TestTrue(TEXT("refused median is labelled not decision-bearing"),
								Line.Contains(TEXT("[not decision-bearing:")));
						}
					}
				}
				bOk &= Test.TestTrue(*FString::Printf(TEXT("%s depth %lld logged once with restored context and positive median"), Arm, Depths[I]),
					Count == 1 && Median > 0.0);
				if (I == 0) { First = Median; }
				if (I == 4) { Last = Median; }
			}
			if (bRequireTrend)
			{
				bOk &= Test.TestTrue(*FString::Printf(TEXT("%s deepest median exceeds depth-1 median"), Arm), Last > First);
			}
		}
		return bOk;
	}

	struct FOverridesGuard
	{
		~FOverridesGuard() { FSuperSLMCalibrationTestAccess::ClearOverrides(); }
	};

	// pwsh is an installed tool. Its script starts two native threads, writes a readiness marker,
	// and stays alive until this owner terminates it. No helper executable is shipped.
	struct FForeignLoad
	{
		HANDLE Process = nullptr;
		FString ScriptPath, ReadyPath;
		double StartWall = 0.0, StartCpu = 0.0, ActiveWall = 0.0, ActiveCpu = 0.0;
		bool bMeasured = false;

		~FForeignLoad()
		{
			Stop();
			if (!ScriptPath.IsEmpty()) { IFileManager::Get().Delete(*ScriptPath); }
			if (!ReadyPath.IsEmpty()) { IFileManager::Get().Delete(*ReadyPath); }
		}

		static bool CpuSeconds(HANDLE Handle, double& Out)
		{
			FILETIME Created, Exited, Kernel, User;
			if (Handle == nullptr || ::GetProcessTimes(Handle, &Created, &Exited, &Kernel, &User) == 0) { return false; }
			ULARGE_INTEGER K, U;
			K.LowPart = Kernel.dwLowDateTime; K.HighPart = Kernel.dwHighDateTime;
			U.LowPart = User.dwLowDateTime; U.HighPart = User.dwHighDateTime;
			Out = static_cast<double>(K.QuadPart + U.QuadPart) / 1.0e7;
			return true;
		}

		bool Start(FString& Error)
		{
			if (Process != nullptr) { Error = TEXT("load already running"); return false; }
			const FString Stem = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ScriptPath = FPaths::ProjectSavedDir() / (TEXT("SuperSLM/calibration-load-") + Stem + TEXT(".ps1"));
			ReadyPath = FPaths::ProjectSavedDir() / (TEXT("SuperSLM/calibration-load-") + Stem + TEXT(".ready"));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ScriptPath), true);
			const FString Script = TEXT(
				"$source = @'\n"
				"using System;\nusing System.Threading;\n"
				"public static class SuperSlmCalibrationLoad {\n"
				"  public static volatile int Sink = 1;\n"
				"  public static void Run(string ready) {\n"
				"    for (int i = 0; i < 2; ++i) {\n"
				"      Thread t = new Thread(() => { while (true) { Sink = unchecked(Sink * 1664525 + 1013904223); } });\n"
				"      t.IsBackground = true; t.Start();\n"
				"    }\n"
				"    System.IO.File.WriteAllText(ready, \"ready\");\n"
				"    Thread.Sleep(Timeout.Infinite);\n"
				"  }\n}\n'@\n"
				"Add-Type -TypeDefinition $source\n"
				"[SuperSlmCalibrationLoad]::Run($args[0])\n");
			if (!FFileHelper::SaveStringToFile(Script, *ScriptPath)) { Error = TEXT("could not write temporary pwsh script"); return false; }
			const FString Command = FString::Printf(TEXT("pwsh.exe -NoProfile -NonInteractive -File \"%s\" \"%s\""), *ScriptPath, *ReadyPath);
			TArray<TCHAR> Mutable;
			Mutable.Append(*Command, Command.Len() + 1);
			STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup);
			PROCESS_INFORMATION Child{};
			if (::CreateProcessW(nullptr, Mutable.GetData(), nullptr, nullptr, false, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Child) == 0)
			{
				Error = FString::Printf(TEXT("CreateProcessW(pwsh) failed: %lu"), ::GetLastError());
				return false;
			}
			::CloseHandle(Child.hThread);
			Process = Child.hProcess;
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (!IFileManager::Get().FileExists(*ReadyPath) && FPlatformTime::Seconds() < Deadline)
			{
				if (::WaitForSingleObject(Process, 0) != WAIT_TIMEOUT) { Error = TEXT("pwsh exited before readiness"); return false; }
				FPlatformProcess::Sleep(0.02f);
			}
			if (!IFileManager::Get().FileExists(*ReadyPath)) { Error = TEXT("pwsh readiness timed out"); return false; }
			if (::WaitForSingleObject(Process, 0) != WAIT_TIMEOUT) { Error = TEXT("pwsh exited after readiness"); return false; }
			if (!CpuSeconds(Process, StartCpu)) { Error = TEXT("GetProcessTimes failed at load start"); return false; }
			StartWall = FPlatformTime::Seconds();
			return true;
		}

		void Stop()
		{
			if (Process == nullptr) { return; }
			const double EndWall = FPlatformTime::Seconds();
			double EndCpu = 0.0;
			bMeasured = StartWall > 0.0 && CpuSeconds(Process, EndCpu);
			if (bMeasured) { ActiveWall = EndWall - StartWall; ActiveCpu = EndCpu - StartCpu; }
			::TerminateProcess(Process, 0);
			::WaitForSingleObject(Process, 5000);
			::CloseHandle(Process);
			Process = nullptr;
		}

		bool IsValid() const { return bMeasured && ActiveWall >= 2.0 && ActiveCpu >= 1.5 * ActiveWall; }
	};

	bool Invoke(FAutomationTestBase& Test, const FString& Artifact)
	{
		const FString Command = FString::Printf(TEXT("SuperSLM.CalibrateCosts \"%s\""), *Artifact);
		return Test.TestTrue(TEXT("calibration command registered"),
			IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, nullptr));
	}

	bool UnchangedFile(FAutomationTestBase& Test, bool bExisted, const TArray<uint8>& Before)
	{
		const bool bExistsAfter = IFileManager::Get().FileExists(*OutputPath);
		bool bOk = Test.TestEqual(TEXT("calibration file existence unchanged"), bExistsAfter, bExisted);
		if (bExisted && bExistsAfter)
		{
			TArray<uint8> After;
			bOk &= Test.TestTrue(TEXT("calibration file readable after refusal"), FFileHelper::LoadFileToArray(After, *OutputPath));
			bOk &= Test.TestEqual(TEXT("calibration bytes unchanged"), After, Before);
		}
		return bOk;
	}

	enum class EConstruction { Pre, ThroughReturn, Interior };

	bool RunRefusal(FAutomationTestBase& Test, EConstruction Kind)
	{
		FForeignLoad Load; // kills the child on every exit path, including a failed assertion
		FOverridesGuard Overrides;
		const bool bExisted = IFileManager::Get().FileExists(*OutputPath);
		TArray<uint8> Before;
		if (bExisted && !Test.TestTrue(TEXT("existing calibration readable"), FFileHelper::LoadFileToArray(Before, *OutputPath))) { return false; }
		FString LoadError;
		if (Kind == EConstruction::Pre && !Load.Start(LoadError)) { Test.AddError(LoadError); return false; }
		bool bStartedInWorkload = true;
		FSuperSLMCalibrationTestAccess::SetPhaseHook([&](ESuperSLMCalibrationEvent Event)
		{
			if (Kind == EConstruction::Pre && Event == ESuperSLMCalibrationEvent::WorkloadStart) { Load.Stop(); }
		});
		FSuperSLMCalibrationTestAccess::SetSyntheticWorkload([&](FString& Error)
		{
			if (Kind == EConstruction::Pre) { FPlatformProcess::Sleep(7.0f); return true; }
			FPlatformProcess::Sleep(1.25f); // at least 1 s after workload-start
			bStartedInWorkload = Load.Start(Error);
			if (!bStartedInWorkload) { LoadError = Error; return false; }
			if (Kind == EConstruction::Interior)
			{
				FPlatformProcess::Sleep(3.0f);
				Load.Stop();
				FPlatformProcess::Sleep(2.5f); // clean tail before workload-end
			}
			else { FPlatformProcess::Sleep(4.0f); }
			return true;
		});
		Test.AddExpectedError(TEXT("REFUSED to write"), EAutomationExpectedErrorFlags::Contains, 1);
		FCalibrationLog Log;
		const bool bFound = Invoke(Test, TEXT("synthetic.sslm"));
		Load.Stop(); // ThroughReturn remains active through the post-window and verdict
		bool bOk = bFound;
		bOk &= Test.TestTrue(TEXT("foreign load started"), bStartedInWorkload);
		if (!bStartedInWorkload) { Test.AddError(LoadError); }
		Test.AddInfo(FString::Printf(TEXT("foreign load: %.3f CPU-s / %.3f active wall-s = %.3f logical processors"),
			Load.ActiveCpu, Load.ActiveWall, Load.ActiveWall > 0.0 ? Load.ActiveCpu / Load.ActiveWall : 0.0));
		bOk &= Test.TestTrue(TEXT("load active at least 2 s and at least 1.5 logical-processor-seconds per second; void otherwise"), Load.IsValid());
		const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
		bOk &= Test.TestTrue(TEXT("synthetic workload succeeded"), Report.bSyntheticWorkload && Report.bWorkloadSucceeded);
		bOk &= Test.TestEqual(TEXT("command refuses"), (uint8)SuperSLMCalibrateCostsCommand::GetLastOutcome(),
			(uint8)SuperSLMCalibrateCostsCommand::EOutcome::RefusedNotIdle);
		bOk &= Test.TestEqual(TEXT("foreign load caused refusal"), (uint8)Report.Verdict, (uint8)ESuperSLMIdlenessVerdict::RefusedForeignLoad);
		bOk &= Test.TestTrue(TEXT("first refusing window exists"), Report.FirstRefusingWindow >= 0);
		if (Report.FirstRefusingWindow >= 0)
		{
			const ESuperSLMCalibrationPhase Expected = Kind == EConstruction::Pre ? ESuperSLMCalibrationPhase::PreWindow : ESuperSLMCalibrationPhase::Workload;
			bOk &= Test.TestEqual(TEXT("first refusal begins in target phase"), (uint8)Report.FirstRefusingWindowFirstPhase, (uint8)Expected);
			if (Kind != EConstruction::ThroughReturn)
			{
				bOk &= Test.TestEqual(TEXT("first refusal ends in target phase"), (uint8)Report.FirstRefusingWindowLastPhase, (uint8)Expected);
			}
		}
		if (Kind == EConstruction::Interior)
		{
			int32 LastWorkload = INDEX_NONE;
			for (int32 I = 0; I < Report.Samples.Num(); ++I)
			{
				if (Report.Samples[I].Phase == ESuperSLMCalibrationPhase::Workload) { LastWorkload = I; }
			}
			bOk &= Test.TestTrue(TEXT("workload has a final window"), LastWorkload >= 3);
			if (LastWorkload >= 3)
			{
				double Tail = 0.0;
				for (int32 I = LastWorkload - 3; I <= LastWorkload; ++I) { Tail += Report.Samples[I].ForeignSharePercent; }
				bOk &= Test.TestTrue(TEXT("last workload window is clean"), Tail / 4.0 <= Report.CeilingPercent);
			}
			if (Report.Samples.Num() >= 4)
			{
				double PostTail = 0.0;
				const int32 Last = Report.Samples.Num() - 1;
				for (int32 I = Last - 3; I <= Last; ++I)
				{
					bOk &= Test.TestEqual(TEXT("post-window tail phase"), (uint8)Report.Samples[I].Phase,
						(uint8)ESuperSLMCalibrationPhase::PostWindow);
					PostTail += Report.Samples[I].ForeignSharePercent;
				}
				bOk &= Test.TestTrue(TEXT("post-window tail is clean"), PostTail / 4.0 <= Report.CeilingPercent);
			}
		}
		if (Kind == EConstruction::Pre)
		{
			const FString* StartLine = FindLog(Log, TEXT("SuperSLM.CalibrateCosts: workload-start at "));
			double WorkloadStart = 0.0;
			bOk &= Test.TestTrue(TEXT("workload-start time logged"), StartLine &&
				ReadLogNumber(*StartLine, TEXT("workload-start at "), WorkloadStart));
			if (StartLine)
			{
				for (int32 I = 0; I + 3 < Report.Samples.Num(); ++I)
				{
					if (Report.Samples[I].EndSeconds < WorkloadStart + 2.0) { continue; }
					double Mean = 0.0;
					for (int32 J = I; J < I + 4; ++J) { Mean += Report.Samples[J].ForeignSharePercent / 4.0; }
					bOk &= Test.TestTrue(TEXT("no refusal starts at least 2 s after workload-start"), Mean <= Report.CeilingPercent);
				}
			}
			const int32 N = Report.Samples.Num();
			bOk &= Test.TestTrue(TEXT("post-window has a last four-sample window"), N >= 4);
			if (N >= 4)
			{
				double Mean = 0.0;
				for (int32 I = N - 4; I < N; ++I)
				{
					bOk &= Test.TestEqual(TEXT("last window is post-window"), (uint8)Report.Samples[I].Phase,
						(uint8)ESuperSLMCalibrationPhase::PostWindow);
					Mean += Report.Samples[I].ForeignSharePercent / 4.0;
				}
				bOk &= Test.TestTrue(TEXT("last post-window is clean"), Mean <= Report.CeilingPercent);
			}
		}
		bOk &= UnchangedFile(Test, bExisted, Before);
		return bOk;
	}

	struct FRestoreFile
	{
		bool bArmed = false;
		bool bExisted = false;
		TArray<uint8> Before;
		~FRestoreFile()
		{
			if (!bArmed) { return; }
			if (bExisted) { FFileHelper::SaveArrayToFile(Before, *OutputPath); }
			else { IFileManager::Get().Delete(*OutputPath); }
		}
	};

	// The build wrapper owns the directory. A cell may change only its marker and must restore the
	// original bytes even when an assertion or file operation fails partway through the workload.
	struct FOwnerMarkerGuard
	{
		FAutomationTestBase& Test;
		TArray<uint8> Original;
		bool bArmed = false;

		explicit FOwnerMarkerGuard(FAutomationTestBase& InTest) : Test(InTest) {}
		~FOwnerMarkerGuard()
		{
			if (bArmed && !Restore()) { Test.AddError(TEXT("owner.txt could not be restored on cell exit")); }
		}

		bool Read()
		{
			if (!Test.TestTrue(TEXT("SUPERSLM_BUILD_LOCK names the build lock"), !LockPath().IsEmpty())) { return false; }
			if (!Test.TestTrue(TEXT("build lock directory exists"), IFileManager::Get().DirectoryExists(*LockPath()))) { return false; }
			if (!Test.TestTrue(TEXT("owner.txt exists"), IFileManager::Get().FileExists(*OwnerPath()))) { return false; }
			if (!Test.TestTrue(TEXT("owner.txt readable"), FFileHelper::LoadFileToArray(Original, *OwnerPath()))) { return false; }
			return true;
		}

		bool Restore()
		{
			if (!bArmed) { return true; }
			if (!FFileHelper::SaveArrayToFile(Original, *OwnerPath())) { return false; }
			TArray<uint8> After;
			if (!FFileHelper::LoadFileToArray(After, *OwnerPath()) || After != Original) { return false; }
			bArmed = false;
			return true;
		}
	};

	enum class ELockConstruction { DifferentToken, MissingMarker };

	bool RunLockRefusal(FAutomationTestBase& Test, ELockConstruction Kind)
	{
		FOwnerMarkerGuard Marker(Test);
		if (!Marker.Read()) { return false; }
		FString OriginalLine;
		for (uint8 Byte : Marker.Original)
		{
			if (Byte == '\r' || Byte == '\n') { break; }
			OriginalLine.AppendChar(static_cast<TCHAR>(Byte));
		}
		const FString InheritedToken = FPlatformMisc::GetEnvironmentVariable(TEXT("SSU_BUILD_LOCK_TOKEN")).TrimStartAndEnd();
		if (!Test.TestTrue(TEXT("inherited build lock token is present"), !InheritedToken.IsEmpty()) ||
			!Test.TestTrue(TEXT("owner.txt begins with the inherited token"), OriginalLine.Equals(InheritedToken, ESearchCase::IgnoreCase)))
		{
			return false;
		}
		TArray<uint8> DifferentOwner;
		if (Kind == ELockConstruction::DifferentToken)
		{
			FString DifferentGuid;
			do { DifferentGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits); }
			while (DifferentGuid.Equals(InheritedToken, ESearchCase::IgnoreCase));
			for (TCHAR Character : DifferentGuid) { DifferentOwner.Add(static_cast<uint8>(Character)); }
			int32 Remainder = 0;
			while (Remainder < Marker.Original.Num() && Marker.Original[Remainder] != '\r' && Marker.Original[Remainder] != '\n') { ++Remainder; }
			DifferentOwner.Append(Marker.Original.GetData() + Remainder, Marker.Original.Num() - Remainder);
		}

		FOverridesGuard Overrides;
		const bool bExisted = IFileManager::Get().FileExists(*OutputPath);
		TArray<uint8> Before;
		if (bExisted && !Test.TestTrue(TEXT("existing calibration readable"), FFileHelper::LoadFileToArray(Before, *OutputPath))) { return false; }
		bool bSwapped = false;
		bool bRestored = false;
		double SwapStart = 0.0, SwapEnd = 0.0;
		FSuperSLMCalibrationTestAccess::SetSyntheticWorkload([&](FString& Error)
		{
			FPlatformProcess::Sleep(1.25f); // at least 1 s after workload-start
			SwapStart = FPlatformTime::Seconds();
			Marker.bArmed = true;
			bSwapped = Kind == ELockConstruction::DifferentToken
				? FFileHelper::SaveArrayToFile(DifferentOwner, *OwnerPath())
				: IFileManager::Get().Delete(*OwnerPath());
			if (!bSwapped) { Error = TEXT("could not plant foreign owner marker"); return false; }
			FPlatformProcess::Sleep(2.5f);
			bRestored = Marker.Restore();
			SwapEnd = FPlatformTime::Seconds();
			if (!bRestored) { Error = TEXT("could not restore owner marker"); return false; }
			FPlatformProcess::Sleep(1.0f); // own-lock samples after the swap
			return true;
		});
		Test.AddExpectedError(TEXT("REFUSED to write"), EAutomationExpectedErrorFlags::Contains, 1);
		const bool bFound = Invoke(Test, TEXT("synthetic.sslm"));
		bool bOk = bFound;
		bOk &= Test.TestTrue(TEXT("foreign owner marker planted"), bSwapped);
		bOk &= Test.TestTrue(TEXT("owner marker restored inside workload"), bRestored);
		bOk &= Test.TestTrue(TEXT("foreign marker held for at least 2 s"), bSwapped && SwapEnd - SwapStart >= 2.0);
		bOk &= Test.TestTrue(TEXT("owner.txt restored byte for byte"), Marker.Restore());
		const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
		bOk &= Test.TestTrue(TEXT("synthetic workload succeeded"), Report.bSyntheticWorkload && Report.bWorkloadSucceeded);
		bOk &= Test.TestTrue(TEXT("run inherited build lock token"), Report.bHadLockToken);
		bOk &= Test.TestEqual(TEXT("command refuses"), (uint8)SuperSLMCalibrateCostsCommand::GetLastOutcome(),
			(uint8)SuperSLMCalibrateCostsCommand::EOutcome::RefusedNotIdle);
		bOk &= Test.TestEqual(TEXT("foreign lock caused refusal"), (uint8)Report.Verdict,
			(uint8)ESuperSLMIdlenessVerdict::RefusedForeignLock);
		bOk &= Test.TestEqual(TEXT("CPU did not cause refusal"), Report.FirstRefusingWindow, -1);
		bOk &= Test.TestTrue(TEXT("first foreign-lock sample exists"), Report.FirstForeignLockSample >= 0);
		int32 FirstForeign = INDEX_NONE, LastForeign = INDEX_NONE;
		for (int32 I = 0; I < Report.Samples.Num(); ++I)
		{
			const FSuperSLMIdlenessSample& Sample = Report.Samples[I];
			if (Sample.LockState == ESuperSLMBuildLockState::Foreign)
			{
				if (FirstForeign == INDEX_NONE) { FirstForeign = I; }
				LastForeign = I;
				bOk &= Test.TestEqual(TEXT("foreign sample is inside workload swap"), (uint8)Sample.Phase,
					(uint8)ESuperSLMCalibrationPhase::Workload);
				bOk &= Test.TestEqual(TEXT("foreign sample has planted reason"), Sample.LockForeignReason,
					Kind == ELockConstruction::DifferentToken ? FString(TEXT("owner.txt names a different token")) : FString(TEXT("the lock has no owner.txt")));
			}
			else
			{
				bOk &= Test.TestEqual(TEXT("outside swap the lock is own"), (uint8)Sample.LockState,
					(uint8)ESuperSLMBuildLockState::Own);
				bOk &= Test.TestTrue(TEXT("own sample has no foreign reason"), Sample.LockForeignReason.IsEmpty());
			}
		}
		bOk &= Test.TestEqual(TEXT("first foreign sample is the refusal sample"), Report.FirstForeignLockSample, FirstForeign);
		bOk &= Test.TestTrue(TEXT("own samples bracket the foreign swap"), FirstForeign > 0 && LastForeign > FirstForeign && LastForeign < Report.Samples.Num() - 1);
		for (int32 I = FirstForeign; I <= LastForeign; ++I)
		{
			bOk &= Test.TestEqual(TEXT("foreign samples are contiguous within the swap"), (uint8)Report.Samples[I].LockState,
				(uint8)ESuperSLMBuildLockState::Foreign);
		}
		bOk &= UnchangedFile(Test, bExisted, Before);
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationHealthyMustAcceptTest,
	"SuperSLM.U1.Calibration.HealthyMustAccept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationHealthyMustAcceptTest::RunTest(const FString& Parameters)
{
	FString Artifact, Reason;
	if (!TestTrue(TEXT("A-EX present for real calibration"), TryGetAExArtifactPath(Artifact, Reason))) { return false; }
	FOverridesGuard Overrides;
	FRestoreFile Restore;
	Restore.bExisted = IFileManager::Get().FileExists(*OutputPath);
	if (Restore.bExisted && !TestTrue(TEXT("existing calibration readable"), FFileHelper::LoadFileToArray(Restore.Before, *OutputPath))) { return false; }
	Restore.bArmed = true;
	FCalibrationLog Log;
	bool bOk = Invoke(*this, Artifact);
	int32 WarmUpCount = 0;
	int32 WarmUpIndex = INDEX_NONE;
	int32 FirstSampleIndex = INDEX_NONE;
	bool bTimedArmBeforeWarmUp = false;
	for (int32 I = 0; I < Log.Lines.Num(); ++I)
	{
		const FString& Line = Log.Lines[I];
		if (Line.StartsWith(TEXT("SuperSLM.CalibrateCosts: warm-up pass:")))
		{
			++WarmUpCount;
			if (WarmUpIndex == INDEX_NONE) { WarmUpIndex = I; }
		}
		if (Line.StartsWith(TEXT("SuperSLM.CalibrateCosts: sample ")))
		{
			if (FirstSampleIndex == INDEX_NONE) { FirstSampleIndex = I; }
			if (WarmUpIndex == INDEX_NONE) { bTimedArmBeforeWarmUp = true; }
		}
		if ((Line.StartsWith(TEXT("SuperSLM.CalibrateCosts: layer arm timed at depth ")) ||
			Line.StartsWith(TEXT("SuperSLM.CalibrateCosts: prompt arm timed at depth "))) && WarmUpIndex == INDEX_NONE)
		{
			bTimedArmBeforeWarmUp = true;
		}
	}
	bOk &= TestEqual(TEXT("one real warm-up pass logged"), WarmUpCount, 1);
	if (WarmUpIndex != INDEX_NONE)
	{
		double WarmUpCalls = 0.0;
		bOk &= TestTrue(TEXT("warm-up completed calls and discarded their readings"),
			ReadLogNumber(Log.Lines[WarmUpIndex], TEXT("warm-up pass: "), WarmUpCalls) &&
			WarmUpCalls > 0.0 && Log.Lines[WarmUpIndex].Contains(TEXT("discarded")));
	}
	bOk &= TestTrue(TEXT("first sample is the pre-window after warm-up"),
		FirstSampleIndex != INDEX_NONE && WarmUpIndex != INDEX_NONE && WarmUpIndex < FirstSampleIndex &&
		Log.Lines[FirstSampleIndex].Contains(TEXT(", pre-window:")));
	bOk &= TestFalse(TEXT("no timed-arm sample precedes the warm-up"), bTimedArmBeforeWarmUp);
	bOk &= TestEqual(TEXT("healthy run writes medians"), (uint8)SuperSLMCalibrateCostsCommand::GetLastOutcome(),
		(uint8)SuperSLMCalibrateCostsCommand::EOutcome::Written);
	const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
	bOk &= TestEqual(TEXT("healthy run meets criterion"), (uint8)Report.Verdict, (uint8)ESuperSLMIdlenessVerdict::Met);
	bOk &= TestTrue(TEXT("real workload succeeded"), !Report.bSyntheticWorkload && Report.bWorkloadSucceeded);
	bOk &= TestTrue(TEXT("healthy run's build-lock check is configured (SUPERSLM_BUILD_LOCK)"), Report.bLockCheckConfigured);
	bOk &= TestTrue(TEXT("healthy run inherited build lock token"), Report.bHadLockToken);
	bOk &= TestTrue(TEXT("healthy run sampled the own lock"), !Report.Samples.IsEmpty());
	for (const FSuperSLMIdlenessSample& Sample : Report.Samples)
	{
		bOk &= TestEqual(TEXT("healthy sample has own build lock"), (uint8)Sample.LockState, (uint8)ESuperSLMBuildLockState::Own);
		bOk &= TestTrue(TEXT("healthy sample has no foreign lock reason"), Sample.LockForeignReason.IsEmpty());
	}
	bOk &= TestEqual(TEXT("healthy run has no foreign lock sample"), Report.FirstForeignLockSample, -1);
	bOk &= TestTrue(TEXT("workload used own-process CPU"), Report.Samples.ContainsByPredicate([](const FSuperSLMIdlenessSample& S)
	{
		return S.Phase == ESuperSLMCalibrationPhase::Workload && S.OwnSharePercent > 0.0;
	}));
	FString Ini;
	bOk &= TestTrue(TEXT("written calibration readable"), FFileHelper::LoadFileToString(Ini, *OutputPath));
	TArray<FString> Lines;
	Ini.ParseIntoArrayLines(Lines);
	for (const TCHAR* Key : CostKeys)
	{
		const FString Prefix = FString(Key) + TEXT("=");
		int32 Matches = 0;
		double Value = 0.0;
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(Prefix)) { ++Matches; Value = FCString::Atod(*Line.Mid(Prefix.Len())); }
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s occurs once with a finite median of required sign"), Key),
			Matches == 1 && FMath::IsFinite(Value) &&
			(FString(Key).Contains(TEXT("PerPosition")) ? Value >= 0.0 : Value > 0.0));
	}
	bOk &= CheckValueLog(*this, Log, false);
	bOk &= CheckDepthLogs(*this, Log, 4096, true, false);
	const int32 BoxLogicalProcessors = static_cast<int32>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
	bOk &= TestTrue(TEXT("whole-box active processor count is available"), BoxLogicalProcessors > 0);
	bOk &= TestEqual(TEXT("report N equals whole-box active processor count"), Report.LogicalProcessors, BoxLogicalProcessors);
	bOk &= TestTrue(TEXT("depth section names artifact cap"), Lines.Contains(TEXT("CapTokens=4096")));
	for (const TCHAR* Arm : { TEXT("LayerMedianMsAtDepth"), TEXT("PromptTokenMedianMsAtDepth") })
	{
		for (int32 Depth : { 1, 1024, 2048, 3072, 4095 })
		{
			const FString Prefix = FString::Printf(TEXT("%s%d="), Arm, Depth);
			bOk &= TestTrue(*FString::Printf(TEXT("ini carries %s%d"), Arm, Depth),
				Lines.ContainsByPredicate([&](const FString& Line) { return Line.StartsWith(Prefix); }));
		}
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationPreWindowMustRejectTest,
	"SuperSLM.U1.Calibration.PreWindowMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationPreWindowMustRejectTest::RunTest(const FString& Parameters)
{
	return RunRefusal(*this, EConstruction::Pre);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationWorkloadStartMustRejectTest,
	"SuperSLM.U1.Calibration.WorkloadStartMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationWorkloadStartMustRejectTest::RunTest(const FString& Parameters)
{
	return RunRefusal(*this, EConstruction::ThroughReturn);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationWorkloadInteriorMustRejectTest,
	"SuperSLM.U1.Calibration.WorkloadInteriorMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationWorkloadInteriorMustRejectTest::RunTest(const FString& Parameters)
{
	return RunRefusal(*this, EConstruction::Interior);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationForeignOwnerMustRejectTest,
	"SuperSLM.U1.Calibration.ForeignOwnerMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationForeignOwnerMustRejectTest::RunTest(const FString& Parameters)
{
	return RunLockRefusal(*this, ELockConstruction::DifferentToken);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationMissingOwnerMustRejectTest,
	"SuperSLM.U1.Calibration.MissingOwnerMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationMissingOwnerMustRejectTest::RunTest(const FString& Parameters)
{
	return RunLockRefusal(*this, ELockConstruction::MissingMarker);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CalibrationRefusedRealMediansTest,
	"SuperSLM.U1.Calibration.RefusedRealMedians",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CalibrationRefusedRealMediansTest::RunTest(const FString& Parameters)
{
	FString Artifact, Reason;
	if (!TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Artifact, Reason))) { return false; }
	FOwnerMarkerGuard Marker(*this);
	if (!Marker.Read()) { return false; }
	const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("SSU_BUILD_LOCK_TOKEN")).TrimStartAndEnd();
	FString FirstLine;
	for (uint8 Byte : Marker.Original)
	{
		if (Byte == '\r' || Byte == '\n') { break; }
		FirstLine.AppendChar(static_cast<TCHAR>(Byte));
	}
	if (!TestTrue(TEXT("own lock token is present and matches owner.txt"),
		!Token.IsEmpty() && FirstLine.Equals(Token, ESearchCase::IgnoreCase))) { return false; }
	FString ForeignToken;
	do { ForeignToken = FGuid::NewGuid().ToString(EGuidFormats::Digits); }
	while (ForeignToken.Equals(Token, ESearchCase::IgnoreCase));
	TArray<uint8> ForeignBytes;
	for (TCHAR C : ForeignToken) { ForeignBytes.Add(static_cast<uint8>(C)); }
	int32 Tail = 0;
	while (Tail < Marker.Original.Num() && Marker.Original[Tail] != '\r' && Marker.Original[Tail] != '\n') { ++Tail; }
	ForeignBytes.Append(Marker.Original.GetData() + Tail, Marker.Original.Num() - Tail);

	const bool bExisted = IFileManager::Get().FileExists(*OutputPath);
	TArray<uint8> Before;
	if (bExisted && !TestTrue(TEXT("prior calibration readable"), FFileHelper::LoadFileToArray(Before, *OutputPath))) { return false; }
	FOverridesGuard Overrides;
	bool bSwapped = false, bRestored = false;
	FSuperSLMCalibrationTestAccess::SetPhaseHook([&](ESuperSLMCalibrationEvent Event)
	{
		if (Event != ESuperSLMCalibrationEvent::WorkloadStart) { return; }
		FPlatformProcess::Sleep(1.25f);
		Marker.bArmed = true;
		bSwapped = FFileHelper::SaveArrayToFile(ForeignBytes, *OwnerPath());
		if (!bSwapped) { return; }
		FPlatformProcess::Sleep(2.5f);
		bRestored = Marker.Restore();
	});
	AddExpectedError(TEXT("REFUSED to write"), EAutomationExpectedErrorFlags::Contains, 1);
	FCalibrationLog Log;
	const bool bFound = Invoke(*this, Artifact);
	bool bOk = TestTrue(TEXT("calibration command found"), bFound);
	bOk &= TestTrue(TEXT("foreign marker planted during graded workload"), bSwapped);
	bOk &= TestTrue(TEXT("owner marker restored within workload"), bRestored);
	bOk &= TestTrue(TEXT("owner marker restored byte for byte"), Marker.Restore());
	const FSuperSLMCalibrationIdlenessReport& Report = FSuperSLMCalibrationTestAccess::GetLastIdlenessReport();
	bOk &= TestTrue(TEXT("real A-EX workload completed"), !Report.bSyntheticWorkload && Report.bWorkloadSucceeded);
	bOk &= TestEqual(TEXT("foreign owner refuses run"), (uint8)Report.Verdict, (uint8)ESuperSLMIdlenessVerdict::RefusedForeignLock);
	bOk &= TestEqual(TEXT("no CPU refusal is needed"), Report.FirstRefusingWindow, -1);
	bOk &= TestEqual(TEXT("command refuses without writing"), (uint8)SuperSLMCalibrateCostsCommand::GetLastOutcome(),
		(uint8)SuperSLMCalibrateCostsCommand::EOutcome::RefusedNotIdle);
	bOk &= CheckValueLog(*this, Log, true);
	bOk &= CheckDepthLogs(*this, Log, 4096, false, true);
	bOk &= UnchangedFile(*this, bExisted, Before);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
