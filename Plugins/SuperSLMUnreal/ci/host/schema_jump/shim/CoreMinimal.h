#pragma once
// Out-of-engine shim for the schema jump-forward host suite's pricing cells (test_pricing.cpp). It
// supplies only what SuperSLMPrefillPlanning.h uses from Unreal's CoreMinimal: the fixed-width
// integer names and FMath's Min/Max/IsFinite. Only test_pricing.cpp includes it. Nothing here is
// compiled into the plugin. (The same shape as an earlier host harness's shim.)
#include <cmath>
#include <cstdint>

using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint32 = std::uint32_t;

struct FMath
{
	template <typename T> static constexpr T Min(T A, T B) { return A < B ? A : B; }
	template <typename T> static constexpr T Max(T A, T B) { return A < B ? B : A; }
	static bool IsFinite(double V) { return std::isfinite(V); }
};
