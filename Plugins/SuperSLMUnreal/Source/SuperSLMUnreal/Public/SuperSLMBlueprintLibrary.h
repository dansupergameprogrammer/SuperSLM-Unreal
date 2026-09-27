#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SuperSLMAdapterHandle.h"
#include "SuperSLMBlueprintTypes.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMBlueprintLibrary.generated.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate.

class USuperSLMModel;
class USuperSLMSubsystem;
struct FSuperSLMSelfCheckReport;

// L2-S3 (the plan §6, §10.4; Coverage Model §9, cell R-S3b;
// the red-suite record §4 "API choices made", item 1). The
// Blueprint entry points §6 names: "create, adopt-prefix, save and restore a sequence; submit a
// prefill; request a bounded decode step; read completion and sslm_stats; tokenize and
// detokenize". "adopt-prefix" (sslm_seq_adopt_prefix) was left open for the plan (the red-suite
// record, open question Q_ADOPT_PREFIX) -- no R-S3 cell names it, and USuperSLMSubsystem
// (SuperSLMSubsystem.h) exposes no prefix-handle mechanism for this suite to wrap.
//
// API CHOICE (recorded per the suite's convention, mirroring T-2805's own precedent): §6 states
// the Blueprint surface "follows the SuperFAISSUnreal subsystem pattern," and
// that plugin's subsystem hosts its own UFUNCTION(BlueprintCallable) methods directly.
// USuperSLMSubsystem cannot take that shape here without editing
// SuperSLMSubsystem.h, which this suite's scope forbade (other work was editing it
// concurrently). A UBlueprintFunctionLibrary wrapping the existing
// plain-C++ subsystem calls is the functionally equivalent, non-conflicting alternative: every
// wrapper below is a one-line forward to an ALREADY-DECLARED USuperSLMSubsystem method (or, for
// Detokenize, to the new SuperSLMDetokenizer.h primitive) plus a BP-type conversion --
// R-S3b's own claim ("Blueprint and inspection do not change results") is unaffected by which
// UE mechanism carries the reflection, because the underlying call and its result are
// identical either way. A later change may fold these onto USuperSLMSubsystem directly as
// UFUNCTIONs; this header is not a claim that it must stay a
// separate library.
//
// L2-S3 build note (2026-09-25): the adopt-prefix routing above is superseded by D-SLM7347 --
// shared prefixes are in the 1.0 Blueprint surface, and CreatePrefix/GetPrefixPhase/
// IsPrefixReady/AdoptPrefix/ReleasePrefix below wrap the L2-S1 C++ API (D-SLM7342). The
// lifecycle wrappers follow L2-S1's queued shape (D-SLM7421): Reset, Save, Restore and Adopt
// Prefix return ESuperSLMRestoreResultBP at once (Success means queued) plus a handle, and
// GetLifecycleOpResult()/GetSaveResult() read the drained result -- the sync-era bool/blob
// signatures this header first declared have no runtime to forward to since L2-S1's rework.
// Every wrapper stays a forward plus a type conversion, and runs on the game thread.

