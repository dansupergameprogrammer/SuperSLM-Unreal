// The Unreal cells for the U2 code review, round 1, against plan rev 1.18.5. Preprocessed out unless
// SUPERSLM_WITH_PREFIX_CACHE is on. Each was red at 79163cd69 by the reading named in its comment.
//
//   SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.DiskTierOff     findings 1, 2; §5.6, §5.4     [U2]
//   SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.OtherDirectory  findings 1, 2                 [U2]
//   SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.Deinitialize    findings 1, 2                 [U2]
//   SuperSLM.PrefixCache.Cpu.U2Review.SetBeforeFirstConfigure          finding 5; §5.8               [U2]
//   SuperSLM.PrefixCache.Cpu.U2Review.SetUnregisteredByOtherModel      finding 5; §5.8               [U2]
//   SuperSLM.PrefixCache.Cpu.U2Review.DropDeleteFailedCarriesError     finding 6; §5.9 (Windows)     [U2]
//
// These drive the subsystem's own re-Configure() and Deinitialize(), so the host's real path runs:
// ReleaseForPoolDestroy (the IO wait, step 1a's drain, step 2's unposted lane jobs, the handle sweep), then
// AbandonCore() -> Abandon(), then the next Configure(). Never BeginTeardown(), which the CPU host does not call.
// The host twins (ci/host/prefix_cache/test_u2_review.cpp, and 1.7's host-path cell in test_residency_disk.cpp)
// model that path in FSim::HostReleaseForPoolDestroy; the two are kept in step.
//
// A delete waiting on a save is reached without a new seam: the build hold keeps S's build on its lane while a
// paging HoldStart() on a second key's build parks that build at the head of the prefix lane queue, so when S's
// build is released and applied its Save is queued behind the parked job and no lane runs it. A re-Configure()
// puts the parked job back and step 2 fails every unposted job, the Save among them. The disk read fault
// (FailRead) recomputes S over a file that already exists, as the review's input (1) does; the cell rewrites
// the file before the drop so a file is under the key whatever the read's fallback did with it.
//
// Findings 3 (GPU, U6-graded), 4 and 7 are host-suite cells only: 4 needs a build-failure seam the plugin does
// not have and its Blueprint mirror is the implementation's patch; 7's CPU host already queues the touch on the key's
// disk queue, so an Unreal cell would be green; 3's core half is the host cell
// release_skipped_by_the_submission_thread_stays_pending.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "UObject/Package.h"

using namespace SuperSLMPrefixCacheFixtures;

// A named namespace, not an anonymous one: the residency and disk files define helpers of these shapes, and a
// unity build would put them in one translation unit.
namespace SuperSLMPrefixCacheU2Review
{
	using Seam = FSuperSLMPrefixCacheTestSeam;

	constexpr int32 kLength = 300;

	FSuperSLMPrefixOpResult WaitResolved(USuperSLMSubsystem& Cpu, const FSuperSLMPrefixOp& Op, double Seconds = LeaseSeconds)
	{
		FSuperSLMPrefixOpResult R;
		Drain(Cpu, [&Cpu, &Op, &R]() { Cpu.GetPrefixOpResult(Op, R); return R.Result != ESuperSLMPrefixOpResult::Pending; }, Seconds);
		Cpu.GetPrefixOpResult(Op, R);
		return R;
	}

	// Budget mode, the disk tier on in Dir, two lanes (one for S's held build; the parked job needs none).
	bool DiskOnRig(FAutomationTestBase& T, FPagingRig& Rig, const FString& Dir, FSuperSLMRuntimeConfig& OutConfig)
	{
		if (!Rig.Init(T) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(T, Rig))
		{
			return false;
		}
		OutConfig = WithCache(MakeConfig(2, 512, 0, 1), BytesForPages(Rig.Geo, 64), Dir, int64(1) << 30);
		return ConfigureCacheBudget(T, Rig, OutConfig);
	}

