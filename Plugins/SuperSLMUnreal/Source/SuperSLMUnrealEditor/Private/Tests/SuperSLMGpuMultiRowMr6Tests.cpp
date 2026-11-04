// Slice MR6 (multi-row GPU plan rev 1.3.2, §5.2-§5.4; red-suite-mr6.md; rulings 37, 39, 40, 42, 42a, 43, 45, 48, 50, 51 and 53): the plugin's multi-row
// scheduling, against the engine MR6 vendors (SuperSLM v1.19.0).
//   P4.4  bGpuMultiRow = false is 1.17's scheduling: one-row actions only, the same action trace for Exact as for Off,
//         one prompt token per OneCall action; tokens equal the CPU's Off run.
//   7.12  An adapter swap requested while a chunk is open waits for the close; the output equals the twin (the swap
//         requested on the one-row path at the same prompt position).
//   8.5   An agent read by rows, then decoding, saving, restoring and continuing equals one read one row at a time, on
//         both paths: tokens, and the save blob at the same token count.
//   9.4   A save while a chunk is open abandons it and re-feeds: the final tokens and the final save blob equal a run
//         never saved.
//   R40.1-R40.6 (ruling 40): the checks of behaviour Exact shares with Off that P2's retired Canonical suite carried,
//         re-pointed at the multi-row Exact path: one result per action (multi-id delivery, listeners, a release
//         mid-chunk), the save carry, re-arm after a dead end, the restore's armed flag, the limits, mixed modes.
//   R42   (ruling 42, 42a; the MR6 review's FIX FIRST): M1 Composed ticks within B (and 1.17's uncapped slices with
//         multi-row off), M2 fault -> reset -> begin, R1 OneCall below one row sliced, S1 a failed Exact open runs
//         Off's step, S2 a no-draft step is one Off token (and a save inside it abandons cleanly), S5 GPU Exact drafts
//         as CPU Exact.
//   R43   (ruling 43; the re-check's N1 and N2, below one row): N1 an Exact fallback at B = 2 and 4 ms stays within B
//         with Off's tokens and device work; N2 a OneCall and a Composed sequence together stay within B, unfaulted.
//   R45   (ruling 45, 45a, 46a): the per-tick GPU-busy seam is the sum of the tick's calls (OneCall, Composed solo, a
//         batch call, an Exact close, a Finish), never the last call's; no submission goes untimed, a Composed batch
//         tick included.
//   R48   (ruling 48, erratum E-13): the load-time row-cost calibration: the game's GpuRowCostMs wins, the measured
//         price is used, logged and exposed, it stays within B at B = 2 ms and the first generation waits, once per
//         model and GPU adapter (a re-import hits the cache), failure, noise and a timeout fall back to 6.2 ms with a
//         warning, and the switch off is 6.2 ms (ruling 48a: GpuRowCostMs 0 is unset).
//   R50   (ruling 50): on the 1.5B at B = 2 ms, each calibration attempt's first slice is one layer and no calibration
//         tick carries more than B unless it ran a single layer (skipped when the 1.5B is absent).
//   E14   (ruling 53, erratum E-14): flat prices and l(1)'s margin; the two-position line, each slice priced at its end;
//         a long-context slice within B on the 1.5B. E14_7 (ruling 57, the ruling-56 review's F2-F4; ruling 58): on
//         the 1.5B with l(1) at 4 or 2, OneCall and Composed in turn, a slice that ends a token overruns only as its
//         tick's lone work. E14_8 (ruling 58.4): on A-EX with OneCall running whole tokens beside Composed, the whole
//         token is billed in the tick.
//   R51   (ruling 51, the E-13 review): F1 an Exact request queued during a calibration that turns Exact off gives the
//         CPU's tokens; F2 the 1.5B at B = 1 ms, paced at a game's frame rate, settles on the measured price within B;
//         F4 a default configuration calibrates without a warning; F7 a retry at B > 0 starts again at one layer.
//
// Every cell opens with Gate(): it fails by name while FSuperSLMGpuMultiRowTestAccess is the red stub (IsBuilt false,
// SetMultiRow cannot set the switch). Every multi-row arm then requires chunk activity (a PromptRows action), so a
// build that never opens a chunk cannot pass an "equals the one-row run" check. The oracle is always the one-row run
// (bGpuMultiRow = false) of the same request in the same session, or the CPU's Off run (C17).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperSLMSlotGates.h"

#if SUPERSLM_WITH_L2S1_ASYNC && SUPERSLMUNREAL_WITH_GPU

#include "Fixtures/SuperSLMGpuSchemaJumpFixtures.h"
#include "Fixtures/SuperSLMPagingGpuFixtures.h"
#include "Fixtures/SuperSLMTestDataPaths.h"
#include "SuperSLMGpuMultiRowTestSeam.h"
#include "SuperSLMBlueprintLibrary.h"
#include "Tests/AutomationCommon.h"
#include "superslm/gpu_rows_cost_table.h"

using namespace SuperSLMGpuSchemaJumpFixtures;
using SuperSLMSchemaJumpFixtures::FRunResult;

namespace
{
	using FMr = FSuperSLMGpuMultiRowTestAccess;
	using FAct = FSuperSLMGpuRowsActionRecord;
	using EKind = ESuperSLMGpuRowsActionKind;

	constexpr ESuperSLMSchemaJumpForward kOff = ESuperSLMSchemaJumpForward::Off;
	constexpr ESuperSLMSchemaJumpForward kExact = ESuperSLMSchemaJumpForward::Exact;
	constexpr int32 kMr6MaxNew = 24;

	// A prompt long enough for several chunks of 8 rows (§5.2: only k = 1 goes one-row).
	const FString& LongPrompt()
	{
		static const FString P = TEXT(
			"The caravan reached the potion shop at dusk. The shopkeeper counted the bottles on the top shelf twice, wrote "
			"the order in her ledger, and asked the traveller which of the three remedies he wanted, and how many of each.");
		return P;
	}

	bool Gate(FAutomationTestBase& T)
	{
		return T.TestTrue(TEXT("the MR6 multi-row test seam is built (FSuperSLMGpuMultiRowTestAccess is the red stub until MR6)"), FMr::IsBuilt());
	}

	// P2's configuration with the switch set. False (and an error) when the switch cannot be set.
	bool MrConfig(FAutomationTestBase& T, EPath Path, bool bMultiRow, FSuperSLMGpuRuntimeConfig& Out, int32 SequenceBudget = 0, int32 BlockCount = 4)
	{
		Out = JumpConfig(Path, SequenceBudget, BlockCount);
		FMr::SetCalibrateGpuRowCost(Out, false); // ruling 48: these cells price at 6.2 ms (R48_* turn it back on; GpuRowCostMs stays as JumpConfig left it)
		return T.TestTrue(*FString::Printf(TEXT("bGpuMultiRow = %s is set on the configuration (ruling 39 Q9)"), bMultiRow ? TEXT("true") : TEXT("false")),
			FMr::SetMultiRow(Out, bMultiRow));
	}

	struct FMrCell
	{
		FTestWorldWrapper W;
		FGpuRig Rig;
		bool Open(FAutomationTestBase& T)
		{
			if (!Gate(T)) { return false; }
			FSuperSLMGpuRuntimeConfig C;
			return MrConfig(T, EPath::WholeToken, true, C) && W.CreateTestWorld(EWorldType::Game) && SetUp(T, W.GetTestWorld(), C, Rig);
		}
		bool Reconfigure(FAutomationTestBase& T, EPath Path, bool bMultiRow, int32 SequenceBudget = 0, int32 BlockCount = 4)
		{
			FSuperSLMGpuRuntimeConfig C;
			return MrConfig(T, Path, bMultiRow, C, SequenceBudget, BlockCount) && ConfigureGpu(T, Rig, C);
		}
	};

	TArray<FAct> ActionsOf(const USuperSLMGpuSubsystem& Gpu, int64 SequenceId)
	{
		TArray<FAct> Out;
		for (const FAct& A : FMr::GetActions(Gpu))
		{
			if (A.SequenceId == SequenceId) { Out.Add(A); }
		}
		return Out;
	}

	int32 CountKind(const TArray<FAct>& Acts, EKind Kind)
	{
		int32 N = 0;
		for (const FAct& A : Acts) { N += A.Kind == Kind ? 1 : 0; }
		return N;
	}

	int32 PromptConsumed(const TArray<FAct>& Acts)
	{
		int32 N = 0;
		for (const FAct& A : Acts) { N += A.PromptTokensConsumed; }
		return N;
	}

	// The action trace as P4.4 compares it: kind, rows, layers and prompt tokens per action, in order, and each action's
	// tick relative to the first.
	FString TraceOf(const TArray<FAct>& Acts)
	{
		FString S;
		const int64 T0 = Acts.Num() > 0 ? Acts[0].Tick : 0;
		for (const FAct& A : Acts)
		{
			S += FString::Printf(TEXT("[%lld k%d r%d l%d+%d p%d]"), A.Tick - T0, (int32)A.Kind, A.Rows, A.FirstLayer, A.Layers, A.PromptTokensConsumed);
		}
		return S;
	}

	// Vend (bound), record, run to the end (or StopWhen), read. The sequence is kept when bKeep.
	bool RunMr(FAutomationTestBase& T, const FGpuRig& Rig, EPath Path, const FSuperSLMGenerationRequest& Req, FGpuRun& Out, TArray<FAct>& OutActs,
		bool bKeep = false, TFunction<bool()> StopWhen = nullptr)
	{
		Out = FGpuRun();
		if (!Vend(T, Rig, Path, Out)) { return false; }
		FMr::StartRecording(*Rig.Gpu);
		const bool bOk = Drive(T, Rig, Req, Out, StopWhen);
		OutActs = ActionsOf(*Rig.Gpu, Out.Sequence.Id);
		if (!bKeep) { ReturnAndSettle(Rig, Out.Sequence); }
		return bOk;
	}

	// Ruling 56 (5): the first offset where two byte arrays differ (INDEX_NONE: equal).
	int32 FirstDiff(const TArray<uint8>& A, const TArray<uint8>& B)
	{
		const int32 N = FMath::Min(A.Num(), B.Num());
		for (int32 I = 0; I < N; ++I) { if (A[I] != B[I]) { return I; } }
		return A.Num() == B.Num() ? INDEX_NONE : N;
	}

	// Ruling 56 (5): "the save equals" compares live state: the header, the metadata and the K/V of positions below the
	// sequence's length (GetLiveSaveBytes); K/V past the length is dead. A mismatch names the first differing offset.
	bool RequireSameLiveSave(FAutomationTestBase& T, const FGpuRig& Rig, const TArray<uint8>& A, const TArray<uint8>& B, const FString& What)
	{
		TArray<uint8> LiveA, LiveB;
		const bool bRead = A.Num() > 0 && B.Num() > 0 && FMr::GetLiveSaveBytes(*Rig.Gpu, A, LiveA) && FMr::GetLiveSaveBytes(*Rig.Gpu, B, LiveB);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: both saves read as live state (%d / %d bytes)"), *What, A.Num(), B.Num()), bRead)) { return false; }
		const int32 At = FirstDiff(LiveA, LiveB);
		// Ruling 59d point 4: the differing field by name (wrapper, GPU extension or Layer-1 header), not just an offset.
		FString Field, Values;
		int32 InFlightA = 0, InFlightB = 0;
		const bool bNamed = FMr::DescribeSaveDiff(*Rig.Gpu, A, B, Field, Values, InFlightA, InFlightB);
		return T.TestTrue(*FString::Printf(TEXT("%s: equal in live state (live %d / %d bytes, first difference at %d: %s%s; InFlightIssued %d / %d)"),
			*What, LiveA.Num(), LiveB.Num(), At, bNamed ? (Field.IsEmpty() ? TEXT("none") : *Field) : TEXT("(not decoded)"), Values.IsEmpty() ? TEXT("") : *(TEXT(" = ") + Values), InFlightA, InFlightB), At == INDEX_NONE);
	}

	bool RequireChunks(FAutomationTestBase& T, const TArray<FAct>& Acts, const FString& What)
	{
		return T.TestTrue(*FString::Printf(TEXT("%s: the multi-row arm read the prompt by rows (%d PromptRows actions)"), *What, CountKind(Acts, EKind::PromptRows)),
			CountKind(Acts, EKind::PromptRows) > 0);
	}

	bool RequireOneRowOnly(FAutomationTestBase& T, const TArray<FAct>& Acts, const FString& What)
	{
		bool bOk = T.TestTrue(*(What + TEXT(": actions ran")), Acts.Num() > 0);
		for (const FAct& A : Acts)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, action %d: one-row work only (kind %d, rows %d)"), *What, A.ActionIndex, (int32)A.Kind, A.Rows),
				A.Kind == EKind::OneRow && A.Rows == 1 && !A.bOpenedChunk);
		}
		return bOk;
	}

	// Pumps a kept sequence until it ends.
	bool Finish(const FGpuRig& Rig, FGpuRun& Run)
	{
		const bool bRan = SuperSLMPagingGpuFixtures::Pump(*Rig.Gpu, [&]() { return IsEnded(*Rig.Gpu, Run.Sequence); }, kRunSeconds);
		Read(Rig, Run);
		return bRan;
	}
}

// P4.4 (ruling 39 Q9; §5.4 *Switch*). bGpuMultiRow = false restores 1.17's scheduling exactly. On both paths, Off and
// Exact: every action is one-row work; on OneCall each prompt action takes one prompt token and there is one per prompt
// token; on Composed no action runs more than LayersPerTick layers; Exact's action trace equals Off's (with multi-row
// off, Exact has no rows to verify and runs as Off); tokens equal the CPU's Off run. The chooser reports the one-row
// plan. Then the same requests with the switch on read the prompt by rows and give the same tokens, so the switch is
// the thing the arm turned off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowMr6P44Test, "SuperSLM.MultiRow.MR6.P4_4.SwitchOffIsOneRowScheduling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowMr6P44Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	bool bOk = true;
	for (const EPath Path : { EPath::WholeToken, EPath::Sliced4 })
	{
		const FString P = PathName(Path);
		FSuperSLMGenerationRequest Req;
		FRunResult Cpu;
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Req, LongPrompt()) || !RunCpu(*this, C.Rig, Req, kOff, Cpu)) { return false; }
		const int32 PromptLen = Req.PromptTokens.Num();
		if (!C.Reconfigure(*this, Path, false)) { return false; }
		int32 MMax = -1, MMaxLayers = -1, CRows = -1, CLayers = -1;
		bool bOn = true;
		bOk &= TestTrue(*(P + TEXT(": the chooser's plan is readable")), FMr::GetRowsPlan(*C.Rig.Gpu, MMax, MMaxLayers, CRows, CLayers, bOn));
		bOk &= TestFalse(*(P + TEXT(": switch off: the plan is the one-row plan")), bOn);
		bOk &= TestEqual(*(P + TEXT(": switch off: M_max 1")), MMax, 1);
		bOk &= TestEqual(*(P + TEXT(": switch off: Composed takes one row")), CRows, 1);
		FString OffTrace;
		for (const ESuperSLMSchemaJumpForward Mode : { kOff, kExact })
		{
			const FString Arm = FString::Printf(TEXT("%s, switch off, %s"), *P, Mode == kOff ? TEXT("Off") : TEXT("Exact"));
			FSuperSLMGenerationRequest R = Req;
			R.SchemaJumpForward = Mode;
			FGpuRun Run;
			TArray<FAct> Acts;
			if (!RunMr(*this, C.Rig, Path, R, Run, Acts)) { return false; }
			bOk &= RequireOneRowOnly(*this, Acts, Arm);
			bOk &= TestEqual(*(Arm + TEXT(": tokens equal the CPU's Off run")), Run.Tokens, Cpu.Tokens);
			bOk &= TestEqual(*(Arm + TEXT(": every prompt token was fed")), PromptConsumed(Acts), PromptLen);
			int32 PromptActions = 0;
			for (const FAct& A : Acts)
			{
				PromptActions += A.PromptTokensConsumed > 0 ? 1 : 0;
				bOk &= TestTrue(*FString::Printf(TEXT("%s, action %d: at most one prompt token per action (1.17)"), *Arm, A.ActionIndex), A.PromptTokensConsumed <= 1);
				if (Path == EPath::Sliced4)
				{
					bOk &= TestTrue(*FString::Printf(TEXT("%s, action %d: at most LayersPerTick (4) layers"), *Arm, A.ActionIndex), A.Layers <= 4);
				}
			}
			if (Path == EPath::WholeToken)
			{
				bOk &= TestEqual(*(Arm + TEXT(": one whole prompt token per OneCall action (123 tokens take 123 ticks)")), PromptActions, PromptLen);
			}
			const FString Trace = TraceOf(Acts);
			if (Mode == kOff) { OffTrace = Trace; }
			else { bOk &= TestEqual(*(Arm + TEXT(": Exact's action trace equals Off's with the switch off")), Trace, OffTrace); }
		}
		// The switch on: the same request reads by rows and gives the same tokens.
		if (!C.Reconfigure(*this, Path, true)) { return false; }
		FGpuRun On;
		TArray<FAct> OnActs;
		if (!RunMr(*this, C.Rig, Path, Req, On, OnActs)) { return false; }
		bOk &= RequireChunks(*this, OnActs, P + TEXT(", switch on"));
		bOk &= TestEqual(*(P + TEXT(", switch on: tokens equal the CPU's Off run")), On.Tokens, Cpu.Tokens);
	}
	return bOk;
}

// 7.12 (SF7; §5.4 *Adapters*). On the 1.5B artifact (the shopkeeper adapter's base), pooled: a OneCall generation reads
// a 40-token prompt by rows; while a chunk is open the sequence's adapter swap is requested. Every slice of that chunk
// passes the adapter its first slice passed (rule 4), and the first action after the chunk's close passes the new one:
// the swap waited for the close. The twin, on the one-row path in the same session, requests the same swap when its
// prompt reaches the chunk's close position; the tokens and the active adapter at the end are equal. (An eviction
// waits the same way; the residency layer that evicts arrives with R-20, so this cell covers the swap.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowMr6AdapterWaitsTest, "SuperSLM.MultiRow.MR6.7_12.AdapterSwapWaitsForTheClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowMr6AdapterWaitsTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPagingGpuFixtures;
	if (!Gate(*this)) { return false; }
	FGpuPagingRig Rig;
	if (!Rig.Init(*this, EGpuArtifact::TwoCap15B)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FString AdapterPath = SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-shopkeeper-lora-v2-t2102-runtime.sslm"));
	TArray<int32> Prompt;
	if (!MakeTokens(*this, *Rig.Model, 40, 712, Prompt)) { return false; }
	FSuperSLMGenerationRequest Req;
	Req.PromptTokens = Prompt;
	Req.SpanKind = ESuperSLMSpanKind::Prompt;
	Req.MaxNewTokens = 8;

	// One arm: configure with the switch, map the adapter, run, and request the swap when SwapNow() holds.
	struct FArm
	{
		TArray<int32> Tokens;
		TArray<FAct> Acts;
		int32 ActiveAdapterAtEnd = -1;
		int32 AdapterId = 0;
		bool bSwapped = false;
	};
	auto RunArm = [&](bool bMultiRow, TFunctionRef<bool(const TArray<FAct>&, const FSuperSLMGpuSequence&)> SwapNow, FArm& Out) -> bool
	{
		FSuperSLMGpuRuntimeConfig C = MakeGpuConfig(EGpuArtifact::TwoCap15B, 1, 512, 1, 0);
		if (!TestTrue(TEXT("bGpuMultiRow is set"), FMr::SetMultiRow(C, bMultiRow)) || !ConfigurePooled(*this, Rig, C)) { return false; }
		FSuperSLMGpuAdapterHandle Adapter;
		FString MapError;
		if (!TestTrue(*FString::Printf(TEXT("MapAdapter (%s)"), *MapError), Gpu.MapAdapter(AdapterPath, *Rig.Model, Adapter, MapError))) { return false; }
		Out.AdapterId = static_cast<int32>(Adapter.Id);
		FSuperSLMGpuSequence Seq;
		if (!Vend(*this, Gpu, Seq)) { return false; }
		FMr::StartRecording(Gpu);
		const FSuperSLMLifecycleOpHandle H = Gpu.RequestBeginGeneration(Seq, Req);
		if (!TestTrue(TEXT("RequestBeginGeneration"), H.IsValid())) { return false; }
		const bool bRan = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&]()
		{
			return IsEnded(Gpu, Seq);
		}, kRunSeconds, [&]()
		{
			if (!Out.bSwapped && SwapNow(ActionsOf(Gpu, Seq.Id), Seq))
			{
				Gpu.RequestAdapterSwap(Seq, Adapter);
				Out.bSwapped = true;
			}
		});
		Out.Tokens = Gpu.GetGeneratedTokens(Seq);
		Out.Acts = ActionsOf(Gpu, Seq.Id);
		Out.ActiveAdapterAtEnd = static_cast<int32>(Gpu.GetActiveAdapter(Seq).Id);
		ReturnAndDrain(Gpu, Seq);
		Gpu.UnmapAdapter(Adapter);
		return TestTrue(TEXT("the arm's generation ends"), bRan) && TestTrue(TEXT("the arm requested the swap"), Out.bSwapped);
	};

	// The multi-row arm: the swap is requested on the first tick a chunk is open after its first slice.
	FArm Rows;
	if (!RunArm(true, [&](const TArray<FAct>& Acts, const FSuperSLMGpuSequence& Seq)
		{
			return Acts.Num() > 0 && Acts.Last().Kind == EKind::PromptRows && FMr::IsChunkOpen(Gpu, Seq);
		}, Rows))
	{
		return false;
	}
	bool bOk = RequireChunks(*this, Rows.Acts, TEXT("7.12, the multi-row arm"));
	// The record marks no request, so the swap's landing is read from the adapter: the first action passing the new
	// adapter must not be inside a chunk (it opens one, or is one-row work), and the action before it closed a chunk.
	int32 FirstNew = INDEX_NONE;
	for (int32 I = 0; I < Rows.Acts.Num(); ++I)
	{
		if (Rows.Acts[I].AdapterId == Rows.AdapterId) { FirstNew = I; break; }
	}
	bOk &= TestTrue(TEXT("7.12: the swap reached the multi-row generation"), FirstNew != INDEX_NONE);
	int32 CloseAt = 0;
	if (FirstNew != INDEX_NONE)
	{
		const FAct& A = Rows.Acts[FirstNew];
		bOk &= TestTrue(*FString::Printf(TEXT("7.12: the first action passing the new adapter (%d) opens a chunk or is one-row work: the swap never lands mid-chunk"), A.ActionIndex),
			A.Kind == EKind::OneRow || A.bOpenedChunk);
		bOk &= TestTrue(TEXT("7.12: the action before it closed a chunk (the swap waited for that close)"), FirstNew > 0 && Rows.Acts[FirstNew - 1].bClosedChunk);
		for (int32 I = 0; I < FirstNew; ++I) { CloseAt += Rows.Acts[I].PromptTokensConsumed; }
		// Every slice of every chunk passes one adapter, the one its opening slice passed (rule 4).
		int32 ChunkAdapter = -1;
		for (const FAct& X : Rows.Acts)
		{
			if (X.Kind == EKind::OneRow) { ChunkAdapter = -1; continue; }
			if (X.bOpenedChunk) { ChunkAdapter = X.AdapterId; }
			bOk &= TestEqual(*FString::Printf(TEXT("7.12, action %d: a chunk's slices pass its first slice's adapter"), X.ActionIndex), X.AdapterId, ChunkAdapter);
			if (X.bClosedChunk || X.bAbandonedChunk) { ChunkAdapter = -1; }
		}
	}
	// The twin: one-row, the swap requested when its prompt reaches the chunk's close position.
	FArm Twin;
	if (!RunArm(false, [&](const TArray<FAct>& Acts, const FSuperSLMGpuSequence&) { return PromptConsumed(Acts) >= CloseAt; }, Twin)) { return false; }
	bOk &= RequireOneRowOnly(*this, Twin.Acts, TEXT("7.12, the twin"));
	bOk &= TestEqual(TEXT("7.12: tokens equal the twin's"), Rows.Tokens, Twin.Tokens);
	bOk &= TestEqual(TEXT("7.12: the active adapter at the end is the swapped one in both"), Rows.ActiveAdapterAtEnd, Rows.AdapterId);
	bOk &= TestEqual(TEXT("7.12: the twin's active adapter at the end is the swapped one"), Twin.ActiveAdapterAtEnd, Twin.AdapterId);
	return bOk;
}

// 8.5 (§5.2 L2: the last prompt token may be inside a chunk). On both paths, dedicated: an agent reads its prompt by
// rows, decodes 3 tokens, is saved, returned, restored and continued to its end. The one-row run of the same request
// (the switch off) is saved at the same token count and continued the same way. The two save blobs are byte-equal,
// and each run's tokens (saved + continued) equal the unsaved one-row run's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowMr6ReadByRowsTest, "SuperSLM.MultiRow.MR6.8_5.ReadByRowsSaveRestoreContinue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowMr6ReadByRowsTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	bool bOk = true;
	constexpr int32 kSaveAt = 3;
	for (const EPath Path : { EPath::WholeToken, EPath::Sliced4 })
	{
		const FString P = PathName(Path);
		FSuperSLMGenerationRequest Req;
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Req, LongPrompt())) { return false; }
		// The unsaved one-row run.
		if (!C.Reconfigure(*this, Path, false)) { return false; }
		FGpuRun Unsaved;
		TArray<FAct> UnsavedActs;
		if (!RunMr(*this, C.Rig, Path, Req, Unsaved, UnsavedActs)) { return false; }
		TArray<uint8> Blobs[2];
		for (int32 Multi = 1; Multi >= 0; --Multi)
		{
			const FString Arm = FString::Printf(TEXT("%s, %s"), *P, Multi ? TEXT("read by rows") : TEXT("one row at a time"));
			if (!C.Reconfigure(*this, Path, Multi != 0)) { return false; }
			FGpuRun Before;
			TArray<FAct> Acts;
			if (!RunMr(*this, C.Rig, Path, Req, Before, Acts, true,
				[&]() { return C.Rig.Gpu->GetGeneratedTokens(Before.Sequence).Num() >= kSaveAt && !FMr::IsChunkOpen(*C.Rig.Gpu, Before.Sequence); }))
			{
				return false;
			}
			if (Multi) { bOk &= RequireChunks(*this, Acts, Arm); }
			else { bOk &= RequireOneRowOnly(*this, Acts, Arm); }
			bOk &= TestEqual(*(Arm + TEXT(": the run stopped at the save point")), Before.Tokens.Num(), kSaveAt);
			if (!Save(*this, C.Rig, Before.Sequence, Blobs[Multi])) { ReturnAndSettle(C.Rig, Before.Sequence); return false; }
			const TArray<int32> TokensBefore = C.Rig.Gpu->GetGeneratedTokens(Before.Sequence);
			ReturnAndSettle(C.Rig, Before.Sequence);
			FGpuRun After;
			if (!TestEqual(*(Arm + TEXT(": the restore succeeds")), (uint8)Restore(C.Rig, Blobs[Multi], After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
			bOk &= TestTrue(*(Arm + TEXT(": the restored run ends")), Finish(C.Rig, After));
			TArray<int32> Joined = TokensBefore;
			Joined.Append(After.Tokens);
			bOk &= TestEqual(*(Arm + TEXT(": saved + continued tokens equal the unsaved one-row run's")), Joined, Unsaved.Tokens);
			ReturnAndSettle(C.Rig, After.Sequence);
		}
		bOk &= RequireSameLiveSave(*this, C.Rig, Blobs[1], Blobs[0], P + TEXT(": the save blob after reading by rows equals one row at a time's"));
	}
	return bOk;
}

// 9.4 (§5.4 *Saves*; P2 makes the abandon invisible). On Composed (4 layers per tick, so a chunk stays open across
// ticks), dedicated and pooled: a save requested while a prompt chunk is open abandons it (the record marks the
// abandon) and the rows are re-fed. The saved blob restores, and the saved generation's final tokens and its final
// save blob equal a run of the same request never saved. The blob saved mid-chunk equals the blob of the one-row run
// saved at the same prompt position (the chunk's rows are not in it). Ruling 53 (triage run 3): the generation goes on
// in the tick the save resolves, so "no chunk open after the save" is read from the record: the abandoned chunk is
// never continued (the next prompt-row slice opens a new chunk); and the one-row run stops at the save's own prompt
// position, the prompt tokens committed before the abandoned chunk opened (its rows are not committed).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowMr6AbandonRefeedTest, "SuperSLM.MultiRow.MR6.9_4.SaveAbandonsAndRefeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowMr6AbandonRefeedTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	bool bOk = true;
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Req, LongPrompt())) { return false; }
	for (const int32 Budget : { 0, 512 })
	{
		const FString Slot = Budget > 0 ? TEXT("pooled") : TEXT("dedicated");
		if (!C.Reconfigure(*this, EPath::Sliced4, true, Budget)) { return false; }
		// The never-saved run, kept for its final blob.
		FGpuRun Never;
		TArray<FAct> NeverActs;
		if (!RunMr(*this, C.Rig, EPath::Sliced4, Req, Never, NeverActs, true)) { return false; }
		bOk &= RequireChunks(*this, NeverActs, Slot + TEXT(", never saved"));
		TArray<uint8> NeverFinal;
		bOk &= Save(*this, C.Rig, Never.Sequence, NeverFinal);
		ReturnAndSettle(C.Rig, Never.Sequence);
		// The saved run: stop while a chunk is open (after its first slice), save, return, restore, continue.
		FGpuRun Before;
		TArray<FAct> Acts;
		// Ruling 59d point 2: stopped in a chunk after the first, so the save's prompt position is past 0 and a one-row twin can
		// stop exactly there (at position 0 the twin's first tick has always issued a prompt token before its save is the front).
		if (!RunMr(*this, C.Rig, EPath::Sliced4, Req, Before, Acts, true, [&]() { return FMr::IsChunkOpen(*C.Rig.Gpu, Before.Sequence) && PromptConsumed(ActionsOf(*C.Rig.Gpu, Before.Sequence.Id)) > 0; }))
		{
			return false;
		}
		if (!TestTrue(*(Slot + TEXT(": the run stopped with a chunk open")), FMr::IsChunkOpen(*C.Rig.Gpu, Before.Sequence) && !IsEnded(*C.Rig.Gpu, Before.Sequence)))
		{
			ReturnAndSettle(C.Rig, Before.Sequence);
			return false;
		}
		TArray<uint8> Mid;
		if (!Save(*this, C.Rig, Before.Sequence, Mid)) { ReturnAndSettle(C.Rig, Before.Sequence); return false; }
		const TArray<FAct> SavedActs = ActionsOf(*C.Rig.Gpu, Before.Sequence.Id);
		int32 Abandons = 0, AbandonAt = INDEX_NONE, OpenAt = INDEX_NONE;
		for (int32 I = 0; I < SavedActs.Num(); ++I)
		{
			const FAct& A = SavedActs[I];
			if (A.Kind != EKind::OneRow && A.bOpenedChunk && AbandonAt == INDEX_NONE) { OpenAt = I; }
			if (A.bAbandonedChunk) { ++Abandons; if (AbandonAt == INDEX_NONE) { AbandonAt = I; } }
		}
		bOk &= TestEqual(*(Slot + TEXT(": the save abandoned the open chunk (once)")), Abandons, 1);
		if (!TestTrue(*(Slot + TEXT(": the abandoned chunk's opening slice is in the record")), AbandonAt != INDEX_NONE && OpenAt != INDEX_NONE && OpenAt <= AbandonAt))
		{
			ReturnAndSettle(C.Rig, Before.Sequence);
			return false;
		}
		// Ruling 53: the abandoned chunk is never continued; the next slice with device work opens a chunk or is one-row work.
		for (int32 I = AbandonAt + 1; I < SavedActs.Num(); ++I)
		{
			const FAct& A = SavedActs[I];
			if (A.Layers <= 0) { continue; }
			bOk &= TestTrue(*FString::Printf(TEXT("%s, action %d: after the abandon, the first slice with device work opens a new chunk or is one-row work (no slice continues the abandoned chunk)"), *Slot, A.ActionIndex),
				A.Kind == EKind::OneRow || A.bOpenedChunk);
			break;
		}
		// The save's own prompt position: what was committed before the abandoned chunk opened.
		int32 FedBefore = 0;
		for (int32 I = 0; I < OpenAt; ++I) { FedBefore += SavedActs[I].PromptTokensConsumed; }
		const TArray<int32> TokensBefore = C.Rig.Gpu->GetGeneratedTokens(Before.Sequence);
		ReturnAndSettle(C.Rig, Before.Sequence);
		FGpuRun After;
		if (!TestEqual(*(Slot + TEXT(": the mid-chunk blob restores")), (uint8)Restore(C.Rig, Mid, After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
		bOk &= TestTrue(*(Slot + TEXT(": the restored run ends")), Finish(C.Rig, After));
		TArray<int32> Joined = TokensBefore;
		Joined.Append(After.Tokens);
		bOk &= TestEqual(*(Slot + TEXT(": final tokens equal the never-saved run's")), Joined, Never.Tokens);
		TArray<uint8> AfterFinal;
		bOk &= Save(*this, C.Rig, After.Sequence, AfterFinal);
		bOk &= RequireSameLiveSave(*this, C.Rig, AfterFinal, NeverFinal, Slot + TEXT(": the final blob equals the never-saved run's"));
		ReturnAndSettle(C.Rig, After.Sequence);
		// The mid-chunk blob holds only what the abandon kept: the one-row run saved at the same prompt position.
		if (!C.Reconfigure(*this, EPath::Sliced4, false, Budget)) { return false; }
		FGpuRun OneRow;
		TArray<FAct> OneActs;
		// Ruling 59d point 2: the twin stops exactly at the save's prompt position, on a token boundary (its last slice ended a
		// token, so no token is in flight when the save becomes its front).
		const int32 TwinL = static_cast<int32>(SuperSLML2S2Fixtures::AExNumHiddenLayers);
		if (!RunMr(*this, C.Rig, EPath::Sliced4, Req, OneRow, OneActs, true,
			[&]()
			{
				const TArray<FAct> TwinActs = ActionsOf(*C.Rig.Gpu, OneRow.Sequence.Id);
				return TwinActs.Num() > 0 && PromptConsumed(TwinActs) == FedBefore && TwinActs.Last().FirstLayer + TwinActs.Last().Layers >= TwinL &&
					!FMr::IsChunkOpen(*C.Rig.Gpu, OneRow.Sequence);
			}))
		{
			return false;
		}
		TArray<uint8> OneMid;
		bOk &= Save(*this, C.Rig, OneRow.Sequence, OneMid);
		int64 TwinContext = -1;
		const bool bTwinRead = FMr::GetSaveContextUsed(OneMid, TwinContext);
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: the one-row twin was saved at prompt position %d (its ContextUsed %lld; ruling 59d point 2)"), *Slot, FedBefore, TwinContext),
			bTwinRead && TwinContext == FedBefore))
		{
			bOk = false;
			ReturnAndSettle(C.Rig, OneRow.Sequence);
			continue;
		}
		bOk &= RequireSameLiveSave(*this, C.Rig, Mid, OneMid, FString::Printf(TEXT("%s: the mid-chunk blob equals the one-row run's at prompt position %d"), *Slot, FedBefore));
		ReturnAndSettle(C.Rig, OneRow.Sequence);
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ruling 40 (red-suite-mr6.md): plugin 1.4 deletes P2's Canonical GPU core. Its host cells (test_gpu_rest, _planner,
// _sliced, _status_map, gpu_fake_engine.h) and its Unreal suite (SuperSLMSchemaJumpP2Tests.cpp) are retired; every
// check of behaviour Exact or Off shares (multi-id delivery, the save carry, re-arm after a dead end, the restore's
// armed flag, the limits, mixed modes, a release mid-chunk) is re-pointed here, at the multi-row Exact path. The
// oracle is the one-row twin (bGpuMultiRow = false, where Exact runs Off's one-row loop: P4.4) in the same session, or
// the CPU's Off run (C17). Each cell gates on the MR6 seam and requires ExactRows actions (RequireExactRows) where the
// behaviour is Exact's, so a build that never opens an Exact chunk cannot pass by running Off.
//
// Drafts: where the GPU's Exact drafts come from is red-suite-mr6.md Q13. These cells seed the CPU subsystem's
// learned table from the Off run of the same request; if the GPU draws its drafts elsewhere the seeding is harmless,
// and RequireExactRows still guards each cell.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
	// Ruling 42a: a chunk that falls back is still recorded as ExactRows, so "Exact ran" is an ExactRows slice that did
	// device work over two rows or more and did not fall back.
	bool RequireExactRows(FAutomationTestBase& T, const TArray<FAct>& Acts, const FString& What)
	{
		int32 Verified = 0;
		for (const FAct& A : Acts) { Verified += A.Kind == EKind::ExactRows && A.Rows >= 2 && A.Layers > 0 && !A.bFellBack ? 1 : 0; }
		return T.TestTrue(*FString::Printf(TEXT("%s: Exact verified drafts by rows (%d ExactRows slices over two rows or more)"), *What, Verified), Verified > 0);
	}

	TArray<int32> DeliveredOf(const TArray<FAct>& Acts)
	{
		TArray<int32> Out;
		for (const FAct& A : Acts) { Out.Append(A.DeliveredTokens); }
		return Out;
	}

	// The ids delivered through the first action that delivered two or more (an Exact close with accepted drafts), or
	// -1 when no action did.
	int32 FirstMultiIdCount(const TArray<FAct>& Acts)
	{
		int32 Cumulative = 0;
		for (const FAct& A : Acts)
		{
			Cumulative += A.DeliveredTokens.Num();
			if (A.DeliveredTokens.Num() >= 2) { return Cumulative; }
		}
		return -1;
	}

	bool SeedExactDrafts(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGenerationRequest& Req)
	{
		FRunResult Off;
		if (!SuperSLMSchemaJumpFixtures::LoadWalkTableForSeeding(T, Rig.Cpu) || !RunCpu(T, Rig, Req, kOff, Off)) { return false; }
		return T.TestTrue(TEXT("the draft table is seeded from the Off run"),
			FSuperSLMSchemaJumpTestAccess::SeedDraftTableFromStreams(*Rig.Cpu.Cpu, Rig.Cpu.Schema, { Off.Tokens }));
	}

	// Ruling 53 (triage run 3, findings 5 and 6a) made a cell that saves, or begins again, after its run use a MaxNew that
	// ends inside its schema. Plugin 1.5.0 ends a finished schema cleanly (Complete / SchemaComplete), but these cells
	// grade a run that stops on MaxNewTokens on a produced token, so the trim stays: the request is made at kMaxNew, and
	// when the CPU's Off run of it reaches the schema's end after N tokens, MaxNew becomes N - 1. The CPU's Off run of the
	// result must end Complete on MaxNewTokens. Exact's tokens are Off's (C17), so one probe serves both.
	bool MakeInSchemaRequest(FAutomationTestBase& T, const FGpuRig& Rig, ESuperSLMSchemaJumpForward Mode, FSuperSLMGenerationRequest& Out,
		const FString& Prompt = SuperSLML2S2Fixtures::PinnedSelfCheckPrompt(), int32 MaxNew = kMaxNew)
	{
		FRunResult Cpu;
		if (!MakeRequest(T, Rig, Mode, MaxNew, Out, Prompt) || !RunCpu(T, Rig, Out, kOff, Cpu)) { return false; }
		if (Cpu.Outcome == ESuperSLMDecodeOutcome::SchemaComplete)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("precondition: the request reaches its schema's end after two tokens or more (%d; phase %d)"), Cpu.Tokens.Num(), (int32)Cpu.Phase),
				Cpu.Phase == ESuperSLMSequencePhase::Complete && Cpu.Tokens.Num() >= 2)) { return false; }
			Out.MaxNewTokens = Cpu.Tokens.Num() - 1;
			if (!RunCpu(T, Rig, Out, kOff, Cpu)) { return false; }
		}
		return T.TestEqual(*FString::Printf(TEXT("precondition: MaxNewTokens %d ends inside the schema (the CPU's Off run completes; ruling 53)"), Out.MaxNewTokens),
			(uint8)Cpu.Phase, (uint8)ESuperSLMSequencePhase::Complete) &&
			T.TestEqual(TEXT("precondition: ... on MaxNewTokens, a produced token"), (uint8)Cpu.Outcome, (uint8)ESuperSLMDecodeOutcome::TokenProduced);
	}

	// The v1 wrapper's carried-event count: the u32 at +20 from its magic (magic, version, path, primed, stopped, 0,
	// LastToken, in-flight issued, count). -1 without a wrapper.
	int32 CarriedEvents(const TArray<uint8>& Blob)
	{
		const int32 At = FindWrapperExt(Blob);
		if (At == INDEX_NONE || At + 24 > Blob.Num()) { return -1; }
		return static_cast<int32>(Blob[At + 20] | (Blob[At + 21] << 8) | (Blob[At + 22] << 16) | (Blob[At + 23] << 24));
	}

	// Ruling 56 (6): the carry is taken when the save finalizes, not when it is requested. Saves Seq as Save() does, but
	// ticks without pacing and reads PendingResultIds() after every tick; OutPendingAtFinalize is the value read right
	// after the tick that resolved the save (the save's finalize ran in it).
	bool SaveWatchingPending(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuSequence& Seq, TArray<uint8>& OutBlob, int32& OutPendingAtFinalize)
	{
		const FSuperSLMLifecycleOpHandle Handle = Rig.Gpu->RequestSaveSequence(Seq);
		if (!T.TestTrue(*FString::Printf(TEXT("RequestSaveSequence (%s)"), *Rig.Gpu->GetLastLifecycleRequestError()), Handle.IsValid())) { return false; }
		OutPendingAtFinalize = FMr::PendingResultIds(*Rig.Gpu, Seq);
		ESuperSLMRestoreResult Result = Rig.Gpu->GetSaveResult(Handle, OutBlob);
		const double Start = FPlatformTime::Seconds();
		while (Result == ESuperSLMRestoreResult::Pending && FPlatformTime::Seconds() - Start < 120.0)
		{
			if (SuperSLML2S2Fixtures::NextTickGatedOnDevice(*Rig.Gpu))
			{
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Rig.Gpu->Tick(1.0f / 60.0f);
			OutPendingAtFinalize = FMr::PendingResultIds(*Rig.Gpu, Seq);
			Result = Rig.Gpu->GetSaveResult(Handle, OutBlob);
		}
		return T.TestEqual(TEXT("the save succeeds"), (uint8)Result, (uint8)ESuperSLMRestoreResult::Success) && T.TestTrue(TEXT("the blob has bytes"), OutBlob.Num() > 0);
	}

	// Begins Request on a kept sequence and runs it to its end (Drive), recording its actions afresh. Seq is a copy: a
	// caller may pass Out.Sequence.
	bool RunOn(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuSequence Seq, const FSuperSLMGenerationRequest& Req, FGpuRun& Out, TArray<FAct>& OutActs)
	{
		Out = FGpuRun();
		Out.Sequence = Seq;
		FMr::StartRecording(*Rig.Gpu);
		const bool bOk = Drive(T, Rig, Req, Out);
		OutActs = ActionsOf(*Rig.Gpu, Seq.Id);
		return bOk;
	}
}

