// Plugin paging plan, GPU red suite (step K5): Configure, regions, the Configure refusals, dedicated
// mode, and the Configure guards (§3.1, §3.2, §3.4; §7 cells 5, 8.1, 11).
//
//   SuperSLM.Paging.Gpu.Configure.PooledRegions               §3.2 regions and pool at 0.5B                  [K6]
//   SuperSLM.Paging.Gpu.Configure.DedicatedUnchanged          cell 8.1 (GPU structural legs); §5 UnsupportedOnGpu [K6/K7]
//   SuperSLM.Paging.Gpu.Configure.InvalidSequenceBudget       cell 5 (InvalidSequenceBudget)                  [K6]
//   SuperSLM.Paging.Gpu.Configure.TwoCap.InvalidSequenceBudget cell 5 [two-cap] leg                          [K6]  (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Configure.KvPoolTooLarge              cell 5 KvPoolRefused, deterministic, 0.5B       [K6]
//   SuperSLM.Paging.Gpu.Configure.TwoCap.KvPoolTooLarge       cell 5 KvPoolRefused, deterministic, 1.5B       [K6]  (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Configure.PoolAllocationFailedMaps    cell 5 KvPoolRefused, ALLOCATION_FAILED mapping [K6]
//   SuperSLM.Paging.Gpu.Configure.AllocationCeiling           cell 5 KvPoolRefused at the operative ceiling   [K8]  (Stress, box only)
//   SuperSLM.Paging.Gpu.Configure.GeometryCrossCheck          cell 5 geometry cross-check, both legs; §11     [K6]
//   SuperSLM.Paging.Gpu.Configure.KFloorPlusOne               §11 the K-floor +1                              [K7]
//   SuperSLM.Paging.Gpu.Configure.DedicatedWarning            §11 the dedicated-mode warning (UP-4)           [K6]
//   SuperSLM.Paging.Gpu.Configure.DeclaredResidency           cell 10.7's declared-figure leg (rev 1.10)      [K6]
//
// Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// A refused Configure() leaves nothing configured: no paged mode, no ledger, and no sequence to vend.
	bool ExpectUnconfigured(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* What)
	{
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: no paged mode after the refusal"), What), Gpu.GetKvPageStats().bPagedMode);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no ledger after the refusal"), What), FSuperSLMPagedGpuTestSeam::Ledger(Gpu).bConfigured);
		FSuperSLMGpuSequence Seq;
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: VendSequence() is NotConfigured"), What), (uint8)Gpu.VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall),
			(uint8)ESuperSLMGpuVendResult::NotConfigured);
		return bOk;
	}

	bool ExpectKvPoolRefused(FAutomationTestBase& T, FGpuPagingRig& Rig, const FSuperSLMGpuRuntimeConfig& C, const TCHAR* Status, int64 Pages, const TCHAR* What)
	{
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, C);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: Configure() is refused KvPoolRefused (%s)"), What, *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::KvPoolRefused);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the message names the status %s"), What, Status), Report.Message.Contains(Status));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the message names the pages, %lld"), What, Pages), Report.Message.Contains(FString::Printf(TEXT("%lld"), Pages)));
		bOk &= ExpectUnconfigured(T, *Rig.Gpu, What);
		return bOk;
	}
}

