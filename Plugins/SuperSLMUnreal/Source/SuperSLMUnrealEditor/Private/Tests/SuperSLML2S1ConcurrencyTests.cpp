// T-2805 -- L2-S1 red suite. Plan cell R-S1d (the plan §9):
// "Concurrent NPCs do not corrupt each other" (dim 3) -- "A-EX, 4 worker threads, one
// workspace each -> tokens identical to the serial run. One sequence released mid-batch yields
// -3, the tick loop terminates within a bounded tick count, and the rest complete."
//
// "4 worker threads, one workspace each" is the subsystem's OWN internal implementation (§4:
// "One workspace per concurrently active call. The workspace pool is sized to the number of
// concurrent worker threads"), driven from this test's single (game) thread by running 4
// sequences concurrently through the ordinary Tick() loop -- exactly the shape a real game uses
// (several NPCs ticking in the same frame), never a raw std::thread stress harness the product
// itself does not run. `MaxConcurrentCalls` now defaults to 1 (D-SLM7341, T-2785 fold 4 --
// batched decode serves many sequences in one call on one lane by default, which R-S1b relies
// on), so THIS cell -- whose whole claim is about real concurrent lanes -- sets it to 4
// explicitly. Needs the tokenizer-bearing A-EX rebuild (D-SLM7339); fails the cell by name (T-2805
// L23 repair) when it is absent, never a silent pass.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	// Three-way, not a bool: "A-EX is absent" (T-2805 L23 repair: fails the cell by name via
	// AddError, never a silent skip) and "A-EX is present but failed to import" (a genuine test
	// failure) are kept distinct only so each fails the cell for its own diagnosable reason.
	enum class EAExAvailability { Absent, ImportFailed, Ready };

	EAExAvailability ImportAEx(FAutomationTestBase& T, const TCHAR* CellName, USuperSLMModel*& OutModel)
	{
		FString AExPath, Reason;
		if (!TryGetAExArtifactPath(AExPath, Reason))
		{
			T.AddError(FString::Printf(TEXT("%s: %s"), CellName, *Reason));
			return EAExAvailability::Absent;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), OutModel) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return EAExAvailability::ImportFailed;
		}
		return EAExAvailability::Ready;
	}

	bool ConfigureFor(FAutomationTestBase& T, UWorld* World, USuperSLMModel* Model, USuperSLMSubsystem*& OutSubsystem, int32 BlockCount)
	{
		OutSubsystem = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), OutSubsystem))
		{
			return false;
		}
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = BlockCount;
		Config.MaxPrefillChunkBudget = 32;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = BlockCount;
		// D-SLM7341: MaxConcurrentCalls defaults to 1 (one lane). This file's whole claim is
		// concurrent lanes, so it asks for real ones -- one per sequence.
		Config.MaxConcurrentCalls = BlockCount;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		const FSuperSLMConfigureReport Report = OutSubsystem->Configure(Model, Config);
		return T.TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	const TCHAR* kPrompts[4] = {
		TEXT("Customer one: what's the wait time today?"),
		TEXT("Customer two: can I see the flash sheet?"),
		TEXT("Customer three: how much for a small line piece?"),
		TEXT("Customer four: do you take walk-ins?"),
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1ConcurrentMatchesSerialTest,
	"SuperSLM.L2S1.Concurrency.FourConcurrentSequencesMatchSerial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1ConcurrentMatchesSerialTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1d FourConcurrentSequencesMatchSerial"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}
	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), Model, Subsystem, /*BlockCount*/ 4))
	{
		return false;
	}

	constexpr int32 N = 4;
	constexpr int32 MaxNewTokens = 32;

	// --- Serial baseline: one sequence at a time, fresh handle each ---
	TArray<TArray<int32>> SerialTokens;
	SerialTokens.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		FSuperSLMSequence Seq;
		if (!TestEqual(*FString::Printf(TEXT("Vend serial sequence %d"), I), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		TArray<int32> PromptTokens;
		Subsystem->Tokenize(kPrompts[I], PromptTokens);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = MaxNewTokens;
		FString RunError;
		const bool bSerialCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, SerialTokens[I], /*MaxWallClockSeconds*/ 60.0, RunError);
		if (!TestTrue(*FString::Printf(TEXT("serial sequence %d must complete (%s)"), I, *RunError), bSerialCompleted))
		{
			return false;
		}
		Subsystem->ReturnSequence(Seq);
	}

	// --- Concurrent run: all 4 begin together, driven by the SAME Tick() calls ---
	TArray<FSuperSLMSequence> Sequences;
	Sequences.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		if (!TestEqual(*FString::Printf(TEXT("Vend concurrent sequence %d"), I), (uint8)Subsystem->VendSequence(Sequences[I]), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		TArray<int32> PromptTokens;
		Subsystem->Tokenize(kPrompts[I], PromptTokens);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = MaxNewTokens;
		FString BeginError;
		const bool bBeginOk = Subsystem->BeginGeneration(Sequences[I], Request, BeginError);
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration concurrent %d: %s"), I, *BeginError), bBeginOk))
		{
			return false;
		}
	}

	// T-2805 round 10 (§14.7): paced FastAsPossible -- a content/order cell (token identity across
	// concurrent lanes), never a timing claim. Generous in wall time rather than tick count
	// (a tick-count bound would encode a timing assumption): 4 sequences x 32 tokens is real worker-side computation.
	auto AllComplete = [Subsystem, &Sequences]()
	{
		for (const FSuperSLMSequence& Seq : Sequences)
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Seq);
			if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
			{
				return false;
			}
		}
		return true;
	};
	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, AllComplete, /*MaxWallClockSeconds*/ 120.0);
	TestTrue(TEXT("all 4 concurrent sequences reached Complete or Faulted"), AllComplete());

	bool bAllMatch = true;
	for (int32 I = 0; I < N; ++I)
	{
		const TArray<int32> ConcurrentTokens = Subsystem->GetGeneratedTokens(Sequences[I]);
		// FEAT oracle: each concurrent sequence's tokens must exactly equal that SAME prompt's
		// serial baseline. A shared-workspace aliasing bug (two sequences' calls corrupting one
		// workspace's scratch, §5) shows up as a WRONG token in exactly the sequence sharing
		// that workspace slot this tick, which this per-sequence equality check catches.
		if (!TestEqual(*FString::Printf(TEXT("concurrent sequence %d must match its own serial baseline"), I),
				ConcurrentTokens, SerialTokens[I]))
		{
			bAllMatch = false;
		}
		Subsystem->ReturnSequence(Sequences[I]);
	}
	return bAllMatch;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1ReleaseMidBatchYieldsSequenceNoLongerValidTest,
	"SuperSLM.L2S1.Concurrency.ReleaseMidBatchYieldsSequenceNoLongerValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1ReleaseMidBatchYieldsSequenceNoLongerValidTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1d ReleaseMidBatchYieldsSequenceNoLongerValid"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}
	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), Model, Subsystem, /*BlockCount*/ 4))
	{
		return false;
	}

	constexpr int32 N = 4;
	constexpr int32 MaxNewTokens = 32;
	int64 TotalPromptTokens = 0;
	TArray<FSuperSLMSequence> Sequences;
	Sequences.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		if (!TestEqual(*FString::Printf(TEXT("Vend sequence %d"), I), (uint8)Subsystem->VendSequence(Sequences[I]), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		TArray<int32> PromptTokens;
		Subsystem->Tokenize(kPrompts[I], PromptTokens);
		TotalPromptTokens += PromptTokens.Num();
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = MaxNewTokens;
		FString BeginError;
		const bool bBeginOk = Subsystem->BeginGeneration(Sequences[I], Request, BeginError);
		if (!TestTrue(*FString::Printf(TEXT("BeginGeneration %d: %s"), I, *BeginError), bBeginOk))
		{
			return false;
		}
	}

	constexpr int32 ReleasedIndex = 1;
	bool bReleasedMidBatch = false;
	bool bBoundedTermination = false;
	int32 NoLongerValidAfterTick = MAX_int32;
	// T-2805 round 10 (§14.7): a real Sleep between polls, not a bare back-to-back Tick() loop, so
	// the worker genuinely gets the wall-clock time its Layer-1 calls need between this test's own
	// checks -- a content/order cell (never a timing claim), so FastAsPossible in spirit even
	// though this loop's own release-detection logic is not a simple Done() predicate DrainTicks
	// can express directly.
	// Independent workload ceiling: at least 1 GiB/s streamed from the artifact,
	// plus 20 ms for every possible Layer-1 call. A prompt or decode token is
	// charged once for each hidden layer and twice more for embed/finish, even
	// though the configured 24-layer budget usually needs fewer calls.
	const int64 MaxTokensAcrossFixture = TotalPromptTokens + int64(N) * MaxNewTokens;
	const double StreamSeconds = double(Model->GetMappedArtifactSize()) * double(MaxTokensAcrossFixture) / 1073741824.0;
	const double CallSeconds = double(MaxTokensAcrossFixture) * (24.0 + 2.0) * 0.020;
	const double WallCeilingSeconds = StreamSeconds + CallSeconds;
	const double DrainStartSeconds = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - DrainStartSeconds < WallCeilingSeconds)
	{
		Subsystem->Tick(1.0f / 60.0f);

		// Release sequence 1 while every sequence is still mid-generation (not yet Complete).
		if (!bReleasedMidBatch && Subsystem->GetPhase(Sequences[ReleasedIndex]) == ESuperSLMSequencePhase::Decoding)
		{
			Subsystem->ReturnSequence(Sequences[ReleasedIndex]);
			bReleasedMidBatch = true;
			NoLongerValidAfterTick = Subsystem->GetLastTickReport().TickIndex;

			// "One sequence released mid-batch yields -3" (§9 R-S1d): the very next tick's
			// attempt to advance the now-released handle reports the ABI's own -3 sentinel by
			// name, never a bare int and never silently re-queued.
			Subsystem->Tick(1.0f / 60.0f);
			TestEqual(TEXT("the released sequence's next decode outcome must be SequenceNoLongerValid (-3)"),
				(uint8)Subsystem->GetLastDecodeOutcome(Sequences[ReleasedIndex]),
				(uint8)ESuperSLMDecodeOutcome::SequenceNoLongerValid);
		}

		bool bOthersDone = true;
		for (int32 I = 0; I < N; ++I)
		{
			if (I == ReleasedIndex)
			{
				continue;
			}
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(Sequences[I]);
			if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
			{
				bOthersDone = false;
			}
		}
		if (bReleasedMidBatch && bOthersDone)
		{
			bBoundedTermination = true;
			break;
		}
		// Give the worker real wall-clock time to make progress before the next poll (§14.7's own
		// fix) -- only while genuinely still waiting, matching Fixtures.h's own DrainTicks.
		FPlatformProcess::Sleep(0.001f);
	}

	if (!TestTrue(TEXT("sequence 1 must have been released while still mid-generation"), bReleasedMidBatch))
	{
		return false;
	}
	// The released sequence's -3 outcome (SequenceNoLongerValid) must never be re-queued;
	// completion is bounded by the workload-derived wall-clock deadline above.
	if (!TestTrue(*FString::Printf(TEXT("the tick loop must terminate within the workload-derived %.3f-second ceiling"), WallCeilingSeconds), bBoundedTermination))
	{
		return false;
	}
	bool bNoPostAfterNoLongerValid = true;
	for (const FSuperSLMWorkerJobReport& Job : Subsystem->GetJobLedger())
	{
		if (Job.PlannedAtTick <= NoLongerValidAfterTick) { continue; }
		for (const FSuperSLMSequence& Member : Job.MemberSequences)
		{
			if (Member.Id == Sequences[ReleasedIndex].Id)
			{
				AddError(FString::Printf(TEXT("job %lld posted for released sequence %lld at tick %d, after -3 at tick %d"),
					Job.JobId, Member.Id, Job.PlannedAtTick, NoLongerValidAfterTick));
				bNoPostAfterNoLongerValid = false;
			}
		}
	}

	int32 CompletedCount = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (I == ReleasedIndex)
		{
			continue;
		}
		if (Subsystem->GetPhase(Sequences[I]) == ESuperSLMSequencePhase::Complete)
		{
			++CompletedCount;
		}
		Subsystem->ReturnSequence(Sequences[I]);
	}
	// "the rest complete" (§9 R-S1d): the 3 sequences never released must all reach Complete,
	// unaffected by sequence 1's mid-batch release.
	return bNoPostAfterNoLongerValid && TestEqual(TEXT("the 3 non-released sequences must all complete"), CompletedCount, N - 1);
}

#endif // WITH_DEV_AUTOMATION_TESTS
