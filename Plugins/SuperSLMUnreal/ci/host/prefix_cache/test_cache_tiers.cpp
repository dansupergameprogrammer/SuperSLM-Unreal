// Tiers, the disk wrapper's misses, the spec compares, key mutation and the fallback rows
// (precache plan §5.2, §5.3, §5.7, §5.9).

#include "cache_fixture.h"

#include <climits>

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	std::vector<uint8_t> WrapperFor(const FPrefixEngineIdentity& Identity, const FPrefixKey& Key, const FPrefixSpec& Spec)
	{
		const std::vector<uint8_t> Payload = MakeFakePayload(Spec.SchemaName, Spec.Tokens);
		return EncodePrefixWrapper(Identity, Key, StoredOf(Spec), Payload.data(), Payload.size());
	}

	bool ServedByRecomputeWithItsOwnTokens(FCacheFixture& F, int64_t H, const FPrefixSpec& Spec)
	{
		if (F.Cache.GetLeaseState(H) != EPrefixLeaseState::Ready || F.Cache.GetSource(H) != EPrefixSource::Recomputed)
		{
			return false;
		}
		const FEngineHandle* Handle = F.Sim->HandleOf(H);
		return Handle != nullptr && Handle->Tokens == Spec.Tokens && Handle->Schema == Spec.SchemaName && !Handle->bFromRestore;
	}
}

HOST_TEST(cache_tier_order_memory_cooked_disk_recompute, "§5.3 lookup order and fall-through")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S1 = FixtureSpec(1);
	const FPrefixSpec S2 = FixtureSpec(2);
	const FPrefixSpec S3 = FixtureSpec(3);
	// S1 is cooked AND on disk: cooked wins.
	F.PutDiskFile(S1);
	F.Sim->Cooked[77] = WrapperFor(F.Config.Identity, FixtureKey(S1), S1);
	F.Cache.RegisterCooked(FixtureKey(S1), 77);
	F.PutDiskFile(S2); // S2 only on disk; S3 nowhere

	const int64_t H1 = F.RequestReady(S1);
	REQUIRE_NE(H1, 0);
	EXPECT_EQ(F.Cache.GetSource(H1), EPrefixSource::Cooked);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::CheckCooked), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ReadDisk), static_cast<size_t>(0));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Restore), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0)); // only a recompute writes back
	const FEngineHandle* Handle1 = F.Sim->HandleOf(H1);
	REQUIRE_TRUE(Handle1 != nullptr);
	EXPECT_EQ(Handle1->Tokens == S1.Tokens, true);

	const int64_t H1b = F.RequestReady(S1);
	EXPECT_EQ(F.Cache.GetSource(H1b), EPrefixSource::Memory);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::CheckCooked), static_cast<size_t>(1));

	const int64_t H2 = F.RequestReady(S2);
	REQUIRE_NE(H2, 0);
	EXPECT_EQ(F.Cache.GetSource(H2), EPrefixSource::Disk);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::CheckCooked), static_cast<size_t>(1)); // no cooked entry for S2
	bool bTouched = false; // a disk hit refreshes its file's mtime
	for (const FPrefixCommand& C : F.Sim->History)
	{
		bTouched = bTouched || (C.Kind == EPrefixCommandKind::TouchDiskFile && C.Key == FixtureKey(S2));
	}
	EXPECT_EQ(bTouched, true);

	const size_t Before = F.Sim->History.size();
	const int64_t H3 = F.RequestReady(S3);
	REQUIRE_NE(H3, 0);
	EXPECT_EQ(F.Cache.GetSource(H3), EPrefixSource::Recomputed);
	size_t ReadAt = SIZE_MAX;
	size_t BuildAt = SIZE_MAX;
	for (size_t I = Before; I < F.Sim->History.size(); ++I)
	{
		if (F.Sim->History[I].Kind == EPrefixCommandKind::ReadDisk && ReadAt == SIZE_MAX) { ReadAt = I; }
		if (F.Sim->History[I].Kind == EPrefixCommandKind::Build && BuildAt == SIZE_MAX) { BuildAt = I; }
	}
	EXPECT_LT(ReadAt, BuildAt); // the disk tier was tried (NotFound) before the recompute
	EXPECT_NE(BuildAt, SIZE_MAX);
	EXPECT_EQ(F.Diag().ServedBySource[static_cast<int>(EPrefixSource::Cooked)], 1u);
	EXPECT_EQ(F.Diag().ServedBySource[static_cast<int>(EPrefixSource::Disk)], 1u);
	EXPECT_EQ(F.Diag().ServedBySource[static_cast<int>(EPrefixSource::Recomputed)], 1u);
}

