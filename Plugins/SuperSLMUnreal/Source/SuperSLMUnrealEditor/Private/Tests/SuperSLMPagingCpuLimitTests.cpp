// Plugin paging plan, CPU red suite (step K1): the generation limit (§3.3 "Generation limit"; engine
// token law E7) and the adopt-side refusals of §5.
//
//   SuperSLM.Paging.Cpu.Limit.ExactCounts              cell 4.2 (the CPU's legs)               [K2]
//   SuperSLM.Paging.Cpu.Limit.ExceededAtActivation     §5 KvBudgetExceeded at activation       [K2]
//   SuperSLM.Paging.Cpu.Limit.SetSchemaAfterAdopt      §5 SetSchema after adopt; cell 8.2's CPU leg [K2]
//
// E7, as the plan states it: an embed is allowed while context_used < limit, and a finish-only step
// at context_used == limit, so a holder resting ready at c emits 1 + (limit - c) tokens, then stops
// by name: KvBudgetExceeded below the cap, the existing cap outcome at it. Each test names its cell
// and the mutant it kills.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "SuperSLMSchemaHandle.h"

using namespace SuperSLMPagingFixtures;

namespace
{
	// The stop at the limit: KvBudgetExceeded below the cap; at the cap, the CPU's existing cap
	// outcome (Faulted, the outcome left Generating by SSLM_CONTEXT_CAP_EXCEEDED).
	bool ExpectStop(FAutomationTestBase& T, const FGeneration& G, bool bAtCap, const FString& What)
	{
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: stops Faulted"), *What), (uint8)G.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: %s"), *What, bAtCap ? TEXT("the cap outcome") : TEXT("KvBudgetExceeded")), (uint8)G.Outcome,
			(uint8)(bAtCap ? ESuperSLMDecodeOutcome::Generating : ESuperSLMDecodeOutcome::KvBudgetExceeded));
		return bOk;
	}
}

// Cell 4.2 (the CPU's legs, B = 64): budgets {1, B-1, B, B+1, 512, cap-1, cap}, for a plain holder and
// for an adopter, each generated with MaxNewTokens above the limit so the limit is the only stop.
//   - Plain holder: prompt p = min(b, 8), resting ready at c = p: emits 1 + (b - p) tokens.
//   - Adopter of a ready prefix of L tokens with an empty suffix: emits 1 + (min(L + b, cap) - L),
//     which is b + 1 below the cap. L = 100, or L = 4,080 when b >= cap - 1 (the limit is the cap
//     either way; the longer prefix keeps that leg short).
//   The next step after the last token resolves KvBudgetExceeded, or the cap outcome at the cap.
// Mutants: the pre-check `<=` in place of `<` (one late); `limit - 1` (one early); the pre-check
// absent. On the CPU the engine enforces E7 by itself (Cp = Ce), and the plugin maps its
// KV_BUDGET_EXCEEDED to the same outcome and count, so "one late" and "absent" are killed only by
// the log: the engine's refusal is reachable only past the plugin's pre-check and is logged as a
// limit fault (§7), so no line containing "limit fault" may appear (the reading is in the K1 record).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuExactCountsTest, "SuperSLM.Paging.Cpu.Limit.ExactCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuExactCountsTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this))
	{
		return false;
	}
	FPagingLogCapture Log;
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	const int64 Cap = ExpectedCap(Rig.Artifact);
	const int64 Budgets[] = { 1, 63, 64, 65, 512, Cap - 1, Cap };
	bool bOk = true;
	for (const int64 Budget : Budgets)
	{
		const int64 L = Budget >= Cap - 1 ? 4080 : 100;
		if (!ConfigureBudget(*this, Rig, MakeConfig(1, int32(Budget), 1, 1)))
		{
			return false;
		}
		// The plain holder.
		{
			const int64 P = FMath::Min<int64>(Budget, 8);
			const int64 Expected = 1 + (Budget - P);
			TArray<int32> Prompt;
			FSuperSLMSequence Seq;
			FGeneration G;
			if (!MakeTokens(*this, Cpu, int32(P), 1, Prompt) || !Vend(*this, Cpu, Seq) || !Generate(*this, Cpu, Seq, Prompt, int32(Expected + 16), G))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Seq);
			const FString What = FString::Printf(TEXT("plain holder, b = %lld, prompt %lld"), Budget, P);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: emits 1 + (b - p) = %lld"), *What, Expected), int64(G.Tokens.Num()), Expected);
			bOk &= ExpectStop(*this, G, Budget >= Cap, What);
		}
		// The adopter.
		{
			const int64 Limit = FMath::Min<int64>(L + Budget, Cap);
			const int64 Expected = 1 + (Limit - L);
			TArray<int32> Tokens;
			FSuperSLMPrefix Prefix;
			FSuperSLMSequence Seq;
			FGeneration G;
			if (!MakeTokens(*this, Cpu, int32(L), 2, Tokens) || !BuildPrefix(*this, Cpu, Tokens, Prefix) || !Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, Prefix)
				|| !Generate(*this, Cpu, Seq, TArray<int32>(), int32(Expected + 16), G))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Seq);
			Cpu.ReleasePrefix(Prefix);
			DrainQuiet(Cpu);
			const FString What = FString::Printf(TEXT("adopter of %lld, b = %lld"), L, Budget);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: emits 1 + (limit - L) = %lld"), *What, Expected), int64(G.Tokens.Num()), Expected);
			bOk &= ExpectStop(*this, G, Limit >= Cap, What);
		}
	}
	bOk &= NoLedgerFault(*this, Cpu, TEXT("cell 4.2"));
	bOk &= TestEqual(TEXT("cell 4.2: no limit fault was logged (every stop was the plugin's pre-check)"), Log.CountContaining(TEXT("limit fault")), 0);
	return bOk;
}

