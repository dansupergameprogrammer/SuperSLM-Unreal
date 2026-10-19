// Plugin paging plan, CPU red suite (step K1): the page ledger's invariants (§3.2, §3.9).
//
//   SuperSLM.Paging.Cpu.Ledger.Lifetime                       cell 1.1                    [K2]
//   SuperSLM.Paging.Cpu.Ledger.ZeroSlack                      cell 1.2 (form 3)           [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.PrefixUnderCharge     cell 1.3, row 1 (form 2)    [K2]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.NestedUnderCharge     cell 1.3, row 2             [K3]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.RestoreWithoutE       cell 1.3, row 3             [K3]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.Ssb5Charge            cell 1.3, row 4             [K3]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.SharerRegistered      cell 1.3, row 5             [K3]
//   SuperSLM.Paging.Cpu.Ledger.Boundary.HeadroomHeldAfterApply cell 1.3, row 7            [K3]
//   SuperSLM.Paging.Cpu.Ledger.InFlightAfterDraw              cell 1.4 (form 1)           [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.EnqueueBeforeFree              cell 1.5 (forms 1 and 2)    [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.FailureRefund                  cell 1.6 (CPU cases)        [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.PinsRefuseRelease              cell 3.1; §11 the pins      [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.OneJobPerSlot                  cell 3.5                    [K2/K3/K6/K7]
//   SuperSLM.Paging.Cpu.Ledger.FaultDetector                  cell 11.2                   [K2/K3]
//   SuperSLM.Paging.Cpu.Ledger.RecycleTwiceInWindow           K6 code review C1, CPU twin [K2]
//
// Tags (plan rev 1.9, §7 conventions): [Kn/Km] means the cell's legs start at Kn and the cell is
// green at Km; a step's exit counts only the legs its own content reaches. On the CPU, restores,
// nested prefixes and hints are K3 content, so 1.4, 1.5, 3.1, 3.5 and 11.2 are green at K3.
//
// Every construction is computed for A-EX (B = 64, a block of 64 pages, R(512) = 9); each test
// checks that shape before it counts. Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	bool ExpectPrefixRefused(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Tokens, const TCHAR* What)
	{
		const int64 PBefore = Charged(Cpu, ERegion::P);
		const int32 LiveBefore = Cpu.GetLivePrefixCount();
		FSuperSLMPrefix Prefix;
		FString Error;
		const ESuperSLMPrefixResult Result = Cpu.CreatePrefix(Tokens, Prefix, &Error);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: refused KvPagesExhausted (%s)"), What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::KvPagesExhausted);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no handle"), What), Prefix.IsValid());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing charged on P"), What), Charged(Cpu, ERegion::P), PBefore);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no prefix created"), What), Cpu.GetLivePrefixCount(), LiveBefore);
		return bOk;
	}

	bool ExpectNestedRefused(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, const TCHAR* What)
	{
		const int64 PBefore = Charged(Cpu, ERegion::P);
		FSuperSLMPrefix Child;
		FString Error;
		const ESuperSLMPrefixResult Result = Cpu.CreatePrefixFrom(Parent, Tokens, Child, &Error);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: refused KvPagesExhausted (%s)"), What, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::KvPagesExhausted);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no handle"), What), Child.IsValid());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing charged on P"), What), Charged(Cpu, ERegion::P), PBefore);
		return bOk;
	}

	// A commissioning leg (§3.9 ForceUnderCharge): with the seam set, the boundary request is
	// admitted, which is what the mutant does. Proves the boundary has resolving power. Admission of
	// a restore is read as: it succeeded, or the engine refused its draw (a ledger fault), never a
	// refusal on arrival.
	bool RestoreWasAdmitted(USuperSLMSubsystem& Cpu, const FIssuedRestore& R, uint64 FaultsBefore)
	{
		const ESuperSLMRestoreResult Outcome = SettleOutcome(Cpu, R);
		return Outcome == ESuperSLMRestoreResult::Success || Cpu.GetKvPageStats().LedgerFaults > FaultsBefore;
	}
}

// Cell 1.1 (§3.2): 1,000 cycles on every slot, vend -> adopt -> generate -> reset -> re-adopt ->
// return. At the end the probe shows only the warm reservations drawn (exact form), the ledger
// equals that count (a charge leaked by a dependency never dropped at re-adopt shows here, where
// form 1's one-sided check would pass it), and every slot's last-cycle tokens equal its first.
// Mutant: the previous dependency not dropped at re-adopt (the ledger ends above S).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuLifetimeTest, "SuperSLM.Paging.Cpu.Ledger.Lifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuLifetimeTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	const FSuperSLMRuntimeConfig Config = MakeConfig(2, 512, 1, 1);
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, Config) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FRegions Want = ExpectedRegions(Rig.Geo, Config);
	TArray<int32> PrefixTokens, Suffix[2];
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, Cpu, 100, 0, PrefixTokens) || !MakeTokens(*this, Cpu, 6, 1, Suffix[0]) || !MakeTokens(*this, Cpu, 6, 2, Suffix[1])
		|| !BuildPrefix(*this, Cpu, PrefixTokens, Prefix))
	{
		return false;
	}
	constexpr int32 Cycles = 1000;
	TArray<int32> First[2], Last[2];
	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		FSuperSLMSequence Seq[2];
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			if (!Vend(*this, Cpu, Seq[Slot]) || !AdoptAndWait(*this, Cpu, Seq[Slot], Prefix))
			{
				AddError(FString::Printf(TEXT("cycle %d, slot %d: vend or adopt failed"), Cycle, Slot));
				return false;
			}
		}
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			TArray<int32> Tokens;
			if (!GenerateComplete(*this, Cpu, Seq[Slot], Suffix[Slot], 4, Tokens) || !ResetAndWait(*this, Cpu, Seq[Slot]) || !AdoptAndWait(*this, Cpu, Seq[Slot], Prefix))
			{
				AddError(FString::Printf(TEXT("cycle %d, slot %d: generate, reset or re-adopt failed"), Cycle, Slot));
				return false;
			}
			(Cycle == 0 ? First[Slot] : Last[Slot]) = Tokens;
			Cpu.ReturnSequence(Seq[Slot]);
		}
		DrainQuiet(Cpu);
	}
	bool bOk = true;
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: the last cycle's tokens equal the first's"), Slot), Last[Slot], First[Slot]);
	}
	bOk &= ReleasePrefixAndDrain(*this, Cpu, Prefix);
	const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
	bOk &= TestEqual(TEXT("after 1,000 cycles the ledger is S alone"), V.TotalCharged, Want.S);
	bOk &= TestEqual(TEXT("P is empty"), V.ChargedOf(ERegion::P), int64(0));
	bOk &= TestEqual(TEXT("H is empty"), V.ChargedOf(ERegion::H), int64(0));
	bOk &= TestEqual(TEXT("no ghost remains"), V.GhostCount, 0);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("after 1,000 cycles"));
	auto Rebuild = [this, &Rig, &Config]() { return ConfigureBudget(*this, Rig, Config); };
	bOk &= ProbeDrawnExactly(*this, Cpu, Want.Pool(), Want.S, Rebuild, TEXT("after 1,000 cycles: the post-Configure value"));
	return bOk;
}

