// T-2818 (L2-S3 red suite). R-S3d (the plan §9): "The
// pre-check agrees with the converter (dim 10) -- The pre-check run over the local checkpoints
// whose converter outcome is already recorded (Qwen2.5 0.5B/1.5B-Instruct, Qwen3-Embedding-
// 0.6B) -> verdicts match those outcomes."

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "SuperSLMConversionPreCheck.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3PreCheckAgreesWithConverterTest,
	"SuperSLM.L2S3.PreCheck.AgreesWithRecordedConverterOutcomes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3PreCheckAgreesWithConverterTest::RunTest(const FString& Parameters)
{
	// A real, measured bandwidth (SuperSLMSequenceLifecycleBudget.h -- "a real, measured whole-
	// KV-block write bandwidth on THIS box, bytes/sec -- never a shipped constant"), not a
	// project-wide caching decision this test makes: R-S1e already establishes it is safe to
	// measure once per run.
	const double BandwidthBytesPerSec = SuperSLMSequenceLifecycleBudget::MeasureHostWriteBandwidthBytesPerSec();
	if (!TestTrue(TEXT("measured bandwidth is positive"), BandwidthBytesPerSec > 0.0))
	{
		return true;
	}

	int32 MismatchCount = 0;
	for (const SuperSLML2S3Fixtures::FRecordedConversionOutcome& Outcome :
		SuperSLML2S3Fixtures::RecordedConversionOutcomes())
	{
		if (!TestTrue(*FString::Printf(TEXT("%s: checkpoint directory exists"), *Outcome.Label),
				IFileManager::Get().DirectoryExists(*Outcome.CheckpointDir)))
		{
			++MismatchCount;
			continue;
		}

		FSuperSLMConversionPreCheckReport Report;
		const bool bRan = SuperSLMConversionPreCheck::Run(
			Outcome.CheckpointDir,
			/*RequestedContextCap*/ 4096,
			/*SequenceLifecycleBudgetMs*/ 100000.0,
			BandwidthBytesPerSec,
			Report);
		if (!TestTrue(*FString::Printf(TEXT("%s: pre-check runs to completion"), *Outcome.Label), bRan))
		{
			++MismatchCount;
			continue;
		}

		if (!TestEqual(*FString::Printf(TEXT("%s: pre-check verdict matches the recorded converter outcome"), *Outcome.Label),
				Report.bPass, Outcome.bExpectedPass))
		{
			++MismatchCount;
		}

		if (!Outcome.bExpectedPass)
		{
			// "A pass means 'no known blocker'" (§7 item 2) -- the converse, a fail, is a
			// NAMED diagnostic, never a bare false (this project's "named diagnostic"
			// convention, SuperSLMImportDiagnostic.h).
			TestFalse(*FString::Printf(TEXT("%s: fail carries a non-empty blocker reason"), *Outcome.Label),
				Report.BlockerReason.IsEmpty());

			// Round 5 (review W3, record §10): "agrees with the converter" covers the reason, not only
			// the verdict. A pre-check that rejects every non-qwen2 model_type by name would pass on
			// the verdict alone; the converter's recorded failure is the geometry / QK-norm cause.
			// Red while the product reports the architecture-name blocker first and alone.
			bool bNamesRecordedCause = false;
			for (const FString& Word : Outcome.RecordedCauseWords)
			{
				bNamesRecordedCause |= Report.BlockerReason.Contains(Word, ESearchCase::IgnoreCase);
			}
			if (!TestTrue(*FString::Printf(TEXT("%s: the blocker names the recorded cause, %s"), *Outcome.Label, *Outcome.RecordedCause),
					bNamesRecordedCause))
			{
				AddInfo(FString::Printf(TEXT("%s: blocker reported: %s"), *Outcome.Label, *Report.BlockerReason));
				++MismatchCount;
			}
		}
	}

	return MismatchCount == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
