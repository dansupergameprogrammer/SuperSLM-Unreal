// Entries defined by text (precache plan §5.8 *Entries defined by text*, rev 1.18, PC-16 (b); rev 1.18.1 audit
// G7, G8; rev 1.18.2 lesser plane 8): the host twins of cells 2.17, 2.18 and 7.20. The Unreal cells
// (SuperSLMPrefixSetTextTests.cpp) run the same rules through the asset, the real tokenizer, the editor build and
// the cook hook.
//
// The fake tokenizer here maps each byte to (byte + 100), except that the pair "ab" merges into one id (900), and
// fails on any text containing "!fail". So a nested entry whose parent text ends in "a" and whose suffix starts
// with "b" satisfies tokenize(parent ‖ suffix) ≠ tokenize(parent) ‖ tokenize(suffix), stated and asserted below,
// and the mutant "tokenize the whole text, then split" is red.

#include "cache_fixture.h"

#include "SuperSLMPrefixSetCore.h"

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	FPrefixSetModelShape TextShape()
	{
		FPrefixSetModelShape S;
		S.ContextCap = 64;
		S.VocabSize = 50000;
		return S;
	}

	bool FakeTokenize(const std::string& Text, std::vector<int32_t>& Out, std::string& Error)
	{
		Out.clear();
		if (Text.find("!fail") != std::string::npos)
		{
			Error = "sslm_tokenize failed (ArtifactRejected): the artifact carries no tokenizer";
			return false;
		}
		for (size_t I = 0; I < Text.size(); ++I)
		{
			if (Text[I] == 'a' && I + 1 < Text.size() && Text[I + 1] == 'b')
			{
				Out.push_back(900);
				++I;
				continue;
			}
			Out.push_back(static_cast<int32_t>(static_cast<unsigned char>(Text[I])) + 100);
		}
		return true;
	}

	std::vector<int32_t> Tok(const std::string& Text)
	{
		std::vector<int32_t> Out;
		std::string Error;
		FakeTokenize(Text, Out, Error);
		return Out;
	}

	std::array<uint8_t, 32> ModelHash()
	{
		return FixtureIdentity().ArtifactHash;
	}

	std::array<uint8_t, 32> OtherModelHash()
	{
		std::array<uint8_t, 32> H = FixtureIdentity().ArtifactHash;
		H[7] ^= 0x3C;
		return H;
	}

	FPrefixSetEntry TokensEntry(const std::string& Name, std::vector<int32_t> Ids, const std::string& Parent = std::string())
	{
		FPrefixSetEntry E;
		E.Name = Name;
		E.Tokens = std::move(Ids);
		E.ParentName = Parent;
		return E;
	}

	FPrefixSetEntry TextEntry(const std::string& Name, const std::string& Text, const std::string& Parent = std::string())
	{
		FPrefixSetEntry E;
		E.Name = Name;
		E.Input = EPrefixSetEntryInput::Text;
		E.Text = Text;
		E.ParentName = Parent;
		return E;
	}

	// A Text entry as a successful build left it: its ids, its text's hash and the model's.
	FPrefixSetEntry BuiltTextEntry(const std::string& Name, const std::string& Text, const std::array<uint8_t, 32>& Model = ModelHash())
	{
		FPrefixSetEntry E = TextEntry(Name, Text);
		E.Tokens = Tok(Text);
		E.TextHash = PrefixSetTextHash(Text);
		E.TokenizedModelHash = Model;
		return E;
	}

	FPrefixSetTokenizePass Pass(const std::vector<FPrefixSetEntry>& Entries, const std::array<uint8_t, 32>& Model = ModelHash())
	{
		return TokenizePrefixSetText(Entries, TextShape(), Model, &FakeTokenize);
	}

	// The build as the editor runs it after the tokenize pass: each buildable, not-failed root's fake payload,
	// assembled under Producer, then the failing entries' previous bytes kept.
	FPrefixSetBuild BuildAfterPass(const FPrefixSetTokenizePass& P, const FPrefixEngineIdentity& Producer, const std::vector<FPrefixSetBuild>& Previous)
	{
		std::vector<bool> Buildable;
		CheckPrefixSetManifest(P.Entries, TextShape(), &Buildable);
		std::vector<std::vector<uint8_t>> Payloads(P.Entries.size());
		for (size_t I = 0; I < P.Entries.size(); ++I)
		{
			if (Buildable[I] && !P.Failed[I])
			{
				Payloads[I] = MakeFakePayload(P.Entries[I].SchemaName, P.Entries[I].Tokens);
			}
		}
		FPrefixSetBuild B = AssemblePrefixSetBuild(P.Entries, Producer, Payloads);
		KeepPreviousPrefixSetBytes(B, Previous, P.Failed);
		return B;
	}

	FPrefixSetBuild BuildAll(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Producer = FixtureIdentity())
	{
		std::vector<bool> Buildable;
		CheckPrefixSetManifest(Entries, TextShape(), &Buildable);
		std::vector<std::vector<uint8_t>> Payloads(Entries.size());
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			if (Buildable[I])
			{
				Payloads[I] = MakeFakePayload(Entries[I].SchemaName, Entries[I].Tokens);
			}
		}
		return AssemblePrefixSetBuild(Entries, Producer, Payloads);
	}

	size_t Count(const std::vector<FPrefixSetFindingLine>& Lines, EPrefixSetFinding Finding, const std::string& Entry)
	{
		size_t N = 0;
		for (const FPrefixSetFindingLine& L : Lines)
		{
			N += L.Finding == Finding && L.EntryName == Entry ? 1 : 0;
		}
		return N;
	}

	const FPrefixSetBuiltEntry* BuiltNamed(const FPrefixSetBuild& B, const std::string& Name)
	{
		for (const FPrefixSetBuiltEntry& E : B.Entries)
		{
			if (E.Name == Name)
			{
				return &E;
			}
		}
		return nullptr;
	}

	FPrefixSpec SpecOfIds(const std::vector<int32_t>& Ids)
	{
		FPrefixSpec S;
		S.Tokens = Ids;
		return S;
	}

	// RegisterPrefixSet's rule (the plugin host's RegisterCookedSet): the set's model against the configured
	// model first, then the selected build's entries, skipping every TextChanged entry.
	size_t RegisterTextSet(FCacheFixture& F, const std::vector<FPrefixSetEntry>& Entries, const std::vector<FPrefixSetBuild>& Builds,
		const std::array<uint8_t, 32>& SetModel, uint64_t FirstBlobId = 300)
	{
		if (!PrefixSetModelMatches(SetModel, F.Config.Identity.ArtifactHash))
		{
			return 0;
		}
		const int32_t Index = SelectPrefixSetBuild(Builds, F.Config.Identity);
		if (Index < 0)
		{
			return 0;
		}
		const FPrefixSetBuild& Build = Builds[static_cast<size_t>(Index)];
		size_t N = 0;
		for (const FPrefixSetRegistration& R : PrefixSetRegistrations(Build, Entries, SetModel))
		{
			const uint64_t Id = FirstBlobId + N++;
			F.Sim->Cooked[Id] = Build.Entries[R.BuiltEntryIndex].Wrapper;
			F.Cache.RegisterCooked(R.Key, Id);
		}
		return N;
	}

	EPrefixSource SourceOf(FCacheFixture& F, const std::vector<int32_t>& Ids)
	{
		const int64_t H = F.RequestReady(SpecOfIds(Ids));
		return H == 0 ? EPrefixSource::None : F.Cache.GetSource(H);
	}
}

