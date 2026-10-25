// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayGuide.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The overlay painting guide. The synthetic cases need no assets; the reference body and face cases run when
// the project has built them and are skipped (with an info line) when it has not.

namespace MantleOverlayGuideTestPrivate
{
	/** Pixels that are not the plain background colour. */
	int32 NonBackground(const TArray<FColor>& Pixels)
	{
		int32 Count = 0;
		for (const FColor& Pixel : Pixels)
		{
			if (!(Pixel.R == 38 && Pixel.G == 38 && Pixel.B == 40))
			{
				++Count;
			}
		}
		return Count;
	}

	bool HasRegion(const TArray<FString>& Regions, const TCHAR* Name)
	{
		for (const FString& Region : Regions)
		{
			if (Region.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOverlayGuideTest,
	"Mantle.Editor.Overlay.PaintingGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOverlayGuideTest::RunTest(const FString& Parameters)
{
	using namespace MantleOverlayGuideTestPrivate;

	// Bone rules.
	TestTrue(TEXT("upperarm_l is the left arm"), FMantleOverlayGuide::ClassifyBone(TEXT("upperarm_l"), false) == FMantleOverlayGuide::ERegion::ArmL);
	TestTrue(TEXT("thumb_01_r is the right hand"), FMantleOverlayGuide::ClassifyBone(TEXT("thumb_01_r"), false) == FMantleOverlayGuide::ERegion::HandR);
	TestTrue(TEXT("calf_twist_01_l is the left leg"), FMantleOverlayGuide::ClassifyBone(TEXT("calf_twist_01_l"), false) == FMantleOverlayGuide::ERegion::LegL);
	TestTrue(TEXT("ball_r is the right foot"), FMantleOverlayGuide::ClassifyBone(TEXT("ball_r"), false) == FMantleOverlayGuide::ERegion::FootR);
	TestTrue(TEXT("spine_03 is the torso"), FMantleOverlayGuide::ClassifyBone(TEXT("spine_03"), false) == FMantleOverlayGuide::ERegion::Chest);
	TestTrue(TEXT("head on a face mesh is the scalp"), FMantleOverlayGuide::ClassifyBone(TEXT("head"), true) == FMantleOverlayGuide::ERegion::Scalp);
	TestTrue(TEXT("a facial bone on a face mesh is the face"), FMantleOverlayGuide::ClassifyBone(TEXT("FACIAL_C_Jaw"), true) == FMantleOverlayGuide::ERegion::Face);

	// A synthetic layout: one big square (two triangles) and a small one, in two regions.
	TArray<FMantleOverlayGuide::FTriangle> Triangles;
	const auto AddQuad = [&Triangles](const FVector2f& Min, const FVector2f& Max, const FMantleOverlayGuide::ERegion Region)
	{
		FMantleOverlayGuide::FTriangle A;
		A.UV[0] = Min; A.UV[1] = FVector2f(Max.X, Min.Y); A.UV[2] = Max; A.Region = Region;
		FMantleOverlayGuide::FTriangle B;
		B.UV[0] = Min; B.UV[1] = Max; B.UV[2] = FVector2f(Min.X, Max.Y); B.Region = Region;
		Triangles.Add(A);
		Triangles.Add(B);
	};
	AddQuad(FVector2f(0.1f, 0.1f), FVector2f(0.6f, 0.6f), FMantleOverlayGuide::ERegion::Chest);
	AddQuad(FVector2f(0.7f, 0.1f), FVector2f(0.9f, 0.3f), FMantleOverlayGuide::ERegion::ArmL);
	TArray<FColor> Pixels;
	TArray<FString> Regions;
	FMantleOverlayGuide::Render(Triangles, Pixels, &Regions);
	TestEqual(TEXT("the synthetic guide is 2048 x 2048"), Pixels.Num(), FMantleOverlayGuide::Size * FMantleOverlayGuide::Size);
	TestTrue(TEXT("the synthetic guide names both regions"), HasRegion(Regions, TEXT("Chest & belly")) && HasRegion(Regions, TEXT("Left arm")));
	TestTrue(TEXT("the corner outside every island is background"), Pixels[0] == FColor(38, 38, 40, 255));
	const int32 Painted = NonBackground(Pixels);
	const int32 Expected = FMath::RoundToInt((0.5f * 0.5f + 0.2f * 0.2f) * FMantleOverlayGuide::Size * FMantleOverlayGuide::Size);
	TestTrue(TEXT("about the two squares' area is island fill"), Painted > Expected * 9 / 10 && Painted < Expected * 11 / 10);
	TArray<FColor> Again;
	FMantleOverlayGuide::Render(Triangles, Again, nullptr);
	TestTrue(TEXT("the same triangles draw the same bytes"), Pixels == Again);

	// The reference body and face, when this project has built them.
	for (const bool bFace : { false, true })
	{
		TArray<FColor> Guide;
		TArray<FString> Drawn;
		FString Error;
		if (!FMantleOverlayGuide::Build(bFace, Guide, Error, &Drawn))
		{
			AddInfo(FString::Printf(TEXT("Skipped the reference %s guide: %s"), bFace ? TEXT("face") : TEXT("body"), *Error));
			continue;
		}
		TestEqual(TEXT("the reference guide is 2048 x 2048"), Guide.Num(), FMantleOverlayGuide::Size * FMantleOverlayGuide::Size);
		TestTrue(TEXT("a reasonable share of the guide is island fill"), NonBackground(Guide) > Guide.Num() / 20);
		if (bFace)
		{
			TestTrue(TEXT("the face guide has the face"), HasRegion(Drawn, TEXT("Face")));
		}
		else
		{
			for (const TCHAR* Needed : { TEXT("Chest & belly"), TEXT("Left arm"), TEXT("Right arm"), TEXT("Left leg"), TEXT("Right leg") })
			{
				TestTrue(FString::Printf(TEXT("the body guide has %s"), Needed), HasRegion(Drawn, Needed));
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
