// Slice 1P (SuperSLM tiled-matmul plan of record §4A and §9.P): the CPU scheduler's prefill chunk
// floor, the batched prompt price, the batched calibration arm's fit and the overrun rule at a
// job's own K. The cells below are the plan's 9.P-1 to 9.P-8 and 9.P-15's nominal arm; each cell's
// comment names its row.
//
// Two fixture rules from the plan hold throughout (§9.P, rev 11.1):
//  - the precondition rule: a cell that claims "floor on" first asserts that the ledger shows a
//    prefill job of exactly 8 prompt tokens;
//  - the distinct-pair rule: a price cell runs with a batched pair that differs from the chunk-1
//    pair in both fields, since at the shipped defaults the two pairs are equal.
//
// The seam cells (FloorSeam, OverrunSeam, BatchedArmSeam) need no artifact. Every other cell
// imports A-EX and fails by name when it is absent.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformTLS.h"
#include "Misc/OutputDevice.h"
#include "SuperSLMCalibrationTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

#include <limits>

using namespace SuperSLML2S1Fixtures;

namespace
{
	constexpr int64 kAExCap = 4096;   // A-EX's context cap
	constexpr int32 kFloor = 8;       // the engine's tiled-GEMM threshold and the shipped floor

	// T-3102 (D-SLM8141): the cap-under-33 skip seam takes the mode and returns that mode's one-time
	// batched pair. Reached through a requires-expression on a dependent type, so this file compiles
	// against the old two-argument seam and the new moded one alike; false means the moded seam is
	// absent (the cell then fails by name).
	template <typename TAccess = FSuperSLMCalibrationTestAccess>
	bool SkipValuesForMode(bool bParallelDecode, double& OutTokenMs, double& OutPerPositionMs)
	{
		if constexpr (requires(bool M, double& A, double& B) { TAccess::BatchedArmSkipValues(M, A, B); })
		{
			TAccess::BatchedArmSkipValues(bParallelDecode, OutTokenMs, OutPerPositionMs);
			return true;
		}
		else
		{
			return false;
		}
	}

	// Written out here, independent of the production header: the pair a chunk is priced with, and
	// its price. A chunk of C tokens from depth d costs C x TokenMs + PerPositionMs x (C d + C(C-1)/2).
	double PositionSum(int32 C, int64 D)
	{
		return static_cast<double>(C) * static_cast<double>(D) + static_cast<double>(C) * (C - 1) / 2.0;
	}

	double OraclePriceMs(int32 C, int64 D, const FSuperSLMRuntimeConfig& Config)
	{
		const bool bBatched = C >= kFloor;
		const double TokenMs = bBatched ? SuperSLMConfigAccess::GetCost(Config, TEXT("BatchedPromptTokenCostMs")) : SuperSLMConfigAccess::GetCost(Config, TEXT("PromptTokenCostMs"));
		const double PerPositionMs = bBatched ? SuperSLMConfigAccess::GetCost(Config, TEXT("BatchedPromptTokenCostPerPositionMs")) : SuperSLMConfigAccess::GetCost(Config, TEXT("PromptTokenCostPerPositionMs"));
		return C * TokenMs + PerPositionMs * PositionSum(C, D);
	}

	// 1.0's PlanPrefillChunk: one price over every size, no floor.
	int32 Oracle10Chunk(int32 MaxChunk, int64 D, double ShareMs, double TokenMs, double PerPositionMs)
	{
		int32 Chunk = 1;
		for (int32 C = 2; C <= MaxChunk; ++C)
		{
			if (C * TokenMs + PerPositionMs * PositionSum(C, D) > ShareMs) { break; }
			Chunk = C;
		}
		return Chunk;
	}

	FSuperSLMRuntimeConfig RunConfig(double TickBudgetMs)
	{
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = 1;
		Config.PrefixBlockCount = 1;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = TickBudgetMs;
		return Config;
	}

	bool ImportModel(FAutomationTestBase& Test, FTestWorldWrapper& World, USuperSLMSubsystem*& Cpu, USuperSLMModel*& Model)
	{
		if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
		FString Path, Reason;
		const bool bPresent = TryGetAExArtifactPath(Path, Reason);
		if (!Test.TestTrue(*FString::Printf(TEXT("A-EX present (%s)"), *Reason), bPresent)) { return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		Model = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		if (!Test.TestNotNull(TEXT("A-EX imports"), Model) || !Test.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted)) { return false; }
		Cpu = GetSubsystem(World.GetTestWorld());
		return Test.TestNotNull(TEXT("CPU subsystem"), Cpu);
	}

