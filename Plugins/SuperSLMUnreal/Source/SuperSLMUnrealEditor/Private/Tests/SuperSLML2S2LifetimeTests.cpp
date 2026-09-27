// T-2816 -- L2-S2 red suite. Plan cell R-S2d (the plan
// §9): "GPU lifetime is clean across long play and shutdown" -- dims 1, 3.
// A-EX: 1,000 vend/return cycles (each a 48-token generation, matching §10.3's own
// costing arithmetic: "1,000 cycles of 48 tokens is 48,000 tokens") with a GPU
// save/restore issued during another sequence's decode -> the restore completes without
// surfacing SSLM_BUSY, VRAM is flat, measured LIVE (D-SLM7335, answering Q_R-S2d of
// the red-suite record §8), and shutdown follows the
// enforced order (unbind -> adapter unmap -> model unmap -> context destroy) with no
// refusal.
//
// VRAM-flat evidence (D-SLM7335): local-segment CurrentUsage
// (USuperSLMGpuSubsystem::GetLocalVideoMemoryUsageBytes(), DXGI
// IDXGIAdapter3::QueryVideoMemoryInfo on the adapter Layer 1 uses) sampled after 10
// warm-up cycles and after cycle 1,000 must differ by less than one A-EX KV block
// (24 MiB = 25,165,824 bytes). The headless run is `-nullrhi`, so UE creates no D3D
// device and the reading is Layer 1's alone. A committed D3D12 resource is at least
// 64 KiB (the default placement alignment), so one resource leaked on every cycle grows
// at least 990 cycles * 64 KiB ~= 61.9 MiB over the run and fails the bound
// (arithmetic, D-SLM7335). The plugin's own declared-residency ledger
// (GetDeclaredGpuResidencyBytes()) stays as a MECHANISM check only -- an
// internal-consistency check on the plugin's own arithmetic, never the evidence for
// this claim (a reference must not share its inputs with the thing it grades).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Set.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	bool SetUpGpu(FAutomationTestBase& T, UWorld* World, int32 BlockCount, USuperSLMModel*& OutModel, USuperSLMGpuSubsystem*& OutGpu)
	{
		FString AExPath, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return false;
		}
		OutGpu = GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), OutGpu))
		{
			return false;
		}
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = 4096;
		Config.BlockCount = BlockCount;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(OutModel, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2LifetimeThousandCyclesTest,
	"SuperSLM.L2S2.Lifetime.ThousandVendReturnCyclesWithConcurrentSaveRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2LifetimeThousandCyclesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	// BlockCount=2: one long-running background sequence occupies one slot for the
	// whole run; the churn sequence occupies the other, cycling 1,000 times.
	if (!SetUpGpu(*this, World, /*BlockCount*/ 2, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGpuSequence LongRunningSeq;
	if (!TestEqual(TEXT("VendSequence() for the long-running background sequence"),
			(uint8)Gpu->VendSequence(LongRunningSeq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	{
		FSuperSLMGenerationRequest LongRequest;
		LongRequest.PromptTokens = {1, 2, 3, 4, 5};
		LongRequest.MaxNewTokens = 2000; // mid-decode through the concurrent-restore cycle by
			// construction, with >2x the schedule's own worst-case margin -- see
			// kSaveRestoreAtCycle's own derivation below, which sizes the restore's cycle index
			// against this budget and the tick schedule, not the other way around. Not sized to
			// survive the full 1,000-cycle loop -- the guard below only checks the moment of the
			// concurrent restore, and the artifact's own 4,096-token ContextCap bounds any single
			// sequence's total tokens regardless of MaxNewTokens (Fixtures/SuperSLML2S2Fixtures.h).
		LongRequest.SpanKind = ESuperSLMSpanKind::Prompt;
		const FSuperSLMLifecycleOpHandle LongBeginHandle = Gpu->RequestBeginGeneration(LongRunningSeq, LongRequest);
		if (!TestTrue(*FString::Printf(TEXT("long-running RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()),
				LongBeginHandle.IsValid()))
		{
			return false;
		}
	}

	// Mechanism check only (D-SLM7335) -- not this cell's own evidence.
	const int64 ResidencyAtStart = Gpu->GetDeclaredGpuResidencyBytes();

	constexpr int32 kCycleCount = 1000;
	constexpr int32 kWarmUpCycles = 10;
	constexpr int64 kOneAExKvBlockBytes = 24LL * 1024 * 1024; // one A-EX KV block, D-SLM7335's own bound

	// Sized from the schedule, not timing (a timing-sized bound fails on a slower machine). Root cause (a full-suite
	// run on 2026-09-19 failed here; derivation at the red-suite record §14): this constant sat at kCycleCount / 2
	// (cycle 500), which is provably too late for
	// LongRunningSeq's 2,000-token budget above. Each ORDINARY churn cycle spends up to
	// kChurnTicksPerOrdinaryCycle ticks: the explicit Tick() above (1) + PromptTokens.Num() (3) +
	// MaxNewTokens (48) = 52. Churn is the pool's only OneCall-path sequence, so PlanAndIssue()'s
	// OneCallDue lane serves it exactly one whole token per tick with no rotation wait (RunOneCallToken()
	// resolves a full token inside the same tick it is planned, SuperSLMGpuSubsystem.cpp), and
	// RunGpuGenerationToCompletion() below calls Tick() in a loop until Churn reaches Complete, not
	// just once per cycle. LongRunningSeq is Composed, and PlanAndIssue() services both lanes inside
	// the SAME Tick() call, so LongRunningSeq's composed decode advances on every one of those ticks
	// too: AExNumHiddenLayers (24) layers per token (prompt or decode -- both are composed-sliced
	// identically), LayersPerTick = Config.DispatchBudget / kLegacyDispatchesPerLayer = 96 / 24 = 4
	// layers per tick (SetUpGpu's own Config.DispatchBudget above, gpu_port.h's
	// DispatchesPerLayer(/*bHasQkNorm*/false) == kLegacyDispatchesPerLayer == 24) = exactly 6 ticks
	// per LongRunningSeq token. At the old cycle 500, the worst case is
	// 500 * kChurnTicksPerOrdinaryCycle + 1 (cycle 500's own explicit Tick()) + 20 (its decode-before-save
	// loop) = 26,021 ticks elapsed by the guard check below -- 26,021 / 6 ~= 4,336 tokens of composed
	// progress, which exceeds both LongRunningSeq's 2,005-token (prompt + MaxNewTokens) budget and the
	// artifact's own 4,096-token ContextCap (Fixtures/SuperSLML2S2Fixtures.h) -- LongRunningSeq had
	// already reached Complete (budget exhausted well before the cap) long before cycle 500, which is
	// why the guard below fired. At cycle 100: the same worst case is
	// 100 * 52 + 1 + 20 = 5,221 ticks, 5,221 / 6 ~= 870 tokens of composed progress (865 decode tokens)
	// against the 2,005-token budget -- under half of it (>2x margin), and under a quarter of the
	// artifact's ContextCap, so LongRunningSeq is deterministically Prefilling/Decoding at the restore.
	constexpr int32 kChurnPromptTokens = 3;
	constexpr int32 kChurnMaxNewTokens = 48;
	constexpr int32 kChurnTicksPerOrdinaryCycle = 1 + kChurnPromptTokens + kChurnMaxNewTokens; // 52
	constexpr int32 kSaveRestoreAtCycle = 100;
	static_assert(kSaveRestoreAtCycle < kCycleCount, "the concurrent restore must land inside the run");
	static_assert(
		(int64(kSaveRestoreAtCycle) * kChurnTicksPerOrdinaryCycle + 21) / 6 * 2 < 5 + 2000,
		"kSaveRestoreAtCycle must leave LongRunningSeq's schedule-derived worst-case token progress "
		"under half of its (prompt + MaxNewTokens) budget above -- re-derive both together if either "
		"changes (the red-suite record §14)");
	int32 LedgerFailures = 0;
	bool bSaveRestoreObservedNoBusy = false;
	bool bLongRunningWasMidDecodeAtRestore = false;
	int64 VramAtWarmUp = 0;
	int64 VramAtEnd = 0;
	bool bObservedBelowProducedOnSave = false;

	for (int32 Cycle = 0; Cycle < kCycleCount; ++Cycle)
	{
		// Advance the long-running sequence one slice per cycle, so it is genuinely
		// mid-flight (Submitted or mid-token) around the save/restore cycle below.
		Gpu->Tick(1.0f / 60.0f);

		FSuperSLMGpuSequence Churn;
		if (Gpu->VendSequence(Churn, ESuperSLMGpuDecodePath::OneCall) != ESuperSLMGpuVendResult::Success)
		{
			++LedgerFailures;
			continue;
		}

		if (Cycle != kSaveRestoreAtCycle)
		{
			// The ordinary cycle: a real 48-token generation (matching plan §10.3's
			// own costing text), then return. PromptTokens.Num() (3) and MaxNewTokens
			// (kChurnMaxNewTokens, wired rather than a second literal 48) are the two
			// components of kChurnTicksPerOrdinaryCycle above -- kSaveRestoreAtCycle's own
			// derivation is only as sound as this Request actually matching it.
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = {1, 2, 3}; // kChurnPromptTokens above
			Request.MaxNewTokens = kChurnMaxNewTokens;
			Request.SpanKind = ESuperSLMSpanKind::Prompt;
			TArray<int32> Tokens;
			FString RunError;
			const bool bChurnCompleted = RunGpuGenerationToCompletion(*Gpu, Churn, Request, Tokens, /*MaxWallClockSeconds*/ 60.0, RunError);
			Gpu->ReturnSequence(Churn);
			if (!TestTrue(*FString::Printf(TEXT("churn generation must complete: %s"), *RunError), bChurnCompleted))
			{
				return false;
			}
		}
		else
		{
			// The one cycle this cell exists for: a GPU save/restore issued while
			// LongRunningSeq is genuinely mid-flight (§5: "Restore is refused while any
			// sequence in the process is Submitted, so the submission thread issues
			// restores at the start of a tick"). Decode a few tokens on Churn first so
			// the save carries a real, partway KV state, matching R-S1c's own
			// "save-at-20/restore/continue" shape.
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = {1, 2, 3};
			Request.MaxNewTokens = 20;
			Request.SpanKind = ESuperSLMSpanKind::Prompt;
			Gpu->RequestBeginGeneration(Churn, Request);
			for (int32 T = 0; T < 20; ++T)
			{
				Gpu->Tick(1.0f / 60.0f);
				FPlatformProcess::Sleep(1.0f / 60.0f);
			}

			TArray<uint8> Blob;
			const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Churn);
			const bool bSaveOk = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
			TestTrue(*FString::Printf(TEXT("RequestSaveSequence() during a concurrent decode must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), bSaveOk);
			const int32 ObservedAtSave = Gpu->GetGeneratedTokens(Churn).Num();
			int32 SavedObserved = -1, SavedProduced = -1;
			const bool bCountsAvailable = Gpu->GetSaveTokenCounts(SaveHandle, SavedObserved, SavedProduced);
			TestTrue(TEXT("churn save exposes observed and produced counts"), bCountsAvailable);
			TestEqual(TEXT("churn saved observed count equals delivered prefix"), SavedObserved, ObservedAtSave);
			bObservedBelowProducedOnSave |= bCountsAvailable && SavedObserved < SavedProduced;

			// Return Churn's own slot BEFORE restoring into a new handle -- the
			// established return-before-restore pattern
			// (SuperSLML2S2SchemaSaveRestoreTests.cpp's UnadvancedReboundAfterRestore
			// arm). At BlockCount=2, LongRunningSeq alone occupies the other slot; a
			// restore attempted while BOTH slots are still vended correctly returns
			// PoolExhausted (a different, already-covered contract), not the
			// BUSY-leak claim this cycle exists to prove. Freeing Churn's slot here is
			// what makes a slot genuinely available for the restore below.
			Gpu->ReturnSequence(Churn);

			// The claim requires LongRunningSeq to be genuinely mid-flight (Prefilling
			// or Decoding, never Idle/Complete/Faulted) at the moment of the restore
			// below -- asserted directly, rather than assumed from MaxNewTokens=2000's
			// own arithmetic, so a future change to per-tick pacing that lets
			// LongRunningSeq finish early fails HERE with a named reason instead of
			// silently testing an idle pool.
			const ESuperSLMSequencePhase LongRunningPhaseBeforeRestore = Gpu->GetPhase(LongRunningSeq);
			bLongRunningWasMidDecodeAtRestore = TestTrue(TEXT("LongRunningSeq must still be genuinely mid-decode (Prefilling or Decoding) at the moment of the concurrent restore -- otherwise this cycle is not exercising the claim under test"),
				LongRunningPhaseBeforeRestore == ESuperSLMSequencePhase::Prefilling || LongRunningPhaseBeforeRestore == ESuperSLMSequencePhase::Decoding);

			FSuperSLMGpuSequence Restored;
			const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
			const ESuperSLMRestoreResult RestoreResult = DriveRestoreToResolution(*Gpu, RestoreHandle, Restored);
			// The claim under test: the restore COMPLETES -- it never surfaces
			// SSLM_BUSY to the caller, because the plugin absorbs the
			// Submitted-elsewhere precondition internally by tick-queueing the
			// restore (§5). ESuperSLMRestoreResult (SuperSLMSaveRestoreTypes.h) has no
			// "Busy" case at all -- a caller-visible BUSY would have to surface as
			// Malformed or a similar mis-mapped case, which this assertion also
			// catches by requiring exactly Success (and distinguishes it from
			// PoolExhausted, the disposition this same call would correctly return
			// if the free-slot precondition above were not held).
			bSaveRestoreObservedNoBusy = TestEqual(TEXT("RequestRestoreSequence() during a concurrent decode must return Success, never surface BUSY"),
				(uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success);

			// Continue the restored sequence to completion, then return it. Churn's own
			// slot was already returned above. THIS RESULT IS THE CLAIM (T-2885 finding 1,
			// the code review record): a prior round called
			// RunGpuGenerationToCompletion() here and discarded its own bool and Tail, which
			// is why 73/73 green over that finding's own reachable path was possible at all --
			// this cycle is the ONE cell in the whole suite whose save lands with an EMPTY
			// PromptRemaining (finding 1's own precondition): Churn consumes all
			// kChurnPromptTokens (3) of its own prompt in the first 3 of the 20 ticks above, so
			// the save lands after the prompt is long gone. The exact number of decode tokens
			// already produced by save time is not fixed: results apply on the game
			// thread at T+K. The oracle counts whatever had been delivered at save
			// resolution, then requires that count plus the restored continuation equal
			// the exact budget of 20. A restore that fails to reinstate
			// TS.LastToken/MaxNewTokens/StopTokenIds/ContextUsed either stalls forever
			// (finding 1(a)'s own outcome) or faults immediately (finding 1(b)'s own outcome)
			// -- a correct restore reaches Complete with the exact delivered total.
			TArray<int32> Tail;
			FString TailError;
			const bool bTailCompleted = RunGpuGenerationToCompletion(*Gpu, Restored, Request, Tail, /*MaxWallClockSeconds*/ 60.0, TailError, /*bAlreadyInProgress*/ true);
			TestTrue(*FString::Printf(TEXT("the restored churn sequence must reach Complete, continuing past a save taken after its own prompt is exhausted (%s)"), *TailError), bTailCompleted);
			TestTrue(TEXT("the restored churn sequence produces a nonempty continuation"), Tail.Num() >= 1);
			TestEqual(TEXT("tokens delivered before save plus restored continuation equal the exact 20-token budget"),
				ObservedAtSave + Tail.Num(), 20);
			Gpu->ReturnSequence(Restored);
		}

		// Ledger self-consistency across every vend/return, the GPU-side counterpart
		// to R-S1e's own CPU pool-ledger cell (Fixtures/SuperSLML2S1Fixtures.h).
		if (Gpu->GetPoolFreeCount() + Gpu->GetPoolOccupiedCount() != 2)
		{
			++LedgerFailures;
		}

		// D-SLM7335's own two sample points: after 10 warm-up cycles (lets any
		// one-time allocation at Configure()/first-use settle out of the comparison,
		// so the bound measures per-cycle GROWTH, not startup cost) and after the
		// final cycle.
		if (Cycle == kWarmUpCycles - 1)
		{
			VramAtWarmUp = Gpu->GetLocalVideoMemoryUsageBytes();
			AddInfo(FString::Printf(TEXT("GetLocalVideoMemoryUsageBytes() after %d warm-up cycles = %lld bytes"), kWarmUpCycles, VramAtWarmUp));
		}
		if (Cycle == kCycleCount - 1)
		{
			VramAtEnd = Gpu->GetLocalVideoMemoryUsageBytes();
			AddInfo(FString::Printf(TEXT("GetLocalVideoMemoryUsageBytes() after cycle %d = %lld bytes"), kCycleCount, VramAtEnd));
		}
	}

	TestEqual(TEXT("pool ledger must stay self-consistent across all 1,000 cycles"), LedgerFailures, 0);
	TestTrue(TEXT("the concurrent save/restore cycle must have run and observed no BUSY leak"), bSaveRestoreObservedNoBusy);
	TestTrue(TEXT("churn's paced save resolves with observed tokens below produced tokens"), bObservedBelowProducedOnSave);

	// The LIVE evidence for this claim (D-SLM7335): growth between the warm-up sample
	// and the final sample must be under one A-EX KV block (24 MiB). Independent of the
	// plugin's own accounting below -- this is a real DXGI device-memory reading, so a
	// leak in Layer 1's own allocations or in a plugin GPU call the ledger does not
	// account for is visible HERE and would be invisible there.
	const int64 VramGrowthBytes = VramAtEnd - VramAtWarmUp;
	const bool bVramFlat = TestTrue(
		FString::Printf(TEXT("VRAM growth from cycle %d to cycle %d must be under one A-EX KV block (24 MiB): measured %lld bytes"),
			kWarmUpCycles, kCycleCount, VramGrowthBytes),
		VramGrowthBytes < kOneAExKvBlockBytes);

	// Mechanism check only (D-SLM7335) -- confirms the plugin's own accounting is
	// internally consistent across the run; NOT this cell's evidence for "VRAM is
	// flat" (that is bVramFlat above, the live DXGI reading).
	const int64 ResidencyAtEnd = Gpu->GetDeclaredGpuResidencyBytes();
	TestEqual(TEXT("declared GPU residency must stay internally consistent across all 1,000 cycles (mechanism check)"),
		ResidencyAtEnd, ResidencyAtStart);

	Gpu->ReturnSequence(LongRunningSeq);
	return LedgerFailures == 0 && bSaveRestoreObservedNoBusy && bObservedBelowProducedOnSave &&
		bLongRunningWasMidDecodeAtRestore && bVramFlat && ResidencyAtEnd == ResidencyAtStart;
}

// --- Shutdown with a live sequence: unbind -> adapter unmap -> model unmap -> context
// destroy, no Layer-1 lifecycle rejection, and a subsequent Configure() succeeds
// cleanly. Mirrors R-S1f.6's CPU-side shape (Fixtures/SuperSLML2S1Fixtures.h,
// SuperSLM.L2S1.Misuse.SubsystemShutdownWithLiveSequences). ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2ShutdownWithLiveSequenceTest,
	"SuperSLM.L2S2.Lifetime.ShutdownWithLiveSequenceThenCleanReconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2ShutdownWithLiveSequenceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGpuSequence LiveSeq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(LiveSeq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 200; // still generating, deliberately never returned/completed
	Gpu->RequestBeginGeneration(LiveSeq, Request);
	Gpu->Tick(1.0f / 60.0f);

	// Deinitialize() with LiveSeq still vended and mid-generation must complete without
	// crashing or triggering an assert inside Layer 1's own lifecycle guards
	// (SSLM_CONTEXT_HAS_LIVE_HANDLES / SSLM_MODEL_HAS_LIVE_ADAPTERS), because the
	// enforced order releases every handle before destroying the context (plan §9
	// R-S2d). This test reaching its next line (rather than crashing the test runner)
	// IS the structural assertion; a subsequent clean Configure() is the second, more
	// direct one.
	Gpu->Deinitialize();

	FSuperSLMGpuRuntimeConfig ReconfigAfterShutdown;
	ReconfigAfterShutdown.ContextCap = 4096;
	ReconfigAfterShutdown.BlockCount = 1;
	ReconfigAfterShutdown.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	ReconfigAfterShutdown.K = AExNumHiddenLayers;
	ReconfigAfterShutdown.TickBudgetMs = 1000.0;
	const FSuperSLMGpuConfigureReport Report = Gpu->Configure(Model, ReconfigAfterShutdown);

	return TestEqual(TEXT("a fresh Configure() after Deinitialize() with a live sequence must succeed cleanly"),
		(uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success);
}

// --- Same-sequence collision (accepted async-tick plan, T-2816 2026-09-19 round; plan §9
// R-S2d's own new arm, "mirrors R-S1j"; D-SLM7418/D-SLM7421/D-SLM7424/D-SLM7457; §5 GPU path /
// §10.3 item 8's per-sequence software queue) ---
//
// BUILT (T-2826 round 5, 2026-09-19): SUPERSLM_WITH_L2S2_QUEUE is retired (SuperSLMSlotGates.h,
// matching L2-S2's own top-level gate) -- USuperSLMGpuSubsystem's per-sequence software queue is
// implemented and this test compiles and runs unconditionally.
//
// SCOPE, and what this construction deliberately does NOT attempt (both routed, source-verified
// findings from the round-6 red-suite record, one now ruled):
//   1. R-S2d's own cell text lists AdoptPrefix() as one of "the first four ... accepted and
//      delivered" same-sequence requests. RULED (D-SLM7457 ruling 1): Layer 1 v1.5.0's GPU ABI
//      has no prefix-adopt verb at all, so AdoptPrefix() admits and resolves directly to
//      ESuperSLMRestoreResult::UnsupportedOnGpu, never reaching Layer 1. This test still uses
//      AdoptPrefix() ONLY as the construction's REFUSED fifth request -- a SSLM_SEQUENCE_QUEUE_
//      FULL refusal needs no real Layer-1 call, so this is the one position the plan's own
//      literal request list can be exercised honestly without a forcing construction. To still
//      prove FOUR real, accepted, arrival-ordered requests as R-S2d's own arithmetic shape
//      requires, this test configures its OWN MaxQueuedOperationsPerSequence = 3 (a smaller,
//      self-contained bound than the plan's shipped default of 4) rather than substituting a
//      second invented op for Adopt.
//   2. R-S2d's own cell text also names RestoreSequence() as one of the five same-sequence
//      requests ("issue ... RestoreSequence() ... against it"). RULED (D-SLM7457 ruling 2):
//      Restore creates a new sequence and has no target handle, so it cannot collide with a
//      busy sequence, and the plan's collision list drops it -- OMITTED from this construction,
//      matching the ruling exactly.
// Both omissions still leave every OTHER claim R-S2d's own text makes fully exercised below:
// arrival-order delivery of the accepted set, the full-queue capacity refusal by name, a retry
// after the queue drains by one succeeding, and no call ever surfacing Layer 1's own SSLM_BUSY.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SameSequenceCollisionQueuesTest,
	"SuperSLM.L2S2.Lifetime.SameSequenceCollisionQueues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SameSequenceCollisionQueuesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	// This test's OWN deliberately small bound (3), documented above -- not the plan's shipped
	// default (4). One physical slot: every request in this construction targets the SAME
	// sequence, which is never returned, so a second slot is never needed.
	constexpr int32 kMaxQueuedOperationsPerSequence = 3;
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = kMaxQueuedOperationsPerSequence;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	// A long enough MaxNewTokens that the sequence stays genuinely mid-flight (Prefilling or
	// Decoding, never Idle/Complete/Faulted) across every tick this test drives -- 24 layers / 4
	// per tick = 6 ticks per token (matching the ThousandVendReturnCycles test's own derivation
	// above), so 200 tokens is well over 1,000 ticks of headroom, far more than this test's own
	// bounded polling loop below needs.
	{
		FSuperSLMGenerationRequest FirstRequest;
		FirstRequest.PromptTokens = {1, 2, 3};
		FirstRequest.MaxNewTokens = 200;
		FirstRequest.SpanKind = ESuperSLMSpanKind::Prompt;
		const FSuperSLMLifecycleOpHandle FirstBeginHandle = Gpu->RequestBeginGeneration(Seq, FirstRequest);
		if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), FirstBeginHandle.IsValid()))
		{
			return false;
		}
	}
	// A few ticks to get the sequence genuinely into Prefilling/Decoding before the collision
	// requests below -- mirrors R-S2c's own "poll rather than read after one Tick()" discipline
	// (a prefill/embed result applies at T+K, never immediately).
	for (int32 T = 0; T < 4; ++T)
	{
		Gpu->Tick(1.0f / 60.0f);
	}
	{
		const ESuperSLMSequencePhase PhaseBeforeCollision = Gpu->GetPhase(Seq);
		if (!TestTrue(TEXT("Seq must be genuinely mid-flight (Prefilling or Decoding) before the collision requests -- otherwise this test is not exercising the claim under test"),
				PhaseBeforeCollision == ESuperSLMSequencePhase::Prefilling || PhaseBeforeCollision == ESuperSLMSequencePhase::Decoding))
		{
			return false;
		}
	}

	// The fixed arrival order (plan §9 R-S2d: "each from a distinct handle, in that fixed
	// arrival order", R-S1j's own wording, mirrored here). Three real, well-defined ops fill the
	// queue to its bound (3); the fourth request, AdoptPrefix(), is issued once the bound is
	// already reached and must be refused at once.
	const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(Seq);
	const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::Composed);
	FSuperSLMGenerationRequest SecondRequest;
	SecondRequest.PromptTokens = {4, 5};
	SecondRequest.MaxNewTokens = 32;
	SecondRequest.SpanKind = ESuperSLMSpanKind::Prompt;
	const FSuperSLMLifecycleOpHandle HSecondGen = Gpu->RequestBeginGeneration(Seq, SecondRequest);
	const FSuperSLMLifecycleOpHandle HAdopt = Gpu->RequestAdoptPrefix(Seq);

	bool bOk = true;
	bOk &= TestTrue(TEXT("RequestSaveSequence() (1st) must be accepted -- IsValid() handle"), HSave.IsValid());
	bOk &= TestTrue(TEXT("RequestResetSequence() (2nd) must be accepted -- IsValid() handle"), HReset.IsValid());
	bOk &= TestTrue(TEXT("RequestBeginGeneration() (3rd, the second decode/prefill request) must be accepted -- IsValid() handle"), HSecondGen.IsValid());
	bOk &= TestFalse(TEXT("RequestAdoptPrefix() (4th) must be REFUSED at once -- the queue is already at its configured bound (3) -- invalid handle"), HAdopt.IsValid());

	// The queue's own count right after issuing: 1 (Seq's own already-Submitted/in-flight job)
	// + 3 (Save, Reset, the second BeginGeneration, all admitted) = 4. AdoptPrefix contributes
	// nothing -- it was refused before anything was queued.
	bOk &= TestEqual(TEXT("GetPendingLifecycleOperationCount() must count the in-flight job plus all 3 admitted requests"),
		Gpu->GetPendingLifecycleOperationCount(Seq), 4);

	// Drain: poll every tick, recording the FIRST tick at which each admitted handle's own
	// result leaves Pending, AND (round 8, D-SLM7481 supersedes D-SLM7480) the resolution ordinal
	// GetLifecycleOpResolutionOrdinal() assigned at that same moment. Arrival order is asserted on
	// the ORDINALS, never the tick indices: T-2826 round 7 (D-SLM7480) briefly made a queued
	// BeginGeneration/AdoptPrefix front defer one extra tick past its own predecessor's
	// resolution specifically so tick-index inequalities here would hold; the maintainer's own
	// ruling (build log §14.4) rejected that fix as changing PRODUCT timing to suit this TEST's
	// own instrument, and reverted it (round 5's own admission-at-resolution shape is back) in
	// favor of making arrival order DIRECTLY observable. Two resolutions
	// landing in the same tick is legal and expected under the reverted code -- the ordinal still
	// strictly ascends in true resolution order regardless of which tick each lands on
	// (ResolveEntry(), SuperSLMGpuSubsystem.cpp, assigns it from one subsystem-wide counter the
	// instant a Result is published, so a same-tick collision still orders correctly: within one
	// AdmitQueuedOps() pass, Reset's own resolution and the newly-admitted SecondGen's own are
	// still two separate ResolveEntry() calls in arrival order). Asserting on tick indices here
	// again would fail exactly as an earlier maintainer run first caught (before D-SLM7480
	// existed) and would fail again now that D-SLM7480 is reverted.
	//
	// PACED, not a tight spin (T-2826 build log §13.2, round-7 review finding, 2026-09-19):
	// this loop polls only Save/Reset/BeginGeneration-admission results, never a decode token, and
	// nothing in Tick() blocks on real progress: since the ruling of 2026-09-26 (plan §2.5 row 21)
	// ApplyDue() never waits on a decode token either, so no loop here is synchronized to the
	// submission thread without a sleep. A lifecycle-op entry's
	// own async Job is polled NON-BLOCKINGLY by design (PollSlotFront, SuperSLMGpuSubsystem.cpp:
	// `Entry.Async->Job->bDone.load()`, never a wait), so an unpaced Tick() loop here can exhaust
	// its whole iteration budget in far less real wall-clock time than the real Save/Reset round
	// trip needs -- diagnosed at source as this test's own failure shape (the build log cited
	// above). A short REAL sleep between ticks, matched to StepSeconds (the same 1/60s a real 60 Hz
	// frame both simulates and actually takes -- the config's own TickBudgetMs is a deliberately
	// generous upper bound against false hitch-positives, not a literal per-tick real duration, so
	// sleeping the full budget here would only make this test impractically slow), gives the real
	// submission thread genuine OS-scheduled wall-clock time between polls.
	constexpr double kMaxWallClockSeconds = 60.0;
	constexpr float kDrainStepSeconds = 1.0f / 60.0f;
	int32 TickSaveResolved = -1, TickResetResolved = -1, TickSecondGenResolved = -1;
	int64 SaveOrdinal = -1, ResetOrdinal = -1, SecondGenOrdinal = -1;
	TArray<uint8> SaveBlob;
	ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
	const double DrainStartSeconds = FPlatformTime::Seconds();
	int32 T = 0;
	while (FPlatformTime::Seconds() - DrainStartSeconds < kMaxWallClockSeconds)
	{
		Gpu->Tick(kDrainStepSeconds);
		FPlatformProcess::Sleep(kDrainStepSeconds);

		if (TickSaveResolved < 0)
		{
			SaveResult = Gpu->GetSaveResult(HSave, SaveBlob);
			if (SaveResult != ESuperSLMRestoreResult::Pending)
			{
				TickSaveResolved = T;
				SaveOrdinal = Gpu->GetLifecycleOpResolutionOrdinal(HSave);
			}
		}
		if (TickResetResolved < 0 && Gpu->GetLifecycleOpResult(HReset) != ESuperSLMRestoreResult::Pending)
		{
			TickResetResolved = T;
			ResetOrdinal = Gpu->GetLifecycleOpResolutionOrdinal(HReset);
		}
		if (TickSecondGenResolved < 0 && Gpu->GetLifecycleOpResult(HSecondGen) != ESuperSLMRestoreResult::Pending)
		{
			TickSecondGenResolved = T;
			SecondGenOrdinal = Gpu->GetLifecycleOpResolutionOrdinal(HSecondGen);
		}
		if (TickSaveResolved >= 0 && TickResetResolved >= 0 && TickSecondGenResolved >= 0)
		{
			break;
		}
		++T;
	}

	bOk &= TestTrue(TEXT("RequestSaveSequence()'s own result must resolve within 60 seconds"), TickSaveResolved >= 0);
	bOk &= TestTrue(TEXT("save assembly resolves after the request tick"), TickSaveResolved > 0);
	bOk &= TestTrue(TEXT("RequestResetSequence()'s own result must resolve within 60 seconds"), TickResetResolved >= 0);
	bOk &= TestTrue(TEXT("RequestBeginGeneration()'s own admission result must resolve within 60 seconds"), TickSecondGenResolved >= 0);

	// Arrival order, asserted on the RESOLUTION ORDINAL (D-SLM7481), never the tick index a
	// resolution happened to be observed on -- two resolutions landing in the same polled tick is
	// legal (this cell's own header comment above); the ordinal still strictly ascends in true
	// resolution order regardless.
	bOk &= TestTrue(TEXT("Save's own resolution ordinal must be valid once its result has left Pending"), SaveOrdinal >= 0);
	bOk &= TestTrue(TEXT("Reset's own resolution ordinal must be valid once its result has left Pending"), ResetOrdinal >= 0);
	bOk &= TestTrue(TEXT("the second BeginGeneration's own resolution ordinal must be valid once its result has left Pending"), SecondGenOrdinal >= 0);
	if (TickSaveResolved >= 0 && TickResetResolved >= 0)
	{
		bOk &= TestTrue(TEXT("arrival order: Save (1st) must resolve strictly before Reset (2nd), by resolution ordinal"), SaveOrdinal < ResetOrdinal);
	}
	if (TickResetResolved >= 0 && TickSecondGenResolved >= 0)
	{
		bOk &= TestTrue(TEXT("arrival order: Reset (2nd) must resolve strictly before the second BeginGeneration (3rd), by resolution ordinal"), ResetOrdinal < SecondGenOrdinal);
	}

	// Never Layer 1's own BUSY -- ESuperSLMRestoreResult has no such case at all (a closed
	// enum, SuperSLMSaveRestoreTypes.h), so requiring EXACTLY Success (never Malformed or any
	// other mis-mapped case) is the concrete, checkable form of "never surfaces BUSY" (mirrors
	// R-S2d's own ThousandVendReturnCyclesWithConcurrentSaveRestore test above).
	if (TickSaveResolved >= 0)
	{
		bOk &= TestEqual(TEXT("RequestSaveSequence()'s own result must be exactly Success, never BUSY-shaped"),
			(uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(TEXT("a successful queued save must populate a non-empty blob"), SaveBlob.Num() > 0);
	}
	if (TickResetResolved >= 0)
	{
		bOk &= TestEqual(TEXT("RequestResetSequence()'s own result must be exactly Success, never BUSY-shaped"),
			(uint8)Gpu->GetLifecycleOpResult(HReset), (uint8)ESuperSLMRestoreResult::Success);
	}
	if (TickSecondGenResolved >= 0)
	{
		bOk &= TestEqual(TEXT("RequestBeginGeneration()'s own admission result must be exactly Success, never BUSY-shaped"),
			(uint8)Gpu->GetLifecycleOpResult(HSecondGen), (uint8)ESuperSLMRestoreResult::Success);
	}

	// A retry after the queue drains by one succeeds (plan §9 R-S1j's own wording, mirrored for
	// the GPU arm): once Save (the 1st, oldest-queued entry) has resolved, the queue has room
	// again -- a fresh request issued right after must be accepted.
	if (TickSaveResolved >= 0)
	{
		const FSuperSLMLifecycleOpHandle HRetry = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::Composed);
		bOk &= TestTrue(TEXT("a retry issued after the queue has drained by one must be accepted"), HRetry.IsValid());
	}

	Gpu->ReturnSequence(Seq);
	return bOk;
}

// =============================================================================================
// Round-7 pins (T-2816, 2026-09-19): production behaviour T-2826's round-5 build (D-SLM7457,
// D-SLM7465-D-SLM7468) added that no cell covered, named in the build log's own routed finding
// (the build record §12.8). Each runs against the real A-EX artifact
// (real artifact, real size, real input) and sets MaxQueuedOperationsPerSequence explicitly, matching every
// other cell in this file -- never relying on Configure()'s own shipped default.
// =============================================================================================

// --- §12.8 item 1: RequestAdoptPrefix() resolving to UnsupportedOnGpu, admitted for real. No
// cell before this one ever issues RequestAdoptPrefix() against a sequence with queue room
// available -- SameSequenceCollisionQueuesTest above uses it only as the queue-full-refused FIFTH
// request, so its own admitted, real resolution (as opposed to the request-time
// SequenceQueueFull refusal) was unpinned. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2AdoptPrefixResolvesUnsupportedOnGpuTest,
	"SuperSLM.L2S2.Lifetime.AdoptPrefixResolvesUnsupportedOnGpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2AdoptPrefixResolvesUnsupportedOnGpuTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 4;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	// Seq is fresh/Idle: its own queue is EMPTY, so this request has queue room available (unlike
	// SameSequenceCollisionQueuesTest's own use of AdoptPrefix, which issues it only once the
	// queue is already full). Admission is synchronous at request time when the target queue is
	// empty (D-SLM7467), and AdoptPrefix's own admission never forwards to Layer 1 (D-SLM7457
	// ruling 1, AdmitAdoptPrefixOp, SuperSLMGpuSubsystem.cpp) -- so the result is already resolved
	// by the time this call returns, with no Tick() needed at all.
	const FSuperSLMLifecycleOpHandle HAdopt = Gpu->RequestAdoptPrefix(Seq);
	bool bOk = true;
	bOk &= TestTrue(TEXT("RequestAdoptPrefix() against an idle sequence with queue room must be ADMITTED (not refused by capacity) -- valid handle"), HAdopt.IsValid());
	bOk &= TestEqual(TEXT("an admitted RequestAdoptPrefix() must resolve to UnsupportedOnGpu -- Layer 1 v1.5.0's GPU ABI has no prefix-adopt verb (D-SLM7457 ruling 1)"),
		(uint8)Gpu->GetLifecycleOpResult(HAdopt), (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);

	// Stable across a tick too -- not a transient value a later poll would overwrite or revert.
	Gpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("the resolved result must stay UnsupportedOnGpu after a tick, not revert or change"),
		(uint8)Gpu->GetLifecycleOpResult(HAdopt), (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);

	Gpu->ReturnSequence(Seq);
	return bOk;
}

// --- §12.8 item 2: cross-sequence global-ordinal admission order (D-SLM7428). Every existing
// cell that exercises the queue (SameSequenceCollisionQueuesTest above) issues every collision
// request against a SINGLE sequence, so only one sequence's queue is ever non-empty with more
// than one pending entry at a time -- no cell constructs TWO different sequences each with a
// simultaneously-eligible front-of-queue entry in the same tick.
//
// CONSTRUCTION, fully deterministic (no reliance on the submission thread's own real-time async
// completion): BeginGeneration's own admission resolves its op-log entry synchronously, the
// instant it is admitted (USuperSLMGpuSubsystem.h's own doc comment; confirmed at source,
// AdmitBeginGenerationOp, SuperSLMGpuSubsystem.cpp -- no async Job at all). Two sequences each
// given a still-unresolved BeginGeneration front (admitted at request time, before any Tick()),
// then a second AdoptPrefix queued behind each (also admission-synchronous once it is its own
// front, AdmitAdoptPrefixOp), have BOTH fronts advance past their (already-resolved) generation
// entry on the exact SAME first Tick() -- PollSlotFront runs once per slot per tick and advances
// OpFront the instant Result != Pending, which it already is here (AdmitQueuedOps(),
// SuperSLMGpuSubsystem.cpp). Both AdoptPrefix entries therefore become admission Candidates in
// the SAME tick's gathering pass, genuinely simultaneously eligible -- the exact precondition
// D-SLM7428 governs -- with NO dependency on background-thread timing.
//
// Ordinals are assigned OPPOSITE the sequences' own vend/slot order: SeqB (slot 1, vended
// second) issues its second request BEFORE SeqA (slot 0, vended first) does, so SeqB's
// AdoptPrefix carries the LOWER internal admission-time request ordinal (D-SLM7428's own
// GlobalOrdinal, the Candidates-sort key inside AdmitQueuedOps(), never itself exposed) despite
// the HIGHER slot index.
//
// UPGRADED, round 8 (D-SLM7481 supersedes D-SLM7480, build log §14.4): this cell originally could
// only prove the WEAKER "neither is starved to a later tick" floor, because nothing exposed
// admission ORDER directly -- the two op kinds' own results carry no shared, capacity-limited
// resource whose success/failure would reveal which admitted first (named here as a limitation in
// the round-7 version of this comment). GetLifecycleOpResolutionOrdinal() (D-SLM7481) closes that
// gap: ResolveEntry() (SuperSLMGpuSubsystem.cpp) assigns a strictly increasing, subsystem-wide
// ordinal the INSTANT a Result is published, and AdoptPrefix's own admission (AdmitAdoptPrefixOp)
// calls it synchronously as part of admitting -- so the Candidates loop admitting HAdoptB (lower
// request ordinal) before HAdoptA (higher) within the SAME AdmitQueuedOps() pass necessarily
// gives HAdoptB's entry the LOWER resolution ordinal too. Asserting
// GetLifecycleOpResolutionOrdinal(HAdoptB) < GetLifecycleOpResolutionOrdinal(HAdoptA) is now a
// DIRECT, decisive proof that the lower-request-ordinal, higher-slot-index candidate was admitted
// (and therefore resolved) strictly first -- exactly D-SLM7428's own claim, not merely its
// same-tick-starvation consequence. The same-tick assertions are kept alongside it: they prove a
// DIFFERENT thing (no cross-tick starvation) that a correct ordinal ordering alone does not by
// itself guarantee (a comparator could in principle order correctly while still processing only
// one candidate per tick across two ticks -- unlikely given AdmitQueuedOps()'s own single-pass
// structure, but not what the ordinal assertion checks).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2CrossSequenceGlobalOrdinalAdmissionOrderTest,
	"SuperSLM.L2S2.Lifetime.CrossSequenceGlobalOrdinalAdmissionOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2CrossSequenceGlobalOrdinalAdmissionOrderTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 2;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 4;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMGpuSequence SeqA, SeqB;
	if (!TestEqual(TEXT("VendSequence() SeqA (slot 0)"), (uint8)Gpu->VendSequence(SeqA, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success) ||
		!TestEqual(TEXT("VendSequence() SeqB (slot 1)"), (uint8)Gpu->VendSequence(SeqB, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	FSuperSLMGenerationRequest LongRequest;
	LongRequest.PromptTokens = {1, 2, 3};
	LongRequest.MaxNewTokens = 200;
	LongRequest.SpanKind = ESuperSLMSpanKind::Prompt;

	// Ordinal 1: SeqA's generation. Ordinal 2: SeqB's generation. Both admit synchronously
	// (queues were empty) and both resolve (Result=Success) at once -- OpFront has not advanced
	// past either yet, since that needs a Tick().
	const FSuperSLMLifecycleOpHandle HGenA = Gpu->RequestBeginGeneration(SeqA, LongRequest);
	const FSuperSLMLifecycleOpHandle HGenB = Gpu->RequestBeginGeneration(SeqB, LongRequest);
	bool bOk = true;
	bOk &= TestTrue(TEXT("RequestBeginGeneration(SeqA) must be admitted -- valid handle"), HGenA.IsValid());
	bOk &= TestTrue(TEXT("RequestBeginGeneration(SeqB) must be admitted -- valid handle"), HGenB.IsValid());

	// Ordinal 3: SeqB's second request, issued FIRST (lower ordinal despite the higher slot
	// index). Ordinal 4: SeqA's second request, issued SECOND (higher ordinal despite the lower
	// slot index) -- the deliberate reversal this cell's own header comment names.
	const FSuperSLMLifecycleOpHandle HAdoptB = Gpu->RequestAdoptPrefix(SeqB);
	const FSuperSLMLifecycleOpHandle HAdoptA = Gpu->RequestAdoptPrefix(SeqA);
	bOk &= TestTrue(TEXT("RequestAdoptPrefix(SeqB) (ordinal 3) must be admitted -- queue room available -- valid handle"), HAdoptB.IsValid());
	bOk &= TestTrue(TEXT("RequestAdoptPrefix(SeqA) (ordinal 4) must be admitted -- queue room available -- valid handle"), HAdoptA.IsValid());
	bOk &= TestEqual(TEXT("both AdoptPrefix requests must still read Pending before any Tick() -- neither sequence's queue was empty when it was issued"),
		(uint8)Gpu->GetLifecycleOpResult(HAdoptB), (uint8)ESuperSLMRestoreResult::Pending);

	// ONE Tick(): both generation entries resolve-and-advance in this same tick's PollSlotFront
	// pass (deterministic, per this cell's own header comment), exposing both AdoptPrefix entries
	// as simultaneously-eligible Candidates in the SAME admission pass.
	Gpu->Tick(1.0f / 60.0f);

	bOk &= TestEqual(TEXT("RequestAdoptPrefix(SeqB) (the lower-request-ordinal, higher-slot-index request) must resolve within this SAME tick, not a later one"),
		(uint8)Gpu->GetLifecycleOpResult(HAdoptB), (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);
	bOk &= TestEqual(TEXT("RequestAdoptPrefix(SeqA) (the higher-request-ordinal, lower-slot-index request) must ALSO resolve within this SAME tick, not be starved behind SeqB's"),
		(uint8)Gpu->GetLifecycleOpResult(HAdoptA), (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);

	// DECISIVE (D-SLM7481): the resolution ordinal directly proves admission ORDER, not merely
	// that both happened within the same tick. Both must be valid (>= 0) the instant their
	// results left Pending (already true above), and SeqB's own (lower request ordinal, higher
	// slot index) must be STRICTLY LOWER than SeqA's -- the Candidates loop inside this tick's
	// AdmitQueuedOps() admitted, and therefore resolved, SeqB's entry first.
	const int64 AdoptBOrdinal = Gpu->GetLifecycleOpResolutionOrdinal(HAdoptB);
	const int64 AdoptAOrdinal = Gpu->GetLifecycleOpResolutionOrdinal(HAdoptA);
	bOk &= TestTrue(TEXT("SeqB's own AdoptPrefix resolution ordinal must be valid (result already left Pending above)"), AdoptBOrdinal >= 0);
	bOk &= TestTrue(TEXT("SeqA's own AdoptPrefix resolution ordinal must be valid (result already left Pending above)"), AdoptAOrdinal >= 0);
	bOk &= TestTrue(TEXT("cross-sequence admission order: SeqB's AdoptPrefix (lower request ordinal, higher slot index) must resolve with a STRICTLY LOWER resolution ordinal than SeqA's (higher request ordinal, lower slot index) -- direct proof of D-SLM7428's arrival order, container/slot index never deciding it"),
		AdoptBOrdinal < AdoptAOrdinal);

	Gpu->ReturnSequence(SeqA);
	Gpu->ReturnSequence(SeqB);
	return bOk;
}

// --- §12.8 item 3: ReturnSequence() deferring while the sequence's queue is non-empty
// (D-SLM7468, bReturnPending/FinishDeferredReturn). Every existing and migrated test calls
// ReturnSequence() only once a sequence's own queue has already fully resolved -- no cell
// returns a sequence while an admitted-but-not-yet-fully-drained op is still outstanding, to
// observe the physical slot staying held until the log drains, then freeing.
//
// DECISIVE, deterministic observable: BlockCount = 1 (a single physical slot), so "is the slot
// still held" is directly checkable via a second VendSequence() call -- PoolExhausted proves the
// slot was NOT handed back yet; Success proves it was. No timing/tick-count assumption is needed
// for the first assertion below: ReturnSequence()'s own OpLog.Num() > OpFront check (read at
// source, SuperSLMGpuSubsystem.cpp) is evaluated synchronously, at the call itself, before any
// Tick(). The exact tick count for the remaining two assertions (still held after 1 tick, freed
// after 2) is hand-traced against PollSlotFront/AdmitQueuedOps's own end-of-pass sweep, read at
// source: tick 1 advances OpFront past the (already-resolved) generation entry AND admits-and-
// resolves the now-exposed AdoptPrefix entry in the same pass, but OpFront does not advance past
// THAT entry until the NEXT tick's own PollSlotFront call, which is also when the deferred-return
// sweep (`bReturnPending && OpFront >= OpLog.Num()`) first observes the log fully drained.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2ReturnSequenceDefersWhileQueueNonEmptyTest,
	"SuperSLM.L2S2.Lifetime.ReturnSequenceDefersWhileQueueNonEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2ReturnSequenceDefersWhileQueueNonEmptyTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	// A single physical slot, deliberately: makes "is the slot still held" directly observable
	// through a second VendSequence() call, with no other sequence around to confuse the reading.
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 4;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 200;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	const FSuperSLMLifecycleOpHandle HGen = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(TEXT("RequestBeginGeneration() must be admitted -- valid handle"), HGen.IsValid()))
	{
		return false;
	}

	// A SECOND op, queued (not yet admitted -- Seq's own front is still the just-admitted
	// generation entry): the outstanding entry ReturnSequence() below must see.
	const FSuperSLMLifecycleOpHandle HAdopt = Gpu->RequestAdoptPrefix(Seq);
	if (!TestTrue(TEXT("RequestAdoptPrefix() (2nd) must be admitted into the queue -- valid handle"), HAdopt.IsValid()))
	{
		return false;
	}

	// Return the sequence NOW, while its own queue still holds this unresolved 2nd entry.
	Gpu->ReturnSequence(Seq);

	// DECISIVE: the ONLY physical slot must NOT be handed to a new vend yet.
	FSuperSLMGpuSequence Reused;
	bool bOk = true;
	bOk &= TestEqual(TEXT("VendSequence() right after a deferred ReturnSequence() must find NO free slot -- the physical block is still held (D-SLM7468)"),
		(uint8)Gpu->VendSequence(Reused, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::PoolExhausted);

	// One tick: the generation entry (already resolved) advances OpFront, exposing AdoptPrefix as
	// the new front, which admits and resolves in this SAME tick -- but OpFront has not yet
	// advanced PAST it (that needs one more tick's own PollSlotFront pass), so the log is still
	// not fully drained.
	Gpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("VendSequence() after exactly one tick must still find no free slot -- the queue's last entry has resolved but the drain check has not yet observed it"),
		(uint8)Gpu->VendSequence(Reused, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::PoolExhausted);

	// A second tick: PollSlotFront now advances OpFront past the resolved AdoptPrefix entry,
	// OpFront reaches OpLog.Num(), and AdmitQueuedOps()'s own end-of-pass sweep
	// (S.bReturnPending && S.OpFront >= S.OpLog.Num()) finally runs FinishDeferredReturn().
	Gpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("VendSequence() must succeed once the deferred return's own queue has fully drained -- the slot is handed back"),
		(uint8)Gpu->VendSequence(Reused, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success);

	Gpu->ReturnSequence(Reused);
	return bOk;
}

// --- §12.8 item 4: Restore's own subsystem-level queue (D-SLM7465) -- two restores queued
// back-to-back, and a restore admitted while an UNRELATED sequence's own per-sequence op is
// independently in flight. Every migrated call site (build log §12.4) issues a single,
// uncontended restore -- no cell queues two restores back to back, or issues a restore while a
// per-sequence op on a DIFFERENT sequence is also outstanding.
//
// DECISIVE, deterministic observable for "two restores back-to-back": RestoreOps is a single
// FIFO (D-SLM7457 ruling 2 -- Restore has no per-sequence OpLog to join, so it queues at the
// subsystem level instead), so its own front-of-queue candidate is unique per tick, exactly like
// a per-sequence queue's own front -- no timing race is needed to prove STRICT arrival order
// between two restores in the SAME queue. What is new here is the POOL ACCOUNTING a restore's
// own admission reads (AdmitRestoreFront, SuperSLMGpuSubsystem.cpp): BlockCount = 2, one slot
// permanently held by SeqA's own long-running generation, one slot free. Restore #1 (admitted
// immediately, RestoreOps was empty) claims the one free slot for real. By the time Restore #2 is
// admitted (only once Restore #1 has fully resolved -- the single-FIFO guarantee above), BOTH
// slots are genuinely occupied (SeqA's own, plus the one Restore #1 just restored into), so
// Restore #2 MUST resolve PoolExhausted -- a real, mutation-provable consequence of the pool
// accounting, not merely "it happened to go second."
//
// What this cell does NOT attempt: forcing a restore and a per-sequence op to become
// simultaneously eligible in the exact SAME admission pass (CrossSequenceGlobalOrdinalAdmissionOrder,
// above, names why no per-sequence op's own admission touches an externally observable shared
// resource, which is exactly what would be needed to prove same-tick ordinal order decisively for
// that pairing too). Instead, this cell proves the two restores queue and drain against real,
// live per-sequence pool state (SeqA's own slot) throughout, and that SeqA's own generation is
// completely unaffected by the restore traffic sharing the same admission passes across the run
// -- named here rather than silently assumed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2RestoreQueueVsPerSequenceOpTest,
	"SuperSLM.L2S2.Lifetime.RestoreQueueVsPerSequenceOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2RestoreQueueVsPerSequenceOpTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 2;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 4;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	// A valid GPU blob to restore from -- saved off a throwaway sequence, then returned, so both
	// slots are free again before the real construction below starts.
	TArray<uint8> Blob;
	{
		FSuperSLMGpuSequence TempSeq;
		if (!TestEqual(TEXT("VendSequence() (temp, for the blob)"), (uint8)Gpu->VendSequence(TempSeq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(TempSeq);
		if (!TestEqual(TEXT("RequestSaveSequence() (temp) must resolve Success"), (uint8)DriveSaveToResolution(*Gpu, HSave, Blob), (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		Gpu->ReturnSequence(TempSeq);
	}
	if (!TestTrue(TEXT("the temp blob must be non-empty"), Blob.Num() > 0))
	{
		return false;
	}

	// SeqA: a long-running generation occupying ONE of the two slots for the rest of this test.
	FSuperSLMGpuSequence SeqA;
	if (!TestEqual(TEXT("VendSequence() SeqA"), (uint8)Gpu->VendSequence(SeqA, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest LongRequest;
	LongRequest.PromptTokens = {1, 2, 3};
	LongRequest.MaxNewTokens = 200;
	LongRequest.SpanKind = ESuperSLMSpanKind::Prompt;
	if (!TestTrue(TEXT("RequestBeginGeneration(SeqA) must be admitted"), Gpu->RequestBeginGeneration(SeqA, LongRequest).IsValid()))
	{
		return false;
	}

	// Restore #1: admits immediately (RestoreOps was empty), claims the ONE remaining free slot.
	const FSuperSLMLifecycleOpHandle HRestore1 = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestTrue(TEXT("RequestRestoreSequence() #1 must be admitted -- valid handle"), HRestore1.IsValid()))
	{
		return false;
	}
	// Restore #2: RestoreOps is now non-empty (Restore #1 Submitted) -- queues, strictly behind #1.
	const FSuperSLMLifecycleOpHandle HRestore2 = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestTrue(TEXT("RequestRestoreSequence() #2 must be admitted into the queue -- valid handle"), HRestore2.IsValid()))
	{
		return false;
	}

	FSuperSLMGpuSequence Restored1, Restored2;
	bool bOk = true;
	bOk &= TestEqual(TEXT("Restore #1 must resolve Success -- it had a genuinely free slot to restore into"),
		(uint8)DriveRestoreToResolution(*Gpu, HRestore1, Restored1), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestTrue(TEXT("Restore #1's own new sequence handle must be valid on Success"), Restored1.IsValid());

	bOk &= TestEqual(TEXT("Restore #2 must resolve PoolExhausted -- by the time it is admitted, SeqA's own slot AND Restore #1's newly-restored slot are both occupied"),
		(uint8)DriveRestoreToResolution(*Gpu, HRestore2, Restored2), (uint8)ESuperSLMRestoreResult::PoolExhausted);
	bOk &= TestFalse(TEXT("Restore #2's own OutSequence must stay invalid on a PoolExhausted resolution"), Restored2.IsValid());

	// SeqA's own generation, running the entire time the two restores queued and drained, must be
	// completely unaffected by them -- never Faulted, still progressing.
	const ESuperSLMSequencePhase SeqAPhase = Gpu->GetPhase(SeqA);
	bOk &= TestTrue(TEXT("SeqA must never have faulted while the restore queue admitted and drained around it"),
		SeqAPhase == ESuperSLMSequencePhase::Prefilling || SeqAPhase == ESuperSLMSequencePhase::Decoding || SeqAPhase == ESuperSLMSequencePhase::Complete);

	Gpu->ReturnSequence(SeqA);
	if (Restored1.IsValid())
	{
		Gpu->ReturnSequence(Restored1);
	}
	return bOk;
}

// --- §12.8 item 5: GetLastLifecycleRequestError()'s own "most recent event only, never
// historied" contract (D-SLM7466). Several migrated tests (build log §12.4) read this accessor
// for a SPECIFIC refusal each expects, but no cell asserts the documented contract directly: that
// a second, later refusal overwrites the text from an earlier one still being read, rather than
// accumulating, clearing, or leaving the earlier text in place.
//
// Both refusals below are REQUEST-TIME (synchronous, no Tick() needed for either), and of
// deliberately DIFFERENT kinds with genuinely different text, so the assertion is a real proof of
// overwrite rather than two calls that coincidentally read the same static string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2LastLifecycleRequestErrorMostRecentOnlyTest,
	"SuperSLM.L2S2.Lifetime.LastLifecycleRequestErrorMostRecentOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2LastLifecycleRequestErrorMostRecentOnlyTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}

	// A tight bound (1) makes the FIRST refusal trivial to trigger: one admitted request already
	// fills the sequence's own queue to capacity.
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 1;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	FSuperSLMGpuSequence SeqA;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(SeqA, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 200;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	if (!TestTrue(TEXT("RequestBeginGeneration(SeqA) must be admitted (fills the queue to its bound of 1)"), Gpu->RequestBeginGeneration(SeqA, Request).IsValid()))
	{
		return false;
	}

	// The FIRST (earlier) refusal: SeqA's own queue is already at its configured bound (1).
	const FSuperSLMLifecycleOpHandle HRefused1 = Gpu->RequestResetSequence(SeqA, ESuperSLMGpuDecodePath::Composed);
	bool bOk = true;
	bOk &= TestFalse(TEXT("RequestResetSequence() against a full queue must be refused -- invalid handle"), HRefused1.IsValid());
	const FString ErrorText1 = Gpu->GetLastLifecycleRequestError();
	bOk &= TestTrue(TEXT("the first refusal's own text must name the capacity bound"), ErrorText1.Contains(TEXT("configured bound")));

	// The SECOND (later) refusal, a DIFFERENT kind and against a different (never-vended, always-
	// invalid) sequence handle -- genuinely different text from the first.
	FSuperSLMGpuSequence NeverVended;
	const FSuperSLMLifecycleOpHandle HRefused2 = Gpu->RequestResetSequence(NeverVended, ESuperSLMGpuDecodePath::Composed);
	bOk &= TestFalse(TEXT("RequestResetSequence() against a never-vended handle must be refused -- invalid handle"), HRefused2.IsValid());
	const FString ErrorText2 = Gpu->GetLastLifecycleRequestError();
	bOk &= TestTrue(TEXT("the second refusal's own text must name a not-live handle, not the capacity bound"), ErrorText2.Contains(TEXT("not a live GPU sequence handle")));

	// The decisive proof of "most recent event only, not historied": the second refusal's text
	// REPLACED the first's -- reading the accessor now, after both events, returns ONLY the
	// second's text.
	bOk &= TestNotEqual(TEXT("the two refusals' own texts must genuinely differ (a real overwrite, not two calls reading the same static string)"), ErrorText1, ErrorText2);
	const FString ErrorTextReadAgain = Gpu->GetLastLifecycleRequestError();
	bOk &= TestEqual(TEXT("re-reading GetLastLifecycleRequestError() must still return the SECOND refusal's own text -- stable, not cleared or reverted"), ErrorTextReadAgain, ErrorText2);
	bOk &= TestFalse(TEXT("the current text must not still carry the first refusal's own wording -- the second event fully replaced it, not appended to it"), ErrorTextReadAgain.Contains(TEXT("configured bound")));

	Gpu->ReturnSequence(SeqA);
	return bOk;
}

// =============================================================================================
// T-2816 round 9 (2026-09-19): T-2885's blind code review (the code review
// record, finding 1) found that restore never reinstates
// FThreadSlot::LastToken/MaxNewTokens/StopTokenIds/ContextUsed on the submission thread --
// invisible to every cell that existed in this suite before this round, because every one of
// them saves MID-PREFILL, where the next post-restore action carries bFirstPrompt and re-runs
// BeginPrompt(), which re-establishes exactly the state restore itself should have written.
// The two cells below save with an EMPTY PromptRemaining instead -- the finding's own stated
// precondition -- reaching its two named, reachable outcomes directly, each compared against an
// INDEPENDENTLY RUN unsaved reference of the identical request (never a hard-coded token count),
// per the review's own remedy. Both are OneCall-path, deliberately small and free of the
// concurrency/timing machinery the 1,000-cycle test above carries, so each is the cheapest
// construction that reaches its own outcome.
// =============================================================================================

// --- (a) Saved PRIMED: the last prompt token has just finished (TS.bPrimed == true) and no
// decode token has been produced yet. Finding 1's own reachable outcome: the restored
// TS.MaxNewTokens defaults to 0, so RecordToken()'s `TS.Produced (1) >= TS.MaxNewTokens (0)`
// marks the sequence bStopped on the submission thread immediately after its first --
// correctly produced -- token, and no action for it ever runs again: GetPhase() reads Decoding
// forever, never Complete, with no fault and nothing on any diagnostic accessor to say why. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2LifetimeSaveAfterPromptExhaustedPrimedTest,
	"SuperSLM.L2S2.Lifetime.SaveRestoreAfterPromptExhaustedPrimedMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2LifetimeSaveAfterPromptExhaustedPrimedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 6;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// --- Unsaved reference: the identical request, run start to finish, no save/restore. ---
	TArray<int32> ReferenceTokens;
	{
		FSuperSLMGpuSequence RefSeq;
		if (!TestEqual(TEXT("VendSequence() (reference)"), (uint8)Gpu->VendSequence(RefSeq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString RefError;
		const bool bRefOk = RunGpuGenerationToCompletion(*Gpu, RefSeq, Request, ReferenceTokens, /*MaxWallClockSeconds*/ 60.0, RefError);
		Gpu->ReturnSequence(RefSeq);
		if (!TestTrue(*FString::Printf(TEXT("the unsaved reference run must complete (%s)"), *RefError), bRefOk))
		{
			return false;
		}
	}

	// --- The interrupted construction: tick EXACTLY the prompt's own length -- one one-call
	// step per prompt token (D-SLM7379) -- so the save lands the instant the sequence becomes
	// primed, before its first decode/finish step has run. ---
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
	{
		return false;
	}
	for (int32 T = 0; T < Request.PromptTokens.Num(); ++T)
	{
		Gpu->Tick(1.0f / 60.0f);
	}
	if (!TestEqual(TEXT("the construction must have consumed the whole prompt and produced no decode token yet -- otherwise this is not the primed-at-save boundary finding 1(a) exists to catch"),
			Gpu->GetGeneratedTokens(Seq).Num(), 0))
	{
		return false;
	}

	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const bool bSaveOk = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
	if (!TestTrue(*FString::Printf(TEXT("RequestSaveSequence() at the primed boundary must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), bSaveOk))
	{
		return false;
	}
	Gpu->ReturnSequence(Seq);

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestEqual(*FString::Printf(TEXT("RequestRestoreSequence() must return Success (%s)"), *Gpu->GetLastLifecycleRequestError()),
			(uint8)DriveRestoreToResolution(*Gpu, RestoreHandle, Restored), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	TArray<int32> RestoredTokens;
	FString RunError;
	const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Restored, Request, RestoredTokens, /*MaxWallClockSeconds*/ 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Restored);

	// FEAT oracle: a restore that fails to reinstate TS.MaxNewTokens/LastToken/StopTokenIds/
	// ContextUsed either stalls (finding 1(a)'s own outcome for a primed save -- never reaches
	// Complete) or faults; a correct restore reaches Complete with EXACTLY the reference's own
	// tokens (all of them, since none were produced before this save).
	const bool bReachedComplete = TestTrue(*FString::Printf(TEXT("the restored sequence must reach Complete, continuing past a save taken exactly at the primed boundary (%s)"), *RunError), bCompleted);
	const bool bTokensMatch = TestEqual(TEXT("the restored continuation must be token-identical to the unsaved reference run"), RestoredTokens, ReferenceTokens);
	return bReachedComplete && bTokensMatch;
}

// --- (b) Saved MID-DECODE, not primed: at least one decode/finish step has already run past
// the prompt (TS.bPrimed == false, TS.LastToken holds a real produced token). Finding 1's other
// reachable outcome: the restored TS.LastToken defaults to -1, which Layer 1 rejects
// (SSLM_TOKEN_ID_OUT_OF_RANGE) on the very next decode step -- the restored sequence faults
// immediately with RecoverablePerSequenceRejection, and the restore appears to have succeeded
// (ESuperSLMRestoreResult::Success) right up until the first token after it. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2LifetimeSaveAfterPromptExhaustedMidDecodeTest,
	"SuperSLM.L2S2.Lifetime.SaveRestoreAfterPromptExhaustedMidDecodeMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2LifetimeSaveAfterPromptExhaustedMidDecodeTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	USuperSLMSubsystem* Cpu = GetSubsystem(World);
	if (!TestNotNull(TEXT("CPU reference subsystem"), Cpu)) { return false; }
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.BlockCount = 1;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU reference configure"), (uint8)Cpu->Configure(Model, CpuConfig).Result,
		(uint8)ESuperSLMConfigureResult::Success) ||
		!TestTrue(TEXT("natural prompt tokenizes"), Cpu->Tokenize(TEXT("A customer walks up to the counter."), Request.PromptTokens))) { return false; }
	Request.MaxNewTokens = 20;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> CpuReference;
	{
		FSuperSLMSequence CpuSeq;
		FString Error;
		if (!TestEqual(TEXT("CPU reference vend"), (uint8)Cpu->VendSequence(CpuSeq),
			(uint8)ESuperSLMVendResult::Success) ||
			!TestTrue(TEXT("CPU reference begins"), Cpu->BeginGeneration(CpuSeq, Request, Error))) { return false; }
		const double Deadline = FPlatformTime::Seconds() + 120.0;
		while (Cpu->GetPhase(CpuSeq) != ESuperSLMSequencePhase::Complete &&
			Cpu->GetPhase(CpuSeq) != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() < Deadline)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestEqual(TEXT("CPU natural-prompt reference completes"), (uint8)Cpu->GetPhase(CpuSeq),
			(uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		CpuReference = Cpu->GetGeneratedTokens(CpuSeq);
		Cpu->ReturnSequence(CpuSeq);
	}

	// --- Unsaved reference: the identical request, run start to finish, no save/restore. ---
	TArray<int32> ReferenceTokens;
	{
		FSuperSLMGpuSequence RefSeq;
		if (!TestEqual(TEXT("VendSequence() (reference)"), (uint8)Gpu->VendSequence(RefSeq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString RefError;
		const bool bRefOk = RunGpuGenerationToCompletion(*Gpu, RefSeq, Request, ReferenceTokens, /*MaxWallClockSeconds*/ 60.0, RefError);
		Gpu->ReturnSequence(RefSeq);
		if (!TestTrue(*FString::Printf(TEXT("the unsaved reference run must complete (%s)"), *RefError), bRefOk))
		{
			return false;
		}
	}
	if (!TestEqual(TEXT("GPU natural-prompt reference matches CPU"), ReferenceTokens, CpuReference)) { return false; }

	// --- The interrupted construction: save as soon as the first decode token
	// reaches the game thread, while later work can still be in flight.
	// The oracle captures the count at save resolution and compares the delivered
	// prefix plus restored tail with the independently run reference. ---
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
	{
		return false;
	}
	constexpr double MaxWaitForFirstTokenSeconds = 120.0;
	const double WaitStartSeconds = FPlatformTime::Seconds();
	while (Gpu->GetGeneratedTokens(Seq).IsEmpty() &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Complete &&
		Gpu->GetPhase(Seq) != ESuperSLMSequencePhase::Faulted &&
		FPlatformTime::Seconds() - WaitStartSeconds < MaxWaitForFirstTokenSeconds)
	{
		Gpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("save follows a delivered decode token"),
		!Gpu->GetGeneratedTokens(Seq).IsEmpty() && Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding)) { return false; }

	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const bool bSaveOk = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
	if (!TestTrue(*FString::Printf(TEXT("RequestSaveSequence() one decode step past the prompt must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), bSaveOk))
	{
		return false;
	}
	const TArray<int32> DeliveredAtSave = Gpu->GetGeneratedTokens(Seq);
	int32 SavedObserved = -1, SavedProduced = -1;
	if (!TestTrue(TEXT("save exposes observed and produced counts"),
		Gpu->GetSaveTokenCounts(SaveHandle, SavedObserved, SavedProduced)) ||
		!TestEqual(TEXT("saved observed count equals delivered prefix"), SavedObserved, DeliveredAtSave.Num()) ||
		!TestTrue(TEXT("paced save has produced tokens still in flight"), SavedObserved < SavedProduced))
	{
		return false;
	}
	Gpu->ReturnSequence(Seq);

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestEqual(*FString::Printf(TEXT("RequestRestoreSequence() must return Success (%s)"), *Gpu->GetLastLifecycleRequestError()),
			(uint8)DriveRestoreToResolution(*Gpu, RestoreHandle, Restored), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	TArray<int32> RestoredTail;
	FString RunError;
	const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Restored, Request, RestoredTail, /*MaxWallClockSeconds*/ 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Restored);

	{
		FString RefStr, TailStr, DeliveredStr;
		for (int32 Tk : ReferenceTokens) { RefStr += FString::Printf(TEXT("%d,"), Tk); }
		for (int32 Tk : RestoredTail) { TailStr += FString::Printf(TEXT("%d,"), Tk); }
		for (int32 Tk : DeliveredAtSave) { DeliveredStr += FString::Printf(TEXT("%d,"), Tk); }
		AddInfo(FString::Printf(TEXT("[diagnostic] ReferenceTokens (%d): [%s]"), ReferenceTokens.Num(), *RefStr));
		AddInfo(FString::Printf(TEXT("[diagnostic] DeliveredAtSave (%d): [%s]"), DeliveredAtSave.Num(), *DeliveredStr));
		AddInfo(FString::Printf(TEXT("[diagnostic] RestoredTail (%d): [%s]"), RestoredTail.Num(), *TailStr));
	}

	// Delivered-total oracle (T-2987 F15): a produced token still in flight at save
	// must not disappear between the delivered prefix and the restored continuation.
	// The unsaved reference is independently generated above. A suffix-only comparison
	// would accept a lost carried token.
	const bool bReachedComplete = TestTrue(*FString::Printf(TEXT("the restored sequence must reach Complete, continuing past a save taken one decode step after the prompt (%s)"), *RunError), bCompleted);
	const bool bHasBothParts = TestTrue(TEXT("save has delivered tokens and restore has a nonempty continuation"),
		DeliveredAtSave.Num() > 0 && RestoredTail.Num() > 0);
	TSet<int32> DistinctAfterSave;
	for (int32 I = DeliveredAtSave.Num(); I < ReferenceTokens.Num(); ++I)
	{
		DistinctAfterSave.Add(ReferenceTokens[I]);
	}
	const bool bDiscriminatingTail = TestTrue(TEXT("the reference has at least eight distinct tokens after the observed save point"),
		DistinctAfterSave.Num() >= 8);
	TArray<int32> DeliveredTotal = DeliveredAtSave;
	DeliveredTotal.Append(RestoredTail);
	const bool bTokensMatch = TestEqual(TEXT("delivered-at-save prefix plus restored tail equals the unsaved reference"),
		DeliveredTotal, ReferenceTokens);
	return bReachedComplete && bHasBothParts && bDiscriminatingTail && bTokensMatch;
}

#endif // WITH_DEV_AUTOMATION_TESTS
