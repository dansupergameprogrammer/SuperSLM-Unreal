// T-3102 (plan §10.3.1 item 5b; D-SLM8071/D-SLM8074, D-SLM8109, D-SLM8144, D-SLM8148, D-SLM8166):
// D4's CPU decode settings and its measured default cost profiles.
//  - SuperSLM.SetDecodeMode 0|1 [--save] and SuperSLM.SetFinishParallelTasks <0..256> [--save]
//    select the next Configure(); an active configuration keeps its installed mode and task count;
//    --save persists to Saved/SuperSLM/RuntimeSettings.ini, which a fresh process loads before its
//    first Configure(); a selection made after BeginConfigure() applies to the next configuration.
//  - Each field resolves on its own (D-SLM8109): the caller's value if set (an explicit value equal
//    to a default included); else this process's selection; else the saved file; else 0/off and 4.
//  - The effective mode is bParallelDecode && FinishParallelTasks > 1 (D-SLM8166); profiles follow it.
//  - Each cost field: the caller's if set, else a matching saved estimate, else the effective
//    mode's one-time profile (Fixtures/D4/d4_one_time_cost_profiles.json).
// Mode is observed through the finish-hook probe and real decode work, task count through whether
// a hook is installed at all, prices through the scheduler's own job ledger, and the resolution
// through the Configure() line and the report's resolved configuration.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

#include "Fixtures/SuperSLMD4Fixtures.h"
#include "CoreGlobals.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Tests/AutomationCommon.h"

// The hold seam for BeginConfigure()'s pool job (D-SLM8165). Until it exists the late-selection arm
// fails by name, never by a compile error.
#if __has_include("SuperSLMConfigureTestAccess.h")
#include "SuperSLMConfigureTestAccess.h"
#define SUPERSLM_D4_CONFIGURE_HOLD_SEAM 1
#else
#define SUPERSLM_D4_CONFIGURE_HOLD_SEAM 0
#endif

namespace SuperSLMD4SettingsTests
{
	using namespace SuperSLMD4Fixtures;

	const TCHAR* const SentinelText = TEXT("; T-3102 sentinel: a caller configuration must leave these bytes\n[SuperSLM.T3102Sentinel]\nValue=1\n");

	struct FProcessSettingsReset
	{
		FProcessSettingsReset() { ResetProcessDecodeSettings(); }
		~FProcessSettingsReset() { ResetProcessDecodeSettings(); }
	};

	// No saved settings and no saved estimate for the cell (both restored after it), and the
	// process settings at their shipped defaults on entry and exit.
	struct FCleanSettings
	{
		FFileBytesGuard Settings{ RuntimeSettingsPath() };
		FFileBytesGuard Costs{ CalibratedCostsPath() };
		FProcessSettingsReset Process;
		bool Arm() { return Settings.Capture() && Costs.Capture() && Settings.Remove() && Costs.Remove(); }
	};

	struct FCpuFixture
	{
		FTestWorldWrapper World;
		USuperSLMModel* Model = nullptr;
		USuperSLMSubsystem* Cpu = nullptr;

		bool Init(FAutomationTestBase& T)
		{
			if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
			if (!ImportAEx(T, Model)) { return false; }
			Cpu = SuperSLML2S1Fixtures::GetSubsystem(World.GetTestWorld());
			return T.TestNotNull(TEXT("CPU subsystem"), Cpu);
		}
	};

	// The runtime-settings section: the INI section carrying a bParallelDecode key.
	bool ReadSavedSettings(FAutomationTestBase& T, TMap<FString, FString>& OutKeys)
	{
		FString Text;
		if (!T.TestTrue(TEXT("RuntimeSettings.ini exists and is readable"), FFileHelper::LoadFileToString(Text, *RuntimeSettingsPath()))) { return false; }
		TArray<FString> Lines;
		const TArray<FIniSection> Sections = ParseIniSections(Text, Lines);
		const TArray<const FIniSection*> Found = SectionsWithKey(Sections, TEXT("bParallelDecode"));
		if (!T.TestEqual(TEXT("RuntimeSettings.ini holds one settings section"), Found.Num(), 1)) { return false; }
		OutKeys = Found[0]->Keys;
		return true;
	}

