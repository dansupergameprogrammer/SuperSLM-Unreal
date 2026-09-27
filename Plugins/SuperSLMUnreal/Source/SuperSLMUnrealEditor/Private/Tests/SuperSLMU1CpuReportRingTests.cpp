// The CPU job ledger and tick history are fixed rings: 1024 rows each by default, created at
// Configure() and never resized. A full ring overwrites its oldest row. Rows are numbered from 0
// since Configure(); the appended count is the next row's number, and FindJobLedgerRow() and
// FindTickHistoryRow() return null for a row that has been overwritten.
//
// ReportRingsEvictAtDefaultCapacity: at the release default (no capacity override), one sequence
// at one layer per job posts well over 1024 jobs and runs well over 1024 ticks. Asserted: both
// rings hold exactly 1024 rows; the appended counts keep counting and agree with the job ids and
// tick indices, which the subsystem numbers on its own; the oldest rows are gone and the Find
// accessors miss them; every retained row is found, in order; and the rings' storage is the same
// memory before and after 256 more rows overwrite older ones, so nothing grew after Configure().
//
// QueryBehindLedgerLogsMissedRows: a query that reads the ledger every tick misses nothing, and
// logs nothing. A query that is not ticked while the subsystem runs another sequence for more than
// 1024 jobs finds its unread rows overwritten; it finishes, and logs one warning that names
// exactly the number of rows it missed.
//
// HitchCountedAfterLedgerRowEvicted: a job's hitch is counted from the figures carried with the
// job, not from its ledger row, so it is counted when the row is gone. The ring is set to one row
// here (the size does not change the code under test, and at 1024 rows no job stays in flight
// long enough for its row to be overwritten). At a 0.1 ms budget every real job overruns, so the
// hitch count must equal every job delivered plus every late one, summed from the tick rows. At
// least one job is delivered after its row was overwritten, which is asserted.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_L2S3

#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMQuery.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchedulingTestAccess.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S1Fixtures;

namespace
{
	// The release capacity of each ring. Not read from the product: the cell states the number the
	// product ships with, so a change to it is a visible change here.
	constexpr int32 kReleaseReportRows = 1024;

	USuperSLMModel* ImportRingCellModel(FAutomationTestBase& T)
	{
		FString AExPath, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
		{
			return nullptr;
		}
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!T.TestNotNull(TEXT("A-EX must import"), Model) || !T.TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
		{
			return nullptr;
		}
		return Model;
	}

	FSuperSLMGenerationRequest MakeRingRequest(int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = {1, 2, 3};
		Request.MaxNewTokens = MaxNewTokens;
		return Request;
	}

	bool IsStopped(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
		return Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted;
	}

	// Collects the query runner's missed-row warning ("... leave out N job(s) ...").
	// Declared usable on multiple threads, so GLog serializes each line to it on the thread that
	// logs it. A device that does not declare it is buffered and served only from the primary
	// logging thread, which in the editor is a dedicated thread: a line the query logs inside the
	// cell would then reach Counts after the cell had read it. Counts is guarded because any
	// thread may log.
	class FMissedRowLog : public FOutputDevice
	{
	public:
		FMissedRowLog() { GLog->AddOutputDevice(this); }
		virtual ~FMissedRowLog() override { GLog->RemoveOutputDevice(this); }

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category != FName(TEXT("LogSuperSLM")))
			{
				return;
			}
			const FString Line(V);
			const int32 At = Line.Find(Prefix);
			if (At == INDEX_NONE)
			{
				return;
			}
			const FString Rest = Line.Mid(At + FCString::Strlen(Prefix));
			int64 N = -1;
			LexFromString(N, *Rest.Left(Rest.Find(TEXT(" "))));
			FScopeLock Lock(&CountsLock);
			Counts.Add(N);
		}

		TArray<int64> GetCounts() const
		{
			FScopeLock Lock(&CountsLock);
			return Counts;
		}

	private:
		mutable FCriticalSection CountsLock;
		TArray<int64> Counts;
		const TCHAR* Prefix = TEXT("SuperSLM query: the job figures leave out ");
	};
}

