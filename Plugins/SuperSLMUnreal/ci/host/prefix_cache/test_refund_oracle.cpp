// The deferred-refund admission against the plan's executed model (precache plan rev 1.17 §5.11 (a),
// "Executed"): data/refund_oracle_trace.txt is that model's own run (rule N, root entries, releases
// applied in random order; data/README.md says how it was generated). Every line is replayed against
// FSuperSLMResidencyCore, and every decision, eviction list, admission set and ledger total must
// match the model's.

#include "harness.h"
#include "SuperSLMResidencyCore.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	// fopen, without MSVC's C4996 ("may be unsafe") under /W4 /WX: fopen_s there, std::fopen elsewhere.
	FILE* OpenTraceForRead(const char* Path)
	{
#if defined(_MSC_VER)
		FILE* File = nullptr;
		return fopen_s(&File, Path, "r") == 0 ? File : nullptr;
#else
		return std::fopen(Path, "r");
#endif
	}
}

using namespace SuperSLMCore;

namespace
{
	FResidencyKey KeyText(const std::string& Name)
	{
		FResidencyKey K;
		for (size_t I = 0; I < Name.size() && I < K.Bytes.size(); ++I)
		{
			K.Bytes[I] = static_cast<uint8_t>(Name[I]);
		}
		return K;
	}

	std::vector<std::string> Split(const std::string& S, char Sep)
	{
		std::vector<std::string> Out;
		if (S == "-")
		{
			return Out;
		}
		std::string Cur;
		for (char Ch : S)
		{
			if (Ch == Sep)
			{
				Out.push_back(Cur);
				Cur.clear();
			}
			else
			{
				Cur += Ch;
			}
		}
		Out.push_back(Cur);
		return Out;
	}

	struct FReplay
	{
		int64_t Counter = 1;
		std::unique_ptr<FSuperSLMResidencyCore> Core;
		std::map<FResidencyEntryId, std::string> KeyOf;
		std::map<std::string, FResidencyEntryId> ById; // key -> its live (keyed) entry
		std::map<int, FResidencyEntryId> Requests;
		std::map<FResidencyEntryId, int> RequestOf;
		std::vector<FResidencyEntryId> Evictions;  // in decision order
		uint64_t Budget = 0;
	};
}

