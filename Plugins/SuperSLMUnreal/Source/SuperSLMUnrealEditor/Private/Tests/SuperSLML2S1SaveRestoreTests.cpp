// T-2805 -- L2-S1 red suite, round 9. Plan cell R-S1c (the plan
// §9, as revised by D-SLM7408/D-SLM7421/D-SLM7423 -- the async worker tick, T-2850 folds 1-3):
// "Save and load mid-generation continues exactly" (dims 2, 9). "A-EX: save at token 20, restore
// into a fresh handle, continue to 48 -> token-identical to the uninterrupted run. Restore against
// a different artifact -> SSLM_RESTORE_MODEL_MISMATCH surfaced by name. A GPU-tagged blob offered
// to the CPU restore -> refused by the plugin's backend tag. A sequence with the A-AD adapter
// bound, saved and restored -> the adapter is re-bound and the continuation matches the unsaved
// run; the same blob restored with that adapter unmapped -> AdapterUnavailable by name. Save and
// restore are queued lifecycle operations on the CPU (D-SLM7408): the cell reads each blob/result
// through the handle SaveSequence()/RestoreSequence() returned (GetSaveResult/GetLifecycleOpResult,
// §5 item 3, D-SLM7421), never a synchronous return."
//
// D-SLM7408 moves Save and Restore from the T-2815 build's direct call+synchronous-return shape
// to the SAME queued-handle shape ResetSequence()/AdoptPrefix() already use: SaveSequence() and
// RestoreSequence() now return ESuperSLMRestoreResult::Success (QUEUED, not completed) or
// SequenceQueueFull at once, with the real blob/result read later via GetSaveResult()/
// GetLifecycleOpResult(). Every one of this file's four tests is rewritten to that shape this
// round; none of the four claims themselves changed.
//
// Four tests realize the cell's four clauses. The first two and the fourth need the
// tokenizer-bearing A-EX rebuild (D-SLM7339) and fail the cell by name (T-2805 L23 repair) when
// it is absent, never a silent pass. The third needs only A-CPU (already on this box) plus
// DebugOverwriteBackendTag (SuperSLMSaveRestoreTypes.h) -- it feeds recorded token ids directly
// rather than calling Tokenize() against A-CPU, which carries no tokenizer (D-SLM7339).
//
// SUPERSLM_WITH_L2S1_ASYNC (SuperSLMSlotGates.h): every test in this file was rewritten to the
// handle-based SaveSequence()/RestoreSequence() shape and does not compile against T-2815's
// synchronous, direct-return build. The switch is always 1 in 1.0, so these tests build and
// run.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
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

	bool ConfigureFor(FAutomationTestBase& T, UWorld* World, USuperSLMModel* Model, USuperSLMSubsystem*& OutSubsystem, int32 BlockCount = 2)
	{
		OutSubsystem = GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), OutSubsystem))
		{
			return false;
		}
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.BlockCount = BlockCount;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		const FSuperSLMConfigureReport Report = OutSubsystem->Configure(Model, Config);
		return T.TestEqual(TEXT("Configure() result"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	// A real, valid token array with no dependency on any artifact's tokenizer -- the first
	// R-S1a reference case's own recorded prompt tokens (Tests/Fixtures/L2S1/PROVENANCE.md),
	// reused here as "some real prompt" for a cell that is testing save/restore mechanics, not
	// text handling. Loads and asserts real; a test using this helper fails loudly if it cannot.
	bool LoadSomeRealPromptTokens(FAutomationTestBase& T, TArray<int32>& OutTokens)
	{
		TArray<FReferenceCase> Cases;
		FString LoadError;
		const bool bLoaded = LoadReferenceCases(Cases, LoadError);
		if (!T.TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), bLoaded))
		{
			return false;
		}
		OutTokens = Cases[0].PromptTokens;
		return true;
	}

	// D-SLM7408 (round 9): SaveSequence() now queues and returns at once. Queues, polls
	// GetSaveResult() to completion, and returns the drained result + blob -- the ONE place this
	// file's tests wait out a save, so a defect in the wait itself cannot silently differ
	// test to test. FastAsPossible pacing (T-2805 round 10, §14.7): a content cell, real
	// wall-clock time given between polls via Fixtures.h's own DrainTicks.
	ESuperSLMRestoreResult SaveAndDrain(FAutomationTestBase& T, USuperSLMSubsystem& Subsystem,
		const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob, FString& OutError)
	{
		FSuperSLMLifecycleOpHandle Handle;
		const ESuperSLMRestoreResult QueueResult = Subsystem.SaveSequence(Seq, Handle, OutError);
		if (!T.TestEqual(*FString::Printf(TEXT("SaveSequence must queue: %s"), *OutError),
				(uint8)QueueResult, (uint8)ESuperSLMRestoreResult::Success))
		{
			return QueueResult;
		}
		ESuperSLMRestoreResult SaveResult = ESuperSLMRestoreResult::Pending;
		DrainTicks(Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&Subsystem, &Handle, &OutBlob, &SaveResult]() { SaveResult = Subsystem.GetSaveResult(Handle, OutBlob); return SaveResult != ESuperSLMRestoreResult::Pending; },
			/*MaxWallClockSeconds*/ 30.0);
		return SaveResult;
	}

	// D-SLM7408 (round 9): RestoreSequence() now queues and returns at once, with the vended
	// sequence handle available immediately (OutSequence) but the named restore result only once
	// it drains. Queues, polls GetLifecycleOpResult() to completion.
	ESuperSLMRestoreResult RestoreAndDrain(FAutomationTestBase& T, USuperSLMSubsystem& Subsystem,
		const TArray<uint8>& Blob, USuperSLMModel* ExpectedModel, FSuperSLMSequence& OutSequence,
		FString& OutError)
	{
		FSuperSLMLifecycleOpHandle Handle;
		const ESuperSLMRestoreResult QueueResult = Subsystem.RestoreSequence(Blob, ExpectedModel, OutSequence, Handle, OutError);
		// A synchronous refusal (BackendMismatch, PoolExhausted, NotConfigured -- checked before
		// anything is queued, §4) is returned here directly and never reaches Pending; a genuine
		// Layer-1-sourced result (ModelMismatch, AdapterUnavailable, Success, ...) queues first
		// and is Pending until it drains.
		if (QueueResult != ESuperSLMRestoreResult::Success && QueueResult != ESuperSLMRestoreResult::Pending)
		{
			return QueueResult;
		}
		DrainTicks(Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&Subsystem, &Handle]() { return Subsystem.GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; },
			/*MaxWallClockSeconds*/ 30.0);
		return Subsystem.GetLifecycleOpResult(Handle);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SaveRestoreContinuesExactlyTest,
	"SuperSLM.L2S1.SaveRestore.ContinuesExactly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SaveRestoreContinuesExactlyTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* Model = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1c ContinuesExactly"), Model))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}
	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), Model, Subsystem))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	Subsystem->Tokenize(TEXT("Describe a busy Saturday at the tattoo shop."), PromptTokens);
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 48;

	// --- Uninterrupted reference run ---
	FSuperSLMSequence RefSeq;
	if (!TestEqual(TEXT("Vend reference sequence"), (uint8)Subsystem->VendSequence(RefSeq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	TArray<int32> ReferenceTokens;
	FString RefError;
	const bool bRefCompleted = RunGenerationToCompletion(*Subsystem, RefSeq, Request, ReferenceTokens, /*MaxWallClockSeconds*/ 60.0, RefError);
	Subsystem->ReturnSequence(RefSeq);
	if (!TestTrue(*FString::Printf(TEXT("reference run must complete (%s)"), *RefError), bRefCompleted))
	{
		return false;
	}
	if (!TestTrue(TEXT("reference run must produce at least 20 tokens (save point)"), ReferenceTokens.Num() >= 20))
	{
		return false;
	}

	// --- Interrupted run: save at token 20, restore into a fresh handle, continue to 48 ---
	FSuperSLMSequence LiveSeq;
	if (!TestEqual(TEXT("Vend live sequence"), (uint8)Subsystem->VendSequence(LiveSeq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(LiveSeq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}

	TArray<uint8> SavedBlob;
	bool bSavedAtTwenty = false;
	// T-2805 round 10 (§14.7): FastAsPossible in spirit (a content cell) -- this loop's own "save
	// exactly at token 20" logic is not a simple Done() predicate, so it stays a manual loop, with
	// a real Sleep between polls (only while neither exit condition has fired) so the worker
	// genuinely gets to advance the live sequence between checks.
	{
		const double DrainStartSeconds = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - DrainStartSeconds < 60.0)
		{
			Subsystem->Tick(1.0f / 60.0f);
			if (!bSavedAtTwenty && Subsystem->GetGeneratedTokens(LiveSeq).Num() >= 20)
			{
				FString SaveError;
				const ESuperSLMRestoreResult SaveOutcome = SaveAndDrain(*this, *Subsystem, LiveSeq, SavedBlob, SaveError);
				bSavedAtTwenty = TestEqual(*FString::Printf(TEXT("SaveSequence at token 20: %s"), *SaveError),
					(uint8)SaveOutcome, (uint8)ESuperSLMRestoreResult::Success);
				break;
			}
			if (Subsystem->GetPhase(LiveSeq) == ESuperSLMSequencePhase::Complete ||
				Subsystem->GetPhase(LiveSeq) == ESuperSLMSequencePhase::Faulted)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
	}
	if (!TestTrue(TEXT("must have saved at token 20 before completing"), bSavedAtTwenty))
	{
		return false;
	}
	const TArray<int32> TokensBeforeSave = Subsystem->GetGeneratedTokens(LiveSeq);
	Subsystem->ReturnSequence(LiveSeq);

	FSuperSLMSequence RestoredSeq;
	FString RestoreError;
	const ESuperSLMRestoreResult RestoreResult = RestoreAndDrain(*this, *Subsystem, SavedBlob, Model, RestoredSeq, RestoreError);
	if (!TestEqual(*FString::Printf(TEXT("RestoreSequence result: %s"), *RestoreError),
			(uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}

	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &RestoredSeq]()
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(RestoredSeq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		},
		/*MaxWallClockSeconds*/ 60.0);
	TArray<int32> ContinuedTokens = TokensBeforeSave;
	ContinuedTokens.Append(Subsystem->GetGeneratedTokens(RestoredSeq));
	Subsystem->ReturnSequence(RestoredSeq);

	// FEAT oracle: the save-interrupted, restore-and-continue path must produce EXACTLY the
	// same tokens as the uninterrupted reference run -- a mutation that drops or corrupts any
	// part of the saved KV/residual state changes at least one token from token 21 onward.
	return TestEqual(TEXT("save/restore/continue tokens must equal the uninterrupted reference run"),
		ContinuedTokens, ReferenceTokens);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1RestoreAgainstDifferentArtifactRejectedTest,
	"SuperSLM.L2S1.SaveRestore.RestoreAgainstDifferentArtifactRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1RestoreAgainstDifferentArtifactRejectedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	USuperSLMModel* AExModel = nullptr;
	switch (ImportAEx(*this, TEXT("R-S1c RestoreAgainstDifferentArtifactRejected"), AExModel))
	{
		case EAExAvailability::Absent: return false;
		case EAExAvailability::ImportFailed: return false;
		case EAExAvailability::Ready: break;
	}
	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), AExModel, Subsystem))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	Subsystem->Tokenize(TEXT("One short reply."), PromptTokens);
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 24;

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	TArray<int32> Generated;
	FString RunError;
	const bool bGenCompleted = RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError);
	if (!TestTrue(*FString::Printf(TEXT("generation must complete before saving (%s)"), *RunError), bGenCompleted))
	{
		return false;
	}
	TArray<uint8> Blob;
	FString SaveError;
	const ESuperSLMRestoreResult SaveOutcome = SaveAndDrain(*this, *Subsystem, Seq, Blob, SaveError);
	if (!TestEqual(*FString::Printf(TEXT("SaveSequence: %s"), *SaveError), (uint8)SaveOutcome, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	Subsystem->ReturnSequence(Seq);

	// A different real artifact: A-CPU, a different model entirely (different weights, and per
	// §12 decision 9 a different context_cap than A-EX's 4096) -- never a hand-constructed
	// "different model" stand-in. Imported but never tokenized against, so A-CPU's own missing
	// tokenizer (D-SLM7339) does not affect this test.
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* DifferentModel = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag);
	if (!TestNotNull(TEXT("A-CPU must import as the 'different artifact'"), DifferentModel) ||
		!TestTrue(TEXT("A-CPU Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}

	FSuperSLMSequence RestoredSeq;
	FString RestoreError;
	const ESuperSLMRestoreResult RestoreResult = RestoreAndDrain(*this, *Subsystem, Blob, DifferentModel, RestoredSeq, RestoreError);
	// R-S1c L22 repair (T-2805, D-SLM7544): "surfaced by name" is satisfied by this typed result
	// reaching the caller through GetLifecycleOpResult -- the channel R-S1c's own closing clause
	// names as required ("never a synchronous return"). RestoreError is that synchronous return's
	// out-param, the channel R-S1c explicitly rules out; asserting it non-empty checked a claim
	// R-S1c does not make, and is dropped rather than kept as a second, uncited assertion.
	return TestEqual(TEXT("restore against a different artifact must surface ModelMismatch"),
		(uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::ModelMismatch);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1GpuTaggedBlobRefusedByCpuRestoreTest,
	"SuperSLM.L2S1.SaveRestore.GpuTaggedBlobRefusedByCpuRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1GpuTaggedBlobRefusedByCpuRestoreTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	// Runs against A-CPU, already present on this box -- this test does not need A-EX or a real
	// GPU backend (L2-S2), only a well-formed CPU-tagged blob and DebugOverwriteBackendTag.
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(SuperSLML2S1Fixtures::ACpuArtifactPath(), Diag);
	if (!TestNotNull(TEXT("A-CPU must import"), Model) || !TestTrue(TEXT("A-CPU Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), Model, Subsystem))
	{
		return false;
	}

	// A-CPU carries no tokenizer (D-SLM7339) -- feed a real, already-tokenized prompt (the R-S1a
	// fixture's own recorded ids, the same artifact) instead of calling Tokenize().
	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 8;

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("Vend sequence"), (uint8)Subsystem->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	TArray<int32> Generated;
	FString RunError;
	if (!TestTrue(TEXT("generation must complete before saving"), RunGenerationToCompletion(*Subsystem, Seq, Request, Generated, /*MaxWallClockSeconds*/ 60.0, RunError)))
	{
		return false;
	}
	TArray<uint8> Blob;
	FString SaveError;
	const ESuperSLMRestoreResult SaveOutcome = SaveAndDrain(*this, *Subsystem, Seq, Blob, SaveError);
	if (!TestEqual(*FString::Printf(TEXT("SaveSequence: %s"), *SaveError), (uint8)SaveOutcome, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	Subsystem->ReturnSequence(Seq);

	// Mutation: a genuinely CPU-saved, well-formed blob, with ONLY its own backend tag flipped
	// to GPU -- everything else (the Layer-1 payload, the model/pin tags) is untouched real
	// output. A restore path that checks only Layer-1's own SSLM_RESTORE_MODEL_MISMATCH and
	// never the plugin's own backend tag would ACCEPT this blob (same model, same pin) -- which
	// is exactly the defect this cell exists to catch.
	if (!TestTrue(TEXT("DebugOverwriteBackendTag must succeed on a well-formed blob"),
			SuperSLM::DebugOverwriteBackendTag(Blob, ESuperSLMBackend::GPU)))
	{
		return false;
	}

	FSuperSLMSequence RestoredSeq;
	FString RestoreError;
	// BackendMismatch is the plugin's OWN tag check, refused before anything is queued (§4) --
	// RestoreAndDrain() returns it directly without ever reaching Pending.
	const ESuperSLMRestoreResult RestoreResult = RestoreAndDrain(*this, *Subsystem, Blob, Model, RestoredSeq, RestoreError);
	// L22 repair (T-2805, same class as D-SLM7544's R-S1c ruling): R-S1c's own text for this
	// clause is "refused by the plugin's backend tag" -- it does not say "by name" at all, and the
	// typed BackendMismatch result below is what the clause requires. RestoreError's non-emptiness
	// is not asserted; a non-empty string does not establish that it names anything.
	return TestEqual(TEXT("a GPU-tagged blob offered to the CPU restore must be refused by backend tag"),
		(uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::BackendMismatch);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuLayer1PinMismatchRefusedTest,
	"SuperSLM.U1.Cpu.Layer1PinMismatchRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuLayer1PinMismatchRefusedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag);
	if (!TestNotNull(TEXT("A-CPU import"), Model) || !TestTrue(TEXT("A-CPU accepted"), Diag.bAccepted)) { return false; }
	USuperSLMSubsystem* Cpu = nullptr;
	if (!ConfigureFor(*this, World.GetTestWorld(), Model, Cpu)) { return false; }
	TArray<int32> Prompt;
	if (!LoadSomeRealPromptTokens(*this, Prompt)) { return false; }
	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("vend"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success)) { return false; }
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Prompt;
	Request.MaxNewTokens = 8;
	TArray<int32> Tokens;
	FString Error;
	if (!TestTrue(*Error, RunGenerationToCompletion(*Cpu, Seq, Request, Tokens, 60.0, Error))) { return false; }
	TArray<uint8> Blob;
	if (!TestEqual(TEXT("save succeeds"), (uint8)SaveAndDrain(*this, *Cpu, Seq, Blob, Error), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	Cpu->ReturnSequence(Seq);

	// The wrapper's documented Layer-1 tag occupies bytes [12, 28); change one
	// character of the tag only, leaving the real Layer-1 payload intact.
	if (!TestTrue(TEXT("the saved wrapper carries a nonempty Layer-1 tag"), Blob.Num() >= 28 && Blob[12] != 0)) { return false; }
	Blob[12] = Blob[12] == 'v' ? 'x' : 'v';
	FSuperSLMSequence Restored;
	FSuperSLMLifecycleOpHandle RestoreHandle;
	const ESuperSLMRestoreResult Result = Cpu->RestoreSequence(Blob, Model, Restored, RestoreHandle, Error);
	bool bOk = TestEqual(TEXT("a blob from another Layer-1 pin is refused before queuing"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Layer1Mismatch);
	bOk &= TestFalse(TEXT("pin mismatch creates no restore operation"), RestoreHandle.IsValid());
	bOk &= TestFalse(TEXT("pin mismatch vends no sequence"), Restored.IsValid());
	return bOk;
}

// D-SLM7341: the save wrapper records the bound adapter's identity; restore re-binds it (the
// continuation matches an unsaved run under the same adapter), or refuses by name
// (AdapterUnavailable) when that adapter is not mapped at restore time. Uses A-AD (already on
// this box) for the base model and the real shopkeeper-v2 adapter.
//
// A-AD's base carries no tokenizer either (D-SLM7339 names it for retokenization too, but no
// concrete rebuilt path has been given for it) -- this cell reuses the R-S1a fixture's own
// recorded ids as "some real prompt", the same substitution SuperSLML2S1MisuseTests.cpp's
// CpuAdapterSwapMidTokenDeferred test makes, on the same documented assumption: Qwen2.5's model
// family shares one tokenizer/vocabulary across sizes. Flagged, not hidden -- see
// the red-suite record §6 for the open question this assumption is
// filed under.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1SaveRestoreAdapterRebindsOrRefusesTest,
	"SuperSLM.L2S1.SaveRestore.AdapterRebindsOrRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1SaveRestoreAdapterRebindsOrRefusesTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Base = FSuperSLMModelImport::ImportFromFile(AAdBaseModelPath(), Diag);
	if (!TestNotNull(TEXT("A-AD base (1.5B) must import"), Base) || !TestTrue(TEXT("A-AD base Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}

	FSuperSLMAdapterHandle Adapter;
	FString AdapterError;
	const bool bAdapterImported = FSuperSLMAdapterImport::ImportFromFile(AAdAdapterPath(), *Base, Adapter, AdapterError);
	if (!TestTrue(*FString::Printf(TEXT("adapter import: %s"), *AdapterError), bAdapterImported))
	{
		return false;
	}

	USuperSLMSubsystem* Subsystem = nullptr;
	if (!ConfigureFor(*this, TestWorldWrapper.GetTestWorld(), Base, Subsystem, /*BlockCount*/ 2))
	{
		return false;
	}

	TArray<int32> PromptTokens;
	if (!LoadSomeRealPromptTokens(*this, PromptTokens))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = PromptTokens;
	Request.MaxNewTokens = 24;

	// --- Unsaved reference run under the adapter ---
	FSuperSLMSequence RefSeq;
	if (!TestEqual(TEXT("Vend reference sequence"), (uint8)Subsystem->VendSequence(RefSeq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	Subsystem->RequestAdapterSwap(RefSeq, Adapter);
	// The swap applies once Layer 1 admits it (fresh/Idle sequences admit immediately, §2.1) --
	// drain a tick before beginning generation so the reference run genuinely starts under it.
	Subsystem->Tick(1.0f / 60.0f);
	if (!TestTrue(TEXT("adapter must be active before the reference run begins"), Subsystem->GetActiveAdapter(RefSeq).IsValid()))
	{
		return false;
	}
	TArray<int32> ReferenceTokens;
	FString RefError;
	const bool bRefCompleted = RunGenerationToCompletion(*Subsystem, RefSeq, Request, ReferenceTokens, /*MaxWallClockSeconds*/ 60.0, RefError);
	Subsystem->ReturnSequence(RefSeq);
	if (!TestTrue(*FString::Printf(TEXT("reference run under the adapter must complete (%s)"), *RefError), bRefCompleted))
	{
		return false;
	}

	// --- Save/restore under the SAME adapter: must re-bind and match the reference exactly ---
	FSuperSLMSequence LiveSeq;
	if (!TestEqual(TEXT("Vend live sequence"), (uint8)Subsystem->VendSequence(LiveSeq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	Subsystem->RequestAdapterSwap(LiveSeq, Adapter);
	Subsystem->Tick(1.0f / 60.0f);

	FString BeginError;
	const bool bBeginOk = Subsystem->BeginGeneration(LiveSeq, Request, BeginError);
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration: %s"), *BeginError), bBeginOk))
	{
		return false;
	}
	// Save immediately (before any decode) so the continuation is directly comparable to the
	// full reference run.
	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &LiveSeq]() { return Subsystem->GetPhase(LiveSeq) != ESuperSLMSequencePhase::Prefilling; },
		/*MaxWallClockSeconds*/ 60.0);
	TArray<uint8> Blob;
	FString SaveError;
	const ESuperSLMRestoreResult SaveOutcome = SaveAndDrain(*this, *Subsystem, LiveSeq, Blob, SaveError);
	if (!TestEqual(*FString::Printf(TEXT("SaveSequence with adapter bound: %s"), *SaveError),
			(uint8)SaveOutcome, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	Subsystem->ReturnSequence(LiveSeq);

	FSuperSLMSequence RestoredSeq;
	FString RestoreError;
	const ESuperSLMRestoreResult RestoreResult = RestoreAndDrain(*this, *Subsystem, Blob, Base, RestoredSeq, RestoreError);
	if (!TestEqual(*FString::Printf(TEXT("restore with the adapter still mapped must succeed: %s"), *RestoreError),
			(uint8)RestoreResult, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the restored sequence's adapter must be re-bound"), Subsystem->GetActiveAdapter(RestoredSeq).IsValid());

	DrainTicks(*Subsystem, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[Subsystem, &RestoredSeq]()
		{
			const ESuperSLMSequencePhase Phase = Subsystem->GetPhase(RestoredSeq);
			return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		},
		/*MaxWallClockSeconds*/ 60.0);
	TArray<int32> ContinuedTokens = Subsystem->GetGeneratedTokens(RestoredSeq);
	Subsystem->ReturnSequence(RestoredSeq);
	// A returned restore holder releases its op pin only after the queued recycle has
	// drained. Release below is the R-S1c pin-release oracle (M-3).
	for (int32 I = 0; I < 40; ++I)
	{
		Subsystem->Tick(1.0f / 60.0f);
		FPlatformProcess::Sleep(0.005f);
	}
	// FEAT oracle: saved before any decode, so the full continuation must equal the full
	// unsaved reference exactly -- a restore that drops the adapter binding decodes under the
	// base model instead and diverges from the first generated token.
	bOk &= TestEqual(TEXT("continuation under the re-bound adapter must equal the unsaved reference run"), ContinuedTokens, ReferenceTokens);

	// --- The SAME blob, restored after the adapter is unmapped: must refuse AdapterUnavailable ---
	FString ReleaseError;
	const bool bReleaseOk = FSuperSLMAdapterImport::Release(Adapter, ReleaseError);
	if (!TestTrue(*FString::Printf(TEXT("adapter Release(): %s"), *ReleaseError), bReleaseOk))
	{
		return false;
	}

	FSuperSLMSequence RestoredAfterUnmap;
	FString UnavailableError;
	const ESuperSLMRestoreResult UnavailableResult = RestoreAndDrain(*this, *Subsystem, Blob, Base, RestoredAfterUnmap, UnavailableError);
	// L22 repair (T-2805, D-SLM7544): "AdapterUnavailable by name" is satisfied by this typed
	// result reaching the caller through GetLifecycleOpResult, the channel R-S1c's own closing
	// clause requires. UnavailableError is the synchronous out-param that clause rules out, and a
	// non-empty string would not by itself establish that it names the adapter; not asserted.
	bOk &= TestEqual(TEXT("restore with the adapter unmapped must refuse AdapterUnavailable"),
		(uint8)UnavailableResult, (uint8)ESuperSLMRestoreResult::AdapterUnavailable);

	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
