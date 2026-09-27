#pragma once

// T-2818 (L2-S3 red suite). Locates the real artifacts and checkpoints R-S3a-R-S3e run
// against. This is a NEW fixture header, separate from
// Tests/Fixtures/SuperSLML2S1Fixtures.h -- T-2805/T-2815/T-2816 are editing files under
// Plugins/SuperSLMUnreal concurrently with this suite, and this suite's own contract
// creates only new files. Where an L2-S1 fixture already exists (A-AD's paths, A-EX's
// override-and-fallback resolution, the subsystem/tick-loop helpers), this header INCLUDES and
// REUSES SuperSLML2S1Fixtures.h's own declarations rather than duplicating them -- reading an
// existing file to reuse its symbols is not editing it.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMJson.h" // the plugin's one JSON-object read (review R4-W1)
#include "SuperSLMGpuRuntimeConfig.h"
#include "SuperSLMGpuSchemaHandle.h"
#include "SuperSLMGpuSubsystem.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMRuntimeConfig.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"
// Resolved from the module's Private/ include root (UBT adds it, UEBuildModuleCPP.cs), because
// after unstaging this header sits in Tests/Fixtures/L2S3/, one level below the file it names.
// The bare "SuperSLML2S1Fixtures.h" this line used to carry resolves from neither place
// (2026-09-25, t2818-l2s3-schema-cells record §5).
#include "Tests/Fixtures/SuperSLML2S1Fixtures.h"
#include "Tests/Fixtures/SuperSLML2S2Fixtures.h"
// Layer 1's per-layer dispatch count (DispatchesPerLayer, gpu_port.h): the one source the
// plan's GPU budgets derive from (§2.5 row 8, §7 item 11). Wrapped because it is vendored C++.
THIRD_PARTY_INCLUDES_START
#include "superslm/gpu_port.h"
THIRD_PARTY_INCLUDES_END

namespace SuperSLML2S3Fixtures
{
	// --- A-EX, A-AD: reuse L2-S1's own resolution (verified present and hash-matched by this
	// suite's own authoring pass; see the red-suite record §3) ---
	using SuperSLML2S1Fixtures::TryGetAExArtifactPath;
	using SuperSLML2S1Fixtures::AExSchemaName;
	using SuperSLML2S1Fixtures::AAdBaseModelPath;
	using SuperSLML2S1Fixtures::AAdAdapterPath;
	using SuperSLML2S1Fixtures::GetSubsystem;
	using SuperSLML2S1Fixtures::RunGenerationToCompletion;

