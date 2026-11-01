// Precache plan rev 1.18 (PC-16 (a), §5.6), the U6 suite's GPU twins of the explicit residency control cells:
// USuperSLMGpuSubsystem::WarmPrefix, DropPrefix and GetPrefixOpResult on the GPU cache instance (pooled mode, region
// C on the GPU ledger). Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on; the disk-hit and write-back legs
// need SUPERSLM_WITH_GPU_PREFIX_SAVE as well (the GPU prefix save and restore).
//
//   SuperSLM.PrefixCache.Gpu.Residency.Churn            1.10 (scaled to 50 cycles)                        [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.MalformedOps     2.16                                              [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.DropDiskPart     3.10 base, the trim race, (i)-(iii), (v)-(vii)    [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.DropDiskPartWaiter  3.10 (iv) and its attach                       [U6]
//   SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.DiskTierOff     3.10 (iii); U2 findings 1, 2       [U6]
//   SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.OtherDirectory  3.10 (iii); U2 findings 1, 2       [U6]
//   SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.Deinitialize    3.10 (iii); U2 findings 1, 2       [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.IssueOrder       3.12 (one completion order per leg, see the cell) [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.MarkOrder        3.13 T1, T2, T4, T5, T6 and the second/third legs [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.Fallbacks        5.12                                              [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.DropLeased       7.18                                              [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.Warm             7.19                                              [U6]
//   SuperSLM.PrefixCache.Gpu.Residency.KeyRecreated     8.17                                              [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.DropPinnedParent    8.14 rev 1.18 "dropping a pinned parent"          [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.DropUnpinnedParent  8.14 rev 1.18.1 G13 (b)                           [U6]
//   SuperSLM.PrefixCache.Gpu.Nested.WarmPinsParent      8.14 rev 1.18.1 G13 (c)                           [U6]
//
// 7.23 (Blueprint parity) has no GPU twin: the GPU subsystem has no Blueprint nodes. 11.7 is out of scope (11.x).
//
// The oracles are the host cells (ci/host/prefix_cache/test_residency_control.cpp, test_residency_order.cpp,
// test_residency_disk.cpp, test_u2_review.cpp, test_box_regressions.cpp) and the CPU Unreal cells
// (SuperSLMPrefixCacheResidencyTests.cpp, SuperSLMPrefixCacheU2ReviewTests.cpp), translated to the GPU subsystem on
// the A-EX pooled rig (B = 16, one block of region C = 256 pages). The GPU cache is budget mode only, so every
// legacy-mode leg is dropped. Cells that need region C exactly full lease a pad that leaves the construction's C
// (as SuperSLMPrefixCacheGpuNestedTests.cpp does; a pad build takes minutes on the box), and every other cell runs
// with the whole block. Prefixes are 1-3 pages. "Physical" is that file's reading: form 1 at the GPU ledger, no
// ledger fault, C never above its capacity.
//
// The helpers live in a named namespace (not an anonymous one): a unity build puts this file in one translation
// unit with the other GPU cache files, whose anonymous helpers have the same shapes.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMPrefixCacheTestSeam.h"

// The runtime module's GPU cache host defaults this switch (SuperSLMGpuPrefixCacheHost.h, private); this file
// reads it too, so it carries the same default.
#ifndef SUPERSLM_WITH_GPU_PREFIX_SAVE
#define SUPERSLM_WITH_GPU_PREFIX_SAVE 0
#endif

namespace SuperSLMGpuResidencyCells
{
	using namespace SuperSLMPagingGpuFixtures;
	using SuperSLMPrefixCacheFixtures::FScopedCacheDirectory;
	using SuperSLMPrefixCacheFixtures::Spec;
	using Seam = FSuperSLMPrefixCacheTestSeam;
	using ERegion = ESuperSLMPagedRegion;
	using EOp = ESuperSLMPrefixOpResult;
	using EDisk = ESuperSLMPrefixDropDisk;
	using ELease = ESuperSLMPrefixLeaseState;
	using EScope = ESuperSLMPrefixDropScope;

	constexpr int32 kOpRing = 1024; // the core's resolved-op ring (kPrefixOpRing, SuperSLMPrefixCacheCore.h; plan §5.6)

	struct FRig
	{
		FGpuPagingRig Rig;
		FSuperSLMGpuPrefixCacheSettings Settings;
		FSuperSLMPrefix Pad;
		int64 PadPages = 0;
		int32 Salt = 300;
		USuperSLMGpuSubsystem& Gpu() { return *Rig.Gpu; }
		int64 B() const { return Rig.Geo.B; }
		int64 Page() const { return Rig.Geo.PageBytes; }
	};

	int64 CapC(const USuperSLMGpuSubsystem& Gpu) { return LedgerOf(Gpu).CapacityOf(ERegion::C); }

	FSuperSLMPrefixCacheStats Stats(const USuperSLMGpuSubsystem& Gpu) { return Gpu.GetPrefixCacheStats(); }

	// Count tokens of the fixture's text, its first token made unique to this rig's salt: MakeTokens' salts wrap at
	// the text's length, and these cells need every key distinct.
	bool Tokens(FAutomationTestBase& T, FRig& R, int32 Count, TArray<int32>& Out)
	{
		const int32 Salt = R.Salt++;
		if (!MakeTokens(T, *R.Rig.Model, Count, Salt, Out) || !T.TestTrue(TEXT("tokens made"), Out.Num() == Count))
		{
			return false;
		}
		Out[0] = 1000 + Salt;
		return true;
	}

	bool PageTokens(FAutomationTestBase& T, FRig& R, int64 Pages, TArray<int32>& Out)
	{
		return Tokens(T, R, int32(Pages * R.B()), Out);
	}

	// (Need - 1) * B tokens: a build is charged one page above its settle, so its charge is Need.
	bool NeedTokens(FAutomationTestBase& T, FRig& R, int64 Need, TArray<int32>& Out)
	{
		return Tokens(T, R, int32((Need - 1) * R.B()), Out);
	}

	// A key no cell requests: 16 synthetic ids (A-EX's vocabulary is 151,936; I < 2,500 here).
	FSuperSLMPrefixSpec Absent(int32 I)
	{
		FSuperSLMPrefixSpec S;
		S.Tokens.Reserve(16);
		S.Tokens.Add(60000 + I);
		for (int32 J = 1; J < 16; ++J)
		{
			S.Tokens.Add(500 + J);
		}
		return S;
	}

	FSuperSLMPrefixSpec ChildSpec(const FSuperSLMPrefix& Parent, const TArray<int32>& Suffix)
	{
		FSuperSLMPrefixSpec S = Spec(Suffix);
		S.Parent = Parent;
		return S;
	}

	// Ticks once per call, as the GPU nested cells' TickN does.
	void Ticks(USuperSLMGpuSubsystem& Gpu, int32 N = 4)
	{
		for (int32 I = 0; I < N; ++I)
		{
			Gpu.Tick(Step);
		}
	}

	// The cache's pool-thread work landed: at least three ticks, then IO and inbox settled on three consecutive
	// ticks (a held hash reads settled). Lane jobs are not counted, so this is safe under the build hold.
	bool SettleIo(USuperSLMGpuSubsystem& Gpu, double Seconds = OpSeconds)
	{
		int32 Seen = 0;
		int32 SettledTicks = 0;
		return Pump(Gpu, [&Gpu, &Seen, &SettledTicks]()
		{
			++Seen;
			SettledTicks = Seam::IsCacheIoSettled(Gpu) ? SettledTicks + 1 : 0;
			return Seen > 3 && SettledTicks > 3;
		}, Seconds);
	}

	// Everything landed: IO, then every lane job, then the IO a delivered job issued. Never under the build hold
	// (a held build step is re-posted, so the job count never reaches 0).
	bool Quiet(USuperSLMGpuSubsystem& Gpu)
	{
		const bool bIo = SettleIo(Gpu);
		const bool bJobs = DrainQuiet(Gpu);
		return SettleIo(Gpu) && bIo && bJobs;
	}

	ELease Resolve(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease)
	{
		Pump(Gpu, [&]() { return Gpu.GetPrefixLeaseState(Lease) != ELease::Pending; }, BuildSeconds);
		return Gpu.GetPrefixLeaseState(Lease);
	}

	bool Expect(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ELease Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s resolves"), What), (uint8)Resolve(Gpu, Lease), (uint8)Want);
	}

	bool ExpectSource(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Lease, ESuperSLMPrefixSource Want, const TCHAR* What)
	{
		return T.TestEqual(*FString::Printf(TEXT("%s: its source"), What), (uint8)Gpu.GetPrefixSource(Lease), (uint8)Want);
	}

	bool Request(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Gpu.RequestPrefix(S, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s requested (%s)"), What, *Error), bOk);
	}

	// Requested, Ready, optionally returned, then everything landed.
	bool Lease(FAutomationTestBase& T, FRig& R, const FSuperSLMPrefixSpec& S, bool bKeep, FSuperSLMPrefix& Out, const TCHAR* What)
	{
		if (!Request(T, R.Gpu(), S, Out, What) || !Expect(T, R.Gpu(), Out, ELease::Ready, What))
		{
			return false;
		}
		if (!bKeep)
		{
			R.Gpu().ReturnPrefix(Out);
		}
		return Quiet(R.Gpu());
	}

	bool Root(FAutomationTestBase& T, FRig& R, int64 Pages, bool bKeep, FSuperSLMPrefix& Out, TArray<int32>& OutTokens, const TCHAR* What)
	{
		return PageTokens(T, R, Pages, OutTokens) && Lease(T, R, Spec(OutTokens), bKeep, Out, What);
	}

	bool Child(FAutomationTestBase& T, FRig& R, const FSuperSLMPrefix& Parent, int64 Pages, bool bKeep, FSuperSLMPrefix& Out, TArray<int32>& OutSuffix, const TCHAR* What)
	{
		return PageTokens(T, R, Pages, OutSuffix) && Lease(T, R, ChildSpec(Parent, OutSuffix), bKeep, Out, What);
	}

	// --- Residency ops ----------------------------------------------------------------------------

	FSuperSLMPrefixOpResult OpOf(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixOp& Op, bool* OutFound = nullptr)
	{
		FSuperSLMPrefixOpResult R;
		const bool bFound = Gpu.GetPrefixOpResult(Op, R);
		if (OutFound)
		{
			*OutFound = bFound;
		}
		return R;
	}

	// Ticks until the op leaves Pending (or reads unknown); its full result.
	FSuperSLMPrefixOpResult WaitPrefixOp(USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixOp& Op, double Seconds = BuildSeconds)
	{
		Pump(Gpu, [&Gpu, &Op]()
		{
			bool bFound = false;
			const FSuperSLMPrefixOpResult R = OpOf(Gpu, Op, &bFound);
			return !bFound || R.Result != EOp::Pending;
		}, Seconds);
		return OpOf(Gpu, Op);
	}

	bool ExpectOp(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixOp& Op, EOp Want, const TCHAR* What, FSuperSLMPrefixOpResult* OutFull = nullptr)
	{
		const FSuperSLMPrefixOpResult R = WaitPrefixOp(Gpu, Op);
		if (OutFull)
		{
			*OutFull = R;
		}
		return T.TestEqual(*FString::Printf(TEXT("%s: the op resolves"), What), (uint8)R.Result, (uint8)Want);
	}

	bool Warm(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefixOp& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Gpu.WarmPrefix(S, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: WarmPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: an op id is issued"), What), Out.Id != 0);
	}

	bool Drop(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, EScope Scope, FSuperSLMPrefixOp& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Gpu.DropPrefix(S, Scope, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: an op id is issued"), What), Out.Id != 0);
	}

	bool WaitHeldBuild(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* What)
	{
		Pump(Gpu, [&Gpu]() { return Seam::HeldBuildCount(Gpu) >= 1; }, OpSeconds);
		return T.TestTrue(*FString::Printf(TEXT("%s: stopped at the build hold"), What), Seam::HeldBuildCount(Gpu) >= 1);
	}

	bool WaitMarks(USuperSLMGpuSubsystem& Gpu, int64 Want, double Seconds = OpSeconds)
	{
		return Pump(Gpu, [&Gpu, Want]() { return Gpu.GetPrefixCacheStats().MarksApplied >= Want; }, Seconds);
	}

	// --- Disk ---------------------------------------------------------------------------------------

	bool HasFile(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S)
	{
		const FString File = Seam::DiskFileOf(Gpu, S);
		return !File.IsEmpty() && FPaths::FileExists(File);
	}

	bool WaitFile(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, bool bWant, const TCHAR* What)
	{
		Pump(Gpu, [&Gpu, &S, bWant]() { return HasFile(Gpu, S) == bWant; }, OpSeconds);
		const bool bQuiet = Quiet(Gpu);
		return T.TestEqual(*FString::Printf(TEXT("%s: a file under the key (%s)"), What, *Seam::DiskFileOf(Gpu, S)), HasFile(Gpu, S), bWant) && bQuiet;
	}

	// --- Readings -----------------------------------------------------------------------------------

	bool Physical(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TCHAR* Instant)
	{
		bool bOk = Form1ChargesCoverDrawn(T, Gpu, Instant);
		bOk &= NoLedgerFault(T, Gpu, Instant);
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: C charged %lld <= capacity %lld"), Instant, V.ChargedOf(ERegion::C), V.CapacityOf(ERegion::C)),
			V.ChargedOf(ERegion::C) <= V.CapacityOf(ERegion::C));
		return bOk;
	}

	// The cache's charge above the pad, in pages.
	int64 ChargedPages(FRig& R)
	{
		return Stats(R.Gpu()).ChargedBytes / R.Page() - R.PadPages;
	}

	// 5.4's counter form (suite audit G4): Reason's counter raised by exactly one, every other unchanged.
	bool RowForm(FAutomationTestBase& T, const TMap<FName, int64>& Before, const TMap<FName, int64>& After, const TCHAR* Reason, const TCHAR* What)
	{
		const FName Name(Reason);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: %s's counter raised by exactly one"), What, Reason), After.FindRef(Name) - Before.FindRef(Name), int64(1));
		for (const TPair<FName, int64>& P : After)
		{
			if (P.Key != Name)
			{
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: %s's counter unchanged"), What, *P.Key.ToString()), P.Value - Before.FindRef(P.Key), int64(0));
			}
		}
		return bOk;
	}

	bool SameFallbacks(FAutomationTestBase& T, const TMap<FName, int64>& Before, const TMap<FName, int64>& After, const TCHAR* What)
	{
		bool bOk = true;
		for (const TPair<FName, int64>& P : After)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: %s's counter unchanged"), What, *P.Key.ToString()), P.Value - Before.FindRef(P.Key), int64(0));
		}
		return bOk;
	}

	// --- Configuration ------------------------------------------------------------------------------

	bool Reconfigure(FAutomationTestBase& T, FRig& R)
	{
		R.Rig.Gpu->ConfigurePrefixCache(R.Settings);
		R.Pad = FSuperSLMPrefix();
		R.PadPages = 0;
		if (!ConfigurePooled(T, R.Rig, MakeGpuConfig(R.Rig.Artifact, 2, 512)))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S = Stats(R.Gpu());
		return T.TestTrue(TEXT("the GPU cache is configured, on region C"), S.bConfigured && S.bBudgetMode);
	}

	// The pooled rig with a cache of one block of region C. CPages > 0 leases a pad leaving exactly CPages of C (a
	// long build); CPages <= 0 keeps the whole block (256 pages). DiskDirectory non-empty turns the disk tier on.
	bool Configure(FAutomationTestBase& T, FRig& R, int64 CPages, const FString& DiskDirectory = FString())
	{
		FGpuPagingRig& Rig = R.Rig;
		if (!Rig.Init(T) || !ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, 512)) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		R.Settings = FSuperSLMGpuPrefixCacheSettings();
		R.Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C
		R.Settings.bDiskEnabled = !DiskDirectory.IsEmpty();
		R.Settings.DiskDirectory = DiskDirectory;
		R.Settings.DiskMaxBytes = R.Settings.bDiskEnabled ? (int64(1) << 30) : 0;
		if (!Reconfigure(T, R))
		{
			return false;
		}
		if (CPages <= 0)
		{
			return true;
		}
		const int64 PadPages = CapC(R.Gpu()) - CPages;
		if (PadPages <= 0)
		{
			return T.TestEqual(TEXT("the construction's C is the region's"), PadPages, int64(0));
		}
		TArray<int32> PadTokens;
		FString Error;
		if (!PageTokens(T, R, PadPages, PadTokens) || !R.Gpu().RequestPrefix(Spec(PadTokens), R.Pad, Error)
			|| !T.TestEqual(TEXT("the pad resolves"), (uint8)Resolve(R.Gpu(), R.Pad), (uint8)ELease::Ready))
		{
			return false;
		}
		R.PadPages = PadPages;
		return Quiet(R.Gpu());
	}

