#include "SuperSLMRuntimeResolve.h"

#include "Core/SuperSLMCacheChunkPricing.h"
#include "Core/SuperSLMInSituPrefixPrefill.h"
#include "SuperSLMConfigureTestAccess.h"
#include "SuperSLMFinishHook.h"
#include "SuperSLMLog.h"
#include "SuperSLMPrefillPlanning.h"
#include "SuperSLMRuntimeRegistry.h"

#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// Decode-threading D4 (plan §10.3.1 item 5b). See SuperSLMRuntimeResolve.h.

namespace SuperSLMRuntimeResolve
{
	namespace
	{
		const FCostField GCostFields[kCostFieldCount] = {
			{ TEXT("LayerCostMs"), &FSuperSLMResolvedCpuConfig::LayerCostMs, false },
			{ TEXT("LayerCostPerPositionMs"), &FSuperSLMResolvedCpuConfig::LayerCostPerPositionMs, true },
			{ TEXT("PromptTokenCostMs"), &FSuperSLMResolvedCpuConfig::PromptTokenCostMs, false },
			{ TEXT("PromptTokenCostPerPositionMs"), &FSuperSLMResolvedCpuConfig::PromptTokenCostPerPositionMs, true },
			{ TEXT("BatchedPromptTokenCostMs"), &FSuperSLMResolvedCpuConfig::BatchedPromptTokenCostMs, false },
			{ TEXT("BatchedPromptTokenCostPerPositionMs"), &FSuperSLMResolvedCpuConfig::BatchedPromptTokenCostPerPositionMs, true },
			{ TEXT("FinishCostMs"), &FSuperSLMResolvedCpuConfig::FinishCostMs, false },
			{ TEXT("ResetCostMs"), &FSuperSLMResolvedCpuConfig::ResetCostMs, false },
			{ TEXT("AdoptCostMs"), &FSuperSLMResolvedCpuConfig::AdoptCostMs, false },
			{ TEXT("SaveCostMs"), &FSuperSLMResolvedCpuConfig::SaveCostMs, false },
			{ TEXT("RestoreCostMs"), &FSuperSLMResolvedCpuConfig::RestoreCostMs, false },
			{ TEXT("PrefixBeginCostMs"), &FSuperSLMResolvedCpuConfig::PrefixBeginCostMs, false },
			{ TEXT("PrefixReleaseCostMs"), &FSuperSLMResolvedCpuConfig::PrefixReleaseCostMs, false },
		};

		// The caller's optional member for each entry of GCostFields, in the same order.
		TOptional<double> FSuperSLMRuntimeConfig::* const GCallerFields[kCostFieldCount] = {
			&FSuperSLMRuntimeConfig::LayerCostMs,
			&FSuperSLMRuntimeConfig::LayerCostPerPositionMs,
			&FSuperSLMRuntimeConfig::PromptTokenCostMs,
			&FSuperSLMRuntimeConfig::PromptTokenCostPerPositionMs,
			&FSuperSLMRuntimeConfig::BatchedPromptTokenCostMs,
			&FSuperSLMRuntimeConfig::BatchedPromptTokenCostPerPositionMs,
			&FSuperSLMRuntimeConfig::FinishCostMs,
			&FSuperSLMRuntimeConfig::ResetCostMs,
			&FSuperSLMRuntimeConfig::AdoptCostMs,
			&FSuperSLMRuntimeConfig::SaveCostMs,
			&FSuperSLMRuntimeConfig::RestoreCostMs,
			&FSuperSLMRuntimeConfig::PrefixBeginCostMs,
			&FSuperSLMRuntimeConfig::PrefixReleaseCostMs,
		};

		// Plan §10.3.1 item 5b's one-time measured profiles, in GCostFields order: the six T-3050d
		// per-call capture blocks on A-EX at cap 4096, FinishParallelTasks 4, one Zen 2 desktop;
		// per mode and arm/depth the median within each block, then the median of the three block
		// medians, the two line fits, each value rounded upward (D-SLM8108, D-SLM8112, D-SLM8113).
		// Reasonable measured starting defaults, not certified costs or accuracy bounds.
		const double GModeProfiles[2][kCostFieldCount] = {
			// Mode 0 (off)
			{ 1.397, 0.000186, 33.583, 0.004547, 15.023, 0.004791, 5.836, 2.369, 2.648, 2.691, 4.644, 2.261, 2.247 },
			// Mode 1 (SSLM_PARALLEL_FOR_MATVEC)
			{ 0.906, 0.000254, 22.108, 0.005209, 15.682, 0.004437, 5.929, 2.397, 2.731, 2.744, 4.766, 2.425, 2.440 },
		};

		// Paging K4, actual cap-4096 A-EX and cap-32768 shipped 1.5B captures. Each entry is
		// adopt of 1,000 tokens, full-cap restore, prefix Begin: median of three 50-call block
		// medians, rounded upward to .001 ms (D-SLM8113). Raw inputs and separately recomputed
		// fixture: Fixtures/Paging/LifecyclePricing and Fixtures/Paging/budget_lifecycle_cost_profiles.json.
		// The artifact's actual cap selects the small/large starting profile; these are estimates,
		// not latency bounds. Caller and matching saved estimates retain priority for every field.
		const double GBudgetLifecycleProfiles[2][2][3] = {
			{ { 0.009, 3.576, 0.001 }, { 0.008, 3.558, 0.001 } }, // cap <= 4096, mode 0 / 1
			{ { 0.020, 90.795, 0.002 }, { 0.020, 90.442, 0.002 } }, // larger cap, mode 0 / 1
		};

		// This process's selection (SuperSLM.SetDecodeMode / SuperSLM.SetFinishParallelTasks).
		FCriticalSection GSelectionLock;
		TOptional<bool> GSelectedDecodeMode;
		TOptional<int32> GSelectedParallelTasks;

		const TCHAR* const kSettingsSection = TEXT("[SuperSLM.RuntimeSettings]");
		const TCHAR* const kModeKey = TEXT("bParallelDecode");
		const TCHAR* const kTasksKey = TEXT("FinishParallelTasks");
		const TCHAR* const kHashKey = TEXT("ArtifactHash");

		bool ParseMode(const FString& Text, bool& OutMode)
		{
			if (Text == TEXT("0")) { OutMode = false; return true; }
			if (Text == TEXT("1")) { OutMode = true; return true; }
			return false;
		}

