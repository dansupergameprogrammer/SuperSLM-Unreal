// See SuperSLMPrefixCacheHost.h.

#include "SuperSLMPrefixCacheHost.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "SuperSLMLog.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetBridge.h"
#include "SuperSLMRuntimeRegistry.h"
#include "Tasks/Task.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#endif

using namespace SuperSLMCore;

namespace
{
	FString KeyHex(const FPrefixKey& Key) { return UTF8_TO_TCHAR(PrefixKeyToHex(Key).c_str()); }

	uint64 HandleBits(sslm_prefix Handle) { return static_cast<uint64>(reinterpret_cast<UPTRINT>(Handle)); }
	sslm_prefix HandleOf(uint64 Bits) { return reinterpret_cast<sslm_prefix>(static_cast<UPTRINT>(Bits)); }

	// §5.7's atomic replace: the temporary takes the key's name in one platform call, replacing any file
	// already there. IFileManager::Move is not that call: with bReplace it deletes the destination and then
	// moves (with retries), so two instances writing one key (cell 3.8) race in the gap, and a lost race is
	// logged at Error ("Error moving file"), which fails the automation run. Box 2026-10-21: Disk.TwoInstances.
	// A failed replace returns false; the core logs it as the write's fallback and removes the temporary.
	// The call is beneath IPlatformFile, so it must be handed the path the OS sees. Elsewhere the file layer goes
	// through IPlatformFile, whose wrappers (sandbox, and the platforms that map engine paths onto app storage:
	// Android, iOS, consoles) translate the engine path first; a raw call on the untranslated path fails there on
	// every write, silently, as the write's fallback (box 2026-10-21 review, finding 5). So the path is translated by
	// the platform-file chain (ConvertToAbsolutePathForExternalAppForWrite: the physical path an outside process
	// would write) and then given to the OS. Windows keeps the full path (its platform file does not translate);
	// the plugin is verified on Win64 only, and a port must check its platform's translation reaches a path
	// rename() accepts.
#if !PLATFORM_WINDOWS
	FString PhysicalPathForWrite(const FString& Path)
	{
		return FPlatformFileManager::Get().GetPlatformFile().ConvertToAbsolutePathForExternalAppForWrite(*FPaths::ConvertRelativePathToFull(Path));
	}
#endif

	bool ReplaceFileAtomically(const FString& From, const FString& To)
	{
#if PLATFORM_WINDOWS
		return ::MoveFileExW(*FPaths::ConvertRelativePathToFull(From), *FPaths::ConvertRelativePathToFull(To),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		return ::rename(TCHAR_TO_UTF8(*PhysicalPathForWrite(From)), TCHAR_TO_UTF8(*PhysicalPathForWrite(To))) == 0;
#endif
	}

	EPrefixEngineResult EngineResultOf(sslm_status Status)
	{
		switch (Status)
		{
		case SSLM_OK: return EPrefixEngineResult::Ok;
		case SSLM_KV_POOL_EXHAUSTED: return EPrefixEngineResult::PoolExhausted;
		case SSLM_INVALID_ARGUMENT:          // the SPX1 digest, framing or a bound
		case SSLM_RESTORE_MODEL_MISMATCH:
		case SSLM_RESTORE_KV_MISMATCH:
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
		case SSLM_RESTORE_PRODUCER_MISMATCH:
#endif
			return EPrefixEngineResult::Refused;
		default: return EPrefixEngineResult::Failed;
		}
	}

	ESuperSLMPrefixSource SourceOf(EPrefixSource S)
	{
		switch (S)
		{
		case EPrefixSource::Pending: return ESuperSLMPrefixSource::Pending;
		case EPrefixSource::Memory: return ESuperSLMPrefixSource::Memory;
		case EPrefixSource::Cooked: return ESuperSLMPrefixSource::Cooked;
		case EPrefixSource::Disk: return ESuperSLMPrefixSource::Disk;
		case EPrefixSource::Recomputed: return ESuperSLMPrefixSource::Recomputed;
		default: return ESuperSLMPrefixSource::Invalid;
		}
	}

	ESuperSLMPrefixLeaseState LeaseStateOf(EPrefixLeaseState S)
	{
		switch (S)
		{
		case EPrefixLeaseState::Pending: return ESuperSLMPrefixLeaseState::Pending;
		case EPrefixLeaseState::Ready: return ESuperSLMPrefixLeaseState::Ready;
		case EPrefixLeaseState::BudgetExhausted: return ESuperSLMPrefixLeaseState::BudgetExhausted;
		case EPrefixLeaseState::PoolExhausted: return ESuperSLMPrefixLeaseState::PoolExhausted;
		case EPrefixLeaseState::UnknownSchema: return ESuperSLMPrefixLeaseState::UnknownSchema;
		case EPrefixLeaseState::Cancelled: return ESuperSLMPrefixLeaseState::Cancelled;
		case EPrefixLeaseState::Failed: return ESuperSLMPrefixLeaseState::Failed;
		case EPrefixLeaseState::Ended: return ESuperSLMPrefixLeaseState::Ended;
		default: return ESuperSLMPrefixLeaseState::Invalid;
		}
	}

	// The disk tier's file layer over IFileManager, with the test seam's faults.
	class FPrefixFileOps final : public IPrefixFileOps
	{
	public:
		explicit FPrefixFileOps(FSuperSLMPrefixCacheHost& InHost, bool bInWriteBack = false) : Host(InHost)
		{
#if WITH_DEV_AUTOMATION_TESTS
			bWriteBack = bInWriteBack; // the write-back's file layer carries the pool-thread hold (§8)
#else
			(void)bInWriteBack;
#endif
		}

		bool WriteWhole(const std::string& Path, const uint8_t* Data, size_t Size) override
		{
			Host.CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
			if (Fault() == ESuperSLMPrefixDiskFault::FailTemporaryWrite)
			{
				return false;
			}
#endif
			return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Data, static_cast<int32>(Size)), UTF8_TO_TCHAR(Path.c_str()));
		}

		EPrefixIoStatus ReadWhole(const std::string& Path, std::vector<uint8_t>& OutData) override
		{
			Host.CheckOffGameThread();
			const FString File = UTF8_TO_TCHAR(Path.c_str());
#if WITH_DEV_AUTOMATION_TESTS
			{
				TFunction<void(const FString&)> Hook;
				{
					FScopeLock Lock(&Host.Test.HookGuard);
					Hook = MoveTemp(Host.Test.BeforeDiskRead);
					Host.Test.BeforeDiskRead = nullptr;
				}
				if (Hook)
				{
					Hook(File);
				}
			}
			if (Fault() == ESuperSLMPrefixDiskFault::FailRead)
			{
				return EPrefixIoStatus::Error;
			}
#endif
			if (!IFileManager::Get().FileExists(*File))
			{
				return EPrefixIoStatus::NotFound;
			}
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent))
			{
				// A file that vanished between the check and the open, or mid-rename, is a plain miss.
				return IFileManager::Get().FileExists(*File) ? EPrefixIoStatus::Error : EPrefixIoStatus::NotFound;
			}
			OutData.assign(Bytes.GetData(), Bytes.GetData() + Bytes.Num());
			return EPrefixIoStatus::Ok;
		}

		bool RenameReplace(const std::string& From, const std::string& To) override
		{
			Host.CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
			if (bWriteBack)
			{
				Host.PassWriteBackHold(); // the pool-thread hold (§8): stopped mid-write, the temporary written
			}
			const ESuperSLMPrefixDiskFault F = Fault();
			if (F == ESuperSLMPrefixDiskFault::FailRename)
			{
				return false;
			}
			if (F == ESuperSLMPrefixDiskFault::CrashBeforeRename)
			{
				bCrashed = true; // the process "died" here: the temporary stays, nothing is renamed
				return true;
			}
#endif
			return ReplaceFileAtomically(UTF8_TO_TCHAR(From.c_str()), UTF8_TO_TCHAR(To.c_str()));
		}

		bool RemoveFile(const std::string& Path) override
		{
			Host.CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
			if (bCrashed)
			{
				return true; // a crashed writer removes nothing
			}
#endif
			const FString File = UTF8_TO_TCHAR(Path.c_str());
			return !IFileManager::Get().FileExists(*File) || IFileManager::Get().Delete(*File, false, true, true);
		}

		bool ListDirectory(const std::string& Dir, std::vector<FPrefixDiskFileInfo>& OutFiles) override
		{
			Host.CheckOffGameThread();
			OutFiles.clear();
			const FString Directory = UTF8_TO_TCHAR(Dir.c_str());
			IFileManager::Get().IterateDirectoryStat(*Directory, [&OutFiles](const TCHAR* Path, const FFileStatData& Stat)
			{
				if (!Stat.bIsDirectory)
				{
					FPrefixDiskFileInfo Info;
					Info.Name = TCHAR_TO_UTF8(*FPaths::GetCleanFilename(Path));
					Info.SizeBytes = static_cast<uint64_t>(FMath::Max<int64>(Stat.FileSize, 0));
					Info.ModifiedTime = Stat.ModificationTime.GetTicks();
					OutFiles.push_back(MoveTemp(Info));
				}
				return true;
			});
			return true;
		}

		bool TouchModifiedTime(const std::string& Path) override
		{
			Host.CheckOffGameThread();
			return IFileManager::Get().SetTimeStamp(UTF8_TO_TCHAR(Path.c_str()), FDateTime::UtcNow());
		}

	private:
		FSuperSLMPrefixCacheHost& Host;
#if WITH_DEV_AUTOMATION_TESTS
		bool bWriteBack = false;
		bool bCrashed = false;
		ESuperSLMPrefixDiskFault Fault() const { return static_cast<ESuperSLMPrefixDiskFault>(Host.Test.DiskFault.load()); }
#endif
	};
}

namespace
{
	// A drop's delete through the platform's own call, so the error is read at the point of failure, before any
	// other call can overwrite it (§5.9 rev 1.18.4; U2 re-check R1). IFileManager::Delete is not used here: it
	// may make further file calls after the failed delete, so the last error read after it returns is not
	// reliably the delete's. Returns true when the file was deleted; OutError is the platform's error otherwise.
	bool DeletePrefixFile(const FString& File, uint32& OutError)
	{
		OutError = 0;
		// As IFileManager::Delete(..., bEvenReadOnly) would: a read-only file is made writable first.
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, false);
#if PLATFORM_WINDOWS
		if (::DeleteFileW(*FPaths::ConvertRelativePathToFull(File)) != 0)
		{
			return true;
		}
		OutError = static_cast<uint32>(::GetLastError());
		return false;
#else
		if (::unlink(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(File))) == 0)
		{
			return true;
		}
		OutError = static_cast<uint32>(errno);
		return false;
