#include "SuperSLMModelImport.h"
#include "SuperSLMModel.h"

#include "HAL/UnrealMemory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#include "superslm/artifact.h"
#include "superslm/model.h"

namespace
{
	// §3, v1.2.0 new cell (a): an artifact header `flags` bit outside
	// superslm::kKnownArtifactFlagsMask rejects as BadHeader with Layer 1's own raw message
	// ("flags has unknown bit(s) set: ..."). This plugin's own diagnostic states the true
	// remedy instead -- a newer SuperSLM than this plugin pins -- because "corrupt" and
	// "requires a newer SuperSLM" have different remedies and Layer 1's own message text does
	// not distinguish them for a product-facing reader.
	bool IsUnknownFlagBitRejection(const superslm::SslmError& Err)
	{
		if (Err.code != superslm::SslmStatus::BadHeader)
		{
			return false;
		}
		const FString Message(Err.message.c_str());
		return Message.Contains(TEXT("flags has unknown bit"));
	}

	FSuperSLMImportDiagnostic DiagnosticFromArtifactError(const superslm::SslmError& Err)
	{
		FSuperSLMImportDiagnostic Diagnostic;
		Diagnostic.bAccepted = false;
		Diagnostic.SectionIndex = (Err.section_index == superslm::kNoSection)
			? INDEX_NONE
			: static_cast<int32>(Err.section_index);
		Diagnostic.StatusName = FString(superslm::SslmStatusName(Err.code));

		if (IsUnknownFlagBitRejection(Err))
		{
			Diagnostic.Message = FString::Printf(
				TEXT("This artifact requires a newer SuperSLM than this plugin pins (%s)"),
				*FString(Err.message.c_str()));
		}
		else
		{
			Diagnostic.Message = FString(Err.message.c_str());
		}
		return Diagnostic;
	}

	// The artifact's embedded Provenance section (type 1), transported as raw JSON text --
	// see SuperSLMModel.h's ProvenanceJson comment. Empty string if absent.
	FString ExtractProvenanceJson(const superslm::SslmArtifact& Artifact)
	{
		if (const superslm::SslmSectionView* Section = Artifact.Section(superslm::SslmSectionType::Provenance))
		{
			if (Section->byte_size > 0 && Section->data != nullptr)
			{
				// T-2249 confirmation review N2: Section->data/byte_size is a length-delimited
				// slice of the artifact's own hostile-input bytes -- Layer 1 validates the
				// RANGE is in bounds (SslmSectionView is only ever constructed after that
				// check) but never guarantees a trailing NUL terminator, and this section's
				// content is caller-supplied JSON text with no such guarantee of its own.
				// FString(FUTF8ToTCHAR(ptr, len).Get()) is wrong here: .Get() returns a
				// pointer the conversion buffer does NOT promise is null-terminated when
				// constructed from an explicit length (StringConv.h's own documented
				// caveat), and handing that pointer to FString(const TCHAR*) calls Strlen on
				// it -- an unbounded read that continues past Section->byte_size until it
				// happens to find a zero byte, which can run off the end of the artifact's
				// FMemory::Malloc allocation entirely on a section placed last in the file.
				// ConstructFromPtrSize is the length-honest API: it consumes exactly
				// `Size` UTF-8 code units and never scans for a terminator, so a
				// non-terminated, embedded-NUL, or maximum-length section is bounded by the
				// declared byte_size and nothing else -- matching how the artifact's own
				// loader already treats every other field as hostile input (§3).
				return FString::ConstructFromPtrSize(
					reinterpret_cast<const UTF8CHAR*>(Section->data),
					static_cast<int32>(Section->byte_size));
			}
		}
		return FString();
	}
}

USuperSLMModel* FSuperSLMModelImport::ImportFromFile(
	const FString& AbsolutePath,
	FSuperSLMImportDiagnostic& OutDiagnostic)
{
	return ImportFromFileInternal(AbsolutePath, nullptr, NAME_None, RF_NoFlags, OutDiagnostic);
}

USuperSLMModel* FSuperSLMModelImport::ImportFromFile(
	const FString& AbsolutePath,
	UObject* InOuter,
	FName InName,
	FSuperSLMImportDiagnostic& OutDiagnostic)
{
	return ImportFromFileInternal(AbsolutePath, InOuter, InName, RF_Public | RF_Standalone, OutDiagnostic);
}

USuperSLMModel* FSuperSLMModelImport::ImportFromFileInternal(
	const FString& AbsolutePath,
	UObject* InOuter,
	FName InName,
	EObjectFlags InFlags,
	FSuperSLMImportDiagnostic& OutDiagnostic)
{
	// The two halves back to back (MCP sibling Q13 split them; see the header).
	FSuperSLMPreparedModelImport Prepared;
	if (!PrepareFromFile(AbsolutePath, Prepared, OutDiagnostic))
	{
		return nullptr;
	}
	return CreateFromPreparedInternal(MoveTemp(Prepared), InOuter, InName, InFlags, OutDiagnostic);
}

