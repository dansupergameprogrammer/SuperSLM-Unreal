// The GPU subsystem's prefix-cache glue (declarations and the wiring list in
// SuperSLMGpuSubsystemPrefixCache.h). Included at the end of SuperSLMGpuSubsystem.cpp, only with
// SUPERSLM_WITH_PREFIX_CACHE on, so it sees that file's FSuperSLMGpuSubsystemState and FJob.

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "SuperSLMGpuPrefixSetAccess.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMSaveBlob.h"

#if SUPERSLMUNREAL_WITH_GPU

namespace SuperSLMGpuPrefixCacheGlue
{
	// The next residency op id any GPU cache host may issue (game thread): raised as each host ends, carried into the next.
	uint64 GGpuPrefixNextOpId = 1;

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
		G.LeaseLength.Empty();
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
		// FSuperSLMPrefixOp's contract: ids never repeat within the subsystem's lifetime. The GPU host is one per
		// configuration, so the ids carry on from every host before it (the residency churn cell found the repeat).
		G.Host->CarryOpIdsFrom(GGpuPrefixNextOpId);

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
		FSuperSLMGpuPrefixCacheHost::ResolveAdapterIds(S.bAdapterIdsKnown, S.AdapterVendorId, S.AdapterDeviceId, VendorId, DeviceId);
		SuperSLMCore::FPrefixEngineIdentity Base;
		FMemory::Memcpy(Base.ArtifactHash.data(), S.ArtifactHash, 32);
		Base.Layer1Tag = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Tag());
		Base.Layer1Commit = TCHAR_TO_UTF8(*SuperSLMSaveBlob::CompiledLayer1Commit());

		SuperSLMCore::FPrefixCacheConfig CacheConfig;
		CacheConfig.Identity = FSuperSLMGpuPrefixCacheHost::MakeGpuIdentity(Base, VendorId, DeviceId);
		CacheConfig.BudgetBytes = static_cast<uint64>(Settings.BudgetBytes); // as the CPU's (region C then sets the budget)
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

	// Posts (or re-posts, for a stepped build's next step) one lane job on the submission thread.
	static void PostLaneJob(FSuperSLMGpuSubsystemState& S, FState::FPosted& Posted)
	{
		const FSuperSLMPrefixLaneContext NoLane; // the GPU jobs read their own Layer-1 handles
		TSharedPtr<FJob> Job = MakeShared<FJob>();
#if WITH_DEV_AUTOMATION_TESTS
		FState* G = &S.PrefixCache;
		Job->Custom = [Run = Posted.Job.Run, Completion = Posted.Job.Completion, NoLane, G]()
		{
			const double Start = FPlatformTime::Seconds();
			Run(NoLane, Completion.Get());
			const double Took = FPlatformTime::Seconds() - Start;
			FScopeLock Lock(&G->TestJobSecondsGuard);
			G->TestJobSeconds.Add(Took);
		};
		if (G->TestJobBoundSeconds > 0.0)
		{
			Job->BoundSeconds = G->TestJobBoundSeconds;
		}
#else
		Job->Custom = [Run = Posted.Job.Run, Completion = Posted.Job.Completion, NoLane]()
		{
			Run(NoLane, Completion.Get());
		};
#endif
		Job->CallKind = TEXT("prefix cache job");
		Posted.Ticket = StaticCastSharedPtr<void>(S.Enqueue(Job));
		++S.PrefixCache.PostedCount;
	}

	// After a terminal loss or with an unresponsive thread nothing is posted (Q9): Run() would complete it unrun, and a
	// lost device takes no re-issue. The teardown leaves the host as it is (TearDown, wiring edit 4).
	static bool LaneClosed(const FSuperSLMGpuSubsystemState& S)
	{
		return S.bTerminalLoss.load() || S.bUnresponsive.load();
	}

	void Pump(FSuperSLMGpuSubsystemState& S, bool bAbortSteps)
	{
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return;
		}
		// Deliver finished lane jobs (the apply pass), then let the core act. A stepped build (Q7) that owes another step
		// keeps its place and is posted again; the jobs behind it are delivered as they finish, as the CPU's lanes deliver
		// independently (box 2026-10-29, cell 8.17: with in-order delivery an eviction's release and refund waited behind
		// a whole multi-step build). The submission thread runs posted jobs in order, so jobs that are not stepped still
		// finish, and are delivered, in posting order.
		for (int32 Index = 0; Index < G.Posted.Num();)
		{
			const FJob* Job = static_cast<const FJob*>(G.Posted[Index].Ticket.Get());
			if (Job == nullptr || !Job->bDone.load(std::memory_order_acquire))
			{
				++Index; // a re-posted build step still running; finished jobs behind it are delivered past it
				continue;
			}
			FSuperSLMPrefixLaneJob& Lane = G.Posted[Index].Job;
			if (Job->bSkipped)
			{
				// Never ran. A skipped release or save is delivered as Skipped, not as a release that returned
				// Failed: the handle stays live for the teardown sweep and its refund pending (§5.4, rev 1.18.5).
				const ESuperSLMPrefixLaneKind Kind = Lane.Kind;
				const bool bNotApplied = Kind == ESuperSLMPrefixLaneKind::Release || Kind == ESuperSLMPrefixLaneKind::Save;
				Lane.Completion->Engine = bNotApplied ? SuperSLMCore::EPrefixEngineResult::Skipped : SuperSLMCore::EPrefixEngineResult::Failed;
				Lane.Completion->EngineStatus = -1;
			}
			else if (Lane.MoreSteps && Lane.MoreSteps())
			{
				// A stepped build (Q7) owes another step. The teardown ends it where it is instead; a closed lane
				// re-issues nothing (Q9), and the build is delivered failed.
				if (bAbortSteps && !LaneClosed(S))
				{
					S.RunSync([&Lane]() { Lane.AbortSteps(Lane.Completion.Get()); }, TEXT("prefix cache build abort"));
				}
				else if (LaneClosed(S))
				{
					Lane.Completion->Engine = SuperSLMCore::EPrefixEngineResult::Failed;
					Lane.Completion->EngineStatus = -1;
				}
				else
				{
					PostLaneJob(S, G.Posted[Index]);
					++Index; // its delivery waits for its last step; later finished jobs are delivered past it
					continue;
				}
			}
			G.Host->DeliverLaneJob(Lane);
			G.Posted.RemoveAt(Index);
		}
		G.Host->Pump();
	}

	void DispatchLaneJobs(FSuperSLMGpuSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		if (!G.Host || LaneClosed(S))
		{
			return;
		}
		while (G.Host->HasLaneJob())
		{
			FState::FPosted& Posted = G.Posted.AddDefaulted_GetRef();
			Posted.Job = G.Host->TakeLaneJob();
			PostLaneJob(S, Posted);
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	// ShortGpuPool's holders (cell 5.12), released on the submission thread, newest first (RestoreGpuPool, and the
	// teardown before the cache's own releases so the pool destroy finds none).
	static void ReleasePoolShortHolders(FSuperSLMGpuSubsystemState& S)
	{
		if (S.PrefixCache.TestPoolShortHolders.Num() == 0 || LaneClosed(S))
		{
			return;
		}
		FSuperSLMGpuSubsystemState* St = &S;
		S.RunSync([St]()
		{
			TArray<SslmGpuSequenceHandle*>& Holders = St->PrefixCache.TestPoolShortHolders;
			for (int32 I = Holders.Num() - 1; I >= 0; --I)
			{
				St->NoteLayer1Status(sslm_gpu_seq_release(St->Ctx, Holders[I]));
			}
			Holders.Reset();
		}, TEXT("test seam (RestoreGpuPool)"), 120.0);
	}
#endif

	TUniquePtr<FSuperSLMGpuPrefixCacheHost> BeforePoolDestroy(FSuperSLMGpuSubsystemState& S)
	{
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return nullptr;
		}
#if WITH_DEV_AUTOMATION_TESTS
		ReleasePoolShortHolders(S);
#endif
		// Every posted job has finished (the submission thread is drained): deliver them, so the
		// handles they produced are known to the host and released below. A build still owing steps
		// is ended where it is (its begun prefix released) and delivered failed. Marked first, as the CPU's
		// TeardownBegins (code review r2 B1): a lease Pending now reads Cancelled after the next Configure(), not the
		// result its job delivers here.
		G.Host->MarkTeardownBegins();
		Pump(S, /*bAbortSteps*/ true);
		G.Host->ReleaseForPoolDestroy(FSuperSLMPrefixLaneContext());
		GGpuPrefixNextOpId = FMath::Max(GGpuPrefixNextOpId, G.Host->PeekNextOpId());
		TUniquePtr<FSuperSLMGpuPrefixCacheHost> Retired = MoveTemp(G.Host);
		G.Posted.Reset();
		G.LeaseLength.Empty();
		return Retired;
	}

	ECacheAdopt BeginCacheAdopt(FSuperSLMGpuSubsystemState& S, int64 PrefixId, FCacheAdopt& Out, FString& OutError)
	{
		Out = FCacheAdopt();
		FState& G = S.PrefixCache;
		if (!G.Host)
		{
			return ECacheAdopt::NotCacheHandle;
		}
		FSuperSLMPrefix Lease;
		Lease.Id = PrefixId;
		sslm_prefix Handle = nullptr;
		switch (G.Host->BeginAdopt(Lease, Handle, Out.Token, Out.EntryKey, OutError))
		{
		case FSuperSLMPrefixCacheHost::EAdoptLookup::NotCacheHandle: return ECacheAdopt::NotCacheHandle; // the subsystem's own prefix
		case FSuperSLMPrefixCacheHost::EAdoptLookup::NotReady: return ECacheAdopt::NotReady;
		case FSuperSLMPrefixCacheHost::EAdoptLookup::LeaseEnded: return ECacheAdopt::LeaseEnded;
		default: break;
		}
		Out.Engine = static_cast<uint64>(reinterpret_cast<UPTRINT>(Handle));
		Out.Length = G.LeaseLength.FindRef(PrefixId);
		return ECacheAdopt::Adoptable;
	}

	void EndCacheAdopt(FSuperSLMGpuSubsystemState& S, uint64 Token)
	{
		if (Token != 0 && S.PrefixCache.Host)
		{
			S.PrefixCache.Host->EndAdopt(Token);
		}
	}

	bool RefuseRelease(FSuperSLMGpuSubsystemState& S, const FSuperSLMPrefix& Prefix, FString& OutError)
	{
		return S.PrefixCache.Host && S.PrefixCache.Host->RefuseRelease(Prefix, OutError);
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
		// Plugin 1.4.0 (precache U6): the cache exists only once ConfigurePrefixCache() set a budget and a pooled Configure()
		// installed it; the refusal names why not.
		const FString Reason = State != nullptr && !State->PrefixCache.NotConfiguredReason.IsEmpty() ? State->PrefixCache.NotConfiguredReason
			: FString(TEXT("no GPU prefix cache is configured: call ConfigurePrefixCache() with BudgetBytes above 0, then Configure() with SequenceBudget above 0 (pooled sequences)"));
		OutError = FString(TEXT("NotConfigured: ")) + Reason;
		return false;
	}
	if (SuperSLMGpuPrefixCacheGlue::LaneClosed(*State))
	{
		// §5.4 *After a terminal GPU device loss* (Q9): the cache serves nothing more, and a request falls back by name.
		OutError = TEXT("DeviceUnavailable: the GPU device was lost (or its submission thread stopped responding); the prefix cache serves nothing more until Configure()");
		return false;
	}
	if (!Host->Request(Spec, OutPrefix, OutError))
	{
		return false;
	}
	const int32 ParentLength = Spec.Parent.IsValid() ? State->PrefixCache.LeaseLength.FindRef(Spec.Parent.Id) : 0;
	State->PrefixCache.LeaseLength.Add(OutPrefix.Id, ParentLength + Spec.Tokens.Num());
	return true;
}

