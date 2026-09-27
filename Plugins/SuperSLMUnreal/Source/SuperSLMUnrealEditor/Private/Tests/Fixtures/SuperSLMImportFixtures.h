#pragma once

// T-2226 (L2-S0 red suite). Loads the committed `.sslm` byte fixtures under
// Tests/Fixtures/SSLM/ (see that directory's PROVENANCE.md for how each was generated and
// which check each one is built to trip). This header is NOT production code: it lives
// under Private/Tests/, compiles only inside WITH_DEV_AUTOMATION_TESTS translation units,
// and is read by no runtime or editor module path.

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace SuperSLMImportFixtures
{
	inline FString FixtureDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperSLMUnreal"));
		checkf(Plugin.IsValid(), TEXT("SuperSLMUnreal plugin not found by IPluginManager -- "
			"the .uplugin this suite ships is not discovered, which is itself part of L2-S0's "
			"red state until the plugin is enabled in a host project"));
		return Plugin->GetBaseDir() / TEXT("Source/SuperSLMUnrealEditor/Private/Tests/Fixtures/SSLM");
	}

	// One committed .sslm fixture plus the check it is built to trip. `ExpectedStatusName` and
	// `ExpectedSectionIndex` are cross-checked against PROVENANCE.md's table and the independent
	// Python re-implementation of superslm::SslmArtifact::OpenFromMemory's check order run at
	// authoring time (2026-08-22) -- not against the plugin's own implementation, which does not
	// exist yet.
	struct FFixtureCase
	{
		FString FileName;
		FString ExpectedStatusName; // "" for the one case that must be ACCEPTED
		int32 ExpectedSectionIndex; // INDEX_NONE (kNoSection) unless the check names a row
	};

	inline const FFixtureCase& ValidReference()
	{
		static const FFixtureCase Case{TEXT("l2s0_valid_reference.sslm"), TEXT(""), INDEX_NONE};
		return Case;
	}
	inline const FFixtureCase& BadMagic()
	{
		static const FFixtureCase Case{TEXT("l2s0_bad_magic.sslm"), TEXT("BadMagic"), INDEX_NONE};
		return Case;
	}
	inline const FFixtureCase& UnknownFlagBit()
	{
		static const FFixtureCase Case{TEXT("l2s0_unknown_flag_bit.sslm"), TEXT("BadHeader"), INDEX_NONE};
		return Case;
	}
	inline const FFixtureCase& Truncated()
	{
		static const FFixtureCase Case{TEXT("l2s0_truncated.sslm"), TEXT("FileSizeMismatch"), INDEX_NONE};
		return Case;
	}
	inline const FFixtureCase& HashMismatch()
	{
		static const FFixtureCase Case{TEXT("l2s0_hash_mismatch.sslm"), TEXT("IntegrityMismatch"), INDEX_NONE};
		return Case;
	}
	inline const FFixtureCase& OverlappingSections()
	{
		// Row 4 (WeightScales) or row 5 (CompositionConstants) -- the loader's sorted-neighbour
		// overlap check (artifact.cpp:388) may name either index of the colliding pair depending
		// on sort order; the test accepts either 4 or 5, never a third value (see IA-5).
		static const FFixtureCase Case{TEXT("l2s0_overlapping_sections.sslm"), TEXT("SectionOverlap"), 4};
		return Case;
	}

	inline bool LoadBytes(const FFixtureCase& Case, TArray<uint8>& OutBytes)
	{
		const FString Path = FixtureDir() / Case.FileName;
		return FFileHelper::LoadFileToArray(OutBytes, *Path);
	}

	inline FString AbsolutePath(const FFixtureCase& Case)
	{
		return FixtureDir() / Case.FileName;
	}
}
