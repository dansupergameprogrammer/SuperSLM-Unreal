// Plugin paging plan, CPU red suite (step K1): Configure, regions, pricing and legacy mode.
//
//   SuperSLM.Paging.Cpu.Configure.InvalidSequenceBudget   cell 5 (InvalidSequenceBudget), §3.1   [K2]
//   SuperSLM.Paging.Cpu.Configure.Regions                 §3.2 regions and pool; GetKvPageStats    [K2]
//   SuperSLM.Paging.Cpu.Configure.LifecyclePricing        cell 7.1                                [K2]
//   SuperSLM.Paging.Cpu.Configure.LegacyUnchanged         cell 8.1 (the CPU's structural legs)    [K2]
//   SuperSLM.Paging.Cpu.Configure.NestedRequiresBudget    cell 5 (NestedRequiresBudgetMode)       [K2]
//
// Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"

using namespace SuperSLMPagingFixtures;

// Cell 5, InvalidSequenceBudget (§3.1): SequenceBudget -1 and cap + 1 are refused by name, with
// nothing allocated; the bound itself, cap, is accepted.
// Mutants: the check absent (admits -1 or cap + 1); the bound one low (refuses cap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuInvalidSequenceBudgetTest, "SuperSLM.Paging.Cpu.Configure.InvalidSequenceBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuInvalidSequenceBudgetTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	const int64 Cap = ExpectedCap(Rig.Artifact);
	bool bOk = true;
	for (const int64 Bad : { int64(-1), Cap + 1 })
	{
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, MakeConfig(2, int32(Bad), 1, 1));
		bOk &= TestEqual(*FString::Printf(TEXT("SequenceBudget %lld is refused InvalidSequenceBudget (%s)"), Bad, *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::InvalidSequenceBudget);
		bOk &= TestTrue(TEXT("the refusal names SequenceBudget"), Report.Message.Contains(TEXT("SequenceBudget")));
		bOk &= TestEqual(TEXT("nothing is allocated: no KV pool"), Rig.Cpu->GetKvPoolReservedBytes(), int64(0));
		bOk &= TestFalse(TEXT("no paged mode after the refusal"), Rig.Cpu->GetKvPageStats().bPagedMode);
		FSuperSLMSequence Seq;
		bOk &= TestEqual(TEXT("the subsystem is unconfigured: VendSequence() is NotConfigured"), (uint8)Rig.Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::NotConfigured);
	}
	// The bound itself is admissible: SequenceBudget = cap, each warm holder reserving the whole cap.
	bOk &= ConfigureBudget(*this, Rig, MakeConfig(2, int32(Cap), 0, 1));
	return bOk;
}

// §3.2 regions and pool, read three ways: the ledger's regions against the plan's sizes
// (ConfigureBudget), GetKvPageStats() against the ledger, and the pool's drawn pages by the probe's
// exact two-sided form (only S is drawn after Configure: the warm holders). Three shapes.
// Mutants: a region sized at the legacy block count; the pool sized without H; warm sequences
// created by the no-budget verb (each draws a whole block: drawn = BlockCount x block, not S).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuRegionsTest, "SuperSLM.Paging.Cpu.Configure.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuRegionsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	struct FShape { int32 Blocks; int32 Budget; int32 PrefixBlocks; int32 HeadroomBlocks; };
	const int32 Cap = int32(ExpectedCap(Rig.Artifact));
	const FShape Shapes[] = { { 4, 512, 1, 1 }, { 3, 1, 2, 0 }, { 2, Cap, 0, 2 } };
	bool bOk = true;
	for (const FShape& Shape : Shapes)
	{
		const FSuperSLMRuntimeConfig Config = MakeConfig(Shape.Blocks, Shape.Budget, Shape.PrefixBlocks, Shape.HeadroomBlocks);
		if (!ConfigureBudget(*this, Rig, Config))
		{
			return false;
		}
		const FRegions Want = ExpectedRegions(Rig.Geo, Config);
		const FSuperSLMKvPageStats Stats = Rig.Cpu->GetKvPageStats();
		bOk &= TestEqual(TEXT("GetKvPageStats: B"), Stats.PagePositions, Rig.Geo.B);
		bOk &= TestEqual(TEXT("GetKvPageStats: page bytes"), Stats.PageBytes, Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("GetKvPageStats: pool"), Stats.PoolPages, Want.Pool());
		bOk &= TestEqual(TEXT("GetKvPageStats: S"), Stats.Sequences.CapacityPages, Want.S);
		bOk &= TestEqual(TEXT("GetKvPageStats: S charged"), Stats.Sequences.ChargedPages, Want.S);
		bOk &= TestEqual(TEXT("GetKvPageStats: P"), Stats.Prefixes.CapacityPages, Want.P);
		bOk &= TestEqual(TEXT("GetKvPageStats: H"), Stats.RestoreHeadroom.CapacityPages, Want.H);
		bOk &= TestEqual(TEXT("GetKvPageStats: free headroom"), Stats.FreeHeadroomPages, Want.H);
		bOk &= TestEqual(TEXT("GetKvPageStats: A"), Stats.CacheSharing.CapacityPages, int64(0));
		bOk &= TestEqual(TEXT("GetKvPageStats: C"), Stats.Cache.CapacityPages, int64(0));
		bOk &= TestEqual(TEXT("GetKvPageStats: no ledger fault"), int64(Stats.LedgerFaults), int64(0));
		auto Rebuild = [this, &Rig, &Config]() { return ConfigureBudget(*this, Rig, Config); };
		bOk &= ProbeDrawnExactly(*this, *Rig.Cpu, Want.Pool(), Want.S, Rebuild,
			*FString::Printf(TEXT("after Configure (BlockCount %d, budget %d): only the warm reservations are drawn"), Shape.Blocks, Shape.Budget));
	}
	return bOk;
}

