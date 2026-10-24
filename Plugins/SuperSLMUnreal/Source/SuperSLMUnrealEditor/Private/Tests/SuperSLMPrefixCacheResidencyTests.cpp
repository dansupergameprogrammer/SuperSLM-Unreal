// Precache plan rev 1.18.4 (suite audit G1): the Unreal side of rev 1.18's explicit residency control and its U2
// neighbours. Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
//   SuperSLM.PrefixCache.Cpu.BlueprintParity       cell 7.23                                       [U2]
//   SuperSLM.PrefixCache.Cpu.AdoptNotReady         cell 7.21, the Unreal leg (and rev 1.18.1 N3)    [U2]
//   SuperSLM.PrefixCache.Cpu.LeaseAsRestoreHint    cell 8.15                                       [U2, box]
//   SuperSLM.PrefixCache.Cpu.ShippingGuards        cell 11.7, the Unreal leg                        [U2]
//
// The rev 1.18 addition to 3.9 and the rev 1.18.1 extension to 7.8 are legs of their existing cells in
// SuperSLMPrefixCacheCpuTests.cpp.
//
// WarmPrefix(), DropPrefix() and GetPrefixOpResult() are the implementation's (U2 core, f765d34c1). The rev 1.18
// seams these cells drive (build hold, write-back hold, build states, disk exclusion, off-game-thread
// recording, restore's shared pages) are red stubs that read "not built" (SuperSLMSubsystemPrefixCache.inl),
// so every cell that needs one is red by that seam's name until the implementation wires it.
//
// Cell 7.23 reaches the Blueprint nodes by reflection: the nodes and their mirror types are applied on the box
// as patches (reflected-prefix-cache.patch, reflected-prefix-set.patch, then reflected-residency-control.patch),
// so this file compiles in a tree without them and the cell is red by name ("the Blueprint node ... exists")
// where they are absent. The signatures it reads are the patches' own:
//   WarmPrefix(Subsystem, Tokens, SchemaName, OutOp, OutError) -> bool
//   DropPrefix(Subsystem, Tokens, SchemaName, Scope, OutOp, OutError) -> bool
//   GetPrefixOpResult(Subsystem, Op, OutResult, OutDisk) -> bool
//   RequestPrefix(Subsystem, Tokens, SchemaName, OutPrefix, OutError) -> bool
//   ReturnPrefix(Subsystem, Prefix) -> bool;  GetPrefixSource(Subsystem, Prefix) -> source
//   RegisterPrefixSet(Subsystem, PrefixSet)
// with FSuperSLMPrefixOpBP / FSuperSLMPrefixBP carrying Id, and each mirror enum keeping its C++ ordinals
// (SUPERSLM_BP_MIRRORS).

#include "Misc/AutomationTest.h"
#include "SuperSLMSlotGates.h"

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC
#include "SuperSLMPrefixCacheTypes.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE

#include "Fixtures/SuperSLMPrefixCacheFixtures.h"
#include "SuperSLMBlueprintLibrary.h"
#include "SuperSLMPrefixSet.h"
#include "SuperSLMPrefixSetEditor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

using namespace SuperSLMPrefixCacheFixtures;

namespace
{
	using Seam = FSuperSLMPrefixCacheTestSeam;

	// --- Residency ops ---------------------------------------------------------------------------

	inline ESuperSLMPrefixOpResult OpResultOf(USuperSLMSubsystem& Cpu, const FSuperSLMPrefixOp& Op, FSuperSLMPrefixOpResult* OutFull = nullptr)
	{
		FSuperSLMPrefixOpResult R;
		Cpu.GetPrefixOpResult(Op, R);
		if (OutFull)
		{
			*OutFull = R;
		}
		return R.Result;
	}

	// Ticks until the op leaves Pending; returns its full result.
	inline FSuperSLMPrefixOpResult WaitOpResolved(USuperSLMSubsystem& Cpu, const FSuperSLMPrefixOp& Op, double Seconds = LeaseSeconds)
	{
		Drain(Cpu, [&Cpu, &Op]() { return OpResultOf(Cpu, Op) != ESuperSLMPrefixOpResult::Pending; }, Seconds);
		FSuperSLMPrefixOpResult R;
		Cpu.GetPrefixOpResult(Op, R);
		return R;
	}

