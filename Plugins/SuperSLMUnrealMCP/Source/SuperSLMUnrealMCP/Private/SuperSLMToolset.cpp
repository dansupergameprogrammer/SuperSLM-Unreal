#include "SuperSLMToolset.h"

#include "SuperSLMMCPInference.h"
#include "SuperSLMMCPReadSource.h"
#include "SuperSLMMCPWriteSink.h"

#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperSLMConversionPreCheck.h"
#include "SuperSLMDetokenizer.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMSequenceLifecycleBudget.h"
#include "SuperSLMSequenceTypes.h"
#include "SuperSLMSubsystem.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

// Every result is compact JSON. Tools are called on the game thread, where the ToolsetRegistry
// dispatches them. InspectSequence answers synchronously: it reads live game-thread state and does
// no I/O, and a failure is an {"error": "..."} object. Every other
// tool returns a pending UToolCallAsyncResultString and never waits on the game thread. A success
// completes it with SetValue; a failure completes it with SetError, so an MCP client sees a tool
// error. Loads go through LoadPackageAsync. The read tier's file reads, parsing and inspection run
// on the background queue, and the action tier runs on the shared runtime host
// (SuperSLMMCPInference.h). The action tier's write runs here, on the game thread, in one
// transaction of its own (SuperSLMMCPWriteSink.h), once the run has finished.
namespace
{
	FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString JsonError(const FString& Message)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("error"), Message);
		return ToJson(Object);
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	using FPendingResult = TStrongObjectPtr<UToolCallAsyncResultString>;

	// The most inputs (prompts, or source rows) one action-tier call runs. A call is one run on
	// the shared host, all-or-nothing, and every other action-tier call waits behind it; at the
	// CPU rate the suite measures (about 1.75 s per input) this cap keeps one call near two
	// minutes. A larger table is refused by name rather than truncated. Declared, not derived.
	constexpr int32 kMaxInputsPerCall = 64;

	// A pending result and the reference that keeps it alive until it completes (the registry's
	// future handler does not keep it alive).
	FPendingResult NewPendingResult()
	{
		return FPendingResult(NewObject<UToolCallAsyncResultString>());
	}

	// A background result: seeded with the cancellation a task skipped at shutdown reports.
	struct FBackgroundResult
	{
		bool bOk = false;
		FString Text = TEXT("cancelled: the editor is shutting down"); // JSON on success, the error otherwise
	};

	void Complete(const FPendingResult& Result, const FBackgroundResult& Outcome)
	{
		if (Outcome.bOk)
		{
			Result->SetValue(Outcome.Text);
		}
		else
		{
			Result->SetError(Outcome.Text);
		}
	}

	// Runs Work on the background queue and completes Result from the game thread.
	void CompleteFromBackground(const FPendingResult& Result, TUniqueFunction<FBackgroundResult()> Work)
	{
		TSharedRef<FBackgroundResult> Outcome = MakeShared<FBackgroundResult>();
		if (!SuperSLMMCPInference::RunInBackground(
				[Outcome, Work = MoveTemp(Work)]() mutable { *Outcome = Work(); },
				[Outcome, Result]() { Complete(Result, *Outcome); }))
		{
			Result->SetError(TEXT("busy or shutting down: too many read-tier calls are already waiting, or the editor is exiting; retry later"));
		}
	}

	FBackgroundResult Succeeded(const TSharedRef<FJsonObject>& Object)
	{
		FBackgroundResult Out;
		Out.bOk = true;
		Out.Text = ToJson(Object);
		return Out;
	}

	FBackgroundResult Failed(const FString& Error)
	{
		FBackgroundResult Out;
		Out.Text = Error;
		return Out;
	}

	const TCHAR* PhaseName(ESuperSLMSequencePhase Phase)
	{
		switch (Phase)
		{
		case ESuperSLMSequencePhase::Idle: return TEXT("Idle");
		case ESuperSLMSequencePhase::Prefilling: return TEXT("Prefilling");
		case ESuperSLMSequencePhase::Decoding: return TEXT("Decoding");
		case ESuperSLMSequencePhase::Complete: return TEXT("Complete");
		default: return TEXT("Faulted");
		}
	}

	const TCHAR* OutcomeName(ESuperSLMDecodeOutcome Outcome)
	{
		switch (Outcome)
		{
		case ESuperSLMDecodeOutcome::TokenProduced: return TEXT("TokenProduced");
		case ESuperSLMDecodeOutcome::Generating: return TEXT("Generating");
		case ESuperSLMDecodeOutcome::SchemaDeadEnd: return TEXT("SchemaDeadEnd");
		default: return TEXT("SequenceNoLongerValid");
		}
	}

	TArray<TSharedPtr<FJsonValue>> TokenValues(const TArray<int32>& Tokens)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Tokens.Num());
		for (const int32 Token : Tokens)
		{
			Values.Add(MakeShared<FJsonValueNumber>(Token));
		}
		return Values;
	}

	// The inspector's readout as JSON. Every figure comes from SuperSLMModelInspector::InspectBytes,
	// so it agrees with the editor's inspection panel field for field.
	TSharedRef<FJsonObject> DescribeJson(const FString& ModelPath, bool bIsFile, const FSuperSLMModelInspection& I)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("path"), ModelPath);
		Object->SetStringField(TEXT("source"), bIsFile ? TEXT("file (inspected in place; no asset created)") : TEXT("asset"));
		Object->SetStringField(TEXT("artifactHash"), I.ArtifactHashHex);
		Object->SetNumberField(TEXT("fileBytes"), static_cast<double>(I.FileBytes));
		Object->SetNumberField(TEXT("formatVersion"), I.FormatVersion);
		Object->SetNumberField(TEXT("headerFlags"), I.Flags);

		TArray<TSharedPtr<FJsonValue>> Sections;
		bool bHasTokenizer = false;
		for (const FSuperSLMSectionRow& Row : I.Sections)
		{
			const TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetNumberField(TEXT("type"), Row.Type);
			S->SetStringField(TEXT("name"), Row.TypeName);
			S->SetNumberField(TEXT("dtype"), Row.Dtype);
			S->SetNumberField(TEXT("offset"), static_cast<double>(Row.Offset));
			S->SetNumberField(TEXT("byteSize"), static_cast<double>(Row.ByteSize));
			Sections.Add(MakeShared<FJsonValueObject>(S));
			bHasTokenizer |= Row.TypeName == TEXT("Tokenizer");
		}
		Object->SetArrayField(TEXT("sections"), Sections);
		Object->SetBoolField(TEXT("hasTokenizer"), bHasTokenizer);
		Object->SetStringField(TEXT("dampedGreedyConstants"), I.bHasDampedGreedyConstants ? TEXT("present, unused at 1.0") : TEXT("absent"));

		const TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
		Config->SetNumberField(TEXT("hiddenSize"), I.HiddenSize);
		Config->SetNumberField(TEXT("numHiddenLayers"), I.NumHiddenLayers);
		Config->SetNumberField(TEXT("numAttentionHeads"), I.NumAttentionHeads);
		Config->SetNumberField(TEXT("numKeyValueHeads"), I.NumKeyValueHeads);
		Config->SetNumberField(TEXT("headDim"), I.HeadDim);
		Config->SetNumberField(TEXT("vocabSize"), I.VocabSize);
		Config->SetNumberField(TEXT("contextCap"), static_cast<double>(I.ContextCap));
		Config->SetNumberField(TEXT("kvPrecisionBytes"), I.KvPrecisionBytes);
		Object->SetObjectField(TEXT("config"), Config);

		TArray<TSharedPtr<FJsonValue>> Schemas;
		for (const FString& Name : I.SchemaNames)
		{
			Schemas.Add(MakeShared<FJsonValueString>(Name));
		}
		Object->SetArrayField(TEXT("schemas"), Schemas);

		if (const TSharedPtr<FJsonObject> Provenance = ParseObject(I.ProvenanceJson))
		{
			Object->SetObjectField(TEXT("provenance"), Provenance);
		}
		else
		{
			Object->SetStringField(TEXT("provenance"), I.ProvenanceJson);
		}

		// The footprint, for the declared shape the inspection panel uses (the query window's
		// default load shape).
		const TSharedRef<FJsonObject> Footprint = MakeShared<FJsonObject>();
		Footprint->SetNumberField(TEXT("workspaceBytes"), static_cast<double>(I.WorkspaceBytes));
		Footprint->SetNumberField(TEXT("kvBlockBytes"), static_cast<double>(I.KvBlockBytes));
		Footprint->SetNumberField(TEXT("kvPoolOverheadBytes"), static_cast<double>(I.KvPoolOverheadBytes));
		Footprint->SetNumberField(TEXT("seqStateBytesUpperBound"), static_cast<double>(I.SeqStateBytesUpperBound));
		Footprint->SetStringField(TEXT("declaredShape"), TEXT("the editor query window's default load (the inspection panel's shape)"));
		Object->SetObjectField(TEXT("footprint"), Footprint);

		// The GPU device head's declared device-local bytes, as the inspector computes them.
		const TSharedRef<FJsonObject> GpuHead = MakeShared<FJsonObject>();
		GpuHead->SetBoolField(TEXT("deviceResidentHead"), I.bGpuDeviceResidentHead);
		GpuHead->SetNumberField(TEXT("declaredBytesLowerBound"), static_cast<double>(I.GpuDeviceHeadDeclaredBytes));
		Object->SetObjectField(TEXT("gpuHead"), GpuHead);

		const TSharedRef<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
		Lifecycle->SetNumberField(TEXT("predictedResetMs"), I.PredictedResetMs);
		Lifecycle->SetNumberField(TEXT("predictedAdoptMs"), I.PredictedAdoptMs);
		Lifecycle->SetNumberField(TEXT("measuredWriteBandwidthBytesPerSec"), I.MeasuredBandwidthBytesPerSec);
		Lifecycle->SetNumberField(TEXT("sequenceLifecycleBudgetMs"), I.SequenceLifecycleBudgetMs);
		Lifecycle->SetBoolField(TEXT("withinBudget"), I.bWithinLifecycleBudget);
		Object->SetObjectField(TEXT("sequenceLifecycle"), Lifecycle);
		return Object;
	}

	// A worker: the self-check's prior-run store for one artifact hash. The store is what the plugin's
	// determinism self-check writes: one file per key "<artifact hash>|<Layer-1 tag and commit>|
	// <plugin version>|<backend>|<device label>", holding the first-ever and most recent runs' joined
	// token digests.
	FBackgroundResult CalibrationOnWorker(const FString& HashHex)
	{
		const FString StoreDir = FPaths::ProjectSavedDir() / TEXT("SuperSLM") / TEXT("SelfCheck");
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(StoreDir / TEXT("*.json")), true, false);
		Files.Sort();

		TArray<TSharedPtr<FJsonValue>> Runs;
		for (const FString& File : Files)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *(StoreDir / File)))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Record = ParseObject(Text);
			FString Key;
			if (!Record.IsValid() || !Record->TryGetStringField(TEXT("key"), Key))
			{
				continue;
			}
			TArray<FString> Parts;
			Key.ParseIntoArray(Parts, TEXT("|"), false);
			if (Parts.Num() != 5 || !Parts[0].Equals(HashHex, ESearchCase::IgnoreCase))
			{
				continue;
			}
			FString First;
			FString Last;
			Record->TryGetStringField(TEXT("first"), First);
			Record->TryGetStringField(TEXT("last"), Last);
			const TSharedRef<FJsonObject> Run = MakeShared<FJsonObject>();
			Run->SetStringField(TEXT("layerOneTagAndCommit"), Parts[1]);
			Run->SetStringField(TEXT("pluginVersion"), Parts[2]);
			Run->SetStringField(TEXT("backend"), Parts[3]);
			Run->SetStringField(TEXT("deviceLabel"), Parts[4]);
			Run->SetStringField(TEXT("firstRunDigests"), First);
			Run->SetStringField(TEXT("mostRecentRunDigests"), Last);
			Run->SetStringField(TEXT("sinceFirstRun"), First == Last ? TEXT("unchanged") : TEXT("changed"));
			Runs.Add(MakeShared<FJsonValueObject>(Run));
		}

		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("artifactHash"), HashHex);
		Object->SetNumberField(TEXT("count"), Runs.Num());
		Object->SetArrayField(TEXT("runs"), Runs);
		Object->SetStringField(TEXT("verdict"),
			TEXT("not reported: the self-check's prior-run store records token digests, not verdicts or scope, and in 1.0 the GPU verdict is always withheld"));
		return Succeeded(Object);
	}

	// A worker: the whole pre-check (file checks, the config.json read and parse, the bandwidth and
	// adopt-ratio measurement).
	FBackgroundResult PreCheckOnWorker(const FString& CheckpointDir, int64 RequestedContextCap, double SequenceLifecycleBudgetMs)
	{
		if (CheckpointDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*CheckpointDir))
		{
			return Failed(FString::Printf(TEXT("no checkpoint directory at %s"), *CheckpointDir));
		}
		if (!FPaths::FileExists(CheckpointDir / TEXT("config.json")))
		{
			return Failed(FString::Printf(TEXT("%s has no config.json"), *CheckpointDir));
		}
		double Bandwidth = 0.0;
		double AdoptToResetRatio = 0.0;
		SuperSLMSequenceLifecycleBudget::MeasureHostBandwidthAndAdoptRatio(Bandwidth, AdoptToResetRatio);
		FSuperSLMConversionPreCheckReport Report;
		if (!SuperSLMConversionPreCheck::Run(CheckpointDir, RequestedContextCap, SequenceLifecycleBudgetMs, Bandwidth,
				AdoptToResetRatio, Report))
		{
			return Failed(FString::Printf(TEXT("the pre-check could not run on %s (no readable config.json)"), *CheckpointDir));
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("checkpointDir"), CheckpointDir);
		Object->SetBoolField(TEXT("pass"), Report.bPass);
		if (!Report.bPass)
		{
			Object->SetStringField(TEXT("blockerReason"), Report.BlockerReason);
		}
		Object->SetStringField(TEXT("modelType"), Report.ModelType);
		Object->SetNumberField(TEXT("derivedHeadDim"), Report.DerivedHeadDim);
		Object->SetNumberField(TEXT("derivedContextCap"), static_cast<double>(Report.DerivedContextCap));
		Object->SetNumberField(TEXT("predictedKvBlockSizeBytes"), static_cast<double>(Report.PredictedKvBlockSizeBytes));
		Object->SetNumberField(TEXT("predictedResetMs"), Report.PredictedResetMs);
		Object->SetNumberField(TEXT("predictedAdoptMs"), Report.PredictedAdoptMs);
		Object->SetNumberField(TEXT("measuredWriteBandwidthBytesPerSec"), Bandwidth);
		Object->SetNumberField(TEXT("measuredAdoptToResetRatio"), AdoptToResetRatio);
		Object->SetStringField(TEXT("meaning"), TEXT("a pass means no known blocker; the real conversion runs Layer 1's converter out of process"));
		return Succeeded(Object);
	}

	// A row's name when the input is a caller's prompt: a function of the schema and the prompt alone,
	// so re-running a prompt rewrites its own row and a batch's rows never depend on the table's prior
	// contents: "<schema>_<first 16 hex of SHA-1(UTF-8 prompt)>".
	FName RowNameFor(const FString& SchemaName, const FString& Prompt)
	{
		const FTCHARToUTF8 Utf8(*Prompt);
		uint8 Hash[20];
		FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Hash);
		return FName(*FString::Printf(TEXT("%s_%s"), *SchemaName, *BytesToHex(Hash, 8).ToLower()));
	}

	// Game thread, when the run's decoded text arrives: parse every decoded object and validate every
	// one against the table, before anything is written. Returns the sink holding the validated rows.
	TUniquePtr<ISuperSLMMCPWriteSink> ValidateDecodedRows(UDataTable* Table, const TArray<FSuperSLMMCPInput>& Inputs,
		const TArray<FString>& Decoded, TArray<FSuperSLMMCPSinkRecord>& OutRecords, FString& OutError)
	{
		TUniquePtr<ISuperSLMMCPWriteSink> Sink = SuperSLMMCPWriteSink::MakeDataTableRowSink(Table, OutError);
		if (!Sink.IsValid())
		{
			return nullptr;
		}
		if (Decoded.Num() != Inputs.Num())
		{
			OutError = TEXT("the run returned a different number of results than inputs");
			return nullptr;
		}
		OutRecords.Reset();
		for (int32 I = 0; I < Inputs.Num(); ++I)
		{
			FSuperSLMMCPSinkRecord Record;
			Record.RowName = Inputs[I].RowName;
			Record.Fields = ParseObject(Decoded[I]);
			if (!Record.Fields.IsValid())
			{
				OutError = FString::Printf(TEXT("input %d (%s): the constrained output is not a JSON object: %s"), I,
					*Inputs[I].RowName.ToString(), *Decoded[I]);
				return nullptr;
			}
			OutRecords.Add(MoveTemp(Record));
		}
		if (!Sink->Validate(OutRecords, OutError))
		{
			return nullptr;
		}
		return Sink;
	}


	TSharedRef<FJsonObject> RowObject(const FSuperSLMMCPSinkRecord& Record)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("rowName"), Record.RowName.ToString());
		Row->SetObjectField(TEXT("row"), Record.Fields);
		return Row;
	}

	// A finished run's rows that were validated but not written, for the refusal's error text, so
	// the caller keeps the results of the run.
	FString UnwrittenRowsJson(const TArray<FSuperSLMMCPSinkRecord>& Records)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FSuperSLMMCPSinkRecord& Record : Records)
		{
			Rows.Add(MakeShared<FJsonValueObject>(RowObject(Record)));
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetArrayField(TEXT("unwrittenRows"), Rows);
		return ToJson(Object);
	}

	FString SuccessJson(bool bBatch, const FString& SchemaName, const FString& Target, const FString& SourcePath,
		const TArray<FSuperSLMMCPSinkRecord>& Records)
	{
		if (!bBatch)
		{
			const TSharedRef<FJsonObject> Object = RowObject(Records[0]);
			Object->SetStringField(TEXT("dataTable"), Target);
			Object->SetStringField(TEXT("schema"), SchemaName);
			return ToJson(Object);
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("dataTable"), Target);
		Object->SetStringField(TEXT("schema"), SchemaName);
		if (!SourcePath.IsEmpty())
		{
			Object->SetStringField(TEXT("source"), SourcePath);
		}
		Object->SetNumberField(TEXT("count"), Records.Num());
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FSuperSLMMCPSinkRecord& Record : Records)
		{
			Rows.Add(MakeShared<FJsonValueObject>(RowObject(Record)));
		}
		Object->SetArrayField(TEXT("rows"), Rows);
		return ToJson(Object);
	}

	// Resolves the target table without blocking (already loaded, or LoadPackageAsync), and checks it
	// has a row struct. Game thread.
	void ResolveTargetTable(const FPendingResult& Result, const FString& DataTablePath, TUniqueFunction<void(UDataTable&)> OnTable)
	{
		SuperSLMMCPInference::ResolveObjectAsync(UDataTable::StaticClass(), DataTablePath,
			[Result, DataTablePath, OnTable = MoveTemp(OnTable)](UObject* Object, FString&& Error) mutable
			{
				UDataTable* Table = Cast<UDataTable>(Object);
				if (Table == nullptr)
				{
					Result->SetError(FString::Printf(TEXT("no data table at %s (%s)"), *DataTablePath, *Error));
					return;
				}
				if (Table->GetRowStruct() == nullptr)
				{
					Result->SetError(FString::Printf(TEXT("data table %s has no row struct"), *DataTablePath));
					return;
				}
				OnTable(*Table);
			});
	}

	// The action tier's one path, once the inputs and the target table are known. Every stage runs on
	// the game thread and none of them waits. Each stage either completes the result or hands on to
	// the next through a callback:
	//  1. resolve the model: a file needs no load here; an asset is loaded by LoadPackageAsync if
	//     it is not already loaded;
	//  2. queue the run on the shared runtime host (import, configure and inference run off the game
	//     thread there);
	//  3. when the run ends, validate every row at once, then write them all in one transaction of
	//     its own: at once, or, while the user holds an editor transaction open, on the first frame
	//     after it closes (SuperSLMMCPWriteSink::ApplyWhenNoTransactionOpen). A write refused after
	//     that wait returns the validated rows, unwritten, in its error.
	void RunIntoTable(const FPendingResult& Result, const FString& ModelPath, const FString& SchemaName,
		TArray<FSuperSLMMCPInput>&& Inputs, UDataTable& Target, const FString& SourcePath, bool bBatch)
	{
		TWeakObjectPtr<UDataTable> WeakTable(&Target);
		SuperSLMMCPInference::ResolveModelSourceAsync(ModelPath,
			[Result, SchemaName, Inputs = MoveTemp(Inputs), WeakTable, SourcePath, bBatch](bool bResolved, FSuperSLMMCPModelSource&& Source, FString&& ModelError) mutable
			{
				if (!bResolved)
				{
					Result->SetError(ModelError);
					return;
				}
				TArray<FSuperSLMMCPInput> RunInputs = Inputs; // the write needs the row names back
				SuperSLMMCPInference::RunConstrainedAsync(MoveTemp(Source), SchemaName, MoveTemp(RunInputs),
					[Result, SchemaName, Inputs = MoveTemp(Inputs), WeakTable, SourcePath, bBatch](FSuperSLMMCPConstrainedOutcome&& Outcome)
					{
						if (!Outcome.bOk)
						{
							Result->SetError(Outcome.Error);
							return;
						}
						TArray<FSuperSLMMCPSinkRecord> Records;
						FString WriteError;
						TUniquePtr<ISuperSLMMCPWriteSink> Sink = ValidateDecodedRows(WeakTable.Get(), Inputs, Outcome.Decoded, Records, WriteError);
						if (!Sink.IsValid())
						{
							Result->SetError(WriteError);
							return;
						}
						const FString Target = Sink->GetTargetPath();
						TArray<FSuperSLMMCPSinkRecord> ToWrite = Records;
						SuperSLMMCPWriteSink::ApplyWhenNoTransactionOpen(MoveTemp(Sink), MoveTemp(ToWrite),
							[Result, bBatch, SchemaName, Target, SourcePath, Records = MoveTemp(Records)](bool bWritten, FString&& Error)
							{
								if (!bWritten)
								{
									Result->SetError(FString::Printf(TEXT("%s. The run's rows, validated but not written: %s"),
										*Error, *UnwrittenRowsJson(Records)));
									return;
								}
								Result->SetValue(SuccessJson(bBatch, SchemaName, Target, SourcePath, Records));
							});
					});
			});
	}

	// The prompt tools' inputs: a schema name, at least one prompt, no empty prompt, and no two prompts
	// that would write the same row.
	bool PromptInputs(const FString& SchemaName, const TArray<FString>& Prompts, const FString& DataTablePath,
		TArray<FSuperSLMMCPInput>& OutInputs, FString& OutError)
	{
		if (Prompts.Num() == 0)
		{
			OutError = TEXT("Prompts is empty");
			return false;
		}
		if (Prompts.Num() > kMaxInputsPerCall)
		{
			OutError = FString::Printf(TEXT("%d prompts; one call runs at most %d. Split them across calls."), Prompts.Num(), kMaxInputsPerCall);
			return false;
		}
		if (SchemaName.IsEmpty())
		{
			OutError = TEXT("SchemaName is empty");
			return false;
		}
		if (DataTablePath.IsEmpty())
		{
			OutError = TEXT("DataTablePath is empty");
			return false;
		}
		TSet<FName> RowNames;
		for (int32 I = 0; I < Prompts.Num(); ++I)
		{
			if (Prompts[I].IsEmpty())
			{
				OutError = FString::Printf(TEXT("prompt %d is empty"), I);
				return false;
			}
			const FName RowName = RowNameFor(SchemaName, Prompts[I]);
			if (RowNames.Contains(RowName))
			{
				OutError = FString::Printf(TEXT("prompt %d repeats an earlier prompt; each prompt writes its own row"), I);
				return false;
			}
			RowNames.Add(RowName);
			OutInputs.Add(FSuperSLMMCPInput{RowName, Prompts[I]});
		}
		return true;
	}

	UToolCallAsyncResultString* StartPromptRun(const FString& ModelPath, const FString& SchemaName,
		const TArray<FString>& Prompts, const FString& DataTablePath, bool bBatch)
	{
		check(IsInGameThread());
		const FPendingResult Result = NewPendingResult();
		TArray<FSuperSLMMCPInput> Inputs;
		FString Error;
		if (!PromptInputs(SchemaName, Prompts, DataTablePath, Inputs, Error))
		{
			Result->SetError(Error);
			return Result.Get();
		}
		ResolveTargetTable(Result, DataTablePath,
			[Result, ModelPath, SchemaName, Inputs = MoveTemp(Inputs), bBatch](UDataTable& Target) mutable
			{
				RunIntoTable(Result, ModelPath, SchemaName, MoveTemp(Inputs), Target, FString(), bBatch);
			});
		return Result.Get();
	}

	// The read source's input for one record: the caller's prompt, a blank line, then the record's
	// text. With no prompt, the record's text alone.
	FString ComposeInput(const FString& Prompt, const FString& RecordText)
	{
		return Prompt.IsEmpty() ? RecordText : Prompt + TEXT("\n\n") + RecordText;
	}
}

