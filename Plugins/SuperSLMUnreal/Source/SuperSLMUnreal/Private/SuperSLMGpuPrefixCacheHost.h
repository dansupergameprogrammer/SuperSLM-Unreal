#pragma once

// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on. Its save and restore calls are preprocessed out
// unless SUPERSLM_WITH_GPU_PREFIX_SAVE is on as well. It defaults to 0: Layer 1 1.15.0 exports the GPU prefix
// save and restore verbs but does not release them (1.16.0 does), and defines no feature macro for them.
// Not yet installed by the GPU subsystem: see SuperSLMGpuSubsystemPrefixCache.h.
//
// The GPU subsystem's prefix cache instance (precache plan §5.3-§5.7, §5.11; step U6). It is the CPU
// cache host (SuperSLMPrefixCacheHost.h) with the GPU's Layer-1 half: the same host-tested core, the
// same pool-thread IO, disk serialisation, payload refs, inbox, cooked tier and test seams; only the
// four Layer-1 commands and teardown's releases are GPU calls:
//
//   Build    sslm_gpu_prefix_begin (root) or sslm_gpu_prefix_begin_from (nested: the parent's full
//            pages shared, budget = the suffix), sslm_gpu_prefix_set_schema (root, by the model's
//            schema index), sslm_gpu_prefix_prefill (prompt kind), sslm_gpu_prefix_freeze;
//   Restore  sslm_gpu_prefix_restore (1.15.0) of the 'SPX1' payload, with the parent as its sharing
//            hint (null for a root, and under the 8.12 seam);
//   Save     sslm_gpu_prefix_save (1.15.0), two-call sizing;
//   Release  sslm_gpu_prefix_release.
//
// Every GPU call runs on the GPU subsystem's submission thread, under its Layer-1 lock, through the
// Submit callback the subsystem installs; teardown's calls run there as well (RunOffGameThreadAndWait).
//
// Keying (§5.2, PC-5): the instance's engine identity is backend 1 with the adapter's PCI vendor and
// device ids, so a disk file or cooked entry written on one GPU never serves another: its key differs,
// and a blob that reached the restore anyway is refused SSLM_RESTORE_PRODUCER_MISMATCH (a miss).
//
// Nested entries need pooled sequences (GPU prefixes exist only in pooled mode); the subsystem sets
// FPrefixCacheConfig::bNestedEntries then, with RegionC its page ledger's region C.

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

// Win64 only, as the GPU backend is (SUPERSLMUNREAL_WITH_GPU, SuperSLMUnreal.Build.cs): elsewhere the
// Layer-1 GPU verbs this file calls are not compiled, so with the switch on it would not link.
#if SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU

#include "SuperSLMPrefixCacheHost.h"
#include "superslm/gpu_1p0.h"

#ifndef SUPERSLM_WITH_GPU_PREFIX_SAVE
#define SUPERSLM_WITH_GPU_PREFIX_SAVE 0
#endif

// What the GPU instance's Layer-1 calls need, captured at Configure (the subsystem's own handles; not
// owned). Read only on the submission thread.
struct FSuperSLMGpuPrefixLayer1
{
	SslmGpuContext* Ctx = nullptr;
	SslmGpuModelHandle* Model = nullptr;
	SslmGpuKvPoolHandle* Pool = nullptr;
	uint32 DispatchBudget = 0;   // sslm_gpu_prefix_prefill's dispatch budget for a whole chunk
	int32 MaxPrefillChunk = 64;  // tokens per prefill call
	// The model's schema table, name to index (sslm_gpu_prefix_set_schema's argument); -1 when the name
	// does not resolve. Captured by value at Configure; called on the submission thread.
	TFunction<int32(const FString& SchemaName)> SchemaIndexOf;
	// Runs Work on the submission thread under the Layer-1 lock and returns when it has run.
	TFunction<void(TFunction<void()> Work)> RunOnSubmissionThread;
	// The subsystem's Layer-1 status seam (NoteLayer1Status), called on the submission thread.
	TFunction<void(SslmGpuStatus)> NoteStatus;
};

class FSuperSLMGpuPrefixCacheHost : public FSuperSLMPrefixCacheHost
{
public:
	explicit FSuperSLMGpuPrefixCacheHost(int64* PrefixIdCounter) : FSuperSLMPrefixCacheHost(PrefixIdCounter) {}

	// The engine identity this instance keys by: the artifact and Layer-1 pin of the CPU instance's,
	// backend 1, and the adapter's vendor and device ids (ResolveAdapterIds).
	static SuperSLMCore::FPrefixEngineIdentity MakeGpuIdentity(const SuperSLMCore::FPrefixEngineIdentity& Base, uint32 AdapterVendorId, uint32 AdapterDeviceId);

	// The adapter ids for the key. 1.15.0 exposes no getter for the context's adapter: the subsystem's
	// RHI adapter ids stand in (GRHIVendorId, GRHIDeviceId), which name the same adapter when the engine
	// context was created on the RHI's adapter. If they ever differ the cost is misses, never a wrong
	// prefix: the engine's restore checks the blob's producer against its own context.
	static void ResolveAdapterIds(uint32& OutVendorId, uint32& OutDeviceId);

	// Configure()'s GPU half, before Configure(): the handles and callbacks the Layer-1 calls use.
	void SetLayer1(FSuperSLMGpuPrefixLayer1 InLayer1) { Layer1 = MoveTemp(InLayer1); }

protected:
	virtual FSuperSLMPrefixLaneJob MakeLaneJob(const SuperSLMCore::FPrefixCommand& Command) override;
	virtual bool ReleaseEngineHandleNow(uint64 Handle, int32& OutStatus) override;
	virtual void RunOffGameThreadAndWait(TFunction<void()> Work) override;

private:
	FSuperSLMGpuPrefixLayer1 Layer1;
};

#endif // SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU
