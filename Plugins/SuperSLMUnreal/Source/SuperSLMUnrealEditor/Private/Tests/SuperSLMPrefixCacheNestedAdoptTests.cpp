// The nested cache adopt's length (plugin 1.3.0 review round, blocking item 1). A nested lease's spec carries only the
// suffix after its parent lease, while the engine's nested prefix holds parent plus suffix. The adopt sets the
// sequence's context, the ledger's origin and the context-cap check from the lease's length, so that length must be
// the whole one. Read as the suffix alone, the first skip-ahead pass replaced the context with the engine's real
// length, the ledger's limit (suffix + SequenceBudget) was crossed and the generation faulted, and the cap check
// passed prompts the engine then refused.
//
//   SuperSLM.PrefixCache.Cpu.Nested.AdoptFullLength   budget mode: a root lease and a nested child, the child
//                                                     adopted and generated Off, Exact and Canonical: the context
//                                                     after the adopt is parent + suffix, no run faults but at its
//                                                     schema's end, Exact equals Off, and Off over the child equals
//                                                     Off from the whole prompt
//   SuperSLM.PrefixCache.Cpu.Nested.AdoptCapFullLength the context-cap check after a nested adopt counts the whole
//                                                     length: a prompt that fits after the suffix alone but not
//                                                     after parent + suffix is refused by name at its activation
//
// A-EX under potion_shop_order, as the skip-ahead cells (SuperSLMSchemaJumpFixtures.h).

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMSchemaJumpFixtures.h"
#include "SuperSLMSchemaJumpReviewTestAccess.h"
#include "SuperSLMSchemaJumpTestSeam.h"
#include "SuperSLMSequenceTypes.h"
#include "Tests/AutomationCommon.h"

namespace SuperSLMPrefixCacheNestedAdoptTests
{
	using namespace SuperSLMSchemaJumpFixtures;

	// Budget mode with the cache: two sequences, a 512-token sequence budget, 64 MiB of cache.
	FSuperSLMRuntimeConfig NestedAdoptConfig()
	{
		FSuperSLMRuntimeConfig Config = BaseConfig(2, 2);
		Config.SequenceBudget = 512;
		Config.RestoreHeadroomBlocks = 1;
		Config.CacheGhostBlocks = 1;
		Config.PrefixCacheBudgetBytes = int64(64) << 20;
		return Config;
	}