HOST_TEST(refund_oracle_trace_matches_the_executed_model, "§5.11 (a) executed model (fold-rev1p17 probes, rule N): equal decisions, evictions, admissions and ledger totals")
{
	FILE* File = OpenTraceForRead(REFUND_ORACLE_TRACE);
	REQUIRE_TRUE(File != nullptr);
	std::string Text;
	char Buf[4096];
	size_t N = 0;
	while ((N = std::fread(Buf, 1, sizeof(Buf), File)) > 0)
	{
		Text.append(Buf, N);
	}
	std::fclose(File);

	std::istringstream In(Text);
	std::string Line;
	FReplay R;
	uint64_t Lines = 0;
	uint64_t Mismatches = 0;
	uint64_t Cases = 0;
	uint64_t Waits = 0;
	uint64_t Admits = 0;
	std::string FirstMismatch;
	auto Fail = [&](const std::string& What) {
		if (Mismatches++ == 0)
		{
			FirstMismatch = "case " + std::to_string(Cases) + ", line " + std::to_string(Lines) + ": " + What;
		}
	};
	while (std::getline(In, Line))
	{
		++Lines;
		if (Line.empty() || Line[0] == '#')
		{
			continue;
		}
		std::istringstream L(Line);
		std::string Op;
		L >> Op;
		if (Op == "CASE")
		{
			R = FReplay();
			L >> R.Budget;
			R.Core.reset(new FSuperSLMResidencyCore(1, &R.Counter));
			R.Core->SetBudget(kResidencyKindPrefix, kResidencyLedgerHostRam, R.Budget);
			++Cases;
		}
		else if (Op == "ENTRY")
		{
			std::string Key;
			uint64_t Charge = 0;
			int Leased = 0;
			L >> Key >> Charge >> Leased;
			const FResidencyEntryId Id = R.Core->CreateEntry(kResidencyKindPrefix, kResidencyLedgerHostRam, KeyText(Key), true);
			if (R.Core->Charge(Id, Charge) != EChargeOutcome::Charged)
			{
				Fail("setup entry not charged");
			}
			R.Core->Settle(Id, Charge);
			if (Leased != 0)
			{
				R.Core->AddPin(Id, EResidencyPin::Lease);
			}
			R.KeyOf[Id] = Key;
			R.ById[Key] = Id;
		}
		else if (Op == "REQ")
		{
			int Req = 0;
			std::string Key;
			uint64_t Charge = 0;
			std::string Expect;
			std::string Evicted;
			L >> Req >> Key >> Charge >> Expect >> Evicted;
			std::string Got;
			const FResidencyEntryId Found = R.Core->FindByKey(kResidencyKindPrefix, KeyText(Key));
			if (Found != kInvalidResidencyEntry && R.Core->GetState(Found) == EResidencyState::Resident)
			{
				R.Core->Touch(Found);
				Got = "hit";
			}
			else if (Found != kInvalidResidencyEntry)
			{
				Got = "coalesced";
			}
			else
			{
				const FResidencyEntryId Id = R.Core->CreateEntry(kResidencyKindPrefix, kResidencyLedgerHostRam, KeyText(Key), true);
				const EChargeOutcome Outcome = R.Core->Charge(Id, Charge);
				if (Outcome == EChargeOutcome::Charged)
				{
					Got = "now";
				}
				else if (Outcome == EChargeOutcome::Deferred)
				{
					Got = "wait";
					++Waits;
				}
				else
				{
					Got = "refused";
					R.Core->Destroy(Id);
				}
				if (Got != "refused")
				{
					R.KeyOf[Id] = Key;
					R.ById[Key] = Id;
					R.Requests[Req] = Id;
					R.RequestOf[Id] = Req;
				}
			}
			std::vector<std::string> GotEvicted;
			for (FResidencyEntryId Victim : R.Core->TakeEvicted())
			{
				R.Evictions.push_back(Victim);
				GotEvicted.push_back(R.KeyOf[Victim]);
			}
			if (Got != Expect)
			{
				Fail("request " + std::to_string(Req) + ": model " + Expect + ", core " + Got);
			}
			if (GotEvicted != Split(Evicted, ','))
			{
				Fail("request " + std::to_string(Req) + ": evictions differ (model " + Evicted + ")");
			}
		}
		else if (Op == "APPLY")
		{
			size_t Index = 0;
			std::string Expect;
			L >> Index >> Expect;
			if (Index >= R.Evictions.size() || !R.Core->ReleaseCompleted(R.Evictions[Index]))
			{
				Fail("apply of eviction " + std::to_string(Index) + " refused");
				continue;
			}
			std::set<int> Got;
			for (FResidencyEntryId Id : R.Core->TakeAdmitted())
			{
				Got.insert(R.RequestOf[Id]);
				++Admits;
			}
			std::set<int> Want;
			for (const std::string& S : Split(Expect, ','))
			{
				Want.insert(std::atoi(S.c_str()));
			}
			if (Got != Want)
			{
				Fail("apply " + std::to_string(Index) + ": admitted sets differ (model " + Expect + ")");
			}
		}
		else if (Op == "LEASE")
		{
			std::string Key;
			int Leased = 0;
			L >> Key >> Leased;
			const FResidencyEntryId Id = R.ById[Key];
			if (Leased != 0)
			{
				R.Core->AddPin(Id, EResidencyPin::Lease);
			}
			else if (!R.Core->RemovePin(Id, EResidencyPin::Lease))
			{
				Fail("lease toggle on an entry with no lease pin");
			}
		}
		else if (Op == "COMPLETE")
		{
			int Req = 0;
			uint64_t Resident = 0;
			L >> Req >> Resident;
			const FResidencyEntryId Id = R.Requests[Req];
			if (R.Core->GetState(Id) != EResidencyState::InFlight)
			{
				Fail("complete of request " + std::to_string(Req) + " that is not in flight");
			}
			R.Core->Settle(Id, Resident);
		}
		else if (Op == "LEDGER")
		{
			uint64_t Expect = 0;
			L >> Expect;
			const FResidencyLedger Led = R.Core->GetLedger(kResidencyKindPrefix, kResidencyLedgerHostRam);
			if (Led.ChargedBytes != Expect)
			{
				Fail("ledger: model " + std::to_string(Expect) + ", core " + std::to_string(Led.ChargedBytes));
			}
			if (Led.ChargedBytes != Led.ResidentBytes + Led.InFlightBytes + Led.ReleasingBytes + Led.WaitingBytes ||
				Led.ChargedBytes > Led.BudgetBytes || Led.ClaimedRefundBytes > Led.ReleasingBytes)
			{
				Fail("ledger invariant");
			}
		}
	}
	std::printf("    oracle: %llu cases, %llu lines, %llu waits, %llu admissions replayed\n",
		static_cast<unsigned long long>(Cases), static_cast<unsigned long long>(Lines),
		static_cast<unsigned long long>(Waits), static_cast<unsigned long long>(Admits));
	if (Mismatches != 0)
	{
		std::printf("    first mismatch: %s\n", FirstMismatch.c_str());
	}
	EXPECT_EQ(Mismatches, 0u);
	EXPECT_GT(Cases, 1000u);
	EXPECT_GT(Waits, 1000u);
}

