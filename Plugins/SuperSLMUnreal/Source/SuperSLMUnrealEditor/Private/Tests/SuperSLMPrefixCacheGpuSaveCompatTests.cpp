// Precache plan cell 9.11 [U2 box leg] (§5.11 (c), PC-13; rev 1.18.1, PC-15 Q8, audit G14 (a)): the GPU half of the second
// save boundary, written at U6 (plugin 1.4.0) to close coverage gap 1. The CPU half
// is SuperSLMPrefixCacheSaveCompatTests.cpp, whose header assigns the 'SLM5' (dedicated) and 'SLM6' (pooled) cases to this
// box leg. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on, and off Win64 (SUPERSLMUNREAL_WITH_GPU).
//
//   SuperSLM.PrefixCache.Gpu.SaveCompat.Recorded1p13p1  9.11, the paging release's own GPU saves         [U6]
//   SuperSLM.PrefixCache.Gpu.SaveCompat.Crafted         9.11, every boundary case, 'SLM5' and 'SLM6'     [U6]
//
// On the shipped engine. This build vendors engine v1.18.1 (ThirdParty/SuperSLM/VENDORED_VERSION.txt). The table's one
// row (SuperSLMSaveCompatCore.cpp: compiled v1.14.0 accepts v1.13.1 @ d697927 around 'SSB5', 'SSB6', 'SLM5', 'SLM6')
// does not apply, so every save from another engine release is refused Layer1Mismatch with nothing drawn, as in the CPU
// file. The row's v1.14.0 acceptance is the host twin's (ci/host/prefix_cache/test_save_compat.cpp).
//
// What differs from the CPU file:
//   - A GPU Layer1Mismatch is a refusal at the call (SuperSLMGpuSubsystem.h, RequestRestoreSequence): no handle is
//     minted and GetLastLifecycleRequestError() begins "Layer1Mismatch:". "Nothing drawn" is read as: no handle, no
//     Restore job posted (FSuperSLMPagedGpuTestSeam::JobsPosted, which counts in both modes), the free slot count
//     unchanged, and in pooled mode the ledger's total charge and restore counts unchanged, with the fill probe's free
//     count (§3.9) read before and after each mode's battery equal.
//   - Two modes: dedicated ('SLM5', SequenceBudget 0) and pooled ('SLM6', SequenceBudget 512, with the GPU prefix cache
//     configured on region C as SuperSLMPrefixCacheGpuTests.cpp's cells configure it).
//   - The own-pin saves are taken mid-turn (as SuperSLM.Paging.Gpu.Armed.Slm6RoundTrip does): a save of a completed
//     turn cannot be continued ("a generation that continues a completed one is not supported"). The restored sequence
//     finishes the saved turn with no new request, and its tokens equal the unsaved original's remaining ones. Pooled
//     mode adds 9.10's other shape, a save between a cache adopt and the first finish: the restored adopter's first
//     generation equals the unsaved adopter's.
//
// Recorded1p13p1 reads the paging release's (plugin 1.2.0, engine v1.13.1) GPU wrappers from the directory
// SUPERSLM_PRECACHE_1P13P1_SAVES names (the CPU cell's directory): gpu_dedicated_slm5.sluw and gpu_pooled_slm6.sluw,
// each beside <name>.continuation.txt (the comma-separated tokens that build generated after restoring it). The test
// fails by name when the variable, the directory or a case is missing. On v1.18.0 each is refused Layer1Mismatch with
// nothing drawn; its payload magic is checked ('SLM5', 'SLM6') and its continuation file must be nonempty, the record of
// what the paging release generated.
//
// Crafted re-tags a save this build makes (FSuperSLMPrefixCacheTestSeam::SetWrapperLayer1) and re-magics its Layer-1
// payload (FSuperSLMPagedTestSeam::EditPayload, which skips the GPU's 'SLUG' section), in each mode and for each own save:
//   accepted: this build's own save (the same-pin rule), continuing as the unsaved original;
//   refused Layer1Mismatch, nothing drawn, on v1.18.0: the 1.13.1 tag with commit d697927 (the row is v1.14.0's); the
//              1.14.0 tag; the 1.11.0 tag; the 1.13.0 tag with any commit (the row is dropped: the 1.13.0 release
//              commit, the patched candidate, the tag's peeled commit and d697927 itself); the 1.13.1 tag around 'SSB4',
//              'SSB3', 'SSB2' and 'SPX1'; the 1.13.1 tag with a foreign commit (9e235965, the plan's example).
// Mutants: the entry widened to any tag -> the 1.11.0, 1.13.0 and 1.14.0 cases red; widened to a magic -> that magic's
// case red; the commit comparison dropped (with a v1.18.0 row) -> the foreign-commit case red; the 1.13.0 row kept -> the
// 1.13.0 cases red; the GPU check bypassed (tag not compared) -> every refusal case red; a refusal queued instead of
// made at the call -> the no-handle and no-job readings red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperSLMPrefixCacheTestSeam.h"

