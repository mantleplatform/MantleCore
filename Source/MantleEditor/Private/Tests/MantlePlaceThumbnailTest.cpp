// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceExporter.h"
#include "MantlePng.h"
#include "MantleSha256.h"
#include "MantlePlaceThumbnail.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The place's catalogue picture: the automatic view's framing, the picture prepared as a role-less JPEG, and the
// exporter naming it in metadata.thumbnail only when Publish prepared one. The capture itself needs a renderer and is
// checked by publishing.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceThumbnailTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FTestWorld
	{
		UWorld* World = nullptr;
		FTestWorld() { World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePlaceThumbnailTestWorld")); }
		~FTestWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceThumbnailAutoViewTest,
	"Mantle.Editor.PlaceThumbnail.AutoViewFramesFromBehindTheSpawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceThumbnailAutoViewTest::RunTest(const FString& Parameters)
{
	// A level 100 m long in X, its middle 50 m ahead of a spawn at the origin.
	const FBox Bounds(FVector(0.0, -2000.0, 0.0), FVector(10000.0, 2000.0, 1000.0));
	const FTransform Spawn(FRotator(0.0, 90.0, 0.0), FVector::ZeroVector);
	const FMantlePlaceThumbnailView View = FMantlePlaceThumbnail::AutoView(Bounds, &Spawn);
	TestTrue(TEXT("automatic"), View.bAutomatic);
	TestEqual(TEXT("the automatic field of view"), View.FOV, FMantlePlaceThumbnail::AutoFOV);
	TestTrue(TEXT("behind the spawn, away from the middle"), View.Location.X < 0.0);
	TestTrue(TEXT("on the spawn's line to the middle"), FMath::IsNearlyZero(View.Location.Y, 1.0));
	TestTrue(TEXT("above a standing player's eyes"), View.Location.Z > FMantlePlaceThumbnail::EyeHeightCm);
	TestTrue(TEXT("looking toward the middle (+X), not the spawn's own facing (+Y)"), FMath::IsNearlyZero(FRotator::NormalizeAxis(View.Rotation.Yaw), 0.5));
	TestTrue(TEXT("looking down"), View.Rotation.Pitch < 0.0);
	TestTrue(TEXT("level"), FMath::IsNearlyZero(View.Rotation.Roll));

	const FMantlePlaceThumbnailView Again = FMantlePlaceThumbnail::AutoView(Bounds, &Spawn);
	TestTrue(TEXT("the same level frames the same picture"), Again.Location.Equals(View.Location, 0.0) && Again.Rotation.Equals(View.Rotation, 0.0));

	// The spawn at the middle: along its own facing (+Y).
	const FBox Around(FVector(-1000.0), FVector(1000.0));
	const FMantlePlaceThumbnailView Facing = FMantlePlaceThumbnail::AutoView(Around, &Spawn);
	TestTrue(TEXT("a spawn in the middle looks the way it faces"), FMath::IsNearlyEqual(FRotator::NormalizeAxis(Facing.Rotation.Yaw), 90.0, 0.5));
	TestTrue(TEXT("from behind it"), Facing.Location.Y < 0.0);

	// No spawn, nothing drawn: still a valid view from above the origin.
	const FMantlePlaceThumbnailView Empty = FMantlePlaceThumbnail::AutoView(FBox(ForceInit), nullptr);
	TestTrue(TEXT("an empty level still has a view above the ground"), Empty.Location.Z > 0.0 && !Empty.Location.ContainsNaN());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceThumbnailConvertTest,
	"Mantle.Editor.PlaceThumbnail.PreparedAsAPictureWithNoRole",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceThumbnailConvertTest::RunTest(const FString& Parameters)
{
	TArray<FColor> Pixels;
	Pixels.Init(FColor(40, 120, 200, 255), FMantlePlaceThumbnail::Width * FMantlePlaceThumbnail::Height);
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	TestTrue(TEXT("prepared"), Converter.ConvertThumbnail(Pixels, FMantlePlaceThumbnail::Width, FMantlePlaceThumbnail::Height, Set));
	const FMantleConvertedTexture* Texture = Set.FindTexture(Set.Thumbnail);
	if (!TestNotNull(TEXT("recorded as the set's thumbnail texture"), Texture))
	{
		return false;
	}
	// No role, so the backend keeps the JPEG the catalogue serves instead of cooking it to MTX1.
	TestTrue(TEXT("no texture role"), Texture->Role.IsEmpty());
	TestEqual(TEXT("a JPEG"), Texture->Format, FString(TEXT("jpeg")));
	TestTrue(TEXT("starting with the JPEG start marker and the Mantle comment segment"), Texture->Bytes.Num() > 4
		&& Texture->Bytes[0] == 0xFF && Texture->Bytes[1] == 0xD8 && Texture->Bytes[2] == 0xFF && Texture->Bytes[3] == 0xFE);
	TestEqual(TEXT("its hash is its bytes'"), Texture->Hash, FMantleSha256::AssetHash(Texture->Bytes));
	// Before the upload below takes them (TakeUploads moves a set's bytes into the uploads).
	TestFalse(TEXT("with bytes"), Texture->Bytes.IsEmpty());
	TArray<FMantlePlaceUpload> Uploads;
	FString Error;
	TestTrue(TEXT("built as a texture upload"), Set.TakeUploads(EMantlePlaceUploadStage::Texture, Uploads, Error));
	const FMantlePlaceUpload* Upload = Uploads.FindByPredicate([&Set](const FMantlePlaceUpload& Candidate) { return Candidate.Key == Set.Thumbnail; });
	if (TestNotNull(TEXT("the thumbnail's upload"), Upload))
	{
		TestEqual(TEXT("uploaded as a texture"), Upload->Options.Kind, FString(TEXT("texture")));
		TestTrue(TEXT("with no role option"), Upload->Options.Role.IsEmpty());
	}

	// The same pixels prepared as a colour map hash differently from the role-less JPEG thumbnail, so dedupe can't hand back that MTX1 asset.
	FMantleConvertedTexture Colour;
	TArray<FMantleValidationIssue> Issues;
	TestTrue(TEXT("the same pixels prepare as a colour map"), FMantlePlaceAssetConverter::PrepareTexture(
		FMantlePng::EncodeRGBA8(FMantlePlaceThumbnail::Width, FMantlePlaceThumbnail::Height, Pixels), EMantlePlaceTextureSlot::BaseColor,
		/*bKeepAlpha*/ false, FMantlePlaceAssetLimits(), TEXT("colour"), /*bWarnDownscale*/ false, Colour, Issues));
	TestEqual(TEXT("which has the colour role"), Colour.Role, FString(TEXT("color")));
	TestNotEqual(TEXT("and other bytes than the thumbnail"), Colour.Hash, Texture->Hash);
	TestEqual(TEXT("16:9 at full size"), FIntPoint(Texture->Width, Texture->Height), FIntPoint(FMantlePlaceThumbnail::Width, FMantlePlaceThumbnail::Height));

	FMantlePlaceAssetSet Bad;
	TestFalse(TEXT("pixels that don't match the size are refused"), Converter.ConvertThumbnail(Pixels, 10, 10, Bad));
	TestTrue(TEXT("leaving no thumbnail"), Bad.Thumbnail.IsEmpty());
	TestFalse(TEXT("as a warning, not an error"), Bad.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceThumbnailExportTest,
	"Mantle.Editor.PlaceThumbnail.ExporterNamesIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceThumbnailExportTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceThumbnailTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}

	// Without Publish's set: no thumbnail.
	const FMantleExportResult Plain = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	TestTrue(TEXT("no thumbnail without one prepared"), Plain.Place.Metadata.Thumbnail.IsEmpty());
	TestFalse(TEXT("and none written"), FMantlePlaceExporter::ToJson(Plain.Place).Contains(TEXT("\"thumbnail\"")));

	// With one prepared and uploaded: its id in metadata.thumbnail and in the manifest.
	TArray<FColor> Pixels;
	Pixels.Init(FColor(200, 80, 30, 255), FMantlePlaceThumbnail::Width * FMantlePlaceThumbnail::Height);
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("prepared"), Converter.ConvertThumbnail(Pixels, FMantlePlaceThumbnail::Width, FMantlePlaceThumbnail::Height, Set)))
	{
		return false;
	}
	const FString Id = TEXT("ast_u0123456789abcdef0123456789abcdef");
	const FMantleConvertedTexture* Texture = Set.FindTexture(Set.Thumbnail);
	Set.RecordUpload(Set.Thumbnail, Id, Texture->Hash, Texture->Bytes.Num());
	FMantleExportOptions Options;
	Options.Assets = &Set;
	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, Options);
	TestEqual(TEXT("metadata.thumbnail is the uploaded id"), Exported.Place.Metadata.Thumbnail, Id);
	TestTrue(TEXT("and the manifest lists it"), Exported.Place.Assets.ContainsByPredicate([&Id](const FMantleAssetRef& Ref) { return Ref.Id == Id; }));
	TestTrue(TEXT("and the place file carries it"), FMantlePlaceExporter::ToJson(Exported.Place).Contains(TEXT("\"thumbnail\":\"") + Id + TEXT("\"")));
	return true;
}

#endif
