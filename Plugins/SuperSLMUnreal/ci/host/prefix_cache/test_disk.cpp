// The disk tier's file rules (precache plan §5.7) as pure functions over the in-memory file layer.

#include "harness.h"
#include "sim.h"

#include <set>

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	FPrefixKey KeyN(int N)
	{
		return FixtureKey(FixtureSpec(N));
	}

	FPrefixKey RawKey(uint8_t Fill)
	{
		FPrefixKey K;
		K.Bytes.fill(Fill);
		return K;
	}
}

HOST_TEST(disk_file_and_temporary_names, "§5.7 names, 3.8 (temporary names unique per instance and write)")
{
	const FPrefixKey K = RawKey(0xAB);
	const std::string Name = PrefixDiskFileName(K);
	EXPECT_EQ(Name, std::string("abababababababababababababababababababababababababababababababab.slup"));
	EXPECT_EQ(Name.size(), static_cast<size_t>(69));
	FPrefixKey Parsed;
	EXPECT_EQ(IsPrefixDiskFileName(Name, &Parsed), true);
	EXPECT_EQ(Parsed == K, true);
	EXPECT_EQ(IsPrefixDiskFileName(Name + ".tmp", nullptr), false);
	EXPECT_EQ(IsPrefixDiskFileName("notes.slup", nullptr), false);
	EXPECT_EQ(IsPrefixDiskFileName(PrefixKeyToHex(K) + ".SLUP", nullptr), false);

	std::set<std::string> Temps;
	for (uint64_t Instance : {1ull, 2ull})
	{
		for (uint64_t Serial : {1ull, 2ull, 3ull})
		{
			const std::string T = PrefixDiskTempFileName(K, Instance, Serial);
			EXPECT_EQ(T.empty(), false);
			EXPECT_EQ(IsPrefixDiskFileName(T, nullptr), false);
			EXPECT_NE(T, Name);
			Temps.insert(T);
		}
	}
	EXPECT_EQ(Temps.size(), static_cast<size_t>(6));
}

HOST_TEST(disk_trim_plan_oldest_first, "7.14 (pure half), §5.7 trim")
{
	std::vector<FPrefixDiskFileInfo> Files;
	auto Add = [&](const FPrefixKey& K, uint64_t Size, int64_t Time) { Files.push_back({PrefixDiskFileName(K), Size, Time}); };
	Add(KeyN(1), 100, 40); // newest
	Add(KeyN(2), 100, 10); // oldest
	Add(KeyN(3), 100, 30);
	Add(KeyN(4), 100, 20);
	Files.push_back({"notes.txt", 5000, 1});                                // not the cache's
	Files.push_back({PrefixDiskTempFileName(KeyN(5), 9, 1), 5000, 2});        // another instance's write in progress

	const std::vector<std::string> Plan = PlanPrefixDiskTrim(Files, 250);
	REQUIRE_EQ(Plan.size(), static_cast<size_t>(2));
	EXPECT_EQ(Plan[0], PrefixDiskFileName(KeyN(2)));
	EXPECT_EQ(Plan[1], PrefixDiskFileName(KeyN(4)));
	EXPECT_EQ(PlanPrefixDiskTrim(Files, 400).empty(), true);
	EXPECT_EQ(PlanPrefixDiskTrim(Files, 0).size(), static_cast<size_t>(4));

	// Ties by name, ascending.
	std::vector<FPrefixDiskFileInfo> Tied = {{PrefixDiskFileName(RawKey(0x22)), 10, 5}, {PrefixDiskFileName(RawKey(0x11)), 10, 5}};
	const std::vector<std::string> TiePlan = PlanPrefixDiskTrim(Tied, 10);
	REQUIRE_EQ(TiePlan.size(), static_cast<size_t>(1));
	EXPECT_EQ(TiePlan[0], PrefixDiskFileName(RawKey(0x11)));
}

