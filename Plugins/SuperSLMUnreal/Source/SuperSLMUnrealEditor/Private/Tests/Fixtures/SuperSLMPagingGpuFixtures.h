#pragma once

// The GPU paged-KV suite's shared fixture (plugin paging plan §3.4, §3.9 and §7; step K5). Test code
// only: compiled inside WITH_DEV_AUTOMATION_TESTS translation units, like every header beside it.
// The GPU twin of SuperSLMPagingFixtures.h (the CPU suite's, step K1), with the same rules:
//   - The page arithmetic (R, E, P_blk, H_blk, the regions) is the plan's formulas (§2 E1, E2, E6;
//     §3.2 with the two caps of UP-15), written from the plan text. No plugin or ledger-core
//     function is called for an expected figure.
//   - B, the page size and the model's cap are read from the engine (sslm_gpu_kv_pool_stats and
//     the model's context_cap) through FSuperSLMPagedGpuTestSeam::RunOnPoolThread, never assumed
//     (E1), and checked against the artifact's known figures.
//   - The fill probe (§3.9) is the reference every page-count verdict uses: creates through the
//     engine's own sslm_gpu_seq_create_budgeted on the subsystem's own pool. Stats are never a
//     verdict (E13).
//   - The grading forms are the CPU fixture's: Form1ChargesCoverDrawn() (form 1),
//     ProbeDrawnExactly() (the exact two-sided form, with a rebuild), and forms 2 and 3 written in
//     each cell.
//
// What differs from the CPU fixture, and why:
//   - The GPU's lifecycle requests return a handle and resolve at a later Tick() (D-SLM7457), so
//     every wait here pumps Tick() until the handle resolves, sleeping while the next tick would be
//     gated on the device (plan §2.5 row 21).
//   - The GPU has two caps (UP-15): the model's cap Ce prices every charge; the plugin's cap Cp
//     (ContextCap) bounds what is admitted. FGpuGeometry carries both.
//   - No HoldStart() exists on the GPU (§3.9), so "refused before any job" is graded by a
//     HoldApply() that is never reached AND FSuperSLMPagedGpuTestSeam::JobsPosted() unchanged
//     (K1 coverage audit B2): a hold alone is satisfied by a job that runs and fails.
//   - Payload edits use FSuperSLMPagedTestSeam::EditPayload(), the CPU seam's: the 'SLUW' v2
//     wrapper is one format for both backends (SuperSLMSaveBlob.h), so the edit is backend-neutral.
//     It is a K1 stub until K2 builds it, and K6 depends on K2.
//   - Tokens come from SuperSLM::TokenizeText() on the model, since the GPU subsystem carries no
//     tokenizer entry point.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Optional.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMGpuTestAccess.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMPagedGpuTestSeam.h"
#include "SuperSLMPagedKvTypes.h"
#include "SuperSLMPagedTestSeam.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSequenceTypes.h"
#include "Tests/AutomationCommon.h"

THIRD_PARTY_INCLUDES_START
#include "superslm/gpu_1p0.h"
THIRD_PARTY_INCLUDES_END

