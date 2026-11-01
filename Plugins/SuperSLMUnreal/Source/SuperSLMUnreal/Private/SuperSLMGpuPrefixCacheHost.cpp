// See SuperSLMGpuPrefixCacheHost.h.

#include "SuperSLMGpuPrefixCacheHost.h"

#if SUPERSLM_WITH_PREFIX_CACHE && SUPERSLMUNREAL_WITH_GPU

#include "RHIGlobals.h"
#include "HAL/PlatformTime.h"
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

void FSuperSLMGpuPrefixCacheHost::ResolveAdapterIds(bool bLayer1Known, uint32 Layer1VendorId, uint32 Layer1DeviceId, uint32& OutVendorId, uint32& OutDeviceId)
{
	if (bLayer1Known)
	{
		OutVendorId = Layer1VendorId;
		OutDeviceId = Layer1DeviceId;
		return;
	}
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
		// Precache U6, Q7: the build runs in steps, each one submission-thread job, so no single job nears the row-21
		// device-loss bound and frame-path jobs run between them. The first step begins (and binds the schema); every step
		// prefills in MaxPrefillChunk calls until the build's tokens are in or the step's slice is spent; the step that
		// prefills the last token freezes. Only the finishing step (or an abort) writes the completion.
		struct FSteps
		{
			TArray<int32> Tokens;
			FString Schema;
			int32 Budget = 1;
			uint64 Parent = 0;
			SslmGpuPrefixHandle* Prefix = nullptr;
			bool bBegun = false;
			std::atomic<bool> bFinished{false};
			int32 Done = 0;
			int32 PrefillCalls = 0;
			uint64 EntryId = 0;
			int32 StepsRun = 0;      // steps that prefilled (the GPU's chunk jobs, cells 1.7 (c), 5.13)
			bool bHeldCounted = false; // counted in Test.HeldBuilds while the build hold keeps it
		};
		TSharedRef<FSteps, ESPMode::ThreadSafe> Steps = MakeShared<FSteps, ESPMode::ThreadSafe>();
		Steps->Tokens = TArray<int32>(Command.Spec.Tokens.data(), static_cast<int32>(Command.Spec.Tokens.size()));
		Steps->Schema = UTF8_TO_TCHAR(Command.Spec.SchemaName.c_str());
		Steps->Budget = FMath::Max(Command.ContextLength, 1); // nested: the suffix's budget
		Steps->Parent = Command.ParentEngineHandle;
		Steps->EntryId = Command.EntryId;
#if WITH_DEV_AUTOMATION_TESTS
		NoteBuildState(Steps->EntryId, ESuperSLMPrefixBuildState::Queued); // emitted: the core has charged it
#endif
		Job.Run = [this, L1, Note, Steps](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
			FSteps& St = *Steps;
			SslmGpuStatus Status = SslmGpuStatus::SSLM_OK;
			if (!St.bBegun)
			{
				St.bBegun = true;
				// A nested build continues its parent, which the core keeps cached and pinned until this build's
				// completion is applied; a child binds no schema of its own (it carries its parent's).
#if WITH_DEV_AUTOMATION_TESTS
				const int32 Forced = Test.GpuNextBuildBeginStatus.exchange(0);
				if (Forced != 0)
				{
					Status = static_cast<SslmGpuStatus>(Forced); // the begin is not called: nothing drawn
				}
				else
#endif
				Status = St.Parent != 0
					? sslm_gpu_prefix_begin_from(L1->Ctx, GpuHandleOf(St.Parent), St.Budget, &St.Prefix)
					: sslm_gpu_prefix_begin(L1->Ctx, L1->Model, L1->Pool, St.Budget, &St.Prefix);
				Note(Status);
				if (Status == SslmGpuStatus::SSLM_OK && St.Parent == 0 && !St.Schema.IsEmpty())
				{
					const int32 Index = L1->SchemaIndexOf ? L1->SchemaIndexOf(St.Schema) : -1;
					if (Index < 0)
					{
						Note(sslm_gpu_prefix_release(L1->Ctx, St.Prefix));
						St.Prefix = nullptr;
						Out->Engine = EPrefixEngineResult::UnknownSchema;
						Out->EngineStatus = -1; // resolved by name before any engine call that binds
						St.bFinished.store(true, std::memory_order_release);
						return;
					}
					Status = sslm_gpu_prefix_set_schema(L1->Ctx, St.Prefix, Index);
					Note(Status);
				}
#if WITH_DEV_AUTOMATION_TESTS
				if (Status == SslmGpuStatus::SSLM_OK)
				{
					NoteBuildState(St.EntryId, ESuperSLMPrefixBuildState::Begun);
				}
#endif
			}
#if WITH_DEV_AUTOMATION_TESTS
			// The build hold (§8; cells 7.21, 1.7) and the chunk hold (cells 1.7 (c), 5.13), the GPU's way: a held step
			// calls nothing more and returns, and the glue posts it again, so the submission thread never waits inside a
			// job (a job that waited would near the row-21 device-loss bound). The build hold holds after begin, before
			// the first prefill; the chunk hold between two prefilling steps.
			if (Status == SslmGpuStatus::SSLM_OK && St.Done < St.Tokens.Num())
			{
				const int32 ChunkAfter = Test.ChunkHoldAfter.load();
				const bool bHeld = (St.StepsRun == 0 && Test.bBuildHold.load())
					|| (ChunkAfter >= 0 && St.StepsRun > 0 && St.StepsRun >= ChunkAfter);
				if (bHeld)
				{
					if (!St.bHeldCounted && St.StepsRun == 0 && Test.bBuildHold.load())
					{
						St.bHeldCounted = true;
						Test.HeldBuilds.fetch_add(1);
					}
					return;
				}
				if (St.bHeldCounted)
				{
					St.bHeldCounted = false;
					Test.HeldBuilds.fetch_sub(1);
				}
			}
#endif
			const double SliceEnd = FPlatformTime::Seconds() + BuildStepSliceSeconds;
			int32 CallsThisStep = 0;
			while (Status == SslmGpuStatus::SSLM_OK && St.Done < St.Tokens.Num() && (CallsThisStep == 0 || FPlatformTime::Seconds() < SliceEnd))
			{
				const int32 Count = FMath::Min(St.Tokens.Num() - St.Done, L1->MaxPrefillChunk);
				int32_t Consumed = 0;
				Status = sslm_gpu_prefix_prefill(L1->Ctx, St.Prefix, St.Tokens.GetData() + St.Done, Count, /*prompt*/ 0, L1->DispatchBudget, &Consumed);
				++St.PrefillCalls;
				++CallsThisStep;
				Note(Status);
				St.Done += Consumed;
				if (Status == SslmGpuStatus::SSLM_OK && Consumed <= 0)
				{
					Status = SslmGpuStatus::SSLM_GPU_PREFIX_REJECTED; // no progress: never loop
				}
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (CallsThisStep > 0)
			{
				++St.StepsRun;
				FScopeLock Lock(&Test.BuildStateGuard);
				Test.ChunkJobsByEntry.FindOrAdd(St.EntryId) += 1;
			}
#endif
			if (Status == SslmGpuStatus::SSLM_OK && St.Done < St.Tokens.Num())
			{
				return; // the slice is spent: the glue posts the next step
			}
			NotePrefixPrefills(St.PrefillCalls); // §15.26
			if (Status == SslmGpuStatus::SSLM_OK)
			{
				Status = sslm_gpu_prefix_freeze(L1->Ctx, St.Prefix);
				Note(Status);
			}
			if (Status != SslmGpuStatus::SSLM_OK && St.Prefix != nullptr)
			{
				Note(sslm_gpu_prefix_release(L1->Ctx, St.Prefix));
				St.Prefix = nullptr;
			}
			Out->Engine = GpuEngineResultOf(Status);
			if (Out->Engine == EPrefixEngineResult::Refused)
			{
				Out->Engine = EPrefixEngineResult::Failed; // a build's refusal is not a miss
			}
			Out->EngineStatus = static_cast<int32_t>(Status);
			Out->EngineHandle = GpuHandleBits(St.Prefix);
#if WITH_DEV_AUTOMATION_TESTS
			if (Status == SslmGpuStatus::SSLM_OK)
			{
				NoteBuildState(St.EntryId, ESuperSLMPrefixBuildState::Frozen);
			}
			else
			{
				ForgetBuildState(St.EntryId);
			}
#endif
			St.bFinished.store(true, std::memory_order_release);
		};
		Job.MoreSteps = [Steps]() { return !Steps->bFinished.load(std::memory_order_acquire); };
		Job.AbortSteps = [this, L1, Note, Steps](FPrefixCompletion* Out)
		{
			FSteps& St = *Steps;
			if (St.bFinished.load(std::memory_order_acquire))
			{
				return;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (St.bHeldCounted)
			{
				St.bHeldCounted = false;
				Test.HeldBuilds.fetch_sub(1);
			}
			ForgetBuildState(St.EntryId);
			if (St.Prefix != nullptr)
			{
				++Test.UnfrozenBuildReleases; // cell 1.7: a begun, unfrozen build the teardown released (once)
			}
#endif
			if (St.Prefix != nullptr)
			{
				Note(sslm_gpu_prefix_release(L1->Ctx, St.Prefix));
				St.Prefix = nullptr;
			}
			Out->Engine = EPrefixEngineResult::Failed;
			Out->EngineStatus = -1;
			Out->EngineHandle = 0;
			St.bFinished.store(true, std::memory_order_release);
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
#if WITH_DEV_AUTOMATION_TESTS
		// The restore hold (cell 1.7): a held restore calls nothing and is posted again, as a held build step is.
		struct FRestoreHold
		{
			std::atomic<bool> bRan{false};
			bool bCounted = false;
		};
		TSharedRef<FRestoreHold, ESPMode::ThreadSafe> RestoreHold = MakeShared<FRestoreHold, ESPMode::ThreadSafe>();
		Job.MoreSteps = [RestoreHold]() { return !RestoreHold->bRan.load(std::memory_order_acquire); };
		Job.AbortSteps = [this, RestoreHold, Ref](FPrefixCompletion* Out)
		{
			if (RestoreHold->bRan.load(std::memory_order_acquire))
			{
				return;
			}
			if (RestoreHold->bCounted)
			{
				RestoreHold->bCounted = false;
				Test.HeldRestores.fetch_sub(1);
			}
			DropPayload(Ref);
			Out->Engine = EPrefixEngineResult::Failed;
			Out->EngineStatus = -1;
			RestoreHold->bRan.store(true, std::memory_order_release);
		};
#endif
		Job.Run = [this, L1, Note, Ref, Hint
#if WITH_DEV_AUTOMATION_TESTS
			, RestoreHold
#endif
		](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
			if (Test.bGpuRestoreHold.load())
			{
				if (!RestoreHold->bCounted)
				{
					RestoreHold->bCounted = true;
					Test.HeldRestores.fetch_add(1);
				}
				return;
			}
			if (RestoreHold->bCounted)
			{
				RestoreHold->bCounted = false;
				Test.HeldRestores.fetch_sub(1);
			}
			ON_SCOPE_EXIT { RestoreHold->bRan.store(true, std::memory_order_release); };
#endif
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
#if WITH_DEV_AUTOMATION_TESTS
		// The save hold (cell 3.10 (v); the U2 review's delete-behind-save, GPU twins): a held save calls nothing and is
		// posted again, as a held restore is, so its write-back's file write is never issued while it holds. A held save the
		// teardown ends (the glue's abort) completes Skipped: it never ran, so nothing failed and nothing is logged, and a
		// drop's delete waiting on it runs in the teardown's drain (§5.4 rev 1.18.5), as the CPU host's step 1a has it.
		struct FSaveHold
		{
			std::atomic<bool> bRan{false};
			bool bCounted = false;
		};
		TSharedRef<FSaveHold, ESPMode::ThreadSafe> SaveHold = MakeShared<FSaveHold, ESPMode::ThreadSafe>();
		Job.MoreSteps = [SaveHold]() { return !SaveHold->bRan.load(std::memory_order_acquire); };
		Job.AbortSteps = [this, SaveHold](FPrefixCompletion* Out)
		{
			if (SaveHold->bRan.load(std::memory_order_acquire))
			{
				return;
			}
			if (SaveHold->bCounted)
			{
				SaveHold->bCounted = false;
				Test.HeldSaves.fetch_sub(1);
			}
			Out->Engine = EPrefixEngineResult::Skipped;
			Out->EngineStatus = -1;
			SaveHold->bRan.store(true, std::memory_order_release);
		};
#endif
		Job.Run = [this, L1, Note, Engine
#if WITH_DEV_AUTOMATION_TESTS
			, SaveHold
#endif
		](const FSuperSLMPrefixLaneContext& /*Lane*/, FPrefixCompletion* Out)
		{
			CheckOffGameThread();
#if WITH_DEV_AUTOMATION_TESTS
			if (Test.bGpuSaveHold.load())
			{
				if (!SaveHold->bCounted)
				{
					SaveHold->bCounted = true;
					Test.HeldSaves.fetch_add(1);
				}
				return;
			}
			if (SaveHold->bCounted)
			{
				SaveHold->bCounted = false;
				Test.HeldSaves.fetch_sub(1);
			}
			ON_SCOPE_EXIT { SaveHold->bRan.store(true, std::memory_order_release); };
#endif
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
#if WITH_DEV_AUTOMATION_TESTS
			// The GPU busy-release substitution (§8, cell 5.15's GPU legs): SSLM_BUSY returned in place of a release
			// that is never called, so the handle stays live and the core issues the release again.
			int32 Busy = Test.GpuBusyReleases.load();
			while (Busy > 0 && !Test.GpuBusyReleases.compare_exchange_weak(Busy, Busy - 1)) {}
			SslmGpuStatus Status = Busy > 0 ? SslmGpuStatus::SSLM_BUSY : sslm_gpu_prefix_release(L1->Ctx, GpuHandleOf(Engine));
			const int32 Substitute = Test.GpuSubstitutedReleaseStatus.load();
			if (Busy <= 0 && Substitute != 0)
			{
				Status = static_cast<SslmGpuStatus>(Substitute); // the real release ran and consumed the handle
			}
#else
			const SslmGpuStatus Status = sslm_gpu_prefix_release(L1->Ctx, GpuHandleOf(Engine));
#endif
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
