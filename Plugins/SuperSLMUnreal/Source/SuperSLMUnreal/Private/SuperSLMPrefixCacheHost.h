#pragma once

// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
// The prefix cache's Unreal half (precache plan §5.3-§5.7, §5.11): one per configured CPU
// subsystem state. It owns the host-tested policy core (Core/SuperSLMPrefixCacheCore.h), carries
// out the commands it emits, and feeds back one completion per command:
//
//   - pool-thread commands (ComputeKey, CheckCooked, ReadDisk, WriteDisk, RemoveDiskFile,
//     TouchDiskFile, TrimDisk) run as UE tasks. Disk commands are serialised per key: at most one
//     disk command per key is in flight, and TrimDisk runs alone (no other disk command in flight,
//     none started while it runs). Their completions arrive through an MPSC inbox the game thread
//     drains in Pump();
//   - Layer-1 commands (Restore, Build, Save, Release) become lane jobs the subsystem posts on its
//     own worker lanes (TakeLaneJob), so they carry job-ledger rows, honour paging's test holds and
//     are delivered at their committed tick; a lane job's completion is applied at its delivery;
//   - DropPayload frees a payload ref here.
//
// Ownership (§5.5): the core is reachable only through Core(), which asserts the game thread and
// counts its calls (cell 3.9). Hashing, file IO and Layer-1 calls assert they are NOT on the game
// thread (CheckOffGameThread, cell 7.8).

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Containers/Queue.h"
#include "Core/SuperSLMBuildSlicing.h"
#include "Core/SuperSLMPrefixCacheCore.h"
#include "HAL/CriticalSection.h"
#include "superslm/sslm_abi.h"
#include "SuperSLMSequenceTypes.h"
#include "Templates/Function.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMPrefixCacheTestSeam.h"
#endif

// What a lane job sees when it runs: the subsystem's model and pool, and the lane's workspace.
struct FSuperSLMPrefixLaneContext
{
	sslm_model Model = nullptr;
	sslm_kv_pool* Pool = nullptr;
	sslm_workspace Workspace = nullptr;
	int32 MaxPrefillChunk = 64;
	bool bBudgetMode = false;
#if WITH_DEV_AUTOMATION_TESTS
	// The build hold (§8, rev 1.18): set by the subsystem when it posts a Build while the hold is armed. The lane's
	// counter then reads the job as held from its post until it passes the hold, so the lane is not free for
	// planning (nothing queues behind a held build) and the lane-park barrier counts it as parked. The stop flag is
	// the lane's own: the hold yields to the stop request, as the park does.
	bool bBuildHoldCounted = false;
	std::atomic<int32>* LaneBuildHolds = nullptr;
	const std::atomic<bool>* LaneStopRequested = nullptr;
	// Paging's one-shot Layer-1 status override (FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride), taken for
	// this job when it was parked: a Build reads it in place of its begin call, a Restore in place of
	// sslm_prefix_restore. SSLM_OK (0) when none was set: the job calls Layer 1.
	int32 ForcedLayer1Status = 0;
#endif
};

// The job kinds the subsystem posts them as (its job ledger and paging's holds key on these).
enum class ESuperSLMPrefixLaneKind : uint8
{
	Build,   // the begin job: posted as a PrefixBegin job; paging's PrefixBuild holds apply. A CPU build's chunk jobs are
	         // Build jobs too (bBuildChunk), posted as DecodeOrPrefill jobs as CreatePrefix's prefills are
	Restore, // posted as a PrefixBegin job; paging's Restore holds apply
	Save,    // posted as a Save job
	Release, // posted as a PrefixRelease job; paging's PrefixRelease holds apply
};

// A sliced CPU cache build (precache plan rev 1.18.8, §5.3 item 4; cells 5.9, 1.7): the chain its begin job and its
// chunk jobs share. One request throughout: the core sees one Build command and one completion, written by the job
// that finishes the build (the last chunk's, which freezes, or the one that fails). Lane-written fields (Slices' lane
// half, the completion) are read on the game thread only after that job's delivery.
struct FSuperSLMCpuBuildRun
{
	FSuperSLMCpuBuildRun(int32 Length, int32 OriginLength) : Slices(Length, OriginLength) {}