// Cell 7.1 (§3.3 "Lifecycle pricing"): in budget mode a create or reset is priced at
// R(SequenceBudget) x page_bytes in place of the block size, for three shapes. Read through
// Configure()'s Sequence Lifecycle Budget refusal, which names the bytes it priced: with a budget
// of a millionth of a millisecond every shape is refused, and the message must name the budget
// reservation's bytes, computed here from the engine's page size.
// Mutant: block-size pricing kept in budget mode. Budgets 1 and 512 kill it. The budget = cap leg
// does not discriminate: R(cap) is the whole block, so both pricings name the same bytes; it stays
// as the bound's correctness leg.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuLifecyclePricingTest, "SuperSLM.Paging.Cpu.Configure.LifecyclePricing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuLifecyclePricingTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(2, 512, 0, 1)))
	{
		return false;
	}
	const FCpuPagingGeometry G = Rig.Geo;
	bool bOk = true;
	for (const int64 Budget : { int64(1), int64(512), G.Cap })
	{
		FSuperSLMRuntimeConfig Tight = MakeConfig(2, int32(Budget), 0, 1);
		Tight.SequenceLifecycleBudgetMs = 1.0e-6;
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Tight);
		const int64 PricedBytes = G.R(Budget) * G.PageBytes;
		bOk &= TestEqual(*FString::Printf(TEXT("budget %lld: refused by the lifecycle budget"), Budget),
			(uint8)Report.Result, (uint8)ESuperSLMConfigureResult::SequenceLifecycleBudgetExceeded);
		bOk &= TestTrue(*FString::Printf(TEXT("budget %lld: priced at R(%lld) = %lld pages x %lld bytes = %lld bytes (message: %s)"),
				Budget, Budget, G.R(Budget), G.PageBytes, PricedBytes, *Report.Message),
			Report.Message.Contains(FString::Printf(TEXT("%lld-byte"), PricedBytes)));
	}
	return bOk;
}

// Cell 8.1, the CPU's structural legacy legs. With SequenceBudget 0: no ledger and no
// page pool (GetKvPageStats().bPagedMode false, Ledger() unconfigured); the pool is the legacy
// block pool, so the engine's no-budget create admits exactly PrefixBlockCount further holders
// beside the BlockCount warm ones, at two pool sizes (the engine's legacy-create count), and refuses
// the next SSLM_KV_POOL_EXHAUSTED.
// Mutant: a ledger or page pool active in legacy mode.
// Not here: the output-identity leg against K0R's recording at the 1.11.0 pin (its fixture is K0R's
// box capture), and the budget-mode re-run of the existing suites (see the K1 record).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuLegacyUnchangedTest, "SuperSLM.Paging.Cpu.Configure.LegacyUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuLegacyUnchangedTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const TPair<int32, int32>& Shape : { TPair<int32, int32>(1, 2), TPair<int32, int32>(2, 5) })
	{
		if (!ConfigureLegacy(*this, Rig, MakeConfig(Shape.Key, 0, Shape.Value, 1)))
		{
			return false;
		}
		bOk &= TestFalse(TEXT("legacy mode has no page ledger (GetKvPageStats)"), Rig.Cpu->GetKvPageStats().bPagedMode);
		bOk &= TestFalse(TEXT("legacy mode has no page ledger (Ledger seam)"), FSuperSLMPagedTestSeam::Ledger(*Rig.Cpu).bConfigured);
		int32 Admitted = 0;
		sslm_status Last = SSLM_OK;
		const bool bRan = FSuperSLMPagedTestSeam::RunOnPoolThread(*Rig.Cpu, [&Admitted, &Last](FSuperSLMPoolThreadAccess& A)
		{
			for (int32 I = 0; I < 64; ++I)
			{
				Last = A.CreateWholeReserve();
				if (Last != SSLM_OK)
				{
					break;
				}
				++Admitted;
			}
			A.ReleaseAll();
		});
		bOk &= TestTrue(TEXT("RunOnPoolThread runs on the legacy pool"), bRan);
		bOk &= TestEqual(*FString::Printf(TEXT("BlockCount %d + PrefixBlockCount %d: the legacy pool admits PrefixBlockCount further whole-block holders"), Shape.Key, Shape.Value),
			Admitted, Shape.Value);
		bOk &= TestEqual(TEXT("and refuses the next SSLM_KV_POOL_EXHAUSTED"), (int32)Last, (int32)SSLM_KV_POOL_EXHAUSTED);
	}
	return bOk;
}

