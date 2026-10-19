// Plugin paging plan, GPU red suite (step K5): the page ledger's invariants on the GPU (§3.2, §3.4,
// §3.9; §7 cells 1.3, 1.9, 1.11, 3.5, 11.2).
//
//   SuperSLM.Paging.Gpu.Ledger.Lifetime                          cell 1.9: the twin of 1.1          [K6/K7] (Stress)
//   SuperSLM.Paging.Gpu.Ledger.ZeroSlack                         cell 1.9: the twin of 1.2 (form 3) [K6/K7] (Stress)
//   SuperSLM.Paging.Gpu.Ledger.TwoCap.ZeroSlack                  cell 1.2 [two-cap] legs (form 3)   [K6/K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Ledger.Boundary.HeadroomHeldAfterApply   cell 1.3 row 7, GPU figures (form 2) [K7]
//   SuperSLM.Paging.Gpu.Ledger.TwoCap.Boundary.RestoreWithoutE   cell 1.3 [two-cap] E row (form 2)  [K7]  (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Ledger.TwoCap.Boundary.PrefixR           cell 1.3 [two-cap] prefix row      [K7]  (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Ledger.InFlightAfterDraw                 cell 1.9: the twin of 1.4 (form 1) [K7]
//   SuperSLM.Paging.Gpu.Ledger.FailureRefund                     cell 1.9: the twin of 1.6, GPU cases [K6/K7]
//   SuperSLM.Paging.Gpu.Ledger.FaultDetector                     cell 11.2, the GPU twin            [K6/K7]
//   SuperSLM.Paging.Gpu.Ledger.OneJobPerSlot                     cell 3.5 on the GPU (the K5 reading) [K6/K7]
//   SuperSLM.Paging.Gpu.Ledger.TeardownReconfigure               cell 1.11, the GPU's states        [K6/K7]
//   SuperSLM.Paging.Gpu.Ledger.RecycleTwiceInWindow              K6 code review C1: two recycles in one window [K6]
//
// Every 0.5B construction is computed for B = 16 and a block of 256 pages (R(512) = 33, the executed
// restore charge 95); every [two-cap] one for B = 16, model cap 32,768, ContextCap 4,096
// (P_blk = 257, H_blk = 513). Each test checks its shape before it counts. Ghosts and prefixes whose
// pages a probe counts are page-aligned lengths (multiples of 16), so a ghost's tail-page overcount
// never enters an exact count. While pooled mode is unbuilt every test fails at its first
// ConfigurePooled() (directly, or inside MakeBlobAt()).
//
// "[K6/K7]": pooled restores of 'SLM6' are K7's content ("SLM6"), yet K6's own cells (1.9's 1.2 and
// 1.6 twins, 11.2's GPU twin) restore pooled blobs; those legs go green at K7 at the latest (the K5
// record, finding F2). Plan rev 1.9 ruling 4 retags the cells to match: 1.9 [K6/K7]; 11.2's GPU twin
// [K6/K7]; 1.11 [K3/K6/K7] and 3.5 [K2/K3/K6/K7], whose K2 and K3 legs are the CPU's (K1's tests).
// A test here carries its GPU legs' range, inside its cell's.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	bool ExpectPrefixRefused(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, ESuperSLMPrefixResult Expected, const TCHAR* What)
	{
		const int64 PBefore = Charged(Gpu, ERegion::P);
		const int32 LiveBefore = Gpu.GetLivePrefixCount();
		FSuperSLMPrefix Prefix;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(Tokens, Prefix, &Error);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: refused by name (%s)"), What, *Error), (uint8)Result, (uint8)Expected);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no handle"), What), Prefix.IsValid());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing charged on P"), What), Charged(Gpu, ERegion::P), PBefore);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no prefix created"), What), Gpu.GetLivePrefixCount(), LiveBefore);
		return bOk;
	}
}

// Cell 1.9, the GPU twin of 1.1: 1,000 cycles on every slot, vend -> adopt -> generate -> reset ->
// re-adopt -> return. At the end the probe shows only the warm reservations and the live prefix
// drawn (exact form), the ledger's total equals that count, and every slot's last-cycle tokens equal
// its first. The prefix is 208 tokens: 13 whole pages.
// Then the re-adopt dependency leg (K5 coverage audit S2): a sequence adopts the prefix, re-adopts a
// second 208-token prefix with no reset between, and the first prefix is released. With the old
// dependency dropped at the re-adopt, the released prefix is freed at once (not a ghost), P holds only
// the second prefix's 13 pages, and the first prefix has no dependents. After the sequence is returned
// and the second prefix released, P is 0, no ghost is left, and no dependent remains.
// Mutant: the previous dependency not dropped at re-adopt (the released first prefix stays a ghost:
// P holds 26 pages, not 13).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuLifetimeTest, "SuperSLM.Paging.Gpu.Ledger.Lifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuLifetimeTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 2, 1);
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
	TArray<int32> PrefixTokens, Suffix[2];
	FSuperSLMPrefix Prefix;
	auto Rebuild = [&]() { return ConfigurePooled(*this, Rig, C) && BuildPrefix(*this, Gpu, PrefixTokens, Prefix); };
	if (!MakeTokens(*this, *Rig.Model, 208, 0, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 6, 1, Suffix[0]) || !MakeTokens(*this, *Rig.Model, 6, 2, Suffix[1])
		|| !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	constexpr int32 Cycles = 1000;
	TArray<int32> First[2], Last[2];
	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		FSuperSLMGpuSequence Seq[2];
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			if (!Vend(*this, Gpu, Seq[Slot]) || !AdoptAndWait(*this, Gpu, Seq[Slot], Prefix))
			{
				return false;
			}
			TArray<int32> Tokens;
			if (!GenerateComplete(*this, Gpu, Seq[Slot], Suffix[Slot], 4, Tokens) || !ResetAndWait(*this, Gpu, Seq[Slot]) || !AdoptAndWait(*this, Gpu, Seq[Slot], Prefix))
			{
				return false;
			}
			(Cycle == 0 ? First[Slot] : Last[Slot]) = Tokens;
		}
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			ReturnAndDrain(Gpu, Seq[Slot]);
		}
	}
	bool bOk = true;
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: the last cycle's tokens equal the first's"), Slot), Last[Slot], First[Slot]);
	}
	const int64 Drawn = Want.S + Rig.Geo.OccupiedPages(208);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after 1,000 cycles"));
	bOk &= TestEqual(TEXT("the ledger's total equals the warm reservations plus the live prefix"), LedgerOf(Gpu).TotalCharged, Drawn);
	bOk &= ProbeDrawnExactly(*this, Gpu, Want.Pool(), Drawn, Rebuild, TEXT("after 1,000 cycles"));

	// The re-adopt dependency leg, on the rebuilt state (Prefix live, no dependents).
	const int64 Pages208 = Rig.Geo.OccupiedPages(208);
	bOk &= TestEqual(TEXT("re-adopt leg: the prefix starts with no dependents"), int64(FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Prefix)), int64(0));
	TArray<int32> SecondTokens;
	FSuperSLMPrefix Second;
	FSuperSLMGpuSequence Seq;
	if (!MakeTokens(*this, *Rig.Model, 208, 3, SecondTokens) || !BuildPrefix(*this, Gpu, SecondTokens, Second) || !Vend(*this, Gpu, Seq)
		|| !AdoptAndWait(*this, Gpu, Seq, Prefix) || !AdoptAndWait(*this, Gpu, Seq, Second))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("re-adopt leg: the first prefix lost its dependent at the re-adopt"), int64(FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Prefix)), int64(0));
	bOk &= TestEqual(TEXT("re-adopt leg: the second prefix has the one dependent"), int64(FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Second)), int64(1));
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	bOk &= TestFalse(TEXT("re-adopt leg: the released first prefix is not a ghost"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Prefix));
	bOk &= TestEqual(TEXT("re-adopt leg: no ghost is counted"), LedgerOf(Gpu).GhostCount, 0);
	bOk &= TestEqual(TEXT("re-adopt leg: P holds only the second prefix's 13 pages"), Charged(Gpu, ERegion::P), Pages208);
	bOk &= ReturnAndDrain(Gpu, Seq);
	bOk &= TestEqual(TEXT("re-adopt leg: the second prefix has no dependents after the return"), int64(FSuperSLMPagedGpuTestSeam::PrefixDependentCount(Gpu, Second)), int64(0));
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Second);
	bOk &= TestEqual(TEXT("re-adopt leg: P is 0 after both prefixes are released"), Charged(Gpu, ERegion::P), int64(0));
	bOk &= TestEqual(TEXT("re-adopt leg: no ghost is left"), LedgerOf(Gpu).GhostCount, 0);
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("re-adopt leg, drained"));
	return bOk;
}