	bool Configure(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, USuperSLMModel* Model, const FSuperSLMRuntimeConfig& Config, const TCHAR* What)
	{
		const FSuperSLMConfigureReport Report = Cpu.Configure(Model, Config);
		return Test.TestEqual(*FString::Printf(TEXT("Configure (%s): %s"), What, *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	bool Until(USuperSLMSubsystem& Cpu, TFunctionRef<bool()> Done, double MaxSeconds, EL2S1DrainPacing Pacing = EL2S1DrainPacing::FastAsPossible)
	{
		DrainTicks(Cpu, 1.0f / 60.0f, Pacing, Done, MaxSeconds);
		return Done();
	}

	// The first Count tokens of a real text prompt (real ids, so every token is one the model serves).
	bool PromptOfLength(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, int32 Count, TArray<int32>& Out)
	{
		TArray<int32> All;
		if (!Test.TestTrue(TEXT("Tokenize"), Cpu.Tokenize(
				TEXT("The lamplighter walked the harbour road at dusk, counting the ships at anchor, naming each one "
					 "aloud to the gulls, and wondering which of them would sail before the winter storms arrived."), All)))
		{
			return false;
		}
		if (!Test.TestTrue(*FString::Printf(TEXT("the text gives at least %d tokens (got %d)"), Count, All.Num()), All.Num() >= Count)) { return false; }
		Out.Reset();
		Out.Append(All.GetData(), Count);
		return true;
	}

	bool MakePrefix(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const TArray<int32>& Tokens, FSuperSLMPrefix& Prefix)
	{
		FString Error;
		const bool bCreated = Cpu.CreatePrefix(Tokens, Prefix, Error);
		if (!Test.TestTrue(*FString::Printf(TEXT("create %d-token prefix: %s"), Tokens.Num(), *Error), bCreated)) { return false; }
		return Test.TestTrue(TEXT("prefix reaches Ready"), Until(Cpu, [&]()
		{
			const ESuperSLMPrefixPhase Phase = Cpu.GetPrefixPhase(Prefix);
			return Phase == ESuperSLMPrefixPhase::Ready || Phase == ESuperSLMPrefixPhase::Faulted;
		}, 180.0) && Cpu.IsPrefixReady(Prefix));
	}

	bool ResetAndAdopt(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMPrefix& Prefix)
	{
		FSuperSLMLifecycleOpHandle Reset;
		FString Error;
		if (!Test.TestEqual(TEXT("reset queues"), (uint8)Cpu.ResetSequence(Seq, Reset, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		if (!Test.TestTrue(TEXT("reset drains"), Until(Cpu, [&]() { return Cpu.GetLifecycleOpResult(Reset) != ESuperSLMRestoreResult::Pending; }, 60.0) &&
				Cpu.GetLifecycleOpResult(Reset) == ESuperSLMRestoreResult::Success)) { return false; }
		FSuperSLMLifecycleOpHandle Adopt;
		if (!Test.TestEqual(TEXT("adopt queues"), (uint8)Cpu.AdoptPrefix(Seq, Prefix, Adopt, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		return Test.TestTrue(TEXT("adopt drains"), Until(Cpu, [&]() { return Cpu.GetLifecycleOpResult(Adopt) != ESuperSLMRestoreResult::Pending; }, 60.0) &&
			Cpu.GetLifecycleOpResult(Adopt) == ESuperSLMRestoreResult::Success);
	}

	bool ResetOnly(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		FSuperSLMLifecycleOpHandle Reset;
		FString Error;
		if (!Test.TestEqual(TEXT("reset queues"), (uint8)Cpu.ResetSequence(Seq, Reset, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		return Test.TestTrue(TEXT("reset drains"), Until(Cpu, [&]() { return Cpu.GetLifecycleOpResult(Reset) != ESuperSLMRestoreResult::Pending; }, 60.0) &&
			Cpu.GetLifecycleOpResult(Reset) == ESuperSLMRestoreResult::Success);
	}

	bool Generate(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const TArray<int32>& Prompt, int32 NewTokens,
		EL2S1DrainPacing Pacing = EL2S1DrainPacing::FastAsPossible)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = NewTokens;
		FString Error;
		const bool bBegun = Cpu.BeginGeneration(Seq, Request, Error);
		if (!Test.TestTrue(*FString::Printf(TEXT("begin generation: %s"), *Error), bBegun)) { return false; }
		return Test.TestTrue(TEXT("generation completes"), Until(Cpu, [&]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}, 180.0, Pacing) && Cpu.GetPhase(Seq) == ESuperSLMSequencePhase::Complete);
	}

	bool SaveBlob(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob)
	{
		FSuperSLMLifecycleOpHandle Save;
		FString Error;
		if (!Test.TestEqual(*FString::Printf(TEXT("save queues: %s"), *Error), (uint8)Cpu.SaveSequence(Seq, Save, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		Until(Cpu, [&]()
		{
			if (Result == ESuperSLMRestoreResult::Pending) { Result = Cpu.GetSaveResult(Save, OutBlob); }
			return Result != ESuperSLMRestoreResult::Pending;
		}, 60.0);
		return Test.TestEqual(TEXT("save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) &&
			Test.TestTrue(TEXT("save blob is non-empty"), OutBlob.Num() > 0);
	}

	// Prefill rows (sequence and prefix) from Start on, in ledger order.
	TArray<FSuperSLMWorkerJobReport> PrefillRows(const TArray<FSuperSLMWorkerJobReport>& Ledger, int32 Start = 0)
	{
		TArray<FSuperSLMWorkerJobReport> Out;
		for (int32 I = Start; I < Ledger.Num(); ++I)
		{
			if (Ledger[I].Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Ledger[I].PromptTokens > 0) { Out.Add(Ledger[I]); }
		}
		return Out;
	}

	bool AnyChunkOf(const TArray<FSuperSLMWorkerJobReport>& Rows, int32 Tokens)
	{
		return Rows.ContainsByPredicate([Tokens](const FSuperSLMWorkerJobReport& R) { return R.PromptTokens == Tokens; });
	}

	// Captures LogSuperSLM warnings (Configure's call-shape warning).
	class FWarningCapture : public FOutputDevice
	{
	public:
		TArray<FString> Lines;
		FWarningCapture() { GLog->AddOutputDevice(this); }
		virtual ~FWarningCapture() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FName(TEXT("LogSuperSLM")) && Verbosity == ELogVerbosity::Warning) { Lines.Add(V); }
		}
		int32 CountContaining(const TCHAR* Needle) const
		{
			int32 N = 0;
			for (const FString& L : Lines) { N += L.Contains(Needle) ? 1 : 0; }
			return N;
		}
	};
	const TCHAR* const kTiledWarning = TEXT("prefill will not reach the tiled GEMM");
}

// ---------------------------------------------------------------------------------------------
// 9.P-4 (the floor through the seam) -- no artifact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PFloorSeamTest,
	"SuperSLM.Slice1P.FloorSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PFloorSeamTest::RunTest(const FString& Parameters)
{
	bool bOk = TestEqual(TEXT("the seam's tiled threshold is 8"), FSuperSLMSchedulingTestAccess::TiledMinTokens(), kFloor);
	const FSuperSLMRuntimeConfig Default; // a default-constructed config: MinPrefillChunk 8, the shipped pairs
	bOk &= TestEqual(TEXT("MinPrefillChunk defaults to 8"), Default.MinPrefillChunk, kFloor);

	// 8 at every shipped budget and depth, on the default config.
	const double Budgets[] = { 8.0, 8.33, 16.6, 33.3, 100.0 };
	const int64 Depths[] = { 0, 1000, kAExCap - kFloor };
	for (const double Budget : Budgets)
	{
		for (const int64 Depth : Depths)
		{
			const int32 MaxChunk = static_cast<int32>(FMath::Min<int64>(64, kAExCap - Depth));
			const int32 Chunk = FSuperSLMSchedulingTestAccess::PlanPrefillChunk(MaxChunk, Depth, Budget * Default.BudgetHeadroom, Default);
			bOk &= TestEqual(*FString::Printf(TEXT("default config, %.2f ms, depth %lld: chunk 8"), Budget, Depth), Chunk, kFloor);
		}
	}

	// At 1000 ms, depth 0, a distinct cheaper batched pair: the largest C whose batched price fits.
	{
		FSuperSLMRuntimeConfig Cheap = Default;
		SuperSLMConfigAccess::SetCost(Cheap, TEXT("BatchedPromptTokenCostMs"), 20.0);
		SuperSLMConfigAccess::SetCost(Cheap, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.001);
		const double Share = 1000.0 * Cheap.BudgetHeadroom;
		int32 Expected = 0;
		for (int32 C = kFloor; C <= 64; ++C)
		{
			if (OraclePriceMs(C, 0, Cheap) <= Share) { Expected = C; } else { break; }
		}
		bOk &= TestEqual(TEXT("the independent oracle's largest batched fit at 1000 ms is 34"), Expected, 34);
		bOk &= TestEqual(TEXT("1000 ms, depth 0, cheaper batched pair: the largest C whose batched price fits"),
			FSuperSLMSchedulingTestAccess::PlanPrefillChunk(64, 0, Share, Cheap), Expected);
		// Preserve the S1P-C regression with its actual input pair. D4's measured mode-0
		// profile is a different input and must independently stop at 46.
		FSuperSLMRuntimeConfig S1PC = Default;
		SuperSLMConfigAccess::SetCost(S1PC, TEXT("BatchedPromptTokenCostMs"), 15.75);
		SuperSLMConfigAccess::SetCost(S1PC, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.0046);
		bOk &= TestEqual(TEXT("1000 ms, depth 0, S1P-C pairs: 44"),
			FSuperSLMSchedulingTestAccess::PlanPrefillChunk(64, 0, Share, S1PC), 44);
		bOk &= TestEqual(TEXT("1000 ms, depth 0, D4 mode-0 pairs: 46"),
			FSuperSLMSchedulingTestAccess::PlanPrefillChunk(64, 0, Share, Default), 46);
	}

	// Left below 8: the floor is min(MinPrefillChunk, MaxChunk).
	for (int32 Left = 1; Left < kFloor; ++Left)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("%d tokens left: chunk %d"), Left, Left),
			FSuperSLMSchedulingTestAccess::PlanPrefillChunk(Left, 100, 16.6 * Default.BudgetHeadroom, Default), Left);
	}

	// MaxPrefillChunkBudget below 8: MaxChunk.
	for (int32 Budget = 1; Budget < kFloor; ++Budget)
	{
		FSuperSLMRuntimeConfig Small = Default;
		Small.MaxPrefillChunkBudget = Budget;
		bOk &= TestEqual(*FString::Printf(TEXT("MaxPrefillChunkBudget %d: chunk %d"), Budget, Budget),
			FSuperSLMSchedulingTestAccess::PlanPrefillChunk(FMath::Min(Budget, 40), 0, 16.6 * Default.BudgetHeadroom, Small), Budget);
	}

	// 1.0's table at MinPrefillChunk 1 with the pairs equal: identical to 1.0's rule at every row.
	{
		FSuperSLMRuntimeConfig One = Default;
		One.MinPrefillChunk = 1;
		const double TokenMsSet[] = { 0.5, 1.0, 2.0, 36.0 };
		const double SlopeSet[] = { 0.0, 0.0015, 0.021 };
		int32 Mismatches = 0, Rows = 0;
		for (const double TokenMs : TokenMsSet)
		{
			for (const double Slope : SlopeSet)
			{
				SuperSLMConfigAccess::SetCost(One, TEXT("PromptTokenCostMs"), TokenMs);
				SuperSLMConfigAccess::SetCost(One, TEXT("BatchedPromptTokenCostMs"), TokenMs);
				SuperSLMConfigAccess::SetCost(One, TEXT("PromptTokenCostPerPositionMs"), Slope);
				SuperSLMConfigAccess::SetCost(One, TEXT("BatchedPromptTokenCostPerPositionMs"), Slope);
				for (const double Budget : { 8.0, 16.6, 33.3, 100.0, 1000.0 })
				{
					for (const int64 Depth : { int64(0), int64(1), int64(1000), int64(4000) })
					{
						for (const int32 MaxChunk : { 1, 2, 7, 8, 9, 64 })
						{
							const double Share = Budget * One.BudgetHeadroom;
							const int32 Got = FSuperSLMSchedulingTestAccess::PlanPrefillChunk(MaxChunk, Depth, Share, One);
							const int32 Want = Oracle10Chunk(MaxChunk, Depth, Share, TokenMs, Slope);
							++Rows;
							if (Got != Want)
							{
								++Mismatches;
								AddError(FString::Printf(TEXT("floor 1, equal pairs %.4f/%.4f, %.1f ms, depth %lld, max %d: got %d, 1.0 gives %d"),
									TokenMs, Slope, Budget, Depth, MaxChunk, Got, Want));
							}
						}
					}
				}
			}
		}
		bOk &= TestEqual(*FString::Printf(TEXT("floor 1 with equal pairs is 1.0's PlanPrefillChunk on all %d rows"), Rows), Mismatches, 0);
	}

	// The chunk's own cost is priced with the pair its size selects (item 2), distinct pairs.
	{
		FSuperSLMRuntimeConfig Distinct = Default;
		SuperSLMConfigAccess::SetCost(Distinct, TEXT("BatchedPromptTokenCostMs"), 30.0);
		SuperSLMConfigAccess::SetCost(Distinct, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.015);
		for (const int32 C : { 1, 7, 8, 13 })
		{
			for (const int64 D : { int64(0), int64(20), int64(1000) })
			{
				bOk &= TestTrue(*FString::Printf(TEXT("chunk %d at depth %lld is priced with the %s pair"), C, D, C >= kFloor ? TEXT("batched") : TEXT("chunk-1")),
					FMath::IsNearlyEqual(FSuperSLMSchedulingTestAccess::PrefillChunkCostMs(C, D, Distinct), OraclePriceMs(C, D, Distinct), 1e-9));
			}
		}
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-7, part 1: overrun at the job's own K, through the seam -- no artifact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1POverrunSeamTest,
	"SuperSLM.Slice1P.OverrunSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1POverrunSeamTest::RunTest(const FString& Parameters)
{
	const double B = 16.6;
	bool bOk = TestFalse(TEXT("K = 3 at 2.5 budgets is not an overrun"), FSuperSLMSchedulingTestAccess::IsWorkerOverrun(2.5 * B, 3, B));
	bOk &= TestTrue(TEXT("K = 3 at 3.5 budgets is an overrun"), FSuperSLMSchedulingTestAccess::IsWorkerOverrun(3.5 * B, 3, B));
	bOk &= TestFalse(TEXT("K = 3 at exactly 3 budgets is not an overrun (strictly over)"), FSuperSLMSchedulingTestAccess::IsWorkerOverrun(3.0 * B, 3, B));
	// At K = 1 the rule is 1.0's: over TickBudgetMs.
	bOk &= TestTrue(TEXT("K = 1 over one budget is an overrun"), FSuperSLMSchedulingTestAccess::IsWorkerOverrun(1.01 * B, 1, B));
	bOk &= TestFalse(TEXT("K = 1 within one budget is not an overrun"), FSuperSLMSchedulingTestAccess::IsWorkerOverrun(0.99 * B, 1, B));
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-8, the synthetic seam: the known-slope fit at d + 3.5, the envelope's raise, the intercept
// comparison, the arm's depths and the skip's values -- no artifact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PBatchedArmSeamTest,
	"SuperSLM.Slice1P.BatchedArmSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PBatchedArmSeamTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	TArray<int64> Depths;
	bOk &= TestFalse(TEXT("cap 24: the named skip (no depths)"), FSuperSLMCalibrationTestAccess::BatchedArmDepthsForCap(24, Depths));
	bOk &= TestFalse(TEXT("cap 32: the named skip"), FSuperSLMCalibrationTestAccess::BatchedArmDepthsForCap(32, Depths));
	for (int64 Cap = 33; Cap <= 300; ++Cap)
	{
		if (!FSuperSLMCalibrationTestAccess::BatchedArmDepthsForCap(Cap, Depths) || Depths.Num() != 5)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("cap %lld gives five depths"), Cap), false);
			continue;
		}
		bool bAscending = Depths[0] == 1 && Depths[4] == Cap - kFloor;
		for (int32 I = 1; I < 5; ++I) { bAscending &= Depths[I] > Depths[I - 1]; }
		if (!bAscending) { bOk &= TestTrue(*FString::Printf(TEXT("cap %lld: depths 1 .. C-8 ascend and are distinct"), Cap), false); }
	}
	if (!TestTrue(TEXT("cap 4096 gives five depths"), FSuperSLMCalibrationTestAccess::BatchedArmDepthsForCap(kAExCap, Depths))) { return false; }
	const TArray<int64> Want = { 1, 1024, 2048, 3072, kAExCap - kFloor };
	bOk &= TestTrue(TEXT("cap 4096: depths 1, 1024, 2048, 3072, 4088 (the last point at C - 8)"), Depths == Want);

	// Known slope: per-token time a + s x position, position = d + 3.5, call = 8 x per-token.
	const double A = 30.0, S = 0.02;
	TArray<double> Calls;
	for (const int64 D : Depths) { Calls.Add(8.0 * (A + S * (static_cast<double>(D) + 3.5))); }
	double TokenMs = -1.0, Slope = -1.0;
	TArray<double> Positions, PerToken;
	FString Error;
	bOk &= TestTrue(*FString::Printf(TEXT("known-slope fit succeeds: %s"), *Error),
		FSuperSLMCalibrationTestAccess::FitBatchedArmFromSyntheticTimings(Depths, Calls, 36.0, TokenMs, Slope, Positions, PerToken, Error));
	bOk &= TestTrue(TEXT("readings recorded at d + 3.5"), Positions.Num() == 5 && FMath::IsNearlyEqual(Positions[0], 4.5, 1e-12) &&
		FMath::IsNearlyEqual(Positions[4], static_cast<double>(kAExCap - kFloor) + 3.5, 1e-12));
	bOk &= TestTrue(TEXT("per-token reading is call / 8"), PerToken.Num() == 5 && FMath::IsNearlyEqual(PerToken[2], Calls[2] / 8.0, 1e-12));
	bOk &= TestTrue(*FString::Printf(TEXT("known slope recovered (%.9f)"), Slope), FMath::IsNearlyEqual(Slope, S, 1e-12));
	bOk &= TestTrue(*FString::Printf(TEXT("known intercept recovered at position 0 (%.9f)"), TokenMs), FMath::IsNearlyEqual(TokenMs, A, 1e-9));

	// The envelope's raise: one middle reading 0.5 ms per token above the line raises the intercept
	// by 0.5 and leaves the slope; one below it changes nothing.
	TArray<double> Raised = Calls;
	Raised[2] += 8.0 * 0.5;
	Raised[3] -= 8.0 * 0.25;
	bOk &= TestTrue(*FString::Printf(TEXT("raised fit succeeds: %s"), *Error),
		FSuperSLMCalibrationTestAccess::FitBatchedArmFromSyntheticTimings(Depths, Raised, 36.0, TokenMs, Slope, Positions, PerToken, Error));
	bOk &= TestTrue(TEXT("raised: slope unchanged"), FMath::IsNearlyEqual(Slope, S, 1e-12));
	bOk &= TestTrue(*FString::Printf(TEXT("raised: intercept up by the largest excess (%.9f)"), TokenMs), FMath::IsNearlyEqual(TokenMs, A + 0.5, 1e-9));
	for (int32 I = 0; I < Positions.Num(); ++I)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("raised: reading %d lies on or under the shipped line"), I),
			PerToken[I] <= TokenMs + Slope * Positions[I] + 1e-9);
	}

	// The intercept comparison: equal passes, above fails by name.
	Error.Reset();
	bOk &= TestTrue(TEXT("batched intercept equal to the chunk-1 intercept passes"),
		FSuperSLMCalibrationTestAccess::FitBatchedArmFromSyntheticTimings(Depths, Calls, A, TokenMs, Slope, Positions, PerToken, Error));
	Error.Reset();
	bOk &= TestFalse(TEXT("batched intercept above the chunk-1 intercept fails"),
		FSuperSLMCalibrationTestAccess::FitBatchedArmFromSyntheticTimings(Depths, Calls, A - 0.001, TokenMs, Slope, Positions, PerToken, Error));
	bOk &= TestTrue(*FString::Printf(TEXT("the failure names the intercepts (%s)"), *Error), Error.Contains(TEXT("intercept")));

	// D-SLM8141: the skip's values are the measured mode's one-time batched pair, the same profile
	// Configure() uses (the committed fixture).
	for (const bool bMode : { false, true })
	{
		double SkipTokenMs = 0.0, SkipSlope = 0.0;
		if (!TestTrue(TEXT("fails unless the moded seam BatchedArmSkipValues(bool, double&, double&) exists"),
			SkipValuesForMode(bMode, SkipTokenMs, SkipSlope)))
		{
			bOk = false;
			continue;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("mode %d skip writes the profile's BatchedPromptTokenCostMs"), bMode ? 1 : 0),
			SkipTokenMs, SuperSLMConfigAccess::OneTimeProfileCost(bMode, TEXT("BatchedPromptTokenCostMs")));
		bOk &= TestEqual(*FString::Printf(TEXT("mode %d skip writes the profile's BatchedPromptTokenCostPerPositionMs"), bMode ? 1 : 0),
			SkipSlope, SuperSLMConfigAccess::OneTimeProfileCost(bMode, TEXT("BatchedPromptTokenCostPerPositionMs")));
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-2 (Configure: warnings, refusals, the batched fields by name) and M11-3g (the skip's values
// are accepted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PConfigureTest,
	"SuperSLM.Slice1P.Configure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PConfigureTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	bool bOk = true;

	// The warning at 7 (either field) and none at 8; tokens unchanged, so a warning, not a refusal.
	struct FShape { int32 MaxBudget; int32 Floor; bool bWarn; };
	const FShape Shapes[] = { { 64, 8, false }, { 8, 8, false }, { 7, 8, true }, { 64, 7, true }, { 64, 1, true } };
	for (const FShape& Shape : Shapes)
	{
		FSuperSLMRuntimeConfig C = RunConfig(16.6);
		C.MaxPrefillChunkBudget = Shape.MaxBudget;
		C.MinPrefillChunk = Shape.Floor;
		FWarningCapture Capture;
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("MaxPrefillChunkBudget %d, MinPrefillChunk %d accepted"), Shape.MaxBudget, Shape.Floor),
			(uint8)R.Result, (uint8)ESuperSLMConfigureResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("MaxPrefillChunkBudget %d, MinPrefillChunk %d: %s"), Shape.MaxBudget, Shape.Floor,
				Shape.bWarn ? TEXT("one call-shape warning") : TEXT("no call-shape warning")),
			Capture.CountContaining(kTiledWarning), Shape.bWarn ? 1 : 0);
	}

	// MinPrefillChunk <= 0 refused, by name.
	for (const int32 Bad : { 0, -1, std::numeric_limits<int32>::min() })
	{
		FSuperSLMRuntimeConfig C = RunConfig(16.6);
		C.MinPrefillChunk = Bad;
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("MinPrefillChunk %d refused"), Bad), (uint8)R.Result, (uint8)ESuperSLMConfigureResult::InvalidCallShape);
		bOk &= TestTrue(*FString::Printf(TEXT("MinPrefillChunk %d refusal names the field"), Bad), R.Message.Contains(TEXT("MinPrefillChunk")));
	}

	// The batched fields refused by name: the per-token field at 0, -1, NaN, +inf; the slope at
	// -1e-3 and NaN.
	struct FBad { const TCHAR* Name; double Value; };
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();
	const FBad Bads[] = {
		{ TEXT("BatchedPromptTokenCostMs"), 0.0 },
		{ TEXT("BatchedPromptTokenCostMs"), -1.0 },
		{ TEXT("BatchedPromptTokenCostMs"), NaN },
		{ TEXT("BatchedPromptTokenCostMs"), Inf },
		{ TEXT("BatchedPromptTokenCostPerPositionMs"), -1e-3 },
		{ TEXT("BatchedPromptTokenCostPerPositionMs"), NaN },
	};
	for (const FBad& Bad : Bads)
	{
		FSuperSLMRuntimeConfig C = RunConfig(16.6);
		SuperSLMConfigAccess::SetCost(C, Bad.Name, Bad.Value);
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("%s = %g refused"), Bad.Name, Bad.Value), (uint8)R.Result, (uint8)ESuperSLMConfigureResult::InvalidCallShape);
		bOk &= TestTrue(*FString::Printf(TEXT("%s = %g refusal names the field (%s)"), Bad.Name, Bad.Value, *R.Message), R.Message.Contains(Bad.Name));
	}
	{
		FSuperSLMRuntimeConfig C = RunConfig(16.6);
		SuperSLMConfigAccess::SetCost(C, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.0);
		bOk &= Configure(*this, *Cpu, Model, C, TEXT("a zero batched slope is accepted"));
	}

