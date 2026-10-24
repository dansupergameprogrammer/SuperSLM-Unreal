// Everything that touches the prefix cache is under SUPERSLM_WITH_PREFIX_CACHE; without it the check reports that and
// exits 1.

#include "SuperSLMPrefixCacheCheck.h"

#include "SuperSLMPrefixCacheTypes.h" // defines SUPERSLM_WITH_PREFIX_CACHE (0 unless the plugin's build turns it on)

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"

#if SUPERSLM_WITH_PREFIX_CACHE
#include "Algo/Reverse.h"
#include "Kismet/GameplayStatics.h"
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMExampleDemo.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMQuery.h"
#include "SuperSLMRuntimeConfig.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMPrefixCacheCheck, Log, All);

namespace SuperSLMPrefixCacheCheck
{
	const TCHAR* const kLine = TEXT("SuperSLMExample.PrefixCacheCheck");
	TUniquePtr<FSuperSLMPrefixCacheCheck> GActive;
	FDelegateHandle GPreExit;

	const TCHAR* const kLegs[] = { TEXT("Cooked"), TEXT("Budget"), TEXT("DiskWrite"), TEXT("DiskRead"), TEXT("Determinism"), TEXT("Calibrate"),
		TEXT("Residency"), TEXT("OneJobBuild") };

	struct FCheckLine
	{
		FString Name;
		bool bPass = false;
		FString Detail;
	};

	FString ReportDirectory()
	{
		FString Dir;
		if (!FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExamplePrefixCacheOut="), Dir) || Dir.IsEmpty())
		{
			Dir = FPaths::ProjectSavedDir() / TEXT("SuperSLMPrefixCacheCheck");
		}
		return FPaths::ConvertRelativePathToFull(Dir);
	}

	// Cell 4.5: the plugin on Windows x64, in a packaged (cooked) build.
	void AddPlatform(TArray<FCheckLine>& Checks, const TSharedRef<FJsonObject>& Report)
	{
		const FString Platform = FPlatformProperties::IniPlatformName();
		const bool bX64 = PLATFORM_64BITS && PLATFORM_CPU_X86_FAMILY;
		const bool bCooked = FPlatformProperties::RequiresCookedData();
		const FString Configuration = LexToString(FApp::GetBuildConfiguration());
		TSharedRef<FJsonObject> Cell = MakeShared<FJsonObject>();
		Cell->SetStringField(TEXT("platform"), Platform);
		Cell->SetBoolField(TEXT("x64"), bX64);
		Cell->SetBoolField(TEXT("cooked"), bCooked);
		Cell->SetStringField(TEXT("build_configuration"), Configuration);
		Report->SetObjectField(TEXT("cell_4_5"), Cell);
		// In the editor (Play in Editor) nothing is packaged or cooked, so 4.5 is recorded there, not graded: the leg's
		// other checks still read the cache. A packaged run grades it.
		const bool bGraded = !GIsEditor;
		Checks.Add({ TEXT("4.5.windows_x64_packaged"), !bGraded || (Platform == TEXT("Windows") && bX64 && bCooked),
			FString::Printf(TEXT("%s%s, %s, %s, %s"), bGraded ? TEXT("") : TEXT("recorded, not graded in the editor: "), *Platform,
				bX64 ? TEXT("x64") : TEXT("not x64"), bCooked ? TEXT("cooked") : TEXT("not cooked"), *Configuration) });
	}

	// Writes <dir>/<Leg>.json, logs one line per check and the summary, and asks the process to exit (not in the editor).
	void Finish(const FString& Leg, TArray<FCheckLine>& Checks, const TSharedRef<FJsonObject>& Report)
	{
		bool bPass = Checks.Num() > 0;
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCheckLine& C : Checks)
		{
			bPass &= C.bPass;
			UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: %s %s -- %s"), kLine, C.bPass ? TEXT("PASS") : TEXT("FAIL"), *C.Name, *C.Detail);
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), C.Name);
			O->SetBoolField(TEXT("pass"), C.bPass);
			O->SetStringField(TEXT("detail"), C.Detail);
			Values.Add(MakeShared<FJsonValueObject>(O));
		}
		Report->SetStringField(TEXT("check"), TEXT("packaged prefix cache"));
		Report->SetStringField(TEXT("leg"), Leg);
		Report->SetBoolField(TEXT("pass"), bPass);
		Report->SetArrayField(TEXT("checks"), Values);
		const FString Dir = ReportDirectory();
		const FString Path = Dir / (Leg.IsEmpty() ? FString(TEXT("unknown")) : Leg) + TEXT(".json");
		FString Text;
		const bool bSaved = FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text))
			&& IFileManager::Get().MakeDirectory(*Dir, true)
			&& FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bSaved)
		{
			UE_LOG(LogSuperSLMPrefixCacheCheck, Error, TEXT("%s: the report could not be written to %s"), kLine, *Path);
			bPass = false;
		}
		UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: %s %s; report %s"), kLine, *Leg, bPass ? TEXT("PASS") : TEXT("FAIL"), *Path);
		// In the editor the check never asks the process to exit: an exit requested while Play in Editor runs tears PIE
		// down during editor shutdown, and the engine crashed there (UAssetEditorSubsystem::FindEditorsForAssetAndSubObjects,
		// from UEditorEngine::EndPlayMap; 1.3.0 manual check, 2026-10-24). Stop Play to end it. A packaged run exits
		// with the verdict as before.
		if (GIsEditor)
		{
			UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: in the editor the check does not exit; stop Play to end it"), kLine);
			return;
		}
		FPlatformMisc::RequestExitWithStatus(false, bPass ? 0 : 1, kLine);
	}
}

bool FSuperSLMPrefixCacheCheck::IsRequested(FString& OutLeg)
{
	return FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExamplePrefixCacheCheck="), OutLeg);
}

FSuperSLMPrefixCacheCheck* FSuperSLMPrefixCacheCheck::Active()
{
	return SuperSLMPrefixCacheCheck::GActive.Get();
}

#if !SUPERSLM_WITH_PREFIX_CACHE

// A build without the prefix cache: the leg cannot run, and says so.
struct FSuperSLMPrefixCacheCheck::FState
{
};

FSuperSLMPrefixCacheCheck::FSuperSLMPrefixCacheCheck() = default;
FSuperSLMPrefixCacheCheck::~FSuperSLMPrefixCacheCheck() = default;

void FSuperSLMPrefixCacheCheck::Start(USuperSLMModel&, USuperSLMSubsystem&, const FString& Leg, const FString&)
{
	using namespace SuperSLMPrefixCacheCheck;
	TArray<FCheckLine> Checks;
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	AddPlatform(Checks, Report);
	Checks.Add({ TEXT("pre.prefix_cache_compiled"), false, TEXT("this build has SUPERSLM_WITH_PREFIX_CACHE off; the plugin has no prefix cache") });
	Finish(Leg, Checks, Report);
}

void FSuperSLMPrefixCacheCheck::Tick(UWorld*, float)
{
}

#else // SUPERSLM_WITH_PREFIX_CACHE

namespace SuperSLMPrefixCacheCheck
{
	constexpr int32 Entries = 10;          // the set's entries: the level's 10 cooked prefixes
	constexpr int32 SuffixLength = 8;
	constexpr int32 MaxNewTokens = 16;
	constexpr int64 LargeBudgetBytes = 64ll * 1024 * 1024; // holds all 10 (each about 3 MiB at 128 tokens)
	constexpr double PhaseSeconds = 5.0 * 60.0;
	constexpr double LegSeconds = 20.0 * 60.0; // after the Calibrate leg's estimate, which blocks the game thread
	constexpr int32 QueryMaxNewTokens = 32;
	constexpr int32 LongBuildTokens = 2000;  // 5.9's build leg, L = 2,000
	// The in-situ prefix_prefill arm's build (rev 1.18.12, §15.32): the isolated arm's length, depth 0 to 2,048.
	constexpr int32 InSituBuildTokens = 2048;
	const TCHAR* const kDefaultSet = TEXT("/Game/SuperSLMExamplePrefixes/U4PrefixSet.U4PrefixSet");

	const TCHAR* SourceName(ESuperSLMPrefixSource Source)
	{
		switch (Source)
		{
		case ESuperSLMPrefixSource::Pending: return TEXT("Pending");
		case ESuperSLMPrefixSource::Memory: return TEXT("Memory");
		case ESuperSLMPrefixSource::Cooked: return TEXT("Cooked");
		case ESuperSLMPrefixSource::Disk: return TEXT("Disk");
		case ESuperSLMPrefixSource::Recomputed: return TEXT("Recomputed");
		default: return TEXT("Invalid");
		}
	}

	const TCHAR* LeaseStateName(ESuperSLMPrefixLeaseState State)
	{
		switch (State)
		{
		case ESuperSLMPrefixLeaseState::Pending: return TEXT("Pending");
		case ESuperSLMPrefixLeaseState::Ready: return TEXT("Ready");
		case ESuperSLMPrefixLeaseState::BudgetExhausted: return TEXT("BudgetExhausted");
		case ESuperSLMPrefixLeaseState::PoolExhausted: return TEXT("PoolExhausted");
		case ESuperSLMPrefixLeaseState::UnknownSchema: return TEXT("UnknownSchema");
		case ESuperSLMPrefixLeaseState::Cancelled: return TEXT("Cancelled");
		case ESuperSLMPrefixLeaseState::Failed: return TEXT("Failed");
		case ESuperSLMPrefixLeaseState::Ended: return TEXT("Ended");
		default: return TEXT("Invalid");
		}
	}

	TArray<TSharedPtr<FJsonValue>> JsonInts(const TArray<int32>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (int32 V : Values) { Out.Add(MakeShared<FJsonValueNumber>(V)); }
		return Out;
	}

	TSharedRef<FJsonObject> JsonStats(const FSuperSLMPrefixCacheStats& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("configured"), S.bConfigured);
		O->SetBoolField(TEXT("budget_mode"), S.bBudgetMode);
		O->SetNumberField(TEXT("charged_bytes"), static_cast<double>(S.ChargedBytes));
		O->SetNumberField(TEXT("resident_bytes"), static_cast<double>(S.ResidentBytes));
		O->SetNumberField(TEXT("resident_entries"), S.ResidentEntries);
		O->SetNumberField(TEXT("requests"), static_cast<double>(S.Requests));
		O->SetNumberField(TEXT("evictions"), static_cast<double>(S.Evictions));
		O->SetNumberField(TEXT("restores_issued"), static_cast<double>(S.RestoresIssued));
		O->SetNumberField(TEXT("builds_issued"), static_cast<double>(S.BuildsIssued));
		O->SetNumberField(TEXT("disk_writes_issued"), static_cast<double>(S.DiskWritesIssued));
		O->SetNumberField(TEXT("spec_mismatches"), static_cast<double>(S.SpecMismatches));
		O->SetNumberField(TEXT("cooked_entries_registered"), S.CookedEntriesRegistered);
		O->SetNumberField(TEXT("prefix_prefill_calls"), static_cast<double>(S.PrefixPrefillCalls));
		O->SetNumberField(TEXT("prefix_prefill_builds"), static_cast<double>(S.PrefixPrefillBuilds));
		TSharedRef<FJsonObject> Served = MakeShared<FJsonObject>();
		for (int32 I = 0; I < 6; ++I)
		{
			Served->SetNumberField(SourceName(static_cast<ESuperSLMPrefixSource>(I)), static_cast<double>(S.ServedBySource[I]));
		}
		O->SetObjectField(TEXT("served_by_source"), Served);
		TSharedRef<FJsonObject> Fallbacks = MakeShared<FJsonObject>();
		for (const TPair<FName, int64>& F : S.Fallbacks)
		{
			Fallbacks->SetNumberField(F.Key.ToString(), static_cast<double>(F.Value));
		}
		O->SetObjectField(TEXT("fallbacks"), Fallbacks);
		return O;
	}

	int64 Served(const FSuperSLMPrefixCacheStats& S, ESuperSLMPrefixSource Source)
	{
		return S.ServedBySource[static_cast<int32>(Source)];
	}
}

struct FSuperSLMPrefixCacheCheck::FState
{
	using FCheckLine = SuperSLMPrefixCacheCheck::FCheckLine;

	enum class ELeg : uint8 { Cooked, Budget, DiskWrite, DiskRead, Determinism, Calibrate, Residency, OneJobBuild };
	enum class EStep : uint8
	{
		Start,
		Reference,      // the recompute pass: the set not registered
		Cooked,         // 10.5: the level's 10 requests at once, the set registered
		BudgetEntry,    // 10.5's second half: the level entered at a budget of 5 entries
		Travel,         // the level left and re-entered (the map opened again)
		BudgetReentry,
		DiskWrite,      // 10.6 run 1
		DiskFlush,
		DiskRead,       // 10.6 run 2
		SelfCheckOff,   // 8.9: no cache
		QueryOff,
		SelfCheckOn,    // 8.9: the cache on and serving the set throughout
		QueryOn,
		CalibratePass,  // cooked restores priced by bytes
		Warm,           // 10.7: the next level's 10 prefixes warmed from the cooked set
		ResidencyStart, // 10.7: the level's start, every request a Memory hit
		Drop,           // 10.7: the level's exit, the 10 dropped (Memory)
		ResidencyNext,  // 10.7: the next level, the dropped specs served from the cooked tier
		LongBuild,      // 5.9: the sliced CPU cache build of 2,000 tokens, then its one-call reference
		InSituBuild,    // Calibrate, rev 1.18.12 (§15.32): the in-situ prefix_prefill arm's two builds
		Done,
	};

	// One request through to its adopter's tokens: request, Ready, vend, adopt, generate, return both.
	struct FLane
	{
		int32 Entry = 0;
		enum class EState : uint8 { Requested, Adopting, Generating, Done } State = EState::Requested;
		FSuperSLMPrefix Lease;
		FSuperSLMSequence Sequence;
		FSuperSLMLifecycleOpHandle Op;
		ESuperSLMPrefixSource Source = ESuperSLMPrefixSource::Pending;
		TArray<int32> Tokens;
	};

	// A pass: Order's entries in turn, at most Concurrency in flight.
	struct FPass
	{
		TArray<int32> Order;
		int32 Concurrency = 1;
		int32 Next = 0;
		TArray<FLane> Lanes;
		FSuperSLMPrefixCacheStats Before;
		int64 FirstTickRow = 0;
		int64 FirstLedgerRow = 0;
		double MaxTickMs = 0.0;
		int32 Ticks = 0;
		bool IsDone() const
		{
			if (Next < Order.Num()) { return false; }
			for (const FLane& L : Lanes) { if (L.State != FLane::EState::Done) { return false; } }
			return true;
		}
	};

	TStrongObjectPtr<USuperSLMModel> Model;
	TStrongObjectPtr<USuperSLMPrefixSet> Set;
	USuperSLMSubsystem& Cpu;
	ELeg Leg = ELeg::Cooked;
	FString LegName;
	FString ModelPath;
	FString SetPath;
	FString DiskDir;
	FString ComparePath;
	FString CalibrateArtifact;
	EStep Step = EStep::Start;
	TArray<FSuperSLMPrefixSpec> Specs;
	TArray<FName> EntryNames;
	TArray<TArray<int32>> Suffixes;
	TArray<TArray<int32>> Reference;   // the recompute pass's tokens (or, for DiskRead, run 1's)
	FPass Pass;
	TArray<FCheckLine> Checks;
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	double LegStarted = FPlatformTime::Seconds();
	double PhaseStarted = FPlatformTime::Seconds();
	int64 EntryBytes = 0;
	int64 ReferenceResidentBytes = 0; // every entry's bytes, resident after the reference pass (10.7's drop leg)
	TWeakObjectPtr<UWorld> TravelFrom;
	TFunction<void()> AfterTravel;
	FString TravelCheck;
	// 10.7
	TArray<FSuperSLMPrefixOp> Ops;
	FSuperSLMPrefixCacheStats PhaseBefore;
	int32 SettleFrames = 0;
	// 5.9: the sliced build and its one-call reference (FLongBuild, below)
	// 8.9
	TUniquePtr<FSuperSLMSelfCheckRun> SelfCheck;
	TUniquePtr<FSuperSLMQueryRunner> Query;
	FSuperSLMSelfCheckReport SelfCheckOffReport;
	FString DigestOff;
	FSuperSLMPrefix Traffic;
	int32 TrafficNext = 0;
	int64 WindowHitsBefore = 0;
	// Calibrate
	FSuperSLMResolvedCpuConfig Resolved;