// R40.1 Delivery (re-points P2H.4's gpu_delivery_one_result_per_action, XG33; P2.5's and P2.6's Exact legs; P2.13;
// P2.14). On both paths, Exact with the switch on: tokens equal the CPU's Off run and ForcedTokenCount is 0; each
// action's result holds its own ids, in order (the concatenation of the records' DeliveredTokens is the output, not
// spread and not shifted), at most Rows of them, and only a closing slice or one-row work delivers; on OneCall an Exact
// close delivers two or more ids in one result; every tick's output is a boundary of those results (a result's ids
// are applied together) and extends the last (listeners see each id once, in order); the final save is version 1 and
// equals the one-row twin's. Then a sequence returned while an Exact chunk is open leaves the pool clean: an Off run
// after it equals a fresh Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40DeliveryTest, "SuperSLM.MultiRow.MR6.R40_1.ExactOneResultPerAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40DeliveryTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	// Ruling 53: the request ends inside its schema, so the final saves below are of a Complete sequence.
	if (!MakeInSchemaRequest(*this, C.Rig, kExact, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	bool bOk = true;
	for (const EPath Path : { EPath::WholeToken, EPath::Sliced4 })
	{
		const FString P = PathName(Path);
		FGpuRun Twin;
		TArray<FAct> TwinActs;
		TArray<uint8> TwinBlob;
		if (!C.Reconfigure(*this, Path, false) || !RunMr(*this, C.Rig, Path, Req, Twin, TwinActs, true)) { return false; }
		bOk &= Save(*this, C.Rig, Twin.Sequence, TwinBlob);
		ReturnAndSettle(C.Rig, Twin.Sequence);

		FGpuRun Run;
		TArray<FAct> Acts;
		if (!C.Reconfigure(*this, Path, true) || !RunMr(*this, C.Rig, Path, Req, Run, Acts, true)) { return false; }
		if (Path == EPath::WholeToken) { bOk &= RequireExactRows(*this, Acts, P); }
		bOk &= TestEqual(*(P + TEXT(": tokens equal the CPU's Off run")), Run.Tokens, Cpu.Tokens);
		bOk &= TestEqual(*(P + TEXT(": ForcedTokenCount stays at Off's (0)")), Run.ForcedTokenCount, (int64)0);
		bOk &= TestEqual(*(P + TEXT(": the results' ids, action by action, are the output (one result per action, not spread, not shifted; XG33)")),
			DeliveredOf(Acts), Run.Tokens);
		TSet<int32> Boundaries;
		Boundaries.Add(0);
		int32 Cumulative = 0;
		bool bMultiId = false;
		for (const FAct& A : Acts)
		{
			const int32 N = A.DeliveredTokens.Num();
			Cumulative += N;
			Boundaries.Add(Cumulative);
			bOk &= TestTrue(*FString::Printf(TEXT("%s, action %d: at most its rows' ids (%d of %d)"), *P, A.ActionIndex, N, A.Rows), N <= FMath::Max(1, A.Rows));
			if (A.Kind != EKind::OneRow && !A.bClosedChunk)
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s, action %d: a slice that does not close its chunk delivers nothing"), *P, A.ActionIndex), N, 0);
			}
			if (N >= 2)
			{
				bMultiId = true;
				bOk &= TestTrue(*FString::Printf(TEXT("%s, action %d: a multi-id result is an Exact close"), *P, A.ActionIndex), A.Kind == EKind::ExactRows && A.bClosedChunk);
			}
		}
		if (Path == EPath::WholeToken)
		{
			bOk &= TestTrue(*(P + TEXT(": an Exact close delivered two or more ids in one result")), bMultiId);
		}
		TArray<int32> Last;
		for (const TArray<int32>& S : Run.Snapshots)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s: a tick's output (%d ids) ends on a result's boundary"), *P, S.Num()), Boundaries.Contains(S.Num()));
			bOk &= TestTrue(*(P + TEXT(": each tick's output extends the last's (listeners see each id once, in order)")), IsPrefixOf(Last, S) && IsPrefixOf(S, Run.Tokens));
			Last = S;
		}
		TArray<uint8> Blob;
		bOk &= Save(*this, C.Rig, Run.Sequence, Blob);
		bOk &= TestEqual(*(P + TEXT(": the save is wrapper version 1")), WrapperVersion(Blob), 1);
		{
			// Ruling 59d point 1: a completed Exact run has issued exactly the tokens it generated, as the one-row twin has.
			FString Field, Values;
			int32 InFlight = -1, TwinInFlight = -1;
			bOk &= TestTrue(*(P + TEXT(": both saves decode")), FMr::DescribeSaveDiff(*C.Rig.Gpu, Blob, TwinBlob, Field, Values, InFlight, TwinInFlight));
			bOk &= TestEqual(*(P + TEXT(": the completed Exact run's InFlightIssued is 0 (issued == generated)")), InFlight, 0);
			bOk &= TestEqual(*(P + TEXT(": the one-row twin's InFlightIssued is 0")), TwinInFlight, 0);
		}
		bOk &= RequireSameLiveSave(*this, C.Rig, Blob, TwinBlob, P + TEXT(": the final save equals the one-row twin's"));
		ReturnAndSettle(C.Rig, Run.Sequence);
	}

	// P2.14's shared half: a release with an Exact chunk open.
	if (!C.Reconfigure(*this, EPath::WholeToken, true)) { return false; }
	FSuperSLMGenerationRequest OffReq = Req;
	OffReq.SchemaJumpForward = kOff;
	FGpuRun Fresh, Mid, After;
	TArray<FAct> Ignored;
	if (!RunMr(*this, C.Rig, EPath::WholeToken, OffReq, Fresh, Ignored)) { return false; }
	if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Mid, Ignored, true, [&]()
		{
			const TArray<FAct> A = ActionsOf(*C.Rig.Gpu, Mid.Sequence.Id);
			return A.Num() > 0 && A.Last().Kind == EKind::ExactRows && FMr::IsChunkOpen(*C.Rig.Gpu, Mid.Sequence);
		}))
	{
		return false;
	}
	bOk &= TestTrue(TEXT("release: the run stopped with an Exact chunk open"), FMr::IsChunkOpen(*C.Rig.Gpu, Mid.Sequence) && !IsEnded(*C.Rig.Gpu, Mid.Sequence));
	ReturnAndSettle(C.Rig, Mid.Sequence);
	if (!RunMr(*this, C.Rig, EPath::WholeToken, OffReq, After, Ignored)) { return false; }
	bOk &= SameGpuEnding(*this, After, Fresh, TEXT("release: an Off run after the release equals a fresh one"));
	return bOk;
}

// R40.2 The save carry (re-points P2H.4's gpu_restore_issued_count, XG40, and the Unreal half of
// gpu_carry_pending_advance, XG27; P2.8's Exact leg). OneCall, dedicated and pooled, MaxNewTokens at K and K + 1, where
// K is the ids through the first multi-id Exact result. A save while that result is issued and not applied carries it
// (the v1 wrapper holds one event per id, at least two); the restored run's issued count counts the carried ids, so
// saved + continued tokens equal the unsaved run's and never pass MaxNewTokens; ForcedTokenCount stays 0; and the final
// save equals the unsaved run's. Ruling 56 (6): the carry is taken when the save finalizes, so the cell watches the
// pending ids through the save (SaveWatchingPending) and requires one carried event per id pending at its finalize;
// the arms are K and K + 1 for every multi-id result of the full run (up to four), and at least one arm across both
// slots must have carried a multi-id result (two events or more).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40CarryTest, "SuperSLM.MultiRow.MR6.R40_2.ExactSaveCarriesPendingIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40CarryTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeInSchemaRequest(*this, C.Rig, kExact, Req) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; } // ruling 53
	bool bOk = true;
	int32 Carried = 0;
	for (const int32 Budget : { 0, 512 })
	{
		const FString Slot = Budget > 0 ? TEXT("pooled") : TEXT("dedicated");
		if (!C.Reconfigure(*this, EPath::WholeToken, true, Budget)) { return false; }
		FGpuRun Full;
		TArray<FAct> FullActs;
		if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Full, FullActs)) { return false; }
		bOk &= RequireExactRows(*this, FullActs, Slot);
		const int32 K = FirstMultiIdCount(FullActs);
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: an Exact close delivered two or more ids (through id %d)"), *Slot, K), K >= 2)) { return false; }
		TArray<int32> Maxes;
		{
			int32 Cumulative = 0;
			for (const FAct& A : FullActs)
			{
				Cumulative += A.DeliveredTokens.Num();
				if (A.DeliveredTokens.Num() >= 2 && Maxes.Num() < 8) { Maxes.Add(Cumulative); Maxes.Add(Cumulative + 1); }
			}
		}
		for (const int32 Max : Maxes)
		{
			const FString Arm = FString::Printf(TEXT("%s, MaxNewTokens %d"), *Slot, Max);
			FSuperSLMGenerationRequest R = Req;
			R.MaxNewTokens = Max;
			FGpuRun Unsaved;
			TArray<FAct> Ignored;
			TArray<uint8> UnsavedFinal;
			if (!RunMr(*this, C.Rig, EPath::WholeToken, R, Unsaved, Ignored, true)) { return false; }
			bOk &= Save(*this, C.Rig, Unsaved.Sequence, UnsavedFinal);
			ReturnAndSettle(C.Rig, Unsaved.Sequence);
			FGpuRun Before;
			if (!RunMr(*this, C.Rig, EPath::WholeToken, R, Before, Ignored, true, [&]() { return FMr::PendingResultIds(*C.Rig.Gpu, Before.Sequence) >= 2; })) { return false; }
			if (IsEnded(*C.Rig.Gpu, Before.Sequence) || FMr::PendingResultIds(*C.Rig.Gpu, Before.Sequence) < 2)
			{
				AddInfo(*(Arm + TEXT(": no multi-id result was pending at a tick's end; arm skipped")));
				ReturnAndSettle(C.Rig, Before.Sequence);
				continue;
			}
			TArray<uint8> Blob;
			int32 AtFinalize = -1;
			if (!SaveWatchingPending(*this, C.Rig, Before.Sequence, Blob, AtFinalize)) { ReturnAndSettle(C.Rig, Before.Sequence); return false; }
			bOk &= TestEqual(*(Arm + TEXT(": the save is wrapper version 1")), WrapperVersion(Blob), 1);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the save carried one event per id pending at its finalize (%d pending)"), *Arm, AtFinalize), CarriedEvents(Blob), AtFinalize);
			Carried += CarriedEvents(Blob) >= 2 ? 1 : 0;
			const TArray<int32> TokensBefore = C.Rig.Gpu->GetGeneratedTokens(Before.Sequence);
			ReturnAndSettle(C.Rig, Before.Sequence);
			FGpuRun After;
			if (!TestEqual(*(Arm + TEXT(": the restore succeeds")), (uint8)Restore(C.Rig, Blob, After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
			bOk &= TestTrue(*(Arm + TEXT(": the restored run ends")), Finish(C.Rig, After));
			TArray<int32> Joined = TokensBefore;
			Joined.Append(After.Tokens);
			bOk &= TestEqual(*(Arm + TEXT(": saved + continued tokens equal the unsaved run's (XG40)")), Joined, Unsaved.Tokens);
			bOk &= TestTrue(*FString::Printf(TEXT("%s: no token past MaxNewTokens (%d)"), *Arm, Joined.Num()), Joined.Num() <= Max);
			bOk &= TestEqual(*(Arm + TEXT(": ForcedTokenCount after the restore stays 0")), After.ForcedTokenCount, (int64)0);
			TArray<uint8> AfterFinal;
			bOk &= Save(*this, C.Rig, After.Sequence, AfterFinal);
			bOk &= RequireSameLiveSave(*this, C.Rig, AfterFinal, UnsavedFinal, Arm + TEXT(": the final save equals the unsaved run's"));
			ReturnAndSettle(C.Rig, After.Sequence);
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: an arm's save carried a multi-id result (%d arms)"), Carried), Carried > 0);
	return bOk;
}

// R40.3 Re-arm after a dead end (re-points P2H.3's gpu_rest_second_generation, the dead-end legs of
// gpu_rest_transitions_advance and gpu_rest_no_chain_finishes, XG20/XG32; P2.12; the shared halves of P2.7 and
// P2.17). Pooled (the armed flag is readable), both paths, Exact with the switch on against the one-row twin: the
// generation ends at its schema's end with the walk accepting, and on OneCall the end (the -2) is met in an Exact close
// (§4.7 step 5); the sequence is armed after it. Plugin 1.5.0: that end is a clean stop (Complete / SchemaComplete, as
// the CPU's), and a completed sequence still needs a reset before its next begin (D-SLM7946); the reset clears the armed
// flag (K7): an empty-prompt
// begin after it faults by name as unarmed, as the twin's does; a second generation with a prompt (ending inside its
// schema) equals the twin's; the armed flag and the final save equal the twin's. Then Exact over an adopted
// prompt-only prefix (armed at the adopt), with no prompt, equals the twin's: the armed empty-prompt begin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40RearmTest, "SuperSLM.MultiRow.MR6.R40_3.ExactDeadEndRearms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40RearmTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req, Second;
	// Req meets its schema's dead end on purpose (kMaxNew); Second ends inside its schema, so it can be saved (ruling 53).
	if (!MakeRequest(*this, C.Rig, kExact, kMaxNew, Req) ||
		!MakeInSchemaRequest(*this, C.Rig, kExact, Second, SuperSLMSchemaJumpFixtures::PopulationPrompts()[2]) ||
		!SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	FSuperSLMGenerationRequest Empty = Req;
	Empty.PromptTokens.Reset();
	bool bOk = true;
	for (const EPath Path : { EPath::WholeToken, EPath::Sliced4 })
	{
		struct FArm { FGpuRun First, EmptyRun, SecondRun; int32 ArmedAfterFirst = -1, ArmedAfterReset = -1, ArmedAfterSecond = -1; TArray<uint8> Final; };
		FArm Arms[2]; // [0] the one-row twin, [1] multi-row
		for (int32 Multi = 1; Multi >= 0; --Multi)
		{
			const FString Arm = FString::Printf(TEXT("%s, %s"), PathName(Path), Multi ? TEXT("multi-row") : TEXT("one-row twin"));
			FArm& A = Arms[Multi];
			TArray<FAct> Acts;
			if (!C.Reconfigure(*this, Path, Multi != 0, 512) || !RunMr(*this, C.Rig, Path, Req, A.First, Acts, true)) { return false; }
			bOk &= TestEqual(*(Arm + TEXT(": the generation ends SchemaComplete")), (uint8)A.First.Outcome, (uint8)ESuperSLMDecodeOutcome::SchemaComplete);
			bOk &= TestTrue(*(Arm + TEXT(": with the walk accepting")), A.First.bAccepting);
			if (Multi && Path == EPath::WholeToken)
			{
				bOk &= RequireExactRows(*this, Acts, Arm);
				const FAct* DeadEnd = nullptr;
				for (const FAct& X : Acts) { if (X.bDeadEnd) { DeadEnd = &X; } }
				bOk &= TestTrue(*(Arm + TEXT(": the dead end was met in an Exact close (§4.7 step 5)")),
					DeadEnd != nullptr && DeadEnd->Kind == EKind::ExactRows && DeadEnd->bClosedChunk);
			}
			A.ArmedAfterFirst = FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, A.First.Sequence);
			bOk &= TestEqual(*(Arm + TEXT(": armed after the dead end (the close sets ready_for_logits)")), A.ArmedAfterFirst, 1);
			// Plugin 1.5.0: the schema's end left the sequence Complete, as the CPU's; a begin still needs a reset first.
			bOk &= TestEqual(*(Arm + TEXT(": the schema's end is a clean stop")), (uint8)A.First.Phase, (uint8)ESuperSLMSequencePhase::Complete);
			const ESuperSLMGpuDecodePath DPath = DecodePathOf(Path);
			if (!SuperSLMPagingGpuFixtures::ResetAndWait(*this, *C.Rig.Gpu, A.First.Sequence, DPath)) { ReturnAndSettle(C.Rig, A.First.Sequence); return false; }
			A.ArmedAfterReset = FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, A.First.Sequence);
			bOk &= TestEqual(*(Arm + TEXT(": the reset clears the armed flag (K7)")), A.ArmedAfterReset, 0);
			TArray<FAct> Ignored;
			bOk &= RunOn(*this, C.Rig, A.First.Sequence, Empty, A.EmptyRun, Ignored);
			bOk &= TestEqual(*(Arm + TEXT(": an empty-prompt begin on the unarmed sequence faults by name")), (uint8)A.EmptyRun.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
			if (!SuperSLMPagingGpuFixtures::ResetAndWait(*this, *C.Rig.Gpu, A.First.Sequence, DPath)) { ReturnAndSettle(C.Rig, A.First.Sequence); return false; }
			bOk &= RunOn(*this, C.Rig, A.First.Sequence, Second, A.SecondRun, Ignored);
			bOk &= TestEqual(*(Arm + TEXT(": the second generation ends inside its schema")), (uint8)A.SecondRun.Phase, (uint8)ESuperSLMSequencePhase::Complete);
			A.ArmedAfterSecond = FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, A.First.Sequence);
			bOk &= Save(*this, C.Rig, A.First.Sequence, A.Final);
			ReturnAndSettle(C.Rig, A.First.Sequence);
		}
		const FString P = PathName(Path);
		bOk &= SameGpuEnding(*this, Arms[1].First, Arms[0].First, P + TEXT(": the first generation equals the twin's"));
		bOk &= SameGpuEnding(*this, Arms[1].EmptyRun, Arms[0].EmptyRun, P + TEXT(": the empty-prompt generation after the reset equals the twin's"));
		bOk &= TestEqual(*(P + TEXT(": its fault reason equals the twin's")), (uint8)Arms[1].EmptyRun.Fault, (uint8)Arms[0].EmptyRun.Fault);
		bOk &= SameGpuEnding(*this, Arms[1].SecondRun, Arms[0].SecondRun, P + TEXT(": the second generation equals the twin's"));
		bOk &= TestEqual(*(P + TEXT(": the armed flag after the second generation equals the twin's")), Arms[1].ArmedAfterSecond, Arms[0].ArmedAfterSecond);
		bOk &= RequireSameLiveSave(*this, C.Rig, Arms[1].Final, Arms[0].Final, P + TEXT(": the final save equals the twin's"));
	}

	// P2.7's shared half: Exact over an adopted prompt-only prefix, with no prompt.
	TArray<int32> Tokens[2];
	int32 Armed[2] = { -1, -1 };
	for (int32 Multi = 1; Multi >= 0; --Multi)
	{
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!MrConfig(*this, EPath::WholeToken, Multi != 0, Cfg, 512)) { return false; }
		Cfg.PrefixBlockCount = 1;
		Cfg.K = FMath::Max(Cfg.K, Cfg.BlockCount + 1);
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
		FSuperSLMPrefix Prefix;
		FString Error;
		if (!TestEqual(*FString::Printf(TEXT("GPU CreatePrefix (schema, prompt-only) (%s)"), *Error),
			(uint8)C.Rig.Gpu->CreatePrefix(Req.PromptTokens, C.Rig.Schema, ESuperSLMSpanKind::Prompt, Prefix, &Error), (uint8)ESuperSLMPrefixResult::Success) ||
			!SuperSLMPagingGpuFixtures::WaitPrefixReady(*this, *C.Rig.Gpu, Prefix)) { return false; }
		FGpuRun Run;
		if (!Vend(*this, C.Rig, EPath::WholeToken, Run) || !SuperSLMPagingGpuFixtures::AdoptAndWait(*this, *C.Rig.Gpu, Run.Sequence, Prefix)) { return false; }
		TArray<FAct> Acts;
		if (!RunOn(*this, C.Rig, Run.Sequence, Empty, Run, Acts)) { return false; }
		if (Multi) { bOk &= RequireExactRows(*this, Acts, TEXT("Exact over the adopted prefix")); }
		Tokens[Multi] = Run.Tokens;
		Armed[Multi] = FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, Run.Sequence);
		ReturnAndSettle(C.Rig, Run.Sequence);
	}
	bOk &= TestTrue(TEXT("over the prefix: tokens were produced"), Tokens[1].Num() > 0);
	bOk &= TestEqual(TEXT("over the prefix: Exact's tokens equal the twin's"), Tokens[1], Tokens[0]);
	bOk &= TestEqual(TEXT("over the prefix: the armed flag at the end equals the twin's"), Armed[1], Armed[0]);
	return bOk;
}

// R40.4 The restore's armed flag (re-points P2H.3's gpu_rest_on_restore and the dedicated restore's "TS.bArmed from the
// getter", and gpu_rest_begin's armed start). OneCall, Exact with the switch on. Two saved states: not armed, MaxNewTokens
// on a produced token (dedicated and pooled); and armed (pooled, where the flag is readable). Under ruling 53 a dead end
// was a fault and could not be saved, so the armed state here is a sequence that adopted a prompt-only prefix (armed at
// the adopt, K7) and has not begun. A save at a schema's clean end (plugin 1.5.0) is saved and restored by
// SuperSLM.SchemaCleanStop.P9 (and P2-P4's restore), which check the restored ending and stop reason on a dedicated
// slot, not the armed flag; no cell restores the armed flag from such a save. The save is version 1; a restore saved again
// gives the same bytes (the armed word survives); on the pooled slot the restored flag equals the saved sequence's (1
// and 0); and a second generation from the restored sequence equals one from the saved sequence, tokens and final save.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40RestoreArmedTest, "SuperSLM.MultiRow.MR6.R40_4.ExactRestoreKeepsTheArmedFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40RestoreArmedTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req, Second;
	if (!MakeInSchemaRequest(*this, C.Rig, kExact, Req) ||
		!MakeInSchemaRequest(*this, C.Rig, kExact, Second, SuperSLMSchemaJumpFixtures::PopulationPrompts()[2]) ||
		!SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	// Ruling 56 (6): on the armed shape Second runs over the adopted prefix (Req's prompt), so its MaxNew is trimmed with
	// the prefix included: the CPU's Off run of the prefix and Second's prompt must end inside the schema.
	FSuperSLMGenerationRequest SecondOverPrefix = Second;
	{
		FSuperSLMGenerationRequest Joined = Second;
		Joined.PromptTokens = Req.PromptTokens;
		Joined.PromptTokens.Append(Second.PromptTokens);
		Joined.MaxNewTokens = kMaxNew;
		FRunResult Probe;
		if (!RunCpu(*this, C.Rig, Joined, kOff, Probe)) { return false; }
		if (Probe.Outcome == ESuperSLMDecodeOutcome::SchemaComplete)
		{
			if (!TestTrue(*FString::Printf(TEXT("precondition: over the prefix, Second reaches its schema's end after two tokens or more (%d)"), Probe.Tokens.Num()),
				Probe.Phase == ESuperSLMSequencePhase::Complete && Probe.Tokens.Num() >= 2)) { return false; }
			Joined.MaxNewTokens = Probe.Tokens.Num() - 1;
			if (!RunCpu(*this, C.Rig, Joined, kOff, Probe)) { return false; }
		}
		if (!TestEqual(*FString::Printf(TEXT("precondition: over the prefix, Second's MaxNewTokens %d ends inside the schema"), Joined.MaxNewTokens),
			(uint8)Probe.Phase, (uint8)ESuperSLMSequencePhase::Complete)) { return false; }
		SecondOverPrefix.MaxNewTokens = Joined.MaxNewTokens;
	}
	bool bOk = true;
	for (const int32 Budget : { 0, 512 })
	{
		const bool bPooled = Budget > 0;
		for (const int32 Shape : { 0, 1 }) // 0: armed (an adopted prompt-only prefix, pooled only); 1: MaxNewTokens on a produced token
		{
			if (Shape == 0 && !bPooled) { continue; }
			const FString Arm = FString::Printf(TEXT("%s, %s"), bPooled ? TEXT("pooled") : TEXT("dedicated"), Shape == 0 ? TEXT("armed at an adopt") : TEXT("MaxNewTokens"));
			FSuperSLMGpuRuntimeConfig Cfg;
			if (!MrConfig(*this, EPath::WholeToken, true, Cfg, Budget)) { return false; }
			if (Shape == 0)
			{
				Cfg.PrefixBlockCount = 1;
				Cfg.K = FMath::Max(Cfg.K, Cfg.BlockCount + 1);
			}
			if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
			FGpuRun Orig;
			TArray<FAct> Ignored;
			if (Shape == 1)
			{
				FGpuRun Full;
				TArray<FAct> FullActs;
				if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Full, FullActs)) { return false; }
				bOk &= RequireExactRows(*this, FullActs, Arm);
				if (!TestTrue(*(Arm + TEXT(": precondition: the full run is long enough to stop on a produced token")), Full.Tokens.Num() >= 4)) { return false; }
				FSuperSLMGenerationRequest R = Req;
				R.MaxNewTokens = Full.Tokens.Num() - 2;
				if (!RunMr(*this, C.Rig, EPath::WholeToken, R, Orig, Ignored, true)) { return false; }
				bOk &= TestEqual(*(Arm + TEXT(": the run completes on MaxNewTokens")), (uint8)Orig.Phase, (uint8)ESuperSLMSequencePhase::Complete);
			}
			else
			{
				FSuperSLMPrefix Prefix;
				FString Error;
				if (!TestEqual(*FString::Printf(TEXT("%s: GPU CreatePrefix (schema, prompt-only) (%s)"), *Arm, *Error),
					(uint8)C.Rig.Gpu->CreatePrefix(Req.PromptTokens, C.Rig.Schema, ESuperSLMSpanKind::Prompt, Prefix, &Error), (uint8)ESuperSLMPrefixResult::Success) ||
					!SuperSLMPagingGpuFixtures::WaitPrefixReady(*this, *C.Rig.Gpu, Prefix)) { return false; }
				if (!Vend(*this, C.Rig, EPath::WholeToken, Orig) || !SuperSLMPagingGpuFixtures::AdoptAndWait(*this, *C.Rig.Gpu, Orig.Sequence, Prefix)) { return false; }
				FString ReleaseError;
				C.Rig.Gpu->ReleasePrefix(Prefix, ReleaseError); // the adopter keeps what it adopted
			}
			const int32 ArmedBefore = FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, Orig.Sequence);
			if (bPooled)
			{
				bOk &= TestEqual(*(Arm + TEXT(": the saved sequence's armed flag")), ArmedBefore, Shape == 0 ? 1 : 0);
			}
			TArray<uint8> Blob;
			if (!Save(*this, C.Rig, Orig.Sequence, Blob)) { ReturnAndSettle(C.Rig, Orig.Sequence); return false; }
			bOk &= TestEqual(*(Arm + TEXT(": the save is wrapper version 1")), WrapperVersion(Blob), 1);
			FGpuRun Res;
			if (!TestEqual(*(Arm + TEXT(": the restore succeeds")), (uint8)Restore(C.Rig, Blob, Res.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
			if (bPooled)
			{
				bOk &= TestEqual(*(Arm + TEXT(": the restored armed flag equals the saved one")), FSuperSLMPagedGpuTestSeam::SequenceArmed(*C.Rig.Gpu, Res.Sequence), ArmedBefore);
			}
			TArray<uint8> Again;
			bOk &= Save(*this, C.Rig, Res.Sequence, Again);
			bOk &= RequireSameLiveSave(*this, C.Rig, Again, Blob, Arm + TEXT(": the restored sequence saves to the same state"));
			// A Complete sequence takes a new generation only after a reset (D-SLM7946); an adopted one has not begun.
			if (Shape == 1)
			{
				if (!SuperSLMPagingGpuFixtures::ResetAndWait(*this, *C.Rig.Gpu, Orig.Sequence) || !SuperSLMPagingGpuFixtures::ResetAndWait(*this, *C.Rig.Gpu, Res.Sequence))
				{
					ReturnAndSettle(C.Rig, Orig.Sequence);
					ReturnAndSettle(C.Rig, Res.Sequence);
					return false;
				}
			}
			FGpuRun Orig2, Res2;
			TArray<uint8> OrigFinal, ResFinal;
			const FSuperSLMGenerationRequest& Next = Shape == 0 ? SecondOverPrefix : Second;
			bOk &= RunOn(*this, C.Rig, Orig.Sequence, Next, Orig2, Ignored);
			bOk &= RunOn(*this, C.Rig, Res.Sequence, Next, Res2, Ignored);
			bOk &= SameGpuEnding(*this, Res2, Orig2, Arm + TEXT(": a second generation from the restore equals one from the saved sequence"));
			bOk &= Save(*this, C.Rig, Orig.Sequence, OrigFinal);
			bOk &= Save(*this, C.Rig, Res.Sequence, ResFinal);
			bOk &= RequireSameLiveSave(*this, C.Rig, ResFinal, OrigFinal, Arm + TEXT(": and so do their final saves"));
			ReturnAndSettle(C.Rig, Orig.Sequence);
			ReturnAndSettle(C.Rig, Res.Sequence);
		}
	}
	return bOk;
}

// R40.5 The limits (re-points P2.11's Exact leg, and P2H.1's clip at the limits, XG16, where Exact shares it). OneCall,
// Exact with the switch on, against the one-row twin in the same configuration: with MaxNewTokens inside the first
// multi-id Exact result, at a context cap and at a pooled limit that fall there, the tokens and the ending equal the
// twin's; at MaxNewTokens the run has exactly MaxNewTokens tokens, a prefix of the full run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40LimitsTest, "SuperSLM.MultiRow.MR6.R40_5.ExactAtTheLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40LimitsTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	if (!MakeRequest(*this, C.Rig, kExact, kMaxNew, Req) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	if (!C.Reconfigure(*this, EPath::WholeToken, true)) { return false; }
	FGpuRun Full;
	TArray<FAct> FullActs;
	if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Full, FullActs)) { return false; }
	bool bOk = RequireExactRows(*this, FullActs, TEXT("R40.5"));
	const int32 K = FirstMultiIdCount(FullActs);
	if (!TestTrue(*FString::Printf(TEXT("precondition: an Exact close delivered two or more ids (through id %d)"), K), K >= 2)) { return false; }
	const int32 Inside = K - 1; // one id short of the result's end: the limit falls inside it
	const int64 Prompt = Req.PromptTokens.Num();
	struct FLimit { const TCHAR* Name; int32 MaxNew; int32 Budget; int64 Cap; };
	const FLimit Limits[] = {
		{ TEXT("MaxNewTokens"), Inside, 0, 4096 },
		{ TEXT("the context cap"), kMaxNew, 0, Prompt + Inside },
		{ TEXT("the pooled limit"), kMaxNew, static_cast<int32>(Prompt + Inside), 4096 },
	};
	for (const FLimit& L : Limits)
	{
		FGpuRun Runs[2];
		for (int32 Multi = 1; Multi >= 0; --Multi)
		{
			FSuperSLMGpuRuntimeConfig Cfg;
			if (!MrConfig(*this, EPath::WholeToken, Multi != 0, Cfg, L.Budget)) { return false; }
			Cfg.ContextCap = L.Cap;
			if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
			FSuperSLMGenerationRequest R = Req;
			R.MaxNewTokens = L.MaxNew;
			TArray<FAct> Acts;
			if (!RunMr(*this, C.Rig, EPath::WholeToken, R, Runs[Multi], Acts)) { return false; }
		}
		bOk &= SameGpuEnding(*this, Runs[1], Runs[0], FString::Printf(TEXT("at %s: Exact equals the twin"), L.Name));
		bOk &= TestTrue(*FString::Printf(TEXT("at %s: a prefix of the full run (XG16)"), L.Name), IsPrefixOf(Runs[1].Tokens, Full.Tokens));
		if (L.MaxNew == Inside)
		{
			bOk &= TestEqual(TEXT("at MaxNewTokens: exactly MaxNewTokens tokens"), Runs[1].Tokens.Num(), Inside);
		}
	}
	return bOk;
}

// R40.6 Mixed modes (re-points P2.9's Off, Exact and unbound members). On both paths, with the switch on: an Off, an
// Exact and an unbound sequence begun together, each equal to its solo run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR40MixedModesTest, "SuperSLM.MultiRow.MR6.R40_6.MixedModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR40MixedModesTest::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	const ESuperSLMSchemaJumpForward Modes[3] = { kOff, kExact, kOff };
	const bool bUnbound[3] = { false, false, true };
	FSuperSLMGenerationRequest Reqs[3];
	for (int32 I = 0; I < 3; ++I)
	{
		if (!MakeRequest(*this, C.Rig, Modes[I], kMaxNew, Reqs[I], SuperSLMSchemaJumpFixtures::PopulationPrompts()[I])) { return false; }
	}
	if (!SeedExactDrafts(*this, C.Rig, Reqs[1])) { return false; }
	bool bOk = true;
	for (const EPath Path : { EPath::WholeToken, EPath::Sliced4 })
	{
		const FString P = PathName(Path);
		if (!C.Reconfigure(*this, Path, true)) { return false; }
		FGpuRun Solo[3], Mixed[3];
		for (int32 I = 0; I < 3; ++I)
		{
			Solo[I] = FGpuRun();
			if (!Vend(*this, C.Rig, Path, Solo[I], bUnbound[I]) || !Drive(*this, C.Rig, Reqs[I], Solo[I])) { return false; }
			ReturnAndSettle(C.Rig, Solo[I].Sequence);
		}
		FMr::StartRecording(*C.Rig.Gpu);
		for (int32 I = 0; I < 3; ++I)
		{
			if (!Vend(*this, C.Rig, Path, Mixed[I], bUnbound[I])) { return false; }
		}
		for (int32 I = 0; I < 3; ++I)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s, member %d: RequestBeginGeneration"), *P, I), C.Rig.Gpu->RequestBeginGeneration(Mixed[I].Sequence, Reqs[I]).IsValid());
		}
		const bool bEnded = Pump(*C.Rig.Gpu, [&]()
		{
			for (const FGpuRun& R : Mixed) { if (!IsEnded(*C.Rig.Gpu, R.Sequence)) { return false; } }
			return true;
		});
		bOk &= TestTrue(*(P + TEXT(": every mixed run ends")), bEnded);
		if (Path == EPath::WholeToken) { bOk &= RequireExactRows(*this, ActionsOf(*C.Rig.Gpu, Mixed[1].Sequence.Id), P + TEXT(", the Exact member")); }
		for (int32 I = 0; I < 3; ++I)
		{
			Read(C.Rig, Mixed[I]);
			bOk &= SameGpuEnding(*this, Mixed[I], Solo[I], FString::Printf(TEXT("%s, member %d (%s%s)"), *P, I, SuperSLMSchemaJumpFixtures::ModeName(Modes[I]), bUnbound[I] ? TEXT(", unbound") : TEXT("")));
			ReturnAndSettle(C.Rig, Mixed[I].Sequence);
		}
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ruling 42 (code-review/poirot-mr6.md: FIX FIRST) and ruling 42a. M1: Composed ticks within B with multi-row on, 1.17's
// uncapped slices with it off. M2: a fault, a reset and a begin leave no stale chunk. R1: OneCall below one row slices
// one-row tokens. S1: a failed Exact open runs Off's step with no fault (re-points P2.Failure and test_gpu_status_map's
// healthy failures). S2: an Exact step with no draft is one Off token of device work, and a save between that chunk's
// open and close abandons cleanly. S5: GPU Exact drafts as CPU Exact does (no table draft after an empty forced chain).
// Tick prices come from the vendored cost table and the plan's pinned conversion (§5.1, rulings 39 Q1/Q2 and 42),
// never from the build's chooser.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
	using EFault = ESuperSLMGpuRowsFaultPoint;

	int64 PinnedUs(double Ms, bool bCost)
	{
		const double V = Ms * 1000.0;
		const double N = FMath::RoundHalfFromZero(V);
		if (FMath::Abs(V - N) <= 0.01) { return static_cast<int64>(N); }
		return static_cast<int64>(bCost ? FMath::CeilToDouble(V) : FMath::FloorToDouble(V));
	}

	struct FTickPrices
	{
		int64 C[8] = {};  // C(M), us
		int64 G[8] = {};  // G(M), us
		int64 C1 = 0;
		int64 RoomUs = 0; // B_eff - Scene (ruling 39 Q1)
		int64 Margin = 0;
		int64 L = 0;
		bool bKnob = false; // SchemaJumpGpuFrameBudgetMs set: E-14 (ruling 53) prices M = 1 with the margin
	};

	FTickPrices PricesOf(const FSuperSLMGpuRuntimeConfig& Cfg)
	{
		FTickPrices P;
		// Ruling 48a: GpuRowCostMs 0 is unset; with calibration off (MrConfig) the row cost in use is then 6.2 ms.
		P.C1 = PinnedUs(Cfg.GpuRowCostMs > 0.0 ? Cfg.GpuRowCostMs : 6.2, true);
		const int64 S = PinnedUs(Cfg.SceneGpuShareMs, true);
		const int64 B = Cfg.SchemaJumpGpuFrameBudgetMs != 0.0
			? FMath::Min(PinnedUs(Cfg.SchemaJumpGpuFrameBudgetMs, false), PinnedUs(Cfg.NominalTickMs, false)) : P.C1 + S;
		for (int32 M = 0; M < 8; ++M)
		{
			P.C[M] = (static_cast<int64>(superslm_gpu::kGpuRowsCostRatioPerMille[M]) * P.C1 + 999) / 1000;
			P.G[M] = (static_cast<int64>(superslm_gpu::kGpuRowsLogitsRatioPerMille[M]) * P.C1 + 999) / 1000;
		}
		P.RoomUs = B - S;
		P.Margin = Cfg.GpuRowsMarginPerMille;
		P.L = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		P.bKnob = Cfg.SchemaJumpGpuFrameBudgetMs != 0.0;
		return P;
	}

	// l(1) = min(L, max(1, floor(L x Room / C(1)))) (ruling 39 Q2); with the knob set, erratum E-14 (ruling 53) gives it
	// the margin: min(L, max(1, floor(L x Room x 1000 / (C(1) x (1000 + m))))).
	int64 OneRowLayers(const FTickPrices& P)
	{
		if (P.RoomUs <= 0) { return 1; }
		const int64 Raw = P.bKnob ? P.L * P.RoomUs * 1000 / (P.C1 * (1000 + P.Margin)) : P.L * P.RoomUs / P.C1;
		return FMath::Min(P.L, FMath::Max<int64>(1, Raw));
	}

	// An action's price x L x 1000: M = 1 at C(1), with the margin when the knob is set (E-14), without at B = 0; M >= 2
	// with the margin; an Exact close that verified rows adds the logits stage (§4.7, §5.1).
	int64 ScaledPrice(const FTickPrices& P, const FAct& A)
	{
		if (A.Layers <= 0) { return 0; }
		if (A.Rows <= 1) { return static_cast<int64>(A.Layers) * P.C[0] * (P.bKnob ? 1000 + P.Margin : 1000); }
		const int32 Mi = FMath::Clamp(A.Rows, 1, 8) - 1;
		int64 X = static_cast<int64>(A.Layers) * P.C[Mi] * (1000 + P.Margin);
		if (A.Kind == EKind::ExactRows && A.bClosedChunk && !A.bFellBack) { X += P.G[Mi] * (1000 + P.Margin) * P.L; }
		return X;
	}

	// Each recorded tick's price, in us, keyed by tick, and its layers.
	void TickPrices(const TArray<FAct>& Acts, const FTickPrices& P, TMap<int64, double>& OutUs, TMap<int64, int32>& OutLayers)
	{
		TMap<int64, int64> Scaled;
		for (const FAct& A : Acts)
		{
			Scaled.FindOrAdd(A.Tick) += ScaledPrice(P, A);
			OutLayers.FindOrAdd(A.Tick) += A.Layers;
		}
		for (const TPair<int64, int64>& KV : Scaled) { OutUs.Add(KV.Key, static_cast<double>(KV.Value) / static_cast<double>(P.L * 1000)); }
	}

	// Every tick is priced within Room; a tick of one layer in all is ruling 39 Q2's always-admitted layer.
	bool RequireTicksWithinBudget(FAutomationTestBase& T, const TArray<FAct>& Acts, const FTickPrices& P, const FString& What)
	{
		TMap<int64, double> Us;
		TMap<int64, int32> Layers;
		TickPrices(Acts, P, Us, Layers);
		bool bOk = T.TestTrue(*(What + TEXT(": actions ran")), Acts.Num() > 0);
		for (const TPair<int64, double>& KV : Us)
		{
			if (Layers.FindRef(KV.Key) <= 1) { continue; }
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: priced %.1f us, within B (Room %lld us)"), *What, KV.Key, KV.Value, P.RoomUs),
				KV.Value <= static_cast<double>(P.RoomUs) + 1e-9);
		}
		return bOk;
	}

	double MaxTickUs(const TArray<FAct>& Acts, const FTickPrices& P)
	{
		TMap<int64, double> Us;
		TMap<int64, int32> Layers;
		TickPrices(Acts, P, Us, Layers);
		double Max = 0.0;
		for (const TPair<int64, double>& KV : Us) { Max = FMath::Max(Max, KV.Value); }
		return Max;
	}

	// Vends one sequence per request on Path, begins them together and runs them all to their ends.
	bool RunTogether(FAutomationTestBase& T, const FGpuRig& Rig, EPath Path, const TArray<FSuperSLMGenerationRequest>& Reqs, TArray<FGpuRun>& Out)
	{
		Out.SetNum(Reqs.Num());
		for (FGpuRun& R : Out) { R = FGpuRun(); if (!Vend(T, Rig, Path, R)) { return false; } }
		FMr::StartRecording(*Rig.Gpu);
		for (int32 I = 0; I < Reqs.Num(); ++I)
		{
			if (!T.TestTrue(*FString::Printf(TEXT("member %d: RequestBeginGeneration"), I), Rig.Gpu->RequestBeginGeneration(Out[I].Sequence, Reqs[I]).IsValid())) { return false; }
		}
		const bool bEnded = Pump(*Rig.Gpu, [&]()
		{
			for (const FGpuRun& R : Out) { if (!IsEnded(*Rig.Gpu, R.Sequence)) { return false; } }
			return true;
		});
		for (FGpuRun& R : Out) { Read(Rig, R); }
		return T.TestTrue(TEXT("every member ends"), bEnded);
	}

	// The Exact chunks in a sequence's record, each from its opening action to its close or abandon.
	TArray<TArray<FAct>> ExactChunksOf(const TArray<FAct>& Acts)
	{
		TArray<TArray<FAct>> Out;
		bool bIn = false;
		for (const FAct& A : Acts)
		{
			if (A.Kind != EKind::ExactRows) { continue; }
			if (A.bOpenedChunk) { Out.AddDefaulted(); bIn = true; }
			if (bIn) { Out.Last().Add(A); }
			if (A.bClosedChunk || A.bAbandonedChunk) { bIn = false; }
		}
		return Out;
	}

	bool FellBack(const TArray<FAct>& Chunk)
	{
		return Chunk.Num() > 0 && Chunk.Last().bClosedChunk && Chunk.Last().bFellBack;
	}

	// Ruling 42a: a chunk that fell back did one Off token of device work: one action with layers, its close, one row,
	// the model's layers; every other slice of it ran nothing.
	bool RequireOneOffTokenOfWork(FAutomationTestBase& T, const TArray<FAct>& Chunk, int32 L, const FString& What)
	{
		int32 Working = 0;
		for (const FAct& A : Chunk) { Working += A.Layers > 0 ? 1 : 0; }
		const FAct& Close = Chunk.Last();
		bool bOk = T.TestEqual(*(What + TEXT(": one action of the chunk did device work")), Working, 1);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the close ran Off's whole-token step (rows %d, layers %d)"), *What, Close.Rows, Close.Layers),
			Close.bClosedChunk && Close.Rows == 1 && Close.Layers == L);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the close delivered one id (%d)"), *What, Close.DeliveredTokens.Num()), Close.DeliveredTokens.Num() <= 1);
		return bOk;
	}

	// Ruling 53 (triage run 3, finding 6): a no-draft fallback happens only at an Exact open whose forced chain is empty,
	// which needs two free tokens in a row (each close produces the free token itself); the pinned prompt never gave one.
	// Over the population prompts in turn (each ending inside its schema), Exact with multi-row on, OneCall, at the
	// default B and with no draft table: the first prompt whose run fell back for want of a draft is the cell's. Whether
	// a chain is empty depends on the tokens and the schema only, never on the path or B (tokens equal Off's, C17).
	// Ruling 56 (6): when no population prompt meets one, the cell forces the no-draft step through the seam: bOutArm
	// is set, Out is the first population prompt's request, and the cell calls ArmNoDraftStep() before each multi-row
	// Exact run (one shot: the run's first Exact open takes the no-draft path).
	bool FindNoDraftRequest(FAutomationTestBase& T, FMrCell& C, FSuperSLMGenerationRequest& Out, bool& bOutArm)
	{
		bOutArm = false;
		FString Tried;
		for (const FString& Prompt : SuperSLMSchemaJumpFixtures::PopulationPrompts())
		{
			FSuperSLMGenerationRequest Req;
			if (!MakeInSchemaRequest(T, C.Rig, kExact, Req, Prompt) || !C.Reconfigure(T, EPath::WholeToken, true)) { return false; }
			FGpuRun Run;
			TArray<FAct> Acts;
			if (!RunMr(T, C.Rig, EPath::WholeToken, Req, Run, Acts)) { return false; }
			int32 NoDraft = 0, OpenFailed = 0, FellBackChunks = 0;
			FMr::GetExactFallbacks(*C.Rig.Gpu, NoDraft, OpenFailed);
			for (const TArray<FAct>& Chunk : ExactChunksOf(Acts)) { FellBackChunks += FellBack(Chunk) ? 1 : 0; }
			Tried += FString::Printf(TEXT(" [\"%s\": %d no-draft fallbacks, %d fallback chunks]"), *Prompt, NoDraft, FellBackChunks);
			if (NoDraft > 0 && FellBackChunks > 0)
			{
				T.AddInfo(FString::Printf(TEXT("the no-draft scenario's prompt (ruling 53):%s"), *Tried));
				Out = Req;
				return true;
			}
		}
		T.AddInfo(FString::Printf(TEXT("no population prompt met an empty forced chain; the no-draft step is forced through ArmNoDraftStep (ruling 56):%s"), *Tried));
		if (!MakeInSchemaRequest(T, C.Rig, kExact, Out, SuperSLMSchemaJumpFixtures::PopulationPrompts()[0]) || !C.Reconfigure(T, EPath::WholeToken, true)) { return false; }
		bOutArm = true;
		return true;
	}

	// Ruling 56 (6): arms the no-draft step when the cell's request needs it.
	bool ArmNoDraftIf(FAutomationTestBase& T, FMrCell& C, bool bArm, const FString& What)
	{
		return !bArm || T.TestTrue(*(What + TEXT(": the no-draft step is armed (ArmNoDraftStep)")), FMr::ArmNoDraftStep(*C.Rig.Gpu));
	}
}

// R42.M1 (review M1; §5.3; ruling 39 Q2; P4.4). Composed, LayersPerTick 4, L 24, B = 1 ms (LayersPerTick x C(1) / L =
// 1,033 us is over it). With multi-row on, three Off sequences run together and every recorded tick's price is within
// B; tokens equal the CPU's Off run. With multi-row off, 1.17's composed slices stay uncapped: every one-row slice runs
// min(LayersPerTick, the layers left), so a tick is priced over B, and the tokens are the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42M1Test, "SuperSLM.MultiRow.MR6.R42_M1.ComposedTicksWithinBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42M1Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	TArray<FSuperSLMGenerationRequest> Reqs;
	TArray<FRunResult> Cpu;
	Reqs.SetNum(3);
	Cpu.SetNum(3);
	for (int32 I = 0; I < 3; ++I)
	{
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Reqs[I], SuperSLMSchemaJumpFixtures::PopulationPrompts()[I]) || !RunCpu(*this, C.Rig, Reqs[I], kOff, Cpu[I])) { return false; }
	}
	bool bOk = true;
	for (const bool bMulti : { true, false })
	{
		const FString Arm = bMulti ? TEXT("multi-row on") : TEXT("multi-row off");
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!MrConfig(*this, EPath::Sliced4, bMulti, Cfg)) { return false; }
		Cfg.SchemaJumpGpuFrameBudgetMs = 1.0;
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
		const FTickPrices P = PricesOf(Cfg);
		if (!TestTrue(*FString::Printf(TEXT("precondition: 4 one-row layers (%lld us) are over B (%lld us), and l(1) = %lld"), 4 * P.C[0] / P.L, P.RoomUs, OneRowLayers(P)),
			4 * P.C[0] > P.RoomUs * P.L && OneRowLayers(P) < 4)) { return false; }
		TArray<FSuperSLMGenerationRequest> These = Reqs;
		if (!bMulti) { These.SetNum(1); }
		TArray<FGpuRun> Runs;
		if (!RunTogether(*this, C.Rig, EPath::Sliced4, These, Runs)) { return false; }
		const TArray<FAct> Acts = FMr::GetActions(*C.Rig.Gpu);
		for (int32 I = 0; I < Runs.Num(); ++I)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("%s, member %d: tokens equal the CPU's Off run"), *Arm, I), Runs[I].Tokens, Cpu[I].Tokens);
			ReturnAndSettle(C.Rig, Runs[I].Sequence);
		}
		if (bMulti)
		{
			bOk &= RequireTicksWithinBudget(*this, Acts, P, Arm);
		}
		else
		{
			bOk &= RequireOneRowOnly(*this, Acts, Arm);
			for (const FAct& A : Acts)
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s, action %d: 1.17's slice, min(LayersPerTick, the layers left)"), *Arm, A.ActionIndex),
					A.Layers, FMath::Min(4, static_cast<int32>(P.L) - A.FirstLayer));
			}
			bOk &= TestTrue(*FString::Printf(TEXT("%s: 1.17's composed slices are not budget-capped (a tick is priced %.1f us, over %lld)"), *Arm, MaxTickUs(Acts, P), P.RoomUs),
				MaxTickUs(Acts, P) > static_cast<double>(P.RoomUs));
		}
	}
	return bOk;
}

// R59_1 (ruling 59.1; run 5's issue 1). Composed, multi-row on, B = 2 ms on the 0.5B: three Off members together, so some
// tick holds two or more one-row slices planned short of their token's end. Layer 1's batch call gives each member the
// running remainder up to its token's end, so a batch holds at most one short member, placed last (where the remainder is
// exactly its slice); any other short member runs as its own call. Passes when no tick batched two short members, some tick
// held two or more short members (the case is exercised), no
// member faulted (no SSLM_BATCH_BUDGET_EXHAUSTED; the product's PlanLayerPos check after every decode, ruling 59.1, faults a
// member on a mismatch), every tick is within B, and the tokens equal the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR591Test, "SuperSLM.MultiRow.MR6.R59_1.ShortMembersNeverShareTheBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR591Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	TArray<FSuperSLMGenerationRequest> Reqs;
	TArray<FRunResult> Cpu;
	Reqs.SetNum(3);
	Cpu.SetNum(3);
	for (int32 I = 0; I < 3; ++I)
	{
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Reqs[I], SuperSLMSchemaJumpFixtures::PopulationPrompts()[I]) || !RunCpu(*this, C.Rig, Reqs[I], kOff, Cpu[I])) { return false; }
	}
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!MrConfig(*this, EPath::Sliced4, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	const FTickPrices P = PricesOf(Cfg);
	TArray<FGpuRun> Runs;
	if (!RunTogether(*this, C.Rig, EPath::Sliced4, Reqs, Runs)) { return false; }
	const TArray<FAct> Acts = FMr::GetActions(*C.Rig.Gpu);
	bool bOk = true;
	for (int32 I = 0; I < Runs.Num(); ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("member %d: tokens equal the CPU's Off run (no member faulted)"), I), Runs[I].Tokens, Cpu[I].Tokens);
		ReturnAndSettle(C.Rig, Runs[I].Sequence);
	}
	bOk &= RequireTicksWithinBudget(*this, Acts, P, TEXT("Composed at B = 2 ms"));
	TMap<int64, int32> ShortPerTick;
	TMap<int64, int32> BatchedShortPerTick;
	int32 MaxShort = 0, BatchedShort = 0;
	for (const FAct& A : Acts)
	{
		if (A.Kind != EKind::OneRow || A.Layers <= 0) { continue; }
		const bool bShort = A.FirstLayer + A.Layers < static_cast<int32>(P.L);
		if (!bShort) { continue; }
		if (A.bBatched)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("action %d (tick %lld): a short slice (layers %d..%d) shares the batch with no other short slice"),
				A.ActionIndex, A.Tick, A.FirstLayer, A.FirstLayer + A.Layers), ++BatchedShortPerTick.FindOrAdd(A.Tick) <= 1);
			++BatchedShort;
		}
		MaxShort = FMath::Max(MaxShort, ++ShortPerTick.FindOrAdd(A.Tick));
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: some tick held two or more short one-row slices (most in a tick: %d)"), MaxShort), MaxShort >= 2);
	// 59g: the batched-short path must run. At B = 2 ms with multi-row on, a member that ends its token and a short member
	// rarely share a tick, so a second leg runs 1.17's path (multi-row off, B = 0) at 5 layers per tick: a token's last
	// slice (24 = 4 x 5 + 4) leaves one unit, which the next member takes short, placed last in the batch (59.1).
	{
		FSuperSLMGpuRuntimeConfig OffCfg;
		if (!MrConfig(*this, EPath::Sliced4, false, OffCfg, 0, 3)) { return false; }
		constexpr int32 kLpt = 5;
		const int32 LegL = SuperSLML2S2Fixtures::AExNumHiddenLayers;
		OffCfg.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(kLpt);
		OffCfg.K = FMath::Max3(LegL, 4, (3 * LegL + kLpt - 1) / kLpt);
		if (!ConfigureGpu(*this, C.Rig, OffCfg)) { return false; }
		TArray<FGpuRun> OffRuns;
		if (!RunTogether(*this, C.Rig, EPath::Sliced4, Reqs, OffRuns)) { return false; }
		const TArray<FAct> OffActs = FMr::GetActions(*C.Rig.Gpu);
		for (int32 I = 0; I < OffRuns.Num(); ++I)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("1.17 leg, member %d: tokens equal the CPU's Off run"), I), OffRuns[I].Tokens, Cpu[I].Tokens);
			ReturnAndSettle(C.Rig, OffRuns[I].Sequence);
		}
		TMap<int64, int32> OffBatchedShort;
		for (const FAct& A : OffActs)
		{
			if (A.Kind != EKind::OneRow || A.Layers <= 0 || !A.bBatched || A.FirstLayer + A.Layers >= static_cast<int32>(P.L)) { continue; }
			bOk &= TestTrue(*FString::Printf(TEXT("1.17 leg, action %d (tick %lld): one short slice per batch"), A.ActionIndex, A.Tick), ++OffBatchedShort.FindOrAdd(A.Tick) <= 1);
			++BatchedShort;
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: a short slice ran in a batch, placed last (59.1; 59g: %d)"), BatchedShort), BatchedShort > 0);
	return bOk;
}
// R42.R1 (ruling 42 R1, re-ruling 39 Q2; 10.4's OneCall arm). OneCall, multi-row on, B = 2 ms (below one row): two Off
// sequences together. Every recorded tick's price is within B; one-row work is sliced at l(1) (no one-row action runs
// more than l(1) layers, and some runs fewer than L); tokens equal the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42R1Test, "SuperSLM.MultiRow.MR6.R42_R1.OneCallBelowOneRowIsSliced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42R1Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	TArray<FSuperSLMGenerationRequest> Reqs;
	TArray<FRunResult> Cpu;
	Reqs.SetNum(2);
	Cpu.SetNum(2);
	for (int32 I = 0; I < 2; ++I)
	{
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Reqs[I], SuperSLMSchemaJumpFixtures::PopulationPrompts()[I]) || !RunCpu(*this, C.Rig, Reqs[I], kOff, Cpu[I])) { return false; }
	}
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!MrConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	const FTickPrices P = PricesOf(Cfg);
	const int64 L1 = OneRowLayers(P);
	if (!TestTrue(*FString::Printf(TEXT("precondition: B (%lld us) is below one row (%lld us); l(1) = %lld"), P.RoomUs, P.C1, L1), P.RoomUs < P.C1 && L1 < P.L)) { return false; }
	TArray<FGpuRun> Runs;
	if (!RunTogether(*this, C.Rig, EPath::WholeToken, Reqs, Runs)) { return false; }
	const TArray<FAct> Acts = FMr::GetActions(*C.Rig.Gpu);
	bool bOk = true;
	for (int32 I = 0; I < Runs.Num(); ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("member %d: tokens equal the CPU's Off run"), I), Runs[I].Tokens, Cpu[I].Tokens);
		ReturnAndSettle(C.Rig, Runs[I].Sequence);
	}
	bOk &= RequireTicksWithinBudget(*this, Acts, P, TEXT("OneCall at B = 2 ms"));
	bool bSliced = false;
	for (const FAct& A : Acts)
	{
		if (A.Kind != EKind::OneRow) { continue; }
		bSliced |= A.Layers < P.L;
		bOk &= TestTrue(*FString::Printf(TEXT("action %d: a one-row action runs at most l(1) = %lld layers (%d)"), A.ActionIndex, L1, A.Layers), A.Layers <= L1);
	}
	bOk &= TestTrue(TEXT("one-row tokens are sliced (1.17's layer slicing on OneCall)"), bSliced);
	return bOk;
}

// R42.M2 (review M2). Pooled, OneCall, K = 4 (so a chunk's next slices are planned before its first result is applied),
// on the 1.5B artifact with the shopkeeper adapter bound: the first chunk's first slice fails (embed_rows(PROMPT)
// refused), and the generation faults. The holder resets the sequence, returns it to the base adapter and begins the
// same request again: the second generation completes, equals a fresh sequence's run, plans no stale slice (its first
// action opens a chunk or is one-row work, and passes the base adapter), and the old adapter then unmaps.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42M2Test, "SuperSLM.MultiRow.MR6.R42_M2.FaultResetBeginLeavesNoStaleChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42M2Test::RunTest(const FString& Parameters)
{
	namespace PG = SuperSLMPagingGpuFixtures;
	if (!Gate(*this)) { return false; }
	PG::FGpuPagingRig Rig;
	if (!Rig.Init(*this, PG::EGpuArtifact::TwoCap15B)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const FString AdapterPath = SuperSLMTestDataPaths::HfCache(TEXT("superslm_artifacts/qwen2.5-1.5b-shopkeeper-lora-v2-t2102-runtime.sslm"));
	TArray<int32> Prompt;
	if (!PG::MakeTokens(*this, *Rig.Model, 40, 712, Prompt)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg = PG::MakeGpuConfig(PG::EGpuArtifact::TwoCap15B, 3, 512, 1, 0);
	if (!TestTrue(TEXT("bGpuMultiRow is set"), FMr::SetMultiRow(Cfg, true)) || !PG::ConfigurePooled(*this, Rig, Cfg)) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: K = %d plans ahead of a result"), Cfg.K), Cfg.K >= 2)) { return false; }
	FSuperSLMGpuAdapterHandle Adapter;
	FString MapError;
	if (!TestTrue(*FString::Printf(TEXT("MapAdapter (%s)"), *MapError), Gpu.MapAdapter(AdapterPath, *Rig.Model, Adapter, MapError))) { return false; }

	// The oracle: a fresh sequence on the base adapter.
	FSuperSLMGpuSequence Fresh;
	PG::FGpuGeneration FreshGen;
	if (!PG::Vend(*this, Gpu, Fresh) || !PG::Generate(*this, Gpu, Fresh, Prompt, 8, FreshGen)) { return false; }
	PG::ReturnAndDrain(Gpu, Fresh);
	bool bOk = TestEqual(TEXT("the fresh run completes"), (uint8)FreshGen.Phase, (uint8)ESuperSLMSequencePhase::Complete);

	// The faulted generation.
	FSuperSLMGpuSequence Seq;
	if (!PG::Vend(*this, Gpu, Seq)) { return false; }
	Gpu.RequestAdapterSwap(Seq, Adapter);
	FMr::StartRecording(Gpu);
	if (!TestTrue(TEXT("the next embed_rows(PROMPT) is refused"), FMr::ArmRowsFault(Gpu, EFault::EmbedRowsPrompt, SslmGpuStatus::SSLM_SEQUENCE_REJECTED))) { return false; }
	PG::FGpuGeneration First;
	if (!PG::Generate(*this, Gpu, Seq, Prompt, 8, First)) { return false; }
	bOk &= TestEqual(TEXT("the refused chunk faults the generation"), (uint8)First.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
	const TArray<FAct> FirstActs = ActionsOf(Gpu, Seq.Id);
	int32 PromptSlices = 0;
	for (const FAct& A : FirstActs) { PromptSlices += A.Kind == EKind::PromptRows ? 1 : 0; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the planner queued the chunk's next slices before the fault was applied (%d prompt-row actions)"), PromptSlices),
		FirstActs.Num() > 0 && FirstActs[0].bOpenedChunk && PromptSlices >= 2))
	{
		PG::ReturnAndDrain(Gpu, Seq);
		return false;
	}

	// Reset, back to base, begin again.
	if (!PG::ResetAndWait(*this, Gpu, Seq)) { PG::ReturnAndDrain(Gpu, Seq); return false; }
	Gpu.RequestAdapterSwap(Seq, FSuperSLMGpuAdapterHandle());
	FMr::StartRecording(Gpu);
	PG::FGpuGeneration Second;
	if (!PG::Generate(*this, Gpu, Seq, Prompt, 8, Second)) { PG::ReturnAndDrain(Gpu, Seq); return false; }
	bOk &= TestEqual(TEXT("the second generation completes"), (uint8)Second.Phase, (uint8)ESuperSLMSequencePhase::Complete);
	bOk &= TestEqual(TEXT("the second generation has no fault"), (uint8)Second.Fault, (uint8)ESuperSLMGpuFaultReason::None);
	bOk &= TestEqual(TEXT("the second generation equals the fresh run"), Second.Tokens, FreshGen.Tokens);
	const TArray<FAct> SecondActs = ActionsOf(Gpu, Seq.Id);
	bOk &= TestTrue(TEXT("the second generation's first action opens a chunk or is one-row work (no stale slice)"),
		SecondActs.Num() > 0 && (SecondActs[0].Kind == EKind::OneRow || SecondActs[0].bOpenedChunk));
	for (const FAct& A : SecondActs)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("action %d passes the base adapter"), A.ActionIndex), A.AdapterId, 0);
	}
	bOk &= TestFalse(TEXT("no chunk is open at the end"), FMr::IsChunkOpen(Gpu, Seq));
	bOk &= TestEqual(TEXT("the sequence's active adapter is the base"), static_cast<int64>(Gpu.GetActiveAdapter(Seq).Id), static_cast<int64>(0));
	// With the sequence still held, nothing uses the adapter: the unmap goes through.
	Gpu.UnmapAdapter(Adapter);
	bOk &= TestFalse(TEXT("the old adapter unmaps (no stale chunk holds it)"), FMr::IsAdapterMapped(Gpu, Adapter));
	PG::ReturnAndDrain(Gpu, Seq);
	return bOk;
}

// R42.S1 (ruling 42 S1, 42a; re-points P2.Failure and test_gpu_status_map's healthy failures, P2H.6, retired under
// ruling 40). OneCall, Exact, multi-row on, the draft table seeded: the first Exact open fails, at forced_tokens or at
// embed_rows(EXACT_DRAFT), with SSLM_GPU_ALLOCATION_FAILED or SSLM_SEQUENCE_REJECTED. The generation is not faulted:
// its ending equals the one-row twin's and its tokens equal the CPU's Off run. That chunk fell back (one open failure
// counted) and did one Off token of device work; the steps after it still verify drafts by rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42S1Test, "SuperSLM.MultiRow.MR6.R42_S1.ExactOpenFailureRunsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42S1Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	if (!MakeRequest(*this, C.Rig, kExact, kMaxNew, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	FGpuRun Twin;
	TArray<FAct> TwinActs;
	if (!C.Reconfigure(*this, EPath::WholeToken, false) || !RunMr(*this, C.Rig, EPath::WholeToken, Req, Twin, TwinActs)) { return false; }
	if (!C.Reconfigure(*this, EPath::WholeToken, true)) { return false; }
	struct FInj { EFault Point; SslmGpuStatus Status; const TCHAR* Name; };
	const FInj Injections[] = {
		{ EFault::ForcedTokens, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED, TEXT("forced_tokens ALLOCATION_FAILED") },
		{ EFault::ForcedTokens, SslmGpuStatus::SSLM_SEQUENCE_REJECTED, TEXT("forced_tokens SEQUENCE_REJECTED") },
		{ EFault::EmbedRowsExactDraft, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED, TEXT("embed_rows(EXACT_DRAFT) ALLOCATION_FAILED") },
		{ EFault::EmbedRowsExactDraft, SslmGpuStatus::SSLM_SEQUENCE_REJECTED, TEXT("embed_rows(EXACT_DRAFT) SEQUENCE_REJECTED") },
	};
	bool bOk = true;
	for (const FInj& I : Injections)
	{
		const FString Arm = I.Name;
		if (!TestTrue(*(Arm + TEXT(": armed")), FMr::ArmRowsFault(*C.Rig.Gpu, I.Point, I.Status))) { return false; }
		FGpuRun Run;
		TArray<FAct> Acts;
		if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Run, Acts)) { return false; }
		bOk &= SameGpuEnding(*this, Run, Twin, Arm + TEXT(": the ending equals the one-row twin's (no fault)"));
		bOk &= TestEqual(*(Arm + TEXT(": no fault")), (uint8)Run.Fault, (uint8)Twin.Fault);
		bOk &= TestEqual(*(Arm + TEXT(": tokens equal the CPU's Off run")), Run.Tokens, Cpu.Tokens);
		const TArray<TArray<FAct>> Chunks = ExactChunksOf(Acts);
		if (!TestTrue(*(Arm + TEXT(": an Exact chunk opened")), Chunks.Num() > 0)) { continue; }
		bOk &= TestTrue(*(Arm + TEXT(": the failed open's chunk fell back")), FellBack(Chunks[0]));
		int32 FellBackCount = 0;
		for (int32 K = 0; K < Chunks.Num(); ++K)
		{
			if (!FellBack(Chunks[K])) { continue; }
			++FellBackCount;
			bOk &= RequireOneOffTokenOfWork(*this, Chunks[K], static_cast<int32>(SuperSLML2S2Fixtures::AExNumHiddenLayers), FString::Printf(TEXT("%s, chunk %d"), *Arm, K));
		}
		int32 NoDraft = -1, OpenFailed = -1;
		bOk &= TestTrue(*(Arm + TEXT(": the fallback counts are readable")), FMr::GetExactFallbacks(*C.Rig.Gpu, NoDraft, OpenFailed));
		bOk &= TestEqual(*(Arm + TEXT(": one open failure counted")), OpenFailed, 1);
		bOk &= TestEqual(*(Arm + TEXT(": the counts match the record's fallbacks")), NoDraft + OpenFailed, FellBackCount);
		bOk &= RequireExactRows(*this, Acts, Arm + TEXT(", after the failed step"));
	}
	return bOk;
}

// R42.S2 (ruling 42 S2 with 42a). OneCall, Exact, multi-row on, no draft table loaded: every step whose forced chain is
// empty has no draft. Tokens equal the CPU's Off run and the ending equals the one-row twin's; each chunk that fell
// back did one Off token of device work (the delivery tick is not graded, 42a); the no-draft count matches the record
// and no open failed. Then the same run is stopped between a fallback chunk's open and its close and saved: the save
// abandons it, the blob restores, and the joined tokens and final save equal the unsaved run's. Ruling 53: the request is
// the first population prompt that meets an empty forced chain (FindNoDraftRequest), ending inside its schema. Ruling
// 56 (6): when none does, the first population prompt's request with ArmNoDraftStep() before each multi-row run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42S2Test, "SuperSLM.MultiRow.MR6.R42_S2.NoDraftStepIsOneOffToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42S2Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	bool bArm = false;
	if (!FindNoDraftRequest(*this, C, Req, bArm) || !RunCpu(*this, C.Rig, Req, kOff, Cpu)) { return false; }
	FGpuRun Twin;
	TArray<FAct> TwinActs;
	if (!C.Reconfigure(*this, EPath::WholeToken, false) || !RunMr(*this, C.Rig, EPath::WholeToken, Req, Twin, TwinActs)) { return false; }
	if (!C.Reconfigure(*this, EPath::WholeToken, true)) { return false; }
	FGpuRun Run;
	TArray<FAct> Acts;
	if (!ArmNoDraftIf(*this, C, bArm, TEXT("the run")) || !RunMr(*this, C.Rig, EPath::WholeToken, Req, Run, Acts, true)) { return false; }
	int32 NoDraft = -1, OpenFailed = -1;
	bool bOk = TestTrue(TEXT("the fallback counts are readable"), FMr::GetExactFallbacks(*C.Rig.Gpu, NoDraft, OpenFailed));
	TArray<uint8> UnsavedFinal;
	bOk &= Save(*this, C.Rig, Run.Sequence, UnsavedFinal);
	ReturnAndSettle(C.Rig, Run.Sequence);
	bOk &= TestEqual(TEXT("tokens equal the CPU's Off run"), Run.Tokens, Cpu.Tokens);
	bOk &= SameGpuEnding(*this, Run, Twin, TEXT("the ending equals the one-row twin's"));
	const TArray<TArray<FAct>> Chunks = ExactChunksOf(Acts);
	int32 FellBackCount = 0, SaveAt = INDEX_NONE;
	for (int32 K = 0; K < Chunks.Num(); ++K)
	{
		if (!FellBack(Chunks[K])) { continue; }
		++FellBackCount;
		bOk &= RequireOneOffTokenOfWork(*this, Chunks[K], static_cast<int32>(SuperSLML2S2Fixtures::AExNumHiddenLayers), FString::Printf(TEXT("chunk %d"), K));
		if (SaveAt == INDEX_NONE && Chunks[K].Num() >= 2) { SaveAt = K; }
	}
	if (!TestTrue(*FString::Printf(TEXT("precondition: a step had no draft (%d fallbacks of %d Exact chunks)"), FellBackCount, Chunks.Num()), FellBackCount > 0)) { return false; }
	bOk &= TestEqual(TEXT("the no-draft count matches the record"), NoDraft, FellBackCount);
	bOk &= TestEqual(TEXT("no open failed"), OpenFailed, 0);

	// A save between a fallback chunk's open and its close.
	if (!TestTrue(TEXT("precondition: a fallback chunk spans two ticks or more"), SaveAt != INDEX_NONE)) { return false; }
	FGpuRun Before;
	TArray<FAct> BeforeActs;
	auto OpensSoFar = [&]()
	{
		int32 N = 0;
		for (const FAct& A : ActionsOf(*C.Rig.Gpu, Before.Sequence.Id)) { N += A.Kind == EKind::ExactRows && A.bOpenedChunk ? 1 : 0; }
		return N;
	};
	if (!ArmNoDraftIf(*this, C, bArm, TEXT("the saved run")) || !RunMr(*this, C.Rig, EPath::WholeToken, Req, Before, BeforeActs, true,
		[&]() { return OpensSoFar() >= SaveAt + 1 && FMr::IsChunkOpen(*C.Rig.Gpu, Before.Sequence); }))
	{
		return false;
	}
	if (!TestTrue(TEXT("the run stopped inside the fallback chunk"), OpensSoFar() == SaveAt + 1 && FMr::IsChunkOpen(*C.Rig.Gpu, Before.Sequence) && !IsEnded(*C.Rig.Gpu, Before.Sequence)))
	{
		ReturnAndSettle(C.Rig, Before.Sequence);
		return false;
	}
	TArray<uint8> Mid;
	if (!Save(*this, C.Rig, Before.Sequence, Mid)) { ReturnAndSettle(C.Rig, Before.Sequence); return false; }
	int32 Abandons = 0, AbandonAt = INDEX_NONE;
	const TArray<FAct> SavedActs = ActionsOf(*C.Rig.Gpu, Before.Sequence.Id);
	for (int32 I = 0; I < SavedActs.Num(); ++I)
	{
		if (SavedActs[I].bAbandonedChunk) { ++Abandons; if (AbandonAt == INDEX_NONE) { AbandonAt = I; } }
	}
	bOk &= TestEqual(TEXT("the save abandoned the fallback chunk (once)"), Abandons, 1);
	// Rulings 53, 59d point 3: read from the record, as 9_4 does -- the generation continues in the tick the save resolves,
	// so a live IsChunkOpen() may already see the next chunk. The abandoned chunk is never continued: the first slice with
	// device work after the abandon opens a new chunk or is one-row work.
	bool bFollowed = false;
	for (int32 I = AbandonAt + 1; AbandonAt != INDEX_NONE && I < SavedActs.Num(); ++I)
	{
		const FAct& A = SavedActs[I];
		if (A.Layers <= 0) { continue; }
		bFollowed = true;
		bOk &= TestTrue(*FString::Printf(TEXT("action %d: after the abandon, the first slice with device work opens a new chunk or is one-row work (no slice continues the abandoned chunk)"), A.ActionIndex),
			A.Kind == EKind::OneRow || A.bOpenedChunk);
		break;
	}
	bOk &= TestTrue(TEXT("precondition: a slice with device work followed the abandon in the saved run's record (59g: the check is never vacuous)"), bFollowed);
	const TArray<int32> TokensBefore = C.Rig.Gpu->GetGeneratedTokens(Before.Sequence);
	ReturnAndSettle(C.Rig, Before.Sequence);
	FGpuRun After;
	if (!TestEqual(TEXT("the mid-chunk blob restores"), (uint8)Restore(C.Rig, Mid, After.Sequence), (uint8)ESuperSLMRestoreResult::Success)) { return false; }
	bOk &= TestTrue(TEXT("the restored run ends"), Finish(C.Rig, After));
	TArray<int32> Joined = TokensBefore;
	Joined.Append(After.Tokens);
	bOk &= TestEqual(TEXT("saved + continued tokens equal the unsaved run's"), Joined, Run.Tokens);
	TArray<uint8> AfterFinal;
	bOk &= Save(*this, C.Rig, After.Sequence, AfterFinal);
	bOk &= RequireSameLiveSave(*this, C.Rig, AfterFinal, UnsavedFinal, TEXT("the final save equals the unsaved run's"));
	ReturnAndSettle(C.Rig, After.Sequence);
	return bOk;
}

