// Precache plan rev 1.18.7, §5.8 *The build runs
// in the background* (PC-15 Q9; the U3 draft's W5; rev 1.18.1 plan review F6, audit G7, N4): cell 3.11 [U3].
// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on. Written red
// first: at 0a188c159 the editor action built on the game thread through SuperSLMPrefixSetTools::BuildCpu, and
// nothing here existed (FSuperSLMPrefixSetBuildAction, the job's phases, its progress and cancel, the game-thread
// build-call counter).
//
//   SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuild               cell 3.11                                [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuildEditRefused    3.11: a set edited while its build runs
//                                                                 stores nothing (review S1)               [U3]
//   SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuildShutdown       3.11: CancelAll on a paused build cancels
//                                                                 and finishes it (review S1)              [U3]
//
// The cell, on a set of 8 entries built once (the previous build's bytes, P), every entry then edited:
//   1. the action returns to the game thread before any prefill runs: the game-thread counter of prefill, freeze
//      and save calls reads 0 after Start() and through the whole action;
//   2. a cancel after 3 entries (the job's test pause holds the worker after its third entry): while paused the
//      set's stored bytes are still P (the set is never replaced entry by entry), and after the cancel they are
//      byte-equal to P;
//   3. a full build with one entry failing in its prefill (an unknown schema) and a GPU build present: the
//      entries-done count reaches 8; the failing entry keeps its own bytes from P and is reported by name; the other
//      seven are the new build's, all stored at the end; the GPU build is untouched and reported GpuNotBuilt (not
//      built), never as a failure;
//   4. the commandlet's output (SuperSLMPrefixSetTools::BuildCpu, which the commandlet runs) is byte-equal to the
//      action's, and BuildCpu on the game thread moves the counter (the counter's must-reject).
//
// Mutants: the action's prefill on the game thread -> the counter reads > 0 -> red; the set replaced entry by entry
// -> the paused or cancel leg's bytes differ from P -> red; a failing entry that clears its own bytes, or blocks the
// others' store -> red; a GPU-empty entry counted as failed -> red on the report; a Finish that skips the edit
// fingerprint -> EditRefused red; CancelAll that leaves an action running or registered -> Shutdown red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace SuperSLMPrefixSetBackgroundBuildTests
{
	constexpr int32 kEntries = 8;
	constexpr int32 kLength = 64;
	constexpr double kActionSeconds = 900.0;

	bool BackgroundRig(FAutomationTestBase& T, FPagingRig& Rig)
	{
		return Rig.Init(T) && ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) && ReadGeometry(T, Rig);
	}

	USuperSLMPrefixSet* NewBackgroundSet(USuperSLMModel* Model)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		return Set;
	}

	const FSuperSLMPrefixSetBuild* CpuBuildOf(const USuperSLMPrefixSet& Set)
	{
		return Set.Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& B) { return B.Backend == 0; });
	}

	// Every CPU entry's bytes, by entry order.
	TArray<TArray<uint8>> CpuBytesOf(const USuperSLMPrefixSet& Set)
	{
		TArray<TArray<uint8>> Out;
		const FSuperSLMPrefixSetBuild* B = CpuBuildOf(Set);
		for (const FSuperSLMPrefixSetEntry& E : Set.Entries)
		{
			const FSuperSLMPrefixSetBuiltEntry* Built = B != nullptr ? B->Entries.FindByPredicate([&E](const FSuperSLMPrefixSetBuiltEntry& X) { return X.Name == E.Name; }) : nullptr;
			Out.Add(Built != nullptr ? Built->Wrapper : TArray<uint8>());
		}
		return Out;
	}

	int32 CountNamed(const TArray<FSuperSLMPrefixSetFinding>& Findings, const TCHAR* Finding, FName Entry)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			N += F.Finding == FName(Finding) && F.Entry == Entry ? 1 : 0;
		}
		return N;
	}

	// Five entries built once (the previous build's bytes, OutP): four Tokens entries and one Text entry (the last),
	// then every entry edited (new ids, new text), so a build that stored anything would differ from OutP.
	bool BackgroundEditedSet(FAutomationTestBase& T, FPagingRig& Rig, USuperSLMPrefixSet*& OutSet, TArray<TArray<uint8>>& OutP,
		TArray<int32>& OutTextIds, FString& OutTextHash)
	{
		USuperSLMPrefixSet* Set = NewBackgroundSet(Rig.Model);
		for (int32 I = 0; I < 4; ++I)
		{
			FSuperSLMPrefixSetEntry E;
			E.Name = FName(*FString::Printf(TEXT("tokens%d"), I));
			if (!MakeTokens(T, *Rig.Cpu, kLength, 40 + I, E.Tokens))
			{
				return false;
			}
			Set->Entries.Add(MoveTemp(E));
		}
		FSuperSLMPrefixSetEntry Text;
		Text.Name = FName(TEXT("text"));
		Text.Input = ESuperSLMPrefixEntryInput::Text;
		Text.Text = TEXT("The harbour bell rang twice before the ferry left the quay.");
		Set->Entries.Add(MoveTemp(Text));
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("the previous build (%s)"), *Error), SuperSLMPrefixSetTools::BuildCpu(*Set, Findings, Error)))
		{
			return false;
		}
		OutP = CpuBytesOf(*Set);
		OutTextIds = Set->Entries[4].Tokens;
		OutTextHash = Set->Entries[4].TextHash;
		if (!T.TestTrue(TEXT("P: every entry has bytes, the Text entry its ids"), !OutP.ContainsByPredicate([](const TArray<uint8>& W) { return W.Num() == 0; }) && OutTextIds.Num() > 0))
		{
			return false;
		}
		for (int32 I = 0; I < 4; ++I)
		{
			TArray<int32> Tokens;
			if (!MakeTokens(T, *Rig.Cpu, kLength, 60 + I, Tokens))
			{
				return false;
			}
			Set->Entries[I].Tokens = Tokens;
		}
		Set->Entries[4].Text = TEXT("The harbour bell rang three times; the ferry stayed at the quay.");
		OutSet = Set;
		return true;
	}

	// Pumps the action on the game thread (as its ticker does) until Done says so or the time runs out.
	template <typename FDone>
	bool PumpUntil(SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction& Action, FDone Done)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done() && FPlatformTime::Seconds() - Start < kActionSeconds)
		{
			Action.Pump();
			FPlatformProcess::Sleep(0.005f);
		}
		return Done();
	}
}

