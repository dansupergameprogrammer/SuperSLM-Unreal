// A GPU sequence's queue removes resolved entries behind its front, so a sequence held across many
// requests keeps a log no longer than its queued work. Nothing is removed while a generation runs:
// the running generation's own entry, which the pending count reads, and every entry resolved
// behind it stay, and no index moves. They are removed the first time an entry resolves after the
// generation has ended.
//
// OpLogCompactionKeepsCountsAndResults: one sequence, held for six cycles of generate; while it
// runs, a save, driven to resolution; the generation completes; a reset, driven to resolution.
// That is eighteen requests against a log reserved for six. On every tick,
// GetPendingLifecycleOperationCount() must read what the queue holds:
//   - 1 while the generation runs with nothing pending behind it (before the save is requested,
//     and after it has resolved);
//   - 2 while the save is pending behind the running generation;
//   - 0 once the generation is Complete.
// Every handle, read again after all six cycles, still reads Success: a result outlives its entry.
//
// The removal itself is asserted through the test-access entry count, read on every tick and after
// every request:
//   - it never exceeds 3 -- the generation's entry and the resolved save, both kept while it runs,
//     then the reset queued behind them -- inside the 4 + 2 the log is reserved for;
//   - it reads 0 once each cycle's reset has resolved, so nothing is carried into the next cycle.
// A log that is never compacted holds 4 entries once the second cycle's generation is requested,
// and 18 by the end, so the bound fails in the second cycle.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	constexpr double kCompactionCellDeadlineSeconds = 120.0;

	FSuperSLMGenerationRequest MakeCompactionRequest(const TArray<int32>& Prompt, int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		return Request;
	}

	bool IsRunningPhase(ESuperSLMSequencePhase Phase)
	{
		return Phase == ESuperSLMSequencePhase::Prefilling || Phase == ESuperSLMSequencePhase::Decoding;
	}

	// Paced ticks until Done() or the deadline; true when Done() held.
	template <typename FDone>
	bool TickCompactionCellUntil(USuperSLMGpuSubsystem& Gpu, FDone Done)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done())
		{
			if (FPlatformTime::Seconds() - Start > kCompactionCellDeadlineSeconds || !TickWhenDeviceReady(Gpu))
			{
				return Done();
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuOpLogCompactionKeepsCountsAndResultsTest,
	"SuperSLM.U1.Gpu.OpLogCompactionKeepsCountsAndResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuOpLogCompactionKeepsCountsAndResultsTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
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
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // one-call path; a whole token's worth
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Config.MaxQueuedOperationsPerSequence = 4; // the log is reserved for 4 + 2 entries
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	constexpr int32 kCycles = 6;
	// The most entries the log may hold at any observation: the generation's kept entry and the
	// resolved save kept behind it, then the reset queued behind them.
	constexpr int32 kMaxOpLogEntries = 3;
	const FSuperSLMGenerationRequest Generation = MakeCompactionRequest({1, 2, 3}, 24);
	TArray<FSuperSLMLifecycleOpHandle> Handles;
	bool bOk = true;
	// Ticks observed with a pending count other than the one the queue's state gives.
	int32 WrongWhileRunsAlone = 0;
	int32 WrongWhileSavePending = 0;
	int32 TicksRunningAloneObserved = 0;
	int32 TicksSavePendingObserved = 0;
	int32 CyclesNotZeroAtComplete = 0;
	// The log's length, read on every tick and after every request, and the cycles whose log was
	// not empty once the reset had resolved.
	int32 MaxOpLogEntries = 0;
	int32 CyclesWithLogLeftAfterReset = 0;
	const auto ReadOpLog = [Gpu, &Seq, &MaxOpLogEntries]()
	{
		const int32 Count = FSuperSLMGpuTestAccess::GetOpLogEntryCount(*Gpu, Seq);
		MaxOpLogEntries = FMath::Max(MaxOpLogEntries, Count);
		return Count;
	};
	// One reading while the generation runs and no save is pending behind it.
	const auto ReadRunningAlone = [Gpu, &Seq, &WrongWhileRunsAlone, &TicksRunningAloneObserved]()
	{
		if (IsRunningPhase(Gpu->GetPhase(Seq)))
		{
			++TicksRunningAloneObserved;
			WrongWhileRunsAlone += Gpu->GetPendingLifecycleOperationCount(Seq) != 1 ? 1 : 0;
		}
	};

	for (int32 Cycle = 0; Cycle < kCycles && bOk; ++Cycle)
	{
		// 1. The generation, admitted at its request; 1 while it runs with nothing behind it.
		const FSuperSLMLifecycleOpHandle HGeneration = Gpu->RequestBeginGeneration(Seq, Generation);
		Handles.Add(HGeneration);
		ReadOpLog();
		bOk &= TestEqual(*FString::Printf(TEXT("cycle %d: the generation is admitted"), Cycle),
			(uint8)DriveLifecycleOpToResolution(*Gpu, HGeneration), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestTrue(*FString::Printf(TEXT("cycle %d: the generation reaches Decoding with a token applied"), Cycle),
			TickCompactionCellUntil(*Gpu, [Gpu, &Seq, &ReadOpLog, &ReadRunningAlone]()
			{
				ReadOpLog();
				ReadRunningAlone();
				return Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Decoding && Gpu->GetGeneratedTokens(Seq).Num() >= 1;
			}));

		// 2. A save while it runs, driven to resolution; 2 while the save is pending (the running
		// generation and the save).
		const FSuperSLMLifecycleOpHandle HSave = Gpu->RequestSaveSequence(Seq);
		Handles.Add(HSave);
		ReadOpLog();
		bOk &= TestTrue(*FString::Printf(TEXT("cycle %d: the save is accepted"), Cycle), HSave.IsValid());
		TickCompactionCellUntil(*Gpu, [Gpu, &Seq, &HSave, &ReadOpLog, &WrongWhileSavePending, &TicksSavePendingObserved]()
		{
			ReadOpLog();
			if (Gpu->GetLifecycleOpResult(HSave) != ESuperSLMRestoreResult::Pending)
			{
				return true;
			}
			if (IsRunningPhase(Gpu->GetPhase(Seq)))
			{
				++TicksSavePendingObserved;
				WrongWhileSavePending += Gpu->GetPendingLifecycleOperationCount(Seq) != 2 ? 1 : 0;
			}
			return false;
		});
		bOk &= TestEqual(*FString::Printf(TEXT("cycle %d: the save resolves Success"), Cycle),
			(uint8)Gpu->GetLifecycleOpResult(HSave), (uint8)ESuperSLMRestoreResult::Success);

		// 3. The generation completes; 1 on every running tick after the save resolved, and 0 once
		// it is Complete.
		bOk &= TestTrue(*FString::Printf(TEXT("cycle %d: the generation completes"), Cycle),
			TickCompactionCellUntil(*Gpu, [Gpu, &Seq, &ReadOpLog, &ReadRunningAlone]()
			{
				ReadOpLog();
				ReadRunningAlone();
				return Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Complete || Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Faulted;
			}));
		bOk &= TestEqual(*FString::Printf(TEXT("cycle %d: the generation ends Complete"), Cycle),
			(uint8)Gpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete);
		CyclesNotZeroAtComplete += Gpu->GetPendingLifecycleOperationCount(Seq) != 0 ? 1 : 0;

		// 4. A reset, driven to resolution; nothing pending after it, and the log empty.
		const FSuperSLMLifecycleOpHandle HReset = Gpu->RequestResetSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
		Handles.Add(HReset);
		ReadOpLog();
		bOk &= TestEqual(*FString::Printf(TEXT("cycle %d: the reset succeeds"), Cycle),
			(uint8)DriveLifecycleOpToResolution(*Gpu, HReset), (uint8)ESuperSLMRestoreResult::Success);
		const int32 AfterReset = ReadOpLog();
		CyclesWithLogLeftAfterReset += AfterReset != 0 ? 1 : 0;
		AddInfo(FString::Printf(TEXT("cycle %d: %d log entries once the reset resolved"), Cycle, AfterReset));
		bOk &= TestEqual(*FString::Printf(TEXT("cycle %d: nothing is pending after the reset"), Cycle),
			Gpu->GetPendingLifecycleOperationCount(Seq), 0);
	}

	AddInfo(FString::Printf(TEXT("%d handles; %d ticks observed running alone, %d with the save pending; at most %d log entries"),
		Handles.Num(), TicksRunningAloneObserved, TicksSavePendingObserved, MaxOpLogEntries));
	bOk &= TestEqual(TEXT("eighteen requests were made on one held sequence"), Handles.Num(), kCycles * 3);
	bOk &= TestEqual(TEXT("while a generation runs with nothing behind it, the pending count reads 1 (ticks observed otherwise)"), WrongWhileRunsAlone, 0);
	bOk &= TestTrue(TEXT("precondition: the generations were observed running with nothing behind them"), TicksRunningAloneObserved >= kCycles);
	bOk &= TestEqual(TEXT("while a save is pending behind a running generation, the pending count reads 2 (ticks observed otherwise)"), WrongWhileSavePending, 0);
	bOk &= TestEqual(TEXT("once the generation is Complete, the pending count reads 0 (cycles otherwise)"), CyclesNotZeroAtComplete, 0);
	bOk &= TestTrue(TEXT("precondition: the log held an entry when read"), MaxOpLogEntries >= 1);
	bOk &= TestTrue(*FString::Printf(TEXT("the log stays bounded across all cycles: at most %d entries, inside the %d it is reserved for (at most %d observed)"),
		kMaxOpLogEntries, Config.MaxQueuedOperationsPerSequence + 2, MaxOpLogEntries), MaxOpLogEntries <= kMaxOpLogEntries);
	bOk &= TestEqual(TEXT("the log is empty once each cycle's reset has resolved (cycles otherwise)"), CyclesWithLogLeftAfterReset, 0);
	int32 HandlesNotSuccess = 0;
	for (const FSuperSLMLifecycleOpHandle& Handle : Handles)
	{
		HandlesNotSuccess += Gpu->GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Success ? 1 : 0;
	}
	bOk &= TestEqual(TEXT("every handle still reads Success after its entry was removed (handles otherwise)"), HandlesNotSuccess, 0);
	Gpu->ReturnSequence(Seq);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
