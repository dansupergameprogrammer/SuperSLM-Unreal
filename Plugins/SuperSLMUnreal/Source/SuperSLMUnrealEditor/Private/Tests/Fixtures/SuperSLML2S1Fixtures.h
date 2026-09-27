#pragma once

// T-2805 (L2-S1 red suite). Locates the REAL artifacts and reference data R-S1a-R-S1f
// run against (at least one test runs the real artifact, at real size, on real input -- every
// cell in this suite is such a
// cell; D-SLM7033/D-SLM7178/D-SLM7220's filter, restated in the plan's own header, "no cell
// tests a test"). This header is NOT production code: it lives under Private/Tests/, compiles
// only inside WITH_DEV_AUTOMATION_TESTS translation units, matching
// SuperSLMImportFixtures.h's own convention in this same module.
//
// Two kinds of input:
//   - A committed, hash-traced EXTRACT of T-2783's own frozen prompts and Layer-1-recorded
//     outputs (Tests/Fixtures/L2S1/l2s1_r_s1a_reference.json; see that directory's
//     PROVENANCE.md) -- small enough to commit, so R-S1a's oracle does not depend on the
//     maintainer's own scratch directory continuing to exist.
//   - The real, multi-hundred-megabyte-to-multi-gigabyte production .sslm artifacts this plan
//     names (A-CPU, A-EX, A-AD; plan §9; CONTRIBUTING.md's test-input table says which is
//     which) -- never committed to this repository; located through the environment variables
//     SuperSLMTestDataPaths.h reads, never faked and never substituted with a smaller stand-in.
//
// Every artifact this header names -- A-CPU, A-EX, and A-AD -- is verified present at the byte
// count and SHA-256 the filed record states, either at this suite's original authoring
// (2026-09-18) or at a maintainer follow-up ruling the same day. TryGetAExArtifactPath() still
// reports a named reason and never substitutes a different artifact if a differently-laid-out
// box lacks it at the default path.
//
// **D-SLM7339: no artifact this suite uses carries a tokenizer, except the rebuilt A-EX.** T-2815
// found A-CPU, the original A-EX, and A-AD's base all lack the tokenizer sections (types 20-22);
// `Tokenize()` therefore fails (`SSLM_ARTIFACT_REJECTED`) against every one of them except the
// rebuilt A-EX this header now points at. R-S1a keeps feeding recorded token ids directly and
// never calls `Tokenize()` at all (its own claim is bit-perturbation, not text handling,
// D-SLM7339). Any cell against A-CPU or A-AD that needs SOME real prompt reuses a fixture's own
// recorded token ids rather than calling `Tokenize()` -- documented at each such call site.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperSLMTestDataPaths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMJson.h" // the plugin's one JSON-object read (review R4-W1)
#include "SuperSLMSlotGates.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Templates/Function.h"
#include "Tests/AutomationCommon.h" // FTestWorldWrapper (Engine module, already a dependency)

#include <atomic>

// --- Real-time pacing for the async worker tick's drain loops (T-2805 round 10) ---
//
// Every drain loop in this header and in every other L2-S1 test file, before this round, called
// Tick() back-to-back with no relation to wall-clock time -- confirmed at source
// (the build record §14.7): under the async worker tick, Tick() is
// cheap Plan/Apply bookkeeping only (by design, R-S1g), so an unpaced loop can exhaust its own
// tick-count cap in microseconds while the REAL worker-side computation (tens of milliseconds
// per job: 46 ms for one prompt token, more for a batched decode) has not even been scheduled to
// run yet -- "ticks are spinning and nothing is delivered". This is a test-harness defect, not a
// production one (§14.7's own diagnosis): the delivery gate the async rework added is correct,
// and is not weakened or compensated for here.
//
// One shared helper, two pacing modes, chosen per cell (a test exercises the real delivery gate,
// it never assumes it):
//
//   FastAsPossible -- for cells about CONTENT, ORDER or LEDGER COMPOSITION (determinism,
//   save/restore, misuse, concurrency, frame-budget ledgers, queueing order): sleeps a short,
//   fixed slice between Tick() calls ONLY while Done() is still false, so the loop advances as
//   fast as the worker genuinely delivers and never slower than that -- "runs as on a
//   fast-enough machine, with no hitches" -- while still exercising the real delivery gate (a
//   job is never treated as finished before the worker AND Tick()'s own Apply phase say so).
//
//   RealTime -- for cells about TIMING ITSELF (R-S1g's tick-cost bound, R-S1i's loaded run, the
//   hitch counter): sleeps approximately StepSeconds (the config's own TickBudgetMs, converted to
//   seconds) BEFORE every Tick() call, like a real game frame, so a job can genuinely arrive late
//   against its own committed delivery tick and the hitch mechanism has real lateness to count --
//   FastAsPossible's "only wait as long as needed" would let the worker always catch up, which is
//   exactly the behavior this shape needs to NOT have.
//
// Both are bounded by elapsed wall time. A count of fast Tick() calls cannot bound
// asynchronous worker work.
enum class EL2S1DrainPacing
{
	FastAsPossible,
	RealTime,
};

namespace SuperSLML2S1Fixtures
{
	// --- A-CPU (T-2783, D-SLM7229) ---