FString USuperSLMToolset::InspectSequence(int64 SequenceId)
{
	if (SequenceId <= 0)
	{
		return JsonError(TEXT("SequenceId must be a vended sequence's positive id"));
	}

	// CPU-backend sequences only: a GPU sequence is a different handle type on a different
	// subsystem. A slot faulted by Layer 1's -3 reads the same as an unknown id and is not found.
	TArray<TSharedPtr<FJsonValue>> Matches;
	for (TObjectIterator<USuperSLMSubsystem> It; It; ++It)
	{
		USuperSLMSubsystem* Cpu = *It;
		// The action tier's own runtime is driven from its dedicated thread and holds no sequence a
		// caller could have been given, so it is never read here.
		if (Cpu->HasAnyFlags(RF_ClassDefaultObject) || SuperSLMMCPInference::IsHostRuntime(Cpu) ||
			Cpu->GetConfiguredModel() == nullptr)
		{
			continue;
		}
		FSuperSLMSequence Seq;
		Seq.Id = SequenceId;
		const ESuperSLMSequencePhase Phase = Cpu->GetPhase(Seq);
		const ESuperSLMDecodeOutcome Outcome = Cpu->GetLastDecodeOutcome(Seq);
		if (Phase == ESuperSLMSequencePhase::Faulted && Outcome == ESuperSLMDecodeOutcome::SequenceNoLongerValid)
		{
			continue;
		}

		const TArray<int32>& Tokens = Cpu->GetGeneratedTokens(Seq);
		const FSuperSLMSequenceStats Stats = Cpu->GetStats(Seq);
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("subsystem"), Cpu->GetPathName());
		Entry->SetStringField(TEXT("model"), Cpu->GetConfiguredModel()->GetPathName());
		Entry->SetStringField(TEXT("phase"), PhaseName(Phase));
		Entry->SetStringField(TEXT("lastDecodeOutcome"), OutcomeName(Outcome));
		Entry->SetArrayField(TEXT("tokensReturned"), TokenValues(Tokens));
		FString Text;
		FString DetokenizeError;
		if (SuperSLM::DetokenizeTokens(*Cpu->GetConfiguredModel(), Tokens, Text, DetokenizeError))
		{
			Entry->SetStringField(TEXT("textReturned"), Text);
		}
		else
		{
			Entry->SetStringField(TEXT("textReturnedError"), DetokenizeError);
		}
		const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetBoolField(TEXT("accepting"), Stats.bSchemaAccepting);
		Schema->SetNumberField(TEXT("forcedTokenCount"), static_cast<double>(Stats.ForcedTokenCount));
		Entry->SetObjectField(TEXT("schemaState"), Schema);
		Entry->SetNumberField(TEXT("kvBlocksResident"), Stats.KvBlocksResident);
		Entry->SetNumberField(TEXT("decodeStepActual"), static_cast<double>(Stats.DecodeStepActual));
		Entry->SetNumberField(TEXT("timeToFirstTokenMs"), Cpu->GetTimeToFirstTokenMs(Seq));
		Entry->SetStringField(TEXT("contextFed"),
			TEXT("not reported: USuperSLMSubsystem exposes no accessor for a sequence's prompt tokens"));
		Matches.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (Matches.Num() == 0)
	{
		return JsonError(FString::Printf(TEXT("no live CPU-backend sequence with id %lld"), SequenceId));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("sequenceId"), static_cast<double>(SequenceId));
	Object->SetArrayField(TEXT("matches"), Matches);
	return ToJson(Object);
}

