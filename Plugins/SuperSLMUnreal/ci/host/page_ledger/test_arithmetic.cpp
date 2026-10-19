// Page arithmetic, Configure and region sizing, the generation-limit pre-check, lifecycle pricing,
// and region C's seam. Plugin paging plan §2 (E1, E2, E6, E7 and the executed arithmetic), §3.1,
// §3.2, §3.3, §3.5; cells 4.2, 5, 7.1, 9.7 (arithmetic legs) and 10.1 (host leg).

#include "ledger_test_support.h"

using namespace ledgertest;
using namespace SuperSLMPaging;

LEDGER_TEST(geometry_reservation_pages, "E2; 4.2 (budget set); §2 arithmetic",
            "stub ReservationPages/PagesPerBlock return 0")
{
	for (const FShape& S : {GpuShape(), CpuShape()})
	{
		const int64_t B = S.B;
		const int64_t Cap = S.Cap;
		LT_EQ(PagesPerBlock(B, Cap), TPblk(B, Cap), std::string(S.Name) + " P_blk");
		for (int64_t Budget : {int64_t(1), B - 1, B, B + 1, int64_t(512), Cap - 1, Cap})
		{
			LT_EQ(ReservationPages(B, Cap, Budget), TR(B, Cap, Budget),
			      std::string(S.Name) + " R(" + std::to_string(Budget) + ")");
		}
		LT_EQ(ReservationPages(B, Cap, 0), 0, "R(0) is outside the budget domain");
		LT_EQ(ReservationPages(B, Cap, -1), 0, "R(-1)");
		LT_EQ(ReservationPages(B, Cap, Cap + 1), 0, "R(cap + 1)");
	}
	// The executed figures, at the plan's B = 16 and at a per-model B = 64 (the engine derives B per model).
	LT_EQ(PagesPerBlock(16, 4096), 256, "P_blk at B = 16");
	LT_EQ(ReservationPages(16, 4096, 512), 33, "R(512) at B = 16");
	LT_EQ(PagesPerBlock(64, 4096), 64, "P_blk at B = 64");
	LT_EQ(ReservationPages(64, 4096, 512), 9, "R(512) at B = 64");
	// A cap the page size does not divide has a partly empty last page (the engine rounds up).
	LT_EQ(PagesPerBlock(64, 4100), 65, "P_blk at cap 4,100, B = 64");
	LT_EQ(ReservationPages(64, 4100, 4100), 65, "R(cap) at cap 4,100");
	// The one-page-per-sequence pool option (B = cap): one page per holder.
	LT_EQ(PagesPerBlock(4096, 4096), 1, "P_blk with B = cap");
	LT_EQ(ReservationPages(4096, 4096, 1), 1, "R(1) with B = cap");
	LT_EQ(ReservationPages(4096, 4096, 4096), 1, "R(cap) with B = cap");
	LT_EQ(PagesPerBlock(0, 4096), 0, "B = 0 is not a geometry");
}

LEDGER_TEST(geometry_materialized_and_restore_charge, "E6; §3.5 restore charge; §2 rows 'private restore', 'maximum of R + E'",
            "stub MaterializedPages/RestoreAdmissionCharge return 0")
{
	LT_EQ(MaterializedPages(16, 4096, 512, 1000), 62, "E at origin 1,000, B = 16");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 512, 1000), 95, "R + E at the executed shape");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::WholeReserve, 0, 0), 256, "an SSB5 payload is charged P_blk");
	LT_EQ(MaterializedPages(64, 4096, 512, 1000), 15, "E at origin 1,000, B = 64");
	LT_EQ(RestoreAdmissionCharge(64, 4096, ERestorePayload::Budget, 512, 1000), 24, "R + E at B = 64");
	LT_EQ(RestoreAdmissionCharge(64, 4096, ERestorePayload::WholeReserve, 0, 0), 64, "SSB5 at B = 64");
	// E is clamped by P_blk - R(b).
	LT_EQ(MaterializedPages(16, 4096, 4000, 4000), 5, "E clamp: R(4000) = 251, floor(4000/16) = 250");
	// Origins {0, B-1, B, 1000, cap-1} (cell 4.5's set).
	for (int64_t O : {int64_t(0), int64_t(15), int64_t(16), int64_t(1000), int64_t(4095)})
	{
		LT_EQ(MaterializedPages(16, 4096, 512, O), TE(16, 4096, 512, O), "E at origin " + std::to_string(O));
	}
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 0, 0), 0, "budget 0 is malformed: no charge");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 4097, 0), 0, "budget > cap");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 512, 4097), 0, "origin > cap");
	LT_EQ(RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, 512, -1), 0, "origin < 0");
	// §2: the maximum of R(b) + E over budgets and origins is P_blk, and it is reached.
	uint32_t Max = 0;
	for (int64_t Budget = 1; Budget <= 4096; ++Budget)
	{
		for (int64_t Origin = 0; Origin <= 4096; Origin += 16)
		{
			const uint32_t C = RestoreAdmissionCharge(16, 4096, ERestorePayload::Budget, Budget, Origin);
			if (C > Max) Max = C;
		}
	}
	LT_EQ(Max, 256, "max over budgets and origins of R(b) + E");
}

