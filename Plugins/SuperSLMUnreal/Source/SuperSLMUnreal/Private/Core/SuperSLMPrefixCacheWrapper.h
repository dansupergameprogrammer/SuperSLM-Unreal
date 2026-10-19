#pragma once

// The plugin's 'SLUP' wrapper around a Layer-1 'SPX1' prefix blob, for the disk tier and for cooked
// prefix sets (precache plan §5.7). The wrapper carries the identity the plugin checks; the payload
// is carried byte-for-byte and never interpreted -- its format and integrity (the SPX1 digest) are
// Layer 1's.
//
// Layout, little-endian (the same field conventions as the save wrapper 'SLUW'):
//   [0]   4  magic 'S','L','U','P'
//   [4]   4  wrapper version (u32, 1)
//   [8]   1  backend (0 CPU, 1 GPU)
//   [9]   3  reserved, zero
//   [12]  16 Layer-1 tag, NUL-padded (longer tags are cut at 16 bytes, as 'SLUW' does)
//   [28]  40 Layer-1 commit, NUL-padded
//   [68]  32 model artifact hash
//   [100] 32 the prefix key (§5.2)
//   [132]    u32 schema-name byte count, then the UTF-8 bytes (0 = unbound)
//            u32 token count, then i32[count]
//            u8 parent present (0 or 1), then the 32-byte parent key when 1
//            u64 payload byte count, then the SPX1 payload; nothing may follow it
//
// Load checks, in this order; the first that fails is the named reason, and every failure is a miss:
//   fewer than 4 bytes (Truncated); magic (BadMagic); fewer than 8 bytes (Truncated); version;
//   fewer than the fixed 132 bytes (Truncated); backend; Layer-1 tag; Layer-1 commit; artifact hash;
//   key; the variable fields' framing (Truncated when a count runs past the end, Malformed for a
//   parent flag above 1 or any byte after the payload); then FULL spec equality (schema name, every
//   token, parent presence and key).
// The spec compare is what makes a key collision a miss rather than a wrong hit (cell 11.4).

#include "SuperSLMPrefixCacheKey.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SuperSLMCore
{
	constexpr uint32_t kPrefixWrapperVersion = 1;
	constexpr size_t kPrefixWrapperFixedBytes = 132;
	constexpr size_t kPrefixWrapperTagBytes = 16;
	constexpr size_t kPrefixWrapperCommitBytes = 40;

	// The engine identity a wrapper is checked against (and the key's fields 1-6).
	struct FPrefixEngineIdentity
	{
		std::array<uint8_t, 32> ArtifactHash{};
		std::string Layer1Tag;
		std::string Layer1Commit;
		uint8_t Backend = 0;
		uint32_t AdapterVendorId = 0;
		uint32_t AdapterDeviceId = 0;
	};

	// The full spec a wrapper stores beside its key.
	struct FPrefixStoredSpec
	{
		std::string SchemaName;
		std::vector<int32_t> Tokens;
		bool bHasParent = false;
		FPrefixKey ParentKey;

		bool operator==(const FPrefixStoredSpec& Other) const
		{
			return SchemaName == Other.SchemaName && Tokens == Other.Tokens && bHasParent == Other.bHasParent &&
				(!bHasParent || ParentKey == Other.ParentKey);
		}
		bool operator!=(const FPrefixStoredSpec& Other) const { return !(*this == Other); }
	};

	enum class EPrefixWrapperCheck : uint8_t
	{
		Ok = 0,
		Truncated = 1,           // shorter than its own framing says (a zero-length file included)
		BadMagic = 2,            // not 'SLUP' (a foreign file)
		UnsupportedVersion = 3,  // any version other than kPrefixWrapperVersion
		BackendMismatch = 4,
		Layer1TagMismatch = 5,   // built under another Layer-1 pin
		Layer1CommitMismatch = 6,
		ArtifactMismatch = 7,    // another model
		KeyMismatch = 8,         // a file under this key's name that carries another key
		Malformed = 9,           // framing that cannot be valid (parent flag > 1, bytes after the payload)
		SpecMismatch = 10,       // equal key, different spec: a collision or a tamper
	};
	constexpr uint32_t kPrefixWrapperCheckCount = 11;

	const char* PrefixWrapperCheckName(EPrefixWrapperCheck Check);

	std::vector<uint8_t> EncodePrefixWrapper(const FPrefixEngineIdentity& Identity, const FPrefixKey& Key,
		const FPrefixStoredSpec& Spec, const uint8_t* Payload, size_t PayloadBytes);

	// Runs every load check. On Ok, OutPayloadOffset/OutPayloadBytes locate the SPX1 payload in Data.
	// On anything else both are 0.
	EPrefixWrapperCheck CheckPrefixWrapper(const uint8_t* Data, size_t Size, const FPrefixEngineIdentity& Expected,
		const FPrefixKey& ExpectedKey, const FPrefixStoredSpec& ExpectedSpec, size_t& OutPayloadOffset, size_t& OutPayloadBytes);

	// Reads only the key field of a wrapper whose magic and version are right (to index a cooked set).
	bool ReadPrefixWrapperKey(const uint8_t* Data, size_t Size, FPrefixKey& OutKey);
}
