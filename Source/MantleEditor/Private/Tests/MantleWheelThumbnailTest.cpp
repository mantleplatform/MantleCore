// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantleThumbnailChoice.h"
#include "MantleThumbnailStore.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The wheel's library picture (#124) through the shared thumbnail system: how its choice is kept and read, what its picture is called, and that
// the shared resolver gives a wheel what it gives a weapon. The render itself needs a renderer and is checked in the editor.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWheelThumbnailTestPrivate
{
	FString TempConfig()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("MantleWheelThumbnailTest.ini"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelThumbnailKeysTest, "Mantle.Wheel.Thumbnail.Keys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelThumbnailKeysTest::RunTest(const FString& Parameters)
{
	// The choice is remembered under the wheel's library path, the item's own identity.
	TestEqual(TEXT("store key"), FMantlePlaceAssetConverter::WheelThumbnailStoreKey(TEXT("Split spoke")), FString(TEXT("/Game/MantleWheels/Split spoke")));
	// The picture is keyed by its bytes' hash, apart from a weapon's, so equal pictures share one upload and a changed one is a new one.
	TestEqual(TEXT("picture key"), FMantlePlaceAssetConverter::WheelPictureKey(TEXT("sha256:abc123")), FString(TEXT("wheelpicture:abc123")));
	TestNotEqual(TEXT("different pictures, different keys"), FMantlePlaceAssetConverter::WheelPictureKey(TEXT("sha256:abc")), FMantlePlaceAssetConverter::WheelPictureKey(TEXT("sha256:abd")));
	// A wheel converted without a picture names none.
	const FMantleConvertedWheel Wheel;
	TestTrue(TEXT("no picture by default"), Wheel.ThumbnailKey.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelThumbnailChoiceTest, "Mantle.Wheel.Thumbnail.Choice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelThumbnailChoiceTest::RunTest(const FString& Parameters)
{
	const FString File = MantleWheelThumbnailTestPrivate::TempConfig();
	IFileManager::Get().Delete(*File, false, true, true);
	const FString Key = FMantlePlaceAssetConverter::WheelThumbnailStoreKey(TEXT("Split spoke"));

	// No saved choice: Automatic, and Load says so.
	FMantleThumbnailChoice Choice;
	Choice.Mode = EMantleThumbnailMode::Custom;
	TestFalse(TEXT("nothing saved"), FMantleThumbnailStore::Load(Key, TEXT("wheel"), Choice, File));
	TestEqual(TEXT("defaults to Automatic"), Choice.Mode, EMantleThumbnailMode::Automatic);

	// A saved angle comes back as saved.
	FMantleThumbnailChoice Saved;
	Saved.Mode = EMantleThumbnailMode::Angle;
	Saved.Angle = FRotator(-20.0, 135.0, 0.0);
	Saved.Zoom = 1.5f;
	FMantleThumbnailStore::Save(Key, TEXT("wheel"), Saved, File);
	FMantleThumbnailChoice Loaded;
	TestTrue(TEXT("saved choice found"), FMantleThumbnailStore::Load(Key, TEXT("wheel"), Loaded, File));
	TestTrue(TEXT("the same choice"), Loaded == Saved);

	// A weapon of the same name keeps its own choice; the wheel's is not read for it.
	FMantleThumbnailChoice WeaponChoice;
	TestFalse(TEXT("a weapon's choice is separate"), FMantleThumbnailStore::Load(Key, TEXT("weapon"), WeaponChoice, File));

	// Choosing Automatic again forgets it.
	FMantleThumbnailStore::Save(Key, TEXT("wheel"), FMantleThumbnailChoice(), File);
	FMantleThumbnailChoice After;
	TestFalse(TEXT("forgotten"), FMantleThumbnailStore::Load(Key, TEXT("wheel"), After, File));
	IFileManager::Get().Delete(*File, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleWheelThumbnailResolveTest, "Mantle.Wheel.Thumbnail.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleWheelThumbnailResolveTest::RunTest(const FString& Parameters)
{
	const FMantleThumbnailTarget Target = FMantleThumbnailTarget::Square();

	// Automatic: the render's picture, cropped and sized to the card.
	const FMantleThumbnailResult Made = FMantleThumbnailResolver::Resolve(FMantleThumbnailChoice(), Target,
		[](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError) -> bool
		{
			OutWidth = 512;
			OutHeight = 512;
			OutPixels.Init(FColor(10, 20, 30, 255), OutWidth * OutHeight);
			return true;
		});
	TestTrue(TEXT("automatic picture made"), Made.Succeeded());
	TestTrue(TEXT("a PNG comes out"), FMantleThumbnailResolver::ToPng(Made, Target).Num() > 0);

	// No renderer: no picture and a reason, never a crash; the wheel publishes without one.
	const FMantleThumbnailResult None = FMantleThumbnailResolver::Resolve(FMantleThumbnailChoice(), Target,
		[](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError) -> bool
		{
			OutError = TEXT("no renderer");
			return false;
		});
	TestFalse(TEXT("no picture without a renderer"), None.Succeeded());
	TestFalse(TEXT("with a reason"), None.Error.IsEmpty());

	// A custom image that is not there is refused with a reason (the converter turns that into a warning).
	FMantleThumbnailChoice Custom;
	Custom.Mode = EMantleThumbnailMode::Custom;
	Custom.ImageFile.FilePath = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("NoSuchWheelPicture.png");
	const FMantleThumbnailResult Missing = FMantleThumbnailResolver::Resolve(Custom, Target,
		[](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError) -> bool
		{
			OutError = TEXT("must not render");
			return false;
		});
	TestFalse(TEXT("missing custom image refused"), Missing.Succeeded());
	TestFalse(TEXT("with a reason"), Missing.Error.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