	// M11-3g: the named skip's values pass Configure.
	for (const bool bMode : { false, true })
	{
		FSuperSLMRuntimeConfig C = RunConfig(16.6);
		double SkipTokenMs = 0.0, SkipSlope = 0.0;
		if (!TestTrue(TEXT("fails unless the moded seam BatchedArmSkipValues(bool, double&, double&) exists"),
			SkipValuesForMode(bMode, SkipTokenMs, SkipSlope)))
		{
			bOk = false;
			continue;
		}
		SuperSLMConfigAccess::SetMode(C, bMode);
		SuperSLMConfigAccess::SetCost(C, TEXT("BatchedPromptTokenCostMs"), SkipTokenMs);
		SuperSLMConfigAccess::SetCost(C, TEXT("BatchedPromptTokenCostPerPositionMs"), SkipSlope);
		bOk &= Configure(*this, *Cpu, Model, C, bMode ? TEXT("the mode-1 skip values") : TEXT("the mode-0 skip values"));
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-5: item 5's one definition. The limit is K <= kMaxJobTicks at TickBudgetMs; positions are
// priced at zero so each bound is tokens x TokenMs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PConfigureBoundTest,
	"SuperSLM.Slice1P.ConfigureBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PConfigureBoundTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	const double Budget = 16.6;
	const double LimitMs = static_cast<double>(FSuperSLMSchedulingTestAccess::MaxJobTicks()) * Budget; // about 1.74e7 ms
	auto Priced = [Budget](double Chunk1TokenMs, double BatchedTokenMs)
	{
		FSuperSLMRuntimeConfig C = RunConfig(Budget); // P = 64
		SuperSLMConfigAccess::SetCost(C, TEXT("PromptTokenCostMs"), Chunk1TokenMs);
		SuperSLMConfigAccess::SetCost(C, TEXT("PromptTokenCostPerPositionMs"), 0.0);
		SuperSLMConfigAccess::SetCost(C, TEXT("BatchedPromptTokenCostMs"), BatchedTokenMs);
		SuperSLMConfigAccess::SetCost(C, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.0);
		return C;
	};
	bool bOk = true;
	auto Expect = [&](const TCHAR* What, const FSuperSLMRuntimeConfig& C, bool bAccept)
	{
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: %s (%s)"), What, bAccept ? TEXT("accepted") : TEXT("refused"), *R.Message), (uint8)R.Result,
			(uint8)(bAccept ? ESuperSLMConfigureResult::Success : ESuperSLMConfigureResult::InvalidCostDomain));
		if (!bAccept)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the refusal names prefill and K"), What), R.Message.Contains(TEXT("prefill")) && R.Message.Contains(TEXT("K =")));
		}
	};
	// A cheap batched pair with an expensive chunk-1 pair whose 7-token tail exceeds the limit.
	const double Chunk1Over = 1.2 * LimitMs / 7.0;
	bOk &= TestTrue(TEXT("construction: 7 chunk-1 tokens exceed the limit, 1 does not"), 7.0 * Chunk1Over > LimitMs && Chunk1Over < LimitMs);
	Expect(TEXT("chunk-1 pair over the limit at a 7-token tail"), Priced(Chunk1Over, 1.0), false);
	// The batched pair exceeding it at P = 64, with a cheap chunk-1 pair.
	const double BatchedOver = 1.2 * LimitMs / 64.0;
	bOk &= TestTrue(TEXT("construction: 64 batched tokens exceed the limit, 7 do not"), 64.0 * BatchedOver > LimitMs && 7.0 * BatchedOver < LimitMs);
	Expect(TEXT("batched pair over the limit at P"), Priced(1.0, BatchedOver), false);
	// Neither exceeding it: accepted. The chunk-1 figure here would exceed the limit priced at P
	// (64 tokens), so a bound that priced the chunk-1 pair at P refuses this.
	const double Chunk1Under = 0.8 * LimitMs / 7.0;
	const double BatchedUnder = 0.8 * LimitMs / 64.0;
	bOk &= TestTrue(TEXT("construction: the chunk-1 figure priced at P would exceed the limit"), 64.0 * Chunk1Under > LimitMs);
	Expect(TEXT("both pairs within the limit"), Priced(Chunk1Under, BatchedUnder), true);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-4 call sites: a real sequence prefill and a real prefix prefill at 16.6 ms each show a
