// The cache core's charge rule, eviction, pins, leases and coalescing (precache plan §5.4, §5.6).

#include "cache_fixture.h"

#include <set>

using namespace SuperSLMCore;
using namespace HostSim;

HOST_TEST(cache_request_refusals, "§5.6 synchronous refusals; 5.4 (nested row)")
{
	int64_t Counter = 1;
	FSuperSLMPrefixCacheCore Unconfigured(&Counter);
	int64_t Handle = -1;
	std::string Error;
	EXPECT_EQ(Unconfigured.Request(FixtureSpec(1), Handle, Error), EPrefixRequestResult::NotConfigured);

	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	FPrefixSpec Empty;
	EXPECT_EQ(F.Cache.Request(Empty, Handle, Error), EPrefixRequestResult::Malformed);
	EXPECT_EQ(Error.empty(), false);
	FPrefixSpec Oov = FixtureSpec(1);
	Oov.Tokens[3] = F.Config.VocabSize;
	EXPECT_EQ(F.Cache.Request(Oov, Handle, Error), EPrefixRequestResult::Malformed);
	Oov.Tokens[3] = -1;
	EXPECT_EQ(F.Cache.Request(Oov, Handle, Error), EPrefixRequestResult::Malformed);
	EXPECT_EQ(F.Cache.Request(FixtureSpec(1, 65), Handle, Error), EPrefixRequestResult::Malformed); // over cap 64
	F.Sim->Pull();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ComputeKey), static_cast<size_t>(0));

	FPrefixSpec Nested = FixtureSpec(1);
	Nested.ParentHandle = 12345;
	F.Cache.TakeLog();
	EXPECT_EQ(F.Cache.Request(Nested, Handle, Error), EPrefixRequestResult::NestedRequiresBudgetMode);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::NestedRequiresBudgetMode), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::NestedRequiresBudgetMode), static_cast<size_t>(1));

	const int64_t Expected = F.Counter;
	EXPECT_EQ(F.Cache.Request(FixtureSpec(1, 64), Handle, Error), EPrefixRequestResult::Accepted); // at cap is fine
	EXPECT_EQ(Handle, Expected);
	EXPECT_EQ(F.Cache.GetLeaseState(Handle), EPrefixLeaseState::Pending);
	EXPECT_EQ(F.Cache.GetSource(Handle), EPrefixSource::Pending);
}

HOST_TEST(cache_one_handle_per_lease, "§5.6 one handle per lease (ids from the plugin's prefix-id counter)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Counter = 700;
	const int64_t A = F.Request(FixtureSpec(1));
	const int64_t CreatePrefixId = F.Counter++; // CreatePrefix minting from the same counter
	const int64_t B = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	const int64_t C = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	EXPECT_EQ(A, 700);
	EXPECT_EQ(B, 702);
	EXPECT_EQ(C, 703);
	EXPECT_EQ(CreatePrefixId, 701);
	for (int64_t H : {A, B, C})
	{
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	}
	EXPECT_EQ(F.Cache.GetLeaseState(CreatePrefixId), EPrefixLeaseState::Unknown);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
}

HOST_TEST(cache_key_is_computed_by_a_command, "7.8 (host half: RequestPrefix hashes nothing; the key arrives as a completion)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(5, 32, "barter");
	const int64_t H = F.Request(Spec);
	REQUIRE_NE(H, 0);
	F.Sim->Pull();
	REQUIRE_EQ(F.Sim->Queue.size(), static_cast<size_t>(1));
	const FPrefixCommand& C = F.Sim->Queue.front();
	EXPECT_EQ(C.Kind, EPrefixCommandKind::ComputeKey);
	EXPECT_EQ(C.LeaseHandle, H);
	EXPECT_EQ(C.KeyInputs.Tokens == Spec.Tokens, true);
	EXPECT_EQ(C.KeyInputs.SchemaName, Spec.SchemaName);
	EXPECT_EQ(C.KeyInputs.ArtifactHash == F.Config.Identity.ArtifactHash, true);
	EXPECT_EQ(C.KeyInputs.Layer1Tag, F.Config.Identity.Layer1Tag);
	EXPECT_EQ(C.KeyInputs.Layer1Commit, F.Config.Identity.Layer1Commit);
	EXPECT_EQ(C.KeyInputs.BlobMagic, std::string("SPX1"));
	EXPECT_EQ(C.KeyInputs.bHasParent, false);
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Pending);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
}