	inline bool Warm(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, FSuperSLMPrefixOp& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Cpu.WarmPrefix(S, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: WarmPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: an op id is issued"), What), (Out.Id != 0));
	}

	inline bool Drop(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, ESuperSLMPrefixDropScope Scope, FSuperSLMPrefixOp& Out, const TCHAR* What)
	{
		FString Error;
		const bool bOk = Cpu.DropPrefix(S, Scope, Out, Error);
		return T.TestTrue(*FString::Printf(TEXT("%s: DropPrefix() accepts the spec (%s)"), What, *Error), bOk)
			&& T.TestTrue(*FString::Printf(TEXT("%s: an op id is issued"), What), (Out.Id != 0));
	}

	// A budget-mode cache with the disk tier on (the 7.8 cell's shape).
	bool DiskRig(FAutomationTestBase& T, FPagingRig& Rig, const FScopedCacheDirectory& Dir, int64 BudgetPages = 64, int32 BlockCount = 2)
	{
		return Rig.Init(T) && ConfigureLegacy(T, Rig, MakeConfig(1, 0, 0, 1)) && ReadGeometry(T, Rig)
			&& ConfigureCacheBudget(T, Rig, WithCache(MakeConfig(BlockCount, 512, 0, 1), BytesForPages(Rig.Geo, BudgetPages), Dir.Path, int64(1) << 30));
	}

	// A resident, unleased entry for S whose write-back has landed.
	bool ResidentWithFile(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, const TCHAR* What)
	{
		FSuperSLMPrefix L;
		if (!RequestReady(T, Cpu, S, L, What))
		{
			return false;
		}
		Cpu.ReturnPrefix(L);
		const FString File = DiskFileOf(Cpu, S);
		Drain(Cpu, [&File]() { return !File.IsEmpty() && FPaths::FileExists(File); }, LeaseSeconds);
		return T.TestTrue(*FString::Printf(TEXT("%s: the write-back landed (%s)"), What, *File), !File.IsEmpty() && FPaths::FileExists(File));
	}

	// --- Reflection over the Blueprint nodes (cell 7.23; the contract is in the file comment) -------------

	struct FBpCall
	{
		UFunction* Fn = nullptr;
		TArray<uint8> Buffer;

		FBpCall() = default;
		FBpCall(const FBpCall&) = delete;
		FBpCall& operator=(const FBpCall&) = delete;
		~FBpCall()
		{
			if (Fn)
			{
				Fn->DestroyStruct(Buffer.GetData());
			}
		}

		bool Begin(FAutomationTestBase& T, const TCHAR* Name, USuperSLMSubsystem& Cpu)
		{
			Fn = USuperSLMBlueprintLibrary::StaticClass()->FindFunctionByName(FName(Name));
			if (!T.TestNotNull(*FString::Printf(TEXT("7.23: the Blueprint node %s exists on USuperSLMBlueprintLibrary"), Name), Fn))
			{
				return false;
			}
			Buffer.SetNumZeroed(Fn->ParmsSize);
			Fn->InitializeStruct(Buffer.GetData());
			FObjectPropertyBase* Sub = CastField<FObjectPropertyBase>(Fn->FindPropertyByName(TEXT("Subsystem")));
			if (!T.TestNotNull(*FString::Printf(TEXT("7.23: %s takes Subsystem"), Name), Sub))
			{
				return false;
			}
			Sub->SetObjectPropertyValue(Sub->ContainerPtrToValuePtr<void>(Buffer.GetData()), &Cpu);
			return true;
		}

		FProperty* Param(const TCHAR* Name) const { return Fn ? Fn->FindPropertyByName(FName(Name)) : nullptr; }

		void Invoke() { USuperSLMBlueprintLibrary::StaticClass()->GetDefaultObject()->ProcessEvent(Fn, Buffer.GetData()); }

		bool ReturnBool() const
		{
			const FBoolProperty* R = CastField<FBoolProperty>(Fn->GetReturnProperty());
			return R && R->GetPropertyValue(R->ContainerPtrToValuePtr<void>(Buffer.GetData()));
		}

		int64 ReturnInt() const { return IntOf(Fn->GetReturnProperty(), Buffer.GetData()); }

		// An integer, enum or bool value of a property in Container (INT64_MIN when it is none of those).
		static int64 IntOf(const FProperty* P, const void* Container)
		{
			if (const FEnumProperty* E = CastField<FEnumProperty>(P))
			{
				return E->GetUnderlyingProperty()->GetSignedIntPropertyValue(E->ContainerPtrToValuePtr<void>(Container));
			}
			if (const FNumericProperty* N = CastField<FNumericProperty>(P))
			{
				return N->GetSignedIntPropertyValue(N->ContainerPtrToValuePtr<void>(Container));
			}
			if (const FBoolProperty* B = CastField<FBoolProperty>(P))
			{
				return B->GetPropertyValue(B->ContainerPtrToValuePtr<void>(Container)) ? 1 : 0;
			}
			return MIN_int64;
		}

		static bool SetInt(const FProperty* P, void* Container, int64 V)
		{
			if (const FEnumProperty* E = CastField<FEnumProperty>(P))
			{
				E->GetUnderlyingProperty()->SetIntPropertyValue(E->ContainerPtrToValuePtr<void>(Container), V);
				return true;
			}
			if (const FNumericProperty* N = CastField<FNumericProperty>(P))
			{
				N->SetIntPropertyValue(N->ContainerPtrToValuePtr<void>(Container), V);
				return true;
			}
			return false;
		}

		// A struct parameter's address and type.
		void* Struct(const TCHAR* Name, UScriptStruct** OutType) const
		{
			const FStructProperty* S = CastField<FStructProperty>(Param(Name));
			if (!S)
			{
				return nullptr;
			}
			*OutType = S->Struct;
			return const_cast<void*>(static_cast<const void*>(S->ContainerPtrToValuePtr<void>(Buffer.GetData())));
		}

		int64 FieldInt(const TCHAR* StructName, const TCHAR* Field) const
		{
			UScriptStruct* Type = nullptr;
			const void* Ptr = Struct(StructName, &Type);
			return Ptr ? IntOf(Type->FindPropertyByName(FName(Field)), Ptr) : MIN_int64;
		}

		bool SetFieldInt(const TCHAR* StructName, const TCHAR* Field, int64 V)
		{
			UScriptStruct* Type = nullptr;
			void* Ptr = Struct(StructName, &Type);
			return Ptr && SetInt(Type->FindPropertyByName(FName(Field)), Ptr, V);
		}

		// The nodes take the spec flat: Tokens (int32 array) and SchemaName.
		bool SetSpec(const FSuperSLMPrefixSpec& Spec)
		{
			const FArrayProperty* Tokens = CastField<FArrayProperty>(Param(TEXT("Tokens")));
			const FNameProperty* Schema = CastField<FNameProperty>(Param(TEXT("SchemaName")));
			if (!Tokens || !Schema || !CastField<FIntProperty>(Tokens->Inner))
			{
				return false;
			}
			*Tokens->ContainerPtrToValuePtr<TArray<int32>>(Buffer.GetData()) = Spec.Tokens;
			*Schema->ContainerPtrToValuePtr<FName>(Buffer.GetData()) = Spec.SchemaName;
			return true;
		}

		int64 ParamInt(const TCHAR* Name) const { return IntOf(Param(Name), Buffer.GetData()); }

		FString StringParam(const TCHAR* Name) const
		{
			const FStrProperty* S = CastField<FStrProperty>(Param(Name));
			return S ? *S->ContainerPtrToValuePtr<FString>(Buffer.GetData()) : FString(TEXT("<no such parameter>"));
		}
	};

	// One Blueprint WarmPrefix / DropPrefix call; returns its result and fills the op id and error.
	bool BpIssue(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixSpec& S, const ESuperSLMPrefixDropScope* Scope, bool& OutAccepted,
		FSuperSLMPrefixOp& OutOp, FString& OutError)
	{
		FBpCall C;
		if (!C.Begin(T, Scope ? TEXT("DropPrefix") : TEXT("WarmPrefix"), Cpu)
			|| !T.TestTrue(TEXT("7.23: the node takes Tokens and SchemaName"), C.SetSpec(S)))
		{
			return false;
		}
		if (Scope && !T.TestTrue(TEXT("7.23: Drop Prefix takes Scope"), FBpCall::SetInt(C.Param(TEXT("Scope")), C.Buffer.GetData(), int64(*Scope))))
		{
			return false;
		}
		C.Invoke();
		OutAccepted = C.ReturnBool();
		OutOp.Id = C.FieldInt(TEXT("OutOp"), TEXT("Id"));
		OutError = C.StringParam(TEXT("OutError"));
		return true;
	}

	bool BpOpResult(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixOp& Op, bool& OutFound, FSuperSLMPrefixOpResult& Out)
	{
		FBpCall C;
		if (!C.Begin(T, TEXT("GetPrefixOpResult"), Cpu) || !T.TestTrue(TEXT("7.23: Get Prefix Op Result takes Op (Id)"), C.SetFieldInt(TEXT("Op"), TEXT("Id"), int64(Op.Id))))
		{
			return false;
		}
		C.Invoke();
		OutFound = C.ReturnBool();
		Out.Result = static_cast<ESuperSLMPrefixOpResult>(C.ParamInt(TEXT("OutResult")));
		Out.Disk = static_cast<ESuperSLMPrefixDropDisk>(C.ParamInt(TEXT("OutDisk")));
		return true;
	}

	// Every GetPrefixOpResult output field, read through the node and through the verb, equal.
	bool OpResultParity(FAutomationTestBase& T, USuperSLMSubsystem& Cpu, const FSuperSLMPrefixOp& Op, const TCHAR* What)
	{
		FSuperSLMPrefixOpResult Cpp, Bp;
		const bool bCppFound = Cpu.GetPrefixOpResult(Op, Cpp);
		bool bBpFound = false;
		if (!BpOpResult(T, Cpu, Op, bBpFound, Bp))
		{
			return false;
		}
		bool bOk = T.TestEqual(*FString::Printf(TEXT("%s: Get Prefix Op Result's return equals the verb's"), What), bBpFound, bCppFound);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: OutResult equal"), What), (uint8)Bp.Result, (uint8)Cpp.Result);
		bOk &= T.TestEqual(*FString::Printf(TEXT("%s: OutDisk equal"), What), (uint8)Bp.Disk, (uint8)Cpp.Disk);
		return bOk;
	}

	USuperSLMPrefixSet* OneEntrySet(USuperSLMModel* Model, const TArray<int32>& Tokens)
	{
		USuperSLMPrefixSet* Set = NewObject<USuperSLMPrefixSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Set->Model = Model;
		FSuperSLMPrefixSetEntry E;
		E.Name = FName(TEXT("parity"));
		E.Tokens = Tokens;
		Set->Entries = {E};
		return Set;
	}
}

// Cell 7.23 (rev 1.18.1, audit G17): each Blueprint node gives what its C++ verb gives, on the same input:
// Warm Prefix, Drop Prefix (both Scope values), Get Prefix Op Result (every output field), and the existing
// Request Prefix, Return Prefix, Get Prefix Source and Register Prefix Set; the op ids are the subsystem's.
// Mutants: a Drop Prefix node that passes a fixed Scope -> the Memory and MemoryAndDisk pairs cannot both
// match -> red; a Disk output never written -> Out.Disk differs on the MemoryAndDisk drop -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheBlueprintParityTest, "SuperSLM.PrefixCache.Cpu.BlueprintParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheBlueprintParityTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("bp-parity"));
	if (!DiskRig(*this, Rig, Dir))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> T0;
	if (!MakeTokens(*this, Cpu, 200, 1, T0))
	{
		return false;
	}
	bool bOk = true;

