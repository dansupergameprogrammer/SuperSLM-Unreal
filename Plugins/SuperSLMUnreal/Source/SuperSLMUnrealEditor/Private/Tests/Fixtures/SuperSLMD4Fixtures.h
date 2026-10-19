// T-3102 (plan §10.3.1 item 5b, D-SLM8071/D-SLM8074): shared fixtures for the D4 mode-settings,
// cost-profile and user-estimate cells. Everything here observes the real production paths --
// the console commands, Saved/SuperSLM/*.ini bytes, the finish-hook probe, and the scheduler's
// own job ledger. Nothing here reproduces production arithmetic: the expected planned cost is
// the shared R-S1b oracle (SuperSLML2S1Fixtures::ExpectedPlannedJobMs), and the estimate's
// statistics are recomputed outside the editor by ci/tests/d4_estimate_witness.py.
//
// The report and file contract these helpers read is stated once, in the T-3037 test record
// §10.5, adopted with amendments by plan §10.3.1 item 5b (D-SLM8114, D-SLM8147). Every structured line is LogSuperSLM Display, begins
// "SuperSLM.EstimateCosts: ", and carries space-separated key=value tokens:
//   SuperSLM.EstimateCosts: CALIBRATION ESTIMATE — not verified          (first report line)
//   SuperSLM.EstimateCosts: run id=<id> artifact_hash=<64 hex> FinishParallelTasks=<n> engine=<v> plugin=<v>
//   SuperSLM.EstimateCosts: progress mode=<0|1> phase=warm-up reps=5 calls=<n>   (before that mode's phase=timed)
//   SuperSLM.EstimateCosts: progress mode=<0|1> phase=<text>
//   SuperSLM.EstimateCosts: row mode=<m> arm=<arm> depth=<d> calls=<n> median_ms=<x> iqr_ms=<y>
//   SuperSLM.EstimateCosts: suggested mode=<m> <Field>=<value>          (13 per mode)
//   SuperSLM.EstimateCosts: load phase=<pre|workload|post> [mode=<m>] samples=<n> median_pct=<x> p95_pct=<y> max_pct=<z>
//   SuperSLM.EstimateCosts: coverage uncovered_calls=<n> samples=<n>
//   SuperSLM.EstimateCosts: lock state=<absent|own|foreign|not-configured>
//   SuperSLM.EstimateCosts: files <absolute directory>
//   SuperSLM.EstimateCosts: run status=complete elapsed_s=<x>
//   SuperSLM.EstimateCosts: wrote <path>                                 (only with --write-ini)
// A failed run logs LogSuperSLM Error "SuperSLM.EstimateCosts: incomplete estimate: <reason>".
#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "SuperSLMJson.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMParallelDecodeTestAccess.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace SuperSLMD4Fixtures
{
	// --- Paths ---------------------------------------------------------------------------------

	inline FString CalibratedCostsPath() { return FPaths::ProjectSavedDir() / TEXT("SuperSLM/CalibratedCosts.ini"); }
	inline FString RuntimeSettingsPath() { return FPaths::ProjectSavedDir() / TEXT("SuperSLM/RuntimeSettings.ini"); }
	inline FString ScratchDir() { return FPaths::ProjectSavedDir() / TEXT("SuperSLMD4Scratch"); }

	// The cost-field vocabulary, the one-time profiles and every config read or write come from the
	// shared accessor header (T-3102, D-SLM8146); this header adds the D4 cells' observations.
	using SuperSLMConfigAccess::CostFields;
	using SuperSLMConfigAccess::FCostProfile;
	using SuperSLMConfigAccess::IsPerPositionField;
	using SuperSLMConfigAccess::LoadOneTimeProfiles;

	// --- Files ---------------------------------------------------------------------------------

	// Captures one file's existence and bytes, and restores both exactly (verified) on Restore()
	// or destruction. Seed() replaces the file for the cell, so a "bytes unchanged" assertion is
	// never vacuous over an absent file.
	struct FFileBytesGuard
	{
		FString Path;
		bool bArmed = false;
		bool bExisted = false;
		TArray<uint8> Before;

		explicit FFileBytesGuard(FString InPath) : Path(MoveTemp(InPath)) {}

		bool Capture()
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
			bExisted = IFileManager::Get().FileExists(*Path);
			if (bExisted && !FFileHelper::LoadFileToArray(Before, *Path)) { return false; }
			bArmed = true;
			return true;
		}

		bool Seed(const FString& Text)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
			return FFileHelper::SaveStringToFile(Text, *Path);
		}

		bool Remove()
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
			return !IFileManager::Get().FileExists(*Path) || IFileManager::Get().Delete(*Path);
		}

		bool Restore()
		{
			if (!bArmed) { return true; }
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
			const bool bRestored = bExisted ? FFileHelper::SaveArrayToFile(Before, *Path) : Remove();
			TArray<uint8> After;
			const bool bVerified = bRestored && (bExisted
				? FFileHelper::LoadFileToArray(After, *Path) && After == Before
				: !IFileManager::Get().FileExists(*Path));
			bArmed = !bVerified;
			return bVerified;
		}

		~FFileBytesGuard() { Restore(); }
	};

	inline bool ReadBytes(const FString& Path, bool& bOutExists, TArray<uint8>& OutBytes)
	{
		bOutExists = IFileManager::Get().FileExists(*Path);
		OutBytes.Reset();
		return !bOutExists || FFileHelper::LoadFileToArray(OutBytes, *Path);
	}

	// Files in Dir named Keep plus a suffix (a write's temporary sibling left behind).
	inline TArray<FString> SiblingsOf(const FString& Dir, const FString& Keep)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*")), true, false);
		return Names.FilterByPredicate([&](const FString& N) { return N.StartsWith(Keep) && N != Keep; });
	}

	// The artifact's embedded integrity hash (header bytes 32..63), the value the runtime calls
	// ArtifactHash, read from the file's own bytes -- independent of SuperSLMRuntime::ReadArtifactHash.
	inline bool ReadEmbeddedArtifactHashHex(const FString& Path, FString& OutHex)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader || Reader->TotalSize() < 64) { return false; }
		uint8 Hash[32] = {};
		Reader->Seek(32);
		Reader->Serialize(Hash, 32);
		if (Reader->IsError()) { return false; }
		OutHex = BytesToHex(Hash, 32).ToLower();
		return true;
	}

	// --- INI text, read without assuming section names -------------------------------------------

	struct FIniSection
	{
		FString Header;               // the "[...]" line
		TMap<FString, FString> Keys;  // key=value lines under it
		int32 FirstLine = -1, LastLine = -1;
	};

	inline TArray<FIniSection> ParseIniSections(const FString& Text, TArray<FString>& OutLines)
	{
		Text.ParseIntoArrayLines(OutLines, false);
		TArray<FIniSection> Sections;
		for (int32 I = 0; I < OutLines.Num(); ++I)
		{
			const FString Line = OutLines[I].TrimStartAndEnd();
			if (Line.StartsWith(TEXT("[")))
			{
				FIniSection& S = Sections.AddDefaulted_GetRef();
				S.Header = Line;
				S.FirstLine = S.LastLine = I;
				continue;
			}
			if (Sections.Num() == 0 || Line.IsEmpty() || Line.StartsWith(TEXT(";"))) { continue; }
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Sections.Last().Keys.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
				Sections.Last().LastLine = I;
			}
		}
		return Sections;
	}

	inline TArray<const FIniSection*> SectionsWithKey(const TArray<FIniSection>& Sections, const TCHAR* Key)
	{
		TArray<const FIniSection*> Out;
		for (const FIniSection& S : Sections) { if (S.Keys.Contains(Key)) { Out.Add(&S); } }
		return Out;
	}

	// Decimal places a printed number carries ("1.25" -> 2).
	inline int32 PrintedDecimals(const FString& Text)
	{
		int32 Dot = INDEX_NONE;
		if (!Text.FindChar(TEXT('.'), Dot)) { return 0; }
		int32 N = 0;
		for (int32 I = Dot + 1; I < Text.Len() && FChar::IsDigit(Text[I]); ++I) { ++N; }
		return N;
	}

	inline bool ParseDouble(const FString& Text, double& Out)
	{
		const FString T = Text.TrimStartAndEnd();
		if (T.IsEmpty() || !(FChar::IsDigit(T[0]) || T[0] == TEXT('-') || T[0] == TEXT('+') || T[0] == TEXT('.'))) { return false; }
		Out = FCString::Atod(*T);
		return FMath::IsFinite(Out);
	}

	// Two printed values of one quantity agree when they differ by no more than half a unit in the
	// last place of the coarser print.
	inline bool PrintedValuesAgree(const FString& A, const FString& B)
	{
		double X = 0.0, Y = 0.0;
		if (!ParseDouble(A, X) || !ParseDouble(B, Y)) { return false; }
		const int32 D = FMath::Min(PrintedDecimals(A), PrintedDecimals(B));
		return FMath::Abs(X - Y) <= 0.5 * FMath::Pow(10.0, -static_cast<double>(D)) + 1e-12;
	}

	// --- Logs ----------------------------------------------------------------------------------

	// LogSuperSLM lines this process emits while the capture is alive. The automation framework
	// demotes an Error before an ordinary output device sees it, so Error lines are asserted through
	// AddExpectedErrorPlain, never read here.
	struct FSuperSLMLogCapture final : FOutputDevice
	{
		mutable FCriticalSection Lock;
		TArray<FString> Lines;
		FSuperSLMLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FSuperSLMLogCapture() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName& Category) override
		{
			FScopeLock Guard(&Lock);
			if (Category == FName(TEXT("LogSuperSLM"))) { Lines.Add(Message); }
		}
		TArray<FString> Snapshot() const
		{
			FScopeLock Guard(&Lock);
			return Lines;
		}
		int32 IndexOf(const FString& Needle, int32 From = 0) const
		{
			FScopeLock Guard(&Lock);
			for (int32 I = From; I < Lines.Num(); ++I) { if (Lines[I].Contains(Needle)) { return I; } }
			return INDEX_NONE;
		}
	};

	inline TMap<FString, FString> KeyValues(const FString& Text)
	{
		TArray<FString> Tokens;
		Text.ParseIntoArrayWS(Tokens);
		TMap<FString, FString> Out;
		for (const FString& Token : Tokens)
		{
			FString K, V;
			if (Token.Split(TEXT("="), &K, &V)) { Out.Add(K, V); }
		}
		return Out;
	}

	inline bool RunConsole(FAutomationTestBase& T, const FString& Command)
	{
		return T.TestTrue(*FString::Printf(TEXT("console command registered: %s"), *Command.Left(72)),
			IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, nullptr));
	}

	struct FHookProbe
	{
		FHookProbe() { FSuperSLMParallelDecodeTestAccess::ResetHookObservations(); }
		~FHookProbe() { FSuperSLMParallelDecodeTestAccess::StopHookObservations(); }
	};

	// --- The estimate report -------------------------------------------------------------------

	inline const TCHAR* EstimatePrefix() { return TEXT("SuperSLM.EstimateCosts: "); }
	inline const TCHAR* EstimateHeader() { return TEXT("SuperSLM.EstimateCosts: CALIBRATION ESTIMATE — not verified"); }

	struct FEstimateRow
	{
		int32 Mode = -1;
		FString Arm;
		int64 Depth = 0;
		int32 Calls = -1;
		FString MedianText, IqrText;
	};

	struct FEstimateLoad
	{
		FString Phase;
		int32 Mode = -1;
		int32 Samples = -1;
		double Median = -1.0, P95 = -1.0, Max = -1.0;
	};

	struct FEstimateReport
	{
		int32 HeaderIndex = INDEX_NONE;
		int32 FirstReportLine = INDEX_NONE;
		TMap<FString, FString> Run;
		TArray<FEstimateRow> Rows;
		TMap<FString, FString> Suggested[2];
		TArray<FEstimateLoad> Loads;
		int32 PostLoadIndex = INDEX_NONE;
		TMap<FString, FString> Coverage;
		FString LockState;
		FString FilesDir;
		TSet<int32> ProgressModes;
		// Contract item 3 (D-SLM8147): each mode's "progress mode=<m> phase=warm-up reps=<r> calls=<n>"
		// line, and the index of its first "phase=timed" line.
		int32 WarmUpReps[2] = { -1, -1 };
		int32 WarmUpCalls[2] = { -1, -1 };
		int32 WarmUpIndex[2] = { INDEX_NONE, INDEX_NONE };
		int32 TimedIndex[2] = { INDEX_NONE, INDEX_NONE };
		FString Status;
		double ElapsedSeconds = -1.0;
		int32 CompleteIndex = INDEX_NONE;
		int32 WroteIndex = INDEX_NONE;
		FString WrotePath;
		bool bIqrDisclosure = false;
		bool bLoadDisclosure = false;
		TArray<FString> Forbidden;
	};

	inline FEstimateReport ParseEstimateReport(const TArray<FString>& Lines)
	{
		static const TCHAR* ForbiddenText[] = {
			TEXT("verified-idle"), TEXT("MEETS the criterion"), TEXT("idleness criterion"),
			TEXT("REFUSED"), TEXT("ceiling") };
		FEstimateReport R;
		const FString Prefix = EstimatePrefix();
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const FString& Line = Lines[I];
			for (const TCHAR* F : ForbiddenText) { if (Line.Contains(F)) { R.Forbidden.Add(Line); } }
			const int32 At = Line.Find(Prefix);
			if (At == INDEX_NONE) { continue; }
			if (R.FirstReportLine == INDEX_NONE) { R.FirstReportLine = I; }
			const FString Body = Line.Mid(At + Prefix.Len());
			if (Line.Contains(EstimateHeader()) && R.HeaderIndex == INDEX_NONE) { R.HeaderIndex = I; }
			if (Body.Contains(TEXT("not a confidence interval"))) { R.bIqrDisclosure = true; }
			if (Body.Contains(TEXT("250 ms interval averages"))) { R.bLoadDisclosure = true; }
			if (Body.StartsWith(TEXT("files ")))
			{
				R.FilesDir = Body.Mid(6).TrimStartAndEnd().TrimQuotes();
				continue;
			}
			if (Body.StartsWith(TEXT("wrote ")))
			{
				R.WroteIndex = I;
				R.WrotePath = Body.Mid(6).TrimStartAndEnd().TrimQuotes();
				continue;
			}
			const TMap<FString, FString> KV = KeyValues(Body);
			auto Int = [&KV](const TCHAR* K, int32 Default) { const FString* V = KV.Find(K); return V ? FCString::Atoi(**V) : Default; };
			auto Dbl = [&KV](const TCHAR* K) { double D = -1.0; const FString* V = KV.Find(K); if (V) { ParseDouble(*V, D); } return D; };
			if (Body.StartsWith(TEXT("run id=")))
			{
				R.Run = KV;
			}
			else if (Body.StartsWith(TEXT("run status=")))
			{
				R.Status = KV.FindRef(TEXT("status"));
				R.ElapsedSeconds = Dbl(TEXT("elapsed_s"));
				if (R.Status == TEXT("complete")) { R.CompleteIndex = I; }
			}
			else if (Body.StartsWith(TEXT("progress ")))
			{
				const int32 Mode = Int(TEXT("mode"), -1);
				R.ProgressModes.Add(Mode);
				const FString Phase = KV.FindRef(TEXT("phase"));
				if ((Mode == 0 || Mode == 1) && Phase == TEXT("warm-up") && R.WarmUpIndex[Mode] == INDEX_NONE)
				{
					R.WarmUpIndex[Mode] = I;
					R.WarmUpReps[Mode] = Int(TEXT("reps"), -1);
					R.WarmUpCalls[Mode] = Int(TEXT("calls"), -1);
				}
				if ((Mode == 0 || Mode == 1) && Phase == TEXT("timed") && R.TimedIndex[Mode] == INDEX_NONE)
				{
					R.TimedIndex[Mode] = I;
				}
			}
			else if (Body.StartsWith(TEXT("row ")))
			{
				FEstimateRow Row;
				Row.Mode = Int(TEXT("mode"), -1);
				Row.Arm = KV.FindRef(TEXT("arm"));
				Row.Depth = FCString::Atoi64(*KV.FindRef(TEXT("depth")));
				Row.Calls = Int(TEXT("calls"), -1);
				Row.MedianText = KV.FindRef(TEXT("median_ms"));
				Row.IqrText = KV.FindRef(TEXT("iqr_ms"));
				R.Rows.Add(Row);
			}
			else if (Body.StartsWith(TEXT("suggested ")))
			{
				const int32 Mode = Int(TEXT("mode"), -1);
				if (Mode == 0 || Mode == 1)
				{
					for (const TPair<FString, FString>& P : KV)
					{
						if (P.Key != TEXT("mode")) { R.Suggested[Mode].Add(P.Key, P.Value); }
					}
				}
			}
			else if (Body.StartsWith(TEXT("load ")))
			{
				FEstimateLoad L;
				L.Phase = KV.FindRef(TEXT("phase"));
				L.Mode = Int(TEXT("mode"), -1);
				L.Samples = Int(TEXT("samples"), -1);
				L.Median = Dbl(TEXT("median_pct"));
				L.P95 = Dbl(TEXT("p95_pct"));
				L.Max = Dbl(TEXT("max_pct"));
				if (L.Phase == TEXT("post")) { R.PostLoadIndex = I; }
				R.Loads.Add(L);
			}
			else if (Body.StartsWith(TEXT("coverage ")))
			{
				R.Coverage = KV;
			}
			else if (Body.StartsWith(TEXT("lock ")))
			{
				R.LockState = KV.FindRef(TEXT("state"));
			}
		}
		return R;
	}

	// The 22 arm/depth groups one mode's T-3050d block times on A-EX at cap 4096, keyed as the
	// per-call CSV keys them (arm, depth).
	struct FArmDepth { const TCHAR* Arm; int64 Depth; };
	inline TArray<FArmDepth> ExpectedArmDepths()
	{
		TArray<FArmDepth> Out = {
			{ TEXT("finish"), 1 }, { TEXT("reset"), -1 }, { TEXT("save"), 0 }, { TEXT("restore"), 0 },
			{ TEXT("adopt"), 1 }, { TEXT("prefix_begin"), -1 }, { TEXT("prefix_release"), -1 } };
		for (const int64 D : { int64(1), int64(1024), int64(2048), int64(3072), int64(4095) })
		{
			Out.Add({ TEXT("prompt_1"), D });
			Out.Add({ TEXT("layer"), D });
		}
		for (const int64 D : { int64(1), int64(1024), int64(2048), int64(3072), int64(4088) }) { Out.Add({ TEXT("prompt_8"), D }); }
		return Out;
	}

	// Twelve timed repetitions per arm/depth; a layer arm times every pure layer of each.
	inline int32 ExpectedCalls(const TCHAR* Arm)
	{
		constexpr int32 Reps = 12;
		return FCString::Strcmp(Arm, TEXT("layer")) == 0 ? Reps * (SuperSLML2S2Fixtures::AExNumHiddenLayers - 2) : Reps;
	}

	inline const TCHAR* CallsCsvHeader()
	{
		return TEXT("call_id,block,mode,arm,depth,rep,layer,start_s,end_s,elapsed_ms,foreign_pct,foreign_peak_pct,foreign_coverage,first_sample,last_sample");
	}

	inline const FEstimateLoad* FindLoad(const FEstimateReport& R, const TCHAR* Phase, int32 Mode)
	{
		return R.Loads.FindByPredicate([&](const FEstimateLoad& L) { return L.Phase == Phase && L.Mode == Mode; });
	}

	// Every structural property of a complete two-mode estimate report the editor can check without
	// recomputing statistics (the Python witness recomputes them). Returns true only if all hold.
	inline bool CheckCompleteEstimate(FAutomationTestBase& T, const FEstimateReport& R, const FString& ExpectedArtifactHashHex, int32 ExpectedTasks = 4)
	{
		bool bOk = T.TestTrue(TEXT("report heads with 'CALIBRATION ESTIMATE — not verified'"), R.HeaderIndex != INDEX_NONE);
		bOk &= T.TestEqual(TEXT("the estimate label is the first report line"), R.HeaderIndex, R.FirstReportLine);
		bOk &= T.TestEqual(TEXT("run reports the artifact's embedded hash"), R.Run.FindRef(TEXT("artifact_hash")), ExpectedArtifactHashHex);
		bOk &= T.TestEqual(TEXT("run reports FinishParallelTasks"), R.Run.FindRef(TEXT("FinishParallelTasks")), FString::FromInt(ExpectedTasks));
		bOk &= T.TestFalse(TEXT("run reports a run id"), R.Run.FindRef(TEXT("id")).IsEmpty());
		bOk &= T.TestFalse(TEXT("run reports the engine version"), R.Run.FindRef(TEXT("engine")).IsEmpty());
		bOk &= T.TestFalse(TEXT("run reports the plugin version"), R.Run.FindRef(TEXT("plugin")).IsEmpty());
		bOk &= T.TestTrue(TEXT("progress is printed for mode 0 and mode 1"), R.ProgressModes.Contains(0) && R.ProgressModes.Contains(1));
		bOk &= T.TestEqual(TEXT("run status is complete"), R.Status, FString(TEXT("complete")));
		bOk &= T.TestTrue(TEXT("elapsed time is reported"), R.ElapsedSeconds > 0.0);
		bOk &= T.TestTrue(TEXT("IQR is labelled within-run spread, not a confidence interval"), R.bIqrDisclosure);
		bOk &= T.TestTrue(TEXT("foreign shares are labelled 250 ms interval averages"), R.bLoadDisclosure);
		bOk &= T.TestEqual(TEXT("no verified/idleness/refusal wording in the estimate path"), R.Forbidden.Num(), 0);
		for (const FString& Line : R.Forbidden) { T.AddInfo(FString::Printf(TEXT("forbidden wording: %s"), *Line)); }

		const TArray<FArmDepth> Groups = ExpectedArmDepths();
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			int32 ModeRows = 0;
			for (const FEstimateRow& Row : R.Rows) { ModeRows += Row.Mode == Mode ? 1 : 0; }
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d reports exactly %d arm/depth rows"), Mode, Groups.Num()), ModeRows, Groups.Num());
			for (const FArmDepth& G : Groups)
			{
				const FEstimateRow* Row = R.Rows.FindByPredicate([&](const FEstimateRow& X) { return X.Mode == Mode && X.Arm == G.Arm && X.Depth == G.Depth; });
				double Median = -1.0, Iqr = -1.0;
				const bool bRow = Row != nullptr && ParseDouble(Row->MedianText, Median) && ParseDouble(Row->IqrText, Iqr);
				bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d %s@%lld row has its call count, a positive median and a non-negative IQR"), Mode, G.Arm, G.Depth),
					bRow && Row->Calls == ExpectedCalls(G.Arm) && Median > 0.0 && Iqr >= 0.0);
			}
			for (const FString& F : CostFields())
			{
				double V = -1.0;
				const FString* Text = R.Suggested[Mode].Find(F);
				const bool bValid = Text && ParseDouble(*Text, V) && (IsPerPositionField(F) ? V >= 0.0 : V > 0.0);
				bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d suggests %s as a finite cost"), Mode, *F), bValid);
				// Contract item 5 (D-SLM8113/D-SLM8147): exactly 3 decimals in ms, exactly 6 per position.
				const int32 Want = IsPerPositionField(F) ? 6 : 3;
				bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d suggested %s prints exactly %d decimals"), Mode, *F, Want),
					Text ? PrintedDecimals(*Text) : -1, Want);
			}
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d suggests exactly thirteen fields"), Mode), R.Suggested[Mode].Num(), CostFields().Num());
			// Moved from SuperSLM.U1.Calibration.HealthyMustAccept (D-SLM8111): the batched per-token
			// intercept never exceeds the chunk-1 one.
			double Batched = -1.0, Chunk1 = -1.0;
			const bool bPair = ParseDouble(R.Suggested[Mode].FindRef(TEXT("BatchedPromptTokenCostMs")), Batched)
				&& ParseDouble(R.Suggested[Mode].FindRef(TEXT("PromptTokenCostMs")), Chunk1);
			bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d suggested BatchedPromptTokenCostMs does not exceed PromptTokenCostMs"), Mode),
				bPair && Batched <= Chunk1);
			// Contract item 3 (D-SLM8147): the warm-up line, reps=5 and 5 x (timed calls / 12) calls,
			// before the mode's first timed phase line.
			int32 TimedCalls = 0;
			for (const FArmDepth& G : Groups) { TimedCalls += ExpectedCalls(G.Arm); }
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d warm-up line reports reps=5"), Mode), R.WarmUpReps[Mode], 5);
			bOk &= T.TestEqual(*FString::Printf(TEXT("mode %d warm-up line reports calls = 5 x timed calls / 12"), Mode), R.WarmUpCalls[Mode], 5 * TimedCalls / 12);
			bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d warm-up precedes its first timed call"), Mode),
				R.WarmUpIndex[Mode] != INDEX_NONE && R.TimedIndex[Mode] != INDEX_NONE && R.WarmUpIndex[Mode] < R.TimedIndex[Mode] && R.WarmUpCalls[Mode] > 0);
		}

		const FEstimateLoad* Loads[] = { FindLoad(R, TEXT("pre"), -1), FindLoad(R, TEXT("workload"), 0), FindLoad(R, TEXT("workload"), 1), FindLoad(R, TEXT("post"), -1) };
		const TCHAR* LoadNames[] = { TEXT("pre"), TEXT("mode-0 workload"), TEXT("mode-1 workload"), TEXT("post") };
		for (int32 I = 0; I < 4; ++I)
		{
			const FEstimateLoad* L = Loads[I];
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s foreign load reports samples and ordered median <= p95 <= max"), LoadNames[I]),
				L != nullptr && L->Samples > 0 && L->Median >= 0.0 && L->Median <= L->P95 && L->P95 <= L->Max && L->Max <= 100.0);
		}
		bOk &= T.TestEqual(TEXT("sampler coverage: no uncovered timed call"), R.Coverage.FindRef(TEXT("uncovered_calls")), FString(TEXT("0")));
		bOk &= T.TestTrue(TEXT("sampler coverage reports its sample count"), FCString::Atoi(*R.Coverage.FindRef(TEXT("samples"))) > 0);
		bOk &= T.TestTrue(TEXT("lock observation is reported"), R.LockState == TEXT("absent") || R.LockState == TEXT("own")
			|| R.LockState == TEXT("foreign") || R.LockState == TEXT("not-configured"));

		// Retained per-call data: the T-3050d CSV, one file per mode, every timed call.
		const bool bDir = !R.FilesDir.IsEmpty() && IFileManager::Get().DirectoryExists(*R.FilesDir);
		bOk &= T.TestTrue(*FString::Printf(TEXT("diagnostic directory exists: %s"), *R.FilesDir), bDir);
		int32 ExpectedPerMode = 0;
		for (const FArmDepth& G : Groups) { ExpectedPerMode += ExpectedCalls(G.Arm); }
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			TArray<FString> CsvLines;
			const FString Csv = R.FilesDir / FString::Printf(TEXT("mode%d.calls.csv"), Mode);
			const bool bRead = bDir && FFileHelper::LoadFileToStringArray(CsvLines, *Csv);
			bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d per-call CSV has the T-3050d header and all %d timed calls"), Mode, ExpectedPerMode),
				bRead && CsvLines.Num() == ExpectedPerMode + 1 && CsvLines[0] == CallsCsvHeader());
		}
		bOk &= T.TestTrue(TEXT("sample series CSV retained"), bDir && IFileManager::Get().FileExists(*(R.FilesDir / TEXT("samples.csv"))));
		return bOk;
	}

	// D-SLM8142: the retained sample series proves the own-process subtraction and the lock
	// observation. Every sample: foreign_pct = max(0, box_pct - own_pct) within 2e-6 (three 6-decimal
	// prints), and lock_state is one of the report's four tokens. Each mode's workload-phase median
	// own_pct >= 0.5 x 100 / N, N = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS): the floor comes
	// from N, never from a reading. With bRequireOwnLock, every sample reads "own" -- the cell runs
	// under the box driver's held SSU build lock, and fails by name when SUPERSLM_BUILD_LOCK is unset.
	inline bool CheckSampleSeries(FAutomationTestBase& T, const FString& FilesDir, bool bRequireOwnLock)
	{
		if (bRequireOwnLock && FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_BUILD_LOCK")).TrimStartAndEnd().IsEmpty())
		{
			T.AddError(TEXT("SUPERSLM_BUILD_LOCK is unset: the per-sample 'own' lock check needs the box driver's held build lock and its token"));
			return false;
		}
		TArray<FString> Lines;
		if (!T.TestTrue(TEXT("samples.csv readable"), FFileHelper::LoadFileToStringArray(Lines, *(FilesDir / TEXT("samples.csv")))) || Lines.Num() < 2)
		{
			return false;
		}
		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(","), false);
		const int32 ModeCol = Header.IndexOfByKey(TEXT("mode")), PhaseCol = Header.IndexOfByKey(TEXT("phase"));
		const int32 BoxCol = Header.IndexOfByKey(TEXT("box_pct")), OwnCol = Header.IndexOfByKey(TEXT("own_pct"));
		const int32 ForeignCol = Header.IndexOfByKey(TEXT("foreign_pct")), LockCol = Header.IndexOfByKey(TEXT("lock_state"));
		if (!T.TestTrue(TEXT("samples.csv carries mode, phase, box_pct, own_pct, foreign_pct and lock_state"),
			ModeCol >= 0 && PhaseCol >= 0 && BoxCol >= 0 && OwnCol >= 0 && ForeignCol >= 0 && LockCol >= 0))
		{
			return false;
		}
		int32 IdentityFailures = 0, VocabularyFailures = 0, NotOwn = 0;
		double WorstIdentity = 0.0;
		TArray<double> WorkloadOwn[2];
		for (int32 I = 1; I < Lines.Num(); ++I)
		{
			TArray<FString> C;
			Lines[I].ParseIntoArray(C, TEXT(","), false);
			if (C.Num() != Header.Num()) { ++IdentityFailures; continue; }
			const double Box = FCString::Atod(*C[BoxCol]), Own = FCString::Atod(*C[OwnCol]), Foreign = FCString::Atod(*C[ForeignCol]);
			const double Error = FMath::Abs(Foreign - FMath::Max(0.0, Box - Own));
			WorstIdentity = FMath::Max(WorstIdentity, Error);
			IdentityFailures += Error > 2e-6 ? 1 : 0;
			const FString Lock = C[LockCol];
			VocabularyFailures += (Lock == TEXT("absent") || Lock == TEXT("own") || Lock == TEXT("foreign") || Lock == TEXT("not-configured")) ? 0 : 1;
			NotOwn += Lock == TEXT("own") ? 0 : 1;
			const int32 Mode = FCString::Atoi(*C[ModeCol]);
			if (C[PhaseCol] == TEXT("workload") && (Mode == 0 || Mode == 1)) { WorkloadOwn[Mode].Add(Own); }
		}
		const int32 Samples = Lines.Num() - 1;
		T.AddInfo(FString::Printf(TEXT("samples.csv: %d samples, worst identity error %.3g, %d not own"), Samples, WorstIdentity, NotOwn));
		bool bOk = T.TestEqual(TEXT("every sample's foreign_pct = max(0, box_pct - own_pct) within 2e-6"), IdentityFailures, 0);
		bOk &= T.TestEqual(TEXT("every sample's lock_state is absent, own, foreign or not-configured"), VocabularyFailures, 0);
		if (bRequireOwnLock)
		{
			bOk &= T.TestEqual(TEXT("under the held build lock every sample's lock_state is own"), NotOwn, 0);
		}
		const int32 N = FMath::Max(1, (int32)::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		const double Floor = 0.5 * 100.0 / N;
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			TArray<double>& V = WorkloadOwn[Mode];
			V.Sort();
			const double Median = V.Num() == 0 ? -1.0 : (V.Num() % 2 ? V[V.Num() / 2] : 0.5 * (V[V.Num() / 2 - 1] + V[V.Num() / 2]));
			bOk &= T.TestTrue(*FString::Printf(TEXT("mode %d workload median own_pct %.4f%% is at least 0.5 x 100/N = %.4f%% (N = %d)"), Mode, Median, Floor, N),
				V.Num() > 0 && Median >= Floor);
		}
		return bOk;
	}

	// --- A separate process that loads the CPU (foreign to the estimate's whole-process subtraction)

	// pwsh is an installed tool. Its script starts Threads spinning threads, writes a readiness
	// marker, and stays alive until this owner terminates it: the U1 idleness constructions' load,
	// with the thread count as a parameter.
	struct FForeignCpuLoad
	{
		HANDLE Process = nullptr;
		FString ScriptPath, ReadyPath;
		double StartWall = 0.0, StartCpu = 0.0, ActiveWall = 0.0, ActiveCpu = 0.0;
		int32 Threads = 0;

		~FForeignCpuLoad()
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

		bool Start(int32 InThreads, FString& Error)
		{
			Threads = InThreads;
			const FString Stem = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ScriptPath = ScratchDir() / (TEXT("estimate-load-") + Stem + TEXT(".ps1"));
			ReadyPath = ScratchDir() / (TEXT("estimate-load-") + Stem + TEXT(".ready"));
			IFileManager::Get().MakeDirectory(*ScratchDir(), true);
			const FString Script = FString::Printf(TEXT(
				"$source = @'\n"
				"using System;\nusing System.Threading;\n"
				"public static class SuperSlmEstimateLoad {\n"
				"  public static volatile int Sink = 1;\n"
				"  public static void Run(string ready) {\n"
				"    for (int i = 0; i < %d; ++i) {\n"
				"      Thread t = new Thread(() => { while (true) { Sink = unchecked(Sink * 1664525 + 1013904223); } });\n"
				"      t.IsBackground = true; t.Start();\n"
				"    }\n"
				"    System.IO.File.WriteAllText(ready, \"ready\");\n"
				"    Thread.Sleep(Timeout.Infinite);\n"
				"  }\n}\n'@\n"
				"Add-Type -TypeDefinition $source\n"
				"[SuperSlmEstimateLoad]::Run($args[0])\n"), Threads);
			if (!FFileHelper::SaveStringToFile(Script, *ScriptPath)) { Error = TEXT("could not write the load script"); return false; }
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
			if (!CpuSeconds(Process, StartCpu)) { Error = TEXT("GetProcessTimes failed at load start"); return false; }
			StartWall = FPlatformTime::Seconds();
			return true;
		}

		void Stop()
		{
			if (Process == nullptr) { return; }
			double EndCpu = 0.0;
			const double EndWall = FPlatformTime::Seconds();
			if (StartWall > 0.0 && CpuSeconds(Process, EndCpu)) { ActiveWall = EndWall - StartWall; ActiveCpu = EndCpu - StartCpu; }
			::TerminateProcess(Process, 0);
			::WaitForSingleObject(Process, 5000);
			::CloseHandle(Process);
			Process = nullptr;
		}

		// The load really ran: at least half its threads' worth of CPU across its whole life.
		bool Ran() const { return ActiveWall > 10.0 && ActiveCpu >= 0.5 * Threads * ActiveWall; }
	};

	// --- Real configuration and job pricing ----------------------------------------------------

	// A CPU config that sets only the call-shape and budget fields. Its cost fields, decode mode
	// and FinishParallelTasks are left as constructed, so a cell observes the defaults, the
	// runtime settings and the saved estimate the plugin supplies (plan §10.3.1 item 5b).
	inline FSuperSLMRuntimeConfig ShapeOnlyCpuConfig()
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 8;
		Config.MaxLayerBudget = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		Config.BlockCount = 3;
		Config.PrefixBlockCount = 1;
		Config.MaxConcurrentCalls = 2;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		return Config;
	}

	inline bool ImportAEx(FAutomationTestBase& T, USuperSLMModel*& OutModel, FString* OutPath = nullptr)
	{
		FString Path, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), SuperSLML2S1Fixtures::TryGetAExArtifactPath(Path, Reason)))
		{
			return false;
		}
		if (OutPath) { *OutPath = Path; }
		FSuperSLMImportDiagnostic Diagnostic;
		OutModel = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		return T.TestNotNull(TEXT("A-EX imports"), OutModel) && T.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted);
	}

	struct FHookReading
	{
		int32 Makes = 0, ZeroBits = 0, MatvecBits = 0;
		uint32 LastBits = 0;
	};

	inline void ReadHooks(FHookReading& Out, const TCHAR* Label, FAutomationTestBase& T)
	{
		Out.Makes = FSuperSLMParallelDecodeTestAccess::GetMakeCount();
		Out.ZeroBits = FSuperSLMParallelDecodeTestAccess::GetZeroBitsMakeCount();
		Out.MatvecBits = FSuperSLMParallelDecodeTestAccess::GetMatvecBitsMakeCount();
		Out.LastBits = FSuperSLMParallelDecodeTestAccess::GetLastMadeBits();
		T.AddInfo(FString::Printf(TEXT("%s: makes=%d zero=%d matvec=%d bits=0x%x"), Label, Out.Makes, Out.ZeroBits, Out.MatvecBits, Out.LastBits));
	}

	// Configure() under the hook probe. ExpectedResult is Success unless the cell expects a refusal.
	inline bool ConfigureObserved(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model,
		const FSuperSLMRuntimeConfig& Config, FHookReading& Out, const TCHAR* Label,
		FSuperSLMConfigureReport* OutReport = nullptr, ESuperSLMConfigureResult ExpectedResult = ESuperSLMConfigureResult::Success)
	{
		FHookProbe Probe;
		const FSuperSLMConfigureReport Report = Cpu.Configure(&Model, Config);
		ReadHooks(Out, Label, T);
		if (OutReport) { *OutReport = Report; }
		return T.TestEqual(*FString::Printf(TEXT("%s: CPU Configure result (%s)"), Label, *Report.Message),
			(uint8)Report.Result, (uint8)ExpectedResult);
	}

	// BeginConfigure() under the hook probe (D-SLM8144): the pool job runs, and OnDone arrives as a
	// game-thread task, which this synchronous cell pumps. BeforeDrain runs between the call and the
	// drain (the late-selection arm issues SetDecodeMode and releases the held pool job there).
	inline bool BeginConfigureObserved(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model,
		const FSuperSLMRuntimeConfig& Config, FHookReading& Out, const TCHAR* Label,
		FSuperSLMConfigureReport* OutReport = nullptr, TFunction<void()> BeforeDrain = nullptr)
	{
		FHookProbe Probe;
		TSharedRef<TOptional<FSuperSLMConfigureReport>> Delivered = MakeShared<TOptional<FSuperSLMConfigureReport>>();
		Cpu.BeginConfigure(&Model, Config, [Delivered](const FSuperSLMConfigureReport& R) { *Delivered = R; });
		if (BeforeDrain) { BeforeDrain(); }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (!Delivered->IsSet() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
		ReadHooks(Out, Label, T);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: BeginConfigure() delivered its report"), Label), Delivered->IsSet())) { return false; }
		const FSuperSLMConfigureReport& Report = Delivered->GetValue();
		if (OutReport) { *OutReport = Report; }
		return T.TestEqual(*FString::Printf(TEXT("%s: CPU BeginConfigure result (%s)"), Label, *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// The Configure() line (plan item 5b "Report", D-SLM8148), matched as a WHOLE line: LogSuperSLM
	// Display "SuperSLM: CPU Configure: bParallelDecode=<effective 0|1> FinishParallelTasks=<n>
	// cost profile=<caller|saved-estimate|mode-default> caller_cost_fields=<0..13>". The packaged-load
	// cell expects the same line ending " caller_cost_fields=0".
	inline FString ConfigureLine(int32 Mode, int32 Tasks, const TCHAR* Profile, int32 CallerFields)
	{
		return FString::Printf(TEXT("SuperSLM: CPU Configure: bParallelDecode=%d FinishParallelTasks=%d cost profile=%s caller_cost_fields=%d"),
			Mode, Tasks, Profile, CallerFields);
	}

	inline bool CheckConfigureLine(FAutomationTestBase& T, const FSuperSLMLogCapture& Log, int32 Mode, int32 Tasks, const TCHAR* Profile,
		int32 CallerFields, const TCHAR* Label)
	{
		const FString Expected = ConfigureLine(Mode, Tasks, Profile, CallerFields);
		const bool bFound = Log.Snapshot().ContainsByPredicate([&Expected](const FString& L) { return L.TrimStartAndEnd() == Expected; });
		return T.TestTrue(*FString::Printf(TEXT("%s: Configure logs exactly '%s'"), Label, *Expected), bFound);
	}

	// The report's resolved configuration (D-SLM8165) equals the logged line and the priced profile.
	inline bool CheckResolved(FAutomationTestBase& T, const FSuperSLMConfigureReport& Report, bool bRequested, bool bEffective, int32 Tasks,
		const TCHAR* Profile, int32 CallerFields, const FCostProfile& Costs, const TCHAR* Label)
	{
		SuperSLMConfigAccess::FResolvedView View;
		if (!SuperSLMConfigAccess::ReadResolved(Report, View))
		{
			T.AddError(FString::Printf(TEXT("%s: %s"), Label, SuperSLMConfigAccess::NoResolvedText()));
			return false;
		}
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: report resolved requested mode"), Label), View.bModeRequested, bRequested);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: report resolved effective mode"), Label), View.bModeEffective, bEffective);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: report resolved task count"), Label), View.ParallelTasks, Tasks);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: report resolved cost profile"), Label), View.CostProfile, FString(Profile));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: report resolved caller-field count"), Label), View.CallerCostFields, CallerFields);
		for (const FString& F : CostFields())
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: report resolved %s"), Label, *F), View.Costs.FindRef(F), Costs.FindRef(F));
		}
		return bOk;
	}

	// Runs one short generation and returns hook runs minus delivered token finishes: the decode
	// projection calls that went through the hook (zero unless SSLM_PARALLEL_FOR_MATVEC is installed).
	inline bool MatvecRunsOfOneGeneration(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int32& OutMatvecRuns)
	{
		FHookProbe Probe;
		const int64 LedgerBefore = Cpu.GetJobLedgerAppendedCount();
		FSuperSLMSequence Seq;
		if (!T.TestEqual(TEXT("VendSequence"), (uint8)Cpu.VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = { 1, 2, 3, 4, 5, 6, 7, 8 };
		Request.MaxNewTokens = 6;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		TArray<int32> Tokens;
		FString Error;
		const bool bDone = SuperSLML2S1Fixtures::RunGenerationToCompletion(Cpu, Seq, Request, Tokens, 120.0, Error);
		const int32 Runs = FSuperSLMParallelDecodeTestAccess::GetRunCount();
		int32 Finishes = 0;
		const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu.GetJobLedger();
		const int64 FirstRow = Cpu.GetJobLedgerAppendedCount() - Ledger.Num();
		for (int32 I = 0; I < Ledger.Num(); ++I)
		{
			const FSuperSLMWorkerJobReport& Job = Ledger[I];
			if (FirstRow + I < LedgerBefore || Job.DeliveredAtTick < 0 || Job.bSkippedContinuation) { continue; }
			Finishes += Job.TokenFinishes;
		}
		Cpu.ReturnSequence(Seq);
		OutMatvecRuns = Runs - Finishes;
		return T.TestTrue(*FString::Printf(TEXT("generation completed (%s)"), *Error), bDone) && T.TestTrue(TEXT("generation finished tokens"), Finishes > 0);
	}

	// Drives one job of every kind the scheduler prices -- a nine-token prompt (an 8-token chunk on
	// the batched pair, a 1-token chunk on the chunk-1 pair) with decode, then reset, save, restore,
	// a prefix (begin and its prefill), adopt and prefix release -- and returns the delivered rows.
	inline bool DriveEveryJobKind(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, USuperSLMModel& Model,
		TArray<FSuperSLMWorkerJobReport>& OutRows, const TCHAR* Label)
	{
		using namespace SuperSLML2S1Fixtures;
		auto Drain = [&Cpu](TFunctionRef<bool()> Done)
		{
			DrainTicks(Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, Done, 120.0);
			return Done();
		};
		FSuperSLMSequence A, B;
		if (!T.TestEqual(*FString::Printf(TEXT("%s: VendSequence"), Label), (uint8)Cpu.VendSequence(A), (uint8)ESuperSLMVendResult::Success)) { return false; }
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		Request.MaxNewTokens = 3;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		TArray<int32> Tokens;
		FString Error;
		const bool bGenerated = RunGenerationToCompletion(Cpu, A, Request, Tokens, 120.0, Error);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: generation completed (%s)"), Label, *Error), bGenerated);

		FSuperSLMLifecycleOpHandle Reset, Save, Restore, Adopt;
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: reset queued"), Label), (uint8)Cpu.ResetSequence(A, Reset, Error), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: reset delivered"), Label), Drain([&]() { return Cpu.GetLifecycleOpResult(Reset) == ESuperSLMRestoreResult::Success; }));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: save queued"), Label), (uint8)Cpu.SaveSequence(A, Save, Error), (uint8)ESuperSLMRestoreResult::Success);
		TArray<uint8> Blob;
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		// The first Success read moves the blob out; any later read returns Consumed with no blob,
		// so the handle is read only while the save is still pending.
		Drain([&]()
		{
			if (SaveResult == ESuperSLMRestoreResult::Pending) { SaveResult = Cpu.GetSaveResult(Save, Blob); }
			return SaveResult != ESuperSLMRestoreResult::Pending;
		});
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: save delivered a blob"), Label), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success);
		bool bRestored = false;
		if (SaveResult == ESuperSLMRestoreResult::Success)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: restore queued"), Label),
				(uint8)Cpu.RestoreSequence(MoveTemp(Blob), &Model, B, Restore, Error), (uint8)ESuperSLMRestoreResult::Success);
			bRestored = Drain([&]() { return Cpu.GetLifecycleOpResult(Restore) == ESuperSLMRestoreResult::Success; });
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: restore delivered"), Label), bRestored);
		}
		FSuperSLMPrefix Prefix;
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: prefix created"), Label), Cpu.CreatePrefix({ 1, 2, 3 }, Prefix, Error));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: prefix ready"), Label), Drain([&]() { return Cpu.IsPrefixReady(Prefix); }));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: adopt queued"), Label), (uint8)Cpu.AdoptPrefix(A, Prefix, Adopt, Error), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: adopt delivered"), Label), Drain([&]() { return Cpu.GetLifecycleOpResult(Adopt) == ESuperSLMRestoreResult::Success; }));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: prefix released"), Label), Cpu.ReleasePrefix(Prefix, Error));
		const bool bReleaseDelivered = Drain([&]()
		{
			for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
			{
				if (Job.Kind == ESuperSLMWorkerJobKind::PrefixRelease && Job.DeliveredAtTick >= 0) { return true; }
			}
			return false;
		});
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: prefix release delivered"), Label), bReleaseDelivered);
		for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
		{
			if (Job.DeliveredAtTick >= 0 && !Job.bSkippedContinuation) { OutRows.Add(Job); }
		}
		Cpu.ReturnSequence(A);
		if (bRestored) { Cpu.ReturnSequence(B); }
		return bOk;
	}

	// Every delivered row's PlannedJobMs equals the shared R-S1b oracle priced at Profile, and every
	// job kind -- with both prompt pairs, a decode at depth and a finish -- is among the rows, so all
	// thirteen fields were exercised.
	inline bool CheckRowsPricedBy(FAutomationTestBase& T, const TArray<FSuperSLMWorkerJobReport>& Rows,
		const FCostProfile& Profile, const TCHAR* Label)
	{
		// Priced from the cost map directly: the explicit profile under test, never a config (D-SLM8146).
		bool bOk = true;
		bool bKinds[7] = {};
		bool bBatched = false, bChunk1 = false, bDecode = false, bFinish = false;
		for (const FSuperSLMWorkerJobReport& Job : Rows)
		{
			const int32 Kind = (int32)Job.Kind;
			if (Kind >= 0 && Kind < 7) { bKinds[Kind] = true; }
			bBatched |= Job.PromptTokens >= 8;
			bChunk1 |= Job.PromptTokens > 0 && Job.PromptTokens < 8;
			bDecode |= Job.DecodeLayers > 0 && Job.DecodeLayerDepthSum > 0;
			bFinish |= Job.TokenFinishes > 0;
			const double Expected = SuperSLML2S1Fixtures::ExpectedPlannedJobMs(T, Job, Profile);
			if (!FMath::IsNearlyEqual(Job.PlannedJobMs, Expected, 1e-9 * FMath::Max(1.0, Expected)))
			{
				bOk = false;
				T.AddError(FString::Printf(TEXT("%s: job %lld kind %d (layers %d, prompt %d, finishes %d) planned %.9f ms; the profile prices it %.9f ms"),
					Label, Job.JobId, Kind, Job.DecodeLayers, Job.PromptTokens, Job.TokenFinishes, Job.PlannedJobMs, Expected));
			}
		}
		const TCHAR* KindNames[7] = { TEXT("decode/prefill"), TEXT("reset"), TEXT("adopt"), TEXT("save"), TEXT("restore"), TEXT("prefix begin"), TEXT("prefix release") };
		for (int32 K = 0; K < 7; ++K)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: a %s job was priced"), Label, KindNames[K]), bKinds[K]);
		}
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the batched pair, the chunk-1 pair, a decode at depth and a finish were priced"), Label),
			bBatched && bChunk1 && bDecode && bFinish);
		return bOk;
	}

	// Restores process-wide decode settings to the shipped defaults (no --save, so no bytes) for a
	// cell that changed them through the console. A missing command is reported by the cell that
	// needs it, not here.
	inline void ResetProcessDecodeSettings()
	{
		IConsoleManager::Get().ProcessUserConsoleInput(TEXT("SuperSLM.SetDecodeMode 0"), *GLog, nullptr);
		IConsoleManager::Get().ProcessUserConsoleInput(TEXT("SuperSLM.SetFinishParallelTasks 4"), *GLog, nullptr);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
