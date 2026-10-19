// 1.1 (plan §5 item 1a, "Lane depth"): a released prefix keeps its place until its worker work is
// done. Before, ReleasePrefix() on a prefix whose Begin was posted but not yet delivered dropped
// the entry at once: the Begin's delivery then found nothing and dropped the handle, leaking the
// pool block it made, and a new prefix's Begin could be posted while that work was still on the
// lane, so create/release cycling stacked jobs without bound. This cell pins the repair:
//   1. The orphaned Begin's block is released: a PrefixRelease job follows it and delivers.
//   2. A prefix created right after the release is accepted at once (CreatePrefix() counts live
//      prefixes only), but its Begin is not posted until that release has delivered.
//   3. The new prefix then reaches Ready, and every job the cell posted delivers.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SuperSLMSlotGates.h"
#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMPrefixRetireTest,
	"SuperSLM.L2S1.PrefixRetire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMPrefixRetireTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	FString AExPath, Reason;
	if (!TryGetAExArtifactPath(AExPath, Reason))
	{
		AddError(FString::Printf(TEXT("SuperSLM.L2S1.PrefixRetire: %s"), *Reason));
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX accepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("CPU subsystem"), Cpu))
	{
		return false;
	}
	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 1; // one prefix place: the second prefix needs the first's block back
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 16.6;
	if (!TestEqual(TEXT("Configure()"), (uint8)Cpu->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}
	TArray<int32> Tokens;
	if (!TestTrue(TEXT("the prefix tokenizes"), Cpu->Tokenize(TEXT("You are the shop's front desk. Answer briefly."), Tokens) && Tokens.Num() > 0))
	{
		return false;
	}

	auto FindKind = [Cpu](ESuperSLMWorkerJobKind Kind, int32 From) -> int32
	{
		const TArray<FSuperSLMWorkerJobReport>& Ledger = Cpu->GetJobLedger();
		for (int32 J = From; J < Ledger.Num(); ++J)
		{
			if (Ledger[J].Kind == Kind)
			{
				return J;
			}
		}
		return INDEX_NONE;
	};

	// 1. Post the first prefix's Begin, then release the prefix before the Begin delivers (a job
	// delivers no earlier than the tick after the one that posts it).
	const int32 LedgerStart = Cpu->GetJobLedger().Num();
	FSuperSLMPrefix First;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("CreatePrefix (first): %s"), *Error), Cpu->CreatePrefix(Tokens, First, Error)))
	{
		return false;
	}
	for (int32 I = 0; I < 60 && FindKind(ESuperSLMWorkerJobKind::PrefixBegin, LedgerStart) == INDEX_NONE; ++I)
	{
		Cpu->Tick(1.0f / 60.0f);
	}
	const int32 FirstBegin = FindKind(ESuperSLMWorkerJobKind::PrefixBegin, LedgerStart);
	if (!TestTrue(TEXT("the first prefix's Begin is posted"), FirstBegin != INDEX_NONE)
		|| !TestTrue(TEXT("the first prefix's Begin has not delivered when the prefix is released"), Cpu->GetJobLedger()[FirstBegin].DeliveredAtTick < 0))
	{
		return false;
	}
	if (!TestTrue(*FString::Printf(TEXT("ReleasePrefix (first): %s"), *Error), Cpu->ReleasePrefix(First, Error)))
	{
		return false;
	}
	bool bOk = true;
	bOk &= TestEqual(TEXT("the released prefix leaves the caller's view at once"), (uint8)Cpu->GetPrefixPhase(First), (uint8)ESuperSLMPrefixPhase::Invalid);

	// 2. A second prefix is accepted at once.
	FSuperSLMPrefix Second;
	if (!TestTrue(*FString::Printf(TEXT("CreatePrefix (second) right after the release: %s"), *Error), Cpu->CreatePrefix(Tokens, Second, Error)))
	{
		return false;
	}

	// 3. Run until the second prefix is Ready and nothing is undelivered.
	DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible, [Cpu, &Second]()
	{
		if (!Cpu->IsPrefixReady(Second))
		{
			return false;
		}
		for (const FSuperSLMWorkerJobReport& Job : Cpu->GetJobLedger())
		{
			if (Job.DeliveredAtTick < 0)
			{
				return false;
			}
		}
		return true;
	}, 60.0);
	const TArray<FSuperSLMWorkerJobReport>& Ledger = Cpu->GetJobLedger();
	const int32 OrphanRelease = FindKind(ESuperSLMWorkerJobKind::PrefixRelease, FirstBegin + 1);
	bOk &= TestTrue(TEXT("the orphaned Begin's block is released: a PrefixRelease job follows it"), OrphanRelease != INDEX_NONE);
	const int32 SecondBegin = FindKind(ESuperSLMWorkerJobKind::PrefixBegin, FirstBegin + 1);
	bOk &= TestTrue(TEXT("the second prefix's Begin is posted"), SecondBegin != INDEX_NONE);
	if (OrphanRelease != INDEX_NONE && SecondBegin != INDEX_NONE)
	{
		bOk &= TestTrue(TEXT("the orphan's release delivers"), Ledger[OrphanRelease].DeliveredAtTick >= 0);
		bOk &= TestTrue(*FString::Printf(TEXT("the second Begin is posted only after the orphan's release delivered (planned at tick %d, release delivered at tick %d)"),
				Ledger[SecondBegin].PlannedAtTick, Ledger[OrphanRelease].DeliveredAtTick),
			Ledger[SecondBegin].PlannedAtTick >= Ledger[OrphanRelease].DeliveredAtTick);
	}
	bOk &= TestTrue(TEXT("the second prefix reaches Ready"), Cpu->IsPrefixReady(Second));
	for (int32 J = LedgerStart; J < Ledger.Num(); ++J)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("job %lld delivers"), Ledger[J].JobId), Ledger[J].DeliveredAtTick >= 0);
	}
	Cpu->ReleasePrefix(Second);
	for (int32 I = 0; I < 40; ++I)
	{
		Cpu->Tick(1.0f / 60.0f);
	}
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