	FState(USuperSLMModel& InModel, USuperSLMSubsystem& InCpu, const FString& InLeg, const FString& InModelPath)
		: Model(&InModel), Cpu(InCpu), LegName(InLeg), ModelPath(InModelPath)
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExamplePrefixSet="), SetPath) || SetPath.IsEmpty())
		{
			SetPath = SuperSLMPrefixCacheCheck::kDefaultSet;
		}
		FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExamplePrefixDiskDir="), DiskDir);
		FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExamplePrefixCompare="), ComparePath);
		FParse::Value(FCommandLine::Get(), TEXT("SuperSLMExampleCalibrateArtifact="), CalibrateArtifact);
		Report->SetNumberField(TEXT("schema"), 1);
		Report->SetStringField(TEXT("model_path"), ModelPath);
		Report->SetStringField(TEXT("prefix_set"), SetPath);
		Report->SetNumberField(TEXT("model_bytes"), static_cast<double>(InModel.GetMappedArtifactSize()));
		Report->SetBoolField(TEXT("model_memory_mapped"), InModel.IsPayloadMemoryMapped());
		SuperSLMPrefixCacheCheck::AddPlatform(Checks, Report);
	}

	~FState()
	{
		// Nothing waits: a runner or self-check returns its sequence when destroyed.
		Query.Reset();
		SelfCheck.Reset();
	}

	// --- Recording ---------------------------------------------------------------------------

	void Add(const FString& Name, bool bPass, const FString& Detail)
	{
		Checks.Add({ Name, bPass, Detail });
	}

	// A refusal or a timeout: the leg cannot go on. Records it and finishes.
	void Fail(const FString& Name, const FString& Detail)
	{
		Add(Name, false, Detail);
		End();
	}

	void End()
	{
		if (Step == EStep::Done)
		{
			return;
		}
		Step = EStep::Done;
		Report->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - LegStarted);
		if (Cpu.GetConfiguredModel() != nullptr)
		{
			Report->SetObjectField(TEXT("stats_at_end"), SuperSLMPrefixCacheCheck::JsonStats(Cpu.GetPrefixCacheStats()));
		}
		Query.Reset();
		SelfCheck.Reset();
		SuperSLMPrefixCacheCheck::Finish(LegName, Checks, Report);
	}

	void EnterPhase(EStep Next)
	{
		Step = Next;
		PhaseStarted = FPlatformTime::Seconds();
	}

	// --- Setup -------------------------------------------------------------------------------

	bool ParseLeg()
	{
		static const ELeg Values[] = { ELeg::Cooked, ELeg::Budget, ELeg::DiskWrite, ELeg::DiskRead, ELeg::Determinism, ELeg::Calibrate,
			ELeg::Residency, ELeg::OneJobBuild };
		for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(SuperSLMPrefixCacheCheck::kLegs)); ++I)
		{
			if (LegName.Equals(SuperSLMPrefixCacheCheck::kLegs[I], ESearchCase::IgnoreCase))
			{
				Leg = Values[I];
				LegName = SuperSLMPrefixCacheCheck::kLegs[I];
				return true;
			}
		}
		return false;
	}

	// The set: 10 root entries, no schema, a CPU build whose every entry has bytes. Its specs are the requests. Every
	// leg but Residency needs the 10 of equal length (the Budget leg prices entries alike). Residency takes a mixed
	// Text + Tokens set (cell 10.7), whose Text entries tokenize to their own lengths, and reads its specs through
	// FindEntrySpec once a model is configured (SpecsFromSet).
	bool LoadSet()
	{
		USuperSLMPrefixSet* Loaded = LoadObject<USuperSLMPrefixSet>(nullptr, *SetPath);
		if (Loaded == nullptr)
		{
			Fail(TEXT("pre.prefix_set_loaded"), FString::Printf(TEXT("no prefix set at %s (is its folder in DirectoriesToAlwaysCook?)"), *SetPath));
			return false;
		}
		Set.Reset(Loaded);
		const FSuperSLMPrefixSetBuild* CpuBuild = Loaded->Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& B) { return B.Backend == 0; });
		int32 BuiltEntries = 0;
		if (CpuBuild != nullptr)
		{
			for (const FSuperSLMPrefixSetBuiltEntry& E : CpuBuild->Entries) { BuiltEntries += E.Wrapper.Num() > 0 ? 1 : 0; }
		}
		bool bRoots = Loaded->Entries.Num() == SuperSLMPrefixCacheCheck::Entries;
		int32 Length = Loaded->Entries.Num() > 0 ? Loaded->Entries[0].Tokens.Num() : 0;
		for (const FSuperSLMPrefixSetEntry& E : Loaded->Entries)
		{
			bRoots &= E.ParentName.IsNone() && E.SchemaName.IsNone() && E.Tokens.Num() > 0 && (Leg == ELeg::Residency || E.Tokens.Num() == Length);
		}
		int32 TextEntries = 0;
		for (const FSuperSLMPrefixSetEntry& E : Loaded->Entries)
		{
			TextEntries += E.Input == ESuperSLMPrefixEntryInput::Text ? 1 : 0;
		}
		const FString Detail = FString::Printf(TEXT("%d entries (%d Text) of %d tokens (the first); CPU build %s with %d entries with bytes (Layer 1 %s %s); set model %s"),
			Loaded->Entries.Num(), TextEntries, Length, CpuBuild ? TEXT("present") : TEXT("absent"), BuiltEntries,
			CpuBuild ? *CpuBuild->Layer1Tag : TEXT("-"), CpuBuild ? *CpuBuild->Layer1Commit : TEXT("-"),
			Loaded->Model ? *Loaded->Model->GetPathName() : TEXT("none"));
		Report->SetStringField(TEXT("prefix_set_detail"), Detail);
		if (!bRoots || CpuBuild == nullptr || BuiltEntries != SuperSLMPrefixCacheCheck::Entries || Loaded->Model != Model.Get())
		{
			Fail(TEXT("pre.prefix_set_built"), Detail + (Leg == ELeg::Residency
				? TEXT("; expected 10 root entries with ids, all built for the CPU, against the loaded model")
				: TEXT("; expected 10 root entries of equal length, all built for the CPU, against the loaded model")));
			return false;
		}
		Add(TEXT("pre.prefix_set_built"), true, Detail);
		for (int32 I = 0; I < Loaded->Entries.Num(); ++I)
		{
			FSuperSLMPrefixSpec Spec;
			Spec.Tokens = Loaded->Entries[I].Tokens;
			Spec.SchemaName = NAME_None;
			Specs.Add(MoveTemp(Spec));
			EntryNames.Add(Loaded->Entries[I].Name);
			TArray<int32> Suffix;
			for (int32 J = 0; J < SuperSLMPrefixCacheCheck::SuffixLength; ++J) { Suffix.Add(1100 + 10 * I + J); }
			Suffixes.Add(MoveTemp(Suffix));
		}
		return true;
	}

	// Cell 10.7's Text half: the specs as a game requests them, through FindEntrySpec against the configured model
	// (USuperSLMPrefixSet::FindEntrySpec; the runtime never tokenizes). The set must hold both input kinds.
	bool SpecsFromSet()
	{
		int32 TextEntries = 0;
		int32 TokensEntries = 0;
		TArray<FString> Lengths;
		for (int32 I = 0; I < Set->Entries.Num(); ++I)
		{
			const FSuperSLMPrefixSetEntry& E = Set->Entries[I];
			FSuperSLMPrefixSpec Spec;
			FName Parent;
			if (!Set->FindEntrySpec(Cpu.GetConfiguredModel(), E.Name, Spec, Parent))
			{
				Fail(TEXT("10.7.find_entry_spec"), FString::Printf(TEXT("FindEntrySpec refused '%s' (the reason is in the log: PrefixSet: ... refused)"), *E.Name.ToString()));
				return false;
			}
			const bool bText = E.Input == ESuperSLMPrefixEntryInput::Text;
			(bText ? TextEntries : TokensEntries) += 1;
			Lengths.Add(FString::Printf(TEXT("%s %s %d"), *E.Name.ToString(), bText ? TEXT("Text") : TEXT("Tokens"), Spec.Tokens.Num()));
			Specs[I] = MoveTemp(Spec);
		}
		Report->SetNumberField(TEXT("text_entries"), TextEntries);
		Report->SetNumberField(TEXT("tokens_entries"), TokensEntries);
		Add(TEXT("10.7.mixed_set_through_find_entry_spec"), TextEntries > 0 && TokensEntries > 0,
			FString::Printf(TEXT("%d Text and %d Tokens entries, every spec from FindEntrySpec: %s"), TextEntries, TokensEntries, *FString::Join(Lengths, TEXT(", "))));
		if (TextEntries == 0 || TokensEntries == 0)
		{
			End(); // the leg checks a mixed set or nothing
			return false;
		}
		return true;
	}

	// After every measurement: each Text entry's stored ids are its text's tokenization on the configured model (a
	// runtime Tokenize, made here only to check the build; nothing above tokenized).
	void CheckTextIds()
	{
		int32 Text = 0;
		TArray<FString> Differ;
		for (const FSuperSLMPrefixSetEntry& E : Set->Entries)
		{
			if (E.Input != ESuperSLMPrefixEntryInput::Text)
			{
				continue;
			}
			++Text;
			TArray<int32> Ids;
			if (!Cpu.Tokenize(E.Text, Ids) || Ids != E.Tokens)
			{
				Differ.Add(FString::Printf(TEXT("%s (stored %d, tokenized %d)"), *E.Name.ToString(), E.Tokens.Num(), Ids.Num()));
			}
		}
		Add(TEXT("10.7.text_ids_are_the_texts"), Text > 0 && Differ.Num() == 0,
			Differ.Num() == 0 ? FString::Printf(TEXT("%d Text entries' stored ids equal Tokenize of their text"), Text) : FString::Join(Differ, TEXT("; ")));
	}

	FSuperSLMRuntimeConfig BaseConfig(int32 Blocks) const
	{
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = Blocks;
		Config.MaxSequencesPerDecodeCall = 4;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = SuperSLMExample::NumHiddenLayers;
		Config.SequenceLifecycleBudgetMs = 100000.0;
		Config.TickBudgetMs = SuperSLMExample::CpuTickBudgetMs;
		return Config;
	}

	bool Configure(const FSuperSLMRuntimeConfig& Config, const TCHAR* What)
	{
		const FSuperSLMConfigureReport R = Cpu.Configure(Model.Get(), Config);
		if (R.Result != ESuperSLMConfigureResult::Success)
		{
			Fail(FString::Printf(TEXT("pre.configure.%s"), What), FString::Printf(TEXT("Configure() refused (%d): %s"), static_cast<int32>(R.Result), *R.Message));
			return false;
		}
		if (R.Resolved.IsSet())
		{
			Resolved = R.Resolved.GetValue();
		}
		return true;
	}

	// --- Passes ------------------------------------------------------------------------------

	void BeginPass(const TArray<int32>& Order, int32 Concurrency, EStep Next)
	{
		Pass = FPass();
		Pass.Order = Order;
		Pass.Concurrency = Concurrency;
		Pass.Before = Cpu.GetPrefixCacheStats();
		Pass.FirstTickRow = Cpu.GetTickHistoryAppendedCount();
		Pass.FirstLedgerRow = Cpu.GetJobLedgerAppendedCount();
		EnterPhase(Next);
	}

	static TArray<int32> Ascending()
	{
		TArray<int32> Out;
		for (int32 I = 0; I < SuperSLMPrefixCacheCheck::Entries; ++I) { Out.Add(I); }
		return Out;
	}

	// The game thread's own time in every tick since the pass began (the plugin's per-tick bound).
	void ReadTicks()
	{
		const int64 Appended = Cpu.GetTickHistoryAppendedCount();
		for (int64 Row = Pass.FirstTickRow; Row < Appended; ++Row)
		{
			if (const FSuperSLMTickReport* T = Cpu.FindTickHistoryRow(Row))
			{
				Pass.MaxTickMs = FMath::Max(Pass.MaxTickMs, T->DurationMs);
				++Pass.Ticks;
			}
		}
		Pass.FirstTickRow = Appended;
	}

	// Advances every lane; false once the leg has failed.
	bool AdvancePass()
	{
		while (Pass.Next < Pass.Order.Num())
		{
			int32 InFlight = 0;
			for (const FLane& L : Pass.Lanes) { InFlight += L.State != FLane::EState::Done ? 1 : 0; }
			if (InFlight >= Pass.Concurrency)
			{
				break;
			}
			FLane Lane;
			Lane.Entry = Pass.Order[Pass.Next++];
			FString Error;
			if (!Cpu.RequestPrefix(Specs[Lane.Entry], Lane.Lease, Error))
			{
				Fail(TEXT("run.request"), FString::Printf(TEXT("RequestPrefix(%s) refused: %s"), *EntryNames[Lane.Entry].ToString(), *Error));
				return false;
			}
			Pass.Lanes.Add(MoveTemp(Lane));
		}
		for (FLane& L : Pass.Lanes)
		{
			const FString Name = EntryNames[L.Entry].ToString();
			FString Error;
			switch (L.State)
			{
			case FLane::EState::Requested:
			{
				const ESuperSLMPrefixLeaseState LeaseState = Cpu.GetPrefixLeaseState(L.Lease);
				if (LeaseState == ESuperSLMPrefixLeaseState::Pending)
				{
					break;
				}
				if (LeaseState != ESuperSLMPrefixLeaseState::Ready)
				{
					Fail(TEXT("run.lease"), FString::Printf(TEXT("%s's lease resolved %s"), *Name, SuperSLMPrefixCacheCheck::LeaseStateName(LeaseState)));
					return false;
				}
				L.Source = Cpu.GetPrefixSource(L.Lease);
				if (Cpu.VendSequence(L.Sequence) != ESuperSLMVendResult::Success)
				{
					Fail(TEXT("run.vend"), FString::Printf(TEXT("no sequence for %s's adopter"), *Name));
					return false;
				}
				const ESuperSLMRestoreResult Adopt = Cpu.AdoptPrefix(L.Sequence, L.Lease, L.Op, Error);
				if (Adopt != ESuperSLMRestoreResult::Pending && Adopt != ESuperSLMRestoreResult::Success)
				{
					Fail(TEXT("run.adopt"), FString::Printf(TEXT("AdoptPrefix(%s) refused (%d): %s"), *Name, static_cast<int32>(Adopt), *Error));
					return false;
				}
				L.State = FLane::EState::Adopting;
				break;
			}
			case FLane::EState::Adopting:
			{
				const ESuperSLMRestoreResult Result = Cpu.GetLifecycleOpResult(L.Op);
				if (Result == ESuperSLMRestoreResult::Pending)
				{
					break;
				}
				Cpu.ReleaseLifecycleOpHandle(L.Op);
				L.Op = {};
				if (Result != ESuperSLMRestoreResult::Success)
				{
					Fail(TEXT("run.adopt"), FString::Printf(TEXT("%s's adopt resolved %d"), *Name, static_cast<int32>(Result)));
					return false;
				}
				FSuperSLMGenerationRequest Request;
				Request.PromptTokens = Suffixes[L.Entry];
				Request.MaxNewTokens = SuperSLMPrefixCacheCheck::MaxNewTokens;
				if (!Cpu.BeginGeneration(L.Sequence, Request, Error))
				{
					Fail(TEXT("run.generate"), FString::Printf(TEXT("BeginGeneration for %s refused: %s"), *Name, *Error));
					return false;
				}
				L.State = FLane::EState::Generating;
				break;
			}
			case FLane::EState::Generating:
			{
				const ESuperSLMSequencePhase Phase = Cpu.GetPhase(L.Sequence);
				if (Phase == ESuperSLMSequencePhase::Faulted)
				{
					Fail(TEXT("run.generate"), FString::Printf(TEXT("%s's adopter faulted"), *Name));
					return false;
				}
				if (Phase != ESuperSLMSequencePhase::Complete)
				{
					break;
				}
				L.Tokens = Cpu.GetGeneratedTokens(L.Sequence);
				Cpu.ReturnSequence(L.Sequence);
				L.Sequence = {};
				Cpu.ReturnPrefix(L.Lease);
				L.State = FLane::EState::Done;
				break;
			}
			default:
				break;
			}
		}
		return true;
	}

	// The pass's tokens by entry, its sources by entry, and its figures, into the report under Key.
	TSharedRef<FJsonObject> RecordPass(const TCHAR* Key)
	{
		const FSuperSLMPrefixCacheStats After = Cpu.GetPrefixCacheStats();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FLane& L : Pass.Lanes)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), L.Entry);
			Row->SetStringField(TEXT("entry"), EntryNames[L.Entry].ToString());
			Row->SetStringField(TEXT("source"), SuperSLMPrefixCacheCheck::SourceName(L.Source));
			Row->SetArrayField(TEXT("tokens"), SuperSLMPrefixCacheCheck::JsonInts(L.Tokens));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		O->SetArrayField(TEXT("entries"), Rows);
		O->SetObjectField(TEXT("stats_before"), SuperSLMPrefixCacheCheck::JsonStats(Pass.Before));
		O->SetObjectField(TEXT("stats_after"), SuperSLMPrefixCacheCheck::JsonStats(After));
		O->SetNumberField(TEXT("prefix_prefill_calls"), static_cast<double>(After.PrefixPrefillCalls - Pass.Before.PrefixPrefillCalls));
		O->SetNumberField(TEXT("prefix_prefill_builds"), static_cast<double>(After.PrefixPrefillBuilds - Pass.Before.PrefixPrefillBuilds));
		O->SetNumberField(TEXT("ticks"), Pass.Ticks);
		O->SetNumberField(TEXT("max_tick_ms"), Pass.MaxTickMs);
		O->SetNumberField(TEXT("tick_bound_ms"), SuperSLMExample::CpuGameThreadTickBoundMs);
		O->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - PhaseStarted);
		Report->SetObjectField(Key, O);
		return O;
	}

	int32 CountSource(ESuperSLMPrefixSource Source) const
	{
		int32 N = 0;
		for (const FLane& L : Pass.Lanes) { N += L.Source == Source ? 1 : 0; }
		return N;
	}

	FString SourcesText() const
	{
		TArray<FString> Parts;
		for (const FLane& L : Pass.Lanes) { Parts.Add(FString::Printf(TEXT("%s %s"), *EntryNames[L.Entry].ToString(), SuperSLMPrefixCacheCheck::SourceName(L.Source))); }
		return FString::Join(Parts, TEXT(", "));
	}

	// Every lane's tokens equal the reference's for its entry.
	void CheckTokens(const FString& Name, const TCHAR* Against)
	{
		TArray<FString> Differ;
		for (const FLane& L : Pass.Lanes)
		{
			if (!Reference.IsValidIndex(L.Entry) || Reference[L.Entry] != L.Tokens || L.Tokens.Num() == 0)
			{
				Differ.Add(EntryNames[L.Entry].ToString());
			}
		}
		Add(Name, Differ.Num() == 0 && Pass.Lanes.Num() == SuperSLMPrefixCacheCheck::Entries,
			Differ.Num() == 0 ? FString::Printf(TEXT("%d adopters' tokens equal %s"), Pass.Lanes.Num(), Against)
				: FString::Printf(TEXT("differ from %s: %s"), Against, *FString::Join(Differ, TEXT(", "))));
	}

	void CheckNoPrefill(const FString& Name)
	{
		const FSuperSLMPrefixCacheStats After = Cpu.GetPrefixCacheStats();
		const int64 Calls = After.PrefixPrefillCalls - Pass.Before.PrefixPrefillCalls;
		const int64 Builds = After.PrefixPrefillBuilds - Pass.Before.PrefixPrefillBuilds;
		Add(Name, Calls == 0 && Builds == 0, FString::Printf(TEXT("%lld prefix prefill calls, %lld builds (the plugin's own count, FSuperSLMPrefixCacheStats::PrefixPrefillCalls)"), Calls, Builds));
	}

	void CheckTickBound(const FString& Name)
	{
		Add(Name, Pass.Ticks > 0 && Pass.MaxTickMs < SuperSLMExample::CpuGameThreadTickBoundMs,
			FString::Printf(TEXT("max %.4f ms over %d ticks; bound %.1f ms"), Pass.MaxTickMs, Pass.Ticks, SuperSLMExample::CpuGameThreadTickBoundMs));
	}

	void KeepReference()
	{
		Reference.SetNum(SuperSLMPrefixCacheCheck::Entries);
		for (const FLane& L : Pass.Lanes) { Reference[L.Entry] = L.Tokens; }
	}

	// --- Leg steps -----------------------------------------------------------------------------

	void StartLeg(UWorld* World)
	{
		if (!ParseLeg())
		{
			Fail(TEXT("pre.leg"), FString::Printf(TEXT("'%s' is not a leg (Cooked, Budget, DiskWrite, DiskRead, Determinism, Calibrate, Residency, OneJobBuild)"), *LegName));
			return;
		}
		UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: %s started"), SuperSLMPrefixCacheCheck::kLine, *LegName);
		if (!LoadSet())
		{
			return;
		}
		switch (Leg)
		{
		case ELeg::Cooked:
		case ELeg::Budget:
		case ELeg::Residency:
		{
			FSuperSLMRuntimeConfig Config = BaseConfig(SuperSLMPrefixCacheCheck::Entries);
			Config.PrefixCacheBudgetBytes = SuperSLMPrefixCacheCheck::LargeBudgetBytes;
			Cpu.UnregisterPrefixSet(Set.Get());
			if (Configure(Config, TEXT("reference")) && (Leg != ELeg::Residency || SpecsFromSet()))
			{
				BeginPass(Ascending(), SuperSLMPrefixCacheCheck::Entries, EStep::Reference);
			}
			return;
		}
		case ELeg::DiskWrite:
		case ELeg::DiskRead:
		{
			if (DiskDir.IsEmpty())
			{
				Fail(TEXT("pre.disk_directory"), TEXT("-SuperSLMExamplePrefixDiskDir= is required"));
				return;
			}
			DiskDir = FPaths::ConvertRelativePathToFull(DiskDir);
			if (Leg == ELeg::DiskWrite)
			{
				// Run 1 starts from an empty directory.
				IFileManager::Get().DeleteDirectory(*DiskDir, false, true);
				IFileManager::Get().MakeDirectory(*DiskDir, true);
			}
			else if (!LoadCompare())
			{
				return;
			}
			Report->SetStringField(TEXT("disk_directory"), DiskDir);
			Report->SetNumberField(TEXT("disk_files_at_start"), DiskFiles(TEXT("*.slup")).Num());
			FSuperSLMRuntimeConfig Config = BaseConfig(SuperSLMPrefixCacheCheck::Entries);
			Config.PrefixCacheBudgetBytes = SuperSLMPrefixCacheCheck::LargeBudgetBytes;
			Config.bPrefixDiskCacheEnabled = true;
			Config.PrefixDiskCacheDirectory = DiskDir;
			Cpu.UnregisterPrefixSet(Set.Get()); // the disk tier alone: no cooked entry may serve these
			if (Configure(Config, TEXT("disk")))
			{
				BeginPass(Ascending(), SuperSLMPrefixCacheCheck::Entries, Leg == ELeg::DiskWrite ? EStep::DiskWrite : EStep::DiskRead);
			}
			return;
		}
		case ELeg::Determinism:
		{
			Cpu.UnregisterPrefixSet(Set.Get());
			if (Configure(BaseConfig(4), TEXT("no_cache")))
			{
				SelfCheck = MakeUnique<FSuperSLMSelfCheckRun>(*Model.Get(), FString());
				EnterPhase(EStep::SelfCheckOff);
			}
			return;
		}
		case ELeg::Calibrate:
			RunEstimate(World);
			return;
		case ELeg::OneJobBuild:
			StartLongBuild();
			return;
		}
	}

	// The job ledger's rows since FirstRow (row numbers count from the last Configure()). False, naming the lost rows,
	// when the ring has already dropped some of them (review N3): a check on them would otherwise pass on fewer rows.
	bool LedgerSince(int64 FirstRow, TArray<FSuperSLMWorkerJobReport>& Out, FString& OutWhy) const
	{
		const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu.GetJobLedger();
		const int64 Appended = Cpu.GetJobLedgerAppendedCount();
		const int64 Oldest = Appended - Ledger.Num();
		if (Oldest > FirstRow)
		{
			OutWhy = FString::Printf(TEXT("the job ledger's ring dropped rows %lld..%lld of this window (rows %lld..%lld retained)"),
				FirstRow, Oldest - 1, Oldest, Appended - 1);
			return false;
		}
		for (int32 I = static_cast<int32>(FirstRow - Oldest); I < Ledger.Num(); ++I)
		{
			Out.Add(Ledger[I]);
		}
		return true;
	}

	// Leaves the level and enters it again (the map opened anew), then runs Then from the new scene's first tick.
	void TravelThen(UWorld* World, const FString& CheckName, TFunction<void()> Then)
	{
		TravelFrom = World;
		TravelCheck = CheckName;
		AfterTravel = MoveTemp(Then);
		EnterPhase(EStep::Travel);
		UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: leaving the level and entering it again"), SuperSLMPrefixCacheCheck::kLine);
		UGameplayStatics::OpenLevel(World, FName(*World->GetPackage()->GetName()));
	}

	// --- 10.7: warm at a level load, Memory hits at the next level's start, drop at its exit ---------------------

	bool IssueOps(bool bWarm)
	{
		Ops.Reset();
		PhaseBefore = Cpu.GetPrefixCacheStats();
		for (int32 I = 0; I < Specs.Num(); ++I)
		{
			FSuperSLMPrefixOp Op;
			FString Error;
			const bool bIssued = bWarm ? Cpu.WarmPrefix(Specs[I], Op, Error) : Cpu.DropPrefix(Specs[I], ESuperSLMPrefixDropScope::Memory, Op, Error);
			if (!bIssued)
			{
				Fail(bWarm ? TEXT("10.7.warm_issued") : TEXT("10.7.drop_issued"), FString::Printf(TEXT("%s(%s) refused: %s"),
					bWarm ? TEXT("WarmPrefix") : TEXT("DropPrefix"), *EntryNames[I].ToString(), *Error));
				return false;
			}
			Ops.Add(Op);
		}
		SettleFrames = 0;
		return true;
	}

	// True once no op is Pending; OutResults holds each op's result by entry.
	bool OpsResolved(TArray<ESuperSLMPrefixOpResult>& OutResults) const
	{
		OutResults.Reset();
		for (const FSuperSLMPrefixOp& Op : Ops)
		{
			FSuperSLMPrefixOpResult Result;
			if (!Cpu.GetPrefixOpResult(Op, Result) || Result.Result == ESuperSLMPrefixOpResult::Pending)
			{
				return false;
			}
			OutResults.Add(Result.Result);
		}
		return true;
	}

	static FString OpResultsText(const TArray<ESuperSLMPrefixOpResult>& Results)
	{
		TArray<FString> Parts;
		for (ESuperSLMPrefixOpResult R : Results) { Parts.Add(FString::FromInt(static_cast<int32>(R))); }
		return FString::Join(Parts, TEXT(","));
	}

	// --- 5.9: the sliced CPU cache build of 2,000 tokens (precache plan rev 1.18.8, §5.3 item 4 and §15.27) ------------
	//
	// Two builds of the same 2,000 ids, each on a fresh configuration with one worker lane, the disk tier on in a
	// directory of its own and nothing cooked registered:
	//   - the sliced build, at the check's call shape (MaxPrefillChunkBudget 64, MinPrefillChunk 8). Once its first
	//     chunk job has delivered, a generation starts on the same lane (the lane is the only one);
	//   - the one-call reference, at MaxPrefillChunkBudget = MinPrefillChunk = 2,000: its begin job, then one chunk job
	//     whose one prefill call takes the whole prefix, then the freeze. It is not U4's one-job build, which made 32
	//     calls of 64 tokens. It is a valid reference because the engine's prefill is bit-identical at every chunk size
	//     and every split of the prompt into chunks (the engine's CHANGELOG guarantee), and it is the stronger one: one
	//     call against the sliced build's floor-sized chunks shares no chunk boundary with it at all.
	// After each build an adopter generates from it, and its disk file (the frozen prefix's save, wrapped) is read.
	//
	// The grades (cell 5.9, rev 1.18.8):
	//   (a) 5.9.sliced: the build is one begin job and more than one chunk job. Each chunk job carries the chunk the
	//       chunk planner gives at its start depth (PlanPrefillChunk over min(MaxPrefillChunkBudget, tokens left),
	//       share TickBudgetMs x BudgetHeadroom, floor MinPrefillChunk; replicated below from the resolved costs) and
	//       is priced at that chunk's price there (PrefillChunkCostMs), so none is above the tick's slice except where
	//       the planner's own floor puts it (a chunk that exceeds the share earns a larger K, as a one-token chunk
	//       does). The chunks sum to L, in order (each one's position sum starts where the last one ended), on the
	//       begin job's worker thread. 5.9.build_recomputed counts one prefill call per chunk job.
	//   (b) 5.9.byte_equal_tokens and 5.9.byte_equal_save: the adopters' tokens, and the two disk files' bytes, are
	//       equal. 5.9.reference_one_call confirms the reference is one prefill of every token.
	//   (c) 5.9.interleaved: a generation job on the build's lane, with a token finished, ran between the build's
	//       first and last chunk jobs (by job id, on the same worker thread) and delivered no later than the last.
	//       Rev 1.18.10 (§15.29): and the build waits at most one generation job between two of its jobs (the lane
	//       alternates; a generation of MaxNewTokens 16 would otherwise put all its jobs in one gap).
	//   (d) Rev 1.18.11 (§15.31): read from the build's own chunk jobs only (price, K, worker ms, overran: worker ms
	//       above K x TickBudgetMs). 5.9.chunk_pricing grades, after calibration, that the median chunk's worker ms is
	//       at most its K x TickBudgetMs (the median over the chunks of worker ms / (K x TickBudgetMs) is at most 1,
	//       so a chunk of a larger K is held to its own bound). "After calibration" is a prefix_prefill pair pricing
	//       the chunks (from the saved estimate's prefix_prefill arm, or the caller's): before one exists the chunks
	//       are priced with the batched pair, which the arm replaces, and the check is recorded, not graded. The
	//       overrun count is recorded beside it. 5.9.hitch_count stays recorded, not graded, with its per-kind
	//       breakdown: the lane's counter adds every job on the lane, so it is not the build's reading.
	// Mutants (each named in the plan): the build run as one job (no chunk job: (a) and (c) red); a chunk priced at
	// PrefixBeginCostMs (the price differs from the planner's: (a) red); a freeze before the last chunk (the frozen
	// prefix is short: (b) red). Rev 1.18.10: the chunk dispatched last only, so the generation keeps the lane through
	// its continuations ((c)'s bound red).

	struct FLongBuild
	{
		bool bReference = false;
		FSuperSLMRuntimeConfig Config;
		FSuperSLMResolvedCpuConfig Costs;
		FString Dir;
		FSuperSLMPrefix Lease;
		FSuperSLMPrefixCacheStats Before;
		int64 FirstLedgerRow = 0;
		int32 HitchesBefore = 0;
		// The build's own reading, taken when its lease is Ready (and the generation is done).
		bool bRead = false;
		TArray<FSuperSLMWorkerJobReport> Rows;
		FSuperSLMPrefixCacheStats AtReady;
		int32 Hitches = 0;
		ESuperSLMPrefixSource Source = ESuperSLMPrefixSource::Pending;
		// The generation between chunks (the sliced build only).
		FSuperSLMSequence Generation;
		bool bGenerationStarted = false;
		bool bGenerationDone = false;
		TArray<int32> GenerationTokens;
		// The adopter.
		enum class EAdopter : uint8 { NotStarted, Adopting, Generating, Done } Adopter = EAdopter::NotStarted;
		FSuperSLMSequence Sequence;
		FSuperSLMLifecycleOpHandle Op;
		TArray<int32> Tokens;
		// The disk file.
		TArray<uint8> Blob;
	};
	FLongBuild Sliced;
	FLongBuild OneCall;

	FLongBuild& CurrentBuild() { return OneCall.bReference ? OneCall : Sliced; }

	TArray<FString> FilesIn(const FString& Dir, const TCHAR* Pattern) const
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / Pattern), true, false);
		return Files;
	}

	FSuperSLMPrefixSpec LongSpec() const
	{
		FSuperSLMPrefixSpec Spec;
		for (int32 J = 0; J < SuperSLMPrefixCacheCheck::LongBuildTokens; ++J) { Spec.Tokens.Add(5000 + J); }
		return Spec;
	}

	void StartLongBuild()
	{
		StartBuild(Sliced, false);
	}

	void StartBuild(FLongBuild& Build, bool bReference)
	{
		using namespace SuperSLMPrefixCacheCheck;
		Build.bReference = bReference;
		// Two sequences and one lane: the generation's and the adopter's sequences, and every job on the build's lane.
		Build.Config = BaseConfig(2);
		Build.Config.MaxConcurrentCalls = 1;
		Build.Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
		if (bReference)
		{
			Build.Config.MaxPrefillChunkBudget = LongBuildTokens;
			Build.Config.MinPrefillChunk = LongBuildTokens;
		}
		Build.Dir = ReportDirectory() / (bReference ? TEXT("OneJobBuildDisk/one_call") : TEXT("OneJobBuildDisk/sliced"));
		IFileManager::Get().DeleteDirectory(*Build.Dir, false, true);
		IFileManager::Get().MakeDirectory(*Build.Dir, true);
		Build.Config.bPrefixDiskCacheEnabled = true;
		Build.Config.PrefixDiskCacheDirectory = Build.Dir;
		Cpu.UnregisterPrefixSet(Set.Get()); // nothing cooked: the request builds
		if (!Configure(Build.Config, bReference ? TEXT("one_call_reference") : TEXT("sliced_build")))
		{
			return;
		}
		Build.Costs = Resolved;
		Build.Before = Cpu.GetPrefixCacheStats();
		Build.FirstLedgerRow = Cpu.GetJobLedgerAppendedCount();
		Build.HitchesBefore = Cpu.GetHitchCount();
		FString Error;
		if (!Cpu.RequestPrefix(LongSpec(), Build.Lease, Error))
		{
			Fail(TEXT("5.9.request"), FString::Printf(TEXT("RequestPrefix(2,000 tokens) refused: %s"), *Error));
			return;
		}
		EnterPhase(EStep::LongBuild);
	}

	static bool IsChunkRow(const FSuperSLMWorkerJobReport& Job)
	{
		return Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job.MemberSequences.Num() == 0 && Job.PromptTokens > 0;
	}

	// One tick of the build in flight. False once the leg has failed.
	bool AdvanceLongBuild(FLongBuild& Build)
	{
		using namespace SuperSLMPrefixCacheCheck;
		FString Error;
		// (c): the generation starts once a chunk job has delivered, so the build has begun and has chunks left.
		if (!Build.bReference && !Build.bGenerationStarted && !Build.bRead)
		{
			TArray<FSuperSLMWorkerJobReport> Rows;
			FString Why;
			if (!LedgerSince(Build.FirstLedgerRow, Rows, Why))
			{
				Fail(TEXT("5.9.ledger_retained"), Why);
				return false;
			}
			const bool bChunkDelivered = Rows.ContainsByPredicate([](const FSuperSLMWorkerJobReport& Job) { return IsChunkRow(Job) && Job.DeliveredAtTick >= 0; });
			if (bChunkDelivered)
			{
				if (Cpu.VendSequence(Build.Generation) != ESuperSLMVendResult::Success)
				{
					Fail(TEXT("5.9.interleaved"), TEXT("no sequence for the generation on the build's lane"));
					return false;
				}
				FSuperSLMGenerationRequest Request;
				Request.PromptTokens = Suffixes[0];
				Request.MaxNewTokens = MaxNewTokens;
				if (!Cpu.BeginGeneration(Build.Generation, Request, Error))
				{
					Fail(TEXT("5.9.interleaved"), FString::Printf(TEXT("BeginGeneration on the build's lane refused: %s"), *Error));
					return false;
				}
				Build.bGenerationStarted = true;
			}
		}
		if (Build.bGenerationStarted && !Build.bGenerationDone)
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Build.Generation);
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				Fail(TEXT("5.9.interleaved"), TEXT("the generation on the build's lane faulted"));
				return false;
			}
			if (Phase == ESuperSLMSequencePhase::Complete)
			{
				Build.GenerationTokens = Cpu.GetGeneratedTokens(Build.Generation);
				Build.bGenerationDone = true;
			}
		}
		if (!Build.bRead)
		{
			const ESuperSLMPrefixLeaseState LeaseState = Cpu.GetPrefixLeaseState(Build.Lease);
			if (LeaseState == ESuperSLMPrefixLeaseState::Pending)
			{
				return true;
			}
			if (LeaseState != ESuperSLMPrefixLeaseState::Ready)
			{
				Fail(TEXT("5.9.build_ready"), FString::Printf(TEXT("the 2,000-token %s resolved %s"), Build.bReference ? TEXT("reference build") : TEXT("build"), LeaseStateName(LeaseState)));
				return false;
			}
			if (Build.bGenerationStarted && !Build.bGenerationDone)
			{
				return true;
			}
			// The build's reading: its ledger rows, counters and hitches up to here (the adopter's jobs come after).
			FString Why;
			if (!LedgerSince(Build.FirstLedgerRow, Build.Rows, Why))
			{
				Fail(TEXT("5.9.ledger_retained"), Why);
				return false;
			}
			Build.AtReady = Cpu.GetPrefixCacheStats();
			Build.Hitches = Cpu.GetHitchCount() - Build.HitchesBefore;
			Build.Source = Cpu.GetPrefixSource(Build.Lease);
			Build.bRead = true;
			if (Build.bGenerationStarted)
			{
				Cpu.ReturnSequence(Build.Generation); // Generation keeps its id: (c) finds its jobs by it
			}
		}
		switch (Build.Adopter)
		{
		case FLongBuild::EAdopter::NotStarted:
		{
			if (Cpu.VendSequence(Build.Sequence) != ESuperSLMVendResult::Success)
			{
				return true; // the generation's sequence is still being recycled: the next tick
			}
			const ESuperSLMRestoreResult Adopt = Cpu.AdoptPrefix(Build.Sequence, Build.Lease, Build.Op, Error);
			if (Adopt != ESuperSLMRestoreResult::Pending && Adopt != ESuperSLMRestoreResult::Success)
			{
				Fail(TEXT("5.9.adopt"), FString::Printf(TEXT("AdoptPrefix(2,000 tokens) refused (%d): %s"), static_cast<int32>(Adopt), *Error));
				return false;
			}
			Build.Adopter = FLongBuild::EAdopter::Adopting;
			return true;
		}
		case FLongBuild::EAdopter::Adopting:
		{
			const ESuperSLMRestoreResult Result = Cpu.GetLifecycleOpResult(Build.Op);
			if (Result == ESuperSLMRestoreResult::Pending)
			{
				return true;
			}
			Cpu.ReleaseLifecycleOpHandle(Build.Op);
			Build.Op = {};
			if (Result != ESuperSLMRestoreResult::Success)
			{
				Fail(TEXT("5.9.adopt"), FString::Printf(TEXT("the adopt resolved %d"), static_cast<int32>(Result)));
				return false;
			}
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Suffixes[1];
			Request.MaxNewTokens = MaxNewTokens;
			if (!Cpu.BeginGeneration(Build.Sequence, Request, Error))
			{
				Fail(TEXT("5.9.adopt"), FString::Printf(TEXT("BeginGeneration for the adopter refused: %s"), *Error));
				return false;
			}
			Build.Adopter = FLongBuild::EAdopter::Generating;
			return true;
		}
		case FLongBuild::EAdopter::Generating:
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Build.Sequence);
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				Fail(TEXT("5.9.adopt"), TEXT("the adopter faulted"));
				return false;
			}
			if (Phase != ESuperSLMSequencePhase::Complete)
			{
				return true;
			}
			Build.Tokens = Cpu.GetGeneratedTokens(Build.Sequence);
			Cpu.ReturnSequence(Build.Sequence);
			Build.Sequence = {};
			Cpu.ReturnPrefix(Build.Lease);
			Build.Lease = {};
			Build.Adopter = FLongBuild::EAdopter::Done;
			return true;
		}
		case FLongBuild::EAdopter::Done:
		default:
		{
			// The write-back lands on the cache's own threads: wait for the one file, with no temporary left.
			const TArray<FString> Files = FilesIn(Build.Dir, TEXT("*.slup"));
			if (Files.Num() < 1 || FilesIn(Build.Dir, TEXT("*.tmp")).Num() > 0 || Cpu.GetPrefixCacheStats().DiskWritesIssued - Build.Before.DiskWritesIssued < 1)
			{
				return true;
			}
			if (Files.Num() != 1 || !FFileHelper::LoadFileToArray(Build.Blob, *(Build.Dir / Files[0])) || Build.Blob.Num() == 0)
			{
				Fail(TEXT("5.9.byte_equal_save"), FString::Printf(TEXT("%d files in %s; the build's file could not be read"), Files.Num(), *Build.Dir));
				return false;
			}
			if (!Build.bReference)
			{
				GradeSliced();
				if (Step == EStep::Done)
				{
					return false;
				}
				StartBuild(OneCall, true);
				return Step != EStep::Done;
			}
			GradeByteEqual();
			End();
			return false;
		}
		}
	}

	// CreatePrefix's chunk planner and price, replicated from the resolved costs (SuperSLMPrefillPlanning:
	// PlanPrefillChunk and PrefillChunkCostMs) so the ledger is checked against an independent reading. Rev 1.18.11
	// (§15.31): a cache chunk of 8 tokens or more is priced with the prefix_prefill pair when one resolved, else the
	// batched pair (the fallback).
	static bool HasPrefixPrefillPair(const FSuperSLMResolvedCpuConfig& C)
	{
		return C.PrefixPrefillPriceSource != ESuperSLMPrefixPrefillPriceSource::BatchedFallback;
	}

	static const TCHAR* PrefixPrefillSourceName(const FSuperSLMResolvedCpuConfig& C)
	{
		return C.PrefixPrefillPriceSource == ESuperSLMPrefixPrefillPriceSource::SavedEstimate ? TEXT("saved-estimate")
			: C.PrefixPrefillPriceSource == ESuperSLMPrefixPrefillPriceSource::Caller ? TEXT("caller") : TEXT("batched-fallback");
	}

	static double ChunkPriceMs(int32 Chunk, int64 StartDepth, const FSuperSLMResolvedCpuConfig& C)
	{
		const bool bBatched = Chunk >= 8; // the engine's tiled-GEMM threshold, where the batched pair starts
		const bool bPrefix = bBatched && HasPrefixPrefillPair(C);
		const double TokenMs = bPrefix ? C.PrefixPrefillTokenCostMs : bBatched ? C.BatchedPromptTokenCostMs : C.PromptTokenCostMs;
		const double PerPositionMs = bPrefix ? C.PrefixPrefillTokenCostPerPositionMs
			: bBatched ? C.BatchedPromptTokenCostPerPositionMs : C.PromptTokenCostPerPositionMs;
		const int64 PositionSum = static_cast<int64>(Chunk) * StartDepth + static_cast<int64>(Chunk) * (Chunk - 1) / 2;
		return Chunk * TokenMs + PerPositionMs * static_cast<double>(PositionSum);
	}

	static int32 PlannedChunk(int32 MaxChunk, int64 StartDepth, double ShareMs, int32 MinChunk, const FSuperSLMResolvedCpuConfig& C)
	{
		int32 Chunk = 1;
		for (int32 N = 2; N <= FMath::Min(MaxChunk, 7); ++N)
		{
			if (ChunkPriceMs(N, StartDepth, C) > ShareMs) { break; }
			Chunk = N;
		}
		for (int32 N = 8; N <= MaxChunk; ++N)
		{
			if (ChunkPriceMs(N, StartDepth, C) > ShareMs) { break; }
			Chunk = N;
		}
		return FMath::Max(Chunk, FMath::Min(MinChunk, MaxChunk));
	}

	void GradeSliced()
	{
		using namespace SuperSLMPrefixCacheCheck;
		const FLongBuild& B = Sliced;
		const int64 Builds = B.AtReady.PrefixPrefillBuilds - B.Before.PrefixPrefillBuilds;
		const int64 Calls = B.AtReady.PrefixPrefillCalls - B.Before.PrefixPrefillCalls;

		const FSuperSLMWorkerJobReport* Begin = nullptr;
		int32 Begins = 0;
		TArray<const FSuperSLMWorkerJobReport*> Chunks;
		for (const FSuperSLMWorkerJobReport& Job : B.Rows)
		{
			if (Job.Kind == ESuperSLMWorkerJobKind::PrefixBegin)
			{
				++Begins;
				Begin = &Job;
			}
			else if (IsChunkRow(Job))
			{
				Chunks.Add(&Job);
			}
		}
		Chunks.Sort([](const FSuperSLMWorkerJobReport& A, const FSuperSLMWorkerJobReport& Z) { return A.JobId < Z.JobId; });

		Add(TEXT("5.9.build_recomputed"), B.Source == ESuperSLMPrefixSource::Recomputed && Builds == 1 && Calls == Chunks.Num(),
			FString::Printf(TEXT("source %s; %lld build, %lld prefill calls, %d chunk jobs for %d tokens"), SourceName(B.Source), Builds, Calls, Chunks.Num(), LongBuildTokens));

		// (a)
		const double ShareMs = B.Config.TickBudgetMs * B.Config.BudgetHeadroom;
		int64 Done = 0;
		int32 WrongChunk = 0, WrongPrice = 0, WrongOrder = 0, OtherLane = 0, AboveShare = 0, AboveShareNotFloor = 0;
		FString FirstWrong;
		TArray<TSharedPtr<FJsonValue>> Jobs;
		for (const FSuperSLMWorkerJobReport* Job : Chunks)
		{
			const int32 Left = static_cast<int32>(FMath::Max<int64>(0, LongBuildTokens - Done));
			const int32 MaxChunk = FMath::Min(B.Config.MaxPrefillChunkBudget, Left);
			const int32 Expected = Left > 0 ? PlannedChunk(MaxChunk, Done, ShareMs, B.Config.MinPrefillChunk, B.Costs) : 0;
			const double ExpectedMs = ChunkPriceMs(Job->PromptTokens, Done, B.Costs);
			const int64 ExpectedSum = static_cast<int64>(Job->PromptTokens) * Done + static_cast<int64>(Job->PromptTokens) * (Job->PromptTokens - 1) / 2;
			const bool bChunk = Job->PromptTokens == Expected;
			const bool bPrice = FMath::Abs(Job->PlannedJobMs - ExpectedMs) <= 1e-9 * FMath::Max(1.0, ExpectedMs);
			const bool bOrder = Job->PromptPositionSum == ExpectedSum;
			const bool bLane = Begin != nullptr && Job->WorkerThreadId == Begin->WorkerThreadId;
			const bool bAbove = Job->PlannedJobMs > ShareMs;
			const bool bFloor = Job->PromptTokens == FMath::Min(B.Config.MinPrefillChunk, MaxChunk);
			WrongChunk += bChunk ? 0 : 1;
			WrongPrice += bPrice ? 0 : 1;
			WrongOrder += bOrder ? 0 : 1;
			OtherLane += bLane ? 0 : 1;
			AboveShare += bAbove ? 1 : 0;
			AboveShareNotFloor += (bAbove && !bFloor) ? 1 : 0;
			if (FirstWrong.IsEmpty() && !(bChunk && bPrice && bOrder && bLane && (!bAbove || bFloor)))
			{
				FirstWrong = FString::Printf(TEXT("; first wrong: job %lld at depth %lld, %d tokens (planner %d), planned %.4f ms (price %.4f), position sum %lld (expected %lld), thread %u"),
					Job->JobId, Done, Job->PromptTokens, Expected, Job->PlannedJobMs, ExpectedMs, Job->PromptPositionSum, ExpectedSum, Job->WorkerThreadId);
			}
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("job"), static_cast<double>(Job->JobId));
			Row->SetNumberField(TEXT("start_depth"), static_cast<double>(Done));
			Row->SetNumberField(TEXT("tokens"), Job->PromptTokens);
			Row->SetNumberField(TEXT("planner_tokens"), Expected);
			Row->SetNumberField(TEXT("planned_ms"), Job->PlannedJobMs);
			Row->SetNumberField(TEXT("k"), Job->K);
			Row->SetNumberField(TEXT("planned_at_tick"), Job->PlannedAtTick);
			Row->SetNumberField(TEXT("committed_delivery_tick"), Job->CommittedDeliveryTick);
			Row->SetNumberField(TEXT("delivered_at_tick"), Job->DeliveredAtTick);
			Row->SetNumberField(TEXT("worker_call_ms"), Job->WorkerCallMs);
			Row->SetBoolField(TEXT("worker_overran"), Job->bWorkerOverran);
			Row->SetBoolField(TEXT("hitch"), Job->bHitch);
			Jobs.Add(MakeShared<FJsonValueObject>(Row));
			Done += Job->PromptTokens;
		}
		Report->SetArrayField(TEXT("chunk_jobs"), Jobs);
		Report->SetNumberField(TEXT("build_tokens"), LongBuildTokens);
		Report->SetNumberField(TEXT("share_ms"), ShareMs);
		Report->SetNumberField(TEXT("chunks_above_share_at_the_floor"), AboveShare - AboveShareNotFloor);
		if (Begin != nullptr)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("job"), static_cast<double>(Begin->JobId));
			Row->SetNumberField(TEXT("planned_ms"), Begin->PlannedJobMs);
			Row->SetNumberField(TEXT("k"), Begin->K);
			Row->SetNumberField(TEXT("delivered_at_tick"), Begin->DeliveredAtTick);
			Row->SetNumberField(TEXT("worker_call_ms"), Begin->WorkerCallMs);
			Report->SetObjectField(TEXT("begin_job"), Row);
		}
		Add(TEXT("5.9.sliced"), Begins == 1 && Chunks.Num() > 1 && Done == LongBuildTokens && WrongChunk == 0 && WrongPrice == 0 && WrongOrder == 0
			&& OtherLane == 0 && AboveShareNotFloor == 0,
			FString::Printf(TEXT("%d begin job, %d chunk jobs summing to %lld of %d tokens; %d not the planner's chunk, %d not its price, %d out of order, %d off the begin's lane; share %.3f ms, %d chunks above it, all at the planner's floor (MinPrefillChunk %d)%s"),
				Begins, Chunks.Num(), Done, LongBuildTokens, WrongChunk, WrongPrice, WrongOrder, OtherLane, ShareMs, AboveShare, B.Config.MinPrefillChunk,
				AboveShareNotFloor > 0 ? *FString::Printf(TEXT(" -- %d above it NOT at the floor"), AboveShareNotFloor) : TEXT(""))
				+ FirstWrong);

		// (c), with its bound (rev 1.18.10, §15.29, code review S1): between two consecutive build jobs (the begin and
		// each chunk, by job id) at most one generation job is posted on the lane.
		const FSuperSLMWorkerJobReport* Between = nullptr;
		int32 GenerationJobs = 0;
		TArray<int64> BuildJobIds;
		if (Begin != nullptr)
		{
			BuildJobIds.Add(Begin->JobId);
		}
		for (const FSuperSLMWorkerJobReport* Job : Chunks)
		{
			BuildJobIds.Add(Job->JobId);
		}
		BuildJobIds.Sort();
		TArray<int32> GenerationJobsInGap;
		GenerationJobsInGap.SetNumZeroed(FMath::Max(0, BuildJobIds.Num() - 1));
		if (Chunks.Num() >= 2 && Begin != nullptr)
		{
			const FSuperSLMWorkerJobReport& First = *Chunks[0];
			const FSuperSLMWorkerJobReport& Last = *Chunks.Last();
			for (const FSuperSLMWorkerJobReport& Job : B.Rows)
			{
				if (Job.Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || !Job.MemberSequences.Contains(Sliced.Generation))
				{
					continue;
				}
				++GenerationJobs;
				for (int32 Gap = 0; Gap + 1 < BuildJobIds.Num(); ++Gap)
				{
					if (Job.JobId > BuildJobIds[Gap] && Job.JobId < BuildJobIds[Gap + 1])
					{
						++GenerationJobsInGap[Gap];
						break;
					}
				}
				if (Between == nullptr && Job.TokenFinishes > 0 && Job.JobId > First.JobId && Job.JobId < Last.JobId && Job.WorkerThreadId == Begin->WorkerThreadId
					&& Job.DeliveredAtTick >= 0 && Job.DeliveredAtTick <= Last.DeliveredAtTick)
				{
					Between = &Job;
				}
			}
		}
		int32 MaxInGap = 0;
		int32 GapsWithGeneration = 0;
		for (int32 N : GenerationJobsInGap)
		{
			MaxInGap = FMath::Max(MaxInGap, N);
			GapsWithGeneration += N > 0 ? 1 : 0;
		}
		Report->SetNumberField(TEXT("generation_jobs"), GenerationJobs);
		Report->SetNumberField(TEXT("max_generation_jobs_between_build_jobs"), MaxInGap);
		Report->SetArrayField(TEXT("generation_tokens"), JsonInts(B.GenerationTokens));
		const FString Bound = FString::Printf(TEXT("; at most %d generation job(s) between two build jobs (bound 1), in %d of %d gaps"), MaxInGap, GapsWithGeneration, GenerationJobsInGap.Num());
		Add(TEXT("5.9.interleaved"), B.bGenerationDone && B.GenerationTokens.Num() > 0 && Between != nullptr && MaxInGap <= 1,
			(Between == nullptr ? FString::Printf(TEXT("no generation job with a token finished between the first and last chunk jobs (generation %s, %d of its jobs in the window, %d tokens)"),
				B.bGenerationStarted ? TEXT("started") : TEXT("never started"), GenerationJobs, B.GenerationTokens.Num())
				: FString::Printf(TEXT("generation job %lld (%d token(s) finished) ran between chunk jobs %lld and %lld on the build's lane and delivered at tick %d (last chunk %d); %d generation jobs, %d tokens"),
					Between->JobId, Between->TokenFinishes, Chunks[0]->JobId, Chunks.Last()->JobId, Between->DeliveredAtTick, Chunks.Last()->DeliveredAtTick, GenerationJobs, B.GenerationTokens.Num()))
				+ Bound);

		// (d), rev 1.18.11 (§15.31). The build's own chunk jobs: each one's price, K, worker ms and overrun (worker ms
		// above K x TickBudgetMs). Graded once a prefix_prefill pair prices the chunks: the median over the chunks of
		// worker ms / (K x TickBudgetMs) is at most 1. With the batched fallback it is recorded, not graded.
		const double TickMs = B.Config.TickBudgetMs;
		TArray<double> Ratios, PlannedPerToken, WorkerPerToken;
		int32 ChunkOverruns = 0;
		for (const FSuperSLMWorkerJobReport* Job : Chunks)
		{
			if (Job->DeliveredAtTick < 0 || Job->PromptTokens <= 0)
			{
				continue;
			}
			const double BoundMs = static_cast<double>(FMath::Max(1, Job->K)) * TickMs;
			Ratios.Add(BoundMs > 0.0 ? Job->WorkerCallMs / BoundMs : 0.0);
			ChunkOverruns += Job->WorkerCallMs > BoundMs ? 1 : 0;
			PlannedPerToken.Add(Job->PlannedJobMs / Job->PromptTokens);
			WorkerPerToken.Add(Job->WorkerCallMs / Job->PromptTokens);
		}
		// The conventional median (the mean of the two middle values of an even count).
		auto Median = [](TArray<double> V)
		{
			if (V.Num() == 0) { return 0.0; }
			V.Sort();
			const int32 N = V.Num();
			return N % 2 == 1 ? V[N / 2] : 0.5 * (V[N / 2 - 1] + V[N / 2]);
		};
		const double MedianRatio = Median(Ratios);
		const double PlannedMsPerToken = Median(PlannedPerToken);
		const double WorkerMsPerToken = Median(WorkerPerToken);
		const bool bGraded = HasPrefixPrefillPair(B.Costs);
		const TCHAR* const Profile = B.Costs.CostProfile == ESuperSLMCostProfile::SavedEstimate ? TEXT("saved-estimate")
			: B.Costs.CostProfile == ESuperSLMCostProfile::Caller ? TEXT("caller") : TEXT("mode-default");
		const double PairTokenMs = bGraded ? B.Costs.PrefixPrefillTokenCostMs : B.Costs.BatchedPromptTokenCostMs;
		const double PairSlopeMs = bGraded ? B.Costs.PrefixPrefillTokenCostPerPositionMs : B.Costs.BatchedPromptTokenCostPerPositionMs;

		TSharedRef<FJsonObject> Pricing = MakeShared<FJsonObject>();
		Pricing->SetBoolField(TEXT("graded"), bGraded);
		Pricing->SetStringField(TEXT("price_source"), PrefixPrefillSourceName(B.Costs));
		Pricing->SetNumberField(TEXT("chunks"), Ratios.Num());
		Pricing->SetNumberField(TEXT("overruns"), ChunkOverruns);
		Pricing->SetNumberField(TEXT("median_worker_over_k_budget"), MedianRatio);
		Pricing->SetNumberField(TEXT("tick_budget_ms"), TickMs);
		Report->SetObjectField(TEXT("chunk_pricing"), Pricing);
		const FString PricingDetail = FString::Printf(TEXT("median chunk worker ms / (K x TickBudgetMs %.3f) = %.3f over %d chunks (bound 1); %d chunk(s) overran; priced with the %s pair %.3f ms/token + %.6f ms/position (pair source %s; cost profile %s); planned a median %.3f ms/token, worker %.3f ms/token"),
			TickMs, MedianRatio, Ratios.Num(), ChunkOverruns, bGraded ? TEXT("prefix_prefill") : TEXT("batched"), PairTokenMs, PairSlopeMs,
			PrefixPrefillSourceName(B.Costs), Profile, PlannedMsPerToken, WorkerMsPerToken);
		Add(TEXT("5.9.chunk_pricing"), bGraded ? (Ratios.Num() > 0 && MedianRatio <= 1.0) : true,
			bGraded ? PricingDetail
				: TEXT("recorded, not graded: no prefix_prefill pair resolved (the batched pair prices the chunks; run the calibration, plan rev 1.18.11 section 15.31); ") + PricingDetail);

		// The lane's hitch counter, recorded beside it, not graded (§15.31). USuperSLMSubsystem::GetHitchCount adds one
		// for each late delivery (after the committed tick, with the worker given K x TickBudgetMs of wall time from
		// the post) and one for each worker overrun, over every job on the lane, so one mispriced chunk counts twice
		// and a generation job queued behind a chunk counts too. The breakdown reads the same two flags from the
		// ledger rows of the build's window, by kind.
		Report->SetNumberField(TEXT("hitch_count"), B.Hitches);
		Report->SetObjectField(TEXT("stats_after_build"), JsonStats(B.AtReady));
		int32 ChunkLate = 0, ChunkOverran = 0, OtherLate = 0, OtherOverran = 0;
		for (const FSuperSLMWorkerJobReport& Job : B.Rows)
		{
			if (Job.DeliveredAtTick < 0)
			{
				continue;
			}
			if (IsChunkRow(Job))
			{
				ChunkLate += Job.bHitch ? 1 : 0;
				ChunkOverran += Job.bWorkerOverran ? 1 : 0;
			}
			else
			{
				OtherLate += Job.bHitch ? 1 : 0;
				OtherOverran += Job.bWorkerOverran ? 1 : 0;
			}
		}
		TSharedRef<FJsonObject> Hitch = MakeShared<FJsonObject>();
		Hitch->SetNumberField(TEXT("chunk_late"), ChunkLate);
		Hitch->SetNumberField(TEXT("chunk_overran"), ChunkOverran);
		Hitch->SetNumberField(TEXT("other_late"), OtherLate);
		Hitch->SetNumberField(TEXT("other_overran"), OtherOverran);
		Hitch->SetNumberField(TEXT("rows_sum"), ChunkLate + ChunkOverran + OtherLate + OtherOverran);
		Report->SetObjectField(TEXT("hitch_breakdown"), Hitch);
		TSharedRef<FJsonObject> Price = MakeShared<FJsonObject>();
		Price->SetStringField(TEXT("cost_profile"), Profile);
		Price->SetStringField(TEXT("prefix_prefill_price_source"), PrefixPrefillSourceName(B.Costs));
		Price->SetNumberField(TEXT("prefix_prefill_token_cost_ms"), B.Costs.PrefixPrefillTokenCostMs);
		Price->SetNumberField(TEXT("prefix_prefill_token_cost_per_position_ms"), B.Costs.PrefixPrefillTokenCostPerPositionMs);
		Price->SetNumberField(TEXT("batched_prompt_token_cost_ms"), B.Costs.BatchedPromptTokenCostMs);
		Price->SetNumberField(TEXT("batched_prompt_token_cost_per_position_ms"), B.Costs.BatchedPromptTokenCostPerPositionMs);
		Price->SetNumberField(TEXT("prompt_token_cost_ms"), B.Costs.PromptTokenCostMs);
		Price->SetNumberField(TEXT("prompt_token_cost_per_position_ms"), B.Costs.PromptTokenCostPerPositionMs);
		Price->SetNumberField(TEXT("tick_budget_ms"), B.Config.TickBudgetMs);
		Price->SetNumberField(TEXT("budget_headroom"), B.Config.BudgetHeadroom);
		Price->SetNumberField(TEXT("chunk_planned_ms_per_token_median"), PlannedMsPerToken);
		Price->SetNumberField(TEXT("chunk_worker_ms_per_token_median"), WorkerMsPerToken);
		Report->SetObjectField(TEXT("chunk_price"), Price);
		Add(TEXT("5.9.hitch_count"), true, FString::Printf(TEXT("%d hitch(es) on the lane while the sliced build ran; recorded, not graded (section 15.31: 5.9.chunk_pricing reads the build's own chunks). Chunks: %d late, %d overran; other jobs: %d late, %d overran"),
			B.Hitches, ChunkLate, ChunkOverran, OtherLate, OtherOverran));
	}

	void GradeByteEqual()
	{
		using namespace SuperSLMPrefixCacheCheck;
		int32 Chunks = 0;
		int32 ChunkTokens = 0;
		for (const FSuperSLMWorkerJobReport& Job : OneCall.Rows)
		{
			if (IsChunkRow(Job)) { ++Chunks; ChunkTokens = Job.PromptTokens; }
		}
		const int64 Calls = OneCall.AtReady.PrefixPrefillCalls - OneCall.Before.PrefixPrefillCalls;
		Add(TEXT("5.9.reference_one_call"), OneCall.Source == ESuperSLMPrefixSource::Recomputed && Chunks == 1 && ChunkTokens == LongBuildTokens && Calls == 1,
			FString::Printf(TEXT("source %s; %d chunk job(s), the last of %d tokens; %lld prefill call(s)"), SourceName(OneCall.Source), Chunks, ChunkTokens, Calls));
		Report->SetArrayField(TEXT("sliced_tokens"), JsonInts(Sliced.Tokens));
		Report->SetArrayField(TEXT("one_call_tokens"), JsonInts(OneCall.Tokens));
		Report->SetNumberField(TEXT("sliced_save_bytes"), Sliced.Blob.Num());
		Report->SetNumberField(TEXT("one_call_save_bytes"), OneCall.Blob.Num());
		Report->SetNumberField(TEXT("one_call_hitch_count"), OneCall.Hitches);
		Add(TEXT("5.9.byte_equal_tokens"), Sliced.Tokens.Num() > 0 && Sliced.Tokens == OneCall.Tokens,
			FString::Printf(TEXT("the adopters' tokens: sliced %d, one-call %d, %s"), Sliced.Tokens.Num(), OneCall.Tokens.Num(), Sliced.Tokens == OneCall.Tokens ? TEXT("equal") : TEXT("DIFFER")));
		int32 FirstDiff = -1;
		for (int32 I = 0; I < FMath::Min(Sliced.Blob.Num(), OneCall.Blob.Num()); ++I)
		{
			if (Sliced.Blob[I] != OneCall.Blob[I]) { FirstDiff = I; break; }
		}
		Add(TEXT("5.9.byte_equal_save"), Sliced.Blob.Num() > 0 && Sliced.Blob == OneCall.Blob,
			Sliced.Blob == OneCall.Blob ? FString::Printf(TEXT("the two disk files (the frozen prefix's save, wrapped) are byte-equal, %d bytes"), Sliced.Blob.Num())
				: FString::Printf(TEXT("the disk files differ: %d and %d bytes, first differing byte %d"), Sliced.Blob.Num(), OneCall.Blob.Num(), FirstDiff));
	}

	TArray<FString> DiskFiles(const TCHAR* Pattern) const
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(DiskDir / Pattern), true, false);
		return Files;
	}

	bool LoadCompare()
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Pass1 = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (ComparePath.IsEmpty() || !FFileHelper::LoadFileToString(Text, *ComparePath)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("disk_write"), Pass1) || !(*Pass1)->TryGetArrayField(TEXT("entries"), Rows))
		{
			Fail(TEXT("pre.run1_report"), FString::Printf(TEXT("run 1's report could not be read from '%s' (-SuperSLMExamplePrefixCompare=)"), *ComparePath));
			return false;
		}
		bool bRun1Passed = false;
		Root->TryGetBoolField(TEXT("pass"), bRun1Passed);
		Add(TEXT("pre.run1_passed"), bRun1Passed, ComparePath);
		Reference.SetNum(SuperSLMPrefixCacheCheck::Entries);
		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			int32 Index = -1;
			const TArray<TSharedPtr<FJsonValue>>* Tokens = nullptr;
			if (Row.IsValid() && Row->TryGetNumberField(TEXT("index"), Index) && Reference.IsValidIndex(Index) && Row->TryGetArrayField(TEXT("tokens"), Tokens))
			{
				for (const TSharedPtr<FJsonValue>& T : *Tokens) { Reference[Index].Add(static_cast<int32>(T->AsNumber())); }
			}
		}
		return true;
	}

	void RunEstimate(UWorld* World)
	{
		if (CalibrateArtifact.IsEmpty() || !FPaths::FileExists(CalibrateArtifact))
		{
			Fail(TEXT("pre.calibrate_artifact"), FString::Printf(TEXT("the .sslm '%s' (-SuperSLMExampleCalibrateArtifact=) does not exist"), *CalibrateArtifact));
			return;
		}
		const FString IniPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini"));
		IFileManager::Get().Delete(*IniPath, false, true, true); // so the profile read below is this run's
		const FString Command = FString::Printf(TEXT("SuperSLM.EstimateCosts \"%s\" --write-ini"), *FPaths::ConvertRelativePathToFull(CalibrateArtifact));
		EstimateDirsBefore = EstimateDirs(); // rev 1.18.12 (§15.32): the run's own directory is the one it adds
		UE_LOG(LogSuperSLMPrefixCacheCheck, Display, TEXT("%s: running %s (blocks the game thread for several minutes)"), SuperSLMPrefixCacheCheck::kLine, *Command);
		const double T0 = FPlatformTime::Seconds();
		const bool bRan = IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, World);
		Report->SetNumberField(TEXT("estimate_s"), FPlatformTime::Seconds() - T0);
		LegStarted = FPlatformTime::Seconds();
		FString Ini;
		const bool bWritten = bRan && FFileHelper::LoadFileToString(Ini, *IniPath);
		TArray<FString> PerMiB;
		bool bAllPositive = bWritten;
		// Rev 1.18.11 (§15.31, review S1): the cache chunk's prefix_prefill pair, one of each key per decode mode.
		TArray<FString> PrefixTokens, PrefixSlopes;
		bool bPrefixInDomain = bWritten;
		TArray<FString> Lines;
		Ini.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Key, Value;
			if (!Line.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Key == TEXT("RestoreCostPerMiBMs"))
			{
				PerMiB.Add(Value);
				bAllPositive &= Value.IsNumeric() && FCString::Atod(*Value) > 0.0;
			}
			else if (Key == TEXT("PrefixPrefillTokenCostMs"))
			{
				PrefixTokens.Add(Value);
				bPrefixInDomain &= Value.IsNumeric() && FMath::IsFinite(FCString::Atod(*Value)) && FCString::Atod(*Value) > 0.0;
			}
			else if (Key == TEXT("PrefixPrefillTokenCostPerPositionMs"))
			{
				PrefixSlopes.Add(Value);
				bPrefixInDomain &= Value.IsNumeric() && FMath::IsFinite(FCString::Atod(*Value)) && FCString::Atod(*Value) >= 0.0;
			}
		}
		Report->SetStringField(TEXT("calibrated_costs_ini"), IniPath);
		Report->SetStringField(TEXT("calibrated_costs_text"), Ini);
		Add(TEXT("calibrate.estimate_wrote_per_mib"), bWritten && PerMiB.Num() == 2 && bAllPositive,
			bWritten ? FString::Printf(TEXT("%s: RestoreCostPerMiBMs = %s (one per decode mode)"), *IniPath, *FString::Join(PerMiB, TEXT(", ")))
				: FString::Printf(TEXT("%s was not written (command %s)"), *IniPath, bRan ? TEXT("ran") : TEXT("did not run")));
		Add(TEXT("calibrate.estimate_wrote_prefix_prefill"), bWritten && PrefixTokens.Num() == 2 && PrefixSlopes.Num() == 2 && bPrefixInDomain,
			bWritten ? FString::Printf(TEXT("%s: PrefixPrefillTokenCostMs = %s; PrefixPrefillTokenCostPerPositionMs = %s (one each per decode mode; a positive cost and a non-negative slope)"),
				*IniPath, *FString::Join(PrefixTokens, TEXT(", ")), *FString::Join(PrefixSlopes, TEXT(", ")))
				: FString::Printf(TEXT("%s was not written (command %s)"), *IniPath, bRan ? TEXT("ran") : TEXT("did not run")));
		// The thirteen costs unset and the decode mode and task count left to resolve, so the saved estimate matches.
		FSuperSLMRuntimeConfig Config = BaseConfig(SuperSLMPrefixCacheCheck::Entries);
		Config.PrefixCacheBudgetBytes = SuperSLMPrefixCacheCheck::LargeBudgetBytes;
		if (!Configure(Config, TEXT("calibrated")))
		{
			return;
		}
		Add(TEXT("calibrate.configure_reads_saved_estimate"), Resolved.CostProfile == ESuperSLMCostProfile::SavedEstimate && Resolved.RestoreCostPerMiBMs > 0.0,
			FString::Printf(TEXT("cost profile %s; RestoreCostMs %.4f; RestoreCostPerMiBMs %.6f; decode mode %d; tasks %d"),
				Resolved.CostProfile == ESuperSLMCostProfile::SavedEstimate ? TEXT("saved-estimate") : Resolved.CostProfile == ESuperSLMCostProfile::Caller ? TEXT("caller") : TEXT("mode-default"),
				Resolved.RestoreCostMs, Resolved.RestoreCostPerMiBMs, Resolved.bDecodeModeEffective ? 1 : 0, Resolved.ParallelTasks));
		// Rev 1.18.11 (§15.31, review S1): the pair resolves from the saved estimate, so a sliced build after this leg
		// is priced with it and 5.9 (d) is graded. A pair Configure ignored (one key, out of the domain) or dropped
		// (the saved profile fell back to the mode profile) is red here, not a silently ungraded 5.9 (d).
		Add(TEXT("calibrate.configure_reads_prefix_prefill"), Resolved.PrefixPrefillPriceSource == ESuperSLMPrefixPrefillPriceSource::SavedEstimate,
			FString::Printf(TEXT("prefix_prefill pair source %s; %.4f ms/token + %.6f ms/position; cost profile %s"),
				PrefixPrefillSourceName(Resolved), Resolved.PrefixPrefillTokenCostMs, Resolved.PrefixPrefillTokenCostPerPositionMs,
				Resolved.CostProfile == ESuperSLMCostProfile::SavedEstimate ? TEXT("saved-estimate") : Resolved.CostProfile == ESuperSLMCostProfile::Caller ? TEXT("caller") : TEXT("mode-default")));
		ReadIsolatedArm();
		Cpu.RegisterPrefixSet(Set.Get());
		BeginPass(Ascending(), SuperSLMPrefixCacheCheck::Entries, EStep::CalibratePass);
	}

	// --- Calibrate, rev 1.18.12 (§15.32): the in-situ prefix_prefill arm ---------------------------------------
	//
	// The estimate's prefix_prefill arm is isolated: it blocks the game thread, so it times sslm_prefix_prefill with
	// the game not ticking, outside the lanes and away from the build's interleaved generation, and the box read its
	// pair 1.5x low against 5.9 (d)'s chunks. The in-situ arm runs where the game ticks: after the restores, two cache
	// builds of the arm's length (2,048 tokens from depth 0) through the subsystem's real path, each on a fresh
	// configuration that leaves the decode mode, task count and costs to resolve (so the saved estimate's), with one
	// worker lane and nothing cooked registered, as 5.9's sliced build:
	//   0. without a generation (attribution only);
	//   1. with 5.9 (c)'s one generation started on the build's lane once a chunk job has delivered.
	// Build 1's ledger rows, the same rows 5.9 (d) grades, are handed to USuperSLMSubsystem::WriteInSituPrefixPrefillPrice,
	// which fits the pair (no margin factor) and writes it over the estimate's in CalibratedCosts.ini. Graded: each
	// build recomputed and measured, the generation interleaved in build 1, the pair written and resolved back by the
	// next Configure(). Recorded (§15.32's attribution): the median worker ms per token of the isolated arm (the
	// estimate's modeN.prefix_prefill.csv, at the resolved mode), of build 0 and of build 1.
	struct FInSituBuild
	{
		bool bWithGeneration = false;
		FSuperSLMRuntimeConfig Config;
		FSuperSLMResolvedCpuConfig Costs;
		FSuperSLMPrefix Lease;
		int64 FirstLedgerRow = 0;
		FSuperSLMSequence Generation;
		bool bGenerationStarted = false;
		bool bGenerationDone = false;
		TArray<int32> GenerationTokens;
		TArray<FSuperSLMWorkerJobReport> Rows;
		ESuperSLMPrefixSource Source = ESuperSLMPrefixSource::Pending;
		int32 ChunkRows = 0;
		double MedianWorkerMsPerToken = 0.0;
	};
	FInSituBuild InSitu[2];
	int32 InSituIndex = 0;
	TArray<FString> EstimateDirsBefore;
	FString IsolatedCsv;
	int32 IsolatedChunks = 0;
	double IsolatedMsPerToken = -1.0;

	FString EstimatesDir() const
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SuperSLM/Estimates"));
	}

	TArray<FString> EstimateDirs() const
	{
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(EstimatesDir() / TEXT("*")), false, true);
		return Dirs;
	}

	static double MedianOf(TArray<double> V)
	{
		if (V.Num() == 0) { return 0.0; }
		V.Sort();
		const int32 N = V.Num();
		return N % 2 == 1 ? V[N / 2] : 0.5 * (V[N / 2 - 1] + V[N / 2]);
	}

	// The isolated arm's attribution reading: this run's estimate directory (the one the command added), its CSV at
	// the decode mode this configuration resolved, the median ms per token over its chunks.
	void ReadIsolatedArm()
	{
		TArray<FString> New;
		for (const FString& Dir : EstimateDirs())
		{
			if (!EstimateDirsBefore.Contains(Dir)) { New.Add(Dir); }
		}
		if (New.Num() != 1)
		{
			IsolatedCsv = FString::Printf(TEXT("(%d new estimate directories in %s)"), New.Num(), *EstimatesDir());
			return;
		}
		IsolatedCsv = EstimatesDir() / New[0] / FString::Printf(TEXT("mode%d.prefix_prefill.csv"), Resolved.bDecodeModeEffective ? 1 : 0);
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *IsolatedCsv))
		{
			return;
		}
		TArray<double> PerToken;
		for (int32 I = 1; I < Lines.Num(); ++I) // the header first
		{
			TArray<FString> Cells;
			Lines[I].ParseIntoArray(Cells, TEXT(","), false);
			if (Cells.Num() == 5 && FCString::Atoi(*Cells[3]) > 0)
			{
				PerToken.Add(FCString::Atod(*Cells[4]) / FCString::Atoi(*Cells[3]));
			}
		}
		IsolatedChunks = PerToken.Num();
		IsolatedMsPerToken = PerToken.Num() > 0 ? MedianOf(PerToken) : -1.0;
	}

	void StartInSitu(int32 Index)
	{
		using namespace SuperSLMPrefixCacheCheck;
		FInSituBuild& B = InSitu[Index];
		B = FInSituBuild();
		B.bWithGeneration = Index == 1;
		B.Config = BaseConfig(2);
		B.Config.MaxConcurrentCalls = 1;
		B.Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
		Cpu.UnregisterPrefixSet(Set.Get()); // nothing cooked: the request builds
		if (!Configure(B.Config, B.bWithGeneration ? TEXT("insitu_with_generation") : TEXT("insitu_no_generation")))
		{
			return;
		}
		B.Costs = Resolved;
		B.FirstLedgerRow = Cpu.GetJobLedgerAppendedCount();
		FSuperSLMPrefixSpec Spec;
		for (int32 J = 0; J < InSituBuildTokens; ++J) { Spec.Tokens.Add(5000 + J); }
		FString Error;
		if (!Cpu.RequestPrefix(Spec, B.Lease, Error))
		{
			Fail(TEXT("calibrate.insitu_request"), FString::Printf(TEXT("RequestPrefix(%d tokens) refused: %s"), InSituBuildTokens, *Error));
			return;
		}
		InSituIndex = Index;
		EnterPhase(EStep::InSituBuild);
	}

	void AdvanceInSitu()
	{
		using namespace SuperSLMPrefixCacheCheck;
		FInSituBuild& B = InSitu[InSituIndex];
		FString Error;
		if (B.bWithGeneration && !B.bGenerationStarted)
		{
			TArray<FSuperSLMWorkerJobReport> Rows;
			FString Why;
			if (!LedgerSince(B.FirstLedgerRow, Rows, Why))
			{
				Fail(TEXT("calibrate.insitu_ledger_retained"), Why);
				return;
			}
			if (Rows.ContainsByPredicate([](const FSuperSLMWorkerJobReport& Job) { return IsChunkRow(Job) && Job.DeliveredAtTick >= 0; }))
			{
				if (Cpu.VendSequence(B.Generation) != ESuperSLMVendResult::Success)
				{
					Fail(TEXT("calibrate.insitu_build_with_generation"), TEXT("no sequence for the generation on the build's lane"));
					return;
				}
				FSuperSLMGenerationRequest Request;
				Request.PromptTokens = Suffixes[0];
				Request.MaxNewTokens = MaxNewTokens;
				if (!Cpu.BeginGeneration(B.Generation, Request, Error))
				{
					Fail(TEXT("calibrate.insitu_build_with_generation"), FString::Printf(TEXT("BeginGeneration on the build's lane refused: %s"), *Error));
					return;
				}
				B.bGenerationStarted = true;
			}
		}
		if (B.bGenerationStarted && !B.bGenerationDone)
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(B.Generation);
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				Fail(TEXT("calibrate.insitu_build_with_generation"), TEXT("the generation on the build's lane faulted"));
				return;
			}
			if (Phase == ESuperSLMSequencePhase::Complete)
			{
				B.GenerationTokens = Cpu.GetGeneratedTokens(B.Generation);
				B.bGenerationDone = true;
			}
		}
		const ESuperSLMPrefixLeaseState LeaseState = Cpu.GetPrefixLeaseState(B.Lease);
		if (LeaseState == ESuperSLMPrefixLeaseState::Pending)
		{
			return;
		}
		if (LeaseState != ESuperSLMPrefixLeaseState::Ready)
		{
			Fail(TEXT("calibrate.insitu_build_ready"), FString::Printf(TEXT("the %d-token in-situ build %d resolved %s"), InSituBuildTokens, InSituIndex, LeaseStateName(LeaseState)));
			return;
		}
		if (B.bGenerationStarted && !B.bGenerationDone)
		{
			return;
		}
		FString Why;
		if (!LedgerSince(B.FirstLedgerRow, B.Rows, Why))
		{
			Fail(TEXT("calibrate.insitu_ledger_retained"), Why);
			return;
		}
		B.Source = Cpu.GetPrefixSource(B.Lease);
		if (B.bGenerationStarted)
		{
			Cpu.ReturnSequence(B.Generation); // Generation keeps its id: the grade finds its jobs by it
		}
		Cpu.ReturnPrefix(B.Lease);
		B.Lease = {};
		GradeInSitu(InSituIndex);
		if (Step == EStep::Done)
		{
			return;
		}
		if (InSituIndex == 0)
		{
			StartInSitu(1);
			return;
		}
		FinishInSitu();
	}

	// One build's chunk rows (as WriteInSituPrefixPrefillPrice reads them: delivered, 8 tokens or more, measured) into
	// the report, and its check.
	void GradeInSitu(int32 Index)
	{
		FInSituBuild& B = InSitu[Index];
		TArray<double> PerToken;
		TArray<TSharedPtr<FJsonValue>> Jobs;
		int64 FirstChunk = TNumericLimits<int64>::Max(), LastChunk = 0;
		for (const FSuperSLMWorkerJobReport& Job : B.Rows)
		{
			if (!IsChunkRow(Job) || Job.DeliveredAtTick < 0 || Job.PromptTokens < 8 || !(Job.WorkerCallMs > 0.0))
			{
				continue;
			}
			PerToken.Add(Job.WorkerCallMs / Job.PromptTokens);
			FirstChunk = FMath::Min(FirstChunk, Job.JobId);
			LastChunk = FMath::Max(LastChunk, Job.JobId);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("job"), static_cast<double>(Job.JobId));
			Row->SetNumberField(TEXT("start_depth"), static_cast<double>((Job.PromptPositionSum - static_cast<int64>(Job.PromptTokens) * (Job.PromptTokens - 1) / 2) / Job.PromptTokens));
			Row->SetNumberField(TEXT("tokens"), Job.PromptTokens);
			Row->SetNumberField(TEXT("planned_ms"), Job.PlannedJobMs);
			Row->SetNumberField(TEXT("k"), Job.K);
			Row->SetNumberField(TEXT("worker_call_ms"), Job.WorkerCallMs);
			Row->SetBoolField(TEXT("worker_overran"), Job.bWorkerOverran);
			Jobs.Add(MakeShared<FJsonValueObject>(Row));
		}
		B.ChunkRows = PerToken.Num();
		B.MedianWorkerMsPerToken = MedianOf(PerToken);
		int32 GenerationJobs = 0, GenerationBetween = 0;
		for (const FSuperSLMWorkerJobReport& Job : B.Rows)
		{
			if (Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Job.MemberSequences.Num() > 0)
			{
				++GenerationJobs;
				GenerationBetween += (B.bGenerationStarted && Job.MemberSequences.Contains(B.Generation) && Job.JobId > FirstChunk && Job.JobId < LastChunk) ? 1 : 0;
			}
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("source"), SuperSLMPrefixCacheCheck::SourceName(B.Source));
		Out->SetNumberField(TEXT("chunk_rows"), B.ChunkRows);
		Out->SetNumberField(TEXT("median_worker_ms_per_token"), B.MedianWorkerMsPerToken);
		Out->SetNumberField(TEXT("generation_jobs"), GenerationJobs);
		Out->SetNumberField(TEXT("generation_jobs_between_chunks"), GenerationBetween);
		Out->SetNumberField(TEXT("generation_tokens"), B.GenerationTokens.Num());
		Out->SetStringField(TEXT("priced_with"), PrefixPrefillSourceName(B.Costs));
		Out->SetNumberField(TEXT("priced_token_cost_ms"), B.Costs.PrefixPrefillTokenCostMs);
		Out->SetNumberField(TEXT("priced_token_cost_per_position_ms"), B.Costs.PrefixPrefillTokenCostPerPositionMs);
		Out->SetBoolField(TEXT("decode_mode_effective"), B.Costs.bDecodeModeEffective);
		Out->SetNumberField(TEXT("parallel_tasks"), B.Costs.ParallelTasks);
		Out->SetArrayField(TEXT("chunk_jobs"), Jobs);
		Report->SetObjectField(B.bWithGeneration ? TEXT("insitu_with_generation") : TEXT("insitu_no_generation"), Out);
		const bool bBuilt = B.Source == ESuperSLMPrefixSource::Recomputed && B.ChunkRows >= 8;
		const FString Detail = FString::Printf(TEXT("source %s; %d chunk rows, median worker %.3f ms/token; %d generation jobs (%d between chunks), %d tokens; priced with %s; decode mode %d, tasks %d"),
			SuperSLMPrefixCacheCheck::SourceName(B.Source), B.ChunkRows, B.MedianWorkerMsPerToken, GenerationJobs, GenerationBetween, B.GenerationTokens.Num(),
			PrefixPrefillSourceName(B.Costs), B.Costs.bDecodeModeEffective ? 1 : 0, B.Costs.ParallelTasks);
		if (B.bWithGeneration)
		{
			Add(TEXT("calibrate.insitu_build_with_generation"), bBuilt && B.bGenerationDone && B.GenerationTokens.Num() > 0 && GenerationBetween > 0, Detail);
		}
		else
		{
			Add(TEXT("calibrate.insitu_build_no_generation"), bBuilt && GenerationJobs == 0, Detail);
		}
	}

	void FinishInSitu()
	{
		using namespace SuperSLMPrefixCacheCheck;
		// The configuration build 1 ran under is still the subsystem's: the write matches its profile.
		const FSuperSLMResolvedCpuConfig Isolated = InSitu[1].Costs;
		FSuperSLMInSituPrefixPrefillPrice Price;
		FString Error;
		const bool bWrote = Cpu.WriteInSituPrefixPrefillPrice(InSitu[1].Rows, Price, Error);
		// The next Configure(), at the calibrate configuration, reads it back.
		FSuperSLMRuntimeConfig Config = BaseConfig(Entries);
		Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
		if (!Configure(Config, TEXT("calibrated_insitu")))
		{
			return;
		}
		const bool bReadBack = bWrote && Resolved.PrefixPrefillPriceSource == ESuperSLMPrefixPrefillPriceSource::SavedEstimate
			&& Resolved.PrefixPrefillTokenCostMs == FCString::Atod(*Price.TokenCostText)
			&& Resolved.PrefixPrefillTokenCostPerPositionMs == FCString::Atod(*Price.TokenCostPerPositionText);
		Add(TEXT("calibrate.insitu_wrote_prefix_prefill"), bReadBack,
			bWrote ? FString::Printf(TEXT("%s in %s: PrefixPrefillTokenCostMs = %s, PrefixPrefillTokenCostPerPositionMs = %s from %d chunk rows (was %.3f + %.6f, the isolated arm's); Configure() reads %s %.3f + %.6f"),
				*Price.Profile, *Price.Path, *Price.TokenCostText, *Price.TokenCostPerPositionText, Price.ChunkRows,
				Isolated.PrefixPrefillTokenCostMs, Isolated.PrefixPrefillTokenCostPerPositionMs,
				PrefixPrefillSourceName(Resolved), Resolved.PrefixPrefillTokenCostMs, Resolved.PrefixPrefillTokenCostPerPositionMs)
				: FString::Printf(TEXT("not written: %s"), *Error));

		// §15.32's attribution, recorded: which condition explains the gap.
		const double NoGen = InSitu[0].MedianWorkerMsPerToken;
		const double WithGen = InSitu[1].MedianWorkerMsPerToken;
		auto Ratio = [](double A, double B) { return B > 0.0 ? A / B : 0.0; };
		TSharedRef<FJsonObject> Attribution = MakeShared<FJsonObject>();
		Attribution->SetStringField(TEXT("isolated_csv"), IsolatedCsv);
		Attribution->SetNumberField(TEXT("isolated_chunks"), IsolatedChunks);
		Attribution->SetNumberField(TEXT("isolated_ms_per_token"), IsolatedMsPerToken);
		Attribution->SetNumberField(TEXT("insitu_no_generation_ms_per_token"), NoGen);
		Attribution->SetNumberField(TEXT("insitu_with_generation_ms_per_token"), WithGen);
		Attribution->SetNumberField(TEXT("insitu_no_generation_over_isolated"), Ratio(NoGen, IsolatedMsPerToken));
		Attribution->SetNumberField(TEXT("insitu_with_generation_over_isolated"), Ratio(WithGen, IsolatedMsPerToken));
		Attribution->SetNumberField(TEXT("isolated_pair_token_cost_ms"), Isolated.PrefixPrefillTokenCostMs);
		Attribution->SetNumberField(TEXT("isolated_pair_token_cost_per_position_ms"), Isolated.PrefixPrefillTokenCostPerPositionMs);
		Attribution->SetNumberField(TEXT("insitu_pair_token_cost_ms"), Price.TokenCostMs);
		Attribution->SetNumberField(TEXT("insitu_pair_token_cost_per_position_ms"), Price.TokenCostPerPositionMs);
		Report->SetObjectField(TEXT("prefix_prefill_attribution"), Attribution);
		Add(TEXT("calibrate.prefix_prefill_attribution"), true,
			FString::Printf(TEXT("recorded: median worker ms/token isolated %.3f (%d chunks, %s), in-situ without generation %.3f (x%.3f), in-situ with generation %.3f (x%.3f)"),
				IsolatedMsPerToken, IsolatedChunks, *IsolatedCsv, NoGen, Ratio(NoGen, IsolatedMsPerToken), WithGen, Ratio(WithGen, IsolatedMsPerToken)));
		End();
	}

	void FinishCalibrate()
	{
		RecordPass(TEXT("calibrate_pass"));
		Add(TEXT("calibrate.restores_cooked"), CountSource(ESuperSLMPrefixSource::Cooked) == SuperSLMPrefixCacheCheck::Entries, SourcesText());
		// Every cache restore is a PrefixBegin job; priced by bytes, each is planned above the fixed per-restore cost.
		// Read from this pass's own rows, all of them retained (review N3).
		TArray<FSuperSLMWorkerJobReport> Ledger;
		FString Why;
		if (!LedgerSince(Pass.FirstLedgerRow, Ledger, Why))
		{
			Fail(TEXT("calibrate.restore_jobs_retained"), Why);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 Restores = 0;
		int32 AboveFixed = 0;
		double MaxWorkerMs = 0.0;
		for (const FSuperSLMWorkerJobReport& Job : Ledger)
		{
			if (Job.Kind != ESuperSLMWorkerJobKind::PrefixBegin)
			{
				continue;
			}
			++Restores;
			AboveFixed += Job.PlannedJobMs > Resolved.RestoreCostMs ? 1 : 0;
			MaxWorkerMs = FMath::Max(MaxWorkerMs, Job.WorkerCallMs);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("job"), static_cast<double>(Job.JobId));
			Row->SetNumberField(TEXT("planned_ms"), Job.PlannedJobMs);
			Row->SetNumberField(TEXT("k"), Job.K);
			Row->SetNumberField(TEXT("worker_call_ms"), Job.WorkerCallMs);
			Row->SetBoolField(TEXT("worker_overran"), Job.bWorkerOverran);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("restore_jobs"), Rows);
		Report->SetNumberField(TEXT("restore_cost_ms"), Resolved.RestoreCostMs);
		Report->SetNumberField(TEXT("restore_cost_per_mib_ms"), Resolved.RestoreCostPerMiBMs);
		Add(TEXT("calibrate.restores_priced_by_bytes"), Restores == SuperSLMPrefixCacheCheck::Entries && AboveFixed == Restores,
			FString::Printf(TEXT("%d restore jobs, %d planned above RestoreCostMs %.4f (per MiB %.6f); worker time up to %.4f ms"),
				Restores, AboveFixed, Resolved.RestoreCostMs, Resolved.RestoreCostPerMiBMs, MaxWorkerMs));
		StartInSitu(0); // rev 1.18.12 (§15.32): the in-situ prefix_prefill arm, where the game ticks
	}

	// 8.9's background traffic: one lease at a time on the set's entries, returned as soon as it resolves.
	void PumpTraffic()
	{
		if (Traffic.IsValid())
		{
			if (Cpu.GetPrefixLeaseState(Traffic) == ESuperSLMPrefixLeaseState::Pending)
			{
				return;
			}
			Cpu.ReturnPrefix(Traffic);
			Traffic = {};
		}
		FString Error;
		if (!Cpu.RequestPrefix(Specs[TrafficNext % Specs.Num()], Traffic, Error))
		{
			Traffic = {};
			UE_LOG(LogSuperSLMPrefixCacheCheck, Warning, TEXT("%s: background request refused: %s"), SuperSLMPrefixCacheCheck::kLine, *Error);
		}
		++TrafficNext;
	}

	int64 Hits() const
	{
		const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
		return SuperSLMPrefixCacheCheck::Served(S, ESuperSLMPrefixSource::Memory) + SuperSLMPrefixCacheCheck::Served(S, ESuperSLMPrefixSource::Cooked);
	}

	bool StartQuery()
	{
		Query = MakeUnique<FSuperSLMQueryRunner>(Cpu, nullptr, *Model.Get());
		FSuperSLMQueryConfig Config;
		Config.Backend = ESuperSLMBackendBP::CPU;
		Config.FrameBudgetLayers = 0;
		Config.ConcurrentQueries = 1;
		Config.bSchemaConstrainedDecoding = false;
		Config.StopTokenIds = SuperSLMExample::StopTokenIds();
		FString Error;
		if (!Query->BeginQuery(SuperSLMExample::DefaultUtterance(), SuperSLMPrefixCacheCheck::QueryMaxNewTokens, Config, Error))
		{
			Fail(TEXT("8.9.query_started"), Error);
			return false;
		}
		return true;
	}

	static FString VerdictText(const FSuperSLMSelfCheckBackendResult& R)
	{
		return FString::Printf(TEXT("%s, first diverging step %d -- %s"), *SuperSLMExample::VerdictName(R.Verdict), R.FirstDivergingStep, *R.ScopeText);
	}

	// --- Tick ----------------------------------------------------------------------------------

	void Tick(UWorld* World, float DeltaSeconds)
	{
		using namespace SuperSLMPrefixCacheCheck;
		if (Step == EStep::Done)
		{
			return;
		}
		if (Step == EStep::Start)
		{
			StartLeg(World);
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - LegStarted > LegSeconds || Now - PhaseStarted > PhaseSeconds)
		{
			Fail(TEXT("run.timeout"), FString::Printf(TEXT("step %d passed its time limit (%.0f s in the phase, %.0f s in the leg)"), static_cast<int32>(Step), Now - PhaseStarted, Now - LegStarted));
			return;
		}
		// The self-check ticks the subsystem itself; everything else ticks it here, once per frame (a query
		// runner's own TickOncePerFrame() then finds it ticked).
		if (Step != EStep::SelfCheckOff && Step != EStep::SelfCheckOn && Step != EStep::Travel)
		{
			Cpu.TickOncePerFrame(DeltaSeconds);
		}

		switch (Step)
		{
		case EStep::Reference:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("reference"));
			Add(TEXT("pre.reference_recomputed"), CountSource(ESuperSLMPrefixSource::Recomputed) == Entries, SourcesText());
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				const int64 Builds = S.PrefixPrefillBuilds - Pass.Before.PrefixPrefillBuilds;
				const int64 Calls = S.PrefixPrefillCalls - Pass.Before.PrefixPrefillCalls;
				// The counter's own control: a recompute is counted, one build per prefix.
				Add(TEXT("pre.prefill_counter_counts_recomputes"), Builds == Entries && Calls >= Entries,
					FString::Printf(TEXT("%lld builds, %lld prefill calls for %d recomputed prefixes"), Builds, Calls, Entries));
				EntryBytes = S.ResidentEntries > 0 ? S.ResidentBytes / S.ResidentEntries : 0;
				ReferenceResidentBytes = S.ResidentBytes;
				Report->SetNumberField(TEXT("entry_bytes"), static_cast<double>(EntryBytes));
				Report->SetNumberField(TEXT("reference_resident_bytes"), static_cast<double>(ReferenceResidentBytes));
				if (S.ResidentEntries != Entries || EntryBytes <= 0)
				{
					Fail(TEXT("pre.all_resident"), FString::Printf(TEXT("%d entries resident after the reference pass, %lld bytes"), S.ResidentEntries, S.ResidentBytes));
					return;
				}
			}
			KeepReference();
			if (Leg == ELeg::Cooked)
			{
				// A new configuration empties the memory tier; the set is then registered and the level loads.
				FSuperSLMRuntimeConfig Config = BaseConfig(Entries);
				Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
				if (!Configure(Config, TEXT("cooked")))
				{
					return;
				}
				Cpu.RegisterPrefixSet(Set.Get());
				BeginPass(Ascending(), Entries, EStep::Cooked);
			}
			else if (Leg == ELeg::Residency)
			{
				// A new configuration empties the memory tier; the set is registered and, at a level load, the game
				// warms its next level's 10 prefixes.
				FSuperSLMRuntimeConfig Config = BaseConfig(Entries);
				Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
				if (!Configure(Config, TEXT("residency")))
				{
					return;
				}
				Cpu.RegisterPrefixSet(Set.Get());
				if (IssueOps(/*bWarm*/ true))
				{
					EnterPhase(EStep::Warm);
				}
			}
			else
			{
				// The budget set to 5 entries: five fit, a sixth does not.
				FSuperSLMRuntimeConfig Config = BaseConfig(Entries);
				Config.PrefixCacheBudgetBytes = 5 * EntryBytes + EntryBytes / 2;
				Report->SetNumberField(TEXT("budget_bytes"), static_cast<double>(Config.PrefixCacheBudgetBytes));
				if (!Configure(Config, TEXT("budget_5")))
				{
					return;
				}
				Cpu.RegisterPrefixSet(Set.Get());
				BeginPass(Ascending(), 1, EStep::BudgetEntry);
			}
			return;

		case EStep::Cooked:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("cooked"));
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				Add(TEXT("10.5.set_registered"), S.CookedEntriesRegistered == Entries, FString::Printf(TEXT("%d cooked entries registered"), S.CookedEntriesRegistered));
				Add(TEXT("10.5.every_request_cooked"), CountSource(ESuperSLMPrefixSource::Cooked) == Entries
					&& Served(S, ESuperSLMPrefixSource::Cooked) - Served(Pass.Before, ESuperSLMPrefixSource::Cooked) == Entries, SourcesText());
			}
			CheckNoPrefill(TEXT("10.5.no_prefix_prefill"));
			CheckTokens(TEXT("10.5.tokens_equal_recompute"), TEXT("the recompute pass's"));
			CheckTickBound(TEXT("10.5.tick_within_bound"));
			End();
			return;

		case EStep::BudgetEntry:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("budget_entry"));
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				Add(TEXT("10.5.budget.entry_cooked"), CountSource(ESuperSLMPrefixSource::Cooked) == Entries, SourcesText());
				Add(TEXT("10.5.budget.evicted_to_five"), S.Evictions - Pass.Before.Evictions >= Entries - 5 && S.ResidentEntries <= 5,
					FString::Printf(TEXT("%lld evictions; %d entries resident (%lld bytes of a %lld-byte budget)"),
						S.Evictions - Pass.Before.Evictions, S.ResidentEntries, S.ResidentBytes, 5 * EntryBytes + EntryBytes / 2));
			}
			CheckNoPrefill(TEXT("10.5.budget.entry_no_prefix_prefill"));
			CheckTokens(TEXT("10.5.budget.entry_tokens_equal_recompute"), TEXT("the recompute pass's"));
			CheckTickBound(TEXT("10.5.budget.entry_tick_within_bound"));
			// Leave the level and enter it again: the map is opened anew, which ends this scene and spawns another.
			TravelThen(World, TEXT("10.5.budget.level_reentered"), [this]()
			{
				// Re-entered in the other order: the five most recent are resident, the five evicted come back.
				TArray<int32> Backward = Ascending();
				Algo::Reverse(Backward);
				BeginPass(Backward, 1, EStep::BudgetReentry);
			});
			return;

		case EStep::Travel:
			if (World == nullptr || TravelFrom.Get() == World)
			{
				return; // still the old world, or between worlds
			}
			Add(TravelCheck, true, FString::Printf(TEXT("%s, after %.2f s"), *World->GetPackage()->GetName(), Now - PhaseStarted));
			{
				TFunction<void()> Then = MoveTemp(AfterTravel);
				AfterTravel = nullptr;
				Then();
			}
			return;

		case EStep::BudgetReentry:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("budget_reentry"));
			{
				const int32 Cooked = CountSource(ESuperSLMPrefixSource::Cooked);
				const int32 Memory = CountSource(ESuperSLMPrefixSource::Memory);
				// Five resident (Memory) and five evicted (Cooked): a flushed memory tier reads 10 Cooked (review S3).
				Add(TEXT("10.5.budget.evicted_return_cooked"), Memory == 5 && Cooked == Entries - 5,
					FString::Printf(TEXT("%d Cooked, %d Memory: %s"), Cooked, Memory, *SourcesText()));
			}
			CheckNoPrefill(TEXT("10.5.budget.reentry_no_prefix_prefill"));
			CheckTokens(TEXT("10.5.budget.reentry_tokens_unchanged"), TEXT("the recompute pass's"));
			CheckTickBound(TEXT("10.5.budget.reentry_tick_within_bound"));
			End();
			return;

		case EStep::DiskWrite:
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			EnterPhase(EStep::DiskFlush);
			return;

		case EStep::DiskFlush:
		{
			// The write-backs land on the cache's own threads; wait for all ten files, with no temporary left.
			const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
			const int32 Files = DiskFiles(TEXT("*.slup")).Num();
			const int32 Temporaries = DiskFiles(TEXT("*.tmp")).Num();
			if (S.DiskWritesIssued - Pass.Before.DiskWritesIssued < Entries || Files < Entries || Temporaries > 0)
			{
				return;
			}
			const TSharedRef<FJsonObject> O = RecordPass(TEXT("disk_write"));
			O->SetNumberField(TEXT("files"), Files);
			Add(TEXT("10.6.run1_recomputed"), CountSource(ESuperSLMPrefixSource::Recomputed) == Entries, SourcesText());
			{
				// The counter's control in the disk legs' own binary run (review S4): 10 recomputes read as 10 builds.
				const int64 Builds = S.PrefixPrefillBuilds - Pass.Before.PrefixPrefillBuilds;
				const int64 Calls = S.PrefixPrefillCalls - Pass.Before.PrefixPrefillCalls;
				Add(TEXT("10.6.run1_prefill_counter_counts_recomputes"), Builds == Entries && Calls >= Entries,
					FString::Printf(TEXT("%lld builds, %lld prefill calls for %d recomputed prefixes"), Builds, Calls, Entries));
			}
			Add(TEXT("10.6.run1_written_back"), Files == Entries && S.DiskWritesIssued - Pass.Before.DiskWritesIssued == Entries,
				FString::Printf(TEXT("%d files in %s; %lld writes issued"), Files, *DiskDir, S.DiskWritesIssued - Pass.Before.DiskWritesIssued));
			End();
			return;
		}

		case EStep::DiskRead:
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("disk_read"));
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				Add(TEXT("10.6.run2_all_disk"), CountSource(ESuperSLMPrefixSource::Disk) == Entries
					&& Served(S, ESuperSLMPrefixSource::Disk) - Served(Pass.Before, ESuperSLMPrefixSource::Disk) == Entries, SourcesText());
			}
			CheckNoPrefill(TEXT("10.6.run2_no_prefix_prefill"));
			CheckTokens(TEXT("10.6.run2_tokens_equal_run1"), TEXT("run 1's"));
			End();
			return;

		case EStep::SelfCheckOff:
		case EStep::SelfCheckOn:
		{
			const bool bOn = Step == EStep::SelfCheckOn;
			if (bOn)
			{
				PumpTraffic();
			}
			if (!SelfCheck->Step(SuperSLMExample::SelfCheckStepBudgetSeconds))
			{
				return;
			}
			const FSuperSLMSelfCheckReport R = SelfCheck->GetReport();
			SelfCheck.Reset();
			if (!bOn)
			{
				SelfCheckOffReport = R;
				Report->SetStringField(TEXT("self_check_off"), VerdictText(R.Cpu));
				Add(TEXT("8.9.self_check_without_cache"), R.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Verified, VerdictText(R.Cpu));
				if (StartQuery())
				{
					EnterPhase(EStep::QueryOff);
				}
				return;
			}
			const int64 WindowHits = Hits() - WindowHitsBefore;
			Report->SetStringField(TEXT("self_check_on"), VerdictText(R.Cpu));
			Report->SetNumberField(TEXT("self_check_window_hits"), static_cast<double>(WindowHits));
			Add(TEXT("8.9.self_check_unchanged_with_cache"), R.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Verified
				&& R.Cpu.Verdict == SelfCheckOffReport.Cpu.Verdict && R.Cpu.FirstDivergingStep == SelfCheckOffReport.Cpu.FirstDivergingStep,
				FString::Printf(TEXT("with the cache: %s; without: %s"), *VerdictText(R.Cpu), *VerdictText(SelfCheckOffReport.Cpu)));
			Add(TEXT("8.9.cache_hits_during_self_check"), WindowHits > 0, FString::Printf(TEXT("%lld Memory or Cooked hits while it ran"), WindowHits));
			WindowHitsBefore = Hits();
			if (StartQuery())
			{
				EnterPhase(EStep::QueryOn);
			}
			return;
		}

		case EStep::QueryOff:
		case EStep::QueryOn:
		{
			const bool bOn = Step == EStep::QueryOn;
			if (bOn)
			{
				PumpTraffic();
			}
			bool bSucceeded = false;
			FSuperSLMQueryReadout Readout;
			FString Error;
			if (!Query->TickQuery(DeltaSeconds, bSucceeded, Readout, Error))
			{
				return;
			}
			Query.Reset();
			if (!bSucceeded)
			{
				Fail(bOn ? TEXT("8.9.query_with_cache") : TEXT("8.9.query_without_cache"), Error);
				return;
			}
			if (!bOn)
			{
				DigestOff = Readout.TokenDigestHex;
				Report->SetStringField(TEXT("query_digest_off"), DigestOff);
				Add(TEXT("8.9.query_without_cache"), !DigestOff.IsEmpty(), DigestOff);
				// The cache on, serving the set throughout.
				FSuperSLMRuntimeConfig Config = BaseConfig(4);
				Config.PrefixCacheBudgetBytes = LargeBudgetBytes;
				if (!Configure(Config, TEXT("cache_on")))
				{
					return;
				}
				Cpu.RegisterPrefixSet(Set.Get());
				WindowHitsBefore = Hits();
				SelfCheck = MakeUnique<FSuperSLMSelfCheckRun>(*Model.Get(), FString());
				EnterPhase(EStep::SelfCheckOn);
				return;
			}
			const int64 WindowHits = Hits() - WindowHitsBefore;
			Report->SetStringField(TEXT("query_digest_on"), Readout.TokenDigestHex);
			Report->SetNumberField(TEXT("query_window_hits"), static_cast<double>(WindowHits));
			Add(TEXT("8.9.query_unchanged_with_cache"), !DigestOff.IsEmpty() && Readout.TokenDigestHex == DigestOff,
				FString::Printf(TEXT("with the cache %s; without %s"), *Readout.TokenDigestHex, *DigestOff));
			Add(TEXT("8.9.cache_hits_during_query"), WindowHits > 0, FString::Printf(TEXT("%lld Memory or Cooked hits while it ran"), WindowHits));
			if (Traffic.IsValid())
			{
				Cpu.ReturnPrefix(Traffic);
				Traffic = {};
			}
			End();
			return;
		}

		case EStep::Warm:
		{
			TArray<ESuperSLMPrefixOpResult> Results;
			if (!OpsResolved(Results))
			{
				return;
			}
			const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
			const int32 Resident = Results.FilterByPredicate([](ESuperSLMPrefixOpResult R) { return R == ESuperSLMPrefixOpResult::Resident; }).Num();
			Report->SetObjectField(TEXT("warm_stats_after"), JsonStats(S));
			Add(TEXT("10.7.warms_resident"), Resident == Entries && S.Warms - PhaseBefore.Warms == Entries && S.ResidentEntries == Entries,
				FString::Printf(TEXT("%d of %d warms Resident (results %s); %lld warms counted; %d entries resident"),
					Resident, Entries, *OpResultsText(Results), S.Warms - PhaseBefore.Warms, S.ResidentEntries));
			Add(TEXT("10.7.warms_from_cooked_set"), S.RestoresIssued - PhaseBefore.RestoresIssued == Entries,
				FString::Printf(TEXT("%lld restores issued by the warms"), S.RestoresIssued - PhaseBefore.RestoresIssued));
			Add(TEXT("10.7.warms_no_prefix_prefill"), S.PrefixPrefillCalls == PhaseBefore.PrefixPrefillCalls && S.PrefixPrefillBuilds == PhaseBefore.PrefixPrefillBuilds,
				FString::Printf(TEXT("%lld prefix prefill calls, %lld builds across the warms"),
					S.PrefixPrefillCalls - PhaseBefore.PrefixPrefillCalls, S.PrefixPrefillBuilds - PhaseBefore.PrefixPrefillBuilds));
			// The next level loads; its start requests the warmed prefixes.
			TravelThen(World, TEXT("10.7.level_loaded"), [this]() { BeginPass(Ascending(), SuperSLMPrefixCacheCheck::Entries, EStep::ResidencyStart); });
			return;
		}

		case EStep::ResidencyStart:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("residency_start"));
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				Add(TEXT("10.7.start_memory_hits"), CountSource(ESuperSLMPrefixSource::Memory) == Entries
					&& Served(S, ESuperSLMPrefixSource::Memory) - Served(Pass.Before, ESuperSLMPrefixSource::Memory) == Entries, SourcesText());
			}
			CheckNoPrefill(TEXT("10.7.start_no_prefix_prefill"));
			CheckTokens(TEXT("10.7.start_tokens_equal_recompute"), TEXT("the recompute pass's"));
			CheckTickBound(TEXT("10.7.start_tick_within_bound"));
			// The level's exit: the game drops its prefixes from memory.
			if (IssueOps(/*bWarm*/ false))
			{
				EnterPhase(EStep::Drop);
			}
			return;

		case EStep::Drop:
		{
			TArray<ESuperSLMPrefixOpResult> Results;
			if (!OpsResolved(Results))
			{
				return;
			}
			const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
			if (S.ResidentEntries > 0 && ++SettleFrames < 120)
			{
				return; // a drop evicts at its apply pass; allow the passes a couple of seconds
			}
			const int32 Dropped = Results.FilterByPredicate([](ESuperSLMPrefixOpResult R) { return R == ESuperSLMPrefixOpResult::Dropped; }).Num();
			const int64 Fell = PhaseBefore.ResidentBytes - S.ResidentBytes;
			Report->SetObjectField(TEXT("drop_stats_after"), JsonStats(S));
			Add(TEXT("10.7.drops_dropped"), Dropped == Entries && S.Drops - PhaseBefore.Drops == Entries,
				FString::Printf(TEXT("%d of %d drops Dropped (results %s); %lld drops counted"), Dropped, Entries, *OpResultsText(Results), S.Drops - PhaseBefore.Drops));
			// The mixed set's entries differ in length, so the fall is checked against what the 10 held when the
			// reference pass left them all resident, and every one must be gone.
			Add(TEXT("10.7.resident_bytes_fall_by_dropped"), Fell == ReferenceResidentBytes && S.ResidentEntries == 0,
				FString::Printf(TEXT("resident bytes %lld -> %lld (fell %lld; the 10 entries hold %lld); %d entries resident"),
					PhaseBefore.ResidentBytes, S.ResidentBytes, Fell, ReferenceResidentBytes, S.ResidentEntries));
			// The next level: the dropped specs themselves are requested again.
			TravelThen(World, TEXT("10.7.next_level_loaded"), [this]() { BeginPass(Ascending(), SuperSLMPrefixCacheCheck::Entries, EStep::ResidencyNext); });
			return;
		}

		case EStep::ResidencyNext:
			ReadTicks();
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			RecordPass(TEXT("residency_next"));
			{
				const FSuperSLMPrefixCacheStats S = Cpu.GetPrefixCacheStats();
				// A drop that did nothing would read Memory here, with no new restores.
				Add(TEXT("10.7.dropped_specs_served_cooked"), CountSource(ESuperSLMPrefixSource::Cooked) == Entries
					&& S.RestoresIssued - Pass.Before.RestoresIssued == Entries,
					FString::Printf(TEXT("%s; %lld restores issued"), *SourcesText(), S.RestoresIssued - Pass.Before.RestoresIssued));
			}
			CheckNoPrefill(TEXT("10.7.next_no_prefix_prefill"));
			CheckTokens(TEXT("10.7.next_tokens_equal_recompute"), TEXT("the recompute pass's"));
			CheckTextIds();
			End();
			return;

		case EStep::LongBuild:
			AdvanceLongBuild(CurrentBuild());
			return;

		case EStep::InSituBuild:
			AdvanceInSitu();
			return;

		case EStep::CalibratePass:
			if (!AdvancePass() || !Pass.IsDone())
			{
				return;
			}
			FinishCalibrate();
			return;

		default:
			return;
		}
	}
};

FSuperSLMPrefixCacheCheck::FSuperSLMPrefixCacheCheck() = default;
FSuperSLMPrefixCacheCheck::~FSuperSLMPrefixCacheCheck() = default;

void FSuperSLMPrefixCacheCheck::Start(USuperSLMModel& Model, USuperSLMSubsystem& Cpu, const FString& Leg, const FString& ModelPath)
{
	using namespace SuperSLMPrefixCacheCheck;
	if (GActive.IsValid())
	{
		return;
	}
	GActive.Reset(new FSuperSLMPrefixCacheCheck());
	GActive->State = MakeUnique<FState>(Model, Cpu, Leg, ModelPath);
	if (!GPreExit.IsValid())
	{
		// The check holds the model and the set; let them go before the engine tears down.
		GPreExit = FCoreDelegates::OnEnginePreExit.AddLambda([]() { GActive.Reset(); });
	}
}

void FSuperSLMPrefixCacheCheck::Tick(UWorld* World, float DeltaSeconds)
{
	State->Tick(World, DeltaSeconds);
}

#endif // SUPERSLM_WITH_PREFIX_CACHE