	// 7.19 / 7.18's waiting scene, as box_1021_ledger_unclaimed_refunds_with_one_free_page (ii) builds it at A-EX:
	// C = 6; E (3, a build of 48) resident and unpinned, L (2) leased, one page free; releases gated; Y (charged 5)
	// evicts E and is refused, E's 3-page refund pending and unclaimed. Returns E's engine handle (0 on failure).
	uint64 UnclaimedScene(FAutomationTestBase& T, FRig& R, FSuperSLMPrefix& OutL, const TCHAR* What)
	{
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		FSuperSLMPrefix E, Y;
		TArray<int32> ET, LT, YT;
		if (!Root(T, R, 3, true, E, ET, TEXT("E")))
		{
			return 0;
		}
		const uint64 EHandle = Seam::EngineHandleOf(Gpu, E);
		Gpu.ReturnPrefix(E);
		if (!Quiet(Gpu) || !Root(T, R, 2, true, OutL, LT, TEXT("L")) || !NeedTokens(T, R, 5, YT))
		{
			return 0;
		}
		const FSuperSLMPrefixCacheStats S0 = Stats(Gpu);
		Seam::HoldReleases(Gpu, true);
		if (!Request(T, Gpu, Spec(YT), Y, TEXT("Y")) || !Expect(T, Gpu, Y, ELease::BudgetExhausted, TEXT("Y (charged 5, one free page and E's 3)")))
		{
			return 0;
		}
		SettleIo(Gpu);
		const FSuperSLMPrefixCacheStats S1 = Stats(Gpu);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: Y evicted E"), What), S1.Evictions - S0.Evictions, int64(1));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E's 3-page refund pending and unclaimed"), What), S1.UnclaimedRefundBytes, 3 * R.Page());
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E's release held"), What), Seam::HeldReleaseCount(Gpu), 1);
		return bOk ? EHandle : 0;
	}

	// --- 3.13's runs --------------------------------------------------------------------------------

	enum class ETrigger : uint8
	{
		T1LeaseReturned,
		T2AdoptDrained,
		T4ChildSettled,
		T5WarmBuilt,
		T6BuiltAfterReturn,
	};

	const TCHAR* TriggerName(ETrigger Trigger)
	{
		switch (Trigger)
		{
			case ETrigger::T1LeaseReturned: return TEXT("T1");
			case ETrigger::T2AdoptDrained: return TEXT("T2");
			case ETrigger::T4ChildSettled: return TEXT("T4");
			case ETrigger::T5WarmBuilt: return TEXT("T5");
			case ETrigger::T6BuiltAfterReturn: return TEXT("T6");
		}
		return TEXT("?");
	}

	struct FMarkRun
	{
		ETrigger Trigger = ETrigger::T1LeaseReturned;
		TArray<int32> ET;
		FSuperSLMPrefixSpec E;
		FSuperSLMPrefix Lease;       // T1, T2, T4: E's lease; T6: the lease returned while Pending
		FSuperSLMPrefix ChildLease;  // T4
		FSuperSLMPrefixOp WarmOp;    // T5
		FSuperSLMGpuSequence Seq;    // T2
		FSuperSLMPagedHold AdoptHold;
		FSuperSLMLifecycleOpHandle Adopt;
		FSuperSLMPrefixOp DropOp;
		int64 Marks0 = 0;
		int64 Cleared0 = 0;
	};

	// One 3.13 run up to the frame of the pass at which E's last pin goes: E marked (DropDeferred), every pin but
	// the trigger's gone.
	bool MarkSetup(FAutomationTestBase& T, FRig& R, FMarkRun& M)
	{
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		const TCHAR* Name = TriggerName(M.Trigger);
		if (!PageTokens(T, R, 1, M.ET))
		{
			return false;
		}
		M.E = Spec(M.ET);
		switch (M.Trigger)
		{
			case ETrigger::T1LeaseReturned:
			case ETrigger::T2AdoptDrained:
			case ETrigger::T4ChildSettled:
				if (!Lease(T, R, M.E, true, M.Lease, Name))
				{
					return false;
				}
				break;
			case ETrigger::T5WarmBuilt:
				Seam::SetBuildHold(Gpu, true);
				if (!Warm(T, Gpu, M.E, M.WarmOp, Name) || !WaitHeldBuild(T, Gpu, Name))
				{
					return false;
				}
				break;
			case ETrigger::T6BuiltAfterReturn:
				Seam::SetBuildHold(Gpu, true);
				if (!Request(T, Gpu, M.E, M.Lease, Name) || !WaitHeldBuild(T, Gpu, Name))
				{
					return false;
				}
				break;
		}
		if (M.Trigger == ETrigger::T2AdoptDrained)
		{
			if (!Vend(T, Gpu, M.Seq))
			{
				return false;
			}
			M.AdoptHold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Adopt);
			M.Adopt = Gpu.RequestAdoptPrefix(M.Seq, M.Lease);
			if (!T.TestTrue(TEXT("T2: E's adopt queues"), M.Adopt.IsValid()) || !T.TestTrue(TEXT("T2: the adopt's apply is held"), WaitHoldReached(Gpu, M.AdoptHold)))
			{
				return false;
			}
		}
		if (M.Trigger == ETrigger::T4ChildSettled)
		{
			TArray<int32> Suffix;
			Seam::SetBuildHold(Gpu, true);
			if (!PageTokens(T, R, 1, Suffix) || !Request(T, Gpu, ChildSpec(M.Lease, Suffix), M.ChildLease, TEXT("T4: E's child")) || !WaitHeldBuild(T, Gpu, TEXT("T4: the child's build")))
			{
				return false;
			}
		}
		M.Marks0 = Stats(Gpu).MarksApplied;
		M.Cleared0 = Stats(Gpu).MarksCleared;
		if (!Drop(T, Gpu, M.E, EScope::Memory, M.DropOp, Name) || !ExpectOp(T, Gpu, M.DropOp, EOp::DropDeferred, *FString::Printf(TEXT("%s: DropPrefix(E)"), Name)))
		{
			return false;
		}
		// The pins that go before the trigger's pass, each followed by passes of its own.
		if (M.Trigger == ETrigger::T2AdoptDrained || M.Trigger == ETrigger::T4ChildSettled || M.Trigger == ETrigger::T6BuiltAfterReturn)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("%s: E's lease returns"), Name), Gpu.ReturnPrefix(M.Lease)))
			{
				return false;
			}
			Ticks(Gpu);
		}
		return T.TestEqual(*FString::Printf(TEXT("%s: the mark not applied before the trigger's pass"), Name), Stats(Gpu).MarksApplied - M.Marks0, int64(0));
	}

	// The frame's game-thread calls before its pass.
	void MarkBefore(USuperSLMGpuSubsystem& Gpu, FMarkRun& M)
	{
		if (M.Trigger == ETrigger::T1LeaseReturned)
		{
			Gpu.ReturnPrefix(M.Lease);
		}
	}

	// The pass at which E's last pin goes (or E becomes resident with no pin).
	bool MarkPass(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FMarkRun& M)
	{
		bool bOk = true;
		switch (M.Trigger)
		{
			case ETrigger::T1LeaseReturned:
				break;
			case ETrigger::T2AdoptDrained:
			{
				FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, M.AdoptHold);
				const ESuperSLMRestoreResult Result = SuperSLMPagingGpuFixtures::WaitOp(Gpu, M.Adopt);
				Gpu.ReleaseLifecycleOpHandle(M.Adopt);
				bOk &= T.TestEqual(TEXT("T2: the adopt drains (Success)"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success);
				break;
			}
			case ETrigger::T4ChildSettled:
				Seam::SetBuildHold(Gpu, false);
				bOk &= Expect(T, Gpu, M.ChildLease, ELease::Ready, TEXT("T4: the child job settles"));
				break;
			case ETrigger::T5WarmBuilt:
				Seam::SetBuildHold(Gpu, false);
				bOk &= T.TestEqual(TEXT("T5: the warm's build completes"), (uint8)WaitPrefixOp(Gpu, M.WarmOp).Result, (uint8)EOp::Resident);
				break;
			case ETrigger::T6BuiltAfterReturn:
				Seam::SetBuildHold(Gpu, false);
				Pump(Gpu, [&Gpu]() { return Seam::InFlightBuildCount(Gpu) == 0; }, BuildSeconds);
				bOk &= T.TestEqual(TEXT("T6: the build completes"), Seam::InFlightBuildCount(Gpu), 0);
				break;
		}
		Ticks(Gpu);
		return bOk;
	}

	// After the run: every lease returned, the adopter returned, everything landed.
	bool MarkCleanup(USuperSLMGpuSubsystem& Gpu, FMarkRun& M)
	{
		Seam::SetBuildHold(Gpu, false);
		if (M.ChildLease.IsValid())
		{
			Gpu.ReturnPrefix(M.ChildLease);
		}
		bool bOk = true;
		if (M.Seq.IsValid())
		{
			bOk &= ReturnAndDrain(Gpu, M.Seq);
		}
		return Quiet(Gpu) && bOk;
	}

	// --- 3.12's runs --------------------------------------------------------------------------------

	enum class EOrderLeg : uint8
	{
		DropThenRequest,
		DropThenRequestLeased,
		RequestThenDrop,
		DropThenWarm,
		DropThenWarmLeased,
		WarmThenDrop,
	};

	const TCHAR* OrderLegName(EOrderLeg Leg)
	{
		switch (Leg)
		{
			case EOrderLeg::DropThenRequest: return TEXT("drop then request, E unleased");
			case EOrderLeg::DropThenRequestLeased: return TEXT("drop then request, E leased");
			case EOrderLeg::RequestThenDrop: return TEXT("request then drop");
			case EOrderLeg::DropThenWarm: return TEXT("drop then warm, E unleased");
			case EOrderLeg::DropThenWarmLeased: return TEXT("drop then warm, E leased");
			case EOrderLeg::WarmThenDrop: return TEXT("warm then drop");
		}
		return TEXT("?");
	}

	bool OrderRun(FAutomationTestBase& T, FRig& R, EOrderLeg Leg)
	{
		USuperSLMGpuSubsystem& Gpu = R.Gpu();
		const TCHAR* Name = OrderLegName(Leg);
		const bool bLeased = Leg == EOrderLeg::DropThenRequestLeased || Leg == EOrderLeg::DropThenWarmLeased;
		const bool bWarm = Leg == EOrderLeg::DropThenWarm || Leg == EOrderLeg::DropThenWarmLeased || Leg == EOrderLeg::WarmThenDrop;
		const bool bDropFirst = Leg != EOrderLeg::RequestThenDrop && Leg != EOrderLeg::WarmThenDrop;
		TArray<int32> ET;
		FSuperSLMPrefix L0, H;
		if (!PageTokens(T, R, 1, ET) || !Lease(T, R, Spec(ET), bLeased, L0, Name))
		{
			return false;
		}
		const FSuperSLMPrefixSpec E = Spec(ET);
		const FSuperSLMPrefixCacheStats S0 = Stats(Gpu);
		FSuperSLMPrefixOp D, W;
		auto IssueRequest = [&]() { return bWarm ? Warm(T, Gpu, E, W, Name) : Request(T, Gpu, E, H, Name); };
		auto IssueDrop = [&]() { return Drop(T, Gpu, E, EScope::Memory, D, Name); };
		// The first-issued op's hash is parked before the second is issued, so the completions are released in the
		// issue order (the hold is per key, and both ops are E's).
		Seam::HoldKeyHash(Gpu, E, true);
		bool bOk = bDropFirst ? IssueDrop() : IssueRequest();
		SettleIo(Gpu);
		bOk &= bDropFirst ? IssueRequest() : IssueDrop();
		SettleIo(Gpu);
		Seam::HoldKeyHash(Gpu, E, false);
		if (!bOk)
		{
			return false;
		}
		const FSuperSLMPrefixOpResult DR = WaitPrefixOp(Gpu, D);
		if (bWarm)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the warm resolves Resident"), Name), (uint8)WaitPrefixOp(Gpu, W).Result, (uint8)EOp::Resident);
		}
		else
		{
			bOk &= Expect(T, Gpu, H, ELease::Ready, Name);
		}
		const auto Builds = [&]() { return Stats(Gpu).BuildsIssued - S0.BuildsIssued; };
		switch (Leg)
		{
			case EOrderLeg::DropThenRequest:
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop is decided first (Dropped)"), Name), (uint8)DR.Result, (uint8)EOp::Dropped);
				bOk &= ExpectSource(T, Gpu, H, ESuperSLMPrefixSource::Recomputed, *FString::Printf(TEXT("%s: the request is a miss"), Name));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: it rebuilds"), Name), Builds(), int64(1));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: one drop"), Name), Stats(Gpu).Drops - S0.Drops, int64(1));
				Gpu.ReturnPrefix(H);
				break;
			case EOrderLeg::DropThenRequestLeased:
			case EOrderLeg::DropThenWarmLeased:
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop marks (DropDeferred)"), Name), (uint8)DR.Result, (uint8)EOp::DropDeferred);
				if (!bWarm)
				{
					bOk &= ExpectSource(T, Gpu, H, ESuperSLMPrefixSource::Memory, *FString::Printf(TEXT("%s: the request"), Name));
				}
				Gpu.ReturnPrefix(L0);
				Ticks(Gpu);
				if (!bWarm)
				{
					Gpu.ReturnPrefix(H);
				}
				bOk &= Quiet(Gpu);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E stays resident after every return"), Name), Stats(Gpu).ResidentEntries, S0.ResidentEntries);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the later op cleared the mark"), Name), Stats(Gpu).MarksCleared - S0.MarksCleared, int64(1));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no mark applied"), Name), Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing released"), Name), Stats(Gpu).ReleasesIssued - S0.ReleasesIssued, int64(0));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no build"), Name), Builds(), int64(0));
				break;
			case EOrderLeg::RequestThenDrop:
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop finds the request's lease (DropDeferred)"), Name), (uint8)DR.Result, (uint8)EOp::DropDeferred);
				bOk &= ExpectSource(T, Gpu, H, ESuperSLMPrefixSource::Memory, *FString::Printf(TEXT("%s: the request"), Name));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E resident while leased"), Name), Stats(Gpu).ResidentEntries, S0.ResidentEntries);
				Gpu.ReturnPrefix(H);
				bOk &= Quiet(Gpu);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E evicted at the lease's return (mark applied)"), Name), Stats(Gpu).MarksApplied - S0.MarksApplied, int64(1));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E gone"), Name), Stats(Gpu).ResidentEntries, S0.ResidentEntries - 1);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E released once"), Name), Stats(Gpu).ReleasesIssued - S0.ReleasesIssued, int64(1));
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no build"), Name), Builds(), int64(0));
				break;
			case EOrderLeg::DropThenWarm:
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop is decided first (Dropped)"), Name), (uint8)DR.Result, (uint8)EOp::Dropped);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the warm rebuilds"), Name), Builds(), int64(1));
				bOk &= Quiet(Gpu);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E resident again"), Name), Stats(Gpu).ResidentEntries, S0.ResidentEntries);
				break;
			case EOrderLeg::WarmThenDrop:
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the warm (a hit, no pin) first, then the drop evicts E (Dropped)"), Name), (uint8)DR.Result, (uint8)EOp::Dropped);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the warm was a hit"), Name), Builds(), int64(0));
				bOk &= Quiet(Gpu);
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: E gone"), Name), Stats(Gpu).ResidentEntries, S0.ResidentEntries - 1);
				break;
		}
		return Quiet(Gpu) && bOk;
	}

	// --- 3.10 (iii) on the host's end: the U2 review's delete-behind-save --------------------------------

	enum class EHostEnd : uint8
	{
		DiskTierOff,    // re-Configure() with the disk tier off
		OtherDirectory, // re-Configure() with another disk directory
		Deinitialize,   // the world torn down: no next Configure()
	};

	// The file at Path read back, its open retried (ticking) for up to Seconds: the write-back's rename is followed on
	// the pool thread by the trim, and an open can race it (the CPU cell's ReadBack, box 2026-10-21).
	bool ReadFileBack(USuperSLMGpuSubsystem& Gpu, const FString& Path, TArray<uint8>& Out, double Seconds = 10.0)
	{
		const double Start = FPlatformTime::Seconds();
		do
		{
			if (FFileHelper::LoadFileToArray(Out, *Path, FILEREAD_Silent) && Out.Num() > 0)
			{
				return true;
			}
			Ticks(Gpu, 1);
			FPlatformProcess::Sleep(0.01f);
		} while (FPlatformTime::Seconds() - Start < Seconds);
		return false;
	}

	// The GPU twin of Cpu.U2Review.DeleteBehindSave (U2 code review findings 1 and 2; plan 3.10 (iii), rev 1.18.5): a
	// MemoryAndDisk drop whose delete waits on a write-back's save that has not run, a file already under the key, then
	// the host's end. The CPU reaches the unrun save with the build hold and a paging start hold on a second build; the
	// GPU has no worker lanes to park, so the save hold keeps the save queued instead (each held step is posted again,
	// and the teardown delivers a held save Skipped, as the CPU host's step 1a does). The CPU recomputes S over its
	// existing file through a read fault; here S's first file is dropped with it, S is recomputed with no file, and the
	// first file's bytes are put back under the key before the drop, so the state at the drop is the CPU cell's.
	// The delete is drained under the old configuration before the end returns: the old directory's file is gone, the op
	// reads DropDeferred with Disk == Deleted (the retired cache answers it), and the new directory gains nothing.
	// Mutant (the CPU cell's red at 79163cd69): the delete carried into the next Configure() -> DiskTierOff or NoFile with
	// the file still there, or at Deinitialize() never run -> the file left -> red.
	bool DeleteBehindSave(FAutomationTestBase& T, EHostEnd End, const TCHAR* Cell)
	{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
		T.AddError(TEXT("needs the GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE): the disk tier's write-backs"));
		(void)End;
		(void)Cell;
		return false;
#else
		FScopedCacheDirectory Dir(Cell);
		FScopedCacheDirectory Other(*FString::Printf(TEXT("%s-other"), Cell));
		FRig Rg;
		if (!Configure(T, Rg, 0, Dir.Path))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
		TArray<int32> ST;
		FSuperSLMPrefix First;
		if (!Root(T, Rg, 1, false, First, ST, TEXT("S, the first build")))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = Spec(ST);
		const FString Path = Seam::DiskFileOf(Gpu, S);
		TArray<uint8> Bytes;
		if (!WaitFile(T, Gpu, S, true, TEXT("S's first write-back"))
			|| !T.TestTrue(*FString::Printf(TEXT("S's file reads back (%s)"), *Path), ReadFileBack(Gpu, Path, Bytes)))
		{
			return false;
		}
		// S out of memory and off disk, so its next request recomputes.
		FSuperSLMPrefixOp Out;
		if (!Drop(T, Gpu, S, EScope::MemoryAndDisk, Out, TEXT("S out")) || !ExpectOp(T, Gpu, Out, EOp::Dropped, TEXT("S out"))
			|| !Quiet(Gpu) || !T.TestFalse(TEXT("S's first file is gone"), HasFile(Gpu, S)))
		{
			return false;
		}

		// (1) S recomputed, Ready, its write-back's save queued and held: no lane-side save has run.
		Seam::SetSaveHold(Gpu, true);
		FSuperSLMPrefix LeaseS;
		if (!Request(T, Gpu, S, LeaseS, TEXT("S, recomputed")) || !Expect(T, Gpu, LeaseS, ELease::Ready, TEXT("S, recomputed")))
		{
			Seam::SetSaveHold(Gpu, false);
			return false;
		}
		Pump(Gpu, [&Gpu]() { return Seam::HeldSaveCount(Gpu) == 1; }, OpSeconds);
		bool bOk = T.TestEqual(TEXT("S's save is queued and held (the save hold)"), Seam::HeldSaveCount(Gpu), 1);
		bOk &= ExpectSource(T, Gpu, LeaseS, ESuperSLMPrefixSource::Recomputed, TEXT("S"));
		// A file under S's key (the review's second PIE instance).
		bOk &= T.TestTrue(TEXT("a file is under S's key"), FFileHelper::SaveArrayToFile(Bytes, *Path));
		const int32 FilesInOther = Other.Files().Num();

		// (2) The drop: S is leased, so DropDeferred, and its delete waits on the held save.
		FSuperSLMPrefixOp Op;
		const int64 DeferredBefore = Gpu.GetPrefixCacheStats().DropsDeferred;
		bOk &= Drop(T, Gpu, S, EScope::MemoryAndDisk, Op, TEXT("delete behind the save"));
		// The memory part is decided at the drop's place in the issue order, after its key is hashed on a pool thread;
		// the CPU's ticks give that real time, a GPU tick does not, so the decision is awaited while S is still leased.
		Pump(Gpu, [&Gpu, DeferredBefore]() { return Gpu.GetPrefixCacheStats().DropsDeferred > DeferredBefore; }, OpSeconds);
		bOk &= T.TestEqual(TEXT("the drop is decided DropDeferred while S is leased"), Gpu.GetPrefixCacheStats().DropsDeferred, DeferredBefore + 1);
		Gpu.ReturnPrefix(LeaseS);
		Ticks(Gpu, 8);
		bOk &= T.TestEqual(TEXT("the drop is Pending: its delete waits on the save that has not run"), (uint8)OpOf(Gpu, Op).Result, (uint8)EOp::Pending);
		bOk &= T.TestTrue(TEXT("the file is still there before the host's end"), FPaths::FileExists(Path));
		if (!bOk)
		{
			Seam::SetSaveHold(Gpu, false);
			return false;
		}

		// (3) The host's end, with the save still held.
		if (End == EHostEnd::Deinitialize)
		{
			Rg.Rig.World.DestroyTestWorld(false); // Deinitialize(): the teardown drains the delete, then abandons the core
			return T.TestFalse(*FString::Printf(TEXT("Deinitialize(): the delete was drained, S's file is gone (%s)"), *Path), FPaths::FileExists(Path));
		}
		if (End == EHostEnd::DiskTierOff)
		{
			Rg.Settings.bDiskEnabled = false;
			Rg.Settings.DiskDirectory = FString();
			Rg.Settings.DiskMaxBytes = 0;
		}
		else
		{
			Rg.Settings.DiskDirectory = Other.Path;
		}
		bOk &= Reconfigure(T, Rg);
		const TCHAR* Name = End == EHostEnd::DiskTierOff ? TEXT("disk tier off") : TEXT("another directory");
		// Drained before Configure() returned: the file is gone with no tick after it.
		bOk &= T.TestFalse(*FString::Printf(TEXT("%s: S's file in the old directory is gone when Configure() returns"), Name), FPaths::FileExists(Path));
		const FSuperSLMPrefixOpResult R = WaitPrefixOp(Gpu, Op, OpSeconds);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the drop reads DropDeferred (read %d, disk %d)"), Name, (int32)R.Result, (int32)R.Disk), (uint8)R.Result, (uint8)EOp::DropDeferred);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: its disk part reads Deleted (not DiskTierOff, not NoFile)"), Name), (uint8)R.Disk, (uint8)EDisk::Deleted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the new directory gained no file"), Name), Other.Files().Num(), FilesInOther);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: no handle released twice"), Name), Seam::MaxReleasesOfOneHandleAtLastTeardown(Gpu) <= 1);
		return bOk;
#endif
	}
}

