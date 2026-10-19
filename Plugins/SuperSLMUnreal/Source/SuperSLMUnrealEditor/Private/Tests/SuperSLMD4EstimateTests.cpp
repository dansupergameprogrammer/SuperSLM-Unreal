// T-3102 (plan §10.3.1 item 5b, D-SLM8071/D-SLM8074 and the rulings D-SLM8108-8115, D-SLM8140-8148,
// D-SLM8165-8168): the user calibration estimate, SuperSLM.EstimateCosts "<ArtifactPath.sslm>"
// [--write-ini], and the retirement of SuperSLM.CalibrateCosts. These cells assert mechanics and
// labelling on the real A-EX path -- both modes install their bits and time every arm, the report
// is labelled an estimate and carries spread, foreign load and a sound own-process subtraction, no
// flag writes nothing, the opt-in write lands after the post window and is read back by the next
// Configure() only on a matching artifact, effective mode and task count, a bad saved profile
// falls back to the mode profile with one named Warning -- and never scheduler accuracy. The
// report's statistics are recomputed from its retained calls by ci/tests/d4_estimate_witness.py.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

#include "Fixtures/SuperSLMD4Fixtures.h"
#include "Tests/AutomationCommon.h"

// The estimate's fault seam (plan item 5b contract item 12): a test-only header added to the
// runtime module beside the estimate. Until it exists every fault cell fails by name, never by a
// compile error.
#if __has_include("SuperSLMEstimateTestAccess.h")
#include "SuperSLMEstimateTestAccess.h"
#define SUPERSLM_D4_ESTIMATE_FAULT_SEAM 1
#else
#define SUPERSLM_D4_ESTIMATE_FAULT_SEAM 0
#endif

namespace SuperSLMD4EstimateTests
{
	using namespace SuperSLMD4Fixtures;

	const TCHAR* const SentinelText = TEXT("; T-3102 sentinel: an estimate run that does not write must leave these bytes\n[SuperSLM.T3102Sentinel]\nValue=1\n");

	// A prior saved estimate for another artifact: --write-ini keeps it, byte-equal, after this run's
	// two profiles.
	const TCHAR* const ForeignProfileText = TEXT("[SuperSLM.EstimatedCosts.Foreign]\nbParallelDecode=0\nArtifactHash=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\nFinishParallelTasks=4\nResetCostMs=1.25\n");

