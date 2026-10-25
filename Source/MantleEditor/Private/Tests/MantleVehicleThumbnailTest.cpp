// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPublish.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePng.h"
#include "MantleThumbnailChoice.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleThumbnail.h"

#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// A built vehicle's library card picture. Vehicles are library items and a card shows a thumbnail (a 512 x 512 PNG texture the item names), which the builder
// takes when it builds the vehicle; publishing uploads it with the vehicle and names it on the item. These tests run without a
// renderer (-nullrhi), so the picture is the one drawn on the CPU; a build in an editor that can render takes the scene capture's instead, the same size and the same bytes' format.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleThumbnailTestPrivate
{
	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}

	/** The sedan chassis's grey body, built as a vehicle in a scratch package (nothing reaches the disk). */
	FMantleVehicleBuildResult BuildSedan(const TCHAR* Name)
	{
		FMantleChassisSpec Spec;
		FString Error;
		FMantleVehicleBuildResult Result;
		if (!FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error))
		{
			Result.Problems.Add(Error);
			return Result;
		}
		return FMantleVehicleBuilder::BuildChassis(Spec, TEXT("/Temp/MantleVehicleThumbnailTest"), Name);
	}

	void DiscardBuild(FMantleVehicleBuildResult& Result)
	{
		if (!Result.Source)
		{
			return;
		}
		if (USkeletalMesh* Mesh = Result.Source->Body.Get())
		{
			if (USkeleton* Skeleton = Mesh->GetSkeleton())
			{
				Skeleton->ClearFlags(RF_Public | RF_Standalone);
				Skeleton->MarkAsGarbage();
			}
			Mesh->ClearFlags(RF_Public | RF_Standalone);
			Mesh->MarkAsGarbage();
		}
		Result.Source->ClearFlags(RF_Public | RF_Standalone);
		Result.Source->MarkAsGarbage();
	}

	/** How many pixels of a picture differ from the card's backdrop at the same row, and how many different colours they hold. */
	void CountVehiclePixels(const FImage& Image, int32& OutVehicle, int32& OutColours)
	{
		OutVehicle = 0;
		TSet<uint32> Colours;
		const auto Pixels = Image.AsBGRA8();
		for (int64 Row = 0; Row < Image.SizeY; ++Row)
		{
			const FColor Backdrop = Pixels[Row * Image.SizeX];
			for (int64 Column = 0; Column < Image.SizeX; ++Column)
			{
				const FColor Pixel = Pixels[Row * Image.SizeX + Column];
				if (Pixel != Backdrop)
				{
					++OutVehicle;
					Colours.Add(Pixel.DWColor());
				}
			}
		}
		OutColours = Colours.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleThumbnailBuiltTest,
	"Mantle.Vehicle.Thumbnail.BuiltVehicleGetsAPicture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleThumbnailBuiltTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleThumbnailTestPrivate;

	// Drawn on the CPU from a body's triangles: a picture of the vehicle on a card, not a blank one.
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Grey;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the sedan chassis loads (%s)"), *Error), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Spec, Grey, Error)))
	{
		return false;
	}
	TArray<FColor> Pixels;
	if (TestTrue(TEXT("the grey body is drawn"), FMantleVehicleThumbnail::Draw(Grey.Body, Pixels)) && TestEqual(TEXT("512 x 512 pixels"), Pixels.Num(), FMantleVehicleThumbnail::Size * FMantleVehicleThumbnail::Size))
	{
		int32 Opaque = 0;
		int32 Vehicle = 0;
		TSet<uint32> Colours;
		for (int32 Row = 0; Row < FMantleVehicleThumbnail::Size; ++Row)
		{
			const FColor Backdrop = Pixels[Row * FMantleVehicleThumbnail::Size];
			for (int32 Column = 0; Column < FMantleVehicleThumbnail::Size; ++Column)
			{
				const FColor Pixel = Pixels[Row * FMantleVehicleThumbnail::Size + Column];
				Opaque += Pixel.A == 255 ? 1 : 0;
				if (Pixel != Backdrop)
				{
					++Vehicle;
					Colours.Add(Pixel.DWColor());
				}
			}
		}
		const int32 Total = FMantleVehicleThumbnail::Size * FMantleVehicleThumbnail::Size;
		TestEqual(TEXT("every pixel is opaque"), Opaque, Total);
		TestTrue(*FString::Printf(TEXT("the vehicle fills part of the card (%d of %d pixels)"), Vehicle, Total), Vehicle > Total / 25 && Vehicle < Total * 4 / 5);
		TestTrue(*FString::Printf(TEXT("and is shaded, not one flat colour (%d colours)"), Colours.Num()), Colours.Num() >= 4);
		TestTrue(TEXT("the card is a backdrop that is lighter at the top"), Pixels[0].R > Pixels[(FMantleVehicleThumbnail::Size - 1) * FMantleVehicleThumbnail::Size].R);
	}
	// Negative control: a body with no triangles draws nothing.
	{
		FMantleSkinnedMesh Empty = Grey.Body;
		Empty.LODs[0].Indices.Reset();
		TArray<FColor> None;
		TestFalse(TEXT("a body with no triangles is not drawn"), FMantleVehicleThumbnail::Draw(Empty, None));
		TestEqual(TEXT("and leaves no pixels"), None.Num(), 0);
	}

	// A vehicle the builder builds carries its picture: a PNG of the right size that decodes to a vehicle on a card.
	FMantleVehicleBuildResult Built = BuildSedan(TEXT("ThumbSedan"));
	for (const FString& Problem : Built.Problems)
	{
		AddError(Problem);
	}
	if (!TestTrue(TEXT("the sedan builds"), Built.bOk && Built.Source != nullptr))
	{
		return false;
	}
	const TArray<uint8>& Png = Built.Source->ThumbnailPng;
	TestFalse(TEXT("a built vehicle has a thumbnail"), Png.IsEmpty());
	int32 Width = 0;
	int32 Height = 0;
	bool bAlpha = false;
	if (TestTrue(TEXT("it is a PNG"), FMantlePng::ReadHeader(Png, Width, Height, bAlpha)))
	{
		TestTrue(*FString::Printf(TEXT("512 x 512, the library thumbnail's size (%d x %d)"), Width, Height), Width == 512 && Height == 512 && Width == FMantleVehicleThumbnail::Size);
	}
	FImage Image;
	if (TestTrue(TEXT("it decodes"), FImageUtils::DecompressImage(Png.GetData(), Png.Num(), Image)))
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		int32 Vehicle = 0;
		int32 Colours = 0;
		CountVehiclePixels(Image, Vehicle, Colours);
		TestTrue(*FString::Printf(TEXT("and shows a shaded vehicle on its card (%d pixels, %d colours)"), Vehicle, Colours), Vehicle > 10000 && Colours >= 4);
	}
	TestTrue(TEXT("the notes do not say the picture failed"), !Built.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("library picture could not be made")); }));

	// Building again replaces the picture rather than adding to it.
	FMantleVehicleBuildResult Again = BuildSedan(TEXT("ThumbSedan"));
	TestTrue(TEXT("a build again keeps one picture"), Again.bOk && Again.Source == Built.Source && !Again.Source->ThumbnailPng.IsEmpty());
	DiscardBuild(Built);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleThumbnailPublishTest,
	"Mantle.Vehicle.Thumbnail.ReachesTheLibraryItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleThumbnailPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleThumbnailTestPrivate;
	FMantleVehicleBuildResult Built = BuildSedan(TEXT("ThumbPublish"));
	if (!TestTrue(TEXT("the sedan builds"), Built.bOk && Built.Source != nullptr) || !TestFalse(TEXT("with a picture"), Built.Source->ThumbnailPng.IsEmpty()))
	{
		for (const FString& Problem : Built.Problems)
		{
			AddError(Problem);
		}
		DiscardBuild(Built);
		return false;
	}

	// Converting the vehicle as a publish does uploads the picture beside it, as a texture with no role (the backend keeps the PNG as it is), and names it on the vehicle.
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	const bool bConverted = Converter.ConvertVehicle(Built.Source, Set);
	for (const FMantleValidationIssue& Issue : Set.Issues)
	{
		AddInfo(Issue.Problem);
	}
	if (TestTrue(TEXT("the vehicle converts"), bConverted && Set.Vehicles.Num() == 1))
	{
		const FMantleConvertedVehicle& Converted = Set.Vehicles[0];
		TestFalse(TEXT("the converted vehicle names its picture"), Converted.ThumbnailKey.IsEmpty());
		const FMantleConvertedTexture* Picture = Set.FindTexture(Converted.ThumbnailKey);
		if (TestNotNull(TEXT("and the picture is among the textures to upload"), Picture))
		{
			TestTrue(TEXT("it is the vehicle's PNG, 512 x 512"), Picture->Bytes == Built.Source->ThumbnailPng && Picture->Format.Equals(TEXT("png"), ESearchCase::CaseSensitive) && Picture->Width == 512 && Picture->Height == 512);
			TestTrue(TEXT("with no role, so the backend keeps it as a PNG"), Picture->Role.IsEmpty());
		}
		TArray<FMantlePlaceUpload> Uploads;
		FString UploadError;
		if (TestTrue(*FString::Printf(TEXT("the texture stage takes uploads (%s)"), *UploadError), Set.TakeUploads(EMantlePlaceUploadStage::Texture, Uploads, UploadError)))
		{
			const FMantlePlaceUpload* Upload = Uploads.FindByPredicate([&Converted](const FMantlePlaceUpload& Candidate) { return Candidate.Key.Equals(Converted.ThumbnailKey, ESearchCase::CaseSensitive); });
			if (TestNotNull(TEXT("the picture is one of them"), Upload))
			{
				TestTrue(TEXT("a texture, with no role"), Upload->Options.Kind.Equals(TEXT("texture"), ESearchCase::CaseSensitive) && Upload->Options.Role.IsEmpty());
			}
		}
	}

	// Negative control: a vehicle with no picture (built before this, or by hand) uploads none and its item has no picture.
	{
		Built.Source->ThumbnailPng.Reset();
		FMantlePlaceAssetSet Bare;
		if (TestTrue(TEXT("the same vehicle without a picture converts"), Converter.ConvertVehicle(Built.Source, Bare) && Bare.Vehicles.Num() == 1))
		{
			TestTrue(TEXT("and names none"), Bare.Vehicles[0].ThumbnailKey.IsEmpty());
		}
	}

	// The item names it: on creation, and on the edit that sets it for an item an earlier publish made. Named only when there is one.
	const FString PictureId = TEXT("ast_u00000000000000000000000000000007");
	const FString AssetId = TEXT("ast_u00000000000000000000000000000009");
	{
		const TSharedPtr<FJsonObject> Created = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Sedan"), false, AssetId, FMantleProvenance(), TEXT("vehicle"), PictureId));
		if (TestTrue(TEXT("the item body is JSON"), Created.IsValid()))
		{
			TestEqual(TEXT("the new item names the picture"), Created->GetStringField(TEXT("thumbnailAssetId")), PictureId);
			TestEqual(TEXT("and its vehicle"), Created->GetStringField(TEXT("primaryAssetId")), AssetId);
		}
		const TSharedPtr<FJsonObject> Without = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Sedan"), false, AssetId));
		TestTrue(TEXT("an item with no picture names none"), Without.IsValid() && !Without->HasField(TEXT("thumbnailAssetId")));
		const TSharedPtr<FJsonObject> Patch = ReadObject(FMantleLibraryPublish::MakeVisibilityPatchBody(true, PictureId));
		if (TestTrue(TEXT("the edit is JSON"), Patch.IsValid()))
		{
			TestEqual(TEXT("an item made by an earlier publish is given the picture"), Patch->GetStringField(TEXT("thumbnailAssetId")), PictureId);
			TestEqual(TEXT("with the visibility it already carried"), Patch->GetStringField(TEXT("visibility")), FString(TEXT("public")));
		}
		const TSharedPtr<FJsonObject> PlainPatch = ReadObject(FMantleLibraryPublish::MakeVisibilityPatchBody(false));
		TestTrue(TEXT("an edit with no picture names none"), PlainPatch.IsValid() && !PlainPatch->HasField(TEXT("thumbnailAssetId")));
	}
	DiscardBuild(Built);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleThumbnailPreviewTest,
	"Mantle.Vehicle.Thumbnail.AutomaticPreviewIsTheUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleThumbnailPreviewTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleThumbnailTestPrivate;
	const FMantleThumbnailChoice Automatic;
	const FMantleThumbnailTarget Square = FMantleThumbnailTarget::Square();

	// Before a build there is no picture to show, only the note that it is taken when the vehicle is built.
	{
		const UMantleVehicleSource* Nothing = nullptr;
		const FMantleThumbnailResult Unbuilt = FMantleThumbnailResolver::Resolve(Automatic, Square,
			FMantleVehicleThumbnail::PreviewFor([&Nothing] { return Nothing; }, []() -> UObject* { return nullptr; }));
		TestFalse(TEXT("an unbuilt vehicle has no preview picture"), Unbuilt.Succeeded());
		TestEqual(TEXT("it says the picture is taken when you build"), Unbuilt.Error, FMantleVehicleThumbnail::TakenWhenBuiltNote());
	}

	FMantleVehicleBuildResult Built = BuildSedan(TEXT("ThumbPreview"));
	if (!TestTrue(TEXT("the sedan builds"), Built.bOk && Built.Source != nullptr) || !TestFalse(TEXT("with a picture"), Built.Source->ThumbnailPng.IsEmpty()))
	{
		DiscardBuild(Built);
		return false;
	}

	// The preview on Automatic is the bytes publishing uploads.
	const UMantleVehicleSource* Source = Built.Source;
	const FMantleThumbnailResult Preview = FMantleThumbnailResolver::Resolve(Automatic, Square,
		FMantleVehicleThumbnail::PreviewFor([Source] { return Source; }, []() -> UObject* { return nullptr; }));
	if (TestTrue(*FString::Printf(TEXT("the built vehicle has a preview (%s)"), *Preview.Error), Preview.Succeeded()))
	{
		const TArray<uint8> PreviewPng = FMantleThumbnailResolver::ToPng(Preview, Square);
		FMantlePlaceAssetConverter Converter;
		FMantlePlaceAssetSet Set;
		if (TestTrue(TEXT("the vehicle converts"), Converter.ConvertVehicle(Built.Source, Set) && Set.Vehicles.Num() == 1))
		{
			const FMantleConvertedTexture* Upload = Set.FindTexture(Set.Vehicles[0].ThumbnailKey);
			if (TestNotNull(TEXT("and uploads its picture"), Upload))
			{
				TestTrue(TEXT("the Automatic preview's bytes are the uploaded bytes"), PreviewPng == Upload->Bytes);
			}
		}
	}
	DiscardBuild(Built);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