HOST_TEST(cache_build_is_charged_its_peak_before_it_draws, "§5.4 charge rule (build R(L), falls to ceil(L/B) at freeze); 8.10(c) host half")
{
	FCacheFixture F(8, false); // 17 pages: the four entries and the last build's peak fit without eviction
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t PB = F.Config.PageBytes;
	struct FCase { int32_t Length; uint64_t Peak; uint64_t Steady; };
	// R(L) = min(ceil(L/B) + 1, ceil(cap/B)) at B = 16, cap = 64.
	const FCase Cases[] = {{32, 3, 2}, {20, 3, 2}, {64, 4, 4}, {1, 2, 1}};
	int32_t Index = 10;
	for (const FCase& Case : Cases)
	{
		F.Sim->Held = {EPrefixCommandKind::Build};
		const uint64_t ChargedBefore = F.Diag().ChargedBytes;
		const uint64_t DrawnBefore = F.Sim->CacheDrawnPages;
		const int64_t H = F.Request(FixtureSpec(Index++, Case.Length));
		F.Sim->RunAll();
		REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
		EXPECT_EQ(F.Diag().InFlightBytes, Case.Peak * PB);
		EXPECT_EQ(F.Diag().ChargedBytes - ChargedBefore, Case.Peak * PB);
		EXPECT_EQ(F.Sim->CacheDrawnPages, DrawnBefore); // charged, nothing drawn yet
		F.Sim->Held.clear();
		F.Sim->RunAll();
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
		EXPECT_EQ(F.Diag().InFlightBytes, 0u);
		EXPECT_EQ(F.Diag().ChargedBytes - ChargedBefore, Case.Steady * PB);
		EXPECT_EQ(F.Sim->CacheDrawnPages - DrawnBefore, Case.Steady);
		EXPECT_LE(F.Sim->CacheDrawnPages * PB, F.Diag().ChargedBytes);
		F.Cache.Return(H);
	}
	EXPECT_EQ(F.Diag().BuildsIssued, 4u);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_restore_is_charged_before_it_draws_and_settled_on_apply, "§5.4 charge rule (restore ceil(L/B) - s, s read as bookkeeping)")
{
	FCacheFixture F(4, true);
	REQUIRE_TRUE(F.bConfigured);
	const uint64_t PB = F.Config.PageBytes;
	const FPrefixSpec Spec = FixtureSpec(1);
	F.PutDiskFile(Spec);
	F.Sim->Held = {EPrefixCommandKind::Restore};
	const int64_t H = F.Request(Spec);
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Restore), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->Queue.front().ContextLength, 32);
	EXPECT_EQ(F.Diag().InFlightBytes, 2 * PB); // ceil(32/16) - 0, charged before the draw
	EXPECT_EQ(F.Sim->CacheDrawnPages, 0u);
	F.Sim->RestoreSharedPages = 1; // the engine reports s = 1
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Disk);
	EXPECT_EQ(F.Diag().ResidentBytes, 1 * PB);
	EXPECT_EQ(F.Diag().ChargedBytes, 1 * PB);
	EXPECT_EQ(F.Diag().RestoresIssued, 1u);
	EXPECT_EQ(F.Diag().BuildsIssued, 0u);
}

HOST_TEST(cache_failed_build_returns_its_whole_charge, "§5.4 (a refused or failed build returns its whole charge)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->bFailBuilds = true;
	const int64_t H = F.Request(FixtureSpec(1));
	REQUIRE_NE(H, 0);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Failed);
	EXPECT_EQ(F.Diag().ChargedBytes, 0u);
	EXPECT_EQ(F.Diag().ResidentEntries, 0u);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	// The key is free again: a later request builds afresh.
	F.Sim->bFailBuilds = false;
	EXPECT_NE(F.RequestReady(FixtureSpec(1)), 0);
}

HOST_TEST(cache_every_entry_pinned_and_one_more_is_budget_exhausted, "5.5, 7.10 (no pinned exception)")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	const int64_t B = F.RequestReady(FixtureSpec(2));
	REQUIRE_TRUE(A != 0 && B != 0); // both leased, so both pinned
	const uint64_t Drawn = F.Sim->CacheDrawnPages;
	const uint64_t Charged = F.Diag().ChargedBytes;
	F.Cache.TakeLog();
	const int64_t C = F.Request(FixtureSpec(3));
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::ComputeKey));
	// No wait: it resolves at the apply that admits it.
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::BudgetExhausted);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::BudgetExhausted);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0)); // nothing evicted
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));   // nothing drawn for C
	EXPECT_EQ(F.Sim->CacheDrawnPages, Drawn);
	EXPECT_EQ(F.Diag().ChargedBytes, Charged);
	EXPECT_LE(F.Diag().ChargedBytes, F.Config.BudgetBytes);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::BudgetExhausted), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::BudgetExhausted), static_cast<size_t>(1));
	EXPECT_EQ(F.Cache.GetLeaseState(A), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::Ready);
}

