#pragma once

// A tiny self-contained test harness for the page-ledger host suite. No exceptions, no external
// framework. Each test names the plan cell it covers and the reason it is expected to be red
// against the stub bodies.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ledgertest
{

// A soft check accumulator: records the first failure and keeps a count. Scenarios use it so
// that one scenario body can run against the correct ledger (must hold) and against a mutant
// (must not hold).
struct FSoft
{
	bool bOk = true;
	int Checks = 0;
	std::string First;

	bool Expect(bool bCond, const std::string& What)
	{
		++Checks;
		if (!bCond && bOk)
		{
			bOk = false;
			First = What;
		}
		return bCond;
	}
};

struct FContext
{
	bool bFailed = false;
	int Checks = 0;
	std::string FirstFailure;

	void Fail(const char* File, int Line, const std::string& What)
	{
		if (!bFailed)
		{
			bFailed = true;
			char Buf[64];
			std::snprintf(Buf, sizeof(Buf), ":%d: ", Line);
			const char* Base = File;
			for (const char* P = File; *P; ++P)
			{
				if (*P == '/' || *P == '\\') Base = P + 1;
			}
			FirstFailure = std::string(Base) + Buf + What;
		}
	}
};

using FTestFn = void (*)(FContext&);

struct FTestCase
{
	const char* Name;
	const char* Cells;
	const char* RedReason;
	FTestFn Fn;
};

inline std::vector<FTestCase>& Registry()
{
	static std::vector<FTestCase> Tests;
	return Tests;
}

struct FRegistrar
{
	FRegistrar(const char* Name, const char* Cells, const char* RedReason, FTestFn Fn)
	{
		Registry().push_back(FTestCase{Name, Cells, RedReason, Fn});
	}
};

inline std::string Str(long long V) { return std::to_string(V); }

}  // namespace ledgertest

#define LEDGER_TEST(Name, Cells, RedReason)                                                  \
	static void Name(ledgertest::FContext& Ctx);                                             \
	static ledgertest::FRegistrar Name##_registrar(#Name, Cells, RedReason, &Name);          \
	static void Name(ledgertest::FContext& Ctx)

// Hard check: records and returns from the test.
#define LT_REQUIRE(Cond, What)                                                                \
	do                                                                                        \
	{                                                                                         \
		++Ctx.Checks;                                                                         \
		if (!(Cond))                                                                          \
		{                                                                                     \
			Ctx.Fail(__FILE__, __LINE__, std::string(#Cond) + " -- " + std::string(What));    \
			return;                                                                           \
		}                                                                                     \
	} while (0)

#define LT_EQ(Actual, Expected, What)                                                          \
	do                                                                                         \
	{                                                                                          \
		++Ctx.Checks;                                                                          \
		const long long LtA = static_cast<long long>(Actual);                                  \
		const long long LtE = static_cast<long long>(Expected);                                \
		if (LtA != LtE)                                                                        \
		{                                                                                      \
			Ctx.Fail(__FILE__, __LINE__,                                                       \
			         std::string(What) + ": got " + std::to_string(LtA) + ", want " +          \
			             std::to_string(LtE));                                                 \
			return;                                                                            \
		}                                                                                      \
	} while (0)

// A scenario must hold on the correct ledger.
#define LT_HOLDS(SoftExpr, What)                                                               \
	do                                                                                         \
	{                                                                                          \
		++Ctx.Checks;                                                                          \
		const ledgertest::FSoft LtS = (SoftExpr);                                              \
		if (!LtS.bOk)                                                                          \
		{                                                                                      \
			Ctx.Fail(__FILE__, __LINE__, std::string(What) + ": " + LtS.First);                \
			return;                                                                            \
		}                                                                                      \
	} while (0)

// A scenario must fail on a mutant ledger (the cell kills the mutant).
#define LT_KILLS(SoftExpr, MutantName)                                                         \
	do                                                                                         \
	{                                                                                          \
		++Ctx.Checks;                                                                          \
		const ledgertest::FSoft LtS = (SoftExpr);                                              \
		if (LtS.bOk)                                                                           \
		{                                                                                      \
			Ctx.Fail(__FILE__, __LINE__, std::string("mutant survived: ") + MutantName);       \
			return;                                                                            \
		}                                                                                      \
	} while (0)
