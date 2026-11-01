// P2's delivery and the carry of pending advances through a save (schema fast-decode plan rev 1.4.4, §6.1 *The
// whole-token path* step 3, *Save and restore* under Q27 A): host cell P2H.4. Mutants XG27, XG27c.
//
// Ruling 40 (red-suite-mr6.md): P2's Canonical GPU core is deleted in plugin 1.4, and the fake engine
// (gpu_fake_engine.h) with it. What stays here is the core the implementation keeps: ApplyResult, the carry
// (CarryPending, ClampCarriedDelay, RequeueCarried) and wrapper codec version 1. The cells that ran the fake's
// Canonical stream, gpu_delivery_one_result_per_action and gpu_restore_issued_count, are re-pointed at the
// multi-row Exact path as automation cells R40.1 and R40.2 (SuperSLMGpuMultiRowMr6Tests.cpp).

#include "harness.h"

#include "SuperSLMGpuSchemaJumpCore.h"

#include <string>
#include <vector>

using namespace SuperSLMCore::GpuJump;

namespace
{
	FDeliveredId F(int32_t T) { return FDeliveredId{T, true}; }
	FDeliveredId P(int32_t T) { return FDeliveredId{T, false}; }

	uint32_t U32At(const std::vector<uint8_t>& B, size_t At)
	{
		return static_cast<uint32_t>(B[At]) | static_cast<uint32_t>(B[At + 1]) << 8 |
			static_cast<uint32_t>(B[At + 2]) << 16 | static_cast<uint32_t>(B[At + 3]) << 24;
	}
}

HOST_TEST(gpu_delivery_order_and_count, "P2H.4")
{
	FActionResultIds R;
	R.Ids = {F(13), F(14), P(21)};
	FDeliveryCounters C;
	std::vector<FDeliveredId> L;
	ApplyResult(R, C, L);
	REQUIRE_EQ(L.size(), static_cast<size_t>(3));
	EXPECT_EQ(L[0].Token, 13);
	EXPECT_EQ(L[1].Token, 14);
	EXPECT_EQ(L[2].Token, 21);
	EXPECT_TRUE(L[0].bForced && L[1].bForced && !L[2].bForced);
	EXPECT_EQ(C.Generated, 3);
	EXPECT_EQ(C.ForcedTokenCount, 2);
	FActionResultIds Empty;
	ApplyResult(Empty, C, L);
	EXPECT_EQ(C.Generated, 3);
	EXPECT_EQ(L.size(), static_cast<size_t>(3));
}

