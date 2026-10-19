// The 'SLUP' disk and cooked wrapper (precache plan §5.7): encode, parse, and every load check.

#include "harness.h"
#include "sim.h"

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	struct FWrapped
	{
		FPrefixEngineIdentity Identity;
		FPrefixKey Key;
		FPrefixStoredSpec Spec;
		std::vector<uint8_t> Payload;
		std::vector<uint8_t> Bytes;
	};

	FWrapped MakeWrapped(const std::string& Schema = "quest")
	{
		FWrapped W;
		W.Identity = FixtureIdentity();
		W.Spec = StoredOf(FixtureSpec(3, 40, Schema));
		W.Key = ComputePrefixKey(KeyInputsFor(W.Identity, W.Spec));
		W.Payload = MakeFakePayload(Schema, W.Spec.Tokens);
		W.Bytes = EncodePrefixWrapper(W.Identity, W.Key, W.Spec, W.Payload.data(), W.Payload.size());
		return W;
	}

	EPrefixWrapperCheck Check(const std::vector<uint8_t>& Bytes, const FPrefixEngineIdentity& Id, const FPrefixKey& Key, const FPrefixStoredSpec& Spec)
	{
		size_t Offset = 0;
		size_t Size = 0;
		return CheckPrefixWrapper(Bytes.data(), Bytes.size(), Id, Key, Spec, Offset, Size);
	}

	uint32_t U32At(const std::vector<uint8_t>& B, size_t At)
	{
		return static_cast<uint32_t>(B[At]) | (static_cast<uint32_t>(B[At + 1]) << 8) | (static_cast<uint32_t>(B[At + 2]) << 16) |
			(static_cast<uint32_t>(B[At + 3]) << 24);
	}
	void SetU32At(std::vector<uint8_t>& B, size_t At, uint32_t V)
	{
		for (size_t I = 0; I < 4; ++I) { B[At + I] = static_cast<uint8_t>(V >> (8 * I)); }
	}
}

HOST_TEST(wrapper_round_trip_and_layout, "§5.7 wrapper layout")
{
	const FWrapped W = MakeWrapped();
	const size_t Expected = kPrefixWrapperFixedBytes + 4 + W.Spec.SchemaName.size() + 4 + 4 * W.Spec.Tokens.size() + 1 + 8 + W.Payload.size();
	REQUIRE_EQ(W.Bytes.size(), Expected);
	EXPECT_EQ(W.Bytes[0], 'S');
	EXPECT_EQ(W.Bytes[1], 'L');
	EXPECT_EQ(W.Bytes[2], 'U');
	EXPECT_EQ(W.Bytes[3], 'P');
	EXPECT_EQ(U32At(W.Bytes, 4), kPrefixWrapperVersion);
	EXPECT_EQ(W.Bytes[8], W.Identity.Backend);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(&W.Bytes[12]), W.Identity.Layer1Tag.size()), W.Identity.Layer1Tag);
	EXPECT_EQ(W.Bytes[12 + W.Identity.Layer1Tag.size()], 0);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(&W.Bytes[28]), 40), W.Identity.Layer1Commit);
	EXPECT_EQ(std::equal(W.Identity.ArtifactHash.begin(), W.Identity.ArtifactHash.end(), W.Bytes.begin() + 68), true);
	EXPECT_EQ(std::equal(W.Key.Bytes.begin(), W.Key.Bytes.end(), W.Bytes.begin() + 100), true);
	EXPECT_EQ(U32At(W.Bytes, 132), static_cast<uint32_t>(W.Spec.SchemaName.size()));

	size_t Offset = 0;
	size_t Size = 0;
	EXPECT_EQ(CheckPrefixWrapper(W.Bytes.data(), W.Bytes.size(), W.Identity, W.Key, W.Spec, Offset, Size), EPrefixWrapperCheck::Ok);
	EXPECT_EQ(Size, W.Payload.size());
	EXPECT_EQ(Offset + Size, W.Bytes.size());
	EXPECT_EQ(Offset + Size <= W.Bytes.size() && std::equal(W.Payload.begin(), W.Payload.end(), W.Bytes.begin() + static_cast<long>(Offset)), true);

	FPrefixKey Read;
	EXPECT_EQ(ReadPrefixWrapperKey(W.Bytes.data(), W.Bytes.size(), Read), true);
	EXPECT_EQ(Read == W.Key, true);
}

HOST_TEST(wrapper_each_load_check_names_its_reason, "2.8 (wrapper half), §5.7 load checks, §5.9 row 2")
{
	const FWrapped W = MakeWrapped();
	REQUIRE_EQ(Check(W.Bytes, W.Identity, W.Key, W.Spec), EPrefixWrapperCheck::Ok);

	struct FCase
	{
		const char* Name;
		std::vector<uint8_t> Bytes;
		FPrefixEngineIdentity Identity;
		FPrefixKey Key;
		FPrefixStoredSpec Spec;
		EPrefixWrapperCheck Expected;
	};
	std::vector<FCase> Cases;
	auto Base = [&](const char* Name, EPrefixWrapperCheck Expected) -> FCase& {
		Cases.push_back({Name, W.Bytes, W.Identity, W.Key, W.Spec, Expected});
		return Cases.back();
	};
	Base("zero-length file", EPrefixWrapperCheck::Truncated).Bytes.clear();
	Base("foreign file", EPrefixWrapperCheck::BadMagic).Bytes = std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '\n'};
	Base("an SPX1 blob, unwrapped", EPrefixWrapperCheck::BadMagic).Bytes = W.Payload;
	Base("magic", EPrefixWrapperCheck::BadMagic).Bytes[3] = 'W';
	SetU32At(Base("version 3", EPrefixWrapperCheck::UnsupportedVersion).Bytes, 4, 3);
	Base("backend in file", EPrefixWrapperCheck::BackendMismatch).Bytes[8] = 1;
	Base("backend expected", EPrefixWrapperCheck::BackendMismatch).Identity.Backend = 1;
	Base("Layer-1 tag", EPrefixWrapperCheck::Layer1TagMismatch).Identity.Layer1Tag = "v1.15.0";
	Base("Layer-1 commit", EPrefixWrapperCheck::Layer1CommitMismatch).Identity.Layer1Commit[0] = 'f';
	Base("artifact hash", EPrefixWrapperCheck::ArtifactMismatch).Identity.ArtifactHash[5] ^= 0x40;
	Base("key", EPrefixWrapperCheck::KeyMismatch).Key.Bytes[0] ^= 1;
	Base("spec: schema name", EPrefixWrapperCheck::SpecMismatch).Spec.SchemaName = "quesT";
	Base("spec: schema unbound", EPrefixWrapperCheck::SpecMismatch).Spec.SchemaName.clear();
	Base("spec: last token", EPrefixWrapperCheck::SpecMismatch).Spec.Tokens.back() += 1;
	Base("spec: token count", EPrefixWrapperCheck::SpecMismatch).Spec.Tokens.pop_back();
	Base("spec: parent", EPrefixWrapperCheck::SpecMismatch).Spec.bHasParent = true;
	{
		FCase& Trailing = Base("a byte after the payload", EPrefixWrapperCheck::Malformed);
		Trailing.Bytes.push_back(0);
	}
	{
		// The parent flag sits after the schema name and the tokens.
		FCase& Flag = Base("parent flag 2", EPrefixWrapperCheck::Malformed);
		const size_t At = kPrefixWrapperFixedBytes + 4 + W.Spec.SchemaName.size() + 4 + 4 * W.Spec.Tokens.size();
		if (At < Flag.Bytes.size()) { Flag.Bytes[At] = 2; }
	}
	{
		FCase& Short = Base("payload one byte short", EPrefixWrapperCheck::Truncated);
		if (!Short.Bytes.empty()) { Short.Bytes.pop_back(); }
	}
	{
		FCase& Huge = Base("token count past the end", EPrefixWrapperCheck::Truncated);
		const size_t At = kPrefixWrapperFixedBytes + 4 + W.Spec.SchemaName.size();
		if (At + 4 <= Huge.Bytes.size()) { SetU32At(Huge.Bytes, At, 0x7FFFFFFF); }
	}

	for (const FCase& C : Cases)
	{
		const EPrefixWrapperCheck Got = Check(C.Bytes, C.Identity, C.Key, C.Spec);
		if (Got != C.Expected)
		{
			HostTest::Fail(__FILE__, __LINE__, std::string(C.Name) + ": got " + HostTest::ToText(Got) + ", expected " + HostTest::ToText(C.Expected));
		}
	}
	EXPECT_EQ(std::string(PrefixWrapperCheckName(EPrefixWrapperCheck::SpecMismatch)).empty(), false);
	EXPECT_NE(std::string(PrefixWrapperCheckName(EPrefixWrapperCheck::SpecMismatch)), std::string(PrefixWrapperCheckName(EPrefixWrapperCheck::KeyMismatch)));
}

