// See SuperSLMGpuPrefixCacheHost.h.

#include "SuperSLMGpuPrefixCacheHost.h"

#if SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU

#include "RHIGlobals.h"
#include "SuperSLMLog.h"

using namespace SuperSLMCore;

namespace
{
	uint64 GpuHandleBits(SslmGpuPrefixHandle* Handle) { return static_cast<uint64>(reinterpret_cast<UPTRINT>(Handle)); }
	SslmGpuPrefixHandle* GpuHandleOf(uint64 Bits) { return reinterpret_cast<SslmGpuPrefixHandle*>(static_cast<UPTRINT>(Bits)); }

	// The core's reading of a GPU status (the CPU host's EngineResultOf, for the GPU's enumerators).
	EPrefixEngineResult GpuEngineResultOf(SslmGpuStatus Status)
	{
		switch (Status)
		{
		case SslmGpuStatus::SSLM_OK: return EPrefixEngineResult::Ok;
		case SslmGpuStatus::SSLM_GPU_KV_POOL_EXHAUSTED: return EPrefixEngineResult::PoolExhausted;
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
		case SslmGpuStatus::SSLM_RESTORE_PRODUCER_MISMATCH: // another adapter or engine version
		case SslmGpuStatus::SSLM_RESTORE_MODEL_MISMATCH:
		case SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH: // the restore's framing, digest and domain checks
			return EPrefixEngineResult::Refused;
#endif
		default: return EPrefixEngineResult::Failed;
		}
	}
}

FPrefixEngineIdentity FSuperSLMGpuPrefixCacheHost::MakeGpuIdentity(const FPrefixEngineIdentity& Base, uint32 AdapterVendorId, uint32 AdapterDeviceId)
{
	FPrefixEngineIdentity Identity = Base;
	Identity.Backend = 1;
	Identity.AdapterVendorId = AdapterVendorId;
	Identity.AdapterDeviceId = AdapterDeviceId;
	return Identity;
}

void FSuperSLMGpuPrefixCacheHost::ResolveAdapterIds(uint32& OutVendorId, uint32& OutDeviceId)
{
	OutVendorId = static_cast<uint32>(GRHIVendorId);
	OutDeviceId = static_cast<uint32>(GRHIDeviceId);
}

void FSuperSLMGpuPrefixCacheHost::RunOffGameThreadAndWait(TFunction<void()> Work)
{
	check(Layer1.RunOnSubmissionThread);
	Layer1.RunOnSubmissionThread(MoveTemp(Work));
}

bool FSuperSLMGpuPrefixCacheHost::ReleaseEngineHandleNow(uint64 Handle, int32& OutStatus)
{
	const SslmGpuStatus Status = sslm_gpu_prefix_release(Layer1.Ctx, GpuHandleOf(Handle));
	if (Layer1.NoteStatus)
	{
		Layer1.NoteStatus(Status);
	}
	OutStatus = static_cast<int32>(Status);
	return Status == SslmGpuStatus::SSLM_OK;
}

