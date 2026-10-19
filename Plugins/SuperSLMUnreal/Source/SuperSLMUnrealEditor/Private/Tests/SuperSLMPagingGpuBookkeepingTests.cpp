#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	bool CheckPrefixBookkeeping(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu,
		int32 ExpectedEntries, uint64 ExpectedPayloadBytes, const TCHAR* Where)
	{
		int32 Entries = -1;
		uint64 PayloadBytes = MAX_uint64;
		if (!T.TestTrue(*FString::Printf(TEXT("%s: retained prefix storage is readable"), Where),
			FSuperSLMPagedGpuTestSeam::GetRetainedPrefixBookkeeping(Gpu, Entries, PayloadBytes)))
		{
			return false;
		}
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: retained prefix map entries"), Where), Entries, ExpectedEntries);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: retained prefix payload allocation"), Where), PayloadBytes, ExpectedPayloadBytes);
		return bOk;
	}

	bool CheckLivePayloadObserved(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu)
	{
		int32 Entries = -1;
		uint64 PayloadBytes = 0;
		if (!T.TestTrue(TEXT("live prefix storage observer runs"),
			FSuperSLMPagedGpuTestSeam::GetRetainedPrefixBookkeeping(Gpu, Entries, PayloadBytes)))
		{
			return false;
		}
		bool bOk = T.TestEqual(TEXT("one live prefix is actually observed"), Entries, 1);
		// A nonempty prefix retains its caller-token copy while Ready. This positive control
		// prevents an observer returning constant zero from certifying the release assertions.
		bOk &= T.TestTrue(TEXT("the observer sees allocated live token/history storage"), PayloadBytes > 0);
		return bOk;
	}
}

// G1: PrefixBlockCount bounds live resources, not the number ever created. Empty, single-token,
// aligned and partial-page builds all recycle, and every completed release leaves no historical
// map entry or token/turn/in-flight/fault-buffer allocation. No pool refill probe can see this leak.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPrefixBookkeepingChurnTest,
	"SuperSLM.Paging.Gpu.Bookkeeping.ReleasedPrefixesDoNotAccumulate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPagingGpuPrefixBookkeepingChurnTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	bool bOk = CheckPrefixBookkeeping(*this, Gpu, 0, 0, TEXT("fresh configuration"));
	const int32 B = static_cast<int32>(Rig.Geo.B);
	const int32 Lengths[] = { 0, 1, B, B + 3 };
	for (int32 Round = 0; Round < 3; ++Round)
	{
		for (const int32 Length : Lengths)
		{
			TArray<int32> Tokens;
			FSuperSLMPrefix Prefix;
			if ((Length > 0 && !MakeTokens(*this, *Rig.Model, Length, Round + 131, Tokens))
				|| !BuildPrefix(*this, Gpu, Tokens, Prefix))
			{
				return false;
			}
			if (Length > 0) { bOk &= CheckLivePayloadObserved(*this, Gpu); }
			if (!ReleasePrefixAndDrain(*this, Gpu, Prefix)) { return false; }
			const FString Where = FString::Printf(TEXT("round %d, released %d-token prefix"), Round, Length);
			bOk &= CheckPrefixBookkeeping(*this, Gpu, 0, 0, *Where);
			bOk &= TestEqual(TEXT("the release returns its live-prefix place"), Gpu.GetLivePrefixCount(), 0);
			bOk &= TestEqual(TEXT("no released prefix remains charged on P"), Charged(Gpu, ESuperSLMPagedRegion::P), int64(0));
		}
	}
	return bOk && NoLedgerFault(*this, Gpu, TEXT("prefix bookkeeping churn"));
}

// G1: two real adopters share full pages. Releasing their parent drops payload allocations at
// once, preserves one ghost mapping across release apply and the first return, then removes it
// after the last dependent's applied recycle. The held release opens the worker/apply boundary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPrefixGhostBookkeepingTest,
	"SuperSLM.Paging.Gpu.Bookkeeping.GhostKeepsOnlyItsMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPagingGpuPrefixGhostBookkeepingTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix Parent;
	FSuperSLMGpuSequence Adopters[2];
	const int32 Length = static_cast<int32>(2 * Rig.Geo.B + 3);
	if (!MakeTokens(*this, *Rig.Model, Length, 141, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Parent))
	{
		return false;
	}
	bool bOk = CheckLivePayloadObserved(*this, Gpu);
	for (FSuperSLMGpuSequence& Adopter : Adopters)
	{
		if (!Vend(*this, Gpu, Adopter) || !AdoptAndWait(*this, Gpu, Adopter, Parent)) { return false; }
	}
	bOk &= TestEqual(TEXT("both real adopters are registered"), FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Parent), 2);
	const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::PrefixRelease);
	if (!TestTrue(TEXT("release apply can be held"), Hold.IsValid())) { return false; }
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("parent release is accepted (%s)"), *Error), Gpu.ReleasePrefix(Parent, Error)))
	{
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		return false;
	}
	bOk &= CheckPrefixBookkeeping(*this, Gpu, 1, 0, TEXT("released, before apply"));
	const bool bReached = WaitHoldReached(Gpu, Hold);
	bOk &= TestTrue(TEXT("the real parent release completes while its apply is held"), bReached);
	bOk &= TestTrue(TEXT("the release remains pending at the held boundary"), FSuperSLMPagedGpuTestSeam::IsPrefixReleasePending(Gpu, Parent));
	bOk &= CheckPrefixBookkeeping(*this, Gpu, 1, 0, TEXT("release worker done, apply held"));
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
	if (!bReached || !TestTrue(TEXT("release reaches its applied ghost state"), Pump(Gpu, [&Gpu, &Parent]()
	{
		return !FSuperSLMPagedGpuTestSeam::IsPrefixReleasePending(Gpu, Parent);
	}, OpSeconds)))
	{
		return false;
	}
	bOk &= TestTrue(TEXT("the released parent is a real ghost"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Parent));
	bOk &= CheckPrefixBookkeeping(*this, Gpu, 1, 0, TEXT("two adopters retain the ghost"));
	if (!TestTrue(TEXT("first adopter's recycle applies"), ReturnAndDrain(Gpu, Adopters[0]))) { return false; }
	bOk &= TestEqual(TEXT("one dependent remains after the first return"), FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Parent), 1);
	bOk &= TestTrue(TEXT("the ghost survives the first return"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Parent));
	bOk &= CheckPrefixBookkeeping(*this, Gpu, 1, 0, TEXT("one adopter retains the ghost"));
	if (!TestTrue(TEXT("last adopter's recycle applies"), ReturnAndDrain(Gpu, Adopters[1]))) { return false; }
	bOk &= CheckPrefixBookkeeping(*this, Gpu, 0, 0, TEXT("all dependents returned"));
	bOk &= TestFalse(TEXT("no ghost survives the last return"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Parent));
	bOk &= TestEqual(TEXT("P is fully refunded"), Charged(Gpu, ESuperSLMPagedRegion::P), int64(0));
	return bOk && NoLedgerFault(*this, Gpu, TEXT("ghost bookkeeping"));
}

#endif
