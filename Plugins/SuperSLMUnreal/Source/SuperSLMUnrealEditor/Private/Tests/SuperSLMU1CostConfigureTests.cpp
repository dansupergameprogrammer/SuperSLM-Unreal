#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSubsystem.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Tests/AutomationCommon.h"
#include <cfloat>
#include <limits>

using namespace SuperSLML2S1Fixtures;

namespace
{
	constexpr int32 kAExContextCap = 4096; // A-EX conversion cap, plan §8/§12 decision 9.
	constexpr int32 kPlanMaxJobTicks = 1 << 20; // Independent plan §5 limit.

	const TCHAR* PhaseName(ESuperSLMSequencePhase Phase)
	{
		switch (Phase)
		{
			case ESuperSLMSequencePhase::Idle: return TEXT("Idle");
			case ESuperSLMSequencePhase::Prefilling: return TEXT("Prefilling");
			case ESuperSLMSequencePhase::Decoding: return TEXT("Decoding");
			case ESuperSLMSequencePhase::Complete: return TEXT("Complete");
			case ESuperSLMSequencePhase::Faulted: return TEXT("Faulted");
		}
		return TEXT("Unknown");
	}

	const TCHAR* DecodeOutcomeName(ESuperSLMDecodeOutcome Outcome)
	{
		switch (Outcome)
		{
			case ESuperSLMDecodeOutcome::TokenProduced: return TEXT("TokenProduced");
			case ESuperSLMDecodeOutcome::Generating: return TEXT("Generating");
			case ESuperSLMDecodeOutcome::SchemaDeadEnd: return TEXT("SchemaDeadEnd");
			case ESuperSLMDecodeOutcome::SequenceNoLongerValid: return TEXT("SequenceNoLongerValid");
		}
		return TEXT("Unknown");
	}

