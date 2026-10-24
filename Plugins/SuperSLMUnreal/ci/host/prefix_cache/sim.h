#pragma once

// The host stand-in for everything the cache core does not do itself: an in-memory file layer, a
// fake engine pool that draws and releases pages, and an executor that runs the core's commands
// (as the plugin's pool threads and worker lanes would) and feeds back one completion per command
// (as Tick()'s apply pass would).
//
// The fake pool is the host's fill probe: CacheDrawnPages counts every page an engine handle the
// cache asked for holds, in flight or frozen, independently of the core's own ledger. Cells grade
// against it, never against the core's diagnostics alone (precache plan §7's grading rule).

#include "SuperSLMPrefixCacheCore.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace HostSim
{
	using namespace SuperSLMCore;

	class FMemFs : public IPrefixFileOps
	{
	public:
		struct FFile
		{
			std::vector<uint8_t> Data;
			int64_t ModifiedTime = 0;
		};

		std::map<std::string, FFile> Files; // full path -> file
		int64_t Clock = 0;                  // advanced by every write and touch
		bool bFailWrites = false;
		bool bFailReads = false;
		bool bFailRemoves = false;          // the file-delete refusal seam (5.12): RemoveFile refuses, the file stays

		bool WriteWhole(const std::string& Path, const uint8_t* Data, size_t Size) override;
		EPrefixIoStatus ReadWhole(const std::string& Path, std::vector<uint8_t>& OutData) override;
		bool RenameReplace(const std::string& From, const std::string& To) override;
		bool RemoveFile(const std::string& Path) override;
		bool ListDirectory(const std::string& Dir, std::vector<FPrefixDiskFileInfo>& OutFiles) override;
		bool TouchModifiedTime(const std::string& Path) override;

		bool Exists(const std::string& Path) const { return Files.count(Path) != 0; }
		// Names (not paths) of every file under Dir.
		std::vector<std::string> NamesIn(const std::string& Dir) const;
	};

	// A fake SPX1 payload: "SPX1", the schema name, the tokens, then an FNV-1a checksum of all of it
	// standing in for the SPX1 digest. A restore of a payload whose checksum fails is Refused.
	std::vector<uint8_t> MakeFakePayload(const std::string& Schema, const std::vector<int32_t>& Tokens);
	bool ParseFakePayload(const std::vector<uint8_t>& Payload, std::string& OutSchema, std::vector<int32_t>& OutTokens);

	struct FEngineHandle
	{
		uint64_t Pages = 0;           // pages it maps (its own and the ones it shares)
		std::vector<uint64_t> PageIds; // the pages it maps, in order; shared pages carry one reference each
		int32_t Length = 0;            // positions held (origin included)
		uint32_t SharedPages = 0;      // leading pages shared with a parent (begin_from or a hinted restore)
		std::vector<int32_t> Tokens;  // what an adopter would see: the twin comparison operand
		std::string Schema;
		bool bReleased = false;
		bool bFromRestore = false;
	};

	class FSim
	{
	public:
		FSim(FSuperSLMPrefixCacheCore& InCache, const FPrefixCacheConfig& InConfig, uint64_t InPoolPages);

		FSuperSLMPrefixCacheCore& Cache;
		FPrefixCacheConfig Config;
		FMemFs OwnFs;
		FMemFs* Fs; // OwnFs unless a test shares one file system across lifetimes

		// --- Knobs -------------------------------------------------------------------------------
		uint64_t PoolPages = 0;
		uint64_t ExternalPages = 0;          // pages held by something outside the cache
		bool bRefuseRestores = false;        // the engine refuses every restore (Refused)
		int32_t RefuseStatus = 5;            // the sslm_status a refusal reports
		uint32_t RestoreSharedPages = 0;     // s reported by every successful ROOT restore (a stand-in; draws less)
		bool bNullHints = false;             // test seam (8.12): every restore runs with a null hint
		bool bFailBuilds = false;
		bool bFailRestores = false;          // every restore ends Failed (an engine failure, not a refusal)
		bool bFailSaves = false;             // every save ends Failed
		// The withdrawn rev 1.18 seam (§8 rev 1.18.1, 5.15's must-reject): the next N releases end Failed and
		// release NOTHING, which no engine release does. Kept only so the release-status seam's must-reject
		// can show the release-counting seam reading 0 releases under it.
		uint32_t FailReleases = 0;
		// The release-status substitution seam (§8, rev 1.18.1; cell 5.15): the next N releases run the real
		// release (the handle is consumed and counted) and then report Failed with ReleaseFailStatus, as the
		// engine's sslm_prefix_release does on a page-module refusal.
		uint32_t ConsumeThenFailReleases = 0;
		int32_t ReleaseFailStatus = 1;       // SSLM_INVALID_ARGUMENT (1); SSLM_ARTIFACT_REJECTED is 4
		// The GPU busy-release substitution seam (§8, rev 1.18.2; cell 5.15's GPU leg): the next N releases
		// return Busy (SSLM_BUSY, 2) WITHOUT releasing, then the real release runs. BusyIssues counts them.
		uint32_t BusyReleases = 0;
		uint32_t BusyIssues = 0;
		// Its must-reject (§8): a busy seam that releases first and then reports Busy.
		bool bBusyAfterRelease = false;
		// The skipped-release seam (U2 code review finding 3; §5.4 rev 1.18.5; cell 5.14's core half): the next N
		// releases are skipped by the GPU submission thread (a terminal device loss, or the thread marked
		// unresponsive): nothing reaches the engine, the handle stays live, and the completion reads Skipped.
		uint32_t SkipReleases = 0;
		uint32_t SkippedReleases = 0;
		// The file API's error a refused delete reports (U2 code review finding 6): the file-delete refusal
		// (FMemFs::bFailRemoves) reports RemoveRefusalError; a delete meeting the key's file open for reading or
		// for a touch (no delete sharing on Windows) reports SharingViolationError.
		std::string RemoveRefusalError = "Windows error 5 (ERROR_ACCESS_DENIED)";
		std::string SharingViolationError = "Windows error 32 (ERROR_SHARING_VIOLATION)";
		EPrefixWriteFault WriteFault = EPrefixWriteFault::None;
		std::set<EPrefixCommandKind> Held;   // RunAll leaves these queued
		// The hash-completion hold (§8, rev 1.18.1; cells 3.12, 3.13, 1.7): RunOne / RunAll leave the
		// ComputeKey of each held lease or op queued; RunHashOfLease / RunHashOfOp run one explicitly.
		std::set<int64_t> HeldHashLeases;
		std::set<uint64_t> HeldHashOps;
		bool bAutoPull = true;               // RunOne / RunOneOfKind take the core's new commands after applying
		// Forced key collisions: a spec matching (schema, tokens) gets this key from ComputeKey.
		std::vector<std::pair<FPrefixStoredSpec, FPrefixKey>> ForcedKeys;
		std::map<uint64_t, std::vector<uint8_t>> Cooked; // cooked blob id -> wrapper bytes
		// The CPU host's lane-side bookkeeping, modeled for the box review's host cells (box 2026-10-21 review,
		// findings 1 and 2; SuperSLMPrefixCacheHost.cpp). The host is Unreal code the suite does not compile, so these
		// model it, as HostReleaseForPoolDestroy does; they are kept in step with the host.
		// Finding 2: the next build or restore is given the id of a handle already released (the allocator handing
		// a freed address out again), its release count restarting (the host's MarkHandleMinted).
		bool bReuseReleasedHandles = false;
		// HostLive is the host's LiveEngineHandles: a handle joins when its Build or Restore is delivered Ok, and
		// leaves when its Release is issued (taken by Pull: the host's StartCommand); a release delivered Busy or
		// Skipped puts it back. bHostLiveAtDelivery is the pre-fix host (the must-reject): it removed the handle
		// when the release was delivered, so a mint of the same address delivered first was removed with it.
		bool bHostLiveAtDelivery = false;
		std::set<uint64_t> HostLive;
		// Read by HostReleaseForPoolDestroy's step 3, before its sweep: handles the engine holds that HostLive does
		// not (the host's sweep misses them, and the pool destroy refuses SSLM_POOL_HAS_LIVE_HANDLES), and handles
		// in HostLive the engine already released (the sweep would release them again).
		uint32_t HostSweepMisses = 0;
		uint32_t HostSweepDoubles = 0;
		// Finding 1: the lane's build-hold count (FSuperSLMCpuWorkerRunnable::TestBuildHolds). A build run while
		// bBuildHoldArmed counts on its lane (the host counts it at post) until it passes the hold (PassBuildHold)
		// at its begin; the sim never stops a begun build, so the pass is immediate. The next build only is forced
		// to ForcedBuildResult / ForcedBuildStatus without a begin (paging's Layer-1 override, taken at park);
		// bForcedBuildSkipsHold is the pre-fix host (the must-reject), whose forced build returned before the pass.
		bool bBuildHoldArmed = false;
		bool bForceNextBuild = false;
		EPrefixEngineResult ForcedBuildResult = EPrefixEngineResult::PoolExhausted;
		int32_t ForcedBuildStatus = 14; // SSLM_KV_POOL_EXHAUSTED
		bool bForcedBuildSkipsHold = false;
		int32_t LaneBuildHolds = 0;
		uint32_t ForcedBuilds = 0;

		// --- State -------------------------------------------------------------------------------
		std::deque<FPrefixCommand> Queue;               // taken from the core, not yet executed
		std::vector<FPrefixCommand> History;            // every command ever taken, in order
		std::map<uint64_t, FEngineHandle> Handles;      // engine handle -> its pages
		std::map<uint64_t, int> ReleaseCount;           // engine handle -> releases executed
		std::map<uint64_t, std::vector<uint8_t>> Payloads;
		uint64_t NextEngineHandle = 1000;
		uint64_t NextPayloadRef = 1;
		uint64_t CacheDrawnPages = 0;                   // the fill probe: pages with a live reference, refcount-accurate
		std::map<uint64_t, int> PageRefs;               // page id -> references (handles mapping it)
		uint64_t NextPageId = 1;
		uint64_t PeakCacheDrawnPages = 0;
		std::vector<std::string> Violations;            // engine-contract breaches the core caused
		// The disk part's counters (§8 rev 1.18.1-1.18.2; cells 3.10, 1.7). A held command (the pool-thread
		// hold, the lane-park seam) is RUNNING: it has started and not finished. Executing a disk command of a
		// key while another disk command of that key is held running is an overlap; executing a drop's
		// delete while a trim is held running (or the reverse) is an exclusion breach. A delete that meets a
		// read of its key held running fails, as on Windows (a file open for reading).
		// Rev 1.18.5 (U2 code review finding 7): the touch of a hit's file time is one of the key's disk commands, and
		// a delete meeting a touch held running fails as one meeting a read does (UE's SetTimeStamp opens the file
		// with read sharing only).
		uint32_t MaxKeyDiskOverlap = 0;                 // the per-key overlap counter: read, write-back write, delete, touch
		uint32_t MaxDeleteTrimOverlap = 0;              // the exclusion counter over a drop's delete and the trim
		uint32_t DropDeletesRun = 0;                    // drop deletes executed (RemoveDiskFile with OpId)

		// Takes the core's commands into Queue.
		void Pull();
		// Executes the first queued command whose kind is not held, applies its completion, pulls.
		bool RunOne();
		// Executes the first queued command of Kind (held or not). False if none.
		bool RunOneOfKind(EPrefixCommandKind Kind);
		// Executes the queued Release of EngineHandle (held or not) and applies it. False if none.
		bool RunRelease(uint64_t EngineHandle);
		// Executes the first queued command of Kind WITHOUT applying its completion (a lane job that
		// ran before a re-Configure whose completion is applied after it). False if none.
		bool ExecuteOnly(EPrefixCommandKind Kind, FPrefixCompletion& Out);
		// The same, also returning the command, so the caller can deliver it later and in any order (DeliverAsPass).
		bool ExecuteOnly(EPrefixCommandKind Kind, FPrefixCommand& OutCommand, FPrefixCompletion& Out);
		// Delivers a lane job that ran (ExecuteOnly): the host's DeliverLaneJob bookkeeping (HostLive), then the
		// completion applied as one apply pass, then Pull.
		void DeliverAsPass(const FPrefixCommand& Command, const FPrefixCompletion& Done);
		// The subsystem's pool destroy at a re-Configure: every handle still held is released with the
		// pool, queued commands and held payloads go with the lanes.
		void DestroyPool();
		uint64_t PoolDestroyReleases = 0;
		// RunOne until nothing runnable remains (bounded). Returns steps taken.
		int RunAll(int MaxSteps = 20000);

		// The CPU host's ReleaseForPoolDestroy, steps 1-3 (SuperSLMPrefixCacheHost.cpp:1155-1259 at 79163cd69): the
		// product's re-Configure() and Deinitialize() path, which goes to the core's Abandon(), never to its
		// BeginTeardown() (U2 code review, findings 1 and 2; plan §5.4 and §5.6 rev 1.18.5). Its reading of the sim:
		// a queued pool-thread command (hash, cooked check, disk read, write, delete, touch, trim, payload drop) is
		// pool-thread IO, and a queued lane command (restore, build, save, release) is a lane job never posted.
		//   1.  The test holds yield (every pool-thread hold and hash hold cleared); the IO taken runs, its
		//       completions to Inbox.
		//   1a. Up to 64 rounds: Inbox applied as one apply pass; stop when no drop delete is pending; else the
		//       core's new commands taken and their IO run.
		//   2.  Every lane job never posted: a release runs (its completion to Inbox); any other is failed
		//       (Engine Failed, status -1) without running; Inbox is NOT applied.
		//   3.  Every engine handle still live is released (the host's LiveEngineHandles sweep), and the pool goes.
		// Returns false, doing only step 1, when the core is not configured (the host returns there). The caller
		// runs step 4 (Abandon()) and then, at a re-Configure(), Configure() and the first Pump, which applies the
		// Inbox left by step 2 (HostReconfigure in cache_fixture.h). This is kept in step with the host.
		bool HostReleaseForPoolDestroy(std::vector<FPrefixCompletion>& Inbox);
		static bool IsLaneKind(EPrefixCommandKind Kind);
		uint32_t HostTeardownRounds = 0; // step 1a's rounds taken by the last HostReleaseForPoolDestroy
		// Every RunOne / RunOneOfKind / RunRelease applies one completion as one apply pass: Apply, then
		// Cache.EndApplyPass() (§5.6's step 1, then steps 2 and 3). Pass() is a pass with no completion
		// (a frame whose calls -- Return, Request, EndAdopt -- are followed by Tick()'s apply pass).
		void Pass();
		// Runs the held ComputeKey of a lease or op (held or not) as one apply pass. False if none queued.
		bool RunHashOfLease(int64_t Lease);
		bool RunHashOfOp(uint64_t OpId);

		size_t CountHistory(EPrefixCommandKind Kind) const;
		size_t CountQueued(EPrefixCommandKind Kind) const;
		uint64_t PagesFor(int32_t Length) const;      // ceil(L/B)
		uint64_t BuildPagesFor(int32_t Length) const; // R(L) = min(ceil(L/B) + 1, ceil(cap/B))
		std::string DiskPath(const FPrefixKey& Key) const;
		// The engine handle a Ready lease adopts (BeginAdopt then EndAdopt at once); 0 if not adoptable.
		uint64_t EngineHandleOf(int64_t Lease);
		const FEngineHandle* HandleOf(int64_t Lease);

	private:
		bool HashHeld(const FPrefixCommand& Command) const;
		uint64_t MintHandle();
		void HostDeliver(const FPrefixCommand& Command, const FPrefixCompletion& Done);
		void ApplyAsPass(const FPrefixCommand& Command);
		FPrefixCompletion Execute(const FPrefixCommand& Command);
		uint64_t StorePayload(std::vector<uint8_t> Bytes);
	};

	// --- Fixtures --------------------------------------------------------------------------------
	// B = 16, cap = 64, PageBytes = 4096, vocab 50,000. A budget of N entries is N x ceil(32/16) + 1
	// pages: N resident entries of L = 32, or N - 1 of them beside one build at its peak R(32) = 3.
	constexpr int32_t kFixtureLength = 32;
	constexpr uint64_t kFixturePageBytes = 4096;
	FPrefixEngineIdentity FixtureIdentity();
	FPrefixCacheConfig FixtureConfig(uint32_t EntryBudget, bool bDisk);
	// Distinct per Index; L tokens, inside the fixture vocabulary.
	FPrefixSpec FixtureSpec(int32_t Index, int32_t Length = kFixtureLength, const std::string& Schema = std::string());
	FPrefixStoredSpec StoredOf(const FPrefixSpec& Spec);
	FPrefixKeyInputs KeyInputsFor(const FPrefixEngineIdentity& Identity, const FPrefixStoredSpec& Spec);
	FPrefixKey FixtureKey(const FPrefixSpec& Spec);
	bool Contains(const std::string& Haystack, const std::string& Needle);
}
