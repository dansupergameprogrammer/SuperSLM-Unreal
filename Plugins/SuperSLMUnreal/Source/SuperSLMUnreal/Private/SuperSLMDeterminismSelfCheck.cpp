#include "SuperSLMDeterminismSelfCheck.h"

#include "SuperSLMGpuDigestBridge.h"
#include "SuperSLMGpuSelfCheckAccess.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMJson.h"
#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"

#include "superslm/sha256.h"

// L2-S2 (the plan §7 item 11; D-SLM3857, D-SLM7228, D-SLM7223). The
// shared computation behind the self-check's editor tool and packaged console command (both
// surfaces are L2-S3/L2-S4).
//
// The workload, pinned here, is one unconstrained workload for all three comparisons
// (D-SLM7383): the configured artifact, kPinnedPrompt, no schema bound, greedy decoding, 32
// decode steps. The CPU backend runs it at layer_budget 1 and at num_hidden_layers; the GPU runs
// it on the composed path at one layer per slice and on the one-call path at a whole token. Two
// comparisons, never sharing a verdict (plan §7 item 6):
//
//  - The GPU arm asks whether THIS DEVICE is deterministic: every GPU run must be
//    token-identical to this machine's CPU runs (comparison 1).
//
//  - The CPU arm asks whether THE PLUGIN perturbs Layer 1's bits: each CPU digest must equal the
//    reference digest shipped with the plugin for this artifact, produced at the pin by Layer 1's
//    own sslm_generate, built by Layer 1's own CMake (comparison 2). With no shipped reference for
//    the artifact the CPU arm reads Not Yet Run and says why. sslm_generate cannot bind a schema
//    at the pinned Layer 1 (v1.5.0, 1.7.0 and 1.9.0), and a schema mask is applied on the host by both
//    backends, so binding one would add no device coverage; schema-path identity on the GPU is
//    R-S2e's and R-S2g's claim. The GPU arm runs the logits head the configured model asset
//    selects (the device-resident head by default) and the report names it.
//
//  - Comparison 3, the plugin's own prior run on this machine, keyed by (artifact hash, pin,
//    plugin version, backend, device label), reports changed / unchanged and never a verdict.
//    The first-ever run and the most recent run are kept per key.
//
// Quarantine is per verdict (plan §7 item 11, D-SLM7223, D-SLM7715): each stays recorded and is
// never displayed nor read into another surface until its own must-accept and must-reject pair
// has fired. The CPU verdict is released by the file pair (a corrupted shipped reference must
// make it Diverged). That pair never reaches the GPU verdict, which compares against CPU tokens
// computed live, so the GPU verdict stays quarantined until its own must-reject -- the
// FSuperSLMSelfCheckAccess seam perturbing one granularity's GPU tokens -- has passed.
namespace
{
	const TCHAR* kPinnedPrompt = TEXT("I would like to buy a health potion, please.");
	constexpr int32 kDecodeSteps = 32;

	const TCHAR* kCpuGranularityNames[2] = { TEXT("LayerBudget1"), TEXT("LayerBudgetFull") };
	const TCHAR* kGpuGranularityNames[2] = { TEXT("ComposedOneLayerPerSlice"), TEXT("OneCallWholeToken") };

	FString DigestHex(const TArray<int32>& Tokens)
	{
		uint8 Digest[32];
		SuperSLMGpuDigest::ComputeTokenDigest(Tokens, Digest);
		return SuperSLMGpuDigest::DigestToHex(Digest);
	}

	int32 FirstDifference(const TArray<int32>& A, const TArray<int32>& B)
	{
		const int32 Common = FMath::Min(A.Num(), B.Num());
		for (int32 I = 0; I < Common; ++I)
		{
			if (A[I] != B[I])
			{
				return I;
			}
		}
		return A.Num() == B.Num() ? -1 : Common;
	}

	// L2-S3 build (2026-09-25): the self-check drives the subsystem it finds from the calling (game)
	// thread, so it drives only a subsystem its GameInstance registered -- the one Tick() is the
	// game thread's to call. A USuperSLMSubsystem constructed directly by a host that drives it on
	// its own thread (the MCP sibling's long-lived host holds one in an uninitialized
	// GameInstance) is configured with the same model but never registered; driving it here would
	// run its runtime from two threads at once. Such a subsystem is skipped, and when it is the
	// only match the report names why (bOutSkippedUnregistered).
	bool IsRegisteredSubsystem(USuperSLMSubsystem& Cpu)
	{
		const UGameInstance* GameInstance = Cpu.GetGameInstance();
		return GameInstance != nullptr && GameInstance->GetSubsystem<USuperSLMSubsystem>() == &Cpu;
	}