// R42.S5 (ruling 42 S5; ruling 41). OneCall, Exact, multi-row on, the draft table seeded from the request's Off run.
// GPU Exact drafts exactly as CPU Exact does: the learned table drafts only after a non-empty forced chain
// (SuperSLMSubsystem.cpp's ForcedChainLength > 0). Every Exact open's forced-chain count is answered and its drafts are
// at least the chain's; an open whose chain was empty drafted nothing and fell back; and the table reached the GPU (an
// open drafted past a non-empty chain). Tokens equal the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR42S5Test, "SuperSLM.MultiRow.MR6.R42_S5.EmptyChainDraftsAsCpuExact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR42S5Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	if (!MakeRequest(*this, C.Rig, kExact, kMaxNew, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	if (!C.Reconfigure(*this, EPath::WholeToken, true)) { return false; }
	FGpuRun Run;
	TArray<FAct> Acts;
	if (!RunMr(*this, C.Rig, EPath::WholeToken, Req, Run, Acts)) { return false; }
	bool bOk = TestEqual(TEXT("tokens equal the CPU's Off run"), Run.Tokens, Cpu.Tokens);
	// Ruling 53 (finding 6, R42_S5 lead): diagnosis only, the verdict below is unchanged. For each open that recorded
	// no chain (-1), what the run says about it: whether its chunk fell back, the run's open failures, the Layer-1
	// statuses a forced read can return, and whether the CPU side had a draft table and a known walk (run level: the seam
	// has no per-open reading of either).
	{
		int32 NoDraft = -1, OpenFailed = -1;
		FMr::GetExactFallbacks(*C.Rig.Gpu, NoDraft, OpenFailed);
		const FString RunLevel = FString::Printf(TEXT("run: no-draft fallbacks %d, open failures %d; Layer-1 statuses since recording: FORCED_TABLE_ABSENT %d, "
			"ALLOCATION_FAILED %d, SEQUENCE_REJECTED %d, ROWS_CHUNK_OPEN %d, CONTEXT_CAP_EXCEEDED %d; CPU draft table %s (%d states), CPU walk table ready %s"),
			NoDraft, OpenFailed,
			FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_GPU_FORCED_TABLE_ABSENT),
			FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED),
			FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_SEQUENCE_REJECTED),
			FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_GPU_ROWS_CHUNK_OPEN),
			FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_GPU_CONTEXT_CAP_EXCEEDED),
			FSuperSLMSchemaJumpTestAccess::HasDraftTable(*C.Rig.Cpu.Cpu, C.Rig.Cpu.Schema) ? TEXT("loaded") : TEXT("absent"),
			FSuperSLMSchemaJumpTestAccess::GetDraftTableStateCount(*C.Rig.Cpu.Cpu, C.Rig.Cpu.Schema),
			FSuperSLMSchemaJumpReviewTestAccess::IsSchemaWalkReady(*C.Rig.Cpu.Cpu) ? TEXT("yes") : TEXT("no"));
		AddInfo(TEXT("R42_S5 diagnosis: ") + RunLevel);
		for (const TArray<FAct>& Chunk : ExactChunksOf(Acts))
		{
			const FAct& Open = Chunk[0];
			if (Open.ForcedChainCount >= 0 && Open.DraftCount >= 0) { continue; }
			FString Slices;
			for (const FAct& A : Chunk)
			{
				Slices += FString::Printf(TEXT(" [action %d tick %lld rows %d layers %d opened %d closed %d abandoned %d fellback %d delivered %d]"), A.ActionIndex, A.Tick, A.Rows,
					A.Layers, A.bOpenedChunk ? 1 : 0, A.bClosedChunk ? 1 : 0, A.bAbandonedChunk ? 1 : 0, A.bFellBack ? 1 : 0, A.DeliveredTokens.Num());
			}
			AddInfo(FString::Printf(TEXT("R42_S5 diagnosis: the Exact open at action %d recorded chain %d, drafts %d; FellBack(chunk) %s; its slices:%s"),
				Open.ActionIndex, Open.ForcedChainCount, Open.DraftCount, FellBack(Chunk) ? TEXT("yes") : TEXT("no"), *Slices));
		}
	}
	// Rulings 59e point 1, 59f (erratum E-15): GPU Exact proposes CPU Exact's draft -- the learned table's, from the walk
	// state after the pending token -- with the engine's chain as the gate and the fallback. At every open: forced_tokens
	// answered, a non-empty chain proposed a draft, the proposal is CPU Exact's draft (or the chain when the table drafts
	// nothing), and the table reached the GPU.
	int32 NonEmpty = 0, CpuDrafted = 0, TableReached = 0;
	auto IsPrefix = [](const TArray<int32>& Short, const TArray<int32>& Long)
	{
		if (Short.Num() > Long.Num()) { return false; }
		for (int32 I = 0; I < Short.Num(); ++I) { if (Short[I] != Long[I]) { return false; } }
		return true;
	};
	for (const TArray<FAct>& Chunk : ExactChunksOf(Acts))
	{
		const FAct& Open = Chunk[0];
		// Ruling 42 M2: a chunk planned past the generation's last token is dropped at the completion and never delivers;
		// what its open read is not the run's (its record keeps no chain).
		if (Open.ForcedChainCount < 0 && Chunk.Last().DeliveredTokens.Num() == 0) { continue; } // never delivered: no reading of the run's
		const FString At = FString::Printf(TEXT("the Exact open at action %d (chain %d, drafts %d)"), Open.ActionIndex, Open.ForcedChainCount, Open.DraftCount);
		// Ruling 59f: the proposal is CPU Exact's draft when the table drafts, else the chain -- so a non-empty chain proposes
		// at least one draft either way.
		bOk &= TestTrue(*(At + TEXT(": forced_tokens answered, and a non-empty chain proposed a draft (59f)")), Open.ForcedChainCount >= 0 && (Open.ForcedChainCount == 0 || Open.DraftCount >= 1));
		TableReached += Open.bDraftTableReached ? 1 : 0;
		if (Open.ForcedChainCount > 0)
		{
			++NonEmpty;
			CpuDrafted += Open.CpuDraftTokens.Num() > 0 ? 1 : 0;
			{
				// Ruling 59f point 2's confirmation: both lists' text, and whether one's bytes are a prefix of the other's.
				FString GpuText, CpuText;
				USuperSLMBlueprintLibrary::Detokenize(C.Rig.Cpu.Model, Open.ProposedTokens, GpuText);
				USuperSLMBlueprintLibrary::Detokenize(C.Rig.Cpu.Model, Open.CpuDraftTokens, CpuText);
				const bool bSameText = GpuText.StartsWith(CpuText, ESearchCase::CaseSensitive) || CpuText.StartsWith(GpuText, ESearchCase::CaseSensitive);
				AddInfo(FString::Printf(TEXT("R42_S5 59f: open %d chain %d: GPU proposal [%s] = \"%s\"; CPU draft [%s] = \"%s\"; same text (one a prefix of the other): %s"),
					Open.ActionIndex, Open.ForcedChainCount, *FString::JoinBy(Open.ProposedTokens, TEXT(","), [](int32 V) { return FString::FromInt(V); }), *GpuText,
					*FString::JoinBy(Open.CpuDraftTokens, TEXT(","), [](int32 V) { return FString::FromInt(V); }), *CpuText, bSameText ? TEXT("yes") : TEXT("no")));
			}
			int32 Same = 0;
			while (Same < Open.CpuDraftTokens.Num() && Same < Open.ProposedTokens.Num() && Open.CpuDraftTokens[Same] == Open.ProposedTokens[Same]) { ++Same; }
			// Ruling 59f point 2: when the table drafts, the GPU proposes exactly CPU Exact's draft; otherwise the chain.
			if (Open.CpuDraftTokens.Num() > 0)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("%s: the GPU proposed CPU Exact's draft from the same walk state (%d / %d ids; the first %d agree)"), *At,
					Open.ProposedTokens.Num(), Open.CpuDraftTokens.Num(), Same), Open.ProposedTokens == Open.CpuDraftTokens);
			}
			else
			{
				bOk &= TestEqual(*FString::Printf(TEXT("%s: with no table draft, the GPU proposed the chain"), *At), Open.ProposedTokens.Num(), Open.ForcedChainCount);
			}
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: an Exact open read a non-empty chain (%d)"), NonEmpty), NonEmpty > 0);
	bOk &= TestTrue(*FString::Printf(TEXT("the learned table reached the GPU at the opens (%d)"), TableReached), TableReached > 0);
	bOk &= TestTrue(*FString::Printf(TEXT("CPU Exact drafted at a non-empty chain's walk state (the prefix check is not vacuous: %d of %d)"), CpuDrafted, NonEmpty), CpuDrafted > 0);

	// Ruling 59e point 2: the empty-chain leg on A-EX's prompt_result schema (a free string: the state after a content token
	// is unforced, so an open there reads an empty chain, drafts nothing, as CPU Exact, and falls back; its key text still
	// gives non-empty chains). Tokens equal the same schema's Off run on the GPU (Exact never changes tokens, C17).
	{
		FString Error;
		const FSuperSLMGpuSchemaHandle Demo = C.Rig.Schema;
		FSuperSLMGpuSchemaHandle PromptResult;
		if (!TestTrue(*FString::Printf(TEXT("the prompt_result schema is found (%s)"), *Error),
			FSuperSLMGpuSchemaLookup::LookupByName(*C.Rig.Cpu.Model, TEXT("prompt_result"), PromptResult, Error))) { return false; }
		C.Rig.Schema = PromptResult;
		FSuperSLMGenerationRequest FreeReq = Req;
		FreeReq.SchemaJumpForward = kOff;
		FGpuRun OffRun, ExactRun;
		TArray<FAct> OffActs, ExactActs;
		const bool bRan = RunMr(*this, C.Rig, EPath::WholeToken, FreeReq, OffRun, OffActs) &&
			[&]() { FreeReq.SchemaJumpForward = kExact; return RunMr(*this, C.Rig, EPath::WholeToken, FreeReq, ExactRun, ExactActs); }();
		C.Rig.Schema = Demo;
		if (!TestTrue(TEXT("prompt_result: the Off and Exact runs ran"), bRan)) { return false; }
		bOk &= TestEqual(TEXT("prompt_result: Exact's tokens equal Off's"), ExactRun.Tokens, OffRun.Tokens);
		int32 EmptyChains = 0, NonEmptyHere = 0;
		for (const TArray<FAct>& Chunk : ExactChunksOf(ExactActs))
		{
			const FAct& Open = Chunk[0];
			if (Open.ForcedChainCount < 0 && Chunk.Last().DeliveredTokens.Num() == 0) { continue; } // never delivered: no reading of the run's
			const FString At = FString::Printf(TEXT("prompt_result, the Exact open at action %d (chain %d, drafts %d)"), Open.ActionIndex, Open.ForcedChainCount, Open.DraftCount);
			// Ruling 59f: the proposal is CPU Exact's draft when the table drafts, else the chain -- so a non-empty chain proposes
		// at least one draft either way.
		bOk &= TestTrue(*(At + TEXT(": forced_tokens answered, and a non-empty chain proposed a draft (59f)")), Open.ForcedChainCount >= 0 && (Open.ForcedChainCount == 0 || Open.DraftCount >= 1));
			if (Open.ForcedChainCount == 0)
			{
				++EmptyChains;
				bOk &= TestEqual(*(At + TEXT(": an empty chain drafts nothing from the table, as CPU Exact")), Open.DraftCount, 0);
				bOk &= TestEqual(*(At + TEXT(": and CPU Exact drafts nothing there either")), Open.CpuDraftTokens.Num(), 0);
				bOk &= TestTrue(*(At + TEXT(": so the step fell back")), FellBack(Chunk));
			}
			else
			{
				++NonEmptyHere;
			}
		}
		bOk &= TestTrue(*FString::Printf(TEXT("precondition: prompt_result: an Exact step met an empty forced chain (%d)"), EmptyChains), EmptyChains > 0);
		AddInfo(FString::Printf(TEXT("R42_S5 prompt_result leg: %d empty-chain opens, %d non-empty"), EmptyChains, NonEmptyHere));
	}	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ruling 43 (the MR6 re-check's N1 and N2, code-review/poirot-mr6.md "Re-check"). Both are below one row (B < C(1)).
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
	int64 TotalRowLayers(const TArray<FAct>& Acts)
	{
		int64 Sum = 0;
		for (const FAct& A : Acts) { Sum += static_cast<int64>(FMath::Max(A.Rows, 1)) * FMath::Max(A.Layers, 0); }
		return Sum;
	}

	// Ruling 42a with ruling 43 N1: a fallback chunk does no device work before its close, and from the close on the
	// step is Off's one-row token as Off runs it at this B: one-row actions of at most PerTick layers (l(1) below one
	// row, L at or above it), summing to exactly L, the last of which delivers the step's id (none after a dead end).
	bool RequireFallbackIsOffsToken(FAutomationTestBase& T, const TArray<FAct>& Acts, const TArray<FAct>& Chunk, int32 L, int64 PerTick, const FString& What)
	{
		bool bOk = true;
		for (int32 I = 0; I + 1 < Chunk.Num(); ++I)
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s, action %d: no device work before the close"), *What, Chunk[I].ActionIndex), Chunk[I].Layers, 0);
		}
		int32 Start = INDEX_NONE;
		for (int32 I = 0; I < Acts.Num(); ++I) { if (Acts[I].ActionIndex == Chunk.Last().ActionIndex) { Start = I; break; } }
		if (!T.TestTrue(*(What + TEXT(": the close is in the record")), Start != INDEX_NONE)) { return false; }
		int32 Done = 0;
		const FAct* Last = nullptr;
		for (int32 I = Start; I < Acts.Num() && Done < L; ++I)
		{
			const FAct& A = Acts[I];
			if (A.Layers <= 0) { continue; }
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, action %d: one row, at most %lld layers, no chunk opened (rows %d, layers %d)"), *What, A.ActionIndex, PerTick, A.Rows, A.Layers),
				A.Rows == 1 && A.Layers <= PerTick && !A.bOpenedChunk);
			Done += A.Layers;
			Last = &A;
		}
		bOk &= T.TestEqual(*(What + TEXT(": the step's device work is one Off token (L layers of one row)")), Done, L);
		bOk &= T.TestTrue(*(What + TEXT(": the step's last slice delivers at most its one id")), Last != nullptr && Last->DeliveredTokens.Num() <= 1);
		return bOk;
	}
}

// R43.N1 (re-check N1; ruling 43: 42a's intent governs below one row). OneCall, Exact, multi-row on, the vendored
// measured table, B = 2 ms and 4 ms (below one row; the chooser still finds an M_exact). Preconditions: B < C(1) and the
// table is measured; Off's run at the same B gives the CPU's tokens; Exact chunks open (Exact is on at this B). Two
// fallback arms: no draft table (steps with an empty forced chain fall back), and every Exact open failing at
// forced_tokens (re-armed every tick, so every step falls back). In both: every recorded tick is within B; tokens and
// the ending equal Off's; each fallback is Off's one-row token at this B (l(1) slices summing to L). With every open
// failing, every chunk fell back, the open-failure count is the chunk count, and the run's total device work (rows x
// layers over every action) equals Off's run's. Ruling 53: the request is FindNoDraftRequest's (an empty forced chain
// occurs), ending inside its schema; ruling 56 (6): or, when none does, ArmNoDraftStep() before each no-table run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR43N1Test, "SuperSLM.MultiRow.MR6.R43_N1.FallbackBelowOneRowWithinBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR43N1Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	if (!TestTrue(TEXT("precondition: the vendored cost table is measured"), superslm_gpu::kGpuRowsCostTableMeasured && superslm_gpu::kGpuRowsLogitsRatioMeasured)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	bool bArm = false;
	if (!FindNoDraftRequest(*this, C, Req, bArm) || !RunCpu(*this, C.Rig, Req, kOff, Cpu)) { return false; }
	FSuperSLMGenerationRequest OffReq = Req;
	OffReq.SchemaJumpForward = kOff;
	const int32 L = static_cast<int32>(SuperSLML2S2Fixtures::AExNumHiddenLayers);
	bool bOk = true;
	// The forced arms run after both budgets' no-table arms: an armed fault left over at a run's end must not reach a
	// no-table run.
	for (const bool bForced : { false, true })
	{
		for (const double B : { 2.0, 4.0 })
		{
			const FString Arm = FString::Printf(TEXT("B = %.0f ms, %s"), B, bForced ? TEXT("every Exact open fails") : TEXT("no draft table"));
			FSuperSLMGpuRuntimeConfig Cfg;
			if (!MrConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
			Cfg.SchemaJumpGpuFrameBudgetMs = B;
			if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
			const FTickPrices P = PricesOf(Cfg);
			const int64 L1 = OneRowLayers(P);
			if (!TestTrue(*FString::Printf(TEXT("%s: precondition: below one row (Room %lld us < C(1) %lld us; l(1) = %lld)"), *Arm, P.RoomUs, P.C1, L1), P.RoomUs < P.C1 && L1 < L)) { return false; }
			FGpuRun OffRun;
			TArray<FAct> OffActs;
			if (!RunMr(*this, C.Rig, EPath::WholeToken, OffReq, OffRun, OffActs)) { return false; }
			if (!TestEqual(*(Arm + TEXT(": precondition: Off at this B gives the CPU's tokens")), OffRun.Tokens, Cpu.Tokens)) { return false; }
			FGpuRun Run;
			TArray<FAct> Acts;
			TFunction<bool()> Rearm = nullptr;
			if (bForced)
			{
				if (!TestTrue(*(Arm + TEXT(": armed")), FMr::ArmRowsFault(*C.Rig.Gpu, EFault::ForcedTokens, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED))) { return false; }
				Rearm = [&C]() { FMr::ArmRowsFault(*C.Rig.Gpu, EFault::ForcedTokens, SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED); return false; };
			}
			if ((!bForced && !ArmNoDraftIf(*this, C, bArm, Arm)) || !RunMr(*this, C.Rig, EPath::WholeToken, Req, Run, Acts, false, Rearm)) { return false; }
			const TArray<TArray<FAct>> Chunks = ExactChunksOf(Acts);
			if (!TestTrue(*FString::Printf(TEXT("%s: precondition: Exact is on at this B (%d Exact chunks)"), *Arm, Chunks.Num()), Chunks.Num() > 0)) { return false; }
			bOk &= RequireTicksWithinBudget(*this, Acts, P, Arm);
			bOk &= TestEqual(*(Arm + TEXT(": tokens equal Off's")), Run.Tokens, Cpu.Tokens);
			bOk &= SameGpuEnding(*this, Run, OffRun, Arm + TEXT(": the ending equals Off's"));
			bOk &= TestEqual(*(Arm + TEXT(": no fault")), (uint8)Run.Fault, (uint8)OffRun.Fault);
			int32 FellBackCount = 0;
			for (int32 K = 0; K < Chunks.Num(); ++K)
			{
				if (!FellBack(Chunks[K])) { continue; }
				++FellBackCount;
				bOk &= RequireFallbackIsOffsToken(*this, Acts, Chunks[K], L, L1, FString::Printf(TEXT("%s, chunk %d"), *Arm, K));
			}
			int32 NoDraft = -1, OpenFailed = -1;
			bOk &= TestTrue(*(Arm + TEXT(": the fallback counts are readable")), FMr::GetExactFallbacks(*C.Rig.Gpu, NoDraft, OpenFailed));
			if (bForced)
			{
				bOk &= TestEqual(*(Arm + TEXT(": every Exact chunk fell back")), FellBackCount, Chunks.Num());
				// Ruling 59.1's re-check: an Exact open with one token left of MaxNew has no room for a draft, so it never reads the
				// forced chain the fault is armed on and falls back as ruling 42 S2's no-draft step instead. Every chunk is one of the
				// two, and only the generation's last token can be the second.
				bOk &= TestEqual(*FString::Printf(TEXT("%s: every chunk counted as an open failure, or (its last token, no room for a draft) a no-draft fallback (%d open, %d no-draft)"), *Arm, OpenFailed, NoDraft),
					OpenFailed + NoDraft, Chunks.Num());
				bOk &= TestTrue(*FString::Printf(TEXT("%s: at most one no-draft fallback, the last token's (%d)"), *Arm, NoDraft), NoDraft >= 0 && NoDraft <= 1);
				// 59g: the no-draft chunk is the generation's last token -- only there is there no room for a draft (one token left
				// of MaxNew), so a no-draft fallback means the run reached MaxNew.
				bOk &= TestTrue(*FString::Printf(TEXT("%s: a no-draft fallback only at the generation's last token (%d no-draft, %d of %d tokens)"), *Arm, NoDraft, Run.Tokens.Num(), Req.MaxNewTokens),
					NoDraft == 0 || Run.Tokens.Num() == Req.MaxNewTokens);
				bOk &= TestEqual(*FString::Printf(TEXT("%s: total device work equals Off's (row-layers)"), *Arm), TotalRowLayers(Acts), TotalRowLayers(OffActs));
			}
			else
			{
				if (!TestTrue(*FString::Printf(TEXT("%s: precondition: a step had no draft (%d fallbacks)"), *Arm, FellBackCount), FellBackCount > 0)) { return false; }
				bOk &= TestEqual(*(Arm + TEXT(": the no-draft count matches the record")), NoDraft, FellBackCount);
				bOk &= TestEqual(*(Arm + TEXT(": no open failed")), OpenFailed, 0);
			}
		}
	}
	return bOk;
}

// R43.N2 (re-check N2; ruling 43). Multi-row on, B = 2 ms (below one row), LayersPerTick 4: one OneCall and one Composed
// Off sequence begun together. Precondition: the OneCall sequence ran a sliced one-row action (fewer than L layers) in
// a tick inside the Composed sequence's life, so the two were live in the same tick below one row. Then: the engine
// never returned SSLM_BATCH_BUDGET_EXHAUSTED or SSLM_DISPATCH_BUDGET_TOO_SMALL; neither sequence faulted; every
// recorded tick (both sequences' actions together) is within B; each sequence's tokens equal the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR43N2Test, "SuperSLM.MultiRow.MR6.R43_N2.OneCallAndComposedBelowOneRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR43N2Test::RunTest(const FString& Parameters)
{
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	const EPath Paths[2] = { EPath::WholeToken, EPath::Sliced4 };
	FSuperSLMGenerationRequest Reqs[2];
	FRunResult Cpu[2];
	for (int32 I = 0; I < 2; ++I)
	{
		if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Reqs[I], SuperSLMSchemaJumpFixtures::PopulationPrompts()[I]) || !RunCpu(*this, C.Rig, Reqs[I], kOff, Cpu[I])) { return false; }
	}
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!MrConfig(*this, EPath::Sliced4, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	const FTickPrices P = PricesOf(Cfg);
	const int64 L1 = OneRowLayers(P);
	if (!TestTrue(*FString::Printf(TEXT("precondition: below one row (Room %lld us < C(1) %lld us; l(1) = %lld)"), P.RoomUs, P.C1, L1), P.RoomUs < P.C1 && L1 < P.L)) { return false; }
	FGpuRun Runs[2];
	for (int32 I = 0; I < 2; ++I)
	{
		if (!Vend(*this, C.Rig, Paths[I], Runs[I])) { return false; }
	}
	FMr::StartRecording(*C.Rig.Gpu);
	for (int32 I = 0; I < 2; ++I)
	{
		if (!TestTrue(*FString::Printf(TEXT("%s: RequestBeginGeneration"), PathName(Paths[I])), C.Rig.Gpu->RequestBeginGeneration(Runs[I].Sequence, Reqs[I]).IsValid())) { return false; }
	}
	const bool bEnded = Pump(*C.Rig.Gpu, [&]() { return IsEnded(*C.Rig.Gpu, Runs[0].Sequence) && IsEnded(*C.Rig.Gpu, Runs[1].Sequence); });
	for (FGpuRun& R : Runs) { Read(C.Rig, R); }
	const TArray<FAct> Acts = FMr::GetActions(*C.Rig.Gpu);
	const int32 BatchExhausted = FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_BATCH_BUDGET_EXHAUSTED);
	const int32 TooSmall = FMr::GetLayer1StatusCount(*C.Rig.Gpu, SslmGpuStatus::SSLM_DISPATCH_BUDGET_TOO_SMALL);
	for (const FGpuRun& R : Runs) { ReturnAndSettle(C.Rig, R.Sequence); }
	bool bOk = TestTrue(TEXT("both sequences end"), bEnded);
	int64 CompFirst = MAX_int64, CompLast = MIN_int64;
	for (const FAct& A : Acts)
	{
		if (A.SequenceId == Runs[1].Sequence.Id) { CompFirst = FMath::Min(CompFirst, A.Tick); CompLast = FMath::Max(CompLast, A.Tick); }
	}
	bool bShared = false;
	for (const FAct& A : Acts)
	{
		bShared |= A.SequenceId == Runs[0].Sequence.Id && A.Kind == EKind::OneRow && A.Layers > 0 && A.Layers < P.L && A.Tick >= CompFirst && A.Tick <= CompLast;
	}
	if (!TestTrue(TEXT("precondition: a sliced OneCall one-row action ran while the Composed sequence was live"), bShared)) { return false; }
	bOk &= TestEqual(TEXT("the engine never returned SSLM_BATCH_BUDGET_EXHAUSTED"), BatchExhausted, 0);
	bOk &= TestEqual(TEXT("the engine never returned SSLM_DISPATCH_BUDGET_TOO_SMALL"), TooSmall, 0);
	for (int32 I = 0; I < 2; ++I)
	{
		const FString What = PathName(Paths[I]);
		bOk &= TestNotEqual(*(What + TEXT(": not faulted")), (uint8)Runs[I].Phase, (uint8)ESuperSLMSequencePhase::Faulted);
		bOk &= TestEqual(*(What + TEXT(": no fault")), (uint8)Runs[I].Fault, (uint8)ESuperSLMGpuFaultReason::None);
		bOk &= TestEqual(*(What + TEXT(": tokens equal the CPU's Off run")), Runs[I].Tokens, Cpu[I].Tokens);
	}
	bOk &= RequireTicksWithinBudget(*this, Acts, P, TEXT("OneCall and Composed together at B = 2 ms"));
	return bOk;
}

// R45 (ruling 45, 45a; MB6 10.4 on the GPU clock). The per-tick GPU-busy seam (GetTickGpuBusyUs) is the sum of the
// tick's calls, never the last call's. The default B, two legs (ruling 56 (6)): multi-row on, one OneCall Exact
// sequence (drafts seeded), one OneCall Off and one Composed Off; multi-row off, two OneCall Off and three Composed Off
// sequences (LayersPerTick 4; two of 24 new tokens and one of 96, so the long one runs on solo once the others end).
// Each leg's sequences are begun together, and the device-resident logits head is active. Preconditions: the seam is built and returned
// records; across the run the calls include a OneCall step, a Composed solo slice, a batch call of two members or
// more, an Exact close, and (ruling 46a: a device-head run) a Finish; and some tick holds a batch call of two members
// or more beside two or more other calls. Then, for every record: GpuBusyUs equals the sum of its calls' counter
// deltas (1 us); Submissions equals the calls' submissions (a batch counts its members, a Finish none); a Finish has
// Members 0 and its delta equals its published time (1 us); UntimedSubmissions is 0, and on every Composed batch tick
// by name (ruling 45a); a single-call delta equals the time that call published (1 us); a batch call's delta is at
// least 1.25 x the
// time it published (its last member's: with two members or more a counter that kept only the last member's time is
// red); an Exact close's delta is at least its published time; a tick of two timed calls or more is at least its last
// call's published time plus half the other calls' deltas (the per-tick value is not the last call's). Every sequence's
// tokens equal the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR45TickGpuBusyTest, "SuperSLM.MultiRow.MR6.R45.TickGpuBusyIsTheSumOfItsCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR45TickGpuBusyTest::RunTest(const FString& Parameters)
{
	using ECall = ESuperSLMGpuTickCallKind;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 56 (6): two legs, each recorded on its own and graded together. Multi-row on: an Exact OneCall sequence
	// (drafts seeded), an Off OneCall and an Off Composed (Exact closes, OneCall steps, rows slices). Multi-row off
	// (P4.4's one-row scheduling, where Composed one-row slices batch): two Off OneCall and three Off Composed sequences
	// (LayersPerTick 4; two of 24 new tokens and one of 96, so the long one runs solo once the others end): batch calls
	// beside two or more other calls, solo slices, and the Composed finish (ruling 46a). The logits head is on the
	// device (IsDeviceHeadActive) in both.
	struct FLeg
	{
		bool bMulti;
		TArray<EPath> Paths;
		TArray<ESuperSLMSchemaJumpForward> Modes;
		TArray<int32> MaxNew;
	};
	const FLeg Legs[2] = {
		{ true, { EPath::WholeToken, EPath::WholeToken, EPath::Sliced4 }, { kExact, kOff, kOff }, { kMaxNew, kMr6MaxNew, kMr6MaxNew } },
		{ false, { EPath::WholeToken, EPath::WholeToken, EPath::Sliced4, EPath::Sliced4, EPath::Sliced4 }, { kOff, kOff, kOff, kOff, kOff },
			{ kMr6MaxNew, kMr6MaxNew, kMr6MaxNew, kMr6MaxNew, kMaxNew } },
	};
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	TArray<FAct> ExactActs;
	bool bOk = true;
	for (const FLeg& Leg : Legs)
	{
		const FString LegName = Leg.bMulti ? TEXT("multi-row on") : TEXT("multi-row off");
		const int32 N = Leg.Paths.Num();
		TArray<FSuperSLMGenerationRequest> Reqs;
		TArray<FRunResult> Cpu;
		Reqs.SetNum(N);
		Cpu.SetNum(N);
		const TArray<FString>& Prompts = SuperSLMSchemaJumpFixtures::PopulationPrompts();
		for (int32 I = 0; I < N; ++I)
		{
			const bool bMade = I == 0 ? MakeRequest(*this, C.Rig, Leg.Modes[I], Leg.MaxNew[I], Reqs[I])
				: MakeRequest(*this, C.Rig, Leg.Modes[I], Leg.MaxNew[I], Reqs[I], Prompts[(I - 1) % Prompts.Num()]);
			// Ruling 59e point 3: an Off sequence runs unbound (on the CPU and the GPU alike), so its finish takes the device head
			// (a schema-bound finish on this tied-embedding model scores on the host and submits nothing). Exact needs its schema.
			if (!bMade || !RunCpu(*this, C.Rig, Reqs[I], kOff, Cpu[I], Leg.Modes[I] != kExact)) { return false; }
		}
		if (Leg.Modes[0] == kExact && !SeedExactDrafts(*this, C.Rig, Reqs[0])) { return false; }
		{
			// Ruling 53: one slot per sequence. Rulings 59d point 6, 59e point 3: the multi-row-off leg gives the tick 12 layers
			// and caps each Composed sequence's slice at 4 (SetLayersPerSlice below), so its three Composed members, begun in the
			// same tick, advance together and end their tokens in the same tick: those ticks batch three members that each end
			// their token (59.1), beside the OneCall step and the finishes. At 4 layers per tick, which divides L, one member
			// took the whole tick and no batch call ever ran.
			FSuperSLMGpuRuntimeConfig LegCfg;
			if (!MrConfig(*this, EPath::Sliced4, Leg.bMulti, LegCfg, 0, N)) { return false; }
			if (!Leg.bMulti)
			{
				constexpr int32 kOffLegLayersPerTick = 12;
				const int32 LegL = SuperSLML2S2Fixtures::AExNumHiddenLayers;
				LegCfg.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(kOffLegLayersPerTick);
				LegCfg.K = FMath::Max3(LegL, N + 1, (N * LegL + kOffLegLayersPerTick - 1) / kOffLegLayersPerTick);
			}
			if (!ConfigureGpu(*this, C.Rig, LegCfg)) { return false; }
		}
		if (!TestTrue(*(LegName + TEXT(": precondition: the logits head is on the device (ruling 46a)")), C.Rig.Gpu->IsDeviceHeadActive())) { return false; }
		TArray<FGpuRun> Runs;
		Runs.SetNum(N);
		for (int32 I = 0; I < N; ++I)
		{
			if (!Vend(*this, C.Rig, Leg.Paths[I], Runs[I], Leg.Modes[I] != kExact)) { return false; } // ruling 59e point 3
			FString SliceError;
			if (!Leg.bMulti && Leg.Paths[I] == EPath::Sliced4 &&
				!TestTrue(*FString::Printf(TEXT("%s, sequence %d: 4 layers per slice (%s)"), *LegName, I, *SliceError), C.Rig.Gpu->SetLayersPerSlice(Runs[I].Sequence, 4, SliceError))) { return false; }
		}
		FMr::StartRecording(*C.Rig.Gpu);
		for (int32 I = 0; I < N; ++I)
		{
			if (!TestTrue(*FString::Printf(TEXT("%s, sequence %d (%s): RequestBeginGeneration"), *LegName, I, PathName(Leg.Paths[I])), C.Rig.Gpu->RequestBeginGeneration(Runs[I].Sequence, Reqs[I]).IsValid())) { return false; }
		}
		const bool bEnded = Pump(*C.Rig.Gpu, [&]()
		{
			for (const FGpuRun& R : Runs) { if (!IsEnded(*C.Rig.Gpu, R.Sequence)) { return false; } }
			return true;
		});
		for (FGpuRun& R : Runs) { Read(C.Rig, R); }
		TArray<FSuperSLMGpuTickGpuBusyRecord> LegRecords;
		const bool bBuilt = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, LegRecords);
		if (Leg.bMulti) { ExactActs = ActionsOf(*C.Rig.Gpu, Runs[0].Sequence.Id); }
		for (const FGpuRun& R : Runs) { ReturnAndSettle(C.Rig, R.Sequence); }
		bOk &= TestTrue(*(LegName + TEXT(": every sequence ends")), bEnded);
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: the per-tick GPU seam is built and recorded the run (%d records)"), *LegName, LegRecords.Num()), bBuilt && LegRecords.Num() > 0)) { return false; }
		Records.Append(LegRecords);
		for (int32 I = 0; I < N; ++I)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("%s, sequence %d (%s): tokens equal the CPU's Off run"), *LegName, I, PathName(Leg.Paths[I])), Runs[I].Tokens, Cpu[I].Tokens);
		}
	}
	if (!RequireExactRows(*this, ExactActs, TEXT("R45, the Exact sequence"))) { return false; }
	// Preconditions on the mix.
	int32 Seen[8] = {};
	bool bMixedBatchTick = false;
	for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
	{
		int32 Batches = 0;
		for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
		{
			++Seen[FMath::Clamp((int32)K.Kind, 0, 7)];
			Batches += K.Kind == ECall::Batch && K.Members >= 2 ? 1 : 0;
		}
		bMixedBatchTick |= Batches > 0 && R.Calls.Num() - Batches >= 2;
	}
	bool bMix = TestTrue(*FString::Printf(TEXT("precondition: a OneCall step ran (%d)"), Seen[(int32)ECall::OneCallStep]), Seen[(int32)ECall::OneCallStep] > 0);
	bMix &= TestTrue(*FString::Printf(TEXT("precondition: a Composed solo slice ran (%d)"), Seen[(int32)ECall::ComposedSolo]), Seen[(int32)ECall::ComposedSolo] > 0);
	bMix &= TestTrue(*FString::Printf(TEXT("precondition: a batch call ran (%d)"), Seen[(int32)ECall::Batch]), Seen[(int32)ECall::Batch] > 0);
	bMix &= TestTrue(*FString::Printf(TEXT("precondition: an Exact close ran (%d)"), Seen[(int32)ECall::ExactClose]), Seen[(int32)ECall::ExactClose] > 0);
	bMix &= TestTrue(*FString::Printf(TEXT("precondition: a device-head run made a Finish call (ruling 46a; %d)"), Seen[(int32)ECall::Finish]), Seen[(int32)ECall::Finish] > 0);
	bMix &= TestTrue(TEXT("precondition: a tick held a batch call of two members or more beside two or more other calls"), bMixedBatchTick);
	if (!bMix) { return false; }
	// The per-tick value is the sum of its calls.
	int32 BatchTicks = 0;
	for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
	{
		const FString Tick = FString::Printf(TEXT("tick %lld"), R.Tick);
		double Sum = 0.0, Others = 0.0;
		int32 Subs = 0, Timed = 0;
		bool bComposedBatch = false;
		for (int32 J = 0; J < R.Calls.Num(); ++J)
		{
			const FSuperSLMGpuTickCallRecord& K = R.Calls[J];
			const FString Call = FString::Printf(TEXT("%s, call %d (kind %d, %d members)"), *Tick, J, (int32)K.Kind, K.Members);
			Sum += K.CounterDeltaUs;
			Subs += K.Kind == ECall::Batch ? K.Members : K.Kind == ECall::Finish ? 0 : 1;
			Timed += K.CounterDeltaUs > 0.0 ? 1 : 0;
			if (J + 1 < R.Calls.Num()) { Others += K.CounterDeltaUs; }
			bComposedBatch |= K.Kind == ECall::Batch && K.Members >= 2;
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the call was timed (delta %.1f us, published %.1f us)"), *Call, K.CounterDeltaUs, K.LastPublishedUs),
				K.CounterDeltaUs > 0.0 && K.LastPublishedUs > 0.0);
			if (K.Kind == ECall::Batch && K.Members >= 2)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("%s: the batch advanced the counter by %.1f us, at least 1.25 x its last member's %.1f us (never just the last member's)"), *Call, K.CounterDeltaUs, K.LastPublishedUs),
					K.CounterDeltaUs >= 1.25 * K.LastPublishedUs);
			}
			else if (K.Kind == ECall::Finish)
			{
				bOk &= TestEqual(*(Call + TEXT(": a Finish is no submission (Members 0, ruling 46a)")), K.Members, 0);
				bOk &= TestTrue(*FString::Printf(TEXT("%s: the Finish advanced the counter by what it published (%.1f us vs %.1f us)"), *Call, K.CounterDeltaUs, K.LastPublishedUs),
					FMath::Abs(K.CounterDeltaUs - K.LastPublishedUs) <= 1.0);
			}
			else if (K.Kind == ECall::ExactClose)
			{
				bOk &= TestTrue(*FString::Printf(TEXT("%s: the close advanced the counter by %.1f us, at least what it published (%.1f us)"), *Call, K.CounterDeltaUs, K.LastPublishedUs),
					K.CounterDeltaUs + 1.0 >= K.LastPublishedUs);
			}
			else
			{
				bOk &= TestTrue(*FString::Printf(TEXT("%s: the call advanced the counter by what it published (%.1f us vs %.1f us)"), *Call, K.CounterDeltaUs, K.LastPublishedUs),
					FMath::Abs(K.CounterDeltaUs - K.LastPublishedUs) <= 1.0);
			}
		}
		bOk &= TestTrue(*FString::Printf(TEXT("%s: GpuBusyUs %.1f us equals the sum of its %d calls' %.1f us"), *Tick, R.GpuBusyUs, R.Calls.Num(), Sum), FMath::Abs(R.GpuBusyUs - Sum) <= 1.0);
		bOk &= TestEqual(*(Tick + TEXT(": Submissions equals its calls' submissions (a batch counts its members)")), R.Submissions, Subs);
		bOk &= TestEqual(*(Tick + TEXT(": no untimed submission (ruling 45a)")), R.UntimedSubmissions, 0);
		if (bComposedBatch)
		{
			++BatchTicks;
			bOk &= TestEqual(*(Tick + TEXT(": a Composed batch tick has no untimed submission (ruling 45a)")), R.UntimedSubmissions, 0);
		}
		if (Timed >= 2)
		{
			const double Last = R.Calls.Last().LastPublishedUs;
			bOk &= TestTrue(*FString::Printf(TEXT("%s: GpuBusyUs %.1f us is at least its last call's %.1f us plus half the other calls' %.1f us (never the last call's)"), *Tick, R.GpuBusyUs, Last, Others),
				R.GpuBusyUs >= Last + 0.5 * Others);
		}
	}
	bOk &= TestTrue(*FString::Printf(TEXT("a Composed batch tick was checked (%d)"), BatchTicks), BatchTicks > 0);
	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ruling 48 (erratum E-13, plan.md §16): the load-time row-cost calibration. At GPU Configure, once per model and