	class FSequenceFaultLog : public FOutputDevice
	{
	public:
		explicit FSequenceFaultLog(int64 SequenceId)
			: Prefix(FString::Printf(TEXT("Sequence %lld faulted: "), SequenceId))
		{
			GLog->AddOutputDevice(this);
		}
		~FSequenceFaultLog() { GLog->RemoveOutputDevice(this); }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category == FName(TEXT("LogSuperSLM")))
			{
				const FString Line(V);
				if (Line.StartsWith(Prefix)) { Reason = Line.Mid(Prefix.Len()); }
			}
		}

		FString Reason;

	private:
		FString Prefix;
	};

	FSuperSLMRuntimeConfig SchedulingConfig()
	{
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = 1;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 16.6;
		return Config;
	}

	// Fixed costs keep the boundary cells independent of calibration defaults. The prompt
	// intercept also makes an uncapped twice-cap chunk exceed the capped prefill bound.
	FSuperSLMRuntimeConfig CostBoundaryConfig()
	{
		FSuperSLMRuntimeConfig Config = SchedulingConfig();
		Config.LayerCostMs = 2.0;
		Config.LayerCostPerPositionMs = 0.001;
		Config.PromptTokenCostMs = 36.0;
		Config.PromptTokenCostPerPositionMs = 0.001;
		Config.FinishCostMs = 5.0;
		Config.ResetCostMs = 1.0;
		Config.AdoptCostMs = 1.0;
		Config.SaveCostMs = 1.0;
		Config.RestoreCostMs = 1.0;
		Config.PrefixBeginCostMs = 1.0;
		Config.PrefixReleaseCostMs = 1.0;
		return Config;
	}

	bool SchedulingFixture(FAutomationTestBase& Test, FTestWorldWrapper& World,
		USuperSLMSubsystem*& Cpu, USuperSLMModel*& Model)
	{
		FString Path, Reason;
		if (!Test.TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		Model = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		if (!Test.TestNotNull(TEXT("A-EX imports"), Model) ||
			!Test.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted) ||
			!World.CreateTestWorld(EWorldType::Game)) { return false; }
		Cpu = GetSubsystem(World.GetTestWorld());
		return Test.TestNotNull(TEXT("CPU subsystem"), Cpu);
	}

	// The three plan §5 bounds are calculated from fixture inputs, never from the production seam.
	struct FPlanBounds { double Decode; double Prefill; double Lifecycle; };
	FPlanBounds PlanBounds(const FSuperSLMRuntimeConfig& C)
	{
		const double B = C.BlockCount, M = C.MaxLayerBudget;
		const double P = FMath::Min(C.MaxPrefillChunkBudget, kAExContextCap);
		const double D = kAExContextCap - 1;
		const double Decode = B * (M * (C.LayerCostMs + C.LayerCostPerPositionMs * kAExContextCap) + C.FinishCostMs);
		const double Prefill = P * C.PromptTokenCostMs + C.PromptTokenCostPerPositionMs *
			(P * (D - P + 1) + P * (P - 1) / 2);
		const double Lifecycle = FMath::Max(FMath::Max(FMath::Max(C.ResetCostMs, C.AdoptCostMs),
			FMath::Max(C.SaveCostMs, C.RestoreCostMs)), FMath::Max(C.PrefixBeginCostMs, C.PrefixReleaseCostMs));
		return { Decode, Prefill, Lifecycle };
	}

	double LargestPlanBound(const FSuperSLMRuntimeConfig& C)
	{
		const FPlanBounds Bounds = PlanBounds(C);
		return FMath::Max(FMath::Max(Bounds.Decode, Bounds.Prefill), Bounds.Lifecycle);
	}

	bool CheckCostField(FAutomationTestBase& Test, double FSuperSLMRuntimeConfig::* Field,
		const TCHAR* Name, bool bAllowsZero)
	{
		FString Path, Reason;
		if (!Test.TestTrue(TEXT("A-EX present"), TryGetAExArtifactPath(Path, Reason))) { return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diagnostic);
		if (!Test.TestNotNull(TEXT("A-EX imports"), Model) || !Test.TestTrue(TEXT("A-EX accepted"), Diagnostic.bAccepted)) { return false; }
		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
		USuperSLMSubsystem* Cpu = GetSubsystem(World.GetTestWorld());
		if (!Test.TestNotNull(TEXT("CPU subsystem"), Cpu)) { return false; }
		const double Values[] = { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() };
		bool bOk = true;
		for (int32 I = 0; I < 4; ++I)
		{
			FSuperSLMRuntimeConfig Config;
			Config.BlockCount = 1;
			Config.MaxLayerBudget = 24;
			Config.MaxPrefillChunkBudget = 64;
			Config.MaxSequencesPerDecodeCall = 1;
			Config.SequenceLifecycleBudgetMs = 1000.0;
			Config.TickBudgetMs = 16.6;
			Config.*Field = Values[I];
			const FSuperSLMConfigureReport Report = Cpu->Configure(Model, Config);
			const bool bAccepted = I == 0 && bAllowsZero;
			bOk &= Test.TestEqual(*FString::Printf(TEXT("%s case %d result"), Name, I), (uint8)Report.Result,
				(uint8)(bAccepted ? ESuperSLMConfigureResult::Success : ESuperSLMConfigureResult::InvalidCallShape));
			if (!bAccepted)
			{
				bOk &= Test.TestTrue(*FString::Printf(TEXT("%s case %d refusal names field"), Name, I),
					Report.Message.Contains(Name));
			}
		}
		return bOk;
	}
}

