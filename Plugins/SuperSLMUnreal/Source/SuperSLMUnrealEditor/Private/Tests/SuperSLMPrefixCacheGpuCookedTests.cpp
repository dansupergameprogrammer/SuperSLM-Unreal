// Precache plan rev 1.17, step U7: cooked prefix sets on the GPU, per device (§5.8 per-producer entries, §5.9, PC-5).
// The GPU twins of the [U3] cooked cells (SuperSLMPrefixCacheCookedTests.cpp; the U2 review's set-before-Configure
// cell, SuperSLMPrefixCacheU2ReviewTests.cpp), on the GPU pooled rig. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE
// is on; every leg needs SUPERSLM_WITH_GPU_PREFIX_SAVE (the GPU save and restore) and the GPU, as the U6 GPU cells do.
//
//   SuperSLM.PrefixCache.Gpu.Cooked.StaleSet               cell 2.9, this device's GPU build                [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.RebuiltAfterPin        cell 9.7, this device's GPU build                [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.NoEntryRecomputes      §5.8: another device's build, a CPU-only set, an
//                                                          entry the build left empty                       [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.ProducerRefused        §5.9: a payload whose SPX1 device field names another
//                                                          GPU (digest resealed) is refused and recomputes  [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.SetBeforeFirstConfigure a set registered before the first Configure() is
//                                                          kept and registered by the cache-on Configure()  [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.SurvivesReconfigure    registrations carry across re-Configure(), also after
//                                                          the set itself is collected                      [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.Build                  cell 3.11 for the GPU entries: the action stores CPU
//                                                          and GPU builds together; per-entry failure; cancel;
//                                                          a GPU that is unavailable                        [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.ModelMismatch.BeforeFirstConfigure  the U2 review's SetBeforeFirstConfigure on the
//                                                          GPU: a set naming the 1.5B, kept before the first
//                                                          Configure(), is refused by name by the cache-on one  [U7]
//   SuperSLM.PrefixCache.Gpu.Cooked.ModelMismatch.Reconfigure  cell 2.18 on the GPU (the U2 review's
//                                                          SetUnregisteredByOtherModel): a re-Configure() with the
//                                                          1.5B unregisters the set; back on A-EX it stays gone  [U7]
//
// The mismatch cells' other model is the shipped 1.5B (SUPERSLM_PAGING_1P5B_PATH, the paging [two-cap] artifact),
// which the GPU maps at ContextCap 4,096.
//
// Construction: entries of 16 to 32 tokens; the GPU cache's disk tier off; each GPU build is made through the rig's own
// cache (SuperSLMPrefixSetTools::BeginBuildGpu, PumpBuildGpu, FinishBuildGpu: the commandlet's loop), and every
// runtime leg begins with a cold re-Configure() (the GPU cache host is one per configuration, so nothing is resident).
// "A previous pin", "another model" and "another device" are the build-identity seam's stamps
// (SuperSLMPrefixSetTools::FScopedBuildIdentityOverride), applied after the payloads' own SPX1 headers are checked.
// Expected values are the CPU cells'. The GPU's producer label in a finding line is [GPU 0x<vendor>:0x<device>].
//
// Mutants: the GPU host selecting with Backend 0 -> every GPU leg registers 0 and recomputes (StaleSet, RebuiltAfterPin
// red); selecting by backend alone -> another device's build registers (NoEntryRecomputes red); a stored GPU build not
// checked against its payloads' SPX1 producer -> the AdapterIdMismatch reading is never made (Build's action leg reads
// the header itself); a refused cooked restore kept as a hit -> ProducerRefused red; sources kept on the configuration
// rather than the subsystem -> SurvivesReconfigure red; a build failure clearing the entry's bytes -> Build red.

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SuperSLMPrefixCacheTestSeam.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace SuperSLMPrefixCacheGpuCookedTests
{
	using namespace SuperSLMPagingGpuFixtures;
	using Seam = FSuperSLMPrefixCacheTestSeam;
	using SuperSLMPrefixSetTools::EBuildFinish;

	constexpr int32 kShort = 24; // an entry: 16 to 32 tokens
	const TCHAR* const k1p13Tag = TEXT("v1.13.0");
	const TCHAR* const k1p13CandidateCommit = TEXT("002094a6da8ba33450e6fabc08503b22e35d1610");

	struct FGpuCooked
	{
		FGpuPagingRig Rig;
		FSuperSLMGpuRuntimeConfig Config;
		FSuperSLMGpuPrefixCacheSettings Settings;
		uint32 Vendor = 0; // this adapter's, as the GPU cache keys by it
		uint32 Device = 0;
		USuperSLMGpuSubsystem& Gpu() { return *Rig.Gpu; }
	};

	// A cold re-Configure(): a new GPU cache (one host per configuration), nothing resident, disk off; the registered
	// sets are registered again with it.
	bool Cold(FAutomationTestBase& T, FGpuCooked& C)
	{
		return ConfigurePooled(T, C.Rig, C.Config)
			&& T.TestTrue(TEXT("the GPU cache is configured"), C.Gpu().GetPrefixCacheStats().bConfigured)
			&& T.TestTrue(TEXT("the GPU cache reports its adapter"), Seam::AdapterIdsOf(C.Gpu(), C.Vendor, C.Device));
	}

	// The pooled rig (A-EX, B = 16) with a GPU cache of one region-C block, disk off. PrefixBlocks 1 for a cell that
	// builds a prefill twin with CreatePrefix (region P). bInit false: the rig is already initialised.
	bool ConfigureCooked(FAutomationTestBase& T, FGpuCooked& C, int32 PrefixBlocks = 0, bool bInit = true)
	{
		FGpuPagingRig& Rig = C.Rig;
		C.Config = MakeGpuConfig(Rig.Artifact, 2, 512, PrefixBlocks);
		if ((bInit && !Rig.Init(T)) || !ConfigurePooled(T, Rig, C.Config) || !RequireAExShape(T, Rig))
		{
			return false;
		}
		C.Settings.BudgetBytes = Rig.Geo.PageBytes; // one page's bytes -> one block of region C: room for every entry here
		C.Settings.bDiskEnabled = false;
		C.Settings.DiskMaxBytes = 0;
		Rig.Gpu->ConfigurePrefixCache(C.Settings);
		return Cold(T, C);
	}

	USuperSLMPrefixSet* NewSet(USuperSLMModel* Model)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		return Set;
	}

	FSuperSLMPrefixSetEntry SetEntry(const TCHAR* Name, const TArray<int32>& Tokens, FName Schema = NAME_None)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(Name);
		E.Tokens = Tokens;
		E.SchemaName = Schema;
		return E;
	}

	FString DeviceLabel(uint32 Vendor, uint32 Device)
	{
		return FString::Printf(TEXT("[GPU 0x%04x:0x%04x]"), Vendor & 0xFFFFu, Device & 0xFFFFu);
	}

	FSuperSLMPrefixSetBuild* GpuBuildOf(USuperSLMPrefixSet& Set, uint32 Vendor, uint32 Device)
	{
		return Set.Builds.FindByPredicate([Vendor, Device](const FSuperSLMPrefixSetBuild& B)
		{
			return B.Backend == 1 && static_cast<uint32>(B.AdapterVendorId) == Vendor && static_cast<uint32>(B.AdapterDeviceId) == Device;
		});
	}

	int32 GpuBuildCount(const USuperSLMPrefixSet& Set)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetBuild& B : Set.Builds)
		{
			N += B.Backend == 1 ? 1 : 0;
		}
		return N;
	}

	const FSuperSLMPrefixSetBuild* CpuBuildOf(const USuperSLMPrefixSet& Set)
	{
		return Set.Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& B) { return B.Backend == 0; });
	}

	// Findings by name and entry against a GPU producer (Backend 1), or against the CPU (Backend 0).
	int32 Count(const TArray<FSuperSLMPrefixSetFinding>& Findings, const TCHAR* Finding, FName Entry, uint8 Backend)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			N += F.Finding == FName(Finding) && F.Entry == Entry && F.Backend == Backend ? 1 : 0;
		}
		return N;
	}

	int32 CountBackend(const TArray<FSuperSLMPrefixSetFinding>& Findings, uint8 Backend)
	{
		int32 N = 0;
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			N += F.bHasProducer && F.Backend == Backend ? 1 : 0;
		}
		return N;
	}

	// Pumps a GPU build until its entries are done (PumpBuildGpu ticks the subsystem), pacing on the device as the
	// fixture's Pump does. Stop (optional) ends the loop early when it reads true.
	bool PumpGpuBuild(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, SuperSLMPrefixSetTools::FPrefixSetBuildJob& Job, const TCHAR* What,
		TFunction<bool()> Stop = nullptr)
	{
		const double Start = FPlatformTime::Seconds();
		for (;;)
		{
			if (Stop && Stop())
			{
				return true;
			}
			if (FPlatformTime::Seconds() - Start > BuildSeconds)
			{
				return T.TestTrue(*FString::Printf(TEXT("%s: the GPU build's entries finish in time"), What), false);
			}
			if (FSuperSLMGpuTestAccess::IsNextTickGatedOnDevice(Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			if (!SuperSLMPrefixSetTools::PumpBuildGpu(Job, Gpu, Step))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.0005f);
		}
	}

	// The set's GPU entries, built through the rig's own cache: the commandlet's loop. Expect: the finish's result.
	bool BuildGpu(FAutomationTestBase& T, FGpuCooked& C, USuperSLMPrefixSet& Set, const TCHAR* What,
		TArray<FSuperSLMPrefixSetFinding>* OutFindings = nullptr, EBuildFinish Expect = EBuildFinish::Stored)
	{
		FString Error;
		TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> Job = SuperSLMPrefixSetTools::BeginBuildGpu(Set, C.Gpu(), Error);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the GPU build begins (%s)"), What, *Error), Job.IsValid()) || !PumpGpuBuild(T, C.Gpu(), *Job, What))
		{
			return false;
		}
		TArray<FSuperSLMPrefixSetFinding> Findings;
		const EBuildFinish Finished = SuperSLMPrefixSetTools::FinishBuildGpu(Set, *Job, Findings, Error);
		if (OutFindings != nullptr)
		{
			*OutFindings = Findings;
		}
		return T.TestEqual(*FString::Printf(TEXT("%s: the GPU build finishes as expected (%s)"), What, *Error), (uint8)Finished, (uint8)Expect);
	}

	// Requests S and expects it Ready from Source; returns the lease (returned by the caller).
	bool ServedFrom(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefixSpec& S, ESuperSLMPrefixSource Source, FSuperSLMPrefix& Out,
		const TCHAR* What)
	{
		FString Error;
		if (!T.TestTrue(*FString::Printf(TEXT("%s: requested (%s)"), What, *Error), Gpu.RequestPrefix(S, Out, Error)))
		{
			return false;
		}
		Pump(Gpu, [&Gpu, &Out]() { return Gpu.GetPrefixLeaseState(Out) != ESuperSLMPrefixLeaseState::Pending; }, BuildSeconds);
		return T.TestEqual(*FString::Printf(TEXT("%s: Ready"), What), (uint8)Gpu.GetPrefixLeaseState(Out), (uint8)ESuperSLMPrefixLeaseState::Ready)
			&& T.TestEqual(*FString::Printf(TEXT("%s: served from the expected tier"), What), (uint8)Gpu.GetPrefixSource(Out), (uint8)Source);
	}

	bool ServedAndReturned(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, ESuperSLMPrefixSource Source, const TCHAR* What)
	{
		FSuperSLMPrefix L;
		const bool bOk = ServedFrom(T, Gpu, SuperSLMPrefixCacheFixtures::Spec(Tokens), Source, L, What);
		Gpu.ReturnPrefix(L);
		return bOk && DrainQuiet(Gpu);
	}

	// The prefill twin (the GPU nested cells' construction): Tokens built by CreatePrefix (region P, outside the cache),
	// adopted by a fresh sequence and generated with Suffix; a cache lease's adopter must produce its tokens.
	bool GpuAdopterTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const FSuperSLMPrefix& Leased, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMGpuSequence Seq;
		if (!Vend(T, Gpu, Seq) || !AdoptAndWait(T, Gpu, Seq, Leased))
		{
			return false;
		}
		const bool bOk = GenerateComplete(T, Gpu, Seq, Suffix, SuperSLMPrefixCacheFixtures::TwinNewTokens, OutTokens);
		ReturnAndDrain(Gpu, Seq);
		return bOk;
	}

	bool GpuTwinTokens(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, const TArray<int32>& Tokens, const TArray<int32>& Suffix, TArray<int32>& OutTokens)
	{
		FSuperSLMPrefix Twin;
		if (!BuildPrefix(T, Gpu, Tokens, Twin))
		{
			return false;
		}
		const bool bOk = GpuAdopterTokens(T, Gpu, Twin, Suffix, OutTokens);
		return ReleasePrefixAndDrain(T, Gpu, Twin) && bOk;
	}

	// One entry built alone into a scratch set (under whatever stamp is alive) and its GPU wrapper.
	bool BuiltAlone(FAutomationTestBase& T, FGpuCooked& C, const FSuperSLMPrefixSetEntry& E, const TCHAR* What, TArray<uint8>& OutWrapper)
	{
		USuperSLMPrefixSet* Scratch = NewSet(C.Rig.Model);
		Scratch->Entries = {E};
		if (!BuildGpu(T, C, *Scratch, What) || Scratch->Builds.Num() != 1 || Scratch->Builds[0].Entries.Num() != 1)
		{
			return T.TestTrue(*FString::Printf(TEXT("%s: one GPU build of one entry"), What), false);
		}
		OutWrapper = Scratch->Builds[0].Entries[0].Wrapper;
		return T.TestTrue(*FString::Printf(TEXT("%s: the entry has bytes"), What), OutWrapper.Num() > 0);
	}

	// Pumps a Build Prefixes action (its GPU half moves on Pump) until it finishes.
	bool PumpAction(SuperSLMPrefixSetEditor::FSuperSLMPrefixSetBuildAction& Action)
	{
		const double Start = FPlatformTime::Seconds();
		while (!Action.IsFinished())
		{
			if (FPlatformTime::Seconds() - Start > BuildSeconds)
			{
				return false;
			}
			Action.Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}
}

