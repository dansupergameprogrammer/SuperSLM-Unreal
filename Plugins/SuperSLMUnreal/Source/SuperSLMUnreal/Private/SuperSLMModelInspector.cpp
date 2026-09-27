#include "SuperSLMModelInspector.h"

#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "SuperSLMLifecycleMeasure.h"
#include "SuperSLMModel.h"
#include "SuperSLMRuntimeRegistry.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMStatusMapping.h"
#include "UObject/StrongObjectPtr.h"

#include "superslm/sslm_abi.h"

namespace
{
	constexpr int64 kHeaderBytes = 64;
	constexpr int64 kSectionRowBytes = 40;
	constexpr int64 kCfg1Bytes = 84;
	constexpr uint32 kDampedGreedyConstantsType = 42;

	uint32 ReadLE32(const uint8* P)
	{
		return static_cast<uint32>(P[0]) | (static_cast<uint32>(P[1]) << 8) | (static_cast<uint32>(P[2]) << 16) | (static_cast<uint32>(P[3]) << 24);
	}

	uint64 ReadLE64(const uint8* P)
	{
		return static_cast<uint64>(ReadLE32(P)) | (static_cast<uint64>(ReadLE32(P + 4)) << 32);
	}

	// docs/sslm_format.md, "Section types".
	FString SectionTypeName(uint32 Type)
	{
		switch (Type)
		{
			case 0: return TEXT("Config");
			case 1: return TEXT("Provenance");
			case 2: return TEXT("Weights");
			case 3: return TEXT("Biases");
			case 4: return TEXT("RopeTables");
			case 5: return TEXT("Scales");
			case 6: return TEXT("WeightScales");
			case 7: return TEXT("CompositionConstants");
			case 8: return TEXT("KvLandingScales");
			case 9: return TEXT("KvLandingReciprocals");
			case 10: return TEXT("Calibration");
			case 11: return TEXT("GoldenHashes");
			case 12: return TEXT("SigmoidLut");
			case 20: return TEXT("Tokenizer");
			case 21: return TEXT("ChatTemplate");
			case 22: return TEXT("UnicodeTables");
			case 30: return TEXT("SchemaMasks");
			case 31: return TEXT("CalibrationBand");
			case 40: return TEXT("DeltaFoldScales");
			case 41: return TEXT("UFoldScales");
			case 42: return TEXT("DampedGreedyConstants");
			case 43: return TEXT("QkChannelTable");
			default: return FString::Printf(TEXT("Unknown(%u)"), Type);
		}
	}

	// The 84-byte CFG1 section, or null (docs/sslm_format.md, "Section table", "Config blob").
	const uint8* FindCfg1(const uint8* Bytes, int64 Size)
	{
		if (Bytes == nullptr || Size < kHeaderBytes)
		{
			return nullptr;
		}
		const uint32 SectionCount = ReadLE32(Bytes + 12);
		if (kHeaderBytes + static_cast<int64>(SectionCount) * kSectionRowBytes > Size)
		{
			return nullptr;
		}
		for (uint32 Row = 0; Row < SectionCount; ++Row)
		{
			const uint8* P = Bytes + kHeaderBytes + static_cast<int64>(Row) * kSectionRowBytes;
			const int64 Offset = static_cast<int64>(ReadLE64(P + 8));
			if (ReadLE32(P) == 0 && static_cast<int64>(ReadLE64(P + 16)) == kCfg1Bytes && Offset >= 0 && Offset + kCfg1Bytes <= Size)
			{
				return Bytes + Offset;
			}
		}
		return nullptr;
	}

	FString StatusName(sslm_status Status)
	{
		return FString(ANSI_TO_TCHAR(SuperSLMStatusMapping::ToDiagnosticText(Status)));
	}
}

bool SuperSLMModelInspector::ReadShape(const USuperSLMModel& Model, FSuperSLMModelShapeFacts& OutShape)
{
	OutShape = FSuperSLMModelShapeFacts();
	const uint8* Cfg = FindCfg1(static_cast<const uint8*>(Model.GetMappedArtifactData()), Model.GetMappedArtifactSize());
	if (Cfg == nullptr)
	{
		return false;
	}
	OutShape.HiddenSize = static_cast<int32>(ReadLE32(Cfg + 8));
	OutShape.NumHiddenLayers = static_cast<int32>(ReadLE32(Cfg + 12));
	OutShape.VocabSize = static_cast<int32>(ReadLE32(Cfg + 32));
	OutShape.ContextCap = static_cast<int64>(ReadLE32(Cfg + 36));
	return true;
}

