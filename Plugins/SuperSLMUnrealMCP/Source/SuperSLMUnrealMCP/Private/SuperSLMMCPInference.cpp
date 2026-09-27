#include "SuperSLMMCPInference.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMImportDiagnostic.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
	// The most tokens one constrained inference may generate before the call fails as
	// "budget exhausted". Generation stops as soon as the schema's walk accepts, so a correct
	// answer never reaches this; it bounds a walk that never closes. Declared, not derived.
	constexpr int32 kMaxNewTokens = 128;

	// Wall-clock ceiling for one input's generation, in seconds. Declared, not derived; a machine
	// that exceeds it fails the call by name rather than waiting unbounded.
	constexpr double kPerPromptCeilingSeconds = 300.0;

	// The decode loop's pace: one Tick() per 1/60 s, a game frame's rate, with a sleep of the same
	// length between ticks. Tick() plans and applies only; the Layer-1 work runs on the subsystem's
	// worker lane meanwhile. The pace also bounds memory: every Tick() appends one report to the
	// subsystem's tick history, which is cleared only when the runtime is configured again (or
	// released after kIdleReleaseSeconds idle). At this pace continuous use grows it by about
	// 3 KB/s, not the ~48 KB/s of a 1 ms loop. The main plugin exposes no cap or trim for that
	// history today; one is asked for in the pending-decisions record. Declared.
	constexpr float kTickSeconds = 1.0f / 60.0f;

	// Read-tier tasks waiting behind the running one before a new call is refused as busy. Each
	// task can read a whole artifact (hundreds of MB), so the queue is small. Declared.
	constexpr int32 kMaxQueuedBackgroundTasks = 8;

	// Inspections kept, per model identity. An inspection is a few kilobytes. Declared.
	constexpr int32 kMaxCachedInspections = 16;

	// The action tier's runtime (the model's mapping, one KV block, the workspace) is released after
	// this long without a run, and rebuilt by the next run. Declared.
	constexpr double kIdleReleaseSeconds = 300.0;

	// The runtime shape one author-time run configures: one sequence, decoded one whole token per
	// call (MaxLayerBudget = num_hidden_layers). There is no frame to protect here, and a 1000 ms tick
	// budget gives every job K = 1; the same values the plugin's own editor automation configures
	// with.
	FSuperSLMRuntimeConfig AuthorTimeConfig(int32 NumHiddenLayers)
	{
		FSuperSLMRuntimeConfig Config;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = NumHiddenLayers;
		Config.BlockCount = 1;
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		return Config;
	}

	// A file's identity for reuse: its full path, size and timestamp, so a rewritten file is read
	// again. Any thread (a file stat).
	FString FileKeyFor(const FString& Path)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(Path);
		IFileManager& Files = IFileManager::Get();
		return FString::Printf(TEXT("%s|%lld|%s"), *Full, Files.FileSize(*Full), *Files.GetTimeStamp(*Full).ToIso8601());
	}

	// One prompt through the configured subsystem, on the calling (dedicated) thread: a fresh
	// sequence, the schema bound before the prompt, greedy decode until the walk accepts. The
	// accepting reading and the tokens are the same delivered job's (GetStats and
	// GetGeneratedTokens both reflect the most recent delivery), so the tokens taken are exactly
	// the span that closed the schema.
	bool DriveOnePrompt(USuperSLMSubsystem& Cpu, const FSuperSLMSchemaHandle& Schema, int32 LayerBudget,
		const TArray<int32>& PromptTokens, const std::atomic<bool>& bCancel, TArray<int32>& OutTokens, FString& OutError)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		auto TimedOut = [StartSeconds]() { return FPlatformTime::Seconds() - StartSeconds > kPerPromptCeilingSeconds; };
		auto Cancelled = [&bCancel, &OutError]()
		{
			if (bCancel.load(std::memory_order_relaxed))
			{
				OutError = TEXT("cancelled: the editor is shutting down");
				return true;
			}
			return false;
		};

		// With one pool block, the previous prompt's returned sequence can still be draining.
		FSuperSLMSequence Seq;
		for (;;)
		{
			const ESuperSLMVendResult Vend = Cpu.VendSequence(Seq);
			if (Vend == ESuperSLMVendResult::Success)
			{
				break;
			}
			if (Cancelled())
			{
				return false;
			}
			if (Vend == ESuperSLMVendResult::NotConfigured || TimedOut())
			{
				OutError = Vend == ESuperSLMVendResult::NotConfigured
					? TEXT("the runtime is not configured")
					: FString::Printf(TEXT("no sequence became free within %.0f s"), kPerPromptCeilingSeconds);
				return false;
			}
			Cpu.Tick(kTickSeconds);
			FPlatformProcess::Sleep(kTickSeconds);
		}
		ON_SCOPE_EXIT { Cpu.ReturnSequence(Seq); };

		if (!Cpu.SetSchema(Seq, Schema, OutError))
		{
			return false;
		}
		Cpu.SetLayerBudget(Seq, LayerBudget);

		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = PromptTokens;
		Request.MaxNewTokens = kMaxNewTokens;
		if (!Cpu.BeginGeneration(Seq, Request, OutError))
		{
			return false;
		}

		while (!TimedOut())
		{
			if (Cancelled())
			{
				return false;
			}
			Cpu.Tick(kTickSeconds);
			if (Cpu.GetStats(Seq).bSchemaAccepting)
			{
				OutTokens = Cpu.GetGeneratedTokens(Seq);
				return true;
			}
			const ESuperSLMSequencePhase Phase = Cpu.GetPhase(Seq);
			if (Phase == ESuperSLMSequencePhase::Complete)
			{
				OutError = FString::Printf(TEXT("generated %d tokens without the schema's walk accepting (budget exhausted)"),
					Cpu.GetGeneratedTokens(Seq).Num());
				return false;
			}
			if (Phase == ESuperSLMSequencePhase::Faulted)
			{
				OutError = Cpu.GetLastDecodeOutcome(Seq) == ESuperSLMDecodeOutcome::SchemaDeadEnd
					? FString::Printf(TEXT("the schema's walk dead-ended after %d tokens without accepting"), Cpu.GetGeneratedTokens(Seq).Num())
					: TEXT("the sequence faulted");
				return false;
			}
			FPlatformProcess::Sleep(kTickSeconds);
		}
		OutError = FString::Printf(TEXT("generation did not finish within %.0f s"), kPerPromptCeilingSeconds);
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// The read tier's background queue: one task at a time, on the thread pool, bounded.
	// ------------------------------------------------------------------------------------------
	class FBackgroundQueue
	{
	public:
		// False, with nothing queued, when the queue is full or draining.
		bool Add(TUniqueFunction<void()>&& Work, TUniqueFunction<void()>&& OnGameThread)
		{
			check(IsInGameThread());
			if (bDraining || Pending.Num() >= kMaxQueuedBackgroundTasks)
			{
				return false;
			}
			Pending.Add(FTask{MoveTemp(Work), MoveTemp(OnGameThread)});
			Pump();
			return true;
		}

		// Shutdown. The ticker is unregistered first, and from here on Add refuses and Pump starts
		// nothing, so a continuation that tries to queue more work (ReportCalibration's inspection
		// continuation does) gets a refusal instead of a new pool task. Waits for the running task
		// and runs its continuation. Queued tasks' work is skipped; their continuations still run,
		// and read the cancellation their callers seeded. Loops until the queue is idle, so on
		// return no task is running, none is queued and no ticker points at this queue.
		void Drain()
		{
			check(IsInGameThread());
			bDraining = true;
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			while (Running.IsSet() || Pending.Num() > 0)
			{
				if (Running.IsSet())
				{
					Running->Future.Wait();
					TUniqueFunction<void()> OnGameThread = MoveTemp(Running->OnGameThread);
					Running.Reset();
					OnGameThread();
				}
				TArray<FTask> Skipped = MoveTemp(Pending);
				for (FTask& Task : Skipped)
				{
					Task.OnGameThread();
				}
			}
			check(!TickerHandle.IsValid());
		}

	private:
		struct FTask
		{
			TUniqueFunction<void()> Work;
			TUniqueFunction<void()> OnGameThread;
		};
		struct FRunning
		{
			TFuture<void> Future;
			TUniqueFunction<void()> OnGameThread;
		};

		void Pump()
		{
			if (bDraining || Running.IsSet() || Pending.Num() == 0)
			{
				return;
			}
			FTask Task = MoveTemp(Pending[0]);
			Pending.RemoveAt(0);
			Running.Emplace(FRunning{Async(EAsyncExecution::ThreadPool, MoveTemp(Task.Work)), MoveTemp(Task.OnGameThread)});
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FBackgroundQueue::Poll), 0.0f);
			}
		}

		// Game thread, once per frame while a task runs. Never waits.
		bool Poll(float)
		{
			if (Running.IsSet() && Running->Future.IsReady())
			{
				TUniqueFunction<void()> OnGameThread = MoveTemp(Running->OnGameThread);
				Running.Reset();
				OnGameThread(); // may add a task, and so start one
				Pump();
			}
			if (!Running.IsSet())
			{
				TickerHandle.Reset();
				return false;
			}
			return true;
		}

		TArray<FTask> Pending;
		TOptional<FRunning> Running;
		FTSTicker::FDelegateHandle TickerHandle;
		bool bDraining = false;
	};

	TUniquePtr<FBackgroundQueue> GBackground; // game thread only

	// Set by ShutdownHost. From then on no queue or host is created again, so nothing the module
	// owns can start work or register a ticker after its shutdown. Game thread only.
	bool GShutDown = false;

	// Null after shutdown.
	FBackgroundQueue* GetBackground()
	{
		check(IsInGameThread());
		if (GShutDown)
		{
			return nullptr;
		}
		if (!GBackground.IsValid())
		{
			GBackground = MakeUnique<FBackgroundQueue>();
		}
		return GBackground.Get();
	}

	// ------------------------------------------------------------------------------------------
	// Inspection cache, any thread. An asset is keyed by object identity (FObjectKey: index and
	// serial number, so a reused address is a different key) and checked against the very bytes
	// object it was inspected from: the entry keeps a weak reference to that pin's bytes, and a hit
	// needs the weak reference to still resolve to the caller's pin. A weak reference keeps its
	// reference controller alive, so a reimport's new bytes can never compare equal to it, even at
	// a reused address. The entry does not keep the bytes alive. A file is keyed by FileKeyFor.
	// ------------------------------------------------------------------------------------------
	using FWeakArtifactBytes = TWeakPtr<const FSuperSLMArtifactBytes, ESPMode::ThreadSafe>;

	struct FCachedInspection
	{
		FWeakArtifactBytes Bytes; // an asset's bytes at inspection time; unset for a file
		bool bFile = false;
		FSuperSLMModelInspection Inspection;
	};

	class FInspectionCache
	{
	public:
		// Pin is null for a file.
		bool Find(const FString& Key, const FSuperSLMArtifactBytesPin& Pin, FSuperSLMModelInspection& Out)
		{
			FScopeLock Lock(&Mutex);
			const FCachedInspection* Hit = Entries.Find(Key);
			if (Hit == nullptr)
			{
				return false;
			}
			const bool bSame = Pin.IsValid() ? (!Hit->bFile && Hit->Bytes.Pin() == Pin) : Hit->bFile;
			if (!bSame)
			{
				return false;
			}
			Out = Hit->Inspection;
			return true;
		}

		void Add(const FString& Key, const FSuperSLMArtifactBytesPin& Pin, const FSuperSLMModelInspection& Inspection)
		{
			if (!Inspection.bValid)
			{
				return;
			}
			FScopeLock Lock(&Mutex);
			if (!Entries.Contains(Key))
			{
				Order.Add(Key);
				while (Order.Num() > kMaxCachedInspections)
				{
					Entries.Remove(Order[0]);
					Order.RemoveAt(0);
				}
			}
			Entries.Add(Key, FCachedInspection{FWeakArtifactBytes(Pin), !Pin.IsValid(), Inspection});
		}

	private:
		FCriticalSection Mutex;
		TMap<FString, FCachedInspection> Entries;
		TArray<FString> Order;
	};

	FInspectionCache GInspectionCache;

	// The artifact's own Provenance section, read from the inspector's section table (a file has no
	// asset to carry it). Empty when absent.
	FString ProvenanceFromSections(const uint8* Bytes, int64 Size, const FSuperSLMModelInspection& Inspection)
	{
		for (const FSuperSLMSectionRow& Row : Inspection.Sections)
		{
			if (Row.TypeName == TEXT("Provenance") && Row.Offset >= 0 && Row.ByteSize > 0 && Row.Offset + Row.ByteSize <= Size)
			{
				const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes + Row.Offset), static_cast<int32>(Row.ByteSize));
				return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
			}
		}
		return FString();
	}

	// A worker: a file's inspection. The file is read into a private buffer aligned as an imported
	// model's bytes are, inspected, and freed.
	FSuperSLMModelInspection InspectFileOnWorker(const FString& Path, bool bGpuDeviceResidentHead, const FSuperSLMRuntimeConfig& Declared)
	{
		check(!IsInGameThread());
		const FString Key = TEXT("file:") + FileKeyFor(Path);
		FSuperSLMModelInspection Result;
		if (GInspectionCache.Find(Key, FSuperSLMArtifactBytesPin(), Result))
		{
			return Result;
		}
		const TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader.IsValid() || Reader->TotalSize() <= 0)
		{
			Result.Error = FString::Printf(TEXT("could not read %s"), *Path);
			return Result;
		}
		const int64 Size = Reader->TotalSize();
		uint8* Data = static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(Size), static_cast<uint32>(USuperSLMModel::CookedArtifactAlignment)));
		ON_SCOPE_EXIT { FMemory::Free(Data); };
		Reader->Serialize(Data, Size);
		if (Reader->IsError())
		{
			Result.Error = FString::Printf(TEXT("could not read %s"), *Path);
			return Result;
		}
		Result = SuperSLMModelInspector::InspectBytes(Data, Size, FString(), bGpuDeviceResidentHead, Declared);
		Result.ProvenanceJson = ProvenanceFromSections(Data, Size, Result);
		GInspectionCache.Add(Key, FSuperSLMArtifactBytesPin(), Result);
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// The action tier's shared runtime host. One CPU runtime for the module's lifetime: created the
	// first time a run needs it, kept configured for the last model between runs, released after
	// kIdleReleaseSeconds without a run, and torn down at engine pre-exit or module shutdown.
	//
	// It is NOT a standalone GameInstance. UGameInstance::InitializeStandalone registers an
	// EWorldType::Game world context, and a Game context that lives for the editor session is
	// unsafe. UEngine::GetCurrentPlayWorld returns the first Game context's world, so it would
	// return this host's dummy world outside PIE and ahead of the PIE world during a session.
	// UGameInstance::Init also binds the single-cast FNetDelegates encryption delegates. So the host
	// is a UGameInstance that is never initialized. It exists only as the Outer that the subsystem's
	// `Within = GameInstance` requires. The subsystem is constructed with NewObject and torn down with
	// Deinitialize(). It does not override Initialize(FSubsystemCollectionBase&), and it never reads
	// its GameInstance or its world, so it needs no collection. Being in none, it is also invisible
	// to the determinism self-check, which drives only subsystems their GameInstance knows.
	// ------------------------------------------------------------------------------------------

	struct FRunJob
	{
		FSuperSLMMCPModelSource Source;
		FString SchemaName;
		TArray<FSuperSLMMCPInput> Inputs;
		TUniqueFunction<void(FSuperSLMMCPConstrainedOutcome&&)> OnDone;
	};

	// A file run's first step, from the pool thread. It holds no UObject: the validated bytes
	// become a model on the game thread (FSuperSLMModelImport::CreateFromPrepared).
	struct FPrepared
	{
		bool bOk = false;
		FString Error;
		FString FileKey;
		bool bReuse = false; // the configured model is this file, unchanged
		FSuperSLMPreparedModelImport Import; // the validated bytes, when !bReuse
	};

	FString ImportError(const FString& Path, const FSuperSLMImportDiagnostic& Diagnostic)
	{
		return FString::Printf(TEXT("import of %s rejected (%s%s): %s"), *Path, *Diagnostic.StatusName,
			Diagnostic.SectionIndex != INDEX_NONE ? *FString::Printf(TEXT(", section %d"), Diagnostic.SectionIndex) : TEXT(""),
			*Diagnostic.Message);
	}

	FSuperSLMMCPConstrainedOutcome Cancelled()
	{
		FSuperSLMMCPConstrainedOutcome Outcome;
		Outcome.Error = TEXT("cancelled: the editor is shutting down");
		return Outcome;
	}

	// What the dedicated thread hands back: each input's generated tokens, in order. The text is
	// made from them on the game thread (SuperSLM::DetokenizeTokens is game-thread API).
	struct FWorkerOutcome
	{
		bool bOk = false;
		FString Error;
		TArray<TArray<int32>> Tokens;
	};

	// The dedicated thread: every input through the configured runtime, in order. It touches only
	// the subsystem it owns for the run (tokenize reads the runtime's own mapping); the schema was
	// resolved on the game thread, and nothing here reads the model object or its bytes.
	FWorkerOutcome InferOnWorker(USuperSLMSubsystem& Runtime, const FSuperSLMSchemaHandle& Schema, int32 LayerBudget,
		const TArray<FSuperSLMMCPInput>& Inputs, const std::atomic<bool>& bCancel)
	{
		FWorkerOutcome Out;
		Out.Tokens.Reserve(Inputs.Num());
		for (int32 I = 0; I < Inputs.Num(); ++I)
		{
			TArray<int32> PromptTokens;
			if (!Runtime.Tokenize(Inputs[I].Text, PromptTokens) || PromptTokens.Num() == 0)
			{
				Out.Error = FString::Printf(TEXT("input %d (%s) could not be tokenized (an empty input, or an artifact with no tokenizer); %d of %d inputs had finished"),
					I, *Inputs[I].RowName.ToString(), I, Inputs.Num());
				Out.Tokens.Reset();
				return Out;
			}
			TArray<int32> Tokens;
			FString InputError;
			if (!DriveOnePrompt(Runtime, Schema, LayerBudget, PromptTokens, bCancel, Tokens, InputError))
			{
				Out.Error = FString::Printf(TEXT("input %d (%s): %s; %d of %d inputs had finished"),
					I, *Inputs[I].RowName.ToString(), *InputError, I, Inputs.Num());
				Out.Tokens.Reset();
				return Out;
			}
			Out.Tokens.Add(MoveTemp(Tokens));
		}
		Out.bOk = true;
		return Out;
	}

	class FRuntimeHost : public TSharedFromThis<FRuntimeHost>
	{
	public:
		void Enqueue(FRunJob&& Job)
		{
			check(IsInGameThread());
			if (bShutDown)
			{
				Job.OnDone(Cancelled());
				return;
			}
			Queue.Add(MoveTemp(Job));
			EnsureTicker();
			StartNext();
		}

		bool Owns(const USuperSLMSubsystem* Subsystem) const
		{
			return Subsystem != nullptr && Cpu.Get() == Subsystem;
		}

		void Shutdown()
		{
			check(IsInGameThread());
			if (bShutDown)
			{
				return;
			}
			bShutDown = true;
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			bCancel.store(true, std::memory_order_relaxed);
			if (Current.IsSet())
			{
				// Bounded: the decode loop checks the flag between ticks. An import already under way
				// finishes first. A pending BeginConfigure is superseded by Deinitialize below and
				// never reports.
				if (Phase == EPhase::Preparing)
				{
					FPrepared Discarded = PrepareFuture.Consume();
				}
				else if (Phase == EPhase::Inferring)
				{
					FWorkerOutcome Discarded = InferFuture.Consume();
				}
				FRunJob Job = MoveTemp(*Current);
				Current.Reset();
				Phase = EPhase::Idle;
				Job.OnDone(Cancelled());
			}
			TArray<FRunJob> Pending = MoveTemp(Queue);
			for (FRunJob& Job : Pending)
			{
				Job.OnDone(Cancelled());
			}
			ReleaseRuntime();
			Cpu.Reset();
			Outer.Reset();
		}

	private:
		enum class EPhase : uint8 { Idle, Preparing, Configuring, Inferring };

		void EnsureTicker()
		{
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FRuntimeHost::Poll), 0.0f);
			}
		}

		bool EnsureCpu(FString& OutError)
		{
			if (Cpu.IsValid())
			{
				return true;
			}
			Outer.Reset(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
			Cpu.Reset(NewObject<USuperSLMSubsystem>(Outer.Get(), NAME_None, RF_Transient));
			if (!Cpu.IsValid())
			{
				OutError = TEXT("could not create the CPU runtime");
				Outer.Reset();
				return false;
			}
			return true;
		}

		// Game thread: the runtime's Layer-1 state and the model go; the objects stay for reuse.
		void ReleaseRuntime()
		{
			if (Cpu.IsValid() && UObjectInitialized())
			{
				Cpu->Deinitialize();
			}
			Model.Reset();
			ModelBytes.Reset();
			FileKey.Reset();
			LayerBudget = 0;
		}

		void StartNext()
		{
			if (Current.IsSet() || Queue.Num() == 0)
			{
				return;
			}
			Current.Emplace(MoveTemp(Queue[0]));
			Queue.RemoveAt(0);
			++JobSerial;
			LastUsedSeconds = FPlatformTime::Seconds();

			FString Error;
			if (!EnsureCpu(Error))
			{
				Finish(MakeError(Error));
				return;
			}
			const FSuperSLMMCPModelSource& Source = Current->Source;
			if (!Source.bIsFile)
			{
				// Reuse keys on the artifact, not the object: the same asset whose bytes are the very
				// bytes the runtime was configured from. A reimport replaces the asset's bytes (and
				// the host's own pin keeps the old ones from being freed, so their address cannot be
				// reused), so it reconfigures.
				if (Model.IsValid() && Model.Get() == Source.Asset.Get() && Source.Asset->PinArtifactBytes() == ModelBytes)
				{
					StartInference();
				}
				else
				{
					StartConfigure(Source.Asset, FString());
				}
				return;
			}

			// A file: its stat and, when it changed, its import run on the pool.
			Phase = EPhase::Preparing;
			const FString Path = Source.Path;
			const FString CurrentKey = Model.IsValid() ? FileKey : FString();
			PrepareFuture = Async(EAsyncExecution::ThreadPool, [Path, CurrentKey]() -> FPrepared
			{
				FPrepared Out;
				Out.FileKey = FileKeyFor(Path);
				if (!CurrentKey.IsEmpty() && Out.FileKey == CurrentKey)
				{
					Out.bReuse = true;
					Out.bOk = true;
					return Out;
				}
				// The heavy half of the import: the read, both validation passes and the aligned copy.
				// It touches no UObject, so it needs no GC guard.
				FSuperSLMImportDiagnostic Diagnostic;
				if (!FSuperSLMModelImport::PrepareFromFile(Path, Out.Import, Diagnostic))
				{
					Out.Error = ImportError(Path, Diagnostic);
					return Out;
				}
				Out.bOk = true;
				return Out;
			});
		}

		void OnPrepared(FPrepared&& Prepared)
		{
			if (!Prepared.bOk)
			{
				Finish(MakeError(Prepared.Error));
				return;
			}
			if (Prepared.bReuse && Model.IsValid())
			{
				StartInference();
				return;
			}
			// The short half of the import, on the game thread: the object's name, NewObject and the
			// hand-off of the prepared bytes. No GC guard is needed here, and none is held anywhere
			// in this module.
			FSuperSLMImportDiagnostic Diagnostic;
			const TStrongObjectPtr<USuperSLMModel> Imported(
				FSuperSLMModelImport::CreateFromPrepared(MoveTemp(Prepared.Import), Diagnostic));
			if (!Imported.IsValid())
			{
				Finish(MakeError(ImportError(Current->Source.Path, Diagnostic)));
				return;
			}
			StartConfigure(Imported, Prepared.FileKey);
		}

		// Game thread: BeginConfigure captures the bytes here and does the rest on the pool.
		void StartConfigure(const TStrongObjectPtr<USuperSLMModel>& NewModel, const FString& NewFileKey)
		{
			FSuperSLMModelShapeFacts Shape;
			if (!NewModel.IsValid() || !SuperSLMModelInspector::ReadShape(*NewModel, Shape) || Shape.NumHiddenLayers <= 0)
			{
				Finish(MakeError(TEXT("the model carries no usable Config (CFG1) section")));
				return;
			}
			// The runtime is rebuilt for the new model; until it reports, nothing is reusable.
			Model.Reset();
			ModelBytes.Reset();
			FileKey.Reset();
			Phase = EPhase::Configuring;
			PendingModel = NewModel;
			// Taken here, on the game thread, in the same frame BeginConfigure captures the bytes,
			// so it names the bytes the runtime maps.
			PendingModelBytes = NewModel->PinArtifactBytes();
			PendingFileKey = NewFileKey;
			PendingLayerBudget = Shape.NumHiddenLayers;
			const TWeakPtr<FRuntimeHost> Weak = AsShared();
			const uint32 Serial = JobSerial;
			Cpu->BeginConfigure(NewModel.Get(), AuthorTimeConfig(Shape.NumHiddenLayers),
				[Weak, Serial](const FSuperSLMConfigureReport& Report)
				{
					if (const TSharedPtr<FRuntimeHost> Host = Weak.Pin())
					{
						Host->OnConfigured(Serial, Report);
					}
				});
		}

		// Game thread, a later frame.
		void OnConfigured(uint32 Serial, const FSuperSLMConfigureReport& Report)
		{
			if (bShutDown || Serial != JobSerial || Phase != EPhase::Configuring)
			{
				return;
			}
			TStrongObjectPtr<USuperSLMModel> Configured = MoveTemp(PendingModel);
			FSuperSLMArtifactBytesPin ConfiguredBytes = MoveTemp(PendingModelBytes);
			if (Report.Result != ESuperSLMConfigureResult::Success)
			{
				Finish(MakeError(FString::Printf(TEXT("the runtime refused this model: %s"), *Report.Message)));
				return;
			}
			Model = MoveTemp(Configured);
			ModelBytes = MoveTemp(ConfiguredBytes);
			FileKey = PendingFileKey;
			LayerBudget = PendingLayerBudget;
			StartInference();
		}

		// Game thread. The schema is resolved here, before the hand-off: the lookup reads the
		// model's bytes, which is game-thread API. The runtime holds the model's mapping, so the
		// lookup reuses it rather than mapping again.
		void StartInference()
		{
			FSuperSLMSchemaHandle Schema;
			FString Error;
			if (!FSuperSLMSchemaLookup::LookupByName(*Model, Current->SchemaName, Schema, Error))
			{
				Finish(MakeError(Error));
				return;
			}
			Phase = EPhase::Inferring;
			USuperSLMSubsystem* Runtime = Cpu.Get();
			const int32 RunLayerBudget = LayerBudget;
			TArray<FSuperSLMMCPInput> Inputs = Current->Inputs;
			const std::atomic<bool>* Cancel = &bCancel;
			// A dedicated thread, not the pool: the decode loop sleeps and polls for up to minutes.
			// Runs are serial, so there is at most one. The host keeps the runtime alive until the
			// future is consumed (Poll or Shutdown).
			InferFuture = Async(EAsyncExecution::Thread,
				[Runtime, Schema, RunLayerBudget, Inputs = MoveTemp(Inputs), Cancel]()
				{
					return InferOnWorker(*Runtime, Schema, RunLayerBudget, Inputs, *Cancel);
				});
		}

		// Game thread, when the dedicated thread is done: the tokens become text here. The model's
		// bytes must still be the ones the run decoded against; a reimport during the run fails the
		// call by name rather than detokenizing with another artifact's vocabulary.
		void OnInferred(FWorkerOutcome&& Worker)
		{
			if (!Worker.bOk)
			{
				Finish(MakeError(Worker.Error));
				return;
			}
			if (!Model.IsValid() || Model->PinArtifactBytes() != ModelBytes)
			{
				ReleaseRuntime();
				Finish(MakeError(TEXT("the model was reimported while the run was in flight; nothing was written. Run the call again.")));
				return;
			}
			const TArray<FSuperSLMMCPInput>& Inputs = Current->Inputs;
			FSuperSLMMCPConstrainedOutcome Outcome;
			Outcome.Decoded.Reserve(Worker.Tokens.Num());
			for (int32 I = 0; I < Worker.Tokens.Num(); ++I)
			{
				FString Text;
				FString Error;
				if (!SuperSLM::DetokenizeTokens(*Model, Worker.Tokens[I], Text, Error))
				{
					Finish(MakeError(FString::Printf(TEXT("input %d (%s): detokenize failed: %s"), I,
						Inputs.IsValidIndex(I) ? *Inputs[I].RowName.ToString() : TEXT("?"), *Error)));
					return;
				}
				Outcome.Decoded.Add(MoveTemp(Text));
			}
			Outcome.bOk = true;
			Finish(MoveTemp(Outcome));
		}

		static FSuperSLMMCPConstrainedOutcome MakeError(const FString& Error)
		{
			FSuperSLMMCPConstrainedOutcome Outcome;
			Outcome.Error = Error;
			return Outcome;
		}

		void Finish(FSuperSLMMCPConstrainedOutcome&& Outcome)
		{
			FRunJob Job = MoveTemp(*Current);
			Current.Reset();
			Phase = EPhase::Idle;
			LastUsedSeconds = FPlatformTime::Seconds();
			Job.OnDone(MoveTemp(Outcome)); // may queue a run, and so start one
			StartNext();
		}

		// Game thread, once per frame from the core ticker. Never waits.
		bool Poll(float)
		{
			if (Current.IsSet())
			{
				if (Phase == EPhase::Preparing && PrepareFuture.IsReady())
				{
					OnPrepared(PrepareFuture.Consume());
				}
				else if (Phase == EPhase::Inferring && InferFuture.IsReady())
				{
					OnInferred(InferFuture.Consume());
				}
			}
			if (!Current.IsSet() && Queue.Num() == 0 && Model.IsValid() &&
				FPlatformTime::Seconds() - LastUsedSeconds > kIdleReleaseSeconds)
			{
				ReleaseRuntime();
			}
			if (!Current.IsSet() && Queue.Num() == 0 && !Model.IsValid())
			{
				TickerHandle.Reset();
				return false;
			}
			return true;
		}

		TStrongObjectPtr<UGameInstance> Outer;
		TStrongObjectPtr<USuperSLMSubsystem> Cpu;

		// What the runtime is configured with, when it is.
		TStrongObjectPtr<USuperSLMModel> Model;
		FSuperSLMArtifactBytesPin ModelBytes; // the bytes the runtime was configured from
		FString FileKey; // empty for an asset
		int32 LayerBudget = 0;

		// A BeginConfigure in flight.
		TStrongObjectPtr<USuperSLMModel> PendingModel;
		FSuperSLMArtifactBytesPin PendingModelBytes;
		FString PendingFileKey;
		int32 PendingLayerBudget = 0;

		TArray<FRunJob> Queue;
		TOptional<FRunJob> Current;
		EPhase Phase = EPhase::Idle;
		uint32 JobSerial = 0;
		TFuture<FPrepared> PrepareFuture;
		TFuture<FWorkerOutcome> InferFuture;
		double LastUsedSeconds = 0.0;

		FTSTicker::FDelegateHandle TickerHandle;
		std::atomic<bool> bCancel{false};
		bool bShutDown = false;
	};

	TSharedPtr<FRuntimeHost> GHost; // game thread only

	// Null after shutdown.
	FRuntimeHost* GetHost()
	{
		check(IsInGameThread());
		if (GShutDown)
		{
			return nullptr;
		}
		if (!GHost.IsValid())
		{
			GHost = MakeShared<FRuntimeHost>();
		}
		return GHost.Get();
	}
}