	SuperSLMCore::FBuildSlices Slices;
	TArray<int32> Tokens;
	FString Schema;
	int32 BeginLength = 0;   // begin's budget (a nested build: the suffix's)
	uint64 Parent = 0;       // a nested build's parent (begin_from)
	uint64 EntryId = 0;      // the seams key the build's progress on it
	int32 PinnedLane = INDEX_NONE; // the lane the begin job ran on: every chunk job runs there, in order
	int64 LastLaneJobId = 0; // the JobId of the build's last job posted on PinnedLane (its begin, then each chunk; §15.29)
	int64 ApplyHoldId = 0;   // paging's HoldApply(PrefixBuild), taken with the begin job: the job that freezes carries it
	SuperSLMCore::FBuildChunkPlan Next; // the chunk the next chunk job runs, planned at its dispatch (game thread)
	TSharedPtr<SuperSLMCore::FPrefixCompletion, ESPMode::ThreadSafe> Completion; // the build's one completion
	bool bBuildCounted = false; // NotePrefixPrefills' build count: once per build, at its first chunk
};

struct FSuperSLMPrefixLaneJob
{
	ESuperSLMPrefixLaneKind Kind = ESuperSLMPrefixLaneKind::Build;
	// Runs on the lane. Writes the command's completion into *Out.
	TFunction<void(const FSuperSLMPrefixLaneContext& Lane, SuperSLMCore::FPrefixCompletion* Out)> Run;
	// The completion, filled by Run, applied at the job's delivery by DeliverLaneJob().
	TSharedPtr<SuperSLMCore::FPrefixCompletion, ESPMode::ThreadSafe> Completion;
	// Release only: the engine handle released, for the release-counting seam.
	uint64 ReleasedHandle = 0;
#if WITH_DEV_AUTOMATION_TESTS
	// Release only: the minting of ReleasedHandle's address this release was issued for (FTestState::MintByHandle).
	uint64 ReleasedMint = 0;
	// Build and Restore: the paging override taken when the job was parked (FSuperSLMPrefixLaneContext).
	int32 ForcedLayer1Status = 0;
#endif
	// Restore only: the payload's bytes, which price the job (RestoreCostMs + RestoreCostPerMiBMs x MiB, §5.5).
	int64 PayloadBytes = 0;
	// A CPU build's chain (§5.3 item 4, rev 1.18.8), on its begin job and on each chunk job; null on every other job
	// and on a GPU build. bBuildChunk: this job is one of its chunk jobs.
	TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe> BuildRun;
	bool bBuildChunk = false;
};

class USuperSLMPrefixSet;

class FSuperSLMPrefixCacheHost
{
public:
	// PrefixIdCounter: the subsystem's GNextPrefixId, so cache handles never share an id with a
	// CreatePrefix handle (§5.6).
	explicit FSuperSLMPrefixCacheHost(int64* PrefixIdCounter);
	virtual ~FSuperSLMPrefixCacheHost();

	// Configure() on the game thread, after the pool exists. Config.RegionC is the page ledger in
	// budget mode, null in legacy mode.
	bool Configure(const SuperSLMCore::FPrefixCacheConfig& Config, FString& OutError);

	// ---- The API (§5.6, §5.10), game thread ----
	bool Request(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefix& OutLease, FString& OutError);
	bool Return(const FSuperSLMPrefix& Lease);
	ESuperSLMPrefixSource GetSource(const FSuperSLMPrefix& Lease) const;
	ESuperSLMPrefixLeaseState GetLeaseState(const FSuperSLMPrefix& Lease) const;
	FSuperSLMPrefixCacheStats GetStats() const;
	// Explicit residency control (§5.6, rev 1.18). The op table is the core's, which outlives configurations.
	bool Warm(const FSuperSLMPrefixSpec& Spec, FSuperSLMPrefixOp& OutOp, FString& OutError);
	bool Drop(const FSuperSLMPrefixSpec& Spec, ESuperSLMPrefixDropScope Scope, FSuperSLMPrefixOp& OutOp, FString& OutError);
	bool GetOpResult(const FSuperSLMPrefixOp& Op, FSuperSLMPrefixOpResult& Out) const;

