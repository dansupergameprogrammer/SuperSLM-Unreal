#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	bool ReadRawCompletedBusy(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, double& OutBusy)
	{
		// This observer queues no GPU work and publishes no plugin sample. Its FIFO callback reads
		// the core's completed LastCallTiming while holding the global Layer-1 lock. No other
		// subsystem/workload is driven by these constructions between the tested work and this read.
		return T.TestTrue(TEXT("the no-submit observer reads raw completed Layer-1 timing"),
			FSuperSLMPagedGpuTestSeam::ReadCompletedLayer1Timing(Gpu, OutBusy));
	}

	bool CheckSameTiming(FAutomationTestBase& T, const FSuperSLMGpuTimingSnapshot& Got,
		const FSuperSLMGpuTimingSnapshot& Expected, const TCHAR* Where)
	{
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: completed submission population"), Where), Got.Dispatches, Expected.Dispatches);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: exact busy observation"), Where), Got.GpuBusyMs, Expected.GpuBusyMs);
		return bOk;
	}
}

// G2: the only submitting work after Configure is one-token prefix prefill. A no-submit FIFO
// observer proves it completed between game ticks and independently reads its exact busy time.
// The game sample stays fixed until Tick, then publishes that drain with exactly one submission.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPrefixTimingRegressionTest,
	"SuperSLM.Paging.Gpu.Timing.PrefixOnlyPublishesCompletedDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPagingGpuPrefixTimingRegressionTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens;
	if (!MakeTokens(*this, *Rig.Model, 1, 151, Tokens)) { return false; }
	const int64 CountBefore = Gpu.GetGpuDispatchCount();
	const FSuperSLMGpuTimingSnapshot Initial = Gpu.GetGpuTimingSnapshot();
	bool bOk = TestEqual(TEXT("fresh game sample has no runtime drain"), Initial.Dispatches, int64(0));
	bOk &= TestEqual(TEXT("fresh game sample has no busy observation"), Initial.GpuBusyMs, 0.0);
	FSuperSLMPrefix Prefix;
	FString Error;
	if (!TestEqual(TEXT("one-token prefix is admitted"), uint8(Gpu.CreatePrefix(Tokens, Prefix, &Error)), uint8(ESuperSLMPrefixResult::Success)))
	{
		return false;
	}
	// Drain the non-submitting begin job without ticking, so the next Tick plans exactly the
	// prefix's single token. The warm-up's prior timing is intentionally never the expected value.
	double UnusedBeginTiming = 0.0;
	if (!ReadRawCompletedBusy(*this, Gpu, UnusedBeginTiming)) { return false; }
	bOk &= TestEqual(TEXT("prefix begin submits no runtime layer loop"), Gpu.GetGpuDispatchCount(), CountBefore);
	Gpu.Tick(Step);
	const FSuperSLMGpuTimingSnapshot BeforeCompletion = Gpu.GetGpuTimingSnapshot();
	bOk &= CheckSameTiming(*this, BeforeCompletion, Initial, TEXT("tick that plans the first prefix token"));
	double RawBusy = -1.0;
	if (!ReadRawCompletedBusy(*this, Gpu, RawBusy)) { return false; }
	bOk &= TestEqual(TEXT("one real prefix token submitted exactly once"), Gpu.GetGpuDispatchCount(), CountBefore + 1);
	bOk &= TestTrue(TEXT("the actual prefix drain has finite positive busy time"), FMath::IsFinite(RawBusy) && RawBusy > 0.0);
	bOk &= CheckSameTiming(*this, Gpu.GetGpuTimingSnapshot(), BeforeCompletion, TEXT("worker finished between game ticks"));
	Gpu.Tick(Step);
	const FSuperSLMGpuTimingSnapshot Published = Gpu.GetGpuTimingSnapshot();
	bOk &= TestEqual(TEXT("next tick publishes the prefix's completed population"), Published.Dispatches, CountBefore + 1);
	bOk &= TestEqual(TEXT("published busy is the independent raw prefix drain"), Published.GpuBusyMs, RawBusy);
	bOk &= TestEqual(TEXT("last-busy diagnostic transports the same actual drain"), Gpu.GetLastGpuBusyMs(), RawBusy);
	bOk &= TestEqual(TEXT("the one-token build applied Ready"), uint8(Gpu.GetPrefixPhase(Prefix)), uint8(ESuperSLMPrefixPhase::Ready));
	if (!ReleasePrefixAndDrain(*this, Gpu, Prefix)) { return false; }

	// A second, multi-turn build prevents a one-shot first-prefix publication from passing.
	if (!MakeTokens(*this, *Rig.Model, 3, 152, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Prefix) || !DrainQuiet(Gpu)
		|| !ReadRawCompletedBusy(*this, Gpu, RawBusy))
	{
		return false;
	}
	const FSuperSLMGpuTimingSnapshot Second = Gpu.GetGpuTimingSnapshot();
	bOk &= TestEqual(TEXT("both prefix-only builds account for exactly four tokens"), Gpu.GetGpuDispatchCount(), CountBefore + 4);
	bOk &= TestEqual(TEXT("multi-turn build publishes the completed population"), Second.Dispatches, CountBefore + 4);
	bOk &= TestEqual(TEXT("multi-turn published busy is its latest raw drain"), Second.GpuBusyMs, RawBusy);
	bOk &= TestTrue(TEXT("multi-turn raw timing is finite and positive"), FMath::IsFinite(RawBusy) && RawBusy > 0.0);
	return ReleasePrefixAndDrain(*this, Gpu, Prefix) && bOk;
}