bool FSuperSLMModelImport::PrepareFromFile(const FString& AbsolutePath, FSuperSLMPreparedModelImport& OutPrepared, FSuperSLMImportDiagnostic& OutDiagnostic)
{
	// Any thread: nothing below touches a UObject (the object's name and NewObject moved to
	// CreateFromPrepared()).
	OutDiagnostic = FSuperSLMImportDiagnostic();
	OutPrepared = FSuperSLMPreparedModelImport();

	// FileBytes (copy 1 of the artifact's bytes -- unavoidable: it is the raw file read that
	// every check below and the final cooked buffer are derived from) -- T-2241 review S3.
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *AbsolutePath))
	{
		OutDiagnostic.bAccepted = false;
		OutDiagnostic.SectionIndex = INDEX_NONE;
		OutDiagnostic.StatusName = TEXT("IoError");
		OutDiagnostic.Message = FString::Printf(TEXT("Could not read file: %s"), *AbsolutePath);
		return false;
	}

	// Layer 1's public C++ throw contract (src/bad_alloc_wrap.h, WrapBadAllocContract): every
	// entry point below narrows any exception to std::bad_alloc and rethrows it unchanged.
	// T-2241 review S1: this path previously had no try/catch at all, so an allocation
	// failure on a multi-gigabyte artifact would leave the plugin's diagnostic ladder
	// entirely and propagate into UE code compiled without exception support upstream of
	// this module. SSLM_ALLOCATION_FAILED is the ABI's own name for exactly this cause.
	try
	{
		FString ProvenanceJson;

		// Artifact-level validation on the C++ path (D-SLM3812 RULING): magic, format
		// version, header flags, section-table bounds/alignment/overlap/dtype, and the
		// whole-file SHA-256 integrity hash -- superslm::SslmArtifact::OpenFromMemory is the
		// same check sequence sslm_model_map performs, and it is the surface that carries a
		// section index and a message (superslm::SslmError) rather than the single collapsed
		// SSLM_ARTIFACT_REJECTED the C ABI alone would report (§3).
		//
		// Scoped to its own block (T-2241 review S3): Artifact owns a second full copy of the
		// bytes (artifact.h: "On Ok, `out` owns a copy of the bytes") and is read for nothing
		// beyond the status check and the Provenance section extracted here, so it is
		// destroyed before SslmModel::Load's own copy (below) is ever made -- the two
		// Layer-1-owned copies are never live at the same time.
		{
			superslm::SslmArtifact Artifact;
			superslm::SslmError ArtifactErr;
			const superslm::SslmStatus ArtifactStatus = superslm::SslmArtifact::OpenFromMemory(
				FileBytes.GetData(), static_cast<size_t>(FileBytes.Num()), Artifact, &ArtifactErr);

			if (ArtifactStatus != superslm::SslmStatus::Ok)
			{
				OutDiagnostic = DiagnosticFromArtifactError(ArtifactErr);
				return false;
			}

			ProvenanceJson = ExtractProvenanceJson(Artifact);
		}
		// Artifact is destroyed here.

		// Deeper per-section semantic validation (config, tensor manifests, calibration,
		// schema masks, etc.) -- "Import validation is sslm_model_map followed immediately by
		// sslm_model_unmap ... because the map call IS the full audit" (§3, D-SLM3532).
		// SslmModel::Load re-runs OpenFromMemory internally; having already passed it above,
		// this call validates the layer the artifact-level check above does not reach.
		//
		// Scoped to its own block for the same reason as Artifact above: View is read for
		// nothing beyond the status check (L2-S0 makes no decode call, so nothing in View is
		// consumed yet), so it is destroyed before the final aligned allocation runs.
		superslm::SslmModelStatus LoadStatus;
		{
			superslm::SslmModelView View;
			std::string LoadErrString;
			LoadStatus = superslm::SslmModel::Load(
				FileBytes.GetData(), static_cast<size_t>(FileBytes.Num()), View, &LoadErrString);

			if (LoadStatus != superslm::SslmModelStatus::Ok)
			{
				// T-2241 review S2: the specific SslmModelStatus, not one collapsed string --
				// SslmModelStatusName mirrors SslmStatusName's role one layer down, and
				// FSuperSLMImportDiagnostic's own header states the struct routes Layer 1's
				// real taxonomy through rather than inventing a second one; the model-load
				// stage owes that exactly as much as the artifact stage does.
				OutDiagnostic.bAccepted = false;
				OutDiagnostic.SectionIndex = INDEX_NONE;
				OutDiagnostic.StatusName = FString(superslm::SslmModelStatusName(LoadStatus));
				OutDiagnostic.Message = FString(LoadErrString.c_str());
				return false;
			}
		}
		// View is destroyed here, before the aligned allocation below.

		// Accepted: cook the artifact bytes into a CookedArtifactAlignment-aligned,
		// in-place-mappable buffer (§3, §10 L2-S0 gate) -- the SAME alignment constant
		// USuperSLMModel::Serialize() uses on load, so an imported and a reloaded instance
		// are indistinguishable to GetMappedArtifactData()'s caller. Owned by the returned
		// USuperSLMModel's shared FSuperSLMArtifactBytes (review S5), freed when the asset and
		// every pin have released it.
		const int64 Size = FileBytes.Num();
		void* AlignedData = FMemory::Malloc(
			static_cast<SIZE_T>(Size), static_cast<uint32>(USuperSLMModel::CookedArtifactAlignment));
		FMemory::Memcpy(AlignedData, FileBytes.GetData(), static_cast<SIZE_T>(Size));

		// FileBytes' own heap buffer is no longer needed once the aligned copy exists
		// (T-2241 review S3) -- released explicitly rather than left live to the end of scope.
		FileBytes.Empty();

		OutPrepared.Bytes = MakeShared<FSuperSLMArtifactBytes, ESPMode::ThreadSafe>(AlignedData, Size);
		OutPrepared.ProvenanceJson = MoveTemp(ProvenanceJson);
		OutPrepared.SourcePath = AbsolutePath;

		OutDiagnostic.bAccepted = true;
		OutDiagnostic.SectionIndex = INDEX_NONE;
		OutDiagnostic.StatusName = TEXT("Ok");
		OutDiagnostic.Message = TEXT("");
		return true;
	}
	catch (const std::bad_alloc&)
	{
		OutDiagnostic.bAccepted = false;
		OutDiagnostic.SectionIndex = INDEX_NONE;
		OutDiagnostic.StatusName = TEXT("SSLM_ALLOCATION_FAILED");
		OutDiagnostic.Message = FString::Printf(
			TEXT("Out of memory while loading artifact: %s"), *AbsolutePath);
		return false;
	}
}