// Cell 1.10 (rev 1.18; PC-16 (a)), scaled from 1,000 cycles to 50 (a GPU build per cycle; the host cell runs the
// 1,000): warm -> request -> adopt -> return -> drop over six keys of mixed lengths (16, 20, 32, 33 tokens), drops of
// leased and unleased entries interleaved (a fixed LCG). Physical and no handle released twice at every 10th cycle.
// At the end every key is dropped and everything lands: nothing resident or charged, region C at 0, every built
// handle released exactly once, and the pool's free count (the fill probe) back at its initial reading. 1.7's
// teardown reading has no GPU seam (TeardownFreePages is CPU only), so the end reading is taken before the teardown.
// Mutants: a leased drop released at once and again at its return -> MaxReleasesOfOneHandle 2 -> red; a dropped
// charge never refunded -> C not 0 at the end -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyChurnTest, "SuperSLM.PrefixCache.Gpu.Residency.Churn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyChurnTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	int64 Free0 = -1;
	if (!R::ProbeFree(*this, Gpu, R::LedgerOf(Gpu).PoolPages, Free0))
	{
		return false;
	}
	const int32 Lengths[] = {16, 20, 32, 33};
	TArray<TArray<int32>> Keys;
	for (int32 K = 0; K < 6; ++K)
	{
		if (!R::Tokens(*this, Rg, Lengths[K % 4], Keys.AddDefaulted_GetRef()))
		{
			return false;
		}
	}
	FSuperSLMGpuSequence Seq;
	if (!R::Vend(*this, Gpu, Seq))
	{
		return false;
	}
	uint32 X = 12345;
	auto Next = [&X](uint32 N)
	{
		X = X * 1664525u + 1013904223u;
		return (X >> 8) % N;
	};
	const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
	int32 Dropped = 0;
	int32 Deferred = 0;
	bool bOk = true;
	for (int32 Cycle = 0; Cycle < 50; ++Cycle)
	{
		const FSuperSLMPrefixSpec S = R::Spec(Keys[int32(Next(6))]);
		FSuperSLMPrefixOp W, D;
		FSuperSLMPrefix H;
		if (!R::Warm(*this, Gpu, S, W, TEXT("churn warm")) || !R::ExpectOp(*this, Gpu, W, R::EOp::Resident, TEXT("churn warm"))
			|| !R::Request(*this, Gpu, S, H, TEXT("churn request")) || !R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("churn request"))
			|| !R::AdoptAndWait(*this, Gpu, Seq, H) || !R::ResetAndWait(*this, Gpu, Seq))
		{
			return false;
		}
		if (Next(2) == 0)
		{
			if (!R::Drop(*this, Gpu, S, R::EScope::Memory, D, TEXT("churn leased drop")))
			{
				return false;
			}
			R::WaitPrefixOp(Gpu, D);
			Gpu.ReturnPrefix(H);
		}
		else
		{
			Gpu.ReturnPrefix(H);
			R::Ticks(Gpu, 1);
			if (!R::Drop(*this, Gpu, S, R::EScope::Memory, D, TEXT("churn drop")))
			{
				return false;
			}
		}
		const R::EOp Result = R::WaitPrefixOp(Gpu, D).Result;
		Dropped += Result == R::EOp::Dropped ? 1 : 0;
		Deferred += Result == R::EOp::DropDeferred ? 1 : 0;
		if (!TestTrue(*FString::Printf(TEXT("cycle %d: the drop resolves Dropped or DropDeferred"), Cycle), Result == R::EOp::Dropped || Result == R::EOp::DropDeferred))
		{
			return false;
		}
		if (Cycle % 10 == 9)
		{
			bOk &= R::Quiet(Gpu);
			bOk &= R::Physical(*this, Gpu, *FString::Printf(TEXT("cycle %d"), Cycle));
			bOk &= TestTrue(*FString::Printf(TEXT("cycle %d: no handle released twice"), Cycle), R::Seam::MaxReleasesOfOneHandle(Gpu) <= 1);
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("some drops were Dropped (%d)"), Dropped), Dropped > 0);
	bOk &= TestTrue(*FString::Printf(TEXT("some drops were DropDeferred (%d)"), Deferred), Deferred > 0);
	for (const TArray<int32>& K : Keys)
	{
		FSuperSLMPrefixOp D;
		bOk &= R::Drop(*this, Gpu, R::Spec(K), R::EScope::Memory, D, TEXT("the final drop")) && R::WaitPrefixOp(Gpu, D).Result != R::EOp::Pending;
	}
	bOk &= R::ReturnAndDrain(Gpu, Seq);
	bOk &= R::Quiet(Gpu);
	const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
	bOk &= TestEqual(TEXT("the end: nothing resident"), S1.ResidentEntries, 0);
	bOk &= TestEqual(TEXT("the end: no refund pending"), S1.PendingRefunds, 0);
	bOk &= TestEqual(TEXT("the end: the cache charges nothing"), S1.ChargedBytes, int64(0));
	bOk &= TestEqual(TEXT("the end: region C at 0"), R::LedgerOf(Gpu).ChargedOf(R::ERegion::C), int64(0));
	bOk &= TestEqual(TEXT("the end: no handle released twice"), R::Seam::MaxReleasesOfOneHandle(Gpu), 1);
	bOk &= TestEqual(TEXT("the end: every built handle released (none left unreleased)"), S1.ReleasesIssued - S0.ReleasesIssued, S1.BuildsIssued - S0.BuildsIssued);
	int64 Free1 = -1;
	bOk &= R::ProbeFree(*this, Gpu, R::LedgerOf(Gpu).PoolPages, Free1);
	bOk &= TestEqual(TEXT("the end: the pool at its initial free count (fill probe)"), Free1, Free0);
	bOk &= R::Physical(*this, Gpu, TEXT("the end"));
	return bOk;
}

// Cell 2.16 (rev 1.18, 1.18.1): WarmPrefix and DropPrefix with each malformed spec RequestPrefix refuses (empty, a
// token out of the vocabulary either side, over ContextCap; on the GPU also a child under an ended lease and a child
// naming a schema) are refused synchronously with a message and no op id, and no warm is counted. GetPrefixOpResult
// on an id never issued is false. The ring is a pair: the 1,024th most recent resolved op reads its result, the
// 1,025th Expired (still found). An op Pending (its build held) while 1,100 later ops resolve still reads Pending.
// A drop of a spec that differs from a resident entry's in one key component (schema, a token, the length either
// way) is NotCached, nothing is released, and the entry stays a Memory hit.
// GPU reading of rev 1.18.1's "op results survive a re-Configure()": the GPU cache ends with its configuration (the
// GPU host belongs to it), so op results do NOT survive: after a re-Configure() a resolved warm, a resolved drop and
// a warm still Pending (its hash held) all read false (not issued by this configuration's cache), not their results
// and not Cancelled. The cell then checks the op type's own contract (FSuperSLMPrefixOp: "Ids never repeat within the
// subsystem's lifetime"): the first op of the new configuration's cache must not reuse the old configuration's
// first id.
// Mutants: a warm that accepts a malformed spec -> an op id -> red; a ring of 1,023 -> the 1,024th Expired -> red; a
// drop matched by token ids only -> the schema variant evicts E -> red on its Memory hit; op results carried into the
// new configuration -> true after the re-Configure() -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyMalformedOpsTest, "SuperSLM.PrefixCache.Gpu.Residency.MalformedOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyMalformedOpsTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	FSuperSLMModelShapeFacts Shape;
	if (!TestTrue(TEXT("the model's shape reads (CFG1)"), SuperSLMModelInspector::ReadShape(*Rg.Rig.Model, Shape)) || !TestTrue(TEXT("a vocabulary"), Shape.VocabSize > 2))
	{
		return false;
	}
	bool bOk = true;

	// Malformed specs, refused synchronously.
	{
		TArray<int32> Good, ParentT, Suffix;
		FSuperSLMPrefix Parent;
		if (!R::PageTokens(*this, Rg, 1, Good) || !R::Root(*this, Rg, 1, false, Parent, ParentT, TEXT("an ended parent")) || !R::PageTokens(*this, Rg, 1, Suffix))
		{
			return false;
		}
		TArray<TPair<FString, FSuperSLMPrefixSpec>> Bad;
		Bad.Emplace(FString(TEXT("empty")), FSuperSLMPrefixSpec());
		FSuperSLMPrefixSpec Oov = R::Spec(Good);
		Oov.Tokens[3] = Shape.VocabSize;
		Bad.Emplace(FString(TEXT("a token = VocabSize")), Oov);
		Oov.Tokens[3] = -1;
		Bad.Emplace(FString(TEXT("a token = -1")), Oov);
		FSuperSLMPrefixSpec Long;
		Long.Tokens.Init(Good[1], int32(Rg.Rig.Config.ContextCap) + 1);
		Bad.Emplace(FString(TEXT("ContextCap + 1 tokens")), Long);
		Bad.Emplace(FString(TEXT("a child of an ended lease")), R::ChildSpec(Parent, Suffix));
		FSuperSLMPrefix LiveParent;
		TArray<int32> LiveT;
		if (!R::Root(*this, Rg, 1, true, LiveParent, LiveT, TEXT("a live parent")))
		{
			return false;
		}
		FSuperSLMPrefixSpec Named = R::ChildSpec(LiveParent, Suffix);
		Named.SchemaName = FName(TEXT("trade"));
		Bad.Emplace(FString(TEXT("a child naming a schema")), Named);
		const int64 Warms0 = R::Stats(Gpu).Warms;
		for (const TPair<FString, FSuperSLMPrefixSpec>& Case : Bad)
		{
			const TCHAR* What = *Case.Key;
			FSuperSLMPrefix Refused;
			FString Error;
			bOk &= TestFalse(*FString::Printf(TEXT("%s: RequestPrefix refuses it (the control)"), What), Gpu.RequestPrefix(Case.Value, Refused, Error));
			FSuperSLMPrefixOp Op;
			Op.Id = 99;
			Error.Reset();
			bOk &= TestFalse(*FString::Printf(TEXT("%s: WarmPrefix refuses it synchronously"), What), Gpu.WarmPrefix(Case.Value, Op, Error));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the warm issues no op"), What), Op.Id, int64(0));
			bOk &= TestFalse(*FString::Printf(TEXT("%s: the warm's refusal says why"), What), Error.IsEmpty());
			for (const R::EScope Scope : {R::EScope::Memory, R::EScope::MemoryAndDisk})
			{
				Op.Id = 99;
				Error.Reset();
				bOk &= TestFalse(*FString::Printf(TEXT("%s: DropPrefix(scope %d) refuses it synchronously"), What, (int32)Scope), Gpu.DropPrefix(Case.Value, Scope, Op, Error));
				bOk &= TestEqual(*FString::Printf(TEXT("%s: the drop issues no op"), What), Op.Id, int64(0));
			}
		}
		bOk &= TestEqual(TEXT("no warm counted for a refused spec"), R::Stats(Gpu).Warms, Warms0);
		Gpu.ReturnPrefix(LiveParent);
		bOk &= R::Quiet(Gpu);
	}

	// An id never issued; the first op (kept for the re-Configure leg).
	FSuperSLMPrefixOp First;
	{
		FSuperSLMPrefixOpResult Out;
		FSuperSLMPrefixOp Never;
		bOk &= TestFalse(TEXT("GetPrefixOpResult(id 0) is false"), Gpu.GetPrefixOpResult(Never, Out));
		Never.Id = 987654321;
		bOk &= TestFalse(TEXT("GetPrefixOpResult(an id never issued) is false"), Gpu.GetPrefixOpResult(Never, Out));
		TArray<int32> WT;
		if (!R::PageTokens(*this, Rg, 1, WT) || !R::Warm(*this, Gpu, R::Spec(WT), First, TEXT("a well-formed warm (the control)")))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("its op is found"), Gpu.GetPrefixOpResult(First, Out));
		Never.Id = First.Id + 100000;
		bOk &= TestFalse(TEXT("an id past the last issued is false"), Gpu.GetPrefixOpResult(Never, Out));
		bOk &= R::ExpectOp(*this, Gpu, First, R::EOp::Resident, TEXT("the control warm"));
	}

	// The ring boundary.
	{
		int32 Next = 0;
		FSuperSLMPrefixOp A, B, Last;
		if (!R::Drop(*this, Gpu, R::Absent(Next++), R::EScope::Memory, A, TEXT("ring: the first")) || !R::ExpectOp(*this, Gpu, A, R::EOp::NotCached, TEXT("ring: the first")))
		{
			return false;
		}
		if (!R::Drop(*this, Gpu, R::Absent(Next++), R::EScope::Memory, B, TEXT("ring: the second")))
		{
			return false;
		}
		for (int32 I = 0; I < R::kOpRing - 2; ++I)
		{
			FString Error;
			bOk &= Gpu.DropPrefix(R::Absent(Next++), R::EScope::Memory, Last, Error);
		}
		bOk &= TestTrue(TEXT("ring: the 1,022 drops resolve"), R::WaitPrefixOp(Gpu, Last, R::OpSeconds).Result == R::EOp::NotCached);
		bOk &= TestEqual(TEXT("ring: the 1,024th most recent resolved op reads its result"), (uint8)R::OpOf(Gpu, A).Result, (uint8)R::EOp::NotCached);
		bOk &= TestEqual(TEXT("ring: the 1,023rd too"), (uint8)R::OpOf(Gpu, B).Result, (uint8)R::EOp::NotCached);
		if (!R::Drop(*this, Gpu, R::Absent(Next++), R::EScope::Memory, Last, TEXT("ring: one more")))
		{
			return false;
		}
		R::WaitPrefixOp(Gpu, Last, R::OpSeconds);
		bool bFound = false;
		bOk &= TestEqual(TEXT("ring: the 1,025th reads Expired"), (uint8)R::OpOf(Gpu, A, &bFound).Result, (uint8)R::EOp::Expired);
		bOk &= TestTrue(TEXT("ring: an expired id is still an id it issued (found)"), bFound);
		bOk &= TestEqual(TEXT("ring: the new 1,024th reads its result"), (uint8)R::OpOf(Gpu, B).Result, (uint8)R::EOp::NotCached);

		// A Pending op never expires.
		TArray<int32> PT;
		FSuperSLMPrefixOp Held;
		R::Seam::SetBuildHold(Gpu, true);
		if (!R::PageTokens(*this, Rg, 1, PT) || !R::Warm(*this, Gpu, R::Spec(PT), Held, TEXT("the held warm")) || !R::WaitHeldBuild(*this, Gpu, TEXT("the held warm")))
		{
			R::Seam::SetBuildHold(Gpu, false);
			return false;
		}
		for (int32 I = 0; I < 1100; ++I)
		{
			FString Error;
			bOk &= Gpu.DropPrefix(R::Absent(Next++), R::EScope::Memory, Last, Error);
		}
		bOk &= TestTrue(TEXT("the 1,100 later drops resolve"), R::WaitPrefixOp(Gpu, Last, R::OpSeconds).Result == R::EOp::NotCached);
		bOk &= TestEqual(TEXT("an op Pending through 1,100 later resolutions still reads Pending"), (uint8)R::OpOf(Gpu, Held, &bFound).Result, (uint8)R::EOp::Pending);
		bOk &= TestTrue(TEXT("... and is found"), bFound);
		R::Seam::SetBuildHold(Gpu, false);
		bOk &= R::ExpectOp(*this, Gpu, Held, R::EOp::Resident, TEXT("the held warm, released"));
		bOk &= R::Quiet(Gpu);
	}

	// A drop matches the whole key.
	{
		TArray<int32> ET;
		FSuperSLMPrefix E;
		if (!R::Root(*this, Rg, 1, false, E, ET, TEXT("E")))
		{
			return false;
		}
		TArray<TPair<FString, FSuperSLMPrefixSpec>> Near;
		FSuperSLMPrefixSpec V = R::Spec(ET);
		V.SchemaName = FName(TEXT("trade"));
		Near.Emplace(FString(TEXT("another schema")), V);
		Near.Emplace(FString(TEXT("one token changed")), R::Spec(R::WithTokenChanged(ET, 5)));
		V = R::Spec(ET);
		V.Tokens.Pop();
		Near.Emplace(FString(TEXT("one token shorter")), V);
		V = R::Spec(ET);
		V.Tokens.Add(ET[1]);
		Near.Emplace(FString(TEXT("one token longer")), V);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		for (const TPair<FString, FSuperSLMPrefixSpec>& Case : Near)
		{
			FSuperSLMPrefixOp Op;
			bOk &= R::Drop(*this, Gpu, Case.Value, R::EScope::MemoryAndDisk, Op, *Case.Key)
				&& R::ExpectOp(*this, Gpu, Op, R::EOp::NotCached, *FString::Printf(TEXT("whole key: %s"), *Case.Key));
		}
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("whole key: nothing dropped"), R::Stats(Gpu).Drops - S0.Drops, int64(0));
		bOk &= TestEqual(TEXT("whole key: nothing released"), R::Stats(Gpu).ReleasesIssued - S0.ReleasesIssued, int64(0));
		FSuperSLMPrefix Again;
		bOk &= R::Lease(*this, Rg, R::Spec(ET), true, Again, TEXT("E again"));
		bOk &= R::ExpectSource(*this, Gpu, Again, ESuperSLMPrefixSource::Memory, TEXT("whole key: E stays a Memory hit"));
		Gpu.ReturnPrefix(Again);
		bOk &= R::Quiet(Gpu);
	}

	// The re-Configure leg (GPU: op results end with the configuration).
	{
		TArray<int32> WT, HT;
		FSuperSLMPrefixOp Warmed, AbsentDrop, HeldWarm;
		if (!R::PageTokens(*this, Rg, 1, WT) || !R::PageTokens(*this, Rg, 1, HT))
		{
			return false;
		}
		bOk &= R::Warm(*this, Gpu, R::Spec(WT), Warmed, TEXT("reconfigure: a warm")) && R::ExpectOp(*this, Gpu, Warmed, R::EOp::Resident, TEXT("reconfigure: the warm"));
		bOk &= R::Drop(*this, Gpu, R::Absent(5000), R::EScope::Memory, AbsentDrop, TEXT("reconfigure: an absent drop"))
			&& R::ExpectOp(*this, Gpu, AbsentDrop, R::EOp::NotCached, TEXT("reconfigure: the absent drop"));
		R::Seam::HoldKeyHash(Gpu, R::Spec(HT), true);
		bOk &= R::Warm(*this, Gpu, R::Spec(HT), HeldWarm, TEXT("reconfigure: a warm whose hash is held"));
		R::SettleIo(Gpu);
		bOk &= TestEqual(TEXT("reconfigure: the held warm is Pending"), (uint8)R::OpOf(Gpu, HeldWarm).Result, (uint8)R::EOp::Pending);
		if (!R::Reconfigure(*this, Rg))
		{
			return false;
		}
		FSuperSLMPrefixOpResult Out;
		// As the CPU's (host residency_op_results_survive_reconfigure): the GPU subsystem keeps the last ended
		// configuration's cache, so a resolved op reads its own result and a warm the teardown cancelled reads Cancelled.
		bOk &= TestTrue(TEXT("after a re-Configure: the resolved warm is found"), Gpu.GetPrefixOpResult(Warmed, Out))
			&& TestEqual(TEXT("after a re-Configure: the resolved warm reads Resident"), (uint8)Out.Result, (uint8)R::EOp::Resident);
		bOk &= TestTrue(TEXT("after a re-Configure: the resolved drop is found"), Gpu.GetPrefixOpResult(AbsentDrop, Out))
			&& TestEqual(TEXT("after a re-Configure: the resolved drop reads NotCached"), (uint8)Out.Result, (uint8)R::EOp::NotCached);
		bOk &= TestTrue(TEXT("after a re-Configure: the warm that was Pending is found"), Gpu.GetPrefixOpResult(HeldWarm, Out))
			&& TestEqual(TEXT("after a re-Configure: the warm that was Pending reads Cancelled"), (uint8)Out.Result, (uint8)ESuperSLMPrefixOpResult::Cancelled);
		bOk &= TestTrue(TEXT("after a re-Configure: the first op is found"), Gpu.GetPrefixOpResult(First, Out));
		TArray<int32> NT;
		FSuperSLMPrefixOp After;
		if (!R::PageTokens(*this, Rg, 1, NT) || !R::Warm(*this, Gpu, R::Spec(NT), After, TEXT("after a re-Configure: a warm")))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("op ids never repeat within the subsystem's lifetime (FSuperSLMPrefixOp): the new configuration's first op %lld is none of the old ones (%lld, %lld, %lld, %lld)"),
			After.Id, First.Id, Warmed.Id, AbsentDrop.Id, HeldWarm.Id),
			After.Id != First.Id && After.Id != Warmed.Id && After.Id != AbsentDrop.Id && After.Id != HeldWarm.Id);
		bOk &= R::ExpectOp(*this, Gpu, After, R::EOp::Resident, TEXT("after a re-Configure: the warm"));
	}
	return bOk;
}

