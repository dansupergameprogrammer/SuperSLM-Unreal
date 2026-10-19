// Plugin paging plan, CPU red suite (step K1): budget prefixes, nested prefixes and ghosts (§3.2,
// §3.3).
//
//   SuperSLM.Paging.Cpu.Prefix.Ghosts                 cell 1.7 (form 1, four cases)     [K3]
//   SuperSLM.Paging.Cpu.Prefix.ParentNotUsable        cell 2.5                          [K3]
//   SuperSLM.Paging.Cpu.Prefix.Lengths                cell 4.3 (CPU legs)               [K2/K3]
//   SuperSLM.Paging.Cpu.Prefix.AdopterIdentity        cells 6.1 and 10.2                [K2]
//   SuperSLM.Paging.Cpu.Prefix.Nested                 cell 10.3 (CPU leg)               [K3]
//   SuperSLM.Paging.Cpu.Prefix.Shipped15B.AdopterIdentity  cell 10.2, the 1.5B CPU leg   [K2; run in K4]
//   SuperSLM.Paging.Cpu.Prefix.Shipped15B.Nested           cell 10.3, the 1.5B CPU leg   [K3; run in K4]
//
// The two Shipped15B legs carry StressFilter and are run by name on the box, beside the figures: the
// 1.5B artifact is found through SUPERSLM_PAGING_1P5B_PATH (see the fixture). Each test names its
// cell and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// The pool's drawn pages by the probe's one-state form (bisection on one state).
	bool DrawnNow(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, int64& OutDrawn)
	{
		const int64 Pool = LedgerOf(Cpu).PoolPages;
		int64 FreePages = -1;
		if (!ProbeFree(T, Cpu, Pool, FreePages))
		{
			return false;
		}
		OutDrawn = Pool - FMath::Max<int64>(FreePages, 0);
		return T.TestTrue(TEXT("the probe reads a free count >= 2 (the pool is sized for it)"), FreePages >= 2);
	}

	// Form 1 with the ghost bound (§3.9): 0 <= ledger - drawn <= one page per ghost.
	bool OvercountBounded(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant, int64* OutOvercount = nullptr)
	{
		int64 Drawn = 0;
		if (!DrawnNow(T, Cpu, Drawn))
		{
			return false;
		}
		const FSuperSLMPagedLedgerView V = LedgerOf(Cpu);
		const int64 Over = V.TotalCharged - Drawn;
		if (OutOvercount)
		{
			*OutOvercount = Over;
		}
		return T.TestTrue(*FString::Printf(TEXT("%s: 0 <= overcount %lld <= ghosts %d (ledger %lld, drawn %lld)"), Instant, Over, V.GhostCount, V.TotalCharged, Drawn),
			Over >= 0 && Over <= V.GhostCount);
	}

	bool OvercountZero(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const TCHAR* Instant)
	{
		int64 Over = -1;
		const bool bBounded = OvercountBounded(T, Cpu, Instant, &Over);
		return bBounded && T.TestEqual(*FString::Printf(TEXT("%s: no ghost and no overcount"), Instant), Over, int64(0))
			&& T.TestEqual(*FString::Printf(TEXT("%s: GhostCount 0"), Instant), LedgerOf(Cpu).GhostCount, 0);
	}
}