// §3.2 and §3.4: the pooled regions and pool, read three ways -- the ledger's regions against the
// plan's sizes (ConfigurePooled), GetKvPageStats() against the ledger, and the pool's drawn pages by
// the probe's exact two-sided form (after Configure only the warm reservations, S, are drawn).
// Three shapes at 0.5B, where P_blk = H_blk = 256 (one cap).
// Mutants: a region sized other than §3.2 says; the pool sized without H; warm sequences created by
// the dedicated verb (each takes a ContextCap buffer and draws nothing from the pool: drawn = 0, not S).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPooledRegionsTest, "SuperSLM.Paging.Gpu.Configure.PooledRegions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuPooledRegionsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	struct FShape { int32 Blocks; int32 Budget; int32 PrefixBlocks; int32 HeadroomBlocks; };
	const FShape Shapes[] = { { 2, 512, 1, 1 }, { 3, 1, 2, 0 }, { 2, 4096, 0, 2 } };
	bool bOk = true;
	for (const FShape& Shape : Shapes)
	{
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, Shape.Blocks, Shape.Budget, Shape.PrefixBlocks, Shape.HeadroomBlocks);
		if (!ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
		{
			return false;
		}
		const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
		const FSuperSLMKvPageStats Stats = Rig.Gpu->GetKvPageStats();
		bOk &= TestEqual(TEXT("GetKvPageStats: B"), Stats.PagePositions, Rig.Geo.B);
		bOk &= TestEqual(TEXT("GetKvPageStats: page bytes"), Stats.PageBytes, Rig.Geo.PageBytes);
		bOk &= TestEqual(TEXT("GetKvPageStats: pool pages"), Stats.PoolPages, Want.Pool());
		bOk &= TestEqual(TEXT("GetKvPageStats: S"), Stats.Sequences.CapacityPages, Want.S);
		bOk &= TestEqual(TEXT("GetKvPageStats: P"), Stats.Prefixes.CapacityPages, Want.P);
		bOk &= TestEqual(TEXT("GetKvPageStats: H"), Stats.RestoreHeadroom.CapacityPages, Want.H);
		bOk &= TestEqual(TEXT("GetKvPageStats: free headroom is all of H"), Stats.FreeHeadroomPages, Want.H);
		bOk &= TestEqual(TEXT("GetKvPageStats: no ledger fault"), int64(Stats.LedgerFaults), int64(0));
		auto Rebuild = [&]() { return ConfigurePooled(*this, Rig, C); };
		bOk &= ProbeDrawnExactly(*this, *Rig.Gpu, Want.Pool(), Want.S, Rebuild,
			*FString::Printf(TEXT("shape (%d slots, budget %d, %d P, %d H): only the warm reservations are drawn"), Shape.Blocks, Shape.Budget, Shape.PrefixBlocks, Shape.HeadroomBlocks));
	}
	return bOk;
}

