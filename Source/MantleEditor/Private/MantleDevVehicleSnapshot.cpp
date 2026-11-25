// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.VehicleSnapshot <VehicleSource> <out.json>
// Writes a built vehicle as stable, sorted JSON (FMantleDevVehicleSnapshot) for the standing car check, which converts a fixed set of real cars fresh
// and diffs each snapshot against the last good one. Reads only: nothing is saved or uploaded. Logs MANTLE_DEV_VEHICLE_SNAPSHOT ok=<0|1>.
#include "MantleDevVehicleSnapshot.h"

#include "MantleVehicleBuiltInMaterials.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleDevVehicleSnapshot, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevVehicleSnapshotPrivate
{
	/** Liveries and their names are picked by index, so their order is kept; any other list of plain names is a set. */
	bool KeepsOrder(const FString& Key)
	{
		return Key.Contains(TEXT("Livery"), ESearchCase::IgnoreCase) || Key.Contains(TEXT("Liveries"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonValue> Stable(const TSharedPtr<FJsonValue>& Value, const FString& Key);

	TSharedRef<FJsonObject> StableObject(const FJsonObject& Object)
	{
		// Values is keyed by a shared string in 5.8, not FString: collect the pairs, then sort them by name.
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
		for (const auto& Pair : Object.Values)
		{
			Fields.Emplace(FString(Pair.Key.ToView()), Pair.Value);
		}
		Fields.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Fields)
		{
			Out->SetField(Field.Key, Stable(Field.Value, Field.Key));
		}
		return Out;
	}

	TSharedPtr<FJsonValue> Stable(const TSharedPtr<FJsonValue>& Value, const FString& Key)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		switch (Value->Type)
		{
		case EJson::Number:
			return MakeShared<FJsonValueNumberString>(FMantleDevVehicleSnapshot::Number(Value->AsNumber()));
		case EJson::Object:
			return MakeShared<FJsonValueObject>(StableObject(*Value->AsObject()));
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			const bool bNames = Items.Num() > 0 && !KeepsOrder(Key) && !Items.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item)
			{
				return !Item.IsValid() || Item->Type != EJson::String;
			});
			if (bNames)
			{
				TArray<FString> Names;
				for (const TSharedPtr<FJsonValue>& Item : Items)
				{
					Names.Add(Item->AsString());
				}
				Names.Sort();
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const FString& Name : Names)
				{
					Out.Add(MakeShared<FJsonValueString>(Name));
				}
				return MakeShared<FJsonValueArray>(Out);
			}
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				Out.Add(Stable(Item, Key));
			}
			return MakeShared<FJsonValueArray>(Out);
		}
		default:
			return Value;
		}
	}
}

FString FMantleDevVehicleSnapshot::Number(const double Value)
{
	const double Rounded = FMath::RoundToDouble(Value * 1000.0) / 1000.0;
	FString Text = FString::Printf(TEXT("%.3f"), Rounded == 0.0 ? 0.0 : Rounded);
	Text.TrimEndInline();
	while (Text.Contains(TEXT(".")) && (Text.EndsWith(TEXT("0")) || Text.EndsWith(TEXT("."))))
	{
		Text.LeftChopInline(1);
	}
	return Text == TEXT("-0") ? TEXT("0") : Text;
}

TSharedRef<FJsonObject> FMantleDevVehicleSnapshot::Make(const UMantleVehicleSource& Source)
{
	TSharedRef<FJsonObject> Raw = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(UMantleVehicleSource::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMantleVehicleSource, ThumbnailPng))
		{
			continue;
		}
		const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(Property, Property->ContainerPtrToValuePtr<void>(&Source), 0, 0, nullptr, nullptr,
			EJsonObjectConversionFlags::SkipStandardizeCase);
		Raw->SetField(Property->GetName(), Value);
	}

	// What the body says: each section (in the body's order, which is its index) with the material it draws with and the built-in one it publishes with, and its bones.
	TArray<TSharedPtr<FJsonValue>> Sections;
	TArray<TSharedPtr<FJsonValue>> Bones;
	if (const USkeletalMesh* Body = Source.Body.LoadSynchronous())
	{
		const TArray<FSkeletalMaterial>& Materials = Body->GetMaterials();
		const TArray<FString> Names = FMantleVehicleLights::SectionNames(Materials);
		for (int32 Index = 0; Index < Names.Num() && Index < Materials.Num(); ++Index)
		{
			const UMaterialInterface* Material = Materials[Index].MaterialInterface;
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			Section->SetStringField(TEXT("Name"), Names[Index]);
			Section->SetStringField(TEXT("Material"), Material ? Material->GetPathName() : FString());
			Section->SetStringField(TEXT("BuiltIn"), FMantleVehicleBuiltInMaterials::ForSection(Source, Names[Index], Material));
			Sections.Add(MakeShared<FJsonValueObject>(Section));
		}
		for (const FMeshBoneInfo& Bone : Body->GetRefSkeleton().GetRefBoneInfo())
		{
			Bones.Add(MakeShared<FJsonValueString>(Bone.Name.ToString()));
		}
	}
	Raw->SetArrayField(TEXT("Sections"), Sections);
	Raw->SetArrayField(TEXT("Bones"), Bones);
	return MantleDevVehicleSnapshotPrivate::StableObject(*Raw);
}

FString FMantleDevVehicleSnapshot::ToText(const UMantleVehicleSource& Source)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Make(Source), Writer);
	return Text + TEXT("\n");
}

namespace MantleDevVehicleSnapshotPrivate
{
	FAutoConsoleCommand VehicleSnapshotCommand(
		TEXT("Mantle.Dev.VehicleSnapshot"),
		TEXT("Mantle.Dev.VehicleSnapshot <VehicleSource> <out.json> -- writes the built vehicle as stable, sorted JSON (every vehicle setting, the body's sections with their materials ")
		TEXT("and built-in materials, its bones) so two builds of a car can be compared. Reads only. Logs MANTLE_DEV_VEHICLE_SNAPSHOT ok=<0|1>."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogMantleDevVehicleSnapshot, Error, TEXT("MANTLE_DEV_VEHICLE_SNAPSHOT ok=0 usage: Mantle.Dev.VehicleSnapshot <VehicleSource> <out.json>"));
				return;
			}
			// "/Game/Cars/Car_Vehicle" names the object of its package's own name.
			FString Path = Args[0];
			if (!Path.Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetShortName(Path);
			}
			const UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Source)
			{
				UE_LOG(LogMantleDevVehicleSnapshot, Warning, TEXT("MANTLE_DEV_VEHICLE_SNAPSHOT ok=0 %s is not a Mantle vehicle"), *Path);
				return;
			}
			const FString Out = FPaths::ConvertRelativePathToFull(Args[1]);
			if (!FFileHelper::SaveStringToFile(FMantleDevVehicleSnapshot::ToText(*Source), *Out, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogMantleDevVehicleSnapshot, Warning, TEXT("MANTLE_DEV_VEHICLE_SNAPSHOT ok=0 could not write %s"), *Out);
				return;
			}
			UE_LOG(LogMantleDevVehicleSnapshot, Display, TEXT("MANTLE_DEV_VEHICLE_SNAPSHOT ok=1 vehicle=%s file=%s"), *Path, *Out);
		}));
}
