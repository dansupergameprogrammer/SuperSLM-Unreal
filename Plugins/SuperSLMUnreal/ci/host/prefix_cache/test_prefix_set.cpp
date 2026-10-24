// Cooked prefix sets (precache plan §5.8; step U3): the manifest, the build's assembly, cook
// validation, runtime selection per producer, and the host twins of cells 2.9, 2.15 (cooked tier)
// and 9.7. The Unreal cells (SuperSLMPrefixCacheCookedTests.cpp) run the same rules through the
// asset, the editor build and a cook.

#include "cache_fixture.h"

#include "SuperSLMPrefixSetCore.h"

#include <set>

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	FPrefixSetModelShape FixtureShape()
	{
		FPrefixSetModelShape S;
		S.ContextCap = 64;
		S.VocabSize = 50000;
		return S;
	}

	FPrefixSetEntry Entry(const std::string& Name, int32_t Index, const std::string& Schema = std::string(), const std::string& Parent = std::string())
	{
		const FPrefixSpec Spec = FixtureSpec(Index, kFixtureLength, Schema);
		FPrefixSetEntry E;
		E.Name = Name;
		E.Tokens = Spec.Tokens;
		E.SchemaName = Schema;
		E.ParentName = Parent;
		return E;
	}

	FPrefixSpec SpecOf(const FPrefixSetEntry& E)
	{
		FPrefixSpec S;
		S.Tokens = E.Tokens;
		S.SchemaName = E.SchemaName;
		return S;
	}

	// What the editor's build produces: each buildable root's payload (the host's fake SPX1).
	std::vector<std::vector<uint8_t>> BuildPayloads(const std::vector<FPrefixSetEntry>& Entries)
	{
		std::vector<bool> Buildable;
		CheckPrefixSetManifest(Entries, FixtureShape(), &Buildable);
		std::vector<std::vector<uint8_t>> Payloads(Entries.size());
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			if (I < Buildable.size() && Buildable[I])
			{
				Payloads[I] = MakeFakePayload(Entries[I].SchemaName, Entries[I].Tokens);
			}
		}
		return Payloads;
	}

	FPrefixSetBuild BuildUnder(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Producer)
	{
		return AssemblePrefixSetBuild(Entries, Producer, BuildPayloads(Entries));
	}

	// One root entry built alone under Producer (empty if the build made nothing).
	FPrefixSetBuiltEntry BuiltAlone(const FPrefixSetEntry& E, const FPrefixEngineIdentity& Producer)
	{
		const FPrefixSetBuild B = BuildUnder({E}, Producer);
		return B.Entries.empty() ? FPrefixSetBuiltEntry() : B.Entries[0];
	}

	// The runtime's registration (the plugin's RegisterPrefixSet): the selected build's entries into
	// the cooked tier. Returns how many were registered.
	size_t RegisterSet(FCacheFixture& F, const std::vector<FPrefixSetBuild>& Builds, uint64_t FirstBlobId = 100)
	{
		const int32_t Index = SelectPrefixSetBuild(Builds, F.Config.Identity);
		if (Index < 0)
		{
			return 0;
		}
		const FPrefixSetBuild& Build = Builds[static_cast<size_t>(Index)];
		size_t N = 0;
		for (const FPrefixSetRegistration& R : PrefixSetRegistrations(Build))
		{
			const uint64_t Id = FirstBlobId + N++;
			F.Sim->Cooked[Id] = Build.Entries[R.BuiltEntryIndex].Wrapper;
			F.Cache.RegisterCooked(R.Key, Id);
		}
		return N;
	}

	size_t CountFindings(const std::vector<FPrefixSetFindingLine>& Lines, EPrefixSetFinding Finding, const std::string& Entry)
	{
		size_t N = 0;
		for (const FPrefixSetFindingLine& L : Lines)
		{
			if (L.Finding == Finding && L.EntryName == Entry)
			{
				++N;
			}
		}
		return N;
	}

	FPrefixEngineIdentity GpuProducer(uint32_t Vendor, uint32_t Device)
	{
		FPrefixEngineIdentity Id = FixtureIdentity();
		Id.Backend = 1;
		Id.AdapterVendorId = Vendor;
		Id.AdapterDeviceId = Device;
		return Id;
	}

	bool ServedFrom(FCacheFixture& F, const FPrefixSetEntry& E, EPrefixSource Source)
	{
		const int64_t H = F.RequestReady(SpecOf(E));
		if (H == 0 || F.Cache.GetSource(H) != Source)
		{
			return false;
		}
		const FEngineHandle* Handle = F.Sim->HandleOf(H);
		return Handle != nullptr && Handle->Tokens == E.Tokens && Handle->Schema == E.SchemaName
			&& Handle->bFromRestore == (Source != EPrefixSource::Recomputed);
	}
}

HOST_TEST(prefix_set_finding_names_are_distinct, "§5.8 (findings by name)")
{
	std::set<std::string> Names;
	for (uint32_t I = 0; I < kPrefixSetFindingCount; ++I)
	{
		const std::string Name = PrefixSetFindingName(static_cast<EPrefixSetFinding>(I));
		EXPECT_NE(Name, std::string("Unknown"));
		Names.insert(Name);
	}
	EXPECT_EQ(Names.size(), static_cast<size_t>(kPrefixSetFindingCount));
	EXPECT_EQ(std::string(PrefixSetFindingName(EPrefixSetFinding::SpecEdited)), std::string("SpecEdited"));
}

HOST_TEST(prefix_set_manifest_rules, "§5.8 (entries), §5.6 (malformed spec), §5.9 (nested before U6)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("greeting", 1));                       // 0 ok
	E.push_back(Entry("", 2));                               // 1 EmptyName
	E.push_back(Entry("greeting", 3));                       // 2 DuplicateName (the second)
	FPrefixSetEntry Empty = Entry("empty", 4);
	Empty.Tokens.clear();
	E.push_back(Empty);                                      // 3 EmptyTokens
	FPrefixSetEntry Oov = Entry("oov", 5);
	Oov.Tokens[3] = 50000;
	E.push_back(Oov);                                        // 4 TokenOutOfVocabulary
	FPrefixSetEntry Negative = Entry("negative", 6);
	Negative.Tokens[0] = -1;
	E.push_back(Negative);                                   // 5 TokenOutOfVocabulary
	FPrefixSetEntry Long = Entry("long", 7);
	Long.Tokens.resize(65, 1);
	E.push_back(Long);                                       // 6 LongerThanContextCap
	FPrefixSetEntry AtCap = Entry("at_cap", 8);
	AtCap.Tokens.resize(64, 1);
	E.push_back(AtCap);                                      // 7 ok (exactly the cap)
	E.push_back(Entry("orphan_child", 9, "", "nobody"));     // 8 UnknownParent
	E.push_back(Entry("loop_a", 10, "", "loop_b"));          // 9 ParentCycle
	E.push_back(Entry("loop_b", 11, "", "loop_a"));          // 10 ParentCycle
	E.push_back(Entry("child", 12, "", "greeting"));         // 11 NestedRequiresBudgetMode
	E.push_back(Entry("bound", 13, "schema_two"));           // 12 ok

	std::vector<bool> Buildable;
	const std::vector<FPrefixSetFindingLine> F = CheckPrefixSetManifest(E, FixtureShape(), &Buildable);
	REQUIRE_EQ(Buildable.size(), E.size());
	const bool Expected[] = {true, false, false, false, false, false, false, true, false, false, false, false, true};
	for (size_t I = 0; I < E.size(); ++I)
	{
		EXPECT_EQ(Buildable[I], Expected[I]);
	}
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::EmptyName, ""), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::DuplicateName, "greeting"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::EmptyTokens, "empty"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::TokenOutOfVocabulary, "oov"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::TokenOutOfVocabulary, "negative"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::LongerThanContextCap, "long"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::UnknownParent, "orphan_child"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::ParentCycle, "loop_a"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::ParentCycle, "loop_b"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::NestedRequiresBudgetMode, "child"), static_cast<size_t>(1));
	EXPECT_EQ(F.size(), static_cast<size_t>(10)); // one finding per refused entry (ten), none for the three ok entries
	for (const FPrefixSetFindingLine& L : F)
	{
		EXPECT_EQ(L.bHasProducer, false);
		const std::string Line = FormatPrefixSetFinding("/Game/Sets/Greetings", L);
		EXPECT_TRUE(Contains(Line, "/Game/Sets/Greetings"));
		EXPECT_TRUE(Contains(Line, PrefixSetFindingName(L.Finding)));
		EXPECT_TRUE(Contains(Line, "'" + L.EntryName + "'"));
	}
}