	// ReleasePrefix's refusal: true (refuse) for any handle this cache minted.
	bool RefuseRelease(const FSuperSLMPrefix& Prefix, FString& OutError);

	enum class EAdoptLookup : uint8 { NotCacheHandle, Adoptable, NotReady, LeaseEnded };
	// AdoptPrefix's cache lookup (§5.6, §5.11 (b)): on Adoptable the entry is pinned until
	// EndAdopt(Token) and OutHandle is its engine handle, which the caller adopts through the
	// subsystem's one adopt entry point. OutEntryId is the entry's residency id, never reused: the
	// paging ledger's cache-entry key (a released handle's address can come back for a new entry).
	EAdoptLookup BeginAdopt(const FSuperSLMPrefix& Prefix, sslm_prefix& OutHandle, uint64& OutToken, uint64& OutEntryId, FString& OutError);
	void EndAdopt(uint64 Token);

	// ---- The cooked tier (§5.3 item 2, §5.8; step U3), game thread ----
	// Registers Set's entries with the core's cooked tier: the build that serves this CPU subsystem
	// and each of its entries with readable bytes (SelectPrefixSetBuild, PrefixSetRegistrations). A
	// set with no CPU build, and an entry the build left empty, register nothing: those requests
	// recompute. The bytes are copied, so the asset may unload. Registering a set again replaces its
	// earlier registration (a rebuilt set). Registrations outlive Configure(), as the core's do, but each
	// Configure() checks every registered set's model against the model it names (§5.8, rev 1.18.5): a set
	// registered before the first Configure(), and a set a re-Configure() to another model no longer
	// matches, is unregistered with PrefixSetModelMismatch, once. Returns how many entries registered.
	int32 RegisterCookedSet(const USuperSLMPrefixSet& Set);
	void UnregisterCookedSet(const USuperSLMPrefixSet& Set);
	int32 CookedEntriesRegistered() const;
	bool IsCoreConfigured() const { return Core().IsConfigured(); }

	// ---- The executor, game thread ----
	// Applies every completion that has arrived, then takes the core's commands and starts them.
	// Called once per Tick(), in the apply pass, after lane deliveries.
	void Pump();
	bool HasLaneJob() const { return LaneJobs.Num() > 0; }
	FSuperSLMPrefixLaneJob TakeLaneJob();
	// A job taken and not posted (parked at its start hold when teardown came) goes back first.
	void PutBackLaneJob(FSuperSLMPrefixLaneJob&& Job) { LaneJobs.Insert(MoveTemp(Job), 0); }
	// A sliced CPU build's next chunk jobs (§5.3 item 4, rev 1.18.8), one per open build, issued at the previous job's
	// delivery and posted by the subsystem on the build's own lane when it is free (other jobs run there between).
	int32 NumChunkJobs() const { return ChunkJobs.Num(); }
	const FSuperSLMPrefixLaneJob& ChunkJobAt(int32 Index) const { return ChunkJobs[Index]; }
	FSuperSLMPrefixLaneJob TakeChunkJob(int32 Index);
	// A lane job's delivery (game thread, the subsystem's Deliver callback).
	void DeliverLaneJob(const FSuperSLMPrefixLaneJob& Job);

	// ---- Teardown ----
	// Re-Configure() and Deinitialize() (§5.11 (a) L1), called by the subsystem after its lanes
	// have stopped and every posted job has been delivered, before it releases its own handles and
	// destroys the pool:
	//   1. pool-thread IO is waited for (cell 3.7: nothing left partial under a key name);
	//   1a. a drop's delete is drained under this configuration (§5.4 rev 1.18.5): the IO completions are
	//      applied and disk work runs until no drop delete is pending; a write-back's Save no lane has run
	//      completes Skipped (no engine call) and is applied, so a delete waiting on it runs here. Never
	//      carried into the next Configure();
	//   2. every lane job not yet posted (held releases included) runs here, off the game thread:
	//      a Release releases, a Build, Restore or Save completes Failed without an engine call.
	//      Their completions go to the inbox and are NOT applied: after the core is abandoned they
	//      belong to an ended generation, and the next configuration's first Pump() drops them
	//      (StaleCompletionsDropped);
	//   3. every engine handle the cache still holds is released (counted by the release seam), so
	//      the pool destroy finds no live cache handle;
	//   4. the core is abandoned: every live lease resolves Cancelled.
	void ReleaseForPoolDestroy(const FSuperSLMPrefixLaneContext& Lane);
	// The subsystem's teardown has begun, before it delivers its lanes' finished jobs: the core marks every
	// lease still Pending, so a build or restore running now reads Cancelled after the next Configure().
	void MarkTeardownBegins()
	{
		bTearingDown = true; // a chunk delivered from here on issues no further chunk (§5.3 item 4, cell 1.7)
		Core().MarkTeardownBegins();
	}
	// The subsystem's own sslm_prefix_release sites report here (release-counting seam, §5.10).
	void NoteRelease(sslm_prefix Handle) { NoteReleasedHandle(static_cast<uint64>(reinterpret_cast<UPTRINT>(Handle))); }
	void NoteReleasedHandle(uint64 HandleBits);
	// A handle minted (a build or restore delivered, or a CreatePrefix begin): its release count restarts.
	void MarkHandleMinted(uint64 HandleBits);
	// A lane release that returned (not Busy, not Skipped): counted by the seam if its address was not minted again.
	void NoteJobRelease(const FSuperSLMPrefixLaneJob& Job);
	int32 DiskIoInFlight() const { return InFlightIo.load(std::memory_order_acquire); }
	// No pool-thread hash or disk IO in flight and nothing waiting for the next Pump (test seam only).
	bool IsIoSettled() const { return InFlightIo.load(std::memory_order_acquire) == 0 && Inbox.IsEmpty(); }

	// The one accessor to the core's state (§5.5): asserts the game thread, counts the call.
	SuperSLMCore::FSuperSLMPrefixCacheCore& Core();
	const SuperSLMCore::FSuperSLMPrefixCacheCore& Core() const;