using namespace SuperSLMPrefixCacheGpuCookedTests;

// Cell 2.9 on the GPU: this device's build with a stale tag, a stale artifact hash, and a spec edited after the build.
// The cook names exactly those three against [GPU v:d] (and, the set having no CPU build, every entry NotBuilt for the
// CPU); at runtime each is served by recompute, and the fresh entry is a cooked hit whose adopter equals the twin, with
// no prefix prefill.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedStaleSetTest, "SuperSLM.PrefixCache.Gpu.Cooked.StaleSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedStaleSetTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	if (!ConfigureCooked(*this, C, /*PrefixBlocks*/ 1))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	TArray<int32> Fresh, OldTag, OldModel, Edited, Suffix, Twin;
	if (!MakeTokens(*this, *C.Rig.Model, kShort, 1, Fresh) || !MakeTokens(*this, *C.Rig.Model, kShort, 2, OldTag)
		|| !MakeTokens(*this, *C.Rig.Model, kShort, 3, OldModel) || !MakeTokens(*this, *C.Rig.Model, kShort, 4, Edited)
		|| !MakeTokens(*this, *C.Rig.Model, SuperSLMPrefixCacheFixtures::SuffixTokens, 5, Suffix) || !GpuTwinTokens(*this, Gpu, Fresh, Suffix, Twin))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewSet(C.Rig.Model);
	Set->Entries = {SetEntry(TEXT("fresh"), Fresh), SetEntry(TEXT("old_tag"), OldTag), SetEntry(TEXT("old_model"), OldModel), SetEntry(TEXT("edited"), Edited)};
	if (!BuildGpu(*this, C, *Set, TEXT("this pin")) || !TestEqual(TEXT("one build: this device's"), Set->Builds.Num(), 1))
	{
		return false;
	}
	FSuperSLMPrefixSetBuild* Build = GpuBuildOf(*Set, C.Vendor, C.Device);
	if (!TestNotNull(TEXT("the build is this device's (backend 1, its vendor and device)"), Build) || !TestEqual(TEXT("four entries"), Build->Entries.Num(), 4))
	{
		return false;
	}
	// old_tag's bytes as a build before a re-pin left them; old_model's as before a reimport.
	bool bOk = true;
	{
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OldPin(k1p13Tag, k1p13CandidateCommit, false);
		bOk &= BuiltAlone(*this, C, Set->Entries[1], TEXT("old_tag under v1.13.0"), Build->Entries[1].Wrapper);
	}
	{
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OtherModel(FString(), FString(), true);
		bOk &= BuiltAlone(*this, C, Set->Entries[2], TEXT("old_model against a flipped artifact hash"), Build->Entries[2].Wrapper);
	}
	Set->Entries[3].Tokens[11] = Set->Entries[3].Tokens[11] == 0 ? 1 : 0; // edited after the build
	if (!bOk)
	{
		return false;
	}

	// The cook leg: three GPU findings, each naming its entry, why, and this device.
	{
		SuperSLMPagingFixtures::FPagingLogCapture Log;
		const TArray<FSuperSLMPrefixSetFinding> Findings = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
		bOk &= TestEqual(TEXT("cook: three findings against this GPU"), CountBackend(Findings, 1), 3);
		bOk &= TestEqual(TEXT("cook: old_tag is StaleLayer1Tag [GPU]"), Count(Findings, TEXT("StaleLayer1Tag"), FName(TEXT("old_tag")), 1), 1);
		bOk &= TestEqual(TEXT("cook: old_model is StaleArtifact [GPU]"), Count(Findings, TEXT("StaleArtifact"), FName(TEXT("old_model")), 1), 1);
		bOk &= TestEqual(TEXT("cook: edited is SpecEdited [GPU]"), Count(Findings, TEXT("SpecEdited"), FName(TEXT("edited")), 1), 1);
		bOk &= TestEqual(TEXT("cook: with no CPU build, every entry is NotBuilt for the CPU"), CountBackend(Findings, 0), 4);
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			if (F.Backend == 1)
			{
				bOk &= TestEqual(*FString::Printf(TEXT("cook: the finding names this device (%s)"), *F.Line), F.AdapterVendorId, C.Vendor);
				bOk &= TestEqual(*FString::Printf(TEXT("cook: the finding names this device's id (%s)"), *F.Line), F.AdapterDeviceId, C.Device);
			}
		}
		const FString Label = DeviceLabel(C.Vendor, C.Device);
		bOk &= TestEqual(TEXT("cook: nothing names fresh against the GPU"), Log.CountContaining(*FString::Printf(TEXT("entry 'fresh' %s"), *Label)), 0);
		for (const TCHAR* Name : {TEXT("old_tag"), TEXT("old_model"), TEXT("edited")})
		{
			bOk &= TestEqual(*FString::Printf(TEXT("cook: one warning line names '%s', the set and this device"), Name),
				Log.CountContaining(*FString::Printf(TEXT("%s: entry '%s' %s"), *Set->GetPathName(), Name, *Label)), 1);
		}
	}

	// The runtime leg, cold: the stale bytes ship, and every stale entry recomputes.
	if (!Cold(*this, C))
	{
		return false;
	}
	Gpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("all four entries register (staleness is the runtime's to find)"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 4);
	FSuperSLMPrefix L;
	const int64 BuildsBefore = Gpu.GetPrefixCacheStats().BuildsIssued;
	bOk &= ServedFrom(*this, Gpu, SuperSLMPrefixCacheFixtures::Spec(Fresh), ESuperSLMPrefixSource::Cooked, L, TEXT("fresh"));
	bOk &= TestEqual(TEXT("fresh: no prefix prefill ran"), Gpu.GetPrefixCacheStats().BuildsIssued, BuildsBefore);
	{
		TArray<int32> Adopted;
		bOk &= GpuAdopterTokens(*this, Gpu, L, Suffix, Adopted) && TestEqual(TEXT("fresh: the cooked hit's adopter equals the prefill twin"), Adopted, Twin);
	}
	Gpu.ReturnPrefix(L);
	bOk &= DrainQuiet(Gpu);
	for (const FSuperSLMPrefixSetEntry& E : {Set->Entries[1], Set->Entries[2], Set->Entries[3]})
	{
		bOk &= ServedAndReturned(*this, Gpu, E.Tokens, ESuperSLMPrefixSource::Recomputed, *E.Name.ToString());
	}
	bOk &= TestEqual(TEXT("one cooked hit, three recomputes"), SuperSLMPrefixCacheFixtures::Served(Gpu.GetPrefixCacheStats(), ESuperSLMPrefixSource::Cooked), int64(1));
	Gpu.UnregisterPrefixSet(Set);
	return bOk;
}

// Cell 9.7 on the GPU: this device's build made under the previous pin is all recompute and warns at cook; rebuilt
// under this pin it replaces that build (one GPU build), validates clean for the GPU, and serves cooked hits again with
// no prefix prefill.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedRebuiltTest, "SuperSLM.PrefixCache.Gpu.Cooked.RebuiltAfterPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedRebuiltTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	TArray<int32> A, B;
	if (!ConfigureCooked(*this, C) || !MakeTokens(*this, *C.Rig.Model, kShort, 1, A) || !MakeTokens(*this, *C.Rig.Model, 16, 2, B))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	USuperSLMPrefixSet* Set = NewSet(C.Rig.Model);
	Set->Entries = {SetEntry(TEXT("a"), A), SetEntry(TEXT("b"), B)};
	{
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OldPin(k1p13Tag, k1p13CandidateCommit, false);
		if (!BuildGpu(*this, C, *Set, TEXT("previous pin")))
		{
			return false;
		}
	}
	const TArray<FSuperSLMPrefixSetFinding> Before = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
	bool bOk = TestEqual(TEXT("before: a is StaleLayer1Tag [GPU]"), Count(Before, TEXT("StaleLayer1Tag"), FName(TEXT("a")), 1), 1);
	bOk &= TestEqual(TEXT("before: b is StaleLayer1Tag [GPU]"), Count(Before, TEXT("StaleLayer1Tag"), FName(TEXT("b")), 1), 1);
	if (!Cold(*this, C))
	{
		return false;
	}
	Gpu.RegisterPrefixSet(Set);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("before the rebuild: a"));
	bOk &= ServedAndReturned(*this, Gpu, B, ESuperSLMPrefixSource::Recomputed, TEXT("before the rebuild: b"));
	Gpu.UnregisterPrefixSet(Set); // the GPU build needs no cooked set registered

	// The rebuild, cold, replaces this device's build; the set is registered again on a cold cache.
	if (!Cold(*this, C) || !BuildGpu(*this, C, *Set, TEXT("this pin")) || !TestEqual(TEXT("the rebuild replaced the GPU build"), GpuBuildCount(*Set), 1)
		|| !TestNotNull(TEXT("the build is this device's"), GpuBuildOf(*Set, C.Vendor, C.Device)) || !Cold(*this, C))
	{
		return false;
	}
	const TArray<FSuperSLMPrefixSetFinding> After = SuperSLMPrefixSetEditor::ValidateAtCook(*Set);
	bOk &= TestEqual(TEXT("after: the set validates clean for the GPU"), CountBackend(After, 1), 0);
	bOk &= TestEqual(TEXT("after: only the CPU's NotBuilt remain (no CPU build)"), After.Num(), 2);
	Gpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("after: both entries register"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 2);
	const int64 BuildsBefore = Gpu.GetPrefixCacheStats().BuildsIssued;
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("after: a"));
	bOk &= ServedAndReturned(*this, Gpu, B, ESuperSLMPrefixSource::Cooked, TEXT("after: b"));
	bOk &= TestEqual(TEXT("after: no prefix prefill ran"), Gpu.GetPrefixCacheStats().BuildsIssued, BuildsBefore);
	Gpu.UnregisterPrefixSet(Set);
	return bOk;
}

// §5.8's per-producer rule for GPU devices (the CPU twin is Cpu.Cooked.NoEntryRecomputes): a set whose only GPU build is
// another device's, and a set with a CPU build only, register nothing on this GPU (one Log line naming the set and this
// device) and recompute; an entry this device's build left empty recomputes while the other entry is a cooked hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedNoEntryTest, "SuperSLM.PrefixCache.Gpu.Cooked.NoEntryRecomputes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedNoEntryTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	TArray<int32> A, B;
	if (!ConfigureCooked(*this, C) || !MakeTokens(*this, *C.Rig.Model, kShort, 1, A) || !MakeTokens(*this, *C.Rig.Model, 32, 2, B))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	USuperSLMPrefixSet* Set = NewSet(C.Rig.Model);
	Set->Entries = {SetEntry(TEXT("a"), A), SetEntry(TEXT("b"), B)};
	if (!BuildGpu(*this, C, *Set, TEXT("this device")) || !TestEqual(TEXT("one build"), Set->Builds.Num(), 1))
	{
		return false;
	}
	const FString Label = DeviceLabel(C.Vendor, C.Device);

	// Another device's build: the same entries stamped as the same vendor's other GPU.
	USuperSLMPrefixSet* Other = NewSet(C.Rig.Model);
	Other->Entries = Set->Entries;
	{
		SuperSLMPrefixSetTools::FScopedBuildIdentityOverride OtherDevice(FString(), FString(), false, C.Vendor, C.Device ^ 1u);
		if (!BuildGpu(*this, C, *Other, TEXT("another device")))
		{
			return false;
		}
	}
	bool bOk = TestNotNull(TEXT("the other set's build is the other device's"), GpuBuildOf(*Other, C.Vendor, C.Device ^ 1u));
	bOk &= TestNull(TEXT("the other set has no build for this device"), GpuBuildOf(*Other, C.Vendor, C.Device));
	if (!Cold(*this, C))
	{
		return false;
	}
	{
		SuperSLMPagingFixtures::FPagingLogCapture Log;
		Gpu.RegisterPrefixSet(Other);
		bOk &= TestEqual(TEXT("another device's build registers nothing"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
		bOk &= TestEqual(TEXT("one Log line names the set and this device"),
			Log.CountLinesContainingAll({*Other->GetPathName(), TEXT("has no build for this producer"), *Label}), 1);
	}
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("another device's build"));
	Gpu.UnregisterPrefixSet(Other);

	// A CPU-only set: no GPU build at all.
	USuperSLMPrefixSet* CpuOnly = NewSet(C.Rig.Model);
	CpuOnly->Entries = Set->Entries;
	{
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		bOk &= TestTrue(*FString::Printf(TEXT("the CPU-only set builds (%s)"), *Error), SuperSLMPrefixSetTools::BuildCpu(*CpuOnly, Findings, Error));
	}
	bOk &= TestEqual(TEXT("the CPU-only set has no GPU build"), GpuBuildCount(*CpuOnly), 0);
	if (!Cold(*this, C))
	{
		return false;
	}
	Gpu.RegisterPrefixSet(CpuOnly);
	bOk &= TestEqual(TEXT("a set with no GPU build registers nothing"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("CPU only"));
	Gpu.UnregisterPrefixSet(CpuOnly);

	// An entry this device's build left empty: the other registers and hits; this one recomputes.
	if (!Cold(*this, C))
	{
		return false;
	}
	FSuperSLMPrefixSetBuild* Mine = GpuBuildOf(*Set, C.Vendor, C.Device);
	if (!TestNotNull(TEXT("this device's build"), Mine) || !TestEqual(TEXT("two entries"), Mine->Entries.Num(), 2))
	{
		return false;
	}
	Mine->Entries[1].Wrapper.Reset();
	Gpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("one entry registers"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("a: built"));
	bOk &= ServedAndReturned(*this, Gpu, B, ESuperSLMPrefixSource::Recomputed, TEXT("b: left empty"));
	Gpu.UnregisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("cook: b NotBuilt for this GPU"), Count(SuperSLMPrefixSetEditor::ValidateAtCook(*Set), TEXT("NotBuilt"), FName(TEXT("b")), 1), 1);
	return bOk;
}

