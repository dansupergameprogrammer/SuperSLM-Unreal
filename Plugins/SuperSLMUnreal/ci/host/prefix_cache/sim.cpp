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
		if (bFailRemoves)
		{
			return false;
		}
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
			// The host's StartCommand: a release leaves the live set when it is issued (review finding 2).
			if (Command.Kind == EPrefixCommandKind::Release && !bHostLiveAtDelivery)
			{
				HostLive.erase(Command.EngineHandle);
			}
			History.push_back(Command);
			Queue.push_back(Command);
		}
	}

	void FSim::HostDeliver(const FPrefixCommand& Command, const FPrefixCompletion& Done)
	{
		// The host's DeliverLaneJob.
		if ((Command.Kind == EPrefixCommandKind::Build || Command.Kind == EPrefixCommandKind::Restore) &&
			Done.Engine == EPrefixEngineResult::Ok && Done.EngineHandle != 0)
		{
			HostLive.insert(Done.EngineHandle);
		}
		if (Command.Kind == EPrefixCommandKind::Release)
		{
			const bool bNotConsumed = Done.Engine == EPrefixEngineResult::Busy || Done.Engine == EPrefixEngineResult::Skipped;
			if (bHostLiveAtDelivery && !bNotConsumed)
			{
				HostLive.erase(Command.EngineHandle);
			}
			else if (!bHostLiveAtDelivery && bNotConsumed)
			{
				HostLive.insert(Command.EngineHandle);
			}
		}
	}

	uint64_t FSim::MintHandle()
	{
		if (bReuseReleasedHandles)
		{
			for (const auto& Pair : Handles)
			{
				if (Pair.second.bReleased)
				{
					ReleaseCount.erase(Pair.first);
					return Pair.first;
				}
			}
		}
		return NextEngineHandle++;
	}

	void FSim::DeliverAsPass(const FPrefixCommand& Command, const FPrefixCompletion& Done)
	{
		HostDeliver(Command, Done);
		Cache.Apply(Done);
		Cache.EndApplyPass();
		Pull();
	}

	bool FSim::HashHeld(const FPrefixCommand& Command) const
	{
		if (Command.Kind != EPrefixCommandKind::ComputeKey)
		{
			return false;
		}
		return (Command.OpId != 0 && HeldHashOps.count(Command.OpId) != 0) ||
			(Command.OpId == 0 && HeldHashLeases.count(Command.LeaseHandle) != 0);
	}

	void FSim::ApplyAsPass(const FPrefixCommand& Command)
	{
		const FPrefixCompletion Done = Execute(Command);
		HostDeliver(Command, Done);
		Cache.Apply(Done);
		Cache.EndApplyPass();
		if (bAutoPull)
		{
			Pull();
		}
	}

	void FSim::Pass()
	{
		Cache.EndApplyPass();
		Pull();
	}

	bool FSim::RunHashOfLease(int64_t Lease)
	{
		Pull();
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == EPrefixCommandKind::ComputeKey && It->OpId == 0 && It->LeaseHandle == Lease)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				ApplyAsPass(Command);
				return true;
			}
		}
		return false;
	}

	bool FSim::RunHashOfOp(uint64_t OpId)
	{
		Pull();
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == EPrefixCommandKind::ComputeKey && It->OpId == OpId)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				ApplyAsPass(Command);
				return true;
			}
		}
		return false;
	}

	bool FSim::RunOne()
	{
		if (bAutoPull)
		{
			Pull();
		}
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (Held.count(It->Kind) == 0 && !HashHeld(*It))
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				ApplyAsPass(Command);
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
				ApplyAsPass(Command);
				return true;
			}
		}
		return false;
	}

	bool FSim::RunRelease(uint64_t EngineHandle)
	{
		if (bAutoPull)
		{
			Pull();
		}
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == EPrefixCommandKind::Release && It->EngineHandle == EngineHandle)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				ApplyAsPass(Command);
				return true;
			}
		}
		return false;
	}

	bool FSim::ExecuteOnly(EPrefixCommandKind Kind, FPrefixCompletion& Out)
	{
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == Kind)
			{
				const FPrefixCommand Command = *It;
				Queue.erase(It);
				Out = Execute(Command);
				return true;
			}
		}
		return false;
	}

	bool FSim::ExecuteOnly(EPrefixCommandKind Kind, FPrefixCommand& OutCommand, FPrefixCompletion& Out)
	{
		for (auto It = Queue.begin(); It != Queue.end(); ++It)
		{
			if (It->Kind == Kind)
			{
				OutCommand = *It;
				Queue.erase(It);
				Out = Execute(OutCommand);
				return true;
			}
		}
		return false;
	}

	void FSim::DestroyPool()
	{
		for (auto& Pair : Handles)
		{
			if (!Pair.second.bReleased)
			{
				Pair.second.bReleased = true;
				++PoolDestroyReleases;
			}
		}
		CacheDrawnPages = 0;
		PageRefs.clear();
		Queue.clear();
		Payloads.clear();
		HostLive.clear();
	}

	bool FSim::IsLaneKind(EPrefixCommandKind Kind)
	{
		return Kind == EPrefixCommandKind::Restore || Kind == EPrefixCommandKind::Build || Kind == EPrefixCommandKind::Save ||
			Kind == EPrefixCommandKind::Release;
	}

	bool FSim::HostReleaseForPoolDestroy(std::vector<FPrefixCompletion>& Inbox)
	{
		auto RunPoolIo = [this, &Inbox]() {
			for (auto It = Queue.begin(); It != Queue.end();)
			{
				if (IsLaneKind(It->Kind) || HashHeld(*It))
				{
					++It;
					continue;
				}
				const FPrefixCommand Command = *It;
				It = Queue.erase(It);
				Inbox.push_back(Execute(Command));
			}
		};
		// 1. The test holds yield to teardown; the pool-thread IO already taken completes. The hash-completion
		// hold does not yield (plan §8: with it set, the held op's hash is never applied), so a held hash
		// stays queued past the teardown and its completion, when released, carries the old generation.
		for (auto It = Held.begin(); It != Held.end();)
		{
			It = IsLaneKind(*It) ? std::next(It) : Held.erase(It);
		}
		RunPoolIo();
		HostTeardownRounds = 0;
		if (!Cache.IsConfigured())
		{
			return false;
		}
		// 1a. Apply the IO completions as an apply pass, then run disk work until no drop delete is pending.
		for (int Round = 0; Round < 64; ++Round)
		{
			++HostTeardownRounds;
			for (const FPrefixCompletion& Done : Inbox)
			{
				Cache.Apply(Done);
			}
			Inbox.clear();
			Cache.EndApplyPass();
			if (Cache.PendingDropDeletes() == 0)
			{
				break;
			}
			Pull();
			RunPoolIo();
			// A write-back's save no lane has run, which a delete may wait on (§5.4 rev 1.18.5): completed without
			// running (Skipped) and applied in the next round, so the delete runs under this configuration.
			for (auto It = Queue.begin(); It != Queue.end();)
			{
				if (It->Kind != EPrefixCommandKind::Save)
				{
					++It;
					continue;
				}
				FPrefixCompletion Skipped;
				Skipped.CommandId = It->Id;
				Skipped.Generation = It->Generation;
				Skipped.Engine = EPrefixEngineResult::Skipped;
				Skipped.EngineStatus = -1;
				Inbox.push_back(Skipped);
				It = Queue.erase(It);
			}
		}
		// 2. Lane jobs never posted: a release runs; any other job is failed without running. Unapplied.
		std::deque<FPrefixCommand> Jobs;
		Jobs.swap(Queue);
		for (const FPrefixCommand& Job : Jobs)
		{
			if (!IsLaneKind(Job.Kind))
			{
				Queue.push_back(Job); // a held hash: completes after Abandon(), stale
				continue;
			}
			if (Job.Kind == EPrefixCommandKind::Release)
			{
				Inbox.push_back(Execute(Job));
				HostDeliver(Job, Inbox.back()); // the host's step 2 bookkeeping, not an apply
				continue;
			}
			FPrefixCompletion Failed;
			Failed.CommandId = Job.Id;
			Failed.Generation = Job.Generation;
			Failed.Engine = EPrefixEngineResult::Failed;
			Failed.EngineStatus = -1;
			Inbox.push_back(Failed);
		}
		Held.clear();
		// 3. Every engine handle the cache still holds, released; then the pool goes. The sweep below is the
		// engine's truth; the host sweeps HostLive, so the differences are what the host would leak or double.
		HostSweepMisses = 0;
		HostSweepDoubles = 0;
		for (const auto& Pair : Handles)
		{
			const bool bInHostLive = HostLive.count(Pair.first) != 0;
			HostSweepMisses += (!Pair.second.bReleased && !bInHostLive) ? 1u : 0u;
			HostSweepDoubles += (Pair.second.bReleased && bInHostLive) ? 1u : 0u;
		}
		HostLive.clear();
		for (auto& Pair : Handles)
		{
			if (!Pair.second.bReleased)
			{
				Pair.second.bReleased = true;
				++ReleaseCount[Pair.first];
			}
		}
		CacheDrawnPages = 0;
		PageRefs.clear();
		Payloads.clear();
		return true;
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

	namespace
	{
		void Ref(std::map<uint64_t, int>& Refs, uint64_t Page) { ++Refs[Page]; }
		void Unref(std::map<uint64_t, int>& Refs, uint64_t Page)
		{
			const auto It = Refs.find(Page);
			if (It != Refs.end() && --It->second == 0)
			{
				Refs.erase(It);
			}
		}
	}

	FPrefixCompletion FSim::Execute(const FPrefixCommand& C)
	{
		struct FProbe
		{
			FSim& S;
			~FProbe()
			{
				S.CacheDrawnPages = S.PageRefs.size();
				S.PeakCacheDrawnPages = std::max(S.PeakCacheDrawnPages, S.CacheDrawnPages);
			}
		} Probe{*this};
		auto Draw = [this](uint64_t Count, std::vector<uint64_t>& Into) {
			for (uint64_t I = 0; I < Count; ++I)
			{
				const uint64_t Page = NextPageId++;
				Ref(PageRefs, Page);
				Into.push_back(Page);
			}
		};
		FPrefixCompletion Done;
		Done.CommandId = C.Id;
		Done.Generation = C.Generation;
		// The disk part's counters: commands held in Queue are running (see sim.h).
		const bool bDropDelete = C.Kind == EPrefixCommandKind::RemoveDiskFile && C.OpId != 0;
		const bool bKeyDisk = C.Kind == EPrefixCommandKind::ReadDisk || C.Kind == EPrefixCommandKind::WriteDisk ||
			C.Kind == EPrefixCommandKind::TouchDiskFile || bDropDelete;
		bool bReadRunning = false;
		for (const FPrefixCommand& Running : Queue)
		{
			if (Held.count(Running.Kind) == 0)
			{
				continue;
			}
			const bool bRunningDelete = Running.Kind == EPrefixCommandKind::RemoveDiskFile && Running.OpId != 0;
			const bool bRunningKeyDisk = Running.Kind == EPrefixCommandKind::ReadDisk ||
				Running.Kind == EPrefixCommandKind::WriteDisk || Running.Kind == EPrefixCommandKind::TouchDiskFile || bRunningDelete;
			if (bKeyDisk && bRunningKeyDisk && Running.Key == C.Key)
			{
				MaxKeyDiskOverlap = std::max<uint32_t>(MaxKeyDiskOverlap, 2);
				bReadRunning = bReadRunning || Running.Kind == EPrefixCommandKind::ReadDisk ||
					Running.Kind == EPrefixCommandKind::TouchDiskFile;
			}
			if ((bDropDelete && Running.Kind == EPrefixCommandKind::TrimDisk) ||
				(C.Kind == EPrefixCommandKind::TrimDisk && bRunningDelete))
			{
				MaxDeleteTrimOverlap = std::max<uint32_t>(MaxDeleteTrimOverlap, 2);
			}
		}
		if (bKeyDisk)
		{
			MaxKeyDiskOverlap = std::max<uint32_t>(MaxKeyDiskOverlap, 1);
		}
		if (bDropDelete || C.Kind == EPrefixCommandKind::TrimDisk)
		{
			MaxDeleteTrimOverlap = std::max<uint32_t>(MaxDeleteTrimOverlap, 1);
		}
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
			// A nested restore shares its hint's leading floor(origin/B) pages when the hint is live and holds
			// exactly origin positions (§3.5); any other hint, or none, restores privately.
			const FEngineHandle* Hint = nullptr;
			if (C.ParentEngineHandle != 0 && !bNullHints)
			{
				const auto HintIt = Handles.find(C.ParentEngineHandle);
				if (HintIt == Handles.end() || HintIt->second.bReleased)
				{
					Violations.push_back("Restore hinted with a released or unknown engine handle");
				}
				else if (HintIt->second.Length == C.OriginLength)
				{
					Hint = &HintIt->second;
				}
			}
			const uint64_t Pages = PagesFor(C.ContextLength);
			uint64_t Shared = 0;
			if (Hint != nullptr)
			{
				Shared = std::min<uint64_t>(static_cast<uint64_t>(C.OriginLength) / Config.PageTokens, Hint->PageIds.size());
			}
			else if (C.OriginLength == 0)
			{
				Shared = std::min<uint64_t>(RestoreSharedPages, Pages);
			}
			const uint64_t Need = Pages - Shared;
			if (CacheDrawnPages + ExternalPages + Need > PoolPages)
			{
				Done.Engine = EPrefixEngineResult::PoolExhausted;
				break;
			}
			if (Hint != nullptr)
			{
				for (uint64_t I = 0; I < Shared; ++I)
				{
					Ref(PageRefs, Hint->PageIds[I]);
					Handle.PageIds.push_back(Hint->PageIds[I]);
				}
				Handle.SharedPages = static_cast<uint32_t>(Shared);
			}
			Draw(Need, Handle.PageIds);
			Handle.Pages = Handle.PageIds.size();
			Handle.Length = C.ContextLength;
			Handle.bFromRestore = true;
			Done.EngineHandle = MintHandle();
			Handles[Done.EngineHandle] = Handle;
			Done.SharedPages = static_cast<uint32_t>(Shared);
			break;
		}
		case EPrefixCommandKind::Build:
		{
			// The host's build lambda (review finding 1): counted on its lane while the hold is armed; a forced
			// build returns without a begin; every other build passes the hold at its begin.
			const bool bHoldCounted = bBuildHoldArmed;
			LaneBuildHolds += bHoldCounted ? 1 : 0;
			if (bForceNextBuild)
			{
				bForceNextBuild = false;
				++ForcedBuilds;
				Done.Engine = ForcedBuildResult;
				Done.EngineStatus = ForcedBuildStatus;
				LaneBuildHolds -= (bHoldCounted && !bForcedBuildSkipsHold) ? 1 : 0;
				break;
			}
			LaneBuildHolds -= bHoldCounted ? 1 : 0;
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
			const FEngineHandle* Parent = nullptr;
			if (C.ParentEngineHandle != 0)
			{
				const auto ParentIt = Handles.find(C.ParentEngineHandle);
				if (ParentIt == Handles.end() || ParentIt->second.bReleased)
				{
					// begin_from against a released parent: caller UB in the engine (§3.7).
					Violations.push_back("Build from a released or unknown parent handle");
					Done.Engine = EPrefixEngineResult::Failed;
					break;
				}
				Parent = &ParentIt->second;
				if (Parent->Length != C.OriginLength)
				{
					Violations.push_back("Build whose OriginLength is not its parent's length");
				}
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
			// begin_budgeted / begin_from draw R(budget); begin_from shares the parent's full pages; the
			// freeze returns the unused reserve, leaving ceil(L/B) - floor(origin/B) private pages.
			FEngineHandle Handle;
			const int32_t Origin = Parent != nullptr ? Parent->Length : 0;
			const uint64_t SharedFull = static_cast<uint64_t>(Origin) / Config.PageTokens;
			for (uint64_t I = 0; Parent != nullptr && I < SharedFull; ++I)
			{
				Ref(PageRefs, Parent->PageIds[I]);
				Handle.PageIds.push_back(Parent->PageIds[I]);
			}
			Handle.SharedPages = static_cast<uint32_t>(SharedFull);
			std::vector<uint64_t> Drawn;
			Draw(Peak, Drawn);
			CacheDrawnPages = PageRefs.size();
			PeakCacheDrawnPages = std::max(PeakCacheDrawnPages, CacheDrawnPages);
			const uint64_t Private = PagesFor(Origin + C.ContextLength) - SharedFull;
			for (uint64_t I = 0; I < Drawn.size(); ++I)
			{
				if (I < Private)
				{
					Handle.PageIds.push_back(Drawn[I]);
				}
				else
				{
					Unref(PageRefs, Drawn[I]);
				}
			}
			Handle.Pages = Handle.PageIds.size();
			Handle.Length = Origin + C.ContextLength;
			if (Parent != nullptr)
			{
				Handle.Tokens = Parent->Tokens;
				Handle.Schema = Parent->Schema;
			}
			else
			{
				Handle.Schema = C.Spec.SchemaName;
			}
			Handle.Tokens.insert(Handle.Tokens.end(), C.Spec.Tokens.begin(), C.Spec.Tokens.end());
			Done.EngineHandle = MintHandle();
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
		{
			// The completion reports the delete's outcome (a drop's disk part reads it; the wrapper-miss
			// removal ignores it): Ok = deleted, NotFound = no file, Error = the file API refused.
			const bool bExisted = Fs->Exists(DiskPath(C.Key));
			if (bDropDelete)
			{
				++DropDeletesRun;
			}
			if (bReadRunning && bExisted)
			{
				Done.Io = EPrefixIoStatus::Error; // a delete meeting a file open for reading fails (Windows)
				Done.IoError = SharingViolationError;
				break;
			}
			const bool bRemoved = Fs->RemoveFile(DiskPath(C.Key));
			Done.Io = bRemoved ? EPrefixIoStatus::Ok : (bExisted ? EPrefixIoStatus::Error : EPrefixIoStatus::NotFound);
			if (Done.Io == EPrefixIoStatus::Error)
			{
				Done.IoError = RemoveRefusalError;
			}
			break;
		}
		case EPrefixCommandKind::TouchDiskFile:
			Fs->TouchModifiedTime(DiskPath(C.Key));
			break;
		case EPrefixCommandKind::TrimDisk:
			TrimPrefixDiskDirectory(*Fs, Config.DiskDirectory, Config.DiskMaxBytes);
			break;
		case EPrefixCommandKind::Release:
		{
			if (SkipReleases > 0)
			{
				// Skipped by the submission thread: never reached the engine; the handle is untouched.
				--SkipReleases;
				++SkippedReleases;
				Done.Engine = EPrefixEngineResult::Skipped;
				Done.EngineStatus = -1;
				break;
			}
			if (FailReleases > 0)
			{
				--FailReleases;
				Done.Engine = EPrefixEngineResult::Failed;
				Done.EngineStatus = 1;
				break;
			}
			if (BusyReleases > 0 && !bBusyAfterRelease)
			{
				// SSLM_BUSY: the engine's contract leaves the handle untouched, so nothing is released here.
				--BusyReleases;
				++BusyIssues;
				Done.Engine = EPrefixEngineResult::Busy;
				Done.EngineStatus = 2;
				break;
			}
			bool bReportBusy = false;
			if (BusyReleases > 0 && bBusyAfterRelease)
			{
				--BusyReleases;
				++BusyIssues;
				bReportBusy = true;
			}
			bool bReportFailed = false;
			if (ConsumeThenFailReleases > 0)
			{
				--ConsumeThenFailReleases;
				bReportFailed = true;
			}
			struct FReport
			{
				FPrefixCompletion& D;
				bool bBusy;
				bool bFailed;
				int32_t Status;
				~FReport()
				{
					if (bBusy)
					{
						D.Engine = EPrefixEngineResult::Busy;
						D.EngineStatus = 2;
					}
					else if (bFailed)
					{
						D.Engine = EPrefixEngineResult::Failed;
						D.EngineStatus = Status;
					}
				}
			} Report{Done, bReportBusy, bReportFailed, ReleaseFailStatus};
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
			for (uint64_t Page : It->second.PageIds)
			{
				Unref(PageRefs, Page);
			}
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
