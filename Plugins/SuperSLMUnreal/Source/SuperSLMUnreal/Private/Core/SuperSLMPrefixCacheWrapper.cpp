#include "SuperSLMPrefixCacheWrapper.h"

#include "SuperSLMCoreBytes.h"

#include <cstring>

namespace SuperSLMCore
{
	namespace
	{
		using CoreBytes::GetU32;
		using CoreBytes::GetU64;
		using CoreBytes::PutU32;
		using CoreBytes::PutU64;

		constexpr uint8_t kWrapperMagic[4] = {'S', 'L', 'U', 'P'};
		constexpr size_t kWrapperTagOffset = 12;
		constexpr size_t kWrapperCommitOffset = kWrapperTagOffset + kPrefixWrapperTagBytes;
		constexpr size_t kWrapperArtifactOffset = kWrapperCommitOffset + kPrefixWrapperCommitBytes;
		constexpr size_t kWrapperKeyOffset = kWrapperArtifactOffset + 32;
		static_assert(kWrapperKeyOffset + 32 == kPrefixWrapperFixedBytes, "the fixed header ends after the key");

		// The string, NUL-padded or cut to Width bytes.
		void PutWrapperFixed(std::vector<uint8_t>& B, const std::string& S, size_t Width)
		{
			for (size_t I = 0; I < Width; ++I)
			{
				B.push_back(I < S.size() ? static_cast<uint8_t>(S[I]) : 0);
			}
		}

		bool WrapperFixedEquals(const uint8_t* At, const std::string& S, size_t Width)
		{
			std::vector<uint8_t> Expected;
			PutWrapperFixed(Expected, S, Width);
			return std::memcmp(At, Expected.data(), Width) == 0;
		}

		// A bounds-checked cursor over the variable fields. Counts are compared against what remains,
		// so a huge count can never overflow an offset.
		struct FWrapperCursor
		{
			const uint8_t* Data;
			size_t Size;
			size_t Pos;

			bool Has(uint64_t N) const { return N <= Size - Pos; }
			const uint8_t* Take(size_t N)
			{
				const uint8_t* At = Data + Pos;
				Pos += N;
				return At;
			}
		};
	}

	const char* PrefixWrapperCheckName(EPrefixWrapperCheck Check)
	{
		switch (Check)
		{
		case EPrefixWrapperCheck::Ok: return "Ok";
		case EPrefixWrapperCheck::Truncated: return "Truncated";
		case EPrefixWrapperCheck::BadMagic: return "BadMagic";
		case EPrefixWrapperCheck::UnsupportedVersion: return "UnsupportedVersion";
		case EPrefixWrapperCheck::BackendMismatch: return "BackendMismatch";
		case EPrefixWrapperCheck::Layer1TagMismatch: return "Layer1TagMismatch";
		case EPrefixWrapperCheck::Layer1CommitMismatch: return "Layer1CommitMismatch";
		case EPrefixWrapperCheck::ArtifactMismatch: return "ArtifactMismatch";
		case EPrefixWrapperCheck::KeyMismatch: return "KeyMismatch";
		case EPrefixWrapperCheck::Malformed: return "Malformed";
		case EPrefixWrapperCheck::SpecMismatch: return "SpecMismatch";
		}
		return "Unknown";
	}

	std::vector<uint8_t> EncodePrefixWrapper(const FPrefixEngineIdentity& Identity, const FPrefixKey& Key,
		const FPrefixStoredSpec& Spec, const uint8_t* Payload, size_t PayloadBytes)
	{
		std::vector<uint8_t> B;
		B.reserve(kPrefixWrapperFixedBytes + 4 + Spec.SchemaName.size() + 4 + 4 * Spec.Tokens.size() + 1 + 32 + 8 + PayloadBytes);
		B.insert(B.end(), kWrapperMagic, kWrapperMagic + 4);
		PutU32(B, kPrefixWrapperVersion);
		B.push_back(Identity.Backend);
		B.insert(B.end(), 3, 0);
		PutWrapperFixed(B, Identity.Layer1Tag, kPrefixWrapperTagBytes);
		PutWrapperFixed(B, Identity.Layer1Commit, kPrefixWrapperCommitBytes);
		B.insert(B.end(), Identity.ArtifactHash.begin(), Identity.ArtifactHash.end());
		B.insert(B.end(), Key.Bytes.begin(), Key.Bytes.end());

		PutU32(B, static_cast<uint32_t>(Spec.SchemaName.size()));
		B.insert(B.end(), Spec.SchemaName.begin(), Spec.SchemaName.end());
		PutU32(B, static_cast<uint32_t>(Spec.Tokens.size()));
		for (int32_t Token : Spec.Tokens)
		{
			PutU32(B, static_cast<uint32_t>(Token));
		}
		B.push_back(Spec.bHasParent ? 1 : 0);
		if (Spec.bHasParent)
		{
			B.insert(B.end(), Spec.ParentKey.Bytes.begin(), Spec.ParentKey.Bytes.end());
		}
		PutU64(B, PayloadBytes);
		if (PayloadBytes != 0)
		{
			B.insert(B.end(), Payload, Payload + PayloadBytes);
		}
		return B;
	}

