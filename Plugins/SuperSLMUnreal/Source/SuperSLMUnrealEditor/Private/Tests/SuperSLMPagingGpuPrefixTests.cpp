// Plugin paging plan, GPU red suite (step K5): GPU prefixes, nested prefixes, the build seat and
// schema-bound prefixes (§3.3, §3.4; §7 cells 2.5, 2.6, 3.4, 4.3, 6.2, 6.5, 8.2, 10.3, and §5's
// KvPagesExhausted for prefix and nested).
//
//   SuperSLM.Paging.Gpu.Prefix.Lengths                 cell 4.3, 0.5B legs                       [K7]
//   SuperSLM.Paging.Gpu.Prefix.TwoCap.Lengths          cell 4.3 [two-cap]                         [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Prefix.ParentNotUsable         cell 2.5, GPU                              [K7]
//   SuperSLM.Paging.Gpu.Prefix.TwoCap.CapBoundsPrefix  cell 2.6 [two-cap], the prefix legs        [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Prefix.BuiltTwice              cell 6.2                                   [K7]
//   SuperSLM.Paging.Gpu.Prefix.BuildIgnoresLoad        cell 6.5                                   [K7]
//   SuperSLM.Paging.Gpu.Prefix.BuildCadence            cell 3.4                                   [K7]
//   SuperSLM.Paging.Gpu.Prefix.SchemaBound             cell 8.2, the schema legs                  [K7]
//   SuperSLM.Paging.Gpu.Prefix.TwoCap.AdapterBound     cell 8.2, the adapter leg                  [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Prefix.Nested                  cell 10.3, GPU                             [K7]
//   SuperSLM.Paging.Gpu.Prefix.PagesExhausted          §5 KvPagesExhausted, prefix and nested     [K7]
//
// The position oracle (cell 10.4 (a)) grades every adopter here: the engine's own context_length,
// read from a save's 'SLM6' header, equals L + s + n - 1, and the plugin's ContextUsed and armed bit
// match that save. Token equality against a no-prefix twin is not the GPU's oracle (the arc and the
// per-token feed may differ numerically: UP-13, cell 10.5). Lengths a probe counts exactly are
// page-aligned or say why not. Every test fails first at ConfigurePooled() (or, for the 2.6 and
// 2.5 legs, at the prefix verbs' K5 stubs, which return NotConfigured) while pooled mode is
// unbuilt.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "SuperSLMGpuSchemaHandle.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// Adopts Prefix onto a fresh sequence on Path, feeds Suffix and N new tokens to MaxNewTokens, and
	// grades the position oracle at L + s + n - 1 and the armed bit at the save. The sequence is
	// returned before this returns; its tokens go to OutTokens.
	bool AdoptGenerateAndGrade(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix, int64 L, const TArray<int32>& Suffix, int32 N,
		ESuperSLMGpuDecodePath Path, const FString& What, TArray<int32>* OutTokens = nullptr)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq, Path) || !AdoptAndWait(T, Gpu, Seq, Prefix))
		{
			return false;
		}
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: armed after the adopt iff L > 0"), *What), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), L > 0 ? 1 : 0);
		TArray<int32> Tokens;
		bOk &= GenerateComplete(T, Gpu, Seq, Suffix, N, Tokens);
		bOk &= PositionOracle(T, Gpu, Seq, L + Suffix.Num() + N - 1, *What);
		if (OutTokens)
		{
			*OutTokens = Tokens;
		}
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	// Pumps until every sequence's generation has ended (Complete or Faulted, nothing pending).
	bool WaitAllEnded(USuperSLMGpuSubsystem& Gpu, const TArray<FSuperSLMGpuSequence>& Seqs)
	{
		return Pump(Gpu, [&Gpu, &Seqs]()
		{
			for (const FSuperSLMGpuSequence& Seq : Seqs)
			{
				const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
				if ((Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted) || Gpu.GetPendingLifecycleOperationCount(Seq) != 0)
				{
					return false;
				}
			}
			return true;
		}, GenerateSeconds);
	}

	// Configure at the lowest K Configure() accepts for C (its KBelowMinimum report names it).
	bool ConfigurePooledAtFloor(FAutomationTestBase& T, FGpuPagingRig& Rig, FSuperSLMGpuRuntimeConfig C)
	{
		C.K = 1;
		const FSuperSLMGpuConfigureReport Probe = Rig.Gpu->Configure(Rig.Model, C);
		if (Probe.Result == ESuperSLMGpuConfigureResult::KBelowMinimum)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("KBelowMinimum names the floor (%s)"), *Probe.Message), Probe.MinimumK > 1))
			{
				return false;
			}
			C.K = Probe.MinimumK;
		}
		else if (Probe.Result == ESuperSLMGpuConfigureResult::Success)
		{
			C.K = FMath::Max(1, Probe.MinimumK);
		}
		else
		{
			T.AddError(FString::Printf(TEXT("Configure() at K = 1 failed for a reason other than K (%s)"), *Probe.Message));
			return false;
		}
		return ConfigurePooled(T, Rig, C);
	}
}

