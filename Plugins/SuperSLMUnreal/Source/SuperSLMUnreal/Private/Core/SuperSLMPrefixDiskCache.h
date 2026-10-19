#pragma once

// The disk tier's file rules (precache plan §5.7), over an abstract file interface so they run on
// the host as well as on Unreal's platform file layer:
//   - one file per key: "<64 hex>.slup";
//   - writes go to a temporary file and are renamed into place, so a failure or crash never leaves a
//     partial file under a key name; temporary names are unique per cache instance AND per write
//     ("<64 hex>.<instance>.<write serial>.tmp"), so several instances can share one directory;
//   - a read that fails because the file vanished or is mid-rename is NotFound, which is a miss;
//   - trimming keeps the directory's cache files at or under the limit, removing the OLDEST first by
//     modification time -- which the cache sets itself on every write and every hit it serves, never
//     the file system's access time. Only "<64 hex>.slup" files are counted or removed; any other
//     file in the directory (a temporary in progress, a file the cache did not write) is left alone.
//
// All of this is pool-thread work in the plugin; none of it touches cache state.

#include "SuperSLMPrefixCacheKey.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SuperSLMCore
{
	enum class EPrefixIoStatus : uint8_t
	{
		Ok = 0,
		NotFound = 1, // no such file (vanished, never written, or mid-rename): a plain miss
		Error = 2,    // any other read or write failure: a miss (read) or no write-back (write), logged
	};

	struct FPrefixDiskFileInfo
	{
		std::string Name;          // file name only, no directory
		uint64_t SizeBytes = 0;
		int64_t ModifiedTime = 0;  // any monotonic unit the file layer uses; larger is newer
	};

	// The file layer. Paths are Dir + "/" + Name. Every call is synchronous.
	class IPrefixFileOps
	{
	public:
		virtual ~IPrefixFileOps() = default;
		virtual bool WriteWhole(const std::string& Path, const uint8_t* Data, size_t Size) = 0;
		virtual EPrefixIoStatus ReadWhole(const std::string& Path, std::vector<uint8_t>& OutData) = 0;
		// Replaces To if it exists; atomic with respect to readers.
		virtual bool RenameReplace(const std::string& From, const std::string& To) = 0;
		virtual bool RemoveFile(const std::string& Path) = 0;
		virtual bool ListDirectory(const std::string& Dir, std::vector<FPrefixDiskFileInfo>& OutFiles) = 0;
		// Sets the file's modification time to now.
		virtual bool TouchModifiedTime(const std::string& Path) = 0;
	};

	std::string PrefixDiskFileName(const FPrefixKey& Key);
	std::string PrefixDiskTempFileName(const FPrefixKey& Key, uint64_t InstanceId, uint64_t WriteSerial);
	// True for "<64 hex>.slup" exactly; OutKey (optional) receives the key.
	bool IsPrefixDiskFileName(const std::string& Name, FPrefixKey* OutKey);

	// The names to remove, in removal order: cache files only, oldest ModifiedTime first (ties by
	// name, ascending), until the remaining cache files total at most MaxBytes.
	std::vector<std::string> PlanPrefixDiskTrim(const std::vector<FPrefixDiskFileInfo>& Files, uint64_t MaxBytes);

	// Test seam for cell 5.8: a fault between the write of the temporary and its rename.
	enum class EPrefixWriteFault : uint8_t
	{
		None = 0,
		BetweenWriteAndRename = 1, // the rename never happens; the temporary is removed; returns false
	};

	// Writes Bytes to a unique temporary and renames it to the key's name, then sets its
	// modification time. False on any failure, with no file left under the key's name by this call
	// and its temporary removed.
	bool WritePrefixFileAtomically(IPrefixFileOps& Files, const std::string& Dir, const FPrefixKey& Key,
		uint64_t InstanceId, uint64_t WriteSerial, const std::vector<uint8_t>& Bytes, EPrefixWriteFault Fault);

	EPrefixIoStatus ReadPrefixFile(IPrefixFileOps& Files, const std::string& Dir, const FPrefixKey& Key, std::vector<uint8_t>& OutBytes);

	// Lists Dir, plans the trim, removes. Returns how many files it removed.
	uint32_t TrimPrefixDiskDirectory(IPrefixFileOps& Files, const std::string& Dir, uint64_t MaxBytes);
}
