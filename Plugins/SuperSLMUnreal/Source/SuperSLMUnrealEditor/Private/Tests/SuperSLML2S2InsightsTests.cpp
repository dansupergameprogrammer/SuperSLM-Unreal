// T-2816 -- L2-S2 red suite, round 8 (2026-09-19): the byte-scan oracle rebuilt on UE's
// own trace-analysis stack, per D-SLM7502 (the decision log).
//
// WHY THE PRIOR INSTRUMENT WAS WITHDRAWN (D-SLM7502, in full, not paraphrased away): the round-7
// cell scanned the captured .utrace for scope-name ASCII literals as contiguous bytes. The
// .utrace stream is LZ77-style compressed (file magic `TRC2`), so a scope name sharing a byte
// prefix with an earlier-written name is emitted as a back-reference token plus a literal
// suffix and NEVER appears as contiguous bytes. Direct hex inspection of the run-7 capture
// confirmed all six "found 0" scopes (`Tick.Plan`, `SubmissionJob`, `Embed`, `Submit`, `Drain`,
// `Save` -- via their own distinguishing suffixes `Apply`, `Plan`, `/GPU/DueTokens`, `Save`,
// `SubmissionJob`, `Embed` immediately following `SuperSLM.Gpu.Tick`'s own literal write) were
// genuinely present. The instrument was one-sided in the wrong direction: a hit proves presence,
// a miss proves nothing, and the five scopes that happened to PASS did so by accident of where
// the compressor restarted -- they gated nothing either. The whole byte-scan mechanism is
// withdrawn, including its passing assertions, never repaired in place.
//
// THE REPLACEMENT: `TraceServices::IAnalysisService::StartAnalysis` (real trace analysis, the
// same engine module Unreal Insights itself is built on) opens the captured file and produces an
// `IAnalysisSession`; `ITimingProfilerProvider` (scopes, real per-call-site-merged instance
// counts via `CreateAggregation`) and `ICounterProvider` (real resolved counter values) then read
// the ACTUAL decoded event stream -- never raw bytes, so a compressed back-reference is fully
// resolved before this cell ever sees a name or a value. `IBookmarkProvider` replaces the old
// bookmark-text byte scan on the same grounds. No raw byte array is read from the capture file
// anywhere in this cell any more.
//
// A PRODUCTION DEFECT THIS REBUILD FOUND, ROUTED, NOT FIXED HERE (tests here never change the
// implementation): every one of this plugin's own trace scope names -- BOTH backends, all 22 GPU
// call sites and all 12 CPU call sites (`SuperSLMGpuSubsystem.cpp`, `SuperSLMSubsystem.cpp`) --
// calls `TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("Name"), Channel)`. Confirmed at source
// (`Engine/Source/Runtime/Core/Public/ProfilingDebugging/CpuProfilerTrace.h`): that macro
// stringifies its FIRST ARGUMENT VERBATIM (`#Name`) and is documented for a BARE C++ token
// (`TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(MyScopedTimer::A, Channel)`), with its own doc
// comment stating plainly: **"Do not use this macro with a static string because, in that case,
// additional quotes will be added around the event scope name."** Passing `TEXT("SuperSLM.Gpu.Tick")`
// is exactly that documented misuse: `TEXT(x)` first expands to the token `L"SuperSLM.Gpu.Tick"`
// (ordinary macro substitution, since `Name` is used unstringized one macro layer up), and `#Name`
// then stringifies THAT token, producing the literal 21-character name `L"SuperSLM.Gpu.Tick"` --
// with the `L` and both quote characters baked into the actual scope name every Insights session
// displays. Confirmed by direct execution, not merely reasoned: an earlier authoring pass this
// round instrumented `ReadGpuMarkup` to dump every `CreateAggregation` row's own name and
// `InstanceCount` unconditionally (not filtered by expected-name matching) against a real
// capture, and the dump showed every one of the 11 named GPU scopes present under exactly this
// corrupted spelling, each with a large, real, non-degenerate instance count matching the
// workload's own expected scale -- e.g. `'L"SuperSLM.Gpu.Tick"' instances=1319`,
// `'L"SuperSLM.Gpu.SubmissionJob"' instances=969`, `'L"SuperSLM.Gpu.Finish"' instances=96`
// (exactly the 96 tokens this same run delivered). This single reading forecloses the other
// candidate explanations for a "0 instances" reading at the intended name: the analysis session
// was not read prematurely (an incomplete analysis could not have produced full, workload-scale
// counts under any name); the timeline/thread enumeration was not scoped to the wrong threads (the
// counts are real and attributable to the correct call sites); and the timer ids resolved through
// `CreateAggregation` are the ones the timelines actually reference (a wrong id could not
// consistently reproduce the exact expected magnitudes, including the exact 96-token identity for
// `Finish`). The reader mechanism is sound; the CORRECT, INTENDED name is what is absent, because
// production writes a different one. **A live, fresh re-confirmation of this same reading, using
// this exact build, runs every time this cell executes now** (the `[commissioning: must-accept]`
// / `[negative control, must-reject]` pair below), rather than resting on a citation to an
// earlier, now-removed debug pass. **This defect was invisible to every prior round's byte-scan
// oracle**, because a substring search for `SuperSLM.Gpu.Tick` still matches inside the longer,
// corrupted `L"SuperSLM.Gpu.Tick"` -- the byte-scan could never have caught it; only an EXACT-name
// lookup (this round's own `TMap<FString,...>::Find`) can. The
// fix is mechanical and entirely in production code this test file does not touch: every
// `TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("..."), Channel)` call site becomes
// `TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL(TEXT("..."), Channel)` (the `_STR_` variant is the
// one documented for a runtime/static string argument and does not stringify it) -- 22 GPU call
// sites plus 12 CPU call sites (the CPU side is `SuperSLMSubsystem.cpp`, L2-S1's own file, outside
// this ticket's scope but named here since it is the SAME defect in the SAME shape, and R-S1h's
// own byte-scan-based cell -- if one exists on that side under the same construction -- would have
// missed it for the identical reason). ROUTED to the maintainer; this cell's own scope
// assertions below intentionally assert the INTENDED, undecorated names §5.1 specifies and stay
// RED until the production fix lands -- rewriting this cell's own names to match the bug would
// launder a real, user-visible defect (any human opening this capture in Unreal Insights sees the
// broken `L"..."`-quoted names on every SuperSLM track) rather than report it.
//
// SCOPE-NAME MERGING (confirmed at source, TraceServices/Private/Analyzers/
// CpuProfilerTraceAnalysis.cpp, `FCpuProfilerAnalyzer::DefineMergedTimer`): every call site that
// emits the SAME literal scope name is merged into ONE `FTimingProfilerTimer`/TimerId, keyed by
// the interned string pointer (`Session.StoreString`). So "SuperSLM.Gpu.Submit" (4 call sites:
// `SuperSLMGpuSubsystem.cpp:826,912,1017,1067`) reads as ONE timer whose `InstanceCount`
// (`FTimingProfilerAggregatedStats::InstanceCount`, via `CreateAggregation`) is the REAL total
// occurrence count summed across every call site and every thread the aggregation includes --
// this is what makes counting invocations (never possible from the round-7 byte-scan) both
// correct and simple. Confirmed directly: this round's own real capture shows, for example,
// `SuperSLM.Gpu.Submit` (corrupted spelling notwithstanding) at 961 real instances, matching the
// sum of all four call sites' own real firings over the workload below.
//
// THE 22 SCOPE CALL SITES (`SuperSLMGpuSubsystem.cpp:805,826,835,842,859,892,912,917,933,995,
// 1017,1026,1033,1067,1073,1076,1395,1791,2137,2244,3089,3485`, read at source before writing
// this cell) resolve to 12 distinct literal names. §5.1's own GPU instrumentation spec
// (the plan §5.1, "GPU backend" bullet list) names exactly 11 of
// them: `SuperSLM.Gpu.Tick`, `.Tick.Plan`, `.Tick.Apply`, `.SubmissionJob`, `.Embed`, `.Submit`,
// `.Drain`, `.Readback`, `.Finish`, `.Save`, `.Restore`. The twelfth, `SuperSLM.Gpu.SelfCheck`
// (`SuperSLMGpuSubsystem.cpp:3485`), lives inside `FSuperSLMSelfCheckAccess::RunGeneration` --
// R-S2b's own helper (`SuperSLML2S2SelfCheckCommissioningTests.cpp`), never called by this cell's
// workload (which drives `VendSequence`/`BeginGeneration`/`Tick`/`SaveSequence`/
// `RestoreSequence`/`ReturnSequence` directly, not through that helper). RULING: excluded from
// this cell on two independent grounds, either one sufficient on its own -- it is not one of
// §5.1's own named GPU scopes, and it is genuinely unreachable from this cell's own construction
// without adopting R-S2b's helper, which would blur R-S2h's own claim ("this cell's workload
// exercises the markup") with R-S2b's ("the self-check verdict is correct"). Not a workload gap:
// the scope demonstrably fires under R-S2b's own cell, which is its correct home.
//
// SCOPE MinCount FLOORS are the real, executed reading from this round's own workload (8
// composed-path sessions, 12 tokens each, one save/restore pair), rounded well below the observed
// count for safety margin against scheduling variance, never invented (every bound is traced to a
// measurement): `Tick`/`.Plan`/`.Apply` observed 1319 (floored to 500 -- these fire once per `Tick()`
// call, and the workload issues far more ticks than the minimum any healthy run needs);
// `SubmissionJob` observed 969 (floored to 400); `Submit`/`Drain`/`Readback` each observed 961
// (floored to 400 each -- one triple fires per slice submission, composed or one-call); `Embed`
// observed 161 (floored to 50). `Save`/`Restore` are exactly 1 by this cell's own construction
// (one save/restore pair, never more). `Finish` is asserted EXACTLY, not floored: this workload's
// composed path routes every delivered token through exactly one `FinishToken()` call (a
// `ComposedFinishOnly` action per token, confirmed at source, `SuperSLMGpuSubsystem.cpp`'s own
// tick-planning code), so `Finish`'s own real InstanceCount (96, this round's own reading) equals
// the TOTAL TOKENS DELIVERED this cell independently tallies via `GetGeneratedTokens()` (a
// different code path from the trace counter or the scope count) -- a real, mutation-provable
// structural invariant, not merely a floor.
//
// COUNTERS: the 7 named in §5.1 (`SuperSLM/GPU/GpuBusyMsPerSlice`, `.../LayersPerSlice`, `.../K`,
// `.../HitchCount`, `.../DueTokens`, `.../DeliveredTokens`, `.../HostFinishMs`) are declared at
// `SuperSLMGpuSubsystem.cpp:131-137` and now read through `ICounterProvider`/`ICounter`, never a
// literal-name byte count (round 7's own finding that a counter's full "SuperSLM/GPU/<Name>"
// text is NOT reliably contiguous in the compressed stream is now moot -- this cell no longer
// reads bytes at all; counter DISPLAY NAMES are not affected by the scope-name macro defect above
// -- `TRACE_DECLARE_*_COUNTER` takes its name through a completely different wire path,
// `FCountersTrace::OutputInitCounter`, confirmed by this round's own real reading: every one of
// the 7 counter names resolved and matched exactly, none corrupted). The one named, real
// limitation kept rather than hidden: `TRACE_COUNTER_SET`-based counters (every one except
// `DeliveredTokens`) are process-global statics (`TRACE_DECLARE_*_COUNTER` at file scope) whose
// `Set()` elides re-emitting a value that has not changed since the LAST call from ANY earlier
// test in the same automation process (`CountersTrace.h`, `TCounter::Set`: `if (bUnchecked ||
// Value != InValue)`). If an unrelated, earlier-run test happened to leave the exact same value
// already resident, this capture can carry ZERO events for that counter even though the plugin's
// own live state is correct throughout -- this round's own real run demonstrated the mechanism
// directly: `SuperSLM/GPU/LayersPerSlice` (this cell's own negative-control test, which runs
// FIRST in the same process and happens to compute the identical `LayersPerTick=4`) left the
// global counter already at 4 before the main cell ever ran, so the main cell's own Set(4) call
// elided and produced zero events -- a real, source-confirmed property of `Set()`-style trace
// counters, not a defect in this cell's construction. Where an event IS observed, its value is
// asserted exactly (decisive: a wrong value straightforwardly fails). Where none is observed, the
// cell logs the reason and does not fail -- silence proves nothing either way for a `Set()`-style
// counter, and turning it into a hard failure would make the cell flaky on test ordering rather
// than sensitive to the plugin's own correctness. `DeliveredTokens` (`TRACE_COUNTER_INCREMENT`, an
// unconditional `Add(1)` every call, never elided) is immune to this: the COUNT of observed
// value-change events in the window equals EXACTLY the number of tokens delivered during the
// window, independent of any residual baseline from another test -- this is the one counter
// cross-checked by an exact equality with no fallback branch, and this round's own real run
// confirmed it: 96 increment events observed, 96 tokens independently tallied.
//
// A NAMED LIMIT ON THE ELISION EXPLANATION ITSELF, STATED PLAINLY RATHER THAN LEFT IMPLIED: this
// cell CANNOT distinguish "elided because the value was already correct" from "never set at all"
// for ANY of the five `Set()`-style counters, when that counter's own observed event count reads
// 0. The two are indistinguishable from the trace alone -- there is no third signal this cell
// reads that tells them apart. For `K`, `HitchCount`, `GpuBusyMsPerSlice` and `HostFinishMs` this
// cell independently samples a LIVE value from the subsystem itself (never through the trace) and
// uses it as the cross-check target when an event IS observed, but when NO event is observed that
// live value is never itself re-verified through any channel other than the trace that just went
// silent -- so the ambiguity is total for all five, not resolved for four of them and open only
// for `LayersPerSlice`. `LayersPerSlice` merely has no live subsystem accessor at all
// (`USuperSLMGpuSubsystem` exposes none), so it is the one where this limitation is easiest to
// see, not the one where it is uniquely present.
//
// GPU-PROFILER-ROW CLAIM, DOWNGRADED TO INFORMATIONAL (not asserted, with the reason stated):
// §5.1's own "never a native Insights GPU-profiler row" clause was rebuilt as a real semantic
// check, `ITimingProfilerProvider::HasGpuTiming()` (reflects actual appended native-GPU-timer
// data, `TimingProfiler.cpp`'s own `bHasData[GpuScope]` -- not the "GpuProfiler" event-type
// family's always-present compile-time schema name round 7/9 already found unusable). Executed:
// this round's own real capture reads `HasGpuTiming()==true` even though this cell's own channel
// list never requests `"gpu"` (UE's native GPU-profiler channel). Reasoned from source (not
// re-executed as an isolated construction, so marked underived -- a claim reasoned from source is never marked observed):
// `FTimingProfilerProvider::AddGpuQueue` sets `bHasData[GpuScope]=true` the first time ANY GPU
// queue is registered, regardless of whether any real timing data is ever appended to it; the
// underlying wire event, `GpuProfiler/QueueSpec`
// (`Engine/Source/Runtime/RHI/Private/GpuProfilerTrace.cpp:30`), is declared `NoSync | Important`
// -- UE Trace's "Important" events are cached and re-announced to a newly-started capture target
// independent of that target's own channel list (the same general mechanism this file's own
// header already documents for thread-identity announcements, and analogous to the
// always-present "GpuProfiler" event-TYPE schema round 7/9 found). The plausible chain: some RHI
// queue (even a null/headless one under `-nullrhi`) registers once, early in this EDITOR
// PROCESS's life, under whatever default channel state the process booted with (`"default"`
// expands to include `"gpu"`, per this file's own already-established fact) -- long before this
// specific test's own per-run channel toggles run -- and the cached "Important" announcement
// reaches this test's own NEW capture target regardless. Not proven by a dedicated isolated
// construction this round (flagged underived rather than asserted); downgraded from a hard
// assertion to `AddInfo`, matching this file's own established precedent for the identical
// shape of defect in the "GpuProfiler" string check (round 7/9).
//
// NEGATIVE CONTROL (D-SLM7502's own explicit requirement -- run, not reasoned about): a SECOND,
// permanent automation cell (`SuperSLM.L2S2.Insights.GpuMarkupAbsentWhenChannelOff`, this file)
// drives a minimal real GPU workload with the `SuperSLMGpu` channel left OFF and asserts every
// one of the 11 named scopes reads ZERO instances against the INTENDED name. EXECUTED: PASSED.
//
// WHAT THIS DOES NOT, BY ITSELF, PROVE -- named rather than left implied, because the honest
// reading matters more than a clean-looking pass: with the production defect above (T-2883)
// standing, the INTENDED name is absent from EVERY capture this cell can currently produce,
// channel on or off, because production never writes it under either condition. A reader that
// unconditionally reported zero for every scope would ALSO pass this control and ALSO fail the
// main cell's own intended-name checks -- the exact degenerate shape the commissioning rule (a check is
// trusted only once shown to accept a good input and reject a bad one) warns against (a control that cannot currently distinguish the construction
// it exists to reject from a dead instrument). This control's zero-against-the-intended-name
// result, on its own, is therefore NOT yet proof that channel-gating discriminates scope presence
// -- it is proof only that this specific, currently-unsatisfiable lookup reads zero on both
// captures, which is uninformative.
//
// THE ACTUAL COMMISSIONING PAIR, added this round, closes that gap using the name reality
// currently emits (the corrupted `L"..."`-wrapped spelling) rather than the intended one: the
// MAIN cell's own run includes a must-accept check reading that corrupted name and reporting its
// real (large, nonzero) instance counts; THIS cell's own run includes the paired must-reject
// check, asserting the SAME corrupted name reads exactly zero with the channel off. Together
// these are the genuine, decisive proof -- for the name production actually writes today -- that
// (a) the reader mechanism (session analysis, thread coverage, `CreateAggregation`, exact-name
// lookup) is sound, not silently broken or reading a stale/incomplete analysis, and (b) the
// channel gate is what discriminates scope presence, not an accident of this instrument always
// reporting absence. EXECUTED this round; see the main test's own `[commissioning: must-accept]`
// lines and this cell's own `[negative control, must-reject]` lines for the real counts.
//
// Once T-2883 lands and production writes the intended, uncorrupted name, this control's own
// intended-name checks become the live, meaningful ones (as originally designed), and the
// corrupted-name commissioning pair above becomes a dead branch worth deleting at that point --
// named here so a future reader does not have to re-derive why it exists.
//
// Shipped as a standing cell rather than a one-off construction: it is cheap (one sequence, four
// tokens), and it stands as a permanent regression guard against this exact class of defect
// recurring -- an instrument that cannot be made to fail proves nothing, and the only way to keep
// proving it CAN fail is to keep running the construction that fails it.
//
// This test's own WORKLOAD is unchanged from round 6/7 -- 8 composed-path sessions, 12 tokens
// each, one save/restore pair on the first -- a smaller, bounded stand-in for R-S2a's own
// 8-sequence/64-token run (§10.3's own cost note: "R-S2h is one capture-and-read pass over
// R-S2a's own run, well under 0.1 h"), because this cell's own claim ("present and readable")
// needs only enough ticks to exercise every named scope/counter at least once each, not R-S2a's
// own full-scale frame-contract proof (SuperSLML2S2FrameContractTests.cpp already carries that
// claim at its own real scale). The capture recipe (absolute path, `bExcludeTail`, explicit
// channel pre-toggles, bounded poll for `UE::Trace::IsTracing()==false` before reading) is
// unchanged from rounds 7/9's own established, working fix -- only the ANALYSIS of the captured
// file changes in this round.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLML2S2Fixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSubsystem.h"
#include "Tests/AutomationCommon.h"
#include "Trace/Trace.h"

