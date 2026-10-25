// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceSecrets.h"

#include "MantleApiClient.h"

#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TCHAR* MantlePlaceSecrets::LocalFileName()
{
	return TEXT("secrets.local.json");
}

FString MantlePlaceSecrets::NormalizeName(const FString& Typed)
{
	return Typed.TrimStartAndEnd().ToUpper();
}

bool MantlePlaceSecrets::CheckName(const FString& Name, FString& OutWhy)
{
	if (Name.IsEmpty())
	{
		OutWhy = TEXT("Type a name for the secret.");
		return false;
	}
	bool bOk = Name.Len() <= 64 && Name[0] >= TEXT('A') && Name[0] <= TEXT('Z');
	for (int32 Index = 1; bOk && Index < Name.Len(); ++Index)
	{
		const TCHAR Character = Name[Index];
		bOk = (Character >= TEXT('A') && Character <= TEXT('Z')) || (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
	}
	if (!bOk)
	{
		OutWhy = TEXT("A secret's name starts with a letter and has only capital letters, digits and underscores, up to 64 characters (for example API_KEY).");
		return false;
	}
	return true;
}

bool MantlePlaceSecrets::CheckValue(const FString& Value, int32 MaxBytes, FString& OutWhy)
{
	if (Value.IsEmpty())
	{
		OutWhy = TEXT("Type or paste the value first.");
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bControl = Character < 0x20 || Character == 0x7F || (Character >= 0x80 && Character < 0xA0);
		if (bControl && Character != TEXT('\t') && Character != TEXT('\n') && Character != TEXT('\r'))
		{
			OutWhy = TEXT("The value has a control character. Only tab and line breaks are allowed.");
			return false;
		}
	}
	const int32 Bytes = FTCHARToUTF8_Convert::ConvertedLength(*Value, Value.Len());
	if (Bytes > MaxBytes)
	{
		OutWhy = FString::Printf(TEXT("The value is %d bytes; a secret holds at most %d bytes (bytes, not characters)."), Bytes, MaxBytes);
		return false;
	}
	return true;
}

bool MantlePlaceSecrets::CheckCount(int32 CurrentCount, int32 MaxSecrets, bool bNameExists, FString& OutWhy)
{
	if (!bNameExists && CurrentCount >= MaxSecrets)
	{
		OutWhy = FString::Printf(TEXT("This place already holds %d secrets, the most it can. Delete one to add another."), MaxSecrets);
		return false;
	}
	return true;
}

FString MantlePlaceSecrets::CountText(int32 Count, int32 Max)
{
	return FString::Printf(TEXT("%d / %d"), Count, Max);
}

bool MantlePlaceSecrets::ParseList(const FString& Body, FList& OutList)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Root) || !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("secrets"), Items))
	{
		return false;
	}
	FList Parsed;
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FEntry Entry;
		if (!Item.IsValid() || !Item->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Entry.Name))
		{
			return false;
		}
		(*Object)->TryGetStringField(TEXT("createdAt"), Entry.CreatedAt);
		(*Object)->TryGetStringField(TEXT("updatedAt"), Entry.UpdatedAt);
		Parsed.Entries.Add(MoveTemp(Entry));
	}
	const TSharedPtr<FJsonObject>* Limits = nullptr;
	if (Root->TryGetObjectField(TEXT("limits"), Limits))
	{
		int32 Limit = 0;
		if ((*Limits)->TryGetNumberField(TEXT("maxSecrets"), Limit) && Limit > 0)
		{
			Parsed.MaxSecrets = Limit;
		}
		if ((*Limits)->TryGetNumberField(TEXT("maxValueBytes"), Limit) && Limit > 0)
		{
			Parsed.MaxValueBytes = Limit;
		}
	}
	OutList = MoveTemp(Parsed);
	return true;
}

FString MantlePlaceSecrets::MakeSetBody(const FString& Value)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("value"), Value);
	Writer->WriteObjectEnd();
	Writer->Close();
	return Out;
}

FString MantlePlaceSecrets::ListPath(const FString& PlaceId)
{
	return TEXT("/places/") + FGenericPlatformHttp::UrlEncode(PlaceId) + TEXT("/secrets");
}

FString MantlePlaceSecrets::ItemPath(const FString& PlaceId, const FString& Name)
{
	return ListPath(PlaceId) + TEXT("/") + FGenericPlatformHttp::UrlEncode(Name);
}

bool MantlePlaceSecrets::IsStoreOff(const FMantleApiResult& Result)
{
	return Result.StatusCode == 503 && Result.Code.Equals(TEXT("secrets_unavailable"), ESearchCase::CaseSensitive);
}

FString MantlePlaceSecrets::StoreOffMessage()
{
	return TEXT("Secrets aren't available on this server.");
}

FString MantlePlaceSecrets::NoPlaceMessage()
{
	return TEXT("This level has no place on Mantle yet. Save or publish it once, then come back to add secrets.");
}

FString MantlePlaceSecrets::DescribeFailure(const FMantleApiResult& Result)
{
	if (IsStoreOff(Result))
	{
		return StoreOffMessage();
	}
	if (!Result.Message.IsEmpty())
	{
		// The backend writes these for creators (the contract is in mantle PR #1016); an error body never carries a value.
		return Result.Message;
	}
	if (Result.Code == TEXT("secret_not_found"))
	{
		return TEXT("That secret is no longer set.");
	}
	if (Result.Code == TEXT("too_many_secrets"))
	{
		return TEXT("This place already holds the most secrets it can. Delete one to add another.");
	}
	switch (Result.StatusCode)
	{
	case 0: return TEXT("Could not reach Mantle. Check your internet connection and try again.");
	case 401: return TEXT("Your Mantle sign-in expired or was not accepted. Sign in again, or make a new creator token.");
	case 403: return TEXT("This creator token can't manage secrets.");
	case 404: return TEXT("Mantle could not find that place or secret.");
	case 409: return TEXT("This place already holds the most secrets it can.");
	case 400: return TEXT("Mantle refused that name or value.");
	default: return FString::Printf(TEXT("Mantle returned an unexpected error (%d)."), Result.StatusCode);
	}
}

FString MantlePlaceSecrets::LocalFilePathFor(const FString& LevelName)
{
	if (LevelName.IsEmpty())
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("MantleSecrets") / LevelName / LocalFileName());
}

bool MantlePlaceSecrets::AddToGitIgnore(FString& InOutText)
{
	TArray<FString> Lines;
	InOutText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
	const FString Name = LocalFileName();
	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed == Name || Trimmed == TEXT("**/") + Name || Trimmed == TEXT("/") + Name)
		{
			return true;
		}
	}
	if (!InOutText.IsEmpty() && !InOutText.EndsWith(TEXT("\n")))
	{
		InOutText += TEXT("\n");
	}
	InOutText += TEXT("# Mantle place secrets for local tests: never committed or uploaded.\n") + Name + TEXT("\n");
	return false;
}

bool MantlePlaceSecrets::EnsureLocalFile(const FString& LevelName, const FString& ProjectDir, FString& OutPath, FString& OutWhy)
{
	OutPath = LocalFilePathFor(LevelName);
	if (OutPath.IsEmpty())
	{
		OutWhy = TEXT("Open a saved level first.");
		return false;
	}
	// The ignore comes first: a file that could be committed is never written.
	const FString IgnoreFile = ProjectDir / TEXT(".gitignore");
	FString Ignore;
	FFileHelper::LoadFileToString(Ignore, *IgnoreFile);
	if (!AddToGitIgnore(Ignore) && !FFileHelper::SaveStringToFile(Ignore, *IgnoreFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutWhy = FString::Printf(TEXT("Could not update %s to ignore %s."), *IgnoreFile, LocalFileName());
		return false;
	}
	if (!IFileManager::Get().FileExists(*OutPath)
		&& !FFileHelper::SaveStringToFile(TEXT("{\n}\n"), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutWhy = FString::Printf(TEXT("Could not create %s."), *OutPath);
		return false;
	}
	return true;
}

bool MantlePlaceSecrets::CopyToBundle(const FString& SourceFile, const FString& BundleDir, FString& OutWhy)
{
	if (SourceFile.IsEmpty() || BundleDir.IsEmpty() || !IFileManager::Get().FileExists(*SourceFile))
	{
		return true;
	}
	if (IFileManager::Get().Copy(*(BundleDir / LocalFileName()), *SourceFile) != COPY_OK)
	{
		OutWhy = TEXT("Could not copy secrets.local.json into the preview folder.");
		return false;
	}
	return true;
}

void MantlePlaceSecrets::RemoveFromBundle(const FString& BundleDir)
{
	if (!BundleDir.IsEmpty())
	{
		IFileManager::Get().Delete(*(BundleDir / LocalFileName()), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
}

void MantlePlaceSecrets::List(const FString& PlaceId, TFunction<void(const FMantleApiResult&)> OnDone)
{
	FMantleApiClient::SendAsync(TEXT("GET"), ListPath(PlaceId), FString(),
		FMantleApiCallback::CreateLambda([OnDone](const FMantleApiResult& Result) { OnDone(Result); }));
}

void MantlePlaceSecrets::Set(const FString& PlaceId, const FString& Name, const FString& Value, TFunction<void(const FMantleApiResult&)> OnDone)
{
	FMantleApiClient::SendAsync(TEXT("PUT"), ItemPath(PlaceId, Name), MakeSetBody(Value),
		FMantleApiCallback::CreateLambda([OnDone](const FMantleApiResult& Result) { OnDone(Result); }));
}

void MantlePlaceSecrets::Delete(const FString& PlaceId, const FString& Name, TFunction<void(const FMantleApiResult&)> OnDone)
{
	FMantleApiClient::SendAsync(TEXT("DELETE"), ItemPath(PlaceId, Name), FString(),
		FMantleApiCallback::CreateLambda([OnDone](const FMantleApiResult& Result) { OnDone(Result); }));
}
