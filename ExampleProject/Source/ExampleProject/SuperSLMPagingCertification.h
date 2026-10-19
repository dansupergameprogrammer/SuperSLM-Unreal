#pragma once

#include "CoreMinimal.h"

class USuperSLMModel;
class USuperSLMSubsystem;
class USuperSLMGpuSubsystem;

// Opt-in unattended Shipping check. Generation advances on ordinary game frames; no dev seams.
class FSuperSLMPagingCertification
{
public:
	FSuperSLMPagingCertification(USuperSLMModel& Model, USuperSLMSubsystem& Cpu, USuperSLMGpuSubsystem* Gpu,
		bool bGpu, int32 ExpectedCap, const FString& AssetPath);
	~FSuperSLMPagingCertification();
	void Tick(float DeltaSeconds);
private:
	struct FState;
	TUniquePtr<FState> State;
};
