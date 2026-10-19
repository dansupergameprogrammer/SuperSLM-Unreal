// Plugin paging plan, GPU red suite (step K5): the pooled holder's limit (§3.4 "Generation limit";
// §7 cell 4.2 and §5's KvBudgetExceeded row).
//
//   SuperSLM.Paging.Gpu.Limit.ExactCounts          cell 4.2, budgets {1, B-1, B, B+1, 512}      [K6 plain; K7 adopter]
//   SuperSLM.Paging.Gpu.Limit.AtCap                cell 4.2, budgets {cap-1, cap}               [K6 plain; K7 adopter] (Stress)
//   SuperSLM.Paging.Gpu.Limit.TwoCap.StopsAtContextCap  cell 4.2 [two-cap]                      [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Limit.ExceededAtActivation §5 KvBudgetExceeded at activation (GPU)       [K6 plain; K7 adopter]
//   SuperSLM.Paging.Gpu.Limit.ActivationReadsAppliedShape  cell 3.6 (rev 1.10)                  [K7]
//
// E7: a holder resting ready at c emits 1 + (limit - c) tokens, then stops by name. A plain holder with
// a 1-token prompt rests at c = 1 with limit b, so it emits b; an adopter of a ready L-token prefix with
// an empty suffix rests at c = L with limit min(L + b, ContextCap), so it emits b + 1 below the cap. The
// stop at the holder's limit is Faulted with ESuperSLMGpuFaultReason::KvBudgetExceeded; at limit ==
// ContextCap the existing cap outcome fires (Faulted with RecoverablePerSequenceRejection, from
// ContextHasRoom; the K5 record, reading R2). AtCap is box-run by name (Stress): its legs generate up
// to 4,096 tokens each. Every test fails first at ConfigurePooled() while pooled mode is unbuilt.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	// Runs one generation on Seq to its stop, and grades the count and the named stop.
	bool ExpectStop(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const TArray<int32>& Prompt, int64 ExpectedTokens,
		ESuperSLMGpuFaultReason ExpectedFault, const FString& What)
	{
		FGpuGeneration G;
		if (!Generate(T, Gpu, Seq, Prompt, int32(ExpectedTokens) + 8, G))
		{
			return false;
		}
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: the generation is accepted (%s)"), *What, *G.Error), G.bRefusedAtCall);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: emits E7's %lld tokens"), *What, ExpectedTokens), int64(G.Tokens.Num()), ExpectedTokens);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the next step stops the sequence (Faulted)"), *What), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: by name, %s (got %s)"), *What, FaultName(ExpectedFault), FaultName(G.Fault)), (uint8)G.Fault, (uint8)ExpectedFault);
		return bOk;
	}

	// A plain holder at budget Budget with a 1-token prompt, and an adopter of an L-token prefix with an
	// empty suffix, each run to its stop. Each stop must be the plugin's own: no "limit fault" line is
	// logged (§3.7). A plugin check one late yields the same count, stopped by the engine's
	// KV_BUDGET_EXCEEDED, which the plugin logs as a limit fault (the K5 coverage audit, S4). The
	// assertion text avoids the token itself, so a failed check cannot feed a later count.
	bool RunBudget(FAutomationTestBase& T, FGpuPagingRig& Rig, int32 Budget, int64 L, bool bAdopter, int64 ExpectedPlain, ESuperSLMGpuFaultReason PlainFault,
		int64 ExpectedAdopter, ESuperSLMGpuFaultReason AdopterFault)
	{
		if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 1, Budget, bAdopter ? 1 : 0, 0)))
		{
			return false;
		}
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		FGpuPagingLogCapture Log;
		TArray<int32> One;
		FSuperSLMGpuSequence Seq;
		if (!MakeTokens(T, *Rig.Model, 1, 601, One) || !Vend(T, Gpu, Seq))
		{
			return false;
		}
		bool bOk = ExpectStop(T, Gpu, Seq, One, ExpectedPlain, PlainFault, FString::Printf(TEXT("budget %d, plain holder, prompt 1"), Budget));
		bOk &= NoLedgerFault(T, Gpu, *FString::Printf(TEXT("budget %d, plain"), Budget));
		bOk &= T.TestEqual(*FString::Printf(TEXT("budget %d, plain: the stop is the plugin's, not the engine's KV_BUDGET_EXCEEDED (no limit-fault log line)"), Budget),
			Log.CountContaining(TEXT("limit fault")), 0);
		ReturnAndDrain(Gpu, Seq);
		if (!bAdopter)
		{
			return bOk;
		}
		TArray<int32> PrefixTokens;
		FSuperSLMPrefix Prefix;
		if (!MakeTokens(T, *Rig.Model, int32(L), 602, PrefixTokens) || !BuildPrefix(T, Gpu, PrefixTokens, Prefix) || !Vend(T, Gpu, Seq) || !AdoptAndWait(T, Gpu, Seq, Prefix))
		{
			return false;
		}
		bOk &= ExpectStop(T, Gpu, Seq, TArray<int32>(), ExpectedAdopter, AdopterFault, FString::Printf(TEXT("budget %d, adopter of %lld, empty suffix"), Budget, L));
		bOk &= NoLedgerFault(T, Gpu, *FString::Printf(TEXT("budget %d, adopter"), Budget));
		bOk &= T.TestEqual(*FString::Printf(TEXT("budget %d, adopter: the stop is the plugin's, not the engine's KV_BUDGET_EXCEEDED (no limit-fault log line)"), Budget),
			Log.CountContaining(TEXT("limit fault")), 0);
		ReturnAndDrain(Gpu, Seq);
		bOk &= ReleasePrefixAndDrain(T, Gpu, Prefix);
		return bOk;
	}

	// The activation pre-check (plan rev 1.9 ruling 6, §3.4): in pooled mode it runs when the
	// BeginGeneration op is admitted at its queue head. A prompt that would run past the limit gets a
	// valid handle at the call, and the sequence faults KvBudgetExceeded before any token (the CPU's
	// ActivateGenerate shape; this settles the K5 record's reading R2). Nothing is emitted.
	bool ExpectActivationRefused(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq, const TArray<int32>& Prompt, const TCHAR* What)
	{
		FGpuGeneration G;
		if (!Generate(T, Gpu, Seq, Prompt, 4, G))
		{
			return false;
		}
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: accepted at the call (a valid handle; %s)"), What, *G.Error), G.bRefusedAtCall);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: at admission the sequence is Faulted"), What), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: by name, KvBudgetExceeded (got %s)"), What, FaultName(G.Fault)), (uint8)G.Fault, (uint8)ESuperSLMGpuFaultReason::KvBudgetExceeded);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing is emitted"), What), G.Tokens.Num(), 0);
		return bOk;
	}
}

// Cell 4.2 (audit G12), budgets {1, B - 1, B, B + 1, 512} at the GPU's B = 16: a plain holder with a
// 1-token prompt emits exactly b tokens, and an adopter of a ready 208-token prefix with an empty
// suffix emits b + 1; each then stops Faulted with KvBudgetExceeded, and no "limit fault" line is
// logged (the stop is the plugin's pre-check, not the engine's budget enforcement).
// Mutants: the pre-check "<=" for "<" (one late: the engine's KV_BUDGET_EXCEEDED stops it at the same
// count, so only the log check kills it); "limit - 1" (one early: the count is one short).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuLimitExactCountsTest, "SuperSLM.Paging.Gpu.Limit.ExactCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuLimitExactCountsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const int32 Budget : { 1, 15, 16, 17, 512 })
	{
		bOk &= RunBudget(*this, Rig, Budget, 208, true, Budget, ESuperSLMGpuFaultReason::KvBudgetExceeded, int64(Budget) + 1, ESuperSLMGpuFaultReason::KvBudgetExceeded);
	}
	bOk &= TestEqual(TEXT("the cell's budgets are B - 1, B and B + 1 at the engine's B"), Rig.Geo.B, int64(16));
	return bOk;
}

// Cell 4.2, budgets {cap - 1, cap} = {4,095, 4,096} at 0.5B (one cap): the plain holder emits 4,095 and
// stops KvBudgetExceeded; at 4,096 it emits 4,096 and the cap outcome fires. The adopter of a
// 208-token prefix reaches ContextCap first at both budgets (limit min(208 + b, 4,096) = 4,096): it
// emits 1 + 4,096 - 208 = 3,889 and the cap outcome fires.
// Mutants: as ExactCounts (one late is killed by the "limit fault" log check at 4,095, not by the
// count); the cap outcome replaced by KvBudgetExceeded at limit == cap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuLimitAtCapTest, "SuperSLM.Paging.Gpu.Limit.AtCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuLimitAtCapTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	const ESuperSLMGpuFaultReason CapOutcome = ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection;
	bool bOk = RunBudget(*this, Rig, 4095, 208, true, 4095, ESuperSLMGpuFaultReason::KvBudgetExceeded, 3889, CapOutcome);
	bOk &= RunBudget(*this, Rig, 4096, 208, true, 4096, CapOutcome, 3889, CapOutcome);
	return bOk;
}