HOST_TEST(gpu_carry_pending_advance, "P2H.4 (XG27)")
{
	std::vector<FPendingResult> Pending(3);
	Pending[0].Ids = {F(13), F(14), P(21)};
	Pending[0].ApplyTick = 105;
	Pending[0].Kind = 1;
	Pending[1].Ids = {F(15)};
	Pending[1].ApplyTick = 106;
	Pending[1].Kind = 1;
	Pending[2].ApplyTick = 104; // a fault result with no id
	Pending[2].Kind = 4;
	Pending[2].Fault = 3;
	const std::vector<FCarriedEvent> C = CarryPending(Pending, 103);
	REQUIRE_EQ(C.size(), static_cast<size_t>(5));
	// One event per id, each with its advance's delay, forced ids as forced.
	EXPECT_EQ(C[0].Token, 13);
	EXPECT_TRUE(C[0].bForced);
	EXPECT_EQ(C[0].Delay, 2);
	EXPECT_EQ(C[1].Token, 14);
	EXPECT_TRUE(C[1].bForced);
	EXPECT_EQ(C[1].Delay, 2);
	EXPECT_EQ(C[2].Token, 21);
	EXPECT_TRUE(!C[2].bForced);
	EXPECT_EQ(C[2].Delay, 2);
	EXPECT_EQ(C[3].Token, 15);
	EXPECT_EQ(C[3].Delay, 3);
	EXPECT_EQ(C[4].Token, -1);
	EXPECT_EQ(C[4].Kind, 4);
	EXPECT_EQ(C[4].Fault, 3);
	EXPECT_EQ(C[4].Delay, 1);
	EXPECT_EQ(C[0].Kind, 1);
	// A result already due is carried with delay 1, as 1.3.0 does.
	std::vector<FPendingResult> Late(1);
	Late[0].Ids = {P(7)};
	Late[0].ApplyTick = 90;
	const std::vector<FCarriedEvent> LateCarried = CarryPending(Late, 103);
	REQUIRE_EQ(LateCarried.size(), static_cast<size_t>(1));
	EXPECT_EQ(LateCarried[0].Delay, 1);

	// The clamp to K - 1, at least 1.
	EXPECT_EQ(ClampCarriedDelay(5, 3), 2);
	EXPECT_EQ(ClampCarriedDelay(2, 4), 2);
	EXPECT_EQ(ClampCarriedDelay(0, 3), 1);
	EXPECT_EQ(ClampCarriedDelay(4, 1), 1);
	EXPECT_EQ(ClampCarriedDelay(3, 2), 1);

	// The re-queue: each carried id its own single-id result, kind kept, delays clamped (the advance's ids apply
	// together, possibly earlier than planned).
	const std::vector<FPendingResult> Q = RequeueCarried(C, 200, 3);
	REQUIRE_EQ(Q.size(), static_cast<size_t>(5));
	for (size_t I = 0; I < 4; ++I)
	{
		REQUIRE_EQ(Q[I].Ids.size(), static_cast<size_t>(1));
		EXPECT_EQ(Q[I].Ids[0].Token, C[I].Token);
		EXPECT_EQ(Q[I].Ids[0].bForced, C[I].bForced);
	}
	EXPECT_EQ(Q[0].ApplyTick, 202);
	EXPECT_EQ(Q[3].ApplyTick, 202);
	EXPECT_EQ(Q[4].ApplyTick, 201);
	EXPECT_TRUE(Q[4].Ids.empty());
	EXPECT_EQ(Q[4].Kind, 4);
	EXPECT_EQ(Q[4].Fault, 3);
	// Restored, the forced ids count: ForcedTokenCount from the restore is 3 of the 4 ids.
	FDeliveryCounters Counters;
	std::vector<FDeliveredId> L;
	for (const FPendingResult& R : Q)
	{
		FActionResultIds A;
		A.Ids = R.Ids;
		ApplyResult(A, Counters, L);
	}
	EXPECT_EQ(Counters.ForcedTokenCount, 3);
	EXPECT_EQ(Counters.Generated, 4);
}

HOST_TEST(gpu_wrapper_writes_version_one, "P2H.4; plugin 1.4 (ruling 39 Q8, ruling 40: version 1 is the only version)")
{
	// Ruling 40 re-points gpu_wrapper_version_is_one_in_1_4: WrapperVersionFor() chose version 2 for a Canonical
	// generation and goes with wrapper v2. A wrapper as the writer builds it is version 1 and encodes as version 1.
	FWrapperExt X;
	EXPECT_EQ(X.Version, WrapperExtVersion1);
	EXPECT_EQ(WrapperExtVersion1, 1u);
	std::vector<uint8_t> B;
	EncodeWrapperExt(X, B);
	REQUIRE_TRUE(B.size() >= 8);
	EXPECT_EQ(U32At(B, 0), WrapperExtMagic);
	EXPECT_EQ(U32At(B, 4), 1u);
}