HOST_TEST(prefix_set_text_hash_is_sha256, "§5.8 (what is stored: the text's SHA-256)")
{
	const std::array<uint8_t, 32> H = PrefixSetTextHash("abc");
	FPrefixKey AsKey;
	AsKey.Bytes = H;
	EXPECT_EQ(PrefixKeyToHex(AsKey), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	EXPECT_TRUE(PrefixSetTextHash("abc") != PrefixSetTextHash("abd"));
	EXPECT_EQ(std::string(PrefixSetFindingName(EPrefixSetFinding::TextChanged)), std::string("TextChanged"));
	EXPECT_EQ(std::string(PrefixSetFindingName(EPrefixSetFinding::TokenizeFailed)), std::string("TokenizeFailed"));
	EXPECT_EQ(std::string(PrefixSetSpecResultName(EPrefixSetSpecResult::TextChanged)), std::string("TextChanged"));
	FPrefixSetEntry Default;
	EXPECT_TRUE(Default.Input == EPrefixSetEntryInput::Tokens); // what an asset saved before the text fields loads as (9.12)
}

HOST_TEST(prefix_set_text_tokenize_pass_fails_per_entry, "2.17 (host twin: the edges, per-entry failure)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(TokensEntry("tokens", {5, 6, 7}));                    // 0 passed through, no call
	E.push_back(TextEntry("greeting", "hello there"));                // 1 tokenized
	FPrefixSetEntry Empty = BuiltTextEntry("empty", "was here");
	Empty.Text.clear();
	E.push_back(Empty);                                               // 2 EmptyTokens, no call; keeps its previous ids
	FPrefixSetEntry Long = BuiltTextEntry("long", "short before");
	Long.Text = std::string(65, 'x');
	E.push_back(Long);                                                // 3 LongerThanContextCap; keeps its previous ids
	FPrefixSetEntry NoTokenizer = BuiltTextEntry("no_tokenizer", "fine before");
	NoTokenizer.Text = "now !fail";
	E.push_back(NoTokenizer);                                         // 4 TokenizeFailed; keeps its previous ids
	E.push_back(TextEntry("at_cap", std::string(64, 'y')));           // 5 tokenized (exactly the cap)

	const FPrefixSetTokenizePass P = Pass(E);
	REQUIRE_EQ(P.Entries.size(), E.size());
	REQUIRE_EQ(P.Failed.size(), E.size());
	EXPECT_EQ(P.Calls, static_cast<size_t>(4)); // greeting, long, no_tokenizer, at_cap; never a Tokens entry or empty text
	const bool ExpectedFailed[] = {false, false, true, true, true, false};
	for (size_t I = 0; I < E.size(); ++I)
	{
		EXPECT_EQ(P.Failed[I], ExpectedFailed[I]);
	}
	EXPECT_TRUE(P.Entries[0].Tokens == E[0].Tokens);
	EXPECT_TRUE(P.Entries[1].Tokens == Tok("hello there"));
	EXPECT_TRUE(P.Entries[1].TextHash == PrefixSetTextHash("hello there"));
	EXPECT_TRUE(P.Entries[1].TokenizedModelHash == ModelHash());
	EXPECT_TRUE(P.Entries[5].Tokens.size() == static_cast<size_t>(64));
	for (size_t I : {static_cast<size_t>(2), static_cast<size_t>(3), static_cast<size_t>(4)})
	{
		// A failing entry keeps its previous ids and hashes, exactly.
		EXPECT_TRUE(P.Entries[I].Tokens == E[I].Tokens);
		EXPECT_TRUE(P.Entries[I].TextHash == E[I].TextHash);
		EXPECT_TRUE(P.Entries[I].TokenizedModelHash == E[I].TokenizedModelHash);
	}
	EXPECT_EQ(Count(P.Lines, EPrefixSetFinding::EmptyTokens, "empty"), static_cast<size_t>(1));
	EXPECT_EQ(Count(P.Lines, EPrefixSetFinding::LongerThanContextCap, "long"), static_cast<size_t>(1));
	EXPECT_EQ(Count(P.Lines, EPrefixSetFinding::TokenizeFailed, "no_tokenizer"), static_cast<size_t>(1));
	EXPECT_EQ(P.Lines.size(), static_cast<size_t>(3));
	for (const FPrefixSetFindingLine& L : P.Lines)
	{
		const std::string Line = FormatPrefixSetFinding("/Game/Sets/Npc", L);
		EXPECT_TRUE(Contains(Line, "'" + L.EntryName + "'"));
		EXPECT_TRUE(Contains(Line, "[CPU]"));
	}
	const FPrefixSetFindingLine* Failed = nullptr;
	for (const FPrefixSetFindingLine& L : P.Lines)
	{
		Failed = L.Finding == EPrefixSetFinding::TokenizeFailed ? &L : Failed;
	}
	REQUIRE_TRUE(Failed != nullptr);
	EXPECT_TRUE(Contains(Failed->Detail, "no tokenizer"));

	// A tokenizer that returns nothing for non-empty text: EmptyTokens, by name, kept.
	const FPrefixSetTokenizePass Nothing = TokenizePrefixSetText({TextEntry("silent", "zzz")}, TextShape(), ModelHash(),
		[](const std::string&, std::vector<int32_t>& Out, std::string&) { Out.clear(); return true; });
	REQUIRE_EQ(Nothing.Failed.size(), static_cast<size_t>(1));
	EXPECT_TRUE(Nothing.Failed[0]);
	EXPECT_EQ(Count(Nothing.Lines, EPrefixSetFinding::EmptyTokens, "silent"), static_cast<size_t>(1));
}

HOST_TEST(prefix_set_text_failing_entries_keep_their_bytes, "2.17 (host twin: per-entry failure keeps previous bytes), 3.11's rule")
{
	// The fixture: every entry built once before with known bytes.
	std::vector<FPrefixSetEntry> Before;
	Before.push_back(TokensEntry("tokens", {5, 6, 7}));
	Before.push_back(BuiltTextEntry("greeting", "hello there"));
	Before.push_back(BuiltTextEntry("failing", "fine before"));
	const std::vector<FPrefixSetBuild> Previous = {BuildAll(Before)};
	REQUIRE_TRUE(!Previous[0].Entries[2].Wrapper.empty());

	// The rebuild: greeting's text changed (it builds anew), failing's text now fails to tokenize.
	std::vector<FPrefixSetEntry> Now = Before;
	Now[1].Text = "hello again";
	Now[2].Text = "!fail now";
	const FPrefixSetTokenizePass P = Pass(Now);
	const FPrefixSetBuild B = BuildAfterPass(P, FixtureIdentity(), Previous);
	REQUIRE_EQ(B.Entries.size(), static_cast<size_t>(3));
	EXPECT_TRUE(B.Entries[2].Wrapper == Previous[0].Entries[2].Wrapper);  // byte-equal after
	EXPECT_TRUE(P.Entries[2].Tokens == Before[2].Tokens);                 // and its ids
	EXPECT_TRUE(B.Entries[1].Wrapper != Previous[0].Entries[1].Wrapper);  // the others build and are stored
	EXPECT_TRUE(!B.Entries[1].Wrapper.empty());
	EXPECT_TRUE(B.Entries[0].Wrapper == Previous[0].Entries[0].Wrapper);  // same spec, same bytes (deterministic)
	EXPECT_TRUE(P.Entries[1].Tokens == Tok("hello again"));

	// A failing entry with no previous build keeps nothing: empty, never another entry's bytes.
	FPrefixSetTokenizePass Fresh = Pass({TextEntry("new_fail", "!fail")});
	const FPrefixSetBuild NoPrevious = BuildAfterPass(Fresh, FixtureIdentity(), {});
	REQUIRE_EQ(NoPrevious.Entries.size(), static_cast<size_t>(1));
	EXPECT_TRUE(NoPrevious.Entries[0].Wrapper.empty());

	// Another producer's previous build is not this producer's: a CPU rebuild keeps CPU bytes only.
	FPrefixEngineIdentity Gpu = FixtureIdentity();
	Gpu.Backend = 1;
	Gpu.AdapterVendorId = 0x10DE;
	Gpu.AdapterDeviceId = 0x2684;
	const std::vector<FPrefixSetBuild> GpuOnly = {BuildAll(Before, Gpu)};
	const FPrefixSetBuild CpuAfterGpu = BuildAfterPass(P, FixtureIdentity(), GpuOnly);
	EXPECT_TRUE(CpuAfterGpu.Entries[2].Wrapper.empty());
}

HOST_TEST(prefix_set_text_nested_entry_is_its_suffix_alone, "2.17 (host twin: the nested suffix)")
{
	const std::string Parent = "xxa";
	const std::string Suffix = "bzz";
	// The fixture's own premise, asserted: tokenize(parent ‖ suffix) ≠ tokenize(parent) ‖ tokenize(suffix).
	std::vector<int32_t> Joined = Tok(Parent);
	const std::vector<int32_t> SuffixIds = Tok(Suffix);
	Joined.insert(Joined.end(), SuffixIds.begin(), SuffixIds.end());
	REQUIRE_TRUE(Tok(Parent + Suffix) != Joined);

	std::vector<FPrefixSetEntry> E;
	E.push_back(TextEntry("parent", Parent));
	E.push_back(TextEntry("child", Suffix, "parent"));
	E.push_back(TokensEntry("tokens_child", {1, 2}, "parent")); // a parent and child are each Tokens or Text independently
	std::vector<std::string> Seen;
	const FPrefixSetTokenizePass P = TokenizePrefixSetText(E, TextShape(), ModelHash(),
		[&Seen](const std::string& Text, std::vector<int32_t>& Out, std::string& Error) { Seen.push_back(Text); return FakeTokenize(Text, Out, Error); });
	REQUIRE_EQ(Seen.size(), static_cast<size_t>(2));
	EXPECT_EQ(Seen[0], Parent);
	EXPECT_EQ(Seen[1], Suffix); // nothing prepended: the suffix alone
	EXPECT_TRUE(P.Entries[0].Tokens == Tok(Parent));
	EXPECT_TRUE(P.Entries[1].Tokens == SuffixIds);
	// "Tokenize the whole text, then split" would store the tail of tokenize(parent ‖ suffix):
	const std::vector<int32_t> Whole = Tok(Parent + Suffix);
	const std::vector<int32_t> Tail(Whole.begin() + static_cast<std::ptrdiff_t>(std::min(Tok(Parent).size(), Whole.size())), Whole.end());
	EXPECT_TRUE(P.Entries[1].Tokens != Tail);
	EXPECT_EQ(P.Failed[1], false);
	EXPECT_TRUE(P.Entries[2].Tokens == E[2].Tokens);
}

HOST_TEST(prefix_set_text_changed_warns_at_cook_and_ships_empty, "2.17 (host twin: cook TextChanged, text hash and model hash)")
{
	std::vector<FPrefixSetEntry> E;
	E.push_back(BuiltTextEntry("fresh_text", "good morning"));
	E.push_back(BuiltTextEntry("edited_text", "good evening"));
	E.push_back(TokensEntry("tokens", {11, 12, 13}));
	const std::vector<FPrefixSetBuild> Builds = {BuildAll(E)};
	// The stored text hash edited outside the editor's change hook (which would have cleared the ids).
	E[1].TextHash[0] ^= 0xFF;

	FPrefixEngineIdentity Current = FixtureIdentity();
	std::vector<FPrefixSetFindingLine> F = ValidatePrefixSetForCook(E, Builds, Current, TextShape());
	EXPECT_EQ(Count(F, EPrefixSetFinding::TextChanged, "edited_text"), static_cast<size_t>(1));
	EXPECT_EQ(F.size(), static_cast<size_t>(1)); // not also SpecEdited, NotBuilt or Orphaned
	for (const FPrefixSetFindingLine& L : F)
	{
		EXPECT_TRUE(Contains(FormatPrefixSetFinding("/Game/Sets/Npc", L), "entry 'edited_text'"));
	}

	// The cooked package: that entry with no ids and no bytes; everything else unchanged; the source untouched.
	const std::vector<FPrefixSetEntry> Source = E;
	const std::vector<size_t> Stale = PrefixSetStaleTextEntries(E, Current.ArtifactHash);
	REQUIRE_EQ(Stale.size(), static_cast<size_t>(1));
	EXPECT_EQ(Stale[0], static_cast<size_t>(1));
	std::vector<FPrefixSetEntry> CookedEntries = E;
	std::vector<FPrefixSetBuild> CookedBuilds = Builds;
	StripPrefixSetEntriesForCook(CookedEntries, CookedBuilds, Stale);
	EXPECT_TRUE(CookedEntries[1].Tokens.empty());
	EXPECT_TRUE(BuiltNamed(CookedBuilds[0], "edited_text")->Wrapper.empty());
	EXPECT_EQ(CookedEntries[1].Text, std::string("good evening"));
	EXPECT_TRUE(CookedEntries[0].Tokens == E[0].Tokens);
	EXPECT_TRUE(BuiltNamed(CookedBuilds[0], "fresh_text")->Wrapper == BuiltNamed(Builds[0], "fresh_text")->Wrapper);
	EXPECT_TRUE(BuiltNamed(CookedBuilds[0], "tokens")->Wrapper == BuiltNamed(Builds[0], "tokens")->Wrapper);
	EXPECT_TRUE(E[1].Tokens == Source[1].Tokens);
	EXPECT_TRUE(!BuiltNamed(Builds[0], "edited_text")->Wrapper.empty());

	// At runtime FindEntrySpec refuses it: uncooked (ids present, hash stale) and cooked (no ids).
	std::vector<int32_t> Ids;
	std::string Schema;
	std::string ParentEntry;
	EXPECT_TRUE(FindPrefixSetEntrySpec(E, ModelHash(), ModelHash(), "edited_text", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::TextChanged);
	EXPECT_TRUE(Ids.empty());
	EXPECT_TRUE(FindPrefixSetEntrySpec(CookedEntries, ModelHash(), ModelHash(), "edited_text", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::NoStoredIds);
	EXPECT_TRUE(FindPrefixSetEntrySpec(E, ModelHash(), ModelHash(), "fresh_text", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::Ok);
	EXPECT_TRUE(Ids == E[0].Tokens);
	EXPECT_TRUE(ParentEntry.empty());

	// A Text entry cleared by the editor's hook (no ids, no hashes) is TextChanged at cook too, never NotBuilt.
	std::vector<FPrefixSetEntry> Cleared = Source;
	Cleared[0].Tokens.clear();
	Cleared[0].TextHash = {};
	Cleared[0].TokenizedModelHash = {};
	F = ValidatePrefixSetForCook(Cleared, Builds, Current, TextShape());
	EXPECT_EQ(Count(F, EPrefixSetFinding::TextChanged, "fresh_text"), static_cast<size_t>(1));
	EXPECT_EQ(Count(F, EPrefixSetFinding::EmptyTokens, "fresh_text"), static_cast<size_t>(0));
	EXPECT_EQ(Count(F, EPrefixSetFinding::Orphaned, "fresh_text"), static_cast<size_t>(0));
}

HOST_TEST(prefix_set_text_model_hash_stale_takes_text_changed_not_stale_artifact, "2.17 (host twin: the model-hash leg and its mutant), 2.9")
{
	// One Text and one Tokens entry, built against model A; the set's model is now B.
	std::vector<FPrefixSetEntry> E;
	E.push_back(BuiltTextEntry("text", "good morning"));
	E.push_back(TokensEntry("tokens", {11, 12, 13}));
	const std::vector<FPrefixSetBuild> Builds = {BuildAll(E)};
	FPrefixEngineIdentity Current = FixtureIdentity();
	Current.ArtifactHash = OtherModelHash();

	const std::vector<FPrefixSetFindingLine> F = ValidatePrefixSetForCook(E, Builds, Current, TextShape());
	EXPECT_EQ(Count(F, EPrefixSetFinding::TextChanged, "text"), static_cast<size_t>(1));
	EXPECT_EQ(Count(F, EPrefixSetFinding::StaleArtifact, "text"), static_cast<size_t>(0)); // the mutant: 2.9's path for a Text entry
	EXPECT_EQ(Count(F, EPrefixSetFinding::StaleArtifact, "tokens"), static_cast<size_t>(1)); // the Tokens entry follows 2.9
	EXPECT_EQ(F.size(), static_cast<size_t>(2));

	const std::vector<size_t> Stale = PrefixSetStaleTextEntries(E, Current.ArtifactHash);
	REQUIRE_EQ(Stale.size(), static_cast<size_t>(1));
	EXPECT_EQ(Stale[0], static_cast<size_t>(0));

	// At runtime under model B (the set re-pointed at B, the subsystem configured with B): FindEntrySpec refuses the
	// Text entry, registration skips it, and the Tokens entry's stale bytes register and are refused by the cooked
	// check (a recompute from its ids, 2.9).
	std::vector<int32_t> Ids;
	std::string Schema;
	std::string ParentEntry;
	EXPECT_TRUE(FindPrefixSetEntrySpec(E, OtherModelHash(), OtherModelHash(), "text", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::TextChanged);
	EXPECT_TRUE(FindPrefixSetEntrySpec(E, OtherModelHash(), OtherModelHash(), "tokens", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::Ok);
	const std::vector<FPrefixSetRegistration> R = PrefixSetRegistrations(Builds[0], E, OtherModelHash());
	REQUIRE_EQ(R.size(), static_cast<size_t>(1));
	EXPECT_EQ(Builds[0].Entries[R[0].BuiltEntryIndex].Name, std::string("tokens"));
	// Under its own model, both register.
	EXPECT_EQ(PrefixSetRegistrations(Builds[0], E, ModelHash()).size(), static_cast<size_t>(2));
}

HOST_TEST(prefix_set_text_model_mismatch_refuses_both_kinds, "2.18 (host twin: FindEntrySpec and RegisterPrefixSet on a mixed set)")
{
	// A set built for model A with one Text and one Tokens entry; the subsystem configured with model B.
	std::vector<FPrefixSetEntry> E;
	E.push_back(BuiltTextEntry("text", "good morning"));
	E.push_back(TokensEntry("tokens", {11, 12, 13}));
	const std::vector<FPrefixSetBuild> Builds = {BuildAll(E)};
	std::vector<int32_t> Ids;
	std::string Schema;
	std::string ParentEntry;
	for (const char* Name : {"text", "tokens"})
	{
		// The mutant "the check on Text entries only" returns the Tokens entry here.
		EXPECT_TRUE(FindPrefixSetEntrySpec(E, ModelHash(), OtherModelHash(), Name, Ids, Schema, ParentEntry) == EPrefixSetSpecResult::PrefixSetModelMismatch);
		EXPECT_TRUE(Ids.empty());
		EXPECT_TRUE(FindPrefixSetEntrySpec(E, ModelHash(), ModelHash(), Name, Ids, Schema, ParentEntry) == EPrefixSetSpecResult::Ok);
		EXPECT_TRUE(!Ids.empty());
	}
	EXPECT_TRUE(FindPrefixSetEntrySpec(E, ModelHash(), ModelHash(), "nobody", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::UnknownEntry);

	// Registration under B registers nothing; under A both are served Cooked.
	FCacheFixture Under(8, false);
	REQUIRE_TRUE(Under.bConfigured);
	EXPECT_EQ(RegisterTextSet(Under, E, Builds, OtherModelHash()), static_cast<size_t>(0)); // a set whose model is B's view: refused
	FCacheFixture C(8, false);
	REQUIRE_TRUE(C.bConfigured);
	EXPECT_EQ(RegisterTextSet(C, E, Builds, ModelHash()), static_cast<size_t>(2));
	EXPECT_TRUE(SourceOf(C, E[0].Tokens) == EPrefixSource::Cooked);
	EXPECT_TRUE(SourceOf(C, E[1].Tokens) == EPrefixSource::Cooked);
	EXPECT_EQ(C.Sim->Violations.size(), static_cast<size_t>(0));
}

HOST_TEST(prefix_set_text_entry_is_keyed_by_its_stored_ids, "7.20 (host twin: equal ids, equal keys, a Cooked hit through the spec)")
{
	// A Text entry and a Tokens entry whose ids are the text's tokenization: one key.
	const std::string Text = "the gate is closed";
	std::vector<FPrefixSetEntry> E;
	E.push_back(TextEntry("text", Text));
	E.push_back(TokensEntry("tokens", Tok(Text)));
	const FPrefixSetTokenizePass P = Pass(E);
	REQUIRE_EQ(P.Calls, static_cast<size_t>(1));
	EXPECT_TRUE(P.Entries[0].Tokens == P.Entries[1].Tokens);
	std::vector<FPrefixKey> Keys;
	std::vector<FPrefixStoredSpec> Specs;
	std::vector<bool> Has;
	ComputePrefixSetKeys(P.Entries, FixtureIdentity(), Keys, Specs, Has);
	REQUIRE_EQ(Keys.size(), static_cast<size_t>(2));
	EXPECT_TRUE(Keys[0] == Keys[1]);
	const FPrefixSetBuild B = BuildAfterPass(P, FixtureIdentity(), {});
	FPrefixKey K0;
	FPrefixKey K1;
	REQUIRE_TRUE(ReadPrefixWrapperKey(B.Entries[0].Wrapper.data(), B.Entries[0].Wrapper.size(), K0));
	REQUIRE_TRUE(ReadPrefixWrapperKey(B.Entries[1].Wrapper.data(), B.Entries[1].Wrapper.size(), K1));
	EXPECT_TRUE(K0 == K1); // one cooked wrapper key (and so one disk file name)

	// The Text entry, served through FindEntrySpec's spec: a Cooked hit. No tokenize call after the build.
	FCacheFixture C(8, false);
	REQUIRE_TRUE(C.bConfigured);
	const std::vector<FPrefixSetBuild> Builds = {B};
	std::vector<FPrefixSetEntry> Built = P.Entries;
	EXPECT_EQ(RegisterTextSet(C, Built, Builds, ModelHash()), static_cast<size_t>(2));
	std::vector<int32_t> Ids;
	std::string Schema;
	std::string ParentEntry;
	REQUIRE_TRUE(FindPrefixSetEntrySpec(Built, ModelHash(), ModelHash(), "text", Ids, Schema, ParentEntry) == EPrefixSetSpecResult::Ok);
	EXPECT_TRUE(Ids == Tok(Text));
	EXPECT_TRUE(SourceOf(C, Ids) == EPrefixSource::Cooked);
}
