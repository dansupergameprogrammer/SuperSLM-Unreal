// T-2226 -- L2-S0 red suite. Fix round routed by confirmation code review T-2249
// (the confirmation review, finding N4, closing
// the provenance quarter of N4 and pinning finding N2).
//
// N2, as found: `ExtractProvenanceJson` (SuperSLMModelImport.cpp:54-63) builds an FString from
// `FUTF8ToTCHAR(Section->data, Section->byte_size).Get()` -- a conversion buffer the engine's
// own documentation (StringConv.h:774) states "may not be null-terminated if constructed from
// an explicitly sized buffer that didn't include the null-terminator" -- and hands that
// possibly-non-terminated pointer to `FString(const WIDECHAR*)`, the Strlen-based constructor.
// The correct API is the adjacent range constructor (`FString(FUTF8ToTCHAR(...))`, or
// `ConstructFromPtrSize`), which never calls Strlen. Mid-file this silently appends whatever
// bytes follow the Provenance section until an accidental zero; as the LAST section it reads
// past the end of the `FMemory::Malloc` allocation entirely.
//
// N4, as found: "the entire §3 provenance deliverable -- code and defect together -- is
// unexercised" -- no fixture in the suite carried a type-1 (Provenance) section at all
// (`l2s0_valid_reference.sslm`'s 11 sections are types 0,2,3,4,6,7,8,9,12,42,30; there is no
// type 1). This file is that missing coverage.
//
// Five fixtures (Fixtures/SSLM/l2s0_provenance_*.sslm, built by gen_provenance_fixtures.py
// directly from docs/sslm_format.md's byte layout -- type 1, dtype 0/Raw = UTF-8 bytes with no
// internal sub-format, so `Section->byte_size` is the ONLY length signal, which is exactly the
// field the current code discards):
//   - normal: a realistic provenance JSON blob, mid-file (not last).
//   - max_no_nul: fills its own declared byte_size with 256 non-NUL printable bytes, and is
//     the LAST section in the file -- N2's sharpest reproduction (overread runs off the real
//     allocation's end).
//   - embedded_nul: one NUL byte partway through a 55-byte blob, mid-file -- isolates the
//     TRUNCATION failure mode (Strlen stops early) from the out-of-bounds failure mode.
//   - zero_length: byte_size == 0 -- already correctly handled by the existing
//     `byte_size > 0` guard; a must-accept boundary case, not a bug reproduction.
//   - oversize: a 16 KiB non-NUL blob, LAST section -- the widest-reach stress case.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperSLMImportFixtures.h"

#include "SuperSLMModel.h"
#include "SuperSLMModelImport.h"

using namespace SuperSLMImportFixtures;

namespace
{
	// Same deterministic pattern gen_provenance_fixtures.py's Python uses for the
	// max_no_nul/oversize fixtures' content: `(i % 94) + 33` -- printable ASCII 33..126,
	// never 0x00. Reconstructed here rather than pasted as a literal so a 256- or
	// 16384-character expected string is never hand-typed (and cannot silently drift from
	// the fixture generator, since both read the same formula, stated once each).
	FString ExpectedPatternContent(int32 Length)
	{
		FString Result;
		Result.Reserve(Length);
		for (int32 i = 0; i < Length; ++i)
		{
			Result.AppendChar(static_cast<TCHAR>((i % 94) + 33));
		}
		return Result;
	}

	// T-2226 fix round (routed from the build, the build record
	// §13): FString's own equality/Compare path (used by TestEqual(FString, FString) below,
	// and by FStringView::Compare -> Strnicmp under it) is a C-string-style comparison that
	// stops at the first embedded NUL -- exactly the byte this cell exists to look past. Two
	// strings that differ only AFTER an embedded NUL therefore compared EQUAL under the old
	// oracle, so PV-3 could not see the bytes it was built to check, even though the real
	// ProvenanceJson is byte-correct (confirmed via a hex dump). This helper compares
	// length and then every raw TCHAR byte via FMemory::Memcmp, which has no NUL-stops-the-
	// scan behaviour at all.
	bool ExactTCharEqual(const FString& A, const FString& B)
	{
		if (A.Len() != B.Len())
		{
			return false;
		}
		if (A.Len() == 0)
		{
			return true;
		}
		return FMemory::Memcmp(*A, *B, A.Len() * sizeof(TCHAR)) == 0;
	}
}