FSuperSLMPrefixLaneJob FSuperSLMGpuPrefixCacheHost::MakeLaneJob(const FPrefixCommand& Command)
{
	FSuperSLMPrefixLaneJob Job;
	Job.Completion = MakeShared<FPrefixCompletion, ESPMode::ThreadSafe>();
	Job.Completion->CommandId = Command.Id;
	Job.Completion->Generation = Command.Generation;
	const FSuperSLMGpuPrefixLayer1* L1 = &Layer1;
	auto Note = [L1](SslmGpuStatus Status)
	{
		if (L1->NoteStatus)
		{
			L1->NoteStatus(Status);
		}
	};
	switch (Command.Kind)
	{
	case EPrefixCommandKind::Build:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Build;
		const TArray<int32> Tokens(Command.Spec.Tokens.data(), static_cast<int32>(Command.Spec.Tokens.size()));
		const FString Schema = UTF8_TO_TCHAR(Command.Spec.SchemaName.c_str());
		const int32 Budget = FMath::Max(Command.ContextLength, 1); // nested: the suffix's budget
		const uint64 Parent = Command.ParentEngineHandle;
		Job.Run = [this, L1, Note, Tokens, Schema, Budget, Parent](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			SslmGpuPrefixHandle* Prefix = nullptr;
			// A nested build continues its parent, which the core keeps cached and pinned until this
			// job's completion is applied; a child binds no schema of its own (it carries its parent's).
			SslmGpuStatus Status = Parent != 0
				? sslm_gpu_prefix_begin_from(L1->Ctx, GpuHandleOf(Parent), Budget, &Prefix)
				: sslm_gpu_prefix_begin(L1->Ctx, L1->Model, L1->Pool, Budget, &Prefix);
			Note(Status);
			if (Status == SslmGpuStatus::SSLM_OK && Parent == 0 && !Schema.IsEmpty())
			{
				const int32 Index = L1->SchemaIndexOf ? L1->SchemaIndexOf(Schema) : -1;
				if (Index < 0)
				{
					Note(sslm_gpu_prefix_release(L1->Ctx, Prefix));
					Out->Engine = EPrefixEngineResult::UnknownSchema;
					Out->EngineStatus = -1; // resolved by name before any engine call that binds
					return;
				}
				Status = sslm_gpu_prefix_set_schema(L1->Ctx, Prefix, Index);
				Note(Status);
			}
			int32 Done = 0;
			int32 PrefillCalls = 0;
			while (Status == SslmGpuStatus::SSLM_OK && Done < Tokens.Num())
			{
				const int32 Count = FMath::Min(Tokens.Num() - Done, L1->MaxPrefillChunk);
				int32_t Consumed = 0;
				Status = sslm_gpu_prefix_prefill(L1->Ctx, Prefix, Tokens.GetData() + Done, Count, /*prompt*/ 0, L1->DispatchBudget, &Consumed);
				++PrefillCalls;
				Note(Status);
				Done += Consumed;
				if (Status == SslmGpuStatus::SSLM_OK && Consumed <= 0)
				{
					Status = SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED; // no progress: never loop
				}
			}
			NotePrefixPrefills(PrefillCalls); // §15.26
			if (Status == SslmGpuStatus::SSLM_OK)
			{
				Status = sslm_gpu_prefix_freeze(L1->Ctx, Prefix);
				Note(Status);
			}
			if (Status != SslmGpuStatus::SSLM_OK && Prefix != nullptr)
			{
				Note(sslm_gpu_prefix_release(L1->Ctx, Prefix));
				Prefix = nullptr;
			}
			Out->Engine = GpuEngineResultOf(Status);
			if (Out->Engine == EPrefixEngineResult::Refused)
			{
				Out->Engine = EPrefixEngineResult::Failed; // a build's refusal is not a miss
			}
			Out->EngineStatus = static_cast<int32_t>(Status);
			Out->EngineHandle = GpuHandleBits(Prefix);
		};
		break;
	}
	case EPrefixCommandKind::Restore:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Restore;
		const uint64 Ref = Command.PayloadRef;
		Job.PayloadBytes = PayloadBytesOf(Ref);
		uint64 Hint = Command.ParentEngineHandle; // never a requirement: the core charged ceil(L/B)
#if WITH_DEV_AUTOMATION_TESTS
		if (Test.bNullRestoreHints)
		{
			Hint = 0; // cell 8.12's GPU twin: the hint fails
		}
#endif
		Job.Run = [this, L1, Note, Ref, Hint](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			TArray<uint8> Bytes;
			const bool bHave = TakePayloadCopy(Ref, Bytes);
			DropPayload(Ref); // the Restore owns the bytes
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
			SslmGpuPrefixHandle* Prefix = nullptr;
			uint32_t Shared = 0;
			const SslmGpuStatus Status = bHave
				? sslm_gpu_prefix_restore(L1->Ctx, L1->Model, L1->Pool, Bytes.GetData(), static_cast<size_t>(Bytes.Num()), GpuHandleOf(Hint), &Prefix, &Shared)
				: SslmGpuStatus::SSLM_SEQUENCE_KV_BUFFER_MISMATCH;
			Note(Status);
			Out->Engine = GpuEngineResultOf(Status);
			Out->EngineStatus = static_cast<int32_t>(Status);
			Out->EngineHandle = Status == SslmGpuStatus::SSLM_OK ? GpuHandleBits(Prefix) : 0;
			Out->SharedPages = Shared;
#else
			// Without SUPERSLM_WITH_GPU_PREFIX_SAVE there is no GPU prefix restore: every restore fails,
			// and the core falls through to recompute.
			(void)L1;
			(void)Note;
			(void)bHave;
			(void)Hint;
			Out->Engine = EPrefixEngineResult::Failed;
			Out->EngineStatus = -1; // no engine call was made
#endif
		};
		break;
	}
	case EPrefixCommandKind::Save:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Save;
		const uint64 Engine = Command.EngineHandle;
		Job.Run = [this, L1, Note, Engine](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
#if SUPERSLM_WITH_GPU_PREFIX_SAVE
			size_t Size = 0;
			SslmGpuStatus Status = sslm_gpu_prefix_save(L1->Ctx, GpuHandleOf(Engine), nullptr, &Size);
			TArray<uint8> Bytes;
			if (Status == SslmGpuStatus::SSLM_OUTPUT_BUFFER_TOO_SMALL)
			{
				Bytes.SetNumUninitialized(static_cast<int32>(Size));
				Status = sslm_gpu_prefix_save(L1->Ctx, GpuHandleOf(Engine), Bytes.GetData(), &Size);
				Bytes.SetNum(static_cast<int32>(Size));
			}
			Note(Status);
			Out->Engine = GpuEngineResultOf(Status);
			Out->EngineStatus = static_cast<int32_t>(Status);
			if (Status == SslmGpuStatus::SSLM_OK)
			{
				Out->PayloadRef = StorePayload(MoveTemp(Bytes));
			}
#else
			(void)L1;
			(void)Note;
			(void)Engine;
			Out->Engine = EPrefixEngineResult::Failed; // no write-back without the 1.15.0 save verb
			Out->EngineStatus = -1;
#endif
		};
		break;
	}
	case EPrefixCommandKind::Release:
	{
		Job.Kind = ESuperSLMPrefixLaneKind::Release;
		const uint64 Engine = Command.EngineHandle;
		Job.ReleasedHandle = Engine;
		Job.Run = [this, L1, Note, Engine](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			const SslmGpuStatus Status = sslm_gpu_prefix_release(L1->Ctx, GpuHandleOf(Engine));
			Note(Status);
			// §5.4 *What a release's status means* (rev 1.18.1): SSLM_BUSY alone leaves the handle untouched (Busy: the
			// core issues it again); every other status consumed the handle, so the release has applied.
			Out->Engine = Status == SslmGpuStatus::SSLM_OK ? EPrefixEngineResult::Ok
				: (Status == SslmGpuStatus::SSLM_BUSY ? EPrefixEngineResult::Busy : EPrefixEngineResult::Failed);
			Out->EngineStatus = static_cast<int32_t>(Status); // SSLM_OK reads 0 on both backends
		};
		break;
	}
	default:
		checkNoEntry();
	}
	return Job;
}

#endif // SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU
