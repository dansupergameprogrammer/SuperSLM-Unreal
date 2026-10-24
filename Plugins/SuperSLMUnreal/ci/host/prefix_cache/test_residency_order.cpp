// Issue order and mark application (precache plan rev 1.18.1-1.18.2 §5.6 *What "later" means* and *When a mark
// is applied*): cells 3.12 and 3.13, host halves, with the hash-completion hold (§8; FSim::HeldHashLeases and
// HeldHashOps, released one at a time by RunHashOfLease / RunHashOfOp, each as its own apply pass).
//
// Red first on the residency-control stubs (SuperSLMPrefixCacheCore.cpp "RED STUBS (U2 ...)"): every cell
// here issues a DropPrefix, which the stub refuses.
//
// 3.13's T4 (a child job settled) is U6's leg and is not here: nested entries are U6's.

#include "cache_fixture.h"

#include <functional>

using namespace HostSim;

namespace
{
	enum class EOrder : uint8_t
	{
		DropHashFirst,
		RequestHashFirst,
	};

	const char* OrderName(EOrder O) { return O == EOrder::DropHashFirst ? "drop's hash first" : "request's hash first"; }

	// The two ops' hashes, released in Order, then everything run.
	void ReleaseHashes(FCacheFixture& F, EOrder Order, uint64_t DropOp, int64_t Lease, uint64_t WarmOp)
	{
		auto Request = [&]() {
			if (WarmOp != 0)
			{
				return F.Sim->RunHashOfOp(WarmOp);
			}
			return F.Sim->RunHashOfLease(Lease);
		};
		if (Order == EOrder::DropHashFirst)
		{
			EXPECT_TRUE(F.Sim->RunHashOfOp(DropOp));
			EXPECT_TRUE(Request());
		}
		else
		{
			EXPECT_TRUE(Request());
			EXPECT_TRUE(F.Sim->RunHashOfOp(DropOp));
		}
		F.Sim->HeldHashOps.clear();
		F.Sim->HeldHashLeases.clear();
		F.Sim->RunAll();
	}
}

// ---- 3.12 --------------------------------------------------------------------------------------------

HOST_TEST(order_drop_issued_before_request, "3.12 [U2] (rev 1.18.1, F8/G10): DropPrefix(E) issued before RequestPrefix(E) in one frame, E resident and unleased, under both hash-completion orders: the drop is decided first (Dropped) and the request is a miss that rebuilds; with E leased by an earlier lease the drop marks, the request clears the mark and E stays resident after every return (mutant: apply in hash-completion order)")
{
	for (const bool bLeased : {false, true})
	{
		for (const EOrder Order : {EOrder::DropHashFirst, EOrder::RequestHashFirst})
		{
			FCacheFixture F(4, false);
			REQUIRE_TRUE(F.bConfigured);
			const FPrefixSpec E = FixtureSpec(1);
			int64_t L0 = 0;
			if (bLeased)
			{
				L0 = F.RequestReady(E);
				REQUIRE_NE(L0, 0);
			}
			else
			{
				REQUIRE_TRUE(F.Resident(E));
			}
			const uint64_t D = F.Drop(E);
			const int64_t H = F.Request(E);
			REQUIRE_TRUE(D != 0 && H != 0);
			F.Sim->HeldHashOps.insert(D);
			F.Sim->HeldHashLeases.insert(H);
			F.Sim->RunAll();
			ReleaseHashes(F, Order, D, H, 0);
			if (!bLeased)
			{
				EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
				EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));
				EXPECT_EQ(F.Diag().Drops, 1u);
			}
			else
			{
				EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
				EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Memory);
				EXPECT_TRUE(F.Cache.Return(L0));
				F.Sim->Pass();
				EXPECT_TRUE(F.Cache.Return(H));
				F.Sim->Pass();
				F.Sim->RunAll();
				EXPECT_EQ(F.Diag().ResidentEntries, 1u);
				EXPECT_EQ(F.Diag().MarksCleared, 1u);
				EXPECT_EQ(F.Diag().MarksApplied, 0u);
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
			}
			(void)OrderName(Order);
		}
	}
}

