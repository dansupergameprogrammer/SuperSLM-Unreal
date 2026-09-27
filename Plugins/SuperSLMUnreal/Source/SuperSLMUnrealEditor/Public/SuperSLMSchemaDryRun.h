#pragma once

#include "CoreMinimal.h"
#include "SuperSLMSequenceTypes.h"

class USuperSLMModel;
class USuperSLMSubsystem;

// L2-S3 (plan §7 item 4, the inspect half; §10.4). "Enumerate, resolve, and dry-run a schema on a
// scratch sequence, reporting schema_accepting, forced_token_count and any -2." Enumeration is the
// model inspector's SchemaNames (SuperSLMModelInspector.h); resolution is
// FSuperSLMSchemaLookup::LookupByName; the dry run vends one CPU sequence, binds the schema at the
// vend, generates from the prompt until the walk dead-ends, completes or reaches MaxNewTokens, and
// returns the sequence. It reads only what the subsystem already reports (GetStats(),
// GetLastDecodeOutcome(), GetGeneratedTokens()). Authoring is L2-S4.
//
// One frame at a time, like the query window's controller: Begin() once, Tick() once per editor
// frame (the subsystem's own bounded Tick), so the editor never blocks on inference. Game thread.
struct SUPERSLMUNREALEDITOR_API FSuperSLMSchemaDryRunReport
{
	FString SchemaName;
	TArray<int32> GeneratedTokens;
	FString Text;                 // GeneratedTokens detokenized
	bool bSchemaAccepting = false; // GetStats().bSchemaAccepting at the stop
	int64 ForcedTokenCount = 0;    // GetStats().ForcedTokenCount at the stop
	bool bDeadEnd = false;         // the walk returned -2 (ESuperSLMDecodeOutcome::SchemaDeadEnd)
	TOptional<ESuperSLMQueryStopReason> StopReason; // SuperSLMPromptResult::ComposeStopReason()
	int32 Frames = 0;
};

class SUPERSLMUNREALEDITOR_API FSuperSLMSchemaDryRun
{
public:
	FSuperSLMSchemaDryRun(USuperSLMSubsystem& InSubsystem, USuperSLMModel& InModel);
	~FSuperSLMSchemaDryRun();

	bool Begin(const FString& SchemaName, const FString& PromptText, int32 MaxNewTokens, FString& OutError);

	// True once the dry run has stopped; bOutSucceeded says whether OutReport is filled.
	bool Tick(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMSchemaDryRunReport& OutReport, FString& OutError);

	bool IsRunning() const { return Sequence.IsValid(); }
	void Cancel();

	// Forgets the scratch sequence without returning it: for when the subsystem was re-Configure()d
	// or shut down under the dry run, which returned every sequence with its pool.
	void Abandon() { Sequence = FSuperSLMSequence(); }

private:
	USuperSLMSubsystem& Subsystem;
	USuperSLMModel& Model;
	FSuperSLMSequence Sequence;
	FString SchemaName;
	int32 PromptTokenCount = 0;
	int32 Frames = 0;
};
