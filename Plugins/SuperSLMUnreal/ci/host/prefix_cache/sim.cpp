#include "sim.h"

#include <algorithm>

namespace HostSim
{
	// --- FMemFs ----------------------------------------------------------------------------------

	bool FMemFs::WriteWhole(const std::string& Path, const uint8_t* Data, size_t Size)
	{
		if (bFailWrites)
		{
			return false;
		}
		FFile& File = Files[Path];
		File.Data.assign(Data, Data + Size);
		File.ModifiedTime = ++Clock;
		return true;
	}

	EPrefixIoStatus FMemFs::ReadWhole(const std::string& Path, std::vector<uint8_t>& OutData)
	{
		OutData.clear();
		if (bFailReads)
		{
			return EPrefixIoStatus::Error;
		}
		const auto It = Files.find(Path);
		if (It == Files.end())
		{
			return EPrefixIoStatus::NotFound;
		}
		OutData = It->second.Data;
		return EPrefixIoStatus::Ok;
	}

	bool FMemFs::RenameReplace(const std::string& From, const std::string& To)
	{
		const auto It = Files.find(From);
		if (It == Files.end())
		{
			return false;
		}
		FFile Moved = It->second;
		Files.erase(It);
		Files[To] = Moved;
		return true;
	}

	bool FMemFs::RemoveFile(const std::string& Path)
	{
		return Files.erase(Path) != 0;
	}

	bool FMemFs::ListDirectory(const std::string& Dir, std::vector<FPrefixDiskFileInfo>& OutFiles)
	{
		OutFiles.clear();
		const std::string Prefix = Dir + "/";
		for (const auto& Pair : Files)
		{
			if (Pair.first.compare(0, Prefix.size(), Prefix) == 0 && Pair.first.find('/', Prefix.size()) == std::string::npos)
			{
				FPrefixDiskFileInfo Info;
				Info.Name = Pair.first.substr(Prefix.size());
				Info.SizeBytes = Pair.second.Data.size();
				Info.ModifiedTime = Pair.second.ModifiedTime;
				OutFiles.push_back(Info);
			}
		}
		return true;
	}

	bool FMemFs::TouchModifiedTime(const std::string& Path)
	{
		const auto It = Files.find(Path);
		if (It == Files.end())
		{
			return false;
		}
		It->second.ModifiedTime = ++Clock;
		return true;
	}

	std::vector<std::string> FMemFs::NamesIn(const std::string& Dir) const
	{
		std::vector<std::string> Names;
		const std::string Prefix = Dir + "/";
		for (const auto& Pair : Files)
		{
			if (Pair.first.compare(0, Prefix.size(), Prefix) == 0)
			{
				Names.push_back(Pair.first.substr(Prefix.size()));
			}
		}
		return Names;
	}

	// --- Fake payload ----------------------------------------------------------------------------

	namespace
	{
		void PutU32(std::vector<uint8_t>& B, uint32_t V)
		{
			for (int I = 0; I < 4; ++I)
			{
				B.push_back(static_cast<uint8_t>(V >> (8 * I)));
			}
		}
		uint32_t GetU32(const std::vector<uint8_t>& B, size_t At)
		{
			uint32_t V = 0;
			for (size_t I = 0; I < 4; ++I)
			{
				V |= static_cast<uint32_t>(B[At + I]) << (8 * I);
			}
			return V;
		}
		uint32_t Fnv1a(const uint8_t* Data, size_t Size)
		{
			uint32_t H = 2166136261u;
			for (size_t I = 0; I < Size; ++I)
			{
				H ^= Data[I];
				H *= 16777619u;
			}
			return H;
		}
		uint64_t CeilDiv(uint64_t A, uint64_t B) { return (A + B - 1) / B; }
	}

