// L2-S0 vendored-source wrapper (D-SLM3812). One translation unit per Layer-1 source file --
// NOT combined into a single wrapper -- because several Layer-1 .cpp files declare their own
// same-named helper types (e.g. S128/U128) in anonymous namespaces; each is private to its
// own translation unit under Layer 1's own CMake build (one .cpp == one TU there too), and
// combining multiple such files into one TU (an earlier draft of this wrapper did) collides
// those anonymous-namespace definitions as a hard redefinition error. One #include per file
// here preserves the same one-.cpp-one-TU shape UBT's automatic module-source discovery
// already gives every other file in this module.
#include "../../../ThirdParty/SuperSLM/src/damped_greedy_phaseD_loop.cpp"