	// The cache's file at Path, read or opened once its pool-thread disk IO is quiet, the open retried (ticking)
	// for up to Seconds. Box 2026-10-21 (second run): DeleteBehindSave.Deinitialize's read-back and
	// DropDeleteFailedCarriesError's open failed straight after the file appeared. The cells waited on the file
	// existing and then on DrainQuiet, which covers lane jobs only: the write-back's rename is followed on the
	// pool thread by the touch (SetTimeStamp) and the trim, and the open raced them (or a scanner opening the new
	// file). The failure message carries the platform's last error, so a third failure names the holder's kind,
	// and every retry is logged with its count (code review r2, N4: a transient lock stays visible on a pass).
	void LogRetries(const TCHAR* What, const FString& Path, int32 Retries, uint32 LastError, bool bDone)
	{
		if (Retries > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("U2Review: %s of %s %s after %d retries (last error %u)"), What, *Path, bDone ? TEXT("succeeded") : TEXT("failed"), Retries, LastError);
		}
	}

	bool ReadBack(USuperSLMSubsystem& Cpu, const FString& Path, TArray<uint8>& Out, uint32& OutLastError, double Seconds = 10.0)
	{
		WaitDiskQuiet(Cpu);
		const double Start = FPlatformTime::Seconds();
		int32 Retries = 0;
		do
		{
			if (FFileHelper::LoadFileToArray(Out, *Path, FILEREAD_Silent) && Out.Num() > 0)
			{
				LogRetries(TEXT("the read"), Path, Retries, OutLastError, true);
				return true;
			}
			++Retries;
			OutLastError = FPlatformMisc::GetLastError();
			Cpu.Tick(Step);
			FPlatformProcess::Sleep(0.01f);
		} while (FPlatformTime::Seconds() - Start < Seconds);
		LogRetries(TEXT("the read"), Path, Retries, OutLastError, false);
		return false;
	}

	IFileHandle* OpenForRead(USuperSLMSubsystem& Cpu, const FString& Path, uint32& OutLastError, double Seconds = 10.0)
	{
		WaitDiskQuiet(Cpu);
		const double Start = FPlatformTime::Seconds();
		int32 Retries = 0;
		do
		{
			if (IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path))
			{
				LogRetries(TEXT("the open"), Path, Retries, OutLastError, true);
				return Handle;
			}
			++Retries;
			OutLastError = FPlatformMisc::GetLastError();
			Cpu.Tick(Step);
			FPlatformProcess::Sleep(0.01f);
		} while (FPlatformTime::Seconds() - Start < Seconds);
		LogRetries(TEXT("the open"), Path, Retries, OutLastError, false);
		return nullptr;
	}

	// S's file on disk and S not in memory: built, its write-back landed, then a re-Configure() of the same
	// configuration (memory dropped, the file kept). OutBytes: the file's bytes, to put back later.
	bool FileNotResident(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMPrefixSpec& S, FString& OutPath, TArray<uint8>& OutBytes)
	{
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		FSuperSLMPrefix L;
		if (!RequestReady(T, Cpu, S, L, TEXT("the first build")))
		{
			return false;
		}
		Cpu.ReturnPrefix(L);
		OutPath = DiskFileOf(Cpu, S);
		Drain(Cpu, [&OutPath]() { return !OutPath.IsEmpty() && IFileManager::Get().FileExists(*OutPath); }, LeaseSeconds);
		DrainQuiet(Cpu);
		if (!T.TestTrue(*FString::Printf(TEXT("S's write-back landed (%s)"), *OutPath), IFileManager::Get().FileExists(*OutPath)))
		{
			return false;
		}
		uint32 LastError = 0;
		const bool bRead = ReadBack(Cpu, OutPath, OutBytes, LastError);
		return T.TestTrue(*FString::Printf(TEXT("its bytes read back (last error %u)"), LastError), bRead)
			&& ConfigureCacheBudget(T, Rig, Rig.Config);
	}

	enum class EHostEnd : uint8
	{
		DiskTierOff,    // re-Configure() with bPrefixDiskCacheEnabled = false
		OtherDirectory, // re-Configure() with another PrefixDiskCacheDirectory
		Deinitialize,   // the world torn down: no next Configure()
	};

	// Findings 1 and 2: a MemoryAndDisk drop whose delete waits on a write-back's save that no lane has run, a
	// file already under the key, then the host's end. The delete is drained under the old configuration, before
	// the end returns: the old directory's file is gone, the op reads DropDeferred with Disk == Deleted, and the
	// new directory gains nothing under S's name. Red at 79163cd69: Abandon() carries the delete into the next
	// Configure(), which issues it under the new configuration (disk tier off: DiskTierOff, the file left; another
	// directory: NoFile there, the file left), and at Deinitialize() it never runs (the file left).
	bool DeleteBehindSave(FAutomationTestBase& T, EHostEnd End, const TCHAR* Cell)
	{
		FScopedCacheDirectory Dir(Cell);
		FScopedCacheDirectory Other(*FString::Printf(TEXT("%s-other"), Cell));
		FPagingRig Rig;
		FSuperSLMRuntimeConfig C;
		if (!DiskOnRig(T, Rig, Dir.Path, C))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		TArray<int32> Tokens, Blocker;
		if (!MakeTokens(T, Cpu, kLength, 1, Tokens) || !MakeTokens(T, Cpu, kLength, 2, Blocker))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = Spec(Tokens);
		FString Path;
		TArray<uint8> Bytes;
		if (!FileNotResident(T, Rig, S, Path, Bytes))
		{
			return false;
		}

		// (1) S's read fails with an IO error over the existing file: S is recomputed, its build held on its lane.
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::FailRead);
		Seam::SetBuildHold(Cpu, true);
		FSuperSLMPrefix LeaseS;
		if (!Request(T, Cpu, S, LeaseS, TEXT("S, recomputed")))
		{
			Seam::SetBuildHold(Cpu, false);
			return false;
		}
		Drain(Cpu, [&Cpu]() { return Seam::HeldBuildCount(Cpu) == 1; }, LeaseSeconds);
		bool bOk = T.TestEqual(TEXT("S's build is held on its lane (build hold)"), Seam::HeldBuildCount(Cpu), 1);
		// A second key's build parks at the head of the prefix lane queue: nothing queued behind it is posted.
		const FSuperSLMPagedHold Park = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::PrefixBuild);
		FSuperSLMPrefix LeaseBlocker;
		bOk &= Request(T, Cpu, Spec(Blocker), LeaseBlocker, TEXT("the parked build"));
		bOk &= T.TestTrue(TEXT("the second build parks at its start hold"), WaitHoldReached(Cpu, Park));
		// S's build finishes and is applied: S is Ready, and its write-back's Save is queued behind the parked job.
		Seam::SetBuildHold(Cpu, false);
		bOk &= ExpectLease(T, Cpu, LeaseS, ESuperSLMPrefixLeaseState::Ready, TEXT("S"));
		Seam::SetDiskFault(Cpu, ESuperSLMPrefixDiskFault::None);
		// A file under S's key whatever the read's fallback did with it (the review's second PIE instance).
		bOk &= T.TestTrue(TEXT("a file is under S's key"), FFileHelper::SaveArrayToFile(Bytes, *Path));
		const int32 FilesInOther = Other.Files().Num();

		// (2) The drop: S is leased, so DropDeferred, and its delete waits on the queued save.
		FSuperSLMPrefixOp Op;
		FString Error;
		bOk &= T.TestTrue(*FString::Printf(TEXT("DropPrefix(S, MemoryAndDisk) is accepted (%s)"), *Error), Cpu.DropPrefix(S, ESuperSLMPrefixDropScope::MemoryAndDisk, Op, Error));
		Cpu.ReturnPrefix(LeaseS);
		for (int32 I = 0; I < 8; ++I)
		{
			Cpu.Tick(Step);
		}
		FSuperSLMPrefixOpResult Before;
		Cpu.GetPrefixOpResult(Op, Before);
		bOk &= T.TestEqual(TEXT("the drop is Pending: its delete waits on the save no lane has run"), (uint8)Before.Result, (uint8)ESuperSLMPrefixOpResult::Pending);
		bOk &= T.TestTrue(TEXT("the file is still there before the host's end"), IFileManager::Get().FileExists(*Path));
		if (!bOk)
		{
			return false;
		}

		// (3) The host's end.
		if (End == EHostEnd::Deinitialize)
		{
			Rig.World.DestroyTestWorld(false); // Deinitialize(): ReleaseForPoolDestroy, then Abandon(), no next Configure()
			return T.TestFalse(*FString::Printf(TEXT("Deinitialize(): the delete was drained, S's file is gone (%s)"), *Path), IFileManager::Get().FileExists(*Path));
		}
		const FSuperSLMRuntimeConfig Next = End == EHostEnd::DiskTierOff
			? WithCache(C, C.PrefixCacheBudgetBytes)                                   // the disk tier off
			: WithCache(C, C.PrefixCacheBudgetBytes, Other.Path, C.PrefixDiskCacheMaxBytes); // another directory
		bOk &= ConfigureCacheBudget(T, Rig, Next);
		const TCHAR* Name = End == EHostEnd::DiskTierOff ? TEXT("disk tier off") : TEXT("another directory");
		// Drained before Configure() returned: the file is gone with no tick after it.
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: S's file in the old directory is gone when Configure() returns"), Name), IFileManager::Get().FileExists(*Path));
		const FSuperSLMPrefixOpResult R = WaitResolved(Cpu, Op, OpSeconds);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop reads DropDeferred"), Name), (uint8)R.Result, (uint8)ESuperSLMPrefixOpResult::DropDeferred);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: its disk part reads Deleted (not DiskTierOff, not NoFile)"), Name), (uint8)R.Disk, (uint8)ESuperSLMPrefixDropDisk::Deleted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the new directory gained no file"), Name), Other.Files().Num(), FilesInOther);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: no handle released twice"), Name), Seam::MaxReleasesOfOneHandle(Cpu) <= 1);
		return bOk;
	}

	// Another model's asset: the shipped 1.5B artifact, imported beside the rig's 0.5B one.
	USuperSLMModel* ImportOtherModel(FAutomationTestBase& T)
	{
		FString Path, Reason;
		if (!ResolveArtifact(EPagingArtifact::Shipped15B, Path, Reason))
		{
			T.AddError(Reason);
			return nullptr;
		}
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		return T.TestNotNull(TEXT("the other model imports"), Model) && T.TestTrue(TEXT("the other model is accepted"), Diag.bAccepted) ? Model : nullptr;
	}

	// A set of one entry built against Model, its CPU build present.
	USuperSLMPrefixSet* BuiltSet(FAutomationTestBase& T, USuperSLMModel* Model, const TArray<int32>& Tokens)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(TEXT("entry"));
		E.Tokens = Tokens;
		Set->Entries = {E};
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("the set builds (%s)"), *Error), SuperSLMPrefixSetEditor::BuildAndLog(*Set, Findings, Error))
			|| !T.TestEqual(TEXT("one CPU build"), Set->Builds.Num(), 1))
		{
			return nullptr;
		}
		return Set;
	}

	bool LegacyCacheOn(FAutomationTestBase& T, FPagingRig& Rig)
	{
		return ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) && ReadGeometry(T, Rig)
			&& ConfigureCacheLegacy(T, Rig, WithCache(MakeConfig(2, 0, 0, 1), BytesForPages(Rig.Geo, 64)));
	}
}

