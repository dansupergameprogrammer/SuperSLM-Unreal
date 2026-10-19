#pragma once

// A host model of the engine's page pool, used as the drawn-pages reference the ledger is graded
// against (the host stand-in for the plan's fill probe, §3.9). It models exactly what the ledger's
// charges must cover, from the engine's documented draw rules (sslm_abi_functions.inc's paged-KV
// contract and the plugin paging plan's §2 rows E2-E6):
//
//   - a budget sequence or prefix draws R(budget) at create/begin, from the pool;
//   - a frozen prefix keeps ceil(L/B) of its own pages (its full pages become a shared group) and
//     returns the rest;
//   - a nested child shares its parent's groups, draws R(child tokens), and keeps its own pages;
//   - a budget adopter shares the prefix's groups and copies the tail into its own reserve: it
//     draws nothing, and returns its materialized pages;
//   - a private budget restore draws R(b) + E (E materialized); a sharing one draws R(b) and
//     shares the hint's groups; a whole-reserve restore draws P_blk;
//   - reset drops shared refs and returns materialized pages; release returns everything;
//   - a group is freed when its last reference goes.
//
// Every page is tagged with the kind of holder that drew it, so per-region grading can attribute
// pages: Seq (regions S and H), Prefix (region P), Cache (regions A and C).

#include <cstdint>
#include <map>
#include <vector>

namespace ledgertest
{

enum class EPageTag : int
{
	Seq = 0,
	Prefix = 1,
	Cache = 2
};

class FFakePagePool
{
public:
	FFakePagePool(int64_t InB, int64_t InCap, uint32_t InPages) : B(InB), Cap(InCap), PoolPages(InPages) {}

	uint32_t PagesPerBlock() const { return static_cast<uint32_t>((Cap + B - 1) / B); }
	uint32_t R(int64_t Budget) const
	{
		if (Budget < 1 || Budget > Cap) return 0;
		const int64_t P = (Budget + B - 1) / B + 1;
		return static_cast<uint32_t>(P < PagesPerBlock() ? P : PagesPerBlock());
	}
	uint32_t E(int64_t Budget, int64_t Origin) const
	{
		const int64_t Full = Origin / B;
		const int64_t Room = static_cast<int64_t>(PagesPerBlock()) - R(Budget);
		return static_cast<uint32_t>(Full < Room ? Full : Room);
	}

	uint32_t Drawn() const
	{
		return DrawnByTag(EPageTag::Seq) + DrawnByTag(EPageTag::Prefix) + DrawnByTag(EPageTag::Cache);
	}
	uint32_t Free() const { return PoolPages - Drawn(); }
	uint32_t DrawnByTag(EPageTag Tag) const
	{
		uint64_t Sum = 0;
		for (const auto& KV : Holders)
		{
			if (KV.second.Tag == Tag) Sum += KV.second.Owned;
		}
		for (const auto& KV : Groups)
		{
			if (KV.second.Tag == Tag) Sum += KV.second.Pages;
		}
		return static_cast<uint32_t>(Sum);
	}
	uint32_t Exhaustions() const { return ExhaustionCount; }
	bool IsLive(int H) const { return Holders.count(H) != 0; }
	int64_t Length(int H) const { return IsLive(H) ? Holders.at(H).Length : -1; }
	uint32_t SharedPagesOf(int H) const
	{
		if (!IsLive(H)) return 0;
		uint32_t Sum = 0;
		const FHolder& Hd = Holders.at(H);
		for (int G : Hd.Chain)
		{
			if (G != Hd.OwnGroup) Sum += Groups.at(G).Pages;
		}
		return Sum;
	}

	// ---- Verbs. Each returns a holder id, or -1 for KV_POOL_EXHAUSTED (nothing drawn). ----

	int CreateSequence(int64_t Budget, EPageTag Tag = EPageTag::Seq) { return Draw(R(Budget), Tag, 0, 0); }

	int CreateWholeReserve(EPageTag Tag = EPageTag::Seq) { return Draw(PagesPerBlock(), Tag, 0, 0); }

	// sslm_prefix_begin_budgeted with budget max(Tokens, 1); the prefill length is Tokens.
	int PrefixBegin(int64_t Tokens, EPageTag Tag)
	{
		const int H = Draw(R(Tokens < 1 ? 1 : Tokens), Tag, 0, Tokens);
		return H;
	}

	// sslm_prefix_begin_from: shares the frozen parent's groups.
	int BeginFrom(int Parent, int64_t ChildTokens, EPageTag Tag)
	{
		if (!IsLive(Parent) || !Holders.at(Parent).bFrozen) return -2;
		const int H = Draw(R(ChildTokens < 1 ? 1 : ChildTokens), Tag, 0, Holders.at(Parent).Length + ChildTokens);
		if (H < 0) return H;
		FHolder& Child = Holders.at(H);
		Child.ParentFullPages = static_cast<uint32_t>(Holders.at(Parent).Length / B);
		for (int G : Holders.at(Parent).Chain)
		{
			Child.Chain.push_back(G);
			Groups.at(G).Refs += 1;
		}
		return H;
	}