HOST_TEST(cache_cooked_miss_marks_the_entry_stale_and_falls_through, "§5.7 (cooked: entry marked stale), §5.3 fall-through")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec S = FixtureSpec(1);
	FPrefixEngineIdentity OldPin = F.Config.Identity;
	OldPin.Layer1Tag = "v1.13.0";
	F.Sim->Cooked[5] = WrapperFor(OldPin, FixtureKey(S), S);
	F.Cache.RegisterCooked(FixtureKey(S), 5);
	F.PutDiskFile(S);
	F.Cache.TakeLog();
	const int64_t H = F.RequestReady(S);
	REQUIRE_NE(H, 0);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Disk);
	EXPECT_EQ(F.Cache.IsCookedStale(5), true);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::WrapperLayer1TagMismatch), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::WrapperLayer1TagMismatch), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(0)); // cooked bytes are not a file
}

HOST_TEST(cache_disk_misses_each_reason, "2.8, 5.4 (wrapper rows), 7.9")
{
	struct FCase
	{
		const char* Name;
		EPrefixFallback Reason;
		int Kind; // 0 truncated, 1 version, 2 other tag, 3 forced collision, 4 zero-length, 5 foreign, 6 replaced
	};
	const FCase Cases[] = {
		{"truncated wrapper", EPrefixFallback::WrapperTruncated, 0},
		{"other wrapper version", EPrefixFallback::WrapperUnsupportedVersion, 1},
		{"other Layer-1 tag", EPrefixFallback::WrapperLayer1TagMismatch, 2},
		{"spec mismatch under an equal key", EPrefixFallback::WrapperSpecMismatch, 3},
		{"zero-length file", EPrefixFallback::WrapperTruncated, 4},
		{"foreign file", EPrefixFallback::WrapperBadMagic, 5},
		{"file replaced between scan and read", EPrefixFallback::WrapperKeyMismatch, 6},
	};
	for (const FCase& Case : Cases)
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec Spec = FixtureSpec(1);
		const FPrefixSpec Other = FixtureSpec(2);
		FPrefixKey Key = FixtureKey(Spec);
		switch (Case.Kind)
		{
		case 0: F.PutDiskFile(Spec); F.Sim->Fs->Files[F.Sim->DiskPath(Key)].Data.resize(100); break;
		case 1: F.PutDiskFile(Spec); F.Sim->Fs->Files[F.Sim->DiskPath(Key)].Data[4] = 2; break;
		case 2:
		{
			FPrefixEngineIdentity OldPin = F.Config.Identity;
			OldPin.Layer1Tag = "v1.13.0";
			F.Sim->Fs->Files[F.Sim->DiskPath(Key)] = {WrapperFor(OldPin, Key, Spec), 1};
			break;
		}
		case 3:
			// The seam: Spec's key is forced to Other's, and Other's file is on disk.
			F.PutDiskFile(Other);
			Key = FixtureKey(Other);
			F.Sim->ForcedKeys.push_back({StoredOf(Spec), Key});
			break;
		case 4: F.Sim->Fs->Files[F.Sim->DiskPath(Key)] = {{}, 1}; break;
		case 5: F.Sim->Fs->Files[F.Sim->DiskPath(Key)] = {{'n', 'o', 't', ' ', 'o', 'u', 'r', 's', '\n'}, 1}; break;
		case 6: F.Sim->Fs->Files[F.Sim->DiskPath(Key)] = {WrapperFor(F.Config.Identity, FixtureKey(Other), Other), 1}; break;
		default: break;
		}
		F.Cache.TakeLog();
		const int64_t H = F.RequestReady(Spec);
		const bool bRecomputed = ServedByRecomputeWithItsOwnTokens(F, H, Spec);
		const size_t Removes = F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile);
		const uint64_t Count = F.Fallbacks(Case.Reason);
		const size_t Lines = F.LogLinesWith(F.Cache.TakeLog(), Case.Reason);
		bool bRemovedThatKey = false;
		for (const FPrefixCommand& C : F.Sim->History)
		{
			bRemovedThatKey = bRemovedThatKey || (C.Kind == EPrefixCommandKind::RemoveDiskFile && C.Key == Key);
		}
		// The bad file is gone; whatever is under the key name now is a valid wrapper (the write-back).
		bool bLeftValid = true;
		const auto It = F.Sim->Fs->Files.find(F.Sim->DiskPath(Key));
		if (It != F.Sim->Fs->Files.end())
		{
			size_t Off = 0;
			size_t N = 0;
			bLeftValid = CheckPrefixWrapper(It->second.Data.data(), It->second.Data.size(), F.Config.Identity, Key,
				StoredOf(Spec), Off, N) == EPrefixWrapperCheck::Ok;
		}
		if (!bRecomputed || Removes != 1 || !bRemovedThatKey || Count != 1 || Lines != 1 || !bLeftValid)
		{
			HostTest::Fail(__FILE__, __LINE__, std::string(Case.Name) + ": recomputed=" + HostTest::ToText(bRecomputed) +
				" removes=" + HostTest::ToText(Removes) + " removedThatKey=" + HostTest::ToText(bRemovedThatKey) +
				" counter=" + HostTest::ToText(Count) + " logLines=" + HostTest::ToText(Lines) + " leftValid=" + HostTest::ToText(bLeftValid));
		}
	}
}

