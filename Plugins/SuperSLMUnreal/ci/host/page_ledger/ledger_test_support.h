#pragma once

// Shared fixtures for the page-ledger host suite: shapes, a "world" that pairs the ledger under
// test with the host page-pool model, job helpers split at the plan's named instants (admission,
// the job's run, apply), and the form-1 grading check.

#include "SuperSLMPageLedgerCore.h"
#include "fake_page_pool.h"
#include "ledger_test_harness.h"

#include <map>
#include <string>
#include <vector>

namespace ledgertest
{

using namespace SuperSLMPaging;

// ---- The test's own arithmetic (never the ledger's), so the reference cannot drift with it ----

inline uint32_t TPblk(int64_t B, int64_t Cap) { return static_cast<uint32_t>((Cap + B - 1) / B); }
inline uint32_t TR(int64_t B, int64_t Cap, int64_t Budget)
{
	if (Budget < 1 || Budget > Cap) return 0;
	const int64_t P = (Budget + B - 1) / B + 1;
	const int64_t Blk = TPblk(B, Cap);
	return static_cast<uint32_t>(P < Blk ? P : Blk);
}
inline uint32_t TE(int64_t B, int64_t Cap, int64_t Budget, int64_t Origin)
{
	const int64_t Full = Origin / B;
	const int64_t Room = static_cast<int64_t>(TPblk(B, Cap)) - TR(B, Cap, Budget);
	return static_cast<uint32_t>(Full < Room ? Full : Room);
}
// The two caps (UP-15 (c)): the P/A block and the H block, from the plugin's cap Cp and
// the engine's cap Ce. With Cp == Ce both are ceil(Ce / B).
inline uint32_t TPBlk2(int64_t B, int64_t Cp, int64_t Ce)
{
	const int64_t A = (Cp + B - 1) / B + 1;
	const int64_t Z = (Ce + B - 1) / B;
	return static_cast<uint32_t>(A < Z ? A : Z);
}
inline uint32_t THBlk2(int64_t B, int64_t Cp, int64_t Ce)
{
	const int64_t A = static_cast<int64_t>(TPBlk2(B, Cp, Ce)) + Cp / B;
	const int64_t Z = (Ce + B - 1) / B;
	return static_cast<uint32_t>(A < Z ? A : Z);
}
inline uint32_t TFull(int64_t B, int64_t L) { return L <= 0 ? 0u : static_cast<uint32_t>(L / B); }
inline uint32_t TOcc(int64_t B, int64_t L) { return L <= 0 ? 0u : static_cast<uint32_t>((L + B - 1) / B); }

// The shapes, per backend (plugin paging plan §2: "Figures are given per backend"). The
// 0.5B model at cap 4,096 has B = 16 on the GPU (96 KiB pages) and B = 64 on the CPU (384 KiB).
// Earlier rounds called the B = 16 shape "the plan's"; it is the GPU's. Tests that run the B = 16
// shape through a CPU configuration grade the ledger's arithmetic at that page size, which the
// ledger takes as an input whatever the backend.
//
// EngineCap is the engine's cap Ce (the model's context_cap); 0 means "equal to Cap" (Cp = Ce).
struct FShape
{
	int64_t B;
	int64_t Cap;
	int32_t SequenceBudget;
	uint64_t PageBytes;
	const char* Name;
	int64_t EngineCap = 0;
};

inline FShape GpuShape() { return FShape{16, 4096, 512, 96u * 1024u, "GPU 0.5B B=16"}; }
inline FShape CpuShape() { return FShape{64, 4096, 512, 384u * 1024u, "CPU 0.5B B=64"}; }
// The shipped 1.5B artifact (model cap 32,768). On the CPU there is no ContextCap: Cp = Ce.
inline FShape Cpu15Shape() { return FShape{64, 32768, 512, 896u * 1024u, "CPU 1.5B B=64", 0}; }
// The GPU two-cap configuration (§7, UP-15): the 1.5B artifact with ContextCap 4,096, so
// P_blk = 257 and H_blk = 513.
inline FShape TwoCapShape() { return FShape{16, 4096, 512, 224u * 1024u, "GPU 1.5B two-cap", 32768}; }
// The probe's second two-cap row: the 0.5B GPU (model cap 4,096) with ContextCap 2,048.
inline FShape TwoCapHalfShape() { return FShape{16, 2048, 512, 96u * 1024u, "GPU 0.5B ContextCap 2048", 4096}; }

inline int64_t TEngineCap(const FShape& S) { return S.EngineCap > 0 ? S.EngineCap : S.Cap; }

inline FPageLedgerConfig MakeConfig(const FShape& S, int32_t BlockCount, int32_t PrefixBlockCount,
                                    int32_t RestoreHeadroomBlocks, int32_t CacheGhostBlocks, bool bCache,
                                    uint32_t CacheRegionPages)
{
	FPageLedgerConfig C;
	C.PagePositions = S.B;
	C.PageBytes = S.PageBytes;
	C.ContextCap = S.Cap;
	C.EngineContextCap = S.EngineCap;
	C.SequenceBudget = S.SequenceBudget;
	C.BlockCount = BlockCount;
	C.PrefixBlockCount = PrefixBlockCount;
	C.RestoreHeadroomBlocks = RestoreHeadroomBlocks;
	C.CacheGhostBlocks = CacheGhostBlocks;
	C.bCacheConfigured = bCache;
	C.CacheRegionPages = CacheRegionPages;
	return C;
}

// The GPU pooled twin: no cache region until a GPU cache exists (precache U6), so A and C are 0.
inline FPageLedgerConfig MakeGpuConfig(const FShape& S, int32_t BlockCount, int32_t PrefixBlockCount,
                                       int32_t RestoreHeadroomBlocks)
{
	return MakeConfig(S, BlockCount, PrefixBlockCount, RestoreHeadroomBlocks, 1, false, 0);
}

inline int64_t TEngineCap(const FPageLedgerConfig& C) { return C.EngineContextCap > 0 ? C.EngineContextCap : C.ContextCap; }

// The pool the test computes for a config: S + P + H + A + C, with R at the engine's cap and the
// two block sizes. With one cap this is S + k x ceil(cap / B) per region.
inline uint32_t TestPoolPages(const FPageLedgerConfig& C)
{
	const int64_t Ce = TEngineCap(C);
	const uint64_t PBlk = TPBlk2(C.PagePositions, C.ContextCap, Ce);
	const uint64_t HBlk = THBlk2(C.PagePositions, C.ContextCap, Ce);
	uint64_t Pages = static_cast<uint64_t>(C.BlockCount) * TR(C.PagePositions, Ce, C.SequenceBudget);
	Pages += static_cast<uint64_t>(C.PrefixBlockCount) * PBlk;
	Pages += static_cast<uint64_t>(C.RestoreHeadroomBlocks) * HBlk;
	if (C.bCacheConfigured)
	{
		Pages += static_cast<uint64_t>(C.CacheGhostBlocks) * PBlk;
		Pages += C.CacheRegionPages;
	}
	return static_cast<uint32_t>(Pages);
}

inline const char* AdmitName(EPageAdmit A)
{
	switch (A)
	{
	case EPageAdmit::Admitted: return "Admitted";
	case EPageAdmit::Waiting: return "Waiting";
	case EPageAdmit::KvPagesExhausted: return "KvPagesExhausted";
	case EPageAdmit::PrefixLimit: return "PrefixLimit";
	case EPageAdmit::ParentNotUsable: return "ParentNotUsable";
	case EPageAdmit::KvModeMismatch: return "KvModeMismatch";
	case EPageAdmit::PrefixPinned: return "PrefixPinned";
	case EPageAdmit::NotConfigured: return "NotConfigured";
	case EPageAdmit::InvalidRequest: return "InvalidRequest";
	case EPageAdmit::ContextCapMismatch: return "ContextCapMismatch";
	}
	return "?";
}

inline std::string Want(const std::string& What, EPageAdmit Got, EPageAdmit Wanted)
{
	return What + ": got " + AdmitName(Got) + ", want " + AdmitName(Wanted);
}

inline std::string WantN(const std::string& What, long long Got, long long Wanted)
{
	return What + ": got " + std::to_string(Got) + ", want " + std::to_string(Wanted);
}

// ---- The world: the ledger under test plus the host pool model ----

struct FWorld
{
	FPageLedgerConfig Cfg;
	FPageLedger Ledger;
	FFakePagePool Pool;
	std::vector<int> SlotHolder;
	std::map<FNodeId, int> NodeHolder;
	std::map<FCacheEntryKey, int> EntryHolder;
	std::map<FCacheEntryKey, int64_t> EntryLength;
	bool bConfigured = false;

