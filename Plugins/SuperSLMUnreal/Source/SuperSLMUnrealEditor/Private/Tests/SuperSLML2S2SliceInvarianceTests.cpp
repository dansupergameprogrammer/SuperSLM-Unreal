// T-2816 -- L2-S2 red suite. Plan cell R-S2e (the plan
// §9): "Slicing a GPU token never changes the answer -- the demonstrator's invariant and
// the duplicate-KV gate" -- dims 6, 8; D-SLM7244. This is the cell that gates the
// duplicate-KV-commit class D-SLM7256 names, and the gate for D-SLM7256's own four rules
// (plan §12 decision 1: "gated by R-S2e's digest equality at every slice setting").
//
// The composed path's token digest equals the one-call path's at every slice setting
// from 1 layer to num_hidden_layers layers (24, for A-EX's Qwen2.5-0.5B, plan §5's own
// arithmetic), including a slice boundary immediately after prompt prefill, a save and
// restore between prefill and the first decode, and the concurrent path with 4
// sequences through the batch call with rotated order.
//
// UPDATED per fold 7 (D-SLM7379, T-2785 §13; maintainer's own routed instruction,
// 2026-09-19): "R-S2e adds prompt slicing and an independent sslm_generate anchor."
// **Prompt slicing** is now inherent to `EverySliceSettingMatchesOneCall` below without
// a separate arm: the plugin no longer calls a separate, atomic batched-prefill verb --
// prompt tokens are fed through the SAME embed-plus-layer-slice loop as decode tokens
// (D-SLM7379), so sweeping DispatchBudget from 1 to num_hidden_layers already slices
// the PROMPT the same way it slices decode, for every setting that sweep already
// covers. **The independent anchor** is the new
// `AgainstSslmGenerateAnchor` test below: it compares the GPU one-call path's own
// tokens against Layer 1's OWN `sslm_generate` output (the shipped self-check
// reference file, T-2826 build log §3.1), never a value this suite or the plugin
// computes -- every other test in this file compares the plugin's composed path
// against the plugin's OWN one-call path, which shares its implementation and could
// share a defect; this one does not.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMJson.h" // the plugin's one JSON-object read (review R4-W1)
#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	const TArray<int32>& kSlicePromptTokens()
	{
		static const TArray<int32> Tokens = {1, 2, 3, 4, 5, 6, 7, 8};
		return Tokens;
	}
	constexpr int32 kSliceMaxNewTokens = 48; // matches plan §10.3's own "each 48 tokens" costing text

	bool SetUpGpu(FAutomationTestBase& T, UWorld* World, int32 BlockCount, uint32 DispatchBudget, int32 K, USuperSLMModel*& OutModel, USuperSLMGpuSubsystem*& OutGpu, bool bHeadOn = true)
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
		Config.DispatchBudget = DispatchBudget;
		Config.K = K;
		Config.TickBudgetMs = 1000.0;
		OutModel->bGpuDeviceResidentHead = bHeadOn;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(OutModel, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	bool RunOneSession(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, ESuperSLMGpuDecodePath Path, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!T.TestEqual(TEXT("VendSequence()"), (uint8)Gpu.VendSequence(Seq, Path), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = kSlicePromptTokens();
		Request.MaxNewTokens = kSliceMaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		FString RunError;
		const bool bOk = RunGpuGenerationToCompletion(Gpu, Seq, Request, OutTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
		Gpu.ReturnSequence(Seq);
		return T.TestTrue(*FString::Printf(TEXT("generation must complete (%s)"), *RunError), bOk);
	}
}

// --- Every slice setting from 1 layer to num_hidden_layers layers matches the
// one-call reference digest. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceInvarianceEverySettingTest,
	"SuperSLM.L2S2.SliceInvariance.EverySliceSettingMatchesOneCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceInvarianceEverySettingTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, DispatchBudgetForLayersPerSlice(1), /*K*/ AExNumHiddenLayers, Model, Gpu))
	{
		return false;
	}

	TArray<int32> OneCallTokens;
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::OneCall, OneCallTokens))
	{
		return false;
	}
	uint8 OneCallDigest[32];
	SuperSLMGpuDigest::ComputeTokenDigest(OneCallTokens, OneCallDigest);
	const FString OneCallHex = SuperSLMGpuDigest::DigestToHex(OneCallDigest);

	int32 MismatchCount = 0;
	for (int32 LayersPerSlice = 1; LayersPerSlice <= AExNumHiddenLayers; ++LayersPerSlice)
	{
		// Re-Configure() per setting (idempotent, matches CPU's own established
		// convention) to change DispatchBudget between arms.
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = 4096;
		Config.BlockCount = 1;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(LayersPerSlice);
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		if (!TestEqual(*FString::Printf(TEXT("layers-per-slice=%d: GPU Configure()"), LayersPerSlice),
				(uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			++MismatchCount;
			continue;
		}

		TArray<int32> ComposedTokens;
		if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::Composed, ComposedTokens))
		{
			++MismatchCount;
			continue;
		}
		uint8 ComposedDigest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(ComposedTokens, ComposedDigest);
		const FString ComposedHex = SuperSLMGpuDigest::DigestToHex(ComposedDigest);

		if (!TestEqual(*FString::Printf(TEXT("layers-per-slice=%d: composed digest must equal the one-call digest"), LayersPerSlice),
				ComposedHex, OneCallHex))
		{
			++MismatchCount;
		}
	}

	return MismatchCount == 0;
}

