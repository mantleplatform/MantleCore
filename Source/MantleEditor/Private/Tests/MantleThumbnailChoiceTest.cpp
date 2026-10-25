// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceThumbnail.h"
#include "MantlePng.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEmoteCheck.h"
#include "MantleModelThumbnail.h"
#include "MantleThumbnailChoice.h"
#include "MantleThumbnailStore.h"

#include "Engine/World.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The shared thumbnail resolver (#124): the crop, the size check, the resize, and Custom images read from a PNG file. The kind's renders need a
// renderer and are checked with the catalog thumbnail capture; here a stand-in render gives the pixels.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleThumbnailChoiceTestPrivate
{
	TArray<FColor> Solid(int32 Width, int32 Height, FColor Color)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Color, Width * Height);
		return Pixels;
	}

	/** The left half red, the right half blue. */
	TArray<FColor> HalvesRedBlue(int32 Width, int32 Height)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Pixels[Y * Width + X] = X < Width / 2 ? FColor(255, 0, 0, 255) : FColor(0, 0, 255, 255);
			}
		}
		return Pixels;
	}

	FString TempPng(const TCHAR* Name, int32 Width, int32 Height, TConstArrayView<FColor> Pixels)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
		FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Width, Height, Pixels), *Path);
		return Path;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailCropTest, "Mantle.Editor.Thumbnail.Crop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailCropTest::RunTest(const FString&)
{
	const FMantleThumbnailTarget Square = FMantleThumbnailTarget::Square();
	const FMantleThumbnailTarget Wide = FMantleThumbnailTarget::Wide();
	// A wide picture loses its sides, a tall one its top and bottom, both centred.
	TestEqual(TEXT("1000x500 to a square"), FMantleThumbnailResolver::CropRect(1000, 500, Square), FIntRect(250, 0, 750, 500));
	TestEqual(TEXT("500x1000 to a square"), FMantleThumbnailResolver::CropRect(500, 1000, Square), FIntRect(0, 250, 500, 750));
	TestEqual(TEXT("a square stays whole"), FMantleThumbnailResolver::CropRect(512, 512, Square), FIntRect(0, 0, 512, 512));
	// 16:9 from a square: full width, 562 tall.
	const FIntRect FromSquare = FMantleThumbnailResolver::CropRect(1000, 1000, Wide);
	TestEqual(TEXT("1000x1000 to 16:9 keeps the full width"), FromSquare.Width(), 1000);
	TestEqual(TEXT("1000x1000 to 16:9 is 562 tall"), FromSquare.Height(), 562);
	TestEqual(TEXT("1920x1080 to 16:9 stays whole"), FMantleThumbnailResolver::CropRect(1920, 1080, Wide), FIntRect(0, 0, 1920, 1080));
	TestEqual(TEXT("an empty picture crops to nothing"), FMantleThumbnailResolver::CropRect(0, 0, Square).Area(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailSizeJudgeTest, "Mantle.Editor.Thumbnail.SizeJudge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailSizeJudgeTest::RunTest(const FString&)
{
	const FMantleThumbnailTarget Square = FMantleThumbnailTarget::Square();
	const FMantleThumbnailTarget Wide = FMantleThumbnailTarget::Wide();
	// Square: fine from 512, enlarged from 256 to 511, refused below 256 (measured on the part that is kept).
	TestTrue(TEXT("512 is fine"), FMantleThumbnailResolver::JudgeSize(512, 512, Square) == EMantleThumbnailSize::Fine);
	TestTrue(TEXT("4000x512 is fine (its square part is 512)"), FMantleThumbnailResolver::JudgeSize(4000, 512, Square) == EMantleThumbnailSize::Fine);
	TestTrue(TEXT("511 is enlarged"), FMantleThumbnailResolver::JudgeSize(511, 511, Square) == EMantleThumbnailSize::Enlarged);
	TestTrue(TEXT("256 is enlarged"), FMantleThumbnailResolver::JudgeSize(256, 256, Square) == EMantleThumbnailSize::Enlarged);
	TestTrue(TEXT("255 is refused"), FMantleThumbnailResolver::JudgeSize(255, 255, Square) == EMantleThumbnailSize::TooSmall);
	TestTrue(TEXT("a thin strip is refused"), FMantleThumbnailResolver::JudgeSize(2000, 100, Square) == EMantleThumbnailSize::TooSmall);
	// A place: the same rule at its size.
	TestTrue(TEXT("1280x720 is fine"), FMantleThumbnailResolver::JudgeSize(1280, 720, Wide) == EMantleThumbnailSize::Fine);
	TestTrue(TEXT("640x360 is enlarged"), FMantleThumbnailResolver::JudgeSize(640, 360, Wide) == EMantleThumbnailSize::Enlarged);
	TestTrue(TEXT("600x337 is refused"), FMantleThumbnailResolver::JudgeSize(600, 337, Wide) == EMantleThumbnailSize::TooSmall);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailFitTest, "Mantle.Editor.Thumbnail.Fit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailFitTest::RunTest(const FString&)
{
	using namespace MantleThumbnailChoiceTestPrivate;
	const FMantleThumbnailTarget Square = FMantleThumbnailTarget::Square();
	TArray<FColor> Out;
	FString Warning;
	FString Error;

	// Shrinking keeps the picture: a 2048 x 1024 red | blue picture is cut to the middle 1024 square (red left, blue right) and shrunk.
	TestTrue(TEXT("a big wide picture fits"), FMantleThumbnailResolver::FitToTarget(HalvesRedBlue(2048, 1024), 2048, 1024, Square, Out, Warning, Error));
	TestEqual(TEXT("512 x 512 pixels"), Out.Num(), 512 * 512);
	TestTrue(TEXT("no warning, no error"), Warning.IsEmpty() && Error.IsEmpty());
	TestEqual(TEXT("left is red"), Out[256 * 512 + 100], FColor(255, 0, 0, 255));
	TestEqual(TEXT("right is blue"), Out[256 * 512 + 400], FColor(0, 0, 255, 255));

	// Enlarging works, with a warning.
	TestTrue(TEXT("a 300 picture is enlarged"), FMantleThumbnailResolver::FitToTarget(Solid(300, 300, FColor(10, 200, 30, 255)), 300, 300, Square, Out, Warning, Error));
	TestEqual(TEXT("still 512 x 512"), Out.Num(), 512 * 512);
	TestFalse(TEXT("the warning says so"), Warning.IsEmpty());
	TestEqual(TEXT("flat colour stays flat"), Out[1000], FColor(10, 200, 30, 255));

	// Too small is refused, with a reason and no pixels.
	TestFalse(TEXT("100 is refused"), FMantleThumbnailResolver::FitToTarget(Solid(100, 100, FColor::White), 100, 100, Square, Out, Warning, Error));
	TestFalse(TEXT("a reason is given"), Error.IsEmpty());
	TestEqual(TEXT("no pixels"), Out.Num(), 0);

	// Transparent pixels are laid over a dark card: the picture is always opaque.
	TestTrue(TEXT("a see-through picture fits"), FMantleThumbnailResolver::FitToTarget(Solid(512, 512, FColor(255, 255, 255, 0)), 512, 512, Square, Out, Warning, Error));
	TestEqual(TEXT("opaque"), static_cast<int32>(Out[0].A), 255);
	TestTrue(TEXT("and dark"), Out[0].R < 20 && Out[0].G < 20 && Out[0].B < 20);

	// A wrong pixel count is an error, not a crash.
	TestFalse(TEXT("a mismatched buffer is refused"), FMantleThumbnailResolver::FitToTarget(Solid(10, 10, FColor::White), 20, 20, Square, Out, Warning, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailResolveTest, "Mantle.Editor.Thumbnail.Resolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailResolveTest::RunTest(const FString&)
{
	using namespace MantleThumbnailChoiceTestPrivate;
	const FMantleThumbnailTarget Square = FMantleThumbnailTarget::Square();
	int32 Renders = 0;
	const FMantleThumbnailResolver::FRender Render = [&Renders](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString&)
	{
		++Renders;
		OutWidth = OutHeight = 512;
		OutPixels = Solid(512, 512, Choice.Mode == EMantleThumbnailMode::Angle ? FColor(0, 255, 0, 255) : FColor(255, 0, 0, 255));
		return true;
	};

	// Automatic and Choose the angle go to the kind's render; the choice reaches it.
	FMantleThumbnailChoice Choice;
	FMantleThumbnailResult Automatic = FMantleThumbnailResolver::Resolve(Choice, Square, Render);
	TestTrue(TEXT("automatic resolves"), Automatic.Succeeded());
	TestEqual(TEXT("automatic is the render"), Automatic.Pixels[0], FColor(255, 0, 0, 255));
	Choice.Mode = EMantleThumbnailMode::Angle;
	TestEqual(TEXT("an angle reaches the render"), FMantleThumbnailResolver::Resolve(Choice, Square, Render).Pixels[0], FColor(0, 255, 0, 255));
	TestEqual(TEXT("the render ran twice"), Renders, 2);

	// A render that cannot draw gives a reason and no pixels.
	const FMantleThumbnailResolver::FRender Failing = [](const FMantleThumbnailChoice&, TArray<FColor>&, int32&, int32&, FString& OutError)
	{
		OutError = TEXT("no renderer");
		return false;
	};
	const FMantleThumbnailResult Failed = FMantleThumbnailResolver::Resolve(FMantleThumbnailChoice(), Square, Failing);
	TestFalse(TEXT("a failed render fails"), Failed.Succeeded());
	TestEqual(TEXT("with its reason"), Failed.Error, FString(TEXT("no renderer")));

	// Custom never calls the render: a PNG file, cut to a square and resized.
	Renders = 0;
	FMantleThumbnailChoice Custom;
	Custom.Mode = EMantleThumbnailMode::Custom;
	Custom.ImageFile.FilePath = TempPng(TEXT("MantleThumbnailChoiceWide.png"), 600, 300, HalvesRedBlue(600, 300));
	const FMantleThumbnailResult FromFile = FMantleThumbnailResolver::Resolve(Custom, Square, Render);
	TestTrue(TEXT("a PNG file resolves"), FromFile.Succeeded());
	TestEqual(TEXT("without rendering"), Renders, 0);
	TestEqual(TEXT("512 x 512"), FromFile.Pixels.Num(), 512 * 512);
	TestFalse(TEXT("300 px of picture is enlarged, with a warning"), FromFile.Warning.IsEmpty());
	TestEqual(TEXT("left of the cut square is red"), FromFile.Pixels[256 * 512 + 50], FColor(255, 0, 0, 255));
	TestEqual(TEXT("right of it is blue"), FromFile.Pixels[256 * 512 + 460], FColor(0, 0, 255, 255));

	// The upload is a 512 x 512 PNG.
	const TArray<uint8> Png = FMantleThumbnailResolver::ToPng(FromFile, Square);
	int32 PngWidth = 0, PngHeight = 0;
	bool bAlpha = false;
	TestTrue(TEXT("the upload is a PNG"), FMantlePng::ReadHeader(Png, PngWidth, PngHeight, bAlpha));
	TestEqual(TEXT("512 wide"), PngWidth, 512);
	TestEqual(TEXT("512 tall"), PngHeight, 512);

	// A picture that is too small, missing, or not a PNG is refused with a reason.
	Custom.ImageFile.FilePath = TempPng(TEXT("MantleThumbnailChoiceSmall.png"), 64, 64, Solid(64, 64, FColor::White));
	TestFalse(TEXT("a 64 px picture is refused"), FMantleThumbnailResolver::Resolve(Custom, Square, Render).Succeeded());
	Custom.ImageFile.FilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleThumbnailChoiceMissing.png"));
	const FMantleThumbnailResult Missing = FMantleThumbnailResolver::Resolve(Custom, Square, Render);
	TestFalse(TEXT("a missing file is refused"), Missing.Succeeded());
	TestFalse(TEXT("with a reason"), Missing.Error.IsEmpty());
	Custom.ImageFile.FilePath = TEXT("C:/pictures/photo.jpg");
	TestFalse(TEXT("a non-PNG is refused"), FMantleThumbnailResolver::Resolve(Custom, Square, Render).Succeeded());
	Custom.ImageFile.FilePath.Reset();
	TestFalse(TEXT("no picture chosen is refused"), FMantleThumbnailResolver::Resolve(Custom, Square, Render).Succeeded());

	// A place takes the same path at 16:9.
	Custom.ImageFile.FilePath = TempPng(TEXT("MantleThumbnailChoiceHD.png"), 1920, 1080, HalvesRedBlue(1920, 1080));
	const FMantleThumbnailResult Place = FMantleThumbnailResolver::Resolve(Custom, FMantlePlaceThumbnail::Target(), Render);
	TestTrue(TEXT("a 1920x1080 picture fits a place"), Place.Succeeded());
	TestEqual(TEXT("1280 x 720"), Place.Pixels.Num(), 1280 * 720);

	// A choice is equal to itself and different when anything changes.
	FMantleThumbnailChoice A;
	FMantleThumbnailChoice B;
	TestTrue(TEXT("equal choices"), A == B);
	B.Mode = EMantleThumbnailMode::Custom;
	TestTrue(TEXT("a different mode differs"), A != B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailPlaceChoiceTest, "Mantle.Editor.Thumbnail.PlaceChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailPlaceChoiceTest::RunTest(const FString&)
{
	// A level with no Mantle Place Settings is Automatic, and a place's picture is 1280 x 720.
	TestTrue(TEXT("no level is automatic"), FMantlePlaceThumbnail::ChoiceFor(nullptr).Mode == EMantleThumbnailMode::Automatic);
	TestEqual(TEXT("a place's picture is 1280 wide"), FMantlePlaceThumbnail::Target().Width, 1280);
	TestEqual(TEXT("and 720 tall"), FMantlePlaceThumbnail::Target().Height, 720);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailStoreTest, "Mantle.Editor.Thumbnail.Store",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailStoreTest::RunTest(const FString&)
{
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleThumbnailStoreTest.ini"));
	IFileManager::Get().Delete(*File);
	FMantleThumbnailChoice Choice;
	TestFalse(TEXT("nothing is saved at first"), FMantleThumbnailStore::Load(TEXT("/Game/A.A"), TEXT("1"), Choice, File));
	TestTrue(TEXT("and the choice is Automatic"), Choice == FMantleThumbnailChoice());

	FMantleThumbnailChoice Saved;
	Saved.Mode = EMantleThumbnailMode::Angle;
	Saved.Angle = FRotator(-15.0, 140.0, 0.0);
	Saved.Zoom = 1.5f;
	Saved.ImageFile.FilePath = TEXT("C:/pictures/a.png");
	FMantleThumbnailStore::Save(TEXT("/Game/A.A"), TEXT("1"), Saved, File);
	TestTrue(TEXT("a saved choice loads"), FMantleThumbnailStore::Load(TEXT("/Game/A.A"), TEXT("1"), Choice, File));
	TestTrue(TEXT("exactly as saved"), Choice == Saved);
	TestFalse(TEXT("another source has none"), FMantleThumbnailStore::Load(TEXT("/Game/B.B"), TEXT("1"), Choice, File));
	TestFalse(TEXT("another class of the same source has none"), FMantleThumbnailStore::Load(TEXT("/Game/A.A"), TEXT("2"), Choice, File));

	// Going back to Automatic removes the entry.
	FMantleThumbnailStore::Save(TEXT("/Game/A.A"), TEXT("1"), FMantleThumbnailChoice(), File);
	TestFalse(TEXT("Automatic is not kept"), FMantleThumbnailStore::Load(TEXT("/Game/A.A"), TEXT("1"), Choice, File));
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailEmoteMomentTest, "Mantle.Editor.Thumbnail.EmoteMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailEmoteMomentTest::RunTest(const FString&)
{
	// Two joints; the third moment has them thrown farthest from where they are on average.
	const TArray<TArray<FVector>> Moments = {
		{ FVector(0, 0, 100), FVector(0, 0, 150) },
		{ FVector(0, 0, 101), FVector(0, 0, 149) },
		{ FVector(40, 0, 180), FVector(-40, 0, 190) },
		{ FVector(0, 0, 100), FVector(0, 0, 150) },
	};
	TestEqual(TEXT("the moment with the joints farthest from average"), FMantleEmoteCheck::MostExpressiveMoment(Moments), 2);
	// A clip that does not move: the first moment.
	const TArray<TArray<FVector>> Still = { { FVector(0, 0, 100) }, { FVector(0, 0, 100) }, { FVector(0, 0, 100) } };
	TestEqual(TEXT("a still clip picks the first moment"), FMantleEmoteCheck::MostExpressiveMoment(Still), 0);
	TestEqual(TEXT("nothing to compare is the first moment"), FMantleEmoteCheck::MostExpressiveMoment(TArray<TArray<FVector>>()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailStoreMigrationTest, "Mantle.Editor.Thumbnail.StoreMigration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailStoreMigrationTest::RunTest(const FString&)
{
	// A choice the first version left in the per-machine file moves into the project's file the first time it is read, and is gone from the old one.
	const FString Project = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleThumbnailProjectStore.ini"));
	const FString Legacy = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleThumbnailLegacyStore.ini"));
	IFileManager::Get().Delete(*Project);
	IFileManager::Get().Delete(*Legacy);
	FMantleThumbnailChoice Old;
	Old.Mode = EMantleThumbnailMode::Angle;
	Old.Angle = FRotator(-5.0, 30.0, 0.0);
	FMantleThumbnailStore::Save(TEXT("/Game/M.M"), TEXT("1"), Old, Legacy);

	FMantleThumbnailChoice Read;
	TestTrue(TEXT("the choice is found through the old file"), FMantleThumbnailStore::Load(TEXT("/Game/M.M"), TEXT("1"), Read, Project, Legacy));
	TestTrue(TEXT("exactly as it was"), Read == Old);
	FMantleThumbnailChoice Again;
	TestTrue(TEXT("now it is in the project's file"), FMantleThumbnailStore::Load(TEXT("/Game/M.M"), TEXT("1"), Again, Project));
	TestTrue(TEXT("still the same"), Again == Old);
	FMantleThumbnailChoice Gone;
	TestFalse(TEXT("and no longer in the old file"), FMantleThumbnailStore::Load(TEXT("/Game/M.M"), TEXT("1"), Gone, Legacy));
	IFileManager::Get().Delete(*Project);
	IFileManager::Get().Delete(*Legacy);

	// The default home is the project's Config folder, which goes with the project.
	TestTrue(TEXT("the default store is under the project's Config"), FMantleThumbnailResolver::IsInsideProject(FMantleThumbnailStore::DefaultConfigFile())
		&& FMantleThumbnailStore::DefaultConfigFile().Contains(TEXT("/Config/")));
	TestTrue(TEXT("the old one is under Saved"), FMantleThumbnailStore::LegacyConfigFile().Contains(TEXT("/Saved/")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailOutsideProjectTest, "Mantle.Editor.Thumbnail.OutsideProject",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailOutsideProjectTest::RunTest(const FString&)
{
	using namespace MantleThumbnailChoiceTestPrivate;
	TestTrue(TEXT("a file in the project is inside it"), FMantleThumbnailResolver::IsInsideProject(FPaths::Combine(FPaths::ProjectDir(), TEXT("Content/a.png"))));
	TestFalse(TEXT("a file elsewhere is not"), FMantleThumbnailResolver::IsInsideProject(TEXT("C:/Users/someone/Pictures/a.png")));
	TestFalse(TEXT("nothing is not"), FMantleThumbnailResolver::IsInsideProject(FString()));

	// A custom PNG outside the project works, with a warning that it will not travel; one inside has none.
	TArray<FColor> Pixels;
	Pixels.Init(FColor(10, 20, 30, 255), 512 * 512);
	const FString Inside = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleThumbInside.png"));
	FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(512, 512, Pixels), *Inside);
	FMantleThumbnailChoice Choice;
	Choice.Mode = EMantleThumbnailMode::Custom;
	Choice.ImageFile.FilePath = Inside;
	TestTrue(TEXT("a picture inside the project has no travel warning"), FMantleThumbnailResolver::Resolve(Choice, FMantleThumbnailTarget::Square(), FMantleThumbnailResolver::FRender()).Warning.IsEmpty());
	const FString Outside = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("MantleThumbOutside.png"));
	FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(512, 512, Pixels), *Outside);
	Choice.ImageFile.FilePath = Outside;
	const FMantleThumbnailResult OutsideResult = FMantleThumbnailResolver::Resolve(Choice, FMantleThumbnailTarget::Square(), FMantleThumbnailResolver::FRender());
	TestTrue(TEXT("a picture outside the project still resolves"), OutsideResult.Succeeded());
	TestTrue(TEXT("with a warning that it will not travel"), OutsideResult.Warning.Contains(TEXT("outside your project")));
	IFileManager::Get().Delete(*Outside);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleThumbnailModelAngleTest, "Mantle.Editor.Thumbnail.ModelAngle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleThumbnailModelAngleTest::RunTest(const FString&)
{
	// Automatic leaves the subject as it is; Choose the angle turns the camera and closes in on the framed box.
	FMantleThumbnailSubject Subject;
	Subject.Frame = FBox(FVector(-100, -50, 0), FVector(100, 50, 100));
	Subject.View = FMantleModelThumbnail::DefaultView();
	const FRotator Before = Subject.View;
	FMantleThumbnailChoice Choice;
	FMantleModelThumbnail::ApplyChoice(Subject, Choice);
	TestTrue(TEXT("Automatic keeps the view"), Subject.View.Equals(Before));
	TestTrue(TEXT("and the frame"), Subject.Frame.GetExtent().Equals(FVector(100, 50, 50)));

	Choice.Mode = EMantleThumbnailMode::Angle;
	Choice.Angle = FRotator(-20.0, 90.0, 0.0);
	Choice.Zoom = 2.0f;
	FMantleModelThumbnail::ApplyChoice(Subject, Choice);
	TestTrue(TEXT("Angle takes the chosen pitch and yaw"), Subject.View.Equals(FRotator(-20.0, 90.0, 0.0)));
	TestTrue(TEXT("zoom 2 halves the framed box"), Subject.Frame.GetExtent().Equals(FVector(50, 25, 25)));
	TestTrue(TEXT("about the same centre"), Subject.Frame.GetCenter().Equals(FVector(0, 0, 50)));

	// A model of nothing has nothing to draw; the resolver says so instead of failing.
	const FMantleThumbnailResult Nothing = FMantleThumbnailResolver::Resolve(Choice, FMantleThumbnailTarget::Square(),
		FMantleModelThumbnail::RenderFor([]() -> UObject* { return nullptr; }));
	TestFalse(TEXT("no model, no picture"), Nothing.Succeeded());
	TestFalse(TEXT("with a reason"), Nothing.Error.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