// Cell 3.10 (rev 1.18, 1.18.1, 1.18.2), the serialization legs, with the disk tier on:
// - behind a held write-back: DropPrefix(MemoryAndDisk) while the key's write-back is held mid-write reads Pending;
//   released, the write-back completes, then the delete: Dropped / Deleted, no file and no temporary left, one delete
//   counted, and neither the exclusion counter nor the per-key overlap counter ever read 2;
// - behind a held disk read (vii): a request that hits the disk tier, its read held; the drop reads Pending; released,
//   the read serves (Disk source), then the delete: DropDeferred (the lease pins the restored entry) / Deleted, never
//   Failed, no file, the per-key counter never 2;
// - during a build: the build serves, no file is written for it, the mark applies at its return, and a request after
//   that writes back normally;
// - (i) a request issued after the drop that attaches to the build in flight is served, one build, the mark cleared,
//   and that build's write-back is restored (the file exists after it completes);
// - (ii) while the delete is held (HoldDropDeletes) the op reads Pending; released, Dropped / Deleted;
// - a drop racing a trim (HoldDropDeletes, HoldTrims), in each order: the second never starts while the first is held,
//   the directory ends without the key's file and within its limit, and the exclusion counter never reads 2;
// - (v) the save queued and held (SetSaveHold, the GPU's lane-park seam), no file write issued: the drop reads Pending;
//   released, the save, its file write, then the delete (Deleted), and no file;
// - (vi) a build refused PoolExhausted (SetNextCacheBuildStatus) waiting to retry on an eviction whose release is held:
//   DropDeferred; the retry serves and no file is written;
// - (iii) a delete held at a re-Configure(): drained in the teardown under the old configuration, run once, the file
//   gone when Configure() returns, and the op Dropped / Deleted after (the product path; rev 1.18.5).
// (iv), the waiter under deferred-refund admission, needs region C exactly 6 behind a pad: Gpu.Residency.DropDiskPartWaiter.
// (iii)'s three host-end runs (disk tier off, another directory, Deinitialize()) are Gpu.U2Review.DeleteBehindSave.*.
// The host oracles are test_residency_disk.cpp's 3.10 cells.
// Mutants: the delete not serialized behind the write-back -> a file after the op, or the counter 2 -> red; the delete
// not serialized with the read -> Disk == Failed or the counter 2 -> red; the attach leaves write-back suppressed -> no
// file -> red; a trim and a delete run together -> the exclusion counter 2 -> red; write-back read as the file write
// only -> NoFile and a file -> red (v); a retry wait not counted in flight -> a file -> red (vi); the delete re-run
// after Configure() -> two deletes counted -> red (iii).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyDropDiskPartTest, "SuperSLM.PrefixCache.Gpu.Residency.DropDiskPart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyDropDiskPartTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE): the disk tier's write-backs and reads"));
	return false;
#else
	namespace R = SuperSLMGpuResidencyCells;
	R::FScopedCacheDirectory Dir(TEXT("gpu-residency-drop-disk"));
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 0, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	bool bOk = true;

	// Behind a held write-back (first, so the directory holds only this key's file).
	{
		TArray<int32> KT;
		FSuperSLMPrefix L;
		if (!R::PageTokens(*this, Rg, 1, KT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec K = R::Spec(KT);
		const int64 Deletes0 = R::Stats(Gpu).DropDeletes;
		R::Seam::HoldWriteBacks(Gpu, true);
		if (!R::Request(*this, Gpu, K, L, TEXT("write-back: K")) || !R::Expect(*this, Gpu, L, R::ELease::Ready, TEXT("write-back: K")))
		{
			R::Seam::HoldWriteBacks(Gpu, false);
			return false;
		}
		Gpu.ReturnPrefix(L);
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldWriteBackCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("write-back: K's write-back is held mid-write"), R::Seam::HeldWriteBackCount(Gpu), 1);
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, K, R::EScope::MemoryAndDisk, D, TEXT("write-back")))
		{
			R::Seam::HoldWriteBacks(Gpu, false);
			return false;
		}
		R::Ticks(Gpu, 20);
		bOk &= TestEqual(TEXT("write-back: the drop's delete waits behind the write-back (Pending)"), (uint8)R::OpOf(Gpu, D).Result, (uint8)R::EOp::Pending);
		R::Seam::HoldWriteBacks(Gpu, false);
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(TEXT("write-back: the memory part applied (Dropped)"), (uint8)Res.Result, (uint8)R::EOp::Dropped);
		bOk &= TestEqual(TEXT("write-back: then the delete ran (Deleted)"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
		bOk &= R::Quiet(Gpu);
		bOk &= TestFalse(TEXT("write-back: no file under the key"), R::HasFile(Gpu, K));
		bOk &= TestEqual(TEXT("write-back: no file and no temporary left in the directory"), Dir.Files().Num(), 0);
		bOk &= TestEqual(TEXT("write-back: one drop delete"), R::Stats(Gpu).DropDeletes - Deletes0, int64(1));
		bOk &= TestTrue(TEXT("write-back: the exclusion counter never read 2"), R::Seam::MaxDiskExclusion(Gpu) < 2);
		bOk &= TestTrue(TEXT("write-back: the per-key overlap counter never read 2"), R::Seam::MaxKeyDiskOverlap(Gpu) < 2);
	}

	// Behind a held disk read (vii).
	{
		TArray<int32> ST;
		FSuperSLMPrefix L, H;
		if (!R::Root(*this, Rg, 1, false, L, ST, TEXT("read: S")))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(ST);
		FSuperSLMPrefixOp M, D;
		if (!R::WaitFile(*this, Gpu, S, true, TEXT("read: S's write-back"))
			|| !R::Drop(*this, Gpu, S, R::EScope::Memory, M, TEXT("read: S out of memory")) || !R::ExpectOp(*this, Gpu, M, R::EOp::Dropped, TEXT("read: S out of memory"))
			|| !R::Quiet(Gpu) || !TestTrue(TEXT("read: S's file stays"), R::HasFile(Gpu, S)))
		{
			return false;
		}
		R::Seam::HoldDiskReads(Gpu, true);
		if (!R::Request(*this, Gpu, S, H, TEXT("read: S from disk")))
		{
			R::Seam::HoldDiskReads(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldDiskReadCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("read: S's disk read is held mid-read"), R::Seam::HeldDiskReadCount(Gpu), 1);
		if (!R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, TEXT("read")))
		{
			R::Seam::HoldDiskReads(Gpu, false);
			return false;
		}
		R::Ticks(Gpu, 20);
		bOk &= TestEqual(TEXT("read: the drop waits behind the read (Pending)"), (uint8)R::OpOf(Gpu, D).Result, (uint8)R::EOp::Pending);
		R::Seam::HoldDiskReads(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("read: the read serves"));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Disk, TEXT("read: served"));
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(TEXT("read: the memory part (DropDeferred: the lease pins the restored entry)"), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
		bOk &= TestEqual(TEXT("read: then the delete ran (Deleted, never Failed)"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
		bOk &= R::Quiet(Gpu);
		bOk &= TestFalse(TEXT("read: no file under the key"), R::HasFile(Gpu, S));
		bOk &= TestTrue(TEXT("read: the per-key overlap counter never read 2"), R::Seam::MaxKeyDiskOverlap(Gpu) < 2);
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
	}

	// During a build: no file for it; the mark applies at its return; a later request writes back.
	{
		TArray<int32> TT;
		FSuperSLMPrefix H, H2;
		if (!R::PageTokens(*this, Rg, 1, TT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(TT);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		FSuperSLMPrefixOp D;
		R::Seam::SetBuildHold(Gpu, true);
		if (!R::Request(*this, Gpu, S, H, TEXT("build: T")) || !R::WaitHeldBuild(*this, Gpu, TEXT("build: T"))
			|| !R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, TEXT("build")))
		{
			R::Seam::SetBuildHold(Gpu, false);
			return false;
		}
		R::WaitPrefixOp(Gpu, D, R::OpSeconds);
		R::Seam::SetBuildHold(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("build: the build serves"));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, TEXT("build: T"));
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(TEXT("build: the drop marks (DropDeferred)"), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
		bOk &= TestTrue(TEXT("build: its disk part ran (Deleted or NoFile)"), Res.Disk == R::EDisk::Deleted || Res.Disk == R::EDisk::NoFile);
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("build: no write-back issued for it"), R::Stats(Gpu).DiskWritesIssued - S0.DiskWritesIssued, int64(0));
		bOk &= TestFalse(TEXT("build: no file under the key"), R::HasFile(Gpu, S));
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("build: the mark applies at the return"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(1));
		bOk &= R::Lease(*this, Rg, S, false, H2, TEXT("build: a request after the drop"));
		bOk &= R::WaitFile(*this, Gpu, S, true, TEXT("build: the later request writes back normally"));
	}

	// (i) A request attached after the drop restores the write-back.
	{
		TArray<int32> UT;
		FSuperSLMPrefix H0, H1;
		if (!R::PageTokens(*this, Rg, 1, UT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(UT);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		FSuperSLMPrefixOp D;
		R::Seam::SetBuildHold(Gpu, true);
		if (!R::Request(*this, Gpu, S, H0, TEXT("attach: U")) || !R::WaitHeldBuild(*this, Gpu, TEXT("attach: U"))
			|| !R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, TEXT("attach")))
		{
			R::Seam::SetBuildHold(Gpu, false);
			return false;
		}
		R::WaitPrefixOp(Gpu, D, R::OpSeconds);
		bOk &= R::Request(*this, Gpu, S, H1, TEXT("attach: U after the drop"));
		R::SettleIo(Gpu);
		R::Seam::SetBuildHold(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H0, R::ELease::Ready, TEXT("attach: the first request"));
		bOk &= R::Expect(*this, Gpu, H1, R::ELease::Ready, TEXT("attach: the attached request"));
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestTrue(TEXT("attach: the drop's disk part ran"), Res.Result != R::EOp::Pending && (Res.Disk == R::EDisk::Deleted || Res.Disk == R::EDisk::NoFile));
		bOk &= TestEqual(TEXT("attach: one build"), R::Stats(Gpu).BuildsIssued - S0.BuildsIssued, int64(1));
		bOk &= TestEqual(TEXT("attach: the attach cleared the mark"), R::Stats(Gpu).MarksCleared - S0.MarksCleared, int64(1));
		bOk &= R::WaitFile(*this, Gpu, S, true, TEXT("attach: the build's write-back restored"));
		Gpu.ReturnPrefix(H0);
		Gpu.ReturnPrefix(H1);
		bOk &= R::Quiet(Gpu);
	}

	// (ii) While the delete is held (HoldDropDeletes) the op reads Pending; after its release Disk == Deleted (the host's
	// disk_held_delete_reads_pending).
	{
		TArray<int32> ET;
		FSuperSLMPrefix L;
		if (!R::Root(*this, Rg, 1, false, L, ET, TEXT("held delete: E")))
		{
			return false;
		}
		const FSuperSLMPrefixSpec E = R::Spec(ET);
		if (!R::WaitFile(*this, Gpu, E, true, TEXT("held delete: E's write-back")))
		{
			return false;
		}
		R::Seam::HoldDropDeletes(Gpu, true);
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, E, R::EScope::MemoryAndDisk, D, TEXT("held delete")))
		{
			R::Seam::HoldDropDeletes(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldDropDeleteCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("held delete: the delete is held running"), R::Seam::HeldDropDeleteCount(Gpu), 1);
		R::Ticks(Gpu, 20);
		bOk &= TestEqual(TEXT("held delete: the op reads Pending"), (uint8)R::OpOf(Gpu, D).Result, (uint8)R::EOp::Pending);
		bOk &= TestTrue(TEXT("held delete: the file is still there"), R::HasFile(Gpu, E));
		R::Seam::HoldDropDeletes(Gpu, false);
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(TEXT("held delete: released, the memory part (Dropped)"), (uint8)Res.Result, (uint8)R::EOp::Dropped);
		bOk &= TestEqual(TEXT("held delete: released, Disk == Deleted"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
		bOk &= R::Quiet(Gpu);
		bOk &= TestFalse(TEXT("held delete: no file under the key"), R::HasFile(Gpu, E));
	}

	// The trim race (base cell; the host's disk_drop_racing_a_trim): a drop's delete and a trim, both held running
	// (HoldDropDeletes, HoldTrims), in each order. The trim is the one X's write-back emits (§5.7: trimmed after writes).
	// Whichever runs first, the other does not start while it is held (the core defers a trim behind a drop's delete
	// and a drop's delete behind a trim, and the host runs a trim alone); released in turn, the directory ends without
	// E's file and within its limit, and the exclusion counter never read 2 (read at the end of the cell). The GPU rig's
	// limit is 1 GiB, so the trim removes nothing here: the race is the serialization, which the host cell grades with a
	// tight limit too.
	for (const bool bTrimFirst : {true, false})
	{
		const TCHAR* Name = bTrimFirst ? TEXT("trim race, trim first") : TEXT("trim race, delete first");
		TArray<int32> ET, XT;
		FSuperSLMPrefix LE, LX;
		if (!R::Root(*this, Rg, 1, false, LE, ET, Name) || !R::PageTokens(*this, Rg, 1, XT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec E = R::Spec(ET);
		const FSuperSLMPrefixSpec X = R::Spec(XT);
		if (!R::WaitFile(*this, Gpu, E, true, *FString::Printf(TEXT("%s: E's write-back"), Name)))
		{
			return false;
		}
		auto Unhold = [&Gpu]()
		{
			R::Seam::HoldTrims(Gpu, false);
			R::Seam::HoldDropDeletes(Gpu, false);
		};
		R::Seam::HoldTrims(Gpu, true);
		R::Seam::HoldDropDeletes(Gpu, true);
		FSuperSLMPrefixOp D;
		if (bTrimFirst)
		{
			if (!R::Request(*this, Gpu, X, LX, Name) || !R::Expect(*this, Gpu, LX, R::ELease::Ready, Name))
			{
				Unhold();
				return false;
			}
			Gpu.ReturnPrefix(LX);
			R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldTrimCount(Gpu) == 1; }, R::OpSeconds);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: X's write-back emitted a trim, held running"), Name), R::Seam::HeldTrimCount(Gpu), 1);
			if (!R::Drop(*this, Gpu, E, R::EScope::MemoryAndDisk, D, Name))
			{
				Unhold();
				return false;
			}
			R::Ticks(Gpu, 20);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the delete does not start while the trim runs"), Name), R::Seam::HeldDropDeleteCount(Gpu), 0);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the drop reads Pending"), Name), (uint8)R::OpOf(Gpu, D).Result, (uint8)R::EOp::Pending);
			R::Seam::HoldTrims(Gpu, false);
			R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldDropDeleteCount(Gpu) == 1; }, R::OpSeconds);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the trim released, then the delete runs (held)"), Name), R::Seam::HeldDropDeleteCount(Gpu), 1);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the trim is no longer running"), Name), R::Seam::HeldTrimCount(Gpu), 0);
			R::Seam::HoldDropDeletes(Gpu, false);
		}
		else
		{
			if (!R::Drop(*this, Gpu, E, R::EScope::MemoryAndDisk, D, Name))
			{
				Unhold();
				return false;
			}
			R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldDropDeleteCount(Gpu) == 1; }, R::OpSeconds);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the delete is held running"), Name), R::Seam::HeldDropDeleteCount(Gpu), 1);
			if (!R::Request(*this, Gpu, X, LX, Name) || !R::Expect(*this, Gpu, LX, R::ELease::Ready, Name))
			{
				Unhold();
				return false;
			}
			Gpu.ReturnPrefix(LX);
			// X's write-back is another key's disk command, so it runs beside the held delete; the trim it emits waits.
			R::Pump(Gpu, [&Gpu, &X]() { return R::HasFile(Gpu, X); }, R::OpSeconds);
			bOk &= TestTrue(*FString::Printf(TEXT("%s: X's write-back landed beside the held delete"), Name), R::HasFile(Gpu, X));
			R::Ticks(Gpu, 20);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the trim does not start while the delete runs"), Name), R::Seam::HeldTrimCount(Gpu), 0);
			R::Seam::HoldDropDeletes(Gpu, false);
			R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldTrimCount(Gpu) == 1; }, R::OpSeconds);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the delete released, then the trim runs (held)"), Name), R::Seam::HeldTrimCount(Gpu), 1);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the delete is no longer running"), Name), R::Seam::HeldDropDeleteCount(Gpu), 0);
			R::Seam::HoldTrims(Gpu, false);
		}
		Unhold();
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the drop (Dropped)"), Name), (uint8)Res.Result, (uint8)R::EOp::Dropped);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: its disk part ran (Deleted or NoFile)"), Name), Res.Disk == R::EDisk::Deleted || Res.Disk == R::EDisk::NoFile);
		bOk &= R::Quiet(Gpu);
		bOk &= TestFalse(*FString::Printf(TEXT("%s: no file under E's key"), Name), R::HasFile(Gpu, E));
		bOk &= TestTrue(*FString::Printf(TEXT("%s: the directory within its limit (%lld <= %lld bytes)"), Name, Dir.TotalBytes(), Rg.Settings.DiskMaxBytes),
			Dir.TotalBytes() <= Rg.Settings.DiskMaxBytes);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: one runs at a time (the exclusion counter never read 2)"), Name), R::Seam::MaxDiskExclusion(Gpu) < 2);
	}

	// (v) The request's build has applied and its save is queued, held by the save hold (the GPU's lane-park seam), with
	// no file write issued: the drop reads Pending. Released, the save, then its file write, then the delete run on the
	// key's serialization: Disk == Deleted (no file was under the key before, so the write landed first) and no file is
	// left (the host's disk_drop_behind_a_parked_save). Mutant: write-back read as the file write only -> the save's
	// later write lands after the delete -> NoFile and a file -> red.
	{
		TArray<int32> VT;
		FSuperSLMPrefix H;
		if (!R::PageTokens(*this, Rg, 1, VT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(VT);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		R::Seam::SetSaveHold(Gpu, true);
		if (!R::Request(*this, Gpu, S, H, TEXT("parked save: V")) || !R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("parked save: V")))
		{
			R::Seam::SetSaveHold(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldSaveCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("parked save: V's save is queued and held"), R::Seam::HeldSaveCount(Gpu), 1);
		bOk &= TestEqual(TEXT("parked save: no file write issued"), R::Stats(Gpu).DiskWritesIssued - S0.DiskWritesIssued, int64(0));
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, TEXT("parked save")))
		{
			R::Seam::SetSaveHold(Gpu, false);
			return false;
		}
		R::Ticks(Gpu, 20);
		bOk &= TestEqual(TEXT("parked save: the delete waits for the write-back (Pending)"), (uint8)R::OpOf(Gpu, D).Result, (uint8)R::EOp::Pending);
		bOk &= TestFalse(TEXT("parked save: no file yet"), R::HasFile(Gpu, S));
		R::Seam::SetSaveHold(Gpu, false);
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D);
		bOk &= TestEqual(TEXT("parked save: the memory part (DropDeferred: V leased)"), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
		bOk &= TestEqual(TEXT("parked save: the delete ran after the save's write (Deleted)"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("parked save: the save's file write ran"), R::Stats(Gpu).DiskWritesIssued - S0.DiskWritesIssued, int64(1));
		bOk &= TestFalse(TEXT("parked save: no file under the key"), R::HasFile(Gpu, S));
		bOk &= TestTrue(TEXT("parked save: the per-key overlap counter never read 2"), R::Seam::MaxKeyDiskOverlap(Gpu) < 2);
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
	}

	// (vi) The request's build was refused PoolExhausted and is waiting to retry: SetNextCacheBuildStatus makes its begin
	// read SSLM_GPU_KV_POOL_EXHAUSTED (nothing drawn), evict-and-retry evicts the LRU unpinned entry (an earlier leg's),
	// and that eviction's release is held at the batch gate, so the build waits on it. The drop reads DropDeferred and
	// its disk part runs; the gate opened, the retry builds and serves (Recomputed), no write-back is issued for it and
	// no file exists (the host's disk_drop_of_a_build_waiting_to_retry). Mutant: a retry wait not counted in flight ->
	// a file -> red.
	{
		TArray<int32> WT;
		FSuperSLMPrefix H;
		if (!R::PageTokens(*this, Rg, 1, WT))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(WT);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		bOk &= TestTrue(TEXT("retry: an unpinned entry is resident to evict"), S0.ResidentEntries > 0);
		R::Seam::HoldReleases(Gpu, true);
		R::Seam::SetNextCacheBuildStatus(Gpu, int32(SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
		if (!R::Request(*this, Gpu, S, H, TEXT("retry: W")))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldReleaseCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("retry: the build was refused and evicted one entry, its release held"), R::Seam::HeldReleaseCount(Gpu), 1);
		bOk &= TestEqual(TEXT("retry: W waits to retry (Pending)"), (uint8)Gpu.GetPrefixLeaseState(H), (uint8)R::ELease::Pending);
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, TEXT("retry")))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D, R::OpSeconds);
		bOk &= TestEqual(TEXT("retry: the drop marks the retry (DropDeferred)"), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
		bOk &= TestTrue(TEXT("retry: its disk part ran (Deleted or NoFile)"), Res.Disk == R::EDisk::Deleted || Res.Disk == R::EDisk::NoFile);
		R::Seam::HoldReleases(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("retry: the retry serves"));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, TEXT("retry: W"));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("retry: no write-back issued for it"), R::Stats(Gpu).DiskWritesIssued - S0.DiskWritesIssued, int64(0));
		bOk &= TestFalse(TEXT("retry: no file under the key"), R::HasFile(Gpu, S));
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
	}
	bOk &= TestTrue(TEXT("the exclusion counter never read 2"), R::Seam::MaxDiskExclusion(Gpu) < 2);
	bOk &= TestTrue(TEXT("the per-key overlap counter never read 2"), R::Seam::MaxKeyDiskOverlap(Gpu) < 2);

	// (iii) A delete held at a re-Configure() (rev 1.18.1; rev 1.18.5's product path): the teardown releases the test
	// holds, drains the delete under the old configuration, then abandons the core. The file is gone when Configure()
	// returns, exactly one delete ran (DropDeletesRunAtLastTeardown), none re-runs under the new configuration, and the op
	// reads Dropped / Deleted after (the retired cache answers it; the host's disk_delete_held_at_reconfigure_runs_once).
	// Last, as it ends the configuration the counters above read. Mutant: the delete re-run after Configure() -> two
	// deletes counted -> red.
	{
		TArray<int32> ET;
		FSuperSLMPrefix L;
		if (!R::Root(*this, Rg, 1, false, L, ET, TEXT("held at re-Configure(): E")))
		{
			return false;
		}
		const FSuperSLMPrefixSpec E = R::Spec(ET);
		const FString Path = R::Seam::DiskFileOf(Gpu, E);
		if (!R::WaitFile(*this, Gpu, E, true, TEXT("held at re-Configure(): E's write-back")))
		{
			return false;
		}
		R::Seam::HoldDropDeletes(Gpu, true);
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, E, R::EScope::MemoryAndDisk, D, TEXT("held at re-Configure()")))
		{
			R::Seam::HoldDropDeletes(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Seam::HeldDropDeleteCount(Gpu) == 1; }, R::OpSeconds);
		bOk &= TestEqual(TEXT("held at re-Configure(): the delete is held running"), R::Seam::HeldDropDeleteCount(Gpu), 1);
		const int64 Run0 = R::Seam::DropDeletesRun(Gpu);
		bOk &= R::Reconfigure(*this, Rg);
		bOk &= TestFalse(*FString::Printf(TEXT("held at re-Configure(): the file is gone when Configure() returns (%s)"), *Path), FPaths::FileExists(Path));
		bOk &= TestEqual(TEXT("held at re-Configure(): one delete ran, in the teardown"), R::Seam::DropDeletesRunAtLastTeardown(Gpu) - Run0, int64(1));
		R::Ticks(Gpu, 20);
		bOk &= TestEqual(TEXT("held at re-Configure(): no delete re-run under the new configuration"), R::Seam::DropDeletesRun(Gpu), int64(0));
		bool bFound = false;
		const FSuperSLMPrefixOpResult Res = R::OpOf(Gpu, D, &bFound);
		bOk &= TestTrue(TEXT("held at re-Configure(): the op is still readable"), bFound);
		bOk &= TestEqual(TEXT("held at re-Configure(): the op reads Dropped"), (uint8)Res.Result, (uint8)R::EOp::Dropped);
		bOk &= TestEqual(TEXT("held at re-Configure(): its disk part Deleted"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
	}
	return bOk;
#endif
}

// Cell 3.10 (iv) (rev 1.18.2, the strike's s3) and its attach (the host's disk_drop_of_a_waiter_writes_no_file and
// disk_attach_to_a_dropped_waiter_restores_write_back), with the disk tier on and region C exactly 6 behind a pad (the
// host's FWaiterScene at A-EX, as 7.19's UnclaimedScene builds it): E (3 pages, a build of 48) resident and unpinned, L
// (2) leased, one page free, releases gated. A request for S (charged 4) evicts E and waits on its refund: a waiter
// under deferred-refund admission (§5.11 (a)), drawing nothing, no build queued. DropPrefix(S, MemoryAndDisk) at that
// apply pass reads DropDeferred and its disk part runs; no build is issued for the waiter by the drop. Then:
// - the waiter alone: the gate opened, S is admitted and served (Recomputed), no write-back is issued, no file;
// - with a request that attaches after the drop: both are served, the attach clears the mark, and the write-back is
//   restored: the file exists after it completes, written after the delete (which ran at the drop).
// Each run builds its own E and S, L leased throughout.
// Mutants: the waiter's write-back not suppressed -> a file -> red; the attach leaves write-back suppressed -> no file
// -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyDropDiskPartWaiterTest, "SuperSLM.PrefixCache.Gpu.Residency.DropDiskPartWaiter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyDropDiskPartWaiterTest::RunTest(const FString& Parameters)
{
#if !SUPERSLM_WITH_GPU_PREFIX_SAVE
	AddError(TEXT("needs the GPU prefix save and restore (SUPERSLM_WITH_GPU_PREFIX_SAVE): the disk tier's write-backs"));
	return false;
#else
	namespace R = SuperSLMGpuResidencyCells;
	R::FScopedCacheDirectory Dir(TEXT("gpu-residency-drop-waiter"));
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 6, Dir.Path))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	FSuperSLMPrefix L;
	TArray<int32> LT;
	bool bOk = true;
	for (const bool bAttach : {false, true})
	{
		const TCHAR* Name = bAttach ? TEXT("waiter, attached") : TEXT("waiter");
		FSuperSLMPrefix E, H, H2;
		TArray<int32> ET, ST;
		if (!R::Root(*this, Rg, 3, false, E, ET, *FString::Printf(TEXT("%s: E"), Name)))
		{
			return false;
		}
		if (!L.IsValid() && !R::Root(*this, Rg, 2, true, L, LT, TEXT("L")))
		{
			return false;
		}
		if (!R::NeedTokens(*this, Rg, 4, ST))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(ST);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: E 3 and L 2 charged, one page of C free"), Name), R::ChargedPages(Rg), int64(5));
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		R::Seam::HoldReleases(Gpu, true);
		if (!R::Request(*this, Gpu, S, H, Name))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		R::Pump(Gpu, [&Gpu]() { return R::Stats(Gpu).Waiters == 1; }, R::OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: S waits on E's refund (deferred-refund admission)"), Name), R::Stats(Gpu).Waiters, 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: S evicted E, its release held"), Name), R::Seam::HeldReleaseCount(Gpu), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: no build queued for the waiter"), Name), R::Stats(Gpu).BuildsIssued - S0.BuildsIssued, int64(0));
		FSuperSLMPrefixOp D;
		if (!R::Drop(*this, Gpu, S, R::EScope::MemoryAndDisk, D, Name))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D, R::OpSeconds);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the drop marks the waiter (DropDeferred)"), Name), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: its disk part ran (Deleted or NoFile)"), Name), Res.Disk == R::EDisk::Deleted || Res.Disk == R::EDisk::NoFile);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: still no build"), Name), R::Stats(Gpu).BuildsIssued - S0.BuildsIssued, int64(0));
		if (bAttach)
		{
			bOk &= R::Request(*this, Gpu, S, H2, TEXT("waiter, attached: S after the drop"));
			R::SettleIo(Gpu);
			bOk &= TestEqual(TEXT("waiter, attached: the attach joins the waiter"), R::Stats(Gpu).Waiters, 1);
		}
		R::Seam::HoldReleases(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, *FString::Printf(TEXT("%s: S, admitted"), Name));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, Name);
		if (bAttach)
		{
			bOk &= R::Expect(*this, Gpu, H2, R::ELease::Ready, TEXT("waiter, attached: the attached request"));
		}
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: one build"), Name), R::Stats(Gpu).BuildsIssued - S0.BuildsIssued, int64(1));
		if (bAttach)
		{
			bOk &= TestEqual(TEXT("waiter, attached: the attach cleared the mark"), R::Stats(Gpu).MarksCleared - S0.MarksCleared, int64(1));
			bOk &= R::WaitFile(*this, Gpu, S, true, TEXT("waiter, attached: the write-back restored, after the delete"));
		}
		else
		{
			bOk &= TestEqual(TEXT("waiter: no write-back issued for it"), R::Stats(Gpu).DiskWritesIssued - S0.DiskWritesIssued, int64(0));
			bOk &= TestFalse(TEXT("waiter: no file under the key"), R::HasFile(Gpu, S));
		}
		Gpu.ReturnPrefix(H);
		if (bAttach)
		{
			Gpu.ReturnPrefix(H2);
		}
		bOk &= R::Quiet(Gpu);
		bOk &= R::Physical(*this, Gpu, *FString::Printf(TEXT("%s: the end"), Name));
	}
	Gpu.ReturnPrefix(L);
	bOk &= R::Quiet(Gpu);
	return bOk;
#endif
}

// Cell 3.10 (iii)'s host-end runs (rev 1.18.5; U2 code review findings 1 and 2): the GPU twins of
// Cpu.U2Review.DeleteBehindSave.{DiskTierOff, OtherDirectory, Deinitialize}, through the GPU subsystem's own
// re-Configure() and Deinitialize() (BeforePoolDestroy: the drain, the held save delivered Skipped, then
// ReleaseForPoolDestroy's step 1a drain and AbandonCore()). See SuperSLMGpuResidencyCells::DeleteBehindSave.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuU2ReviewDeleteDiskOffTest, "SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.DiskTierOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuU2ReviewDeleteDiskOffTest::RunTest(const FString& Parameters)
{
	return SuperSLMGpuResidencyCells::DeleteBehindSave(*this, SuperSLMGpuResidencyCells::EHostEnd::DiskTierOff, TEXT("gpu-u2r-disk-off"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuU2ReviewDeleteOtherDirTest, "SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.OtherDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuU2ReviewDeleteOtherDirTest::RunTest(const FString& Parameters)
{
	return SuperSLMGpuResidencyCells::DeleteBehindSave(*this, SuperSLMGpuResidencyCells::EHostEnd::OtherDirectory, TEXT("gpu-u2r-other-dir"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuU2ReviewDeleteDeinitTest, "SuperSLM.PrefixCache.Gpu.U2Review.DeleteBehindSave.Deinitialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuU2ReviewDeleteDeinitTest::RunTest(const FString& Parameters)
{
	return SuperSLMGpuResidencyCells::DeleteBehindSave(*this, SuperSLMGpuResidencyCells::EHostEnd::Deinitialize, TEXT("gpu-u2r-deinit"));
}

// Cell 3.12 (rev 1.18.1, F8/G10; §5.6 *What "later" means*): ops of one key apply in issue order. With E's hash held
// (HoldKeyHash), each run issues two ops of E, the first-issued op's hash parked before the second is issued, then
// releases both: drop then request (E unleased: Dropped, the request a miss that rebuilds; E leased: DropDeferred,
// the request a Memory hit that clears the mark, E resident after every return, nothing released); request then drop
// (DropDeferred, the request Memory, E evicted at its return); the same with WarmPrefix for the request (drop then
// warm: Dropped and one rebuild, or with E leased DropDeferred, the mark cleared and no build; warm then drop: the
// warm a hit with no pin, then Dropped).
// GPU limit: the hash-completion hold is per key (HoldKeyHash), and both ops are E's, so a run cannot release the
// second-issued op's hash before the first's. Each leg runs in the completion order equal to the issue order only;
// the reversed order (which is what catches the mutant below) is the host cell's (test_residency_order.cpp) until the
// GPU has a per-op hold.
// Mutant: ops applied in hash-completion order -> red only in the reversed order (host cell).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyIssueOrderTest, "SuperSLM.PrefixCache.Gpu.Residency.IssueOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyIssueOrderTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 0))
	{
		return false;
	}
	bool bOk = true;
	for (const R::EOrderLeg Leg : {R::EOrderLeg::DropThenRequest, R::EOrderLeg::DropThenRequestLeased, R::EOrderLeg::RequestThenDrop,
		R::EOrderLeg::DropThenWarm, R::EOrderLeg::DropThenWarmLeased, R::EOrderLeg::WarmThenDrop})
	{
		bOk &= R::OrderRun(*this, Rg, Leg);
	}
	bOk &= R::Physical(*this, Rg.Gpu(), TEXT("after the runs"));
	return bOk;
}

// Cell 3.13 (rev 1.18.2, Fracture 1; §5.6 *When a mark is applied*). E marked (DropPrefix resolved DropDeferred);
// then RequestPrefix(E) issued before the pass at which E's last pin goes, its hash held past that pass (HoldKeyHash)
// and, in a fresh run, not held. Triggers: T1 the last lease returned in that frame; T2 a queued adopt drained (its
// apply held by the paging seam); T4 a child job settled [U6]; T5 a warm's build completing (E not resident at the
// drop); T6 a request's build completing after its only lease was returned while Pending. In all 10 runs the request
// is a Memory hit or attached to T5's / T6's build, with no further build, the mark is not applied, it is cleared
// (+1), and nothing is released. Second leg (T1, T5): the request issued after that pass, held and not: E is dropped
// (marks applied +1) and the request recomputes. With an earlier unrelated hash held across the pass, E's mark waits
// for it, and a request for E issued after the pass finds the mark ahead of it. Third leg (T1): one request a frame
// for a fresh key, each hash held one frame: the mark still applies (the host cell's bound is the second frame; the
// GPU's hash latency is real, so this cell reads it within a 40-frame watchdog and reports the frame).
// Not here: T3 (a write-back's save drained) needs a hold on the GPU cache's save lane job, which the seams lack.
// Mutants: marks applied as their own stage ignoring later pending hashes -> every held run recomputes -> red; one
// trigger's path applying the mark at once -> that trigger's held run red; the wait not bounded by the pass -> the
// third leg's mark never applies -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyMarkOrderTest, "SuperSLM.PrefixCache.Gpu.Residency.MarkOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyMarkOrderTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 0))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	bool bOk = true;

	// First leg: the request issued before the pass.
	for (const R::ETrigger Trigger : {R::ETrigger::T1LeaseReturned, R::ETrigger::T2AdoptDrained, R::ETrigger::T4ChildSettled, R::ETrigger::T5WarmBuilt, R::ETrigger::T6BuiltAfterReturn})
	{
		for (const bool bHashHeld : {true, false})
		{
			const FString Name = FString::Printf(TEXT("%s, hash %s"), R::TriggerName(Trigger), bHashHeld ? TEXT("held past the pass") : TEXT("not held"));
			R::FMarkRun M;
			M.Trigger = Trigger;
			if (!R::MarkSetup(*this, Rg, M))
			{
				R::MarkCleanup(Gpu, M);
				return false;
			}
			const int64 Builds0 = R::Stats(Gpu).BuildsIssued;
			FSuperSLMPrefix H;
			R::MarkBefore(Gpu, M);
			R::Seam::HoldKeyHash(Gpu, M.E, bHashHeld);
			bOk &= R::Request(*this, Gpu, M.E, H, *Name);
			bOk &= R::MarkPass(*this, Gpu, M);
			if (bHashHeld)
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s: the mark not applied while the request's hash is pending"), *Name), R::Stats(Gpu).MarksApplied - M.Marks0, int64(0));
				R::Seam::HoldKeyHash(Gpu, M.E, false);
			}
			bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, *Name);
			const ESuperSLMPrefixSource Source = Gpu.GetPrefixSource(H);
			const bool bAttachable = Trigger == R::ETrigger::T5WarmBuilt || Trigger == R::ETrigger::T6BuiltAfterReturn;
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the request is a Memory hit, or attached to the trigger's build (source %d)"), *Name, (int32)Source),
				Source == ESuperSLMPrefixSource::Memory || (bAttachable && Source == ESuperSLMPrefixSource::Recomputed));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: no further build"), *Name), R::Stats(Gpu).BuildsIssued - Builds0, int64(0));
			const uint64 EHandle = R::Seam::EngineHandleOf(Gpu, H);
			R::SettleIo(Gpu);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the mark cleared"), *Name), R::Stats(Gpu).MarksCleared - M.Cleared0, int64(1));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the mark never applied"), *Name), R::Stats(Gpu).MarksApplied - M.Marks0, int64(0));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: E never released"), *Name), R::Seam::ReleasesOf(Gpu, EHandle), 0);
			Gpu.ReturnPrefix(H);
			bOk &= R::MarkCleanup(Gpu, M);
		}
	}

	// Second leg (T1, T5): the request issued after the pass finds the mark ahead of it.
	for (const R::ETrigger Trigger : {R::ETrigger::T1LeaseReturned, R::ETrigger::T5WarmBuilt})
	{
		for (const bool bHashHeld : {true, false})
		{
			const FString Name = FString::Printf(TEXT("second leg %s, hash %s"), R::TriggerName(Trigger), bHashHeld ? TEXT("held") : TEXT("not held"));
			R::FMarkRun M;
			M.Trigger = Trigger;
			if (!R::MarkSetup(*this, Rg, M))
			{
				R::MarkCleanup(Gpu, M);
				return false;
			}
			R::MarkBefore(Gpu, M);
			bOk &= R::MarkPass(*this, Gpu, M);
			bOk &= TestTrue(*FString::Printf(TEXT("%s: E dropped at the pass (marks applied +1)"), *Name), R::WaitMarks(Gpu, M.Marks0 + 1));
			bOk &= R::Quiet(Gpu);
			const int64 Builds0 = R::Stats(Gpu).BuildsIssued;
			FSuperSLMPrefix H;
			R::Seam::HoldKeyHash(Gpu, M.E, bHashHeld);
			bOk &= R::Request(*this, Gpu, M.E, H, *Name);
			if (bHashHeld)
			{
				R::Ticks(Gpu, 8);
				bOk &= TestEqual(*FString::Printf(TEXT("%s: the request waits on its hash"), *Name), (uint8)Gpu.GetPrefixLeaseState(H), (uint8)R::ELease::Pending);
				R::Seam::HoldKeyHash(Gpu, M.E, false);
			}
			bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, *Name);
			bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, *FString::Printf(TEXT("%s: the request recomputes"), *Name));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: one build"), *Name), R::Stats(Gpu).BuildsIssued - Builds0, int64(1));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: one mark applied"), *Name), R::Stats(Gpu).MarksApplied - M.Marks0, int64(1));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: none cleared"), *Name), R::Stats(Gpu).MarksCleared - M.Cleared0, int64(0));
			Gpu.ReturnPrefix(H);
			bOk &= R::MarkCleanup(Gpu, M);
		}
	}

	// An earlier unrelated hash holds the mark back; a request for E after the pass finds the mark ahead.
	{
		R::FMarkRun M;
		M.Trigger = R::ETrigger::T1LeaseReturned;
		TArray<int32> OT;
		if (!R::MarkSetup(*this, Rg, M) || !R::PageTokens(*this, Rg, 1, OT))
		{
			R::MarkCleanup(Gpu, M);
			return false;
		}
		const FSuperSLMPrefixSpec O = R::Spec(OT);
		FSuperSLMPrefix OL, H;
		R::Seam::HoldKeyHash(Gpu, O, true);
		bOk &= R::Request(*this, Gpu, O, OL, TEXT("earlier hash: the unrelated request"));
		R::MarkBefore(Gpu, M);
		R::SettleIo(Gpu);
		bOk &= TestEqual(TEXT("earlier hash: the mark waits for the earlier op's hash"), R::Stats(Gpu).MarksApplied - M.Marks0, int64(0));
		R::Seam::HoldKeyHash(Gpu, M.E, true);
		bOk &= R::Request(*this, Gpu, M.E, H, TEXT("earlier hash: E after the pass"));
		R::SettleIo(Gpu);
		R::Seam::HoldKeyHash(Gpu, O, false);
		bOk &= TestTrue(TEXT("earlier hash: the mark applies once the earlier hash resolves (queued ahead of E's request)"), R::WaitMarks(Gpu, M.Marks0 + 1));
		R::Seam::HoldKeyHash(Gpu, M.E, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("earlier hash: E's request"));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, TEXT("earlier hash: E's request recomputes"));
		bOk &= TestEqual(TEXT("earlier hash: no mark cleared"), R::Stats(Gpu).MarksCleared - M.Cleared0, int64(0));
		bOk &= R::Expect(*this, Gpu, OL, R::ELease::Ready, TEXT("earlier hash: the unrelated request"));
		Gpu.ReturnPrefix(H);
		Gpu.ReturnPrefix(OL);
		bOk &= R::MarkCleanup(Gpu, M);
	}

	// Third leg (T1): a request a frame, each hash held one frame.
	{
		R::FMarkRun M;
		M.Trigger = R::ETrigger::T1LeaseReturned;
		if (!R::MarkSetup(*this, Rg, M))
		{
			R::MarkCleanup(Gpu, M);
			return false;
		}
		R::MarkBefore(Gpu, M);
		TArray<FSuperSLMPrefix> Leases;
		FSuperSLMPrefixSpec Previous;
		int32 AppliedAt = -1;
		for (int32 Frame = 0; Frame < 40 && AppliedAt < 0; ++Frame)
		{
			TArray<int32> FT;
			if (!R::PageTokens(*this, Rg, 1, FT))
			{
				break;
			}
			const FSuperSLMPrefixSpec S = R::Spec(FT);
			R::Seam::HoldKeyHash(Gpu, S, true);
			bOk &= R::Request(*this, Gpu, S, Leases.AddDefaulted_GetRef(), TEXT("third leg"));
			if (Previous.Tokens.Num() > 0)
			{
				R::Seam::HoldKeyHash(Gpu, Previous, false);
			}
			Gpu.Tick(R::Step);
			if (R::Stats(Gpu).MarksApplied - M.Marks0 == 1)
			{
				AppliedAt = Frame;
			}
			Previous = S;
		}
		if (Previous.Tokens.Num() > 0)
		{
			R::Seam::HoldKeyHash(Gpu, Previous, false);
		}
		AddInfo(FString::Printf(TEXT("third leg: the mark applied at frame %d (the host cell's bound is frame 1)"), AppliedAt));
		bOk &= TestTrue(TEXT("third leg: the mark applies under a request every frame (within the watchdog)"), AppliedAt >= 0);
		for (const FSuperSLMPrefix& L : Leases)
		{
			R::Resolve(Gpu, L);
			Gpu.ReturnPrefix(L);
		}
		bOk &= R::MarkCleanup(Gpu, M);
	}
	bOk &= R::Physical(*this, Gpu, TEXT("after the runs"));
	return bOk;
}

