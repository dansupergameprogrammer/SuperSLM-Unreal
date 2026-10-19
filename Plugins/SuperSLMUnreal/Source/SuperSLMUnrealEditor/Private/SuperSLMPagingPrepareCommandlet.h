#pragma once

#include "Commandlets/Commandlet.h"
#include "SuperSLMPagingPrepareCommandlet.generated.h"

// Unattended preparation of real cooked-model inputs for the paging Shipping certification.
// Refuses existing destinations; ordinary import and the example's normal startup are unchanged.
UCLASS()
class USuperSLMPagingPrepareCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	USuperSLMPagingPrepareCommandlet();
	virtual int32 Main(const FString& Params) override;
};