// adapter, the subsystem measures C(1) on a throwaway sequence (1 warm-up, then 5 timed one-row whole-depth tokens
// including the logits finish, each the cumulative GPU-busy counter's delta), takes the max rounded up to 0.1 ms, and
// uses it as GpuRowCostMs, unless the game set GpuRowCostMs (it wins and calibration is skipped) or
// bCalibrateGpuRowCost is false (6.2 ms). A price outside [1.24 ms, 124 ms] is retried once, then falls back to 6.2 ms
// with a warning; a failure falls back too; the model is never refused. At a frame budget the calibration runs one slice
// per tick and never puts more than B into a tick; at B = 0 it runs whole tokens. The first generation waits for it.
// The pure pieces are Private/Core/SuperSLMGpuRowCostCalibration.h (ci/host/multirow/test_calibration.cpp).
// Ruling 54: calibration runs in two phases. Phase 1 (attempts 0 and 1) times the early position only; it is what
// IsRowCostCalibrating() and the first generation wait for, and it settles Calibrated with s_default = 0.0015 x C_a per
// position. Phase 2 (attempt 2 in the call records) runs only at B > 0, at the lowest priority, in the room the
// generations leave; it fills to P2, times it, and replaces s_default with the measured slope (bSlopeMeasured).
// MrConfig() turns calibration off for every other cell here, so their prices stay at 6.2 ms.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
	using ESrc = ESuperSLMGpuRowCostSource;

	// Every line logged while it lives, from any thread.
	struct FLogCapture : public FOutputDevice
	{
		FCriticalSection Lock;
		TArray<TPair<ELogVerbosity::Type, FString>> Lines;
		FLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FLogCapture() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock L(&Lock);
			Lines.Emplace(Verbosity, FString(V));
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		// Lines at Verbosity (or, with bAtMost, at that level or more verbose) that contain every one of Parts.
		// Ruling 53 (triage run 3): the redirector delivers to an any-thread device asynchronously, so Count() flushes
		// GLog first; a count read before the lines arrive is 0 whatever was logged.
		int32 Count(ELogVerbosity::Type Verbosity, bool bAtMost, const TArray<FString>& Parts)
		{
			GLog->Flush();
			FScopeLock L(&Lock);
			int32 N = 0;
			for (const TPair<ELogVerbosity::Type, FString>& Line : Lines)
			{
				const ELogVerbosity::Type V = static_cast<ELogVerbosity::Type>(Line.Key & ELogVerbosity::VerbosityMask);
				if (bAtMost ? V < Verbosity : V != Verbosity) { continue; }
				bool bAll = true;
				for (const FString& P : Parts) { bAll &= Line.Value.Contains(P); }
				N += bAll ? 1 : 0;
			}
			return N;
		}
	};

	// Ruling 48's price, worked here: the max of the timed runs rounded up to a multiple of 100 us, a max within 0.01
	// us of a multiple being that multiple. -1 for no runs or a run that is not finite or not above 0.
	int64 RefPriceUs(const TArray<double>& Runs)
	{
		if (Runs.Num() == 0) { return -1; }
		double Max = 0.0;
		for (const double R : Runs)
		{
			if (!FMath::IsFinite(R) || R <= 0.0) { return -1; }
			Max = FMath::Max(Max, R);
		}
		const double Near = FMath::RoundHalfFromZero(Max / 100.0) * 100.0;
		return FMath::Abs(Max - Near) <= 0.01 ? static_cast<int64>(Near) : static_cast<int64>(FMath::CeilToDouble(Max / 100.0)) * 100;
	}

	// GpuRowCostMs 0: unset (ruling 48a), so the switch decides.
	bool CalConfig(FAutomationTestBase& T, EPath Path, bool bCalibrate, FSuperSLMGpuRuntimeConfig& Out)
	{
		if (!MrConfig(T, Path, true, Out)) { return false; }
		Out.GpuRowCostMs = 0.0;
		return
			T.TestTrue(*FString::Printf(TEXT("bCalibrateGpuRowCost = %s is set on the configuration (ruling 48)"), bCalibrate ? TEXT("true") : TEXT("false")),
				FMr::SetCalibrateGpuRowCost(Out, bCalibrate));
	}

	// Pumps until the calibration settles (or at once when none runs), then reads the record.
	bool Settle(FAutomationTestBase& T, const FGpuRig& Rig, FSuperSLMGpuRowCostRecord& Out, const FString& What)
	{
		const bool bSettled = Pump(*Rig.Gpu, [&]() { return !FMr::IsRowCostCalibrating(*Rig.Gpu); });
		return T.TestTrue(*(What + TEXT(": the calibration settles")), bSettled) &&
			T.TestTrue(*(What + TEXT(": the row-cost record is readable")), FMr::GetRowCost(*Rig.Gpu, Out)) &&
			T.TestNotEqual(*(What + TEXT(": the row cost is settled")), (uint8)Out.Source, (uint8)ESrc::Pending);
	}

	// Ruling 51 (F7): the calibration token's own finish is a Finish call that carries its attempt.
	bool IsCalibrationFinish(const FSuperSLMGpuTickCallRecord& K)
	{
		return K.Kind == ESuperSLMGpuTickCallKind::Finish && K.CalibrationAttempt >= 0;
	}

	// Ruling 54: phase 2's calls (its slices and finishes) carry attempt 2.
	constexpr int32 kPhase2Attempt = 2;

	// One phase's calibration slices (Calibration calls) in the tick records, and the last tick that held one of that
	// phase's slices or calibration finishes (MIN_int64: none). Phase 1 by default (attempts 0 and 1, ruling 54).
	int32 CalibrationCalls(const TArray<FSuperSLMGpuTickGpuBusyRecord>& Records, int64& OutLastTick, bool bPhase2 = false)
	{
		int32 N = 0;
		OutLastTick = MIN_int64;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				const bool bSlice = K.Kind == ESuperSLMGpuTickCallKind::Calibration;
				if (!bSlice && !IsCalibrationFinish(K)) { continue; }
				if ((K.CalibrationAttempt == kPhase2Attempt) != bPhase2) { continue; }
				N += bSlice ? 1 : 0;
				OutLastTick = FMath::Max(OutLastTick, R.Tick);
			}
		}
		return N;
	}

	// Ruling 51 (F5): a cell that calibrates leaves no price in the process-wide cache for later cells or suites.
	struct FForgetCalibrationsOnExit
	{
		~FForgetCalibrationsOnExit() { FMr::ForgetRowCostCalibrations(); }
	};

	// Ruling 51 (F2), ruling 52: Settle() at a game's frame rate: each tick is followed by about 33 ms of sleep (30 fps),
	// so the cell tells the 10 s cap at B > 0 from the old 2 s one.
	bool SettlePaced(FAutomationTestBase& T, const FGpuRig& Rig, FSuperSLMGpuRowCostRecord& Out, const FString& What)
	{
		const bool bSettled = Pump(*Rig.Gpu, [&]() { return !FMr::IsRowCostCalibrating(*Rig.Gpu); }, kRunSeconds,
			[]() { FPlatformProcess::Sleep(0.033f); });
		return T.TestTrue(*(What + TEXT(": the calibration settles (paced at about 33 ms a tick)")), bSettled) &&
			T.TestTrue(*(What + TEXT(": the row-cost record is readable")), FMr::GetRowCost(*Rig.Gpu, Out)) &&
			T.TestNotEqual(*(What + TEXT(": the row cost is settled")), (uint8)Out.Source, (uint8)ESrc::Pending);
	}

	// Ruling 54: a tick's work in units: its generation actions' rows x layers, each calibration slice's layers, and each
	// calibration finish as one (ruling 50: a finish counts as a layer).
	TMap<int64, int64> RowLayersByTick(const TArray<FAct>& Acts)
	{
		TMap<int64, int64> Out;
		for (const FAct& A : Acts) { Out.FindOrAdd(A.Tick) += static_cast<int64>(FMath::Max(A.Rows, 1)) * FMath::Max(A.Layers, 0); }
		return Out;
	}

	int64 TickUnits(const FSuperSLMGpuTickGpuBusyRecord& R, const TMap<int64, int64>& ActionRowLayers)
	{
		int64 U = ActionRowLayers.FindRef(R.Tick);
		for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
		{
			if (K.Kind == ESuperSLMGpuTickCallKind::Calibration) { U += static_cast<int64>(FMath::Max(K.Layers, 0)) * FMath::Max(K.Rows, 1); } // ruling 56: a widened warm-up is M row-layers
			else if (IsCalibrationFinish(K)) { U += 1; }
		}
		return U;
	}

	// Ruling 54: every tick (with bCalibrationTicksOnly, every tick that holds a calibration call of either phase) holds
	// at most one phase-2 slice, and its GPU time is at most BudgetUs unless its work is a single unit (TickUnits <= 1).
	bool RequireTicksWithinB(FAutomationTestBase& T, const TArray<FSuperSLMGpuTickGpuBusyRecord>& Records, const TArray<FAct>& Acts, double BudgetUs,
		bool bCalibrationTicksOnly, const FString& What)
	{
		const TMap<int64, int64> RowLayers = RowLayersByTick(Acts);
		bool bOk = true;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			int32 Phase2 = 0;
			bool bCal = false;
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				const bool bSlice = K.Kind == ESuperSLMGpuTickCallKind::Calibration;
				bCal |= bSlice || IsCalibrationFinish(K);
				Phase2 += bSlice && K.CalibrationAttempt == kPhase2Attempt ? 1 : 0;
			}
			if (bCalibrationTicksOnly && !bCal) { continue; }
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: at most one phase-2 slice (%d)"), *What, R.Tick, Phase2), Phase2 <= 1);
			const int64 Units = TickUnits(R, RowLayers);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: %.1f us of GPU time, at most B (%.0f us), or a single unit (%lld)"), *What, R.Tick, R.GpuBusyUs, BudgetUs, Units),
				R.GpuBusyUs <= BudgetUs || Units <= 1);
		}
		return bOk;
	}

	// Rulings 50 and 51: with only the calibration recorded, each phase-1 attempt's first slice is one layer, each tick
	// holds at most one phase-1 slice of one layer or more, and every tick's GPU time is at most BudgetUs unless the tick
	// ran a single layer with no finish riding, or only the calibration token's finish. Ruling 54: phase 2's slices
	// (attempt 2) are not attempts and their first slice is not held to one layer; a tick holds at most one of them, and
	// the budget rule counts them.
	bool RequireCalibrationSlices(FAutomationTestBase& T, const TArray<FSuperSLMGpuTickGpuBusyRecord>& Records, const FSuperSLMGpuRowCostRecord& Rec,
		double BudgetUs, const FString& What)
	{
		using ECall = ESuperSLMGpuTickCallKind;
		bool bLayers = true;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls) { bLayers &= K.Kind != ECall::Calibration || (K.Layers >= 0 && K.CalibrationAttempt >= 0); }
		}
		if (!T.TestTrue(*(What + TEXT(": precondition: every calibration call carries its layers and its attempt")), bLayers)) { return false; }
		bool bOk = true;
		TSet<int32> AttemptsSeen;
		const TMap<int64, int64> NoActions;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			int32 Phase1 = 0, Phase2 = 0, Others = 0;
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				if (K.Kind == ECall::Calibration)
				{
					bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: a calibration slice ran one layer or more (%d)"), *What, R.Tick, K.Layers), K.Layers >= 1);
					if (K.CalibrationAttempt == kPhase2Attempt) { ++Phase2; continue; }
					if (K.Rows >= 2) { continue; } // ruling 56 (2): the widened warm-up is no token slice
					++Phase1;
					if (!AttemptsSeen.Contains(K.CalibrationAttempt))
					{
						AttemptsSeen.Add(K.CalibrationAttempt);
						bOk &= T.TestEqual(*FString::Printf(TEXT("%s, attempt %d: its first slice (tick %lld) is exactly one layer"), *What, K.CalibrationAttempt, R.Tick), K.Layers, 1);
					}
				}
				else if (!IsCalibrationFinish(K)) { ++Others; }
			}
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s, tick %lld: nothing but the calibration ran"), *What, R.Tick), Others, 0);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: at most one phase-1 slice (%d)"), *What, R.Tick, Phase1), Phase1 <= 1);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: at most one phase-2 slice (%d)"), *What, R.Tick, Phase2), Phase2 <= 1);
			const int64 Units = TickUnits(R, NoActions);
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: %.1f us of GPU time, at most B (%.0f us), or a single layer (%lld units: layers and finishes)"), *What, R.Tick, R.GpuBusyUs, BudgetUs, Units),
				R.GpuBusyUs <= BudgetUs || Units <= 1);
		}
		bOk &= T.TestEqual(*(What + TEXT(": every phase-1 attempt's slices were seen")), AttemptsSeen.Num(), Rec.Attempts);
		return bOk;
	}

	// Both phases' calibration slices since StartRecording().
	int32 CalibrationCallsSinceRecording(const FGpuRig& Rig)
	{
		TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
		FMr::GetTickGpuBusyUs(*Rig.Gpu, Records);
		int64 Last = 0;
		return CalibrationCalls(Records, Last, false) + CalibrationCalls(Records, Last, true);
	}

	int32 Phase2CallsSinceRecording(const FGpuRig& Rig)
	{
		TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
		FMr::GetTickGpuBusyUs(*Rig.Gpu, Records);
		int64 Last = 0;
		return CalibrationCalls(Records, Last, true);
	}

	// Ruling 54: pumps with nothing else running until phase 2 is no longer pending (settled, failed or never started),
	// then reads the record.
	bool SettleLate(FAutomationTestBase& T, const FGpuRig& Rig, FSuperSLMGpuRowCostRecord& Out, const FString& What)
	{
		const bool bSettled = Pump(*Rig.Gpu, [&]()
		{
			FSuperSLMGpuRowCostRecord R;
			return FMr::GetRowCost(*Rig.Gpu, R) && !R.bLatePending;
		});
		return T.TestTrue(*(What + TEXT(": phase 2 settles in a free pump (ruling 54: no timeout)")), bSettled) &&
			T.TestTrue(*(What + TEXT(": the row-cost record is readable")), FMr::GetRowCost(*Rig.Gpu, Out));
	}

	// Pumps N ticks.
	void PumpTicks(const FGpuRig& Rig, int32 N)
	{
		int32 Ticks = 0;
		Pump(*Rig.Gpu, [&]() { return ++Ticks >= N; });
	}

	// The row cost in use is Ms everywhere it shows: the record, the chooser's C(1) and the public getter.
	bool RequireRowCost(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuRowCostRecord& Rec, double Ms, const FString& What)
	{
		bool bOk = T.TestTrue(*FString::Printf(TEXT("%s: the row cost in use is %.4f ms (want %.4f)"), *What, Rec.RowCostMs, Ms), FMath::Abs(Rec.RowCostMs - Ms) <= 1e-9);
		bOk &= T.TestEqual(*(What + TEXT(": the chooser's C(1), in us")), Rec.PlanRowCostUs, static_cast<int64>(FMath::RoundHalfFromZero(Ms * 1000.0)));
		const double Pub = FMr::GetPublicGpuRowCostMs(*Rig.Gpu);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the public getter reads %.4f ms (want %.4f)"), *What, Pub, Ms), FMath::Abs(Pub - Ms) <= 1e-9);
		return bOk;
	}

	// Erratum E-14 (ruling 53): a line in position, worked here: C(1, pos) = C_a + ceil(Num x (pos - pos_a) / Den), a
	// cost rounded up (before pos_a the term is negative and rounds toward C_a).
	int64 RefLineAtUs(int64 CaUs, int64 PosA, int64 Num, int64 Den, int64 Pos)
	{
		if (Den <= 0 || Num <= 0) { return CaUs; }
		const int64 X = Num * (Pos - PosA);
		return CaUs + (X >= 0 ? (X + Den - 1) / Den : -((-X) / Den));
	}

	// The measured line: slope = max(0, C_b - C_a) / (P2 - pos_a).
	int64 RefRowCostAtUs(int64 CaUs, int64 PosA, int64 CbUs, int64 PosB, int64 Pos)
	{
		return RefLineAtUs(CaUs, PosA, FMath::Max<int64>(0, CbUs - CaUs), PosB - PosA, Pos);
	}

	// Ruling 54: the default line: s_default = 0.0015 x C_a = 3 C_a / 2,000 us per position.
	int64 RefDefaultRowCostAtUs(int64 CaUs, int64 PosA, int64 Pos) { return RefLineAtUs(CaUs, PosA, 3 * CaUs, 2000, Pos); }

	// l(1) for a slice that ends where C(1) is RowCostUs, from FTickPrices' rules (E-14's margin with the knob set).
	int64 OneRowLayersAtUs(const FSuperSLMGpuRuntimeConfig& Cfg, int64 RowCostUs)
	{
		FTickPrices P = PricesOf(Cfg);
		P.C1 = RowCostUs;
		// Ruling 56 (6): with the knob at 0 the room is the C(1) in use (B_eff = C(1) + Scene), so l(1) = L.
		if (!P.bKnob) { P.RoomUs = RowCostUs; }
		return OneRowLayers(P);
	}

	// Ruling 56 (1): F with none measured, ceil(g(1) x C(1) / 1000) from the vendored table.
	int64 RefDefaultFinishUs(int64 RowCostUs)
	{
		return (static_cast<int64>(superslm_gpu::kGpuRowsLogitsRatioPerMille[0]) * RowCostUs + 999) / 1000;
	}

	// Ruling 56 (1), worked here: the most layers (at least 1) of a slice that ends a token, priced
	// (l x (C(1) - F) + F x L) x (1000 + m) with the knob set (x 1000 at B = 0, where the room is C(1)), within Room x L x 1000.
	int64 RefFinishSliceLayers(const FSuperSLMGpuRuntimeConfig& Cfg, int64 RowCostUs, int64 FinishUs)
	{
		const FTickPrices P = PricesOf(Cfg);
		const int64 Room = P.bKnob ? P.RoomUs : RowCostUs;
		const int64 K = P.bKnob ? 1000 + P.Margin : 1000;
		int64 Best = 1;
		for (int64 Layers = 1; Layers <= P.L; ++Layers)
		{
			if ((Layers * (RowCostUs - FinishUs) + FinishUs * P.L) * K <= Room * P.L * 1000) { Best = Layers; }
		}
		return Best;
	}

	// Ruling 56 (2), 56a (3), 57 (F1, F5), 57a: phase 1's load-time warm-up of the widened row kernels. The widths are
	// exactly the chooser's candidate widths (GetWidenWarmUpRows: within 2..8, unique; the set may be empty). In each
	// phase-1 attempt (0, and 1 for a retry, which adds its own set) each width either ran once -- one Calibration call
	// of one layer at M rows, carrying that attempt -- or was skipped with a Display line naming it ("GPU GpuRowCost
	// load-time warm-up for" and "M = <M> skipped"; ruling 57a: Display, since the box counts Warnings as test errors,
	// so no Warning names it); the smallest width ran in every attempt; no widened calibration call is outside the
	// widths or of another shape. A cache hit runs no phase 1, so it has no attempt and no set.
	bool RequireWidenedWarmUp(FAutomationTestBase& T, const USuperSLMGpuSubsystem& Gpu, const TArray<FSuperSLMGpuTickGpuBusyRecord>& Records, FLogCapture& Log,
		const FString& What)
	{
		TArray<int32> Widths;
		if (!T.TestTrue(*(What + TEXT(": the chooser's candidate widths are readable (GetWidenWarmUpRows; ruling 57 F5)")), FMr::GetWidenWarmUpRows(Gpu, Widths)))
		{
			return false;
		}
		FString Listed;
		for (const int32 M : Widths) { Listed += FString::Printf(TEXT(" %d"), M); }
		T.AddInfo(FString::Printf(TEXT("%s: the chooser's candidate widths:%s"), *What, Listed.IsEmpty() ? TEXT(" none") : *Listed));
		bool bOk = true;
		int32 Smallest = 0;
		for (const int32 M : Widths) { Smallest = Smallest == 0 ? M : FMath::Min(Smallest, M); }
		for (int32 I = 0; I < Widths.Num(); ++I)
		{
			bOk &= T.TestTrue(*FString::Printf(TEXT("%s: width %d is in 2..8 and listed once"), *What, Widths[I]),
				Widths[I] >= 2 && Widths[I] <= 8 && Widths.IndexOfByKey(Widths[I]) == I);
		}
		TMap<int32, TArray<int32>> ByAttempt; // phase-1 attempt -> calls at M (index M)
		int32 Other = 0;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				if (K.Kind != ESuperSLMGpuTickCallKind::Calibration || K.CalibrationAttempt < 0 || K.CalibrationAttempt == kPhase2Attempt) { continue; }
				TArray<int32>& ByM = ByAttempt.FindOrAdd(K.CalibrationAttempt);
				if (ByM.Num() == 0) { ByM.SetNumZeroed(9); }
				if (K.Rows < 2) { continue; }
				const bool bWarmUp = K.Layers == 1 && K.Rows <= 8 && Widths.Contains(K.Rows);
				if (bWarmUp) { ++ByM[K.Rows]; }
				else { ++Other; }
			}
		}
		bOk &= T.TestTrue(*(What + TEXT(": phase 1 ran (precondition for its widened warm-up)")), ByAttempt.Num() > 0);
		for (const int32 M : Widths)
		{
			if (M < 2 || M > 8) { continue; }
			const TArray<FString> Parts = { TEXT("GPU GpuRowCost load-time warm-up for"), FString::Printf(TEXT("M = %d skipped"), M) };
			const int32 Skipped = Log.Count(ELogVerbosity::Display, false, Parts);
			// 59g: the smallest width's skip is accepted only as the half-room rule's (59.2), never a skip a failed status caused.
			const TArray<FString> HalfParts = { TEXT("GPU GpuRowCost load-time warm-up for"), FString::Printf(TEXT("M = %d skipped"), M), TEXT("over half the room") };
			const int32 SkippedHalf = Log.Count(ELogVerbosity::Display, false, HalfParts);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: no Warning names the %d-row skip (ruling 57a: it logs at Display)"), *What, M), Log.Count(ELogVerbosity::Warning, false, Parts), 0);
			int32 Ran = 0;
			for (const TPair<int32, TArray<int32>>& A : ByAttempt)
			{
				Ran += A.Value[M];
				bOk &= T.TestTrue(*FString::Printf(TEXT("%s: phase 1 attempt %d warmed the %d-row kernel at most once (%d)"), *What, A.Key, M, A.Value[M]), A.Value[M] <= 1);
				if (M == Smallest)
				{
					// Rulings 59.2, 59c point 3: the smallest width is warmed, or skipped by the half-room rule with its Display line
					// when Configure()'s own warm-up ran that width (ruling 59b L1). A skip with no Configure warm-up still fails.
					uint32 ConfigureMask = 0;
					const bool bMaskRead = FMr::GetConfigureWidenedMask(Gpu, ConfigureMask);
					bOk &= T.TestTrue(*FString::Printf(TEXT("%s: phase 1 attempt %d warmed the smallest width, the %d-row kernel, one layer (ruling 57a), or skipped it above half the room with Configure's warm-up having run it (rulings 59.2, 59c; %d ran, %d skip lines, Configure mask 0x%x)"), *What, A.Key, M, A.Value[M], Skipped, ConfigureMask),
						A.Value[M] == 1 || (A.Value[M] == 0 && SkippedHalf > 0 && bMaskRead && (ConfigureMask >> M & 1u) != 0));
				}
			}
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: the %d-row kernel ran or was skipped with a Display line naming it in each phase-1 attempt (%d ran, %d skip lines; rulings 57 F1, 57a)"),
				*What, M, Ran, Skipped), Ran + Skipped, ByAttempt.Num());
		}
		bOk &= T.TestEqual(*(What + TEXT(": no other widened calibration call")), Other, 0);
		return bOk;
	}

	int64 CaUsOf(const FSuperSLMGpuRowCostRecord& Rec) { return static_cast<int64>(FMath::RoundHalfFromZero(Rec.RowCostMs * 1000.0)); }

	// P2 = min(256, ContextCap - 8).
	int64 RefLatePosition(int64 ContextCap) { return FMath::Min<int64>(256, ContextCap - 8); }

	// A flat line (the game's value, the default, the fallback): slope 0, no positions, the price at every position.
	bool RequireFlatLine(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuRowCostRecord& Rec, int64 Us, const FString& What)
	{
		bool bOk = T.TestEqual(*(What + TEXT(": the line is flat (slope 0; E-14)")), Rec.SlopeUsPerPosition, 0.0);
		for (const int64 Pos : { int64{0}, int64{100}, int64{256}, int64{4000} })
		{
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: C(1, %lld) is the flat price"), *What, Pos), FMr::GetRowCostAtUs(*Rig.Gpu, Pos), Us);
		}
		return bOk;
	}

	// Ruling 54: the default line is in use: the slope is not measured, it is 0.0015 x C_a, the late fields are empty,
	// and C(1, pos) and (given Cfg) l(1) follow C_a + s_default x (pos - pos_a) at pos_a, 256 and 4,000.
	bool RequireDefaultLine(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuRowCostRecord& Rec, const FSuperSLMGpuRuntimeConfig* Cfg, const FString& What)
	{
		const int64 Ca = CaUsOf(Rec);
		bool bOk = T.TestFalse(*(What + TEXT(": the slope is not the measured one (ruling 54)")), Rec.bSlopeMeasured);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the slope %.6f us/position is s_default = 0.0015 x C_a = %.6f"), *What, Rec.SlopeUsPerPosition, 0.0015 * static_cast<double>(Ca)),
			FMath::Abs(Rec.SlopeUsPerPosition - 0.0015 * static_cast<double>(Ca)) <= 1e-9);
		bOk &= T.TestEqual(*(What + TEXT(": no late position yet")), Rec.LatePosition, static_cast<int64>(-1));
		bOk &= T.TestEqual(*(What + TEXT(": no C_b yet")), Rec.LateRowCostUs, static_cast<int64>(-1));
		bOk &= T.TestEqual(*(What + TEXT(": no late warm-up yet")), Rec.LateWarmUpRuns, 0);
		bOk &= T.TestEqual(*(What + TEXT(": no late timed runs yet")), Rec.LateTimedRunsUs.Num(), 0);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: pos_a (%lld) is recorded"), *What, Rec.AnchorPosition), Rec.AnchorPosition >= 0)) { return false; }
		for (const int64 Pos : { Rec.AnchorPosition, int64{256}, int64{4000} })
		{
			const int64 Want = RefDefaultRowCostAtUs(Ca, Rec.AnchorPosition, Pos);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: C(1, %lld) follows the default line"), *What, Pos), FMr::GetRowCostAtUs(*Rig.Gpu, Pos), Want);
			if (Cfg)
			{
				bOk &= T.TestEqual(*FString::Printf(TEXT("%s: l(1) for a slice ending at %lld is priced at C(1, %lld) = %lld us"), *What, Pos, Pos, Want),
					static_cast<int64>(FMr::GetOneRowLayersAt(*Rig.Gpu, Pos)), OneRowLayersAtUs(*Cfg, Want));
			}
		}
		return bOk;
	}

	// Ruling 54: phase 2 settled on the measured line through (pos_a, C_a) and (P2, CbUs): one warm-up and 3 timed
	// tokens at P2, the slope max(0, C_b - C_a) / (P2 - pos_a), and C(1, pos) and l(1) on it at pos_a, P2 and 4,000.
	bool RequireMeasuredLine(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuRowCostRecord& Rec, const FSuperSLMGpuRuntimeConfig& Cfg, int64 CbUs, const FString& What)
	{
		const int64 Ca = CaUsOf(Rec), P2 = RefLatePosition(Cfg.ContextCap);
		bool bOk = T.TestTrue(*(What + TEXT(": the slope is the measured one (ruling 54)")), Rec.bSlopeMeasured);
		bOk &= T.TestFalse(*(What + TEXT(": phase 2 is no longer pending")), Rec.bLatePending);
		bOk &= T.TestEqual(*(What + TEXT(": P2 = min(256, ContextCap - 8)")), Rec.LatePosition, P2);
		bOk &= T.TestEqual(*(What + TEXT(": C_b")), Rec.LateRowCostUs, CbUs);
		bOk &= T.TestEqual(*(What + TEXT(": the late position: one warm-up token")), Rec.LateWarmUpRuns, 1);
		bOk &= T.TestEqual(*(What + TEXT(": the late position: 3 timed tokens")), Rec.LateTimedRunsUs.Num(), 3);
		if (!T.TestTrue(*FString::Printf(TEXT("%s: pos_a (%lld) is below P2"), *What, Rec.AnchorPosition), Rec.AnchorPosition >= 0 && Rec.AnchorPosition < P2)) { return false; }
		const double Slope = static_cast<double>(FMath::Max<int64>(0, CbUs - Ca)) / static_cast<double>(P2 - Rec.AnchorPosition);
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the slope %.6f us/position is max(0, C_b - C_a) / (P2 - pos_a) = %.6f"), *What, Rec.SlopeUsPerPosition, Slope),
			FMath::Abs(Rec.SlopeUsPerPosition - Slope) <= 1e-9);
		for (const int64 Pos : { Rec.AnchorPosition, P2, int64{4000} })
		{
			const int64 Want = RefRowCostAtUs(Ca, Rec.AnchorPosition, CbUs, P2, Pos);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: C(1, %lld) follows the measured line"), *What, Pos), FMr::GetRowCostAtUs(*Rig.Gpu, Pos), Want);
			bOk &= T.TestEqual(*FString::Printf(TEXT("%s: l(1) for a slice ending at %lld is priced at C(1, %lld) = %lld us, with the margin"), *What, Pos, Pos, Want),
				static_cast<int64>(FMr::GetOneRowLayersAt(*Rig.Gpu, Pos)), OneRowLayersAtUs(Cfg, Want));
		}
		return bOk;
	}

	// A short Off generation after the calibration: the model was not refused and its tokens are the CPU's.
	bool RequireGenerates(FAutomationTestBase& T, FGpuRig& Rig, const FString& What)
	{
		FSuperSLMGenerationRequest Req;
		FRunResult Cpu;
		if (!MakeRequest(T, Rig, kOff, kMr6MaxNew, Req) || !RunCpu(T, Rig, Req, kOff, Cpu)) { return false; }
		FGpuRun Run;
		TArray<FAct> Acts;
		const bool bRan = RunMr(T, Rig, EPath::WholeToken, Req, Run, Acts);
		return T.TestTrue(*(What + TEXT(": a generation runs")), bRan) && T.TestEqual(*(What + TEXT(": tokens equal the CPU's Off run")), Run.Tokens, Cpu.Tokens);
	}
}

// R48.1 (ruling 48: the game's value wins). bCalibrateGpuRowCost true and GpuRowCostMs set by the game to 7.3 ms (> 0
// is the game's value, ruling 48a): no
// calibration runs (none counted, no calibration call recorded), the source is Game and 7.3 ms is the row cost in the
// record, the chooser's C(1) (7,300 us) and the public getter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48GameWinsTest, "SuperSLM.MultiRow.MR6.R48_1.GameRowCostWinsAndSkipsCalibration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48GameWinsTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.GpuRowCostMs = 7.3;
	FMr::ForgetRowCostCalibrations();
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R48.1"))) { return false; }
	bool bOk = TestEqual(TEXT("the source is the game's value"), (uint8)Rec.Source, (uint8)ESrc::Game);
	bOk &= RequireRowCost(*this, C.Rig, Rec, 7.3, TEXT("R48.1"));
	bOk &= RequireFlatLine(*this, C.Rig, Rec, 7300, TEXT("R48.1, the game's value (E-14)"));
	bOk &= TestFalse(TEXT("F is not measured (ruling 56)"), Rec.bFinishMeasured);
	bOk &= TestEqual(TEXT("F is ceil(g(1) x C(1) / 1000) at the game's 7.3 ms (ruling 56)"), Rec.FinishUs, RefDefaultFinishUs(7300));
	bOk &= TestEqual(TEXT("no calibration ran"), FMr::GetRowCostCalibrationCount(), 0);
	bOk &= TestEqual(TEXT("no attempt"), Rec.Attempts, 0);
	bOk &= TestEqual(TEXT("no calibration call was recorded"), CalibrationCallsSinceRecording(C.Rig), 0);
	return bOk;
}

// R48.2 (ruling 48: the measured value is used, logged and exposed; ruling 54: at B = 0, phase 1 only). OneCall,
// multi-row on, B = 0, bCalibrateGpuRowCost true, no game value. The calibration settles Calibrated after one attempt,
// timed at the early position (pos_a) with 3 warm-up and 3 timed runs (ruling 56 (2)), each above 0. C_a is their max rounded up to
// 0.1 ms (worked here), inside [1.24 ms, 124 ms]; it is the row cost in the record, the chooser's C(1) and the public
// getter. At B = 0 phase 2 is skipped: after 30 more ticks it is not pending, no phase-2 call was recorded, the late
// fields are empty, and the line keeps s_default = 0.0015 x C_a (C(1, pos) and l(1) on it at pos_a, 256 and 4,000). One
// calibration is counted; it ran whole tokens (6 phase-1 token calls or more) and warmed each widened row kernel the
// chooser can pick once (M = 2, 4, 8, one layer each, ruling 56 (2)); F was measured (ruling 56 (1)). One Log line "GPU GpuRowCost calibrated for"
// names the model, the adapter and C_a ("%.1f ms"), and no "GPU GpuRowCost line for" line is logged. A generation then
// gives the CPU's tokens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48UsedTest, "SuperSLM.MultiRow.MR6.R48_2.CalibratedRowCostIsUsedLoggedAndExposed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48UsedTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	FLogCapture Log;
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R48.2"))) { return false; }
	if (!TestEqual(TEXT("the source is the measured price"), (uint8)Rec.Source, (uint8)ESrc::Calibrated)) { return false; }
	bool bOk = TestEqual(TEXT("one attempt"), Rec.Attempts, 1);
	bOk &= TestEqual(TEXT("the early position: 3 warm-up tokens (ruling 56)"), Rec.WarmUpRuns, 3);
	bOk &= TestTrue(*FString::Printf(TEXT("F was measured by phase 1 (%lld us; ruling 56)"), Rec.FinishUs), Rec.bFinishMeasured && Rec.FinishUs > 0);
	bOk &= TestEqual(TEXT("the early position: 3 timed tokens (E-14)"), Rec.TimedRunsUs.Num(), 3);
	for (int32 I = 0; I < Rec.TimedRunsUs.Num(); ++I)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("early timed run %d took GPU time (%.1f us)"), I, Rec.TimedRunsUs[I]), Rec.TimedRunsUs[I] > 0.0);
	}
	const int64 Price = RefPriceUs(Rec.TimedRunsUs);
	bOk &= TestTrue(*FString::Printf(TEXT("C_a %lld us is inside [1,240, 124,000] us"), Price), Price >= 1240 && Price <= 124000);
	bOk &= RequireRowCost(*this, C.Rig, Rec, static_cast<double>(Price) / 1000.0, TEXT("R48.2"));
	PumpTicks(C.Rig, 30);
	FSuperSLMGpuRowCostRecord After;
	if (!TestTrue(TEXT("the record is readable after 30 more ticks"), FMr::GetRowCost(*C.Rig.Gpu, After))) { return false; }
	bOk &= TestFalse(TEXT("at B = 0 phase 2 is not pending (ruling 54)"), After.bLatePending);
	bOk &= TestEqual(TEXT("at B = 0 no phase-2 call was recorded (ruling 54)"), Phase2CallsSinceRecording(C.Rig), 0);
	bOk &= RequireDefaultLine(*this, C.Rig, After, &Cfg, TEXT("R48.2, at B = 0 (ruling 54)"));
	bOk &= TestEqual(TEXT("one calibration counted"), FMr::GetRowCostCalibrationCount(), 1);
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	int64 Last = 0;
	const int32 CalCalls = CalibrationCalls(Records, Last);
	bOk &= TestTrue(*FString::Printf(TEXT("at B = 0 phase 1 ran its 6 tokens and 3 widened warm-ups (%d calibration calls; want 9 or more)"), CalCalls), CalCalls >= 9);
	bOk &= RequireWidenedWarmUp(*this, *C.Rig.Gpu, Records, Log, TEXT("R48.2"));
	bOk &= TestTrue(TEXT("the adapter is named"), !Rec.AdapterName.IsEmpty());
	const int32 Lines = Log.Count(ELogVerbosity::Log, true, { TEXT("GPU GpuRowCost calibrated for"), C.Rig.Cpu.Model->GetName(), Rec.AdapterName,
		FString::Printf(TEXT("%.1f ms"), Rec.RowCostMs) });
	bOk &= TestEqual(TEXT("one log line names the model, the adapter and C_a"), Lines, 1);
	bOk &= TestEqual(TEXT("no phase-2 line is logged at B = 0"), Log.Count(ELogVerbosity::Log, true, { TEXT("GPU GpuRowCost line for") }), 0);
	bOk &= RequireGenerates(*this, C.Rig, TEXT("R48.2"));
	return bOk;
}

// R48.3 (ruling 48: the calibration respects the frame budget; the first generation waits). OneCall, multi-row on,
// B = 2 ms, bCalibrateGpuRowCost true. Recording starts before Configure() and an Off generation is begun at once,
// before the calibration settles. Ruling 54 grades phase 1 here. Preconditions: phase 1 settled Calibrated and made at
// least 4 calls. Then: every tick that holds a phase-1 slice holds exactly one; every tick that holds a calibration call
// of either phase (phase 2 rides after the generation's actions) holds at most one phase-2 slice, and its GPU time (the
// per-tick seam's GpuBusyUs) is at most 2,000 us unless its work is a single unit (the generation's row-layers, the
// calibration's layers and finishes, rulings 50, 51); every action of the generation is recorded at a tick after phase
// 1's last tick (its finish included: the first generation waits for phase 1 only); the tokens equal the CPU's Off run.
// Ruling 56 (2), 57 (F1, F5), 57a: phase 1 warmed each of the chooser's candidate widths once (one layer; within B by
// the rule above) or skipped it with a Display line naming it, and the smallest width ran.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48BudgetTest, "SuperSLM.MultiRow.MR6.R48_3.CalibrationWithinBudgetAndFirstGenerationWaits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48BudgetTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	FMr::ForgetRowCostCalibrations();
	FLogCapture Log; // rulings 57 F1, 57a: a skipped width's Display line
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FGpuRun Run;
	if (!Vend(*this, C.Rig, EPath::WholeToken, Run)) { return false; }
	if (!TestTrue(TEXT("RequestBeginGeneration right after Configure()"), C.Rig.Gpu->RequestBeginGeneration(Run.Sequence, Req).IsValid())) { return false; }
	const bool bEnded = Pump(*C.Rig.Gpu, [&]() { return IsEnded(*C.Rig.Gpu, Run.Sequence); });
	Read(C.Rig, Run);
	const TArray<FAct> Acts = ActionsOf(*C.Rig.Gpu, Run.Sequence.Id);
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	ReturnAndSettle(C.Rig, Run.Sequence);
	FSuperSLMGpuRowCostRecord Rec;
	if (!TestTrue(TEXT("the generation ends"), bEnded) || !Settle(*this, C.Rig, Rec, TEXT("R48.3"))) { return false; }
	if (!TestEqual(TEXT("precondition: phase 1 settled on the measured price"), (uint8)Rec.Source, (uint8)ESrc::Calibrated)) { return false; }
	int64 LastCal = MIN_int64;
	const int32 Calls = CalibrationCalls(Records, LastCal);
	if (!TestTrue(*FString::Printf(TEXT("precondition: phase 1 was recorded (%d calls; want 4 or more: a warm-up and 3 timed, ruling 54)"), Calls), bRecorded && Calls >= 4)) { return false; }
	bool bOk = true;
	for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
	{
		int32 InTick = 0;
		for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
		{
			InTick += K.Kind == ESuperSLMGpuTickCallKind::Calibration && K.CalibrationAttempt != kPhase2Attempt && K.Rows < 2 ? 1 : 0; // ruling 56: one-row slices
		}
		if (InTick == 0) { continue; }
		bOk &= TestEqual(*FString::Printf(TEXT("tick %lld: one phase-1 slice per tick"), R.Tick), InTick, 1);
	}
	// Rulings 50, 51 and 54: the budget, phase 2's slices and the generation's work in the same tick included.
	bOk &= RequireTicksWithinB(*this, Records, Acts, 2000.0, true, TEXT("R48.3"));
	bOk &= RequireWidenedWarmUp(*this, *C.Rig.Gpu, Records, Log, TEXT("R48.3"));
	bOk &= TestTrue(TEXT("the generation ran actions"), Acts.Num() > 0);
	for (const FAct& A : Acts)
	{
		bOk &= TestTrue(*FString::Printf(TEXT("action %d at tick %lld: after phase 1's last tick (%lld): the first generation waits"), A.ActionIndex, A.Tick, LastCal), A.Tick > LastCal);
	}
	bOk &= TestEqual(TEXT("tokens equal the CPU's Off run"), Run.Tokens, Cpu.Tokens);
	return bOk;
}