// Cell 1.9, the GPU twin of 1.2 (form 3, zero slack): each GPU request kind runs where the region under
// test has exactly its charge free and every other region is drawn to its bound or sized 0, so a draw
// above the charge reaches the engine's KV_POOL_EXHAUSTED. Kinds: a prefix build, a nested build, an
// unhinted restore, a hinted restore that does not share ('SSB5' into budget mode has no GPU twin: an
// 'SLM5' into a pooled subsystem is KvModeMismatch, cell 9.6). Each is admitted, completes, and leaves
// LedgerFaults 0; after quiescence the probe equals the test's count exactly.
// Mutant: any kind drawing above its charge (the engine exhausts), which zero slack makes visible.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuZeroSlackTest, "SuperSLM.Paging.Gpu.Ledger.ZeroSlack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuZeroSlackTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	// The blob first: budget 4,096 at origin 0, charged R(4,096) = 256, a whole H block.
	TArray<uint8> Blob4096;
	if (!MakeBlobAt(*this, Rig, 4096, 0, 3, Blob4096) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	const FGpuGeometry& G = Rig.Geo;
	bool bOk = TestEqual(TEXT("the budget-4,096 restore is charged a whole block"), G.RestoreCharge(4096, 0), int64(256));

	// (a) Prefix build. P = 1 block: a ghost of 1,008 tokens (63 whole pages, held by the only slot's
	// adopter) leaves 193 free, exactly R(3,072) = 193. H = 0.
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 1, 512, 1, 0);
		TArray<int32> GhostTokens, Tokens;
		FSuperSLMPrefix Ghost, Built;
		FSuperSLMGpuSequence Adopter;
		auto Build = [&]() { return ConfigurePooled(*this, Rig, C) && MakeGhost(*this, Gpu, GhostTokens, Ghost, Adopter); };
		if (!MakeTokens(*this, *Rig.Model, 1008, 1, GhostTokens) || !MakeTokens(*this, *Rig.Model, 3072, 2, Tokens) || !Build())
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(a) P has exactly R(3,072) = 193 free"), Free(Gpu, ERegion::P), G.PrefixCharge(3072));
		bOk &= BuildPrefix(*this, Gpu, Tokens, Built);
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(a) prefix build under zero slack"));
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Built);
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), G.R(512) + 63, Build, TEXT("(a) after the build's release"));
	}

	// (b) Nested build. P = 2 blocks (512): a ghost of 4,080 tokens (255 whole pages) and a 1,008-token
	// parent (63) leave 194, exactly R(3,088) = 194; parent + child = 4,096 = ContextCap.
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 1, 512, 2, 0);
		TArray<int32> GhostTokens, ParentTokens, ChildTokens;
		FSuperSLMPrefix Ghost, Parent, Child;
		FSuperSLMGpuSequence Adopter;
		auto Build = [&]()
		{
			return ConfigurePooled(*this, Rig, C) && MakeGhost(*this, Gpu, GhostTokens, Ghost, Adopter) && BuildPrefix(*this, Gpu, ParentTokens, Parent);
		};
		if (!MakeTokens(*this, *Rig.Model, 4080, 3, GhostTokens) || !MakeTokens(*this, *Rig.Model, 1008, 4, ParentTokens) || !MakeTokens(*this, *Rig.Model, 3088, 5, ChildTokens)
			|| !Build())
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(b) P has exactly R(3,088) = 194 free"), Free(Gpu, ERegion::P), G.PrefixCharge(3088));
		bOk &= BuildNested(*this, Gpu, Parent, ChildTokens, Child);
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(b) nested build under zero slack"));
		bOk &= TestEqual(TEXT("(b) the child settles to its private pages, 193"), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Child), G.NestedPrivate(1008, 3088));
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Child);
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), G.R(512) + 255 + 63, Build, TEXT("(b) after the child's release"));
	}

	// (c) Unhinted restore. 2 slots (the restore takes the free one), P = 0, H = 1 block, exactly the
	// charge of 256. Restore-first: the new holder draws 256 while the free slot's 33 is still held,
	// which is the whole pool.
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 512, 0, 1);
		FSuperSLMGpuSequence Seq;
		auto Build = [&]() { return ConfigurePooled(*this, Rig, C) && ApplyRestore(*this, Gpu, Blob4096, Rig.Model, Seq); };
		if (!Build())
		{
			return false;
		}
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(c) unhinted restore under zero slack"));
		bOk &= TestEqual(TEXT("(c) H holds max(0, 256 - R(512)) = 223 after the apply"), Charged(Gpu, ERegion::H), G.HeadroomHold(256, 512));
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), G.R(512) + 256, Build, TEXT("(c) after the apply"));
	}

	// (d) Hinted restore that does not share. P = 1 block, drawn whole by a 4,096-token prefix (the hint,
	// whose length differs from the blob's origin 0); H = 1 block, exactly the charge.
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1);
		TArray<int32> HintTokens;
		FSuperSLMPrefix Hint;
		FSuperSLMGpuSequence Seq;
		auto Build = [&]()
		{
			return ConfigurePooled(*this, Rig, C) && BuildPrefix(*this, Gpu, HintTokens, Hint) && ApplyRestore(*this, Gpu, Blob4096, Rig.Model, Seq, Hint);
		};
		if (!MakeTokens(*this, *Rig.Model, 4096, 6, HintTokens) || !Build())
		{
			return false;
		}
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(d) hinted, non-sharing restore under zero slack"));
		bOk &= TestEqual(TEXT("(d) the hint did not share: H holds 223"), Charged(Gpu, ERegion::H), G.HeadroomHold(256, 512));
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), G.R(512) + 256 + 256, Build, TEXT("(d) after the apply"));
	}
	return bOk;
}

