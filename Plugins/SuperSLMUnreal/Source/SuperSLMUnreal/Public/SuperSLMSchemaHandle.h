#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;

// L2-S1's own mechanism (the plan §9 R-S1f: "Schema bind on a
// non-fresh sequence -> routed to reset-then-bind"). USuperSLMSchema -- the editor-facing asset
// wrapping a model plus a schema name, per §3 -- is L2-S3 scope (§10.4's own sequencing: "L2-S3
// -- Blueprint, editor surface, inspection ..."; the L2-S0 red suite's own scope note already
// placed "USuperSLMSchema import/editor surfaces" at L2-S3, not L2-S0 or L2-S1).
//
// A compiled schema is already fully resident wherever its model is mapped (§2.1: "no
// map/release verb -- a schema is already resident wherever the model is"), so a name-lookup
// handle is the WHOLE mechanism R-S1f needs -- it does not wait on the asset wrapper. See
// the red-suite record §6 for this scope split, recorded as an
// explicit reading rather than a guess.
struct SUPERSLMUNREAL_API FSuperSLMSchemaHandle
{
	// 0 mirrors SSLM_SCHEMA_NONE (sslm_abi.h), the ABI's own unconstrained sentinel, rather than
	// inventing a second "no schema" convention.
	uint64 Opaque = 0;

	bool IsNone() const { return Opaque == 0; }
};

// A class of static methods, matching FSuperSLMModelImport's own established shape
// (SuperSLMModelImport.h) rather than a bare namespace.
class SUPERSLMUNREAL_API FSuperSLMSchemaLookup
{
public:
	// sslm_schema_lookup by name (§2.1): SSLM_SCHEMA_NOT_FOUND surfaces as a false return with
	// OutError naming the schema and the model.
	static bool LookupByName(
		const USuperSLMModel& Model,
		const FString& SchemaName,
		FSuperSLMSchemaHandle& OutHandle,
		FString& OutError);
};