// Cell 4.3, the 0.5B legs: prefix lengths {0, 1, B - 1, B, B + 1, 1000, cap - 1} at B = 16, each built,
// charged R(max(L, 1)) at admission, settled to ceil(L/B) at freeze (form 1), adopted and generated,
// and graded by the position oracle. Length 0 leaves the sequence unarmed (so its suffix cannot be
// empty). At L >= 4000 the limit leaves no room for a suffix, so the suffix is empty and n = 2.
// Then the empty nested child on parents of 16 and 17 tokens settles to 0 and 1 private pages: by
// the exact two-sided probe (drawn 34 and 36 at (1 slot, budget 512, 2 prefix blocks, H 0)) and by
// the ledger's settle.
// Mutants: an empty child settled to 0 (charge < drawn at the 17-token parent, form 1 and the exact
// probe); length 0 adopted as armed (the empty-prompt refusal or the oracle's armed bit); the finish
// bridge on the armed step (one short in the oracle).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPrefixLengthsTest, "SuperSLM.Paging.Gpu.Prefix.Lengths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuPrefixLengthsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 2, 0);
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuGeometry& G = Rig.Geo;
	const int64 Lengths[] = { 0, 1, G.B - 1, G.B, G.B + 1, 1000, G.Cp - 1 };
	bool bOk = true;
	for (const int64 L : Lengths)
	{
		TArray<int32> PrefixTokens, Suffix;
		const bool bNoRoom = L >= 4000;
		if (!MakeTokens(*this, *Rig.Model, int32(L), 5, PrefixTokens) || !MakeTokens(*this, *Rig.Model, bNoRoom ? 0 : 4, 6, Suffix))
		{
			return false;
		}
		const FString What = FString::Printf(TEXT("L = %lld"), L);
		FSuperSLMPrefix Prefix;
		FString Error;
		const int64 PBefore = Charged(Gpu, ERegion::P);
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(PrefixTokens, Prefix, &Error);
		if (!TestEqual(*FString::Printf(TEXT("%s: admitted (%s)"), *What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			bOk = false;
			continue;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: charged R(max(L, 1)) at admission"), *What), Charged(Gpu, ERegion::P) - PBefore, G.PrefixCharge(L));
		if (!WaitPrefixReady(*this, Gpu, Prefix))
		{
			bOk = false;
			continue;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: settles to ceil(L/B) at freeze"), *What), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Prefix), G.OccupiedPages(L));
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, *FString::Printf(TEXT("%s, frozen"), *What));
		bOk &= AdoptGenerateAndGrade(*this, Gpu, Prefix, L, Suffix, bNoRoom ? 2 : 3, ESuperSLMGpuDecodePath::OneCall, What);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	}

	// The empty nested child on parents of length B and B + 1.
	const int64 Pool = ExpectedRegions(G, C).Pool();
	for (const int64 Lp : { G.B, G.B + 1 })
	{
		TArray<int32> ParentTokens;
		if (!MakeTokens(*this, *Rig.Model, int32(Lp), 7, ParentTokens))
		{
			return false;
		}
		FSuperSLMPrefix Parent, Child;
		auto Build = [&]()
		{
			return BuildPrefix(*this, Gpu, ParentTokens, Parent) && BuildNested(*this, Gpu, Parent, TArray<int32>(), Child);
		};
		if (!Build())
		{
			return false;
		}
		const int64 Private = G.NestedPrivate(Lp, 0);
		const FString What = FString::Printf(TEXT("parent %lld, empty child"), Lp);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the plan's private pages"), *What), Private, Lp % G.B == 0 ? int64(0) : int64(1));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the ledger's settle"), *What), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Child), Private);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, *What);
		// Drawn: the warm holder's R(512), the parent's occupied pages and the child's private pages.
		const int64 Drawn = G.R(512) + G.OccupiedPages(Lp) + Private;
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the construction's drawn figure"), *What), Drawn, Lp == G.B ? int64(34) : int64(36));
		bOk &= ProbeDrawnExactly(*this, Gpu, Pool, Drawn, [&]() { return ConfigurePooled(*this, Rig, C) && Build(); }, *What);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Child);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Parent);
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 4.3"));
	return bOk;
}

// Cell 4.3 [two-cap]: at the 1.5B artifact with ContextCap 4,096, a prefix of cap - 1 = 4,095 tokens
// (R = 257 = P_blk) is admitted at PrefixBlockCount = 1, adopted, and generated to n = 2 (the
// adopter's limit is ContextCap), graded by the position oracle at 4,096.
// Mutant: a P block of ceil(ContextCap/B) = 256, which refuses it KvPagesExhausted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapPrefixLengthsTest, "SuperSLM.Paging.Gpu.Prefix.TwoCap.Lengths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapPrefixLengthsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 1, 0)) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 4095, 9, Tokens))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("[two-cap]: R(4,095) at the model's cap is 257"), Rig.Geo.PrefixCharge(4095), int64(257));
	if (!BuildPrefix(*this, Gpu, Tokens, Prefix))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("[two-cap]: the prefix settles to 256 pages"), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Prefix), int64(256));
	bOk &= AdoptGenerateAndGrade(*this, Gpu, Prefix, 4095, TArray<int32>(), 2, ESuperSLMGpuDecodePath::OneCall, TEXT("[two-cap] L = 4,095"));
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 4.3 [two-cap]"));
	return bOk;
}

// Cell 2.5 on the GPU (§3.3, §3.4): CreatePrefixFrom() with a released parent, an unfrozen one (still
// building), and an unknown handle each resolves ParentNotUsable, with no handle, nothing charged on
// P and no prefix created.
// Mutant: the parent check absent (begin_from on a released or unfrozen handle).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuParentNotUsableTest, "SuperSLM.Paging.Gpu.Prefix.ParentNotUsable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuParentNotUsableTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 3, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, ChildTokens;
	if (!MakeTokens(*this, *Rig.Model, 1000, 1, Tokens) || !MakeTokens(*this, *Rig.Model, 50, 2, ChildTokens))
	{
		return false;
	}
	bool bOk = true;
	auto ExpectNotUsable = [&](const FSuperSLMPrefix& Parent, const TCHAR* What)
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		const int32 LiveBefore = Gpu.GetLivePrefixCount();
		FSuperSLMPrefix Out;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefixFrom(Parent, ChildTokens, Out, &Error);
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: ParentNotUsable (%s)"), What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::ParentNotUsable);
		bOk &= TestFalse(*FString::Printf(TEXT("%s parent: no handle"), What), Out.IsValid());
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: nothing charged on P"), What), Charged(Gpu, ERegion::P), PBefore);
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: no prefix created"), What), Gpu.GetLivePrefixCount(), LiveBefore);
	};

	FSuperSLMPrefix Released;
	if (!BuildPrefix(*this, Gpu, Tokens, Released) || !ReleasePrefixAndDrain(*this, Gpu, Released))
	{
		return false;
	}
	ExpectNotUsable(Released, TEXT("a released"));

	FSuperSLMPrefix Building;
	FString BuildError;
	if (!TestEqual(*FString::Printf(TEXT("a 1,000-token build is admitted (%s)"), *BuildError), (uint8)Gpu.CreatePrefix(Tokens, Building, &BuildError), (uint8)ESuperSLMPrefixResult::Success))
	{
		return false;
	}
	bOk &= TestTrue(TEXT("the build is not yet frozen (one token per build turn)"), Gpu.GetPrefixPhase(Building) != ESuperSLMPrefixPhase::Ready);
	ExpectNotUsable(Building, TEXT("an unfrozen"));
	bOk &= WaitPrefixReady(*this, Gpu, Building);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Building);

	FSuperSLMPrefix Unknown;
	Unknown.Id = (int64(1) << 60) + 12345;
	ExpectNotUsable(Unknown, TEXT("an unknown"));
	return bOk;
}

