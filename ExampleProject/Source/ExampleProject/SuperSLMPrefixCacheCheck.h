#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;
class USuperSLMSubsystem;
class UWorld;

// The packaged prefix-cache check (precache plan step U4: cells 4.5, 8.9, 10.5, 10.6 and 10.7, 5.9's build leg,
// and the restore's per-MiB calibration). CPU only. Opt-in from the command line, like the packaged check: each run
// is one leg, writes one JSON report, logs one line per check and exits 0 when every check passed, 1 otherwise.
//
//   -SuperSLMExamplePrefixCacheCheck=<Leg>
//       Cooked       10.5: a recompute pass (the set not registered), then the set registered and the
//                    "level" loads its 10 cooked prefixes: every request a Cooked hit, no prefix prefill,
//                    adopters' tokens equal to the recompute pass's, every tick within the plugin's bound.
//       Budget       10.5's second half: the recompute pass, then the budget set to 5 entries and the level
//                    entered (10 requests in turn), then left and re-entered (the map is opened again):
//                    evicted entries come back from the cooked tier, with no prefill and equal tokens.
//       DiskWrite    10.6 run 1: the disk tier on in an empty directory; every request recomputes and is
//                    written back (10 files). The report keeps the tokens for run 2.
//       DiskRead     10.6 run 2 (a new process): every request is a Disk hit, no prefill, tokens equal run 1's.
//       Determinism  8.9: the determinism self-check and the plugin's query path, first with no cache, then
//                    with the cache on and serving the set throughout: same verdict and digest, hits > 0.
//       Calibrate    The per-MiB term (precache plan section 5.5): SuperSLM.EstimateCosts --write-ini, then a Configure() that reads
//                    it back and cooked restores priced by bytes. Rev 1.18.11 (section 15.31): the ini holds the
//                    prefix_prefill pair for both decode modes, and the Configure() resolves it from the saved estimate.
//                    Rev 1.18.12 (section 15.32): then the in-situ prefix_prefill arm, with the game ticking: two
//                    2,048-token cache builds (without, then with 5.9 (c)'s generation on the build's lane); the
//                    second's ledger chunk rows are fitted and written over the estimate's pair
//                    (USuperSLMSubsystem::WriteInSituPrefixPrefillPrice), read back by the next Configure(), and the
//                    isolated, in-situ and in-situ-with-generation ms per token are recorded.
//       Residency    10.7: at a level load the next level's 10 prefixes are warmed from the cooked set (each
//                    Resident, no prefill); the level is entered and its start's requests are all Memory hits (no
//                    prefill, equal tokens, the tick bound); at its exit they are dropped (Memory) and resident bytes
//                    fall by theirs; the next level requests the dropped specs again and each is served Cooked.
//                    The set holds Text entries and Tokens entries (u4.ps1 passes U4MixedSet), and every spec comes
//                    from USuperSLMPrefixSet::FindEntrySpec against the configured model; at the end each Text
//                    entry's stored ids are checked against Tokenize of its text.
//       OneJobBuild  5.9's build leg (precache plan rev 1.18.8, section 15.27; the leg keeps its U4 name): one CPU
//                    cache build of 2,000 tokens, now sliced, on one lane, with a generation started on that lane
//                    once its first chunk job has delivered; then the same ids built as a one-call reference
//                    (MaxPrefillChunkBudget = MinPrefillChunk = 2,000). Graded: (a) sliced, each chunk the chunk
//                    planner's at its price, in order, summing to 2,000, on one lane; (b) the adopters' tokens and
//                    the two disk files byte-equal; (c) a generation job ran between the chunk jobs, and at most one
//                    between any two build jobs (rev 1.18.10); (d) rev 1.18.11 (section 15.31): the build's own
//                    chunk jobs, graded once a prefix_prefill pair prices them (the median chunk's worker ms at
//                    most its K x TickBudgetMs), recorded before; the lane's hitch count recorded beside it.
//   -SuperSLMExamplePrefixSet=<object path>   the set (default /Game/SuperSLMExamplePrefixes/U4PrefixSet.U4PrefixSet):
//                                             10 root entries, built for the CPU and cooked; of equal length for
//                                             every leg but Residency, which needs Text and Tokens entries
//                                             (/Game/SuperSLMExamplePrefixes/U4MixedSet.U4MixedSet)
//   -SuperSLMExamplePrefixCacheOut=<dir>      where the report goes (default <Saved>/SuperSLMPrefixCacheCheck)
//   -SuperSLMExamplePrefixDiskDir=<dir>       DiskWrite and DiskRead: the disk tier's directory
//   -SuperSLMExamplePrefixCompare=<file>      DiskRead: DiskWrite's report, whose tokens it must equal
//   -SuperSLMExampleCalibrateArtifact=<file>  Calibrate: the .sslm the model asset was imported from
//
// Every leg also records cell 4.5's reading: the platform (Windows x64), the build configuration and that the
// data is cooked. Prefix prefills are read from the plugin's own counter (FSuperSLMPrefixCacheStats::
// PrefixPrefillCalls, precache plan section 15.26), which ships in Development and Shipping; the engine's test-only counter
// does not exist in a packaged build.
//
// The check outlives the scene actor: the Budget and Residency legs re-enter the level, which destroys the actor and
// spawns a new one, and the new scene finds the running check (Active()) and keeps ticking it. Game thread only.
class FSuperSLMPrefixCacheCheck
{
public:
	// True when the command line asks for a leg; OutLeg is its name as given.
	static bool IsRequested(FString& OutLeg);
	// Starts the leg over the loaded model and the GameInstance's CPU subsystem. A leg name that is not one of
	// the eight, or a build without the prefix cache (SUPERSLM_WITH_PREFIX_CACHE off), fails at once and exits 1.
	static void Start(USuperSLMModel& Model, USuperSLMSubsystem& Cpu, const FString& Leg, const FString& ModelPath);
	// The running check, or null.
	static FSuperSLMPrefixCacheCheck* Active();

	// Once per frame, from the scene in World.
	void Tick(UWorld* World, float DeltaSeconds);

	~FSuperSLMPrefixCacheCheck();

private:
	FSuperSLMPrefixCacheCheck();
	struct FState;
	TUniquePtr<FState> State;
};
