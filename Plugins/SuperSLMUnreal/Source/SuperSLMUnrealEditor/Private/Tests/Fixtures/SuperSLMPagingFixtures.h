#pragma once

// The CPU paged-KV suite's shared fixture (plugin paging plan §3.9 and §7; step K1). Test code
// only: compiled inside WITH_DEV_AUTOMATION_TESTS translation units, like every header beside it.
//
// What lives here, and why it is written out rather than read from the product:
//   - The page arithmetic (R, E, the block, the regions) is the plan's formulas (§2 E1, E2, E6;
//     §3.2), written from the plan text. No plugin or ledger-core function is called for an
//     expected figure, so an oracle cannot inherit the defect it grades.
//   - B and the cap are read from the engine (sslm_kv_page_positions; the largest budget
//     sslm_kv_pages_for_budget accepts) through FSuperSLMPagedTestSeam::RunOnPoolThread, never
//     assumed (E1), and checked against the artifact's known cap.
//   - The fill probe (§3.9) is the reference every page-count verdict uses: creates through the
//     engine's own sslm_seq_create_budgeted on the subsystem's own pool, sized from the engine's
//     own sslm_kv_pages_for_budget. Stats are never a verdict (E13).
//
// Grading forms (§3.9):
//   Form 1  Form1ChargesCoverDrawn(): the ledger's total charge is at least the probe's drawn
//           pages. One-sided, so it needs no rebuild: drawn <= charge exactly when the probe
//           admits creates totalling (pool - charge).
//   Exact   ProbeDrawnExactly(): the two-sided form at quiescence. Creates totalling k are admitted;
//           then, on a fresh rebuild of the same state (the caller's Rebuild), creates totalling
//           k + 1 are not all admitted. k >= 2 (the engine probe's precondition), so every pool here is sized to
//           keep it.
//   Form 2  is written in each cell: a boundary pair at (correct charge - 1) and (correct charge)
//           free pages.
//   Form 3  is written in each cell: zero slack outside the region under test.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Optional.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMPagedKvTypes.h"
#include "SuperSLMPagedTestSeam.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