	// The pool model is the engine's: it draws at the engine's cap. Its size is the test's own
	// figure by default; ePoolFromLedger sizes it from the ledger's PoolPages() instead, as the
	// subsystem's pool create does (needed where a mutant mis-sizes the pool itself, e.g. S).
	enum EPoolSize
	{
		ePoolFromTest,
		ePoolFromLedger
	};

	FWorld(const FPageLedgerConfig& C, ELedgerMutation Mutation = ELedgerMutation::None, EPoolSize Size = ePoolFromTest)
	    : Cfg(C), Pool(C.PagePositions, TEngineCap(C), TestPoolPages(C))
	{
		Ledger.SetMutationForTest(Mutation);
		bConfigured = Ledger.Configure(C) == ELedgerConfigure::Ok;
		if (Size == ePoolFromLedger) Pool = FFakePagePool(C.PagePositions, TEngineCap(C), Ledger.PoolPages());
		for (int32_t I = 0; I < C.BlockCount; ++I)
		{
			SlotHolder.push_back(Pool.CreateSequence(C.SequenceBudget));
		}
	}

	uint32_t Blk() const { return TPBlk2(Cfg.PagePositions, Cfg.ContextCap, TEngineCap(Cfg)); }
	uint32_t RSb() const { return TR(Cfg.PagePositions, TEngineCap(Cfg), Cfg.SequenceBudget); }
	uint32_t Free(ERegion R) const { return Ledger.RegionFree(R); }
	uint32_t Charged(ERegion R) const { return Ledger.RegionCharged(R); }

