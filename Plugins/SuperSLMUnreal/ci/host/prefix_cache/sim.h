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
		uint64_t Pages = 0;
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
		uint32_t RestoreSharedPages = 0;     // s reported by every successful restore
		bool bFailBuilds = false;
		bool bFailRestores = false;          // every restore ends Failed (an engine failure, not a refusal)
		bool bFailSaves = false;             // every save ends Failed
		EPrefixWriteFault WriteFault = EPrefixWriteFault::None;
		std::set<EPrefixCommandKind> Held;   // RunAll leaves these queued
		bool bAutoPull = true;               // RunOne / RunOneOfKind take the core's new commands after applying
		// Forced key collisions: a spec matching (schema, tokens) gets this key from ComputeKey.
		std::vector<std::pair<FPrefixStoredSpec, FPrefixKey>> ForcedKeys;
		std::map<uint64_t, std::vector<uint8_t>> Cooked; // cooked blob id -> wrapper bytes

		// --- State -------------------------------------------------------------------------------
		std::deque<FPrefixCommand> Queue;               // taken from the core, not yet executed
		std::vector<FPrefixCommand> History;            // every command ever taken, in order
		std::map<uint64_t, FEngineHandle> Handles;      // engine handle -> its pages
		std::map<uint64_t, int> ReleaseCount;           // engine handle -> releases executed
		std::map<uint64_t, std::vector<uint8_t>> Payloads;
		uint64_t NextEngineHandle = 1000;
		uint64_t NextPayloadRef = 1;
		uint64_t CacheDrawnPages = 0;                   // the fill probe
		uint64_t PeakCacheDrawnPages = 0;
		std::vector<std::string> Violations;            // engine-contract breaches the core caused

		// Takes the core's commands into Queue.
		void Pull();
		// Executes the first queued command whose kind is not held, applies its completion, pulls.
		bool RunOne();
		// Executes the first queued command of Kind (held or not). False if none.
		bool RunOneOfKind(EPrefixCommandKind Kind);
		// RunOne until nothing runnable remains (bounded). Returns steps taken.
		int RunAll(int MaxSteps = 20000);

		size_t CountHistory(EPrefixCommandKind Kind) const;
		size_t CountQueued(EPrefixCommandKind Kind) const;
		uint64_t PagesFor(int32_t Length) const;      // ceil(L/B)
		uint64_t BuildPagesFor(int32_t Length) const; // R(L) = min(ceil(L/B) + 1, ceil(cap/B))
		std::string DiskPath(const FPrefixKey& Key) const;
		// The engine handle a Ready lease adopts (BeginAdopt then EndAdopt at once); 0 if not adoptable.
		uint64_t EngineHandleOf(int64_t Lease);
		const FEngineHandle* HandleOf(int64_t Lease);

	private:
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