// §5.9's GPU row for a cooked entry: this device's build, its payload's SPX1 adapter device (offset 24) changed and the
// digest resealed, as a blob from the same vendor's other GPU would read. The wrapper's key is unchanged, so it
// registers; the restore is refused (SSLM_RESTORE_PRODUCER_MISMATCH): PayloadRefused once, the request is Recomputed,
// and the blob is marked stale. The unedited payload's header names this device (the build's own check).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedProducerRefusedTest, "SuperSLM.PrefixCache.Gpu.Cooked.ProducerRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedProducerRefusedTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	TArray<int32> A;
	if (!ConfigureCooked(*this, C) || !MakeTokens(*this, *C.Rig.Model, kShort, 1, A))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	USuperSLMPrefixSet* Set = NewSet(C.Rig.Model);
	Set->Entries = {SetEntry(TEXT("a"), A)};
	if (!BuildGpu(*this, C, *Set, TEXT("this device")))
	{
		return false;
	}
	FSuperSLMPrefixSetBuild* Mine = GpuBuildOf(*Set, C.Vendor, C.Device);
	if (!TestNotNull(TEXT("this device's build"), Mine) || !TestEqual(TEXT("one entry"), Mine->Entries.Num(), 1))
	{
		return false;
	}
	TArray<uint8>& Wrapper = Mine->Entries[0].Wrapper;
	uint32 Backend = 0, Vendor = 0, Device = 0;
	bool bOk = TestTrue(TEXT("the payload is an SPX1 blob"), Seam::ReadWrapperPayloadProducer(Wrapper, Backend, Vendor, Device));
	bOk &= TestEqual(TEXT("the payload's backend is the GPU's"), Backend, 1u);
	bOk &= TestEqual(TEXT("the payload's vendor is this adapter's"), Vendor, C.Vendor);
	bOk &= TestEqual(TEXT("the payload's device is this adapter's"), Device, C.Device);
	bOk &= TestTrue(TEXT("the payload's device field is rewritten and the digest resealed"), Seam::EditWrapperBytes(Wrapper, ESuperSLMDiskWrapperEdit::PayloadDevice));
	bOk &= TestTrue(TEXT("the edited payload reads"), Seam::ReadWrapperPayloadProducer(Wrapper, Backend, Vendor, Device));
	bOk &= TestEqual(TEXT("the edited payload names another device"), Device, C.Device ^ 1u);
	if (!bOk || !Cold(*this, C))
	{
		return false;
	}
	Gpu.RegisterPrefixSet(Set);
	bOk &= TestEqual(TEXT("the edited entry registers (its key is unchanged)"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	const FSuperSLMPrefixCacheStats S0 = Gpu.GetPrefixCacheStats();
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("another device's payload"));
	const FSuperSLMPrefixCacheStats S1 = Gpu.GetPrefixCacheStats();
	bOk &= TestEqual(TEXT("one cooked restore was tried"), S1.RestoresIssued - S0.RestoresIssued, int64(1));
	bOk &= TestEqual(TEXT("PayloadRefused once"), SuperSLMPrefixCacheFixtures::FallbackCount(S1, TEXT("PayloadRefused")) - SuperSLMPrefixCacheFixtures::FallbackCount(S0, TEXT("PayloadRefused")), int64(1));
	bOk &= TestEqual(TEXT("no cooked hit"), SuperSLMPrefixCacheFixtures::Served(S1, ESuperSLMPrefixSource::Cooked), int64(0));
	bOk &= TestEqual(TEXT("the refused blob is marked stale"), Seam::StaleCookedEntries(Gpu), 1);
	Gpu.UnregisterPrefixSet(Set);
	return bOk;
}

// A set registered with a GPU subsystem before its first Configure() is kept (nothing to register it with: the GPU cache
// host is made by a Configure()), stays unregistered through a Configure() without a cache, and is registered, its model
// checked, by the first cache-on Configure(): a cooked hit, with no PrefixSetModelMismatch. The set is built on another
// GPU subsystem (torn down first) against another import of the same artifact: the model is matched by artifact hash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedSetBeforeConfigureTest, "SuperSLM.PrefixCache.Gpu.Cooked.SetBeforeFirstConfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedSetBeforeConfigureTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<USuperSLMPrefixSet> Set;
	TArray<int32> A;
	{
		FGpuCooked Builder;
		if (!ConfigureCooked(*this, Builder) || !MakeTokens(*this, *Builder.Rig.Model, kShort, 1, A))
		{
			return false;
		}
		Set.Reset(NewSet(Builder.Rig.Model));
		Set->Entries = {SetEntry(TEXT("a"), A)};
		if (!BuildGpu(*this, Builder, *Set, TEXT("the build rig")))
		{
			return false;
		}
	} // the build rig's GPU subsystem is torn down here

	FGpuCooked C;
	if (!C.Rig.Init(*this))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	bool bOk = true;
	{
		SuperSLMPagingFixtures::FPagingLogCapture Log;
		Gpu.RegisterPrefixSet(Set.Get()); // before the first Configure(): kept
		bOk &= TestEqual(TEXT("before Configure(): one line says the set is kept"), Log.CountLinesContainingAll({TEXT("kept"), *Set->GetPathName()}), 1);
	}
	bOk &= TestFalse(TEXT("before Configure(): no GPU cache"), Gpu.GetPrefixCacheStats().bConfigured);
	// A Configure() without a cache, then the cache-on one (ConfigureCooked's order).
	if (!ConfigureCooked(*this, C, /*PrefixBlocks*/ 0, /*bInit*/ false))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("the cache-on Configure() registers the kept set"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	bOk &= TestEqual(TEXT("no PrefixSetModelMismatch (the same artifact)"), SuperSLMPrefixCacheFixtures::FallbackCount(Gpu.GetPrefixCacheStats(), TEXT("PrefixSetModelMismatch")), int64(0));
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("the kept set's entry"));
	Gpu.UnregisterPrefixSet(Set.Get());
	return bOk;
}

