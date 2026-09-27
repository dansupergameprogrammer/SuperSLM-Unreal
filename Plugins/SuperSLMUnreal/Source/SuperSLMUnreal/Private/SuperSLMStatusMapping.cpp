#include "SuperSLMStatusMapping.h"

#include "Misc/AssertionMacros.h"

// T-2241 review C2. Real, always-compiled translation unit -- see the header's own comment
// on VerifyEveryStatusHasDiagnosticText(). Exercises every real sslm_status enumerator plus
// the sentinel, so a Layer-1 pin bump that appends a status and a mapping arm both compile
// and pass this check at module startup, or the build fails at guard G5's own compile-time
// check first (the scoped #pragma in SuperSLMStatusMapping.h, around ToDiagnosticText's
// switch only -- not a module-wide Build.cs setting, which would also apply to Layer 1's
// own vendored switches this plugin may never edit).
namespace SuperSLMStatusMapping
{
	void VerifyEveryStatusHasDiagnosticText()
	{
#define SSLM_STATUS_MAPPING_VERIFY_(Name) check(ToDiagnosticText(Name) != nullptr);
		SSLM_STATUS_ENUM_LIST(SSLM_STATUS_MAPPING_VERIFY_)
#undef SSLM_STATUS_MAPPING_VERIFY_
		check(ToDiagnosticText(SSLM_STATUS_NEXT_FREE) != nullptr);
	}
}