HOST_TEST(cache_disk_spec_compare_is_load_bearing, "11.4 (disk tier)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec A = FixtureSpec(1);
	const FPrefixSpec B = FixtureSpec(2);
	F.PutDiskFile(A);
	F.Sim->ForcedKeys.push_back({StoredOf(B), FixtureKey(A)});
	const int64_t H = F.RequestReady(B);
	REQUIRE_NE(H, 0);
	// Without the compare this is a Disk hit that restores A's rows for B.
	EXPECT_NE(F.Cache.GetSource(H), EPrefixSource::Disk);
	EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H, B), true);
	EXPECT_EQ(F.Diag().SpecMismatches, 1u);
}

HOST_TEST(cache_memory_spec_compare_is_load_bearing, "11.4 (memory tier, rev 1.1)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec A = FixtureSpec(1, 32, "quest");
	const FPrefixSpec B = FixtureSpec(2, 32, "quest");
	F.Sim->ForcedKeys.push_back({StoredOf(B), FixtureKey(A)});
	const int64_t HA = F.RequestReady(A);
	REQUIRE_NE(HA, 0);
	F.Cache.TakeLog();
	const int64_t HB = F.RequestReady(B);
	REQUIRE_NE(HB, 0);
	// Without the compare this is a Memory hit on A's entry.
	EXPECT_NE(F.Cache.GetSource(HB), EPrefixSource::Memory);
	EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, HB, B), true);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::MemorySpecMismatch), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::MemorySpecMismatch), static_cast<size_t>(1));
	// A's entry is untouched and still serves A.
	const int64_t HA2 = F.RequestReady(A);
	EXPECT_EQ(F.Cache.GetSource(HA2), EPrefixSource::Memory);
	const FEngineHandle* Handle = F.Sim->HandleOf(HA2);
	REQUIRE_TRUE(Handle != nullptr);
	EXPECT_EQ(Handle->Tokens == A.Tokens, true);
}

