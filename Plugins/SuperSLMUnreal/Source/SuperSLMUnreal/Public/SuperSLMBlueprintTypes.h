#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSlotGates.h"
#include "SuperSLMBlueprintTypes.generated.h"

// L2-S3's slot gate is retired (SuperSLMSlotGates.h): the slot is built, and UnrealHeaderTool
// rejects a reflected type inside an #if it does not know, so its files carry no gate.

// L2-S3 (the plan §6, §10.4; Coverage Model §9, cell R-S3b;
// the red-suite record). Blueprint-reflected mirrors of the L2-S1
// plain-C++ types (SuperSLMSequenceTypes.h, SuperSLMSaveRestoreTypes.h) -- those headers
// declare no UPROPERTY/UENUM(BlueprintType) by design (their own header comment: "§10.4 places
// the Blueprint surface at L2-S3 ... Reflection is added when that surface is actually built,
// not speculatively here"). This is that surface's own type layer, arriving now that L2-S3 is
// under work; the C++-only originals are left untouched (this layer edits no existing
// file).
//
// §6: "Decode status is an enum, never a sentinel number: Generating (-1), Schema Dead End
// (-2), Sequence No Longer Valid (-3)" -- ESuperSLMDecodeOutcomeBP mirrors
// ESuperSLMDecodeOutcome (SuperSLMSequenceTypes.h) ordinal-for-ordinal, so the two convert with
// a plain static_cast rather than a switch a future enumerator could silently miss.

/** What the most recent decode step of a sequence produced. */
UENUM(BlueprintType)
enum class ESuperSLMDecodeOutcomeBP : uint8
{
	/** The step finished a token. */
	TokenProduced = 0,
	/** The step ran part of a token; the token is not finished yet. On a Faulted sequence it names no cause: the fault's reason is in the log (LogSuperSLM, "Sequence N faulted: ..."). */
	Generating = 1,
	/** The bound schema allows no next token, so the sequence stopped (Schema Dead End). */
	SchemaDeadEnd = 2,
	/** The sequence is no longer live: it was released while the step was queued. */
	SequenceNoLongerValid = 3,
};

/** Where a sequence is in its generation. */
UENUM(BlueprintType)
enum class ESuperSLMSequencePhaseBP : uint8
{
	Idle,
	Prefilling,
	Decoding,
	Complete,
	Faulted,
};

/** What a generation request's tokens are: a prompt, or content for the bound schema. */
UENUM(BlueprintType)
enum class ESuperSLMSpanKindBP : uint8
{
	Prompt = 0,
	SchemaContent = 1,
};

/** The result of asking a backend for a sequence. Mirrors ESuperSLMVendResult. */
UENUM(BlueprintType)
enum class ESuperSLMVendResultBP : uint8
{
	Success,
	PoolExhausted,
	NotConfigured,
};

// Mirrors ESuperSLMBackend (SuperSLMSaveRestoreTypes.h) -- also the demonstrator's own Backend
// control (§8).

/** The inference backend: CPU or GPU. */
UENUM(BlueprintType)
enum class ESuperSLMBackendBP : uint8
{
	CPU = 0,
	GPU = 1,
};

// Mirrors ESuperSLMRestoreResult (SuperSLMSaveRestoreTypes.h).
//
// L2-S3 build (plan §6, §10.4; D-SLM7421, D-SLM7423): the full runtime enum, ordinal for
// ordinal -- NotConfigured, PoolExhausted, AdapterUnavailable and SequenceQueueFull as §10.4
// names, and every later value the runtime appended (Pending through OutOfMemory), because this is
// the one result type Restore, Save, Reset and Adopt Prefix all return and a queued call's read
// reports Pending. SuperSLMBlueprintLibrary.cpp static_asserts each ordinal against the runtime
// enum, so the two convert with a static_cast.

/**
 * The result of a queued lifecycle operation (Reset, Save, Restore, Adopt Prefix): Success, a
 * refusal by name, or Pending while it is still queued.
 */
UENUM(BlueprintType)
enum class ESuperSLMRestoreResultBP : uint8
{
	Success,
	BackendMismatch,
	ModelMismatch,
	KvMismatch,
	ResidualLost,
	Malformed,
	NotConfigured,
	PoolExhausted,
	AdapterUnavailable,
	SequenceQueueFull,
	Pending,
	UnsupportedOnGpu,
	SaveRefused,
	Layer1Mismatch,
	Consumed,
	ResetRequired, // Mirrors the C++ GPU result; no Blueprint node returns it: a GPU generation queued behind a reset Layer 1 refused, at its turn
	OutOfMemory,   // Mirrors the C++ GPU result; no Blueprint node returns it: a GPU restore ran out of GPU memory
};

// Mirrors ESuperSLMPrefixPhase (SuperSLMSequenceTypes.h) -- the shared-prefix wrappers (D-SLM7347).