// --- At the release default, over 1024 jobs and 1024 ticks: 1024 rows each, the counts keep
// counting, the oldest rows are gone, and the storage never moved. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuReportRingsEvictAtDefaultCapacityTest,
	"SuperSLM.U1.Cpu.ReportRingsEvictAtDefaultCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuReportRingsEvictAtDefaultCapacityTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = ImportRingCellModel(*this);
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (Model == nullptr || !TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Cpu))
	{
		return false;
	}
	// The release default, not an override: the cell exists to run the shipping size.
	if (!TestEqual(TEXT("precondition: no capacity override is in force (the release default)"),
		FSuperSLMSchedulingTestAccess::GetReportHistoryCapacity(), kReleaseReportRows))
	{
		return false;
	}
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 1;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0; // K = 1; hitches are not this cell's subject
	if (!TestEqual(TEXT("Configure()"), (uint8)Cpu->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("a fresh Configure() starts the job ledger's row count at 0"), Cpu->GetJobLedgerAppendedCount(), (int64)0);
	bOk &= TestEqual(TEXT("a fresh Configure() starts the tick history's row count at 0"), Cpu->GetTickHistoryAppendedCount(), (int64)0);

	FSuperSLMSequence Seq;
	if (!TestEqual(TEXT("VendSequence()"), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	// One layer per job: a token is 24 jobs, so 72 tokens are about 1,730 jobs, and every job
	// takes at least one tick.
	Cpu->SetLayerBudget(Seq, 1);
	FString BeginError;
	if (!TestTrue(*FString::Printf(TEXT("BeginGeneration(): %s"), *BeginError), Cpu->BeginGeneration(Seq, MakeRingRequest(72), BeginError)))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}

	// Numbering, read while row 0 of each ring is still retained: the subsystem's own job id and
	// tick index for row 0.
	int64 JobIdOfRow0 = -1;
	int64 TickIndexOfRow0 = -1;
	// Snapshot A: the first time both rings are full, every retained row's slot address and its
	// member list's allocation. Addresses are compared, never dereferenced, after later ticks.
	bool bSnapshotTaken = false;
	int64 JobsAppendedAtA = 0;
	int64 TicksAppendedAtA = 0;
	TSet<const void*> JobSlotsAtA;
	TMap<const void*, const void*> MemberDataAtA;
	TSet<const void*> TickSlotsAtA;
	int64 LastJobCountSeen = 0;
	int32 JobCountWentBack = 0;
	constexpr int64 kOverwritesAfterA = 256;

	DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[&]()
		{
			const int64 Jobs = Cpu->GetJobLedgerAppendedCount();
			const int64 Ticks = Cpu->GetTickHistoryAppendedCount();
			JobCountWentBack += Jobs < LastJobCountSeen ? 1 : 0;
			LastJobCountSeen = Jobs;
			if (JobIdOfRow0 < 0)
			{
				if (const FSuperSLMWorkerJobReport* Row0 = Cpu->FindJobLedgerRow(0))
				{
					JobIdOfRow0 = Row0->JobId;
				}
			}
			if (TickIndexOfRow0 < 0)
			{
				if (const FSuperSLMTickReport* Row0 = Cpu->FindTickHistoryRow(0))
				{
					TickIndexOfRow0 = Row0->TickIndex;
				}
			}
			if (!bSnapshotTaken && Jobs >= kReleaseReportRows && Ticks >= kReleaseReportRows)
			{
				bSnapshotTaken = true;
				JobsAppendedAtA = Jobs;
				TicksAppendedAtA = Ticks;
				for (int64 Row = Jobs - kReleaseReportRows; Row < Jobs; ++Row)
				{
					if (const FSuperSLMWorkerJobReport* Job = Cpu->FindJobLedgerRow(Row))
					{
						JobSlotsAtA.Add(Job);
						MemberDataAtA.Add(Job, Job->MemberSequences.GetData());
					}
				}
				for (int64 Row = Ticks - kReleaseReportRows; Row < Ticks; ++Row)
				{
					if (const FSuperSLMTickReport* Tick = Cpu->FindTickHistoryRow(Row))
					{
						TickSlotsAtA.Add(Tick);
					}
				}
			}
			return IsStopped(*Cpu, Seq) && bSnapshotTaken &&
				Jobs >= JobsAppendedAtA + kOverwritesAfterA && Ticks >= TicksAppendedAtA + kOverwritesAfterA;
		},
		/*MaxWallClockSeconds*/ 600.0);

	const int64 Jobs = Cpu->GetJobLedgerAppendedCount();
	const int64 Ticks = Cpu->GetTickHistoryAppendedCount();
	AddInfo(FString::Printf(TEXT("phase %d; %lld jobs and %lld ticks appended; snapshot at %lld jobs, %lld ticks"),
		(int32)Cpu->GetPhase(Seq), Jobs, Ticks, JobsAppendedAtA, TicksAppendedAtA));
	bOk &= TestEqual(TEXT("the generation completes"), (uint8)Cpu->GetPhase(Seq), (uint8)ESuperSLMSequencePhase::Complete);
	if (!TestTrue(TEXT("precondition: over 1024 jobs and over 1024 ticks, with 256 more of each after both rings were full"),
		bSnapshotTaken && Jobs >= JobsAppendedAtA + kOverwritesAfterA && Ticks >= TicksAppendedAtA + kOverwritesAfterA))
	{
		Cpu->ReturnSequence(Seq);
		return false;
	}
	bOk &= TestTrue(TEXT("row 0 of each ring was read while it was retained"), JobIdOfRow0 > 0 && TickIndexOfRow0 >= 0);
	bOk &= TestEqual(TEXT("the job ledger's appended count never went back (ticks observed)"), JobCountWentBack, 0);

	// --- Row counts are capped; appended counts keep counting. ---
	const TArray<FSuperSLMWorkerJobReport> Ledger = Cpu->GetJobLedger();
	const TArray<FSuperSLMTickReport> History = Cpu->GetTickHistory();
	bOk &= TestEqual(TEXT("the job ledger holds exactly 1024 rows"), Ledger.Num(), kReleaseReportRows);
	bOk &= TestEqual(TEXT("the tick history holds exactly 1024 rows"), History.Num(), kReleaseReportRows);
	bOk &= TestTrue(TEXT("the job ledger's appended count passed the capacity"), Jobs > kReleaseReportRows);
	bOk &= TestTrue(TEXT("the tick history's appended count passed the capacity"), Ticks > kReleaseReportRows);
	// The subsystem numbers jobs and ticks on its own; the row numbers must stay in step with both.
	bOk &= TestEqual(TEXT("the newest job's id is its row number plus row 0's id"), Ledger.Last().JobId, Jobs - 1 + JobIdOfRow0);
	bOk &= TestEqual(TEXT("the newest tick's index is its row number plus row 0's index"), (int64)History.Last().TickIndex, Ticks - 1 + TickIndexOfRow0);
	bOk &= TestEqual(TEXT("the newest tick row is GetLastTickReport()"), History.Last().TickIndex, Cpu->GetLastTickReport().TickIndex);

	// --- The oldest rows are gone, and Find misses exactly those. ---
	const int64 FirstJob = Jobs - kReleaseReportRows;
	const int64 FirstTick = Ticks - kReleaseReportRows;
	bOk &= TestNull(TEXT("FindJobLedgerRow(0) misses: row 0 was overwritten"), Cpu->FindJobLedgerRow(0));
	bOk &= TestNull(TEXT("FindJobLedgerRow() misses the row just before the oldest retained one"), Cpu->FindJobLedgerRow(FirstJob - 1));
	bOk &= TestNull(TEXT("FindJobLedgerRow() misses the next row, which does not exist yet"), Cpu->FindJobLedgerRow(Jobs));
	bOk &= TestNull(TEXT("FindJobLedgerRow(-1) misses"), Cpu->FindJobLedgerRow(-1));
	bOk &= TestNull(TEXT("FindTickHistoryRow(0) misses: row 0 was overwritten"), Cpu->FindTickHistoryRow(0));
	bOk &= TestNull(TEXT("FindTickHistoryRow() misses the row just before the oldest retained one"), Cpu->FindTickHistoryRow(FirstTick - 1));
	bOk &= TestNull(TEXT("FindTickHistoryRow() misses the next row, which does not exist yet"), Cpu->FindTickHistoryRow(Ticks));
	bOk &= TestFalse(TEXT("no retained job row carries row 0's job id"),
		Ledger.ContainsByPredicate([JobIdOfRow0](const FSuperSLMWorkerJobReport& Job) { return Job.JobId == JobIdOfRow0; }));

	// --- Every retained row is found, in order, and GetJobLedger()/GetTickHistory() copy the same rows. ---
	int32 JobRowsWrong = 0;
	int32 TickRowsWrong = 0;
	for (int32 I = 0; I < kReleaseReportRows; ++I)
	{
		const FSuperSLMWorkerJobReport* Job = Cpu->FindJobLedgerRow(FirstJob + I);
		JobRowsWrong += (Job == nullptr || Job->JobId != FirstJob + I + JobIdOfRow0 ||
			(Ledger.IsValidIndex(I) && Ledger[I].JobId != Job->JobId)) ? 1 : 0;
		const FSuperSLMTickReport* Tick = Cpu->FindTickHistoryRow(FirstTick + I);
		TickRowsWrong += (Tick == nullptr || (int64)Tick->TickIndex != FirstTick + I + TickIndexOfRow0 ||
			(History.IsValidIndex(I) && History[I].TickIndex != Tick->TickIndex)) ? 1 : 0;
	}
	bOk &= TestEqual(TEXT("every retained job row is found, numbered in step with its job id, and matches GetJobLedger() (rows wrong)"), JobRowsWrong, 0);
	bOk &= TestEqual(TEXT("every retained tick row is found, numbered in step with its tick index, and matches GetTickHistory() (rows wrong)"), TickRowsWrong, 0);

	// --- No allocation after Configure(): the retained rows now sit in the same slots as at the
	// snapshot, 256 or more rows later, and each slot's member list keeps its allocation. ---
	int32 JobSlotsMoved = 0;
	int32 MemberListsMoved = 0;
	for (int64 Row = FirstJob; Row < Jobs; ++Row)
	{
		const FSuperSLMWorkerJobReport* Job = Cpu->FindJobLedgerRow(Row);
		if (Job == nullptr || !JobSlotsAtA.Contains(Job))
		{
			++JobSlotsMoved;
			continue;
		}
		MemberListsMoved += MemberDataAtA.FindRef(Job) != Job->MemberSequences.GetData() ? 1 : 0;
	}
	int32 TickSlotsMoved = 0;
	for (int64 Row = FirstTick; Row < Ticks; ++Row)
	{
		const FSuperSLMTickReport* Tick = Cpu->FindTickHistoryRow(Row);
		TickSlotsMoved += (Tick == nullptr || !TickSlotsAtA.Contains(Tick)) ? 1 : 0;
	}
	bOk &= TestEqual(TEXT("the snapshot saw 1024 distinct job slots"), JobSlotsAtA.Num(), kReleaseReportRows);
	bOk &= TestEqual(TEXT("the snapshot saw 1024 distinct tick slots"), TickSlotsAtA.Num(), kReleaseReportRows);
	bOk &= TestEqual(TEXT("the job ledger's storage did not move: every retained row is in a slot the snapshot saw (rows elsewhere)"), JobSlotsMoved, 0);
	bOk &= TestEqual(TEXT("each ledger slot's member list kept its allocation across overwrites (slots reallocated)"), MemberListsMoved, 0);
	bOk &= TestEqual(TEXT("the tick history's storage did not move: every retained row is in a slot the snapshot saw (rows elsewhere)"), TickSlotsMoved, 0);

	Cpu->ReturnSequence(Seq);
	return bOk;
}

