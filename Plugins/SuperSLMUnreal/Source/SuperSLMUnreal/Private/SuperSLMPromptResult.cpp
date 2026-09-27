#include "SuperSLMPromptResult.h"

#include "SuperSLMJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool IsJsonWhitespace(TCHAR C)
	{
		return C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\n') || C == TEXT('\r');
	}

	int32 HexValue(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) { return C - TEXT('0'); }
		if (C >= TEXT('a') && C <= TEXT('f')) { return 10 + (C - TEXT('a')); }
		if (C >= TEXT('A') && C <= TEXT('F')) { return 10 + (C - TEXT('A')); }
		return -1;
	}

	// One Unicode scalar value, in whatever encoding TCHAR is: UTF-16 (Windows) takes a pair for a
	// non-BMP value, UTF-32 takes it whole.
	void AppendCodePoint(FString& Out, uint32 CodePoint)
	{
		if constexpr (sizeof(TCHAR) == 2)
		{
			if (CodePoint >= 0x10000u)
			{
				const uint32 V = CodePoint - 0x10000u;
				Out.AppendChar(static_cast<TCHAR>(0xD800u + (V >> 10)));
				Out.AppendChar(static_cast<TCHAR>(0xDC00u + (V & 0x3FFu)));
				return;
			}
		}
		Out.AppendChar(static_cast<TCHAR>(CodePoint));
	}

	bool IsHighSurrogate(uint32 V) { return V >= 0xD800u && V <= 0xDBFFu; }
	bool IsLowSurrogate(uint32 V) { return V >= 0xDC00u && V <= 0xDFFFu; }
}

const TCHAR* SuperSLMPromptResult::PromptResultKey()
{
	return TEXT("Prompt_Result");
}

bool SuperSLMPromptResult::ComposeStopReason(const FStopFacts& Facts, ESuperSLMQueryStopReason& OutReason)
{
	const bool bStopped = Facts.Phase == ESuperSLMSequencePhase::Complete || Facts.Phase == ESuperSLMSequencePhase::Faulted;
	if (!bStopped)
	{
		return false;
	}
	if (Facts.bSchemaAccepting)
	{
		OutReason = ESuperSLMQueryStopReason::Completed;
		return true;
	}
	if (Facts.Phase == ESuperSLMSequencePhase::Faulted && Facts.LastOutcome == ESuperSLMDecodeOutcome::SchemaDeadEnd)
	{
		OutReason = ESuperSLMQueryStopReason::SchemaRejected;
		return true;
	}
	if (Facts.Phase == ESuperSLMSequencePhase::Complete ||
		(Facts.LastOutcome == ESuperSLMDecodeOutcome::Generating && Facts.bContextCapReached))
	{
		OutReason = ESuperSLMQueryStopReason::BudgetExhausted;
		return true;
	}
	return false;
}

bool SuperSLMPromptResult::ParseCompletedOutput(const FString& RawOutput, FString& OutValue, FString& OutError)
{
	OutValue.Reset();
	// SuperSLMJson::TryReadObject refuses output ending in a backslash before the reader runs
	// (UE 5.8's TJsonReader overruns on it; review R4-W1 moved the guard there for every site).
	TSharedPtr<FJsonObject> Object;
	FString ReadError;
	if (!SuperSLMJson::TryReadObject(RawOutput, Object, ReadError))
	{
		OutError = FString::Printf(TEXT("the output is not one JSON object (%s)"), *ReadError);
		return false;
	}
	if (Object->Values.Num() != 1)
	{
		OutError = FString::Printf(TEXT("the object has %d keys; %s expects exactly one"), Object->Values.Num(), PromptResultKey());
		return false;
	}
	for (const auto& Pair : Object->Values)
	{
		// FJsonObject's key type is UE::FSharedString at 5.8 (FString under
		// UE_JSONOBJECT_LEGACY_STRING_KEYS); operator* reads either as TCHARs.
		const FString Key(*Pair.Key);
		if (!Key.Equals(PromptResultKey(), ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(TEXT("the one key is '%s', not '%s'"), *Key, PromptResultKey());
			return false;
		}
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("'%s' does not hold a string"), PromptResultKey());
			return false;
		}
		OutValue = Pair.Value->AsString();
		// Review N2: UE's reader appends \u0000 as a NUL character; the answer shows it as U+FFFD,
		// matching RecoverTruncatedOutput(). (Unpaired surrogate escapes still differ: the reader
		// keeps the raw UTF-16 unit, while recovery drops it as T-2853 design §5 specifies -- a
		// design-level divergence recorded for the plan, not changed here.)
		for (int32 I = 0; I < OutValue.Len(); ++I) // Len() counts embedded NULs, not the terminator
		{
			if (OutValue[I] == TCHAR(0))
			{
				OutValue[I] = TCHAR(0xFFFD);
			}
		}
	}
	return true;
}