	USuperSLMSubsystem* FindCpuSubsystemFor(const USuperSLMModel& Model, USuperSLMGpuSubsystem* Gpu, bool& bOutSkippedUnregistered)
	{
		bOutSkippedUnregistered = false;
		if (Gpu != nullptr)
		{
			if (UGameInstance* GameInstance = Gpu->GetGameInstance())
			{
				USuperSLMSubsystem* Cpu = GameInstance->GetSubsystem<USuperSLMSubsystem>();
				if (Cpu != nullptr && Cpu->GetConfiguredModel() == &Model)
				{
					return Cpu;
				}
			}
		}
		for (TObjectIterator<USuperSLMSubsystem> It; It; ++It)
		{
			if (It->GetConfiguredModel() == &Model)
			{
				if (!IsRegisteredSubsystem(**It))
				{
					bOutSkippedUnregistered = true;
					continue;
				}
				return *It;
			}
		}
		return nullptr;
	}

	// The CPU run's wall-clock ceiling. Tick() only plans and applies; the Layer-1 work runs on the
	// CPU backend's worker thread, so the run is bounded in real time, never in a count of ticks
	// (a non-waiting tick-count cap ends before the worker finishes on healthy hardware, D-SLM7696).
	//
	// Derived from the workload's physical size and two declared floors -- nothing here is read
	// from the scheduler under test (its cost model, its K, its tick schedule):
	//  - Bytes: every forward pass over a prompt or generated token streams each weight byte once
	//    and reads at most the resident KV pool, so the run moves at most
	//    (prompt + steps + 1) x (artifact bytes + KV pool bytes). The +1 is the one sequence
	//    zero-fill the vend can wait behind (the prior run's recycle reset). Artifact bytes are the
	//    artifact's own size; the KV pool bytes are Layer 1's own sslm_kv_block_size /
	//    sslm_kv_pool_overhead_size sizing, which the self-check does not grade.
	//  - Calls: with the layer budget pinned, the run makes at most
	//    prompt + steps x (num_hidden_layers + 1) + 2 Layer-1 calls (a prefill call consumes at
	//    least one token; a token at layer budget 1 takes one call per layer, the first token after
	//    prefill one finish-only call; +2 lifecycle). RunCpu sleeps only while a posted job's
	//    committed tick has come and its result has not, so each call costs at most one sleep
	//    quantum of slack beyond its own work.
	//  - kCpuFloorBytesPerSecond: 1 GiB/s of effective weight streaming, declared an order of
	//    magnitude below any machine that runs these artifacts interactively. Not measured.
	//  - kPerCallSlackSeconds: 20 ms, above the 15.625 ms default Windows timer quantum that bounds
	//    how late a Sleep(1 ms) returns. Declared.
	// A machine slower than the floor, or a CPU backend shared with other sequences for the whole
	// run, can exceed the ceiling; the run then reports that it did not finish within it, naming
	// the ceiling. It never waits unbounded.
	constexpr double kCpuFloorBytesPerSecond = 1024.0 * 1024.0 * 1024.0;
	constexpr double kPerCallSlackSeconds = 0.020;

	double CpuRunCeilingSeconds(int64 ArtifactBytes, int64 KvPoolBytes, int32 PromptTokens, int32 NumHiddenLayers)
	{
		const double TokenPasses = static_cast<double>(PromptTokens) + kDecodeSteps + 1;
		const double Bytes = TokenPasses * (static_cast<double>(FMath::Max<int64>(ArtifactBytes, 0)) + static_cast<double>(FMath::Max<int64>(KvPoolBytes, 0)));
		const double Calls = static_cast<double>(PromptTokens) + static_cast<double>(kDecodeSteps) * (FMath::Max(NumHiddenLayers, 1) + 1) + 2;
		return Bytes / kCpuFloorBytesPerSecond + Calls * kPerCallSlackSeconds;
	}

	// True when a job this run posted has reached its committed delivery tick and is still
	// undelivered -- the worker, not the schedule, is what the next Tick() would wait on, so real
	// time must pass. While the only gate is the schedule (a job's K ticks), ticking again without
	// sleeping is enough. Scans only the rows this run appended that the ledger ring still holds.
	// An undelivered job is among the newest rows, so in practice none is overwritten.
	bool WorkerIsTheGate(const USuperSLMSubsystem& Cpu, int64 LedgerRowsBefore)
	{
#if SUPERSLM_WITH_L2S1_ASYNC
		const int32 Now = Cpu.GetLastTickReport().TickIndex;
		const int64 End = Cpu.GetJobLedgerAppendedCount();
		for (int64 Row = FMath::Max<int64>(0, LedgerRowsBefore); Row < End; ++Row)
		{
			const FSuperSLMWorkerJobReport* Job = Cpu.FindJobLedgerRow(Row);
			if (Job != nullptr && Job->DeliveredAtTick < 0 && Job->CommittedDeliveryTick <= Now)
			{
				return true;
			}
		}
		return false;
#else
		return true;
#endif
	}