		// A task count written as decimal digits only, 0-256.
		bool ParseTasks(const FString& Text, int32& OutTasks)
		{
			if (Text.IsEmpty() || Text.Len() > 3)
			{
				return false;
			}
			for (const TCHAR Ch : Text)
			{
				if (!FChar::IsDigit(Ch))
				{
					return false;
				}
			}
			const int32 Value = FCString::Atoi(*Text);
			if (!SuperSLMFinishHook::IsValidTaskCount(Value))
			{
				return false;
			}
			OutTasks = Value;
			return true;
		}

		// RuntimeSettings.ini's saved mode and task count, each only when present and valid. A
		// present but invalid value is not used, with one Warning naming the file and the key.
		void ReadSavedSettings(TOptional<bool>& OutMode, TOptional<int32>& OutTasks)
		{
			const FString Path = RuntimeSettingsPath();
			FString Text;
			if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(Text, *Path))
			{
				return;
			}
			for (const FIniSection& Section : ParseIniSections(Text))
			{
				if (!Section.Keys.Contains(kModeKey) && !Section.Keys.Contains(kTasksKey))
				{
					continue;
				}
				TArray<FString> Bad;
				if (const FString* Mode = Section.Keys.Find(kModeKey))
				{
					bool bMode = false;
					if (ParseMode(*Mode, bMode)) { OutMode = bMode; } else { Bad.Add(FString::Printf(TEXT("%s=%s is not 0 or 1"), kModeKey, **Mode)); }
				}
				if (const FString* Tasks = Section.Keys.Find(kTasksKey))
				{
					int32 Value = 0;
					if (ParseTasks(*Tasks, Value)) { OutTasks = Value; } else { Bad.Add(FString::Printf(TEXT("%s=%s is not a whole number in [0, %d]"), kTasksKey, **Tasks, SuperSLMFinishHook::MaxFinishParallelTasks)); }
				}
				if (Bad.Num() > 0)
				{
					UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: CPU Configure: %s: %s; that setting is not used."), *Path, *FString::Join(Bad, TEXT("; ")));
				}
				return; // the first settings section is the file's
			}
		}

		// The thirteen costs of a saved-estimate section, when all are present and in domain. When
		// any is not, OutProblems names each such key.
		bool ReadSavedCosts(const FIniSection& Section, double (&OutCosts)[kCostFieldCount], TArray<FString>& OutProblems)
		{
			for (int32 I = 0; I < kCostFieldCount; ++I)
			{
				const FCostField& Field = GCostFields[I];
				const FString* Text = Section.Keys.Find(Field.Name);
				double Value = 0.0;
				if (Text == nullptr)
				{
					OutProblems.Add(FString::Printf(TEXT("%s is missing"), Field.Name));
				}
				else if (!ParseFiniteDouble(*Text, Value) || !IsInFieldDomain(Field, Value))
				{
					OutProblems.Add(FString::Printf(TEXT("%s=%s is not a finite, %s"), Field.Name, **Text,
						Field.bPerPosition ? TEXT("non-negative number of milliseconds per position") : TEXT("positive number of milliseconds")));
				}
				else
				{
					OutCosts[I] = Value;
				}
			}
			return OutProblems.Num() == 0;
		}

		// The first CalibratedCosts.ini section labelled with this artifact hash, effective mode and
		// task count, or null. Labels that do not parse never match.
		const FIniSection* FindSavedEstimate(const TArray<FIniSection>& Sections, const FString& HashHex, bool bEffectiveMode, int32 Tasks)
		{
			for (const FIniSection& Section : Sections)
			{
				const FString* Hash = Section.Keys.Find(kHashKey);
				const FString* Mode = Section.Keys.Find(kModeKey);
				const FString* TaskText = Section.Keys.Find(kTasksKey);
				bool bMode = false;
				int32 SectionTasks = -1;
				if (Hash == nullptr || Mode == nullptr || TaskText == nullptr || !ParseMode(*Mode, bMode) || !ParseTasks(*TaskText, SectionTasks))
				{
					continue;
				}
				if (Hash->Equals(HashHex, ESearchCase::IgnoreCase) && bMode == bEffectiveMode && SectionTasks == Tasks)
				{
					return &Section;
				}
			}
			return nullptr;
		}

		// Whether the configuration's call shape lets the composed cost-domain check run at all.
		// When it does not, Configure() refuses on the shape first and never reaches the check.
		bool ShapeAdmitsDomainCheck(const FSuperSLMRuntimeConfig& Config)
		{
			return Config.BlockCount > 0 && Config.MaxLayerBudget > 0 && Config.MaxPrefillChunkBudget > 0
				&& FMath::IsFinite(Config.TickBudgetMs) && Config.TickBudgetMs > 0.0;
		}

		// Writes the setting the command chose. The other setting is written only when the file
		// already holds a valid value for it, which is kept; a value nobody chose is never saved, so a
		// saved key always means a choice and a later change of the shipped value reaches it.
		void SaveSettingsToDisk(const TCHAR* Command, bool bSavingMode)
		{
			TOptional<bool> Mode;
			TOptional<int32> Tasks;
			ReadSavedSettings(Mode, Tasks);
			{
				FScopeLock Lock(&GSelectionLock);
				if (bSavingMode) { Mode = GSelectedDecodeMode; } else { Tasks = GSelectedParallelTasks; }
			}
			const FString Path = RuntimeSettingsPath();
			FString Text = FString::Printf(TEXT(
				"; SuperSLM CPU decode settings, saved by SuperSLM.SetDecodeMode or SuperSLM.SetFinishParallelTasks with --save.\n"
				"; The next CPU Configure() uses them for any field the caller's FSuperSLMRuntimeConfig leaves unset,\n"
				"; unless this process has selected another value since it started. A setting not listed here takes\n"
				"; its shipped value.\n"
				"%s\n"), kSettingsSection);
			FString Saved;
			if (Mode.IsSet())
			{
				Text += FString::Printf(TEXT("%s=%d\n"), kModeKey, Mode.GetValue() ? 1 : 0);
				Saved += FString::Printf(TEXT(" %s=%d"), kModeKey, Mode.GetValue() ? 1 : 0);
			}
			if (Tasks.IsSet())
			{
				Text += FString::Printf(TEXT("%s=%d\n"), kTasksKey, Tasks.GetValue());
				Saved += FString::Printf(TEXT(" %s=%d"), kTasksKey, Tasks.GetValue());
			}
			FString Error;
			if (WriteTextFileAtomically(Path, Text, Error))
			{
				UE_LOG(LogSuperSLM, Display, TEXT("%s: saved%s to %s."), Command, *Saved, *Path);
			}
			else
			{
				UE_LOG(LogSuperSLM, Error, TEXT("%s: could not save %s (%s); the prior file is unchanged, and the selection applies to this process only."), Command, *Path, *Error);
			}
		}

