#pragma once

// Cooked prefix sets (precache plan §5.8, §5.3 item 2, §5.9), with no Unreal and no Layer-1 call in
// it. USuperSLMPrefixSet is the asset; this is every rule about it that needs no engine:
//
//   - the manifest check: what an authored entry list may hold, and which entries the build makes
//     (CheckPrefixSetManifest);
//   - each entry's key under an identity, parents chained by name (ComputePrefixSetKeys);
//   - assembling one producer's build from the payloads the build produced (AssemblePrefixSetBuild),
//     and replacing that producer's earlier build in the set (ReplacePrefixSetBuild);
//   - cook validation: every entry whose stored bytes no longer match the identity the cook ships
//     with, or the entry as it is authored now, is a finding naming the entry (ValidatePrefixSetForCook).
//     The check is the runtime's own wrapper check (CheckPrefixWrapper), run early, so a cook warning
//     names exactly the entries the runtime will refuse and recompute;
//   - runtime selection: which build serves this producer (SelectPrefixSetBuild), and which of its
//     entries are registered with the cache's cooked tier (PrefixSetRegistrations). A producer with no
//     build, and an entry its build left empty, register nothing, so the request recomputes;
//   - entries defined by text (§5.8 *Entries defined by text*, rev 1.18, PC-16 (b)): the build's tokenize
//     pass (TokenizePrefixSetText), per-entry failure keeping an entry's previous bytes
//     (KeepPreviousPrefixSetBytes), the staleness rule (PrefixSetTextEntryCurrent) and what the cook ships
//     for a stale entry (PrefixSetStaleTextEntries, StripPrefixSetEntriesForCook). The ids stay the runtime
//     key: the runtime never tokenizes, and a Text entry's key, wrapper and request are a Tokens entry's
//     with the same ids.
//
// The stored bytes are whole 'SLUP' wrappers (SuperSLMPrefixCacheWrapper.h), the same bytes the disk
// tier writes; the cache's CheckCooked command checks them again at load, as it does a disk file.
//
// Producers: a CPU build serves every CPU (the CPU backend is deterministic across machines, and the
// SPX1 producer check is the engine's). A GPU build serves only the adapter vendor and device that
// built it (PC-5); GPU entries are built per device at U7, so until then a set has no GPU build and a
// GPU subsystem recomputes every entry.

