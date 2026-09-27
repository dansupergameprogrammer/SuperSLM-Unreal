// U1 R-S2i/R-S2j: real A-EX maps in one process, with the model asset's head switch
// in both positions and DXGI LOCAL occupancy sampled around the actual Layer-1 map.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMGpuTestAccess.h"
#include "Tests/AutomationCommon.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	bool ImportAEx(FAutomationTestBase& T, USuperSLMModel*& OutModel)
	{
		FString Path, Reason;
		if (!T.TestTrue(*FString::Printf(TEXT("A-EX present (%s)"), *Reason), TryGetAExArtifactPath(Path, Reason)))
		{
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		OutModel = FSuperSLMModelImport::ImportFromFile(Path, Diag);
		return T.TestNotNull(TEXT("A-EX imports"), OutModel) && T.TestTrue(TEXT("A-EX accepted"), Diag.bAccepted);
	}

	bool Configure(FAutomationTestBase& T, USuperSLMGpuSubsystem& Gpu, USuperSLMModel& Model, bool bHeadOn)
	{
		Model.bGpuDeviceResidentHead = bHeadOn;
		FSuperSLMGpuRuntimeConfig Config;
		Config.ContextCap = 4096;
		Config.BlockCount = 1;
		Config.DispatchBudget = DispatchBudgetForLayersPerSlice(AExNumHiddenLayers);
		Config.K = AExNumHiddenLayers;
		Config.TickBudgetMs = 1000.0;
		return T.TestEqual(TEXT("GPU Configure"), (uint8)Gpu.Configure(&Model, Config).Result,
			(uint8)ESuperSLMGpuConfigureResult::Success);
	}

	struct FMapReading
	{
		int64 Pre = -1;
		int64 Post = -1;
		int64 Configured = -1;
		int64 Declared = -1;
		bool bHeadActive = false;
		int64 MapDelta() const { return Post - Pre; }
	};

	FMapReading ReadMap(const USuperSLMGpuSubsystem& Gpu)
	{
		FMapReading R;
		R.Pre = Gpu.GetPreMapLocalVideoMemoryBytes();
		R.Post = Gpu.GetPostMapLocalVideoMemoryBytes();
		R.Configured = Gpu.GetLocalVideoMemoryUsageBytes();
		R.Declared = Gpu.GetDeclaredGpuResidencyBytes();
		R.bHeadActive = Gpu.IsDeviceHeadActive();
		return R;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuShaderDirectoryTest,
	"SuperSLM.U1.Gpu.PluginShaderDirectory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuShaderDirectoryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (!ImportAEx(*this, Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }
	TArray<FString> AdjacentShaders;
	IFileManager::Get().FindFiles(AdjacentShaders,
		*FPaths::Combine(FPaths::GetPath(FPlatformProcess::ExecutablePath()), TEXT("*.cso")), true, false);
	if (!TestEqual(TEXT("no compiled shader beside UnrealEditor.exe"), AdjacentShaders.Num(), 0)) { return false; }
	if (!Configure(*this, *Gpu, *Model, true)) { return false; }
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
	if (!TestTrue(TEXT("plugin registered"), Plugin.IsValid())) { return false; }
	const FString Expected = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/Win64/shaders")));
	const FString Actual = Gpu->GetShaderDirectory();
	bool bOk = TestFalse(TEXT("shader_dir is absolute"), FPaths::IsRelative(Actual));
	bOk &= TestEqual(TEXT("context shader_dir is the plugin's directory"),
		FPaths::ConvertRelativePathToFull(Actual), Expected);
	FSuperSLMGpuSequence Seq;
	bOk &= TestEqual(TEXT("vend after configure warm-up"),
		(uint8)Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::OneCall), (uint8)ESuperSLMGpuVendResult::Success);
	FSuperSLMGenerationRequest Request;
	Request.PromptTokens = {1, 2, 3};
	Request.MaxNewTokens = 32;
	Request.SpanKind = ESuperSLMSpanKind::Prompt;
	TArray<int32> Tokens;
	FString Error;
	bOk &= TestTrue(*FString::Printf(TEXT("real generation completes (%s)"), *Error),
		RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, 60.0, Error));
	bOk &= TestTrue(TEXT("generation produces tokens"), !Tokens.IsEmpty());
	Gpu->ReturnSequence(Seq);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMU1GpuDeclaredResidencyTest,
	"SuperSLM.U1.Gpu.DeclaredResidencyAgainstDxgi", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMU1GpuDeclaredResidencyTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper W;
	if (!W.CreateTestWorld(EWorldType::Game)) { return false; }
	USuperSLMModel* Model = nullptr;
	if (!ImportAEx(*this, Model)) { return false; }
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(W.GetTestWorld());
	if (!TestNotNull(TEXT("GPU subsystem"), Gpu)) { return false; }

	FSuperSLMLayer1ModelFacts Facts;
	if (!TestTrue(TEXT("independent artifact view loads"),
		FSuperSLMGpuTestAccess::LoadLayer1ModelFacts(Model->GetMappedArtifactData(),
			Model->GetMappedArtifactSize(), Facts))) { return false; }
	const int64 HeadTable = int64(Facts.VocabSize) * int64(Facts.HiddenSize);
	const int64 HeadBytes = FMath::Max<int64>(4, (HeadTable + 3) & ~int64(3))
		+ int64(Facts.HiddenSize) * 4 + int64(Facts.VocabSize) * 8;
	if (!TestEqual(TEXT("A-EX independent head size"), HeadBytes, int64(137353728))) { return false; }
	const FSuperSLMLayer1LayerLayout Layout = FSuperSLMGpuTestAccess::ComputeLayer1LayerLayout(
		Facts.HiddenSize, Facts.NumKeyValueHeads * Facts.HeadDim, Facts.NumKeyValueHeads,
		Facts.NumAttentionHeads, Facts.IntermediateSize, Facts.NumAttentionHeads * Facts.HeadDim);
	const int64 RopeBytes = (Facts.bHasRopeCos ? int64(Facts.RopeCosElemCount) * 8 : 8)
		+ (Facts.bHasRopeSin ? int64(Facts.RopeSinElemCount) * 8 : 8);
	const int64 BaseBytes = int64(Layout.Stride) * Facts.NumHiddenLayers + RopeBytes
		+ (Facts.bHasSchemaMasks ? int64(Facts.SchemaMasksByteSize) : 0);
	const int64 KvBytes = int64(Facts.NumHiddenLayers) * 4096 * Facts.NumKeyValueHeads * Facts.HeadDim * 2;

	// Both pairing orders in one process. The samples bracket Layer 1's successful map;
	// the post-configure reading includes the pooled K/V buffer and warm-up.
	FMapReading Arms[4];
	const bool Switches[4] = {false, true, true, false};
	bool bOk = true;
	for (int32 I = 0; I < 4; ++I)
	{
		if (!Configure(*this, *Gpu, *Model, Switches[I])) { return false; }
		Arms[I] = ReadMap(*Gpu);
		const FString Arm = FString::Printf(TEXT("map arm %d (%s)"), I, Switches[I] ? TEXT("on") : TEXT("off"));
		bOk &= TestTrue(*(Arm + TEXT(" DXGI samples exist")), Arms[I].Pre >= 0 && Arms[I].Post >= Arms[I].Pre);
		bOk &= TestEqual(*(Arm + TEXT(" active head matches map")), Arms[I].bHeadActive, Switches[I]);
		bOk &= TestEqual(*(Arm + TEXT(" exact declared model, head and pooled K/V")),
			Arms[I].Declared, BaseBytes + KvBytes + (Switches[I] ? HeadBytes : 0));
		bOk &= TestTrue(*(Arm + TEXT(" declared occupancy is a DXGI lower bound")),
			Arms[I].Declared <= Arms[I].Configured - Arms[I].Pre);
		AddInfo(FString::Printf(TEXT("%s: map delta %lld B, declared %lld B, occupancy since map %lld B"),
			*Arm, Arms[I].MapDelta(), Arms[I].Declared, Arms[I].Configured - Arms[I].Pre));
	}
	for (const int32 On : {1, 2})
	{
		const int32 Off = On == 1 ? 0 : 3;
		const int64 MeasuredHead = Arms[On].MapDelta() - Arms[Off].MapDelta();
		bOk &= TestTrue(*FString::Printf(TEXT("pair %d head declaration is no larger than measured head cost"), On),
			HeadBytes <= MeasuredHead);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
