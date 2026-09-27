#pragma once

#include "CoreMinimal.h"
#include "SuperSLMImportDiagnostic.h"
#include "Templates/SharedPointer.h"

class FSuperSLMArtifactBytes;
class USuperSLMModel;

// An import's validated bytes, before any UObject exists (L2-S3 fold; the MCP sibling's
// open question Q13): the CookedArtifactAlignment-aligned copy of an accepted `.sslm` and its
// Provenance text, produced by FSuperSLMModelImport::PrepareFromFile() on any thread and consumed
// by CreateFromPrepared(). Move-only in practice: CreateFromPrepared() takes the bytes.
struct SUPERSLMUNREAL_API FSuperSLMPreparedModelImport
{
	TSharedPtr<FSuperSLMArtifactBytes, ESPMode::ThreadSafe> Bytes;
	FString ProvenanceJson;
	FString SourcePath;

	bool IsValid() const { return Bytes.IsValid(); }
};

// L2-S0 (the plan §3, §10). The `.sslm` model-asset import path.
//
// "Import validation is sslm_model_map followed immediately by sslm_model_unmap -- there is
// no separate validate verb and none is owed, because the map call IS the full audit"
// (§3, D-SLM3532). This entry point links Layer 1's C++ path directly (D-SLM3812 RULING) --
// superslm::SslmArtifact::OpenFromMemory for the artifact-level ladder (magic, size, header
// flags, section table, integrity hash), which is the surface that carries a section index
// and a message rather than the single collapsed SSLM_ARTIFACT_REJECTED the C ABI alone
// would report -- followed by superslm::SslmModel::Load for the deeper per-section semantic
// validation sslm_model_map performs internally.
class SUPERSLMUNREAL_API FSuperSLMModelImport
{
public:
	// Imports the `.sslm` at AbsolutePath into the transient package. Every call site this
	// suite committed before T-2241's review uses this exact shape; preserved unchanged.
	static USuperSLMModel* ImportFromFile(const FString& AbsolutePath, FSuperSLMImportDiagnostic& OutDiagnostic);

	// Imports the `.sslm` at AbsolutePath directly into InOuter/InName (T-2241 review C1) --
	// USuperSLMModelFactory::FactoryCreateFile and SuperSLMSaveReloadTests.cpp's C1 proof cell
	// both use this shape, matching the (Path, Outer, Name, ..., OutDiagnostic) parameter order
	// of a sibling plugin in the project's private history (not published here), so the two
	// plugins' import entry points stay consistent. Created with RF_Public |
	// RF_Standalone so the returned object is saveable into InOuter's package.
	static USuperSLMModel* ImportFromFile(
		const FString& AbsolutePath,
		UObject* InOuter,
		FName InName,
		FSuperSLMImportDiagnostic& OutDiagnostic);

	// --- The same import in two halves (L2-S3 fold; MCP sibling Q13) ---
	//
	// ImportFromFile() above reads, validates (the artifact ladder's whole-file SHA-256, then
	// SslmModel::Load) and copies the file, then calls NewObject, all in one call -- so a caller
	// running it off the game thread must hold a GC guard (FGCScopeGuard) across the whole import,
	// because creating a UObject while GC runs is fatal. Split, the guard covers only the second
	// half:
	//
	// PrepareFromFile(): ANY thread, touches no UObject and needs no GC guard. Everything heavy:
	// the read, both validation passes and the aligned copy. False with OutDiagnostic filled on a
	// rejection, exactly as ImportFromFile() reports it; true with OutPrepared holding the bytes.
	static bool PrepareFromFile(const FString& AbsolutePath, FSuperSLMPreparedModelImport& OutPrepared, FSuperSLMImportDiagnostic& OutDiagnostic);

	// CreateFromPrepared(): the short second half -- the object's name, NewObject and the hand-off
	// of the prepared bytes; no file I/O and no validation. On the game thread, or on another thread
	// under a GC guard. Consumes Prepared (its bytes move into the model). The first form creates
	// in the transient package, as ImportFromFile(Path, Diagnostic) does; the second in
	// InOuter/InName with RF_Public | RF_Standalone, as ImportFromFile(Path, Outer, Name, Diagnostic)
	// does. Null, with OutDiagnostic filled, when Prepared holds no bytes.
	// Review round 2, R2-N5: off the game thread, the returned object is referenced by nothing once
	// the GC guard drops (the transient form has no RF_Standalone), so a GC between the guard's
	// release and the game thread's use can collect it. A caller off the game thread must root it
	// (AddToRoot(), or a TStrongObjectPtr created) before releasing the guard.
	static USuperSLMModel* CreateFromPrepared(FSuperSLMPreparedModelImport&& Prepared, FSuperSLMImportDiagnostic& OutDiagnostic);
	static USuperSLMModel* CreateFromPrepared(
		FSuperSLMPreparedModelImport&& Prepared,
		UObject* InOuter,
		FName InName,
		FSuperSLMImportDiagnostic& OutDiagnostic);

private:
	static USuperSLMModel* CreateFromPreparedInternal(
		FSuperSLMPreparedModelImport&& Prepared,
		UObject* InOuter,
		FName InName,
		EObjectFlags InFlags,
		FSuperSLMImportDiagnostic& OutDiagnostic);

	static USuperSLMModel* ImportFromFileInternal(
		const FString& AbsolutePath,
		UObject* InOuter,
		FName InName,
		EObjectFlags InFlags,
		FSuperSLMImportDiagnostic& OutDiagnostic);
};