	// One CPU run of the workload through the plugin's own CPU scheduler, bounded by
	// CpuRunCeilingSeconds in real time -- in steps (fold-round ruling 1), so a caller can advance
	// it a bounded slice per editor frame. BeginCpuRun() vends, binds, sets the layer budget and
	// begins the generation; StepCpuRun() ticks; EndCpuRun() returns the sequence.
	struct FCpuRun
	{
		FSuperSLMSequence Seq;
		bool bVended = false;
		int64 LedgerRowsBefore = 0;
		double CeilingSeconds = 0.0;
		// The run's time against the ceiling: every step's own time, plus the time between steps
		// capped at kPerCallSlackSeconds. The blocking loop's gap is its 1 ms sleep, so there this
		// is the wall time since the generation began, as before; a frame-sliced caller's gap is
		// the editor's frame, which is not the run's.
		double ChargedSeconds = 0.0;
		double LastStepEndSeconds = 0.0;
	};

	enum class ERunStep : uint8
	{
		Running,          // the step's own time budget is spent; step again
		WaitingOnWorker,  // a posted job's committed tick has come and its result has not: real time must pass
		WaitingOnDevice,  // the GPU twin: the next Tick() would block on a submission job not yet finished
		Finished,
		Failed,
	};

	bool BeginCpuRun(USuperSLMSubsystem& Cpu, const TArray<int32>& Prompt, const FSuperSLMSchemaHandle& Schema, int32 LayerBudget,
		double CeilingSeconds, FCpuRun& Run, FString& OutError)
	{
		Run = FCpuRun();
		if (Cpu.VendSequence(Run.Seq) != ESuperSLMVendResult::Success)
		{
			OutError = TEXT("no CPU sequence could be vended for the self-check");
			return false;
		}
		Run.bVended = true; // EndCpuRun() returns it, on every path
		if (!Schema.IsNone() && !Cpu.SetSchema(Run.Seq, Schema, OutError))
		{
			return false;
		}
		Cpu.SetLayerBudget(Run.Seq, LayerBudget);
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = kDecodeSteps;
#if SUPERSLM_WITH_L2S1_ASYNC
		Run.LedgerRowsBefore = Cpu.GetJobLedgerAppendedCount();
#endif
		if (!Cpu.BeginGeneration(Run.Seq, Request, OutError))
		{
			return false;
		}
		Run.CeilingSeconds = CeilingSeconds;
		Run.LastStepEndSeconds = FPlatformTime::Seconds();
		return true;
	}

	void EndCpuRun(USuperSLMSubsystem& Cpu, FCpuRun& Run)
	{
		if (Run.bVended)
		{
			Cpu.ReturnSequence(Run.Seq);
			Run.bVended = false;
		}
	}