#endif
	}
}

FSuperSLMPrefixCacheHost::FSuperSLMPrefixCacheHost(int64* PrefixIdCounter)
	: CacheCore(PrefixIdCounter)
{
}

FSuperSLMPrefixCacheHost::~FSuperSLMPrefixCacheHost()
{
	// The subsystem calls ReleaseForPoolDestroy() before the pool goes; here only the IO and the
	// bookkeeping are left.
#if WITH_DEV_AUTOMATION_TESTS
	ReleaseTestHolds();
#endif
	while (InFlightIo.load(std::memory_order_acquire) > 0)
	{
		FPlatformProcess::Sleep(0.001f);
	}
}

FSuperSLMPrefixCacheCore& FSuperSLMPrefixCacheHost::Core()
{
#if WITH_DEV_AUTOMATION_TESTS
	Test.GameThreadStateAccesses.fetch_add(1);
	if (!IsInGameThread())
	{
		if (Test.bRecordAsserts.load())
		{
			Test.bAssertTripped.store(true);
		}
		else
		{
			checkf(false, TEXT("SuperSLM prefix cache: its state was reached off the game thread"));
		}
	}
#else
	check(IsInGameThread());
#endif
	return CacheCore;
}

const FSuperSLMPrefixCacheCore& FSuperSLMPrefixCacheHost::Core() const
{
	return const_cast<FSuperSLMPrefixCacheHost*>(this)->Core();
}

void FSuperSLMPrefixCacheHost::CheckOffGameThread() const
{
#if WITH_DEV_AUTOMATION_TESTS
	Test.OffGameThreadChecks.fetch_add(1);
	if (IsInGameThread())
	{
		if (Test.bRecordOffGameThread.load())
		{
			Test.OffGameThreadViolations.fetch_add(1); // recording mode (cell 7.8): counted, not asserted
			if (Test.bRecordAsserts.load())
			{
				Test.bAssertTripped.store(true);
			}
		}
		else if (Test.bRecordAsserts.load())
		{
			Test.bAssertTripped.store(true);
		}
		else
		{
			checkf(false, TEXT("SuperSLM prefix cache: hashing, file IO or a Layer-1 call ran on the game thread"));
		}
	}
#else
	checkSlow(!IsInGameThread());
#endif
}