	std::vector<uint8_t> MakeFakePayload(const std::string& Schema, const std::vector<int32_t>& Tokens)
	{
		std::vector<uint8_t> B = {'S', 'P', 'X', '1'};
		PutU32(B, static_cast<uint32_t>(Schema.size()));
		B.insert(B.end(), Schema.begin(), Schema.end());
		PutU32(B, static_cast<uint32_t>(Tokens.size()));
		for (int32_t T : Tokens)
		{
			PutU32(B, static_cast<uint32_t>(T));
		}
		PutU32(B, Fnv1a(B.data(), B.size()));
		return B;
	}

	bool ParseFakePayload(const std::vector<uint8_t>& P, std::string& OutSchema, std::vector<int32_t>& OutTokens)
	{
		OutSchema.clear();
		OutTokens.clear();
		if (P.size() < 16 || P[0] != 'S' || P[1] != 'P' || P[2] != 'X' || P[3] != '1')
		{
			return false;
		}
		if (Fnv1a(P.data(), P.size() - 4) != GetU32(P, P.size() - 4))
		{
			return false;
		}
		size_t At = 4;
		const uint32_t SchemaLen = GetU32(P, At);
		At += 4;
		if (At + SchemaLen + 8 > P.size())
		{
			return false;
		}
		OutSchema.assign(P.begin() + static_cast<long>(At), P.begin() + static_cast<long>(At + SchemaLen));
		At += SchemaLen;
		const uint32_t Count = GetU32(P, At);
		At += 4;
		if (At + 4ull * Count + 4 != P.size())
		{
			return false;
		}
		for (uint32_t I = 0; I < Count; ++I)
		{
			OutTokens.push_back(static_cast<int32_t>(GetU32(P, At)));
			At += 4;
		}
		return true;
	}

	// --- FSim ------------------------------------------------------------------------------------

	FSim::FSim(FSuperSLMPrefixCacheCore& InCache, const FPrefixCacheConfig& InConfig, uint64_t InPoolPages)
		: Cache(InCache), Config(InConfig), Fs(&OwnFs), PoolPages(InPoolPages)
	{
	}

	uint64_t FSim::PagesFor(int32_t Length) const
	{
		return CeilDiv(static_cast<uint64_t>(Length), Config.PageTokens);
	}

	uint64_t FSim::BuildPagesFor(int32_t Length) const
	{
		const uint64_t CapPages = CeilDiv(static_cast<uint64_t>(Config.ContextCap), Config.PageTokens);
		return std::min(PagesFor(Length) + 1, CapPages);
	}

	std::string FSim::DiskPath(const FPrefixKey& Key) const
	{
		return Config.DiskDirectory + "/" + PrefixDiskFileName(Key);
	}

	uint64_t FSim::StorePayload(std::vector<uint8_t> Bytes)
	{
		const uint64_t Ref = NextPayloadRef++;
		Payloads[Ref] = std::move(Bytes);
		return Ref;
	}

	void FSim::Pull()
	{
		for (FPrefixCommand& Command : Cache.TakeCommands())
		{
			History.push_back(Command);
			Queue.push_back(Command);
		}
	}