/** Blueprint nodes for the CPU backend: sequences, generation, lifecycle operations, shared prefixes and diagnostics. Game thread only. */
UCLASS()
class SUPERSLMUNREAL_API USuperSLMBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The CPU backend's subsystem for this world's GameInstance, or null. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM", meta = (WorldContext = "WorldContextObject"))
	static USuperSLMSubsystem* GetSuperSLMSubsystem(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMVendResultBP VendSequence(USuperSLMSubsystem* Subsystem, FSuperSLMSequenceBP& OutSequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static void ReturnSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMRestoreResultBP ResetSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
		FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static void SetLayerBudget(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence, int32 LayerBudget);

	// One Blueprint call in place of the C++ lookup-then-bind pair: FSuperSLMSchemaLookup::
	// LookupByName (SuperSLMSchemaHandle.h), then USuperSLMSubsystem::SetSchema.

	/** Binds the schema named SchemaName, compiled into Model, to Sequence. False, with OutError, if the name does not resolve or the bind is refused. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static bool SetSchemaByName(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
		const USuperSLMModel* Model, const FString& SchemaName, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static void RequestAdapterSwap(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence, int64 AdapterId);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static int64 GetActiveAdapter(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static bool BeginGeneration(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
		const FSuperSLMGenerationRequestBP& Request, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMSequencePhaseBP GetPhase(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static TArray<int32> GetGeneratedTokens(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	/** The outcome of the sequence's most recent decode step. On a Faulted sequence it names only a schema dead end or a released sequence, and reads Generating for every other fault; the fault's reason is in the log (LogSuperSLM, "Sequence N faulted: ..."). */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMDecodeOutcomeBP GetLastDecodeOutcome(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static FSuperSLMSequenceStatsBP GetStats(const USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static bool Tokenize(const USuperSLMSubsystem* Subsystem, const FString& Utf8Text, TArray<int32>& OutTokens);

	// §6: "the plugin owns sslm_detok_state per sequence." This wrapper detokenizes the WHOLE
	// token array supplied in one call (a fresh sslm_detok_state{0}, Layer 1's own documented
	// valid start state -- SuperSLMDetokenizer.h), which is sufficient for every §8/§9 claim
	// (the demonstrator's readout detokenizes a COMPLETE generated span, never a streaming
	// partial one) without USuperSLMSubsystem persisting per-sequence detok state -- an API
	// choice recorded here rather than a change to the subsystem, because it needs no new
	// subsystem-owned state at all.

	/** Turns a complete array of tokens into text with Model's tokenizer. Not for streaming a partial span. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static bool Detokenize(const USuperSLMModel* Model, const TArray<int32>& Tokens, FString& OutText);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static void Tick(USuperSLMSubsystem* Subsystem, float DeltaSeconds);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMRestoreResultBP SaveSequence(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
		FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMRestoreResultBP RestoreSequence(USuperSLMSubsystem* Subsystem, const TArray<uint8>& Blob,
		USuperSLMModel* ExpectedModel, FSuperSLMSequenceBP& OutSequence, FSuperSLMLifecycleOpHandleBP& OutHandle,
		FString& OutError);

	/** Pending until the queued Reset, Adopt Prefix or Restore has run, then its result. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMRestoreResultBP GetLifecycleOpResult(const USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle);

	/** Pending until the queued Save has run. The first Success read moves the saved bytes out; later reads return Consumed. */
	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static ESuperSLMRestoreResultBP GetSaveResult(USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle,
		TArray<uint8>& OutBlob);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM")
	static bool ReleaseLifecycleOpHandle(USuperSLMSubsystem* Subsystem, const FSuperSLMLifecycleOpHandleBP& Handle);

	// --- Shared prefixes (plan §6, D-SLM7347; R-S3b) ---

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Prefix")
	static bool CreatePrefix(USuperSLMSubsystem* Subsystem, const TArray<int32>& Tokens, FSuperSLMPrefixBP& OutPrefix, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Prefix")
	static ESuperSLMPrefixPhaseBP GetPrefixPhase(const USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Prefix")
	static bool IsPrefixReady(const USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Prefix")
	static ESuperSLMRestoreResultBP AdoptPrefix(USuperSLMSubsystem* Subsystem, const FSuperSLMSequenceBP& Sequence,
		const FSuperSLMPrefixBP& Prefix, FSuperSLMLifecycleOpHandleBP& OutHandle, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Prefix")
	static bool ReleasePrefix(USuperSLMSubsystem* Subsystem, const FSuperSLMPrefixBP& Prefix, FString& OutError);

	// --- Determinism self-check (plan §6, §7 item 11) ---

	// Review S3: the Blueprint route is the latent node USuperSLMSelfCheckAsyncAction
	// (SuperSLMSelfCheckAsyncAction.h), which steps the run once per frame and fires when it is
	// done. The blocking node that stood here ran the whole reference workload on the game thread
	// and is removed; C++ callers that may block (tests, commandlets) keep
	// SuperSLMDeterminismSelfCheck::Run().
	//
	// The per-backend verdicts of a finished run, as Blueprint reads them. A quarantined verdict
	// (FSuperSLMSelfCheckBackendResult::bQuarantined) reads NotYetRun here and its scope text says
	// it is quarantined -- recorded, never displayed (§7 item 11). Not a UFUNCTION.
	static FSuperSLMSelfCheckReportBP ToSelfCheckReportBP(const FSuperSLMSelfCheckReport& Report);

	// --- Diagnostics (§7 items 5, 7, 8 -- the live inspection panel's data source) ---

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static double GetLastTickDurationMs(const USuperSLMSubsystem* Subsystem);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static int32 GetHitchCount(const USuperSLMSubsystem* Subsystem);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static int32 GetPoolFreeCount(const USuperSLMSubsystem* Subsystem);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static int32 GetPoolOccupiedCount(const USuperSLMSubsystem* Subsystem);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static int64 GetKvPoolReservedBytes(const USuperSLMSubsystem* Subsystem);

	UFUNCTION(BlueprintCallable, Category = "SuperSLM|Diagnostics")
	static int64 GetWorkspaceReservedBytes(const USuperSLMSubsystem* Subsystem);
};
