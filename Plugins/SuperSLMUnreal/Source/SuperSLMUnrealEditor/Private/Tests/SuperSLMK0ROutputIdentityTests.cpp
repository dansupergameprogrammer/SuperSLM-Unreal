// Plugin paging plan, step K0R and cell 8.1's output-identity leg (coverage audit G4, G5).
//
// One harness, compiled into three trees: the pre-re-vendor build at 68a75d621 (the 1.11.0 pin), the
// re-vendored tree at 5e97b63f1 (1.13.0), and the paging release's head. Every call it makes exists
// unchanged in all three, and both subsystems run in their default (legacy CPU, dedicated GPU) mode.
//
// For each leg (backend x model) it runs four fixed prompts on fresh sequences, greedy (the plugin has
// no sampler), 32 new tokens each, no stop ids, and saves each sequence once its turn is Complete:
//   - r_s1a_<id> x3: the first three prompts of the L2-S1 reference fixture
//     (Tests/Fixtures/L2S1/l2s1_r_s1a_reference.json, cases 0..2), fed as recorded token ids;
//   - long1000: 1,000 tokens, the 20 reference prompts' token ids concatenated and repeated, cut at
//     1,000.
// It also writes the cross-pin wrapper fixture: long1000 with 8 new tokens, saved Complete
// ('SSB5' inside the wrapper on the CPU, 'SLM5' on the GPU).
//
// Two modes, chosen by the environment:
//   - SUPERSLM_K0R_RECORD=1: writes the recording under SUPERSLM_K0R_DIR/<backend>/<model>/ and
//     refuses to overwrite one that exists. Run on 68a75d621 only.
//   - otherwise: writes this build's outputs under SUPERSLM_K0R_OUT/<backend>/<model>/ and compares
//     them with the recording. Every generated token must match. Each save wrapper must match outside
//     the Layer-1 tag and commit fields (bytes 12..67), which name the pin and are expected to differ;
//     the Layer-1 payload inside it must match byte for byte.
// A missing variable, artifact or recording fails the test by name.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC

#include "Dom/JsonObject.h"
#include "Fixtures/SuperSLML2S1Fixtures.h"
#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSaveRestoreTypes.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"

namespace
{
	constexpr int32 kK0RNewTokens = 32;
	constexpr int32 kK0RLongPromptTokens = 1000;
	constexpr int32 kK0RWrapperNewTokens = 8;
	constexpr double kK0RDeadlineSeconds = 900.0;

	// The plugin's save wrapper (SuperSLMSaveBlob.h, version 2): a 156-byte fixed header, then three
	// counted arrays, then a u64-counted payload. On GPU that payload starts with the SLUG
	// extension (24 fixed bytes, 12 per carried event, a u64 Layer-1 size), then Layer 1.
	constexpr int32 kWrapperFixedBytes = 156;
	constexpr int32 kWrapperTagBegin = 12;
	constexpr int32 kWrapperTagEnd = 68;

	enum class EK0RBackend { Cpu, Gpu };
	enum class EK0RModel { Q05B, Q15B };

	const TCHAR* BackendName(EK0RBackend B) { return B == EK0RBackend::Cpu ? TEXT("cpu") : TEXT("gpu"); }
	const TCHAR* ModelName(EK0RModel M) { return M == EK0RModel::Q05B ? TEXT("0p5b") : TEXT("1p5b"); }
	int32 ModelLayers(EK0RModel M) { return M == EK0RModel::Q05B ? 24 : 28; }

	struct FK0RPrompt
	{
		FString Id;
		TArray<int32> Tokens;
	};

	struct FK0RRun
	{
		FString Id;
		int32 PromptTokens = 0;
		TArray<int32> Generated;
		TArray<uint8> Blob;
	};

