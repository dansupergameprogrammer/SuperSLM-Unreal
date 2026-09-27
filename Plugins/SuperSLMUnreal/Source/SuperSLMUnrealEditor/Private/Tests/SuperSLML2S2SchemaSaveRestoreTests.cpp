// T-2816 -- L2-S2 red suite. Cell R-S2g, added to this suite mid-authoring
// (maintainer's routed finding, T-2811 §3, folding D-SLM7256's own scope): "A GPU
// save/restore never silently drops a schema constraint" -- dims 2, 9. RULED in
// D-SLM7334 (T-2785 fold 3, answering Q_R-S2g of
// the red-suite record §8).
//
// Verified at v1.5.0 source (src/gpu/gpu_1p0.cpp, sslm_gpu_seq_saveImpl/
// sslm_gpu_seq_restoreImpl; include/superslm/gpu_port.h, SaveGpuSequenceState/
// RestoreGpuSequenceState): the 'SLM4' blob carries seq->live_state (hidden_scale,
// layer_index, kv_saturation_count, context_length), the residual stream, the K/V
// bytes, and the model's own content hash -- and NOTHING ELSE. `bound_schema_index`
// and `dfa_walk_state` are fields of SslmGpuSequenceHandle itself (gpu_1p0.cpp:494-495),
// never passed into SaveGpuSequenceState, and sslm_gpu_seq_restoreImpl allocates a
// FRESH handle via sslm_gpu_seq_create (whose own default leaves bound_schema_index at
// its unbound sentinel) and never sets either field from the blob. A schema bound and
// advanced past its start state before a save is therefore UNRECOVERABLE by restore at
// this pin: SslmGpuSeqSetSchemaForG5Bridge's own precondition (gpu_1p0.h) admits only a
// fresh/reset (walk-state-0) sequence, so no verb this ABI exposes can rebind a schema
// AT a resumed, non-zero walk state. This confirms the routed finding's own claim.
//
// Two arms, per D-SLM7334 (both this file's own tests, matching plan §9 R-S2g's own
// "(i)"/"(ii)" split), recorded in full in SuperSLMGpuSubsystem.h's own SaveSequence()
// doc comment:
//   (i) Bound, walk advanced past 0: SaveSequence() refuses by name, before any blob is
//       written; the original sequence is untouched and finishes constrained.
//   (ii) Bound, walk still at 0 (the post-bind state): SaveSequence() succeeds, the
//        wrapper carries the schema's name, and RestoreSequence() re-binds it by name
//        before the first decode; the continuation is token-identical to an unsaved run.
// No new ESuperSLMRestoreResult enumerator is needed for either arm.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// kSslmGpuDfaWalkStateUnused (ThirdParty gpu_1p0_g5_bridge.h), the value GetSchemaWalkState() reads
	// when no computed reading has been stored; declared here so the test needs no Layer-1 header.
	constexpr uint32 kWalkStateUnused = 0xFFFFFFFFu;

	// T-2826 build log §3: the demo schema (`potion_shop_order`) bound to the pinned
	// prompt dead-ends after a MEASURED 29 tokens, identical on both GPU paths --
	// well under this file's own MaxNewTokens=48, so both of this file's own
	// constructions are EXPECTED to reach Faulted/SchemaDeadEnd, never Complete
	// (§5.3: "the schema dead-ends at 29 tokens ... the sequence ends
	// Faulted/SchemaDeadEnd, exactly as the CPU backend's -2 does"). Fixtures'
	// RunGpuGenerationToCompletion() treats Faulted as a test FAILURE, which is
	// correct for cells that expect a clean stop -- R-S2g's own claim is about
	// whether a save/restore CHANGES what a sequence does, not about whether the
	// schema itself resolves the prompt, so this file compares the REAL terminal
	// outcome (Complete or Faulted, whichever the construction genuinely reaches)
	// against an independently-run unsaved reference's own terminal outcome, rather
	// than asserting either one by name.
	bool RunGpuGenerationToTerminalState(
		USuperSLMGpuSubsystem& Subsystem,
		const FSuperSLMGpuSequence& Seq,
		const FSuperSLMGenerationRequest& Request,
		TArray<int32>& OutTokens,
		ESuperSLMSequencePhase& OutPhase,
		double MaxWallClockSeconds,
		FString& OutError,
		bool bAlreadyInProgress = false)
	{
		if (!bAlreadyInProgress)
		{
			const FSuperSLMLifecycleOpHandle Handle = Subsystem.RequestBeginGeneration(Seq, Request);
			if (!Handle.IsValid())
			{
				OutError = FString::Printf(TEXT("RequestBeginGeneration failed: %s"), *Subsystem.GetLastLifecycleRequestError());
				return false;
			}
		}
		constexpr float StepSeconds = 1.0f / 60.0f;
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - StartSeconds < MaxWallClockSeconds)
		{
			if (NextTickGatedOnDevice(Subsystem))
			{
				FPlatformProcess::Sleep(0.001f); // round 2, finding 6: the device is the gate
				continue;
			}
			Subsystem.Tick(StepSeconds);
			const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				OutTokens = Subsystem.GetGeneratedTokens(Seq);
				OutPhase = Phase;
				return true;
			}
		}
		OutError = FString::Printf(TEXT("did not reach a terminal state (Complete or Faulted) within %.0f seconds"), MaxWallClockSeconds);
		return false;
	}
}

