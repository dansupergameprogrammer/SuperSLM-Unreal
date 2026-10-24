// See SuperSLMBuildPrefixesCommandlet.h.

#include "SuperSLMBuildPrefixesCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMBuildPrefixes, Log, All);

USuperSLMBuildPrefixesCommandlet::USuperSLMBuildPrefixesCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 USuperSLMBuildPrefixesCommandlet::Main(const FString& Params)
{
	TArray<FString> Paths;
	FString SetsArg;
	if (FParse::Value(*Params, TEXT("Sets="), SetsArg, /*bShouldStopOnSeparator*/ false))
	{
		SetsArg.ParseIntoArray(Paths, TEXT(","), true);
	}
	if (FParse::Param(*Params, TEXT("All")))
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(USuperSLMPrefixSet::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);
		for (const FAssetData& Asset : Assets)
		{
			Paths.AddUnique(Asset.GetObjectPathString());
		}
	}
	if (Paths.Num() == 0)
	{
		UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("Name the sets to build: -Sets=/Game/Path.Asset[,...] or -All"));
		return 2;
	}

	int32 Refused = 0;
	for (const FString& Path : Paths)
	{
		USuperSLMPrefixSet* Set = LoadObject<USuperSLMPrefixSet>(nullptr, *Path);
		if (Set == nullptr)
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s: not a SuperSLM prefix set"), *Path);
			++Refused;
			continue;
		}
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		const bool bBuilt = SuperSLMPrefixSetTools::BuildCpu(*Set, Findings, Error);
		SuperSLMPrefixSetEditor::LogFindings(Findings);
		if (!bBuilt)
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s"), *Error);
			++Refused;
			continue;
		}
		UPackage* Package = Set->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Set, *Filename, SaveArgs))
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s: could not save %s"), *Path, *Filename);
			++Refused;
			continue;
		}
		UE_LOG(LogSuperSLMBuildPrefixes, Display, TEXT("%s: built and saved (%d finding(s))"), *Path, Findings.Num());
	}
	return Refused == 0 ? 0 : 1;
}
