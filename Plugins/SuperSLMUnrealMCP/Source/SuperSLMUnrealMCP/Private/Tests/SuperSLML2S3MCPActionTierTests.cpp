// The MCP toolset's automation cells. What they hold the toolset to: a batch of 20 constrained
// inferences writes 20 schema-valid rows into a UDataTable; one undo restores the table exactly;
// a re-run gives identical rows; the same holds for the read-transform-write tool over a source
// table; DescribeModel reports the model inspector's own figures, field by field; RunPreCheck
// reports the pre-check's figures; no read-tier call changes any asset; and every refusal
// (unknown model, schema or checkpoint, empty batch, an open editor transaction, a source that is
// also the target) is a named error that writes nothing.
//
// Asynchronous tools. Each tool returns a pending UToolCallAsyncResultString and completes it on a
// later frame, so each cell is a chain of latent commands and never waits inside one call:
// - a command starts a tool and holds its result (TStrongObjectPtr);
// - the next command returns false every frame until bIsComplete, then records Value and Error;
// - a deadline turns a result that never completes into a named failure, never a hang or a pass.
// A success completes with SetValue (a JSON object) and an empty Error. A failure completes with
// SetError (the message) and no Value. A cell that expects success fails on a non-empty Error, and
// a cell that expects a refusal fails on an empty one.
//
// Isolation. The write sink refuses while any editor transaction is open, so every cell asserts
// that none is open when it starts and when it ends; a cell that left one open would otherwise
// fail the next cell's writes under that cell's name.
//
// A missing artifact fails the cell by name; nothing here passes on a default it could not check.

#include "Misc/AutomationTest.h"