// chunk of 8, and a 13-token prompt gives {8, 5} and a context of 13.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PFloorCallSitesTest,
	"SuperSLM.Slice1P.FloorCallSites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PFloorCallSitesTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	const FSuperSLMRuntimeConfig Config = RunConfig(16.6); // the shipped floor and pairs
	if (!Configure(*this, *Cpu, Model, Config, TEXT("16.6 ms, shipped floor"))) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	bool bOk = true;

	TArray<int32> Prompt13;
	if (!PromptOfLength(*this, *Cpu, 13, Prompt13)) { return false; }
	const int32 SeqStart = Cpu->GetJobLedger().Num();
	if (!Generate(*this, *Cpu, Seq, Prompt13, 2)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger();
	const TArray<FSuperSLMWorkerJobReport> Rows = PrefillRows(Ledger, SeqStart);
	bOk &= TestEqual(TEXT("13-token prompt: two prefill jobs"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		bOk &= TestEqual(TEXT("sequence prefill (floor-on precondition): first chunk 8"), Rows[0].PromptTokens, 8);
		bOk &= TestEqual(TEXT("13-token prompt: second chunk 5"), Rows[1].PromptTokens, 5);
		bOk &= TestEqual(TEXT("second chunk starts at 8"), Rows[1].PromptPositionSum, static_cast<int64>(5 * 8 + 10));
	}
	// ContextUsed 13: the first decode after prefill prices each layer at depth 13.
	for (int32 I = SeqStart; I < Ledger.Num(); ++I)
	{
		if (Ledger[I].Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Ledger[I].DecodeLayers > 0)
		{
			bOk &= TestEqual(TEXT("ContextUsed after the {8, 5} prefill is 13"), Ledger[I].DecodeLayerDepthSum,
				static_cast<int64>(Ledger[I].DecodeLayers) * 13);
			break;
		}
	}

	// A real prefix prefill.
	TArray<int32> PrefixTokens;
	if (!PromptOfLength(*this, *Cpu, 20, PrefixTokens)) { return false; }
	const int32 PrefixStart = Cpu->GetJobLedger().Num();
	FSuperSLMPrefix Prefix;
	if (!MakePrefix(*this, *Cpu, PrefixTokens, Prefix)) { return false; }
	TArray<int32> PrefixChunks;
	for (const FSuperSLMWorkerJobReport& Row : PrefillRows(Cpu->GetJobLedger(), PrefixStart))
	{
		if (Row.MemberSequences.IsEmpty()) { PrefixChunks.Add(Row.PromptTokens); }
	}
	bOk &= TestTrue(*FString::Printf(TEXT("prefix prefill (floor-on precondition): chunks {8, 8, 4} (got %d chunks)"), PrefixChunks.Num()),
		PrefixChunks == TArray<int32>({ 8, 8, 4 }));
	Cpu->ReturnSequence(Seq);
	Cpu->ReleasePrefix(Prefix);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-6: price selection (R-S1b(i) extended), distinct pairs, depth > 0, sequence and prefix
// prefill at C >= 8 and a 7-token tail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PPriceSelectionTest,
	"SuperSLM.Slice1P.PriceSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PPriceSelectionTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	FSuperSLMRuntimeConfig Config = RunConfig(16.6);
	SuperSLMConfigAccess::SetCost(Config, TEXT("BatchedPromptTokenCostMs"), 30.0);               // differs from the chunk-1 pair (36.0, 0.021) in both fields
	SuperSLMConfigAccess::SetCost(Config, TEXT("BatchedPromptTokenCostPerPositionMs"), 0.015);
	bool bOk = TestTrue(TEXT("distinct-pair rule: the pairs differ in both fields"),
		SuperSLMConfigAccess::GetCost(Config, TEXT("BatchedPromptTokenCostMs")) != SuperSLMConfigAccess::GetCost(Config, TEXT("PromptTokenCostMs")) && SuperSLMConfigAccess::GetCost(Config, TEXT("BatchedPromptTokenCostPerPositionMs")) != SuperSLMConfigAccess::GetCost(Config, TEXT("PromptTokenCostPerPositionMs")));
	if (!Configure(*this, *Cpu, Model, Config, TEXT("distinct pairs"))) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }

	// A 20-token prefix (prefix chunks 8, 8 and a 4-token tail), adopted, then a 15-token suffix
	// at depth 20 (sequence chunks 8 and a 7-token tail).
	TArray<int32> Tokens;
	if (!PromptOfLength(*this, *Cpu, 35, Tokens)) { return false; }
	const TArray<int32> PrefixTokens(Tokens.GetData(), 20);
	const TArray<int32> Suffix(Tokens.GetData() + 20, 15);
	FSuperSLMPrefix Prefix;
	if (!MakePrefix(*this, *Cpu, PrefixTokens, Prefix) || !ResetAndAdopt(*this, *Cpu, Seq, Prefix)) { return false; }
	if (!Generate(*this, *Cpu, Seq, Suffix, 2)) { return false; }

	const TArray<FSuperSLMWorkerJobReport> Rows = PrefillRows(Cpu->GetJobLedger());
	bOk &= TestTrue(TEXT("floor-on precondition: a prefill job of 8"), AnyChunkOf(Rows, 8));
	bOk &= TestTrue(TEXT("the run has a 7-token tail"), AnyChunkOf(Rows, 7));
	int32 PrefixBatched = 0, SequenceBatched = 0, SequenceTail = 0;
	for (const FSuperSLMWorkerJobReport& Row : Rows)
	{
		const bool bPrefix = Row.MemberSequences.IsEmpty();
		// The start depth, from the positions the row served: sum = C d + C(C-1)/2.
		const int64 C = Row.PromptTokens;
		const int64 D = (Row.PromptPositionSum - C * (C - 1) / 2) / C;
		const double Expected = OraclePriceMs(Row.PromptTokens, D, Config);
		bOk &= TestTrue(*FString::Printf(TEXT("%s chunk %d at depth %lld: planned %.6f ms, the %s pair gives %.6f ms"),
				bPrefix ? TEXT("prefix") : TEXT("sequence"), Row.PromptTokens, D, Row.PlannedJobMs,
				Row.PromptTokens >= kFloor ? TEXT("batched") : TEXT("chunk-1"), Expected),
			FMath::IsNearlyEqual(Row.PlannedJobMs, Expected, 1e-6));
		bOk &= TestEqual(*FString::Printf(TEXT("job %lld: K from the selected price"), Row.JobId), Row.K, ExpectedK(Expected, Config.TickBudgetMs));
		PrefixBatched += (bPrefix && C >= kFloor) ? 1 : 0;
		SequenceBatched += (!bPrefix && C >= kFloor) ? 1 : 0;
		SequenceTail += (!bPrefix && C == 7 && D > 0) ? 1 : 0;
	}
	bOk &= TestTrue(TEXT("a prefix prefill job at C >= 8"), PrefixBatched > 0);
	bOk &= TestTrue(TEXT("a sequence prefill job at C >= 8"), SequenceBatched > 0);
	bOk &= TestEqual(TEXT("one sequence 7-token tail at depth > 0"), SequenceTail, 1);
	// R-S1b's full oracle, whose planned-cost check is the pair-selected one.
	bOk &= AssertRS1bProperties(*this, Cpu->GetJobLedger(), Config, Cpu->GetHitchCount(), FPlatformTLS::GetCurrentThreadId(), TEXT("Slice1P.PriceSelection"));
	Cpu->ReturnSequence(Seq);
	Cpu->ReleasePrefix(Prefix);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-7, part 2: a real run at 16.6 ms under R-S1b(v)'s restated oracle, with at least one job
// whose WorkerCallMs lies in (budget, K x budget] -- over one tick, within its own K.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1POverrunRealRunTest,
	"SuperSLM.Slice1P.OverrunRealRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1POverrunRealRunTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	const FSuperSLMRuntimeConfig Config = RunConfig(16.6);
	if (!Configure(*this, *Cpu, Model, Config, TEXT("16.6 ms"))) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	TArray<int32> Prompt;
	if (!PromptOfLength(*this, *Cpu, 24, Prompt)) { return false; }
	if (!Generate(*this, *Cpu, Seq, Prompt, 8, EL2S1DrainPacing::RealTime)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger();
	bool bOk = TestTrue(TEXT("floor-on precondition: a prefill job of 8"), AnyChunkOf(PrefillRows(Ledger), 8));
	int32 WithinOwnK = 0;
	for (const FSuperSLMWorkerJobReport& Job : Ledger)
	{
		if (Job.DeliveredAtTick >= 0 && Job.K > 1 && Job.WorkerCallMs > Config.TickBudgetMs && Job.WorkerCallMs <= Job.K * Config.TickBudgetMs)
		{
			++WithinOwnK;
			bOk &= TestFalse(*FString::Printf(TEXT("job %lld: %.3f ms is over one tick but within its own K %d: not an overrun"), Job.JobId, Job.WorkerCallMs, Job.K),
				Job.bWorkerOverran);
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("non-vacuity: at least one job's WorkerCallMs lies in (budget, K x budget] (got %d)"), WithinOwnK), WithinOwnK >= 1);
	bOk &= AssertRS1bProperties(*this, Ledger, Config, Cpu->GetHitchCount(), FPlatformTLS::GetCurrentThreadId(), TEXT("Slice1P.OverrunRealRun"));
	Cpu->ReturnSequence(Seq);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-1: lifetime and reuse. A sequence prefilled in chunks of 8, reset and prefilled again, and a
// prefix prefilled then adopted: tokens and the save blob equal the same run at MinPrefillChunk 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PLifetimeTest,
	"SuperSLM.Slice1P.LifetimeAndReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PLifetimeTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }

	struct FRun
	{
		TArray<int32> First, Second, Adopted;
		TArray<uint8> SecondBlob, AdoptedBlob;
		TArray<int32> Chunks;
	};
	auto RunAt = [&](int32 Floor, FRun& Out) -> bool
	{
		FSuperSLMRuntimeConfig Config = RunConfig(16.6);
		Config.MinPrefillChunk = Floor;
		if (!Configure(*this, *Cpu, Model, Config, *FString::Printf(TEXT("floor %d"), Floor))) { return false; }
		FSuperSLMSequence Seq;
		if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
		TArray<int32> Tokens;
		if (!PromptOfLength(*this, *Cpu, 38, Tokens)) { return false; }
		const TArray<int32> Prompt(Tokens.GetData(), 18);
		const TArray<int32> PrefixTokens(Tokens.GetData() + 18, 20);
		if (!Generate(*this, *Cpu, Seq, Prompt, 6)) { return false; }
		Out.First = Cpu->GetGeneratedTokens(Seq);
		if (!ResetOnly(*this, *Cpu, Seq) || !Generate(*this, *Cpu, Seq, Prompt, 6)) { return false; }
		Out.Second = Cpu->GetGeneratedTokens(Seq);
		if (!SaveBlob(*this, *Cpu, Seq, Out.SecondBlob)) { return false; }
		FSuperSLMPrefix Prefix;
		if (!MakePrefix(*this, *Cpu, PrefixTokens, Prefix) || !ResetAndAdopt(*this, *Cpu, Seq, Prefix)) { return false; }
		if (!Generate(*this, *Cpu, Seq, TArray<int32>(Tokens.GetData(), 10), 6)) { return false; }
		Out.Adopted = Cpu->GetGeneratedTokens(Seq);
		if (!SaveBlob(*this, *Cpu, Seq, Out.AdoptedBlob)) { return false; }
		for (const FSuperSLMWorkerJobReport& Row : PrefillRows(Cpu->GetJobLedger())) { Out.Chunks.Add(Row.PromptTokens); }
		Cpu->ReturnSequence(Seq);
		Cpu->ReleasePrefix(Prefix);
		return true;
	};
	FRun Floor8, Floor1;
	if (!RunAt(8, Floor8) || !RunAt(1, Floor1)) { return false; }
	bool bOk = TestTrue(TEXT("floor-on precondition: a prefill job of 8 at floor 8"), Floor8.Chunks.Contains(8));
	bOk &= TestFalse(TEXT("floor 1 run is the one-token run (no chunk of 8)"), Floor1.Chunks.Contains(8));
	bOk &= TestTrue(TEXT("first generation: tokens equal floor 1"), Floor8.First.Num() > 0 && Floor8.First == Floor1.First);
	bOk &= TestTrue(TEXT("after reset: tokens equal floor 1"), Floor8.Second == Floor1.Second);
	bOk &= TestTrue(TEXT("after reset: tokens equal the first generation"), Floor8.Second == Floor8.First);
	bOk &= TestTrue(TEXT("after reset: save blob equals floor 1"), Floor8.SecondBlob == Floor1.SecondBlob);
	bOk &= TestTrue(TEXT("adopted prefix: tokens equal floor 1"), Floor8.Adopted.Num() > 0 && Floor8.Adopted == Floor1.Adopted);
	bOk &= TestTrue(TEXT("adopted prefix: save blob equals floor 1"), Floor8.AdoptedBlob == Floor1.AdoptedBlob);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-3: at MaxConcurrentCalls 2, two sequences prefill in chunks of 8 on two lanes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PTwoLanesTest,
	"SuperSLM.Slice1P.TwoLanes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PTwoLanesTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	FSuperSLMRuntimeConfig Config = RunConfig(16.6);
	Config.BlockCount = 2;
	Config.MaxConcurrentCalls = 2;
	Config.PrefixBlockCount = 0;
	if (!Configure(*this, *Cpu, Model, Config, TEXT("two lanes"))) { return false; }
	TArray<int32> Tokens;
	if (!PromptOfLength(*this, *Cpu, 36, Tokens)) { return false; }
	const TArray<int32> Prompts[2] = { TArray<int32>(Tokens.GetData(), 18), TArray<int32>(Tokens.GetData() + 18, 18) };
	FSuperSLMSequence Seqs[2];
	for (int32 I = 0; I < 2; ++I)
	{
		if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seqs[I]), (uint8)ESuperSLMVendResult::Success)) { return false; }
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompts[I];
		Request.MaxNewTokens = 4;
		FString Error;
		const bool bBegun = Cpu->BeginGeneration(Seqs[I], Request, Error);
		if (!TestTrue(*FString::Printf(TEXT("begin %d: %s"), I, *Error), bBegun)) { return false; }
	}
	auto Terminal = [&](const FSuperSLMSequence& Seq)
	{
		const ESuperSLMSequencePhase P = Cpu->GetPhase(Seq);
		return P == ESuperSLMSequencePhase::Complete || P == ESuperSLMSequencePhase::Faulted;
	};
	Until(*Cpu, [&]() { return Terminal(Seqs[0]) && Terminal(Seqs[1]); }, 180.0);
	bool bOk = true;
	TSet<uint32> PrefillThreads;
	for (int32 I = 0; I < 2; ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("sequence %d completes"), I), (uint8)Cpu->GetPhase(Seqs[I]), (uint8)ESuperSLMSequencePhase::Complete);
		TArray<int32> Chunks;
		for (const FSuperSLMWorkerJobReport& Row : PrefillRows(Cpu->GetJobLedger()))
		{
			if (Row.MemberSequences.Contains(Seqs[I]))
			{
				Chunks.Add(Row.PromptTokens);
				PrefillThreads.Add(Row.WorkerThreadId);
			}
		}
		bOk &= TestTrue(*FString::Printf(TEXT("sequence %d (floor-on precondition): prefill chunks {8, 8, 2}"), I), Chunks == TArray<int32>({ 8, 8, 2 }));
	}
	bOk &= TestTrue(*FString::Printf(TEXT("prefill ran on both lanes' worker threads (%d distinct)"), PrefillThreads.Num()), PrefillThreads.Num() >= 2);
	bOk &= AssertRS1bProperties(*this, Cpu->GetJobLedger(), Config, Cpu->GetHitchCount(), FPlatformTLS::GetCurrentThreadId(), TEXT("Slice1P.TwoLanes"));
	Cpu->ReturnSequence(Seqs[0]);
	Cpu->ReturnSequence(Seqs[1]);
	return bOk;
}

