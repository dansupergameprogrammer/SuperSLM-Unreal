// T-2818 (L2-S3). The "Load SuperSLM Model" Blueprint node (USuperSLMLoadModelAsyncAction,
// SuperSLMLoadModelAsyncAction.h; box record items 14-15; review round 5 R5-W1, R5-N3).
//
// Driven the way the Blueprint node drives it: the factory, a binding on each pin, then
// Activate(), with the pins read from a UObject listener and each load's completion waited for one
// editor frame at a time (BeginConfigure() delivers on the game thread).
//
// Cell 1 (A-EX):
//   1. It loads on both backends: Loaded fires once, the result reports the CPU configured and the
//      GPU backend active, and each subsystem holds this model. Each backend then vends, so the
//      load is usable, not only reported.
//   2. It refuses by name at Activate() while a backend it would reconfigure holds a vended
//      sequence, on each backend separately, and nothing is torn down.
//   3. R5-N3: it refuses at Activate() while a CPU shared prefix is live, naming the prefix count,
//      and the prefix survives.
//   4. It refuses at Activate() while another load's CPU configure is in flight.
//   5. R5-W1: a GPU sequence vended AFTER Activate() -- during the CPU phase, before the CPU's
//      configure delivers -- leaves the GPU untouched: the node finishes Loaded with the CPU only,
//      naming why ("GPU backend not reconfigured: ..."), the held sequence is still counted by the
//      GPU pool, and the GPU still holds the model it had. This load uses a second import of A-EX,
//      so "CPU only" is visible: the result's bGpuBackendActive is false because the GPU does not
//      hold the model this load was given.
// Cell 2 (A-AD's base, R5-N3's adapter half): with an adapter mapped on the GPU subsystem, the node
//   refuses at Activate(), naming the mapped-adapter count, and the mapping survives. A CPU adapter
//   handle, which survives a CPU reconfigure, does not refuse a load, and releases afterwards.
// The GPU is claimed (the dev box's RTX 2080 SUPER): a GPU backend that does not come up fails the
// cell rather than passing on the CPU alone.

#include "Misc/AutomationTest.h"

#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3

