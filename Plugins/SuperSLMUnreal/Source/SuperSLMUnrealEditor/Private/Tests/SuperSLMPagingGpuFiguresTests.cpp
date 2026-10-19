// Plugin paging plan, GPU red suite (step K5): the GPU figures (§7 cells 1.10, 10.7, 10.9). Every test
// here carries StressFilter and is run by name on the box at K8 (10.7's [two-cap] leg needs the 1.5B
// artifact through SUPERSLM_PAGING_1P5B_PATH):
//
//   SuperSLM.Paging.Gpu.Figures.Memory.AEx05B      cell 10.7, GPU 0.5B: regions, probe, adapter delta   [K8]
//   SuperSLM.Paging.Gpu.Figures.TwoCap.Memory      cell 10.7 [two-cap]                                  [K8]
//   SuperSLM.Paging.Gpu.Figures.LifetimePooled     cell 1.10: R-S2d in pooled mode                       [K8]
//   SuperSLM.Paging.Gpu.Figures.ManyNpcs.AEx05B    cell 10.9, GPU 0.5B                                   [K8]
//
// The adapter's local-memory delta (audit N19; the K5 record, reading R11) is the reading independent
// of the plugin: Delta = U(pooled) - U(dedicated at the same BlockCount), where U is
// GetLocalVideoMemoryUsageBytes() (DXGI CurrentUsage) after Configure. §2's arithmetic predicts it as
// the pool's bytes less the dedicated sequences' (N x ContextCap x page_bytes / B). The tolerance is
// max(5% of the pool's bytes, 32 MiB): other allocations move between the two Configures. Every test
// fails first at ConfigurePooled() while pooled mode is unbuilt.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// Cell 10.7 at the 50-NPC shape (50 sequences, budget 512, one prefix block, one headroom block):
	// the ledger's regions, the pool by the fill probe (one-state: free = pool - S at quiescence), the
	// page size, and the adapter's delta, pooled against dedicated.
	bool RunMemory(FAutomationTestBase& T, EGpuArtifact Artifact, int64 WantS, int64 WantP, int64 WantH, int64 WantPageBytes)
	{
		FGpuPagingRig Rig;
		if (!Rig.Init(T, Artifact) || !ConfigureDedicated(T, Rig, MakeGpuConfig(Artifact, 50, 0, 0, 1)))
		{
			return false;
		}
		const int64 Dedicated = Rig.Gpu->GetLocalVideoMemoryUsageBytes();
		const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(Artifact, 50, 512, 1, 1);
		if (!ConfigurePooled(T, Rig, C))
		{
			return false;
		}
		const int64 Pooled = Rig.Gpu->GetLocalVideoMemoryUsageBytes();
		const FSuperSLMPagedLedgerView V = LedgerOf(*Rig.Gpu);
		const int64 Pool = WantS + WantP + WantH;
		bool bOk = T.TestEqual(TEXT("S"), V.CapacityOf(ERegion::S), WantS);
		bOk &= T.TestEqual(TEXT("P"), V.CapacityOf(ERegion::P), WantP);
		bOk &= T.TestEqual(TEXT("H"), V.CapacityOf(ERegion::H), WantH);
		bOk &= T.TestEqual(TEXT("the pool, by the ledger"), V.PoolPages, Pool);
		bOk &= T.TestEqual(TEXT("page bytes, from the engine"), Rig.Geo.PageBytes, WantPageBytes);
		int64 FreePages = -1;
		bOk &= ProbeFree(T, *Rig.Gpu, Pool, FreePages);
		bOk &= T.TestEqual(TEXT("the pool, by the fill probe: free = pool - S at quiescence"), FreePages, Pool - WantS);
		const int64 PoolBytes = Pool * Rig.Geo.PageBytes;
		const int64 Predicted = PoolBytes - 50 * Rig.Geo.Cp * Rig.Geo.PageBytes / Rig.Geo.B;
		const int64 Delta = Pooled - Dedicated;
		const int64 Tolerance = FMath::Max<int64>(PoolBytes / 20, 32LL * 1024 * 1024);
		T.AddInfo(FString::Printf(TEXT("cell 10.7: dedicated %lld bytes, pooled %lld bytes, delta %lld, predicted %lld (pool %lld bytes), tolerance %lld"),
			Dedicated, Pooled, Delta, Predicted, PoolBytes, Tolerance));
		bOk &= T.TestTrue(TEXT("the adapter's local-memory readings are available"), Dedicated > 0 && Pooled > 0);
		bOk &= T.TestTrue(*FString::Printf(TEXT("the adapter's delta %lld is within %lld of §2's %lld"), Delta, Tolerance, Predicted), FMath::Abs(Delta - Predicted) <= Tolerance);
		bOk &= NoLedgerFault(T, *Rig.Gpu, TEXT("cell 10.7"));
		return bOk;
	}
}

