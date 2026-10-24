// The GPU subsystem's prefix-cache glue (declarations and the wiring list in
// SuperSLMGpuSubsystemPrefixCache.h). Included at the end of SuperSLMGpuSubsystem.cpp, only with
// SUPERSLM_WITH_PREFIX_CACHE on, so it sees that file's FSuperSLMGpuSubsystemState and FJob.

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "SuperSLMSaveBlob.h"

#if SUPERSLMUNREAL_WITH_GPU

namespace SuperSLMGpuPrefixCacheGlue
{
	uint32 RegionCPagesFor(int64 BudgetBytes, int64 PageBytes, int64 PagePositions, int64 ContextCap)
	{
		if (BudgetBytes <= 0 || PageBytes <= 0 || PagePositions <= 0 || ContextCap <= 0)
		{
			return 0;
		}
		const int64 BlockPages = (ContextCap + PagePositions - 1) / PagePositions;
		const int64 BlockBytes = BlockPages * PageBytes;
		const int64 Blocks = (BudgetBytes + BlockBytes - 1) / BlockBytes;
		return static_cast<uint32>(FMath::Min<int64>(Blocks * BlockPages, MAX_uint32));
	}

	bool Install(FSuperSLMGpuSubsystemState& S, int64* PrefixIdCounter, const FSuperSLMGpuPrefixCacheSettings& Settings, FString& OutError)
	{
		FState& G = S.PrefixCache;
		G.Settings = Settings;
		if (Settings.BudgetBytes <= 0)
		{
			return true; // no cache
		}
		if (!S.bPooled)
		{
			OutError = TEXT("PrefixCacheRequiresPooled: the GPU prefix cache keeps GPU prefixes, which exist only with pooled sequences");
			return false;
		}
		G.Host = MakeUnique<FSuperSLMGpuPrefixCacheHost>(PrefixIdCounter);

		FSuperSLMGpuPrefixLayer1 L1;
		L1.Ctx = S.Ctx;
		L1.Model = S.Model;
		L1.Pool = S.Pool;
		L1.DispatchBudget = S.ConfiguredDispatchBudget;
		L1.MaxPrefillChunk = 64;
		FSuperSLMGpuSubsystemState* St8 = &S;
		L1.SchemaIndexOf = [St8](const FString& Name) -> int32
		{
			const FTCHARToUTF8 Utf8(*Name);
			size_t Index = 0;
			return St8->Schemas.ByName(std::string_view(Utf8.Get(), Utf8.Length()), &Index) != nullptr ? static_cast<int32>(Index) : -1;
		};
		L1.RunOnSubmissionThread = [St8](TFunction<void()> Work)
		{
			St8->RunSync([Work = MoveTemp(Work)]() { Work(); }, TEXT("prefix cache teardown"));
		};
		L1.NoteStatus = [St8](SslmGpuStatus Status) { St8->NoteLayer1Status(Status); };
		G.Host->SetLayer1(MoveTemp(L1));

		uint32 VendorId = 0, DeviceId = 0;
		FSuperSLMGpuPrefixCacheHost::ResolveAdapterIds(VendorId, DeviceId);
		SuperSLMCore::FPrefixEngineIdentity Base;
		FMemory::Memcpy(Base.ArtifactHash.data(), S.ArtifactHash, 32);
		Base.Layer1Tag = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Tag());
		Base.Layer1Commit = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Commit());

		SuperSLMCore::FPrefixCacheConfig CacheConfig;
		CacheConfig.Identity = FSuperSLMGpuPrefixCacheHost::MakeGpuIdentity(Base, VendorId, DeviceId);
		CacheConfig.PageBytes = static_cast<uint64>(S.PageGeometry.page_bytes);
		CacheConfig.PageTokens = static_cast<uint32>(S.PageGeometry.page_positions);
		CacheConfig.ContextCap = S.Config.ContextCap;
		CacheConfig.VocabSize = S.VocabSize;
		CacheConfig.bDiskEnabled = Settings.bDiskEnabled;
		const FString Directory = Settings.DiskDirectory.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("SuperSLM/PrefixCacheGpu") : Settings.DiskDirectory;
		CacheConfig.DiskDirectory = TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Directory));
		CacheConfig.DiskMaxBytes = static_cast<uint64>(FMath::Max<int64>(0, Settings.DiskMaxBytes));
		CacheConfig.InstanceId = (static_cast<uint64>(FPlatformProcess::GetCurrentProcessId()) << 40) ^ static_cast<uint64>(reinterpret_cast<UPTRINT>(G.Host.Get()));
		CacheConfig.RegionC = &S.Ledger;       // pooled mode is the GPU's budget mode
		CacheConfig.bNestedEntries = true;     // begin_from and the restore's re-share are pooled verbs
		G.RegionCPages = RegionCPagesFor(Settings.BudgetBytes, S.PageGeometry.page_bytes, S.PageGeometry.page_positions, S.Config.ContextCap);
		return G.Host->Configure(CacheConfig, OutError);
	}

	void Pump(FSuperSLMGpuSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return;
		}
		// Deliver finished lane jobs in posting order (the apply pass), then let the core act.
		while (G.Posted.Num() > 0)
		{
			const FJob* Job = static_cast<const FJob*>(G.Posted[0].Ticket.Get());
			if (Job == nullptr || !Job->bDone.load(std::memory_order_acquire))
			{
				break;
			}
			if (Job->bSkipped)
			{
				// Never ran. A skipped release or save is delivered as Skipped, not as a release that returned
				// Failed: the handle stays live for the teardown sweep and its refund pending (§5.4, rev 1.18.5).
				const ESuperSLMPrefixLaneKind Kind = G.Posted[0].Job.Kind;
				const bool bNotApplied = Kind == ESuperSLMPrefixLaneKind::Release || Kind == ESuperSLMPrefixLaneKind::Save;
				G.Posted[0].Job.Completion->Engine = bNotApplied ? SuperSLMCore::EPrefixEngineResult::Skipped : SuperSLMCore::EPrefixEngineResult::Failed;
				G.Posted[0].Job.Completion->EngineStatus = -1;
			}
			G.Host->DeliverLaneJob(G.Posted[0].Job);
			G.Posted.RemoveAt(0);
		}
		G.Host->Pump();
	}

	void DispatchLaneJobs(FSuperSLMGpuSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return;
		}
		const FSuperSLMPrefixLaneContext NoLane; // the GPU jobs read their own Layer-1 handles
		while (G.Host->HasLaneJob())
		{
			FState::FPosted& Posted = G.Posted.AddDefaulted_GetRef();
			Posted.Job = G.Host->TakeLaneJob();
			TSharedPtr<FJob> Job = MakeShared<FJob>();
			Job->Custom = [Run = Posted.Job.Run, Completion = Posted.Job.Completion, NoLane]()
			{
				Run(NoLane, Completion.Get());
			};
			Job->CallKind = TEXT("prefix cache job");
			Posted.Ticket = StaticCastSharedPtr<void>(S.Enqueue(Job));
		}
	}

	void BeforePoolDestroy(FSuperSLMGpuSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return;
		}
		// Every posted job has finished (the submission thread is drained): deliver them, so the
		// handles they produced are known to the host and released below.
		Pump(S);
		G.Host->ReleaseForPoolDestroy(FSuperSLMPrefixLaneContext());
		G.Host.Reset();
		G.Posted.Reset();
	}
}

