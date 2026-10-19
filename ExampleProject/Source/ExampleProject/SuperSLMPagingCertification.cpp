#include "SuperSLMPagingCertification.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMSubsystem.h"
#include "UObject/StrongObjectPtr.h"

namespace SuperSLMExamplePaging
{
	constexpr int32 Npcs = 50;
	constexpr int32 PersonaLength = 1000;
	constexpr int32 SuffixLength = 8;
	constexpr int32 Budget = 512;
	constexpr int32 ToStop = 505; // 1 + ((1000 + 512) - (1000 + 8)), plan 10.9 / token law E7
	// Composed clients let the separate prefix-build seat progress while their fixed-K
	// events await apply. Keep every solo, reset and interleaved client on this same path.
	constexpr ESuperSLMGpuDecodePath GpuPath = ESuperSLMGpuDecodePath::Composed;

	struct FSlot
	{
		FSuperSLMSequence Cpu;
		FSuperSLMGpuSequence Gpu;
		FSuperSLMLifecycleOpHandle Lifecycle;
		FSuperSLMLifecycleOpHandle Generation;
	};

	TArray<TSharedPtr<FJsonValue>> JsonTokens(const TArray<int32>& Tokens)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Tokens.Num());
		for (int32 Token : Tokens) { Values.Add(MakeShared<FJsonValueNumber>(Token)); }
		return Values;
	}

	TSharedRef<FJsonObject> JsonStats(const FSuperSLMKvPageStats& Stats)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("paged"), Stats.bPagedMode);
		Object->SetNumberField(TEXT("page_positions"), Stats.PagePositions);
		Object->SetNumberField(TEXT("page_bytes"), Stats.PageBytes);
		Object->SetNumberField(TEXT("pool_pages"), Stats.PoolPages);
		Object->SetNumberField(TEXT("sequence_capacity"), Stats.Sequences.CapacityPages);
		Object->SetNumberField(TEXT("prefix_capacity"), Stats.Prefixes.CapacityPages);
		Object->SetNumberField(TEXT("prefix_charged"), Stats.Prefixes.ChargedPages);
		Object->SetNumberField(TEXT("headroom_capacity"), Stats.RestoreHeadroom.CapacityPages);
		Object->SetNumberField(TEXT("ghosts"), Stats.GhostCount);
		Object->SetNumberField(TEXT("live_prefixes"), Stats.LivePrefixCount);
		Object->SetStringField(TEXT("ledger_faults"), FString::Printf(TEXT("%llu"), Stats.LedgerFaults));
		Object->SetBoolField(TEXT("has_engine_snapshot"), Stats.bHasPoolSnapshot);
		Object->SetNumberField(TEXT("engine_page_count"), Stats.SnapshotPageCount);
		Object->SetNumberField(TEXT("engine_free_pages"), Stats.SnapshotFreePages);
		Object->SetNumberField(TEXT("engine_shared_pages"), Stats.SnapshotSharedPages);
		return Object;
	}
}

struct FSuperSLMPagingCertification::FState
{
	using FSlot = SuperSLMExamplePaging::FSlot;
	using EPhase = ESuperSLMSequencePhase;
	enum class EStep { Start, SoloPrefix, SoloAdopt, SoloRun, SoloReset, PagedPrefix, PagedAdopt, MidRun, Released, Rebuild, FinishRun, Cleanup, Done };
	TStrongObjectPtr<USuperSLMModel> Model;
	USuperSLMSubsystem& Cpu;
	USuperSLMGpuSubsystem* Gpu;
	bool bGpu = false;
	int32 ExpectedCap = 0;
	EStep Step = EStep::Start;
	FSlot Solo;
	TArray<FSlot> Slots;
	FSuperSLMPrefix Prefix, Replacement;
	TArray<int32> Persona;
	TArray<TArray<int32>> Suffixes, Twins;
	int32 TwinIndex = 0;
	int32 ReplacementLength = 0;
	FString Failure, OutputDirectory;
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SlotResults;
	double Started = FPlatformTime::Seconds();
	double LastCheckpoint = 0;
	double InterleavedStarted = 0;

