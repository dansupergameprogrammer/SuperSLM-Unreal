#pragma once

// Multi-row GPU plan, ruling 41 (Q13): the plugin has one Exact draft provider. CPU Exact drafts from the learned table
// (SuperSLMCore::BuildDraft over the per-(model, schema) FDraftTableStore that USuperSLMSubsystem owns and feeds); GPU
// multi-row Exact reads the same table, through this accessor, and never keeps a table of its own. Game thread: the
// snapshot is an immutable table (FDraftTableRef) that any thread may then read for its whole life, as a CPU span job
// does.

#include "CoreMinimal.h"
#include "Core/SuperSLMSchemaJumpCore.h"

class USuperSLMSubsystem;

struct FSuperSLMExactDraftProvider
{
	// The table CPU Exact would draft from now for the schema at SchemaIndex (its index in the model's SCM1 section) of
	// the model whose 32-byte artifact hash is ArtifactHash. Null when Cpu is not configured with that artifact, or holds
	// no table for the schema.
	static SuperSLMCore::FDraftTableRef Snapshot(const USuperSLMSubsystem& Cpu, const uint8* ArtifactHash, int32 SchemaIndex);
};
