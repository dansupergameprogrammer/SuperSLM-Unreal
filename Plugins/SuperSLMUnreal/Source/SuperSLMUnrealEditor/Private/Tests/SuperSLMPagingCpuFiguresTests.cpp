// Plugin paging plan, CPU red suite (step K1): the box run's figures (step K4), at A-EX (0.5B, cap
// 4,096, a block of 64 pages) and at the shipped 1.5B artifact (cap 32,768, a block of 512 pages).
//
//   SuperSLM.Paging.Cpu.Figures.Sharing.<artifact>            cell 10.1                 [K4]
//   SuperSLM.Paging.Cpu.Figures.Memory.<artifact>             cell 10.7 (the CPU leg)   [K4]
//   SuperSLM.Paging.Cpu.Figures.ManyNpcs.<artifact>           cell 10.9 (the CPU legs)  [K4]
//   SuperSLM.Paging.Cpu.Figures.Headroom15B                   cell 4.4 (the 1.5B legs)  [K4]
//
// These are long runs (K4 budgets about 75 minutes for the set), so they carry StressFilter and are
// run by name on the box. The 1.5B artifact is found through SUPERSLM_PAGING_1P5B_PATH (see the
// fixture). Figures are graded by the fill probe against the plan's arithmetic, never by stats; the
// MiB figures are recorded with AddInfo. Each test names its cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "HAL/PlatformTime.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	constexpr int32 Npcs = 50;
	constexpr int32 PersonaTokens = 1000;
	constexpr int32 NpcBudget = 512;
	// The interleaved run of 10.9: 50 slots x 505 tokens. At single-stream CPU rates, it and its 50
	// twins come to about 2,200 s at 0.5B and 6,300 s at 1.5B before batching, so the run's bound is
	// wider than one generation's.
	constexpr double ManyNpcsSeconds = 4.0 * 3600.0;

	double MiB(int64 Pages, int64 PageBytes)
	{
		return double(Pages) * double(PageBytes) / (1024.0 * 1024.0);
	}

	// Cell 10.1: 50 adopters of a 1,000-token prefix at budget 512 draw 450 + 16 = 466 pages, not 50 x 16
	// more. Exact form, rebuilt by replaying the construction.
	bool RunSharing(FAutomationTestBase& T, EPagingArtifact Artifact)
	{
		FPagingRig Rig;
		const FSuperSLMRuntimeConfig C = MakeConfig(Npcs, NpcBudget, 1, 1);
		if (!Rig.Init(T, Artifact) || !ConfigureBudget(T, Rig, C))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		TArray<int32> Persona;
		if (!MakeTokens(T, Cpu, PersonaTokens, 1, Persona))
		{
			return false;
		}
		auto Build = [&]()
		{
			FSuperSLMPrefix Prefix;
			if (!ConfigureBudget(T, Rig, C) || !BuildPrefix(T, Cpu, Persona, Prefix))
			{
				return false;
			}
			for (int32 I = 0; I < Npcs; ++I)
			{
				FSuperSLMSequence Seq;
				if (!Vend(T, Cpu, Seq) || !AdoptAndWait(T, Cpu, Seq, Prefix))
				{
					return false;
				}
			}
			return true;
		};
		if (!Build())
		{
			return false;
		}
		const FCpuPagingGeometry& G = Rig.Geo;
		const int64 Expected = int64(Npcs) * G.R(NpcBudget) + G.OccupiedPages(PersonaTokens);
		bool bOk = T.TestEqual(TEXT("the plan's figure: 450 + 16 = 466"), Expected, int64(466));
		T.AddInfo(FString::Printf(TEXT("cell 10.1, %s: %lld pages drawn expected, %.1f MiB"), ArtifactName(Artifact), Expected, MiB(Expected, G.PageBytes)));
		bOk &= ProbeDrawnExactly(T, Cpu, ExpectedRegions(G, C).Pool(), Expected, Build, TEXT("50 adopters of one persona"));
		bOk &= NoLedgerFault(T, Cpu, TEXT("cell 10.1"));
		return bOk;
	}

	// Cell 10.7 (the CPU leg): the pool at the 50-NPC shape (BlockCount 50, budget 512, one prefix
	// block, one headroom block) is S + P + H = 450 + 2 x block pages: 578 at 0.5B, 1,474 at 1.5B.
	// Read three ways: the plan's arithmetic, the fill probe (after Configure only S is drawn, so the
	// exact form's k is the rest of the pool), and the subsystem's reserved bytes. The reserved bytes
	// also carry the engine's page-pool overhead (its page tables, 64-byte aligned), which the test
	// module cannot price, so they are bounded: at least the pages' bytes, and less than one page
	// more. A pool a page or more off, or the legacy block pool, falls outside the bound.
	bool RunMemory(FAutomationTestBase& T, EPagingArtifact Artifact)
	{
		FPagingRig Rig;
		const FSuperSLMRuntimeConfig C = MakeConfig(Npcs, NpcBudget, 1, 1);
		if (!Rig.Init(T, Artifact) || !ConfigureBudget(T, Rig, C))
		{
			return false;
		}
		const FCpuPagingGeometry& G = Rig.Geo;
		const FRegions Want = ExpectedRegions(G, C);
		const int64 Arithmetic = int64(Npcs) * G.R(NpcBudget) + 2 * G.Block();
		bool bOk = T.TestEqual(TEXT("the arithmetic: 450 + 2 x block"), Want.Pool(), Arithmetic);
		bOk &= T.TestEqual(TEXT("the plan's figure for this artifact"), Arithmetic, Artifact == EPagingArtifact::AEx05B ? int64(578) : int64(1474));
		auto Rebuild = [&]() { return ConfigureBudget(T, Rig, C); };
		bOk &= ProbeDrawnExactly(T, *Rig.Cpu, Arithmetic, Want.S, Rebuild, TEXT("the 50-NPC pool, by the probe"));
		const int64 Reserved = Rig.Cpu->GetKvPoolReservedBytes();
		const int64 PagesBytes = Arithmetic * G.PageBytes;
		bOk &= T.TestTrue(*FString::Printf(TEXT("the reserved bytes (%lld) cover the pool's pages (%lld)"), Reserved, PagesBytes), Reserved >= PagesBytes);
		bOk &= T.TestTrue(*FString::Printf(TEXT("the reserved bytes (%lld) exceed the pages' (%lld) by less than one page (%lld): the aligned overhead only"),
			Reserved, PagesBytes, G.PageBytes), Reserved < PagesBytes + G.PageBytes);
		T.AddInfo(FString::Printf(TEXT("cell 10.7, %s: %lld pages of %lld bytes, %.1f MiB"), ArtifactName(Artifact), Arithmetic, G.PageBytes, MiB(Arithmetic, G.PageBytes)));
		return bOk;
	}

	// Cell 10.9 (the CPU legs): many NPCs at real size. 50 slots adopt one 1,000-token persona at
	// budget 512, each with its own 8-token suffix, and generate interleaved by the scheduler to each
	// slot's own limit stop (plan rev 1.8): MaxNewTokens is above the limit, and every slot resolves
	// KvBudgetExceeded having emitted E7's 1 + (limit - c) = 1 + (1,512 - 1,008) = 505 tokens. Every
	// slot's tokens equal its solo no-prefix twin's 505 (run first, in legacy mode, where a plain
	// holder reserves the cap, so the twin's only stop is its MaxNewTokens). Mid-run, with
	// every slot past 8 tokens, the probe's one-state form reads the occupancy (466 drawn). Phase 2
	// (use after free): the persona is released mid-run (a ghost of 16 while the 50 hold it), and a
	// new prefix is built with H sized 0 and P's room exact, R(L2) = block - 16, so the only free pages
	// are the ones it must draw. It builds with no ledger fault, and every slot's tokens are unchanged.
	bool RunManyNpcs(FAutomationTestBase& T, EPagingArtifact Artifact)
	{
		FPagingRig Rig;
		if (!Rig.Init(T, Artifact) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)))
		{
			return false;
		}
		USuperSLMSubsystem& Cpu = *Rig.Cpu;
		constexpr int32 SuffixTokens = 8;
		const int64 Limit = FMath::Min<int64>(PersonaTokens + NpcBudget, ExpectedCap(Artifact));
		const int32 ToStop = int32(1 + (Limit - (PersonaTokens + SuffixTokens)));  // E7: 505
		if (!T.TestEqual(TEXT("E7's count to each slot's stop: 1 + (1,512 - 1,008) = 505"), ToStop, 505))
		{
			return false;
		}
		TArray<int32> Persona;
		TArray<TArray<int32>> Suffixes, Twins;
		if (!MakeTokens(T, Cpu, PersonaTokens, 1, Persona))
		{
			return false;
		}
		const double TwinStart = FPlatformTime::Seconds();
		for (int32 I = 0; I < Npcs; ++I)
		{
			TArray<int32> Suffix, Tokens;
			FSuperSLMSequence Twin;
			if (!MakeTokens(T, Cpu, SuffixTokens, 100 + I, Suffix) || !Vend(T, Cpu, Twin) || !GenerateComplete(T, Cpu, Twin, Concat(Persona, Suffix), ToStop, Tokens))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Twin);
			Suffixes.Add(Suffix);
			Twins.Add(Tokens);
		}
		T.AddInfo(FString::Printf(TEXT("cell 10.9, %s: 50 solo twins in %.1f s"), ArtifactName(Artifact), FPlatformTime::Seconds() - TwinStart));

		const FSuperSLMRuntimeConfig C = MakeConfig(Npcs, NpcBudget, 1, 0);
		FSuperSLMPrefix Prefix;
		if (!ConfigureBudget(T, Rig, C) || !BuildPrefix(T, Cpu, Persona, Prefix))
		{
			return false;
		}
		const FCpuPagingGeometry& G = Rig.Geo;
		TArray<FSuperSLMSequence> Slots;
		for (int32 I = 0; I < Npcs; ++I)
		{
			FSuperSLMSequence Seq;
			if (!Vend(T, Cpu, Seq) || !AdoptAndWait(T, Cpu, Seq, Prefix))
			{
				return false;
			}
			Slots.Add(Seq);
		}
		const double RunStart = FPlatformTime::Seconds();
		// One existing scenario watchdog covers every interleaved stage, including the
		// full-cap replacement build. The plan's Ready and token assertions stay below.
		const double RunDeadline = RunStart + ManyNpcsSeconds;
		double LastRunTime = RunStart;
		bool bRunClockValid = FMath::IsFinite(RunStart) && FMath::IsFinite(RunDeadline);
		auto RemainingRunSeconds = [&]()
		{
			const double Now = FPlatformTime::Seconds();
			if (!bRunClockValid || !FMath::IsFinite(Now) || Now < LastRunTime)
			{
				bRunClockValid = false;
				return 0.0;
			}
			LastRunTime = Now;
			return FMath::Max(0.0, RunDeadline - Now);
		};
		for (int32 I = 0; I < Npcs; ++I)
		{
			if (!T.TestTrue(TEXT("scenario watchdog permits generation admission"), RemainingRunSeconds() > 0.0))
			{
				return false;
			}
			FSuperSLMGenerationRequest Request;
			Request.PromptTokens = Suffixes[I];
			Request.MaxNewTokens = ToStop + 16;  // above the limit: the limit is the only stop
			FString Error;
			const bool bBegun = Cpu.BeginGeneration(Slots[I], Request, Error);
			if (!T.TestTrue(*FString::Printf(TEXT("slot %d begins (%s)"), I, *Error), bBegun))
			{
				return false;
			}
		}
		auto AllPast = [&](int32 Tokens)
		{
			for (const FSuperSLMSequence& Seq : Slots)
			{
				const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
				if (Cpu.GetGeneratedTokens(Seq).Num() < Tokens && Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
				{
					return false;
				}
			}
			return true;
		};
		Drain(Cpu, [&]() { return AllPast(8); }, FMath::Min(GenerateSeconds, RemainingRunSeconds()));
		bool bOk = T.TestTrue(TEXT("every slot is past 8 tokens mid-run"), AllPast(8));

		// Mid-run occupancy, one-state form.
		const int64 Expected = int64(Npcs) * G.R(NpcBudget) + G.OccupiedPages(PersonaTokens);
		int64 FreePages = -1;
		const int64 Pool = ExpectedRegions(G, C).Pool();
		bOk &= ProbeFree(T, Cpu, Pool, FreePages) && T.TestEqual(TEXT("mid-run: the probe reads 466 drawn"), Pool - FreePages, Expected);

		// Phase 2: release mid-run, then build into exactly the room left.
		// Other slots deliberately keep posting work. Global quiescence cannot identify this
		// release's completion: wait for its delivered PrefixRelease row instead. This is the
		// only released prefix here; the replacement is created below, after this observation.
		int64 BeforeReleaseJobId = 0;
		for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
		{
			BeforeReleaseJobId = FMath::Max(BeforeReleaseJobId, Job.JobId);
		}
		if (!T.TestTrue(TEXT("scenario watchdog permits prefix release admission"), RemainingRunSeconds() > 0.0))
		{
			return false;
		}
		FString ReleaseError;
		const bool bPersonaReleased = Cpu.ReleasePrefix(Prefix, ReleaseError);
		bOk &= T.TestTrue(*FString::Printf(TEXT("phase 2: the live persona release is admitted (%s)"), *ReleaseError), bPersonaReleased);
		auto ReleaseDelivered = [&]()
		{
			for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
			{
				if (Job.JobId > BeforeReleaseJobId && Job.Kind == ESuperSLMWorkerJobKind::PrefixRelease && Job.DeliveredAtTick >= 0)
				{
					return true;
				}
			}
			return false;
		};
		Drain(Cpu, ReleaseDelivered, FMath::Min(OpSeconds, RemainingRunSeconds()));
		bOk &= T.TestTrue(TEXT("phase 2: the specific prefix release delivered while generations continue"), ReleaseDelivered());
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("phase 2: slot %d is still decoding at release"), I), (uint8)Cpu.GetPhase(Slots[I]), (uint8)ESuperSLMSequencePhase::Decoding);
			bOk &= T.TestTrue(*FString::Printf(TEXT("phase 2: slot %d is between 8 and its unchanged 505-token stop"), I), Cpu.GetGeneratedTokens(Slots[I]).Num() >= 8 && Cpu.GetGeneratedTokens(Slots[I]).Num() < ToStop);
		}
		bOk &= T.TestTrue(TEXT("phase 2: the persona is a ghost while the slots hold it"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, Prefix));
		// Not quite zero slack: P still charges the ghost its 16 occupied pages, but the ghost's partial
		// tail page (1,000 = 15 x 64 + 40; the adopters share only full pages) is freed at the release,
		// so the engine has one page beyond Room. A build that draws one page over its charge still fits; the mutant
		// this phase targets (the ghost's charge refunded or misread at the release) is caught by the
		// Room check below, and a use-after-free by the slots' unchanged tokens.
		const int64 Room = Free(Cpu, ERegion::P);
		bOk &= T.TestEqual(TEXT("phase 2: P's room is the block less the ghost's 16"), Room, G.Block() - G.OccupiedPages(PersonaTokens));
		const int32 L2 = int32((Room - 1) * G.B);
		bOk &= T.TestEqual(TEXT("phase 2: the new prefix's charge is exactly the room"), G.PrefixCharge(L2), Room);
		TArray<int32> NewTokens;
		FSuperSLMPrefix NewPrefix;
		bOk &= MakeTokens(T, Cpu, L2, 2, NewTokens) && BuildPrefix(T, Cpu, NewTokens, NewPrefix, RemainingRunSeconds());
		bOk &= NoLedgerFault(T, Cpu, TEXT("phase 2: the build drew only the free pages"));

		Drain(Cpu, [&]() { return AllPast(ToStop + 16); }, RemainingRunSeconds());
		const double RemainingAtCompletion = RemainingRunSeconds();
		bOk &= T.TestTrue(TEXT("cell 10.9: all interleaved stages finish before the existing four-hour scenario watchdog expires"),
			bRunClockValid && RemainingAtCompletion > 0.0);
		T.AddInfo(FString::Printf(TEXT("cell 10.9, %s: 50 interleaved slots in %.1f s"), ArtifactName(Artifact), FPlatformTime::Seconds() - RunStart));
		for (int32 I = 0; I < Npcs; ++I)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("slot %d: stops Faulted at its limit"), I), (uint8)Cpu.GetPhase(Slots[I]), (uint8)ESuperSLMSequencePhase::Faulted);
			bOk &= T.TestEqual(*FString::Printf(TEXT("slot %d: resolves KvBudgetExceeded"), I), (uint8)Cpu.GetLastDecodeOutcome(Slots[I]), (uint8)ESuperSLMDecodeOutcome::KvBudgetExceeded);
			bOk &= T.TestEqual(*FString::Printf(TEXT("slot %d: emits 1 + (limit - c) = %d"), I, ToStop), Cpu.GetGeneratedTokens(Slots[I]).Num(), ToStop);
			bOk &= T.TestEqual(*FString::Printf(TEXT("slot %d: tokens equal its solo twin (unchanged across phase 2)"), I), Cpu.GetGeneratedTokens(Slots[I]), Twins[I]);
		}
		for (const FSuperSLMSequence& Seq : Slots)
		{
			Cpu.ReturnSequence(Seq);
		}
		DrainQuiet(Cpu);
		bOk &= NoLedgerFault(T, Cpu, TEXT("cell 10.9"));
		return bOk;
	}
}

