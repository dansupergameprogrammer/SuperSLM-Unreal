#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;

// L2-S1's own mechanism (the plan §9 R-S1f: "CPU adapter swap
// mid-token -> deferred to the token boundary"). USuperSLMAdapter -- the editor-facing asset
// validated against its base via sslm_adapter_map, per §3 -- is L2-S3 scope (R-S3a, the
// adapter's own achievement cell, is L2-S3 per §10.4). R-S1f needs only the SCHEDULING
// mechanism: that a swap requested mid-token is deferred rather than rejected or applied early.
// That does not need the asset wrapper to exist. See
// the red-suite record §6 for this scope split.
struct SUPERSLMUNREAL_API FSuperSLMAdapterHandle
{
	int64 Id = 0;
	bool IsValid() const { return Id != 0; }
};

// A class of static methods, matching FSuperSLMModelImport's own established shape
// (SuperSLMModelImport.h) rather than a bare namespace.
class SUPERSLMUNREAL_API FSuperSLMAdapterImport
{
public:
	// sslm_adapter_map against Base (§2.1, §3): SSLM_ADAPTER_MODEL_MISMATCH surfaces as a false
	// return with OutError naming the mismatch. This is the minimal import primitive R-S1f's
	// adapter-swap mechanism test needs; USuperSLMAdapter (L2-S3) vends one of these handles
	// itself once it exists, rather than this function being duplicated there.
	static bool ImportFromFile(
		const FString& AbsolutePath,
		const USuperSLMModel& Base,
		FSuperSLMAdapterHandle& OutHandle,
		FString& OutError);

	// sslm_adapter_release, then the adapter's hold on its base model's mapping. Refused
	// (false, OutError names the cause) while any sequence still has this adapter active
	// (SSLM_ADAPTER_HAS_LIVE_SEQUENCES), and while a held adapter request, a queued or in-flight
	// restore, or a worker job carrying the adapter still pins it (plan §2.5 row 20 rule 4); on
	// success Handle is reset to invalid. An adapter never released lives until module shutdown,
	// holding its base model's mapping with it.
	static bool Release(FSuperSLMAdapterHandle& Handle, FString& OutError);
};