bool FSuperSLMPrefixCacheHost::Configure(const FPrefixCacheConfig& InConfig, FString& OutError)
{
	Config = InConfig;
	bTearingDown = false;
	ChunkJobs.Reset();   // the teardown released every open build (ReleaseForPoolDestroy)
	OpenBuilds.Reset();
	if (!Core().Configure(Config))
	{
		OutError = TEXT("the prefix cache refused its configuration (in budget mode: region C must read 0 charged and whole pages)");
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	// Test.ReleasesByHandle is NOT cleared here (box 2026-10-21, Cpu.WarmReconfigure): the teardown before this
	// Configure() released the previous configuration's handles, and cells 1.7 and 2.10 read those counts after
	// the re-Configure() returns. A handle's count restarts when the address is minted again (MarkHandleMinted).
	Test.Collisions.Reset();
	Test.HeldReleases.Reset();
	Test.bHoldReleases = false;
	// The rev 1.18 seams read "since Configure()" (§8): the counters restart and the holds start disarmed.
	ReleaseTestHolds();
	{
		FScopeLock Lock(&Test.BuildStateGuard);
		Test.BuildStates.Reset();
	}
	Test.MaxDiskExclusion.store(0);
	Test.OffGameThreadViolations.store(0);
	Test.LastRestoreSharedPages = -1;
	Test.MaxKeyDiskOverlap.store(0);
#endif
	// Every registered set against the model this configuration names (§5.8, rev 1.18.5; U2 code review finding
	// 5): one registered before the first Configure(), when there was no model to check against, and one a
	// re-Configure() to another model no longer matches, is unregistered with PrefixSetModelMismatch, once.
	{
		TArray<FString> Mismatched;
		for (const TPair<FString, FCookedSet>& Pair : CookedBySet)
		{
			if (!Core().CheckPrefixSetModel(Pair.Value.Name, Pair.Value.ModelHash))
			{
				Mismatched.Add(Pair.Key);
			}
		}
		for (const FString& SetPath : Mismatched)
		{
			FCookedSet Gone;
			CookedBySet.RemoveAndCopyValue(SetPath, Gone);
			UnregisterCookedIds(Gone.Ids);
		}
	}
	Pump(); // takes Configure()'s TrimDisk
	return true;
}

bool FSuperSLMPrefixCacheHost::IsCacheHandle(const FSuperSLMPrefix& Prefix) const
{
	return Prefix.IsValid() && Core().GetLeaseState(Prefix.Id) != EPrefixLeaseState::Unknown;
}

bool FSuperSLMPrefixCacheHost::Request(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& OutLease, FString& OutError)
{
	FPrefixSpec CoreSpec;
	CoreSpec.Tokens.assign(Spec.Tokens.GetData(), Spec.Tokens.GetData() + Spec.Tokens.Num());
	CoreSpec.SchemaName = Spec.SchemaName.IsNone() ? std::string() : std::string(TCHAR_TO_UTF8(*Spec.SchemaName.ToString()));
	CoreSpec.ParentHandle = Spec.Parent.Id;
	int64_t Handle = 0;
	std::string Error;
	const EPrefixRequestResult Result = Core().Request(CoreSpec, Handle, Error);
	FlushLog();
	if (Result != EPrefixRequestResult::Accepted)
	{
		OutError = UTF8_TO_TCHAR(Error.c_str());
		if (Result == EPrefixRequestResult::NestedRequiresBudgetMode && !OutError.Contains(TEXT("NestedRequiresBudgetMode")))
		{
			OutError = TEXT("NestedRequiresBudgetMode: ") + OutError;
		}
		return false;
	}
	OutLease.Id = Handle;
	return true;
}

bool FSuperSLMPrefixCacheHost::Return(const FSuperSLMPrefix& Lease)
{
	const bool bEnded = Core().Return(Lease.Id);
	FlushLog();
	return bEnded;
}

ESuperSLMPrefixSource FSuperSLMPrefixCacheHost::GetSource(const FSuperSLMPrefix& Lease) const
{
	return SourceOf(Core().GetSource(Lease.Id));
}

ESuperSLMPrefixLeaseState FSuperSLMPrefixCacheHost::GetLeaseState(const FSuperSLMPrefix& Lease) const
{
	return LeaseStateOf(Core().GetLeaseState(Lease.Id));
}

FSuperSLMPrefixCacheStats FSuperSLMPrefixCacheHost::GetStats() const
{
	const FPrefixCacheDiagnostics D = Core().GetDiagnostics();
	FSuperSLMPrefixCacheStats S;
	S.bConfigured = Core().IsConfigured();
	S.bBudgetMode = Config.RegionC != nullptr;
	S.BudgetBytes = static_cast<int64>(D.BudgetBytes);
	S.ChargedBytes = static_cast<int64>(D.ChargedBytes);
	S.ResidentBytes = static_cast<int64>(D.ResidentBytes);
	S.InFlightBytes = static_cast<int64>(D.InFlightBytes);
	S.PendingRefundBytes = static_cast<int64>(D.ReleasingBytes);
	S.ClaimedRefundBytes = static_cast<int64>(D.ClaimedRefundBytes);
	S.UnclaimedRefundBytes = static_cast<int64>(D.UnclaimedRefundBytes);
	S.WaitingBytes = static_cast<int64>(D.WaitingBytes);
	S.ResidentEntries = static_cast<int32>(D.ResidentEntries);
	S.Waiters = static_cast<int32>(D.Waiters);
	S.PendingRefunds = static_cast<int32>(D.PendingRefunds);
	S.Requests = static_cast<int64>(D.Requests);
	S.Evictions = static_cast<int64>(D.Evictions);
	S.RestoresIssued = static_cast<int64>(D.RestoresIssued);
	S.BuildsIssued = static_cast<int64>(D.BuildsIssued);
	S.ReleasesIssued = static_cast<int64>(D.ReleasesIssued);
	S.DiskWritesIssued = static_cast<int64>(D.DiskWritesIssued);
	S.SpecMismatches = static_cast<int64>(D.SpecMismatches);
	// EPrefixSource and ESuperSLMPrefixSource share the order Pending..Recomputed at 1..5; the
	// public index 0 is Pending and 5 Invalid, so each is moved by name.
	S.ServedBySource[(int32)ESuperSLMPrefixSource::Memory] = static_cast<int64>(D.ServedBySource[(int32)EPrefixSource::Memory]);
	S.ServedBySource[(int32)ESuperSLMPrefixSource::Cooked] = static_cast<int64>(D.ServedBySource[(int32)EPrefixSource::Cooked]);
	S.ServedBySource[(int32)ESuperSLMPrefixSource::Disk] = static_cast<int64>(D.ServedBySource[(int32)EPrefixSource::Disk]);
	S.ServedBySource[(int32)ESuperSLMPrefixSource::Recomputed] = static_cast<int64>(D.ServedBySource[(int32)EPrefixSource::Recomputed]);
	for (uint32 R = 0; R < kPrefixFallbackCount; ++R)
	{
		if (D.Fallbacks[R] != 0)
		{
			S.Fallbacks.Add(FName(UTF8_TO_TCHAR(PrefixFallbackName(static_cast<EPrefixFallback>(R)))), static_cast<int64>(D.Fallbacks[R]));
		}
	}
	S.StaleCompletionsDropped = static_cast<int64>(D.StaleCompletionsDropped);
	S.CookedEntriesRegistered = CookedEntriesRegistered();
	S.RegionChargeFaults = static_cast<int64>(D.RegionChargeFaults);
	S.GhostBytes = static_cast<int64>(D.GhostBytes);
	S.Ghosts = static_cast<int32>(D.Ghosts);
	S.GhostEndings = static_cast<int64>(D.GhostEndings);
	S.NestedRequests = static_cast<int64>(D.NestedRequests);
	S.Warms = static_cast<int64>(D.Warms);
	S.Drops = static_cast<int64>(D.Drops);
	S.DropsDeferred = static_cast<int64>(D.DropsDeferred);
	S.MarksApplied = static_cast<int64>(D.MarksApplied);
	S.MarksCleared = static_cast<int64>(D.MarksCleared);
	S.DropDeletes = static_cast<int64>(D.DropDeletes);
	S.PrefixPrefillCalls = PrefixPrefillCalls.load(std::memory_order_relaxed);
	S.PrefixPrefillBuilds = PrefixPrefillBuilds.load(std::memory_order_relaxed);
	return S;
}

// ---- Explicit residency control (§5.6, rev 1.18, PC-16 (a)) ----

namespace
{
	FPrefixSpec CoreSpecOf(const FSuperSLMPrefixSpec& Spec)
	{
		FPrefixSpec CoreSpec;
		CoreSpec.Tokens.assign(Spec.Tokens.GetData(), Spec.Tokens.GetData() + Spec.Tokens.Num());
		CoreSpec.SchemaName = Spec.SchemaName.IsNone() ? std::string() : std::string(TCHAR_TO_UTF8(*Spec.SchemaName.ToString()));
		CoreSpec.ParentHandle = Spec.Parent.Id;
		return CoreSpec;
	}

	// A refusal's message, with the refusal's name when the core's text lacks it (as Request does).
	FString RefusalText(EPrefixRequestResult Result, const std::string& Error)
	{
		FString Out = UTF8_TO_TCHAR(Error.c_str());
		if (Result == EPrefixRequestResult::NestedRequiresBudgetMode && !Out.Contains(TEXT("NestedRequiresBudgetMode")))
		{
			Out = TEXT("NestedRequiresBudgetMode: ") + Out;
		}
		return Out;
	}
}

bool FSuperSLMPrefixCacheHost::Warm(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	uint64_t OpId = 0;
	std::string Error;
	const EPrefixRequestResult Result = Core().Warm(CoreSpecOf(Spec), OpId, Error);
	FlushLog();
	if (Result != EPrefixRequestResult::Accepted)
	{
		OutError = RefusalText(Result, Error);
		return false;
	}
	OutOp.Id = static_cast<int64>(OpId);
	return true;
}

bool FSuperSLMPrefixCacheHost::Drop(const FSuperSLMPrefixSpec& Spec, ESuperSLMPrefixDropScope Scope, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	uint64_t OpId = 0;
	std::string Error;
	const EPrefixDropScope CoreScope = Scope == ESuperSLMPrefixDropScope::MemoryAndDisk ? EPrefixDropScope::MemoryAndDisk : EPrefixDropScope::Memory;
	const EPrefixRequestResult Result = Core().Drop(CoreSpecOf(Spec), CoreScope, OpId, Error);
	FlushLog();
	if (Result != EPrefixRequestResult::Accepted)
	{
		OutError = RefusalText(Result, Error);
		return false;
	}
	OutOp.Id = static_cast<int64>(OpId);
	return true;
}

bool FSuperSLMPrefixCacheHost::GetOpResult(const FSuperSLMPrefixOp& Op, FSuperSLMPrefixOpResult& Out) const
{
	Out = FSuperSLMPrefixOpResult();
	FPrefixOpResult R;
	if (Op.Id <= 0 || !Core().GetOpResult(static_cast<uint64_t>(Op.Id), R))
	{
		return false;
	}
	// The native enums keep the core's order (SuperSLMPrefixCacheTypes.h, SuperSLMPrefixCacheCore.h).
	static_assert(static_cast<uint8>(ESuperSLMPrefixOpResult::Expired) == static_cast<uint8>(EPrefixOpResult::Expired), "op result order");
	static_assert(static_cast<uint8>(ESuperSLMPrefixOpResult::Cancelled) == static_cast<uint8>(EPrefixOpResult::Cancelled), "op result order");
	static_assert(static_cast<uint8>(ESuperSLMPrefixOpResult::Failed) == static_cast<uint8>(EPrefixOpResult::Failed), "op result order");
	static_assert(static_cast<uint8>(ESuperSLMPrefixDropDisk::Failed) == static_cast<uint8>(EPrefixDropDisk::Failed), "drop disk order");
	Out.Result = static_cast<ESuperSLMPrefixOpResult>(static_cast<uint8>(R.Result));
	Out.Disk = static_cast<ESuperSLMPrefixDropDisk>(static_cast<uint8>(R.Disk));
	if (R.Result == EPrefixOpResult::Failed && R.Reason != EPrefixFallback::Count)
	{
		Out.Reason = FName(UTF8_TO_TCHAR(PrefixFallbackName(R.Reason))); // as its log line names it (§5.6 rev 1.18.5)
	}
	return true;
}

bool FSuperSLMPrefixCacheHost::RefuseRelease(const FSuperSLMPrefix& Prefix, FString& OutError)
{
	std::string Error;
	const bool bRefuse = Core().RefuseRelease(Prefix.Id, Error);
	FlushLog();
	if (bRefuse)
	{
		OutError = UTF8_TO_TCHAR(Error.c_str());
	}
	return bRefuse;
}

FSuperSLMPrefixCacheHost::EAdoptLookup FSuperSLMPrefixCacheHost::BeginAdopt(const FSuperSLMPrefix& Prefix, sslm_prefix& OutHandle, uint64& OutToken, uint64& OutEntryId, FString& OutError)
{
	uint64_t Engine = 0;
	uint64_t Token = 0;
	uint64_t EntryId = 0;
	const EPrefixAdoptCheck Check = Core().BeginAdopt(Prefix.Id, Engine, Token, &EntryId);
	FlushLog();
	OutHandle = nullptr;
	OutToken = 0;
	OutEntryId = 0;
	switch (Check)
	{
	case EPrefixAdoptCheck::Adoptable:
		OutHandle = HandleOf(Engine);
		OutToken = Token;
		OutEntryId = EntryId;
		return EAdoptLookup::Adoptable;
	case EPrefixAdoptCheck::NotReady:
		OutError = TEXT("the cached prefix's lease is not Ready");
		return EAdoptLookup::NotReady;
	case EPrefixAdoptCheck::PrefixLeaseEnded:
		OutError = TEXT("PrefixLeaseEnded: the lease was returned, or its entry was evicted");
		return EAdoptLookup::LeaseEnded;
	default:
		return EAdoptLookup::NotCacheHandle;
	}
}

void FSuperSLMPrefixCacheHost::EndAdopt(uint64 Token)
{
	Core().EndAdopt(Token);
	FlushLog();
}

// ---- Payloads ----

uint64 FSuperSLMPrefixCacheHost::StorePayload(TArray<uint8>&& Bytes)
{
	const uint64 Ref = NextPayloadRef.fetch_add(1);
	FScopeLock Lock(&PayloadGuard);
	Payloads.Add(Ref, MoveTemp(Bytes));
	return Ref;
}

bool FSuperSLMPrefixCacheHost::TakePayloadCopy(uint64 Ref, TArray<uint8>& Out) const
{
	FScopeLock Lock(&PayloadGuard);
	const TArray<uint8>* Found = Payloads.Find(Ref);
	if (!Found)
	{
		return false;
	}
	Out = *Found;
	return true;
}

int64 FSuperSLMPrefixCacheHost::PayloadBytesOf(uint64 Ref) const
{
	FScopeLock Lock(&PayloadGuard);
	const TArray<uint8>* Found = Payloads.Find(Ref);
	return Found != nullptr ? static_cast<int64>(Found->Num()) : 0;
}

void FSuperSLMPrefixCacheHost::DropPayload(uint64 Ref)
{
	FScopeLock Lock(&PayloadGuard);
	Payloads.Remove(Ref);
}

// ---- The executor ----

void FSuperSLMPrefixCacheHost::Pump()
{
	check(IsInGameThread());
	FPrefixCompletion Done;
	while (Inbox.Dequeue(Done))
	{
		ApplyCompletion(Done);
	}
	// The end of Tick()'s apply pass (§5.6 *When a mark is applied*, steps 2 and 3): mark applications are queued
	// and the issue-ordered ops (requests, warms, drops) whose hashes have completed are applied, in issue order.
	Core().EndApplyPass();
	for (const FPrefixCommand& Command : Core().TakeCommands())
	{
		StartCommand(Command);
	}
	StartNextDiskWork();
	FlushLog();
}

void FSuperSLMPrefixCacheHost::ApplyCompletion(const FPrefixCompletion& Completion)
{
	Core().Apply(Completion);
	OnDiskCommandApplied(Completion.CommandId);
}

void FSuperSLMPrefixCacheHost::StartCommand(const FPrefixCommand& Command)
{
	switch (Command.Kind)
	{
	case EPrefixCommandKind::ComputeKey:
	case EPrefixCommandKind::CheckCooked:
		LaunchPoolThread(Command);
		break;
	case EPrefixCommandKind::ReadDisk:
	case EPrefixCommandKind::WriteDisk:
	case EPrefixCommandKind::RemoveDiskFile:
	case EPrefixCommandKind::TouchDiskFile:
	case EPrefixCommandKind::TrimDisk:
		StartDiskCommand(Command);
		break;
	case EPrefixCommandKind::Restore:
	case EPrefixCommandKind::Build:
	case EPrefixCommandKind::Save:
		LaneJobs.Add(MakeLaneJob(Command));
		break;
	case EPrefixCommandKind::Release:
	{
		FSuperSLMPrefixLaneJob Job = MakeLaneJob(Command);
		// Out of the live set at issue, on the game thread, before any lane can free the address (see
		// LiveEngineHandles; review finding 2). A held release is issued too: the core has committed to it.
		LiveEngineHandles.Remove(Job.ReleasedHandle);
#if WITH_DEV_AUTOMATION_TESTS
		Job.ReleasedMint = Test.MintByHandle.FindRef(Job.ReleasedHandle);
		if (Test.bHoldReleases)
		{
			Test.HeldReleases.Add(MoveTemp(Job));
			break;
		}
#endif
		LaneJobs.Add(MoveTemp(Job));
		break;
	}
	case EPrefixCommandKind::DropPayload:
	{
		DropPayload(Command.PayloadRef);
		FPrefixCompletion Done;
		Done.CommandId = Command.Id;
		Done.Generation = Command.Generation;
		Core().Apply(Done);
		break;
	}
	}
}

// Disk commands: one per key in flight; TrimDisk alone.
void FSuperSLMPrefixCacheHost::StartDiskCommand(const FPrefixCommand& Command)
{
	if (Command.Kind == EPrefixCommandKind::TrimDisk)
	{
		TrimQueue.Add(Command);
		return;
	}
	DiskQueues.FindOrAdd(KeyHex(Command.Key)).Add(Command);
}

void FSuperSLMPrefixCacheHost::StartNextDiskWork()
{
	if (bTrimInFlight)
	{
		return;
	}
	if (TrimQueue.Num() > 0 && DiskKeysInFlight.Num() == 0)
	{
		const FPrefixCommand Trim = TrimQueue[0];
		TrimQueue.RemoveAt(0);
		bTrimInFlight = true;
		DiskCommandKey.Add(Trim.Id, FString());
		LaunchPoolThread(Trim);
		return;
	}
	if (TrimQueue.Num() > 0)
	{
		return; // a trim waits for the keys in flight; nothing new starts before it
	}
	for (auto It = DiskQueues.CreateIterator(); It; ++It)
	{
		if (It.Value().Num() == 0)
		{
			It.RemoveCurrent();
			continue;
		}
		if (DiskKeysInFlight.Contains(It.Key()))
		{
			continue;
		}
		const FPrefixCommand Next = It.Value()[0];
		It.Value().RemoveAt(0);
		DiskKeysInFlight.Add(It.Key());
		DiskCommandKey.Add(Next.Id, It.Key());
		LaunchPoolThread(Next);
	}
}

void FSuperSLMPrefixCacheHost::OnDiskCommandApplied(uint64 CommandId)
{
	FString Key;
	if (DiskCommandKey.RemoveAndCopyValue(CommandId, Key))
	{
		if (Key.IsEmpty())
		{
			bTrimInFlight = false;
		}
		else
		{
			DiskKeysInFlight.Remove(Key);
		}
	}
}

void FSuperSLMPrefixCacheHost::LaunchPoolThread(const FPrefixCommand& Command)
{
	InFlightIo.fetch_add(1);
	UE::Tasks::Launch(TEXT("SuperSLMPrefixCacheIo"), [this, Command]()
	{
		FPrefixCompletion Out;
		Out.CommandId = Command.Id;
		Out.Generation = Command.Generation;
#if WITH_DEV_AUTOMATION_TESTS
		// The per-key overlap counter (§8): a key's read, write-back write, delete and touch.
		const bool bKeyDisk = Command.Kind == EPrefixCommandKind::ReadDisk || Command.Kind == EPrefixCommandKind::WriteDisk ||
			Command.Kind == EPrefixCommandKind::RemoveDiskFile || Command.Kind == EPrefixCommandKind::TouchDiskFile;
		if (bKeyDisk)
		{
			EnterKeyDisk(Command.Key);
		}
#endif
		switch (Command.Kind)
		{
		case EPrefixCommandKind::ComputeKey: RunComputeKey(Command, Out); break;
		case EPrefixCommandKind::CheckCooked: RunCheckCooked(Command, Out); break;
		case EPrefixCommandKind::ReadDisk: RunReadDisk(Command, Out); break;
		case EPrefixCommandKind::WriteDisk: RunWriteDisk(Command, Out); break;
		case EPrefixCommandKind::RemoveDiskFile:
		{
#if WITH_DEV_AUTOMATION_TESTS
			EnterDiskExclusion();
#endif
			// The delete's outcome (a drop's disk part reads it, §5.6; a wrapper miss's removal ignores it):
			// Ok = deleted, NotFound = no file, Error = the file API refused (the file stays), with the file API's
			// error for the DropDeleteFailed line (§5.9 rev 1.18.4), captured where the delete fails (R1).
			CheckOffGameThread();
			const std::string Path = Config.DiskDirectory + "/" + PrefixDiskFileName(Command.Key);
			const bool bExisted = IFileManager::Get().FileExists(UTF8_TO_TCHAR(Path.c_str()));
#if WITH_DEV_AUTOMATION_TESTS
			// The file-delete refusal (§8): a drop's delete meets its file held open without delete sharing, so the
			// refusal and its error are the platform's own.
			TUniquePtr<IFileHandle> Refusal;
			if (bExisted && Command.OpId != 0 && Test.bRefuseDeletes.load())
			{
				Refusal.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(UTF8_TO_TCHAR(Path.c_str())));
			}
#endif
			uint32 LastError = 0;
			bool bRemoved = !bExisted || DeletePrefixFile(UTF8_TO_TCHAR(Path.c_str()), LastError);
#if WITH_DEV_AUTOMATION_TESTS && !PLATFORM_WINDOWS
			if (Refusal.IsValid() && bRemoved)
			{
				// Where an open file can be deleted (POSIX) the seam cannot make the platform refuse; it reports the
				// refusal Windows would (ERROR_SHARING_VIOLATION). The file is gone either way.
				bRemoved = false;
				LastError = 32;
			}
#endif
			Out.Io = !bExisted ? EPrefixIoStatus::NotFound : (bRemoved ? EPrefixIoStatus::Ok : EPrefixIoStatus::Error);
			if (Out.Io == EPrefixIoStatus::Error)
			{
				TCHAR Message[512] = {};
				FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), static_cast<int32>(LastError));
				const FString Text = FString(Message).TrimStartAndEnd();
				Out.IoError = TCHAR_TO_UTF8(*FString::Printf(TEXT("file API error %u%s%s"), LastError, Text.IsEmpty() ? TEXT("") : TEXT(": "), *Text));
			}
#if WITH_DEV_AUTOMATION_TESTS
			Refusal.Reset();
			LeaveDiskExclusion();
#endif
			break;
		}
		case EPrefixCommandKind::TouchDiskFile:
		{
			FPrefixFileOps Files(*this);
			Files.TouchModifiedTime(Config.DiskDirectory + "/" + PrefixDiskFileName(Command.Key));
			break;
		}
		case EPrefixCommandKind::TrimDisk:
		{
#if WITH_DEV_AUTOMATION_TESTS
			EnterDiskExclusion();
#endif
			FPrefixFileOps Files(*this);
			// A crash between a write and its rename leaves its temporary; one an hour old is no write in progress
			// (FDateTime ticks are 100 ns, the file layer's ModifiedTime unit).
			RemoveStalePrefixDiskTemps(Files, Config.DiskDirectory, FDateTime::UtcNow().GetTicks(), ETimespan::TicksPerHour);
			TrimPrefixDiskDirectory(Files, Config.DiskDirectory, Config.DiskMaxBytes);
#if WITH_DEV_AUTOMATION_TESTS
			LeaveDiskExclusion();
#endif
			break;
		}
		default:
			break;
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (bKeyDisk)
		{
			LeaveKeyDisk(Command.Key);
		}
		// The hash-completion hold (§8): a held spec's hash is kept, not enqueued, until its hold is cleared.
		if (Command.Kind == EPrefixCommandKind::ComputeKey && ParkHeldHash(Command, Out))
		{
			InFlightIo.fetch_sub(1);
			return;
		}
#endif
		Inbox.Enqueue(MoveTemp(Out));
		InFlightIo.fetch_sub(1);
	});
}

void FSuperSLMPrefixCacheHost::RunComputeKey(const FPrefixCommand& Command, FPrefixCompletion& Out) const
{
	CheckOffGameThread();
	FPrefixKeyInputs Inputs = Command.KeyInputs;
#if WITH_DEV_AUTOMATION_TESTS
	// ForceKeyCollision (cells 2.8, 11.4): this spec's key is computed from CollideWith's fields.
	for (const FTestState::FCollision& C : Test.Collisions)
	{
		const std::string FromSchema = TCHAR_TO_UTF8(*C.FromSchema);
		if (Inputs.SchemaName == FromSchema && Inputs.Tokens.size() == static_cast<size_t>(C.FromTokens.Num()) &&
			FMemory::Memcmp(Inputs.Tokens.data(), C.FromTokens.GetData(), Inputs.Tokens.size() * sizeof(int32)) == 0)
		{
			Inputs.SchemaName = TCHAR_TO_UTF8(*C.ToSchema);
			Inputs.Tokens.assign(C.ToTokens.GetData(), C.ToTokens.GetData() + C.ToTokens.Num());
			break;
		}
	}
#endif
	Out.Key = ComputePrefixKey(Inputs);
}

void FSuperSLMPrefixCacheHost::RunReadDisk(const FPrefixCommand& Command, FPrefixCompletion& Out)
{
	CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
	PassDiskReadHold(); // taken, the file not yet opened (§8)
#endif
	FPrefixFileOps Files(*this);
	std::vector<uint8_t> Bytes;
	Out.Io = ReadPrefixFile(Files, Config.DiskDirectory, Command.Key, Bytes);
	if (Out.Io != EPrefixIoStatus::Ok)
	{
		return;
	}
	size_t Offset = 0;
	size_t Size = 0;
	Out.Wrapper = CheckPrefixWrapper(Bytes.data(), Bytes.size(), Config.Identity, Command.Key, Command.Spec, Offset, Size);
	if (Out.Wrapper == EPrefixWrapperCheck::Ok)
	{
		TArray<uint8> Payload(Bytes.data() + Offset, static_cast<int32>(Size));
		Out.PayloadRef = StorePayload(MoveTemp(Payload));
	}
}

void FSuperSLMPrefixCacheHost::RunCheckCooked(const FPrefixCommand& Command, FPrefixCompletion& Out)
{
	CheckOffGameThread();
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Blob;
	{
		FScopeLock Lock(&CookedGuard);
		if (const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Found = CookedBlobs.Find(Command.CookedBlobId))
		{
			Blob = *Found;
		}
	}
	if (!Blob.IsValid())
	{
		// Unregistered while the check was queued: a miss (the core marks the gone blob stale).
		Out.Wrapper = EPrefixWrapperCheck::Truncated;
		return;
	}
	// The disk tier's checks over bytes already in memory (§5.7): a failure is a miss, and the core
	// marks the entry stale and logs the reason once.
	size_t Offset = 0;
	size_t Size = 0;
	Out.Wrapper = CheckPrefixWrapper(Blob->GetData(), static_cast<size_t>(Blob->Num()), Config.Identity, Command.Key, Command.Spec, Offset, Size);
	if (Out.Wrapper == EPrefixWrapperCheck::Ok)
	{
		TArray<uint8> Payload(Blob->GetData() + Offset, static_cast<int32>(Size));
		Out.PayloadRef = StorePayload(MoveTemp(Payload));
	}
}

// ---- The cooked tier ----

int32 FSuperSLMPrefixCacheHost::RegisterCookedSet(const USuperSLMPrefixSet& Set)
{
	UnregisterCookedSet(Set);
	// The set's model against the configured model (§5.8, rev 1.18.2; §5.9 rev 1.18.4): a set's ids belong to its
	// model's tokenizer, so a mismatched set is refused by name (PrefixSetModelMismatch, once) and nothing registers.
	// An unreadable model reads as zeros, and is refused the same way. Before the first Configure() there is no
	// model to check against: the set registers, and Configure() checks it (rev 1.18.5).
	std::array<uint8_t, 32> SetHash{};
	const std::string SetName = TCHAR_TO_UTF8(*Set.GetPathName());
	{
		uint8 Hash[32];
		if (Set.Model != nullptr && SuperSLMRuntime::ReadArtifactHash(*Set.Model, Hash))
		{
			FMemory::Memcpy(SetHash.data(), Hash, 32);
		}
		const bool bMatches = Core().CheckPrefixSetModel(SetName, SetHash);
		FlushLog();
		if (!bMatches)
		{
			return 0;
		}
	}
	const std::vector<FPrefixSetBuild> Builds = SuperSLMPrefixSetBridge::CoreBuilds(Set);
	FPrefixEngineIdentity Runtime;
	Runtime.Backend = 0; // this host serves the CPU subsystem; the GPU twin selects by its device (U7)
	const int32_t Index = SelectPrefixSetBuild(Builds, Runtime);
	if (Index < 0)
	{
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache: %s has no CPU build; its entries recompute"), *Set.GetPathName());
		return 0;
	}
	const FPrefixSetBuild& Build = Builds[static_cast<size_t>(Index)];
	FCookedSet& Registered = CookedBySet.FindOrAdd(Set.GetPathName());
	Registered.ModelHash = SetHash;
	Registered.Name = SetName;
	TArray<uint64>& Ids = Registered.Ids;
	// A Text entry whose ids are not its text's under the set's model (TextChanged, §5.8 *Staleness*) is skipped:
	// it has no spec and is not served. A cooked set carries such an entry with no bytes; an uncooked one may still
	// carry the bytes of its old ids, and they never register.
	const std::vector<FPrefixSetEntry> Entries = SuperSLMPrefixSetBridge::CoreEntries(Set);
	for (const size_t Stale : PrefixSetStaleTextEntries(Entries, SetHash))
	{
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache: %s: entry '%s' is TextChanged (its ids are not its text's under the set's model); not registered"),
			*Set.GetPathName(), UTF8_TO_TCHAR(Entries[Stale].Name.c_str()));
	}
	for (const FPrefixSetRegistration& R : PrefixSetRegistrations(Build, Entries, SetHash))
	{
		const std::vector<uint8_t>& W = Build.Entries[R.BuiltEntryIndex].Wrapper;
		const uint64 Id = NextCookedBlobId++;
		{
			FScopeLock Lock(&CookedGuard);
			CookedBlobs.Add(Id, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>(MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(W.data(), static_cast<int32>(W.size()))));
		}
		Core().RegisterCooked(R.Key, Id);
		Ids.Add(Id);
	}
	return Ids.Num();
}

void FSuperSLMPrefixCacheHost::UnregisterCookedSet(const USuperSLMPrefixSet& Set)
{
	FCookedSet Gone;
	if (CookedBySet.RemoveAndCopyValue(Set.GetPathName(), Gone))
	{
		UnregisterCookedIds(Gone.Ids);
	}
}

void FSuperSLMPrefixCacheHost::UnregisterCookedIds(const TArray<uint64>& Ids)
{
	for (const uint64 Id : Ids)
	{
		Core().UnregisterCooked(Id);
		FScopeLock Lock(&CookedGuard);
		CookedBlobs.Remove(Id); // a check already running keeps its own reference
	}
}

int32 FSuperSLMPrefixCacheHost::CookedEntriesRegistered() const
{
	FScopeLock Lock(&CookedGuard);
	return CookedBlobs.Num();
}

void FSuperSLMPrefixCacheHost::RunWriteDisk(const FPrefixCommand& Command, FPrefixCompletion& Out)
{
	CheckOffGameThread();
	TArray<uint8> Payload;
	if (!TakePayloadCopy(Command.PayloadRef, Payload))
	{
		Out.Io = EPrefixIoStatus::Error;
		return;
	}
	DropPayload(Command.PayloadRef); // WriteDisk owns the bytes
	const std::vector<uint8_t> Wrapper = EncodePrefixWrapper(Config.Identity, Command.Key, Command.Spec, Payload.GetData(), static_cast<size_t>(Payload.Num()));
	FPrefixFileOps Files(*this, /*bWriteBack*/ true);
	const bool bWritten = WritePrefixFileAtomically(Files, Config.DiskDirectory, Command.Key, Config.InstanceId, Command.WriteSerial, Wrapper, EPrefixWriteFault::None);
	Out.Io = bWritten ? EPrefixIoStatus::Ok : EPrefixIoStatus::Error; // the core emits the trim after it
}

// ---- Lane jobs (Layer 1) ----

FSuperSLMPrefixLaneJob FSuperSLMPrefixCacheHost::MakeLaneJob(const FPrefixCommand& Command)
{
	FSuperSLMPrefixLaneJob Job;
	Job.Completion = MakeShared<FPrefixCompletion, ESPMode::ThreadSafe>();
	Job.Completion->CommandId = Command.Id;
	Job.Completion->Generation = Command.Generation;
	switch (Command.Kind)
	{
	case EPrefixCommandKind::Build:
	{
		// The begin job of a sliced build (§5.3 item 4, rev 1.18.8): begin and bind here; the prefill runs in chunk
		// jobs (MakeChunkJob), issued one at a time at each delivery (DeliverLaneJob) and planned and priced as
		// CreatePrefix's prefills are (the subsystem's DispatchLaneJobs). The last chunk's job freezes.
		Job.Kind = ESuperSLMPrefixLaneKind::Build;
		TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe> Run =
			MakeShared<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>(static_cast<int32>(Command.Spec.Tokens.size()), Command.OriginLength);
		Run->Tokens = TArray<int32>(Command.Spec.Tokens.data(), static_cast<int32>(Command.Spec.Tokens.size()));
		Run->Schema = UTF8_TO_TCHAR(Command.Spec.SchemaName.c_str());
		Run->BeginLength = Command.ContextLength; // a nested build: the suffix's budget
		Run->Parent = Command.ParentEngineHandle;
		Run->EntryId = Command.EntryId;
		Run->Completion = Job.Completion;
		Job.BuildRun = Run;
#if WITH_DEV_AUTOMATION_TESTS
		NoteBuildState(Run->EntryId, ESuperSLMPrefixBuildState::Queued); // emitted: the core has charged it
#endif
		Job.Run = [this, Run](const FSuperSLMPrefixLaneContext& Lane, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			const uint64 EntryId = Run->EntryId;
			(void)EntryId;
			sslm_prefix Prefix = nullptr;
			// Both modes build a budgeted root (charged R(L) by the core): the legacy pool is a page
			// pool at 1.13.0+ as well, and a legacy sslm_prefix_begin would draw a whole block. A
			// nested build (budget mode only, U6) continues its parent, which the core keeps cached
			// and pinned until the build's completion is applied; it binds no schema of its own.
#if WITH_DEV_AUTOMATION_TESTS
			// Paging's Layer-1 override (§3.9), taken for this job when it was parked: the begin is not called and
			// its status is the forced one, so nothing is drawn.
			if (Lane.ForcedLayer1Status != static_cast<int32>(SSLM_OK))
			{
				ForgetBuildState(EntryId);
				Run->Slices.OnFailed();
				Out->Engine = EngineResultOf(static_cast<sslm_status>(Lane.ForcedLayer1Status));
				if (Out->Engine == EPrefixEngineResult::Refused)
				{
					Out->Engine = EPrefixEngineResult::Failed; // a build's refusal is not a miss
				}
				Out->EngineStatus = Lane.ForcedLayer1Status;
				// The build hold's count is taken at post, so it is given back on every path out, this one too: a
				// forced build never began, so it never stops, but without the pass its lane stayed counted as held
				// (never free for planning, and the barrier read it as parked) for the rest of the configuration.
				// Box 2026-10-21 review, finding 1.
				PassBuildHold(Lane, /*bBegun*/ false);
				return;
			}
#endif
			sslm_status Status = Run->Parent != 0
				? sslm_prefix_begin_from(HandleOf(Run->Parent), FMath::Max(Run->BeginLength, 1), &Prefix)
				: sslm_prefix_begin_budgeted(Lane.Model, Lane.Pool, FMath::Max(Run->BeginLength, 1), &Prefix);
#if WITH_DEV_AUTOMATION_TESTS
			// The build hold (§8, rev 1.18, PC-15 Q7): checked once, after begin returns and before any further
			// Layer-1 call (the schema bind, the first prefill). A held job has drawn its reserve and calls nothing.
			// It stays in the begin job when the build is sliced (rev 1.18.8), so 8.10 (c)'s reading is unchanged.
			if (Status == SSLM_OK)
			{
				NoteBuildState(EntryId, ESuperSLMPrefixBuildState::Begun);
			}
			PassBuildHold(Lane, Status == SSLM_OK);
#endif
			if (Status == SSLM_OK && !Run->Schema.IsEmpty())
			{
				sslm_schema Bound = SSLM_SCHEMA_NONE;
				Status = sslm_schema_lookup(Lane.Model, TCHAR_TO_UTF8(*Run->Schema), &Bound);
				if (Status != SSLM_OK)
				{
					sslm_prefix_release(Prefix);
					Run->Slices.OnFailed();
					Out->Engine = Status == SSLM_SCHEMA_NOT_FOUND ? EPrefixEngineResult::UnknownSchema : EPrefixEngineResult::Failed;
					Out->EngineStatus = static_cast<int32_t>(Status);
#if WITH_DEV_AUTOMATION_TESTS
					ForgetBuildState(EntryId);
#endif
					return;
				}
				Status = sslm_prefix_set_schema(Prefix, Bound);
			}
			if (Status == SSLM_OK)
			{
				Run->Slices.OnBegun(HandleBits(Prefix));
				if (Run->Tokens.Num() > 0)
				{
					return; // the chunk jobs follow; the build's completion is theirs
				}
				Status = sslm_prefix_freeze(Prefix); // nothing to prefill: this job is also the last
				if (Status == SSLM_OK)
				{
					Run->Slices.OnFrozen();
				}
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (Status == SSLM_OK)
			{
				NoteBuildState(EntryId, ESuperSLMPrefixBuildState::Frozen);
			}
			else
			{
				ForgetBuildState(EntryId);
			}
#endif
			if (Status != SSLM_OK)
			{
				if (Prefix != nullptr)
				{
					sslm_prefix_release(Prefix);
					Prefix = nullptr;
				}
				Run->Slices.OnFailed();
			}
			Out->Engine = EngineResultOf(Status);
			if (Out->Engine == EPrefixEngineResult::Refused)
			{
				Out->Engine = EPrefixEngineResult::Failed; // a build's refusal is not a miss
			}
			Out->EngineStatus = static_cast<int32_t>(Status);
			Out->EngineHandle = HandleBits(Prefix);
		};
		break;
	}
	case EPrefixCommandKind::Restore:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Restore;
		const uint64 Ref = Command.PayloadRef;
		Job.PayloadBytes = PayloadBytesOf(Ref); // its price (§5.5)
		// A nested restore's sharing hint (U6): the parent, when the core kept it cached; never a
		// requirement (the core charged ceil(L/B) before the draw, as if private).
		uint64 Hint = Command.ParentEngineHandle;
#if WITH_DEV_AUTOMATION_TESTS
		if (Test.bNullRestoreHints)
		{
			Hint = 0; // cell 8.12: the hint fails
		}
#endif
		Job.Run = [this, Ref, Hint](const FSuperSLMPrefixLaneContext& Lane, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			TArray<uint8> Bytes;
			const bool bHave = TakePayloadCopy(Ref, Bytes);
			DropPayload(Ref); // the Restore owns the bytes
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
			sslm_prefix Prefix = nullptr;
			uint32_t Shared = 0;
#if WITH_DEV_AUTOMATION_TESTS
			// Paging's Layer-1 override (§3.9) in place of the restore call: nothing is drawn.
			const sslm_status Forced = static_cast<sslm_status>(Lane.ForcedLayer1Status);
#else
			const sslm_status Forced = SSLM_OK;
#endif
			const sslm_status Status = Forced != SSLM_OK ? Forced
				: bHave
				? sslm_prefix_restore(Lane.Model, Lane.Pool, Bytes.GetData(), static_cast<size_t>(Bytes.Num()), HandleOf(Hint), &Prefix, &Shared)
				: SSLM_INVALID_ARGUMENT;
			Out->Engine = EngineResultOf(Status);
			Out->EngineStatus = static_cast<int32_t>(Status);
			Out->EngineHandle = Status == SSLM_OK ? HandleBits(Prefix) : 0;
			Out->SharedPages = Shared;
#else
			// Layer 1 before 1.14.0 has no prefix restore: every restore is a refusal, so the
			// core falls through to recompute.
			(void)Lane;
			(void)bHave;
			(void)Hint;
			Out->Engine = EPrefixEngineResult::Failed;
			Out->EngineStatus = -1; // no engine call was made
#endif
		};
		break;
	}
	case EPrefixCommandKind::Save:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Save;
		const uint64 Engine = Command.EngineHandle;
		Job.Run = [this, Engine](const FSuperSLMPrefixLaneContext& Lane, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			(void)Lane;
#if defined(SSLM_HAS_PREFIX_SAVE_ABI)
			size_t Size = 0;
			sslm_status Status = sslm_prefix_save(HandleOf(Engine), nullptr, &Size);
			TArray<uint8> Bytes;
			if (Status == SSLM_BUFFER_TOO_SMALL)
			{
				Bytes.SetNumUninitialized(static_cast<int32>(Size));
				Status = sslm_prefix_save(HandleOf(Engine), Bytes.GetData(), &Size);
				Bytes.SetNum(static_cast<int32>(Size));
			}
			Out->Engine = EngineResultOf(Status);
			Out->EngineStatus = static_cast<int32_t>(Status);
			if (Status == SSLM_OK)
			{
				Out->PayloadRef = StorePayload(MoveTemp(Bytes));
			}
#else
			(void)Engine;
			Out->Engine = EPrefixEngineResult::Failed; // no write-back without the 1.14.0 save verb
			Out->EngineStatus = -1;
#endif
		};
		break;
	}
	case EPrefixCommandKind::Release:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Release;
		const uint64 Engine = Command.EngineHandle;
		Job.ReleasedHandle = Engine;
		Job.Run = [this, Engine](const FSuperSLMPrefixLaneContext& Lane, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			(void)Lane;
			// sslm_prefix_release consumes the handle on every status (§5.4, rev 1.18.1): a non-OK release has applied.
			sslm_status Status = sslm_prefix_release(HandleOf(Engine));
#if WITH_DEV_AUTOMATION_TESTS
			// The release-status substitution (§8, cell 5.15): the real release ran; its status is replaced.
			const int32 Substitute = Test.SubstitutedReleaseStatus.load();
			if (Substitute != static_cast<int32>(SSLM_OK))
			{
				Status = static_cast<sslm_status>(Substitute);
			}
#endif
			Out->Engine = Status == SSLM_OK ? EPrefixEngineResult::Ok : EPrefixEngineResult::Failed;
			Out->EngineStatus = static_cast<int32_t>(Status);
		};
		break;
	}
	default:
		checkNoEntry();
	}
	return Job;
}

