// Plugin paging plan, GPU red suite (step K5): the GPU subsystem beside the CPU one (§3.4 "Busy",
// "Prefixes"; §7 cells 2.4, 3.3, 8.6). Both subsystems live in one game world's GameInstance; the CPU
// fixture's names are qualified (CpuFx::), the GPU fixture's are not.
//
//   SuperSLM.Paging.Gpu.Composition.CrossBackendPrefix  cell 2.4                         [K7]
//   SuperSLM.Paging.Gpu.Composition.ZeroBusy            cell 3.3                         [K7/K8] (Stress)
//   SuperSLM.Paging.Gpu.Composition.BothPaged           cell 8.6                         [K8] (Stress)
//
// The two Stress cells are run by name on the box. Their plan text adds "every other GPU plugin
// enabled" (3.3) and "every other plugin enabled" (8.6): that is the box's project configuration,
// recorded as a box step in the K5 record, not something a test can set. Every test fails first at a
// pooled or budget Configure() while the modes are unbuilt.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC

#include "Fixtures/SuperSLMPagingFixtures.h"
#include "Fixtures/SuperSLMPagingGpuFixtures.h"

using namespace SuperSLMPagingGpuFixtures;
namespace CpuFx = SuperSLMPagingFixtures;

namespace
{
	// The CPU subsystem of the GPU rig's own world, configured in budget mode with the same model.
	bool ConfigureCpuBeside(FAutomationTestBase& T, FGpuPagingRig& Rig, CpuFx::FPagingRig& CpuRig, const FSuperSLMRuntimeConfig& C)
	{
		CpuRig.Artifact = CpuFx::EPagingArtifact::AEx05B;
		CpuRig.Model = Rig.Model;
		CpuRig.Cpu = SuperSLML2S1Fixtures::GetSubsystem(Rig.World.GetTestWorld());
		return T.TestNotNull(TEXT("the CPU subsystem in the same GameInstance"), CpuRig.Cpu) && CpuFx::ConfigureBudget(T, CpuRig, C);
	}

	// Ticks both subsystems until Done(): the CPU every pass, the GPU when its next tick is not gated
	// on the device.
	bool PumpBoth(USuperSLMSubsystem& Cpu, USuperSLMGpuSubsystem& Gpu, TFunctionRef<bool()> Done, double Seconds)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Done())
		{
			if (FPlatformTime::Seconds() - Start > Seconds)
			{
				return false;
			}
			Cpu.Tick(Step);
			if (!FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				Gpu.Tick(Step);
			}
			FPlatformProcess::Sleep(0.0005f);
		}
		return true;
	}

	bool CpuEnded(const USuperSLMSubsystem& Cpu, const FSuperSLMSequence& Seq)
	{
		const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
		return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Cpu.GetPendingLifecycleOperationCount(Seq) == 0;
	}

	bool GpuEnded(const USuperSLMGpuSubsystem& Gpu, const FSuperSLMGpuSequence& Seq)
	{
		const ESuperSLMSequencePhase Phase = Gpu.GetPhase(Seq);
		return (Phase == ESuperSLMSequencePhase::Complete || Phase == ESuperSLMSequencePhase::Faulted) && Gpu.GetPendingLifecycleOperationCount(Seq) == 0;
	}
}