// Cell 1.2 (form 3, zero slack): each CPU request kind runs where the region under test has
// exactly its charge free and every other region is drawn to its bound or sized 0, so a draw above
// the charge reaches the engine's KV_POOL_EXHAUSTED. Kinds: a prefix build, a nested build, an
// unhinted restore, a hinted restore that does not share, an 'SSB5' blob into budget mode. Each is
// admitted, completes, and leaves LedgerFaults 0; after quiescence the probe equals the test's
// count exactly (the [two-cap] and [cache] kinds are the GPU's and a later release's).
// Mutant: any kind drawing above its charge (the engine exhausts: a fault, or a refused job), which
// is what zero slack makes visible. Not killed here: a kind charged below its draw, since the region
// under test holds exactly its charge free and the draw still fits the pool (cell 1.3's boundary
// pairs kill those); and R priced at the plugin's cap, which is the same figure on the CPU (one cap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuZeroSlackTest, "SuperSLM.Paging.Cpu.Ledger.ZeroSlack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuZeroSlackTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	// The blobs first, each at its own shape: origin 3,520 at budget 512 (E = 55, charge 9 + 55 = 64,
	// a whole H block), and an 'SSB5' blob (charged P_blk = 64).
	TArray<uint8> Blob3520, BlobSsb5;
	if (!MakeBlobAt(*this, Rig, 512, 3520, 3, Blob3520) || !RequireAExShape(*this, Rig) || !MakeSsb5Blob(*this, Rig, BlobSsb5))
	{
		return false;
	}
	const FCpuPagingGeometry& G = Rig.Geo;
	TestEqual(TEXT("the origin-3,520 restore is charged a whole block"), G.RestoreCharge(512, 3520), int64(64));
	bool bOk = true;

	// (a) Prefix build. P = 1 block; a ghost of 1,024 tokens (16 whole pages, held by the only slot's
	// adopter) holds 16, so P has 48 free, exactly R(3,000) = 48. H = 0. The 16 are a ghost, not a
	// live prefix: with PrefixBlockCount 1 a second live prefix is refused by the count rule before
	// the page rule is reached. The ghost's length is page-aligned, so its release frees no tail
	// page and the engine's free pages equal P's room.
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 1, 0);
		TArray<int32> QTokens, Tokens;
		FSuperSLMPrefix Q, Built;
		FSuperSLMSequence QAdopter;
		auto Build = [&]() { return ConfigureBudget(*this, Rig, C) && MakeGhost(*this, Cpu, QTokens, Q, QAdopter); };
		if (!MakeTokens(*this, Cpu, 1024, 1, QTokens) || !MakeTokens(*this, Cpu, 3000, 2, Tokens) || !Build())
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(a) P has exactly R(3,000) = 48 free"), Free(Cpu, ERegion::P), G.PrefixCharge(3000));
		bOk &= BuildPrefix(*this, Cpu, Tokens, Built);
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(a) prefix build under zero slack"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Built);
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), G.R(512) + G.OccupiedPages(1024), Build, TEXT("(a) after the build's release"));
	}

	// (b) Nested build. P = 2 blocks (128): a ghost of 4,032 tokens (63, held by the only slot's
	// adopter) and a 1,000-token parent (16) leave 49, exactly R(3,050) = 49.
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 2, 0);
		TArray<int32> GhostTokens, ParentTokens, ChildTokens;
		FSuperSLMPrefix Ghost, Parent, Child;
		FSuperSLMSequence Adopter;
		auto Build = [&]()
		{
			return ConfigureBudget(*this, Rig, C) && MakeGhost(*this, Cpu, GhostTokens, Ghost, Adopter) && BuildPrefix(*this, Cpu, ParentTokens, Parent);
		};
		if (!MakeTokens(*this, Cpu, 4032, 3, GhostTokens) || !MakeTokens(*this, Cpu, 1000, 4, ParentTokens) || !MakeTokens(*this, Cpu, 3050, 5, ChildTokens) || !Build())
		{
			return false;
		}
		bOk &= TestEqual(TEXT("(b) P has exactly R(3,050) = 49 free"), Free(Cpu, ERegion::P), G.PrefixCharge(3050));
		bOk &= BuildNested(*this, Cpu, Parent, ChildTokens, Child);
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(b) nested build under zero slack"));
		bOk &= TestEqual(TEXT("(b) the child settles to its private pages"), FSuperSLMPagedTestSeam::PrefixChargedPages(Cpu, Child), G.NestedPrivate(1000, 3050));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Child);
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), G.R(512) + 63 + G.OccupiedPages(1000), Build, TEXT("(b) after the child's release"));
	}

	// (c) Unhinted restore. P = 0, H = 1 block, exactly the charge of 64. The restore draws 64 beside
	// the warm holder's 9: the whole pool.
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 0, 1);
		FSuperSLMSequence Seq;
		auto Build = [&]() { return ConfigureBudget(*this, Rig, C) && ApplyRestore(*this, Cpu, Blob3520, Rig.Model, Seq); };
		if (!Build())
		{
			return false;
		}
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(c) unhinted restore under zero slack"));
		bOk &= TestEqual(TEXT("(c) H holds max(0, 64 - R(512)) = 55 after the apply"), Charged(Cpu, ERegion::H), G.HeadroomHold(64, 512));
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), 64, Build, TEXT("(c) after the apply"));
	}

	// (d) Hinted restore that does not share. P = 1 block, drawn whole by a 4,090-token prefix (the
	// hint, whose length differs from the blob's origin 3,520); H = 1 block, exactly the charge.
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 1, 1);
		TArray<int32> HintTokens;
		FSuperSLMPrefix Hint;
		FSuperSLMSequence Seq;
		auto Build = [&]()
		{
			return ConfigureBudget(*this, Rig, C) && BuildPrefix(*this, Cpu, HintTokens, Hint) && ApplyRestore(*this, Cpu, Blob3520, Rig.Model, Seq, Hint);
		};
		if (!MakeTokens(*this, Cpu, 4090, 3, HintTokens) || !Build())
		{
			return false;
		}
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(d) hinted, non-sharing restore under zero slack"));
		bOk &= TestEqual(TEXT("(d) the hint did not share: H holds 55"), Charged(Cpu, ERegion::H), G.HeadroomHold(64, 512));
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), 64 + G.OccupiedPages(4090), Build, TEXT("(d) after the apply"));
	}

	// (e) 'SSB5' into budget mode: charged P_blk = 64 on H, exactly H's block; the whole-reserve
	// holder draws one block.
	{
		const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 0, 1);
		FSuperSLMSequence Seq;
		auto Build = [&]() { return ConfigureBudget(*this, Rig, C) && ApplyRestore(*this, Cpu, BlobSsb5, Rig.Model, Seq); };
		if (!Build())
		{
			return false;
		}
		bOk &= NoLedgerFault(*this, Cpu, TEXT("(e) 'SSB5' restore under zero slack"));
		bOk &= ProbeDrawnExactly(*this, Cpu, ExpectedRegions(G, C).Pool(), G.Block(), Build, TEXT("(e) after the apply"));
	}
	return bOk;
}