// PV-1: a realistic provenance blob, mid-file -- the FEAT oracle for "the asset carries the
// artifact's embedded provenance metadata" (§3). Exact-string match, not merely non-empty:
// an implementation that reads SOME bytes but the wrong range would still pass a
// non-empty/length-only check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceNormalTest,
	"SuperSLM.L2S0.Provenance.NormalExtractsExactly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceNormalTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(FFixtureCase{TEXT("l2s0_provenance_normal.sslm"), TEXT(""), INDEX_NONE}), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}
	TestEqual(TEXT("ProvenanceJson must match the section's exact content"),
		Model->ProvenanceJson,
		TEXT("{\"checkpoint\":\"shopkeeper-ref-1.5b\",\"license\":\"Apache-2.0\","
			"\"source_hash\":\"9f2b7c4a1e6d3f80\"}"));
	return true;
}

// PV-2: N2's own sharpest reproduction. 256 non-NUL bytes, the LAST section in the file, so a
// Strlen-style overread runs off the real FMemory::Malloc allocation rather than merely into
// another section's bytes. Correct behaviour: exactly 256 characters, matching the
// deterministic pattern -- not "at least 256" (a truncation bug caught by a length-only
// upper-bound check is not what this cell exists to catch; an OVERREAD bug is length > 256,
// truncation is length < 256, and this assertion catches both by requiring exact equality).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceMaxNoNulTest,
	"SuperSLM.L2S0.Provenance.MaxLengthNoNulDoesNotOverread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceMaxNoNulTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(FFixtureCase{TEXT("l2s0_provenance_max_no_nul.sslm"), TEXT(""), INDEX_NONE}), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}
	TestEqual(TEXT("ProvenanceJson length must be exactly the section's byte_size (256) -- "
		"longer means an overread past the allocation, shorter means a spurious early stop"),
		Model->ProvenanceJson.Len(), 256);
	TestEqual(TEXT("ProvenanceJson content must match the deterministic pattern exactly"),
		Model->ProvenanceJson, ExpectedPatternContent(256));
	return true;
}

// PV-3: isolates the TRUNCATION failure mode. A NUL byte sits at index 21 of a 55-byte
// section, mid-file (not last -- deliberately, so any content past 55 bytes correct or
// otherwise is attributable to truncation/overread specifically, not conflated with PV-2's
// end-of-allocation case). The current buggy code's FString(const WIDECHAR*) constructor
// stops at the embedded NUL (length 21); the correct fix preserves the full 55 characters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceEmbeddedNulTest,
	"SuperSLM.L2S0.Provenance.EmbeddedNulDoesNotTruncate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceEmbeddedNulTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(FFixtureCase{TEXT("l2s0_provenance_embedded_nul.sslm"), TEXT(""), INDEX_NONE}), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}
	TestEqual(TEXT("ProvenanceJson length must be the full section byte_size (55), not "
		"truncated at the embedded NUL (which would yield 21)"),
		Model->ProvenanceJson.Len(), 55);
	// Build the expected string directly (FString literals cannot embed \0 and keep parsing
	// the rest of the literal), so the assertion exercises the exact same embedded-NUL shape
	// the fixture carries. NOT via FString::AppendChar in a loop -- routed from the build
	// (§15): AppendChar(TCHAR(0)) is a documented no-op (String.cpp.inl:399), so a 55-byte
	// loop that calls it once (at index 21, the embedded NUL) silently produces a 54-character
	// string, not 55 -- this test's own expected value was wrong, not the plugin, and it was
	// failing on that self-inflicted defect while the real 55-char ProvenanceJson was correct.
	// FString::ConstructFromPtrSize copies InLen TCHARs verbatim with no per-character
	// special-casing, which AppendChar's public API does not guarantee.
	const ANSICHAR Expected[] = "{\"checkpoint\":\"trunc\0ated-if-buggy\",\"source_hash\":\"aa\"}";
	TCHAR ExpectedBuffer[55];
	for (int32 i = 0; i < 55; ++i)
	{
		ExpectedBuffer[i] = static_cast<TCHAR>(Expected[i]);
	}
	FString ExpectedStr = FString::ConstructFromPtrSize(ExpectedBuffer, 55);
	TestEqual(TEXT("the expected-value buffer itself must be 55 characters -- guards against "
		"this test's own construction regressing back to AppendChar's silent NUL no-op"),
		ExpectedStr.Len(), 55);
	TestTrue(TEXT("ProvenanceJson must match byte-for-byte, embedded NUL included -- compared "
		"via FMemory::Memcmp over Len()*sizeof(TCHAR), never FString's own operator== (which "
		"routes to a Strnicmp-family, NUL-stops-the-scan comparison that cannot see bytes "
		"past an embedded NUL -- the exact defect that let this cell pass on a truncated "
		"value at the prior round)"),
		ExactTCharEqual(Model->ProvenanceJson, ExpectedStr));
	return true;
}