// G3: an armed empty-prompt generation produces exactly one host-finished token. The model's
// device head is explicitly off; there is no embed, layer loop or device-head submission. Both
// scheduler paths must leave the real-submission count and completed drain pair exactly fixed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuArmedTimingRegressionTest,
	"SuperSLM.Paging.Gpu.Timing.ArmedHostFinishDoesNotReplayDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPagingGpuArmedTimingRegressionTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this)) { return false; }
	Rig.Model->bGpuDeviceResidentHead = false;
	FSuperSLMGpuRuntimeConfig Config = MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 1, 0);
	// A preceding real drain must exceed this bound, making replay visible in the worker hitch
	// count as well as in the submission population. The construction checks that premise.
	Config.TickBudgetMs = 1.0e-6;
	if (!ConfigurePooled(*this, Rig, Config)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	bool bOk = TestFalse(TEXT("the native model is configured with a host-only head"), Gpu.IsDeviceHeadActive());
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	const int32 Length = static_cast<int32>(Rig.Geo.B);
	if (!MakeTokens(*this, *Rig.Model, Length, 161, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Prefix) || !DrainQuiet(Gpu))
	{
		return false;
	}
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq, Path) || !AdoptAndWait(*this, Gpu, Seq, Prefix) || !DrainQuiet(Gpu)) { return false; }
		bOk &= TestEqual(TEXT("the aligned adopter is armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);
		const int64 CountBefore = Gpu.GetGpuDispatchCount();
		const int32 HitchesBefore = FSuperSLMGpuTestAccess::GetThreadHitchCount(Gpu);
		const FSuperSLMGpuTimingSnapshot Before = Gpu.GetGpuTimingSnapshot();
		double RawBefore = -1.0;
		if (!ReadRawCompletedBusy(*this, Gpu, RawBefore)) { return false; }
		bOk &= TestTrue(TEXT("the preceding raw drain would be a hitch if replayed"), FMath::IsFinite(RawBefore) && RawBefore > Config.TickBudgetMs);
		bOk &= TestTrue(TEXT("the preceding real prefix work was observed as a hitch"), HitchesBefore > 0);
		bOk &= TestEqual(TEXT("the preceding published drain is the raw reference"), Before.GpuBusyMs, RawBefore);
		TArray<int32> Generated;
		if (!GenerateComplete(*this, Gpu, Seq, TArray<int32>(), 1, Generated) || !DrainQuiet(Gpu)) { return false; }
		bOk &= TestEqual(TEXT("finish-only generation produces its one requested token"), Generated.Num(), 1);
		bOk &= TestEqual(TEXT("host-only armed finish submits exactly zero GPU work"), Gpu.GetGpuDispatchCount(), CountBefore);
		bOk &= CheckSameTiming(*this, Gpu.GetGpuTimingSnapshot(), Before, TEXT("after armed host-only finish"));
		bOk &= TestEqual(TEXT("armed finish does not charge the prior busy time as another worker hitch"),
			FSuperSLMGpuTestAccess::GetThreadHitchCount(Gpu), HitchesBefore);
		bOk &= TestEqual(TEXT("finish consumes the armed state"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
		bOk &= TestEqual(TEXT("finish-only step embeds no position"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(Length));
		double RawAfter = -1.0;
		if (!ReadRawCompletedBusy(*this, Gpu, RawAfter)) { return false; }
		bOk &= TestEqual(TEXT("the raw core drain is unchanged across zero GPU work"), RawAfter, RawBefore);
		if (!TestTrue(TEXT("the adopter returns cleanly"), ReturnAndDrain(Gpu, Seq))) { return false; }
	}
	return ReleasePrefixAndDrain(*this, Gpu, Prefix) && bOk;
}

// G3's adopt population: length zero and exact page multiples share/rebind without any tail
// copy. The partial-page control has one real synchronous tail upload, so a permanently frozen
// submission counter cannot satisfy the construction. Expected zero cases remain exact equality.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuAdoptSubmissionRegressionTest,
	"SuperSLM.Paging.Gpu.Timing.AdoptCountsOnlyRealTailUploads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPagingGpuAdoptSubmissionRegressionTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const int32 B = static_cast<int32>(Rig.Geo.B);
	const int32 Lengths[] = { 0, B, 2 * B, B + 1 };
	bool bOk = true;
	for (const int32 Length : Lengths)
	{
		TArray<int32> Tokens;
		FSuperSLMPrefix Prefix;
		FSuperSLMGpuSequence Seq;
		if ((Length > 0 && !MakeTokens(*this, *Rig.Model, Length, 171, Tokens)) || !BuildPrefix(*this, Gpu, Tokens, Prefix)
			|| !DrainQuiet(Gpu) || !Vend(*this, Gpu, Seq))
		{
			return false;
		}
		const int64 Before = Gpu.GetGpuDispatchCount();
		if (!AdoptAndWait(*this, Gpu, Seq, Prefix) || !DrainQuiet(Gpu)) { return false; }
		const int64 ExpectedUploads = Length % B == 0 ? 0 : 1;
		bOk &= TestEqual(*FString::Printf(TEXT("adopt length %d: exactly %lld real tail-upload submission(s)"), Length, ExpectedUploads),
			Gpu.GetGpuDispatchCount() - Before, ExpectedUploads);
		bOk &= TestEqual(TEXT("adopted context length matches the input prefix"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(Length));
		if (!TestTrue(TEXT("adopter returns after the counter observation"), ReturnAndDrain(Gpu, Seq)) || !ReleasePrefixAndDrain(*this, Gpu, Prefix))
		{
			return false;
		}
	}
	return bOk && NoLedgerFault(*this, Gpu, TEXT("adopt submission population"));
}

#endif
