// Copyright Joshua Eger. All Rights Reserved.
#include "MantleProvenance.h"

#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

FString FMantleProvenance::Clean(const FString& Text, const int32 MaxUtf8Bytes)
{
	FString Kept;
	Kept.Reserve(Text.Len());
	for (const TCHAR Character : Text)
	{
		// Control characters: C0, DEL, C1, and the Unicode line and paragraph separators.
		if (Character < 0x20 || (Character >= 0x7f && Character <= 0x9f) || Character == 0x2028 || Character == 0x2029)
		{
			continue;
		}
		// Invisible format characters (Unicode Cf), which the server refuses: soft hyphen, zero-width
		// and direction marks, bidi embeddings and isolates, word joiners, the byte-order mark.
		if (Character == 0x00ad || (Character >= 0x0600 && Character <= 0x0605) || Character == 0x061c || Character == 0x06dd
			|| Character == 0x070f || Character == 0x180e || (Character >= 0x200b && Character <= 0x200f)
			|| (Character >= 0x202a && Character <= 0x202e) || (Character >= 0x2060 && Character <= 0x2064)
			|| (Character >= 0x2066 && Character <= 0x206f) || Character == 0xfeff || (Character >= 0xfff9 && Character <= 0xfffb))
		{
			continue;
		}
		Kept.AppendChar(Character);
	}
	Kept.TrimStartAndEndInline();

	// Cut on whole characters so the UTF-8 form fits; a lone surrogate half is dropped (it has no UTF-8 form).
	FString Out;
	int32 Bytes = 0;
	for (int32 Index = 0; Index < Kept.Len();)
	{
		const TCHAR Character = Kept[Index];
		int32 Units = 1;
		int32 CharacterBytes = 0;
		if (Character >= 0xd800 && Character <= 0xdbff && Index + 1 < Kept.Len() && Kept[Index + 1] >= 0xdc00 && Kept[Index + 1] <= 0xdfff)
		{
			Units = 2;
			CharacterBytes = 4;
		}
		else if (Character >= 0xd800 && Character <= 0xdfff)
		{
			++Index;
			continue;
		}
		else
		{
			CharacterBytes = Character < 0x80 ? 1 : (Character < 0x800 ? 2 : 3);
		}
		if (Bytes + CharacterBytes > MaxUtf8Bytes)
		{
			break;
		}
		Out.AppendChars(&Kept[Index], Units);
		Bytes += CharacterBytes;
		Index += Units;
	}
	Out.TrimEndInline();
	return Out;
}

FString FMantleProvenance::FileNameOf(const FString& Path)
{
	int32 Cut = INDEX_NONE;
	for (int32 Index = Path.Len() - 1; Index >= 0; --Index)
	{
		if (Path[Index] == TEXT('/') || Path[Index] == TEXT('\\'))
		{
			Cut = Index;
			break;
		}
	}
	return Cut == INDEX_NONE ? Path : Path.Mid(Cut + 1);
}

FString FMantleProvenance::AssetPathOf(const FString& ObjectPath)
{
	int32 Dot = INDEX_NONE;
	int32 Slash = INDEX_NONE;
	if (!ObjectPath.FindLastChar(TEXT('.'), Dot) || !ObjectPath.FindLastChar(TEXT('/'), Slash) || Dot < Slash)
	{
		return ObjectPath;
	}
	const FString Between = ObjectPath.Mid(Slash + 1, Dot - Slash - 1);
	const FString After = ObjectPath.Mid(Dot + 1);
	return !Between.IsEmpty() && Between.Equals(After, ESearchCase::CaseSensitive) ? ObjectPath.Left(Dot) : ObjectPath;
}

FString FMantleProvenance::CleanFilename(const FString& RawFilename)
{
	return Clean(FileNameOf(RawFilename), MaxImportFilenameBytes);
}

FMantleProvenance FMantleProvenance::Make(const FString& InSourcePath, const FString& RawImportFilename)
{
	FMantleProvenance Provenance;
	Provenance.SourcePath = Clean(AssetPathOf(InSourcePath), MaxSourcePathBytes);
	Provenance.ImportFilename = CleanFilename(RawImportFilename);
	return Provenance;
}

FString FMantleProvenance::ImportFilenameOf(const UObject* Source)
{
#if WITH_EDITORONLY_DATA
	if (!Source)
	{
		return FString();
	}
	// Skeletal and static meshes, animations and grooms all name their import data AssetImportData; reading it by name
	// keeps this independent of which class the creator's source is.
	const FObjectProperty* Property = CastField<FObjectProperty>(Source->GetClass()->FindPropertyByName(TEXT("AssetImportData")));
	if (!Property)
	{
		return FString();
	}
	const UAssetImportData* ImportData = Cast<UAssetImportData>(Property->GetObjectPropertyValue_InContainer(Source));
	return ImportData ? CleanFilename(ImportData->GetFirstFilename()) : FString();
#else
	return FString();
#endif
}

FMantleProvenance FMantleProvenance::FromAsset(const UObject* Source)
{
	return Source ? Make(Source->GetPathName(), ImportFilenameOf(Source)) : FMantleProvenance();
}

TSharedPtr<FJsonObject> FMantleProvenance::ToJson() const
{
	if (IsEmpty())
	{
		return nullptr;
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("sourcePath"), SourcePath);
	Object->SetStringField(TEXT("importFilename"), ImportFilename);
	return Object;
}

void FMantleProvenance::AddTo(FJsonObject& Body, const bool bPublic) const
{
	if (!bPublic)
	{
		return;
	}
	if (const TSharedPtr<FJsonObject> Object = ToJson())
	{
		Body.SetObjectField(TEXT("provenance"), Object);
	}
}
