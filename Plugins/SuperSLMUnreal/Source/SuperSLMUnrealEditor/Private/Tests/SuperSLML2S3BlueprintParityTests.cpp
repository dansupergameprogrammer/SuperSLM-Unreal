// T-2818 (L2-S3 suite). R-S3b (the plan §9; D-SLM7347): "Blueprint
// and inspection do not change results -- R-S1a's first 5 prompts driven from Blueprint with the
// inspection panel polling -> token-identical to R-S1a. On A-EX, a shared prefix created, adopted
// and released from Blueprint, then one suffix generated -> token-identical to the same steps
// through the C++ API."
//
// Two tests, one per arm. Record: the test record §10.
//
// "From Blueprint" is USuperSLMBlueprintLibrary's own functions, called directly. The wrapper
// bodies -- the handle and enum mapping to and from the native API -- are what can be wrong; the
// UHT-generated exec thunk in front of them only unpacks parameters.
//
// Assumed signatures: USuperSLMBlueprintLibrary's current VendSequence, ReturnSequence,
// BeginGeneration, GetPhase, GetGeneratedTokens, GetStats, Tick, CreatePrefix, GetPrefixPhase,
// AdoptPrefix, GetLifecycleOpResult and ReleasePrefix, each taking the CPU USuperSLMSubsystem*.
// The implementation is moving the unified interface into the runtime module in parallel; if these move
// with it, the calls move, and the assertions do not change.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMBlueprintLibrary.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Tests/AutomationCommon.h"

namespace
{
	constexpr float StepSeconds = 1.0f / 60.0f;

	// Runs the game thread's queued tasks, so async completions (the inspector's OnDone) are
	// delivered while the test's own loop runs, as they would be between editor frames.
	void PumpGameThreadTasks()
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
}