	bool ArtifactAndHash(FAutomationTestBase& T, FString& OutArtifact, FString& OutHash)
	{
		FString Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), SuperSLML2S1Fixtures::TryGetAExArtifactPath(OutArtifact, Reason)))
		{
			return false;
		}
		return T.TestTrue(TEXT("A-EX embedded artifact hash readable"), ReadEmbeddedArtifactHashHex(OutArtifact, OutHash));
	}

	FString EstimateCommand(const FString& Artifact, const TCHAR* Flags)
	{
		return FString::Printf(TEXT("SuperSLM.EstimateCosts \"%s\"%s"), *Artifact, Flags);
	}

	// Runs one estimate under a hook probe; the caller owns the log capture so its lines outlive
	// the run.
	bool RunEstimate(FAutomationTestBase& T, const FString& Command, FHookReading& OutHooks, int32& OutHookRuns)
	{
		FHookProbe Probe;
		const bool bRegistered = RunConsole(T, Command);
		FSuperSLMParallelDecodeTestAccess::StopHookObservations();
		ReadHooks(OutHooks, TEXT("estimate hooks"), T);
		OutHookRuns = FSuperSLMParallelDecodeTestAccess::GetRunCount();
		T.AddInfo(FString::Printf(TEXT("estimate hook runs=%d"), OutHookRuns));
		return bRegistered;
	}

	// Both modes ran on real hooks: one zero-bit install and one SSLM_PARALLEL_FOR_MATVEC install,
	// and the timed arms called them.
	bool CheckBothModeHooks(FAutomationTestBase& T, const FHookReading& Hooks, int32 HookRuns)
	{
		bool bOk = T.TestEqual(TEXT("the estimate installs one finish hook per mode"), Hooks.Makes, 2);
		bOk &= T.TestEqual(TEXT("mode 0 installs a zero-bit hook"), Hooks.ZeroBits, 1);
		bOk &= T.TestEqual(TEXT("mode 1 installs an SSLM_PARALLEL_FOR_MATVEC hook"), Hooks.MatvecBits, 1);
		bOk &= T.TestTrue(TEXT("the timed arms called the installed hooks"), HookRuns > 0);
		return bOk;
	}

	bool FileBytesEqual(FAutomationTestBase& T, const FString& Path, bool bExpectExists, const TArray<uint8>& Expected, const TCHAR* What)
	{
		bool bExists = false;
		TArray<uint8> Bytes;
		const bool bRead = ReadBytes(Path, bExists, Bytes);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: existence unchanged"), What), bExists, bExpectExists);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: bytes unchanged"), What), bRead && (!bExists || Bytes == Expected));
		return bOk;
	}

	bool NoTemporarySiblings(FAutomationTestBase& T, const FString& Path)
	{
		const TArray<FString> Left = SiblingsOf(FPaths::GetPath(Path), FPaths::GetCleanFilename(Path));
		for (const FString& N : Left) { T.AddInfo(FString::Printf(TEXT("left beside %s: %s"), *Path, *N)); }
		return T.TestEqual(*FString::Printf(TEXT("no temporary sibling of %s is left behind"), *FPaths::GetCleanFilename(Path)), Left.Num(), 0);
	}

	// The written CalibratedCosts.ini: exactly two estimate profiles for this artifact, one per mode,
	// each labelled with the artifact hash and task count, carrying the report's thirteen suggested
	// decimal strings exactly (contract item 10). Profiles kept for other artifacts are not counted.
	// Returns the parsed profiles.
	bool CheckWrittenProfiles(FAutomationTestBase& T, const FEstimateReport& R, const FString& Hash, FCostProfile (&OutProfiles)[2])
	{
		FString Text;
		if (!T.TestTrue(TEXT("written CalibratedCosts.ini is readable"), FFileHelper::LoadFileToString(Text, *CalibratedCostsPath()))) { return false; }
		bool bOk = T.TestTrue(TEXT("written file is labelled an estimate that is not verified"), Text.Contains(TEXT("not verified")));
		bOk &= T.TestFalse(TEXT("written file carries no sentinel left from before the write"), Text.Contains(TEXT("T3102Sentinel")));
		TArray<FString> Lines;
		const TArray<FIniSection> Sections = ParseIniSections(Text, Lines);
		TArray<const FIniSection*> Profiles = SectionsWithKey(Sections, TEXT("bParallelDecode"));
		Profiles.RemoveAll([&Hash](const FIniSection* S) { return S->Keys.FindRef(TEXT("ArtifactHash")).ToLower() != Hash; });
		bOk &= T.TestEqual(TEXT("written file holds exactly two mode-labelled profiles for this artifact"), Profiles.Num(), 2);
		bool bSeen[2] = {};
		for (const FIniSection* S : Profiles)
		{
			const FString ModeText = S->Keys.FindRef(TEXT("bParallelDecode"));
			const int32 Mode = ModeText == TEXT("1") ? 1 : (ModeText == TEXT("0") ? 0 : -1);
			if (!T.TestTrue(*FString::Printf(TEXT("profile %s is labelled mode 0 or 1"), *S->Header), Mode >= 0)) { bOk = false; continue; }
			bOk &= T.TestFalse(*FString::Printf(TEXT("mode %d profile appears once"), Mode), bSeen[Mode]);
			bSeen[Mode] = true;
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d profile carries the artifact hash"), Mode), S->Keys.FindRef(TEXT("ArtifactHash")).ToLower(), Hash);
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d profile carries the run's task count"), Mode), S->Keys.FindRef(TEXT("FinishParallelTasks")), R.Run.FindRef(TEXT("FinishParallelTasks")));
			OutProfiles[Mode].Reset();
			for (const FString& F : CostFields())
			{
				const FString* Written = S->Keys.Find(F);
				const FString* Reported = R.Suggested[Mode].Find(F);
				double V = 0.0;
				bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d written %s is the report's decimal string exactly"), Mode, *F),
					Written && Reported && *Written == *Reported && ParseDouble(*Written, V));
				OutProfiles[Mode].Add(F, V);
			}
		}
		bOk &= T.TestTrue(TEXT("both mode profiles were written"), bSeen[0] && bSeen[1]);
		return bOk;
	}

	// The written text with the profile labelled bParallelDecode=Mode edited: each (key, value) in
	// Edits replaces that key's value, a value of "<delete>" removes the key's line, and an empty
	// Edits map removes the whole section.
	bool WriteMutatedProfile(FAutomationTestBase& T, const FString& Original, int32 Mode, const TMap<FString, FString>& Edits, const TCHAR* What)
	{
		TArray<FString> Lines;
		const TArray<FIniSection> Sections = ParseIniSections(Original, Lines);
		const FIniSection* Target = nullptr;
		for (const FIniSection* S : SectionsWithKey(Sections, TEXT("bParallelDecode")))
		{
			// The first match, as Configure() reads it: this run's profiles come before kept ones.
			if (Target == nullptr && S->Keys.FindRef(TEXT("bParallelDecode")) == FString::FromInt(Mode)) { Target = S; }
		}
		if (!T.TestNotNull(*FString::Printf(TEXT("%s: mode %d profile found"), What, Mode), Target)) { return false; }
		TArray<FString> Out;
		int32 Changed = 0;
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const bool bInSection = I >= Target->FirstLine && I <= Target->LastLine;
			if (bInSection && Edits.Num() == 0) { ++Changed; continue; }
			FString Key, Value;
			if (bInSection && Lines[I].TrimStart().Split(TEXT("="), &Key, &Value) && Edits.Contains(Key.TrimStartAndEnd()))
			{
				const FString& NewValue = Edits.FindChecked(Key.TrimStartAndEnd());
				++Changed;
				if (NewValue != TEXT("<delete>")) { Out.Add(Key.TrimStartAndEnd() + TEXT("=") + NewValue); }
				continue;
			}
			Out.Add(Lines[I]);
		}
		return T.TestTrue(*FString::Printf(TEXT("%s: mode %d profile edited (%d line(s))"), What, Mode, Changed), Changed > 0 && (Edits.Num() == 0 || Changed == Edits.Num()))
			&& T.TestTrue(*FString::Printf(TEXT("%s: CalibratedCosts.ini written"), What), FFileHelper::SaveStringToFile(FString::Join(Out, TEXT("\n")) + TEXT("\n"), *CalibratedCostsPath()));
	}

	// Configures A-EX in Mode, with the listed caller costs set and every other cost field left
	// unset, drives every job kind, and requires every planned cost to come from Expected and the
	// Configure() line to name Source and the caller-field count. bBegin runs BeginConfigure().
	bool PricedBy(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model, int32 Mode,
		const FCostProfile& Caller, const FCostProfile& Expected, const TCHAR* Source, const TCHAR* Label,
		bool bBegin = false, FSuperSLMConfigureReport* OutReport = nullptr)
	{
		FSuperSLMRuntimeConfig Config = ShapeOnlyCpuConfig();
		SuperSLMConfigAccess::SetMode(Config, Mode == 1);
		SuperSLMConfigAccess::SetCosts(Config, Caller);
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		const bool bConfigured = bBegin
			? BeginConfigureObserved(T, Cpu, Model, Config, Hooks, Label, OutReport)
			: ConfigureObserved(T, Cpu, Model, Config, Hooks, Label, OutReport);
		if (!bConfigured) { return false; }
		bool bOk = CheckConfigureLine(T, Log, Mode, 4, Source, Caller.Num(), Label);
		TArray<FSuperSLMWorkerJobReport> Rows;
		bOk &= DriveEveryJobKind(T, Cpu, Model, Rows, Label);
		bOk &= CheckRowsPricedBy(T, Rows, Expected, Label);
		return bOk;
	}

	FCostProfile WithOverrides(FCostProfile Base, const FCostProfile& Overrides)
	{
		for (const TPair<FString, double>& P : Overrides) { Base.Add(P.Key, P.Value); }
		return Base;
	}

	// The saved profile must differ from the mode profile somewhere other than the corrupted key,
	// or a fallback to the mode profile could not be told from reading the saved one.
	bool DiscriminatesBeyond(FAutomationTestBase& T, const FCostProfile& Saved, const FCostProfile& Mode, const TArray<FString>& Corrupted, const TCHAR* What)
	{
		bool bDiffers = false;
		for (const FString& F : CostFields())
		{
			if (!Corrupted.Contains(F) && !FMath::IsNearlyEqual(Saved.FindRef(F), Mode.FindRef(F), 1e-9)) { bDiffers = true; }
		}
		return T.TestTrue(*FString::Printf(TEXT("precondition (%s): the saved profile differs from the mode profile in a field other than the corrupted key"), What), bDiffers);
	}

	struct FProcessSettingsReset
	{
		FProcessSettingsReset() { ResetProcessDecodeSettings(); }
		~FProcessSettingsReset() { ResetProcessDecodeSettings(); }
	};

	// The shipped mode and task settings for the cell: no saved RuntimeSettings.ini (restored after)
	// and the process settings at their defaults, so the estimate runs at FinishParallelTasks 4.
	struct FShippedSettings
	{
		FFileBytesGuard Settings{ RuntimeSettingsPath() };
		FProcessSettingsReset Process;
		bool Arm() { return Settings.Capture() && Settings.Remove(); }
	};