HOST_TEST(order_request_issued_before_drop, "3.12 [U2]: RequestPrefix(E) issued before DropPrefix(E), under both hash-completion orders: the drop finds the request's lease and marks, and E is evicted when that lease returns (mutant: apply in hash-completion order)")
{
	for (const EOrder Order : {EOrder::DropHashFirst, EOrder::RequestHashFirst})
	{
		FCacheFixture F(4, false);
		REQUIRE_TRUE(F.bConfigured);
		const FPrefixSpec E = FixtureSpec(1);
		REQUIRE_TRUE(F.Resident(E));
		const int64_t H = F.Request(E);
		const uint64_t D = F.Drop(E);
		REQUIRE_TRUE(D != 0 && H != 0);
		F.Sim->HeldHashOps.insert(D);
		F.Sim->HeldHashLeases.insert(H);
		F.Sim->RunAll();
		ReleaseHashes(F, Order, D, H, 0);
		EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
		EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Memory);
		EXPECT_EQ(F.Diag().ResidentEntries, 1u);
		EXPECT_TRUE(F.Cache.Return(H));
		F.Sim->Pass();
		EXPECT_EQ(F.Diag().MarksApplied, 1u);
		EXPECT_EQ(F.Diag().ResidentEntries, 0u);
		F.Sim->RunAll();
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(1));
		EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	}
}

HOST_TEST(order_drop_and_warm_both_issue_orders, "3.12 [U2]: the same legs with WarmPrefix for the request, under both hash-completion orders. Drop before warm: the drop is decided first (Dropped) and the warm rebuilds (Resident); with E leased the drop marks, the warm clears the mark and E stays resident. Warm before drop: the warm (a hit that takes no pin) is decided first, then the drop finds E unpinned and evicts it (mutant: apply in hash-completion order)")
{
	for (const EOrder Order : {EOrder::DropHashFirst, EOrder::RequestHashFirst})
	{
		// Drop before warm, E unleased; then E leased.
		for (const bool bLeased : {false, true})
		{
			FCacheFixture F(4, false);
			REQUIRE_TRUE(F.bConfigured);
			const FPrefixSpec E = FixtureSpec(1);
			int64_t L0 = 0;
			if (bLeased)
			{
				L0 = F.RequestReady(E);
				REQUIRE_NE(L0, 0);
			}
			else
			{
				REQUIRE_TRUE(F.Resident(E));
			}
			const uint64_t D = F.Drop(E);
			const uint64_t W = F.Warm(E);
			REQUIRE_TRUE(D != 0 && W != 0);
			F.Sim->HeldHashOps.insert(D);
			F.Sim->HeldHashOps.insert(W);
			F.Sim->RunAll();
			ReleaseHashes(F, Order, D, 0, W);
			EXPECT_EQ(F.OpResult(W), EPrefixOpResult::Resident);
			if (!bLeased)
			{
				EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));
				EXPECT_EQ(F.Diag().ResidentEntries, 1u);
			}
			else
			{
				EXPECT_EQ(F.OpResult(D), EPrefixOpResult::DropDeferred);
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
				EXPECT_TRUE(F.Cache.Return(L0));
				F.Sim->Pass();
				F.Sim->RunAll();
				EXPECT_EQ(F.Diag().ResidentEntries, 1u);
				EXPECT_EQ(F.Diag().MarksCleared, 1u);
				EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
			}
		}
		// Warm before drop, E unleased.
		{
			FCacheFixture F(4, false);
			REQUIRE_TRUE(F.bConfigured);
			const FPrefixSpec E = FixtureSpec(1);
			REQUIRE_TRUE(F.Resident(E));
			const uint64_t W = F.Warm(E);
			const uint64_t D = F.Drop(E);
			REQUIRE_TRUE(D != 0 && W != 0);
			F.Sim->HeldHashOps.insert(D);
			F.Sim->HeldHashOps.insert(W);
			F.Sim->RunAll();
			ReleaseHashes(F, Order, D, 0, W);
			EXPECT_EQ(F.OpResult(W), EPrefixOpResult::Resident);
			EXPECT_EQ(F.OpResult(D), EPrefixOpResult::Dropped);
			EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1)); // the warm was a hit
			EXPECT_EQ(F.Diag().ResidentEntries, 0u);
		}
	}
}

