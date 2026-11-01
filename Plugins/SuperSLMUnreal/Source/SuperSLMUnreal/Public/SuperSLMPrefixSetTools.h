#pragma once

// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
// The Unreal side of cooked prefix sets (precache plan §5.8). Every rule is the host-tested core's
// (Private/Core/SuperSLMPrefixSetCore.h); this converts the asset to and from it, and runs the
// Layer-1 build. The Layer-1 calls stay in this module (the editor module does not link Layer 1);
// the editor module's Build Prefixes action (in the background), its commandlet and its cook hook call these.
// GPU entries are built per device (§5.8, PC-5; step U7) through a configured GPU subsystem's own prefix cache:
// BeginBuildGpu, PumpBuildGpu and FinishBuildGpu (or FinishBuildCpuAndGpu, the action's and the commandlet's).

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

#if SUPERSLM_WITH_PREFIX_CACHE

class USuperSLMGpuSubsystem;
class USuperSLMModel;
class USuperSLMPrefixSet;
struct FSuperSLMGpuRuntimeConfig;
struct FSuperSLMPrefixSetBuild;
struct FSuperSLMPrefixSetEntry;
struct FSuperSLMPrefixSpec;

// One finding about a set, by name: the core's finding (EmptyName ... Orphaned, TokenizeFailed, TextChanged), or
// a build's own (ModelRefused, UnknownSchema, BuildFailed, GpuNotBuilt, AdapterIdMismatch). Line is the whole log
// line: "<set path>: entry '<name>' [CPU]: <Finding> (<detail>)", [GPU 0x1002:0x744c] for a GPU producer, and no
// entry part for a finding about the whole set. AdapterVendorId and AdapterDeviceId are the GPU producer's (0 on
// the CPU and without a producer).
struct SUPERSLMUNREAL_API FSuperSLMPrefixSetFinding
{
	FName Finding;
	FName Entry;
	bool bHasProducer = false;
	uint8 Backend = 0;
	uint32 AdapterVendorId = 0;
	uint32 AdapterDeviceId = 0;
	FString Line;
};

namespace SuperSLMPrefixSetTools
{
	// Cook validation (§5.8): every entry whose stored bytes no longer match this build's Layer-1 tag
	// and commit, the set's model's artifact hash, or the entry as authored now; plus the manifest's
	// findings, entries left empty and orphaned bytes. Empty = a clean set. False only when the set
	// cannot be read at all (no model, or the model's header unreadable): OutError says why.
	// A Text entry whose stored text hash or model hash no longer matches is one TextChanged finding: the
	// cook ships it with no ids and no bytes (CookedImage), and the runtime serves nothing for it.
	SUPERSLMUNREAL_API bool ValidateForCook(const USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError);

	// USuperSLMPrefixSet::FindEntrySpec's rule (§5.8 *How a game requests it*, rev 1.18.3; cells 2.17, 2.18,
	// 7.20): the entry's stored ids and schema in Out (Out.Parent left invalid: the caller requests the parent
	// entry itself and passes that lease) and its parent entry's name (NAME_None for a root). Refused, with
	// OutReason naming why and the outputs empty: PrefixSetModelMismatch (ConfiguredModel is not the set's model,
	// by artifact hash; checked first, for every entry), UnknownEntry, NoStoredIds, TextChanged (a Text entry
	// whose ids are not its text's under the set's model). Never tokenizes.
	SUPERSLMUNREAL_API bool FindEntrySpec(const USuperSLMPrefixSet& Set, const USuperSLMModel* ConfiguredModel, FName Entry,
		FSuperSLMPrefixSpec& Out, FName& OutParentEntry, FName& OutReason);

#if WITH_EDITOR
	// What the cook ships (§5.8 *Staleness*; cell 2.17): a copy of the set's entries and builds in which every
	// Text entry that is not current under the set's model has no ids and no bytes. Returns how many entries were
	// cleared (0 = the copy equals the set). The set itself is not modified: USuperSLMPrefixSet::Serialize writes
	// the copy into a cooked package and the source asset stays byte-unchanged.
	SUPERSLMUNREAL_API int32 CookedImage(const USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetEntry>& OutEntries, TArray<FSuperSLMPrefixSetBuild>& OutBuilds);