// Cell 2.4: a CPU prefix passed to the GPU subsystem, and a GPU prefix passed to the CPU one, is
// unknown there (ids come from one shared counter, so a handle of the other backend never names a
// prefix): GetPrefixPhase() reads Invalid, ReleasePrefix() is refused, CreatePrefixFrom() resolves
// ParentNotUsable and an adopt does not succeed. Each prefix stays Ready on its own backend.
// Mutants: a backend that accepts the other's handle (a phase other than Invalid, a release, a child or
// an adopt that succeeds). Not reliably killed: per-backend id counters. A collision needs a GPU id
// equal to the live CPU prefix's, and the CPU counter is process-global and advanced by every earlier
// test, so a GPU counter restarting at 1 rarely meets it here (the K5 coverage audit, N1). The
// different-ids check catches it only when the two counters happen to coincide.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuCrossBackendPrefixTest, "SuperSLM.Paging.Gpu.Composition.CrossBackendPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPagingGpuCrossBackendPrefixTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	CpuFx::FPagingRig CpuRig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 1, 512, 2, 0))
		|| !ConfigureCpuBeside(*this, Rig, CpuRig, CpuFx::MakeConfig(1, 512, 2, 0)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	USuperSLMSubsystem& Cpu = *CpuRig.Cpu;
	TArray<int32> Tokens, ChildTokens;
	FSuperSLMPrefix CpuPrefix, GpuPrefix;
	if (!MakeTokens(*this, *Rig.Model, 64, 1101, Tokens) || !MakeTokens(*this, *Rig.Model, 4, 1102, ChildTokens) || !CpuFx::BuildPrefix(*this, Cpu, Tokens, CpuPrefix)
		|| !BuildPrefix(*this, Gpu, Tokens, GpuPrefix))
	{
		return false;
	}
	bool bOk = TestTrue(TEXT("the two prefixes have different ids"), CpuPrefix.Id != GpuPrefix.Id);

	// A CPU prefix on the GPU.
	{
		bOk &= TestEqual(TEXT("GPU: a CPU prefix's phase is Invalid"), (uint8)Gpu.GetPrefixPhase(CpuPrefix), (uint8)ESuperSLMPrefixPhase::Invalid);
		FString Error;
		bOk &= TestFalse(TEXT("GPU: ReleasePrefix(a CPU prefix) is refused"), Gpu.ReleasePrefix(CpuPrefix, Error));
		FSuperSLMPrefix Child;
		FString FromError;
		bOk &= TestEqual(*FString::Printf(TEXT("GPU: CreatePrefixFrom(a CPU prefix) is ParentNotUsable (%s)"), *FromError), (uint8)Gpu.CreatePrefixFrom(CpuPrefix, ChildTokens, Child, &FromError),
			(uint8)ESuperSLMPrefixResult::ParentNotUsable);
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq))
		{
			return false;
		}
		FString AdoptError;
		const ESuperSLMRestoreResult Result = AdoptResult(Gpu, Seq, CpuPrefix, &AdoptError);
		bOk &= TestTrue(*FString::Printf(TEXT("GPU: adopting a CPU prefix does not succeed (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Result), *AdoptError),
			Result != ESuperSLMRestoreResult::Success);
		ReturnAndDrain(Gpu, Seq);
	}
	// A GPU prefix on the CPU.
	{
		bOk &= TestEqual(TEXT("CPU: a GPU prefix's phase is Invalid"), (uint8)Cpu.GetPrefixPhase(GpuPrefix), (uint8)ESuperSLMPrefixPhase::Invalid);
		FString Error;
		bOk &= TestFalse(TEXT("CPU: ReleasePrefix(a GPU prefix) is refused"), Cpu.ReleasePrefix(GpuPrefix, Error));
		FSuperSLMPrefix Child;
		FString FromError;
		bOk &= TestEqual(*FString::Printf(TEXT("CPU: CreatePrefixFrom(a GPU prefix) is ParentNotUsable (%s)"), *FromError), (uint8)Cpu.CreatePrefixFrom(GpuPrefix, ChildTokens, Child, &FromError),
			(uint8)ESuperSLMPrefixResult::ParentNotUsable);
		FSuperSLMSequence Seq;
		if (!CpuFx::Vend(*this, Cpu, Seq))
		{
			return false;
		}
		FSuperSLMLifecycleOpHandle Handle;
		FString AdoptError;
		ESuperSLMRestoreResult Result = Cpu.AdoptPrefix(Seq, GpuPrefix, Handle, AdoptError);
		if (Result == ESuperSLMRestoreResult::Success)
		{
			Result = CpuFx::WaitOp(Cpu, Handle);
		}
		bOk &= TestTrue(*FString::Printf(TEXT("CPU: adopting a GPU prefix does not succeed (got %s; %s)"), SuperSLML2S2Fixtures::RestoreResultName(Result), *AdoptError),
			Result != ESuperSLMRestoreResult::Success);
		CpuFx::ReturnAndDrain(Cpu, Seq);
	}
	bOk &= TestEqual(TEXT("the CPU prefix is still Ready on the CPU"), (uint8)Cpu.GetPrefixPhase(CpuPrefix), (uint8)ESuperSLMPrefixPhase::Ready);
	bOk &= TestEqual(TEXT("the GPU prefix is still Ready on the GPU"), (uint8)Gpu.GetPrefixPhase(GpuPrefix), (uint8)ESuperSLMPrefixPhase::Ready);
	bOk &= CpuFx::ReleasePrefixAndDrain(*this, Cpu, CpuPrefix);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, GpuPrefix);
	return bOk;
}