// Cell 1.3, row 1 (form 2), and §5 KvPagesExhausted (prefix; both CreatePrefix overloads): a prefix
// charged ceil(L/B) for R(L). P = 1 block with a ghost of 3,072
// tokens (48) leaves 16 free: a 1,000-token prefix (R = 17) is refused KvPagesExhausted; a
// 900-token prefix (R = 16) is admitted. Commissioning: ForceUnderCharge(PrefixBuild, 1) admits the
// 1,000.
// Mutant: the prefix charged ceil(L/B), which admits the 1,000.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundaryPrefixTest, "SuperSLM.Paging.Cpu.Ledger.Boundary.PrefixUnderCharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundaryPrefixTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 1, 0);
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> GhostTokens, Over, Exact;
	FSuperSLMPrefix Ghost, Admitted;
	FSuperSLMSequence Adopter;
	if (!MakeTokens(*this, Cpu, 3072, 1, GhostTokens) || !MakeTokens(*this, Cpu, 1000, 2, Over) || !MakeTokens(*this, Cpu, 900, 3, Exact)
		|| !MakeGhost(*this, Cpu, GhostTokens, Ghost, Adopter))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P has 16 free"), Free(Cpu, ERegion::P), int64(16));
	bOk &= TestEqual(TEXT("R(1,000) = 17"), Rig.Geo.PrefixCharge(1000), int64(17));
	bOk &= TestEqual(TEXT("R(900) = 16"), Rig.Geo.PrefixCharge(900), int64(16));
	bOk &= ExpectPrefixRefused(*this, Cpu, Over, TEXT("P free = R(1,000) - 1"));
	{
		// §3.6: the existing bool overload is unchanged, and its page refusal's message starts with the
		// stable token "KvPagesExhausted:".
		FSuperSLMPrefix Refused;
		FString Message;
		bOk &= TestFalse(TEXT("the bool CreatePrefix overload refuses the same build"), Cpu.CreatePrefix(Over, Refused, Message));
		bOk &= TestTrue(*FString::Printf(TEXT("its message starts with \"KvPagesExhausted:\" (%s)"), *Message), Message.StartsWith(TEXT("KvPagesExhausted:"), ESearchCase::CaseSensitive));
	}
	bOk &= BuildPrefix(*this, Cpu, Exact, Admitted);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit build"));
	bOk &= ReleasePrefixAndDrain(*this, Cpu, Admitted);
	bOk &= TestTrue(TEXT("commissioning: ForceUnderCharge(PrefixBuild, 1) is set"), FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::PrefixBuild, 1));
	FSuperSLMPrefix Mutant;
	FString Error;
	bOk &= TestEqual(*FString::Printf(TEXT("commissioning: the under-charged build is admitted (%s)"), *Error),
		(uint8)Cpu.CreatePrefix(Over, Mutant, &Error), (uint8)ESuperSLMPrefixResult::Success);
	FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::PrefixBuild, 0);
	DrainQuiet(Cpu, BuildSeconds);
	return bOk;
}

// Cell 1.3, row 2 (form 2): a nested child charged 0. P = 2 blocks with a ghost of 4,032 tokens
// (63) and a 1,000-token parent (16) leaves 49: a 3,090-token child (R = 50) is refused; a
// 3,050-token child (R = 49) is admitted. Commissioning: ForceUnderCharge(NestedBuild, 1).
// Mutant: the child charged 0 (or one page short), which admits the 3,090.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundaryNestedTest, "SuperSLM.Paging.Cpu.Ledger.Boundary.NestedUnderCharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundaryNestedTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 2, 0);
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> GhostTokens, ParentTokens, Over, Exact;
	FSuperSLMPrefix Ghost, Parent, Child;
	FSuperSLMSequence Adopter;
	if (!MakeTokens(*this, Cpu, 4032, 1, GhostTokens) || !MakeTokens(*this, Cpu, 1000, 2, ParentTokens) || !MakeTokens(*this, Cpu, 3090, 3, Over)
		|| !MakeTokens(*this, Cpu, 3050, 4, Exact) || !MakeGhost(*this, Cpu, GhostTokens, Ghost, Adopter) || !BuildPrefix(*this, Cpu, ParentTokens, Parent))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("P has 49 free"), Free(Cpu, ERegion::P), int64(49));
	bOk &= ExpectNestedRefused(*this, Cpu, Parent, Over, TEXT("P free = R(3,090) - 1"));
	bOk &= BuildNested(*this, Cpu, Parent, Exact, Child);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit child"));
	bOk &= ReleasePrefixAndDrain(*this, Cpu, Child);
	bOk &= TestTrue(TEXT("commissioning: ForceUnderCharge(NestedBuild, 1) is set"), FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::NestedBuild, 1));
	FSuperSLMPrefix Mutant;
	FString Error;
	bOk &= TestEqual(*FString::Printf(TEXT("commissioning: the under-charged child is admitted (%s)"), *Error),
		(uint8)Cpu.CreatePrefixFrom(Parent, Over, Mutant, &Error), (uint8)ESuperSLMPrefixResult::Success);
	FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::NestedBuild, 0);
	DrainQuiet(Cpu, BuildSeconds);
	return bOk;
}

// Cell 1.3, row 3 (form 2): a restore charged without E. Two applied private holders (origins 1,000
// and 1,664 at budget 512: holds 15 and 26) leave H 23 free = R(512) + E(1,000) - 1: a restore at
// origin 1,000 (charge 24) is refused on arrival; one at origin 896 (charge 23) is admitted.
// Commissioning: ForceUnderCharge(BudgetRestore, 1) admits the 24.
// Mutant: the restore charged R(b) alone (9), which admits it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundaryRestoreTest, "SuperSLM.Paging.Cpu.Ledger.Boundary.RestoreWithoutE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundaryRestoreTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000, B1664, B896;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !RequireAExShape(*this, Rig) || !MakeBlobAt(*this, Rig, 512, 1664, 2, B1664)
		|| !MakeBlobAt(*this, Rig, 512, 896, 3, B896))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FSuperSLMRuntimeConfig C = MakeConfig(3, 512, 0, 1);
	FSuperSLMSequence H1, H2;
	auto Build = [&]()
	{
		return ConfigureBudget(*this, Rig, C) && ApplyRestore(*this, Cpu, B1000, Rig.Model, H1) && ApplyRestore(*this, Cpu, B1664, Rig.Model, H2);
	};
	if (!Build())
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("H has 23 free"), Free(Cpu, ERegion::H), Rig.Geo.RestoreCharge(512, 1000) - 1);
	{
		const FIssuedRestore R = IssueRestore(Cpu, B1000, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Cpu, R, Charged(Cpu, ERegion::H), TEXT("H free = R(b) + E - 1"));
		Discard(Cpu, R);
	}
	FSuperSLMSequence Fits;
	bOk &= TestEqual(TEXT("the origin-896 restore is charged 23"), Rig.Geo.RestoreCharge(512, 896), int64(23));
	bOk &= ApplyRestore(*this, Cpu, B896, Rig.Model, Fits);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit restore"));
	if (!Build())
	{
		return false;
	}
	bOk &= TestTrue(TEXT("commissioning: ForceUnderCharge(BudgetRestore, 1) is set"), FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::BudgetRestore, 1));
	const uint64 FaultsBefore = Cpu.GetKvPageStats().LedgerFaults;
	const FIssuedRestore Mutant = IssueRestore(Cpu, B1000, Rig.Model);
	bOk &= TestTrue(TEXT("commissioning: the under-charged restore is admitted"), RestoreWasAdmitted(Cpu, Mutant, FaultsBefore));
	return bOk;
}