// --- A slice boundary immediately after prompt prefill: the first post-prefill step
// on the composed path is finish-only (D-SLM7256 rule 1), verified by digest equality
// at the finest slicing (1 layer/slice), where the guard is under the most pressure to
// misfire (every subsequent step is also a fresh slice boundary). ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceInvariancePrefillBoundaryTest,
	"SuperSLM.L2S2.SliceInvariance.PrefillBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceInvariancePrefillBoundaryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, DispatchBudgetForLayersPerSlice(1), /*K*/ AExNumHiddenLayers, Model, Gpu))
	{
		return false;
	}

	TArray<int32> OneCallTokens;
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::OneCall, OneCallTokens))
	{
		return false;
	}
	TArray<int32> ComposedTokens;
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::Composed, ComposedTokens))
	{
		return false;
	}

	// The FIRST generated token specifically isolates the prefill-boundary guard: it is
	// the one produced by the finish-only step D-SLM7256 rule 1 names, before any
	// layer-loop slicing has happened at all this token.
	if (!TestTrue(TEXT("both runs must have produced at least one token"), OneCallTokens.Num() > 0 && ComposedTokens.Num() > 0))
	{
		return false;
	}
	return TestEqual(TEXT("the first post-prefill token must match between the two paths"), ComposedTokens[0], OneCallTokens[0]);
}