	bool CheckSaved(FAutomationTestBase& T, const TCHAR* Mode, const TCHAR* Tasks, const TCHAR* Label)
	{
		TMap<FString, FString> Keys;
		if (!ReadSavedSettings(T, Keys)) { return false; }
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: saved bParallelDecode"), Label), Keys.FindRef(TEXT("bParallelDecode")), FString(Mode));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: saved FinishParallelTasks"), Label), Keys.FindRef(TEXT("FinishParallelTasks")), FString(Tasks));
		return bOk;
	}

	bool BytesUnchanged(FAutomationTestBase& T, const FString& Path, bool bExisted, const TArray<uint8>& Before, const TCHAR* What)
	{
		bool bExists = false;
		TArray<uint8> After;
		ReadBytes(Path, bExists, After);
		return T.TestTrue(*FString::Printf(TEXT("%s byte-identical"), What), bExists == bExisted && (!bExists || After == Before));
	}

	// The restart pair's handoff: the prepare cell keeps the prior RuntimeSettings.ini bytes here,
	// and the observe cell (a later editor process) restores them.
	FString BackupPath() { return ScratchDir() / TEXT("t3102-RuntimeSettings.before"); }
	FString AbsentMarkerPath() { return ScratchDir() / TEXT("t3102-RuntimeSettings.absent"); }

	bool BackUpForRestart(FAutomationTestBase& T)
	{
		if (!T.TestFalse(*FString::Printf(TEXT("no earlier restart pair is unfinished (restore %s or delete %s first)"), *BackupPath(), *AbsentMarkerPath()),
			IFileManager::Get().FileExists(*BackupPath()) || IFileManager::Get().FileExists(*AbsentMarkerPath())))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*ScratchDir(), true);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*RuntimeSettingsPath(), false);
		if (IFileManager::Get().FileExists(*RuntimeSettingsPath()))
		{
			return T.TestTrue(TEXT("prior RuntimeSettings.ini backed up"), IFileManager::Get().Copy(*BackupPath(), *RuntimeSettingsPath()) == COPY_OK)
				&& T.TestTrue(TEXT("prior RuntimeSettings.ini cleared"), IFileManager::Get().Delete(*RuntimeSettingsPath()));
		}
		return T.TestTrue(TEXT("prior RuntimeSettings.ini absence recorded"), FFileHelper::SaveStringToFile(TEXT("absent"), *AbsentMarkerPath()));
	}

	struct FRestartRestore
	{
		~FRestartRestore()
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*RuntimeSettingsPath(), false);
			if (IFileManager::Get().FileExists(*BackupPath()))
			{
				IFileManager::Get().Copy(*RuntimeSettingsPath(), *BackupPath());
				IFileManager::Get().Delete(*BackupPath());
			}
			else if (IFileManager::Get().FileExists(*AbsentMarkerPath()))
			{
				IFileManager::Get().Delete(*RuntimeSettingsPath());
				IFileManager::Get().Delete(*AbsentMarkerPath());
			}
		}
	};

	// The observe half runs in a later editor process than the prepare half: the settings file was
	// written before this process started, so only a load from disk can carry it into Configure().
	bool CheckWrittenBeforeThisProcess(FAutomationTestBase& T)
	{
		if (!T.TestTrue(TEXT("the prepare cell ran first (its backup or absence marker exists)"),
			IFileManager::Get().FileExists(*BackupPath()) || IFileManager::Get().FileExists(*AbsentMarkerPath())))
		{
			return false;
		}
		const FDateTime ProcessStart = FDateTime::UtcNow() - FTimespan::FromSeconds(FPlatformTime::Seconds() - GStartTime);
		const FDateTime Written = IFileManager::Get().GetTimeStamp(*RuntimeSettingsPath());
		T.AddInfo(FString::Printf(TEXT("RuntimeSettings.ini written %s UTC; this process started %s UTC"), *Written.ToIso8601(), *ProcessStart.ToIso8601()));
		return T.TestTrue(TEXT("RuntimeSettings.ini was written by an earlier process"), Written != FDateTime::MinValue() && Written < ProcessStart);
	}

	// Configures with no caller cost, drives every job kind, and requires every price to come from
	// Expected and the Configure() line to read (Mode, Tasks, Profile, CallerFields).
	bool ConfigureAndPrice(FAutomationTestBase& T, FCpuFixture& F, const FSuperSLMRuntimeConfig& Config, int32 Mode, int32 Tasks,
		const TCHAR* Profile, int32 CallerFields, const FCostProfile& Expected, const TCHAR* Label, FSuperSLMConfigureReport* OutReport = nullptr)
	{
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		if (!ConfigureObserved(T, *F.Cpu, *F.Model, Config, Hooks, Label, OutReport)) { return false; }
		bool bOk = CheckConfigureLine(T, Log, Mode, Tasks, Profile, CallerFields, Label);
		TArray<FSuperSLMWorkerJobReport> Rows;
		bOk &= DriveEveryJobKind(T, *F.Cpu, *F.Model, Rows, Label);
		bOk &= CheckRowsPricedBy(T, Rows, Expected, Label);
		return bOk;
	}

	// SetDecodeMode selects the NEXT Configure(): a generation already running, and the configuration
	// it runs under, keep the installed mode; without --save no settings byte is written; a selection
	// made while BeginConfigure()'s pool job is held applies to the next configuration, not that one.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4SettingsModeNextConfigureTest,
		"SuperSLM.D4.Settings.ModeAppliesAtNextConfigure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4SettingsModeNextConfigureTest::RunTest(const FString& Parameters)
	{
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("shipped mode"))) { return false; }
		bool bOk = TestEqual(TEXT("shipped mode: both lanes install zero-bit hooks"), Hooks.ZeroBits, 2);
		bOk &= CheckConfigureLine(*this, Log, 0, 4, TEXT("mode-default"), 0, TEXT("shipped mode"));

		// Switch while a generation is running: it keeps its installed mode to the end.
		{
			FHookProbe Probe;
			const int64 LedgerBefore = F.Cpu->GetJobLedgerAppendedCount();
			FSuperSLMSequence Seq;
			bOk &= TestEqual(TEXT("VendSequence"), (uint8)F.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success);
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = { 1, 2, 3, 4, 5, 6, 7, 8 };
			Request.MaxNewTokens = 6;
			Request.SpanKind = ESuperSLMSpanKind::Prompt;
			FString Error;
			bOk &= TestTrue(*FString::Printf(TEXT("generation begins (%s)"), *Error), F.Cpu->BeginGeneration(Seq, Request, Error));
			F.Cpu->Tick(1.0f / 60.0f);
			bOk &= RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1"));
			auto Done = [&]() { const ESuperSLMSequencePhase P = F.Cpu->GetPhase(Seq); return (P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted) && F.Cpu->GetPendingLifecycleOperationCount(Seq) == 0; };
			SuperSLML2S1Fixtures::DrainTicks(*F.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, Done, 120.0);
			bOk &= TestEqual(TEXT("the running generation completes"), (uint8)F.Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete);
			int32 Finishes = 0;
			const TArray<FSuperSLMWorkerJobReport> Ledger = F.Cpu->GetJobLedger();
			const int64 FirstRow = F.Cpu->GetJobLedgerAppendedCount() - Ledger.Num();
			for (int32 I = 0; I < Ledger.Num(); ++I)
			{
				if (FirstRow + I >= LedgerBefore && Ledger[I].DeliveredAtTick >= 0 && !Ledger[I].bSkippedContinuation) { Finishes += Ledger[I].TokenFinishes; }
			}
			F.Cpu->ReturnSequence(Seq);
			bOk &= TestTrue(TEXT("precondition: the running generation finished tokens"), Finishes > 0);
			bOk &= TestEqual(TEXT("a generation running across SetDecodeMode 1 makes no matvec hook run"),
				FSuperSLMParallelDecodeTestAccess::GetRunCount() - Finishes, 0);
			bOk &= TestEqual(TEXT("SetDecodeMode installs nothing until the next Configure()"), FSuperSLMParallelDecodeTestAccess::GetMakeCount(), 0);
		}
		int32 Matvec = -1;
		bOk &= MatvecRunsOfOneGeneration(*this, *F.Cpu, Matvec);
		bOk &= TestEqual(TEXT("a later generation under the same configuration still runs mode 0"), Matvec, 0);

		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("after SetDecodeMode 1"))) { return false; }
		bOk &= TestEqual(TEXT("the next Configure() installs SSLM_PARALLEL_FOR_MATVEC on both lanes"), Hooks.MatvecBits, 2);
		bOk &= CheckConfigureLine(*this, Log, 1, 4, TEXT("mode-default"), 0, TEXT("after SetDecodeMode 1"));
		bOk &= MatvecRunsOfOneGeneration(*this, *F.Cpu, Matvec);
		bOk &= TestTrue(TEXT("mode 1 decode runs projections through the hook"), Matvec > 0);
		bOk &= TestFalse(TEXT("SetDecodeMode without --save writes no RuntimeSettings.ini"), IFileManager::Get().FileExists(*RuntimeSettingsPath()));

		bOk &= RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0"));
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("after SetDecodeMode 0"))) { return false; }
		bOk &= TestEqual(TEXT("SetDecodeMode 0 returns both lanes to zero-bit hooks"), Hooks.ZeroBits, 2);

		// Late selection (D-SLM8144): resolution happens at the BeginConfigure() call, on the game
		// thread, so SetDecodeMode 1 issued while the pool job is held does not reach it.