HOST_TEST(prefix_set_keys_chain_parents, "§5.2 (the parent's key), §5.8 (parent entry name)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("root", 1, "schema_one"));
	E.push_back(Entry("child", 2, "", "root"));
	E.push_back(Entry("grandchild", 3, "", "child"));
	E.push_back(Entry("stray", 4, "", "missing"));
	std::vector<FPrefixKey> Keys;
	std::vector<FPrefixStoredSpec> Specs;
	std::vector<bool> Has;
	ComputePrefixSetKeys(E, FixtureIdentity(), Keys, Specs, Has);
	REQUIRE_EQ(Keys.size(), E.size());
	REQUIRE_EQ(Specs.size(), E.size());
	REQUIRE_EQ(Has.size(), E.size());

	const FPrefixKey RootKey = FixtureKey(SpecOf(E[0]));
	EXPECT_TRUE(Has[0]);
	EXPECT_TRUE(Keys[0] == RootKey);
	EXPECT_EQ(Specs[0].SchemaName, std::string("schema_one"));
	EXPECT_EQ(Specs[0].bHasParent, false);

	EXPECT_TRUE(Has[1]);
	EXPECT_EQ(Specs[1].bHasParent, true);
	EXPECT_TRUE(Specs[1].ParentKey == RootKey);
	EXPECT_TRUE(Keys[1] == ComputePrefixKey(KeyInputsFor(FixtureIdentity(), Specs[1])));
	EXPECT_TRUE(Keys[1] != FixtureKey(SpecOf(E[1]))); // the same tokens as a root are another key

	EXPECT_TRUE(Has[2]);
	EXPECT_TRUE(Specs[2].ParentKey == Keys[1]);

	EXPECT_EQ(Has[3], false);
	EXPECT_TRUE(Keys[3] == FPrefixKey());
}

HOST_TEST(prefix_set_assemble_wraps_each_payload_under_its_key, "§5.7 (the cooked wrapper), §5.8 (build)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	E.push_back(Entry("b", 2, "schema_two"));
	E.push_back(Entry("c", 3, "", "a")); // nested: left empty
	const FPrefixSetBuild B = BuildUnder(E, FixtureIdentity());
	REQUIRE_EQ(B.Entries.size(), E.size());
	EXPECT_EQ(B.Producer.Layer1Tag, FixtureIdentity().Layer1Tag);
	for (size_t I = 0; I < 2; ++I)
	{
		EXPECT_EQ(B.Entries[I].Name, E[I].Name);
		size_t Offset = 0;
		size_t Size = 0;
		const FPrefixSpec S = SpecOf(E[I]);
		EXPECT_EQ(CheckPrefixWrapper(B.Entries[I].Wrapper.data(), B.Entries[I].Wrapper.size(), FixtureIdentity(), FixtureKey(S), StoredOf(S), Offset, Size),
			EPrefixWrapperCheck::Ok);
		EXPECT_EQ(Size, MakeFakePayload(S.SchemaName, S.Tokens).size());
	}
	EXPECT_EQ(B.Entries[2].Name, std::string("c"));
	EXPECT_TRUE(B.Entries[2].Wrapper.empty());
}

