// T-2226 -- L2-S0 red suite. §9 dim 2 (Trust boundaries) and §3 (the `.sslm` asset
// and import pipeline), the cells L2-S0 owns: USuperSLMModel import via sslm_model_map,
// with the specific section-index-and-message diagnostic D-SLM3812's C++-headers ruling
// affords rather than a bare ABI status (§10 L2-S0 gate: "an import failure reports its
// section index and message rather than a bare status").
//
// Every test targets FSuperSLMModelImport::ImportFromFile, the import entry point this
// suite specifies (the red-suite record, "Expected
// interface") and expects Public/SuperSLMModelImport.h and Public/SuperSLMModel.h from the
// "SuperSLMUnreal" runtime module -- neither exists in this tree. Every test in this file is
// therefore RED by construction: it cannot compile until the implementation supplies both headers,
// which is the whole of L2-S0's asset-import deliverable. Fixture provenance:
// Tests/Fixtures/SSLM/PROVENANCE.md.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

// Expected runtime-module surface (not yet built -- see file header).
#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"

using namespace SuperSLMImportFixtures;

namespace
{
	// Shared assertion: an import that must be REJECTED fails with the named status, and,
	// where the fixture's check names a row, the plugin's own diagnostic carries a real
	// section index and a non-empty message -- never a bare status string. This is the
	// oracle for the L2-S0 gate line "an import failure reports its section index and
	// message rather than a bare status," pinned per-fixture below.
	bool CheckRejected(FAutomationTestBase& T, const FFixtureCase& Case)
	{
		FSuperSLMImportDiagnostic Diag;
		USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(Case), Diag);

		T.TestNull(*FString::Printf(TEXT("%s: import must be rejected, not produce an asset"), *Case.FileName), Model);
		T.TestFalse(*FString::Printf(TEXT("%s: Diagnostic.bAccepted must be false"), *Case.FileName), Diag.bAccepted);
		T.TestEqual(*FString::Printf(TEXT("%s: StatusName"), *Case.FileName), Diag.StatusName, Case.ExpectedStatusName);

		// The message is never empty on a rejection: a diagnostic with an empty message is
		// indistinguishable from a bare status code re-typed into a string field, which is
		// exactly the defect D-SLM3812's C++-path ruling exists to close (§3).
		T.TestTrue(*FString::Printf(TEXT("%s: Diagnostic.Message must be non-empty"), *Case.FileName), !Diag.Message.IsEmpty());

		return Model == nullptr && !Diag.bAccepted;
	}
}

// IA-1: a structurally valid artifact (real synthetic model, not a placeholder -- see
// PROVENANCE.md) imports and produces a live asset. The one FEAT-oracle "achieves" test in
// this file: every other test in this dimension is a rejection cell, and a suite built only
// from rejection cells never proves the accept path exists at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportValidTest,
	"SuperSLM.L2S0.Import.ValidArtifactAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportValidTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(ValidReference()), Diag);

	TestNotNull(TEXT("valid artifact must import to a live asset"), Model);
	TestTrue(TEXT("Diagnostic.bAccepted"), Diag.bAccepted);
	return true;
}

// IA-2: bad magic -- BadMagic, before the hash check (artifact.cpp:239).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportBadMagicTest,
	"SuperSLM.L2S0.Import.BadMagicRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportBadMagicTest::RunTest(const FString& Parameters)
{
	return CheckRejected(*this, BadMagic());
}

// IA-3: truncated -- FileSizeMismatch, before the hash check (artifact.cpp:288-291).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportTruncatedTest,
	"SuperSLM.L2S0.Import.TruncatedRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportTruncatedTest::RunTest(const FString& Parameters)
{
	return CheckRejected(*this, Truncated());
}

// IA-4: one content byte flipped, header untouched -- IntegrityMismatch (artifact.cpp:305).
// This is the general "corrupt artifact fails in the editor, not at runtime" cell §3 states.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportHashMismatchTest,
	"SuperSLM.L2S0.Import.CorruptContentRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportHashMismatchTest::RunTest(const FString& Parameters)
{
	return CheckRejected(*this, HashMismatch());
}