// Cell 1.3, row 4 (form 2): an 'SSB5' payload charged R(b) in place of P_blk. One applied holder at
// origin 64 (budget 512: charge 10, hold 1) leaves H 63 free = P_blk - 1: the 'SSB5' restore is
// refused on arrival. After that holder resets (its hold returns), H has 64 and the same restore
// is admitted. Commissioning: ForceUnderCharge(WholeReserveRestore, 1).
// Mutant: 'SSB5' charged R(b), which admits it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundarySsb5Test, "SuperSLM.Paging.Cpu.Ledger.Boundary.Ssb5Charge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundarySsb5Test::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B64, Ssb5;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 64, 1, B64) || !RequireAExShape(*this, Rig) || !MakeSsb5Blob(*this, Rig, Ssb5))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FSuperSLMRuntimeConfig C = MakeConfig(2, 512, 0, 1);
	FSuperSLMSequence Holder;
	auto Build = [&]() { return ConfigureBudget(*this, Rig, C) && ApplyRestore(*this, Cpu, B64, Rig.Model, Holder); };
	if (!Build())
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("H has P_blk - 1 = 63 free"), Free(Cpu, ERegion::H), Rig.Geo.Block() - 1);
	{
		const FIssuedRestore R = IssueRestore(Cpu, Ssb5, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Cpu, R, Charged(Cpu, ERegion::H), TEXT("H free = P_blk - 1"));
		Discard(Cpu, R);
	}
	bOk &= ResetAndWait(*this, Cpu, Holder);
	bOk &= TestEqual(TEXT("after the reset H has P_blk free"), Free(Cpu, ERegion::H), Rig.Geo.Block());
	FSuperSLMSequence Restored;
	bOk &= ApplyRestore(*this, Cpu, Ssb5, Rig.Model, Restored);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit 'SSB5' restore"));
	if (!Build())
	{
		return false;
	}
	bOk &= TestTrue(TEXT("commissioning: ForceUnderCharge(WholeReserveRestore, 1) is set"),
		FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::WholeReserveRestore, 1));
	const uint64 FaultsBefore = Cpu.GetKvPageStats().LedgerFaults;
	const FIssuedRestore Mutant = IssueRestore(Cpu, Ssb5, Rig.Model);
	bOk &= TestTrue(TEXT("commissioning: the under-charged 'SSB5' restore is admitted"), RestoreWasAdmitted(Cpu, Mutant, FaultsBefore));
	return bOk;
}

// Cell 1.3, row 5 (form 2): a restored sharer not registered. P = 1 block: a ghost Y of 2,112 tokens
// (33) and a live prefix X of 1,000 tokens (16). A blob saved from an adopter of X is restored with
// X as its hint and shares; X is then released, and stays a ghost of 16 because the restored
// sequence depends on it. P has 15 free: a 900-token prefix (R = 16) is refused; an 850-token
// prefix (R = 15) is admitted. (A patch mutant: no seam makes it, so no commissioning leg.)
// Mutant: the restored sharer not registered, so X's release frees its 16 and admits the 900.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundarySharerTest, "SuperSLM.Paging.Cpu.Ledger.Boundary.SharerRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundarySharerTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	const FSuperSLMRuntimeConfig C = MakeConfig(2, 512, 1, 1);
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> YTokens, XTokens, Suffix, Over, Exact;
	FSuperSLMPrefix Y, X, Admitted;
	FSuperSLMSequence YAdopter, Restored;
	TArray<uint8> Blob;
	if (!MakeTokens(*this, Cpu, 2112, 1, YTokens) || !MakeTokens(*this, Cpu, 1000, 2, XTokens) || !MakeTokens(*this, Cpu, 4, 3, Suffix)
		|| !MakeTokens(*this, Cpu, 900, 4, Over) || !MakeTokens(*this, Cpu, 850, 5, Exact)
		|| !MakeGhost(*this, Cpu, YTokens, Y, YAdopter) || !BuildPrefix(*this, Cpu, XTokens, X) || !MakeAdoptedBlob(*this, Cpu, X, Suffix, 2, Blob)
		|| !ApplyRestore(*this, Cpu, Blob, Rig.Model, Restored, X))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the restored sequence registered on X"), FSuperSLMPagedTestSeam::PrefixDependentCount(Cpu, X), 1);
	bOk &= TestEqual(TEXT("the restore shared: H holds nothing after the apply"), Charged(Cpu, ERegion::H), int64(0));
	bOk &= ReleasePrefixAndDrain(*this, Cpu, X);
	bOk &= TestTrue(TEXT("X is a ghost while the restored sequence depends on it"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, X));
	bOk &= TestEqual(TEXT("P has 15 free"), Free(Cpu, ERegion::P), int64(15));
	bOk &= ExpectPrefixRefused(*this, Cpu, Over, TEXT("P free = R(900) - 1"));
	bOk &= BuildPrefix(*this, Cpu, Exact, Admitted);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit build"));
	return bOk;
}

// Cell 1.3, row 7 (form 2): H refunded at apply for a private restore. Three applied private
// holders (origins 1,280, 1,000 and 1,000 at budget 512: holds 20, 15 and 15) leave H 14 free, one
// below the net hold of 15: a restore at origin 384 (charge 15) is refused on arrival; one at
// origin 320 (charge 14) is admitted. (A patch mutant: no commissioning leg.)
// Mutant: the private hold refunded at apply, which leaves H empty and admits the 15.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuBoundaryHeldTest, "SuperSLM.Paging.Cpu.Ledger.Boundary.HeadroomHeldAfterApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuBoundaryHeldTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1280, B1000, B384, B320;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1280, 1, B1280) || !RequireAExShape(*this, Rig) || !MakeBlobAt(*this, Rig, 512, 1000, 2, B1000)
		|| !MakeBlobAt(*this, Rig, 512, 384, 3, B384) || !MakeBlobAt(*this, Rig, 512, 320, 4, B320))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	FSuperSLMSequence S1, S2, S3, Fits;
	if (!ConfigureBudget(*this, Rig, MakeConfig(4, 512, 0, 1)) || !ApplyRestore(*this, Cpu, B1280, Rig.Model, S1)
		|| !ApplyRestore(*this, Cpu, B1000, Rig.Model, S2) || !ApplyRestore(*this, Cpu, B1000, Rig.Model, S3))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("the holds are 20 + 15 + 15"), Charged(Cpu, ERegion::H),
		G.HeadroomHold(G.RestoreCharge(512, 1280), 512) + 2 * G.HeadroomHold(G.RestoreCharge(512, 1000), 512));
	bOk &= TestEqual(TEXT("H has 14 free"), Free(Cpu, ERegion::H), int64(14));
	bOk &= TestEqual(TEXT("the origin-384 restore is charged 15"), G.RestoreCharge(512, 384), int64(15));
	bOk &= TestEqual(TEXT("the origin-320 restore is charged 14"), G.RestoreCharge(512, 320), int64(14));
	{
		const FIssuedRestore R = IssueRestore(Cpu, B384, Rig.Model);
		bOk &= ExpectRefusedOnArrival(*this, Cpu, R, Charged(Cpu, ERegion::H), TEXT("H free = net hold - 1"));
		Discard(Cpu, R);
	}
	bOk &= ApplyRestore(*this, Cpu, B320, Rig.Model, Fits);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("the exact-fit restore"));
	return bOk;
}

