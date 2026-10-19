// Plugin paging plan, GPU red suite (step K5): GPU adopt, the armed mirror and the position oracle
// (§3.4 "Adopt", "The armed flag"; §7 cells 9.4, 10.4, 10.5, 10.6, and §5's empty-prompt row).
//
//   SuperSLM.Paging.Gpu.Armed.Position          cell 10.4 (a): adopters and the non-adopting leg   [K7]
//   SuperSLM.Paging.Gpu.Armed.EngineOnlyTwin    cell 10.4 (b): the engine-only twin               [K7]
//   SuperSLM.Paging.Gpu.Armed.Measurement       cells 10.5 and 6.4: UP-13's measurement (records)  [K7]
//   SuperSLM.Paging.Gpu.Armed.Transitions       cell 10.6                                          [K7]
//   SuperSLM.Paging.Gpu.Armed.Slm6RoundTrip     cell 9.4                                           [K7]
//   SuperSLM.Paging.Gpu.Armed.EmptyPrompt       §5: empty prompt faults unarmed at admission, accepted armed [K7]
//
// The oracle (§7 10.4 (a)) uses no plugin code: the engine's own context_length, read from the
// 'SLM6' header of a save taken at that point (offset 32), equals L + s + n - 1; the plugin's
// ContextUsed equals it; the plugin's armed bit equals the header's ready_for_logits. Every leg runs
// to n >= 2 (audit N1). Every test fails first at ConfigurePooled() while pooled mode is unbuilt.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;

namespace
{
	const TCHAR* PathName(ESuperSLMGpuDecodePath Path)
	{
		return Path == ESuperSLMGpuDecodePath::OneCall ? TEXT("OneCall") : TEXT("Composed");
	}

	// The engine-only twin of cell 10.4 (b), on the subsystem's own pool with every job drained: the
	// prefix built through sslm_gpu_prefix_begin and count-1 sslm_gpu_prefix_prefill calls, frozen,
	// adopted onto a budget-512 sequence the test creates, and decoded N steps with the DecodeStep
	// bridge alone (the engine's ready shortcut produces the first token; each later step embeds the
	// last). No plugin code runs between the build and the decode.
	bool RunEngineOnlyTwin(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& PrefixTokens, int32 N, TArray<int32>& OutTokens,
		int64& OutContextLength)
	{
		OutTokens.Reset();
		OutContextLength = -1;
		FString Error;
		const bool bRan = FSuperSLMPagedGpuTestSeam::RunOnPoolThread(Gpu, [&PrefixTokens, N, &OutTokens, &OutContextLength, &Error](FSuperSLMGpuPoolThreadAccess& A)
		{
			auto Fail = [&Error, &A](const TCHAR* Verb, SslmGpuStatus Failed)
			{
				Error = FString::Printf(TEXT("the engine-only twin: %s returned status %u"), Verb, uint32(Failed));
				A.ReleaseAll();
			};
			SslmGpuStatus Status = A.BeginPrefix(FMath::Max(PrefixTokens.Num(), 1));
			if (Status != SslmGpuStatus::SSLM_OK)
			{
				Fail(TEXT("sslm_gpu_prefix_begin"), Status);
				return;
			}
			const int32 PrefixIndex = A.PrefixCount() - 1;
			for (const int32 Token : PrefixTokens)
			{
				int32 Consumed = 0;
				Status = A.PrefillPrefix(PrefixIndex, Token, Consumed);
				if (Status != SslmGpuStatus::SSLM_OK || Consumed != 1)
				{
					Fail(TEXT("sslm_gpu_prefix_prefill (count 1)"), Status);
					return;
				}
			}
			Status = A.FreezePrefix(PrefixIndex);
			if (Status != SslmGpuStatus::SSLM_OK)
			{
				Fail(TEXT("sslm_gpu_prefix_freeze"), Status);
				return;
			}
			Status = A.CreateBudgeted(512);
			if (Status != SslmGpuStatus::SSLM_OK)
			{
				Fail(TEXT("sslm_gpu_seq_create_budgeted(512)"), Status);
				return;
			}
			const int32 SeqIndex = A.SequenceCount() - 1;
			Status = A.AdoptPrefix(SeqIndex, PrefixIndex);
			if (Status != SslmGpuStatus::SSLM_OK)
			{
				Fail(TEXT("sslm_gpu_seq_adopt_prefix"), Status);
				return;
			}
			int32 Last = PrefixTokens.Num() > 0 ? PrefixTokens.Last() : 0;
			for (int32 I = 0; I < N; ++I)
			{
				int32 Token = -1;
				Status = A.DecodeStep(SeqIndex, Last, Token);
				if (Status != SslmGpuStatus::SSLM_OK)
				{
					Fail(TEXT("the DecodeStep bridge"), Status);
					return;
				}
				OutTokens.Add(Token);
				Last = Token;
			}
			OutContextLength = A.ContextLength(SeqIndex);
			A.ReleaseAll();
		});
		if (!T.TestTrue(TEXT("FSuperSLMPagedGpuTestSeam::RunOnPoolThread runs the engine-only twin"), bRan))
		{
			return false;
		}
		return T.TestTrue(*FString::Printf(TEXT("the engine-only twin runs (%s)"), *Error), Error.IsEmpty() && OutTokens.Num() == N);
	}

