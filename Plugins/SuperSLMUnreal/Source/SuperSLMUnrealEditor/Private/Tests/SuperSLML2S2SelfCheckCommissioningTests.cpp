// T-2816 -- L2-S2 red suite. Plan cell R-S2b (the plan
// §9, §7 item 11): "The self-check tells the truth" -- dim 6, M6. D-SLM7223's own
// commissioning pair, authored in this file, never in the implementation:
//   Must-accept: on the dev box, the RTX 2080 SUPER's GPU and the CPU backend both
//   report Verified.
//   Must-reject: a corrupted CPU reference entry and test-only GPU token perturbations
//   make their respective self-check arms report Diverged.
// Until both fire correctly, the commissioning rule holds (a check is trusted only once shown
// to accept a good input and reject a bad one): the
// self-check's verdicts are quarantined -- recorded, never displayed, never read by
// downstream surfaces (plan §7 item 11's own quarantine clause).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "SuperSLMDeterminismSelfCheck.h"
#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// Writes a reference-digest JSON file in this suite's own schema (documented in
	// SuperSLML2S2Fixtures.h's TryGetReferenceDigestFilePath() comment): one entry per
	// (backend, granularity) this self-check computes, each carrying ArtifactHash and a
	// hex token digest.
	bool WriteReferenceFile(const FString& Path, const FString& ArtifactHash, const TArray<TPair<FString, FString>>& Entries)
	{
		FString Json = TEXT("{\n  \"artifactHash\": \"") + ArtifactHash + TEXT("\",\n  \"entries\": [\n");
		for (int32 I = 0; I < Entries.Num(); ++I)
		{
			// Every entry in this reference file is a CPU-arm entry: comparison (2) --
		// the shipped-reference comparison this file backs -- applies only to the CPU
		// arm (plan §7 item 11; SuperSLMDeterminismSelfCheck.h's own header note).
		Json += FString::Printf(TEXT("    { \"backend\": \"CPU\", \"granularity\": \"%s\", \"tokenDigestHex\": \"%s\" }%s\n"),
				*Entries[I].Key, *Entries[I].Value, I + 1 < Entries.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]\n}\n");
		return FFileHelper::SaveStringToFile(Json, *Path);
	}

	bool ConfigureBoth(FAutomationTestBase& T, UWorld* World, USuperSLMModel*& OutModel, USuperSLMGpuSubsystem*& OutGpu, USuperSLMSubsystem*& OutCpu)
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
		OutCpu = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), OutGpu) ||
			!T.TestNotNull(TEXT("USuperSLMSubsystem (CPU) must be reachable"), OutCpu))
		{
			return false;
		}

		FSuperSLMGpuRuntimeConfig GpuConfig;
		GpuConfig.ContextCap = 4096;
		GpuConfig.BlockCount = 1;
		GpuConfig.DispatchBudget = DispatchBudgetForLayersPerSlice(1); // the finest composed granularity, matching plan §7 item 11's own GPU sweep
		GpuConfig.K = AExNumHiddenLayers;
		GpuConfig.TickBudgetMs = 1000.0;
		if (!T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(OutModel, GpuConfig).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}

		FSuperSLMRuntimeConfig CpuConfig;
		CpuConfig.MaxSequencesPerDecodeCall = 1;
		CpuConfig.MaxPrefillChunkBudget = 64;
		CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
		CpuConfig.BlockCount = 1;
		CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
		CpuConfig.TickBudgetMs = 1000.0;
		return T.TestEqual(TEXT("CPU Configure()"), (uint8)OutCpu->Configure(OutModel, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// T-2885 finding 4 (the code review record): the must-reject below used
	// to write its "correct" file from PlaceholderHex, the digest of the literal token list
	// {1,2,3,4,5} -- unrelated to anything the self-check actually computes -- for BOTH entries,
	// while Baseline (SuperSLMDeterminismSelfCheck::Run(Model, "")) contributed only ArtifactHash.
	// That file was already divergent from what a real CPU arm run produces, so it reported
	// Diverged with or without the one-character flip below, and the corrupted CHARACTER was
	// never the variable the must-reject actually isolated. FSuperSLMSelfCheckReport exposes no
	// accessor for the CPU arm's own per-granularity digests (ScopeText is prose, not data), so
	// this reproduces SuperSLMDeterminismSelfCheck.cpp's own documented CPU recipe (that file's
	// own header comment, and its D-SLM7383 update: kPinnedPrompt/PinnedSelfCheckPrompt(), NO
	// schema bound, greedy decode, kDecodeSteps (32) new tokens, at CPU layer_budget 1 and
	// num_hidden_layers) directly through USuperSLMSubsystem's own public surface -- the same
	// VendSequence/SetLayerBudget/BeginGeneration/Tick/GetGeneratedTokens sequence RunCpu() in
	// that .cpp runs internally, so its digest is the digest a real, current shipped reference
	// file for this artifact/pin/plugin build would actually carry. Running the self-check
	// against a file built from THIS digest and asserting Verified is what proves the two agree,
	// before the corruption below makes them disagree by exactly one character.
	bool RunCpuBaselineForSelfCheck(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int32 LayerBudget, TArray<int32>& OutTokens)
	{
		FSuperSLMSequence Seq;
		if (Cpu.VendSequence(Seq) != ESuperSLMVendResult::Success)
		{
			T.AddError(TEXT("no CPU sequence could be vended for the must-reject's own baseline reproduction"));
			return false;
		}
		Cpu.SetLayerBudget(Seq, LayerBudget);
		TArray<int32> Prompt;
		if (!T.TestTrue(TEXT("Tokenize() must succeed for the must-reject's own baseline reproduction"), Cpu.Tokenize(PinnedSelfCheckPrompt(), Prompt)))
		{
			Cpu.ReturnSequence(Seq);
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = 32; // SuperSLMDeterminismSelfCheck.cpp's own kDecodeSteps
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration() must succeed for the must-reject's own baseline reproduction (%s)"), *Error), Cpu.BeginGeneration(Seq, Request, Error)))
		{
			Cpu.ReturnSequence(Seq);
			return false;
		}
		bool bDone = false;
		const double StartSeconds = FPlatformTime::Seconds();
		while (!bDone && FPlatformTime::Seconds() - StartSeconds < 60.0)
		{
			Cpu.Tick(1.0f / 60.0f);
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				OutTokens = Cpu.GetGeneratedTokens(Seq);
				bDone = true;
			}
			if (!bDone)
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		Cpu.ReturnSequence(Seq);
		if (!T.TestTrue(TEXT("the must-reject's own baseline reproduction must reach a terminal state"), bDone))
		{
			return false;
		}
		return true;
	}
}

// --- Must-accept: on the dev box, the GPU and the CPU backend both report Verified. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SelfCheckMustAcceptTest,
	"SuperSLM.L2S2.SelfCheck.MustAccept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SelfCheckMustAcceptTest::RunTest(const FString& Parameters)
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
	if (!ConfigureBoth(*this, World, Model, Gpu, Cpu))
	{
		return false;
	}

	// The must-accept construction needs the REAL shipped reference file (2), produced
	// by the build's own harness run of Layer 1's sslm_generate at the pin (T-2826
	// build log §3.1) and shipped at Resources/SelfCheck/ -- unlike the must-reject arm
	// below, this one cannot substitute a self-written synthetic file, because a
	// synthetic file this test writes from its OWN just-computed digest would make the
	// CPU arm "Verified" by construction regardless of whether the plugin's CPU path
	// actually agrees with Layer 1's own sslm_generate, which is exactly the question
	// comparison (2) exists to ask. Fails loudly, naming the gap, if the shipped file is
	// ever absent, rather than silently accepting a synthetic stand-in for this specific
	// arm -- matching R-S1a/R-S1b/R-S1e's own treatment of a missing artifact
	// (Fixtures/SuperSLML2S1Fixtures.h).
	FString RefPath, RefReason;
	if (!TestTrue(*FString::Printf(TEXT(
			"the shipped reference file must exist for the must-accept construction to be "
			"meaningful (%s). It ships as Resources/SelfCheck/<the example model's header "
			"hash>.reference_digests.json."), *RefReason),
		TryGetReferenceDigestFilePath(RefPath, RefReason)))
	{
		return false;
	}

	const FSuperSLMSelfCheckReport Report = SuperSLMDeterminismSelfCheck::Run(*Model, RefPath);
	AddInfo(FString::Printf(TEXT("CPU: %s (%s); GPU: %s (%s)"),
		Report.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Verified ? TEXT("Verified") : TEXT("NOT Verified"), *Report.Cpu.ScopeText,
		Report.Gpu.Verdict == ESuperSLMSelfCheckVerdict::Verified ? TEXT("Verified") : TEXT("NOT Verified"), *Report.Gpu.ScopeText));

	const bool bCpuOk = TestEqual(TEXT("CPU arm must report Verified"), (uint8)Report.Cpu.Verdict, (uint8)ESuperSLMSelfCheckVerdict::Verified);
	const bool bGpuOk = TestEqual(TEXT("GPU arm must report Verified"), (uint8)Report.Gpu.Verdict, (uint8)ESuperSLMSelfCheckVerdict::Verified);

	// Comparison (1) uses the live CPU tokens and does not read the shipped file. Its
	// independent perturbation pair is GpuMustReject below; commissioning remains pending
	// until that cell and this must-accept have been executed by the maintainer.
	const bool bGpuNeverReadsReferenceFile = TestFalse(TEXT("the GPU arm's own bReferenceFileAvailable must stay false -- comparison (1) never depends on a shipped reference file, so this arm cannot be commissioned through this must-reject's own seam"), Report.Gpu.bReferenceFileAvailable);
	AddInfo(TEXT("GPU verdict awaits the separate GpuMustReject commissioning cell."));

	return bCpuOk && bGpuOk && bGpuNeverReadsReferenceFile;
}

// --- Must-reject: a corrupted entry in the shipped reference file makes the self-check
// report Diverged. Producible by the production path (D-SLM7223): a self-check reading
// ANY file at the reference path, real or synthetic, exercises the SAME comparison code
// -- what a genuine on-disk corruption (a bad download, a bit flip) would also produce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SelfCheckMustRejectTest,
	"SuperSLM.L2S2.SelfCheck.MustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SelfCheckMustRejectTest::RunTest(const FString& Parameters)
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
	if (!ConfigureBoth(*this, World, Model, Gpu, Cpu))
	{
		return false;
	}

	// Step 1: run the self-check with NO reference file override, to read Baseline.ArtifactHash
	// (the only field FSuperSLMSelfCheckReport exposes that this construction needs from a live
	// run -- there is no accessor for the CPU arm's own per-granularity digests).
	const FSuperSLMSelfCheckReport Baseline = SuperSLMDeterminismSelfCheck::Run(*Model, /*ReferenceDigestFilePathOverride*/ TEXT(""));

	// T-2885 finding 4 (the code review record): the CORRECT file's own
	// digests must be the REAL digests a current run of this artifact/pin/plugin build actually
	// produces -- reproduced here via RunCpuBaselineForSelfCheck() against USuperSLMSubsystem's
	// public surface (this namespace's own helper, matching SuperSLMDeterminismSelfCheck.cpp's
	// documented recipe exactly: PinnedSelfCheckPrompt() is the same TEXT as that file's own
	// kPinnedPrompt, unconstrained, 32 new tokens, at CPU layer_budget 1 and NumHiddenLayers) --
	// never a placeholder digest of an arbitrary, unrelated token list. This is what isolates the
	// corrupted CHARACTER as the only variable between the two readings below: a "correct" file
	// built from anything else could report Diverged for a reason that has nothing to do with the
	// one-character flip this must-reject exists to catch.
	TArray<int32> CpuTokensLayerBudget1, CpuTokensLayerBudgetFull;
	if (!RunCpuBaselineForSelfCheck(*this, *Cpu, /*LayerBudget*/ 1, CpuTokensLayerBudget1) ||
		!RunCpuBaselineForSelfCheck(*this, *Cpu, /*LayerBudget*/ AExNumHiddenLayers, CpuTokensLayerBudgetFull))
	{
		return false;
	}
	uint8 DigestLayerBudget1[32], DigestLayerBudgetFull[32];
	SuperSLMGpuDigest::ComputeTokenDigest(CpuTokensLayerBudget1, DigestLayerBudget1);
	SuperSLMGpuDigest::ComputeTokenDigest(CpuTokensLayerBudgetFull, DigestLayerBudgetFull);
	const FString DigestHexLayerBudget1 = SuperSLMGpuDigest::DigestToHex(DigestLayerBudget1);
	const FString DigestHexLayerBudgetFull = SuperSLMGpuDigest::DigestToHex(DigestLayerBudgetFull);

	// This suite's own scratch directory (never Content/, never committed) -- a
	// synthetic reference file exists only for the duration of this test.
	const FString ScratchDir = FPaths::ProjectSavedDir() / TEXT("SuperSLML2S2Scratch");
	IFileManager::Get().MakeDirectory(*ScratchDir, /*Tree*/ true);
	const FString CorrectPath = ScratchDir / TEXT("must_reject_correct.json");
	const FString CorruptedPath = ScratchDir / TEXT("must_reject_corrupted.json");

	TArray<TPair<FString, FString>> Entries;
	Entries.Add(TPair<FString, FString>(TEXT("LayerBudget1"), DigestHexLayerBudget1));
	Entries.Add(TPair<FString, FString>(TEXT("LayerBudgetFull"), DigestHexLayerBudgetFull));
	if (!TestTrue(TEXT("WriteReferenceFile(correct) must succeed"), WriteReferenceFile(CorrectPath, Baseline.ArtifactHash, Entries)))
	{
		return false;
	}

	// The isolating half of this must-reject: a self-check run against the CORRECT file must
	// itself report Verified. If this fails, either the baseline reproduction above does not
	// match SuperSLMDeterminismSelfCheck::Run()'s own internal recipe, or that recipe changed --
	// either way, the corrupted-file assertion below would be meaningless until this one passes,
	// because it would no longer be true that corruption is the only difference between the two
	// readings.
	const FSuperSLMSelfCheckReport CorrectReport = SuperSLMDeterminismSelfCheck::Run(*Model, CorrectPath);
	if (!TestEqual(TEXT("CPU arm must report Verified against the CORRECT (uncorrupted) reference file -- this isolates the corruption below as the only variable"),
			(uint8)CorrectReport.Cpu.Verdict, (uint8)ESuperSLMSelfCheckVerdict::Verified))
	{
		return false;
	}

	// A corrupted copy: flip the first hex character of the first entry's digest --
	// still well-formed JSON, still the right shape, wrong content. This is exactly
	// D-SLM7223's own must-reject shape: "a corrupted entry in the shipped reference
	// file," not a malformed file (Malformed is a DIFFERENT, already-covered
	// disposition, not this cell's own subject).
	FString CorrectJson;
	FFileHelper::LoadFileToString(CorrectJson, *CorrectPath);
	FString CorruptedJson = CorrectJson;
	const int32 FirstHexCharIndex = CorruptedJson.Find(DigestHexLayerBudget1);
	if (!TestTrue(TEXT("the correct digest must be found in the written file, to corrupt it"), FirstHexCharIndex != INDEX_NONE))
	{
		return false;
	}
	const TCHAR OriginalChar = CorruptedJson[FirstHexCharIndex];
	const TCHAR FlippedChar = (OriginalChar == '0') ? TEXT('1') : TEXT('0');
	CorruptedJson[FirstHexCharIndex] = FlippedChar;
	if (!TestTrue(TEXT("saving the corrupted reference file must succeed"), FFileHelper::SaveStringToFile(CorruptedJson, *CorruptedPath)))
	{
		return false;
	}

	const FSuperSLMSelfCheckReport Corrupted = SuperSLMDeterminismSelfCheck::Run(*Model, CorruptedPath);
	AddInfo(FString::Printf(TEXT("Corrupted-reference CPU arm: %s (first diverging step %d, granularity %d)"),
		Corrupted.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Diverged ? TEXT("Diverged") : TEXT("NOT Diverged"),
		Corrupted.Cpu.FirstDivergingStep, Corrupted.Cpu.FirstDivergingGranularityIndex));

	return TestEqual(TEXT("CPU arm must report Diverged against a reference file that differs from the correct one by exactly one character"),
		(uint8)Corrupted.Cpu.Verdict, (uint8)ESuperSLMSelfCheckVerdict::Diverged);
}

