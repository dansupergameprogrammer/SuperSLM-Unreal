// Paging plan cell 7.2: real scheduler selection of measured budget lifecycle defaults.
// Expected prices are read from the separately recomputed raw-call fixture, never the resolver.
// Both actual artifacts, legacy/budget mode, effective decode modes, saved estimates and explicit
// caller overrides are exercised through real lifecycle jobs. Keeping legacy defaults in budget
// mode, changing legacy defaults, ignoring a saved estimate or ignoring a caller each fails here.
#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && PLATFORM_WINDOWS

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "Fixtures/SuperSLMD4Fixtures.h"

namespace SuperSLMPagingLifecyclePricesTests
{
	using namespace SuperSLMPagingFixtures;
	using namespace SuperSLMD4Fixtures;

	const TCHAR* const LifecycleFields[] = { TEXT("AdoptCostMs"), TEXT("RestoreCostMs"), TEXT("PrefixBeginCostMs") };

	bool ReadBudgetProfile(FAutomationTestBase& T, int64 Cap, int32 Mode, FCostProfile& Out)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		if (!T.TestTrue(TEXT("plugin exists for independent fixture"), Plugin.IsValid())) { return false; }
		const FString Path = Plugin->GetBaseDir() / TEXT("Source/SuperSLMUnrealEditor/Private/Tests/Fixtures/Paging/budget_lifecycle_cost_profiles.json");
		FString Text, Error;
		TSharedPtr<FJsonObject> Root;
		if (!T.TestTrue(TEXT("separately recomputed K4 fixture is readable"), FFileHelper::LoadFileToString(Text, *Path)) ||
			!T.TestTrue(TEXT("K4 fixture is valid JSON"), SuperSLMJson::TryReadObject(Text, Root, Error))) { return false; }
		const TSharedPtr<FJsonObject>* Profiles = nullptr;
		const TSharedPtr<FJsonObject>* Artifact = nullptr;
		const TSharedPtr<FJsonObject>* Profile = nullptr;
		if (!T.TestTrue(TEXT("fixture has the actual cap and effective mode"),
			Root->TryGetObjectField(TEXT("profiles"), Profiles) && Profiles &&
			(*Profiles)->TryGetObjectField(FString::Printf(TEXT("%lld"), Cap), Artifact) && Artifact &&
			(*Artifact)->TryGetObjectField(FString::FromInt(Mode), Profile) && Profile)) { return false; }
		for (const TCHAR* Field : LifecycleFields)
		{
			double Value = 0.0;
			if (!T.TestTrue(*FString::Printf(TEXT("fixture %s is finite and positive"), Field),
				(*Profile)->TryGetNumberField(Field, Value) && FMath::IsFinite(Value) && Value > 0.0)) { return false; }
			Out.Add(Field, Value);
		}
		return true;
	}

	FString SavedProfileText(const FString& Hash, int32 Mode, int32 Tasks, const FCostProfile& Profile)
	{
		FString Text = FString::Printf(TEXT("[SuperSLM.PagingPriceSelection]\nArtifactHash=%s\nbParallelDecode=%d\nFinishParallelTasks=%d\n"), *Hash, Mode, Tasks);
		for (const FString& Field : CostFields()) { Text += FString::Printf(TEXT("%s=%.17g\n"), *Field, Profile.FindChecked(Field)); }
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBudgetLifecyclePricesTest,
	"SuperSLM.Paging.Cpu.Configure.BudgetLifecyclePrices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBudgetLifecyclePricesTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPagingLifecyclePricesTests;
	FCostProfile Legacy[2];
	FString Error;
	if (!TestTrue(TEXT("independent legacy profiles load"), LoadOneTimeProfiles(Legacy, Error))) { return false; }
	FFileBytesGuard Settings{ RuntimeSettingsPath() }, Costs{ CalibratedCostsPath() };
	if (!TestTrue(TEXT("both prior INI files captured"), Settings.Capture() && Costs.Capture()) ||
		!TestTrue(TEXT("settings sentinel seeded"), Settings.Seed(TEXT("; lifecycle price selection settings sentinel\n")))) { return false; }
	bool bOk = true;
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	for (const EPagingArtifact Artifact : { EPagingArtifact::AEx05B, EPagingArtifact::Shipped15B })
	{
		FPagingRig Rig;
		if (!Rig.Init(*this, Artifact)) { return false; }
		FString Path, Reason, Hash;
		if (!TestTrue(TEXT("actual artifact is resolved and independently hashed"),
			ResolveArtifact(Artifact, Path, Reason) && ReadEmbeddedArtifactHashHex(Path, Hash))) { return false; }
		// The third selection requests mode 1 with one task: no hook means effective mode 0.
		for (int32 Selection = 0; Selection < 3; ++Selection)
		{
			const bool bRequested = Selection != 0;
			const int32 Tasks = Selection == 2 ? 1 : 4;
			const int32 Mode = Selection == 1 ? 1 : 0;
			FCostProfile Budget;
			if (!ReadBudgetProfile(*this, ExpectedCap(Artifact), Mode, Budget)) { return false; }
			for (const int32 SequenceBudget : { 0, 512 })
			{
				// Defaults, matching saved, caller over saved, invalid saved field, and a finite
				// saved estimate whose composed scheduling cost exceeds the admitted tick horizon.
				for (int32 Source = 0; Source < 5; ++Source)
				{
					const FString Label = FString::Printf(TEXT("cap=%lld request=%d tasks=%d budget=%d source=%d"), ExpectedCap(Artifact), bRequested, Tasks, SequenceBudget, Source);
					FSuperSLMRuntimeConfig Config = MakeConfig(2, SequenceBudget, 1, 1);
					Config.bParallelDecode = bRequested;
					Config.FinishParallelTasks = Tasks;
					FCostProfile Expected = Legacy[Mode];
					if (SequenceBudget > 0)
					{
						for (const TPair<FString, double>& Entry : Budget) { Expected.Add(Entry.Key, Entry.Value); }
					}
					FCostProfile Saved = Legacy[Mode];
					Saved.Add(TEXT("AdoptCostMs"), 17.125);
					Saved.Add(TEXT("RestoreCostMs"), 19.25);
					Saved.Add(TEXT("PrefixBeginCostMs"), 23.5);
					if (Source == 3) { Saved.Add(TEXT("AdoptCostMs"), 0.0); }
					if (Source == 4) { Saved.Add(TEXT("AdoptCostMs"), 1.0e50); }
					if (!TestTrue(TEXT("cost sentinel or matching estimate seeded"), Costs.Seed(Source == 0
						? TEXT("; lifecycle price selection cost sentinel\n") : SavedProfileText(Hash, Mode, Tasks, Saved)))) { return false; }
					if (Source == 1 || Source == 2) { Expected = Saved; }
					if (Source == 2)
					{
						Config.AdoptCostMs = 29.125;
						Config.RestoreCostMs = 31.25;
						Config.PrefixBeginCostMs = 37.5;
						Expected.Add(TEXT("AdoptCostMs"), 29.125);
						Expected.Add(TEXT("RestoreCostMs"), 31.25);
						Expected.Add(TEXT("PrefixBeginCostMs"), 37.5);
					}
					TArray<uint8> SettingsBefore, CostsBefore, After;
					if (!TestTrue(TEXT("seeded bytes captured before real Configure"),
						FFileHelper::LoadFileToArray(SettingsBefore, *Settings.Path) && FFileHelper::LoadFileToArray(CostsBefore, *Costs.Path))) { return false; }
					const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Config);
					if (!TestEqual(*FString::Printf(TEXT("%s Configure succeeds: %s"), *Label, *Report.Message),
						(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success)) { return false; }
					SuperSLMConfigAccess::FResolvedView Resolved;
					if (!TestTrue(TEXT("actual resolved report exists"), SuperSLMConfigAccess::ReadResolved(Report, Resolved))) { return false; }
					bOk &= TestEqual(TEXT("requested mode"), Resolved.bModeRequested, bRequested);
					bOk &= TestEqual(TEXT("effective mode"), Resolved.bModeEffective, Mode == 1);
					bOk &= TestEqual(TEXT("actual task selection"), Resolved.ParallelTasks, Tasks);
					bOk &= TestEqual(TEXT("cost source priority and saved fallback"), Resolved.CostProfile, FString(Source == 1 || Source == 2 ? TEXT("saved-estimate") : TEXT("mode-default")));
					bOk &= TestEqual(TEXT("explicit caller field count"), Resolved.CallerCostFields, Source == 2 ? 3 : 0);
					bOk &= TestEqual(TEXT("real backend selected budget mode"), Rig.Cpu->GetKvPageStats().bPagedMode, SequenceBudget > 0);
					if (SequenceBudget > 0 && !ReadGeometry(*this, Rig)) { return false; }
					TArray<FSuperSLMWorkerJobReport> Rows;
					bOk &= DriveEveryJobKind(*this, *Rig.Cpu, *Rig.Model, Rows, *Label);
					bOk &= CheckRowsPricedBy(*this, Rows, Expected, *Label);
					for (const TCHAR* Field : LifecycleFields)
					{
						bOk &= TestEqual(*FString::Printf(TEXT("%s exact resolved %s"), *Label, Field), Resolved.Costs.FindChecked(Field), Expected.FindChecked(Field));
					}
					int32 Seen[3] = {};
					for (const FSuperSLMWorkerJobReport& Row : Rows)
					{
						const int32 Index = Row.Kind == ESuperSLMWorkerJobKind::Adopt ? 0
							: Row.Kind == ESuperSLMWorkerJobKind::Restore ? 1 : Row.Kind == ESuperSLMWorkerJobKind::PrefixBegin ? 2 : -1;
						if (Index < 0) { continue; }
						++Seen[Index];
						bOk &= TestEqual(*FString::Printf(TEXT("%s exact actual job %s"), *Label, LifecycleFields[Index]), Row.PlannedJobMs, Expected.FindChecked(LifecycleFields[Index]));
					}
					for (const int32 Count : Seen) { bOk &= TestTrue(TEXT("each lifecycle price was actually used by a delivered job"), Count > 0); }
					bOk &= TestTrue(TEXT("Configure and work preserve seeded settings bytes"), FFileHelper::LoadFileToArray(After, *Settings.Path) && After == SettingsBefore);
					bOk &= TestTrue(TEXT("Configure and work preserve seeded cost bytes"), FFileHelper::LoadFileToArray(After, *Costs.Path) && After == CostsBefore);
				}
			}
		}
	}
	bOk &= TestTrue(TEXT("original settings bytes restored and verified"), Settings.Restore());
	bOk &= TestTrue(TEXT("original cost bytes restored and verified"), Costs.Restore());
	return bOk;
}

#endif