namespace SuperSLMPagingFixtures
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr double OpSeconds = 120.0;        // one queued lifecycle operation
	constexpr double BuildSeconds = 900.0;     // a prefix build of up to a cap's tokens on the CPU
	constexpr double GenerateSeconds = 900.0;  // one generation to its stop

	// --- Artifacts ---------------------------------------------------------------------------

	enum class EPagingArtifact : uint8
	{
		AEx05B,     // A-EX, Qwen2.5-0.5B at cap 4,096: B = 64, a block of 64 pages (plan §2)
		Shipped15B, // the shipped 1.5B artifact, model cap 32,768: B = 64, a block of 512 pages
	};

	inline int64 ExpectedCap(EPagingArtifact Artifact)
	{
		return Artifact == EPagingArtifact::AEx05B ? 4096 : 32768;
	}

	inline const TCHAR* ArtifactName(EPagingArtifact Artifact)
	{
		return Artifact == EPagingArtifact::AEx05B ? TEXT("A-EX (0.5B, cap 4,096)") : TEXT("the shipped 1.5B artifact (cap 32,768)");
	}

	// SUPERSLM_PAGING_1P5B_PATH names the shipped 1.5B artifact; unset, the A-AD base model's path is
	// tried. Its cap is checked against 32,768 once the engine has read it (ReadGeometry).
	inline bool ResolveArtifact(EPagingArtifact Artifact, FString& OutPath, FString& OutReason)
	{
		if (Artifact == EPagingArtifact::AEx05B)
		{
			return SuperSLML2S1Fixtures::TryGetAExArtifactPath(OutPath, OutReason);
		}
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_PAGING_1P5B_PATH"));
		OutPath = Override.IsEmpty() ? SuperSLML2S1Fixtures::AAdBaseModelPath() : Override;
		if (!IFileManager::Get().FileExists(*OutPath))
		{
			OutReason = FString::Printf(TEXT("the shipped 1.5B artifact (model cap 32,768) was not found at '%s'; set SUPERSLM_PAGING_1P5B_PATH to it"), *OutPath);
			return false;
		}
		return true;
	}

	// --- The plan's page arithmetic, written from the plan (§2 E1, E2, E6; §3.2, §3.3) ----------
	//
	// The CPU's plugin cap is the model's cap (Cp = Ce), so both blocks are ceil(cap / B).

	struct FCpuPagingGeometry
	{
		int64 B = 0;         // positions per page, from the engine
		int64 Cap = 0;       // the model's context_cap, from the engine
		int64 PageBytes = 0; // from the engine

		bool IsValid() const { return B > 0 && Cap > 0 && PageBytes > 0; }

		static int64 CeilDiv(int64 A, int64 D) { return (A + D - 1) / D; }

		int64 Block() const { return CeilDiv(Cap, B); }                                      // ceil(cap / B)
		int64 R(int64 Budget) const { return FMath::Min(CeilDiv(Budget, B) + 1, Block()); } // E2
		int64 E(int64 Budget, int64 Origin) const { return FMath::Min(Origin / B, Block() - R(Budget)); } // E6
		int64 RestoreCharge(int64 Budget, int64 Origin) const { return R(Budget) + E(Budget, Origin); }  // §3.5, s = 0
		int64 FullPages(int64 Length) const { return Length / B; }
		int64 OccupiedPages(int64 Length) const { return Length > 0 ? CeilDiv(Length, B) : 0; }
		int64 PrefixCharge(int64 Tokens) const { return R(FMath::Max<int64>(Tokens, 1)); }  // §3.3 "Empty prefixes"
		int64 NestedPrivate(int64 ParentLength, int64 ChildTokens) const                    // §3.3
		{
			return CeilDiv(ParentLength + ChildTokens, B) - ParentLength / B;
		}
		// §3.5 "Guaranteed return": min(charge, R(SequenceBudget)).
		int64 Guarantee(int64 Charge, int64 SequenceBudget) const { return FMath::Min(Charge, R(SequenceBudget)); }
		// §3.5 "Settle at apply": max(0, draw - R(SequenceBudget)).
		int64 HeadroomHold(int64 Draw, int64 SequenceBudget) const { return FMath::Max<int64>(0, Draw - R(SequenceBudget)); }
	};

	// The regions a CPU budget-mode Configure must size (§3.2; A and C are 0 in the paging release).
	struct FRegions
	{
		int64 S = 0;
		int64 P = 0;
		int64 H = 0;
		int64 Pool() const { return S + P + H; }
	};

	inline FRegions ExpectedRegions(const FCpuPagingGeometry& G, const FSuperSLMRuntimeConfig& C)
	{
		FRegions Out;
		Out.S = int64(C.BlockCount) * G.R(C.SequenceBudget);
		Out.P = int64(C.PrefixBlockCount) * G.Block();
		Out.H = int64(C.RestoreHeadroomBlocks) * G.Block();
		return Out;
	}

	// --- The rig: a game world, the artifact, the CPU subsystem ---------------------------------

	struct FPagingRig
	{
		FTestWorldWrapper World;
		EPagingArtifact Artifact = EPagingArtifact::AEx05B;
		USuperSLMModel* Model = nullptr;
		USuperSLMSubsystem* Cpu = nullptr;
		FCpuPagingGeometry Geo;
		FSuperSLMRuntimeConfig Config;

		bool Init(FAutomationTestBase& T, EPagingArtifact InArtifact = EPagingArtifact::AEx05B)
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
			Cpu = SuperSLML2S1Fixtures::GetSubsystem(World.GetTestWorld());
			return T.TestNotNull(TEXT("the CPU subsystem"), Cpu);
		}
	};

	// A configuration for the paged cells. SequenceBudget 0 is legacy mode. Two lanes, so a probe
	// can run beside a job held at its start (§3.9 HoldStart) when BlockCount >= 2.
	inline FSuperSLMRuntimeConfig MakeConfig(int32 BlockCount, int32 SequenceBudget, int32 PrefixBlocks = 0, int32 HeadroomBlocks = 1)
	{
		FSuperSLMRuntimeConfig C;
		C.BlockCount = BlockCount;
		C.PrefixBlockCount = PrefixBlocks;
		C.SequenceBudget = SequenceBudget;
		C.RestoreHeadroomBlocks = HeadroomBlocks;
		C.CacheGhostBlocks = 1;
		C.MaxSequencesPerDecodeCall = FMath::Clamp(BlockCount, 1, 8);
		C.MaxPrefillChunkBudget = 64;
		C.MaxLayerBudget = 24;
		C.SequenceLifecycleBudgetMs = 100000.0;
		C.TickBudgetMs = 16.6;
		C.MaxConcurrentCalls = 2;
		return C;
	}

	// Reads B, the page size and the cap from the engine, on the subsystem's own lane. The cap is
	// the largest budget sslm_kv_pages_for_budget accepts (it returns 0 outside [1, cap]).
	inline bool ReadGeometry(FAutomationTestBase& T, FPagingRig& Rig)
	{
		FCpuPagingGeometry G;
		const bool bRan = FSuperSLMPagedTestSeam::RunOnPoolThread(*Rig.Cpu, [&G](FSuperSLMPoolThreadAccess& A)
		{
			G.B = A.PagePositions();
			G.PageBytes = A.PageBytes();
			int64 Lo = 1;
			int64 Hi = 1;
			while (Hi < (int64(1) << 30) && A.PagesForBudget(int32(Hi)) > 0)
			{
				Lo = Hi;
				Hi *= 2;
			}
			while (Hi - Lo > 1)
			{
				const int64 Mid = Lo + (Hi - Lo) / 2;
				(A.PagesForBudget(int32(Mid)) > 0 ? Lo : Hi) = Mid;
			}
			G.Cap = A.PagesForBudget(int32(Lo)) > 0 ? Lo : 0;
		});
		if (!T.TestTrue(TEXT("FSuperSLMPagedTestSeam::RunOnPoolThread runs (it reads the engine's geometry)"), bRan))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("the engine reports a page geometry"), G.IsValid()))
		{
			return false;
		}
		T.TestEqual(TEXT("the engine's cap is the artifact's"), G.Cap, ExpectedCap(Rig.Artifact));
		Rig.Geo = G;
		return true;
	}

	inline bool ConfigureLegacy(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMRuntimeConfig& C)
	{
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, C);
		if (!T.TestEqual(*FString::Printf(TEXT("legacy Configure() succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = C;
		return true;
	}

	// Configures budget mode and checks it took: GetKvPageStats().bPagedMode, then the engine's
	// geometry, then the ledger's regions against §3.2's sizes. This is where every budget-mode
	// cell first fails while budget mode is unbuilt.
	inline bool ConfigureBudget(FAutomationTestBase& T, FPagingRig& Rig, const FSuperSLMRuntimeConfig& C)
	{
		check(C.SequenceBudget > 0);
		const FSuperSLMConfigureReport Report = Rig.Cpu->Configure(Rig.Model, C);
		if (!T.TestEqual(*FString::Printf(TEXT("budget-mode Configure() succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
		Rig.Config = C;
		if (!T.TestTrue(TEXT("SequenceBudget > 0 configures budget mode (GetKvPageStats().bPagedMode)"), Rig.Cpu->GetKvPageStats().bPagedMode))
		{
			return false;
		}
		if (!ReadGeometry(T, Rig))
		{
			return false;
		}
		const FRegions Want = ExpectedRegions(Rig.Geo, C);
		const FSuperSLMPagedLedgerView V = FSuperSLMPagedTestSeam::Ledger(*Rig.Cpu);
		bool bOk = T.TestTrue(TEXT("the ledger is configured"), V.bConfigured);
		bOk &= T.TestEqual(TEXT("region S = BlockCount x R(SequenceBudget)"), V.CapacityOf(ESuperSLMPagedRegion::S), Want.S);
		bOk &= T.TestEqual(TEXT("region P = PrefixBlockCount x block"), V.CapacityOf(ESuperSLMPagedRegion::P), Want.P);
		bOk &= T.TestEqual(TEXT("region H = RestoreHeadroomBlocks x block"), V.CapacityOf(ESuperSLMPagedRegion::H), Want.H);
		bOk &= T.TestEqual(TEXT("region A is 0 (no cache in the paging release)"), V.CapacityOf(ESuperSLMPagedRegion::A), int64(0));
		bOk &= T.TestEqual(TEXT("region C is 0 (no cache in the paging release)"), V.CapacityOf(ESuperSLMPagedRegion::C), int64(0));
		bOk &= T.TestEqual(TEXT("the pool is S + P + H"), V.PoolPages, Want.Pool());
		bOk &= T.TestEqual(TEXT("S is fully charged at Configure (the warm reservations)"), V.ChargedOf(ESuperSLMPagedRegion::S), Want.S);
		return bOk;
	}

	// --- The fill probe (§3.9) -------------------------------------------------------------------

	// Creates budget holders on the subsystem's own pool whose reservations total exactly K pages
	// (each 2 or 3 pages: budget 1 and budget B + 1, sized by the engine's own
	// sslm_kv_pages_for_budget), then releases every one. True when all were admitted, false when
	// one was refused SSLM_KV_POOL_EXHAUSTED, unset (with an error) for anything else. K = 0 is
	// trivially admitted; K = 1 cannot be built and is reported.
	inline TOptional<bool> ProbeAdmits(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64 K)
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
		const bool bRan = FSuperSLMPagedTestSeam::RunOnPoolThread(Cpu, [K, &bAdmitted, &Error](FSuperSLMPoolThreadAccess& A)
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
				const int32 Budget = (Left % 2 == 1) ? int32(B + 1) : 1; // one 3-page holder makes an odd total
				const sslm_status Status = A.CreateBudgeted(Budget);
				if (Status == SSLM_KV_POOL_EXHAUSTED)
				{
					bAdmitted = false;
					break;
				}
				if (Status != SSLM_OK)
				{
					Error = FString::Printf(TEXT("fill probe: sslm_seq_create_budgeted returned status %d"), (int32)Status);
					break;
				}
				Left -= (Budget == 1) ? 2 : 3;
			}
			A.ReleaseAll();
		});
		if (!bRan)
		{
			T.AddError(TEXT("fill probe: FSuperSLMPagedTestSeam::RunOnPoolThread did not run"));
			return TOptional<bool>();
		}
		if (!Error.IsEmpty())
		{
			T.AddError(Error);
			return TOptional<bool>();
		}
		return TOptional<bool>(bAdmitted);
	}

	// The largest k in [2, Hi] the probe admits, by bisection on one state (each attempt releases
	// what it drew, so the state is the same for the next). -1 when even 2 is refused. A reading,
	// for messages and for the one-state cells; verdicts use the two forms below.
	inline bool ProbeFree(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64 Hi, int64& OutFree)
	{
		OutFree = -1;
		TOptional<bool> Two = ProbeAdmits(T, Cpu, 2);
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
			TOptional<bool> At = ProbeAdmits(T, Cpu, Mid);
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

	// Form 1 (§3.9): at this instant the ledger's total charge is at least the probe's drawn pages,
	// and every region's charge is within its capacity. Drawn <= charge exactly when creates totalling
	// (pool - charge) are admitted.
	inline bool Form1ChargesCoverDrawn(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		const FSuperSLMPagedLedgerView V = FSuperSLMPagedTestSeam::Ledger(Cpu);
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
		const TOptional<bool> Admits = ProbeAdmits(T, Cpu, MustBeFree);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: charges >= drawn (the probe admits pool %lld - charge %lld = %lld pages)"), Instant, V.PoolPages, V.TotalCharged, MustBeFree),
			Admits.IsSet() && Admits.GetValue());
		return bOk;
	}

	// The exact two-sided form (§3.9) at quiescence: the pool has exactly ExpectedDrawn pages drawn.
	// Rebuild replays the cell's construction after a re-Configure (true when it did).
	inline bool ProbeDrawnExactly(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64 Pool, int64 ExpectedDrawn, TFunctionRef<bool()> Rebuild, const TCHAR* Instant)
	{
		const int64 K = Pool - ExpectedDrawn;
		const TOptional<bool> AtK = ProbeAdmits(T, Cpu, K);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the probe admits creates totalling %lld (drawn <= %lld)"), Instant, K, ExpectedDrawn), AtK.IsSet() && AtK.GetValue());
		if (!bOk)
		{
			return false;
		}
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the state is rebuilt for the probe's second side"), Instant), Rebuild()))
		{
			return false;
		}
		const TOptional<bool> AtK1 = ProbeAdmits(T, Cpu, K + 1);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: on the rebuild the probe refuses creates totalling %lld (drawn >= %lld)"), Instant, K + 1, ExpectedDrawn), AtK1.IsSet() && !AtK1.GetValue());
		return bOk;
	}

	// --- Driving the subsystem -----------------------------------------------------------------

	inline int32 Drain(USuperSLMSubsystem& Cpu, TFunctionRef<bool()> Done, double Seconds)
	{
		return SuperSLML2S1Fixtures::DrainTicks(Cpu, Step, EL2S1DrainPacing::FastAsPossible, Done, Seconds);
	}

	// Ticks until every job the subsystem has posted has delivered.
	inline bool DrainQuiet(USuperSLMSubsystem& Cpu, double Seconds = OpSeconds)
	{
		auto AllDelivered = [&Cpu]()
		{
			for (const FSuperSLMWorkerJobReport& Job : Cpu.GetJobLedger())
			{
				if (Job.DeliveredAtTick < 0)
				{
					return false;
				}
			}
			return true;
		};
		Cpu.Tick(Step);
		Drain(Cpu, AllDelivered, Seconds);
		Cpu.Tick(Step);
		return AllDelivered();
	}

	inline ESuperSLMRestoreResult WaitOp(USuperSLMSubsystem& Cpu, const FSuperSLMLifecycleOpHandle& Handle, double Seconds = OpSeconds)
	{
		ESuperSLMRestoreResult Result = Cpu.GetLifecycleOpResult(Handle);
		Drain(Cpu, [&Cpu, &Handle, &Result]()
		{
			Result = Cpu.GetLifecycleOpResult(Handle);
			return Result != ESuperSLMRestoreResult::Pending;
		}, Seconds);
		return Result;
	}

	inline bool WaitHoldReached(USuperSLMSubsystem& Cpu, const FSuperSLMPagedHold& Hold, double Seconds = OpSeconds)
	{
		Drain(Cpu, [&Cpu, &Hold]() { return FSuperSLMPagedTestSeam::IsHoldReached(Cpu, Hold); }, Seconds);
		return FSuperSLMPagedTestSeam::IsHoldReached(Cpu, Hold);
	}

	inline bool Vend(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, FSuperSLMSequence& Out)
	{
		return T.TestEqual(TEXT("VendSequence()"), (uint8)Cpu.VendSequence(Out), (uint8)ESuperSLMVendResult::Success);
	}

	inline bool ReturnAndDrain(USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		Cpu.ReturnSequence(Seq);
		return DrainQuiet(Cpu);
	}

	inline bool WaitPrefixReady(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Prefix, double Seconds = BuildSeconds)
	{
		Drain(Cpu, [&Cpu, &Prefix]()
		{
			const ESuperSLMPrefixPhase Phase = Cpu.GetPrefixPhase(Prefix);
			return Phase == ESuperSLMPrefixPhase::Ready || Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid;
		}, Seconds);
		return T.TestEqual(TEXT("the prefix reaches Ready"), (uint8)Cpu.GetPrefixPhase(Prefix), (uint8)ESuperSLMPrefixPhase::Ready);
	}

	// A root prefix through the result-returning CreatePrefix (§3.6), built to Ready.
	inline bool BuildPrefix(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Tokens, FSuperSLMPrefix& Out, double Seconds = BuildSeconds)
	{
		if (!T.TestTrue(TEXT("prefix build has a finite positive watchdog budget"), FMath::IsFinite(Seconds) && Seconds > 0.0))
		{
			return false;
		}
		FString Error;
		const ESuperSLMPrefixResult Result = Cpu.CreatePrefix(Tokens, Out, &Error);
		if (!T.TestEqual(*FString::Printf(TEXT("CreatePrefix(%d tokens) is admitted (%s)"), Tokens.Num(), *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			return false;
		}
		return WaitPrefixReady(T, Cpu, Out, Seconds);
	}

	inline bool BuildNested(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Parent, const TArray<int32>& Tokens, FSuperSLMPrefix& Out)
	{
		FString Error;
		const ESuperSLMPrefixResult Result = Cpu.CreatePrefixFrom(Parent, Tokens, Out, &Error);
		if (!T.TestEqual(*FString::Printf(TEXT("CreatePrefixFrom(%d tokens) is admitted (%s)"), Tokens.Num(), *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			return false;
		}
		return WaitPrefixReady(T, Cpu, Out);
	}

	inline bool ReleasePrefixAndDrain(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Prefix)
	{
		FString Error;
		const bool bReleased = Cpu.ReleasePrefix(Prefix, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("ReleasePrefix() (%s)"), *Error), bReleased))
		{
			return false;
		}
		return DrainQuiet(Cpu);
	}

	inline bool AdoptAndWait(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const FSuperSLMPrefix& Prefix,
		ESuperSLMRestoreResult Expected = ESuperSLMRestoreResult::Success)
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		const ESuperSLMRestoreResult Queued = Cpu.AdoptPrefix(Seq, Prefix, Handle, Error);
		if (Queued != ESuperSLMRestoreResult::Success)
		{
			return T.TestEqual(*FString::Printf(TEXT("AdoptPrefix() (%s)"), *Error), (uint8)Queued, (uint8)Expected);
		}
		return T.TestEqual(TEXT("the adopt's result"), (uint8)WaitOp(Cpu, Handle), (uint8)Expected);
	}

	inline bool ResetAndWait(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		const ESuperSLMRestoreResult Queued = Cpu.ResetSequence(Seq, Handle, Error);
		if (!T.TestEqual(*FString::Printf(TEXT("ResetSequence() queues (%s)"), *Error), (uint8)Queued, (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		return T.TestEqual(TEXT("the reset succeeds"), (uint8)WaitOp(Cpu, Handle), (uint8)ESuperSLMRestoreResult::Success);
	}

	struct FGeneration
	{
		ESuperSLMSequencePhase Phase = ESuperSLMSequencePhase::Idle;
		ESuperSLMDecodeOutcome Outcome = ESuperSLMDecodeOutcome::Generating;
		TArray<int32> Tokens;
	};

	// Runs one generation to its end (Complete or Faulted, nothing pending) and reports how it ended.
	// A stop at the holder's limit is Faulted with KvBudgetExceeded (§3.3), so a Faulted end is not
	// an error here; the caller asserts the end it expects.
	inline bool Generate(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const TArray<int32>& Prompt, int32 MaxNew,
		FGeneration& Out, double Seconds = GenerateSeconds)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNew;
		FString Error;
		const bool bBegun = Cpu.BeginGeneration(Seq, Request, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("BeginGeneration() (%s)"), *Error), bBegun))
		{
			return false;
		}
		auto Ended = [&Cpu, &Seq]()
		{
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Cpu.GetPendingLifecycleOperationCount(Seq) == 0;
		};
		Drain(Cpu, Ended, Seconds);
		if (!T.TestTrue(TEXT("the generation ends"), Ended()))
		{
			return false;
		}
		Out.Phase = Cpu.GetPhase(Seq);
		Out.Outcome = Cpu.GetLastDecodeOutcome(Seq);
		Out.Tokens = Cpu.GetGeneratedTokens(Seq);
		return true;
	}

	inline bool GenerateComplete(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, const TArray<int32>& Prompt, int32 MaxNew,
		TArray<int32>& OutTokens)
	{
		FGeneration G;
		if (!Generate(T, Cpu, Seq, Prompt, MaxNew, G))
		{
			return false;
		}
		OutTokens = G.Tokens;
		return T.TestEqual(TEXT("the generation completes at MaxNewTokens"), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Complete)
			&& T.TestEqual(TEXT("MaxNewTokens tokens"), G.Tokens.Num(), MaxNew);
	}

	// Saves a sequence and takes its blob.
	inline bool SaveAndTake(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq, TArray<uint8>& OutBlob)
	{
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		const ESuperSLMRestoreResult Queued = Cpu.SaveSequence(Seq, Handle, Error);
		if (!T.TestEqual(*FString::Printf(TEXT("SaveSequence() queues (%s)"), *Error), (uint8)Queued, (uint8)ESuperSLMRestoreResult::Success))
		{
			return false;
		}
		ESuperSLMRestoreResult Result = ESuperSLMRestoreResult::Pending;
		Drain(Cpu, [&Cpu, &Handle, &Result, &OutBlob]()
		{
			Result = Cpu.GetSaveResult(Handle, OutBlob);
			return Result != ESuperSLMRestoreResult::Pending;
		}, OpSeconds);
		Cpu.ReleaseLifecycleOpHandle(Handle);
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the save has bytes"), OutBlob.Num() > 0);
	}

	// One issued restore: its synchronous result (Success means queued), the sequence it vended and
	// the handle its named result is read through.
	struct FIssuedRestore
	{
		ESuperSLMRestoreResult Sync = ESuperSLMRestoreResult::Pending;
		FSuperSLMSequence Sequence;
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
	};

	inline FIssuedRestore IssueRestore(USuperSLMSubsystem& Cpu, const TArray<uint8>& Blob, USuperSLMModel* Model, const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		FIssuedRestore Out;
		Out.Sync = Hint.IsValid()
			? Cpu.RestoreSequence(Blob, Model, Hint, Out.Sequence, Out.Handle, Out.Error)
			: Cpu.RestoreSequence(Blob, Model, Out.Sequence, Out.Handle, Out.Error);
		return Out;
	}

	// An issued restore's outcome so far: a refusal at the call, else its handle's result (Pending
	// while it is queued, waiting or in flight). Plan rev 1.9 fixes the carrier: a refusal the
	// payload header alone decides is made at the call with no handle; "refused on arrival" (§3.2:
	// admission at the head of the sequence's queue) and every engine refusal after admission are
	// carried on a valid handle. Cells that grade the carrier check R.Sync and R.Handle themselves.
	inline ESuperSLMRestoreResult OutcomeOf(const USuperSLMSubsystem& Cpu, const FIssuedRestore& R)
	{
		if (R.Sync != ESuperSLMRestoreResult::Success)
		{
			return R.Sync;
		}
		return Cpu.GetLifecycleOpResult(R.Handle);
	}

	inline ESuperSLMRestoreResult RestoreAndWait(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<uint8>& Blob, USuperSLMModel* Model,
		FSuperSLMSequence& OutSeq, const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		const FIssuedRestore R = IssueRestore(Cpu, Blob, Model, Hint);
		OutSeq = R.Sequence;
		if (R.Sync != ESuperSLMRestoreResult::Success)
		{
			T.AddInfo(FString::Printf(TEXT("restore refused at the call: %s"), *R.Error));
			return R.Sync;
		}
		return WaitOp(Cpu, R.Handle);
	}

	// --- Tokens --------------------------------------------------------------------------------

	// Count tokens of real text through the model's own tokenizer, repeated as needed. Salt rotates
	// the start, so two salts give different content of the same length.
	inline bool MakeTokens(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int32 Count, int32 Salt, TArray<int32>& Out)
	{
		static const TCHAR* const Text = TEXT(
			"The lighthouse keeper of Saltmere kept a ledger of every ship that passed: its flag, its cargo, "
			"the hour it cleared the reef and the name of the pilot who brought it in. She traded rope for "
			"lamp oil with the fishing families, mended nets in winter, and told travellers which roads "
			"flooded in spring. When asked about the old smugglers' cave she smiled and changed the subject, "
			"though she knew every tunnel beneath the cliffs and the tide that filled each one. ");
		TArray<int32> Base;
		if (!T.TestTrue(TEXT("the persona text tokenizes"), Cpu.Tokenize(Text, Base) && Base.Num() > 16))
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

	// A copy of Tokens with the token at Position replaced by a different one from the same text.
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

	// --- Blobs ---------------------------------------------------------------------------------

	// A budget-mode save of a holder that adopted Prefix (so the payload's origin is the prefix's
	// length and its budget is the configured SequenceBudget, §3.3), fed Suffix and MaxNew new
	// tokens. The sequence is returned before this returns.
	inline bool MakeAdoptedBlob(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefix& Prefix, const TArray<int32>& Suffix, int32 MaxNew,
		TArray<uint8>& OutBlob, TArray<int32>* OutTokens = nullptr)
	{
		FSuperSLMSequence Seq;
		if (!Vend(T, Cpu, Seq) || !AdoptAndWait(T, Cpu, Seq, Prefix))
		{
			return false;
		}
		TArray<int32> Tokens;
		const bool bOk = GenerateComplete(T, Cpu, Seq, Suffix, MaxNew, Tokens) && SaveAndTake(T, Cpu, Seq, OutBlob);
		if (OutTokens)
		{
			*OutTokens = Tokens;
		}
		ReturnAndDrain(Cpu, Seq);
		return bOk;
	}

	// A save of a plain holder (no prefix) fed Prompt and MaxNew new tokens. In legacy mode the
	// payload is 'SSB5'; in budget mode 'SSB6' with origin 0.
	inline bool MakePlainBlob(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Prompt, int32 MaxNew, TArray<uint8>& OutBlob,
		TArray<int32>* OutTokens = nullptr)
	{
		FSuperSLMSequence Seq;
		if (!Vend(T, Cpu, Seq))
		{
			return false;
		}
		TArray<int32> Tokens;
		const bool bOk = GenerateComplete(T, Cpu, Seq, Prompt, MaxNew, Tokens) && SaveAndTake(T, Cpu, Seq, OutBlob);
		if (OutTokens)
		{
			*OutTokens = Tokens;
		}
		ReturnAndDrain(Cpu, Seq);
		return bOk;
	}

	// A budget-mode blob at a chosen saved budget and origin, so its restore charge is
	// R(Budget) + E(Budget, Origin) (§3.5). Configures budget mode at (1 block, Budget, 1 prefix block,
	// 1 headroom block), builds a prefix of Origin tokens from Salt (none at origin 0), and saves an
	// adopter of it that took a short suffix and one new token, inside its own limit. The rig is left
	// at that shape: the caller re-Configures its own. OutPrefixTokens receives the prefix's tokens, so
	// a cell can rebuild the same prefix as a sharing hint.
	inline bool MakeBlobAt(FAutomationTestBase& T, FPagingRig& Rig, int32 Budget, int32 Origin, int32 Salt, TArray<uint8>& OutBlob,
		TArray<int32>* OutPrefixTokens = nullptr)
	{
		if (!ConfigureBudget(T, Rig, MakeConfig(1, Budget, Origin > 0 ? 1 : 0, 1)))
		{
			return false;
		}
		const int64 Limit = FMath::Min<int64>(int64(Origin) + Budget, Rig.Geo.Cap);
		const int32 Room = int32(Limit - Origin);
		TArray<int32> Suffix;
		if (!MakeTokens(T, *Rig.Cpu, FMath::Clamp(Room - 2, Origin > 0 ? 0 : 1, 4), Salt + 101, Suffix))
		{
			return false;
		}
		if (Origin == 0)
		{
			return MakePlainBlob(T, *Rig.Cpu, Suffix, 1, OutBlob);
		}
		TArray<int32> PrefixTokens;
		FSuperSLMPrefix Prefix;
		if (!MakeTokens(T, *Rig.Cpu, Origin, Salt, PrefixTokens) || !BuildPrefix(T, *Rig.Cpu, PrefixTokens, Prefix))
		{
			return false;
		}
		const bool bOk = MakeAdoptedBlob(T, *Rig.Cpu, Prefix, Suffix, 1, OutBlob);
		Rig.Cpu->ReleasePrefix(Prefix);
		DrainQuiet(*Rig.Cpu);
		if (OutPrefixTokens)
		{
			*OutPrefixTokens = PrefixTokens;
		}
		return bOk;
	}

	// A legacy-mode ('SSB5') blob of a plain holder. Leaves the rig in legacy mode.
	inline bool MakeSsb5Blob(FAutomationTestBase& T, FPagingRig& Rig, TArray<uint8>& OutBlob, TArray<int32>* OutTokens = nullptr)
	{
		TArray<int32> Prompt;
		return ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) && MakeTokens(T, *Rig.Cpu, 8, 7, Prompt)
			&& MakePlainBlob(T, *Rig.Cpu, Prompt, 2, OutBlob, OutTokens);
	}

	// A ghost (§3.2): a prefix built, adopted by one vended sequence, and released. The adopter stays
	// vended and keeps the ghost's whole charge on P until it resets, re-adopts or is returned.
	inline bool MakeGhost(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<int32>& Tokens, FSuperSLMPrefix& OutPrefix, FSuperSLMSequence& OutAdopter)
	{
		if (!BuildPrefix(T, Cpu, Tokens, OutPrefix) || !Vend(T, Cpu, OutAdopter) || !AdoptAndWait(T, Cpu, OutAdopter, OutPrefix)
			|| !ReleasePrefixAndDrain(T, Cpu, OutPrefix))
		{
			return false;
		}
		return T.TestTrue(TEXT("the released prefix is a ghost while its adopter holds it"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, OutPrefix));
	}

	// A restore driven to its apply, required to succeed.
	inline bool ApplyRestore(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TArray<uint8>& Blob, USuperSLMModel* Model, FSuperSLMSequence& OutSeq,
		const FSuperSLMPrefix& Hint = FSuperSLMPrefix())
	{
		const ESuperSLMRestoreResult Result = RestoreAndWait(T, Cpu, Blob, Model, OutSeq, Hint);
		return T.TestEqual(TEXT("the restore succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
	}

	// Captures every log line, of any category and verbosity, from construction to destruction, from
	// any thread. Counts lines containing a token (case-insensitive), so a cell can assert a line is
	// absent, which AddExpectedError cannot.
	struct FPagingLogCapture final : FOutputDevice
	{
		FPagingLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FPagingLogCapture() override { GLog->RemoveOutputDevice(this); }
		FPagingLogCapture(const FPagingLogCapture&) = delete;
		FPagingLogCapture& operator=(const FPagingLogCapture&) = delete;

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

	private:
		mutable FCriticalSection Guard;
		TArray<FString> Lines;
	};

	// No engine KV_POOL_EXHAUSTED reached this plan's own requests (§3.7): the ledger-fault counter, read
	// through both readings.
	inline bool NoLedgerFault(FAutomationTestBase& T, const USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: LedgerFaults is 0 (GetKvPageStats)"), Instant), int64(Cpu.GetKvPageStats().LedgerFaults), int64(0))
			&& T.TestEqual(*FString::Printf(TEXT("%s: LedgerFaults is 0 (Ledger seam)"), Instant), int64(FSuperSLMPagedTestSeam::Ledger(Cpu).LedgerFaults), int64(0));
	}

	// The constructions in the A-EX cells are computed for B = 64 and a block of 64 pages (plan §2).
	// Checked once the engine's geometry is read, so a different artifact fails by name, not by a
	// wrong count.
	inline bool RequireAExShape(FAutomationTestBase& T, const FPagingRig& Rig)
	{
		return T.TestEqual(TEXT("A-EX: B = 64 (the cell's construction is computed for it)"), Rig.Geo.B, int64(64))
			&& T.TestEqual(TEXT("A-EX: a block of 64 pages"), Rig.Geo.Block(), int64(64));
	}

	// Gives back a restore's vended sequence whatever its outcome (a refused restore has already
	// recycled it; ReturnSequence() on a handle that names nothing does nothing).
	inline void Discard(USuperSLMSubsystem& Cpu, const FIssuedRestore& R)
	{
		if (R.Sequence.IsValid())
		{
			Cpu.ReturnSequence(R.Sequence);
		}
		DrainQuiet(Cpu);
	}

	// Waits up to Seconds for an issued restore to resolve. A waiter behind held applies stays
	// Pending, so cells that may meet one pass a short bound.
	inline ESuperSLMRestoreResult SettleOutcome(USuperSLMSubsystem& Cpu, const FIssuedRestore& R, double Seconds = OpSeconds)
	{
		if (R.Sync != ESuperSLMRestoreResult::Success)
		{
			return R.Sync;
		}
		return WaitOp(Cpu, R.Handle, Seconds);
	}

	// Refused KvPagesExhausted on arrival (§3.2, §3.5): the call accepts the request and mints a
	// handle (the refusal depends on the ledger at the queue head, so it is never made at the call;
	// plan rev 1.9, ruling 1), and the handle resolves KvPagesExhausted within a short bound, with no
	// charge taken on H.
	inline bool ExpectRefusedOnArrival(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FIssuedRestore& R, int64 HChargedBefore, const TCHAR* What)
	{
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: the call accepts the request (%s)"), What, *R.Error), (uint8)R.Sync, (uint8)ESuperSLMRestoreResult::Success);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: a handle is minted"), What), R.Handle.IsValid());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the handle resolves KvPagesExhausted on arrival"), What),
			(uint8)WaitOp(Cpu, R.Handle, 10.0), (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no charge is taken on H"), What), FSuperSLMPagedTestSeam::Ledger(Cpu).ChargedOf(ESuperSLMPagedRegion::H), HChargedBefore);
		return bOk;
	}

	// --- The Layer-1 payload header, at the plan's §3.5 offsets (the oracle states them) --------

	namespace PayloadHeader
	{
		constexpr int32 Magic = 0;
		constexpr int32 SsbContextLength = 60; // LE64, SSB5 and SSB6
		constexpr int32 Ssb6KvMode = 156;      // LE32
		constexpr int32 Ssb6Budget = 160;      // i32
		constexpr int32 Ssb6Origin = 164;      // i64
		constexpr int32 Ssb6HeaderEnd = 172;   // the last field's end

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

	// A copy of a wrapped blob's Layer-1 payload (read through the payload-edit seam with an edit
	// that changes nothing).
	inline bool ReadPayload(FAutomationTestBase& T, const TArray<uint8>& Blob, TArray<uint8>& OutPayload)
	{
		TArray<uint8> Copy = Blob;
		const bool bOk = FSuperSLMPagedTestSeam::EditPayload(Copy, [&OutPayload](TArray<uint8>& Payload) { OutPayload = Payload; });
		return T.TestTrue(TEXT("FSuperSLMPagedTestSeam::EditPayload reads the payload"), bOk && OutPayload.Num() > 0);
	}

	// The budget payload's header fields, read by the test at §3.5's offsets.
	struct FSsb6Header
	{
		int64 ContextLength = -1;
		int64 KvMode = -1;
		int64 Budget = -1;
		int64 Origin = -1;
	};

	inline bool ReadSsb6Header(FAutomationTestBase& T, const TArray<uint8>& Blob, FSsb6Header& Out)
	{
		TArray<uint8> Payload;
		if (!ReadPayload(T, Blob, Payload))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("the payload is 'SSB6'"), PayloadHeader::MagicIs(Payload, "SSB6") && Payload.Num() >= PayloadHeader::Ssb6HeaderEnd))
		{
			return false;
		}
		Out.ContextLength = PayloadHeader::ReadLE(Payload, PayloadHeader::SsbContextLength, 8);
		Out.KvMode = PayloadHeader::ReadLE(Payload, PayloadHeader::Ssb6KvMode, 4);
		Out.Budget = PayloadHeader::ReadLE(Payload, PayloadHeader::Ssb6Budget, 4);
		Out.Origin = PayloadHeader::ReadLE(Payload, PayloadHeader::Ssb6Origin, 8);
		return true;
	}

	// --- Ledger readings -----------------------------------------------------------------------

	inline FSuperSLMPagedLedgerView LedgerOf(const USuperSLMSubsystem& Cpu)
	{
		return FSuperSLMPagedTestSeam::Ledger(Cpu);
	}

	inline int64 Charged(const USuperSLMSubsystem& Cpu, ESuperSLMPagedRegion Region)
	{
		return LedgerOf(Cpu).ChargedOf(Region);
	}

	inline int64 Free(const USuperSLMSubsystem& Cpu, ESuperSLMPagedRegion Region)
	{
		return LedgerOf(Cpu).FreeOf(Region);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