	// Form 1 (§3.9): the ledger's charges against the pool model's drawn pages, in total and per
	// attributable group of regions, plus the ledger's own pool bound and zero engine exhaustion.
	void Form1(FSoft& S, const std::string& At) const
	{
		const uint32_t Total = Ledger.TotalCharged();
		S.Expect(Total >= Pool.Drawn(), At + ": total charge " + std::to_string(Total) + " < drawn " +
		                                    std::to_string(Pool.Drawn()));
		const uint32_t SH = Ledger.RegionCharged(ERegion::S) + Ledger.RegionCharged(ERegion::H);
		S.Expect(SH >= Pool.DrawnByTag(EPageTag::Seq), At + ": S+H charge " + std::to_string(SH) +
		                                                   " < sequence pages " +
		                                                   std::to_string(Pool.DrawnByTag(EPageTag::Seq)));
		S.Expect(Ledger.RegionCharged(ERegion::P) >= Pool.DrawnByTag(EPageTag::Prefix),
		         At + ": P charge " + std::to_string(Ledger.RegionCharged(ERegion::P)) + " < prefix pages " +
		             std::to_string(Pool.DrawnByTag(EPageTag::Prefix)));
		const uint32_t AC = Ledger.RegionCharged(ERegion::A) + Ledger.RegionCharged(ERegion::C);
		S.Expect(AC >= Pool.DrawnByTag(EPageTag::Cache), At + ": A+C charge " + std::to_string(AC) +
		                                                     " < cache pages " +
		                                                     std::to_string(Pool.DrawnByTag(EPageTag::Cache)));
		S.Expect(Ledger.ChargesWithinPool(), At + ": charges exceed the pool");
		S.Expect(Total <= Ledger.PoolPages(), At + ": total charge above the pool");
		S.Expect(Pool.Exhaustions() == 0, At + ": the engine model exhausted " + std::to_string(Pool.Exhaustions()) +
		                                      " time(s)");
		S.Expect(!Pool.IsBroken(), At + ": the pool model's freeze invariant broke");
	}