// --- A save and restore between prefill and the first decode: proves the plugin's own
// immunity (D-SLM7256 rules 1-2 -- "primed" lives in the plugin's OWN save wrapper, not
// Layer 1's ready_for_logits) with a real executed test, not just the reasoning the
// fold record (T-2785 §8.3) states. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceInvarianceSaveRestoreBeforeFirstDecodeTest,
	"SuperSLM.L2S2.SliceInvariance.SaveRestoreBetweenPrefillAndFirstDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceInvarianceSaveRestoreBeforeFirstDecodeTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*BlockCount*/ 1, DispatchBudgetForLayersPerSlice(1), /*K*/ AExNumHiddenLayers, Model, Gpu))
	{
		return false;
	}

	TArray<int32> OneCallTokens;
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::OneCall, OneCallTokens))
	{
		return false;
	}

	// Composed-path run, but save immediately after prefill completes (before the
	// first decode step) and restore into a fresh handle before continuing.
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = kSlicePromptTokens();
	Request.MaxNewTokens = kSliceMaxNewTokens;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
	{
		return false;
	}
	// T-2885 finding 5 (the code review record): a prior round justified
	// "one tick" here by quoting plan §5.1 as "GPU prompt prefill is atomic" -- that sentence
	// appears nowhere in the plan (grepped whole), §5.1 is titled
	// "Instrumentation and Unreal Insights" and has nothing to do with prefill, and the plan
	// states the OPPOSITE: D-SLM7379 (plan §5, "GPU prompt prefill is sliced, and no
	// prompt-length ceiling exists") retires the atomic batched-prefill verb outright -- "a
	// prompt of any length now costs ceil(P x num_hidden_layers / LayersPerTick) ticks of
	// sliced work". One tick therefore reaches only layer 1 of prompt token 0, which is what
	// this file's own header comment already documents (fold 7, above) and what T-2885 found:
	// a save this early re-triggers BeginPrompt() on the very next action (bFirstPrompt), which
	// re-establishes the state a restore that drops it would otherwise expose -- so this
	// cell's OWN name ("...BetweenPrefillAndFirstDecode") was not being tested. Reaching the
	// state the name promises needs the WHOLE prompt slotted to full depth first: at this
	// test's own DispatchBudgetForLayersPerSlice(1) (1 layer/tick), each of the
	// kSlicePromptTokens().Num() (8) prompt tokens needs AExNumHiddenLayers (24) ticks to reach
	// full depth before the next one embeds (the same one-layer-per-tick slicing the composed
	// decode path already uses, D-SLM7379) -- 8 * 24 = 192 ticks totals exactly the last prompt
	// token's own last layer, landing on the primed boundary (D-SLM7334's own "walk still at
	// its post-bind state" shape, here with no schema bound): the prompt fully consumed, no
	// decode/finish step run yet.
	if (!TestEqual(TEXT("kSlicePromptTokens() must still be 8 tokens -- the constant below is derived from that count"), kSlicePromptTokens().Num(), 8))
	{
		return false;
	}
	constexpr int32 kTicksToFinishPrefill = 8 * AExNumHiddenLayers; // kSlicePromptTokens().Num() * AExNumHiddenLayers
	for (int32 T = 0; T < kTicksToFinishPrefill; ++T)
	{
		Gpu->Tick(1.0f / 60.0f);
	}
	if (!TestEqual(TEXT("the whole prompt must be consumed with no decode token produced yet -- otherwise this save is not between prefill and the first decode"),
			Gpu->GetGeneratedTokens(Seq).Num(), 0))
	{
		return false;
	}

	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const bool bSaveOk = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
	if (!TestTrue(*FString::Printf(TEXT("RequestSaveSequence() right after prefill must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), bSaveOk))
	{
		return false;
	}
	Gpu->ReturnSequence(Seq);

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestEqual(TEXT("RequestRestoreSequence() must return Success"), (uint8)DriveRestoreToResolution(*Gpu, RestoreHandle, Restored), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	// Continue to completion on the RESTORED handle -- the first decode step it takes
	// is exactly the one that must be finish-only, per D-SLM7256 rule 1, using the
	// plugin's OWN "primed" bit carried through the save wrapper (rule 2), never
	// Layer 1's own ready_for_logits (which the routed finding, T-2811/D-SLM7256,
	// confirms this exact save/restore round trip does NOT preserve at the ABI level).
	TArray<int32> RestoredTokens;
	FString RunError;
	const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Restored, Request, RestoredTokens, /*MaxWallClockSeconds*/ 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Restored);
	if (!TestTrue(*FString::Printf(TEXT("the restored sequence must complete (%s)"), *RunError), bCompleted))
	{
		return false;
	}

	uint8 OneCallDigest[32], RestoredDigest[32];
	SuperSLMGpuDigest::ComputeTokenDigest(OneCallTokens, OneCallDigest);
	SuperSLMGpuDigest::ComputeTokenDigest(RestoredTokens, RestoredDigest);
	return TestEqual(TEXT("the save/restore-before-first-decode digest must equal the one-call digest"),
		SuperSLMGpuDigest::DigestToHex(RestoredDigest), SuperSLMGpuDigest::DigestToHex(OneCallDigest));
}

// --- The concurrent path: 4 composed sequences through the batch call with rotated
// order -- each sequence's own digest must still equal ITS OWN one-call reference,
// proving concurrent batching does not perturb any individual sequence. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceInvarianceConcurrentTest,
	"SuperSLM.L2S2.SliceInvariance.ConcurrentFourSequencesRotatedOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceInvarianceConcurrentTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	constexpr int32 kConcurrentCount = 4;
	// D-SLM7381's own composed-path formula: ceil(BlockCount x L / LayersPerTick) =
	// ceil(4 * 24 / 1) = 96, at this test's own 1-layer/tick DispatchBudget.
	const int32 ConcurrentMinimumK = FMath::CeilToInt32((float)(kConcurrentCount * AExNumHiddenLayers) / 1.0f);
	if (!SetUpGpu(*this, World, kConcurrentCount, DispatchBudgetForLayersPerSlice(1), ConcurrentMinimumK, Model, Gpu))
	{
		return false;
	}

	// 4 distinct prompts, one per sequence -- each has its own independently-computed
	// one-call reference digest before any concurrency is introduced.
	TArray<TArray<int32>> Prompts;
	for (int32 I = 0; I < kConcurrentCount; ++I)
	{
		TArray<int32> P = kSlicePromptTokens();
		P.Add(100 + I); // perturb each prompt distinctly so the 4 sessions are not identical
		Prompts.Add(P);
	}

	TArray<FString> OneCallHexes;
	for (int32 I = 0; I < kConcurrentCount; ++I)
	{
		FSuperSLMGpuSequence Seq;
		if (!TestEqual(*FString::Printf(TEXT("prompt %d: VendSequence(OneCall)"), I), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompts[I];
		Request.MaxNewTokens = kSliceMaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		TArray<int32> Tokens;
		FString RunError;
		const bool bOk = RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, /*MaxWallClockSeconds*/ 60.0, RunError);
		Gpu->ReturnSequence(Seq);
		if (!TestTrue(*FString::Printf(TEXT("prompt %d: one-call reference must complete (%s)"), I, *RunError), bOk))
		{
			return false;
		}
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		OneCallHexes.Add(SuperSLMGpuDigest::DigestToHex(Digest));
	}

	// Now all 4 concurrently, composed path, driven through one shared Tick() loop --
	// exactly what USuperSLMGpuSubsystem::Tick() is documented to route through the
	// batch call with rotated array order when more than one Composed sequence is due
	// (SuperSLMGpuSubsystem.h).
	TArray<FSuperSLMGpuSequence> ConcurrentSeqs;
	for (int32 I = 0; I < kConcurrentCount; ++I)
	{
		FSuperSLMGpuSequence Seq;
		if (!TestEqual(*FString::Printf(TEXT("prompt %d: VendSequence(Composed)"), I), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompts[I];
		Request.MaxNewTokens = kSliceMaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
		TestTrue(*FString::Printf(TEXT("prompt %d: concurrent RequestBeginGeneration() must succeed (%s)"), I, *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid());
		ConcurrentSeqs.Add(Seq);
	}

	int32 CompletedCount = 0;
	const double ConcurrentStartSeconds = FPlatformTime::Seconds();
	while (CompletedCount < kConcurrentCount && FPlatformTime::Seconds() - ConcurrentStartSeconds < 60.0)
	{
		Gpu->Tick(1.0f / 60.0f);
		CompletedCount = 0;
		for (const FSuperSLMGpuSequence& Seq : ConcurrentSeqs)
		{
			if (Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Complete)
			{
				++CompletedCount;
			}
		}
		if (CompletedCount < kConcurrentCount)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
	TestEqual(TEXT("all 4 concurrent sequences must reach Complete"), CompletedCount, kConcurrentCount);

	int32 MismatchCount = 0;
	for (int32 I = 0; I < kConcurrentCount; ++I)
	{
		const TArray<int32> Tokens = Gpu->GetGeneratedTokens(ConcurrentSeqs[I]);
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		if (!TestEqual(*FString::Printf(TEXT("prompt %d: concurrent digest must equal its own one-call reference"), I),
				SuperSLMGpuDigest::DigestToHex(Digest), OneCallHexes[I]))
		{
			++MismatchCount;
		}
		Gpu->ReturnSequence(ConcurrentSeqs[I]);
	}

	return CompletedCount == kConcurrentCount && MismatchCount == 0;
}

// --- An INDEPENDENT anchor: the GPU one-call path's own tokens, on the pinned
// self-check prompt (unconstrained, 32 steps, matching T-2826 build log §3.1's own
// sslm_generate invocation exactly), against Layer 1's OWN sslm_generate output --
// never a value this suite or the plugin computes. Every other test in this file
// compares the plugin's composed path against the plugin's own one-call path, both
// computed BY the plugin; this test's own reference is computed entirely OUTSIDE it
// (D-SLM7379, T-2785 fold 7 §13: "R-S2e adds ... an independent sslm_generate anchor"). ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SliceInvarianceAgainstSslmGenerateAnchorTest,
	"SuperSLM.L2S2.SliceInvariance.AgainstSslmGenerateAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SliceInvarianceAgainstSslmGenerateAnchorTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	// --- Load the shipped reference file's own CPU/LayerBudget1 entry -- Layer 1's CPU
	// bits do not depend on the layer budget (SuperSLML2S2Fixtures.h's own header note
	// on this file's schema), so either CPU entry carries the SAME anchor tokens
	// sslm_generate produced at the pin (T-2826 build log §3.1). ---
	FString RefPath, RefReason;
	if (!TestTrue(*FString::Printf(TEXT("the shipped reference file must exist (%s)"), *RefReason), TryGetReferenceDigestFilePath(RefPath, RefReason)))
	{
		return false;
	}
	FString RefJson;
	if (!TestTrue(*FString::Printf(TEXT("reading '%s' must succeed"), *RefPath), FFileHelper::LoadFileToString(RefJson, *RefPath)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> RefRoot;
	FString RefReadError;
	const bool bRefRead = SuperSLMJson::TryReadObject(RefJson, RefRoot, RefReadError);
	if (!TestTrue(*FString::Printf(TEXT("the reference file must be a JSON object (%s)"), *RefReadError), bRefRead))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!TestTrue(TEXT("the reference file must carry an 'entries' array"), RefRoot->TryGetArrayField(TEXT("entries"), EntriesArray) && EntriesArray != nullptr && EntriesArray->Num() > 0))
	{
		return false;
	}
	TArray<int32> AnchorTokens;
	bool bFoundCpuEntry = false;
	for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
	{
		const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
		if (!Entry.IsValid())
		{
			continue;
		}
		FString Backend;
		Entry->TryGetStringField(TEXT("backend"), Backend);
		if (Backend != TEXT("CPU"))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* TokensArray = nullptr;
		if (Entry->TryGetArrayField(TEXT("tokens"), TokensArray) && TokensArray != nullptr && TokensArray->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& TokenValue : *TokensArray)
			{
				AnchorTokens.Add((int32)TokenValue->AsNumber());
			}
			bFoundCpuEntry = true;
			break;
		}
	}
	if (!TestTrue(TEXT("a CPU entry with a 'tokens' array must be found in the reference file"), bFoundCpuEntry))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("anchor: %d tokens from '%s'"), AnchorTokens.Num(), *RefPath));

	// --- Configure and run the GPU one-call path on the SAME workload the anchor was
	// produced from: the pinned prompt, unconstrained, 32 steps (T-2826 build log §3.1:
	// "sslm_generate A-EX A-EX ... --max-new 32"). ---
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
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers); // one-call path; DispatchBudget is unused by it, set to a whole token's worth
	Config.K = AExNumHiddenLayers; // one-call path MinimumK = BlockCount (1); generous
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}

	// Tokenization is CPU-subsystem-owned (SuperSLML2S2FrameContractTests.cpp's own
	// SetUpGpuAndCpu() establishes this pattern): the CPU subsystem must itself be
	// Configure()d before Tokenize() can read a live State -- fetching the subsystem
	// alone is not enough.
	USuperSLMSubsystem* CpuForTokenize = GetSubsystem(World);
	if (!TestNotNull(TEXT("a CPU subsystem must be reachable to tokenize the pinned prompt"), CpuForTokenize))
	{
		return false;
	}
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = Config.BlockCount;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.BlockCount = Config.BlockCount;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU Configure()"), (uint8)CpuForTokenize->Configure(Model, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("Tokenize() must succeed"), CpuForTokenize->Tokenize(PinnedSelfCheckPrompt(), Request.PromptTokens)))
	{
		return false;
	}
	Request.MaxNewTokens = 32; // matches the anchor's own --max-new 32
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	TArray<int32> GpuTokens;
	FString RunError;
	const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Seq, Request, GpuTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
	Gpu->ReturnSequence(Seq);
	if (!TestTrue(*FString::Printf(TEXT("GPU one-call generation must complete (%s)"), *RunError), bCompleted))
	{
		return false;
	}

	// FEAT oracle: token-identical to Layer 1's OWN sslm_generate output -- a plugin
	// perturbation of even one bit anywhere in the GPU decode path changes at least one
	// token here, and this reference shares no code with the plugin at all.
	return TestEqual(TEXT("GPU one-call tokens must equal the independent sslm_generate anchor"), GpuTokens, AnchorTokens);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1GpuHeadOffSwitchMatchesOnTest,
	"SuperSLM.U1.Gpu.HeadOffSwitchMatchesOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuHeadOffSwitchMatchesOnTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World.GetTestWorld(), 1, DispatchBudgetForLayersPerSlice(1),
			AExNumHiddenLayers, Model, Gpu, /*bHeadOn*/ false)) { return false; }
	TArray<int32> OffOneCall, OffComposed, OnOneCall, OnComposed;
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::OneCall, OffOneCall) ||
		!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::Composed, OffComposed)) { return false; }
	FSuperSLMGpuRuntimeConfig Config;
	Config.ContextCap = 4096;
	Config.BlockCount = 1;
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(1);
	Config.K = AExNumHiddenLayers;
	Config.TickBudgetMs = 1000.0;
	Model->bGpuDeviceResidentHead = true;
	if (!TestEqual(TEXT("head-on Configure"), (uint8)Gpu->Configure(Model, Config).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	if (!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::OneCall, OnOneCall) ||
		!RunOneSession(*this, *Gpu, ESuperSLMGpuDecodePath::Composed, OnComposed)) { return false; }
	bool bOk = TestEqual(TEXT("head-off composed matches head-off one-call"), OffComposed, OffOneCall);
	bOk &= TestEqual(TEXT("head-on one-call matches head-off one-call"), OnOneCall, OffOneCall);
	bOk &= TestEqual(TEXT("head-on composed matches head-off one-call"), OnComposed, OffOneCall);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