namespace SuperSLMMCPInference
{
	void ResolveObjectAsync(UClass* Class, const FString& ObjectPath,
		TUniqueFunction<void(UObject* Object, FString&& Error)> OnResolved)
	{
		check(IsInGameThread());
		if (ObjectPath.IsEmpty())
		{
			OnResolved(nullptr, TEXT("the object path is empty"));
			return;
		}
		if (UObject* Found = StaticFindObject(Class, nullptr, *ObjectPath))
		{
			OnResolved(Found, FString());
			return;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OnResolved(nullptr, FString::Printf(TEXT("%s is neither a file nor a valid object path"), *ObjectPath));
			return;
		}
		// The completion delegate is copied by the delegate system, so the move-only callback is
		// shared rather than captured by value. LoadPackageAsync calls it on the game thread.
		TSharedRef<TUniqueFunction<void(UObject*, FString&&)>> Callback =
			MakeShared<TUniqueFunction<void(UObject*, FString&&)>>(MoveTemp(OnResolved));
		TWeakObjectPtr<UClass> WeakClass(Class);
		LoadPackageAsync(PackageName, FLoadPackageAsyncDelegate::CreateLambda(
			[Callback, WeakClass, ObjectPath](const FName&, UPackage*, EAsyncLoadingResult::Type Result)
			{
				UObject* Loaded = (Result == EAsyncLoadingResult::Succeeded && WeakClass.IsValid())
					? StaticFindObject(WeakClass.Get(), nullptr, *ObjectPath)
					: nullptr;
				(*Callback)(Loaded, Loaded != nullptr ? FString()
					: FString::Printf(TEXT("no %s at %s"), WeakClass.IsValid() ? *WeakClass->GetName() : TEXT("object"), *ObjectPath));
			}));
	}

