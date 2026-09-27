#include "SuperSLMMCPWriteSink.h"

#include "Containers/Ticker.h"
#include "DataTableEditorUtils.h"
#include "HAL/PlatformTime.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "ScopedTransaction.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SuperSLMMCPWriteSink"

namespace
{
	FString JsonTypeName(EJson Type)
	{
		switch (Type)
		{
		case EJson::None: return TEXT("none");
		case EJson::Null: return TEXT("null");
		case EJson::String: return TEXT("string");
		case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("boolean");
		case EJson::Array: return TEXT("array");
		case EJson::Object: return TEXT("object");
		default: return TEXT("unknown");
		}
	}

	FProperty* FindRowProperty(const UScriptStruct& RowStruct, const FString& Key)
	{
		for (TFieldIterator<FProperty> It(&RowStruct); It; ++It)
		{
			const FString Authored = It->GetAuthoredName();
			if (Authored.Equals(Key, ESearchCase::IgnoreCase))
			{
				return *It;
			}
			if (CastField<FBoolProperty>(*It) != nullptr && Authored.Len() > 1 &&
				(Authored[0] == TEXT('b') || Authored[0] == TEXT('B')) &&
				Authored.RightChop(1).Equals(Key, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	// The closed range an integer property holds, by its C++ type. False for a numeric property
	// that is not one of the engine's integer property classes.
	bool IntegerBounds(const FNumericProperty& Property, double& OutMin, double& OutMax, const TCHAR*& OutTypeName)
	{
		auto Set = [&](double Min, double Max, const TCHAR* Name) { OutMin = Min; OutMax = Max; OutTypeName = Name; return true; };
		if (Property.IsA<FByteProperty>())   { return Set(0.0, 255.0, TEXT("uint8")); }
		if (Property.IsA<FInt8Property>())   { return Set(-128.0, 127.0, TEXT("int8")); }
		if (Property.IsA<FInt16Property>())  { return Set(-32768.0, 32767.0, TEXT("int16")); }
		if (Property.IsA<FUInt16Property>()) { return Set(0.0, 65535.0, TEXT("uint16")); }
		if (Property.IsA<FIntProperty>())    { return Set(-2147483648.0, 2147483647.0, TEXT("int32")); }
		if (Property.IsA<FUInt32Property>()) { return Set(0.0, 4294967295.0, TEXT("uint32")); }
		// 64-bit: bounded below by the exactness limit the caller applies first.
		if (Property.IsA<FInt64Property>())  { return Set(-9223372036854775808.0, 9223372036854775807.0, TEXT("int64")); }
		if (Property.IsA<FUInt64Property>()) { return Set(0.0, 18446744073709551615.0, TEXT("uint64")); }
		return false;
	}

	// A JSON number is a double, which holds every integer only up to 2^53 in magnitude. Beyond it,
	// the value parsed may not be the value the model wrote, so such a value is refused rather than
	// rounded.
	constexpr double kLargestExactInteger = 9007199254740992.0; // 2^53

	// Writes Value into Property on RowData, or fails naming why. Never writes a value it has not
	// first checked against the property's type.
	bool SetField(FProperty& Property, void* RowData, const FString& Key, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		const EJson Type = Value.IsValid() ? Value->Type : EJson::None;
		void* ValuePtr = Property.ContainerPtrToValuePtr<void>(RowData);
		auto Mismatch = [&]()
		{
			OutError = FString::Printf(TEXT("field '%s' is a JSON %s; row property '%s' (%s) does not accept it"),
				*Key, *JsonTypeName(Type), *Property.GetAuthoredName(), *Property.GetCPPType());
			return false;
		};

		if (Property.GetSize() != Property.GetElementSize())
		{
			OutError = FString::Printf(TEXT("row property '%s' is a fixed-size array, which no schema field fills"), *Property.GetAuthoredName());
			return false;
		}

		if (FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			if (Type != EJson::Boolean) { return Mismatch(); }
			Bool->SetPropertyValue(ValuePtr, Value->AsBool());
			return true;
		}
		if (FStrProperty* Str = CastField<FStrProperty>(&Property))
		{
			if (Type != EJson::String) { return Mismatch(); }
			Str->SetPropertyValue(ValuePtr, Value->AsString());
			return true;
		}
		if (FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			if (Type != EJson::String) { return Mismatch(); }
			Name->SetPropertyValue(ValuePtr, FName(*Value->AsString()));
			return true;
		}
		if (FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			if (Type != EJson::String) { return Mismatch(); }
			Text->SetPropertyValue(ValuePtr, FText::FromString(Value->AsString()));
			return true;
		}

		UEnum* Enum = nullptr;
		FNumericProperty* Numeric = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			Enum = EnumProperty->GetEnum();
			Numeric = EnumProperty->GetUnderlyingProperty();
		}
		else if (FNumericProperty* AsNumeric = CastField<FNumericProperty>(&Property))
		{
			Numeric = AsNumeric;
			Enum = AsNumeric->GetIntPropertyEnum();
		}

		if (Enum != nullptr && Numeric != nullptr)
		{
			if (Type != EJson::String) { return Mismatch(); }
			const int64 EnumValue = Enum->GetValueByNameString(Value->AsString());
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("field '%s' = '%s' is not a value of enum %s (row property '%s')"),
					*Key, *Value->AsString(), *Enum->GetName(), *Property.GetAuthoredName());
				return false;
			}
			Numeric->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}
		if (Numeric != nullptr)
		{
			if (Type != EJson::Number) { return Mismatch(); }
			const double Number = Value->AsNumber();
			if (Numeric->IsInteger())
			{
				if (!FMath::IsFinite(Number) || FMath::Frac(Number) != 0.0)
				{
					OutError = FString::Printf(TEXT("field '%s' = %f is not integral; row property '%s' is an integer"),
						*Key, Number, *Property.GetAuthoredName());
					return false;
				}
				if (FMath::Abs(Number) > kLargestExactInteger)
				{
					OutError = FString::Printf(TEXT("field '%s' = %.0f is beyond 2^53, so a JSON number does not hold it exactly (row property '%s')"),
						*Key, Number, *Property.GetAuthoredName());
					return false;
				}
				double Min = 0.0;
				double Max = 0.0;
				const TCHAR* TypeName = TEXT("");
				if (!IntegerBounds(*Numeric, Min, Max, TypeName))
				{
					OutError = FString::Printf(TEXT("row property '%s' (%s) is an integer type the write sink does not fill"),
						*Property.GetAuthoredName(), *Property.GetCPPType());
					return false;
				}
				if (Number < Min || Number > Max)
				{
					OutError = FString::Printf(TEXT("field '%s' = %.0f is out of range for row property '%s' (%s: %.0f to %.0f)"),
						*Key, Number, *Property.GetAuthoredName(), TypeName, Min, Max);
					return false;
				}
				// In range, so each cast is exact.
				if (Number < 0.0)
				{
					Numeric->SetIntPropertyValue(ValuePtr, static_cast<int64>(Number));
				}
				else
				{
					Numeric->SetIntPropertyValue(ValuePtr, static_cast<uint64>(Number));
				}
			}
			else
			{
				if (!FMath::IsFinite(Number) || (Property.IsA<FFloatProperty>() && FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max())))
				{
					OutError = FString::Printf(TEXT("field '%s' = %g is out of range for row property '%s' (%s)"),
						*Key, Number, *Property.GetAuthoredName(), *Property.GetCPPType());
					return false;
				}
				Numeric->SetFloatingPointPropertyValue(ValuePtr, Number);
			}
			return true;
		}

		OutError = FString::Printf(TEXT("row property '%s' (%s) is of a type the write sink does not fill"),
			*Property.GetAuthoredName(), *Property.GetCPPType());
		return false;
	}