// Cell 4.2 [two-cap]: at the 1.5B artifact with ContextCap 4,096, an adopter of a 3,800-token prefix at
// budget 512 has origin + b = 4,312 above ContextCap and below the model's cap. It stops at ContextCap:
// 1 + 4,096 - 3,800 = 297 tokens, then the cap outcome. The plugin's pre-check is the only stop there.
// Mutant: the limit at the model's cap (it emits 513 tokens, past ContextCap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuLimitTwoCapTest, "SuperSLM.Paging.Gpu.Limit.TwoCap.StopsAtContextCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuLimitTwoCapTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 1, 0)) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> PrefixTokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMGpuSequence Seq;
	if (!MakeTokens(*this, *Rig.Model, 3800, 611, PrefixTokens) || !BuildPrefix(*this, Gpu, PrefixTokens, Prefix) || !Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
	{
		return false;
	}
	bool bOk = ExpectStop(*this, Gpu, Seq, TArray<int32>(), 297, ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection, TEXT("[two-cap] adopter of 3,800 at budget 512"));
	ReturnAndDrain(Gpu, Seq);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	return bOk;
}

// §5's KvBudgetExceeded at activation on the GPU (round-2 finding 7; §3.4 "Activation pre-check"): at
// budget 16, a plain holder's 17-token prompt is refused by name (rev 1.9 ruling 6: a valid handle, then
// Faulted with KvBudgetExceeded at admission, before any token) and a 16-token prompt is accepted
// (emitting 1); an adopter of a 208-token prefix (limit 224) is refused a 17-token suffix and accepts
// a 16-token one (emitting 1).
// Mutants: the check "context_used + prompt >= limit" (the boundary prompt refused); the check absent
// (the engine's KV_BUDGET_EXCEEDED reached mid-prefill, logged as a limit fault); the pooled check left
// at the call (an invalid handle).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuLimitActivationTest, "SuperSLM.Paging.Gpu.Limit.ExceededAtActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuLimitActivationTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 16, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> P16, P17, PrefixTokens;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 16, 621, P16) || !MakeTokens(*this, *Rig.Model, 17, 622, P17) || !MakeTokens(*this, *Rig.Model, 208, 623, PrefixTokens)
		|| !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	FGpuPagingLogCapture Log;
	bool bOk = true;
	FSuperSLMGpuSequence Seq;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}
	bOk &= ExpectActivationRefused(*this, Gpu, Seq, P17, TEXT("plain holder, budget 16, prompt 17"));
	bOk &= ResetAndWait(*this, Gpu, Seq);
	bOk &= ExpectStop(*this, Gpu, Seq, P16, 1, ESuperSLMGpuFaultReason::KvBudgetExceeded, TEXT("plain holder, budget 16, prompt 16"));
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, Prefix);
	bOk &= ExpectActivationRefused(*this, Gpu, Seq, P17, TEXT("adopter of 208, budget 16, suffix 17"));
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, Prefix);
	bOk &= ExpectStop(*this, Gpu, Seq, P16, 1, ESuperSLMGpuFaultReason::KvBudgetExceeded, TEXT("adopter of 208, budget 16, suffix 16"));
	bOk &= TestEqual(TEXT("no limit fault is logged (the engine's KV_BUDGET_EXCEEDED is never reached)"), Log.CountContaining(TEXT("limit fault")), 0);
	ReturnAndDrain(Gpu, Seq);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	return bOk;
}