	// Ticks until the run stops, the worker is the gate, or StepBudgetSeconds of this call's own
	// time is spent. While the only gate is the schedule (a job's K ticks), ticking again at once is
	// enough, exactly as the blocking loop did.
	ERunStep StepCpuRun(USuperSLMSubsystem& Cpu, FCpuRun& Run, double StepBudgetSeconds, TArray<int32>& OutTokens, FString& OutError)
	{
		const double StepStart = FPlatformTime::Seconds();
		Run.ChargedSeconds += FMath::Min(FMath::Max(0.0, StepStart - Run.LastStepEndSeconds), kPerCallSlackSeconds);
		ON_SCOPE_EXIT
		{
			const double End = FPlatformTime::Seconds();
			Run.ChargedSeconds += End - StepStart;
			Run.LastStepEndSeconds = End;
		};
		for (;;)
		{
			const double InStep = FPlatformTime::Seconds() - StepStart;
			if (Run.ChargedSeconds + InStep > Run.CeilingSeconds)
			{
				OutError = FString::Printf(TEXT("the CPU self-check sequence did not finish within %.1f s, the ceiling derived from the artifact's size, the KV pool's size and a 1 GiB/s machine floor"), Run.CeilingSeconds);
				return ERunStep::Failed;
			}
			if (InStep > StepBudgetSeconds)
			{
				return ERunStep::Running;
			}
			Cpu.Tick(1.0f / 60.0f);
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Run.Seq);
			if (Phase == ESuperSLMSequencePhase::Complete)
			{
				OutTokens = Cpu.GetGeneratedTokens(Run.Seq);
				return ERunStep::Finished;
			}
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				OutTokens = Cpu.GetGeneratedTokens(Run.Seq);
				if (Cpu.GetLastDecodeOutcome(Run.Seq) == ESuperSLMDecodeOutcome::SchemaDeadEnd)
				{
					return ERunStep::Finished; // the schema is complete: the workload ends here on every backend
				}
				OutError = TEXT("the CPU self-check sequence faulted");
				return ERunStep::Failed;
			}
			if (WorkerIsTheGate(Cpu, Run.LedgerRowsBefore))
			{
				return ERunStep::WaitingOnWorker;
			}
		}
	}

	// One GPU run, in the same steps, through FSuperSLMSelfCheckAccess's own Begin/Step/End
	// (RunGeneration() is those in a loop), bounded by its tick count.
	//
	// T-2818 nonblock: the step never waits on the GPU. The run ticks back to back, so its ticks
	// outrun the device: an event comes due K ticks after its request with no real time passed.
	// Before the ruling of 2026-09-26 GPU Tick()'s ApplyDue() then waited on the game thread until
	// the submission thread finished that job (a packaged run logged one 1,074 ms frame from it);
	// since then the tick never waits, and a tick taken while the device is the gate would only
	// spend one of MaxTicks doing nothing. Before each tick the step asks whether the device is the
	// gate (NextTickWouldWaitOnDevice(), which forwards to NextTickGatedOnDevice()) and, when it is,
	// returns WaitingOnDevice without ticking, so the wait becomes real time between the caller's
	// frames, as the CPU arm's WaitingOnWorker does.
	// Only when ticks happen changes, never which ticks or what they compute: the tick sequence,
	// its DeltaSeconds and the jobs it plans are the same, so the tokens, digests and verdicts are
	// unchanged. Ticks is counted only when a tick runs, so MaxTicks means what it meant.
	struct FGpuRun
	{
		FSuperSLMGpuSequence Seq;
		bool bVended = false;
		int64 MaxTicks = 0;
		int64 Ticks = 0;
	};

	ERunStep StepGpuRun(USuperSLMGpuSubsystem& Gpu, FGpuRun& Run, double StepBudgetSeconds, TArray<int32>& OutTokens, FString& OutError)
	{
		const double StepStart = FPlatformTime::Seconds();
		for (;;)
		{
			if (Run.Ticks >= Run.MaxTicks)
			{
				OutError = TEXT("the GPU self-check sequence did not finish");
				return ERunStep::Failed;
			}
			if (FPlatformTime::Seconds() - StepStart > StepBudgetSeconds)
			{
				return ERunStep::Running;
			}
			if (FSuperSLMSelfCheckAccess::NextTickWouldWaitOnDevice(Gpu))
			{
				return ERunStep::WaitingOnDevice;
			}
			bool bDeadEnd = false;
			const int32 Step = FSuperSLMSelfCheckAccess::StepGeneration(Gpu, Run.Seq, OutTokens, bDeadEnd, OutError);
			++Run.Ticks;
			if (Step != 0)
			{
				return Step > 0 ? ERunStep::Finished : ERunStep::Failed;
			}
		}
	}

	struct FReferenceEntry
	{
		FString DigestHex;
		TArray<int32> Tokens; // optional in the file
		bool bHasTokens = false;
	};

	bool LoadReference(const FString& Path, const FString& ArtifactHash, TMap<FString, FReferenceEntry>& OutEntries, FString& OutWhy)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutWhy = FString::Printf(TEXT("no reference digest file for this artifact at '%s'"), *Path);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		FString ReadError;
		if (!SuperSLMJson::TryReadObject(Text, Root, ReadError)) // R4-W1: refuses a trailing backslash
		{
			OutWhy = FString::Printf(TEXT("the reference digest file '%s' is not valid JSON (%s)"), *Path, *ReadError);
			return false;
		}
		FString FileHash;
		Root->TryGetStringField(TEXT("artifactHash"), FileHash);
		if (!FileHash.Equals(ArtifactHash, ESearchCase::IgnoreCase))
		{
			OutWhy = FString::Printf(TEXT("the reference digest file '%s' is for artifact %s, not %s"), *Path, *FileHash, *ArtifactHash);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Root->TryGetArrayField(TEXT("entries"), Entries) || Entries == nullptr)
		{
			OutWhy = FString::Printf(TEXT("the reference digest file '%s' has no entries"), *Path);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FString Backend, Granularity;
			FReferenceEntry Entry;
			Obj->TryGetStringField(TEXT("backend"), Backend);
			Obj->TryGetStringField(TEXT("granularity"), Granularity);
			Obj->TryGetStringField(TEXT("tokenDigestHex"), Entry.DigestHex);
			const TArray<TSharedPtr<FJsonValue>>* Tokens = nullptr;
			if (Obj->TryGetArrayField(TEXT("tokens"), Tokens) && Tokens != nullptr)
			{
				Entry.bHasTokens = true;
				for (const TSharedPtr<FJsonValue>& T : *Tokens)
				{
					Entry.Tokens.Add(static_cast<int32>(T->AsNumber()));
				}
			}
			if (Backend.Equals(TEXT("CPU"), ESearchCase::IgnoreCase))
			{
				OutEntries.Add(Granularity, Entry);
			}
		}
		return true;
	}

	FString DefaultReferencePath(const FString& ArtifactHash)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		const FString Base = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::ProjectPluginsDir() / TEXT("SuperSLMUnreal");
		return Base / TEXT("Resources") / TEXT("SelfCheck") / (ArtifactHash + TEXT(".reference_digests.json"));
	}

	// Comparison 3: changed / unchanged against this machine's own prior run for the same key.
	void ComparePriorRun(const FSuperSLMSelfCheckReport& Report, const TCHAR* Backend, const TArray<FString>& Digests, FSuperSLMSelfCheckBackendResult& Result)
	{
		const FString Key = FString::Join(TArray<FString>{ Report.ArtifactHash, Report.LayerOneTagAndCommit, Report.PluginVersion, Backend, Report.DeviceLabel }, TEXT("|"));
		const FTCHARToUTF8 KeyUtf8(*Key);
		uint8 KeyHash[32];
		superslm::Sha256Hash(reinterpret_cast<const uint8_t*>(KeyUtf8.Get()), static_cast<size_t>(KeyUtf8.Length()), KeyHash);
		const FString Path = FPaths::ProjectSavedDir() / TEXT("SuperSLM") / TEXT("SelfCheck") / (BytesToHex(KeyHash, 16).ToLower() + TEXT(".json"));
		const FString Joined = FString::Join(Digests, TEXT(","));

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			// R4-W1: a file left truncated by a crash mid-write is refused (no prior run), never
			// handed to the reader's backslash overrun.
			TSharedPtr<FJsonObject> Prior;
			FString ReadError;
			if (SuperSLMJson::TryReadObject(Text, Prior, ReadError))
			{
				FString Last;
				if (Prior->TryGetStringField(TEXT("last"), Last))
				{
					Result.bHadPriorRun = true;
					Result.bChangedFromPriorRun = Last != Joined;
				}
				Root = Prior;
			}
		}
		Root->SetStringField(TEXT("key"), Key);
		if (!Root->HasField(TEXT("first")))
		{
			Root->SetStringField(TEXT("first"), Joined);
		}
		Root->SetStringField(TEXT("last"), Joined);
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FFileHelper::SaveStringToFile(Out, *Path);
	}
}

// Fold-round ruling 1: the self-check as a run that advances in steps. Every step is game-thread
// work -- the subsystems' own Tick()/vend/return API -- and each is bounded by the caller's step
// budget, so an editor surface calls Step() once per frame and the game thread pays a few
// milliseconds of scheduling per frame; the inference itself runs on the CPU backend's worker
// threads and the GPU backend's submission thread, as it always did. No step waits on either:
// the CPU arm returns WaitingOnWorker and the GPU arm WaitingOnDevice (T-2818 nonblock) when the
// next tick would. The blocking Run() is the same run stepped without a budget, sleeping 1 ms only
// while the CPU worker or the GPU device is the gate.
struct FSuperSLMSelfCheckRun::FImpl
{
	enum class EPhase : uint8 { CpuRuns, GpuRuns, Done };

	TWeakObjectPtr<USuperSLMModel> Model;
	FString ReferenceDigestFilePathOverride;
	int32 PerturbGpuGranularity = -1;
	int32 PerturbGpuStep = -1;

	FSuperSLMSelfCheckReport Report;
	EPhase Phase = EPhase::Done;
	TWeakObjectPtr<USuperSLMSubsystem> Cpu;
	TWeakObjectPtr<USuperSLMGpuSubsystem> Gpu;
	TArray<int32> Prompt;
	int32 CpuBudgets[2] = { 1, 1 };
	double CpuCeilingSeconds = 0.0;
	FString LogitScope;

	int32 Granularity = 0;
	bool bRunStarted = false;
	bool bWaitingOnWorker = false;
	FCpuRun CpuRun;
	FGpuRun GpuRun;

	TArray<FString> CpuDigests;
	bool bCpuRan = true;
	TArray<int32> CpuUnconstrained[2];
	TArray<FString> GpuDigests;
	TArray<int32> CpuReference;