using namespace SuperSLMPrefixSetBackgroundBuildTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetBackgroundBuildTest, "SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetBackgroundBuildTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPrefixSetEditor;
	FPagingRig Rig;
	if (!BackgroundRig(*this, Rig))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewBackgroundSet(Rig.Model);
	for (int32 I = 0; I < kEntries; ++I)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(*FString::Printf(TEXT("entry%d"), I));
		if (!MakeTokens(*this, *Rig.Cpu, kLength, 1 + I, E.Tokens))
		{
			return false;
		}
		Set->Entries.Add(MoveTemp(E));
	}
	{
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("the previous build (%s)"), *Error), SuperSLMPrefixSetTools::BuildCpu(*Set, Findings, Error)))
		{
			return false;
		}
	}
	const TArray<TArray<uint8>> P = CpuBytesOf(*Set);
	bool bOk = TestTrue(TEXT("P: every entry has bytes"), !P.ContainsByPredicate([](const TArray<uint8>& W) { return W.Num() == 0; }));
	// Every entry edited, so a new build's bytes differ from P.
	for (int32 I = 0; I < kEntries; ++I)
	{
		TArray<int32> Tokens;
		bOk &= MakeTokens(*this, *Rig.Cpu, kLength, 20 + I, Tokens);
		Set->Entries[I].Tokens = Tokens;
	}

	// 1, 2: Start() returns before any prefill; a cancel after 3 entries leaves P.
	SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
	{
		FString Error;
		TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error, /*TestPauseAfterEntries*/ 3);
		if (!TestTrue(*FString::Printf(TEXT("cancel leg: the action starts (%s)"), *Error), Action.IsValid()))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("Start() returned to the game thread before any prefill, freeze or save on it"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
		bOk &= TestEqual(TEXT("the progress counts 8 entries"), Action->GetProgress().EntriesTotal, kEntries);
		bOk &= TestTrue(TEXT("the worker pauses after 3 entries"), PumpUntil(*Action, [&Action]() { return Action->GetProgress().bPaused; }));
		bOk &= TestEqual(TEXT("paused: 3 entries done"), Action->GetProgress().EntriesDone, 3);
		bOk &= TestTrue(TEXT("paused: the stored bytes are still P (never replaced entry by entry)"), CpuBytesOf(*Set) == P);
		Action->Cancel();
		bOk &= TestTrue(TEXT("the cancelled action finishes"), PumpUntil(*Action, [&Action]() { return Action->IsFinished(); }));
		bOk &= TestEqual(TEXT("the action reads Cancelled"), (uint8)Action->GetResult(), (uint8)SuperSLMPrefixSetTools::EBuildFinish::Cancelled);
		bOk &= TestTrue(TEXT("cancelled: the stored bytes are byte-equal to P"), CpuBytesOf(*Set) == P);
		bOk &= TestEqual(TEXT("cancel leg: no prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
	}

	// 3: one entry fails in its prefill (an unknown schema); a GPU build is present.
	const int32 Failing = 5;
	Set->Entries[Failing].SchemaName = FName(TEXT("no_such_schema_3_11"));
	FSuperSLMPrefixSetBuild Gpu = *CpuBuildOf(*Set);
	Gpu.Backend = 1;
	Gpu.AdapterVendorId = 0x1002;
	Gpu.AdapterDeviceId = 0x744C;
	Set->Builds.Add(Gpu);
	{
		FString Error;
		TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error);
		if (!TestTrue(*FString::Printf(TEXT("full leg: the action starts (%s)"), *Error), Action.IsValid()))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("the full action finishes"), PumpUntil(*Action, [&Action]() { return Action->IsFinished(); }));
		bOk &= TestEqual(TEXT("entries done reaches 8"), Action->GetProgress().EntriesDone, kEntries);
		bOk &= TestEqual(TEXT("the action reads Stored"), (uint8)Action->GetResult(), (uint8)SuperSLMPrefixSetTools::EBuildFinish::Stored);
		bOk &= TestEqual(TEXT("full leg: no prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
		const TArray<TArray<uint8>> After = CpuBytesOf(*Set);
		const TArray<FSuperSLMPrefixSetFinding>& Findings = Action->GetFindings();
		const FName FailingName = Set->Entries[Failing].Name;
		bOk &= TestEqual(TEXT("the failing entry is reported by name (UnknownSchema)"), CountNamed(Findings, TEXT("UnknownSchema"), FailingName), 1);
		bOk &= TestTrue(TEXT("the failing entry keeps its own previous bytes"), After[Failing] == P[Failing]);
		for (int32 I = 0; I < kEntries; ++I)
		{
			if (I != Failing)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("entry%d: the new build's bytes, stored"), I), After[I].Num() > 0 && After[I] != P[I]);
			}
		}
		const FSuperSLMPrefixSetBuild* GpuAfter = Set->Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& B) { return B.Backend == 1; });
		bOk &= TestTrue(TEXT("the GPU build is kept as it was"), GpuAfter != nullptr && GpuAfter->Entries.Num() == Gpu.Entries.Num()
			&& GpuAfter->Entries[Failing].Wrapper == Gpu.Entries[Failing].Wrapper && GpuAfter->Entries[0].Wrapper == Gpu.Entries[0].Wrapper);
		bOk &= TestEqual(TEXT("the GPU entries are reported not built (GpuNotBuilt)"), CountNamed(Findings, TEXT("GpuNotBuilt"), NAME_None), 1);
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			const bool bFailure = F.Finding == FName(TEXT("BuildFailed")) || F.Finding == FName(TEXT("UnknownSchema")) || F.Finding == FName(TEXT("TokenizeFailed"));
			bOk &= TestTrue(*FString::Printf(TEXT("only the failing CPU entry is a failure (%s)"), *F.Line), !bFailure || (F.Entry == FailingName && F.Backend == 0));
		}
	}

	// 4: the commandlet's output equals the action's; BuildCpu on the game thread moves the counter.
	{
		USuperSLMPrefixSet* Twin = NewBackgroundSet(Rig.Model);
		Twin->Entries = Set->Entries;
		Twin->Builds = {*CpuBuildOf(*Set)};
		Twin->Builds[0].Entries[Failing].Wrapper = P[Failing]; // the same previous bytes for the failing entry
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
		bOk &= TestTrue(*FString::Printf(TEXT("the commandlet's build (%s)"), *Error), SuperSLMPrefixSetTools::BuildCpu(*Twin, Findings, Error));
		bOk &= TestTrue(TEXT("must-reject: BuildCpu on the game thread counts"), SuperSLMPrefixSetTools::GameThreadBuildCalls() > 0);
		bOk &= TestTrue(TEXT("the commandlet's bytes are byte-equal to the action's"), CpuBytesOf(*Twin) == CpuBytesOf(*Set));
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

// Cell 3.11, the mid-build edit (review S1): a set edited after Start() and before the action finishes stores nothing.
// Finish runs only in Pump(), on the game thread, so an edit made on the game thread right after Start() is before the
// finish whatever the worker has done by then. Expected: Refused, the stored bytes byte-equal to P, the Text entry's
// ids and text hash still P's (the tokenize pass's new ids are not written), no prefill on the game thread.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetBackgroundBuildEditTest, "SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuildEditRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetBackgroundBuildEditTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPrefixSetEditor;
	FPagingRig Rig;
	USuperSLMPrefixSet* Set = nullptr;
	TArray<TArray<uint8>> P;
	TArray<int32> TextIds;
	FString TextHash;
	TArray<int32> Edited;
	if (!BackgroundRig(*this, Rig) || !BackgroundEditedSet(*this, Rig, Set, P, TextIds, TextHash) || !MakeTokens(*this, *Rig.Cpu, kLength, 80, Edited))
	{
		return false;
	}
	AddExpectedError(TEXT("was edited while the build ran"), EAutomationExpectedErrorFlags::Contains, 1);
	SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
	FString Error;
	TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error);
	if (!TestTrue(*FString::Printf(TEXT("the action starts (%s)"), *Error), Action.IsValid()))
	{
		return false;
	}
	Set->Entries[0].Tokens = Edited; // the mid-build edit
	bool bOk = TestTrue(TEXT("the action finishes"), PumpUntil(*Action, [&Action]() { return Action->IsFinished(); }));
	bOk &= TestEqual(TEXT("the action reads Refused"), (uint8)Action->GetResult(), (uint8)SuperSLMPrefixSetTools::EBuildFinish::Refused);
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal names the edit (%s)"), *Action->GetError()), Action->GetError().Contains(TEXT("edited while the build ran")));
	bOk &= TestTrue(TEXT("refused: the stored bytes are byte-equal to P"), CpuBytesOf(*Set) == P);
	bOk &= TestTrue(TEXT("refused: the Text entry keeps P's ids"), Set->Entries[4].Tokens == TextIds);
	bOk &= TestEqual(TEXT("refused: the Text entry keeps P's text hash"), Set->Entries[4].TextHash, TextHash);
	bOk &= TestTrue(TEXT("refused: the edit itself stands"), Set->Entries[0].Tokens == Edited);
	bOk &= TestEqual(TEXT("no prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
	return bOk;
}

// Cell 3.11, module shutdown (review S1): FSuperSLMPrefixSetBuildAction::CancelAll() on a build held by the test pause,
// with no explicit Cancel(), cancels it, waits for its worker and finishes it: Cancelled, the stored bytes byte-equal
// to P, and the registry empty (a second Start() on the same set, refused while the first runs, is accepted after).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixSetBackgroundBuildShutdownTest, "SuperSLM.PrefixCache.Cpu.Cooked.BackgroundBuildShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixSetBackgroundBuildShutdownTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPrefixSetEditor;
	FPagingRig Rig;
	USuperSLMPrefixSet* Set = nullptr;
	TArray<TArray<uint8>> P;
	TArray<int32> TextIds;
	FString TextHash;
	if (!BackgroundRig(*this, Rig) || !BackgroundEditedSet(*this, Rig, Set, P, TextIds, TextHash))
	{
		return false;
	}
	AddExpectedError(TEXT("a build of this set is already running"), EAutomationExpectedErrorFlags::Contains, 1);
	SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
	FString Error;
	TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error, /*TestPauseAfterEntries*/ 2);
	if (!TestTrue(*FString::Printf(TEXT("the action starts (%s)"), *Error), Action.IsValid()))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the worker pauses after 2 entries"), PumpUntil(*Action, [&Action]() { return Action->GetProgress().bPaused; }));
	{
		FString Second;
		bOk &= TestFalse(TEXT("while it runs: a second Start() on the set is refused"), FSuperSLMPrefixSetBuildAction::Start(*Set, Second).IsValid());
	}
	FSuperSLMPrefixSetBuildAction::CancelAll();
	bOk &= TestTrue(TEXT("CancelAll: the action is finished, with no Pump()"), Action->IsFinished());
	bOk &= TestEqual(TEXT("CancelAll: the action reads Cancelled"), (uint8)Action->GetResult(), (uint8)SuperSLMPrefixSetTools::EBuildFinish::Cancelled);
	bOk &= TestTrue(TEXT("CancelAll: the stored bytes are byte-equal to P"), CpuBytesOf(*Set) == P);
	bOk &= TestTrue(TEXT("CancelAll: the Text entry keeps P's ids"), Set->Entries[4].Tokens == TextIds);
	bOk &= TestEqual(TEXT("CancelAll: no prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
	{
		FString Again;
		TSharedPtr<FSuperSLMPrefixSetBuildAction> Next = FSuperSLMPrefixSetBuildAction::Start(*Set, Again);
		if (TestTrue(*FString::Printf(TEXT("the registry is empty: a second Start() on the set is accepted (%s)"), *Again), Next.IsValid()))
		{
			Next->Cancel();
			bOk &= TestTrue(TEXT("the second action finishes"), PumpUntil(*Next, [&Next]() { return Next->IsFinished(); }));
			bOk &= TestTrue(TEXT("the second action, cancelled, leaves P"), CpuBytesOf(*Set) == P);
		}
		else
		{
			bOk = false;
		}
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