// Cell 8.1, the GPU's structural legs (audit G9), and §5's UnsupportedOnGpu on the GPU: a dedicated
// subsystem (SequenceBudget 0) has no page pool and no ledger, refuses every prefix verb by name, and
// still generates and saves 'SLM5' as before. Plan rev 1.9 ruling 2: both CreatePrefix overloads and
// CreatePrefixFrom return UnsupportedOnGpu before any other check, with a reason naming SequenceBudget.
// "Before any other check" is shown by inputs that would fail a later check: a parent id that names
// no prefix, and a schema handle that names no schema.
// The dedicated rows of the restore mapping are SuperSLM.U1.Gpu.RestoreStatusMappingByMode's. The
// output-identity legs against the K0R recording run on the box (the K5 record, §4).
// Mutants: the ledger or the pool active in dedicated mode; a dedicated CreatePrefix that builds;
// CreatePrefixFrom answering the CPU's NestedRequiresBudgetMode, or ParentNotUsable (the parent checked
// before the mode); the schema overload checking its schema before the mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuDedicatedUnchangedTest, "SuperSLM.Paging.Gpu.Configure.DedicatedUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuDedicatedUnchangedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureDedicated(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 0, 1, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	bool bOk = TestFalse(TEXT("dedicated: GetKvPageStats().bPagedMode is false"), Gpu.GetKvPageStats().bPagedMode);
	bOk &= TestEqual(TEXT("dedicated: GetKvPageStats() pool pages 0"), Gpu.GetKvPageStats().PoolPages, int64(0));
	bOk &= TestFalse(TEXT("dedicated: no ledger"), FSuperSLMPagedGpuTestSeam::Ledger(Gpu).bConfigured);
	bOk &= TestFalse(TEXT("dedicated: no pool to probe (RunOnPoolThread refuses)"), FSuperSLMPagedGpuTestSeam::RunOnPoolThread(Gpu, [](FSuperSLMGpuPoolThreadAccess&) {}));

	TArray<int32> Tokens;
	if (!MakeTokens(*this, *Rig.Model, 32, 1, Tokens))
	{
		return false;
	}
	// Each prefix verb: UnsupportedOnGpu, no handle, and a reason naming SequenceBudget.
	auto ExpectUnsupported = [this](ESuperSLMPrefixResult Result, const FSuperSLMPrefix& Out, const FString& Reason, const TCHAR* Verb)
	{
		bool bVerb = TestEqual(*FString::Printf(TEXT("dedicated: %s is refused UnsupportedOnGpu (%s)"), Verb, *Reason), (uint8)Result, (uint8)ESuperSLMPrefixResult::UnsupportedOnGpu);
		bVerb &= TestFalse(*FString::Printf(TEXT("dedicated: %s gives no prefix handle"), Verb), Out.IsValid());
		bVerb &= TestTrue(*FString::Printf(TEXT("dedicated: %s's reason names SequenceBudget (%s)"), Verb, *Reason), Reason.Contains(TEXT("SequenceBudget")));
		return bVerb;
	};
	{
		FSuperSLMPrefix Prefix;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(Tokens, Prefix, &Error);
		bOk &= ExpectUnsupported(Result, Prefix, Error, TEXT("CreatePrefix"));
	}
	{
		FSuperSLMPrefix Prefix;
		FString Error;
		const FSuperSLMGpuSchemaHandle NoSchema{}; // names no schema: a schema check made first would refuse it otherwise
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(Tokens, NoSchema, ESuperSLMSpanKind::SchemaContent, Prefix, &Error);
		bOk &= ExpectUnsupported(Result, Prefix, Error, TEXT("CreatePrefix (schema overload)"));
	}
	FSuperSLMPrefix Foreign;
	Foreign.Id = 1; // names no prefix here: a parent check made first would answer ParentNotUsable
	{
		FSuperSLMPrefix Child;
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefixFrom(Foreign, Tokens, Child, &Error);
		bOk &= ExpectUnsupported(Result, Child, Error, TEXT("CreatePrefixFrom"));
	}
	bOk &= TestEqual(TEXT("dedicated: no live prefix"), Gpu.GetLivePrefixCount(), 0);

	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}
	{
		const FSuperSLMLifecycleOpHandle H = Gpu.RequestAdoptPrefix(Seq);
		bOk &= TestEqual(TEXT("dedicated: the no-prefix adopt overload resolves UnsupportedOnGpu (unchanged)"), (uint8)WaitOp(Gpu, H), (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);
		Gpu.ReleaseLifecycleOpHandle(H);
	}
	{
		const FSuperSLMLifecycleOpHandle H = Gpu.RequestAdoptPrefix(Seq, Foreign);
		const ESuperSLMRestoreResult R = WaitOp(Gpu, H);
		const FString Reason = Gpu.GetLastLifecycleRequestError();
		bOk &= TestEqual(TEXT("dedicated: the prefix adopt overload resolves UnsupportedOnGpu"), (uint8)R, (uint8)ESuperSLMRestoreResult::UnsupportedOnGpu);
		bOk &= TestTrue(*FString::Printf(TEXT("dedicated: with the reason (%s)"), *Reason), Reason.Contains(TEXT("SequenceBudget")) || Reason.Contains(TEXT("pooled")) || Reason.Contains(TEXT("dedicated")));
		Gpu.ReleaseLifecycleOpHandle(H);
	}
	TArray<int32> Prompt, Out;
	TArray<uint8> Blob;
	FGpuPayloadHeaderFields Header;
	bOk &= MakeTokens(*this, *Rig.Model, 8, 2, Prompt) && GenerateComplete(*this, Gpu, Seq, Prompt, 4, Out);
	bOk &= SaveAndTake(*this, Gpu, Seq, Blob) && ReadGpuHeader(*this, Blob, Header);
	bOk &= TestFalse(TEXT("dedicated: the save is 'SLM5', not 'SLM6'"), Header.bSlm6);
	ReturnAndDrain(Gpu, Seq);
	return bOk;
}