// Cell 2.6 [two-cap], the prefix legs (coverage re-audit R1-G1): at the 1.5B artifact with ContextCap
// 4,096 and PrefixBlockCount = 2, CreatePrefix() with ContextCap + 1 = 4,097 tokens resolves Malformed
// and CreatePrefixFrom() whose parent (1,008) + child (3,089) = 4,097 exceeds ContextCap resolves
// Malformed, each with nothing charged on P and no prefix created. The budget leg (an 'SLM6' with
// budget 4,097) is in SuperSLM.Paging.Gpu.Restore.TwoCap.HostileHeader.
// Mutant: each bound at the model's cap, which admits the prefix (R = 258) and leaves its adopters
// starting above their own limit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapCapBoundsPrefixTest, "SuperSLM.Paging.Gpu.Prefix.TwoCap.CapBoundsPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapCapBoundsPrefixTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 2, 0)) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> TooLong, ParentTokens, ChildTokens;
	if (!MakeTokens(*this, *Rig.Model, 4097, 1, TooLong) || !MakeTokens(*this, *Rig.Model, 1008, 2, ParentTokens) || !MakeTokens(*this, *Rig.Model, 3089, 3, ChildTokens))
	{
		return false;
	}
	bool bOk = true;
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("[two-cap]: CreatePrefix(4,097 tokens) is Malformed (%s)"), *Error), (uint8)Gpu.CreatePrefix(TooLong, Out, &Error),
			(uint8)ESuperSLMPrefixResult::Malformed);
		bOk &= TestFalse(TEXT("[two-cap]: no handle"), Out.IsValid());
		bOk &= TestEqual(TEXT("[two-cap]: nothing charged on P"), Charged(Gpu, ERegion::P), PBefore);
		bOk &= TestEqual(TEXT("[two-cap]: no prefix is live"), Gpu.GetLivePrefixCount(), 0);
	}
	FSuperSLMPrefix Parent;
	if (!BuildPrefix(*this, Gpu, ParentTokens, Parent))
	{
		return false;
	}
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("[two-cap]: CreatePrefixFrom(1,008 + 3,089) is Malformed (%s)"), *Error), (uint8)Gpu.CreatePrefixFrom(Parent, ChildTokens, Out, &Error),
			(uint8)ESuperSLMPrefixResult::Malformed);
		bOk &= TestFalse(TEXT("[two-cap]: no child handle"), Out.IsValid());
		bOk &= TestEqual(TEXT("[two-cap]: nothing charged on P for the child"), Charged(Gpu, ERegion::P), PBefore);
		bOk &= TestEqual(TEXT("[two-cap]: only the parent is live"), Gpu.GetLivePrefixCount(), 1);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Parent);
	return bOk;
}

// Cell 6.2: a GPU prefix built twice (released in between) gives adopters with identical outputs, on
// the device the test runs on (K8 runs it on both). Both adopters also pass the position oracle.
// Mutant: a build whose content depends on anything but its tokens (a chunking or schedule leak).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuBuiltTwiceTest, "SuperSLM.Paging.Gpu.Prefix.BuiltTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuBuiltTwiceTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, Suffix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 11, Tokens) || !MakeTokens(*this, *Rig.Model, 5, 12, Suffix))
	{
		return false;
	}
	TArray<int32> Outputs[2];
	bool bOk = true;
	for (int32 Build = 0; Build < 2; ++Build)
	{
		FSuperSLMPrefix Prefix;
		if (!BuildPrefix(*this, Gpu, Tokens, Prefix))
		{
			return false;
		}
		bOk &= AdoptGenerateAndGrade(*this, Gpu, Prefix, 1000, Suffix, 16, ESuperSLMGpuDecodePath::OneCall, FString::Printf(TEXT("build %d"), Build + 1), &Outputs[Build]);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	}
	bOk &= TestEqual(TEXT("the two builds' adopters emit identical tokens"), Outputs[1], Outputs[0]);
	return bOk;
}

