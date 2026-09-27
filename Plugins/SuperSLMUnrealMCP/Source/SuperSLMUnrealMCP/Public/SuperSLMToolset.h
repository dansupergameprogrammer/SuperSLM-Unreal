#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition.h"

#include "SuperSLMToolset.generated.h"

class UToolCallAsyncResultString;

// Maintainer notes, kept out of the class tooltip: the toolset follows SuperFAISSUnrealMCP's
// shape (a UCLASS of reflected AICallable statics). Do not wrap it in a custom #if:
// UnrealHeaderTool rejects a UCLASS inside a preprocessor block it does not recognize.
// SuperSLMMCPInference.h says which thread runs each step of an asynchronous tool.

/**
 * SuperSLM's MCP toolset, registered with the engine's ToolsetRegistry, which exposes it to MCP
 * clients. Every result is compact JSON.
 *
 * Read tier: describe a model, run the conversion pre-check, inspect a live sequence, report the
 * determinism self-check's prior runs (ReportCalibration; not the cost calibration). Action tier:
 * schema-constrained inference whose results are written as rows of a UDataTable. It reads from a
 * caller's prompts or from a column of another UDataTable (the first read source), and writes
 * through a transactional, undoable, schema-validated write sink. Batch variants run N inferences
 * in one pass.
 *
 * Action-tier limits and failure policy. One call runs at most 64 inputs (prompts, or source rows);
 * a larger call is refused by name, never truncated. A call is all-or-nothing: the first input that
 * fails to generate, or any result that does not fit the table, fails the whole call, nothing is
 * written, and the error names the input and how many had finished. The write is one undo step of
 * its own. While the user holds an editor transaction open (a gizmo or slider drag), the write
 * waits for it to close, for up to 120 s; if it is still open then, the call fails and the error
 * carries the run's validated rows, unwritten.
 *
 * ModelPath, in every tool that takes one, names one of two things, and the file form is checked
 * first:
 *  - an `.sslm` file on disk, by filesystem path. The read tier inspects its bytes in place; the
 *    action tier imports it transiently and never saves an asset;
 *  - an imported USuperSLMModel asset, by object path (/Game/Models/Example.Example).
 *
 * Threads. The ToolsetRegistry calls every tool on the game thread. InspectSequence answers
 * synchronously: it reads live runtime state that belongs to the game thread and does no I/O. Every
 * other tool returns a pending UToolCallAsyncResultString at once and never waits on the game
 * thread. Its result is completed later, on the game thread: with SetValue and the JSON the tool
 * describes on success, and with SetError and a message on failure.
 */
UCLASS()
class SUPERSLMUNREALMCP_API USuperSLMToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * The model inspector's readout for one model, field for field as the editor's inspection panel
	 * shows it: format version, header flags, artifact hash, section table, config, schemas carried,
	 * provenance, and the footprint for the query window's default load shape (workspace, KV block,
	 * KV pool overhead, and the upper bound of one sequence's saved state). It also gives the GPU
	 * device head's declared bytes, and the predicted reset and adopt milliseconds against the
	 * Sequence Lifecycle Budget. Results are cached per model. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* DescribeModel(const FString& ModelPath);

	/**
	 * The conversion pre-check over a local HuggingFace-snapshot checkpoint directory, before any
	 * conversion is paid for. Reports pass or fail, the blocker when it fails, and the derived
	 * context_cap, KV block size and predicted reset and adopt costs. The adopt cost uses this
	 * machine's measured adopt-to-reset ratio. It never runs the converter. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* RunPreCheck(const FString& CheckpointDir, int64 RequestedContextCap,
		double SequenceLifecycleBudgetMs);

	/**
	 * One live CPU-backend sequence, by id: its phase, last decode outcome, the tokens and text
	 * returned, the schema state, resident KV blocks and time to first token. Synchronous. A
	 * failure is an {"error": "..."} object. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static FString InspectSequence(int64 SequenceId);

	/**
	 * The determinism self-check's prior runs on this machine for one model: for each
	 * (inference-runtime version, plugin version, backend, device label), the first and most recent runs' token
	 * digests and whether they changed. Verdicts are not reported. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* ReportCalibration(const FString& ModelPath);

	/**
	 * Runs one schema-constrained inference of Prompt against ModelPath with SchemaName, which must
	 * be a schema compiled into the model. The decoded object is then written as one row of
	 * DataTablePath. Every decoded field must match a property of the table's row struct by name and
	 * type, or nothing is written. The write is one undo step of its own; while another editor
	 * transaction is open it waits for that one to close, for up to 120 s, and if it is still open
	 * then, the call fails and writes nothing. The row is named
	 * "<schema>_<first 16 hex digits of SHA-1(prompt)>", so re-running a prompt rewrites its own
	 * row. Decoding is greedy, so a re-run gives the same row. Completes with the row and its name.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* RunConstrainedInference(const FString& ModelPath, const FString& SchemaName,
		const FString& Prompt, const FString& DataTablePath);

	/**
	 * RunConstrainedInference over N prompts (at most 64) in one pass, with every row written in one
	 * undo step. Every result is validated before any row is written. A repeated prompt is refused.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* RunConstrainedInferenceBatch(const FString& ModelPath, const FString& SchemaName,
		const TArray<FString>& Prompts, const FString& DataTablePath);

	/**
	 * The read-transform-write loop over project data. It reads SourceColumn from every row of
	 * SourceDataTablePath, in row order. SourceColumn must be a string, name, text, numeric, bool or
	 * enum property. Each row's value runs through one schema-constrained inference, as Prompt
	 * followed by a blank line and then the value (the value alone when Prompt is empty). The results
	 * are written as rows of DataTablePath, one per source row, named "<schema>_<source row name>" so
	 * the two tables join on row name. All rows are written in one undo step, under the same checks
	 * as RunConstrainedInference. The source is read once, when the call starts. The source and
	 * target must be different tables. A source of more than 64 rows is refused.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperSLM")
	static UToolCallAsyncResultString* RunConstrainedInferenceOverDataTable(const FString& ModelPath, const FString& SchemaName,
		const FString& Prompt, const FString& SourceDataTablePath, const FString& SourceColumn, const FString& DataTablePath);
};