USuperSLMModel* FSuperSLMModelImport::CreateFromPrepared(FSuperSLMPreparedModelImport&& Prepared, FSuperSLMImportDiagnostic& OutDiagnostic)
{
	return CreateFromPreparedInternal(MoveTemp(Prepared), nullptr, NAME_None, RF_NoFlags, OutDiagnostic);
}

USuperSLMModel* FSuperSLMModelImport::CreateFromPrepared(
	FSuperSLMPreparedModelImport&& Prepared,
	UObject* InOuter,
	FName InName,
	FSuperSLMImportDiagnostic& OutDiagnostic)
{
	return CreateFromPreparedInternal(MoveTemp(Prepared), InOuter, InName, RF_Public | RF_Standalone, OutDiagnostic);
}

USuperSLMModel* FSuperSLMModelImport::CreateFromPreparedInternal(
	FSuperSLMPreparedModelImport&& Prepared,
	UObject* InOuter,
	FName InName,
	EObjectFlags InFlags,
	FSuperSLMImportDiagnostic& OutDiagnostic)
{
	// Game thread, or under a GC guard: the name and NewObject only.
	if (!Prepared.IsValid())
	{
		OutDiagnostic = FSuperSLMImportDiagnostic();
		OutDiagnostic.bAccepted = false;
		OutDiagnostic.SectionIndex = INDEX_NONE;
		OutDiagnostic.StatusName = TEXT("NotPrepared");
		OutDiagnostic.Message = TEXT("CreateFromPrepared() was given no prepared bytes (PrepareFromFile() did not succeed, or they were already consumed)");
		return nullptr;
	}
	UObject* const Outer = (InOuter != nullptr) ? InOuter : GetTransientPackage();
	const FName Name = (InName != NAME_None) ? InName : MakeUniqueObjectName(Outer, USuperSLMModel::StaticClass());

	USuperSLMModel* Model = NewObject<USuperSLMModel>(Outer, USuperSLMModel::StaticClass(), Name, InFlags);
	Model->SetArtifactBytes(MoveTemp(Prepared.Bytes));
	Model->ProvenanceJson = MoveTemp(Prepared.ProvenanceJson);
	Prepared = FSuperSLMPreparedModelImport();

	OutDiagnostic = FSuperSLMImportDiagnostic();
	OutDiagnostic.bAccepted = true;
	OutDiagnostic.SectionIndex = INDEX_NONE;
	OutDiagnostic.StatusName = TEXT("Ok");
	OutDiagnostic.Message = TEXT("");
	return Model;
}
