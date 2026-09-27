#include "SuperSLMSchemaDryRun.h"

#include "SuperSLMDetokenizer.h"
#include "SuperSLMModelInspector.h"
#include "SuperSLMPromptResult.h"
#include "SuperSLMSchemaHandle.h"
#include "SuperSLMSubsystem.h"

FSuperSLMSchemaDryRun::FSuperSLMSchemaDryRun(USuperSLMSubsystem& InSubsystem, USuperSLMModel& InModel)
	: Subsystem(InSubsystem)
	, Model(InModel)
{
}

FSuperSLMSchemaDryRun::~FSuperSLMSchemaDryRun()
{
	Cancel();
}

void FSuperSLMSchemaDryRun::Cancel()
{
	if (Sequence.IsValid())
	{
		Subsystem.ReturnSequence(Sequence);
		Sequence = FSuperSLMSequence();
	}
}

bool FSuperSLMSchemaDryRun::Begin(const FString& InSchemaName, const FString& PromptText, int32 MaxNewTokens, FString& OutError)
{
	check(IsInGameThread());
	if (Sequence.IsValid())
	{
		OutError = TEXT("a dry run is already running");
		return false;
	}
	if (Subsystem.GetConfiguredModel() != &Model)
	{
		OutError = TEXT("the CPU subsystem is not configured with this model");
		return false;
	}
	FSuperSLMSchemaHandle Schema;
	if (!FSuperSLMSchemaLookup::LookupByName(Model, InSchemaName, Schema, OutError))
	{
		return false;
	}
	FSuperSLMGenerationRequest Request;
	if (!SuperSLM::TokenizeText(Model, PromptText, Request.PromptTokens, OutError))
	{
		return false;
	}
	Request.MaxNewTokens = MaxNewTokens;
	if (Subsystem.VendSequence(Sequence) != ESuperSLMVendResult::Success)
	{
		Sequence = FSuperSLMSequence();
		OutError = TEXT("no free sequence for the scratch run");
		return false;
	}
	// The bind-eligible point: a freshly vended sequence (plan §5 CPU path item 6).
	if (!Subsystem.SetSchema(Sequence, Schema, OutError) || !Subsystem.BeginGeneration(Sequence, Request, OutError))
	{
		Cancel();
		return false;
	}
	SchemaName = InSchemaName;
	PromptTokenCount = Request.PromptTokens.Num();
	Frames = 0;
	return true;
}

bool FSuperSLMSchemaDryRun::Tick(float DeltaSeconds, bool& bOutSucceeded, FSuperSLMSchemaDryRunReport& OutReport, FString& OutError)
{
	check(IsInGameThread());
	bOutSucceeded = false;
	if (!Sequence.IsValid())
	{
		OutError = TEXT("no dry run is running");
		return true;
	}
	// Review W6: one subsystem Tick() per engine frame, shared with the query window's per-frame
	// query (FSuperSLMQueryRunner::TickQuery()), so running both does not double the tick rate.
	Subsystem.TickOncePerFrame(DeltaSeconds);
	++Frames;
	const ESuperSLMSequencePhase Phase = Subsystem.GetPhase(Sequence);
	if (Phase != ESuperSLMSequencePhase::Complete && Phase != ESuperSLMSequencePhase::Faulted)
	{
		return false;
	}
	OutReport = FSuperSLMSchemaDryRunReport();
	OutReport.SchemaName = SchemaName;
	OutReport.GeneratedTokens = Subsystem.GetGeneratedTokens(Sequence);
	const FSuperSLMSequenceStats Stats = Subsystem.GetStats(Sequence);
	OutReport.bSchemaAccepting = Stats.bSchemaAccepting;
	OutReport.ForcedTokenCount = Stats.ForcedTokenCount;
	OutReport.bDeadEnd = Subsystem.GetLastDecodeOutcome(Sequence) == ESuperSLMDecodeOutcome::SchemaDeadEnd;
	OutReport.Frames = Frames;

	SuperSLMPromptResult::FStopFacts Facts;
	Facts.Phase = Phase;
	Facts.LastOutcome = Subsystem.GetLastDecodeOutcome(Sequence);
	Facts.bSchemaAccepting = Stats.bSchemaAccepting;
	FSuperSLMModelShapeFacts Shape;
	Facts.bContextCapReached = SuperSLMModelInspector::ReadShape(Model, Shape) &&
		static_cast<int64>(PromptTokenCount) + OutReport.GeneratedTokens.Num() >= Shape.ContextCap;
	ESuperSLMQueryStopReason Reason;
	if (SuperSLMPromptResult::ComposeStopReason(Facts, Reason))
	{
		OutReport.StopReason = Reason;
	}
	Cancel();
	bOutSucceeded = SuperSLM::DetokenizeTokens(Model, OutReport.GeneratedTokens, OutReport.Text, OutError);
	return true;
}
