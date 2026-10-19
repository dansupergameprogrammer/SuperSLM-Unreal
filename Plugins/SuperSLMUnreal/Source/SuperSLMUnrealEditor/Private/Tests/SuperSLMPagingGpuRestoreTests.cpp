// Plugin paging plan, GPU red suite (step K5): pooled save and restore (§3.5; §7 cells 1.12, 2.1, 2.3,
// 2.6, 3.2, 4.4, 4.5, 6.3, 9.6, 9.10).
//
//   SuperSLM.Paging.Gpu.Restore.HostileHeader               cell 2.1, GPU legs                       [K7]
//   SuperSLM.Paging.Gpu.Restore.TwoCap.HostileHeader        cell 2.1 [two-cap]; cell 2.6 budget bound [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Restore.ModeCrossings               cells 2.3, 9.6 (GPU); §5 KvModeMismatch  [K7]
//   SuperSLM.Paging.Gpu.Restore.NoLostState                 cell 1.12, GPU                           [K7]
//   SuperSLM.Paging.Gpu.Restore.Waits                       cell 3.2 (c)                              [K7]
//   SuperSLM.Paging.Gpu.Restore.StrikeConstructions         cell 3.2 (d) and (e), GPU                 [K7]
//   SuperSLM.Paging.Gpu.Restore.Headroom                    cell 4.4, 0.5B legs                       [K7]
//   SuperSLM.Paging.Gpu.Restore.TwoCap.Headroom             cell 4.4, [two-cap] legs                  [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Restore.Origins                     cell 4.5, 0.5B                            [K7]
//   SuperSLM.Paging.Gpu.Restore.TwoCap.Origins              cell 4.5 [two-cap]                        [K7] (Stress, 1.5B)
//   SuperSLM.Paging.Gpu.Restore.HintSharing                 cells 9.10 (twins of 9.2, 9.3), 6.3       [K7]
//
// The executed shape is budget 512 at origin 1,000: charge R(512) + E = 33 + 62 = 95, guarantee 33,
// net private hold 62. Constructions are for 0.5B (B = 16, block 256) unless marked [two-cap]
// (H_blk = 513). "Refused before any job" is graded by a HoldApply that is never reached and
// JobsPosted() unchanged (K1 coverage audit B2). Every test fails first at ConfigurePooled() while
// pooled mode is unbuilt.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	using ERegion = ESuperSLMPagedRegion;

	// A refusal the payload header alone decides is made at the call (plan rev 1.9 §3.5, "Where a header
	// refusal is carried"; ruling 1): no handle is minted, and GetLastLifecycleRequestError() begins
	// "<Result>:". The wrapper's ContextUsed check is one of §3.5's listed header defects, so it is
	// carried the same way.
	bool ExpectRefusedAtCall(FAutomationTestBase& T, const FIssuedGpuRestore& R, ESuperSLMRestoreResult Expected, const TCHAR* What)
	{
		const FString Prefix = FString::Printf(TEXT("%s:"), SuperSLML2S2Fixtures::RestoreResultName(Expected));
		bool bOk = T.TestFalse(*FString::Printf(TEXT("%s: refused at the call, no handle minted (%s)"), What, *R.CallError), R.Handle.IsValid());
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the error begins '%s' (got '%s')"), What, *Prefix, *R.CallError), R.CallError.StartsWith(Prefix, ESearchCase::CaseSensitive));
		return bOk;
	}

	// A restore refused at the call, before any job: an invalid handle with the named error (above), no
	// sequence, nothing charged on H, no Restore job posted (the armed HoldApply is never reached), and
	// the bystander sequence's state unchanged.
	bool ExpectRefusedBeforeAnyJob(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, USuperSLMModel* Model, ESuperSLMRestoreResult Expected,
		const FSuperSLMGpuSequence& Bystander, const TCHAR* What, const TCHAR* MessageToken = nullptr)
	{
		const int64 HBefore = Charged(Gpu, ERegion::H);
		const int64 JobsBefore = FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore);
		const TArray<int32> TokensBefore = Gpu.GetGeneratedTokens(Bystander);
		const ESuperSLMSequencePhase PhaseBefore = Gpu.GetPhase(Bystander);
		const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
		const FIssuedGpuRestore R = IssueRestore(Gpu, Blob, Model);
		bool bOk = ExpectRefusedAtCall(T, R, Expected, What);
		if (MessageToken)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the message names %s (%s)"), What, MessageToken, *R.CallError), R.CallError.Contains(MessageToken));
		}
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: nothing charged on H"), What), Charged(Gpu, ERegion::H), HBefore);
		bOk &= ExpectNoJobPosted(T, Gpu, ESuperSLMPagedRequestKind::Restore, Hold, JobsBefore, What);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the bystander's tokens are unchanged"), What), Gpu.GetGeneratedTokens(Bystander), TokensBefore);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the bystander's phase is unchanged"), What), (uint8)Gpu.GetPhase(Bystander), (uint8)PhaseBefore);
		if (R.Handle.IsValid())
		{
			// A wrong build minted one: settle it so it cannot leak into the next leg, and say what it read.
			FSuperSLMGpuSequence Seq;
			const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, R, Seq, 10.0);
			T.AddError(FString::Printf(TEXT("%s: the handle resolved %s"), What, SuperSLML2S2Fixtures::RestoreResultName(Outcome)));
			if (Seq.IsValid())
			{
				Gpu.ReturnSequence(Seq);
			}
			Gpu.ReleaseLifecycleOpHandle(R.Handle);
			DrainQuiet(Gpu);
		}
		return bOk;
	}

	TArray<uint8> Edited(const TArray<uint8>& Blob, TFunctionRef<void(TArray<uint8>&)> Edit)
	{
		TArray<uint8> Out = Blob;
		FSuperSLMPagedTestSeam::EditPayload(Out, Edit);
		return Out;
	}

	// The burst cells: N restores issued in one frame with every apply held, then read: admitted (held
	// at their applies), waiting (Pending, not held), refused (KvPagesExhausted on arrival).
	struct FBurst
	{
		TArray<FIssuedGpuRestore> Restores;
		TArray<FSuperSLMPagedHold> Holds;
	};

	FBurstCounts IssueBurst(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<uint8>& Blob, USuperSLMModel* Model, const FSuperSLMPrefix& Hint, int32 N,
		int32 HoldsToArm, FBurst& Out)
	{
		for (int32 I = 0; I < HoldsToArm; ++I)
		{
			Out.Holds.Add(FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore));
		}
		for (int32 I = 0; I < N; ++I)
		{
			Out.Restores.Add(IssueRestore(Gpu, Blob, Model, Hint));
		}
		TickN(T, Gpu, 6, TEXT("the burst arrives"));
		// Every admitted restore reaches its hold; then the counts are stable.
		const int32 InFlight = LedgerOf(Gpu).InFlightRestores;
		for (int32 I = 0; I < FMath::Min(InFlight, Out.Holds.Num()); ++I)
		{
			WaitHoldReached(Gpu, Out.Holds[I], 30.0);
		}
		FBurstCounts Counts;
		for (const FIssuedGpuRestore& R : Out.Restores)
		{
			FSuperSLMGpuSequence Seq;
			const ESuperSLMRestoreResult Outcome = OutcomeOf(Gpu, R, Seq);
			if (Outcome == ESuperSLMRestoreResult::KvPagesExhausted)
			{
				++Counts.Refused;
			}
			else if (Outcome != ESuperSLMRestoreResult::Pending)
			{
				T.AddError(FString::Printf(TEXT("a burst restore resolved %s while every apply was held"), SuperSLML2S2Fixtures::RestoreResultName(Outcome)));
			}
		}
		const FSuperSLMPagedLedgerView V = LedgerOf(Gpu);
		Counts.Admitted = V.InFlightRestores;
		Counts.Waiting = V.WaitingRestores;
		return Counts;
	}

	// Releases every hold, then requires every non-refused restore of the burst to succeed: no waiter
	// ever resolves KvPagesExhausted (§3.5: "every waiter is admitted"). Nothing is given back here:
	// OutSettled receives every restored sequence, so a cell can read H and each holder's hold before
	// DiscardBurst() returns them (K5 coverage audit S1).
	bool DrainBurst(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, FBurst& Burst, const TCHAR* What, TArray<FSuperSLMGpuSequence>* OutSettled = nullptr)
	{
		bool bOk = true;
		TArray<ESuperSLMRestoreResult> AtArrival;
		for (const FIssuedGpuRestore& R : Burst.Restores)
		{
			FSuperSLMGpuSequence Seq;
			AtArrival.Add(OutcomeOf(Gpu, R, Seq));
		}
		for (const FSuperSLMPagedHold& Hold : Burst.Holds)
		{
			FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
		}
		for (int32 I = 0; I < Burst.Restores.Num(); ++I)
		{
			FSuperSLMGpuSequence Seq;
			const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, Burst.Restores[I], Seq);
			if (AtArrival[I] == ESuperSLMRestoreResult::Pending)
			{
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: restore %d (admitted or waiting) succeeds; no waiter ends short"), What, I),
					(uint8)Outcome, (uint8)ESuperSLMRestoreResult::Success);
			}
			if (OutSettled && Seq.IsValid())
			{
				OutSettled->Add(Seq);
			}
		}
		DrainQuiet(Gpu);
		return bOk;
	}

	// Gives back every sequence the burst vended and releases its handles.
	void DiscardBurst(USuperSLMGpuSubsystem& Gpu, const FBurst& Burst)
	{
		for (const FIssuedGpuRestore& R : Burst.Restores)
		{
			Discard(Gpu, R);
		}
	}

	bool ExpectCounts(FAutomationTestBase& T, const FBurstCounts& Got, const FBurstCounts& Want, const TCHAR* What)
	{
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: admitted"), What), Got.Admitted, Want.Admitted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: waiting"), What), Got.Waiting, Want.Waiting);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: refused on arrival"), What), Got.Refused, Want.Refused);
		return bOk;
	}
}