// Cell 1.2's [two-cap] legs (form 3; UP-15). (e) Warm creates at SequenceBudget = ContextCap: R is
// priced at the model's cap, 257 pages, not 256. With P = H = 0 the pool is S exactly, so a warm
// reservation priced one page short exhausts the engine's create at Configure; both slots vend and
// generate, with LedgerFaults 0. (e') The same S with one P block beside it, for the probe's room:
// exactly 514 drawn. (f) A prefix of ContextCap - 1 = 4,095 tokens (R = 257) into a P of one 257-page
// block with H = 0: admitted, with no exhaustion and no ledger fault (the 256-page mutant does not
// exhaust here: the engine draws 257 from the 257 free, so 1.3's [two-cap] prefix row kills it).
// Mutants: R priced at ContextCap (e: S = 512, the second warm create exhausts); blocks of
// ceil(ContextCap/B) = 256 (f: the 257-page build does not fit P).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapZeroSlackTest, "SuperSLM.Paging.Gpu.Ledger.TwoCap.ZeroSlack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapZeroSlackTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	bool bOk = true;
	// (e)
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 4096, 0, 0);
		if (!ConfigurePooled(*this, Rig, C) || !RequireTwoCapShape(*this, Rig))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(e) S = 2 x R(4,096) = 514: the pool is S exactly"), ExpectedRegions(Rig.Geo, C).Pool(), int64(514));
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(e) after the warm creates"));
		TArray<int32> Prompt;
		if (!MakeTokens(*this, *Rig.Model, 6, 1, Prompt))
		{
			return false;
		}
		FSuperSLMGpuSequence Seq[2];
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			TArray<int32> Tokens;
			bOk &= Vend(*this, Gpu, Seq[Slot]) && GenerateComplete(*this, Gpu, Seq[Slot], Prompt, 2, Tokens);
		}
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			ReturnAndDrain(Gpu, Seq[Slot]);
		}
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(e) after both slots generated"));
	}
	// (e')
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 4096, 1, 0);
		auto Build = [&]() { return ConfigurePooled(*this, Rig, C); };
		if (!Build())
		{
			return false;
		}
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(Rig.Geo, C).Pool(), 514, Build, TEXT("(e') warm reservations at R(4,096) = 257 each"));
	}
	// (f)
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 1, 512, 1, 0);
		TArray<int32> Tokens;
		FSuperSLMPrefix Prefix;
		if (!ConfigurePooled(*this, Rig, C) || !MakeTokens(*this, *Rig.Model, 4095, 2, Tokens))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(f) P has exactly R(4,095) = 257 free"), Free(Gpu, ERegion::P), Rig.Geo.PrefixCharge(4095));
		bOk &= BuildPrefix(*this, Gpu, Tokens, Prefix);
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(f) the 4,095-token build under zero slack"));
		bOk &= TestEqual(TEXT("(f) the prefix settles to ceil(4,095/16) = 256"), FSuperSLMPagedGpuTestSeam::PrefixChargedPages(Gpu, Prefix), Rig.Geo.OccupiedPages(4095));
	}
	return bOk;
}

// Cell 1.3, row 7, at the GPU's figures (form 2): H refunded at apply for a private restore. One applied
// private holder at budget 512, origin 3,120 (charge 33 + 195 = 228, net hold 195) leaves H 61 free =
// 62 - 1: a restore charged 62 (origin 464: 33 + 29) is refused on arrival. On a rebuild with the holder
// at origin 3,104 (hold 194), H has exactly 62 free and the same restore is admitted. Form 1 holds after
// each apply.
// Mutant: H refunded at the apply (H free 256 after it), which admits the 62 in the first leg.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuBoundaryHeadroomHeldTest, "SuperSLM.Paging.Gpu.Ledger.Boundary.HeadroomHeldAfterApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuBoundaryHeadroomHeldTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B3120, B3104, B464;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 3120, 1, B3120) || !RequireAExShape(*this, Rig) || !MakeBlobAt(*this, Rig, 512, 3104, 2, B3104)
		|| !MakeBlobAt(*this, Rig, 512, 464, 3, B464))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuGeometry& G = Rig.Geo;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 3, 512, 0, 1);
	bool bOk = TestEqual(TEXT("the probe restore is charged 62"), G.RestoreCharge(512, 464), int64(62));
	bOk &= TestEqual(TEXT("the origin-3,120 holder's net hold is 195"), G.HeadroomHold(G.RestoreCharge(512, 3120), 512), int64(195));
	FSuperSLMGpuSequence Holder;
	if (!ConfigurePooled(*this, Rig, C) || !ApplyRestore(*this, Gpu, B3120, Rig.Model, Holder))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("H has 61 free after the apply"), Free(Gpu, ERegion::H), int64(61));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the origin-3,120 apply"));
	{
		const FIssuedGpuRestore R = IssueRestore(Gpu, B464, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Gpu, R, Charged(Gpu, ERegion::H), TEXT("H free = 62 - 1"));
		Discard(Gpu, R);
	}
	FSuperSLMGpuSequence Holder2, Fits;
	if (!ConfigurePooled(*this, Rig, C) || !ApplyRestore(*this, Gpu, B3104, Rig.Model, Holder2))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("H has 62 free after the origin-3,104 apply"), Free(Gpu, ERegion::H), int64(62));
	bOk &= ApplyRestore(*this, Gpu, B464, Rig.Model, Fits);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("the exact-fit restore"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the exact-fit apply"));
	return bOk;
}