	bool FSim::RunOne()
	{
		if (bAutoPull)
		{
			Pull();
		}
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (Held.count(It->Kind) == 0)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				Cache.Apply(Execute(Command));
				if (bAutoPull)
				{
					Pull();
				}
				return true;
			}
		}
		return false;
	}

	bool FSim::RunOneOfKind(EPrefixCommandKind Kind)
	{
		if (bAutoPull)
		{
			Pull();
		}
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == Kind)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				Cache.Apply(Execute(Command));
				if (bAutoPull)
				{
					Pull();
				}
				return true;
			}
		}
		return false;
	}

	int FSim::RunAll(int MaxSteps)
	{
		int Steps = 0;
		while (Steps < MaxSteps && RunOne())
		{
			++Steps;
		}
		return Steps;
	}

	size_t FSim::CountHistory(EPrefixCommandKind Kind) const
	{
		return static_cast<size_t>(std::count_if(History.begin(), History.end(), [Kind](const FPrefixCommand& C) { return C.Kind == Kind; }));
	}

	size_t FSim::CountQueued(EPrefixCommandKind Kind) const
	{
		return static_cast<size_t>(std::count_if(Queue.begin(), Queue.end(), [Kind](const FPrefixCommand& C) { return C.Kind == Kind; }));
	}

	uint64_t FSim::EngineHandleOf(int64_t Lease)
	{
		uint64_t Engine = 0;
		uint64_t Token = 0;
		if (Cache.BeginAdopt(Lease, Engine, Token) != EPrefixAdoptCheck::Adoptable)
		{
			return 0;
		}
		Cache.EndAdopt(Token);
		return Engine;
	}

	const FEngineHandle* FSim::HandleOf(int64_t Lease)
	{
		const uint64_t Engine = EngineHandleOf(Lease);
		const auto It = Handles.find(Engine);
		return It == Handles.end() ? nullptr : &It->second;
	}

	FPrefixCompletion FSim::Execute(const FPrefixCommand& C)
	{
		FPrefixCompletion Done;
		Done.CommandId = C.Id;
		switch (C.Kind)
		{
		case EPrefixCommandKind::ComputeKey:
		{
			FPrefixStoredSpec Spec;
			Spec.SchemaName = C.KeyInputs.SchemaName;
			Spec.Tokens = C.KeyInputs.Tokens;
			Spec.bHasParent = C.KeyInputs.bHasParent;
			Spec.ParentKey = C.KeyInputs.ParentKey;
			Done.Key = ComputePrefixKey(C.KeyInputs);
			for (const auto& Forced : ForcedKeys)
			{
				if (Forced.first == Spec)
				{
					Done.Key = Forced.second;
				}
			}
			break;
		}
		case EPrefixCommandKind::CheckCooked:
		{
			const auto It = Cooked.find(C.CookedBlobId);
			if (It == Cooked.end())
			{
				Violations.push_back("CheckCooked of an unregistered blob");
				Done.Wrapper = EPrefixWrapperCheck::Truncated;
				break;
			}
			size_t Offset = 0;
			size_t Bytes = 0;
			Done.Wrapper = CheckPrefixWrapper(It->second.data(), It->second.size(), Config.Identity, C.Key, C.Spec, Offset, Bytes);
			if (Done.Wrapper == EPrefixWrapperCheck::Ok)
			{
				Done.PayloadRef = StorePayload(std::vector<uint8_t>(It->second.begin() + static_cast<long>(Offset),
					It->second.begin() + static_cast<long>(Offset + Bytes)));
			}
			break;
		}
		case EPrefixCommandKind::ReadDisk:
		{
			std::vector<uint8_t> File;
			Done.Io = ReadPrefixFile(*Fs, Config.DiskDirectory, C.Key, File);
			if (Done.Io == EPrefixIoStatus::Ok)
			{
				size_t Offset = 0;
				size_t Bytes = 0;
				Done.Wrapper = CheckPrefixWrapper(File.data(), File.size(), Config.Identity, C.Key, C.Spec, Offset, Bytes);
				if (Done.Wrapper == EPrefixWrapperCheck::Ok)
				{
					Done.PayloadRef = StorePayload(std::vector<uint8_t>(File.begin() + static_cast<long>(Offset),
						File.begin() + static_cast<long>(Offset + Bytes)));
				}
			}
			break;
		}
		case EPrefixCommandKind::Restore:
		{
			const auto It = Payloads.find(C.PayloadRef);
			if (It == Payloads.end())
			{
				Violations.push_back("Restore of an unknown payload ref");
				Done.Engine = EPrefixEngineResult::Failed;
				break;
			}
			const std::vector<uint8_t> Payload = It->second;
			Payloads.erase(It);
			if (bFailRestores)
			{
				Done.Engine = EPrefixEngineResult::Failed;
				Done.EngineStatus = RefuseStatus;
				break;
			}
			FEngineHandle Handle;
			if (bRefuseRestores || !ParseFakePayload(Payload, Handle.Schema, Handle.Tokens))
			{
				Done.Engine = EPrefixEngineResult::Refused;
				Done.EngineStatus = RefuseStatus;
				break;
			}
			const uint64_t Need = PagesFor(C.ContextLength) - std::min<uint64_t>(RestoreSharedPages, PagesFor(C.ContextLength));
			if (CacheDrawnPages + ExternalPages + Need > PoolPages)
			{
				Done.Engine = EPrefixEngineResult::PoolExhausted;
				break;
			}
			Handle.Pages = Need;
			Handle.bFromRestore = true;
			CacheDrawnPages += Need;
			PeakCacheDrawnPages = std::max(PeakCacheDrawnPages, CacheDrawnPages);
			Done.EngineHandle = NextEngineHandle++;
			Handles[Done.EngineHandle] = Handle;
			Done.SharedPages = RestoreSharedPages;
			break;
		}
		case EPrefixCommandKind::Build:
		{
			if (C.Spec.SchemaName.compare(0, 7, "missing") == 0)
			{
				Done.Engine = EPrefixEngineResult::UnknownSchema;
				break;
			}
			if (bFailBuilds)
			{
				Done.Engine = EPrefixEngineResult::Failed;
				break;
			}
			const uint64_t Peak = BuildPagesFor(C.ContextLength);
			if (CacheDrawnPages + ExternalPages + Peak > PoolPages)
			{
				Done.Engine = EPrefixEngineResult::PoolExhausted;
				break;
			}
			if (static_cast<size_t>(C.ContextLength) != C.Spec.Tokens.size())
			{
				Violations.push_back("Build whose ContextLength is not its token count");
			}
			// begin_budgeted draws R(L); the freeze returns the unused reserve, leaving ceil(L/B).
			CacheDrawnPages += Peak;
			PeakCacheDrawnPages = std::max(PeakCacheDrawnPages, CacheDrawnPages);
			const uint64_t Steady = PagesFor(C.ContextLength);
			CacheDrawnPages -= (Peak - Steady);
			FEngineHandle Handle;
			Handle.Pages = Steady;
			Handle.Tokens = C.Spec.Tokens;
			Handle.Schema = C.Spec.SchemaName;
			Done.EngineHandle = NextEngineHandle++;
			Handles[Done.EngineHandle] = Handle;
			break;
		}
		case EPrefixCommandKind::Save:
		{
			const auto It = Handles.find(C.EngineHandle);
			if (It == Handles.end() || It->second.bReleased)
			{
				Violations.push_back("Save of a released or unknown engine handle");
				Done.Engine = EPrefixEngineResult::Failed;
				break;
			}
			if (bFailSaves)
			{
				Done.Engine = EPrefixEngineResult::Failed;
				break;
			}
			Done.PayloadRef = StorePayload(MakeFakePayload(It->second.Schema, It->second.Tokens));
			break;
		}
		case EPrefixCommandKind::WriteDisk:
		{
			const auto It = Payloads.find(C.PayloadRef);
			if (It == Payloads.end())
			{
				Violations.push_back("WriteDisk of an unknown payload ref");
				Done.Io = EPrefixIoStatus::Error;
				break;
			}
			const std::vector<uint8_t> Payload = It->second;
			Payloads.erase(It);
			const std::vector<uint8_t> Bytes = EncodePrefixWrapper(Config.Identity, C.Key, C.Spec, Payload.data(), Payload.size());
			const bool bOk = WritePrefixFileAtomically(*Fs, Config.DiskDirectory, C.Key, Config.InstanceId, C.WriteSerial, Bytes, WriteFault);
			Done.Io = bOk ? EPrefixIoStatus::Ok : EPrefixIoStatus::Error;
			break;
		}
		case EPrefixCommandKind::RemoveDiskFile:
			Fs->RemoveFile(DiskPath(C.Key));
			break;
		case EPrefixCommandKind::TouchDiskFile:
			Fs->TouchModifiedTime(DiskPath(C.Key));
			break;
		case EPrefixCommandKind::TrimDisk:
			TrimPrefixDiskDirectory(*Fs, Config.DiskDirectory, Config.DiskMaxBytes);
			break;
		case EPrefixCommandKind::Release:
		{
			++ReleaseCount[C.EngineHandle];
			const auto It = Handles.find(C.EngineHandle);
			if (It == Handles.end())
			{
				Violations.push_back("Release of an unknown engine handle");
				break;
			}
			if (It->second.bReleased)
			{
				Violations.push_back("Release of an engine handle already released");
				break;
			}
			It->second.bReleased = true;
			CacheDrawnPages -= It->second.Pages;
			break;
		}
		case EPrefixCommandKind::DropPayload:
			Payloads.erase(C.PayloadRef);
			break;
		default:
			Violations.push_back("a command of unknown kind");
			break;
		}
		return Done;
	}

	// --- Fixtures --------------------------------------------------------------------------------

	FPrefixEngineIdentity FixtureIdentity()
	{
		FPrefixEngineIdentity Id;
		for (size_t I = 0; I < Id.ArtifactHash.size(); ++I)
		{
			Id.ArtifactHash[I] = static_cast<uint8_t>(0xA0 + I);
		}
		Id.Layer1Tag = "v1.14.0";
		Id.Layer1Commit = "0123456789abcdef0123456789abcdef01234567";
		Id.Backend = 0;
		return Id;
	}

	FPrefixCacheConfig FixtureConfig(uint32_t EntryBudget, bool bDisk)
	{
		FPrefixCacheConfig C;
		C.Identity = FixtureIdentity();
		C.PageTokens = 16;
		C.ContextCap = 64;
		C.VocabSize = 50000;
		C.PageBytes = kFixturePageBytes;
		// ceil(32/16) = 2 pages per entry, plus one page so a build's peak R(32) = 3 fits beside N - 1 entries.
		C.BudgetBytes = (static_cast<uint64_t>(EntryBudget) * 2 + 1) * kFixturePageBytes;
		C.bDiskEnabled = bDisk;
		C.DiskDirectory = "cache";
		C.DiskMaxBytes = 1ull << 40;
		C.InstanceId = 7;
		return C;
	}

	FPrefixSpec FixtureSpec(int32_t Index, int32_t Length, const std::string& Schema)
	{
		FPrefixSpec Spec;
		for (int32_t I = 0; I < Length; ++I)
		{
			Spec.Tokens.push_back((Index * 131 + I * 7 + 1) % 50000);
		}
		Spec.SchemaName = Schema;
		return Spec;
	}

	FPrefixStoredSpec StoredOf(const FPrefixSpec& Spec)
	{
		FPrefixStoredSpec Stored;
		Stored.SchemaName = Spec.SchemaName;
		Stored.Tokens = Spec.Tokens;
		return Stored;
	}

	FPrefixKeyInputs KeyInputsFor(const FPrefixEngineIdentity& Identity, const FPrefixStoredSpec& Spec)
	{
		FPrefixKeyInputs In;
		In.ArtifactHash = Identity.ArtifactHash;
		In.Layer1Tag = Identity.Layer1Tag;
		In.Layer1Commit = Identity.Layer1Commit;
		In.Backend = Identity.Backend;
		In.AdapterVendorId = Identity.AdapterVendorId;
		In.AdapterDeviceId = Identity.AdapterDeviceId;
		In.SchemaName = Spec.SchemaName;
		In.Tokens = Spec.Tokens;
		In.bHasParent = Spec.bHasParent;
		In.ParentKey = Spec.ParentKey;
		return In;
	}

	FPrefixKey FixtureKey(const FPrefixSpec& Spec)
	{
		return ComputePrefixKey(KeyInputsFor(FixtureIdentity(), StoredOf(Spec)));
	}

	bool Contains(const std::string& Haystack, const std::string& Needle)
	{
		return !Needle.empty() && Haystack.find(Needle) != std::string::npos;
	}
}