// Cell 6.5 (audit G13; coverage audit B3): the build ignores load. One 208-token prefix is built
// under BlockCount 1 (no load) and 4 (three generating sequences beside the build); under Composed-
// only, OneCall-only and mixed rotations; at two tick budgets (1,000 ms and 2 ms); and at
// DispatchBudget equal to one layer's dispatches (Configure's floor) and to one token's dispatches
// - 1, each at the K Configure() states as its floor. Every build completes, and the adopters' tokens
// are byte-equal across every leg.
// Mutant: the configured DispatchBudget passed to sslm_gpu_prefix_prefill, which is refused
// DISPATCH_BUDGET_TOO_SMALL in the last two legs (the build faults; the leg fails at WaitPrefixReady).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuBuildIgnoresLoadTest, "SuperSLM.Paging.Gpu.Prefix.BuildIgnoresLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuBuildIgnoresLoadTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, Suffix, LoadPrompt;
	if (!MakeTokens(*this, *Rig.Model, 208, 21, Tokens) || !MakeTokens(*this, *Rig.Model, 4, 22, Suffix) || !MakeTokens(*this, *Rig.Model, 8, 23, LoadPrompt))
	{
		return false;
	}
	enum class ERotation : uint8 { OneCall, Composed, Mixed };
	struct FLeg
	{
		const TCHAR* Name;
		int32 BlockCount;
		ERotation Rotation;
		double TickBudgetMs;
		int32 DispatchLayers; // 0: one whole token (the fixture's), else this many layers' dispatches
		int32 DispatchDelta;  // added to the dispatch budget
	};
	const int32 Layers = LayersOf(EGpuArtifact::AEx05B);
	const FLeg Legs[] = {
		{ TEXT("BlockCount 1, no load"), 1, ERotation::OneCall, 1000.0, 0, 0 },
		{ TEXT("BlockCount 4, OneCall rotation"), 4, ERotation::OneCall, 1000.0, 0, 0 },
		{ TEXT("BlockCount 4, Composed rotation"), 4, ERotation::Composed, 1000.0, 0, 0 },
		{ TEXT("BlockCount 4, mixed rotation"), 4, ERotation::Mixed, 1000.0, 0, 0 },
		{ TEXT("BlockCount 4, mixed, tick budget 2 ms"), 4, ERotation::Mixed, 2.0, 0, 0 },
		{ TEXT("BlockCount 4, mixed, DispatchBudget one layer"), 4, ERotation::Mixed, 1000.0, 1, 0 },
		{ TEXT("BlockCount 4, mixed, DispatchBudget one token - 1"), 4, ERotation::Mixed, 1000.0, Layers, -1 },
	};
	TArray<int32> Reference;
	bool bOk = true;
	for (const FLeg& Leg : Legs)
	{
		FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, Leg.BlockCount, 512, 1, 0);
		C.TickBudgetMs = Leg.TickBudgetMs;
		if (Leg.DispatchLayers > 0)
		{
			C.DispatchBudget = uint32(int64(SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(Leg.DispatchLayers)) + Leg.DispatchDelta);
		}
		if (!ConfigurePooledAtFloor(*this, Rig, C))
		{
			bOk = false;
			continue;
		}
		TArray<FSuperSLMGpuSequence> Load;
		TArray<FSuperSLMLifecycleOpHandle> LoadHandles;
		for (int32 I = 0; I < Leg.BlockCount - 1; ++I)
		{
			const bool bComposed = Leg.Rotation == ERotation::Composed || (Leg.Rotation == ERotation::Mixed && I % 2 == 0);
			FSuperSLMGpuSequence Seq;
			if (!Vend(*this, Gpu, Seq, bComposed ? ESuperSLMGpuDecodePath::Composed : ESuperSLMGpuDecodePath::OneCall))
			{
				return false;
			}
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = LoadPrompt;
			Request.MaxNewTokens = 400;
			LoadHandles.Add(Gpu.RequestBeginGeneration(Seq, Request));
			Load.Add(Seq);
		}
		TickN(*this, Gpu, 2, Leg.Name);
		FSuperSLMPrefix Prefix;
		if (!BuildPrefix(*this, Gpu, Tokens, Prefix))
		{
			bOk = false;
		}
		else
		{
			const ESuperSLMGpuDecodePath AdopterPath = Leg.Rotation == ERotation::Composed ? ESuperSLMGpuDecodePath::Composed : ESuperSLMGpuDecodePath::OneCall;
			// BlockCount - 1 load sequences leave the adopter one free slot.
			TArray<int32> Out;
			bOk &= AdoptGenerateAndGrade(*this, Gpu, Prefix, Tokens.Num(), Suffix, 8, AdopterPath, Leg.Name, &Out);
			if (Reference.Num() == 0)
			{
				Reference = Out;
			}
			else
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s: the adopter's tokens equal the first leg's"), Leg.Name), Out, Reference);
			}
			bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the load generations end"), Leg.Name), WaitAllEnded(Gpu, Load));
		for (const FSuperSLMLifecycleOpHandle& Handle : LoadHandles)
		{
			Gpu.ReleaseLifecycleOpHandle(Handle);
		}
		for (const FSuperSLMGpuSequence& Seq : Load)
		{
			Gpu.ReturnSequence(Seq);
		}
		DrainQuiet(Gpu);
	}
	return bOk;
}

