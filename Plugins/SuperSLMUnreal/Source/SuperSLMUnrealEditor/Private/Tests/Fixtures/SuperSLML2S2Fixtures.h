#pragma once

// T-2816 (L2-S2 red suite). Locates the real GPU-arm artifacts and reference
// data R-S2a-R-S2g run against (at least one test runs the real artifact, at real size, on
// real input), and the
// shared subsystem-access/generation-loop helpers every L2-S2 test file uses.
//
// Deliberately does NOT #include Fixtures/SuperSLML2S1Fixtures.h, even though both
// suites' cells run against the SAME A-EX artifact: T-2815 (the L2-S1 build) and a
// second, concurrent test suite are both actively editing files under this same
// Tests/ directory tree right now (this suite's task scope), so this file avoids any
// compile-time coupling to a sibling file that may be mid-edit. The resulting
// duplication (an independent A-EX path lookup, below) is small and self-contained;
// unifying the two once L2-S1 lands is a housekeeping matter, not a correctness one --
// see the red-suite record §4 API choice 1.
//
// This header is NOT production code: it compiles only inside WITH_DEV_AUTOMATION_TESTS
// translation units, matching SuperSLML2S1Fixtures.h's own convention. The
// SUPERSLM_WITH_L2S2 gate this comment once described is RETIRED (T-2826 build log §2
// item 0, 2026-09-19): the GPU backend is now implemented and unstaged, so the six
// headers this file depends on compile unconditionally on Win64
// (SUPERSLMUNREAL_WITH_GPU), the same as every other plugin header.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "SuperSLMTestDataPaths.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMSubsystem.h"

namespace SuperSLML2S2Fixtures
{
	// --- A-EX (plan §8, §12 decision 9; D-SLM7306, superseded by D-SLM7339; PRODUCED,
	// the build record §10) ---