// Cell 5.12 (rev 1.18, 1.18.1, 1.18.4): residency fallbacks, each row in 5.4's form where it has one (the named
// result; one log line carrying the reason; that reason's counter +1, every other unchanged):
// - a warm with every entry leased and a charge that cannot fit (C = 6: A 2 and B 3 leased, one free page; the warm
//   charged 3) resolves BudgetExhausted, nothing drawn (Physical, region C's charge, builds and evictions unchanged);
// - a deferred drop (B leased) raises the deferred-drop count by one and leaves every reason counter unchanged;
// - DropPrefix(MemoryAndDisk) with the disk tier off: the memory part applies (Dropped), Disk == DiskTierOff;
// - (rev 1.18.1, G9) a warm with the pool filled from outside the cache (ShortGpuPool: sequences on the subsystem's own
//   pool, outside the cache and the ledger) and nothing evictable resolves PoolExhausted, in 5.4's form;
// - (rev 1.18.5, finding 4) a warm whose build fails (SetNextCacheBuildStatus: the begin reads SSLM_GPU_PREFIX_REJECTED)
//   reads Failed, Out.Reason BuildFailed, never Cancelled, in 5.4's form;
// - with the disk tier on: a warm with an unknown SchemaName resolves UnknownSchema, no entry, no file;
//   DropPrefix(Memory) reads NotRequested and leaves the file; then DropPrefix(MemoryAndDisk) of that key, not resident
//   with a file, reads NotCached and Deleted and the file is gone; a key whose file was removed reads NoFile; with the
//   delete refused (SetDeleteRefusal) Dropped and Failed, the file stays, one DropDeleteFailed line, which names the file
//   path and carries the file API's error (finding 6; the CPU oracle Cpu.U2Review.DropDeleteFailedCarriesError);
// - drops are counted apart from evictions.
// N/A on the GPU: the nested-before-U6 refusal (the GPU cache is budget mode, nested entries on); the cooked-tier leg
// (no RegisterPrefixSet on the GPU).
// Mutants: a row logged under another reason, or its counter not raised -> red on its row form; the drop's disk part
// with the tier off not reading DiskTierOff -> red; a failed build mapped to Cancelled -> red; the delete's line
// reduced to fixed text -> red on the error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyFallbacksTest, "SuperSLM.PrefixCache.Gpu.Residency.Fallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyFallbacksTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	bool bOk = true;
	{
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 6))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
		FSuperSLMPrefix A, B;
		TArray<int32> AT, BT, XT;
		if (!R::Root(*this, Rg, 2, true, A, AT, TEXT("A")) || !R::Root(*this, Rg, 3, true, B, BT, TEXT("B")) || !R::PageTokens(*this, Rg, 2, XT))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("A 2 and B 3 leased: one page of C free"), R::ChargedPages(Rg), int64(5));
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		const int64 C0 = R::LedgerOf(Gpu).ChargedOf(R::ERegion::C);
		FSuperSLMPrefixOp W;
		int32 Lines = -1;
		{
			R::FGpuPagingLogCapture Log;
			bOk &= R::Warm(*this, Gpu, R::Spec(XT), W, TEXT("budget"));
			const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, W);
			R::SettleIo(Gpu);
			Lines = Log.CountContaining(TEXT("BudgetExhausted"));
			bOk &= TestEqual(TEXT("budget: the warm resolves BudgetExhausted"), (uint8)Res.Result, (uint8)R::EOp::BudgetExhausted);
		}
		bOk &= TestEqual(TEXT("budget: one log line carrying BudgetExhausted"), Lines, 1);
		const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
		bOk &= R::RowForm(*this, S0.Fallbacks, S1.Fallbacks, TEXT("BudgetExhausted"), TEXT("budget"));
		bOk &= TestEqual(TEXT("budget: no build"), S1.BuildsIssued - S0.BuildsIssued, int64(0));
		bOk &= TestEqual(TEXT("budget: no eviction"), S1.Evictions - S0.Evictions, int64(0));
		bOk &= TestEqual(TEXT("budget: region C's charge unchanged (nothing drawn)"), R::LedgerOf(Gpu).ChargedOf(R::ERegion::C), C0);
		bOk &= R::Physical(*this, Gpu, TEXT("budget: after the refusal"));

		// A deferred drop.
		FSuperSLMPrefixOp DB;
		bOk &= R::Drop(*this, Gpu, R::Spec(BT), R::EScope::Memory, DB, TEXT("deferred")) && R::ExpectOp(*this, Gpu, DB, R::EOp::DropDeferred, TEXT("deferred: B leased"));
		const FSuperSLMPrefixCacheStats S2 = R::Stats(Gpu);
		bOk &= TestEqual(TEXT("deferred: the deferred-drop count +1"), S2.DropsDeferred - S1.DropsDeferred, int64(1));
		bOk &= R::SameFallbacks(*this, S1.Fallbacks, S2.Fallbacks, TEXT("deferred"));
		bOk &= TestEqual(TEXT("deferred: B's lease still Ready"), (uint8)Gpu.GetPrefixLeaseState(B), (uint8)R::ELease::Ready);
		bOk &= TestEqual(TEXT("deferred: nothing released"), S2.ReleasesIssued - S1.ReleasesIssued, int64(0));

		// MemoryAndDisk with the disk tier off.
		Gpu.ReturnPrefix(A);
		bOk &= R::Quiet(Gpu);
		FSuperSLMPrefixOp DA;
		FSuperSLMPrefixOpResult Res;
		bOk &= R::Drop(*this, Gpu, R::Spec(AT), R::EScope::MemoryAndDisk, DA, TEXT("tier off")) && R::ExpectOp(*this, Gpu, DA, R::EOp::Dropped, TEXT("tier off: the memory part applies"), &Res);
		bOk &= TestEqual(TEXT("tier off: Disk == DiskTierOff"), (uint8)Res.Disk, (uint8)R::EDisk::DiskTierOff);
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("tier off: one drop, no eviction"), R::Stats(Gpu).Drops - S2.Drops, int64(1));
		bOk &= TestEqual(TEXT("tier off: drops counted apart from evictions"), R::Stats(Gpu).Evictions - S2.Evictions, int64(0));
		Gpu.ReturnPrefix(B);
		bOk &= R::Quiet(Gpu);
		bOk &= R::Physical(*this, Gpu, TEXT("tier off: the end"));

		// PoolExhausted (rev 1.18.1, G9; rev 1.18.4's row form): the pool filled from outside the cache (ShortGpuPool:
		// sequences on the subsystem's own pool, outside the cache and the ledger) and nothing evictable (A dropped, B
		// evicted at its return, the pad leased). Region C admits the warm (charged 3 of 6); its build's begin meets the
		// exhaustion, evict-and-retry finds nothing, and the warm resolves PoolExhausted with one line and that counter +1.
		// Its charge is returned. Physical is read only once the pool is restored (the holders are outside every region).
		{
			TArray<int32> PT;
			if (!R::PageTokens(*this, Rg, 2, PT))
			{
				return false;
			}
			bOk &= TestEqual(TEXT("pool: nothing resident beside the pad"), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries - 2);
			const FSuperSLMPrefixCacheStats Before = R::Stats(Gpu);
			const int64 Charged0 = R::ChargedPages(Rg);
			const int32 Free = R::Seam::ShortGpuPool(Gpu, 1);
			bOk &= TestTrue(*FString::Printf(TEXT("pool: the pool is short from outside the cache (%d free pages, at most 1)"), Free), Free >= 0 && Free <= 1);
			FSuperSLMPrefixOp PoolW;
			FSuperSLMPrefixOpResult PoolRes;
			int32 PoolLines = -1;
			{
				R::FGpuPagingLogCapture Log;
				bOk &= R::Warm(*this, Gpu, R::Spec(PT), PoolW, TEXT("pool"));
				PoolRes = R::WaitPrefixOp(Gpu, PoolW);
				R::SettleIo(Gpu);
				PoolLines = Log.CountContaining(TEXT("PrefixCache fallback PoolExhausted"));
			}
			R::Seam::RestoreGpuPool(Gpu);
			bOk &= TestEqual(TEXT("pool: the warm resolves PoolExhausted"), (uint8)PoolRes.Result, (uint8)R::EOp::PoolExhausted);
			bOk &= TestEqual(TEXT("pool: one log line carrying PoolExhausted"), PoolLines, 1);
			bOk &= R::RowForm(*this, Before.Fallbacks, R::Stats(Gpu).Fallbacks, TEXT("PoolExhausted"), TEXT("pool"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("pool: no entry"), R::Stats(Gpu).ResidentEntries, Before.ResidentEntries);
			bOk &= TestEqual(TEXT("pool: its charge returned"), R::ChargedPages(Rg), Charged0);
			bOk &= R::Physical(*this, Gpu, TEXT("pool: after the pool is restored"));
		}

		// A warm whose build fails (rev 1.18.5, U2 code review finding 4, the review's probe P3): the next cache build's
		// begin reads SSLM_GPU_PREFIX_REJECTED (SetNextCacheBuildStatus: the begin is not called, nothing is drawn). The warm
		// reads Failed with Out.Reason BuildFailed, never Cancelled; one BuildFailed line; that counter +1, every other
		// unchanged. Mutant: a failed build mapped to Cancelled -> red.
		{
			TArray<int32> FT;
			if (!R::PageTokens(*this, Rg, 2, FT))
			{
				return false;
			}
			const FSuperSLMPrefixCacheStats Before = R::Stats(Gpu);
			const int64 Charged0 = R::ChargedPages(Rg);
			FSuperSLMPrefixOp FailW;
			FSuperSLMPrefixOpResult FailRes;
			int32 FailLines = -1;
			{
				R::FGpuPagingLogCapture Log;
				R::Seam::SetNextCacheBuildStatus(Gpu, int32(SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED));
				bOk &= R::Warm(*this, Gpu, R::Spec(FT), FailW, TEXT("build fails"));
				FailRes = R::WaitPrefixOp(Gpu, FailW);
				R::SettleIo(Gpu);
				FailLines = Log.CountContaining(TEXT("PrefixCache fallback BuildFailed"));
			}
			bOk &= TestEqual(TEXT("build fails: the warm reads Failed (not Cancelled)"), (uint8)FailRes.Result, (uint8)R::EOp::Failed);
			bOk &= TestEqual(TEXT("build fails: Out.Reason is BuildFailed"), FailRes.Reason.ToString(), FString(TEXT("BuildFailed")));
			bOk &= TestEqual(TEXT("build fails: one log line carrying BuildFailed"), FailLines, 1);
			bOk &= R::RowForm(*this, Before.Fallbacks, R::Stats(Gpu).Fallbacks, TEXT("BuildFailed"), TEXT("build fails"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("build fails: no entry"), R::Stats(Gpu).ResidentEntries, Before.ResidentEntries);
			bOk &= TestEqual(TEXT("build fails: its charge returned"), R::ChargedPages(Rg), Charged0);
			bOk &= R::Physical(*this, Gpu, TEXT("build fails: the end"));
		}
	}
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
	{
		R::FScopedCacheDirectory Dir(TEXT("gpu-residency-fallbacks"));
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 0, Dir.Path))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);

		// UnknownSchema.
		{
			TArray<int32> UT;
			if (!R::PageTokens(*this, Rg, 1, UT))
			{
				return false;
			}
			const FSuperSLMPrefixSpec U = R::Spec(UT, FName(TEXT("no_such_schema")));
			FSuperSLMPrefixOp W;
			int32 Lines = -1;
			FSuperSLMPrefixOpResult Res;
			{
				R::FGpuPagingLogCapture Log;
				bOk &= R::Warm(*this, Gpu, U, W, TEXT("schema"));
				Res = R::WaitPrefixOp(Gpu, W);
				R::SettleIo(Gpu);
				Lines = Log.CountContaining(TEXT("UnknownSchema"));
			}
			bOk &= TestEqual(TEXT("schema: the warm resolves UnknownSchema"), (uint8)Res.Result, (uint8)R::EOp::UnknownSchema);
			bOk &= TestEqual(TEXT("schema: one log line carrying UnknownSchema"), Lines, 1);
			bOk &= R::RowForm(*this, S0.Fallbacks, R::Stats(Gpu).Fallbacks, TEXT("UnknownSchema"), TEXT("schema"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("schema: no entry"), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries);
			bOk &= TestFalse(TEXT("schema: no file"), R::HasFile(Gpu, U));
		}
		const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);

		// NotRequested, then the stale-file purge (NotCached, Deleted).
		{
			TArray<int32> BT;
			FSuperSLMPrefix L;
			if (!R::Root(*this, Rg, 1, false, L, BT, TEXT("purge: B")) || !R::WaitFile(*this, Gpu, R::Spec(BT), true, TEXT("purge: B's write-back")))
			{
				return false;
			}
			FSuperSLMPrefixOp M, P;
			FSuperSLMPrefixOpResult Res;
			bOk &= R::Drop(*this, Gpu, R::Spec(BT), R::EScope::Memory, M, TEXT("purge")) && R::ExpectOp(*this, Gpu, M, R::EOp::Dropped, TEXT("purge: DropPrefix(Memory)"), &Res);
			bOk &= TestEqual(TEXT("purge: DropPrefix(Memory) reads NotRequested"), (uint8)Res.Disk, (uint8)R::EDisk::NotRequested);
			bOk &= R::Quiet(Gpu);
			bOk &= TestTrue(TEXT("purge: the file stays"), R::HasFile(Gpu, R::Spec(BT)));
			bOk &= R::Drop(*this, Gpu, R::Spec(BT), R::EScope::MemoryAndDisk, P, TEXT("purge: the stale file"))
				&& R::ExpectOp(*this, Gpu, P, R::EOp::NotCached, TEXT("purge: not resident"), &Res);
			bOk &= TestEqual(TEXT("purge: the file deleted (Deleted)"), (uint8)Res.Disk, (uint8)R::EDisk::Deleted);
			bOk &= R::Quiet(Gpu);
			bOk &= TestFalse(TEXT("purge: the file is gone"), R::HasFile(Gpu, R::Spec(BT)));
		}

		// NoFile.
		{
			TArray<int32> CT;
			FSuperSLMPrefix L;
			if (!R::Root(*this, Rg, 1, false, L, CT, TEXT("no file: C")) || !R::WaitFile(*this, Gpu, R::Spec(CT), true, TEXT("no file: C's write-back")))
			{
				return false;
			}
			bOk &= TestTrue(TEXT("no file: C's file removed"), IFileManager::Get().Delete(*R::Seam::DiskFileOf(Gpu, R::Spec(CT))));
			FSuperSLMPrefixOp D;
			FSuperSLMPrefixOpResult Res;
			bOk &= R::Drop(*this, Gpu, R::Spec(CT), R::EScope::MemoryAndDisk, D, TEXT("no file")) && R::ExpectOp(*this, Gpu, D, R::EOp::Dropped, TEXT("no file"), &Res);
			bOk &= TestEqual(TEXT("no file: Disk == NoFile"), (uint8)Res.Disk, (uint8)R::EDisk::NoFile);
			bOk &= R::Quiet(Gpu);
		}

		// The refused delete: Failed, DropDeleteFailed's row form.
		{
			TArray<int32> DT;
			FSuperSLMPrefix L;
			if (!R::Root(*this, Rg, 1, false, L, DT, TEXT("refused: D")) || !R::WaitFile(*this, Gpu, R::Spec(DT), true, TEXT("refused: D's write-back")))
			{
				return false;
			}
			const FSuperSLMPrefixCacheStats Before = R::Stats(Gpu);
			const FString Path = R::Seam::DiskFileOf(Gpu, R::Spec(DT));
			// Finding 6 (§5.9 rev 1.18.4; the CPU oracle Cpu.U2Review.DropDeleteFailedCarriesError): the line carries the
			// key, the file path and the file API's error. SetDeleteRefusal holds the file open without delete sharing, so
			// the refusal is the platform's own: ERROR_SHARING_VIOLATION (32) on Windows, which the seam reports elsewhere
			// too. The line must carry its system message, or "error 32".
			TCHAR Message[512] = {};
			FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), 32);
			const FString ErrorText = FString(Message).TrimStartAndEnd();
			FSuperSLMPrefixOp D;
			FSuperSLMPrefixOpResult Res;
			int32 Lines = -1;
			int32 WithPath = -1;
			int32 WithError = -1;
			{
				R::FGpuPagingLogCapture Log;
				R::Seam::SetDeleteRefusal(Gpu, true);
				bOk &= R::Drop(*this, Gpu, R::Spec(DT), R::EScope::MemoryAndDisk, D, TEXT("refused"));
				Res = R::WaitPrefixOp(Gpu, D);
				R::Seam::SetDeleteRefusal(Gpu, false);
				R::SettleIo(Gpu);
				Lines = Log.CountContaining(TEXT("DropDeleteFailed"));
				WithPath = Log.CountContaining(*Path);
				WithError = FMath::Max(ErrorText.IsEmpty() ? 0 : Log.CountContaining(*ErrorText), Log.CountContaining(TEXT("error 32")));
			}
			bOk &= TestEqual(TEXT("refused: the memory part kept (Dropped)"), (uint8)Res.Result, (uint8)R::EOp::Dropped);
			bOk &= TestEqual(TEXT("refused: Disk == Failed"), (uint8)Res.Disk, (uint8)R::EDisk::Failed);
			bOk &= TestEqual(TEXT("refused: one log line carrying DropDeleteFailed"), Lines, 1);
			bOk &= TestEqual(*FString::Printf(TEXT("refused: the line names the file path (%s)"), *Path), WithPath, 1);
			bOk &= TestEqual(*FString::Printf(TEXT("refused: the line carries the file API's error (\"%s\", or \"error 32\")"), *ErrorText), WithError, 1);
			bOk &= R::RowForm(*this, Before.Fallbacks, R::Stats(Gpu).Fallbacks, TEXT("DropDeleteFailed"), TEXT("refused"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestTrue(TEXT("refused: the file stays"), R::HasFile(Gpu, R::Spec(DT)));
		}
		bOk &= TestEqual(TEXT("disk legs: three drops Dropped"), R::Stats(Gpu).Drops - S1.Drops, int64(3));
		bOk &= TestEqual(TEXT("disk legs: drops counted apart from evictions"), R::Stats(Gpu).Evictions - S1.Evictions, int64(0));
	}
