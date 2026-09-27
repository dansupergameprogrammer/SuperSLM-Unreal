#include "SuperSLMGpuDigestBridge.h"

#include "superslm/decode_digest.h"

// L2-S2 (plan §10.3 item 5, D-SLM7337): Layer 1's own superslm::ComputeTokenDigest, called
// directly. src/decode_digest.cpp is already compiled into this module
// (Private/Vendored/SuperSLMVendored_decode_digest.cpp), so no second wrapping and no forwarder
// is needed. Available on every platform: the digest takes only emitted tokens.
namespace SuperSLMGpuDigest
{
	void ComputeTokenDigest(const TArray<int32>& Tokens, uint8 OutDigest[32])
	{
		static_assert(sizeof(int32) == sizeof(int32_t), "int32 must match Layer 1's int32_t");
		superslm::ComputeTokenDigest(reinterpret_cast<const int32_t*>(Tokens.GetData()),
			static_cast<size_t>(Tokens.Num()), OutDigest);
	}

	FString DigestToHex(const uint8 Digest[32])
	{
		return BytesToHex(Digest, 32).ToLower();
	}
}