// ---------------------------------------------------------------------------------------------
// 9.P-15, the nominal arm (extends SuperSLM.L2S1.WorkerContinuation; the name keeps it under that
// filter). A chunked prompt of 24 tokens and one of 21 (the {8, 8, 5} tail), continuation on:
// the continuations are chunks of 8 (or the tail), each continuation's ChainedWaitTicks is its
// predecessor's remaining committed ticks, each overrun reads its own K, and tokens equal the run
// with continuation off. Precondition: no job overran, or the arm voids (a warning, no verdict).
// The overrun arms (a CPU worker hold seam) and M11-13h are not in this cell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMSlice1PContinuationNominalTest,
	"SuperSLM.L2S1.WorkerContinuation.Slice1PNominal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMSlice1PContinuationNominalTest::RunTest(const FString& Parameters)
{
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!ImportModel(*this, World, Cpu, Model)) { return false; }
	// Tokenize needs a configured subsystem; each arm below reconfigures before it vends.
	if (!Configure(*this, *Cpu, Model, RunConfig(16.6), TEXT("tokenize precondition"))) { return false; }
	TArray<int32> Tokens;
	if (!PromptOfLength(*this, *Cpu, 24, Tokens)) { return false; }
	bool bOk = true;
	for (const int32 Length : { 24, 21 })
	{
		const TArray<int32> Prompt(Tokens.GetData(), Length);
		TArray<int32> GeneratedOff, GeneratedOn;
		TArray<FSuperSLMWorkerJobReport> LedgerOn;
		FSuperSLMRuntimeConfig ConfigOn;
		int32 HitchesOn = 0;
		for (const bool bContinuation : { false, true })
		{
			FSuperSLMRuntimeConfig Config = RunConfig(16.6);
			Config.PrefixBlockCount = 0;
			Config.bWorkerContinuation = bContinuation;
			if (!Configure(*this, *Cpu, Model, Config, bContinuation ? TEXT("continuation on") : TEXT("continuation off"))) { return false; }
			FSuperSLMSequence Seq;
			if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
			if (!Generate(*this, *Cpu, Seq, Prompt, 6, EL2S1DrainPacing::RealTime)) { return false; }
			(bContinuation ? GeneratedOn : GeneratedOff) = Cpu->GetGeneratedTokens(Seq);
			if (bContinuation)
			{
				LedgerOn = Cpu->GetJobLedger();
				ConfigOn = Config;
				HitchesOn = Cpu->GetHitchCount();
			}
			Cpu->ReturnSequence(Seq);
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%d-token prompt: tokens equal continuation off"), Length), GeneratedOn.Num() > 0 && GeneratedOn == GeneratedOff);
		if (LedgerOn.ContainsByPredicate([](const FSuperSLMWorkerJobReport& J) { return J.bWorkerOverran; }))
		{
			AddWarning(FString::Printf(TEXT("VOID: %d-token prompt: a job overran, so the nominal arm gives no verdict on the continuation chain"), Length));
			continue;
		}
		const TArray<FSuperSLMWorkerJobReport> Rows = PrefillRows(LedgerOn);
		TArray<int32> Chunks;
		for (const FSuperSLMWorkerJobReport& Row : Rows) { Chunks.Add(Row.PromptTokens); }
		const TArray<int32> Want = Length == 24 ? TArray<int32>({ 8, 8, 8 }) : TArray<int32>({ 8, 8, 5 });
		bOk &= TestTrue(*FString::Printf(TEXT("%d-token prompt (floor-on precondition): prefill chunks %s"), Length, Length == 24 ? TEXT("{8, 8, 8}") : TEXT("{8, 8, 5}")),
			Chunks == Want);
		int32 Continuations = 0;
		for (int32 I = 1; I < LedgerOn.Num(); ++I)
		{
			const FSuperSLMWorkerJobReport& Job = LedgerOn[I];
			if (!Job.bContinuation) { continue; }
			// The job it follows: the latest earlier DecodeOrPrefill job of the same sequence.
			const FSuperSLMWorkerJobReport* Prev = nullptr;
			for (int32 J = I - 1; J >= 0 && !Prev; --J)
			{
				if (LedgerOn[J].Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && LedgerOn[J].MemberSequences == Job.MemberSequences) { Prev = &LedgerOn[J]; }
			}
			if (!TestNotNull(*FString::Printf(TEXT("continuation %lld has a predecessor"), Job.JobId), Prev)) { bOk = false; continue; }
			++Continuations;
			bOk &= TestEqual(*FString::Printf(TEXT("continuation %lld: ChainedWaitTicks is its predecessor's remaining committed ticks"), Job.JobId),
				Job.ChainedWaitTicks, FMath::Max(0, Prev->CommittedDeliveryTick - Job.PlannedAtTick));
			bOk &= TestEqual(*FString::Printf(TEXT("continuation %lld: the overrun reads its own K"), Job.JobId),
				Job.bWorkerOverran, Job.WorkerCallMs > Job.K * ConfigOn.TickBudgetMs);
			if (Job.PromptTokens > 0)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("prefill continuation %lld is a chunk of 8 or the tail (%d)"), Job.JobId, Job.PromptTokens),
					Job.PromptTokens == 8 || Job.PromptTokens == Want.Last());
				bOk &= TestTrue(*FString::Printf(TEXT("prefill continuation %lld is priced with the pair its size selects"), Job.JobId),
					FMath::IsNearlyEqual(Job.PlannedJobMs, OraclePriceMs(Job.PromptTokens,
						(Job.PromptPositionSum - int64(Job.PromptTokens) * (Job.PromptTokens - 1) / 2) / Job.PromptTokens, ConfigOn), 1e-6));
			}
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%d-token prompt: the run has continuations"), Length), Continuations > 0);
		bOk &= AssertRS1bProperties(*this, LedgerOn, ConfigOn, HitchesOn, FPlatformTLS::GetCurrentThreadId(), TEXT("Slice1P.ContinuationNominal"));
	}
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