// Cell 5, InvalidSequenceBudget (§3.1): SequenceBudget -1 and ContextCap + 1 are refused by name, with
// nothing configured; ContextCap itself is accepted.
// Mutants: the check absent; the bound one low (refuses ContextCap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuInvalidSequenceBudgetTest, "SuperSLM.Paging.Gpu.Configure.InvalidSequenceBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuInvalidSequenceBudgetTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const int32 Bad : { -1, int32(PluginCap) + 1 })
	{
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, MakeGpuConfig(Rig.Artifact, 2, Bad, 1, 1));
		bOk &= TestEqual(*FString::Printf(TEXT("SequenceBudget %d is refused InvalidSequenceBudget (%s)"), Bad, *Report.Message),
			(uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::InvalidSequenceBudget);
		bOk &= TestTrue(TEXT("the refusal names SequenceBudget"), Report.Message.Contains(TEXT("SequenceBudget")));
		bOk &= ExpectUnconfigured(*this, *Rig.Gpu, TEXT("after InvalidSequenceBudget"));
	}
	bOk &= ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, int32(PluginCap), 0, 1));
	return bOk;
}

// Cell 5, the [two-cap] leg (§3.1, UP-15): at the 1.5B artifact with ContextCap 4,096, SequenceBudget
// 4,097 is refused InvalidSequenceBudget although the model's cap (32,768) would admit it; 4,096 is
// accepted, each warm holder reserving R(4,096) = 257 pages at the model's cap.
// Mutant: the bound at the model's cap, which admits 4,097.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapInvalidSequenceBudgetTest, "SuperSLM.Paging.Gpu.Configure.TwoCap.InvalidSequenceBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapInvalidSequenceBudgetTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B))
	{
		return false;
	}
	const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, MakeGpuConfig(Rig.Artifact, 2, 4097, 0, 1));
	bool bOk = TestEqual(*FString::Printf(TEXT("[two-cap] SequenceBudget 4,097 is refused InvalidSequenceBudget (%s)"), *Report.Message),
		(uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::InvalidSequenceBudget);
	bOk &= ExpectUnconfigured(*this, *Rig.Gpu, TEXT("[two-cap] after InvalidSequenceBudget"));
	if (!ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 4096, 0, 1)) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("[two-cap] R(4,096) = 257 at the model's cap"), Rig.Geo.R(4096), int64(257));
	return bOk;
}

// Cell 5, KvPoolRefused, the deterministic leg at 0.5B (audit G7; plan review F5): a pool of 21,846
// pages of 96 KiB, one page past 2 GiB, the largest pool cap any adapter has (D3D12's required
// single-resource size; below 2 GiB on an adapter reporting less than 8 GiB of video memory), is refused
// by the engine KV_POOL_TOO_LARGE on every adapter. Built as 114 slots at SequenceBudget 32 (R = 3:
// 342 pages) plus 84 H blocks of 256 (21,504). The figure is the plan's, at B = 16, stated here, not
// read from the plugin.
// Mutant: the pool-create status not checked (Configure succeeds with no pool, or crashes later).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuKvPoolTooLargeTest, "SuperSLM.Paging.Gpu.Configure.KvPoolTooLarge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuKvPoolTooLargeTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 114, 32, 0, 84);
	return ExpectKvPoolRefused(*this, Rig, C, TEXT("SSLM_GPU_KV_POOL_TOO_LARGE"), 21846, TEXT("21,846 pages at 0.5B"));
}

// Cell 5, KvPoolRefused, the deterministic leg at 1.5B: 9,363 pages of 224 KiB (one page past 2 GiB)
// in the [two-cap] configuration: 1 slot at SequenceBudget 2,048 (R = 129) plus 18 H blocks of
// H_blk = 513 (9,234).
// Mutant: as the 0.5B leg; and H blocks of ceil(ContextCap / B) = 256 (4,737 pages, admitted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapKvPoolTooLargeTest, "SuperSLM.Paging.Gpu.Configure.TwoCap.KvPoolTooLarge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapKvPoolTooLargeTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 1, 2048, 0, 18);
	return ExpectKvPoolRefused(*this, Rig, C, TEXT("SSLM_GPU_KV_POOL_TOO_LARGE"), 9363, TEXT("9,363 pages at 1.5B [two-cap]"));
}

