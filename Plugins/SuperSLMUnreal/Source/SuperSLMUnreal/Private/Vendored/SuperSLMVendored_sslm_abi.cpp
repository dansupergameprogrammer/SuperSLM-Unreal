// L2-S0 vendored-source wrapper (D-SLM3812). One translation unit per Layer-1 source file --
// NOT combined into a single wrapper -- because several Layer-1 .cpp files declare their own
// same-named helper types (e.g. S128/U128) in anonymous namespaces; each is private to its
// own translation unit under Layer 1's own CMake build (one .cpp == one TU there too), and
// combining multiple such files into one TU (an earlier draft of this wrapper did) collides
// those anonymous-namespace definitions as a hard redefinition error. One #include per file
// here preserves the same one-.cpp-one-TU shape UBT's automatic module-source discovery
// already gives every other file in this module.
// Plugin 1.5.0 (the clean stop's cell P5): development builds compile SuperSLM's single-shot CPU degenerate-row seam
// (ArmCpuFinishDegenerateLogitRowInjection), so a test can reach a -2 whose walk is not accepting. It does nothing
// until armed (FSuperSLMSchemaJumpTestAccess::ArmCpuDegenerateFinishRow); a shipping build does not compile it.
#if WITH_DEV_AUTOMATION_TESTS && !defined(SUPERSLM_CPU_G5_FINISH_ROW_FAULT_INJECTION)
#define SUPERSLM_CPU_G5_FINISH_ROW_FAULT_INJECTION 1
#endif
#include "../../../ThirdParty/SuperSLM/src/sslm_abi.cpp"