	// Verified at authoring time (2026-09-18): 514,717,780 bytes, sha256
	// 8dcd082d1dace85874d6924aab7c2389126638a3dc8531566fb6b2298863a980 -- matching the plan's
	// own §9 citation and D-SLM7229's decision-log entry, both re-verified by this suite's own
	// authoring pass (the red-suite record §3). Overridable so a
	// differently-laid-out box can run this suite without editing it.
	inline FString ACpuArtifactPath()
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S1_ACPU_PATH"));
		return Override.IsEmpty() ? SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-0.5b-instruct.sslm")) : Override;
	}
	inline constexpr int64 ACpuArtifactExpectedBytes = 514717780;
	inline const TCHAR* ACpuArtifactExpectedSha256Hex()
	{
		return TEXT("8dcd082d1dace85874d6924aab7c2389126638a3dc8531566fb6b2298863a980");
	}
	// Qwen2.5-0.5B's own config (plan §5: "24 layers, 2 KV heads, head dim 64") -- the value
	// R-S1a's layer_budget=24 sweep arm and R-S1e's KV-block-size math both depend on. Recorded
	// here once so no test re-derives or re-guesses it.
	inline constexpr int32 ACpuNumHiddenLayers = 24;

	// --- R-S1a's reference: one case per selected prompt (§9, D-SLM7229) ---

	struct FReferenceCase
	{
		FString Id;
		int32 SourceIndex = INDEX_NONE;
		FString PromptText;
		TArray<int32> PromptTokens;
		TArray<int32> ExpectedOutputTokens;
		FString ExpectedText;
	};

	inline FString FixtureDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		checkf(Plugin.IsValid(), TEXT("SuperSLMUnreal plugin not found by IPluginManager"));
		return Plugin->GetBaseDir() / TEXT("Source/SuperSLMUnrealEditor/Private/Tests/Fixtures/L2S1");
	}

	// Loads the 20 stride-sampled cases committed at
	// Tests/Fixtures/L2S1/l2s1_r_s1a_reference.json (see that directory's PROVENANCE.md).
	// Returns false with OutError on a missing or malformed file -- never a partial result set.
	inline bool LoadReferenceCases(TArray<FReferenceCase>& OutCases, FString& OutError)
	{
		const FString Path = FixtureDir() / TEXT("l2s1_r_s1a_reference.json");
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			OutError = FString::Printf(TEXT("could not read %s"), *Path);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		FString ReadError;
		if (!SuperSLMJson::TryReadObject(JsonText, Root, ReadError))
		{
			OutError = FString::Printf(TEXT("%s is not a JSON object: %s"), *Path, *ReadError);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* CasesArray = nullptr;
		if (!Root->TryGetArrayField(TEXT("cases"), CasesArray) || CasesArray == nullptr)
		{
			OutError = FString::Printf(TEXT("%s has no 'cases' array"), *Path);
			return false;
		}

		OutCases.Reset(CasesArray->Num());
		for (const TSharedPtr<FJsonValue>& Entry : *CasesArray)
		{
			const TSharedPtr<FJsonObject> Obj = Entry->AsObject();
			if (!Obj.IsValid())
			{
				OutError = FString::Printf(TEXT("%s: a 'cases' entry is not an object"), *Path);
				return false;
			}

			FReferenceCase Case;
			Obj->TryGetStringField(TEXT("id"), Case.Id);
			int32 SrcIdx = INDEX_NONE;
			Obj->TryGetNumberField(TEXT("sourceIndex"), SrcIdx);
			Case.SourceIndex = SrcIdx;
			Obj->TryGetStringField(TEXT("promptText"), Case.PromptText);

			const TArray<TSharedPtr<FJsonValue>>* PromptArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("promptTokens"), PromptArr) && PromptArr != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& V : *PromptArr)
				{
					Case.PromptTokens.Add(static_cast<int32>(V->AsNumber()));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* OutArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("expectedOutputTokens"), OutArr) && OutArr != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& V : *OutArr)
				{
					Case.ExpectedOutputTokens.Add(static_cast<int32>(V->AsNumber()));
				}
			}

			Obj->TryGetStringField(TEXT("expectedText"), Case.ExpectedText);
			OutCases.Add(MoveTemp(Case));
		}

		if (OutCases.Num() != 20)
		{
			OutError = FString::Printf(TEXT("%s: expected 20 cases, found %d"), *Path, OutCases.Num());
			return false;
		}
		return true;
	}

	// --- A-EX (D-SLM7306; tokenizer-bearing rebuild D-SLM7339; plan §10.4 schema rebuild) ---

	// The pinned A-EX is the plan §10.4 rebuild, `qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm`
	// (potion_shop_order and prompt_result in one SCM1 section), built 2026-09-25 and published as
	// the release asset qwen2.5-0.5b-instruct-cap4096.sslm: 510,316,184 bytes, sha256 in the pins that follow, recorded in
	// Tests/Fixtures/L2S1/PROVENANCE.md. No .cpp reads the pins. Its base is the tokenizer-bearing
	// `-tok` rebuild (D-SLM7339, landed 2026-09-19), which added Layer 1's `Tokenizer`,
	// `UnicodeTables` and chat-template sections the original A-EX (D-SLM7306) lacked; the `-tok`
	// file is a build input now, not a pin.
	inline constexpr int64 AExArtifactExpectedBytes = 510316184;
	inline const TCHAR* AExArtifactExpectedSha256Hex()
	{
		return TEXT("9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da");
	}
	inline const TCHAR* AExSchemaName() { return TEXT("potion_shop_order"); }

	// Fold-round ruling 6 (D-SLM7847): the default is the plan §10.4 rebuild of A-EX (potion_shop_order
	// and prompt_result in one SCM1 section, on the tokenizer-carrying -tok base), expected under
	// SUPERSLM_ARTIFACTS_DIR's superslm/aex/ (built 2026-09-25; the pins above name it). It is the
	// release asset qwen2.5-0.5b-instruct-cap4096.sslm under another name: same size and SHA-256.
	inline bool TryGetAExArtifactPath(FString& OutPath, FString& OutReason)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S1_AEX_PATH"));
		const FString Candidate = Override.IsEmpty()
			? SuperSLMTestDataPaths::ArtifactsDir(TEXT("superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm"))
			: Override;

		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutPath = Candidate;
			return true;
		}

		OutReason = FString::Printf(TEXT(
			"A-EX (the plan §10.4 rebuild, qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm) not found at '%s'. "
			"It is the release asset qwen2.5-0.5b-instruct-cap4096.sslm (same bytes): download it and place it there under this name, or set SUPERSLM_L2S1_AEX_PATH to it."), *Candidate);
		return false;
	}

	// --- A-AD (§9: the 1.5B base + the shopkeeper-v2 runtime adapter) ---

	// Not an R-S1 cell's own headline artifact (R-S3a, L2-S3, is A-AD's real consumer) -- but
	// R-S1f's adapter-swap MECHANISM test (SuperSLMAdapterHandle.h) needs a real adapter file to
	// map, and this is the one the plan already names (§9). Verified present at authoring time:
	// base 1,585,485,524 bytes; adapter 26,300,552 bytes, matching the plan's own citations.
	inline FString AAdBaseModelPath() { return SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-instruct.sslm")); }
	inline FString AAdAdapterPath() { return SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-shopkeeper-lora-v2-t2102-runtime.sslm")); }

	// --- Subsystem access and a generation-driving loop, shared across every R-S1 test file ---

	// T-2815 build log D1 (reasoned from engine source, confirmed against
	// Engine/Public/Tests/AutomationCommon.cpp's own FTestWorldWrapper::CreateTestWorld): the
	// editor `-Test` run creates no PIE and no game instance, so a plain scan of
	// GEngine->GetWorldContexts() never finds a world whose OwningGameInstance is non-null and
	// USuperSLMSubsystem (a UGameInstanceSubsystem) is unreachable. Every RunTest() now creates
	// its own FTestWorldWrapper of EWorldType::Game -- the one case that constructs a
	// UGameInstance, Init()s it, and assigns it to the new world context's OwningGameInstance --
	// and this helper reads the subsystem off THAT world, passed in explicitly rather than
	// re-scanned, so a test's own wrapper is unambiguously the one it just created.
	inline USuperSLMSubsystem* GetSubsystem(UWorld* World)
	{
		if (!World || !World->GetGameInstance())
		{
			return nullptr;
		}
		return World->GetGameInstance()->GetSubsystem<USuperSLMSubsystem>();
	}

	// The shared pacing loop every L2S1 drain site uses (T-2805 round 10; see this header's own
	// EL2S1DrainPacing comment above for the two modes and why each exists). Returns the number
	// of Tick() calls actually made -- callers that want to distinguish "converged" from "hit the
	// cap" read Done() again after this returns, since a wall-clock-capped or tick-capped exit
	// looks identical from the return value alone.
	inline int32 DrainTicks(
		USuperSLMSubsystem& Subsystem,
		float StepSeconds,
		EL2S1DrainPacing Pacing,
		TFunctionRef<bool()> Done,
		double MaxWallClockSeconds = 30.0)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		int32 TicksRun = 0;
		while (!Done() && FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (Pacing == EL2S1DrainPacing::RealTime)
			{
				// Pace the CALL itself to real time, like a real game frame -- this is what lets a
				// job genuinely arrive late against its own committed delivery tick, which is the
				// property R-S1g/R-S1i-under-load/the hitch counter each need to observe.
				FPlatformProcess::Sleep(StepSeconds);
			}
			Subsystem.Tick(StepSeconds);
			++TicksRun;
			if (Done() || FPlatformTime::Seconds() - StartSeconds > MaxWallClockSeconds)
			{
				break;
			}
			if (Pacing == EL2S1DrainPacing::FastAsPossible)
			{
				// Give the worker thread(s) real wall-clock time to make progress before polling
				// again -- the fix for §14.7: an unpaced loop starves the worker of the real time
				// its Layer-1 call needs, regardless of how many logical ticks remain in the cap.
				// Only slept when work is genuinely still outstanding, so a run that is already
				// done never pays this cost -- "runs as on a fast-enough machine, with no hitches".
				FPlatformProcess::Sleep(0.001f);
			}
		}
		return TicksRun;
	}

	// Drives BeginGeneration() to Complete/Faulted, paced per EL2S1DrainPacing (default
	// FastAsPossible -- every current caller is a content/order cell, which exercises the real delivery gate).
	// Every R-S1 cell that runs a real generation to completion shares this one loop, so a defect
	// in the loop itself (as opposed to the subsystem under test) cannot silently differ cell to
	// cell.
	//
	// Done means the phase is Complete or Faulted AND nothing is pending on the sequence. While a
	// reset queued ahead of the generation is undelivered, the sequence still reads its previous
	// phase and tokens, Complete included; GetPendingLifecycleOperationCount() counts the queued
	// generation (and one while it runs), so it reads 0 only once this generation has ended or
	// was dropped at its turn. A lifecycle op queued behind the generation only makes the loop
	// wait for it.
	inline bool RunGenerationToCompletion(
		USuperSLMSubsystem& Subsystem,
		const FSuperSLMSequence& Seq,
		const FSuperSLMGenerationRequest& Request,
		TArray<int32>& OutTokens,
		double MaxWallClockSeconds,
		FString& OutError,
		EL2S1DrainPacing Pacing = EL2S1DrainPacing::FastAsPossible)
	{
		FString BeginError;
		if (!Subsystem.BeginGeneration(Seq, Request, BeginError))
		{
			OutError = FString::Printf(TEXT("BeginGeneration failed: %s"), *BeginError);
			return false;
		}

		constexpr float StepSeconds = 1.0f / 60.0f;
		auto IsDone = [&Subsystem, &Seq]()
		{
			const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Seq);
			return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) &&
				Subsystem.GetPendingLifecycleOperationCount(Seq) == 0;
		};
		DrainTicks(Subsystem, StepSeconds, Pacing, IsDone, MaxWallClockSeconds);

		const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Seq);
		if (!IsDone())
		{
			// A Complete read here with work still pending is the previous generation's.
			OutError = FString::Printf(TEXT("did not reach Complete/Faulted with nothing pending within %.0f seconds (phase %d, %d pending)"),
				MaxWallClockSeconds, (int32)Phase, Subsystem.GetPendingLifecycleOperationCount(Seq));
			return false;
		}
		if (Phase == ESuperSLMSequencePhase::Complete)
		{
			OutTokens = Subsystem.GetGeneratedTokens(Seq);
			return true;
		}
		if (Phase == ESuperSLMSequencePhase::Faulted)
		{
			OutTokens = Subsystem.GetGeneratedTokens(Seq);
			OutError = TEXT("sequence faulted before completion");
			return false;
		}
		OutError = FString::Printf(TEXT("did not reach Complete/Faulted within %.0f seconds"), MaxWallClockSeconds);
		return false;
	}

	// R-S1i's own "added concurrent CPU load" (plan §9): a pure CPU-bound spin, no sleep or
	// yield, genuinely contending whatever core(s) the SuperSLM worker thread(s) share -- never a
	// fabricated GPU/IO load, matching this plan's own standing rule against a synthetic load
	// that does not exercise the real contention path (R-S2f's own precedent: "no synthetic load
	// stands in for rendering"). Shared (round 10) so R-S1i and the ImportGate hitch-counter arm
	// (SuperSLML2S1ImportGateTests.cpp) use the identical contention mechanism rather than two
	// independently-authored approximations of it.
	class FL2S1BusyLoadRunnable : public FRunnable
	{
	public:
		virtual uint32 Run() override
		{
			while (!bStop.load(std::memory_order_relaxed))
			{
				volatile double X = 0.0;
				for (int32 I = 0; I < 200000; ++I)
				{
					X += FMath::Sin((double)I) * FMath::Cos((double)I);
				}
			}
			return 0;
		}
		virtual void Stop() override { bStop.store(true, std::memory_order_relaxed); }

	private:
		std::atomic<bool> bStop{ false };
	};

	// Starts N busy-spin threads (N derived from this box's own core count, never hardcoded, and
	// never fewer than 2) and returns them for the caller to Kill() once the loaded run completes.
	inline void StartL2S1BusyLoad(TArray<FL2S1BusyLoadRunnable*>& OutRunnables, TArray<FRunnableThread*>& OutThreads, const TCHAR* Label)
	{
		const int32 ThreadCount = FMath::Max(2, FPlatformMisc::NumberOfCoresIncludingHyperthreads() / 2);
		for (int32 I = 0; I < ThreadCount; ++I)
		{
			FL2S1BusyLoadRunnable* Runnable = new FL2S1BusyLoadRunnable();
			FRunnableThread* Thread = FRunnableThread::Create(Runnable, *FString::Printf(TEXT("%s.%d"), Label, I));
			OutRunnables.Add(Runnable);
			OutThreads.Add(Thread);
		}
	}

	inline void StopL2S1BusyLoad(TArray<FL2S1BusyLoadRunnable*>& Runnables, TArray<FRunnableThread*>& Threads)
	{
		for (FRunnableThread* Thread : Threads)
		{
			if (Thread)
			{
				Thread->Kill(/*bShouldWait*/ true);
				delete Thread;
			}
		}
		for (FL2S1BusyLoadRunnable* Runnable : Runnables)
		{
			delete Runnable;
		}
		Threads.Reset();
		Runnables.Reset();
	}