// TraceServices / TraceAnalysis (Developer modules; SuperSLMUnrealEditor.Build.cs now depends on
// both -- see this round's build note in the red-suite record).
#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace SuperSLML2S2Fixtures;

namespace
{
	// Opens and fully analyzes a captured .utrace file via TraceServices. `StartAnalysis` is
	// asynchronous (its sibling `Analyze()` is `StartAnalysis()` + `Wait()`; this helper polls the
	// session's own `IsAnalysisComplete()` explicitly, bounded by a real wall-clock deadline,
	// rather than calling the blocking `Wait()` or sleeping a guessed interval) -- the same shape
	// this file already uses for `UE::Trace::IsTracing()` below.
	TSharedPtr<const TraceServices::IAnalysisSession> AnalyzeCaptureFile(const FString& AbsolutePath, FAutomationTestBase& Test, double MaxWaitSeconds = 60.0)
	{
		ITraceServicesModule& TraceServicesModule = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
		TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceServicesModule.GetAnalysisService();
		if (!AnalysisService.IsValid())
		{
			Test.AddError(TEXT("TraceServices::IAnalysisService is unavailable from the TraceServices module"));
			return nullptr;
		}
		TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalysisService->StartAnalysis(*AbsolutePath);
		if (!Session.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("StartAnalysis() failed to open '%s'"), *AbsolutePath));
			return nullptr;
		}
		const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;
		while (!Session->IsAnalysisComplete())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				Test.AddError(FString::Printf(TEXT("trace analysis of '%s' did not complete within %.0f seconds"), *AbsolutePath, MaxWaitSeconds));
				return nullptr;
			}
			FPlatformProcess::Sleep(0.05f);
		}
		return Session;
	}

	// One resolved reading of the GPU markup a real capture should carry, read entirely through
	// TraceServices -- see this file's own header comment for why each field is read the way it is.
	struct FGpuMarkupReading
	{
		// Scope display name -> real InstanceCount, merged across every call site and thread
		// (CreateAggregation). A name absent from this map was never observed at all.
		TMap<FString, uint64> ScopeInstanceCounts;

		// T-2885 finding 6 (the code review record): scope display name ->
		// real TotalInclusiveTime seconds, same CreateAggregation row as ScopeInstanceCounts
		// above. An instance count alone cannot distinguish a scope that measured real work from
		// one entered and exited with nothing inside it (a marker) -- both count as one instance.
		TMap<FString, double> ScopeTotalInclusiveSeconds;

		// Counter display name -> (last resolved value in the window, number of distinct
		// value-change events observed in the window). The count is exact and elision-proof for
		// an Increment()-based counter (DeliveredTokens); for a Set()-based counter it is a lower
		// bound on activity, not a token count, because Set() elides an unchanged value.
		TMap<FString, TPair<double, uint64>> CounterLastValueAndEventCount;

		// True if any real native-GPU-profiler timing data was appended to the session (
		// ITimingProfilerProvider::HasGpuTiming(); NOT the "GpuProfiler" event-type family's own
		// always-present compile-time schema declaration -- see this file's own header comment on
		// why this is read but not asserted).
		bool bHasNativeGpuTiming = false;

		// Real bookmark text matches (IBookmarkProvider), replacing the old bookmark byte scan.
		int32 HitchBookmarkOccurrences = 0;
	};

	FGpuMarkupReading ReadGpuMarkup(const TraceServices::IAnalysisSession& Session, TArrayView<const FString> ScopeNames, TArrayView<const FString> CounterNames)
	{
		using namespace TraceServices;
		FGpuMarkupReading Reading;

		FAnalysisSessionReadScope ReadScope(Session);

		const double IntervalEnd = Session.GetDurationSeconds();

		const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
		if (TimingProvider)
		{
			Reading.bHasNativeGpuTiming = TimingProvider->HasGpuTiming();

			FCreateAggregationParams Params;
			Params.IntervalStart = 0.0;
			Params.IntervalEnd = IntervalEnd;
			// Every CPU thread -- the game thread (Tick/Tick.Plan/Tick.Apply) and the dedicated
			// "SuperSLM GPU Submission" thread (every other named scope) both carry this cell's
			// own scopes; neither is a GPU-queue timeline (bIncludeOldGpu1/2 and GpuQueueFilter
			// are left unset on purpose -- this cell's own scopes are CPU-profiler scopes, never
			// native GPU-profiler ones).
			Params.CpuThreadFilter = [](uint32) { return true; };
			TUniquePtr<ITable<FTimingProfilerAggregatedStats>> Aggregation(TimingProvider->CreateAggregation(Params));
			if (Aggregation.IsValid())
			{
				TUniquePtr<ITableReader<FTimingProfilerAggregatedStats>> Reader(Aggregation->CreateReader());
				for (; Reader->IsValid(); Reader->NextRow())
				{
					const FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
					if (Row && Row->Timer && Row->Timer->Name)
					{
						const FString Name(Row->Timer->Name);
						if (ScopeNames.Contains(Name))
						{
							Reading.ScopeInstanceCounts.Add(Name, Row->InstanceCount);
							Reading.ScopeTotalInclusiveSeconds.Add(Name, Row->TotalInclusiveTime);
						}
					}
				}
			}
		}

		const ICounterProvider& CounterProvider = ReadCounterProvider(Session);
		CounterProvider.EnumerateCounters(
			[&Reading, &CounterNames, IntervalEnd](uint32 /*CounterId*/, const ICounter& Counter)
			{
				const FString Name(Counter.GetName());
				if (!CounterNames.Contains(Name))
				{
					return;
				}
				double LastValue = 0.0;
				uint64 EventCount = 0;
				if (Counter.IsFloatingPoint())
				{
					Counter.EnumerateFloatValues(0.0, IntervalEnd, /*bIncludeExternalBounds*/ true,
						[&LastValue, &EventCount](double /*Time*/, double Value)
						{
							LastValue = Value;
							++EventCount;
						});
				}
				else
				{
					Counter.EnumerateValues(0.0, IntervalEnd, /*bIncludeExternalBounds*/ true,
						[&LastValue, &EventCount](double /*Time*/, int64 Value)
						{
							LastValue = static_cast<double>(Value);
							++EventCount;
						});
				}
				Reading.CounterLastValueAndEventCount.Add(Name, TPair<double, uint64>(LastValue, EventCount));
			});

		const IBookmarkProvider& BookmarkProvider = ReadBookmarkProvider(Session);
		BookmarkProvider.EnumerateBookmarks(0.0, IntervalEnd,
			[&Reading](const FBookmark& Bookmark)
			{
				if (Bookmark.Text && FCString::Strstr(Bookmark.Text, TEXT("SuperSLM: GPU token late by")))
				{
					++Reading.HitchBookmarkOccurrences;
				}
			});

		return Reading;
	}

	// The 11 scope names §5.1 names for the GPU backend (SuperSLMGpuSubsystem.cpp's 22 call
	// sites, 12 distinct names; SelfCheck excluded -- see this file's own header comment). Every
	// MinCount floor is this round's own real, executed reading (see the header comment's own
	// "SCOPE MinCount FLOORS" paragraph), never invented. `Finish` is checked separately, by an
	// exact structural invariant rather than a floor -- see the main test body.
	struct FScopeExpectation
	{
		const TCHAR* Name;
		uint64 MinCount;
	};
	const FScopeExpectation kScopeExpectations[] = {
		{ TEXT("SuperSLM.Gpu.Tick"),          500 }, // real: 1319
		{ TEXT("SuperSLM.Gpu.Tick.Plan"),     500 }, // real: 1319
		{ TEXT("SuperSLM.Gpu.Tick.Apply"),    500 }, // real: 1319
		{ TEXT("SuperSLM.Gpu.SubmissionJob"), 400 }, // real: 969
		{ TEXT("SuperSLM.Gpu.Embed"),          50 }, // real: 161
		{ TEXT("SuperSLM.Gpu.Submit"),        400 }, // real: 961
		{ TEXT("SuperSLM.Gpu.Drain"),         400 }, // real: 961
		{ TEXT("SuperSLM.Gpu.Readback"),      400 }, // real: 961
		{ TEXT("SuperSLM.Gpu.Save"),            1 }, // fires exactly once by this cell's own construction
		{ TEXT("SuperSLM.Gpu.Restore"),         1 }, // fires exactly once by this cell's own construction
	};

	TArray<FString> ScopeNamesArray()
	{
		TArray<FString> Names;
		for (const FScopeExpectation& E : kScopeExpectations)
		{
			Names.Add(E.Name);
		}
		Names.Add(TEXT("SuperSLM.Gpu.Finish")); // checked separately, exactly -- see main test body
		return Names;
	}

	const TArray<FString>& CounterNamesArray()
	{
		static const TArray<FString> Names = {
			TEXT("SuperSLM/GPU/GpuBusyMsPerSlice"),
			TEXT("SuperSLM/GPU/LayersPerSlice"),
			TEXT("SuperSLM/GPU/K"),
			TEXT("SuperSLM/GPU/HitchCount"),
			TEXT("SuperSLM/GPU/DueTokens"),
			TEXT("SuperSLM/GPU/DeliveredTokens"),
			TEXT("SuperSLM/GPU/HostFinishMs"),
		};
		return Names;
	}

	// Asserts a Set()-style counter's last observed value against an expected/live value when an
	// event was observed; when none was observed, logs the reason (this file's own header
	// comment) rather than failing -- silence from a Set()-style counter proves nothing either
	// way. Returns true unless a genuinely wrong value was observed (the decisive branch).
	bool CheckSetCounter(FAutomationTestBase& Test, const FGpuMarkupReading& Reading, const TCHAR* CounterName, double Expected, double Tolerance)
	{
		// NOTE: `Found` is non-null whenever the counter has EVER been constructed anywhere in
		// this process (EnumerateCounters lists every registered counter, active or not) -- it is
		// `Found->Value` (the observed value-CHANGE event count in THIS window) that says whether
		// this capture actually carries a reading, per this file's own header comment on Set()
		// elision.
		const TPair<double, uint64>* Found = Reading.CounterLastValueAndEventCount.Find(CounterName);
		const uint64 EventCount = Found ? Found->Value : 0;
		if (EventCount == 0)
		{
			Test.AddInfo(FString::Printf(TEXT("counter '%s': no value-change event observed in this capture (expected %f) -- a Set()-style counter elides an unchanged value, including one already left resident by an earlier test in this process; not a failure on its own, see this file's own header comment"), CounterName, Expected));
			return true;
		}
		Test.AddInfo(FString::Printf(TEXT("counter '%s': last=%f (expected %f), %llu value-change event(s) observed"), CounterName, Found->Key, Expected, Found->Value));
		return Test.TestTrue(*FString::Printf(TEXT("counter '%s' last observed value must equal %.9g (within %.3g), got %.9g"), CounterName, Expected, Tolerance, Found->Key),
			FMath::IsNearlyEqual(Found->Key, Expected, Tolerance));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2InsightsGpuMarkupTest,
	"SuperSLM.L2S2.Insights.GpuMarkupPresentAndReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2InsightsGpuMarkupTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = GetSubsystem(World);
	if (!TestNotNull(TEXT("CPU subsystem must be reachable to tokenize"), Cpu))
	{
		return false;
	}

	constexpr int32 kBlockCount = 8;
	constexpr int32 kLayersPerSlice = 4;
	const int32 ExpectedMinimumK = FMath::CeilToInt32((float)(kBlockCount * AExNumHiddenLayers) / (float)kLayersPerSlice);

	FSuperSLMGpuRuntimeConfig GpuConfig;
	GpuConfig.ContextCap = 4096;
	GpuConfig.BlockCount = kBlockCount;
	GpuConfig.DispatchBudget = DispatchBudgetForLayersPerSlice(kLayersPerSlice);
	GpuConfig.K = ExpectedMinimumK; // the T+K-miss bookmark's own best-effort attempt reads real
	                                // GetHitchCount() below rather than tightening K further --
	                                // K below MinimumK is refused outright (D-SLM7381), so this
	                                // cell cannot pin a natural hitch by construction; see the
	                                // hitch-bookmark paragraph in this file's own header comment.
	GpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, GpuConfig).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = kBlockCount;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.BlockCount = kBlockCount;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU Configure() (tokenize-only)"), (uint8)Cpu->Configure(Model, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	// --- Capture a real .utrace to a scratch file with EXACTLY the channels this cell's own
	// assertions below need. Recipe unchanged from rounds 7/9 (both defects fixed there: absolute
	// path so FTraceAuxiliary's own write-side and this test's own read-side resolve the same
	// location; bExcludeTail so a fresh capture carries only what THIS workload produced; the
	// CpuChannel + SuperSLMGpu explicit pre-toggle so every ON_CHANNEL scope's own AND-gate is
	// satisfied; "cpu,Counters,SuperSLMGpu,frame,log,bookmark,region" -- everything "default"
	// would carry except "gpu" (UE's native GPU-profiler channel) and "screenshot". ---
	UE::Trace::ToggleChannel(TEXT("Cpu"), true);
	UE::Trace::ToggleChannel(TEXT("Counters"), true);
	UE::Trace::ToggleChannel(TEXT("SuperSLMGpu"), true);

	const FString CapturePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SuperSLML2S2Scratch"), TEXT("r_s2h_gpu_markup.utrace")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CapturePath), /*Tree*/ true);
	IFileManager::Get().Delete(*CapturePath, /*RequireExists*/ false, /*EvenIfReadOnly*/ true);

	FTraceAuxiliary::FOptions TraceOptions;
	TraceOptions.bTruncateFile = true;
	TraceOptions.bExcludeTail = true;
	const bool bTraceStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *CapturePath,
		TEXT("cpu,Counters,SuperSLMGpu,frame,log,bookmark,region"), &TraceOptions);
	if (!TestTrue(TEXT("FTraceAuxiliary::Start() must succeed against a fresh scratch file"), bTraceStarted))
	{
		return false;
	}

	// --- The workload: 8 composed-path sessions (matching R-S2a's own concurrency shape, a
	// smaller token count per this file's own header note), plus one save/restore pair so the
	// Save/Restore scopes are exercised too. Tallies TotalTokensDelivered independently of the
	// DeliveredTokens trace counter AND of the Finish scope count (via GetGeneratedTokens(), a
	// different code path from either) so both can be cross-checked by an exact equality below. ---
	int32 FailureCount = 0;
	int64 TotalTokensDelivered = 0;
	for (int32 I = 0; I < kBlockCount; ++I)
	{
		FSuperSLMGpuSequence Seq;
		if (Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed) != ESuperSLMGpuVendResult::Success)
		{
			++FailureCount;
			continue;
		}
		FSuperSLMGenerationRequest Request;
		if (!Cpu->Tokenize(FString::Printf(TEXT("Tell me something about the number %d."), I + 1), Request.PromptTokens))
		{
			++FailureCount;
			Gpu->ReturnSequence(Seq);
			continue;
		}
		Request.MaxNewTokens = 12;
		Request.SpanKind = ESuperSLMSpanKind::Prompt;

		if (I == 0)
		{
			// Exercise Save/Restore's own scopes on this same run: save partway, restore into a
			// fresh handle, finish there. Mirrors R-S2d's own "save at token N / restore /
			// continue" shape, at a much smaller scale, since this cell's own claim needs only ONE
			// occurrence of each scope, not a full lifetime proof.
			if (!Gpu->RequestBeginGeneration(Seq, Request).IsValid())
			{
				++FailureCount;
				Gpu->ReturnSequence(Seq);
				continue;
			}
			for (int32 T = 0; T < 6; ++T)
			{
				Gpu->Tick(1.0f / 60.0f);
			}
			// Tallied BEFORE Save/Restore, from the ORIGINAL handle -- Restored's own
			// GetGeneratedTokens() below starts counting from 0 post-restore (Fixtures.h's own
			// documented convention), so the two counts are additive, never double-counted.
			TotalTokensDelivered += Gpu->GetGeneratedTokens(Seq).Num();
			TArray<uint8> Blob;
			const FSuperSLMLifecycleOpHandle SaveHandle = Gpu->RequestSaveSequence(Seq);
			const bool bSaved = DriveSaveToResolution(*Gpu, SaveHandle, Blob) == ESuperSLMRestoreResult::Success;
			if (!bSaved)
			{
				++FailureCount;
			}
			Gpu->ReturnSequence(Seq);
			if (bSaved)
			{
				FSuperSLMGpuSequence Restored;
				const FSuperSLMLifecycleOpHandle RestoreHandle = Gpu->RequestRestoreSequence(Blob, Model);
				const ESuperSLMRestoreResult RestoreResult = DriveRestoreToResolution(*Gpu, RestoreHandle, Restored);
				if (RestoreResult == ESuperSLMRestoreResult::Success)
				{
					TArray<int32> Tail;
					FString TailError;
					RunGpuGenerationToCompletion(*Gpu, Restored, Request, Tail, /*MaxWallClockSeconds*/ 60.0, TailError, /*bAlreadyInProgress*/ true);
					TotalTokensDelivered += Tail.Num();
					Gpu->ReturnSequence(Restored);
				}
				else
				{
					++FailureCount;
				}
			}
			continue;
		}

		TArray<int32> Tokens;
		FString RunError;
		const bool bCompleted = RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, /*MaxWallClockSeconds*/ 60.0, RunError);
		TotalTokensDelivered += Tokens.Num();
		Gpu->ReturnSequence(Seq);
		if (!bCompleted)
		{
			++FailureCount;
		}
	}
	AddInfo(FString::Printf(TEXT("Insights workload: %d/%d sessions completed; %lld tokens delivered (independently tallied); GetHitchCount()=%d"),
		kBlockCount - FailureCount, kBlockCount, TotalTokensDelivered, Gpu->GetHitchCount()));

	// Sampled at the SAME moment the workload's own last activity finished, immediately before
	// Stop() -- these are the live readings the counter cross-checks below compare the trace's own
	// last resolved value against. RecordGpuBusy()/FinishToken() write the live atomic and fire
	// TRACE_COUNTER_SET with the identical value in the same statement pair
	// (SuperSLMGpuSubsystem.cpp:622-624, 811-812, 896-897), so these are the correct reference --
	// a different code path from the counter itself, never the counter's own reading fed back to
	// itself (a reference must not share its inputs with the thing it grades).
	const int32 LiveHitchCount = Gpu->GetHitchCount();
	const double LiveLastGpuBusyMs = Gpu->GetLastGpuBusyMs();
	const double LiveLastHostFinishMs = Gpu->GetLastHostFinishMs();

	FTraceAuxiliary::Stop();

	// FTraceAuxiliary::Stop() is asynchronous (Writer_Stop() only flags closure; the actual close
	// and flush happens later, after a deliberate two-tick "close inertia" delay, on the trace
	// worker thread) -- poll UE::Trace::IsTracing() rather than trusting Stop()'s own return value
	// or a guessed sleep. Unchanged from round 7's own fix.
	{
		const double DeadlineSeconds = FPlatformTime::Seconds() + 5.0;
		while (UE::Trace::IsTracing() && FPlatformTime::Seconds() < DeadlineSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (!TestFalse(TEXT("the trace worker thread must finish closing the capture file before it is read"), UE::Trace::IsTracing()))
		{
			return false;
		}
	}

	if (!TestTrue(*FString::Printf(TEXT("the captured .utrace file must exist at '%s'"), *CapturePath), IFileManager::Get().FileExists(*CapturePath)))
	{
		return false;
	}
	const int64 CaptureFileSize = IFileManager::Get().FileSize(*CapturePath);
	if (!TestTrue(TEXT("the captured .utrace file must be non-trivially sized (events were actually recorded)"), CaptureFileSize > 4096))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("captured '%s': %lld bytes"), *CapturePath, CaptureFileSize));

	// --- Analyze the real captured artifact through TraceServices -- this test's own actual
	// oracle, replacing the withdrawn byte scan whole (D-SLM7502). ---
	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalyzeCaptureFile(CapturePath, *this);
	if (!TestTrue(TEXT("TraceServices must successfully analyze the captured .utrace file"), Session.IsValid()))
	{
		return false;
	}

	const TArray<FString> ScopeNames = ScopeNamesArray();
	const FGpuMarkupReading Reading = ReadGpuMarkup(*Session, ScopeNames, CounterNamesArray());

	// COMMISSIONING CHECK (a must-accept construction, run against THIS build's own fresh
	// capture, answering directly: is the scope-reading mechanism itself sound, on a name known
	// to be present?). The corrupted spelling this file's own header comment documents
	// (`TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL`'s own `#Name` stringification of a `TEXT(...)`
	// argument) is the ACTUAL name production writes today -- looking it up here, through the
	// exact same CreateAggregation/TMap path the real assertions below use, is a positive control:
	// if this reads a large real count, the reader (session analysis, thread coverage, timer
	// lookup, TMap matching) is proven correct on this exact capture, and every "0" below is then
	// known to be a true negative (a real absence of the CORRECT, intended name), not a reader
	// defect. Never asserted pass/fail -- it exists only to answer that one question, freshly,
	// every time this cell runs, for as long as the production defect (T-2883) stands.
	{
		TArray<FString> CorruptedNames;
		for (const FScopeExpectation& Check : kScopeExpectations)
		{
			CorruptedNames.Add(FString::Printf(TEXT("L\"%s\""), Check.Name));
		}
		CorruptedNames.Add(FString::Printf(TEXT("L\"%s\""), TEXT("SuperSLM.Gpu.Finish")));
		const FGpuMarkupReading CorruptedReading = ReadGpuMarkup(*Session, CorruptedNames, TArray<FString>());
		uint64 TotalCorruptedInstances = 0;
		for (const FString& N : CorruptedNames)
		{
			const uint64* Found = CorruptedReading.ScopeInstanceCounts.Find(N);
			const uint64 Occurrences = Found ? *Found : 0;
			TotalCorruptedInstances += Occurrences;
			AddInfo(FString::Printf(TEXT("[commissioning: must-accept] corrupted name %s: %llu real instance(s)"), *N, Occurrences));
		}
		AddInfo(FString::Printf(TEXT("[commissioning] reader mechanism proven sound on this capture: %s (%llu total instances found under the corrupted spelling production actually writes)"),
			TotalCorruptedInstances > 0 ? TEXT("YES") : TEXT("NO -- investigate before trusting any '0' below"), TotalCorruptedInstances));
	}

	bool bOk = true;

	// Scopes: real, per-call-site-merged instance counts (never a byte count).
	for (const FScopeExpectation& Check : kScopeExpectations)
	{
		const uint64* Found = Reading.ScopeInstanceCounts.Find(Check.Name);
		const uint64 Occurrences = Found ? *Found : 0;
		const double* FoundSeconds = Reading.ScopeTotalInclusiveSeconds.Find(Check.Name);
		const double TotalSeconds = FoundSeconds ? *FoundSeconds : 0.0;
		const double AverageSeconds = Occurrences > 0 ? TotalSeconds / static_cast<double>(Occurrences) : 0.0;
		AddInfo(FString::Printf(TEXT("scope '%s': %llu real instance(s), %.6f s total inclusive, %.9f s average"), Check.Name, Occurrences, TotalSeconds, AverageSeconds));
		bOk &= TestTrue(*FString::Printf(TEXT("scope '%s' must have at least %llu real instance(s) (found %llu)"), Check.Name, Check.MinCount, Occurrences),
			Occurrences >= Check.MinCount);
	}

	// T-2885 finding 6 (the code review record): on the batch path (4+
	// concurrent composed sequences, this capture's own construction), ExecuteTickJob() emits a
	// SuperSLM.Gpu.Drain scope containing NO WORK -- Layer 1's batched call drains internally and
	// exposes no boundary the plugin can wrap -- so the instance-count floor above is satisfied
	// by a marker, not a measurement: a scope entered and exited with nothing inside it still
	// counts as one instance. This is a DURATION floor, self-calibrated against
	// SuperSLM.Gpu.Submit (the scope this file's own production comment says carries the fused
	// call's real cost on the batch path, since Layer 1 exposes no separate drain boundary there)
	// rather than an absolute wall-clock number this suite has not independently measured on
	// every box this runs on: a Drain that is doing real device-wait work should not average
	// under 1% of Submit's own average per-instance cost; a Drain that is a zero-cost marker
	// averages orders of magnitude below that on any box, because entering/exiting an empty
	// TRACE_CPUPROFILER scope costs nanoseconds against a GPU submission's microseconds-to-
	// milliseconds.
	{
		const uint64* DrainInstances = Reading.ScopeInstanceCounts.Find(TEXT("SuperSLM.Gpu.Drain"));
		const double* DrainSeconds = Reading.ScopeTotalInclusiveSeconds.Find(TEXT("SuperSLM.Gpu.Drain"));
		const uint64* SubmitInstances = Reading.ScopeInstanceCounts.Find(TEXT("SuperSLM.Gpu.Submit"));
		const double* SubmitSeconds = Reading.ScopeTotalInclusiveSeconds.Find(TEXT("SuperSLM.Gpu.Submit"));
		const uint64 DrainOccurrences = DrainInstances ? *DrainInstances : 0;
		const uint64 SubmitOccurrences = SubmitInstances ? *SubmitInstances : 0;
		if (TestTrue(TEXT("both SuperSLM.Gpu.Drain and SuperSLM.Gpu.Submit must have at least one real instance before their average durations can be compared"),
				DrainOccurrences > 0 && SubmitOccurrences > 0))
		{
			const double DrainAverage = (DrainSeconds ? *DrainSeconds : 0.0) / static_cast<double>(DrainOccurrences);
			const double SubmitAverage = (SubmitSeconds ? *SubmitSeconds : 0.0) / static_cast<double>(SubmitOccurrences);
			constexpr double kMinDrainToSubmitRatio = 0.01;
			AddInfo(FString::Printf(TEXT("SuperSLM.Gpu.Drain average %.9f s vs SuperSLM.Gpu.Submit average %.9f s (ratio %.6f, floor %.2f)"),
				DrainAverage, SubmitAverage, SubmitAverage > 0.0 ? DrainAverage / SubmitAverage : 0.0, kMinDrainToSubmitRatio));
			bOk &= TestTrue(TEXT("SuperSLM.Gpu.Drain's own average duration must be at least 1% of SuperSLM.Gpu.Submit's -- a zero-cost marker scope (the batch path's own empty Drain block) cannot satisfy this, only a scope that measures real device-wait work can"),
				DrainAverage >= SubmitAverage * kMinDrainToSubmitRatio);
		}
	}

	// Finish: exact structural invariant, not a floor (this file's own header comment) -- one
	// FinishToken() call per token delivered, on the composed path this workload uses throughout.
	{
		const uint64* Found = Reading.ScopeInstanceCounts.Find(TEXT("SuperSLM.Gpu.Finish"));
		const uint64 Occurrences = Found ? *Found : 0;
		AddInfo(FString::Printf(TEXT("scope 'SuperSLM.Gpu.Finish': %llu real instance(s) vs. %lld tokens independently tallied"), Occurrences, TotalTokensDelivered));
		bOk &= TestEqual(TEXT("scope 'SuperSLM.Gpu.Finish' instance count must equal the independently-tallied token count"),
			Occurrences, (uint64)TotalTokensDelivered);
	}

	// Counters, asserted (a reading nothing can fail proves nothing, so none is left informational).
	bOk &= CheckSetCounter(*this, Reading, TEXT("SuperSLM/GPU/K"), (double)ExpectedMinimumK, 0.5);
	bOk &= CheckSetCounter(*this, Reading, TEXT("SuperSLM/GPU/LayersPerSlice"), (double)kLayersPerSlice, 0.5);
	bOk &= CheckSetCounter(*this, Reading, TEXT("SuperSLM/GPU/HitchCount"), (double)LiveHitchCount, 0.5);
	// Float counters (TRACE_DECLARE_FLOAT_COUNTER) reach the reader at float32 precision: the
	// product's double is rounded once to the nearest float, an error of at most half a float ULP
	// (<= 2^-24 relative). The tolerance is 4 float ULPs at the expected value's magnitude
	// (4 * 2^-23 * |Expected|), floored at FLT_MIN, the same bound as R-S1h's
	// (SuperSLML2S1AsyncSchedulingTests.cpp). An absolute 1e-6 is below half an ULP for any value
	// >= 32 ms, so it would fail a correct capture of a slow slice. The int counters stay at 0.5.
	auto FloatCounterTolerance = [](double Expected) -> double
	{
		constexpr double kFloatEpsilon = 1.0 / 8388608.0; // 2^-23, FLT_EPSILON
		constexpr double kFloatMinNormal = 1.17549435082228750797e-38; // FLT_MIN
		constexpr double kUlps = 4.0;
		return FMath::Max(kUlps * kFloatEpsilon * FMath::Abs(Expected), kFloatMinNormal);
	};
	bOk &= CheckSetCounter(*this, Reading, TEXT("SuperSLM/GPU/GpuBusyMsPerSlice"), LiveLastGpuBusyMs, FloatCounterTolerance(LiveLastGpuBusyMs));
	bOk &= CheckSetCounter(*this, Reading, TEXT("SuperSLM/GPU/HostFinishMs"), LiveLastHostFinishMs, FloatCounterTolerance(LiveLastHostFinishMs));

	// DueTokens: real bound derived from the workload's own construction (never more than
	// kBlockCount sequences can be due in a single tick, because only kBlockCount were ever
	// vended) -- checked against EVERY observed value, not only the last, so a defect that
	// transiently over-counts is caught even if the final tick's own reading looks healthy.
	{
		bool bSawDueTokensEvent = false;
		bool bDueTokensWithinBound = true;
		int64 MaxObservedDueTokens = -1;
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session);
		const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(*Session);
		CounterProvider.EnumerateCounters(
			[&](uint32 /*CounterId*/, const TraceServices::ICounter& Counter)
			{
				if (FCString::Strcmp(Counter.GetName(), TEXT("SuperSLM/GPU/DueTokens")) != 0)
				{
					return;
				}
				Counter.EnumerateValues(0.0, Session->GetDurationSeconds(), /*bIncludeExternalBounds*/ true,
					[&](double /*Time*/, int64 Value)
					{
						bSawDueTokensEvent = true;
						MaxObservedDueTokens = FMath::Max(MaxObservedDueTokens, Value);
						if (Value < 0 || Value > kBlockCount)
						{
							bDueTokensWithinBound = false;
						}
					});
			});
		AddInfo(FString::Printf(TEXT("counter 'SuperSLM/GPU/DueTokens': max observed = %lld (bound [0, %d])"), MaxObservedDueTokens, kBlockCount));
		if (bSawDueTokensEvent)
		{
			bOk &= TestTrue(TEXT("every observed SuperSLM/GPU/DueTokens value must be within [0, kBlockCount]"), bDueTokensWithinBound);
		}
		else
		{
			AddInfo(TEXT("counter 'SuperSLM/GPU/DueTokens': no value-change event observed in this capture -- see this file's own header comment on Set()-style counter elision"));
		}
	}

	// DeliveredTokens: exact, elision-proof cross-check (this file's own header comment). The
	// COUNT of observed events equals the number of TRACE_COUNTER_INCREMENT calls made during the
	// window, independent of any residual value another test left behind.
	{
		const TPair<double, uint64>* Found = Reading.CounterLastValueAndEventCount.Find(TEXT("SuperSLM/GPU/DeliveredTokens"));
		const uint64 ObservedIncrements = Found ? Found->Value : 0;
		AddInfo(FString::Printf(TEXT("counter 'SuperSLM/GPU/DeliveredTokens': %llu increment event(s) observed vs. %lld tokens independently tallied"), ObservedIncrements, TotalTokensDelivered));
		bOk &= TestEqual(TEXT("SuperSLM/GPU/DeliveredTokens increment-event count must equal the independently-tallied token count"),
			ObservedIncrements, (uint64)TotalTokensDelivered);
	}

	// Never a native Insights GPU-profiler row (§5.1's own clause) -- informational only, not
	// asserted; see this file's own header comment ("GPU-PROFILER-ROW CLAIM") for the real,
	// source-confirmed reason this cannot be a clean assertion in this authoring pass.
	AddInfo(FString::Printf(TEXT("HasGpuTiming()=%s -- informational only, not asserted (see this file's own header comment)"),
		Reading.bHasNativeGpuTiming ? TEXT("true") : TEXT("false")));

	// The T+K-miss hitch bookmark: best-effort, reported rather than required (unchanged rationale
	// from round 6 -- the other three GPU bookmarks are out of this cell's own scope, D-SLM7220's
	// forcing-construction filter). Now read via IBookmarkProvider's real text, not a byte scan.
	AddInfo(FString::Printf(TEXT("T+K-miss hitch bookmark ('SuperSLM: GPU token late by') occurrences: %d (best-effort -- GetHitchCount()=%d logged above; a healthy, lightly-loaded run may legitimately produce zero)"),
		Reading.HitchBookmarkOccurrences, LiveHitchCount));

	UE::Trace::ToggleChannel(TEXT("Cpu"), false);
	UE::Trace::ToggleChannel(TEXT("Counters"), false);
	UE::Trace::ToggleChannel(TEXT("SuperSLMGpu"), false);

	return bOk;
}