LEDGER_TEST(geometry_prefix_and_nested_charges, "§3.3 prefixes, empty prefixes, nested; §2 rows '1,000-token prefix', 'nested child'",
            "stub PrefixBuildCharge/NestedSettledPages/FullPages return 0")
{
	LT_EQ(FullPages(16, 1000), 62, "floor(1000/16)");
	LT_EQ(OccupiedPages(16, 1000), 63, "ceil(1000/16)");
	LT_EQ(PrefixBuildCharge(16, 4096, 1000), 64, "R(1000) at B = 16");
	LT_EQ(PrefixBuildCharge(16, 4096, 0), 2, "an empty prefix is begun with budget 1: R(1) = 2");
	LT_EQ(OccupiedPages(16, 0), 0, "an empty prefix settles to 0");
	LT_EQ(PrefixBuildCharge(16, 4096, -1), 0, "negative length");
	LT_EQ(PrefixBuildCharge(16, 4096, 4097), 0, "length above the cap");
	LT_EQ(PrefixBuildCharge(16, 4096, 200), 14, "a 200-token child peaks at R(200) = 14");
	LT_EQ(NestedSettledPages(16, 1000, 200), 13, "and settles to 13 private pages");
	LT_EQ(NestedSettledPages(16, 0, 1000), 63, "a child of an empty parent is a root");
	LT_EQ(NestedSettledPages(16, 1000, 0), 1, "an empty child of a mid-page parent holds the copied tail page");
	LT_EQ(NestedSettledPages(16, 992, 0), 0, "an empty child of a page-aligned parent holds nothing");
	for (int64_t L : {int64_t(0), int64_t(1), int64_t(15), int64_t(16), int64_t(17), int64_t(1000), int64_t(4095)})
	{
		LT_EQ(PrefixBuildCharge(16, 4096, L), TR(16, 4096, L < 1 ? 1 : L), "R(max(L, 1)) at L = " + std::to_string(L));
	}
}

// Emits from a holder resting ready at C until the pre-check refuses; returns the count and the
// stopping verdict.
static int64_t EmitToStop(int64_t C, int64_t Limit, int64_t Cap, ELimitVerdict* Stop)
{
	bool bReady = true;
	int64_t Emitted = 0;
	for (int64_t Guard = 0; Guard < 2 * Cap + 8; ++Guard)
	{
		if (bReady)
		{
			const ELimitVerdict V = CheckStep(C, true, Limit, Cap);
			if (V != ELimitVerdict::Allowed)
			{
				*Stop = V;
				return Emitted;
			}
			++Emitted;
			bReady = false;
		}
		else
		{
			const ELimitVerdict V = CheckStep(C, false, Limit, Cap);
			if (V != ELimitVerdict::Allowed)
			{
				*Stop = V;
				return Emitted;
			}
			++C;
			bReady = true;
		}
	}
	*Stop = ELimitVerdict::Allowed;
	return Emitted;
}

static FSoft TokenLawScenario(ELimitMutation Mutation)
{
	FSoft S;
	SetLimitMutationForTest(Mutation);
	for (const FShape& Sh : {GpuShape(), CpuShape()})
	{
		const int64_t B = Sh.B;
		const int64_t Cap = Sh.Cap;
		for (int64_t Budget : {int64_t(1), B - 1, B, B + 1, int64_t(512), Cap - 1, Cap})
		{
			// A plain holder: one prompt token, resting ready at c = 1.
			const FHolderShape Plain{EHolderMode::Budget, Budget, 0};
			const int64_t Limit = HolderLimit(Cap, Plain);
			S.Expect(Limit == (Budget < Cap ? Budget : Cap), "plain holder limit at budget " + std::to_string(Budget));
			ELimitVerdict Stop = ELimitVerdict::Allowed;
			const int64_t N = EmitToStop(1, Limit, Cap, &Stop);
			S.Expect(N == 1 + (Limit - 1), WantN("plain holder emitted at budget " + std::to_string(Budget), N, Limit));
			S.Expect(Stop == (Limit == Cap ? ELimitVerdict::ContextCapExceeded : ELimitVerdict::KvBudgetExceeded),
			         "plain holder stop outcome at budget " + std::to_string(Budget));
			// An adopter of a ready prefix of length 100: origin 100, resting ready at c = 100.
			const FHolderShape Adopter{EHolderMode::Budget, Budget, 100};
			const int64_t ALimit = HolderLimit(Cap, Adopter);
			const int64_t AWant = (100 + Budget < Cap) ? 100 + Budget : Cap;
			S.Expect(ALimit == AWant, "adopter limit at budget " + std::to_string(Budget));
			const int64_t AN = EmitToStop(100, ALimit, Cap, &Stop);
			S.Expect(AN == 1 + (AWant - 100), WantN("adopter emitted at budget " + std::to_string(Budget), AN, 1 + (AWant - 100)));
			if (100 + Budget <= Cap)
			{
				S.Expect(AN == Budget + 1, "an adopter of a ready prefix emits b + 1");
			}
		}
	}
	SetLimitMutationForTest(ELimitMutation::None);
	return S;
}

LEDGER_TEST(limit_token_law_exact_counts, "4.2 [K2/K3, GPU twin K6/K7]; E7; §7 'exact count at the limit'",
            "stub HolderLimit returns 0 and CheckStep always allows")
{
	LT_HOLDS(TokenLawScenario(ELimitMutation::None), "E7's 1 + (limit - c)");
	LT_KILLS(TokenLawScenario(ELimitMutation::OneLate), "pre-check <= in place of < (one late)");
	LT_KILLS(TokenLawScenario(ELimitMutation::OneEarly), "limit - 1 (one early)");
}

LEDGER_TEST(limit_activation_precheck, "5 KvBudgetExceeded at activation [K2/K6]; §3.4 activation pre-check",
            "stub CheckActivation always allows")
{
	LT_REQUIRE(CheckActivation(10, 5, 15, 4096) == ELimitVerdict::Allowed, "prompt reaching the limit exactly is allowed");
	LT_REQUIRE(CheckActivation(10, 6, 15, 4096) == ELimitVerdict::KvBudgetExceeded, "one past the limit");
	LT_REQUIRE(CheckActivation(4090, 7, 4096, 4096) == ELimitVerdict::ContextCapExceeded, "at limit == cap the cap outcome fires");
	LT_REQUIRE(CheckActivation(0, 0, 0, 4096) == ELimitVerdict::Allowed, "empty prompt at limit 0");
}

LEDGER_TEST(limit_restored_holder_keeps_saved_limit, "9.7 [K3] arithmetic leg; §3.3 generation limit (restored holder's own b)",
            "stub HolderLimit returns 0")
{
	// b < SequenceBudget and b > SequenceBudget: the saved limit, not the configured one.
	LT_EQ(HolderLimit(4096, FHolderShape{EHolderMode::Budget, 256, 1000}), 1256, "saved b = 256 at origin 1,000");
	LT_EQ(HolderLimit(4096, FHolderShape{EHolderMode::Budget, 1024, 1000}), 2024, "saved b = 1,024 at origin 1,000");
	LT_EQ(HolderLimit(4096, FHolderShape{EHolderMode::Budget, 512, 3900}), 4096, "origin + b above the cap clamps to the cap");
	LT_EQ(HolderLimit(4096, FHolderShape{EHolderMode::WholeReserve, 4096, 0}), 4096, "a whole-reserve holder's limit is the cap");
}

