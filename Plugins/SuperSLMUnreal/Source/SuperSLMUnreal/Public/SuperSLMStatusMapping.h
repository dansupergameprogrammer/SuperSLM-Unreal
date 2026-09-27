#pragma once

// L2-S0 (the plan §9 dim 11, guard G5; D-SLM3812).
//
// "The plugin's status mapping is a switch with no `default` arm, compiled with the warning
// that flags a non-exhaustive switch promoted to an error ... Mutation: remove one arm -- the
// build must fail. A `default:` arm would make this guard permanently vacuous, which is why
// its absence is the cell's subject and not an implementation detail" (§9 dim 11, G5).
//
// The switch below is built from Layer 1's own SSLM_STATUS_ENUM_LIST X-macro
// (include/superslm/sslm_abi.h) rather than a hand-transcribed case list: every enumerator
// this header's switch handles is generated from the exact same list `sslm_status` itself is
// generated from, so the two can never drift, and a Layer-1 pin bump that appends a status
// (the ABI's own stated additive-only discipline) grows this switch automatically -- the
// build fails to compile only if the switch is missing an arm relative to the enum it is
// switching over, which is exactly G5's guard.
//
// This header is build-time-only surface: it depends on sslm_abi.h (Layer 1's C ABI header,
// vendored at ../ThirdParty/SuperSLM/include), not on any UE type, so it is usable from a
// bare host compiler: ci/tests/test_guard_g5_status_switch_exhaustive.py compiles this header,
// and scratch copies of it with one switch arm removed, from a small generated probe file.

#include "superslm/sslm_abi.h"

namespace SuperSLMStatusMapping
{
	// T-2241 review C2: the unhandled-enumerator warning is promoted to an error with a
	// PRAGMA SCOPED TO THIS SWITCH ONLY -- not a module-wide UBT setting. A module-wide
	// SwitchUnhandledEnumeratorWarningLevel = Error was tried first (SuperSLMUnreal.Build.cs)
	// and broke the build: this module also compiles Layer 1's own vendored sources
	// (Private/Vendored/*.cpp), which switch over other enums (SslmSectionType, etc.) with
	// their own legitimate non-exhaustive shapes this plugin may never edit (D-SLM3812). The
	// pragma keeps the promotion to exactly the one switch guard G5 names, in the one file
	// that owns it, regardless of what else this module compiles.
	//
	// T-2249 confirmation review N1: the number was WRONG -- this switch has no `default`
	// label, which is MSVC C4062 ("switch of enum ... not handled and there is no default
	// label"), not C4061 (which fires only when a `default` DOES exist and an enumerator is
	// still unhandled). C4061 never fires on a default-less switch, so the guard could not
	// compile-fail under the module's own flags -- confirmed by the review's own four-row
	// compile matrix (real header / module flags -> exit 0; MUTATED header / module flags AS
	// SHIPPED -> exit 0, the guard silent; mutated header / module flags + /we4062 -> C4062,
	// exit 2; mutated header / pragma corrected to 4062 -> C4062, exit 2). Promoting BOTH
	// numbers below (the review's own suggestion) is harmless and forward-safe: 4062 is the
	// live diagnostic today, and 4061 becomes the live one the day a `default:` arm is ever
	// added to this switch.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(error : 4062) // switch of enum with NO default label, an enumerator unhandled -- this switch's actual shape
#pragma warning(error : 4061) // (also promoted) enumerator unhandled in a switch that DOES have a default -- not this switch's shape today, kept for the day a default arm is added
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif

	// Product-facing text for one sslm_status value. Every arm below returns a literal;
	// there is no default arm, so an enumerator this switch does not handle fails to compile
	// under the pragma above rather than falling through to a generic string at runtime.
	inline const char* ToDiagnosticText(sslm_status Status)
	{
		switch (Status)
		{
#define SSLM_STATUS_MAPPING_ARM_(Name) case Name: return #Name;
			SSLM_STATUS_ENUM_LIST(SSLM_STATUS_MAPPING_ARM_)
#undef SSLM_STATUS_MAPPING_ARM_

			// Not a real status (sslm_abi.h: "NOT a real status; never returned by any verb,
			// never a valid argument"); handled explicitly, same as every real enumerator
			// above, so this switch stays exhaustive over the FULL sslm_status value set
			// rather than relying on a default arm to cover the sentinel.
			case SSLM_STATUS_NEXT_FREE:
				return "SSLM_STATUS_NEXT_FREE";
		}
		// Unreachable if the switch above is genuinely exhaustive over sslm_status -- every
		// real enumerator plus the sentinel has its own arm above. This trailing return exists
		// only so a compiler that cannot itself prove switch-exhaustiveness for a missing-
		// return warning (MSVC's C4715, independent of /we4062's own switch check) does not
		// fail the build on that unrelated diagnostic; it is code AFTER the switch, not a
		// `default:` label inside it, so it does not weaken G5's own guard -- removing one of
		// the case arms above still fails to compile under -Wswitch -Werror / /we4062,
		// because that diagnostic is raised by the switch itself, not by reachability of the
		// code following it.
		return "SSLM_STATUS_UNKNOWN";
	}

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

	// T-2241 review C2: this header was included by no translation unit in the module
	// (confirmed at review time by an Intermediate/ search finding it in none), so the switch
	// above was never actually compiled by the plugin's own build and no warning setting
	// could ever have fired on it. VerifyEveryStatusHasDiagnosticText() is defined in
	// SuperSLMStatusMapping.cpp -- a real, always-compiled translation unit of this module.
	// T-2249 confirmation review N8: what makes this switch part of the module's real compile
	// line is SuperSLMStatusMapping.cpp's own `#include "SuperSLMStatusMapping.h"` -- the
	// preprocessor inclusion, which is what causes the compiler to parse and typecheck the
	// switch at all. The CALL to this function from FSuperSLMUnrealModule::StartupModule()
	// keeps the function odr-used (so nothing strips it as dead code) and, via `check()`
	// inside it, adds a RUNTIME cross-check that every real enumerator maps to non-null text
	// -- a belt-and-suspenders measure, not what makes the switch compile. The compile-time
	// guard (the pragma above) does not depend on this function ever being called, and does
	// not depend on `check()`, which DO_CHECK=0 strips in Shipping (D-SLM3841 forbids a
	// Shipping-stripped implementation for G1-G5) -- the guard survives stripping because it
	// is a compile-time diagnostic, not because of anything this function's body does.
	void VerifyEveryStatusHasDiagnosticText();
}
