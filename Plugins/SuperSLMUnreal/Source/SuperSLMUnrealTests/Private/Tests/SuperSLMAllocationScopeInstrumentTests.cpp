// T-2448 -- pins for the T-2447 instrument repair. The suite's remedy-pins
// record is the design record; the build record
// is the round these cells pin.
//
// FSuperSLMAllocationScope is the oracle every §9 dim 9(d)/9(e) cell decided on: MM-1 answered
// "was there a heap copy" from HasAllocationAtLeast(), and MM-5 answers "how much did the load
// actually allocate" from PeakNetAllocatedBytes(). It shipped contaminated once already -- its
// own bookkeeping raised the number MM-1 read (D-SLM5345) -- and the defect survived because
// nothing outside the instrument's own author had ever fired it. An instrument's readings are
// quarantined until someone other than its author has shown it both accepts a healthy input and
// rejects an unhealthy one. These cells are that showing for two of
// the three properties T-2447's repair introduced; the contamination-guard showing (below) is
// removed under D-SLM7220's filter (D-SLM7305) -- its subject, MM-1, is gone, and no retained
// product cell reads the oracle it exercised.
//
// Every cell here runs in BOTH contexts, for the reason §9's M7 re-cross dimension 3 gives for
// the proxy's own oracle cell: an instrument commissioned only where it is not used is not
// commissioned, and the packaged Development client is where these readings are taken.
//
// This file reads NO file from the plugin's Source/ tree (RULING D-SLM5342) -- every fixture it
// needs is an allocation it makes itself, which is what lets it be hosted here at all.
//
// ---------------------------------------------------------------------------------------
// The cells, and the mutation each is authored to be red under.
//
//  1. ArmResetsEveryCounter
//     Mutation: remove the counter/map reset from FSuperSLMAllocationScopeProxy::Arm().
//
//  2. SelfAdoptionRefusedStateIsReachable
//     Mutation: remove `UE::Private::GMalloc = &Proxy;` from FSuperSLMAllocationScope's
//     constructor. This cell is also the reachability half of the self-adoption guard's
//     commissioning: the guard is a checkf, which terminates the process rather than failing a
//     cell, so no automation test can watch it fire. What a cell CAN establish is that the state
//     it refuses is genuinely produced by the real path. Its other half -- that the constructor
//     still MAKES the comparison, and still makes it before arming -- is
//     ci/tests/test_packaged_module_guards.py.
//
// InstrumentDoesNotContaminateItsOwnOracle and DegenerateArtifactSizeDefeatsTheOracle -- the
// commissioning showings for the reentrancy guard and for MM-1's strict-positivity gate -- are
// REMOVED at T-2802 (O5, D-SLM7305): both exercised only HasAllocationAtLeast(), which no
// retained product cell reads after U0 (MM-5 reads PeakNetAllocatedBytes()), and their subject,
// MM-1, was itself removed at T-2788 U0 (fold record §4.1 dim 9(d), D-SLM7221).
// ---------------------------------------------------------------------------------------

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMAllocationScope.h"
#include "HAL/MemoryBase.h"
#include "HAL/UnrealMemory.h"

namespace
{
	// Window 1 allocates this much and leaves it live across the window boundary; window 2
	// allocates the smaller block. The gap between them is what a leaked counter shows up in.
	constexpr SIZE_T FirstWindowBlockBytes = 8u * 1024u * 1024u;
	constexpr SIZE_T SecondWindowBlockBytes = 1024u * 1024u;
}