// Cell 1.3, the [two-cap] E row (form 2; coverage re-audit N-b): E priced at ContextCap. One applied
// private holder at budget 512, origin 3,696 (E = 231, charge 264) leaves H_blk 513 - 231 = 282 free;
// a restore at origin 4,000 (charge 33 + 250 = 283) is refused on arrival -- the holder is applied, so
// no in-flight guarantee lets it wait. On a rebuild with the holder at origin 3,680 (hold 230), H has
// exactly 283 free and the same restore is admitted and succeeds. Host leg
// cell_1_3_two_cap_restore_without_e_boundary.
// Mutant: E priced at ContextCap (E <= 256 - 33 = 223): the 4,000 is charged 256 and the holder holds
// 223, so the first leg admits it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapBoundaryRestoreTest, "SuperSLM.Paging.Gpu.Ledger.TwoCap.Boundary.RestoreWithoutE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapBoundaryRestoreTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B3696, B4000, B3680;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !MakeBlobAt(*this, Rig, 512, 3696, 1, B3696) || !RequireTwoCapShape(*this, Rig)
		|| !MakeBlobAt(*this, Rig, 512, 4000, 2, B4000) || !MakeBlobAt(*this, Rig, 512, 3680, 3, B3680))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuGeometry& G = Rig.Geo;
	bool bOk = TestEqual(TEXT("the origin-4,000 restore is charged 283 (E at the model's cap)"), G.RestoreCharge(512, 4000), int64(283));
	bOk &= TestEqual(TEXT("the origin-3,696 holder holds 231"), G.HeadroomHold(G.RestoreCharge(512, 3696), 512), int64(231));
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 3, 512, 0, 1);
	FSuperSLMGpuSequence Holder;
	if (!ConfigurePooled(*this, Rig, C) || !ApplyRestore(*this, Gpu, B3696, Rig.Model, Holder))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("H has 282 free"), Free(Gpu, ERegion::H), int64(282));
	{
		const FIssuedGpuRestore R = IssueRestore(Gpu, B4000, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Gpu, R, Charged(Gpu, ERegion::H), TEXT("[two-cap] H free = 283 - 1"));
		Discard(Gpu, R);
	}
	FSuperSLMGpuSequence Holder2, Fits;
	if (!ConfigurePooled(*this, Rig, C) || !ApplyRestore(*this, Gpu, B3680, Rig.Model, Holder2))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("H has 283 free"), Free(Gpu, ERegion::H), int64(283));
	bOk &= ApplyRestore(*this, Gpu, B4000, Rig.Model, Fits);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("[two-cap] the exact-fit restore"));
	return bOk;
}

// Cell 1.3, the [two-cap] prefix row (coverage re-audit R1-B1): prefix R priced at ContextCap. P is one
// 257-page block; a ghost of 16 tokens (one whole page, held by the only slot's adopter; a ghost frees
// its PrefixBlockCount place) leaves 256 free: a 4,095-token prefix (R = 257) is refused
// KvPagesExhausted, with nothing charged. Once the adopter is returned the ghost drains, P has exactly
// 257 free, and the same build is admitted. Host leg cell_1_3_two_cap_prefix_undercharge_boundary.
// Mutant: R priced at ContextCap (256), which admits the first leg.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapBoundaryPrefixTest, "SuperSLM.Paging.Gpu.Ledger.TwoCap.Boundary.PrefixR",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapBoundaryPrefixTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 1, 0);
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !ConfigurePooled(*this, Rig, C) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> GhostTokens, Tokens;
	FSuperSLMPrefix Ghost, Built;
	FSuperSLMGpuSequence Adopter;
	if (!MakeTokens(*this, *Rig.Model, 16, 1, GhostTokens) || !MakeTokens(*this, *Rig.Model, 4095, 2, Tokens) || !MakeGhost(*this, Gpu, GhostTokens, Ghost, Adopter))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P has 256 free"), Free(Gpu, ERegion::P), int64(256));
	bOk &= TestEqual(TEXT("R(4,095) = 257 at the model's cap"), Rig.Geo.PrefixCharge(4095), int64(257));
	bOk &= ExpectPrefixRefused(*this, Gpu, Tokens, ESuperSLMPrefixResult::KvPagesExhausted, TEXT("[two-cap] P free = 257 - 1"));
	bOk &= ReturnAndDrain(Gpu, Adopter);
	bOk &= TestEqual(TEXT("the ghost drained: P has 257 free"), Free(Gpu, ERegion::P), int64(257));
	bOk &= BuildPrefix(*this, Gpu, Tokens, Built);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("[two-cap] the exact-fit build"));
	return bOk;
}

// Cell 1.9, the GPU twin of 1.4 (form 1): the in-flight window after the draw. With HoldApply on each
// GPU drawing kind -- a prefix build, a nested build, a restore -- the probe's drawn pages are at most
// the ledger's charge while the job is held after its Layer-1 work, and again after it applies.
// Mutant: a charge settled (refunded) before the apply, which shows drawn > charge during the hold.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuInFlightAfterDrawTest, "SuperSLM.Paging.Gpu.Ledger.InFlightAfterDraw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuInFlightAfterDrawTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 2, 1);
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> ParentTokens, ChildTokens;
	if (!MakeTokens(*this, *Rig.Model, 200, 2, ParentTokens) || !MakeTokens(*this, *Rig.Model, 100, 3, ChildTokens))
	{
		return false;
	}
	bool bOk = true;
	FSuperSLMPrefix Parent, Child;
	{
		const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::PrefixBuild);
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("the build is admitted (%s)"), *Error), (uint8)Gpu.CreatePrefix(ParentTokens, Parent, &Error), (uint8)ESuperSLMPrefixResult::Success);
		if (!TestTrue(TEXT("the build's apply is held"), WaitHoldReached(Gpu, Hold)))
		{
			return false;
		}
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("a prefix build held at its apply"));
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		bOk &= WaitPrefixReady(*this, Gpu, Parent);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the build"));
	}
	{
		const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::NestedBuild);
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("the nested build is admitted (%s)"), *Error), (uint8)Gpu.CreatePrefixFrom(Parent, ChildTokens, Child, &Error), (uint8)ESuperSLMPrefixResult::Success);
		if (!TestTrue(TEXT("the nested build's apply is held"), WaitHoldReached(Gpu, Hold)))
		{
			return false;
		}
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("a nested build held at its apply"));
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		bOk &= WaitPrefixReady(*this, Gpu, Child);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the nested build"));
	}
	{
		const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedGpuRestore R = IssueRestore(Gpu, B1000, Rig.Model);
		if (!TestTrue(TEXT("the restore's apply is held"), WaitHoldReached(Gpu, Hold)))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("the held restore is charged R(512) + E = 95 on H"), Charged(Gpu, ERegion::H), Rig.Geo.RestoreCharge(512, 1000));
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("a restore held at its apply"));
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		FSuperSLMGpuSequence Seq;
		bOk &= TestEqual(TEXT("the restore succeeds"), (uint8)SettleOutcome(Gpu, R, Seq), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after the restore"));
		Discard(Gpu, R);
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("every drawing kind"));
	return bOk;
}