// Cell 5, KvPoolRefused from ALLOCATION_FAILED at pool create (§3.7), through the pool-create seam:
// no adapter fails an allocation on demand, so the mapping and the clean refusal are graded here and
// the real ceiling on the box (Configure.AllocationCeiling). The seam is spent by that Configure, so
// the next one succeeds.
// Mutant: ALLOCATION_FAILED at pool create mapped to DeviceUnavailable, or the status not named.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuPoolAllocationFailedMapsTest, "SuperSLM.Paging.Gpu.Configure.PoolAllocationFailedMaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuPoolAllocationFailedMapsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1);
	bool bOk = TestTrue(TEXT("the pool-create seam arms the next Configure()"),
		FSuperSLMPagedGpuTestSeam::SetNextPoolCreateStatusOverride(*Rig.Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED));
	bOk &= ExpectKvPoolRefused(*this, Rig, C, TEXT("SSLM_GPU_ALLOCATION_FAILED"), 66 + 256 + 256, TEXT("ALLOCATION_FAILED at pool create"));
	bOk &= ConfigurePooled(*this, Rig, C);
	return bOk;
}

// Cell 5, KvPoolRefused at the operative ALLOCATION_FAILED ceiling PKV G3's bisection records for each
// GPU. Box only: the figure is that record's, per adapter, given as SUPERSLM_PAGING_GPU_ALLOC_CEILING_PAGES
// (the smallest pool, in pages, the adapter refuses), or "unreachable" for an adapter that allocates
// every pool below KV_POOL_TOO_LARGE, which is then recorded. The pool built is the smallest whole
// number of 0.5B H blocks at or above the ceiling, beside one slot at SequenceBudget 4,096.
// Every leg first configures a small pool (2 slots at budget 512, 1 H block), so the test cannot pass
// before pooled mode exists, the "unreachable" leg included (the K5 coverage audit, N2).
// Unset, it fails by name: the box must supply the record.
// Mutant: ALLOCATION_FAILED at pool create not mapped to KvPoolRefused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuAllocationCeilingTest, "SuperSLM.Paging.Gpu.Configure.AllocationCeiling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuAllocationCeilingTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1)))
	{
		return false;
	}
	const FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PAGING_GPU_ALLOC_CEILING_PAGES"));
	if (Value.IsEmpty())
	{
		AddError(TEXT("box only: set SUPERSLM_PAGING_GPU_ALLOC_CEILING_PAGES to this adapter's ceiling from PKV G3's bisection record, or to 'unreachable'"));
		return false;
	}
	if (Value.Equals(TEXT("unreachable"), ESearchCase::IgnoreCase))
	{
		AddInfo(TEXT("recorded: this adapter allocates every pool below KV_POOL_TOO_LARGE; the ALLOCATION_FAILED ceiling is unreachable here"));
		return true;
	}
	const int64 Ceiling = FCString::Atoi64(*Value);
	if (!TestTrue(TEXT("the ceiling is a page count"), Ceiling > 256))
	{
		return false;
	}
	const int32 HBlocks = int32(FGpuGeometry::CeilDiv(Ceiling - 256, 256));
	const int64 Pages = 256 + int64(HBlocks) * 256;
	AddInfo(FString::Printf(TEXT("ceiling %lld pages: pool of %lld pages (1 slot at budget 4,096, %d H blocks)"), Ceiling, Pages, HBlocks));
	return ExpectKvPoolRefused(*this, Rig, MakeGpuConfig(Rig.Artifact, 1, 4096, 0, HBlocks), TEXT("SSLM_GPU_ALLOCATION_FAILED"), Pages, TEXT("at the operative ceiling"));
}

