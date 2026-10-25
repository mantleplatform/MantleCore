// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFusedClothing.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #50 / #55: the fused-clothing check on synthetic tubes. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleFusedClothingTestPrivate
{
	/** An open tube about the Z axis (outward windings) from ZMin to ZMax: Radius, 32 segments, 1 cm rings. */
	void FusedTestTube(const double Radius, const double ZMin, const double ZMax, TArray<FVector3f>& Vertices, TArray<int32>& Indices)
	{
		constexpr int32 Segments = 32;
		const int32 Rings = FMath::RoundToInt(ZMax - ZMin) + 1;
		const int32 Base = Vertices.Num();
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			for (int32 Step = 0; Step < Segments; ++Step)
			{
				const double Angle = Step * UE_TWO_PI / Segments;
				Vertices.Add(FVector3f(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), ZMin + Ring));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int32 Step = 0; Step < Segments; ++Step)
			{
				const int32 Next = (Step + 1) % Segments;
				const int32 A = Base + Ring * Segments + Step;
				const int32 B = Base + Ring * Segments + Next;
				const int32 C = Base + (Ring + 1) * Segments + Step;
				const int32 D = Base + (Ring + 1) * Segments + Next;
				Indices.Append({ A, B, C, B, D, C });
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFusedClothingCheckTest,
	"Mantle.Editor.FusedClothing.Check",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFusedClothingCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleFusedClothingTestPrivate;
	TArray<FVector3f> BareVertices;
	TArray<int32> BareIndices;
	FusedTestTube(15.0, 0.0, 170.0, BareVertices, BareIndices);
	const FMantleFusedResult Bare = FMantleFusedClothing::CheckSlot({ TEXT("Body_Skin"), BareVertices, BareIndices }, 170.0);
	TestEqual(TEXT("a plain body is bare"), Bare.Verdict, EMantleFusedVerdict::Bare);

	// A jacket modelled into the same slot: a second surface 1 cm outside the body over the torso.
	TArray<FVector3f> FusedVertices = BareVertices;
	TArray<int32> FusedIndices = BareIndices;
	FusedTestTube(16.0, 70.0, 130.0, FusedVertices, FusedIndices);
	const FMantleFusedResult Fused = FMantleFusedClothing::CheckSlot({ TEXT("Body_Skin"), FusedVertices, FusedIndices }, 170.0);
	TestEqual(TEXT("clothing over the body in one slot is fused"), Fused.Verdict, EMantleFusedVerdict::FusedClothing);
	TestTrue(TEXT("and says so in words"), Fused.Reason.Contains(TEXT("built into the body")));

	// Clothing a long way off the body (a cloak 10 cm out) is not a layer on it.
	TArray<FVector3f> CloakVertices = BareVertices;
	TArray<int32> CloakIndices = BareIndices;
	FusedTestTube(25.0, 70.0, 130.0, CloakVertices, CloakIndices);
	TestEqual(TEXT("a surface far from the body is not fused"), FMantleFusedClothing::CheckSlot({ TEXT("Body_Skin"), CloakVertices, CloakIndices }, 170.0).Verdict,
		EMantleFusedVerdict::Bare);

	TestEqual(TEXT("a skin slot named for boots is fused"), FMantleFusedClothing::CheckSlot({ TEXT("Skin_Boots"), BareVertices, BareIndices }, 170.0).Verdict,
		EMantleFusedVerdict::FusedClothing);

	TArray<FVector3f> FewVertices;
	TArray<int32> FewIndices;
	FusedTestTube(15.0, 0.0, 3.0, FewVertices, FewIndices);
	TestEqual(TEXT("a few triangles can't tell"), FMantleFusedClothing::CheckSlot({ TEXT("Body_Skin"), FewVertices, FewIndices }, 170.0).Verdict, EMantleFusedVerdict::Unknown);

	TestTrue(TEXT("armour is clothing"), FMantleFusedClothing::NamesClothing(TEXT("M_Armor_Skin")));
	TestFalse(TEXT("a body is not"), FMantleFusedClothing::NamesClothing(TEXT("M_Body_Skin")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
