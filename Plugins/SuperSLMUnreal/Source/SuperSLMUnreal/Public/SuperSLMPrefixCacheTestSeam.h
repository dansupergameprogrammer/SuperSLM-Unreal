#pragma once

// The prefix cache's test seams (precache plan §8's
// plugin seams; §5.10's release-counting seam; cell 8.14's batch gate). Compiled only under
// WITH_DEV_AUTOMATION_TESTS and SUPERSLM_WITH_PREFIX_CACHE, so Shipping has none. CPU subsystem
// only; game thread only.

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"
#include "Templates/Function.h"

class USuperSLMSubsystem;
class USuperSLMGpuSubsystem;

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE

#include "superslm/sslm_abi.h" // sslm_status

// The disk write's fault seam (cell 5.8), in the order the write runs.
enum class ESuperSLMPrefixDiskFault : uint8
{
	None,
	FailTemporaryWrite, // the temporary file cannot be written
	FailRename,         // written, then the rename into place fails
	CrashBeforeRename,  // written, then the process "dies": the temporary stays, no rename
	FailRead,           // a disk read fails with an IO error (not a missing file): DiskReadError
};

// Edits to a disk-tier wrapper ('SLUP', §5.7) for the hostile-file cells (2.8, 5.4, 9.6, 11.4). The
// wrapper's layout stays this module's detail; each edit changes one field and nothing else.
enum class ESuperSLMDiskWrapperEdit : uint8
{
	Truncate,          // the file cut to half its length
	WrapperVersion0,   // the wrapper version field set to 0
	WrapperVersion2,   // ... and to 2
	OtherLayer1Tag,    // the Layer-1 tag field set to another release's tag (commit unchanged)
	PayloadByte,       // one byte inside the SPX1 payload's body flipped (the engine's digest refuses it)
	PayloadProducer,   // the SPX1 payload's producer identity changed, digest recomputed by the seam
	                   // (the engine's producer check refuses it: SSLM_RESTORE_PRODUCER_MISMATCH)
};

// A cache build's progress through its lane jobs (cell 8.10 (c), the build hold; §5.3 item 4): Begun once its begin
// job's begin returns, Frozen once its last chunk job freezes (a sliced CPU build, rev 1.18.8).
enum class ESuperSLMPrefixBuildState : uint8
{
	Unknown,  // not a build lease, or the seam is not built
	Queued,   // charged, its job not yet begun
	Begun,    // begin returned: its reserve is drawn, nothing frozen
	Frozen,   // frozen: its pages settled to ceil(L/B)
};

class SUPERSLMUNREAL_API FSuperSLMPrefixCacheTestSeam
{
public:
	// --- Rev 1.18 seams (§8), red stubs until U2 (suite audit G1, G2) ---