// Cell 10.7, GPU 0.5B (audit N19): at 50 sequences, budget 512, one block of P and H: S = 1,650, P =
// 256, H = 256, a pool of 2,162 pages of 96 KiB (202.7 MiB) by the ledger and the fill probe, and the
// adapter's delta against 50 dedicated sequences (1,200 MiB) matches §2's arithmetic.
// Mutants: blocks of the wrong size (the regions and the probe); a pool not in VRAM, or a second copy
// of it (the delta).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFiguresMemoryTest, "SuperSLM.Paging.Gpu.Figures.Memory.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuFiguresMemoryTest::RunTest(const FString& Parameters)
{
	return RunMemory(*this, EGpuArtifact::AEx05B, 1650, 256, 256, 98304);
}

// Cell 10.7 [two-cap] (coverage re-audit R1-G2): at the 1.5B artifact with ContextCap 4,096, the same
// shape gives S = 1,650, P = 257, H = 513 and a pool of 2,420 pages of 224 KiB, by the fill probe and
// by the adapter's delta (against 50 dedicated sequences at ContextCap, 2,800 MiB).
// Mutant: option (a)'s 2,048-page blocks, which the regions and the pool both expose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFiguresTwoCapMemoryTest, "SuperSLM.Paging.Gpu.Figures.TwoCap.Memory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuFiguresTwoCapMemoryTest::RunTest(const FString& Parameters)
{
	return RunMemory(*this, EGpuArtifact::TwoCap15B, 1650, 257, 513, 229376);
}

