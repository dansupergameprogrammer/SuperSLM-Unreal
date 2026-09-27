// T-2816 -- L2-S2 red suite. Plan cell R-S2a (the plan
// §9): "The GPU backend matches the CPU and keeps the frame contract on a passing
// device" -- dims 6, 7, 8. Also the cell that sets the k default (§12 decision 4).
//
// UPDATED per fold 7 (D-SLM7381, T-2785 §13; maintainer's own routed instruction,
// 2026-09-19): the plan's single-sequence K floor made this cell UNSATISFIABLE by
// arithmetic -- 8 composed sequences sharing 4 layers/tick need 48 ticks per token
// round against the old K=24, so every token was late. `MinimumK` is now
// `ceil(BlockCount x num_hidden_layers / LayersPerTick)` for the composed path, and
// `Configure()` refuses any K below it by name. This cell configures K = MinimumK
// (48, at this cell's own BlockCount=8/4-layers-per-tick) with a FIXED DeltaSeconds of
// 1/60 (never adapted, matching the "K is never adapted" contract), asserts the
// refusal of `MinimumK - 1`, and REPORTS the hitch count rather than asserting it zero
// -- D-SLM7358's own principle: lateness on a shared GPU is measured, not promised.
//
// A-EX on the RTX 2080 SUPER, 8 concurrent sequences, 64 tokens each, the composed path
// at a calibrated slice: ~~results applied at T+K or counted as a hitch~~ never
// reordered; replaying the session gives identical tokens; tokens equal the CPU
// backend's for the same session.
//
// RESTATED 2026-09-26 (plan §2.5 row 21, "Ruling 2026-09-26", folded the same day with its blind
// review's findings; §9 R-S2a): the GPU Tick() never waits on the submission thread. An overdue
// event applies on the first later tick at which its job has finished, in order, never before its
// own apply tick; each event is examined once, at its apply tick, and counts at most one hitch.
// Plan starts no new token for a sequence left holding an overdue event, nor any while K tick jobs
// are outstanding. So the main arm asserts, from the test access's event log: no event applies
// before its own apply tick, applied ticks never decrease within a sequence, and
// GetGameHitchCount()'s increase since the logs were enabled, read with the queue drained, equals
// the test's own count. There is still no zero-hitch assertion. The slowed-device arm
// (SuperSLM.L2S2.FrameContract.SlowedDevice) makes the device about six times slower than a 60 Hz
// harness; the stall arm (SuperSLM.L2S2.FrameContract.Stall) holds one tick job on a test-released
// gate for at least 11 s past its examination (the second review round). Record:
// the test record.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// The composed path's own calibrated slice is R-S2f's OUTPUT (plan §9: "R-S2f ...
	// sets the default slice"), not yet measured when this suite is authored --
	// 4 layers/slice (DispatchBudget 96) is the plan's own DERIVED, UNDERIVED candidate
	// (§5: "4 layers (dispatch_budget 96, <= 1.7 ms)"), used here as this test's own
	// placeholder until R-S2f's real measurement supersedes it. Flagged in
	// the red-suite record §3.
	constexpr int32 kCandidateLayersPerSlice = 4;

	bool SetUpGpuAndCpu(FAutomationTestBase& T, UWorld* World, int32 BlockCount, USuperSLMModel*& OutModel,
		USuperSLMGpuSubsystem*& OutGpu, USuperSLMSubsystem*& OutCpu, int32 KOverride = 0)
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
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable from the automation world's GameInstance"), OutGpu))
		{
			return false;
		}

		// D-SLM7381's own formula for the composed path: ceil(BlockCount x L / LayersPerTick).
		// BlockCount=8, L=AExNumHiddenLayers=24, LayersPerTick=kCandidateLayersPerSlice=4
		// -> MinimumK = ceil(8 * 24 / 4) = 48.
		const int32 ExpectedMinimumK = FMath::CeilToInt32((float)(BlockCount * AExNumHiddenLayers) / (float)kCandidateLayersPerSlice);

		FSuperSLMGpuRuntimeConfig GpuConfig;
		GpuConfig.ContextCap = 4096; // matches A-EX's own conversion cap, plan §8/§12 decision 9
		GpuConfig.BlockCount = BlockCount;
		GpuConfig.DispatchBudget = DispatchBudgetForLayersPerSlice(kCandidateLayersPerSlice);
		GpuConfig.TickBudgetMs = 1000.0;

		// A K below MinimumK must be refused, by name (D-SLM7381) -- checked BEFORE the
		// real Configure() below, against a throwaway config, so the refusal itself
		// never leaves the subsystem configured for the rest of the arm.
		FSuperSLMGpuRuntimeConfig BelowMinConfig = GpuConfig;
		BelowMinConfig.K = ExpectedMinimumK - 1;
		const FSuperSLMGpuConfigureReport BelowMinReport = OutGpu->Configure(OutModel, BelowMinConfig);
		if (!T.TestEqual(TEXT("Configure() must refuse K = MinimumK - 1"), (uint8)BelowMinReport.Result, (uint8)ESuperSLMGpuConfigureResult::KBelowMinimum))
		{
			return false;
		}

		GpuConfig.K = KOverride > 0 ? KOverride : ExpectedMinimumK; // the stall arm sets K = 52 (§9 R-S2a)
		const FSuperSLMGpuConfigureReport GpuReport = OutGpu->Configure(OutModel, GpuConfig);
		if (!T.TestEqual(TEXT("GPU Configure() result at K = MinimumK"), (uint8)GpuReport.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}
		T.TestEqual(TEXT("Configure()'s own reported MinimumK must match this cell's own derivation"), GpuReport.MinimumK, ExpectedMinimumK);
		T.AddInfo(FString::Printf(TEXT("GPU Configure(): MinimumK=%d, K=%d"), GpuReport.MinimumK, GpuConfig.K));

		OutCpu = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem (CPU) must be reachable from the automation world's GameInstance"), OutCpu))
		{
			return false;
		}

		FSuperSLMRuntimeConfig CpuConfig;
		CpuConfig.MaxSequencesPerDecodeCall = BlockCount;
		CpuConfig.MaxPrefillChunkBudget = 64;
		CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
		CpuConfig.BlockCount = BlockCount;
		CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
		CpuConfig.TickBudgetMs = 1000.0;
		const FSuperSLMConfigureReport CpuReport = OutCpu->Configure(OutModel, CpuConfig);
		return T.TestEqual(TEXT("CPU Configure() result"), (uint8)CpuReport.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// Runs ONE session (one prompt) through the GPU composed path and returns its
	// generated tokens.
	bool RunOneGpuSession(FAutomationTestBase& T, UWorld* World, USuperSLMGpuSubsystem& Gpu, const FString& PromptText, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!T.TestEqual(TEXT("VendSequence()"), (uint8)Gpu.VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}

		FSuperSLMGenerationRequest Request;
		// Tokenization is CPU-subsystem-owned (Tokenize() is not duplicated on the GPU
		// subsystem, since Layer 1's tokenizer is backend-agnostic, plan §2.1) --
		// callers tokenize once and pass ids to whichever backend they target. This
		// test tokenizes via the CPU subsystem for that reason.
		USuperSLMSubsystem* Cpu = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("CPU subsystem must be reachable to tokenize"), Cpu))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("Tokenize() must succeed"), Cpu->Tokenize(PromptText, Request.PromptTokens)))
		{
			return false;
		}
		Request.MaxNewTokens = 64;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;

		FString RunError;
		const bool bCompleted = RunGpuGenerationToCompletion(Gpu, Seq, Request, OutTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
		Gpu.ReturnSequence(Seq);
		return T.TestTrue(*FString::Printf(TEXT("GPU generation must complete (%s)"), *RunError), bCompleted);
	}

	// --- The event and plan logs (plan §2.5 row 21, "Ruling 2026-09-26" as folded, implementation item
	// 13). This adapter is the only code in the suite that names the test access's log types and
	// fields (record §1). Everything below it reads these local copies. ---
	struct FLoggedEvent
	{
		int64 SequenceId = 0;
		bool bPrefill = false;
		bool bCarried = false;
		int64 RequestTick = 0;
		int64 ApplyTick = 0;
		double RequestWallSeconds = 0.0;
		double RequestSimSeconds = 0.0;
		bool bExamined = false;
		bool bJobDoneAtExamination = false;
		double WallAtExamination = 0.0;
		double SimAtExamination = 0.0;
		int32 TickJobsAheadAtExamination = 0;
		int64 RemovedTick = -1;           // applied or dropped at; -1 = still pending
		bool bDropped = false;
		double RemovedWallSeconds = 0.0;

		bool IsApplied() const { return RemovedTick >= 0 && !bDropped; }
		// (v)'s reading: a pending event counts as removed at +inf, a dropped one at its drop tick.
		int64 RemovedOrInfinity() const { return RemovedTick < 0 ? TNumericLimits<int64>::Max() : RemovedTick; }
	};

	struct FLoggedPlanTick
	{
		int64 Tick = 0;
		int32 OutstandingTickJobsAtPlan = 0;
		bool bCapBinding = false;
		TArray<int64> IssuedSequenceIds;
		TArray<int64> StartedSequenceIds;
		TArray<int64> HeldSequenceIds;
		TArray<int64> InFlightAtPlanSequenceIds;
	};

	FLoggedPlanTick CopyPlanTick(const FSuperSLMGpuPlanLogEntry& P)
	{
		FLoggedPlanTick L;
		L.Tick = P.Tick;
		L.OutstandingTickJobsAtPlan = P.OutstandingTickJobsAtPlan;
		L.bCapBinding = P.bCapBinding;
		L.IssuedSequenceIds = P.IssuedSequenceIds;
		L.StartedSequenceIds = P.StartedSequenceIds;
		L.HeldSequenceIds = P.HeldSequenceIds;
		L.InFlightAtPlanSequenceIds = P.InFlightAtPlanSequenceIds;
		return L;
	}

	void ReadApplyLogs(const USuperSLMGpuSubsystem& Gpu, TArray<FLoggedEvent>& OutEvents, TArray<FLoggedPlanTick>& OutPlan)
	{
		OutEvents.Reset();
		OutPlan.Reset();
		for (const FSuperSLMGpuEventLogEntry& E : FSuperSLMGpuTestAccess::GetEventLog(Gpu))
		{
			FLoggedEvent& L = OutEvents.AddDefaulted_GetRef();
			L.SequenceId = E.SequenceId;
			L.bPrefill = E.bPrefill;
			L.bCarried = E.bCarried;
			L.RequestTick = E.RequestTick;
			L.ApplyTick = E.ApplyTick;
			L.RequestWallSeconds = E.RequestWallSeconds;
			L.RequestSimSeconds = E.RequestSimSeconds;
			L.bExamined = E.bExamined;
			L.bJobDoneAtExamination = E.bJobDoneAtExamination;
			L.WallAtExamination = E.WallAtExamination;
			L.SimAtExamination = E.SimAtExamination;
			L.TickJobsAheadAtExamination = E.TickJobsAheadAtExamination;
			L.RemovedTick = E.RemovedTick;
			L.bDropped = E.bDropped;
			L.RemovedWallSeconds = E.RemovedWallSeconds;
		}
		for (const FSuperSLMGpuPlanLogEntry& P : FSuperSLMGpuTestAccess::GetPlanLog(Gpu))
		{
			OutPlan.Add(CopyPlanTick(P));
		}
	}

	const FLoggedPlanTick* FindPlanTick(const TArray<FLoggedPlanTick>& Plan, int64 Tick)
	{
		for (const FLoggedPlanTick& P : Plan)
		{
			if (P.Tick == Tick)
			{
				return &P;
			}
		}
		return nullptr;
	}

	// What the logs say, computed by the test alone, with no hitch decision taken from the
	// product.
	struct FApplyLogVerdict
	{
		int32 Events = 0;
		int32 Examined = 0;
		int32 Pending = 0;
		int32 Dropped = 0;
		int32 UnfinishedAtExamination = 0;
		int32 TokenMissHitches = 0;       // tokens with ApplyTick > RequestTick + K
		int32 DeviceHitches = 0;          // every other examined event read unfinished, wall >= sim
		int32 EarlyApplications = 0;      // applied before its own apply tick
		int32 OrderViolations = 0;        // applied ticks decrease within a sequence, or apply after a pending one
		int32 BackPressureViolations = 0; // (v), over StartedSequenceIds only
		FString FirstEarly;
		FString FirstOrder;
		FString FirstBackPressure;

		int32 ExpectedHitches() const { return TokenMissHitches + DeviceHitches; }
	};

	FApplyLogVerdict EvaluateApplyLog(const TArray<FLoggedEvent>& Events, const TArray<FLoggedPlanTick>& Plan, int32 K)
	{
		FApplyLogVerdict V;
		TMap<int64, TArray<const FLoggedEvent*>> BySequence;
		for (const FLoggedEvent& E : Events)
		{
			++V.Events;
			BySequence.FindOrAdd(E.SequenceId).Add(&E); // log order = planning order
			V.Pending += E.RemovedTick < 0 ? 1 : 0;
			V.Dropped += E.bDropped ? 1 : 0;
			if (E.IsApplied() && E.RemovedTick < E.ApplyTick)
			{
				if (V.EarlyApplications++ == 0)
				{
					V.FirstEarly = FString::Printf(TEXT("sequence %lld: applied at tick %lld, its apply tick %lld"), E.SequenceId, E.RemovedTick, E.ApplyTick);
				}
			}
			if (!E.bExamined)
			{
				continue;
			}
			++V.Examined;
			V.UnfinishedAtExamination += E.bJobDoneAtExamination ? 0 : 1;
			// The ruling's hitch, at most once per event, in exactly one of two cases, from the
			// decision's own logged readings.
			if (!E.bPrefill && E.ApplyTick > E.RequestTick + K)
			{
				++V.TokenMissHitches;
			}
			else if (!E.bJobDoneAtExamination
				&& (E.WallAtExamination - E.RequestWallSeconds) >= (E.SimAtExamination - E.RequestSimSeconds))
			{
				++V.DeviceHitches;
			}
		}
		for (const TPair<int64, TArray<const FLoggedEvent*>>& Pair : BySequence)
		{
			int64 LastApplied = TNumericLimits<int64>::Lowest();
			bool bSeenPending = false;
			for (const FLoggedEvent* E : Pair.Value)
			{
				if (E->bDropped)
				{
					continue; // removed without applying; it constrains no order
				}
				if (!E->IsApplied())
				{
					bSeenPending = true;
					continue;
				}
				if (bSeenPending || E->RemovedTick < LastApplied)
				{
					if (V.OrderViolations++ == 0)
					{
						V.FirstOrder = FString::Printf(TEXT("sequence %lld: an event applied at tick %lld after an earlier one %s"),
							Pair.Key, E->RemovedTick, bSeenPending ? TEXT("still pending") : *FString::Printf(TEXT("applied at tick %lld"), LastApplied));
					}
				}
				LastApplied = FMath::Max(LastApplied, E->RemovedTick);
			}
		}
		for (const FLoggedPlanTick& P : Plan)
		{
			for (const int64 SequenceId : P.StartedSequenceIds)
			{
				const TArray<const FLoggedEvent*>* SequenceEvents = BySequence.Find(SequenceId);
				if (SequenceEvents == nullptr)
				{
					continue;
				}
				for (const FLoggedEvent* E : *SequenceEvents)
				{
					if (E->ApplyTick <= P.Tick && P.Tick < E->RemovedOrInfinity())
					{
						if (V.BackPressureViolations++ == 0)
						{
							V.FirstBackPressure = FString::Printf(TEXT("tick %lld started a token for sequence %lld while its event with apply tick %lld was unremoved (removed at %lld)"),
								P.Tick, SequenceId, E->ApplyTick, E->RemovedTick);
						}
						break; // one violation per (tick, sequence)
					}
				}
			}
		}
		return V;
	}

	// Ticks until both outstanding job counts read 0, so the game-side hitch counter and the logs
	// are read over a settled queue (§9 R-S2a, folded finding 7).
	bool DrainOutstandingJobs(USuperSLMGpuSubsystem& Gpu, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(Gpu) + FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(Gpu) > 0)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			Gpu.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(1.0f / 60.0f);
		}
		return true;
	}

	// The arms' harness pacing (§9 R-S2a, round 2, finding 4): each tick starts at least
	// kHarnessWallStepSeconds (1/60 s) of wall time after the previous one and passes
	// kHarnessDeltaSeconds (1/120) as DeltaSeconds, so every event examined unfinished has had more
	// wall time than sim time since its request and the device term fires for it. Returns the
	// Tick() call's own wall time in ms.
	constexpr double kHarnessWallStepSeconds = 1.0 / 60.0;
	constexpr float kHarnessDeltaSeconds = 1.0f / 120.0f;

	double PacedHarnessTick(USuperSLMGpuSubsystem& Gpu, double& InOutLastTickStart)
	{
		for (double Now = FPlatformTime::Seconds(); Now - InOutLastTickStart < kHarnessWallStepSeconds; Now = FPlatformTime::Seconds())
		{
			FPlatformProcess::Sleep(FMath::Max(0.0f, (float)(kHarnessWallStepSeconds - (Now - InOutLastTickStart)) * 0.5f));
		}
		InOutLastTickStart = FPlatformTime::Seconds();
		Gpu.Tick(kHarnessDeltaSeconds);
		return (FPlatformTime::Seconds() - InOutLastTickStart) * 1000.0;
	}

	// The CPU backend's tokens for one prompt, the reference R-S2a compares against.
	bool RunCpuReference(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FString& PromptText, int32 MaxNewTokens, TArray<int32>& OutTokens)
	{
		FSuperSLMSequence CpuSeq;
		if (!T.TestEqual(TEXT("CPU VendSequence()"), (uint8)Cpu.VendSequence(CpuSeq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest CpuRequest;
		Cpu.Tokenize(PromptText, CpuRequest.PromptTokens);
		CpuRequest.MaxNewTokens = MaxNewTokens;
		CpuRequest.SpanKind = ESuperSLMSpanKind::Prompt;
		FString BeginError;
		bool bCompleted = Cpu.BeginGeneration(CpuSeq, CpuRequest, BeginError);
		if (bCompleted)
		{
			bCompleted = false;
			const double CpuStartSeconds = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() - CpuStartSeconds < 60.0)
			{
				Cpu.Tick(1.0f / 60.0f);
				const ESuperSLMSequencePhase Phase = Cpu.GetPhase(CpuSeq);
				if (Phase == ESuperSLMSequencePhase::Complete)
				{
					OutTokens = Cpu.GetGeneratedTokens(CpuSeq);
					bCompleted = true;
					break;
				}
				if (Phase == ESuperSLMSequencePhase::Faulted)
				{
					break;
				}
				FPlatformProcess::Sleep(0.001f);
			}
		}
		Cpu.ReturnSequence(CpuSeq);
		return T.TestTrue(*FString::Printf(TEXT("CPU reference generation must complete (%s)"), *BeginError), bCompleted);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2FrameContractTest,
	"SuperSLM.L2S2.FrameContract.GpuMatchesCpuAndKeepsContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2FrameContractTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	USuperSLMSubsystem* Cpu = nullptr;
	constexpr int32 kBlockCount = 8;
	if (!SetUpGpuAndCpu(*this, World, kBlockCount, Model, Gpu, Cpu))
	{
		return false;
	}
	// Restated 2026-09-26: the event and plan logs are on for the whole GPU part of this arm, and
	// the hitch counter is read from here.
	TestTrue(TEXT("the logs are enabled while no event is pending"), FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, true));
	const int32 GameHitchesBefore = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu);
	const int32 ThreadHitchesBefore = FSuperSLMGpuTestAccess::GetThreadHitchCount(*Gpu);
	const int32 ConfiguredK = FMath::CeilToInt32((float)(kBlockCount * AExNumHiddenLayers) / (float)kCandidateLayersPerSlice);

	// 8 distinct prompts, one per concurrent sequence -- reaching the demo schema's own
	// "buy" intent is not required here (R-S2a is the frame-contract claim, not the
	// schema-accuracy one); a generic set of short prompts stresses concurrency without
	// needing the schema bound.
	TArray<FString> Prompts;
	for (int32 I = 0; I < kBlockCount; ++I)
	{
		Prompts.Add(FString::Printf(TEXT("Tell me something about the number %d."), I + 1));
	}

	TArray<TArray<int32>> GpuTokensFirstRun;
	GpuTokensFirstRun.SetNum(kBlockCount);
	int32 FailureCount = 0;
	for (int32 I = 0; I < kBlockCount; ++I)
	{
		if (!RunOneGpuSession(*this, World, *Gpu, Prompts[I], GpuTokensFirstRun[I]))
		{
			++FailureCount;
		}
	}
	if (!TestEqual(TEXT("every concurrent session must complete"), FailureCount, 0))
	{
		return false;
	}

	// D-SLM7381: a hitch is a token not computed at its T+K tick despite the device
	// being given the sim time those ticks represent, or a slice over TickBudgetMs --
	// REPORTED, never asserted zero: lateness on a shared GPU is measured, not
	// promised (D-SLM7358's own principle, applied here to the GPU scheduler).
	AddInfo(FString::Printf(TEXT("GetHitchCount()=%d across the concurrent run"), Gpu->GetHitchCount()));
	AddInfo(FString::Printf(TEXT("GetLastGpuBusyMs()=%f GetLastHostFinishMs()=%f"), Gpu->GetLastGpuBusyMs(), Gpu->GetLastHostFinishMs()));

	// Tokens equal the CPU backend's for the SAME session (plan §9 R-S2a).
	for (int32 I = 0; I < kBlockCount; ++I)
	{
		FSuperSLMSequence CpuSeq;
		if (!TestEqual(TEXT("CPU VendSequence()"), (uint8)Cpu->VendSequence(CpuSeq), (uint8)ESuperSLMVendResult::Success))
		{
			++FailureCount;
			continue;
		}
		FSuperSLMGenerationRequest CpuRequest;
		Cpu->Tokenize(Prompts[I], CpuRequest.PromptTokens);
		CpuRequest.MaxNewTokens = 64;
		CpuRequest.SpanKind = ESuperSLMSpanKind::Prompt;

		TArray<int32> CpuTokens;
		FString CpuError;
		// Fixtures/SuperSLML2S1Fixtures.h's own loop is not reused here (this file's own
		// no-cross-suite-coupling choice, header note above); a locally-equivalent loop
		// is inlined.
		FString BeginError;
		bool bCompleted = Cpu->BeginGeneration(CpuSeq, CpuRequest, BeginError);
		if (bCompleted)
		{
			bCompleted = false;
			const double CpuStartSeconds = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() - CpuStartSeconds < 60.0)
			{
				Cpu->Tick(1.0f / 60.0f);
				const ESuperSLMSequencePhase Phase = Cpu->GetPhase(CpuSeq);
				if (Phase == ESuperSLMSequencePhase::Complete)
				{
					CpuTokens = Cpu->GetGeneratedTokens(CpuSeq);
					bCompleted = true;
					break;
				}
				if (Phase == ESuperSLMSequencePhase::Faulted)
				{
					break;
				}
				FPlatformProcess::Sleep(0.001f);
			}
		}
		Cpu->ReturnSequence(CpuSeq);

		if (!TestTrue(*FString::Printf(TEXT("prompt %d: CPU reference generation must complete"), I), bCompleted))
		{
			++FailureCount;
			continue;
		}
		TestEqual(*FString::Printf(TEXT("prompt %d: GPU tokens must equal CPU tokens for the same session"), I), GpuTokensFirstRun[I], CpuTokens);
	}

	// Replaying the session gives identical tokens (determinism, not just cross-backend
	// agreement) -- re-run prompt 0 through the GPU path a second time.
	TArray<int32> GpuTokensReplay;
	if (RunOneGpuSession(*this, World, *Gpu, Prompts[0], GpuTokensReplay))
	{
		TestEqual(TEXT("replaying the same session on the GPU gives identical tokens"), GpuTokensReplay, GpuTokensFirstRun[0]);
	}
	else
	{
		++FailureCount;
	}

	// Restated 2026-09-26 and folded (§9 R-S2a, review finding 7): from the event log, no event
	// applies before its own apply tick and applied ticks never decrease within a sequence; once both
	// outstanding job counts are 0, GetGameHitchCount()'s increase since the logs were enabled
	// equals the test's own count, formula (iv) of the slowed-device arm (round 2, finding 10).
	// GetThreadHitchCount() is reported. No zero-hitch assertion.
	{
		TestTrue(TEXT("both outstanding job counts reach 0 before the hitch counts are read"), DrainOutstandingJobs(*Gpu, 60.0));
		const int32 HitchIncrease = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu) - GameHitchesBefore;
		const int32 ThreadHitchIncrease = FSuperSLMGpuTestAccess::GetThreadHitchCount(*Gpu) - ThreadHitchesBefore;
		TArray<FLoggedEvent> Events;
		TArray<FLoggedPlanTick> Plan;
		ReadApplyLogs(*Gpu, Events, Plan);
		FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
		const FApplyLogVerdict V = EvaluateApplyLog(Events, Plan, ConfiguredK);
		AddInfo(FString::Printf(TEXT("event log: %d events, %d examined, %d pending, %d dropped, %d unfinished at examination; game hitches: %d counted by the product, %d by the log (%d T+K misses, %d device); slice hitches (GetThreadHitchCount()): %d"),
			V.Events, V.Examined, V.Pending, V.Dropped, V.UnfinishedAtExamination, HitchIncrease, V.ExpectedHitches(), V.TokenMissHitches, V.DeviceHitches, ThreadHitchIncrease));
		TestTrue(TEXT("the event log recorded the run's events"), V.Events > 0);
		TestEqual(*FString::Printf(TEXT("no event applies before its own apply tick (first: %s)"), *V.FirstEarly), V.EarlyApplications, 0);
		TestEqual(*FString::Printf(TEXT("applied ticks never decrease within a sequence (first: %s)"), *V.FirstOrder), V.OrderViolations, 0);
		TestEqual(TEXT("GetGameHitchCount()'s increase since the logs were enabled equals the test's own count from the event log"), HitchIncrease, V.ExpectedHitches());
	}

	return FailureCount == 0 && !HasAnyErrors();
}

// --- R-S2a slowed-device arm (plan §9 R-S2a, ruling 2026-09-26 as folded; kills M-58, M-59, M-60,
// M-61, M-65) ---
// The main arm's configuration (BlockCount 8, K = MinimumK = 48, 4 layers per tick, TickBudgetMs
// 1000), 7 sequences generating concurrently at MaxNewTokens 8 with the eighth slot free, and every
// tick job made to sleep D = 100 ms after its call (outside the Layer-1 lock; D was 50 ms until
// round 3, finding 11), so the device runs about six times slower than a 60 Hz harness. Ticks are
// DeltaSeconds 1/120, each started at least 1/60 s of wall time after the previous one (round 2,
// finding 4). The seven prompts are pinned, each at least 12 tokens, and before (vii) is read two
// preconditions are asserted: sum(P) x NumHiddenLayers / LayersPerTick >= 450 prompt ticks (round 2,
// finding 5) and a mean prompt-phase tick rate >= 25 Hz (round 3, finding 11). Cost, derived: about
// 2 min.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2FrameContractSlowedDeviceTest,
	"SuperSLM.L2S2.FrameContract.SlowedDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2FrameContractSlowedDeviceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	constexpr int32 kBlockCount = 8;
	constexpr int32 kSequences = 7;                   // the eighth slot is left free for (vi)
	constexpr int32 kMaxNewTokens = 8;
	constexpr double kSubmissionDelaySeconds = 0.100; // D (round 3, finding 11)
	constexpr double kTickBoundMs = 50.0;             // (i): M-58 then waits >= 2D = 200 ms
	constexpr double kTickPercentile = 0.95;          // (i): at least 95 % of qualifying ticks (round 3, finding 10)
	constexpr double kCallBoundMs = 100.0;            // (vi): M-59/M-60 then wait >= 7D = 700 ms (round 3, finding 10)
	constexpr int32 kConstructionJobsAhead = 2;       // (i)'s qualifying ticks
	constexpr int32 kVendAtTickJobs = 8;              // (vi) (round 3, finding 10)
	constexpr double kMinPromptTickRateHz = 25.0;     // (vii)'s rate precondition: 450 x (1 - 10/25) = 270 > 240
	constexpr double kArmDeadlineSeconds = 180.0;
	constexpr int32 kMinPromptTokens = 12;            // round 2, finding 5
	constexpr int32 kMinPromptTicks = 450;            // sum(P) x NumHiddenLayers / LayersPerTick, (vii)'s precondition

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	USuperSLMSubsystem* Cpu = nullptr;
	if (!SetUpGpuAndCpu(*this, World, kBlockCount, Model, Gpu, Cpu))
	{
		return false;
	}
	const int32 ConfiguredK = FMath::CeilToInt32((float)(kBlockCount * AExNumHiddenLayers) / (float)kCandidateLayersPerSlice);
	const int32 HardQueueBound = ConfiguredK + kBlockCount * AExNumHiddenLayers; // (vii): 240
	const int32 TypicalQueueBound = ConfiguredK + ConfiguredK;                   // K + MinimumK = 96, reported

	FSuperSLMGpuSchemaHandle Schema;
	FString LookupError;
	if (!TestTrue(*FString::Printf(TEXT("the demo schema resolves by name (%s)"), *LookupError),
			FSuperSLMGpuSchemaLookup::LookupByName(*Model, DemoSchemaName(), Schema, LookupError)))
	{
		return false;
	}

	// The seven pinned prompts (§9 R-S2a, round 2, finding 5), each well over 12 tokens.
	static const TCHAR* const kPinnedPrompts[kSequences] = {
		TEXT("Tell me a short story about a brave knight who guards the old stone bridge at the edge of the village."),
		TEXT("Describe the busy harbor market on a cold winter morning, with the fishermen and their loud customers."),
		TEXT("Explain to a young apprentice how the town blacksmith heats, folds and hammers iron into a sword."),
		TEXT("Write a friendly letter from a traveling merchant to his sister about the mountains he crossed."),
		TEXT("Give the potion seller's advice to a nervous adventurer who is about to enter the dark forest."),
		TEXT("List the things a careful innkeeper checks every evening before she locks the doors of the inn."),
		TEXT("Tell the legend of the silver dragon that sleeps beneath the frozen lake north of the castle."),
	};
	TArray<FString> Prompts;
	TArray<FSuperSLMGenerationRequest> Requests;
	int32 PromptTokenSum = 0;
	for (int32 I = 0; I < kSequences; ++I)
	{
		Prompts.Add(kPinnedPrompts[I]);
		FSuperSLMGenerationRequest& Request = Requests.AddDefaulted_GetRef();
		if (!TestTrue(TEXT("Tokenize() must succeed"), Cpu->Tokenize(Prompts[I], Request.PromptTokens)))
		{
			return false;
		}
		TestTrue(*FString::Printf(TEXT("pinned prompt %d is at least %d tokens (%d)"), I, kMinPromptTokens, Request.PromptTokens.Num()),
			Request.PromptTokens.Num() >= kMinPromptTokens);
		PromptTokenSum += Request.PromptTokens.Num();
		Request.MaxNewTokens = kMaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
	}
	const int32 PromptTicks = PromptTokenSum * AExNumHiddenLayers / kCandidateLayersPerSlice;

	TestTrue(TEXT("the logs are enabled while no event is pending"), FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, true));
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, kSubmissionDelaySeconds);
	const int32 GameHitchesBefore = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu);
	const int32 ThreadHitchesBefore = FSuperSLMGpuTestAccess::GetThreadHitchCount(*Gpu);

	TArray<FSuperSLMGpuSequence> Sequences;
	for (int32 I = 0; I < kSequences; ++I)
	{
		FSuperSLMGpuSequence& Seq = Sequences.AddDefaulted_GetRef();
		if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success)
			|| !TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() (%s)"), *Gpu->GetLastLifecycleRequestError()), Gpu->RequestBeginGeneration(Seq, Requests[I]).IsValid()))
		{
			FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, 0.0);
			FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
			for (const FSuperSLMGpuSequence& S : Sequences)
			{
				if (S.IsValid()) { Gpu->ReturnSequence(S); }
			}
			return false;
		}
	}

	// The paced run. TickWallMs[i] is the i-th Tick() since the logs were enabled; the plan log
	// holds one entry per Tick() while logging is on, so entry i names that tick's TickIndex.
	TArray<double> TickWallMs;
	double MaxTickMs = 0.0;
	int32 MaxOutstandingTickJobs = 0;
	int32 QueueBoundViolations = 0;
	bool bFreeVended = false;
	bool bSetSchemaReturned = false;
	double SetSchemaMs = 0.0;
	double WalkStateMs = 0.0;
	uint32 WalkStateAtVend = 0;
	int32 OutstandingAtVend = 0;
	FString SetSchemaError;
	FSuperSLMGpuSequence FreeSeq;
	bool bBoundSeen = false;
	double BoundSeenAtSeconds = 0.0;
	bool bAllTerminal = false;
	const double ArmStart = FPlatformTime::Seconds();
	double LastTickStart = ArmStart - kHarnessWallStepSeconds;
	double FirstTickStartSeconds = -1.0; // (vii)'s rate precondition: the prompt phase starts at the first tick
	while (FPlatformTime::Seconds() - ArmStart < kArmDeadlineSeconds)
	{
		const double TickMs = PacedHarnessTick(*Gpu, LastTickStart);
		if (FirstTickStartSeconds < 0.0)
		{
			FirstTickStartSeconds = LastTickStart;
		}
		TickWallMs.Add(TickMs);
		MaxTickMs = FMath::Max(MaxTickMs, TickMs);
		const int32 OutstandingTickJobs = FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu);
		MaxOutstandingTickJobs = FMath::Max(MaxOutstandingTickJobs, OutstandingTickJobs);
		// (vii): the hard queue bound, after every Tick().
		QueueBoundViolations += OutstandingTickJobs > HardQueueBound ? 1 : 0;

		// (vi): at a moment with at least eight tick jobs outstanding, the free slot is vended and bound.
		if (!bFreeVended && OutstandingTickJobs >= kVendAtTickJobs)
		{
			bFreeVended = true;
			OutstandingAtVend = OutstandingTickJobs;
			if (TestEqual(TEXT("(vi) the free slot vends"), (uint8)Gpu->VendSequence(FreeSeq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
			{
				const double SetStart = FPlatformTime::Seconds();
				bSetSchemaReturned = Gpu->SetSchema(FreeSeq, Schema, SetSchemaError);
				SetSchemaMs = (FPlatformTime::Seconds() - SetStart) * 1000.0;
				const double WalkStart = FPlatformTime::Seconds();
				WalkStateAtVend = Gpu->GetSchemaWalkState(FreeSeq);
				WalkStateMs = (FPlatformTime::Seconds() - WalkStart) * 1000.0;
			}
		}
		if (bFreeVended && !bBoundSeen && FreeSeq.IsValid() && Gpu->GetBoundSchema(FreeSeq).Index == Schema.Index)
		{
			bBoundSeen = true;
			BoundSeenAtSeconds = FPlatformTime::Seconds() - ArmStart;
		}

		bAllTerminal = true;
		for (const FSuperSLMGpuSequence& Seq : Sequences)
		{
			const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Seq);
			bAllTerminal &= Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}
		if (bAllTerminal && (bBoundSeen || !bFreeVended || !FreeSeq.IsValid()))
		{
			break;
		}
	}
	const double ArmWallSeconds = FPlatformTime::Seconds() - ArmStart;
	const int32 PacedTicks = TickWallMs.Num();

	TArray<TArray<int32>> GpuTokens;
	TArray<ESuperSLMSequencePhase> GpuPhases;
	for (const FSuperSLMGpuSequence& Seq : Sequences)
	{
		GpuTokens.Add(Gpu->GetGeneratedTokens(Seq));
		GpuPhases.Add(Gpu->GetPhase(Seq));
	}

	// Undo the slowdown and drain, so the hitch counters and logs are read over a settled queue.
	// Jobs already queued keep the delay they were created with (implementation item 12).
	FSuperSLMGpuTestAccess::SetSubmissionDelaySeconds(*Gpu, 0.0);
	{
		const double DrainDeadline = FPlatformTime::Seconds() + 60.0;
		while (FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) + FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(*Gpu) > 0
			&& FPlatformTime::Seconds() < DrainDeadline)
		{
			TickWallMs.Add(PacedHarnessTick(*Gpu, LastTickStart));
		}
	}
	const bool bDrained = FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) + FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(*Gpu) == 0;
	const int32 GameHitchIncrease = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu) - GameHitchesBefore;
	const int32 ThreadHitchIncrease = FSuperSLMGpuTestAccess::GetThreadHitchCount(*Gpu) - ThreadHitchesBefore;
	TArray<FLoggedEvent> Events;
	TArray<FLoggedPlanTick> Plan;
	ReadApplyLogs(*Gpu, Events, Plan);
	FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
	for (const FSuperSLMGpuSequence& Seq : Sequences)
	{
		Gpu->ReturnSequence(Seq);
	}
	if (FreeSeq.IsValid())
	{
		Gpu->ReturnSequence(FreeSeq);
	}

	const FApplyLogVerdict V = EvaluateApplyLog(Events, Plan, ConfiguredK);

	// (i)/(ii): the ticks at which some event was examined unfinished with at least two tick jobs
	// ahead of its own. Examination happens at the event's apply tick.
	TMap<int64, double> TickMsByTickIndex;
	const bool bPlanMatchesTicks = Plan.Num() == TickWallMs.Num();
	for (int32 I = 0; bPlanMatchesTicks && I < Plan.Num(); ++I)
	{
		TickMsByTickIndex.Add(Plan[I].Tick, TickWallMs[I]);
	}
	TSet<int64> ConstructionTicks;
	int32 AppliedDeviceTermEntries = 0; // (ii), round 2 finding 4: (iv)'s device term fired on an applied entry
	for (const FLoggedEvent& E : Events)
	{
		if (E.bExamined && !E.bJobDoneAtExamination && E.TickJobsAheadAtExamination >= kConstructionJobsAhead)
		{
			ConstructionTicks.Add(E.ApplyTick);
		}
		const bool bTokenMiss = !E.bPrefill && E.ApplyTick > E.RequestTick + ConfiguredK;
		if (E.bExamined && E.IsApplied() && !bTokenMiss && !E.bJobDoneAtExamination
			&& (E.WallAtExamination - E.RequestWallSeconds) >= (E.SimAtExamination - E.RequestSimSeconds))
		{
			++AppliedDeviceTermEntries;
		}
	}
	int32 TimedConstructionTicks = 0;
	int32 FastConstructionTicks = 0;
	double MaxConstructionTickMs = 0.0;
	for (const int64 Tick : ConstructionTicks)
	{
		const double* Ms = TickMsByTickIndex.Find(Tick);
		if (Ms == nullptr)
		{
			continue;
		}
		++TimedConstructionTicks;
		MaxConstructionTickMs = FMath::Max(MaxConstructionTickMs, *Ms);
		FastConstructionTicks += *Ms < kTickBoundMs ? 1 : 0;
	}
	const double FastConstructionFraction = TimedConstructionTicks > 0 ? (double)FastConstructionTicks / (double)TimedConstructionTicks : 0.0;

	// (vii)'s rate precondition (round 3, finding 11): the prompt phase runs from the first tick to
	// the tick at which the last sequence's Prefill event is created (its RequestTick, from the event
	// log); its mean tick rate is the ticks between them over the wall time between them.
	int64 LastPrefillTick = -1;
	double LastPrefillWallSeconds = 0.0;
	for (const FLoggedEvent& E : Events)
	{
		if (E.bPrefill && E.RequestTick > LastPrefillTick)
		{
			LastPrefillTick = E.RequestTick;
			LastPrefillWallSeconds = E.RequestWallSeconds;
		}
	}
	const int64 PromptPhaseTicks = (LastPrefillTick >= 0 && Plan.Num() > 0) ? LastPrefillTick - Plan[0].Tick : 0;
	const double PromptPhaseWallSeconds = LastPrefillTick >= 0 ? LastPrefillWallSeconds - FirstTickStartSeconds : 0.0;
	const double PromptPhaseRateHz = PromptPhaseWallSeconds > 0.0 ? (double)PromptPhaseTicks / PromptPhaseWallSeconds : 0.0;

	// Reported.
	AddInfo(FString::Printf(TEXT("slowed device: game hitches %d (the log's own count %d: %d T+K misses, %d device); slice hitches (GetThreadHitchCount()) %d; largest outstanding tick-job count %d (typical bound K + MinimumK = %d, hard bound %d); arm wall time %.1f s over %d paced ticks; longest Tick() %.2f ms, longest construction tick %.2f ms over %d construction ticks"),
		GameHitchIncrease, V.ExpectedHitches(), V.TokenMissHitches, V.DeviceHitches, ThreadHitchIncrease, MaxOutstandingTickJobs, TypicalQueueBound, HardQueueBound,
		ArmWallSeconds, PacedTicks, MaxTickMs, MaxConstructionTickMs, ConstructionTicks.Num()));
	AddInfo(FString::Printf(TEXT("event log: %d events, %d examined, %d pending, %d dropped, %d unfinished at examination; %d plan-log ticks for %d Tick() calls"),
		V.Events, V.Examined, V.Pending, V.Dropped, V.UnfinishedAtExamination, Plan.Num(), TickWallMs.Num()));
	AddInfo(FString::Printf(TEXT("pinned prompts: sum(P) = %d tokens, %d prompt ticks (precondition >= %d); prompt phase: %lld ticks over %.2f s, mean %.1f Hz (precondition >= %.0f Hz); M-61 witness: peak outstanding tick jobs %d; applied entries counted by the device term %d"),
		PromptTokenSum, PromptTicks, kMinPromptTicks, PromptPhaseTicks, PromptPhaseWallSeconds, PromptPhaseRateHz, kMinPromptTickRateHz, MaxOutstandingTickJobs, AppliedDeviceTermEntries));
	AddInfo(FString::Printf(TEXT("(i): %d of %d qualifying ticks under %.0f ms (%.1f %%); longest qualifying tick %.2f ms; longest Tick() of all %.2f ms"),
		FastConstructionTicks, TimedConstructionTicks, kTickBoundMs, FastConstructionFraction * 100.0, MaxConstructionTickMs, MaxTickMs));
	AddInfo(FString::Printf(TEXT("(vi): vended at %d outstanding tick jobs; SetSchema() %.2f ms, GetSchemaWalkState() %.2f ms (returned %u); bound reported at %.1f s"),
		OutstandingAtVend, SetSchemaMs, WalkStateMs, WalkStateAtVend, bBoundSeen ? BoundSeenAtSeconds : -1.0));

	TestTrue(*FString::Printf(TEXT("the arm finishes before its %.0f s wall deadline"), kArmDeadlineSeconds), bAllTerminal);
	TestTrue(TEXT("both outstanding job counts reach 0 before the hitch counts are read"), bDrained);
	TestTrue(*FString::Printf(TEXT("the plan log holds one entry per Tick() while logging is on (%d entries, %d ticks)"), Plan.Num(), TickWallMs.Num()), bPlanMatchesTicks);

	// (i) the tick never waits, timed where M-58 would wait for at least 2D = 200 ms on every such
	// tick; a percentile, so one OS preemption does not fail a correct build (round 3, finding 10).
	TestTrue(*FString::Printf(TEXT("(i) at least %.0f %% of the Tick()s that examined an event unfinished with >= %d tick jobs ahead return in under %.0f ms (%d of %d; longest %.2f ms)"),
		kTickPercentile * 100.0, kConstructionJobsAhead, kTickBoundMs, FastConstructionTicks, TimedConstructionTicks, MaxConstructionTickMs),
		TimedConstructionTicks > 0 && FastConstructionFraction >= kTickPercentile);

	// (ii) the construction bit. Derived: once the prompts have queued, the cap holds about K tick
	// jobs outstanding and the device finishes one per >= D = 100 ms, so an event's job sits behind
	// j ~ K jobs of 100 ms each (~4.8 s) against its K ticks of harness time (~0.8 s at 60 Hz), and
	// is unfinished at its apply tick with many jobs ahead.
	TestTrue(*FString::Printf(TEXT("(ii) at least one tick examined an event unfinished with >= %d tick jobs ahead"), kConstructionJobsAhead), ConstructionTicks.Num() >= 1);
	TestTrue(TEXT("(ii) at least one examined entry that was later applied, not dropped, is counted by the device term"), AppliedDeviceTermEntries >= 1);

	// (iii) tokens, none early, order.
	for (int32 I = 0; I < kSequences; ++I)
	{
		TestEqual(*FString::Printf(TEXT("(iii) sequence %d completes"), I), (uint8)GpuPhases[I], (uint8)ESuperSLMSequencePhase::Complete);
		TArray<int32> CpuTokens;
		if (RunCpuReference(*this, *Cpu, Prompts[I], kMaxNewTokens, CpuTokens))
		{
			TestEqual(*FString::Printf(TEXT("(iii) sequence %d: GPU tokens equal the CPU backend's for its prompt"), I), GpuTokens[I], CpuTokens);
		}
	}
	TestTrue(TEXT("the event log recorded the run's events"), V.Events > 0);
	TestEqual(*FString::Printf(TEXT("(iii) no event applies before its own apply tick (first: %s)"), *V.FirstEarly), V.EarlyApplications, 0);
	TestEqual(*FString::Printf(TEXT("(iii) applied ticks never decrease within a sequence (first: %s)"), *V.FirstOrder), V.OrderViolations, 0);

	// (iv) the game-side hitch count equals the log's.
	TestEqual(TEXT("(iv) GetGameHitchCount()'s increase since the logs were enabled equals the test's own count from the event log"), GameHitchIncrease, V.ExpectedHitches());

	// (v) back-pressure at token starts.
	TestTrue(TEXT("(v) the plan log recorded the run's ticks"), Plan.Num() > 0);
	TestEqual(*FString::Printf(TEXT("(v) no tick starts a token for a sequence with an unremoved event past its apply tick (first: %s)"), *V.FirstBackPressure), V.BackPressureViolations, 0);

	// (vi) SetSchema() and GetSchemaWalkState() never wait.
	if (TestTrue(*FString::Printf(TEXT("(vi) the run reached %d outstanding tick jobs, and the free slot was vended then"), kVendAtTickJobs), bFreeVended && FreeSeq.IsValid()))
	{
		TestTrue(*FString::Printf(TEXT("(vi) SetSchema() returns true (%s)"), *SetSchemaError), bSetSchemaReturned);
		TestTrue(*FString::Printf(TEXT("(vi) SetSchema() returns in under %.0f ms (took %.2f ms)"), kCallBoundMs, SetSchemaMs), SetSchemaMs < kCallBoundMs);
		TestTrue(*FString::Printf(TEXT("(vi) GetSchemaWalkState() returns in under %.0f ms (took %.2f ms)"), kCallBoundMs, WalkStateMs), WalkStateMs < kCallBoundMs);
		TestTrue(*FString::Printf(TEXT("(vi) GetBoundSchema() reports the schema before the %.0f s deadline"), kArmDeadlineSeconds), bBoundSeen);
	}

	// (vii) the hard queue bound, read only once its preconditions hold: enough prompt ticks (round 2,
	// finding 5), and a mean prompt-phase rate of at least 25 Hz (round 3, finding 11), so that
	// M-61's uncapped queue passes 240 by construction: 450 x (1 - 10/25) = 270 > 240.
	const bool bPromptTicksHold = TestTrue(*FString::Printf(TEXT("(vii) precondition: sum(P) x NumHiddenLayers / LayersPerTick >= %d prompt ticks (%d)"), kMinPromptTicks, PromptTicks),
		PromptTicks >= kMinPromptTicks);
	const bool bPromptRateHolds = TestTrue(*FString::Printf(TEXT("(vii) precondition: the prompt phase's mean tick rate is >= %.0f Hz (%.1f Hz: %lld ticks over %.2f s)"),
		kMinPromptTickRateHz, PromptPhaseRateHz, PromptPhaseTicks, PromptPhaseWallSeconds), PromptPhaseRateHz >= kMinPromptTickRateHz);
	if (bPromptTicksHold && bPromptRateHolds)
	{
		TestEqual(*FString::Printf(TEXT("(vii) after every Tick(), GetOutstandingTickJobCount() <= K + BlockCount x NumHiddenLayers = %d (largest %d)"), HardQueueBound, MaxOutstandingTickJobs),
			QueueBoundViolations, 0);
	}

	return !HasAnyErrors();
}

