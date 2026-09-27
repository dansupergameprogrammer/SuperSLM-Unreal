#include "SuperSLMModelFactory.h"
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"
#include "SuperSLMImportDiagnostic.h"

#include "Logging/LogMacros.h"
#include "Misc/FeedbackContext.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperSLMImport, Log, All);

USuperSLMModelFactory::USuperSLMModelFactory()
{
	SupportedClass = USuperSLMModel::StaticClass();
	bCreateNew = false;
	bEditorImport = true;
	bText = false;

	Formats.Add(TEXT("sslm;SuperSLM Model"));
}

UObject* USuperSLMModelFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	FSuperSLMImportDiagnostic Diagnostic;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(Filename, InParent, InName, Diagnostic);

	if (Model != nullptr)
	{
		// The (Path, Outer, Name, OutDiagnostic) overload always sets RF_Public | RF_Standalone
		// (T-2241 review C1 -- matching SuperSLMSaveReloadTests.cpp's own proof cell, which
		// relies on exactly those flags to make the imported asset saveable). UFactory's own
		// Flags parameter may carry additional flags the import flow expects (e.g.
		// RF_Transactional for undo/redo support) -- OR them in rather than dropping them.
		Model->SetFlags(Flags);
	}

	if (Model == nullptr)
	{
		// §10 L2-S0 gate: "an import failure reports its section index and message rather
		// than a bare status" -- surfaced through the editor's own import-failure channel
		// (FFeedbackContext::Log, which the Content Browser's import flow reads to report the
		// failure to the user), not only through the diagnostic struct a caller may inspect.
		//
		// T-2227 fix round 5: this file previously ALSO called
		// UE_LOG(LogSuperSLMImport, Error, "Import of '%s' rejected: ...") immediately after
		// Warn->Log -- a second, differently-worded Error-severity message. Diagnosed at the
		// real log output (SuperSLM.L2S0.Factory.RejectionLogsSectionAndMessage): the
		// automation framework treats ANY Error-severity log line with no matching
		// AddExpectedError/AddExpectedMessage registration as an implicit, unexpected test
		// failure -- confirmed in the failing run's own log,
		// "LogAutomationController: Error: LogSuperSLMImport: Import of '...' rejected: ...
		// [log]", which is the framework's own report of exactly that second, unregistered
		// Error line, NOT a complaint about the (correctly-registered, correctly-matched)
		// Warn->Log call. The Warn->Log call alone already satisfies the gate line -- it is
		// what the editor's Content Browser import UI actually surfaces to a user, and it is
		// what the test's own AddExpectedError registers against. The UE_LOG call added a
		// second, redundant diagnostic with no consumer of its own; kept for log
		// searchability by category, but at Display verbosity so it can never trip
		// automation's implicit-error rule again.
		Warn->Log(ELogVerbosity::Error, FString::Printf(
			TEXT("SuperSLM import failed: %s (section %d): %s"),
			*Diagnostic.StatusName,
			Diagnostic.SectionIndex,
			*Diagnostic.Message));
		UE_LOG(LogSuperSLMImport, Display, TEXT("Import of '%s' rejected: %s (section %d): %s"),
			*Filename, *Diagnostic.StatusName, Diagnostic.SectionIndex, *Diagnostic.Message);
		bOutOperationCanceled = false;
		return nullptr;
	}

	bOutOperationCanceled = false;
	return Model;
}