	// A save of Seq now, restored, and the restored sequence returned in OutRestored (with its blob).
	bool SaveRestore(FAutomationTestBase& T, FGpuPagingRig& Rig, const FSuperSLMGpuSequence& Seq, TArray<uint8>& OutBlob, FSuperSLMGpuSequence& OutRestored)
	{
		return SaveAndTake(T, *Rig.Gpu, Seq, OutBlob) && ApplyRestore(T, *Rig.Gpu, OutBlob, Rig.Model, OutRestored);
	}
}

// Cell 10.4 (a) (audit B2, N1): after adopt(L) + suffix(s) + n produced tokens, the engine's own
// context_length (from the 'SLM6' header) equals L + s + n - 1, the plugin's ContextUsed equals it
// and its armed bit equals the header's ready_for_logits. L = 1,000; s in {0, 5}; n in {2, 3}; both
// paths. The third leg: a non-adopting pooled sequence with a plain 8-token prompt, n in {2, 3}, both
// paths (8 + n - 1).
// Mutants: the finish bridge on the armed step (one short at n = 2); ContextUsed advanced on the armed
// step (one long in ContextUsed only); the DecodeStep bridge on an unarmed primed step (one long, the
// non-adopting leg).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuArmedPositionTest, "SuperSLM.Paging.Gpu.Armed.Position",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuArmedPositionTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> PrefixTokens, Suffix5, Prompt;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 81, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 5, 82, Suffix5) || !MakeTokens(*this, *Rig.Model, 8, 83, Prompt)
		|| !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	bool bOk = true;
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		for (const int32 S : { 0, 5 })
		{
			for (const int32 N : { 2, 3 })
			{
				const FString What = FString::Printf(TEXT("%s, adopt(1000) + s %d + n %d"), PathName(Path), S, N);
				FSuperSLMGpuSequence Seq;
				TArray<int32> Out;
				if (!Vend(*this, Gpu, Seq, Path) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
				{
					return false;
				}
				bOk &= TestEqual(*FString::Printf(TEXT("%s: armed after the adopt"), *What), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);
				bOk &= GenerateComplete(*this, Gpu, Seq, S > 0 ? Suffix5 : TArray<int32>(), N, Out);
				bOk &= PositionOracle(*this, Gpu, Seq, 1000 + S + N - 1, *What);
				ReturnAndDrain(Gpu, Seq);
			}
		}
		for (const int32 N : { 2, 3 })
		{
			const FString What = FString::Printf(TEXT("%s, non-adopting, prompt 8 + n %d"), PathName(Path), N);
			FSuperSLMGpuSequence Seq;
			TArray<int32> Out;
			if (!Vend(*this, Gpu, Seq, Path))
			{
				return false;
			}
			bOk &= TestEqual(*FString::Printf(TEXT("%s: unarmed at vend"), *What), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
			bOk &= GenerateComplete(*this, Gpu, Seq, Prompt, N, Out);
			bOk &= PositionOracle(*this, Gpu, Seq, 8 + N - 1, *What);
			ReturnAndDrain(Gpu, Seq);
		}
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	return bOk;
}

// Cell 10.4 (b) (audit B2): the engine-only twin, suffix empty. The test module builds the prefix
// through the same arc with count-1 chunks, adopts it onto a pooled sequence it creates itself, and
// decodes with the DecodeStep bridge alone; the plugin's adopter of the same 1,000-token prefix, with
// an empty suffix, must emit the same tokens on both paths. The twin's engine context_length, where
// the accessor is reachable from this build (§13 item 13), equals 1,000 + n - 1. The non-empty-suffix
// leg is not authored: it needs UP-13's measurement to show the arc and the per-token feed identical,
// and that measurement is unreadable here (the K5 record, finding F1); (a) grades it.
// Mutants: the finish bridge on the armed step (the plugin's first token is a second finish: the
// sequences diverge at once); an adopt that re-feeds the prefix's last token (diverge).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuEngineOnlyTwinTest, "SuperSLM.Paging.Gpu.Armed.EngineOnlyTwin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuEngineOnlyTwinTest::RunTest(const FString& Parameters)
{
	// P has room for the plugin's prefix and the twin's (R(1000) = 64 each); H's free 256 pages hold the
	// twin's budget-512 sequence.
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 2, 1)) || !RequireAExShape(*this, Rig))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	constexpr int32 N = 8;
	TArray<int32> PrefixTokens, Twin;
	int64 TwinContext = -1;
	if (!MakeTokens(*this, *Rig.Model, 1000, 91, PrefixTokens) || !RunEngineOnlyTwin(*this, Gpu, PrefixTokens, N, Twin, TwinContext))
	{
		return false;
	}
	bool bOk = true;
	if (TwinContext >= 0)
	{
		bOk &= TestEqual(TEXT("the twin's engine context_length is 1,000 + n - 1"), TwinContext, int64(1000 + N - 1));
	}
	else
	{
		AddInfo(TEXT("SslmGpuSeqHandleContextLengthForBench is not reachable from this build; the twin's position is not read (K0 leg (a))"));
	}
	FSuperSLMPrefix Prefix;
	if (!BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	for (const ESuperSLMGpuDecodePath Path : { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed })
	{
		FSuperSLMGpuSequence Seq;
		TArray<int32> Out;
		if (!Vend(*this, Gpu, Seq, Path) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
		{
			return false;
		}
		bOk &= GenerateComplete(*this, Gpu, Seq, TArray<int32>(), N, Out);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the plugin's adopter emits the engine-only twin's tokens"), PathName(Path)), Out, Twin);
		bOk &= PositionOracle(*this, Gpu, Seq, 1000 + N - 1, PathName(Path));
		ReturnAndDrain(Gpu, Seq);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 10.4 (b)"));
	return bOk;
}

// Cells 10.5 and 6.4: UP-13's measurement. K0 found no public GPU logit readback (§13 item 13), and
// the comparison has no second side: adopt copies the prefix's prefill-final-hidden snapshot, but the
// per-token twin's feed never writes one (the K5 record, finding F1; plan rev 1.9 ruling 8). 10.5
// therefore grades nothing on the GPU: 10.4 (a) carries the position claim, and this cell RECORDS
// token agreement for the release notes. Over 8 texts at
// the adopter cells' split (prefix 1,000, suffix 5), the adopter's tokens are compared with those of a
// pooled no-prefix twin fed prefix + suffix as its prompt (budget 2,048, so the twin's limit admits
// it); the agreement (the first divergent position, or "all") is logged per text. The cell fails only
// if a leg does not run.
// Mutant: none is graded here; the fallback's grading is 10.4 (a)'s.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuArmedMeasurementTest, "SuperSLM.Paging.Gpu.Armed.Measurement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuArmedMeasurementTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 2048, 1, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	constexpr int32 Texts = 8;
	constexpr int32 N = 16;
	bool bOk = true;
	int32 FullAgreement = 0;
	for (int32 Text = 0; Text < Texts; ++Text)
	{
		TArray<int32> PrefixTokens, Suffix, Adopted, Twin;
		FSuperSLMPrefix Prefix;
		FSuperSLMGpuSequence Seq;
		if (!MakeTokens(*this, *Rig.Model, 1000, 100 + Text, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 5, 200 + Text, Suffix) || !BuildPrefix(*this, Gpu, PrefixTokens, Prefix)
			|| !Vend(*this, Gpu, Seq) || !AdoptAndWait(*this, Gpu, Seq, Prefix))
		{
			return false;
		}
		bOk &= GenerateComplete(*this, Gpu, Seq, Suffix, N, Adopted);
		ReturnAndDrain(Gpu, Seq);
		bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
		if (!Vend(*this, Gpu, Seq))
		{
			return false;
		}
		bOk &= GenerateComplete(*this, Gpu, Seq, Concat(PrefixTokens, Suffix), N, Twin);
		ReturnAndDrain(Gpu, Seq);
		int32 FirstDiff = INDEX_NONE;
		for (int32 I = 0; I < FMath::Min(Adopted.Num(), Twin.Num()); ++I)
		{
			if (Adopted[I] != Twin[I])
			{
				FirstDiff = I;
				break;
			}
		}
		FullAgreement += (FirstDiff == INDEX_NONE && Adopted.Num() == Twin.Num()) ? 1 : 0;
		AddInfo(FString::Printf(TEXT("cell 10.5, text %d: adopter vs per-token twin: %s"), Text,
			FirstDiff == INDEX_NONE ? TEXT("all tokens agree") : *FString::Printf(TEXT("first divergence at token %d"), FirstDiff)));
	}
	AddInfo(FString::Printf(TEXT("cell 10.5: %d of %d texts agree on every token (recorded, not graded; finding F1)"), FullAgreement, Texts));
	return bOk;
}

// Cell 10.6 (audit G4): the armed transitions, each graded by 10.4 (a)'s oracle. At (1 slot, budget
// 512, 4 prefix blocks, 1 headroom block), with prefixes P208 (208 tokens), P96 (96), E0 (empty), and
// a nested child of P208 that is empty (C0) and one of 5 tokens (C5):
//   - reset while armed: 0, unarmed;
//   - re-adopt while armed (no reset between; reading R9) onto E0: 0, unarmed; then a 4-token prompt
//     and n = 3: 6 (the DecodeStep-bridge-on-unarmed mutant's site);
//   - re-adopt while armed onto P96: 96, armed; then n = 3 with an empty suffix: 98;
//   - restore of an unarmed blob into the previously armed slot, and of an armed blob into the
//     previously unarmed one (BlockCount 1, so the slot is the same): armed equals the header's
//     ready word, and the position is the header's;
//   - adopt of a nested child: C0 takes its parent's last write (armed, 208), C5 its own (armed, 213);
//   - an adopt faulted after the share (§3.4's seam, ALLOCATION_FAILED; reading R7): the adopt resolves
//     non-Success naming the status, and the oracle reads 0, unarmed, with the engine's context_length 0.
// The schema dead end on the armed step is deferred (the K5 record: no deterministic construction).
// Mutant: armed not cleared at reset (and at re-adopt onto E0, and at the fault after the share).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuArmedTransitionsTest, "SuperSLM.Paging.Gpu.Armed.Transitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuArmedTransitionsTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 4, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> T208, T96, T5, Prompt4;
	FSuperSLMPrefix P208, P96, E0;
	if (!MakeTokens(*this, *Rig.Model, 208, 301, T208) || !MakeTokens(*this, *Rig.Model, 96, 302, T96) || !MakeTokens(*this, *Rig.Model, 5, 303, T5)
		|| !MakeTokens(*this, *Rig.Model, 4, 304, Prompt4) || !BuildPrefix(*this, Gpu, T208, P208) || !BuildPrefix(*this, Gpu, T96, P96)
		|| !BuildPrefix(*this, Gpu, TArray<int32>(), E0))
	{
		return false;
	}
	bool bOk = true;
	FSuperSLMGpuSequence Seq;
	TArray<int32> Out;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}

	// Reset while armed.
	bOk &= AdoptAndWait(*this, Gpu, Seq, P208) && PositionOracle(*this, Gpu, Seq, 208, TEXT("adopted P208"));
	bOk &= ResetAndWait(*this, Gpu, Seq) && PositionOracle(*this, Gpu, Seq, 0, TEXT("reset while armed"));
	bOk &= TestEqual(TEXT("reset while armed: unarmed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);

	// Re-adopt while armed, onto an empty prefix, then generate.
	bOk &= AdoptAndWait(*this, Gpu, Seq, P208);
	bOk &= AdoptAndWait(*this, Gpu, Seq, E0) && PositionOracle(*this, Gpu, Seq, 0, TEXT("re-adopt while armed onto E0"));
	bOk &= TestEqual(TEXT("re-adopt onto E0: unarmed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
	bOk &= GenerateComplete(*this, Gpu, Seq, Prompt4, 3, Out) && PositionOracle(*this, Gpu, Seq, 4 + 3 - 1, TEXT("E0 adopter, prompt 4, n 3"));

	// Re-adopt while armed, onto a non-empty prefix, then generate.
	bOk &= ResetAndWait(*this, Gpu, Seq);
	bOk &= AdoptAndWait(*this, Gpu, Seq, P208);
	bOk &= AdoptAndWait(*this, Gpu, Seq, P96) && PositionOracle(*this, Gpu, Seq, 96, TEXT("re-adopt while armed onto P96"));
	bOk &= TestEqual(TEXT("re-adopt onto P96: armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);
	bOk &= GenerateComplete(*this, Gpu, Seq, TArray<int32>(), 3, Out) && PositionOracle(*this, Gpu, Seq, 96 + 3 - 1, TEXT("P96 adopter, n 3"));

	// Restores across the armed bit, through the one slot.
	TArray<uint8> Unarmed, Armed;
	bOk &= SaveAndTake(*this, Gpu, Seq, Unarmed); // after a completed generation: not ready
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, P208) && SaveAndTake(*this, Gpu, Seq, Armed); // between adopt and first finish: ready
	FGpuPayloadHeaderFields UH, AH;
	if (!ReadGpuHeader(*this, Unarmed, UH) || !ReadGpuHeader(*this, Armed, AH))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the unarmed blob's ready word is 0"), UH.Ready, int64(0));
	bOk &= TestTrue(TEXT("the armed blob's ready word is non-zero"), AH.Ready != 0);
	// The slot is armed now (adopted P208): restore the unarmed blob into it.
	ReturnAndDrain(Gpu, Seq);
	FSuperSLMGpuSequence Restored;
	if (!ApplyRestore(*this, Gpu, Unarmed, Rig.Model, Restored))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the unarmed blob restores unarmed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Restored), 0);
	bOk &= PositionOracle(*this, Gpu, Restored, UH.ContextLength, TEXT("unarmed blob into the armed slot"));
	ReturnAndDrain(Gpu, Restored);
	// The slot's last holder was unarmed: restore the armed blob into it.
	if (!ApplyRestore(*this, Gpu, Armed, Rig.Model, Restored))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the armed blob restores armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Restored), 1);
	bOk &= PositionOracle(*this, Gpu, Restored, AH.ContextLength, TEXT("armed blob into the unarmed slot"));
	bOk &= TestEqual(TEXT("the armed blob's position is 208"), AH.ContextLength, int64(208));
	bOk &= GenerateComplete(*this, Gpu, Restored, TArray<int32>(), 3, Out) && PositionOracle(*this, Gpu, Restored, 208 + 3 - 1, TEXT("restored armed, n 3"));
	ReturnAndDrain(Gpu, Restored);

	// Adopt of a nested child: empty (its parent's last write) and of 5 tokens (its own).
	bOk &= ReleasePrefixAndDrain(*this, Gpu, P96) && ReleasePrefixAndDrain(*this, Gpu, E0);
	FSuperSLMPrefix C0, C5;
	if (!BuildNested(*this, Gpu, P208, TArray<int32>(), C0) || !BuildNested(*this, Gpu, P208, T5, C5) || !Vend(*this, Gpu, Seq))
	{
		return false;
	}
	bOk &= AdoptAndWait(*this, Gpu, Seq, C0) && PositionOracle(*this, Gpu, Seq, 208, TEXT("adopt of the empty nested child"));
	bOk &= TestEqual(TEXT("the empty child takes its parent's last write: armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);
	bOk &= ResetAndWait(*this, Gpu, Seq);
	bOk &= AdoptAndWait(*this, Gpu, Seq, C5) && PositionOracle(*this, Gpu, Seq, 213, TEXT("adopt of the 5-token nested child"));
	bOk &= TestEqual(TEXT("the 5-token child: armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 1);

	// An adopt faulted after the share.
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, P208);
	bOk &= TestTrue(TEXT("ArmAdoptFaultAfterShare arms"), FSuperSLMPagedGpuTestSeam::ArmAdoptFaultAfterShare(Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED));
	FString FaultError;
	const ESuperSLMRestoreResult FaultResult = AdoptResult(Gpu, Seq, C5, &FaultError);
	bOk &= TestTrue(*FString::Printf(TEXT("the faulted adopt resolves non-Success (got %s)"), SuperSLML2S2Fixtures::RestoreResultName(FaultResult)),
		FaultResult != ESuperSLMRestoreResult::Success && FaultResult != ESuperSLMRestoreResult::Pending);
	bOk &= TestTrue(*FString::Printf(TEXT("the error names the status (%s)"), *FaultError), FaultError.Contains(TEXT("SSLM_GPU_ALLOCATION_FAILED")));
	bOk &= PositionOracle(*this, Gpu, Seq, 0, TEXT("the adopt faulted after the share"));
	bOk &= TestEqual(TEXT("faulted after the share: ContextUsed 0"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), int64(0));
	bOk &= TestEqual(TEXT("faulted after the share: unarmed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), 0);
	ReturnAndDrain(Gpu, Seq);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, C5) && ReleasePrefixAndDrain(*this, Gpu, C0) && ReleasePrefixAndDrain(*this, Gpu, P208);
	return bOk;
}

// Cell 9.4: 'SLM6' round trips. (i) Mid-turn (plan rev 1.10; K7 code review T1): an adopter of a
// 1,000-token prefix begins one turn, a 5-token suffix and 9 new tokens. Once 3 of its tokens have been
// applied, it is saved inside that turn, and the blob is restored. The save is admitted at the sequence's queue head
// while the turn runs, and it records the tokens the game has applied (k) plus the thread's carried
// results (§2.5 row 24). The plan's cell reads "saved after 3 tokens have applied ... each produce the
// remaining 6". The cell requests the save as soon as 3 have applied. No seam pauses token application
// between that request and the save's finalize, so k, the count at the finalize, is at least 3 and
// depends on tick timing. The cell reads k from the outcome and compares the remaining 9 - k, which is 6
// when the save finalizes at 3 (the K5 record, §12).
//   - Right after the restore, the restored sequence's armed bit equals the payload's ready word, and its
//     ContextUsed equals the payload's context_length.
//   - The restored sequence continues the same turn with no new request, and completes. It emits exactly
//     the original's remaining 9 - k tokens, with 1 <= 9 - k <= 6, so the save was mid-turn. Those
//     tokens equal the unsaved original's last 9 - k.
//   - Both sequences end at 10.4's oracle position 1,000 + 5 + 9 - 1 = 1,013.
// The first draft saved a completed turn and then asked it to continue, which the subsystem refuses
// ("a generation that continues a completed one is not supported"), so that leg could never pass.
// (ii) A save taken between the adopt and the first finish: armed, position 1,000; the restored sequence's first tokens (empty prompt, 6 new) equal the unsaved
// adopter's, and the oracle holds at 1,005.
// Mutants: armed taken from a wrapper field or defaulted at restore (the restored adopter is refused an
// empty prompt, or re-finishes); the restore's origin dropped (the oracle and the continuation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuSlm6RoundTripTest, "SuperSLM.Paging.Gpu.Armed.Slm6RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuSlm6RoundTripTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> PrefixTokens, Suffix;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 401, PrefixTokens) || !MakeTokens(*this, *Rig.Model, 5, 402, Suffix)
		|| !BuildPrefix(*this, Gpu, PrefixTokens, Prefix))
	{
		return false;
	}
	bool bOk = true;
	{
		FSuperSLMGpuSequence Original, Restored;
		TArray<uint8> Blob;
		if (!Vend(*this, Gpu, Original) || !AdoptAndWait(*this, Gpu, Original, Prefix))
		{
			return false;
		}
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Suffix;
		Request.MaxNewTokens = 9;
		const FSuperSLMLifecycleOpHandle Turn = Gpu.RequestBeginGeneration(Original, Request);
		if (!TestTrue(*FString::Printf(TEXT("(i) the 9-token turn is accepted (%s)"), *Gpu.GetLastLifecycleRequestError()), Turn.IsValid()))
		{
			return false;
		}
		// Save inside the turn: requested as soon as 3 of its tokens have been applied.
		Pump(Gpu, [&Gpu, &Original]()
		{
			const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Original);
			return Gpu.GetGeneratedTokens(Original).Num() >= 3 || Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
		}, GenerateSeconds);
		const int32 AppliedAtRequest = Gpu.GetGeneratedTokens(Original).Num();
		if (!TestTrue(*FString::Printf(TEXT("(i) the save is requested mid-turn (%d of 9 applied, phase %d)"), AppliedAtRequest, int32(Gpu.GetPhase(Original))),
				AppliedAtRequest >= 3 && AppliedAtRequest < 9 && Gpu.GetPhase(Original) != ESuperSLMSequencePhase::Complete && Gpu.GetPhase(Original) != ESuperSLMSequencePhase::Faulted)
			|| !SaveRestore(*this, Rig, Original, Blob, Restored))
		{
			return false;
		}
		FGpuPayloadHeaderFields H;
		bOk &= ReadGpuHeader(*this, Blob, H);
		bOk &= TestEqual(TEXT("(i) armed equals the payload's ready word"), int64(FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Restored)), int64(H.Ready != 0 ? 1 : 0));
		bOk &= TestEqual(TEXT("(i) the restored ContextUsed equals the payload's context_length"), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Restored), H.ContextLength);
		// Both finish the turn: the original from where it was, the restored one with no new request.
		auto Finished = [&Gpu](const FSuperSLMGpuSequence& Seq)
		{
			const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
			return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Gpu.GetPendingLifecycleOperationCount(Seq) == 0;
		};
		Pump(Gpu, [&Finished, &Original, &Restored]() { return Finished(Original) && Finished(Restored); }, GenerateSeconds);
		Gpu.ReleaseLifecycleOpHandle(Turn);
		bOk &= TestEqual(TEXT("(i) the original completes its turn"), (uint8)Gpu.GetPhase(Original), (uint8)ESuperSLMSequencePhase::Complete);
		bOk &= TestEqual(TEXT("(i) the restored sequence completes the same turn"), (uint8)Gpu.GetPhase(Restored), (uint8)ESuperSLMSequencePhase::Complete);
		const TArray<int32> Whole = Gpu.GetGeneratedTokens(Original);
		const TArray<int32> Rest = Gpu.GetGeneratedTokens(Restored);
		bOk &= TestEqual(TEXT("(i) the original emits its 9 tokens"), Whole.Num(), 9);
		const int32 Saved = 9 - Rest.Num(); // k: the tokens the game had applied when the save was taken
		AddInfo(FString::Printf(TEXT("(i) the save was taken with %d of 9 tokens applied; the restored sequence emitted the remaining %d"), Saved, Rest.Num()));
		bOk &= TestTrue(*FString::Printf(TEXT("(i) the save was mid-turn, after the 3rd token (k = %d, 3 <= k <= 8)"), Saved), Saved >= 3 && Saved <= 8);
		if (Whole.Num() == 9 && Saved >= 0 && Saved <= 9)
		{
			const TArray<int32> Tail(Whole.GetData() + Saved, 9 - Saved);
			bOk &= TestEqual(TEXT("(i) the restored continuation equals the unsaved original's remaining tokens"), Rest, Tail);
		}
		bOk &= PositionOracle(*this, Gpu, Original, 1000 + 5 + 9 - 1, TEXT("(i) the original at the turn's end"));
		bOk &= PositionOracle(*this, Gpu, Restored, 1000 + 5 + 9 - 1, TEXT("(i) the restored sequence at the turn's end"));
		ReturnAndDrain(Gpu, Original);
		ReturnAndDrain(Gpu, Restored);
	}
	{
		FSuperSLMGpuSequence Original, Restored;
		TArray<int32> Expected, Got;
		TArray<uint8> Blob;
		if (!Vend(*this, Gpu, Original) || !AdoptAndWait(*this, Gpu, Original, Prefix) || !SaveRestore(*this, Rig, Original, Blob, Restored))
		{
			return false;
		}
		FGpuPayloadHeaderFields H;
		bOk &= ReadGpuHeader(*this, Blob, H);
		bOk &= TestTrue(TEXT("(ii) the save between adopt and first finish is ready"), H.Ready != 0);
		bOk &= TestEqual(TEXT("(ii) the restored sequence is armed"), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Restored), 1);
		bOk &= TestEqual(TEXT("(ii) its position is the prefix's"), H.ContextLength, int64(1000));
		bOk &= GenerateComplete(*this, Gpu, Original, TArray<int32>(), 6, Expected) && GenerateComplete(*this, Gpu, Restored, TArray<int32>(), 6, Got);
		bOk &= TestEqual(TEXT("(ii) the restored adopter's tokens equal the unsaved adopter's"), Got, Expected);
		bOk &= PositionOracle(*this, Gpu, Restored, 1000 + 6 - 1, TEXT("(ii) restored armed, n 6"));
		ReturnAndDrain(Gpu, Original);
		ReturnAndDrain(Gpu, Restored);
	}
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Prefix);
	return bOk;
}

