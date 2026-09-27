#include "SuperSLMSaveBlob.h"

#ifndef SUPERSLM_LAYER1_TAG
#error "SUPERSLM_LAYER1_TAG must be defined by SuperSLMUnreal.Build.cs from ThirdParty/SuperSLM/VENDORED_VERSION.txt"
#endif
#ifndef SUPERSLM_LAYER1_COMMIT
#error "SUPERSLM_LAYER1_COMMIT must be defined by SuperSLMUnreal.Build.cs from ThirdParty/SuperSLM/VENDORED_VERSION.txt"
#endif

namespace
{
	constexpr uint8 kMagic[4] = { 'S', 'L', 'U', 'W' };
	constexpr uint32 kWrapperVersion = 2; // 2 adds the adapter identity (D-SLM7341)
	constexpr int32 kTagBytes = 16;
	constexpr int32 kCommitBytes = 40;
	constexpr int32 kFixedBytes = 156; // through the adapter hash, see the layout table

	void PutU8(TArray<uint8>& B, uint8 V) { B.Add(V); }
	void PutU32(TArray<uint8>& B, uint32 V)
	{
		for (int32 I = 0; I < 4; ++I) { B.Add(static_cast<uint8>(V >> (8 * I))); }
	}
	void PutU64(TArray<uint8>& B, uint64 V)
	{
		for (int32 I = 0; I < 8; ++I) { B.Add(static_cast<uint8>(V >> (8 * I))); }
	}
	void PutFixedAscii(TArray<uint8>& B, const FString& S, int32 Width)
	{
		const FTCHARToUTF8 Utf8(*S);
		for (int32 I = 0; I < Width; ++I)
		{
			B.Add(I < Utf8.Length() ? static_cast<uint8>(Utf8.Get()[I]) : 0);
		}
	}
	void PutInt32Array(TArray<uint8>& B, const TArray<int32>& A)
	{
		PutU32(B, static_cast<uint32>(A.Num()));
		for (int32 V : A) { PutU32(B, static_cast<uint32>(V)); }
	}

	struct FReader
	{
		const TArray<uint8>& B;
		int64 Pos = 0;
		bool bOk = true;

		explicit FReader(const TArray<uint8>& InB) : B(InB) {}

		bool Need(int64 N)
		{
			if (!bOk || N < 0 || Pos + N > B.Num())
			{
				bOk = false;
			}
			return bOk;
		}
		uint8 U8()
		{
			return Need(1) ? B[Pos++] : 0;
		}
		uint32 U32()
		{
			if (!Need(4)) { return 0; }
			uint32 V = 0;
			for (int32 I = 0; I < 4; ++I) { V |= static_cast<uint32>(B[Pos + I]) << (8 * I); }
			Pos += 4;
			return V;
		}
		uint64 U64()
		{
			if (!Need(8)) { return 0; }
			uint64 V = 0;
			for (int32 I = 0; I < 8; ++I) { V |= static_cast<uint64>(B[Pos + I]) << (8 * I); }
			Pos += 8;
			return V;
		}
		FString FixedAscii(int32 Width)
		{
			if (!Need(Width)) { return FString(); }
			int32 Len = 0;
			while (Len < Width && B[Pos + Len] != 0) { ++Len; }
			const FString S = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(B.GetData() + Pos), Len);
			Pos += Width;
			return S;
		}
		bool Int32Array(TArray<int32>& Out)
		{
			const uint32 Count = U32();
			if (!Need(static_cast<int64>(Count) * 4)) { return false; }
			Out.Reset(Count);
			for (uint32 I = 0; I < Count; ++I) { Out.Add(static_cast<int32>(U32())); }
			return bOk;
		}
	};
}

namespace SuperSLMSaveBlob
{
	FString CompiledLayer1Tag() { return FString(TEXT(SUPERSLM_LAYER1_TAG)); }
	FString CompiledLayer1Commit() { return FString(TEXT(SUPERSLM_LAYER1_COMMIT)); }

	int64 BeginWrite(TArray<uint8>& OutBlob, const FContents& C, int64 PayloadCapacity)
	{
		OutBlob.Reset();
		for (uint8 M : kMagic) { PutU8(OutBlob, M); }
		PutU32(OutBlob, kWrapperVersion);
		PutU8(OutBlob, static_cast<uint8>(C.Backend));
		PutU8(OutBlob, static_cast<uint8>(C.Phase));
		PutU8(OutBlob, static_cast<uint8>(C.SpanKind));
		PutU8(OutBlob, C.bReadyForLogits ? 1 : 0);
		PutFixedAscii(OutBlob, C.Layer1Tag, kTagBytes);
		PutFixedAscii(OutBlob, C.Layer1Commit, kCommitBytes);
		for (int32 I = 0; I < 32; ++I) { PutU8(OutBlob, C.ArtifactHash[I]); }
		PutU32(OutBlob, static_cast<uint32>(C.MaxNewTokensRemaining));
		PutU32(OutBlob, static_cast<uint32>(C.LayerBudget));
		PutU32(OutBlob, static_cast<uint32>(C.LayersDoneInToken));
		PutU64(OutBlob, static_cast<uint64>(C.ContextUsed));
		PutU8(OutBlob, C.bHasAdapter ? 1 : 0);
		PutU8(OutBlob, 0);
		PutU8(OutBlob, 0);
		PutU8(OutBlob, 0);
		for (int32 I = 0; I < 32; ++I) { PutU8(OutBlob, C.bHasAdapter ? C.AdapterHash[I] : 0); }
		check(OutBlob.Num() == kFixedBytes);
		PutInt32Array(OutBlob, C.PromptRemaining);
		PutInt32Array(OutBlob, C.StopTokenIds);
		{
			const FTCHARToUTF8 Name(*C.SchemaName);
			PutU32(OutBlob, static_cast<uint32>(Name.Length()));
			OutBlob.Append(reinterpret_cast<const uint8*>(Name.Get()), Name.Length());
		}
		PutU64(OutBlob, static_cast<uint64>(PayloadCapacity));
		const int64 PayloadOffset = OutBlob.Num();
		OutBlob.AddUninitialized(PayloadCapacity);
		return PayloadOffset;
	}

