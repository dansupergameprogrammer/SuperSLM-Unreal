#include "SuperSLMPrefixCacheKey.h"

#include "SuperSLMCoreBytes.h"

#include "superslm/sha256.h"

namespace SuperSLMCore
{
	namespace
	{
		using CoreBytes::PutU32;

		// One field: u32 LE byte count, then the bytes.
		void PutKeyField(std::vector<uint8_t>& Out, const uint8_t* Data, size_t Size)
		{
			PutU32(Out, static_cast<uint32_t>(Size));
			Out.insert(Out.end(), Data, Data + Size);
		}

		void PutKeyStringField(std::vector<uint8_t>& Out, const std::string& S)
		{
			PutKeyField(Out, reinterpret_cast<const uint8_t*>(S.data()), S.size());
		}

		void PutKeyU32Field(std::vector<uint8_t>& Out, uint32_t V)
		{
			PutU32(Out, 4);
			PutU32(Out, V);
		}

		int KeyHexDigit(char C)
		{
			if (C >= '0' && C <= '9')
			{
				return C - '0';
			}
			if (C >= 'a' && C <= 'f')
			{
				return C - 'a' + 10;
			}
			if (C >= 'A' && C <= 'F')
			{
				return C - 'A' + 10;
			}
			return -1;
		}
	}

	std::vector<uint8_t> EncodePrefixKeyPreimage(const FPrefixKeyInputs& Inputs)
	{
		std::vector<uint8_t> Out;
		Out.reserve(160 + Inputs.SchemaName.size() + 4 * Inputs.Tokens.size());
		PutKeyField(Out, Inputs.ArtifactHash.data(), Inputs.ArtifactHash.size());
		PutKeyStringField(Out, Inputs.Layer1Tag);
		PutKeyStringField(Out, Inputs.Layer1Commit);
		PutKeyField(Out, &Inputs.Backend, 1);
		PutKeyU32Field(Out, Inputs.AdapterVendorId);
		PutKeyU32Field(Out, Inputs.AdapterDeviceId);
		PutKeyStringField(Out, Inputs.BlobMagic);
		PutKeyStringField(Out, Inputs.AdapterIdentity);
		PutKeyStringField(Out, Inputs.SchemaName);
		PutU32(Out, static_cast<uint32_t>(4 * Inputs.Tokens.size()));
		for (int32_t Token : Inputs.Tokens)
		{
			PutU32(Out, static_cast<uint32_t>(Token));
		}
		PutKeyField(Out, Inputs.ParentKey.Bytes.data(), Inputs.bHasParent ? Inputs.ParentKey.Bytes.size() : 0);
		return Out;
	}

	FPrefixKey ComputePrefixKey(const FPrefixKeyInputs& Inputs)
	{
		const std::vector<uint8_t> Preimage = EncodePrefixKeyPreimage(Inputs);
		FPrefixKey Key;
		superslm::Sha256Hash(Preimage.data(), Preimage.size(), Key.Bytes.data());
		return Key;
	}

	std::string PrefixKeyToHex(const FPrefixKey& Key)
	{
		static const char kDigits[] = "0123456789abcdef";
		std::string Hex;
		Hex.reserve(64);
		for (uint8_t Byte : Key.Bytes)
		{
			Hex.push_back(kDigits[Byte >> 4]);
			Hex.push_back(kDigits[Byte & 0xF]);
		}
		return Hex;
	}

	bool PrefixKeyFromHex(const std::string& Hex, FPrefixKey& OutKey)
	{
		OutKey = FPrefixKey();
		if (Hex.size() != 2 * OutKey.Bytes.size())
		{
			return false;
		}
		FPrefixKey Key;
		for (size_t I = 0; I < Key.Bytes.size(); ++I)
		{
			const int High = KeyHexDigit(Hex[2 * I]);
			const int Low = KeyHexDigit(Hex[2 * I + 1]);
			if (High < 0 || Low < 0)
			{
				return false;
			}
			Key.Bytes[I] = static_cast<uint8_t>((High << 4) | Low);
		}
		OutKey = Key;
		return true;
	}
}
