#pragma once

// One configured cache core with its simulator, for the cache cells.

#include "harness.h"
#include "sim.h"

#include <memory>

namespace HostSim
{
	struct FCacheFixture
	{
		int64_t Counter = 1;
		FSuperSLMPrefixCacheCore Cache{&Counter};
		FPrefixCacheConfig Config;
		std::unique_ptr<FSim> Sim;
		bool bConfigured = false;

		// PoolSlackPages: pages the fake pool has beyond the budget's.
		FCacheFixture(uint32_t EntryBudget, bool bDisk, uint64_t PoolSlackPages = 8)
		{
			Config = FixtureConfig(EntryBudget, bDisk);
			bConfigured = Cache.Configure(Config);
			Sim.reset(new FSim(Cache, Config, BudgetPages() + PoolSlackPages));
		}

		// Tears down (draining every command) and configures again with NewConfig; the simulator keeps
		// its file system and pool. False if the teardown did not complete or Configure refused.
		bool Reconfigure(const FPrefixCacheConfig& NewConfig)
		{
			Cache.BeginTeardown();
			Sim->RunAll();
			if (!Cache.IsTornDown())
			{
				return false;
			}
			Config = NewConfig;
			Sim->Config = NewConfig;
			return Cache.Configure(NewConfig);
		}

		// The product's re-Configure() (Next set) and Deinitialize() (Next null): the CPU host's
		// ReleaseForPoolDestroy (FSim::HostReleaseForPoolDestroy, steps 1-3), then step 4, the core's Abandon();
		// then, at a re-Configure(), Configure(Next) and its Pump, the first apply pass, which applies the
		// completions step 2 left in the inbox (stale: another generation). The core's BeginTeardown() is not on
		// this path: the CPU host never calls it (U2 code review, findings 1 and 2). False if Configure refused.
		// It starts with the core's MarkTeardownBegins(), as DestroyCpuStateContents does at its entry (code review r2
		// B1). A cell replaying the subsystem's lane delivery (a running build or restore delivered by the teardown)
		// calls MarkTeardownBegins() itself before that delivery, then this.
		bool HostReconfigure(const FPrefixCacheConfig* Next)
		{
			Cache.MarkTeardownBegins();
			std::vector<FPrefixCompletion> Inbox;
			if (Sim->HostReleaseForPoolDestroy(Inbox))
			{
				Cache.Abandon();
			}
			if (Next == nullptr)
			{
				return true;
			}
			Config = *Next;
			Sim->Config = *Next;
			if (!Cache.Configure(*Next))
			{
				return false;
			}
			for (const FPrefixCompletion& Done : Inbox)
			{
				Cache.Apply(Done);
			}
			Cache.EndApplyPass();
			Sim->Pull();
			return true;
		}

		uint64_t BudgetPages() const { return Config.BudgetBytes / Config.PageBytes; }
		FPrefixCacheDiagnostics Diag() const { return Cache.GetDiagnostics(); }
		uint64_t Fallbacks(EPrefixFallback Reason) const { return Diag().Fallbacks[static_cast<uint32_t>(Reason)]; }

		// A request that must be accepted; 0 if it was not.
		int64_t Request(const FPrefixSpec& Spec)
		{
			int64_t Handle = 0;
			std::string Error;
			return Cache.Request(Spec, Handle, Error) == EPrefixRequestResult::Accepted ? Handle : 0;
		}

		// Request, then run everything. Returns the handle if it is Ready, else 0.
		int64_t RequestReady(const FPrefixSpec& Spec)
		{
			const int64_t Handle = Request(Spec);
			Sim->RunAll();
			return Handle != 0 && Cache.GetLeaseState(Handle) == EPrefixLeaseState::Ready ? Handle : 0;
		}

		// Writes a valid wrapper for Spec straight into the simulated directory (not through the core).
		void PutDiskFile(const FPrefixSpec& Spec, uint64_t Serial = 1)
		{
			const FPrefixStoredSpec Stored = StoredOf(Spec);
			const FPrefixKey Key = ComputePrefixKey(KeyInputsFor(Config.Identity, Stored));
			const std::vector<uint8_t> Payload = MakeFakePayload(Spec.SchemaName, Spec.Tokens);
			const std::vector<uint8_t> Bytes = EncodePrefixWrapper(Config.Identity, Key, Stored, Payload.data(), Payload.size());
			WritePrefixFileAtomically(*Sim->Fs, Config.DiskDirectory, Key, 99, Serial, Bytes, EPrefixWriteFault::None);
		}

		size_t LogLinesWith(const std::vector<FPrefixLogLine>& Log, EPrefixFallback Reason) const
		{
			size_t N = 0;
			for (const FPrefixLogLine& Line : Log)
			{
				if (Line.Reason == Reason && Contains(Line.Message, PrefixFallbackName(Reason)))
				{
					++N;
				}
			}
			return N;
		}