	// The editor's change hooks (§5.8 *Staleness*; cell 2.17), called by USuperSLMPrefixSet::PostEditChangeProperty.
	// An edit to entry Index's Text or Input clears a Text entry's stored ids, its text and model hashes, and its
	// bytes in every build (a Tokens entry keeps its authored ids and bytes; its hashes are cleared). A change of
	// the set's model does the same for every Text entry. Each marks the package dirty when it cleared anything.
	SUPERSLMUNREAL_API void OnEntryTextEdited(USuperSLMPrefixSet& Set, int32 Index);
	SUPERSLMUNREAL_API void OnModelEdited(USuperSLMPrefixSet& Set);
#endif

#if WITH_EDITOR
	// The build (§5.8): first, on the game thread, the tokenize pass: each Text entry's text is tokenized
	// against the set's model (SuperSLM::TokenizeText, nothing prepended; a nested entry's suffix alone), and
	// its ids, text hash and model hash are stored. Then it maps the set's model on this build's Layer 1,
	// builds each buildable entry by prefill (begin_budgeted, schema bind, prefill, freeze), saves it, wraps
	// it, and replaces the set's CPU build. Failure is per entry (rev 1.18.1): an entry that fails at the
	// tokenize pass (TokenizeFailed, EmptyTokens, LongerThanContextCap) or in its prefill, freeze or save
	// keeps its own previously stored ids and bytes and is reported by name; every other entry builds. BuildCpu
	// makes no GPU entries: the set's GPU builds are left as they are and a GpuNotBuilt finding says so (the GPU
	// build is BeginBuildGpu's, below). Marks the package dirty.
	// False, with the set unchanged and OutError naming the set and the model, when the model is
	// missing or refused at map (cell 2.15: the two-name artifact), or this Layer 1 has no prefix save.
	// BuildCpu is the three phases below run in a row on the calling thread: the commandlet's build, and the
	// synchronous one the cells use. The editor's Build Prefixes action runs the same phases with the entries on a
	// worker thread (SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction), so the two outputs are byte-equal.
	SUPERSLMUNREAL_API bool BuildCpu(USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError);

	// The build in three phases (§5.8 *The build runs in the background*, PC-15 Q9; cell 3.11).
	//   BeginBuildCpu, game thread: the identity, the tokenize pass, the manifest, the model's mapping, the build's
	//     pool and workspace, and a fingerprint of the entries and the model. Null, with OutError, where BuildCpu
	//     refuses; the set is then unchanged (the tokenize pass's ids are not written until Finish).
	//   RunBuildEntries, any one thread: each buildable entry's prefill, freeze and save, in entry order. A cancel is
	//     honoured between entries (the entry in hand finishes). Touches no UObject and no registry state.
	//   FinishBuildCpu, game thread, after the entries' thread has returned: releases the build's mapping, then
	//     stores. Cancelled: nothing is stored, the set keeps its previous build byte for byte. Refused: the set or
	//     its model was edited while the build ran (the fingerprint differs), nothing is stored. Stored: the set's
	//     CPU build is replaced once, at the end, with every failing entry keeping its own previous bytes (rev
	//     1.18.1), and the GPU builds are kept and reported GpuNotBuilt. OutFindings holds every finding either way.
	//     FinishBuildCpuAndGpu, below, finishes this job together with a GPU job.
	// A job must be finished on the game thread before it is released.
	struct FPrefixSetBuildJob;

	enum class EBuildFinish : uint8
	{
		Stored,
		Cancelled,
		Refused,
	};

	// What the progress dialog shows: entries done of the buildable total (an entry that fails counts as done).
	struct FBuildProgress
	{
		int32 EntriesTotal = 0;
		int32 EntriesDone = 0;
		bool bCancelRequested = false;
		bool bPaused = false;   // the test pause seam is holding the entries' thread
		bool bWorkDone = false; // RunBuildEntries has returned; FinishBuildCpu may run
	};