// Cell 3.4 (audit G13; reading R10): with the build seat plus BlockCount = 3 OneCall sequences
// generating at K = MinimumK = BlockCount + 1, over the build's window every sequence starts a token
// at least once in every K consecutive ticks, and every build turn follows the last within K ticks:
// none starves. The build seat's cadence is checked again with every sequence Composed. Sequence
// starts come from the plan log (FSuperSLMGpuTestAccess::GetPlanLog), build turns from the seam.
// Mutants: the build seat outside the rotation, or behind every sequence (its gaps exceed K); a
// sequence starved while the build runs (no start in the window). MinimumK's one-call term itself is
// pinned by SuperSLM.Paging.Gpu.Configure.KFloorPlusOne and read again here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuBuildCadenceTest, "SuperSLM.Paging.Gpu.Prefix.BuildCadence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuBuildCadenceTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens, Prompt;
	if (!MakeTokens(*this, *Rig.Model, 128, 31, Tokens) || !MakeTokens(*this, *Rig.Model, 8, 32, Prompt))
	{
		return false;
	}
	constexpr int32 Slots = 3;
	bool bOk = true;
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		const bool bOneCall = Path == ESuperSLMGpuDecodePath::OneCall;
		const TCHAR* Leg = bOneCall ? TEXT("OneCall") : TEXT("Composed");
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, Slots, 512, 1, 0);
		if (!ConfigurePooled(*this, Rig, C))
		{
			return false;
		}
		const int64 K = Gpu.GetConfiguredK();
		bOk &= TestEqual(*FString::Printf(TEXT("%s: MinimumK is BlockCount + 1 with the build seat"), Leg), int64(Gpu.GetConfiguredMinimumK()), int64(Slots + 1));
		if (!TestTrue(TEXT("the plan log turns on"), FSuperSLMGpuTestAccess::SetEventLogEnabled(Gpu, true)))
		{
			return false;
		}
		TArray<FSuperSLMGpuSequence> Seqs;
		TArray<FSuperSLMLifecycleOpHandle> Handles;
		for (int32 I = 0; I < Slots; ++I)
		{
			FSuperSLMGpuSequence Seq;
			if (!Vend(*this, Gpu, Seq, Path))
			{
				return false;
			}
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Prompt;
			Request.MaxNewTokens = 400;
			Handles.Add(Gpu.RequestBeginGeneration(Seq, Request));
			Seqs.Add(Seq);
		}
		TickN(*this, Gpu, 4, TEXT("the sequences start"));
		FSuperSLMPrefix Prefix;
		const bool bBuilt = BuildPrefix(*this, Gpu, Tokens, Prefix);
		FSuperSLMGpuTestAccess::SetEventLogEnabled(Gpu, false);
		TArray<int64> Turns;
		bOk &= bBuilt && TestTrue(TEXT("the build's turn ticks are readable"), FSuperSLMPagedGpuTestSeam::GetPrefixBuildTurnTicks(Gpu, Prefix, Turns));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: one build turn per token"), Leg), Turns.Num(), Tokens.Num());
		if (Turns.Num() >= 2)
		{
			int64 MaxGap = 0;
			for (int32 I = 1; I < Turns.Num(); ++I)
			{
				MaxGap = FMath::Max(MaxGap, Turns[I] - Turns[I - 1]);
			}
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the build seat's largest gap %lld <= K = %lld"), Leg, MaxGap, K), MaxGap <= K);
			if (bOneCall)
			{
				const TArray<FSuperSLMGpuPlanLogEntry> Plan = FSuperSLMGpuTestAccess::GetPlanLog(Gpu);
				for (const FSuperSLMGpuSequence& Seq : Seqs)
				{
					int64 Last = Turns[0];
					int64 SeqMaxGap = 0;
					int32 Starts = 0;
					for (const FSuperSLMGpuPlanLogEntry& Entry : Plan)
					{
						if (Entry.Tick < Turns[0] || Entry.Tick > Turns.Last() || !Entry.StartedSequenceIds.Contains(Seq.Id))
						{
							continue;
						}
						SeqMaxGap = FMath::Max(SeqMaxGap, Entry.Tick - Last);
						Last = Entry.Tick;
						++Starts;
					}
					SeqMaxGap = FMath::Max(SeqMaxGap, Turns.Last() - Last);
					bOk &= TestTrue(*FString::Printf(TEXT("sequence %lld starts during the build (none starves)"), Seq.Id), Starts > 0);
					bOk &= TestTrue(*FString::Printf(TEXT("sequence %lld: largest gap %lld <= K = %lld"), Seq.Id, SeqMaxGap, K), SeqMaxGap <= K);
				}
			}
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the generations end"), Leg), WaitAllEnded(Gpu, Seqs));
		for (const FSuperSLMLifecycleOpHandle& Handle : Handles)
		{
			Gpu.ReleaseLifecycleOpHandle(Handle);
		}
		for (const FSuperSLMGpuSequence& Seq : Seqs)
		{
			Gpu.ReturnSequence(Seq);
		}
		DrainQuiet(Gpu);
		if (bBuilt)
		{
			bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
		}
	}
	return bOk;
}