	// The one-shot RunImpl()'s setup, moved here unchanged: finds the subsystems, encodes the prompt, reads the shape.
	// Leaves Phase at Done when the run cannot start, with the report saying why.
	void Setup(USuperSLMModel& InModel)
	{
		Model = &InModel;
		Report.DeviceLabel = FPlatformMisc::GetPrimaryGPUBrand();
		Report.LayerOneTagAndCommit = FString::Printf(TEXT("%s %s"), TEXT(SUPERSLM_LAYER1_TAG), TEXT(SUPERSLM_LAYER1_COMMIT));
		Report.PluginVersion = TEXT(SUPERSLM_PLUGIN_VERSION);
		uint8 Hash[32];
		if (SuperSLMRuntime::ReadArtifactHash(InModel, Hash))
		{
			Report.ArtifactHash = SuperSLMRuntime::HashToHex(Hash);
		}
		// Plan §2.5 row 14, §7 item 11: the scope names the compiled pin, never a literal.
		LogitScope = FString::Printf(TEXT("final-logit digest unavailable at Layer 1 %s"), TEXT(SUPERSLM_LAYER1_TAG));
		Report.Cpu.ScopeText = TEXT("Not run: ");
		Report.Gpu.ScopeText = TEXT("Not run: ");

		USuperSLMGpuSubsystem* FoundGpu = FSuperSLMSelfCheckAccess::FindGpuSubsystemFor(InModel);
		bool bSkippedUnregistered = false;
		USuperSLMSubsystem* FoundCpu = FindCpuSubsystemFor(InModel, FoundGpu, bSkippedUnregistered);
		if (FoundCpu == nullptr)
		{
			const TCHAR* Why = bSkippedUnregistered
				? TEXT("the only CPU subsystem configured with this model is not a GameInstance's registered subsystem (a host that drives it on its own thread, such as the MCP tier's); the self-check does not drive it")
				: TEXT("no CPU subsystem is configured with this model");
			Report.Cpu.ScopeText += Why;
			Report.Gpu.ScopeText += FString::Printf(TEXT("the GPU arm compares against this machine's CPU backend, and %s"), Why);
			return;
		}
		Cpu = FoundCpu;
		Gpu = FoundGpu;

		if (!FoundCpu->Tokenize(kPinnedPrompt, Prompt))
		{
			Report.Cpu.ScopeText += TEXT("the artifact carries no tokenizer, so the pinned prompt cannot be encoded");
			Report.Gpu.ScopeText += TEXT("the artifact carries no tokenizer, so the pinned prompt cannot be encoded");
			return;
		}
		SuperSLMRuntime::FModelShape Shape;
		FString ShapeError;
		if (!SuperSLMRuntime::ReadModelShape(InModel, Shape, ShapeError))
		{
			Report.Cpu.ScopeText += ShapeError;
			Report.Gpu.ScopeText += ShapeError;
			return;
		}
		CpuBudgets[1] = Shape.NumHiddenLayers;
		CpuCeilingSeconds = CpuRunCeilingSeconds(InModel.GetMappedArtifactSize(), FoundCpu->GetKvPoolReservedBytes(), Prompt.Num(), Shape.NumHiddenLayers);
		Phase = EPhase::CpuRuns;
	}

	// The CPU arm's verdict once both CPU runs are in (comparison 2, then 3), then the GPU arm's
	// preconditions. The one-shot RunImpl()'s code, moved here unchanged.
	void FinishCpuArm()
	{
		Phase = EPhase::Done;
		if (bCpuRan)
		{
			const FString Path = ReferenceDigestFilePathOverride.IsEmpty() ? DefaultReferencePath(Report.ArtifactHash) : ReferenceDigestFilePathOverride;
			TMap<FString, FReferenceEntry> Reference;
			FString Why;
			if (!LoadReference(Path, Report.ArtifactHash, Reference, Why))
			{
				Report.Cpu.Verdict = ESuperSLMSelfCheckVerdict::NotYetRun;
				Report.Cpu.ScopeText = FString::Printf(TEXT("Not run: %s; the CPU arm needs the shipped reference produced by Layer 1's own sslm_generate"), *Why);
			}
			else
			{
				Report.Cpu.bReferenceFileAvailable = true;
				Report.Cpu.Verdict = ESuperSLMSelfCheckVerdict::Verified;
				for (int32 G = 0; G < 2 && Report.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Verified; ++G)
				{
					const FReferenceEntry* Entry = Reference.Find(kCpuGranularityNames[G]);
					if (Entry == nullptr || !Entry->DigestHex.Equals(CpuDigests[G], ESearchCase::IgnoreCase))
					{
						Report.Cpu.Verdict = ESuperSLMSelfCheckVerdict::Diverged;
						Report.Cpu.FirstDivergingGranularityIndex = G;
						// The step is locatable only when the reference carries its tokens.
						Report.Cpu.FirstDivergingStep = (Entry != nullptr && Entry->bHasTokens)
							? FMath::Max(0, FirstDifference(CpuUnconstrained[G], Entry->Tokens)) : 0;
					}
				}
				Report.Cpu.ScopeText = FString::Printf(TEXT("CPU tokens %s Layer 1's own sslm_generate reference on the reference workload, unconstrained "
					"(%d steps x 2 layer budgets: %s, %s); %s"),
					Report.Cpu.Verdict == ESuperSLMSelfCheckVerdict::Verified ? TEXT("identical to") : TEXT("differ from"),
					kDecodeSteps, kCpuGranularityNames[0], kCpuGranularityNames[1], *LogitScope);
			}
			ComparePriorRun(Report, TEXT("CPU"), CpuDigests, Report.Cpu);
		}

		// --- GPU arm: the same unconstrained workload, against this machine's CPU (comparison 1) ---
		if (!Gpu.IsValid())
		{
			Report.Gpu.ScopeText += TEXT("no active GPU subsystem is configured with this model");
			return;
		}
		if (!bCpuRan)
		{
			Report.Gpu.ScopeText += TEXT("the CPU reference runs did not complete (the CPU arm reports why)");
			return;
		}
		if (CpuUnconstrained[0] != CpuUnconstrained[1])
		{
			Report.Gpu.ScopeText += TEXT("this machine's CPU backend disagrees with itself across layer budgets, so it cannot serve as the GPU's reference (the CPU arm reports why)");
			return;
		}
		CpuReference = CpuUnconstrained[1];
		Report.Gpu.Verdict = ESuperSLMSelfCheckVerdict::Verified;
		Granularity = 0;
		bRunStarted = false;
		Phase = EPhase::GpuRuns;
	}