	void ResolveModelSourceAsync(const FString& ModelPath,
		TUniqueFunction<void(bool bOk, FSuperSLMMCPModelSource&& Source, FString&& Error)> OnResolved)
	{
		check(IsInGameThread());
		FSuperSLMMCPModelSource Source;
		Source.Path = ModelPath;
		if (ModelPath.IsEmpty())
		{
			OnResolved(false, MoveTemp(Source), TEXT("ModelPath is empty"));
			return;
		}
		if (FPaths::FileExists(ModelPath))
		{
			Source.bIsFile = true;
			OnResolved(true, MoveTemp(Source), FString());
			return;
		}
		ResolveObjectAsync(USuperSLMModel::StaticClass(), ModelPath,
			[ModelPath, OnResolved = MoveTemp(OnResolved)](UObject* Object, FString&& Error) mutable
			{
				FSuperSLMMCPModelSource Resolved;
				Resolved.Path = ModelPath;
				if (USuperSLMModel* Found = Cast<USuperSLMModel>(Object))
				{
					Resolved.Asset.Reset(Found);
					OnResolved(true, MoveTemp(Resolved), FString());
					return;
				}
				OnResolved(false, MoveTemp(Resolved),
					FString::Printf(TEXT("no .sslm file and no SuperSLM model asset at %s (%s)"), *ModelPath, *Error));
			});
	}