// --- A query ticked every frame misses nothing. A query left unticked while more than 1024 other
// jobs are posted misses its unread rows and logs how many, once. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuQueryBehindLedgerLogsMissedRowsTest,
	"SuperSLM.U1.Cpu.QueryBehindLedgerLogsMissedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuQueryBehindLedgerLogsMissedRowsTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = ImportRingCellModel(*this);
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (Model == nullptr || !TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Cpu))
	{
		return false;
	}
	if (!TestEqual(TEXT("precondition: no capacity override is in force (the release default)"),
		FSuperSLMSchedulingTestAccess::GetReportHistoryCapacity(), kReleaseReportRows))
	{
		return false;
	}
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1; // the query's jobs and the other sequence's are separate rows
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 2;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	Config.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("Configure()"), (uint8)Cpu->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	// The warning, expected exactly once: only the query that falls behind logs it.
	AddExpectedErrorPlain(TEXT("the ledger overwrote before the query read them"), EAutomationExpectedErrorFlags::Contains, 1);
	FMissedRowLog MissedLog;

	FSuperSLMQueryConfig QueryConfig;
	QueryConfig.Backend = ESuperSLMBackendBP::CPU;
	QueryConfig.FrameBudgetLayers = 1; // one layer per job: a token is 24 of this query's rows
	QueryConfig.ConcurrentQueries = 1;
	const FString Prompt = TEXT("Hello");
	FSuperSLMQueryRunner Runner(*Cpu, nullptr, *Model);
	bool bOk = true;

	// 1. Control: a query that reads the ledger every tick (RunQuery ticks directly) misses nothing.
	{
		FSuperSLMQueryReadout Readout;
		FString Error;
		const bool bRan = Runner.RunQuery(Prompt, 4, QueryConfig, Readout, Error);
		bOk &= TestTrue(*FString::Printf(TEXT("control: the query that keeps up completes (%s)"), *Error), bRan);
		bOk &= TestEqual(TEXT("control: the query that keeps up logs no missed rows"), MissedLog.GetCounts().Num(), 0);
	}

	// 2. The query that falls behind. It starts, and is not ticked while the subsystem runs another
	// sequence at one layer per job until more than 1024 rows follow the query's first row.
	FSuperSLMSequence Other;
	if (!TestEqual(TEXT("VendSequence() for the other sequence"), (uint8)Cpu->VendSequence(Other), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	Cpu->SetLayerBudget(Other, 1);
	const int64 QueryFirstRow = Cpu->GetJobLedgerAppendedCount(); // the query's cursor starts here
	FString BeginError;
	bOk &= TestTrue(*FString::Printf(TEXT("BeginQuery(): %s"), *BeginError), Runner.BeginQuery(Prompt, 4, QueryConfig, BeginError));
	bOk &= TestTrue(*FString::Printf(TEXT("BeginGeneration() for the other sequence: %s"), *BeginError),
		Cpu->BeginGeneration(Other, MakeRingRequest(72), BeginError));
	if (!bOk)
	{
		Runner.CancelQuery();
		Cpu->ReturnSequence(Other);
		return false;
	}
	constexpr int64 kMargin = 64;
	DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
		[&]()
		{
			FSuperSLMQueryLiveView View;
			const bool bQueryStopped = Runner.GetLiveView(View) &&
				(View.Phase == ESuperSLMSequencePhase::Complete || View.Phase == ESuperSLMSequencePhase::Faulted);
			const int64 FirstRetained = Cpu->GetJobLedgerAppendedCount() - kReleaseReportRows;
			return bQueryStopped && FirstRetained > QueryFirstRow + kMargin;
		},
		/*MaxWallClockSeconds*/ 600.0);
	FSuperSLMQueryLiveView View;
	Runner.GetLiveView(View);
	const int64 FirstRetainedBefore = Cpu->GetJobLedgerAppendedCount() - kReleaseReportRows;
	if (!TestTrue(TEXT("precondition: the query's generation completed while it was not ticked"), View.Phase == ESuperSLMSequencePhase::Complete) ||
		!TestTrue(*FString::Printf(TEXT("precondition: the ring overwrote rows past the query's first row (%lld), oldest retained %lld"), QueryFirstRow, FirstRetainedBefore),
			FirstRetainedBefore > QueryFirstRow + kMargin))
	{
		Runner.CancelQuery();
		Cpu->ReturnSequence(Other);
		return false;
	}

	// One TickQuery(): it reads from its cursor, finds every generation stopped, and finishes.
	bool bSucceeded = false;
	FSuperSLMQueryReadout Readout;
	FString QueryError;
	const bool bStopped = Runner.TickQuery(1.0f / 60.0f, bSucceeded, Readout, QueryError);
	// Rows before the oldest retained one, from the query's first row, are the rows it missed. The
	// ring is read after TickQuery(), which may have ticked the subsystem once more.
	const int64 ExpectedMissed = Cpu->GetJobLedgerAppendedCount() - Cpu->GetJobLedger().Num() - QueryFirstRow;
	bOk &= TestTrue(TEXT("the query finishes on its next tick"), bStopped);
	bOk &= TestTrue(*FString::Printf(TEXT("the query that fell behind still succeeds (%s)"), *QueryError), bSucceeded);
	const TArray<int64> MissedCounts = MissedLog.GetCounts();
	bOk &= TestEqual(TEXT("the query that fell behind logs its missed rows exactly once"), MissedCounts.Num(), 1);
	if (MissedCounts.Num() == 1)
	{
		bOk &= TestEqual(TEXT("the warning names exactly the rows overwritten before the query read them"), MissedCounts[0], ExpectedMissed);
	}
	Cpu->ReturnSequence(Other);
	return bOk;
}