	// SUPERSEDED mid-authoring (D-SLM7339, 2026-09-18): the FIRST A-EX build
	// (696c2a4a...) carried no tokenizer section, so Tokenize() fails on every
	// text-built prompt (T-2815's own finding, D-SLM7339's provenance) -- this
	// fixture's own PinnedSelfCheckPrompt()/tokenize-then-compare pattern needs a
	// tokenizer-carrying artifact to run at all. The rebuild (an intermediate build input,
	// not published) added Layer 1's Tokenizer/UnicodeTables/chat-template sections
	// (types 20-22) alongside the SCM1 schema (30): sections
	// [0, 2, 3, 4, 6, 7, 8, 9, 12, 30, 20, 22, 21]; context_cap 4096. The release asset
	// keeps that section list. An independent
	// lookup of this SAME artifact L2-S1's own TryGetAExArtifactPath() names
	// (Fixtures/SuperSLML2S1Fixtures.h) -- a DIFFERENT override variable
	// (SUPERSLM_L2S2_AEX_PATH, not SUPERSLM_L2S1_AEX_PATH), per this file's own header
	// note on why the two fixture files are not coupled.
	// Fold-round ruling 6 (D-SLM7847): the default is the plan §10.4 rebuild of A-EX (potion_shop_order
	// and prompt_result in one SCM1 section, on the tokenizer-carrying -tok base), expected under
	// SUPERSLM_ARTIFACTS_DIR's superslm/aex/ (built 2026-09-25; the pins below name it). It is the
	// release asset qwen2.5-0.5b-instruct-cap4096.sslm under another name: same size and SHA-256.
	inline bool TryGetAExArtifactPath(FString& OutPath, FString& OutReason)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S2_AEX_PATH"));
		const FString Candidate = Override.IsEmpty()
			? SuperSLMTestDataPaths::ArtifactsDir(TEXT("superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm"))
			: Override;

		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutPath = Candidate;
			return true;
		}

		OutReason = FString::Printf(TEXT(
			"A-EX (tokenizer-carrying) not found at '%s'. It is the release asset "
			"qwen2.5-0.5b-instruct-cap4096.sslm (same bytes): download it and place it "
			"there under this name, or set SUPERSLM_L2S2_AEX_PATH "
			"to it."), *Candidate);
		return false;
	}

	// These pins name the plan §10.4 rebuild (no .cpp reads them), built 2026-09-25 on the -tok
	// base and published as the release asset qwen2.5-0.5b-instruct-cap4096.sslm: 510,316,184 bytes, whole-file SHA-256 below,
	// header integrity hash 83e96aa46e1404c824e2bfea22e1511a528298a2edd32299e69a77f414e2007a.
	inline constexpr int64 AExArtifactExpectedBytes = 510316184;
	inline const TCHAR* AExArtifactExpectedSha256Hex()
	{
		return TEXT("9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da");
	}

	// The tokenizer's own stop-token set (D-SLM7339, plan §8): <|im_end|> (151645) and
	// <|endoftext|> (151643) for Qwen2.5's chat template. FSuperSLMGenerationRequest carries
	// PromptTokens, MaxNewTokens, SpanKind and StopTokenIds (SuperSLMSequenceTypes.h): a
	// sequence completes on MaxNewTokens or on the first produced token in StopTokenIds, which
	// the caller supplies because the .sslm file records no end-of-text id. This suite's own
	// requests leave StopTokenIds empty and stop on MaxNewTokens; some L2-S3 tests pass these
	// two ids (SuperSLML2S3Fixtures.h re-exports them).
	inline constexpr int32 AExStopTokenImEnd = 151645;
	inline constexpr int32 AExStopTokenEndOfText = 151643;

	// Qwen2.5-0.5B's own config (plan §5: "24 layers"), shared with L2-S1's identically
	// named constant -- the model's own architecture fact, not a fixture choice, so it
	// is not expected to diverge between the two suites.
	inline constexpr int32 AExNumHiddenLayers = 24;
	inline constexpr bool AExHasQkNorm = false; // Qwen2.5 is a legacy model (plan §5, §9 R-S2f arithmetic)

	// --- The demo schema (plan §8) ---

	// The REAL name compiled into A-EX's SCM1 section, confirmed at source in the
	// build record's schema-append step (`SCHEMA_NAME = "potion_shop_order"`) -- not a
	// guess (the plan itself names no schema-name string, only the compiled fields, §8).
	inline const TCHAR* DemoSchemaName() { return TEXT("potion_shop_order"); }

	// A pinned prompt for the demo schema (plan §7 item 11: "a pinned prompt"), chosen
	// to reach the schema's own "buy" intent in a few constrained tokens -- the exact
	// TEXT is this suite's own placeholder choice pending A-EX's real production
	// record naming one; R-S2g's own construction ("decode a few constrained tokens")
	// needs any prompt that reaches the schema's accepting path, not a specific one.
	inline const TCHAR* PinnedSelfCheckPrompt() { return TEXT("I would like to buy a health potion, please."); }

	// --- The self-check's shipped reference file (plan §7 item 11; R-S2b) ---

	// PRODUCED (T-2826 build log §3.1, 2026-09-19): the implementation ran Layer 1's own
	// sslm_generate at the pin and shipped
	// Resources/SelfCheck/<hash>.reference_digests.json, keyed by A-EX's own header integrity
	// hash (distinct from the whole-file SHA-256, AExArtifactExpectedSha256Hex() above). The
	// plan §10.4 rebuild's file is 83e96aa4....reference_digests.json (2026-09-25; its tokens are
	// identical to the pre-rebuild 9d82879d... file's, which it replaces). The
	// JSON shape is this suite's OWN test-design choice (the L2-S2
	// red-suite record §3), which the shipped file matches exactly:
	//   { "artifactHash": "<hex>",
	//     "entries": [ { "backend": "CPU", "granularity": "LayerBudget1", "tokenDigestHex": "<hex>", "tokens": [...] }, ... ] }
	//
	// Resolved by SCANNING Resources/SelfCheck/ for the one shipped
	// "*.reference_digests.json" file, rather than hard-coding the hash-keyed filename:
	// a future A-EX re-build changes the header integrity hash (and therefore the
	// filename) without this fixture needing an edit, matching how a production
	// self-check would resolve "the shipped reference for this artifact" by content,
	// not by a name pinned at authoring time.
	inline bool TryGetReferenceDigestFilePath(FString& OutPath, FString& OutReason)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S2_REFERENCE_DIGEST_PATH"));
		if (!Override.IsEmpty())
		{
			if (IFileManager::Get().FileExists(*Override))
			{
				OutPath = Override;
				return true;
			}
			OutReason = FString::Printf(TEXT("SUPERSLM_L2S2_REFERENCE_DIGEST_PATH set to '%s', but no file exists there."), *Override);
			return false;
		}

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		if (!Plugin.IsValid())
		{
			OutReason = TEXT("SuperSLMUnreal plugin not found by IPluginManager -- cannot resolve Resources/SelfCheck/.");
			return false;
		}
		const FString SelfCheckDir = Plugin->GetBaseDir() / TEXT("Resources/SelfCheck");
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(SelfCheckDir / TEXT("*.reference_digests.json")), true, false);
		if (Found.Num() == 0)
		{
			OutReason = FString::Printf(TEXT(
				"No '*.reference_digests.json' found under '%s'. Set "
				"SUPERSLM_L2S2_REFERENCE_DIGEST_PATH, or restore the plugin's shipped file, "
				"Resources/SelfCheck/<the example model's header hash>.reference_digests.json."), *SelfCheckDir);
			return false;
		}
		OutPath = SelfCheckDir / Found[0];
		return true;
	}

	// --- Subsystem access ---

	// T-2826 build log §5.2 (read at source, confirmed against T-2815 build log D1 /
	// Engine/Public/Tests/AutomationCommon.cpp's own FTestWorldWrapper::CreateTestWorld):
	// a headless test run (UnrealEditor-Cmd with -ExecCmds="Automation RunTests ...") has no PIE
	// and no game instance, so
	// scanning GEngine->GetWorldContexts() for a world whose OwningGameInstance is
	// non-null never finds one -- a UGameInstanceSubsystem is unreachable that way.
	// Every RunTest() now creates its own FTestWorldWrapper of EWorldType::Game (the one
	// case that constructs and Init()s a UGameInstance and assigns it to the new world
	// context's OwningGameInstance, exactly matching SuperSLML2S1Fixtures.h's own fixed
	// GetSubsystem(UWorld*) shape) and passes that world in explicitly, rather than
	// re-scanning, so a test's own wrapper is unambiguously the one it just created.
	//
	// Both accessors are DUPLICATED here rather than taken from
	// Fixtures/SuperSLML2S1Fixtures.h (this file's own header note above explains why
	// the two fixtures are not coupled): USuperSLMSubsystem's own accessor is needed by
	// the L2-S2 test files that tokenize or cross-compare against the CPU backend
	// (FrameContract, SchemaSaveRestore, SelfCheckCommissioning).
	inline USuperSLMSubsystem* GetSubsystem(UWorld* World)
	{
		if (!World || !World->GetGameInstance())
		{
			return nullptr;
		}
		return World->GetGameInstance()->GetSubsystem<USuperSLMSubsystem>();
	}

	inline USuperSLMGpuSubsystem* GetGpuSubsystem(UWorld* World)
	{
		if (!World || !World->GetGameInstance())
		{
			return nullptr;
		}
		return World->GetGameInstance()->GetSubsystem<USuperSLMGpuSubsystem>();
	}

	// Round 2 of the GPU apply ruling (plan §2.5 row 21, finding 6): a loop bounded only by a wall
	// deadline sleeps 1 ms instead of ticking while the next Tick() would be gated on the device, so
	// it does not spin tens of thousands of ticks a second for the device's whole run.
	inline bool NextTickGatedOnDevice(const USuperSLMGpuSubsystem& Gpu)
	{
#if WITH_DEV_AUTOMATION_TESTS
		return FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu);
