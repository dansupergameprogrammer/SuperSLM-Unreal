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
}