	bool BuildPrompts(FAutomationTestBase& T, TArray<FK0RPrompt>& OutPrompts)
	{
		TArray<SuperSLML2S1Fixtures::FReferenceCase> Cases;
		FString Error;
		const bool bLoaded = SuperSLML2S1Fixtures::LoadReferenceCases(Cases, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("the L2-S1 reference fixture loads (%s)"), *Error), bLoaded) ||
			!T.TestTrue(TEXT("the reference fixture holds at least three cases"), Cases.Num() >= 3))
		{
			return false;
		}
		for (int32 I = 0; I < 3; ++I)
		{
			OutPrompts.Add({ FString::Printf(TEXT("r_s1a_%s"), *Cases[I].Id), Cases[I].PromptTokens });
		}
		FK0RPrompt Long;
		Long.Id = TEXT("long1000");
		while (Long.Tokens.Num() < kK0RLongPromptTokens)
		{
			for (const SuperSLML2S1Fixtures::FReferenceCase& Case : Cases)
			{
				for (const int32 Token : Case.PromptTokens)
				{
					if (Long.Tokens.Num() < kK0RLongPromptTokens)
					{
						Long.Tokens.Add(Token);
					}
				}
			}
		}
		OutPrompts.Add(MoveTemp(Long));
		return true;
	}

	FSuperSLMGenerationRequest MakeRequest(const TArray<int32>& Prompt, int32 MaxNewTokens)
	{
		FSuperSLMGenerationRequest Request;
		Request.PromptTokens = Prompt;
		Request.MaxNewTokens = MaxNewTokens;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;
		return Request;
	}

	FString ModelPath(EK0RModel M)
	{
		if (M == EK0RModel::Q15B)
		{
			return SuperSLML2S1Fixtures::AAdBaseModelPath();
		}
		FString Path, Reason;
		return SuperSLML2S1Fixtures::TryGetAExArtifactPath(Path, Reason) ? Path : Reason;
	}

	// --- The CPU leg: legacy mode (no SequenceBudget set) ---
	bool ConfigureCpu(FAutomationTestBase& T, UWorld* World, USuperSLMModel* Model, EK0RModel M)
	{
		USuperSLMSubsystem* Cpu = SuperSLML2S1Fixtures::GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem"), Cpu))
		{
			return false;
		}
		FSuperSLMRuntimeConfig Config;
		Config.BlockCount = 1;
		Config.MaxSequencesPerDecodeCall = 1;
		Config.MaxPrefillChunkBudget = 64;
		Config.MaxLayerBudget = ModelLayers(M);
		Config.SequenceLifecycleBudgetMs = 1000.0;
		Config.TickBudgetMs = 1000.0;
		const FSuperSLMConfigureReport Report = Cpu->Configure(Model, Config);
		return T.TestEqual(TEXT("CPU Configure"), (uint8)Report.Result, (uint8)ESuperSLMConfigureResult::Success);
	}

	bool RunCpu(FAutomationTestBase& T, UWorld* World, const FK0RPrompt& Prompt, int32 NewTokens, FK0RRun& Out)
	{
		USuperSLMSubsystem* Cpu = SuperSLML2S1Fixtures::GetSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMSubsystem"), Cpu))
		{
			return false;
		}
		Out.Id = Prompt.Id;
		Out.PromptTokens = Prompt.Tokens.Num();
		FSuperSLMSequence Seq;
		if (!T.TestEqual(*FString::Printf(TEXT("%s: VendSequence"), *Prompt.Id), (uint8)Cpu->VendSequence(Seq), (uint8)ESuperSLMVendResult::Success))
		{
			return false;
		}
		FString Error;
		const bool bDone = SuperSLML2S1Fixtures::RunGenerationToCompletion(*Cpu, Seq, MakeRequest(Prompt.Tokens, NewTokens), Out.Generated, kK0RDeadlineSeconds, Error);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the generation completes (%s)"), *Prompt.Id, *Error), bDone);
		if (bOk)
		{
			FSuperSLMLifecycleOpHandle Handle;
			const ESuperSLMRestoreResult Queued = Cpu->SaveSequence(Seq, Handle, Error);
			bOk = T.TestEqual(*FString::Printf(TEXT("%s: SaveSequence queues (%s)"), *Prompt.Id, *Error), (uint8)Queued, (uint8)ESuperSLMRestoreResult::Success);
			if (bOk)
			{
				ESuperSLMRestoreResult Saved = ESuperSLMRestoreResult::Pending;
				SuperSLML2S1Fixtures::DrainTicks(*Cpu, 1.0f / 60.0f, EL2S1DrainPacing::FastAsPossible,
					[Cpu, &Handle, &Out, &Saved]() { Saved = Cpu->GetSaveResult(Handle, Out.Blob); return Saved != ESuperSLMRestoreResult::Pending; },
					kK0RDeadlineSeconds);
				bOk = T.TestEqual(*FString::Printf(TEXT("%s: the save resolves Success"), *Prompt.Id), (uint8)Saved, (uint8)ESuperSLMRestoreResult::Success);
			}
		}
		Cpu->ReturnSequence(Seq);
		return bOk;
	}

	// --- The GPU leg: dedicated mode (no SequenceBudget set), the one-call path, a whole token per tick ---
	bool ConfigureGpu(FAutomationTestBase& T, UWorld* World, USuperSLMModel* Model, EK0RModel M)
	{
		USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem"), Gpu))
		{
			return false;
		}
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = 4096;
		Config.BlockCount = 1;
		Config.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(ModelLayers(M));
		Config.K = ModelLayers(M);
		Config.TickBudgetMs = 1000.0;
		const FSuperSLMGpuConfigureReport Report = Gpu->Configure(Model, Config);
		return T.TestEqual(*FString::Printf(TEXT("GPU Configure (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success);
	}

	bool RunGpu(FAutomationTestBase& T, UWorld* World, const FK0RPrompt& Prompt, int32 NewTokens, FK0RRun& Out)
	{
		USuperSLMGpuSubsystem* Gpu = SuperSLML2S2Fixtures::GetGpuSubsystem(World);
		if (!T.TestNotNull(TEXT("USuperSLMGpuSubsystem"), Gpu))
		{
			return false;
		}
		Out.Id = Prompt.Id;
		Out.PromptTokens = Prompt.Tokens.Num();
		FSuperSLMGpuSequence Seq;
		const double Start = FPlatformTime::Seconds();
		ESuperSLMGpuVendResult Vend = Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
		while (Vend == ESuperSLMGpuVendResult::PoolExhausted && FPlatformTime::Seconds() - Start < 120.0 && SuperSLML2S2Fixtures::TickWhenDeviceReady(*Gpu))
		{
			Vend = Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall);
		}
		if (!T.TestEqual(*FString::Printf(TEXT("%s: GPU VendSequence"), *Prompt.Id), (uint8)Vend, (uint8)ESuperSLMGpuVendResult::Success))
		{
			return false;
		}
		FString Error;
		const bool bDone = SuperSLML2S2Fixtures::RunGpuGenerationToCompletion(*Gpu, Seq, MakeRequest(Prompt.Tokens, NewTokens), Out.Generated, kK0RDeadlineSeconds, Error);
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the GPU generation completes (%s)"), *Prompt.Id, *Error), bDone);
		if (bOk)
		{
			const FSuperSLMLifecycleOpHandle Handle = Gpu->RequestSaveSequence(Seq);
			bOk = T.TestTrue(*FString::Printf(TEXT("%s: RequestSaveSequence queues (%s)"), *Prompt.Id, *Gpu->GetLastLifecycleRequestError()), Handle.IsValid());
			if (bOk)
			{
				const ESuperSLMRestoreResult Saved = SuperSLML2S2Fixtures::DriveSaveToResolution(*Gpu, Handle, Out.Blob, kK0RDeadlineSeconds);
				bOk = T.TestEqual(*FString::Printf(TEXT("%s: the GPU save resolves Success"), *Prompt.Id), (uint8)Saved, (uint8)ESuperSLMRestoreResult::Success);
			}
		}
		Gpu->ReturnSequence(Seq);
		return bOk;
	}

	// --- The wrapper, parsed only as far as the comparison needs ---
	struct FWrapperView
	{
		int64 PayloadOffset = 0;
		int64 PayloadBytes = 0;
		FString Layer1Tag;
		FString PayloadMagic;
	};

	FString AsciiField(const uint8* Bytes, int32 Count)
	{
		FString Out;
		for (int32 I = 0; I < Count && Bytes[I] != 0; ++I)
		{
			Out.AppendChar(TCHAR(Bytes[I]));
		}
		return Out;
	}

	bool ReadI32(const TArray<uint8>& Blob, int64 At, int32& OutValue)
	{
		if (At < 0 || At + 4 > Blob.Num())
		{
			return false;
		}
		FMemory::Memcpy(&OutValue, Blob.GetData() + At, 4);
		return true;
	}

	bool ParseWrapper(const TArray<uint8>& Blob, FWrapperView& Out, FString& OutError)
	{
		if (Blob.Num() < kWrapperFixedBytes || FMemory::Memcmp(Blob.GetData(), "SLUW", 4) != 0)
		{
			OutError = TEXT("not a plugin save wrapper ('SLUW')");
			return false;
		}
		int32 Version = 0;
		ReadI32(Blob, 4, Version);
		if (Version != 2)
		{
			OutError = FString::Printf(TEXT("wrapper version %d, the harness reads version 2"), Version);
			return false;
		}
		Out.Layer1Tag = AsciiField(Blob.GetData() + kWrapperTagBegin, 16);
		int64 At = kWrapperFixedBytes;
		const int32 ElementBytes[3] = { 4, 4, 1 };
		for (const int32 Bytes : ElementBytes)
		{
			int32 Count = 0;
			if (!ReadI32(Blob, At, Count) || Count < 0)
			{
				OutError = TEXT("truncated counted array");
				return false;
			}
			At += 4 + int64(Count) * Bytes;
		}
		if (At + 8 > Blob.Num())
		{
			OutError = TEXT("truncated payload count");
			return false;
		}
		uint64 PayloadBytes = 0;
		FMemory::Memcpy(&PayloadBytes, Blob.GetData() + At, 8);
		At += 8;
		if (At + int64(PayloadBytes) != int64(Blob.Num()))
		{
			OutError = FString::Printf(TEXT("a payload of %llu bytes at %lld does not end the %d-byte blob"), PayloadBytes, At, Blob.Num());
			return false;
		}
		if (Blob[8] == static_cast<uint8>(ESuperSLMBackend::GPU))
		{
			int32 ExtensionVersion = 0, CarriedCount = 0;
			if (PayloadBytes < 32 || FMemory::Memcmp(Blob.GetData() + At, "SLUG", 4) != 0 ||
				!ReadI32(Blob, At + 4, ExtensionVersion) || ExtensionVersion != 1 ||
				!ReadI32(Blob, At + 20, CarriedCount) || CarriedCount < 0 ||
				uint64(CarriedCount) > (PayloadBytes - 32) / 12)
			{
				OutError = TEXT("malformed GPU save extension ('SLUG' version 1)");
				return false;
			}
			At += 24 + int64(CarriedCount) * 12;
			FMemory::Memcpy(&PayloadBytes, Blob.GetData() + At, 8);
			At += 8;
			if (PayloadBytes != uint64(Blob.Num() - At))
			{
				OutError = TEXT("the GPU extension's Layer-1 size does not end the blob");
				return false;
			}
		}
		Out.PayloadOffset = At;
		Out.PayloadBytes = int64(PayloadBytes);
		Out.PayloadMagic = AsciiField(Blob.GetData() + At, PayloadBytes >= 4 ? 4 : 0);
		return true;
	}

	// The byte ranges [Begin, End) where A and B differ over Bytes; the first 16 ranges are listed.
	FString DescribeDiffs(const uint8* A, const uint8* B, int64 Bytes, int64 BaseOffset, int64& OutDiffBytes)
	{
		OutDiffBytes = 0;
		FString Ranges;
		int32 RangesListed = 0;
		for (int64 I = 0; I < Bytes;)
		{
			if (A[I] == B[I])
			{
				++I;
				continue;
			}
			const int64 Begin = I;
			while (I < Bytes && A[I] != B[I])
			{
				++I;
			}
			OutDiffBytes += I - Begin;
			if (RangesListed++ < 16)
			{
				Ranges += FString::Printf(TEXT(" [%lld,%lld)"), BaseOffset + Begin, BaseOffset + I);
			}
		}
		return Ranges;
	}

	FString TokensJson(const TArray<FK0RRun>& Runs, const FString& Layer1Tag)
	{
		FString Json = FString::Printf(TEXT("{\n  \"layer1_tag\": \"%s\",\n  \"new_tokens\": %d,\n  \"runs\": [\n"), *Layer1Tag, kK0RNewTokens);
		for (int32 R = 0; R < Runs.Num(); ++R)
		{
			TArray<FString> Ids;
			for (const int32 Token : Runs[R].Generated)
			{
				Ids.Add(FString::FromInt(Token));
			}
			Json += FString::Printf(TEXT("    { \"id\": \"%s\", \"prompt_tokens\": %d, \"generated\": [%s] }%s\n"),
				*Runs[R].Id, Runs[R].PromptTokens, *FString::Join(Ids, TEXT(",")), R + 1 < Runs.Num() ? TEXT(",") : TEXT(""));
		}
		return Json + TEXT("  ]\n}\n");
	}

	bool ReadRecordedTokens(FAutomationTestBase& T, const FString& Path, TMap<FString, TArray<int32>>& Out)
	{
		FString Text;
		if (!T.TestTrue(*FString::Printf(TEXT("the recording's tokens.json is readable (%s)"), *Path), FFileHelper::LoadFileToString(Text, *Path)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid();
		if (!T.TestTrue(TEXT("tokens.json parses"), bParsed))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RunValue : Root->GetArrayField(TEXT("runs")))
		{
			const TSharedPtr<FJsonObject> Run = RunValue->AsObject();
			TArray<int32>& Ids = Out.Add(Run->GetStringField(TEXT("id")));
			for (const TSharedPtr<FJsonValue>& Id : Run->GetArrayField(TEXT("generated")))
			{
				Ids.Add(int32(Id->AsNumber()));
			}
		}
		return true;
	}

	bool CompareBlob(FAutomationTestBase& T, const FString& What, const TArray<uint8>& Recorded, const TArray<uint8>& Current)
	{
		FWrapperView RecView, CurView;
		FString RecError, CurError;
		const bool bRecParsed = ParseWrapper(Recorded, RecView, RecError);
		const bool bCurParsed = ParseWrapper(Current, CurView, CurError);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the recorded wrapper parses (%s)"), *What, *RecError), bRecParsed) ||
			!T.TestTrue(*FString::Printf(TEXT("%s: this build's wrapper parses (%s)"), *What, *CurError), bCurParsed))
		{
			return false;
		}
		T.AddInfo(FString::Printf(TEXT("%s: Layer-1 tag recorded '%s', this build '%s'; payload '%s' %lld bytes recorded, '%s' %lld bytes this build"),
			*What, *RecView.Layer1Tag, *CurView.Layer1Tag, *RecView.PayloadMagic, RecView.PayloadBytes, *CurView.PayloadMagic, CurView.PayloadBytes));
		int64 HeaderDiff = 0, FieldDiff = 0, PayloadDiff = 0;
		const FString HeaderRanges = DescribeDiffs(Recorded.GetData(), Current.GetData(), kWrapperTagBegin, 0, HeaderDiff);
		const FString FieldRanges = DescribeDiffs(Recorded.GetData() + kWrapperTagEnd, Current.GetData() + kWrapperTagEnd,
			FMath::Min(RecView.PayloadOffset, CurView.PayloadOffset) - kWrapperTagEnd, kWrapperTagEnd, FieldDiff);
		const FString PayloadRanges = DescribeDiffs(Recorded.GetData() + RecView.PayloadOffset, Current.GetData() + CurView.PayloadOffset,
			FMath::Min(RecView.PayloadBytes, CurView.PayloadBytes), 0, PayloadDiff);
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: wrapper bytes outside the Layer-1 tag and commit that differ (at%s)"), *What, *(HeaderRanges + FieldRanges)), HeaderDiff + FieldDiff, int64(0));
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the offset the Layer-1 payload starts at"), *What), CurView.PayloadOffset, RecView.PayloadOffset);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the Layer-1 payload length"), *What), CurView.PayloadBytes, RecView.PayloadBytes);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: Layer-1 payload bytes that differ (at payload offsets%s)"), *What, *PayloadRanges), PayloadDiff, int64(0));
		return bOk;
	}

	bool RunK0RLeg(FAutomationTestBase& T, EK0RBackend Backend, EK0RModel M)
	{
		const bool bRecord = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_K0R_RECORD")) == TEXT("1");
		const FString RecordRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_K0R_DIR"));
		const FString OutRoot = bRecord ? RecordRoot : FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_K0R_OUT"));
		if (!T.TestFalse(TEXT("SUPERSLM_K0R_DIR is set"), RecordRoot.IsEmpty()) ||
			!T.TestFalse(TEXT("SUPERSLM_K0R_OUT is set (compare mode)"), OutRoot.IsEmpty()))
		{
			return false;
		}
		const FString Leg = FString(BackendName(Backend)) / ModelName(M);
		const FString RecordDir = RecordRoot / Leg;
		const FString OutDir = OutRoot / Leg;
		if (bRecord && !T.TestFalse(*FString::Printf(TEXT("record mode: no recording exists yet at %s"), *RecordDir),
				IFileManager::Get().FileExists(*(RecordDir / TEXT("tokens.json")))))
		{
			return false;
		}

		TArray<FK0RPrompt> Prompts;
		if (!BuildPrompts(T, Prompts))
		{
			return false;
		}
		FTestWorldWrapper World;
		if (!World.CreateTestWorld(EWorldType::Game))
		{
			return false;
		}
		const FString Path = ModelPath(M);
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		if (!T.TestNotNull(*FString::Printf(TEXT("the model imports (%s)"), *Path), Model) || !T.TestTrue(TEXT("the model is accepted"), Diag.bAccepted))
		{
			return false;
		}
		UWorld* W = World.GetTestWorld();
		const bool bConfigured = Backend == EK0RBackend::Cpu ? ConfigureCpu(T, W, Model, M) : ConfigureGpu(T, W, Model, M);
		if (!bConfigured)
		{
			return false;
		}

		bool bOk = true;
		TArray<FK0RRun> Runs;
		for (const FK0RPrompt& Prompt : Prompts)
		{
			FK0RRun& Run = Runs.AddDefaulted_GetRef();
			bOk &= Backend == EK0RBackend::Cpu ? RunCpu(T, W, Prompt, kK0RNewTokens, Run) : RunGpu(T, W, Prompt, kK0RNewTokens, Run);
		}
		FK0RRun Wrapper;
		bOk &= Backend == EK0RBackend::Cpu ? RunCpu(T, W, Prompts.Last(), kK0RWrapperNewTokens, Wrapper) : RunGpu(T, W, Prompts.Last(), kK0RWrapperNewTokens, Wrapper);
		if (!bOk)
		{
			return false;
		}

		FWrapperView View;
		FString Error;
		const bool bParsed = ParseWrapper(Wrapper.Blob, View, Error);
		if (!T.TestTrue(*FString::Printf(TEXT("the wrapper fixture parses (%s)"), *Error), bParsed))
		{
			return false;
		}
		const FString WrapperName = FString::Printf(TEXT("wrapper-%s.bin"), *View.PayloadMagic.ToLower());
		bOk &= T.TestEqual(TEXT("the wrapper fixture's payload magic"), View.PayloadMagic, FString(Backend == EK0RBackend::Cpu ? TEXT("SSB5") : TEXT("SLM5")));
		bOk &= T.TestTrue(*FString::Printf(TEXT("tokens.json is written to %s"), *OutDir), FFileHelper::SaveStringToFile(TokensJson(Runs, View.Layer1Tag), *(OutDir / TEXT("tokens.json"))));
		for (const FK0RRun& Run : Runs)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s.bin is written"), *Run.Id), FFileHelper::SaveArrayToFile(Run.Blob, *(OutDir / (Run.Id + TEXT(".bin")))));
		}
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s is written"), *WrapperName), FFileHelper::SaveArrayToFile(Wrapper.Blob, *(OutDir / WrapperName)));
		T.AddInfo(FString::Printf(TEXT("K0R %s: %s, Layer-1 tag '%s', outputs in %s"), *Leg, bRecord ? TEXT("recorded") : TEXT("compared"), *View.Layer1Tag, *OutDir));
		if (bRecord || !bOk)
		{
			return bOk;
		}

		TMap<FString, TArray<int32>> Recorded;
		if (!ReadRecordedTokens(T, RecordDir / TEXT("tokens.json"), Recorded))
		{
			return false;
		}
		for (const FK0RRun& Run : Runs)
		{
			const TArray<int32>* Expected = Recorded.Find(Run.Id);
			bOk &= T.TestNotNull(*FString::Printf(TEXT("%s is in the recording"), *Run.Id), Expected);
			if (Expected)
			{
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: generated tokens equal the recording"), *Run.Id), Run.Generated, *Expected);
			}
			TArray<uint8> RecordedBlob;
			const bool bHaveBlob = FFileHelper::LoadFileToArray(RecordedBlob, *(RecordDir / (Run.Id + TEXT(".bin"))));
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s.bin is in the recording"), *Run.Id), bHaveBlob) && CompareBlob(T, Run.Id, RecordedBlob, Run.Blob);
		}
		TArray<uint8> RecordedWrapper;
		const bool bHaveWrapper = FFileHelper::LoadFileToArray(RecordedWrapper, *(RecordDir / WrapperName));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s is in the recording"), *WrapperName), bHaveWrapper) && CompareBlob(T, WrapperName, RecordedWrapper, Wrapper.Blob);
		return bOk;
	}
}

#define SUPERSLM_K0R_LEG(Class, Name, Backend, Model) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "SuperSLM.K0R.OutputIdentity." Name, EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter) \
	bool Class::RunTest(const FString& Parameters) { return RunK0RLeg(*this, Backend, Model); }

SUPERSLM_K0R_LEG(FSuperSLMK0RCpu05BTest, "Cpu.0p5B", EK0RBackend::Cpu, EK0RModel::Q05B)
SUPERSLM_K0R_LEG(FSuperSLMK0RCpu15BTest, "Cpu.1p5B", EK0RBackend::Cpu, EK0RModel::Q15B)
SUPERSLM_K0R_LEG(FSuperSLMK0RGpu05BTest, "Gpu.0p5B", EK0RBackend::Gpu, EK0RModel::Q05B)
SUPERSLM_K0R_LEG(FSuperSLMK0RGpu15BTest, "Gpu.1p5B", EK0RBackend::Gpu, EK0RModel::Q15B)

#undef SUPERSLM_K0R_LEG

#endif // SUPERSLM_WITH_L2S1_ASYNC

#endif // WITH_DEV_AUTOMATION_TESTS