	// The GPU backend's own subsystem (USuperSLMGpuSubsystem, SuperSLMGpuSubsystem.h, T-2816;
	// per D-SLM7337 folded into the SuperSLMUnreal runtime module itself, not a separate
	// SuperSLMUnrealGPU module) -- mirrors SuperSLML2S1Fixtures::GetSubsystem()'s own
	// world-context walk exactly, for the SEPARATE GameInstanceSubsystem the GPU backend still
	// lives in as a C++ TYPE (SuperSLMGpuSubsystem.h's own header comment: "A SEPARATE
	// GameInstance subsystem from the CPU backend's USuperSLMSubsystem" -- module folding does
	// not change that it is its own UCLASS). nullptr is a normal result before Configure() has
	// ever run on this GameInstance, not a fixture failure.
	inline USuperSLMGpuSubsystem* GetGpuSubsystem()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Editor || Context.WorldType == EWorldType::Game) &&
				Context.OwningGameInstance != nullptr)
			{
				return Context.OwningGameInstance->GetSubsystem<USuperSLMGpuSubsystem>();
			}
		}
		return nullptr;
	}

	// --- R-S3d: checkpoint directories with an already-recorded converter outcome ---

	// Qwen2.5-0.5B-Instruct -- a real, already-converted artifact exists (A-CPU,
	// SuperSLML2S1Fixtures::ACpuArtifactPath()), so this checkpoint's converter outcome is
	// recorded PASS (D-SLM7229).
	inline FString Qwen25_05B_InstructCheckpointDir()
	{
		return SuperSLMTestDataPaths::HfCache(TEXT("hub/models--Qwen--Qwen2.5-0.5B-Instruct/snapshots/7ae557604adf67be50417f59c2c2f167def9a775"));
	}

	// Qwen2.5-1.5B-Instruct -- A-AD's own base model is a real, already-converted artifact of
	// this checkpoint, so its converter outcome is recorded PASS (plan §9's A-AD citation).
	inline FString Qwen25_15B_InstructCheckpointDir()
	{
		return SuperSLMTestDataPaths::HfCache(TEXT("hub/models--Qwen--Qwen2.5-1.5B-Instruct/snapshots/989aa7980e4cf806f80c7fef2b1adb7bc71aa306"));
	}

	// Qwen3-Embedding-0.6B -- the standard conversion path rejects this checkpoint (D-SLM4714,
	// D-SLM4719, D-SLM4721): its declared geometry violates hidden_size ==
	// num_attention_heads * head_dim (hidden_size=1024, num_attention_heads=16, head_dim=128 ->
	// 16*128 = 2048 != 1024) and the engine's forward pipeline has no per-head QK-norm slot at
	// v1.5.0. Recorded outcome: FAIL.
	inline FString Qwen3Embedding06BCheckpointDir()
	{
		return SuperSLMTestDataPaths::HfCache(TEXT("hub/models--Qwen--Qwen3-Embedding-0.6B/snapshots/97b0c614be4d77ee51c0cef4e5f07c00f9eb65b3"));
	}

	struct FRecordedConversionOutcome
	{
		FString CheckpointDir;
		bool bExpectedPass = false;
		FString Label;
		// For a recorded failure: the recorded cause, as words a blocker naming it must contain (any
		// one, case-insensitive). Round 5 (review W3): the verdict alone agrees with the converter
		// even when the pre-check rejects on the architecture name, which is not the recorded cause.
		TArray<FString> RecordedCauseWords;
		FString RecordedCause;
	};

	// The three checkpoints R-S3d's own coverage-model cell names by architecture family
	// ("Qwen2.5 0.5B/1.5B-Instruct, Qwen3-Embedding-0.6B", plan §9 R-S3d), each verified present
	// on this box (config.json read directly by this suite's own authoring pass) with a
	// converter outcome already on record.
	inline TArray<FRecordedConversionOutcome> RecordedConversionOutcomes()
	{
		return {
			{Qwen25_05B_InstructCheckpointDir(), true, TEXT("Qwen2.5-0.5B-Instruct")},
			{Qwen25_15B_InstructCheckpointDir(), true, TEXT("Qwen2.5-1.5B-Instruct")},
			{Qwen3Embedding06BCheckpointDir(), false, TEXT("Qwen3-Embedding-0.6B"),
				{TEXT("head_dim"), TEXT("qk_norm"), TEXT("qk-norm")},
				TEXT("the head_dim geometry (hidden_size 1024 != 16 heads x head_dim 128) and the missing per-head QK-norm slot (D-SLM4714, D-SLM4719, D-SLM4721)")},
		};
	}

	// --- The demo prompt (plan §8's potion-shop schema) ---

	inline FString DemoPromptText()
	{
		return TEXT("A customer walks up and asks to buy two health potions, politely.");
	}

	// A shorter prompt for the concurrent/sweep cells (R-S3e), where the same prompt runs many
	// times across many configs -- kept identical in content but named separately so a future
	// change to DemoPromptText() (a longer scene-facing line) does not silently change R-S3e's
	// own per-config wall-clock cost without a deliberate edit here too.
	inline FString SweepPromptText()
	{
		return TEXT("A customer asks to buy one mana potion.");
	}

	inline FString HexEncodeDigest(const uint8 Digest[32])
	{
		return BytesToHex(Digest, 32);
	}

	// ====================================================================================
	// The schema-constrained-decoding checkbox cells (R-S3e checkbox-on arm, R-S3g, R-S3h;
	// plan §8, §9, §10.4; T-2853 design §5-§6; the test record).
	// ====================================================================================

	// The world-scoped GPU accessor. The no-argument GetGpuSubsystem() above predates
	// FTestWorldWrapper (T-2815 build log D1: an editor -Test run has no game instance to scan);
	// the cells added on 2026-09-25 use this one, from SuperSLML2S2Fixtures.
	using SuperSLML2S2Fixtures::GetGpuSubsystem;
	using SuperSLML2S2Fixtures::DispatchBudgetForLayersPerSlice;
	using SuperSLML2S2Fixtures::AExNumHiddenLayers;
	using SuperSLML2S2Fixtures::AExStopTokenImEnd;
	using SuperSLML2S2Fixtures::AExStopTokenEndOfText;

	// The second named schema the §10.4 A-EX rebuild compiles into A-EX's one SCM1 section beside
	// potion_shop_order (plan §8, D-SLM7668), and the one key its object carries (D-SLM7432).
	inline const TCHAR* PromptResultSchemaName() { return TEXT("prompt_result"); }
	inline const TCHAR* PromptResultKey() { return TEXT("Prompt_Result"); }

	// R-S3g's prompt: a real, user-shaped question with a one-sentence answer, so the object
	// closes well inside the budget.
	inline FString RS3gPromptText()
	{
		return TEXT("What does a mana potion do? Answer in one sentence.");
	}

	// R-S3h's prompt: "one real prompt whose natural answer runs to a full sentence" (plan §9). It
	// asks for three sentences, so the answer runs far past RS3hTruncatedMaxNewTokens.
	inline FString RS3hPromptText()
	{
		return TEXT("Describe the inside of a small potion shop in three sentences, as its shopkeeper would.");
	}

	// The budget both backends run R-S3g under, and R-S3h's reference ("MaxNewTokens raised past
	// acceptance", plan §9). 256 tokens is about 190 English words, and 256 positions plus the
	// prompt sit far inside A-EX's context_cap of 4096.
	inline constexpr int32 RS3gMaxNewTokens = 256;
	inline constexpr int32 RS3hReferenceMaxNewTokens = 256;

	// R-S3h's truncating budget. UNDERIVED: the plan leaves it to the implementation to confirm against
	// A-EX's tokenizer on the machine (plan §9 R-S3h). Reasoning, 2026-09-25, no tokenizer in
	// the authoring container:
	//  - Floor. The schema's literal prefix {"Prompt_Result": " is 19 characters. Qwen2.5's BPE
	//    covers it in about 5 tokens ({" / Prompt / _Result / ": / space-quote); at the absolute
	//    worst, one character per token, it takes 19. 24 leaves at least 5 tokens of answer, so
	//    the recovered text cannot be empty through the prefix alone.
	//  - Ceiling. A three-sentence answer is roughly 40 to 80 tokens, so at about 5 tokens of
	//    prefix the cut lands some 19 tokens into an answer that needs 35 or more to close.
	// Both edges fail loudly rather than pass: an answer that closes inside 24 tokens fails the
	// "does not parse" assertion and the reference-length precondition; a prefix that eats the
	// whole budget fails the non-empty assertion.
	inline constexpr int32 RS3hTruncatedMaxNewTokens = 24;

	// A GPU DispatchBudget of whole layers, as FSuperSLMGpuRuntimeConfig::DispatchBudget requires
	// (Layer-1 dispatch units, never raw layers; below one layer's worth, Configure() refuses
	// InvalidDispatchBudget). bHasQkNorm is the model's own: false for Qwen2.5 (A-EX, A-AD), 25
	// dispatches per layer for a QK-norm model.
	inline uint32 DispatchBudgetForLayers(int32 Layers, bool bHasQkNorm)
	{
		check(Layers >= 1);
		return static_cast<uint32>(Layers) * superslm_gpu::DispatchesPerLayer(bHasQkNorm);
	}

	// What SetUpQueryWindowBackends() hands back. Every pointer is non-null when it returns true.
	struct FQueryWindowBackends
	{
		USuperSLMModel* Model = nullptr;
		USuperSLMSubsystem* Cpu = nullptr;
		USuperSLMGpuSubsystem* Gpu = nullptr;
	};

	// Imports A-EX and configures BOTH backends on World's game instance. Every cell that uses it
	// claims both backends (plan §9 R-S3e, R-S3g, R-S3h), so every unknown is a loud failure
	// naming CellName, never a skipped arm: A-EX absent or refused, a subsystem unreachable, a
	// Configure() that does not report Success (the GPU's DeviceUnavailable and
	// ShaderStagingIncomplete included), and -- when bRequirePromptResultSchema -- a `prompt_result`
	// that does not resolve on either backend, which is what an A-EX without the §10.4 rebuild
	// looks like.
	//
	// GPU configuration: GpuLayersPerTick whole layers per tick (DispatchBudget in Layer-1
	// dispatches, layers x DispatchesPerLayer), defaulting to the whole model. The per-tick budget
	// is shared by every due sequence and the controller caps each sequence's slice at
	// GetLayersPerTick() (SuperSLMQuery.cpp StartOnBackend), so the round-4 value of
	// ONE layer per tick capped every Frame Budget setting at 1 and made R-S3e's GPU sweep vacuous
	// (2026-09-25 review, record §10). K is at least the larger of the two paths' floors
	// (SuperSLMGpuRuntimeConfig.h: composed = ceil(BlockCount x layers / layers_per_tick),
	// one-call = BlockCount), so Configure() cannot refuse KBelowMinimum on either path.
	inline bool SetUpQueryWindowBackends(
		FAutomationTestBase& Test,
		UWorld* World,
		const TCHAR* CellName,
		int32 BlockCount,
		bool bRequirePromptResultSchema,
		FQueryWindowBackends& Out,
		int32 GpuLayersPerTick = AExNumHiddenLayers)
	{
		check(GpuLayersPerTick >= 1 && GpuLayersPerTick <= AExNumHiddenLayers);
		FString AExPath, Reason;
		if (!TryGetAExArtifactPath(AExPath, Reason))
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), CellName, *Reason));
			return false;
		}
		FSuperSLMImportDiagnostic Diag;
		Out.Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
		if (!Test.TestNotNull(TEXT("A-EX imports"), Out.Model) || !Test.TestTrue(TEXT("A-EX accepted"), Diag.bAccepted))
		{
			Test.AddError(FString::Printf(TEXT("%s: A-EX import: %s"), CellName, *Diag.Message));
			return false;
		}

		FString Error;
		if (bRequirePromptResultSchema)
		{
			FSuperSLMSchemaHandle CpuSchema;
			if (!FSuperSLMSchemaLookup::LookupByName(*Out.Model, PromptResultSchemaName(), CpuSchema, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s: A-EX at '%s' carries no '%s' schema, so it is not the plan "
					"§10.4 rebuild (prompt_result compiled beside potion_shop_order in one SCM1 section). %s"),
					CellName, *AExPath, PromptResultSchemaName(), *Error));
				return false;
			}
		}

		Out.Cpu = GetSubsystem(World);
		Out.Gpu = GetGpuSubsystem(World);
		if (!Test.TestNotNull(TEXT("CPU subsystem reachable"), Out.Cpu) ||
			!Test.TestNotNull(TEXT("GPU subsystem reachable"), Out.Gpu))
		{
			return false;
		}

		FSuperSLMRuntimeConfig CpuConfig;
		CpuConfig.MaxSequencesPerDecodeCall = BlockCount;
		CpuConfig.MaxPrefillChunkBudget = 512;
		CpuConfig.MaxLayerBudget = AExNumHiddenLayers;
		CpuConfig.BlockCount = BlockCount;
		CpuConfig.SequenceLifecycleBudgetMs = 100000.0;
		CpuConfig.TickBudgetMs = 100000.0;
		const FSuperSLMConfigureReport CpuReport = Out.Cpu->Configure(Out.Model, CpuConfig);
		if (!Test.TestEqual(TEXT("CPU Configure() result"), (uint8)CpuReport.Result, (uint8)ESuperSLMConfigureResult::Success))
		{
			return false;
		}

		FSuperSLMGpuRuntimeConfig GpuConfig;
		GpuConfig.ContextCap = 4096;
		GpuConfig.BlockCount = BlockCount;
		GpuConfig.DispatchBudget = DispatchBudgetForLayers(GpuLayersPerTick, SuperSLML2S2Fixtures::AExHasQkNorm);
		// BlockCount x L covers the composed floor at any GpuLayersPerTick >= 1 and the one-call floor.
		GpuConfig.K = FMath::Max(BlockCount * AExNumHiddenLayers, BlockCount);
		GpuConfig.TickBudgetMs = 100000.0;
		const FSuperSLMGpuConfigureReport GpuReport = Out.Gpu->Configure(Out.Model, GpuConfig);
		if (GpuReport.Result != ESuperSLMGpuConfigureResult::Success)
		{
			Test.AddError(FString::Printf(TEXT("%s: GPU Configure() reported %d (%s). This cell claims the GPU "
				"backend on the dev box's RTX 2080 SUPER; a CPU-only pass would be a plausible default, so "
				"the cell fails instead."), CellName, (int32)GpuReport.Result, *GpuReport.Message));
			return false;
		}

		if (bRequirePromptResultSchema)
		{
			FSuperSLMGpuSchemaHandle GpuSchema;
			if (!FSuperSLMGpuSchemaLookup::LookupByName(*Out.Model, PromptResultSchemaName(), GpuSchema, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s: '%s' resolves on the CPU but not on the GPU: %s"),
					CellName, PromptResultSchemaName(), *Error));
				return false;
			}
		}
		return true;
	}

	// Parses Raw as exactly one JSON object. False, with Why, on any failure.
	//
	// Review round 4, R4-W1: through the plugin's shared read, SuperSLMJson::TryReadObject
	// (SuperSLMJson.h), which
	// refuses input ending in a backslash before UE 5.8's TJsonReader overruns on it
	// (JsonReader.h:666, BufferReader.h:52). R-S3h's truncated answers can end that way, and this
	// suite then reads JSON exactly as the product does.
	inline bool TryParseJsonObject(const FString& Raw, TSharedPtr<FJsonObject>& OutObject, FString& OutWhy)
	{
		return SuperSLMJson::TryReadObject(Raw, OutObject, OutWhy);
	}

	// The one-field check R-S3g asserts: the object has exactly one key, it is `Prompt_Result`
	// compared CASE-SENSITIVELY (FJsonObject::Values is a TMap<FString, ...>, whose key compare
	// ignores case, so Contains()/HasField() would accept "prompt_result"), and its value is a JSON
	// string. OutValue is the unescaped string. False, with Why, otherwise.
	inline bool TryReadPromptResultField(const FJsonObject& Object, FString& OutValue, FString& OutWhy)
	{
		if (Object.Values.Num() != 1)
		{
			OutWhy = FString::Printf(TEXT("object has %d keys, expected exactly 1"), Object.Values.Num());
			return false;
		}
		// L2-S3 mechanical repair (2026-09-25): at UE 5.8 FJsonObject::Values is keyed by
		// UE::FSharedString (Dom/JsonObject.h, UE_JSONOBJECT_LEGACY_STRING_KEYS 0), whose map compare
		// is case-sensitive; `auto` plus operator* reads the key under either key type.
		for (const auto& Pair : Object.Values)
		{
			const FString Key(*Pair.Key);
			if (!Key.Equals(PromptResultKey(), ESearchCase::CaseSensitive))
			{
				OutWhy = FString::Printf(TEXT("the one key is '%s', expected '%s'"), *Key, PromptResultKey());
				return false;
			}
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
			{
				OutWhy = FString::Printf(TEXT("'%s' is not a JSON string"), PromptResultKey());
				return false;
			}
			OutValue = Pair.Value->AsString();
			// Review round 2, N4: UE's reader keeps \u0000 as a NUL, and the displayed text shows it
			// as U+FFFD (SuperSLMPromptResult.cpp); the reference applies the same mapping, so R-S3h's
			// prefix oracle compares like with like.
			for (int32 I = 0; I < OutValue.Len(); ++I)
			{
				if (OutValue[I] == TCHAR(0))
				{
					OutValue[I] = TCHAR(0xFFFD);
				}
			}
		}
		return true;
	}

	// Where the Prompt_Result string's content begins in a raw constrained output: after `{`, the
	// key "Prompt_Result" (case-sensitive), `:` and the opening `"`, with JSON whitespace allowed
	// between tokens. INDEX_NONE when Raw does not carry that literal prefix. This reads the
	// schema's fixed opening (T-2853 design §5: "known at schema-compile time") without assuming
	// how the compiler spaced it.
	inline int32 FindPromptResultContentStart(const FString& Raw)
	{
		int32 I = 0;
		auto SkipWhitespace = [&Raw, &I]()
		{
			while (I < Raw.Len() && (Raw[I] == TEXT(' ') || Raw[I] == TEXT('\t') || Raw[I] == TEXT('\n') || Raw[I] == TEXT('\r')))
			{
				++I;
			}
		};
		auto Expect = [&Raw, &I](const TCHAR* Literal)
		{
			const int32 Len = FCString::Strlen(Literal);
			if (Raw.Len() - I < Len || FCString::Strncmp(*Raw + I, Literal, Len) != 0)
			{
				return false;
			}
			I += Len;
			return true;
		};
		SkipWhitespace();
		if (!Expect(TEXT("{"))) { return INDEX_NONE; }
		SkipWhitespace();
		if (!Expect(TEXT("\"Prompt_Result\""))) { return INDEX_NONE; }
		SkipWhitespace();
		if (!Expect(TEXT(":"))) { return INDEX_NONE; }
		SkipWhitespace();
		if (!Expect(TEXT("\""))) { return INDEX_NONE; }
		return I;
	}

	inline FString StopReasonName(const TOptional<ESuperSLMQueryStopReason>& Reason)
	{
		if (!Reason.IsSet())
		{
			return TEXT("unset");
		}
		switch (Reason.GetValue())
		{
			case ESuperSLMQueryStopReason::Completed: return TEXT("Completed");
			case ESuperSLMQueryStopReason::SchemaRejected: return TEXT("SchemaRejected");
			case ESuperSLMQueryStopReason::BudgetExhausted: return TEXT("BudgetExhausted");
		}
		return FString::Printf(TEXT("unknown enumerator %d"), (int32)Reason.GetValue());
	}
}