// --- R-S2a stall arm (plan §9 R-S2a, review finding 4, restated by round 2's findings 3, 4 and
// 11 and round 3's findings 8 and 9; kills M-24, M-57, M-62) ---
// One composed sequence, slice = LayersPerTick = 4 (a token is 6 slices on 6 consecutive ticks),
// BlockCount 8, K = 52, MaxNewTokens 16, the harness pacing (DeltaSeconds 1/120, ticks >= 1/60 s of
// wall time apart), logs on, no submission delay. e is the tick at which the first decode token
// with slices embeds (the token after the primed finish-only step); t0 = e + 5 issues that token's
// finishing slice. Before every Tick() up to and including t0 the harness sleeps 1 ms at a time,
// without ticking, until both outstanding job counts read 0 (round 3, finding 8), so every job
// before t0 is done when t0 is enqueued. HoldNextTickJob() immediately before Tick() t0 holds job
// t0 after its call, outside the Layer-1 lock, on a test-released gate; ReleaseHeldTickJob() once
// the plan log has an entry for t0 + 49, some Tick() that started >= 10.5 s after that token's
// WallAtExamination has returned, and at least 11 s have passed since WallAtExamination (round 3,
// finding 9: a tick is then guaranteed inside M-24's window). That token's apply tick is t0 + 47;
// at t0 + 47 the sequence is in flight and held, at t0 + 49 it is held at a boundary with exactly
// 49 tick jobs outstanding, below K.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2FrameContractStallTest,
	"SuperSLM.L2S2.FrameContract.Stall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2FrameContractStallTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	constexpr int32 kBlockCount = 8;
	constexpr int32 kK = 52;                      // >= MinimumK 48; aligns the apply tick off a boundary (§9)
	constexpr int32 kMaxNewTokens = 16;
	constexpr double kHeldPastExaminationSeconds = 11.0; // the 10 s bound + 1 s; M-24 faults at 10 s
	constexpr double kWitnessTickAfterExaminationSeconds = 10.5; // a tick inside M-24's window (round 3, finding 9)
	constexpr int32 kSlicesPerToken = 6;          // 24 layers / 4 per slice
	constexpr int32 kReleaseAfterTicks = 49;      // t0 + 49, the held boundary
	constexpr double kArmDeadlineSeconds = 120.0;

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	USuperSLMSubsystem* Cpu = nullptr;
	if (!SetUpGpuAndCpu(*this, World, kBlockCount, Model, Gpu, Cpu, kK))
	{
		return false;
	}

	const FString Prompt = TEXT("Tell me something about the number 1.");
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("Tokenize() must succeed"), Cpu->Tokenize(Prompt, Request.PromptTokens)))
	{
		return false;
	}
	Request.MaxNewTokens = kMaxNewTokens;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	const int32 PromptTokenCount = Request.PromptTokens.Num();

	TestTrue(TEXT("the logs are enabled while no event is pending"), FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, true)); // no submission delay in this arm: the gate is the stall
	const int32 GameHitchesBefore = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu);

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
		return false;
	}
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() (%s)"), *Gpu->GetLastLifecycleRequestError()), Gpu->RequestBeginGeneration(Seq, Request).IsValid()))
	{
		Gpu->ReturnSequence(Seq);
		FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
		return false;
	}

	// e: the (PromptTokenCount + 2)-th tick at which the sequence starts a token -- every prompt
	// token's embed, then the primed finish-only step, then the first decode token with slices.
	// Read online from the plan log's last entry until found; after that the TickIndex of the next
	// Tick() is the last read one plus the ticks since.
	int64 E = -1;
	int64 T0 = -1;
	int64 LastKnownTick = -1;
	int32 StartedCount = 0;
	bool bHoldIssued = false;
	bool bReleased = false;
	bool bActiveThroughout = true;
	int32 Ticks = 0;
	const double ArmStart = FPlatformTime::Seconds();
	double LastTickStart = ArmStart - kHarnessWallStepSeconds;
	double HoldTickStartSeconds = 0.0;
	double ReleasedAtSeconds = 0.0;
	bool bHoldArmed = false;
	bool bPreT0DrainTimedOut = false;
	TArray<double> TickStartSeconds; // each Tick()'s start, appended once it has returned
	ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
	while (FPlatformTime::Seconds() - ArmStart < kArmDeadlineSeconds)
	{
		// Up to and including t0: sleep without ticking until nothing is outstanding (round 3,
		// finding 8). Not ticking keeps the tick alignment.
		if (T0 < 0 || LastKnownTick + 1 <= T0)
		{
			while (FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(*Gpu) + FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(*Gpu) > 0)
			{
				if (FPlatformTime::Seconds() - ArmStart >= kArmDeadlineSeconds)
				{
					bPreT0DrainTimedOut = true;
					break;
				}
				FPlatformProcess::Sleep(0.001f);
			}
			if (bPreT0DrainTimedOut)
			{
				break;
			}
		}
		if (T0 >= 0 && !bHoldIssued && LastKnownTick + 1 == T0)
		{
			bHoldArmed = FSuperSLMGpuTestAccess::HoldNextTickJob(*Gpu); // immediately before Tick() t0
			HoldTickStartSeconds = FPlatformTime::Seconds();
			bHoldIssued = true;
		}
		PacedHarnessTick(*Gpu, LastTickStart);
		TickStartSeconds.Add(LastTickStart);
		++Ticks;
		bActiveThroughout &= Gpu->IsGpuBackendActive();

		if (E < 0)
		{
			const TArray<FSuperSLMGpuPlanLogEntry> PlanSoFar = FSuperSLMGpuTestAccess::GetPlanLog(*Gpu);
			if (PlanSoFar.Num() > 0)
			{
				const FLoggedPlanTick Last = CopyPlanTick(PlanSoFar.Last());
				LastKnownTick = Last.Tick;
				if (Last.StartedSequenceIds.Contains(Seq.Id) && ++StartedCount == PromptTokenCount + 2)
				{
					E = Last.Tick;
					T0 = E + kSlicesPerToken - 1;
				}
			}
		}
		else
		{
			++LastKnownTick;
		}

		// The release: once the plan log has an entry for t0 + 49, a Tick() that started at least
		// 10.5 s after the stalled token's (the one started at e) examination has returned, and at
		// least 11 s have passed since that examination.
		if (bHoldIssued && !bReleased && LastKnownTick >= T0 + kReleaseAfterTicks)
		{
			TArray<FLoggedEvent> EventsSoFar;
			TArray<FLoggedPlanTick> PlanSoFar;
			ReadApplyLogs(*Gpu, EventsSoFar, PlanSoFar);
			const bool bHasT0Plus49 = FindPlanTick(PlanSoFar, T0 + kReleaseAfterTicks) != nullptr;
			for (const FLoggedEvent& Ev : EventsSoFar)
			{
				if (Ev.SequenceId == Seq.Id && !Ev.bPrefill && Ev.RequestTick == E)
				{
					const bool bWitnessTickReturned = Ev.bExamined && TickStartSeconds.Num() > 0
						&& TickStartSeconds.Last() >= Ev.WallAtExamination + kWitnessTickAfterExaminationSeconds;
					if (bHasT0Plus49 && bWitnessTickReturned && FPlatformTime::Seconds() - Ev.WallAtExamination >= kHeldPastExaminationSeconds)
					{
						FSuperSLMGpuTestAccess::ReleaseHeldTickJob(*Gpu);
						ReleasedAtSeconds = FPlatformTime::Seconds();
						bReleased = true;
					}
					break;
				}
			}
		}

		Phase = Gpu->GetPhase(Seq);
		if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
		{
			break;
		}
	}
	const double ArmWallSeconds = FPlatformTime::Seconds() - ArmStart;
	const TArray<int32> GpuTokens = Gpu->GetGeneratedTokens(Seq);
	if (bHoldIssued && !bReleased)
	{
		FSuperSLMGpuTestAccess::ReleaseHeldTickJob(*Gpu); // the deadline passed with the gate held; never leave it held
	}

	const bool bDrained = DrainOutstandingJobs(*Gpu, 60.0);
	const int32 GameHitchIncrease = FSuperSLMGpuTestAccess::GetGameHitchCount(*Gpu) - GameHitchesBefore;
	TArray<FLoggedEvent> Events;
	TArray<FLoggedPlanTick> Plan;
	ReadApplyLogs(*Gpu, Events, Plan);
	FSuperSLMGpuTestAccess::SetEventLogEnabled(*Gpu, false);
	Gpu->ReturnSequence(Seq);

	const FApplyLogVerdict V = EvaluateApplyLog(Events, Plan, kK);
	AddInfo(FString::Printf(TEXT("stall arm: prompt %d tokens; e = %lld, t0 = %lld, hold issued %d, released %d (%.2f s after Tick() t0 started); %d ticks, %.1f s wall; game hitches %d (log %d); %d tokens, phase %d"),
		PromptTokenCount, E, T0, bHoldIssued ? 1 : 0, bReleased ? 1 : 0, bReleased ? ReleasedAtSeconds - HoldTickStartSeconds : -1.0,
		Ticks, ArmWallSeconds, GameHitchIncrease, V.ExpectedHitches(), GpuTokens.Num(), (int32)Phase));

	TestFalse(TEXT("the pre-t0 drains (both outstanding counts 0 before each tick up to t0) finished before the arm deadline"), bPreT0DrainTimedOut);
	if (!TestTrue(TEXT("the construction identified e and held job t0 = e + 5"), E >= 0 && bHoldIssued))
	{
		return false;
	}
	TestTrue(TEXT("HoldNextTickJob() returned true (armed) before Tick() t0"), bHoldArmed);
	TestTrue(TEXT("the gate was released by the construction's own rule (t0 + 49 logged, >= 11 s past the examination), not by the deadline"), bReleased);
	TestTrue(TEXT("both outstanding job counts reach 0 before the hitch counts are read"), bDrained);

	// The construction's own preconditions: the start before e was the finish-only step (no slice
	// in flight the tick after it), and the token started at e is in flight on e + 1 .. e + 5.
	{
		int64 PreviousStart = -1;
		for (const FLoggedPlanTick& P : Plan)
		{
			if (P.Tick < E && P.StartedSequenceIds.Contains(Seq.Id))
			{
				PreviousStart = P.Tick;
			}
		}
		const FLoggedPlanTick* AfterPrevious = FindPlanTick(Plan, PreviousStart + 1);
		TestTrue(*FString::Printf(TEXT("construction: the start before e (tick %lld) is the finish-only step, with nothing in flight after it"), PreviousStart),
			PreviousStart >= 0 && AfterPrevious != nullptr && !AfterPrevious->InFlightAtPlanSequenceIds.Contains(Seq.Id));
		for (int64 Tick = E + 1; Tick <= T0; ++Tick)
		{
			const FLoggedPlanTick* P = FindPlanTick(Plan, Tick);
			TestTrue(*FString::Printf(TEXT("construction: the token started at e is in flight at tick %lld"), Tick),
				P != nullptr && P->InFlightAtPlanSequenceIds.Contains(Seq.Id));
		}
	}

	// (a) the construction: every event created before tick t0 was done at its examination; the
	// token started at e is examined unfinished and held >= 11 s past its examination; at t0 + 47
	// the sequence is in flight and held; at t0 + 49 it is held at a boundary, cap not binding.
	{
		int32 StalledIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Events.Num(); ++Index)
		{
			const FLoggedEvent& Ev = Events[Index];
			if (Ev.SequenceId == Seq.Id && !Ev.bPrefill && Ev.RequestTick == E)
			{
				StalledIndex = Index;
				break;
			}
		}
		if (TestTrue(TEXT("(a) the event log holds the token started at e"), StalledIndex != INDEX_NONE))
		{
			// The log is in planning order, and the stalled token's event is created at t0, so the
			// entries before it are exactly the events created before tick t0 (one sequence).
			int32 EarlierNotDone = 0;
			int32 FirstEarlierNotDone = INDEX_NONE;
			for (int32 Index = 0; Index < StalledIndex; ++Index)
			{
				if (!(Events[Index].bExamined && Events[Index].bJobDoneAtExamination) && EarlierNotDone++ == 0)
				{
					FirstEarlierNotDone = Index;
				}
			}
			TestEqual(*FString::Printf(TEXT("(a) every event created before tick t0 reads bJobDoneAtExamination true (%d earlier events; first not: log entry %d)"), StalledIndex, FirstEarlierNotDone),
				EarlierNotDone, 0);
			const FLoggedEvent* Stalled = &Events[StalledIndex];
			AddInfo(FString::Printf(TEXT("(a) stalled token: apply tick %lld (t0 + %lld), examined %d, done at examination %d, removed at tick %lld, %.2f s from examination to removal, %.2f s from Tick() t0 to removal"),
				Stalled->ApplyTick, Stalled->ApplyTick - T0, Stalled->bExamined ? 1 : 0, Stalled->bJobDoneAtExamination ? 1 : 0, Stalled->RemovedTick,
				Stalled->RemovedWallSeconds - Stalled->WallAtExamination, Stalled->RemovedWallSeconds - HoldTickStartSeconds));
			TestTrue(TEXT("(a) the stalled token was examined with its job unfinished"), Stalled->bExamined && !Stalled->bJobDoneAtExamination);
			TestTrue(*FString::Printf(TEXT("(a) RemovedWallSeconds - WallAtExamination >= %.0f s"), kHeldPastExaminationSeconds),
				Stalled->RemovedTick >= 0 && Stalled->RemovedWallSeconds - Stalled->WallAtExamination >= kHeldPastExaminationSeconds);
		}
		const FLoggedPlanTick* At47 = FindPlanTick(Plan, T0 + 47);
		TestTrue(TEXT("(a) at t0 + 47 the sequence is in InFlightAtPlanSequenceIds and HeldSequenceIds"),
			At47 != nullptr && At47->InFlightAtPlanSequenceIds.Contains(Seq.Id) && At47->HeldSequenceIds.Contains(Seq.Id));
		const FLoggedPlanTick* At49 = FindPlanTick(Plan, T0 + 49);
		TestTrue(TEXT("(a) at t0 + 49 the sequence is held at a boundary with bCapBinding false"),
			At49 != nullptr && At49->HeldSequenceIds.Contains(Seq.Id) && !At49->InFlightAtPlanSequenceIds.Contains(Seq.Id) && !At49->bCapBinding);
	}

	// (b) no device loss; the sequence completes with the CPU's tokens.
	TestTrue(TEXT("(b) IsGpuBackendActive() stays true throughout"), bActiveThroughout);
	TestEqual(TEXT("(b) the sequence reaches Complete"), (uint8)Phase, (uint8)ESuperSLMSequencePhase::Complete);
	{
		TArray<int32> CpuTokens;
		if (RunCpuReference(*this, *Cpu, Prompt, kMaxNewTokens, CpuTokens))
		{
			TestEqual(TEXT("(b) GPU tokens equal the CPU backend's"), GpuTokens, CpuTokens);
		}
	}

	// (c) an in-flight token is never held: every tick with the sequence in flight at Plan's start
	// issued for it.
	{
		int32 InFlightNotIssued = 0;
		int64 FirstInFlightNotIssued = -1;
		for (const FLoggedPlanTick& P : Plan)
		{
			if (P.InFlightAtPlanSequenceIds.Contains(Seq.Id) && !P.IssuedSequenceIds.Contains(Seq.Id) && InFlightNotIssued++ == 0)
			{
				FirstInFlightNotIssued = P.Tick;
			}
		}
		TestEqual(*FString::Printf(TEXT("(c) every tick with the sequence in flight at Plan's start issues for it (first miss at tick %lld)"), FirstInFlightNotIssued), InFlightNotIssued, 0);
	}

	// (d) back-pressure, (v) of the slowed-device arm.
	TestEqual(*FString::Printf(TEXT("(d) no tick starts a token for a sequence with an unremoved event past its apply tick (first: %s)"), *V.FirstBackPressure), V.BackPressureViolations, 0);

	// (e) the game-side hitch count, (iv) of the slowed-device arm.
	TestEqual(TEXT("(e) GetGameHitchCount()'s increase since the logs were enabled equals the test's own count from the event log"), GameHitchIncrease, V.ExpectedHitches());

	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
