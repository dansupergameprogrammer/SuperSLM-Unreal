#pragma once

// Where the real-artifact cells find their large inputs. These are models and converted artifacts
// that are too large to commit, so a machine running the suite says where they are through two
// environment variables:
//
//   SUPERSLM_HF_CACHE       the model cache root. It holds `hub/` (Hugging Face checkpoint
//                           snapshots) and `superslm_artifacts/` (converted .sslm artifacts).
//   SUPERSLM_ARTIFACTS_DIR  the root that holds `superslm/aex/` (the rebuilt A-EX artifact).
//
// When a variable is unset, the path returned is a relative one that starts with
// `<VARIABLE>-is-unset/`. No file exists there, so the cell that needs it fails at its import or
// existence check, and the path in the message names the variable to set. A machine without the
// files at all behaves the same way.

#include "CoreMinimal.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

namespace SuperSLMTestDataPaths
{
	inline FString UnderEnvRoot(const TCHAR* Variable, const TCHAR* Relative)
	{
		const FString Root = FPlatformMisc::GetEnvironmentVariable(Variable);
		return Root.IsEmpty()
			? FString::Printf(TEXT("%s-is-unset/%s"), Variable, Relative)
			: FPaths::Combine(Root, Relative);
	}

	// A path under SUPERSLM_HF_CACHE, e.g. HfCache(TEXT("superslm_artifacts/model.sslm")).
	inline FString HfCache(const TCHAR* Relative) { return UnderEnvRoot(TEXT("SUPERSLM_HF_CACHE"), Relative); }

	// A path under SUPERSLM_ARTIFACTS_DIR, e.g. ArtifactsDir(TEXT("superslm/aex/x.sslm")).
	inline FString ArtifactsDir(const TCHAR* Relative) { return UnderEnvRoot(TEXT("SUPERSLM_ARTIFACTS_DIR"), Relative); }
}