	bool RunInBackground(TUniqueFunction<void()> Work, TUniqueFunction<void()> OnGameThread)
	{
		FBackgroundQueue* Background = GetBackground();
		return Background != nullptr && Background->Add(MoveTemp(Work), MoveTemp(OnGameThread));
	}

	void InspectModelAsync(FSuperSLMMCPModelSource&& Source, TUniqueFunction<void(const FSuperSLMModelInspection&)> OnDone)
	{
		check(IsInGameThread());
		// The declared shape the inspection panel uses under default query-window settings.
		// MaxLayerBudget 0 makes InspectBytes use the model's own layer count, which is what
		// MakeCpuConfig sets from the model's shape.
		const FSuperSLMRuntimeConfig Declared =
			FSuperSLMEditorRuntimeHost::MakeCpuConfig(FSuperSLMModelShapeFacts(), FSuperSLMEditorHostSettings());

		// Seeded with the cancellation, which a task skipped at shutdown reports.
		TSharedRef<FSuperSLMModelInspection> Result = MakeShared<FSuperSLMModelInspection>();
		Result->Error = TEXT("cancelled: the editor is shutting down");
		TUniqueFunction<void()> Work;
		if (Source.bIsFile)
		{
			// What an import of this file would carry: the class default of the head switch.
			const bool bHead = GetDefault<USuperSLMModel>()->bGpuDeviceResidentHead;
			Work = [Result, Path = Source.Path, bHead, Declared]()
			{
				*Result = InspectFileOnWorker(Path, bHead, Declared);
			};
		}
		else
		{
			// Captured here, on the game thread. The pin keeps the bytes alive for the worker even if
			// the asset is reloaded or destroyed meanwhile; it is released with the task.
			USuperSLMModel& Asset = *Source.Asset;
			const FString Key = FString::Printf(TEXT("asset:%u"), GetTypeHash(FObjectKey(&Asset))) + TEXT("|") + Asset.GetPathName();
			const FSuperSLMArtifactBytesPin Pin = Asset.PinArtifactBytes();
			if (!Pin.IsValid())
			{
				FSuperSLMModelInspection Empty;
				Empty.Error = FString::Printf(TEXT("model %s carries no artifact bytes"), *Asset.GetPathName());
				OnDone(Empty);
				return;
			}
			const void* Data = Pin->GetData();
			const int64 Size = Pin->GetSize();
			Work = [Result, Pin, Key, Data, Size, Provenance = Asset.ProvenanceJson,
				bHead = Asset.bGpuDeviceResidentHead, Declared]()
			{
				if (!GInspectionCache.Find(Key, Pin, *Result))
				{
					*Result = SuperSLMModelInspector::InspectBytes(Data, Size, Provenance, bHead, Declared);
					GInspectionCache.Add(Key, Pin, *Result);
				}
			};
		}
		TSharedRef<TUniqueFunction<void(const FSuperSLMModelInspection&)>> Done =
			MakeShared<TUniqueFunction<void(const FSuperSLMModelInspection&)>>(MoveTemp(OnDone));
		if (!RunInBackground(MoveTemp(Work), [Result, Done]() { (*Done)(*Result); }))
		{
			FSuperSLMModelInspection Busy;
			Busy.Error = FString::Printf(TEXT("busy or shutting down: %d read-tier calls may wait at once; retry later"), kMaxQueuedBackgroundTasks);
			(*Done)(Busy);
		}
	}

	void RunConstrainedAsync(FSuperSLMMCPModelSource&& Source, const FString& SchemaName, TArray<FSuperSLMMCPInput>&& Inputs,
		TUniqueFunction<void(FSuperSLMMCPConstrainedOutcome&&)> OnDone)
	{
		FRunJob Job;
		Job.Source = MoveTemp(Source);
		Job.SchemaName = SchemaName;
		Job.Inputs = MoveTemp(Inputs);
		Job.OnDone = MoveTemp(OnDone);
		FRuntimeHost* Host = GetHost();
		if (Host == nullptr)
		{
			Job.OnDone(Cancelled());
			return;
		}
		Host->Enqueue(MoveTemp(Job));
	}

	bool IsHostRuntime(const USuperSLMSubsystem* Subsystem)
	{
		check(IsInGameThread());
		return GHost.IsValid() && GHost->Owns(Subsystem);
	}

	void ShutdownHost()
	{
		check(IsInGameThread());
		GShutDown = true;
		if (GBackground.IsValid())
		{
			GBackground->Drain();
			GBackground.Reset();
		}
		if (GHost.IsValid())
		{
			GHost->Shutdown();
			GHost.Reset();
		}
	}
}