	// ---- Prefixes ----

	struct FBuild
	{
		FAdmission Adm;
		int Holder = -1;
	};

	FBuild AdmitRoot(int64_t Tokens)
	{
		FBuild Bd;
		Bd.Adm = Ledger.AdmitPrefixBuild(Tokens);
		return Bd;
	}
	FBuild AdmitChild(FNodeId Parent, int64_t Tokens)
	{
		FBuild Bd;
		Bd.Adm = Ledger.AdmitNestedPrefixBuild(Parent, Tokens);
		return Bd;
	}
	// The job's Layer-1 work: begin (or begin_from), prefill, freeze.
	void RunBuild(FBuild& Bd, FNodeId Parent, int64_t Tokens)
	{
		if (Parent == InvalidNode)
		{
			Bd.Holder = Pool.PrefixBegin(Tokens, EPageTag::Prefix);
		}
		else
		{
			Bd.Holder = Pool.BeginFrom(NodeHolder.count(Parent) ? NodeHolder[Parent] : -1, Tokens, EPageTag::Prefix);
		}
		if (Bd.Holder >= 0) Pool.Freeze(Bd.Holder);
	}
	bool ApplyBuild(FBuild& Bd)
	{
		const bool bOk = Bd.Holder >= 0;
		if (bOk) NodeHolder[Bd.Adm.Node] = Bd.Holder;
		return Ledger.ApplyPrefixBuild(Bd.Adm.Ticket, bOk);
	}
	// A whole build; returns the node or InvalidNode.
	FNodeId Build(int64_t Tokens, FNodeId Parent = InvalidNode)
	{
		FBuild Bd = Parent == InvalidNode ? AdmitRoot(Tokens) : AdmitChild(Parent, Tokens);
		if (Bd.Adm.Outcome != EPageAdmit::Admitted) return InvalidNode;
		RunBuild(Bd, Parent, Tokens);
		ApplyBuild(Bd);
		return Bd.Adm.Node;
	}
	// ReleasePrefix: admit, run, apply.
	bool Release(FNodeId Node)
	{
		const FAdmission A = Ledger.AdmitPrefixRelease(Node);
		if (A.Outcome != EPageAdmit::Admitted) return false;
		if (NodeHolder.count(Node))
		{
			Pool.Release(NodeHolder[Node]);
			NodeHolder.erase(Node);
		}
		return Ledger.ApplyPrefixRelease(A.Ticket);
	}

	// ---- Adopt ----

	bool AdoptPrefix(int32_t Slot, FNodeId Node)
	{
		const FAdmission A = Ledger.AdmitAdoptPrefix(Slot, Node);
		if (A.Outcome != EPageAdmit::Admitted) return false;
		if (Slot >= 0 && Slot < static_cast<int32_t>(SlotHolder.size()) && NodeHolder.count(Node))
		{
			Pool.Adopt(SlotHolder[Slot], NodeHolder[Node]);
		}
		return Ledger.ApplyAdopt(A.Ticket, true);
	}
	EPageAdmit AdoptEntry(int32_t Slot, FCacheEntryKey Key)
	{
		const FAdmission A = Ledger.AdmitAdoptCacheEntry(Slot, Key, EntryLength[Key]);
		if (A.Outcome != EPageAdmit::Admitted) return A.Outcome;
		if (EntryHolder.count(Key) && Slot >= 0 && Slot < static_cast<int32_t>(SlotHolder.size()))
		{
			Pool.Adopt(SlotHolder[Slot], EntryHolder[Key]);
		}
		Ledger.ApplyAdopt(A.Ticket, true);
		return A.Outcome;
	}

	// ---- Cache entries, through region C's seam (the cache's residency core's role) ----

	bool BuildEntry(FCacheEntryKey Key, int64_t L)
	{
		const uint64_t Pb = Cfg.PageBytes;
		const uint32_t Peak = TR(Cfg.PagePositions, TEngineCap(Cfg), L < 1 ? 1 : L);
		if (Ledger.ChargeCacheRegionBytes(Peak * Pb) != ECacheRegionCharge::Ok) return false;
		const int H = Pool.PrefixBegin(L, EPageTag::Cache);
		if (H < 0) return false;
		Pool.Freeze(H);
		Ledger.RefundCacheRegionBytes((Peak - TOcc(Cfg.PagePositions, L)) * Pb);
		EntryHolder[Key] = H;
		EntryLength[Key] = L;
		return true;
	}
	// Eviction: the cache refunds C on the game thread; the engine release runs (here, at once).
	void EvictEntry(FCacheEntryKey Key)
	{
		Ledger.RefundCacheRegionBytes(TOcc(Cfg.PagePositions, EntryLength[Key]) * Cfg.PageBytes);
		if (EntryHolder.count(Key)) Pool.Release(EntryHolder[Key]);
		EntryHolder.erase(Key);
	}

	// ---- Restore (restore-first: the new holder is drawn, then the old one released) ----

	struct FRestoreJob
	{
		FRestoreRequest Req;
		FAdmission Adm;
		int NewHolder = -1;
		uint32_t Shared = 0;
		bool bRan = false;
	};

	FRestoreJob AdmitRestore(const FRestoreRequest& Req)
	{
		FRestoreJob J;
		J.Req = Req;
		J.Adm = Ledger.AdmitRestore(Req);
		return J;
	}
	// The job's Layer-1 work in its two phases: the restore draws its new holder, then releases
	// the old one (restore-first). Tests that interleave jobs call the phases separately.
	void RunRestoreDraw(FRestoreJob& J, bool bBytesMatch = true)
	{
		J.bRan = true;
		if (J.Req.Payload == ERestorePayload::WholeReserve)
		{
			J.NewHolder = Pool.RestoreWholeReserve();
		}
		else
		{
			int Hint = -1;
			if (J.Req.Hint == EHintKind::Prefix && NodeHolder.count(J.Req.HintNode)) Hint = NodeHolder[J.Req.HintNode];
			if (J.Req.Hint == EHintKind::CacheEntry && EntryHolder.count(J.Req.HintEntry)) Hint = EntryHolder[J.Req.HintEntry];
			J.NewHolder = Pool.RestoreBudget(J.Req.Budget, J.Req.Origin, Hint, bBytesMatch, &J.Shared);
		}
	}
	void RunRestoreRelease(FRestoreJob& J)
	{
		if (J.NewHolder >= 0 && J.Req.Slot >= 0 && J.Req.Slot < static_cast<int32_t>(SlotHolder.size()))
		{
			Pool.Release(SlotHolder[J.Req.Slot]);
			SlotHolder[J.Req.Slot] = J.NewHolder;
		}
	}
	void RunRestore(FRestoreJob& J, bool bBytesMatch = true)
	{
		RunRestoreDraw(J, bBytesMatch);
		RunRestoreRelease(J);
	}
	bool ApplyRestore(FRestoreJob& J) { return Ledger.ApplyRestore(J.Adm.Ticket, J.NewHolder >= 0, J.Shared); }

	// ---- Reset (create-first when normalizing) ----

	struct FResetJob
	{
		int32_t Slot = -1;
		FAdmission Adm;
		EResetResult Result = EResetResult::Refused;
	};
	FResetJob AdmitReset(int32_t Slot)
	{
		FResetJob J;
		J.Slot = Slot;
		J.Adm = Ledger.AdmitReset(Slot);
		return J;
	}
	// The job's Layer-1 work. bCreateRefused / bResetRefused force the engine's refusals (a seam on
	// the box), so each ApplyReset outcome can be reached.
	void RunReset(FResetJob& J, bool bCreateRefused = false, bool bResetRefused = false)
	{
		if (J.Slot < 0 || J.Slot >= static_cast<int32_t>(SlotHolder.size())) return;
		if (J.Adm.bNormalizes && !bCreateRefused)
		{
			const int New = Pool.CreateSequence(Cfg.SequenceBudget);
			if (New >= 0)
			{
				Pool.Release(SlotHolder[J.Slot]);
				SlotHolder[J.Slot] = New;
				J.Result = EResetResult::Normalized;
				return;
			}
		}
		if (bResetRefused)
		{
			J.Result = EResetResult::Refused;
			return;
		}
		Pool.Reset(SlotHolder[J.Slot]);
		J.Result = EResetResult::ResetInPlace;
	}
	bool ApplyReset(FResetJob& J) { return Ledger.ApplyReset(J.Adm.Ticket, J.Result); }
	bool Reset(int32_t Slot)
	{
		FResetJob J = AdmitReset(Slot);
		if (J.Adm.Outcome != EPageAdmit::Admitted) return false;
		RunReset(J);
		return ApplyReset(J);
	}
};

inline FRestoreRequest BudgetRestore(int32_t Slot, int64_t Budget, int64_t Origin)
{
	FRestoreRequest R;
	R.Slot = Slot;
	R.Payload = ERestorePayload::Budget;
	R.Budget = Budget;
	R.Origin = Origin;
	return R;
}
inline FRestoreRequest HintedRestore(int32_t Slot, int64_t Budget, int64_t Origin, FNodeId Hint)
{
	FRestoreRequest R = BudgetRestore(Slot, Budget, Origin);
	R.Hint = EHintKind::Prefix;
	R.HintNode = Hint;
	return R;
}
inline FRestoreRequest EntryHintedRestore(int32_t Slot, int64_t Budget, int64_t Origin, FCacheEntryKey Key, int64_t Len)
{
	FRestoreRequest R = BudgetRestore(Slot, Budget, Origin);
	R.Hint = EHintKind::CacheEntry;
	R.HintEntry = Key;
	R.HintEntryLength = Len;
	return R;
}
inline FRestoreRequest WholeReserveRestore(int32_t Slot)
{
	FRestoreRequest R;
	R.Slot = Slot;
	R.Payload = ERestorePayload::WholeReserve;
	return R;
}

// A filler restore whose settled H hold is exactly Hold pages (Hold >= 1): budget 1, an origin
// whose E makes R(1) + E - R(SequenceBudget) == Hold. Used to place H at an exact free count with
// nothing in flight (so nothing is guaranteed to return, and a boundary restore must be refused
// on arrival rather than wait).
inline int64_t OriginForHold(const FShape& S, uint32_t Hold)
{
	const uint32_t RSb = TR(S.B, S.Cap, S.SequenceBudget);
	const uint32_t R1 = TR(S.B, S.Cap, 1);
	const int64_t E = static_cast<int64_t>(Hold) + RSb - R1;
	return E * S.B;
}

// A budget restore of exactly Charge pages at admission (Charge >= R(1)): budget 1, E = Charge - R(1).
inline int64_t OriginForCharge(const FShape& S, uint32_t Charge)
{
	return static_cast<int64_t>(Charge - TR(S.B, S.Cap, 1)) * S.B;
}

}  // namespace ledgertest