	class FDataTableRowSink final : public ISuperSLMMCPWriteSink
	{
	public:
		explicit FDataTableRowSink(UDataTable& InTable) : Table(&InTable) {}

		virtual FString GetTargetPath() const override
		{
			return Table.IsValid() ? Table->GetPathName() : FString();
		}

		virtual bool Validate(const TArray<FSuperSLMMCPSinkRecord>& Records, FString& OutError) override
		{
			PreparedRows.Reset();
			UDataTable* DataTable = Table.Get();
			const UScriptStruct* RowStruct = DataTable != nullptr ? DataTable->GetRowStruct() : nullptr;
			if (RowStruct == nullptr)
			{
				OutError = TEXT("the data table is gone or has no row struct");
				return false;
			}

			for (int32 R = 0; R < Records.Num(); ++R)
			{
				const FSuperSLMMCPSinkRecord& Record = Records[R];
				if (Record.RowName.IsNone() || !Record.Fields.IsValid())
				{
					OutError = FString::Printf(TEXT("record %d has no row name or no decoded object"), R);
					return false;
				}
				TSharedRef<FStructOnScope> Row = MakeShared<FStructOnScope>(RowStruct);
				TSet<const FProperty*> Filled;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Record.Fields->Values)
				{
					FProperty* Property = FindRowProperty(*RowStruct, Field.Key);
					if (Property == nullptr)
					{
						OutError = FString::Printf(TEXT("record %d: decoded field '%s' has no property in row struct %s"),
							R, *Field.Key, *RowStruct->GetName());
						return false;
					}
					if (Filled.Contains(Property))
					{
						OutError = FString::Printf(TEXT("record %d: two decoded fields map to row property '%s'"),
							R, *Property->GetAuthoredName());
						return false;
					}
					Filled.Add(Property);
					FString FieldError;
					if (!SetField(*Property, Row->GetStructMemory(), Field.Key, Field.Value, FieldError))
					{
						OutError = FString::Printf(TEXT("record %d: %s"), R, *FieldError);
						return false;
					}
				}
				PreparedRows.Add(Row);
			}
			return true;
		}