// The GPU cache host is one per configuration; the subsystem keeps each registered set's copy and registers it with
// every configuration's cache. A set registered once serves cooked hits after a re-Configure(), and still after the set
// object itself is collected (the copy holds its entries and bytes); UnregisterPrefixSet ends it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedSurvivesReconfigureTest, "SuperSLM.PrefixCache.Gpu.Cooked.SurvivesReconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedSurvivesReconfigureTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	TArray<int32> A, B;
	if (!ConfigureCooked(*this, C) || !MakeTokens(*this, *C.Rig.Model, kShort, 1, A) || !MakeTokens(*this, *C.Rig.Model, kShort, 2, B))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	// Kept {a} and gone {b}: b's set is unregistered at the end; a's is collected while registered.
	USuperSLMPrefixSet* Kept = NewSet(C.Rig.Model);
	Kept->Entries = {SetEntry(TEXT("b"), B)};
	USuperSLMPrefixSet* Collected = NewSet(C.Rig.Model);
	Collected->Entries = {SetEntry(TEXT("a"), A)};
	if (!BuildGpu(*this, C, *Kept, TEXT("the kept set")) || !BuildGpu(*this, C, *Collected, TEXT("the collected set")) || !Cold(*this, C))
	{
		return false;
	}
	TStrongObjectPtr<USuperSLMPrefixSet> KeepKept(Kept);
	Gpu.RegisterPrefixSet(Kept);
	Gpu.RegisterPrefixSet(Collected);
	bool bOk = TestEqual(TEXT("both sets register"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 2);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("a, first configuration"));

	// A re-Configure(): a new cache host, registered again from the copies.
	if (!Cold(*this, C))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("after a re-Configure(): both sets registered again"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 2);
	bOk &= ServedAndReturned(*this, Gpu, B, ESuperSLMPrefixSource::Cooked, TEXT("b, second configuration"));

	// The registered set is collected; its copy carries on.
	const FString CollectedPath = Collected->GetPathName();
	TWeakObjectPtr<USuperSLMPrefixSet> WeakCollected(Collected);
	Collected->MarkAsGarbage();
	Collected = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	bOk &= TestFalse(*FString::Printf(TEXT("precondition: %s is collected"), *CollectedPath), WeakCollected.IsValid());
	if (!Cold(*this, C))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("after the set is collected and a re-Configure(): its copy is registered"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 2);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("a, after its set was collected"));

	// Unregistered: gone from this configuration and from the next.
	Gpu.UnregisterPrefixSet(Kept);
	bOk &= TestEqual(TEXT("unregistered: one set left"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	if (!Cold(*this, C))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("after a re-Configure(): the unregistered set stays gone"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	bOk &= ServedAndReturned(*this, Gpu, B, ESuperSLMPrefixSource::Recomputed, TEXT("b, unregistered"));
	return bOk;
}

// Cell 3.11 for the GPU entries.
//   1. The Build Prefixes action (bBuildGpu, the menu's) on an editor with no GPU subsystem configured: Start() returns
//      before any prefill on the game thread, and the action stores the CPU build and this GPU's build together; the
//      GPU build's every payload names the device it is stored under (its SPX1 header); the game-thread counter reads 0.
//   2. Through the rig's cache: one entry's cache build fails (FSuperSLMPrefixCacheTestSeam::SetNextCacheBuildStatus):
//      it is reported BuildFailed [GPU v:d] and keeps its previous bytes; the others are the new build's, stored at the
//      end; the CPU build is untouched.
//   3. A cancel after one entry (the job's test pause): while paused the GPU bytes are the previous build's; after the
//      cancel they are byte-equal to it.
//   4. A GPU that is unavailable (SuperSLMPrefixSetEditor::SetNextGpuLegUnavailable): the action stores the CPU build,
//      keeps the GPU build as it was, and reports GpuNotBuilt [GPU] with the reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedBuildTest, "SuperSLM.PrefixCache.Gpu.Cooked.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedBuildTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPrefixSetEditor;
	constexpr int32 kEntries = 3;
	FGpuCooked C;
	if (!C.Rig.Init(*this))
	{
		return false;
	}
	USuperSLMPrefixSet* Set = NewSet(C.Rig.Model);
	TStrongObjectPtr<USuperSLMPrefixSet> KeepSet(Set);
	for (int32 I = 0; I < kEntries; ++I)
	{
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(*FString::Printf(TEXT("entry%d"), I));
		if (!MakeTokens(*this, *C.Rig.Model, 16 + 8 * I, 1 + I, E.Tokens))
		{
			return false;
		}
		Set->Entries.Add(MoveTemp(E));
	}
	auto EditEntries = [this, &C, Set](int32 SaltBase) -> bool
	{
		bool bEdited = true;
		for (int32 I = 0; I < kEntries; ++I)
		{
			TArray<int32> Tokens;
			bEdited &= MakeTokens(*this, *C.Rig.Model, 16 + 8 * I, SaltBase + I, Tokens);
			Set->Entries[I].Tokens = Tokens;
		}
		return bEdited;
	};
	auto GpuBytes = [Set]() -> TArray<TArray<uint8>>
	{
		TArray<TArray<uint8>> Out;
		const FSuperSLMPrefixSetBuild* B = Set->Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& X) { return X.Backend == 1; });
		for (const FSuperSLMPrefixSetEntry& E : Set->Entries)
		{
			const FSuperSLMPrefixSetBuiltEntry* Built = B != nullptr ? B->Entries.FindByPredicate([&E](const FSuperSLMPrefixSetBuiltEntry& X) { return X.Name == E.Name; }) : nullptr;
			Out.Add(Built != nullptr ? Built->Wrapper : TArray<uint8>());
		}
		return Out;
	};

	// 1. The action, CPU and GPU, on an editor with no GPU subsystem configured (this rig's is not, yet).
	bool bOk = TestFalse(TEXT("precondition: no GPU subsystem is configured"), SuperSLMPrefixSetTools::AnyGpuSubsystemConfigured());
	SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
	{
		FString Error;
		TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error, /*TestPauseAfterEntries*/ -1, /*bWithGpu*/ true);
		if (!TestTrue(*FString::Printf(TEXT("action: starts (%s)"), *Error), Action.IsValid()))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("action: Start() returned before any prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
		bOk &= TestTrue(*FString::Printf(TEXT("action: the GPU half started (%s)"), *Action->GetGpuNotBuiltReason()), Action->GetGpuNotBuiltReason().IsEmpty());
		bOk &= TestTrue(TEXT("action: finishes"), PumpAction(*Action));
		bOk &= TestEqual(TEXT("action: Stored"), (uint8)Action->GetResult(), (uint8)EBuildFinish::Stored);
		bOk &= TestTrue(TEXT("action: the GPU build was stored with the CPU build"), Action->WasGpuStored());
		bOk &= TestEqual(TEXT("action: no GpuNotBuilt"), Count(Action->GetFindings(), TEXT("GpuNotBuilt"), NAME_None, 1), 0);
		bOk &= TestEqual(TEXT("action: no prefill, freeze or save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
	}
	bOk &= TestEqual(TEXT("action: one GPU build"), GpuBuildCount(*Set), 1);
	const FSuperSLMPrefixSetBuild* ActionGpu = Set->Builds.FindByPredicate([](const FSuperSLMPrefixSetBuild& X) { return X.Backend == 1; });
	if (!TestNotNull(TEXT("action: a CPU build"), CpuBuildOf(*Set)) || !TestNotNull(TEXT("action: the GPU build"), ActionGpu))
	{
		return false;
	}
	for (const FSuperSLMPrefixSetBuiltEntry& E : ActionGpu->Entries)
	{
		uint32 Backend = 0, Vendor = 0, Device = 0;
		bOk &= TestTrue(*FString::Printf(TEXT("action: %s's GPU payload is SPX1"), *E.Name.ToString()), Seam::ReadWrapperPayloadProducer(E.Wrapper, Backend, Vendor, Device));
		bOk &= TestEqual(*FString::Printf(TEXT("action: %s's payload backend is the GPU's"), *E.Name.ToString()), Backend, 1u);
		bOk &= TestEqual(*FString::Printf(TEXT("action: %s's payload vendor is the build's"), *E.Name.ToString()), Vendor, static_cast<uint32>(ActionGpu->AdapterVendorId));
		bOk &= TestEqual(*FString::Printf(TEXT("action: %s's payload device is the build's"), *E.Name.ToString()), Device, static_cast<uint32>(ActionGpu->AdapterDeviceId));
	}
	bOk &= TestFalse(TEXT("action: its transient GPU subsystem is torn down"), SuperSLMPrefixSetTools::AnyGpuSubsystemConfigured());

	// 2. Per-entry failure, through the rig's own cache: the previous bytes P are the action's GPU build (the same device).
	if (!ConfigureCooked(*this, C, /*PrefixBlocks*/ 0, /*bInit*/ false))
	{
		return false;
	}
	bOk &= TestNotNull(TEXT("the action's GPU build is this rig's device's"), GpuBuildOf(*Set, C.Vendor, C.Device));
	const TArray<TArray<uint8>> P = GpuBytes();
	const FSuperSLMPrefixSetBuild CpuBefore = *CpuBuildOf(*Set);
	bOk &= EditEntries(20);
	SuperSLMPrefixSetTools::ResetGameThreadBuildCalls();
	{
		FString Error;
		TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> Job = SuperSLMPrefixSetTools::BeginBuildGpu(*Set, C.Gpu(), Error);
		if (!TestTrue(*FString::Printf(TEXT("failure leg: the GPU build begins (%s)"), *Error), Job.IsValid()))
		{
			return false;
		}
		// The first entry's cache build: its begin is not called, and the build fails.
		Seam::SetNextCacheBuildStatus(C.Gpu(), static_cast<int32>(SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED));
		bOk &= PumpGpuBuild(*this, C.Gpu(), *Job, TEXT("failure leg"));
		bOk &= TestEqual(TEXT("failure leg: every entry done"), SuperSLMPrefixSetTools::GetBuildProgress(*Job).EntriesDone, kEntries);
		TArray<FSuperSLMPrefixSetFinding> Findings;
		bOk &= TestEqual(*FString::Printf(TEXT("failure leg: Stored (%s)"), *Error), (uint8)SuperSLMPrefixSetTools::FinishBuildGpu(*Set, *Job, Findings, Error), (uint8)EBuildFinish::Stored);
		const FName Failing = Set->Entries[0].Name;
		bOk &= TestEqual(TEXT("failure leg: the failing entry is reported BuildFailed [GPU]"), Count(Findings, TEXT("BuildFailed"), Failing, 1), 1);
		for (const FSuperSLMPrefixSetFinding& F : Findings)
		{
			if (F.Finding == FName(TEXT("BuildFailed")))
			{
				bOk &= TestTrue(*FString::Printf(TEXT("failure leg: the line names this device (%s)"), *F.Line), F.Line.Contains(DeviceLabel(C.Vendor, C.Device)));
				bOk &= TestTrue(*FString::Printf(TEXT("failure leg: only the first entry fails (%s)"), *F.Line), F.Entry == Failing);
			}
		}
		const TArray<TArray<uint8>> After = GpuBytes();
		bOk &= TestTrue(TEXT("failure leg: the failing entry keeps its previous bytes"), After[0] == P[0]);
		for (int32 I = 1; I < kEntries; ++I)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("failure leg: entry%d is the new build's"), I), After[I].Num() > 0 && After[I] != P[I]);
		}
		bOk &= TestEqual(TEXT("failure leg: still one GPU build"), GpuBuildCount(*Set), 1);
		const FSuperSLMPrefixSetBuild* CpuAfter = CpuBuildOf(*Set);
		bOk &= TestTrue(TEXT("failure leg: the CPU build is untouched"), CpuAfter != nullptr && CpuAfter->Entries.Num() == CpuBefore.Entries.Num()
			&& CpuAfter->Entries[0].Wrapper == CpuBefore.Entries[0].Wrapper && CpuAfter->BuiltAt == CpuBefore.BuiltAt);
		bOk &= TestEqual(TEXT("failure leg: no GPU save on the game thread"), SuperSLMPrefixSetTools::GameThreadBuildCalls(), int64(0));
	}

	// 3. A cancel after one entry: the GPU bytes stay the previous build's (Q).
	if (!Cold(*this, C))
	{
		return false;
	}
	const TArray<TArray<uint8>> Q = GpuBytes();
	bOk &= EditEntries(40);
	{
		FString Error;
		TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> Job = SuperSLMPrefixSetTools::BeginBuildGpu(*Set, C.Gpu(), Error);
		if (!TestTrue(*FString::Printf(TEXT("cancel leg: the GPU build begins (%s)"), *Error), Job.IsValid()))
		{
			return false;
		}
		SuperSLMPrefixSetTools::SetBuildPauseAfterEntries(*Job, 1);
		SuperSLMPrefixSetTools::FPrefixSetBuildJob* JobPtr = Job.Get();
		bOk &= PumpGpuBuild(*this, C.Gpu(), *Job, TEXT("cancel leg, to the pause"), [JobPtr]() { return SuperSLMPrefixSetTools::GetBuildProgress(*JobPtr).bPaused; });
		bOk &= TestTrue(TEXT("cancel leg: paused after one entry"), SuperSLMPrefixSetTools::GetBuildProgress(*Job).bPaused);
		bOk &= TestEqual(TEXT("cancel leg: one entry done"), SuperSLMPrefixSetTools::GetBuildProgress(*Job).EntriesDone, 1);
		bOk &= TestTrue(TEXT("cancel leg: paused, the stored bytes are still Q"), GpuBytes() == Q);
		SuperSLMPrefixSetTools::CancelBuild(*Job);
		bOk &= PumpGpuBuild(*this, C.Gpu(), *Job, TEXT("cancel leg, after the cancel"));
		TArray<FSuperSLMPrefixSetFinding> Findings;
		bOk &= TestEqual(TEXT("cancel leg: Cancelled"), (uint8)SuperSLMPrefixSetTools::FinishBuildGpu(*Set, *Job, Findings, Error), (uint8)EBuildFinish::Cancelled);
		bOk &= TestTrue(TEXT("cancel leg: the stored bytes are byte-equal to Q"), GpuBytes() == Q);
	}

	// 4. A GPU that is unavailable: the CPU build is stored alone, the GPU build kept, GpuNotBuilt [GPU] says why.
	{
		const TCHAR* const Reason = TEXT("test seam: this editor's GPU is unavailable");
		if (!TestNotNull(TEXT("unavailable leg: a CPU build to replace"), CpuBuildOf(*Set)))
		{
			return false;
		}
		const FSuperSLMPrefixSetBuild CpuPrevious = *CpuBuildOf(*Set);
		SuperSLMPrefixSetEditor::SetNextGpuLegUnavailable(Reason);
		FString Error;
		TSharedPtr<FSuperSLMPrefixSetBuildAction> Action = FSuperSLMPrefixSetBuildAction::Start(*Set, Error, /*TestPauseAfterEntries*/ -1, /*bWithGpu*/ true);
		if (!TestTrue(*FString::Printf(TEXT("unavailable leg: the action starts (%s)"), *Error), Action.IsValid()))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("unavailable leg: finishes"), PumpAction(*Action));
		bOk &= TestEqual(TEXT("unavailable leg: Stored"), (uint8)Action->GetResult(), (uint8)EBuildFinish::Stored);
		bOk &= TestFalse(TEXT("unavailable leg: no GPU build stored"), Action->WasGpuStored());
		bOk &= TestEqual(TEXT("unavailable leg: one GpuNotBuilt [GPU]"), Count(Action->GetFindings(), TEXT("GpuNotBuilt"), NAME_None, 1), 1);
		for (const FSuperSLMPrefixSetFinding& F : Action->GetFindings())
		{
			if (F.Finding == FName(TEXT("GpuNotBuilt")))
			{
				bOk &= TestTrue(*FString::Printf(TEXT("unavailable leg: the line names the reason (%s)"), *F.Line), F.Line.Contains(Reason) && F.Line.Contains(TEXT("[GPU]")));
			}
		}
		const FSuperSLMPrefixSetBuild* CpuNow = CpuBuildOf(*Set);
		bOk &= TestTrue(TEXT("unavailable leg: the CPU build is the new one"), CpuNow != nullptr && CpuNow->Entries.Num() == kEntries
			&& CpuNow->Entries[0].Wrapper.Num() > 0 && CpuNow->Entries[0].Wrapper != CpuPrevious.Entries[0].Wrapper);
		bOk &= TestTrue(TEXT("unavailable leg: the GPU build is kept as it was (Q)"), GpuBytes() == Q);
	}
	return bOk;
}