HOST_TEST(wrapper_versions_0_and_2_and_other_pins_are_misses, "9.6")
{
	const FWrapped W = MakeWrapped();
	REQUIRE_EQ(Check(W.Bytes, W.Identity, W.Key, W.Spec), EPrefixWrapperCheck::Ok);
	for (uint32_t Version : {0u, 2u})
	{
		std::vector<uint8_t> B = W.Bytes;
		SetU32At(B, 4, Version);
		EXPECT_EQ(Check(B, W.Identity, W.Key, W.Spec), EPrefixWrapperCheck::UnsupportedVersion);
	}
	// A blob built under another Layer-1 pin: the file says v1.13.0, this build is the fixture's pin.
	FPrefixEngineIdentity OtherPin = W.Identity;
	OtherPin.Layer1Tag = "v1.13.0";
	OtherPin.Layer1Commit = "fedcba9876543210fedcba9876543210fedcba98";
	const std::vector<uint8_t> Old = EncodePrefixWrapper(OtherPin, W.Key, W.Spec, W.Payload.data(), W.Payload.size());
	EXPECT_EQ(Check(Old, W.Identity, W.Key, W.Spec), EPrefixWrapperCheck::Layer1TagMismatch);
	FPrefixEngineIdentity SameTagOtherCommit = W.Identity;
	SameTagOtherCommit.Layer1Commit = OtherPin.Layer1Commit;
	const std::vector<uint8_t> Rebuilt = EncodePrefixWrapper(SameTagOtherCommit, W.Key, W.Spec, W.Payload.data(), W.Payload.size());
	EXPECT_EQ(Check(Rebuilt, W.Identity, W.Key, W.Spec), EPrefixWrapperCheck::Layer1CommitMismatch);
}

HOST_TEST(wrapper_spec_compare_is_load_bearing, "11.4 (wrapper half), §5.2 collisions are not trusted")
{
	// A forced collision: the file stores spec A under key K; the request is spec B with the same K.
	// If the spec compare were removed this would read Ok and restore A's payload for B.
	const FWrapped A = MakeWrapped("quest");
	FPrefixStoredSpec B = A.Spec;
	B.Tokens[0] += 1;
	FPrefixStoredSpec C = A.Spec;
	C.SchemaName = "barter";
	REQUIRE_EQ(Check(A.Bytes, A.Identity, A.Key, A.Spec), EPrefixWrapperCheck::Ok);
	EXPECT_EQ(Check(A.Bytes, A.Identity, A.Key, B), EPrefixWrapperCheck::SpecMismatch);
	EXPECT_EQ(Check(A.Bytes, A.Identity, A.Key, C), EPrefixWrapperCheck::SpecMismatch);
}

HOST_TEST(wrapper_every_truncation_is_a_miss, "2.8 (truncated, zero-length)")
{
	const FWrapped W = MakeWrapped();
	REQUIRE_GT(W.Bytes.size(), static_cast<size_t>(kPrefixWrapperFixedBytes));
	size_t Accepted = 0;
	for (size_t N = 0; N < W.Bytes.size(); ++N)
	{
		const std::vector<uint8_t> Cut(W.Bytes.begin(), W.Bytes.begin() + static_cast<long>(N));
		const EPrefixWrapperCheck Got = Check(Cut, W.Identity, W.Key, W.Spec);
		if (Got != EPrefixWrapperCheck::Truncated)
		{
			++Accepted;
		}
	}
	EXPECT_EQ(Accepted, static_cast<size_t>(0));
}