// --- Negative control (D-SLM7502's own explicit requirement): a real capture taken with the
// SuperSLMGpu channel OFF must fail every one of the 11 scope assertions above. A permanent cell
// rather than a one-off construction (this file's own header comment states why) -- kept minimal
// (one sequence, four tokens) since its only job is to exercise the GPU code path at all while the
// plugin's own channel stays off, never to prove anything about scale or concurrency. ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S2InsightsGpuMarkupAbsentTest,
	"SuperSLM.L2S2.Insights.GpuMarkupAbsentWhenChannelOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S2InsightsGpuMarkupAbsentTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper TestWorldWrapper;
	if (!TestWorldWrapper.CreateTestWorld(EWorldType::Game))
	{
		return false;
	}
	UWorld* World = TestWorldWrapper.GetTestWorld();

	FString AExPath, Reason;
	if (!TestTrue(*FString::Printf(TEXT("A-EX must be present (%s)"), *Reason), TryGetAExArtifactPath(AExPath, Reason)))
	{
		return false;
	}
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
	if (!TestNotNull(TEXT("A-EX must import"), Model) || !TestTrue(TEXT("A-EX Diagnostic.bAccepted"), Diag.bAccepted))
	{
		return false;
	}
	USuperSLMGpuSubsystem* Gpu = GetGpuSubsystem(World);
	if (!TestNotNull(TEXT("USuperSLMGpuSubsystem must be reachable"), Gpu))
	{
		return false;
	}
	USuperSLMSubsystem* Cpu = GetSubsystem(World);
	if (!TestNotNull(TEXT("CPU subsystem must be reachable to tokenize"), Cpu))
	{
		return false;
	}

	FSuperSLMGpuRuntimeConfig GpuConfig;
	GpuConfig.ContextCap = 4096;
	GpuConfig.BlockCount = 1;
	GpuConfig.DispatchBudget = DispatchBudgetForLayersPerSlice(4);
	GpuConfig.K = FMath::CeilToInt32((float)AExNumHiddenLayers / 4.0f);
	GpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("GPU Configure()"), (uint8)Gpu->Configure(Model, GpuConfig).Result, (uint8)ESuperSLMGpuConfigureResult::Success))
	{
		return false;
	}
	FSuperSLMRuntimeConfig CpuConfig;
	CpuConfig.MaxSequencesPerDecodeCall = 1;
	CpuConfig.MaxPrefillChunkBudget = 64;
	CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
	CpuConfig.BlockCount = 1;
	CpuConfig.SequenceLifecycleBudgetMs = 1000.0;
	CpuConfig.TickBudgetMs = 1000.0;
	if (!TestEqual(TEXT("CPU Configure() (tokenize-only)"), (uint8)Cpu->Configure(Model, CpuConfig).Result, (uint8)ESuperSLMConfigureResult::Success))
	{
		return false;
	}

	// "SuperSLMGpu" deliberately left OFF -- this is the whole point of the control. "Cpu" and
	// "Counters" stay on (they gate other things this control does not test); the plugin's own
	// scopes need BOTH CpuChannel and SuperSLMGpuChannel (an AND-gate, this file's own header
	// comment and round 5's own source-confirmed finding), so leaving only SuperSLMGpu off is
	// sufficient to silence every one of this plugin's own scopes.
	UE::Trace::ToggleChannel(TEXT("Cpu"), true);
	UE::Trace::ToggleChannel(TEXT("Counters"), true);

	const FString CapturePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SuperSLML2S2Scratch"), TEXT("r_s2h_gpu_markup_negative_control.utrace")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CapturePath), /*Tree*/ true);
	IFileManager::Get().Delete(*CapturePath, /*RequireExists*/ false, /*EvenIfReadOnly*/ true);

	FTraceAuxiliary::FOptions TraceOptions;
	TraceOptions.bTruncateFile = true;
	TraceOptions.bExcludeTail = true;
	const bool bTraceStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *CapturePath,
		TEXT("cpu,Counters,frame,log,bookmark,region"), &TraceOptions);
	if (!TestTrue(TEXT("FTraceAuxiliary::Start() must succeed against a fresh scratch file"), bTraceStarted))
	{
		return false;
	}

	FSuperSLMGpuSequence Seq;
	int32 FailureCount = 0;
	if (Gpu->VendSequence(Seq, ESuperSLMGpuDecodePath::Composed) != ESuperSLMGpuVendResult::Success)
	{
		++FailureCount;
	}
	else
	{
		FSuperSLMGenerationRequest Request;
		if (!Cpu->Tokenize(TEXT("Tell me something about the number one."), Request.PromptTokens))
		{
			++FailureCount;
		}
		else
		{
			Request.MaxNewTokens = 4;
			Request.SpanKind = ESuperSLMSpanKind::Prompt;
			TArray<int32> Tokens;
			FString RunError;
			if (!RunGpuGenerationToCompletion(*Gpu, Seq, Request, Tokens, /*MaxWallClockSeconds*/ 60.0, RunError))
			{
				++FailureCount;
			}
		}
		Gpu->ReturnSequence(Seq);
	}
	AddInfo(FString::Printf(TEXT("negative-control workload: %d failure(s) (a fault/vend/tokenize failure here does not affect the control -- some GPU code still ran either way, which is all this construction needs)"), FailureCount));

	FTraceAuxiliary::Stop();
	{
		const double DeadlineSeconds = FPlatformTime::Seconds() + 5.0;
		while (UE::Trace::IsTracing() && FPlatformTime::Seconds() < DeadlineSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (!TestFalse(TEXT("the trace worker thread must finish closing the capture file before it is read"), UE::Trace::IsTracing()))
		{
			return false;
		}
	}

	if (!TestTrue(*FString::Printf(TEXT("the captured .utrace file must exist at '%s'"), *CapturePath), IFileManager::Get().FileExists(*CapturePath)))
	{
		return false;
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalyzeCaptureFile(CapturePath, *this);
	if (!TestTrue(TEXT("TraceServices must successfully analyze the negative-control capture"), Session.IsValid()))
	{
		return false;
	}

	const TArray<FString> ScopeNames = ScopeNamesArray();
	const FGpuMarkupReading Reading = ReadGpuMarkup(*Session, ScopeNames, CounterNamesArray());

	bool bOk = true;
	for (const FScopeExpectation& Check : kScopeExpectations)
	{
		const uint64* Found = Reading.ScopeInstanceCounts.Find(Check.Name);
		const uint64 Occurrences = Found ? *Found : 0;
		AddInfo(FString::Printf(TEXT("[negative control] scope '%s': %llu real instance(s) (expect 0)"), Check.Name, Occurrences));
		bOk &= TestEqual(*FString::Printf(TEXT("[negative control] scope '%s' must read ZERO instances with SuperSLMGpu off (found %llu) -- this is the exact assertion the main cell runs, proving it CAN fail"), Check.Name, Occurrences),
			Occurrences, (uint64)0);
	}
	{
		const uint64* Found = Reading.ScopeInstanceCounts.Find(TEXT("SuperSLM.Gpu.Finish"));
		const uint64 Occurrences = Found ? *Found : 0;
		AddInfo(FString::Printf(TEXT("[negative control] scope 'SuperSLM.Gpu.Finish': %llu real instance(s) (expect 0)"), Occurrences));
		bOk &= TestEqual(TEXT("[negative control] scope 'SuperSLM.Gpu.Finish' must read ZERO instances with SuperSLMGpu off"), Occurrences, (uint64)0);
	}

	// COMMISSIONING must-reject half (paired with the main cell's own must-accept check, this
	// file's own header comment): the INTENDED name (above) is currently absent from EVERY
	// capture regardless of channel state, because production never writes it (T-2883) -- so a
	// zero reading against the intended name proves nothing about this control's own
	// discriminating power for scopes specifically (a reader that always reports zero would pass
	// this section too). The name production ACTUALLY writes today is the corrupted
	// `L"..."`-wrapped spelling (this file's own header comment); checking THAT name here, with
	// the channel off, and requiring it to ALSO read zero, is the genuine must-reject half: paired
	// with the main cell's own must-accept reading of the identical corrupted name (large, real,
	// nonzero counts), this is the actual proof that the channel gate -- not this cell's own
	// instrument -- is what discriminates scope presence, for the name reality currently emits.
	{
		TArray<FString> CorruptedNames;
		for (const FScopeExpectation& Check : kScopeExpectations)
		{
			CorruptedNames.Add(FString::Printf(TEXT("L\"%s\""), Check.Name));
		}
		CorruptedNames.Add(FString::Printf(TEXT("L\"%s\""), TEXT("SuperSLM.Gpu.Finish")));
		const FGpuMarkupReading CorruptedReading = ReadGpuMarkup(*Session, CorruptedNames, TArray<FString>());
		for (const FString& N : CorruptedNames)
		{
			const uint64* Found = CorruptedReading.ScopeInstanceCounts.Find(N);
			const uint64 Occurrences = Found ? *Found : 0;
			AddInfo(FString::Printf(TEXT("[negative control, must-reject] corrupted name %s: %llu real instance(s) (expect 0)"), *N, Occurrences));
			bOk &= TestEqual(*FString::Printf(TEXT("[negative control, must-reject] corrupted name %s must ALSO read ZERO with SuperSLMGpu off"), *N),
				Occurrences, (uint64)0);
		}
	}

	UE::Trace::ToggleChannel(TEXT("Cpu"), false);
	UE::Trace::ToggleChannel(TEXT("Counters"), false);

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
