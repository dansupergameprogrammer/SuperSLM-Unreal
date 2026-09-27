#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;

// L2-S2's own mechanism, needed by R-S2g (the red-suite record
// §4; routed finding T-2811, D-SLM7256's GPU save/restore schema-loss gap). The GPU-1.0
// twin of SuperSLMSchemaHandle.h's CPU-side FSuperSLMSchemaHandle/FSuperSLMSchemaLookup --
// a DISTINCT type because the lookup mechanism differs by backend (SslmGpuSchemaLookupForG5Bridge
// resolves against a GPU model handle's own host-side parsed SchemaMasksTable, gpu_1p0.h,
// never the CPU sslm_schema_lookup verb), even though both ultimately index the same
// artifact-embedded SchemaMasks (SCM1) section.
struct SUPERSLMUNREAL_API FSuperSLMGpuSchemaHandle
{
	// -1 mirrors SslmGpuSchemaLookupForG5Bridge's own "no match" return and unbinds when
	// passed to USuperSLMGpuSubsystem::SetSchema() -- the GPU-1.0 twin of SSLM_SCHEMA_NONE.
	int32 Index = -1;

	bool IsNone() const { return Index < 0; }
};

class SUPERSLMUNREAL_API FSuperSLMGpuSchemaLookup
{
public:
	// SslmGpuSchemaLookupForG5Bridge by name, against Model's own GPU-mapped handle
	// (read at v1.5.0, gpu_1p0.h: "Resolves a compiled schema by name against model's
	// own host-side parsed SchemaMasksTable ... Returns the schema's own index (>= 0)
	// on a match, -1 on no match"). Model must already be Configure()'d into a GPU
	// subsystem (mapped) -- OutError names the model and the schema on a no-match.
	static bool LookupByName(
		const USuperSLMModel& Model,
		const FString& SchemaName,
		FSuperSLMGpuSchemaHandle& OutHandle,
		FString& OutError);
};