// Cell 1.4 (form 1): the in-flight window after the draw. HoldApply on each drawing kind (a prefix
// build, a nested build, a budget restore, an 'SSB5' restore, a normalization reset); during each
// hold the probe's drawn pages are at most the ledger's charge, and again after each apply.
// Mutant: any charge settled (refunded) before the apply, which drops the charge below the draw
// inside the window.
// Sizing (A-EX, block 64): RestoreHeadroomBlocks = 2, so H = 128. The budget restore at b = 512,
// o = 1000 is charged R + E = 9 + 15 = 24 and, once applied, keeps a hold of 24 - 9 = 15 until its
// holder resets: 113 of H free. The 'SSB5' restore is charged its whole block, 64 <= 113, so it is
// admitted (with one block of H it would meet 64 - 15 = 49 free, be refused on arrival, and its hold
// would never be reached); after its apply it holds 64 - 9 = 55. The normalization create then needs
// R(512) = 9: 55 + 15 + 9 = 79 <= 128.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuInFlightTest, "SuperSLM.Paging.Cpu.Ledger.InFlightAfterDraw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuInFlightTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000, Ssb5;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !MakeSsb5Blob(*this, Rig, Ssb5) || !ConfigureBudget(*this, Rig, MakeConfig(3, 512, 2, 2)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> ParentTokens, ChildTokens;
	if (!MakeTokens(*this, Cpu, 1000, 2, ParentTokens) || !MakeTokens(*this, Cpu, 500, 3, ChildTokens))
	{
		return false;
	}
	bool bOk = true;
	auto HeldWindow = [&](ESuperSLMPagedRequestKind Kind, const TCHAR* Name, TFunctionRef<bool()> Issue, TFunctionRef<bool()> Finish)
	{
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, Kind);
		if (!TestTrue(*FString::Printf(TEXT("%s: HoldApply is armed"), Name), Hold.IsValid()) || !Issue())
		{
			bOk = false;
			return;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the job reaches its held apply"), Name), WaitHoldReached(Cpu, Hold, BuildSeconds));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("%s, held at its apply"), Name));
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the hold is released"), Name), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= Finish();
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("%s, after its apply"), Name));
	};

	FSuperSLMPrefix Parent, Child;
	HeldWindow(ESuperSLMPagedRequestKind::PrefixBuild, TEXT("prefix build"),
		[&]() { FString E; return TestEqual(TEXT("CreatePrefix"), (uint8)Cpu.CreatePrefix(ParentTokens, Parent, &E), (uint8)ESuperSLMPrefixResult::Success); },
		[&]() { return WaitPrefixReady(*this, Cpu, Parent); });
	HeldWindow(ESuperSLMPagedRequestKind::NestedBuild, TEXT("nested build"),
		[&]() { FString E; return TestEqual(TEXT("CreatePrefixFrom"), (uint8)Cpu.CreatePrefixFrom(Parent, ChildTokens, Child, &E), (uint8)ESuperSLMPrefixResult::Success); },
		[&]() { return WaitPrefixReady(*this, Cpu, Child); });
	FIssuedRestore Budget, Whole;
	HeldWindow(ESuperSLMPagedRequestKind::Restore, TEXT("budget restore"),
		[&]() { Budget = IssueRestore(Cpu, B1000, Rig.Model); return TestEqual(TEXT("the restore queues"), (uint8)Budget.Sync, (uint8)ESuperSLMRestoreResult::Success); },
		[&]() { return TestEqual(TEXT("the restore succeeds"), (uint8)WaitOp(Cpu, Budget.Handle), (uint8)ESuperSLMRestoreResult::Success); });
	HeldWindow(ESuperSLMPagedRequestKind::Restore, TEXT("'SSB5' restore"),
		[&]() { Whole = IssueRestore(Cpu, Ssb5, Rig.Model); return TestEqual(TEXT("the restore queues"), (uint8)Whole.Sync, (uint8)ESuperSLMRestoreResult::Success); },
		[&]() { return TestEqual(TEXT("the restore succeeds"), (uint8)WaitOp(Cpu, Whole.Handle), (uint8)ESuperSLMRestoreResult::Success); });
	FSuperSLMLifecycleOpHandle ResetHandle;
	HeldWindow(ESuperSLMPagedRequestKind::Reset, TEXT("normalization reset of the 'SSB5' holder"),
		[&]()
		{
			FString E;
			const ESuperSLMRestoreResult Queued = Cpu.ResetSequence(Whole.Sequence, ResetHandle, E);
			return TestEqual(*FString::Printf(TEXT("ResetSequence (%s)"), *E), (uint8)Queued, (uint8)ESuperSLMRestoreResult::Success);
		},
		[&]() { return TestEqual(TEXT("the reset succeeds"), (uint8)WaitOp(Cpu, ResetHandle), (uint8)ESuperSLMRestoreResult::Success); });
	bOk &= NoLedgerFault(*this, Cpu, TEXT("after every held window"));
	return bOk;
}