// Cell 2.1, the GPU legs (§3.5 "Malformed input"; rev 1.9 ruling 1): the 'SLM6' header corrupted in each
// field is refused Malformed at the call (an invalid handle, the error beginning "Malformed:"), before
// any job, with nothing drawn and a bystander sequence untouched: an unknown magic;
// budget, origin and context_length above the cap (the model's cap, 4,096 at 0.5B, where the two caps
// coincide); the wrapper's ContextUsed differing from context_length; kv_mode not budget (plan review
// N3: never the engine's SEQUENCE_KV_BUFFER_MISMATCH; at the call since rev 1.9, no longer at
// admission); a budget of 0 (the domain is [1, cap]). A ready
// word of 2 reads as ready, as the engine reads it (round-2 note 10): the restored sequence is armed.
// Mutants: a field unchecked (the engine refuses it after a draw, or accepts it); the [0, cap] budget
// check (budget 0 passes to a zero charge); ready read as "== 1".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuHostileHeaderTest, "SuperSLM.Paging.Gpu.Restore.HostileHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuHostileHeaderTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 1);
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	FGpuPayloadHeaderFields H;
	if (!ReadGpuHeader(*this, B1000, H) || !TestTrue(TEXT("the base blob is 'SLM6'"), H.bSlm6))
	{
		return false;
	}
	FSuperSLMGpuSequence Bystander;
	TArray<int32> Prompt, Tokens;
	if (!Vend(*this, Gpu, Bystander) || !MakeTokens(*this, *Rig.Model, 8, 2, Prompt) || !GenerateComplete(*this, Gpu, Bystander, Prompt, 3, Tokens))
	{
		return false;
	}
	using namespace GpuPayloadHeader;
	const int64 Cap = Rig.Geo.Ce;
	bool bOk = true;
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [](TArray<uint8>& P) { P[0] = 'X'; P[1] = 'X'; P[2] = 'X'; P[3] = 'X'; }), Rig.Model,
		ESuperSLMRestoreResult::Malformed, Bystander, TEXT("an unknown magic"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [Cap](TArray<uint8>& P) { WriteLE(P, Budget, 4, Cap + 1); }), Rig.Model,
		ESuperSLMRestoreResult::Malformed, Bystander, TEXT("budget > cap"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [Cap](TArray<uint8>& P) { WriteLE(P, Origin, 8, Cap + 1); }), Rig.Model,
		ESuperSLMRestoreResult::Malformed, Bystander, TEXT("origin > cap"));
	{
		TArray<uint8> Long = Edited(B1000, [Cap](TArray<uint8>& P) { WriteLE(P, ContextLength, 8, Cap + 1); });
		bOk &= TestTrue(TEXT("the wrapper's ContextUsed is kept equal, so only the cap check can refuse"), FSuperSLMPagedTestSeam::SetWrapperContextUsed(Long, Cap + 1));
		bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Long, Rig.Model, ESuperSLMRestoreResult::Malformed, Bystander, TEXT("context_length > cap"));
	}
	{
		TArray<uint8> Lying = B1000;
		bOk &= TestTrue(TEXT("the wrapper's ContextUsed is rewritten"), FSuperSLMPagedTestSeam::SetWrapperContextUsed(Lying, H.ContextLength + 1));
		bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Lying, Rig.Model, ESuperSLMRestoreResult::Malformed, Bystander, TEXT("the wrapper's ContextUsed differs from context_length"));
	}
	const int64 NotBudget = H.KvMode + 1;
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [NotBudget](TArray<uint8>& P) { WriteLE(P, KvMode, 4, NotBudget); }), Rig.Model,
		ESuperSLMRestoreResult::Malformed, Bystander, TEXT("kv_mode not budget"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [](TArray<uint8>& P) { WriteLE(P, Budget, 4, 0); }), Rig.Model,
		ESuperSLMRestoreResult::Malformed, Bystander, TEXT("a budget of 0"));
	ReturnAndDrain(Gpu, Bystander);

	// A ready word of 2: a save taken between the adopt and the first finish (armed), its ready word
	// rewritten to 2.
	TArray<int32> PrefixTokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMGpuSequence Adopter, Restored;
	TArray<uint8> Armed;
	if (!MakeTokens(*this, *Rig.Model, 208, 3, PrefixTokens) || !BuildPrefix(*this, Gpu, PrefixTokens, Prefix) || !Vend(*this, Gpu, Adopter)
		|| !AdoptAndWait(*this, Gpu, Adopter, Prefix) || !SaveAndTake(*this, Gpu, Adopter, Armed))
	{
		return false;
	}
	ReturnAndDrain(Gpu, Adopter);
	FGpuPayloadHeaderFields AH;
	bOk &= ReadGpuHeader(*this, Armed, AH) && TestTrue(TEXT("the save between adopt and first finish is ready"), AH.Ready != 0);
	const TArray<uint8> Ready2 = Edited(Armed, [](TArray<uint8>& P) { WriteLE(P, Ready, 4, 2); });
	bOk &= ApplyRestore(*this, Gpu, Ready2, Rig.Model, Restored);
	bOk &= TestEqual(TEXT("a ready word of 2 reads as ready: the restored sequence is armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Restored), 1);
	return bOk;
}

// Cell 2.1's [two-cap] legs and cell 2.6's budget bound (rev 1.7; coverage re-audit R1-G1): at the 1.5B
// artifact with ContextCap 4,096, an 'SLM6' whose origin (and context_length) is 4,500 -- above
// ContextCap, below the model's cap -- resolves ContextCapMismatch before any job, its message naming
// the figure and ContextCap; one at 40,000 (above the model's cap) resolves Malformed; one whose budget
// is 4,097 resolves ContextCapMismatch. Each is refused at the call (rev 1.9 ruling 1: an invalid
// handle, the error beginning with the result's name and a colon), before any job.
// Mutants: validation against the model's cap (admits 4,500 and 4,097); the first reported Malformed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapHostileHeaderTest, "SuperSLM.Paging.Gpu.Restore.TwoCap.HostileHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapHostileHeaderTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000)
		|| !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::TwoCap15B, 2, 512, 2, 1)) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	FSuperSLMGpuSequence Bystander;
	if (!Vend(*this, Gpu, Bystander))
	{
		return false;
	}
	using namespace GpuPayloadHeader;
	auto AtOrigin = [&B1000](int64 Value)
	{
		TArray<uint8> Out = Edited(B1000, [Value](TArray<uint8>& P) { WriteLE(P, Origin, 8, Value); WriteLE(P, ContextLength, 8, Value + 5); });
		FSuperSLMPagedTestSeam::SetWrapperContextUsed(Out, Value + 5);
		return Out;
	};
	bool bOk = ExpectRefusedBeforeAnyJob(*this, Gpu, AtOrigin(4500), Rig.Model, ESuperSLMRestoreResult::ContextCapMismatch, Bystander,
		TEXT("[two-cap] origin 4,500"), TEXT("4500"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, AtOrigin(40000), Rig.Model, ESuperSLMRestoreResult::Malformed, Bystander, TEXT("[two-cap] origin 40,000"));
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Edited(B1000, [](TArray<uint8>& P) { WriteLE(P, Budget, 4, 4097); }), Rig.Model,
		ESuperSLMRestoreResult::ContextCapMismatch, Bystander, TEXT("[two-cap] budget 4,097 (cell 2.6)"), TEXT("4096"));
	ReturnAndDrain(Gpu, Bystander);
	return bOk;
}