	void Freeze(int H)
	{
		if (!IsLive(H)) return;
		FHolder& Hd = Holders.at(H);
		if (Hd.bFrozen) return;
		Hd.bFrozen = true;
		const uint32_t AllFull = static_cast<uint32_t>(Hd.Length / B);
		const uint32_t OwnFull = AllFull - Hd.ParentFullPages;
		const uint32_t Tail = (Hd.Length % B) != 0 ? 1u : 0u;
		// Owned covers OwnFull + Tail by construction of R; the rest returns to the pool.
		if (Hd.Owned < OwnFull + Tail) Broken = true;
		Hd.Owned = Tail;
		if (OwnFull > 0)
		{
			const int G = NextGroup++;
			Groups[G] = FGroup{OwnFull, Hd.Tag, 1};
			Hd.Chain.push_back(G);
			Hd.OwnGroup = G;
		}
	}

	// A budget adopt: unmap (drop shared refs, return materialized), then share the prefix's groups.
	void Adopt(int Seq, int Prefix)
	{
		if (!IsLive(Seq) || !IsLive(Prefix)) return;
		Unmap(Seq);
		FHolder& S = Holders.at(Seq);
		for (int G : Holders.at(Prefix).Chain)
		{
			S.Chain.push_back(G);
			Groups.at(G).Refs += 1;
		}
		S.Length = Holders.at(Prefix).Length;
	}

	void Reset(int Seq)
	{
		if (!IsLive(Seq)) return;
		Unmap(Seq);
		Holders.at(Seq).Length = 0;
	}

	// sslm_seq_restore_shared for a budget payload. Shares when the hint is frozen at length ==
	// Origin and bBytesMatch; otherwise private (R + E). Returns the holder; *OutShared gets the
	// shared page count.
	int RestoreBudget(int64_t Budget, int64_t Origin, int Hint, bool bBytesMatch, uint32_t* OutShared)
	{
		*OutShared = 0;
		const bool bShare = Hint >= 0 && IsLive(Hint) && Holders.at(Hint).bFrozen &&
		                    Holders.at(Hint).Length == Origin && bBytesMatch && (Origin / B) > 0;
		if (bShare)
		{
			const int H = Draw(R(Budget), EPageTag::Seq, 0, Origin);
			if (H < 0) return H;
			for (int G : Holders.at(Hint).Chain)
			{
				Holders.at(H).Chain.push_back(G);
				Groups.at(G).Refs += 1;
			}
			*OutShared = SharedPagesOf(H);
			return H;
		}
		const uint32_t Mat = E(Budget, Origin);
		return Draw(R(Budget) + Mat, EPageTag::Seq, Mat, Origin);
	}

	int RestoreWholeReserve() { return Draw(PagesPerBlock(), EPageTag::Seq, 0, 0); }

	void Release(int H)
	{
		if (!IsLive(H)) return;
		FHolder& Hd = Holders.at(H);
		for (int G : Hd.Chain) DropRef(G);
		Holders.erase(H);
	}

	bool IsBroken() const { return Broken; }

private:
	struct FGroup
	{
		uint32_t Pages = 0;
		EPageTag Tag = EPageTag::Seq;
		int Refs = 0;
	};
	struct FHolder
	{
		EPageTag Tag = EPageTag::Seq;
		uint32_t Owned = 0;         // reserve + private + materialized pages this holder drew
		uint32_t Materialized = 0;
		int64_t Length = 0;
		bool bFrozen = false;
		uint32_t ParentFullPages = 0;
		int OwnGroup = -1;
		std::vector<int> Chain;     // groups this holder references (its own included)
	};

	int Draw(uint32_t Pages, EPageTag Tag, uint32_t Materialized, int64_t Length)
	{
		if (Pages > Free())
		{
			++ExhaustionCount;
			return -1;
		}
		const int H = NextHolder++;
		FHolder Hd;
		Hd.Tag = Tag;
		Hd.Owned = Pages;
		Hd.Materialized = Materialized;
		Hd.Length = Length;
		Holders[H] = Hd;
		return H;
	}

	void Unmap(int H)
	{
		FHolder& Hd = Holders.at(H);
		for (int G : Hd.Chain) DropRef(G);
		Hd.Chain.clear();
		Hd.OwnGroup = -1;
		Hd.Owned -= Hd.Materialized;
		Hd.Materialized = 0;
	}

	void DropRef(int G)
	{
		auto It = Groups.find(G);
		if (It == Groups.end()) return;
		It->second.Refs -= 1;
		if (It->second.Refs <= 0) Groups.erase(It);
	}

	int64_t B;
	int64_t Cap;
	uint32_t PoolPages;
	std::map<int, FHolder> Holders;
	std::map<int, FGroup> Groups;
	int NextHolder = 1;
	int NextGroup = 1;
	uint32_t ExhaustionCount = 0;
	bool Broken = false;
};

}  // namespace ledgertest