#if SUPERSLM_D4_ESTIMATE_FAULT_SEAM
	struct FFaultGuard
	{
		explicit FFaultGuard(ESuperSLMEstimateFault Fault) { FSuperSLMEstimateTestAccess::SetNextFault(Fault); }
		~FFaultGuard() { FSuperSLMEstimateTestAccess::ClearOverrides(); }
	};
#endif

	// A run the seam breaks in mode 1, after mode 0 has completed: it is a named incomplete
	// estimate, it suggests nothing, and --write-ini changes no byte.
	bool RunFaultCell(FAutomationTestBase& T, const TCHAR* FaultName, int32 FaultValue, const TCHAR* SpecificError = nullptr)
	{
#if SUPERSLM_D4_ESTIMATE_FAULT_SEAM
		FString Artifact, Hash;
		if (!ArtifactAndHash(T, Artifact, Hash)) { return false; }
		FShippedSettings Shipped;
		FFileBytesGuard Costs(CalibratedCostsPath());
		if (!T.TestTrue(TEXT("prior INI files captured"), Shipped.Arm() && Costs.Capture() && Costs.Seed(SentinelText))) { return false; }
		bool bSeedExists = false;
		TArray<uint8> Seeded;
		ReadBytes(CalibratedCostsPath(), bSeedExists, Seeded);
		// Unreal credits a log line to its first matching expectation. One specific message
		// already proves its general prefix; registering both requires a second error that the
		// command correctly never emits. Require exactly one occurrence of the strongest match.
		T.AddExpectedErrorPlain(SpecificError ? SpecificError : TEXT("SuperSLM.EstimateCosts: incomplete estimate"),
			EAutomationExpectedErrorFlags::Contains, 1);
		FFaultGuard Fault(static_cast<ESuperSLMEstimateFault>(FaultValue));
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		int32 Runs = 0;
		bool bOk = RunEstimate(T, EstimateCommand(Artifact, TEXT(" --write-ini")), Hooks, Runs);
		const FEstimateReport R = ParseEstimateReport(Log.Snapshot());
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the run reached mode 1, so mode 0 completed first"), FaultName), Hooks.MatvecBits == 1);
		bOk &= T.TestNotEqual(*FString::Printf(TEXT("%s: the run is not reported complete"), FaultName), R.Status, FString(TEXT("complete")));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no mode-0 suggestion is produced"), FaultName), R.Suggested[0].Num(), 0);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no mode-1 suggestion is produced"), FaultName), R.Suggested[1].Num(), 0);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing is written"), FaultName), R.WroteIndex, (int32)INDEX_NONE);
		bOk &= FileBytesEqual(T, CalibratedCostsPath(), bSeedExists, Seeded, FaultName);
		bOk &= NoTemporarySiblings(T, CalibratedCostsPath());
		bOk &= T.TestTrue(TEXT("CalibratedCosts.ini restored"), Costs.Restore());
		return bOk;