// Cell 8.2, the schema legs (plan review F12; K0; readings R8 and the schema overload in the K5
// record): a prefix built with sslm_gpu_prefix_set_schema (CreatePrefix's schema overload) from
// potion_shop_order content that a schema-bound generation produced, fed as SchemaContent. A
// sequence bound to potion_shop_order adopts it (Success), and SetSchema() after that adopt is refused
// by name; the adopter's generation is accepted with an empty prompt (armed). A sequence bound to
// prompt_result adopts it and is refused for the schema mismatch, which maps by name (Malformed, the
// CPU twin's outcome, with SSLM_GPU_PREFIX_SCHEMA_MISMATCH named); SetSchema() after that refused adopt
// is allowed (the GPU revoked nothing). The CPU's SetSchema leg is SuperSLM.Paging.Cpu.Limit.
// SetSchemaAfterAdopt (K1).
// Mutants: SetSchema allowed after a successful adopt (the engine's revoked bind); the mismatch read as
// the default arm's unnamed result; the CPU's revoke-on-refusal rule applied to the GPU.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuSchemaBoundTest, "SuperSLM.Paging.Gpu.Prefix.SchemaBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuSchemaBoundTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	FSuperSLMGpuSchemaHandle Potion, PromptResult;
	FString LookupError;
	if (!TestTrue(*FString::Printf(TEXT("potion_shop_order resolves (%s)"), *LookupError), FSuperSLMGpuSchemaLookup::LookupByName(*Rig.Model, TEXT("potion_shop_order"), Potion, LookupError))
		|| !TestTrue(*FString::Printf(TEXT("prompt_result resolves (%s)"), *LookupError), FSuperSLMGpuSchemaLookup::LookupByName(*Rig.Model, TEXT("prompt_result"), PromptResult, LookupError)))
	{
		return false;
	}
	// Schema content: the first tokens a potion_shop_order-bound generation produces are valid from the
	// schema walk's start.
	TArray<int32> Prompt;
	FSuperSLMGpuSequence Source;
	FString BindError;
	if (!MakeTokens(*this, *Rig.Model, 8, 41, Prompt) || !Vend(*this, Gpu, Source)
		|| !TestTrue(*FString::Printf(TEXT("SetSchema(potion_shop_order) on the source (%s)"), *BindError), Gpu.SetSchema(Source, Potion, BindError)))
	{
		return false;
	}
	FGpuGeneration SourceRun;
	if (!Generate(*this, Gpu, Source, Prompt, 12, SourceRun) || !TestFalse(*FString::Printf(TEXT("the source generation is accepted (%s)"), *SourceRun.Error), SourceRun.bRefusedAtCall))
	{
		return false;
	}
	ReturnAndDrain(Gpu, Source);
	const int32 ContentCount = FMath::Min(SourceRun.Tokens.Num(), 6);
	if (!TestTrue(TEXT("the source produced schema content"), ContentCount >= 2))
	{
		return false;
	}
	const TArray<int32> Content(SourceRun.Tokens.GetData(), ContentCount);
	FSuperSLMPrefix SchemaPrefix;
	FString PrefixError;
	if (!TestEqual(*FString::Printf(TEXT("CreatePrefix(content, potion_shop_order, SchemaContent) is admitted (%s)"), *PrefixError),
			(uint8)Gpu.CreatePrefix(Content, Potion, ESuperSLMSpanKind::SchemaContent, SchemaPrefix, &PrefixError), (uint8)ESuperSLMPrefixResult::Success)
		|| !WaitPrefixReady(*this, Gpu, SchemaPrefix))
	{
		return false;
	}
	bool bOk = true;
	{
		FSuperSLMGpuSequence Seq;
		FString Error;
		if (!Vend(*this, Gpu, Seq) || !TestTrue(*FString::Printf(TEXT("SetSchema(potion_shop_order) (%s)"), *Error), Gpu.SetSchema(Seq, Potion, Error)))
		{
			return false;
		}
		bOk &= AdoptAndWait(*this, Gpu, Seq, SchemaPrefix);
		FString AfterError;
		bOk &= TestFalse(TEXT("SetSchema() after a successful adopt is refused"), Gpu.SetSchema(Seq, Potion, AfterError));
		bOk &= TestFalse(*FString::Printf(TEXT("the refusal is named (%s)"), *AfterError), AfterError.IsEmpty());
		FGpuGeneration Run;
		bOk &= Generate(*this, Gpu, Seq, TArray<int32>(), 4, Run);
		bOk &= TestFalse(*FString::Printf(TEXT("the schema-bound adopter's generation is accepted with an empty prompt (%s)"), *Run.Error), Run.bRefusedAtCall);
		bOk &= TestTrue(TEXT("the schema-bound adopter produces at least one token"), Run.Tokens.Num() >= 1);
		ReturnAndDrain(Gpu, Seq);
	}
	{
		FSuperSLMGpuSequence Seq;
		FString Error;
		if (!Vend(*this, Gpu, Seq) || !TestTrue(*FString::Printf(TEXT("SetSchema(prompt_result) (%s)"), *Error), Gpu.SetSchema(Seq, PromptResult, Error)))
		{
			return false;
		}
		FString AdoptError;
		const ESuperSLMRestoreResult Result = AdoptResult(Gpu, Seq, SchemaPrefix, &AdoptError);
		bOk &= TestEqual(*FString::Printf(TEXT("a schema-mismatched adopt resolves Malformed (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Result), *AdoptError),
			(uint8)Result, (uint8)ESuperSLMRestoreResult::Malformed);
		bOk &= TestTrue(*FString::Printf(TEXT("the error names SSLM_GPU_PREFIX_SCHEMA_MISMATCH (%s)"), *AdoptError), AdoptError.Contains(TEXT("SSLM_GPU_PREFIX_SCHEMA_MISMATCH")));
		FString AfterError;
		bOk &= TestTrue(*FString::Printf(TEXT("SetSchema() after a refused GPU adopt is allowed (%s)"), *AfterError), Gpu.SetSchema(Seq, PromptResult, AfterError));
		ReturnAndDrain(Gpu, Seq);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, SchemaPrefix);
	return bOk;
}

// Cell 8.2, the adapter leg: at the 1.5B artifact (the adapter's base; ContextCap 4,096), a sequence
// bound to the shopkeeper adapter (MapAdapter, RequestAdapterSwap) adopts a GPU prefix and generates,
// graded by the position oracle. potion_shop_order is looked up on the 1.5B model at run time (the K5
// record, reading R13; the coverage audit, S7). When it resolves, the prefix is schema-built as in
// SchemaBound (CreatePrefix's schema overload over SchemaContent a potion_shop_order-bound source
// generated), the adopter is bound to the schema and the adapter before its adopt, and it generates
// with an empty prompt. When it does not resolve, that is logged and the prefix is a plain 1,000-token
// one with a 4-token suffix.
// Mutants: an adopt that drops or ignores the adapter bind (the adopt is refused, or the position is
// off); with the schema, an adopt that drops the schema bind (the empty-prompt generation is refused).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapAdapterBoundTest, "SuperSLM.Paging.Gpu.Prefix.TwoCap.AdapterBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapAdapterBoundTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FString AdapterPath = SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-shopkeeper-lora-v2-t2102-runtime.sslm"));
	FSuperSLMGpuAdapterHandle Adapter;
	FString MapError;
	if (!TestTrue(*FString::Printf(TEXT("MapAdapter (%s)"), *MapError), Gpu.MapAdapter(AdapterPath, *Rig.Model, Adapter, MapError)))
	{
		return false;
	}
	FSuperSLMGpuSchemaHandle Potion;
	FString LookupError;
	const bool bSchema = FSuperSLMGpuSchemaLookup::LookupByName(*Rig.Model, TEXT("potion_shop_order"), Potion, LookupError);
	TArray<int32> Tokens, Suffix;
	FSuperSLMPrefix Prefix;
	if (bSchema)
	{
		AddInfo(TEXT("potion_shop_order resolves on the 1.5B model: the adapter leg adopts a schema-built prefix"));
		TArray<int32> Prompt;
		FSuperSLMGpuSequence Source;
		FString BindError;
		if (!MakeTokens(*this, *Rig.Model, 8, 53, Prompt) || !Vend(*this, Gpu, Source)
			|| !TestTrue(*FString::Printf(TEXT("SetSchema(potion_shop_order) on the source (%s)"), *BindError), Gpu.SetSchema(Source, Potion, BindError)))
		{
			return false;
		}
		FGpuGeneration SourceRun;
		if (!Generate(*this, Gpu, Source, Prompt, 12, SourceRun) || !TestFalse(*FString::Printf(TEXT("the source generation is accepted (%s)"), *SourceRun.Error), SourceRun.bRefusedAtCall))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Source);
		const int32 ContentCount = FMath::Min(SourceRun.Tokens.Num(), 6);
		if (!TestTrue(TEXT("the source produced schema content"), ContentCount >= 2))
		{
			return false;
		}
		Tokens = TArray<int32>(SourceRun.Tokens.GetData(), ContentCount);
		FString PrefixError;
		if (!TestEqual(*FString::Printf(TEXT("CreatePrefix(content, potion_shop_order, SchemaContent) is admitted (%s)"), *PrefixError),
				(uint8)Gpu.CreatePrefix(Tokens, Potion, ESuperSLMSpanKind::SchemaContent, Prefix, &PrefixError), (uint8)ESuperSLMPrefixResult::Success)
			|| !WaitPrefixReady(*this, Gpu, Prefix))
		{
			return false;
		}
	}
	else
	{
		AddInfo(FString::Printf(TEXT("potion_shop_order does not resolve on the 1.5B model (%s): the adapter leg adopts a plain prefix"), *LookupError));
		if (!MakeTokens(*this, *Rig.Model, 1000, 51, Tokens) || !MakeTokens(*this, *Rig.Model, 4, 52, Suffix) || !BuildPrefix(*this, Gpu, Tokens, Prefix))
		{
			return false;
		}
	}
	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}
	// GetActiveAdapter confirms a swap only after a real token result. An Idle
	// sequence has no token work to carry the request; warm it before resetting
	// to an empty holder, preserving its binding for the original adopt.
	Gpu.RequestAdapterSwap(Seq, Adapter);
	TArray<int32> BindPrompt, BindOutput;
	if (!MakeTokens(*this, *Rig.Model, 8, 54, BindPrompt)
		|| !GenerateComplete(*this, Gpu, Seq, BindPrompt, 1, BindOutput)
		|| !TestEqual(TEXT("a completed warm token confirms the requested adapter"), Gpu.GetActiveAdapter(Seq).Id, Adapter.Id)
		|| !ResetAndWait(*this, Gpu, Seq))
	{
		return false;
	}
	if (bSchema)
	{
		FString BindError;
		if (!TestTrue(*FString::Printf(TEXT("SetSchema(potion_shop_order) on the adopter, before its adopt (%s)"), *BindError), Gpu.SetSchema(Seq, Potion, BindError)))
		{
			return false;
		}
	}
	bool bOk = TestEqual(TEXT("the sequence is adapter-bound"), Gpu.GetActiveAdapter(Seq).Id, Adapter.Id);
	bOk &= AdoptAndWait(*this, Gpu, Seq, Prefix);
	bOk &= TestEqual(TEXT("the successful adopt preserves the confirmed adapter"), Gpu.GetActiveAdapter(Seq).Id, Adapter.Id);
	TArray<int32> Out;
	bOk &= GenerateComplete(*this, Gpu, Seq, Suffix, 3, Out);
	bOk &= PositionOracle(*this, Gpu, Seq, int64(Tokens.Num()) + Suffix.Num() + 3 - 1, bSchema ? TEXT("schema- and adapter-bound adopter") : TEXT("adapter-bound adopter"));
	ReturnAndDrain(Gpu, Seq);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	Gpu.UnmapAdapter(Adapter);
	return bOk;
}