	bool WaitReady(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMPrefix& Lease, const TCHAR* What)
	{
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [&]()
		{
			const ESuperSLMPrefixPhase P = Rig.Cpu->GetPrefixPhase(Lease);
			return P != ESuperSLMPrefixPhase::Pending && P != ESuperSLMPrefixPhase::Prefilling;
		}, 120.0);
		return T.TestEqual(*FString::Printf(TEXT("%s is Ready (lease state %d)"), What, (int32)Rig.Cpu->GetPrefixLeaseState(Lease)),
			(uint8)Rig.Cpu->GetPrefixPhase(Lease), (uint8)ESuperSLMPrefixPhase::Ready);
	}

	// A root lease over the first half of PromptTokens under the schema, and a nested child over the rest.
	bool MakeNestedLeases(FAutomationTestBase& T, const FCpuRig& Rig, const TArray<int32>& PromptTokens,
		FSuperSLMPrefix& OutRoot, FSuperSLMPrefix& OutChild, int32& OutRootLength, int32& OutSuffixLength)
	{
		OutRootLength = PromptTokens.Num() / 2;
		OutSuffixLength = PromptTokens.Num() - OutRootLength;
		if (!T.TestTrue(TEXT("the prompt splits into two nonempty parts"), OutRootLength > 0 && OutSuffixLength > 0)) { return false; }
		FSuperSLMPrefixSpec Root;
		Root.Tokens.Append(PromptTokens.GetData(), OutRootLength);
		Root.SchemaName = FName(AExSchemaName());
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("RequestPrefix, root (%s)"), *Error), Rig.Cpu->RequestPrefix(Root, OutRoot, Error)) ||
			!WaitReady(T, Rig, OutRoot, TEXT("the root lease"))) { return false; }
		FSuperSLMPrefixSpec Child;
		Child.Tokens.Append(PromptTokens.GetData() + OutRootLength, OutSuffixLength);
		Child.Parent = OutRoot;
		return T.TestTrue(*FString::Printf(TEXT("RequestPrefix, nested child (%s)"), *Error), Rig.Cpu->RequestPrefix(Child, OutChild, Error)) &&
			WaitReady(T, Rig, OutChild, TEXT("the nested lease"));
	}

	// Vends, binds the schema, adopts Lease (drained) and returns the context the subsystem then holds.
	bool VendAndAdopt(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMPrefix& Lease, FSuperSLMSequence& OutSeq, int64& OutContext)
	{
		FString Error;
		if (!T.TestEqual(TEXT("vend"), (uint8)Rig.Cpu->VendSequence(OutSeq), (uint8)ESuperSLMVendResult::Success)) { return false; }
		if (!T.TestTrue(*FString::Printf(TEXT("schema bind before adopting (%s)"), *Error), Rig.Cpu->SetSchema(OutSeq, Rig.Schema, Error))) { return false; }
		FSuperSLMLifecycleOpHandle Handle;
		if (!T.TestEqual(*FString::Printf(TEXT("AdoptPrefix queues (%s)"), *Error), (uint8)Rig.Cpu->AdoptPrefix(OutSeq, Lease, Handle, Error),
			(uint8)ESuperSLMRestoreResult::Success)) { return false; }
		DrainTicks(*Rig.Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
			[&]() { return Rig.Cpu->GetLifecycleOpResult(Handle) != ESuperSLMRestoreResult::Pending; }, 60.0);
		const ESuperSLMRestoreResult Adopted = Rig.Cpu->GetLifecycleOpResult(Handle);
		Rig.Cpu->ReleaseLifecycleOpHandle(Handle);
		OutContext = FSuperSLMSchemaJumpReviewTestAccess::GetContextUsed(*Rig.Cpu, OutSeq);
		return T.TestEqual(TEXT("the adoption succeeds"), (uint8)Adopted, (uint8)ESuperSLMRestoreResult::Success);
	}

	// Adopts Lease and runs a no-prompt generation in Mode to its end, then returns the sequence.
	bool RunOverLease(FAutomationTestBase& T, const FCpuRig& Rig, const FSuperSLMPrefix& Lease, ESuperSLMSchemaJumpForward Mode,
		int32 MaxNewTokens, FRunResult& Out, int64& OutContextAfterAdopt)
	{
		Out = FRunResult();
		if (!VendAndAdopt(T, Rig, Lease, Out.Sequence, OutContextAfterAdopt)) { return false; }
		FSuperSLMGenerationRequest Req;
		Req.MaxNewTokens = MaxNewTokens;
		Req.SchemaJumpForward = Mode;
		RunGenerationToCompletion(*Rig.Cpu, Out.Sequence, Req, Out.Tokens, kRunSeconds, Out.Error);
		Out.Phase = Rig.Cpu->GetPhase(Out.Sequence);
		Out.Tokens = Rig.Cpu->GetGeneratedTokens(Out.Sequence);
		Out.Outcome = Rig.Cpu->GetLastDecodeOutcome(Out.Sequence);
		Out.Stats = Rig.Cpu->GetStats(Out.Sequence);
		Rig.Cpu->ReturnSequence(Out.Sequence);
		Settle(*Rig.Cpu);
		return true;
	}

	// The run ended as a schema-bound generation should: Complete, or Faulted with the documented Schema Dead End close.
	bool EndedCleanly(FAutomationTestBase& T, const FRunResult& R, const TCHAR* What)
	{
		const bool bClean = R.Phase == ESuperSLMSequencePhase::Complete ||
			(R.Phase == ESuperSLMSequencePhase::Faulted && R.Outcome == ESuperSLMDecodeOutcome::SchemaDeadEnd);
		return T.TestTrue(*FString::Printf(TEXT("%s: ends Complete or at its schema's end, not by a fault (phase %d, outcome %d, %d tokens; %s)"),
			What, (int32)R.Phase, (int32)R.Outcome, R.Tokens.Num(), *R.Error), bClean);
	}
}