HOST_TEST(prefix_set_fresh_build_validates_clean, "§5.8 (cook validation), 9.7 (after the rebuild)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	E.push_back(Entry("b", 2, "schema_two"));
	const std::vector<FPrefixSetBuild> Builds = {BuildUnder(E, FixtureIdentity())};
	const std::vector<FPrefixSetFindingLine> F = ValidatePrefixSetForCook(E, Builds, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(F.size(), static_cast<size_t>(0));
	for (const FPrefixSetFindingLine& L : F)
	{
		HostTest::Fail(__FILE__, __LINE__, "unexpected finding: " + FormatPrefixSetFinding("set", L));
	}
}

// Cell 2.9's host twin: a stale tag, a stale artifact hash and a spec edited after the build are each
// a cook finding naming the entry, and the runtime serves each by recompute; the fresh entry is a
// cooked hit.
HOST_TEST(prefix_set_stale_entries_warn_at_cook_and_recompute_at_runtime, "2.9 (host twin)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("fresh", 1));
	E.push_back(Entry("old_tag", 2));
	E.push_back(Entry("old_model", 3));
	E.push_back(Entry("edited", 4));
	FPrefixSetBuild Build = BuildUnder(E, FixtureIdentity());
	REQUIRE_EQ(Build.Entries.size(), E.size());

	FPrefixEngineIdentity OldPin = FixtureIdentity();
	OldPin.Layer1Tag = "v1.13.0";
	Build.Entries[1] = BuiltAlone(E[1], OldPin);
	FPrefixEngineIdentity OldModel = FixtureIdentity();
	OldModel.ArtifactHash[5] ^= 0x5A;
	Build.Entries[2] = BuiltAlone(E[2], OldModel);
	E[3].Tokens[7] += 1; // edited after the build

	const std::vector<FPrefixSetBuild> Builds = {Build};
	const std::vector<FPrefixSetFindingLine> F = ValidatePrefixSetForCook(E, Builds, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::StaleLayer1Tag, "old_tag"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::StaleArtifact, "old_model"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::SpecEdited, "edited"), static_cast<size_t>(1));
	EXPECT_EQ(F.size(), static_cast<size_t>(3));
	for (const FPrefixSetFindingLine& L : F)
	{
		EXPECT_EQ(L.bHasProducer, true);
		EXPECT_EQ(L.Backend, static_cast<uint8_t>(0));
		const std::string Line = FormatPrefixSetFinding("/Game/Sets/Npc", L);
		EXPECT_TRUE(Contains(Line, "'" + L.EntryName + "'"));
		EXPECT_TRUE(Contains(Line, "[CPU]"));
		EXPECT_TRUE(Contains(Line, PrefixSetFindingName(L.Finding)));
	}

	// The stale bytes ship and are refused or never found at runtime: a recompute, never used.
	FCacheFixture C(8, false);
	REQUIRE_TRUE(C.bConfigured);
	EXPECT_EQ(RegisterSet(C, Builds), static_cast<size_t>(4));
	EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Cooked));
	EXPECT_TRUE(ServedFrom(C, E[1], EPrefixSource::Recomputed));
	EXPECT_TRUE(ServedFrom(C, E[2], EPrefixSource::Recomputed));
	EXPECT_TRUE(ServedFrom(C, E[3], EPrefixSource::Recomputed));
	EXPECT_EQ(C.Diag().ServedBySource[static_cast<int>(EPrefixSource::Cooked)], 1u);
	EXPECT_EQ(C.Diag().ServedBySource[static_cast<int>(EPrefixSource::Recomputed)], 3u);
	EXPECT_EQ(C.Sim->Violations.size(), static_cast<size_t>(0));
}

