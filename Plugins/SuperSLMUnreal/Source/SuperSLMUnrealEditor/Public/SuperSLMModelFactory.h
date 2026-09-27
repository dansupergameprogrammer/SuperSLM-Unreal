#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "SuperSLMModelFactory.generated.h"

// T-2241 review C1: the import factory §10 L2-S0's gate names ("USuperSLMModel asset + import
// factory ..."). Registers `.sslm` as an editor-importable file type; imports into the
// destination package/name the Content Browser import flow provides, via
// FSuperSLMModelImport::ImportFromFile's InOuter/InName/InFlags parameters, so a `.sslm`
// dragged into the Content Browser becomes a real, saveable package asset rather than a
// transient object the caller has to relocate.

/** Imports a .sslm model file as a SuperSLM Model asset. */
UCLASS()
class USuperSLMModelFactory : public UFactory
{
	GENERATED_BODY()

public:
	USuperSLMModelFactory();

	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
};
