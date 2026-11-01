#pragma once

// -run=SuperSLMBuildPrefixes (precache plan §5.8): builds cooked prefix sets unattended, as the
// asset's Build Prefixes action does, and saves each set's package: the CPU entries, and the GPU
// entries of the GPU this machine's Layer 1 runs on (step U7), stored together.
//   -Sets=/Game/A.A,/Game/B.B   the sets to build (object paths, comma-separated), or
//   -All                        every USuperSLMPrefixSet the asset registry knows.
//   -NoGpu                      build the CPU entries only (the sets' GPU builds are kept as they were).
//   -RequireGpu                 exit 1 when a set's GPU entries were not built (no GPU, a GPU that does
//                               not configure, a GPU build refused); its CPU entries are still stored.
//   -ExpectGpuDevice=0x1002:0x744C   build the GPU entries only on that PCI vendor and device (hex);
//                               any other GPU leaves them unbuilt. Implies -RequireGpu.
// Every finding is logged as a warning by entry name. Exit code 0 when every set built (findings
// included: an entry left empty is recomputed at runtime), 1 when any set was refused (no model, a
// model refused at map, no prefix save in this Layer 1), could not be saved, or (-RequireGpu) has no
// GPU build from this run, 2 for bad arguments.

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