// PV-3b: must-reject for PV-3's own oracle, ExactTCharEqual (a check is trusted only after it
// has been shown to reject a known-bad input).
// Proves the byte-exact comparison actually discriminates -- a production value that
// truncates at the embedded NUL (reproducing the OLD FString(const WIDECHAR*)/Strlen bug
// PV-3 exists to catch) must compare UNEQUAL to the full 55-character expected value under
// this oracle. The prior oracle (FString::operator==/TestEqual) could NOT prove this: it
// reported the truncated 21-character value and the full 55-character value as equal,
// because its own comparison also stops at the NUL -- which is exactly why PV-3 passed on a
// production defect the implementation found via C1's own hex dump. This test targets the ORACLE,
// not the plugin: it is red if and only if ExactTCharEqual regresses back to a NUL-stopping
// comparison.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceEmbeddedNulOracleMustRejectTruncationTest,
	"SuperSLM.L2S0.Provenance.EmbeddedNulOracleRejectsTruncatedCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceEmbeddedNulOracleMustRejectTruncationTest::RunTest(const FString& Parameters)
{
	const ANSICHAR Expected[] = "{\"checkpoint\":\"trunc\0ated-if-buggy\",\"source_hash\":\"aa\"}";

	// Built via FString::ConstructFromPtrSize, not FString::AppendChar in a loop -- routed
	// from the implementation (§15): AppendChar(TCHAR(0)) is a documented no-op (String.cpp.inl:399),
	// so a 55-iteration AppendChar loop over this SAME Expected buffer silently drops the
	// embedded NUL at index 21 and yields a 54-character string, not 55. Every value this test
	// treats as "the full byte-correct value" is built the same way PV-3's own fix now is.
	auto BuildFull = [&Expected]() -> FString
	{
		TCHAR Buffer[55];
		for (int32 i = 0; i < 55; ++i)
		{
			Buffer[i] = static_cast<TCHAR>(Expected[i]);
		}
		return FString::ConstructFromPtrSize(Buffer, 55);
	};
	FString FullValue = BuildFull();
	check(FullValue.Len() == 55);

	// A production copy that truncates at the embedded NUL -- reproducing the OLD
	// FString(const WIDECHAR*) constructor's Strlen-based behaviour, never the fixture's own
	// data. This is the "must-reject" construction: a value ExactTCharEqual must NOT accept
	// as equal to FullValue.
	FString TruncatedValue;
	for (int32 i = 0; i < 21; ++i) // 21 == strlen up to (but not including) the embedded NUL
	{
		TruncatedValue.AppendChar(static_cast<TCHAR>(Expected[i]));
	}

	TestFalse(TEXT("ExactTCharEqual must report a truncated-at-NUL copy as UNEQUAL to the "
		"full byte-correct value -- this is the must-reject proof that the byte-exact oracle "
		"can actually fail (a check that cannot fail proves nothing); a comparison that reports these "
		"two strings equal is the exact defect PV-3 was silently passing under before this "
		"fix"),
		ExactTCharEqual(TruncatedValue, FullValue));

	// Corroborating must-accept, in the same test: the oracle must still report two
	// genuinely identical byte-exact copies as equal.
	FString FullValueCopy = BuildFull();
	TestTrue(TEXT("ExactTCharEqual must report two genuinely identical embedded-NUL strings "
		"as equal"),
		ExactTCharEqual(FullValue, FullValueCopy));

	// Second must-reject, routed from the build (§15): a 54-character value built the OLD,
	// buggy way (an AppendChar loop over the same 55-byte Expected buffer, which silently
	// drops the embedded NUL at index 21) must NOT compare equal to the genuine 55-character
	// value -- proving this test file's own PRIOR construction bug (not a plugin defect) is
	// itself now caught rather than silently repeating.
	FString BuggyAppendCharExpectedStr;
	for (int32 i = 0; i < 55; ++i)
	{
		BuggyAppendCharExpectedStr.AppendChar(static_cast<TCHAR>(Expected[i]));
	}
	TestEqual(TEXT("AppendChar(TCHAR(0)) must still be the documented no-op this must-reject "
		"relies on -- if this ever fails, String.cpp.inl's own behaviour changed and this "
		"must-reject needs re-deriving, not the plugin"),
		BuggyAppendCharExpectedStr.Len(), 54);
	TestFalse(TEXT("ExactTCharEqual must report a 54-character AppendChar-built expectation "
		"(the exact construction bug that made PV-3 fail against a byte-correct 55-character "
		"production value) as UNEQUAL to the genuine 55-character value"),
		ExactTCharEqual(BuggyAppendCharExpectedStr, FullValue));

	return true;
}