	// Asserts the caller is NOT on the game thread (cell 7.8). Counts every check.
	void CheckOffGameThread() const;

#if WITH_DEV_AUTOMATION_TESTS
	struct FTestState
	{
		bool bHoldReleases = false;
		TArray<FSuperSLMPrefixLaneJob> HeldReleases;
		TMap<uint64, int32> ReleasesByHandle;
		// Each address's current minting (MarkHandleMinted), so a release is counted against the minting it was
		// issued for and a stale delivery is not counted against a newer one at the same address (review finding 2).
		// Never reset: serials only grow.
		TMap<uint64, uint64> MintByHandle;
		uint64 NextMint = 0;
		struct FCollision { TArray<int32> FromTokens; FString FromSchema; TArray<int32> ToTokens; FString ToSchema; };
		TArray<FCollision> Collisions;
		std::atomic<uint8> DiskFault{0}; // ESuperSLMPrefixDiskFault
		FCriticalSection HookGuard;
		TFunction<void(const FString&)> BeforeDiskRead;
		mutable std::atomic<int64> GameThreadStateAccesses{0};
		mutable std::atomic<int64> OffGameThreadChecks{0};
		mutable std::atomic<bool> bRecordAsserts{false};
		mutable std::atomic<bool> bAssertTripped{false};
		// The teardown reading (cells 1.7, 2.10), recorded by the subsystem.
		bool bTeardownProbeArmed = false;
		int64 TeardownFreePages = -1;
		int32 LastPoolDestroyStatus = MIN_int32;
		// Cell 8.10's live(t): 1.0.0 CreatePrefix handles begun and not yet released.
		TSet<uint64> LegacyLive;
		// Nested entries (U6): cell 8.12's failing hint, and each restored handle's shared pages s.
		bool bNullRestoreHints = false;
		TMap<uint64, int64> SharedPagesByHandle;
		// Rev 1.18 seams (§8). The build hold (cells 7.21, 8.10 (c)): armed on the game thread; HeldBuilds counts the
		// build jobs stopped at it now. Each cache build's progress, by the entry it builds (an entry id is never
		// reused): Queued when the core emits it (charged), Begun once begin returns, Frozen once freeze returns; a
		// failed build is removed. Reset at Configure().
		std::atomic<bool> bBuildHold{false};
		std::atomic<int32> HeldBuilds{0};
		mutable FCriticalSection BuildStateGuard;
		TMap<uint64, ESuperSLMPrefixBuildState> BuildStates;
		// The pool-thread hold on write-backs (cells 3.10, 11.7): a write-back stops mid-write (its temporary file
		// written, not yet renamed) while armed. The exclusion counter over delete and trim, and its highest
		// reading since Configure().
		std::atomic<bool> bHoldWriteBacks{false};
		std::atomic<int32> HeldWriteBacks{0};
		std::atomic<int32> DiskExclusion{0};
		std::atomic<int32> MaxDiskExclusion{0};
		// The off-game-thread assert's recording mode (cell 7.8, rev 1.18.1): a trip is counted, not asserted.
		std::atomic<bool> bRecordOffGameThread{false};
		mutable std::atomic<int64> OffGameThreadViolations{0};
		// The shared pages s the last applied sequence restore reported (*out_shared_pages; cell 8.15); -1 before any.
		int64 LastRestoreSharedPages = -1;
		// The U2 code review's seams (§8; finding 2). The disk-read hold: each disk read stops on its pool thread
		// after it is taken and before it opens the file, while armed; yields to teardown (ReleaseTestHolds).
		std::atomic<bool> bHoldDiskReads{false};
		std::atomic<int32> HeldDiskReads{0};
		// The per-key overlap counter: one key's disk commands (read, write-back write, delete, touch) in flight
		// at once, and its highest reading since Configure().
		FCriticalSection KeyOverlapGuard;
		TMap<FString, int32> KeyDiskInFlight;
		std::atomic<int32> MaxKeyDiskOverlap{0};
		// The file-delete refusal: a drop's delete meets its file held open without delete sharing.
		std::atomic<bool> bRefuseDeletes{false};
		// The hash-completion hold, by spec: a matching hash's completion is kept here, not enqueued, until its
		// spec's hold is cleared. It does not yield to teardown.
		struct FHashHold { TArray<int32> Tokens; FString Schema; };
		FCriticalSection HashHoldGuard;
		TArray<FHashHold> HashHolds;
		TArray<TPair<FHashHold, SuperSLMCore::FPrefixCompletion>> HeldHashes;
		// The release-status substitution: SSLM_OK when off.
		std::atomic<int32> SubstitutedReleaseStatus{0};
		// The sliced CPU build's seams (rev 1.18.8; cells 1.7, 5.9). The chunk hold: while >= 0, a build's chunk job
		// is not posted once that many of its chunk jobs have run, so the build waits between two chunk jobs; teardown
		// clears it. Chunk jobs delivered per build (by entry id). Never reset, so a cell reads them across a
		// re-Configure(): chunk jobs issued at a delivery under a teardown (must stay 0), and unfrozen builds the
		// teardown released (each exactly once).
		std::atomic<int32> ChunkHoldAfter{-1};
		TMap<uint64, int32> ChunkJobsByEntry;
		int64 ChunkJobsIssuedInTeardown = 0;
		int64 UnfrozenBuildReleases = 0;
	};
	FTestState Test;