#define SSU_COST_FIELD_CELL(Suffix, Member, AllowsZero) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1Cost##Suffix##Test, \
		"SuperSLM.U1.Cpu.ConfigureCost." #Suffix, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter) \
	bool FSuperSLMU1Cost##Suffix##Test::RunTest(const FString& Parameters) \
	{ return CheckCostField(*this, &FSuperSLMRuntimeConfig::Member, TEXT(#Member), AllowsZero); }

SSU_COST_FIELD_CELL(LayerCostMs, LayerCostMs, false)
SSU_COST_FIELD_CELL(LayerCostPerPositionMs, LayerCostPerPositionMs, true)
SSU_COST_FIELD_CELL(PromptTokenCostMs, PromptTokenCostMs, false)
SSU_COST_FIELD_CELL(PromptTokenCostPerPositionMs, PromptTokenCostPerPositionMs, true)
SSU_COST_FIELD_CELL(FinishCostMs, FinishCostMs, false)
SSU_COST_FIELD_CELL(ResetCostMs, ResetCostMs, false)
SSU_COST_FIELD_CELL(AdoptCostMs, AdoptCostMs, false)
SSU_COST_FIELD_CELL(SaveCostMs, SaveCostMs, false)
SSU_COST_FIELD_CELL(RestoreCostMs, RestoreCostMs, false)
SSU_COST_FIELD_CELL(PrefixBeginCostMs, PrefixBeginCostMs, false)
SSU_COST_FIELD_CELL(PrefixReleaseCostMs, PrefixReleaseCostMs, false)

#undef SSU_COST_FIELD_CELL

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1TickBudgetNonFiniteTest,
	"SuperSLM.U1.Cpu.TickBudgetNonFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1TickBudgetNonFiniteTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SchedulingFixture(*this, World, Cpu, Model)) { return false; }
	const double Values[] = { std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
	bool bOk = true;
	for (int32 I = 0; I < 3; ++I)
	{
		FSuperSLMRuntimeConfig C = SchedulingConfig();
		C.TickBudgetMs = Values[I];
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("CPU non-finite case %d result"), I),
			(uint8)R.Result, (uint8)ESuperSLMConfigureResult::InvalidCallShape);
		bOk &= TestTrue(*FString::Printf(TEXT("CPU non-finite case %d names TickBudgetMs"), I),
			R.Message.Contains(TEXT("TickBudgetMs")) && R.Message.Contains(TEXT("finite")));
	}
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMGpuRuntimeConfig G;
	G.ContextCap = kAExContextCap;
	G.BlockCount = 1;
	G.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(4);
	G.K = 24;
	G.TickBudgetMs = std::numeric_limits<double>::infinity();
	const FSuperSLMGpuConfigureReport GR = Gpu->Configure(Model, G);
	bOk &= TestEqual(TEXT("GPU +inf result"), (uint8)GR.Result,
		(uint8)ESuperSLMGpuConfigureResult::InvalidTickBudget);
	bOk &= TestTrue(TEXT("GPU +inf names finite TickBudgetMs"),
		GR.Message.Contains(TEXT("TickBudgetMs")) && GR.Message.Contains(TEXT("finite")));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CostDomainRefusalsTest,
	"SuperSLM.U1.Cpu.CostDomainRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CostDomainRefusalsTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SchedulingFixture(*this, World, Cpu, Model)) { return false; }
	struct FCase { const TCHAR* Name; double FSuperSLMRuntimeConfig::* Field; const TCHAR* Kind; };
	const FCase Cases[] = {
		{ TEXT("LayerCostMs"), &FSuperSLMRuntimeConfig::LayerCostMs, TEXT("decode") },
		{ TEXT("FinishCostMs"), &FSuperSLMRuntimeConfig::FinishCostMs, TEXT("decode") },
		{ TEXT("PromptTokenCostMs"), &FSuperSLMRuntimeConfig::PromptTokenCostMs, TEXT("prefill") },
		{ TEXT("ResetCostMs"), &FSuperSLMRuntimeConfig::ResetCostMs, TEXT("lifecycle") },
		{ TEXT("AdoptCostMs"), &FSuperSLMRuntimeConfig::AdoptCostMs, TEXT("lifecycle") },
		{ TEXT("SaveCostMs"), &FSuperSLMRuntimeConfig::SaveCostMs, TEXT("lifecycle") },
		{ TEXT("RestoreCostMs"), &FSuperSLMRuntimeConfig::RestoreCostMs, TEXT("lifecycle") },
		{ TEXT("PrefixBeginCostMs"), &FSuperSLMRuntimeConfig::PrefixBeginCostMs, TEXT("lifecycle") },
		{ TEXT("PrefixReleaseCostMs"), &FSuperSLMRuntimeConfig::PrefixReleaseCostMs, TEXT("lifecycle") },
		{ TEXT("LayerCostPerPositionMs"), &FSuperSLMRuntimeConfig::LayerCostPerPositionMs, TEXT("decode") },
		{ TEXT("PromptTokenCostPerPositionMs"), &FSuperSLMRuntimeConfig::PromptTokenCostPerPositionMs, TEXT("prefill") },
	};
	bool bOk = true;
	for (const FCase& Case : Cases)
	{
		FSuperSLMRuntimeConfig C = SchedulingConfig();
		C.*Case.Field = DBL_MAX;
		const FPlanBounds Bounds = PlanBounds(C);
		const double Target = FCString::Strcmp(Case.Kind, TEXT("decode")) == 0 ? Bounds.Decode :
			(FCString::Strcmp(Case.Kind, TEXT("prefill")) == 0 ? Bounds.Prefill : Bounds.Lifecycle);
		bOk &= TestTrue(*FString::Printf(TEXT("%s plan bound exceeds K limit"), Case.Name),
			!FMath::IsFinite(Target) || Target / C.TickBudgetMs > kPlanMaxJobTicks);
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("%s DBL_MAX result"), Case.Name),
			(uint8)R.Result, (uint8)ESuperSLMConfigureResult::InvalidCostDomain);
		bOk &= TestTrue(*FString::Printf(TEXT("%s DBL_MAX names job kind"), Case.Name),
			R.Message.Contains(Case.Kind) && R.Message.Contains(TEXT("K =")));
	}
	FSuperSLMRuntimeConfig Overflow = SchedulingConfig();
	Overflow.LayerCostPerPositionMs = 1e305;
	bOk &= TestFalse(TEXT("finite slope overflows independent decode bound"),
		FMath::IsFinite(PlanBounds(Overflow).Decode));
	const FSuperSLMConfigureReport OR = Cpu->Configure(Model, Overflow);
	bOk &= TestEqual(TEXT("finite slope whose depth product overflows"), (uint8)OR.Result,
		(uint8)ESuperSLMConfigureResult::InvalidCostDomain);
	bOk &= TestTrue(TEXT("overflow refusal names decode and K"),
		OR.Message.Contains(TEXT("decode")) && OR.Message.Contains(TEXT("K =")));
	const double TinyBudgets[] = { 1e-300, std::numeric_limits<double>::denorm_min() };
	for (int32 I = 0; I < 2; ++I)
	{
		FSuperSLMRuntimeConfig C = SchedulingConfig();
		C.TickBudgetMs = TinyBudgets[I];
		bOk &= TestTrue(*FString::Printf(TEXT("tiny budget %d exceeds independent K limit"), I),
			PlanBounds(C).Decode / C.TickBudgetMs > kPlanMaxJobTicks);
		const FSuperSLMConfigureReport R = Cpu->Configure(Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("tiny budget %d result"), I), (uint8)R.Result,
			(uint8)ESuperSLMConfigureResult::InvalidCostDomain);
		bOk &= TestTrue(*FString::Printf(TEXT("tiny budget %d names K"), I),
			R.Message.Contains(TEXT("K =")) && R.Message.Contains(TEXT("kMaxJobTicks")));
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CostDomainBoundaryTest,
	"SuperSLM.U1.Cpu.CostDomainBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CostDomainBoundaryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SchedulingFixture(*this, World, Cpu, Model)) { return false; }
	auto CheckBoundary = [this, Cpu, Model](const TCHAR* Name, FSuperSLMRuntimeConfig C, const TCHAR* Kind)
	{
		const FPlanBounds Bounds = PlanBounds(C);
		const double Bound = LargestPlanBound(C);
		const double Target = FCString::Strcmp(Kind, TEXT("decode")) == 0 ? Bounds.Decode :
			(FCString::Strcmp(Kind, TEXT("prefill")) == 0 ? Bounds.Prefill : Bounds.Lifecycle);
		const double Floor = Bound / static_cast<double>(kPlanMaxJobTicks);
		if (!TestTrue(*FString::Printf(TEXT("%s independently dominant %s bound"), Name, Kind),
			Target == Bound && Target > 0.0 &&
			(FCString::Strcmp(Kind, TEXT("decode")) == 0 || Target > Bounds.Decode) &&
			(FCString::Strcmp(Kind, TEXT("prefill")) == 0 || Target > Bounds.Prefill) &&
			(FCString::Strcmp(Kind, TEXT("lifecycle")) == 0 || Target > Bounds.Lifecycle) &&
			FMath::IsFinite(Bound) && FMath::IsFinite(Floor) && Floor > 0.0)) { return false; }
		C.TickBudgetMs = Floor * (1.0 + 1e-9);
		const FSuperSLMConfigureReport Accepted = Cpu->Configure(Model, C);
		bool bCaseOk = TestEqual(*FString::Printf(TEXT("%s above-floor result"), Name),
			(uint8)Accepted.Result, (uint8)ESuperSLMConfigureResult::Success);
		C.TickBudgetMs = Floor * (1.0 - 1e-6);
		const FSuperSLMConfigureReport Refused = Cpu->Configure(Model, C);
		bCaseOk &= TestEqual(*FString::Printf(TEXT("%s below-floor result"), Name),
			(uint8)Refused.Result, (uint8)ESuperSLMConfigureResult::InvalidCostDomain);
		bCaseOk &= TestTrue(*FString::Printf(TEXT("%s refusal names %s, K and limit"), Name, Kind),
			Refused.Message.Contains(Kind) && Refused.Message.Contains(TEXT("K =")) &&
			Refused.Message.Contains(TEXT("kMaxJobTicks")));
		return bCaseOk;
	};
	bool bOk = CheckBoundary(TEXT("baseline prefill"), CostBoundaryConfig(), TEXT("prefill"));
	FSuperSLMRuntimeConfig Decode = CostBoundaryConfig();
	Decode.BlockCount = 4;
	Decode.MaxLayerBudget = 24;
	Decode.MaxPrefillChunkBudget = 1;
	bOk &= CheckBoundary(TEXT("four-sequence depth-cap decode"), Decode, TEXT("decode"));
	struct FOpCost { const TCHAR* Name; double FSuperSLMRuntimeConfig::* Field; };
	const FOpCost OpCosts[] = {
		{ TEXT("ResetCostMs"), &FSuperSLMRuntimeConfig::ResetCostMs },
		{ TEXT("AdoptCostMs"), &FSuperSLMRuntimeConfig::AdoptCostMs },
		{ TEXT("SaveCostMs"), &FSuperSLMRuntimeConfig::SaveCostMs },
		{ TEXT("RestoreCostMs"), &FSuperSLMRuntimeConfig::RestoreCostMs },
		{ TEXT("PrefixBeginCostMs"), &FSuperSLMRuntimeConfig::PrefixBeginCostMs },
		{ TEXT("PrefixReleaseCostMs"), &FSuperSLMRuntimeConfig::PrefixReleaseCostMs },
	};
	for (const FOpCost& Op : OpCosts)
	{
		FSuperSLMRuntimeConfig Lifecycle = CostBoundaryConfig();
		const FPlanBounds OtherBounds = PlanBounds(Lifecycle);
		Lifecycle.*Op.Field = 2.0 * FMath::Max(OtherBounds.Decode, OtherBounds.Prefill);
		bOk &= CheckBoundary(Op.Name, Lifecycle, TEXT("lifecycle"));
	}
	FSuperSLMRuntimeConfig PrefillCap = CostBoundaryConfig();
	PrefillCap.MaxPrefillChunkBudget = 2 * kAExContextCap;
	const double UncappedP = PrefillCap.MaxPrefillChunkBudget;
	const double D = kAExContextCap - 1;
	const double UncappedPrefill = UncappedP * PrefillCap.PromptTokenCostMs +
		PrefillCap.PromptTokenCostPerPositionMs *
		(UncappedP * (D - UncappedP + 1) + UncappedP * (UncappedP - 1) / 2);
	bOk &= TestTrue(TEXT("twice-cap fixture distinguishes uncapped prefill"),
		UncappedPrefill > PlanBounds(PrefillCap).Prefill * (1.0 + 1e-9));
	bOk &= CheckBoundary(TEXT("prefill chunk twice context cap"), PrefillCap, TEXT("prefill"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuKAboveMaximumTest,
	"SuperSLM.U1.Gpu.KAboveMaximum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1GpuKAboveMaximumTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SchedulingFixture(*this, World, Cpu, Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	FSuperSLMGpuRuntimeConfig G;
	G.ContextCap = kAExContextCap;
	G.BlockCount = 1;
	G.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(4);
	G.TickBudgetMs = 16.6;
	G.K = kPlanMaxJobTicks;
	const FSuperSLMGpuConfigureReport Accepted = Gpu->Configure(Model, G);
	bool bOk = TestEqual(TEXT("GPU K at plan maximum accepted"), (uint8)Accepted.Result,
		(uint8)ESuperSLMGpuConfigureResult::Success);
	G.K = kPlanMaxJobTicks + 1;
	const FSuperSLMGpuConfigureReport Refused = Gpu->Configure(Model, G);
	bOk &= TestEqual(TEXT("GPU K above plan maximum refused"), (uint8)Refused.Result,
		(uint8)ESuperSLMGpuConfigureResult::KAboveMaximum);
	bOk &= TestTrue(TEXT("GPU K refusal names K and maximum"),
		Refused.Message.Contains(TEXT("K")) && Refused.Message.Contains(TEXT("kMaxJobTicks")));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1CheckedKAndTickTest,
	"SuperSLM.U1.Cpu.CheckedKAndTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1CheckedKAndTickTest::RunTest(const FString& Parameters)
{
	bool bOk = TestEqual(TEXT("seam max K equals independent plan limit"),
		FSuperSLMSchedulingTestAccess::MaxJobTicks(), kPlanMaxJobTicks);
	const double InvalidCosts[] = { std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN(), -1.0, static_cast<double>(kPlanMaxJobTicks) + 1.0 };
	for (int32 I = 0; I < 4; ++I)
	{
		int32 K = -99;
		bOk &= TestFalse(*FString::Printf(TEXT("invalid planned cost %d refused"), I),
			FSuperSLMSchedulingTestAccess::TryComputeK(InvalidCosts[I], 1.0, K));
		bOk &= TestEqual(*FString::Printf(TEXT("invalid planned cost %d preserves K"), I), K, -99);
	}
	int32 K = -99;
	bOk &= TestTrue(TEXT("K at plan limit accepted"), FSuperSLMSchedulingTestAccess::TryComputeK(
		static_cast<double>(kPlanMaxJobTicks), 1.0, K));
	bOk &= TestEqual(TEXT("K equals plan limit"), K, kPlanMaxJobTicks);
	const int32 LastTick = std::numeric_limits<int32>::max();
	int32 Committed = -99;
	bOk &= TestTrue(TEXT("last representable delivery tick accepted"),
		FSuperSLMSchedulingTestAccess::TryCommitTick(LastTick - 5, 5, Committed));
	bOk &= TestEqual(TEXT("last committed tick is INT32_MAX"), Committed, LastTick);
	Committed = -99;
	bOk &= TestFalse(TEXT("delivery beyond INT32_MAX refused"),
		FSuperSLMSchedulingTestAccess::TryCommitTick(LastTick - 5, 6, Committed));
	bOk &= TestEqual(TEXT("overflow refusal preserves output"), Committed, -99);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1TickHorizonFaultsTest,
	"SuperSLM.U1.Cpu.TickHorizonFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMU1TickHorizonFaultsTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	USuperSLMSubsystem* Cpu = nullptr;
	USuperSLMModel* Model = nullptr;
	if (!SchedulingFixture(*this, World, Cpu, Model)) { return false; }
	FSuperSLMRuntimeConfig C = SchedulingConfig();
	if (!TestEqual(TEXT("initial Configure"), (uint8)Cpu->Configure(Model, C).Result,
		(uint8)ESuperSLMConfigureResult::Success)) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend live sequence"), (uint8)Cpu->VendSequence(Seq),
		(uint8)ESuperSLMVendResult::Success)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = { 1 };
	Request.MaxNewTokens = 1024;
	FString Error;
	FSequenceFaultLog FaultLog(Seq.Id);
	if (!TestTrue(TEXT("generation begins"), Cpu->BeginGeneration(Seq, Request, Error))) { return false; }
	const double Deadline = FPlatformTime::Seconds() + 120.0;
	ESuperSLMSequencePhase Phase = Cpu->GetPhase(Seq);
	while (Phase != ESuperSLMSequencePhase::Decoding &&
		Phase != ESuperSLMSequencePhase::Complete &&
		Phase != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() < Deadline)
	{
		Cpu->Tick(1.0f / 60.0f);
		Phase = Cpu->GetPhase(Seq);
		FPlatformProcess::Sleep(0.001f);
	}
	const ESuperSLMDecodeOutcome Outcome = Cpu->GetLastDecodeOutcome(Seq);
	if (!TestTrue(*FString::Printf(
		TEXT("sequence is decoding before horizon injection (phase=%s, last decode outcome=%s, fault reason=%s)"),
		PhaseName(Phase), DecodeOutcomeName(Outcome), FaultLog.Reason.IsEmpty() ? TEXT("none logged") : *FaultLog.Reason),
		Phase == ESuperSLMSequencePhase::Decoding)) { return false; }
	const int32 Horizon = std::numeric_limits<int32>::max() - kPlanMaxJobTicks;
	bool bOk = TestEqual(TEXT("seam horizon equals independent plan value"),
		FSuperSLMSchedulingTestAccess::TickHorizon(), Horizon);
	if (!TestTrue(TEXT("set counter to horizon minus two"),
		FSuperSLMSchedulingTestAccess::SetTickCounter(*Cpu, Horizon - 2))) { return false; }
	Cpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("first tick advances"), FSuperSLMSchedulingTestAccess::GetTickCounter(*Cpu), Horizon - 1);
	Cpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("second tick reaches horizon"), FSuperSLMSchedulingTestAccess::GetTickCounter(*Cpu), Horizon);
	AddExpectedErrorPlain(FString::Printf(TEXT("SuperSLM: tick horizon reached after %d ticks: call Configure() again"), Horizon),
		EAutomationExpectedErrorFlags::Contains, 1);
	Cpu->Tick(1.0f / 60.0f);
	bOk &= TestEqual(TEXT("live sequence faults at horizon"), (uint8)Cpu->GetPhase(Seq),
		(uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("counter stops at horizon"), FSuperSLMSchedulingTestAccess::GetTickCounter(*Cpu), Horizon);
	Error.Reset();
	bOk &= TestFalse(TEXT("new generation request refused at horizon"), Cpu->BeginGeneration(Seq, Request, Error));
	bOk &= TestTrue(TEXT("request carries exact horizon remedy"), Error.Contains(
		*FString::Printf(TEXT("tick horizon reached after %d ticks: call Configure() again"), Horizon)));
	bOk &= TestEqual(TEXT("reconfigure clears horizon"), (uint8)Cpu->Configure(Model, C).Result,
		(uint8)ESuperSLMConfigureResult::Success);
	bOk &= TestEqual(TEXT("reconfigure resets tick counter"), FSuperSLMSchedulingTestAccess::GetTickCounter(*Cpu), 0);
	FSuperSLMSequence NewSeq;
	if (!TestEqual(TEXT("vend after reconfigure"), (uint8)Cpu->VendSequence(NewSeq),
		(uint8)ESuperSLMVendResult::Success)) { return false; }
	Request.MaxNewTokens = 2;
	Error.Reset();
	if (!TestTrue(TEXT("generation begins after reconfigure"), Cpu->BeginGeneration(NewSeq, Request, Error))) { return false; }
	const double End = FPlatformTime::Seconds() + 120.0;
	while (Cpu->GetPhase(NewSeq) != ESuperSLMSequencePhase::Complete &&
		Cpu->GetPhase(NewSeq) != ESuperSLMSequencePhase::Faulted && FPlatformTime::Seconds() < End)
	{
		Cpu->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(0.001f);
	}
	bOk &= TestEqual(TEXT("new sequence decodes after reconfigure"), (uint8)Cpu->GetPhase(NewSeq),
		(uint8)ESuperSLMSequencePhase::Complete);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
