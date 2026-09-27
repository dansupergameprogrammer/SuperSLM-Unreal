#include "SuperSLMMCPReadSource.h"

#include "Engine/DataTable.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	bool IsReadableColumn(const FProperty& Property)
	{
		if (Property.GetSize() != Property.GetElementSize())
		{
			return false; // a fixed-size array
		}
		return CastField<FStrProperty>(&Property) != nullptr || CastField<FNameProperty>(&Property) != nullptr ||
			CastField<FTextProperty>(&Property) != nullptr || CastField<FBoolProperty>(&Property) != nullptr ||
			CastField<FNumericProperty>(&Property) != nullptr || CastField<FEnumProperty>(&Property) != nullptr;
	}

	FString ReadColumn(const FProperty& Property, const void* RowData)
	{
		const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(RowData);
		if (const FStrProperty* Str = CastField<FStrProperty>(&Property))
		{
			return Str->GetPropertyValue(ValuePtr);
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			const FName Value = Name->GetPropertyValue(ValuePtr);
			return Value.IsNone() ? FString() : Value.ToString();
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			return Text->GetPropertyValue(ValuePtr).ToString();
		}
		// Numbers, bools and enums: the text the property itself exports.
		FString Exported;
		Property.ExportText_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return Exported;
	}

	class FDataTableColumnSource final : public ISuperSLMMCPReadSource
	{
	public:
		FDataTableColumnSource(UDataTable& InTable, const FProperty& InColumn) : Table(&InTable), Column(&InColumn) {}

		virtual FString GetSourcePath() const override
		{
			return Table.IsValid() ? Table->GetPathName() + TEXT(":") + Column->GetAuthoredName() : FString();
		}

		virtual bool Read(TArray<FSuperSLMMCPSourceRecord>& OutRecords, FString& OutError) const override
		{
			check(IsInGameThread());
			OutRecords.Reset();
			const UDataTable* DataTable = Table.Get();
			if (DataTable == nullptr || DataTable->GetRowStruct() == nullptr)
			{
				OutError = TEXT("the source data table is gone or has no row struct");
				return false;
			}
			for (const TPair<FName, uint8*>& Row : DataTable->GetRowMap())
			{
				FString Text = ReadColumn(*Column, Row.Value);
				if (Text.IsEmpty())
				{
					OutError = FString::Printf(TEXT("source row '%s' has an empty '%s'"), *Row.Key.ToString(), *Column->GetAuthoredName());
					OutRecords.Reset();
					return false;
				}
				OutRecords.Add(FSuperSLMMCPSourceRecord{Row.Key, MoveTemp(Text)});
			}
			return true;
		}

	private:
		TWeakObjectPtr<UDataTable> Table;
		const FProperty* Column; // owned by the row struct, which the table keeps alive
	};
}

namespace SuperSLMMCPReadSource
{
	TUniquePtr<ISuperSLMMCPReadSource> MakeDataTableColumnSource(UDataTable* Table, const FString& Column, FString& OutError)
	{
		check(IsInGameThread());
		const UScriptStruct* RowStruct = Table != nullptr ? Table->GetRowStruct() : nullptr;
		if (RowStruct == nullptr)
		{
			OutError = Table == nullptr ? TEXT("the source data table is not loaded")
				: FString::Printf(TEXT("source data table %s has no row struct"), *Table->GetPathName());
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			if (!It->GetAuthoredName().Equals(Column, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!IsReadableColumn(**It))
			{
				OutError = FString::Printf(TEXT("source column '%s' is a %s; a read source reads string, name, text, numeric, bool or enum columns"),
					*It->GetAuthoredName(), *It->GetCPPType());
				return nullptr;
			}
			return MakeUnique<FDataTableColumnSource>(*Table, **It);
		}
		OutError = FString::Printf(TEXT("source data table %s has no column '%s'"), *Table->GetPathName(), *Column);
		return nullptr;
	}
}