// The GPU verdict compares the just-produced GPU tokens with a live CPU reference.
// The test-only seam changes one GPU token after the real generation and before that
// comparison; the unperturbed run establishes the must-accept in the same fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SelfCheckGpuMustRejectTest,
	"SuperSLM.L2S2.SelfCheck.GpuMustReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SelfCheckGpuMustRejectTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	USuperSLMSubsystem* Cpu = nullptr;
	if (!ConfigureBoth(*this, TestWorldWrapper.GetTestWorld(), Model, Gpu, Cpu))
	{
		return false;
	}
	FString RefPath, RefReason;
	if (!TestTrue(TEXT("shipped self-check reference exists"), TryGetReferenceDigestFilePath(RefPath, RefReason)))
	{
		return false;
	}
	const FSuperSLMSelfCheckReport Control = SuperSLMDeterminismSelfCheck::Run(*Model, RefPath);
	if (!TestEqual(TEXT("unperturbed GPU must be Verified"), (uint8)Control.Gpu.Verdict,
			(uint8)ESuperSLMSelfCheckVerdict::Verified))
	{
		return false;
	}
	bool bOk = true;
	for (int32 Granularity = 0; Granularity < 2; ++Granularity)
	{
		for (const int32 Step : { 0, 31 })
		{
			const FSuperSLMSelfCheckReport Perturbed =
				SuperSLMDeterminismSelfCheck::RunWithGpuTokenPerturbation(*Model, RefPath, Granularity, Step);
			const FString Arm = FString::Printf(TEXT("GPU granularity %d step %d"), Granularity, Step);
			bOk &= TestEqual(*(Arm + TEXT(" rejects")), (uint8)Perturbed.Gpu.Verdict,
				(uint8)ESuperSLMSelfCheckVerdict::Diverged);
			bOk &= TestEqual(*(Arm + TEXT(" first diverging granularity")),
				Perturbed.Gpu.FirstDivergingGranularityIndex, Granularity);
			bOk &= TestEqual(*(Arm + TEXT(" first diverging step")),
				Perturbed.Gpu.FirstDivergingStep, Step);
			bOk &= TestEqual(*(Arm + TEXT(" leaves CPU verdict Verified")),
				(uint8)Perturbed.Cpu.Verdict, (uint8)ESuperSLMSelfCheckVerdict::Verified);
		}
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