	void FailGpuRun(const FString& Error)
	{
		Report.Gpu.Verdict = ESuperSLMSelfCheckVerdict::NotYetRun;
		Report.Gpu.ScopeText = FString::Printf(TEXT("Not run: the GPU %s run failed: %s"), kGpuGranularityNames[Granularity], *Error);
		Phase = EPhase::Done;
	}

	void FinishGpuArm(USuperSLMGpuSubsystem& G)
	{
		// In 1.0 the GPU verdict is always withheld (recorded, never displayed), and its scope text
		// says so; the report also names the logits head the GPU arm ran.
		Report.Gpu.ScopeText = FString::Printf(TEXT("GPU tokens %s this machine's CPU backend on the reference workload, unconstrained (%d steps x 2 granularities: %s, %s; logits head %s); %s. "
			"In 1.0 this GPU verdict is always withheld: recorded in the report, shown as Not Yet Run"),
			Report.Gpu.Verdict == ESuperSLMSelfCheckVerdict::Verified ? TEXT("identical to") : TEXT("differ from"),
			kDecodeSteps, kGpuGranularityNames[0], kGpuGranularityNames[1], *G.GetDeviceHeadStatus(), *LogitScope);
		Report.Gpu.bQuarantined = true; // the same condition the scope text states (L2-S3 build)
		ComparePriorRun(Report, TEXT("GPU"), GpuDigests, Report.Gpu);
		Phase = EPhase::Done;
	}

	bool Step(double StepBudgetSeconds)
	{
		bWaitingOnWorker = false;
		if (Phase == EPhase::CpuRuns)
		{
			USuperSLMSubsystem* C = Cpu.Get();
			if (C == nullptr || !Model.IsValid())
			{
				Report.Cpu.ScopeText += TEXT("the model or its CPU subsystem went away during the self-check");
				Phase = EPhase::Done;
				return true;
			}
			FString Error;
			if (!bRunStarted)
			{
				bRunStarted = true;
				if (!BeginCpuRun(*C, Prompt, FSuperSLMSchemaHandle(), CpuBudgets[Granularity], CpuCeilingSeconds, CpuRun, Error))
				{
					EndCpuRun(*C, CpuRun);
					Report.Cpu.ScopeText += Error;
					bCpuRan = false;
					FinishCpuArm();
				}
				return Phase == EPhase::Done;
			}
			TArray<int32> Tokens;
			const ERunStep Result = StepCpuRun(*C, CpuRun, StepBudgetSeconds, Tokens, Error);
			if (Result == ERunStep::Running || Result == ERunStep::WaitingOnWorker)
			{
				bWaitingOnWorker = Result == ERunStep::WaitingOnWorker;
				return false;
			}
			EndCpuRun(*C, CpuRun);
			bRunStarted = false;
			if (Result == ERunStep::Failed)
			{
				Report.Cpu.ScopeText += Error;
				bCpuRan = false;
				FinishCpuArm();
				return Phase == EPhase::Done;
			}
			CpuUnconstrained[Granularity] = MoveTemp(Tokens);
			CpuDigests.Add(DigestHex(CpuUnconstrained[Granularity]));
			if (++Granularity == 2)
			{
				FinishCpuArm();
			}
			return Phase == EPhase::Done;
		}
		if (Phase == EPhase::GpuRuns)
		{
			USuperSLMGpuSubsystem* G = Gpu.Get();
			if (G == nullptr)
			{
				FailGpuRun(TEXT("the GPU subsystem went away during the self-check"));
				return true;
			}
			const ESuperSLMGpuDecodePath Paths[2] = { ESuperSLMGpuDecodePath::Composed, ESuperSLMGpuDecodePath::OneCall };
			FString Error;
			if (!bRunStarted)
			{
				bRunStarted = true;
				GpuRun = FGpuRun();
				if (!FSuperSLMSelfCheckAccess::BeginGeneration(*G, Paths[Granularity], Granularity == 0 ? 1 : 0, Prompt, FString(), kDecodeSteps, GpuRun.Seq, GpuRun.MaxTicks, Error))
				{
					FailGpuRun(Error);
					return true;
				}
				GpuRun.bVended = true;
				return false;
			}
			TArray<int32> Tokens;
			const ERunStep Result = StepGpuRun(*G, GpuRun, StepBudgetSeconds, Tokens, Error);
			if (Result == ERunStep::Running || Result == ERunStep::WaitingOnDevice)
			{
				bWaitingOnWorker = Result == ERunStep::WaitingOnDevice;
				return false;
			}
			FSuperSLMSelfCheckAccess::EndGeneration(*G, GpuRun.Seq);
			GpuRun.bVended = false;
			bRunStarted = false;
			if (Result == ERunStep::Failed)
			{
				FailGpuRun(Error);
				return true;
			}
			if (Granularity == PerturbGpuGranularity && Tokens.IsValidIndex(PerturbGpuStep))
			{
				Tokens[PerturbGpuStep] ^= 1;
			}
			GpuDigests.Add(DigestHex(Tokens));
			const int32 Diff = FirstDifference(Tokens, CpuReference);
			if (Diff >= 0 && Report.Gpu.Verdict == ESuperSLMSelfCheckVerdict::Verified)
			{
				Report.Gpu.Verdict = ESuperSLMSelfCheckVerdict::Diverged;
				Report.Gpu.FirstDivergingStep = Diff;
				Report.Gpu.FirstDivergingGranularityIndex = Granularity;
			}
			if (++Granularity == 2)
			{
				FinishGpuArm(*G);
			}
			return Phase == EPhase::Done;
		}
		return true;
	}

	// A run abandoned mid-way returns the sequence it holds.
	~FImpl()
	{
		if (CpuRun.bVended)
		{
			if (USuperSLMSubsystem* C = Cpu.Get())
			{
				EndCpuRun(*C, CpuRun);
			}
		}
		if (GpuRun.bVended)
		{
			if (USuperSLMGpuSubsystem* G = Gpu.Get())
			{
				FSuperSLMSelfCheckAccess::EndGeneration(*G, GpuRun.Seq);
			}
		}
	}
};

FSuperSLMSelfCheckRun::FSuperSLMSelfCheckRun(USuperSLMModel& Model, const FString& ReferenceDigestFilePathOverride)
	: Impl(MakeUnique<FImpl>())
{
	check(IsInGameThread());
	Impl->ReferenceDigestFilePathOverride = ReferenceDigestFilePathOverride;
	Impl->Setup(Model);
}

FSuperSLMSelfCheckRun::~FSuperSLMSelfCheckRun() = default;

bool FSuperSLMSelfCheckRun::Step(double StepBudgetSeconds)
{
	check(IsInGameThread());
	return Impl->Step(StepBudgetSeconds);
}

bool FSuperSLMSelfCheckRun::IsDone() const
{
	return Impl->Phase == FImpl::EPhase::Done;
}

bool FSuperSLMSelfCheckRun::IsWaitingOnWorker() const
{
	return Impl->bWaitingOnWorker;
}

const FSuperSLMSelfCheckReport& FSuperSLMSelfCheckRun::GetReport() const
{
	return Impl->Report;
}

#if WITH_DEV_AUTOMATION_TESTS
void FSuperSLMSelfCheckRun::SetGpuTokenPerturbation(int32 GranularityIndex, int32 StepIndex)
{
	Impl->PerturbGpuGranularity = GranularityIndex;
	Impl->PerturbGpuStep = StepIndex;
}
#endif

namespace SuperSLMDeterminismSelfCheck
{
	// The blocking form: the same run, stepped without a budget, sleeping 1 ms only while the CPU
	// worker or the GPU device is the gate (the loop RunCpu() ran before the run was split into
	// steps; the GPU arm's wait moved here from ApplyDue()'s former Wait(), T-2818 nonblock).
	static FSuperSLMSelfCheckReport RunToCompletion(FSuperSLMSelfCheckRun& Run)
	{
		while (!Run.Step(TNumericLimits<double>::Max()))
		{
			if (Run.IsWaitingOnWorker())
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		return Run.GetReport();
	}

	FSuperSLMSelfCheckReport Run(USuperSLMModel& Model, const FString& ReferenceDigestFilePathOverride)
	{
		FSuperSLMSelfCheckRun SelfCheck(Model, ReferenceDigestFilePathOverride);
		return RunToCompletion(SelfCheck);
	}

#if WITH_DEV_AUTOMATION_TESTS
	FSuperSLMSelfCheckReport RunWithGpuTokenPerturbation(USuperSLMModel& Model,
		const FString& ReferenceDigestFilePathOverride, int32 GranularityIndex, int32 StepIndex)
	{
		FSuperSLMSelfCheckRun SelfCheck(Model, ReferenceDigestFilePathOverride);
		SelfCheck.SetGpuTokenPerturbation(GranularityIndex, StepIndex);
		return RunToCompletion(SelfCheck);
	}
#endif
}