// ---------------------------------------------------------------------------------------------
// Arm (a): R-S1a's first 5 prompts through the Blueprint library, with the inspection panel's
// reads running throughout.
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3BlueprintParityTest,
	"SuperSLM.L2S3.BlueprintParity.FirstFivePromptsTokenIdenticalToRS1a",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3BlueprintParityTest::RunTest(const FString& Parameters)
{
	TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
	FString LoadError;
	if (!TestTrue(TEXT("load R-S1a reference cases"), SuperSLML2S1Fixtures::LoadReferenceCases(Cases, LoadError)))
	{
		AddError(LoadError);
		return false;
	}
	if (!TestTrue(TEXT("at least 5 reference cases"), Cases.Num() >= 5))
	{
		return false;
	}

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::ACpuArtifactPath(), Diag);
	if (!TestNotNull(TEXT("A-CPU imports"), Model))
	{
		AddError(Diag.Message);
		return false;
	}

	FTestWorldWrapper World;
	if (!TestTrue(TEXT("game world created"), World.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = SuperSLML2S3Fixtures::GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return false;
	}

	// Configure, vend and return go through the raw subsystem: they are not on §6's Blueprint
	// entry-point list. Everything downstream of a vended sequence goes through the library.
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 512;
	Config.MaxLayerBudget = SuperSLML2S1Fixtures::ACpuNumHiddenLayers;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 100000.0;
	if (!TestEqual(TEXT("configure ok"), (uint8)Subsystem->Configure(Model, Config).Result,
			(uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	int32 Failures = 0;
	for (int32 I = 0; I < 5; ++I)
	{
		const SuperSLML2S1Fixtures::FReferenceCase& Case = Cases[I];

		FSuperSLMSequenceBP SeqBP;
		if (!TestEqual(*FString::Printf(TEXT("%s: VendSequence (BP)"), *Case.Id),
				(uint8)USuperSLMBlueprintLibrary::VendSequence(Subsystem, SeqBP), (uint8)ESuperSLMVendResultBP::Success))
		{
			++Failures;
			continue;
		}

		// R-S1a's own request: the recorded prompt ids and T-2783's stop set (plan §9 R-S1a,
		// D-SLM7339). A-CPU carries no tokenizer, so the ids are fed, never re-tokenized. The
		// earlier version of this cell tokenized the prompt text on A-CPU, which fails, and sent
		// no stop set, which would run past a recorded early stop.
		FSuperSLMGenerationRequestBP RequestBP;
		RequestBP.PromptTokens = Case.PromptTokens;
		RequestBP.MaxNewTokens = 48;
		RequestBP.SpanKind = ESuperSLMSpanKindBP::Prompt;
		RequestBP.StopTokenIds = {SuperSLML2S3Fixtures::AExStopTokenImEnd, SuperSLML2S3Fixtures::AExStopTokenEndOfText};

		FString BeginError;
		if (!TestTrue(*FString::Printf(TEXT("%s: BeginGeneration (BP)"), *Case.Id),
				USuperSLMBlueprintLibrary::BeginGeneration(Subsystem, SeqBP, RequestBP, BeginError)))
		{
			AddError(BeginError);
			++Failures;
			continue;
		}

		// The inspection panel's reads (§7 items 1 and 5, as SSuperSLMInspectionPanel issues them):
		// an async model inspection started with the generation and completing during it, plus
		// ReadShape and the Blueprint stats and pool reads every tick. The claim is that none of it
		// perturbs the tokens.
		TSharedRef<TOptional<FSuperSLMModelInspection>> Inspection = MakeShared<TOptional<FSuperSLMModelInspection>>();
		SuperSLMModelInspector::InspectModelAsync(*Model, Config,
			[Inspection](const FSuperSLMModelInspection& Result) { *Inspection = Result; });

		ESuperSLMSequencePhaseBP Phase = ESuperSLMSequencePhaseBP::Prefilling;
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - StartSeconds < 60.0)
		{
			USuperSLMBlueprintLibrary::Tick(Subsystem, StepSeconds);
			PumpGameThreadTasks();

			FSuperSLMModelShapeFacts Shape;
			SuperSLMModelInspector::ReadShape(*Model, Shape);
			USuperSLMBlueprintLibrary::GetLastTickDurationMs(Subsystem);
			USuperSLMBlueprintLibrary::GetStats(Subsystem, SeqBP);
			USuperSLMBlueprintLibrary::GetPoolFreeCount(Subsystem);
			USuperSLMBlueprintLibrary::GetPoolOccupiedCount(Subsystem);

			Phase = USuperSLMBlueprintLibrary::GetPhase(Subsystem, SeqBP);
			if (Phase == ESuperSLMSequencePhaseBP::Complete || Phase == ESuperSLMSequencePhaseBP::Faulted)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(*FString::Printf(TEXT("%s: BP generation completes"), *Case.Id), Phase == ESuperSLMSequencePhaseBP::Complete))
		{
			USuperSLMBlueprintLibrary::ReturnSequence(Subsystem, SeqBP);
			++Failures;
			continue;
		}

		// FEAT oracle: R-S1a's reference, Layer 1's own recorded sslm_generate output (D-SLM7229).
		Failures += !TestEqual(*FString::Printf(TEXT("%s: BP-driven tokens equal R-S1a's recorded output"), *Case.Id),
			USuperSLMBlueprintLibrary::GetGeneratedTokens(Subsystem, SeqBP), Case.ExpectedOutputTokens);
		USuperSLMBlueprintLibrary::ReturnSequence(Subsystem, SeqBP);

		// The inspection really ran: wait (bounded) for its result and require a valid one, so the
		// "polling does not perturb" claim is not made about polling that never happened.
		const double InspectDeadline = FPlatformTime::Seconds() + 60.0;
		while (!Inspection->IsSet() && FPlatformTime::Seconds() < InspectDeadline)
		{
			PumpGameThreadTasks();
			FPlatformProcess::Sleep(0.005f);
		}
		if (TestTrue(*FString::Printf(TEXT("%s: the concurrent model inspection completed"), *Case.Id), Inspection->IsSet()))
		{
			Failures += !TestTrue(*FString::Printf(TEXT("%s: the concurrent model inspection is valid (%s)"), *Case.Id,
				*Inspection->GetValue().Error), Inspection->GetValue().bValid);
		}
		else
		{
			++Failures;
		}
	}
	return Failures == 0;
}

// ---------------------------------------------------------------------------------------------
// Arm (b): a shared prefix created, adopted and released from Blueprint, then one suffix,
// token-identical to the same steps through the C++ API (D-SLM7347).
// ---------------------------------------------------------------------------------------------

namespace
{
	// Two distinct persona prefixes. Adopting the SECOND of two live prefixes is what lets a
	// handle-mapping defect show: a wrapper that resolved any prefix handle to the first live
	// prefix, or dropped the Id, would adopt the wrong content.
	const TCHAR* kPrefixText[2] = {
		TEXT("You are a gruff dwarven blacksmith who answers in short, clipped sentences."),
		TEXT("You are a cheerful elven herbalist who loves describing the smell of every potion."),
	};
	const TCHAR* kSuffixText = TEXT("A traveller asks what you would recommend today.");
	constexpr int32 kSuffixMaxNewTokens = 32;

	struct FPrefixRun
	{
		TArray<int32> Tokens;
		bool bOk = false;
	};

	// Ticks the subsystem through the plain C++ API until Done() or the deadline.
	bool DrainUntil(USuperSLMSubsystem& Subsystem, TFunctionRef<bool()> Done, double Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < Deadline)
		{
			Subsystem.Tick(StepSeconds);
			FPlatformProcess::Sleep(0.001f);
		}
		return Done();
	}

	// The C++ reference: create both prefixes, adopt prefix AdoptIndex (or none when -1), release
	// both, generate the suffix. Every step through USuperSLMSubsystem directly.
	FPrefixRun RunPrefixStepsCpp(FAutomationTestBase& T, USuperSLMSubsystem& S, const TArray<int32> PrefixTokens[2],
		const TArray<int32>& SuffixTokens, int32 AdoptIndex, const FString& Label)
	{
		FPrefixRun Out;
		FString Error;
		FSuperSLMPrefix Prefixes[2];
		for (int32 P = 0; P < 2; ++P)
		{
			if (!T.TestTrue(FString::Printf(TEXT("%s: CreatePrefix %d (%s)"), *Label, P, *Error), S.CreatePrefix(PrefixTokens[P], Prefixes[P], Error)))
			{
				return Out;
			}
		}
		if (!T.TestTrue(Label + TEXT(": both prefixes Ready"),
				DrainUntil(S, [&S, &Prefixes]() { return S.IsPrefixReady(Prefixes[0]) && S.IsPrefixReady(Prefixes[1]); }, 60.0)))
		{
			return Out;
		}
		FSuperSLMSequence Seq;
		if (!T.TestEqual(Label + TEXT(": vend"), (uint8)S.VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return Out;
		}
		if (AdoptIndex >= 0)
		{
			FSuperSLMLifecycleOpHandle Handle;
			if (!T.TestEqual(FString::Printf(TEXT("%s: AdoptPrefix queues (%s)"), *Label, *Error),
					(uint8)S.AdoptPrefix(Seq, Prefixes[AdoptIndex], Handle, Error), (uint8)ESuperSLMRestoreResult::Success) ||
				!DrainUntil(S, [&S, &Handle]() { return S.GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; }, 60.0) ||
				!T.TestEqual(Label + TEXT(": AdoptPrefix delivers Success"),
					(uint8)S.GetLifecycleOpResult(Handle), (uint8)ESuperSLMRestoreResult::Success))
			{
				S.ReturnSequence(Seq);
				return Out;
			}
		}
		for (int32 P = 0; P < 2; ++P)
		{
			T.TestTrue(FString::Printf(TEXT("%s: ReleasePrefix %d (%s)"), *Label, P, *Error), S.ReleasePrefix(Prefixes[P], Error));
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = SuffixTokens;
		Request.MaxNewTokens = kSuffixMaxNewTokens;
		Request.StopTokenIds = {SuperSLML2S3Fixtures::AExStopTokenImEnd, SuperSLML2S3Fixtures::AExStopTokenEndOfText};
		if (!T.TestTrue(FString::Printf(TEXT("%s: BeginGeneration (%s)"), *Label, *Error), S.BeginGeneration(Seq, Request, Error)) ||
			!T.TestTrue(Label + TEXT(": generation completes"),
				DrainUntil(S, [&S, &Seq]() { return S.GetPhase(Seq) == ESuperSLMSequencePhase::Complete || S.GetPhase(Seq) == ESuperSLMSequencePhase::Faulted; }, 90.0)) ||
			!T.TestEqual(Label + TEXT(": generation reaches Complete"), (uint8)S.GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete))
		{
			S.ReturnSequence(Seq);
			return Out;
		}
		Out.Tokens = S.GetGeneratedTokens(Seq);
		Out.bOk = true;
		S.ReturnSequence(Seq);
		return Out;
	}

	// The same steps, adopting prefix 1, every one through USuperSLMBlueprintLibrary. Also asserts
	// the Blueprint surface's own reads: both prefixes read Ready through it, and each released
	// handle reads Invalid through it afterwards.
	FPrefixRun RunPrefixStepsBlueprint(FAutomationTestBase& T, USuperSLMSubsystem* S, const TArray<int32> PrefixTokens[2],
		const TArray<int32>& SuffixTokens)
	{
		const FString Label = TEXT("BP");
		FPrefixRun Out;
		FString Error;
		FSuperSLMPrefixBP Prefixes[2];
		for (int32 P = 0; P < 2; ++P)
		{
			if (!T.TestTrue(FString::Printf(TEXT("%s: CreatePrefix %d (%s)"), *Label, P, *Error),
					USuperSLMBlueprintLibrary::CreatePrefix(S, PrefixTokens[P], Prefixes[P], Error)) ||
				!T.TestTrue(FString::Printf(TEXT("%s: prefix %d handle is valid"), *Label, P), Prefixes[P].IsValid()))
			{
				return Out;
			}
		}
		T.TestNotEqual(Label + TEXT(": the two prefix handles differ"), Prefixes[0].Id, Prefixes[1].Id);
		if (!T.TestTrue(Label + TEXT(": both prefixes read Ready through Blueprint"),
				DrainUntil(*S, [S, &Prefixes]()
				{
					return USuperSLMBlueprintLibrary::IsPrefixReady(S, Prefixes[0]) &&
						USuperSLMBlueprintLibrary::GetPrefixPhase(S, Prefixes[1]) == ESuperSLMPrefixPhaseBP::Ready;
				}, 60.0)))
		{
			return Out;
		}
		FSuperSLMSequenceBP Seq;
		if (!T.TestEqual(Label + TEXT(": vend"), (uint8)USuperSLMBlueprintLibrary::VendSequence(S, Seq), (uint8)ESuperSLMVendResultBP::Success))
		{
			return Out;
		}
		FSuperSLMLifecycleOpHandleBP Handle;
		if (!T.TestEqual(FString::Printf(TEXT("%s: AdoptPrefix queues (%s)"), *Label, *Error),
				(uint8)USuperSLMBlueprintLibrary::AdoptPrefix(S, Seq, Prefixes[1], Handle, Error), (uint8)ESuperSLMRestoreResultBP::Success) ||
			!T.TestTrue(Label + TEXT(": adopt handle is valid"), Handle.IsValid()) ||
			!DrainUntil(*S, [S, &Handle]() { return USuperSLMBlueprintLibrary::GetLifecycleOpResult(S, Handle) != ESuperSLMRestoreResultBP::Pending; }, 60.0) ||
			!T.TestEqual(Label + TEXT(": AdoptPrefix delivers Success"),
				(uint8)USuperSLMBlueprintLibrary::GetLifecycleOpResult(S, Handle), (uint8)ESuperSLMRestoreResultBP::Success))
		{
			USuperSLMBlueprintLibrary::ReturnSequence(S, Seq);
			return Out;
		}
		for (int32 P = 0; P < 2; ++P)
		{
			T.TestTrue(FString::Printf(TEXT("%s: ReleasePrefix %d (%s)"), *Label, P, *Error), USuperSLMBlueprintLibrary::ReleasePrefix(S, Prefixes[P], Error));
			T.TestEqual(FString::Printf(TEXT("%s: released prefix %d reads Invalid"), *Label, P),
				(uint8)USuperSLMBlueprintLibrary::GetPrefixPhase(S, Prefixes[P]), (uint8)ESuperSLMPrefixPhaseBP::Invalid);
		}
		FSuperSLMGenerationRequestBP Request;
		Request.PromptTokens = SuffixTokens;
		Request.MaxNewTokens = kSuffixMaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKindBP::Prompt;
		Request.StopTokenIds = {SuperSLML2S3Fixtures::AExStopTokenImEnd, SuperSLML2S3Fixtures::AExStopTokenEndOfText};
		if (!T.TestTrue(FString::Printf(TEXT("%s: BeginGeneration (%s)"), *Label, *Error), USuperSLMBlueprintLibrary::BeginGeneration(S, Seq, Request, Error)) ||
			!T.TestTrue(Label + TEXT(": generation completes"), DrainUntil(*S, [S, &Seq]()
			{
				const ESuperSLMSequencePhaseBP Phase = USuperSLMBlueprintLibrary::GetPhase(S, Seq);
				return Phase == ESuperSLMSequencePhaseBP::Complete || Phase == ESuperSLMSequencePhaseBP::Faulted;
			}, 90.0)) ||
			!T.TestEqual(Label + TEXT(": generation reaches Complete"),
				(uint8)USuperSLMBlueprintLibrary::GetPhase(S, Seq), (uint8)ESuperSLMSequencePhaseBP::Complete))
		{
			USuperSLMBlueprintLibrary::ReturnSequence(S, Seq);
			return Out;
		}
		Out.Tokens = USuperSLMBlueprintLibrary::GetGeneratedTokens(S, Seq);
		Out.bOk = true;
		USuperSLMBlueprintLibrary::ReturnSequence(S, Seq);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3BlueprintPrefixParityTest,
	"SuperSLM.L2S3.BlueprintParity.SharedPrefixFromBlueprintTokenIdenticalToCpp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3BlueprintPrefixParityTest::RunTest(const FString& Parameters)
{
	FString AExPath, Reason;
	if (!TestTrue(TEXT("A-EX present"), SuperSLML2S3Fixtures::TryGetAExArtifactPath(AExPath, Reason)))
	{
		AddError(Reason);
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX imports"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted))
	{
		AddError(Diag.Message);
		return false;
	}
	FTestWorldWrapper World;
	if (!TestTrue(TEXT("game world created"), World.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	USuperSLMSubsystem* S = SuperSLML2S3Fixtures::GetSubsystem(World.GetTestWorld());
	if (!TestNotNull(TEXT("subsystem"), S))
	{
		return false;
	}
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = SuperSLML2S3Fixtures::AExNumHiddenLayers;
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 2; // two live prefixes at once
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 100000.0;
	if (!TestEqual(TEXT("configure ok"), (uint8)S->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<int32> PrefixTokens[2];
	TArray<int32> SuffixTokens;
	if (!TestTrue(TEXT("tokenize prefix 0"), S->Tokenize(kPrefixText[0], PrefixTokens[0])) ||
		!TestTrue(TEXT("tokenize prefix 1"), S->Tokenize(kPrefixText[1], PrefixTokens[1])) ||
		!TestTrue(TEXT("tokenize suffix"), S->Tokenize(kSuffixText, SuffixTokens)))
	{
		return false;
	}

	// References through the C++ API, checked to discriminate first: adopting prefix 1 differs
	// from adopting prefix 0 and from adopting none. Otherwise a Blueprint adopt of the wrong
	// prefix, or of none, would still match.
	const FPrefixRun Cpp1 = RunPrefixStepsCpp(*this, *S, PrefixTokens, SuffixTokens, 1, TEXT("C++ adopt prefix 1"));
	const FPrefixRun Cpp0 = RunPrefixStepsCpp(*this, *S, PrefixTokens, SuffixTokens, 0, TEXT("C++ adopt prefix 0"));
	const FPrefixRun CppNone = RunPrefixStepsCpp(*this, *S, PrefixTokens, SuffixTokens, -1, TEXT("C++ no adopt"));
	if (!Cpp1.bOk || !Cpp0.bOk || !CppNone.bOk)
	{
		return false;
	}
	if (!TestNotEqual(TEXT("reference discriminates: prefix 1 differs from prefix 0"), Cpp1.Tokens, Cpp0.Tokens) ||
		!TestNotEqual(TEXT("reference discriminates: prefix 1 differs from no prefix"), Cpp1.Tokens, CppNone.Tokens))
	{
		return false;
	}

	const FPrefixRun Bp = RunPrefixStepsBlueprint(*this, S, PrefixTokens, SuffixTokens);
	if (!Bp.bOk)
	{
		return false;
	}
	return TestEqual(TEXT("Blueprint create/adopt/release + suffix equals the same steps through the C++ API"), Bp.Tokens, Cpp1.Tokens);
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
