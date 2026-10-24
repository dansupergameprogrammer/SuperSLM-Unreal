// Cell 9.11's host half (precache plan rev 1.18.1 §5.11 (c), PC-13; rewritten for the one-row table, PC-15 Q8
// and audit G14 (a)): the 'SLUW' reader's Layer-1 pin check with its one compatibility entry. The predecessor
// is the paging release's engine, v1.13.1 at commit d697927 (plugin 1.2.0 vendors it); the 1.13.0 row is
// dropped. The restores themselves, their continuations and "nothing drawn" are the Unreal leg (U1 suite;
// the GPU dedicated and pooled legs are U2's box leg).
//
// The wrapper records a commit (S14: 'SLUW' carries the Layer-1 tag and commit), so the foreign-commit case
// applies and the entry matches tag and commit together.
//
// Red first against this commit's table (the 1.13.0 row): the 1.13.1 cases read Layer1Mismatch, the 1.13.0
// case reads CompatibilityEntry, and the row's tag and commit are 1.13.0's.

#include "harness.h"
#include "SuperSLMSaveCompatCore.h"

#include <cstring>
#include <vector>

using namespace SuperSLMCore;

namespace
{
	const char* const kCompiled = "v1.14.0";
	const char* const kPrev = "v1.13.1";
	// v1.13.1^{commit} in the engine repository (the tag object 034a76a peels to it).
	const char* const kPrevCommit = "d697927f6d5ce5fa73ecbb8660020a425518b923";
	// A commit other than d697927 under the 1.13.1 tag: the plan's example, 9e235965 (the commit the upstream
	// v1.13.0 tag peels to).
	const char* const kForeignCommit = "9e235965fa46baf4a63c52c4b7857405ca4456fe";
	// The dropped 1.13.0 row's commits: the 1.13.0 release commit and the patched candidate.
	const char* const k1p13p0Commits[] = {"a5f3ad1ba5cc5c62d7c97d7248e9389c6072a75a", "002094a6da8ba33450e6fabc08503b22e35d1610",
		"9e235965fa46baf4a63c52c4b7857405ca4456fe", "d697927f6d5ce5fa73ecbb8660020a425518b923"};
	const char* const kOtherCommit = "0123456789abcdef0123456789abcdef01234567";

	std::vector<uint8_t> Payload(const char* Magic)
	{
		std::vector<uint8_t> P(64, 0);
		std::memcpy(P.data(), Magic, 4);
		return P;
	}

	ESaveLayer1Check Check(const char* Tag, const char* Commit, const char* Magic)
	{
		const std::vector<uint8_t> P = Payload(Magic);
		return CheckSaveLayer1(kCompiled, Tag, Commit, P.data(), P.size());
	}
}

HOST_TEST(save_compat_1p13p1_saves_are_accepted_by_the_entry, "9.11 [U2] (rev 1.18.1): wrappers written by the paging release (v1.13.1, d697927) around SSB5, SSB6, SLM5 and SLM6 are accepted by the compatibility entry, nothing refused Layer1Mismatch (mutant: the entry removed)")
{
	for (const char* Magic : {"SSB5", "SSB6", "SLM5", "SLM6"})
	{
		EXPECT_EQ(Check(kPrev, kPrevCommit, Magic), ESaveLayer1Check::CompatibilityEntry);
	}
}

HOST_TEST(save_compat_same_pin_is_unchanged, "9.11 / S14: a wrapper of this build's own tag is the existing rule (SamePin)")
{
	EXPECT_EQ(Check(kCompiled, kOtherCommit, "SSB6"), ESaveLayer1Check::SamePin);
	EXPECT_EQ(Check(kCompiled, kPrevCommit, "SSB4"), ESaveLayer1Check::SamePin);
}

HOST_TEST(save_compat_1p11_saves_keep_their_boundary, "9.11: a wrapper written by a 1.0.x/1.1.x build (the 1.11.0 tag) is still Layer1Mismatch (mutant: the entry widened to any tag)")
{
	for (const char* Magic : {"SSB5", "SSB6", "SLM5", "SLM6"})
	{
		EXPECT_EQ(Check("v1.11.0", kPrevCommit, Magic), ESaveLayer1Check::Layer1Mismatch);
	}
	EXPECT_EQ(Check("v1.12.0", kPrevCommit, "SSB6"), ESaveLayer1Check::Layer1Mismatch);
}

HOST_TEST(save_compat_1p13p0_row_is_dropped, "9.11 [U2] (rev 1.18.1, PC-15 Q8): a wrapper tagged 1.13.0, with any commit, is Layer1Mismatch: the row is dropped (mutant: the entry widened to any tag)")
{
	for (const char* Commit : k1p13p0Commits)
	{
		for (const char* Magic : {"SSB5", "SSB6", "SLM5", "SLM6"})
		{
			EXPECT_EQ(Check("v1.13.0", Commit, Magic), ESaveLayer1Check::Layer1Mismatch);
		}
	}
	EXPECT_EQ(Check("v1.13.0", kOtherCommit, "SSB6"), ESaveLayer1Check::Layer1Mismatch);
}