// The other model's asset: the shipped 1.5B, imported beside the rig's A-EX.
static USuperSLMModel* ImportOtherGpuModel(FAutomationTestBase& T)
{
	FString Path, Reason;
	if (!ResolveArtifact(EGpuArtifact::TwoCap15B, Path, Reason))
	{
		T.AddError(Reason);
		return nullptr;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
	return T.TestNotNull(TEXT("the 1.5B imports"), Model) && T.TestTrue(TEXT("the 1.5B is accepted"), Diag.bAccepted) ? Model : nullptr;
}

// The U2 review's SetBeforeFirstConfigure on the GPU (§5.8): a set registered before the first Configure() is checked
// against the model the cache-on Configure() names. The set's GPU build is A-EX's, so its keys would match and serve if
// it registered; its Model is the 1.5B asset. The cache-on Configure() refuses it by name, once: PrefixSetModelMismatch
// reads 1, one line names it, nothing registers, its entry recomputes, and a later re-Configure() does not bring it back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedMismatchBeforeConfigureTest, "SuperSLM.PrefixCache.Gpu.Cooked.ModelMismatch.BeforeFirstConfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedMismatchBeforeConfigureTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<USuperSLMPrefixSet> Set;
	TArray<int32> A;
	{
		FGpuCooked Builder;
		if (!ConfigureCooked(*this, Builder) || !MakeTokens(*this, *Builder.Rig.Model, kShort, 1, A))
		{
			return false;
		}
		Set.Reset(NewSet(Builder.Rig.Model));
		Set->Entries = {SetEntry(TEXT("a"), A)};
		if (!BuildGpu(*this, Builder, *Set, TEXT("the build rig")))
		{
			return false;
		}
	} // the build rig's GPU subsystem is torn down here
	USuperSLMModel* Other = ImportOtherGpuModel(*this);
	if (Other == nullptr)
	{
		return false;
	}
	Set->Model = Other; // the set names the 1.5B; its bytes stay A-EX's build

	FGpuCooked C;
	if (!C.Rig.Init(*this))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	Gpu.RegisterPrefixSet(Set.Get()); // before the first Configure(): kept
	bool bOk = true;
	{
		SuperSLMPagingFixtures::FPagingLogCapture Log;
		if (!ConfigureCooked(*this, C, /*PrefixBlocks*/ 0, /*bInit*/ false))
		{
			return false;
		}
		bOk &= TestEqual(TEXT("PrefixSetModelMismatch reads 1 after the first cache-on Configure()"),
			SuperSLMPrefixCacheFixtures::FallbackCount(Gpu.GetPrefixCacheStats(), TEXT("PrefixSetModelMismatch")), int64(1));
		bOk &= TestEqual(TEXT("one PrefixSetModelMismatch line, naming the set"), Log.CountLinesContainingAll({TEXT("PrefixSetModelMismatch"), *Set->GetPathName()}), 1);
	}
	bOk &= TestEqual(TEXT("nothing registered"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("the set's entry: recomputed, never cooked"));
	// Refused means forgotten: a re-Configure() on A-EX does not bring it back.
	if (!Cold(*this, C))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("after a re-Configure(): still nothing registered"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
	Gpu.UnregisterPrefixSet(Set.Get());
	return bOk;
}

// Cell 2.18 on the GPU (the U2 review's SetUnregisteredByOtherModel, §5.8): the set matches A-EX and registers; a
// re-Configure() with the 1.5B unregisters it and raises PrefixSetModelMismatch once, naming the set; a later
// Configure() on A-EX does not bring it back, so its entry recomputes. The 1.5B's configuration is the [two-cap] one
// (ContextCap 4,096); its cache budget is four A-EX pages, room for a block at the 1.5B's larger page.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheGpuCookedMismatchReconfigureTest, "SuperSLM.PrefixCache.Gpu.Cooked.ModelMismatch.Reconfigure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheGpuCookedMismatchReconfigureTest::RunTest(const FString& Parameters)
{
	FGpuCooked C;
	TArray<int32> A;
	if (!ConfigureCooked(*this, C) || !MakeTokens(*this, *C.Rig.Model, kShort, 1, A))
	{
		return false;
	}
	USuperSLMGpuSubsystem& Gpu = C.Gpu();
	USuperSLMModel* Other = ImportOtherGpuModel(*this);
	if (Other == nullptr)
	{
		return false;
	}
	TStrongObjectPtr<USuperSLMPrefixSet> Set(NewSet(C.Rig.Model));
	Set->Entries = {SetEntry(TEXT("a"), A)};
	if (!BuildGpu(*this, C, *Set, TEXT("the set")) || !Cold(*this, C))
	{
		return false;
	}
	Gpu.RegisterPrefixSet(Set.Get());
	bool bOk = TestEqual(TEXT("A-EX: the set's entry registers"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 1);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Cooked, TEXT("A-EX: a cooked hit"));

	{
		SuperSLMPagingFixtures::FPagingLogCapture Log;
		FSuperSLMGpuPrefixCacheSettings Wide = C.Settings;
		Wide.BudgetBytes = C.Rig.Geo.PageBytes * 4;
		Gpu.ConfigurePrefixCache(Wide);
		const FSuperSLMGpuConfigureReport Report = Gpu.Configure(Other, MakeGpuConfig(EGpuArtifact::TwoCap15B, 2, 512));
		if (!TestEqual(*FString::Printf(TEXT("Configure(1.5B) succeeds (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success))
		{
			return false;
		}
		bOk &= TestTrue(TEXT("1.5B: the GPU cache is configured"), Gpu.GetPrefixCacheStats().bConfigured);
		bOk &= TestEqual(TEXT("1.5B: the set is unregistered"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
		bOk &= TestEqual(TEXT("1.5B: PrefixSetModelMismatch reads 1"),
			SuperSLMPrefixCacheFixtures::FallbackCount(Gpu.GetPrefixCacheStats(), TEXT("PrefixSetModelMismatch")), int64(1));
		bOk &= TestEqual(TEXT("1.5B: one PrefixSetModelMismatch line, naming the set"), Log.CountLinesContainingAll({TEXT("PrefixSetModelMismatch"), *Set->GetPathName()}), 1);
	}
	// Back to A-EX: unregistered means gone, not suspended.
	Gpu.ConfigurePrefixCache(C.Settings);
	if (!Cold(*this, C))
	{
		return false;
	}
	bOk &= TestEqual(TEXT("A-EX again: the set stays unregistered"), Gpu.GetPrefixCacheStats().CookedEntriesRegistered, 0);
	bOk &= ServedAndReturned(*this, Gpu, A, ESuperSLMPrefixSource::Recomputed, TEXT("A-EX again: recomputed"));
	Gpu.UnregisterPrefixSet(Set.Get());
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