namespace
{
	std::string ReadWhole(const char* Path, bool& bOk)
	{
		std::string Text;
		FILE* File = OpenTraceForRead(Path);
		bOk = File != nullptr;
		if (File == nullptr)
		{
			return Text;
		}
		char Buf[4096];
		size_t N = 0;
		while ((N = std::fread(Buf, 1, sizeof(Buf), File)) > 0)
		{
			Text.append(Buf, N);
		}
		std::fclose(File);
		return Text;
	}
}

HOST_TEST(nested_refund_oracle_trace_matches_the_executed_model, "§5.4 / §5.11 (a) nested (U6): fold-rev1p17 model, rule N with the rev 1.16/1.17 fixes; equal decisions, evictions, admissions, ledger and ghost totals, pinned sets")
{
	bool bOk = false;
	const std::string Text = ReadWhole(NESTED_REFUND_ORACLE_TRACE, bOk);
	REQUIRE_TRUE(bOk);
	std::istringstream In(Text);
	std::string Line;
	FReplay R;
	uint64_t Lines = 0;
	uint64_t Mismatches = 0;
	uint64_t Cases = 0;
	uint64_t Waits = 0;
	uint64_t Nested = 0;
	uint64_t Admits = 0;
	uint64_t GhostSteps = 0;
	std::string FirstMismatch;
	auto Fail = [&](const std::string& What) {
		if (Mismatches++ == 0)
		{
			FirstMismatch = "case " + std::to_string(Cases) + ", line " + std::to_string(Lines) + ": " + What;
		}
	};
	while (std::getline(In, Line))
	{
		++Lines;
		if (Line.empty() || Line[0] == '#')
		{
			continue;
		}
		std::istringstream L(Line);
		std::string Op;
		L >> Op;
		if (Op == "CASE")
		{
			R = FReplay();
			L >> R.Budget;
			R.Core.reset(new FSuperSLMResidencyCore(1, &R.Counter));
			R.Core->SetBudget(kResidencyKindPrefix, kResidencyLedgerHostRam, R.Budget);
			++Cases;
		}
		else if (Op == "ENTRY")
		{
			std::string Key;
			std::string Parent;
			uint64_t Charge = 0;
			int Leased = 0;
			L >> Key >> Charge >> Leased >> Parent;
			const FResidencyEntryId Id = R.Core->CreateEntry(kResidencyKindPrefix, kResidencyLedgerHostRam, KeyText(Key), true);
			if (R.Core->Charge(Id, Charge) != EChargeOutcome::Charged)
			{
				Fail("setup entry not charged");
			}
			R.Core->Settle(Id, Charge);
			if (Parent != "-" && !R.Core->SetParent(Id, R.ById[Parent]))
			{
				Fail("setup parent refused");
			}
			if (Leased != 0)
			{
				R.Core->AddPin(Id, EResidencyPin::Lease);
			}
			R.KeyOf[Id] = Key;
			R.ById[Key] = Id;
		}
		else if (Op == "REQ")
		{
			int Req = 0;
			std::string Key;
			uint64_t Charge = 0;
			std::string Parent;
			std::string Kind;
			std::string Expect;
			std::string Evicted;
			L >> Req >> Key >> Charge >> Parent >> Kind >> Expect >> Evicted;
			std::string Got;
			const FResidencyEntryId Found = R.Core->FindByKey(kResidencyKindPrefix, KeyText(Key));
			if (Found != kInvalidResidencyEntry && R.Core->GetState(Found) == EResidencyState::Resident)
			{
				R.Core->Touch(Found);
				Got = "hit";
			}
			else if (Found != kInvalidResidencyEntry)
			{
				Got = "coalesced";
			}
			else
			{
				const FResidencyEntryId Id = R.Core->CreateEntry(kResidencyKindPrefix, kResidencyLedgerHostRam, KeyText(Key), true);
				if (Parent != "-")
				{
					++Nested;
					if (!R.Core->SetParent(Id, R.ById[Parent]))
					{
						Fail("request " + std::to_string(Req) + ": parent refused");
					}
				}
				const EChargeOutcome Outcome = R.Core->Charge(Id, Charge, Kind == "build");
				if (Outcome == EChargeOutcome::Charged)
				{
					Got = "now";
				}
				else if (Outcome == EChargeOutcome::Deferred)
				{
					Got = "wait";
					++Waits;
				}
				else
				{
					Got = "refused";
					R.Core->Destroy(Id);
				}
				if (Got != "refused")
				{
					R.KeyOf[Id] = Key;
					R.ById[Key] = Id;
					R.Requests[Req] = Id;
					R.RequestOf[Id] = Req;
				}
			}
			std::vector<std::string> GotEvicted;
			for (FResidencyEntryId Victim : R.Core->TakeEvicted())
			{
				R.Evictions.push_back(Victim);
				GotEvicted.push_back(R.KeyOf[Victim]);
			}
			if (Got != Expect)
			{
				Fail("request " + std::to_string(Req) + ": model " + Expect + ", core " + Got);
			}
			if (GotEvicted != Split(Evicted, ','))
			{
				Fail("request " + std::to_string(Req) + ": evictions differ (model " + Evicted + ")");
			}
		}
		else if (Op == "APPLY")
		{
			size_t Index = 0;
			std::string Expect;
			L >> Index >> Expect;
			if (Index >= R.Evictions.size() || !R.Core->ReleaseCompleted(R.Evictions[Index]))
			{
				Fail("apply of eviction " + std::to_string(Index) + " refused");
				continue;
			}
			std::set<int> Got;
			for (FResidencyEntryId Id : R.Core->TakeAdmitted())
			{
				Got.insert(R.RequestOf[Id]);
				++Admits;
			}
			std::set<int> Want;
			for (const std::string& S : Split(Expect, ','))
			{
				Want.insert(std::atoi(S.c_str()));
			}
			if (Got != Want)
			{
				Fail("apply " + std::to_string(Index) + ": admitted sets differ (model " + Expect + ")");
			}
		}
		else if (Op == "LEASE")
		{
			std::string Key;
			int Leased = 0;
			L >> Key >> Leased;
			const FResidencyEntryId Id = R.ById[Key];
			if (Leased != 0)
			{
				R.Core->AddPin(Id, EResidencyPin::Lease);
			}
			else if (!R.Core->RemovePin(Id, EResidencyPin::Lease))
			{
				Fail("lease toggle on an entry with no lease pin");
			}
		}
		else if (Op == "COMPLETE")
		{
			int Req = 0;
			uint64_t Resident = 0;
			L >> Req >> Resident;
			const FResidencyEntryId Id = R.Requests[Req];
			if (R.Core->GetState(Id) != EResidencyState::InFlight)
			{
				Fail("complete of request " + std::to_string(Req) + " that is not in flight");
			}
			R.Core->Settle(Id, Resident);
		}
		else if (Op == "LEDGER")
		{
			uint64_t Expect = 0;
			uint64_t ExpectGhost = 0;
			std::string Pinned;
			L >> Expect >> ExpectGhost >> Pinned;
			const FResidencyLedger Led = R.Core->GetLedger(kResidencyKindPrefix, kResidencyLedgerHostRam);
			if (Led.ChargedBytes != Expect)
			{
				Fail("ledger: model " + std::to_string(Expect) + ", core " + std::to_string(Led.ChargedBytes));
			}
			if (Led.GhostBytes != ExpectGhost)
			{
				Fail("ghosts: model " + std::to_string(ExpectGhost) + ", core " + std::to_string(Led.GhostBytes));
			}
			GhostSteps += Led.GhostBytes != 0;
			if (Led.ChargedBytes != Led.ResidentBytes + Led.InFlightBytes + Led.ReleasingBytes + Led.WaitingBytes + Led.GhostBytes ||
				Led.ChargedBytes > Led.BudgetBytes || Led.ClaimedRefundBytes > Led.ReleasingBytes)
			{
				Fail("ledger invariant");
			}
			std::vector<std::string> GotPinned;
			for (const auto& Pair : R.ById)
			{
				if (R.Core->GetState(Pair.second) == EResidencyState::Resident && R.Core->IsPinned(Pair.second))
				{
					GotPinned.push_back(Pair.first);
				}
			}
			if (GotPinned != Split(Pinned, ','))
			{
				Fail("pinned sets differ (model " + Pinned + ")");
			}
		}
	}
	std::printf("    nested oracle: %llu cases, %llu lines, %llu nested requests, %llu waits, %llu admissions, %llu ghost steps\n",
		static_cast<unsigned long long>(Cases), static_cast<unsigned long long>(Lines), static_cast<unsigned long long>(Nested),
		static_cast<unsigned long long>(Waits), static_cast<unsigned long long>(Admits), static_cast<unsigned long long>(GhostSteps));
	if (Mismatches != 0)
	{
		std::printf("    first mismatch: %s\n", FirstMismatch.c_str());
	}
	EXPECT_EQ(Mismatches, 0u);
	EXPECT_GT(Cases, 500u);
	EXPECT_GT(Nested, 1000u);
	EXPECT_GT(GhostSteps, 100u);
}