#else
		(void)FaultValue;
		(void)SpecificError;
		T.AddError(FString::Printf(TEXT("%s: missing seam -- SuperSLMEstimateTestAccess.h, the estimate's fault seam (plan item 5b contract item 12), is absent"), FaultName));
		return false;
#endif
	}

	// Rewrites T-3037/T-3050 OmittedMode, ZeroMode and OneMode for the estimate: one run measures
	// both modes, installs each mode's bits, warms each mode before timing it, times every arm,
	// reports an estimate with spread, foreign load and a sound own-process subtraction under the
	// held build lock, and without --write-ini changes no INI byte.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateNoFlagTest,
		"SuperSLM.D4.Estimate.NoFlagReportsBothModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateNoFlagTest::RunTest(const FString& Parameters)
	{
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FFileBytesGuard Costs(CalibratedCostsPath());
		FFileBytesGuard Settings(RuntimeSettingsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Costs.Capture() && Settings.Capture() && Costs.Seed(SentinelText) && Settings.Remove())) { return false; }
		FProcessSettingsReset ProcessSettings;
		bool bCostsExist = false, bSettingsExist = false;
		TArray<uint8> CostBytes, SettingsBytes;
		ReadBytes(CalibratedCostsPath(), bCostsExist, CostBytes);
		ReadBytes(RuntimeSettingsPath(), bSettingsExist, SettingsBytes);

		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		int32 Runs = 0;
		bool bOk = RunEstimate(*this, EstimateCommand(Artifact, TEXT("")), Hooks, Runs);
		bOk &= CheckBothModeHooks(*this, Hooks, Runs);
		const FEstimateReport R = ParseEstimateReport(Log.Snapshot());
		bOk &= CheckCompleteEstimate(*this, R, Hash);
		bOk &= !R.FilesDir.IsEmpty() && CheckSampleSeries(*this, R.FilesDir, /*bRequireOwnLock*/ true);
		bOk &= TestEqual(TEXT("without --write-ini nothing is reported written"), R.WroteIndex, (int32)INDEX_NONE);
		bOk &= FileBytesEqual(*this, CalibratedCostsPath(), bCostsExist, CostBytes, TEXT("CalibratedCosts.ini without --write-ini"));
		bOk &= FileBytesEqual(*this, RuntimeSettingsPath(), bSettingsExist, SettingsBytes, TEXT("RuntimeSettings.ini during an estimate"));
		bOk &= NoTemporarySiblings(*this, CalibratedCostsPath());
		bOk &= TestTrue(TEXT("INI files restored"), Costs.Restore() && Settings.Restore());
		return bOk;
	}

	// The opt-in write: after the post window, two mode-labelled profiles equal to the report's
	// strings; the next Configure() reads them back only on a matching artifact, mode and task
	// count; a corrupted saved profile falls back to the mode profile with one named Warning; caller
	// costs win field by field, also through BeginConfigure(); a caller cost that trips the cost
	// domain refuses with no fallback.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateWriteIniReadBackTest,
		"SuperSLM.D4.Estimate.WriteIniReadBack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateWriteIniReadBackTest::RunTest(const FString& Parameters)
	{
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FCostProfile Defaults[2];
		FString FixtureError;
		if (!TestTrue(*FString::Printf(TEXT("one-time profiles load (%s)"), *FixtureError), LoadOneTimeProfiles(Defaults, FixtureError))) { return false; }
		FFileBytesGuard Costs(CalibratedCostsPath());
		FFileBytesGuard Settings(RuntimeSettingsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Costs.Capture() && Settings.Capture() && Costs.Seed(FString(SentinelText) + ForeignProfileText) && Settings.Remove())) { return false; }
		FProcessSettingsReset ProcessSettings;

		// The four bad-profile arms each expect exactly one Warning per Configure() of the corrupted
		// mode (D-SLM8145, D-SLM8148); the caller-1e12 arm expects none (D-SLM8167), so "cost
		// domain" is seen exactly once, from the all-1e12 arm.
		AddExpectedError(TEXT("CalibratedCosts\\.ini.*AdoptCostMs|AdoptCostMs.*CalibratedCosts\\.ini"), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedError(TEXT("CalibratedCosts\\.ini.*SaveCostMs|SaveCostMs.*CalibratedCosts\\.ini"), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedError(TEXT("CalibratedCosts\\.ini.*RestoreCostMs|RestoreCostMs.*CalibratedCosts\\.ini"), EAutomationExpectedErrorFlags::Contains, 1);
		AddExpectedError(TEXT("CalibratedCosts\\.ini.*cost domain|cost domain.*CalibratedCosts\\.ini"), EAutomationExpectedErrorFlags::Contains, 1);

		FEstimateReport R;
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			int32 Runs = 0;
			bool bRan = RunEstimate(*this, EstimateCommand(Artifact, TEXT(" --write-ini")), Hooks, Runs);
			bRan &= CheckBothModeHooks(*this, Hooks, Runs);
			R = ParseEstimateReport(Log.Snapshot());
			if (!CheckCompleteEstimate(*this, R, Hash) || !bRan) { return false; }
		}
		bool bOk = TestTrue(TEXT("--write-ini reports the write"), R.WroteIndex != INDEX_NONE);
		bOk &= TestTrue(TEXT("the write is reported after the post window's load summary"), R.PostLoadIndex != INDEX_NONE && R.WroteIndex > R.PostLoadIndex);
		bOk &= TestTrue(TEXT("the write is reported after the run completed"), R.CompleteIndex != INDEX_NONE && R.WroteIndex > R.CompleteIndex);
		bOk &= TestTrue(*FString::Printf(TEXT("the written path is Saved/SuperSLM/CalibratedCosts.ini (%s)"), *R.WrotePath), FPaths::IsSamePath(R.WrotePath, CalibratedCostsPath()));
		bOk &= NoTemporarySiblings(*this, CalibratedCostsPath());
		FCostProfile Written[2];
		if (!CheckWrittenProfiles(*this, R, Hash, Written)) { return false; }
		{
			FString Merged;
			FFileHelper::LoadFileToString(Merged, *CalibratedCostsPath());
			const int32 Foreign = Merged.Find(ForeignProfileText, ESearchCase::CaseSensitive);
			const int32 Mode1 = Merged.Find(TEXT("[SuperSLM.EstimatedCosts.Mode1]"), ESearchCase::CaseSensitive);
			bOk &= TestTrue(TEXT("the other artifact's prior profile survives the write byte-equal"), Foreign != INDEX_NONE);
			bOk &= TestTrue(TEXT("the kept profile follows this run's two profiles"), Mode1 != INDEX_NONE && Foreign > Mode1);
		}
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			bOk &= TestFalse(*FString::Printf(TEXT("precondition: the mode %d estimate's ResetCostMs differs from the mode profile, so the read-back discriminates"), Mode),
				FMath::IsNearlyEqual(Written[Mode].FindRef(TEXT("ResetCostMs")), Defaults[Mode].FindRef(TEXT("ResetCostMs")), 1e-9));
		}
		FString Original;
		if (!TestTrue(TEXT("written file kept for mutation"), FFileHelper::LoadFileToString(Original, *CalibratedCostsPath()))) { return false; }

		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
		USuperSLMModel* Model = nullptr;
		if (!ImportAEx(*this, Model)) { return false; }
		USuperSLMSubsystem* Cpu = SuperSLML2S1Fixtures::GetSubsystem(World.GetTestWorld());
		if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity Capacity;
		const FCostProfile None;

		// Matching artifact, mode and task count: the saved estimate prices every job kind.
		{
			FSuperSLMConfigureReport Report;
			bOk &= PricedBy(*this, *Cpu, *Model, 0, None, Written[0], TEXT("saved-estimate"), TEXT("matching mode-0 estimate"), false, &Report);
			bOk &= CheckResolved(*this, Report, false, false, 4, TEXT("saved-estimate"), 0, Written[0], TEXT("matching mode-0 estimate"));
		}
		bOk &= PricedBy(*this, *Cpu, *Model, 1, None, Written[1], TEXT("saved-estimate"), TEXT("matching mode-1 estimate"));

		// One caller field over a matching saved estimate, through BeginConfigure() (D-SLM8144, D-SLM8148).
		{
			FCostProfile Caller;
			Caller.Add(TEXT("ResetCostMs"), Defaults[0].FindRef(TEXT("ResetCostMs")) * 1.5 + 0.125);
			FSuperSLMConfigureReport Report;
			const FCostProfile Expected = WithOverrides(Written[0], Caller);
			bOk &= PricedBy(*this, *Cpu, *Model, 0, Caller, Expected, TEXT("saved-estimate"), TEXT("one caller field over a saved estimate (BeginConfigure)"), true, &Report);
			bOk &= CheckResolved(*this, Report, false, false, 4, TEXT("saved-estimate"), 1, Expected, TEXT("one caller field over a saved estimate"));
		}

		// Artifact mismatch: the mode-0 profile names another artifact.
		bOk &= WriteMutatedProfile(*this, Original, 0, { { TEXT("ArtifactHash"), FString::ChrN(64, TEXT('0')) } }, TEXT("artifact mismatch"));
		bOk &= PricedBy(*this, *Cpu, *Model, 0, None, Defaults[0], TEXT("mode-default"), TEXT("artifact-mismatched estimate"));
		// Task-count mismatch: the mode-1 profile was measured at another FinishParallelTasks.
		bOk &= WriteMutatedProfile(*this, Original, 1, { { TEXT("FinishParallelTasks"), TEXT("2") } }, TEXT("task mismatch"));
		bOk &= PricedBy(*this, *Cpu, *Model, 1, None, Defaults[1], TEXT("mode-default"), TEXT("task-mismatched estimate"));
		// Mode mismatch: only a mode-1 profile remains, and a mode-0 configuration does not use it.
		bOk &= WriteMutatedProfile(*this, Original, 0, {}, TEXT("mode mismatch"));
		bOk &= PricedBy(*this, *Cpu, *Model, 0, None, Defaults[0], TEXT("mode-default"), TEXT("mode-mismatched estimate"));

		// Four corrupted saved profiles (D-SLM8145): each falls back to the mode profile with one
		// Warning, and the other mode's intact profile still prices as a saved estimate.
		TMap<FString, FString> AllMs;
		TArray<FString> AllMsKeys;
		for (const FString& F : CostFields())
		{
			if (!IsPerPositionField(F)) { AllMs.Add(F, TEXT("1e12")); AllMsKeys.Add(F); }
		}
		struct FBadArm { const TCHAR* Name; TMap<FString, FString> Edits; TArray<FString> Keys; };
		const FBadArm Arms[] = {
			{ TEXT("non-finite AdoptCostMs"), { { TEXT("AdoptCostMs"), TEXT("nan") } }, { TEXT("AdoptCostMs") } },
			{ TEXT("missing SaveCostMs"), { { TEXT("SaveCostMs"), TEXT("<delete>") } }, { TEXT("SaveCostMs") } },
			{ TEXT("negative RestoreCostMs"), { { TEXT("RestoreCostMs"), TEXT("-1") } }, { TEXT("RestoreCostMs") } },
			{ TEXT("every millisecond key 1e12 (cost domain)"), AllMs, AllMsKeys },
		};
		for (const FBadArm& Arm : Arms)
		{
			bOk &= DiscriminatesBeyond(*this, Written[0], Defaults[0], Arm.Keys, Arm.Name);
			bOk &= WriteMutatedProfile(*this, Original, 0, Arm.Edits, Arm.Name);
			bOk &= PricedBy(*this, *Cpu, *Model, 0, None, Defaults[0], TEXT("mode-default"), *FString::Printf(TEXT("%s: falls back"), Arm.Name));
			bOk &= PricedBy(*this, *Cpu, *Model, 1, None, Written[1], TEXT("saved-estimate"), *FString::Printf(TEXT("%s: the other mode still reads its estimate"), Arm.Name));
		}

		// A caller cost that trips the cost domain over a matching saved estimate refuses, naming the
		// field, with no fallback Warning (D-SLM8167).
		bOk &= TestTrue(TEXT("unmutated estimate restored"), FFileHelper::SaveStringToFile(Original, *CalibratedCostsPath()));
		{
			FSuperSLMRuntimeConfig Config = ShapeOnlyCpuConfig();
			SuperSLMConfigAccess::SetMode(Config, false);
			SuperSLMConfigAccess::SetCost(Config, TEXT("LayerCostMs"), 1e12);
			FHookReading Hooks;
			FSuperSLMConfigureReport Report;
			bOk &= ConfigureObserved(*this, *Cpu, *Model, Config, Hooks, TEXT("caller LayerCostMs 1e12 over a saved estimate"), &Report, ESuperSLMConfigureResult::InvalidCostDomain);
			bOk &= TestTrue(*FString::Printf(TEXT("the cost-domain refusal names LayerCostMs (%s)"), *Report.Message), Report.Message.Contains(TEXT("LayerCostMs")));
		}

		// Explicit caller costs win over a matching saved estimate, in both modes.
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FCostProfile Caller;
			for (const FString& F : CostFields()) { Caller.Add(F, Defaults[Mode].FindRef(F) * 1.5 + (IsPerPositionField(F) ? 0.0005 : 0.125)); }
			bOk &= PricedBy(*this, *Cpu, *Model, Mode, Caller, Caller, TEXT("caller"), Mode == 0 ? TEXT("mode-0 caller costs over an estimate") : TEXT("mode-1 caller costs over an estimate"));
		}
		bOk &= TestTrue(TEXT("INI files restored"), Costs.Restore() && Settings.Restore());
		return bOk;
	}

	// The estimate measures, prints and writes the task count Configure() would resolve with no
	// caller value (D-SLM8140), and the next Configure() with no caller fields matches it.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateTaskCountTest,
		"SuperSLM.D4.Estimate.TaskCountFollowsSettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateTaskCountTest::RunTest(const FString& Parameters)
	{
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FShippedSettings Shipped;
		FFileBytesGuard Costs(CalibratedCostsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Shipped.Arm() && Costs.Capture() && Costs.Seed(SentinelText))) { return false; }
		bool bOk = RunConsole(*this, TEXT("SuperSLM.SetFinishParallelTasks 2"));
		FEstimateReport R;
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			int32 Runs = 0;
			bOk &= RunEstimate(*this, EstimateCommand(Artifact, TEXT(" --write-ini")), Hooks, Runs);
			bOk &= CheckBothModeHooks(*this, Hooks, Runs);
			R = ParseEstimateReport(Log.Snapshot());
			if (!CheckCompleteEstimate(*this, R, Hash, /*ExpectedTasks*/ 2) || !bOk) { return false; }
		}
		FCostProfile Written[2];
		bOk &= CheckWrittenProfiles(*this, R, Hash, Written); // both profiles carry the run's FinishParallelTasks=2

		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
		USuperSLMModel* Model = nullptr;
		if (!ImportAEx(*this, Model)) { return false; }
		USuperSLMSubsystem* Cpu = SuperSLML2S1Fixtures::GetSubsystem(World.GetTestWorld());
		if (!TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		if (!ConfigureObserved(*this, *Cpu, *Model, ShapeOnlyCpuConfig(), Hooks, TEXT("Configure() after a task-count-2 estimate"))) { return false; }
		bOk &= CheckConfigureLine(*this, Log, 0, 2, TEXT("saved-estimate"), 0, TEXT("Configure() after a task-count-2 estimate"));
		bOk &= TestEqual(TEXT("task count 2 installs a hook on both lanes"), Hooks.Makes, 2);
		bOk &= TestTrue(TEXT("CalibratedCosts.ini restored"), Costs.Restore());
		return bOk;
	}

	// High foreign load never suppresses a complete estimate or its opt-in write: the report
	// discloses the load it ran under.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateHighLoadTest,
		"SuperSLM.D4.Estimate.HighLoadStillReportsAndWrites", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateHighLoadTest::RunTest(const FString& Parameters)
	{
		constexpr int32 LoadThreads = 8;
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FShippedSettings Shipped;
		FFileBytesGuard Costs(CalibratedCostsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Shipped.Arm() && Costs.Capture() && Costs.Seed(SentinelText))) { return false; }

		FEstimateReport R;
		FForeignCpuLoad Load;
		{
			FString LoadError;
			if (!TestTrue(*FString::Printf(TEXT("foreign load process started (%s)"), *LoadError), Load.Start(LoadThreads, LoadError))) { return false; }
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			int32 Runs = 0;
			bool bRan = RunEstimate(*this, EstimateCommand(Artifact, TEXT(" --write-ini")), Hooks, Runs);
			Load.Stop();
			bRan &= CheckBothModeHooks(*this, Hooks, Runs);
			R = ParseEstimateReport(Log.Snapshot());
			if (!bRan) { return false; }
		}
		const int32 LogicalProcessors = FMath::Max(1, (int32)::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		const double Visible = 0.5 * 100.0 * LoadThreads / LogicalProcessors;
		AddInfo(FString::Printf(TEXT("foreign load: %d threads over %.1f s used %.1f CPU-s; N=%d; visible threshold %.3f%%"),
			LoadThreads, Load.ActiveWall, Load.ActiveCpu, LogicalProcessors, Visible));
		bool bOk = TestTrue(TEXT("precondition: the foreign load ran across the estimate"), Load.Ran());
		bOk &= CheckCompleteEstimate(*this, R, Hash);
#if SUPERSLM_D4_ESTIMATE_FAULT_SEAM
		const TArray<FSuperSLMEstimateResetObservation>& Resets = FSuperSLMEstimateTestAccess::GetResetObservations();
		bOk &= TestEqual(TEXT("both modes reset real populated states in all five warm-up and twelve timed repetitions"), Resets.Num(), 2 * (5 + 12));
		for (int32 I = 0; I < Resets.Num(); ++I)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("reset %d starts at the actual full context"), I), Resets[I].Before, int64(4095));
			bOk &= TestEqual(*FString::Printf(TEXT("reset %d actually empties the context"), I), Resets[I].After, int64(0));
		}