UToolCallAsyncResultString* USuperSLMToolset::DescribeModel(const FString& ModelPath)
{
	const FPendingResult Result = NewPendingResult();
	SuperSLMMCPInference::ResolveModelSourceAsync(ModelPath,
		[Result, ModelPath](bool bOk, FSuperSLMMCPModelSource&& Source, FString&& Error)
		{
			if (!bOk)
			{
				Result->SetError(Error);
				return;
			}
			const bool bIsFile = Source.bIsFile;
			SuperSLMMCPInference::InspectModelAsync(MoveTemp(Source),
				[Result, ModelPath, bIsFile](const FSuperSLMModelInspection& Inspection)
				{
					if (!Inspection.bValid)
					{
						Result->SetError(Inspection.Error);
						return;
					}
					Result->SetValue(ToJson(DescribeJson(ModelPath, bIsFile, Inspection)));
				});
		});
	return Result.Get();
}

UToolCallAsyncResultString* USuperSLMToolset::RunPreCheck(const FString& CheckpointDir, int64 RequestedContextCap,
	double SequenceLifecycleBudgetMs)
{
	const FPendingResult Result = NewPendingResult();
	CompleteFromBackground(Result, [CheckpointDir, RequestedContextCap, SequenceLifecycleBudgetMs]()
	{
		return PreCheckOnWorker(CheckpointDir, RequestedContextCap, SequenceLifecycleBudgetMs);
	});
	return Result.Get();
}

UToolCallAsyncResultString* USuperSLMToolset::ReportCalibration(const FString& ModelPath)
{
	// The artifact hash keys the self-check's store. It comes from the inspector, which is cached
	// per model, so a model already described costs no second read.
	const FPendingResult Result = NewPendingResult();
	SuperSLMMCPInference::ResolveModelSourceAsync(ModelPath,
		[Result](bool bOk, FSuperSLMMCPModelSource&& Source, FString&& Error)
		{
			if (!bOk)
			{
				Result->SetError(Error);
				return;
			}
			SuperSLMMCPInference::InspectModelAsync(MoveTemp(Source),
				[Result](const FSuperSLMModelInspection& Inspection)
				{
					if (Inspection.ArtifactHashHex.IsEmpty())
					{
						Result->SetError(Inspection.Error.IsEmpty() ? TEXT("the artifact carries no hash") : Inspection.Error);
						return;
					}
					CompleteFromBackground(Result, [HashHex = Inspection.ArtifactHashHex]() { return CalibrationOnWorker(HashHex); });
				});
		});
	return Result.Get();
}

UToolCallAsyncResultString* USuperSLMToolset::RunConstrainedInference(const FString& ModelPath, const FString& SchemaName,
	const FString& Prompt, const FString& DataTablePath)
{
	return StartPromptRun(ModelPath, SchemaName, {Prompt}, DataTablePath, /*bBatch*/ false);
}