// Cell 1.10: R-S2d in pooled mode. 1,000 cycles, each building a 32-token prefix, adopting it on a
// vended sequence, generating 4 tokens, resetting, returning and releasing the prefix; every 50th cycle
// also saves and restores the adopter. The adapter's local-memory usage after cycle 1,000 is within
// 24 MiB of its reading after 10 warm-up cycles (one A-EX KV block, R-S2d's own bound), the probe
// reads the post-Configure free count (one-state), and form 1 holds.
// Mutant: a per-cycle leak of any GPU resource (a committed resource is at least 64 KiB: 990 cycles
// exceed the bound) or of pool pages (the probe).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFiguresLifetimeTest, "SuperSLM.Paging.Gpu.Figures.LifetimePooled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuFiguresLifetimeTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const int64 Pool = LedgerOf(Gpu).PoolPages;
	int64 FreeAtStart = -1;
	TArray<int32> Tokens, Suffix;
	if (!ProbeFree(*this, Gpu, Pool, FreeAtStart) || !MakeTokens(*this, *Rig.Model, 32, 801, Tokens) || !MakeTokens(*this, *Rig.Model, 4, 802, Suffix))
	{
		return false;
	}
	constexpr int32 Cycles = 1000;
	constexpr int32 WarmUp = 10;
	int64 VramAtWarmUp = 0;
	int64 VramAtEnd = 0;
	int32 Failures = 0;
	for (int32 Cycle = 0; Cycle < Cycles && Failures < 5; ++Cycle)
	{
		FSuperSLMPrefix Prefix;
		FSuperSLMGpuSequence Seq;
		TArray<int32> Out;
		bool bCycle = BuildPrefix(*this, Gpu, Tokens, Prefix) && Vend(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, Prefix) && GenerateComplete(*this, Gpu, Seq, Suffix, 4, Out);
		if (bCycle && Cycle % 50 == 49)
		{
			TArray<uint8> Blob;
			FSuperSLMGpuSequence Restored;
			bCycle = SaveAndTake(*this, Gpu, Seq, Blob) && ApplyRestore(*this, Gpu, Blob, Rig.Model, Restored);
			if (Restored.IsValid())
			{
				ReturnAndDrain(Gpu, Restored);
			}
		}
		bCycle = bCycle && ResetAndWait(*this, Gpu, Seq);
		if (Seq.IsValid())
		{
			ReturnAndDrain(Gpu, Seq);
		}
		if (Prefix.IsValid())
		{
			bCycle = ReleasePrefixAndDrain(*this, Gpu, Prefix) && bCycle;
		}
		Failures += bCycle ? 0 : 1;
		if (Cycle == WarmUp - 1)
		{
			VramAtWarmUp = Gpu.GetLocalVideoMemoryUsageBytes();
		}
		if (Cycle == Cycles - 1)
		{
			VramAtEnd = Gpu.GetLocalVideoMemoryUsageBytes();
		}
	}
	bool bOk = TestEqual(TEXT("every cycle runs"), Failures, 0);
	const int64 Growth = VramAtEnd - VramAtWarmUp;
	AddInfo(FString::Printf(TEXT("cell 1.10: local memory after %d cycles %lld, after %d cycles %lld, growth %lld"), WarmUp, VramAtWarmUp, Cycles, VramAtEnd, Growth));
	bOk &= TestTrue(TEXT("both local-memory samples were taken"), VramAtWarmUp > 0 && VramAtEnd > 0);
	bOk &= TestTrue(*FString::Printf(TEXT("local-memory growth %lld is under one A-EX KV block (24 MiB)"), Growth), Growth < 24LL * 1024 * 1024);
	int64 FreeAtEnd = -1;
	bOk &= ProbeFree(*this, Gpu, Pool, FreeAtEnd);
	bOk &= TestEqual(TEXT("the probe reads its post-Configure free count"), FreeAtEnd, FreeAtStart);
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("after 1,000 cycles"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 1.10"));
	return bOk;
}