// ---- 3.13 --------------------------------------------------------------------------------------------

namespace
{
	enum class ETrigger : uint8_t
	{
		T1LeaseReturned,
		T2AdoptDrained,
		T3SaveDrained,
		T5WarmBuildCompleted,
		T6BuildCompletedAfterReturn,
	};

	const char* TriggerName(ETrigger T)
	{
		switch (T)
		{
		case ETrigger::T1LeaseReturned: return "T1";
		case ETrigger::T2AdoptDrained: return "T2";
		case ETrigger::T3SaveDrained: return "T3";
		case ETrigger::T5WarmBuildCompleted: return "T5";
		case ETrigger::T6BuildCompletedAfterReturn: return "T6";
		}
		return "?";
	}

	// One 3.13 run up to the frame of the pass at which E's last pin goes. E = FixtureSpec(1) is marked
	// (DropPrefix resolved DropDeferred). Before() makes the frame's game-thread calls up to that pass, and
	// ThePass() runs that pass.
	struct FTriggerRun
	{
		ETrigger Trigger;
		std::unique_ptr<FCacheFixture> F;
		FPrefixSpec E = FixtureSpec(1);
		int64_t Lease = 0;
		uint64_t AdoptToken = 0;
		uint64_t DropOp = 0;
		bool bReady = false;
		std::string Why;

		explicit FTriggerRun(ETrigger InTrigger) : Trigger(InTrigger)
		{
			F.reset(new FCacheFixture(4, Trigger == ETrigger::T3SaveDrained));
			if (!F->bConfigured)
			{
				Why = "not configured";
				return;
			}
			FSim& Sim = *F->Sim;
			switch (Trigger)
			{
			case ETrigger::T1LeaseReturned:
				Lease = F->RequestReady(E);
				break;
			case ETrigger::T2AdoptDrained:
			{
				Lease = F->RequestReady(E);
				uint64_t Engine = 0;
				if (Lease == 0 || F->Cache.BeginAdopt(Lease, Engine, AdoptToken) != EPrefixAdoptCheck::Adoptable)
				{
					Why = "E's lease did not adopt";
					return;
				}
				break;
			}
			case ETrigger::T3SaveDrained:
				Sim.Held.insert(EPrefixCommandKind::Save);
				Lease = F->RequestReady(E);
				break;
			case ETrigger::T5WarmBuildCompleted:
				Sim.Held.insert(EPrefixCommandKind::Build);
				if (F->Warm(E) == 0)
				{
					Why = "WarmPrefix(E) refused";
					return;
				}
				Sim.RunAll();
				break;
			case ETrigger::T6BuildCompletedAfterReturn:
				Sim.Held.insert(EPrefixCommandKind::Build);
				Lease = F->Request(E);
				Sim.RunAll();
				break;
			}
			DropOp = F->Drop(E);
			if (DropOp == 0)
			{
				Why = "DropPrefix(E) refused";
				return;
			}
			Sim.RunAll();
			if (F->OpResult(DropOp) != EPrefixOpResult::DropDeferred)
			{
				Why = "DropPrefix(E) did not resolve DropDeferred";
				return;
			}
			// Pins that go before the trigger's pass, each followed by its own pass (the mark still pinned).
			if (Trigger == ETrigger::T2AdoptDrained || Trigger == ETrigger::T3SaveDrained ||
				Trigger == ETrigger::T6BuildCompletedAfterReturn)
			{
				if (!F->Cache.Return(Lease))
				{
					Why = "E's lease did not return";
					return;
				}
				Sim.Pass();
			}
			Why = "the mark applied, or E's residency is not the trigger's, before the trigger's pass";
			bReady = F->Diag().MarksApplied == 0 && F->Diag().ResidentEntries == (Trigger == ETrigger::T5WarmBuildCompleted ||
				Trigger == ETrigger::T6BuildCompletedAfterReturn ? 0u : 1u);
			if (bReady)
			{
				Why.clear();
			}
		}

		// The frame's game-thread calls before its pass.
		void Before()
		{
			if (Trigger == ETrigger::T1LeaseReturned)
			{
				EXPECT_TRUE(F->Cache.Return(Lease));
			}
			else if (Trigger == ETrigger::T2AdoptDrained)
			{
				EXPECT_TRUE(F->Cache.EndAdopt(AdoptToken));
			}
		}