// Cell 1.9, the GPU twin of 1.6: post-admission failures refund exactly (form 1 and the exact probe:
// the ledger returns to its pre-admission value, equal to the probe). Cases:
//   (i)   engine-refused blobs: an 'SLM6' payload with one trailing byte, and one cut short past its
//         header, each resolving Malformed (§3.5 "Exact size"); each is shown refused after admission
//         (exactly one restore job posted), so the refund check is not vacuous;
//   (ii)  a failed begin_from (the Layer-1 status seam: SSLM_GPU_PREFIX_REJECTED). The plan names a
//         released parent as the cause; this is a status-override construction of the same engine
//         refusal on a live parent, chosen because it fails after admission deterministically (the
//         K5 coverage audit, N3'). The refused child reads Faulted (plan rev 1.9 ruling 7);
//   (iii) a HOLDER_MODE_MISMATCH forced by the seam, resolving KvModeMismatch in pooled mode
//         (coverage audit B2);
//   (iv)  an adopt faulted after the share (§3.4's seam: the real adopt, then an engine reset, then the
//         fault status): the slot reads reset (ContextUsed 0, armed false), form 1 holds while the
//         ledger may still carry the old dependency, and after the slot's next reset the probe equals
//         the ledger.
// Mutants: a failed restore that never refunds H; K0R's KvMismatch arm kept in pooled mode (iii);
// the adopt fault leaving the mirror armed or ContextUsed advanced (iv).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFailureRefundTest, "SuperSLM.Paging.Gpu.Ledger.FailureRefund",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuFailureRefundTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 2, 1);
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
	auto Rebuild = [&]() { return ConfigurePooled(*this, Rig, C); };
	bool bOk = true;

	// (i) Engine-refused blobs.
	TArray<uint8> Trailing = B1000, Short = B1000;
	bOk &= TestTrue(TEXT("(i) one trailing byte is appended"), FSuperSLMPagedTestSeam::EditPayload(Trailing, [](TArray<uint8>& P) { P.Add(0); }));
	bOk &= TestTrue(TEXT("(i) the body is cut short past the header"), FSuperSLMPagedTestSeam::EditPayload(Short, [](TArray<uint8>& P)
	{
		P.SetNum(FMath::Max(GpuPayloadHeader::Slm6HeaderEnd + 1, P.Num() - 64));
	}));
	for (const TArray<uint8>* Blob : { &Trailing, &Short })
	{
		const FSuperSLMPagedLedgerView Before = LedgerOf(Gpu);
		const int64 JobsBefore = FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore);
		FSuperSLMGpuSequence Seq;
		const ESuperSLMRestoreResult Result = RestoreAndWait(*this, Gpu, *Blob, Rig.Model, Seq);
		bOk &= TestEqual(*FString::Printf(TEXT("(i) the engine's refusal resolves Malformed (%s)"), *Gpu.GetLastLifecycleRequestError()), (uint8)Result, (uint8)ESuperSLMRestoreResult::Malformed);
		bOk &= TestEqual(TEXT("(i) the refusal came after admission: exactly one restore job was posted"),
			FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore), JobsBefore + 1);
		bOk &= TestFalse(TEXT("(i) no sequence"), Seq.IsValid());
		bOk &= TestEqual(TEXT("(i) the ledger returns to its pre-admission total"), LedgerOf(Gpu).TotalCharged, Before.TotalCharged);
		bOk &= NoLedgerFault(*this, Gpu, TEXT("(i)"));
		bOk &= ProbeDrawnExactly(*this, Gpu, Want.Pool(), Want.S, Rebuild, TEXT("(i) after the refusal"));
	}

	// (ii) A failed begin_from.
	{
		TArray<int32> ParentTokens, ChildTokens;
		FSuperSLMPrefix Parent, Child;
		if (!MakeTokens(*this, *Rig.Model, 208, 2, ParentTokens) || !MakeTokens(*this, *Rig.Model, 64, 3, ChildTokens) || !BuildPrefix(*this, Gpu, ParentTokens, Parent))
		{
			return false;
		}
		const FSuperSLMPagedLedgerView Before = LedgerOf(Gpu);
		bOk &= TestTrue(TEXT("(ii) the Layer-1 status seam is set"),
			FSuperSLMPagedGpuTestSeam::SetNextLayer1StatusOverride(Gpu, ESuperSLMPagedRequestKind::NestedBuild, SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED));
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("(ii) the child is admitted (%s)"), *Error), (uint8)Gpu.CreatePrefixFrom(Parent, ChildTokens, Child, &Error), (uint8)ESuperSLMPrefixResult::Success);
		auto IsSettledPhase = [&Gpu, &Child]()
		{
			const ESuperSLMPrefixPhase Phase = Gpu.GetPrefixPhase(Child);
			return Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid || Phase == ESuperSLMPrefixPhase::Ready;
		};
		Pump(Gpu, IsSettledPhase, OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("(ii) the refused child reads Faulted (rev 1.9 ruling 7; phase %d)"), (int32)Gpu.GetPrefixPhase(Child)),
			(uint8)Gpu.GetPrefixPhase(Child), (uint8)ESuperSLMPrefixPhase::Faulted);
		if (Gpu.GetPrefixPhase(Child) != ESuperSLMPrefixPhase::Invalid)
		{
			FString Ignored;
			Gpu.ReleasePrefix(Child, Ignored);
		}
		DrainQuiet(Gpu);
		bOk &= TestEqual(TEXT("(ii) P returns to its pre-admission value"), LedgerOf(Gpu).ChargedOf(ERegion::P), Before.ChargedOf(ERegion::P));
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("(ii) after the refusal"));
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Parent);
	}

	// (iii) HOLDER_MODE_MISMATCH in pooled mode.
	{
		const FSuperSLMPagedLedgerView Before = LedgerOf(Gpu);
		bOk &= TestTrue(TEXT("(iii) the Layer-1 status seam is set"),
			FSuperSLMPagedGpuTestSeam::SetNextLayer1StatusOverride(Gpu, ESuperSLMPagedRequestKind::Restore, SslmGpuStatus::SSLM_GPU_HOLDER_MODE_MISMATCH));
		FSuperSLMGpuSequence Seq;
		const ESuperSLMRestoreResult Result = RestoreAndWait(*this, Gpu, B1000, Rig.Model, Seq);
		bOk &= TestEqual(*FString::Printf(TEXT("(iii) HOLDER_MODE_MISMATCH resolves KvModeMismatch in pooled mode, not K0R's KvMismatch (got %s)"),
			SuperSLML2S2Fixtures::RestoreResultName(Result)), (uint8)Result, (uint8)ESuperSLMRestoreResult::KvModeMismatch);
		bOk &= TestEqual(TEXT("(iii) H returns to its pre-admission value"), LedgerOf(Gpu).ChargedOf(ERegion::H), Before.ChargedOf(ERegion::H));
		bOk &= ProbeDrawnExactly(*this, Gpu, Want.Pool(), Want.S, Rebuild, TEXT("(iii) after the refusal"));
	}

	// (iv) An adopt faulted after the share.
	{
		TArray<int32> Tokens;
		FSuperSLMPrefix Prefix;
		FSuperSLMGpuSequence Seq;
		if (!MakeTokens(*this, *Rig.Model, 208, 4, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Prefix) || !Vend(*this, Gpu, Seq))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("(iv) the fault-after-share seam is armed"), FSuperSLMPagedGpuTestSeam::ArmAdoptFaultAfterShare(Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED));
		FString Error;
		const ESuperSLMRestoreResult Result = AdoptResult(Gpu, Seq, Prefix, &Error);
		bOk &= TestTrue(*FString::Printf(TEXT("(iv) the adopt resolves the mapped fault, not Success (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Result), *Error),
			Result != ESuperSLMRestoreResult::Success && Result != ESuperSLMRestoreResult::Pending);
		bOk &= TestTrue(TEXT("(iv) the error names the status"), Error.Contains(TEXT("SSLM_GPU_ALLOCATION_FAILED")));
		bOk &= TestEqual(TEXT("(iv) the slot reads reset: ContextUsed 0"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(0));
		bOk &= TestEqual(TEXT("(iv) the slot reads reset: armed false"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
		bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("(iv) after the fault, before the slot's reset"));
		bOk &= ResetAndWait(*this, Gpu, Seq);
		bOk &= TestEqual(TEXT("(iv) after the reset no ghost is left"), LedgerOf(Gpu).GhostCount, 0);
		bOk &= TestEqual(TEXT("(iv) after the reset the ledger is the warm reservations alone"), LedgerOf(Gpu).TotalCharged, Want.S);
		bOk &= ProbeDrawnExactly(*this, Gpu, Want.Pool(), Want.S, Rebuild, TEXT("(iv) after the slot's reset"));
	}
	return bOk;
}

// Cell 11.2, the GPU twin (coverage audit B2): the ledger-fault detector, under zero slack with one
// ForceUnderCharge set. Two slots (66) and one H block (256): an applied holder at budget 512, origin
// 16 (hold 1) leaves H 255 free and the pool 255 pages physically free. A budget-4,096 restore
// (charge 256) under-charged by one page is admitted at 255; the engine draws 256 and returns
// KV_POOL_EXHAUSTED, the outcome is KvPagesExhausted, LedgerFaults rises by one in both readings, and
// a line containing "ledger fault" is logged (Development, where the seams exist).
// Mutants: K0R's OutOfMemory arm kept in pooled mode; the counter increment removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFaultDetectorTest, "SuperSLM.Paging.Gpu.Ledger.FaultDetector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuFaultDetectorTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B16, B4096;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 16, 1, B16) || !MakeBlobAt(*this, Rig, 4096, 0, 2, B4096)
		|| !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 0, 1)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	FSuperSLMGpuSequence Holder;
	if (!ApplyRestore(*this, Gpu, B16, Rig.Model, Holder))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("H has 255 free"), Free(Gpu, ERegion::H), int64(255));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("before the forced under-charge"));
	bOk &= TestTrue(TEXT("ForceUnderCharge(BudgetRestore, 1) is set"), FSuperSLMPagedGpuTestSeam::ForceUnderCharge(Gpu, ESuperSLMPagedChargeKind::BudgetRestore, 1));
	FGpuPagingLogCapture Log;
	AddExpectedError(TEXT("ledger fault"), EAutomationExpectedErrorFlags::Contains, 0);
	const FIssuedGpuRestore R = IssueRestore(Gpu, B4096, Rig.Model);
	FSuperSLMGpuSequence Seq;
	const ESuperSLMRestoreResult Result = SettleOutcome(Gpu, R, Seq);
	bOk &= TestTrue(TEXT("the under-charged restore is accepted"), R.Handle.IsValid());
	bOk &= TestEqual(*FString::Printf(TEXT("the engine's exhaustion resolves KvPagesExhausted in pooled mode, not K0R's OutOfMemory (got %s)"),
		SuperSLML2S2Fixtures::RestoreResultName(Result)), (uint8)Result, (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
	bOk &= TestEqual(TEXT("LedgerFaults rose by one (GetKvPageStats)"), int64(Gpu.GetKvPageStats().LedgerFaults), int64(1));
	bOk &= TestEqual(TEXT("LedgerFaults rose by one (Ledger seam)"), int64(LedgerOf(Gpu).LedgerFaults), int64(1));
	bOk &= TestTrue(TEXT("a line containing \"ledger fault\" is logged"), Log.CountContaining(TEXT("ledger fault")) > 0);
	FSuperSLMPagedGpuTestSeam::ForceUnderCharge(Gpu, ESuperSLMPagedChargeKind::BudgetRestore, 0);
	Discard(Gpu, R);
	return bOk;
}

// Cell 3.5 on the GPU (coverage audit G1; the K5 reading R6). A GPU restore is not a per-sequence
// request (it makes a new sequence, D-SLM7457), so the per-slot leg queues the requests the GPU slot
// does take, 4 deep, on a restored budget-64 holder: reset (a normalization: create-first, charged
// R(512) = 33 on H at admission), adopt, reset, adopt. Nothing is refused at queue time; while the first
// reset is held at its apply, H carries that one charge and nothing for the requests behind it; then
// all four resolve Success in order. The "refused at the head, not at queue time" leg runs on the
// subsystem's restore queue, which is FIFO in arrival order: three restores issued in one frame
// (charges 95, 256, 34) all queue; the 256 is refused KvPagesExhausted when it reaches the head
// (161 + 33 < 256), and the 34 behind it then runs.
// Mutants: admission at queue time (the second reset is charged, or the 256 is refused at the call);
// the queue narrowed to 1 (the second queued call is refused SequenceQueueFull).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuOneJobPerSlotTest, "SuperSLM.Paging.Gpu.Ledger.OneJobPerSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuOneJobPerSlotTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B64, B1000, B4096, B16;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 64, 0, 1, B64) || !MakeBlobAt(*this, Rig, 512, 1000, 2, B1000) || !MakeBlobAt(*this, Rig, 4096, 0, 3, B4096)
		|| !MakeBlobAt(*this, Rig, 512, 16, 4, B16))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 3, 512, 1, 1);
	if (!ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMGpuSequence Holder;
	if (!MakeTokens(*this, *Rig.Model, 64, 5, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Prefix) || !ApplyRestore(*this, Gpu, B64, Rig.Model, Holder))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the budget-64 holder holds nothing on H (draw 5 <= R(512))"), Charged(Gpu, ERegion::H), int64(0));
	const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Reset);
	FSuperSLMLifecycleOpHandle Ops[4];
	Ops[0] = Gpu.RequestResetSequence(Holder, ESuperSLMGpuDecodePath::OneCall);
	Ops[1] = Gpu.RequestAdoptPrefix(Holder, Prefix);
	Ops[2] = Gpu.RequestResetSequence(Holder, ESuperSLMGpuDecodePath::OneCall);
	Ops[3] = Gpu.RequestAdoptPrefix(Holder, Prefix);
	for (int32 I = 0; I < 4; ++I)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("queued request %d is accepted at the call (%s)"), I, *Gpu.GetLastLifecycleRequestError()), Ops[I].IsValid());
	}
	if (!TestTrue(TEXT("the first reset (a normalization) is held at its apply"), WaitHoldReached(Gpu, Hold)))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("H carries the head's create-first charge alone, R(512) = 33"), Charged(Gpu, ERegion::H), Rig.Geo.R(512));
	bOk &= TestTrue(TEXT("the slot has its one open job"), FSuperSLMPagedGpuTestSeam::SequenceHasOpenJob(Gpu, Holder));
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
	int64 LastOrdinal = -1;
	for (int32 I = 0; I < 4; ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("queued request %d succeeds"), I), (uint8)WaitOp(Gpu, Ops[I]), (uint8)ESuperSLMRestoreResult::Success);
		const int64 Ordinal = Gpu.GetLifecycleOpResolutionOrdinal(Ops[I]);
		bOk &= TestTrue(*FString::Printf(TEXT("queued request %d resolves after the one before it"), I), Ordinal > LastOrdinal);
		LastOrdinal = Ordinal;
		Gpu.ReleaseLifecycleOpHandle(Ops[I]);
	}
	bOk &= TestEqual(TEXT("after the normalization's apply H is free again"), Charged(Gpu, ERegion::H), int64(0));
	ReturnAndDrain(Gpu, Holder);

	// The subsystem's restore queue: refused at the head, then the request behind it runs.
	const FIssuedGpuRestore R95 = IssueRestore(Gpu, B1000, Rig.Model);
	const FIssuedGpuRestore R256 = IssueRestore(Gpu, B4096, Rig.Model);
	const FIssuedGpuRestore R34 = IssueRestore(Gpu, B16, Rig.Model);
	bOk &= TestTrue(TEXT("all three restores queue at the call"), R95.Handle.IsValid() && R256.Handle.IsValid() && R34.Handle.IsValid());
	FSuperSLMGpuSequence S95, S256, S34;
	bOk &= TestEqual(TEXT("the 95 succeeds"), (uint8)SettleOutcome(Gpu, R95, S95), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the 256 is refused KvPagesExhausted at the head"), (uint8)SettleOutcome(Gpu, R256, S256), (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
	bOk &= TestEqual(TEXT("the 34 behind it runs and succeeds"), (uint8)SettleOutcome(Gpu, R34, S34), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("the restore queue"));
	Discard(Gpu, R95);
	Discard(Gpu, R256);
	Discard(Gpu, R34);
	return bOk;
}