// Cells 2.3 and 9.6 on the GPU (audit G6), and §5's KvModeMismatch: the magic decides the mode. An
// 'SLM5' payload into a pooled subsystem, and an 'SLM6' into a dedicated one, are each refused
// KvModeMismatch at the call (rev 1.9 ruling 1: an invalid handle, the error beginning
// "KvModeMismatch:"), before any job. A pooled restore's charge
// equals E6's formula from its own header, and the probe mid-job (HoldApply) shows the draw within it.
// Mutants: the mode read from a wrapper field; an 'SLM5' handed to restore_into_pool (the engine's
// HOLDER_MODE_MISMATCH after a draw); a charge not taken from the header.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuModeCrossingsTest, "SuperSLM.Paging.Gpu.Restore.ModeCrossings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuModeCrossingsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> Slm5, Slm6;
	if (!Rig.Init(*this) || !MakeSlm5Blob(*this, Rig, Slm5) || !MakeBlobAt(*this, Rig, 512, 1000, 1, Slm6))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	bool bOk = true;
	// 'SLM6' into dedicated.
	if (!ConfigureDedicated(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 0, 0, 1)))
	{
		return false;
	}
	{
		// The seam counts jobs in any configured mode (-1 only when unconfigured), so the dedicated
		// leg's no-job check reads a real count (the K5 coverage audit, N5).
		const int64 JobsBefore = FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore);
		bOk &= TestTrue(*FString::Printf(TEXT("dedicated: the restore job count is readable (%lld)"), JobsBefore), JobsBefore >= 0);
		const FIssuedGpuRestore R = IssueRestore(Gpu, Slm6, Rig.Model);
		bOk &= ExpectRefusedAtCall(*this, R, ESuperSLMRestoreResult::KvModeMismatch, TEXT("'SLM6' into dedicated"));
		TickN(*this, Gpu, 4, TEXT("'SLM6' into dedicated"));
		bOk &= TestEqual(TEXT("dedicated: refused before any job (the restore job count is unchanged)"),
			FSuperSLMPagedGpuTestSeam::JobsPosted(Gpu, ESuperSLMPagedRequestKind::Restore), JobsBefore);
		if (R.Handle.IsValid())
		{
			Gpu.ReleaseLifecycleOpHandle(R.Handle);
		}
	}
	// 'SLM5' into pooled.
	if (!ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512, 0, 1)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	FSuperSLMGpuSequence Bystander;
	if (!Vend(*this, Gpu, Bystander))
	{
		return false;
	}
	bOk &= ExpectRefusedBeforeAnyJob(*this, Gpu, Slm5, Rig.Model, ESuperSLMRestoreResult::KvModeMismatch, Bystander, TEXT("'SLM5' into pooled"));
	ReturnAndDrain(Gpu, Bystander);
	// The charge, from the header, and the draw within it mid-job.
	FGpuPayloadHeaderFields H;
	if (!ReadGpuHeader(*this, Slm6, H))
	{
		return false;
	}
	const FSuperSLMPagedHold Hold = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
	const FIssuedGpuRestore R = IssueRestore(Gpu, Slm6, Rig.Model);
	if (!TestTrue(TEXT("the pooled restore is held at its apply"), WaitHoldReached(Gpu, Hold)))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the charge is R(b) + E from the payload header"), Charged(Gpu, ERegion::H), Rig.Geo.RestoreCharge(H.Budget, H.Origin));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("the restore held mid-job"));
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, Hold);
	FSuperSLMGpuSequence Seq;
	bOk &= TestEqual(TEXT("the restore succeeds"), (uint8)SettleOutcome(Gpu, R, Seq), (uint8)ESuperSLMRestoreResult::Success);
	Discard(Gpu, R);
	return bOk;
}

// Cell 1.12 on the GPU (round-2 B1; restore-first). A restore is refused by the engine after admission,
// twice: the payload's body cut one byte short past its header, and the draw forced to exhaust (the
// Layer-1 status seam, KV_POOL_EXHAUSTED: KvPagesExhausted, with its ledger fault). A GPU restore makes a
// new sequence in a free slot, so "the old sequence" is that slot's warm holder: with one slot, every
// vend is that slot. After each refusal the slot keeps its warm holder (holder generation unchanged), and
// its next user generates exactly the tokens it generated before any restore was attempted.
// Mutant: release-first in the slot, which replaces (or loses) the warm holder on a refused restore.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuNoLostStateTest, "SuperSLM.Paging.Gpu.Restore.NoLostState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuNoLostStateTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 0, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Prompt, Reference;
	{
		FSuperSLMGpuSequence Ref;
		if (!MakeTokens(*this, *Rig.Model, 8, 2, Prompt) || !Vend(*this, Gpu, Ref) || !GenerateComplete(*this, Gpu, Ref, Prompt, 16, Reference))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Ref);
	}
	TArray<uint8> Corrupt = B1000;
	if (!TestTrue(TEXT("the payload's body is cut one byte short"), FSuperSLMPagedTestSeam::EditPayload(Corrupt, [](TArray<uint8>& P) { P.SetNum(P.Num() - 1); })))
	{
		return false;
	}
	bool bOk = true;
	struct FLeg { const TCHAR* Name; const TArray<uint8>* Blob; bool bForceExhaust; };
	for (const FLeg& Leg : { FLeg{ TEXT("body corrupted past its header"), &Corrupt, false }, FLeg{ TEXT("draw forced to exhaust"), &B1000, true } })
	{
		const int64 GenBefore = FSuperSLMPagedGpuTestSeam::SlotHolderGeneration(Gpu, 0);
		if (Leg.bForceExhaust)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the status seam is set"), Leg.Name),
				FSuperSLMPagedGpuTestSeam::SetNextLayer1StatusOverride(Gpu, ESuperSLMPagedRequestKind::Restore, SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED));
			AddExpectedError(TEXT("ledger fault"), EAutomationExpectedErrorFlags::Contains, 0);
		}
		const FIssuedGpuRestore R = IssueRestore(Gpu, *Leg.Blob, Rig.Model);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: admitted (the header is sound) (%s)"), Leg.Name, *R.CallError), R.Handle.IsValid());
		FSuperSLMGpuSequence Seq;
		const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, R, Seq);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: refused after admission (got %s)"), Leg.Name, SuperSLML2S2Fixtures::RestoreResultName(Outcome)), (uint8)Outcome,
			(uint8)(Leg.bForceExhaust ? ESuperSLMRestoreResult::KvPagesExhausted : ESuperSLMRestoreResult::Malformed));
		Discard(Gpu, R);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the slot keeps its warm holder"), Leg.Name), FSuperSLMPagedGpuTestSeam::SlotHolderGeneration(Gpu, 0), GenBefore);
		FSuperSLMGpuSequence Next;
		TArray<int32> Tokens;
		bOk &= Vend(*this, Gpu, Next) && GenerateComplete(*this, Gpu, Next, Prompt, 16, Tokens);
		ReturnAndDrain(Gpu, Next);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the slot generates as if no restore was attempted"), Leg.Name), Tokens, Reference);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: H is refunded"), Leg.Name), Charged(Gpu, ERegion::H), int64(0));
	}
	return bOk;
}