bool USuperSLMGpuSubsystem::ReturnPrefix(const FSuperSLMPrefix& Prefix)
{
	FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (Host == nullptr || !Host->Return(Prefix))
	{
		return false;
	}
	State->PrefixCache.LeaseLength.Remove(Prefix.Id);
	return true;
}

ESuperSLMPrefixSource USuperSLMGpuSubsystem::GetPrefixSource(const FSuperSLMPrefix& Prefix) const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr ? Host->GetSource(Prefix) : ESuperSLMPrefixSource::Invalid;
}

ESuperSLMPrefixLeaseState USuperSLMGpuSubsystem::GetPrefixLeaseState(const FSuperSLMPrefix& Prefix) const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	const ESuperSLMPrefixLeaseState LeaseState = Host != nullptr ? Host->GetLeaseState(Prefix) : ESuperSLMPrefixLeaseState::Invalid;
	if (LeaseState != ESuperSLMPrefixLeaseState::Invalid || !RetiredPrefixCacheHost.IsValid())
	{
		return LeaseState;
	}
	// A lease of the last ended configuration reads as the CPU cache's does across a re-Configure(): Cancelled when the
	// teardown cancelled it while Pending (§5.11 (a)); any other (a Ready lease included, its handle released) Invalid.
	return RetiredPrefixCacheHost->IsCancelledWhilePending(Prefix)
		? ESuperSLMPrefixLeaseState::Cancelled : ESuperSLMPrefixLeaseState::Invalid;
}