// §5 KvBudgetExceeded at activation (§3.3: ActivateGenerate faults KvBudgetExceeded if prefix +
// prompt > limit). At SequenceBudget 64: a plain holder given a 65-token prompt, and an adopter of a
// 100-token prefix given a 65-token suffix, each Fault KvBudgetExceeded with no token produced; a
// 64-token prompt and suffix (prefix + prompt == limit) are admitted and emit one token.
// Mutant: the activation check `>=` in place of `>` (refuses the exact fit); the check absent (the
// engine's KV_BUDGET_EXCEEDED arrives mid-prefill instead, with the same outcome: killed by the
// absent "limit fault" log line, as in cell 4.2).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuExceededAtActivationTest, "SuperSLM.Paging.Cpu.Limit.ExceededAtActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuExceededAtActivationTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(1, 64, 1, 1)))
	{
		return false;
	}
	FPagingLogCapture Log;
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> PrefixTokens;
	FSuperSLMPrefix Prefix;
	if (!MakeTokens(*this, Cpu, 100, 1, PrefixTokens) || !BuildPrefix(*this, Cpu, PrefixTokens, Prefix))
	{
		return false;
	}
	bool bOk = true;
	for (const bool bAdopter : { false, true })
	{
		for (const int32 Length : { 65, 64 })
		{
			TArray<int32> Prompt;
			FSuperSLMSequence Seq;
			FGeneration G;
			if (!MakeTokens(*this, Cpu, Length, 2, Prompt) || !Vend(*this, Cpu, Seq) || (bAdopter && !AdoptAndWait(*this, Cpu, Seq, Prefix))
				|| !Generate(*this, Cpu, Seq, Prompt, 8, G))
			{
				return false;
			}
			ReturnAndDrain(Cpu, Seq);
			const FString What = FString::Printf(TEXT("%s, %d tokens against a limit of %s"), bAdopter ? TEXT("adopter") : TEXT("plain holder"), Length,
				bAdopter ? TEXT("100 + 64") : TEXT("64"));
			if (Length > 64)
			{
				bOk &= ExpectStop(*this, G, false, What);
				bOk &= TestEqual(*FString::Printf(TEXT("%s: no token produced"), *What), G.Tokens.Num(), 0);
			}
			else
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s: the exact fit emits one token"), *What), G.Tokens.Num(), 1);
				bOk &= ExpectStop(*this, G, false, What);
			}
		}
	}
	Cpu.ReleasePrefix(Prefix);
	DrainQuiet(Cpu);
	bOk &= TestEqual(TEXT("no limit fault was logged (the refusals were the activation check's)"), Log.CountContaining(TEXT("limit fault")), 0);
	return bOk;
}

// §5 "SetSchema after adopt, refused by name" and cell 8.2's CPU leg (§3.3 "Adopt", E14): in budget
// mode, SetSchema() after a successful adopt is refused with a named reason, and nothing is bound.
// This pins the legacy refusal in budget mode, where the adopt is the engine's budget adopt.
// Mutant: a bind allowed after a budget adopt (the engine refuses it SCHEMA_BIND_REJECTED, unnamed).
// This is cell 8.2's CPU leg, and its CPU owner (plan rev 1.9, ruling 7). 8.2's other CPU leg, a
// SetSchema after an adopt refused PREFIX_SCHEMA_MISMATCH, is recorded as unreachable through the
// plugin (ruling 5): the engine refuses that adopt only when the prefix carries schema progress, and
// no plugin path binds a schema to a CPU prefix. The engine's own suites carry that refusal; no CPU
// twin is added.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingCpuSetSchemaAfterAdoptTest, "SuperSLM.Paging.Cpu.Limit.SetSchemaAfterAdopt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingCpuSetSchemaAfterAdoptTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureBudget(*this, Rig, MakeConfig(1, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	FSuperSLMSchemaHandle Schema;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the demo schema resolves (%s)"), *Error),
			FSuperSLMSchemaLookup::LookupByName(*Rig.Model, SuperSLML2S2Fixtures::DemoSchemaName(), Schema, Error)))
	{
		return false;
	}
	TArray<int32> Tokens;
	FSuperSLMPrefix Prefix;
	FSuperSLMSequence Seq;
	if (!MakeTokens(*this, Cpu, 100, 1, Tokens) || !BuildPrefix(*this, Cpu, Tokens, Prefix) || !Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, Prefix))
	{
		return false;
	}
	FString BindError;
	bool bOk = TestFalse(TEXT("SetSchema after a budget adopt is refused"), Cpu.SetSchema(Seq, Schema, BindError));
	bOk &= TestTrue(*FString::Printf(TEXT("the refusal is named (%s)"), *BindError), BindError.Contains(TEXT("adopted a prefix")));
	ReturnAndDrain(Cpu, Seq);
	Cpu.ReleasePrefix(Prefix);
	DrainQuiet(Cpu);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