// --- With the ring at one row, jobs are delivered after their rows are overwritten, and the hitch
// count still equals every overrun plus every late delivery. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMU1CpuHitchCountedAfterLedgerRowEvictedTest,
	"SuperSLM.U1.Cpu.HitchCountedAfterLedgerRowEvicted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1CpuHitchCountedAfterLedgerRowEvictedTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	USuperSLMModel* Model = ImportRingCellModel(*this);
	USuperSLMSubsystem* Cpu = GetSubsystem(W.GetTestWorld());
	if (Model == nullptr || !TestNotNull(TEXT("USuperSLMSubsystem must be reachable"), Cpu))
	{
		return false;
	}
	FSuperSLMRuntimeConfig Config;
	Config.MaxSequencesPerDecodeCall = 1;
	Config.MaxPrefillChunkBudget = 32;
	Config.MaxLayerBudget = 24;
	Config.BlockCount = 2;
	Config.MaxConcurrentCalls = 2;
	Config.SequenceLifecycleBudgetMs = 100000.0;
	// Every real Layer-1 call takes far longer than 0.1 ms, so every delivered job overruns. The
	// static costs are priced near zero so that K is 1 and a whole token or prompt is one job.
	Config.TickBudgetMs = 0.1;
	Config.LayerCostMs = 0.001;
	Config.LayerCostPerPositionMs = 0.0;
	Config.PromptTokenCostMs = 0.001;
	Config.PromptTokenCostPerPositionMs = 0.0;
	Config.FinishCostMs = 0.001;
	{
		// One row per ring, applied at this Configure() and then restored to the default.
		FSuperSLMSchedulingTestAccess::FScopedReportHistoryCapacity OneRow(1);
		if (!TestEqual(TEXT("Configure()"), (uint8)Cpu->Configure(Model, Config).Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}
	}
	bool bOk = TestEqual(TEXT("precondition: the capacity override was restored after Configure()"),
		FSuperSLMSchedulingTestAccess::GetReportHistoryCapacity(), kReleaseReportRows);
	bOk &= TestEqual(TEXT("precondition: no hitch before the run"), Cpu->GetHitchCount(), 0);

	FSuperSLMSequence A, B;
	if (!TestEqual(TEXT("VendSequence() A"), (uint8)Cpu->VendSequence(A), (uint8)ESuperSLMVendResult::Success) ||
		!TestEqual(TEXT("VendSequence() B"), (uint8)Cpu->VendSequence(B), (uint8)ESuperSLMVendResult::Success))
	{
		return false;
	}
	FString BeginError;
	bOk &= TestTrue(*FString::Printf(TEXT("BeginGeneration() A: %s"), *BeginError), Cpu->BeginGeneration(A, MakeRingRequest(16), BeginError));
	bOk &= TestTrue(*FString::Printf(TEXT("BeginGeneration() B: %s"), *BeginError), Cpu->BeginGeneration(B, MakeRingRequest(16), BeginError));
	if (!bOk)
	{
		Cpu->ReturnSequence(A);
		Cpu->ReturnSequence(B);
		return false;
	}

	// Ticked one at a time so that every tick row is read before the next tick overwrites it.
	const int64 JobsAtStart = Cpu->GetJobLedgerAppendedCount();
	int64 TickRowsRead = 0;
	int64 TickRowsMissed = 0;
	int64 Delivered = 0;
	int64 Late = 0;
	int64 MaxInFlight = 0;
	const double Start = FPlatformTime::Seconds();
	for (;;)
	{
		Cpu->Tick(1.0f / 60.0f);
		const int64 TickRow = Cpu->GetTickHistoryAppendedCount() - 1;
		if (const FSuperSLMTickReport* Tick = Cpu->FindTickHistoryRow(TickRow))
		{
			++TickRowsRead;
			Delivered += Tick->JobsDeliveredThisTick;
			Late += Tick->LateJobsThisTick;
		}
		else
		{
			++TickRowsMissed;
		}
		const int64 InFlight = (Cpu->GetJobLedgerAppendedCount() - JobsAtStart) - Delivered;
		MaxInFlight = FMath::Max(MaxInFlight, InFlight);
		if ((IsStopped(*Cpu, A) && IsStopped(*Cpu, B) && InFlight == 0) || FPlatformTime::Seconds() - Start > 120.0)
		{
			break;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	const int64 Posted = Cpu->GetJobLedgerAppendedCount() - JobsAtStart;
	AddInfo(FString::Printf(TEXT("%lld jobs posted, %lld delivered, %lld late, at most %lld in flight; hitch count %d"),
		Posted, Delivered, Late, MaxInFlight, Cpu->GetHitchCount()));
	bOk &= TestTrue(TEXT("both generations complete"),
		Cpu->GetPhase(A) == ESuperSLMSequencePhase::Complete && Cpu->GetPhase(B) == ESuperSLMSequencePhase::Complete);
	bOk &= TestEqual(TEXT("precondition: the configured ring kept one row"), Cpu->GetJobLedger().Num(), 1);
	bOk &= TestEqual(TEXT("every tick row was read (rows missed)"), TickRowsMissed, (int64)0);
	bOk &= TestEqual(TEXT("every posted job was delivered"), Delivered, Posted);
	// Two jobs in flight with one row: the older one's row was overwritten before its delivery.
	bOk &= TestTrue(TEXT("precondition: at least one job was delivered after its ledger row was overwritten (two in flight at once)"), MaxInFlight >= 2);
	bOk &= TestEqual(TEXT("the hitch count equals every delivered job (each overran 0.1 ms) plus every late one, whether or not its row was retained"),
		(int64)Cpu->GetHitchCount(), Delivered + Late);
	Cpu->ReturnSequence(A);
	Cpu->ReturnSequence(B);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_L2S3
