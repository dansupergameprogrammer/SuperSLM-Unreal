#pragma once

// The GPU subsystem's multi-id delivery, save carry and wrapper extension, the policy that needs neither Unreal nor
// the engine. Written for schema fast-decode plan rev 1.4.4, §6.1 (slice P2); kept by plugin 1.4 for Exact on the
// multi-row path (multi-row GPU plan rev 1.3.2, MR6; MR-D11).
//
// Plugin 1.4 removes Canonical (errata E-11 (e); multi-row rulings 39 Q7, Q8, Q10 and 40). P2's Canonical machinery is
// gone with it: the R_max planner and its wall table, the clip, the resting shapes, the jump verb's advance and status
// map, the sliced path's forced rows, and the wrapper extension's version 2. What is left is what an Exact chunk's
// close shares with it:
//   - delivery: one result per action holding its id list in order (the chunk's committed drafts, each marked
//     forced, then the produced token), counted into Generated and ForcedTokenCount;
//   - the carry of pending results through a save (one carried event per id, delays clamped to K - 1 on restore);
//   - the wrapper extension's codec, version 1 only (1.3.0's layout byte for byte). A version 2 wrapper, which only a
//     Canonical GPU generation of the unreleased P2 wrote, is refused (ruling 39 Q8).
//
// It never calls Layer 1. Build constraints (it compiles inside Unreal): C++17, standard library only, no exceptions,
// no RTTI, no dynamically initialised globals. Every name is inside SuperSLMCore::GpuJump, so a unity build that
// includes the other cores meets no second definition.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SuperSLMCore
{
	namespace GpuJump
	{
		// --- Statuses (SslmGpuStatus's values; the subsystem static_asserts them against the vendored header) ---

		constexpr int32_t StatusOk = 0;
		constexpr int32_t StatusBusy = 2;
		constexpr int32_t StatusKvBufferMismatch = 7;
		constexpr int32_t StatusDeviceLost = 8;
		constexpr int32_t StatusTokenIdOutOfRange = 10;
		constexpr int32_t StatusSequenceRejected = 11;
		constexpr int32_t StatusAllocationFailed = 16;
		constexpr int32_t StatusKvBudgetExceeded = 24;
		constexpr int32_t StatusForcedTableAbsent = 31;   // engine 1.17.0's ordinal 31
		constexpr int32_t StatusContextCapExceeded = 32;  // engine 1.17.0's ordinal 32

		// The decode bridges' "schema dead end" token (the engine re-arms there).
		constexpr int32_t DeadEndToken = -2;

		// --- Delivery ---

		// One delivered id. bForced: a forced id (no 1.4 path delivers one: an Exact close's committed drafts are the
		// model's own tokens, ruling 40, so the plugin always passes false); kept for the carry's shape.
		struct FDeliveredId
		{
			int32_t Token = -1;
			bool bForced = false;
		};

		// One job's results: Results[I] stays one-to-one with the job's actions; a chunk close's ids are one result.
		struct FActionResultIds
		{
			std::vector<FDeliveredId> Ids;
		};

		struct FDeliveryCounters
		{
			int64_t Generated = 0;
			int64_t ForcedTokenCount = 0;
		};

		// The apply: delivers the ids in order to Listener (each once) and counts them.
		void ApplyResult(const FActionResultIds& Result, FDeliveryCounters& Counters,
			std::vector<FDeliveredId>& ListenerRecord);

		// --- Save and restore ---

		// A pending result: the action's ids, and its apply tick.
		struct FPendingResult
		{
			std::vector<FDeliveredId> Ids;
			int64_t ApplyTick = 0;
			// The result kind and fault bytes of 1.3.0's carried event (EResultKind, ESuperSLMGpuFaultReason).
			uint8_t Kind = 0;
			uint8_t Fault = 0;
		};

		struct FCarriedEvent
		{
			uint8_t Kind = 0;
			uint8_t Fault = 0;
			// Not written by version 1 (1.3.0's layout has no flag): a restored event reads false.
			bool bForced = false;
			int32_t Token = -1;
			int32_t Delay = 1;
		};

		// The save's carry: one event per id, each with its result's delay max(1, ApplyTick - NowTick). A result
		// with no ids (a fault, a drain) is one event with Token -1.
		std::vector<FCarriedEvent> CarryPending(const std::vector<FPendingResult>& Pending, int64_t NowTick);

		// The restore's clamp: max(1, min(Delay, K - 1)).
		int32_t ClampCarriedDelay(int32_t Delay, int32_t K);

		// The restore's re-queue: each carried event its own single-id result at NowTick + the clamped delay.
		std::vector<FPendingResult> RequeueCarried(const std::vector<FCarriedEvent>& Carried, int64_t NowTick, int32_t K);

		// --- The wrapper extension (the subsystem's GPU section, before the Layer-1 size) ---

		constexpr uint32_t WrapperExtMagic = 0x47554c53u;
		constexpr uint32_t WrapperExtVersion1 = 1;
		// Written only by the unreleased P2 for a Canonical GPU generation; refused (ruling 39 Q8).
		constexpr uint32_t WrapperExtVersion2 = 2;
		constexpr uint32_t WrapperExtMaxCarried = 1u << 20;

		struct FWrapperExt
		{
			uint32_t Version = WrapperExtVersion1;
			uint8_t Path = 0;
			bool bPrimed = false;
			bool bStopped = false;
			int32_t LastToken = -1;
			int32_t InFlightIssued = 0;
			std::vector<FCarriedEvent> Carried;
		};

		// Appends magic, version 1, the header and the carried events (not the Layer-1 size), with zero spare bytes:
		// 1.3.0's layout byte for byte, whatever Ext.Version says.
		void EncodeWrapperExt(const FWrapperExt& Ext, std::vector<uint8_t>& Out);

		// Reads what EncodeWrapperExt wrote from Data[0..Size). Refuses (false) a bad magic, a version other than 1
		// (version 2 included: a Canonical GPU wrapper, ruling 39 Q8), a path byte > 1, more than WrapperExtMaxCarried
		// events and a truncation. Version 1's spare bytes are ignored, as 1.3.0 does. OutConsumed is the bytes read.
		bool DecodeWrapperExt(const uint8_t* Data, size_t Size, FWrapperExt& Out, size_t& OutConsumed);

		// True when Data[0..Size) starts with the magic and version 2: the wrapper a Canonical GPU generation wrote. The
		// subsystem names it in the refusal's message.
		bool IsCanonicalWrapperExt(const uint8_t* Data, size_t Size);
	}
}