// ---------------------------------------------------------------------------------------
// 1. Arming resets the counters, so readings cannot leak between observation windows.
// ---------------------------------------------------------------------------------------
//
// The proxy is now a single process-lifetime object rather than one object per test frame
// (D-SLM5345 defect 1), which means every window after the first inherits whatever the previous
// one left in the counters unless arming clears them. Three separate readings are asserted
// because the reset covers three separate pieces of state and a mutation can drop one:
//
//   (a) the largest-single-allocation maximum -- window 2 must not see window 1's 8 MB;
//   (b) the net high-water mark -- window 2's peak must reflect window 2's own traffic;
//   (c) the live-pointer map -- window 2 frees a pointer window 1 recorded, and if that entry
//       survives the free subtracts 8 MB from a net that never held it, driving the peak
//       NEGATIVE and suppressing (b) below the block window 2 genuinely allocated.
//
// (c)'s own mutation needs both `LiveSizes.Reset()` in Arm() and `LiveSizes.Empty()` in Disarm()
// removed, because those two are jointly sufficient and either alone clears the map -- stated
// here rather than left for a later reader to discover that half the remedy is redundant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMAllocationScopeArmResetsEveryCounterTest,
	"SuperSLM.L2S0.MemoryMapping.AllocationScopeArmResetsEveryCounter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMAllocationScopeArmResetsEveryCounterTest::RunTest(const FString& Parameters)
{
	void* FirstWindowBlock = nullptr;

	{
		FSuperSLMAllocationScope FirstWindow;
		FirstWindowBlock = FMemory::Malloc(FirstWindowBlockBytes);
		if (!TestNotNull(TEXT("the first window's 8 MB allocation must succeed"), FirstWindowBlock))
		{
			return false;
		}
		// Must-accept: the first window really did observe an 8 MB single call, so the second
		// window's assertions below are about a reset and not about an instrument that never
		// recorded anything.
		TestTrue(TEXT("the first window must observe its own 8 MB allocation -- otherwise the "
			"second window's 'must not see it' assertions prove nothing"),
			FirstWindow.HasAllocationAtLeast(FirstWindowBlockBytes));
	}

	// Deliberately NOT freed here. It is freed INSIDE the second window below, which is what
	// exercises the live-pointer map's state across the boundary.

	bool bSecondWindowInheritedTheFirstsMaximum = false;
	int64 SecondWindowPeak = 0;

	{
		FSuperSLMAllocationScope SecondWindow;

		FMemory::Free(FirstWindowBlock);
		FirstWindowBlock = nullptr;

		void* SecondWindowBlock = FMemory::Malloc(SecondWindowBlockBytes);
		if (!TestNotNull(TEXT("the second window's 1 MB allocation must succeed"), SecondWindowBlock))
		{
			return false;
		}

		bSecondWindowInheritedTheFirstsMaximum = SecondWindow.HasAllocationAtLeast(FirstWindowBlockBytes);
		SecondWindowPeak = SecondWindow.PeakNetAllocatedBytes();

		FMemory::Free(SecondWindowBlock);
	}

	// (a)
	TestFalse(FString::Printf(TEXT("the second window must not report an allocation of at least "
		"%llu bytes -- it never made one, and the only place that value can come from is the "
		"first window's counter surviving Arm()"), static_cast<uint64>(FirstWindowBlockBytes)),
		bSecondWindowInheritedTheFirstsMaximum);

	// (c)
	TestTrue(FString::Printf(TEXT("the second window's peak net (%lld bytes) must be at least "
		"the %llu bytes it actually allocated -- a peak below that means the free of a pointer "
		"the FIRST window recorded was subtracted against this window, driving its net negative "
		"and suppressing a real allocation out of an upper-bound oracle"),
		SecondWindowPeak, static_cast<uint64>(SecondWindowBlockBytes)),
		SecondWindowPeak >= static_cast<int64>(SecondWindowBlockBytes));

	// (b)
	TestTrue(FString::Printf(TEXT("the second window's peak net (%lld bytes) must stay below the "
		"first window's %llu-byte block -- a peak at or above it is the first window's net "
		"high-water mark surviving Arm()"),
		SecondWindowPeak, static_cast<uint64>(FirstWindowBlockBytes)),
		SecondWindowPeak < static_cast<int64>(FirstWindowBlockBytes));

	return true;
}

// ---------------------------------------------------------------------------------------
// 2. The state the self-adoption guard refuses is produced by the real path.
// ---------------------------------------------------------------------------------------
//
// The handle's constructor carries `checkf(CurrentAllocator != &Proxy, ...)`: it refuses to
// adopt the proxy as its own inner allocator, which would forward to itself forever on the next
// allocation from any thread. A checkf is fatal, so no automation cell can observe it firing --
// the cell that would watch it would be terminated by it. What this cell establishes instead is
// the half a structural check cannot: that the refused state is REACHED by the real path, so the
// guard is live code rather than a defence against an impossible input.
//
// It is also the pin on the installation itself. If the constructor stopped writing the proxy
// into UE::Private::GMalloc, every dim 9(d)/9(e) reading would come from an instrument that sees
// no allocations at all -- and every "no allocation this large occurred" assertion in the
// suite would pass for the wrong reason, silently and permanently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMAllocationScopeSelfAdoptionRefusedStateIsReachableTest,
	"SuperSLM.L2S0.MemoryMapping.AllocationScopeSelfAdoptionRefusedStateIsReachable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMAllocationScopeSelfAdoptionRefusedStateIsReachableTest::RunTest(const FString& Parameters)
{
	const FMalloc* const ProxyAddress = &SuperSLMAllocationScopeProxy();

	FMalloc* InstalledOutsideAnyScope = UE::Private::GMalloc;
	TestNotEqual(TEXT("outside any scope the proxy must NOT be the installed allocator -- if it "
		"is, a previous scope failed to restore and every measurement after it is attributed to "
		"the wrong window"),
		static_cast<const FMalloc*>(InstalledOutsideAnyScope), ProxyAddress);

	{
		FSuperSLMAllocationScope AllocationScope;

		// This is the guard's own comparison, evaluated at the moment a nested construction
		// would evaluate it. True here means the guard's refusing branch is reachable; false
		// means the scope is not instrumenting anything at all.
		TestEqual(TEXT("while a scope is live the proxy IS the installed allocator -- which is "
			"both what makes the measurement real and what a second, nested construction would "
			"try to adopt as its own inner allocator"),
			static_cast<const FMalloc*>(UE::Private::GMalloc), ProxyAddress);
	}

	TestNotEqual(TEXT("after the scope closes the previous allocator must be restored -- the "
		"proxy stays alive for late callers, but it must not stay installed"),
		static_cast<const FMalloc*>(UE::Private::GMalloc), ProxyAddress);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