	void FinalizePayload(TArray<uint8>& Blob, int64 PayloadOffset, int64 PayloadBytes)
	{
		uint8* LengthField = Blob.GetData() + PayloadOffset - 8;
		for (int32 I = 0; I < 8; ++I)
		{
			LengthField[I] = static_cast<uint8>(static_cast<uint64>(PayloadBytes) >> (8 * I));
		}
		Blob.SetNum(PayloadOffset + PayloadBytes, EAllowShrinking::Yes);
	}

	bool Read(const TArray<uint8>& Blob, FContents& Out, const uint8*& OutPayload, int64& OutPayloadBytes, FString& OutError)
	{
		FReader R(Blob);
		if (!R.Need(kFixedBytes) || FMemory::Memcmp(Blob.GetData(), kMagic, 4) != 0)
		{
			OutError = TEXT("not a SuperSLM save blob (missing or wrong wrapper header)");
			return false;
		}
		R.Pos = 4;
		const uint32 Version = R.U32();
		if (Version != kWrapperVersion)
		{
			OutError = FString::Printf(TEXT("save blob wrapper version %u is not supported (this build reads version %u)"), Version, kWrapperVersion);
			return false;
		}
		const uint8 Backend = R.U8();
		const uint8 Phase = R.U8();
		const uint8 SpanKind = R.U8();
		const uint8 Ready = R.U8();
		if (Backend > static_cast<uint8>(ESuperSLMBackend::GPU) ||
			Phase > static_cast<uint8>(ESuperSLMSequencePhase::Faulted) ||
			SpanKind > static_cast<uint8>(ESuperSLMSpanKind::SchemaContent) || Ready > 1)
		{
			OutError = TEXT("save blob header carries an out-of-range field");
			return false;
		}
		Out.Backend = static_cast<ESuperSLMBackend>(Backend);
		Out.Phase = static_cast<ESuperSLMSequencePhase>(Phase);
		Out.SpanKind = static_cast<ESuperSLMSpanKind>(SpanKind);
		Out.bReadyForLogits = Ready != 0;
		Out.Layer1Tag = R.FixedAscii(kTagBytes);
		Out.Layer1Commit = R.FixedAscii(kCommitBytes);
		if (R.Need(32))
		{
			FMemory::Memcpy(Out.ArtifactHash, Blob.GetData() + R.Pos, 32);
			R.Pos += 32;
		}
		Out.MaxNewTokensRemaining = static_cast<int32>(R.U32());
		Out.LayerBudget = static_cast<int32>(R.U32());
		Out.LayersDoneInToken = static_cast<int32>(R.U32());
		Out.ContextUsed = static_cast<int64>(R.U64());
		{
			const uint8 HasAdapter = R.U8();
			R.U8(); R.U8(); R.U8();
			if (HasAdapter > 1)
			{
				OutError = TEXT("save blob header carries an out-of-range adapter flag");
				return false;
			}
			Out.bHasAdapter = HasAdapter != 0;
			if (R.Need(32))
			{
				FMemory::Memcpy(Out.AdapterHash, Blob.GetData() + R.Pos, 32);
				R.Pos += 32;
			}
		}
		R.Int32Array(Out.PromptRemaining);
		R.Int32Array(Out.StopTokenIds);
		{
			const uint32 NameBytes = R.U32();
			if (R.Need(NameBytes))
			{
				Out.SchemaName = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Blob.GetData() + R.Pos), static_cast<int32>(NameBytes));
				R.Pos += NameBytes;
			}
		}
		const uint64 PayloadBytes = R.U64();
		if (!R.bOk || PayloadBytes == 0 || PayloadBytes > static_cast<uint64>(Blob.Num() - R.Pos))
		{
			OutError = TEXT("save blob is truncated or its payload length is inconsistent");
			return false;
		}
		if (Out.MaxNewTokensRemaining < 0 || Out.LayerBudget < 0 || Out.LayersDoneInToken < 0 || Out.ContextUsed < 0)
		{
			OutError = TEXT("save blob header carries a negative count");
			return false;
		}
		OutPayload = Blob.GetData() + R.Pos;
		OutPayloadBytes = static_cast<int64>(PayloadBytes);
		return true;
	}
}

namespace SuperSLM
{
	bool DebugOverwriteBackendTag(TArray<uint8>& Blob, ESuperSLMBackend NewBackend)
	{
		SuperSLMSaveBlob::FContents Contents;
		const uint8* Payload = nullptr;
		int64 PayloadBytes = 0;
		FString Error;
		if (!SuperSLMSaveBlob::Read(Blob, Contents, Payload, PayloadBytes, Error))
		{
			return false;
		}
		Blob[SuperSLMSaveBlob::kBackendTagOffset] = static_cast<uint8>(NewBackend);
		return true;
	}
}