// Cell 5, NestedRequiresBudgetMode (§3.3, UP-9): CreatePrefixFrom() in legacy mode is refused by
// name, and nothing is created.
// Mutant: nesting admitted in legacy mode (a budget sharer in a legacy pool, which breaks E4).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNestedRequiresBudgetTest, "SuperSLM.Paging.Cpu.Configure.NestedRequiresBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuNestedRequiresBudgetTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 2, 1)))
	{
		return false;
	}
	TArray<int32> ParentTokens, ChildTokens;
	FSuperSLMPrefix Parent;
	FString Error;
	if (!MakeTokens(*this, *Rig.Cpu, 40, 0, ParentTokens) || !MakeTokens(*this, *Rig.Cpu, 8, 1, ChildTokens))
	{
		return false;
	}
	const bool bCreated = Rig.Cpu->CreatePrefix(ParentTokens, Parent, Error);
	if (!TestTrue(*FString::Printf(TEXT("a legacy root prefix (%s)"), *Error), bCreated) || !WaitPrefixReady(*this, *Rig.Cpu, Parent))
	{
		return false;
	}
	const int32 LiveBefore = Rig.Cpu->GetLivePrefixCount();
	FSuperSLMPrefix Child;
	const ESuperSLMPrefixResult Result = Rig.Cpu->CreatePrefixFrom(Parent, ChildTokens, Child, &Error);
	bool bOk = TestEqual(*FString::Printf(TEXT("CreatePrefixFrom() in legacy mode is NestedRequiresBudgetMode (%s)"), *Error),
		(uint8)Result, (uint8)ESuperSLMPrefixResult::NestedRequiresBudgetMode);
	bOk &= TestFalse(TEXT("no child handle"), Child.IsValid());
	bOk &= TestEqual(TEXT("no prefix is created"), Rig.Cpu->GetLivePrefixCount(), LiveBefore);
	Rig.Cpu->ReleasePrefix(Parent);
	DrainQuiet(*Rig.Cpu);
	return bOk;
}

// Invalid geometry must retain a complete diagnostic after temporary-state cleanup and leave
// no configured runtime. The static lifetime audit separately pins read-before-destruction;
// these native checks exercise refusal, mapping cleanup and subsequent successful recovery.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuInvalidGeometryDiagnosticTest,
	"SuperSLM.Paging.Cpu.Configure.InvalidGeometryDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuInvalidGeometryDiagnosticTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(1, 512, 1, 1))) { return false; }
	struct FInvalidCounts { int32 Headroom; int32 Ghost; };
	const FInvalidCounts Invalid[] = { { -1, 0 }, { 1, -1 }, { MAX_int32, 0 } };
	bool bOk = true;
	for (const FInvalidCounts& Counts : Invalid)
	{
		FSuperSLMRuntimeConfig Config = MakeConfig(1, 512, 1, Counts.Headroom);
		Config.CacheGhostBlocks = Counts.Ghost;
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, Config);
		bOk &= TestEqual(TEXT("invalid region counts refuse by InvalidCallShape"),
			uint8(Report.Result), uint8(ESuperSLMConfigureResult::InvalidCallShape));
		bOk &= TestTrue(TEXT("diagnostic retains the model's actual page geometry"),
			Report.Message.Contains(FString::Printf(TEXT("B %lld, page %llu bytes, context_cap %lld"),
				Rig.Geo.B, uint64(Rig.Geo.PageBytes), Rig.Geo.Cap)));
		bOk &= TestTrue(TEXT("diagnostic retains the exact invalid region counts"),
			Report.Message.Contains(FString::Printf(TEXT("RestoreHeadroomBlocks %d, CacheGhostBlocks %d"),
				Counts.Headroom, Counts.Ghost)));
		bOk &= TestTrue(TEXT("diagnostic states the existing page-domain bound"),
			Report.Message.Contains(TEXT("2^32 - 1 pages")));
		bOk &= TestNull(TEXT("refusal installs no runtime state"), Rig.Cpu->GetResolvedConfig());
		bOk &= TestFalse(TEXT("refusal leaves no active page pool"), Rig.Cpu->GetKvPageStats().bPagedMode);
		if (!ConfigureBudget(*this, Rig, MakeConfig(1, 512, 1, 1))) { return false; }
		bOk &= TestEqual(TEXT("recovery has no ledger faults"), Rig.Cpu->GetKvPageStats().LedgerFaults, uint64(0));
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