		// Explicit residency control (§5.6, rev 1.18). Each returns the op id, or 0 if the call was refused.
		uint64_t Warm(const FPrefixSpec& Spec)
		{
			uint64_t Op = 0;
			std::string Error;
			return Cache.Warm(Spec, Op, Error) == EPrefixRequestResult::Accepted ? Op : 0;
		}

		uint64_t Drop(const FPrefixSpec& Spec, EPrefixDropScope Scope = EPrefixDropScope::Memory)
		{
			uint64_t Op = 0;
			std::string Error;
			return Cache.Drop(Spec, Scope, Op, Error) == EPrefixRequestResult::Accepted ? Op : 0;
		}

		// The op's result; Pending with bFound false when GetOpResult returns false.
		FPrefixOpResult Op(uint64_t OpId, bool* bFound = nullptr) const
		{
			FPrefixOpResult Out;
			const bool bOk = Cache.GetOpResult(OpId, Out);
			if (bFound != nullptr)
			{
				*bFound = bOk;
			}
			return Out;
		}

		EPrefixOpResult OpResult(uint64_t OpId) const { return Op(OpId).Result; }

		// A request to Ready, then returned: the entry is resident and unpinned. True on success.
		bool Resident(const FPrefixSpec& Spec)
		{
			const int64_t H = RequestReady(Spec);
			return H != 0 && Cache.Return(H);
		}

		// The source a fresh request for Spec is served from (the lease returned afterwards).
		EPrefixSource SourceOfNextRequest(const FPrefixSpec& Spec)
		{
			const int64_t H = RequestReady(Spec);
			const EPrefixSource Source = H != 0 ? Cache.GetSource(H) : EPrefixSource::None;
			if (H != 0)
			{
				Cache.Return(H);
				Sim->Pass();
			}
			return Source;
		}

		// Registers a cooked wrapper for Spec as blob BlobId (the cooked tier, §5.3 item 2).
		void PutCooked(const FPrefixSpec& Spec, uint64_t BlobId)
		{
			const FPrefixKey Key = FixtureKey(Spec);
			const std::vector<uint8_t> Payload = MakeFakePayload(Spec.SchemaName, Spec.Tokens);
			Sim->Cooked[BlobId] = EncodePrefixWrapper(Config.Identity, Key, StoredOf(Spec), Payload.data(), Payload.size());
			Cache.RegisterCooked(Key, BlobId);
		}

		bool DiskFileExists(const FPrefixSpec& Spec) const { return Sim->Fs->Exists(Sim->DiskPath(FixtureKey(Spec))); }

		// Every handle the simulator minted is released exactly once (the release-counting seam), and none
		// is left live: the pool destroy would return SSLM_OK, not SSLM_POOL_HAS_LIVE_HANDLES.
		bool EveryHandleReleasedOnce() const
		{
			for (const auto& Pair : Sim->Handles)
			{
				const auto It = Sim->ReleaseCount.find(Pair.first);
				if (It == Sim->ReleaseCount.end() || It->second != 1 || !Pair.second.bReleased)
				{
					return false;
				}
			}
			return true;
		}

		// Tears down within a watchdog bound of steps; true when the teardown completed inside it.
		bool TeardownWithin(int MaxSteps = 20000)
		{
			Cache.BeginTeardown();
			const int Steps = Sim->RunAll(MaxSteps);
			return Steps < MaxSteps && Cache.IsTornDown();
		}

		// Engine handles released, in order.
		std::vector<uint64_t> Released() const
		{
			std::vector<uint64_t> Out;
			for (const FPrefixCommand& C : Sim->History)
			{
				if (C.Kind == EPrefixCommandKind::Release)
				{
					Out.push_back(C.EngineHandle);
				}
			}
			return Out;
		}
	};

	// A fixture whose budget is exactly Pages (B = 16: a restore of L tokens is charged ceil(L/16), a build
	// R(L) = min(ceil(L/16) + 1, 4) settling to ceil(L/16)).
	struct FPagesFixture : FCacheFixture
	{
		FPagesFixture(uint64_t Pages, bool bDisk, uint64_t PoolSlackPages = 8) : FCacheFixture(1, bDisk, PoolSlackPages)
		{
			Config.BudgetBytes = Pages * kFixturePageBytes;
			Cache.BeginTeardown();
			Sim->RunAll();
			bConfigured = Cache.IsTornDown() && Cache.Configure(Config);
			Sim->Config = Config;
			Sim->PoolPages = Pages + PoolSlackPages;
		}

		// Puts a file for Spec and requests it to Ready: a disk restore of ceil(L/16) pages.
		int64_t DiskEntry(const FPrefixSpec& Spec)
		{
			PutDiskFile(Spec);
			return RequestReady(Spec);
		}

		bool LedgerOk() const
		{
			const FPrefixCacheDiagnostics D = Diag();
			return D.ChargedBytes == D.ResidentBytes + D.InFlightBytes + D.ReleasingBytes + D.WaitingBytes &&
				D.ChargedBytes <= D.BudgetBytes && D.ClaimedRefundBytes + D.UnclaimedRefundBytes == D.ReleasingBytes;
		}
	};
}