	// Warm Prefix: the same spec through the verb and the node. Both accepted, both ids nonzero and distinct,
	// the node's id read back through the verb (one counter), and every output field equal once resolved.
	{
		FSuperSLMPrefixOp Cpp, Bp;
		if (!Warm(*this, Cpu, Spec(T0), Cpp, TEXT("warm (C++)")))
		{
			return false;
		}
		bool bAccepted = false;
		FString Error;
		if (!BpIssue(*this, Cpu, Spec(T0), nullptr, bAccepted, Bp, Error))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("warm: the node accepts the spec the verb accepted (%s)"), *Error), bAccepted);
		bOk &= TestTrue(TEXT("warm: the node issues a nonzero op id distinct from the verb's"), (Bp.Id != 0) && Bp.Id != Cpp.Id);
		FSuperSLMPrefixOpResult Probe;
		bOk &= TestTrue(TEXT("warm: the node's op id is one the subsystem issued"), Cpu.GetPrefixOpResult(Bp, Probe));
		WaitOpResolved(Cpu, Cpp);
		WaitOpResolved(Cpu, Bp);
		bOk &= TestEqual(TEXT("warm: both resolve Resident"), (uint8)OpResultOf(Cpu, Bp), (uint8)OpResultOf(Cpu, Cpp));
		bOk &= OpResultParity(*this, Cpu, Cpp, TEXT("warm, the verb's op"));
		bOk &= OpResultParity(*this, Cpu, Bp, TEXT("warm, the node's op"));
	}

	// Both refuse a malformed spec synchronously, with no op issued.
	{
		FSuperSLMPrefixOp Cpp, Bp;
		FString CppError, BpError;
		const bool bCpp = Cpu.WarmPrefix(Spec(TArray<int32>()), Cpp, CppError);
		bool bBp = true;
		if (!BpIssue(*this, Cpu, Spec(TArray<int32>()), nullptr, bBp, Bp, BpError))
		{
			return false;
		}
		bOk &= TestFalse(TEXT("malformed: the verb refuses an empty spec"), bCpp);
		bOk &= TestEqual(TEXT("malformed: the node refuses as the verb does"), bBp, bCpp);
		bOk &= TestTrue(TEXT("malformed: no op id from either"), !(Cpp.Id != 0) && !(Bp.Id != 0));
		bOk &= TestTrue(TEXT("malformed: the node names the refusal"), !BpError.IsEmpty());
	}

	// Drop Prefix, each Scope: four resident entries with files, the verb and the node on twin entries.
	for (const ESuperSLMPrefixDropScope Scope : {ESuperSLMPrefixDropScope::Memory, ESuperSLMPrefixDropScope::MemoryAndDisk})
	{
		const bool bDisk = Scope == ESuperSLMPrefixDropScope::MemoryAndDisk;
		const TCHAR* ScopeName = bDisk ? TEXT("MemoryAndDisk") : TEXT("Memory");
		TArray<int32> A, B;
		if (!MakeTokens(*this, Cpu, 200, bDisk ? 20 : 10, A) || !MakeTokens(*this, Cpu, 200, bDisk ? 21 : 11, B)
			|| !ResidentWithFile(*this, Cpu, Spec(A), ScopeName) || !ResidentWithFile(*this, Cpu, Spec(B), ScopeName))
		{
			return false;
		}
		FSuperSLMPrefixOp Cpp, Bp;
		if (!Drop(*this, Cpu, Spec(A), Scope, Cpp, ScopeName))
		{
			return false;
		}
		bool bAccepted = false;
		FString Error;
		if (!BpIssue(*this, Cpu, Spec(B), &Scope, bAccepted, Bp, Error))
		{
			return false;
		}
		bOk &= TestTrue(*FString::Printf(TEXT("drop %s: the node accepts (%s)"), ScopeName, *Error), bAccepted && (Bp.Id != 0) && Bp.Id != Cpp.Id);
		const FSuperSLMPrefixOpResult RC = WaitOpResolved(Cpu, Cpp);
		const FSuperSLMPrefixOpResult RB = WaitOpResolved(Cpu, Bp);
		bOk &= TestEqual(*FString::Printf(TEXT("drop %s: the verb's drop reads Dropped"), ScopeName), (uint8)RC.Result, (uint8)ESuperSLMPrefixOpResult::Dropped);
		bOk &= TestEqual(*FString::Printf(TEXT("drop %s: the verb's Disk"), ScopeName), (uint8)RC.Disk,
			(uint8)(bDisk ? ESuperSLMPrefixDropDisk::Deleted : ESuperSLMPrefixDropDisk::NotRequested));
		bOk &= TestEqual(*FString::Printf(TEXT("drop %s: the node's drop resolves as the verb's"), ScopeName), (uint8)RB.Result, (uint8)RC.Result);
		bOk &= TestEqual(*FString::Printf(TEXT("drop %s: and its Disk equals the verb's (a fixed Scope cannot match both scopes)"), ScopeName), (uint8)RB.Disk, (uint8)RC.Disk);
		bOk &= TestEqual(*FString::Printf(TEXT("drop %s: the node's entry's file as the verb's"), ScopeName), FPaths::FileExists(DiskFileOf(Cpu, Spec(B))), FPaths::FileExists(DiskFileOf(Cpu, Spec(A))));
		bOk &= OpResultParity(*this, Cpu, Cpp, *FString::Printf(TEXT("drop %s, the verb's op"), ScopeName));
		bOk &= OpResultParity(*this, Cpu, Bp, *FString::Printf(TEXT("drop %s, the node's op"), ScopeName));
	}

	// An id never issued: false through both.
	bOk &= OpResultParity(*this, Cpu, FSuperSLMPrefixOp{int64(1) << 62}, TEXT("an id never issued"));

	// Request Prefix, Get Prefix Source and Return Prefix on the same spec.
	{
		TArray<int32> R;
		FSuperSLMPrefix CppLease;
		if (!MakeTokens(*this, Cpu, 200, 30, R) || !RequestReady(*this, Cpu, Spec(R), CppLease, TEXT("request (C++)")))
		{
			return false;
		}
		FSuperSLMPrefix BpLease;
		{
			FBpCall C;
			if (!C.Begin(*this, TEXT("RequestPrefix"), Cpu) || !TestTrue(TEXT("7.23: Request Prefix takes Tokens and SchemaName"), C.SetSpec(Spec(R))))
			{
				return false;
			}
			C.Invoke();
			bOk &= TestTrue(TEXT("request: the node accepts the spec the verb accepted"), C.ReturnBool());
			BpLease.Id = C.FieldInt(TEXT("OutPrefix"), TEXT("Id"));
		}
		bOk &= TestTrue(TEXT("request: the node mints its own lease (one handle per lease)"), BpLease.IsValid() && BpLease.Id != CppLease.Id);
		bOk &= ExpectLease(*this, Cpu, BpLease, ESuperSLMPrefixLeaseState::Ready, TEXT("request (node)"));
		for (const FSuperSLMPrefix& L : {CppLease, BpLease})
		{
			FBpCall C;
			if (!C.Begin(*this, TEXT("GetPrefixSource"), Cpu) || !TestTrue(TEXT("7.23: Get Prefix Source takes Prefix (Id)"), C.SetFieldInt(TEXT("Prefix"), TEXT("Id"), L.Id)))
			{
				return false;
			}
			C.Invoke();
			bOk &= TestEqual(TEXT("source: the node reads the verb's tier"), C.ReturnInt(), int64(Cpu.GetPrefixSource(L)));
		}
		for (int32 Attempt = 0; Attempt < 2; ++Attempt)
		{
			FBpCall C;
			if (!C.Begin(*this, TEXT("ReturnPrefix"), Cpu) || !C.SetFieldInt(TEXT("Prefix"), TEXT("Id"), BpLease.Id))
			{
				return false;
			}
			C.Invoke();
			// The first return of each lease ends it (true); the second ends nothing (false).
			const bool bVerb = Cpu.ReturnPrefix(CppLease);
			bOk &= TestEqual(*FString::Printf(TEXT("return %d: the node returns what the verb returns"), Attempt + 1), C.ReturnBool(), bVerb);
		}
	}

	// Register Prefix Set: the node registers what the verb registers.
	{
		TArray<int32> E;
		if (!MakeTokens(*this, Cpu, 200, 40, E))
		{
			return false;
		}
		USuperSLMPrefixSet* Set = OneEntrySet(Rig.Model, E);
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("register: the set builds (%s)"), *Error), SuperSLMPrefixSetEditor::BuildAndLog(*Set, Findings, Error)))
		{
			return false;
		}
		const int32 Before = Stats(Cpu).CookedEntriesRegistered;
		Cpu.RegisterPrefixSet(Set);
		const int32 ByVerb = Stats(Cpu).CookedEntriesRegistered - Before;
		Cpu.UnregisterPrefixSet(Set);
		{
			FBpCall C;
			FObjectPropertyBase* P = nullptr;
			if (!C.Begin(*this, TEXT("RegisterPrefixSet"), Cpu)
				|| !TestNotNull(TEXT("7.23: Register Prefix Set takes PrefixSet"), P = CastField<FObjectPropertyBase>(C.Param(TEXT("PrefixSet")))))
			{
				return false;
			}
			P->SetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(C.Buffer.GetData()), Set);
			C.Invoke();
		}
		bOk &= TestTrue(TEXT("register: the verb registered the set's entry"), ByVerb == 1);
		bOk &= TestEqual(TEXT("register: the node registers what the verb registered"), Stats(Cpu).CookedEntriesRegistered - Before, ByVerb);
		FSuperSLMPrefix L;
		bOk &= RequestReady(*this, Cpu, Spec(E), L, TEXT("register (node)")) && ExpectSource(*this, Cpu, L, ESuperSLMPrefixSource::Cooked, TEXT("register (node)"));
		Cpu.ReturnPrefix(L);
		Cpu.UnregisterPrefixSet(Set);
	}
	return bOk;
}