// Cell 10.3 on the GPU (audit G8): a nested child of 20 tokens on a 1,000-token parent. The child holds
// only its private pages, ceil(1,020/16) - floor(1,000/16) = 2, by the exact two-sided probe (drawn
// 33 + 63 + 2 = 98 at (1 slot, budget 512, 2 prefix blocks, H 0)) and by the ledger's settle. The
// child's adopter is armed and passes 10.4's oracle: context_length 1,020 + 4 + 3 - 1 = 1,026.
// Mutants: a child that copies its parent's full pages (drawn > 98); the child's armed bit taken from
// the parent rather than its own last write (a no-op here, both are armed; 10.6 covers the empty child).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuNestedTest, "SuperSLM.Paging.Gpu.Prefix.Nested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuNestedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 2, 0);
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuGeometry& G = Rig.Geo;
	TArray<int32> ParentTokens, ChildTokens, Suffix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 61, ParentTokens) || !MakeTokens(*this, *Rig.Model, 20, 62, ChildTokens) || !MakeTokens(*this, *Rig.Model, 4, 63, Suffix))
	{
		return false;
	}
	FSuperSLMPrefix Parent, Child;
	auto Build = [&]() { return BuildPrefix(*this, Gpu, ParentTokens, Parent) && BuildNested(*this, Gpu, Parent, ChildTokens, Child); };
	if (!Build())
	{
		return false;
	}
	const int64 Private = G.NestedPrivate(1000, 20);
	bool bOk = TestEqual(TEXT("the child's private pages are 2"), Private, int64(2));
	bOk &= TestEqual(TEXT("the ledger settles the child to its private pages"), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Child), Private);
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("parent and child frozen"));
	const int64 Drawn = G.R(512) + G.OccupiedPages(1000) + Private;
	bOk &= TestEqual(TEXT("drawn is 98"), Drawn, int64(98));
	bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), Drawn, [&]() { return ConfigurePooled(*this, Rig, C) && Build(); }, TEXT("the child holds only its private pages"));
	bOk &= AdoptGenerateAndGrade(*this, Gpu, Child, 1020, Suffix, 3, ESuperSLMGpuDecodePath::OneCall, TEXT("the child's adopter"));
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Child);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Parent);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 10.3"));
	return bOk;
}