// Cell 3.2 (c), the GPU twin of (a) and (b) (audit G5, R2-B3; the strike). Two restores at the executed
// shape are held in flight (190 of 256); a third is admitted as a waiter (66 + 33 + 33 >= 95); a fourth
// has 66 + 33 + 33 + 33 - 95 = 70 < 95 and resolves KvPagesExhausted on arrival. Releasing the first
// hold applies it privately (net hold 62), returning its guaranteed 33: 66 + 33 = 99 >= 95, so the waiter
// is admitted at that apply and succeeds.
// Mutants: a waiter admitted with no guarantee (the fourth waits: it never resolves while the holds are
// in place, and the cell fails on its bound); the waiter admitted before the apply that makes it fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuWaitsTest, "SuperSLM.Paging.Gpu.Restore.Waits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuWaitsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 4, 512, 0, 1))
		|| !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FSuperSLMPagedHold H0 = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
	const FSuperSLMPagedHold H1 = FSuperSLMPagedGpuTestSeam::HoldApply(Gpu, ESuperSLMPagedRequestKind::Restore);
	const FIssuedGpuRestore R0 = IssueRestore(Gpu, B1000, Rig.Model);
	const FIssuedGpuRestore R1 = IssueRestore(Gpu, B1000, Rig.Model);
	if (!TestTrue(TEXT("two restores are held in flight"), WaitHoldReached(Gpu, H0) && WaitHoldReached(Gpu, H1)))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("190 of 256 in flight"), Charged(Gpu, ERegion::H), int64(190));
	const FIssuedGpuRestore R2 = IssueRestore(Gpu, B1000, Rig.Model);
	TickN(*this, Gpu, 4, TEXT("the third arrives"));
	FSuperSLMGpuSequence Seq;
	bOk &= TestEqual(TEXT("the third waits (66 + 33 + 33 >= 95)"), (uint8)OutcomeOf(Gpu, R2, Seq), (uint8)ESuperSLMRestoreResult::Pending);
	bOk &= TestEqual(TEXT("the ledger counts one waiter"), LedgerOf(Gpu).WaitingRestores, 1);
	const int64 HBefore = Charged(Gpu, ERegion::H);
	const FIssuedGpuRestore R3 = IssueRestore(Gpu, B1000, Rig.Model);
	bOk &= ExpectRefusedOnArrival(*this, Gpu, R3, HBefore, TEXT("the fourth (70 < 95)"));
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, H0);
	bOk &= TestEqual(TEXT("the first applies privately"), (uint8)SettleOutcome(Gpu, R0, Seq), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= TestEqual(TEXT("the waiter is admitted at that apply and succeeds (66 + 33 >= 95)"), (uint8)SettleOutcome(Gpu, R2, Seq), (uint8)ESuperSLMRestoreResult::Success);
	FSuperSLMPagedGpuTestSeam::ReleaseHold(Gpu, H1);
	bOk &= TestEqual(TEXT("the second succeeds"), (uint8)SettleOutcome(Gpu, R1, Seq), (uint8)ESuperSLMRestoreResult::Success);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("3.2 (c)"));
	for (const FIssuedGpuRestore* R : { &R0, &R1, &R2, &R3 })
	{
		Discard(Gpu, *R);
	}
	return bOk;
}