HOST_TEST(prefix_set_cook_reports_unbuilt_orphaned_and_invalid_bytes, "§5.8 (cook validation)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	E.push_back(Entry("b", 2));
	E.push_back(Entry("c", 3));
	E.push_back(Entry("nested", 4, "", "a"));
	FPrefixSetBuild Build = BuildUnder(E, FixtureIdentity());
	REQUIRE_EQ(Build.Entries.size(), E.size());
	Build.Entries[1].Wrapper.clear();                       // b left empty
	Build.Entries[2].Wrapper.resize(40);                    // c cut short
	FPrefixSetBuiltEntry Gone;
	Gone.Name = "removed";
	Gone.Wrapper = BuiltAlone(Entry("removed", 9), FixtureIdentity()).Wrapper;
	Build.Entries.push_back(Gone);                          // bytes for an entry the set no longer has

	const std::vector<FPrefixSetFindingLine> F = ValidatePrefixSetForCook(E, {Build}, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::NestedRequiresBudgetMode, "nested"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::NotBuilt, "b"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::WrapperInvalid, "c"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::Orphaned, "removed"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(F, EPrefixSetFinding::NotBuilt, "nested"), static_cast<size_t>(0)); // the manifest finding covers it
	EXPECT_EQ(F.size(), static_cast<size_t>(4));
	REQUIRE_GE(F.size(), static_cast<size_t>(1));
	EXPECT_EQ(F[0].Finding, EPrefixSetFinding::NestedRequiresBudgetMode); // the manifest's findings come first

	// A set never built for the CPU: every buildable entry is NotBuilt against the CPU.
	const std::vector<FPrefixSetFindingLine> Unbuilt = ValidatePrefixSetForCook(E, {}, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(CountFindings(Unbuilt, EPrefixSetFinding::NotBuilt, "a"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(Unbuilt, EPrefixSetFinding::NotBuilt, "b"), static_cast<size_t>(1));
	EXPECT_EQ(CountFindings(Unbuilt, EPrefixSetFinding::NotBuilt, "c"), static_cast<size_t>(1));
	EXPECT_EQ(Unbuilt.size(), static_cast<size_t>(4)); // with the manifest's nested finding
}

// Cell 9.7's host twin: a set built under one pin is all recompute after a pin change, warns at cook,
// and after a rebuild (which replaces that producer's build) validates clean and serves cooked hits.
HOST_TEST(prefix_set_rebuild_after_pin_change_serves_hits_again, "9.7 (host twin)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	E.push_back(Entry("b", 2, "schema_two"));
	FPrefixEngineIdentity OldPin = FixtureIdentity();
	OldPin.Layer1Tag = "v1.13.0";
	OldPin.Layer1Commit = "002094a6da8ba33450e6fabc08503b22e35d1610";
	FPrefixEngineIdentity OldGpu = GpuProducer(0x10DE, 0x1E84);
	OldGpu.Layer1Tag = OldPin.Layer1Tag;
	OldGpu.Layer1Commit = OldPin.Layer1Commit;
	std::vector<FPrefixSetBuild> Builds = {BuildUnder(E, OldPin), BuildUnder(E, OldGpu)};

	const std::vector<FPrefixSetFindingLine> Before = ValidatePrefixSetForCook(E, Builds, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(CountFindings(Before, EPrefixSetFinding::StaleLayer1Tag, "a"), static_cast<size_t>(2)); // the CPU and the GPU build
	EXPECT_EQ(CountFindings(Before, EPrefixSetFinding::StaleLayer1Tag, "b"), static_cast<size_t>(2));
	EXPECT_EQ(Before.size(), static_cast<size_t>(4));
	{
		FCacheFixture C(8, false);
		REQUIRE_TRUE(C.bConfigured);
		RegisterSet(C, Builds);
		EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Recomputed));
		EXPECT_TRUE(ServedFrom(C, E[1], EPrefixSource::Recomputed));
	}

	ReplacePrefixSetBuild(Builds, BuildUnder(E, FixtureIdentity()));
	REQUIRE_EQ(Builds.size(), static_cast<size_t>(2)); // replaced, not appended; the GPU build kept
	EXPECT_EQ(SelectPrefixSetBuild(Builds, FixtureIdentity()), 1); // the rebuilt CPU build goes last
	EXPECT_EQ(Builds[0].Producer.Backend, static_cast<uint8_t>(1));
	const std::vector<FPrefixSetFindingLine> After = ValidatePrefixSetForCook(E, Builds, FixtureIdentity(), FixtureShape());
	EXPECT_EQ(CountFindings(After, EPrefixSetFinding::StaleLayer1Tag, "a"), static_cast<size_t>(1)); // the GPU build is still old
	EXPECT_EQ(CountFindings(After, EPrefixSetFinding::StaleLayer1Tag, "b"), static_cast<size_t>(1));
	for (const FPrefixSetFindingLine& L : After)
	{
		EXPECT_EQ(L.Backend, static_cast<uint8_t>(1)); // nothing left against the rebuilt CPU build
	}
	{
		FCacheFixture C(8, false);
		REQUIRE_TRUE(C.bConfigured);
		EXPECT_EQ(RegisterSet(C, Builds), static_cast<size_t>(2));
		EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Cooked));
		EXPECT_TRUE(ServedFrom(C, E[1], EPrefixSource::Cooked));
		EXPECT_EQ(C.Diag().BuildsIssued, 0u); // no prefix prefill ran
	}
}

// Cell 2.15's cooked half, host twin: a spec bound to a non-first schema is served from the cooked
// tier with its binding and tokens.
HOST_TEST(prefix_set_schema_bound_entry_is_a_cooked_hit, "2.15 (cooked tier, host twin)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("index2", 1, "schema_two"));
	FCacheFixture C(4, false);
	REQUIRE_TRUE(C.bConfigured);
	EXPECT_EQ(RegisterSet(C, {BuildUnder(E, FixtureIdentity())}), static_cast<size_t>(1));
	EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Cooked));
	// The same tokens bound to another schema are another key: not served from the set.
	FPrefixSetEntry Other = E[0];
	Other.SchemaName = "schema_one";
	EXPECT_TRUE(ServedFrom(C, Other, EPrefixSource::Recomputed));
}

