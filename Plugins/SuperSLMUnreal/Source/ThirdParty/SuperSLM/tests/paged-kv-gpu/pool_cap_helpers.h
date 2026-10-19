// The GPU page pool's cap, the tests' own arithmetic (§4 item 3), never read from the engine.
//
// The pool is one committed D3D12 buffer, so its cap is D3D12's required single-resource size for the
// adapter the test runs on, from d3d12.h's D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION terms
// (A = 128, B = 0.25, C = 2048): max(128 MiB, min(1/4 of the adapter's dedicated video memory,
// 2048 MiB)), and never above 2^32, the reach of the shaders' uint byte offsets. The RTX 2080 SUPER
// reports 8,374,976,512 bytes, so its cap is 2,093,744,128 bytes: 21,298 pages at Qwen2.5-0.5B
// (98,304-byte pages) and 9,128 at Qwen2.5-1.5B (229,376-byte pages). An adapter reporting 8 GiB or more
// (the RX 7900 XTX, 24 GiB) gets 2 GiB: 21,845 and 9,362.
//
// The adapter is the one the engine's device selects: SSLM_GPU_ADAPTER_INDEX's raw EnumAdapters1 index
// when it is set, else the first hardware adapter in high-performance order (d3d12_harness.h, Init()).
// A process that cannot read an adapter gets 0, which the cells report as a failed precondition.
#ifndef SUPERSLM_TESTS_PAGED_KV_GPU_POOL_CAP_HELPERS_H
#define SUPERSLM_TESTS_PAGED_KV_GPU_POOL_CAP_HELPERS_H

#include <dxgi1_6.h>
#include <wrl/client.h>
// rpcndr.h (through dxgi1_6.h) defines small as char; the cells use it as an identifier.
#ifdef small
#undef small
#endif

#include <cstdint>
#include <cstdlib>

namespace pool_cap {

inline uint64_t AdapterDedicatedVideoMemory() {
	Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
	const char* v = std::getenv("SSLM_GPU_ADAPTER_INDEX");
	if (v && *v) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
		if (FAILED(factory->EnumAdapters1(static_cast<UINT>(std::strtoul(v, nullptr, 10)), &a))) return 0;
		DXGI_ADAPTER_DESC1 d;
		return SUCCEEDED(a->GetDesc1(&d)) ? static_cast<uint64_t>(d.DedicatedVideoMemory) : 0;
	}
	for (UINT i = 0;; ++i) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
		if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) {
			return 0;
		}
		DXGI_ADAPTER_DESC1 d;
		if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
		return static_cast<uint64_t>(d.DedicatedVideoMemory);
	}
}

// The cap in bytes for this process's adapter (0 when no adapter could be read).
inline uint64_t TestPoolCapBytes() {
	static const uint64_t cap = [] {
		const uint64_t vram = AdapterDedicatedVideoMemory();
		if (vram == 0) return uint64_t{0};
		const uint64_t a = uint64_t{128} << 20, c = uint64_t{2048} << 20, shader = uint64_t{1} << 32;
		uint64_t bytes = vram / 4 < c ? vram / 4 : c;
		if (bytes < a) bytes = a;
		return bytes < shader ? bytes : shader;
	}();
	return cap;
}

}  // namespace pool_cap

#endif