HOST_TEST(cache_release_before_reuse, "§5.4 (every page counted is held or may be drawn); LoRA §5.3 release before reuse")
{
	FCacheFixture F(1, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	const uint64_t EngineA = F.Sim->EngineHandleOf(A);
	REQUIRE_TRUE(F.Cache.Return(A));
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t B = F.Request(FixtureSpec(2));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->Queue.front().EngineHandle, EngineA);
	// A's pages are still held, so B may not draw yet.
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::Pending);
	EXPECT_LE(F.Diag().ChargedBytes, F.Config.BudgetBytes);
	EXPECT_GE(F.Diag().ReleasingBytes, 2 * F.Config.PageBytes);
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(2));
	EXPECT_LE(F.Sim->PeakCacheDrawnPages, F.BudgetPages());
	EXPECT_EQ(F.Diag().Evictions, 1u);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_pool_exhausted_evicts_and_retries, "§5.4 the pool is the hard limit (evict and retry)")
{
	FCacheFixture F(4, false, 0);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	const int64_t B = F.RequestReady(FixtureSpec(2));
	REQUIRE_TRUE(A != 0 && B != 0);
	const uint64_t EngineA = F.Sim->EngineHandleOf(A);
	F.Cache.Return(A);
	F.Cache.Return(B);
	// Something outside the cache holds pages: 2 free, a build needs R(32) = 3. The ledger has room.
	F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 2;
	const int64_t C = F.Request(FixtureSpec(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(4)); // A, B, C refused, C retried
	const std::vector<uint64_t> Released = F.Released();
	REQUIRE_EQ(Released.size(), static_cast<size_t>(1));
	EXPECT_EQ(Released[0], EngineA); // the LRU unpinned entry
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::PoolExhausted), 0u);
	EXPECT_LE(F.Diag().ChargedBytes, F.Config.BudgetBytes);
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_pool_exhausted_with_nothing_evictable_is_a_named_refusal, "§5.4, 5.4 (PoolExhausted row), 5.5 (PoolExhausted reached from outside the cache)")
{
	FCacheFixture F(4, false, 0);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	const int64_t B = F.RequestReady(FixtureSpec(2));
	REQUIRE_TRUE(A != 0 && B != 0); // leased: pinned
	const uint64_t Charged = F.Diag().ChargedBytes;
	F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 2;
	F.Cache.TakeLog();
	const int64_t C = F.Request(FixtureSpec(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::PoolExhausted);
	EXPECT_EQ(F.Diag().ChargedBytes, Charged); // its charge returned
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::PoolExhausted), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::PoolExhausted), static_cast<size_t>(1));
}

HOST_TEST(cache_a_save_in_flight_pins_its_entry, "§5.4 pins (a save of it)")
{
	FCacheFixture F(1, true);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::Save};
	const int64_t A = F.Request(FixtureSpec(1));
	F.Sim->RunAll();
	REQUIRE_EQ(F.Cache.GetLeaseState(A), EPrefixLeaseState::Ready);
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Save), static_cast<size_t>(1));
	REQUIRE_TRUE(F.Cache.Return(A));
	const int64_t B = F.Request(FixtureSpec(2));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(B), EPrefixLeaseState::BudgetExhausted); // A is pinned by its save
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(0));
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::WriteDisk), static_cast<size_t>(1));
	const int64_t C = F.Request(FixtureSpec(3));
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(C), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->Violations.empty(), true);
}

HOST_TEST(cache_memory_hit_oracle, "7.13 (memory hit)")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(1, 32, "quest");
	const int64_t A = F.RequestReady(Spec);
	REQUIRE_NE(A, 0);
	const uint64_t Builds = F.Diag().BuildsIssued;
	const uint64_t Restores = F.Diag().RestoresIssued;
	const int64_t A2 = F.Request(Spec);
	REQUIRE_NE(A2, 0);
	EXPECT_EQ(F.Cache.GetLeaseState(A2), EPrefixLeaseState::Pending); // never delivered inside the call
	REQUIRE_TRUE(F.Sim->RunOneOfKind(EPrefixCommandKind::ComputeKey));
	EXPECT_EQ(F.Cache.GetLeaseState(A2), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(A2), EPrefixSource::Memory);
	EXPECT_NE(A2, A);
	EXPECT_EQ(F.Diag().BuildsIssued, Builds);
	EXPECT_EQ(F.Diag().RestoresIssued, Restores);
	EXPECT_EQ(F.Diag().ServedBySource[static_cast<int>(EPrefixSource::Memory)], 1u);
	const FEngineHandle* Handle = F.Sim->HandleOf(A2);
	REQUIRE_TRUE(Handle != nullptr);
	EXPECT_EQ(Handle->Tokens == Spec.Tokens, true); // the adopter's operand equals the twin's
	EXPECT_EQ(Handle->Schema, Spec.SchemaName);
}