#include "Tests/SuperSLMPotionShopOrderRow.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperSLMEditorRuntimeHost.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMToolset.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	// The row type is FSuperSLMPotionShopOrderRow (Tests/SuperSLMPotionShopOrderRow.h): a USTRUCT
	// with UPROPERTY Intent, Item, Quantity and bPolite. A decoded field that does not match it
	// is a rejected write, never a partial one.

	TSharedPtr<FJsonObject> ParseTool(const FString& Result)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Result);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	const TCHAR* TableDest = TEXT("/Game/SuperSLMMCPTests/PotionShopOrders");
	const TCHAR* TablePath = TEXT("/Game/SuperSLMMCPTests/PotionShopOrders.PotionShopOrders");
	// The read-transform-write cell's source table, a different asset from the target.
	const TCHAR* SourceTableDest = TEXT("/Game/SuperSLMMCPTests/CustomerLines");
	const TCHAR* SourceTablePath = TEXT("/Game/SuperSLMMCPTests/CustomerLines.CustomerLines");

	UDataTable* CreateEmptyOrderTable(const TCHAR* Dest = TableDest, const TCHAR* AssetName = TEXT("PotionShopOrders"))
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Dest, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*FileName, false, true, true);

		UPackage* Package = CreatePackage(Dest);
		Package->FullyLoad();
		UDataTable* Table = NewObject<UDataTable>(Package, FName(AssetName),
			RF_Public | RF_Standalone);
		Table->RowStruct = FSuperSLMPotionShopOrderRow::StaticStruct();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Table, *FileName, SaveArgs);
		FAssetRegistryModule::AssetCreated(Table);
		return Table;
	}

	void CleanupOrderTable(const TCHAR* Dest = TableDest)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Dest, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*FileName, false, true, true);
	}

	// True when no editor transaction is open. The write sink refuses while one is, so a cell that
	// leaves one open fails the next cell's writes; each cell checks at its start and its end.
	bool ExpectNoOpenTransaction(FAutomationTestBase* Test, const TCHAR* When)
	{
		const bool bOpen = GUndo != nullptr || (GEditor != nullptr && GEditor->IsTransactionActive());
		return Test->TestFalse(FString::Printf(TEXT("no editor transaction is open %s (open: '%s')"), When,
			(bOpen && GEditor != nullptr) ? *GEditor->GetTransactionName().ToString() : TEXT("")), bOpen);
	}

	// The example model: Qwen2.5-0.5B-Instruct with its tokenizer and the potion_shop_order and
	// prompt_result schemas compiled in. Found the way the main plugin's fixtures find it
	// (TryGetAExArtifactPath()): SUPERSLM_L2S1_AEX_PATH when it is set, otherwise
	// superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm under SUPERSLM_ARTIFACTS_DIR. This module
	// cannot include that fixture, so the lookup is written here. With neither variable set, the
	// path starts with "SUPERSLM_ARTIFACTS_DIR-is-unset/", where no file exists. A missing file fails
	// the cell by name, and no machine-specific path ships in the test.
	FString AExArtifactPath(FString& OutReason)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_L2S1_AEX_PATH"));
		const TCHAR* Relative = TEXT("superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm");
		const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("SUPERSLM_ARTIFACTS_DIR"));
		const FString Candidate = !Override.IsEmpty()
			? Override
			: (Root.IsEmpty() ? FString::Printf(TEXT("SUPERSLM_ARTIFACTS_DIR-is-unset/%s"), Relative) : FPaths::Combine(Root, Relative));
		if (IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
		OutReason = FString::Printf(TEXT("the example model is not at '%s' (SUPERSLM_L2S1_AEX_PATH: '%s'; SUPERSLM_ARTIFACTS_DIR: '%s')."),
			*Candidate, *Override, *Root);
		return FString();
	}

	// The write sink's row name for a prompt (SuperSLMToolset.cpp RowNameFor): the schema name, an
	// underscore, then the lower-case hex of the first 8 bytes of SHA-1 over the UTF-8 prompt.
	FName ExpectedRowName(const FString& SchemaName, const FString& Prompt)
	{
		const FTCHARToUTF8 Utf8(*Prompt);
		uint8 Hash[20];
		FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Hash);
		return FName(*FString::Printf(TEXT("%s_%s"), *SchemaName, *BytesToHex(Hash, 8).ToLower()));
	}

	// A row's content as one comparable string.
	FString RowContent(const FSuperSLMPotionShopOrderRow& Row)
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *Row.Intent, *Row.Item, *Row.Quantity, Row.bPolite ? TEXT("true") : TEXT("false"));
	}

	// Every row's name and content, sorted: two snapshots are equal only if the table is.
	TArray<FString> TableSnapshot(const UDataTable& Table)
	{
		TArray<FString> Out;
		for (const TPair<FName, uint8*>& Pair : Table.GetRowMap())
		{
			Out.Add(Pair.Key.ToString() + TEXT("=") + RowContent(*reinterpret_cast<const FSuperSLMPotionShopOrderRow*>(Pair.Value)));
		}
		Out.Sort();
		return Out;
	}

	// The potion_shop_order schema's declared sets. A written row outside them is a
	// write the sink should have rejected.
	bool IsSchemaValidRow(const FSuperSLMPotionShopOrderRow& Row)
	{
		static const TArray<FString> Intents = { TEXT("buy"), TEXT("sell"), TEXT("ask_price"), TEXT("haggle"), TEXT("leave") };
		static const TArray<FString> Items = { TEXT("health_potion"), TEXT("mana_potion"), TEXT("rope"), TEXT("torch"), TEXT("lantern"), TEXT("none") };
		static const TArray<FString> Quantities = { TEXT("one"), TEXT("two"), TEXT("three"), TEXT("several"), TEXT("none") };
		return Intents.Contains(Row.Intent) && Items.Contains(Row.Item) && Quantities.Contains(Row.Quantity);
	}

	// A checkpoint directory the pre-check can read, written at test time under the automation
	// transient directory: Qwen2.5-0.5B-Instruct's config.json shape and the two tokenizer files
	// the pre-check requires (their content is not read). ModelType selects the architecture.
	FString WritePreCheckFixture(const TCHAR* Name, const TCHAR* ModelType)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("SuperSLMMCPPreCheck") / Name);
		const FString Config = FString::Printf(TEXT("{\"model_type\": \"%s\", \"hidden_size\": 896, \"num_hidden_layers\": 24, ")
			TEXT("\"num_attention_heads\": 14, \"num_key_value_heads\": 2, \"intermediate_size\": 4864, ")
			TEXT("\"vocab_size\": 151936, \"max_position_embeddings\": 32768}"), ModelType);
		const bool bWritten = FFileHelper::SaveStringToFile(Config, *(Dir / TEXT("config.json")))
			&& FFileHelper::SaveStringToFile(TEXT("{}"), *(Dir / TEXT("tokenizer.json")))
			&& FFileHelper::SaveStringToFile(TEXT("{}"), *(Dir / TEXT("tokenizer_config.json")));
		return bWritten ? Dir : FString();
	}

	// 20 distinct potion-shop prompts -- varied enough that
	// a write sink silently dropping or aliasing rows shows up as a row-count/content mismatch
	// rather than 20 copies of one trivially-repeatable answer.
	TArray<FString> TwentyPrompts()
	{
		// L2-S3 box session, mechanical repair (2026-09-25): UE 5.8's FString::Printf takes only a
		// literal format string, so the four templates are literals in a switch. The prompts are
		// byte-identical to the former table's.
		static const TCHAR* Items[] = {
			TEXT("a health potion"), TEXT("a mana potion"), TEXT("rope"), TEXT("a torch"), TEXT("a lantern"),
		};
		TArray<FString> Prompts;
		Prompts.Reserve(20);
		for (int32 I = 0; I < 20; ++I)
		{
			const TCHAR* Item = Items[I % 5];
			switch (I % 4)
			{
			case 0:  Prompts.Add(FString::Printf(TEXT("A customer asks to buy %s %s."), Item, TEXT("two"))); break;
			case 1:  Prompts.Add(FString::Printf(TEXT("Someone politely asks the price of %s."), Item)); break;
			case 2:  Prompts.Add(FString::Printf(TEXT("A haggler tries to talk down the price of %s."), Item)); break;
			default: Prompts.Add(FString::Printf(TEXT("A customer wants to sell %s %s back."), Item, TEXT("two"))); break;
			}
		}
		return Prompts;
	}
}

namespace
{
	// Deadlines. A 20-prompt batch is about 20 x 48 tokens at CPU rates (~35 s), plus
	// a first-call import and Configure. The read tier does no decode.
	constexpr double BatchTimeoutSeconds = 600.0;
	constexpr double ReadTierTimeoutSeconds = 120.0;

	// One awaited tool call. Value and Error are meaningful only when bCompleted.
	struct FAwaitedTool
	{
		TStrongObjectPtr<UToolCallAsyncResultString> Pending;
		double DeadlineSeconds = 0.0;
		bool bCompleted = false;
		FString Value;
		FString Error;
	};

