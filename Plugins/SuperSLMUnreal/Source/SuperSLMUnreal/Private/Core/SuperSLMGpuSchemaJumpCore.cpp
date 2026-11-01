// The GPU subsystem's multi-id delivery, save carry and wrapper extension (P2's, kept by plugin 1.4 for Exact on the
// multi-row path). See SuperSLMGpuSchemaJumpCore.h for the contract. C++17, standard library only, no exceptions, no
// RTTI.

#include "SuperSLMGpuSchemaJumpCore.h"

#include <algorithm>

namespace SuperSLMCore
{
	namespace GpuJump
	{
		void ApplyResult(const FActionResultIds& Result, FDeliveryCounters& Counters,
			std::vector<FDeliveredId>& ListenerRecord)
		{
			for (const FDeliveredId& D : Result.Ids)
			{
				ListenerRecord.push_back(D);
				Counters.Generated += 1;
				if (D.bForced)
				{
					Counters.ForcedTokenCount += 1;
				}
			}
		}

		std::vector<FCarriedEvent> CarryPending(const std::vector<FPendingResult>& Pending, int64_t NowTick)
		{
			std::vector<FCarriedEvent> Out;
			for (const FPendingResult& R : Pending)
			{
				const int64_t Raw = R.ApplyTick - NowTick;
				const int32_t Delay = static_cast<int32_t>(std::max<int64_t>(1, std::min<int64_t>(Raw, 0x7fffffff)));
				if (R.Ids.empty())
				{
					FCarriedEvent E;
					E.Kind = R.Kind;
					E.Fault = R.Fault;
					E.Token = -1;
					E.Delay = Delay;
					Out.push_back(E);
					continue;
				}
				for (const FDeliveredId& D : R.Ids)
				{
					FCarriedEvent E;
					E.Kind = R.Kind;
					E.Fault = R.Fault;
					E.bForced = D.bForced;
					E.Token = D.Token;
					E.Delay = Delay;
					Out.push_back(E);
				}
			}
			return Out;
		}

		int32_t ClampCarriedDelay(int32_t Delay, int32_t K)
		{
			return std::max(1, std::min(Delay, K - 1));
		}

		std::vector<FPendingResult> RequeueCarried(const std::vector<FCarriedEvent>& Carried, int64_t NowTick, int32_t K)
		{
			std::vector<FPendingResult> Out;
			for (const FCarriedEvent& E : Carried)
			{
				FPendingResult R;
				R.Kind = E.Kind;
				R.Fault = E.Fault;
				if (E.Token >= 0)
				{
					R.Ids.push_back(FDeliveredId{E.Token, E.bForced});
				}
				R.ApplyTick = NowTick + ClampCarriedDelay(E.Delay, K);
				Out.push_back(R);
			}
			return Out;
		}

		namespace Detail
		{
			void PutU8(std::vector<uint8_t>& Out, uint8_t V)
			{
				Out.push_back(V);
			}

			void PutU32(std::vector<uint8_t>& Out, uint32_t V)
			{
				for (uint32_t B = 0; B < 4; ++B)
				{
					Out.push_back(static_cast<uint8_t>(V >> (8u * B)));
				}
			}

			struct FReader
			{
				const uint8_t* Data;
				size_t Size;
				size_t At = 0;
				bool bBad = false;

				uint8_t U8()
				{
					if (bBad || At + 1 > Size)
					{
						bBad = true;
						return 0;
					}
					return Data[At++];
				}

				uint32_t U32()
				{
					if (bBad || At + 4 > Size)
					{
						bBad = true;
						return 0;
					}
					uint32_t V = 0;
					for (uint32_t B = 0; B < 4; ++B)
					{
						V |= static_cast<uint32_t>(Data[At + B]) << (8u * B);
					}
					At += 4;
					return V;
				}
			};
		}

		void EncodeWrapperExt(const FWrapperExt& Ext, std::vector<uint8_t>& Out)
		{
			Detail::PutU32(Out, WrapperExtMagic);
			Detail::PutU32(Out, WrapperExtVersion1);
			Detail::PutU8(Out, Ext.Path);
			Detail::PutU8(Out, Ext.bPrimed ? 1 : 0);
			Detail::PutU8(Out, Ext.bStopped ? 1 : 0);
			Detail::PutU8(Out, 0);
			Detail::PutU32(Out, static_cast<uint32_t>(Ext.LastToken));
			Detail::PutU32(Out, static_cast<uint32_t>(Ext.InFlightIssued));
			Detail::PutU32(Out, static_cast<uint32_t>(Ext.Carried.size()));
			for (const FCarriedEvent& E : Ext.Carried)
			{
				Detail::PutU8(Out, E.Kind);
				Detail::PutU8(Out, E.Fault);
				Detail::PutU8(Out, 0);
				Detail::PutU8(Out, 0);
				Detail::PutU32(Out, static_cast<uint32_t>(E.Token));
				Detail::PutU32(Out, static_cast<uint32_t>(E.Delay));
			}
		}

		bool DecodeWrapperExt(const uint8_t* Data, size_t Size, FWrapperExt& Out, size_t& OutConsumed)
		{
			OutConsumed = 0;
			if (Data == nullptr)
			{
				return false;
			}
			Detail::FReader R{Data, Size};
			FWrapperExt X;
			const uint32_t Magic = R.U32();
			X.Version = R.U32();
			X.Path = R.U8();
			X.bPrimed = R.U8() != 0;
			X.bStopped = R.U8() != 0;
			R.U8(); // version 1's spare byte, ignored as 1.3.0 does
			X.LastToken = static_cast<int32_t>(R.U32());
			X.InFlightIssued = static_cast<int32_t>(R.U32());
			const uint32_t Count = R.U32();
			if (R.bBad || Magic != WrapperExtMagic || X.Version != WrapperExtVersion1 || X.Path > 1 || Count > WrapperExtMaxCarried)
			{
				return false;
			}
			if (static_cast<size_t>(Count) * 12u > Size - R.At)
			{
				return false;
			}
			X.Carried.reserve(Count);
			for (uint32_t I = 0; I < Count; ++I)
			{
				FCarriedEvent E;
				E.Kind = R.U8();
				E.Fault = R.U8();
				R.U8(); // spare bytes, ignored in version 1
				R.U8();
				E.Token = static_cast<int32_t>(R.U32());
				E.Delay = static_cast<int32_t>(R.U32());
				X.Carried.push_back(E);
			}
			if (R.bBad)
			{
				return false;
			}
			OutConsumed = R.At;
			Out = X;
			return true;
		}

		bool IsCanonicalWrapperExt(const uint8_t* Data, size_t Size)
		{
			if (Data == nullptr)
			{
				return false;
			}
			Detail::FReader R{Data, Size};
			const uint32_t Magic = R.U32();
			const uint32_t Version = R.U32();
			return !R.bBad && Magic == WrapperExtMagic && Version == WrapperExtVersion2;
		}
	}
}
