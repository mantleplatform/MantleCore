// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyCorrespondence.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Unreal splits a render vertex at a UV seam when the computed tangent differs on each side (as on Walter's
// LOD1/LOD2). The body converter merges those tangent-only twins and still refuses every other difference.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBodyCorrespondenceTestPrivate
{
	/** A 3x3 vertex grid (8 triangles) as the reference, with the centre vertex 4 optionally split into a twin, vertex 9. */
	struct FGrid
	{
		FMantleBodyCorrespondence Reference;
		TArray<FVector3f> Positions, Normals;
		TArray<FVector2f> UVs;
		TArray<uint32> Indices;
		TArray<uint16> Bones;
		TArray<float> Weights;

		FGrid(const bool bSplitCentre)
		{
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Column = 0; Column < 3; ++Column)
				{
					const FVector2f UV(0.1f + Column * 0.25f, 0.1f + Row * 0.25f);
					Reference.QuantizedUVs.Add(FIntPoint(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f)));
					UVs.Add(UV); Positions.Add(FVector3f(Column * 10.0f, Row * 10.0f, 0.0f)); Normals.Add(FVector3f(0, 0, 1));
					Bones.Add(static_cast<uint16>(Row * 3 + Column)); Weights.Add(65535.0f);
				}
			}
			for (int32 Row = 0; Row < 2; ++Row)
			{
				for (int32 Column = 0; Column < 2; ++Column)
				{
					const uint32 A = Row * 3 + Column, B = A + 1, C = A + 3, D = C + 1;
					for (const uint32 Index : {A, B, C, B, D, C}) Reference.Indices.Add(Index);
				}
			}
			Indices = Reference.Indices;
			if (bSplitCentre)
			{
				const FVector2f CentreUV = UVs[4]; const FVector3f CentrePosition = Positions[4], CentreNormal = Normals[4];
				const uint16 CentreBone = Bones[4]; const float CentreWeight = Weights[4];
				UVs.Add(CentreUV); Positions.Add(CentrePosition); Normals.Add(CentreNormal); Bones.Add(CentreBone); Weights.Add(CentreWeight);
				// The first cell's triangles use the twin; the rest keep vertex 4.
				for (int32 I = 0; I < 6; ++I) if (Indices[I] == 4) Indices[I] = 9;
			}
		}

		bool Run(FString& Error, TArray<int32>& ReferenceOfInput, TArray<int32>& InputOfReference) const
		{
			return Reference.MatchTangentSplits(UVs, TConstArrayView<FVector2f>(), Indices, Positions, Normals,
				TConstArrayView<FColor>(), Bones, Weights, 1, ReferenceOfInput, InputOfReference, Error);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyCorrespondenceTangentSplitTest,
	"Mantle.Editor.BodyCorrespondence.TangentOnlySplitMatches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyCorrespondenceTangentSplitTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyCorrespondenceTestPrivate;
	FString Error;
	TArray<int32> ReferenceOfInput, InputOfReference;

	const FGrid Plain(false);
	TestTrue(TEXT("the unsplit grid matches (control)"), Plain.Run(Error, ReferenceOfInput, InputOfReference));

	const FGrid Split(true);
	if (!TestTrue(TEXT("a vertex duplicated in everything but tangent matches the reference"), Split.Run(Error, ReferenceOfInput, InputOfReference)))
	{
		AddInfo(Error);
		return false;
	}
	TestEqual(TEXT("one reference entry a reference vertex"), InputOfReference.Num(), 9);
	TestEqual(TEXT("every render vertex is mapped, the duplicate too"), ReferenceOfInput.Num(), 10);
	TestEqual(TEXT("the duplicate maps to the same reference vertex as the original"), ReferenceOfInput[9], ReferenceOfInput[4]);
	TestEqual(TEXT("the lowest physical vertex survives"), InputOfReference[ReferenceOfInput[4]], 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodyCorrespondenceRealSplitRefusedTest,
	"Mantle.Editor.BodyCorrespondence.NonTangentSplitRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodyCorrespondenceRealSplitRefusedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodyCorrespondenceTestPrivate;
	FString Error;
	TArray<int32> ReferenceOfInput, InputOfReference;

	FGrid UVSeam(true);
	UVSeam.UVs[9].X += 0.01f;
	TestFalse(TEXT("a twin in another UV (a real seam) is refused"), UVSeam.Run(Error, ReferenceOfInput, InputOfReference));
	TestFalse(TEXT("the refusal says why"), Error.IsEmpty());
	TestTrue(TEXT("a refusal leaves no mapping"), ReferenceOfInput.IsEmpty() && InputOfReference.IsEmpty());

	FGrid HardEdge(true);
	HardEdge.Normals[9] = FVector3f(0, 1, 0);
	TestFalse(TEXT("a twin with another normal (a hard edge) is refused"), HardEdge.Run(Error, ReferenceOfInput, InputOfReference));

	FGrid Skin(true);
	Skin.Weights[9] = 32768.0f;
	TestFalse(TEXT("a twin with other skin weights is refused"), Skin.Run(Error, ReferenceOfInput, InputOfReference));

	FGrid Position(true);
	Position.Positions[9].Z += 0.5f;
	TestFalse(TEXT("a twin at another position is refused"), Position.Run(Error, ReferenceOfInput, InputOfReference));
	return true;
}

#endif
