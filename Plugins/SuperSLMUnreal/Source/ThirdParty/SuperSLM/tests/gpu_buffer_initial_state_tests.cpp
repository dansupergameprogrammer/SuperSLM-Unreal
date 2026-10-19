// Real D3D12 creation, two completed copy/readback submissions, and queue observer vitality.
// The initial-state cases are the production allocator's actual default/upload/readback heaps.
#include "d3d12_harness.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace superslm_gpu::harness;

#if defined(SUPERSLM_TEST_D3D12_SDK_VERSION)
// Optional app-local Agility runtime, staged by the caller under D3D12/. Default builds
// exercise the OS runtime. This selects the same runtime as a consuming application.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = SUPERSLM_TEST_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

int main() {
	const char* requested = std::getenv("SSLM_GPU_ENABLE_DEBUG_LAYER");
	if (!requested || std::strcmp(requested, "1") != 0) {
		std::fprintf(stderr, "FAIL: explicitly enable the real debug layer\n");
		return 2;
	}
	Device device;
	device.Init();
	if (!device.available.load() || !device.debug_layer_enabled || !device.debug_info_queue) {
		std::fprintf(stderr, "FAIL: actual debug device/InfoQueue required: %s\n", device.init_error.c_str());
		return 2;
	}
	if (const HMODULE runtime = GetModuleHandleW(L"D3D12Core.dll")) {
		wchar_t path[32768] = {};
		GetModuleFileNameW(runtime, path, static_cast<DWORD>(std::size(path)));
		const auto version = reinterpret_cast<const UINT*>(GetProcAddress(runtime, "D3D12SDKVersion"));
		std::fprintf(stderr, "D3D12 runtime: SDK=%u path=%ls\n", version ? *version : 0, path);
	}
	bool ok = true;
	auto check = [&ok](bool condition, const char* message) {
		std::fprintf(stderr, "%s: %s\n", condition ? "PASS" : "FAIL", message);
		ok = condition && ok;
	};
	try {
		std::vector<ComPtr<ID3D12Resource>> buffers;
		for (const D3D12_RESOURCE_STATES intended : {D3D12_RESOURCE_STATE_COMMON,
		     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST,
		     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}) {
			buffers.push_back(device.MakeBuffer(256, D3D12_HEAP_TYPE_DEFAULT,
			    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, intended));
		}
		buffers.push_back(device.MakeBuffer(256, D3D12_HEAP_TYPE_UPLOAD,
		    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
		buffers.push_back(device.MakeBuffer(256, D3D12_HEAP_TYPE_READBACK,
		    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST));
		check(buffers.size() == 6, "all four default-state requests and both CPU-visible heaps allocate");
		check(device.debug_info_queue->GetNumMessagesDiscardedByMessageCountLimit() == 0, "creation queue has dropped no messages");
		check(device.DrainDebugLayerMessages() == 0, "creation produces zero warnings/errors/corruption");

		std::array<uint32_t, 64> input{};
		auto target = device.MakeBuffer(sizeof(input), D3D12_HEAP_TYPE_DEFAULT,
		    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		auto readback = device.MakeBuffer(sizeof(input), D3D12_HEAP_TYPE_READBACK,
		    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
		for (uint32_t round = 0; round < 2; ++round) {
			for (uint32_t i = 0; i < input.size(); ++i) { input[i] = (i * 0x9e3779b9u) ^ (round ? 0xdeadbeefu : 0x12345678u); }
			auto upload = device.Upload(input.data(), sizeof(input));
			device.BeginRecording();
			RecordTransition(device.list.Get(), target.Get(), round ? D3D12_RESOURCE_STATE_COPY_SOURCE
			    : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
			RecordCopyBufferRegion(device.list.Get(), target.Get(), 0, upload.Get(), 0, sizeof(input));
			RecordTransition(device.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
			RecordCopyBufferRegion(device.list.Get(), readback.Get(), 0, target.Get(), 0, sizeof(input));
			device.CloseListOrThrow();
			device.Submit().Wait();
			void* mapped = nullptr;
			D3D12_RANGE read{0, sizeof(input)};
			SSLM_GPU_HR(readback->Map(0, &read, &mapped));
			check(std::memcmp(mapped, input.data(), sizeof(input)) == 0, "actual GPU copy/readback matches every input byte");
			D3D12_RANGE none{0, 0};
			readback->Unmap(0, &none);
		}
		check(device.DrainDebugLayerMessages() == 0, "both completed submissions produce zero validation messages");
		check(device.debug_info_queue->GetNumMessagesDiscardedByMessageCountLimit() == 0, "submission queue has dropped no messages");
		check(SUCCEEDED(device.debug_info_queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING,
		    "SuperSLM buffer-state observer warning canary")), "real queue accepts warning canary");
		check(device.DrainDebugLayerMessages() == 1, "observer catches exactly the planted warning");
		check(device.DrainDebugLayerMessages() == 0, "canary consumed with zero residual messages");
	} catch (const std::exception& error) {
		std::fprintf(stderr, "FAIL: real D3D12 operation: %s\n", error.what());
		std::fprintf(stderr, "Device removal reason: 0x%08lx\n", static_cast<unsigned long>(device.dev->GetDeviceRemovedReason()));
		device.DrainDebugLayerMessages();
		ok = false;
	}
	check(StateTrackingViolations().Total() == 0, "logical state and lifetime checks report zero violations");
#if defined(SUPERSLM_GPU_STATE_TRACKING)
	{
		auto& tracker = state_tracking::Get();
		std::lock_guard<std::mutex> guard(tracker.mutex);
		check(tracker.buffers_created >= 11 && tracker.buffers_released >= 10,
		    "tracking observed the real allocations and releases");
	}
#endif
	return ok ? 0 : 1;
}