// ---- USuperSLMGpuSubsystem's prefix-cache methods (declared in SuperSLMGpuSubsystem.h) ----

void USuperSLMGpuSubsystem::ConfigurePrefixCache(const FSuperSLMGpuPrefixCacheSettings& Settings)
{
	PendingPrefixCacheSettings = MakeShared<FSuperSLMGpuPrefixCacheSettings>(Settings);
}

bool USuperSLMGpuSubsystem::RequestPrefix(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& OutPrefix, FString& OutError)
{
	FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (Host == nullptr)
	{
		OutError = TEXT("NotConfigured: the GPU prefix cache is not in this release (it is planned for plugin 1.4.0); the prefix cache is CPU-only in 1.3.0");
		return false;
	}
	return Host->Request(Spec, OutPrefix, OutError);
}

bool USuperSLMGpuSubsystem::ReturnPrefix(const FSuperSLMPrefix& Prefix)
{
	FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr && Host->Return(Prefix);
}

ESuperSLMPrefixSource USuperSLMGpuSubsystem::GetPrefixSource(const FSuperSLMPrefix& Prefix) const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr ? Host->GetSource(Prefix) : ESuperSLMPrefixSource::Invalid;
}

ESuperSLMPrefixLeaseState USuperSLMGpuSubsystem::GetPrefixLeaseState(const FSuperSLMPrefix& Prefix) const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr ? Host->GetLeaseState(Prefix) : ESuperSLMPrefixLeaseState::Invalid;
}

FSuperSLMPrefixCacheStats USuperSLMGpuSubsystem::GetPrefixCacheStats() const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr ? Host->GetStats() : FSuperSLMPrefixCacheStats();
}

// ---- The GPU instance's test seams (SuperSLMPrefixCacheTestSeam.h) ----

#if WITH_DEV_AUTOMATION_TESTS
FSuperSLMGpuPrefixCacheHost* FSuperSLMPrefixCacheTestSeam::GpuCacheHostOf(const USuperSLMGpuSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->PrefixCache.Host.Get() : nullptr;
}

void FSuperSLMPrefixCacheTestSeam::HoldReleases(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bHoldReleases = bHold; }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldReleaseCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldReleases.Num() : 0;
}

int32 FSuperSLMPrefixCacheTestSeam::ReleasesOf(const USuperSLMGpuSubsystem& Subsystem, uint64 EngineHandle)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.ReleasesByHandle.FindRef(EngineHandle) : 0;
}

uint64 FSuperSLMPrefixCacheTestSeam::EngineHandleOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->EngineHandleOf(Lease) : 0;
}

void FSuperSLMPrefixCacheTestSeam::SetNullRestoreHints(USuperSLMGpuSubsystem& Subsystem, bool bNull)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bNullRestoreHints = bNull; }
}

int64 FSuperSLMPrefixCacheTestSeam::SharedPagesOf(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	const uint64 Engine = Host != nullptr ? Host->EngineHandleOf(Lease) : 0;
	const int64* Shared = Engine != 0 ? Host->Test.SharedPagesByHandle.Find(Engine) : nullptr;
	return Shared != nullptr ? *Shared : -1;
}

FString FSuperSLMPrefixCacheTestSeam::DiskFileOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->DiskFileOf(Spec) : FString();
}

FString FSuperSLMPrefixCacheTestSeam::DiskFileOfWithAdapter(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, uint32 VendorId, uint32 DeviceId)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->DiskFileOfWithAdapter(Spec, VendorId, DeviceId) : FString();
}

bool FSuperSLMPrefixCacheTestSeam::AdapterIdsOf(const USuperSLMGpuSubsystem& Subsystem, uint32& OutVendorId, uint32& OutDeviceId)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	if (Host == nullptr || Host->ConfigForTest().Identity.Backend != 1)
	{
		return false;
	}
	OutVendorId = Host->ConfigForTest().Identity.AdapterVendorId;
	OutDeviceId = Host->ConfigForTest().Identity.AdapterDeviceId;
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

#else // SUPERSLMUNREAL_WITH_GPU == 0: no GPU backend, so no GPU cache; the declared surface refuses.

void USuperSLMGpuSubsystem::ConfigurePrefixCache(const FSuperSLMGpuPrefixCacheSettings&) {}

bool USuperSLMGpuSubsystem::RequestPrefix(const FSuperSLMPrefixSpec&, FSuperSLMPrefix&, FString& OutError)
{
	OutError = TEXT("NotConfigured: the GPU backend is built on Windows x64 only");
	return false;
}

bool USuperSLMGpuSubsystem::ReturnPrefix(const FSuperSLMPrefix&) { return false; }
ESuperSLMPrefixSource USuperSLMGpuSubsystem::GetPrefixSource(const FSuperSLMPrefix&) const { return ESuperSLMPrefixSource::Invalid; }
ESuperSLMPrefixLeaseState USuperSLMGpuSubsystem::GetPrefixLeaseState(const FSuperSLMPrefix&) const { return ESuperSLMPrefixLeaseState::Invalid; }
FSuperSLMPrefixCacheStats USuperSLMGpuSubsystem::GetPrefixCacheStats() const { return FSuperSLMPrefixCacheStats(); }

#if WITH_DEV_AUTOMATION_TESTS
FSuperSLMGpuPrefixCacheHost* FSuperSLMPrefixCacheTestSeam::GpuCacheHostOf(const USuperSLMGpuSubsystem&) { return nullptr; }
void FSuperSLMPrefixCacheTestSeam::HoldReleases(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldReleaseCount(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMPrefixCacheTestSeam::ReleasesOf(const USuperSLMGpuSubsystem&, uint64) { return 0; }
uint64 FSuperSLMPrefixCacheTestSeam::EngineHandleOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return 0; }
void FSuperSLMPrefixCacheTestSeam::SetNullRestoreHints(USuperSLMGpuSubsystem&, bool) {}
int64 FSuperSLMPrefixCacheTestSeam::SharedPagesOf(USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return -1; }
FString FSuperSLMPrefixCacheTestSeam::DiskFileOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&) { return FString(); }
FString FSuperSLMPrefixCacheTestSeam::DiskFileOfWithAdapter(const USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&, uint32, uint32) { return FString(); }
bool FSuperSLMPrefixCacheTestSeam::AdapterIdsOf(const USuperSLMGpuSubsystem&, uint32&, uint32&) { return false; }
#endif // WITH_DEV_AUTOMATION_TESTS

#endif // SUPERSLMUNREAL_WITH_GPU