FSuperSLMPrefixLaneJob FSuperSLMPrefixCacheHost::TakeLaneJob()
{
	check(LaneJobs.Num() > 0);
	FSuperSLMPrefixLaneJob Job = MoveTemp(LaneJobs[0]);
	LaneJobs.RemoveAt(0);
	return Job;
}

FSuperSLMPrefixLaneJob FSuperSLMPrefixCacheHost::TakeChunkJob(int32 Index)
{
	check(ChunkJobs.IsValidIndex(Index));
	FSuperSLMPrefixLaneJob Job = MoveTemp(ChunkJobs[Index]);
	ChunkJobs.RemoveAt(Index);
	return Job;
}

FSuperSLMPrefixLaneJob FSuperSLMPrefixCacheHost::MakeChunkJob(const TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>& Run)
{
	FSuperSLMPrefixLaneJob Job;
	Job.Kind = ESuperSLMPrefixLaneKind::Build;
	Job.bBuildChunk = true;
	Job.BuildRun = Run;
	Job.Completion = Run->Completion; // the build's one completion: written by the job that finishes it
	Job.Run = [this, Run](const FSuperSLMPrefixLaneContext& Lane, FPrefixCompletion* Out)
	{
		CheckOffGameThread();
		const uint64 EntryId = Run->EntryId;
		(void)EntryId;
		sslm_prefix Prefix = HandleOf(Run->Slices.GetHandle());
		const int32 Done = Run->Slices.GetDone();
		const int32 Left = Run->Tokens.Num() - Done;
		// One call of the planned chunk, as DispatchPrefixPrefill makes it: every token left offered, the chunk the
		// planner chose consumed.
		int32 Consumed = 0;
		sslm_status Status = sslm_prefix_prefill(Lane.Model, Prefix, Run->Tokens.GetData() + Done, Left, FMath::Max(1, Run->Next.Tokens),
			SSLM_SPAN_PROMPT, Lane.Workspace, &Consumed);
		NotePrefixPrefills(1, !Run->bBuildCounted); // §15.26: one call per chunk, one build per build
		Run->bBuildCounted = true;
		bool bFinished = false;
		if (Status == SSLM_OK)
		{
			if (Run->Slices.OnPrefilled(Consumed))
			{
				// The build's last chunk: this job freezes, and no other does.
				bFinished = true;
				Status = sslm_prefix_freeze(Prefix);
				if (Status == SSLM_OK)
				{
					Run->Slices.OnFrozen();
				}
			}
			else if (Run->Slices.MadeNoProgress())
			{
				Status = SSLM_INVALID_ARGUMENT; // no progress: never loop
			}
		}
		if (Status != SSLM_OK)
		{
			// The build fails here: the job releases what it began, and its completion is the build's.
			sslm_prefix_release(Prefix);
			Prefix = nullptr;
			Run->Slices.OnFailed();
			bFinished = true;
		}
		if (!bFinished)
		{
			return; // a further chunk follows (issued at this job's delivery)
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (Status == SSLM_OK)
		{
			NoteBuildState(EntryId, ESuperSLMPrefixBuildState::Frozen);
		}
		else
		{
			ForgetBuildState(EntryId);
		}
#endif
		Out->Engine = EngineResultOf(Status);
		if (Out->Engine == EPrefixEngineResult::Refused)
		{
			Out->Engine = EPrefixEngineResult::Failed; // a build's refusal is not a miss
		}
		Out->EngineStatus = static_cast<int32_t>(Status);
		Out->EngineHandle = HandleBits(Prefix);
	};
	return Job;
}

void FSuperSLMPrefixCacheHost::DeliverLaneJob(const FSuperSLMPrefixLaneJob& Job)
{
	// A sliced CPU build (§5.3 item 4, rev 1.18.8): a begin or chunk job that did not finish the build carries no
	// completion. Its delivery issues the next chunk job, never while tearing down (cell 1.7): the build then stays
	// open, unfrozen, and the teardown releases it once (ReleaseForPoolDestroy).
	if (Job.BuildRun.IsValid())
	{
		const TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>& Run = Job.BuildRun;
		if (!Job.bBuildChunk && Run->Slices.GetState() == SuperSLMCore::EBuildSliceState::Prefilling)
		{
			OpenBuilds.Add(Run);
			MarkHandleMinted(Run->Slices.GetHandle()); // the release-counting seam counts this handle's teardown release
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (Job.bBuildChunk)
		{
			Test.ChunkJobsByEntry.FindOrAdd(Run->EntryId) += 1;
		}
#endif
		if (!Run->Slices.IsFinished())
		{
			if (Run->Slices.WantsNextChunk(bTearingDown))
			{
#if WITH_DEV_AUTOMATION_TESTS
				// Cell 1.7's witness: a chunk issued at a delivery under the teardown (must stay 0).
				Test.ChunkJobsIssuedInTeardown += bTearingDown ? 1 : 0;
#endif
				ChunkJobs.Add(MakeChunkJob(Run));
			}
			return;
		}
		OpenBuilds.Remove(Run);
	}
	const FPrefixCompletion& Done = *Job.Completion;
	if ((Job.Kind == ESuperSLMPrefixLaneKind::Build || Job.Kind == ESuperSLMPrefixLaneKind::Restore) && Done.Engine == EPrefixEngineResult::Ok && Done.EngineHandle != 0)
	{
		LiveEngineHandles.Add(Done.EngineHandle);
		MarkHandleMinted(Done.EngineHandle);
#if WITH_DEV_AUTOMATION_TESTS
		if (Job.Kind == ESuperSLMPrefixLaneKind::Restore)
		{
			Test.SharedPagesByHandle.Add(Done.EngineHandle, static_cast<int64>(Done.SharedPages));
		}
#endif
	}
	if (Job.Kind == ESuperSLMPrefixLaneKind::Release)
	{
		// A release that returned has applied, whatever its status (the engine consumed the handle), except a GPU
		// release that returned SSLM_BUSY: that handle stays live, and the core issues it again (§5.4, rev 1.18.1).
		// A release the submission thread skipped never reached the engine: its handle stays live too, for the
		// teardown sweep, and the core does not issue it again (§5.4, rev 1.18.5). The handle left the live set at
		// issue (StartCommand), so those two add it back; neither freed the address, so it cannot have been minted
		// again meanwhile. CPU releases are neither.
		if (Done.Engine == EPrefixEngineResult::Busy || Done.Engine == EPrefixEngineResult::Skipped)
		{
			LiveEngineHandles.Add(Job.ReleasedHandle);
		}
		else
		{
			NoteJobRelease(Job);
		}
	}
	// Applied only: Tick()'s Pump() after every delivery takes the commands, so waiters admitted by
	// several deliveries in one apply pass are enqueued in arrival order (§5.11 (a)).
	ApplyCompletion(Done);
}

bool FSuperSLMPrefixCacheHost::ReleaseEngineHandleNow(uint64 Handle, int32& OutStatus)
{
	const sslm_status Status = sslm_prefix_release(HandleOf(Handle));
	OutStatus = static_cast<int32>(Status);
	return Status == SSLM_OK;
}

void FSuperSLMPrefixCacheHost::RunOffGameThreadAndWait(TFunction<void()> Work)
{
	// A thread-pool future, not a UE::Tasks task: a task's Wait() may retract it and run it inline
	// on this (the game) thread, which CheckOffGameThread() refuses.
	Async(EAsyncExecution::ThreadPool, MoveTemp(Work)).Wait();
}

void FSuperSLMPrefixCacheHost::MarkHandleMinted(uint64 Bits)
{
#if WITH_DEV_AUTOMATION_TESTS
	// The release-counting seam counts releases of one minting of an address: an address the allocator hands
	// out again is a new handle, so its count restarts here rather than at Configure() (cells 1.7, 2.10).
	Test.ReleasesByHandle.Remove(Bits);
	Test.MintByHandle.Add(Bits, ++Test.NextMint);
#else
	(void)Bits;
#endif
}

void FSuperSLMPrefixCacheHost::NoteJobRelease(const FSuperSLMPrefixLaneJob& Job)
{
#if WITH_DEV_AUTOMATION_TESTS
	// A release delivered after its address was minted again (and that minting delivered first) released the old
	// handle, not the new one: counting it would read the new handle as released (review finding 2).
	if (Test.MintByHandle.FindRef(Job.ReleasedHandle) == Job.ReleasedMint)
	{
		NoteReleasedHandle(Job.ReleasedHandle);
	}
#else
	(void)Job;
#endif
}

void FSuperSLMPrefixCacheHost::NoteReleasedHandle(uint64 Bits)
{
#if WITH_DEV_AUTOMATION_TESTS
	Test.ReleasesByHandle.FindOrAdd(Bits) += 1;
	Test.LegacyLive.Remove(Bits);
	Test.SharedPagesByHandle.Remove(Bits); // the address may come back for a new entry
#else
	(void)Bits;
#endif
}

void FSuperSLMPrefixCacheHost::ReleaseForPoolDestroy(const FSuperSLMPrefixLaneContext& Lane)
{
	// 1. Pool-thread IO. The test holds yield to teardown first (§8: as the park yields to the stop request).
#if WITH_DEV_AUTOMATION_TESTS
	ReleaseTestHolds();
#endif
	while (InFlightIo.load(std::memory_order_acquire) > 0)
	{
		FPlatformProcess::Sleep(0.001f);
	}
	if (!CacheCore.IsConfigured())
	{
		return;
	}

	// 1a. A drop's delete is drained, never cancelled, under this configuration (§5.4 rev 1.18.5, §5.6
	// *Generations and teardown*): apply the IO completions as an apply pass, then run disk work until no drop
	// delete is pending. A write-back's save no lane has run, which a delete may wait on, completes without
	// running (Skipped) and is applied in the next round; other lane jobs join step 2. Bounded; a delete still
	// pending after it is never carried into the next Configure(): Abandon() resolves it Failed, logged.
	for (int32 Round = 0; Round < 64; ++Round)
	{
		FPrefixCompletion Done;
		while (Inbox.Dequeue(Done))
		{
			ApplyCompletion(Done);
		}
		Core().EndApplyPass();
		if (Core().PendingDropDeletes() == 0)
		{
			break;
		}
		for (const FPrefixCommand& Command : Core().TakeCommands())
		{
			StartCommand(Command);
		}
		StartNextDiskWork();
		while (InFlightIo.load(std::memory_order_acquire) > 0)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		for (int32 J = LaneJobs.Num() - 1; J >= 0; --J)
		{
			if (LaneJobs[J].Kind == ESuperSLMPrefixLaneKind::Save)
			{
				FPrefixCompletion Skipped = *LaneJobs[J].Completion;
				Skipped.Engine = EPrefixEngineResult::Skipped; // never ran: no write-back, nothing to log
				Skipped.EngineStatus = -1;
				Inbox.Enqueue(Skipped);
				LaneJobs.RemoveAt(J);
			}
		}
	}

	// 2. Lane jobs never posted, held releases included: run off the game thread; completions to
	// the inbox, unapplied.
	TArray<FSuperSLMPrefixLaneJob> Pending = MoveTemp(LaneJobs);
	LaneJobs.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	Pending.Append(MoveTemp(Test.HeldReleases));
	Test.HeldReleases.Reset();
	Test.bHoldReleases = false;
#endif
	// Each release left the live set when it was issued (StartCommand); one that returned Busy did not release, so
	// its handle goes back for step 3's sweep.
	TArray<int32> Returned;
	TArray<uint64> NotReleased;
	RunOffGameThreadAndWait([this, &Pending, &Lane, &Returned, &NotReleased]()
	{
		for (int32 Index = 0; Index < Pending.Num(); ++Index)
		{
			FSuperSLMPrefixLaneJob& Job = Pending[Index];
			if (Job.Kind == ESuperSLMPrefixLaneKind::Release)
			{
				Job.Run(Lane, Job.Completion.Get());
				if (Job.Completion->Engine != EPrefixEngineResult::Busy) // returned: applied (§5.4, rev 1.18.1)
				{
					Returned.Add(Index);
				}
				else
				{
					NotReleased.Add(Job.ReleasedHandle);
				}
			}
			else
			{
				Job.Completion->Engine = EPrefixEngineResult::Failed; // never ran: the pool is going
				Job.Completion->EngineStatus = -1;
			}
			Inbox.Enqueue(*Job.Completion);
		}
	});
	for (int32 Index : Returned)
	{
		NoteJobRelease(Pending[Index]);
	}
	for (uint64 Handle : NotReleased)
	{
		LiveEngineHandles.Add(Handle);
	}

	// 2a. Sliced CPU builds (§5.3 item 4, rev 1.18.8; cell 1.7): no further chunk is issued. A build's next chunk job
	// never runs; every build begun and not frozen hands out its unfrozen handle once, released with step 3's, before
	// the pool is destroyed. Their completions go to the inbox unapplied (the core resolves their requests Cancelled).
	ChunkJobs.Reset();
	TArray<uint64> Unfrozen;
	for (const TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>& Run : OpenBuilds)
	{
		const uint64 Handle = Run->Slices.TakeUnfrozenHandle();
		if (Handle != 0)
		{
			Unfrozen.Add(Handle);
		}
		FPrefixCompletion Cancelled = *Run->Completion;
		Cancelled.Engine = EPrefixEngineResult::Failed; // never finished: the pool is going
		Cancelled.EngineStatus = -1;
		Cancelled.EngineHandle = 0;
		Inbox.Enqueue(Cancelled);
	}
	OpenBuilds.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	Test.UnfrozenBuildReleases += Unfrozen.Num();
#endif

	// 3. Every engine handle the cache still holds (and 2a's unfrozen builds).
	TArray<uint64> Remaining = LiveEngineHandles.Array();
	LiveEngineHandles.Reset();
	Remaining.Append(Unfrozen);
	TArray<int32> Statuses;
	TArray<bool> Done;
	Statuses.SetNum(Remaining.Num());
	Done.SetNum(Remaining.Num());
	// Nested entries (U6) are released in any order: the engine counts a shared page's holders, so a
	// parent's pages stay until the last child mapping them is released.
	RunOffGameThreadAndWait([this, &Remaining, &Statuses, &Done]()
	{
		CheckOffGameThread();
		for (int32 I = Remaining.Num() - 1; I >= 0; --I)
		{
			Done[I] = ReleaseEngineHandleNow(Remaining[I], Statuses[I]);
		}
	});
	for (int32 I = 0; I < Remaining.Num(); ++I)
	{
		if (Done[I])
		{
			NoteReleasedHandle(Remaining[I]);
		}
		else
		{
			UE_LOG(LogSuperSLM, Error, TEXT("PrefixCache teardown: the engine refused a prefix release (status %d)."), Statuses[I]);
		}
	}

	// 4. The core.
	AbandonCore();
}

void FSuperSLMPrefixCacheHost::AbandonCore()
{
	{
		FScopeLock Lock(&PayloadGuard);
		Payloads.Reset();
	}
	LaneJobs.Reset();
	DiskQueues.Reset();
	DiskKeysInFlight.Reset();
	TrimQueue.Reset();
	DiskCommandKey.Reset();
	bTrimInFlight = false;
	Core().Abandon();
	FlushLog();
}

void FSuperSLMPrefixCacheHost::FlushLog()
{
	for (const FPrefixLogLine& Line : CacheCore.TakeLog())
	{
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache fallback %s: %s"), UTF8_TO_TCHAR(PrefixFallbackName(Line.Reason)), UTF8_TO_TCHAR(Line.Message.c_str()));
	}
}

#if WITH_DEV_AUTOMATION_TESTS

void FSuperSLMPrefixCacheHost::SetHoldReleases(bool bHold)
{
	Test.bHoldReleases = bHold;
	if (!bHold)
	{
		// Releasing the gate dispatches every held release, in eviction-decision order (the seam's contract,
		// SuperSLMPrefixCacheTestSeam.h). Box 2026-10-21: clearing the flag alone left them held for good, so
		// every cell that opened the gate and then waited on a waiter's admission hung (Ledger.Waiters,
		// Nested.Chain, Nested.GhostClaimable, Nested.ParentAfterChild).
		LaneJobs.Append(MoveTemp(Test.HeldReleases));
		Test.HeldReleases.Reset();
	}
}

void FSuperSLMPrefixCacheHost::DispatchHeldRelease(int32 Index)
{
	if (Test.HeldReleases.IsValidIndex(Index))
	{
		LaneJobs.Add(MoveTemp(Test.HeldReleases[Index]));
		Test.HeldReleases.RemoveAt(Index);
	}
}

FString FSuperSLMPrefixCacheHost::DiskFileOf(const FSuperSLMPrefixSpec& Spec) const
{
	return DiskFileOfWithAdapter(Spec, Config.Identity.AdapterVendorId, Config.Identity.AdapterDeviceId);
}

FString FSuperSLMPrefixCacheHost::DiskFileOfWithAdapter(const FSuperSLMPrefixSpec& Spec, uint32 VendorId, uint32 DeviceId) const
{
	if (!Config.bDiskEnabled)
	{
		return FString();
	}
	FPrefixKeyInputs Inputs;
	Inputs.ArtifactHash = Config.Identity.ArtifactHash;
	Inputs.Layer1Tag = Config.Identity.Layer1Tag;
	Inputs.Layer1Commit = Config.Identity.Layer1Commit;
	if (Spec.Parent.IsValid())
	{
		// A nested spec's key hashes its parent's (U6, cell 2.12); a parent that is no Ready lease has
		// no key, and the spec is refused at the request.
		FPrefixKey ParentKey;
		if (!Core().GetLeaseKey(Spec.Parent.Id, ParentKey))
		{
			return FString();
		}
		Inputs.bHasParent = true;
		Inputs.ParentKey = ParentKey;
	}
	Inputs.Backend = Config.Identity.Backend;
	Inputs.AdapterVendorId = VendorId;
	Inputs.AdapterDeviceId = DeviceId;
	Inputs.SchemaName = Spec.SchemaName.IsNone() ? std::string() : std::string(TCHAR_TO_UTF8(*Spec.SchemaName.ToString()));
	Inputs.Tokens.assign(Spec.Tokens.GetData(), Spec.Tokens.GetData() + Spec.Tokens.Num());
	return FPaths::Combine(UTF8_TO_TCHAR(Config.DiskDirectory.c_str()), UTF8_TO_TCHAR(PrefixDiskFileName(ComputePrefixKey(Inputs)).c_str()));
}

uint64 FSuperSLMPrefixCacheHost::EngineHandleOf(const FSuperSLMPrefix& Lease)
{
	uint64_t Engine = 0;
	uint64_t Token = 0;
	if (Core().BeginAdopt(Lease.Id, Engine, Token) == EPrefixAdoptCheck::Adoptable)
	{
		Core().EndAdopt(Token);
		return Engine;
	}
	return 0;
}

void FSuperSLMPrefixCacheHost::NoteBuildState(uint64 EntryId, ESuperSLMPrefixBuildState State)
{
	if (EntryId != 0)
	{
		FScopeLock Lock(&Test.BuildStateGuard);
		Test.BuildStates.Add(EntryId, State);
	}
}

void FSuperSLMPrefixCacheHost::ForgetBuildState(uint64 EntryId)
{
	FScopeLock Lock(&Test.BuildStateGuard);
	Test.BuildStates.Remove(EntryId);
}

void FSuperSLMPrefixCacheHost::PassBuildHold(const FSuperSLMPrefixLaneContext& Lane, bool bBegun)
{
	if (!Lane.bBuildHoldCounted)
	{
		return; // posted with the hold disarmed: never stops
	}
	if (bBegun && Test.bBuildHold.load())
	{
		Test.HeldBuilds.fetch_add(1);
		while (Test.bBuildHold.load() && !(Lane.LaneStopRequested != nullptr && Lane.LaneStopRequested->load(std::memory_order_acquire)))
		{
			FPlatformProcess::Sleep(0.0005f);
		}
		Test.HeldBuilds.fetch_sub(1);
	}
	if (Lane.LaneBuildHolds != nullptr)
	{
		Lane.LaneBuildHolds->fetch_sub(1); // past the hold: the lane plans again
	}
}

void FSuperSLMPrefixCacheHost::PassWriteBackHold()
{
	if (!Test.bHoldWriteBacks.load())
	{
		return;
	}
	Test.HeldWriteBacks.fetch_add(1);
	while (Test.bHoldWriteBacks.load())
	{
		FPlatformProcess::Sleep(0.0005f);
	}
	Test.HeldWriteBacks.fetch_sub(1);
}

void FSuperSLMPrefixCacheHost::EnterDiskExclusion()
{
	const int32 Now = Test.DiskExclusion.fetch_add(1) + 1;
	int32 Seen = Test.MaxDiskExclusion.load();
	while (Now > Seen && !Test.MaxDiskExclusion.compare_exchange_weak(Seen, Now))
	{
	}
}

ESuperSLMPrefixBuildState FSuperSLMPrefixCacheHost::BuildStateOf(const FSuperSLMPrefix& Lease) const
{
	const uint64 EntryId = static_cast<uint64>(Core().GetLeaseEntryId(Lease.Id));
	if (EntryId == 0)
	{
		return ESuperSLMPrefixBuildState::Unknown;
	}
	FScopeLock Lock(&Test.BuildStateGuard);
	const ESuperSLMPrefixBuildState* Found = Test.BuildStates.Find(EntryId);
	return Found != nullptr ? *Found : ESuperSLMPrefixBuildState::Unknown;
}

int32 FSuperSLMPrefixCacheHost::ChunkJobsOf(const FSuperSLMPrefix& Lease) const
{
	const uint64 EntryId = static_cast<uint64>(Core().GetLeaseEntryId(Lease.Id));
	return EntryId != 0 ? Test.ChunkJobsByEntry.FindRef(EntryId) : 0;
}

int32 FSuperSLMPrefixCacheHost::InFlightBuildCount() const
{
	// Builds charged and not yet frozen or failed: emitted (Queued) or begun.
	int32 Count = 0;
	FScopeLock Lock(&Test.BuildStateGuard);
	for (const TPair<uint64, ESuperSLMPrefixBuildState>& Pair : Test.BuildStates)
	{
		Count += (Pair.Value == ESuperSLMPrefixBuildState::Queued || Pair.Value == ESuperSLMPrefixBuildState::Begun) ? 1 : 0;
	}
	return Count;
}

void FSuperSLMPrefixCacheHost::ReleaseTestHolds()
{
	Test.ChunkHoldAfter.store(-1); // the sliced build's chunk hold yields to teardown too
	Test.bBuildHold.store(false);
	Test.bHoldWriteBacks.store(false);
	Test.bHoldDiskReads.store(false); // the hash-completion hold does not yield (§8)
}

void FSuperSLMPrefixCacheHost::PassDiskReadHold()
{
	if (!Test.bHoldDiskReads.load())
	{
		return;
	}
	Test.HeldDiskReads.fetch_add(1);
	while (Test.bHoldDiskReads.load())
	{
		FPlatformProcess::Sleep(0.0005f);
	}
	Test.HeldDiskReads.fetch_sub(1);
}

void FSuperSLMPrefixCacheHost::EnterKeyDisk(const FPrefixKey& Key)
{
	FScopeLock Lock(&Test.KeyOverlapGuard);
	const int32 Now = ++Test.KeyDiskInFlight.FindOrAdd(KeyHex(Key));
	if (Now > Test.MaxKeyDiskOverlap.load())
	{
		Test.MaxKeyDiskOverlap.store(Now);
	}
}

void FSuperSLMPrefixCacheHost::LeaveKeyDisk(const FPrefixKey& Key)
{
	FScopeLock Lock(&Test.KeyOverlapGuard);
	const FString Hex = KeyHex(Key);
	if (int32* Count = Test.KeyDiskInFlight.Find(Hex))
	{
		if (--*Count <= 0)
		{
			Test.KeyDiskInFlight.Remove(Hex);
		}
	}
}

bool FSuperSLMPrefixCacheHost::ParkHeldHash(const FPrefixCommand& Command, const FPrefixCompletion& Out)
{
	const FString Schema = UTF8_TO_TCHAR(Command.KeyInputs.SchemaName.c_str());
	FScopeLock Lock(&Test.HashHoldGuard);
	for (const FTestState::FHashHold& Hold : Test.HashHolds)
	{
		if (Hold.Schema == Schema && Hold.Tokens.Num() == static_cast<int32>(Command.KeyInputs.Tokens.size()) &&
			(Hold.Tokens.Num() == 0 || FMemory::Memcmp(Hold.Tokens.GetData(), Command.KeyInputs.Tokens.data(), Hold.Tokens.Num() * sizeof(int32)) == 0))
		{
			Test.HeldHashes.Add(TPair<FTestState::FHashHold, FPrefixCompletion>(Hold, Out));
			return true;
		}
	}
	return false;
}

void FSuperSLMPrefixCacheHost::SetHashHold(const TArray<int32>& Tokens, const FString& Schema, bool bHold)
{
	FScopeLock Lock(&Test.HashHoldGuard);
	auto Same = [&Tokens, &Schema](const FTestState::FHashHold& H) { return H.Schema == Schema && H.Tokens == Tokens; };
	if (bHold)
	{
		if (!Test.HashHolds.ContainsByPredicate(Same))
		{
			Test.HashHolds.Add(FTestState::FHashHold{Tokens, Schema});
		}
		return;
	}
	Test.HashHolds.RemoveAll(Same);
	// Released: the held completions are enqueued now, under the generation they were issued in (a hash held
	// across a re-Configure() is stale and dropped).
	for (int32 I = 0; I < Test.HeldHashes.Num();)
	{
		if (Same(Test.HeldHashes[I].Key))
		{
			Inbox.Enqueue(Test.HeldHashes[I].Value);
			Test.HeldHashes.RemoveAt(I);
			continue;
		}
		++I;
	}
}

bool FSuperSLMPrefixCacheHost::OffGameThreadAssertTrips()
{
	Test.bRecordAsserts.store(true);
	Test.bAssertTripped.store(false);
	FPrefixCommand Probe;
	FPrefixCompletion Out;
	RunComputeKey(Probe, Out); // on the game thread: must trip
	const bool bTripped = Test.bAssertTripped.load();
	Test.bRecordAsserts.store(false);
	return bTripped;
}

bool FSuperSLMPrefixCacheHost::GameThreadAccessorTrips()
{
	Test.bRecordAsserts.store(true);
	Test.bAssertTripped.store(false);
	Async(EAsyncExecution::ThreadPool, [this]() { (void)Core().IsConfigured(); }).Wait(); // never inline on this thread
	const bool bTripped = Test.bAssertTripped.load();
	Test.bRecordAsserts.store(false);
	return bTripped;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // SUPERSLM_WITH_PREFIX_CACHE