// Cell 1.5 (CPU only; forms 1 and 2): the enqueue window before the free. HoldStart holds a reset
// and a prefix release un-run. During each hold the freeing charge is still in the ledger and the
// probe still shows its pages drawn (form 1), and a boundary request sized to the pages being freed
// is refused by name (form 2). After the hold is released and the free applies, the same request is
// admitted.
//   Reset leg: three applied private holders (holds 15 each) leave H 19 free; one is reset under
//   HoldStart; a restore charged 24 is refused, then admitted once the reset applies (34 free).
//   Release leg: P = 2 blocks with a ghost of 4,032 tokens (63) and a live 1,000-token prefix (16)
//   leave 49; the 1,000 is released under HoldStart; a 3,136-token prefix (R = 50) is refused, then
//   admitted once the release applies (65 free).
// Mutant: the refund taken at enqueue, which admits each boundary request inside the window.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuEnqueueWindowTest, "SuperSLM.Paging.Cpu.Ledger.EnqueueBeforeFree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuEnqueueWindowTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	bool bOk = true;

	// The reset leg.
	{
		FSuperSLMSequence Holders[3];
		if (!ConfigureBudget(*this, Rig, MakeConfig(4, 512, 0, 1)))
		{
			return false;
		}
		for (FSuperSLMSequence& H : Holders)
		{
			if (!ApplyRestore(*this, Cpu, B1000, Rig.Model, H))
			{
				return false;
			}
		}
		bOk &= TestEqual(TEXT("reset leg: H has 19 free"), Free(Cpu, ERegion::H), int64(19));
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::Reset);
		FSuperSLMLifecycleOpHandle ResetHandle;
		FString Error;
		if (!TestTrue(TEXT("reset leg: HoldStart is armed"), Hold.IsValid())
			|| !TestEqual(TEXT("reset leg: the reset queues"), (uint8)Cpu.ResetSequence(Holders[0], ResetHandle, Error), (uint8)ESuperSLMRestoreResult::Success)
			|| !TestTrue(TEXT("reset leg: the reset is held before its Layer-1 work"), WaitHoldReached(Cpu, Hold)))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("reset leg: the freeing hold is still charged"), Charged(Cpu, ERegion::H), int64(45));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("reset leg, held at its start"));
		{
			const FIssuedRestore R = IssueRestore(Cpu, B1000, Rig.Model);
			bOk &= ExpectRefusedOnArrival(*this, Cpu, R, int64(45), TEXT("reset leg: a restore sized to the pages being freed"));
			Discard(Cpu, R);
		}
		bOk &= TestTrue(TEXT("reset leg: the hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= TestEqual(TEXT("reset leg: the reset succeeds"), (uint8)WaitOp(Cpu, ResetHandle), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(TEXT("reset leg: the hold is refunded after the apply"), Charged(Cpu, ERegion::H), int64(30));
		FSuperSLMSequence After;
		bOk &= ApplyRestore(*this, Cpu, B1000, Rig.Model, After);
	}

	// The release leg.
	{
		TArray<int32> GhostTokens, LiveTokens, Boundary;
		FSuperSLMPrefix Ghost, Live, Admitted;
		FSuperSLMSequence Adopter;
		if (!ConfigureBudget(*this, Rig, MakeConfig(1, 512, 2, 0)) || !MakeTokens(*this, Cpu, 4032, 1, GhostTokens) || !MakeTokens(*this, Cpu, 1000, 2, LiveTokens)
			|| !MakeTokens(*this, Cpu, 3136, 3, Boundary) || !MakeGhost(*this, Cpu, GhostTokens, Ghost, Adopter) || !BuildPrefix(*this, Cpu, LiveTokens, Live))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("release leg: P has 49 free"), Free(Cpu, ERegion::P), int64(49));
		bOk &= TestEqual(TEXT("release leg: R(3,136) = 50"), Rig.Geo.PrefixCharge(3136), int64(50));
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldStart(Cpu, ESuperSLMPagedRequestKind::PrefixRelease);
		FString Error;
		if (!TestTrue(TEXT("release leg: HoldStart is armed"), Hold.IsValid()) || !TestTrue(*FString::Printf(TEXT("release leg: ReleasePrefix (%s)"), *Error), Cpu.ReleasePrefix(Live, Error))
			|| !TestTrue(TEXT("release leg: the release is held before its Layer-1 work"), WaitHoldReached(Cpu, Hold)))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("release leg: the freeing charge is still present"), Charged(Cpu, ERegion::P), int64(63 + 16));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("release leg, held at its start"));
		bOk &= ExpectPrefixRefused(*this, Cpu, Boundary, TEXT("release leg: a prefix sized to the pages being freed"));
		bOk &= TestTrue(TEXT("release leg: the hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= DrainQuiet(Cpu);
		bOk &= TestEqual(TEXT("release leg: P has 65 free after the apply"), Free(Cpu, ERegion::P), int64(65));
		bOk &= BuildPrefix(*this, Cpu, Boundary, Admitted);
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 1.5"));
	return bOk;
}

// Cell 1.6 (form 1, the CPU's cases): a post-admission failure refunds exactly. Three failures, each
// after an admitted charge: a budget payload with one trailing byte, and one cut short past its
// header (the engine restores only from the exact saved size, §3.5), each resolving Malformed; and a
// begin_from that fails (the parent released underneath it, made by the Layer-1 status seam: a real
// release would leave the engine's own handle dangling). After each the ledger returns to its
// pre-admission value, the probe agrees (form 1), and the probe's exact form holds.
// Mutant: a failed restore that never refunds H (H stays 24 above its value).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuFailureRefundTest, "SuperSLM.Paging.Cpu.Ledger.FailureRefund",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuFailureRefundTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B1000;
	const FSuperSLMRuntimeConfig C = MakeConfig(2, 512, 2, 1);
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigureBudget(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FRegions Want = ExpectedRegions(Rig.Geo, C);
	bool bOk = true;
	auto Rebuild = [this, &Rig, &C]() { return ConfigureBudget(*this, Rig, C); };

	struct FCase { const TCHAR* Name; bool bAppend; };
	for (const FCase& Case : { FCase{ TEXT("one trailing byte"), true }, FCase{ TEXT("cut short past its header"), false } })
	{
		TArray<uint8> Bad = B1000;
		const bool bEdited = FSuperSLMPagedTestSeam::EditPayload(Bad, [&Case](TArray<uint8>& Payload)
		{
			if (Case.bAppend)
			{
				Payload.Add(0);
			}
			else
			{
				Payload.SetNum(FMath::Max(PayloadHeader::Ssb6HeaderEnd + 64, Payload.Num() - 1024));
			}
		});
		if (!TestTrue(*FString::Printf(TEXT("%s: the payload is edited"), Case.Name), bEdited))
		{
			return false;
		}
		const FSuperSLMPagedLedgerView Before = LedgerOf(Cpu);
		const FIssuedRestore R = IssueRestore(Cpu, Bad, Rig.Model);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the header is readable, so admission queues it (%s)"), Case.Name, *R.Error), (uint8)R.Sync, (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the engine refuses it after admission: Malformed"), Case.Name), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Malformed);
		Discard(Cpu, R);
		const FSuperSLMPagedLedgerView After = LedgerOf(Cpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the ledger returns to its pre-admission total"), Case.Name), After.TotalCharged, Before.TotalCharged);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: H is refunded whole"), Case.Name), After.ChargedOf(ERegion::H), Before.ChargedOf(ERegion::H));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("%s, after the refusal"), Case.Name));
		bOk &= ProbeDrawnExactly(*this, Cpu, Want.Pool(), Want.S, Rebuild, *FString::Printf(TEXT("%s, after the refusal"), Case.Name));
	}

	// The failed begin_from.
	{
		TArray<int32> ParentTokens, ChildTokens;
		FSuperSLMPrefix Parent, Child;
		if (!MakeTokens(*this, Cpu, 1000, 2, ParentTokens) || !MakeTokens(*this, Cpu, 200, 3, ChildTokens) || !BuildPrefix(*this, Cpu, ParentTokens, Parent))
		{
			return false;
		}
		const FSuperSLMPagedLedgerView Before = LedgerOf(Cpu);
		bOk &= TestTrue(TEXT("begin_from: the Layer-1 status seam is set"),
			FSuperSLMPagedTestSeam::SetNextLayer1StatusOverride(Cpu, ESuperSLMPagedRequestKind::NestedBuild, SSLM_INVALID_ARGUMENT));
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("begin_from: the child is admitted (%s)"), *Error), (uint8)Cpu.CreatePrefixFrom(Parent, ChildTokens, Child, &Error), (uint8)ESuperSLMPrefixResult::Success);
		// A failed begin_from reads Faulted ("a Layer-1 call rejected it; release it"), as a failed root
		// Begin does (plan rev 1.9, ruling 7).
		Drain(Cpu, [&Cpu, &Child]()
		{
			const ESuperSLMPrefixPhase Phase = Cpu.GetPrefixPhase(Child);
			return Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid || Phase == ESuperSLMPrefixPhase::Ready;
		}, OpSeconds);
		bOk &= TestEqual(TEXT("begin_from: the refused child is Faulted"), (uint8)Cpu.GetPrefixPhase(Child), (uint8)ESuperSLMPrefixPhase::Faulted);
		Cpu.ReleasePrefix(Child);
		DrainQuiet(Cpu);
		const FSuperSLMPagedLedgerView After = LedgerOf(Cpu);
		bOk &= TestEqual(TEXT("begin_from: P returns to its pre-admission value"), After.ChargedOf(ERegion::P), Before.ChargedOf(ERegion::P));
		bOk &= TestTrue(TEXT("begin_from: the parent is unpinned (it releases)"), Cpu.ReleasePrefix(Parent, Error));
		DrainQuiet(Cpu);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("begin_from, after the refusal"));
		bOk &= ProbeDrawnExactly(*this, Cpu, Want.Pool(), Want.S, Rebuild, TEXT("begin_from, after the refusal and the parent's release"));
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 1.6"));
	return bOk;
}