#endif
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			const FEstimateLoad* L = FindLoad(R, TEXT("workload"), Mode);
			bOk &= TestTrue(*FString::Printf(TEXT("mode %d workload discloses the elevated foreign load (median >= %.3f%%)"), Mode, Visible), L != nullptr && L->Median >= Visible);
		}
		bOk &= TestTrue(TEXT("the opt-in write still happens under high load, after the post window"),
			R.WroteIndex != INDEX_NONE && R.PostLoadIndex != INDEX_NONE && R.WroteIndex > R.PostLoadIndex);
		FCostProfile Written[2];
		bOk &= CheckWrittenProfiles(*this, R, Hash, Written);
		bOk &= TestTrue(TEXT("CalibratedCosts.ini restored"), Costs.Restore());
		return bOk;
	}

	// Rewrites T-3037/T-3050 InvalidMode for the estimate: malformed input never starts a run, and
	// an invalid artifact is a named incomplete estimate; neither writes, even with --write-ini.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateInvalidInputTest,
		"SuperSLM.D4.Estimate.InvalidInputWritesNothing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateInvalidInputTest::RunTest(const FString& Parameters)
	{
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FShippedSettings Shipped;
		FFileBytesGuard Costs(CalibratedCostsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Shipped.Arm() && Costs.Capture() && Costs.Seed(SentinelText))) { return false; }
		bool bSeedExists = false;
		TArray<uint8> Seeded;
		ReadBytes(CalibratedCostsPath(), bSeedExists, Seeded);

		const FString Usage[] = {
			TEXT("SuperSLM.EstimateCosts"),                              // no artifact
			EstimateCommand(Artifact, TEXT(" --write")),                 // unknown flag
			EstimateCommand(Artifact, TEXT(" 1")),                       // the retired CalibrateCosts mode argument
		};
		AddExpectedErrorPlain(TEXT("Usage: SuperSLM.EstimateCosts"), EAutomationExpectedErrorFlags::Contains, static_cast<int32>(UE_ARRAY_COUNT(Usage)));
		AddExpectedErrorPlain(TEXT("SuperSLM.EstimateCosts: incomplete estimate"), EAutomationExpectedErrorFlags::Contains, 0);
		bool bOk = true;
		for (const FString& Command : Usage)
		{
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			int32 Runs = 0;
			bOk &= RunEstimate(*this, Command, Hooks, Runs);
			const FEstimateReport R = ParseEstimateReport(Log.Snapshot());
			bOk &= TestEqual(*FString::Printf(TEXT("'%s' installs no hook"), *Command.Left(48)), Hooks.Makes, 0);
			bOk &= TestEqual(*FString::Printf(TEXT("'%s' starts no estimate"), *Command.Left(48)), R.HeaderIndex, (int32)INDEX_NONE);
			bOk &= TestEqual(*FString::Printf(TEXT("'%s' writes nothing"), *Command.Left(48)), R.WroteIndex, (int32)INDEX_NONE);
		}
		{
			const FString Missing = FPaths::ConvertRelativePathToFull(ScratchDir() / TEXT("t3102-no-such-artifact.sslm"));
			IFileManager::Get().Delete(*Missing);
			FSuperSLMLogCapture Log;
			FHookReading Hooks;
			int32 Runs = 0;
			bOk &= RunEstimate(*this, EstimateCommand(Missing, TEXT(" --write-ini")), Hooks, Runs);
			const FEstimateReport R = ParseEstimateReport(Log.Snapshot());
			bOk &= TestNotEqual(TEXT("an invalid artifact is not reported complete"), R.Status, FString(TEXT("complete")));
			bOk &= TestEqual(TEXT("an invalid artifact suggests nothing"), R.Suggested[0].Num() + R.Suggested[1].Num(), 0);
			bOk &= TestEqual(TEXT("an invalid artifact writes nothing, even with --write-ini"), R.WroteIndex, (int32)INDEX_NONE);
		}
		bOk &= FileBytesEqual(*this, CalibratedCostsPath(), bSeedExists, Seeded, TEXT("CalibratedCosts.ini after invalid input"));
		bOk &= TestTrue(TEXT("CalibratedCosts.ini restored"), Costs.Restore());
		return bOk;
	}

	// A failed opt-in write (the destination is read-only) preserves the prior bytes and is named;
	// the estimate itself still completes.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateWriteFailureTest,
		"SuperSLM.D4.Estimate.WriteFailurePreservesPriorBytes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateWriteFailureTest::RunTest(const FString& Parameters)
	{
		FString Artifact, Hash;
		if (!ArtifactAndHash(*this, Artifact, Hash)) { return false; }
		FShippedSettings Shipped;
		FFileBytesGuard Costs(CalibratedCostsPath());
		if (!TestTrue(TEXT("prior INI files captured"), Shipped.Arm() && Costs.Capture() && Costs.Seed(SentinelText))) { return false; }
		bool bSeedExists = false;
		TArray<uint8> Seeded;
		ReadBytes(CalibratedCostsPath(), bSeedExists, Seeded);
		if (!TestTrue(TEXT("CalibratedCosts.ini made read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*CalibratedCostsPath(), true))) { return false; }
		AddExpectedErrorPlain(TEXT("SuperSLM.EstimateCosts: could not write"), EAutomationExpectedErrorFlags::Contains, 0);

		FSuperSLMLogCapture Log;
		FHookReading Hooks;
		int32 Runs = 0;
		bool bOk = RunEstimate(*this, EstimateCommand(Artifact, TEXT(" --write-ini")), Hooks, Runs);
		const FEstimateReport R = ParseEstimateReport(Log.Snapshot());
		bOk &= CheckCompleteEstimate(*this, R, Hash);
		bOk &= TestEqual(TEXT("a failed write is not reported written"), R.WroteIndex, (int32)INDEX_NONE);
		bOk &= FileBytesEqual(*this, CalibratedCostsPath(), bSeedExists, Seeded, TEXT("read-only CalibratedCosts.ini after a failed write"));
		bOk &= NoTemporarySiblings(*this, CalibratedCostsPath());
		bOk &= TestTrue(TEXT("CalibratedCosts.ini restored"), Costs.Restore());
		return bOk;
	}

	// The retired commands are gone, not aliased (D-SLM8111): no console object, and console input
	// naming either is not accepted.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateRetiredCommandsTest,
		"SuperSLM.D4.Estimate.RetiredCommandsAbsent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateRetiredCommandsTest::RunTest(const FString& Parameters)
	{
		bool bOk = true;
		for (const TCHAR* Name : { TEXT("SuperSLM.CalibrateCosts"), TEXT("SuperSLM.CaptureCallTimes") })
		{
			bOk &= TestNull(*FString::Printf(TEXT("%s has no console object"), Name), IConsoleManager::Get().FindConsoleObject(Name));
			bOk &= TestFalse(*FString::Printf(TEXT("console input %s is not accepted"), Name), IConsoleManager::Get().ProcessUserConsoleInput(Name, *GLog, nullptr));
		}
		return bOk;
	}

	// The faults a real run can meet, injected through the seam in mode 1 after mode 0 completes.
	// The enumerator values are the seam's (contract item 12): 1 TruncateArm, 2 InvalidAbiResult,
	// 3 NonfiniteCost, 4 SamplerFailure, 5 InterruptDuringMode1, 6 BatchedPremise.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultTruncatedArmTest,
		"SuperSLM.D4.Estimate.Fault.TruncatedArm", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultTruncatedArmTest::RunTest(const FString& Parameters) { return RunFaultCell(*this, TEXT("truncated arm"), 1); }

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultInvalidAbiTest,
		"SuperSLM.D4.Estimate.Fault.InvalidAbiResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultInvalidAbiTest::RunTest(const FString& Parameters) { return RunFaultCell(*this, TEXT("invalid ABI result"), 2); }

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultNonfiniteTest,
		"SuperSLM.D4.Estimate.Fault.NonfiniteCost", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultNonfiniteTest::RunTest(const FString& Parameters) { return RunFaultCell(*this, TEXT("nonfinite cost"), 3); }

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultSamplerTest,
		"SuperSLM.D4.Estimate.Fault.SamplerFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultSamplerTest::RunTest(const FString& Parameters) { return RunFaultCell(*this, TEXT("sampler failure"), 4); }

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultInterruptedTest,
		"SuperSLM.D4.Estimate.Fault.Interrupted", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultInterruptedTest::RunTest(const FString& Parameters) { return RunFaultCell(*this, TEXT("interrupted during mode 1"), 5); }

	// D-SLM8147: in mode 1 the seam raises each prompt_8 per-token median above the chunk-1
	// intercept before the fit, so the existing premise check fires on the production path.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMD4EstimateFaultBatchedPremiseTest,
		"SuperSLM.D4.Estimate.Fault.BatchedPremise", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	bool FSuperSLMD4EstimateFaultBatchedPremiseTest::RunTest(const FString& Parameters)
	{
		return RunFaultCell(*this, TEXT("batched premise"), 6, TEXT("incomplete estimate: batched prompt arm"));
	}
} // namespace SuperSLMD4EstimateTests

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
