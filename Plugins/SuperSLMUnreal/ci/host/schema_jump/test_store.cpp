// The per-(model, schema) table store (plan §6 P1-X "The learned draft", its Concurrency and Lifetime
// paragraphs; cells P1X.8 and P1X.11, host halves): the snapshot a span job reads, the merge on
// completion and the swap by pointer, which generations feed it (P1X.12 (b), rev 1.3.5), the seed,
// and the drop at unmap.

#include "fixture.h"
#include "harness.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace SuperSLMCore;
using namespace SchemaJumpFixture;

namespace
{
	const FDraftTableKey KeyA{1, 0};
	const FDraftTableKey KeyB{1, 1}; // same model, another schema
	const FDraftTableKey KeyC{2, 0}; // another model, same schema index

	// A generation that ended with the walk accepting, unless bAccepting says otherwise.
	void Finish(FDraftTableStore& Store, const FDraftTableKey& Key, ESchemaJumpMode Mode, const ISchemaAutomaton& A,
		const std::vector<int32_t>& S, bool bAccepting = true)
	{
		Store.OnGenerationFinished(Key, Mode, bAccepting, A, S.data(), S.size());
	}
}

HOST_TEST(store_has_no_table_before_a_generation_finishes, "§6 P1-X; cold start")
{
	FDraftTableStore Store;
	EXPECT_TRUE(Store.Snapshot(KeyA) == nullptr);
	EXPECT_EQ(Store.TableCount(), 0u);

	// Positive clause: a finished Exact generation creates the table.
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	EXPECT_TRUE(Store.Snapshot(KeyA) != nullptr);
	EXPECT_EQ(Store.TableCount(), 1u);
}

HOST_TEST(store_merges_on_completion_into_a_new_table, "§6 P1-X Concurrency: merged at completion into a new table, swapped by pointer")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef First = Store.Snapshot(KeyA);
	REQUIRE_TRUE(First != nullptr);
	EXPECT_EQ(First->CountOf(S0, 100), 1u);

	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef Second = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Second != nullptr);
	EXPECT_TRUE(Second.get() != First.get());
	EXPECT_EQ(Second->CountOf(S0, 100), 2u);
	// The snapshot a span job took before the merge is unchanged.
	EXPECT_EQ(First->CountOf(S0, 100), 1u);
}

HOST_TEST(store_snapshot_held_across_many_merges_never_changes, "P1X.8 host half: a span job reads the snapshot it started with")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef Held = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Held != nullptr);
	const std::vector<int32_t> DraftBefore = BuildDraft(Held.get(), A, S0, Wide());
	REQUIRE_TRUE(!DraftBefore.empty());

	for (int I = 0; I < 25; ++I)
	{
		Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamB());
	}
	// The store moved on: B now leads at S0.
	const FDraftTableRef Now = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Now != nullptr);
	EXPECT_EQ(Now->CountOf(S0, 101), 25u);
	const std::vector<int32_t> DraftNow = BuildDraft(Now.get(), A, S0, Wide());
	REQUIRE_TRUE(!DraftNow.empty());
	EXPECT_EQ(DraftNow.front(), 101);

	// The held one did not.
	EXPECT_EQ(Held->CountOf(S0, 101), 0u);
	EXPECT_EQ(Held->CountOf(S0, 100), 1u);
	EXPECT_TRUE(BuildDraft(Held.get(), A, S0, Wide()) == DraftBefore);
}

HOST_TEST(store_learns_nothing_from_a_canonical_generation, "P1X.12 (b); §6 P1-X: the table learns only from the model; mutant XD3 (canonical feeding)")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Canonical, A, OrderStreamA());
	EXPECT_TRUE(Store.Snapshot(KeyA) == nullptr);

	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef Exact = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Exact != nullptr);
	EXPECT_EQ(Exact->CountOf(S0, 100), 1u);

	Finish(Store, KeyA, ESchemaJumpMode::Canonical, A, OrderStreamB());
	const FDraftTableRef After = Store.Snapshot(KeyA);
	REQUIRE_TRUE(After != nullptr);
	EXPECT_EQ(After->CountOf(S0, 101), 0u);
	EXPECT_EQ(After->CountOf(S0, 100), 1u);
}

HOST_TEST(store_keys_tables_by_model_and_schema, "§6 P1-X: a per-(model, schema) table")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	REQUIRE_TRUE(Store.Snapshot(KeyA) != nullptr);
	EXPECT_TRUE(Store.Snapshot(KeyB) == nullptr);
	EXPECT_TRUE(Store.Snapshot(KeyC) == nullptr);

	Finish(Store, KeyB, ESchemaJumpMode::Exact, A, OrderStreamB());
	REQUIRE_TRUE(Store.Snapshot(KeyB) != nullptr);
	EXPECT_EQ(Store.Snapshot(KeyB)->CountOf(S0, 101), 1u);
	EXPECT_EQ(Store.Snapshot(KeyA)->CountOf(S0, 101), 0u);
	EXPECT_EQ(Store.TableCount(), 2u);
}

HOST_TEST(store_drops_a_models_tables_at_unmap, "§6 P1-X Lifetime; P1X.11 host half: a remap starts from no table")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	Finish(Store, KeyB, ESchemaJumpMode::Exact, A, OrderStreamA());
	Finish(Store, KeyC, ESchemaJumpMode::Exact, A, OrderStreamA());
	REQUIRE_EQ(Store.TableCount(), 3u);

	const FDraftTableRef HeldByASpanJob = Store.Snapshot(KeyA);
	REQUIRE_TRUE(HeldByASpanJob != nullptr);

	Store.DropModel(1);
	EXPECT_TRUE(Store.Snapshot(KeyA) == nullptr);
	EXPECT_TRUE(Store.Snapshot(KeyB) == nullptr);
	EXPECT_TRUE(Store.Snapshot(KeyC) != nullptr);
	EXPECT_EQ(Store.TableCount(), 1u);

	// A span job still holding the dropped snapshot reads it safely.
	EXPECT_EQ(HeldByASpanJob->CountOf(S0, 100), 1u);

	// The remapped model starts from no table: its first merge counts 1, not 2.
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	REQUIRE_TRUE(Store.Snapshot(KeyA) != nullptr);
	EXPECT_EQ(Store.Snapshot(KeyA)->CountOf(S0, 100), 1u);
}

HOST_TEST(store_seed_drafts_at_once_and_learning_adds_to_it, "§16 Q20 A: optionally seeded from a shipped table; P1X.1 seeded arm (host half)")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	Store.Seed(KeyA, MakeSeedTable({{S0, 101, 3}, {S5, 105, 3}, {S2, 103, 3}}));
	const FDraftTableRef Seeded = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Seeded != nullptr);
	EXPECT_TRUE(BuildDraft(Seeded.get(), A, S0, Wide()) == (std::vector<int32_t>{101, 105, 103}));

	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamB());
	const FDraftTableRef Learned = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Learned != nullptr);
	EXPECT_EQ(Learned->CountOf(S0, 101), 4u);
	EXPECT_EQ(Learned->CountOf(S4, 104), 1u);
}

HOST_TEST(store_stays_bounded_over_a_long_population, "§6 P1-X Lifetime: at most the forced states, each capped; P1X.11 host half")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = OrderAutomaton();
	REQUIRE_GT(FDraftTable::MaxTokensPerState(), 0u);
	// 2,000 orders, a seeded LCG choosing the split and the choice (no std distribution, so the
	// sequence is the same on every standard library).
	uint32_t X = 12345u;
	for (int I = 0; I < 2000; ++I)
	{
		X = X * 1664525u + 1013904223u;
		const bool bSplitB = (X >> 16) & 1u;
		const int32_t Choice = static_cast<int32_t>(200 + ((X >> 17) % 3u));
		std::vector<int32_t> S = bSplitB ? std::vector<int32_t>{101, 105, 103} : std::vector<int32_t>{100, 102, 103};
		S.push_back(Choice);
		S.push_back(Choice == 202 ? 203 : 104);
		Finish(Store, KeyA, ESchemaJumpMode::Exact, A, S);
	}
	const FDraftTableRef T = Store.Snapshot(KeyA);
	REQUIRE_TRUE(T != nullptr);
	EXPECT_LE(T->StateCount(), kOrderForcedStates);
	const uint32_t States[] = {S0, S1, S2, S3, S4, S5, S6, S7};
	for (uint32_t S : States)
	{
		EXPECT_LE(T->TokenCountAt(S), static_cast<size_t>(FDraftTable::MaxTokensPerState()));
	}
	EXPECT_EQ(T->TokenCountAt(S3), 0u);
	EXPECT_EQ(T->TokenCountAt(S6), 0u);
	EXPECT_EQ(Store.TableCount(), 1u);
}

