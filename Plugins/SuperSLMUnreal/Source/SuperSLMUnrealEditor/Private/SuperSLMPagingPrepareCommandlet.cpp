#include "SuperSLMPagingPrepareCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMPagingPrepare, Log, All);

USuperSLMPagingPrepareCommandlet::USuperSLMPagingPrepareCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 USuperSLMPagingPrepareCommandlet::Main(const FString& Params)
{
	FString Artifacts[2];
	FParse::Value(*Params, TEXT("Artifact05="), Artifacts[0]);
	FParse::Value(*Params, TEXT("Artifact15="), Artifacts[1]);
	const TCHAR* PackageNames[] = {
		TEXT("/Game/SuperSLMExample/Certification/Model05"),
		TEXT("/Game/SuperSLMExample/Certification/Model15")
	};
	// Check both destinations before creating either asset.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageNames[Index], FPackageName::GetAssetPackageExtension());
		if (Artifacts[Index].IsEmpty() || !IFileManager::Get().FileExists(*Artifacts[Index]) || IFileManager::Get().FileExists(*Filename))
		{
			UE_LOG(LogSuperSLMPagingPrepare, Error, TEXT("Require two existing artifacts and fresh model destinations; source='%s', destination='%s'"), *Artifacts[Index], *Filename);
			return 1;
		}
	}
	TArray<TSharedPtr<FJsonValue>> Models;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FString PackageName(PackageNames[Index]);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UPackage* Package = CreatePackage(*PackageName);
		FSuperSLMImportDiagnostic Diagnostic;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Artifacts[Index], Package, FName(*AssetName), Diagnostic);
		if (!Model || !Diagnostic.bAccepted || !Model->GetMappedArtifactData() || Model->GetMappedArtifactSize() < 64)
		{
			UE_LOG(LogSuperSLMPagingPrepare, Error, TEXT("Real model import failed: %s"), *Diagnostic.Message);
			return 1;
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)) { return 1; }
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Model, *Filename, SaveArgs))
		{
			UE_LOG(LogSuperSLMPagingPrepare, Error, TEXT("Could not save %s"), *Filename);
			return 1;
		}
		FAssetRegistryModule::AssetCreated(Model);
		const uint8* Bytes = static_cast<const uint8*>(Model->GetMappedArtifactData());
		FString Hash;
		for (int32 Byte = 32; Byte < 64; ++Byte) { Hash += FString::Printf(TEXT("%02x"), Bytes[Byte]); }
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("source_artifact"), FPaths::ConvertRelativePathToFull(Artifacts[Index]));
		Entry->SetStringField(TEXT("artifact_hash"), Hash);
		Entry->SetStringField(TEXT("asset_path"), PackageName + TEXT(".") + AssetName);
		Entry->SetNumberField(TEXT("artifact_bytes"), Model->GetMappedArtifactSize());
		Models.Add(MakeShared<FJsonValueObject>(Entry));
		UE_LOG(LogSuperSLMPagingPrepare, Display, TEXT("Prepared real model %s, hash %s"), *PackageName, *Hash);
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("models"), Models);
	FString Json;
	const FString Output = FPaths::ProjectSavedDir() / TEXT("SuperSLMPagingCertification/prepared-models.json");
	if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json))
		|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true)
		|| !FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return 1; }
	return 0;
}