// PV-4: byte_size == 0 -- a must-accept boundary case (already correctly handled by the
// existing `byte_size > 0` guard), included so the family covers both numeric extremes
// deliberately rather than only the failing ones (this suite's own enumeration
// discipline, the red-suite record §9's own convention).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceZeroLengthTest,
	"SuperSLM.L2S0.Provenance.ZeroLengthIsEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceZeroLengthTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(FFixtureCase{TEXT("l2s0_provenance_zero_length.sslm"), TEXT(""), INDEX_NONE}), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}
	TestTrue(TEXT("ProvenanceJson must be empty for a zero-length Provenance section"),
		Model->ProvenanceJson.IsEmpty());
	return true;
}

// PV-5: the widest-reach stress case -- 16 KiB, LAST section. Same oracle shape as PV-2 at a
// much larger scale, so an overread or truncation bug that happens to land on a
// coincidentally-matching byte at the 256-byte boundary (PV-2) has nowhere to hide at 16 KiB.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperSLMProvenanceOversizeTest,
	"SuperSLM.L2S0.Provenance.OversizeDoesNotOverreadOrTruncate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperSLMProvenanceOversizeTest::RunTest(const FString& Parameters)
{
	FSuperSLMImportDiagnostic Diag;
	USuperSLMModel* Model = FSuperSLMModelImport::ImportFromFile(
		AbsolutePath(FFixtureCase{TEXT("l2s0_provenance_oversize.sslm"), TEXT(""), INDEX_NONE}), Diag);
	if (!TestNotNull(TEXT("import must succeed"), Model))
	{
		return false;
	}
	const int32 ExpectedLen = 16 * 1024;
	TestEqual(TEXT("ProvenanceJson length must be exactly the section's byte_size (16384)"),
		Model->ProvenanceJson.Len(), ExpectedLen);
	TestEqual(TEXT("ProvenanceJson content must match the deterministic pattern exactly"),
		Model->ProvenanceJson, ExpectedPatternContent(ExpectedLen));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