// Cell 1.7 (§3.2 "Sharer registry"; form 1): ghosts last until the last dependent. Four cases, each
// with a 1,000-token prefix (not page-aligned, so a ghost overcounts by exactly its freed tail page):
//   (a) a released prefix with 3 adopters, which reset one at a time;
//   (b) two begin_from children after their parent's release, released one at a time;
//   (c) two sequences restored with sharing, then reset one at a time;
//   (d) a grandchild holding its grandparent through a released middle prefix.
// At every step 0 <= overcount <= 1 per ghost; after the last dependent lets go it is 0.
// Mutant: a ghost released at its first dependent, which makes the overcount negative.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuGhostsTest, "SuperSLM.Paging.Cpu.Prefix.Ghosts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuGhostsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(4, 512, 3, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens, ChildA, ChildB, Suffix;
	if (!MakeTokens(*this, Cpu, 1000, 1, Tokens) || !MakeTokens(*this, Cpu, 100, 2, ChildA) || !MakeTokens(*this, Cpu, 200, 3, ChildB) || !MakeTokens(*this, Cpu, 4, 4, Suffix))
	{
		return false;
	}
	bool bOk = true;

	// (a) Three adopters.
	{
		FSuperSLMPrefix X;
		FSuperSLMSequence A[3];
		if (!BuildPrefix(*this, Cpu, Tokens, X))
		{
			return false;
		}
		for (FSuperSLMSequence& S : A)
		{
			if (!Vend(*this, Cpu, S) || !AdoptAndWait(*this, Cpu, S, X))
			{
				return false;
			}
		}
		bOk &= TestEqual(TEXT("(a) three dependents"), FSuperSLMPagedTestSeam::PrefixDependentCount(Cpu, X), 3);
		bOk &= OvercountZero(*this, Cpu, TEXT("(a) live, adopted"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, X);
		bOk &= TestTrue(TEXT("(a) released with adopters: a ghost"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, X));
		bOk &= OvercountBounded(*this, Cpu, TEXT("(a) released"));
		for (int32 I = 0; I < 3; ++I)
		{
			bOk &= ResetAndWait(*this, Cpu, A[I]);
			bOk &= OvercountBounded(*this, Cpu, *FString::Printf(TEXT("(a) after adopter %d resets"), I + 1));
			if (I < 2)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("(a) still a ghost after %d of 3 resets"), I + 1), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, X));
			}
		}
		bOk &= OvercountZero(*this, Cpu, TEXT("(a) after the last reset"));
		for (const FSuperSLMSequence& S : A)
		{
			Cpu.ReturnSequence(S);
		}
		DrainQuiet(Cpu);
	}

	// (b) Two begin_from children outlive their parent.
	{
		FSuperSLMPrefix Parent, C1, C2;
		if (!BuildPrefix(*this, Cpu, Tokens, Parent) || !BuildNested(*this, Cpu, Parent, ChildA, C1) || !BuildNested(*this, Cpu, Parent, ChildB, C2))
		{
			return false;
		}
		bOk &= OvercountZero(*this, Cpu, TEXT("(b) parent and children live"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Parent);
		bOk &= TestTrue(TEXT("(b) the released parent is a ghost"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, Parent));
		bOk &= OvercountBounded(*this, Cpu, TEXT("(b) parent released"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, C1);
		bOk &= TestTrue(TEXT("(b) still a ghost while the second child lives"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, Parent));
		bOk &= OvercountBounded(*this, Cpu, TEXT("(b) first child released"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, C2);
		bOk &= OvercountZero(*this, Cpu, TEXT("(b) both children released"));
	}

	// (c) Two sequences restored with sharing.
	{
		FSuperSLMPrefix X;
		TArray<uint8> Blob;
		FSuperSLMSequence R[2];
		if (!BuildPrefix(*this, Cpu, Tokens, X) || !MakeAdoptedBlob(*this, Cpu, X, Suffix, 2, Blob))
		{
			return false;
		}
		for (FSuperSLMSequence& S : R)
		{
			if (!ApplyRestore(*this, Cpu, Blob, Rig.Model, S, X))
			{
				return false;
			}
		}
		bOk &= TestEqual(TEXT("(c) both restores shared and registered"), FSuperSLMPagedTestSeam::PrefixDependentCount(Cpu, X), 2);
		bOk &= OvercountZero(*this, Cpu, TEXT("(c) live, restored with sharing"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, X);
		bOk &= OvercountBounded(*this, Cpu, TEXT("(c) released"));
		bOk &= ResetAndWait(*this, Cpu, R[0]);
		bOk &= TestTrue(TEXT("(c) still a ghost after the first reset"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, X));
		bOk &= OvercountBounded(*this, Cpu, TEXT("(c) first restored sequence reset"));
		bOk &= ResetAndWait(*this, Cpu, R[1]);
		bOk &= OvercountZero(*this, Cpu, TEXT("(c) both reset"));
		Cpu.ReturnSequence(R[0]);
		Cpu.ReturnSequence(R[1]);
		DrainQuiet(Cpu);
	}

	// (d) A grandchild holds its grandparent.
	{
		FSuperSLMPrefix Grand, Middle, Leaf;
		if (!BuildPrefix(*this, Cpu, Tokens, Grand) || !BuildNested(*this, Cpu, Grand, ChildB, Middle) || !BuildNested(*this, Cpu, Middle, ChildA, Leaf))
		{
			return false;
		}
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Grand);
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Middle);
		bOk &= TestTrue(TEXT("(d) the grandparent is a ghost"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, Grand));
		bOk &= TestTrue(TEXT("(d) the middle prefix is a ghost"), FSuperSLMPagedTestSeam::IsPrefixGhost(Cpu, Middle));
		bOk &= TestEqual(TEXT("(d) two ghosts"), LedgerOf(Cpu).GhostCount, 2);
		bOk &= OvercountBounded(*this, Cpu, TEXT("(d) both ancestors released"));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Leaf);
		bOk &= OvercountZero(*this, Cpu, TEXT("(d) the grandchild released"));
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 1.7"));
	return bOk;
}

// Cell 2.5 (§3.3): CreatePrefixFrom() with a released parent, an unfrozen one (still building), and
// a foreign one (an id this subsystem never minted) is refused ParentNotUsable, with nothing
// charged and no prefix created.
// Mutant: a parent check absent (begin_from on a released or unfrozen handle).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuParentNotUsableTest, "SuperSLM.Paging.Cpu.Prefix.ParentNotUsable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuParentNotUsableTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(1, 512, 3, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> Tokens, Child;
	if (!MakeTokens(*this, Cpu, 1000, 1, Tokens) || !MakeTokens(*this, Cpu, 50, 2, Child))
	{
		return false;
	}
	bool bOk = true;
	auto ExpectNotUsable = [&](const FSuperSLMPrefix& Parent, const TCHAR* What)
	{
		const int64 PBefore = Charged(Cpu, ERegion::P);
		const int32 LiveBefore = Cpu.GetLivePrefixCount();
		FSuperSLMPrefix Out;
		FString Error;
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: ParentNotUsable (%s)"), What, *Error), (uint8)Cpu.CreatePrefixFrom(Parent, Child, Out, &Error),
			(uint8)ESuperSLMPrefixResult::ParentNotUsable);
		bOk &= TestFalse(*FString::Printf(TEXT("%s parent: no handle"), What), Out.IsValid());
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: nothing charged"), What), Charged(Cpu, ERegion::P), PBefore);
		bOk &= TestEqual(*FString::Printf(TEXT("%s parent: no prefix created"), What), Cpu.GetLivePrefixCount(), LiveBefore);
	};

	FSuperSLMPrefix Released;
	if (!BuildPrefix(*this, Cpu, Tokens, Released) || !ReleasePrefixAndDrain(*this, Cpu, Released))
	{
		return false;
	}
	ExpectNotUsable(Released, TEXT("a released"));

	FSuperSLMPrefix Building;
	FString Error;
	if (!TestEqual(TEXT("a build is admitted"), (uint8)Cpu.CreatePrefix(Tokens, Building, &Error), (uint8)ESuperSLMPrefixResult::Success))
	{
		return false;
	}
	bOk &= TestTrue(TEXT("the build is not yet frozen"), Cpu.GetPrefixPhase(Building) != ESuperSLMPrefixPhase::Ready);
	ExpectNotUsable(Building, TEXT("an unfrozen"));
	WaitPrefixReady(*this, Cpu, Building);
	Cpu.ReleasePrefix(Building);
	DrainQuiet(Cpu);

	FSuperSLMPrefix Foreign;
	Foreign.Id = (int64(1) << 60) + 12345;
	ExpectNotUsable(Foreign, TEXT("a foreign"));
	return bOk;
}

