#pragma once

// A tiny self-contained test harness for the schema jump-forward host suite (copied from ../prefix_cache/harness.h). No exceptions, no RTTI.
// HOST_TEST(Name, "cell ids") registers a test; EXPECT_* records a failure and continues;
// REQUIRE_* records a failure and returns from the test (so a stub's empty result never indexes
// past an end: every red is the assertion's, never a crash).

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace HostTest
{
	using FTestFn = void (*)();

	struct FTestCase
	{
		const char* Name;
		const char* Cells;
		FTestFn Fn;
	};

	std::vector<FTestCase>& Registry();

	struct FRegistrar
	{
		FRegistrar(const char* Name, const char* Cells, FTestFn Fn) { Registry().push_back({Name, Cells, Fn}); }
	};

	void Fail(const char* File, int Line, const std::string& Message);

	template <typename T>
	std::string ToText(const T& Value)
	{
		if constexpr (std::is_enum_v<T>)
		{
			return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(Value)));
		}
		else if constexpr (std::is_same_v<T, bool>)
		{
			return Value ? "true" : "false";
		}
		else if constexpr (std::is_integral_v<T>)
		{
			if constexpr (std::is_signed_v<T>)
			{
				return std::to_string(static_cast<long long>(Value));
			}
			else
			{
				return std::to_string(static_cast<unsigned long long>(Value));
			}
		}
		else if constexpr (std::is_convertible_v<T, std::string>)
		{
			return std::string("\"") + std::string(Value) + "\"";
		}
		else
		{
			return "<value>";
		}
	}
}

#define HT_CONCAT_INNER(A, B) A##B
#define HT_CONCAT(A, B) HT_CONCAT_INNER(A, B)

#define HOST_TEST(Name, Cells)                                                         \
	static void Name();                                                                \
	static const HostTest::FRegistrar HT_CONCAT(Name, _Registrar)(#Name, Cells, &Name); \
	static void Name()

#define EXPECT_TRUE(Cond)                                                          \
	do                                                                             \
	{                                                                              \
		if (!(Cond))                                                               \
		{                                                                          \
			HostTest::Fail(__FILE__, __LINE__, std::string("expected: ") + #Cond); \
		}                                                                          \
	} while (0)

#define REQUIRE_TRUE(Cond)                                                                    \
	do                                                                                        \
	{                                                                                         \
		if (!(Cond))                                                                          \
		{                                                                                     \
			HostTest::Fail(__FILE__, __LINE__, std::string("required: ") + #Cond);           \
			return;                                                                           \
		}                                                                                     \
	} while (0)

#define HT_CMP(A, B, Op, Kind, OnFail)                                                                         \
	do                                                                                                         \
	{                                                                                                          \
		const auto& HtA = (A);                                                                                 \
		const auto& HtB = (B);                                                                                 \
		if (!(HtA Op HtB))                                                                                     \
		{                                                                                                      \
			HostTest::Fail(__FILE__, __LINE__, std::string(Kind) + ": " #A " " #Op " " #B " (" +              \
				HostTest::ToText(HtA) + " vs " + HostTest::ToText(HtB) + ")");                                 \
			OnFail;                                                                                            \
		}                                                                                                      \
	} while (0)

#define EXPECT_EQ(A, B) HT_CMP(A, B, ==, "expected", (void)0)
#define EXPECT_NE(A, B) HT_CMP(A, B, !=, "expected", (void)0)
#define EXPECT_LE(A, B) HT_CMP(A, B, <=, "expected", (void)0)
#define EXPECT_LT(A, B) HT_CMP(A, B, <, "expected", (void)0)
#define EXPECT_GE(A, B) HT_CMP(A, B, >=, "expected", (void)0)
#define EXPECT_GT(A, B) HT_CMP(A, B, >, "expected", (void)0)
#define REQUIRE_EQ(A, B) HT_CMP(A, B, ==, "required", return)
#define REQUIRE_NE(A, B) HT_CMP(A, B, !=, "required", return)
#define REQUIRE_GE(A, B) HT_CMP(A, B, >=, "required", return)
#define REQUIRE_GT(A, B) HT_CMP(A, B, >, "required", return)