namespace SuperSLMPagingGpuFixtures
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr double OpSeconds = 120.0;        // one queued lifecycle operation
	constexpr double BuildSeconds = 1800.0;    // a prefix build of up to ContextCap tokens, one token per build turn
	constexpr double GenerateSeconds = 1800.0; // one generation to its stop (up to ContextCap tokens)

	// --- Artifacts ---------------------------------------------------------------------------

	enum class EGpuArtifact : uint8
	{
		AEx05B,    // A-EX, Qwen2.5-0.5B, model cap 4,096, ContextCap 4,096: B = 16, one cap
		TwoCap15B, // the shipped 1.5B artifact, model cap 32,768, at ContextCap 4,096: the [two-cap] configuration (§7)
	};

	inline int64 ExpectedModelCap(EGpuArtifact Artifact) { return Artifact == EGpuArtifact::AEx05B ? 4096 : 32768; }
	inline int32 LayersOf(EGpuArtifact Artifact) { return Artifact == EGpuArtifact::AEx05B ? 24 : 28; }
	constexpr int64 PluginCap = 4096; // ContextCap in every cell of this suite but 5's geometry leg

	inline const TCHAR* ArtifactName(EGpuArtifact Artifact)
	{
		return Artifact == EGpuArtifact::AEx05B ? TEXT("A-EX (0.5B, cap 4,096)") : TEXT("the shipped 1.5B artifact at ContextCap 4,096 ([two-cap])");
	}

	// The 0.5B artifact is the L2-S2 suite's A-EX (SUPERSLM_L2S2_AEX_PATH). The 1.5B artifact is the
	// one the CPU suite names: SUPERSLM_PAGING_1P5B_PATH, or the A-AD base model under the HF cache.
	inline bool ResolveArtifact(EGpuArtifact Artifact, FString& OutPath, FString& OutReason)
	{
		if (Artifact == EGpuArtifact::AEx05B)
		{
			return SuperSLML2S2Fixtures::TryGetAExArtifactPath(OutPath, OutReason);
		}
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PAGING_1P5B_PATH"));
		OutPath = Override.IsEmpty() ? SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-instruct.sslm")) : Override;
		if (!IFileManager::Get().FileExists(*OutPath))
		{
			OutReason = FString::Printf(TEXT("the shipped 1.5B artifact (model cap 32,768) was not found at '%s'; set SUPERSLM_PAGING_1P5B_PATH to it"), *OutPath);
			return false;
		}
		return true;
	}

	// --- The plan's page arithmetic, written from the plan (§2 E1, E2, E6; §3.2 with UP-15) ------

	struct FGpuGeometry
	{
		int64 B = 0;         // positions per page, from the engine
		int64 Ce = 0;        // the model's context_cap, from the engine: every charge is priced at it
		int64 Cp = 0;        // the plugin's cap, ContextCap: every admission is bounded by it
		int64 PageBytes = 0; // from the engine

		bool IsValid() const { return B > 0 && Ce > 0 && Cp > 0 && PageBytes > 0; }

		static int64 CeilDiv(int64 A, int64 D) { return (A + D - 1) / D; }

		int64 BlockE() const { return CeilDiv(Ce, B); }                                           // ceil(Ce / B)
		int64 R(int64 Budget) const { return FMath::Min(CeilDiv(Budget, B) + 1, BlockE()); }      // E2, at Ce
		int64 E(int64 Budget, int64 Origin) const { return FMath::Min(Origin / B, BlockE() - R(Budget)); } // E6, at Ce
		int64 RestoreCharge(int64 Budget, int64 Origin) const { return R(Budget) + E(Budget, Origin); }
		int64 Pblk() const { return FMath::Min(CeilDiv(Cp, B) + 1, BlockE()); }                   // §3.2
		int64 Hblk() const { return FMath::Min(Pblk() + Cp / B, BlockE()); }                      // §3.2
		int64 FullPages(int64 Length) const { return Length / B; }
		int64 OccupiedPages(int64 Length) const { return Length > 0 ? CeilDiv(Length, B) : 0; }
		int64 PrefixCharge(int64 Tokens) const { return R(FMath::Max<int64>(Tokens, 1)); }       // §3.3 "Empty prefixes"
		int64 NestedPrivate(int64 ParentLength, int64 ChildTokens) const                           // §3.3
		{
			return CeilDiv(ParentLength + ChildTokens, B) - ParentLength / B;
		}
		int64 Guarantee(int64 Charge, int64 SequenceBudget) const { return FMath::Min(Charge, R(SequenceBudget)); }
		int64 HeadroomHold(int64 Draw, int64 SequenceBudget) const { return FMath::Max<int64>(0, Draw - R(SequenceBudget)); }
	};

	struct FGpuRegions
	{
		int64 S = 0;
		int64 P = 0;
		int64 H = 0;
		int64 Pool() const { return S + P + H; }
	};

	inline FGpuRegions ExpectedRegions(const FGpuGeometry& G, const FSuperSLMGpuRuntimeConfig& C)
	{
		FGpuRegions Out;
		Out.S = int64(C.BlockCount) * G.R(C.SequenceBudget);
		Out.P = int64(C.PrefixBlockCount) * G.Pblk();
		Out.H = int64(C.RestoreHeadroomBlocks) * G.Hblk();
		return Out;
	}

	// §3.5's arrival rule, as the plan writes it, for a burst of N equal restores into an empty H with
	// every apply held: admitted while the charge fits and nobody waits; else waiting while
	// free + the guarantees ahead - the waiters' charges ahead >= the charge; else refused on arrival.
	struct FBurstCounts
	{
		int32 Admitted = 0;
		int32 Waiting = 0;
		int32 Refused = 0;
	};

	inline FBurstCounts BurstModel(int64 HFree, int64 Charge, int64 GuaranteeEach, int32 N)
	{
		FBurstCounts Out;
		int64 Free = HFree;
		int64 Guarantees = 0;
		int64 WaitingCharges = 0;
		for (int32 I = 0; I < N; ++I)
		{
			if (Out.Waiting == 0 && Charge <= Free)
			{
				Free -= Charge;
				Guarantees += GuaranteeEach;
				++Out.Admitted;
			}
			else if (Free + Guarantees - WaitingCharges >= Charge)
			{
				WaitingCharges += Charge;
				Guarantees += GuaranteeEach;
				++Out.Waiting;
			}
			else
			{
				++Out.Refused;
			}
		}
		return Out;
	}

	// --- The rig: a game world, the artifact, the GPU subsystem ---------------------------------

	struct FGpuPagingRig
	{
		FTestWorldWrapper World;
		EGpuArtifact Artifact = EGpuArtifact::AEx05B;
		USuperSLMModel* Model = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
		FGpuGeometry Geo;
		FSuperSLMGpuRuntimeConfig Config;

		bool Init(FAutomationTestBase& T, EGpuArtifact InArtifact = EGpuArtifact::AEx05B)
		{
			Artifact = InArtifact;
			if (!World.CreateTestWorld(EWorldType::Game))
			{
				T.AddError(TEXT("could not create a game world"));
				return false;
			}
			FString Path, Reason;
			if (!ResolveArtifact(Artifact, Path, Reason))
			{
				T.AddError(Reason);
				return false;
			}
			FSuperSLMImportDiagnostic Diag;
			Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
			if (!T.TestNotNull(*FString::Printf(TEXT("%s imports"), ArtifactName(Artifact)), Model) || !T.TestTrue(TEXT("the artifact is accepted"), Diag.bAccepted))
			{
				return false;
			}
			Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World.GetTestWorld());
			return T.TestNotNull(TEXT("the GPU subsystem (Win64 only)"), Gpu);
		}
	};

	// A configuration for the pooled cells. SequenceBudget 0 is dedicated mode. DispatchBudget is one
	// whole token (every layer), so a OneCall slice is one token and Configure's MinimumK is
	// max(BlockCount, BlockCount) = BlockCount; K = BlockCount + 1 is the floor once
	// PrefixBlockCount > 0 adds the build seat (§3.4 UP-11), and is above the floor otherwise.
	inline FSuperSLMGpuRuntimeConfig MakeGpuConfig(EGpuArtifact Artifact, int32 BlockCount, int32 SequenceBudget, int32 PrefixBlocks = 0, int32 HeadroomBlocks = 1,
		int64 ContextCap = PluginCap)
	{
		FSuperSLMGpuRuntimeConfig C;
		C.ContextCap = ContextCap;
		C.BlockCount = BlockCount;
		C.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(LayersOf(Artifact));
		C.K = BlockCount + 1;
		C.TickBudgetMs = 1000.0;
		C.MaxQueuedOperationsPerSequence = 4;
		C.FinishParallelTasks = 4;
		C.SequenceBudget = SequenceBudget;
		C.PrefixBlockCount = PrefixBlocks;
		C.RestoreHeadroomBlocks = HeadroomBlocks;
		C.CacheGhostBlocks = 1;
		return C;
	}

	// Reads B, the page size and the model's cap from the engine, on the submission thread.
	inline bool ReadGeometry(FAutomationTestBase& T, FGpuPagingRig& Rig)
	{
		FGpuGeometry G;
		G.Cp = Rig.Config.ContextCap;
		int64 PoolPages = -1;
		const bool bRan = FSuperSLMPagedGpuTestSeam::RunOnPoolThread(*Rig.Gpu, [&G, &PoolPages](FSuperSLMGpuPoolThreadAccess& A)
		{
			G.B = A.PagePositions();
			G.PageBytes = A.PageBytes();
			G.Ce = A.ModelCap();
			PoolPages = A.PoolPageCount();
		});
		if (!T.TestTrue(TEXT("FSuperSLMPagedGpuTestSeam::RunOnPoolThread runs (it reads the engine's geometry)"), bRan))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("the engine reports a page geometry"), G.IsValid()))
		{
			return false;
		}
		T.TestEqual(TEXT("the engine's model cap is the artifact's"), G.Ce, ExpectedModelCap(Rig.Artifact));
		T.AddInfo(FString::Printf(TEXT("geometry: B %lld, page bytes %lld, model cap %lld, ContextCap %lld, pool %lld pages"), G.B, G.PageBytes, G.Ce, G.Cp, PoolPages));
		Rig.Geo = G;
		return true;
	}

	inline bool ConfigureDedicated(FAutomationTestBase& T, FGpuPagingRig& Rig, const FSuperSLMGpuRuntimeConfig& C)
	{
		check(C.SequenceBudget == 0);
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, C);
		if (!T.TestEqual(*FString::Printf(TEXT("dedicated Configure() succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = C;
		return true;
	}

	// Configures pooled mode and checks it took: GetKvPageStats().bPagedMode, the engine's geometry,
	// then the ledger's regions against §3.2's sizes. This is where every pooled cell first fails
	// while pooled mode is unbuilt.
	inline bool ConfigurePooled(FAutomationTestBase& T, FGpuPagingRig& Rig, const FSuperSLMGpuRuntimeConfig& C)
	{
		check(C.SequenceBudget > 0);
		const FSuperSLMGpuConfigureReport Report = Rig.Gpu->Configure(Rig.Model, C);
		if (!T.TestEqual(*FString::Printf(TEXT("pooled Configure() succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = C;
		if (!T.TestTrue(TEXT("SequenceBudget > 0 configures pooled mode (GetKvPageStats().bPagedMode)"), Rig.Gpu->GetKvPageStats().bPagedMode))
		{
			return false;
		}
		if (!ReadGeometry(T, Rig))
		{
			return false;
		}
		const FGpuRegions Want = ExpectedRegions(Rig.Geo, C);
		const FSuperSLMPagedLedgerView V = FSuperSLMPagedGpuTestSeam::Ledger(*Rig.Gpu);
		bool bOk = T.TestTrue(TEXT("the ledger is configured"), V.bConfigured);
		bOk &= T.TestEqual(TEXT("region S = BlockCount x R(SequenceBudget), R at the model's cap"), V.CapacityOf(ESuperSLMPagedRegion::S), Want.S);
		bOk &= T.TestEqual(TEXT("region P = PrefixBlockCount x P_blk"), V.CapacityOf(ESuperSLMPagedRegion::P), Want.P);
		bOk &= T.TestEqual(TEXT("region H = RestoreHeadroomBlocks x H_blk"), V.CapacityOf(ESuperSLMPagedRegion::H), Want.H);
		bOk &= T.TestEqual(TEXT("region A is 0 (no cache in the paging release)"), V.CapacityOf(ESuperSLMPagedRegion::A), int64(0));
		bOk &= T.TestEqual(TEXT("region C is 0 (no cache in the paging release)"), V.CapacityOf(ESuperSLMPagedRegion::C), int64(0));
		bOk &= T.TestEqual(TEXT("the pool is S + P + H"), V.PoolPages, Want.Pool());
		bOk &= T.TestEqual(TEXT("S is fully charged at Configure (the warm reservations)"), V.ChargedOf(ESuperSLMPagedRegion::S), Want.S);
		return bOk;
	}

	// The constructions in the 0.5B cells are computed for B = 16 and a block of 256 pages (§2).
	inline bool RequireAExShape(FAutomationTestBase& T, const FGpuPagingRig& Rig)
	{
		return T.TestEqual(TEXT("A-EX on the GPU: B = 16 (the cell's construction is computed for it)"), Rig.Geo.B, int64(16))
			&& T.TestEqual(TEXT("A-EX on the GPU: a block of 256 pages"), Rig.Geo.BlockE(), int64(256))
			&& T.TestEqual(TEXT("A-EX on the GPU: P_blk = H_blk = 256 (one cap)"), Rig.Geo.Hblk(), int64(256));
	}

	// The [two-cap] cells are computed for B = 16, model cap 32,768 and ContextCap 4,096 (§7).
	inline bool RequireTwoCapShape(FAutomationTestBase& T, const FGpuPagingRig& Rig)
	{
		return T.TestEqual(TEXT("[two-cap]: B = 16"), Rig.Geo.B, int64(16))
			&& T.TestEqual(TEXT("[two-cap]: model cap 32,768"), Rig.Geo.Ce, int64(32768))
			&& T.TestEqual(TEXT("[two-cap]: ContextCap 4,096"), Rig.Geo.Cp, int64(4096))
			&& T.TestEqual(TEXT("[two-cap]: P_blk = 257"), Rig.Geo.Pblk(), int64(257))
			&& T.TestEqual(TEXT("[two-cap]: H_blk = 513"), Rig.Geo.Hblk(), int64(513));
	}

	// --- The fill probe (§3.9) -------------------------------------------------------------------

	// Creates budget holders on the subsystem's own pool whose reservations total exactly K pages
	// (budget 1 holds 2 pages, budget B + 1 holds 3, each checked against the engine's own
	// reservation), then releases every one. True when all were admitted, false when one was refused
	// SSLM_GPU_KV_POOL_EXHAUSTED, unset (with an error) for anything else.
	inline TOptional<bool> ProbeAdmits(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, int64 K)
	{
		if (K == 0)
		{
			return TOptional<bool>(true);
		}
		if (K < 2)
		{
			T.AddError(FString::Printf(TEXT("fill probe: k = %lld is below the probe's precondition k >= 2; the cell's pool must be sized to keep it"), K));
			return TOptional<bool>();
		}
		bool bAdmitted = true;
		FString Error;
		const bool bRan = FSuperSLMPagedGpuTestSeam::RunOnPoolThread(Gpu, [K, &bAdmitted, &Error](FSuperSLMGpuPoolThreadAccess& A)
		{
			const int64 B = A.PagePositions();
			const int64 Two = A.PagesForBudget(1);
			const int64 Three = A.PagesForBudget(int32(B + 1));
			if (Two != 2 || Three != 3)
			{
				Error = FString::Printf(TEXT("fill probe: the engine's R(1) = %lld and R(B + 1) = %lld, not 2 and 3"), Two, Three);
				return;
			}
			int64 Left = K;
			while (Left > 0 && Error.IsEmpty())
			{
				const int32 Budget = (Left % 2 == 1) ? int32(B + 1) : 1;
				const SslmGpuStatus Status = A.CreateBudgeted(Budget);
				if (Status == SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED)
				{
					bAdmitted = false;
					break;
				}
				if (Status != SslmGpuStatus::SSLM_OK)
				{
					Error = FString::Printf(TEXT("fill probe: sslm_gpu_seq_create_budgeted returned status %u"), uint32(Status));
					break;
				}
				Left -= (Budget == 1) ? 2 : 3;
			}
			A.ReleaseAll();
		});
		if (!bRan)
		{
			T.AddError(TEXT("fill probe: FSuperSLMPagedGpuTestSeam::RunOnPoolThread did not run"));
			return TOptional<bool>();
		}
		if (!Error.IsEmpty())
		{
			T.AddError(Error);
			return TOptional<bool>();
		}
		return TOptional<bool>(bAdmitted);
	}

	// The largest k in [2, Hi] the probe admits, by bisection on one state; -1 when even 2 is
	// refused. The probe's one-state form (§3.9, k >= 3 where a verdict rests on it).
	inline bool ProbeFree(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, int64 Hi, int64& OutFree)
	{
		OutFree = -1;
		TOptional<bool> Two = ProbeAdmits(T, Gpu, 2);
		if (!Two.IsSet())
		{
			return false;
		}
		if (!Two.GetValue())
		{
			return true;
		}
		int64 Lo = 2;
		while (Hi - Lo > 0)
		{
			const int64 Mid = Lo + (Hi - Lo + 1) / 2;
			TOptional<bool> At = ProbeAdmits(T, Gpu, Mid);
			if (!At.IsSet())
			{
				return false;
			}
			if (At.GetValue())
			{
				Lo = Mid;
			}
			else
			{
				Hi = Mid - 1;
			}
		}
		OutFree = Lo;
		return true;
	}

	// Form 1 (§3.9): the ledger's total charge is at least the probe's drawn pages, and every region's
	// charge is within its capacity. It grades the total, not the attribution: a charge booked to the
	// wrong region passes when the totals agree. Every claim the suite makes through form 1 is a total;
	// the one place attribution matters (which holder carries H) is graded per holder by
	// SequenceHeadroomHold in Restore.Headroom (the K5 coverage audit, N6).
	inline bool Form1ChargesCoverDrawn(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		const FSuperSLMPagedLedgerView V = FSuperSLMPagedGpuTestSeam::Ledger(Gpu);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the ledger is configured"), Instant), V.bConfigured))
		{
			return false;
		}
		bool bOk = true;
		for (int32 Region = 0; Region < int32(ESuperSLMPagedRegion::Count); ++Region)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: region %d charge %lld within its capacity %lld"), Instant, Region, V.Charged[Region], V.Capacity[Region]),
				V.Charged[Region] <= V.Capacity[Region]);
		}
		const int64 MustBeFree = V.PoolPages - V.TotalCharged;
		const TOptional<bool> Admits = ProbeAdmits(T, Gpu, MustBeFree);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: charges >= drawn (the probe admits pool %lld - charge %lld = %lld pages)"), Instant, V.PoolPages, V.TotalCharged, MustBeFree),
			Admits.IsSet() && Admits.GetValue());
		return bOk;
	}

	// The exact two-sided form (§3.9) at quiescence: the pool has exactly ExpectedDrawn pages drawn.
	// Rebuild replays the cell's construction after a re-Configure (true when it did).
	// A rebuilt prefix is byte-identical to the one it replaces: the same tokens through the same
	// deterministic prefill on the same model. Cells that share a prefix across a rebuild (6.2, 9.2,
	// and 3.2 (e)) rely on that (the K5 coverage audit, N7).
	inline bool ProbeDrawnExactly(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, int64 Pool, int64 ExpectedDrawn, TFunctionRef<bool()> Rebuild, const TCHAR* Instant)
	{
		const int64 K = Pool - ExpectedDrawn;
		const TOptional<bool> AtK = ProbeAdmits(T, Gpu, K);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the probe admits creates totalling %lld (drawn <= %lld)"), Instant, K, ExpectedDrawn), AtK.IsSet() && AtK.GetValue());
		if (!bOk)
		{
			return false;
		}
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the state is rebuilt for the probe's second side"), Instant), Rebuild()))
		{
			return false;
		}
		const TOptional<bool> AtK1 = ProbeAdmits(T, Gpu, K + 1);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: on the rebuild the probe refuses creates totalling %lld (drawn >= %lld)"), Instant, K + 1, ExpectedDrawn), AtK1.IsSet() && !AtK1.GetValue());
		return bOk;
	}

	// --- Driving the subsystem -----------------------------------------------------------------

	// Ticks until Done(), sleeping 1 ms while the next tick would be gated on the device. False on
	// the wall-clock bound.
	inline bool Pump(USuperSLMGpuSubsystem& Gpu, TFunctionRef<bool()> Done, double Seconds)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done())
		{
			if (FPlatformTime::Seconds() - Start > Seconds)
			{
				return false;
			}
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Gpu.Tick(Step);
			FPlatformProcess::Sleep(0.0005f);
		}
		return true;
	}

	// Exactly N ticks, each paced on the device.
	inline bool TickN(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, int32 N, const TCHAR* Where)
	{
		bool bOk = true;
		for (int32 I = 0; I < N && bOk; ++I)
		{
			bOk = SuperSLML2S2Fixtures::PacedTick(T, Gpu, Where);
		}
		return bOk;
	}

	// Ticks until no submission-thread job is outstanding (at least three ticks, so a request queued
	// this frame is admitted and its job posted first). A held apply is not outstanding work.
	inline bool DrainQuiet(USuperSLMGpuSubsystem& Gpu, double Seconds = OpSeconds)
	{
		int32 Ticks = 0;
		return Pump(Gpu, [&Gpu, &Ticks]()
		{
			return ++Ticks > 3 && FSuperSLMGpuTestAccess::GetOutstandingTickJobCount(Gpu) == 0 && FSuperSLMGpuTestAccess::GetOutstandingOtherJobCount(Gpu) == 0;
		}, Seconds);
	}

	inline ESuperSLMRestoreResult WaitOp(USuperSLMGpuSubsystem& Gpu, const FSuperSLMLifecycleOpHandle& Handle, double Seconds = OpSeconds)
	{
		if (!Handle.IsValid())
		{
			return ESuperSLMRestoreResult::Pending;
		}
		ESuperSLMRestoreResult Result = Gpu.GetLifecycleOpResult(Handle);
		Pump(Gpu, [&Gpu, &Handle, &Result]()
		{
			Result = Gpu.GetLifecycleOpResult(Handle);
			return Result != ESuperSLMRestoreResult::Pending;
		}, Seconds);
		return Result;
	}

	inline bool WaitHoldReached(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPagedHold& Hold, double Seconds = OpSeconds)
	{
		Pump(Gpu, [&Gpu, &Hold]() { return FSuperSLMPagedGpuTestSeam::IsHoldReached(Gpu, Hold); }, Seconds);
		return FSuperSLMPagedGpuTestSeam::IsHoldReached(Gpu, Hold);
	}

	inline bool Vend(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FSuperSLMGpuSequence& Out, ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::OneCall)
	{
		return T.TestEqual(TEXT("VendSequence()"), (uint8)Gpu.VendSequence(Out, Path), (uint8)ESuperSLMGpuVendResult::Success);
	}

	inline bool ReturnAndDrain(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq)
	{
		const int32 Slot = FSuperSLMPagedGpuTestSeam::SlotIndexOf(Gpu, Seq);
		Gpu.ReturnSequence(Seq);
		// The worker count reaches zero before PollRecycles applies its reset/dependency drop.
		// Keep the physical slot index captured before ReturnSequence removes the logical handle.
		const bool bApplied = Slot == INDEX_NONE || Pump(Gpu, [&Gpu, Slot]()
		{
			return !FSuperSLMPagedGpuTestSeam::IsSlotRecyclePending(Gpu, Slot);
		}, OpSeconds);
		return bApplied && DrainQuiet(Gpu);
	}

	inline bool WaitPrefixReady(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix, double Seconds = BuildSeconds)
	{
		Pump(Gpu, [&Gpu, &Prefix]()
		{
			const ESuperSLMPrefixPhase Phase = Gpu.GetPrefixPhase(Prefix);
			return Phase == ESuperSLMPrefixPhase::Ready || Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid;
		}, Seconds);
		return T.TestEqual(TEXT("the prefix reaches Ready"), (uint8)Gpu.GetPrefixPhase(Prefix), (uint8)ESuperSLMPrefixPhase::Ready);
	}

	inline bool BuildPrefix(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, FSuperSLMPrefix& Out)
	{
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefix(Tokens, Out, &Error);
		if (!T.TestEqual(*FString::Printf(TEXT("GPU CreatePrefix(%d tokens) is admitted (%s)"), Tokens.Num(), *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			return false;
		}
		return WaitPrefixReady(T, Gpu, Out);
	}

	inline bool BuildNested(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, FSuperSLMPrefix& Out)
	{
		FString Error;
		const ESuperSLMPrefixResult Result = Gpu.CreatePrefixFrom(Parent, Tokens, Out, &Error);
		if (!T.TestEqual(*FString::Printf(TEXT("GPU CreatePrefixFrom(%d tokens) is admitted (%s)"), Tokens.Num(), *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			return false;
		}
		return WaitPrefixReady(T, Gpu, Out);
	}

	inline bool ReleasePrefixAndDrain(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix)
	{
		FString Error;
		const bool bReleased = Gpu.ReleasePrefix(Prefix, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("GPU ReleasePrefix() (%s)"), *Error), bReleased))
		{
			return false;
		}
		// The worker's outstanding count drops before bDone is published. Even once bDone is
		// visible, PollPrefixBuilds() must apply the release before its refund/ghost is observable.
		// Wait for that apply explicitly; a worker-count drain alone can return one tick too early.
		const bool bApplied = Pump(Gpu, [&Gpu, &Prefix]()
		{
			return !FSuperSLMPagedGpuTestSeam::IsPrefixReleasePending(Gpu, Prefix);
		}, OpSeconds);
		return T.TestTrue(TEXT("the prefix release reaches its game-thread apply"), bApplied) && DrainQuiet(Gpu);
	}

	// An adopt (the prefix overload, §3.4), driven to its result and its handle released.
	inline ESuperSLMRestoreResult AdoptResult(USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const FSuperSLMPrefix& Prefix, FString* OutError = nullptr)
	{
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestAdoptPrefix(Seq, Prefix);
		if (!Handle.IsValid())
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("refused at the call: %s"), *Gpu.GetLastLifecycleRequestError());
			}
			return ESuperSLMRestoreResult::Pending;
		}
		const ESuperSLMRestoreResult Result = WaitOp(Gpu, Handle);
		if (OutError)
		{
			*OutError = Gpu.GetLastLifecycleRequestError();
		}
		Gpu.ReleaseLifecycleOpHandle(Handle);
		return Result;
	}

	inline bool AdoptAndWait(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const FSuperSLMPrefix& Prefix,
		ESuperSLMRestoreResult Expected = ESuperSLMRestoreResult::Success)
	{
		FString Error;
		const ESuperSLMRestoreResult Result = AdoptResult(Gpu, Seq, Prefix, &Error);
		return T.TestEqual(*FString::Printf(TEXT("the GPU adopt resolves %s (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Expected), SuperSLML2S2Fixtures::RestoreResultName(Result), *Error),
			(uint8)Result, (uint8)Expected);
	}

	inline bool ResetAndWait(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, ESuperSLMGpuDecodePath Path = ESuperSLMGpuDecodePath::OneCall)
	{
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestResetSequence(Seq, Path);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestResetSequence() queues (%s)"), *Gpu.GetLastLifecycleRequestError()), Handle.IsValid()))
		{
			return false;
		}
		const ESuperSLMRestoreResult Result = WaitOp(Gpu, Handle);
		Gpu.ReleaseLifecycleOpHandle(Handle);
		return T.TestEqual(TEXT("the reset succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
	}

	struct FGpuGeneration
	{
		bool bRefusedAtCall = false;                                      // RequestBeginGeneration() returned an invalid handle
		ESuperSLMRestoreResult Started = ESuperSLMRestoreResult::Pending; // the request's handle
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMGpuFaultReason Fault = ESuperSLMGpuFaultReason::None;
		TArray<int32> Tokens;
		FString Error;                                                    // GetLastLifecycleRequestError() at the end
	};

	// Runs one generation to its end (Complete or Faulted, nothing pending) and reports how it ended.
	// A stop at the holder's limit is Faulted with KvBudgetExceeded (§3.4), and a refusal at the call
	// is reported, not failed: the caller asserts the end it expects. False only when the generation
	// did not end in time.
	inline bool Generate(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const TArray<int32>& Prompt, int32 MaxNew,
		FGpuGeneration& Out, double Seconds = GenerateSeconds, ESuperSLMSpanKind SpanKind = ESuperSLMSpanKind::Prompt)
	{
		Out = FGpuGeneration();
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNew;
		Request.SpanKind = SpanKind;
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestBeginGeneration(Seq, Request);
		if (!Handle.IsValid())
		{
			Out.bRefusedAtCall = true;
			Out.Error = Gpu.GetLastLifecycleRequestError();
			Out.Phase = Gpu.GetPhase(Seq);
			Out.Fault = Gpu.GetLastFaultReason(Seq);
			return true;
		}
		bool bStarted = false;
		auto Ended = [&Gpu, &Seq, &Handle, &Out, &bStarted]()
		{
			if (!bStarted)
			{
				Out.Started = Gpu.GetLifecycleOpResult(Handle);
				if (Out.Started == ESuperSLMRestoreResult::Pending)
				{
					return false;
				}
				bStarted = true;
			}
			const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
			return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted || Out.Started != ESuperSLMRestoreResult::Success)
				&& Gpu.GetPendingLifecycleOperationCount(Seq) == 0;
		};
		const bool bEnded = Pump(Gpu, Ended, Seconds);
		Out.Phase = Gpu.GetPhase(Seq);
		Out.Fault = Gpu.GetLastFaultReason(Seq);
		Out.Tokens = Gpu.GetGeneratedTokens(Seq);
		Out.Error = Gpu.GetLastLifecycleRequestError();
		Gpu.ReleaseLifecycleOpHandle(Handle);
		return T.TestTrue(TEXT("the generation ends"), bEnded);
	}

	inline bool GenerateComplete(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const TArray<int32>& Prompt, int32 MaxNew,
		TArray<int32>& OutTokens, ESuperSLMSpanKind SpanKind = ESuperSLMSpanKind::Prompt)
	{
		FGpuGeneration G;
		if (!Generate(T, Gpu, Seq, Prompt, MaxNew, G, GenerateSeconds, SpanKind))
		{
			return false;
		}
		OutTokens = G.Tokens;
		return T.TestFalse(*FString::Printf(TEXT("the generation is accepted (%s)"), *G.Error), G.bRefusedAtCall)
			&& T.TestEqual(TEXT("the generation completes at MaxNewTokens"), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Complete)
			&& T.TestEqual(TEXT("MaxNewTokens tokens"), G.Tokens.Num(), MaxNew);
	}

	// Saves a sequence and takes its blob; the save's handle is released.
	inline bool SaveAndTake(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, TArray<uint8>& OutBlob)
	{
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestSaveSequence(Seq);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestSaveSequence() queues (%s)"), *Gpu.GetLastLifecycleRequestError()), Handle.IsValid()))
		{
			return false;
		}
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		Pump(Gpu, [&Gpu, &Handle, &Result, &OutBlob]()
		{
			Result = Gpu.GetSaveResult(Handle, OutBlob);
			return Result != ESuperSLMRestoreResult::Pending;
		}, OpSeconds);
		Gpu.ReleaseLifecycleOpHandle(Handle);
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the save has bytes"), OutBlob.Num() > 0);
	}

	// One issued restore: its handle (invalid when refused at the call) and the error read then.
	struct FIssuedGpuRestore
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString CallError;
	};

	inline FIssuedGpuRestore IssueRestore(USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, USuperSLMModel* Model, const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		FIssuedGpuRestore Out;
		Out.Handle = Hint.IsValid() ? Gpu.RequestRestoreSequence(Blob, Model, Hint) : Gpu.RequestRestoreSequence(Blob, Model);
		if (!Out.Handle.IsValid())
		{
			Out.CallError = Gpu.GetLastLifecycleRequestError();
		}
		return Out;
	}

	// The restore's outcome so far, through its handle. Since plan rev 1.9 (ruling 1; the K5 record,
	// reading R3), the refusals the payload header decides (Malformed, ContextCapMismatch,
	// KvModeMismatch by magic) are made at the call, with no handle: the Restore tests grade those with
	// ExpectRefusedAtCall. Only KvPagesExhausted on arrival and the engine's refusals after admission
	// resolve through a handle. A refusal at the call reads Pending here.
	inline ESuperSLMRestoreResult OutcomeOf(const USuperSLMGpuSubsystem& Gpu, const FIssuedGpuRestore& R, FSuperSLMGpuSequence& OutSeq)
	{
		OutSeq = FSuperSLMGpuSequence();
		if (!R.Handle.IsValid())
		{
			return ESuperSLMRestoreResult::Pending;
		}
		return Gpu.GetRestoreResult(R.Handle, OutSeq);
	}

	inline ESuperSLMRestoreResult SettleOutcome(USuperSLMGpuSubsystem& Gpu, const FIssuedGpuRestore& R, FSuperSLMGpuSequence& OutSeq, double Seconds = OpSeconds)
	{
		ESuperSLMRestoreResult Result = OutcomeOf(Gpu, R, OutSeq);
		if (!R.Handle.IsValid())
		{
			return Result;
		}
		Pump(Gpu, [&Gpu, &R, &Result, &OutSeq]()
		{
			Result = Gpu.GetRestoreResult(R.Handle, OutSeq);
			return Result != ESuperSLMRestoreResult::Pending;
		}, Seconds);
		return Result;
	}

	inline ESuperSLMRestoreResult RestoreAndWait(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, USuperSLMModel* Model,
		FSuperSLMGpuSequence& OutSeq, const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		const FIssuedGpuRestore R = IssueRestore(Gpu, Blob, Model, Hint);
		if (!R.Handle.IsValid())
		{
			T.AddInfo(FString::Printf(TEXT("restore refused at the call: %s"), *R.CallError));
			OutSeq = FSuperSLMGpuSequence();
			return ESuperSLMRestoreResult::Pending;
		}
		const ESuperSLMRestoreResult Result = SettleOutcome(Gpu, R, OutSeq);
		Gpu.ReleaseLifecycleOpHandle(R.Handle);
		return Result;
	}

	inline bool ApplyRestore(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, USuperSLMModel* Model, FSuperSLMGpuSequence& OutSeq,
		const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		const ESuperSLMRestoreResult Result = RestoreAndWait(T, Gpu, Blob, Model, OutSeq, Hint);
		return T.TestEqual(*FString::Printf(TEXT("the restore succeeds (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Result), *Gpu.GetLastLifecycleRequestError()),
			(uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
	}

	// Gives back whatever an issued restore vended, and releases its handle.
	inline void Discard(USuperSLMGpuSubsystem& Gpu, const FIssuedGpuRestore& R)
	{
		FSuperSLMGpuSequence Seq;
		OutcomeOf(Gpu, R, Seq);
		if (Seq.IsValid())
		{
			Gpu.ReturnSequence(Seq);
		}
		if (R.Handle.IsValid())
		{
			Gpu.ReleaseLifecycleOpHandle(R.Handle);
		}
		DrainQuiet(Gpu);
	}

	// Refused KvPagesExhausted on arrival (§3.2, §3.5): resolved by name within a short bound, with
	// no charge taken on H.
	inline bool ExpectRefusedOnArrival(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FIssuedGpuRestore& R, int64 HChargedBefore, const TCHAR* What)
	{
		FSuperSLMGpuSequence Seq;
		const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, R, Seq, 10.0);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: refused KvPagesExhausted on arrival (got %s; %s%s)"), What, SuperSLML2S2Fixtures::RestoreResultName(Outcome), *R.CallError, *Gpu.GetLastLifecycleRequestError()),
			(uint8)Outcome, (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: no sequence"), What), Seq.IsValid());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no charge is taken on H"), What), FSuperSLMPagedGpuTestSeam::Ledger(Gpu).ChargedOf(ESuperSLMPagedRegion::H), HChargedBefore);
		return bOk;
	}

	// "Refused before any job" (K1 coverage audit B2): the caller armed Hold (a HoldApply on Kind)
	// and read JobsBefore = JobsPosted(Kind) before issuing; after a few ticks the hold was never
	// reached and no job of Kind was posted. A hold alone is satisfied by a job that runs and fails.
	inline bool ExpectNoJobPosted(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRequestKind Kind, const FSuperSLMPagedHold& Hold, int64 JobsBefore,
		const TCHAR* What)
	{
		TickN(T, Gpu, 4, What);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the HoldApply was armed"), What), Hold.IsValid());
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: the hold is never reached (no job ran to its apply)"), What), FSuperSLMPagedGpuTestSeam::IsHoldReached(Gpu, Hold));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: JobsPosted is readable"), What), JobsBefore >= 0);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no job was posted"), What), FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, Kind), JobsBefore);
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold); // disarms it
		return bOk;
	}

	// --- Tokens --------------------------------------------------------------------------------

	// Count tokens of real text through the model's own tokenizer, repeated as needed. Salt rotates
	// the start, so two salts give different content of the same length.
	inline bool MakeTokens(FAutomationTestBase& T, const USuperSLMModel& Model, int32 Count, int32 Salt, TArray<int32>& Out)
	{
		static const TCHAR* const Text = TEXT(
			"The lighthouse keeper of Saltmere kept a ledger of every ship that passed: its flag, its cargo, "
			"the hour it cleared the reef and the name of the pilot who brought it in. She traded rope for "
			"lamp oil with the fishing families, mended nets in winter, and told travellers which roads "
			"flooded in spring. When asked about the old smugglers' cave she smiled and changed the subject, "
			"though she knew every tunnel beneath the cliffs and the tide that filled each one. ");
		TArray<int32> Base;
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("the persona text tokenizes (%s)"), *Error), SuperSLM::TokenizeText(Model, Text, Base, Error) && Base.Num() > 16))
		{
			return false;
		}
		Out.Reset(Count);
		const int32 Start = (Salt * 37) % Base.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			Out.Add(Base[(Start + I) % Base.Num()]);
		}
		return true;
	}

	inline TArray<int32> WithTokenChanged(const TArray<int32>& Tokens, int32 Position)
	{
		TArray<int32> Out = Tokens;
		for (int32 I = 0; I < Tokens.Num(); ++I)
		{
			if (Tokens[I] != Tokens[Position])
			{
				Out[Position] = Tokens[I];
				break;
			}
		}
		return Out;
	}

	inline TArray<int32> Concat(const TArray<int32>& A, const TArray<int32>& B)
	{
		TArray<int32> Out = A;
		Out.Append(B);
		return Out;
	}

	// --- The Layer-1 payload header, at the plan's §3.5 offsets (the oracle states them) --------

	namespace GpuPayloadHeader
	{
		constexpr int32 Magic = 0;
		constexpr int32 ContextLength = 32; // i64, SLM5 and SLM6
		constexpr int32 Ready = 128;        // u32, SLM5 and SLM6; any non-zero value is ready
		constexpr int32 KvMode = 132;       // u32, SLM6
		constexpr int32 Budget = 136;       // i32, SLM6
		constexpr int32 Origin = 140;       // i64, SLM6
		constexpr int32 Slm6HeaderEnd = 148;

		inline bool MagicIs(const TArray<uint8>& Payload, const char* FourCC)
		{
			return Payload.Num() >= 4 && Payload[0] == uint8(FourCC[0]) && Payload[1] == uint8(FourCC[1]) && Payload[2] == uint8(FourCC[2]) && Payload[3] == uint8(FourCC[3]);
		}

		inline int64 ReadLE(const TArray<uint8>& P, int32 Offset, int32 Bytes)
		{
			uint64 V = 0;
			for (int32 I = Bytes - 1; I >= 0; --I)
			{
				V = (V << 8) | uint64(P[Offset + I]);
			}
			if (Bytes == 4)
			{
				return int64(int32(uint32(V)));
			}
			return int64(V);
		}

		inline void WriteLE(TArray<uint8>& P, int32 Offset, int32 Bytes, int64 Value)
		{
			uint64 V = uint64(Value);
			for (int32 I = 0; I < Bytes; ++I)
			{
				P[Offset + I] = uint8(V & 0xFF);
				V >>= 8;
			}
		}
	}

	inline bool ReadPayload(FAutomationTestBase& T, const TArray<uint8>& Blob, TArray<uint8>& OutPayload)
	{
		TArray<uint8> Copy = Blob;
		const bool bOk = FSuperSLMPagedTestSeam::EditPayload(Copy, [&OutPayload](TArray<uint8>& Payload) { OutPayload = Payload; });
		return T.TestTrue(TEXT("FSuperSLMPagedTestSeam::EditPayload reads the payload (the wrapper is shared by both backends)"), bOk && OutPayload.Num() > 0);
	}

	struct FGpuPayloadHeaderFields
	{
		bool bSlm6 = false;
		int64 ContextLength = -1;
		int64 Ready = -1;
		int64 KvMode = -1;
		int64 Budget = -1;
		int64 Origin = -1;
	};

	// The 'SLM5'/'SLM6' header's fields, read by the test at §3.5's offsets.
	inline bool ReadGpuHeader(FAutomationTestBase& T, const TArray<uint8>& Blob, FGpuPayloadHeaderFields& Out)
	{
		TArray<uint8> Payload;
		if (!ReadPayload(T, Blob, Payload))
		{
			return false;
		}
		Out.bSlm6 = GpuPayloadHeader::MagicIs(Payload, "SLM6");
		const bool bSlm5 = GpuPayloadHeader::MagicIs(Payload, "SLM5");
		if (!T.TestTrue(TEXT("the payload is 'SLM5' or 'SLM6'"), (Out.bSlm6 || bSlm5) && Payload.Num() >= GpuPayloadHeader::Slm6HeaderEnd))
		{
			return false;
		}
		Out.ContextLength = GpuPayloadHeader::ReadLE(Payload, GpuPayloadHeader::ContextLength, 8);
		Out.Ready = int64(uint32(GpuPayloadHeader::ReadLE(Payload, GpuPayloadHeader::Ready, 4)));
		if (Out.bSlm6)
		{
			Out.KvMode = int64(uint32(GpuPayloadHeader::ReadLE(Payload, GpuPayloadHeader::KvMode, 4)));
			Out.Budget = GpuPayloadHeader::ReadLE(Payload, GpuPayloadHeader::Budget, 4);
			Out.Origin = GpuPayloadHeader::ReadLE(Payload, GpuPayloadHeader::Origin, 8);
		}
		return true;
	}

	// Cell 10.4 (a)'s oracle (§7): the engine's own context_length, read from the 'SLM6' header of a
	// save taken now (offset 32, exact), equals Expected; the plugin's ContextUsed equals it; and the
	// plugin's armed bit equals the header's ready_for_logits. No plugin arithmetic is in the
	// reference.
	inline bool PositionOracle(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, int64 Expected, const TCHAR* What,
		TArray<uint8>* OutBlob = nullptr)
	{
		TArray<uint8> Blob;
		FGpuPayloadHeaderFields H;
		if (!SaveAndTake(T, Gpu, Seq, Blob) || !ReadGpuHeader(T, Blob, H))
		{
			return false;
		}
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: a pooled sequence saves 'SLM6'"), What), H.bSlm6);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the engine's context_length (the 'SLM6' header at 32)"), What), H.ContextLength, Expected);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the plugin's ContextUsed equals the engine's context_length"), What),
			FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), H.ContextLength);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the plugin's armed bit equals the header's ready_for_logits"), What),
			int64(FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq)), int64(H.Ready != 0 ? 1 : 0));
		if (OutBlob)
		{
			*OutBlob = MoveTemp(Blob);
		}
		return bOk;
	}

	// --- Blobs ---------------------------------------------------------------------------------

	// A pooled save of a holder that adopted Prefix, fed Suffix and MaxNew new tokens. The sequence
	// is returned before this returns.
	inline bool MakeAdoptedBlob(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Prefix, const TArray<int32>& Suffix, int32 MaxNew,
		TArray<uint8>& OutBlob, TArray<int32>* OutTokens = nullptr)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq) || !AdoptAndWait(T, Gpu, Seq, Prefix))
		{
			return false;
		}
		TArray<int32> Tokens;
		const bool bOk = GenerateComplete(T, Gpu, Seq, Suffix, MaxNew, Tokens) && SaveAndTake(T, Gpu, Seq, OutBlob);
		if (OutTokens)
		{
			*OutTokens = Tokens;
		}
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	// A save of a plain holder (no prefix) fed Prompt and MaxNew new tokens: 'SLM5' in dedicated
	// mode, 'SLM6' with origin 0 in pooled mode.
	inline bool MakePlainBlob(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Prompt, int32 MaxNew, TArray<uint8>& OutBlob,
		TArray<int32>* OutTokens = nullptr)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq))
		{
			return false;
		}
		TArray<int32> Tokens;
		const bool bOk = GenerateComplete(T, Gpu, Seq, Prompt, MaxNew, Tokens) && SaveAndTake(T, Gpu, Seq, OutBlob);
		if (OutTokens)
		{
			*OutTokens = Tokens;
		}
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	// A pooled blob at a chosen saved budget and origin, so its restore charge is R(Budget) +
	// E(Budget, Origin) at the model's cap (§3.5). Configures pooled mode at (1 slot, Budget, 1 prefix
	// block, 1 headroom block), builds a prefix of Origin tokens from Salt (none at origin 0), and
	// saves an adopter of it that took a short suffix (none when the limit leaves no room) and one new
	// token. The rig is left at that shape: the caller re-Configures its own. OutPrefixTokens
	// receives the prefix's tokens, so a cell can rebuild the same prefix as a sharing hint.
	inline bool MakeBlobAt(FAutomationTestBase& T, FGpuPagingRig& Rig, int32 Budget, int32 Origin, int32 Salt, TArray<uint8>& OutBlob,
		TArray<int32>* OutPrefixTokens = nullptr)
	{
		if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 1, Budget, Origin > 0 ? 1 : 0, 1)))
		{
			return false;
		}
		const int64 Limit = FMath::Min<int64>(int64(Origin) + Budget, Rig.Geo.Cp);
		const int32 Room = int32(Limit - Origin);
		TArray<int32> Suffix;
		if (!MakeTokens(T, *Rig.Model, FMath::Clamp(Room - 2, Origin > 0 ? 0 : 1, 4), Salt + 101, Suffix))
		{
			return false;
		}
		if (Origin == 0)
		{
			return MakePlainBlob(T, *Rig.Gpu, Suffix, 1, OutBlob);
		}
		TArray<int32> PrefixTokens;
		FSuperSLMPrefix Prefix;
		if (!MakeTokens(T, *Rig.Model, Origin, Salt, PrefixTokens) || !BuildPrefix(T, *Rig.Gpu, PrefixTokens, Prefix))
		{
			return false;
		}
		const bool bOk = MakeAdoptedBlob(T, *Rig.Gpu, Prefix, Suffix, 1, OutBlob);
		FString Ignored;
		Rig.Gpu->ReleasePrefix(Prefix, Ignored);
		DrainQuiet(*Rig.Gpu);
		if (OutPrefixTokens)
		{
			*OutPrefixTokens = PrefixTokens;
		}
		return bOk;
	}

	// A dedicated-mode ('SLM5') blob of a plain holder. Leaves the rig in dedicated mode.
	inline bool MakeSlm5Blob(FAutomationTestBase& T, FGpuPagingRig& Rig, TArray<uint8>& OutBlob, TArray<int32>* OutTokens = nullptr)
	{
		TArray<int32> Prompt;
		return ConfigureDedicated(T, Rig, MakeGpuConfig(Rig.Artifact, 1, 0, 0, 1)) && MakeTokens(T, *Rig.Model, 8, 7, Prompt)
			&& MakePlainBlob(T, *Rig.Gpu, Prompt, 2, OutBlob, OutTokens);
	}

	// A ghost (§3.2): a prefix built, adopted by one vended sequence, and released. The adopter stays
	// vended and keeps the ghost's whole charge on P; the ghost no longer counts against
	// PrefixBlockCount.
	inline bool MakeGhost(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FSuperSLMGpuSequence& OutAdopter)
	{
		if (!BuildPrefix(T, Gpu, Tokens, OutPrefix) || !Vend(T, Gpu, OutAdopter) || !AdoptAndWait(T, Gpu, OutAdopter, OutPrefix)
			|| !ReleasePrefixAndDrain(T, Gpu, OutPrefix))
		{
			return false;
		}
		return T.TestTrue(TEXT("the released prefix is a ghost while its adopter holds it"), FSuperSLMPagedGpuTestSeam::IsPrefixGhost(Gpu, OutPrefix));
	}

	// --- Logs and faults -----------------------------------------------------------------------

	// Captures every log line, from construction to destruction, from any thread, so a cell can count
	// a token in it (and assert it absent, which AddExpectedError cannot).
	struct FGpuPagingLogCapture final : FOutputDevice
	{
		FGpuPagingLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FGpuPagingLogCapture() override { GLog->RemoveOutputDevice(this); }
		FGpuPagingLogCapture(const FGpuPagingLogCapture&) = delete;
		FGpuPagingLogCapture& operator=(const FGpuPagingLogCapture&) = delete;

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
		{
			FScopeLock Lock(&Guard);
			Lines.Add(Message);
		}

		int32 CountContaining(const TCHAR* Token) const
		{
			FScopeLock Lock(&Guard);
			int32 Count = 0;
			for (const FString& Line : Lines)
			{
				Count += Line.Contains(Token) ? 1 : 0;
			}
			return Count;
		}

		int32 CountContainingBoth(const TCHAR* A, const TCHAR* B) const
		{
			FScopeLock Lock(&Guard);
			int32 Count = 0;
			for (const FString& Line : Lines)
			{
				Count += (Line.Contains(A) && Line.Contains(B)) ? 1 : 0;
			}
			return Count;
		}

	private:
		mutable FCriticalSection Guard;
		TArray<FString> Lines;
	};

	inline bool NoLedgerFault(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: LedgerFaults is 0 (GetKvPageStats)"), Instant), int64(Gpu.GetKvPageStats().LedgerFaults), int64(0))
			&& T.TestEqual(*FString::Printf(TEXT("%s: LedgerFaults is 0 (Ledger seam)"), Instant), int64(FSuperSLMPagedGpuTestSeam::Ledger(Gpu).LedgerFaults), int64(0));
	}

	// --- Ledger readings -----------------------------------------------------------------------

	inline FSuperSLMPagedLedgerView LedgerOf(const USuperSLMGpuSubsystem& Gpu)
	{
		return FSuperSLMPagedGpuTestSeam::Ledger(Gpu);
	}

	inline int64 Charged(const USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRegion Region)
	{
		return LedgerOf(Gpu).ChargedOf(Region);
	}

	inline int64 Free(const USuperSLMGpuSubsystem& Gpu, ESuperSLMPagedRegion Region)
	{
		return LedgerOf(Gpu).FreeOf(Region);
	}

	inline const TCHAR* FaultName(ESuperSLMGpuFaultReason Reason)
	{
		switch (Reason)
		{
			case ESuperSLMGpuFaultReason::None: return TEXT("None");
			case ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection: return TEXT("RecoverablePerSequenceRejection");
			case ESuperSLMGpuFaultReason::TerminalDeviceLost: return TEXT("TerminalDeviceLost");
			case ESuperSLMGpuFaultReason::KvBudgetExceeded: return TEXT("KvBudgetExceeded");
		}
		return TEXT("an unnamed fault reason");
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