	// Queues two latent commands. The first calls Start() and holds the result it returns. The
	// second waits, one frame at a time, for completion, then copies Value and Error into Out and
	// sets bCompleted. A null result, or a result not complete by the deadline, fails the test by
	// name and leaves bCompleted false. Whether an Error is a failure is the caller's to say
	// (ExpectToolSuccess / ExpectToolError).
	void EnqueueToolAwait(FAutomationTestBase* Test, const FString& Label,
		TFunction<UToolCallAsyncResultString*()> Start, TSharedRef<FAwaitedTool> Out, double TimeoutSeconds)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Label, Start, Out, TimeoutSeconds]()
		{
			UToolCallAsyncResultString* Result = Start();
			if (Result == nullptr)
			{
				Test->AddError(FString::Printf(TEXT("%s: the tool returned no result object"), *Label));
				return true;
			}
			Out->Pending.Reset(Result);
			Out->DeadlineSeconds = FPlatformTime::Seconds() + TimeoutSeconds;
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Label, Out, TimeoutSeconds]()
		{
			if (!Out->Pending.IsValid())
			{
				return true; // the start step already failed by name
			}
			if (Out->Pending->bIsComplete)
			{
				Out->Value = Out->Pending->Value;
				Out->Error = Out->Pending->Error;
				Out->bCompleted = true;
				Out->Pending.Reset();
				return true;
			}
			if (FPlatformTime::Seconds() > Out->DeadlineSeconds)
			{
				Test->AddError(FString::Printf(TEXT("%s: not complete after %.0f s"), *Label, TimeoutSeconds));
				Out->Pending.Reset();
				return true;
			}
			return false;
		}));
	}

	void EnqueueStep(TFunction<void()> Step)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() { Step(); return true; }));
	}

	// Asserts the awaited call completed on the Value channel with a JSON object and an empty Error,
	// and returns the object (null otherwise).
	TSharedPtr<FJsonObject> ExpectToolSuccess(FAutomationTestBase* Test, const FString& Label, const FAwaitedTool& Tool)
	{
		if (!Tool.bCompleted)
		{
			Test->AddError(FString::Printf(TEXT("%s: no completed result to check"), *Label));
			return nullptr;
		}
		if (!Test->TestTrue(FString::Printf(TEXT("%s succeeds (Error: '%s')"), *Label, *Tool.Error), Tool.Error.IsEmpty()))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Object = ParseTool(Tool.Value);
		if (!Test->TestTrue(FString::Printf(TEXT("%s's Value parses as a JSON object (%s)"), *Label, *Tool.Value), Object.IsValid()))
		{
			return nullptr;
		}
		return Object;
	}

	// Asserts the awaited call completed on the Error channel with a non-empty message.
	bool ExpectToolError(FAutomationTestBase* Test, const FString& Label, const FAwaitedTool& Tool)
	{
		if (!Tool.bCompleted)
		{
			Test->AddError(FString::Printf(TEXT("%s: no completed result to check"), *Label));
			return false;
		}
		return Test->TestFalse(FString::Printf(TEXT("%s is refused on the Error channel (Value: '%s')"), *Label, *Tool.Value),
			Tool.Error.IsEmpty());
	}

	// The model inspector's own readout of the example model, as the editor's inspection panel
	// takes it: the model imported, its shape read, and the declared shape the panel passes
	// (MakeCpuConfig with the host's default settings). The oracle DescribeModel is compared with.
	struct FInspectorOracle
	{
		TStrongObjectPtr<USuperSLMModel> Model;
		bool bDone = false;
		FSuperSLMModelInspection Inspection;
		double DeadlineSeconds = 0.0;
	};

	void EnqueueInspectorOracle(FAutomationTestBase* Test, const FString& AExPath, TSharedRef<FInspectorOracle> Oracle)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, AExPath, Oracle]()
		{
			FSuperSLMImportDiagnostic Diag;
			USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AExPath, Diag);
			FSuperSLMModelShapeFacts Shape;
			if (Model == nullptr || !SuperSLMModelInspector::ReadShape(*Model, Shape))
			{
				Test->AddError(FString::Printf(TEXT("inspector oracle: the example model did not import or has no shape (%s)"), *Diag.Message));
				return true;
			}
			Oracle->Model.Reset(Model);
			Oracle->DeadlineSeconds = FPlatformTime::Seconds() + ReadTierTimeoutSeconds;
			const FSuperSLMRuntimeConfig Declared = FSuperSLMEditorRuntimeHost::MakeCpuConfig(Shape, FSuperSLMEditorHostSettings());
			SuperSLMModelInspector::InspectModelAsync(*Model, Declared, [Oracle](const FSuperSLMModelInspection& Result)
			{
				Oracle->Inspection = Result;
				Oracle->bDone = true;
			});
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Oracle]()
		{
			if (!Oracle->Model.IsValid() || Oracle->bDone)
			{
				return true;
			}
			if (FPlatformTime::Seconds() > Oracle->DeadlineSeconds)
			{
				Test->AddError(TEXT("inspector oracle: the inspection did not complete in time"));
				return true;
			}
			return false;
		}));
	}

	// DescribeModel's Value against the inspector, field by field. Timing-derived figures (the
	// measured bandwidth and the reset/adopt ms predicted from it) are re-measured per call and are
	// not compared; everything read from the artifact or computed from its shape is.
	void ExpectDescribeMatchesInspector(FAutomationTestBase* Test, const FJsonObject& D, const FSuperSLMModelInspection& I)
	{
		auto Num = [Test](const FJsonObject& O, const TCHAR* Field, int64 Expected)
		{
			double Got = -1.0;
			const bool bHas = O.TryGetNumberField(Field, Got);
			Test->TestTrue(FString::Printf(TEXT("DescribeModel.%s == inspector (%lld; got %s)"), Field, Expected,
				bHas ? *FString::Printf(TEXT("%.0f"), Got) : TEXT("absent")), bHas && static_cast<int64>(Got) == Expected);
		};
		Test->TestEqual(TEXT("DescribeModel.artifactHash == inspector"), D.GetStringField(TEXT("artifactHash")), I.ArtifactHashHex);
		Num(D, TEXT("fileBytes"), I.FileBytes);
		Num(D, TEXT("formatVersion"), I.FormatVersion);
		Num(D, TEXT("headerFlags"), I.Flags);

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (Test->TestTrue(TEXT("DescribeModel.sections present"), D.TryGetArrayField(TEXT("sections"), Sections)) &&
			Test->TestEqual(TEXT("DescribeModel.sections count == inspector"), Sections->Num(), I.Sections.Num()))
		{
			for (int32 i = 0; i < I.Sections.Num(); ++i)
			{
				const TSharedPtr<FJsonObject> S = (*Sections)[i]->AsObject();
				if (!Test->TestTrue(FString::Printf(TEXT("DescribeModel.sections[%d] is an object"), i), S.IsValid()))
				{
					continue;
				}
				Num(*S, TEXT("type"), I.Sections[i].Type);
				Test->TestEqual(FString::Printf(TEXT("DescribeModel.sections[%d].name == inspector"), i), S->GetStringField(TEXT("name")), I.Sections[i].TypeName);
				Num(*S, TEXT("dtype"), I.Sections[i].Dtype);
				Num(*S, TEXT("offset"), I.Sections[i].Offset);
				Num(*S, TEXT("byteSize"), I.Sections[i].ByteSize);
			}
		}

		const TSharedPtr<FJsonObject>* Config = nullptr;
		if (Test->TestTrue(TEXT("DescribeModel.config present"), D.TryGetObjectField(TEXT("config"), Config)))
		{
			Num(**Config, TEXT("hiddenSize"), I.HiddenSize);
			Num(**Config, TEXT("numHiddenLayers"), I.NumHiddenLayers);
			Num(**Config, TEXT("numAttentionHeads"), I.NumAttentionHeads);
			Num(**Config, TEXT("numKeyValueHeads"), I.NumKeyValueHeads);
			Num(**Config, TEXT("headDim"), I.HeadDim);
			Num(**Config, TEXT("vocabSize"), I.VocabSize);
			Num(**Config, TEXT("contextCap"), I.ContextCap);
			Num(**Config, TEXT("kvPrecisionBytes"), I.KvPrecisionBytes);
		}

		TArray<FString> Schemas;
		D.TryGetStringArrayField(TEXT("schemas"), Schemas);
		Test->TestEqual(TEXT("DescribeModel.schemas == inspector, in index order"), Schemas, I.SchemaNames);

		// The footprint and the GPU head: the figures the earlier describe got wrong (reported as not
		// exported, and the head as V x H without the inspector's padding and rows).
		const TSharedPtr<FJsonObject>* Footprint = nullptr;
		if (Test->TestTrue(TEXT("DescribeModel.footprint present"), D.TryGetObjectField(TEXT("footprint"), Footprint)))
		{
			Num(**Footprint, TEXT("workspaceBytes"), I.WorkspaceBytes);
			Num(**Footprint, TEXT("kvBlockBytes"), I.KvBlockBytes);
			Num(**Footprint, TEXT("kvPoolOverheadBytes"), I.KvPoolOverheadBytes);
			Num(**Footprint, TEXT("seqStateBytesUpperBound"), I.SeqStateBytesUpperBound);
		}
		const TSharedPtr<FJsonObject>* GpuHead = nullptr;
		if (Test->TestTrue(TEXT("DescribeModel.gpuHead present"), D.TryGetObjectField(TEXT("gpuHead"), GpuHead)))
		{
			Test->TestEqual(TEXT("DescribeModel.gpuHead.deviceResidentHead == inspector"),
				(*GpuHead)->GetBoolField(TEXT("deviceResidentHead")), I.bGpuDeviceResidentHead);
			Num(**GpuHead, TEXT("declaredBytesLowerBound"), I.GpuDeviceHeadDeclaredBytes);
		}
		const TSharedPtr<FJsonObject>* Lifecycle = nullptr;
		if (Test->TestTrue(TEXT("DescribeModel.sequenceLifecycle present"), D.TryGetObjectField(TEXT("sequenceLifecycle"), Lifecycle)))
		{
			double Budget = -1.0;
			(*Lifecycle)->TryGetNumberField(TEXT("sequenceLifecycleBudgetMs"), Budget);
			Test->TestEqual(TEXT("DescribeModel.sequenceLifecycle.sequenceLifecycleBudgetMs == inspector"), Budget, I.SequenceLifecycleBudgetMs);
		}
		Test->TestEqual(TEXT("DescribeModel.dampedGreedyConstants == inspector"), D.GetStringField(TEXT("dampedGreedyConstants")),
			FString(I.bHasDampedGreedyConstants ? TEXT("present, unused at 1.0") : TEXT("absent")));
	}

	TArray<FString> SortedRowNames(const UDataTable& Table)
	{
		TArray<FName> Names;
		Table.GetRowMap().GetKeys(Names);
		TArray<FString> Out;
		for (const FName& Name : Names)
		{
			Out.Add(Name.ToString());
		}
		Out.Sort();
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3MCPActionTierTest,
	"SuperSLM.L2S3.MCP.ActionTierBatchWritesUndoesAndReproduces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3MCPActionTierTest::RunTest(const FString& Parameters)
{
	FString AExReason;
	const FString AExPath = AExArtifactPath(AExReason);
	if (!TestFalse(TEXT("A-EX present"), AExPath.IsEmpty()))
	{
		AddError(AExReason); // an absent artifact fails the cell, never a warning and a pass
		return false;
	}
	if (!ExpectNoOpenTransaction(this, TEXT("when the batch cell starts")))
	{
		return false;
	}

	const FString PreCheckDir = WritePreCheckFixture(TEXT("qwen2"), TEXT("qwen2"));
	const FString PreCheckRejectDir = WritePreCheckFixture(TEXT("llama"), TEXT("llama"));
	if (!TestFalse(TEXT("pre-check fixtures written"), PreCheckDir.IsEmpty() || PreCheckRejectDir.IsEmpty()))
	{
		return false;
	}

	TStrongObjectPtr<UDataTable> Table(CreateEmptyOrderTable());
	if (!TestNotNull(TEXT("order table created"), Table.Get()) ||
		!TestEqual(TEXT("table starts empty"), Table->GetRowMap().Num(), 0))
	{
		CleanupOrderTable();
		return false;
	}

	FAutomationTestBase* Test = this;
	const TArray<FString> Prompts = TwentyPrompts();

	// Seed two rows outside any transaction, so batch #1 exercises the replace path and undo has
	// something other than an empty table to restore: one row under the name batch #1 writes for
	// its first prompt (to be replaced), one unrelated row (to be left alone). Their sentinel
	// content is outside the schema's sets, so a replaced row is told apart from a kept one.
	const FName ReplacedRow = ExpectedRowName(TEXT("potion_shop_order"), Prompts[0]);
	{
		FSuperSLMPotionShopOrderRow Seed;
		Seed.Intent = TEXT("SEED");
		Seed.Item = TEXT("SEED");
		Seed.Quantity = TEXT("SEED");
		Table->AddRow(ReplacedRow, Seed);
		FSuperSLMPotionShopOrderRow Unrelated;
		Unrelated.Intent = TEXT("UNRELATED");
		Unrelated.Item = TEXT("UNRELATED");
		Unrelated.Quantity = TEXT("UNRELATED");
		Unrelated.bPolite = true;
		Table->AddRow(FName(TEXT("unrelated_row")), Unrelated);
	}
	const TArray<FString> SeededSnapshot = TableSnapshot(*Table);
	if (!TestEqual(TEXT("table holds the two seeded rows"), SeededSnapshot.Num(), 2))
	{
		CleanupOrderTable();
		return false;
	}
	const TSharedRef<TArray<FString>> Batch1Snapshot = MakeShared<TArray<FString>>();
	const TSharedRef<FAwaitedTool> Batch1 = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> Batch2 = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> Described = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> PreChecked = MakeShared<FAwaitedTool>();
	const TSharedRef<TArray<FString>> Batch1RowNames = MakeShared<TArray<FString>>();
	struct FReadSnapshot
	{
		TArray<FString> Rows;
		bool bPackageDirty = false;
		int32 UndoQueueLength = -1;
	};
	const TSharedRef<FReadSnapshot> ReadSnapshot = MakeShared<FReadSnapshot>();
	const TSharedRef<FAwaitedTool> PreCheckRejected = MakeShared<FAwaitedTool>();
	const TSharedRef<FInspectorOracle> Oracle = MakeShared<FInspectorOracle>();

	// --- Batch run #1: 20 schema-valid rows written under one transaction ---
	EnqueueToolAwait(Test, TEXT("batch #1"), [AExPath, Prompts]()
	{
		return USuperSLMToolset::RunConstrainedInferenceBatch(AExPath, TEXT("potion_shop_order"), Prompts, TablePath);
	}, Batch1, BatchTimeoutSeconds);
	EnqueueStep([Test, Table, Batch1, Batch1RowNames, Batch1Snapshot, Prompts, ReplacedRow, SeededSnapshot]()
	{
		if (ExpectToolSuccess(Test, TEXT("batch #1"), *Batch1).IsValid())
		{
			// 20 written rows, one of them over the seeded row, plus the unrelated row.
			Test->TestEqual(TEXT("table has 21 rows after batch #1 (20 written, one replacing a seeded row, plus the unrelated row)"),
				Table->GetRowMap().Num(), 21);
			for (const FString& Prompt : Prompts)
			{
				const FName Name = ExpectedRowName(TEXT("potion_shop_order"), Prompt);
				const FSuperSLMPotionShopOrderRow* Row = Table->FindRow<FSuperSLMPotionShopOrderRow>(Name, TEXT("order table"), false);
				if (Test->TestNotNull(FString::Printf(TEXT("row %s written for \"%s\""), *Name.ToString(), *Prompt), Row))
				{
					Test->TestTrue(FString::Printf(TEXT("row %s is inside the schema's sets (%s)"), *Name.ToString(), *RowContent(*Row)),
						IsSchemaValidRow(*Row));
				}
			}
			const FSuperSLMPotionShopOrderRow* Unrelated = Table->FindRow<FSuperSLMPotionShopOrderRow>(FName(TEXT("unrelated_row")), TEXT("order table"), false);
			Test->TestTrue(TEXT("the unrelated seeded row is untouched"), Unrelated != nullptr && Unrelated->Intent == TEXT("UNRELATED"));
			*Batch1RowNames = SortedRowNames(*Table);
			*Batch1Snapshot = TableSnapshot(*Table);
		}

		// --- Undo restores the table EXACTLY (the write is one undoable transaction), the
		// replaced row's seeded content included ---
		if (Test->TestNotNull(TEXT("GEditor available"), GEditor))
		{
			GEditor->UndoTransaction();
			Test->TestEqual(TEXT("undo restores the seeded table exactly, row names and content"), TableSnapshot(*Table), SeededSnapshot);
			const FSuperSLMPotionShopOrderRow* Restored = Table->FindRow<FSuperSLMPotionShopOrderRow>(ReplacedRow, TEXT("order table"), false);
			Test->TestTrue(TEXT("undo restores the replaced row's seeded content"), Restored != nullptr && Restored->Intent == TEXT("SEED"));
		}
	});

	// --- Re-run: identical rows (reproducibility, greedy decode, no sampling) ---
	EnqueueToolAwait(Test, TEXT("batch #2"), [AExPath, Prompts]()
	{
		return USuperSLMToolset::RunConstrainedInferenceBatch(AExPath, TEXT("potion_shop_order"), Prompts, TablePath);
	}, Batch2, BatchTimeoutSeconds);
	EnqueueStep([Test, Table, Batch1, Batch2, Batch1RowNames, Batch1Snapshot, ReadSnapshot]()
	{
		if (ExpectToolSuccess(Test, TEXT("batch #2"), *Batch2).IsValid())
		{
			Test->TestEqual(TEXT("table has 21 rows after batch #2"), Table->GetRowMap().Num(), 21);
			Test->TestEqual(TEXT("batch #2's row set matches batch #1's row set"), SortedRowNames(*Table), *Batch1RowNames);
			Test->TestEqual(TEXT("batch #2's rows are identical to batch #1's, name and content"), TableSnapshot(*Table), *Batch1Snapshot);
			// The tool's JSON result is the reproducibility oracle: greedy decoding, so an identical
			// batch against an identical model and schema gives the identical string.
			Test->TestTrue(TEXT("batch #1 and batch #2 both completed"), Batch1->bCompleted && Batch2->bCompleted);
			Test->TestEqual(TEXT("batch #1 and batch #2 results are identical"), Batch2->Value, Batch1->Value);
		}
		// What a read-tier call must not change: the table's rows and content, its package's
		// dirty flag, and the editor's undo queue.
		ReadSnapshot->Rows = TableSnapshot(*Table);
		ReadSnapshot->bPackageDirty = Table->GetOutermost()->IsDirty();
		ReadSnapshot->UndoQueueLength = (GEditor != nullptr && GEditor->Trans != nullptr) ? GEditor->Trans->GetQueueLength() : -1;
	});

	// --- Read tier: DescribeModel reports figures; no read-tier call mutates the table ---
	EnqueueToolAwait(Test, TEXT("DescribeModel"), [AExPath]()
	{
		return USuperSLMToolset::DescribeModel(AExPath);
	}, Described, ReadTierTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("RunPreCheck"), [PreCheckDir]()
	{
		return USuperSLMToolset::RunPreCheck(PreCheckDir, 4096, 100000.0);
	}, PreChecked, ReadTierTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("RunPreCheck (llama)"), [PreCheckRejectDir]()
	{
		return USuperSLMToolset::RunPreCheck(PreCheckRejectDir, 4096, 100000.0);
	}, PreCheckRejected, ReadTierTimeoutSeconds);
	EnqueueInspectorOracle(Test, AExPath, Oracle);
	EnqueueStep([Test, Table, Described, PreChecked, PreCheckRejected, ReadSnapshot, Oracle]()
	{
		// DescribeModel reports the inspector's figures, field by field, not merely "not an error".
		if (const TSharedPtr<FJsonObject> Describe = ExpectToolSuccess(Test, TEXT("DescribeModel"), *Described))
		{
			if (Test->TestTrue(FString::Printf(TEXT("the inspector oracle produced a valid inspection (%s)"), *Oracle->Inspection.Error),
					Oracle->bDone && Oracle->Inspection.bValid))
			{
				ExpectDescribeMatchesInspector(Test, *Describe, Oracle->Inspection);
			}
		}

		// The pre-check's figures for the fixture's shape: head_dim = 896 / 14 = 64; context_cap
		// as requested (4096 <= max_position_embeddings); KV block = 24 layers x 4096 x 2 KV
		// heads x 64 x 2 (K and V) x 1 byte = 25,165,824 bytes.
		if (const TSharedPtr<FJsonObject> Report = ExpectToolSuccess(Test, TEXT("RunPreCheck"), *PreChecked))
		{
			Test->TestTrue(FString::Printf(TEXT("RunPreCheck passes the qwen2 fixture (%s)"), *PreChecked->Value), Report->GetBoolField(TEXT("pass")));
			Test->TestEqual(TEXT("RunPreCheck modelType"), Report->GetStringField(TEXT("modelType")), FString(TEXT("qwen2")));
			Test->TestEqual(TEXT("RunPreCheck derivedHeadDim"), static_cast<int64>(Report->GetNumberField(TEXT("derivedHeadDim"))), static_cast<int64>(64));
			Test->TestEqual(TEXT("RunPreCheck derivedContextCap"), static_cast<int64>(Report->GetNumberField(TEXT("derivedContextCap"))), static_cast<int64>(4096));
			Test->TestEqual(TEXT("RunPreCheck predictedKvBlockSizeBytes"), static_cast<int64>(Report->GetNumberField(TEXT("predictedKvBlockSizeBytes"))), static_cast<int64>(25165824));
		}
		if (const TSharedPtr<FJsonObject> Report = ExpectToolSuccess(Test, TEXT("RunPreCheck (llama)"), *PreCheckRejected))
		{
			Test->TestFalse(TEXT("RunPreCheck refuses an unsupported architecture"), Report->GetBoolField(TEXT("pass")));
			FString Blocker;
			Report->TryGetStringField(TEXT("blockerReason"), Blocker);
			Test->TestTrue(FString::Printf(TEXT("the blocker names model_type (%s)"), *Blocker), Blocker.Contains(TEXT("model_type")));
		}

		Test->TestEqual(TEXT("read-tier calls do not change the order table's rows or content"), TableSnapshot(*Table), ReadSnapshot->Rows);
		Test->TestTrue(TEXT("read-tier calls do not change the table package's dirty flag"), Table->GetOutermost()->IsDirty() == ReadSnapshot->bPackageDirty);
		const int32 UndoQueueLength = (GEditor != nullptr && GEditor->Trans != nullptr) ? GEditor->Trans->GetQueueLength() : -1;
		Test->TestEqual(TEXT("read-tier calls record no undo transaction"), UndoQueueLength, ReadSnapshot->UndoQueueLength);
		ExpectNoOpenTransaction(Test, TEXT("when the batch cell ends"));
		Oracle->Model.Reset();
		CleanupOrderTable();
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3MCPActionTierValidationTest,
	"SuperSLM.L2S3.MCP.ActionTierValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3MCPActionTierValidationTest::RunTest(const FString& Parameters)
{
	// Every bad input is a tool-level error, never a crash (the precedent is a sibling plugin's
	// MCP validation test, in the project's private history).
	//
	// The unknown-schema and empty-batch cases run against the REAL A-EX, so the model loads and
	// the call reaches the check each case names. With a missing model they would error at load,
	// and a tool that never validates schemas or batch sizes would still pass. The unknown-schema
	// error must also name the schema: an error that is present but about something else is not
	// that check firing. The empty-batch error has no fixed text to check.
	FString AExReason;
	const FString AExPath = AExArtifactPath(AExReason);
	if (!TestFalse(TEXT("A-EX present"), AExPath.IsEmpty()))
	{
		AddError(AExReason);
		return false;
	}
	if (!ExpectNoOpenTransaction(this, TEXT("when the validation cell starts")))
	{
		return false;
	}
	TStrongObjectPtr<UDataTable> Table(CreateEmptyOrderTable());
	if (!TestNotNull(TEXT("order table created"), Table.Get()))
	{
		return false;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FAwaitedTool> UnknownModel = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> UnknownDir = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> UnknownSchema = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> EmptyBatch = MakeShared<FAwaitedTool>();

	EnqueueToolAwait(Test, TEXT("unknown model"), []()
	{
		return USuperSLMToolset::DescribeModel(TEXT("/Game/DoesNotExist.DoesNotExist"));
	}, UnknownModel, ReadTierTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("unknown checkpoint dir"), []()
	{
		return USuperSLMToolset::RunPreCheck(FPaths::ProjectIntermediateDir() / TEXT("SuperSLMTests/definitely_missing_checkpoint_dir"), 4096, 100000.0);
	}, UnknownDir, ReadTierTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("unknown schema"), [AExPath]()
	{
		return USuperSLMToolset::RunConstrainedInference(AExPath, TEXT("not_a_real_schema"), TEXT("hello"), TablePath);
	}, UnknownSchema, BatchTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("empty batch"), [AExPath]()
	{
		return USuperSLMToolset::RunConstrainedInferenceBatch(AExPath, TEXT("potion_shop_order"), {}, TablePath);
	}, EmptyBatch, BatchTimeoutSeconds);

	EnqueueStep([Test, Table, UnknownModel, UnknownDir, UnknownSchema, EmptyBatch]()
	{
		ExpectToolError(Test, TEXT("unknown model"), *UnknownModel);
		ExpectToolError(Test, TEXT("unknown checkpoint dir"), *UnknownDir);
		if (ExpectToolError(Test, TEXT("unknown schema"), *UnknownSchema))
		{
			Test->TestTrue(FString::Printf(TEXT("unknown schema's error names the schema (%s)"), *UnknownSchema->Error),
				UnknownSchema->Error.Contains(TEXT("not_a_real_schema")));
		}
		ExpectToolError(Test, TEXT("empty batch"), *EmptyBatch);
		Test->TestEqual(TEXT("rejected calls wrote no rows"), Table->GetRowMap().Num(), 0);
		ExpectNoOpenTransaction(Test, TEXT("when the validation cell ends"));
		CleanupOrderTable();
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3MCPOverDataTableTest,
	"SuperSLM.L2S3.MCP.OverDataTableWritesUndoesAndReproduces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3MCPOverDataTableTest::RunTest(const FString& Parameters)
{
	// RunConstrainedInferenceOverDataTable, the read-transform-write loop: one column of a source
	// table in, one schema-valid row per source row out, named "<schema>_<source row>", in one undo
	// step; the source is only read; a re-run gives identical rows; a source that is also the
	// target is refused and writes nothing.
	FString AExReason;
	const FString AExPath = AExArtifactPath(AExReason);
	if (!TestFalse(TEXT("A-EX present"), AExPath.IsEmpty()))
	{
		AddError(AExReason);
		return false;
	}
	if (!ExpectNoOpenTransaction(this, TEXT("when the over-data-table cell starts")))
	{
		return false;
	}
	TStrongObjectPtr<UDataTable> Target(CreateEmptyOrderTable());
	TStrongObjectPtr<UDataTable> Source(CreateEmptyOrderTable(SourceTableDest, TEXT("CustomerLines")));
	if (!TestNotNull(TEXT("target table created"), Target.Get()) || !TestNotNull(TEXT("source table created"), Source.Get()))
	{
		CleanupOrderTable();
		CleanupOrderTable(SourceTableDest);
		return false;
	}

	// The source column is Item (an FString property); the other fields are left empty.
	static const TCHAR* Lines[][2] = {
		{ TEXT("line_a"), TEXT("I would like two health potions, please.") },
		{ TEXT("line_b"), TEXT("How much is a lantern?") },
		{ TEXT("line_c"), TEXT("Give me that rope for less, come on!") },
		{ TEXT("line_d"), TEXT("I want to sell you three torches.") },
	};
	for (const auto& Line : Lines)
	{
		FSuperSLMPotionShopOrderRow Row;
		Row.Item = Line[1];
		Source->AddRow(FName(Line[0]), Row);
	}
	const TArray<FString> SourceSnapshot = TableSnapshot(*Source);

	FAutomationTestBase* Test = this;
	const FString Prompt = TEXT("A customer at the potion shop says:");
	const TSharedRef<FAwaitedTool> Run1 = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> Run2 = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> SameTable = MakeShared<FAwaitedTool>();
	const TSharedRef<TArray<FString>> Run1Snapshot = MakeShared<TArray<FString>>();

	auto StartRun = [AExPath, Prompt]()
	{
		return USuperSLMToolset::RunConstrainedInferenceOverDataTable(AExPath, TEXT("potion_shop_order"), Prompt,
			SourceTablePath, TEXT("Item"), TablePath);
	};
	EnqueueToolAwait(Test, TEXT("over data table #1"), StartRun, Run1, BatchTimeoutSeconds);
	EnqueueStep([Test, Target, Source, Run1, Run1Snapshot, SourceSnapshot]()
	{
		if (const TSharedPtr<FJsonObject> Result = ExpectToolSuccess(Test, TEXT("over data table #1"), *Run1))
		{
			double Count = -1.0;
			Result->TryGetNumberField(TEXT("count"), Count);
			Test->TestEqual(TEXT("over data table #1 reports one record per source row"), static_cast<int32>(Count), static_cast<int32>(UE_ARRAY_COUNT(Lines)));
			Test->TestEqual(TEXT("target has one row per source row"), Target->GetRowMap().Num(), static_cast<int32>(UE_ARRAY_COUNT(Lines)));
			for (const auto& Line : Lines)
			{
				const FName Name(*FString::Printf(TEXT("potion_shop_order_%s"), Line[0]));
				const FSuperSLMPotionShopOrderRow* Row = Target->FindRow<FSuperSLMPotionShopOrderRow>(Name, TEXT("target table"), false);
				if (Test->TestNotNull(FString::Printf(TEXT("row %s written for source row %s"), *Name.ToString(), Line[0]), Row))
				{
					Test->TestTrue(FString::Printf(TEXT("row %s is inside the schema's sets (%s)"), *Name.ToString(), *RowContent(*Row)),
						IsSchemaValidRow(*Row));
				}
			}
			*Run1Snapshot = TableSnapshot(*Target);
		}
		Test->TestEqual(TEXT("the source table is only read"), TableSnapshot(*Source), SourceSnapshot);

		if (Test->TestNotNull(TEXT("GEditor available"), GEditor))
		{
			GEditor->UndoTransaction();
			Test->TestEqual(TEXT("one undo empties the target"), Target->GetRowMap().Num(), 0);
			Test->TestEqual(TEXT("undo leaves the source as it was"), TableSnapshot(*Source), SourceSnapshot);
		}
	});

	EnqueueToolAwait(Test, TEXT("over data table #2"), StartRun, Run2, BatchTimeoutSeconds);
	EnqueueStep([Test, Target, Run1, Run2, Run1Snapshot]()
	{
		if (ExpectToolSuccess(Test, TEXT("over data table #2"), *Run2).IsValid())
		{
			Test->TestEqual(TEXT("the re-run's rows are identical to the first run's, name and content"), TableSnapshot(*Target), *Run1Snapshot);
			Test->TestEqual(TEXT("the re-run's result is identical to the first run's"), Run2->Value, Run1->Value);
		}
	});

	// A source that is also the target: refused by name, and nothing is written.
	const TSharedRef<TArray<FString>> BeforeSameTable = MakeShared<TArray<FString>>();
	EnqueueStep([Target, BeforeSameTable]() { *BeforeSameTable = TableSnapshot(*Target); });
	EnqueueToolAwait(Test, TEXT("source is the target"), [AExPath, Prompt]()
	{
		return USuperSLMToolset::RunConstrainedInferenceOverDataTable(AExPath, TEXT("potion_shop_order"), Prompt,
			TablePath, TEXT("Item"), TablePath);
	}, SameTable, BatchTimeoutSeconds);
	EnqueueStep([Test, Target, SameTable, BeforeSameTable]()
	{
		ExpectToolError(Test, TEXT("source is the target"), *SameTable);
		Test->TestEqual(TEXT("the refused call wrote nothing"), TableSnapshot(*Target), *BeforeSameTable);
		ExpectNoOpenTransaction(Test, TEXT("when the over-data-table cell ends"));
		CleanupOrderTable();
		CleanupOrderTable(SourceTableDest);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLML2S3MCPOpenTransactionRefusedTest,
	"SuperSLM.L2S3.MCP.WriteRefusedWhileTransactionOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLML2S3MCPOpenTransactionRefusedTest::RunTest(const FString& Parameters)
{
	// The write is its own undo step: while another editor transaction is open, the sink waits for
	// it to close (up to 120 s), then refuses by that transaction's name and writes nothing, so one
	// undo never reverts both. The transaction is held open across the call's frames, past that
	// wait, and cancelled after, on every path; the cell therefore takes a little over 120 s.
	FString AExReason;
	const FString AExPath = AExArtifactPath(AExReason);
	if (!TestFalse(TEXT("A-EX present"), AExPath.IsEmpty()))
	{
		AddError(AExReason);
		return false;
	}
	if (!ExpectNoOpenTransaction(this, TEXT("when the open-transaction cell starts")))
	{
		return false;
	}
	TStrongObjectPtr<UDataTable> Table(CreateEmptyOrderTable());
	if (!TestNotNull(TEXT("order table created"), Table.Get()) || !TestNotNull(TEXT("GEditor available"), GEditor))
	{
		CleanupOrderTable();
		return false;
	}

	FAutomationTestBase* Test = this;
	static const TCHAR* HeldName = TEXT("SuperSLM test: transaction held open");
	const TSharedRef<int32> HeldIndex = MakeShared<int32>(INDEX_NONE);
	const TSharedRef<FAwaitedTool> Refused = MakeShared<FAwaitedTool>();

	EnqueueStep([Test, HeldIndex]()
	{
		*HeldIndex = GEditor->BeginTransaction(FText::FromString(HeldName));
		Test->TestTrue(TEXT("the held transaction is open"), GEditor->IsTransactionActive());
	});
	EnqueueToolAwait(Test, TEXT("write under an open transaction"), [AExPath]()
	{
		return USuperSLMToolset::RunConstrainedInference(AExPath, TEXT("potion_shop_order"),
			TEXT("A customer asks to buy two health potions."), TablePath);
	}, Refused, BatchTimeoutSeconds);
	EnqueueStep([Test, Table, Refused, HeldIndex]()
	{
		if (*HeldIndex != INDEX_NONE)
		{
			GEditor->CancelTransaction(*HeldIndex);
		}
		if (ExpectToolError(Test, TEXT("write under an open transaction"), *Refused))
		{
			Test->TestTrue(FString::Printf(TEXT("the refusal names the open transaction (%s)"), *Refused->Error),
				Refused->Error.Contains(HeldName));
		}
		Test->TestEqual(TEXT("the refused write wrote no rows"), Table->GetRowMap().Num(), 0);
		ExpectNoOpenTransaction(Test, TEXT("when the open-transaction cell ends"));
		CleanupOrderTable();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
