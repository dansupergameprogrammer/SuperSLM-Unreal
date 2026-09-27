#pragma once

// The order table's row: the potion_shop_order schema's four keys. Test-only, used by
// SuperSLML2S3MCPActionTierTests.cpp.
//
// A USTRUCT so the UDataTable has row fields the write sink can validate against. In the
// earlier version this was a plain struct in the test's .cpp: it had no StaticStruct(), so the
// table had no row fields, and every write was rejected. Two constraints on where it lives:
// - It is in a header, because UnrealHeaderTool scans headers, never .cpp files.
// - It is deliberately not inside any `#if`, because UHT refuses reflected types inside
//   preprocessor blocks it does not recognize.
// Its fields are the schema's four keys. FJsonObjectConverter maps them case-insensitively and
// drops a bool property's leading 'b' (bPolite <-> "polite"). The enum labels are held as
// strings.

#include "CoreMinimal.h"
#include "Engine/DataTable.h"

#include "SuperSLMPotionShopOrderRow.generated.h"

USTRUCT()
struct FSuperSLMPotionShopOrderRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	FString Intent;

	UPROPERTY()
	FString Item;

	UPROPERTY()
	FString Quantity;

	UPROPERTY()
	bool bPolite = false;
};