HOST_TEST(cache_lru_oracle, "7.13 (LRU: touched neither in insertion order nor its reverse)")
{
	FCacheFixture F(4, false);
	REQUIRE_TRUE(F.bConfigured);
	uint64_t Engine[4] = {};
	for (int I = 0; I < 4; ++I)
	{
		const int64_t H = F.RequestReady(FixtureSpec(I));
		REQUIRE_NE(H, 0);
		Engine[I] = F.Sim->EngineHandleOf(H);
		F.Cache.Return(H);
	}
	// Touch C, A, D, B (indices 2, 0, 3, 1) by memory hits. LRU is now C.
	for (int I : {2, 0, 3, 1})
	{
		const int64_t H = F.RequestReady(FixtureSpec(I));
		REQUIRE_NE(H, 0);
		EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Memory);
		F.Cache.Return(H);
	}
	const int64_t E = F.RequestReady(FixtureSpec(9));
	EXPECT_NE(E, 0);
	const std::vector<uint64_t> Released = F.Released();
	REQUIRE_EQ(Released.size(), static_cast<size_t>(1));
	EXPECT_EQ(Released[0], Engine[2]); // not B (most recent), not A (inserted first)
}

HOST_TEST(cache_eviction_skips_an_entry_with_a_queued_adopt, "3.6")
{
	FCacheFixture F(3, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	const int64_t B = F.RequestReady(FixtureSpec(2));
	const int64_t C = F.RequestReady(FixtureSpec(3));
	REQUIRE_TRUE(A != 0 && B != 0 && C != 0);
	const uint64_t EngineB = F.Sim->EngineHandleOf(B);
	uint64_t EngineA = 0;
	uint64_t Token = 0;
	REQUIRE_EQ(F.Cache.BeginAdopt(A, EngineA, Token), EPrefixAdoptCheck::Adoptable); // the adopt is queued
	F.Cache.Return(A);
	F.Cache.Return(B);
	F.Cache.Return(C);
	// A is the LRU entry and has no lease, but its adopt is queued: the eviction takes B.
	const int64_t D = F.RequestReady(FixtureSpec(4));
	EXPECT_NE(D, 0);
	const std::vector<uint64_t> Released = F.Released();
	REQUIRE_EQ(Released.size(), static_cast<size_t>(1));
	EXPECT_EQ(Released[0], EngineB);
	EXPECT_EQ(F.Sim->Handles[EngineA].bReleased, false); // the adopt can still run
	EXPECT_EQ(F.Cache.EndAdopt(Token), true);
	EXPECT_EQ(F.Cache.EndAdopt(Token), false);
}

HOST_TEST(cache_coalesces_sixteen_requests_into_one_build, "3.5")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	std::set<int64_t> Handles;
	for (int I = 0; I < 16; ++I)
	{
		Handles.insert(F.Request(FixtureSpec(1)));
	}
	EXPECT_EQ(Handles.size(), static_cast<size_t>(16));
	EXPECT_EQ(Handles.count(0), static_cast<size_t>(0));
	const int Steps = F.Sim->RunAll();
	EXPECT_LE(Steps, 64);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	size_t Ready = 0;
	for (int64_t H : Handles)
	{
		Ready += F.Cache.GetLeaseState(H) == EPrefixLeaseState::Ready ? 1 : 0;
	}
	EXPECT_EQ(Ready, static_cast<size_t>(16));
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
}

HOST_TEST(cache_coalesced_shared_load_fails_then_recomputes, "3.5 (the shared load fails: a corrupt disk file, then recompute)")
{
	FCacheFixture F(2, true);
	REQUIRE_TRUE(F.bConfigured);
	const FPrefixSpec Spec = FixtureSpec(1);
	F.PutDiskFile(Spec);
	const std::string Path = F.Sim->DiskPath(FixtureKey(Spec));
	REQUIRE_TRUE(F.Sim->Fs->Exists(Path));
	F.Sim->Fs->Files[Path].Data[4] = 2; // wrapper version 2
	F.Cache.TakeLog();
	std::vector<int64_t> Handles;
	for (int I = 0; I < 16; ++I)
	{
		Handles.push_back(F.Request(Spec));
	}
	EXPECT_LE(F.Sim->RunAll(), 96);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ReadDisk), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::RemoveDiskFile), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	for (int64_t H : Handles)
	{
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
		EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Recomputed);
	}
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::WrapperUnsupportedVersion), 1u);
	EXPECT_EQ(F.LogLinesWith(F.Cache.TakeLog(), EPrefixFallback::WrapperUnsupportedVersion), static_cast<size_t>(1));
}