	// The build hold (§8; cells 7.21, 8.10 (c), 1.7): while set, each cache build job stops after its
	// begin returns and before its first prefill; the lane-park barrier counts a lane stopped there as
	// parked. Clearing it lets the held jobs run on. HeldBuildCount: builds stopped at the hold now.
	static void SetBuildHold(USuperSLMSubsystem& Subsystem, bool bHold);
	static int32 HeldBuildCount(const USuperSLMSubsystem& Subsystem);
	// Per-build state for the lease that requested it (cell 8.10 (c)'s witness), and the ledger's count
	// of builds charged and not yet frozen or failed.
	static ESuperSLMPrefixBuildState BuildStateOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease);
	static int32 InFlightBuildCount(const USuperSLMSubsystem& Subsystem);

	// The pool-thread hold on write-backs (§8; cells 3.10, 11.7): while set, a key's write-back stops
	// mid-write; HeldWriteBackCount reads how many are stopped. MaxDiskExclusion: the exclusion
	// counter's highest reading over delete and trim since Configure() (must never read 2).
	static void HoldWriteBacks(USuperSLMSubsystem& Subsystem, bool bHold);
	static int32 HeldWriteBackCount(const USuperSLMSubsystem& Subsystem);
	static int32 MaxDiskExclusion(const USuperSLMSubsystem& Subsystem);

	// The off-game-thread assert's recording mode (cell 7.8, rev 1.18.1): while set, a trip is counted
	// instead of asserting. OffGameThreadViolations: trips counted since Configure().
	static void SetOffGameThreadRecording(USuperSLMSubsystem& Subsystem, bool bRecord);
	static int64 OffGameThreadViolations(const USuperSLMSubsystem& Subsystem);

	// The shared pages s the last applied sequence restore reported (*out_shared_pages; cell 8.15).
	static int64 LastRestoreSharedPages(const USuperSLMSubsystem& Subsystem);

	// The batch gate on the cache's release jobs (cell 8.14): while held, a cache eviction's
	// sslm_prefix_release job is queued but not dispatched to a lane, so the window between an
	// eviction decision and its release's apply can be read. Releasing the gate dispatches them.
	static void HoldReleases(USuperSLMSubsystem& Subsystem, bool bHold);
	static int32 HeldReleaseCount(const USuperSLMSubsystem& Subsystem);
	// Dispatches only the held release at Index in eviction-decision order (0 = the oldest held),
	// leaving the gate closed for the rest, so a cell can apply one victim's release at a time.
	// False when no release is held at Index.
	static bool DispatchHeldRelease(USuperSLMSubsystem& Subsystem, int32 Index = 0);

	// The release-counting seam on the plugin's sslm_prefix_release call site (§5.10, cells 1.7,
	// 2.10): releases executed per engine handle, cache and CreatePrefix handles alike. A handle's
	// count restarts when its address is minted again (a cache build or restore delivered, or a
	// CreatePrefix begin), not at Configure(): a re-Configure()'s own teardown releases are read
	// after it returns (box 2026-10-21, Cpu.WarmReconfigure).
	static int32 MaxReleasesOfOneHandle(const USuperSLMSubsystem& Subsystem);
	static int32 ReleasesOf(const USuperSLMSubsystem& Subsystem, uint64 EngineHandle);
	// The engine handle a Ready lease adopts from (0 if none). Opens and closes no adopt.
	static uint64 EngineHandleOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease);

	// Forces ComputeKey for Spec to return the key of CollideWith (cells 2.8, 11.4). Cleared at
	// Configure().
	static void ForceKeyCollision(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, const FSuperSLMPrefixSpec& CollideWith);

	// The disk write fault seam (cell 5.8); applies to every write until set back to None.
	static void SetDiskFault(USuperSLMSubsystem& Subsystem, ESuperSLMPrefixDiskFault Fault);

	// Applies Edit to the wrapper file at Path in place. False when Path is not a well-formed wrapper.
	static bool EditDiskWrapper(const FString& Path, ESuperSLMDiskWrapperEdit Edit);

	// Runs Hook on the pool thread after the disk tier's directory scan names a file and before it
	// opens it (cell 2.8's "replaced between scan and read"). One-shot; cleared at Configure().
	static void SetBeforeDiskRead(USuperSLMSubsystem& Subsystem, TFunction<void(const FString& Path)> Hook);

	// Disk reads and writes the cache has in flight on pool threads (cell 3.7's positive clause).
	// Pool-thread IO only: a write-back's Save lane job (§5.7: the save, then the file write) is not
	// counted while it is queued or running, so 0 here does not mean a write-back has landed. Cells
	// wait on SuperSLMPrefixCacheFixtures::WaitWriteBacks for that.
	static int32 DiskIoInFlight(const USuperSLMSubsystem& Subsystem);
	// True when no key hash or disk IO is in flight on a pool thread and no completion is waiting in the inbox
	// for the next Tick's apply pass. The cache's work completes off the game thread and lands one Tick later,
	// so a cell that reads state after a fixed number of Ticks races it; cells settle on
	// SuperSLMPrefixCacheFixtures::SettleCacheIo instead. Lane jobs are not counted (DrainQuiet covers those).
	static bool IsCacheIoSettled(const USuperSLMSubsystem& Subsystem);

	// The game-thread accessor's checked-call count (cell 3.9) and the off-game-thread assert's
	// checked-call count (cell 7.8): both must read > 0 in their cells.
	static int64 GameThreadStateAccesses(const USuperSLMSubsystem& Subsystem);
	static int64 OffGameThreadChecks(const USuperSLMSubsystem& Subsystem);

	// Region C's capacity in pages as the subsystem sized it (cell 8.14 reads the pool's region C
	// through FSuperSLMPagedTestSeam::Ledger instead; this is for messages only).
	static int64 CacheRegionPages(const USuperSLMSubsystem& Subsystem);

	// The disk tier's file for Spec under the current configuration (the key computed as the
	// cache computes it, on the calling thread: test code only). Empty when the disk tier is off.
	static FString DiskFileOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec);

	// The teardown reading (cells 1.7, 2.10): armed once, the next Configure() or Deinitialize()
	// runs paging's fill probe after every lane has stopped and before the pool is destroyed, and
	// records the free count and the destroy's status. -1 and INT32_MIN respectively until a
	// teardown has run armed.
	static void ArmTeardownProbe(USuperSLMSubsystem& Subsystem);
	static int64 TeardownFreePages(const USuperSLMSubsystem& Subsystem);
	static int32 LastPoolDestroyStatus(const USuperSLMSubsystem& Subsystem);

	// Cell 8.10's live(t): handles a successful 1.0.0 CreatePrefix begin job returned, less those a
	// release job has since released. Cache handles never enter the set. Read behind the
	// lane-park barrier (FSuperSLMPagedTestSeam::RunOnPoolThread) by the cell.
	static int32 LegacyPrefixLive(const USuperSLMSubsystem& Subsystem);

	// Rewrites a 'SLUW' wrapper's Layer-1 tag and commit fields only (cell 9.11's crafted cases).
	// False for a blob that is not a well-formed wrapper. The layout stays this module's detail.
	static bool SetWrapperLayer1(TArray<uint8>& WrappedBlob, const FString& Tag, const FString& Commit);

	// The must-rejects of the two thread asserts (§8): each makes one instrumented call on the
	// wrong thread with the assert in its recording mode and returns true when it tripped.
	// OffGameThreadAssertTrips: a hashing call made on the game thread (cell 7.8).
	// GameThreadAccessorTrips: the state accessor reached from a pool thread (cell 3.9).
	static bool OffGameThreadAssertTrips(USuperSLMSubsystem& Subsystem);
	static bool GameThreadAccessorTrips(USuperSLMSubsystem& Subsystem);

	// --- The U2 seams plan §8 names and the plugin lacked (U2 code review finding 2) ---
	// Each reads its "not built" value (-1) when no cache host exists.
	//
	// The pool-thread hold of a key's disk read (§8 rev 1.18.2; cells 1.7, 3.10 (vii)): while set, each disk read
	// stops on its pool thread after it is taken and before it opens the file. Like the write-back hold it yields
	// to teardown (ReleaseTestHolds). HeldDiskReadCount: reads stopped at the hold now.
	static void HoldDiskReads(USuperSLMSubsystem& Subsystem, bool bHold);
	static int32 HeldDiskReadCount(const USuperSLMSubsystem& Subsystem);
	// The per-key overlap counter (§8 rev 1.18.2, rev 1.18.5 adds touch; cell 3.10 (vii)): the most disk commands
	// of one key (read, write-back file write, delete, touch) ever in flight at once. Cleared at Configure().
	// Must-reject: a forced overlap reads 2.
	static int32 MaxKeyDiskOverlap(const USuperSLMSubsystem& Subsystem);
	// The file-delete refusal (§8; cells 5.12, 5.14): while set, every drop's delete is refused by the file API
	// (the seam holds the file open without delete sharing, so the error is the platform's own).
	static void SetDeleteRefusal(USuperSLMSubsystem& Subsystem, bool bRefuse);
	// The hash-completion hold, per key (§8 rev 1.18.1; cells 3.12, 3.13, 1.7): while set for Spec, a hash of
	// Spec's tokens that completes is never applied. It does not yield to teardown: a hash held across a
	// re-Configure() is applied, if ever, under the old generation.
	static void HoldKeyHash(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, bool bHold);
	// The release-status substitution (§8 rev 1.18.1, plan review F1; cell 5.15): runs the real
	// sslm_prefix_release, then returns Status in place of its own (SSLM_OK clears it).
	static void SubstituteReleaseStatus(USuperSLMSubsystem& Subsystem, sslm_status Status);

	// The sliced CPU cache build (precache plan rev 1.18.8, §5.3 item 4; cells 1.7, 5.9). SetBuildChunkHold: while
	// AfterChunkJobs >= 0, a build's next chunk job is not posted once that many of its chunk jobs have run, so the
	// build waits between two chunk jobs (-1 clears it; a teardown clears it). BuildChunkJobsOf: the chunk jobs
	// delivered for the build Lease requested. Never reset, read across a re-Configure(): ChunkJobsIssuedInTeardown,
	// chunk jobs issued at a delivery under a teardown (must stay 0); UnfrozenBuildReleases, builds begun and not
	// frozen that a teardown released (each exactly once).
	static void SetBuildChunkHold(USuperSLMSubsystem& Subsystem, int32 AfterChunkJobs);
	static int32 BuildChunkJobsOf(const USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease);
	static int64 ChunkJobsIssuedInTeardown(const USuperSLMSubsystem& Subsystem);
	static int64 UnfrozenBuildReleases(const USuperSLMSubsystem& Subsystem);

	// Nested entries (step U6). SetNullRestoreHints: every cache restore issued while set passes a
	// null sharing hint, as a hint that fails would (cell 8.12). SharedPagesOf: the shared pages s
	// the restore that produced Lease's entry reported (*out_shared_pages); -1 for a lease whose entry
	// was not restored, or is not Ready.
	static void SetNullRestoreHints(USuperSLMSubsystem& Subsystem, bool bNull);
	static int64 SharedPagesOf(USuperSLMSubsystem& Subsystem, const FSuperSLMPrefix& Lease);

	// The GPU instance's twins of the seams above (step U6; defined in
	// SuperSLMGpuSubsystemPrefixCache.inl), plus its keying: AdapterIdsOf reads the vendor and device
	// the instance keys by (false when no GPU cache is configured); DiskFileOfWithAdapter names the
	// file another adapter's instance would use for Spec.
	static void HoldReleases(USuperSLMGpuSubsystem& Subsystem, bool bHold);
	static int32 HeldReleaseCount(const USuperSLMGpuSubsystem& Subsystem);
	static int32 ReleasesOf(const USuperSLMGpuSubsystem& Subsystem, uint64 EngineHandle);
	static uint64 EngineHandleOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease);
	static void SetNullRestoreHints(USuperSLMGpuSubsystem& Subsystem, bool bNull);
	static int64 SharedPagesOf(USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefix& Lease);
	static FString DiskFileOf(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec);
	static FString DiskFileOfWithAdapter(const USuperSLMGpuSubsystem& Subsystem, const FSuperSLMPrefixSpec& Spec, uint32 VendorId, uint32 DeviceId);
	static bool AdapterIdsOf(const USuperSLMGpuSubsystem& Subsystem, uint32& OutVendorId, uint32& OutDeviceId);

private:
	// The GPU subsystem's cache instance, through this class's friendship (null when none).
	static class FSuperSLMGpuPrefixCacheHost* GpuCacheHostOf(const USuperSLMGpuSubsystem& Subsystem);
};

#endif