int32 SuperSLMPromptResult::FindPromptResultContentStart(const FString& RawOutput)
{
	const FString QuotedKey = FString::Printf(TEXT("\"%s\""), PromptResultKey());
	int32 I = 0;
	auto SkipWhitespace = [&RawOutput, &I]()
	{
		while (I < RawOutput.Len() && IsJsonWhitespace(RawOutput[I]))
		{
			++I;
		}
	};
	auto Expect = [&RawOutput, &I](const FString& Literal)
	{
		if (RawOutput.Len() - I < Literal.Len() ||
			FCString::Strncmp(*RawOutput + I, *Literal, Literal.Len()) != 0)
		{
			return false;
		}
		I += Literal.Len();
		return true;
	};
	SkipWhitespace();
	if (!Expect(TEXT("{"))) { return INDEX_NONE; }
	SkipWhitespace();
	if (!Expect(QuotedKey)) { return INDEX_NONE; }
	SkipWhitespace();
	if (!Expect(TEXT(":"))) { return INDEX_NONE; }
	SkipWhitespace();
	if (!Expect(TEXT("\""))) { return INDEX_NONE; }
	return I;
}

bool SuperSLMPromptResult::RecoverTruncatedOutput(const FString& RawOutput, FString& OutText)
{
	OutText.Reset();
	const int32 Start = FindPromptResultContentStart(RawOutput);
	if (Start == INDEX_NONE)
	{
		return false;
	}

	// T-2853 design §5's scan. Only a fully-formed character is ever appended, so a cut in any
	// non-content state (after a backslash, mid-hex, awaiting a low surrogate) leaves OutText at
	// the last fully-formed character with nothing to undo.
	enum class EScan : uint8 { Content, AfterBackslash, Hex };
	EScan Scan = EScan::Content;
	int32 HexDigits = 0;
	uint32 HexUnit = 0;
	uint32 PendingHigh = 0; // a completed \uD800-\uDBFF escape awaiting its low surrogate; 0 = none

	for (int32 I = Start; I < RawOutput.Len(); ++I)
	{
		const TCHAR C = RawOutput[I];
		switch (Scan)
		{
			case EScan::Content:
				if (C == TEXT('"'))
				{
					// The closing quote: the value is whole. A high surrogate still awaiting its
					// pair is unpaired, and dropped.
					return true;
				}
				if (C == TEXT('\\'))
				{
					Scan = EScan::AfterBackslash;
					break;
				}
				PendingHigh = 0; // an ordinary character settles an awaiting high surrogate as unpaired
				OutText.AppendChar(C);
				break;

			case EScan::AfterBackslash:
			{
				TCHAR Unescaped = 0;
				switch (C)
				{
					case TEXT('"'): Unescaped = TEXT('"'); break;
					case TEXT('\\'): Unescaped = TEXT('\\'); break;
					case TEXT('/'): Unescaped = TEXT('/'); break;
					case TEXT('b'): Unescaped = TEXT('\b'); break;
					case TEXT('f'): Unescaped = TEXT('\f'); break;
					case TEXT('n'): Unescaped = TEXT('\n'); break;
					case TEXT('r'): Unescaped = TEXT('\r'); break;
					case TEXT('t'): Unescaped = TEXT('\t'); break;
					case TEXT('u'):
						Scan = EScan::Hex;
						HexDigits = 0;
						HexUnit = 0;
						break;
					default:
						// Outside the string leaf's escape grammar, which the schema's mask never
						// admits; stop at the last fully-formed character rather than guess.
						return true;
				}
				if (Scan == EScan::AfterBackslash)
				{
					PendingHigh = 0; // a short escape settles an awaiting high surrogate as unpaired
					OutText.AppendChar(Unescaped);
					Scan = EScan::Content;
				}
				break;
			}

			case EScan::Hex:
			{
				const int32 Digit = HexValue(C);
				if (Digit < 0)
				{
					return true; // as above: not the grammar's own shape
				}
				HexUnit = (HexUnit << 4) | static_cast<uint32>(Digit);
				if (++HexDigits < 4)
				{
					break;
				}
				Scan = EScan::Content;
				if (IsHighSurrogate(HexUnit))
				{
					// A second high surrogate replaces an unpaired first one, which is dropped.
					PendingHigh = HexUnit;
				}
				else if (IsLowSurrogate(HexUnit))
				{
					if (PendingHigh != 0)
					{
						AppendCodePoint(OutText, 0x10000u + ((PendingHigh - 0xD800u) << 10) + (HexUnit - 0xDC00u));
					}
					// else: a lone low surrogate, dropped by the same rule.
					PendingHigh = 0;
				}
				else
				{
					PendingHigh = 0;
					// Review N2: \u0000 would put a NUL inside the FString, which every C-string
					// consumer then truncates at; it shows as U+FFFD, as ParseCompletedOutput() shows it.
					AppendCodePoint(OutText, HexUnit == 0 ? 0xFFFDu : HexUnit);
				}
				break;
			}
		}
	}
	// Cut before the closing quote: whatever state the scan is in, OutText already stops at the
	// last fully-formed character (a trailing partial escape or an awaiting high surrogate is
	// simply never appended).
	return true;
}