// IA-5: the diagnostic names a SPECIFIC section, not a bare status. This is the direct pin
// of the L2-S0 gate item and of D-SLM3812's "first user-visible payment": through the C ABI
// alone every artifact-content failure collapses to one status (SSLM_ARTIFACT_REJECTED); the
// C++ path (SslmArtifact::OpenFromFile / SslmError) carries a section index and a message,
// and the ruling is that the editor surfaces THAT, not the collapsed ABI status.
// Mutation: an importer that maps every content rejection to the single string
// "SSLM_ARTIFACT_REJECTED" passes CheckRejected's StatusName/Message assertions (both are
// satisfiable by a constant string) but fails SectionIndex below -- SectionIndex is what a
// bare-status implementation cannot produce, because kNoSection carries no row information.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportDiagnosticNamesSectionTest,
	"SuperSLM.L2S0.Import.DiagnosticNamesSectionAndMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportDiagnosticNamesSectionTest::RunTest(const FString& Parameters)
{
	const FFixtureCase& Case = OverlappingSections();
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(Case), Diag);

	TestNull(TEXT("overlapping-sections artifact must be rejected"), Model);
	TestEqual(TEXT("StatusName"), Diag.StatusName, Case.ExpectedStatusName);
	TestTrue(TEXT("Diagnostic.SectionIndex must name one of the two colliding rows (4 or 5), "
		"never INDEX_NONE -- a bare-status diagnostic cannot populate this field"),
		Diag.SectionIndex == 4 || Diag.SectionIndex == 5);
	TestTrue(TEXT("Diagnostic.Message must be non-empty"), !Diag.Message.IsEmpty());
	return true;
}

// IA-6 (§9 dim 2, v1.2.0 new cell a): an artifact header flags bit outside
// kKnownArtifactFlagsMask is a BadHeader rejection, and the plugin's own diagnostic text
// says "this artifact requires a newer SuperSLM than the plugin pins" -- never "corrupt",
// because the two causes have different remedies (§3). The oracle checks BOTH the presence
// of the right phrase and the absence of the wrong one: a message containing neither word
// would pass a presence-only check vacuously, and a message containing both would pass an
// absence-only check vacuously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportUnknownFlagBitTest,
	"SuperSLM.L2S0.Import.UnknownFlagBitReportsNewerLayer1Required",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportUnknownFlagBitTest::RunTest(const FString& Parameters)
{
	const FFixtureCase& Case = UnknownFlagBit();
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(Case), Diag);

	TestNull(TEXT("unknown-flag-bit artifact must be rejected"), Model);
	TestEqual(TEXT("StatusName"), Diag.StatusName, Case.ExpectedStatusName);
	TestTrue(TEXT("Message must state this requires a newer SuperSLM than the plugin pins"),
		Diag.Message.Contains(TEXT("newer"), ESearchCase::IgnoreCase)
		&& Diag.Message.Contains(TEXT("SuperSLM"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Message must NOT say 'corrupt' -- the two causes have different remedies"),
		Diag.Message.Contains(TEXT("corrupt"), ESearchCase::IgnoreCase));
	return true;
}

// IA-7 (§9 dim 2, v1.2.0 new cell b): an artifact carrying the optional DGC1
// (DampedGreedyConstants) section imports and maps normally, even though this plugin ships
// greedy-only (§2.3 RULING D-SLM3826) and never reads the section's content. The cell exists
// because "an unused optional section is harmless" is a claim, not an observation --
// l2s0_valid_reference.sslm carries this section by construction (PROVENANCE.md), so this
// test is IA-1's own acceptance assertion, re-stated against the specific claim it is
// grounding, rather than a second fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMImportDgc1InertTest,
	"SuperSLM.L2S0.Import.OptionalDampedGreedySectionInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMImportDgc1InertTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(AbsolutePath(ValidReference()), Diag);

	TestNotNull(TEXT("an artifact carrying the optional DGC1 section must still import"), Model);
	TestTrue(TEXT("Diagnostic.bAccepted"), Diag.bAccepted);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