FSuperSLMModelInspection SuperSLMModelInspector::InspectBytes(
	const void* Data, int64 Size, const FString& ProvenanceJson, bool bGpuDeviceResidentHead,
	const FSuperSLMRuntimeConfig& DeclaredShape)
{
	FSuperSLMModelInspection Out;
	Out.ProvenanceJson = ProvenanceJson;
	Out.bGpuDeviceResidentHead = bGpuDeviceResidentHead;
	Out.SequenceLifecycleBudgetMs = DeclaredShape.SequenceLifecycleBudgetMs;
	const uint8* Bytes = static_cast<const uint8*>(Data);
	if (Bytes == nullptr || Size < kHeaderBytes)
	{
		Out.Error = TEXT("the model has no mapped artifact data");
		return Out;
	}

	// Header and section table. Import already validated both (SslmArtifact::OpenFromMemory), so
	// this is a read; the bounds checks keep it a safe one on any input.
	Out.FormatVersion = ReadLE32(Bytes + 4);
	const uint32 SectionCount = ReadLE32(Bytes + 12);
	Out.Flags = ReadLE32(Bytes + 16);
	Out.FileBytes = static_cast<int64>(ReadLE64(Bytes + 24));
	Out.ArtifactHashHex = SuperSLMRuntime::HashToHex(Bytes + 32);
	if (kHeaderBytes + static_cast<int64>(SectionCount) * kSectionRowBytes > Size)
	{
		Out.Error = TEXT("the section table runs past the artifact");
		return Out;
	}
	const uint8* Cfg = nullptr;
	for (uint32 Row = 0; Row < SectionCount; ++Row)
	{
		const uint8* P = Bytes + kHeaderBytes + static_cast<int64>(Row) * kSectionRowBytes;
		FSuperSLMSectionRow& S = Out.Sections.AddDefaulted_GetRef();
		S.Type = ReadLE32(P);
		S.TypeName = SectionTypeName(S.Type);
		S.Dtype = ReadLE32(P + 4);
		S.Offset = static_cast<int64>(ReadLE64(P + 8));
		S.ByteSize = static_cast<int64>(ReadLE64(P + 16));
		Out.bHasDampedGreedyConstants |= S.Type == kDampedGreedyConstantsType;
		if (S.Type == 0 && S.ByteSize == kCfg1Bytes && S.Offset >= 0 && S.Offset + kCfg1Bytes <= Size)
		{
			Cfg = Bytes + S.Offset;
		}
	}
	if (Cfg == nullptr)
	{
		Out.Error = TEXT("no 84-byte CFG1 Config section");
		return Out;
	}
	// docs/sslm_format.md, "Config blob -- CFG1".
	Out.HiddenSize = static_cast<int32>(ReadLE32(Cfg + 8));
	Out.NumHiddenLayers = static_cast<int32>(ReadLE32(Cfg + 12));
	Out.NumAttentionHeads = static_cast<int32>(ReadLE32(Cfg + 16));
	Out.NumKeyValueHeads = static_cast<int32>(ReadLE32(Cfg + 20));
	Out.HeadDim = static_cast<int32>(ReadLE32(Cfg + 24));
	Out.VocabSize = static_cast<int32>(ReadLE32(Cfg + 32));
	Out.ContextCap = static_cast<int64>(ReadLE32(Cfg + 36));
	Out.KvPrecisionBytes = ReadLE32(Cfg + 44) == 0 ? 1 : 2;

	// USuperSLMGpuSubsystem::Configure()'s own declared head term (plan §2.5 row 3).
	if (bGpuDeviceResidentHead)
	{
		const int64 HeadTableBytes = static_cast<int64>(Out.VocabSize) * static_cast<int64>(Out.HiddenSize);
		Out.GpuDeviceHeadDeclaredBytes = FMath::Max<int64>(4, (HeadTableBytes + 3) & ~static_cast<int64>(3))
			+ static_cast<int64>(Out.HiddenSize) * 4 + static_cast<int64>(Out.VocabSize) * 8;
	}

	// Layer 1's own size queries, on a private mapping of these bytes.
	sslm_model Model = nullptr;
	const sslm_status MapStatus = sslm_model_map(Data, static_cast<size_t>(Size), &Model);
	if (MapStatus != SSLM_OK || Model == nullptr)
	{
		Out.Error = FString::Printf(TEXT("sslm_model_map refused the artifact (%s)"), *StatusName(MapStatus));
		return Out;
	}
	sslm_config Shape = {};
	Shape.max_batch = DeclaredShape.MaxSequencesPerDecodeCall;
	Shape.max_chunk_budget = DeclaredShape.MaxPrefillChunkBudget;
	Shape.max_layer_budget = DeclaredShape.MaxLayerBudget > 0 ? DeclaredShape.MaxLayerBudget : Out.NumHiddenLayers;
	Out.WorkspaceBytes = static_cast<int64>(sslm_workspace_size(Model, &Shape));
	Out.KvBlockBytes = static_cast<int64>(sslm_kv_block_size(Model));
	Out.KvPoolOverheadBytes = static_cast<int64>(sslm_kv_pool_overhead_size(Model, static_cast<uint32_t>(FMath::Max(DeclaredShape.BlockCount, 0))));
	Out.SeqStateBytesUpperBound = static_cast<int64>(sslm_seq_state_size(Model));
	const int32 SchemaCount = static_cast<int32>(sslm_schema_count(Model));
	for (int32 Index = 0; Index < SchemaCount; ++Index)
	{
		size_t NameBytes = 0;
		if (sslm_schema_name(Model, Index, nullptr, &NameBytes) != SSLM_BUFFER_TOO_SMALL || NameBytes == 0)
		{
			Out.SchemaNames.Add(FString::Printf(TEXT("<schema %d: name unreadable>"), Index));
			continue;
		}
		TArray<char> Name;
		Name.SetNumZeroed(static_cast<int32>(NameBytes) + 1); // the name is not NUL-terminated
		if (sslm_schema_name(Model, Index, Name.GetData(), &NameBytes) != SSLM_OK)
		{
			Out.SchemaNames.Add(FString::Printf(TEXT("<schema %d: name unreadable>"), Index));
			continue;
		}
		Out.SchemaNames.Add(FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Name.GetData()), static_cast<int32>(NameBytes)));
	}
	const sslm_status UnmapStatus = sslm_model_unmap(Model);
	if (UnmapStatus != SSLM_OK)
	{
		Out.Error = FString::Printf(TEXT("sslm_model_unmap reported %s"), *StatusName(UnmapStatus));
	}

	// The Sequence Lifecycle Budget's own prediction (SuperSLMSequenceLifecycleBudget::CheckModel's
	// inputs: the write bandwidth, and adopt's measured ratio to reset).
	const SuperSLMLifecycleMeasure::FBandwidths Bw = SuperSLMLifecycleMeasure::MeasureHostBandwidths();
	const double Ratio = (Bw.CopyBytesPerSec > 0.0) ? Bw.WriteBytesPerSec / Bw.CopyBytesPerSec : 0.0;
	const FSuperSLMSequenceLifecycleReport Lifecycle = SuperSLMSequenceLifecycleBudget::Predict(
		Out.KvBlockBytes, Bw.WriteBytesPerSec, Ratio, DeclaredShape.SequenceLifecycleBudgetMs);
	Out.MeasuredBandwidthBytesPerSec = Lifecycle.MeasuredBandwidthBytesPerSec;
	Out.PredictedResetMs = Lifecycle.PredictedResetMs;
	Out.PredictedAdoptMs = Lifecycle.PredictedAdoptMs;
	Out.bWithinLifecycleBudget = Lifecycle.bWithinBudget;

	Out.bValid = Out.Error.IsEmpty();
	return Out;
}