LEDGER_TEST(pricing_budget_mode_three_shapes, "7.1 [K2]; §3.3 lifecycle pricing",
            "stub BudgetModeLifecyclePricedBytes returns 0")
{
	LT_EQ(BudgetModeLifecyclePricedBytes(16, 4096, 512, 96ull * 1024), 33ull * 96 * 1024, "B = 16, budget 512, 96 KiB pages");
	LT_EQ(BudgetModeLifecyclePricedBytes(64, 4096, 512, 384ull * 1024), 9ull * 384 * 1024, "B = 64, budget 512, 384 KiB pages");
	LT_EQ(BudgetModeLifecyclePricedBytes(64, 32768, 1024, 896ull * 1024), 17ull * 896 * 1024, "1.5B: B = 64, budget 1,024");
	LT_EQ(BudgetModeLifecyclePricedBytes(64, 4096, 0, 384ull * 1024), 0, "budget 0 is legacy: not budget-mode pricing");
}

LEDGER_TEST(configure_regions_and_pool, "§3.2 region table and pool; §2 row '50 sequences' (2,162 pages); 10.1 host leg",
            "stub Configure sizes nothing: capacities and PoolPages are 0")
{
	{
		FPageLedger L;
		const FPageLedgerConfig C = MakeConfig(GpuShape(), 50, 1, 1, 1, false, 0);
		LT_REQUIRE(L.Configure(C) == ELedgerConfigure::Ok, "Configure");
		LT_REQUIRE(L.IsConfigured(), "configured after Ok");
		LT_EQ(L.RegionCapacity(ERegion::S), 1650, "S = 50 x R(512)");
		LT_EQ(L.RegionCapacity(ERegion::P), 256, "P = 1 x P_blk");
		LT_EQ(L.RegionCapacity(ERegion::H), 256, "H = 1 x P_blk");
		LT_EQ(L.RegionCapacity(ERegion::A), 0, "A = 0 without a cache");
		LT_EQ(L.RegionCapacity(ERegion::C), 0, "C = 0 without a cache");
		LT_EQ(L.PoolPages(), 2162, "the executed 2,162 pages");
		LT_EQ(L.RegionCharged(ERegion::S), 1650, "S is fully charged by the warm sequences");
		LT_EQ(L.TotalCharged(), 1650, "nothing else is charged at Configure");
		for (int32_t Slot = 0; Slot < 50; ++Slot)
		{
			const FHolderShape Sh = L.SlotShape(Slot);
			LT_REQUIRE(Sh.Mode == EHolderMode::Budget && Sh.Budget == 512 && Sh.Origin == 0, "every slot starts normalized");
		}
		const FPageLedgerStats St = L.GetStats();
		LT_REQUIRE(St.bConfigured && St.PoolPages == 2162 && St.FreeHeadroomPages == 256, "GetStats mirrors the regions");
	}
	{
		FPageLedger L;
		const FPageLedgerConfig C = MakeConfig(GpuShape(), 50, 1, 1, 1, true, 1280);
		LT_REQUIRE(L.Configure(C) == ELedgerConfigure::Ok, "Configure with a cache");
		LT_EQ(L.RegionCapacity(ERegion::A), 256, "A = CacheGhostBlocks x P_blk");
		LT_EQ(L.RegionCapacity(ERegion::C), 1280, "C as given");
		LT_EQ(L.PoolPages(), 2162 + 256 + 1280, "S + P + H + A + C");
	}
	{
		FPageLedger L;
		LT_REQUIRE(L.Configure(MakeConfig(CpuShape(), 50, 1, 1, 1, false, 0)) == ELedgerConfigure::Ok, "Configure at B = 64");
		LT_EQ(L.PoolPages(), 50 * 9 + 64 + 64, "S + P + H at B = 64");
	}
	{
		// 10.1's arithmetic on the ledger: 50 adopters of a 1,000-token prefix hold 1,650 + 63.
		FWorld W(MakeConfig(GpuShape(), 50, 1, 1, 1, false, 0));
		LT_REQUIRE(W.bConfigured, "Configure");
		const FNodeId N = W.Build(1000);
		LT_REQUIRE(N != InvalidNode, "build the persona prefix");
		for (int32_t Slot = 0; Slot < 50; ++Slot) LT_REQUIRE(W.AdoptPrefix(Slot, N), "adopt");
		LT_EQ(W.Ledger.TotalCharged(), 1713, "1,650 + 63 charged");
		LT_EQ(W.Pool.Drawn(), 1713, "and the pool model draws exactly that");
	}
}