#include "SuperSLMPrefixCacheKey.h"
#include "SuperSLMPrefixCacheWrapper.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace SuperSLMCore
{
	// How an entry is authored (§5.8, rev 1.18): by its token ids, or by text the build tokenizes. Tokens is
	// the default, and what every asset saved before the text fields existed loads as (cell 9.12).
	enum class EPrefixSetEntryInput : uint8_t
	{
		Tokens = 0,
		Text = 1,
	};

	// One authored entry (the asset's Entries[i]). ParentName empty = a root.
	struct FPrefixSetEntry
	{
		std::string Name;
		// The runtime key's ids. A Tokens entry's are authored; a Text entry's are the ids its last
		// successful build tokenized from Text (empty until then, and after an edit clears them).
		std::vector<int32_t> Tokens;
		std::string SchemaName; // empty = unbound
		std::string ParentName;
		EPrefixSetEntryInput Input = EPrefixSetEntryInput::Tokens;
		// A Text entry's text, UTF-8. A nested entry's is its suffix alone, tokenized alone.
		std::string Text;
		// What a Text entry's stored ids were tokenized from: the SHA-256 of Text then, and the model's
		// artifact hash then. Zeros = none (never built, or cleared by an edit). Unused for a Tokens entry.
		std::array<uint8_t, 32> TextHash{};
		std::array<uint8_t, 32> TokenizedModelHash{};
	};

	// One entry's stored bytes in a build. An empty Wrapper is an entry the build left empty.
	struct FPrefixSetBuiltEntry
	{
		std::string Name;
		std::vector<uint8_t> Wrapper;
	};

	// One producer's build: the identity it was built under (backend, adapter ids, Layer-1 tag and
	// commit, the model's artifact hash) and its entries' bytes.
	struct FPrefixSetBuild
	{
		FPrefixEngineIdentity Producer;
		std::vector<FPrefixSetBuiltEntry> Entries;
	};

	struct FPrefixSetModelShape
	{
		int64_t ContextCap = 0;
		int32_t VocabSize = 0;
	};

	enum class EPrefixSetFinding : uint8_t
	{
		// The manifest (an entry the build cannot make).
		EmptyName = 0,
		DuplicateName,
		EmptyTokens,
		TokenOutOfVocabulary,
		LongerThanContextCap,
		UnknownParent,
		ParentCycle,
		NestedRequiresBudgetMode, // a nested entry: the cache builds none until U6, so the build leaves it empty
		// Cook validation (stored bytes the runtime will refuse or never find; it recomputes them).
		NotBuilt,                 // no bytes for this entry in this build
		StaleLayer1Tag,           // built under another Layer-1 tag
		StaleLayer1Commit,        // built under another Layer-1 commit
		StaleArtifact,            // built against another model (its artifact hash)
		SpecEdited,               // the entry was edited after the build (its key or stored spec differs)
		WrapperInvalid,           // bytes that fail the wrapper's framing checks
		Orphaned,                 // bytes for an entry the set no longer has (they ship and are never used)
		// Entries defined by text (§5.8, rev 1.18). Appended, so the earlier values keep their numbers.
		TokenizeFailed,           // the build's tokenize pass failed for the entry (it keeps its previous ids and bytes)
		TextChanged,              // a Text entry whose text or model changed since its ids were built: the cook
		                          // ships it with no ids and no bytes, and the runtime serves nothing for it
		Count
	};
	constexpr uint32_t kPrefixSetFindingCount = static_cast<uint32_t>(EPrefixSetFinding::Count);

	const char* PrefixSetFindingName(EPrefixSetFinding Finding);

	struct FPrefixSetFindingLine
	{
		EPrefixSetFinding Finding = EPrefixSetFinding::Count;
		std::string EntryName;
		bool bHasProducer = false; // cook findings name the build; manifest findings do not
		uint8_t Backend = 0;
		uint32_t AdapterVendorId = 0;
		uint32_t AdapterDeviceId = 0;
		std::string Detail;
	};

	// "<set>: entry '<name>' [CPU]: <FindingName> (<detail>)", the line the cook and the build log.
	// [GPU <vendor hex>:<device hex>] for a GPU build; no bracket for a manifest finding.
	std::string FormatPrefixSetFinding(const std::string& SetName, const FPrefixSetFindingLine& Line);

	// The manifest check, in entry order. OutBuildable (optional) receives one flag per entry: true when
	// the build makes that entry now (no finding against it, and a root).
	std::vector<FPrefixSetFindingLine> CheckPrefixSetManifest(const std::vector<FPrefixSetEntry>& Entries,
		const FPrefixSetModelShape& Shape, std::vector<bool>* OutBuildable);

	// Each entry's stored spec and key under Identity; a nested entry's parent key is its parent
	// entry's key. OutHasKey[i] is false (and OutKeys[i] zero) for an entry whose parent chain does not
	// resolve (an unknown parent, a cycle, a duplicate name on the chain).
	void ComputePrefixSetKeys(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Identity,
		std::vector<FPrefixKey>& OutKeys, std::vector<FPrefixStoredSpec>& OutSpecs, std::vector<bool>& OutHasKey);

	// One producer's build from the build's payloads. Payloads[i] is entry i's SPX1 payload; empty =
	// the entry was not built (left empty). Each non-empty payload is wrapped under Producer with the
	// entry's key and full spec. Entries keep the manifest's order and names.
	FPrefixSetBuild AssemblePrefixSetBuild(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Producer,
		const std::vector<std::vector<uint8_t>>& Payloads);

	// True when Build serves Runtime: the same backend, and on the GPU the same adapter vendor and device.
	bool PrefixSetBuildServes(const FPrefixEngineIdentity& BuildProducer, const FPrefixEngineIdentity& Runtime);

	// Puts Build into Builds, replacing every earlier build for the same producer (a rebuild after a pin
	// or model change replaces, it never appends). Other producers' builds are kept.
	void ReplacePrefixSetBuild(std::vector<FPrefixSetBuild>& Builds, const FPrefixSetBuild& Build);

	// Cook validation. Current is the identity the cook ships with: this build's compiled Layer-1 tag
	// and commit and the set's model's artifact hash (its backend and adapter fields are ignored; each
	// build is checked for its own producer). Findings, in order: the manifest's; then, for each build
	// in order, each authored entry the build should hold (buildable now) in entry order, then the
	// build's orphaned bytes. A set with no CPU build reports NotBuilt for every buildable entry,
	// against the CPU. An empty result is a clean set.
	std::vector<FPrefixSetFindingLine> ValidatePrefixSetForCook(const std::vector<FPrefixSetEntry>& Entries,
		const std::vector<FPrefixSetBuild>& Builds, const FPrefixEngineIdentity& Current, const FPrefixSetModelShape& Shape);

	// The index of the build that serves Runtime, or -1 (the player's producer has no build: every
	// request recomputes). With more than one, the first.
	int32_t SelectPrefixSetBuild(const std::vector<FPrefixSetBuild>& Builds, const FPrefixEngineIdentity& Runtime);

	// What registers with the cache's cooked tier from one build: every entry with bytes whose key field
	// reads (ReadPrefixWrapperKey). An empty entry and unreadable bytes register nothing.
	struct FPrefixSetRegistration
	{
		size_t BuiltEntryIndex = 0; // into Build.Entries
		FPrefixKey Key;
	};
	std::vector<FPrefixSetRegistration> PrefixSetRegistrations(const FPrefixSetBuild& Build);

	// ---- Entries defined by text (§5.8 *Entries defined by text*; cells 2.17, 2.18, 7.20, 9.12) ----

	// The SHA-256 of a Text entry's UTF-8 text, as the entry stores it beside its ids.
	std::array<uint8_t, 32> PrefixSetTextHash(const std::string& Utf8Text);

	// True when the entry's stored ids may serve: always for a Tokens entry; for a Text entry, when it has ids,
	// its stored text hash is its text's, and its stored model hash is ModelArtifactHash (the set's model now).
	// A Text entry that fails this is TextChanged: warned at cook, shipped with no ids and no bytes, skipped by
	// registration and refused by FindEntrySpec. A stale model takes this path for a Text entry, not the
	// stale-artifact path, because its ids belong to the old tokenizer (rev 1.18.1, audit G8 (a)).
	bool PrefixSetTextEntryCurrent(const FPrefixSetEntry& Entry, const std::array<uint8_t, 32>& ModelArtifactHash);

	// The build's tokenize pass (§5.8 *Tokenized at build*), run on the game thread before any prefill. Tokenize
	// is SuperSLM::TokenizeText against the set's model: it gets the entry's own Text with nothing prepended or
	// appended (a nested entry's suffix alone) and returns false with an error on failure. Each Text entry with
	// non-empty text is tokenized once; Tokens entries are passed through untouched. A Text entry fails by name
	// when its text is empty (EmptyTokens, no call), the call fails (TokenizeFailed), the result is empty
	// (EmptyTokens) or longer than the context cap (LongerThanContextCap); a failing entry keeps its previous ids
	// and hashes, and Failed[i] is set so the build makes nothing for it and keeps its previous bytes. A
	// tokenized entry stores its new ids, PrefixSetTextHash(Text) and ModelArtifactHash.
	using FPrefixSetTokenizeFn = std::function<bool(const std::string& Utf8Text, std::vector<int32_t>& OutIds, std::string& OutError)>;
	struct FPrefixSetTokenizePass
	{
		std::vector<FPrefixSetEntry> Entries;
		std::vector<bool> Failed;
		std::vector<FPrefixSetFindingLine> Lines; // one per failing entry, CPU producer
		size_t Calls = 0;                         // tokenize calls made
	};
	FPrefixSetTokenizePass TokenizePrefixSetText(const std::vector<FPrefixSetEntry>& Entries, const FPrefixSetModelShape& Shape,
		const std::array<uint8_t, 32>& ModelArtifactHash, const FPrefixSetTokenizeFn& Tokenize);

	// Per-entry failure (§5.8, rev 1.18.1; cells 2.17, 3.11): every entry with Kept[i] set takes the bytes the
	// same producer's previous build stored under its name (none = empty), and every other entry keeps Built's
	// new bytes. Previous is the set's builds before this one; the build for Built's producer is the one read.
	void KeepPreviousPrefixSetBytes(FPrefixSetBuild& Built, const std::vector<FPrefixSetBuild>& Previous, const std::vector<bool>& Kept);

	// The entries the cook ships with no ids and no bytes: every Text entry not PrefixSetTextEntryCurrent under
	// ModelArtifactHash, in entry order.
	std::vector<size_t> PrefixSetStaleTextEntries(const std::vector<FPrefixSetEntry>& Entries, const std::array<uint8_t, 32>& ModelArtifactHash);

	// What the cook ships for those entries: their ids cleared, and their bytes cleared in every build. Their text
	// and hashes are kept, and every other entry and byte is unchanged. The source asset is never modified: the
	// cook applies this to a copy.
	void StripPrefixSetEntriesForCook(std::vector<FPrefixSetEntry>& Entries, std::vector<FPrefixSetBuild>& Builds, const std::vector<size_t>& Stale);

	// PrefixSetRegistrations, skipping the bytes of every entry that is not PrefixSetTextEntryCurrent under the
	// set's model (RegisterPrefixSet's rule: a TextChanged entry is not served, even from bytes an uncooked set
	// still carries).
	std::vector<FPrefixSetRegistration> PrefixSetRegistrations(const FPrefixSetBuild& Build, const std::vector<FPrefixSetEntry>& Entries,
		const std::array<uint8_t, 32>& SetModelArtifactHash);

	// The set's model against the subsystem's (§5.8 *The set's model against the subsystem's*, rev 1.18.2; cell
	// 2.18 [U3]). A set's ids, Text and Tokens entries alike, belong to its model's tokenizer, so a set whose
	// model's artifact hash is not the configured model's is refused by name, per set, never re-tokenized.
	// RegisterPrefixSet registers nothing for it; FindEntrySpec returns no spec.
	bool PrefixSetModelMatches(const std::array<uint8_t, 32>& SetModelArtifactHash,
		const std::array<uint8_t, 32>& ConfiguredModelArtifactHash);

	enum class EPrefixSetSpecResult : uint8_t
	{
		Ok = 0,
		PrefixSetModelMismatch = 1, // the set's model is not the configured model (checked first, for the whole set)
		UnknownEntry = 2,           // no entry of that name
		NoStoredIds = 3,            // the entry has no stored ids (a Text entry not built, cleared by an edit, or cooked stale)
		TextChanged = 4,            // a Text entry whose stored ids are not its text's under the set's model
	};

	const char* PrefixSetSpecResultName(EPrefixSetSpecResult Result);

	// FindEntrySpec's rule (§5.8 *How a game requests it*, rev 1.18.3: USuperSLMPrefixSet::FindEntrySpec(
	// ConfiguredModel, Entry, Out, OutParentEntry)). On Ok: the entry's stored ids and schema name, and its parent
	// entry's name (empty for a root). The parent's lease is the caller's: it requests the parent entry itself and
	// passes that lease as the spec's parent. Every other result leaves the outputs empty. Checked in order: the
	// set's model, the entry's name, stored ids, then (a Text entry) PrefixSetTextEntryCurrent under the set's model.
	EPrefixSetSpecResult FindPrefixSetEntrySpec(const std::vector<FPrefixSetEntry>& Entries,
		const std::array<uint8_t, 32>& SetModelArtifactHash, const std::array<uint8_t, 32>& ConfiguredModelArtifactHash,
		const std::string& EntryName, std::vector<int32_t>& OutTokens, std::string& OutSchemaName, std::string& OutParentEntry);
}