FSuperSLMAdapterInspection SuperSLMModelInspector::InspectAdapterBytes(const void* BaseData, int64 BaseSize, const FString& AdapterPath)
{
	FSuperSLMAdapterInspection Out;
	TArray<uint8> AdapterBytes;
	if (!FFileHelper::LoadFileToArray(AdapterBytes, *AdapterPath))
	{
		Out.Error = FString::Printf(TEXT("could not read '%s'"), *AdapterPath);
		return Out;
	}
	Out.FileBytes = AdapterBytes.Num();
	if (BaseData == nullptr || BaseSize <= 0)
	{
		Out.Error = TEXT("the base model has no mapped artifact data");
		return Out;
	}
	sslm_model Base = nullptr;
	const sslm_status MapStatus = sslm_model_map(BaseData, static_cast<size_t>(BaseSize), &Base);
	if (MapStatus != SSLM_OK || Base == nullptr)
	{
		Out.Error = FString::Printf(TEXT("sslm_model_map refused the base (%s)"), *StatusName(MapStatus));
		return Out;
	}
	sslm_adapter Adapter = nullptr;
	const sslm_status AdapterStatus = sslm_adapter_map(AdapterBytes.GetData(), static_cast<size_t>(AdapterBytes.Num()), Base, &Adapter);
	Out.BaseCheckStatus = StatusName(AdapterStatus);
	Out.bBaseMatches = AdapterStatus == SSLM_OK && Adapter != nullptr;
	if (Out.bBaseMatches)
	{
		Out.ResidencyBytes = static_cast<int64>(sslm_adapter_residency(Adapter));
		sslm_adapter_release(Adapter);
	}
	sslm_model_unmap(Base);
	Out.bValid = true;
	return Out;
}

void SuperSLMModelInspector::InspectModelAsync(
	USuperSLMModel& Model, const FSuperSLMRuntimeConfig& DeclaredShape,
	TUniqueFunction<void(const FSuperSLMModelInspection&)> OnDone)
{
	check(IsInGameThread());
	// The strong reference keeps the model alive while the worker runs; it is created here and
	// released in the game-thread continuation below. The object alone does not keep its bytes
	// (review S5: a reload replaces them in place), so the worker also holds a pin on them.
	TSharedPtr<TStrongObjectPtr<USuperSLMModel>> Hold = MakeShared<TStrongObjectPtr<USuperSLMModel>>(&Model);
	FSuperSLMArtifactBytesPin Bytes = Model.PinArtifactBytes();
	const FString Provenance = Model.ProvenanceJson;
	const bool bHead = Model.bGpuDeviceResidentHead;
	Async(EAsyncExecution::ThreadPool,
		[Hold, Bytes = MoveTemp(Bytes), Provenance, bHead, DeclaredShape, OnDone = MoveTemp(OnDone)]() mutable
		{
			FSuperSLMModelInspection Result = InspectBytes(
				Bytes.IsValid() ? Bytes->GetData() : nullptr, Bytes.IsValid() ? Bytes->GetSize() : 0,
				Provenance, bHead, DeclaredShape);
			Bytes.Reset();
			AsyncTask(ENamedThreads::GameThread,
				[Hold = MoveTemp(Hold), Result = MoveTemp(Result), OnDone = MoveTemp(OnDone)]() mutable
				{
					OnDone(Result);
					Hold.Reset(); // the last reference to the strong pointer, released on the game thread
				});
		});
}

void SuperSLMModelInspector::InspectAdapterAsync(
	USuperSLMModel& Base, const FString& AdapterPath,
	TUniqueFunction<void(const FSuperSLMAdapterInspection&)> OnDone)
{
	check(IsInGameThread());
	TSharedPtr<TStrongObjectPtr<USuperSLMModel>> Hold = MakeShared<TStrongObjectPtr<USuperSLMModel>>(&Base);
	FSuperSLMArtifactBytesPin Bytes = Base.PinArtifactBytes(); // review S5, as above
	Async(EAsyncExecution::ThreadPool,
		[Hold, Bytes = MoveTemp(Bytes), AdapterPath, OnDone = MoveTemp(OnDone)]() mutable
		{
			FSuperSLMAdapterInspection Result = InspectAdapterBytes(
				Bytes.IsValid() ? Bytes->GetData() : nullptr, Bytes.IsValid() ? Bytes->GetSize() : 0, AdapterPath);
			Bytes.Reset();
			AsyncTask(ENamedThreads::GameThread,
				[Hold = MoveTemp(Hold), Result = MoveTemp(Result), OnDone = MoveTemp(OnDone)]() mutable
				{
					OnDone(Result);
					Hold.Reset();
				});
		});
}