FSuperSLMPrefixCacheStats USuperSLMGpuSubsystem::GetPrefixCacheStats() const
{
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	return Host != nullptr ? Host->GetStats() : FSuperSLMPrefixCacheStats();
}

// Explicit residency control (§5.6, PC-16 (a)), refused where RequestPrefix is: no cache, or a lost device (Q9).
static bool GpuResidencyHostOrRefuse(FSuperSLMGpuSubsystemState* State, FSuperSLMGpuPrefixCacheHost*& OutHost, FString& OutError)
{
	OutHost = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (OutHost == nullptr)
	{
		const FString Reason = State != nullptr && !State->PrefixCache.NotConfiguredReason.IsEmpty() ? State->PrefixCache.NotConfiguredReason
			: FString(TEXT("no GPU prefix cache is configured: call ConfigurePrefixCache() with BudgetBytes above 0, then Configure() with SequenceBudget above 0 (pooled sequences)"));
		OutError = FString(TEXT("NotConfigured: ")) + Reason;
		return false;
	}
	if (SuperSLMGpuPrefixCacheGlue::LaneClosed(*State))
	{
		OutError = TEXT("DeviceUnavailable: the GPU device was lost (or its submission thread stopped responding); the prefix cache serves nothing more until Configure()");
		OutHost = nullptr;
		return false;
	}
	return true;
}

bool USuperSLMGpuSubsystem::WarmPrefix(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	FSuperSLMGpuPrefixCacheHost* Host = nullptr;
	return GpuResidencyHostOrRefuse(State, Host, OutError) && Host->Warm(Spec, OutOp, OutError);
}

bool USuperSLMGpuSubsystem::DropPrefix(const FSuperSLMPrefixSpec& Spec, ESuperSLMPrefixDropScope Scope, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	FSuperSLMGpuPrefixCacheHost* Host = nullptr;
	return GpuResidencyHostOrRefuse(State, Host, OutError) && Host->Drop(Spec, Scope, OutOp, OutError);
}

bool USuperSLMGpuSubsystem::GetPrefixOpResult(const FSuperSLMPrefixOp& Op, FSuperSLMPrefixOpResult& Out) const
{
	Out = FSuperSLMPrefixOpResult();
	const FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (Host != nullptr && Host->GetOpResult(Op, Out))
	{
		return true;
	}
	Out = FSuperSLMPrefixOpResult();
	return RetiredPrefixCacheHost.IsValid() && RetiredPrefixCacheHost->GetOpResult(Op, Out);
}

// ---- The cooked tier (§5.3 item 2, §5.8; precache U7) ----
// The subsystem keeps a copy of each registered set (FSuperSLMCookedSetSource) and registers it with each
// configuration's cache host, whose identity selects the build: only a build of this adapter's exact vendor and device.

void USuperSLMGpuSubsystem::RegisterPrefixSet(USuperSLMPrefixSet* Set)
{
	if (Set == nullptr)
	{
		return;
	}
	if (!CookedPrefixSets.IsValid())
	{
		CookedPrefixSets = MakeShared<FSuperSLMGpuCookedSources>();
	}
	FSuperSLMCookedSetSource Source = FSuperSLMPrefixCacheHost::MakeCookedSetSource(*Set);
	const FString Path = Source.Path;
	FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (Host == nullptr)
	{
		// No GPU cache yet (before the first Configure(), or one that configured none): kept, and registered with its
		// model checked when a Configure() installs a cache (ReRegisterPrefixSets()).
		CookedPrefixSets->ByPath.Add(Path, MoveTemp(Source));
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache (GPU): kept %s; it registers, and its model is checked, when a Configure() installs a GPU prefix cache"), *Path);
		return;
	}
	FSuperSLMPrefixCacheHost::ECookedRegistration Outcome = FSuperSLMPrefixCacheHost::ECookedRegistration::Registered;
	const int32 Registered = Host->RegisterCookedSource(Source, Outcome);
	if (Outcome == FSuperSLMPrefixCacheHost::ECookedRegistration::ModelMismatch)
	{
		CookedPrefixSets->ByPath.Remove(Path); // refused by name (the core's PrefixSetModelMismatch line, once) and forgotten
		return;
	}
	CookedPrefixSets->ByPath.Add(Path, MoveTemp(Source));
	if (Outcome == FSuperSLMPrefixCacheHost::ECookedRegistration::Registered)
	{
		UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache (GPU): registered %d cooked entr%s from %s"), Registered, Registered == 1 ? TEXT("y") : TEXT("ies"), *Path);
	}
	// NoBuild: the host logged its one line (no build for this device); the copy is kept for the next configuration.
}

void USuperSLMGpuSubsystem::UnregisterPrefixSet(USuperSLMPrefixSet* Set)
{
	if (Set == nullptr)
	{
		return;
	}
	const FString Path = Set->GetPathName();
	if (CookedPrefixSets.IsValid())
	{
		CookedPrefixSets->ByPath.Remove(Path);
	}
	if (FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr)
	{
		Host->UnregisterCookedPath(Path);
	}
}

void USuperSLMGpuSubsystem::ReRegisterPrefixSets()
{
	FSuperSLMGpuPrefixCacheHost* Host = State != nullptr ? State->PrefixCache.Host.Get() : nullptr;
	if (Host == nullptr || !CookedPrefixSets.IsValid())
	{
		return;
	}
	// The host's Configure() has run, so each set's model is checked against this configuration's model.
	TArray<FString> Refused;
	for (const TPair<FString, FSuperSLMCookedSetSource>& Pair : CookedPrefixSets->ByPath)
	{
		FSuperSLMPrefixCacheHost::ECookedRegistration Outcome = FSuperSLMPrefixCacheHost::ECookedRegistration::Registered;
		const int32 Registered = Host->RegisterCookedSource(Pair.Value, Outcome);
		if (Outcome == FSuperSLMPrefixCacheHost::ECookedRegistration::ModelMismatch)
		{
			Refused.Add(Pair.Key);
		}
		else if (Outcome == FSuperSLMPrefixCacheHost::ECookedRegistration::Registered)
		{
			UE_LOG(LogSuperSLM, Log, TEXT("PrefixCache (GPU): registered %d cooked entr%s from %s with this configuration's cache"), Registered,
				Registered == 1 ? TEXT("y") : TEXT("ies"), *Pair.Key);
		}
	}
	for (const FString& Path : Refused)
	{
		CookedPrefixSets->ByPath.Remove(Path); // logged once, by the core; register the set again after configuring its model
	}
}

// ---- A cooked prefix set's GPU build (SuperSLMGpuPrefixSetAccess.h; precache U7) ----

bool FSuperSLMGpuPrefixSetAccess::ReadBuildCache(const USuperSLMGpuSubsystem& Gpu, SuperSLMCore::FPrefixEngineIdentity& OutIdentity, bool& bOutDiskEnabled,
	int32& OutCookedEntries, FString& OutError)
{
	OutIdentity = SuperSLMCore::FPrefixEngineIdentity();
	bOutDiskEnabled = false;
	OutCookedEntries = 0;
	const FSuperSLMGpuSubsystemState* S = Gpu.State;
	const FSuperSLMGpuPrefixCacheHost* Host = S != nullptr ? S->PrefixCache.Host.Get() : nullptr;
	if (Host == nullptr)
	{
		OutError = S != nullptr && !S->PrefixCache.NotConfiguredReason.IsEmpty() ? S->PrefixCache.NotConfiguredReason
			: FString(TEXT("no GPU prefix cache is configured: ConfigurePrefixCache() with BudgetBytes above 0, then Configure() with SequenceBudget above 0 (pooled sequences)"));
		return false;
	}
	if (SuperSLMGpuPrefixCacheGlue::LaneClosed(*S))
	{
		OutError = TEXT("the GPU device was lost (or its submission thread stopped responding)");
		return false;
	}
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	OutIdentity = Host->GetIdentity();
	bOutDiskEnabled = Host->IsDiskEnabled();
	OutCookedEntries = Host->CookedEntriesRegistered();
	return true;
#else
	OutError = TEXT("this build has no GPU prefix save (SUPERSLM_WITH_GPU_PREFIX_SAVE is off)");
	return false;
#endif
}

bool FSuperSLMGpuPrefixSetAccess::SaveLease(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, TArray<uint8>& OutPayload, bool& bOutOnGameThread, FString& OutError)
{
	OutPayload.Reset();
	bOutOnGameThread = false;
	FSuperSLMGpuSubsystemState* S = Gpu.State;
	if (S == nullptr || !S->PrefixCache.Host || SuperSLMGpuPrefixCacheGlue::LaneClosed(*S))
	{
		OutError = TEXT("the GPU subsystem's cache is gone, or its device was lost");
		return false;
	}
	SuperSLMGpuPrefixCacheGlue::FCacheAdopt Pin;
	FString AdoptError;
	const SuperSLMGpuPrefixCacheGlue::ECacheAdopt Lookup = SuperSLMGpuPrefixCacheGlue::BeginCacheAdopt(*S, Lease.Id, Pin, AdoptError);
	if (Lookup != SuperSLMGpuPrefixCacheGlue::ECacheAdopt::Adoptable)
	{
		OutError = FString::Printf(TEXT("the lease's entry could not be pinned for its save (%s)"), AdoptError.IsEmpty() ? TEXT("not Ready") : *AdoptError);
		return false;
	}
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	// Shared with the job, so a submission thread that runs it late (after an unresponsive wait) writes into live memory.
	struct FSaveResult
	{
		SslmGpuStatus Status = SslmGpuStatus::SSLM_OK;
		TArray<uint8> Bytes;
		bool bOnGameThread = false;
		bool bRan = false;
	};
	TSharedRef<FSaveResult, ESPMode::ThreadSafe> Result = MakeShared<FSaveResult, ESPMode::ThreadSafe>();
	FSuperSLMGpuSubsystemState* St8 = S;
	SslmGpuPrefixHandle* Handle = reinterpret_cast<SslmGpuPrefixHandle*>(static_cast<UPTRINT>(Pin.Engine));
	const bool bFinished = S->RunSync([St8, Handle, Result]()
	{
		Result->bOnGameThread = IsInGameThread();
		size_t Size = 0;
		SslmGpuStatus Status = sslm_gpu_prefix_save(St8->Ctx, Handle, nullptr, &Size);
		if (Status == SslmGpuStatus::SSLM_OUTPUT_BUFFER_TOO_SMALL)
		{
			Result->Bytes.SetNumUninitialized(static_cast<int32>(Size));
			Status = sslm_gpu_prefix_save(St8->Ctx, Handle, Result->Bytes.GetData(), &Size);
			Result->Bytes.SetNum(static_cast<int32>(Size));
		}
		St8->NoteLayer1Status(Status);
		Result->Status = Status;
		Result->bRan = true;
	}, TEXT("prefix set build save"));
	SuperSLMGpuPrefixCacheGlue::EndCacheAdopt(*S, Pin.Token);
	bOutOnGameThread = Result->bOnGameThread;
	if (!bFinished || !Result->bRan)
	{
		OutError = TEXT("the submission thread did not run the save");
		return false;
	}
	if (Result->Status != SslmGpuStatus::SSLM_OK || Result->Bytes.Num() == 0)
	{
		OutError = FString::Printf(TEXT("sslm_gpu_prefix_save returned %d"), static_cast<int32>(Result->Status));
		return false;
	}
	OutPayload = MoveTemp(Result->Bytes);
	return true;
#else
	SuperSLMGpuPrefixCacheGlue::EndCacheAdopt(*S, Pin.Token);
	OutError = TEXT("this build has no GPU prefix save (SUPERSLM_WITH_GPU_PREFIX_SAVE is off)");
	return false;
#endif
}