// Cell 7.21, the Unreal leg (rev 1.18, PC-15 Q4, §5.10): AdoptPrefix on a lease not yet Ready is refused
// Malformed with the "not Ready" error, nothing queued (the sequence's queue count unchanged), and the same
// lease adopts once Ready. Rev 1.18.1 (audit N3): a live lease whose request resolved BudgetExhausted and
// has not been returned reads the same. The lease is held Pending by the build hold (§8).
// Mutant: the adopt queued before Ready -> the queue count rises -> red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheAdoptNotReadyTest, "SuperSLM.PrefixCache.Cpu.AdoptNotReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheAdoptNotReadyTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig)
		|| !ConfigureCacheLegacy(*this, Rig, WithCache(MakeConfig(2, 0, 1, 1), BytesForPages(Rig.Geo, 64))))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	auto ExpectNotReadyRefusal = [this, &Cpu](const FSuperSLMSequence& Seq, const FSuperSLMPrefix& Lease, const TCHAR* What)
	{
		const int32 QueuedBefore = Cpu.GetPendingLifecycleOperationCount(Seq);
		FSuperSLMLifecycleOpHandle Handle;
		FString Error;
		const ESuperSLMRestoreResult R = Cpu.AdoptPrefix(Seq, Lease, Handle, Error);
		bool bOk = TestEqual(*FString::Printf(TEXT("%s: AdoptPrefix is refused Malformed"), What), (uint8)R, (uint8)ESuperSLMRestoreResult::Malformed);
		bOk &= TestTrue(*FString::Printf(TEXT("%s: with the \"not Ready\" error (%s)"), What, *Error), Error.Contains(TEXT("not Ready")));
		bOk &= TestEqual(*FString::Printf(TEXT("%s: nothing queued"), What), Cpu.GetPendingLifecycleOperationCount(Seq), QueuedBefore);
		return bOk;
	};

	// A lease whose build is held at the build hold.
	TArray<int32> T0;
	FSuperSLMPrefix Lease;
	FSuperSLMSequence Seq;
	if (!MakeTokens(*this, Cpu, 300, 1, T0) || !Vend(*this, Cpu, Seq))
	{
		return false;
	}
	Seam::SetBuildHold(Cpu, true);
	if (!Request(*this, Cpu, Spec(T0), Lease, TEXT("held build")))
	{
		Seam::SetBuildHold(Cpu, false);
		return false;
	}
	Drain(Cpu, [&Cpu]() { return Seam::HeldBuildCount(Cpu) == 1; }, LeaseSeconds);
	bool bOk = TestEqual(TEXT("held build: the build stops at the build hold"), Seam::HeldBuildCount(Cpu), 1);
	bOk &= TestEqual(TEXT("held build: the lease is Pending"), (uint8)Cpu.GetPrefixLeaseState(Lease), (uint8)ESuperSLMPrefixLeaseState::Pending);
	bOk &= ExpectNotReadyRefusal(Seq, Lease, TEXT("held build"));
	Seam::SetBuildHold(Cpu, false);
	bOk &= ExpectLease(*this, Cpu, Lease, ESuperSLMPrefixLeaseState::Ready, TEXT("after the hold"));
	bOk &= AdoptAndWait(*this, Cpu, Seq, Lease);
	ReturnAndDrain(Cpu, Seq);
	Cpu.ReturnPrefix(Lease);
	DrainQuiet(Cpu);

	// Rev 1.18.1 (N3): a live lease that resolved BudgetExhausted, not returned.
	TArray<int32> A, B, W;
	FSuperSLMPrefix LA, LB, LW;
	FSuperSLMSequence Seq2;
	// A 31 and B 32 pages (box 2026-10-21: with A at 32, B's build peaks at R = 33 and was itself refused,
	// as the first leg's 5-page entry is all it can evict). One page free; W (R = 3) cannot fit.
	if (!MakeTokens(*this, Cpu, int32(Rig.Geo.B * 31), 2, A) || !MakeTokens(*this, Cpu, int32(Rig.Geo.B * 32), 3, B) || !MakeTokens(*this, Cpu, 100, 4, W)
		|| !RequestReady(*this, Cpu, Spec(A), LA, TEXT("A")) || !RequestReady(*this, Cpu, Spec(B), LB, TEXT("B"))
		|| !Request(*this, Cpu, Spec(W), LW, TEXT("one more")) || !Vend(*this, Cpu, Seq2))
	{
		return false;
	}
	bOk &= ExpectLease(*this, Cpu, LW, ESuperSLMPrefixLeaseState::BudgetExhausted, TEXT("one more"), 10.0);
	bOk &= ExpectNotReadyRefusal(Seq2, LW, TEXT("BudgetExhausted lease"));
	ReturnAndDrain(Cpu, Seq2);
	Cpu.ReturnPrefix(LW);
	Cpu.ReturnPrefix(LA);
	Cpu.ReturnPrefix(LB);
	DrainQuiet(Cpu);
	return bOk;
}