	void DispatchHeldRelease(int32 Index);
	// The batch gate on release jobs (cell 8.14): closing it holds each release as it is started; opening it
	// dispatches every held release in order.
	void SetHoldReleases(bool bHold);
	FString DiskFileOf(const FSuperSLMPrefixSpec& Spec) const;
	// The same, keyed as an instance on another adapter would key it (the GPU instance's AdapterKey cell).
	FString DiskFileOfWithAdapter(const FSuperSLMPrefixSpec& Spec, uint32 VendorId, uint32 DeviceId) const;
	const SuperSLMCore::FPrefixCacheConfig& ConfigForTest() const { return Config; }
	uint64 EngineHandleOf(const FSuperSLMPrefix& Lease);
	bool OffGameThreadAssertTrips();
	bool GameThreadAccessorTrips();
	// The rev 1.18 seams' readings (SuperSLMPrefixCacheTestSeam.h).
	ESuperSLMPrefixBuildState BuildStateOf(const FSuperSLMPrefix& Lease) const;
	int32 InFlightBuildCount() const;
	bool IsBuildHoldArmed() const { return Test.bBuildHold.load(); }
	// The chunk hold (rev 1.18.8): this chunk job waits.
	bool IsChunkHeld(const FSuperSLMCpuBuildRun& Run) const
	{
		const int32 After = Test.ChunkHoldAfter.load();
		return After >= 0 && Run.Slices.GetChunkJobs() >= After;
	}
	int32 ChunkJobsOf(const FSuperSLMPrefix& Lease) const;
	// Every test hold released (teardown: the holds yield to it, so pool-thread IO and lanes drain).
	void ReleaseTestHolds();
#endif

protected:
	// The backend's Layer-1 half (step U6): the CPU verbs here; the GPU subsystem's cache instance
	// (SuperSLMGpuPrefixCacheHost.h) overrides these with the GPU prefix verbs. Everything else -- the
	// core, the pool-thread IO, the disk serialisation, the payload refs, the inbox -- is shared.
	// MakeLaneJob: the job for a Build, Restore, Save or Release command (engine handles are opaque
	// bits to the base). ReleaseEngineHandleNow: teardown's release of a handle the cache still
	// holds, off the game thread; true when it released. RunOffGameThreadAndWait: where teardown's
	// engine calls run (a thread-pool future for the CPU; the GPU's submission thread).
	virtual FSuperSLMPrefixLaneJob MakeLaneJob(const SuperSLMCore::FPrefixCommand& Command);
	virtual bool ReleaseEngineHandleNow(uint64 Handle, int32& OutStatus);
	virtual void RunOffGameThreadAndWait(TFunction<void()> Work);