// R48.4 (ruling 48: once per model and adapter). bCalibrateGpuRowCost true. A-EX configured: one calibration. A-EX
// configured again: none (the count stays 1, no calibration call recorded, no attempt) and the same row cost, from the
// cache; the same A-EX artifact imported again as a new model: the same (ruling 48a: the key is the artifact's content
// hash and the GPU adapter). The shipped 1.5B configured: a second calibration. A-EX again: still two, and its first
// row cost.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48OnceTest, "SuperSLM.MultiRow.MR6.R48_4.CalibratesOncePerModelAndAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48OnceTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPagingGpuFixtures;
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FString Path15, Reason;
	if (!TestTrue(*FString::Printf(TEXT("precondition: the shipped 1.5B artifact is present (%s)"), *Reason), ResolveArtifact(EGpuArtifact::TwoCap15B, Path15, Reason))) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model15 = FSuperSLMModelImport::ImportFromFile(Path15, Diag);
	if (!TestNotNull(TEXT("the 1.5B imports"), Model15) || !TestTrue(TEXT("the 1.5B is accepted"), Diag.bAccepted)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg, Cfg15 = MakeGpuConfig(EGpuArtifact::TwoCap15B, 4, 0);
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg) || !TestTrue(TEXT("bCalibrateGpuRowCost = true on the 1.5B's configuration"), FMr::SetCalibrateGpuRowCost(Cfg15, true))) { return false; }
	FMr::ForgetRowCostCalibrations();
	FSuperSLMGpuRowCostRecord First, Rec;
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, First, TEXT("A-EX, first"))) { return false; }
	bool bOk = TestEqual(TEXT("A-EX, first: calibrated"), (uint8)First.Source, (uint8)ESrc::Calibrated);
	bOk &= TestEqual(TEXT("A-EX, first: one calibration"), FMr::GetRowCostCalibrationCount(), 1);
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, TEXT("A-EX, again"))) { return false; }
	bOk &= TestEqual(TEXT("A-EX, again: no second calibration"), FMr::GetRowCostCalibrationCount(), 1);
	bOk &= TestEqual(TEXT("A-EX, again: no calibration call recorded"), CalibrationCallsSinceRecording(C.Rig), 0);
	bOk &= TestEqual(TEXT("A-EX, again: no attempt (the cache)"), Rec.Attempts, 0);
	bOk &= TestEqual(TEXT("A-EX, again: still the measured price"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	bOk &= RequireRowCost(*this, C.Rig, Rec, First.RowCostMs, TEXT("A-EX, again"));
	// E-14 (ruling 53): the cache holds both numbers, so a hit gives the same line.
	bOk &= TestEqual(TEXT("A-EX, again: the same slope (the cache holds both numbers)"), Rec.SlopeUsPerPosition, First.SlopeUsPerPosition);
	bOk &= TestEqual(TEXT("A-EX, again: the same pos_a"), Rec.AnchorPosition, First.AnchorPosition);
	bOk &= TestEqual(TEXT("A-EX, again: the same C_b"), Rec.LateRowCostUs, First.LateRowCostUs);
	bOk &= TestTrue(TEXT("A-EX, first: F was measured (ruling 56)"), First.bFinishMeasured && First.FinishUs > 0);
	bOk &= TestTrue(TEXT("A-EX, again: the cache holds F (ruling 56)"), Rec.bFinishMeasured && Rec.FinishUs == First.FinishUs);
	{
		FString PathAEx;
		if (!TestTrue(*FString::Printf(TEXT("precondition: the A-EX artifact is present (%s)"), *Reason), ResolveArtifact(EGpuArtifact::AEx05B, PathAEx, Reason))) { return false; }
		FSuperSLMImportDiagnostic DiagAEx;
		USuperSLMModel* Again = FSuperSLMModelImport::ImportFromFile(PathAEx, DiagAEx);
		if (!TestNotNull(TEXT("A-EX imports again"), Again) || !TestTrue(TEXT("A-EX is accepted again"), DiagAEx.bAccepted)) { return false; }
		if (!TestTrue(TEXT("precondition: the re-import is a new model object"), Again != C.Rig.Cpu.Model)) { return false; }
		FMr::StartRecording(*C.Rig.Gpu);
		const FSuperSLMGpuConfigureReport ReportAgain = C.Rig.Gpu->Configure(Again, Cfg);
		if (!TestEqual(*FString::Printf(TEXT("A-EX re-imported configures (%s)"), *ReportAgain.Message), (uint8)ReportAgain.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
		if (!Settle(*this, C.Rig, Rec, TEXT("A-EX re-imported"))) { return false; }
		bOk &= TestEqual(TEXT("A-EX re-imported: no second calibration (the artifact's hash)"), FMr::GetRowCostCalibrationCount(), 1);
		bOk &= TestEqual(TEXT("A-EX re-imported: no calibration call recorded"), CalibrationCallsSinceRecording(C.Rig), 0);
		bOk &= TestEqual(TEXT("A-EX re-imported: no attempt (the cache)"), Rec.Attempts, 0);
		bOk &= RequireRowCost(*this, C.Rig, Rec, First.RowCostMs, TEXT("A-EX re-imported"));
	}
	const FSuperSLMGpuConfigureReport Report15 = C.Rig.Gpu->Configure(Model15, Cfg15);
	if (!TestEqual(*FString::Printf(TEXT("the 1.5B configures (%s)"), *Report15.Message), (uint8)Report15.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	if (!Settle(*this, C.Rig, Rec, TEXT("1.5B"))) { return false; }
	bOk &= TestEqual(TEXT("1.5B: calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	bOk &= TestEqual(TEXT("1.5B: a second calibration (another model)"), FMr::GetRowCostCalibrationCount(), 2);
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, TEXT("A-EX, after the 1.5B"))) { return false; }
	bOk &= TestEqual(TEXT("A-EX, after the 1.5B: no third calibration"), FMr::GetRowCostCalibrationCount(), 2);
	bOk &= RequireRowCost(*this, C.Rig, Rec, First.RowCostMs, TEXT("A-EX, after the 1.5B"));
	return bOk;
}

// R48.5 (ruling 48: failure and noise fall back, never refusing the model). bCalibrateGpuRowCost true; each arm
// forgets the cache, arms the attempts' measured maxima and configures A-EX. Every arm: Configure() succeeds and a
// generation gives the CPU's tokens. A failure ({fail}) and noise twice ({0.5 ms, 200 ms}) fall back: source FallBack,
// 6.2 ms (6,200 us in the chooser, the getter), one and two attempts, and a warning naming GpuRowCost and 6.2 ms.
// Noise then a good measurement ({0.5 ms, as measured} and {200 ms, as measured}) is retried once and used: two
// attempts, the second's price. The top of the range ({124 ms}) is used: 124.0 ms after one attempt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48FallBackTest, "SuperSLM.MultiRow.MR6.R48_5.FailureOrNoiseFallsBackWithAWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48FallBackTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 53: only the product's fallback warning is expected, by its own text, so a failure of this cell (whose texts
	// name GpuRowCost too) is never swallowed.
	// Ruling 56 (6): the framework counts the product's warnings: one per fallback arm (two), exactly.
	AddExpectedError(TEXT("GPU GpuRowCost calibration for"), EAutomationExpectedErrorFlags::Contains, 2);
	struct FArm
	{
		const TCHAR* Name;
		TArray<double> Maxima;
		ESrc Source;
		int32 Attempts;
		double FixedMs; // the row cost when it does not come from the measurement (0: the last attempt's price)
	};
	const FArm Arms[] = {
		{ TEXT("a failed attempt"), { -1.0 }, ESrc::FallBack, 1, 6.2 },
		{ TEXT("noise twice (0.5 ms, 200 ms)"), { 500.0, 200000.0 }, ESrc::FallBack, 2, 6.2 },
		{ TEXT("noise below, then as measured"), { 500.0, 0.0 }, ESrc::Calibrated, 2, 0.0 },
		{ TEXT("noise above, then as measured"), { 200000.0, 0.0 }, ESrc::Calibrated, 2, 0.0 },
		{ TEXT("the top of the range (124 ms)"), { 124000.0 }, ESrc::Calibrated, 1, 124.0 },
	};
	bool bOk = true;
	for (const FArm& Arm : Arms)
	{
		const FString What = Arm.Name;
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		FMr::ForgetRowCostCalibrations();
		if (!TestTrue(*(What + TEXT(": armed")), FMr::ArmRowCostCalibration(*C.Rig.Gpu, Arm.Maxima))) { return false; }
		FLogCapture Log;
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; } // never refused
		FSuperSLMGpuRowCostRecord Rec;
		if (!Settle(*this, C.Rig, Rec, What)) { return false; }
		bOk &= TestEqual(*(What + TEXT(": the source")), (uint8)Rec.Source, (uint8)Arm.Source);
		bOk &= TestEqual(*(What + TEXT(": the attempts")), Rec.Attempts, Arm.Attempts);
		const double Ms = Arm.FixedMs > 0.0 ? Arm.FixedMs : static_cast<double>(RefPriceUs(Rec.TimedRunsUs)) / 1000.0;
		bOk &= RequireRowCost(*this, C.Rig, Rec, Ms, What);
		if (Arm.Source == ESrc::FallBack)
		{
			bOk &= RequireFlatLine(*this, C.Rig, Rec, 6200, What + TEXT(", the fallback (E-14)"));
		}
		bOk &= RequireGenerates(*this, C.Rig, What);
	}
	return bOk;
}

// R48.7 (ruling 48a (4): the 2 s wall-clock timeout). bCalibrateGpuRowCost true; the cache forgotten; the next
// calibration's first attempt forced to time out. Configure() succeeds (the model is not refused); the source is
// FallBack after one attempt (no retry follows a timeout); 6.2 ms is the row cost in the record, the chooser and the
// getter; a warning names GpuRowCost and 6.2 ms; a generation gives the CPU's tokens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48TimeoutTest, "SuperSLM.MultiRow.MR6.R48_7.TimeoutFallsBackWithAWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48TimeoutTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 53: only the product's fallback warning is expected, by its own text (see R48.5).
	AddExpectedError(TEXT("GPU GpuRowCost calibration for"), EAutomationExpectedErrorFlags::Contains, 1); // ruling 56 (6): the fallback's warning, exactly once
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	if (!TestTrue(TEXT("the timeout is armed"), FMr::ArmRowCostCalibrationTimeout(*C.Rig.Gpu))) { return false; }
	FLogCapture Log;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; } // never refused
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R48.7"))) { return false; }
	bool bOk = TestEqual(TEXT("the source is the fallback"), (uint8)Rec.Source, (uint8)ESrc::FallBack);
	bOk &= TestEqual(TEXT("one attempt (no retry after a timeout)"), Rec.Attempts, 1);
	bOk &= RequireRowCost(*this, C.Rig, Rec, 6.2, TEXT("R48.7"));
	bOk &= RequireFlatLine(*this, C.Rig, Rec, 6200, TEXT("R48.7, the fallback (E-14)"));
	bOk &= RequireGenerates(*this, C.Rig, TEXT("R48.7"));
	return bOk;
}

// R48.6 (ruling 48: the switch). bCalibrateGpuRowCost false, no game value: no calibration runs (none counted, no
// calibration call recorded), the source is Default and the row cost is 6.2 ms in the record, the chooser and the
// getter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR48OffTest, "SuperSLM.MultiRow.MR6.R48_6.CalibrationOffIsTheDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR48OffTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget; // ruling 51 F5
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, false, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R48.6"))) { return false; }
	bool bOk = TestEqual(TEXT("the source is the default"), (uint8)Rec.Source, (uint8)ESrc::Default);
	bOk &= RequireRowCost(*this, C.Rig, Rec, 6.2, TEXT("R48.6"));
	bOk &= RequireFlatLine(*this, C.Rig, Rec, 6200, TEXT("R48.6, the default (E-14)"));
	bOk &= TestFalse(TEXT("F is not measured (ruling 56)"), Rec.bFinishMeasured);
	bOk &= TestEqual(TEXT("F is ceil(g(1) x C(1) / 1000) at 6.2 ms (ruling 56)"), Rec.FinishUs, RefDefaultFinishUs(6200));
	bOk &= TestEqual(TEXT("no calibration ran"), FMr::GetRowCostCalibrationCount(), 0);
	bOk &= TestEqual(TEXT("no calibration call was recorded"), CalibrationCallsSinceRecording(C.Rig), 0);
	return bOk;
}