UToolCallAsyncResultString* USuperSLMToolset::RunConstrainedInferenceBatch(const FString& ModelPath, const FString& SchemaName,
	const TArray<FString>& Prompts, const FString& DataTablePath)
{
	return StartPromptRun(ModelPath, SchemaName, Prompts, DataTablePath, /*bBatch*/ true);
}

UToolCallAsyncResultString* USuperSLMToolset::RunConstrainedInferenceOverDataTable(const FString& ModelPath, const FString& SchemaName,
	const FString& Prompt, const FString& SourceDataTablePath, const FString& SourceColumn, const FString& DataTablePath)
{
	check(IsInGameThread());
	const FPendingResult Result = NewPendingResult();
	if (SchemaName.IsEmpty() || SourceColumn.IsEmpty() || SourceDataTablePath.IsEmpty() || DataTablePath.IsEmpty())
	{
		Result->SetError(TEXT("SchemaName, SourceDataTablePath, SourceColumn and DataTablePath are all required"));
		return Result.Get();
	}
	// Source first: its records are read once, on the game thread, before anything runs.
	SuperSLMMCPInference::ResolveObjectAsync(UDataTable::StaticClass(), SourceDataTablePath,
		[Result, ModelPath, SchemaName, Prompt, SourceDataTablePath, SourceColumn, DataTablePath](UObject* Object, FString&& Error)
		{
			UDataTable* SourceTable = Cast<UDataTable>(Object);
			if (SourceTable == nullptr)
			{
				Result->SetError(FString::Printf(TEXT("no source data table at %s (%s)"), *SourceDataTablePath, *Error));
				return;
			}
			FString SourceError;
			const TUniquePtr<ISuperSLMMCPReadSource> Source =
				SuperSLMMCPReadSource::MakeDataTableColumnSource(SourceTable, SourceColumn, SourceError);
			TArray<FSuperSLMMCPSourceRecord> Records;
			if (!Source.IsValid() || !Source->Read(Records, SourceError))
			{
				Result->SetError(SourceError);
				return;
			}
			if (Records.Num() == 0)
			{
				Result->SetError(FString::Printf(TEXT("source data table %s has no rows"), *SourceDataTablePath));
				return;
			}
			if (Records.Num() > kMaxInputsPerCall)
			{
				Result->SetError(FString::Printf(TEXT("source data table %s has %d rows; one call runs at most %d. Split the table, or run it in parts."),
					*SourceDataTablePath, Records.Num(), kMaxInputsPerCall));
				return;
			}
			// One output row per source row, named after it, so the two tables join on row name.
			TArray<FSuperSLMMCPInput> Inputs;
			Inputs.Reserve(Records.Num());
			for (const FSuperSLMMCPSourceRecord& Record : Records)
			{
				Inputs.Add(FSuperSLMMCPInput{FName(*FString::Printf(TEXT("%s_%s"), *SchemaName, *Record.Key.ToString())),
					ComposeInput(Prompt, Record.Text)});
			}
			const FString SourcePath = Source->GetSourcePath();
			TWeakObjectPtr<UDataTable> WeakSource(SourceTable);
			ResolveTargetTable(Result, DataTablePath,
				[Result, ModelPath, SchemaName, Inputs = MoveTemp(Inputs), SourcePath, WeakSource](UDataTable& Target) mutable
				{
					if (WeakSource.Get() == &Target)
					{
						Result->SetError(TEXT("the source and target data tables are the same table; writing would replace the rows being read"));
						return;
					}
					RunIntoTable(Result, ModelPath, SchemaName, MoveTemp(Inputs), Target, SourcePath, /*bBatch*/ true);
				});
		});
	return Result.Get();
}