// Cell 1.11, the GPU's states (audit N18; rev 1.8 tombstones): one pooled epoch holds, at once, a prefix
// with a dependent; two restores in flight (held at their applies) and a third waiting on H (95 each:
// free 66 + guarantees 66 >= 95); a fourth queued in the same frame as the re-Configure; and a build
// mid-rotation. The re-Configure then:
//   - every ended restore's handle reads NotConfigured exactly (its tombstone), the waiting one with
//     nothing drawn; GetLifecycleHandleEntryCount() counts them; each reads Pending again only after
//     ReleaseLifecycleOpHandle() returns true (a released-entry Pending is not accepted, S1);
//   - the build's prefix reads Invalid (the prefix phase has no NotConfigured: the K5 reading R5) and
//     no prefix is live;
//   - the rebuilt pool shows only its warm reservations drawn (the exact probe): nothing leaked.
// A second leg rebuilds the state and tears down by the Deinitialize() path: the model unmaps and the
// context is destroyed with SSLM_OK (a live prefix or the pool left behind would refuse them). The
// debug-layer leak check is the box's (the K5 record, §5).
// Read in the new epoch, before any tombstone is released, as many fresh lifecycle operations as the
// highest tombstoned id are run (K1's 1.11 leg): each fresh handle is above every tombstoned id, and
// every tombstone still reads NotConfigured after they resolve.
// Mutants: the tombstone skipped (the handles read Pending after the re-Configure); the handle
// counter restarted at 1 (a fresh operation takes a tombstoned id and its result shows through).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTeardownReconfigureTest, "SuperSLM.Paging.Gpu.Ledger.TeardownReconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuTeardownReconfigureTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 4, 512, 2, 1);
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> PrefixTokens, BuildTokens;
	if (!MakeTokens(*this, *Rig.Model, 208, 2, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 2000, 3, BuildTokens))
	{
		return false;
	}
	struct FState
	{
		FSuperSLMPrefix Prefix;
		FSuperSLMPrefix Building;
		FSuperSLMGpuSequence Adopter;
		FIssuedGpuRestore Restores[4];
	};
	auto BuildState = [&](FState& S)
	{
		if (!ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig) || !BuildPrefix(*this, Gpu, PrefixTokens, S.Prefix) || !Vend(*this, Gpu, S.Adopter)
			|| !AdoptAndWait(*this, Gpu, S.Adopter, S.Prefix))
		{
			return false;
		}
		const FSuperSLMPagedHold H0 = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
		const FSuperSLMPagedHold H1 = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
		S.Restores[0] = IssueRestore(Gpu, B1000, Rig.Model);
		S.Restores[1] = IssueRestore(Gpu, B1000, Rig.Model);
		S.Restores[2] = IssueRestore(Gpu, B1000, Rig.Model);
		if (!TestTrue(TEXT("two restores are held in flight"), WaitHoldReached(Gpu, H0) && WaitHoldReached(Gpu, H1)))
		{
			return false;
		}
		TickN(*this, Gpu, 4, TEXT("the third restore arrives"));
		FSuperSLMGpuSequence Ignored;
		bool bOk = TestEqual(TEXT("the third restore waits on H"), (uint8)OutcomeOf(Gpu, S.Restores[2], Ignored), (uint8)ESuperSLMRestoreResult::Pending);
		bOk &= TestEqual(TEXT("the ledger counts one waiting restore"), LedgerOf(Gpu).WaitingRestores, 1);
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("the build is admitted (%s)"), *Error), (uint8)Gpu.CreatePrefix(BuildTokens, S.Building, &Error), (uint8)ESuperSLMPrefixResult::Success);
		Pump(Gpu, [&Gpu, &S]() { return FSuperSLMPagedGpuTestSeam::PrefixBuiltTokens(Gpu, S.Building) > 0; }, OpSeconds);
		const int64 Built = FSuperSLMPagedGpuTestSeam::PrefixBuiltTokens(Gpu, S.Building);
		bOk &= TestTrue(*FString::Printf(TEXT("the build is mid-rotation (%lld of 2,000 tokens fed)"), Built), Built > 0 && Built < 2000);
		S.Restores[3] = IssueRestore(Gpu, B1000, Rig.Model); // queued: no tick before the re-Configure
		return bOk;
	};

	FState S;
	if (!BuildState(S))
	{
		return false;
	}
	bool bOk = ConfigurePooled(*this, Rig, C);
	bOk &= TestTrue(TEXT("tombstones are counted as handle entries"), Gpu.GetLifecycleHandleEntryCount() >= 4);
	int64 HighestTombstone = 0;
	for (int32 I = 0; I < 4; ++I)
	{
		FSuperSLMGpuSequence Seq;
		bOk &= TestTrue(*FString::Printf(TEXT("restore %d had a handle"), I), S.Restores[I].Handle.IsValid());
		bOk &= TestEqual(*FString::Printf(TEXT("restore %d reads NotConfigured after the re-Configure (its tombstone)"), I),
			(uint8)Gpu.GetRestoreResult(S.Restores[I].Handle, Seq), (uint8)ESuperSLMRestoreResult::NotConfigured);
		bOk &= TestFalse(*FString::Printf(TEXT("restore %d gives no sequence"), I), Seq.IsValid());
		HighestTombstone = FMath::Max(HighestTombstone, S.Restores[I].Handle.Id);
	}
	// Fresh handles in the new epoch (K1's 1.11 leg): as many fresh lifecycle operations as the highest
	// tombstoned id are run; none takes a tombstoned id, each is above them all, and every tombstone
	// still reads NotConfigured once they resolve.
	{
		FSuperSLMGpuSequence Fresh;
		if (!Vend(*this, Gpu, Fresh))
		{
			return false;
		}
		const int32 FreshOps = int32(FMath::Clamp<int64>(HighestTombstone, 1, 256));
		for (int32 I = 0; I < FreshOps; ++I)
		{
			const FSuperSLMLifecycleOpHandle Op = Gpu.RequestResetSequence(Fresh, ESuperSLMGpuDecodePath::OneCall);
			bOk &= TestTrue(*FString::Printf(TEXT("fresh reset %d queues (%s)"), I, *Gpu.GetLastLifecycleRequestError()), Op.IsValid());
			bOk &= TestTrue(*FString::Printf(TEXT("fresh handle %lld is above every tombstoned id (highest %lld)"), Op.Id, HighestTombstone), Op.Id > HighestTombstone);
			bOk &= TestEqual(*FString::Printf(TEXT("fresh reset %d succeeds"), I), (uint8)WaitOp(Gpu, Op), (uint8)ESuperSLMRestoreResult::Success);
			Gpu.ReleaseLifecycleOpHandle(Op);
		}
		ReturnAndDrain(Gpu, Fresh);
		for (int32 I = 0; I < 4; ++I)
		{
			FSuperSLMGpuSequence Seq;
			bOk &= TestEqual(*FString::Printf(TEXT("after %d fresh operations restore %d still reads NotConfigured"), FreshOps, I),
				(uint8)Gpu.GetRestoreResult(S.Restores[I].Handle, Seq), (uint8)ESuperSLMRestoreResult::NotConfigured);
		}
	}
	for (int32 I = 0; I < 4; ++I)
	{
		FSuperSLMGpuSequence Seq;
		bOk &= TestTrue(*FString::Printf(TEXT("restore %d's tombstone is released"), I), Gpu.ReleaseLifecycleOpHandle(S.Restores[I].Handle));
		bOk &= TestEqual(*FString::Printf(TEXT("restore %d reads Pending once released"), I),
			(uint8)Gpu.GetRestoreResult(S.Restores[I].Handle, Seq), (uint8)ESuperSLMRestoreResult::Pending);
	}
	bOk &= TestEqual(TEXT("the mid-rotation build's prefix reads Invalid"), (uint8)Gpu.GetPrefixPhase(S.Building), (uint8)ESuperSLMPrefixPhase::Invalid);
	bOk &= TestEqual(TEXT("the prefix with a dependent reads Invalid"), (uint8)Gpu.GetPrefixPhase(S.Prefix), (uint8)ESuperSLMPrefixPhase::Invalid);
	bOk &= TestEqual(TEXT("no prefix is live"), Gpu.GetLivePrefixCount(), 0);
	const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
	auto Rebuild = [&]() { return ConfigurePooled(*this, Rig, C); };
	bOk &= ProbeDrawnExactly(*this, Gpu, Want.Pool(), Want.S, Rebuild, TEXT("the rebuilt pool"));

	// The Deinitialize() path.
	FState S2;
	if (!BuildState(S2))
	{
		return false;
	}
	FSuperSLMGpuTeardownStatuses Statuses;
	bOk &= TestTrue(TEXT("the teardown runs"), FSuperSLMGpuTestAccess::TearDownWithStatuses(Gpu, Statuses));
	bOk &= TestEqual(TEXT("the model unmaps (no live sequence, prefix or pool on it)"), Statuses.ModelUnmapStatus, FString(TEXT("SSLM_OK")));
	bOk &= TestEqual(TEXT("the context is destroyed (no live handle)"), Statuses.ContextDestroyStatus, FString(TEXT("SSLM_OK")));
	return bOk;
}