// §5, KvPagesExhausted for a prefix and a nested child (UP-6): P is short while the count admits. At
// (2 slots, budget 512, 2 prefix blocks, H 0), P = 512 holds 256 pages of ghost and a live 2,992-token
// parent (187 pages), so P has 69 free and one prefix is live (of 2). The ghost pages are construction
// (a) of the K5 coverage audit's B2 fix: one ghost of 4,096 tokens (256 pages). If the engine refuses
// to adopt a 4,096-token prefix, the test falls back to construction (b), a 4,080-token ghost (255
// pages) plus a 16-token ghost (1 page) on the second slot, and says which it used.
//   - CreatePrefix(1,100 tokens), charged R = 70, is refused KvPagesExhausted by name.
//   - CreatePrefixFrom(parent, 1,100 tokens), charged R(1,100) = 70 (parent + child = 4,092, within
//     ContextCap), is refused KvPagesExhausted by name.
//   - Each refusal charges nothing on P and creates no prefix.
//   - The boundary legs at exactly 69: a nested child of 1,088 tokens (R = 69; parent + child = 4,080)
//     and a 1,088-token root are each admitted, charged 69 at admission, built and released.
// Mutants: admission by count alone (both refused requests admitted; the engine exhausts); the nested
// charge taken as its private pages, ceil(4,092/16) - floor(2,992/16) = 256 - 187 = 69 (the 1,100-token
// child admitted); a charge one page high (the boundary legs refused).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPagesExhaustedTest, "SuperSLM.Paging.Gpu.Prefix.PagesExhausted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuPagesExhaustedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 2, 0)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Ghost4096, Ghost4080, Ghost16, ParentTokens, RootTokens, ChildTokens, FitChildTokens, FitRootTokens;
	if (!MakeTokens(*this, *Rig.Model, 4096, 71, Ghost4096) || !MakeTokens(*this, *Rig.Model, 4080, 76, Ghost4080) || !MakeTokens(*this, *Rig.Model, 16, 77, Ghost16)
		|| !MakeTokens(*this, *Rig.Model, 2992, 72, ParentTokens) || !MakeTokens(*this, *Rig.Model, 1100, 73, RootTokens) || !MakeTokens(*this, *Rig.Model, 1100, 74, ChildTokens)
		|| !MakeTokens(*this, *Rig.Model, 1088, 78, FitChildTokens) || !MakeTokens(*this, *Rig.Model, 1088, 75, FitRootTokens))
	{
		return false;
	}

	// The ghost pages: construction (a), else (b).
	TArray<FSuperSLMGpuSequence> GhostAdopters;
	{
		FSuperSLMPrefix Big;
		FSuperSLMGpuSequence Adopter;
		if (!BuildPrefix(*this, Gpu, Ghost4096, Big) || !Vend(*this, Gpu, Adopter))
		{
			return false;
		}
		FString AdoptError;
		if (AdoptResult(Gpu, Adopter, Big, &AdoptError) == ESuperSLMRestoreResult::Success)
		{
			AddInfo(TEXT("construction (a): one 4,096-token ghost"));
			if (!ReleasePrefixAndDrain(*this, Gpu, Big) || !TestTrue(TEXT("the 4,096-token prefix is a ghost"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Big)))
			{
				return false;
			}
			GhostAdopters.Add(Adopter);
		}
		else
		{
			AddInfo(FString::Printf(TEXT("construction (b): the engine did not adopt a 4,096-token prefix (%s); a 4,080-token ghost and a 16-token ghost"), *AdoptError));
			ReturnAndDrain(Gpu, Adopter);
			FSuperSLMPrefix G1, G2;
			FSuperSLMGpuSequence A1, A2;
			if (!ReleasePrefixAndDrain(*this, Gpu, Big) || !MakeGhost(*this, Gpu, Ghost4080, G1, A1) || !MakeGhost(*this, Gpu, Ghost16, G2, A2))
			{
				return false;
			}
			GhostAdopters.Add(A1);
			GhostAdopters.Add(A2);
		}
	}
	FSuperSLMPrefix Parent;
	if (!BuildPrefix(*this, Gpu, ParentTokens, Parent))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P holds 256 ghost pages and the parent's 187: 69 free"), Free(Gpu, ERegion::P), int64(69));
	bOk &= TestEqual(TEXT("one prefix is live (the count admits)"), Gpu.GetLivePrefixCount(), 1);
	bOk &= TestEqual(TEXT("R(1,100) = 70"), Rig.Geo.PrefixCharge(1100), int64(70));
	bOk &= TestEqual(TEXT("R(1,088) = 69"), Rig.Geo.PrefixCharge(1088), int64(69));
	bOk &= TestEqual(TEXT("the private-pages mutant's charge for the 1,100-token child is 69"), Rig.Geo.NestedPrivate(2992, 1100), int64(69));
	auto ExpectExhausted = [&](ESuperSLMPrefixResult Result, const FSuperSLMPrefix& Out, int64 PBefore, const FString& Error, const TCHAR* What)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("%s: KvPagesExhausted (%s)"), What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::KvPagesExhausted);
		bOk &= TestFalse(*FString::Printf(TEXT("%s: no handle"), What), Out.IsValid());
		bOk &= TestEqual(*FString::Printf(TEXT("%s: nothing charged on P"), What), Charged(Gpu, ERegion::P), PBefore);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: no prefix created"), What), Gpu.GetLivePrefixCount(), 1);
	};
	auto ExpectAdmittedAt69 = [&](ESuperSLMPrefixResult Result, const FSuperSLMPrefix& Out, int64 PBefore, const FString& Error, const TCHAR* What)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("%s: admitted at exactly 69 free (%s)"), What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: charged 69 at admission"), What), Charged(Gpu, ERegion::P) - PBefore, int64(69));
		if (Out.IsValid())
		{
			bOk &= WaitPrefixReady(*this, Gpu, Out);
			bOk &= ReleasePrefixAndDrain(*this, Gpu, Out);
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: released, P has 69 free again"), What), Free(Gpu, ERegion::P), int64(69));
	};
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(RootTokens, Out, &Error);
		ExpectExhausted(Result, Out, PBefore, Error, TEXT("a 1,100-token prefix (R = 70)"));
	}
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefixFrom(Parent, ChildTokens, Out, &Error);
		ExpectExhausted(Result, Out, PBefore, Error, TEXT("a 1,100-token nested child (R = 70)"));
	}
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefixFrom(Parent, FitChildTokens, Out, &Error);
		ExpectAdmittedAt69(Result, Out, PBefore, Error, TEXT("a 1,088-token nested child (R = 69)"));
	}
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		FSuperSLMPrefix Out;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(FitRootTokens, Out, &Error);
		ExpectAdmittedAt69(Result, Out, PBefore, Error, TEXT("a 1,088-token prefix (R = 69)"));
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("§5 KvPagesExhausted"));
	for (const FSuperSLMGpuSequence& Adopter : GhostAdopters)
	{
		bOk &= ReturnAndDrain(Gpu, Adopter);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Parent);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