/** Where a shared prefix is in its preparation. */
UENUM(BlueprintType)
enum class ESuperSLMPrefixPhaseBP : uint8
{
	Pending,
	Prefilling,
	Ready,
	Faulted,
	Invalid,
};

// §6: "The self-check verdict is Blueprint-readable as Verified / Diverged / Not Yet Run per
// backend, with the scope string of §7 item 11. No Blueprint text states GPU determinism
// unconditionally." (item 11's own verdict enum, plan §7).

/** A backend's determinism self-check verdict: Verified, Diverged, or Not Yet Run. */
UENUM(BlueprintType)
enum class ESuperSLMSelfCheckVerdictBP : uint8
{
	Verified,
	Diverged,
	NotYetRun,
};

/**
 * One sequence a backend has handed out. Wraps the sequence's opaque Id; a default value (Id 0)
 * is always invalid.
 */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMSequenceBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 Id = 0;

	FSuperSLMSequenceBP() = default;
	explicit FSuperSLMSequenceBP(const FSuperSLMSequence& Native) : Id(Native.Id) {}
	FSuperSLMSequence ToNative() const { FSuperSLMSequence S; S.Id = Id; return S; }

	bool IsValid() const { return Id != 0; }
};

/** A generation request: the prompt tokens, how many tokens to generate, and what ends it. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMGenerationRequestBP
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM")
	TArray<int32> PromptTokens;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM")
	int32 MaxNewTokens = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM")
	ESuperSLMSpanKindBP SpanKind = ESuperSLMSpanKindBP::Prompt;

	// L2-S3 build: the native request's stop set (plan §8: the game supplies {151645, 151643} for
	// A-EX, because the artifact records none).

	/**
	 * Token ids that end generation. The model file records none, so the game supplies them: for
	 * Qwen2.5-Instruct, 151645 and 151643. With a schema bound they never end it, because the schema
	 * excludes every special id: generation ends when the schema allows no next token, which is
	 * reported as Schema Dead End whether the output is complete there or not, or at MaxNewTokens.
	 * Empty, with no schema bound: generation stops only at MaxNewTokens.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperSLM")
	TArray<int32> StopTokenIds;
};

// L2-S3 build (plan §6, D-SLM7421).

/**
 * The handle a queued lifecycle operation returns (Reset, Save, Restore, Adopt Prefix). Read its
 * result with Get Lifecycle Op Result or Get Save Result. Id 0 is invalid.
 */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMLifecycleOpHandleBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 Id = 0;

	FSuperSLMLifecycleOpHandleBP() = default;
	explicit FSuperSLMLifecycleOpHandleBP(const FSuperSLMLifecycleOpHandle& Native) : Id(Native.Id) {}
	FSuperSLMLifecycleOpHandle ToNative() const { FSuperSLMLifecycleOpHandle H; H.Id = Id; return H; }

	bool IsValid() const { return Id != 0; }
};

// L2-S3 build (plan §6, D-SLM7347).

/** A shared prefix: a prompt prepared once that several sequences can adopt. Id 0 is invalid. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMPrefixBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 Id = 0;

	FSuperSLMPrefixBP() = default;
	explicit FSuperSLMPrefixBP(const FSuperSLMPrefix& Native) : Id(Native.Id) {}
	FSuperSLMPrefix ToNative() const { FSuperSLMPrefix P; P.Id = Id; return P; }

	bool IsValid() const { return Id != 0; }
};

/** A sequence's runtime statistics, as the inference runtime reports them. */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMSequenceStatsBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 DecodeStepCeiling = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 DecodeStepActual = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int64 ForcedTokenCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	int32 KvBlocksResident = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	bool bSchemaAccepting = false;
};

// §7 item 11's per-backend verdict, Blueprint-readable (§6). ScopeText carries "GPU tokens
// identical to the CPU reference on the reference workload (32 steps x 2 granularities);
// final-logit digest unavailable at Layer 1 v1.5.0" verbatim -- the report's own words, never
// re-summarized by this struct, so a scope change in the self-check (plan §7 item 11) cannot
// silently drift from what the demonstrator displays (§8: "shows its verdict and scope beside
// the digest").

/**
 * The determinism self-check's verdict for each backend, and the report's own scope text. In
 * 1.0 the GPU verdict is always withheld and reads Not Yet Run.
 */
USTRUCT(BlueprintType)
struct SUPERSLMUNREAL_API FSuperSLMSelfCheckReportBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	ESuperSLMSelfCheckVerdictBP CpuVerdict = ESuperSLMSelfCheckVerdictBP::NotYetRun;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	ESuperSLMSelfCheckVerdictBP GpuVerdict = ESuperSLMSelfCheckVerdictBP::NotYetRun;

	UPROPERTY(BlueprintReadOnly, Category = "SuperSLM")
	FString ScopeText;
};
