#pragma once

#include "CoreMinimal.h"

// §10.2 item 10 (plan §5 item 2, D-SLM7414/D-SLM7422/D-SLM7430/D-SLM7444/D-SLM7450): the
// "SuperSLM.CalibrateCosts" console command. See SuperSLMCalibrateCostsCommand.cpp for the full
// spec this command realizes. Registration happens at static-init time (FAutoConsoleCommand);
// Register() exists only to force the implementation file to link.
namespace SuperSLMCalibrateCostsCommand
{
	void Register();

	// Plan §10.3.1 items 5b and 6b: what the most recent run of the command did. Diagnostic only;
	// it never changes what the command measures or writes.
	enum class EOutcome : uint8
	{
		NotRun,            // the command has not run in this process
		MeasurementFailed, // no artifact argument, or a measurement arm failed; nothing written
		RefusedNotIdle,    // the idleness criterion below was not met (or could not be graded): the medians were NOT written
		// All nine arms measured in full and written to Saved/SuperSLM/CalibratedCosts.ini, because the
		// run met plan §10.3.1 item 5b's idleness criterion (D-SLM7778): across one series sampled
		// every 250 ms over a 2 s pre-window, the whole workload and a 2 s post-window, the foreign
		// CPU share (the box's busy time less this process's, over capacity) averaged over every
		// four consecutive samples stayed at or under 50/N percent (N logical processors), and
		// the build lock was never held by a foreign owner at any sample (own = the build wrapper
		// that launched this run, identified by its owner.txt token, D-SLM7788). Load inside this process that is not the
		// calibration is subtracted with it (item 5b's stated residual; each sample's own share is logged).
		Written,
		WriteFailed,       // measured and idle, but the file could not be written
	};
	SUPERSLMUNREAL_API EOutcome GetLastOutcome();
}