		void RunSetDecodeMode(const TArray<FString>& Args)
		{
			bool bMode = false;
			const bool bSave = Args.Num() == 2 && Args[1] == TEXT("--save");
			if ((Args.Num() != 1 && !bSave) || !ParseMode(Args[0], bMode))
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Usage: SuperSLM.SetDecodeMode 0|1 [--save] -- 1 splits CPU one-row decode projections through the finish hook (SSLM_PARALLEL_FOR_MATVEC) from the next CPU Configure(); --save also keeps the choice in Saved/SuperSLM/RuntimeSettings.ini."));
				return;
			}
			{
				FScopeLock Lock(&GSelectionLock);
				GSelectedDecodeMode = bMode;
			}
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.SetDecodeMode: the next CPU Configure() resolves bParallelDecode=%d when the caller leaves it unset; a configuration already made keeps its mode."), bMode ? 1 : 0);
			if (bSave)
			{
				SaveSettingsToDisk(TEXT("SuperSLM.SetDecodeMode"), /*bSavingMode*/ true);
			}
		}

		void RunSetFinishParallelTasks(const TArray<FString>& Args)
		{
			int32 Tasks = 0;
			const bool bSave = Args.Num() == 2 && Args[1] == TEXT("--save");
			if (Args.Num() < 1 || (Args.Num() != 1 && !bSave) || !ParseTasks(Args[0], Tasks))
			{
				UE_LOG(LogSuperSLM, Error, TEXT("Usage: SuperSLM.SetFinishParallelTasks <0..%d> [--save] -- the most tasks the CPU finish hook splits one token finish or decode projection into, from the next CPU Configure() (0 or 1 installs no hook); --save also keeps it in Saved/SuperSLM/RuntimeSettings.ini."),
					SuperSLMFinishHook::MaxFinishParallelTasks);
				return;
			}
			{
				FScopeLock Lock(&GSelectionLock);
				GSelectedParallelTasks = Tasks;
			}
			UE_LOG(LogSuperSLM, Display, TEXT("SuperSLM.SetFinishParallelTasks: the next CPU Configure() resolves FinishParallelTasks=%d when the caller leaves it unset; a configuration already made keeps its task count."), Tasks);
			if (bSave)
			{
				SaveSettingsToDisk(TEXT("SuperSLM.SetFinishParallelTasks"), /*bSavingMode*/ false);
			}
		}

		static FAutoConsoleCommand GSetDecodeModeCommand(
			TEXT("SuperSLM.SetDecodeMode"),
			TEXT("SuperSLM.SetDecodeMode 0|1 [--save] -- the CPU decode mode the next Configure() uses when the caller's config leaves it unset. 1 splits one-row decode projections through the finish hook; 0, the shipped mode, leaves that off; --save keeps it in Saved/SuperSLM/RuntimeSettings.ini, which later sessions load. Editor and packaged."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&RunSetDecodeMode));

		static FAutoConsoleCommand GSetFinishParallelTasksCommand(
			TEXT("SuperSLM.SetFinishParallelTasks"),
			TEXT("SuperSLM.SetFinishParallelTasks <0..256> [--save] -- the CPU finish hook's task count the next Configure() uses when the caller's config leaves it unset (0 or 1 installs no hook); --save keeps it in Saved/SuperSLM/RuntimeSettings.ini. Editor and packaged."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&RunSetFinishParallelTasks));
	}

	const TCHAR* PerChunkTokenCostKey(int32 Chunk)
	{
		static const TCHAR* const Keys[8] = { nullptr, nullptr,
			TEXT("PromptChunk2TokenCostMs"), TEXT("PromptChunk3TokenCostMs"), TEXT("PromptChunk4TokenCostMs"),
			TEXT("PromptChunk5TokenCostMs"), TEXT("PromptChunk6TokenCostMs"), TEXT("PromptChunk7TokenCostMs") };
		return Chunk >= 2 && Chunk <= 7 ? Keys[Chunk] : nullptr;
	}

	const TCHAR* PerChunkTokenCostPerPositionKey(int32 Chunk)
	{
		static const TCHAR* const Keys[8] = { nullptr, nullptr,
			TEXT("PromptChunk2TokenCostPerPositionMs"), TEXT("PromptChunk3TokenCostPerPositionMs"), TEXT("PromptChunk4TokenCostPerPositionMs"),
			TEXT("PromptChunk5TokenCostPerPositionMs"), TEXT("PromptChunk6TokenCostPerPositionMs"), TEXT("PromptChunk7TokenCostPerPositionMs") };
		return Chunk >= 2 && Chunk <= 7 ? Keys[Chunk] : nullptr;
	}

	SuperSLMPrefillPlanning::FPerChunkPromptPrices PerChunkPromptPrices(const FSuperSLMResolvedCpuConfig& Costs)
	{
		SuperSLMPrefillPlanning::FPerChunkPromptPrices Out;
		for (int32 C = 2; C <= 7; ++C)
		{
			Out.ByChunk[C] = SuperSLMPrefillPlanning::FPromptPrice{ Costs.PerChunkPromptTokenCostMs[C], Costs.PerChunkPromptTokenCostPerPositionMs[C] };
		}
		return Out;
	}

	TConstArrayView<FCostField> CostFields()
	{
		return MakeArrayView(GCostFields);
	}

	bool IsInFieldDomain(const FCostField& Field, double Value)
	{
		return FMath::IsFinite(Value) && (Field.bPerPosition ? Value >= 0.0 : Value > 0.0);
	}

	void ApplyModeProfile(bool bEffectiveMode, FSuperSLMResolvedCpuConfig& Out)
	{
		const double (&Profile)[kCostFieldCount] = GModeProfiles[bEffectiveMode ? 1 : 0];
		for (int32 I = 0; I < kCostFieldCount; ++I)
		{
			Out.*(GCostFields[I].Member) = Profile[I];
		}
	}

	FString RuntimeSettingsPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/RuntimeSettings.ini"));
	}

	FString CalibratedCostsPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini"));
	}

	void ResolveUncalledSettings(bool& OutDecodeModeRequested, int32& OutParallelTasks)
	{
		TOptional<bool> Mode;
		TOptional<int32> Tasks;
		{
			FScopeLock Lock(&GSelectionLock);
			Mode = GSelectedDecodeMode;
			Tasks = GSelectedParallelTasks;
		}
		if (!Mode.IsSet() || !Tasks.IsSet())
		{
			TOptional<bool> SavedMode;
			TOptional<int32> SavedTasks;
			ReadSavedSettings(SavedMode, SavedTasks);
			if (!Mode.IsSet()) { Mode = SavedMode; }
			if (!Tasks.IsSet()) { Tasks = SavedTasks; }
		}
		OutDecodeModeRequested = Mode.Get(kShippedDecodeMode);
		OutParallelTasks = Tasks.Get(kShippedParallelTasks);
	}

	FSuperSLMResolvedCpuConfig Resolve(const FSuperSLMRuntimeConfig& Config, const SuperSLMRuntime::FModelBytes* Model)
	{
		FSuperSLMResolvedCpuConfig Out;

		// Mode and task count, each on its own: the caller's value if set, else the process
		// selection, else RuntimeSettings.ini, else off and 4.
		if (Config.bParallelDecode.IsSet() && Config.FinishParallelTasks.IsSet())
		{
			Out.bDecodeModeRequested = Config.bParallelDecode.GetValue();
			Out.ParallelTasks = Config.FinishParallelTasks.GetValue();
		}
		else
		{
			bool bUncalledMode;
			int32 UncalledTasks;
			ResolveUncalledSettings(bUncalledMode, UncalledTasks);
			Out.bDecodeModeRequested = Config.bParallelDecode.IsSet() ? Config.bParallelDecode.GetValue() : bUncalledMode;
			Out.ParallelTasks = Config.FinishParallelTasks.IsSet() ? Config.FinishParallelTasks.GetValue() : UncalledTasks;
		}
		// D-SLM8166: the matvec bit rides on the finish hook, which is installed only above 1 task.
		Out.bDecodeModeEffective = Out.bDecodeModeRequested && SuperSLMFinishHook::ShouldInstall(Out.ParallelTasks);

		// Cost fields: the caller's value where set; the rest from one profile.
		bool bCallerSet[kCostFieldCount] = {};
		for (int32 I = 0; I < kCostFieldCount; ++I)
		{
			const TOptional<double>& Caller = Config.*(GCallerFields[I]);
			bCallerSet[I] = Caller.IsSet();
			Out.CallerCostFields += bCallerSet[I] ? 1 : 0;
		}
		FSuperSLMResolvedCpuConfig ModeProfile;
		ApplyModeProfile(Out.bDecodeModeEffective, ModeProfile);
		if (Config.SequenceBudget > 0 && Model != nullptr)
		{
			SuperSLMRuntime::FModelShape Shape;
			FString ShapeError;
			if (SuperSLMRuntime::ReadModelShape(*Model, Shape, ShapeError))
			{
				const double (&Lifecycle)[3] = GBudgetLifecycleProfiles[Shape.ContextCap <= 4096 ? 0 : 1][Out.bDecodeModeEffective ? 1 : 0];
				ModeProfile.AdoptCostMs = Lifecycle[0];
				ModeProfile.RestoreCostMs = Lifecycle[1];
				ModeProfile.PrefixBeginCostMs = Lifecycle[2];
			}
		}

		double Saved[kCostFieldCount] = {};
		bool bUseSaved = false;
		double SavedPerMiB = 0.0; // the saved profile's RestoreCostPerMiBMs (U4), read only when it is used
		SuperSLMCore::FPrefixPrefillPriceInputs PrefixPrice; // rev 1.18.11 (§15.31): the cache chunk's pair
		FString SavedHeader;
		PrefixPrice.bCallerTokenSet = Config.PrefixPrefillTokenCostMs.IsSet();
		PrefixPrice.bCallerSlopeSet = Config.PrefixPrefillTokenCostPerPositionMs.IsSet();
		PrefixPrice.Caller.TokenMs = Config.PrefixPrefillTokenCostMs.Get(0.0);
		PrefixPrice.Caller.PerPositionMs = Config.PrefixPrefillTokenCostPerPositionMs.Get(0.0);
		// Rev 1.3.6 (P1.1): the per-C pairs are read from the same matching section, so it is kept.
		TArray<FIniSection> SavedSections;
		const FIniSection* SavedMatch = nullptr;
		bool bAnyPerChunkUnset = false;
		for (int32 C = 2; C <= 7; ++C)
		{
			bAnyPerChunkUnset = bAnyPerChunkUnset || !Config.PerChunkPromptTokenCostMs[C].IsSet() || !Config.PerChunkPromptTokenCostPerPositionMs[C].IsSet();
		}
		if ((Out.CallerCostFields < kCostFieldCount || bAnyPerChunkUnset) && Model != nullptr)
		{
			uint8 Hash[32] = {};
			FString Text;
			const FString Path = CalibratedCostsPath();
			if (SuperSLMRuntime::ReadArtifactHash(*Model, Hash) && IFileManager::Get().FileExists(*Path) && FFileHelper::LoadFileToString(Text, *Path))
			{
				SavedSections = ParseIniSections(Text);
				const TArray<FIniSection>& Sections = SavedSections;
				SavedMatch = FindSavedEstimate(Sections, SuperSLMRuntime::HashToHex(Hash), Out.bDecodeModeEffective, Out.ParallelTasks);
				if (const FIniSection* Match = Out.CallerCostFields < kCostFieldCount ? SavedMatch : nullptr)
				{
					TArray<FString> Problems;
					bUseSaved = ReadSavedCosts(*Match, Saved, Problems);
					if (bUseSaved)
					{
						// Rev 1.18.11 (§15.31): the prefix_prefill pair, optional. A text that does not parse reads as a
						// value out of the domain, so the pair is not used (ResolvePrefixPrefillPrice).
						PrefixPrice.bSavedUsed = true;
						SavedHeader = Match->Header;
						if (const FString* TokenText = Match->Keys.Find(PrefixPrefillTokenKey()))
						{
							PrefixPrice.bSavedTokenSet = true;
							if (!ParseFiniteDouble(*TokenText, PrefixPrice.Saved.TokenMs)) { PrefixPrice.Saved.TokenMs = -1.0; }
						}
						if (const FString* SlopeText = Match->Keys.Find(PrefixPrefillSlopeKey()))
						{
							PrefixPrice.bSavedSlopeSet = true;
							if (!ParseFiniteDouble(*SlopeText, PrefixPrice.Saved.PerPositionMs)) { PrefixPrice.Saved.PerPositionMs = -1.0; }
						}
					}
					if (const FString* PerMiBText = bUseSaved ? Match->Keys.Find(RestorePerMiBKey()) : nullptr)
					{
						double Value = 0.0;
						if (ParseFiniteDouble(*PerMiBText, Value) && Value >= 0.0)
						{
							SavedPerMiB = Value;
						}
						else
						{
							UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: CPU Configure: the saved estimate %s in %s has %s=%s, not a finite, non-negative number of milliseconds per MiB; the cache restore is priced flat (the term reads 0)."),
								*Match->Header, *Path, RestorePerMiBKey(), **PerMiBText);
						}
					}
					if (!bUseSaved)
					{
						// D-SLM8145: a saved profile with a missing, non-finite or out-of-domain key is not
						// used; the unset fields come from the mode profile.
						UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: CPU Configure: the saved estimate %s in %s for this artifact, bParallelDecode=%d and FinishParallelTasks=%d is not used: %s. Its fields come from the mode-%d profile."),
							*Match->Header, *Path, Out.bDecodeModeEffective ? 1 : 0, Out.ParallelTasks, *FString::Join(Problems, TEXT("; ")), Out.bDecodeModeEffective ? 1 : 0);
					}
				}
			}
		}

		for (int32 I = 0; I < kCostFieldCount; ++I)
		{
			const FCostField& Field = GCostFields[I];
			Out.*(Field.Member) = bCallerSet[I] ? (Config.*(GCallerFields[I])).GetValue()
				: bUseSaved ? Saved[I]
				: ModeProfile.*(Field.Member);
		}
		Out.CostProfile = Out.CallerCostFields == kCostFieldCount ? ESuperSLMCostProfile::Caller
			: bUseSaved ? ESuperSLMCostProfile::SavedEstimate
			: ESuperSLMCostProfile::ModeDefault;
		// Precache U4 (§5.5): the per-MiB term, outside the thirteen: the caller's, else the used saved profile's.
		Out.RestoreCostPerMiBMs = Config.RestoreCostPerMiBMs.IsSet() ? Config.RestoreCostPerMiBMs.GetValue()
			: bUseSaved ? SavedPerMiB : 0.0;
		// Rev 1.18.11 (§15.31): the cache chunk's pair. A caller error (one field, or out of the domain) resolves to
		// no pair here; Configure() refuses it by name.
		{
			const SuperSLMCore::FPrefixPrefillPrice Price = SuperSLMCore::ResolvePrefixPrefillPrice(PrefixPrice);
			Out.PrefixPrefillPriceSource = Price.Source == SuperSLMCore::EPrefixPrefillPriceSource::Caller ? ESuperSLMPrefixPrefillPriceSource::Caller
				: Price.Source == SuperSLMCore::EPrefixPrefillPriceSource::SavedEstimate ? ESuperSLMPrefixPrefillPriceSource::SavedEstimate
				: ESuperSLMPrefixPrefillPriceSource::BatchedFallback;
			Out.PrefixPrefillTokenCostMs = Price.Source == SuperSLMCore::EPrefixPrefillPriceSource::BatchedFallback ? 0.0 : Price.Pair.TokenMs;
			Out.PrefixPrefillTokenCostPerPositionMs = Price.Source == SuperSLMCore::EPrefixPrefillPriceSource::BatchedFallback ? 0.0 : Price.Pair.PerPositionMs;
			if (Price.bSavedIgnored)
			{
				UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: CPU Configure: the saved estimate %s in %s has %s and %s not both present as a finite, positive cost and a finite, non-negative slope; the cache chunk is priced with the batched pair."),
					*SavedHeader, *CalibratedCostsPath(), PrefixPrefillTokenKey(), PrefixPrefillSlopeKey());
			}
		}

		// Rev 1.3.6 (P1.1): the per-C pairs, each member on its own. A saved value is used only when
		// the saved estimate's thirteen were (or the caller set all thirteen), so a rejected profile
		// never contributes half its figures.
		auto ResolvePerChunk = [&Config](FSuperSLMResolvedCpuConfig& Into, const FIniSection* Section)
		{
			for (int32 C = 2; C <= 7; ++C)
			{
				const TOptional<double>& CallerToken = Config.PerChunkPromptTokenCostMs[C];
				const TOptional<double>& CallerSlope = Config.PerChunkPromptTokenCostPerPositionMs[C];
				double SavedToken = 0.0, SavedSlope = 0.0;
				const FString* TokenText = Section ? Section->Keys.Find(PerChunkTokenCostKey(C)) : nullptr;
				const FString* SlopeText = Section ? Section->Keys.Find(PerChunkTokenCostPerPositionKey(C)) : nullptr;
				const bool bSavedToken = TokenText && ParseFiniteDouble(*TokenText, SavedToken) && SavedToken > 0.0;
				const bool bSavedSlope = SlopeText && ParseFiniteDouble(*SlopeText, SavedSlope) && SavedSlope >= 0.0;
				Into.PerChunkPromptTokenCostMs[C] = CallerToken.IsSet() ? CallerToken.GetValue() : bSavedToken ? SavedToken : Into.PromptTokenCostMs;
				Into.PerChunkPromptTokenCostPerPositionMs[C] = CallerSlope.IsSet() ? CallerSlope.GetValue() : bSavedSlope ? SavedSlope : Into.PromptTokenCostPerPositionMs;
			}
		};
		const bool bSavedProfileUsable = bUseSaved || (Out.CallerCostFields == kCostFieldCount && SavedMatch != nullptr);
		ResolvePerChunk(Out, bSavedProfileUsable ? SavedMatch : nullptr);

		// D-SLM8145, ordered by D-SLM8167: when the resolved set trips the composed cost-domain check
		// and holds saved-estimate values, and the same set with the mode profile in their place does
		// not trip, that set is used, with one Warning. Configure() then checks the final set and
		// refuses InvalidCostDomain if it still trips, with no fallback Warning.
		if (bUseSaved && ShapeAdmitsDomainCheck(Config))
		{
			SuperSLMRuntime::FModelShape Shape;
			FString ShapeError, DomainError, FallbackError;
			if (SuperSLMRuntime::ReadModelShape(*Model, Shape, ShapeError) && !CheckCostDomain(Config, Out, Shape.ContextCap, DomainError))
			{
				FSuperSLMResolvedCpuConfig Fallback = Out;
				for (int32 I = 0; I < kCostFieldCount; ++I)
				{
					if (!bCallerSet[I])
					{
						Fallback.*(GCostFields[I].Member) = ModeProfile.*(GCostFields[I].Member);
					}
				}
				Fallback.CostProfile = ESuperSLMCostProfile::ModeDefault;
				Fallback.RestoreCostPerMiBMs = Config.RestoreCostPerMiBMs.IsSet() ? Config.RestoreCostPerMiBMs.GetValue() : 0.0;
				if (Fallback.PrefixPrefillPriceSource == ESuperSLMPrefixPrefillPriceSource::SavedEstimate)
				{
					Fallback.PrefixPrefillPriceSource = ESuperSLMPrefixPrefillPriceSource::BatchedFallback;
					Fallback.PrefixPrefillTokenCostMs = 0.0;
					Fallback.PrefixPrefillTokenCostPerPositionMs = 0.0;
				}
				ResolvePerChunk(Fallback, nullptr); // the saved per-C pairs go with the rest of the saved estimate
				if (CheckCostDomain(Config, Fallback, Shape.ContextCap, FallbackError))
				{
					UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM: CPU Configure: the saved estimate in %s for this artifact, bParallelDecode=%d and FinishParallelTasks=%d trips the cost domain at this configuration's call shape and TickBudgetMs; its fields come from the mode-%d profile instead."),
						*CalibratedCostsPath(), Out.bDecodeModeEffective ? 1 : 0, Out.ParallelTasks, Out.bDecodeModeEffective ? 1 : 0);
					Out = Fallback;
				}
			}
		}
		return Out;
	}

	// D-SLM7799 rule 3 (plan §5 "The scheduler's numeric domain"): the largest planned cost each job
	// kind can reach, computed in double with every product checked. With B = BlockCount, M =
	// MaxLayerBudget, P = min(MaxPrefillChunkBudget, context_cap) and D = context_cap - 1:
	//   decode:      B x (M x (LayerCostMs + LayerCostPerPositionMs x context_cap) + FinishCostMs)
	//   prefill:     slice 1P's one definition (tiled-matmul plan §4A.1 item 5, audit F4): each price
	//                pair at the largest chunk it can price, ending at D --
	//                  chunk-1 pair at n = min(P, 7):  n x PromptTokenCostMs + PromptTokenCostPerPositionMs x (n x (D - n + 1) + n x (n - 1) / 2)
	//                  batched pair at n = P:          the same form with the two Batched* fields
	//                and the configuration is refused when EITHER implies K > kMaxJobTicks (the
	//                two pairs can cross, so "the larger pair" is not a defined comparison)
	//   lifecycle:   the largest of the six op costs
	// A non-finite bound, or one whose ceil(bound / TickBudgetMs) exceeds kMaxJobTicks, refuses the
	// configuration. Each bound over-approximates what the four pricing sites and the op dispatch
	// can compute. There are two depth limits (D-SLM7803, T-3006 N1):
	//   - a decode prices its layers at depths up to context_cap itself. A prompt of exactly
	//     context_cap tokens is admitted and prefill leaves ContextUsed at the cap; the next decode
	//     job is priced at the cap, and only its Layer-1 call refuses (SSLM_CONTEXT_CAP_EXCEEDED). Bounding decode at
	//     the cap keeps that refusal the caller's context-cap error, never the plugin-defect fault;
	//   - a prefill position never exceeds D, because a prompt holds at most context_cap tokens.
	// A job's layers never exceed M, and its chunk never exceeds P.
	bool CheckCostDomain(const FSuperSLMRuntimeConfig& Shape, const FSuperSLMResolvedCpuConfig& Costs, int64 ContextCap, FString& OutError)
	{
		bool bFinite = true;
		auto Mul = [&bFinite](double A, double B)
		{
			const double R = A * B;
			bFinite = bFinite && FMath::IsFinite(R);
			return R;
		};
		auto Add = [&bFinite](double A, double B)
		{
			const double R = A + B;
			bFinite = bFinite && FMath::IsFinite(R);
			return R;
		};
		const double B = static_cast<double>(Shape.BlockCount);
		const double M = static_cast<double>(Shape.MaxLayerBudget);
		const double DecodeDepth = static_cast<double>(FMath::Max<int64>(0, ContextCap)); // T-3006 N1: decode prices up to the cap

		struct FBound { const TCHAR* Kind; const TCHAR* Fields; double Ms; bool bFinite; };
		FBound Bounds[5];

		bFinite = true;
		const double LayerAtCap = Add(Costs.LayerCostMs, Mul(Costs.LayerCostPerPositionMs, DecodeDepth));
		const double Decode = Mul(B, Add(Mul(M, LayerAtCap), Costs.FinishCostMs));
		Bounds[0] = { TEXT("decode"), TEXT("LayerCostMs, LayerCostPerPositionMs and FinishCostMs"), Decode, bFinite };

		const int64 PTokens = FMath::Min<int64>(Shape.MaxPrefillChunkBudget, ContextCap);
		const int64 DPosition = FMath::Max<int64>(0, ContextCap - 1);
		// Schema fast-decode plan rev 1.3.5/1.3.6, §6 P1 "Pricing": the bound for chunks below 8 uses
		// the per-C table up to C = 7 (the resolved PerChunkPromptTokenCost*Ms, which default to the
		// chunk-1 pair until B5f fills them).
		const SuperSLMPrefillPlanning::FPromptPrice Chunk1Price{ Costs.PromptTokenCostMs, Costs.PromptTokenCostPerPositionMs };
		const SuperSLMPrefillPlanning::FPrefillBound Chunk1Prefill = SuperSLMPrefillPlanning::SmallChunkPrefillBoundMsWithTable(PTokens, DPosition,
			Chunk1Price, PerChunkPromptPrices(Costs));
		const SuperSLMPrefillPlanning::FPrefillBound BatchedPrefill = SuperSLMPrefillPlanning::BatchedPrefillBoundMs(PTokens, DPosition,
			SuperSLMPrefillPlanning::FPromptPrice{ Costs.BatchedPromptTokenCostMs, Costs.BatchedPromptTokenCostPerPositionMs });
		Bounds[1] = { TEXT("prefill (chunk-1 price, chunks below 8)"), TEXT("PromptTokenCostMs and PromptTokenCostPerPositionMs, or the per-C PerChunkPromptTokenCost*Ms[2..7]"), Chunk1Prefill.Ms, Chunk1Prefill.bFinite };
		Bounds[2] = { TEXT("prefill (batched price, chunks of 8 or more)"), TEXT("BatchedPromptTokenCostMs and BatchedPromptTokenCostPerPositionMs"), BatchedPrefill.Ms, BatchedPrefill.bFinite };

		// The lifecycle bound is the largest of the six operation costs; it names that field.
		const TPair<const TCHAR*, double> Ops[] = {
			{ TEXT("ResetCostMs"), Costs.ResetCostMs }, { TEXT("AdoptCostMs"), Costs.AdoptCostMs },
			{ TEXT("SaveCostMs"), Costs.SaveCostMs }, { TEXT("RestoreCostMs"), Costs.RestoreCostMs },
			{ TEXT("PrefixBeginCostMs"), Costs.PrefixBeginCostMs }, { TEXT("PrefixReleaseCostMs"), Costs.PrefixReleaseCostMs },
		};
		const TPair<const TCHAR*, double>* Largest = &Ops[0];
		bool bOpsFinite = true;
		for (const TPair<const TCHAR*, double>& Op : Ops)
		{
			bOpsFinite = bOpsFinite && FMath::IsFinite(Op.Value);
			if (!FMath::IsFinite(Op.Value) || (FMath::IsFinite(Largest->Value) && Op.Value > Largest->Value))
			{
				Largest = &Op;
			}
		}
		Bounds[3] = { TEXT("lifecycle"), Largest->Key, Largest->Value, bOpsFinite };

		// Rev 1.18.11 (§15.31): a cache chunk priced with the prefix_prefill pair, as the batched bound is, when the
		// pair is present (it prices every cache chunk of 8 tokens or more in place of the batched pair).
		if (Costs.PrefixPrefillPriceSource != ESuperSLMPrefixPrefillPriceSource::BatchedFallback)
		{
			const SuperSLMPrefillPlanning::FPrefillBound PrefixPrefill = SuperSLMPrefillPlanning::BatchedPrefillBoundMs(PTokens, DPosition,
				SuperSLMPrefillPlanning::FPromptPrice{ Costs.PrefixPrefillTokenCostMs, Costs.PrefixPrefillTokenCostPerPositionMs });
			Bounds[4] = { TEXT("cache chunk (prefix_prefill price, chunks of 8 or more)"), TEXT("PrefixPrefillTokenCostMs and PrefixPrefillTokenCostPerPositionMs"), PrefixPrefill.Ms, PrefixPrefill.bFinite };
		}
		else
		{
			Bounds[4] = { TEXT("cache chunk"), TEXT("the batched pair"), 0.0, true };
		}

		for (const FBound& Bound : Bounds)
		{
			const double K = Bound.bFinite ? std::ceil(Bound.Ms / Shape.TickBudgetMs) : TNumericLimits<double>::Max();
			if (!Bound.bFinite || !FMath::IsFinite(K) || K > static_cast<double>(kMaxJobTicks))
			{
				OutError = FString::Printf(
					TEXT("InvalidCostDomain: the %s job's largest planned cost, priced from %s, is %s ms, which implies K = %s at TickBudgetMs %g; K must not exceed kMaxJobTicks = %d"),
					Bound.Kind, Bound.Fields,
					Bound.bFinite ? *FString::Printf(TEXT("%g"), Bound.Ms) : TEXT("not finite"),
					(Bound.bFinite && FMath::IsFinite(K)) ? *FString::Printf(TEXT("%.0f"), K) : TEXT("not finite"),
					Shape.TickBudgetMs, kMaxJobTicks);
				return false;
			}
		}
		return true;
	}

	const TCHAR* CostProfileName(ESuperSLMCostProfile Profile)
	{
		switch (Profile)
		{
			case ESuperSLMCostProfile::Caller: return TEXT("caller");
			case ESuperSLMCostProfile::SavedEstimate: return TEXT("saved-estimate");
			case ESuperSLMCostProfile::ModeDefault: return TEXT("mode-default");
		}
		return TEXT("mode-default");
	}

	FString ConfigureLine(const FSuperSLMResolvedCpuConfig& Resolved)
	{
		return FString::Printf(TEXT("SuperSLM: CPU Configure: bParallelDecode=%d FinishParallelTasks=%d cost profile=%s caller_cost_fields=%d"),
			Resolved.bDecodeModeEffective ? 1 : 0, Resolved.ParallelTasks, CostProfileName(Resolved.CostProfile), Resolved.CallerCostFields);
	}

	bool WriteTextFileAtomically(const FString& Path, const FString& Text, FString& OutError)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(Path);
		IFileManager& Files = IFileManager::Get();
		Files.MakeDirectory(*FPaths::GetPath(FullPath), true);
		const FString TempPath = FullPath + TEXT(".tmp-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!FFileHelper::SaveStringToFile(Text, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Files.Delete(*TempPath, false, true, true);
			OutError = FString::Printf(TEXT("the temporary file %s could not be written"), *TempPath);
			return false;
		}