		virtual bool Apply(const TArray<FSuperSLMMCPSinkRecord>& Records, FString& OutError) override
		{
			UDataTable* DataTable = Table.Get();
			const UScriptStruct* RowStruct = DataTable != nullptr ? DataTable->GetRowStruct() : nullptr;
			if (RowStruct == nullptr || PreparedRows.Num() != Records.Num())
			{
				OutError = TEXT("the write sink was applied without a matching validation");
				return false;
			}

			// The write is its own undo step. An FScopedTransaction opened while another transaction is
			// open nests inside it, so the rows would join the user's step and one undo would revert
			// both. So a write that would nest is refused here, by the open transaction's name;
			// ApplyWhenNoTransactionOpen waits for it to close instead.
			FString OpenTransaction;
			if (SuperSLMMCPWriteSink::IsTransactionOpen(OpenTransaction))
			{
				OutError = FString::Printf(TEXT("the editor has an open transaction ('%s'); the rows were not written, so that one undo does not revert both"),
					*OpenTransaction);
				return false;
			}
			if (GEditor == nullptr)
			{
				OutError = TEXT("no editor is running, so there is no undo buffer to write into");
				return false;
			}
			const FScopedTransaction Transaction(LOCTEXT("WriteRows", "SuperSLM: Write Constrained-Inference Rows"));
			// Modify() records the table only when it is transactional; an asset created in code
			// (rather than by a factory) may not be.
			if (!DataTable->HasAnyFlags(RF_Transactional))
			{
				DataTable->SetFlags(RF_Transactional);
			}
			FDataTableEditorUtils::BroadcastPreChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
			DataTable->Modify();
			for (int32 R = 0; R < Records.Num(); ++R)
			{
				DataTable->AddRow(Records[R].RowName, PreparedRows[R]->GetStructMemory(), RowStruct);
			}
			FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
			PreparedRows.Reset();
			return true;
		}

