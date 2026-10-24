#pragma once

// Preprocessed out unless SUPERSLM_WITH_PREFIX_CACHE is on.
//
// The editor half of cooked prefix sets (precache plan §5.8):
//   - the asset's Content Browser identity and its Build Prefixes action (the context menu of a
//     SuperSLM Prefix Set), which starts one background build (FSuperSLMPrefixSetBuildAction) per
//     selected set: the prefills run off the game thread behind a progress notification with a Cancel
//     button (§5.8 *The build runs in the background*, PC-15 Q9; cell 3.11);
//   - cook validation: a pre-save hook that, when the save is a cook, validates every prefix set
//     being cooked and logs one warning per finding, naming the set and the entry. The stale bytes
//     still ship; the runtime refuses or never finds them and recomputes (§5.8, cell 2.9).
// The commandlet (-run=SuperSLMBuildPrefixes) is SuperSLMBuildPrefixesCommandlet.h.

#include "CoreMinimal.h"
#include "SuperSLMPrefixCacheTypes.h"

#if SUPERSLM_WITH_PREFIX_CACHE

#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "SuperSLMPrefixSetTools.h"
#include "UObject/StrongObjectPtr.h"

class SNotificationItem;
class USuperSLMPrefixSet;

DECLARE_LOG_CATEGORY_EXTERN(LogSuperSLMPrefixSetCook, Log, All);

namespace SuperSLMPrefixSetEditor
{
	// Module startup and shutdown: the asset type, the menu entry, the cook hook.
	void Register();
	void Unregister();

	// What the cook hook runs for one set: validates and logs one warning per finding (an error line
	// when the set cannot be read). Returns the findings, for the cells and the commandlet.
	TArray<FSuperSLMPrefixSetFinding> ValidateAtCook(const USuperSLMPrefixSet& Set);

	// The build action on one set: builds, logs every finding as a warning, and an error when the
	// build is refused. True when the set was built.
	bool BuildAndLog(USuperSLMPrefixSet& Set, TArray<FSuperSLMPrefixSetFinding>& OutFindings, FString& OutError);

	void LogFindings(const TArray<FSuperSLMPrefixSetFinding>& Findings);

	// The Build Prefixes action on one set, in the background (§5.8, rev 1.18.1; cell 3.11). Start runs the
	// build's first phase on the game thread (the tokenize pass, the manifest, the model's mapping), launches the
	// entries' prefills, freezes and saves on a worker thread, and returns. A progress notification shows entries
	// done of the total and carries a Cancel button. Pump, on the game thread (a core ticker calls it; a cell may
	// call it directly), refreshes the notification and, once the worker has returned, finishes: one replacement
	// of the set's CPU build (Stored), nothing on a cancel (Cancelled: the previous build's bytes as they were), or
	// nothing because the set was edited while it ran (Refused). The findings are logged as BuildAndLog logs them.
	// An action that is released or shut down unfinished is cancelled, waits for its worker, and finishes.
	class FSuperSLMPrefixSetBuildAction : public TSharedFromThis<FSuperSLMPrefixSetBuildAction>
	{
	public:
		// Null, with OutError (also logged), when the build is refused at its first phase or the set already
		// has a build running. TestPauseAfterEntries >= 0 (automation builds only) holds the worker after that
		// many entries until a cancel.
		static TSharedPtr<FSuperSLMPrefixSetBuildAction> Start(USuperSLMPrefixSet& Set, FString& OutError, int32 TestPauseAfterEntries = -1);

		~FSuperSLMPrefixSetBuildAction();

		void Pump();
		void Cancel();
		bool IsFinished() const { return bFinished; }
		SuperSLMPrefixSetTools::EBuildFinish GetResult() const { return Result; }
		SuperSLMPrefixSetTools::FBuildProgress GetProgress() const;
		const TArray<FSuperSLMPrefixSetFinding>& GetFindings() const { return Findings; }
		const FString& GetError() const { return Error; }

		// Module shutdown: cancels every running action and finishes it.
		static void CancelAll();

	private:
		FSuperSLMPrefixSetBuildAction() = default;
		void Finish();
		void UpdateNotification(const SuperSLMPrefixSetTools::FBuildProgress& Progress);

		TStrongObjectPtr<USuperSLMPrefixSet> Set;
		FString SetName;
		TSharedPtr<SuperSLMPrefixSetTools::FPrefixSetBuildJob, ESPMode::ThreadSafe> Job;
		TFuture<void> Worker;
		FTSTicker::FDelegateHandle TickerHandle;
		TSharedPtr<SNotificationItem> Notification;
		SuperSLMPrefixSetTools::FBuildProgress LastProgress;
		SuperSLMPrefixSetTools::EBuildFinish Result = SuperSLMPrefixSetTools::EBuildFinish::Refused;
		TArray<FSuperSLMPrefixSetFinding> Findings;
		FString Error;
		bool bFinished = false;
	};
}

#endif // SUPERSLM_WITH_PREFIX_CACHE
