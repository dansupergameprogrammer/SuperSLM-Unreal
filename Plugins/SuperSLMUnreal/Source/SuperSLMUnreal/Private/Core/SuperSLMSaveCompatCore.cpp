#include "SuperSLMSaveCompatCore.h"

#include <cstring>

namespace SuperSLMCore
{
	namespace
	{
		// The one entry (precache plan §5.11 (c), PC-13; rev 1.18.1, PC-15 Q8: the 1.13.0 row is dropped). The
		// predecessor is the paging release's engine, v1.13.1, which 1.14.0 keeps byte for byte in its four
		// sequence formats, so a 1.13.1 save of one of them restores on 1.14.0. The row matches tag and commit
		// together: d697927 is v1.13.1^{commit} (the tag object 034a76a peels to it). A wrapper tagged 1.13.0,
		// or tagged 1.13.1 under any other commit, is Layer1Mismatch.
		const char* const kSaveCompat1p13Commits[] = {
			"d697927f6d5ce5fa73ecbb8660020a425518b923",
		};
		const char* const kSaveCompat1p13Magics[] = {"SSB5", "SSB6", "SLM5", "SLM6"};

		const FSaveCompatRow kSaveCompatRows[] = {
			{"v1.14.0", "v1.13.1", kSaveCompat1p13Commits, sizeof(kSaveCompat1p13Commits) / sizeof(kSaveCompat1p13Commits[0]), kSaveCompat1p13Magics,
				sizeof(kSaveCompat1p13Magics) / sizeof(kSaveCompat1p13Magics[0])},
		};

		bool SaveCompatRowAccepts(const FSaveCompatRow& Row, const std::string& CompiledTag, const std::string& WrapperTag,
			const std::string& WrapperCommit, const uint8_t* Payload, size_t PayloadBytes)
		{
			if (Row.CompiledTag == nullptr || Row.AcceptedTag == nullptr || CompiledTag != Row.CompiledTag ||
				WrapperTag != Row.AcceptedTag)
			{
				return false;
			}
			if (Row.AcceptedCommitCount != 0)
			{
				bool bCommit = false;
				for (size_t I = 0; I < Row.AcceptedCommitCount; ++I)
				{
					bCommit = bCommit || WrapperCommit == Row.AcceptedCommits[I];
				}
				if (!bCommit)
				{
					return false;
				}
			}
			if (Payload == nullptr || PayloadBytes < 4)
			{
				return false;
			}
			for (size_t I = 0; I < Row.AcceptedMagicCount; ++I)
			{
				if (std::memcmp(Payload, Row.AcceptedMagics[I], 4) == 0)
				{
					return true;
				}
			}
			return false;
		}
	}

	const FSaveCompatRow* SaveCompatTable(size_t& OutRowCount)
	{
		OutRowCount = sizeof(kSaveCompatRows) / sizeof(kSaveCompatRows[0]);
		return kSaveCompatRows;
	}

	ESaveLayer1Check CheckSaveLayer1With(const FSaveCompatRow* Rows, size_t RowCount, const std::string& CompiledTag,
		const std::string& WrapperTag, const std::string& WrapperCommit, const uint8_t* Payload, size_t PayloadBytes)
	{
		if (WrapperTag == CompiledTag)
		{
			return ESaveLayer1Check::SamePin;
		}
		for (size_t I = 0; Rows != nullptr && I < RowCount; ++I)
		{
			if (SaveCompatRowAccepts(Rows[I], CompiledTag, WrapperTag, WrapperCommit, Payload, PayloadBytes))
			{
				return ESaveLayer1Check::CompatibilityEntry;
			}
		}
		return ESaveLayer1Check::Layer1Mismatch;
	}

	ESaveLayer1Check CheckSaveLayer1(const std::string& CompiledTag, const std::string& WrapperTag,
		const std::string& WrapperCommit, const uint8_t* Payload, size_t PayloadBytes)
	{
		size_t Count = 0;
		const FSaveCompatRow* Rows = SaveCompatTable(Count);
		return CheckSaveLayer1With(Rows, Count, CompiledTag, WrapperTag, WrapperCommit, Payload, PayloadBytes);
	}
}
