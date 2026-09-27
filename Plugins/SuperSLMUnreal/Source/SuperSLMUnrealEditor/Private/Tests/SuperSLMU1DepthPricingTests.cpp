#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	bool SetUp(FAutomationTestBase& Test, FTestWorldWrapper& World, USuperSLMSubsystem*& Cpu,
		USuperSLMModel*& Model, const FSuperSLMRuntimeConfig& Config)
	{
		if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
		FString Path, Reason;
		if (!Test.TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		Model = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		if (!Test.TestNotNull(TEXT("A-EX imports"), Model) || !Test.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted)) { return false; }
		Cpu = GetSubsystem(World.GetTestWorld());
		return Test.TestNotNull(TEXT("CPU subsystem"), Cpu) && Test.TestEqual(TEXT("Configure"),
			(uint8)Cpu->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	bool Until(USuperSLMSubsystem& Cpu, TFunctionRef<bool()> Done, double MaxSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done() && FPlatformTime::Seconds() - Start < MaxSeconds)
		{
			Cpu.Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		return Done();
	}

	bool MakePrefix(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, int32 Length,
		FSuperSLMPrefix& Prefix, double Timeout = 180.0)
	{
		TArray<int32> Tokens;
		Tokens.Init(1, Length);
		FString Error;
		if (!Test.TestTrue(*FString::Printf(TEXT("create %d-token prefix: %s"), Length, *Error),
			Cpu.CreatePrefix(Tokens, Prefix, Error))) { return false; }
		return Test.TestTrue(TEXT("prefix reaches Ready"), Until(Cpu, [&]()
		{
			return Cpu.GetPrefixPhase(Prefix) == ESuperSLMPrefixPhase::Ready ||
				Cpu.GetPrefixPhase(Prefix) == ESuperSLMPrefixPhase::Faulted;
		}, Timeout) && Cpu.IsPrefixReady(Prefix));
	}

	bool Run(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq,
		const TArray<int32>& Prompt, int32 Tokens, double Timeout = 180.0)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = Tokens;
		FString Error;
		if (!Test.TestTrue(*FString::Printf(TEXT("begin generation: %s"), *Error),
			Cpu.BeginGeneration(Seq, Request, Error))) { return false; }
		return Test.TestTrue(TEXT("generation completes"), Until(Cpu, [&]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}, Timeout) && Cpu.GetPhase(Seq) == ESuperSLMSequencePhase::Complete);
	}

	bool ResetAndAdopt(FAutomationTestBase& Test, USuperSLMSubsystem& Cpu,
		const FSuperSLMSequence& Seq, const FSuperSLMPrefix& Prefix)
	{
		FSuperSLMLifecycleOpHandle Reset;
		FString Error;
		if (!Test.TestEqual(TEXT("reset queues"), (uint8)Cpu.ResetSequence(Seq, Reset, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		if (!Test.TestTrue(TEXT("reset drains"), Until(Cpu, [&]()
		{
			return Cpu.GetLifecycleOpResult(Reset) != ESuperSLMRestoreResult::Pending;
		}, 60.0) && Cpu.GetLifecycleOpResult(Reset) == ESuperSLMRestoreResult::Success)) { return false; }
		FSuperSLMLifecycleOpHandle Adopt;
		if (!Test.TestEqual(TEXT("adopt queues"), (uint8)Cpu.AdoptPrefix(Seq, Prefix, Adopt, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		return Test.TestTrue(TEXT("adopt drains"), Until(Cpu, [&]()
		{
			return Cpu.GetLifecycleOpResult(Adopt) != ESuperSLMRestoreResult::Pending;
		}, 60.0) && Cpu.GetLifecycleOpResult(Adopt) == ESuperSLMRestoreResult::Success);
	}

	// Ledger is the caller's own copy (GetJobLedger() returns one), so the row outlives the call.
	const FSuperSLMWorkerJobReport* FirstDecode(const TArray<FSuperSLMWorkerJobReport>& Ledger, int32 From)
	{
		for (int32 I = From; I < Ledger.Num(); ++I)
		{
			if (Ledger[I].Kind == ESuperSLMWorkerJobKind::DecodeOrPrefill && Ledger[I].DecodeLayers > 0)
			{
				return &Ledger[I];
			}
		}
		return nullptr;
	}

	int64 PositionSum(int32 Chunk, int64 Start)
	{
		return static_cast<int64>(Chunk) * Start + static_cast<int64>(Chunk) * (Chunk - 1) / 2;
	}

	int32 ExpectedChunk(int32 Remaining, int64 Start, const FSuperSLMRuntimeConfig& Config)
	{
		int32 Best = 1;
		for (int32 Count = 2; Count <= FMath::Min(Config.MaxPrefillChunkBudget, Remaining); ++Count)
		{
			const double Price = Count * Config.PromptTokenCostMs +
				PositionSum(Count, Start) * Config.PromptTokenCostPerPositionMs;
			if (Price > Config.TickBudgetMs * Config.BudgetHeadroom) { break; }
			Best = Count;
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuDepthPricingTest,
	"SuperSLM.U1.Cpu.DepthPricing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CpuDepthPricingTest::RunTest(const FString& Parameters)
{
	// Reads the ledger by row number from the start of the run, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 2;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 16.6;
	Config.BudgetHeadroom = 0.5;
	Config.LayerCostMs = 2.0;
	Config.LayerCostPerPositionMs = 0.0015;
	Config.PromptTokenCostMs = 1.0;
	Config.PromptTokenCostPerPositionMs = 0.0015;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SetUp(*this, World, Cpu, Model, Config)) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	const int32 ShallowStart = Cpu->GetJobLedger().Num();
	if (!Run(*this, *Cpu, Seq, { 1 }, 2)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> ShallowLedger = Cpu->GetJobLedger();
	const FSuperSLMWorkerJobReport* Shallow = FirstDecode(ShallowLedger, ShallowStart);
	if (!TestNotNull(TEXT("depth-1 decode row"), Shallow)) { return false; }
	bool bOk = true;
	bOk &= TestEqual(TEXT("depth-1 free decode selects four layers"), Shallow->DecodeLayers, 4);
	bOk &= TestEqual(TEXT("depth-1 decode positions"), Shallow->DecodeLayerDepthSum, int64(4));
	bOk &= TestTrue(TEXT("depth-1 decode exact planned price 8.006 ms"), FMath::IsNearlyEqual(Shallow->PlannedJobMs, 8.006, 1e-6));

	FSuperSLMPrefix DeepPrefix;
	if (!MakePrefix(*this, *Cpu, 1024, DeepPrefix)) { return false; }
	if (!ResetAndAdopt(*this, *Cpu, Seq, DeepPrefix)) { return false; }
	const int32 DeepStart = Cpu->GetJobLedger().Num();
	if (!Run(*this, *Cpu, Seq, {}, 2)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> DeepLedger = Cpu->GetJobLedger();
	const FSuperSLMWorkerJobReport* Deep = FirstDecode(DeepLedger, DeepStart);
	if (!TestNotNull(TEXT("depth-1024 free decode row"), Deep)) { return false; }
	bOk &= TestEqual(TEXT("depth-1024 free decode selects two layers"), Deep->DecodeLayers, 2);
	bOk &= TestEqual(TEXT("depth-1024 decode positions"), Deep->DecodeLayerDepthSum, int64(2048));
	bOk &= TestTrue(TEXT("depth-1024 decode exact planned price 7.072 ms"), FMath::IsNearlyEqual(Deep->PlannedJobMs, 7.072, 1e-6));

	if (!ResetAndAdopt(*this, *Cpu, Seq, DeepPrefix)) { return false; }
	Cpu->SetLayerBudget(Seq, 6);
	const int32 PinnedStart = Cpu->GetJobLedger().Num();
	if (!Run(*this, *Cpu, Seq, {}, 2)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> PinnedLedger = Cpu->GetJobLedger();
	const FSuperSLMWorkerJobReport* Pinned = FirstDecode(PinnedLedger, PinnedStart);
	if (!TestNotNull(TEXT("depth-1024 pinned decode row"), Pinned)) { return false; }
	bOk &= TestEqual(TEXT("pinned decode runs six layers"), Pinned->DecodeLayers, 6);
	bOk &= TestEqual(TEXT("pinned decode positions"), Pinned->DecodeLayerDepthSum, int64(6144));
	bOk &= TestTrue(TEXT("pinned decode exact planned price 21.216 ms"), FMath::IsNearlyEqual(Pinned->PlannedJobMs, 21.216, 1e-6));
	bOk &= TestEqual(TEXT("pinned depth price requires K=2"), Pinned->K, 2);

	if (!ResetAndAdopt(*this, *Cpu, Seq, DeepPrefix)) { return false; }
	const int32 PrefillStart = Cpu->GetJobLedger().Num();
	TArray<int32> Suffix;
	Suffix.Init(1, 10);
	if (!Run(*this, *Cpu, Seq, Suffix, 1)) { return false; }
	const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger();
	const FSuperSLMWorkerJobReport* FirstPrefill = nullptr;
	for (int32 I = PrefillStart; I < Ledger.Num(); ++I)
	{
		if (Ledger[I].PromptTokens > 0) { FirstPrefill = &Ledger[I]; break; }
	}
	if (!TestNotNull(TEXT("depth-1024 sequence prefill row"), FirstPrefill)) { return false; }
	bOk &= TestEqual(TEXT("sequence prefill chunk has three tokens"), FirstPrefill->PromptTokens, 3);
	bOk &= TestEqual(TEXT("sequence prefill positions"), FirstPrefill->PromptPositionSum, int64(3075));
	bOk &= TestTrue(TEXT("sequence prefill exact price 7.6125 ms"), FMath::IsNearlyEqual(FirstPrefill->PlannedJobMs, 7.6125, 1e-6));

	const int32 PrefixStart = Cpu->GetJobLedger().Num();
	FSuperSLMPrefix LongPrefix;
	if (!MakePrefix(*this, *Cpu, 1030, LongPrefix)) { return false; }
	int32 Consumed = 0;
	const TArray<FSuperSLMWorkerJobReport> PrefixLedger = Cpu->GetJobLedger();
	for (int32 I = PrefixStart; I < PrefixLedger.Num(); ++I)
	{
		const FSuperSLMWorkerJobReport& Job = PrefixLedger[I];
		if (Job.Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || Job.PromptTokens == 0 ||
			!Job.MemberSequences.IsEmpty()) { continue; }
		const int32 Expected = ExpectedChunk(1030 - Consumed, Consumed, Config);
		bOk &= TestEqual(TEXT("prefix chunk is largest fitting at its own start position"), Job.PromptTokens, Expected);
		bOk &= TestEqual(TEXT("prefix chunk reports served positions"), Job.PromptPositionSum, PositionSum(Job.PromptTokens, Consumed));
		bOk &= TestTrue(TEXT("prefix chunk has exact depth-priced cost"), FMath::IsNearlyEqual(Job.PlannedJobMs,
			Job.PromptTokens * Config.PromptTokenCostMs + PositionSum(Job.PromptTokens, Consumed) * Config.PromptTokenCostPerPositionMs, 1e-6));
		Consumed += Job.PromptTokens;
	}
	bOk &= TestEqual(TEXT("prefix chunks cover all 1030 positions"), Consumed, 1030);
	Cpu->ReturnSequence(Seq);
	Cpu->ReleasePrefix(DeepPrefix);
	Cpu->ReleasePrefix(LongPrefix);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CpuDeepContextFitTest,
	"SuperSLM.U1.Cpu.DeepContextFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CpuDeepContextFitTest::RunTest(const FString& Parameters)
{
	// Reads the ledger by row number from the start of each generation, so it keeps the whole run.
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity HistoryCapacity;
	FSuperSLMRuntimeConfig Config; // item 5c's shipped costs, never replaced by readings from this cell
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 1;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 16.6;
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SetUp(*this, World, Cpu, Model, Config)) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	const double Share = Config.TickBudgetMs * Config.BudgetHeadroom;
	bool bOk = true;
	auto Grade = [&](int64 Depth, int32 Start)
	{
		TArray<double> Times;
		int32 Overruns = 0;
		const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger();
		for (int32 I = Start; I < Ledger.Num(); ++I)
		{
			const FSuperSLMWorkerJobReport& Job = Ledger[I];
			if (Job.Kind != ESuperSLMWorkerJobKind::DecodeOrPrefill || Job.PromptTokens != 0 ||
				Job.DeliveredAtTick < 0) { continue; }
			if (Job.DecodeLayers > 0)
			{
				bOk &= TestTrue(TEXT("decode row prices a context at or beyond the starting depth"),
					Job.DecodeLayerDepthSum >= static_cast<int64>(Job.DecodeLayers) * Depth);
			}
			bOk &= TestTrue(TEXT("decode worker call completed"), Job.WorkerCallMs >= 0.0);
			Times.Add(Job.WorkerCallMs);
			if (Job.bWorkerOverran) { ++Overruns; }
		}
		bOk &= TestTrue(TEXT("decode jobs were measured"), !Times.IsEmpty());
		if (!Times.IsEmpty())
		{
			Times.Sort();
			const double Median = Times[Times.Num() / 2];
			bOk &= TestTrue(*FString::Printf(TEXT("depth %lld median worker decode %.6f ms fits %.6f ms share"), Depth, Median, Share),
				Median <= Share);
			AddInfo(FString::Printf(TEXT("depth %lld: %d decode jobs, median WorkerCallMs %.6f ms, share %.6f ms, overruns %d"),
				Depth, Times.Num(), Median, Share, Overruns));
		}
	};
	const int32 ShallowStart = Cpu->GetJobLedger().Num();
	if (!Run(*this, *Cpu, Seq, { 1 }, 32, 600.0)) { return false; }
	bOk &= TestEqual(TEXT("shallow generation delivered 32 tokens"), Cpu->GetGeneratedTokens(Seq).Num(), 32);
	Grade(1, ShallowStart);
	FSuperSLMPrefix Prefix;
	if (!MakePrefix(*this, *Cpu, 3072, Prefix, 600.0) || !ResetAndAdopt(*this, *Cpu, Seq, Prefix)) { return false; }
	const int32 DeepStart = Cpu->GetJobLedger().Num();
	if (!Run(*this, *Cpu, Seq, {}, 32, 600.0)) { return false; }
	bOk &= TestEqual(TEXT("deep generation delivered 32 tokens"), Cpu->GetGeneratedTokens(Seq).Num(), 32);
	Grade(3072, DeepStart);
	Cpu->ReturnSequence(Seq);
	Cpu->ReleasePrefix(Prefix);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