HOST_TEST(cache_key_mutation_through_the_cache, "2.12 (tokens, schema name, the length prefix)")
{
	struct FPair
	{
		const char* Component;
		FPrefixSpec A;
		FPrefixSpec B;
	};
	std::vector<FPair> Pairs;
	{
		FPair P{"schema name", FixtureSpec(1, 32, "quest"), FixtureSpec(1, 32, "barter")};
		Pairs.push_back(P);
	}
	{
		FPair P{"tokens", FixtureSpec(1), FixtureSpec(1)};
		P.B.Tokens[31] += 1;
		Pairs.push_back(P);
	}
	{
		// §5.2 puts the schema name before the tokens. With the length prefixes dropped,
		// ("", [0x64636261, 9]) and ("abcd", [9]) encode the same bytes.
		FPair P{"length prefix", FPrefixSpec(), FPrefixSpec()};
		P.A.Tokens = {0x64636261, 9};
		P.B.SchemaName = "abcd";
		P.B.Tokens = {9};
		Pairs.push_back(P);
	}
	for (const FPair& Pair : Pairs)
	{
		int64_t Counter = 1;
		FSuperSLMPrefixCacheCore Cache(&Counter);
		FPrefixCacheConfig Config = FixtureConfig(4, false);
		Config.VocabSize = INT32_MAX; // so the printable length-prefix pair is in vocabulary
		REQUIRE_TRUE(Cache.Configure(Config));
		FSim Sim(Cache, Config, 64);
		int64_t HA = 0;
		int64_t HB = 0;
		std::string Error;
		REQUIRE_EQ(Cache.Request(Pair.A, HA, Error), EPrefixRequestResult::Accepted);
		Sim.RunAll();
		REQUIRE_EQ(Cache.GetLeaseState(HA), EPrefixLeaseState::Ready);
		REQUIRE_EQ(Cache.Request(Pair.B, HB, Error), EPrefixRequestResult::Accepted);
		Sim.RunAll();
		const FEngineHandle* Handle = Sim.HandleOf(HB);
		const bool bOk = Cache.GetLeaseState(HB) == EPrefixLeaseState::Ready && Cache.GetSource(HB) != EPrefixSource::Memory &&
			Handle != nullptr && Handle->Tokens == Pair.B.Tokens && Handle->Schema == Pair.B.SchemaName &&
			Cache.GetDiagnostics().SpecMismatches == 0;
		if (!bOk)
		{
			HostTest::Fail(__FILE__, __LINE__, std::string("component ") + Pair.Component + ": B was served from A's entry or caught only by the compare; spec mismatches = " +
				HostTest::ToText(Cache.GetDiagnostics().SpecMismatches));
		}
	}
}

HOST_TEST(cache_unknown_schema, "2.11, 5.4 (UnknownSchema row)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(1, 32, "missing_schema");
	F.Cache.TakeLog();
	const int64_t H = F.Request(Spec);
	REQUIRE_NE(H, 0); // found on a worker lane, not refused synchronously
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::UnknownSchema);
	EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	EXPECT_EQ(F.Diag().ChargedBytes, 0u);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Save), static_cast<size_t>(0));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0));
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(Spec))), false);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::UnknownSchema), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::UnknownSchema), static_cast<size_t>(1));
}

HOST_TEST(cache_recompute_is_saved_and_written_back, "§5.3 item 4 (disk on: save and write back), §5.7")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec A = FixtureSpec(1, 32, "quest");
	const FPrefixSpec B = FixtureSpec(2);
	EXPECT_NE(F.RequestReady(A), 0);
	EXPECT_NE(F.RequestReady(B), 0);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Save), static_cast<size_t>(2));
	EXPECT_EQ(F.Diag().DiskWritesIssued, 2u);
	uint64_t Serials[2] = {0, 0};
	size_t N = 0;
	for (const FPrefixCommand& C : F.Sim->History)
	{
		if (C.Kind == EPrefixCommandKind::WriteDisk && N < 2) { Serials[N++] = C.WriteSerial; }
	}
	EXPECT_EQ(N, static_cast<size_t>(2));
	EXPECT_NE(Serials[0], Serials[1]);
	EXPECT_GE(F.Sim->CountHistory(EPrefixCommandKind::TrimDisk), static_cast<size_t>(3)); // at Configure and after each write
	for (const FPrefixSpec* S : {&A, &B})
	{
		const auto It = F.Sim->Fs->Files.find(F.Sim->DiskPath(FixtureKey(*S)));
		REQUIRE_TRUE(It != F.Sim->Fs->Files.end());
		size_t Off = 0;
		size_t Size = 0;
		EXPECT_EQ(CheckPrefixWrapper(It->second.Data.data(), It->second.Data.size(), F.Config.Identity, FixtureKey(*S), StoredOf(*S), Off, Size),
			EPrefixWrapperCheck::Ok);
	}
	for (const std::string& Name : F.Sim->Fs->NamesIn("cache"))
	{
		EXPECT_EQ(IsPrefixDiskFileName(Name, nullptr), true); // no temporary left behind
	}
}

