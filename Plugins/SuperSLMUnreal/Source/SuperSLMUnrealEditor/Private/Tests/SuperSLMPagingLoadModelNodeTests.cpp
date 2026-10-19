// Independent paging Load-node regressions (paging plan 3.1 and 3.5; CPU review CPU-R5).
// Actual factory/Activate/async publication and the existing reflected pin listener are used.
// The default budget is observed in real saved holder magic, and an explicit 512 is observed in
// both engines' saved budget fields at the approved fixed payload offsets. Prefix-count wiring is
// decided by actual admissions, not region statistics. The GPU must come up: CPU-only fallback
// cannot pass the two mode cells. The guard cells use distinct imports, making replacement visible.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3 && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Fixtures/L2S3/SuperSLMLoadModelTestListener.h"
#include "SuperSLMConfigureTestAccess.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMLoadModelAsyncAction.h"
#include "SuperSLMPagedTestSeam.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMPagingLoadNodeTests
{
	constexpr float StepSeconds = 1.0f / 60.0f;
	constexpr double LoadCapSeconds = 180.0;
	constexpr double OperationCapSeconds = 60.0;
	constexpr int32 PoolSize = 2;
	constexpr int32 Budget = 512;

	enum class ECase : uint8 { DefaultLegacy, BudgetAndPrefixes, PrefixAtActivate, PrefixDuringCpu };
	enum class EStage : uint8 { InitialLoad, SavingHolders, BuildingCapacityPrefixes, BuildingGuardPrefix, SecondLoad };

	struct FState
	{
		ECase Case = ECase::DefaultLegacy;
		EStage Stage = EStage::InitialLoad;
		TSharedPtr<FTestWorldWrapper> World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TStrongObjectPtr<USuperSLMModel> SecondModel;
		TStrongObjectPtr<USuperSLMLoadModelTestListener> Listener;
		TWeakObjectPtr<USuperSLMSubsystem> Cpu;
		TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
		TArray<FSuperSLMPrefix> CpuPrefixes;
		TArray<FSuperSLMPrefix> GpuPrefixes;
		FSuperSLMSequence CpuSequence;
		FSuperSLMGpuSequence GpuSequence;
		FSuperSLMLifecycleOpHandle CpuSave;
		FSuperSLMLifecycleOpHandle GpuSave;
		bool bCpuSequenceHeld = false;
		bool bGpuSequenceHeld = false;
		bool bCpuSaveRead = false;
		bool bGpuSaveRead = false;
		bool bPoolJobHeld = false;
		double StageStarted = 0.0;

		void SetStage(EStage Next) { Stage = Next; StageStarted = FPlatformTime::Seconds(); }
	};

	void CleanUp(FState& S)
	{
		// Release our test hold before world destruction can join the pending pool job.
		if (S.bPoolJobHeld)
		{
			FSuperSLMConfigureTestAccess::ReleasePoolJob();
			S.bPoolJobHeld = false;
		}
		if (S.Cpu.IsValid())
		{
			if (S.bCpuSequenceHeld) { S.Cpu->ReturnSequence(S.CpuSequence); }
			for (const FSuperSLMPrefix& Prefix : S.CpuPrefixes) { S.Cpu->ReleasePrefix(Prefix); }
			if (S.CpuSave.IsValid()) { S.Cpu->ReleaseLifecycleOpHandle(S.CpuSave); }
		}
		if (S.Gpu.IsValid())
		{
			if (S.bGpuSequenceHeld) { S.Gpu->ReturnSequence(S.GpuSequence); }
			for (const FSuperSLMPrefix& Prefix : S.GpuPrefixes)
			{
				FString Ignored;
				S.Gpu->ReleasePrefix(Prefix, Ignored);
			}
			if (S.GpuSave.IsValid()) { S.Gpu->ReleaseLifecycleOpHandle(S.GpuSave); }
		}
		S.Listener.Reset();
		S.World.Reset();
		S.SecondModel.Reset();
		S.Model.Reset();
	}

	void StartLoad(FState& S, USuperSLMModel* Model, bool bDefaultBudget, int32 PrefixBlocks)
	{
		S.Listener->Reset();
		// The first form is an earlier full call: the appended SequenceBudget is deliberately
		// omitted. This guards its default and preserves existing caller syntax.
		USuperSLMLoadModelAsyncAction* Action = bDefaultBudget
			? USuperSLMLoadModelAsyncAction::LoadSuperSLMModelAsync(S.World->GetTestWorld(), Model,
				true, PoolSize, 100000.0f, 100000.0f, 100000.0f, PrefixBlocks)
			: USuperSLMLoadModelAsyncAction::LoadSuperSLMModelAsync(S.World->GetTestWorld(), Model,
				true, PoolSize, 100000.0f, 100000.0f, 100000.0f, PrefixBlocks, Budget);
		Action->Loaded.AddDynamic(S.Listener.Get(), &USuperSLMLoadModelTestListener::OnLoaded);
		Action->Failed.AddDynamic(S.Listener.Get(), &USuperSLMLoadModelTestListener::OnFailed);
		Action->Activate();
		S.StageStarted = FPlatformTime::Seconds();
	}

	bool SetUp(FAutomationTestBase& T, FState& S)
	{
		FString Path, Reason;
		if (!SuperSLML2S3Fixtures::TryGetAExArtifactPath(Path, Reason))
		{
			T.AddError(Reason);
			return false;
		}
		FSuperSLMImportDiagnostic Diagnostic;
		S.Model.Reset(FSuperSLMModelImport::ImportFromFile(Path, Diagnostic));
		if (!T.TestTrue(TEXT("real A-EX imports"), S.Model.IsValid() && Diagnostic.bAccepted)) { return false; }
		S.World = MakeShared<FTestWorldWrapper>();
		if (!T.TestTrue(TEXT("game world is created"), S.World->CreateTestWorld(EWorldType::Game))) { return false; }
		S.Cpu = SuperSLML2S3Fixtures::GetSubsystem(S.World->GetTestWorld());
		S.Gpu = SuperSLML2S3Fixtures::GetGpuSubsystem(S.World->GetTestWorld());
		if (!T.TestTrue(TEXT("both game-instance subsystems exist"), S.Cpu.IsValid() && S.Gpu.IsValid())) { return false; }
		S.Listener.Reset(NewObject<USuperSLMLoadModelTestListener>());
		StartLoad(S, S.Model.Get(), S.Case == ECase::DefaultLegacy,
			S.Case == ECase::DefaultLegacy ? 0 : S.Case == ECase::BudgetAndPrefixes ? 2 : 1);
		return true;
	}

	bool ImportSecond(FAutomationTestBase& T, FState& S)
	{
		FString Path, Reason;
		if (!SuperSLML2S3Fixtures::TryGetAExArtifactPath(Path, Reason)) { T.AddError(Reason); return false; }
		FSuperSLMImportDiagnostic Diagnostic;
		S.SecondModel.Reset(FSuperSLMModelImport::ImportFromFile(Path, Diagnostic));
		return T.TestTrue(TEXT("second import is a distinct asset identity"),
			S.SecondModel.IsValid() && Diagnostic.bAccepted && S.SecondModel.Get() != S.Model.Get());
	}

	bool CheckLoadedBoth(FAutomationTestBase& T, const FState& S)
	{
		bool bOk = T.TestEqual(TEXT("one completion pin"), S.Listener->Calls, 1);
		bOk &= T.TestTrue(*FString::Printf(TEXT("Loaded pin: %s"), *S.Listener->Result.Message), S.Listener->bLoaded);
		bOk &= T.TestTrue(TEXT("both configured backends are reported"), S.Listener->Result.bCpuConfigured && S.Listener->Result.bGpuBackendActive);
		bOk &= T.TestTrue(TEXT("CPU publishes the requested model"), S.Cpu->GetConfiguredModel() == S.Model.Get());
		bOk &= T.TestTrue(TEXT("GPU actually publishes the requested model"), S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get());
		bOk &= T.TestTrue(TEXT("final GPU K meets its public minimum"), S.Gpu->GetConfiguredMinimumK() > 0 && S.Gpu->GetConfiguredK() >= S.Gpu->GetConfiguredMinimumK());
		return bOk;
	}

	bool StartSaves(FAutomationTestBase& T, FState& S)
	{
		const ESuperSLMVendResult CpuVend = S.Cpu->VendSequence(S.CpuSequence);
		S.bCpuSequenceHeld = CpuVend == ESuperSLMVendResult::Success;
		const ESuperSLMGpuVendResult GpuVend = S.Gpu->VendSequence(S.GpuSequence, ESuperSLMGpuDecodePath::Composed);
		S.bGpuSequenceHeld = GpuVend == ESuperSLMGpuVendResult::Success;
		if (!T.TestTrue(TEXT("the loaded pools vend real CPU and GPU holders"), S.bCpuSequenceHeld && S.bGpuSequenceHeld)) { return false; }
		FString Error;
		const ESuperSLMRestoreResult Queued = S.Cpu->SaveSequence(S.CpuSequence, S.CpuSave, Error);
		S.GpuSave = S.Gpu->RequestSaveSequence(S.GpuSequence);
		S.SetStage(EStage::SavingHolders);
		return T.TestEqual(TEXT("CPU save is admitted"), uint8(Queued), uint8(ESuperSLMRestoreResult::Success))
			&& T.TestTrue(TEXT("both queued saves have handles"), S.CpuSave.IsValid() && S.GpuSave.IsValid());
	}

	bool CheckHolderPayload(FAutomationTestBase& T, TArray<uint8>& Blob, bool bGpu, bool bBudget)
	{
		bool bObserved = false;
		bool bOk = true;
		// This existing seam exposes the shared SLUW payload without assigning a second wrapper
		// layout to the oracle. We inspect only; no payload byte is changed. The payload fields
		// below are the independent fixed offsets in approved paging plan section 3.5.
		const bool bRead = FSuperSLMPagedTestSeam::EditPayload(Blob, [&](TArray<uint8>& Payload)
		{
			bObserved = true;
			const ANSICHAR* Magic = bGpu ? (bBudget ? "SLM6" : "SLM5") : (bBudget ? "SSB6" : "SSB5");
			bOk &= T.TestTrue(bGpu ? TEXT("GPU saved actual holder-mode magic") : TEXT("CPU saved actual holder-mode magic"),
				Payload.Num() >= 4 && FMemory::Memcmp(Payload.GetData(), Magic, 4) == 0);
			if (bBudget)
			{
				const int32 BudgetOffset = bGpu ? 136 : 160;
				if (!T.TestTrue(TEXT("saved budget field is present"), Payload.Num() >= BudgetOffset + 4)) { bOk = false; return; }
				uint32 ActualBudget = 0;
				for (int32 I = 0; I < 4; ++I) { ActualBudget |= uint32(Payload[BudgetOffset + I]) << (8 * I); }
				bOk &= T.TestEqual(bGpu ? TEXT("GPU holder received budget exactly 512") : TEXT("CPU holder received budget exactly 512"), ActualBudget, uint32(Budget));
			}
		});
		return T.TestTrue(TEXT("real save payload is readable"), bRead && bObserved) && bOk;
	}

	bool CreateCapacityPrefixes(FAutomationTestBase& T, FState& S)
	{
		// Empty prefixes are explicitly admitted in budget mode (plan 3.3). They execute real
		// Begin/freeze jobs while avoiding an unrelated prompt-throughput workload.
		for (int32 I = 0; I < 2; ++I)
		{
			FSuperSLMPrefix CpuPrefix, GpuPrefix;
			FString Error;
			const ESuperSLMPrefixResult CpuResult = S.Cpu->CreatePrefix(TArray<int32>(), CpuPrefix, &Error);
			const ESuperSLMPrefixResult GpuResult = S.Gpu->CreatePrefix(TArray<int32>(), GpuPrefix, &Error);
			if (CpuPrefix.IsValid()) { S.CpuPrefixes.Add(CpuPrefix); }
			if (GpuPrefix.IsValid()) { S.GpuPrefixes.Add(GpuPrefix); }
			if (!T.TestEqual(TEXT("CPU prefix count 2 admits each of two builds"), uint8(CpuResult), uint8(ESuperSLMPrefixResult::Success))
				|| !T.TestEqual(TEXT("GPU prefix count 2 admits each of two builds"), uint8(GpuResult), uint8(ESuperSLMPrefixResult::Success))) { return false; }
		}
		FSuperSLMPrefix Extra;
		FString Error;
		bool bOk = T.TestEqual(TEXT("CPU third prefix is PrefixLimit"), uint8(S.Cpu->CreatePrefix(TArray<int32>(), Extra, &Error)), uint8(ESuperSLMPrefixResult::PrefixLimit));
		bOk &= T.TestFalse(TEXT("CPU refusal mints no prefix"), Extra.IsValid());
		bOk &= T.TestEqual(TEXT("GPU third prefix is PrefixLimit"), uint8(S.Gpu->CreatePrefix(TArray<int32>(), Extra, &Error)), uint8(ESuperSLMPrefixResult::PrefixLimit));
		bOk &= T.TestFalse(TEXT("GPU refusal mints no prefix"), Extra.IsValid());
		S.SetStage(EStage::BuildingCapacityPrefixes);
		return bOk;
	}

	bool GuardConstruction(FAutomationTestBase& T, FState& S)
	{
		if (!ImportSecond(T, S)) { return false; }
		if (S.Case == ECase::PrefixDuringCpu)
		{
			FSuperSLMConfigureTestAccess::HoldNextPoolJob();
			S.bPoolJobHeld = true;
			StartLoad(S, S.SecondModel.Get(), false, 1);
			if (!T.TestEqual(TEXT("held CPU prepare has fired no load pin"), S.Listener->Calls, 0)
				|| !T.TestTrue(TEXT("CPU BeginConfigure is held pending"), S.Cpu->IsConfigurePending())
				|| !T.TestTrue(TEXT("old GPU backend remains configured during CPU prepare"), S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get())) { return false; }
		}
		FSuperSLMPrefix Prefix;
		FString Error;
		const ESuperSLMPrefixResult Result = S.Gpu->CreatePrefix(TArray<int32>(), Prefix, &Error);
		if (Prefix.IsValid()) { S.GpuPrefixes.Add(Prefix); }
		S.SetStage(EStage::BuildingGuardPrefix);
		return T.TestEqual(*FString::Printf(TEXT("real GPU prefix build is admitted: %s"), *Error), uint8(Result), uint8(ESuperSLMPrefixResult::Success));
	}

	bool AllPrefixesReady(const FState& S, bool& bFaulted)
	{
		bFaulted = false;
		bool bReady = true;
		for (const FSuperSLMPrefix& Prefix : S.CpuPrefixes)
		{
			const ESuperSLMPrefixPhase Phase = S.Cpu->GetPrefixPhase(Prefix);
			bFaulted |= Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid;
			bReady &= Phase == ESuperSLMPrefixPhase::Ready;
		}
		for (const FSuperSLMPrefix& Prefix : S.GpuPrefixes)
		{
			const ESuperSLMPrefixPhase Phase = S.Gpu->GetPrefixPhase(Prefix);
			bFaulted |= Phase == ESuperSLMPrefixPhase::Faulted || Phase == ESuperSLMPrefixPhase::Invalid;
			bReady &= Phase == ESuperSLMPrefixPhase::Ready;
		}
		return bReady;
	}

	bool Step(FAutomationTestBase& T, FState& S)
	{
		auto Finish = [&S]() { CleanUp(S); return true; };
		if (!S.Cpu.IsValid() || !S.Gpu.IsValid()) { T.AddError(TEXT("load test subsystem was destroyed")); return Finish(); }
		// One latent invocation per editor frame; actual asynchronous Configure callbacks publish
		// on the game thread between invocations. Explicit ticks drive real save/prefix jobs.
		S.Cpu->Tick(StepSeconds);
		S.Gpu->Tick(StepSeconds);
		const bool bLoading = S.Stage == EStage::InitialLoad || S.Stage == EStage::SecondLoad;
		if (FPlatformTime::Seconds() - S.StageStarted > (bLoading ? LoadCapSeconds : OperationCapSeconds))
		{
			T.AddError(FString::Printf(TEXT("paging Load-node stage %d timed out"), int32(S.Stage)));
			return Finish();
		}
		if (S.Stage == EStage::InitialLoad)
		{
			if (S.Listener->Calls == 0) { return false; }
			if (!CheckLoadedBoth(T, S)) { return Finish(); }
			const bool bBudget = S.Case != ECase::DefaultLegacy;
			if (!T.TestEqual(TEXT("CPU publishes requested mode"), S.Cpu->GetKvPageStats().bPagedMode, bBudget)
				|| !T.TestEqual(TEXT("GPU publishes requested mode"), S.Gpu->GetKvPageStats().bPagedMode, bBudget)) { return Finish(); }
			if (S.Case == ECase::DefaultLegacy || S.Case == ECase::BudgetAndPrefixes)
			{
				if (!StartSaves(T, S)) { return Finish(); }
			}
			else if (!GuardConstruction(T, S)) { return Finish(); }
			return false;
		}
		if (S.Stage == EStage::SavingHolders)
		{
			auto ReadSave = [&](bool bGpu)
			{
				bool& bRead = bGpu ? S.bGpuSaveRead : S.bCpuSaveRead;
				if (bRead) { return true; }
				TArray<uint8> Blob;
				const ESuperSLMRestoreResult Result = bGpu ? S.Gpu->GetSaveResult(S.GpuSave, Blob) : S.Cpu->GetSaveResult(S.CpuSave, Blob);
				if (Result == ESuperSLMRestoreResult::Pending) { return true; }
				bRead = true;
				return T.TestEqual(bGpu ? TEXT("actual GPU save completes") : TEXT("actual CPU save completes"), uint8(Result), uint8(ESuperSLMRestoreResult::Success))
					&& CheckHolderPayload(T, Blob, bGpu, S.Case == ECase::BudgetAndPrefixes);
			};
			if (!ReadSave(false) || !ReadSave(true)) { return Finish(); }
			if (!S.bCpuSaveRead || !S.bGpuSaveRead) { return false; }
			if (S.Case == ECase::DefaultLegacy) { return Finish(); }
			if (!CreateCapacityPrefixes(T, S)) { return Finish(); }
			return false;
		}
		if (S.Stage == EStage::BuildingCapacityPrefixes || S.Stage == EStage::BuildingGuardPrefix)
		{
			bool bFaulted = false;
			const bool bReady = AllPrefixesReady(S, bFaulted);
			if (bFaulted) { T.AddError(TEXT("an admitted prefix faulted or became invalid")); return Finish(); }
			if (!bReady) { return false; }
			if (S.Stage == EStage::BuildingCapacityPrefixes)
			{
				T.TestEqual(TEXT("CPU retains exactly two usable prefixes"), S.Cpu->GetLivePrefixCount(), 2);
				T.TestEqual(TEXT("GPU retains exactly two usable prefixes"), S.Gpu->GetLivePrefixCount(), 2);
				return Finish();
			}
			if (S.Case == ECase::PrefixAtActivate)
			{
				// Only GPU prefix liveness can block this load: no CPU resource, sequence or
				// adapter is held, and the target is a different model asset.
				T.TestEqual(TEXT("construction: CPU has no live prefix"), S.Cpu->GetLivePrefixCount(), 0);
				T.TestEqual(TEXT("construction: CPU has no vended sequence"), S.Cpu->GetPoolOccupiedCount(), 0);
				T.TestEqual(TEXT("construction: GPU has no vended sequence"), S.Gpu->GetPoolOccupiedCount(), 0);
				T.TestEqual(TEXT("construction: GPU has no mapped adapter"), S.Gpu->GetMappedAdapterCount(), 0);
				StartLoad(S, S.SecondModel.Get(), false, 1);
				T.TestEqual(TEXT("live GPU prefix refuses synchronously before any prepare"), S.Listener->Calls, 1);
				T.TestFalse(TEXT("live GPU prefix fires Failed"), S.Listener->bLoaded);
				T.TestTrue(TEXT("initial refusal names the GPU prefix"), S.Listener->Result.Message.Contains(TEXT("refused"))
					&& S.Listener->Result.Message.Contains(TEXT("GPU")) && S.Listener->Result.Message.Contains(TEXT("1 prefix")));
				T.TestFalse(TEXT("neither backend begins configuration"), S.Cpu->IsConfigurePending() || S.Gpu->IsConfigurePending());
				T.TestTrue(TEXT("CPU was not torn down or replaced"), S.Cpu->GetConfiguredModel() == S.Model.Get() && S.Cpu->GetResolvedConfig() != nullptr);
				T.TestTrue(TEXT("GPU model survives the initial refusal"), S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get());
				T.TestEqual(TEXT("GPU prefix remains Ready after initial refusal"), uint8(S.Gpu->GetPrefixPhase(S.GpuPrefixes[0])), uint8(ESuperSLMPrefixPhase::Ready));
				return Finish();
			}
			if (!T.TestTrue(TEXT("prefix reached Ready while CPU pool job remained held"), S.bPoolJobHeld && S.Cpu->IsConfigurePending())
				|| !T.TestEqual(TEXT("held CPU prepare has still fired no pin"), S.Listener->Calls, 0)) { return Finish(); }
			FSuperSLMConfigureTestAccess::ReleasePoolJob();
			S.bPoolJobHeld = false;
			S.SetStage(EStage::SecondLoad);
			return false;
		}
		// The post-CPU busy check must preserve the GPU state created during that CPU phase.
		if (S.Listener->Calls == 0) { return false; }
		T.TestEqual(TEXT("second-check path fires exactly one pin"), S.Listener->Calls, 1);
		T.TestTrue(TEXT("second-check path finishes Loaded with CPU only"), S.Listener->bLoaded && S.Listener->Result.bCpuConfigured && !S.Listener->Result.bGpuBackendActive);
		T.TestTrue(TEXT("CPU publishes the second load's model"), S.Cpu->GetConfiguredModel() == S.SecondModel.Get());
		T.TestTrue(TEXT("second GPU check names why it skipped reconfiguration"), S.Listener->Result.Message.Contains(TEXT("GPU backend not reconfigured"))
			&& S.Listener->Result.Message.Contains(TEXT("1 prefix")));
		T.TestFalse(TEXT("GPU has no replacement configure pending"), S.Gpu->IsConfigurePending());
		T.TestTrue(TEXT("GPU retains its original active model"), S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get());
		T.TestEqual(TEXT("GPU prefix created during CPU prepare stays live"), S.Gpu->GetLivePrefixCount(), 1);
		T.TestEqual(TEXT("GPU prefix created during CPU prepare remains Ready"), uint8(S.Gpu->GetPrefixPhase(S.GpuPrefixes[0])), uint8(ESuperSLMPrefixPhase::Ready));
		return Finish();
	}

	bool Run(FAutomationTestBase& T, ECase Case)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->Case = Case;
		if (!SetUp(T, *State)) { CleanUp(*State); return false; }
		FAutomationTestBase* Test = &T;
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, State]() { return Step(*Test, *State); }));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLoadDefaultTest, "SuperSLM.Paging.Blueprint.Load.DefaultLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingLoadDefaultTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLoadNodeTests::Run(*this, SuperSLMPagingLoadNodeTests::ECase::DefaultLegacy);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLoadBudgetTest, "SuperSLM.Paging.Blueprint.Load.BudgetBothBackendsAndPrefixes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingLoadBudgetTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLoadNodeTests::Run(*this, SuperSLMPagingLoadNodeTests::ECase::BudgetAndPrefixes);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLoadPrefixInitialTest, "SuperSLM.Paging.Blueprint.Load.GpuPrefixRefusedBeforeCpuTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingLoadPrefixInitialTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLoadNodeTests::Run(*this, SuperSLMPagingLoadNodeTests::ECase::PrefixAtActivate);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingLoadPrefixDuringCpuTest, "SuperSLM.Paging.Blueprint.Load.GpuPrefixCreatedDuringCpuPrepare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingLoadPrefixDuringCpuTest::RunTest(const FString& Parameters)
{
	return SuperSLMPagingLoadNodeTests::Run(*this, SuperSLMPagingLoadNodeTests::ECase::PrefixDuringCpu);
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3 && SUPERSLM_WITH_L2S1_ASYNC
