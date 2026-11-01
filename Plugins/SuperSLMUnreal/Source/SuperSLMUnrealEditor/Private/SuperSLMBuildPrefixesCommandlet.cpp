// See SuperSLMBuildPrefixesCommandlet.h.

#include "SuperSLMBuildPrefixesCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CString.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "SuperSLMPrefixSetTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMBuildPrefixes, Log, All);

namespace
{
	// "0x1002:0x744C" (the 0x optional) to a vendor and device id. False for anything else.
	bool ParseGpuDevice(const FString& Text, uint32& OutVendorId, uint32& OutDeviceId)
	{
		FString Vendor;
		FString Device;
		if (!Text.Split(TEXT(":"), &Vendor, &Device))
		{
			return false;
		}
		auto ParseHex = [](FString Part, uint32& Out) -> bool
		{
			Part.TrimStartAndEndInline();
			Part.RemoveFromStart(TEXT("0x"), ESearchCase::IgnoreCase);
			if (Part.IsEmpty() || Part.Len() > 8)
			{
				return false;
			}
			for (const TCHAR C : Part)
			{
				if (!FChar::IsHexDigit(C))
				{
					return false;
				}
			}
			Out = static_cast<uint32>(FCString::Strtoui64(*Part, nullptr, 16));
			return true;
		};
		return ParseHex(Vendor, OutVendorId) && ParseHex(Device, OutDeviceId);
	}
}

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
	const bool bNoGpu = FParse::Param(*Params, TEXT("NoGpu"));
	bool bRequireGpu = FParse::Param(*Params, TEXT("RequireGpu"));
	FString ExpectArg;
	const bool bExpectDevice = FParse::Value(*Params, TEXT("ExpectGpuDevice="), ExpectArg);
	uint32 ExpectVendor = 0;
	uint32 ExpectDevice = 0;
	if (bExpectDevice && !ParseGpuDevice(ExpectArg, ExpectVendor, ExpectDevice))
	{
		UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("-ExpectGpuDevice=%s: give the PCI vendor and device in hex, as -ExpectGpuDevice=0x1002:0x744C"), *ExpectArg);
		return 2;
	}
	bRequireGpu |= bExpectDevice;
	if (bNoGpu && bRequireGpu)
	{
		UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("-NoGpu cannot be given with -RequireGpu or -ExpectGpuDevice"));
		return 2;
	}

	int32 Refused = 0;
	int32 GpuMissing = 0;
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
		// Both halves begin from the same set (the action's order): the CPU job, then the GPU leg on a transient game
		// instance's GPU subsystem.
		TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> CpuJob = SuperSLMPrefixSetTools::BeginBuildCpu(*Set, Error);
		if (!CpuJob.IsValid())
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s"), *Error);
			++Refused;
			continue;
		}
		FString GpuReason = TEXT("-NoGpu was given");
		TUniquePtr<SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg> GpuLeg;
		if (!bNoGpu)
		{
			GpuLeg = SuperSLMPrefixSetEditor::FSuperSLMPrefixSetGpuLeg::Start(*Set, GpuReason);
			if (GpuLeg.IsValid() && bExpectDevice)
			{
				uint32 Vendor = 0;
				uint32 Device = 0;
				GpuLeg->GetDevice(Vendor, Device);
				if (Vendor != ExpectVendor || Device != ExpectDevice)
				{
					GpuReason = FString::Printf(TEXT("this machine's GPU is 0x%04X:0x%04X, not the expected 0x%04X:0x%04X (-ExpectGpuDevice)"), Vendor, Device, ExpectVendor, ExpectDevice);
					GpuLeg.Reset();
				}
			}
		}
		// The CPU entries here, then the GPU's, ticked until done (each GPU call runs on its submission thread).
		SuperSLMPrefixSetTools::RunBuildEntries(*CpuJob);
		if (GpuLeg.IsValid())
		{
			while (GpuLeg->Pump())
			{
				FPlatformProcess::Sleep(0.0005f);
			}
		}
		bool bGpuStored = false;
		const SuperSLMPrefixSetTools::EBuildFinish Finished = SuperSLMPrefixSetTools::FinishBuildCpuAndGpu(*Set, *CpuJob,
			GpuLeg.IsValid() ? &GpuLeg->GetJob() : nullptr, GpuReason, Findings, Error, bGpuStored);
		uint32 BuiltVendor = 0;
		uint32 BuiltDevice = 0;
		if (GpuLeg.IsValid())
		{
			GpuLeg->GetDevice(BuiltVendor, BuiltDevice);
		}
		GpuLeg.Reset(); // the transient game instance is torn down before the package is saved
		SuperSLMPrefixSetEditor::LogFindings(Findings);
		if (Finished != SuperSLMPrefixSetTools::EBuildFinish::Stored)
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s"), *Error);
			++Refused;
			continue;
		}
		if (bGpuStored)
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Display, TEXT("%s: GPU entries built for 0x%04X:0x%04X"), *Path, BuiltVendor, BuiltDevice);
		}
		else if (bRequireGpu)
		{
			UE_LOG(LogSuperSLMBuildPrefixes, Error, TEXT("%s: the GPU entries were not built (-RequireGpu): %s"), *Path, *GpuReason);
			++GpuMissing;
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
	return Refused == 0 && GpuMissing == 0 ? 0 : 1;
}
