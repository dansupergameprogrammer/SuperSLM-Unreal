// T-2971 (L7 root cause, D-SLM7553): a subsystem torn down while a worker job has executed but
// not yet been delivered must deliver that job before it releases any Layer-1 handle.
//
// A worker job is two halves of one handle change: Execute (worker thread) makes the Layer-1
// call, Deliver (game thread, from Tick()'s Apply) records the resulting handle on the slot or
// prefix. TearDown() used to stop the workers -- which runs every enqueued Execute -- and then
// discard every pending Deliver. For a Restore that left FSlot::Seq naming the warm sequence the
// worker had already released, and TearDown() released it a second time: the
// EXCEPTION_ACCESS_VIOLATION in sslm_seq_release that SaveRestore.AdapterRebindsOrRefuses hit when
// a stalled restore sent it down its early-return path. That shape is a use-after-free, so its red
// state is undefined behaviour: it crashes only when the freed object's memory has been reused,
// and usually does not, because sslm_seq_restore allocates the new sequence on the same thread
// immediately after the release frees the old one.
//
// This cell pins the same defect -- an executed job's delivery dropped at teardown -- on the one
// job kind whose red state is deterministic and observable without undefined behaviour:
// PrefixBegin. Its Execute creates a prefix handle (drawing a pool block and taking a model
// reference) and only its Deliver records that handle on the prefix entry. With the delivery
// dropped, TearDown() never releases the handle, sslm_kv_pool_destroy refuses with a live handle
// and sslm_model_unmap refuses with a live reference, and each refusal is logged as a
// LogSuperSLM error. With the delivery made, the handle is released and teardown is silent.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

#include <atomic>

using namespace SuperSLML2S1Fixtures;

namespace
{
	// Counts LogSuperSLM lines at Error verbosity or worse while attached. Category and verbosity
	// only -- no message text is matched, so the oracle does not depend on diagnostic wording.
	class FSuperSLMErrorCounter : public FOutputDevice
	{
	public:
		std::atomic<int32> Errors{0};

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
			if (Category == FName(TEXT("LogSuperSLM")) && Level <= ELogVerbosity::Error)
			{
				Errors.fetch_add(1, std::memory_order_relaxed);
			}
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S1TeardownDeliversExecutedJobTest,
	"SuperSLM.L2S1.Teardown.ExecutedJobDeliveredBeforeHandleRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S1TeardownDeliversExecutedJobTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}

	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(ACpuArtifactPath(), Diag);
	if (!TestNotNull(TEXT("A-CPU must import"), Model) || !TestTrue(TEXT("A-CPU Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMSubsystem* Subsystem = GetSubsystem(TestWorldWrapper.GetTestWorld());
	if (!TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Subsystem))
	{
		return false;
	}

	FSuperSLMRuntimeConfig Config;
	Config.BlockCount = 1;
	Config.PrefixBlockCount = 1;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 64;
	Config.MaxLayerBudget = 24;
	Config.SequenceLifecycleBudgetMs = 1000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure() result"), (uint8)Subsystem->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	TArray<FReferenceCase> Cases;
	FString LoadError;
	if (!TestTrue(*FString::Printf(TEXT("LoadReferenceCases: %s"), *LoadError), LoadReferenceCases(Cases, LoadError)))
	{
		return false;
	}

	FSuperSLMPrefix Prefix;
	FString PrefixError;
	if (!TestTrue(*FString::Printf(TEXT("CreatePrefix: %s"), *PrefixError), Subsystem->CreatePrefix(Cases[0].PromptTokens, Prefix, PrefixError)))
	{
		return false;
	}

	// One tick: Plan posts the PrefixBegin job to the lane. Its Deliver needs a LATER tick's
	// Apply, and no later tick is ever made -- the teardown below is the next thing that happens.
	Subsystem->Tick(1.0f / 60.0f);

	// Vitality of the construction: exactly one job is on the ledger, it is the PrefixBegin, and
	// it has not been delivered. If any of these fail, the teardown below would not exercise an
	// executed-but-undelivered job and this cell could not fail on the defect.
	const TArray<FSuperSLMWorkerJobReport>& Ledger = Subsystem->GetJobLedger();
	if (!TestEqual(TEXT("exactly one job is posted after one tick"), Ledger.Num(), 1) ||
		!TestEqual(TEXT("the posted job is the PrefixBegin"), (uint8)Ledger[0].Kind, (uint8)ESuperSLMWorkerJobKind::PrefixBegin) ||
		!TestEqual(TEXT("the PrefixBegin has not been delivered"), Ledger[0].DeliveredAtTick, -1) ||
		!TestEqual(TEXT("the prefix is still Pending (its Begin is undelivered)"), (uint8)Subsystem->GetPrefixPhase(Prefix), (uint8)ESuperSLMPrefixPhase::Pending))
	{
		return false;
	}

	// Real time for the worker to execute sslm_prefix_begin before the teardown. The fixed build
	// needs none of it -- its worker runs every enqueued job before honouring a stop -- but the
	// defective build's worker could exit on Stop() without running a just-enqueued job, which
	// would make this cell pass on that build by never creating the handle it leaks. Waiting
	// removes that race, so the cell is red on the defect every time rather than most times.
	FPlatformProcess::Sleep(1.0f);

	// Configure() runs TearDown() first. That teardown is the one under test.
	FSuperSLMErrorCounter Counter;
	GLog->AddOutputDevice(&Counter);
	const FSuperSLMConfigureReport Reconfigure = Subsystem->Configure(Model, Config);
	GLog->RemoveOutputDevice(&Counter);

	bool bOk = TestEqual(TEXT("re-Configure() after the teardown result"), (uint8)Reconfigure.Result, (uint8)ESuperSLMConfigureResult::Success);
	// FEAT oracle: a teardown that drops the executed PrefixBegin's delivery leaves the created
	// prefix handle unreleased, and sslm_kv_pool_destroy / sslm_model_unmap each refuse and log a
	// LogSuperSLM error. A teardown that delivers it releases every handle and logs none.
	bOk &= TestEqual(TEXT("teardown with an executed, undelivered PrefixBegin logs no LogSuperSLM error (every Layer-1 handle released)"),
		Counter.Errors.load(), 0);
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC
#endif // WITH_DEV_AUTOMATION_TESTS
