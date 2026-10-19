// T-3102 (plan §10.3.1 item 5b "Cells", D-SLM8146, D-SLM8165): the ONE place tests touch
// FSuperSLMRuntimeConfig's decode mode, task count and thirteen cost fields, and
// FSuperSLMConfigureReport's resolved configuration.
//
// D4 changes those fifteen fields from plain values to TOptional (D-SLM8109) and adds
// FSuperSLMConfigureReport::Resolved (D-SLM8165). Every accessor below is a function template on the
// config or report type, so each member access is a dependent expression: a C++20
// requires-expression over it is false, not ill-formed, when the member has the other shape
// ([expr.prim.req]/5). The Tests module therefore compiles against either header, and a cell that
// needs the resolved report fails by name rather than on a compile error.
//
// Semantics, both shapes:
//  - SetMode / SetTasks / SetCost assign: the caller's explicit value (D-SLM8109: explicit means set).
//  - GetMode / GetTasks / GetCost return the value a cell EXPECTS Configure() to resolve, for an
//    oracle such as ExpectedPlannedJobMs: the field if it is set; otherwise 0/off, 4, and the
//    one-time profile of the expected effective mode (bParallelDecode && FinishParallelTasks > 1,
//    D-SLM8166), read from Fixtures/D4/d4_one_time_cost_profiles.json. This assumes no process
//    selection and no saved RuntimeSettings.ini/CalibratedCosts.ini profile applies, as in every
//    cell that uses these getters as its oracle.
//  - With the plain (pre-D4) header a field cannot be "unset"; a cost equal to the struct's own
//    default is read as unset. That is the pre-build expectation the kept oracles carry, and it is
//    why they go red before the build for the profile reason (T-3037 record §10.10 lists each).
//  - ReadResolved returns false when the report has no resolved configuration, or when it is unset.
//
// Only this header and Private/SuperSLMRuntimeResolve.cpp may call IsSet()/GetValue()/Get( on the
// fifteen fields (the source pin in ci/tests/test_d4_fix_round_pins.py).
#pragma once

#include "CoreMinimal.h"


#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperSLMJson.h"
#include "SuperSLMRuntimeConfig.h"

#include <limits>

namespace SuperSLMConfigAccess
{
	// The thirteen scheduler cost fields, under their config names (plan §10.3.1 item 5b's table).
	inline const TArray<FString>& CostFields()
	{
		static const TArray<FString> Fields = {
			TEXT("LayerCostMs"), TEXT("LayerCostPerPositionMs"), TEXT("PromptTokenCostMs"),
			TEXT("PromptTokenCostPerPositionMs"), TEXT("BatchedPromptTokenCostMs"),
			TEXT("BatchedPromptTokenCostPerPositionMs"), TEXT("FinishCostMs"), TEXT("ResetCostMs"),
			TEXT("AdoptCostMs"), TEXT("SaveCostMs"), TEXT("RestoreCostMs"), TEXT("PrefixBeginCostMs"),
			TEXT("PrefixReleaseCostMs") };
		return Fields;
	}

	inline bool IsPerPositionField(const FString& Field) { return Field.Contains(TEXT("PerPosition")); }

	using FCostProfile = TMap<FString, double>;