// R50 (ruling 50, amending 48a (5): calibration slices at B > 0 never overshoot B). The shipped 1.5B (L 28; about 10.9
// ms a row on the RX 7900 XT, so ruling 48a's l(1) from the provisional 6.2 ms price ran about 1.76 x over B), B = 2 ms,
// bCalibrateGpuRowCost true, GpuRowCostMs unset, the cache forgotten; recording starts before Configure() and nothing
// else runs until phase 1 settles, so every tick record is the calibration's (ruling 54 grades phase 1; a phase-2 slice
// that shares phase 1's last tick is held to the same budget and is not an attempt). Skipped, with a warning saying
// so, when the 1.5B is absent (SUPERSLM_PAGING_1P5B_PATH, else SUPERSLM_HF_CACHE/superslm_artifacts/
// qwen2.5-1.5b-instruct.sslm: the box kit sets both). Preconditions: the calibration settled Calibrated and its calls
// carry their layers. Then: every calibration call ran 1 layer or more and each tick holds at most one; each attempt's
// first slice is exactly one layer; every tick record's GPU time is at most B (2,000 us) unless the tick ran a single
// layer (one calibration call of one layer) or only the token's finish (it counts as a layer, ruling 50). Ruling 56
// (2): a widened warm-up call counts M row-layers (never exempt). Ruling 57 (F1, F5), 57a: each of the chooser's
// candidate widths ran once or was skipped with a Display line naming it, and the smallest width ran.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR50SlicesTest, "SuperSLM.MultiRow.MR6.R50.CalibrationSlicesNeverOvershootB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR50SlicesTest::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPagingGpuFixtures;
	FString Path15, Reason;
	if (!ResolveArtifact(EGpuArtifact::TwoCap15B, Path15, Reason))
	{
		AddWarning(FString::Printf(TEXT("SKIPPED (ruling 50 needs the 1.5B): %s"), *Reason));
		return true;
	}
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model15 = FSuperSLMModelImport::ImportFromFile(Path15, Diag);
	if (!TestNotNull(TEXT("the 1.5B imports"), Model15) || !TestTrue(TEXT("the 1.5B is accepted"), Diag.bAccepted)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg = MakeGpuConfig(EGpuArtifact::TwoCap15B, 4, 0);
	if (!TestTrue(TEXT("bCalibrateGpuRowCost = true (ruling 48)"), FMr::SetCalibrateGpuRowCost(Cfg, true))) { return false; }
	Cfg.GpuRowCostMs = 0.0; // unset (ruling 48a)
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	FMr::ForgetRowCostCalibrations();
	FLogCapture Log; // rulings 57 F1, 57a: a skipped width's Display line
	FMr::StartRecording(*C.Rig.Gpu);
	const FSuperSLMGpuConfigureReport Report = C.Rig.Gpu->Configure(Model15, Cfg);
	if (!TestEqual(*FString::Printf(TEXT("the 1.5B configures at B = 2 ms (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R50"))) { return false; }
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	if (!TestEqual(TEXT("precondition: the calibration settled on the measured price"), (uint8)Rec.Source, (uint8)ESrc::Calibrated)) { return false; }
	int64 LastCal = MIN_int64;
	const int32 Calls = CalibrationCalls(Records, LastCal);
	if (!TestTrue(*FString::Printf(TEXT("precondition: phase 1 was recorded in slices (%d calls; want more than its 4 tokens, ruling 54)"), Calls), bRecorded && Calls > 4)) { return false; }
	bool bOk = RequireCalibrationSlices(*this, Records, Rec, 2000.0, TEXT("R50"));
	bOk &= RequireWidenedWarmUp(*this, *C.Rig.Gpu, Records, Log, TEXT("R50")); // ruling 56 (2): within B through the budget rule above
	return bOk;
}

// R51.F1 (ruling 51 F1: admission waits for the settled price). A-EX, OneCall, multi-row on, B = 1.5 ms, calibration on,
// the price armed at 11,000 us (11.0 ms: the provisional 6.2 ms plan has Exact on at this B, the settled one has it off,
// poirot-e13.md F1), drafts seeded. An Exact request is begun right after Configure(), before the price settles.
// Preconditions: the price settled Calibrated at 11.0 ms. Then: the request ends, is not faulted, and its tokens equal
// the CPU's Off run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR51F1Test, "SuperSLM.MultiRow.MR6.R51_F1.ExactQueuedDuringCalibrationGivesTheCpuTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR51F1Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	// Ruling 53: the request ends inside its schema, so "not faulted" grades F1 and not the schema's dead end.
	if (!MakeInSchemaRequest(*this, C.Rig, kExact, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 1.5;
	FMr::ForgetRowCostCalibrations();
	if (!TestTrue(TEXT("the price is armed at 11,000 us"), FMr::ArmRowCostCalibration(*C.Rig.Gpu, { 11000.0 }))) { return false; }
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FGpuRun Run;
	if (!Vend(*this, C.Rig, EPath::WholeToken, Run)) { return false; }
	FMr::StartRecording(*C.Rig.Gpu);
	if (!TestTrue(TEXT("the Exact request is begun before the price settles"), C.Rig.Gpu->RequestBeginGeneration(Run.Sequence, Req).IsValid())) { return false; }
	const bool bEnded = Pump(*C.Rig.Gpu, [&]() { return IsEnded(*C.Rig.Gpu, Run.Sequence); });
	Read(C.Rig, Run);
	ReturnAndSettle(C.Rig, Run.Sequence);
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R51.F1"))) { return false; }
	if (!TestEqual(TEXT("precondition: the price settled Calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated) ||
		!TestTrue(*FString::Printf(TEXT("precondition: the price is the armed 11.0 ms (%.4f)"), Rec.RowCostMs), FMath::Abs(Rec.RowCostMs - 11.0) <= 1e-9)) { return false; }
	bool bOk = TestTrue(TEXT("the request ends"), bEnded);
	bOk &= TestNotEqual(TEXT("not faulted"), (uint8)Run.Phase, (uint8)ESuperSLMSequencePhase::Faulted);
	bOk &= TestEqual(TEXT("no fault"), (uint8)Run.Fault, (uint8)ESuperSLMGpuFaultReason::None);
	bOk &= TestEqual(TEXT("tokens equal the CPU's Off run"), Run.Tokens, Cpu.Tokens);
	return bOk;
}

// R51.F2 (ruling 51 F2: the frame-paced R50). The shipped 1.5B, B = 1 ms, calibration on, the row cost unset, the cache
// forgotten; each tick is followed by about 33 ms of sleep (a game at 30 fps, ruling 52), so the calibration's wall time
// is a game's and runs past the old 2 s cap (the B > 0 timeout is 10 s). Skipped, with a warning, when the 1.5B is absent (as R50). Ruling 54 grades
// phase 1 (the early position only, the 10 s cap its own). Then: phase 1 settles on the measured value, not a fallback
// (Source Calibrated), and R50's slice rules hold at B = 1,000 us.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR51F2Test, "SuperSLM.MultiRow.MR6.R51_F2.FramePacedCalibrationSettlesMeasured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR51F2Test::RunTest(const FString& Parameters)
{
	using namespace SuperSLMPagingGpuFixtures;
	FString Path15, Reason;
	if (!ResolveArtifact(EGpuArtifact::TwoCap15B, Path15, Reason))
	{
		AddWarning(FString::Printf(TEXT("SKIPPED (ruling 51 F2 needs the 1.5B): %s"), *Reason));
		return true;
	}
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model15 = FSuperSLMModelImport::ImportFromFile(Path15, Diag);
	if (!TestNotNull(TEXT("the 1.5B imports"), Model15) || !TestTrue(TEXT("the 1.5B is accepted"), Diag.bAccepted)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg = MakeGpuConfig(EGpuArtifact::TwoCap15B, 4, 0);
	if (!TestTrue(TEXT("bCalibrateGpuRowCost = true (ruling 48)"), FMr::SetCalibrateGpuRowCost(Cfg, true))) { return false; }
	Cfg.GpuRowCostMs = 0.0;
	Cfg.SchemaJumpGpuFrameBudgetMs = 1.0;
	FMr::ForgetRowCostCalibrations();
	FMr::StartRecording(*C.Rig.Gpu);
	const FSuperSLMGpuConfigureReport Report = C.Rig.Gpu->Configure(Model15, Cfg);
	if (!TestEqual(*FString::Printf(TEXT("the 1.5B configures at B = 1 ms (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!SettlePaced(*this, C.Rig, Rec, TEXT("R51.F2"))) { return false; }
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	bool bOk = TestEqual(TEXT("the price settled on the measured value, not a fallback"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	int64 LastCal = MIN_int64;
	const int32 Calls = CalibrationCalls(Records, LastCal);
	if (!TestTrue(*FString::Printf(TEXT("precondition: phase 1 was recorded in slices (%d calls; want more than its 4 tokens, ruling 54)"), Calls), bRecorded && Calls > 4)) { return false; }
	bOk &= RequireCalibrationSlices(*this, Records, Rec, 1000.0, TEXT("R51.F2"));
	return bOk;
}

// R51.F4 (ruling 51 F4: the default-configuration smoke). JumpConfig with bCalibrateGpuRowCost and GpuRowCostMs put
// back to FSuperSLMGpuRuntimeConfig's defaults (on, unset), as a game that sets neither: Configure() succeeds, the price
// settles Calibrated, no Warning or Error line names GpuRowCost (ruling 52 N1), and a generation gives the CPU's tokens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR51F4Test, "SuperSLM.MultiRow.MR6.R51_F4.DefaultConfigurationCalibratesQuietly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR51F4Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg = JumpConfig(EPath::WholeToken);
	const FSuperSLMGpuRuntimeConfig Defaults;
	Cfg.bCalibrateGpuRowCost = Defaults.bCalibrateGpuRowCost;
	Cfg.GpuRowCostMs = Defaults.GpuRowCostMs;
	if (!TestTrue(TEXT("precondition: the defaults are calibration on and the row cost unset"), Cfg.bCalibrateGpuRowCost && Cfg.GpuRowCostMs == 0.0)) { return false; }
	FMr::ForgetRowCostCalibrations();
	FLogCapture Log;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R51.F4"))) { return false; }
	bool bOk = TestEqual(TEXT("the price settled on the measured value"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	// Ruling 52 (N1): exact Warning and Error lines only; the price's Log line names GpuRowCost by design.
	bOk &= TestEqual(TEXT("no warning or error names GpuRowCost"),
		Log.Count(ELogVerbosity::Warning, false, { TEXT("GpuRowCost") }) + Log.Count(ELogVerbosity::Error, false, { TEXT("GpuRowCost") }), 0);
	bOk &= RequireGenerates(*this, C.Rig, TEXT("R51.F4"));
	return bOk;
}

// R51.F7 (ruling 51 F7: a retry at B > 0). A-EX, OneCall, B = 2 ms, calibration on, the first attempt armed out of
// range (0.5 ms) and the second as measured. Recording starts before Configure() and nothing else runs until the price
// settles. Then: Calibrated after two attempts, at the second attempt's price; R50's slice rules hold at 2,000 us, the
// retry's first slice one layer again; and no warning names GpuRowCost (ruling 52 N2).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR51F7Test, "SuperSLM.MultiRow.MR6.R51_F7.RetryAtABudgetStartsAgainAtOneLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR51F7Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	FMr::ForgetRowCostCalibrations();
	if (!TestTrue(TEXT("armed: 0.5 ms, then as measured"), FMr::ArmRowCostCalibration(*C.Rig.Gpu, { 500.0, 0.0 }))) { return false; }
	FLogCapture Log;
	FMr::StartRecording(*C.Rig.Gpu);
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R51.F7"))) { return false; }
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	bool bOk = TestEqual(TEXT("Calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	bOk &= TestEqual(TEXT("two attempts"), Rec.Attempts, 2);
	bOk &= RequireRowCost(*this, C.Rig, Rec, static_cast<double>(RefPriceUs(Rec.TimedRunsUs)) / 1000.0, TEXT("R51.F7"));
	bOk &= RequireCalibrationSlices(*this, Records, Rec, 2000.0, TEXT("R51.F7"));
	// Ruling 52 (N2): a retry that succeeds warns about nothing.
	bOk &= TestEqual(TEXT("no warning names GpuRowCost"), Log.Count(ELogVerbosity::Warning, false, { TEXT("GpuRowCost") }), 0);
	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ruling 53 (erratum E-14, triage-run3.md finding 1): the price depends on position, and l(1) carries the margin. The
// calibration times C_a at its early position and C_b at P2 = min(256, ContextCap - 8); C(1, pos) = C_a + max(0, slope)
// x (pos - pos_a); a slice is priced at the sequence's position at its END; the game's value and the 6.2 ms default and
// fallback are flat (slope 0); the cache holds both numbers; sanity applies to C_a and to C(1, cap) <= 124 ms. The pure
// pieces are Private/Core/SuperSLMGpuRowCostCalibration.h and ChooseRowsPlanAt (ci/host/multirow).
// Ruling 54: the two positions are two phases. Phase 1 (C_a; the first generation waits) gives the default line, slope
// s_default = 0.0015 x C_a; phase 2 (B > 0 only, lowest priority, no timeout) measures P2 and replaces s_default, or
// keeps it with one warning ("GPU GpuRowCost late position for") on a failure or a line out of sanity.
// ---------------------------------------------------------------------------------------------------------------------

// E14.1 (ruling 53: flat prices, and l(1) with the margin). A-EX, OneCall, multi-row on. Calibration off (6.2 ms), and
// then the game's 7.3 ms: the line is flat (C(1, pos) the price at 0, 100, 256 and 4,000), and the plan's l(1) at every
// position is min(L, max(1, floor(L x Room x 1000 / (C(1) x (1000 + m))))) at B = 2 ms (6 at 6.2 ms, 5 at 7.3 ms, m =
// 150), and L at B = 0 (no budget, no margin: 1.17's whole token). Ruling 56 (1): F is not measured, so it is
// ceil(g(1) x C(1) / 1000) (298 and 351 us), and the slice that ends a token runs 5, 4 and 24 layers beside it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14FlatTest, "SuperSLM.MultiRow.MR6.E14_1.FlatPricesAndTheMarginOnOneRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14FlatTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	bool bOk = true;
	// Ruling 56 (1): WantFinishL is the slice that ends a token, beside F = ceil(48 x C(1) / 1000) (298 and 351 us):
	// floor((48,000,000 - F x 24 x 1,150) / ((C(1) - F) x 1,150)) at B = 2 ms, L at B = 0.
	struct FArm { const TCHAR* Name; double GameMs; double B; int64 Us; int64 WantL1; int64 WantFinishL; };
	const FArm Arms[] = {
		{ TEXT("the 6.2 ms default, B = 2 ms"), 0.0, 2.0, 6200, 6, 5 },
		{ TEXT("the game's 7.3 ms, B = 2 ms"), 7.3, 2.0, 7300, 5, 4 },
		{ TEXT("the 6.2 ms default, B = 0"), 0.0, 0.0, 6200, 24, 24 },
	};
	for (const FArm& A : Arms)
	{
		const FString What = A.Name;
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!MrConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		Cfg.GpuRowCostMs = A.GameMs;
		Cfg.SchemaJumpGpuFrameBudgetMs = A.B;
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
		FSuperSLMGpuRowCostRecord Rec;
		if (!Settle(*this, C.Rig, Rec, What)) { return false; }
		bOk &= RequireFlatLine(*this, C.Rig, Rec, A.Us, What);
		bOk &= TestEqual(*(What + TEXT(": the worked l(1) agrees with the hand figure")), OneRowLayersAtUs(Cfg, A.Us), A.WantL1);
		const int64 F = RefDefaultFinishUs(A.Us);
		bOk &= TestEqual(*(What + TEXT(": the worked finish slice agrees with the hand figure (ruling 56)")), RefFinishSliceLayers(Cfg, A.Us, F), A.WantFinishL);
		bOk &= TestFalse(*(What + TEXT(": F is not measured (ruling 56)")), Rec.bFinishMeasured);
		bOk &= TestEqual(*(What + TEXT(": F is ceil(g(1) x C(1) / 1000) (ruling 56)")), Rec.FinishUs, F);
		for (const int64 Pos : { int64{0}, int64{100}, int64{4000} })
		{
			bOk &= TestEqual(*FString::Printf(TEXT("%s: l(1) for a slice ending at %lld"), *What, Pos), static_cast<int64>(FMr::GetOneRowLayersAt(*C.Rig.Gpu, Pos)), A.WantL1);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: the slice that ends a token at %lld runs l x (C - F) / L + F within B (ruling 56)"), *What, Pos),
				static_cast<int64>(FMr::GetFinishSliceLayersAt(*C.Rig.Gpu, Pos)), A.WantFinishL);
		}
	}
	return bOk;
}

// E14.2 (ruling 53: the line; ruling 54: in two phases). A-EX, OneCall, multi-row on, B = 2 ms, calibration on, the row
// cost unset, each arm with the cache forgotten and the phases armed apart (ArmRowCostCalibrationPositions: the early
// entries phase 1's attempts, the late entry phase 2). C_a 7.0 ms, C_b 8.2 ms: phase 1 settles Calibrated after one
// attempt with phase 2 pending, the default line in use (C(1, pos) and l(1) on C_a + 0.0015 x C_a x (pos - pos_a)) and
// one "GPU GpuRowCost calibrated for" line naming the model, the adapter and 7.0 ms. A free pump, recorded: phase 2
// settles; every tick that holds a calibration call is at most B (2,000 us) unless its work is a single unit, and at
// most one phase-2 slice; then the measured line is in use (C_b 8.2 ms, P2 = 256, slope 1,200 / (P2 - pos_a), C(1, pos)
// and l(1) on it at pos_a, P2 and 4,000) and one "GPU GpuRowCost line for" line names the model, the adapter, 7.0 ms and
// 8.2 ms. A-EX configured again hits the cache with the measured line: no attempt, phase 2 not pending, and no
// calibration call of either phase in 30 ticks. C_b below C_a (8.0 then 7.5 ms): measured, slope 0, flat at 8.0 ms. Too
// steep at the cap (C_b 60 ms: C(1, 4,096) > 124 ms), and the late position failing: phase 2 keeps s_default (the
// default line, not measured, not pending) and one warning "GPU GpuRowCost late position for" is logged. Phase 1
// failing: the fallback after one attempt, flat at 6.2 ms, phase 2 not pending and no phase-2 call in 30 ticks.
// ForgetRowCostCalibrations() forgets the once-per-model-and-adapter warning too, so each arm warns once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14LineTest, "SuperSLM.MultiRow.MR6.E14_2.TwoPositionLinePricesEachSliceAtItsEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14LineTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 56 (6): the framework counts the product's warnings, exactly: phase 1's fallback once (the phase-1 arm) and
	// phase 2's late-position warning once in each of the two arms that keep s_default.
	AddExpectedError(TEXT("GPU GpuRowCost calibration for"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("GPU GpuRowCost late position for"), EAutomationExpectedErrorFlags::Contains, 2);
	enum class EEnd : uint8 { Measured, Flat, KeepsDefault, FallBack };
	struct FArm { const TCHAR* Name; TArray<double> Early, Late; EEnd End; int64 FlatUs; };
	const FArm Arms[] = {
		{ TEXT("C_a 7.0 ms, C_b 8.2 ms"), { 7000.0 }, { 8200.0 }, EEnd::Measured, 0 },
		{ TEXT("C_b below C_a (8.0, 7.5 ms)"), { 8000.0 }, { 7500.0 }, EEnd::Flat, 8000 },
		{ TEXT("too steep at the cap (7.0, 60 ms)"), { 7000.0 }, { 60000.0 }, EEnd::KeepsDefault, 0 },
		{ TEXT("the late position fails"), { 7000.0 }, { -1.0 }, EEnd::KeepsDefault, 0 },
		{ TEXT("phase 1 fails"), { -1.0 }, { 8200.0 }, EEnd::FallBack, 6200 },
	};
	bool bOk = true;
	for (const FArm& A : Arms)
	{
		const FString What = A.Name;
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
		FMr::ForgetRowCostCalibrations();
		if (!TestTrue(*(What + TEXT(": armed")), FMr::ArmRowCostCalibrationPositions(*C.Rig.Gpu, A.Early, A.Late))) { return false; }
		FLogCapture Log;
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; } // never refused
		FSuperSLMGpuRowCostRecord Rec;
		if (!Settle(*this, C.Rig, Rec, What)) { return false; }
		if (A.End == EEnd::FallBack)
		{
			bOk &= TestEqual(*(What + TEXT(": the fallback")), (uint8)Rec.Source, (uint8)ESrc::FallBack);
			bOk &= TestEqual(*(What + TEXT(": one attempt")), Rec.Attempts, 1);
			bOk &= RequireRowCost(*this, C.Rig, Rec, 6.2, What);
			bOk &= RequireFlatLine(*this, C.Rig, Rec, A.FlatUs, What);
			bOk &= TestFalse(*(What + TEXT(": no phase 2 is pending after a fallback")), Rec.bLatePending);
			FMr::StartRecording(*C.Rig.Gpu);
			PumpTicks(C.Rig, 30);
			bOk &= TestEqual(*(What + TEXT(": no phase-2 call in 30 ticks")), Phase2CallsSinceRecording(C.Rig), 0);
			continue;
		}
		// Phase 1.
		if (!TestEqual(*(What + TEXT(": phase 1 settled Calibrated")), (uint8)Rec.Source, (uint8)ESrc::Calibrated)) { continue; }
		const double CaMs = A.Early[0] / 1000.0;
		bOk &= TestEqual(*(What + TEXT(": one attempt")), Rec.Attempts, 1);
		bOk &= RequireRowCost(*this, C.Rig, Rec, CaMs, What);
		bOk &= TestTrue(*(What + TEXT(": phase 2 is pending (B > 0)")), Rec.bLatePending);
		bOk &= RequireDefaultLine(*this, C.Rig, Rec, &Cfg, What + TEXT(", before phase 2"));
		bOk &= TestEqual(*(What + TEXT(": one phase-1 line names the model, the adapter and C_a")),
			Log.Count(ELogVerbosity::Log, true, { TEXT("GPU GpuRowCost calibrated for"), C.Rig.Cpu.Model->GetName(), Rec.AdapterName, FString::Printf(TEXT("%.1f ms"), CaMs) }), 1);
		// Phase 2, in a free pump.
		FMr::StartRecording(*C.Rig.Gpu);
		FSuperSLMGpuRowCostRecord Late;
		if (!SettleLate(*this, C.Rig, Late, What)) { bOk = false; continue; }
		TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
		FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
		int64 Last = 0;
		bOk &= TestTrue(*(What + TEXT(": precondition: phase 2 was recorded")), CalibrationCalls(Records, Last, true) > 0);
		bOk &= RequireTicksWithinB(*this, Records, {}, 2000.0, true, What + TEXT(", phase 2"));
		bOk &= TestEqual(*(What + TEXT(": still Calibrated")), (uint8)Late.Source, (uint8)ESrc::Calibrated);
		bOk &= RequireRowCost(*this, C.Rig, Late, CaMs, What + TEXT(", after phase 2"));
		if (A.End == EEnd::KeepsDefault)
		{
			bOk &= TestFalse(*(What + TEXT(": phase 2 is no longer pending")), Late.bLatePending);
			bOk &= RequireDefaultLine(*this, C.Rig, Late, &Cfg, What + TEXT(", s_default kept"));
			continue;
		}
		const int64 Cb = static_cast<int64>(A.Late[0]);
		if (A.End == EEnd::Flat)
		{
			bOk &= TestTrue(*(What + TEXT(": the slope is the measured one")), Late.bSlopeMeasured);
			bOk &= RequireFlatLine(*this, C.Rig, Late, A.FlatUs, What);
			continue;
		}
		bOk &= RequireMeasuredLine(*this, C.Rig, Late, Cfg, Cb, What);
		bOk &= TestEqual(*(What + TEXT(": one phase-2 line names the model, the adapter, C_a and C_b")),
			Log.Count(ELogVerbosity::Log, true, { TEXT("GPU GpuRowCost line for"), C.Rig.Cpu.Model->GetName(), Late.AdapterName,
				FString::Printf(TEXT("%.1f ms"), CaMs), FString::Printf(TEXT("%.1f ms"), static_cast<double>(Cb) / 1000.0) }), 1);
		// The cache holds the measured line.
		FSuperSLMGpuRowCostRecord Again;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Again, What + TEXT(", again"))) { return false; }
		FMr::StartRecording(*C.Rig.Gpu);
		PumpTicks(C.Rig, 30);
		bOk &= TestEqual(*(What + TEXT(", again: the cache (no attempt)")), Again.Attempts, 0);
		bOk &= TestFalse(*(What + TEXT(", again: no phase 2 pending (the slope is cached measured)")), Again.bLatePending);
		bOk &= TestEqual(*(What + TEXT(", again: no calibration call of either phase in 30 ticks")), CalibrationCallsSinceRecording(C.Rig), 0);
		bOk &= TestTrue(*(What + TEXT(", again: the slope is the measured one")), Again.bSlopeMeasured);
		bOk &= TestEqual(*(What + TEXT(", again: the same C_a")), Again.RowCostMs, Late.RowCostMs);
		bOk &= TestEqual(*(What + TEXT(", again: the same slope")), Again.SlopeUsPerPosition, Late.SlopeUsPerPosition);
		bOk &= TestEqual(*(What + TEXT(", again: C(1, 4,000) is the same")), FMr::GetRowCostAtUs(*C.Rig.Gpu, 4000),
			RefRowCostAtUs(CaUsOf(Late), Late.AnchorPosition, Cb, RefLatePosition(Cfg.ContextCap), 4000));
	}
	return bOk;
}

// E14.3 (ruling 53: a long-context slice stays within B). The shipped 1.5B (L 28), B = 2 ms, multi-row on,
// LayersPerTick 4, the product's own calibration (on, the row cost unset, the cache forgotten), settled before recording.
// Two arms: one OneCall sequence; a OneCall and a Composed sequence together (Mixed). Each reads a 40-token prompt and
// generates 160 tokens (no stop token), so it runs to about 200 positions. Skipped, with a warning, when the 1.5B is
// absent (as R50). Preconditions: the calibration settled Calibrated; each sequence produced its 160 tokens; the per-
// tick GPU seam recorded the run. Then every tick's GPU time is at most B (2,000 us), unless the tick ran a single unit
// (one row-layer in all). Ruling 54: phase 1 settled is the precondition; phase 2 runs in the room the generations
// leave, and its slices count in the tick's units (TickUnits).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14LongContextTest, "SuperSLM.MultiRow.MR6.E14_3.LongContextSliceWithinBudgetOn15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14LongContextTest::RunTest(const FString& Parameters)
{
	namespace PG = SuperSLMPagingGpuFixtures;
	FString Path15, Reason;
	if (!PG::ResolveArtifact(PG::EGpuArtifact::TwoCap15B, Path15, Reason))
	{
		AddWarning(FString::Printf(TEXT("SKIPPED (E-14's long-context cell needs the 1.5B): %s"), *Reason));
		return true;
	}
	if (!Gate(*this)) { return false; }
	FForgetCalibrationsOnExit Forget;
	PG::FGpuPagingRig Rig;
	if (!Rig.Init(*this, PG::EGpuArtifact::TwoCap15B)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const int32 L = PG::LayersOf(PG::EGpuArtifact::TwoCap15B);
	constexpr int32 kPrompt = 40, kNew = 160;
	TArray<int32> Prompt;
	if (!PG::MakeTokens(*this, *Rig.Model, kPrompt, 53, Prompt)) { return false; }
	bool bOk = true;
	for (const bool bMixed : { false, true })
	{
		const FString Arm = bMixed ? TEXT("Mixed (OneCall + Composed)") : TEXT("OneCall");
		const int32 BlockCount = bMixed ? 2 : 1;
		FSuperSLMGpuRuntimeConfig Cfg = PG::MakeGpuConfig(PG::EGpuArtifact::TwoCap15B, BlockCount, 0);
		Cfg.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(4);
		Cfg.K = FMath::Max3(L, BlockCount + 1, (BlockCount * L + 3) / 4);
		Cfg.GpuRowCostMs = 0.0;
		Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
		if (!TestTrue(TEXT("bGpuMultiRow is set"), FMr::SetMultiRow(Cfg, true)) || !TestTrue(TEXT("bCalibrateGpuRowCost is set"), FMr::SetCalibrateGpuRowCost(Cfg, true))) { return false; }
		FMr::ForgetRowCostCalibrations();
		const FSuperSLMGpuConfigureReport Report = Gpu.Configure(Rig.Model, Cfg);
		if (!TestEqual(*FString::Printf(TEXT("%s: the 1.5B configures at B = 2 ms (%s)"), *Arm, *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
		Rig.Config = Cfg;
		const bool bSettled = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&Gpu]() { return !FMr::IsRowCostCalibrating(Gpu); });
		FSuperSLMGpuRowCostRecord Rec;
		if (!TestTrue(*(Arm + TEXT(": the calibration settles")), bSettled) || !TestTrue(TEXT("the row-cost record is readable"), FMr::GetRowCost(Gpu, Rec))) { return false; }
		if (!TestEqual(*FString::Printf(TEXT("%s: precondition: the product's phase 1 settled (C_a %.3f ms, slope %.4f us/position)"), *Arm, Rec.RowCostMs, Rec.SlopeUsPerPosition),
			(uint8)Rec.Source, (uint8)ESrc::Calibrated)) { return false; }
		TArray<FSuperSLMGpuSequence> Seqs;
		const ESuperSLMGpuDecodePath Paths[2] = { ESuperSLMGpuDecodePath::OneCall, ESuperSLMGpuDecodePath::Composed };
		for (int32 I = 0; I < BlockCount; ++I)
		{
			FSuperSLMGpuSequence S;
			if (!PG::Vend(*this, Gpu, S, Paths[I])) { return false; }
			Seqs.Add(S);
		}
		FSuperSLMGenerationRequest Req;
		Req.PromptTokens = Prompt;
		Req.SpanKind = ESuperSLMSpanKind::Prompt;
		Req.MaxNewTokens = kNew;
		FMr::StartRecording(Gpu);
		for (const FSuperSLMGpuSequence& S : Seqs)
		{
			if (!TestTrue(*(Arm + TEXT(": RequestBeginGeneration")), Gpu.RequestBeginGeneration(S, Req).IsValid())) { return false; }
		}
		const bool bEnded = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&]()
		{
			for (const FSuperSLMGpuSequence& S : Seqs) { if (!IsEnded(Gpu, S)) { return false; } }
			return true;
		});
		PG::DrainQuiet(Gpu);
		TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
		const bool bRecorded = FMr::GetTickGpuBusyUs(Gpu, Records);
		const TArray<FAct> Acts = FMr::GetActions(Gpu);
		bool bProduced = true;
		for (const FSuperSLMGpuSequence& S : Seqs)
		{
			const int32 N = Gpu.GetGeneratedTokens(S).Num();
			bProduced &= TestEqual(*FString::Printf(TEXT("%s: precondition: a sequence produced its %d tokens, to position %d"), *Arm, kNew, kPrompt + N), N, kNew);
			PG::ReturnAndDrain(Gpu, S);
		}
		if (!TestTrue(*(Arm + TEXT(": every sequence ends")), bEnded) || !bProduced) { bOk = false; continue; }
		if (!TestTrue(*FString::Printf(TEXT("%s: precondition: the per-tick GPU seam recorded the run (%d records)"), *Arm, Records.Num()), bRecorded && Records.Num() > 0)) { bOk = false; continue; }
		const TMap<int64, int64> RowLayers = RowLayersByTick(Acts);
		int32 Over = 0;
		double Max = 0.0;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			Max = FMath::Max(Max, R.GpuBusyUs);
			const int64 Units = TickUnits(R, RowLayers);
			if (R.GpuBusyUs <= 2000.0 || Units <= 1) { continue; }
			++Over;
			bOk &= TestTrue(*FString::Printf(TEXT("%s, tick %lld: %.1f us of GPU time, at most B (2,000 us), or a single unit (%lld row-layers)"), *Arm, R.Tick, R.GpuBusyUs, Units), false);
		}
		AddInfo(FString::Printf(TEXT("%s: %d ticks, max %.1f us, %d over B; C_a %.3f ms, slope %.4f us/position from %lld"), *Arm, Records.Num(), Max, Over, Rec.RowCostMs,
			Rec.SlopeUsPerPosition, Rec.AnchorPosition));
	}
	return bOk;
}

// E14.4 (ruling 54: generations interleaved with phase 2). A-EX, multi-row on, B = 2 ms, 4 layers per slice,
// calibration on, the row cost unset, the cache forgotten. Phase 1 settles (Calibrated, phase 2 pending); recording
// starts and an Off generation is begun at once on a OneCall and on a Composed sequence. While phase 2 is pending, at
// every tick C(1, 256) is the default line's. Preconditions: phase 2 was still pending when the generations began, and
// some tick holds both a phase-2 slice and a generation action. Then: both sequences end with the CPU's Off tokens;
// every recorded tick is at most B (2,000 us) unless its work (row-layers, calibration layers and finishes) is a single
// unit, and holds at most one phase-2 slice. Then a free pump: phase 2 settles, and the measured line is in use (C_b the
// late runs' price, C(1, pos) and l(1) on it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14InterleavedTest, "SuperSLM.MultiRow.MR6.E14_4.GenerationsInterleaveWithPhase2WithinB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14InterleavedTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGenerationRequest Req;
	FRunResult Cpu;
	if (!MakeRequest(*this, C.Rig, kOff, kMr6MaxNew, Req) || !RunCpu(*this, C.Rig, Req, kOff, Cpu)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::Sliced4, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	FMr::ForgetRowCostCalibrations();
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("E14.4"))) { return false; }
	if (!TestEqual(TEXT("precondition: phase 1 settled Calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated) ||
		!TestTrue(TEXT("precondition: phase 2 is pending when the generations begin"), Rec.bLatePending)) { return false; }
	const int64 Ca = CaUsOf(Rec), DefaultAt256 = RefDefaultRowCostAtUs(Ca, Rec.AnchorPosition, 256);
	FGpuRun Runs[2];
	const EPath Paths[2] = { EPath::WholeToken, EPath::Sliced4 };
	for (int32 I = 0; I < 2; ++I) { if (!Vend(*this, C.Rig, Paths[I], Runs[I])) { return false; } }
	FMr::StartRecording(*C.Rig.Gpu);
	for (int32 I = 0; I < 2; ++I)
	{
		if (!TestTrue(*FString::Printf(TEXT("%s: RequestBeginGeneration while phase 2 is pending"), PathName(Paths[I])), C.Rig.Gpu->RequestBeginGeneration(Runs[I].Sequence, Req).IsValid())) { return false; }
	}
	int32 Sampled = 0, OffLine = 0;
	const bool bEnded = Pump(*C.Rig.Gpu, [&]()
	{
		FSuperSLMGpuRowCostRecord Now;
		if (FMr::GetRowCost(*C.Rig.Gpu, Now) && Now.bLatePending)
		{
			++Sampled;
			OffLine += FMr::GetRowCostAtUs(*C.Rig.Gpu, 256) == DefaultAt256 ? 0 : 1;
		}
		return IsEnded(*C.Rig.Gpu, Runs[0].Sequence) && IsEnded(*C.Rig.Gpu, Runs[1].Sequence);
	});
	for (FGpuRun& R : Runs) { Read(C.Rig, R); }
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	const TArray<FAct> Acts = FMr::GetActions(*C.Rig.Gpu);
	// Ruling 56 (3): on a stall the cell says where it stopped: each sequence's phase, tokens, pending ids and open
	// chunk; the device gate and phase 2; the last recorded tick's calls; the last actions.
	FString State;
	if (!bEnded)
	{
		for (int32 I = 0; I < 2; ++I)
		{
			State += FString::Printf(TEXT(" [%s: phase %d, %d tokens, %d pending ids, chunk open %d]"), PathName(Paths[I]), (int32)Runs[I].Phase, Runs[I].Tokens.Num(),
				FMr::PendingResultIds(*C.Rig.Gpu, Runs[I].Sequence), FMr::IsChunkOpen(*C.Rig.Gpu, Runs[I].Sequence) ? 1 : 0);
		}
		FSuperSLMGpuRowCostRecord Now;
		const bool bRead = FMr::GetRowCost(*C.Rig.Gpu, Now);
		State += FString::Printf(TEXT(" [gated on the device %d; phase 2 pending %d]"), SuperSLML2S2Fixtures::NextTickGatedOnDevice(*C.Rig.Gpu) ? 1 : 0, bRead && Now.bLatePending ? 1 : 0);
		if (Records.Num() > 0)
		{
			const FSuperSLMGpuTickGpuBusyRecord& R = Records.Last();
			State += FString::Printf(TEXT(" [last recorded tick %lld, %.1f us:"), R.Tick, R.GpuBusyUs);
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				State += FString::Printf(TEXT(" kind %d rows %d layers %d attempt %d;"), (int32)K.Kind, K.Rows, K.Layers, K.CalibrationAttempt);
			}
			State += TEXT("]");
		}
		for (int32 I = FMath::Max(0, Acts.Num() - 6); I < Acts.Num(); ++I)
		{
			const FAct& A = Acts[I];
			State += FString::Printf(TEXT(" [action %d, sequence %lld, tick %lld: kind %d, rows %d, layers %d+%d]"), A.ActionIndex, A.SequenceId, A.Tick, (int32)A.Kind, A.Rows, A.FirstLayer, A.Layers);
		}
		AddInfo(TEXT("E14.4 stalled:") + State);
	}
	for (FGpuRun& R : Runs) { ReturnAndSettle(C.Rig, R.Sequence); }
	if (!TestTrue(*FString::Printf(TEXT("both generations end%s"), bEnded ? TEXT("") : *(TEXT(" (stalled:") + State + TEXT(")"))), bEnded) ||
		!TestTrue(TEXT("precondition: the per-tick GPU seam recorded the run"), bRecorded && Records.Num() > 0)) { return false; }
	const TMap<int64, int64> RowLayers = RowLayersByTick(Acts);
	int32 Shared = 0;
	for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
	{
		bool bPhase2 = false;
		for (const FSuperSLMGpuTickCallRecord& K : R.Calls) { bPhase2 |= K.Kind == ESuperSLMGpuTickCallKind::Calibration && K.CalibrationAttempt == kPhase2Attempt; }
		Shared += bPhase2 && RowLayers.FindRef(R.Tick) > 0 ? 1 : 0;
	}
	if (!TestTrue(*FString::Printf(TEXT("precondition: phase-2 slices shared ticks with the generations (%d ticks)"), Shared), Shared > 0)) { return false; }
	bool bOk = TestEqual(*FString::Printf(TEXT("while phase 2 was pending, C(1, 256) was the default line's %lld us at every tick (%d sampled)"), DefaultAt256, Sampled), OffLine, 0);
	for (int32 I = 0; I < 2; ++I)
	{
		bOk &= TestEqual(*FString::Printf(TEXT("%s: tokens equal the CPU's Off run"), PathName(Paths[I])), Runs[I].Tokens, Cpu.Tokens);
	}
	bOk &= RequireTicksWithinB(*this, Records, Acts, 2000.0, false, TEXT("E14.4"));
	FSuperSLMGpuRowCostRecord Late;
	if (!SettleLate(*this, C.Rig, Late, TEXT("E14.4"))) { return false; }
	bOk &= TestEqual(TEXT("C_b is the late runs' price"), Late.LateRowCostUs, RefPriceUs(Late.LateTimedRunsUs));
	bOk &= RequireMeasuredLine(*this, C.Rig, Late, Cfg, Late.LateRowCostUs, TEXT("E14.4, after phase 2"));
	return bOk;
}

// E14.5 (ruling 55 F1: an Exact chunk's delivered drafts are counted once in the priced position). A-EX, OneCall,
// multi-row on, B = 2 ms, calibration on, the row cost unset, the cache forgotten; phase 1 and then phase 2 settle in a
// free pump before the run (the measured line in use). An Exact request on the pinned prompt, with MaxNew set so the
// run reaches about 150 positions and kept inside its schema (ruling 53; ruling 56: the pinned prompt lengthened by
// LongPrompt() copies until the reach is 120, and at least 100), drafts seeded from the CPU's Off run.
// While it runs, at every tick where the sequence has produced a token, has no chunk open and no result pending, the
// priced next-row position (GetNextRowPosition) is the prompt's length + the tokens produced - 1. Preconditions: the
// run reaches 100 positions or more, an Exact close delivered 2 tokens or more, and a sample was taken after it. Then:
// the positions agree at every sample; the last third of the sequence's actions holds a verified ExactRows slice; the
// tokens equal the CPU's Off run; every recorded tick is at most B (2,000 us) unless its work is a single unit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14ExactPositionTest, "SuperSLM.MultiRow.MR6.E14_5.ExactDraftsCountedOnceInThePricedPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14ExactPositionTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 56 (6): the pinned prompt's schema ends at 39 positions, so the prompt is lengthened: the pinned prompt
	// after 0 to 3 copies of LongPrompt(), the first whose in-schema run reaches 120 positions, else the longest reach,
	// which must be 100 or more. PromptLen is the chosen request's own.
	constexpr int32 kPositions = 150;
	// Ruling 59d point 6: at B = 2 ms no Exact chunk fits at this reach (the long-context price, run 5: 0 Exact opens in 823
	// actions), so the cell's case never happened; at 4 ms Exact runs.
	constexpr double kE145BudgetMs = 4.0;
	FSuperSLMGenerationRequest Req;
	int32 Reach = -1;
	FString Tried;
	for (int32 Pad = 0; Pad <= 3; ++Pad)
	{
		FString Prompt;
		for (int32 I = 0; I < Pad; ++I) { Prompt += LongPrompt() + TEXT(" "); }
		Prompt += SuperSLML2S2Fixtures::PinnedSelfCheckPrompt();
		FSuperSLMGenerationRequest Probe, Cand;
		if (!MakeRequest(*this, C.Rig, kExact, 1, Probe, Prompt) ||
			!MakeInSchemaRequest(*this, C.Rig, kExact, Cand, Prompt, FMath::Max(8, kPositions - Probe.PromptTokens.Num()))) { return false; }
		const int32 CandReach = Cand.PromptTokens.Num() + Cand.MaxNewTokens;
		Tried += FString::Printf(TEXT(" [%d copies: prompt %d, MaxNew %d, reach %d]"), Pad, Cand.PromptTokens.Num(), Cand.MaxNewTokens, CandReach);
		if (CandReach > Reach) { Reach = CandReach; Req = Cand; }
		if (CandReach >= 120) { break; }
	}
	const int32 PromptLen = Req.PromptTokens.Num();
	AddInfo(FString::Printf(TEXT("E14.5's prompt (ruling 56):%s"), *Tried));
	if (!TestTrue(*FString::Printf(TEXT("precondition: the run reaches 100 positions or more (%d;%s)"), Reach, *Tried), Reach >= 100)) { return false; }
	FRunResult Cpu;
	if (!RunCpu(*this, C.Rig, Req, kOff, Cpu) || !SeedExactDrafts(*this, C.Rig, Req)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = kE145BudgetMs; // ruling 59d point 6: a budget where an Exact chunk fits at this reach
	FMr::ForgetRowCostCalibrations();
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec, Late;
	if (!Settle(*this, C.Rig, Rec, TEXT("E14.5")) || !SettleLate(*this, C.Rig, Late, TEXT("E14.5"))) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: phase 2 settled on the measured line (C_a %.3f ms, slope %.4f us/position)"), Late.RowCostMs, Late.SlopeUsPerPosition),
		Late.Source == ESrc::Calibrated && Late.bSlopeMeasured)) { return false; }
	FGpuRun Run;
	if (!Vend(*this, C.Rig, EPath::WholeToken, Run)) { return false; }
	FMr::StartRecording(*C.Rig.Gpu);
	if (!TestTrue(TEXT("RequestBeginGeneration (Exact)"), C.Rig.Gpu->RequestBeginGeneration(Run.Sequence, Req).IsValid())) { return false; }
	int32 Samples = 0, Disagree = 0, MaxSampled = 0;
	FString FirstDisagreement;
	const bool bEnded = Pump(*C.Rig.Gpu, [&]() { return IsEnded(*C.Rig.Gpu, Run.Sequence); }, kRunSeconds);
	{
		// Ruling 59d point 6: sampled where the case is exact -- right after each Exact close applies. The planner held the
		// sequence for that close, so nothing else of it is in flight: the priced next-row position is the committed rows'
		// (the prompt's length + the tokens produced - 1), and the Exact bound the chunk counted is spent (0). The pump's own
		// ticks almost never see such a moment: the next chunk opens in the tick the close applies.
		TArray<FIntVector4> ApplySamples;
		FMr::GetExactApplySamples(*C.Rig.Gpu, Run.Sequence.Id, ApplySamples);
		for (const FIntVector4& P : ApplySamples)
		{
			const int32 N = P.X;
			if (N < 1 || P.W != 0) { continue; }
			const int64 Want = static_cast<int64>(PromptLen) + N - 1;
			++Samples;
			MaxSampled = FMath::Max(MaxSampled, N);
			if (P.Y != Want || P.Z != 0)
			{
				++Disagree;
				if (FirstDisagreement.IsEmpty()) { FirstDisagreement = FString::Printf(TEXT("after %d tokens: priced at %d, the next row is %lld, the Exact bound %d"), N, P.Y, Want, P.Z); }
			}
		}
	}
	Read(C.Rig, Run);
	const TArray<FAct> Acts = ActionsOf(*C.Rig.Gpu, Run.Sequence.Id);
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(*C.Rig.Gpu, Records);
	ReturnAndSettle(C.Rig, Run.Sequence);
	if (!TestTrue(TEXT("the generation ends"), bEnded) || !TestTrue(TEXT("precondition: the per-tick GPU seam recorded the run"), bRecorded && Records.Num() > 0)) { return false; }
	// The tokens produced through the first Exact close that delivered 2 or more.
	int32 Through = 0, FirstClose = -1;
	for (const FAct& A : Acts)
	{
		Through += A.DeliveredTokens.Num();
		if (A.Kind == EKind::ExactRows && A.bClosedChunk && !A.bFellBack && A.DeliveredTokens.Num() >= 2) { FirstClose = Through; break; }
	}
	{
		// Ruling 59d point 6's diagnosis: what the run's Exact chunks did.
		int32 Opens = 0, Closes = 0, FellBackN = 0;
		FString Delivered;
		for (const FAct& A : Acts)
		{
			if (A.Kind != EKind::ExactRows) { continue; }
			Opens += A.bOpenedChunk ? 1 : 0;
			if (A.bClosedChunk) { ++Closes; FellBackN += A.bFellBack ? 1 : 0; Delivered += FString::Printf(TEXT("%s%d"), Delivered.IsEmpty() ? TEXT("") : TEXT(","), A.DeliveredTokens.Num()); }
		}
		AddInfo(FString::Printf(TEXT("E14.5 diagnosis: %d actions, %d Exact opens, %d closes (%d fell back), delivered per close [%s], %d tokens"), Acts.Num(), Opens, Closes, FellBackN, *Delivered, Run.Tokens.Num()));
	}
	if (!TestTrue(TEXT("precondition: an Exact close delivered 2 tokens or more"), FirstClose > 0) ||
		!TestTrue(*FString::Printf(TEXT("precondition: a position was sampled after it (%d samples, up to %d tokens; the close ends at %d)"), Samples, MaxSampled, FirstClose), MaxSampled >= FirstClose)) { return false; }
	bool bOk = TestEqual(*FString::Printf(TEXT("the priced next-row position is the committed rows' at every sample (%d samples; %s)"), Samples,
		FirstDisagreement.IsEmpty() ? TEXT("all agree") : *FirstDisagreement), Disagree, 0);
	int32 LateExact = 0;
	for (int32 I = (2 * Acts.Num()) / 3; I < Acts.Num(); ++I)
	{
		const FAct& A = Acts[I];
		LateExact += A.Kind == EKind::ExactRows && A.Rows >= 2 && A.Layers > 0 && !A.bFellBack ? 1 : 0;
	}
	bOk &= TestTrue(*FString::Printf(TEXT("the last third of the run's %d actions holds verified ExactRows slices (%d)"), Acts.Num(), LateExact), LateExact > 0);
	bOk &= TestEqual(TEXT("tokens equal the CPU's Off run"), Run.Tokens, Cpu.Tokens);
	bOk &= RequireTicksWithinB(*this, Records, Acts, kE145BudgetMs * 1000.0, false, TEXT("E14.5"));
	return bOk;
}

// E14.6 (ruling 55 F2, keeping 54a.1: a failed phase 2 is not retried on a cache hit). A-EX, OneCall, multi-row on,
// B = 2 ms, calibration on, the row cost unset. Two arms, each with the cache forgotten and the phases armed apart: the
// late position failing, and too steep at the cap (C_b 60 ms, ContextCap 4,096). Phase 1 and phase 2 settle (a free
// pump); precondition: s_default kept, with one "GPU GpuRowCost late position for" warning. Then, with no
// ForgetRowCostCalibrations(), A-EX configured again: a cache hit (no attempt), phase 2 not pending, no calibration
// call of either phase in 30 ticks, the default line in use, and no second warning. Then ForgetRowCostCalibrations()
// and a Configure: phase 1 runs (one attempt) and phase 2 is pending again. A third arm (the cap re-checked on a hit):
// C_a 7.0 ms and C_b 60 ms measured at ContextCap 264 (P2 = 256, C(1, 264) within 124 ms), then A-EX configured at
// ContextCap 4,096 with no forget: a cache hit (no attempt) whose line fails C(1, 4,096) <= 124 ms, so s_default is in
// use (not measured, slope 0.0015 x C_a, C(1, 4,000) on the default line), with one warning naming GpuRowCost.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14FailedLateTest, "SuperSLM.MultiRow.MR6.E14_6.FailedPhase2IsNotRetriedOnACacheHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14FailedLateTest::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	// Ruling 56 (6): the framework counts the product's warnings, exactly: one late-position warning in each failure arm
	// (none on the hit that follows) and one on the cap arm's hit.
	AddExpectedError(TEXT("GPU GpuRowCost late position for"), EAutomationExpectedErrorFlags::Contains, 3);
	struct FArm { const TCHAR* Name; TArray<double> Early, Late; };
	const FArm Arms[] = {
		{ TEXT("the late position fails"), { 7000.0 }, { -1.0 } },
		{ TEXT("too steep at the cap (7.0, 60 ms)"), { 7000.0 }, { 60000.0 } },
	};
	bool bOk = true;
	for (const FArm& A : Arms)
	{
		const FString What = A.Name;
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
		FMr::ForgetRowCostCalibrations();
		if (!TestTrue(*(What + TEXT(": armed")), FMr::ArmRowCostCalibrationPositions(*C.Rig.Gpu, A.Early, A.Late))) { return false; }
		FLogCapture Log;
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
		FSuperSLMGpuRowCostRecord Rec, Late;
		if (!Settle(*this, C.Rig, Rec, What) || !SettleLate(*this, C.Rig, Late, What)) { return false; }
		if (!TestTrue(*(What + TEXT(": precondition: phase 2 failed and kept s_default")), Late.Source == ESrc::Calibrated && !Late.bSlopeMeasured && !Late.bLatePending)) { bOk = false; continue; }
		// The same key again, no forget.
		FMr::StartRecording(*C.Rig.Gpu);
		FSuperSLMGpuRowCostRecord Again;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Again, What + TEXT(", again"))) { return false; }
		PumpTicks(C.Rig, 30);
		FSuperSLMGpuRowCostRecord After;
		if (!TestTrue(*(What + TEXT(", again: the record is readable after 30 ticks")), FMr::GetRowCost(*C.Rig.Gpu, After))) { return false; }
		bOk &= TestEqual(*(What + TEXT(", again: a cache hit (no attempt)")), Again.Attempts, 0);
		bOk &= TestFalse(*(What + TEXT(", again: phase 2 is not pending (the failure is cached, ruling 55 F2)")), Again.bLatePending || After.bLatePending);
		bOk &= TestEqual(*(What + TEXT(", again: no calibration call of either phase in 30 ticks")), CalibrationCallsSinceRecording(C.Rig), 0);
		bOk &= RequireDefaultLine(*this, C.Rig, After, &Cfg, What + TEXT(", again"));
		// ForgetRowCostCalibrations() clears it.
		FMr::ForgetRowCostCalibrations();
		FSuperSLMGpuRowCostRecord Fresh;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Fresh, What + TEXT(", after a forget"))) { return false; }
		bOk &= TestEqual(*(What + TEXT(", after a forget: phase 1 runs")), Fresh.Attempts, 1);
		bOk &= TestTrue(*(What + TEXT(", after a forget: phase 2 is pending again")), Fresh.bLatePending);
	}
	{
		const FString What = TEXT("the cap re-checked on a hit");
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
		Cfg.ContextCap = 264;
		FMr::ForgetRowCostCalibrations();
		if (!TestTrue(*(What + TEXT(": armed (7.0, 60 ms)")), FMr::ArmRowCostCalibrationPositions(*C.Rig.Gpu, { 7000.0 }, { 60000.0 }))) { return false; }
		if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
		FSuperSLMGpuRowCostRecord Rec, Late;
		if (!Settle(*this, C.Rig, Rec, What) || !SettleLate(*this, C.Rig, Late, What)) { return false; }
		if (!TestTrue(*(What + TEXT(": precondition: measured at ContextCap 264 (C(1, 264) within 124 ms)")), Late.bSlopeMeasured)) { return false; }
		FLogCapture Log;
		Cfg.ContextCap = 4096;
		FSuperSLMGpuRowCostRecord Hit;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Hit, What + TEXT(", at 4,096"))) { return false; }
		const int64 Ca = CaUsOf(Hit);
		bOk &= TestEqual(*(What + TEXT(", at 4,096: a cache hit (no attempt)")), Hit.Attempts, 0);
		bOk &= TestFalse(*(What + TEXT(", at 4,096: the measured line is not used (C(1, 4,096) > 124 ms)")), Hit.bSlopeMeasured);
		bOk &= TestTrue(*FString::Printf(TEXT("%s, at 4,096: the slope %.6f is s_default (%.6f)"), *What, Hit.SlopeUsPerPosition, 0.0015 * static_cast<double>(Ca)),
			FMath::Abs(Hit.SlopeUsPerPosition - 0.0015 * static_cast<double>(Ca)) <= 1e-9);
		bOk &= TestEqual(*(What + TEXT(", at 4,096: C(1, 4,000) is on the default line")), FMr::GetRowCostAtUs(*C.Rig.Gpu, 4000), RefDefaultRowCostAtUs(Ca, Hit.AnchorPosition, 4000));
	}
	return bOk;
}

namespace
{
	// Ruling 60 (5): the delay hook, cleared when the cell ends (it stays armed across Configure() calls otherwise).
	struct FRunDelayOnExit
	{
		~FRunDelayOnExit() { FMr::ArmRowCostRunDelay(1, 0u, 0.0); FMr::ArmRowCostRunDelay(2, 0u, 0.0); }
	};

	uint32 RunBits(std::initializer_list<int32> Bits)
	{
		uint32 M = 0;
		for (const int32 B : Bits) { M |= 1u << B; }
		return M;
	}

	FString RunsOf(const TArray<double>& Runs)
	{
		FString S;
		for (const double R : Runs) { S += FString::Printf(TEXT("%s%.1f"), S.IsEmpty() ? TEXT("") : TEXT(", "), R); }
		return S;
	}

	// Ruling 60: a set with no run over 3 x its median.
	bool NoOutlier(const TArray<double>& Runs)
	{
		TArray<double> S = Runs;
		S.Sort();
		if (S.Num() == 0) { return false; }
		const double Median = (S.Num() & 1) != 0 ? S[S.Num() / 2] : 0.5 * (S[S.Num() / 2 - 1] + S[S.Num() / 2]);
		for (const double R : S) { if (R > 3.0 * Median) { return false; } }
		return true;
	}

	// Ruling 60: a phase-2 configuration (A-EX, OneCall, multi-row on, B = 2 ms, calibration on, the row cost unset).
	bool Phase2Config(FAutomationTestBase& T, FSuperSLMGpuRuntimeConfig& Cfg)
	{
		if (!CalConfig(T, EPath::WholeToken, true, Cfg)) { return false; }
		Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
		return true;
	}

	// Ruling 60: the measured line through (pos_a, C_a) and (the late position the record names, C_b), and its kept runs priced.
	bool RequireLineThroughLate(FAutomationTestBase& T, const FGpuRig& Rig, const FSuperSLMGpuRowCostRecord& Late, const FString& What)
	{
		const int64 Ca = CaUsOf(Late);
		bool bOk = T.TestTrue(*(What + TEXT(": the slope is the measured one")), Late.bSlopeMeasured && !Late.bLatePending);
		bOk &= T.TestEqual(*(What + TEXT(": 3 kept late runs")), Late.LateTimedRunsUs.Num(), 3);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: C_b is the kept runs' price ([%s] us)"), *What, *RunsOf(Late.LateTimedRunsUs)), Late.LateRowCostUs, RefPriceUs(Late.LateTimedRunsUs));
		bOk &= T.TestTrue(*FString::Printf(TEXT("%s: the kept late runs hold no outlier ([%s] us)"), *What, *RunsOf(Late.LateTimedRunsUs)), NoOutlier(Late.LateTimedRunsUs));
		if (!T.TestTrue(*FString::Printf(TEXT("%s: the late position %lld is past pos_a %lld"), *What, Late.LatePosition, Late.AnchorPosition), Late.LatePosition > Late.AnchorPosition && Late.AnchorPosition >= 0)) { return false; }
		bOk &= T.TestEqual(*(What + TEXT(": C(1, 4,000) is on the line through the late position")), FMr::GetRowCostAtUs(*Rig.Gpu, 4000),
			RefRowCostAtUs(Ca, Late.AnchorPosition, Late.LateRowCostUs, Late.LatePosition, 4000));
		return bOk;
	}
}

// R60_1 (ruling 60 (1), (3), (5): one outlier in phase 1's set). A-EX, OneCall, multi-row on, B = 0, calibration on, the
// row cost unset, the cache forgotten; the hook adds 100 ms to phase 1's second timed run. Then: one attempt, Calibrated;
// one dropped run (the delayed one, 100 ms or more); 3 kept runs, none over 3 x their median and every one under 100 ms;
// C_a their price, in use; no spread failure; the drop is logged at Display (never Warning).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR601Test, "SuperSLM.MultiRow.MR6.R60_1.Phase1OutlierDroppedAndRetimed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR601Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FRunDelayOnExit Clear;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	constexpr double kDelayUs = 100000.0;
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	if (!TestTrue(TEXT("the delay hook is armed"), FMr::ArmRowCostRunDelay(1, RunBits({1}), kDelayUs))) { return false; }
	FLogCapture Log;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("R60.1"))) { return false; }
	bool bOk = TestEqual(TEXT("Calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated);
	bOk &= TestEqual(TEXT("one attempt"), Rec.Attempts, 1);
	bOk &= TestEqual(*FString::Printf(TEXT("one run dropped ([%s] us)"), *RunsOf(Rec.DroppedRunsUs)), Rec.DroppedRunsUs.Num(), 1);
	bOk &= TestTrue(TEXT("the dropped run is the delayed one"), Rec.DroppedRunsUs.Num() == 1 && Rec.DroppedRunsUs[0] >= kDelayUs);
	bOk &= TestEqual(TEXT("3 kept runs"), Rec.TimedRunsUs.Num(), 3);
	for (const double R : Rec.TimedRunsUs) { bOk &= TestTrue(*FString::Printf(TEXT("kept run %.1f us is a normal one"), R), R > 0.0 && R < kDelayUs); }
	bOk &= TestTrue(*FString::Printf(TEXT("the kept runs hold no outlier ([%s] us)"), *RunsOf(Rec.TimedRunsUs)), NoOutlier(Rec.TimedRunsUs));
	bOk &= RequireRowCost(*this, C.Rig, Rec, static_cast<double>(RefPriceUs(Rec.TimedRunsUs)) / 1000.0, TEXT("R60.1: C_a is the max of the kept runs"));
	bOk &= TestEqual(TEXT("no spread failure"), Rec.SpreadFailures, 0);
	bOk &= TestEqual(TEXT("the drop is logged at Display"), Log.Count(ELogVerbosity::Display, false, { TEXT("GPU GpuRowCost for"), TEXT("phase 1's runs"), TEXT("dropped") }), 1);
	bOk &= TestEqual(TEXT("no GpuRowCost warning"), Log.Count(ELogVerbosity::Warning, false, { TEXT("GpuRowCost") }), 0);
	bOk &= RequireGenerates(*this, C.Rig, TEXT("R60.1"));
	return bOk;
}

// R60_2 (ruling 60 (1), (5): one outlier in phase 2's set). A-EX, OneCall, multi-row on, B = 2 ms, calibration on, the
// row cost unset, the cache forgotten; the hook adds 100 ms to phase 2's second timed run. Phase 1 and phase 2 settle in
// a free pump. Then: the slope is measured; one late run dropped (the delayed one); 3 kept late runs, none an outlier,
// C_b their price; the late position past P2 (the replacement is a later token); the line runs through it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR602Test, "SuperSLM.MultiRow.MR6.R60_2.Phase2OutlierDroppedSlopeMeasured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR602Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FRunDelayOnExit Clear;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	constexpr double kDelayUs = 100000.0;
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!Phase2Config(*this, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	if (!TestTrue(TEXT("the delay hook is armed"), FMr::ArmRowCostRunDelay(2, RunBits({1}), kDelayUs))) { return false; }
	FLogCapture Log;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec, Late;
	if (!Settle(*this, C.Rig, Rec, TEXT("R60.2")) || !SettleLate(*this, C.Rig, Late, TEXT("R60.2"))) { return false; }
	bool bOk = TestEqual(*FString::Printf(TEXT("one late run dropped ([%s] us)"), *RunsOf(Late.LateDroppedRunsUs)), Late.LateDroppedRunsUs.Num(), 1);
	bOk &= TestTrue(TEXT("the dropped late run is the delayed one"), Late.LateDroppedRunsUs.Num() == 1 && Late.LateDroppedRunsUs[0] >= kDelayUs);
	for (const double R : Late.LateTimedRunsUs) { bOk &= TestTrue(*FString::Printf(TEXT("kept late run %.1f us is a normal one"), R), R > 0.0 && R < kDelayUs); }
	bOk &= TestTrue(*FString::Printf(TEXT("the late position %lld is past P2 %lld (the replacement is a later token)"), Late.LatePosition, RefLatePosition(Cfg.ContextCap)), Late.LatePosition > RefLatePosition(Cfg.ContextCap));
	bOk &= TestEqual(TEXT("no late spread failure"), Late.LateSpreadFailures, 0);
	bOk &= RequireLineThroughLate(*this, C.Rig, Late, TEXT("R60.2"));
	bOk &= TestEqual(TEXT("no GpuRowCost warning"), Log.Count(ELogVerbosity::Warning, false, { TEXT("GpuRowCost") }), 0);
	return bOk;
}

// R60_3 (ruling 60 (2), (5): outliers in every run a set can take). Four arms, each with the cache forgotten. The hook
// adds 200 ms to a set's third run and both its replacements, so the set still holds one after its 2 extra runs: a
// spread failure. Phase 1 (B = 0): once (runs 2, 3, 4) -> retried, not cached: Calibrated on a second attempt, 1 spread
// failure, the 2 replacements' predecessors dropped, and a second Configure() is a cache hit on that price; twice
// (also runs 7, 8, 9, the retry's) -> the 6.2 ms fallback with one warning, 2 spread failures, and a second Configure()
// is a cache hit on the fallback (cached). Phase 2 (B = 2 ms): once -> retried, not cached: the slope measured, 1 late
// spread failure, a second Configure() a hit on the measured line; twice -> the default slope kept with one warning,
// 2 late spread failures, and a second Configure() a hit with phase 2 not pending.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR603Test, "SuperSLM.MultiRow.MR6.R60_3.SpreadFailureRetriedOnceThenCached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR603Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FRunDelayOnExit Clear;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	AddExpectedError(TEXT("GPU GpuRowCost calibration for"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("GPU GpuRowCost late position for"), EAutomationExpectedErrorFlags::Contains, 1);
	constexpr double kDelayUs = 200000.0;
	bool bOk = true;
	for (const bool bTwice : { false, true })
	{
		const FString What = FString::Printf(TEXT("phase 1, %s"), bTwice ? TEXT("twice") : TEXT("once"));
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
		FMr::ForgetRowCostCalibrations();
		FMr::ArmRowCostRunDelay(1, bTwice ? RunBits({2, 3, 4, 7, 8, 9}) : RunBits({2, 3, 4}), kDelayUs);
		FSuperSLMGpuRowCostRecord Rec;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, What)) { return false; }
		FMr::ArmRowCostRunDelay(1, 0u, 0.0);
		bOk &= TestEqual(*(What + TEXT(": spread failures")), Rec.SpreadFailures, bTwice ? 2 : 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: dropped runs ([%s] us)"), *What, *RunsOf(Rec.DroppedRunsUs)), Rec.DroppedRunsUs.Num(), bTwice ? 4 : 2);
		bOk &= TestEqual(*(What + TEXT(": attempts")), Rec.Attempts, 2);
		bOk &= TestEqual(*(What + TEXT(": the source")), (uint8)Rec.Source, bTwice ? (uint8)ESrc::FallBack : (uint8)ESrc::Calibrated);
		if (!bTwice)
		{
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the retry's runs hold no outlier ([%s] us)"), *What, *RunsOf(Rec.TimedRunsUs)), Rec.TimedRunsUs.Num() == 3 && NoOutlier(Rec.TimedRunsUs));
			bOk &= RequireRowCost(*this, C.Rig, Rec, static_cast<double>(RefPriceUs(Rec.TimedRunsUs)) / 1000.0, What);
		}
		FSuperSLMGpuRowCostRecord Again;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Again, What + TEXT(", again"))) { return false; }
		bOk &= TestEqual(*(What + TEXT(", again: a cache hit (no attempt)")), Again.Attempts, 0);
		bOk &= TestEqual(*(What + TEXT(", again: the same source")), (uint8)Again.Source, (uint8)Rec.Source);
		bOk &= TestTrue(*(What + TEXT(", again: the same row cost")), FMath::Abs(Again.RowCostMs - Rec.RowCostMs) <= 1e-9);
	}
	for (const bool bTwice : { false, true })
	{
		const FString What = FString::Printf(TEXT("phase 2, %s"), bTwice ? TEXT("twice") : TEXT("once"));
		FSuperSLMGpuRuntimeConfig Cfg;
		if (!Phase2Config(*this, Cfg)) { return false; }
		FMr::ForgetRowCostCalibrations();
		FMr::ArmRowCostRunDelay(2, bTwice ? RunBits({2, 3, 4, 7, 8, 9}) : RunBits({2, 3, 4}), kDelayUs);
		FSuperSLMGpuRowCostRecord Rec, Late;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, What) || !SettleLate(*this, C.Rig, Late, What)) { return false; }
		FMr::ArmRowCostRunDelay(2, 0u, 0.0);
		bOk &= TestEqual(*(What + TEXT(": late spread failures")), Late.LateSpreadFailures, bTwice ? 2 : 1);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: dropped late runs ([%s] us)"), *What, *RunsOf(Late.LateDroppedRunsUs)), Late.LateDroppedRunsUs.Num(), bTwice ? 4 : 2);
		if (bTwice)
		{
			bOk &= TestTrue(*(What + TEXT(": the default slope is kept")), Late.Source == ESrc::Calibrated && !Late.bSlopeMeasured && !Late.bLatePending);
		}
		else
		{
			bOk &= RequireLineThroughLate(*this, C.Rig, Late, What);
		}
		FSuperSLMGpuRowCostRecord Again;
		if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Again, What + TEXT(", again"))) { return false; }
		bOk &= TestEqual(*(What + TEXT(", again: a cache hit (no attempt)")), Again.Attempts, 0);
		bOk &= TestEqual(*(What + TEXT(", again: the slope is as before")), Again.bSlopeMeasured, !bTwice);
		bOk &= TestFalse(*(What + TEXT(", again: phase 2 is not pending")), Again.bLatePending);
	}
	return bOk;
}