	private:
		TWeakObjectPtr<UDataTable> Table;
		TArray<TSharedRef<FStructOnScope>> PreparedRows;
	};

	// ------------------------------------------------------------------------------------------
	// Writes waiting for the user's open transaction to close. Game thread only.
	// ------------------------------------------------------------------------------------------
	struct FDeferredWrite
	{
		TUniquePtr<ISuperSLMMCPWriteSink> Sink;
		TArray<FSuperSLMMCPSinkRecord> Records;
		TUniqueFunction<void(bool, FString&&)> OnDone;
		double DeadlineSeconds = 0.0;
	};

	TArray<FDeferredWrite> GDeferredWrites;
	FTSTicker::FDelegateHandle GDeferredTicker;
	bool GDeferredShutDown = false;

	// Validates again (the table may have changed while the write waited) and applies.
	void ValidateAndApply(FDeferredWrite& Write)
	{
		FString Error;
		const bool bWritten = Write.Sink->Validate(Write.Records, Error) && Write.Sink->Apply(Write.Records, Error);
		Write.OnDone(bWritten, MoveTemp(Error));
	}

	// Game thread, once per frame while a write waits. Never waits.
	bool PollDeferredWrites(float)
	{
		FString OpenTransaction;
		const bool bOpen = SuperSLMMCPWriteSink::IsTransactionOpen(OpenTransaction);
		const double Now = FPlatformTime::Seconds();
		// Settled writes are taken out before any OnDone runs, so a callback that defers another
		// write only appends to the list.
		TArray<FDeferredWrite> Ready;
		TArray<FDeferredWrite> Expired;
		for (int32 I = GDeferredWrites.Num() - 1; I >= 0; --I)
		{
			if (!bOpen)
			{
				Ready.Insert(MoveTemp(GDeferredWrites[I]), 0);
				GDeferredWrites.RemoveAt(I);
			}
			else if (Now > GDeferredWrites[I].DeadlineSeconds)
			{
				Expired.Insert(MoveTemp(GDeferredWrites[I]), 0);
				GDeferredWrites.RemoveAt(I);
			}
		}
		for (FDeferredWrite& Write : Ready)
		{
			ValidateAndApply(Write);
		}
		for (FDeferredWrite& Write : Expired)
		{
			Write.OnDone(false, FString::Printf(TEXT("the editor's transaction ('%s') stayed open for %.0f s after the run finished; the rows were not written, so that one undo does not revert both"),
				*OpenTransaction, SuperSLMMCPWriteSink::kMaxWriteDeferSeconds));
		}
		if (GDeferredWrites.Num() == 0)
		{
			GDeferredTicker.Reset();
			return false;
		}
		return true;
	}
}

namespace SuperSLMMCPWriteSink
{
	TUniquePtr<ISuperSLMMCPWriteSink> MakeDataTableRowSink(UDataTable* Table, FString& OutError)
	{
		check(IsInGameThread());
		if (Table == nullptr)
		{
			OutError = TEXT("the data table is no longer loaded");
			return nullptr;
		}
		if (Table->GetRowStruct() == nullptr)
		{
			OutError = FString::Printf(TEXT("data table %s has no row struct"), *Table->GetPathName());
			return nullptr;
		}
		return MakeUnique<FDataTableRowSink>(*Table);
	}

	bool IsTransactionOpen(FString& OutName)
	{
		check(IsInGameThread());
		if (GUndo != nullptr || (GEditor != nullptr && GEditor->IsTransactionActive()))
		{
			OutName = GEditor != nullptr ? GEditor->GetTransactionName().ToString() : FString();
			if (OutName.IsEmpty())
			{
				OutName = TEXT("unnamed");
			}
			return true;
		}
		return false;
	}

	void ApplyWhenNoTransactionOpen(TUniquePtr<ISuperSLMMCPWriteSink>&& Sink, TArray<FSuperSLMMCPSinkRecord>&& Records,
		TUniqueFunction<void(bool bWritten, FString&& Error)> OnDone)
	{
		check(IsInGameThread());
		FString OpenTransaction;
		if (!IsTransactionOpen(OpenTransaction))
		{
			FString Error;
			const bool bWritten = Sink->Apply(Records, Error);
			OnDone(bWritten, MoveTemp(Error));
			return;
		}
		if (GDeferredShutDown)
		{
			OnDone(false, TEXT("cancelled: the editor is shutting down; the rows were not written"));
			return;
		}
		GDeferredWrites.Add(FDeferredWrite{MoveTemp(Sink), MoveTemp(Records), MoveTemp(OnDone),
			FPlatformTime::Seconds() + kMaxWriteDeferSeconds});
		if (!GDeferredTicker.IsValid())
		{
			GDeferredTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&PollDeferredWrites), 0.0f);
		}
	}

	void ShutdownDeferredWrites()
	{
		check(IsInGameThread());
		GDeferredShutDown = true;
		if (GDeferredTicker.IsValid())
		{
			FTSTicker::RemoveTicker(GDeferredTicker);
			GDeferredTicker.Reset();
		}
		TArray<FDeferredWrite> Pending = MoveTemp(GDeferredWrites);
		for (FDeferredWrite& Write : Pending)
		{
			Write.OnDone(false, TEXT("cancelled: the editor is shutting down; the rows were not written"));
		}
	}
}

#undef LOCTEXT_NAMESPACE