HOST_TEST(cache_coalesced_load_ending_pool_exhausted_resolves_all, "3.5 (the shared load ends PoolExhausted)")
{
	FCacheFixture F(2, false, 0);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->ExternalPages = F.Sim->PoolPages - 2; // a build needs 3
	std::vector<int64_t> Handles;
	for (int I = 0; I < 16; ++I)
	{
		Handles.push_back(F.Request(FixtureSpec(1)));
	}
	EXPECT_LE(F.Sim->RunAll(), 64);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	for (int64_t H : Handles)
	{
		EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::PoolExhausted);
	}
	EXPECT_EQ(F.Diag().ChargedBytes, 0u);
	EXPECT_EQ(F.Fallbacks(EPrefixFallback::PoolExhausted), 1u);
}

HOST_TEST(cache_coalesced_first_requester_returns_before_ready, "3.5 (the first requester returns its lease before Ready)")
{
	FCacheFixture F(2, false);
	REQUIRE_TRUE(F.bConfigured);
	F.Sim->Held = {EPrefixCommandKind::Build};
	std::vector<int64_t> Handles;
	for (int I = 0; I < 16; ++I)
	{
		Handles.push_back(F.Request(FixtureSpec(1)));
	}
	F.Sim->RunAll();
	REQUIRE_EQ(F.Sim->CountQueued(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Cache.Return(Handles[0]), true);
	EXPECT_EQ(F.Cache.GetLeaseState(Handles[0]), EPrefixLeaseState::Ended);
	F.Sim->Held.clear();
	EXPECT_LE(F.Sim->RunAll(), 64);
	EXPECT_EQ(F.Cache.GetLeaseState(Handles[0]), EPrefixLeaseState::Ended);
	for (size_t I = 1; I < Handles.size(); ++I)
	{
		EXPECT_EQ(F.Cache.GetLeaseState(Handles[I]), EPrefixLeaseState::Ready);
	}
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Build), static_cast<size_t>(1));
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
}

HOST_TEST(cache_lease_returned_while_its_admission_waits, "3.5 (the entry still loads once no lease waits), §5.4 (no charge leaks)")
{
	FCacheFixture F(1, false);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_TRUE(F.Cache.Return(A));
	F.Sim->Held = {EPrefixCommandKind::Release};
	const int64_t B = F.Request(FixtureSpec(2));
	F.Sim->RunAll();
	REQUIRE_TRUE(F.Cache.Return(B));
	F.Sim->Held.clear();
	F.Sim->RunAll();
	EXPECT_EQ(F.Diag().ResidentEntries, 1u);
	EXPECT_LE(F.Diag().ChargedBytes, F.Config.BudgetBytes);
	const int64_t C = F.RequestReady(FixtureSpec(2));
	EXPECT_NE(C, 0);
	EXPECT_EQ(F.Cache.GetSource(C), EPrefixSource::Memory);
}

HOST_TEST(cache_pool_exhausted_restore_is_read_again, "§5.4 the pool is the hard limit (a restore's evict and retry)")
{
	FCacheFixture F(4, true, 0);
	REQUIRE_TRUE(F.bConfigured);
	const int64_t A = F.RequestReady(FixtureSpec(1));
	REQUIRE_NE(A, 0);
	F.Cache.Return(A);
	const FPrefixSpec S = FixtureSpec(2);
	F.PutDiskFile(S);
	F.Sim->ExternalPages = F.Sim->PoolPages - F.Sim->CacheDrawnPages - 1; // a restore needs 2
	const int64_t H = F.Request(S);
	F.Sim->RunAll();
	EXPECT_EQ(F.Cache.GetLeaseState(H), EPrefixLeaseState::Ready);
	EXPECT_EQ(F.Cache.GetSource(H), EPrefixSource::Disk);
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::Release), static_cast<size_t>(1));
	EXPECT_EQ(F.Sim->CountHistory(EPrefixCommandKind::ReadDisk), static_cast<size_t>(3)); // A's miss, then S twice
	EXPECT_LE(F.Diag().ChargedBytes, F.Config.BudgetBytes);
	EXPECT_EQ(F.Sim->Violations.empty(), true); // the consumed payload is never restored twice
}
