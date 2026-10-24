#pragma once

// -run=SuperSLMBuildPrefixes (precache plan §5.8): builds cooked prefix sets unattended, as the
// asset's Build Prefixes action does, and saves each set's package.
//   -Sets=/Game/A.A,/Game/B.B   the sets to build (object paths, comma-separated), or
//   -All                        every USuperSLMPrefixSet the asset registry knows.
// Every finding is logged as a warning by entry name. Exit code 0 when every set built (findings
// included: an entry left empty is recomputed at runtime), 1 when any set was refused (no model, a
// model refused at map, no prefix save in this Layer 1) or could not be saved, 2 for bad arguments.

#include "Commandlets/Commandlet.h"
#include "SuperSLMBuildPrefixesCommandlet.generated.h"

UCLASS()
class USuperSLMBuildPrefixesCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	USuperSLMBuildPrefixesCommandlet();
	virtual int32 Main(const FString& Params) override;
};