// Cell 8.15 [U2, box] (rev 1.18, PC-15 Q5, §5.11 (b) Adopt and hint): in budget mode, parent entry P leased; a
// restore of a child's blob (a holder that adopted P, saved) with P's lease as the hint shares floor(origin/B)
// of P's pages (*out_shared_pages through the seam, and the fill probe's draw: the private restore's draw less
// floor(origin/B)), P stays pinned by its lease for the restore (a drop issued while the restore is held at its
// apply reads DropDeferred), A is pre-charged for the shared pages (form 1 clean), and the restored child's
// continuation equals the unsaved twin's. The same restore with P's lease already returned runs private (s = 0).
// Mutants: the hint ignored (the draft's V9) -> s = 0 and the draws equal -> red; the hint wired but A not
// pre-charged -> form 1 red.
// Box 2026-10-21 (second run): the twin and the restored child each began a second generation (Turn2) on a
// Complete sequence, which one-generation-at-a-time refuses. The child's blob is now a mid-turn save (at 20 of
// 48 tokens after the suffix) and the restored child resumes that generation; the twin is the same 48 tokens
// in one generation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheLeaseAsRestoreHintTest, "SuperSLM.PrefixCache.Cpu.LeaseAsRestoreHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheLeaseAsRestoreHintTest::RunTest(const FString& Parameters)
{
	FPagingRig Rig;
	if (!Rig.Init(*this) || !ConfigureLegacy(*this, Rig, MakeConfig(1, 0, 0, 1)) || !ReadGeometry(*this, Rig) || !RequireAExShape(*this, Rig)
		|| !ConfigureCacheBudget(*this, Rig, WithCache(MakeConfig(2, 512, 0, 1), BytesForPages(Rig.Geo, 64))))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	constexpr int32 Origin = 1000;
	constexpr int32 kNew = 48;
	constexpr int32 kSaveAt = 20;
	TArray<int32> P, Suffix, Twin, Before;
	FSuperSLMPrefix LP;
	if (!MakeTokens(*this, Cpu, Origin, 1, P) || !MakeTokens(*this, Cpu, SuffixTokens, 2, Suffix) || !RequestReady(*this, Cpu, Spec(P), LP, TEXT("P")))
	{
		return false;
	}
	const int64 S = Origin / Rig.Geo.B; // floor(origin / B), written from the plan
	// The unsaved twin: adopt P, then kNew tokens after the suffix in one generation.
	{
		FSuperSLMSequence Seq;
		if (!Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, LP) || !GenerateComplete(*this, Cpu, Seq, Suffix, kNew, Twin))
		{
			return false;
		}
		ReturnAndDrain(Cpu, Seq);
	}
	// The child's blob: a holder that adopted P, saved mid-turn.
	TArray<uint8> Blob;
	{
		FSuperSLMSequence Seq;
		if (!Vend(*this, Cpu, Seq) || !AdoptAndWait(*this, Cpu, Seq, LP) || !CacheSaveMidTurn(*this, Cpu, Seq, GenerationRequest(Suffix, kNew), kSaveAt, Blob, Before))
		{
			return false;
		}
	}
	const int64 Pool = LedgerOf(Cpu).PoolPages;

	// One restore of the child's blob with Hint, measured by the probe; the restored holder's resumed
	// generation, after the tokens it had at the save, compared with the twin's.
	auto HintedRestore = [&](const FSuperSLMPrefix& Hint, bool bCheckPin, int64& OutDraw, int64& OutShared, const TCHAR* What)
	{
		int64 FreeBefore = -1, FreeAfter = -1;
		if (!DrainQuiet(Cpu) || !ProbeFree(*this, Cpu, Pool, FreeBefore))
		{
			return false;
		}
		bool bOk = true;
		FIssuedRestore R;
		if (bCheckPin)
		{
			const FSuperSLMPagedHold Hold = FSuperSLMPagedTestSeam::HoldApply(Cpu, ESuperSLMPagedRequestKind::Restore);
			R = IssueRestore(Cpu, Blob, Rig.Model, Hint);
			bOk &= TestTrue(*FString::Printf(TEXT("%s: the restore is held at its apply"), What), Hold.IsValid() && WaitHoldReached(Cpu, Hold));
			FSuperSLMPrefixOp Op;
			bOk &= Drop(*this, Cpu, Spec(P), ESuperSLMPrefixDropScope::Memory, Op, What);
			bOk &= TestEqual(*FString::Printf(TEXT("%s: P stays pinned by its lease for the restore (a drop is deferred)"), What),
				(uint8)WaitOpResolved(Cpu, Op).Result, (uint8)ESuperSLMPrefixOpResult::DropDeferred);
			FSuperSLMPagedTestSeam::ReleaseHold(Cpu, Hold);
		}
		else
		{
			R = IssueRestore(Cpu, Blob, Rig.Model, Hint);
		}
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the restore succeeds"), What), (uint8)SettleOutcome(Cpu, R), (uint8)ESuperSLMRestoreResult::Success);
		OutShared = Seam::LastRestoreSharedPages(Cpu);
		TArray<int32> Tokens = Before;
		bOk &= CacheContinueRestored(*this, Cpu, R.Sequence, Tokens);
		bOk &= TestEqual(*FString::Printf(TEXT("%s: the restored child's continuation equals the twin's"), What), Tokens, Twin);
		bOk &= DrainQuiet(Cpu) && ProbeFree(*this, Cpu, Pool, FreeAfter);
		OutDraw = FreeBefore - FreeAfter;
		bOk &= Form1ChargesCoverDrawn(*this, Cpu, *FString::Printf(TEXT("%s: A pre-charged for the shared pages"), What));
		ReturnAndDrain(Cpu, R.Sequence);
		return bOk;
	};

	int64 SharedDraw = -1, SharedS = -1, PrivateDraw = -1, PrivateS = -1;
	bool bOk = HintedRestore(LP, true, SharedDraw, SharedS, TEXT("P's live lease as the hint"));
	bOk &= TestEqual(TEXT("live lease: *out_shared_pages = floor(origin / B)"), SharedS, S);
	Cpu.ReturnPrefix(LP);
	DrainQuiet(Cpu);
	bOk &= HintedRestore(LP, false, PrivateDraw, PrivateS, TEXT("P's lease returned"));
	bOk &= TestEqual(TEXT("returned lease: the restore runs private (s = 0)"), PrivateS, int64(0));
	bOk &= TestEqual(*FString::Printf(TEXT("the sharing restore draws floor(origin / B) = %lld fewer pages than the private one (%lld vs %lld)"), S, SharedDraw, PrivateDraw),
		PrivateDraw - SharedDraw, S);
	return bOk;
}