// Cell 3.1 (§3.3 "Concurrency"; §11's pins): ReleasePrefix() is refused while an adopt, a
// begin_from or a hinted restore of the prefix is queued (each held un-run by HoldStart), and
// succeeds after that job applies.
// Mutant: a pin absent (the release admitted while the job is queued, freeing what it will read).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuPinsTest, "SuperSLM.Paging.Cpu.Ledger.PinsRefuseRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuPinsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(3, 512, 3, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens, ChildTokens, Suffix;
	if (!MakeTokens(*this, Cpu, 1000, 1, Tokens) || !MakeTokens(*this, Cpu, 100, 2, ChildTokens) || !MakeTokens(*this, Cpu, 4, 3, Suffix))
	{
		return false;
	}
	bool bOk = true;
	auto PinLeg = [&](const TCHAR* Name, ESuperSLMPagedRequestKind Kind, TFunctionRef<bool(const FSuperSLMPrefix&)> Queue, TFunctionRef<bool()> Finish)
	{
		FSuperSLMPrefix Prefix;
		if (!BuildPrefix(*this, Cpu, Tokens, Prefix))
		{
			bOk = false;
			return;
		}
		const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldStart(Cpu, Kind);
		if (!TestTrue(*FString::Printf(TEXT("%s: HoldStart is armed"), Name), Hold.IsValid()) || !Queue(Prefix))
		{
			bOk = false;
			return;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the job reaches its held start"), Name), WaitHoldReached(Cpu, Hold));
		FString Error;
		bOk &= TestFalse(*FString::Printf(TEXT("%s: ReleasePrefix is refused while it is queued"), Name), Cpu.ReleasePrefix(Prefix, Error));
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the refusal is named (%s)"), Name, *Error), !Error.IsEmpty());
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the prefix is still Ready"), Name), (uint8)Cpu.GetPrefixPhase(Prefix), (uint8)ESuperSLMPrefixPhase::Ready);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the hold is released"), Name), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
		bOk &= Finish();
		FString After;
		bOk &= TestTrue(*FString::Printf(TEXT("%s: ReleasePrefix succeeds after the apply (%s)"), Name, *After), Cpu.ReleasePrefix(Prefix, After));
		DrainQuiet(Cpu);
	};

	FSuperSLMSequence Adopter;
	FSuperSLMLifecycleOpHandle AdoptHandle;
	PinLeg(TEXT("adopt"), ESuperSLMPagedRequestKind::Adopt,
		[&](const FSuperSLMPrefix& P) { FString E; return Vend(*this, Cpu, Adopter) && TestEqual(TEXT("AdoptPrefix queues"), (uint8)Cpu.AdoptPrefix(Adopter, P, AdoptHandle, E), (uint8)ESuperSLMRestoreResult::Success); },
		[&]() { return TestEqual(TEXT("the adopt succeeds"), (uint8)WaitOp(Cpu, AdoptHandle), (uint8)ESuperSLMRestoreResult::Success); });
	ReturnAndDrain(Cpu, Adopter);

	FSuperSLMPrefix Child;
	PinLeg(TEXT("begin_from"), ESuperSLMPagedRequestKind::NestedBuild,
		[&](const FSuperSLMPrefix& P) { FString E; return TestEqual(TEXT("CreatePrefixFrom is admitted"), (uint8)Cpu.CreatePrefixFrom(P, ChildTokens, Child, &E), (uint8)ESuperSLMPrefixResult::Success); },
		[&]() { return WaitPrefixReady(*this, Cpu, Child); });
	Cpu.ReleasePrefix(Child);
	DrainQuiet(Cpu);

	// The hinted restore's blob, from an adopter of the same tokens (so the hint can share).
	TArray<uint8> Blob;
	{
		FSuperSLMPrefix Source;
		if (!BuildPrefix(*this, Cpu, Tokens, Source) || !MakeAdoptedBlob(*this, Cpu, Source, Suffix, 2, Blob) || !ReleasePrefixAndDrain(*this, Cpu, Source))
		{
			return false;
		}
	}
	FIssuedRestore Hinted;
	PinLeg(TEXT("hinted restore"), ESuperSLMPagedRequestKind::Restore,
		[&](const FSuperSLMPrefix& P) { Hinted = IssueRestore(Cpu, Blob, Rig.Model, P); return TestEqual(TEXT("the hinted restore queues"), (uint8)Hinted.Sync, (uint8)ESuperSLMRestoreResult::Success); },
		[&]() { return TestEqual(TEXT("the hinted restore succeeds"), (uint8)WaitOp(Cpu, Hinted.Handle), (uint8)ESuperSLMRestoreResult::Success); });
	return bOk;
}

