// T-2816 -- L2-S2 red suite. Plan cell R-S2c (the plan
// §9): "A recoverable GPU rejection does not kill the backend" -- dim 5, M4.
//
// UPDATED per fold 7 (D-SLM7379, T-2785 §13; maintainer's own routed instruction,
// 2026-09-19): the GPU prompt-length ceiling is RETIRED. The plugin no longer calls
// Layer 1's batched prefill verb; instead it feeds each prompt token as an embed plus
// decode-step layers, exactly like a decode token, with no upfront refusal by prompt
// length ("no ceiling refusal") -- BeginGeneration() SUCCEEDS for a prompt of any
// length, and ingestion proceeds one token (and its own layer slices) per tick. Every
// embed is checked against the sequence's own ContextCap before it is issued
// (D-SLM7379: "every embed is checked against the context cap first"), so a prompt
// LONGER than ContextCap is admitted token by token until the cap is reached, at which
// point the NEXT embed is refused as a healthy, recoverable per-sequence rejection --
// the GPU-side twin of the CPU backend's own partial-admission contract (plan §5).
// This file's own former "OverCeilingPromptCostsNoGpuCalls" test (a PRE-call refusal
// keyed to a plugin-computed PromptCeilingTokens figure) is REMOVED: that entry point
// no longer exists.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	bool SetUpGpu(FAutomationTestBase& T, UWorld* World, int64 ContextCap, USuperSLMModel*& OutModel, USuperSLMGpuSubsystem*& OutGpu)
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
		Config.ContextCap = ContextCap;
		Config.BlockCount = 2;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
		// D-SLM7381's own formula, one-call path: MinimumK = BlockCount. This file's
		// tests vend OneCall sequences only, so BlockCount (2) is the floor; K is set
		// generously above it, matching every other cell's own convention of using a
		// generous K where K itself is not the cell's own subject (that is R-S2a's).
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		return T.TestEqual(TEXT("GPU Configure()"), (uint8)OutGpu->Configure(OutModel, Config).Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}
}