int32 FSuperSLMGpuPrefixSetAccess::ConfiguredSubsystemCount()
{
	FScopeLock Lock(&ConfiguredSubsystemsLock());
	return ConfiguredSubsystems().Num();
}

// ---- The GPU instance's test seams (SuperSLMPrefixCacheTestSeam.h) ----

#if WITH_DEV_AUTOMATION_TESTS
FSuperSLMGpuPrefixCacheHost* FSuperSLMPrefixCacheTestSeam::GpuCacheHostOf(const USuperSLMGpuSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->PrefixCache.Host.Get() : nullptr;
}

void FSuperSLMPrefixCacheTestSeam::HoldReleases(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	// As the CPU's (afe45536d): opening the gate sends every held release on, so a cell that unholds never waits for ever.
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->SetHoldReleases(bHold); }
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

// The rest of the CPU seams, for the GPU instance: each reads its CPU twin's "not built" value with no GPU cache.
void FSuperSLMPrefixCacheTestSeam::SetBuildHold(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bBuildHold.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldBuildCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldBuilds.load() : -1;
}

ESuperSLMPrefixBuildState FSuperSLMPrefixCacheTestSeam::BuildStateOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->BuildStateOf(Lease) : ESuperSLMPrefixBuildState::Unknown;
}

int32 FSuperSLMPrefixCacheTestSeam::InFlightBuildCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->InFlightBuildCount() : -1;
}

void FSuperSLMPrefixCacheTestSeam::SetBuildChunkHold(USuperSLMGpuSubsystem& Subsystem, int32 AfterChunkJobs)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.ChunkHoldAfter.store(AfterChunkJobs < 0 ? -1 : AfterChunkJobs); }
}

int32 FSuperSLMPrefixCacheTestSeam::BuildChunkJobsOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->ChunkJobsOf(Lease) : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::UnfrozenBuildReleases(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.UnfrozenBuildReleases : 0;
}

bool FSuperSLMPrefixCacheTestSeam::DispatchHeldRelease(USuperSLMGpuSubsystem& Subsystem, int32 Index)
{
	FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	if (Host == nullptr || !Host->Test.HeldReleases.IsValidIndex(Index))
	{
		return false;
	}
	Host->DispatchHeldRelease(Index);
	return true;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandle(const USuperSLMGpuSubsystem& Subsystem)
{
	int32 Max = 0;
	if (const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem))
	{
		for (const TPair<uint64, int32>& Pair : Host->Test.ReleasesByHandle) { Max = FMath::Max(Max, Pair.Value); }
	}
	return Max;
}

void FSuperSLMPrefixCacheTestSeam::ForceKeyCollision(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, const FSuperSLMPrefixSpec& CollideWith)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem))
	{
		FSuperSLMPrefixCacheHost::FTestState::FCollision& C = Host->Test.Collisions.AddDefaulted_GetRef();
		C.FromTokens = Spec.Tokens;
		C.FromSchema = Spec.SchemaName.IsNone() ? FString() : Spec.SchemaName.ToString();
		C.ToTokens = CollideWith.Tokens;
		C.ToSchema = CollideWith.SchemaName.IsNone() ? FString() : CollideWith.SchemaName.ToString();
	}
}

void FSuperSLMPrefixCacheTestSeam::SetDiskFault(USuperSLMGpuSubsystem& Subsystem, ESuperSLMPrefixDiskFault Fault)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.DiskFault.store(static_cast<uint8>(Fault)); }
}

void FSuperSLMPrefixCacheTestSeam::SetBeforeDiskRead(USuperSLMGpuSubsystem& Subsystem, TFunction<void(const FString& Path)> Hook)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem))
	{
		FScopeLock Lock(&Host->Test.HookGuard);
		Host->Test.BeforeDiskRead = MoveTemp(Hook);
	}
}

int32 FSuperSLMPrefixCacheTestSeam::DiskIoInFlight(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->DiskIoInFlight() : 0;
}

bool FSuperSLMPrefixCacheTestSeam::IsCacheIoSettled(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host == nullptr || Host->IsIoSettled();
}

void FSuperSLMPrefixCacheTestSeam::HoldWriteBacks(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bHoldWriteBacks.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldWriteBackCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldWriteBacks.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxDiskExclusion(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.MaxDiskExclusion.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::HoldDiskReads(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bHoldDiskReads.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldDiskReadCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldDiskReads.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxKeyDiskOverlap(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.MaxKeyDiskOverlap.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::SetDeleteRefusal(USuperSLMGpuSubsystem& Subsystem, bool bRefuse)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bRefuseDeletes.store(bRefuse); }
}

void FSuperSLMPrefixCacheTestSeam::HoldKeyHash(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem))
	{
		Host->SetHashHold(Spec.Tokens, Spec.SchemaName.IsNone() ? FString() : Spec.SchemaName.ToString(), bHold);
	}
}