LEDGER_TEST(configure_and_sharing_figures_cpu_b64, "§2 per-backend table: the CPU's 578 pages (50 sequences at B = 64) and 1,474 at the 1.5B artifact; 10.1 host leg at B = 64: 450 + 16 = 466; K0's suite note; the plan's host-suite item for the CPU's B = 64 legs",
            "none: green on arrival (figures the core already computes; pinned so they cannot drift)")
{
	{
		FPageLedger L;
		LT_REQUIRE(L.Configure(MakeConfig(CpuShape(), 50, 1, 1, 1, false, 0)) == ELedgerConfigure::Ok, "Configure, CPU 0.5B");
		LT_EQ(L.RegionCapacity(ERegion::S), 450, "S = 50 x R(512) = 50 x 9");
		LT_EQ(L.RegionCapacity(ERegion::P), 64, "P = 1 x 64");
		LT_EQ(L.RegionCapacity(ERegion::H), 64, "H = 1 x 64");
		LT_EQ(L.PoolPages(), 578, "578 pages = 216.8 MiB at 384 KiB");
	}
	{
		FPageLedger L;
		LT_REQUIRE(L.Configure(MakeConfig(Cpu15Shape(), 50, 1, 1, 1, false, 0)) == ELedgerConfigure::Ok, "Configure, CPU 1.5B (cap 32,768)");
		LT_EQ(L.RegionCapacity(ERegion::S), 450, "S = 50 x 9");
		LT_EQ(L.RegionCapacity(ERegion::P), 512, "P_blk = H_blk = 512 (Cp = Ce)");
		LT_EQ(L.RegionCapacity(ERegion::H), 512, "H");
		LT_EQ(L.PoolPages(), 1474, "1,474 pages = 1,289.8 MiB at 896 KiB");
	}
	for (const FShape& Sh : {CpuShape(), Cpu15Shape()})
	{
		FWorld W(MakeConfig(Sh, 50, 1, 1, 1, false, 0));
		LT_REQUIRE(W.bConfigured, "Configure");
		const FNodeId N = W.Build(1000);
		LT_REQUIRE(N != InvalidNode, "the 1,000-token persona");
		for (int32_t Slot = 0; Slot < 50; ++Slot) LT_REQUIRE(W.AdoptPrefix(Slot, N), "adopt");
		LT_EQ(W.Ledger.TotalCharged(), 466, std::string(Sh.Name) + ": 450 + 16 charged");
		LT_EQ(W.Pool.Drawn(), 466, std::string(Sh.Name) + ": and the pool model draws 466, not 50 x 16 more");
	}
}

LEDGER_TEST(configure_refusals, "5 InvalidSequenceBudget [K2/K6]; §3.1 configure refusals",
            "stub Configure accepts everything")
{
	FPageLedger L;
	FPageLedgerConfig C = MakeConfig(GpuShape(), 4, 1, 1, 1, false, 0);
	C.SequenceBudget = -1;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidSequenceBudget, "budget -1");
	LT_REQUIRE(!L.IsConfigured(), "a refused Configure leaves no ledger");
	C.SequenceBudget = 4097;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidSequenceBudget, "budget cap + 1");
	C.SequenceBudget = 0;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::LegacyMode, "budget 0 is legacy: no ledger");
	LT_REQUIRE(!L.IsConfigured(), "legacy mode has no ledger");
	C.SequenceBudget = 512;
	C.PagePositions = 0;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidGeometry, "B = 0");
	C.PagePositions = 16;
	C.PageBytes = 0;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidGeometry, "page bytes 0");
	C.PageBytes = 96 * 1024;
	C.BlockCount = -1;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidGeometry, "negative BlockCount");
	C.BlockCount = 0x7fffffff;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::InvalidGeometry, "a pool above UINT32_MAX pages");
	C.BlockCount = 4;
	C.RestoreHeadroomBlocks = 0;
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::Ok, "zero headroom is a valid configuration");
	LT_EQ(L.RegionCapacity(ERegion::H), 0, "H = 0");
}

