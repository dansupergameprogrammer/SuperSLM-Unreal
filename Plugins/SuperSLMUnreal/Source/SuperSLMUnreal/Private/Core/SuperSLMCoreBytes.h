#pragma once

// Little-endian byte helpers the cores share. One definition here, so a unity build that puts the
// cores in one translation unit sees each name once.

#include <cstdint>
#include <vector>

namespace SuperSLMCore
{
	namespace CoreBytes
	{
		inline void PutU32(std::vector<uint8_t>& Out, uint32_t V)
		{
			for (int I = 0; I < 4; ++I)
			{
				Out.push_back(static_cast<uint8_t>(V >> (8 * I)));
			}
		}

		inline void PutU64(std::vector<uint8_t>& Out, uint64_t V)
		{
			for (int I = 0; I < 8; ++I)
			{
				Out.push_back(static_cast<uint8_t>(V >> (8 * I)));
			}
		}

		inline uint32_t GetU32(const uint8_t* At)
		{
			uint32_t V = 0;
			for (unsigned I = 0; I < 4; ++I)
			{
				V |= static_cast<uint32_t>(At[I]) << (8 * I);
			}
			return V;
		}

		inline uint64_t GetU64(const uint8_t* At)
		{
			uint64_t V = 0;
			for (unsigned I = 0; I < 8; ++I)
			{
				V |= static_cast<uint64_t>(At[I]) << (8 * I);
			}
			return V;
		}

		inline uint64_t CeilDiv(uint64_t A, uint64_t B)
		{
			return (A + B - 1) / B;
		}
	}
}