// Cell 10.1. Mutant: an adopter that copies the prefix's
// full pages (drawn 50 x 16 more).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuSharing05BTest, "SuperSLM.Paging.Cpu.Figures.Sharing.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuSharing05BTest::RunTest(const FString& Parameters)
{
	return RunSharing(*this, EPagingArtifact::AEx05B);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuSharing15BTest, "SuperSLM.Paging.Cpu.Figures.Sharing.Shipped15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuSharing15BTest::RunTest(const FString& Parameters)
{
	return RunSharing(*this, EPagingArtifact::Shipped15B);
}

// Cell 10.7. Mutant: blocks of the legacy size, or a region
// sized at the plugin's cap (the pool's page count differs).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuMemory05BTest, "SuperSLM.Paging.Cpu.Figures.Memory.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuMemory05BTest::RunTest(const FString& Parameters)
{
	return RunMemory(*this, EPagingArtifact::AEx05B);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuMemory15BTest, "SuperSLM.Paging.Cpu.Figures.Memory.Shipped15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuMemory15BTest::RunTest(const FString& Parameters)
{
	return RunMemory(*this, EPagingArtifact::Shipped15B);
}

// Cell 10.9. Mutants: a ghost freed at release
// (phase 2's build reuses the slots' shared pages and their tokens change); an adopt at the wrong
// origin (the tokens differ from the twins); a limit taken off by one or not enforced among
// interleaved sharers (a slot's count or outcome differs from 505 and KvBudgetExceeded).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuManyNpcs05BTest, "SuperSLM.Paging.Cpu.Figures.ManyNpcs.AEx05B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuManyNpcs05BTest::RunTest(const FString& Parameters)
{
	return RunManyNpcs(*this, EPagingArtifact::AEx05B);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuManyNpcs15BTest, "SuperSLM.Paging.Cpu.Figures.ManyNpcs.Shipped15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuManyNpcs15BTest::RunTest(const FString& Parameters)
{
	return RunManyNpcs(*this, EPagingArtifact::Shipped15B);
}

// Cell 4.4, the 1.5B legs (cap 32,768, a block of 512 pages; a restore at budget 512, origin 1,000 is
// still charged 9 + 15 = 24):
//   - a burst of 10 hinted restores at one block is all admitted (floor(512 / 24) = 21 >= 10);
//   - private holders per block: 33 applied one at a time ((512 - 24) / 15 + 1), the 34th refused on
//     arrival (free 17 < 24), and admitted after one holder resets.
// Mutant: a hold settled one page smaller (14 per holder admits 35, not 33).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuHeadroom15BTest, "SuperSLM.Paging.Cpu.Figures.Headroom15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuHeadroom15BTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	TArray<uint8> Blob;
	TArray<int32> PrefixTokens;
	if (!Rig.Init(*this, EPagingArtifact::Shipped15B) || !MakeBlobAt(*this, Rig, 512, 1000, 1, Blob, &PrefixTokens))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const FCpuPagingGeometry& G = Rig.Geo;
	bool bOk = TestEqual(TEXT("1.5B: a block of 512 pages"), G.Block(), int64(512));
	bOk &= TestEqual(TEXT("1.5B: the restore is charged 24"), G.RestoreCharge(512, 1000), int64(24));

	// The burst.
	{
		FSuperSLMPrefix Hint;
		if (!ConfigureBudget(*this, Rig, MakeConfig(10, 512, 1, 1)) || !BuildPrefix(*this, Cpu, PrefixTokens, Hint))
		{
			return false;
		}
		TArray<FSuperSLMPagedHold> Holds;
		TArray<FIssuedRestore> Restores;
		for (int32 I = 0; I < 10; ++I)
		{
			Holds.Add(FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore));
		}
		for (int32 I = 0; I < 10; ++I)
		{
			Restores.Add(IssueRestore(Cpu, Blob, Rig.Model, Hint));
		}
		for (const FSuperSLMPagedHold& Hold : Holds)
		{
			bOk &= TestTrue(TEXT("each of the 10 reaches its held apply"), WaitHoldReached(Cpu, Hold, BuildSeconds));
		}
		bOk &= TestEqual(TEXT("all 10 in flight at one block"), LedgerOf(Cpu).InFlightRestores, 10);
		bOk &= TestEqual(TEXT("H charged 240"), Charged(Cpu, ERegion::H), int64(240));
		for (const FSuperSLMPagedHold& Hold : Holds)
		{
			FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
		}
		for (const FIssuedRestore& R : Restores)
		{
			bOk &= TestEqual(TEXT("each succeeds"), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Success);
			Discard(Cpu, R);
		}
	}

	// Private holders per block.
	{
		if (!ConfigureBudget(*this, Rig, MakeConfig(35, 512, 0, 1)))
		{
			return false;
		}
		TArray<FSuperSLMSequence> Holders;
		int32 Admitted = 0;
		for (int32 I = 0; I < 40; ++I)
		{
			const int64 HBefore = Charged(Cpu, ERegion::H);
			const FIssuedRestore R = IssueRestore(Cpu, Blob, Rig.Model);
			const ESuperSLMRestoreResult Outcome = SettleOutcome(Cpu, R, BuildSeconds);
			if (Outcome != ESuperSLMRestoreResult::Success)
			{
				bOk &= TestEqual(TEXT("the first refusal is on arrival, by name"), (uint8)Outcome, (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
				bOk &= TestEqual(TEXT("nothing charged by the refusal"), Charged(Cpu, ERegion::H), HBefore);
				Discard(Cpu, R);
				break;
			}
			Holders.Add(R.Sequence);
			++Admitted;
		}
		bOk &= TestEqual(TEXT("33 private holders per 1.5B block"), Admitted, 33);
		bOk &= TestEqual(TEXT("17 pages left free"), Free(Cpu, ERegion::H), int64(17));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, TEXT("33 private holders applied"));
		if (Holders.Num() > 0)
		{
			bOk &= ResetAndWait(*this, Cpu, Holders[0]);
			FSuperSLMSequence Again;
			bOk &= ApplyRestore(*this, Cpu, Blob, Rig.Model, Again);
		}
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 4.4, 1.5B"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
