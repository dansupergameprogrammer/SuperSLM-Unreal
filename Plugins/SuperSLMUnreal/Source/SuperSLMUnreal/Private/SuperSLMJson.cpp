#include "SuperSLMJson.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool SuperSLMJson::TryReadObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
	OutObject.Reset();
	OutError.Reset();
	if (Text.EndsWith(TEXT("\\"), ESearchCase::CaseSensitive))
	{
		OutError = TEXT("it ends inside an escape");
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		OutObject.Reset();
		OutError = Reader->GetErrorMessage().IsEmpty() ? FString(TEXT("no object")) : Reader->GetErrorMessage();
		return false;
	}
	return true;
}