// R60_4 (ruling 60 (1), (5): a moderate outlier is dropped, not adopted). A-EX, OneCall, multi-row on, B = 0, calibration
// on, the row cost unset. A first Configure() with the cache forgotten and no hook reads the set's largest run X; then,
// with the cache forgotten again, the hook adds 3 x X to phase 1's second timed run (about 3.5 to 4 x the median: sized
// on X, since the box's runs vary by about 1.3 x between sets, so a delay sized on the reference median can land under 3
// x the new set's). Then: one run dropped, the kept runs hold no outlier, and C_a is their price, below the delay (the
// moderate run did not set it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR604Test, "SuperSLM.MultiRow.MR6.R60_4.ModerateOutlierNotAdopted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR604Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FRunDelayOnExit Clear;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::WholeToken, true, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	FSuperSLMGpuRowCostRecord Ref;
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Ref, TEXT("R60.4, the reference"))) { return false; }
	TArray<double> Sorted = Ref.TimedRunsUs;
	Sorted.Sort();
	if (!TestTrue(*FString::Printf(TEXT("precondition: the reference set is clean ([%s] us)"), *RunsOf(Ref.TimedRunsUs)), Sorted.Num() == 3 && Ref.DroppedRunsUs.Num() == 0)) { return false; }
	const double Median = Sorted[1];
	const double DelayUs = 3.0 * Sorted[2];
	FMr::ForgetRowCostCalibrations();
	FMr::ArmRowCostRunDelay(1, RunBits({1}), DelayUs);
	FSuperSLMGpuRowCostRecord Rec;
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, TEXT("R60.4"))) { return false; }
	bool bOk = TestEqual(*FString::Printf(TEXT("the moderate run is dropped (dropped [%s] us; the reference median %.1f us)"), *RunsOf(Rec.DroppedRunsUs), Median), Rec.DroppedRunsUs.Num(), 1);
	bOk &= TestTrue(*FString::Printf(TEXT("the kept runs hold no outlier ([%s] us)"), *RunsOf(Rec.TimedRunsUs)), Rec.TimedRunsUs.Num() == 3 && NoOutlier(Rec.TimedRunsUs));
	const int64 Price = RefPriceUs(Rec.TimedRunsUs);
	bOk &= RequireRowCost(*this, C.Rig, Rec, static_cast<double>(Price) / 1000.0, TEXT("R60.4: C_a is the max of the kept runs"));
	bOk &= TestTrue(*FString::Printf(TEXT("C_a %lld us is below the %.1f us delay: not adopted"), Price, DelayUs), static_cast<double>(Price) < DelayUs);
	return bOk;
}

// R60_5 (ruling 60 (2), (5): E14_6's premise narrowed -- a real over-limit cost is still cached). A-EX, OneCall,
// multi-row on, B = 2 ms, calibration on, the row cost unset, the cache forgotten; the hook adds 200 ms to every one of
// phase 2's timed runs, so the set is consistent (no outlier) and C(1, ContextCap) is over 124 ms. Then: nothing dropped,
// no spread failure, the default slope kept with one warning; a second Configure() is a cache hit with phase 2 not
// pending and no calibration call in 30 ticks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowR605Test, "SuperSLM.MultiRow.MR6.R60_5.ConsistentOverLimitCostCached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowR605Test::RunTest(const FString& Parameters)
{
	FForgetCalibrationsOnExit Forget;
	FRunDelayOnExit Clear;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	AddExpectedError(TEXT("GPU GpuRowCost late position for"), EAutomationExpectedErrorFlags::Contains, 1);
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!Phase2Config(*this, Cfg)) { return false; }
	FMr::ForgetRowCostCalibrations();
	FMr::ArmRowCostRunDelay(2, 0xFFFFFFFFu, 200000.0);
	FSuperSLMGpuRowCostRecord Rec, Late;
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Rec, TEXT("R60.5")) || !SettleLate(*this, C.Rig, Late, TEXT("R60.5"))) { return false; }
	FMr::ArmRowCostRunDelay(2, 0u, 0.0);
	bool bOk = TestEqual(TEXT("nothing dropped"), Late.LateDroppedRunsUs.Num(), 0);
	bOk &= TestEqual(TEXT("no spread failure"), Late.LateSpreadFailures, 0);
	bOk &= TestTrue(TEXT("the default slope is kept"), Late.Source == ESrc::Calibrated && !Late.bSlopeMeasured && !Late.bLatePending);
	FMr::StartRecording(*C.Rig.Gpu);
	FSuperSLMGpuRowCostRecord Again;
	if (!ConfigureGpu(*this, C.Rig, Cfg) || !Settle(*this, C.Rig, Again, TEXT("R60.5, again"))) { return false; }
	PumpTicks(C.Rig, 30);
	FSuperSLMGpuRowCostRecord After;
	if (!TestTrue(TEXT("again: the record is readable after 30 ticks"), FMr::GetRowCost(*C.Rig.Gpu, After))) { return false; }
	bOk &= TestEqual(TEXT("again: a cache hit (no attempt)"), Again.Attempts, 0);
	bOk &= TestFalse(TEXT("again: phase 2 is not pending (the failure is cached)"), Again.bLatePending || After.bLatePending);
	bOk &= TestEqual(TEXT("again: no calibration call of either phase in 30 ticks"), CalibrationCallsSinceRecording(C.Rig), 0);
	bOk &= RequireDefaultLine(*this, C.Rig, After, &Cfg, TEXT("R60.5, again"));
	return bOk;
}

namespace
{
	// Rulings 57 and 58 (E14_7, E14_8): a tick's billed work, bounded in the product's favour.
	struct FBilledBounds
	{
		int32 L = 0;
		int64 F = 0;         // the finish, us
		int64 CHi = 1;       // the highest C(1) the run can price
		int64 RoomUnits = 0; // the most units B buys at the lowest C(1): floor(L x Room x 1000 / (C x (1000 + m))), uncapped
		int64 Margin = 0;
	};

	struct FBilledTally
	{
		int32 Both = 0, OneAlone = 0, CompAlone = 0, FinishOnly = 0, EndingShared = 0, WholeShared = 0, Over = 0, Crowded = 0;
		double Max = 0.0;
	};

	// The units B buys at C(1) = C, scene 0: floor(L x B x 1000 / (C x (1000 + m))), not capped at L.
	int64 RoomUnitsAt(int32 L, int64 BudgetUs, int64 C, int64 Margin)
	{
		return static_cast<int64>(L) * BudgetUs * 1000 / (FMath::Max<int64>(1, C) * (1000 + Margin));
	}

	// The smallest whole-us B at which RoomUnitsAt(L, B, C, m) >= Want.
	int64 BudgetForRoomUnitsUs(int32 L, int64 Want, int64 C, int64 Margin)
	{
		const int64 Den = static_cast<int64>(L) * 1000;
		return (Want * FMath::Max<int64>(1, C) * (1000 + Margin) + Den - 1) / Den;
	}

	// A primed finish-only step: one row, no layers, recorded at the full depth.
	bool IsFinishOnlyAct(const FAct& A, int32 L) { return A.Kind == ESuperSLMGpuRowsActionKind::OneRow && A.Rows <= 1 && A.Layers == 0 && A.FirstLayer == L; }

	// A one-row slice that ends a produced token (its finish rides): its last layer, not a prompt token. A whole OneCall
	// token is one.
	bool IsEndingAct(const FAct& A, int32 L)
	{
		return A.Kind == ESuperSLMGpuRowsActionKind::OneRow && A.Rows <= 1 && A.Layers > 0 && A.FirstLayer + A.Layers == L && A.PromptTokensConsumed == 0;
	}

	bool IsWholeTokenAct(const FAct& A, int32 L) { return IsEndingAct(A, L) && A.FirstLayer == 0 && A.Layers == L; }

	// Units an action bills, at least: a slice ending a token l + ceil(F x (L - l) / C) and a finish-only step
	// ceil(F x L / C), at the highest C(1); any other action its layers (a rows slice: r(M) >= 1).
	int64 BilledUnitsLo(const FAct& A, const FBilledBounds& B)
	{
		if (IsFinishOnlyAct(A, B.L)) { return (B.F * B.L + B.CHi - 1) / B.CHi; }
		if (IsEndingAct(A, B.L)) { return A.Layers + (B.F * (B.L - A.Layers) + B.CHi - 1) / B.CHi; }
		return FMath::Max(A.Layers, 0);
	}

	// Rulings 57 and 58, at every recorded tick:
	//   (a) the GPU time is at most BudgetUs unless the tick's work is a lone single unit: its only billed work is one
	//       one-row action of at most one layer (a one-layer slice, ending a token or not, or a finish-only step: ruling
	//       56a.2, 58.1), or one calibration slice of one row and one layer;
	//   (b) a tick that holds a slice ending a token (a whole OneCall token included) or a finish-only step beside any
	//       other billed work (another action, a phase-2 slice or finish) bills at most B.RoomUnits: actions at
	//       BilledUnitsLo, a phase-2 slice ceil(layers x 1000 / (1000 + m)) (its room is handed over in us with the
	//       margin), a phase-2 finish 0.
	// Tallies the paths' sharing (OneId against CompIds) for the cells' preconditions.
	bool GradeBilledTicks(FAutomationTestBase& T, const TArray<FSuperSLMGpuTickGpuBusyRecord>& Records, const TArray<FAct>& Acts, double BudgetUs,
		const FBilledBounds& B, int64 OneId, const TSet<int64>& CompIds, const FString& What, FBilledTally& Out)
	{
		TMap<int64, TArray<const FAct*>> ByTick;
		for (const FAct& A : Acts) { ByTick.FindOrAdd(A.Tick).Add(&A); }
		bool bOk = true;
		for (const FSuperSLMGpuTickGpuBusyRecord& R : Records)
		{
			Out.Max = FMath::Max(Out.Max, R.GpuBusyUs);
			const TArray<const FAct*>* TickActs = ByTick.Find(R.Tick);
			int32 Items = 0, OneActs = 0, CompActs = 0, Ending = 0;
			bool bOneWhole = false;
			int64 Units = 0;
			bool bLoneSingle = false;
			FString Work;
			if (TickActs != nullptr)
			{
				for (const FAct* A : *TickActs)
				{
					++Items;
					const bool bOne = A->SequenceId == OneId;
					const bool bComp = CompIds.Contains(A->SequenceId);
					OneActs += bOne ? 1 : 0;
					CompActs += bComp ? 1 : 0;
					bOneWhole |= bOne && IsWholeTokenAct(*A, B.L);
					Out.FinishOnly += bComp && IsFinishOnlyAct(*A, B.L) ? 1 : 0;
					Ending += IsEndingAct(*A, B.L) || IsFinishOnlyAct(*A, B.L) ? 1 : 0;
					Units += BilledUnitsLo(*A, B);
					bLoneSingle = A->Kind == ESuperSLMGpuRowsActionKind::OneRow && A->Rows <= 1 && A->Layers <= 1;
					Work += FString::Printf(TEXT(" [sequence %lld: kind %d, rows %d, layers %d+%d, prompt %d]"), A->SequenceId, (int32)A->Kind, A->Rows, A->FirstLayer, A->Layers,
						A->PromptTokensConsumed);
				}
			}
			for (const FSuperSLMGpuTickCallRecord& K : R.Calls)
			{
				if (K.Kind == ESuperSLMGpuTickCallKind::Calibration)
				{
					++Items;
					const int64 Layers = static_cast<int64>(FMath::Max(K.Layers, 0)) * FMath::Max(K.Rows, 1);
					Units += (Layers * 1000 + 1000 + B.Margin - 1) / (1000 + B.Margin);
					bLoneSingle = FMath::Max(K.Rows, 1) == 1 && K.Layers == 1;
					Work += FString::Printf(TEXT(" [calibration: attempt %d, rows %d, layers %d]"), K.CalibrationAttempt, K.Rows, K.Layers);
				}
				else if (IsCalibrationFinish(K))
				{
					++Items;
					bLoneSingle = false;
					Work += FString::Printf(TEXT(" [calibration finish: attempt %d]"), K.CalibrationAttempt);
				}
			}
			bLoneSingle &= Items == 1;
			Out.Both += OneActs > 0 && CompActs > 0 ? 1 : 0;
			Out.OneAlone += OneActs > 0 && CompActs == 0 ? 1 : 0;
			Out.CompAlone += CompActs > 0 && OneActs == 0 ? 1 : 0;
			Out.WholeShared += bOneWhole && CompActs > 0 ? 1 : 0;
			if (R.GpuBusyUs > BudgetUs && !bLoneSingle)
			{
				++Out.Over;
				bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: %.1f us of GPU time, at most B (%.0f us), or a lone single unit (rulings 57, 58):%s"), *What, R.Tick, R.GpuBusyUs,
					BudgetUs, *Work), false);
			}
			if (Ending > 0 && Items > 1)
			{
				++Out.EndingShared;
				if (Units > B.RoomUnits)
				{
					++Out.Crowded;
					bOk &= T.TestTrue(*FString::Printf(TEXT("%s, tick %lld: a slice ending a token shares the tick, so the tick bills at most the %lld units B buys (it bills %lld or more; rulings 57 F2-F4, 58.4):%s"),
						*What, R.Tick, B.RoomUnits, Units, *Work), false);
				}
			}
		}
		return bOk;
	}
}

// E14.7 (ruling 57: the ruling-56 review's F2, F3 and F4 -- a slice that ends a token overruns only alone; ruling 58.1).
// The shipped 1.5B (L 28), the scene share 0, multi-row on, LayersPerTick 4, the product's own calibration (on, the row
// cost unset, the cache forgotten). Configured at B = 2 ms, phase 1 and then phase 2 settle in a free pump (the measured
// line, cached). Ruling 58: the budget is then set so that l(1) is 4, else 2, at every position the run prices (never
// 3, at which OneCall's 27 layers before its ending slice leave no remainder): the smallest whole-us B with l(1) at the
// highest C(1) (C(1, the furthest position + 8)) at that value, used only when l(1) at the lowest (C(1, 0)) is the same.
// A-EX is configured again at that B: a cache hit (no attempt, the measured line, phase 2 not pending), and l(1)
// (GetOneRowLayersAt) is that value at position 0 and at the furthest position. A OneCall sequence generates 160 tokens
// from a 40-token prompt while three Composed generations (prompts of 34, 40 and 45 tokens, 40 tokens each, each begun
// as the one before ends, on their own sequences) take the contended ticks in turn with it (ruling 43 N2). Each Composed
// generation's first step after its prompt is the primed finish-only step. Skipped, with a warning, when the 1.5B is
// absent (as R50). Preconditions: as above; F measured; every generation produced its tokens; the per-tick seam
// recorded the run; some tick held a OneCall and a Composed action, some a OneCall action alone and some a Composed
// action alone; a primed finish-only step was recorded. Then GradeBilledTicks at that B: (a) no tick over B unless its
// work is a lone single unit; (b) a tick holding a slice ending a token, or a finish-only step, beside other billed work
// bills at most the units B buys at the lowest C(1), each piece counted at its least (an ending slice l + ceil(F x
// (L - l) / C), a finish-only step ceil(F x L / C), at the highest C(1)). So an ending slice or a finish-only step that
// overruns what its tick has left is the only billed work in its tick (F2, F4), and phase 2 (when it rides) is never
// handed room on a tick an overrun has filled (F3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14OverrunAloneTest, "SuperSLM.MultiRow.MR6.E14_7.EndingSliceOverrunsOnlyAloneOn15B",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14OverrunAloneTest::RunTest(const FString& Parameters)
{
	namespace PG = SuperSLMPagingGpuFixtures;
	FString Path15, Reason;
	if (!PG::ResolveArtifact(PG::EGpuArtifact::TwoCap15B, Path15, Reason))
	{
		AddWarning(FString::Printf(TEXT("SKIPPED (ruling 57's overrun cell needs the 1.5B): %s"), *Reason));
		return true;
	}
	if (!Gate(*this)) { return false; }
	FForgetCalibrationsOnExit Forget;
	PG::FGpuPagingRig Rig;
	if (!Rig.Init(*this, PG::EGpuArtifact::TwoCap15B)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *Rig.Gpu;
	const int32 L = PG::LayersOf(PG::EGpuArtifact::TwoCap15B);
	constexpr int32 kBlocks = 4, kOnePrompt = 40, kOneNew = 160, kComposedNew = 40, kComposedRuns = 3;
	const int32 ComposedPrompts[kComposedRuns] = { 34, 40, 45 };
	FSuperSLMGpuRuntimeConfig Cfg = PG::MakeGpuConfig(PG::EGpuArtifact::TwoCap15B, kBlocks, 0);
	Cfg.DispatchBudget = SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice(4);
	Cfg.K = FMath::Max3(L, kBlocks + 1, (kBlocks * L + 3) / 4);
	Cfg.GpuRowCostMs = 0.0;
	Cfg.SceneGpuShareMs = 0.0;
	Cfg.SchemaJumpGpuFrameBudgetMs = 2.0;
	if (!TestTrue(TEXT("bGpuMultiRow is set"), FMr::SetMultiRow(Cfg, true)) || !TestTrue(TEXT("bCalibrateGpuRowCost is set"), FMr::SetCalibrateGpuRowCost(Cfg, true))) { return false; }
	FMr::ForgetRowCostCalibrations();
	FSuperSLMGpuConfigureReport Report = Gpu.Configure(Rig.Model, Cfg);
	if (!TestEqual(*FString::Printf(TEXT("the 1.5B configures at B = 2 ms (%s)"), *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	Rig.Config = Cfg;
	const bool bSettled = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&Gpu]() { return !FMr::IsRowCostCalibrating(Gpu); });
	FSuperSLMGpuRowCostRecord Rec;
	if (!TestTrue(TEXT("phase 1 settles"), bSettled) || !TestTrue(TEXT("the row-cost record is readable"), FMr::GetRowCost(Gpu, Rec))) { return false; }
	if (!TestEqual(*FString::Printf(TEXT("precondition: phase 1 settled Calibrated (C_a %.3f ms)"), Rec.RowCostMs), (uint8)Rec.Source, (uint8)ESrc::Calibrated) ||
		!TestTrue(*FString::Printf(TEXT("precondition: F was measured (%lld us)"), Rec.FinishUs), Rec.bFinishMeasured && Rec.FinishUs > 0)) { return false; }
	const bool bLateSettled = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&Gpu]()
	{
		FSuperSLMGpuRowCostRecord Now;
		return FMr::GetRowCost(Gpu, Now) && !Now.bLatePending;
	});
	FSuperSLMGpuRowCostRecord Late;
	if (!TestTrue(TEXT("phase 2 settles in a free pump"), bLateSettled) || !TestTrue(TEXT("the row-cost record is readable after phase 2"), FMr::GetRowCost(Gpu, Late)) ||
		!TestTrue(TEXT("precondition: phase 2 measured the slope (so the line the run prices is fixed and cached)"), Late.bSlopeMeasured)) { return false; }

	// Ruling 58: l(1) pinned at 4, else 2, over the run's positions.
	const int64 m = Cfg.GpuRowsMarginPerMille;
	int32 MaxReach = kOnePrompt + kOneNew;
	for (int32 I = 0; I < kComposedRuns; ++I) { MaxReach = FMath::Max(MaxReach, ComposedPrompts[I] + kComposedNew); }
	const int64 FarPos = MaxReach + 8;
	const int64 Ca = CaUsOf(Late);
	const int64 CLo = FMath::Max<int64>(1, FMath::Min(Ca, RefRowCostAtUs(Ca, Late.AnchorPosition, Late.LateRowCostUs, Late.LatePosition, 0)));
	const int64 CHi = FMath::Max(CLo, RefRowCostAtUs(Ca, Late.AnchorPosition, Late.LateRowCostUs, Late.LatePosition, FarPos));
	int64 Want = 0, BudgetUs = 0;
	for (const int64 Try : { int64{4}, int64{2} })
	{
		const int64 Bu = BudgetForRoomUnitsUs(L, Try, CHi, m);
		if (RoomUnitsAt(L, Bu, CHi, m) == Try && RoomUnitsAt(L, Bu, CLo, m) == Try) { Want = Try; BudgetUs = Bu; break; }
	}
	if (!TestTrue(*FString::Printf(TEXT("precondition: a budget gives l(1) = 4 or 2 at every position (C(1) from %lld to %lld us)"), CLo, CHi), Want > 0)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = static_cast<double>(BudgetUs) / 1000.0;
	Report = Gpu.Configure(Rig.Model, Cfg);
	if (!TestEqual(*FString::Printf(TEXT("the 1.5B configures again at B = %lld us (%s)"), BudgetUs, *Report.Message), (uint8)Report.Result, (uint8)ESuperSLMGpuConfigureResult::Success)) { return false; }
	Rig.Config = Cfg;
	FSuperSLMGpuRowCostRecord Hit;
	if (!TestTrue(TEXT("the second Configure settles"), SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&Gpu]() { return !FMr::IsRowCostCalibrating(Gpu); })) ||
		!TestTrue(TEXT("the row-cost record is readable after the second Configure"), FMr::GetRowCost(Gpu, Hit))) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the second Configure is a cache hit on the measured line (attempts %d, measured %d, phase 2 pending %d)"), Hit.Attempts,
			Hit.bSlopeMeasured ? 1 : 0, Hit.bLatePending ? 1 : 0), Hit.Attempts == 0 && Hit.bSlopeMeasured && !Hit.bLatePending) ||
		!TestEqual(*FString::Printf(TEXT("precondition: l(1) at position 0 is %lld (ruling 58)"), Want), static_cast<int64>(FMr::GetOneRowLayersAt(Gpu, 0)), Want) ||
		!TestEqual(*FString::Printf(TEXT("precondition: l(1) at position %lld is %lld (ruling 58)"), FarPos, Want), static_cast<int64>(FMr::GetOneRowLayersAt(Gpu, FarPos)), Want)) { return false; }

	FSuperSLMGpuSequence One;
	FSuperSLMGpuSequence Comp[kComposedRuns];
	if (!PG::Vend(*this, Gpu, One, ESuperSLMGpuDecodePath::OneCall)) { return false; }
	for (FSuperSLMGpuSequence& S : Comp) { if (!PG::Vend(*this, Gpu, S, ESuperSLMGpuDecodePath::Composed)) { return false; } }
	FSuperSLMGenerationRequest OneReq;
	if (!PG::MakeTokens(*this, *Rig.Model, kOnePrompt, 53, OneReq.PromptTokens)) { return false; }
	OneReq.SpanKind = ESuperSLMSpanKind::Prompt;
	OneReq.MaxNewTokens = kOneNew;
	FSuperSLMGenerationRequest CompReq[kComposedRuns];
	for (int32 I = 0; I < kComposedRuns; ++I)
	{
		if (!PG::MakeTokens(*this, *Rig.Model, ComposedPrompts[I], 71 + I, CompReq[I].PromptTokens)) { return false; }
		CompReq[I].SpanKind = ESuperSLMSpanKind::Prompt;
		CompReq[I].MaxNewTokens = kComposedNew;
	}
	TSet<int64> CompIds;
	for (const FSuperSLMGpuSequence& S : Comp) { CompIds.Add(S.Id); }

	FMr::StartRecording(Gpu);
	if (!TestTrue(TEXT("OneCall: RequestBeginGeneration"), Gpu.RequestBeginGeneration(One, OneReq).IsValid()) ||
		!TestTrue(TEXT("Composed 1: RequestBeginGeneration"), Gpu.RequestBeginGeneration(Comp[0], CompReq[0]).IsValid())) { return false; }
	int32 Begun = 1;
	bool bBeginOk = true;
	const bool bEnded = SuperSLMGpuSchemaJumpFixtures::Pump(Gpu, [&]()
	{
		if (Begun < kComposedRuns && IsEnded(Gpu, Comp[Begun - 1]))
		{
			bBeginOk &= Gpu.RequestBeginGeneration(Comp[Begun], CompReq[Begun]).IsValid();
			++Begun;
		}
		if (!bBeginOk) { return true; }
		return Begun == kComposedRuns && IsEnded(Gpu, One) && IsEnded(Gpu, Comp[kComposedRuns - 1]);
	});
	PG::DrainQuiet(Gpu);
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(Gpu, Records);
	const TArray<FAct> Acts = FMr::GetActions(Gpu);
	bool bProduced = TestEqual(TEXT("precondition: OneCall produced its tokens"), Gpu.GetGeneratedTokens(One).Num(), kOneNew);
	for (int32 I = 0; I < kComposedRuns; ++I)
	{
		bProduced &= TestEqual(*FString::Printf(TEXT("precondition: Composed %d produced its tokens"), I + 1), Gpu.GetGeneratedTokens(Comp[I]).Num(), kComposedNew);
	}
	PG::ReturnAndDrain(Gpu, One);
	for (const FSuperSLMGpuSequence& S : Comp) { PG::ReturnAndDrain(Gpu, S); }
	if (!TestTrue(TEXT("every generation is begun"), bBeginOk) || !TestTrue(TEXT("every generation ends"), bEnded) || !bProduced) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the per-tick GPU seam recorded the run (%d records)"), Records.Num()), bRecorded && Records.Num() > 0)) { return false; }

	FBilledBounds Bounds;
	Bounds.L = L;
	Bounds.CHi = CHi;
	Bounds.F = FMath::Min<int64>(Rec.FinishUs, CHi);
	Bounds.Margin = m;
	Bounds.RoomUnits = RoomUnitsAt(L, BudgetUs, CLo, m);
	FBilledTally Tally;
	bool bOk = GradeBilledTicks(*this, Records, Acts, static_cast<double>(BudgetUs), Bounds, One.Id, CompIds, TEXT("E14.7"), Tally);
	AddInfo(FString::Printf(TEXT("E14.7 at B = %lld us, l(1) = %lld: %d ticks, max %.1f us, %d over B; %d ticks with an ending slice beside other work, %d of them over %lld units; ")
		TEXT("%d finish-only steps; C(1) in [%lld, %lld] us, F %lld us; OneCall + Composed %d ticks, OneCall alone %d, Composed alone %d"),
		BudgetUs, Want, Records.Num(), Tally.Max, Tally.Over, Tally.EndingShared, Tally.Crowded, Bounds.RoomUnits, Tally.FinishOnly, CLo, CHi, Bounds.F, Tally.Both, Tally.OneAlone,
		Tally.CompAlone));
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: OneCall and Composed shared ticks (%d) and took ticks alone (%d, %d): the paths alternated"), Tally.Both, Tally.OneAlone,
		Tally.CompAlone), Tally.Both > 0 && Tally.OneAlone > 0 && Tally.CompAlone > 0);
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: a primed finish-only step was recorded (%d)"), Tally.FinishOnly), Tally.FinishOnly > 0);
	return bOk;
}

// E14.8 (ruling 58.4: OneCall's whole token is billed as L units in the tick's accounting, so nothing overruns on top of
// it). A-EX (L 24), multi-row on, LayersPerTick 4 (Sliced4), the scene share 0, calibration on, the row cost unset, the
// cache forgotten. Configured at B = 10 ms, phase 1 and then phase 2 settle in a free pump (the measured line, cached).
// The budget is then the smallest whole-us B that buys L + 1 units at the highest C(1) the run prices (C(1, the
// furthest position + 8)), so OneCall runs whole tokens and leaves Composed a share; A-EX is configured again at it (a
// cache hit on the measured line, phase 2 not pending; l(1) (GetOneRowLayersAt) is L at the furthest position). A
// OneCall sequence generates 100 tokens from a 30-token prompt beside a Composed sequence generating 20 from its own
// 30-token prompt. Preconditions: as above; every generation produced its tokens; the per-tick seam recorded the run;
// ruling 58a (the implementation's design: a whole OneCall token fills l(1), so the paths take contended ticks in turn and
// OneCall yields the tick it owes Composed): the paths alternated (some tick held a OneCall action alone and some a
// Composed action alone) and every Composed ending slice ran (its ending slices and finish-only step number its 20
// tokens). Then GradeBilledTicks at that B: (a) no tick over B unless
// its work is a lone single unit; (b) a tick holding a slice ending a token -- a whole OneCall token is one, billed L --
// beside other billed work bills at most the units B buys at the lowest C(1) (each piece at its least).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMGpuMultiRowE14WholeTokenBilledTest, "SuperSLM.MultiRow.MR6.E14_8.WholeOneCallTokenBilledInTheTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMGpuMultiRowE14WholeTokenBilledTest::RunTest(const FString& Parameters)
{
	namespace PG = SuperSLMPagingGpuFixtures;
	FForgetCalibrationsOnExit Forget;
	FMrCell C;
	if (!C.Open(*this)) { return false; }
	USuperSLMGpuSubsystem& Gpu = *C.Rig.Gpu;
	const int32 L = SuperSLML2S2Fixtures::AExNumHiddenLayers;
	constexpr int32 kPrompt = 30, kOneNew = 100, kComposedNew = 20;
	FSuperSLMGpuRuntimeConfig Cfg;
	if (!CalConfig(*this, EPath::Sliced4, true, Cfg)) { return false; }
	Cfg.SceneGpuShareMs = 0.0;
	Cfg.SchemaJumpGpuFrameBudgetMs = 10.0;
	FMr::ForgetRowCostCalibrations();
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Rec;
	if (!Settle(*this, C.Rig, Rec, TEXT("E14.8"))) { return false; }
	if (!TestEqual(TEXT("precondition: phase 1 settled Calibrated"), (uint8)Rec.Source, (uint8)ESrc::Calibrated) ||
		!TestTrue(*FString::Printf(TEXT("precondition: F was measured (%lld us)"), Rec.FinishUs), Rec.bFinishMeasured && Rec.FinishUs > 0)) { return false; }
	FSuperSLMGpuRowCostRecord Late;
	if (!SettleLate(*this, C.Rig, Late, TEXT("E14.8")) ||
		!TestTrue(TEXT("precondition: phase 2 measured the slope (so the line the run prices is fixed and cached)"), Late.bSlopeMeasured)) { return false; }
	const int64 m = Cfg.GpuRowsMarginPerMille;
	const int64 FarPos = kPrompt + kOneNew + 8;
	const int64 Ca = CaUsOf(Late);
	const int64 CLo = FMath::Max<int64>(1, FMath::Min(Ca, RefRowCostAtUs(Ca, Late.AnchorPosition, Late.LateRowCostUs, Late.LatePosition, 0)));
	const int64 CHi = FMath::Max(CLo, RefRowCostAtUs(Ca, Late.AnchorPosition, Late.LateRowCostUs, Late.LatePosition, FarPos));
	const int64 BudgetUs = BudgetForRoomUnitsUs(L, L + 1, CHi, m);
	const int64 NominalUs = static_cast<int64>(FMath::FloorToDouble(Cfg.NominalTickMs * 1000.0));
	if (!TestTrue(*FString::Printf(TEXT("precondition: B = %lld us, which buys L + 1 units at C(1) %lld us, is within the nominal tick (%lld us)"), BudgetUs, CHi, NominalUs),
		BudgetUs <= NominalUs)) { return false; }
	Cfg.SchemaJumpGpuFrameBudgetMs = static_cast<double>(BudgetUs) / 1000.0;
	if (!ConfigureGpu(*this, C.Rig, Cfg)) { return false; }
	FSuperSLMGpuRowCostRecord Hit;
	if (!Settle(*this, C.Rig, Hit, TEXT("E14.8, again"))) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("precondition: the second Configure is a cache hit on the measured line (attempts %d, measured %d, phase 2 pending %d)"), Hit.Attempts,
			Hit.bSlopeMeasured ? 1 : 0, Hit.bLatePending ? 1 : 0), Hit.Attempts == 0 && Hit.bSlopeMeasured && !Hit.bLatePending) ||
		!TestEqual(*FString::Printf(TEXT("precondition: l(1) at position %lld is L: OneCall runs whole tokens"), FarPos), static_cast<int64>(FMr::GetOneRowLayersAt(Gpu, FarPos)),
			static_cast<int64>(L))) { return false; }

	// Ruling 59.3: the whole-token agent's MaxNew is trimmed inside its schema, as the MB6 probe does (ruling 53): a probe
	// run of the same prompt (tokens never depend on the path, P4.4) that meets the dead end after N tokens gives MaxNew
	// N - 1, so the graded run ends on MaxNewTokens on a produced token.
	int32 OneNew = kOneNew;
	{
		FGpuRun Probe;
		FSuperSLMGenerationRequest ProbeReq;
		if (!Vend(*this, C.Rig, EPath::WholeToken, Probe)) { return false; }
		if (!PG::MakeTokens(*this, *C.Rig.Cpu.Model, kPrompt, 91, ProbeReq.PromptTokens)) { return false; }
		ProbeReq.SpanKind = ESuperSLMSpanKind::Prompt;
		ProbeReq.MaxNewTokens = kOneNew;
		if (!TestTrue(TEXT("probe: RequestBeginGeneration"), Gpu.RequestBeginGeneration(Probe.Sequence, ProbeReq).IsValid())) { return false; }
		const bool bProbeEnded = Pump(Gpu, [&]() { return IsEnded(Gpu, Probe.Sequence); });
		Read(C.Rig, Probe);
		ReturnAndSettle(C.Rig, Probe.Sequence);
		if (!TestTrue(TEXT("probe: the generation ends"), bProbeEnded)) { return false; }
		if (Probe.Tokens.Num() < kOneNew)
		{
			if (!TestTrue(*FString::Printf(TEXT("probe: a MaxNew inside the schema exists (%d tokens to the dead end)"), Probe.Tokens.Num()), Probe.Tokens.Num() >= 2)) { return false; }
			OneNew = Probe.Tokens.Num() - 1;
		}
		AddInfo(FString::Printf(TEXT("E14.8 probe pass 0: the whole-token agent made %d of %d tokens; MaxNew %d (ruling 59.3)"), Probe.Tokens.Num(), kOneNew, OneNew));
	}

	FGpuRun Runs[2];
	const EPath Paths[2] = { EPath::WholeToken, EPath::Sliced4 };
	const int32 News[2] = { OneNew, kComposedNew };
	FSuperSLMGenerationRequest Reqs[2];
	for (int32 I = 0; I < 2; ++I)
	{
		if (!Vend(*this, C.Rig, Paths[I], Runs[I])) { return false; }
		if (!PG::MakeTokens(*this, *C.Rig.Cpu.Model, kPrompt, 91 + I, Reqs[I].PromptTokens)) { return false; }
		Reqs[I].SpanKind = ESuperSLMSpanKind::Prompt;
		Reqs[I].MaxNewTokens = News[I];
	}
	FMr::StartRecording(Gpu);
	for (int32 I = 0; I < 2; ++I)
	{
		if (!TestTrue(*FString::Printf(TEXT("%s: RequestBeginGeneration"), PathName(Paths[I])), Gpu.RequestBeginGeneration(Runs[I].Sequence, Reqs[I]).IsValid())) { return false; }
	}
	const bool bEnded = Pump(Gpu, [&]() { return IsEnded(Gpu, Runs[0].Sequence) && IsEnded(Gpu, Runs[1].Sequence); });
	for (FGpuRun& R : Runs) { Read(C.Rig, R); }
	TArray<FSuperSLMGpuTickGpuBusyRecord> Records;
	const bool bRecorded = FMr::GetTickGpuBusyUs(Gpu, Records);
	const TArray<FAct> Acts = FMr::GetActions(Gpu);
	for (FGpuRun& R : Runs) { ReturnAndSettle(C.Rig, R.Sequence); }
	if (!TestTrue(TEXT("both generations end"), bEnded)) { return false; }
	bool bProduced = true;
	for (int32 I = 0; I < 2; ++I)
	{
		bProduced &= TestEqual(*FString::Printf(TEXT("precondition: %s produced its tokens"), PathName(Paths[I])), Runs[I].Tokens.Num(), News[I]);
	}
	if (!bProduced || !TestTrue(*FString::Printf(TEXT("precondition: the per-tick GPU seam recorded the run (%d records)"), Records.Num()), bRecorded && Records.Num() > 0)) { return false; }

	FBilledBounds Bounds;
	Bounds.L = L;
	Bounds.CHi = CHi;
	Bounds.F = FMath::Min<int64>(Rec.FinishUs, CHi);
	Bounds.Margin = m;
	Bounds.RoomUnits = RoomUnitsAt(L, BudgetUs, CLo, m);
	TSet<int64> CompIds;
	CompIds.Add(Runs[1].Sequence.Id);
	FBilledTally Tally;
	bool bOk = GradeBilledTicks(*this, Records, Acts, static_cast<double>(BudgetUs), Bounds, Runs[0].Sequence.Id, CompIds, TEXT("E14.8"), Tally);
	AddInfo(FString::Printf(TEXT("E14.8 at B = %lld us (%lld units at the lowest C(1)): %d ticks, max %.1f us, %d over B; %d ticks with a whole OneCall token and a Composed action, ")
		TEXT("%d ticks with an ending slice beside other work, %d of them over; C(1) in [%lld, %lld] us, F %lld us"),
		BudgetUs, Bounds.RoomUnits, Records.Num(), Tally.Max, Tally.Over, Tally.WholeShared, Tally.EndingShared, Tally.Crowded, CLo, CHi, Bounds.F));
	int32 CompEndings = 0;
	for (const FAct& A : Acts)
	{
		CompEndings += A.SequenceId == Runs[1].Sequence.Id && (IsEndingAct(A, L) || IsFinishOnlyAct(A, L)) ? 1 : 0;
	}
	bOk &= TestTrue(*FString::Printf(TEXT("precondition: the paths alternated (OneCall alone %d ticks, Composed alone %d; ruling 58a)"), Tally.OneAlone, Tally.CompAlone),
		Tally.OneAlone > 0 && Tally.CompAlone > 0);
	bOk &= TestEqual(*FString::Printf(TEXT("precondition: every Composed ending slice ran (one per token; ruling 58a)")), CompEndings, kComposedNew);
	return bOk;
}

#endif // SUPERSLM_WITH_L2S1_ASYNC && SUPERSLMUNREAL_WITH_GPU
#endif // WITH_DEV_AUTOMATION_TESTS