HOST_TEST(store_snapshots_read_on_worker_threads_while_the_game_thread_merges, "P1X.8 host half (run under SCHEMA_JUMP_TSAN for the race check)")
{
	FDraftTableStore Store;
	const FFakeAutomaton A = ChainAutomaton(8);
	const std::vector<int32_t> Chain = ChainStream(8);
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, Chain);

	// Each "span job" takes its snapshot on the game thread when planned, then reads it on a worker.
	constexpr int kWorkers = 4;
	std::vector<FDraftTableRef> Snapshots;
	for (int W = 0; W < kWorkers; ++W)
	{
		Snapshots.push_back(Store.Snapshot(KeyA));
		REQUIRE_TRUE(Snapshots.back() != nullptr);
	}
	const std::vector<int32_t> Expected = BuildDraft(Snapshots[0].get(), A, 0, Wide());
	REQUIRE_EQ(Expected.size(), 8u);

	std::atomic<int> Mismatches{0};
	std::vector<std::thread> Workers;
	for (int W = 0; W < kWorkers; ++W)
	{
		const FDraftTableRef Mine = Snapshots[static_cast<size_t>(W)];
		Workers.emplace_back([&A, &Expected, &Mismatches, Mine]()
			{
				for (int I = 0; I < 2000; ++I)
				{
					if (BuildDraft(Mine.get(), A, 0, Wide()) != Expected)
					{
						Mismatches.fetch_add(1, std::memory_order_relaxed);
					}
				}
			});
	}
	// Meanwhile the game thread merges finished generations and swaps tables in.
	std::vector<int32_t> Other = Chain;
	Other[0] = ChainToken(0, 1);
	for (int I = 0; I < 300; ++I)
	{
		Finish(Store, KeyA, ESchemaJumpMode::Exact, A, Other);
	}
	for (std::thread& T : Workers)
	{
		T.join();
	}
	EXPECT_EQ(Mismatches.load(), 0);
	const FDraftTableRef Final = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Final != nullptr);
	EXPECT_EQ(Final->CountOf(0, ChainToken(0, 1)), 300u);
	EXPECT_EQ(Snapshots[0]->CountOf(0, ChainToken(0, 1)), 0u);
}

HOST_TEST(p1x12b_off_and_exact_generations_that_end_accepting_feed, "P1X.12 (b): Off and Exact feed (rev 1.3.5)")
{
	const FFakeAutomaton A = OrderAutomaton();
	FDraftTableStore Store;
	Finish(Store, KeyA, ESchemaJumpMode::Off, A, OrderStreamA());
	const FDraftTableRef AfterOff = Store.Snapshot(KeyA);
	REQUIRE_TRUE(AfterOff != nullptr);
	EXPECT_EQ(AfterOff->CountOf(S0, 100), 1u);
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef AfterExact = Store.Snapshot(KeyA);
	REQUIRE_TRUE(AfterExact != nullptr);
	EXPECT_EQ(AfterExact->CountOf(S0, 100), 2u);
}

HOST_TEST(p1x12b_a_generation_that_did_not_end_accepting_feeds_nothing, "P1X.12 (b): cut by MaxNewTokens, a fault, a cancel or a release; mutant XD3 (an unfinished stream feeding)")
{
	const FFakeAutomaton A = OrderAutomaton();
	const ESchemaJumpMode Modes[] = {ESchemaJumpMode::Off, ESchemaJumpMode::Exact, ESchemaJumpMode::Canonical};
	for (ESchemaJumpMode M : Modes)
	{
		FDraftTableStore Store;
		// A truncated stream, as MaxNewTokens or a release leaves it.
		Finish(Store, KeyA, M, A, {100, 102}, /*bAccepting*/ false);
		EXPECT_TRUE(Store.Snapshot(KeyA) == nullptr);
		EXPECT_EQ(Store.TableCount(), 0u);
	}
	// Onto an existing table: unchanged, and not swapped.
	FDraftTableStore Store;
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamA());
	const FDraftTableRef Before = Store.Snapshot(KeyA);
	REQUIRE_TRUE(Before != nullptr);
	Finish(Store, KeyA, ESchemaJumpMode::Exact, A, OrderStreamB(), /*bAccepting*/ false);
	Finish(Store, KeyA, ESchemaJumpMode::Off, A, OrderStreamB(), /*bAccepting*/ false);
	const FDraftTableRef After = Store.Snapshot(KeyA);
	REQUIRE_TRUE(After != nullptr);
	EXPECT_EQ(After->CountOf(S0, 101), 0u);
	EXPECT_EQ(After->CountOf(S0, 100), 1u);
}