	// Prefix prefills (precache U4, plan §15.26): a backend's build job reports the prefill calls it made, on its
	// lane, after they return. GetStats() reads the totals (PrefixPrefillCalls, PrefixPrefillBuilds). A sliced CPU
	// build reports each chunk's one call, and counts as one build (bNewBuild on its first chunk only).
	void NotePrefixPrefills(int32 Calls, bool bNewBuild = true)
	{
		if (Calls > 0)
		{
			PrefixPrefillCalls.fetch_add(Calls, std::memory_order_relaxed);
			if (bNewBuild)
			{
				PrefixPrefillBuilds.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	// A sliced CPU build's chunk job (§5.3 item 4, rev 1.18.8): one prefill call of the planned chunk, and the freeze
	// when it reaches the build's length.
	FSuperSLMPrefixLaneJob MakeChunkJob(const TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>& Run);

	// Payload refs, for a backend's Restore and Save jobs.
	uint64 StorePayload(TArray<uint8>&& Bytes);
	// A payload's size in bytes, 0 when the ref holds none (the restore's per-MiB price, §5.5).
	int64 PayloadBytesOf(uint64 Ref) const;
	bool TakePayloadCopy(uint64 Ref, TArray<uint8>& Out) const;
	void DropPayload(uint64 Ref);

private:
	SuperSLMCore::FSuperSLMPrefixCacheCore CacheCore;
	SuperSLMCore::FPrefixCacheConfig Config;

	// Completions from pool-thread commands.
	TQueue<SuperSLMCore::FPrefixCompletion, EQueueMode::Mpsc> Inbox;
	TArray<FSuperSLMPrefixLaneJob> LaneJobs;
	// Sliced CPU builds (§5.3 item 4, rev 1.18.8): each open build's next chunk job, and every build begun and not
	// finished (its unfrozen handle is the teardown's to release, once). Game thread.
	TArray<FSuperSLMPrefixLaneJob> ChunkJobs;
	TArray<TSharedPtr<FSuperSLMCpuBuildRun, ESPMode::ThreadSafe>> OpenBuilds;
	bool bTearingDown = false;
	// Engine handles the cache holds (game thread): added when a Build or Restore delivers one, removed when its
	// Release is issued (StartCommand), and added back if that release returns Busy or is Skipped (the handle was
	// not consumed). A handle's identity is its address, and a released address can be minted again by a lane job
	// that is delivered before the release is: removing at delivery would then remove the new handle (box
	// 2026-10-21 review, finding 2). Issue precedes every lane's view of the release, so nothing can reuse the
	// address before it leaves the set; a Busy or Skipped release never freed the address, so it cannot have been
	// reused when it is added back.
	TSet<uint64> LiveEngineHandles;
	void AbandonCore();

	// Payload refs: bytes read, checked or saved off the game thread.
	mutable FCriticalSection PayloadGuard;
	TMap<uint64, TArray<uint8>> Payloads;
	std::atomic<uint64> NextPayloadRef{1};

	// NotePrefixPrefills()'s totals: written on the lanes, read on the game thread. Never reset.
	std::atomic<int64> PrefixPrefillCalls{0};
	std::atomic<int64> PrefixPrefillBuilds{0};

	// Cooked blobs (whole 'SLUP' wrappers) by the blob id the core knows them by, and each registered
	// set's blob ids by the set's path name (its object id is reused after garbage collection, so a new set
	// could take an old one's registration; 1.3.0 review item 4). Read on pool threads (RunCheckCooked) under the guard.
	mutable FCriticalSection CookedGuard;
	TMap<uint64, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> CookedBlobs;
	struct FCookedSet
	{
		TArray<uint64> Ids;
		std::array<uint8_t, 32> ModelHash{}; // the set's model's artifact hash, checked at every Configure()
		std::string Name;                    // the set's path name, for PrefixSetModelMismatch's line
	};
	TMap<FString, FCookedSet> CookedBySet;
	void UnregisterCookedIds(const TArray<uint64>& Ids);
	uint64 NextCookedBlobId = 1;

	// Per-key disk serialisation (game thread).
	TMap<FString, TArray<SuperSLMCore::FPrefixCommand>> DiskQueues; // key hex -> commands waiting
	TSet<FString> DiskKeysInFlight;
	TArray<SuperSLMCore::FPrefixCommand> TrimQueue;
	bool bTrimInFlight = false;
	TMap<uint64, FString> DiskCommandKey; // command id -> key hex, for the commands in flight
	std::atomic<int32> InFlightIo{0};

	void StartCommand(const SuperSLMCore::FPrefixCommand& Command);
	void StartDiskCommand(const SuperSLMCore::FPrefixCommand& Command);
	void LaunchPoolThread(const SuperSLMCore::FPrefixCommand& Command);
	void OnDiskCommandApplied(uint64 CommandId);
	void StartNextDiskWork();
	void ApplyCompletion(const SuperSLMCore::FPrefixCompletion& Completion);
	void FlushLog();

	// Pool-thread bodies.
	void RunComputeKey(const SuperSLMCore::FPrefixCommand& Command, SuperSLMCore::FPrefixCompletion& Out) const;
	void RunReadDisk(const SuperSLMCore::FPrefixCommand& Command, SuperSLMCore::FPrefixCompletion& Out);
	void RunCheckCooked(const SuperSLMCore::FPrefixCommand& Command, SuperSLMCore::FPrefixCompletion& Out);
	void RunWriteDisk(const SuperSLMCore::FPrefixCommand& Command, SuperSLMCore::FPrefixCompletion& Out);

#if WITH_DEV_AUTOMATION_TESTS
public:
	// Lane and pool-thread halves of the rev 1.18 seams (called from jobs and the file layer).
	void NoteBuildState(uint64 EntryId, ESuperSLMPrefixBuildState State);
	void ForgetBuildState(uint64 EntryId);
	void PassBuildHold(const FSuperSLMPrefixLaneContext& Lane, bool bBegun);
	void PassWriteBackHold();
	void EnterDiskExclusion();
	void LeaveDiskExclusion() { Test.DiskExclusion.fetch_sub(1); }
	void PassDiskReadHold();
	void EnterKeyDisk(const SuperSLMCore::FPrefixKey& Key);
	void LeaveKeyDisk(const SuperSLMCore::FPrefixKey& Key);
	bool ParkHeldHash(const SuperSLMCore::FPrefixCommand& Command, const SuperSLMCore::FPrefixCompletion& Out);
	// The seams' game-thread halves (SuperSLMPrefixCacheTestSeam.h).
	void SetHashHold(const TArray<int32>& Tokens, const FString& Schema, bool bHold);
private:
#endif

	bool IsCacheHandle(const FSuperSLMPrefix& Prefix) const;
};

#endif // SUPERSLM_WITH_PREFIX_CACHE