HOST_TEST(cache_fallback_payload_refused_by_the_engine, "5.4 (payload corrupted row; another producer row), 7.9")
{
	struct FCase { const char* Name; bool bCorruptByte; int32_t Status; };
	const FCase Cases[] = {{"a payload byte corrupted (digest)", true, 5}, {"another producer", false, 30}};
	for (const FCase& Case : Cases)
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec Spec = FixtureSpec(1);
		F.PutDiskFile(Spec);
		const std::string Path = F.Sim->DiskPath(FixtureKey(Spec));
		REQUIRE_TRUE(F.Sim->Fs->Exists(Path));
		if (Case.bCorruptByte)
		{
			F.Sim->Fs->Files[Path].Data[F.Sim->Fs->Files[Path].Data.size() - 10] ^= 0x01; // inside the payload
		}
		else
		{
			F.Sim->bRefuseRestores = true;
		}
		F.Sim->RefuseStatus = Case.Status;
		F.Cache.TakeLog();
		const int64_t H = F.RequestReady(Spec);
		const std::vector<FPrefixLogLine> Log = F.Cache.TakeLog();
		bool bStatusLogged = false;
		for (const FPrefixLogLine& Line : Log)
		{
			bStatusLogged = bStatusLogged || (Line.Reason == EPrefixFallback::PayloadRefused && Contains(Line.Message, std::to_string(Case.Status)));
		}
		const bool bOk = ServedByRecomputeWithItsOwnTokens(F, H, Spec) && F.Fallbacks(EPrefixFallback::PayloadRefused) == 1 &&
			F.LogLinesWith(Log, EPrefixFallback::PayloadRefused) == 1 && bStatusLogged &&
			F.Sim->CountHistory(EPrefixCommandKind::Restore) == 1 && F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile) == 1 &&
			F.Diag().ChargedBytes == 2 * F.Config.PageBytes;
		if (!bOk)
		{
			HostTest::Fail(__FILE__, __LINE__, std::string(Case.Name) + ": not a logged miss followed by a recompute");
		}
	}
}

HOST_TEST(cache_fallback_disk_read_and_write_errors, "5.4 (disk read or write error row)")
{
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec Spec = FixtureSpec(1);
		F.PutDiskFile(Spec);
		F.Sim->Fs->bFailReads = true;
		F.Cache.TakeLog();
		const int64_t H = F.RequestReady(Spec);
		EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H, Spec), true);
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::DiskReadError), 1u);
		EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::DiskReadError), static_cast<size_t>(1));
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(0)); // an IO error deletes nothing
	}
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec Spec = FixtureSpec(1);
		F.Sim->Fs->bFailWrites = true;
		F.Cache.TakeLog();
		const int64_t H = F.RequestReady(Spec);
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready); // no write-back, the lease is unaffected
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::DiskWriteError), 1u);
		EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::DiskWriteError), static_cast<size_t>(1));
		EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(Spec))), false);
	}
}