// Cell 4.3 (the CPU's legs, B = 64): prefix lengths {0, 1, B-1, B, B+1, 1000, cap-1}. Each is
// admitted at a charge of R(max(L, 1)) on P, settles at freeze to ceil(L/B) (0 for the empty root),
// is adopted, and is generated from: the adopter's tokens equal the no-prefix twin fed the prefix
// and the suffix. Then an empty nested child on parents of length B and B+1 settles to 0 and 1
// private pages, by the probe (the drawn pages' change) and by the ledger, and form 1 holds.
// Mutants: an empty child settled to 0 (charge < drawn at the B+1 parent, form 1 fails); an empty
// root refused (budget domain [1, cap] applied to the length).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuPrefixLengthsTest, "SuperSLM.Paging.Cpu.Prefix.Lengths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuPrefixLengthsTest::RunTest(const FString& Parameters)
{
	// The twins run first, in legacy mode, where a plain holder reserves the whole cap: in budget mode
	// a plain holder's own limit is SequenceBudget, below most of these prompts.
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int64 Cap = ExpectedCap(Rig.Artifact);
	const int64 B = 64; // checked against the engine below, once budget mode has read it
	const int64 Lengths[] = { 0, 1, B - 1, B, B + 1, 1000, Cap - 1 };
	constexpr int32 NumLengths = UE_ARRAY_COUNT(Lengths);
	TArray<int32> Prefixes[NumLengths], Suffixes[NumLengths], Twins[NumLengths];
	for (int32 I = 0; I < NumLengths; ++I)
	{
		const int64 L = Lengths[I];
		const int64 Room = FMath::Min<int64>(L + 512, Cap) - L;
		FSuperSLMSequence Twin;
		if (!MakeTokens(*this, Cpu, int32(L), 5, Prefixes[I]) || !MakeTokens(*this, Cpu, int32(FMath::Clamp<int64>(Room - 2, L > 0 ? 0 : 1, 4)), 6, Suffixes[I])
			|| !Vend(*this, Cpu, Twin) || !GenerateComplete(*this, Cpu, Twin, Concat(Prefixes[I], Suffixes[I]), 1, Twins[I]))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Twin);
	}
	if (!ConfigureBudget(*this, Rig, MakeConfig(1, 512, 2, 1)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	const FCpuPagingGeometry& G = Rig.Geo;
	bool bOk = true;
	for (int32 I = 0; I < NumLengths; ++I)
	{
		const int64 L = Lengths[I];
		TArray<int32> Adopted;
		FSuperSLMPrefix Prefix;
		FString Error;
		const int64 PBefore = Charged(Cpu, ERegion::P);
		const ESuperSLMPrefixResult Result = Cpu.CreatePrefix(Prefixes[I], Prefix, &Error);
		if (!TestEqual(*FString::Printf(TEXT("L = %lld: admitted (%s)"), L, *Error), (uint8)Result, (uint8)ESuperSLMPrefixResult::Success))
		{
			bOk = false;
			continue;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("L = %lld: charged R(max(L, 1)) at admission"), L), Charged(Cpu, ERegion::P) - PBefore, G.PrefixCharge(L));
		if (!WaitPrefixReady(*this, Cpu, Prefix))
		{
			bOk = false;
			continue;
		}
		bOk &= TestEqual(*FString::Printf(TEXT("L = %lld: settles to ceil(L/B) at freeze"), L), FSuperSLMPagedTestSeam::PrefixChargedPages(Cpu, Prefix), G.OccupiedPages(L));
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("L = %lld, frozen"), L));
		FSuperSLMSequence Seq;
		bOk &= Vend(*this, Cpu, Seq) && AdoptAndWait(*this, Cpu, Seq, Prefix) && GenerateComplete(*this, Cpu, Seq, Suffixes[I], 1, Adopted);
		ReturnAndDrain(Cpu, Seq);
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Prefix);
		bOk &= TestEqual(*FString::Printf(TEXT("L = %lld: the adopter's token equals the no-prefix twin's"), L), Adopted, Twins[I]);
	}

	// The empty nested child on parents of length B and B + 1.
	for (const int64 Lp : { G.B, G.B + 1 })
	{
		TArray<int32> ParentTokens;
		FSuperSLMPrefix Parent, Child;
		if (!MakeTokens(*this, Cpu, int32(Lp), 7, ParentTokens) || !BuildPrefix(*this, Cpu, ParentTokens, Parent))
		{
			return false;
		}
		int64 DrawnBefore = 0, DrawnAfter = 0;
		if (!DrawnNow(*this, Cpu, DrawnBefore) || !BuildNested(*this, Cpu, Parent, TArray<int32>(), Child) || !DrawnNow(*this, Cpu, DrawnAfter))
		{
			return false;
		}
		const int64 Private = G.NestedPrivate(Lp, 0);
		bOk &= TestEqual(*FString::Printf(TEXT("parent %lld: the empty child's private pages are %lld"), Lp, Private), Private, Lp % G.B == 0 ? int64(0) : int64(1));
		bOk &= TestEqual(*FString::Printf(TEXT("parent %lld: the probe shows the child drew %lld"), Lp, Private), DrawnAfter - DrawnBefore, Private);
		bOk &= TestEqual(*FString::Printf(TEXT("parent %lld: the ledger's settle matches"), Lp), FSuperSLMPagedTestSeam::PrefixChargedPages(Cpu, Child), Private);
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("parent %lld, empty child frozen"), Lp));
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Child);
		bOk &= ReleasePrefixAndDrain(*this, Cpu, Parent);
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 4.3"));
	return bOk;
}