namespace
{
	using namespace SuperSLMPagingGpuFixtures;

	const TCHAR* const k1p13p1Tag = TEXT("v1.13.1");
	const TCHAR* const k1p13p1Commit = TEXT("d697927f6d5ce5fa73ecbb8660020a425518b923"); // v1.13.1^{commit}
	const TCHAR* const k1p13p0Tag = TEXT("v1.13.0");
	const TCHAR* const k1p11Tag = TEXT("v1.11.0");
	const TCHAR* const k1p14p0Tag = TEXT("v1.14.0");
	const TCHAR* const k1p14p0Commit = TEXT("52b69b008a1291de0df08eb1036eca55e73e71f0"); // v1.14.0^{commit}
	// The dropped 1.13.0 row's commits (as the CPU file): no 1.13.0-tagged wrapper is accepted, whatever its commit.
	const TCHAR* const k1p13p0Commits[] = {TEXT("a5f3ad1ba5cc5c62d7c97d7248e9389c6072a75a"), TEXT("002094a6da8ba33450e6fabc08503b22e35d1610"),
		TEXT("9e235965fa46baf4a63c52c4b7857405ca4456fe"), TEXT("d697927f6d5ce5fa73ecbb8660020a425518b923")};
	// A commit other than d697927 under the 1.13.1 tag: the plan's example.
	const TCHAR* const kForeignCommit = TEXT("9e235965fa46baf4a63c52c4b7857405ca4456fe");

	// The mid-turn save: a turn of kTurn new tokens, the save requested once kSaveAfter have applied.
	constexpr int32 kTurn = 12;
	constexpr int32 kSaveAfter = 3;
	constexpr int32 kPromptTokens = 8;
	// The adopter save (pooled): a two-page cache prefix, a short suffix, kNew new tokens.
	constexpr int32 kPrefixTokens = 32;
	constexpr int32 kSuffixTokens = 4;
	constexpr int32 kNew = 8;

