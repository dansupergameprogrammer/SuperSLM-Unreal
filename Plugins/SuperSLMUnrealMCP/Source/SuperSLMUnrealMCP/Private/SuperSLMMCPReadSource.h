#pragma once

#include "CoreMinimal.h"

class UDataTable;

// One record a read source yields: a stable key (the output row it becomes) and the text the model
// reads for it.
struct FSuperSLMMCPSourceRecord
{
	FName Key;
	FString Text;
};

// The action tier's pluggable read source, reading from project data first. Constrained inference
// takes a read source, a prompt and a schema. The source is the counterpart of the
// write sink (SuperSLMMCPWriteSink.h): a source yields records from project data, each record runs
// through the model with the caller's prompt, and the sink writes one row per record, so the pair
// forms a read-transform-write loop over project data. Game thread only: a source reads UObject
// properties. It takes a snapshot when read; the rest of the run never touches the source again.
class ISuperSLMMCPReadSource
{
public:
	virtual ~ISuperSLMMCPReadSource() = default;

	// The source's path, for the tool's result.
	virtual FString GetSourcePath() const = 0;

	// Every record, in the source's own order. False, with OutError naming the record, when a record
	// cannot be read; nothing is returned then.
	virtual bool Read(TArray<FSuperSLMMCPSourceRecord>& OutRecords, FString& OutError) const = 0;
};

namespace SuperSLMMCPReadSource
{
	// The first source: one column of a UDataTable, one record per row, keyed by the row name, in
	// the table's row order. The column is a property of the row struct, matched by authored name,
	// ignoring case. A string, name or text property is read as its text. A numeric, bool or enum
	// property is read as the text the property exports (an enum as its name). Any other property
	// type is refused by name, as is a row whose value is empty.
	TUniquePtr<ISuperSLMMCPReadSource> MakeDataTableColumnSource(UDataTable* Table, const FString& Column, FString& OutError);
}