HOST_TEST(cache_a_different_model_or_pin_is_a_miss, "5.4 (the key row: model, engine pin), §5.2")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(1);
	REQUIRE_NE(F.RequestReady(Spec), 0);
	REQUIRE_TRUE(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(Spec))));
	FPrefixCacheConfig OtherModel = F.Config;
	OtherModel.Identity.ArtifactHash[0] ^= 0xFF;
	REQUIRE_TRUE(F.Reconfigure(OtherModel));
	const int64_t H = F.RequestReady(Spec);
	EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H, Spec), true);
	FPrefixCacheConfig OtherPin = F.Config;
	OtherPin.Identity.Layer1Tag = "v1.15.0";
	REQUIRE_TRUE(F.Reconfigure(OtherPin));
	const int64_t H2 = F.RequestReady(Spec);
	EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H2, Spec), true);
	uint64_t Total = 0;
	for (uint32_t I = 0; I < kPrefixFallbackCount; ++I) { Total += F.Diag().Fallbacks[I]; }
	EXPECT_EQ(Total, 0u); // a different key is a plain miss: no fallback, no wrong hit
}

HOST_TEST(cache_disk_trim_oldest_first_and_a_hit_refreshes, "7.14")
{
	// One wrapper for an L = 32, unbound spec is 132 + 4 + 4 + 128 + 1 + 8 + 144 (fake payload) bytes.
	const FPrefixSpec Probe = FixtureSpec(0);
	const uint64_t FileBytes = WrapperFor(FixtureIdentity(), FixtureKey(Probe), Probe).size();
	REQUIRE_GT(FileBytes, static_cast<uint64_t>(0));
	FPrefixCacheConfig Config = FixtureConfig(8, true);
	Config.DiskMaxBytes = 3 * FileBytes;
	FCacheFixture F(8, true);
	REQUIRE_TRUE(F.Reconfigure(Config));
	const FPrefixSpec A = FixtureSpec(1), B = FixtureSpec(2), C = FixtureSpec(3), D = FixtureSpec(4);
	for (const FPrefixSpec* S : {&A, &B, &C})
	{
		REQUIRE_NE(F.RequestReady(*S), 0);
	}
	// A Disk hit on A, from a fresh lifetime (an empty memory tier).
	REQUIRE_TRUE(F.Reconfigure(Config));
	const int64_t HA = F.RequestReady(A);
	EXPECT_EQ(F.Cache.GetSource(HA), EPrefixSource::Disk);
	// Write D: four files exceed the limit; the oldest by the cache's own mtime is B.
	REQUIRE_NE(F.RequestReady(D), 0);
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(A))), true);
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(B))), false);
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(C))), true);
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(D))), true);
	// And after a re-Configure the directory is at or under the limit.
	Config.DiskMaxBytes = 2 * FileBytes;
	REQUIRE_TRUE(F.Reconfigure(Config));
	F.Sim->RunAll();
	uint64_t Total = 0;
	for (const auto& Pair : F.Sim->Fs->Files) { Total += Pair.second.Data.size(); }
	EXPECT_LE(Total, Config.DiskMaxBytes);
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(C))), false); // C is now the oldest
}

HOST_TEST(cache_write_fault_then_the_next_request_recomputes, "5.8")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(1);
	F.Sim->WriteFault = EPrefixWriteFault::BetweenWriteAndRename;
	const int64_t H = F.RequestReady(Spec);
	REQUIRE_NE(H, 0);
	REQUIRE_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->Fs->Exists(F.Sim->DiskPath(FixtureKey(Spec))), false);
	EXPECT_EQ(F.Sim->Fs->NamesIn("cache").empty(), true); // and the temporary was removed
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::DiskWriteError), 1u);
	F.Sim->WriteFault = EPrefixWriteFault::None;
	REQUIRE_TRUE(F.Reconfigure(F.Config));
	const int64_t H2 = F.RequestReady(Spec);
	EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H2, Spec), true);
}