// §5 (audit G1) and plan rev 1.9 ruling 6 (§3.4): in pooled mode the empty-prompt check runs when the
// BeginGeneration op is admitted at its queue head, not at the call. On an unarmed pooled sequence (a
// fresh one, and one that adopted an empty prefix) the request gets a valid handle, and at admission
// the sequence faults RecoverablePerSequenceRejection before any Layer-1 call: no token, and
// ContextUsed and the armed bit unchanged (no seam counts generation calls, so these are what grade
// "before any Layer-1 call"). An armed adopter of a non-empty prefix is accepted and emits its tokens.
// The dedicated leg keeps today's call-time refusal: an invalid handle.
// Mutants: the lift for every sequence (an unarmed sequence would begin with nothing primed); the lift
// absent (the armed adopter is refused); the pooled check left at the call (an invalid handle for the
// unarmed legs); the check run after a Layer-1 call (a token, or ContextUsed advanced).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuEmptyPromptTest, "SuperSLM.Paging.Gpu.Armed.EmptyPrompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuEmptyPromptTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 2, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix, Empty;
	if (!MakeTokens(*this, *Rig.Model, 208, 501, Tokens) || !BuildPrefix(*this, Gpu, Tokens, Prefix) || !BuildPrefix(*this, Gpu, TArray<int32>(), Empty))
	{
		return false;
	}
	bool bOk = true;
	FSuperSLMGpuSequence Seq;
	FGpuGeneration Run;
	if (!Vend(*this, Gpu, Seq))
	{
		return false;
	}
	// An unarmed pooled sequence: a valid handle, then the fault at admission, before any Layer-1 call.
	auto ExpectFaultAtAdmission = [this, &Gpu, &Seq, &Run](const TCHAR* What)
	{
		const int64 UsedBefore = FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq);
		const int32 ArmedBefore = FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq);
		bool bLeg = Generate(*this, Gpu, Seq, TArray<int32>(), 3, Run);
		bLeg &= TestFalse(*FString::Printf(TEXT("%s: the empty prompt is accepted at the call (a valid handle; %s)"), What, *Run.Error), Run.bRefusedAtCall);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: at admission the sequence is Faulted"), What), (uint8)Run.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: by name, RecoverablePerSequenceRejection (got %s)"), What, FaultName(Run.Fault)),
			(uint8)Run.Fault, (uint8)ESuperSLMGpuFaultReason::RecoverablePerSequenceRejection);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: no token"), What), Run.Tokens.Num(), 0);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: ContextUsed unchanged (no Layer-1 call)"), What), FSuperSLMPagedGpuTestSeam::SequenceContextUsed(Gpu, Seq), UsedBefore);
		bLeg &= TestEqual(*FString::Printf(TEXT("%s: the armed bit unchanged"), What), FSuperSLMPagedGpuTestSeam::SequenceArmed(Gpu, Seq), ArmedBefore);
		return bLeg;
	};
	bOk &= ExpectFaultAtAdmission(TEXT("a fresh (unarmed) sequence"));
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, Empty);
	bOk &= ExpectFaultAtAdmission(TEXT("an adopter of an empty prefix (unarmed)"));
	bOk &= ResetAndWait(*this, Gpu, Seq) && AdoptAndWait(*this, Gpu, Seq, Prefix);
	bOk &= Generate(*this, Gpu, Seq, TArray<int32>(), 3, Run);
	bOk &= TestFalse(*FString::Printf(TEXT("an armed adopter: the empty prompt is accepted (%s)"), *Run.Error), Run.bRefusedAtCall);
	bOk &= TestEqual(TEXT("the armed adopter completes"), (uint8)Run.Phase, (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= TestEqual(TEXT("the armed adopter emits 3 tokens"), Run.Tokens.Num(), 3);
	ReturnAndDrain(Gpu, Seq);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, Empty) && ReleasePrefixAndDrain(*this, Gpu, Prefix);

	// Dedicated mode keeps the call-time refusal.
	if (!ConfigureDedicated(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 0, 0, 0)))
	{
		return false;
	}
	FSuperSLMGpuSequence Dedicated;
	if (!Vend(*this, Gpu, Dedicated))
	{
		return false;
	}
	bOk &= Generate(*this, Gpu, Dedicated, TArray<int32>(), 3, Run);
	bOk &= TestTrue(*FString::Printf(TEXT("dedicated: the empty prompt is refused at the call (an invalid handle; %s)"), *Run.Error), Run.bRefusedAtCall);
	bOk &= TestEqual(TEXT("dedicated: no token"), Run.Tokens.Num(), 0);
	ReturnAndDrain(Gpu, Dedicated);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