// Cell 11.7, the Unreal leg (rev 1.18, PC-16 (a)): the drop's pin check and its disk-delete serialization are
// real branches, not asserts, so they hold in the shipping configuration. The cell runs 7.18's pin check (a
// drop of a leased entry is DropDeferred, the lease still adopts, the entry is evicted only once the lease
// returns) and 3.10's serialization (a MemoryAndDisk drop behind a held write-back: the write-back completes,
// then the delete; no file after, the exclusion counter never 2). It grades 11.7 only in a build with checks
// compiled out (DO_CHECK 0: the box runs it in the Test configuration); in a build with checks it runs the same
// legs, which then grade 7.18 and 3.10 only, and says so. Mutants: each guard removed -> its leg red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuperSLMPrefixCacheShippingGuardsTest, "SuperSLM.PrefixCache.Cpu.ShippingGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FSuperSLMPrefixCacheShippingGuardsTest::RunTest(const FString& Parameters)
{
#if DO_CHECK
	AddInfo(TEXT("11.7: checks are compiled in here; this run grades 7.18's and 3.10's legs only. 11.7 is graded by the run with DO_CHECK 0 (the Test configuration)."));
#endif
	FPagingRig Rig;
	FScopedCacheDirectory Dir(TEXT("shipping-guards"));
	if (!DiskRig(*this, Rig, Dir))
	{
		return false;
	}
	USuperSLMSubsystem& Cpu = *Rig.Cpu;
	TArray<int32> E, K;
	if (!MakeTokens(*this, Cpu, 300, 1, E) || !MakeTokens(*this, Cpu, 300, 2, K))
	{
		return false;
	}
	bool bOk = true;

	// 7.18's pin check.
	{
		FSuperSLMPrefix Lease;
		FSuperSLMSequence Seq;
		FSuperSLMPrefixOp Op;
		if (!RequestReady(*this, Cpu, Spec(E), Lease, TEXT("pin check")) || !Drop(*this, Cpu, Spec(E), ESuperSLMPrefixDropScope::Memory, Op, TEXT("pin check")))
		{
			return false;
		}
		const int64 EvictionsBefore = Stats(Cpu).Evictions;
		const int64 MarksBefore = Stats(Cpu).MarksApplied;
		bOk &= TestEqual(TEXT("pin check: a drop of a leased entry is DropDeferred"), (uint8)WaitOpResolved(Cpu, Op).Result, (uint8)ESuperSLMPrefixOpResult::DropDeferred);
		bOk &= TestEqual(TEXT("pin check: the entry is not evicted while leased"), Stats(Cpu).Evictions, EvictionsBefore);
		bOk &= Vend(*this, Cpu, Seq) && AdoptAndWait(*this, Cpu, Seq, Lease);
		ReturnAndDrain(Cpu, Seq);
		Cpu.ReturnPrefix(Lease);
		DrainQuiet(Cpu);
		// 7.18: the marks-applied count rises by one. A mark applied is a drop, counted apart from evictions
		// (§5.6; the core's MarksApplied), so the eviction diagnostic is unchanged (box 2026-10-21: this read
		// Evictions + 1 and failed on 0).
		bOk &= TestEqual(TEXT("pin check: evicted once the lease returns (the mark applied)"), Stats(Cpu).MarksApplied, MarksBefore + 1);
		bOk &= TestEqual(TEXT("pin check: counted as a drop, not an eviction"), Stats(Cpu).Evictions, EvictionsBefore);
	}

	// 3.10's delete serialization behind a held write-back.
	{
		FSuperSLMPrefix Lease;
		Seam::HoldWriteBacks(Cpu, true);
		if (!RequestReady(*this, Cpu, Spec(K), Lease, TEXT("serialization")))
		{
			Seam::HoldWriteBacks(Cpu, false);
			return false;
		}
		Cpu.ReturnPrefix(Lease);
		Drain(Cpu, [&Cpu]() { return Seam::HeldWriteBackCount(Cpu) == 1; }, LeaseSeconds);
		bOk &= TestEqual(TEXT("serialization: the key's write-back is held mid-write"), Seam::HeldWriteBackCount(Cpu), 1);
		FSuperSLMPrefixOp Op;
		if (!Drop(*this, Cpu, Spec(K), ESuperSLMPrefixDropScope::MemoryAndDisk, Op, TEXT("serialization")))
		{
			Seam::HoldWriteBacks(Cpu, false);
			return false;
		}
		for (int32 I = 0; I < 20; ++I)
		{
			Cpu.Tick(Step);
		}
		bOk &= TestEqual(TEXT("serialization: the drop's delete waits behind the write-back (Pending)"), (uint8)OpResultOf(Cpu, Op), (uint8)ESuperSLMPrefixOpResult::Pending);
		Seam::HoldWriteBacks(Cpu, false);
		const FSuperSLMPrefixOpResult R = WaitOpResolved(Cpu, Op);
		bOk &= TestEqual(TEXT("serialization: the memory part applied (Dropped)"), (uint8)R.Result, (uint8)ESuperSLMPrefixOpResult::Dropped);
		bOk &= TestEqual(TEXT("serialization: then the delete ran (Deleted)"), (uint8)R.Disk, (uint8)ESuperSLMPrefixDropDisk::Deleted);
		bOk &= TestFalse(TEXT("serialization: no file under the key after the op resolves"), FPaths::FileExists(DiskFileOf(Cpu, Spec(K))));
		bOk &= TestTrue(TEXT("serialization: one disk operation at a time (the exclusion counter never read 2)"), Seam::MaxDiskExclusion(Cpu) == 1);
	}
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS && SUPERSLM_WITH_L2S1_ASYNC && SUPERSLM_WITH_PREFIX_CACHE
