// L2-S2 vendored-source wrapper (plan §10.3 item 3, D-SLM7337). One translation unit per Layer-1
// GPU source, the same one-.cpp-one-TU rule the core wrappers follow (see
// SuperSLMVendored_decode_digest.cpp). Layer 1's GPU library is Windows-only (D3D12), so the
// include is guarded by SUPERSLMUNREAL_WITH_GPU, which SuperSLMUnreal.Build.cs sets to 1 on
// Win64 and 0 elsewhere. The Layer-1 source includes <windows.h> and <d3d12.h> itself; UE's
// platform-types guards keep those headers' macros from colliding with the engine's.
#if SUPERSLMUNREAL_WITH_GPU
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#pragma warning(push)
#pragma warning(disable : 4996) // Layer 1's d3d12_harness.h uses fopen; a vendored header this plugin does not edit
#include "../../../ThirdParty/SuperSLM/src/gpu/superslm_gpu.cpp"
#pragma warning(pop)
#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif
