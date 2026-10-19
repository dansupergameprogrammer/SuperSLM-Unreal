#pragma once

// The prefix cache key (precache plan §5.2): SHA-256 (Layer 1's, superslm/sha256.h) over a
// length-prefixed encoding of every input that decides what a frozen prefix is.
//
// The preimage is exactly eleven fields, in this order, each written as
//     u32 little-endian byte count, then that many bytes:
//
//    #  field                 bytes
//    1  model artifact hash   32 (the artifact integrity hash)
//    2  Layer-1 tag           UTF-8, as compiled (e.g. "v1.14.0")
//    3  Layer-1 commit        UTF-8, as compiled (40 hex digits)
//    4  backend               1 (0 CPU, 1 GPU)
//    5  adapter vendor id     4, u32 LE (0 on the CPU)
//    6  adapter device id     4, u32 LE (0 on the CPU)
//    7  blob magic            UTF-8, "SPX1"
//    8  adapter identity      0 bytes = none (prefixes are base-only)
//    9  schema name           UTF-8, 0 bytes = unbound
//   10  token ids             4 x count, each i32 LE
//   11  parent key            0 bytes for a root prefix, else the parent's 32-byte key
//
// Nothing else is hashed: no domain string, no version field, no padding. The schema name comes
// before the token ids, as §5.2 lists them, so the length prefix of field 9 is what separates
// ("", [4, 9]) from ("\x08\x00\x00\x00", [9]) -- cell 2.12's length-prefix pair.
//
// Hashing is pool-thread work (§5.5): the cache core never calls ComputePrefixKey itself; it emits a
// ComputeKey command and receives the key as a completion.

#include "SuperSLMResidencyCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SuperSLMCore
{
	using FPrefixKey = FResidencyKey;

	struct FPrefixKeyInputs
	{
		std::array<uint8_t, 32> ArtifactHash{};
		std::string Layer1Tag;
		std::string Layer1Commit;
		uint8_t Backend = 0;
		uint32_t AdapterVendorId = 0;
		uint32_t AdapterDeviceId = 0;
		std::string BlobMagic = "SPX1";
		std::string AdapterIdentity; // empty = none
		std::string SchemaName;      // empty = unbound
		std::vector<int32_t> Tokens;
		bool bHasParent = false;
		FPrefixKey ParentKey;        // read only when bHasParent
	};

	// The exact bytes ComputePrefixKey hashes (documented above). Exposed so a test can pin the
	// encoding independently of the hash.
	std::vector<uint8_t> EncodePrefixKeyPreimage(const FPrefixKeyInputs& Inputs);

	// SHA-256 of EncodePrefixKeyPreimage(Inputs).
	FPrefixKey ComputePrefixKey(const FPrefixKeyInputs& Inputs);

	// Lowercase hex, 64 characters.
	std::string PrefixKeyToHex(const FPrefixKey& Key);
	// Parses exactly 64 hex digits (either case). False on anything else.
	bool PrefixKeyFromHex(const std::string& Hex, FPrefixKey& OutKey);
}