HOST_TEST(save_compat_other_magics_keep_their_boundary, "9.11 (rev 1.14 audit N6): 1.13.1-tagged wrappers whose payload magic is outside the four, one each for SSB4, SSB3, SSB2 and SPX1, are Layer1Mismatch (mutant: the entry widened to that magic)")
{
	for (const char* Magic : {"SSB4", "SSB3", "SSB2", "SPX1"})
	{
		EXPECT_EQ(Check(kPrev, kPrevCommit, Magic), ESaveLayer1Check::Layer1Mismatch);
	}
	// A payload shorter than its magic is never accepted by the entry.
	const std::vector<uint8_t> P = Payload("SSB6");
	EXPECT_EQ(CheckSaveLayer1(kCompiled, kPrev, kPrevCommit, P.data(), 3), ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(CheckSaveLayer1(kCompiled, kPrev, kPrevCommit, nullptr, 0), ESaveLayer1Check::Layer1Mismatch);
}

HOST_TEST(save_compat_foreign_commit_keeps_its_boundary, "9.11 tag and commit (rev 1.14 audit S6; rev 1.18.1 audit G14 (a)): a wrapper carrying the 1.13.1 tag with a commit other than d697927 (9e235965) is Layer1Mismatch (mutant: the commit comparison dropped)")
{
	for (const char* Magic : {"SSB5", "SSB6", "SLM5", "SLM6"})
	{
		EXPECT_EQ(Check(kPrev, kForeignCommit, Magic), ESaveLayer1Check::Layer1Mismatch);
	}
	EXPECT_EQ(Check(kPrev, kOtherCommit, "SSB6"), ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(Check(kPrev, "", "SSB6"), ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(Check(kPrev, "d697927", "SSB6"), ESaveLayer1Check::Layer1Mismatch); // a short id is not the commit
}

HOST_TEST(save_compat_entry_is_the_one_1p13p1_row, "9.11 / §5.11 (c) (rev 1.18.1, the one-row table): the table has one row, for the 1.14.0 build, accepting v1.13.1 at d697927 alone, with the four sequence magics; it is inert on other builds")
{
	const std::vector<uint8_t> P = Payload("SSB6");
	EXPECT_EQ(CheckSaveLayer1(kPrev, kPrev, kOtherCommit, P.data(), P.size()), ESaveLayer1Check::SamePin);
	EXPECT_EQ(CheckSaveLayer1(kPrev, kCompiled, kOtherCommit, P.data(), P.size()), ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(CheckSaveLayer1("v1.15.0", kPrev, kPrevCommit, P.data(), P.size()), ESaveLayer1Check::Layer1Mismatch);
	size_t Rows = 0;
	const FSaveCompatRow* Table = SaveCompatTable(Rows);
	REQUIRE_EQ(Rows, static_cast<size_t>(1));
	REQUIRE_TRUE(Table != nullptr);
	EXPECT_EQ(std::strcmp(Table[0].CompiledTag, kCompiled), 0);
	EXPECT_EQ(std::strcmp(Table[0].AcceptedTag, kPrev), 0);
	REQUIRE_EQ(Table[0].AcceptedCommitCount, static_cast<size_t>(1));
	EXPECT_EQ(std::strcmp(Table[0].AcceptedCommits[0], kPrevCommit), 0);
	REQUIRE_EQ(Table[0].AcceptedMagicCount, static_cast<size_t>(4));
	for (size_t I = 0; I < 4; ++I)
	{
		const char* M = Table[0].AcceptedMagics[I];
		EXPECT_TRUE(std::strcmp(M, "SSB5") == 0 || std::strcmp(M, "SSB6") == 0 || std::strcmp(M, "SLM5") == 0 ||
			std::strcmp(M, "SLM6") == 0);
	}
}

HOST_TEST(save_compat_entry_removed_is_red, "9.11 mutant: the compatibility entry removed -> the 1.13.1 cases are Layer1Mismatch")
{
	const std::vector<uint8_t> P = Payload("SSB6");
	EXPECT_EQ(CheckSaveLayer1With(nullptr, 0, kCompiled, kPrev, kPrevCommit, P.data(), P.size()), ESaveLayer1Check::Layer1Mismatch);
	EXPECT_EQ(CheckSaveLayer1With(nullptr, 0, kCompiled, kCompiled, kPrevCommit, P.data(), P.size()), ESaveLayer1Check::SamePin);
}
