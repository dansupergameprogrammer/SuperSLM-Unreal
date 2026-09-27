#include "SuperSLMIntegrity.h"

#include "superslm/sha256.h"

namespace SuperSLM
{
	void ComputeArtifactDigest(const uint8* Data, int64 SizeBytes, uint8 OutDigest[32])
	{
		superslm::Sha256Hash(Data, static_cast<size_t>(SizeBytes), OutDigest);
	}
}