// Cell 3.6 (plan rev 1.10; K7 code review S1): activation reads the applied shape. Pooled, one slot,
// SequenceBudget 512 (one P block, one H block), so every vend is slot 0. In pooled mode BeginGeneration
// waits at the slot front like a reset or adopt (§3.4), and a faulted adopt applies the reset shape to the
// ledger.
//   - Too strict, without the wait: a holder restored from a budget-100 save at origin 0 (limit 100) is
//     returned. The slot is vended again at once, and a 300-token prompt (MaxNewTokens 2) is begun with
//     nothing queued ahead. The generation is admitted once the recycle has applied, and runs: Complete,
//     2 tokens.
//   - Too permissive, without the wait: a holder that adopted a 1,000-token prefix (origin 1,000) is
//     returned. The slot is vended, and a 513-token prompt is begun at once. It is refused
//     KvBudgetExceeded at activation, before any token: a valid handle, Faulted, no token, and ContextUsed
//     0. It is never stopped mid-prompt, which would show as ContextUsed above 0.
//   - After a fault-after-share adopt (§3.4's seam), built afresh for each prompt. The holder first adopts
//     the 1,000-token prefix successfully, so the ledger's shape is origin 1,000 with limit 1,512. The
//     faulted adopt of the same prefix runs next. The ledger admits a second adopt of the same node
//     (AdmitAdoptPrefix checks only the slot's job, the node's state and the holder's mode), and the
//     engine's adopt accepts a non-empty sequence. The reset shape then puts the limit back to 512.
//     A 512-token prompt is admitted, emits E7's 1 + (512 - 512) = 1 token, and stops KvBudgetExceeded.
//     A 513-token prompt is refused at activation, before any token. Without the reset shape the ledger
//     keeps origin 1,000: the 513-token prompt passes activation and completes, and the 512-token one
//     runs past one token. Either way the leg goes red (K7 re-check). The first draft vended each holder
//     fresh, so its slot already had the reset shape, and the leg passed without the rule.
// In the first two legs the previous holder's recycle is held at its apply (HoldApply(Return)) while the
// new holder's BeginGeneration is issued and ticked, which keeps the window open. The cell asserts no
// fault in that window, then releases the hold. A build may admit the generation then (the plan's wait) or
// earlier against the new holder's own shape; the cell grades only the outcome.
// Mutants: BeginGeneration not held at the slot front, which reads the previous holder's limit. Leg 1 is
// then faulted KvBudgetExceeded (100 < 304), and leg 2 passes activation against the stale
// min(1,000 + 512, 4,096) and is stopped mid-prompt. The ledger origin kept after a faulted adopt (the
// reset shape not applied) fails leg 3: the 513-token prompt is admitted and completes, and the 512-token
// one emits more than 1 token.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuActivationAppliedShapeTest, "SuperSLM.Paging.Gpu.Limit.ActivationReadsAppliedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuActivationAppliedShapeTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B100;
	// MakeBlobAt() configures its own shape, so the cell's shape is configured after it.
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 100, 0, 631, B100))
	{
		return false;
	}
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 1, 1);
	if (!ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> PrefixTokens, P300, P512, P513;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 632, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 300, 633, P300) || !MakeTokens(*this, *Rig.Model, 512, 634, P512)
		|| !MakeTokens(*this, *Rig.Model, 513, 635, P513) || !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}

	// Begins Prompt on Seq and runs it to its end; Window, when set, runs after the request and before
	// anything is pumped to the end (the held-recycle legs release their hold there).
	auto RunTurn = [this, &Gpu](const FSuperSLMGpuSequence& Seq, const TArray<int32>& Prompt, int32 MaxNew, const TCHAR* What, TFunctionRef<bool()> Window, FGpuGeneration& Out)
	{
		Out = FGpuGeneration();
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNew;
		const FSuperSLMLifecycleOpHandle Handle = Gpu.RequestBeginGeneration(Seq, Request);
		Out.bRefusedAtCall = !Handle.IsValid();
		bool bTurn = TestTrue(*FString::Printf(TEXT("%s: the request is accepted at the call (%s)"), What, *Gpu.GetLastLifecycleRequestError()), Handle.IsValid());
		bTurn &= Window();
		if (Handle.IsValid())
		{
			Pump(Gpu, [&Gpu, &Seq, &Handle]()
			{
				const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
				return Gpu.GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending
					&& (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Gpu.GetPendingLifecycleOperationCount(Seq) == 0;
			}, GenerateSeconds);
			Out.Started = Gpu.GetLifecycleOpResult(Handle);
			Gpu.ReleaseLifecycleOpHandle(Handle);
		}
		Out.Phase = Gpu.GetPhase(Seq);
		Out.Fault = Gpu.GetLastFaultReason(Seq);
		Out.Tokens = Gpu.GetGeneratedTokens(Seq);
		Out.Error = Gpu.GetLastLifecycleRequestError();
		return bTurn;
	};
	// The previous holder is returned with its recycle held at the apply; the slot is vended to Next at once.
	auto ReturnHeldAndRevend = [this, &Gpu](const FSuperSLMGpuSequence& Previous, FSuperSLMGpuSequence& Next, FSuperSLMPagedHold& OutHold, const TCHAR* What)
	{
		OutHold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Return);
		Gpu.ReturnSequence(Previous);
		bool bHeld = TestTrue(*FString::Printf(TEXT("%s: the previous holder's recycle is held at its apply"), What), OutHold.IsValid() && WaitHoldReached(Gpu, OutHold));
		bHeld &= TestEqual(*FString::Printf(TEXT("%s: the slot is vended again at once"), What), (uint8)Gpu.VendSequence(Next, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success);
		bHeld &= TestEqual(*FString::Printf(TEXT("%s: the new holder is on the same slot"), What), FSuperSLMPagedGpuTestSeam::SlotIndexOf(Gpu, Next), 0);
		return bHeld;
	};
	// In the held window: the new holder has not been faulted on the previous holder's shape.
	auto HeldWindow = [this, &Gpu](const FSuperSLMGpuSequence& Next, const FSuperSLMPagedHold& Hold, const TCHAR* What)
	{
		TickN(*this, Gpu, 4, What);
		const bool bClean = TestNotEqual(*FString::Printf(TEXT("%s: no fault while the previous holder's recycle is open"), What),
			(uint8)Gpu.GetPhase(Next), (uint8)ESuperSLMSequencePhase::Faulted);
		FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		return bClean;
	};
	auto ExpectRefusedAtActivation = [this, &Gpu](const FSuperSLMGpuSequence& Seq, const FGpuGeneration& G, const TCHAR* What)
	{
		bool bLeg = TestFalse(*FString::Printf(TEXT("%s: a valid handle (%s)"), What, *G.Error), G.bRefusedAtCall);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: Faulted"), What), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: by name, KvBudgetExceeded (got %s)"), What, FaultName(G.Fault)), (uint8)G.Fault, (uint8)ESuperSLMGpuFaultReason::KvBudgetExceeded);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: before any token"), What), G.Tokens.Num(), 0);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: never stopped mid-prompt (ContextUsed 0)"), What), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(0));
		return bLeg;
	};
	bool bOk = true;

	// Leg 1: too strict without the wait.
	{
		FSuperSLMGpuSequence Restored, Next;
		FSuperSLMPagedHold Hold;
		if (!ApplyRestore(*this, Gpu, B100, Rig.Model, Restored) || !ReturnHeldAndRevend(Restored, Next, Hold, TEXT("leg 1")))
		{
			return false;
		}
		FGpuGeneration G;
		bOk &= RunTurn(Next, P300, 2, TEXT("leg 1, a 300-token prompt after a budget-100 holder"), [&]() { return HeldWindow(Next, Hold, TEXT("leg 1")); }, G);
		bOk &= TestEqual(*FString::Printf(TEXT("leg 1: admitted and run to Complete (fault %s; %s)"), FaultName(G.Fault), *G.Error), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Complete);
		bOk &= TestEqual(TEXT("leg 1: 2 tokens"), G.Tokens.Num(), 2);
		ReturnAndDrain(Gpu, Next);
	}
	// Leg 2: too permissive without the wait.
	{
		FSuperSLMGpuSequence Adopter, Next;
		FSuperSLMPagedHold Hold;
		if (!Vend(*this, Gpu, Adopter) || !AdoptAndWait(*this, Gpu, Adopter, Prefix) || !ReturnHeldAndRevend(Adopter, Next, Hold, TEXT("leg 2")))
		{
			return false;
		}
		FGpuGeneration G;
		bOk &= RunTurn(Next, P513, 2, TEXT("leg 2, a 513-token prompt after an adopter at 1,000"), [&]() { return HeldWindow(Next, Hold, TEXT("leg 2")); }, G);
		bOk &= ExpectRefusedAtActivation(Next, G, TEXT("leg 2: refused at activation"));
		ReturnAndDrain(Gpu, Next);
	}
	// Leg 3: a successful adopt (origin 1,000), then a fault-after-share adopt of the same prefix, built
	// afresh for each prompt.
	auto FaultedAdopter = [this, &Gpu, &Prefix](FSuperSLMGpuSequence& Seq, const TCHAR* What)
	{
		if (!Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, Prefix)
			|| !TestEqual(*FString::Printf(TEXT("%s: the first adopt leaves the holder at origin 1,000"), What), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(1000))
			|| !TestTrue(*FString::Printf(TEXT("%s: the fault-after-share seam is armed"), What), FSuperSLMPagedGpuTestSeam::ArmAdoptFaultAfterShare(Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED)))
		{
			return false;
		}
		FString Error;
		const ESuperSLMRestoreResult Result = AdoptResult(Gpu, Seq, Prefix, &Error);
		bool bBuilt = TestTrue(*FString::Printf(TEXT("%s: the adopt faults after the share (got %s; %s)"), What, SuperSLML2S2Fixtures::RestoreResultName(Result), *Error),
			Result != ESuperSLMRestoreResult::Success && Result != ESuperSLMRestoreResult::Pending);
		bBuilt &= TestEqual(*FString::Printf(TEXT("%s: the sequence is left reset (ContextUsed 0)"), What), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(0));
		bBuilt &= TestEqual(*FString::Printf(TEXT("%s: the sequence is left unarmed"), What), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
		return bBuilt;
	};
	auto NoWindow = []() { return true; };
	{
		FSuperSLMGpuSequence Seq;
		if (!FaultedAdopter(Seq, TEXT("leg 3 (512)")))
		{
			return false;
		}
		FGpuGeneration G;
		bOk &= RunTurn(Seq, P512, 4, TEXT("leg 3, a 512-token prompt after a faulted adopt"), NoWindow, G);
		bOk &= TestFalse(*FString::Printf(TEXT("leg 3 (512): a valid handle (%s)"), *G.Error), G.bRefusedAtCall);
		bOk &= TestEqual(TEXT("leg 3 (512): admitted, it emits E7's 1 token (limit 512, not the stale 1,512)"), G.Tokens.Num(), 1);
		bOk &= TestEqual(*FString::Printf(TEXT("leg 3 (512): then stops at its limit, KvBudgetExceeded (got %s)"), FaultName(G.Fault)), (uint8)G.Fault, (uint8)ESuperSLMGpuFaultReason::KvBudgetExceeded);
		ReturnAndDrain(Gpu, Seq);
	}
	{
		FSuperSLMGpuSequence Seq;
		if (!FaultedAdopter(Seq, TEXT("leg 3 (513)")))
		{
			return false;
		}
		FGpuGeneration G;
		bOk &= RunTurn(Seq, P513, 4, TEXT("leg 3, a 513-token prompt after a faulted adopt"), NoWindow, G);
		bOk &= ExpectRefusedAtActivation(Seq, G, TEXT("leg 3 (513): refused at activation"));
		ReturnAndDrain(Gpu, Seq);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 3.6"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