#if PLATFORM_WINDOWS
		// One replace: MoveFileExW swaps the name to the new file, or leaves the destination as it was.
		if (::MoveFileExW(*TempPath, *FullPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
		{
			const uint32 Error = ::GetLastError();
			Files.Delete(*TempPath, false, true, true);
			OutError = FString::Printf(TEXT("replacing the file failed with Windows error %u"), Error);
			return false;
		}
		return true;
#else
		if (!Files.Move(*FullPath, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ false, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
		{
			Files.Delete(*TempPath, false, true, true);
			OutError = TEXT("replacing the file failed");
			return false;
		}
		return true;
#endif
	}

	TArray<FIniSection> ParseIniSections(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		TArray<FIniSection> Sections;
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.StartsWith(TEXT("[")))
			{
				Sections.AddDefaulted_GetRef().Header = Line;
				continue;
			}
			if (Sections.Num() == 0 || Line.IsEmpty() || Line.StartsWith(TEXT(";")))
			{
				continue;
			}
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Sections.Last().Keys.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
			}
		}
		return Sections;
	}

	FString KeptEstimateProfiles(const FString& ExistingText, const FString& HashHex, int32 Tasks)
	{
		TArray<FString> Lines;
		ExistingText.ParseIntoArrayLines(Lines, false);
		FString Kept;
		FString Block;
		FIniSection Section;
		bool bInSection = false;
		auto Flush = [&]()
		{
			if (!bInSection)
			{
				return;
			}
			const FString* Hash = Section.Keys.Find(kHashKey);
			const FString* Mode = Section.Keys.Find(kModeKey);
			const FString* TaskText = Section.Keys.Find(kTasksKey);
			bool bMode = false;
			int32 SectionTasks = -1;
			if (Hash != nullptr && Mode != nullptr && TaskText != nullptr && ParseMode(*Mode, bMode) && ParseTasks(*TaskText, SectionTasks)
				&& (!Hash->Equals(HashHex, ESearchCase::IgnoreCase) || SectionTasks != Tasks))
			{
				Kept += Block;
			}
		};
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.StartsWith(TEXT("[")))
			{
				Flush();
				bInSection = true;
				Section = FIniSection();
				Section.Header = Line;
				Block = Line + TEXT("\n");
				continue;
			}
			if (!bInSection || Line.IsEmpty() || Line.StartsWith(TEXT(";")))
			{
				continue;
			}
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Section.Keys.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
				Block += Line + TEXT("\n");
			}
		}
		Flush();
		return Kept;
	}

	bool ParseFiniteDouble(const FString& Text, double& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}
		// The whole string must be one number, so strtod must consume every character. A number is
		// ASCII; anything else cannot parse.
		for (const TCHAR Ch : Trimmed)
		{
			if (Ch > 0x7F)
			{
				return false;
			}
		}
		const FTCHARToUTF8 Narrow(*Trimmed);
		const char* Begin = Narrow.Get();
		char* End = nullptr;
		const double Value = std::strtod(Begin, &End);
		if (End == Begin || End == nullptr || *End != '\0' || !FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	bool WriteSavedPrefixPrefillPair(const FString& HashHex, bool bEffectiveMode, int32 Tasks, const FString& TokenText, const FString& SlopeText,
		FString& OutHeader, FString& OutError)
	{
		OutHeader.Reset();
		const FString Path = CalibratedCostsPath();
		FString Text;
		if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("%s is missing or unreadable: SuperSLM.EstimateCosts --write-ini writes the profile the in-situ pair goes into"), *Path);
			return false;
		}
		const TArray<FIniSection> Sections = ParseIniSections(Text);
		const FIniSection* Match = FindSavedEstimate(Sections, HashHex, bEffectiveMode, Tasks);
		if (Match == nullptr)
		{
			OutError = FString::Printf(TEXT("%s has no profile for ArtifactHash %s, bParallelDecode=%d and FinishParallelTasks=%d: run SuperSLM.EstimateCosts --write-ini at this configuration first"),
				*Path, *HashHex, bEffectiveMode ? 1 : 0, Tasks);
			return false;
		}
		const size_t Index = static_cast<size_t>(Match - Sections.GetData());
		const std::string Narrow(FTCHARToUTF8(*Text).Get());
		const std::vector<std::pair<std::string, std::string>> Keys = {
			{ std::string(FTCHARToUTF8(PrefixPrefillTokenKey()).Get()), std::string(FTCHARToUTF8(*TokenText).Get()) },
			{ std::string(FTCHARToUTF8(PrefixPrefillSlopeKey()).Get()), std::string(FTCHARToUTF8(*SlopeText).Get()) } };
		bool bFound = false;
		const std::string Edited = SuperSLMCore::SetIniSectionKeys(Narrow, Index, Keys, bFound);
		if (!bFound)
		{
			OutError = FString::Printf(TEXT("%s: the profile %s (section %d) was not found again in its text"), *Path, *Match->Header, static_cast<int32>(Index));
			return false;
		}
		// Code review S1: the rewrite is checked with the parser that chose the section, so a line break or whitespace
		// the two read differently can never touch another profile. Every other section must come back unchanged, and
		// the target with its own keys unchanged and exactly the new pair; otherwise nothing is written.
		const FString EditedText(UTF8_TO_TCHAR(Edited.c_str()));
		const TArray<FIniSection> After = ParseIniSections(EditedText);
		const FString TokenKey(PrefixPrefillTokenKey());
		const FString SlopeKey(PrefixPrefillSlopeKey());
		auto SameKeys = [](const TMap<FString, FString>& A, const TMap<FString, FString>& B, const FString& SkipA, const FString& SkipB)
		{
			int32 Counted = 0;
			for (const TPair<FString, FString>& Pair : A)
			{
				if (Pair.Key == SkipA || Pair.Key == SkipB)
				{
					continue;
				}
				const FString* Other = B.Find(Pair.Key);
				if (Other == nullptr || *Other != Pair.Value)
				{
					return false;
				}
				++Counted;
			}
			int32 OtherCounted = 0;
			for (const TPair<FString, FString>& Pair : B)
			{
				OtherCounted += (Pair.Key == SkipA || Pair.Key == SkipB) ? 0 : 1;
			}
			return Counted == OtherCounted;
		};
		bool bIntact = After.Num() == Sections.Num();
		for (int32 I = 0; bIntact && I < Sections.Num(); ++I)
		{
			bIntact = After[I].Header == Sections[I].Header;
			if (bIntact && I != static_cast<int32>(Index))
			{
				bIntact = SameKeys(Sections[I].Keys, After[I].Keys, FString(), FString());
			}
			else if (bIntact)
			{
				const FString* Token = After[I].Keys.Find(TokenKey);
				const FString* Slope = After[I].Keys.Find(SlopeKey);
				bIntact = SameKeys(Sections[I].Keys, After[I].Keys, TokenKey, SlopeKey)
					&& Token != nullptr && *Token == TokenText.TrimStartAndEnd() && Slope != nullptr && *Slope == SlopeText.TrimStartAndEnd();
			}
		}
		if (!bIntact)
		{
			OutError = FString::Printf(TEXT("%s: the rewrite of the profile %s (section %d) did not read back as that profile with only the new pair changed; the file is unchanged"),
				*Path, *Match->Header, static_cast<int32>(Index));
			return false;
		}
		OutHeader = Match->Header;
		return WriteTextFileAtomically(Path, EditedText, OutError);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FSuperSLMConfigureTestAccess::ClearDecodeSelection()
{
	FScopeLock Lock(&SuperSLMRuntimeResolve::GSelectionLock);
	SuperSLMRuntimeResolve::GSelectedDecodeMode.Reset();
	SuperSLMRuntimeResolve::GSelectedParallelTasks.Reset();
}
#endif // WITH_DEV_AUTOMATION_TESTS