#else
	AddInfo(TEXT("5.12's disk legs skipped: they need the GPU prefix save (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
#endif
	return bOk;
}

// Cell 7.18 (rev 1.18, 1.18.1, 1.18.2): a drop of a leased entry never invalidates a live lease.
// - E (2 pages) with two leases and one adopt of it queued (its apply held by the paging seam): DropPrefix(E) is
//   DropDeferred, nothing released (the release-counting seam); the held adopt and an adopt of lease 2 both succeed;
//   after the queued work drains and lease 1 returns, E is still resident and unreleased; at lease 2's return E is
//   evicted at that pass (marks applied +1), its refund pending (its release held) until its release applies, then
//   released exactly once. (The CPU's adopters-equal-the-twin check needs CreatePrefix, which the rig's
//   PrefixBlockCount 0 refuses; the adopts' Success and the release counts stand for it here.)
// - a drop of a key whose build is in flight is DropDeferred; the build serves its requester; the mark applies at the
//   lease's return;
// - a RequestPrefix for E between the drop and the last return clears the mark, and E stays resident; so does one
//   issued after the last return in the same frame with its hash held past the pass (3.13's T1 run);
// - (C = 6) a drop of a key whose request is a waiter reads DropDeferred; the waiter is admitted and served, and the
//   mark applies at its lease's return.
// Not here: the child-job leg [U6] of the first bullet is 3.13's T4 run (MarkOrder).
// Mutants: a drop that releases a pinned entry -> an adopt refused or the seam shows E released under a lease -> red;
// the mark not cleared by a later request -> E evicted after that request's return -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyDropLeasedTest, "SuperSLM.PrefixCache.Gpu.Residency.DropLeased",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyDropLeasedTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	bool bOk = true;
	{
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 0))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();

		// Two leases, one adopt queued.
		{
			TArray<int32> ET;
			FSuperSLMPrefix L1, L2;
			if (!R::Root(*this, Rg, 2, true, L1, ET, TEXT("E, lease 1")) || !R::Lease(*this, Rg, R::Spec(ET), true, L2, TEXT("E, lease 2")))
			{
				return false;
			}
			const uint64 EHandle = R::Seam::EngineHandleOf(Gpu, L1);
			FSuperSLMGpuSequence S1, S2;
			if (!R::Vend(*this, Gpu, S1) || !R::Vend(*this, Gpu, S2))
			{
				return false;
			}
			const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Adopt);
			const FSuperSLMLifecycleOpHandle A1 = Gpu.RequestAdoptPrefix(S1, L1);
			if (!TestTrue(TEXT("leased: lease 1's adopt queues"), A1.IsValid()) || !TestTrue(TEXT("leased: its apply is held"), R::WaitHoldReached(Gpu, Hold)))
			{
				return false;
			}
			const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
			FSuperSLMPrefixOp D;
			bOk &= R::Drop(*this, Gpu, R::Spec(ET), R::EScope::Memory, D, TEXT("leased")) && R::ExpectOp(*this, Gpu, D, R::EOp::DropDeferred, TEXT("leased: DropPrefix(E)"));
			bOk &= TestEqual(TEXT("leased: the deferred-drop count +1"), R::Stats(Gpu).DropsDeferred - S0.DropsDeferred, int64(1));
			bOk &= TestEqual(TEXT("leased: E unreleased"), R::Seam::ReleasesOf(Gpu, EHandle), 0);
			bOk &= TestEqual(TEXT("leased: lease 1 still Ready"), (uint8)Gpu.GetPrefixLeaseState(L1), (uint8)R::ELease::Ready);
			bOk &= TestEqual(TEXT("leased: lease 2 still Ready"), (uint8)Gpu.GetPrefixLeaseState(L2), (uint8)R::ELease::Ready);
			FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
			bOk &= TestEqual(TEXT("leased: the queued adopt succeeds"), (uint8)R::WaitOp(Gpu, A1), (uint8)ESuperSLMRestoreResult::Success);
			Gpu.ReleaseLifecycleOpHandle(A1);
			bOk &= R::AdoptAndWait(*this, Gpu, S2, L2);
			bOk &= R::ReturnAndDrain(Gpu, S1);
			bOk &= R::ReturnAndDrain(Gpu, S2);
			Gpu.ReturnPrefix(L1);
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("leased: lease 1 returned, E still resident"), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries);
			bOk &= TestEqual(TEXT("leased: no mark applied yet"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
			bOk &= TestEqual(TEXT("leased: E still unreleased"), R::Seam::ReleasesOf(Gpu, EHandle), 0);
			R::Seam::HoldReleases(Gpu, true);
			Gpu.ReturnPrefix(L2);
			R::Ticks(Gpu);
			R::SettleIo(Gpu);
			bOk &= TestEqual(TEXT("leased: at lease 2's return E is evicted (marks applied +1)"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(1));
			bOk &= TestEqual(TEXT("leased: E out of the map"), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries - 1);
			bOk &= TestEqual(TEXT("leased: its release held"), R::Seam::HeldReleaseCount(Gpu), 1);
			bOk &= TestEqual(TEXT("leased: its refund pending until its release applies"), R::Stats(Gpu).PendingRefundBytes, 2 * Rg.Page());
			R::Seam::HoldReleases(Gpu, false);
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("leased: the refund applied"), R::Stats(Gpu).PendingRefundBytes, int64(0));
			bOk &= TestEqual(TEXT("leased: E released exactly once"), R::Seam::ReleasesOf(Gpu, EHandle), 1);
			bOk &= R::Physical(*this, Gpu, TEXT("leased: the end"));
		}

		// A drop during a build.
		{
			TArray<int32> FT;
			FSuperSLMPrefix H;
			if (!R::PageTokens(*this, Rg, 1, FT))
			{
				return false;
			}
			const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
			FSuperSLMPrefixOp D;
			R::Seam::SetBuildHold(Gpu, true);
			if (!R::Request(*this, Gpu, R::Spec(FT), H, TEXT("build: F")) || !R::WaitHeldBuild(*this, Gpu, TEXT("build: F"))
				|| !R::Drop(*this, Gpu, R::Spec(FT), R::EScope::Memory, D, TEXT("build")))
			{
				R::Seam::SetBuildHold(Gpu, false);
				return false;
			}
			const FSuperSLMPrefixOpResult Res = R::WaitPrefixOp(Gpu, D, R::OpSeconds);
			R::Seam::SetBuildHold(Gpu, false);
			bOk &= TestEqual(TEXT("build: the drop is DropDeferred"), (uint8)Res.Result, (uint8)R::EOp::DropDeferred);
			bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("build: the build serves its requester"));
			bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, TEXT("build: F"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("build: no mark applied while leased"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
			Gpu.ReturnPrefix(H);
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("build: the mark applies at the return"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(1));
			bOk &= TestEqual(TEXT("build: F gone"), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries);
		}

		// A later request clears the mark (between the drop and the last return; then in the last return's frame,
		// its hash held past the pass).
		for (int32 Leg = 0; Leg < 2; ++Leg)
		{
			const TCHAR* Name = Leg == 0 ? TEXT("clear (between)") : TEXT("clear (same frame, hash held)");
			TArray<int32> ET;
			FSuperSLMPrefix L1, L2;
			if (!R::Root(*this, Rg, 1, true, L1, ET, Name))
			{
				return false;
			}
			const FSuperSLMPrefixSpec E = R::Spec(ET);
			const uint64 EHandle = R::Seam::EngineHandleOf(Gpu, L1);
			const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
			FSuperSLMPrefixOp D;
			if (!R::Drop(*this, Gpu, E, R::EScope::Memory, D, Name) || !R::ExpectOp(*this, Gpu, D, R::EOp::DropDeferred, Name))
			{
				return false;
			}
			if (Leg == 0)
			{
				bOk &= R::Lease(*this, Rg, E, true, L2, Name);
				bOk &= R::ExpectSource(*this, Gpu, L2, ESuperSLMPrefixSource::Memory, Name);
				Gpu.ReturnPrefix(L1);
				R::Ticks(Gpu);
				Gpu.ReturnPrefix(L2);
			}
			else
			{
				Gpu.ReturnPrefix(L1);
				R::Seam::HoldKeyHash(Gpu, E, true);
				bOk &= R::Request(*this, Gpu, E, L2, Name);
				R::Ticks(Gpu);
				R::SettleIo(Gpu);
				bOk &= TestEqual(*FString::Printf(TEXT("%s: no mark applied at the pass"), Name), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
				R::Seam::HoldKeyHash(Gpu, E, false);
				bOk &= R::Expect(*this, Gpu, L2, R::ELease::Ready, Name);
				bOk &= R::ExpectSource(*this, Gpu, L2, ESuperSLMPrefixSource::Memory, Name);
				Gpu.ReturnPrefix(L2);
			}
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: E resident after every return"), Name), R::Stats(Gpu).ResidentEntries, S0.ResidentEntries);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the mark cleared"), Name), R::Stats(Gpu).MarksCleared - S0.MarksCleared, int64(1));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: no mark applied"), Name), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
			bOk &= TestEqual(*FString::Printf(TEXT("%s: E unreleased"), Name), R::Seam::ReleasesOf(Gpu, EHandle), 0);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: no build"), Name), R::Stats(Gpu).BuildsIssued - S0.BuildsIssued, int64(0));
		}
	}

	// The waiter (C = 6).
	{
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 6))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
		FSuperSLMPrefix L, H;
		TArray<int32> WT;
		if (R::UnclaimedScene(*this, Rg, L, TEXT("waiter")) == 0 || !R::NeedTokens(*this, Rg, 4, WT))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		bOk &= R::Request(*this, Gpu, R::Spec(WT), H, TEXT("waiter: W (charged 4)"));
		R::SettleIo(Gpu);
		R::Ticks(Gpu);
		bOk &= TestEqual(TEXT("waiter: W waits on E's refund"), R::Stats(Gpu).Waiters, 1);
		FSuperSLMPrefixOp D;
		bOk &= R::Drop(*this, Gpu, R::Spec(WT), R::EScope::Memory, D, TEXT("waiter")) && R::ExpectOp(*this, Gpu, D, R::EOp::DropDeferred, TEXT("waiter: DropPrefix(W)"));
		bOk &= TestEqual(TEXT("waiter: still one waiter"), R::Stats(Gpu).Waiters, 1);
		R::Seam::HoldReleases(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, TEXT("waiter: admitted at E's apply and served"));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("waiter: no mark applied while leased"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(0));
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("waiter: the mark applies at the lease's return"), R::Stats(Gpu).MarksApplied - S0.MarksApplied, int64(1));
		bOk &= R::Physical(*this, Gpu, TEXT("waiter: the end"));
		Gpu.ReturnPrefix(L);
	}
	return bOk;
}

// Cell 7.19 (rev 1.18, 1.18.1): a warm is a request that takes no lease.
// Whole block (disk on when the GPU save is built): WarmPrefix(S) on a cold cache reads Pending on the call and
// resolves Resident after one build (warms +1, an entry +1); RequestPrefix(S) after it is a Memory hit with no build.
// With the disk tier on a warm writes back as a request does (a file); and (tier order) with S dropped from memory and
// its file kept, a warm reads the file and restores once with no build, then a Memory hit. Coalescing, both orders:
// a warm and a request of one key in one tick share one build.
// C = 6: with the budget filled by warmed entries and nothing leased, requests evict them like any unleased entry
// (two evictions, no refusal); a warm of a resident key touches the LRU (no build; a request then evicts the other
// entry and the warmed one is still a Memory hit); waiting: a warm whose charge fits only by an unclaimed pending
// refund waits (a waiter, the refund claimed, no build) and is admitted at that release's apply; and a warm issued
// while a request of its key is a waiter attaches to it (one waiter, one build).
// N/A: the cooked tier leg (no RegisterPrefixSet on the GPU).
// Mutants: a warm that leases -> the last eviction refuses -> red; a warm that does not coalesce -> two builds -> red;
// a warm hit that does not touch the LRU -> the warmed entry evicted -> red; a warm that always recomputes -> a build in
// the tier-order leg -> red; the waiting warm refused -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyWarmTest, "SuperSLM.PrefixCache.Gpu.Residency.Warm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyWarmTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	bool bOk = true;
	{
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
		R::FScopedCacheDirectory Dir(TEXT("gpu-residency-warm"));
		const FString DiskDirectory = Dir.Path;
#else
		const FString DiskDirectory;
#endif
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 0, DiskDirectory))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
		TArray<int32> ST;
		if (!R::PageTokens(*this, Rg, 2, ST))
		{
			return false;
		}
		const FSuperSLMPrefixSpec S = R::Spec(ST);
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		FSuperSLMPrefixOp W;
		if (!R::Warm(*this, Gpu, S, W, TEXT("cold")))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("cold: Pending on the call (the key is hashed off the game thread)"), (uint8)R::OpOf(Gpu, W).Result, (uint8)R::EOp::Pending);
		bOk &= R::ExpectOp(*this, Gpu, W, R::EOp::Resident, TEXT("cold: WarmPrefix(S)"));
		bOk &= R::Quiet(Gpu);
		const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
		bOk &= TestEqual(TEXT("cold: one build"), S1.BuildsIssued - S0.BuildsIssued, int64(1));
		bOk &= TestEqual(TEXT("cold: one warm counted"), S1.Warms - S0.Warms, int64(1));
		bOk &= TestEqual(TEXT("cold: one entry resident"), S1.ResidentEntries - S0.ResidentEntries, 1);
		FSuperSLMPrefix H;
		bOk &= R::Lease(*this, Rg, S, true, H, TEXT("cold: S after the warm"));
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Memory, TEXT("cold: S after the warm"));
		bOk &= TestEqual(TEXT("cold: no second build"), R::Stats(Gpu).BuildsIssued, S1.BuildsIssued);
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
		bOk &= R::WaitFile(*this, Gpu, S, true, TEXT("write-back: the warm writes back"));
		bOk &= TestTrue(TEXT("write-back: a disk write issued"), R::Stats(Gpu).DiskWritesIssued > S0.DiskWritesIssued);
		{
			FSuperSLMPrefixOp D, W2;
			FSuperSLMPrefixOpResult Res;
			bOk &= R::Drop(*this, Gpu, S, R::EScope::Memory, D, TEXT("tier order")) && R::ExpectOp(*this, Gpu, D, R::EOp::Dropped, TEXT("tier order: S out of memory"), &Res);
			bOk &= R::Quiet(Gpu);
			bOk &= TestTrue(TEXT("tier order: S's file stays"), R::HasFile(Gpu, S));
			const FSuperSLMPrefixCacheStats S2 = R::Stats(Gpu);
			bOk &= R::Warm(*this, Gpu, S, W2, TEXT("tier order")) && R::ExpectOp(*this, Gpu, W2, R::EOp::Resident, TEXT("tier order: the warm reads the file"));
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("tier order: no build"), R::Stats(Gpu).BuildsIssued - S2.BuildsIssued, int64(0));
			bOk &= TestEqual(TEXT("tier order: one restore"), R::Stats(Gpu).RestoresIssued - S2.RestoresIssued, int64(1));
			FSuperSLMPrefix H2;
			bOk &= R::Lease(*this, Rg, S, false, H2, TEXT("tier order: S again"));
			bOk &= TestEqual(TEXT("tier order: then a Memory hit, no build"), R::Stats(Gpu).BuildsIssued - S2.BuildsIssued, int64(0));
		}
#else
		AddInfo(TEXT("7.19's write-back and tier-order legs skipped: they need the GPU prefix save (SUPERSLM_WITH_GPU_PREFIX_SAVE)"));
#endif
		// Coalescing, both orders.
		for (int32 Order = 0; Order < 2; ++Order)
		{
			const TCHAR* Name = Order == 0 ? TEXT("coalescing (warm then request)") : TEXT("coalescing (request then warm)");
			TArray<int32> CT;
			if (!R::PageTokens(*this, Rg, 1, CT))
			{
				return false;
			}
			const FSuperSLMPrefixCacheStats B0 = R::Stats(Gpu);
			FSuperSLMPrefixOp CW;
			FSuperSLMPrefix CL;
			if (Order == 0)
			{
				bOk &= R::Warm(*this, Gpu, R::Spec(CT), CW, Name) && R::Request(*this, Gpu, R::Spec(CT), CL, Name);
			}
			else
			{
				bOk &= R::Request(*this, Gpu, R::Spec(CT), CL, Name) && R::Warm(*this, Gpu, R::Spec(CT), CW, Name);
			}
			bOk &= R::ExpectOp(*this, Gpu, CW, R::EOp::Resident, Name);
			bOk &= R::Expect(*this, Gpu, CL, R::ELease::Ready, Name);
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: one build"), Name), R::Stats(Gpu).BuildsIssued - B0.BuildsIssued, int64(1));
			Gpu.ReturnPrefix(CL);
			bOk &= R::Quiet(Gpu);
		}
	}

	{
		R::FRig Rg;
		if (!R::Configure(*this, Rg, 6))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = Rg.Gpu();

		// Warmed entries are unpinned.
		TArray<int32> AT, BT, CT, DT, ET;
		FSuperSLMPrefixOp WA, WB;
		FSuperSLMPrefix LC, LD, LE, Again;
		if (!R::PageTokens(*this, Rg, 2, AT) || !R::PageTokens(*this, Rg, 2, BT) || !R::PageTokens(*this, Rg, 2, CT) || !R::PageTokens(*this, Rg, 2, DT) || !R::PageTokens(*this, Rg, 2, ET))
		{
			return false;
		}
		const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
		bOk &= R::Warm(*this, Gpu, R::Spec(AT), WA, TEXT("unpinned: A")) && R::ExpectOp(*this, Gpu, WA, R::EOp::Resident, TEXT("unpinned: A"));
		bOk &= R::Quiet(Gpu);
		bOk &= R::Warm(*this, Gpu, R::Spec(BT), WB, TEXT("unpinned: B")) && R::ExpectOp(*this, Gpu, WB, R::EOp::Resident, TEXT("unpinned: B"));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("unpinned: the two warmed entries charge 4 of C's 6"), R::ChargedPages(Rg), int64(4));
		bOk &= R::Lease(*this, Rg, R::Spec(CT), false, LC, TEXT("unpinned: C (charged 3) evicts a warmed entry"));
		bOk &= TestEqual(TEXT("unpinned: one eviction"), R::Stats(Gpu).Evictions - S0.Evictions, int64(1));
		bOk &= R::Lease(*this, Rg, R::Spec(DT), false, LD, TEXT("unpinned: D evicts the other"));
		bOk &= TestEqual(TEXT("unpinned: two evictions"), R::Stats(Gpu).Evictions - S0.Evictions, int64(2));
		bOk &= TestEqual(TEXT("unpinned: no refusal"), R::Stats(Gpu).Fallbacks.FindRef(FName(TEXT("BudgetExhausted"))), S0.Fallbacks.FindRef(FName(TEXT("BudgetExhausted"))));
		bOk &= R::Physical(*this, Gpu, TEXT("unpinned"));

		// The LRU: C (older) and D resident; a warm of C touches it; E evicts D; C is still a Memory hit.
		const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
		FSuperSLMPrefixOp WC;
		bOk &= R::Warm(*this, Gpu, R::Spec(CT), WC, TEXT("LRU: warm C")) && R::ExpectOp(*this, Gpu, WC, R::EOp::Resident, TEXT("LRU: warm C"));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("LRU: the warm of C is a hit (no build)"), R::Stats(Gpu).BuildsIssued, S1.BuildsIssued);
		bOk &= R::Lease(*this, Rg, R::Spec(ET), false, LE, TEXT("LRU: E evicts the LRU entry"));
		bOk &= TestEqual(TEXT("LRU: one eviction"), R::Stats(Gpu).Evictions - S1.Evictions, int64(1));
		bOk &= R::Lease(*this, Rg, R::Spec(CT), true, Again, TEXT("LRU: C again"));
		bOk &= R::ExpectSource(*this, Gpu, Again, ESuperSLMPrefixSource::Memory, TEXT("LRU: the warmed C survived"));
		Gpu.ReturnPrefix(Again);
		bOk &= R::Quiet(Gpu);

		// Clear C for the waiting scene.
		for (const TArray<int32>* K : {&CT, &ET})
		{
			FSuperSLMPrefixOp D;
			bOk &= R::Drop(*this, Gpu, R::Spec(*K), R::EScope::Memory, D, TEXT("clearing")) && R::WaitPrefixOp(Gpu, D).Result != R::EOp::Pending;
		}
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("C cleared for the waiting scene"), R::ChargedPages(Rg), int64(0));

		// Waiting on an unclaimed refund.
		FSuperSLMPrefix L;
		TArray<int32> WT;
		if (R::UnclaimedScene(*this, Rg, L, TEXT("waiting")) == 0 || !R::NeedTokens(*this, Rg, 4, WT))
		{
			R::Seam::HoldReleases(Gpu, false);
			return false;
		}
		const FSuperSLMPrefixCacheStats S2 = R::Stats(Gpu);
		const int64 C2 = R::LedgerOf(Gpu).ChargedOf(R::ERegion::C);
		FSuperSLMPrefixOp WW;
		bOk &= R::Warm(*this, Gpu, R::Spec(WT), WW, TEXT("waiting: W (charged 4)"));
		R::SettleIo(Gpu);
		R::Ticks(Gpu);
		bOk &= TestEqual(TEXT("waiting: the warm is Pending"), (uint8)R::OpOf(Gpu, WW).Result, (uint8)R::EOp::Pending);
		bOk &= TestEqual(TEXT("waiting: a waiter"), R::Stats(Gpu).Waiters, 1);
		bOk &= TestEqual(TEXT("waiting: E's refund claimed"), R::Stats(Gpu).ClaimedRefundBytes, 3 * Rg.Page());
		bOk &= TestEqual(TEXT("waiting: no build before E's apply"), R::Stats(Gpu).BuildsIssued, S2.BuildsIssued);
		bOk &= TestTrue(TEXT("waiting: region C charged no more than before (the free page held, nothing drawn)"), R::LedgerOf(Gpu).ChargedOf(R::ERegion::C) <= C2 + 1);
		bOk &= R::Physical(*this, Gpu, TEXT("waiting: before E's apply"));
		R::Seam::HoldReleases(Gpu, false);
		bOk &= R::ExpectOp(*this, Gpu, WW, R::EOp::Resident, TEXT("waiting: admitted at E's apply"));
		bOk &= R::Quiet(Gpu);
		bOk &= R::Physical(*this, Gpu, TEXT("waiting: after"));

		// A warm attaches to a waiting request: W (3, warmed) resident, L (2) leased, one page free.
		TArray<int32> Y2T, XT;
		FSuperSLMPrefix Y2, XL;
		if (!R::NeedTokens(*this, Rg, 5, Y2T) || !R::NeedTokens(*this, Rg, 4, XT))
		{
			return false;
		}
		R::Seam::HoldReleases(Gpu, true);
		bOk &= R::Request(*this, Gpu, R::Spec(Y2T), Y2, TEXT("attach: Y2")) && R::Expect(*this, Gpu, Y2, R::ELease::BudgetExhausted, TEXT("attach: Y2 evicts W, refused"));
		R::SettleIo(Gpu);
		const FSuperSLMPrefixCacheStats S3 = R::Stats(Gpu);
		bOk &= R::Request(*this, Gpu, R::Spec(XT), XL, TEXT("attach: X"));
		R::SettleIo(Gpu);
		R::Ticks(Gpu);
		bOk &= TestEqual(TEXT("attach: X waits"), R::Stats(Gpu).Waiters, 1);
		FSuperSLMPrefixOp WX;
		bOk &= R::Warm(*this, Gpu, R::Spec(XT), WX, TEXT("attach: warm X"));
		R::SettleIo(Gpu);
		R::Ticks(Gpu);
		bOk &= TestEqual(TEXT("attach: the warm attaches (still one waiter)"), R::Stats(Gpu).Waiters, 1);
		bOk &= TestEqual(TEXT("attach: the warm is Pending"), (uint8)R::OpOf(Gpu, WX).Result, (uint8)R::EOp::Pending);
		R::Seam::HoldReleases(Gpu, false);
		bOk &= R::Expect(*this, Gpu, XL, R::ELease::Ready, TEXT("attach: X"));
		bOk &= R::ExpectOp(*this, Gpu, WX, R::EOp::Resident, TEXT("attach: the warm"));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(TEXT("attach: one build"), R::Stats(Gpu).BuildsIssued - S3.BuildsIssued, int64(1));
		Gpu.ReturnPrefix(XL);
		Gpu.ReturnPrefix(L);
		bOk &= R::Quiet(Gpu);
		bOk &= R::Physical(*this, Gpu, TEXT("the end"));
	}
	return bOk;
}

// Cell 8.17 (rev 1.18.1, F10/G20): a key re-created while its earlier instance is still pending. C = 5, E's release
// gated (HoldReleases) and the new instance's build held (so the reading is taken with its charge in flight):
// DropPrefix(E) then RequestPrefix(E) in one frame. Run 1, E (2) unleased beside F (2) leased, one page free: the drop
// is Dropped and the request waits on E's own refund; at the required reading the old pending k = 2 is still charged
// beside F and the waiter's free page (5 pages), one waiter, and form 1 is clean. At the old release's apply exactly
// k refunds (E released once), the waiter is admitted, and once built the new instance is Recomputed, C carries F and
// the new E (4 pages), and E is a Memory hit. Run 2, E (2) alone, three pages free: the request is admitted at once;
// the old 2 pending plus the new 3 in flight (5 pages), no waiter; after: the new E alone (2 pages).
// Not here: the [U6] variant (E a ghost parent with a cached child).
// Mutant: instances keyed by key -> the new charge overwrites the pending one -> the ledger below physical by k ->
// red on form 1 and the page readings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuResidencyKeyRecreatedTest, "SuperSLM.PrefixCache.Gpu.Residency.KeyRecreated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuResidencyKeyRecreatedTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 5))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	bool bOk = true;
	TArray<int32> FT;
	FSuperSLMPrefix F;
	for (int32 Run = 0; Run < 2; ++Run)
	{
		const bool bWaits = Run == 0;
		const TCHAR* Name = bWaits ? TEXT("run 1 (waits on E's own refund)") : TEXT("run 2 (admitted at once)");
		TArray<int32> ET;
		FSuperSLMPrefix E, H;
		if (!R::Root(*this, Rg, 2, true, E, ET, Name))
		{
			return false;
		}
		const uint64 EHandle = R::Seam::EngineHandleOf(Gpu, E);
		Gpu.ReturnPrefix(E);
		if (!R::Quiet(Gpu) || (bWaits && !R::Root(*this, Rg, 2, true, F, FT, TEXT("F"))))
		{
			return false;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the construction's charge"), Name), R::ChargedPages(Rg), int64(bWaits ? 4 : 2));
		R::Seam::HoldReleases(Gpu, true);
		R::Seam::SetBuildHold(Gpu, true);
		FSuperSLMPrefixOp D;
		bOk &= R::Drop(*this, Gpu, R::Spec(ET), R::EScope::Memory, D, Name) && R::Request(*this, Gpu, R::Spec(ET), H, Name);
		bOk &= R::ExpectOp(*this, Gpu, D, R::EOp::Dropped, Name);
		R::SettleIo(Gpu);
		if (!bWaits)
		{
			R::WaitHeldBuild(*this, Gpu, Name);
		}
		R::Ticks(Gpu);
		const FSuperSLMPrefixCacheStats S = R::Stats(Gpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the old instance's k = 2 still pending"), Name), S.PendingRefundBytes, 2 * Rg.Page());
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the charge holds the old k beside the new instance's"), Name), R::ChargedPages(Rg), int64(5));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: waiters"), Name), S.Waiters, bWaits ? 1 : 0);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the new instance's free charge (waiting or in flight)"), Name), S.WaitingBytes + S.InFlightBytes, (bWaits ? 1 : 3) * Rg.Page());
		bOk &= R::Physical(*this, Gpu, *FString::Printf(TEXT("%s: the required reading"), Name));
		R::Seam::HoldReleases(Gpu, false);
		R::Pump(Gpu, [&Gpu, EHandle]() { return R::Seam::ReleasesOf(Gpu, EHandle) >= 1; }, R::OpSeconds);
		R::SettleIo(Gpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: exactly k refunded at the old release's apply"), Name), R::Stats(Gpu).PendingRefundBytes, int64(0));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the old instance released once"), Name), R::Seam::ReleasesOf(Gpu, EHandle), 1);
		R::Seam::SetBuildHold(Gpu, false);
		bOk &= R::Expect(*this, Gpu, H, R::ELease::Ready, Name);
		bOk &= R::ExpectSource(*this, Gpu, H, ESuperSLMPrefixSource::Recomputed, *FString::Printf(TEXT("%s: the new instance"), Name));
		bOk &= R::Quiet(Gpu);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: C carries the resident entries only"), Name), R::ChargedPages(Rg), int64(bWaits ? 4 : 2));
		bOk &= R::Physical(*this, Gpu, *FString::Printf(TEXT("%s: after"), Name));
		Gpu.ReturnPrefix(H);
		bOk &= R::Quiet(Gpu);
		FSuperSLMPrefix Again;
		bOk &= R::Lease(*this, Rg, R::Spec(ET), false, Again, Name);
		bOk &= R::ExpectSource(*this, Gpu, Again, ESuperSLMPrefixSource::Memory, *FString::Printf(TEXT("%s: the new instance stays"), Name));
		if (bWaits)
		{
			// Run 2's construction: E alone. F and the new E leave C.
			Gpu.ReturnPrefix(F);
			for (const TArray<int32>* K : {&FT, &ET})
			{
				FSuperSLMPrefixOp Clear;
				bOk &= R::Drop(*this, Gpu, R::Spec(*K), R::EScope::Memory, Clear, TEXT("clearing")) && R::WaitPrefixOp(Gpu, Clear).Result == R::EOp::Dropped;
			}
			bOk &= R::Quiet(Gpu);
			bOk &= TestEqual(TEXT("C cleared for run 2"), R::ChargedPages(Rg), int64(0));
		}
	}
	return bOk;
}

// Cell 8.14, rev 1.18 (PC-16 (a)): dropping a pinned parent. The F1 construction (Gpu.Nested.WaiterPinsParent, C = 13:
// V 3, P 4 named by a lease returned after the request, Q 3, L 2 leased; W, a child of P charged 3, waits on V and
// pins P; X 6 evicts Q and is refused; releases gated): DropPrefix(P) resolves DropDeferred, and no release is queued
// for P (still two held: V and Q). With V's release alone dispatched, W is admitted and its job runs against a live
// P (P never released). P is evicted at the apply pass after W's job completes and P is unpinned (marks applied +1),
// and its family's refund rule applies as for any eviction: W is cached, so P stays a ghost of its 4 pages.
// Physical throughout.
// Mutant: the drop releases P at once -> a third held release at the drop, and W's job against a released P -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedDropPinnedParentTest, "SuperSLM.PrefixCache.Gpu.Nested.DropPinnedParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedDropPinnedParentTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 13))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	FSuperSLMPrefix V, P, Q, L, W, X;
	TArray<int32> VT, PT, QT, LT, WS, XT;
	if (!R::Root(*this, Rg, 3, false, V, VT, TEXT("V")) || !R::Root(*this, Rg, 4, true, P, PT, TEXT("P")) || !R::Root(*this, Rg, 3, false, Q, QT, TEXT("Q"))
		|| !R::Root(*this, Rg, 2, true, L, LT, TEXT("L")) || !R::PageTokens(*this, Rg, 2, WS) || !R::NeedTokens(*this, Rg, 6, XT))
	{
		return false;
	}
	const uint64 PHandle = R::Seam::EngineHandleOf(Gpu, P);
	R::Seam::HoldReleases(Gpu, true);
	FString Error;
	bool bOk = Gpu.RequestPrefix(R::ChildSpec(P, WS), W, Error) && Gpu.RequestPrefix(R::Spec(XT), X, Error);
	R::Ticks(Gpu);
	Gpu.ReturnPrefix(P);
	R::Ticks(Gpu);
	bOk &= TestEqual(TEXT("W waits"), (uint8)Gpu.GetPrefixLeaseState(W), (uint8)R::ELease::Pending);
	bOk &= R::Expect(*this, Gpu, X, R::ELease::BudgetExhausted, TEXT("X"));
	bOk &= TestEqual(TEXT("V and Q evicted, never P"), R::Seam::HeldReleaseCount(Gpu), 2);
	const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
	FSuperSLMPrefixOp D;
	bOk &= R::Drop(*this, Gpu, R::Spec(PT), R::EScope::Memory, D, TEXT("P")) && R::ExpectOp(*this, Gpu, D, R::EOp::DropDeferred, TEXT("DropPrefix(P), pinned by W"));
	R::Ticks(Gpu);
	bOk &= TestEqual(TEXT("the drop queued no release of P (still V and Q held)"), R::Seam::HeldReleaseCount(Gpu), 2);
	bOk &= R::Physical(*this, Gpu, TEXT("marked, not applied"));
	bOk &= TestTrue(TEXT("V's release (the oldest held) dispatched alone"), R::Seam::DispatchHeldRelease(Gpu, 0));
	bOk &= R::Expect(*this, Gpu, W, R::ELease::Ready, TEXT("W at V's apply"));
	bOk &= TestEqual(TEXT("W's job ran against a live P (P not released)"), R::Seam::ReleasesOf(Gpu, PHandle), 0);
	bOk &= TestTrue(TEXT("P evicted at the pass after W's job (marks applied +1)"), R::WaitMarks(Gpu, S0.MarksApplied + 1));
	R::Seam::HoldReleases(Gpu, false);
	bOk &= R::Quiet(Gpu);
	const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
	bOk &= TestEqual(TEXT("one mark applied"), S1.MarksApplied - S0.MarksApplied, int64(1));
	bOk &= TestEqual(TEXT("the family's refund rule: P a ghost of its 4 pages under the cached W"), S1.GhostBytes - S0.GhostBytes, 4 * Rg.Page());
	bOk &= TestTrue(TEXT("no handle released twice"), R::Seam::MaxReleasesOfOneHandle(Gpu) <= 1);
	bOk &= R::Physical(*this, Gpu, TEXT("after the mark"));
	Gpu.ReturnPrefix(W);
	Gpu.ReturnPrefix(L);
	return bOk;
}

// Cell 8.14, rev 1.18.1 (audit G13 (b)): dropping an unpinned parent. C = 13; P (4 full pages) with a cached child D
// (1), both returned (nothing pinned). DropPrefix(P) is Dropped; P's 4 pages stay charged as a ghost (D maps them),
// D is not released (no cascade), and Physical. P requested again is a new instance (Recomputed), and D under it is
// still a Memory hit. Admitting 1-page roots up to the budget keeps the fill probe within C (Physical at each).
// Mutants: the drop refunds P's whole charge with no ghost -> physical above the charges -> red; the drop cascades to
// D -> D released and its hit lost -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedDropUnpinnedParentTest, "SuperSLM.PrefixCache.Gpu.Nested.DropUnpinnedParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedDropUnpinnedParentTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 13))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	FSuperSLMPrefix P, D, P2, D2;
	TArray<int32> PT, DT;
	if (!R::Root(*this, Rg, 4, true, P, PT, TEXT("P")) || !R::Child(*this, Rg, P, 1, true, D, DT, TEXT("D")))
	{
		return false;
	}
	const uint64 DHandle = R::Seam::EngineHandleOf(Gpu, D);
	Gpu.ReturnPrefix(D);
	Gpu.ReturnPrefix(P);
	bool bOk = R::Quiet(Gpu);
	const FSuperSLMPrefixCacheStats S0 = R::Stats(Gpu);
	FSuperSLMPrefixOp Op;
	bOk &= R::Drop(*this, Gpu, R::Spec(PT), R::EScope::Memory, Op, TEXT("P")) && R::ExpectOp(*this, Gpu, Op, R::EOp::Dropped, TEXT("DropPrefix(P), unpinned"));
	bOk &= R::Quiet(Gpu);
	const FSuperSLMPrefixCacheStats S1 = R::Stats(Gpu);
	bOk &= TestEqual(TEXT("P's 4 pages stay charged as a ghost (D maps them)"), S1.GhostBytes - S0.GhostBytes, 4 * Rg.Page());
	bOk &= TestEqual(TEXT("no cascade: D not released"), R::Seam::ReleasesOf(Gpu, DHandle), 0);
	bOk &= R::Physical(*this, Gpu, TEXT("after the drop"));
	bOk &= R::Lease(*this, Rg, R::Spec(PT), true, P2, TEXT("P again"));
	bOk &= R::ExpectSource(*this, Gpu, P2, ESuperSLMPrefixSource::Recomputed, TEXT("P again: a new instance"));
	bOk &= R::Lease(*this, Rg, R::ChildSpec(P2, DT), true, D2, TEXT("D under the new P"));
	bOk &= R::ExpectSource(*this, Gpu, D2, ESuperSLMPrefixSource::Memory, TEXT("D: still a Memory hit (no cascade)"));
	bOk &= R::Physical(*this, Gpu, TEXT("P rebuilt beside its ghost"));
	Gpu.ReturnPrefix(D2);
	Gpu.ReturnPrefix(P2);
	bOk &= R::Quiet(Gpu);
	for (int32 I = 0; I < 8; ++I)
	{
		FSuperSLMPrefix Y;
		TArray<int32> YT;
		bOk &= R::Root(*this, Rg, 1, false, Y, YT, TEXT("an admission up to the budget"));
		bOk &= R::Physical(*this, Gpu, *FString::Printf(TEXT("admission %d"), I));
	}
	return bOk;
}

// Cell 8.14, rev 1.18.1 (audit G13 (c)): a nested warm pins its parent. The F1 construction (C = 13) with WarmPrefix
// as the nested waiter W: W's op waits on V and pins P; X 6 evicts Q and is refused, never P (two held releases); at
// V's apply the warm's job runs against a live P (P never released) and the op resolves Resident. Physical.
// Mutant: a warm waiter that does not pin its parent -> X evicts P too (three held releases) and W's job starts
// against a released P -> red on the release-counting seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuNestedWarmPinsParentTest, "SuperSLM.PrefixCache.Gpu.Nested.WarmPinsParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuNestedWarmPinsParentTest::RunTest(const FString& Parameters)
{
	namespace R = SuperSLMGpuResidencyCells;
	R::FRig Rg;
	if (!R::Configure(*this, Rg, 13))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = Rg.Gpu();
	FSuperSLMPrefix V, P, Q, L, X;
	TArray<int32> VT, PT, QT, LT, WS, XT;
	if (!R::Root(*this, Rg, 3, false, V, VT, TEXT("V")) || !R::Root(*this, Rg, 4, true, P, PT, TEXT("P")) || !R::Root(*this, Rg, 3, false, Q, QT, TEXT("Q"))
		|| !R::Root(*this, Rg, 2, true, L, LT, TEXT("L")) || !R::PageTokens(*this, Rg, 2, WS) || !R::NeedTokens(*this, Rg, 6, XT))
	{
		return false;
	}
	const uint64 PHandle = R::Seam::EngineHandleOf(Gpu, P);
	R::Seam::HoldReleases(Gpu, true);
	FSuperSLMPrefixOp W;
	FString Error;
	bool bOk = R::Warm(*this, Gpu, R::ChildSpec(P, WS), W, TEXT("the nested warm W")) && Gpu.RequestPrefix(R::Spec(XT), X, Error);
	R::Ticks(Gpu);
	Gpu.ReturnPrefix(P);
	R::Ticks(Gpu);
	bOk &= TestEqual(TEXT("W's op waits"), (uint8)R::OpOf(Gpu, W).Result, (uint8)R::EOp::Pending);
	bOk &= R::Expect(*this, Gpu, X, R::ELease::BudgetExhausted, TEXT("X"));
	bOk &= TestEqual(TEXT("V and Q evicted, never P"), R::Seam::HeldReleaseCount(Gpu), 2);
	bOk &= R::Physical(*this, Gpu, TEXT("decided, not applied"));
	R::Seam::HoldReleases(Gpu, false);
	bOk &= R::ExpectOp(*this, Gpu, W, R::EOp::Resident, TEXT("W at V's apply"));
	bOk &= TestEqual(TEXT("P never released"), R::Seam::ReleasesOf(Gpu, PHandle), 0);
	bOk &= R::Quiet(Gpu);
	bOk &= R::Physical(*this, Gpu, TEXT("W built"));
	Gpu.ReturnPrefix(L);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