HOST_TEST(cache_memory_collision_falls_through_the_tiers, "§5.3 (a tier that fails for any reason falls through to the next), 11.4 (memory tier)")
{
	const FPrefixSpec A = FixtureSpec(1, 32, "quest");
	const FPrefixSpec B = FixtureSpec(2, 32, "quest");
	const FPrefixKey K = FixtureKey(A); // B is forced onto A's key
	for (int Case = 0; Case < 3; ++Case)
	{
		// 0: B's own bytes are cooked under K. 1: B's own file is on disk under K. 2: K's file is A's.
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		F.Sim->ForcedKeys.push_back({StoredOf(B), K});
		REQUIRE_NE(F.RequestReady(A), 0); // A is resident, and its file is written under K
		const std::string Path = F.Sim->DiskPath(K);
		REQUIRE_TRUE(F.Sim->Fs->Exists(Path));
		if (Case == 0)
		{
			F.Sim->Cooked[9] = WrapperFor(F.Config.Identity, K, B);
			F.Cache.RegisterCooked(K, 9);
		}
		else if (Case == 1)
		{
			F.Sim->Fs->Files[Path].Data = WrapperFor(F.Config.Identity, K, B);
		}
		const std::vector<uint8_t> FileBefore = F.Sim->Fs->Files[Path].Data;
		const size_t WritesBefore = F.Sim->CountHistory(EPrefixCommandKind::WriteDisk);
		const int64_t H = F.RequestReady(B);
		REQUIRE_NE(H, 0);
		const EPrefixSource Expected = Case == 0 ? EPrefixSource::Cooked : Case == 1 ? EPrefixSource::Disk : EPrefixSource::Recomputed;
		EXPECT_EQ(F.Cache.GetSource(H), Expected);
		const FEngineHandle* Handle = F.Sim->HandleOf(H);
		REQUIRE_TRUE(Handle != nullptr);
		EXPECT_EQ(Handle->Tokens == B.Tokens, true);
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::MemorySpecMismatch), 1u);
		// The colliding entry never writes under K, and leaves K's file and A's entry alone.
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), WritesBefore);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(0));
		EXPECT_EQ(F.Sim->Fs->Exists(Path) && F.Sim->Fs->Files[Path].Data == FileBefore, true);
		EXPECT_EQ(F.Cache.IsCookedStale(9), false);
		const int64_t HA = F.RequestReady(A);
		EXPECT_EQ(F.Cache.GetSource(HA), EPrefixSource::Memory);
	}
}

HOST_TEST(cache_build_failure_is_counted_and_logged, "§5.9 (every row ends in a recompute or a named refusal, logged)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->bFailBuilds = true;
	F.Cache.TakeLog();
	const int64_t H = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Failed);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::BuildFailed), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::BuildFailed), static_cast<size_t>(1));
}

HOST_TEST(cache_restore_failure_keeps_the_bytes, "§5.9 (only the engine's refusal of the payload deletes the file or marks the entry stale)")
{
	for (int Cooked = 0; Cooked < 2; ++Cooked)
	{
		FCacheFixture F(4, true);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec Spec = FixtureSpec(1);
		if (Cooked == 1)
		{
			F.Sim->Cooked[3] = WrapperFor(F.Config.Identity, FixtureKey(Spec), Spec);
			F.Cache.RegisterCooked(FixtureKey(Spec), 3);
		}
		else
		{
			F.PutDiskFile(Spec);
		}
		F.Sim->bFailRestores = true;
		F.Sim->RefuseStatus = 12;
		F.Cache.TakeLog();
		const int64_t H = F.RequestReady(Spec);
		const std::vector<FPrefixLogLine> Log = F.Cache.TakeLog();
		EXPECT_EQ(ServedByRecomputeWithItsOwnTokens(F, H, Spec), true); // a miss, then the next tiers
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::RestoreFailed), 1u);
		EXPECT_EQ(F.LogLinesWith(Log, EPrefixFallback::RestoreFailed), static_cast<size_t>(1));
		EXPECT_EQ(F.Fallbacks(EPrefixFallback::PayloadRefused), 0u);
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(0));
		EXPECT_EQ(F.Cache.IsCookedStale(3), false);
	}
}

HOST_TEST(cache_save_failure_is_counted_and_logged, "§5.9 (disk write error row: no write-back, logged)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->bFailSaves = true;
	F.Cache.TakeLog();
	const int64_t H = F.RequestReady(FixtureSpec(1));
	EXPECT_NE(H, 0); // the lease is unaffected
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(0));
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::SaveFailed), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::SaveFailed), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}
