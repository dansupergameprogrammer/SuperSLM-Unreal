#include "harness.h"

#include <cstring>

namespace HostTest
{
	namespace
	{
		int GFailures = 0;
		std::string GFirstFailure;
	}

	std::vector<FTestCase>& Registry()
	{
		static std::vector<FTestCase> Cases;
		return Cases;
	}

	void Fail(const char* File, int Line, const std::string& Message)
	{
		if (GFailures == 0)
		{
			const char* Slash = std::strrchr(File, '/');
			GFirstFailure = std::string(Slash ? Slash + 1 : File) + ":" + std::to_string(Line) + ": " + Message;
		}
		++GFailures;
	}
}

// Usage: prefix_cache_host_tests [substring]   runs the tests whose name contains substring.
int main(int Argc, char** Argv)
{
	const char* Filter = Argc > 1 ? Argv[1] : nullptr;
	int Run = 0;
	int Passed = 0;
	int Failed = 0;
	for (const HostTest::FTestCase& Case : HostTest::Registry())
	{
		if (Filter != nullptr && std::strstr(Case.Name, Filter) == nullptr)
		{
			continue;
		}
		HostTest::GFailures = 0;
		HostTest::GFirstFailure.clear();
		Case.Fn();
		++Run;
		if (HostTest::GFailures == 0)
		{
			++Passed;
			std::printf("[PASS] %s  (%s)\n", Case.Name, Case.Cells);
		}
		else
		{
			++Failed;
			std::printf("[FAIL] %s  (%s)\n       %d failure(s); first: %s\n", Case.Name, Case.Cells, HostTest::GFailures,
				HostTest::GFirstFailure.c_str());
		}
	}
	std::printf("\n%d tests, %d passed, %d failed\n", Run, Passed, Failed);
	return Failed == 0 && Run > 0 ? 0 : 1;
}
