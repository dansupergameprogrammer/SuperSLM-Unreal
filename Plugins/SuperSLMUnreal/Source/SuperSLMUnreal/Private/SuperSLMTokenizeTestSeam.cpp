#include "SuperSLMTokenizeTestSeam.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <atomic>

namespace
{
	std::atomic<int64> GTokenizeCalls[2] = {{0}, {0}};
}

int64 SuperSLMTokenizeTestSeam::Calls(ESite Site)
{
	return GTokenizeCalls[static_cast<int32>(Site)].load();
}

int64 SuperSLMTokenizeTestSeam::TotalCalls()
{
	return GTokenizeCalls[0].load() + GTokenizeCalls[1].load();
}

void SuperSLMTokenizeTestSeam::Reset()
{
	GTokenizeCalls[0].store(0);
	GTokenizeCalls[1].store(0);
}

void SuperSLMTokenizeTestSeam::CountCall(ESite Site)
{
	GTokenizeCalls[static_cast<int32>(Site)].fetch_add(1);
}

#endif // WITH_DEV_AUTOMATION_TESTS