	bool ConfigureDedicatedMode(FAutomationTestBase& T, FGpuPagingRig& Rig)
	{
		return ConfigureDedicated(T, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 0, 0, 1));
	}

	// Pooled mode (2 slots, SequenceBudget 512, one headroom block) with the GPU prefix cache at one block of region C, as
	// SuperSLMPrefixCacheGpuTests.cpp's TwConfigure sets it (disk off, no pad).
	bool ConfigurePooledMode(FAutomationTestBase& T, FGpuPagingRig& Rig)
	{
		const FSuperSLMGpuRuntimeConfig Config = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 0);
		if (!ConfigurePooled(T, Rig, Config) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		FSuperSLMGpuPrefixCacheSettings Settings;
		Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C
		Settings.bDiskEnabled = false;
		Rig.Gpu->ConfigurePrefixCache(Settings);
		if (!ConfigurePooled(T, Rig, Config))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S = Rig.Gpu->GetPrefixCacheStats();
		return T.TestTrue(TEXT("the GPU cache is configured, on region C"), S.bConfigured && S.bBudgetMode);
	}

	bool ConfigureMode(FAutomationTestBase& T, FGpuPagingRig& Rig, bool bPooled)
	{
		return bPooled ? ConfigurePooledMode(T, Rig) : ConfigureDedicatedMode(T, Rig);
	}

	// The Layer-1 payload's magic (after the GPU section) is Magic.
	bool PayloadMagicIs(FAutomationTestBase& T, const TArray<uint8>& Blob, const char* Magic, const TCHAR* What)
	{
		TArray<uint8> Payload;
		return ReadPayload(T, Blob, Payload)
			&& T.TestTrue(*FString::Printf(TEXT("%s: the Layer-1 payload is '%hs'"), What, Magic), GpuPayloadHeader::MagicIs(Payload, Magic));
	}

	bool TurnEnded(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq)
	{
		const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
		return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Gpu.GetPendingLifecycleOperationCount(Seq) == 0;
	}

	// This build's own save, taken mid-turn: the original begins a kTurn-token turn, is saved once kSaveAfter tokens have
	// applied, and the blob is restored at once. Both finish the turn (the restored one with no new request); the restored
	// sequence's tokens equal the original's remaining ones. The save's k (tokens applied at its finalize) is read from
	// the outcome, as in SuperSLM.Paging.Gpu.Armed.Slm6RoundTrip.
	bool OwnMidTurnSaveContinues(FAutomationTestBase& T, FGpuPagingRig& Rig, const TArray<int32>& Prompt, TArray<uint8>& OutBlob, const TCHAR* Mode)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		FSuperSLMGpuSequence Original, Restored;
		if (!Vend(T, Gpu, Original))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = kTurn;
		const FSuperSLMLifecycleOpHandle Turn = Gpu.RequestBeginGeneration(Original, Request);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the %d-token turn is accepted (%s)"), Mode, kTurn, *Gpu.GetLastLifecycleRequestError()), Turn.IsValid()))
		{
			ReturnAndDrain(Gpu, Original);
			return false;
		}
		Pump(Gpu, [&Gpu, &Original]() { return Gpu.GetGeneratedTokens(Original).Num() >= kSaveAfter || TurnEnded(Gpu, Original); }, GenerateSeconds);
		const int32 AppliedAtRequest = Gpu.GetGeneratedTokens(Original).Num();
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the save is requested mid-turn (%d of %d applied)"), Mode, AppliedAtRequest, kTurn),
			AppliedAtRequest >= kSaveAfter && AppliedAtRequest < kTurn && !TurnEnded(Gpu, Original));
		bOk = bOk && SaveAndTake(T, Gpu, Original, OutBlob);
		if (bOk)
		{
			const ESuperSLMRestoreResult R = RestoreAndWait(T, Gpu, OutBlob, Rig.Model, Restored);
			bOk = T.TestEqual(*FString::Printf(TEXT("%s, this build's own save: accepted (got %s; %s)"), Mode, SuperSLML2S2Fixtures::RestoreResultName(R), *Gpu.GetLastLifecycleRequestError()),
					  (uint8)R, (uint8)ESuperSLMRestoreResult::Success)
				&& T.TestTrue(*FString::Printf(TEXT("%s, this build's own save: a restored sequence"), Mode), Restored.IsValid());
		}
		Pump(Gpu, [&Gpu, &Original, &Restored]() { return TurnEnded(Gpu, Original) && (!Restored.IsValid() || TurnEnded(Gpu, Restored)); }, GenerateSeconds);
		Gpu.ReleaseLifecycleOpHandle(Turn);
		if (bOk)
		{
			const TArray<int32> Whole = Gpu.GetGeneratedTokens(Original);
			const TArray<int32> Rest = Gpu.GetGeneratedTokens(Restored);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the original completes its turn"), Mode), (uint8)Gpu.GetPhase(Original), (uint8)ESuperSLMSequencePhase::Complete);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the restored sequence completes the same turn"), Mode), (uint8)Gpu.GetPhase(Restored), (uint8)ESuperSLMSequencePhase::Complete);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the original emits its %d tokens"), Mode, kTurn), Whole.Num(), kTurn);
			const int32 Saved = kTurn - Rest.Num();
			T.AddInfo(FString::Printf(TEXT("%s: the save was taken with %d of %d tokens applied; the restored sequence emitted the remaining %d"), Mode, Saved, kTurn, Rest.Num()));
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the save was mid-turn (k = %d, %d <= k < %d)"), Mode, Saved, kSaveAfter, kTurn), Saved >= kSaveAfter && Saved < kTurn);
			if (Whole.Num() == kTurn && Saved >= 0 && Saved <= kTurn)
			{
				const TArray<int32> Tail(Whole.GetData() + Saved, kTurn - Saved);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s, this build's own save: the continuation equals the unsaved original's remaining tokens"), Mode), Rest, Tail);
			}
		}
		ReturnAndDrain(Gpu, Original);
		if (Restored.IsValid())
		{
			ReturnAndDrain(Gpu, Restored);
		}
		return bOk;
	}

	// Pooled only: this build's own save of a cache adopter, taken between the adopt and the first finish (9.10's pooled
	// shape; cell 8.8's GPU twin). The restore, hinted with the lease, then generates as the unsaved adopter does.
	bool OwnAdopterSaveContinues(FAutomationTestBase& T, FGpuPagingRig& Rig, TArray<uint8>& OutBlob)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		const TCHAR* const What = TEXT("pooled 'SLM6', a cache adopter saved before its first finish");
		TArray<int32> Tokens, Suffix, Reference, Got;
		if (!MakeTokens(T, *Rig.Model, kPrefixTokens, 911, Tokens) || !MakeTokens(T, *Rig.Model, kSuffixTokens, 912, Suffix))
		{
			return false;
		}
		FSuperSLMPrefixSpec Spec;
		Spec.Tokens = Tokens;
		FSuperSLMPrefix Lease;
		FString Error;
		const bool bAccepted = Gpu.RequestPrefix(Spec, Lease, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: RequestPrefix() accepts the spec (%s)"), What, *Error), bAccepted && Lease.IsValid()))
		{
			return false;
		}
		Pump(Gpu, [&Gpu, &Lease]() { return Gpu.GetPrefixLeaseState(Lease) != ESuperSLMPrefixLeaseState::Pending; }, BuildSeconds);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: the lease resolves Ready"), What), (uint8)Gpu.GetPrefixLeaseState(Lease), (uint8)ESuperSLMPrefixLeaseState::Ready);
		FSuperSLMGpuSequence Twin, Seq, Restored;
		if (bOk)
		{
			bOk = Vend(T, Gpu, Twin) && AdoptAndWait(T, Gpu, Twin, Lease) && GenerateComplete(T, Gpu, Twin, Suffix, kNew, Reference);
			ReturnAndDrain(Gpu, Twin);
		}
		if (bOk)
		{
			bOk = Vend(T, Gpu, Seq) && AdoptAndWait(T, Gpu, Seq, Lease) && SaveAndTake(T, Gpu, Seq, OutBlob);
			ReturnAndDrain(Gpu, Seq);
		}
		if (bOk)
		{
			const ESuperSLMRestoreResult R = RestoreAndWait(T, Gpu, OutBlob, Rig.Model, Restored, Lease);
			bOk = T.TestEqual(*FString::Printf(TEXT("%s: accepted (got %s; %s)"), What, SuperSLML2S2Fixtures::RestoreResultName(R), *Gpu.GetLastLifecycleRequestError()),
				(uint8)R, (uint8)ESuperSLMRestoreResult::Success);
			if (Restored.IsValid())
			{
				bOk = bOk && GenerateComplete(T, Gpu, Restored, Suffix, kNew, Got)
					&& T.TestEqual(*FString::Printf(TEXT("%s: the continuation equals the unsaved adopter's"), What), Got, Reference);
				ReturnAndDrain(Gpu, Restored);
			}
		}
		Gpu.ReturnPrefix(Lease);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the subsystem drains"), What), DrainQuiet(Gpu));
		return bOk;
	}

	// Refused Layer1Mismatch at the call, with nothing drawn (this file's header).
	bool RefusedNothingDrawn(FAutomationTestBase& T, FGpuPagingRig& Rig, const TArray<uint8>& Blob, const TCHAR* What)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the subsystem drains first"), What), DrainQuiet(Gpu)))
		{
			return false;
		}
		const int32 FreeSlotsBefore = Gpu.GetPoolFreeCount();
		const int64 JobsBefore = FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore);
		const FSuperSLMPagedLedgerView Before = LedgerOf(Gpu);
		const FIssuedGpuRestore R = IssueRestore(Gpu, Blob, Rig.Model);
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: refused at the call, no handle minted (%s)"), What, *R.CallError), R.Handle.IsValid());
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the error begins 'Layer1Mismatch:' (got '%s')"), What, *R.CallError),
			R.CallError.StartsWith(TEXT("Layer1Mismatch:"), ESearchCase::CaseSensitive));
		bOk &= TickN(T, Gpu, 4, What);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no Restore job is posted"), What), FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore), JobsBefore);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no slot is taken"), What), Gpu.GetPoolFreeCount(), FreeSlotsBefore);
		const FSuperSLMPagedLedgerView After = LedgerOf(Gpu);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the ledger's total charge is unchanged"), What), After.TotalCharged, Before.TotalCharged);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no restore is in flight or waiting"), What), After.InFlightRestores + After.WaitingRestores, Before.InFlightRestores + Before.WaitingRestores);
		if (R.Handle.IsValid())
		{
			// A wrong build minted one: settle it so it cannot leak into the next case, and say what it read.
			FSuperSLMGpuSequence Seq;
			const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, R, Seq, 10.0);
			T.AddError(FString::Printf(TEXT("%s: the handle resolved %s"), What, SuperSLML2S2Fixtures::RestoreResultName(Outcome)));
			Discard(Gpu, R);
		}
		return bOk;
	}

	// Pooled mode: the fill probe's free count (§3.9), the one-state form. Dedicated mode has no pool to probe.
	bool ProbeFreeIfPooled(FAutomationTestBase& T, FGpuPagingRig& Rig, bool bPooled, int64& OutFree)
	{
		OutFree = -1;
		return !bPooled || (DrainQuiet(*Rig.Gpu) && ProbeFree(T, *Rig.Gpu, LedgerOf(*Rig.Gpu).PoolPages, OutFree));
	}

	bool Retag(FAutomationTestBase& T, const TArray<uint8>& In, const TCHAR* Tag, const TCHAR* Commit, const char* Magic, TArray<uint8>& Out)
	{
		Out = In;
		bool bOk = T.TestTrue(TEXT("the wrapper's Layer-1 fields are rewritten"), FSuperSLMPrefixCacheTestSeam::SetWrapperLayer1(Out, Tag, Commit));
		if (Magic)
		{
			bOk &= T.TestTrue(TEXT("the Layer-1 payload's magic is rewritten"),
				FSuperSLMPagedTestSeam::EditPayload(Out, [Magic](TArray<uint8>& P) { if (P.Num() >= 4) { FMemory::Memcpy(P.GetData(), Magic, 4); } }));
		}
		return bOk;
	}

	// Every crafted boundary case of 9.11 on one of this build's own saves (Own), in the configured mode.
	bool RefusalBattery(FAutomationTestBase& T, FGpuPagingRig& Rig, const TArray<uint8>& Own, bool bPooled, const TCHAR* Shape)
	{
		int64 FreeBefore = -1;
		if (!ProbeFreeIfPooled(T, Rig, bPooled, FreeBefore))
		{
			return false;
		}
		TArray<uint8> Blob;
		// On v1.18.0 the v1.14.0 row does not apply: the 1.13.1 tag with commit d697927 is refused, and so is 1.14.0.
		bool bOk = Retag(T, Own, k1p13p1Tag, k1p13p1Commit, nullptr, Blob)
			&& RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 @ d697927 on v1.18.0"), Shape));
		bOk &= Retag(T, Own, k1p14p0Tag, k1p14p0Commit, nullptr, Blob)
			&& RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.14.0 tag on v1.18.0"), Shape));
		// The 1.11.0 tag keeps its boundary.
		bOk &= Retag(T, Own, k1p11Tag, k1p13p1Commit, nullptr, Blob) && RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.11.0 tag"), Shape));
		// The 1.13.0 row is dropped: a 1.13.0-tagged wrapper is refused whatever its commit.
		for (const TCHAR* Commit : k1p13p0Commits)
		{
			bOk &= Retag(T, Own, k1p13p0Tag, Commit, nullptr, Blob)
				&& RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.0 tag @ %.7s (row dropped)"), Shape, Commit));
		}
		// Magics outside the four keep theirs.
		for (const char* Magic : {"SSB4", "SSB3", "SSB2", "SPX1"})
		{
			bOk &= Retag(T, Own, k1p13p1Tag, k1p13p1Commit, Magic, Blob)
				&& RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 tag around '%hs'"), Shape, Magic));
		}
		// The 1.13.1 tag with a foreign commit (the wrapper records a commit).
		bOk &= Retag(T, Own, k1p13p1Tag, kForeignCommit, nullptr, Blob)
			&& RefusedNothingDrawn(T, Rig, Blob, *FString::Printf(TEXT("%s, 1.13.1 tag, foreign commit 9e23596"), Shape));
		if (bPooled)
		{
			int64 FreeAfter = -1;
			bOk &= ProbeFreeIfPooled(T, Rig, bPooled, FreeAfter)
				&& T.TestEqual(*FString::Printf(TEXT("%s: the fill probe's free count is unchanged across the refusals"), Shape), FreeAfter, FreeBefore);
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuSaveCompatRecordedTest, "SuperSLM.PrefixCache.Gpu.SaveCompat.Recorded1p13p1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuSaveCompatRecordedTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PRECACHE_1P13P1_SAVES"));
	if (!TestTrue(*FString::Printf(TEXT("the paging release's (1.13.1) saves directory is set and exists (SUPERSLM_PRECACHE_1P13P1_SAVES = '%s')"), *Dir),
			!Dir.IsEmpty() && IFileManager::Get().DirectoryExists(*Dir)))
	{
		return false;
	}
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	struct FCase { const TCHAR* Name; bool bPooled; const char* Magic; };
	for (const FCase& C : {FCase{TEXT("gpu_dedicated_slm5"), false, "SLM5"}, FCase{TEXT("gpu_pooled_slm6"), true, "SLM6"}})
	{
		TArray<uint8> Blob;
		FString Continuation;
		const FString Base = FPaths::Combine(Dir, C.Name);
		if (!TestTrue(*FString::Printf(TEXT("%s: the recorded wrapper and its continuation are readable (%s.sluw, %s.continuation.txt under SUPERSLM_PRECACHE_1P13P1_SAVES)"), C.Name, C.Name, C.Name),
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
		// On the shipped engine (v1.18.1) no row accepts a 1.13.1 save: refused by name, nothing drawn. The continuation
		// file stays the record of what the paging release generated after it.
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the continuation file is nonempty"), C.Name), Expected.Num() > 0);
		bOk &= PayloadMagicIs(*this, Blob, C.Magic, C.Name);
		if (!ConfigureMode(*this, Rig, C.bPooled))
		{
			bOk = false;
			continue;
		}
		int64 FreeBefore = -1, FreeAfter = -1;
		bOk &= ProbeFreeIfPooled(*this, Rig, C.bPooled, FreeBefore);
		bOk &= RefusedNothingDrawn(*this, Rig, Blob, *FString::Printf(TEXT("%s (1.13.1 save) on v1.18.0"), C.Name));
		bOk &= ProbeFreeIfPooled(*this, Rig, C.bPooled, FreeAfter);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the fill probe's free count is unchanged (-1 in dedicated mode)"), C.Name), FreeAfter, FreeBefore);
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuSaveCompatCraftedTest, "SuperSLM.PrefixCache.Gpu.SaveCompat.Crafted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuSaveCompatCraftedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const bool bPooled : {false, true})
	{
		const TCHAR* Mode = bPooled ? TEXT("pooled 'SLM6'") : TEXT("dedicated 'SLM5'");
		TArray<int32> Prompt;
		TArray<uint8> Own;
		if (!ConfigureMode(*this, Rig, bPooled) || !MakeTokens(*this, *Rig.Model, kPromptTokens, bPooled ? 902 : 901, Prompt))
		{
			return false;
		}
		// The build's own tag restores and continues as the unsaved original (the same-pin rule).
		if (!OwnMidTurnSaveContinues(*this, Rig, Prompt, Own, Mode))
		{
			return false;
		}
		bOk &= PayloadMagicIs(*this, Own, bPooled ? "SLM6" : "SLM5", Mode);
		bOk &= RefusalBattery(*this, Rig, Own, bPooled, *FString::Printf(TEXT("%s, mid-turn save"), Mode));
		if (bPooled)
		{
			TArray<uint8> Adopter;
			if (!OwnAdopterSaveContinues(*this, Rig, Adopter))
			{
				return false;
			}
			bOk &= PayloadMagicIs(*this, Adopter, "SLM6", TEXT("pooled 'SLM6', cache adopter"));
			bOk &= RefusalBattery(*this, Rig, Adopter, bPooled, TEXT("pooled 'SLM6', cache adopter saved before its first finish"));
		}
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU
