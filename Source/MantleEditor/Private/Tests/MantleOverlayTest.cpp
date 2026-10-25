// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleOverlayChecks.h"
#include "MantlePng.h"
#include "MantleThumbnailChoice.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Scar and tattoo overlays, creator side: the mask and normal-map rules, and the exported package and item body.
// Synthetic images only; no assets, no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayTestPrivate
{
	/** A Size x Size image: Paint(X, Y) gives each pixel. */
	template <typename PaintType>
	TArray<FColor> OverlayImage(const int32 Size, PaintType Paint)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Pixels[Y * Size + X] = Paint(X, Y);
			}
		}
		return Pixels;
	}

	/** A painted disc in the middle on black: greyscale, some paint, not saturated. */
	TArray<FColor> GreyDisc(const int32 Size)
	{
		return OverlayImage(Size, [Size](const int32 X, const int32 Y)
		{
			const int32 DX = X - Size / 2;
			const int32 DY = Y - Size / 2;
			const uint8 Value = DX * DX + DY * DY < (Size / 4) * (Size / 4) ? 255 : 0;
			return FColor(Value, Value, Value, 255);
		});
	}

	/** The same disc in the red channel only. */
	TArray<FColor> RedDisc(const int32 Size)
	{
		TArray<FColor> Pixels = GreyDisc(Size);
		for (FColor& Pixel : Pixels)
		{
			Pixel.G = 0;
			Pixel.B = 0;
		}
		return Pixels;
	}

	/** A flat tangent-space normal map with a little tilt. */
	TArray<FColor> NormalImage(const int32 Size)
	{
		return OverlayImage(Size, [](const int32 X, const int32)
		{
			return FColor(static_cast<uint8>(120 + (X % 16)), 128, 240, 255);
		});
	}

	bool HasError(const TArray<FMantleCosmeticIssue>& Issues, const TCHAR* Fragment)
	{
		for (const FMantleCosmeticIssue& Issue : Issues)
		{
			if (Issue.Severity == EMantleCosmeticSeverity::Error && Issue.ToString().Contains(Fragment))
			{
				return true;
			}
		}
		return false;
	}

	bool MaskPasses(const int32 Width, const int32 Height, const TArray<FColor>& Pixels, TArray<FMantleCosmeticIssue>& Issues)
	{
		return FMantleOverlayChecks::CheckMask(TEXT("The mask"), Width, Height, Pixels, Issues);
	}

	/** Writes Pixels as a PNG in the saved folder; the path, or empty. */
	FString WritePng(const int32 Width, const int32 Height, const TArray<FColor>& Pixels, const TCHAR* Prefix)
	{
		const FString File = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), Prefix, TEXT(".png"));
		return FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Width, Height, Pixels), *File) ? File : FString();
	}

	TSharedPtr<FJsonObject> ReadOverlayJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Root);
		return Root;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOverlayMaskChecksTest,
	"Mantle.Editor.Overlay.MaskAndNormalChecks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOverlayMaskChecksTest::RunTest(const FString& Parameters)
{
	using namespace MantleOverlayTestPrivate;
	TArray<FMantleCosmeticIssue> Issues;

	TestTrue(TEXT("a greyscale 256 mask passes"), MaskPasses(256, 256, GreyDisc(256), Issues));
	TestTrue(TEXT("a 1024 mask passes"), MaskPasses(1024, 1024, GreyDisc(1024), Issues));
	TestTrue(TEXT("a red-channel-only mask passes"), MaskPasses(64, 64, RedDisc(64), Issues));
	TestEqual(TEXT("and none of them raised anything"), Issues.Num(), 0);

	Issues.Reset();
	TestFalse(TEXT("a non-square mask fails"), MaskPasses(64, 32, GreyDisc(64), Issues));
	TestTrue(TEXT("and says it must be square"), HasError(Issues, TEXT("square")));

	Issues.Reset();
	TestFalse(TEXT("a 60 x 60 mask fails"), MaskPasses(60, 60, GreyDisc(60), Issues));
	TestTrue(TEXT("and says power of two"), HasError(Issues, TEXT("power of two")));

	Issues.Reset();
	TestFalse(TEXT("a 2048 mask fails"), MaskPasses(2048, 2048, TArray<FColor>(), Issues));
	TestTrue(TEXT("and names the 1024 limit"), HasError(Issues, TEXT("at most 1024")));

	Issues.Reset();
	const TArray<FColor> Black = OverlayImage(64, [](const int32, const int32) { return FColor(0, 0, 0, 255); });
	TestFalse(TEXT("a blank mask fails"), MaskPasses(64, 64, Black, Issues));
	TestTrue(TEXT("and says it is blank"), HasError(Issues, TEXT("blank")));

	Issues.Reset();
	const TArray<FColor> White = OverlayImage(64, [](const int32, const int32) { return FColor(255, 255, 255, 255); });
	TestFalse(TEXT("a solid white mask fails"), MaskPasses(64, 64, White, Issues));
	TestTrue(TEXT("and says it covers everything"), HasError(Issues, TEXT("solid white")));

	Issues.Reset();
	const TArray<FColor> Colour = OverlayImage(64, [](const int32 X, const int32 Y) { return FColor(static_cast<uint8>(X * 4), static_cast<uint8>(Y * 4), 200, 255); });
	TestFalse(TEXT("a colour image fails"), MaskPasses(64, 64, Colour, Issues));
	TestTrue(TEXT("and says the mask must be one channel"), HasError(Issues, TEXT("one channel")));

	// The normal map rules.
	Issues.Reset();
	TestTrue(TEXT("a normal map passes"), FMantleOverlayChecks::CheckNormal(TEXT("The normal map"), 128, 128, NormalImage(128), Issues));
	TestEqual(TEXT("without issues"), Issues.Num(), 0);
	TestFalse(TEXT("a colour photo is not a normal map"), FMantleOverlayChecks::CheckNormal(TEXT("The normal map"), 64, 64,
		OverlayImage(64, [](const int32, const int32) { return FColor(220, 40, 30, 255); }), Issues));
	TestTrue(TEXT("and says so"), HasError(Issues, TEXT("normal map")));
	Issues.Reset();
	TestFalse(TEXT("a non-square normal map fails"), FMantleOverlayChecks::CheckNormal(TEXT("The normal map"), 128, 64, NormalImage(128), Issues));

	// A normal map belongs to the scar slots.
	Issues.Reset();
	TestTrue(TEXT("a normal map is fine on scar_body"), FMantleOverlayChecks::CheckNormalAllowed(TEXT("scar_body"), true, Issues));
	TestTrue(TEXT("a normal map is fine on scar_face"), FMantleOverlayChecks::CheckNormalAllowed(TEXT("scar_face"), true, Issues));
	TestTrue(TEXT("no normal map is fine on a tattoo"), FMantleOverlayChecks::CheckNormalAllowed(TEXT("tattoo_body"), false, Issues));
	TestFalse(TEXT("a normal map on tattoo_body is refused"), FMantleOverlayChecks::CheckNormalAllowed(TEXT("tattoo_body"), true, Issues));
	TestFalse(TEXT("a normal map on tattoo_face is refused"), FMantleOverlayChecks::CheckNormalAllowed(TEXT("tattoo_face"), true, Issues));
	TestTrue(TEXT("with a plain sentence"), HasError(Issues, TEXT("Tattoos do not take a normal map")));

	// The slots and where they are drawn.
	TestEqual(TEXT("four overlay slots"), FMantleOverlayChecks::Slots().Num(), 4);
	TestTrue(TEXT("scar_face is on the face"), FMantleOverlayChecks::IsFaceSlot(TEXT("scar_face")));
	TestTrue(TEXT("tattoo_face is on the face"), FMantleOverlayChecks::IsFaceSlot(TEXT("tattoo_face")));
	TestFalse(TEXT("tattoo_body is on the body"), FMantleOverlayChecks::IsFaceSlot(TEXT("tattoo_body")));
	TestFalse(TEXT("hair is no overlay slot"), FMantleOverlayChecks::IsOverlaySlot(TEXT("hair")));

	// The exported mask is one canonical image: the mask value in every colour channel, opaque.
	const TArray<FColor> Canonical = FMantleOverlayChecks::CanonicalMask(RedDisc(32));
	bool bCanonical = Canonical.Num() == 32 * 32;
	for (const FColor& Pixel : Canonical)
	{
		bCanonical &= Pixel.R == Pixel.G && Pixel.R == Pixel.B && Pixel.A == 255;
	}
	TestTrue(TEXT("a red-only mask exports as grey"), bCanonical);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOverlayExportTest,
	"Mantle.Editor.Overlay.ExportManifest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOverlayExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleOverlayTestPrivate;
	TestEqual(TEXT("the overlay class is limits.json's overlay"), FString(ContractItemClass(EMantleCosmeticClass::Overlay)), FString(TEXT("overlay")));

	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}
	TestTrue(TEXT("the panel offers the four overlay slots"), CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Overlay) == FMantleOverlayChecks::Slots());

	const FString MaskFile = WritePng(128, 128, RedDisc(128), TEXT("overlay_mask"));
	const FString NormalFile = WritePng(128, 128, NormalImage(128), TEXT("overlay_normal"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*MaskFile);
		IFileManager::Get().Delete(*NormalFile);
	};
	if (!TestTrue(TEXT("the test images were written"), !MaskFile.IsEmpty() && !NormalFile.IsEmpty()))
	{
		return false;
	}

	FMantleCosmeticDraft Draft;
	Draft.Class = EMantleCosmeticClass::Overlay;
	Draft.Slot = TEXT("scar_face");
	Draft.Name = TEXT("Test scar");
	Draft.bPublic = false;
	Draft.bRenderThumbnail = false;
	Draft.OverlayMaskFile = MaskFile;
	Draft.OverlayNormalFile = NormalFile;

	const FMantleCosmeticPackage Package = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	for (const FMantleCosmeticIssue& Issue : Package.Issues)
	{
		if (Issue.Severity == EMantleCosmeticSeverity::Error)
		{
			AddError(Issue.ToString());
		}
	}
	if (!TestFalse(TEXT("a scar with a mask and a normal map has no errors"), Package.HasErrors()))
	{
		return false;
	}
	TestTrue(TEXT("the package is an overlay"), Package.Class == EMantleCosmeticClass::Overlay);
	TestEqual(TEXT("in the scar_face slot"), Package.Slot, FString(TEXT("scar_face")));
	TestTrue(TEXT("with no mesh"), Package.MeshKey.IsEmpty() && Package.FaceMeshKey.IsEmpty() && Package.ClipKey.IsEmpty());

	const FMantleCosmeticUploadAsset* Mask = Package.FindAsset(Package.MaskKey);
	const FMantleCosmeticUploadAsset* Normal = Package.FindAsset(Package.OverlayNormalKey);
	const FMantleCosmeticUploadAsset* Thumbnail = Package.FindAsset(Package.ThumbnailKey);
	if (!TestTrue(TEXT("the mask, the normal map and a thumbnail are in the package"), Mask && Normal && Thumbnail))
	{
		return false;
	}
	TestEqual(TEXT("the mask is a texture"), Mask->Options.Kind, FString(TEXT("texture")));
	TestEqual(TEXT("the mask is data"), Mask->Options.Role, FString(TEXT("data")));
	TestEqual(TEXT("the normal map is a texture"), Normal->Options.Kind, FString(TEXT("texture")));
	TestEqual(TEXT("the normal map has the normal role"), Normal->Options.Role, FString(TEXT("normal")));
	TestEqual(TEXT("three files upload: mask, normal map, thumbnail"), Package.Assets.Num(), 3);
	int32 Width = 0;
	int32 Height = 0;
	bool bAlpha = false;
	TestTrue(TEXT("the mask is a PNG"), FMantlePng::ReadHeader(Mask->Bytes, Width, Height, bAlpha));
	TestEqual(TEXT("at its own size"), Width, 128);
	TestEqual(TEXT("square"), Height, 128);

	// The item body.
	TMap<FString, FString> Ids;
	Ids.Add(Package.MaskKey, TEXT("ast_u00000000000000000000000000000001"));
	Ids.Add(Package.OverlayNormalKey, TEXT("ast_u00000000000000000000000000000002"));
	Ids.Add(Package.ThumbnailKey, TEXT("ast_u00000000000000000000000000000003"));
	FString Json;
	FString Error;
	if (!TestTrue(TEXT("the item body is made"), Package.MakeItemJson(Ids, Json, Error)))
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject> Item = ReadOverlayJson(Json);
	if (!TestTrue(TEXT("it is JSON"), Item.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("slot"), Item->GetStringField(TEXT("slot")), FString(TEXT("scar_face")));
	TestEqual(TEXT("mask"), Item->GetStringField(TEXT("textureAssetId")), FString(TEXT("ast_u00000000000000000000000000000001")));
	TestEqual(TEXT("normal"), Item->GetStringField(TEXT("normalAssetId")), FString(TEXT("ast_u00000000000000000000000000000002")));
	TestEqual(TEXT("thumbnail"), Item->GetStringField(TEXT("thumbnailAssetId")), FString(TEXT("ast_u00000000000000000000000000000003")));
	TestEqual(TEXT("private"), Item->GetStringField(TEXT("visibility")), FString(TEXT("private")));
	TestFalse(TEXT("no mesh field"), Item->HasField(TEXT("meshAssetId")));

	// A tattoo with only a mask: no normal field.
	Draft.Slot = TEXT("tattoo_body");
	Draft.OverlayNormalFile.Reset();
	const FMantleCosmeticPackage Tattoo = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestFalse(TEXT("a tattoo with a mask has no errors"), Tattoo.HasErrors());
	TMap<FString, FString> TattooIds;
	TattooIds.Add(Tattoo.MaskKey, TEXT("ast_u00000000000000000000000000000004"));
	TattooIds.Add(Tattoo.ThumbnailKey, TEXT("ast_u00000000000000000000000000000005"));
	if (TestTrue(TEXT("its item body is made"), Tattoo.MakeItemJson(TattooIds, Json, Error)))
	{
		const TSharedPtr<FJsonObject> TattooItem = ReadOverlayJson(Json);
		TestTrue(TEXT("the tattoo body has a mask and no normal map"),
			TattooItem.IsValid() && TattooItem->HasField(TEXT("textureAssetId")) && !TattooItem->HasField(TEXT("normalAssetId")));
	}

	// A normal map on a tattoo is refused before anything is exported.
	Draft.OverlayNormalFile = NormalFile;
	const FMantleCosmeticPackage Refused = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestTrue(TEXT("a normal map on a tattoo is an error"), HasError(Refused.Issues, TEXT("Tattoos do not take a normal map")));
	TestEqual(TEXT("and nothing uploads"), Refused.Assets.Num(), 0);

	// A blank mask is refused with its reason.
	const FString BlankFile = WritePng(64, 64, OverlayImage(64, [](const int32, const int32) { return FColor(0, 0, 0, 255); }), TEXT("overlay_blank"));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*BlankFile); };
	Draft.OverlayNormalFile.Reset();
	Draft.OverlayMaskFile = BlankFile;
	const FMantleCosmeticPackage Blank = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestTrue(TEXT("a blank mask is an error"), HasError(Blank.Issues, TEXT("blank")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOverlayCustomThumbnailTest,
	"Mantle.Editor.Overlay.CustomThumbnail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// #124: an overlay takes the shared Thumbnail section too: a custom image is its 512 x 512 thumbnail, and one that cannot be used refuses the export.
bool FMantleOverlayCustomThumbnailTest::RunTest(const FString& Parameters)
{
	using namespace MantleOverlayTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the contract loads"), Contract))
	{
		return false;
	}
	const FString MaskFile = WritePng(128, 128, RedDisc(128), TEXT("overlay_thumb_mask"));
	const FString PictureFile = WritePng(640, 640, RedDisc(640), TEXT("overlay_thumb_picture"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*MaskFile);
		IFileManager::Get().Delete(*PictureFile);
	};
	FMantleCosmeticDraft Draft;
	Draft.Class = EMantleCosmeticClass::Overlay;
	Draft.Slot = TEXT("tattoo_body");
	Draft.Name = TEXT("Test tattoo");
	Draft.bPublic = false;
	Draft.bRenderThumbnail = false;
	Draft.OverlayMaskFile = MaskFile;
	Draft.Thumbnail.Mode = EMantleThumbnailMode::Custom;
	Draft.Thumbnail.ImageFile.FilePath = PictureFile;
	const FMantleCosmeticPackage Package = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestFalse(TEXT("a custom thumbnail exports without errors"), Package.HasErrors());
	const FMantleCosmeticUploadAsset* Thumbnail = Package.FindAsset(Package.ThumbnailKey);
	int32 Width = 0, Height = 0;
	bool bAlpha = false;
	TestTrue(TEXT("the thumbnail is a PNG"), Thumbnail && FMantlePng::ReadHeader(Thumbnail->Bytes, Width, Height, bAlpha));
	TestEqual(TEXT("512 wide"), Width, Contract->ThumbnailSize);

	Draft.Thumbnail.ImageFile.FilePath = PictureFile + TEXT(".missing.png");
	TestTrue(TEXT("a custom picture that is not there refuses the export"), FMantleCosmeticExport::BuildCore(Draft, *Contract).HasErrors());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
