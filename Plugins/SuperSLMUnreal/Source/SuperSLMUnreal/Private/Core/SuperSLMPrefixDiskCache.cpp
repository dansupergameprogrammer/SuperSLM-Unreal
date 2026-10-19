#include "SuperSLMPrefixDiskCache.h"

#include <algorithm>

namespace SuperSLMCore
{
	namespace
	{
		const char kDiskFileSuffix[] = ".slup";
		constexpr size_t kDiskFileSuffixBytes = sizeof(kDiskFileSuffix) - 1;

		std::string DiskPathOf(const std::string& Dir, const std::string& Name)
		{
			return Dir + "/" + Name;
		}
	}

	std::string PrefixDiskFileName(const FPrefixKey& Key)
	{
		return PrefixKeyToHex(Key) + kDiskFileSuffix;
	}

	std::string PrefixDiskTempFileName(const FPrefixKey& Key, uint64_t InstanceId, uint64_t WriteSerial)
	{
		return PrefixKeyToHex(Key) + "." + std::to_string(InstanceId) + "." + std::to_string(WriteSerial) + ".tmp";
	}

	bool IsPrefixDiskFileName(const std::string& Name, FPrefixKey* OutKey)
	{
		if (Name.size() != 64 + kDiskFileSuffixBytes || Name.compare(64, kDiskFileSuffixBytes, kDiskFileSuffix) != 0)
		{
			return false;
		}
		FPrefixKey Key;
		if (!PrefixKeyFromHex(Name.substr(0, 64), Key))
		{
			return false;
		}
		if (OutKey != nullptr)
		{
			*OutKey = Key;
		}
		return true;
	}

	std::vector<std::string> PlanPrefixDiskTrim(const std::vector<FPrefixDiskFileInfo>& Files, uint64_t MaxBytes)
	{
		std::vector<const FPrefixDiskFileInfo*> CacheFiles;
		uint64_t Total = 0;
		for (const FPrefixDiskFileInfo& File : Files)
		{
			if (IsPrefixDiskFileName(File.Name, nullptr))
			{
				CacheFiles.push_back(&File);
				Total += File.SizeBytes;
			}
		}
		std::sort(CacheFiles.begin(), CacheFiles.end(), [](const FPrefixDiskFileInfo* A, const FPrefixDiskFileInfo* B) {
			return A->ModifiedTime != B->ModifiedTime ? A->ModifiedTime < B->ModifiedTime : A->Name < B->Name;
		});

		std::vector<std::string> Remove;
		for (const FPrefixDiskFileInfo* File : CacheFiles)
		{
			if (Total <= MaxBytes)
			{
				break;
			}
			Remove.push_back(File->Name);
			Total -= File->SizeBytes;
		}
		return Remove;
	}

	bool WritePrefixFileAtomically(IPrefixFileOps& Files, const std::string& Dir, const FPrefixKey& Key,
		uint64_t InstanceId, uint64_t WriteSerial, const std::vector<uint8_t>& Bytes, EPrefixWriteFault Fault)
	{
		const std::string Temp = DiskPathOf(Dir, PrefixDiskTempFileName(Key, InstanceId, WriteSerial));
		const std::string Final = DiskPathOf(Dir, PrefixDiskFileName(Key));
		if (!Files.WriteWhole(Temp, Bytes.data(), Bytes.size()) || Fault == EPrefixWriteFault::BetweenWriteAndRename ||
			!Files.RenameReplace(Temp, Final))
		{
			Files.RemoveFile(Temp);
			return false;
		}
		// The trim's "oldest" is this time, set by the cache itself (§5.7). A failed touch leaves the
		// rename's time, which is still this write's.
		Files.TouchModifiedTime(Final);
		return true;
	}

	EPrefixIoStatus ReadPrefixFile(IPrefixFileOps& Files, const std::string& Dir, const FPrefixKey& Key, std::vector<uint8_t>& OutBytes)
	{
		OutBytes.clear();
		const EPrefixIoStatus Status = Files.ReadWhole(DiskPathOf(Dir, PrefixDiskFileName(Key)), OutBytes);
		if (Status != EPrefixIoStatus::Ok)
		{
			OutBytes.clear();
		}
		return Status;
	}

	uint32_t TrimPrefixDiskDirectory(IPrefixFileOps& Files, const std::string& Dir, uint64_t MaxBytes)
	{
		std::vector<FPrefixDiskFileInfo> Listed;
		if (!Files.ListDirectory(Dir, Listed))
		{
			return 0;
		}
		uint32_t Removed = 0;
		for (const std::string& Name : PlanPrefixDiskTrim(Listed, MaxBytes))
		{
			if (Files.RemoveFile(DiskPathOf(Dir, Name)))
			{
				++Removed;
			}
		}
		return Removed;
	}
}