void FSuperSLMPrefixCacheTestSeam::SetOffGameThreadRecording(USuperSLMGpuSubsystem& Subsystem, bool bRecord)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bRecordOffGameThread.store(bRecord); }
}

int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadViolations(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.OffGameThreadViolations.load() : -1;
}

int64 FSuperSLMPrefixCacheTestSeam::GameThreadStateAccesses(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.GameThreadStateAccesses.load() : 0;
}

int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadChecks(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.OffGameThreadChecks.load() : 0;
}

bool FSuperSLMPrefixCacheTestSeam::OffGameThreadAssertTrips(USuperSLMGpuSubsystem& Subsystem)
{
	FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr && Host->OffGameThreadAssertTrips();
}

bool FSuperSLMPrefixCacheTestSeam::GameThreadAccessorTrips(USuperSLMGpuSubsystem& Subsystem)
{
	FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr && Host->GameThreadAccessorTrips();
}

void FSuperSLMPrefixCacheTestSeam::SetBusyReleases(USuperSLMGpuSubsystem& Subsystem, int32 Count)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.GpuBusyReleases.store(FMath::Max(Count, 0)); }
}

void FSuperSLMPrefixCacheTestSeam::ForceTerminalLoss(USuperSLMGpuSubsystem& Subsystem)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->bTerminalLoss = true;
	}
}

int64 FSuperSLMPrefixCacheTestSeam::PostedLaneJobs(const USuperSLMGpuSubsystem& Subsystem)
{
	return Subsystem.State != nullptr ? Subsystem.State->PrefixCache.PostedCount : -1;
}

void FSuperSLMPrefixCacheTestSeam::ForceUnresponsive(USuperSLMGpuSubsystem& Subsystem)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->MarkUnresponsive(TEXT("the test seam marked the submission thread unresponsive (ForceUnresponsive)"));
	}
}

void FSuperSLMPrefixCacheTestSeam::SubstituteReleaseStatus(USuperSLMGpuSubsystem& Subsystem, int32 GpuStatus)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.GpuSubstitutedReleaseStatus.store(GpuStatus); }
}

void FSuperSLMPrefixCacheTestSeam::SetNextCacheBuildStatus(USuperSLMGpuSubsystem& Subsystem, int32 GpuStatus)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.GpuNextBuildBeginStatus.store(GpuStatus); }
}