#if SUPERSLM_WITH_L2S1_ASYNC
	// Everything below this point targets the async worker tick's own API (the per-job ledger,
	// FSuperSLMLifecycleOpHandle, handle-based AdoptPrefix) -- gated with the test files that
	// consume it (SuperSLMSlotGates.h), so this header stays usable, unconditionally, by every
	// OTHER L2-S1 test file that has not moved to the async shape yet.

	// --- The static per-unit cost model itself (plan §5, D-SLM7414/D-SLM7422, round 9) ---

	// The SAME formula the scheduler's own Plan step uses to size PlannedJobMs -- defined once so
	// every cell that checks a job's composition against it (R-S1b (i), R-S1i, R-S1j, RoutedPins
	// pin 3) computes the SAME expectation. A formula duplicated per test file risks two
	// independently-reasoned "expected" values silently disagreeing with each other rather than
	// with the product.
	//
	// T-2885 round 12 (build log §20.3): this switch used to carry a combined
	// `DecodeOrPrefill`/`default:` case, so the two new PrefixBegin/PrefixRelease kinds
	// (D-SLM7504) fell through it silently and returned 0.0 -- a plausible-looking number for an
	// input the oracle had never been taught, which regressed EightConcurrentSequences (a
	// currently-red cell reads as a stale oracle, not a currently-green cell reading as broken;
	// this one read the other way, which is the more dangerous direction). Every declared
	// ESuperSLMWorkerJobKind now has its own named case and there is no `default:` -- a value
	// added to the enum without a matching case here fails at compile time under `-Wswitch`
	// (an enum switch with no default warns on any uncovered enumerator) rather than silently
	// returning a number nobody asked for. The runtime path below is the second layer, for a raw
	// Job.Kind value outside the enum's declared range (not reachable through any current
	// production call site, since PostLedgerJob only ever constructs a declared enumerator) --
	// it fails the calling test outright rather than returning a value at all.
	inline double ExpectedPlannedJobMs(FAutomationTestBase& T, const FSuperSLMWorkerJobReport& Job, const FSuperSLMRuntimeConfig& Config)
	{
		switch (Job.Kind)
		{
			case ESuperSLMWorkerJobKind::Reset:         return Config.ResetCostMs;
			case ESuperSLMWorkerJobKind::Adopt:         return Config.AdoptCostMs;
			case ESuperSLMWorkerJobKind::Save:          return Config.SaveCostMs;
			case ESuperSLMWorkerJobKind::Restore:       return Config.RestoreCostMs;
			case ESuperSLMWorkerJobKind::PrefixBegin:   return Config.PrefixBeginCostMs;
			case ESuperSLMWorkerJobKind::PrefixRelease: return Config.PrefixReleaseCostMs;
			case ESuperSLMWorkerJobKind::DecodeOrPrefill:
				return Job.DecodeLayers * Config.LayerCostMs
					+ Job.DecodeLayerDepthSum * Config.LayerCostPerPositionMs
					+ Job.PromptTokens * Config.PromptTokenCostMs
					+ Job.PromptPositionSum * Config.PromptTokenCostPerPositionMs
					+ Job.TokenFinishes * Config.FinishCostMs;
		}
		T.AddError(FString::Printf(
			TEXT("ExpectedPlannedJobMs: unhandled ESuperSLMWorkerJobKind value %d (job %lld) -- the oracle was never taught this kind; teach it a case rather than trust the fallthrough"),
			(int32)Job.Kind, Job.JobId));
		return -1.0; // never a legitimate cost; visibly wrong in any log line that prints it
	}

	// D-SLM7407: K = max(1, ceil(PlannedJobMs / TickBudgetMs)).
	inline int32 ExpectedK(double PlannedJobMs, double TickBudgetMs)
	{
		return FMath::Max(1, (int32)FMath::CeilToInt(PlannedJobMs / TickBudgetMs));
	}

	// --- The R-S1b/R-S1i canonical 8-sequence shape (T-2805 round 9, D-SLM7407/D-SLM7414) ---

	// Plan §9 R-S1b's own row: "A-EX, 8 concurrent sequences, 64 tokens each, text prompts,
	// handles recycled, one shared persona prefix adopted by each (D-SLM7342)." One construction,
	// shared by EightConcurrentSequences (R-S1b) and both runs of DeliveryTickIdentityAcrossLoad
	// (R-S1i, D-SLM7424: "R-S1i's own fixture already ran these calls") -- so the two runs R-S1i
	// compares are genuinely the SAME request schedule, not two independently-authored
	// approximations of it that could silently diverge on a detail neither suite notices.
	struct FEightSequenceRunResult
	{
		bool bOk = false;
		TArray<FSuperSLMSequence> Sequences;              // still vended; caller returns them
		TArray<FSuperSLMWorkerJobReport> Ledger;           // copied out -- Configure() clears the live one
		int32 HitchCount = 0;
		int32 TicksRun = 0;
		// The shared persona prefix every sequence adopted, still live when the run returns (L4,
		// 2026-09-25): R-S1h's rebuilt capture releases it inside the capture window so
		// SuperSLM.PrefixRelease is exercised; every other caller may ignore it.
		FSuperSLMPrefix Prefix;
	};

	// Pacing (T-2805 round 10, see this header's own EL2S1DrainPacing comment): FastAsPossible for
	// R-S1b/R-S1h's own run (content/ledger-composition cells). R-S1g and BOTH of R-S1i's own
	// runs use RealTime (T-2805 round 12 correction: plan §9 R-S1i requires "the SAME tick-by-tick
	// due-set" across its two runs, differing only in an added CPU-bound load thread -- since
	// TickIndex is a per-Tick()-call counter, pacing baseline under FastAsPossible and loaded
	// under RealTime, as round 10 originally did, varies the due-set-determining call cadence
	// itself, not only the load. Both of R-S1i's own runs now share RealTime so the worker must
	// genuinely be able to fall behind its committed delivery tick for the assertion to mean
	// anything, in either run).
	inline bool RunEightSequenceSharedPrefixShape(
		FAutomationTestBase& T,
		USuperSLMSubsystem& Subsystem,
		USuperSLMModel& Model,
		double TickBudgetMs,
		FEightSequenceRunResult& OutResult,
		FString& OutError,
		EL2S1DrainPacing Pacing = EL2S1DrainPacing::FastAsPossible,
		double PromptTokenCostMsOverride = -1.0)
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 8;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24; // A-EX is Qwen2.5-0.5B: 24 hidden layers
		Config.BlockCount = 8;
		Config.PrefixBlockCount = 1;
		// MaxConcurrentCalls left at its default of 1 (D-SLM7341): batched decode serves all 8
		// sequences in one job on one lane, which is exactly what this shape and R-S1b/R-S1i claim.
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = TickBudgetMs;
		// R-S1h only (box session 2026-09-25): at the default prompt-token cost (36 ms) one token
		// already exceeds the tick's share, so every prefill chunk is one token and a per-token count
		// cannot be told from a per-call count. A caller that needs multi-token chunks prices a
		// prompt token below the share; every other caller leaves the default.
		if (PromptTokenCostMsOverride >= 0.0)
		{
			Config.PromptTokenCostMs = PromptTokenCostMsOverride;
			Config.PromptTokenCostPerPositionMs = 0.0;
		}

		// The caller reads the whole run's ledger and tick history, so this configuration keeps
		// every row. The capacity is applied here, at Configure(), and outlives the scope.
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
		const FSuperSLMConfigureReport Report = Subsystem.Configure(&Model, Config);
		if (!T.TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			OutError = Report.Message;
			return false;
		}

		TArray<int32> PersonaTokens;
		if (!T.TestTrue(TEXT("Tokenize persona prefix"), Subsystem.Tokenize(
				TEXT("You are a friendly, talkative tattoo artist who loves telling stories about past clients."),
				PersonaTokens)))
		{
			OutError = TEXT("Tokenize failed");
			return false;
		}

		FSuperSLMPrefix Prefix;
		FString PrefixError;
		if (!T.TestTrue(*FString::Printf(TEXT("CreatePrefix: %s"), *PrefixError), Subsystem.CreatePrefix(PersonaTokens, Prefix, PrefixError)))
		{
			OutError = PrefixError;
			return false;
		}
		DrainTicks(Subsystem, 1.0f / 60.0f, Pacing,
			[&Subsystem, &Prefix]() { return Subsystem.IsPrefixReady(Prefix); },
			/*MaxWallClockSeconds*/ 30.0);
		if (!T.TestTrue(TEXT("prefix must become ready within the wall-clock cap"), Subsystem.IsPrefixReady(Prefix)))
		{
			OutError = TEXT("prefix never became Ready");
			return false;
		}
		OutResult.Prefix = Prefix;

		constexpr int32 SequenceCount = 8;
		constexpr int32 TokensPerPrompt = 64;
		OutResult.Sequences.SetNum(SequenceCount);

		// T-2885 (build log §19.2): every sequence is vended and every AdoptPrefix() queued here,
		// BEFORE any drain runs -- so all eight requests reach the subsystem's own Plan step
		// while it sits at the same tick, and the ADMISSION ORDER (hence which tick each
		// sequence's own first job lands on) is a pure function of the static, schedule-only
		// per-tick admission rule (D-SLM7407: at most one lifecycle-kind job admitted per tick),
		// never of how long any one sequence's own Adopt happens to take in real wall-clock time.
		// The prior shape vended sequence I+1 only after DRAINING sequence I's own AdoptPrefix to
		// REAL completion -- a second, real-time-gated serialization layered on top of the
		// product's schedule-only one, entirely of this fixture's own making (confirmed at
		// source: MaxConcurrentCalls stays at its default of 1 here, so this was never a
		// lane-count effect). Under FastAsPossible vs. RealTime pacing, that per-sequence real
		// wait differed, so each sequence's own setup finished at a different real elapsed time
		// between the two runs DeliveryTickIdentityAcrossLoad (R-S1i) compares, landing its first
		// job at a different tick each time -- a test-fixture defect, not a product one.
		TArray<FSuperSLMLifecycleOpHandle> AdoptHandles;
		AdoptHandles.SetNum(SequenceCount);
		for (int32 I = 0; I < SequenceCount; ++I)
		{
			if (!T.TestEqual(*FString::Printf(TEXT("Vend sequence %d"), I),
					(uint8)Subsystem.VendSequence(OutResult.Sequences[I]), (uint8)ESuperSLMVendResult::Success))
			{
				OutError = FString::Printf(TEXT("vend %d failed"), I);
				return false;
			}

			FString AdoptError;
			const ESuperSLMRestoreResult QueueResult = Subsystem.AdoptPrefix(OutResult.Sequences[I], Prefix, AdoptHandles[I], AdoptError);
			if (!T.TestEqual(*FString::Printf(TEXT("AdoptPrefix %d must queue: %s"), I, *AdoptError),
					(uint8)QueueResult, (uint8)ESuperSLMRestoreResult::Success))
			{
				OutError = FString::Printf(TEXT("AdoptPrefix %d refused"), I);
				return false;
			}
		}

		// One shared drain for all eight handles together, on the run's own Pacing -- never one
		// drain per sequence. This is the fix's whole effect: the SCHEDULE that results depends
		// only on the eight AdoptPrefix() calls having all been queued before any tick ran, never
		// on how many real ticks or how much real wall-clock time this shared wait itself takes.
		auto AllAdoptsResolved = [&Subsystem, &AdoptHandles]()
		{
			for (const FSuperSLMLifecycleOpHandle& Handle : AdoptHandles)
			{
				if (Subsystem.GetLifecycleOpResult(Handle) == ESuperSLMRestoreResult::Pending)
				{
					return false;
				}
			}
			return true;
		};
		DrainTicks(Subsystem, 1.0f / 60.0f, Pacing, AllAdoptsResolved, /*MaxWallClockSeconds*/ 30.0);

		for (int32 I = 0; I < SequenceCount; ++I)
		{
			const ESuperSLMRestoreResult AdoptOutcome = Subsystem.GetLifecycleOpResult(AdoptHandles[I]);
			if (!T.TestEqual(*FString::Printf(TEXT("AdoptPrefix %d must drain to Success"), I),
					(uint8)AdoptOutcome, (uint8)ESuperSLMRestoreResult::Success))
			{
				OutError = FString::Printf(TEXT("AdoptPrefix %d did not drain"), I);
				return false;
			}
		}

		// Every BeginGeneration() is likewise issued here with no drain between calls, so all
		// eight generations are posted while the subsystem sits at the same tick the shared
		// Adopt-drain above left it at -- matching the "no ticks between consecutive setup calls"
		// shape the prior per-sequence loop already had for THIS transition (nothing drained
		// between one sequence's own BeginGeneration and the next sequence's own Vend); only the
		// AdoptPrefix step was real-time-gated, and that is what moved above.
		for (int32 I = 0; I < SequenceCount; ++I)
		{
			TArray<int32> Continuation;
			const FString PromptText = FString::Printf(TEXT("Client %d walked in and asked:"), I);
			Subsystem.Tokenize(PromptText, Continuation);
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Continuation;
			Request.MaxNewTokens = TokensPerPrompt;
			FString BeginError;
			if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration %d: %s"), I, *BeginError),
					Subsystem.BeginGeneration(OutResult.Sequences[I], Request, BeginError)))
			{
				OutError = FString::Printf(TEXT("BeginGeneration %d failed"), I);
				return false;
			}
		}

		auto AllComplete = [&Subsystem, &OutResult]()
		{
			for (const FSuperSLMSequence& Seq : OutResult.Sequences)
			{
				const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Seq);
				if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
				{
					return false;
				}
			}
			return true;
		};
		// Generous in wall time, not tick count (a tick-count bound would encode a timing assumption): 8 sequences x 64 tokens,
		// each token a prefill or decode job whose real worker cost is tens of milliseconds --
		// this run genuinely needs real wall-clock time to finish, whichever pacing mode is used.
		OutResult.TicksRun = DrainTicks(Subsystem, 1.0f / 60.0f, Pacing, AllComplete, /*MaxWallClockSeconds*/ 180.0);
		if (!T.TestTrue(TEXT("all 8 sequences reached Complete or Faulted"), AllComplete()))
		{
			OutError = TEXT("timed out before all 8 sequences completed");
			return false;
		}

		OutResult.Ledger = Subsystem.GetJobLedger();
		OutResult.HitchCount = Subsystem.GetHitchCount();
		OutResult.bOk = true;
		return true;
	}

	// Asserts plan §9 R-S1b's five threshold-free properties against a completed run's job
	// ledger and Config. Every individual assertion still reports through T so a specific
	// violated property is diagnosable from the log; the return is the AND of all of them. Shared
	// by EightConcurrentSequences (R-S1b itself, SuperSLML2S1FrameBudgetTests.cpp) and
	// DeliveryTickIdentityAcrossLoad (R-S1i, which asserts identity ACROSS two such checks rather
	// than re-deriving them).
	inline bool AssertRS1bProperties(
		FAutomationTestBase& T,
		const TArray<FSuperSLMWorkerJobReport>& Ledger,
		const FSuperSLMRuntimeConfig& Config,
		int32 ReportedHitchCount,
		uint32 CallingThreadId,
		const TCHAR* RunLabel)
	{
		bool bOk = true;
		int32 ExpectedHitches = 0;
		TMap<int64, int32> LastDeliveredTickBySequence; // (iv): never reordered, per owner's own arrival order

		for (const FSuperSLMWorkerJobReport& Job : Ledger)
		{
			// (i) planned cost equals the static-cost-model figure for the job's own composition;
			// K and the committed delivery tick fix from it, at plan time, never revised after.
			const double Expected = ExpectedPlannedJobMs(T, Job, Config);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s job %lld (kind %d): PlannedJobMs must equal the static-cost-model figure (got %.6f ms, expected %.6f ms)"),
					RunLabel, Job.JobId, (int32)Job.Kind, Job.PlannedJobMs, Expected),
				FMath::IsNearlyEqual(Job.PlannedJobMs, Expected, 1e-6));

			const int32 ExpectedKValue = ExpectedK(Job.PlannedJobMs, Config.TickBudgetMs);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s job %lld: K must equal max(1, ceil(PlannedJobMs/TickBudgetMs)) (got %d, expected %d)"),
					RunLabel, Job.JobId, Job.K, ExpectedKValue),
				Job.K, ExpectedKValue);

			bOk &= T.TestEqual(*FString::Printf(TEXT("%s job %lld: CommittedDeliveryTick must equal PlannedAtTick + K"), RunLabel, Job.JobId),
				Job.CommittedDeliveryTick, Job.PlannedAtTick + Job.K);

			if (Job.DeliveredAtTick < 0)
			{
				continue; // never delivered inside this run's own tick cap -- reported, not asserted, below
			}

			// (ii) no Layer-1 symbol on the game-thread call stack: this job's own worker thread
			// id is real and never the calling (game) thread's -- confirmed jointly with R-S1h's
			// own trace-content read (plan §9 R-S1b (ii)).
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s job %lld: must run on a worker thread, never the calling (game) thread (worker id %u, calling id %u)"),
					RunLabel, Job.JobId, Job.WorkerThreadId, CallingThreadId),
				Job.WorkerThreadId != 0 && Job.WorkerThreadId != CallingThreadId);

			// (iii) hitch source: a result delivered after its own committed delivery tick, AND
			// the worker had more than that many ticks' worth of real wall time for it (plan §5
			// "What counts as a hitch", D-SLM7407). Review round 4, R4-S1: the rule is applied
			// here from the ledger's own figures and bHitch is checked against it, rather than
			// bHitch being summed on trust. The span is FSuperSLMWorkerJobReport::WorkerSpanMs (ms,
			// finished minus posted, -1 until delivered).
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s job %lld: WorkerSpanMs is set on delivery (%.3f)"),
					RunLabel, Job.JobId, Job.WorkerSpanMs),
				Job.WorkerSpanMs >= 0.0);
			const bool bLateByTicks = Job.DeliveredAtTick > Job.CommittedDeliveryTick;
			const bool bExpectedHitch = bLateByTicks && Job.WorkerSpanMs > double(Job.K) * Config.TickBudgetMs;
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s job %lld: bHitch must equal (delivered tick %d > committed %d) AND (WorkerSpanMs %.3f > K %d x TickBudgetMs %.3f)"),
					RunLabel, Job.JobId, Job.DeliveredAtTick, Job.CommittedDeliveryTick, Job.WorkerSpanMs, Job.K, Config.TickBudgetMs),
				Job.bHitch, bExpectedHitch);
			if (bExpectedHitch)
			{
				++ExpectedHitches;
			}
			// (v)'s own worker-side overrun also counts toward GetHitchCount() (plan §5: "GetHitchCount()
			// counts both"), tallied once per job regardless of how many internal Layer-1 calls
			// overran -- a documented simplification, since a job's own report carries one scalar
			// WorkerCallMs/bWorkerOverran rather than a per-internal-call breakdown.
			if (Job.bWorkerOverran)
			{
				++ExpectedHitches;
			}

			// (iv) a finished result may apply before its planned tick at U1. It cannot
			// apply before the planning tick, and cannot overtake an earlier result -- checked
			// per owner (one shared key for every DecodeOrPrefill job, since with the default one
			// worker lane they drain strictly in commit order on one thread; each lifecycle-op
			// job's own sequence for the rest).
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s job %lld: DeliveredAtTick (%d) must not precede PlannedAtTick (%d)"),
					RunLabel, Job.JobId, Job.DeliveredAtTick, Job.PlannedAtTick),
				Job.DeliveredAtTick >= Job.PlannedAtTick);

			const int64 OwnerKey = Job.Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill ? -1 : Job.LifecycleOpSequence.Id;
			if (const int32* PrevTick = LastDeliveredTickBySequence.Find(OwnerKey))
			{
				bOk &= T.TestTrue(*FString::Printf(TEXT("%s job %lld: must not deliver before an earlier job's own delivery (prev tick %d, this tick %d)"),
						RunLabel, Job.JobId, *PrevTick, Job.DeliveredAtTick),
					Job.DeliveredAtTick >= *PrevTick);
			}
			LastDeliveredTickBySequence.Add(OwnerKey, Job.DeliveredAtTick);

			// (v) a job's own Layer-1 call time exceeding TickBudgetMs is reported as a
			// worker-side overrun exactly on that job, independent of K's own headroom.
			const bool bExpectedOverran = Job.WorkerCallMs > Config.TickBudgetMs;
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s job %lld: bWorkerOverran must equal (WorkerCallMs %.3f ms > TickBudgetMs %.3f ms)"),
					RunLabel, Job.JobId, Job.WorkerCallMs, Config.TickBudgetMs),
				Job.bWorkerOverran, bExpectedOverran);
		}

		// (iii)+(v) continued: GetHitchCount() must equal the late jobs by the rule above plus the
		// worker overruns, both derived from the ledger's own figures, so this checks the counter
		// against the rule, not merely that it moved.
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: GetHitchCount() must equal late jobs (by ticks and real time) plus worker overruns"), RunLabel),
			ReportedHitchCount, ExpectedHitches);

		return bOk;
	}
#endif // SUPERSLM_WITH_L2S1_ASYNC
}