// --- Arm (i): save a schema-bound sequence after its walk advances, restore it,
// and compare the delivered total and terminal state with an unsaved run. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SchemaSaveRestoreAdvancedWalkResumesTest,
	"SuperSLM.L2S2.SchemaSaveRestore.AdvancedWalkResumes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SchemaSaveRestoreAdvancedWalkResumesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

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
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(1);
	Config.K = AExNumHiddenLayers;
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

	// 1. Bind the schema.
	FSuperSLMGpuSchemaHandle Schema;
	FString LookupError;
	if (!TestTrue(*FString::Printf(TEXT("the demo schema must resolve by name (%s)"), *LookupError),
			FSuperSLMGpuSchemaLookup::LookupByName(*Model, DemoSchemaName(), Schema, LookupError)))
	{
		return false;
	}

	// 2. Prefill (shared prompt/config for both the reference run and the
	// interrupted one below).
	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("Tokenize() must succeed"), CpuForTokenize->Tokenize(PinnedSelfCheckPrompt(), Request.PromptTokens)))
	{
		return false;
	}
	Request.MaxNewTokens = 48;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// --- Unsaved reference run: bind, prefill, decode to its REAL terminal state (the
	// schema dead-ends at a measured 29 tokens on this prompt, T-2826 build log §3 --
	// this run never asserts Complete by name, only records what it genuinely
	// reaches). The interrupted construction below is compared against THIS, not
	// against a hard-coded expectation. ---
	TArray<int32> ReferenceTokens;
	ESuperSLMSequencePhase ReferencePhase = ESuperSLMSequencePhase::Idle;
	{
		FSuperSLMGpuSequence RefSeq;
		if (!TestEqual(TEXT("VendSequence() (reference)"), (uint8)Gpu->VendSequence(RefSeq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString RefSchemaError;
		if (!TestTrue(*FString::Printf(TEXT("SetSchema() (reference) must succeed (%s)"), *RefSchemaError), Gpu->SetSchema(RefSeq, Schema, RefSchemaError)))
		{
			return false;
		}
		FString RefRunError;
		const bool bRefOk = RunGpuGenerationToTerminalState(*Gpu, RefSeq, Request, ReferenceTokens, ReferencePhase, /*MaxWallClockSeconds*/ 60.0, RefRunError);
		Gpu->ReturnSequence(RefSeq);
		if (!TestTrue(*FString::Printf(TEXT("reference run must reach a terminal state (%s)"), *RefRunError), bRefOk))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("reference run: %d tokens, terminal phase %d"), ReferenceTokens.Num(), (int32)ReferencePhase));
	}

	// --- The interrupted construction: bind, prefill, decode until the walk has
	// advanced, then attempt a save mid-walk. ---
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FString SchemaError;
	if (!TestTrue(*FString::Printf(TEXT("SetSchema() on a fresh sequence must succeed (%s)"), *SchemaError), Gpu->SetSchema(Seq, Schema, SchemaError)))
	{
		return false;
	}
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
	{
		return false;
	}

	// 3. Decode a few constrained tokens -- enough ticks for the composed path (1
	// layer/slice, the finest granularity, so the most ticks per token) to reach and
	// pass a few full tokens' worth of decode, advancing the DFA walk state past 0.
	const double WalkStartSeconds = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - WalkStartSeconds < 60.0)
	{
		Gpu->Tick(1.0f / 60.0f);
		if (Gpu->GetGeneratedTokens(Seq).Num() >= 3 || Gpu->GetPhase(Seq) == ESuperSLMSequencePhase::Faulted)
		{
			break;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	const uint32 WalkStateBeforeSave = Gpu->GetSchemaWalkState(Seq);
	AddInfo(FString::Printf(TEXT("GetSchemaWalkState() before save = %u (0 would mean this construction failed to advance past the schema's start state)"), WalkStateBeforeSave));
	if (!TestTrue(TEXT("the construction must have advanced the DFA walk state past its start (0) before saving -- otherwise this is not the lossy case R-S2g exists to catch"),
			WalkStateBeforeSave != 0))
	{
		return false;
	}
	const TArray<int32> TokensBeforeSave = Gpu->GetGeneratedTokens(Seq);

	// 4. Save via the plugin.
	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const ESuperSLMRestoreResult SaveResult = DriveSaveToResolution(*Gpu, SaveHandle, Blob);
	if (!TestEqual(TEXT("a schema-bound mid-walk save succeeds"), (uint8)SaveResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	const TArray<int32> ObservedAtSave = Gpu->GetGeneratedTokens(Seq);
	if (!TestTrue(TEXT("save resolution cannot lose previously observed tokens"), ObservedAtSave.Num() >= TokensBeforeSave.Num()))
	{
		return false;
	}
	TSet<int32> DistinctContinuation;
	for (int32 I = ObservedAtSave.Num(); I < ReferenceTokens.Num(); ++I)
	{
		DistinctContinuation.Add(ReferenceTokens[I]);
	}
	if (!TestTrue(TEXT("the reference has at least eight distinct tokens after the save point"), DistinctContinuation.Num() >= 8))
	{
		return false;
	}
	Gpu->ReturnSequence(Seq);
	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestEqual(TEXT("schema-bound restore succeeds"),
			(uint8)DriveRestoreToResolution(*Gpu, RestoreHandle, Restored), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	if (!TestEqual(TEXT("the schema remains bound after restore"), Gpu->GetBoundSchema(Restored).Index, Schema.Index))
	{
		return false;
	}

	// Plan §9 R-S2g, arm (i)'s walk record across the restore (ruling 2026-09-26 as folded,
	// review finding 6; kills M-63): the save carried results, so the restored sequence has
	// pending events right after the restore resolves. Then the restored sequence is driven here,
	// one Tick() at a time, and after every tick until a terminal phase its walk-state record must
	// never read the default. Derived (corrected in round 2, finding 9): a carried event's delay is
	// at most K - 1, so the carried events apply by r + K - 1, where r is the restore's finalize
	// tick, and the first fresh token, which can be planned on r itself, applies at r + K or later.
	// So a carried result that stored the default would show on at least one tick between them.
	// The loop sleeps 1 ms instead of ticking while the next Tick() would be gated on the device
	// (round 2, finding 6).
	const int32 PendingAfterRestore = FSuperSLMGpuTestAccess::GetPendingEventCount(*Gpu, Restored);
	AddInfo(FString::Printf(TEXT("GetPendingEventCount(Restored) right after the restore resolved = %d"), PendingAfterRestore));
	if (!TestTrue(TEXT("the save carried results: GetPendingEventCount(Restored) >= 1 right after the restore resolves"), PendingAfterRestore >= 1))
	{
		Gpu->ReturnSequence(Restored);
		return false;
	}

	// The restored sequence is already in progress; the save's delivered prefix and
	// its continuation must equal the independently-run unsaved session.
	TArray<int32> TailTokens;
	ESuperSLMSequencePhase FinalPhase = ESuperSLMSequencePhase::Idle;
	bool bCompleted = false;
	int32 WalkTicks = 0;
	int32 UnusedWalkTicks = 0;
	int32 FirstUnusedWalkTick = -1;
	{
		const double RestoredStart = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - RestoredStart < 60.0)
		{
			if (NextTickGatedOnDevice(*Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu->Tick(1.0f / 60.0f);
			++WalkTicks;
			const ESuperSLMSequencePhase Phase = Gpu->GetPhase(Restored);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted)
			{
				TailTokens = Gpu->GetGeneratedTokens(Restored);
				FinalPhase = Phase;
				bCompleted = true;
				break;
			}
			if (Gpu->GetSchemaWalkState(Restored) == kWalkStateUnused && UnusedWalkTicks++ == 0)
			{
				FirstUnusedWalkTick = WalkTicks;
			}
		}
	}
	Gpu->ReturnSequence(Restored);
	TestEqual(*FString::Printf(TEXT("GetSchemaWalkState(Restored) never reads kSslmGpuDfaWalkStateUnused after a tick before the terminal phase (%d ticks; first at tick %d)"),
		WalkTicks, FirstUnusedWalkTick), UnusedWalkTicks, 0);
	if (!TestTrue(TEXT("the restored sequence must reach a terminal state within 60 s"), bCompleted))
	{
		return false;
	}

	TArray<int32> ResumedTokens = ObservedAtSave;
	ResumedTokens.Append(TailTokens);
	const bool bPhaseMatches = TestEqual(TEXT("restored terminal phase matches the reference"), (uint8)FinalPhase, (uint8)ReferencePhase);
	const bool bTokensMatch = TestEqual(TEXT("save prefix plus restored continuation matches the unsaved reference"), ResumedTokens, ReferenceTokens);
	return bPhaseMatches && bTokensMatch;
}

// --- Arm (ii): walk still at its post-bind state (0) -- SaveSequence() succeeds, the
// wrapper carries the schema's name, and RestoreSequence() re-binds it by name before
// the restored handle's first decode; the continuation is token-identical to an
// unsaved run of the same session. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2SchemaSaveRestoreUnadvancedReboundAfterRestoreTest,
	"SuperSLM.L2S2.SchemaSaveRestore.UnadvancedReboundAfterRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2SchemaSaveRestoreUnadvancedReboundAfterRestoreTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

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
	Config.DispatchBudget = DispatchBudgetForLayersPerSlice(1);
	Config.K = AExNumHiddenLayers;
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

	FSuperSLMGpuSchemaHandle Schema;
	FString LookupError;
	if (!TestTrue(*FString::Printf(TEXT("the demo schema must resolve by name (%s)"), *LookupError),
			FSuperSLMGpuSchemaLookup::LookupByName(*Model, DemoSchemaName(), Schema, LookupError)))
	{
		return false;
	}

	FSuperSLMGenerationRequest Request;
	if (!TestTrue(TEXT("Tokenize() must succeed"), CpuForTokenize->Tokenize(PinnedSelfCheckPrompt(), Request.PromptTokens)))
	{
		return false;
	}
	Request.MaxNewTokens = 48;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;

	// --- Unsaved reference run: bind, prefill, decode to completion, no save/restore. ---
	TArray<int32> ReferenceTokens;
	ESuperSLMSequencePhase ReferencePhase = ESuperSLMSequencePhase::Idle;
	{
		FSuperSLMGpuSequence RefSeq;
		if (!TestEqual(TEXT("VendSequence() (reference)"), (uint8)Gpu->VendSequence(RefSeq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString SchemaError;
		if (!TestTrue(*FString::Printf(TEXT("SetSchema() (reference) must succeed (%s)"), *SchemaError), Gpu->SetSchema(RefSeq, Schema, SchemaError)))
		{
			return false;
		}
		FString RunError;
		const bool bCompleted = RunGpuGenerationToTerminalState(*Gpu, RefSeq, Request, ReferenceTokens, ReferencePhase, /*MaxWallClockSeconds*/ 60.0, RunError);
		Gpu->ReturnSequence(RefSeq);
		if (!TestTrue(*FString::Printf(TEXT("reference run must reach a terminal state (%s)"), *RunError), bCompleted))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("reference run: %d tokens, terminal phase %d"), ReferenceTokens.Num(), (int32)ReferencePhase));
	}

	// --- Interrupted run: bind, prefill, save WHILE the walk is still at its post-bind
	// state (0) -- before any schema-constrained token has been produced -- restore,
	// then continue. ---
	FSuperSLMGpuSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed), (uint8)ESuperSLMGpuVendResult::Success))
	{
		return false;
	}
	FString SchemaError;
	if (!TestTrue(*FString::Printf(TEXT("SetSchema() must succeed (%s)"), *SchemaError), Gpu->SetSchema(Seq, Schema, SchemaError)))
	{
		return false;
	}
	// Plan §2.5 row 21, "Ruling 2026-09-26": SetSchema() always holds the bind in the op log, and
	// GetBoundSchema() reports it once the bind has reached Layer 1. This arm's tick count below
	// assumed the old at-call bind, so the bind is driven to completion first, through paced
	// ticks, before the generation is requested.
	{
		const double BindDeadline = FPlatformTime::Seconds() + 60.0;
		while (Gpu->GetBoundSchema(Seq).Index != Schema.Index && FPlatformTime::Seconds() < BindDeadline)
		{
			if (!PacedTick(*this, *Gpu, TEXT("R-S2g (ii) bind")))
			{
				return false;
			}
		}
		if (!TestEqual(TEXT("GetBoundSchema() reports the held bind before the 60 s deadline"), Gpu->GetBoundSchema(Seq).Index, Schema.Index))
		{
			return false;
		}
	}
	const FSuperSLMLifecycleOpHandle BeginHandle = Gpu->RequestBeginGeneration(Seq, Request);
	if (!TestTrue(*FString::Printf(TEXT("RequestBeginGeneration() must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), BeginHandle.IsValid()))
	{
		return false;
	}
	// T-2885 finding 5 (the code review record): a prior round justified
	// "one tick" here by quoting plan §5.1 as "GPU prompt prefill is atomic" -- that sentence
	// appears nowhere in the plan (grepped whole), §5.1 is titled
	// "Instrumentation and Unreal Insights" and has nothing to do with prefill, and the plan
	// states the OPPOSITE: D-SLM7379 (plan §5) retires the atomic batched-prefill verb outright
	// -- prompts are fed as embed-plus-layers per token, the same slicing decode uses, with no
	// call to the atomic verb on any path. One tick therefore reaches only layer 1 of this
	// prompt's own first token, not the post-prefill state this arm's own name promises
	// ("Unadvanced" -- i.e. the walk's post-BIND state, reached only once the WHOLE prompt has
	// been fed to full depth). The walk-state claim itself still holds, independently: plan §5
	// (D-SLM7392 passage) states the DFA walk "moves only inside Layer 1's finish", which prefill
	// -- embed plus layer dispatch, no finish call -- never reaches, so the walk cannot advance
	// during prefill regardless of how many prompt tokens it is fed. Reaching the true boundary
	// needs one one-call-equivalent tick per prompt token: at this file's own
	// DispatchBudgetForLayersPerSlice(1) (1 layer/tick), each of the tokenized prompt's own
	// Request.PromptTokens.Num() tokens needs AExNumHiddenLayers (24) ticks to reach full depth
	// before the next one embeds (D-SLM7379) -- computed from the REAL tokenized prompt length,
	// never a hard-coded token count, since Tokenize() is the real tokenizer's own output.
	const int32 TicksToFinishPrefill = Request.PromptTokens.Num() * AExNumHiddenLayers;
	// Paced (ruling 2026-09-26): the tick no longer waits on the device, so each of these ticks
	// waits, through the test access, until the device has caught up -- the count keeps its
	// meaning of "one layer of one prompt token per tick".
	for (int32 T = 0; T < TicksToFinishPrefill; ++T)
	{
		if (!PacedTick(*this, *Gpu, TEXT("R-S2g (ii) prefill")))
		{
			return false;
		}
	}
	if (!TestEqual(TEXT("the whole prompt must be consumed with no schema-constrained decode token produced yet -- otherwise this is not this arm's own post-bind boundary"),
			Gpu->GetGeneratedTokens(Seq).Num(), 0))
	{
		return false;
	}
	const uint32 WalkStateBeforeSave = Gpu->GetSchemaWalkState(Seq);
	AddInfo(FString::Printf(TEXT("GetSchemaWalkState() before save = %u (must be 0 -- this is the arm (ii) construction, the walk's post-bind state)"), WalkStateBeforeSave));
	if (!TestEqual(TEXT("the walk state must still be 0 (post-bind) before this arm's save"), WalkStateBeforeSave, (uint32)0))
	{
		return false;
	}

	TArray<uint8> Blob;
	const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
	const bool bSaveOk = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
	if (!TestTrue(*FString::Printf(TEXT("RequestSaveSequence() at the post-bind state must succeed (%s)"), *Gpu->GetLastLifecycleRequestError()), bSaveOk))
	{
		return false;
	}
	Gpu->ReturnSequence(Seq);

	FSuperSLMGpuSequence Restored;
	const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
	if (!TestEqual(*FString::Printf(TEXT("RequestRestoreSequence() must return Success (%s)"), *Gpu->GetLastLifecycleRequestError()),
			(uint8)DriveRestoreToResolution(*Gpu, RestoreHandle, Restored), (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	// The restored handle must be RE-BOUND to the same schema, by name, at the walk's
	// own post-bind state (D-SLM7334: "restore re-binds it by name before the first
	// decode") -- never left unbound, which would be exactly the silent-unconstrained
	// defect R-S2g exists to catch.
	if (!TestEqual(TEXT("the restored sequence must be re-bound to the same schema"), Gpu->GetBoundSchema(Restored).Index, Schema.Index))
	{
		return false;
	}
	// T-2816 round 9 (2026-09-19): informational only, not a hard gate. Measured: with this
	// arm's own tick count corrected to reach the REAL post-prefill boundary (this file's own
	// comment above, replacing a 1-tick stand-in that never got close enough to race), this
	// accessor was observed reading non-zero here on a run whose FINAL assertions below --
	// schema re-bind and full token-identical continuation -- both still passed. That is
	// consistent with SuperSLMGpuSubsystem.h's own documented caveat on GetSchemaWalkState():
	// "may run ahead by up to K ticks" (D-SLM7384) -- checked immediately after
	// DriveRestoreToResolution() returns, this accessor can observe the SUBMISSION thread
	// already mid-way through the restored sequence's first post-restore decode step, which is
	// correct plugin behaviour, not a defect this cell means to catch. R-S2g's own claim --
	// schema-constrained determinism survives a save/restore round trip -- is what the
	// unconditional assertions below actually prove; this line stays as a diagnostic because a
	// walk state genuinely stuck away from 0 for the WHOLE remaining run would still show up
	// there, as a token divergence.
	AddInfo(FString::Printf(TEXT("GetSchemaWalkState(Restored) immediately after restore = %u (informational -- see this line's own comment; not asserted 0 here)"), Gpu->GetSchemaWalkState(Restored)));

	TArray<int32> RestoredTokens;
	ESuperSLMSequencePhase RestoredPhase = ESuperSLMSequencePhase::Idle;
	FString RunError;
	const bool bRestoredCompleted = RunGpuGenerationToTerminalState(*Gpu, Restored, Request, RestoredTokens, RestoredPhase, /*MaxWallClockSeconds*/ 60.0, RunError, /*bAlreadyInProgress*/ true);
	Gpu->ReturnSequence(Restored);
	if (!TestTrue(*FString::Printf(TEXT("the restored sequence must reach a terminal state (%s)"), *RunError), bRestoredCompleted))
	{
		return false;
	}

	// FEAT oracle: the restored, re-bound continuation must reach the SAME real
	// terminal state (Complete or Faulted, whichever the reference genuinely reaches --
	// maintainer's own instruction, 2026-09-19) and be EXACTLY token-identical to the
	// unsaved reference run (plan §9 R-S2g arm ii) -- a mutation that fails to re-bind
	// the schema (or re-binds the wrong one, or leaves the walk state anything but 0)
	// changes at least one generated token, because an unconstrained or
	// differently-constrained decode diverges from the schema-constrained reference at
	// the first token the two paths disagree on.
	const bool bPhaseMatches = TestEqual(TEXT("the restored sequence's terminal phase must match the unsaved reference run's"), (uint8)RestoredPhase, (uint8)ReferencePhase);
	const bool bTokensMatch = TestEqual(TEXT("the restored, re-bound continuation must be token-identical to the unsaved reference run"), RestoredTokens, ReferenceTokens);
	return bPhaseMatches && bTokensMatch;
}

#endif // WITH_DEV_AUTOMATION_TESTS