// Cell 3.3 (E12): zero SSLM_BUSY under a stress mix. Ten rounds, each running at once: a GPU prefix
// build; a OneCall adopter of a standing prefix generating; a Composed plain sequence generating; a
// GPU restore of a pooled blob; and CPU budget-mode work (a generation) on the same model. The GPU
// subsystem's Layer-1 calls see no SSLM_BUSY (FSuperSLMPagedGpuTestSeam::Layer1BusyCount reads 0).
// "Every other GPU plugin enabled" is the box's configuration (the K5 record's box steps).
// Mutant: a prefix verb called outside the drain (a BUSY from a verb that races the tick's job).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuZeroBusyTest, "SuperSLM.Paging.Gpu.Composition.ZeroBusy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuZeroBusyTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	CpuFx::FPagingRig CpuRig;
	const FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::AEx05B, 4, 512, 2, 1);
	TArray<uint8> Blob;
	if (!Rig.Init(*this) || !MakeBlobAt(*this, Rig, 512, 208, 1, Blob) || !ConfigurePooled(*this, Rig, C)
		|| !ConfigureCpuBeside(*this, Rig, CpuRig, CpuFx::MakeConfig(2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	USuperSLMSubsystem& Cpu = *CpuRig.Cpu;
	TArray<int32> Standing, Prompt;
	FSuperSLMPrefix StandingPrefix;
	if (!MakeTokens(*this, *Rig.Model, 208, 1201, Standing) || !MakeTokens(*this, *Rig.Model, 8, 1202, Prompt) || !BuildPrefix(*this, Gpu, Standing, StandingPrefix))
	{
		return false;
	}
	bool bOk = TestEqual(TEXT("Layer1BusyCount is readable and 0 after Configure"), FSuperSLMPagedGpuTestSeam::Layer1BusyCount(Gpu), int64(0));
	for (int32 Round = 0; Round < 10 && bOk; ++Round)
	{
		TArray<int32> BuildTokens;
		FSuperSLMSequence CpuSeq;
		FSuperSLMGpuSequence Adopter, Plain;
		if (!MakeTokens(*this, *Rig.Model, 96, 1300 + Round, BuildTokens) || !CpuFx::Vend(*this, Cpu, CpuSeq) || !Vend(*this, Gpu, Adopter, ESuperSLMGpuDecodePath::OneCall)
			|| !Vend(*this, Gpu, Plain, ESuperSLMGpuDecodePath::Composed))
		{
			return false;
		}
		FSuperSLMGenerationRequest CpuRequest;
		CpuRequest.PromptTokens = Prompt;
		CpuRequest.MaxNewTokens = 32;
		FString CpuError;
		bOk &= TestTrue(*FString::Printf(TEXT("round %d: CPU BeginGeneration (%s)"), Round, *CpuError), Cpu.BeginGeneration(CpuSeq, CpuRequest, CpuError));
		FSuperSLMPrefix Build;
		FString BuildError;
		bOk &= TestEqual(*FString::Printf(TEXT("round %d: the build is admitted (%s)"), Round, *BuildError), (uint8)Gpu.CreatePrefix(BuildTokens, Build, &BuildError), (uint8)ESuperSLMPrefixResult::Success);
		const FSuperSLMLifecycleOpHandle Adopt = Gpu.RequestAdoptPrefix(Adopter, StandingPrefix);
		FSuperSLMGenerationRequest AdopterRequest;
		AdopterRequest.PromptTokens = { Prompt[0], Prompt[1] };
		AdopterRequest.MaxNewTokens = 24;
		const FSuperSLMLifecycleOpHandle AdopterGen = Gpu.RequestBeginGeneration(Adopter, AdopterRequest);
		FSuperSLMGenerationRequest PlainRequest;
		PlainRequest.PromptTokens = Prompt;
		PlainRequest.MaxNewTokens = 24;
		const FSuperSLMLifecycleOpHandle PlainGen = Gpu.RequestBeginGeneration(Plain, PlainRequest);
		const FIssuedGpuRestore Restore = IssueRestore(Gpu, Blob, Rig.Model);
		const bool bDone = PumpBoth(Cpu, Gpu, [&]()
		{
			FSuperSLMGpuSequence Restored;
			const ESuperSLMPrefixPhase Built = Gpu.GetPrefixPhase(Build);
			return CpuEnded(Cpu, CpuSeq) && GpuEnded(Gpu, Adopter) && GpuEnded(Gpu, Plain) && OutcomeOf(Gpu, Restore, Restored) != ESuperSLMRestoreResult::Pending
				&& (Built == ESuperSLMPrefixPhase::Ready || Built == ESuperSLMPrefixPhase::Faulted || Built == ESuperSLMPrefixPhase::Invalid);
		}, GenerateSeconds);
		bOk &= TestTrue(*FString::Printf(TEXT("round %d: the mix ends"), Round), bDone);
		bOk &= TestEqual(*FString::Printf(TEXT("round %d: the adopt succeeds"), Round), (uint8)Gpu.GetLifecycleOpResult(Adopt), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("round %d: the build is Ready"), Round), (uint8)Gpu.GetPrefixPhase(Build), (uint8)ESuperSLMPrefixPhase::Ready);
		FSuperSLMGpuSequence Restored;
		bOk &= TestEqual(*FString::Printf(TEXT("round %d: the restore succeeds"), Round), (uint8)OutcomeOf(Gpu, Restore, Restored), (uint8)ESuperSLMRestoreResult::Success);
		bOk &= TestEqual(*FString::Printf(TEXT("round %d: no SSLM_BUSY"), Round), FSuperSLMPagedGpuTestSeam::Layer1BusyCount(Gpu), int64(0));
		for (const FSuperSLMLifecycleOpHandle& Handle : { Adopt, AdopterGen, PlainGen })
		{
			Gpu.ReleaseLifecycleOpHandle(Handle);
		}
		Discard(Gpu, Restore);
		Gpu.ReturnSequence(Adopter);
		Gpu.ReturnSequence(Plain);
		CpuFx::ReturnAndDrain(Cpu, CpuSeq);
		DrainQuiet(Gpu);
		FString ReleaseError;
		Gpu.ReleasePrefix(Build, ReleaseError);
		DrainQuiet(Gpu);
	}
	bOk &= TestEqual(TEXT("no SSLM_BUSY across the mix"), FSuperSLMPagedGpuTestSeam::Layer1BusyCount(Gpu), int64(0));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 3.3"));
	bOk &= ReleasePrefixAndDrain(*this, Gpu, StandingPrefix);
	return bOk;
}

// Cell 8.6: the CPU subsystem in budget mode and the GPU subsystem in pooled mode, in one GameInstance,
// each adopting its own 1,000-token prefix and generating at once. Each backend's tokens equal its own
// run alone (taken first), each ledger shows no fault and form 1 holds on each. "Every other plugin
// enabled" is the box's configuration.
// Mutant: shared state between the two backends' ledgers or pools (a charge or a page crossing over).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPagingGpuBothPagedTest, "SuperSLM.Paging.Gpu.Composition.BothPaged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
bool FSuperSLMPagingGpuBothPagedTest::RunTest(const FString& Parameters)
{
	FGpuPagingRig Rig;
	CpuFx::FPagingRig CpuRig;
	if (!Rig.Init(*this) || !ConfigurePooled(*this, Rig, MakeGpuConfig(EGpuArtifact::AEx05B, 2, 512, 1, 1))
		|| !ConfigureCpuBeside(*this, Rig, CpuRig, CpuFx::MakeConfig(2, 512, 1, 1)))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	USuperSLMSubsystem& Cpu = *CpuRig.Cpu;
	TArray<int32> Persona, Suffix;
	FSuperSLMPrefix CpuPrefix, GpuPrefix;
	if (!MakeTokens(*this, *Rig.Model, 1000, 1401, Persona) || !MakeTokens(*this, *Rig.Model, 8, 1402, Suffix) || !CpuFx::BuildPrefix(*this, Cpu, Persona, CpuPrefix)
		|| !BuildPrefix(*this, Gpu, Persona, GpuPrefix))
	{
		return false;
	}
	constexpr int32 N = 32;
	// Alone.
	TArray<int32> CpuAlone, GpuAlone;
	{
		FSuperSLMSequence CpuSeq;
		FSuperSLMGpuSequence GpuSeq;
		if (!CpuFx::Vend(*this, Cpu, CpuSeq) || !CpuFx::AdoptAndWait(*this, Cpu, CpuSeq, CpuPrefix) || !CpuFx::GenerateComplete(*this, Cpu, CpuSeq, Suffix, N, CpuAlone)
			|| !Vend(*this, Gpu, GpuSeq) || !AdoptAndWait(*this, Gpu, GpuSeq, GpuPrefix) || !GenerateComplete(*this, Gpu, GpuSeq, Suffix, N, GpuAlone))
		{
			return false;
		}
		CpuFx::ReturnAndDrain(Cpu, CpuSeq);
		ReturnAndDrain(Gpu, GpuSeq);
	}
	// Together.
	FSuperSLMSequence CpuSeq;
	FSuperSLMGpuSequence GpuSeq;
	if (!CpuFx::Vend(*this, Cpu, CpuSeq) || !CpuFx::AdoptAndWait(*this, Cpu, CpuSeq, CpuPrefix) || !Vend(*this, Gpu, GpuSeq) || !AdoptAndWait(*this, Gpu, GpuSeq, GpuPrefix))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = Suffix;
	Request.MaxNewTokens = N;
	FString CpuError;
	bool bOk = TestTrue(*FString::Printf(TEXT("CPU BeginGeneration (%s)"), *CpuError), Cpu.BeginGeneration(CpuSeq, Request, CpuError));
	const FSuperSLMLifecycleOpHandle GpuGen = Gpu.RequestBeginGeneration(GpuSeq, Request);
	bOk &= TestTrue(TEXT("both generations end"), PumpBoth(Cpu, Gpu, [&]() { return CpuEnded(Cpu, CpuSeq) && GpuEnded(Gpu, GpuSeq); }, GenerateSeconds));
	bOk &= TestEqual(TEXT("the CPU's tokens beside the GPU equal its run alone"), Cpu.GetGeneratedTokens(CpuSeq), CpuAlone);
	bOk &= TestEqual(TEXT("the GPU's tokens beside the CPU equal its run alone"), Gpu.GetGeneratedTokens(GpuSeq), GpuAlone);
	Gpu.ReleaseLifecycleOpHandle(GpuGen);
	bOk &= CpuFx::NoLedgerFault(*this, Cpu, TEXT("cell 8.6, CPU"));
	bOk &= NoLedgerFault(*this, Gpu, TEXT("cell 8.6, GPU"));
	bOk &= CpuFx::Form1ChargesCoverDrawn(*this, Cpu, TEXT("cell 8.6, CPU"));
	bOk &= Form1ChargesCoverDrawn(*this, Gpu, TEXT("cell 8.6, GPU"));
	CpuFx::ReturnAndDrain(Cpu, CpuSeq);
	ReturnAndDrain(Gpu, GpuSeq);
	bOk &= CpuFx::ReleasePrefixAndDrain(*this, Cpu, CpuPrefix);
	bOk &= ReleasePrefixAndDrain(*this, Gpu, GpuPrefix);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