HOST_TEST(gpu_wrapper_v1_layout, "P2H.4")
{
	// Version 1 is 1.3.0's layout byte for byte (SuperSLMGpuSubsystem.cpp's writer): magic, version, path, primed,
	// stopped, 0, LastToken, in-flight issued, count, then per event kind, fault, 0, 0, token, delay.
	FWrapperExt X;
	X.Version = WrapperExtVersion1;
	X.Path = 1;
	X.bPrimed = true;
	X.bStopped = false;
	X.LastToken = 77;
	X.InFlightIssued = 2;
	FCarriedEvent A;
	A.Kind = 1;
	A.Fault = 0;
	A.bForced = true; // version 1 has no place for it
	A.Token = 13;
	A.Delay = 2;
	X.Carried = {A};
	std::vector<uint8_t> B;
	EncodeWrapperExt(X, B);
	REQUIRE_EQ(B.size(), static_cast<size_t>(4 + 4 + 4 + 4 + 4 + 4 + 12));
	EXPECT_EQ(U32At(B, 0), WrapperExtMagic);
	EXPECT_EQ(U32At(B, 4), 1u);
	EXPECT_EQ(B[8], 1);
	EXPECT_EQ(B[9], 1);
	EXPECT_EQ(B[10], 0);
	EXPECT_EQ(B[11], 0);
	EXPECT_EQ(U32At(B, 12), 77u);
	EXPECT_EQ(U32At(B, 16), 2u);
	EXPECT_EQ(U32At(B, 20), 1u);
	EXPECT_EQ(B[24], 1);
	EXPECT_EQ(B[25], 0);
	EXPECT_EQ(B[26], 0);
	EXPECT_EQ(B[27], 0);
	EXPECT_EQ(U32At(B, 28), 13u);
	EXPECT_EQ(U32At(B, 32), 2u);
	FWrapperExt Y;
	size_t Used = 0;
	REQUIRE_TRUE(DecodeWrapperExt(B.data(), B.size(), Y, Used));
	EXPECT_EQ(Used, B.size());
	EXPECT_EQ(Y.Version, 1u);
	REQUIRE_EQ(Y.Carried.size(), static_cast<size_t>(1));
	EXPECT_TRUE(!Y.Carried[0].bForced);
	EXPECT_EQ(Y.Carried[0].Token, 13);
	// 1.3.0's spare bytes are ignored on a version 1 read.
	std::vector<uint8_t> Dirty = B;
	Dirty[11] = 3;
	Dirty[26] = 1;
	REQUIRE_TRUE(DecodeWrapperExt(Dirty.data(), Dirty.size(), Y, Used));
	EXPECT_TRUE(!Y.Carried[0].bForced);
}

HOST_TEST(gpu_wrapper_v2_is_refused, "plugin 1.4 (ruling 39 Q8: a version 2 wrapper, Canonical GPU, never released, is refused)")
{
	// A version 1 wrapper reads (the positive clause), and the same bytes stamped version 2 are refused.
	FWrapperExt X;
	X.Version = WrapperExtVersion1;
	X.Path = 1;
	X.LastToken = 13;
	FCarriedEvent E;
	E.Kind = 1;
	E.Token = 14;
	E.Delay = 1;
	X.Carried = {E};
	std::vector<uint8_t> B;
	EncodeWrapperExt(X, B);
	FWrapperExt Y;
	size_t Used = 0;
	REQUIRE_TRUE(DecodeWrapperExt(B.data(), B.size(), Y, Used));
	EXPECT_EQ(Y.Version, 1u);
	B[4] = 2;  // the version field (u32 little-endian after the magic)
	EXPECT_TRUE(!DecodeWrapperExt(B.data(), B.size(), Y, Used));
	// A wrapper P2's writer would have produced for a Canonical generation (version 2, its Rest byte set) is refused
	// too, whatever its Rest bits.
	for (uint8_t Rest = 0; Rest < 16; ++Rest)
	{
		std::vector<uint8_t> V2 = B;
		V2[11] = Rest;
		EXPECT_TRUE(!DecodeWrapperExt(V2.data(), V2.size(), Y, Used));
	}
}

HOST_TEST(gpu_wrapper_refusals, "P2H.4; plugin 1.4: on version 1, the only version (ruling 39 Q8)")
{
	FWrapperExt X;
	X.Version = WrapperExtVersion1;
	X.Path = 1;
	FCarriedEvent E;
	E.Kind = 1;
	E.Token = 5;
	X.Carried = {E};
	std::vector<uint8_t> Good;
	EncodeWrapperExt(X, Good);
	FWrapperExt Y;
	size_t Used = 0;
	REQUIRE_TRUE(DecodeWrapperExt(Good.data(), Good.size(), Y, Used));
	auto Refused = [&](std::vector<uint8_t> B) { return !DecodeWrapperExt(B.data(), B.size(), Y, Used); };
	std::vector<uint8_t> B = Good;
	B[0] ^= 1;
	EXPECT_TRUE(Refused(B)); // magic
	for (uint8_t V : {0, 2, 3})
	{
		B = Good;
		B[4] = V;
		EXPECT_TRUE(Refused(B)); // version 0, 2 (ruling 39 Q8) and 3
	}
	B = Good;
	B[8] = 2;
	EXPECT_TRUE(Refused(B)); // path
	B = Good;
	B.resize(B.size() - 1);
	EXPECT_TRUE(Refused(B)); // truncated
	B = Good;
	B[20] = 0;
	B[21] = 0;
	B[22] = 0x20; // 2 MiB events
	EXPECT_TRUE(Refused(B));
	EXPECT_TRUE(!DecodeWrapperExt(nullptr, 0, Y, Used));
}