LEDGER_TEST(configure_gpu_cache_regions_zero_until_cache, "§3.2 CacheRegionPages() hook; §3.4 pool S + P + H + CacheRegionPages(); [K6]",
            "stub Configure sizes nothing")
{
	FPageLedger L;
	FPageLedgerConfig C = MakeGpuConfig(GpuShape(), 8, 2, 1);
	C.CacheGhostBlocks = 3;      // ignored: no GPU cache yet
	C.CacheRegionPages = 999;    // ignored: no GPU cache yet
	LT_REQUIRE(L.Configure(C) == ELedgerConfigure::Ok, "Configure");
	LT_EQ(L.RegionCapacity(ERegion::A), 0, "A is zero-sized until a GPU cache exists");
	LT_EQ(L.RegionCapacity(ERegion::C), 0, "C is zero-sized until a GPU cache exists");
	LT_EQ(L.PoolPages(), 8 * 33 + 2 * 256 + 256, "pool = S + P + H");
	LT_EQ(CacheRegionPagesFor(100ull << 20, 24ull << 20, 256), 5 * 256, "ceil(budget / block bytes) x P_blk");
	LT_EQ(CacheRegionPagesFor(48ull << 20, 24ull << 20, 256), 2 * 256, "an exact multiple");
	LT_EQ(CacheRegionPagesFor(0, 24ull << 20, 256), 0, "no cache budget");
}

LEDGER_TEST(cache_region_seam, "§3.2 region C; LoRA residency plan §5.0 'Who counts prefix pages' (the prefix kind's charged bytes ARE region C's charge)",
            "stub seam answers NotConfigured and reports 0 capacity")
{
	FPageLedger L;
	const FShape S = GpuShape();
	LT_REQUIRE(L.Configure(MakeConfig(S, 4, 1, 1, 1, true, 512)) == ELedgerConfigure::Ok, "Configure");
	ISuperSLMCacheRegionCharge& Seam = L;
	const uint64_t Pb = S.PageBytes;
	LT_EQ(Seam.CacheRegionCapacityBytes(), 512 * Pb, "capacity in bytes");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(100 * Pb) == ECacheRegionCharge::Ok, "charge 100 pages");
	LT_EQ(Seam.CacheRegionChargedBytes(), 100 * Pb, "charged bytes read back");
	LT_EQ(L.RegionCharged(ERegion::C), 100, "region C's charge IS the seam's charge");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(Pb + 1) == ECacheRegionCharge::InvalidRequest, "not a whole page");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(413 * Pb) == ECacheRegionCharge::Exhausted, "one page past C");
	LT_EQ(L.RegionCharged(ERegion::C), 100, "an exhausted charge takes nothing");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(412 * Pb) == ECacheRegionCharge::Ok, "exactly fills C");
	LT_REQUIRE(Seam.RefundCacheRegionBytes(513 * Pb) == ECacheRegionCharge::InvalidRequest, "refund above the charge");
	LT_REQUIRE(Seam.RefundCacheRegionBytes(512 * Pb) == ECacheRegionCharge::Ok, "refund all");
	LT_EQ(L.RegionCharged(ERegion::C), 0, "C empty");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(10 * Pb) == ECacheRegionCharge::Ok, "charge again");
	L.EndEpoch(EEpochEnd::Teardown);
	LT_EQ(Seam.CacheRegionChargedBytes(), 0, "the charge goes with the pool");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(Pb) == ECacheRegionCharge::NotConfigured, "no ledger, no charge");
	// Without a cache, region C is zero-sized.
	LT_REQUIRE(L.Configure(MakeConfig(S, 4, 1, 1, 1, false, 512)) == ELedgerConfigure::Ok, "Configure without a cache");
	LT_REQUIRE(Seam.ChargeCacheRegionBytes(Pb) == ECacheRegionCharge::Exhausted, "C is 0 without a cache");
}