// K6 code review C1, the GPU cell (no plan cell number yet; the CPU twin is
// SuperSLM.Paging.Cpu.Ledger.RecycleTwiceInWindow, in K1's file).
// One slot (BlockCount 1, so every vend is slot 0), with no tick or drain anywhere between the four
// calls: Vend -> Return -> Vend -> Return. The first Return's recycle is still open when the second
// Return queues the next one on the same slot. Afterwards: no ledger fault (LedgerFaults 0 in both
// readings, and no Error line, which the automation run would also fail on), the slot is vended again
// once the subsystem drains (it is not withheld), and form 1 holds; the ledger is the warm reservation
// alone.
// The second Vend must succeed on slot 0, and the cell then returns it at once. That requires the
// review's fix (b), the second recycle deferred behind the first. Fix (a), skipping a slot whose
// recycle is open, would refuse that Vend PoolExhausted at one slot. That is a spurious refusal for a
// game, whose only slot is merely waiting on its recycle, so the cell fails it (the review's ruling).
// Against the K6 build
// as reviewed it fails on the ledger fault (review C1).
// Mutants: the recycle admitted at Return (a second admission on a slot with an open job is refused:
// a ledger fault, and the slot withheld until teardown); a slot with an open recycle skipped at vend
// (the second Vend refused PoolExhausted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuRecycleTwiceTest, "SuperSLM.Paging.Gpu.Ledger.RecycleTwiceInWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuRecycleTwiceTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 0, 1);
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
	bool bOk = true;
	{
		// No tick, drain or pump between these four calls.
		FSuperSLMGpuSequence First;
		if (!Vend(*this, Gpu, First))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("the first vend is slot 0"), FSuperSLMPagedGpuTestSeam::SlotIndexOf(Gpu, First), 0);
		Gpu.ReturnSequence(First);
		FSuperSLMGpuSequence Seq;
		const ESuperSLMGpuVendResult Second = Gpu.VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
		if (!TestEqual(TEXT("the second vend, inside the first recycle's window, succeeds (a slot only waiting on its recycle is not refused PoolExhausted)"),
				(uint8)Second, (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("the second vend is slot 0"), FSuperSLMPagedGpuTestSeam::SlotIndexOf(Gpu, Seq), 0);
		Gpu.ReturnSequence(Seq);
	}
	bOk &= TestTrue(TEXT("the subsystem drains"), DrainQuiet(Gpu));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("after two recycles in one window"));
	{
		FSuperSLMGpuSequence Again;
		bOk &= TestEqual(TEXT("the slot is vended again afterwards (not withheld)"), (uint8)Gpu.VendSequence(Again, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success);
		bOk &= TestEqual(TEXT("the vend afterwards is slot 0"), FSuperSLMPagedGpuTestSeam::SlotIndexOf(Gpu, Again), 0);
		bOk &= TestTrue(TEXT("the slot is returned"), ReturnAndDrain(Gpu, Again));
	}
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after two recycles in one window"));
	bOk &= TestEqual(TEXT("the ledger is the warm reservation alone"), LedgerOf(Gpu).TotalCharged, Want.S);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("at the end"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