// Cell 5, the geometry cross-check (§3.4; coverage audit G6), and §11's guard for it. Leg 1: a seam
// perturbs the page count the plugin computed by one; Configure destroys the pool and resolves
// KvPoolRefused, naming both figures. Leg 2: ContextCap 4,090 at 0.5B (two caps: the model's is
// 4,096), where the geometry computed at the model's cap (B = 16) matches the pool and Configure
// succeeds, with P_blk = H_blk = 256 still.
// Mutants: the cross-check skipped (leg 1 configures); geometry computed at ContextCap, which gives
// B = 4,090 (16 does not divide it) and is refused (leg 2).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuGeometryCrossCheckTest, "SuperSLM.Paging.Gpu.Configure.GeometryCrossCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuGeometryCrossCheckTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1);
	bool bOk = TestTrue(TEXT("leg 1: the page-count perturbation arms the next Configure()"),
		FSuperSLMPagedGpuTestSeam::SetNextConfigurePageCountPerturbation(*Rig.Gpu, 1));
	{
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, C);
		bOk &= TestEqual(*FString::Printf(TEXT("leg 1: refused KvPoolRefused (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::KvPoolRefused);
		bOk &= TestTrue(TEXT("leg 1: the message names the pool's 578 pages"), Report.Message.Contains(TEXT("578")));
		bOk &= TestTrue(TEXT("leg 1: and the computed 579"), Report.Message.Contains(TEXT("579")));
		bOk &= ExpectUnconfigured(*this, *Rig.Gpu, TEXT("leg 1"));
	}
	const FSuperSLMGpuRuntimeConfig Two = MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1, 4090);
	if (!ConfigurePooled(*this, Rig, Two))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("leg 2: B = 16, from the model's cap"), Rig.Geo.B, int64(16));
	bOk &= TestEqual(TEXT("leg 2: P_blk = min(ceil(4,090/16) + 1, 256) = 256"), Rig.Geo.Pblk(), int64(256));
	bOk &= TestEqual(TEXT("leg 2: H_blk = 256"), Rig.Geo.Hblk(), int64(256));
	return bOk;
}

// §11, the K-floor +1 (§3.4 UP-11): in pooled mode with PrefixBlockCount > 0 the one-call term of
// MinimumK becomes BlockCount + 1 (the build seat). At DispatchBudget = one whole token, BlockCount 2
// and one prefix block: K = 2 is refused KBelowMinimum with MinimumK 3, and K = 3 is accepted. Without
// prefix blocks, and in dedicated mode (PrefixBlockCount ignored), K = 2 is accepted.
// Mutant: the +1 omitted (K = 2 accepted with a prefix block).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuKFloorPlusOneTest, "SuperSLM.Paging.Gpu.Configure.KFloorPlusOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuKFloorPlusOneTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1);
	C.K = 2;
	const FSuperSLMGpuConfigureReport Below = Rig.Gpu->Configure(Rig.Model, C);
	bool bOk = TestEqual(*FString::Printf(TEXT("pooled with a prefix block: K = BlockCount is refused KBelowMinimum (%s)"), *Below.Message),
		(uint8)Below.Result, (uint8)ESuperSLMGpuConfigureResult::KBelowMinimum);
	bOk &= TestEqual(TEXT("MinimumK = BlockCount + 1"), Below.MinimumK, 3);
	C.K = 3;
	bOk &= ConfigurePooled(*this, Rig, C);
	FSuperSLMGpuRuntimeConfig NoPrefix = MakeGpuConfig(Rig.Artifact, 2, 512, 0, 1);
	NoPrefix.K = 2;
	bOk &= ConfigurePooled(*this, Rig, NoPrefix);
	FSuperSLMGpuRuntimeConfig Dedicated = MakeGpuConfig(Rig.Artifact, 2, 0, 1, 1);
	Dedicated.K = 2;
	bOk &= ConfigureDedicated(*this, Rig, Dedicated);
	return bOk;
}

// §11, the dedicated-mode warning (§3.1, UP-4): PrefixBlockCount > 0 in dedicated mode is ignored
// with a logged warning, asserted by its tokens ("PrefixBlockCount" and "ignored" on one line; the K5
// reading). The same field in pooled mode logs no such line.
// Mutant: the warning removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuDedicatedWarningTest, "SuperSLM.Paging.Gpu.Configure.DedicatedWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuDedicatedWarningTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	{
		FGpuPagingLogCapture Log;
		AddExpectedError(TEXT("PrefixBlockCount"), EAutomationExpectedErrorFlags::Contains, 0);
		bOk &= ConfigureDedicated(*this, Rig, MakeGpuConfig(Rig.Artifact, 1, 0, 2, 1));
		bOk &= TestTrue(TEXT("dedicated: a line names PrefixBlockCount as ignored"), Log.CountContainingBoth(TEXT("PrefixBlockCount"), TEXT("ignored")) > 0);
	}
	{
		FGpuPagingLogCapture Log;
		bOk &= ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 1, 512, 2, 1));
		bOk &= TestEqual(TEXT("pooled: no line names PrefixBlockCount as ignored"), Log.CountContainingBoth(TEXT("PrefixBlockCount"), TEXT("ignored")), 0);
	}
	return bOk;
}

// Cell 10.7's [K6] leg, the declared figure (plan rev 1.10; K7 code review S2; §3.4 "Residency and
// VRAM"): in pooled mode GetDeclaredGpuResidencyBytes() equals the model's declared buffers plus
// PoolPages x PageBytes, exactly, with both read from GetKvPageStats(). The host mirror is system RAM and
// is not counted.
// No accessor exposes the model's declared buffers on their own, so the cell reads them by difference.
// Two pooled shapes on the same model have different pools: (2 slots, budget 512, 1 P block, 1 H block)
// and (4 slots, budget 512, 2 P blocks, 2 H blocks). No adapter is mapped, and the device head's state is
// the same in both. In each shape, Declared - PoolPages x PageBytes is the same positive figure (the
// model's buffers, which do not depend on the pool). Equivalently, the change in Declared between the two
// shapes is exactly the change in PoolPages x PageBytes.
// Mutant: the host mirror counted. Declared would then carry 2 x PoolPages x PageBytes, and its change
// between the shapes would be twice the pool's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuDeclaredResidencyTest, "SuperSLM.Paging.Gpu.Configure.DeclaredResidency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuDeclaredResidencyTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	struct FReading
	{
		int64 Declared = 0;
		int64 PoolBytes = 0;
		bool bHead = false;
	};
	auto Read = [this, &Rig](const FSuperSLMGpuRuntimeConfig& Shape, const TCHAR* What, FReading& Out)
	{
		if (!ConfigurePooled(*this, Rig, Shape))
		{
			return false;
		}
		const USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		const FSuperSLMKvPageStats Stats = Gpu.GetKvPageStats();
		bool bRead = TestTrue(*FString::Printf(TEXT("%s: pooled"), What), Stats.bPagedMode);
		bRead &= TestTrue(*FString::Printf(TEXT("%s: the page stats carry the pool (%lld pages of %lld bytes)"), What, Stats.PoolPages, Stats.PageBytes),
			Stats.PoolPages > 0 && Stats.PageBytes > 0);
		bRead &= TestEqual(*FString::Printf(TEXT("%s: no adapter is mapped"), What), Gpu.GetMappedAdapterCount(), 0);
		Out.Declared = Gpu.GetDeclaredGpuResidencyBytes();
		Out.PoolBytes = Stats.PoolPages * Stats.PageBytes;
		Out.bHead = Gpu.IsDeviceHeadActive();
		AddInfo(FString::Printf(TEXT("%s: declared %lld bytes; pool %lld bytes; declared less the pool %lld"), What, Out.Declared, Out.PoolBytes, Out.Declared - Out.PoolBytes));
		bRead &= TestTrue(*FString::Printf(TEXT("%s: the declared figure less the pool's device bytes (the model's buffers) is positive"), What), Out.Declared - Out.PoolBytes > 0);
		return bRead;
	};
	FReading Small, Large;
	if (!Read(MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1), TEXT("(2 slots, 1 P, 1 H)"), Small) || !Read(MakeGpuConfig(Rig.Artifact, 4, 512, 2, 2), TEXT("(4 slots, 2 P, 2 H)"), Large))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the two shapes' pools differ"), Large.PoolBytes > Small.PoolBytes);
	bOk &= TestEqual(TEXT("the device head's state is the same in both shapes"), Large.bHead, Small.bHead);
	bOk &= TestEqual(TEXT("declared less PoolPages x PageBytes is the same in both shapes (the model's buffers; the mirror is not counted)"),
		Large.Declared - Large.PoolBytes, Small.Declared - Small.PoolBytes);
	bOk &= TestEqual(TEXT("the declared figure moves by exactly the pool's device bytes"), Large.Declared - Small.Declared, Large.PoolBytes - Small.PoolBytes);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