using namespace SuperSLMPrefixCacheU2Review;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewDeleteDiskOffTest, "SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.DiskTierOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewDeleteDiskOffTest::RunTest(const FString& Parameters)
{
	return DeleteBehindSave(*this, EHostEnd::DiskTierOff, TEXT("u2r-disk-off"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewDeleteOtherDirTest, "SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.OtherDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewDeleteOtherDirTest::RunTest(const FString& Parameters)
{
	return DeleteBehindSave(*this, EHostEnd::OtherDirectory, TEXT("u2r-other-dir"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewDeleteDeinitTest, "SuperSLM.PrefixCache.Cpu.U2Review.DeleteBehindSave.Deinitialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewDeleteDeinitTest::RunTest(const FString& Parameters)
{
	return DeleteBehindSave(*this, EHostEnd::Deinitialize, TEXT("u2r-deinit"));
}

// Finding 5, first half (§5.8): a set registered before the first Configure() is checked against the model that
// Configure() names. The set's Model is the 1.5B asset; its build was made against the rig's 0.5B model, so its
// cooked keys would match and serve if it registered (a sharper reading than the review's, where they never
// match). The cache-on Configure() refuses it by name, once: PrefixSetModelMismatch reads 1, one line names it,
// nothing registers, and S recomputes. Red at 79163cd69: CheckPrefixSetModel() passes while the core is
// unconfigured, so the entry registers (CookedEntriesRegistered 1) and PrefixSetModelMismatch reads 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewSetBeforeConfigureTest, "SuperSLM.PrefixCache.Cpu.U2Review.SetBeforeFirstConfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewSetBeforeConfigureTest::RunTest(const FString& Parameters)
{
	// A configured rig only to tokenize and build the set; the subsystem under test is a second, fresh one.
	FPagingRig Builder;
	TArray<int32> Tokens;
	if (!Builder.Init(*this) || !LegacyCacheOn(*this, Builder) || !MakeTokens(*this, *Builder.Cpu, 200, 1, Tokens))
	{
		return false;
	}
	USuperSLMModel* Other = ImportOtherModel(*this);
	USuperSLMPrefixSet* Set = Other != nullptr ? BuiltSet(*this, Builder.Model, Tokens) : nullptr;
	if (Set == nullptr)
	{
		return false;
	}
	Set->Model = Other; // the set's model is now the 1.5B asset; its bytes stay the 0.5B build's

	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	FPagingLogCapture Log;
	Cpu.RegisterPrefixSet(Set); // before the first Configure(): the host is created early, the core unconfigured
	if (!LegacyCacheOn(*this, Rig))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("PrefixSetModelMismatch reads 1 after the first cache-on Configure()"), FallbackCount(Stats(Cpu), TEXT("PrefixSetModelMismatch")), int64(1));
	bOk &= TestEqual(TEXT("one PrefixSetModelMismatch line"), Log.CountContaining(TEXT("PrefixSetModelMismatch")), 1);
	bOk &= TestEqual(TEXT("its line names the set"), Log.CountContaining(*Set->GetPathName()), 1);
	bOk &= TestEqual(TEXT("nothing registered"), Stats(Cpu).CookedEntriesRegistered, 0);
	FSuperSLMPrefix L;
	bOk &= RequestReady(*this, Cpu, Spec(Tokens), L, TEXT("the set's entry"));
	bOk &= ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("the set's entry: recomputed, never cooked"));
	Cpu.ReturnPrefix(L);
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Finding 5, second half (§5.8): "a re-Configure() with another model unregisters such a set". The set matches
// model A and registers; Configure(B) unregisters it and raises PrefixSetModelMismatch once, against B (not the
// previous model, which bConfigured still names between Abandon() and the next Configure()); a later
// Configure(A) does not bring it back, so its entry recomputes. Red at 79163cd69: the core's Configure() keeps
// Cooked, so CookedEntriesRegistered stays 1 and PrefixSetModelMismatch reads 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewSetOtherModelTest, "SuperSLM.PrefixCache.Cpu.U2Review.SetUnregisteredByOtherModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewSetOtherModelTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<int32> Tokens;
	if (!Rig.Init(*this) || !LegacyCacheOn(*this, Rig) || !MakeTokens(*this, *Rig.Cpu, 200, 1, Tokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	USuperSLMModel* Other = ImportOtherModel(*this);
	USuperSLMPrefixSet* Set = Other != nullptr ? BuiltSet(*this, Rig.Model, Tokens) : nullptr;
	if (Set == nullptr)
	{
		return false;
	}
	Cpu.RegisterPrefixSet(Set);
	bool bOk = TestEqual(TEXT("model A: the set's entry registers"), Stats(Cpu).CookedEntriesRegistered, 1);
	{
		FSuperSLMPrefix L;
		bOk &= RequestReady(*this, Cpu, Spec(Tokens), L, TEXT("model A"));
		bOk &= ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, TEXT("model A: a cooked hit"));
		Cpu.ReturnPrefix(L);
	}
	const FSuperSLMRuntimeConfig A = Rig.Config;
	{
		FPagingLogCapture Log;
		const FSuperSLMConfigureReport Report = Cpu.Configure(Other, A);
		bOk &= TestEqual(*FString::Printf(TEXT("Configure(model B) succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
		bOk &= TestEqual(TEXT("model B: the set is unregistered"), Stats(Cpu).CookedEntriesRegistered, 0);
		bOk &= TestEqual(TEXT("model B: PrefixSetModelMismatch reads 1"), FallbackCount(Stats(Cpu), TEXT("PrefixSetModelMismatch")), int64(1));
		bOk &= TestEqual(TEXT("model B: one line, naming the set"), Log.CountContaining(*Set->GetPathName()), 1);
	}
	// Back to model A: unregistered means gone, not suspended.
	const FSuperSLMConfigureReport Back = Cpu.Configure(Rig.Model, A);
	bOk &= TestEqual(*FString::Printf(TEXT("Configure(model A) again succeeds (%s)"), *Back.Message), (uint8)Back.Result, (uint8)ESuperSLMConfigureResult::Success);
	bOk &= TestEqual(TEXT("model A again: the set stays unregistered"), Stats(Cpu).CookedEntriesRegistered, 0);
	{
		FSuperSLMPrefix L;
		bOk &= RequestReady(*this, Cpu, Spec(Tokens), L, TEXT("model A again"));
		bOk &= ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Recomputed, TEXT("model A again: recomputed"));
		Cpu.ReturnPrefix(L);
	}
	Cpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Finding 6 (§5.9 rev 1.18.4): DropDeleteFailed's line carries the key, the file path and the file API's error.
// The refusal is the platform's own: the test holds S's file open for reading, and UE's Windows OpenRead shares
// read (and write) but not delete, so DeleteFile fails with ERROR_SHARING_VIOLATION (32). The line must carry
// that error: its system message, or "error 32". Red at 79163cd69: the host reduces the outcome to
// EPrefixIoStatus::Error and logs fixed text ("the file API refused the delete (the file stays)").
#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheU2ReviewDeleteErrorTest, "SuperSLM.PrefixCache.Cpu.U2Review.DropDeleteFailedCarriesError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheU2ReviewDeleteErrorTest::RunTest(const FString& Parameters)
{
	FScopedCacheDirectory Dir(TEXT("u2r-delete-error"));
	FPagingRig Rig;
	FSuperSLMRuntimeConfig C;
	TArray<int32> Tokens;
	if (!DiskOnRig(*this, Rig, Dir.Path, C) || !MakeTokens(*this, *Rig.Cpu, kLength, 3, Tokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FSuperSLMPrefixSpec S = Spec(Tokens);
	FSuperSLMPrefix L;
	if (!RequestReady(*this, Cpu, S, L, TEXT("S")))
	{
		return false;
	}
	Cpu.ReturnPrefix(L);
	const FString Path = DiskFileOf(Cpu, S);
	Drain(Cpu, [&Path]() { return !Path.IsEmpty() && IFileManager::Get().FileExists(*Path); }, LeaseSeconds);
	DrainQuiet(Cpu);
	uint32 LastError = 0;
	TUniquePtr<IFileHandle> Holder(OpenForRead(Cpu, Path, LastError));
	if (!TestTrue(*FString::Printf(TEXT("S's file is held open for reading (%s; last error %u)"), *Path, LastError), Holder.IsValid()))
	{
		return false;
	}
	FPagingLogCapture Log;
	FSuperSLMPrefixOp Op;
	FString Error;
	bool bOk = TestTrue(*FString::Printf(TEXT("DropPrefix(S, MemoryAndDisk) is accepted (%s)"), *Error), Cpu.DropPrefix(S, ESuperSLMPrefixDropScope::MemoryAndDisk, Op, Error));
	const FSuperSLMPrefixOpResult R = WaitResolved(Cpu, Op, OpSeconds);
	Holder.Reset();
	bOk &= TestEqual(TEXT("its disk part reads Failed"), (uint8)R.Disk, (uint8)ESuperSLMPrefixDropDisk::Failed);
	bOk &= TestEqual(TEXT("one DropDeleteFailed line"), Log.CountContaining(TEXT("DropDeleteFailed")), 1);
	bOk &= TestEqual(TEXT("the line names the file path"), Log.CountContaining(*Path), 1);
	TCHAR Message[512] = {};
	FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), 32);
	FString Text = FString(Message).TrimStartAndEnd();
	const int32 WithMessage = Text.IsEmpty() ? 0 : Log.CountContaining(*Text);
	bOk &= TestEqual(*FString::Printf(TEXT("the line carries the file API's error (\"%s\", or \"error 32\")"), *Text),
		FMath::Max(WithMessage, Log.CountContaining(TEXT("error 32"))), 1);
	return bOk;
}
#endif // PLATFORM_WINDOWS

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