#else
		(void)Gpu;
		return false;
#endif
	}

	// --- The per-sequence software queue's own test-side polling helpers (T-2826 round 5,
	// 2026-09-19; D-SLM7457's ruling removed the GPU subsystem's synchronous SaveSequence()/
	// RestoreSequence()/ResetSequence()/BeginGeneration() forms -- every caller now goes through
	// Request*()/Get*Result(), which resolve at a later tick, never inside the call that queues
	// them). Each helper polls the same way RunGpuGenerationToCompletion() below already does --
	// Tick() in a loop until the handle stops reading Pending or the wall-clock deadline expires. ---
	//
	// PACED, not a tight spin (T-2826 build log §13.2, round-7 review finding, 2026-09-19):
	// a lifecycle-op entry (Reset/Save/Restore's own async Job) is polled NON-BLOCKINGLY by design
	// (PollSlotFront/PollRestoreFront, SuperSLMGpuSubsystem.cpp: `Entry.Async->Job->bDone.load()`,
	// never a wait) -- deliberately, so a real per-frame Tick() never stalls a frame on a GPU save.
	// Since the ruling of 2026-09-26 (plan §2.5 row 21) decode-token application does not block
	// either: ApplyDue() never waits, and leaves a due token whose job is not done for a later
	// tick, so RunGpuGenerationToCompletion()'s own loop below is bounded by its wall-clock
	// deadline rather than synchronized to submission-thread progress. A helper that polls ONLY a lifecycle-op result never touches
	// that decode/due-token path at all, so nothing in its own loop ever blocks on real progress --
	// a tight, unpaced Tick() loop can in principle exhaust its whole iteration budget in far less
	// real wall-clock time than the real submission-thread round trip needs, reading as the handle
	// never leaving Pending (confirmed as the root cause of
	// SuperSLML2S2LifetimeTests.cpp's own SameSequenceCollisionQueuesTest, diagnosed at source in
	// the build log cited above). A short REAL sleep between ticks -- matched to StepSeconds, the
	// same 1/60s a real 60 Hz frame both simulates and actually takes, rather than the config's own
	// TickBudgetMs (a deliberately generous UPPER BOUND these tests set to avoid false
	// hitch-positives, not a literal per-tick real-world duration -- sleeping the full
	// TickBudgetMs, typically 1000 ms, per iteration would make every caller of these helpers
	// impractically slow for no added synchronization value) -- gives the real submission thread
	// genuine OS-scheduled wall-clock time between polls, "like a game frame" actually would.
	inline ESuperSLMRestoreResult DriveLifecycleOpToResolution(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMLifecycleOpHandle& Handle, double MaxWallClockSeconds = 60.0)
	{
		if (!Handle.IsValid())
		{
			return ESuperSLMRestoreResult::Pending;
		}
		constexpr float StepSeconds = 1.0f / 60.0f;
		ESuperSLMRestoreResult Result = Subsystem.GetLifecycleOpResult(Handle);
		const double StartSeconds = FPlatformTime::Seconds();
		while (Result == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (NextTickGatedOnDevice(Subsystem))
			{
				FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
				continue;
			}
			Subsystem.Tick(StepSeconds);
			FPlatformProcess::Sleep(StepSeconds);
			Result = Subsystem.GetLifecycleOpResult(Handle);
		}
		return Result;
	}

	// Load-bearing for R-S2g (iv) (SuperSLM.U1.Gpu.CarriedTokensAtSavePaced) and (x): while the next
	// Tick() is gated on the device this helper sleeps instead of ticking (round 2, finding 6), so
	// the save resolves while produced tokens are still in flight rather than after a spin that
	// applies them. (iv)'s precondition (observed < produced at the save) and (x)'s (produced -
	// observed >= 10) depend on it; do not replace it with an unpaced tick loop.
	inline ESuperSLMRestoreResult DriveSaveToResolution(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMLifecycleOpHandle& Handle, TArray<uint8>& OutBlob, double MaxWallClockSeconds = 60.0)
	{
		if (!Handle.IsValid())
		{
			OutBlob.Reset();
			return ESuperSLMRestoreResult::Pending;
		}
		constexpr float StepSeconds = 1.0f / 60.0f;
		ESuperSLMRestoreResult Result = Subsystem.GetSaveResult(Handle, OutBlob);
		const double StartSeconds = FPlatformTime::Seconds();
		while (Result == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (NextTickGatedOnDevice(Subsystem))
			{
				FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
				continue;
			}
			Subsystem.Tick(StepSeconds);
			FPlatformProcess::Sleep(StepSeconds);
			Result = Subsystem.GetSaveResult(Handle, OutBlob);
		}
		return Result;
	}

	inline ESuperSLMRestoreResult DriveRestoreToResolution(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMLifecycleOpHandle& Handle, FSuperSLMGpuSequence& OutSequence, double MaxWallClockSeconds = 60.0)
	{
		if (!Handle.IsValid())
		{
			OutSequence = FSuperSLMGpuSequence();
			return ESuperSLMRestoreResult::Pending;
		}
		constexpr float StepSeconds = 1.0f / 60.0f;
		ESuperSLMRestoreResult Result = Subsystem.GetRestoreResult(Handle, OutSequence);
		const double StartSeconds = FPlatformTime::Seconds();
		while (Result == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (NextTickGatedOnDevice(Subsystem))
			{
				FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
				continue;
			}
			Subsystem.Tick(StepSeconds);
			FPlatformProcess::Sleep(StepSeconds);
			Result = Subsystem.GetRestoreResult(Handle, OutSequence);
		}
		return Result;
	}

	// The name of a lifecycle result, for failure messages.
	inline const TCHAR* RestoreResultName(ESuperSLMRestoreResult Result)
	{
		switch (Result)
		{
			case ESuperSLMRestoreResult::Success: return TEXT("Success");
			case ESuperSLMRestoreResult::BackendMismatch: return TEXT("BackendMismatch");
			case ESuperSLMRestoreResult::ModelMismatch: return TEXT("ModelMismatch");
			case ESuperSLMRestoreResult::KvMismatch: return TEXT("KvMismatch");
			case ESuperSLMRestoreResult::ResidualLost: return TEXT("ResidualLost");
			case ESuperSLMRestoreResult::Malformed: return TEXT("Malformed");
			case ESuperSLMRestoreResult::NotConfigured: return TEXT("NotConfigured");
			case ESuperSLMRestoreResult::PoolExhausted: return TEXT("PoolExhausted");
			case ESuperSLMRestoreResult::AdapterUnavailable: return TEXT("AdapterUnavailable");
			case ESuperSLMRestoreResult::SequenceQueueFull: return TEXT("SequenceQueueFull");
			case ESuperSLMRestoreResult::Pending: return TEXT("Pending");
			case ESuperSLMRestoreResult::UnsupportedOnGpu: return TEXT("UnsupportedOnGpu");
			case ESuperSLMRestoreResult::SaveRefused: return TEXT("SaveRefused");
			case ESuperSLMRestoreResult::Layer1Mismatch: return TEXT("Layer1Mismatch");
			case ESuperSLMRestoreResult::Consumed: return TEXT("Consumed");
			case ESuperSLMRestoreResult::ResetRequired: return TEXT("ResetRequired");
			case ESuperSLMRestoreResult::OutOfMemory: return TEXT("OutOfMemory");
		}
		return TEXT("an unnamed result");
	}

	// Drives RequestBeginGeneration() to Complete/Faulted via Tick() at a fixed 60 Hz step,
	// the GPU-subsystem twin of SuperSLML2S1Fixtures::RunGenerationToCompletion() --
	// every R-S2 cell that runs a real GPU generation to completion shares this one
	// loop.
	//
	// The request's handle decides when the phase may be read. While a reset queued ahead of the
	// generation runs, the sequence keeps its old phase and tokens, Complete included, so a loop
	// that returned on the first Complete it read would hand back the previous generation's tokens
	// as this one's. The phase is therefore read only once the handle has resolved Success (the
	// generation has started); a handle that resolves anything else ends the call with false,
	// naming the result -- ResetRequired when a reset queued ahead of it was refused.
	//
	// bAlreadyInProgress (default false): a RESTORED sequence handle is already past
	// Idle (mid-generation) -- a generation is accepted only when the sequence will be Idle at
	// its turn (SuperSLMGpuSubsystem.h, D-SLM7946), so requesting one on a restored handle is
	// refused at the call. Pass true to skip straight to the
	// Tick() loop, matching the CPU L2-S1 reference test's own established pattern
	// (SuperSLML2S1SaveRestoreTests.cpp's ContinuesExactly test drives Tick() directly
	// on a restored handle, never BeginGeneration() again). There is no handle then, so the
	// phase is read from the first tick: the caller has already seen the generation start.
	// OutTokens then reflects only
	// the tokens generated SINCE the point this call started driving the sequence, not
	// any generated before a save -- the same convention that CPU reference test uses
	// (its own ContinuedTokens = TokensBeforeSave, then appends the restored handle's
	// own GetGeneratedTokens()); a caller resuming a partially-generated restored
	// sequence concatenates its own pre-save tokens itself.
	inline bool RunGpuGenerationToCompletion(
		USuperSLMGpuSubsystem& Subsystem,
		const FSuperSLMGpuSequence& Seq,
		const FSuperSLMGenerationRequest& Request,
		TArray<int32>& OutTokens,
		double MaxWallClockSeconds,
		FString& OutError,
		bool bAlreadyInProgress = false)
	{
		FSuperSLMLifecycleOpHandle Handle;
		if (!bAlreadyInProgress)
		{
			Handle = Subsystem.RequestBeginGeneration(Seq, Request);
			if (!Handle.IsValid())
			{
				OutError = FString::Printf(TEXT("RequestBeginGeneration failed: %s"), *Subsystem.GetLastLifecycleRequestError());
				return false;
			}
		}

		constexpr float StepSeconds = 1.0f / 60.0f;
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (NextTickGatedOnDevice(Subsystem))
			{
				FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
				continue;
			}
			Subsystem.Tick(StepSeconds);
			if (Handle.IsValid())
			{
				const ESuperSLMRestoreResult Started = Subsystem.GetLifecycleOpResult(Handle);
				if (Started == ESuperSLMRestoreResult::Pending)
				{
					continue; // not started: the phase and tokens are still the sequence's previous ones
				}
				if (Started != ESuperSLMRestoreResult::Success)
				{
					OutError = FString::Printf(TEXT("RequestBeginGeneration resolved %s, not Success: %s"),
						RestoreResultName(Started), *Subsystem.GetLastLifecycleRequestError());
					return false;
				}
			}
			const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Seq);
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
		}
		OutError = FString::Printf(TEXT("did not reach Complete/Faulted within %.0f seconds"), MaxWallClockSeconds);
		return false;
	}

	// Layers-per-slice, from a dispatch budget, matching
	// superslm_gpu::PlanDispatchBudgetGpu's own floor-division contract (gpu_port.h,
	// read at v1.5.0) -- used by test code to derive DispatchBudget values from a
	// desired layers-per-slice count rather than hand-computing the multiply at every
	// call site.
	inline uint32 DispatchBudgetForLayersPerSlice(int32 LayersPerSlice)
	{
		constexpr uint32 kLegacyDispatchesPerLayer = 24; // superslm_gpu::kLegacyDispatchesPerLayer, gpu_port.h
		return static_cast<uint32>(LayersPerSlice) * kLegacyDispatchesPerLayer;
	}

#if WITH_DEV_AUTOMATION_TESTS
	// --- Pacing for fixed-count GPU tick loops (plan §2.5 row 21, "Ruling 2026-09-26": the GPU
	// Tick() never waits on the submission thread). A loop that positions or drains by tick count
	// relied on that wait to keep it in step with the device; it now ticks through the test
	// access's TickWhenDeviceReady(), which sleeps 1 ms while the next tick would be gated on the
	// device and then ticks once. It returns false, without ticking, on timeout. Every call to the
	// test access's pacing goes through here, so a signature change is reconciled in one place
	// (the test record §1). ---
	constexpr float kPacedTickSeconds = 1.0f / 60.0f;
	constexpr double kPacedTickTimeoutSeconds = 60.0;

	// One paced tick; false when the device did not become ready within the timeout.
	inline bool TickWhenDeviceReady(USuperSLMGpuSubsystem& Gpu)
	{
		return FSuperSLMGpuTestAccess::TickWhenDeviceReady(Gpu, kPacedTickSeconds, kPacedTickTimeoutSeconds);
	}

	// One paced tick with its return asserted; Where names the loop in the failure message.
	inline bool PacedTick(FAutomationTestBase& Test, USuperSLMGpuSubsystem& Gpu, const TCHAR* Where)
	{
		return Test.TestTrue(FString::Printf(TEXT("TickWhenDeviceReady() ticks within %.0f s (%s)"), kPacedTickTimeoutSeconds, Where),
			TickWhenDeviceReady(Gpu));
	}
#endif
}

