#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UDataTable;

// One decoded, schema-constrained result bound for a sink: the name it is written under and the
// decoded JSON object's fields.
struct FSuperSLMMCPSinkRecord
{
	FName RowName;
	TSharedPtr<FJsonObject> Fields;
};

// The action tier's pluggable write sink: a transactional, undoable, schema-validated edit of
// project data, UDataTable rows first. A sink validates every record before it writes any, so a
// rejected record is a rejected call, never a partial write. It then applies all of them as one
// editor transaction of its own, so one undo restores the target exactly. Game thread only: both
// halves read or edit UObjects.
class ISuperSLMMCPWriteSink
{
public:
	virtual ~ISuperSLMMCPWriteSink() = default;

	// The target's path, for the tool's result.
	virtual FString GetTargetPath() const = 0;

	// Checks every record against the target's own shape. False, with OutError naming the
	// record and the field, on the first mismatch; nothing is written.
	virtual bool Validate(const TArray<FSuperSLMMCPSinkRecord>& Records, FString& OutError) = 0;

	// Writes every record (already validated) inside one FScopedTransaction of its own. Refused,
	// with the open transaction named, when another transaction is open, so the write never joins
	// the user's undo step. Callers that finish on a later frame than they started use
	// SuperSLMMCPWriteSink::ApplyWhenNoTransactionOpen, which waits for that transaction to close.
	virtual bool Apply(const TArray<FSuperSLMMCPSinkRecord>& Records, FString& OutError) = 0;
};

namespace SuperSLMMCPWriteSink
{
	// The first sink: rows of a UDataTable (resolved by the caller; see
	// SuperSLMMCPInference::ResolveObjectAsync). Null, or a table with no row struct, is refused. A row is added, or replaced when a
	// row of that name exists. Every decoded field must name a property of the table's row struct
	// (matched case-insensitively on the property's authored name; a bool property's leading "b"
	// may be omitted) and carry a value of that property's type: a JSON string for a string, name,
	// text or enum property (an enum value must be one of the enum's names), a JSON bool for a
	// bool, a JSON number for a numeric property (integral for an integer one). A decoded field
	// with no such property, or with the wrong type, rejects the call. Row-struct properties the
	// schema does not decode keep their defaults.
	TUniquePtr<ISuperSLMMCPWriteSink> MakeDataTableRowSink(UDataTable* Table, FString& OutError);

	// How long a finished run's write waits for the user's open editor transaction (a viewport
	// gizmo drag, a details-panel slider drag) to close before it is refused. Declared.
	constexpr double kMaxWriteDeferSeconds = 120.0;

	// Game thread. True when an editor transaction is open; OutName names it.
	bool IsTransactionOpen(FString& OutName);

	// Game thread. Applies Records (already validated by Sink) now when no editor transaction is
	// open. Otherwise the write waits, and is tried on each later frame until none is open, for up
	// to kMaxWriteDeferSeconds; it is validated again just before it is applied, since the table
	// may have changed meanwhile. OnDone runs exactly once, on the game thread: with true once the
	// rows are written, or with false and the reason when the wait ran out, the table was unloaded,
	// the re-validation failed, or the module shut down. On false nothing was written.
	void ApplyWhenNoTransactionOpen(TUniquePtr<ISuperSLMMCPWriteSink>&& Sink, TArray<FSuperSLMMCPSinkRecord>&& Records,
		TUniqueFunction<void(bool bWritten, FString&& Error)> OnDone);

	// Game thread. Unregisters the deferred-write ticker, then fails every waiting write (nothing
	// is written). Later calls to ApplyWhenNoTransactionOpen fail at once. Idempotent. The module
	// calls it at engine pre-exit and at shutdown.
	void ShutdownDeferredWrites();
}