HOST_TEST(disk_write_is_atomic_and_the_fault_seam_leaves_no_key_file, "5.8 (file half), §5.7 temporary then rename")
{
	FMemFs Fs;
	const FPrefixKey K = KeyN(1);
	const std::vector<uint8_t> First = {1, 2, 3, 4};
	REQUIRE_EQ(WritePrefixFileAtomically(Fs, "d", K, 7, 1, First, EPrefixWriteFault::None), true);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(K)), true);
	EXPECT_EQ(Fs.NamesIn("d").size(), static_cast<size_t>(1));
	std::vector<uint8_t> Read;
	EXPECT_EQ(ReadPrefixFile(Fs, "d", K, Read), EPrefixIoStatus::Ok);
	EXPECT_EQ(Read == First, true);

	// Fault on a key with no file: nothing under the key name, no temporary left.
	const FPrefixKey K2 = KeyN(2);
	EXPECT_EQ(WritePrefixFileAtomically(Fs, "d", K2, 7, 2, First, EPrefixWriteFault::BetweenWriteAndRename), false);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(K2)), false);
	EXPECT_EQ(Fs.NamesIn("d").size(), static_cast<size_t>(1));

	// Fault while replacing: the previous file is intact, never partial.
	const std::vector<uint8_t> Second = {9, 9, 9, 9, 9, 9};
	EXPECT_EQ(WritePrefixFileAtomically(Fs, "d", K, 7, 3, Second, EPrefixWriteFault::BetweenWriteAndRename), false);
	EXPECT_EQ(ReadPrefixFile(Fs, "d", K, Read), EPrefixIoStatus::Ok);
	EXPECT_EQ(Read == First, true);

	// A write failure is false with nothing under the key name.
	Fs.bFailWrites = true;
	EXPECT_EQ(WritePrefixFileAtomically(Fs, "d", K2, 7, 4, First, EPrefixWriteFault::None), false);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(K2)), false);
}

HOST_TEST(disk_write_sets_the_modification_time, "7.14 (the cache sets mtime itself on every write)")
{
	FMemFs Fs;
	const FPrefixKey K = KeyN(1);
	REQUIRE_EQ(WritePrefixFileAtomically(Fs, "d", K, 7, 1, {1}, EPrefixWriteFault::None), true);
	const int64_t After = Fs.Clock;
	// The final file's time is the newest event: a touch after the rename, not the temporary's write.
	EXPECT_EQ(Fs.Files["d/" + PrefixDiskFileName(K)].ModifiedTime, After);
	EXPECT_GE(After, static_cast<int64_t>(2));
}

HOST_TEST(disk_read_vanished_is_not_found, "§5.7 (a vanished or mid-rename file is a miss), §5.9 disk error row")
{
	FMemFs Fs;
	std::vector<uint8_t> Read = {7};
	EXPECT_EQ(ReadPrefixFile(Fs, "d", KeyN(1), Read), EPrefixIoStatus::NotFound);
	EXPECT_EQ(Read.empty(), true);
	REQUIRE_EQ(WritePrefixFileAtomically(Fs, "d", KeyN(1), 7, 1, {1, 2}, EPrefixWriteFault::None), true);
	Fs.bFailReads = true;
	EXPECT_EQ(ReadPrefixFile(Fs, "d", KeyN(1), Read), EPrefixIoStatus::Error);
}

HOST_TEST(disk_trim_directory_removes_oldest, "7.14 (file half)")
{
	FMemFs Fs;
	for (int I = 1; I <= 4; ++I)
	{
		REQUIRE_EQ(WritePrefixFileAtomically(Fs, "d", KeyN(I), 7, static_cast<uint64_t>(I), std::vector<uint8_t>(100, 1), EPrefixWriteFault::None), true);
	}
	Fs.TouchModifiedTime("d/" + PrefixDiskFileName(KeyN(1))); // a hit refreshes 1
	Fs.Files["d/notes.txt"] = {std::vector<uint8_t>(1000, 0), 0};
	EXPECT_EQ(TrimPrefixDiskDirectory(Fs, "d", 250), 2u);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(KeyN(1))), true);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(KeyN(2))), false);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(KeyN(3))), false);
	EXPECT_EQ(Fs.Exists("d/" + PrefixDiskFileName(KeyN(4))), true);
	EXPECT_EQ(Fs.Exists("d/notes.txt"), true);
}