#if SUPERSLM_D4_CONFIGURE_HOLD_SEAM
		{
			FSuperSLMLogCapture LateLog;
			FHookReading Late;
			FSuperSLMConfigureTestAccess::HoldNextPoolJob();
			const bool bLate = BeginConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Late, TEXT("selection after BeginConfigure()"), nullptr,
				[this]()
				{
					RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1"));
					FSuperSLMConfigureTestAccess::ReleasePoolJob();
				});
			bOk &= bLate;
			bOk &= TestEqual(TEXT("a selection after BeginConfigure() leaves the held configuration at mode 0: zero matvec bits"), Late.MatvecBits, 0);
			bOk &= CheckConfigureLine(*this, LateLog, 0, 4, TEXT("mode-default"), 0, TEXT("selection after BeginConfigure()"));
		}
#else
		AddError(TEXT("late-selection arm: missing seam -- SuperSLMConfigureTestAccess.h, the BeginConfigure() hold seam (D-SLM8165), is absent"));
		bOk = false;
#endif
		return bOk;
	}

	// SetFinishParallelTasks selects the next Configure()'s task count (at 0 or 1 no hook is
	// installed); out-of-range or malformed input is refused by name and changes nothing.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4SettingsTaskCountTest,
		"SuperSLM.D4.Settings.TaskCountAppliesAtNextConfigure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4SettingsTaskCountTest::RunTest(const FString& Parameters)
	{
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 1"));
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("tasks 1"))) { return false; }
		bOk &= TestEqual(TEXT("FinishParallelTasks 1 installs no hook"), Hooks.Makes, 0);
		bOk &= CheckConfigureLine(*this, Log, 0, 1, TEXT("mode-default"), 0, TEXT("tasks 1"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 2"));
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("tasks 2"))) { return false; }
		bOk &= TestEqual(TEXT("FinishParallelTasks 2 installs a hook on both lanes"), Hooks.Makes, 2);
		bOk &= CheckConfigureLine(*this, Log, 0, 2, TEXT("mode-default"), 0, TEXT("tasks 2"));

		const TCHAR* BadTasks[] = { TEXT("SuperSLM.SetFinishParallelTasks 257"), TEXT("SuperSLM.SetFinishParallelTasks -1"),
			TEXT("SuperSLM.SetFinishParallelTasks four"), TEXT("SuperSLM.SetFinishParallelTasks") };
		const TCHAR* BadModes[] = { TEXT("SuperSLM.SetDecodeMode 2"), TEXT("SuperSLM.SetDecodeMode on"), TEXT("SuperSLM.SetDecodeMode") };
		AddExpectedErrorPlain(TEXT("Usage: SuperSLM.SetFinishParallelTasks"), EAutomationExpectedErrorFlags::Contains, static_cast<int32>(UE_ARRAY_COUNT(BadTasks)));
		AddExpectedErrorPlain(TEXT("Usage: SuperSLM.SetDecodeMode"), EAutomationExpectedErrorFlags::Contains, static_cast<int32>(UE_ARRAY_COUNT(BadModes)));
		for (const TCHAR* Command : BadTasks) { bOk &= RunConsole(*this, Command); }
		for (const TCHAR* Command : BadModes) { bOk &= RunConsole(*this, Command); }
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("after refused input"))) { return false; }
		bOk &= TestEqual(TEXT("refused input leaves the task count at 2"), Hooks.Makes, 2);
		bOk &= TestEqual(TEXT("refused input leaves the mode at 0"), Hooks.ZeroBits, 2);
		bOk &= CheckConfigureLine(*this, Log, 0, 2, TEXT("mode-default"), 0, TEXT("after refused input"));
		bOk &= TestFalse(TEXT("no --save, no RuntimeSettings.ini"), IFileManager::Get().FileExists(*RuntimeSettingsPath()));
		return bOk;
	}

	// --save persists each setting without dropping the other, leaves no temporary file, and a failed
	// save (read-only destination) preserves the prior bytes.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4SettingsSaveTest,
		"SuperSLM.D4.Settings.SaveWritesBothSettingsAtomically", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4SettingsSaveTest::RunTest(const FString& Parameters)
	{
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1 --save"));
		bOk &= CheckSaved(*this, TEXT("1"), TEXT("4"), TEXT("after SetDecodeMode 1 --save"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 2 --save"));
		bOk &= CheckSaved(*this, TEXT("1"), TEXT("2"), TEXT("after SetFinishParallelTasks 2 --save"));
		TArray<FString> Left = SiblingsOf(FPaths::GetPath(RuntimeSettingsPath()), FPaths::GetCleanFilename(RuntimeSettingsPath()));
		bOk &= TestEqual(TEXT("no temporary sibling of RuntimeSettings.ini is left behind"), Left.Num(), 0);

		bool bExists = false;
		TArray<uint8> Saved, After;
		ReadBytes(RuntimeSettingsPath(), bExists, Saved);
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 3"));
		ReadBytes(RuntimeSettingsPath(), bExists, After);
		bOk &= TestTrue(TEXT("settings changed without --save leave the saved bytes"), bExists && After == Saved);

		bOk &= TestTrue(TEXT("RuntimeSettings.ini made read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*RuntimeSettingsPath(), true));
		AddExpectedErrorPlain(TEXT("SuperSLM.SetDecodeMode: could not save"), EAutomationExpectedErrorFlags::Contains, 1);
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0 --save"));
		ReadBytes(RuntimeSettingsPath(), bExists, After);
		bOk &= TestTrue(TEXT("a failed save preserves the prior bytes"), bExists && After == Saved);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*RuntimeSettingsPath(), false);
		return bOk;
	}

	// A caller's explicit setting wins over the saved one, an explicit value equal to the shipped
	// default included (D-SLM8109); with the fields unset again the saved values return; neither
	// configuration writes a byte to either INI. The return to the saved values runs through
	// BeginConfigure() (D-SLM8144).
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4SettingsExplicitCallerTest,
		"SuperSLM.D4.Settings.ExplicitCallerSettingsWin", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4SettingsExplicitCallerTest::RunTest(const FString& Parameters)
	{
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm() && Clean.Costs.Seed(SentinelText))) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		// Saved mode 1 and one task; the process selection is the same pair, so nothing but a caller
		// value can move either field.
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1 --save"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 1 --save"));
		bool bSettingsExist = false, bCostsExist = false;
		TArray<uint8> SettingsBytes, CostsBytes;
		ReadBytes(RuntimeSettingsPath(), bSettingsExist, SettingsBytes);
		ReadBytes(CalibratedCostsPath(), bCostsExist, CostsBytes);

		{
			FSuperSLMRuntimeConfig Config = ShapeOnlyCpuConfig();
			SuperSLMConfigAccess::SetMode(Config, false); // equal to the shipped default, still explicit
			SuperSLMConfigAccess::SetTasks(Config, 4);    // equal to the shipped default, still explicit
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			FSuperSLMConfigureReport Report;
			if (!ConfigureObserved(*this, *F.Cpu, *F.Model, Config, Hooks, TEXT("explicit mode 0, tasks 4 over saved mode 1, tasks 1"), &Report)) { return false; }
			bOk &= TestEqual(TEXT("the caller's FinishParallelTasks 4 installs hooks despite the saved 1"), Hooks.Makes, 2);
			bOk &= TestEqual(TEXT("the caller's bParallelDecode false installs zero matvec bits despite the saved 1"), Hooks.MatvecBits, 0);
			bOk &= CheckConfigureLine(*this, Log, 0, 4, TEXT("mode-default"), 0, TEXT("explicit caller settings"));
		}
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			FSuperSLMConfigureReport Report;
			if (!BeginConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("fields unset again (BeginConfigure)"), &Report)) { return false; }
			bOk &= TestEqual(TEXT("unset again: the saved task count 1 installs no hook"), Hooks.Makes, 0);
			bOk &= CheckConfigureLine(*this, Log, 0, 1, TEXT("mode-default"), 0, TEXT("fields unset again"));
			FCostProfile Profiles[2];
			FString Error;
			if (TestTrue(*FString::Printf(TEXT("one-time profiles load (%s)"), *Error), LoadOneTimeProfiles(Profiles, Error)))
			{
				bOk &= CheckResolved(*this, Report, true, false, 1, TEXT("mode-default"), 0, Profiles[0], TEXT("fields unset again"));
			}
		}
		bOk &= BytesUnchanged(*this, RuntimeSettingsPath(), bSettingsExist, SettingsBytes, TEXT("RuntimeSettings.ini after caller configurations"));
		bOk &= BytesUnchanged(*this, CalibratedCostsPath(), bCostsExist, CostsBytes, TEXT("seeded CalibratedCosts.ini after caller configurations"));
		return bOk;
	}

	// This process's selection beats the saved file, saved or not (D-SLM8148).
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4SettingsProcessSelectionTest,
		"SuperSLM.D4.Settings.ProcessSelectionBeatsSavedFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4SettingsProcessSelectionTest::RunTest(const FString& Parameters)
	{
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1 --save"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0"));
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("saved mode 1, selected mode 0"))) { return false; }
			bOk &= TestEqual(TEXT("the unsaved selection 0 beats the saved 1: zero-bit hooks on both lanes"), Hooks.ZeroBits, 2);
			bOk &= CheckConfigureLine(*this, Log, 0, 4, TEXT("mode-default"), 0, TEXT("saved mode 1, selected mode 0"));
		}
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 1 --save"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 2"));
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("saved tasks 1, selected tasks 2"))) { return false; }
			bOk &= TestEqual(TEXT("the unsaved selection 2 beats the saved 1: hooks on both lanes"), Hooks.Makes, 2);
			bOk &= CheckConfigureLine(*this, Log, 0, 2, TEXT("mode-default"), 0, TEXT("saved tasks 1, selected tasks 2"));
		}
		return bOk;
	}

	// --- Restart pairs: the prepare cell and the observe cell run in SEPARATE editor processes, in
	// that order (the box driver runs each filter in its own UnrealEditor-Cmd invocation).

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4RestartPrepareOnTest,
		"SuperSLM.D4.Settings.Restart.PrepareSavedOn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4RestartPrepareOnTest::RunTest(const FString& Parameters)
	{
		if (!BackUpForRestart(*this)) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1 --save"));
		bOk &= CheckSaved(*this, TEXT("1"), TEXT("4"), TEXT("prepared saved-on"));
		return bOk;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4RestartObserveOnTest,
		"SuperSLM.D4.Settings.Restart.ObserveSavedOn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4RestartObserveOnTest::RunTest(const FString& Parameters)
	{
		FRestartRestore Restore;
		bool bOk = CheckWrittenBeforeThisProcess(*this);
		bOk &= CheckSaved(*this, TEXT("1"), TEXT("4"), TEXT("restarted saved-on"));
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("first Configure after a saved-on restart"))) { return false; }
		bOk &= TestEqual(TEXT("the saved mode 1 is loaded before the first Configure(): matvec bits on both lanes"), Hooks.MatvecBits, 2);
		bOk &= CheckConfigureLine(*this, Log, 1, 4, TEXT("mode-default"), 0, TEXT("saved-on restart"));
		int32 Matvec = -1;
		bOk &= MatvecRunsOfOneGeneration(*this, *F.Cpu, Matvec);
		bOk &= TestTrue(TEXT("the restarted mode-1 decode runs projections through the hook"), Matvec > 0);
		return bOk;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4RestartPrepareOffTest,
		"SuperSLM.D4.Settings.Restart.PrepareSavedOffOneTask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4RestartPrepareOffTest::RunTest(const FString& Parameters)
	{
		if (!BackUpForRestart(*this)) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0 --save"));
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 1 --save"));
		bOk &= CheckSaved(*this, TEXT("0"), TEXT("1"), TEXT("prepared saved-off, one task"));
		return bOk;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4RestartObserveOffTest,
		"SuperSLM.D4.Settings.Restart.ObserveSavedOffOneTask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4RestartObserveOffTest::RunTest(const FString& Parameters)
	{
		FRestartRestore Restore;
		bool bOk = CheckWrittenBeforeThisProcess(*this);
		bOk &= CheckSaved(*this, TEXT("0"), TEXT("1"), TEXT("restarted saved-off, one task"));
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		if (!ConfigureObserved(*this, *F.Cpu, *F.Model, ShapeOnlyCpuConfig(), Hooks, TEXT("first Configure after a saved-off restart"))) { return false; }
		bOk &= TestEqual(TEXT("the saved FinishParallelTasks 1 is loaded before the first Configure(): no hook"), Hooks.Makes, 0);
		bOk &= CheckConfigureLine(*this, Log, 0, 1, TEXT("mode-default"), 0, TEXT("saved-off restart"));
		return bOk;
	}

	// --- The one-time measured profiles ---

	// With no caller costs and no saved estimate, every job kind is priced from the configured mode's
	// one-time profile -- all thirteen fields, both modes.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4ProfilesDefaultTest,
		"SuperSLM.D4.Profiles.DefaultsFollowModeAndPinTable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4ProfilesDefaultTest::RunTest(const FString& Parameters)
	{
		FCostProfile Profiles[2];
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("one-time profiles load (%s)"), *Error), LoadOneTimeProfiles(Profiles, Error))) { return false; }
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity Capacity;
		bool bOk = true;
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FSuperSLMRuntimeConfig Config = ShapeOnlyCpuConfig();
			SuperSLMConfigAccess::SetMode(Config, Mode == 1);
			bOk &= ConfigureAndPrice(*this, F, Config, Mode, 4, TEXT("mode-default"), 0, Profiles[Mode],
				Mode == 0 ? TEXT("mode-0 default profile") : TEXT("mode-1 default profile"));
		}
		return bOk;
	}

	// Per-field resolution (D-SLM8109, D-SLM8148), in mode 0: each of the thirteen caller costs alone
	// (the other twelve from the profile), a caller cost equal to its profile value (still counted),
	// twelve caller costs with the thirteenth from the profile, and all thirteen (no profile
	// consulted). No caller configuration writes a byte to either INI.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4ProfilesCallerTest,
		"SuperSLM.D4.Profiles.ExplicitCallerCostsWin", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4ProfilesCallerTest::RunTest(const FString& Parameters)
	{
		FCostProfile Profiles[2];
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("one-time profiles load (%s)"), *Error), LoadOneTimeProfiles(Profiles, Error))) { return false; }
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm() && Clean.Costs.Seed(SentinelText))) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 0 --save")); // a saved file whose bytes must survive
		bool bSettingsExist = false, bCostsExist = false;
		TArray<uint8> SettingsBytes, CostsBytes;
		ReadBytes(RuntimeSettingsPath(), bSettingsExist, SettingsBytes);
		ReadBytes(CalibratedCostsPath(), bCostsExist, CostsBytes);
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity Capacity;
		const FCostProfile& Profile = Profiles[0];
		FCostProfile Caller;
		for (const FString& Field : CostFields()) { Caller.Add(Field, Profile.FindRef(Field) * 1.5 + (IsPerPositionField(Field) ? 0.0005 : 0.125)); }

		auto Run = [&](const FCostProfile& Set, const TCHAR* Source, const FString& Label, FSuperSLMConfigureReport* OutReport)
		{
			FSuperSLMRuntimeConfig Config = ShapeOnlyCpuConfig();
			SuperSLMConfigAccess::SetMode(Config, false);
			SuperSLMConfigAccess::SetCosts(Config, Set);
			FCostProfile Expected = Profile;
			for (const TPair<FString, double>& P : Set) { Expected.Add(P.Key, P.Value); }
			return ConfigureAndPrice(*this, F, Config, 0, 4, Source, Set.Num(), Expected, *Label, OutReport);
		};
		for (const FString& Field : CostFields())
		{
			FCostProfile One;
			One.Add(Field, Caller.FindRef(Field));
			bOk &= Run(One, TEXT("mode-default"), FString::Printf(TEXT("caller %s alone"), *Field), nullptr);
		}
		{
			FCostProfile EqualToProfile;
			EqualToProfile.Add(TEXT("ResetCostMs"), Profile.FindRef(TEXT("ResetCostMs")));
			bOk &= Run(EqualToProfile, TEXT("mode-default"), TEXT("caller ResetCostMs equal to its profile value"), nullptr);
		}
		{
			FCostProfile Twelve = Caller;
			Twelve.Remove(TEXT("PrefixReleaseCostMs"));
			FSuperSLMConfigureReport Report;
			bOk &= Run(Twelve, TEXT("mode-default"), TEXT("twelve caller costs"), &Report);
			FCostProfile Expected = Profile;
			for (const TPair<FString, double>& P : Twelve) { Expected.Add(P.Key, P.Value); }
			bOk &= CheckResolved(*this, Report, false, false, 4, TEXT("mode-default"), 12, Expected, TEXT("twelve caller costs"));
		}
		bOk &= Run(Caller, TEXT("caller"), TEXT("all thirteen caller costs"), nullptr);
		bOk &= BytesUnchanged(*this, RuntimeSettingsPath(), bSettingsExist, SettingsBytes, TEXT("RuntimeSettings.ini after caller configurations"));
		bOk &= BytesUnchanged(*this, CalibratedCostsPath(), bCostsExist, CostsBytes, TEXT("seeded CalibratedCosts.ini after caller configurations"));
		return bOk;
	}

	// The profile follows the RESOLVED mode and task count (D-SLM8148, D-SLM8166): a process
	// selection of mode 1 prices from mode 1's profile; a matching mode-1 saved estimate measured at
	// task count 2 is chosen by the selected mode and task count; mode 1 at task count 1 is
	// effectively mode 0 and prices from mode 0's profile.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4ProfilesResolvedModeTest,
		"SuperSLM.D4.Profiles.ProfileFollowsResolvedMode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4ProfilesResolvedModeTest::RunTest(const FString& Parameters)
	{
		FCostProfile Profiles[2];
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("one-time profiles load (%s)"), *Error), LoadOneTimeProfiles(Profiles, Error))) { return false; }
		FCleanSettings Clean;
		if (!TestTrue(TEXT("prior INI files captured and cleared"), Clean.Arm())) { return false; }
		FCpuFixture F;
		if (!F.Init(*this)) { return false; }
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity Capacity;

		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetDecodeMode 1"));
		bOk &= ConfigureAndPrice(*this, F, ShapeOnlyCpuConfig(), 1, 4, TEXT("mode-default"), 0, Profiles[1], TEXT("selected mode 1, no estimate"));

		// A mode-1 saved estimate measured at task count 2.
		FString Artifact, Reason;
		if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), SuperSLML2S1Fixtures::TryGetAExArtifactPath(Artifact, Reason))) { return false; }
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 2"));
		FEstimateReport R;
		{
			FSuperSLMLogCapture Log;
			bOk &= RunConsole(*this, FString::Printf(TEXT("SuperSLM.EstimateCosts \"%s\" --write-ini"), *Artifact));
			R = ParseEstimateReport(Log.Snapshot());
		}
		FCostProfile Saved;
		bool bWritten = false;
		{
			FString Text;
			TArray<FString> Lines;
			if (FFileHelper::LoadFileToString(Text, *CalibratedCostsPath()))
			{
				const TArray<FIniSection> Sections = ParseIniSections(Text, Lines);
				for (const FIniSection* S : SectionsWithKey(Sections, TEXT("bParallelDecode")))
				{
					if (S->Keys.FindRef(TEXT("bParallelDecode")) != TEXT("1")) { continue; }
					bWritten = S->Keys.FindRef(TEXT("FinishParallelTasks")) == TEXT("2");
					for (const FString& Field : CostFields())
					{
						double V = 0.0;
						bWritten &= ParseDouble(S->Keys.FindRef(Field), V);
						Saved.Add(Field, V);
					}
				}
			}
		}
		if (!TestTrue(TEXT("precondition: the estimate wrote a mode-1 profile labelled FinishParallelTasks=2"), bWritten && R.Status == TEXT("complete"))) { return false; }
		{
			FSuperSLMConfigureReport Report;
			bOk &= ConfigureAndPrice(*this, F, ShapeOnlyCpuConfig(), 1, 2, TEXT("saved-estimate"), 0, Saved, TEXT("selected mode 1 and tasks 2 over a matching estimate"), &Report);
			bOk &= CheckResolved(*this, Report, true, true, 2, TEXT("saved-estimate"), 0, Saved, TEXT("selected mode 1 and tasks 2 over a matching estimate"));
		}

		// Mode 1 at task count 1: no hook, so the matvec bit cannot ride; effectively mode 0.
		bOk &= RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 1"));
		bOk &= ConfigureAndPrice(*this, F, ShapeOnlyCpuConfig(), 0, 1, TEXT("mode-default"), 0, Profiles[0], TEXT("selected mode 1 at task count 1"));
		return bOk;
	}
} // namespace SuperSLMD4SettingsTests

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
