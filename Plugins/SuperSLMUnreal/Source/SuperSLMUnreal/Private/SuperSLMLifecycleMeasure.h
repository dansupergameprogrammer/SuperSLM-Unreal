#pragma once

#include "CoreMinimal.h"

// The two host-memory measurements the Sequence Lifecycle Budget rests on (§5): whole-buffer
// write bandwidth (the cost shape of sslm_seq_reset) and whole-buffer copy bandwidth (the cost
// shape of sslm_seq_adopt_prefix's copy-on-adopt). Measured each call; never a shipped constant.
namespace SuperSLMLifecycleMeasure
{
	struct FBandwidths
	{
		double WriteBytesPerSec = 0.0;
		double CopyBytesPerSec = 0.0;
	};

	FBandwidths MeasureHostBandwidths();
}