// Cell 10.9, GPU 0.5B (audit B3; rev 1.8): 50 slots, one 1,000-token persona, budget 512, a distinct
// 8-token suffix per slot, at (50, 512, 1 prefix block, H 0).
//   - Solo references first: each slot's suffix run alone on an adopter of the same persona to E7's
//     count, 1 + (1,512 - 1,008) = 505 tokens, graded by 10.4 (a)'s oracle at 1,512.
//   - Then all 50 adopt and generate together, interleaved by the scheduler, each to its own limit stop
//     (MaxNewTokens above it): each resolves KvBudgetExceeded having emitted 505 tokens equal to its
//     solo run.
//   - Mid-run, the probe's one-state form reads 193 free: drawn 1,650 + 63 = 1,713.
//   - Phase 2 (use after free): the persona is released mid-run (a ghost while its adopters hold it),
//     and a 3,072-token prefix is built with H sized 0 and P's room exact (256 - 63 = 193 = R(3,072)),
//     so the only free pages are the ones it must draw. Every sharer's tokens are unchanged.
// Mutants: a ghost released at the persona's release (phase 2 draws the sharers' pages: their tokens
// change); a fixed MaxNewTokens stop below the limit (the count and the stop by name).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuFiguresManyNpcsTest, "SuperSLM.Paging.Gpu.Figures.ManyNpcs.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuFiguresManyNpcsTest::RunTest(const FString& Parameters)
{
	constexpr int32 Slots = 50;
	constexpr int32 ToStop = 505;
	// Keep the build seat advancing while the fifty clients share composed slices.
	constexpr ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::Composed;
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, Slots, 512, 1, 0)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Persona, Phase2Tokens;
	TArray<int32> Suffixes[Slots];
	TArray<int32> Solo[Slots];
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 901, Persona) || !MakeTokens(*this, *Rig.Model, 3072, 902, Phase2Tokens) || !BuildPrefix(*this, Gpu, Persona, Prefix))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("E7: 1 + (1,000 + 512 - 1,008) = 505"), int64(1 + (1000 + 512 - 1008)), int64(ToStop));
	for (int32 I = 0; I < Slots; ++I)
	{
		FSuperSLMGpuSequence Seq;
		if (!MakeTokens(*this, *Rig.Model, 8, 1000 + I, Suffixes[I]) || !Vend(*this, Gpu, Seq, Path) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
		{
			return false;
		}
		bOk &= GenerateComplete(*this, Gpu, Seq, Suffixes[I], ToStop, Solo[I]);
		bOk &= PositionOracle(*this, Gpu, Seq, 1000 + 8 + ToStop - 1, *FString::Printf(TEXT("slot %d solo"), I));
		ReturnAndDrain(Gpu, Seq);
	}

	TArray<FSuperSLMGpuSequence> Seqs;
	TArray<FSuperSLMLifecycleOpHandle> Handles;
	for (int32 I = 0; I < Slots; ++I)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq, Path) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
		{
			return false;
		}
		Seqs.Add(Seq);
	}
	for (int32 I = 0; I < Slots; ++I)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Suffixes[I];
		Request.MaxNewTokens = ToStop + 64;
		Handles.Add(Gpu.RequestBeginGeneration(Seqs[I], Request));
	}
	bOk &= TickN(*this, Gpu, 60, TEXT("the 50 slots run"));
	const auto AllActive = [&Gpu, &Seqs]()
	{
		for (const FSuperSLMGpuSequence& Seq : Seqs)
		{
			const int32 Count = Gpu.GetGeneratedTokens(Seq).Num();
			if (Gpu.GetPhase(Seq) != ESuperSLMSequencePhase::Decoding || Count < 8 || Count >= ToStop)
			{
				return false;
			}
		}
		return true;
	};
	// A tick count does not establish prompt completion or generated-token progress.
	const bool bReachedActive = Pump(Gpu, [&Gpu, &Seqs, &AllActive]()
	{
		if (AllActive()) { return true; }
		for (const FSuperSLMGpuSequence& Seq : Seqs)
		{
			const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) { return true; }
		}
		return false;
	}, GenerateSeconds);
	if (!TestTrue(TEXT("all fifty clients reach Decoding with 8..504 generated tokens before release"), bReachedActive && AllActive()))
	{
		return false;
	}
	int64 FreeMid = -1;
	bOk &= ProbeFree(*this, Gpu, LedgerOf(Gpu).PoolPages, FreeMid);
	bOk &= TestEqual(TEXT("mid-run, one-state: 193 free (drawn 1,650 + 63 = 1,713)"), FreeMid, int64(193));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("mid-run"));

	// Phase 2.
	FString ReleaseError;
	bOk &= TestTrue(*FString::Printf(TEXT("the persona is released mid-run (%s)"), *ReleaseError), Gpu.ReleasePrefix(Prefix, ReleaseError));
	// Four logical ticks do not establish that the asynchronous release has applied.
	// Wait for this release, while the other fifty generations continue to run.
	const bool bReleaseApplied = Pump(Gpu, [&Gpu, &Prefix]()
	{
		return !FSuperSLMPagedGpuTestSeam::IsPrefixReleasePending(Gpu, Prefix);
	}, OpSeconds);
	bOk &= TestTrue(TEXT("the persona release reaches its game-thread apply mid-run"), bReleaseApplied);
	TArray<int32> ReleasedCounts;
	for (int32 I = 0; I < Seqs.Num(); ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("phase 2: slot %d is still decoding at release"), I), (uint8)Gpu.GetPhase(Seqs[I]), (uint8)ESuperSLMSequencePhase::Decoding);
		bOk &= TestTrue(*FString::Printf(TEXT("phase 2: slot %d is between 8 and its unchanged 505-token stop"), I), Gpu.GetGeneratedTokens(Seqs[I]).Num() >= 8 && Gpu.GetGeneratedTokens(Seqs[I]).Num() < ToStop);
		ReleasedCounts.Add(Gpu.GetGeneratedTokens(Seqs[I]).Num());
	}
	bOk &= TestTrue(TEXT("the persona is a ghost while its adopters hold it"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, Prefix));
	bOk &= TestEqual(TEXT("P's room is exact: 256 - 63 = 193 = R(3,072)"), Free(Gpu, ERegion::P), Rig.Geo.PrefixCharge(3072));
	FSuperSLMPrefix Phase2;
	FString BuildError;
	bOk &= TestEqual(*FString::Printf(TEXT("phase 2's prefix is admitted (%s)"), *BuildError), (uint8)Gpu.CreatePrefix(Phase2Tokens, Phase2, &BuildError), (uint8)ESuperSLMPrefixResult::Success);
	bOk &= WaitPrefixReady(*this, Gpu, Phase2);
	for (int32 I = 0; I < Seqs.Num(); ++I)
	{
		const int32 Count = Gpu.GetGeneratedTokens(Seqs[I]).Num();
		bOk &= TestEqual(*FString::Printf(TEXT("phase 2: slot %d is still decoding when the replacement is Ready"), I), (uint8)Gpu.GetPhase(Seqs[I]), (uint8)ESuperSLMSequencePhase::Decoding);
		bOk &= TestTrue(*FString::Printf(TEXT("phase 2: slot %d advances during replacement construction and remains below 505"), I), Count > ReleasedCounts[I] && Count < ToStop);
		AddInfo(FString::Printf(TEXT("phase 2: slot %d generated %d at release, %d at replacement Ready"), I, ReleasedCounts[I], Count));
	}
	const bool bEnded = Pump(Gpu, [&Gpu, &Seqs, &Phase2]()
	{
		for (const FSuperSLMGpuSequence& Seq : Seqs)
		{
			const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
			if ((Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted) || Gpu.GetPendingLifecycleOperationCount(Seq) != 0)
			{
				return false;
			}
		}
		const ESuperSLMPrefixPhase Built = Gpu.GetPrefixPhase(Phase2);
		return Built == ESuperSLMPrefixPhase::Ready || Built == ESuperSLMPrefixPhase::Faulted || Built == ESuperSLMPrefixPhase::Invalid;
	}, GenerateSeconds * 4);
	bOk &= TestTrue(TEXT("every slot reaches its stop and phase 2's build ends"), bEnded);
	bOk &= TestEqual(TEXT("phase 2's prefix is built"), (uint8)Gpu.GetPrefixPhase(Phase2), (uint8)ESuperSLMPrefixPhase::Ready);
	for (int32 I = 0; I < Slots; ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: stops Faulted"), I), (uint8)Gpu.GetPhase(Seqs[I]), (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: by name, KvBudgetExceeded"), I), (uint8)Gpu.GetLastFaultReason(Seqs[I]), (uint8)ESuperSLMGpuFaultReason::KvBudgetExceeded);
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: emits 505"), I), Gpu.GetGeneratedTokens(Seqs[I]).Num(), ToStop);
		bOk &= TestEqual(*FString::Printf(TEXT("slot %d: tokens equal its solo run (unchanged across phase 2)"), I), Gpu.GetGeneratedTokens(Seqs[I]), Solo[I]);
	}
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 10.9"));
	for (const FSuperSLMLifecycleOpHandle& Handle : Handles)
	{
		Gpu.ReleaseLifecycleOpHandle(Handle);
	}
	for (const FSuperSLMGpuSequence& Seq : Seqs)
	{
		Gpu.ReturnSequence(Seq);
	}
	DrainQuiet(Gpu);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Phase2);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