#include "Fixtures/L2S3/SuperSLML2S3Fixtures.h"
#include "Fixtures/L2S3/SuperSLMLoadModelTestListener.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMLoadModelAsyncAction.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h" // FTestWorldWrapper, FFunctionLatentCommand
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMLoadNode
{
	constexpr double kLoadCapSeconds = 180.0;
	constexpr double kPrefixReadyCapSeconds = 60.0;

	enum class EStage : uint8
	{
		InitialLoad,     // waiting for the first load's pin
		VendedDuringCpu, // R5-W1: waiting for the pin of a load a GPU sequence was vended under
		CpuAdapterHeld,  // cell 2: waiting for the pin of a load made while a CPU adapter is held
	};

	struct FState
	{
		TSharedPtr<FTestWorldWrapper> World;
		TStrongObjectPtr<USuperSLMModel> Model;
		TStrongObjectPtr<USuperSLMModel> SecondModel; // R5-W1: a second import of the same file
		TStrongObjectPtr<USuperSLMLoadModelTestListener> Listener;
		TStrongObjectPtr<USuperSLMLoadModelTestListener> SecondListener;
		FSuperSLMAdapterHandle CpuAdapter; // cell 2
		TWeakObjectPtr<USuperSLMSubsystem> Cpu;
		TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
		int32 PoolSize = 2;
		EStage Stage = EStage::InitialLoad;
		double StartSeconds = 0.0;
		FSuperSLMGpuSequence HeldGpu;
		bool bHoldingGpu = false;
	};

	void StartLoadWith(FState& S, USuperSLMModel* Model, USuperSLMLoadModelTestListener* Listener)
	{
		Listener->Reset();
		USuperSLMLoadModelAsyncAction* Action = USuperSLMLoadModelAsyncAction::LoadSuperSLMModelAsync(
			S.World->GetTestWorld(), Model, /*bConfigureGpu*/ true, S.PoolSize,
			/*CpuTickBudgetMs*/ 100000.0f, /*GpuTickBudgetMs*/ 100000.0f, /*SequenceLifecycleBudgetMs*/ 100000.0f,
			/*PrefixBlockCount*/ 1); // CheckRefusedWhilePrefixLive's one live prefix
		Action->Loaded.AddDynamic(Listener, &USuperSLMLoadModelTestListener::OnLoaded);
		Action->Failed.AddDynamic(Listener, &USuperSLMLoadModelTestListener::OnFailed);
		S.StartSeconds = FPlatformTime::Seconds();
		Action->Activate();
	}

	void StartLoad(FState& S)
	{
		StartLoadWith(S, S.Model.Get(), S.Listener.Get());
	}

	// Imports Path, creates the test world, reaches both subsystems and starts the first load.
	bool SetUp(FAutomationTestBase& T, FState& S, const FString& Path, const TCHAR* What)
	{
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		if (Model == nullptr || !Diag.bAccepted)
		{
			T.AddError(FString::Printf(TEXT("Load SuperSLM Model: %s import from '%s': %s"), What, *Path, *Diag.Message));
			return false;
		}
		S.Model.Reset(Model); // held across frames; the editor collects garbage between them
		S.World = MakeShared<FTestWorldWrapper>();
		if (!S.World->CreateTestWorld(EWorldType::Game))
		{
			T.AddError(TEXT("Load SuperSLM Model: the test game world was not created."));
			return false;
		}
		S.Cpu = SuperSLML2S3Fixtures::GetSubsystem(S.World->GetTestWorld());
		S.Gpu = SuperSLML2S3Fixtures::GetGpuSubsystem(S.World->GetTestWorld());
		if (!S.Cpu.IsValid() || !S.Gpu.IsValid())
		{
			T.AddError(TEXT("Load SuperSLM Model: a SuperSLM subsystem is unreachable on the test world's game instance."));
			return false;
		}
		if (!T.TestTrue(TEXT("construction: nothing is vended before the load"),
				S.Cpu->GetPoolOccupiedCount() == 0 && S.Gpu->GetPoolOccupiedCount() == 0))
		{
			return false;
		}
		S.Listener.Reset(NewObject<USuperSLMLoadModelTestListener>());
		StartLoad(S);
		return true;
	}

	void TearDown(FState& S)
	{
		if (S.bHoldingGpu && S.Gpu.IsValid())
		{
			S.Gpu->ReturnSequence(S.HeldGpu);
		}
		S.bHoldingGpu = false;
		if (S.CpuAdapter.IsValid())
		{
			FString Ignored;
			FSuperSLMAdapterImport::Release(S.CpuAdapter, Ignored);
		}
		S.Listener.Reset();
		S.SecondListener.Reset();
		S.SecondModel.Reset();
		S.World.Reset();
		S.Model.Reset();
	}

	// True while the pin has not fired and the cap has not passed; on the cap, fails and returns false.
	bool StillWaiting(FAutomationTestBase& T, const FState& S, bool& bOutTimedOut)
	{
		bOutTimedOut = false;
		if (S.Listener->Calls > 0)
		{
			return false;
		}
		if (FPlatformTime::Seconds() - S.StartSeconds > kLoadCapSeconds)
		{
			T.AddError(FString::Printf(TEXT("Load SuperSLM Model: neither pin fired within %.0f s."), kLoadCapSeconds));
			bOutTimedOut = true;
			return false;
		}
		return true;
	}

	bool CheckLoadedOnBothBackends(FAutomationTestBase& T, const FState& S)
	{
		USuperSLMSubsystem* Cpu = S.Cpu.Get();
		USuperSLMGpuSubsystem* Gpu = S.Gpu.Get();
		T.AddInfo(FString::Printf(TEXT("Load SuperSLM Model: %s -- %s"), S.Listener->bLoaded ? TEXT("Loaded") : TEXT("Failed"), *S.Listener->Result.Message));
		bool bOk = T.TestEqual(TEXT("load: exactly one pin fires"), S.Listener->Calls, 1);
		bOk &= T.TestTrue(FString::Printf(TEXT("load: the pin is Loaded (%s)"), *S.Listener->Result.Message), S.Listener->bLoaded);
		bOk &= T.TestTrue(TEXT("load: the result reports the CPU configured"), S.Listener->Result.bCpuConfigured);
		bOk &= T.TestTrue(FString::Printf(TEXT("load: the result reports the GPU backend active (%s)"), *S.Listener->Result.Message),
			S.Listener->Result.bGpuBackendActive);
		bOk &= T.TestTrue(TEXT("load: the CPU subsystem holds this model"), Cpu->GetConfiguredModel() == S.Model.Get());
		bOk &= T.TestTrue(TEXT("load: the GPU backend is active with this model"),
			Gpu->IsGpuBackendActive() && Gpu->GetConfiguredModel() == S.Model.Get());
		return bOk;
	}

	// The refusal at Activate(): one Failed pin, already fired when Activate() returns, whose
	// message contains "refused" and every one of Names.
	bool CheckRefusedAtActivate(FAutomationTestBase& T, const FState& S, const FString& Label, std::initializer_list<FString> Names)
	{
		bool bOk = T.TestEqual(FString::Printf(TEXT("%s: exactly one pin fires, at Activate()"), *Label), S.Listener->Calls, 1);
		bOk &= T.TestFalse(FString::Printf(TEXT("%s: the pin is Failed"), *Label), S.Listener->bLoaded);
		bool bNamed = S.Listener->Result.Message.Contains(TEXT("refused"));
		for (const FString& Name : Names)
		{
			bNamed &= S.Listener->Result.Message.Contains(Name);
		}
		bOk &= T.TestTrue(FString::Printf(TEXT("%s: the message names the refusal (\"%s\")"), *Label, *S.Listener->Result.Message), bNamed);
		bOk &= T.TestFalse(FString::Printf(TEXT("%s: no configure was started"), *Label),
			S.Cpu->IsConfigurePending() || S.Gpu->IsConfigurePending());
		bOk &= T.TestTrue(FString::Printf(TEXT("%s: the CPU still holds the loaded model"), *Label), S.Cpu->GetConfiguredModel() == S.Model.Get());
		bOk &= T.TestTrue(FString::Printf(TEXT("%s: the GPU backend is still active with the loaded model"), *Label),
			S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get());
		return bOk;
	}

	// With one sequence vended on the named backend, the node refuses at Activate(), by name, and
	// tears nothing down.
	bool CheckRefusedWhileVended(FAutomationTestBase& T, FState& S, bool bHoldOnGpu)
	{
		USuperSLMSubsystem* Cpu = S.Cpu.Get();
		USuperSLMGpuSubsystem* Gpu = S.Gpu.Get();
		const TCHAR* Which = bHoldOnGpu ? TEXT("GPU") : TEXT("CPU");
		FSuperSLMSequence CpuSeq;
		FSuperSLMGpuSequence GpuSeq;
		const bool bVended = bHoldOnGpu
			? Gpu->VendSequence(GpuSeq, ESuperSLMGpuDecodePath::Composed) == ESuperSLMGpuVendResult::Success
			: Cpu->VendSequence(CpuSeq) == ESuperSLMVendResult::Success;
		if (!T.TestTrue(FString::Printf(TEXT("construction: a %s sequence vends after the load"), Which), bVended))
		{
			return false;
		}

		StartLoad(S);
		const int32 ExpectedCpu = bHoldOnGpu ? 0 : 1;
		const int32 ExpectedGpu = bHoldOnGpu ? 1 : 0;
		const FString Label = FString::Printf(TEXT("%s sequence held"), Which);
		bool bOk = CheckRefusedAtActivate(T, S, Label, {bHoldOnGpu
			? FString(TEXT("the GPU backend still holds 1 vended sequence(s) and 0 mapped adapter(s)"))
			: FString(TEXT("the CPU backend still holds 1 vended sequence(s) and 0 prefix(es)"))});
		bOk &= T.TestEqual(Label + TEXT(": the CPU pool still counts its vended sequences"), Cpu->GetPoolOccupiedCount(), ExpectedCpu);
		bOk &= T.TestEqual(Label + TEXT(": the GPU pool still counts its vended sequences"), Gpu->GetPoolOccupiedCount(), ExpectedGpu);

		if (bHoldOnGpu)
		{
			Gpu->ReturnSequence(GpuSeq);
		}
		else
		{
			Cpu->ReturnSequence(CpuSeq);
		}
		return bOk;
	}

	// R5-N3, prefix half: a live CPU shared prefix refuses the load, by name, and survives it.
	bool CheckRefusedWhilePrefixLive(FAutomationTestBase& T, FState& S)
	{
		USuperSLMSubsystem* Cpu = S.Cpu.Get();
		TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
		FString Error;
		const bool bCases = SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error);
		if (!T.TestTrue(FString::Printf(TEXT("construction: R-S1a recorded cases load (%s)"), *Error), bCases && Cases.Num() > 0))
		{
			return false;
		}
		// R-S1a's recorded prompt ids, from the Qwen2.5 tokenizer A-EX carries.
		FSuperSLMPrefix Prefix;
		const bool bCreated = Cpu->CreatePrefix(Cases[0].PromptTokens, Prefix, Error);
		if (!T.TestTrue(FString::Printf(TEXT("construction: a CPU shared prefix is created (%s)"), *Error), bCreated))
		{
			return false;
		}
		// Ready, so the prefix is the kind a game holds across queries, not one still being built.
		const double Start = FPlatformTime::Seconds();
		while (!Cpu->IsPrefixReady(Prefix) && FPlatformTime::Seconds() - Start < kPrefixReadyCapSeconds)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!T.TestTrue(TEXT("construction: the prefix is Ready"), Cpu->IsPrefixReady(Prefix)))
		{
			Cpu->ReleasePrefix(Prefix);
			return false;
		}

		StartLoad(S);
		bool bOk = CheckRefusedAtActivate(T, S, TEXT("prefix live"),
			{FString(TEXT("the CPU backend still holds 0 vended sequence(s) and 1 prefix(es)"))});
		bOk &= T.TestTrue(TEXT("prefix live: the prefix is still Ready after the refusal"), Cpu->IsPrefixReady(Prefix));
		Cpu->ReleasePrefix(Prefix);
		// ReleasePrefix removes the entry from the subsystem's table at once, so its phase reads
		// Invalid on the first check and this loop does not normally tick; it is a guard, not a
		// drain. The worker-side release it queued is not waited for: the next arm counts only
		// live table entries (now 0), and a reconfigure's teardown releases any queued handle.
		const double ReleaseStart = FPlatformTime::Seconds();
		while (Cpu->GetPrefixPhase(Prefix) != ESuperSLMPrefixPhase::Invalid && FPlatformTime::Seconds() - ReleaseStart < kPrefixReadyCapSeconds)
		{
			Cpu->Tick(1.0f / 60.0f);
			FPlatformProcess::Sleep(0.001f);
		}
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3LoadModelNodeTest,
	"SuperSLM.L2S3.Blueprint.LoadModelNodeLoadsBothBackendsAndRefusesWhileVended",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3LoadModelNodeTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMLoadNode;

	TSharedRef<FState> State = MakeShared<FState>();
	FString AExPath, Reason;
	if (!SuperSLML2S3Fixtures::TryGetAExArtifactPath(AExPath, Reason))
	{
		AddError(FString::Printf(TEXT("Load SuperSLM Model: %s"), *Reason));
		return false;
	}
	if (!SetUp(*this, *State, AExPath, TEXT("A-EX")))
	{
		TearDown(*State);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		auto Finish = [&S]() { TearDown(S); return true; };
		bool bTimedOut = false;
		if (StillWaiting(*this, S, bTimedOut))
		{
			return false; // next frame
		}
		if (bTimedOut)
		{
			return Finish();
		}
		if (!S.Cpu.IsValid() || !S.Gpu.IsValid())
		{
			AddError(TEXT("Load SuperSLM Model: a subsystem was destroyed during the load."));
			return Finish();
		}

		if (S.Stage == EStage::InitialLoad)
		{
			if (!CheckLoadedOnBothBackends(*this, S))
			{
				return Finish(); // the arms below need both backends loaded
			}
			// Refusals at Activate(). The vends are also the load's usability check.
			CheckRefusedWhileVended(*this, S, /*bHoldOnGpu*/ true);
			CheckRefusedWhileVended(*this, S, /*bHoldOnGpu*/ false);
			CheckRefusedWhilePrefixLive(*this, S);

			// R5-W1: nothing is vended at Activate(), so the load starts; a GPU sequence is then
			// vended while the CPU configure is in flight (its OnDone arrives on a later game-thread
			// task, so it cannot have delivered yet). The GPU still holds the loaded model here.
			if (!TestTrue(TEXT("R5-W1 construction: nothing is vended before the load"),
					S.Cpu->GetPoolOccupiedCount() == 0 && S.Gpu->GetPoolOccupiedCount() == 0))
			{
				return Finish();
			}
			// A second import of A-EX, so the GPU (still holding the first) does not hold this load's model.
			FString AExPath, Reason;
			FSuperSLMImportDiagnostic Diag;
			USuperSLMModel* Second = SuperSLML2S3Fixtures::TryGetAExArtifactPath(AExPath, Reason)
				? FSuperSLMModelImport::ImportFromFile(AExPath, Diag) : nullptr;
			if (!TestTrue(FString::Printf(TEXT("R5-W1 construction: a second A-EX import (%s%s)"), *Reason, *Diag.Message),
					Second != nullptr && Diag.bAccepted && Second != S.Model.Get()))
			{
				return Finish();
			}
			S.SecondModel.Reset(Second);
			StartLoadWith(S, Second, S.Listener.Get());
			if (!TestEqual(TEXT("R5-W1 construction: the load passed Activate() (no pin yet)"), S.Listener->Calls, 0) ||
				!TestTrue(TEXT("R5-W1 construction: the CPU configure is in flight"), S.Cpu->IsConfigurePending()))
			{
				return Finish();
			}

			// A configure in flight refuses another load at Activate(), by name.
			S.SecondListener.Reset(NewObject<USuperSLMLoadModelTestListener>());
			StartLoadWith(S, S.Model.Get(), S.SecondListener.Get());
			TestEqual(TEXT("configure in flight: exactly one pin fires, at Activate()"), S.SecondListener->Calls, 1);
			TestFalse(TEXT("configure in flight: the pin is Failed"), S.SecondListener->bLoaded);
			TestTrue(FString::Printf(TEXT("configure in flight: the message names it (\"%s\")"), *S.SecondListener->Result.Message),
				S.SecondListener->Result.Message.Contains(TEXT("refused")) &&
				S.SecondListener->Result.Message.Contains(TEXT("a CPU configure is already in flight")));
			TestTrue(TEXT("configure in flight: the first load's CPU configure is still in flight"), S.Cpu->IsConfigurePending());
			const ESuperSLMGpuVendResult Vend = S.Gpu->VendSequence(S.HeldGpu, ESuperSLMGpuDecodePath::Composed);
			if (!TestEqual(TEXT("R5-W1 construction: a GPU sequence vends during the CPU phase"),
					(int32)Vend, (int32)ESuperSLMGpuVendResult::Success))
			{
				return Finish();
			}
			S.bHoldingGpu = true;
			S.Stage = EStage::VendedDuringCpu;
			return false; // wait for this load's pin
		}

		// EStage::VendedDuringCpu
		const FString& Message = S.Listener->Result.Message;
		AddInfo(FString::Printf(TEXT("R5-W1: %s -- %s"), S.Listener->bLoaded ? TEXT("Loaded") : TEXT("Failed"), *Message));
		TestEqual(TEXT("R5-W1: exactly one pin fires"), S.Listener->Calls, 1);
		TestTrue(TEXT("R5-W1: the load finishes Loaded (CPU only)"), S.Listener->bLoaded);
		TestTrue(TEXT("R5-W1: the result reports the CPU configured"), S.Listener->Result.bCpuConfigured);
		TestFalse(TEXT("R5-W1: the result does not report the GPU active with this load's model"), S.Listener->Result.bGpuBackendActive);
		TestTrue(FString::Printf(TEXT("R5-W1: the message names why the GPU was left (\"%s\")"), *Message),
			Message.Contains(TEXT("GPU backend not reconfigured")) &&
			Message.Contains(TEXT("the GPU backend still holds 1 vended sequence(s)")));
		TestTrue(TEXT("R5-W1: the CPU holds this load's model"), S.Cpu->GetConfiguredModel() == S.SecondModel.Get());
		TestFalse(TEXT("R5-W1: the GPU was not reconfigured (no GPU configure pending)"), S.Gpu->IsConfigurePending());
		TestEqual(TEXT("R5-W1: the GPU pool still counts the held sequence (not torn down)"), S.Gpu->GetPoolOccupiedCount(), 1);
		TestTrue(TEXT("R5-W1: the GPU is still active with the model it had"),
			S.Gpu->IsGpuBackendActive() && S.Gpu->GetConfiguredModel() == S.Model.Get());
		return Finish();
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3LoadModelNodeAdapterTest,
	"SuperSLM.L2S3.Blueprint.LoadModelNodeRefusesWhileAGpuAdapterIsMapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3LoadModelNodeAdapterTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMLoadNode;

	// A-AD's 1.5B base, the model the plan's one runtime adapter is built for; one block per pool.
	TSharedRef<FState> State = MakeShared<FState>();
	State->PoolSize = 1;
	if (!SetUp(*this, *State, SuperSLML2S3Fixtures::AAdBaseModelPath(), TEXT("A-AD base")))
	{
		TearDown(*State);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		FState& S = *State;
		auto Finish = [&S]() { TearDown(S); return true; };
		bool bTimedOut = false;
		if (StillWaiting(*this, S, bTimedOut))
		{
			return false;
		}
		if (bTimedOut || !S.Cpu.IsValid() || !S.Gpu.IsValid())
		{
			return Finish();
		}
		if (S.Stage == EStage::CpuAdapterHeld)
		{
			// A CPU adapter handle survives a CPU reconfigure, so it does not refuse the load.
			AddInfo(FString::Printf(TEXT("CPU adapter held: %s -- %s"), S.Listener->bLoaded ? TEXT("Loaded") : TEXT("Failed"), *S.Listener->Result.Message));
			CheckLoadedOnBothBackends(*this, S);
			FString ReleaseError;
			TestTrue(FString::Printf(TEXT("CPU adapter held: the adapter releases after the reload (%s)"), *ReleaseError),
				FSuperSLMAdapterImport::Release(S.CpuAdapter, ReleaseError));
			return Finish();
		}
		if (!CheckLoadedOnBothBackends(*this, S))
		{
			return Finish();
		}

		FSuperSLMGpuAdapterHandle Adapter;
		FString MapError;
		const bool bMapped = S.Gpu->MapAdapter(SuperSLML2S3Fixtures::AAdAdapterPath(), *S.Model.Get(), Adapter, MapError);
		if (!TestTrue(FString::Printf(TEXT("construction: the A-AD adapter maps on the GPU (%s)"), *MapError), bMapped))
		{
			return Finish();
		}
		StartLoad(S);
		CheckRefusedAtActivate(*this, S, TEXT("GPU adapter mapped"),
			{FString(TEXT("the GPU backend still holds 0 vended sequence(s) and 1 mapped adapter(s)"))});
		TestEqual(TEXT("GPU adapter mapped: the mapping survives the refusal"), S.Gpu->GetMappedAdapterCount(), 1);
		S.Gpu->UnmapAdapter(Adapter);
		if (!TestEqual(TEXT("construction: the GPU adapter unmaps"), S.Gpu->GetMappedAdapterCount(), 0))
		{
			return Finish();
		}

		// A CPU adapter handle held across a load: not refused (the CPU's adapters survive its reconfigure).
		FString ImportError;
		const bool bImported = FSuperSLMAdapterImport::ImportFromFile(SuperSLML2S3Fixtures::AAdAdapterPath(), *S.Model.Get(), S.CpuAdapter, ImportError);
		if (!TestTrue(FString::Printf(TEXT("construction: the A-AD adapter imports on the CPU (%s)"), *ImportError), bImported))
		{
			return Finish();
		}
		StartLoad(S);
		S.Stage = EStage::CpuAdapterHeld;
		return false; // wait for this load's pin
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S3