// --- A prompt longer than ContextCap is admitted token by token, with NO upfront
// refusal (D-SLM7379: "no ceiling refusal") -- BeginGeneration() succeeds, ingestion
// proceeds per token, and the embed that would push context_length past ContextCap is
// refused as a healthy, RECOVERABLE per-sequence rejection: the backend, the context,
// and every OTHER sequence stay usable, and a subsequent well-formed session still
// completes on the GPU. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2LongPromptRecoversAtContextCapTest,
	"SuperSLM.L2S2.RecoverableRejection.LongPromptRecoversAtContextCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2LongPromptRecoversAtContextCapTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	// A tiny ContextCap makes a modest prompt genuinely exceed the sequence's own
	// remaining capacity, without needing an enormous prompt or a long wait.
	if (!SetUpGpu(*this, World, /*ContextCap*/ 16, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	// 20 tokens against ContextCap=16 genuinely EXCEEDS the sequence's own remaining
	// capacity (context_length starts at 0 on a fresh sequence): per-token ingestion
	// admits tokens 1-16 and the 17th embed is refused, the plugin's own per-embed cap
	// check (D-SLM7379), never a Layer-1 batched-prefill saturation (that entry point
	// is no longer called at all).
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens.Init(1, 20);
	Request.MaxNewTokens = 8;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// D-SLM7412 (T-2826 build log §10.1 item 2, 2026-09-19): BeginGeneration() had
	// briefly regressed to an upfront `PromptTokens.Num() > ContextCap` refusal in round
	// 2, which this very assertion caught this round (the fault this cell now exists to
	// pin). "No ceiling refusal" (D-SLM7379): BeginGeneration() itself must SUCCEED for
	// this prompt -- there is no more upfront prompt-length check to refuse it with.
	// Ingestion begins and is expected to fault MID-STREAM, once the cap is genuinely
	// reached.
	const int64 DispatchCountAtBegin = Gpu->GetGpuDispatchCount();
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed -- no upfront ceiling refusal (%s)"), *Gpu->GetLastLifecycleRequestError()),
			BeginHandle.IsValid()))
	{
		return false;
	}
	// D-SLM7412's own regression guard, distinct from SchemaContentSpanRefused's
	// own whole-request dispatch-count check (this file's other test): THIS assertion isolates RequestBeginGeneration()
	// itself, before the first Tick() runs any per-token ingestion slice (and before admission, which is
	// also Layer-1-free), so a future change that reintroduces an upfront length check by issuing a GPU
	// probe call ahead of admitting the first token (while still returning a valid handle) cannot
	// masquerade as compliant -- the two "zero GPU calls" claims (the whole refused request vs. this
	// one, RequestBeginGeneration()'s own call) stay distinguishable.
	if (!TestEqual(TEXT("RequestBeginGeneration() itself must issue no GPU dispatch (D-SLM7412) -- ingestion happens only on Tick()"),
			Gpu->GetGpuDispatchCount(), DispatchCountAtBegin))
	{
		return false;
	}

	// Per-token ingestion: one prompt token (plus its own layer slices) per tick, so
	// the cap is reached around tick 16-17, not within a single atomic call. Poll for
	// the fault over a wall-clock deadline so asynchronous device work can finish.
	ESuperSLMGpuFaultReason Fault = ESuperSLMGpuFaultReason::None;
	const double FaultStartSeconds = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - FaultStartSeconds < 30.0)
	{
		Gpu->Tick(1.0f / 60.0f);
		Fault = Gpu->GetLastFaultReason(Seq);
		if (Fault != ESuperSLMGpuFaultReason::None)
		{
			break;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	AddInfo(FString::Printf(TEXT("GetLastFaultReason()=%d"), (int32)Fault));
	if (!TestEqual(TEXT("the context-cap rejection must be recoverable, never terminal"), (uint8)Fault, (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection))
	{
		return false;
	}

	if (!TestTrue(TEXT("ProbeContextUsable() must confirm the context is still usable"), Gpu->ProbeContextUsable()))
	{
		return false;
	}
	if (!TestTrue(TEXT("IsGpuBackendActive() must stay true after a recoverable rejection"), Gpu->IsGpuBackendActive()))
	{
		return false;
	}

	// The NEXT tick still runs on the GPU -- a fresh, well-formed sequence completes.
	Gpu->ReturnSequence(Seq);
	FSuperSLMGpuSequence NextSeq;
	if (!TestEqual(TEXT("VendSequence() for the next session"), (uint8)Gpu->VendSequence(NextSeq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FSuperSLMGenerationRequest NextRequest;
	NextRequest.PromptTokens = {1, 2, 3};
	NextRequest.MaxNewTokens = 4;
	NextRequest.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> NextTokens;
	FString RunError;
	const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, NextSeq, NextRequest, NextTokens, /*MaxWallClockSeconds*/ 60.0, RunError);
	Gpu->ReturnSequence(NextSeq);
	return TestTrue(*FString::Printf(TEXT("a subsequent, well-formed session must still complete on the GPU (%s)"), *RunError), bCompleted);
}

// --- D-SLM7392 (fold 8, T-2785 §13/§14; maintainer's own routed instruction, 2026-09-19):
// a forced schema-content span (ESuperSLMSpanKind::SchemaContent) is refused on the GPU
// backend, by name, at 1.0. Layer 1's only GPU verb for forced content
// (SslmGpuSeqPrefillSchemaContentForG5Bridge) is an atomic batch that sets
// ready_for_logits -- the exact property D-SLM7379 retires for prompts -- and no
// per-token form exists at v1.5.0 (the DFA walk state has no public setter outside the
// finish step). No R cell and no demonstrator control uses the span on the GPU (the CPU
// backend keeps it, D-SLM7392's own text); this is the ONE assertion of the refusal
// this suite carries, since no other cell exercises the span at all. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SchemaContentSpanRefusedTest,
	"SuperSLM.L2S2.RecoverableRejection.SchemaContentSpanRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SchemaContentSpanRefusedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	USuperSLMModel* Model = nullptr;
	USuperSLMGpuSubsystem* Gpu = nullptr;
	if (!SetUpGpu(*this, World, /*ContextCap*/ 4096, Model, Gpu))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 4;
	// The claim under test: SchemaContent is refused on the GPU, by name, at
	// BeginGeneration() -- never silently treated as Prompt, and never reaching any GPU
	// call (D-SLM7392: no per-token form exists at this pin, so there is nothing this
	// backend could correctly DO with a forced span; refusing is the only honest
	// response).
	Request.SpanKind = ESuperSLMSpanKind::SchemaContent;

	const int64 DispatchCountBefore = Gpu->GetGpuDispatchCount();
	const bool bRefused = !Gpu->RequestBeginGeneration(Seq, Request).IsValid();
	const FString BeginError = Gpu->GetLastLifecycleRequestError();
	Gpu->ReturnSequence(Seq);

	if (!TestTrue(TEXT("RequestBeginGeneration() must refuse a SchemaContent span on the GPU backend"), bRefused))
	{
		return false;
	}
	if (!TestTrue(*FString::Printf(TEXT("the refusal must name the span kind (got: '%s')"), *BeginError),
			BeginError.Contains(TEXT("SchemaContent"), ESearchCase::IgnoreCase) || BeginError.Contains(TEXT("span"), ESearchCase::IgnoreCase)))
	{
		return false;
	}
	return TestEqual(TEXT("GetGpuDispatchCount() must be unchanged -- the refusal reaches no GPU call"), Gpu->GetGpuDispatchCount(), DispatchCountBefore);
}

#endif // WITH_DEV_AUTOMATION_TESTS