void FSuperSLMPrefixCacheTestSeam::SetRestoreHold(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bGpuRestoreHold.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldRestoreCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldRestores.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::ReleasesOfAtLastTeardown(const USuperSLMGpuSubsystem& Subsystem, uint64 EngineHandle)
{
	const FSuperSLMGpuPrefixCacheHost* Host = Subsystem.RetiredPrefixCacheHost.Get();
	return Host != nullptr ? Host->Test.ReleasesByHandle.FindRef(EngineHandle) : 0;
}

int32 FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandleAtLastTeardown(const USuperSLMGpuSubsystem& Subsystem)
{
	int32 Max = 0;
	if (const FSuperSLMGpuPrefixCacheHost* Host = Subsystem.RetiredPrefixCacheHost.Get())
	{
		for (const TPair<uint64, int32>& Pair : Host->Test.ReleasesByHandle) { Max = FMath::Max(Max, Pair.Value); }
	}
	return Max;
}

int64 FSuperSLMPrefixCacheTestSeam::UnfrozenBuildReleasesAtLastTeardown(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = Subsystem.RetiredPrefixCacheHost.Get();
	return Host != nullptr ? Host->Test.UnfrozenBuildReleases : 0;
}

void FSuperSLMPrefixCacheTestSeam::SetCacheJobBoundSeconds(USuperSLMGpuSubsystem& Subsystem, double Seconds)
{
	if (Subsystem.State != nullptr)
	{
		Subsystem.State->PrefixCache.TestJobBoundSeconds = FMath::Max(Seconds, 0.0);
	}
}

TArray<double> FSuperSLMPrefixCacheTestSeam::CacheJobSeconds(const USuperSLMGpuSubsystem& Subsystem)
{
	if (Subsystem.State == nullptr)
	{
		return TArray<double>();
	}
	FScopeLock Lock(&Subsystem.State->PrefixCache.TestJobSecondsGuard);
	return Subsystem.State->PrefixCache.TestJobSeconds;
}

void FSuperSLMPrefixCacheTestSeam::HoldDropDeletes(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bHoldDropDeletes.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldDropDeleteCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldDropDeletes.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::HoldTrims(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bHoldTrims.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldTrimCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldTrims.load() : -1;
}

int64 FSuperSLMPrefixCacheTestSeam::DropDeletesRun(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.DropDeletesRun.load() : -1;
}

int64 FSuperSLMPrefixCacheTestSeam::DropDeletesRunAtLastTeardown(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = Subsystem.RetiredPrefixCacheHost.Get();
	return Host != nullptr ? Host->Test.DropDeletesRun.load() : -1;
}

void FSuperSLMPrefixCacheTestSeam::SetSaveHold(USuperSLMGpuSubsystem& Subsystem, bool bHold)
{
	if (FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem)) { Host->Test.bGpuSaveHold.store(bHold); }
}

int32 FSuperSLMPrefixCacheTestSeam::HeldSaveCount(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->Test.HeldSaves.load() : -1;
}

int32 FSuperSLMPrefixCacheTestSeam::ShortGpuPool(USuperSLMGpuSubsystem& Subsystem, int32 LeaveFreePages)
{
	FSuperSLMGpuSubsystemState* S = Subsystem.State;
	if (S == nullptr || !S->PrefixCache.Host || SuperSLMGpuPrefixCacheGlue::LaneClosed(*S))
	{
		return -1;
	}
	// On the submission thread, behind every queued job: budget-1 sequences (R(1) = 2 pages) and, for an odd excess of 3
	// or more, one budget-(B + 1) sequence (3 pages), until the free count is at most LeaveFreePages. Each is re-read from
	// the pool, so the engine's own reservation decides; a refusal (the pool already short) ends the fill.
	int32 FreeLeft = -1;
	const int32 Leave = FMath::Max(LeaveFreePages, 0);
	S->RunSync([S, Leave, &FreeLeft]()
	{
		const int32 B = static_cast<int32>(S->PageGeometry.page_positions);
		for (int32 Guard = 0; Guard < (1 << 16); ++Guard)
		{
			SslmGpuKvPoolStats Stats = {};
			Stats.struct_size = static_cast<uint32_t>(sizeof(Stats));
			const SslmGpuStatus StatsStatus = sslm_gpu_kv_pool_stats(S->Pool, &Stats);
			S->NoteLayer1Status(StatsStatus);
			if (StatsStatus != SslmGpuStatus::SSLM_OK)
			{
				FreeLeft = -1;
				return;
			}
			FreeLeft = static_cast<int32>(Stats.free_pages);
			const int32 Excess = FreeLeft - Leave;
			if (Excess <= 0 || FreeLeft < 2)
			{
				return;
			}
			const int32 Budget = (Excess % 2 == 1 && Excess >= 3) ? B + 1 : 1;
			SslmGpuSequenceHandle* Seq = nullptr;
			const SslmGpuStatus Status = sslm_gpu_seq_create_budgeted(S->Ctx, S->Model, S->Pool, Budget, &Seq);
			S->NoteLayer1Status(Status);
			if (Status != SslmGpuStatus::SSLM_OK || Seq == nullptr)
			{
				return;
			}
			S->PrefixCache.TestPoolShortHolders.Add(Seq);
		}
	}, TEXT("test seam (ShortGpuPool)"), 120.0);
	return FreeLeft;
}

void FSuperSLMPrefixCacheTestSeam::RestoreGpuPool(USuperSLMGpuSubsystem& Subsystem)
{
	if (Subsystem.State != nullptr)
	{
		SuperSLMGpuPrefixCacheGlue::ReleasePoolShortHolders(*Subsystem.State);
	}
}

int32 FSuperSLMPrefixCacheTestSeam::StaleCookedEntries(const USuperSLMGpuSubsystem& Subsystem)
{
	const FSuperSLMGpuPrefixCacheHost* Host = GpuCacheHostOf(Subsystem);
	return Host != nullptr ? Host->StaleCookedEntries() : -1;
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

bool USuperSLMGpuSubsystem::WarmPrefix(const FSuperSLMPrefixSpec&, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	OutError = TEXT("NotConfigured: the GPU backend is built on Windows x64 only");
	return false;
}

bool USuperSLMGpuSubsystem::DropPrefix(const FSuperSLMPrefixSpec&, ESuperSLMPrefixDropScope, FSuperSLMPrefixOp& OutOp, FString& OutError)
{
	OutOp = FSuperSLMPrefixOp();
	OutError = TEXT("NotConfigured: the GPU backend is built on Windows x64 only");
	return false;
}

bool USuperSLMGpuSubsystem::GetPrefixOpResult(const FSuperSLMPrefixOp&, FSuperSLMPrefixOpResult& Out) const
{
	Out = FSuperSLMPrefixOpResult();
	return false;
}

// No GPU cache exists here, so nothing is kept or registered.
void USuperSLMGpuSubsystem::RegisterPrefixSet(USuperSLMPrefixSet*) {}
void USuperSLMGpuSubsystem::UnregisterPrefixSet(USuperSLMPrefixSet*) {}
void USuperSLMGpuSubsystem::ReRegisterPrefixSets() {}

bool FSuperSLMGpuPrefixSetAccess::ReadBuildCache(const USuperSLMGpuSubsystem&, SuperSLMCore::FPrefixEngineIdentity& OutIdentity, bool& bOutDiskEnabled,
	int32& OutCookedEntries, FString& OutError)
{
	OutIdentity = SuperSLMCore::FPrefixEngineIdentity();
	bOutDiskEnabled = false;
	OutCookedEntries = 0;
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}

bool FSuperSLMGpuPrefixSetAccess::SaveLease(USuperSLMGpuSubsystem&, const FSuperSLMPrefix&, TArray<uint8>& OutPayload, bool& bOutOnGameThread, FString& OutError)
{
	OutPayload.Reset();
	bOutOnGameThread = false;
	OutError = TEXT("the GPU backend is built on Windows x64 only");
	return false;
}

int32 FSuperSLMGpuPrefixSetAccess::ConfiguredSubsystemCount() { return 0; }

#if WITH_DEV_AUTOMATION_TESTS
FSuperSLMGpuPrefixCacheHost* FSuperSLMPrefixCacheTestSeam::GpuCacheHostOf(const USuperSLMGpuSubsystem&) { return nullptr; }
void FSuperSLMPrefixCacheTestSeam::HoldReleases(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldReleaseCount(const USuperSLMGpuSubsystem&) { return 0; }
int32 FSuperSLMPrefixCacheTestSeam::ReleasesOf(const USuperSLMGpuSubsystem&, uint64) { return 0; }
uint64 FSuperSLMPrefixCacheTestSeam::EngineHandleOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return 0; }
void FSuperSLMPrefixCacheTestSeam::SetNullRestoreHints(USuperSLMGpuSubsystem&, bool) {}
int64 FSuperSLMPrefixCacheTestSeam::SharedPagesOf(USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::SetBuildHold(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldBuildCount(const USuperSLMGpuSubsystem&) { return -1; }
ESuperSLMPrefixBuildState FSuperSLMPrefixCacheTestSeam::BuildStateOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return ESuperSLMPrefixBuildState::Unknown; }
int32 FSuperSLMPrefixCacheTestSeam::InFlightBuildCount(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::SetBuildChunkHold(USuperSLMGpuSubsystem&, int32) {}
int32 FSuperSLMPrefixCacheTestSeam::BuildChunkJobsOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefix&) { return 0; }
int64 FSuperSLMPrefixCacheTestSeam::UnfrozenBuildReleases(const USuperSLMGpuSubsystem&) { return 0; }
bool FSuperSLMPrefixCacheTestSeam::DispatchHeldRelease(USuperSLMGpuSubsystem&, int32) { return false; }
int32 FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandle(const USuperSLMGpuSubsystem&) { return 0; }
void FSuperSLMPrefixCacheTestSeam::ForceKeyCollision(USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&, const FSuperSLMPrefixSpec&) {}
void FSuperSLMPrefixCacheTestSeam::SetDiskFault(USuperSLMGpuSubsystem&, ESuperSLMPrefixDiskFault) {}
void FSuperSLMPrefixCacheTestSeam::SetBeforeDiskRead(USuperSLMGpuSubsystem&, TFunction<void(const FString&)>) {}
int32 FSuperSLMPrefixCacheTestSeam::DiskIoInFlight(const USuperSLMGpuSubsystem&) { return 0; }
bool FSuperSLMPrefixCacheTestSeam::IsCacheIoSettled(const USuperSLMGpuSubsystem&) { return true; }
void FSuperSLMPrefixCacheTestSeam::HoldWriteBacks(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldWriteBackCount(const USuperSLMGpuSubsystem&) { return -1; }
int32 FSuperSLMPrefixCacheTestSeam::MaxDiskExclusion(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::HoldDiskReads(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldDiskReadCount(const USuperSLMGpuSubsystem&) { return -1; }
int32 FSuperSLMPrefixCacheTestSeam::MaxKeyDiskOverlap(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::SetDeleteRefusal(USuperSLMGpuSubsystem&, bool) {}
void FSuperSLMPrefixCacheTestSeam::HoldKeyHash(USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&, bool) {}
void FSuperSLMPrefixCacheTestSeam::SetOffGameThreadRecording(USuperSLMGpuSubsystem&, bool) {}
int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadViolations(const USuperSLMGpuSubsystem&) { return -1; }
int64 FSuperSLMPrefixCacheTestSeam::GameThreadStateAccesses(const USuperSLMGpuSubsystem&) { return 0; }
int64 FSuperSLMPrefixCacheTestSeam::OffGameThreadChecks(const USuperSLMGpuSubsystem&) { return 0; }
bool FSuperSLMPrefixCacheTestSeam::OffGameThreadAssertTrips(USuperSLMGpuSubsystem&) { return false; }
bool FSuperSLMPrefixCacheTestSeam::GameThreadAccessorTrips(USuperSLMGpuSubsystem&) { return false; }
void FSuperSLMPrefixCacheTestSeam::SetBusyReleases(USuperSLMGpuSubsystem&, int32) {}
void FSuperSLMPrefixCacheTestSeam::ForceTerminalLoss(USuperSLMGpuSubsystem&) {}
int64 FSuperSLMPrefixCacheTestSeam::PostedLaneJobs(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::ForceUnresponsive(USuperSLMGpuSubsystem&) {}
void FSuperSLMPrefixCacheTestSeam::SubstituteReleaseStatus(USuperSLMGpuSubsystem&, int32) {}
void FSuperSLMPrefixCacheTestSeam::SetCacheJobBoundSeconds(USuperSLMGpuSubsystem&, double) {}
TArray<double> FSuperSLMPrefixCacheTestSeam::CacheJobSeconds(const USuperSLMGpuSubsystem&) { return TArray<double>(); }
void FSuperSLMPrefixCacheTestSeam::SetNextCacheBuildStatus(USuperSLMGpuSubsystem&, int32) {}
void FSuperSLMPrefixCacheTestSeam::SetRestoreHold(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldRestoreCount(const USuperSLMGpuSubsystem&) { return -1; }
int32 FSuperSLMPrefixCacheTestSeam::ReleasesOfAtLastTeardown(const USuperSLMGpuSubsystem&, uint64) { return 0; }
int32 FSuperSLMPrefixCacheTestSeam::MaxReleasesOfOneHandleAtLastTeardown(const USuperSLMGpuSubsystem&) { return 0; }
int64 FSuperSLMPrefixCacheTestSeam::UnfrozenBuildReleasesAtLastTeardown(const USuperSLMGpuSubsystem&) { return 0; }
FString FSuperSLMPrefixCacheTestSeam::DiskFileOf(const USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&) { return FString(); }
FString FSuperSLMPrefixCacheTestSeam::DiskFileOfWithAdapter(const USuperSLMGpuSubsystem&, const FSuperSLMPrefixSpec&, uint32, uint32) { return FString(); }
bool FSuperSLMPrefixCacheTestSeam::AdapterIdsOf(const USuperSLMGpuSubsystem&, uint32&, uint32&) { return false; }
void FSuperSLMPrefixCacheTestSeam::HoldDropDeletes(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldDropDeleteCount(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::HoldTrims(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldTrimCount(const USuperSLMGpuSubsystem&) { return -1; }
int64 FSuperSLMPrefixCacheTestSeam::DropDeletesRun(const USuperSLMGpuSubsystem&) { return -1; }
int64 FSuperSLMPrefixCacheTestSeam::DropDeletesRunAtLastTeardown(const USuperSLMGpuSubsystem&) { return -1; }
void FSuperSLMPrefixCacheTestSeam::SetSaveHold(USuperSLMGpuSubsystem&, bool) {}
int32 FSuperSLMPrefixCacheTestSeam::HeldSaveCount(const USuperSLMGpuSubsystem&) { return -1; }
int32 FSuperSLMPrefixCacheTestSeam::ShortGpuPool(USuperSLMGpuSubsystem&, int32) { return -1; }
void FSuperSLMPrefixCacheTestSeam::RestoreGpuPool(USuperSLMGpuSubsystem&) {}
int32 FSuperSLMPrefixCacheTestSeam::StaleCookedEntries(const USuperSLMGpuSubsystem&) { return -1; }
#endif // WITH_DEV_AUTOMATION_TESTS

#endif // SUPERSLMUNREAL_WITH_GPU