// Cell 3.2 (d) and (e) on the GPU (the strike's constructions, rev 1.8 A7): 10 restores at once at the
// executed shape, hinted with a live prefix that does not share -- length 1,001; length 999; one token
// differing inside the full-page span (position 500); a different producer (the token at the last
// full-page position, 991, differs) -- and, (e), a hint that does share (the same 1,000 tokens). With the
// first two applies held: 2 admitted, 1 waiting, 7 refused on arrival, in every construction (the rule
// does not depend on sharing); on release every admitted or waiting restore succeeds -- no waiter ever
// resolves KvPagesExhausted -- and with the sharing hint every apply frees its whole charge (H ends at 0,
// form 1). The strike's whole-reserve ('SSB5') construction has no GPU twin: an 'SLM5' into a pooled
// subsystem is KvModeMismatch (cell 9.6).
// Mutant: a hint's share counted as guaranteed (more waiters, which end short: 7).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuStrikeConstructionsTest, "SuperSLM.Paging.Gpu.Restore.StrikeConstructions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuStrikeConstructionsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	TArray<int32> Tokens1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000, &Tokens1000) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Extra;
	if (!MakeTokens(*this, *Rig.Model, 1, 9, Extra))
	{
		return false;
	}
	struct FConstruction { const TCHAR* Name; TArray<int32> Hint; bool bShares; };
	TArray<FConstruction> Constructions;
	Constructions.Add({ TEXT("hint of length 1,001"), Concat(Tokens1000, Extra), false });
	Constructions.Add({ TEXT("hint of length 999"), TArray<int32>(Tokens1000.GetData(), 999), false });
	Constructions.Add({ TEXT("one token differing in the full-page span"), WithTokenChanged(Tokens1000, 500), false });
	Constructions.Add({ TEXT("a different producer (last full-page position differs)"), WithTokenChanged(Tokens1000, 991), false });
	Constructions.Add({ TEXT("(e) a hint that shares"), Tokens1000, true });
	const FBurstCounts Want = BurstModel(256, Rig.Geo.RestoreCharge(512, 1000), Rig.Geo.Guarantee(95, 512), 10);
	bool bOk = TestTrue(TEXT("the probe model gives 2 admitted, 1 waiting, 7 refused"), Want.Admitted == 2 && Want.Waiting == 1 && Want.Refused == 7);
	for (const FConstruction& Construction : Constructions)
	{
		FSuperSLMPrefix Hint;
		if (!ConfigurePooled(*this, Rig, MakeGpuConfig(Rig.Artifact, 4, 512, 1, 1)) || !BuildPrefix(*this, Gpu, Construction.Hint, Hint))
		{
			return false;
		}
		FBurst Burst;
		bOk &= ExpectCounts(*this, IssueBurst(*this, Gpu, B1000, Rig.Model, Hint, 10, 2, Burst), Want, Construction.Name);
		TArray<FSuperSLMGpuSequence> Settled;
		bOk &= DrainBurst(*this, Gpu, Burst, Construction.Name, &Settled);
		bOk &= NoLedgerFault(*this, Gpu, Construction.Name);
		if (Construction.bShares)
		{
			// Read while every sharing holder is still vended, before DiscardBurst() refunds anything.
			bOk &= TestEqual(TEXT("(e) every apply freed its whole charge: H holds nothing while the sharers live"), Charged(Gpu, ERegion::H), int64(0));
			for (const FSuperSLMGpuSequence& Seq : Settled)
			{
				bOk &= TestEqual(*FString::Printf(TEXT("(e) sharing holder %lld keeps no headroom hold"), Seq.Id), FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(Gpu, Seq), int64(0));
			}
			bOk &= TestEqual(TEXT("(e) every non-refused restore vended a sharing holder"), Settled.Num(), Want.Admitted + Want.Waiting);
			bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("(e) after the burst, sharers live"));
		}
		DiscardBurst(Gpu, Burst);
	}
	return bOk;
}

namespace
{
	// Cell 4.4's legs at one artifact. (a) RestoreHeadroomBlocks = 0: a hinted (sharing) restore at the
	// matching budget is refused on arrival. (b) 1 and 3 blocks: a burst of 10 hinted restores with every
	// apply held; the in-flight count is at most floor(H_blk k / 95) and the admitted, waiting and refused
	// counts match the probe model's; every non-refused one then succeeds. (c) Private holders per block:
	// private restores applied one at a time until the next is refused; ExpectedHolders of them; form 1
	// after each apply (the hold mutant's site); the next refused on arrival; after one holder resets, the
	// same restore admitted.
	bool RunHeadroomLegs(FAutomationTestBase& T, FGpuPagingRig& Rig, const TArray<uint8>& Shared, const TArray<int32>& PrefixTokens, int32 ExpectedHolders,
		int32 HolderSlots)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		const int64 Charge = Rig.Geo.RestoreCharge(512, 1000);
		bool bOk = T.TestEqual(TEXT("the executed charge is 95"), Charge, int64(95));
		// (a)
		{
			FSuperSLMPrefix Hint;
			if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 2, 512, 1, 0)) || !BuildPrefix(T, Gpu, PrefixTokens, Hint))
			{
				return false;
			}
			const FIssuedGpuRestore R = IssueRestore(Gpu, Shared, Rig.Model, Hint);
			bOk &= ExpectRefusedOnArrival(T, Gpu, R, 0, TEXT("(a) RestoreHeadroomBlocks = 0, hinted"));
			Discard(Gpu, R);
		}
		// (b)
		for (const int32 K : { 1, 3 })
		{
			FSuperSLMPrefix Hint;
			if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, 10, 512, 1, K)) || !BuildPrefix(T, Gpu, PrefixTokens, Hint))
			{
				return false;
			}
			const int64 H = Rig.Geo.Hblk() * K;
			const FBurstCounts Want = BurstModel(H, Charge, Rig.Geo.Guarantee(Charge, 512), 10);
			FBurst Burst;
			const FString What = FString::Printf(TEXT("(b) %d block(s)"), K);
			const FBurstCounts Got = IssueBurst(T, Gpu, Shared, Rig.Model, Hint, 10, 10, Burst);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: in flight at most floor(%lld / 95)"), *What, H), Got.Admitted <= H / Charge);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: H carries the in-flight charges"), *What), Charged(Gpu, ESuperSLMPagedRegion::H), int64(Got.Admitted) * Charge);
			bOk &= ExpectCounts(T, Got, Want, *What);
			bOk &= DrainBurst(T, Gpu, Burst, *What);
			DiscardBurst(Gpu, Burst);
		}
		// (c)
		{
			if (!ConfigurePooled(T, Rig, MakeGpuConfig(Rig.Artifact, HolderSlots, 512, 0, 1)))
			{
				return false;
			}
			TArray<FSuperSLMGpuSequence> Holders;
			for (int32 I = 0; I < ExpectedHolders + 1 && I < HolderSlots - 1; ++I)
			{
				const int64 HBefore = Charged(Gpu, ESuperSLMPagedRegion::H);
				const FIssuedGpuRestore R = IssueRestore(Gpu, Shared, Rig.Model);
				FSuperSLMGpuSequence Seq;
				const ESuperSLMRestoreResult Outcome = SettleOutcome(Gpu, R, Seq, 30.0);
				if (Outcome != ESuperSLMRestoreResult::Success)
				{
					bOk &= T.TestEqual(TEXT("(c) the first restore that does not apply is refused on arrival"), (uint8)Outcome, (uint8)ESuperSLMRestoreResult::KvPagesExhausted);
					bOk &= T.TestEqual(TEXT("(c) nothing charged by the refusal"), Charged(Gpu, ESuperSLMPagedRegion::H), HBefore);
					Discard(Gpu, R);
					break;
				}
				Holders.Add(Seq);
				Gpu.ReleaseLifecycleOpHandle(R.Handle);
				bOk &= T.TestEqual(*FString::Printf(TEXT("(c) holder %d holds the net 62 on H"), Holders.Num()), FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(Gpu, Seq), int64(62));
				bOk &= Form1ChargesCoverDrawn(T, Gpu, *FString::Printf(TEXT("(c) after private holder %d's apply"), Holders.Num()));
			}
			bOk &= T.TestEqual(TEXT("(c) private holders per block"), Holders.Num(), ExpectedHolders);
			if (Holders.Num() > 0)
			{
				bOk &= ResetAndWait(T, Gpu, Holders[0]);
				FSuperSLMGpuSequence Again;
				bOk &= ApplyRestore(T, Gpu, Shared, Rig.Model, Again);
			}
			bOk &= NoLedgerFault(T, Gpu, TEXT("(c)"));
		}
		return bOk;
	}
}

// Cell 4.4 at 0.5B (audit G5b, N5; strike F1; rev 1.8 F1): the headroom legs, at H_blk = 256: (b) gives
// 2 / 1 / 7 at one block and 8 / 2 / 0 at three; (c) gives 3 private holders (186 of 256), the next
// refused (70 < 95). The hold mutant (a hold settled one page smaller, 61) leaves the count at 3 (73 < 95),
// so it is killed by form 1 after each apply, where its H charge is one page below the drawn hold.
// Mutants: a flat or absent wait rule (b's counts); a hold settled one page smaller (form 1 in (c)).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuHeadroomTest, "SuperSLM.Paging.Gpu.Restore.Headroom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuHeadroomTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	TArray<int32> Tokens1000;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000, &Tokens1000) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	return RunHeadroomLegs(*this, Rig, B1000, Tokens1000, 3, 5);
}

// Cell 4.4's [two-cap] legs: H_blk = 513 at the 1.5B artifact, ContextCap 4,096. (b) gives 5 / 2 / 3 at
// one block (in flight at most 5) and 10 / 0 / 0 at three (at most 16); (c) gives 7 private holders
// (434 of 513), the next refused (79 < 95). The hold mutant also gives 7 (floor(418/61) + 1), so form 1
// after each apply kills it.
// Mutants: as the 0.5B legs; and blocks of ceil(ContextCap/B) = 256 (2 / 1 / 7, 3 holders).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapHeadroomTest, "SuperSLM.Paging.Gpu.Restore.TwoCap.Headroom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapHeadroomTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B1000;
	TArray<int32> Tokens1000;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !MakeBlobAt(*this, Rig, 512, 1000, 1, B1000, &Tokens1000) || !RequireTwoCapShape(*this, Rig))
	{
		return false;
	}
	const FBurstCounts One = BurstModel(513, 95, 33, 10);
	bool bOk = TestTrue(TEXT("the probe model gives 5 / 2 / 3 at one [two-cap] block (§2)"), One.Admitted == 5 && One.Waiting == 2 && One.Refused == 3);
	bOk &= RunHeadroomLegs(*this, Rig, B1000, Tokens1000, 7, 9);
	return bOk;
}

namespace
{
	// One restore of Blob into an empty H, unhinted and (when HintTokens is non-empty) hinted, each on a
	// fresh Configure: each is admitted and succeeds, and the restored ContextUsed is the header's.
	bool RestoreBothWays(FAutomationTestBase& T, FGpuPagingRig& Rig, const FSuperSLMGpuRuntimeConfig& C, const TArray<uint8>& Blob, const TArray<int32>& HintTokens,
		const FString& What)
	{
		USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
		FGpuPayloadHeaderFields H;
		if (!ReadGpuHeader(T, Blob, H))
		{
			return false;
		}
		bool bOk = true;
		for (const bool bHinted : { false, true })
		{
			if (bHinted && HintTokens.Num() == 0)
			{
				continue;
			}
			FSuperSLMPrefix Hint;
			FSuperSLMGpuSequence Seq;
			if (!ConfigurePooled(T, Rig, C) || (bHinted && !BuildPrefix(T, Gpu, HintTokens, Hint)))
			{
				return false;
			}
			const FString Leg = FString::Printf(TEXT("%s, %s"), *What, bHinted ? TEXT("hinted") : TEXT("unhinted"));
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the charge fits one empty H block"), *Leg), Rig.Geo.RestoreCharge(H.Budget, H.Origin) <= Rig.Geo.Hblk());
			bOk &= ApplyRestore(T, Gpu, Blob, Rig.Model, Seq, Hint);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: ContextUsed is the header's context_length"), *Leg), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), H.ContextLength);
			bOk &= NoLedgerFault(T, Gpu, *Leg);
		}
		return bOk;
	}
}

// Cell 4.5 at 0.5B (audit N20): restore and hint at origins {0, B - 1, B, 1,000, cap - 1} = {0, 15, 16,
// 1,000, 4,095} at budget 512, origin 16 being a page-aligned origin with an empty tail page. Each is
// admitted at the default headroom (one 256-page block) and succeeds, hinted and unhinted (origin 0 has
// no prefix to hint).
// Mutant: a charge or an origin bound off by one page at a page boundary (origin 16 or 4,095 refused).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuOriginsTest, "SuperSLM.Paging.Gpu.Restore.Origins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuOriginsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	bool bOk = true;
	for (const int32 Origin : { 0, 15, 16, 1000, 4095 })
	{
		TArray<uint8> Blob;
		TArray<int32> PrefixTokens;
		if (!MakeBlobAt(*this, Rig, 512, Origin, Origin + 1, Blob, &PrefixTokens) || !RequireAExShape(*this, Rig))
		{
			return false;
		}
		bOk &= RestoreBothWays(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1), Blob, PrefixTokens, FString::Printf(TEXT("origin %d"), Origin));
	}
	return bOk;
}

// Cell 4.5's [two-cap] leg (plan review S1): at origin 4,095 with budget 512 (charge 33 + 255 = 288), and
// at origin and budget both 4,096 (charge 257 + 256 = 513, the largest admissible), each restore is
// admitted into an empty H at RestoreHeadroomBlocks = 1 and succeeds.
// Mutant: rev 1.4's H block of 256, which refuses both on arrival, every time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuTwoCapOriginsTest, "SuperSLM.Paging.Gpu.Restore.TwoCap.Origins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuTwoCapOriginsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	TArray<uint8> B4095, B4096;
	TArray<int32> T4095, T4096;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B) || !MakeBlobAt(*this, Rig, 512, 4095, 1, B4095, &T4095) || !RequireTwoCapShape(*this, Rig)
		|| !MakeBlobAt(*this, Rig, 4096, 4096, 2, B4096, &T4096))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("origin 4,095 at budget 512 is charged 288"), Rig.Geo.RestoreCharge(512, 4095), int64(288));
	bOk &= TestEqual(TEXT("origin = budget = 4,096 is charged 513"), Rig.Geo.RestoreCharge(4096, 4096), int64(513));
	bOk &= RestoreBothWays(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 512, 1, 1), B4095, T4095, TEXT("[two-cap] origin 4,095"));
	bOk &= RestoreBothWays(*this, Rig, MakeGpuConfig(Rig.Artifact, 2, 4096, 1, 1), B4096, T4096, TEXT("[two-cap] origin and budget 4,096"));
	return bOk;
}

// Cells 9.10 (the GPU twins of 9.2 and 9.3, through restore_into_pool; audit G8) and 6.3. The blob is a
// save taken between the adopt of a 1,000-token prefix and its first finish (budget 512, origin 1,000).
//   9.2: a hinted restore re-shares -- the probe shows R(b) = 33 drawn for the holder, not R(b) + E = 95,
//        and its H hold is 0;
//   9.3: hint mismatch (another live prefix of the same length) -- the restore is private: the probe
//        shows 95, and its H hold is 62;
//   6.3: the restored sequence, hinted, unhinted and mismatched, emits the same tokens as an unsaved
//        adopter of the same prefix given the same suffix.
// Exact probe at quiescence: 2 slots, one restored, the other's warm 33, and the live prefixes.
// Mutants: the hint not passed (9.2 draws 95); a mismatched hint shared anyway (9.3's tokens differ);
// the restored position off by one (6.3's tokens differ).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuHintSharingTest, "SuperSLM.Paging.Gpu.Restore.HintSharing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuHintSharingTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 2, 1);
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, C) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FGpuGeometry& G = Rig.Geo;
	TArray<int32> PrefixTokens, OtherTokens, Suffix, Reference;
	FSuperSLMPrefix Prefix;
	TArray<uint8> Blob;
	if (!MakeTokens(*this, *Rig.Model, 1000, 1, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 1000, 2, OtherTokens) || !MakeTokens(*this, *Rig.Model, 5, 3, Suffix)
		|| !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	{
		FSuperSLMGpuSequence Adopter, Twin;
		if (!Vend(*this, Gpu, Adopter) || !AdoptAndWait(*this, Gpu, Adopter, Prefix) || !SaveAndTake(*this, Gpu, Adopter, Blob))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Adopter);
		if (!Vend(*this, Gpu, Twin) || !AdoptAndWait(*this, Gpu, Twin, Prefix) || !GenerateComplete(*this, Gpu, Twin, Suffix, 8, Reference))
		{
			return false;
		}
		ReturnAndDrain(Gpu, Twin);
	}
	const int64 PrefixPages = G.OccupiedPages(1000);
	bool bOk = true;
	struct FLeg { const TCHAR* Name; int32 HintKind; int64 HolderDraw; };
	const FLeg Legs[] = {
		{ TEXT("unhinted"), 0, G.RestoreCharge(512, 1000) },
		{ TEXT("9.2 hinted, sharing"), 1, G.R(512) },
		{ TEXT("9.3 hint mismatch, private"), 2, G.RestoreCharge(512, 1000) },
	};
	for (const FLeg& Leg : Legs)
	{
		FSuperSLMPrefix Other;
		FSuperSLMGpuSequence Seq;
		auto Build = [&]()
		{
			if (!ConfigurePooled(*this, Rig, C) || !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
			{
				return false;
			}
			if (Leg.HintKind == 2 && !BuildPrefix(*this, Gpu, OtherTokens, Other))
			{
				return false;
			}
			const FSuperSLMPrefix Hint = Leg.HintKind == 1 ? Prefix : (Leg.HintKind == 2 ? Other : FSuperSLMPrefix());
			return ApplyRestore(*this, Gpu, Blob, Rig.Model, Seq, Hint);
		};
		if (!Build())
		{
			return false;
		}
		const int64 Live = PrefixPages * (Leg.HintKind == 2 ? 2 : 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the holder's H hold"), Leg.Name), FSuperSLMPagedGpuTestSeam::SequenceHeadroomHold(Gpu, Seq), G.HeadroomHold(Leg.HolderDraw, 512));
		bOk &= ProbeDrawnExactly(*this, Gpu, ExpectedRegions(G, C).Pool(), G.R(512) + Leg.HolderDraw + Live, Build, *FString::Printf(TEXT("%s: the pool's drawn pages"), Leg.Name));
		TArray<int32> Tokens;
		bOk &= GenerateComplete(*this, Gpu, Seq, Suffix, 8, Tokens);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: 6.3, the restored sequence emits the unsaved adopter's tokens"), Leg.Name), Tokens, Reference);
		bOk &= NoLedgerFault(*this, Gpu, Leg.Name);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