		// The apply pass at which E's last pin goes (or E becomes resident with no pin).
		void ThePass()
		{
			FSim& Sim = *F->Sim;
			switch (Trigger)
			{
			case ETrigger::T1LeaseReturned:
			case ETrigger::T2AdoptDrained:
				Sim.Pass();
				break;
			case ETrigger::T3SaveDrained:
				EXPECT_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::Save));
				break;
			case ETrigger::T5WarmBuildCompleted:
			case ETrigger::T6BuildCompletedAfterReturn:
				EXPECT_TRUE(Sim.RunOneOfKind(EPrefixCommandKind::Build));
				break;
			}
		}

		size_t Builds() const { return F->Sim->CountHistory(EPrefixCommandKind::Build); }
	};

	constexpr ETrigger kTriggers[] = {ETrigger::T1LeaseReturned, ETrigger::T2AdoptDrained, ETrigger::T3SaveDrained,
		ETrigger::T5WarmBuildCompleted, ETrigger::T6BuildCompletedAfterReturn};
}

HOST_TEST(order_mark_waits_for_a_request_issued_before_the_unpin, "3.13 [U2] (rev 1.18.2, Fracture 1): for each trigger T1, T2, T3, T5, T6, a RequestPrefix(E) issued before the pass at which E's last pin goes, its hash held past that pass and, in a fresh run, released before it: in all 10 runs the request is served from Memory or attached to T5's / T6's build with no further prefill, E is resident after the pass, and the marks-cleared count is up by one (mutants: marks applied as their own stage ignoring later pending hashes; one trigger's path applying the mark at once)")
{
	for (const ETrigger Trigger : kTriggers)
	{
		for (const bool bHashHeld : {true, false})
		{
			FTriggerRun R(Trigger);
			EXPECT_EQ(R.Why, std::string());
			REQUIRE_TRUE(R.bReady);
			FSim& Sim = *R.F->Sim;
			const size_t Builds = R.Builds(); // History holds every command taken, queued or run
			R.Before();
			const int64_t H = R.F->Request(R.E);
			REQUIRE_NE(H, 0);
			if (bHashHeld)
			{
				Sim.HeldHashLeases.insert(H);
				R.ThePass();
				EXPECT_EQ(R.F->Diag().MarksApplied, 0u);
				EXPECT_TRUE(Sim.RunHashOfLease(H));
			}
			else
			{
				EXPECT_TRUE(Sim.RunHashOfLease(H));
				R.ThePass();
			}
			Sim.Held.clear();
			Sim.RunAll();
			const EPrefixSource Source = R.F->Cache.GetSource(H);
			const bool bAttachable = Trigger == ETrigger::T5WarmBuildCompleted || Trigger == ETrigger::T6BuildCompletedAfterReturn;
			EXPECT_TRUE(Source == EPrefixSource::Memory || (bAttachable && Source == EPrefixSource::Recomputed));
			EXPECT_EQ(R.F->Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
			EXPECT_EQ(R.Builds(), Builds); // no further prefill
			EXPECT_EQ(R.F->Diag().ResidentEntries, 1u);
			EXPECT_EQ(R.F->Diag().MarksCleared, 1u);
			EXPECT_EQ(R.F->Diag().MarksApplied, 0u);
			EXPECT_EQ(Sim.CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
			(void)TriggerName(Trigger);
		}
	}
}

HOST_TEST(order_request_issued_after_the_unpin_finds_the_mark_ahead, "3.13 [U2] second leg (T1, T5): a RequestPrefix(E) issued after the pass that finds E unpinned, its hash held and, in a fresh run, released: E is dropped once the earlier hashes resolve (marks applied +1), and the request recomputes")
{
	for (const ETrigger Trigger : {ETrigger::T1LeaseReturned, ETrigger::T5WarmBuildCompleted})
	{
		for (const bool bHashHeld : {true, false})
		{
			FTriggerRun R(Trigger);
			EXPECT_EQ(R.Why, std::string());
			REQUIRE_TRUE(R.bReady);
			FSim& Sim = *R.F->Sim;
			R.Before();
			R.ThePass();
			EXPECT_EQ(R.F->Diag().MarksApplied, 1u);
			EXPECT_EQ(R.F->Diag().ResidentEntries, 0u);
			Sim.Held.clear();
			const size_t Builds = R.Builds(); // History holds every command taken, queued or run
			const int64_t H = R.F->Request(R.E);
			REQUIRE_NE(H, 0);
			if (bHashHeld)
			{
				Sim.HeldHashLeases.insert(H);
				Sim.RunAll();
				EXPECT_EQ(R.F->Cache.GetLeaseState(H), EPrefixLeaseState::Pending);
				EXPECT_TRUE(Sim.RunHashOfLease(H));
			}
			Sim.RunAll();
			EXPECT_EQ(R.F->Cache.GetSource(H), EPrefixSource::Recomputed);
			EXPECT_EQ(R.Builds(), Builds + 1);
			EXPECT_EQ(R.F->Diag().MarksApplied, 1u);
			EXPECT_EQ(R.F->Diag().MarksCleared, 0u);
		}
	}
}

HOST_TEST(order_mark_waits_only_for_hashes_issued_before_its_pass, "3.13 [U2] second leg with an earlier hash (§5.6: the wait is bounded by the hash latency of ops issued before that pass): an unrelated request issued before the pass with its hash held holds E's mark back; a request for E issued after the pass finds the mark ahead of it, E is dropped once the earlier hash resolves, and the request recomputes")
{
	FTriggerRun R(ETrigger::T1LeaseReturned);
	EXPECT_EQ(R.Why, std::string());
	REQUIRE_TRUE(R.bReady);
	FSim& Sim = *R.F->Sim;
	const int64_t Other = R.F->Request(FixtureSpec(2));
	REQUIRE_NE(Other, 0);
	Sim.HeldHashLeases.insert(Other);
	R.Before();
	R.ThePass();
	EXPECT_EQ(R.F->Diag().MarksApplied, 0u); // the earlier op's hash is pending
	EXPECT_EQ(R.F->Diag().ResidentEntries, 1u);
	const int64_t H = R.F->Request(R.E); // issued after the pass
	REQUIRE_NE(H, 0);
	Sim.HeldHashLeases.insert(H);
	EXPECT_TRUE(Sim.RunHashOfLease(Other));
	EXPECT_EQ(R.F->Diag().MarksApplied, 1u); // the mark's application was queued ahead of H
	EXPECT_TRUE(Sim.RunHashOfLease(H));
	Sim.RunAll();
	EXPECT_EQ(R.F->Cache.GetSource(H), EPrefixSource::Recomputed);
	EXPECT_EQ(R.F->Diag().MarksCleared, 0u);
	EXPECT_EQ(R.F->Cache.GetLeaseState(Other), EPrefixLeaseState::Ready);
}

HOST_TEST(order_mark_applies_under_a_request_every_frame, "3.13 [U2] third leg (T1): after the return, one request a frame for a fresh key, each hash held one frame: E's mark applies by the second frame (mutant: the wait not bounded by the pass -> the mark never applies within the watchdog)")
{
	FTriggerRun R(ETrigger::T1LeaseReturned);
	EXPECT_EQ(R.Why, std::string());
	REQUIRE_TRUE(R.bReady);
	FSim& Sim = *R.F->Sim;
	R.Before();
	int64_t Previous = 0;
	int AppliedAtFrame = -1;
	for (int Frame = 0; Frame < 40 && AppliedAtFrame < 0; ++Frame)
	{
		const int64_t H = R.F->Request(FixtureSpec(100 + Frame, 16));
		REQUIRE_NE(H, 0);
		Sim.HeldHashLeases.insert(H);
		if (Previous != 0)
		{
			EXPECT_TRUE(Sim.RunHashOfLease(Previous)); // this frame's pass, with last frame's hash
		}
		else
		{
			Sim.Pass(); // the first frame's pass: nothing has completed
		}
		Sim.RunAll();
		if (R.F->Diag().MarksApplied == 1u)
		{
			AppliedAtFrame = Frame;
		}
		Previous = H;
	}
	EXPECT_GE(AppliedAtFrame, 0);
	EXPECT_LE(AppliedAtFrame, 1); // by the second frame
}
