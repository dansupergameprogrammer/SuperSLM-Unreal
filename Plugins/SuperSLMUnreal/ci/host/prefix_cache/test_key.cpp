// The cache key (precache plan §5.2) and cell 2.12's key-mutation half that needs no cache.

#include "harness.h"
#include "sim.h"

#include "superslm/sha256.h"

using namespace SuperSLMCore;
using namespace HostSim;

namespace
{
	// An encoder written here from the header's documented layout, independent of the code under test.
	void Field(std::vector<uint8_t>& Out, const uint8_t* Data, size_t Size)
	{
		for (int I = 0; I < 4; ++I)
		{
			Out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(Size) >> (8 * I)));
		}
		Out.insert(Out.end(), Data, Data + Size);
	}
	void FieldStr(std::vector<uint8_t>& Out, const std::string& S)
	{
		Field(Out, reinterpret_cast<const uint8_t*>(S.data()), S.size());
	}
	void FieldU32(std::vector<uint8_t>& Out, uint32_t V)
	{
		const uint8_t B[4] = {static_cast<uint8_t>(V), static_cast<uint8_t>(V >> 8), static_cast<uint8_t>(V >> 16), static_cast<uint8_t>(V >> 24)};
		Field(Out, B, 4);
	}
	std::vector<uint8_t> ReferencePreimage(const FPrefixKeyInputs& In)
	{
		std::vector<uint8_t> Out;
		Field(Out, In.ArtifactHash.data(), 32);
		FieldStr(Out, In.Layer1Tag);
		FieldStr(Out, In.Layer1Commit);
		Field(Out, &In.Backend, 1);
		FieldU32(Out, In.AdapterVendorId);
		FieldU32(Out, In.AdapterDeviceId);
		FieldStr(Out, In.BlobMagic);
		FieldStr(Out, In.AdapterIdentity);
		FieldStr(Out, In.SchemaName);
		std::vector<uint8_t> Tokens;
		for (int32_t T : In.Tokens)
		{
			for (int I = 0; I < 4; ++I)
			{
				Tokens.push_back(static_cast<uint8_t>(static_cast<uint32_t>(T) >> (8 * I)));
			}
		}
		Field(Out, Tokens.data(), Tokens.size());
		Field(Out, In.ParentKey.Bytes.data(), In.bHasParent ? 32 : 0);
		return Out;
	}

	FPrefixKeyInputs BaseInputs()
	{
		return KeyInputsFor(FixtureIdentity(), StoredOf(FixtureSpec(1, 20, "dialogue")));
	}

	FPrefixKey Sha(const std::vector<uint8_t>& Bytes)
	{
		FPrefixKey K;
		superslm::Sha256Hash(Bytes.data(), Bytes.size(), K.Bytes.data());
		return K;
	}
}

HOST_TEST(key_preimage_matches_documented_layout, "§5.2 key encoding")
{
	FPrefixKeyInputs In = BaseInputs();
	In.AdapterVendorId = 0x10DE;
	In.AdapterDeviceId = 0x1E81;
	In.Backend = 1;
	const std::vector<uint8_t> Expected = ReferencePreimage(In);
	EXPECT_EQ(EncodePrefixKeyPreimage(In) == Expected, true);
	EXPECT_EQ(ComputePrefixKey(In) == Sha(Expected), true);

	FPrefixKeyInputs Nested = BaseInputs();
	Nested.bHasParent = true;
	Nested.ParentKey.Bytes.fill(0x5A);
	EXPECT_EQ(EncodePrefixKeyPreimage(Nested) == ReferencePreimage(Nested), true);
	EXPECT_EQ(ComputePrefixKey(Nested) == Sha(ReferencePreimage(Nested)), true);
}

HOST_TEST(key_each_component_changes_the_key, "2.12 (pure half), §5.2 every component")
{
	std::vector<std::pair<const char*, FPrefixKeyInputs>> Variants;
	const FPrefixKeyInputs Base = BaseInputs();
	auto Add = [&](const char* Name, auto Mutate) {
		FPrefixKeyInputs V = Base;
		Mutate(V);
		Variants.push_back({Name, V});
	};
	Add("artifact hash", [](FPrefixKeyInputs& V) { V.ArtifactHash[31] ^= 1; });
	Add("Layer-1 tag", [](FPrefixKeyInputs& V) { V.Layer1Tag = "v1.14.1"; });
	Add("Layer-1 commit", [](FPrefixKeyInputs& V) { V.Layer1Commit[39] = '8'; });
	Add("backend", [](FPrefixKeyInputs& V) { V.Backend = 1; });
	Add("adapter vendor", [](FPrefixKeyInputs& V) { V.AdapterVendorId = 0x1002; });
	Add("adapter device", [](FPrefixKeyInputs& V) { V.AdapterDeviceId = 0x744C; });
	Add("blob magic", [](FPrefixKeyInputs& V) { V.BlobMagic = "SPX2"; });
	Add("adapter identity", [](FPrefixKeyInputs& V) { V.AdapterIdentity = "lora-a"; });
	Add("schema name", [](FPrefixKeyInputs& V) { V.SchemaName = "dialogue2"; });
	Add("schema unbound", [](FPrefixKeyInputs& V) { V.SchemaName.clear(); });
	Add("one token", [](FPrefixKeyInputs& V) { V.Tokens[7] += 1; });
	Add("one token more", [](FPrefixKeyInputs& V) { V.Tokens.push_back(3); });
	Add("one token fewer", [](FPrefixKeyInputs& V) { V.Tokens.pop_back(); });
	Add("parent present", [](FPrefixKeyInputs& V) { V.bHasParent = true; });
	Add("parent key", [](FPrefixKeyInputs& V) { V.bHasParent = true; V.ParentKey.Bytes[0] = 1; });

	const FPrefixKey BaseKey = ComputePrefixKey(Base);
	EXPECT_EQ(BaseKey == FPrefixKey(), false); // a real hash, never all zero
	EXPECT_EQ(BaseKey == ComputePrefixKey(BaseInputs()), true); // deterministic
	for (size_t I = 0; I < Variants.size(); ++I)
	{
		const FPrefixKey K = ComputePrefixKey(Variants[I].second);
		if (K == BaseKey)
		{
			HostTest::Fail(__FILE__, __LINE__, std::string("component did not change the key: ") + Variants[I].first);
		}
		for (size_t J = 0; J < I; ++J)
		{
			if (K == ComputePrefixKey(Variants[J].second))
			{
				HostTest::Fail(__FILE__, __LINE__, std::string("two mutations share a key: ") + Variants[I].first + " / " + Variants[J].first);
			}
		}
	}
}

HOST_TEST(key_length_prefix_pairs_do_not_collide, "2.12 (length-prefix pair, derived from §5.2's order)")
{
	// Pair 1: with field 9's (schema name's) length prefix dropped, ("", [4, 9]) and ("\x08\0\0\0", [9])
	// encode the same bytes: field 10's own prefix (8, then 4) lines up with the name.
	FPrefixKeyInputs A1 = BaseInputs();
	A1.SchemaName.clear();
	A1.Tokens = {4, 9};
	FPrefixKeyInputs B1 = BaseInputs();
	B1.SchemaName = std::string("\x08\x00\x00\x00", 4);
	B1.Tokens = {9};
	// Pair 2 (printable): with every length prefix dropped, ("", [0x64636261, 9]) and ("abcd", [9]) collide.
	FPrefixKeyInputs A2 = BaseInputs();
	A2.SchemaName.clear();
	A2.Tokens = {0x64636261, 9};
	FPrefixKeyInputs B2 = BaseInputs();
	B2.SchemaName = "abcd";
	B2.Tokens = {9};

	// The fixture is right: the mutant encodings collide (computed here, not by the code under test).
	auto WithoutSchemaPrefix = [](const FPrefixKeyInputs& In) {
		std::vector<uint8_t> Out(In.SchemaName.begin(), In.SchemaName.end());
		const uint32_t N = static_cast<uint32_t>(In.Tokens.size() * 4);
		for (int I = 0; I < 4; ++I) { Out.push_back(static_cast<uint8_t>(N >> (8 * I))); }
		for (int32_t T : In.Tokens) { for (int I = 0; I < 4; ++I) { Out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(T) >> (8 * I))); } }
		return Out;
	};
	auto WithoutAnyPrefix = [](const FPrefixKeyInputs& In) {
		std::vector<uint8_t> Out(In.SchemaName.begin(), In.SchemaName.end());
		for (int32_t T : In.Tokens) { for (int I = 0; I < 4; ++I) { Out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(T) >> (8 * I))); } }
		return Out;
	};
	REQUIRE_TRUE(WithoutSchemaPrefix(A1) == WithoutSchemaPrefix(B1));
	REQUIRE_TRUE(WithoutAnyPrefix(A2) == WithoutAnyPrefix(B2));

	// The real encoding keeps them apart.
	EXPECT_EQ(EncodePrefixKeyPreimage(A1) != EncodePrefixKeyPreimage(B1), true);
	EXPECT_EQ(EncodePrefixKeyPreimage(A2) != EncodePrefixKeyPreimage(B2), true);
	EXPECT_EQ(ComputePrefixKey(A1) != ComputePrefixKey(B1), true);
	EXPECT_EQ(ComputePrefixKey(A2) != ComputePrefixKey(B2), true);
	EXPECT_EQ(EncodePrefixKeyPreimage(A1).empty(), false);
}

HOST_TEST(key_hex_round_trip, "§5.7 one file per key")
{
	const FPrefixKey K = ComputePrefixKey(BaseInputs());
	const std::string Hex = PrefixKeyToHex(K);
	REQUIRE_EQ(Hex.size(), static_cast<size_t>(64));
	for (char C : Hex)
	{
		EXPECT_TRUE((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'));
	}
	FPrefixKey Back;
	EXPECT_EQ(PrefixKeyFromHex(Hex, Back), true);
	EXPECT_EQ(Back == K, true);
	std::string Upper = Hex;
	for (char& C : Upper) { if (C >= 'a' && C <= 'f') { C = static_cast<char>(C - 'a' + 'A'); } }
	EXPECT_EQ(PrefixKeyFromHex(Upper, Back), true);
	EXPECT_EQ(PrefixKeyFromHex(Hex.substr(1), Back), false);
	EXPECT_EQ(PrefixKeyFromHex(Hex + "0", Back), false);
	std::string Bad = Hex;
	Bad[10] = 'g';
	EXPECT_EQ(PrefixKeyFromHex(Bad, Back), false);
}