// Cell 3.5 (§3.2 "One job per slot"): admission at the head of the sequence's queue. The CPU vends a
// fresh sequence for every restore, so no restore can queue behind another on one slot; the cell's
// 4-deep queue is built on a restored sequence instead (the reading is in the K1 record): an 'SSB5'
// restore (held at its apply), then an adopt, a reset and an adopt queued behind it.
//   - nothing is refused at queue time;
//   - while the restore is held, the ledger shows only the restore's charge (P_blk on H), none for
//     the requests behind it;
//   - the first adopt is refused KvModeMismatch when it reaches the head (the holder is then a
//     whole-reserve holder, which only the head can know), and the requests behind it run: the
//     reset normalizes, and the second adopt succeeds.
// Mutants: admission at queue time (the first adopt sees the warm budget holder and is admitted;
// a charge appears early); the queue narrowed to 1 (the second call is refused SequenceQueueFull).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuOneJobPerSlotTest, "SuperSLM.Paging.Cpu.Ledger.OneJobPerSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuOneJobPerSlotTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Ssb5;
	if (!Rig.Init(*this) || !MakeSsb5Blob(*this, Rig, Ssb5) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, Cpu, 200, 1, Tokens) || !BuildPrefix(*this, Cpu, Tokens, Prefix))
	{
		return false;
	}
	const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
	const FIssuedRestore R = IssueRestore(Cpu, Ssb5, Rig.Model);
	if (!TestTrue(TEXT("HoldApply is armed"), Hold.IsValid()) || !TestEqual(TEXT("the 'SSB5' restore queues"), (uint8)R.Sync, (uint8)ESuperSLMRestoreResult::Success))
	{
		return false;
	}
	FSuperSLMLifecycleOpHandle Adopt1, Reset, Adopt2;
	FString E1, E2, E3;
	bool bOk = TestEqual(*FString::Printf(TEXT("queue 2: the adopt is not refused at queue time (%s)"), *E1), (uint8)Cpu.AdoptPrefix(R.Sequence, Prefix, Adopt1, E1), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(*FString::Printf(TEXT("queue 3: the reset is not refused at queue time (%s)"), *E2), (uint8)Cpu.ResetSequence(R.Sequence, Reset, E2), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(*FString::Printf(TEXT("queue 4: the adopt is not refused at queue time (%s)"), *E3), (uint8)Cpu.AdoptPrefix(R.Sequence, Prefix, Adopt2, E3), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestTrue(TEXT("the restore reaches its held apply"), WaitHoldReached(Cpu, Hold));
	bOk &= TestEqual(TEXT("only the head's charge is taken: H holds P_blk for the restore"), Charged(Cpu, ERegion::H), Rig.Geo.Block());
	bOk &= TestEqual(TEXT("the requests behind the head are pending"), (uint8)Cpu.GetLifecycleOpResult(Adopt1), (uint8)ESuperSLMRestoreResult::Pending);
	bOk &= TestTrue(TEXT("the hold is released"), FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold));
	bOk &= TestEqual(TEXT("the restore succeeds"), (uint8)WaitOp(Cpu, R.Handle), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the first adopt is refused KvModeMismatch at the head"), (uint8)WaitOp(Cpu, Adopt1), (uint8)ESuperSLMRestoreResult::KvModeMismatch);
	bOk &= TestEqual(TEXT("the reset behind it runs (normalization)"), (uint8)WaitOp(Cpu, Reset), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the second adopt runs and succeeds"), (uint8)WaitOp(Cpu, Adopt2), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("after normalization H is empty"), Charged(Cpu, ERegion::H), int64(0));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 3.5"));
	return bOk;
}

// Cell 11.2 (§3.7; the ledger-fault detector): under the zero-slack construction with one
// ForceUnderCharge set, the engine's KV_POOL_EXHAUSTED reaches one of this plan's own requests.
// One applied holder at origin 64 (hold 1) leaves H and the pool 63 free; ForceUnderCharge(
// WholeReserveRestore, 1) admits an 'SSB5' restore charged 63, whose draw of 64 the engine refuses.
// The outcome is KvPagesExhausted, LedgerFaults rises by exactly one, and the fault's log line
// appears (read as a warning or error containing "ledger fault"; the reading is in the K1 record;
// at least one such line, so a second line from the same fault does not turn the test red).
// Mutant: the counter increment removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuFaultDetectorTest, "SuperSLM.Paging.Cpu.Ledger.FaultDetector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuFaultDetectorTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> B64, Ssb5;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 64, 1, B64) || !MakeSsb5Blob(*this, Rig, Ssb5) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1))
		|| !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	FSuperSLMSequence Holder;
	if (!ApplyRestore(*this, Cpu, B64, Rig.Model, Holder))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("H has 63 free"), Free(Cpu, ERegion::H), int64(63));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("before the forced under-charge"));
	bOk &= TestTrue(TEXT("ForceUnderCharge(WholeReserveRestore, 1) is set"), FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::WholeReserveRestore, 1));
	AddExpectedError(TEXT("ledger fault"), EAutomationExpectedErrorFlags::Contains, 0);  // 0: at least once
	const FIssuedRestore R = IssueRestore(Cpu, Ssb5, Rig.Model);
	bOk &= TestEqual(TEXT("the under-charged restore is admitted"), (uint8)R.Sync, (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the engine's exhaustion resolves KvPagesExhausted"), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
	bOk &= TestEqual(TEXT("LedgerFaults rose by one (GetKvPageStats)"), int64(Cpu.GetKvPageStats().LedgerFaults), int64(1));
	bOk &= TestEqual(TEXT("LedgerFaults rose by one (Ledger seam)"), int64(LedgerOf(Cpu).LedgerFaults), int64(1));
	FSuperSLMPagedTestSeam::ForceUnderCharge(Cpu, ESuperSLMPagedChargeKind::WholeReserveRestore, 0);
	Discard(Cpu, R);
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after the fault: nothing was drawn"));
	return bOk;
}

// K6 code review C1, the CPU twin (no plan cell number yet; the GPU cell is
// SuperSLM.Paging.Gpu.Ledger.RecycleTwiceInWindow, in K5's file). It grades the CPU path for the same
// defect.
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
// Mutants: the recycle admitted at Return (a second admission on a slot with an open job is refused:
// a ledger fault, and the slot withheld until teardown); a slot with an open recycle skipped at vend
// (the second Vend refused PoolExhausted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuRecycleTwiceTest, "SuperSLM.Paging.Cpu.Ledger.RecycleTwiceInWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuRecycleTwiceTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	const FSuperSLMRuntimeConfig C = MakeConfig(1, 512, 0, 1);
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FRegions Want = ExpectedRegions(Rig.Geo, C);
	bool bOk = true;
	{
		// No tick, drain or pump between these four calls.
		FSuperSLMSequence First;
		if (!Vend(*this, Cpu, First))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("the first vend is slot 0"), FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, First), 0);
		Cpu.ReturnSequence(First);
		FSuperSLMSequence Seq;
		const ESuperSLMVendResult Second = Cpu.VendSequence(Seq);
		if (!TestEqual(TEXT("the second vend, inside the first recycle's window, succeeds (a slot only waiting on its recycle is not refused PoolExhausted)"),
				(uint8)Second, (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("the second vend is slot 0"), FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Seq), 0);
		Cpu.ReturnSequence(Seq);
	}
	bOk &= TestTrue(TEXT("the subsystem drains"), DrainQuiet(Cpu));
	bOk &= NoLedgerFault(*this, Cpu, TEXT("after two recycles in one window"));
	{
		FSuperSLMSequence Again;
		bOk &= TestEqual(TEXT("the slot is vended again afterwards (not withheld)"), (uint8)Cpu.VendSequence(Again), (uint8)ESuperSLMVendResult::Success);
		bOk &= TestEqual(TEXT("the vend afterwards is slot 0"), FSuperSLMPagedTestSeam::SlotIndexOf(Cpu, Again), 0);
		bOk &= TestTrue(TEXT("the slot is returned"), ReturnAndDrain(Cpu, Again));
	}
	bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("after two recycles in one window"));
	bOk &= TestEqual(TEXT("the ledger is the warm reservation alone"), LedgerOf(Cpu).TotalCharged, Want.S);
	bOk &= NoLedgerFault(*this, Cpu, TEXT("at the end"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
