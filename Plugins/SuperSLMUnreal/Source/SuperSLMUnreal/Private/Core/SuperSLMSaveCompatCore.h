#pragma once

// The 'SLUW' save wrapper's Layer-1 pin check, with its compatibility table (precache plan §5.11 (c),
// PC-13; cell 9.11). Engine-independent: C++17 and the standard library only, no exceptions, no
// RTTI, no Unreal header, no Layer-1 call. Both subsystems' restore paths call it before anything is
// queued.
//
// The rule:
//   - a wrapper whose Layer-1 tag equals the tag this build compiled is SamePin (the existing rule;
//     the commit is not compared, as before);
//   - otherwise it is accepted only by a row of the table whose CompiledTag is this build's tag,
//     whose AcceptedTag is the wrapper's tag, whose AcceptedCommits contain the wrapper's commit
//     (when the row lists any), and whose AcceptedMagics contain the payload's first four bytes;
//   - everything else is Layer1Mismatch.
// The table has one row per predecessor release whose saves the engine was shown to keep byte for
// byte (the plan's 9.9 and 9.10 re-run against it). A format that leaves the list keeps its
// boundary by name.

#include <cstddef>
#include <cstdint>
#include <string>

namespace SuperSLMCore
{
	enum class ESaveLayer1Check : uint8_t
	{
		SamePin = 0,            // the wrapper's tag is this build's
		CompatibilityEntry = 1, // another tag, accepted by a table row
		Layer1Mismatch = 2,     // refused by name; nothing is drawn
	};

	struct FSaveCompatRow
	{
		const char* CompiledTag = nullptr;            // the build this row applies to
		const char* AcceptedTag = nullptr;            // the predecessor's tag, as the wrapper stores it
		const char* const* AcceptedCommits = nullptr; // full commit ids; none listed = tag alone
		size_t AcceptedCommitCount = 0;
		const char* const* AcceptedMagics = nullptr;  // 4-character payload magics
		size_t AcceptedMagicCount = 0;
	};

	// The shipped table.
	const FSaveCompatRow* SaveCompatTable(size_t& OutRowCount);

	// The check against the shipped table. Payload is the Layer-1 payload inside the wrapper.
	ESaveLayer1Check CheckSaveLayer1(const std::string& CompiledTag, const std::string& WrapperTag,
		const std::string& WrapperCommit, const uint8_t* Payload, size_t PayloadBytes);

	// The same check against a given table (the host suite's mutants use it).
	ESaveLayer1Check CheckSaveLayer1With(const FSaveCompatRow* Rows, size_t RowCount, const std::string& CompiledTag,
		const std::string& WrapperTag, const std::string& WrapperCommit, const uint8_t* Payload, size_t PayloadBytes);
}