using namespace SuperSLMPrefixCacheNestedAdoptTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedAdoptFullLengthTest, "SuperSLM.PrefixCache.Cpu.Nested.AdoptFullLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedAdoptFullLengthTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), NestedAdoptConfig(), Rig)) { return false; }
	if (!TestTrue(TEXT("budget mode with the cache"), Rig.Cpu->GetPrefixCacheStats().bConfigured && Rig.Cpu->GetPrefixCacheStats().bBudgetMode)) { return false; }
	if (!LoadWalkTableForSeeding(*this, Rig)) { return false; }
	constexpr int32 kMaxNew = 96;

	// The reference: Off from the whole prompt on a fresh sequence.
	FSuperSLMGenerationRequest Whole;
	if (!MakeRequest(*this, Rig, PopulationPrompts()[0], ESuperSLMSchemaJumpForward::Off, kMaxNew, Whole)) { return false; }
	FRunResult Reference;
	if (!RunSolo(*this, Rig, Whole, Reference)) { return false; }

	FSuperSLMPrefix Root, Child;
	int32 RootLength = 0, SuffixLength = 0;
	if (!MakeNestedLeases(*this, Rig, Whole.PromptTokens, Root, Child, RootLength, SuffixLength)) { return false; }
	const int64 WholeLength = RootLength + SuffixLength;

	FRunResult Off, Exact, Canon;
	int64 OffContext = -1, ExactContext = -1, CanonContext = -1;
	if (!RunOverLease(*this, Rig, Child, ESuperSLMSchemaJumpForward::Off, kMaxNew, Off, OffContext)) { return false; }
	bool bOk = TestEqual(TEXT("after the nested adopt the context is parent + suffix"), OffContext, WholeLength);
	bOk &= EndedCleanly(*this, Off, TEXT("Off over the nested lease"));
	bOk &= SameEnding(*this, Off, Reference, TEXT("Off over the nested lease equals Off from the whole prompt"));

	// Exact, with the draft learned from the Off stream, as the other exact cells seed it.
	FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu, Rig.Schema, { Off.Tokens });
	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	if (!RunOverLease(*this, Rig, Child, ESuperSLMSchemaJumpForward::Exact, kMaxNew, Exact, ExactContext)) { return false; }
	bOk &= TestEqual(TEXT("Exact: after the nested adopt the context is parent + suffix"), ExactContext, WholeLength);
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Exact.Sequence), TEXT("Exact over the nested lease"));
	bOk &= EndedCleanly(*this, Exact, TEXT("Exact over the nested lease"));
	bOk &= SameEnding(*this, Exact, Off, TEXT("Exact over the nested lease equals Off over it"));

	FSuperSLMSchemaJumpTestAccess::StartRecording(*Rig.Cpu);
	if (!RunOverLease(*this, Rig, Child, ESuperSLMSchemaJumpForward::Canonical, kMaxNew, Canon, CanonContext)) { return false; }
	bOk &= TestEqual(TEXT("Canonical: after the nested adopt the context is parent + suffix"), CanonContext, WholeLength);
	bOk &= RequireSpanActivity(*this, SpanJobsOf(*Rig.Cpu, Canon.Sequence), TEXT("Canonical over the nested lease"));
	bOk &= EndedCleanly(*this, Canon, TEXT("Canonical over the nested lease"));

	Rig.Cpu->ReturnPrefix(Child);
	Rig.Cpu->ReturnPrefix(Root);
	Settle(*Rig.Cpu);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheCpuNestedAdoptCapFullLengthTest, "SuperSLM.PrefixCache.Cpu.Nested.AdoptCapFullLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheCpuNestedAdoptCapFullLengthTest::RunTest(const FString& Parameters)
{
	if (!LiveGate(*this)) { return false; }
	FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity History;
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	FCpuRig Rig;
	if (!SetUpCpu(*this, W.GetTestWorld(), NestedAdoptConfig(), Rig)) { return false; }
	TArray<int32> PromptTokens;
	if (!TestTrue(TEXT("tokenize"), Rig.Cpu->Tokenize(PopulationPrompts()[0], PromptTokens))) { return false; }
	FSuperSLMPrefix Root, Child;
	int32 RootLength = 0, SuffixLength = 0;
	if (!MakeNestedLeases(*this, Rig, PromptTokens, Root, Child, RootLength, SuffixLength)) { return false; }
	const int64 Cap = FSuperSLMSchemaJumpReviewTestAccess::GetContextCap(*Rig.Cpu);
	const int64 WholeLength = RootLength + SuffixLength;
	if (!TestTrue(TEXT("the cap is known"), Cap > WholeLength)) { return false; }

	FSuperSLMSequence Seq;
	int64 Context = -1;
	if (!VendAndAdopt(*this, Rig, Child, Seq, Context)) { return false; }
	// One token past the cap after parent + suffix; it would fit after the suffix alone (RootLength >= 1).
	const int32 PromptCount = int32(Cap - WholeLength + 1);
	FSuperSLMGenerationRequest Req;
	Req.PromptTokens.Init(PromptTokens[0], PromptCount);
	Req.SpanKind = ESuperSLMSpanKind::Prompt;
	Req.MaxNewTokens = 1;
	AddExpectedErrorPlain(FString::Printf(TEXT("the prompt's %d tokens after a %lld-token prefix exceed the model's context_cap of %lld"),
		PromptCount, WholeLength, Cap), EAutomationExpectedErrorFlags::Contains, 1);
	TArray<int32> Tokens;
	FString Error;
	RunGenerationToCompletion(*Rig.Cpu, Seq, Req, Tokens, kRunSeconds, Error);
	bool bOk = TestEqual(TEXT("the over-cap prompt is refused: the sequence faults"), (uint8)Rig.Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("the refusal comes before any generated token"), Rig.Cpu->GetGeneratedTokens(Seq).Num(), 0);
	Rig.Cpu->ReturnSequence(Seq);
	Rig.Cpu->ReturnPrefix(Child);
	Rig.Cpu->ReturnPrefix(Root);
	Settle(*Rig.Cpu);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_PREFIX_CACHE
