#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

#include "SuperSLMModel.h"

#include "superslm/sslm_abi.h"

class USuperSLMModel;

// L2-S1 (the plan §4, §5, §10.2). The module's one owner of Layer-1
// handles that outlive a single subsystem call: model mappings, mapped adapters, and resolved
// schema names. Game-thread API; every entry point takes the registry lock, so a stray call
// from another thread is serialized rather than racing.
//
// Why a shared mapping rather than one sslm_model per consumer: Layer 1 binds an adapter to the
// exact sslm_model handle it was mapped against (sslm_seq_set_adapter refuses
// `adapter->base != seq->model`, src/sslm_abi.cpp), and a compiled schema handle carries its
// model the same way (sslm_seq_set_schema). An adapter imported through
// FSuperSLMAdapterImport and a subsystem configured with the same USuperSLMModel must therefore
// see ONE sslm_model, or the swap is refused as a model mismatch. Mapping is also the costly step
// (a whole-file SHA-256 plus a copy of the artifact into the view, model.h "Ownership"), so a
// second consumer of an already-mapped model pays nothing.
namespace SuperSLMRuntime
{
	// --- A model's bytes, captured for a load that runs off the game thread ---

	// Fold-round ruling 1 (Configure() must not block the editor): what a load reads from a
	// USuperSLMModel, captured ON THE GAME THREAD so the load itself can run on a worker without
	// touching the UObject. The capturer keeps the model alive -- a strong reference created and
	// released on the game thread -- for as long as any worker reads Data, and Pin keeps Data
	// itself alive across a reload or destruction of the asset meanwhile (review S5: the object
	// alone is not enough, since Serialize() replaces its bytes in place). The FModelBytes entry
	// points below never dereference Owner: a mapping is matched by key and by weak-pointer
	// identity (index and serial number), so they are safe to call from a worker.
	struct FModelBytes
	{
		const USuperSLMModel* Key = nullptr;
		TWeakObjectPtr<const USuperSLMModel> Owner;
		const uint8* Data = nullptr;
		int64 Size = 0;
		FSuperSLMArtifactBytesPin Pin; // owns what Data points at
		FString Name;
		bool bGpuDeviceResidentHead = true;
	};
	FModelBytes CaptureModelBytes(const USuperSLMModel& Model); // game thread for an off-thread load

	// --- Artifact header facts, read from a USuperSLMModel's already-validated bytes ---

	// The 32-byte integrity hash at header offset 32 (docs/sslm_format.md, Header): SHA-256 of
	// the file with those bytes zeroed, and the artifact's content-addressed identity. Import
	// already verified it (SslmArtifact::OpenFromMemory), so this is a read, not a re-hash.
	bool ReadArtifactHash(const USuperSLMModel& Model, uint8 OutHash[32]);
	bool ReadArtifactHash(const FModelBytes& Model, uint8 OutHash[32]);
	FString HashToHex(const uint8 Hash[32]);

	// The CFG1 Config section parsed by Layer 1's own superslm::ParseConfig, located through
	// the section table (docs/sslm_format.md, Section table). Only the fields the scheduler
	// reads are surfaced.
	struct FModelShape
	{
		int32 NumHiddenLayers = 0;
		int64 ContextCap = 0;
		int32 VocabSize = 0;
	};
	bool ReadModelShape(const USuperSLMModel& Model, FModelShape& OutShape, FString& OutError);
	bool ReadModelShape(const FModelBytes& Model, FModelShape& OutShape, FString& OutError);

	// --- Model mappings (refcounted, one sslm_model per USuperSLMModel) ---

	// Returns the model's shared mapping, mapping it on first use. Each successful call must be
	// balanced by one ReleaseModelMapping on the returned handle.
	sslm_model AcquireModelMapping(const USuperSLMModel& Model, FString& OutError);
	// The same, from captured bytes; callable from a worker (the mapping itself -- the whole-file
	// SHA-256 and the copy -- then runs there, outside the registry lock; only the lookup and the
	// publish take it, and a concurrent duplicate map is unmapped in favour of the first).
	sslm_model AcquireModelMapping(const FModelBytes& Model, FString& OutError);

	// Drops one reference. The last reference unmaps; a refusal from sslm_model_unmap (a
	// sequence still live against it) is logged by status name and the mapping is kept, so the
	// handle is never freed under a live sequence.
	void ReleaseModelMapping(sslm_model Mapping);

	// --- Adapters ---

	struct FResolvedAdapter
	{
		sslm_adapter Adapter = nullptr;
		sslm_model Base = nullptr;
		uint8 ArtifactHash[32] = {}; // the adapter file's own header integrity hash
	};

	// The adapter's identity across sessions (D-SLM7341): a save records this hash, and a
	// restore finds whichever registered adapter carries it, mapped against Base. 0 if none.
	int64 FindAdapterByHash(const uint8 ArtifactHash[32], sslm_model Base);

	// Maps the adapter file against Base's shared mapping and returns a nonzero id.
	int64 RegisterAdapter(const FString& AbsolutePath, const USuperSLMModel& Base, FString& OutError);
	bool ResolveAdapter(int64 AdapterId, FResolvedAdapter& OutAdapter);

	// Refused by name while the adapter is pinned (below) or while Layer 1 still has a sequence
	// bound to it (SSLM_ADAPTER_HAS_LIVE_SEQUENCES).
	bool UnregisterAdapter(int64 AdapterId, FString& OutError);

	// Plan §2.5 row 20 rule 4 (T-2987 L2): a held adapter request, a CPU restore from its dispatch
	// to its delivery, and a worker job that carries an adapter each keep the adapter's import
	// alive, because the handle is resolved on the game thread and used later on a worker. A pin
	// resolves the adapter and counts one reference; UnregisterAdapter refuses while any is held.
	// Each successful PinAdapter is balanced by exactly one UnpinAdapter. Game-thread API.
	bool PinAdapter(int64 AdapterId, FResolvedAdapter& OutAdapter);
	void UnpinAdapter(int64 AdapterId);

	// --- Schema names ---

	// A schema handle names a schema by (artifact hash, schema name), never by an sslm_schema
	// pointer: the pointer is only valid against the one sslm_model it was looked up on, and a
	// subsystem re-configured later may hold a different mapping of the same artifact.
	uint64 RegisterSchemaName(const uint8 ArtifactHash[32], const FString& SchemaName);
	bool ResolveSchemaName(uint64 SchemaId, uint8 OutArtifactHash[32], FString& OutSchemaName);

	// Module shutdown: releases every adapter, then every mapping still held.
	void ShutdownRegistry();
}
