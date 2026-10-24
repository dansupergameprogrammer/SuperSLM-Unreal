// See SuperSLMPrefixSet.h.

#include "SuperSLMPrefixSet.h"

#include "SuperSLMLog.h"
#include "SuperSLMModel.h"
#include "SuperSLMPrefixCacheTypes.h"
#include "SuperSLMPrefixSetTools.h"

// Every rule below is SuperSLMPrefixSetTools' (and through it the host-tested core's); this file only
// forwards, so the reflected patch stays thin.

bool USuperSLMPrefixSet::FindEntrySpec(const USuperSLMModel* ConfiguredModel, FName Entry, FSuperSLMPrefixSpec& Out, FName& OutParentEntry) const
{
	FName Reason;
	if (SuperSLMPrefixSetTools::FindEntrySpec(*this, ConfiguredModel, Entry, Out, OutParentEntry, Reason))
	{
		return true;
	}
	UE_LOG(LogSuperSLM, Log, TEXT("PrefixSet: %s: FindEntrySpec('%s') refused: %s"), *GetPathName(), *Entry.ToString(), *Reason.ToString());
	return false;
}

void USuperSLMPrefixSet::Serialize(FArchive& Ar)
{
#if WITH_EDITOR
	// The cook's copy (§5.8 *Staleness*): the cooked package carries each TextChanged entry with no ids and no
	// bytes. The arrays are swapped for the save and swapped back, so the object, and the source package, keep
	// what they had.
	if (Ar.IsSaving() && Ar.IsCooking())
	{
		TArray<FSuperSLMPrefixSetEntry> CookedEntries;
		TArray<FSuperSLMPrefixSetBuild> CookedBuilds;
		if (SuperSLMPrefixSetTools::CookedImage(*this, CookedEntries, CookedBuilds) > 0)
		{
			Swap(Entries, CookedEntries);
			Swap(Builds, CookedBuilds);
			Super::Serialize(Ar);
			Swap(Entries, CookedEntries);
			Swap(Builds, CookedBuilds);
			return;
		}
	}
#endif
	Super::Serialize(Ar);
}

#if WITH_EDITOR
void USuperSLMPrefixSet::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	const FName Member = Event.GetMemberPropertyName();
	const FName Property = Event.GetPropertyName();
	if (Member == GET_MEMBER_NAME_CHECKED(USuperSLMPrefixSet, Model))
	{
		SuperSLMPrefixSetTools::OnModelEdited(*this);
	}
	else if (Member == GET_MEMBER_NAME_CHECKED(USuperSLMPrefixSet, Entries)
		&& (Property == GET_MEMBER_NAME_CHECKED(FSuperSLMPrefixSetEntry, Text) || Property == GET_MEMBER_NAME_CHECKED(FSuperSLMPrefixSetEntry, Input)))
	{
		const int32 Index = Event.GetArrayIndex(GET_MEMBER_NAME_CHECKED(USuperSLMPrefixSet, Entries).ToString());
		SuperSLMPrefixSetTools::OnEntryTextEdited(*this, Index);
	}
}
#endif