	FState(USuperSLMModel& InModel, USuperSLMSubsystem& InCpu, USuperSLMGpuSubsystem* InGpu,
		bool bInGpu, int32 InCap, const FString& AssetPath)
		: Model(&InModel), Cpu(InCpu), Gpu(InGpu), bGpu(bInGpu), ExpectedCap(InCap)
	{
		OutputDirectory = FPaths::ProjectSavedDir() / TEXT("SuperSLMPagingCertification") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Report->SetNumberField(TEXT("schema"), 1);
		Report->SetStringField(TEXT("backend"), bGpu ? TEXT("GPU") : TEXT("CPU"));
		Report->SetStringField(TEXT("model_asset"), AssetPath);
		Report->SetNumberField(TEXT("expected_model_cap"), ExpectedCap);
		Report->SetBoolField(TEXT("payload_memory_mapped"), InModel.IsPayloadMemoryMapped());
		Report->SetBoolField(TEXT("payload_aligned"), reinterpret_cast<UPTRINT>(InModel.GetMappedArtifactData()) % USuperSLMModel::CookedArtifactAlignment == 0);
		Report->SetNumberField(TEXT("payload_bytes"), InModel.GetMappedArtifactSize());
		FString Hash;
		if (InModel.GetMappedArtifactData() && InModel.GetMappedArtifactSize() >= 64)
		{
			const uint8* Bytes = static_cast<const uint8*>(InModel.GetMappedArtifactData());
			for (int32 Byte = 32; Byte < 64; ++Byte) { Hash += FString::Printf(TEXT("%02x"), Bytes[Byte]); }
		}
		Report->SetStringField(TEXT("artifact_hash"), Hash);
		TArray<TSharedPtr<FJsonValue>> Enabled;
		const TSharedRef<FJsonObject> Locations = MakeShared<FJsonObject>();
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			Enabled.Add(MakeShared<FJsonValueString>(Plugin->GetName()));
			Locations->SetStringField(Plugin->GetName(), Plugin->GetBaseDir());
		}
		Report->SetArrayField(TEXT("enabled_plugins"), Enabled);
		Report->SetObjectField(TEXT("enabled_plugin_locations"), Locations);
		Report->SetStringField(TEXT("stats_scope"), TEXT("Page statistics are observations, not the page-count oracle. The Development figures grade occupancy with the independent fill probe; this Shipping run grades actual transcripts, stops and the surviving ledger-fault counter."));
		Write(TEXT("running"));
	}

	bool Write(const TCHAR* Status)
	{
		Report->SetStringField(TEXT("status"), Status);
		Report->SetStringField(TEXT("failure"), Failure);
		Report->SetNumberField(TEXT("step"), int32(Step));
		Report->SetNumberField(TEXT("solo_twins_completed"), Twins.Num());
		Report->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - Started);
		TArray<TSharedPtr<FJsonValue>> Progress;
		for (const FSlot& Slot : Slots) { Progress.Add(MakeShared<FJsonValueNumber>(Tokens(Slot).Num())); }
		Report->SetArrayField(TEXT("slot_token_counts"), Progress);
		FString Json;
		const bool bSaved = FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json))
			&& IFileManager::Get().MakeDirectory(*OutputDirectory, true)
			&& FFileHelper::SaveStringToFile(Json, *(OutputDirectory / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bSaved) { Step = EStep::Done; FPlatformMisc::RequestExitWithStatus(false, 1); }
		LastCheckpoint = FPlatformTime::Seconds();
		return bSaved;
	}
	bool Check(bool bCondition, const FString& Message)
	{
		if (bCondition) { return true; }
		Failure = Message;
		Step = EStep::Done;
		Write(TEXT("failed"));
		FPlatformMisc::RequestExitWithStatus(false, 1);
		return false;
	}
	FSuperSLMKvPageStats Stats() const { return bGpu ? Gpu->GetKvPageStats() : Cpu.GetKvPageStats(); }
	const TArray<int32>& Tokens(const FSlot& Slot) const { return bGpu ? Gpu->GetGeneratedTokens(Slot.Gpu) : Cpu.GetGeneratedTokens(Slot.Cpu); }
	EPhase Phase(const FSlot& Slot) const { return bGpu ? Gpu->GetPhase(Slot.Gpu) : Cpu.GetPhase(Slot.Cpu); }
	ESuperSLMDecodeOutcome Outcome(const FSlot& Slot) const { return bGpu ? Gpu->GetLastDecodeOutcome(Slot.Gpu) : Cpu.GetLastDecodeOutcome(Slot.Cpu); }
	ESuperSLMPrefixPhase PrefixPhase(const FSuperSLMPrefix& Handle) const { return bGpu ? Gpu->GetPrefixPhase(Handle) : Cpu.GetPrefixPhase(Handle); }
	bool RecordActiveSlotCounts(const TCHAR* Field)
	{
		TArray<int32> Counts;
		if (!Check(Slots.Num() == SuperSLMExamplePaging::Npcs, TEXT("phase observation is missing NPC slots"))) { return false; }
		for (const FSlot& Slot : Slots)
		{
			const int32 Count = Tokens(Slot).Num();
			if (!Check(Count >= 8 && Count < SuperSLMExamplePaging::ToStop && Phase(Slot) == EPhase::Decoding,
				FString::Printf(TEXT("%s: every NPC must still be decoding after 8 and before 505 tokens"), Field))) { return false; }
			Counts.Add(Count);
		}
		Report->SetArrayField(Field, SuperSLMExamplePaging::JsonTokens(Counts));
		return true;
	}

	// Plan 10.9 phase 2: "every surviving sharer's tokens are unchanged". At replacement Ready a slot may
	// already have reached its limit stop (on the CPU at cap 32,768 the exact-room replacement is 31,680
	// tokens of serial batched prefill, longer than the slots' remaining decode); at least one slot must
	// still be decoding, so a use after free of the freed pages reaches a transcript the twin check reads.
	bool RecordSurvivingSlotCounts(const TCHAR* Field)
	{
		TArray<int32> Counts;
		int32 Surviving = 0;
		if (!Check(Slots.Num() == SuperSLMExamplePaging::Npcs, TEXT("phase observation is missing NPC slots"))) { return false; }
		for (const FSlot& Slot : Slots)
		{
			const int32 Count = Tokens(Slot).Num();
			const bool bDecoding = Phase(Slot) == EPhase::Decoding;
			if (!Check(Count >= 8 && (bDecoding ? Count < SuperSLMExamplePaging::ToStop : Count == SuperSLMExamplePaging::ToStop),
				FString::Printf(TEXT("%s: every NPC must be decoding below 505 tokens or stopped at 505"), Field))) { return false; }
			Surviving += bDecoding ? 1 : 0;
			Counts.Add(Count);
		}
		if (!Check(Surviving > 0, FString::Printf(TEXT("%s: at least one NPC must still be decoding"), Field))) { return false; }
		Report->SetArrayField(Field, SuperSLMExamplePaging::JsonTokens(Counts));
		return true;
	}

	bool Configure(int32 Count, int32 SequenceBudget, int32 PrefixBlocks)
	{
		if (bGpu)
		{
			FSuperSLMGpuRuntimeConfig Config;
			Config.ContextCap = 4096;
			Config.BlockCount = Count;
			Config.SequenceBudget = SequenceBudget;
			Config.PrefixBlockCount = PrefixBlocks;
			Config.RestoreHeadroomBlocks = 0;
			Config.DispatchBudget = 4096;
			Config.K = Count + 1;
			Config.TickBudgetMs = 1000.0;
			Config.FinishParallelTasks = 4;
			return Check(Gpu->Configure(Model.Get(), Config).Result == ESuperSLMGpuConfigureResult::Success, TEXT("GPU configuration refused"));
		}
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = Count;
		Config.SequenceBudget = SequenceBudget;
		Config.PrefixBlockCount = PrefixBlocks;
		Config.RestoreHeadroomBlocks = 0;
		Config.MaxSequencesPerDecodeCall = FMath::Clamp(Count, 1, 8);
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = 24;
		Config.MaxConcurrentCalls = 2;
		Config.TickBudgetMs = 16.6;
		Config.SequenceLifecycleBudgetMs = 100000.0;
		Config.bParallelDecode = false;
		Config.FinishParallelTasks = 4;
		return Check(Cpu.Configure(Model.Get(), Config).Result == ESuperSLMConfigureResult::Success, TEXT("CPU configuration refused"));
	}
	bool Vend(FSlot& Slot)
	{
		return bGpu ? Check(Gpu->VendSequence(Slot.Gpu, SuperSLMExamplePaging::GpuPath) == ESuperSLMGpuVendResult::Success, TEXT("GPU vend refused"))
			: Check(Cpu.VendSequence(Slot.Cpu) == ESuperSLMVendResult::Success, TEXT("CPU vend refused"));
	}
	void Return(FSlot& Slot)
	{
		if (bGpu) { if (Slot.Gpu.IsValid()) { Gpu->ReturnSequence(Slot.Gpu); } }
		else { if (Slot.Cpu.IsValid()) { Cpu.ReturnSequence(Slot.Cpu); } }
		Slot = {};
	}
	bool Build(const TArray<int32>& Input, FSuperSLMPrefix& Handle)
	{
		FString Error;
		const ESuperSLMPrefixResult Result = bGpu ? Gpu->CreatePrefix(Input, Handle, &Error) : Cpu.CreatePrefix(Input, Handle, &Error);
		return Check(Result == ESuperSLMPrefixResult::Success, TEXT("prefix build refused: ") + Error);
	}
	bool Release(const FSuperSLMPrefix& Handle)
	{
		FString Error;
		return Check(bGpu ? Gpu->ReleasePrefix(Handle, Error) : Cpu.ReleasePrefix(Handle, Error), TEXT("prefix release refused: ") + Error);
	}
	bool Adopt(FSlot& Slot)
	{
		if (bGpu) { Slot.Lifecycle = Gpu->RequestAdoptPrefix(Slot.Gpu, Prefix); }
		else
		{
			FString Error;
			const ESuperSLMRestoreResult Result = Cpu.AdoptPrefix(Slot.Cpu, Prefix, Slot.Lifecycle, Error);
			if (!Check(Result == ESuperSLMRestoreResult::Pending || Result == ESuperSLMRestoreResult::Success, TEXT("CPU adopt refused: ") + Error)) { return false; }
		}
		return Check(Slot.Lifecycle.IsValid(), TEXT("adopt returned no handle"));
	}
	bool Reset(FSlot& Slot)
	{
		if (bGpu) { Slot.Lifecycle = Gpu->RequestResetSequence(Slot.Gpu, SuperSLMExamplePaging::GpuPath); }
		else
		{
			FString Error;
			const ESuperSLMRestoreResult Result = Cpu.ResetSequence(Slot.Cpu, Slot.Lifecycle, Error);
			if (!Check(Result == ESuperSLMRestoreResult::Pending || Result == ESuperSLMRestoreResult::Success, TEXT("CPU reset refused: ") + Error)) { return false; }
		}
		return Check(Slot.Lifecycle.IsValid(), TEXT("reset returned no handle"));
	}
	bool Resolved(FSlot& Slot)
	{
		const ESuperSLMRestoreResult Result = bGpu ? Gpu->GetLifecycleOpResult(Slot.Lifecycle) : Cpu.GetLifecycleOpResult(Slot.Lifecycle);
		if (Result == ESuperSLMRestoreResult::Pending) { return false; }
		if (!Check(Result == ESuperSLMRestoreResult::Success, TEXT("queued lifecycle operation failed"))) { return false; }
		if (bGpu) { Gpu->ReleaseLifecycleOpHandle(Slot.Lifecycle); } else { Cpu.ReleaseLifecycleOpHandle(Slot.Lifecycle); }
		Slot.Lifecycle = {};
		return true;
	}
	bool Begin(FSlot& Slot, const TArray<int32>& Input, int32 Maximum)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Input;
		Request.MaxNewTokens = Maximum;
		Request.StopTokenIds.Reset();
		if (bGpu)
		{
			Slot.Generation = Gpu->RequestBeginGeneration(Slot.Gpu, Request);
			return Check(Slot.Generation.IsValid(), TEXT("GPU generation request refused"));
		}
		FString Error;
		return Check(Cpu.BeginGeneration(Slot.Cpu, Request, Error), TEXT("CPU generation refused: ") + Error);
	}
	bool Ended(FSlot& Slot)
	{
		if (bGpu && Slot.Generation.IsValid())
		{
			const ESuperSLMRestoreResult Result = Gpu->GetLifecycleOpResult(Slot.Generation);
			if (Result == ESuperSLMRestoreResult::Pending) { return false; }
			if (!Check(Result == ESuperSLMRestoreResult::Success, TEXT("queued GPU generation refused at activation"))) { return false; }
			Gpu->ReleaseLifecycleOpHandle(Slot.Generation);
			Slot.Generation = {};
		}
		return Phase(Slot) == EPhase::Complete || Phase(Slot) == EPhase::Faulted;
	}
	bool BeginTwin()
	{
		TArray<int32> Input = bGpu ? TArray<int32>() : Persona;
		Input.Append(Suffixes[TwinIndex]);
		Step = EStep::SoloRun;
		return Begin(Solo, Input, SuperSLMExamplePaging::ToStop);
	}
	bool InputTokens()
	{
		TArray<int32> Base;
		FString Error;
		if (!Check(SuperSLM::TokenizeText(*Model.Get(), TEXT("The harbour keeper records ships, repairs ropes, shares lamp oil and remembers the roads through the coastal hills. "), Base, Error) && Base.Num() > 0, TEXT("persona tokenization failed: ") + Error)) { return false; }
		for (int32 Index = 0; Index < SuperSLMExamplePaging::PersonaLength; ++Index) { Persona.Add(Base[Index % Base.Num()]); }
		for (int32 Index = 0; Index < SuperSLMExamplePaging::Npcs; ++Index)
		{
			TArray<int32> Input;
			if (!Check(SuperSLM::TokenizeText(*Model.Get(), FString::Printf(TEXT("Customer %d asks about rope, oil, ships and the harbour."), Index), Input, Error)
				&& Input.Num() >= SuperSLMExamplePaging::SuffixLength, TEXT("suffix tokenization failed: ") + Error)) { return false; }
			Input.SetNum(SuperSLMExamplePaging::SuffixLength);
			for (const TArray<int32>& Prior : Suffixes) { if (!Check(Prior != Input, TEXT("suffixes are not distinct"))) { return false; } }
			Suffixes.Add(MoveTemp(Input));
		}
		Report->SetArrayField(TEXT("persona_tokens"), SuperSLMExamplePaging::JsonTokens(Persona));
		return true;
	}

	void Tick(float DeltaSeconds)
	{
		using namespace SuperSLMExamplePaging;
		if (Step == EStep::Done) { return; }
		if (!Check(FPlatformTime::Seconds() - Started < 8.0 * 3600.0, TEXT("certification timed out"))) { return; }
		if (Step != EStep::Start) { if (bGpu) { Gpu->Tick(DeltaSeconds); } else { Cpu.Tick(DeltaSeconds); } }
		if (FPlatformTime::Seconds() - LastCheckpoint >= 10.0 && !Write(TEXT("running"))) { return; }
		switch (Step)
		{
		case EStep::Start:
			if (!Check((ExpectedCap == 4096 || ExpectedCap == 32768) && (!bGpu || (Gpu && ExpectedCap == 4096)), TEXT("unsupported certification model/backend"))
				|| !Check(Model->IsPayloadMemoryMapped(), TEXT("cooked Windows artifact is not memory mapped"))
				|| !Check(Model->GetMappedArtifactData() && reinterpret_cast<UPTRINT>(Model->GetMappedArtifactData()) % USuperSLMModel::CookedArtifactAlignment == 0, TEXT("cooked artifact mapping is missing or misaligned"))
				|| !InputTokens() || !Configure(1, bGpu ? Budget : 0, bGpu ? 1 : 0) || !Vend(Solo)) { return; }
			if (bGpu) { if (Build(Persona, Prefix)) { Step = EStep::SoloPrefix; } }
			else { BeginTwin(); }
			break;
		case EStep::SoloPrefix:
			if (PrefixPhase(Prefix) == ESuperSLMPrefixPhase::Ready) { if (Adopt(Solo)) { Step = EStep::SoloAdopt; } }
			else { Check(PrefixPhase(Prefix) != ESuperSLMPrefixPhase::Faulted && PrefixPhase(Prefix) != ESuperSLMPrefixPhase::Invalid, TEXT("solo prefix build failed")); }
			break;
		case EStep::SoloAdopt:
			if (Resolved(Solo)) { BeginTwin(); }
			break;
		case EStep::SoloRun:
			if (!Ended(Solo)) { break; }
			if (!Check(Phase(Solo) == EPhase::Complete && Tokens(Solo).Num() == ToStop, TEXT("solo twin did not complete all 505 tokens"))) { return; }
			Twins.Add(Tokens(Solo));
			TwinIndex++;
			if (TwinIndex < Npcs) { if (Reset(Solo)) { Step = EStep::SoloReset; } break; }
			Report->SetNumberField(TEXT("solo_elapsed_s"), FPlatformTime::Seconds() - Started);
			Return(Solo);
			if (!Configure(Npcs, Budget, 1)) { return; }
			Prefix = {};
			if (Build(Persona, Prefix)) { Step = EStep::PagedPrefix; }
			break;
		case EStep::SoloReset:
			if (Resolved(Solo)) { if (bGpu) { if (Adopt(Solo)) { Step = EStep::SoloAdopt; } } else { BeginTwin(); } }
			break;
		case EStep::PagedPrefix:
			if (PrefixPhase(Prefix) != ESuperSLMPrefixPhase::Ready) { Check(PrefixPhase(Prefix) != ESuperSLMPrefixPhase::Faulted && PrefixPhase(Prefix) != ESuperSLMPrefixPhase::Invalid, TEXT("paged prefix failed")); break; }
			for (int32 Index = 0; Index < Npcs; ++Index)
			{
				FSlot Slot;
				if (!Vend(Slot) || !Adopt(Slot)) { return; }
				Slots.Add(Slot);
			}
			Step = EStep::PagedAdopt;
			break;
		case EStep::PagedAdopt:
			{
				bool bAll = true;
				for (FSlot& Slot : Slots) { if (Slot.Lifecycle.IsValid()) { bAll &= Resolved(Slot); } }
				if (Step == EStep::Done || !bAll) { break; }
				InterleavedStarted = FPlatformTime::Seconds();
				for (int32 Index = 0; Index < Npcs; ++Index) { if (!Begin(Slots[Index], Suffixes[Index], ToStop + 16)) { return; } }
				Step = EStep::MidRun;
			}
			break;
		case EStep::MidRun:
			{
				bool bAll = true;
				for (FSlot& Slot : Slots)
				{
					if (Tokens(Slot).Num() < 8) { if (Ended(Slot)) { Check(false, TEXT("a slot stopped before phase 2")); return; } bAll = false; }
				}
				if (!bAll) { break; }
				if (!RecordActiveSlotCounts(TEXT("mid_run_token_counts"))) { return; }
				Report->SetObjectField(TEXT("mid_run"), JsonStats(Stats()));
				if (Release(Prefix)) { Step = EStep::Released; }
			}
			break;
		case EStep::Released:
			{
				const FSuperSLMKvPageStats Snapshot = Stats();
				if (Snapshot.GhostCount < 1 || Snapshot.LivePrefixCount != 0) { break; }
				const int64 ExpectedPagePositions = bGpu ? 16 : 64;
				const int64 ExpectedBlock = bGpu ? 256 : ExpectedCap / 64;
				const int64 ExpectedGhost = bGpu ? 63 : 16;
				if (!Check(Snapshot.PagePositions == ExpectedPagePositions && Snapshot.Prefixes.CapacityPages == ExpectedBlock
					&& Snapshot.Prefixes.ChargedPages == ExpectedGhost, TEXT("phase-2 construction does not have the required geometry and ghost charge"))) { return; }
				if (!RecordActiveSlotCounts(TEXT("released_token_counts"))) { return; }
				ReplacementLength = int32((ExpectedBlock - ExpectedGhost - 1) * ExpectedPagePositions);
				TArray<int32> ReplacementTokens;
				for (int32 Index = 0; Index < ReplacementLength; ++Index) { ReplacementTokens.Add(Persona[(Index + 17) % Persona.Num()]); }
				Report->SetObjectField(TEXT("released"), JsonStats(Snapshot));
				Report->SetNumberField(TEXT("replacement_length"), ReplacementLength);
				if (Build(ReplacementTokens, Replacement)) { Step = EStep::Rebuild; }
			}
			break;
		case EStep::Rebuild:
			if (PrefixPhase(Replacement) != ESuperSLMPrefixPhase::Ready) { Check(PrefixPhase(Replacement) != ESuperSLMPrefixPhase::Faulted && PrefixPhase(Replacement) != ESuperSLMPrefixPhase::Invalid, TEXT("exact-room replacement prefix failed")); break; }
			if (!RecordSurvivingSlotCounts(TEXT("replacement_ready_token_counts"))) { return; }
			Report->SetObjectField(TEXT("replacement_ready"), JsonStats(Stats()));
			Step = EStep::FinishRun;
			break;
		case EStep::FinishRun:
			{
				bool bAll = true;
				for (FSlot& Slot : Slots) { bAll &= Ended(Slot); }
				if (Step == EStep::Done || !bAll) { break; }
				for (int32 Index = 0; Index < Npcs; ++Index)
				{
					const FSlot& Slot = Slots[Index];
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetNumberField(TEXT("slot"), Index);
					Row->SetArrayField(TEXT("suffix"), JsonTokens(Suffixes[Index]));
					Row->SetArrayField(TEXT("solo_tokens"), JsonTokens(Twins[Index]));
					Row->SetArrayField(TEXT("tokens"), JsonTokens(Tokens(Slot)));
					Row->SetNumberField(TEXT("phase"), uint8(Phase(Slot)));
					Row->SetNumberField(TEXT("outcome"), uint8(Outcome(Slot)));
					if (bGpu) { Row->SetNumberField(TEXT("gpu_fault_reason"), uint8(Gpu->GetLastFaultReason(Slot.Gpu))); }
					SlotResults.Add(MakeShared<FJsonValueObject>(Row));
				}
				Report->SetArrayField(TEXT("slots"), SlotResults);
				Report->SetObjectField(TEXT("ended"), JsonStats(Stats()));
				Report->SetNumberField(TEXT("interleaved_elapsed_s"), FPlatformTime::Seconds() - InterleavedStarted);
				if (!Write(TEXT("running"))) { return; } // retain mismatching raw transcripts too
				for (int32 Index = 0; Index < Npcs; ++Index)
				{
					const bool bLimitStop = bGpu ? Gpu->GetLastFaultReason(Slots[Index].Gpu) == ESuperSLMGpuFaultReason::KvBudgetExceeded
						: Outcome(Slots[Index]) == ESuperSLMDecodeOutcome::KvBudgetExceeded;
					if (!Check(Phase(Slots[Index]) == EPhase::Faulted && bLimitStop
						&& Tokens(Slots[Index]).Num() == ToStop && Tokens(Slots[Index]) == Twins[Index], FString::Printf(TEXT("slot %d failed its limit or solo-token oracle"), Index))) { return; }
				}
				for (FSlot& Slot : Slots) { Return(Slot); }
				if (Release(Replacement)) { Step = EStep::Cleanup; }
			}
			break;
		case EStep::Cleanup:
			{
				const FSuperSLMKvPageStats Snapshot = Stats();
				if (Snapshot.GhostCount != 0 || Snapshot.LivePrefixCount != 0) { break; }
				Report->SetObjectField(TEXT("cleanup"), JsonStats(Snapshot));
				if (!Check(Snapshot.LedgerFaults == 0, TEXT("Shipping ledger-fault counter is nonzero"))) { return; }
				Step = EStep::Done;
				if (Write(TEXT("complete"))) { FPlatformMisc::RequestExitWithStatus(false, 0); }
			}
			break;
		case EStep::Done: break;
		}
	}
};

FSuperSLMPagingCertification::FSuperSLMPagingCertification(USuperSLMModel& Model, USuperSLMSubsystem& Cpu, USuperSLMGpuSubsystem* Gpu,
	bool bGpu, int32 ExpectedCap, const FString& AssetPath)
	: State(MakeUnique<FState>(Model, Cpu, Gpu, bGpu, ExpectedCap, AssetPath)) {}
FSuperSLMPagingCertification::~FSuperSLMPagingCertification() = default;
void FSuperSLMPagingCertification::Tick(float DeltaSeconds) { State->Tick(DeltaSeconds); }