// Cells 6.1 and 10.2: a budget-mode CPU adopter emits the same tokens as a legacy adopter of the same
// prefix and as the no-prefix twin fed the prefix and the suffix. Run at A-EX (ProductFilter) and at
// the shipped 1.5B artifact (a block of 512 pages; StressFilter, run by name on the box).
// Mutant: a budget adopt that maps the shared pages at the wrong origin (or recomputes them), which
// changes the continuation.
static bool RunAdopterIdentity(FAutomationTestBase& T, EPagingArtifact Artifact)
{
	FPagingRig Rig;
	if (!Rig.Init(T, Artifact) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 1, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> PrefixTokens, Suffix, Legacy, Budget, Twin;
	if (!MakeTokens(T, Cpu, 300, 1, PrefixTokens) || !MakeTokens(T, Cpu, 8, 2, Suffix))
	{
		return false;
	}
	auto AdoptRun = [&](TArray<int32>& Out)
	{
		FSuperSLMPrefix Prefix;
		FString Error;
		FSuperSLMSequence Seq;
		const bool bCreated = Cpu.CreatePrefix(PrefixTokens, Prefix, Error);
		const bool bOk = T.TestTrue(*FString::Printf(TEXT("CreatePrefix (%s)"), *Error), bCreated) && WaitPrefixReady(T, Cpu, Prefix)
			&& Vend(T, Cpu, Seq) && AdoptAndWait(T, Cpu, Seq, Prefix) && GenerateComplete(T, Cpu, Seq, Suffix, 16, Out);
		ReturnAndDrain(Cpu, Seq);
		Cpu.ReleasePrefix(Prefix);
		DrainQuiet(Cpu);
		return bOk;
	};
	if (!AdoptRun(Legacy))
	{
		return false;
	}
	{
		FSuperSLMSequence Seq;
		if (!Vend(T, Cpu, Seq) || !GenerateComplete(T, Cpu, Seq, Concat(PrefixTokens, Suffix), 16, Twin))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Seq);
	}
	bool bOk = T.TestEqual(TEXT("the legacy adopter equals the no-prefix twin (control)"), Legacy, Twin);
	if (!ConfigureBudget(T, Rig, MakeConfig(1, 512, 1, 1)) || !AdoptRun(Budget))
	{
		return false;
	}
	bOk &= T.TestEqual(TEXT("the budget adopter equals the legacy adopter"), Budget, Legacy);
	bOk &= T.TestEqual(TEXT("the budget adopter equals the no-prefix twin"), Budget, Twin);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuAdopterIdentityTest, "SuperSLM.Paging.Cpu.Prefix.AdopterIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuAdopterIdentityTest::RunTest(const FString& Parameters)
{
	return RunAdopterIdentity(*this, EPagingArtifact::AEx05B);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuAdopterIdentity15BTest, "SuperSLM.Paging.Cpu.Prefix.Shipped15B.AdopterIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuAdopterIdentity15BTest::RunTest(const FString& Parameters)
{
	return RunAdopterIdentity(*this, EPagingArtifact::Shipped15B);
}

// Cell 10.3 (the CPU leg): nested. A child adopter's tokens equal the no-prefix twin fed parent +
// child + suffix, and the probe shows the child holding only its private pages,
// ceil((Lp + T)/B) - floor(Lp/B) (parent 1,000, child 300: 6 pages), not its whole length.
// The figures depend only on B (64 at both artifacts), so the 1.5B leg (a block of 512 pages;
// StressFilter, run by name on the box) asserts the same 6.
// Mutants: a child that copies the parent's full pages (draws 21, not 6); a child prefilled at the
// wrong origin (its adopter's tokens differ).
static bool RunNested(FAutomationTestBase& T, EPagingArtifact Artifact)
{
	// The twin first, in legacy mode (a budget-mode plain holder's limit is SequenceBudget, below
	// its 1,308-token prompt).
	FPagingRig Rig;
	if (!Rig.Init(T, Artifact) || !ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> ParentTokens, ChildTokens, Suffix, Adopted, Twin;
	FSuperSLMPrefix Parent, Child;
	FSuperSLMSequence TwinSeq;
	if (!MakeTokens(T, Cpu, 1000, 1, ParentTokens) || !MakeTokens(T, Cpu, 300, 2, ChildTokens) || !MakeTokens(T, Cpu, 8, 3, Suffix)
		|| !Vend(T, Cpu, TwinSeq) || !GenerateComplete(T, Cpu, TwinSeq, Concat(Concat(ParentTokens, ChildTokens), Suffix), 16, Twin))
	{
		return false;
	}
	ReturnAndDrain(Cpu, TwinSeq);
	if (!ConfigureBudget(T, Rig, MakeConfig(1, 512, 2, 1)) || !BuildPrefix(T, Cpu, ParentTokens, Parent))
	{
		return false;
	}
	int64 DrawnBefore = 0, DrawnAfter = 0;
	if (!DrawnNow(T, Cpu, DrawnBefore) || !BuildNested(T, Cpu, Parent, ChildTokens, Child) || !DrawnNow(T, Cpu, DrawnAfter))
	{
		return false;
	}
	const int64 Private = Rig.Geo.NestedPrivate(1000, 300);
	bool bOk = T.TestEqual(TEXT("parent 1,000 + child 300: 6 private pages"), Private, int64(6));
	bOk &= T.TestEqual(TEXT("the probe shows the child drew only its private pages"), DrawnAfter - DrawnBefore, Private);
	bOk &= T.TestEqual(TEXT("the ledger's settle matches"), FSuperSLMPagedTestSeam::PrefixChargedPages(Cpu, Child), Private);
	FSuperSLMSequence Seq;
	bOk &= Vend(T, Cpu, Seq) && AdoptAndWait(T, Cpu, Seq, Child) && GenerateComplete(T, Cpu, Seq, Suffix, 16, Adopted);
	ReturnAndDrain(Cpu, Seq);
	bOk &= T.TestEqual(TEXT("the child adopter equals the no-prefix twin fed parent + child + suffix"), Adopted, Twin);
	bOk &= NoLedgerFault(T, Cpu, TEXT("cell 10.3"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNestedTest, "SuperSLM.Paging.Cpu.Prefix.Nested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuNestedTest::RunTest(const FString& Parameters)
{
	return RunNested(*this, EPagingArtifact::AEx05B);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuNested15BTest, "SuperSLM.Paging.Cpu.Prefix.Shipped15B.Nested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingCpuNested15BTest::RunTest(const FString& Parameters)
{
	return RunNested(*this, EPagingArtifact::Shipped15B);
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
