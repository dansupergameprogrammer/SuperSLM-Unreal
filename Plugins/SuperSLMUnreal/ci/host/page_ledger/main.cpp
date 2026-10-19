// The page-ledger host suite's runner. Usage: page_ledger_tests [substring]
// Runs every registered test (or those whose name contains the substring), prints one line per
// test with its cells and, for a failure, the first failing check, and exits nonzero on any
// failure.

#include "ledger_test_harness.h"

#include <cstdio>
#include <cstring>

int main(int Argc, char** Argv)
{
	const char* Filter = Argc > 1 ? Argv[1] : nullptr;
	int Passed = 0;
	int Failed = 0;
	for (const ledgertest::FTestCase& T : ledgertest::Registry())
	{
		if (Filter && std::strstr(T.Name, Filter) == nullptr) continue;
		ledgertest::FContext Ctx;
		T.Fn(Ctx);
		if (Ctx.bFailed)
		{
			++Failed;
			std::printf("FAIL %-58s [%s]\n     %s\n", T.Name, T.Cells, Ctx.FirstFailure.c_str());
		}
		else
		{
			++Passed;
			std::printf("PASS %-58s (%d checks)\n", T.Name, Ctx.Checks);
		}
	}
	std::printf("\n%d passed, %d failed, %d total\n", Passed, Failed, Passed + Failed);
	return Failed == 0 ? 0 : 1;
}