	inline FString OneTimeProfilesPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		checkf(Plugin.IsValid(), TEXT("SuperSLMUnreal plugin not found by IPluginManager"));
		return Plugin->GetBaseDir() / TEXT("Source/SuperSLMUnrealEditor/Private/Tests/Fixtures/D4/d4_one_time_cost_profiles.json");
	}

	// The committed one-time profiles (the Python witness recomputes the same file from the T-3050d
	// capture). Loaded once; a fixture that cannot be read leaves both profiles empty and
	// OneTimeProfileCost() returns NaN, which no price equals.
	inline bool LoadOneTimeProfiles(FCostProfile (&OutProfiles)[2], FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *OneTimeProfilesPath()))
		{
			OutError = FString::Printf(TEXT("could not read %s"), *OneTimeProfilesPath());
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!SuperSLMJson::TryReadObject(Text, Root, OutError)) { return false; }
		const TSharedPtr<FJsonObject>* Profiles = nullptr;
		if (!Root->TryGetObjectField(TEXT("profiles"), Profiles) || Profiles == nullptr)
		{
			OutError = TEXT("fixture has no 'profiles' object");
			return false;
		}
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			const TSharedPtr<FJsonObject>* P = nullptr;
			if (!(*Profiles)->TryGetObjectField(FString::FromInt(Mode), P) || P == nullptr)
			{
				OutError = FString::Printf(TEXT("fixture has no profile for mode %d"), Mode);
				return false;
			}
			OutProfiles[Mode].Reset();
			for (const FString& F : CostFields())
			{
				double V = 0.0;
				if (!(*P)->TryGetNumberField(F, V))
				{
					OutError = FString::Printf(TEXT("fixture mode %d lacks %s"), Mode, *F);
					return false;
				}
				OutProfiles[Mode].Add(F, V);
			}
		}
		return true;
	}

	inline double OneTimeProfileCost(bool bEffectiveMode, const FString& Field)
	{
		static FCostProfile Profiles[2];
		static bool bLoaded = false;
		if (!bLoaded)
		{
			FString Error;
			if (!LoadOneTimeProfiles(Profiles, Error)) { UE_LOG(LogTemp, Error, TEXT("SuperSLMConfigAccess: %s"), *Error); }
			bLoaded = true;
		}
		const double* V = Profiles[bEffectiveMode ? 1 : 0].Find(Field);
		return V ? *V : std::numeric_limits<double>::quiet_NaN();
	}

	// A reference to one cost member, in whichever shape the config type has.
	template <typename TConfig>
	decltype(auto) CostRef(TConfig& Config, const FString& F)
	{
		if (F == TEXT("LayerCostMs")) { return (Config.LayerCostMs); }
		if (F == TEXT("LayerCostPerPositionMs")) { return (Config.LayerCostPerPositionMs); }
		if (F == TEXT("PromptTokenCostMs")) { return (Config.PromptTokenCostMs); }
		if (F == TEXT("PromptTokenCostPerPositionMs")) { return (Config.PromptTokenCostPerPositionMs); }
		if (F == TEXT("BatchedPromptTokenCostMs")) { return (Config.BatchedPromptTokenCostMs); }
		if (F == TEXT("BatchedPromptTokenCostPerPositionMs")) { return (Config.BatchedPromptTokenCostPerPositionMs); }
		if (F == TEXT("FinishCostMs")) { return (Config.FinishCostMs); }
		if (F == TEXT("ResetCostMs")) { return (Config.ResetCostMs); }
		if (F == TEXT("AdoptCostMs")) { return (Config.AdoptCostMs); }
		if (F == TEXT("SaveCostMs")) { return (Config.SaveCostMs); }
		if (F == TEXT("RestoreCostMs")) { return (Config.RestoreCostMs); }
		if (F == TEXT("PrefixBeginCostMs")) { return (Config.PrefixBeginCostMs); }
		checkf(F == TEXT("PrefixReleaseCostMs"), TEXT("SuperSLMConfigAccess: unknown cost field %s"), *F);
		return (Config.PrefixReleaseCostMs);
	}

	// --- Setters: the caller's explicit value, in either shape ---

	template <typename TConfig> void SetMode(TConfig& Config, bool bOn) { Config.bParallelDecode = bOn; }
	template <typename TConfig> void SetTasks(TConfig& Config, int32 Tasks) { Config.FinishParallelTasks = Tasks; }
	template <typename TConfig> void SetCost(TConfig& Config, const FString& Field, double Value) { CostRef(Config, Field) = Value; }
	template <typename TConfig> void SetCosts(TConfig& Config, const FCostProfile& Costs)
	{
		for (const TPair<FString, double>& P : Costs) { SetCost(Config, P.Key, P.Value); }
	}

	// --- Getters: the value a cell expects Configure() to resolve ---

	template <typename TConfig> bool GetMode(const TConfig& Config)
	{
		if constexpr (requires { Config.bParallelDecode.IsSet(); })
		{
			return Config.bParallelDecode.IsSet() && Config.bParallelDecode.GetValue();
		}
		else
		{
			return static_cast<bool>(Config.bParallelDecode);
		}
	}

	template <typename TConfig> int32 GetTasks(const TConfig& Config)
	{
		if constexpr (requires { Config.FinishParallelTasks.IsSet(); })
		{
			return Config.FinishParallelTasks.IsSet() ? Config.FinishParallelTasks.GetValue() : 4;
		}
		else
		{
			return Config.FinishParallelTasks;
		}
	}

	// The expected effective decode mode (D-SLM8166): the matvec bit rides on the finish hook.
	template <typename TConfig> bool GetEffectiveMode(const TConfig& Config) { return GetMode(Config) && GetTasks(Config) > 1; }

	template <typename TConfig> double GetCost(const TConfig& Config, const FString& Field)
	{
		const auto& Member = CostRef(Config, Field);
		if constexpr (requires { Member.IsSet(); })
		{
			return Member.IsSet() ? Member.GetValue() : OneTimeProfileCost(GetEffectiveMode(Config), Field);
		}
		else
		{
			const TConfig Default{};
			const double ShippedDefault = CostRef(Default, Field);
			return Member != ShippedDefault ? Member : OneTimeProfileCost(GetEffectiveMode(Config), Field);
		}
	}

	template <typename TConfig> FCostProfile GetCosts(const TConfig& Config)
	{
		FCostProfile Out;
		for (const FString& F : CostFields()) { Out.Add(F, GetCost(Config, F)); }
		return Out;
	}

	// --- The resolved configuration a Configure() report carries (D-SLM8165) ---

	struct FResolvedView
	{
		bool bModeRequested = false;
		bool bModeEffective = false;
		int32 ParallelTasks = -1;
		FCostProfile Costs;
		FString CostProfile; // "caller", "saved-estimate" or "mode-default", as the Configure line prints it
		int32 CallerCostFields = -1;
	};

	template <typename TResolved> void ReadResolvedValues(const TResolved& R, FResolvedView& Out)
	{
		Out.bModeRequested = R.bDecodeModeRequested;
		Out.bModeEffective = R.bDecodeModeEffective;
		Out.ParallelTasks = R.ParallelTasks;
		for (const FString& F : CostFields()) { Out.Costs.Add(F, CostRef(R, F)); }
		// ESuperSLMCostProfile's enumerators in plan order: Caller, SavedEstimate, ModeDefault. Named
		// by value so this header compiles before the enum exists.
		const int32 Profile = static_cast<int32>(R.CostProfile);
		Out.CostProfile = Profile == 0 ? TEXT("caller") : Profile == 1 ? TEXT("saved-estimate") : Profile == 2 ? TEXT("mode-default") : TEXT("?");
		Out.CallerCostFields = R.CallerCostFields;
	}

	template <typename TReport> bool ReadResolved(const TReport& Report, FResolvedView& Out)
	{
		if constexpr (requires { Report.Resolved.IsSet(); Report.Resolved.GetValue().ParallelTasks; })
		{
			if (!Report.Resolved.IsSet()) { return false; }
			ReadResolvedValues(Report.Resolved.GetValue(), Out);
			return true;
		}
		else
		{
			return false;
		}
	}

	inline const TCHAR* NoResolvedText() { return TEXT("missing feature: FSuperSLMConfigureReport has no resolved configuration"); }
}