	SUPERSLMUNREAL_API TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> BeginBuildCpu(USuperSLMPrefixSet& Set, FString& OutError);
	SUPERSLMUNREAL_API void RunBuildEntries(FPrefixSetBuildJob& Job);
	SUPERSLMUNREAL_API void CancelBuild(FPrefixSetBuildJob& Job);
	SUPERSLMUNREAL_API FBuildProgress GetBuildProgress(const FPrefixSetBuildJob& Job);
	SUPERSLMUNREAL_API EBuildFinish FinishBuildCpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& Job,
		TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError);

	// ---- The GPU build, per device (§5.8, PC-5; step U7; cell 3.11) ----
	// The GPU entries of one set, built for the adapter Gpu runs on, through Gpu's own prefix cache: each buildable
	// root is requested (RequestPrefix) and, once Ready, saved (sslm_gpu_prefix_save on the GPU subsystem's
	// submission thread, the entry pinned meanwhile), then returned. Every GPU call runs on the submission thread; the
	// game thread only requests, ticks and stores. Nested entries stay unbuilt (the manifest names them).
	//   BeginBuildGpu, game thread: the tokenize pass and the manifest as BeginBuildCpu, and the producer, which is the
	//     GPU cache's own identity (backend 1, the vendor and device of the adapter Layer 1 runs on). Null, with
	//     OutError and the set unchanged, unless Gpu is configured pooled with a prefix cache, for the set's model, with
	//     its disk tier off and no cooked prefix set registered (so every entry is built here, never restored).
	//   PumpBuildGpu, game thread: ticks Gpu once and moves the build on (one entry in flight at a time). True while
	//     work remains. A cancel (CancelBuild) is honoured between entries; the test pause holds between entries too.
	//     An entry that is refused, fails or is not Ready reports BuildFailed [GPU v:d] and keeps its previous bytes.
	//   FinishBuildGpu, game thread, after PumpBuildGpu returned false: checks every payload's SPX1 header names the
	//     producer (backend 1, its vendor and device; otherwise the GPU part is refused, AdapterIdMismatch), then
	//     replaces this device's GPU build once, every failing entry keeping its own previous bytes; every other
	//     producer's build is kept. Cancelled and Refused as FinishBuildCpu.
	// Gpu must stay configured, unchanged, from Begin to the last Pump (a re-Configure() fails the entries left).
	SUPERSLMUNREAL_API TSharedPtr<FPrefixSetBuildJob, ESPMode::ThreadSafe> BeginBuildGpu(USuperSLMPrefixSet& Set, USuperSLMGpuSubsystem& Gpu, FString& OutError);
	SUPERSLMUNREAL_API bool PumpBuildGpu(FPrefixSetBuildJob& Job, USuperSLMGpuSubsystem& Gpu, float DeltaSeconds = 1.0f / 60.0f);
	SUPERSLMUNREAL_API EBuildFinish FinishBuildGpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& Job,
		TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError);

	// The Build Prefixes action's and the commandlet's finish: a CPU job and, when the GPU part ran, a GPU job of the
	// same set, begun together, stored in one Modify(). The result is the CPU job's (Cancelled when either was
	// cancelled: nothing is stored). With the CPU part Stored, the GPU part is stored with it, or, when GpuJob is null,
	// refused, cancelled or failed its check, not stored (the set's GPU builds stay as they were) and reported:
	// GpuNotBuilt [GPU] naming GpuNotBuiltReason, or the GPU finish's own findings. bOutGpuStored says which.
	SUPERSLMUNREAL_API EBuildFinish FinishBuildCpuAndGpu(USuperSLMPrefixSet& Set, FPrefixSetBuildJob& CpuJob, FPrefixSetBuildJob* GpuJob,
		const FString& GpuNotBuiltReason, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError, bool& bOutGpuStored);

	// The producer a job builds for: backend 0 (CPU) or 1 with the adapter's PCI vendor and device.
	SUPERSLMUNREAL_API void GetBuildProducer(const FPrefixSetBuildJob& Job, uint8& OutBackend, uint32& OutVendorId, uint32& OutDeviceId);

	// The GPU subsystem configuration the editor's build and the commandlet use for Set (a transient game instance's
	// GPU subsystem): pooled, one sequence, ContextCap the model's, and a prefix cache budget that holds the set's
	// largest entry, disk off, no row-cost calibration. False, with OutError, when the model's shape does not read.
	SUPERSLMUNREAL_API bool MakeGpuBuildConfig(const USuperSLMPrefixSet& Set, FSuperSLMGpuRuntimeConfig& OutConfig,
		FSuperSLMGpuPrefixCacheSettings& OutCacheSettings, FString& OutError);

	// True while any GPU subsystem in the process is configured (Play In Editor, the query window): the editor's
	// build then leaves the GPU part unbuilt rather than configure a second GPU context beside it.
	SUPERSLMUNREAL_API bool AnyGpuSubsystemConfigured();

#if WITH_DEV_AUTOMATION_TESTS
	// Test seams (cell 3.11). SetBuildPauseAfterEntries: before RunBuildEntries starts, makes the entries' thread
	// hold after that many entries until ResumeBuild or a cancel. The seam is one-shot: it holds once, at that count,
	// and ResumeBuild's flag is never cleared, so a cell cannot expect a second pause from the same job. GameThreadBuildCalls: how many prefill, freeze
	// and save calls ran on the game thread since the last reset (the action's must read 0; BuildCpu's reads > 0).
	SUPERSLMUNREAL_API void SetBuildPauseAfterEntries(FPrefixSetBuildJob& Job, int32 Entries);
	SUPERSLMUNREAL_API void ResumeBuild(FPrefixSetBuildJob& Job);
	SUPERSLMUNREAL_API int64 GameThreadBuildCalls();
	SUPERSLMUNREAL_API void ResetGameThreadBuildCalls();
#endif
#endif

#if WITH_DEV_AUTOMATION_TESTS
	// Test seam (cells 2.9, 9.7): builds made while one is alive are stamped as if built under another
	// Layer-1 pin (a non-empty tag or commit replaces this build's) or against another model (the
	// artifact hash's first byte flipped), so a test can make a set that a pin change or a model
	// reimport left stale without either happening. The entries' keys follow the stamped identity,
	// exactly as a build made under it would. CPU and GPU builds alike. A non-zero AdapterVendorId or
	// AdapterDeviceId stamps a GPU build as another device's (the payloads' own SPX1 headers are checked
	// against the real device first, and are unchanged), for the cells' other-device legs.
	struct SUPERSLMUNREAL_API FScopedBuildIdentityOverride
	{
		FScopedBuildIdentityOverride(const FString& Layer1Tag, const FString& Layer1Commit, bool bFlipArtifactHash,
			uint32 AdapterVendorId = 0, uint32 AdapterDeviceId = 0);
		~FScopedBuildIdentityOverride();
	};
#endif
}

#endif // SUPERSLM_WITH_PREFIX_CACHE