	EPrefixWrapperCheck CheckPrefixWrapper(const uint8_t* Data, size_t Size, const FPrefixEngineIdentity& Expected,
		const FPrefixKey& ExpectedKey, const FPrefixStoredSpec& ExpectedSpec, size_t& OutPayloadOffset, size_t& OutPayloadBytes)
	{
		OutPayloadOffset = 0;
		OutPayloadBytes = 0;

		// The fixed header, each check where it is first detectable.
		if (Size < 4)
		{
			return EPrefixWrapperCheck::Truncated;
		}
		if (std::memcmp(Data, kWrapperMagic, 4) != 0)
		{
			return EPrefixWrapperCheck::BadMagic;
		}
		if (Size < 8)
		{
			return EPrefixWrapperCheck::Truncated;
		}
		if (GetU32(Data + 4) != kPrefixWrapperVersion)
		{
			return EPrefixWrapperCheck::UnsupportedVersion;
		}
		if (Size < kPrefixWrapperFixedBytes)
		{
			return EPrefixWrapperCheck::Truncated;
		}
		if (Data[8] != Expected.Backend)
		{
			return EPrefixWrapperCheck::BackendMismatch;
		}
		if (!WrapperFixedEquals(Data + kWrapperTagOffset, Expected.Layer1Tag, kPrefixWrapperTagBytes))
		{
			return EPrefixWrapperCheck::Layer1TagMismatch;
		}
		if (!WrapperFixedEquals(Data + kWrapperCommitOffset, Expected.Layer1Commit, kPrefixWrapperCommitBytes))
		{
			return EPrefixWrapperCheck::Layer1CommitMismatch;
		}
		if (std::memcmp(Data + kWrapperArtifactOffset, Expected.ArtifactHash.data(), 32) != 0)
		{
			return EPrefixWrapperCheck::ArtifactMismatch;
		}
		if (std::memcmp(Data + kWrapperKeyOffset, ExpectedKey.Bytes.data(), 32) != 0)
		{
			return EPrefixWrapperCheck::KeyMismatch;
		}

		// The variable fields' framing.
		FWrapperCursor C{Data, Size, kPrefixWrapperFixedBytes};
		FPrefixStoredSpec Stored;
		if (!C.Has(4))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		const uint32_t SchemaBytes = GetU32(C.Take(4));
		if (!C.Has(SchemaBytes))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		const uint8_t* Schema = C.Take(SchemaBytes);
		Stored.SchemaName.assign(reinterpret_cast<const char*>(Schema), SchemaBytes);
		if (!C.Has(4))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		const uint32_t TokenCount = GetU32(C.Take(4));
		if (!C.Has(4ull * TokenCount))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		Stored.Tokens.resize(TokenCount);
		for (uint32_t I = 0; I < TokenCount; ++I)
		{
			Stored.Tokens[I] = static_cast<int32_t>(GetU32(C.Take(4)));
		}
		if (!C.Has(1))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		const uint8_t ParentFlag = *C.Take(1);
		if (ParentFlag > 1)
		{
			return EPrefixWrapperCheck::Malformed;
		}
		Stored.bHasParent = ParentFlag == 1;
		if (Stored.bHasParent)
		{
			if (!C.Has(32))
			{
				return EPrefixWrapperCheck::Truncated;
			}
			std::memcpy(Stored.ParentKey.Bytes.data(), C.Take(32), 32);
		}
		if (!C.Has(8))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		const uint64_t PayloadBytes = GetU64(C.Take(8));
		if (!C.Has(PayloadBytes))
		{
			return EPrefixWrapperCheck::Truncated;
		}
		if (Size - C.Pos != PayloadBytes)
		{
			return EPrefixWrapperCheck::Malformed;
		}

		// Full spec equality: a key collision is a miss, never a wrong hit.
		if (Stored != ExpectedSpec)
		{
			return EPrefixWrapperCheck::SpecMismatch;
		}
		OutPayloadOffset = C.Pos;
		OutPayloadBytes = static_cast<size_t>(PayloadBytes);
		return EPrefixWrapperCheck::Ok;
	}

	bool ReadPrefixWrapperKey(const uint8_t* Data, size_t Size, FPrefixKey& OutKey)
	{
		OutKey = FPrefixKey();
		if (Size < kPrefixWrapperFixedBytes || std::memcmp(Data, kWrapperMagic, 4) != 0 || GetU32(Data + 4) != kPrefixWrapperVersion)
		{
			return false;
		}
		std::memcpy(OutKey.Bytes.data(), Data + kWrapperKeyOffset, 32);
		return true;
	}
}
