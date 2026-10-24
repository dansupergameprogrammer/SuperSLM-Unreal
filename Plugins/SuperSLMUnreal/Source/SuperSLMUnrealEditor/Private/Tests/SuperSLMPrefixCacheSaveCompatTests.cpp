// Rewritten for plan rev 1.18.1 (2026-10-21, suite audit G2). Precache plan cell 9.11 (§5.11 (c), PC-13; PC-15 Q8, audit G14 (a)): the second
// save boundary removed, with the one-row table. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.SaveCompat.Recorded1p13p1  9.11, the paging release's own CPU saves     [U2]
//   SuperSLM.PrefixCache.Cpu.SaveCompat.Crafted         9.11, every boundary case                   [U2]
//
// On the shipped engine. The table's one row applies to a build of engine v1.14.0, which no plugin release vendored;
// plugin 1.3.0 vendors v1.15.0, where no row matches, so every save from another engine release is refused
// Layer1Mismatch with nothing drawn (CHANGELOG 1.3.0: a 1.2.0 save is refused). The row's v1.14.0 acceptance is the
// host twin's (ci/host/prefix_cache/test_save_compat.cpp), which passes the compiled tag.
//
// The paging release is plugin 1.2.0, vendoring engine v1.13.1 (commit d697927; the tag object peels to it).
// Recorded1p13p1 reads wrappers that release wrote, with their continuations, from the directory
// SUPERSLM_PRECACHE_1P13P1_SAVES names (box queue: record them on the paging release's build before U0). Each case
// is <name>.sluw beside <name>.continuation.txt, the comma-separated tokens that build generated after restoring
// it. The CPU names are cpu_legacy_ssb5 and cpu_budget_ssb6. The GPU cases (dedicated 'SLM5', pooled 'SLM6')
// are 9.11's [U2 box leg] on the GPU subsystem and are not written in this file. The test fails by name when the
// directory or a case is missing.
//
// Crafted re-tags a save this build makes (SetWrapperLayer1) and re-magics its payload
// (FSuperSLMPagedTestSeam::EditPayload), so every boundary is exercised without a recorded file:
//   refused Layer1Mismatch, nothing drawn, on v1.15.0: the 1.13.1 tag with commit d697927 (legacy 'SSB5' and
//              budget 'SSB6': the row is v1.14.0's); the 1.14.0 tag; the 1.11.0 tag; the 1.13.0 tag with any commit (the row is dropped:
//              the 1.13.0 release commit, the patched candidate, the tag's peeled commit and d697927 itself);
//              the 1.13.1 tag around 'SSB4', 'SSB3', 'SSB2' and 'SPX1'; the 1.13.1 tag with a foreign commit
//              (9e235965, the plan's example).
// The host twin of every boundary case is ci/host/prefix_cache/test_save_compat.cpp.
// Mutants: the entry removed -> the accepted cases red; widened to any tag -> the 1.11.0 and 1.13.0 cases red;
// widened to a magic -> that magic's case red; the commit comparison dropped -> the foreign-commit case red; the
// 1.13.0 row kept -> the 1.13.0 cases red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "Misc/FileHelper.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	const TCHAR* const k1p13p1Tag = TEXT("v1.13.1");
	const TCHAR* const k1p13p1Commit = TEXT("d697927f6d5ce5fa73ecbb8660020a425518b923"); // v1.13.1^{commit}
	const TCHAR* const k1p13p0Tag = TEXT("v1.13.0");
	const TCHAR* const k1p11Tag = TEXT("v1.11.0");
	const TCHAR* const k1p14p0Tag = TEXT("v1.14.0");
	const TCHAR* const k1p14p0Commit = TEXT("52b69b008a1291de0df08eb1036eca55e73e71f0"); // v1.14.0^{commit}
	// The dropped 1.13.0 row's commits: the 1.13.0 release commit, the patched candidate, the tag's peeled
	// commit, and d697927 itself (no 1.13.0-tagged wrapper is accepted, whatever its commit).
	const TCHAR* const k1p13p0Commits[] = {TEXT("a5f3ad1ba5cc5c62d7c97d7248e9389c6072a75a"), TEXT("002094a6da8ba33450e6fabc08503b22e35d1610"),
		TEXT("9e235965fa46baf4a63c52c4b7857405ca4456fe"), TEXT("d697927f6d5ce5fa73ecbb8660020a425518b923")};
	// A commit other than d697927 under the 1.13.1 tag: the plan's example.
	const TCHAR* const kForeignCommit = TEXT("9e235965fa46baf4a63c52c4b7857405ca4456fe");

	// Box 2026-10-21 (second run): the save was of a Complete sequence (a 4-token generation) and the
	// restored one began a second generation, which one-generation-at-a-time refuses (Crafted failed there).
	// The save is now mid-turn, at kSaveAt of kTotal tokens, and the restored sequence resumes it.
	constexpr int32 kSaveAt = 20;
	constexpr int32 kTotal = 48;

	// A plain sequence saved mid-turn, and the unsaved twin's tokens after the ones the save had: the
	// twin generates kTotal in one generation; the save lands at kSaveAt or a little later (a tick can
	// produce more than one token), so the tail is cut at the saved head's length.
	bool SaveWithTwin(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Prompt, TArray<uint8>& OutBlob, TArray<int32>& OutTwinTail)
	{
		TArray<int32> All, Head;
		FSuperSLMSequence Twin, Saved;
		if (!Vend(T, Cpu, Twin) || !GenerateComplete(T, Cpu, Twin, Prompt, kTotal, All))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Twin);
		if (!Vend(T, Cpu, Saved) || !CacheSaveMidTurn(T, Cpu, Saved, GenerationRequest(Prompt, kTotal), kSaveAt, OutBlob, Head))
		{
			return false;
		}
		if (!T.TestTrue(*FString::Printf(TEXT("the save's head (%d tokens) is a strict prefix of the twin's %d"), Head.Num(), All.Num()),
				Head.Num() < All.Num() && TArray<int32>(All.GetData(), Head.Num()) == Head))
		{
			return false;
		}
		OutTwinTail = TArray<int32>(All.GetData() + Head.Num(), All.Num() - Head.Num());
		return true;
	}

	// Restores Blob, lets the restored sequence resume its saved generation to its end, and compares what it
	// produced after the restore with Expected. Recorded1p13p1 shares this: its recorded saves and
	// continuation files must then be a mid-turn save and the tokens after it (input-gated; not run on the box).
	bool RestoreContinues(FAutomationTestBase& T, FPagingRig& Rig, const TArray<uint8>& Blob, const TArray<int32>& Expected, const TCHAR* What)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		FSuperSLMSequence Seq;
		const ESuperSLMRestoreResult R = RestoreAndWait(T, Cpu, Blob, Rig.Model, Seq);
		if (!T.TestEqual(*FString::Printf(TEXT("%s: accepted (not Layer1Mismatch)"), What), (uint8)R, (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		TArray<int32> Tail;
		const bool bOk = CacheContinueRestored(T, Cpu, Seq, Tail)
			&& T.TestEqual(*FString::Printf(TEXT("%s: the continuation equals the reference"), What), Tail, Expected);
		ReturnAndDrain(Cpu, Seq);
		return bOk;
	}

	bool RefusedNothingDrawn(FAutomationTestBase& T, FPagingRig& Rig, const TArray<uint8>& Blob, const TCHAR* What)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		int64 FreeBefore = -1;
		if (!DrainQuiet(Cpu) || !ProbeFree(T, Cpu, ProbeCeiling(T, Cpu), FreeBefore)) // legacy mode has no ledger bound
		{
			return false;
		}
		const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: Layer1Mismatch"), What), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Layer1Mismatch);
		Discard(Cpu, R);
		bOk &= FreeUnchanged(T, Cpu, FreeBefore, What);
		return bOk;
	}

	bool Retag(FAutomationTestBase& T, const TArray<uint8>& In, const TCHAR* Tag, const TCHAR* Commit, const char* Magic, TArray<uint8>& Out)
	{
		Out = In;
		bool bOk = T.TestTrue(TEXT("the wrapper's Layer-1 fields are rewritten"), FSuperSLMPrefixCacheTestSeam::SetWrapperLayer1(Out, Tag, Commit));
		if (Magic)
		{
			bOk &= T.TestTrue(TEXT("the payload's magic is rewritten"), FSuperSLMPagedTestSeam::EditPayload(Out, [Magic](TArray<uint8>& P) { FMemory::Memcpy(P.GetData(), Magic, 4); }));
		}
		return bOk;
	}

	bool ConfigureMode(FAutomationTestBase& T, FPagingRig& Rig, bool bBudget)
	{
		return bBudget ? ConfigureBudget(T, Rig, MakeConfig(2, 512, 0, 1)) : ConfigureLegacy(T, Rig, MakeConfig(2, 0, 0, 1));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheSaveCompatRecordedTest, "SuperSLM.PrefixCache.Cpu.SaveCompat.Recorded1p13p1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheSaveCompatRecordedTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PRECACHE_1P13P1_SAVES"));
	if (!TestTrue(*FString::Printf(TEXT("the paging release's (1.13.1) saves directory is set and exists (SUPERSLM_PRECACHE_1P13P1_SAVES = '%s')"), *Dir), !Dir.IsEmpty() && IFileManager::Get().DirectoryExists(*Dir)))
	{
		return false;
	}
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	struct FCase { const TCHAR* Name; bool bBudget; };
	for (const FCase& C : { FCase{ TEXT("cpu_legacy_ssb5"), false }, FCase{ TEXT("cpu_budget_ssb6"), true } })
	{
		TArray<uint8> Blob;
		FString Continuation;
		const FString Base = FPaths::Combine(Dir, C.Name);
		if (!TestTrue(*FString::Printf(TEXT("%s: the recorded wrapper and its continuation are readable"), C.Name),
				FFileHelper::LoadFileToArray(Blob, *(Base + TEXT(".sluw"))) && FFileHelper::LoadFileToString(Continuation, *(Base + TEXT(".continuation.txt")))))
		{
			bOk = false;
			continue;
		}
		TArray<FString> Parts;
		Continuation.TrimStartAndEnd().ParseIntoArray(Parts, TEXT(","));
		TArray<int32> Expected;
		for (const FString& P : Parts)
		{
			Expected.Add(FCString::Atoi(*P));
		}
		// On the shipped engine (v1.15.0) no row accepts a 1.13.1 save: refused by name, nothing drawn. The
		// continuation file stays the record of what the paging release generated after it.
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the continuation file is nonempty"), C.Name), Expected.Num() > 0);
		bOk &= ConfigureMode(*this, Rig, C.bBudget) && RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s (1.13.1 save) on v1.15.0"), C.Name));
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheSaveCompatCraftedTest, "SuperSLM.PrefixCache.Cpu.SaveCompat.Crafted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheSaveCompatCraftedTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const bool bBudget : { false, true })
	{
		const TCHAR* Mode = bBudget ? TEXT("budget 'SSB6'") : TEXT("legacy 'SSB5'");
		TArray<int32> Prompt, TwinTail;
		TArray<uint8> Own, Blob;
		if (!ConfigureMode(*this, Rig, bBudget) || !MakeTokens(*this, *Rig.Cpu, 16, 1, Prompt) || !SaveWithTwin(*this, *Rig.Cpu, Prompt, Own, TwinTail))
		{
			return false;
		}
		// The build's own tag restores and continues as the unsaved twin (the same-pin rule).
		bOk &= RestoreContinues(*this, Rig, Own, TwinTail, *FString::Printf(TEXT("%s, this build's own save"), Mode));
		// On v1.15.0 the 1.14.0 row does not apply: the 1.13.1 tag with commit d697927 is refused, and so is 1.14.0.
		bOk &= Retag(*this, Own, k1p13p1Tag, k1p13p1Commit, nullptr, Blob)
			&& RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 @ d697927 on v1.15.0"), Mode));
		bOk &= Retag(*this, Own, k1p14p0Tag, k1p14p0Commit, nullptr, Blob)
			&& RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.14.0 tag on v1.15.0"), Mode));
		// The 1.11.0 tag keeps its boundary.
		bOk &= Retag(*this, Own, k1p11Tag, k1p13p1Commit, nullptr, Blob) && RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.11.0 tag"), Mode));
		// The 1.13.0 row is dropped: a 1.13.0-tagged wrapper is refused whatever its commit.
		for (const TCHAR* Commit : k1p13p0Commits)
		{
			bOk &= Retag(*this, Own, k1p13p0Tag, Commit, nullptr, Blob)
				&& RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.0 tag @ %.7s (row dropped)"), Mode, Commit));
		}
		// Magics outside the four keep theirs.
		for (const char* Magic : { "SSB4", "SSB3", "SSB2", "SPX1" })
		{
			bOk &= Retag(*this, Own, k1p13p1Tag, k1p13p1Commit, Magic, Blob)
				&& RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 tag around '%hs'"), Mode, Magic));
		}
		// The 1.13.1 tag with a foreign commit (the wrapper records a commit).
		bOk &= Retag(*this, Own, k1p13p1Tag, kForeignCommit, nullptr, Blob) && RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 tag, foreign commit 9e23596"), Mode));
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
