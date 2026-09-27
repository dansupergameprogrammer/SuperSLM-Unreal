#pragma once

#include "CoreMinimal.h"
#include "SuperSLMModelInspector.h"
#include "Templates/Function.h"
#include "UObject/StrongObjectPtr.h"

class USuperSLMModel;
class USuperSLMSubsystem;

// What a tool's ModelPath names. Both forms are accepted, and the file form is checked first:
//  - an `.sslm` file on disk (what the action-tier suite passes). The read tier inspects its bytes
//    without creating any object; the action tier imports it transiently and never saves it;
//  - an imported USuperSLMModel asset, by object path (for example /Game/Models/Example.Example).
struct FSuperSLMMCPModelSource
{
	FString Path;
	bool bIsFile = false;
	TStrongObjectPtr<USuperSLMModel> Asset; // set when !bIsFile
};

// One constrained inference to run: the text the model reads, and the output row it becomes.
struct FSuperSLMMCPInput
{
	FName RowName;
	FString Text;
};

// One constrained run's outcome, delivered on the game thread.
struct FSuperSLMMCPConstrainedOutcome
{
	bool bOk = false;
	FString Error;
	TArray<FString> Decoded; // input i's schema-constrained text, detokenized
};

namespace SuperSLMMCPInference
{
	// Game thread, non-blocking. A file resolves at once (FPaths::FileExists, one stat). An asset
	// already loaded resolves at once; otherwise its package is loaded with LoadPackageAsync and
	// OnResolved runs on the game thread when the load completes. OnResolved runs exactly once.
	void ResolveModelSourceAsync(const FString& ModelPath,
		TUniqueFunction<void(bool bOk, FSuperSLMMCPModelSource&& Source, FString&& Error)> OnResolved);

	// Game thread, non-blocking: an object of Class by object path, found or loaded the same way.
	// OnResolved runs on the game thread exactly once, with nullptr and a reason on failure.
	void ResolveObjectAsync(UClass* Class, const FString& ObjectPath,
		TUniqueFunction<void(UObject* Object, FString&& Error)> OnResolved);

	// Game thread, non-blocking: the model inspector's own readout
	// (SuperSLMModelInspector::InspectBytes) for Source. The declared shape is the one the editor's
	// inspection panel uses for a model under default query-window settings
	// (FSuperSLMEditorRuntimeHost::MakeCpuConfig), so the footprint figures agree with the panel.
	// A file's bytes are read into a private buffer; an asset's bytes are read in place, under a
	// PinArtifactBytes() pin taken on the game thread. Nothing
	// touches the runtime registry, and no UObject is created. The result is cached per model identity
	// (an asset by object and by the very bytes object it was read from, a file by path, size and
	// timestamp). It runs on the background queue, so it
	// can be refused when the queue is full. OnDone runs on the game thread exactly once.
	void InspectModelAsync(FSuperSLMMCPModelSource&& Source, TUniqueFunction<void(const FSuperSLMModelInspection&)> OnDone);

	// Game thread, non-blocking: the background queue. Tasks run one at a time, in order, on the
	// thread pool. OnGameThread runs afterwards on the game thread, from a core ticker that polls
	// once per frame, or from ShutdownHost, which waits for the running task. False, with nothing
	// queued, when kMaxQueuedBackgroundTasks tasks are already waiting, or once shutdown has begun.
	bool RunInBackground(TUniqueFunction<void()> Work, TUniqueFunction<void()> OnGameThread);

	// Game thread, non-blocking. Queues one constrained run over Inputs on the module's shared
	// runtime host and returns at once. Runs execute one at a time, in the order they were queued.
	// OnDone runs on the game thread exactly once. After ShutdownHost it runs at once, cancelled.
	// A run is all-or-nothing: the first input that fails fails the run, and the error names that
	// input and how many had finished.
	//
	// An asset's configured runtime is reused only while the asset still holds the bytes it was
	// configured from (a reimport reconfigures), and a reimport during a run fails the run.
	//
	// Threads, per run:
	//  - pool thread, a file only: the file stat for the reuse key and, when the model changed,
	//    FSuperSLMModelImport::PrepareFromFile (the read, both validation passes, the aligned copy;
	//    no UObject, so no GC guard). The runtime reuses the model, so a file is prepared once per
	//    model per session;
	//  - game thread, a file whose model changed: FSuperSLMModelImport::CreateFromPrepared, the
	//    short half (NewObject and the hand-off of the bytes). No GC guard is held anywhere in this
	//    module;
	//  - game thread, when the model changed: SuperSLMModelInspector::ReadShape, then
	//    USuperSLMSubsystem::BeginConfigure, whose heavy work runs on the pool and whose report
	//    arrives on the game thread in a later frame;
	//  - game thread: the schema lookup (it reads the model's bytes, which is game-thread API);
	//  - one dedicated thread: tokenize (on the runtime's own mapping) and the vend/Tick/poll decode
	//    loop, paced at one Tick per 1/60 s;
	//  - the subsystem's own worker lanes: every Layer-1 call;
	//  - game thread: polling once per frame for the end of each step; detokenize (game-thread
	//    API); OnDone.
	void RunConstrainedAsync(FSuperSLMMCPModelSource&& Source, const FString& SchemaName, TArray<FSuperSLMMCPInput>&& Inputs,
		TUniqueFunction<void(FSuperSLMMCPConstrainedOutcome&&)> OnDone);

	// Game thread. True for the shared host's own subsystem. Its state belongs to the host's
	// threads while a run is in flight, so no other code may read or drive it.
	bool IsHostRuntime(const USuperSLMSubsystem* Subsystem);

	// Game thread. Marks the module shut down, so no queue or host is created again and new calls
	// are refused. Unregisters the background queue's ticker, then waits for the running
	// background task and runs every continuation, failing the queued ones; work a continuation
	// tries to queue is refused. Cancels the run in flight and waits for its thread; the decode loop checks the
	// cancel flag between ticks. Fails every queued run, tears the runtime down, and releases the
	// host. Idempotent. The module calls it at engine pre-exit and at shutdown.
	void ShutdownHost();
}