// "A player's device without an entry recomputes" (U7's rule, CPU now): a producer with no build, or
// an entry its build left empty, registers nothing, and the request recomputes.
HOST_TEST(prefix_set_producer_without_an_entry_recomputes, "§5.8 (per-producer entries), §5.9")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	E.push_back(Entry("b", 2));

	// Builds for one GPU device only.
	const std::vector<FPrefixSetBuild> GpuOnly = {BuildUnder(E, GpuProducer(0x10DE, 0x1E84))};
	EXPECT_EQ(SelectPrefixSetBuild(GpuOnly, FixtureIdentity()), -1);
	EXPECT_EQ(SelectPrefixSetBuild(GpuOnly, GpuProducer(0x1002, 0x744C)), -1);  // another vendor
	EXPECT_EQ(SelectPrefixSetBuild(GpuOnly, GpuProducer(0x10DE, 0x2204)), -1);  // the same vendor, another device
	EXPECT_EQ(SelectPrefixSetBuild(GpuOnly, GpuProducer(0x10DE, 0x1E84)), 0);
	{
		FCacheFixture C(4, false);
		REQUIRE_TRUE(C.bConfigured);
		EXPECT_EQ(RegisterSet(C, GpuOnly), static_cast<size_t>(0));
		EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Recomputed));
		EXPECT_EQ(C.Sim->CountHistory(EPrefixCommandKind::CheckCooked), static_cast<size_t>(0));
	}

	// A CPU build serves any CPU, whatever adapter ids a runtime identity carries.
	FPrefixEngineIdentity OtherCpu = FixtureIdentity();
	OtherCpu.AdapterVendorId = 7;
	const std::vector<FPrefixSetBuild> Cpu = {BuildUnder(E, FixtureIdentity())};
	REQUIRE_EQ(Cpu[0].Entries.size(), E.size());
	EXPECT_EQ(SelectPrefixSetBuild(Cpu, OtherCpu), 0);
	EXPECT_EQ(PrefixSetBuildServes(Cpu[0].Producer, OtherCpu), true);
	EXPECT_EQ(PrefixSetBuildServes(Cpu[0].Producer, GpuProducer(0, 0)), false);

	// An entry the build left empty recomputes; the others are cooked hits.
	std::vector<FPrefixSetBuild> Partial = Cpu;
	Partial[0].Entries[1].Wrapper.clear();
	const std::vector<FPrefixSetRegistration> R = PrefixSetRegistrations(Partial[0]);
	REQUIRE_EQ(R.size(), static_cast<size_t>(1));
	EXPECT_EQ(R[0].BuiltEntryIndex, static_cast<size_t>(0));
	EXPECT_TRUE(R[0].Key == FixtureKey(SpecOf(E[0])));
	{
		FCacheFixture C(4, false);
		REQUIRE_TRUE(C.bConfigured);
		EXPECT_EQ(RegisterSet(C, Partial), static_cast<size_t>(1));
		EXPECT_TRUE(ServedFrom(C, E[0], EPrefixSource::Cooked));
		EXPECT_TRUE(ServedFrom(C, E[1], EPrefixSource::Recomputed));
	}

	// Unreadable bytes register nothing (the cook names them WrapperInvalid).
	std::vector<FPrefixSetBuild> Broken = Cpu;
	Broken[0].Entries[0].Wrapper.resize(20);
	EXPECT_EQ(PrefixSetRegistrations(Broken[0]).size(), static_cast<size_t>(1));
}

HOST_TEST(prefix_set_replace_keeps_other_producers, "§5.8 (one build per producer)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(Entry("a", 1));
	FPrefixEngineIdentity OldPin = FixtureIdentity();
	OldPin.Layer1Tag = "v1.13.0";
	std::vector<FPrefixSetBuild> Builds;
	ReplacePrefixSetBuild(Builds, BuildUnder(E, OldPin));
	ReplacePrefixSetBuild(Builds, BuildUnder(E, GpuProducer(0x10DE, 0x1E84)));
	ReplacePrefixSetBuild(Builds, BuildUnder(E, GpuProducer(0x1002, 0x744C)));
	ReplacePrefixSetBuild(Builds, BuildUnder(E, OldPin)); // a duplicate CPU build, replaced
	REQUIRE_EQ(Builds.size(), static_cast<size_t>(3));
	ReplacePrefixSetBuild(Builds, BuildUnder(E, FixtureIdentity()));
	REQUIRE_EQ(Builds.size(), static_cast<size_t>(3));
	size_t CpuBuilds = 0;
	for (const FPrefixSetBuild& B : Builds)
	{
		if (B.Producer.Backend == 0)
		{
			++CpuBuilds;
			EXPECT_EQ(B.Producer.Layer1Tag, FixtureIdentity().Layer1Tag);
		}
	}
	EXPECT_EQ(CpuBuilds, static_cast<size_t>(1));
	EXPECT_NE(SelectPrefixSetBuild(Builds, GpuProducer(0x1002, 0x744C)), -1);
}
