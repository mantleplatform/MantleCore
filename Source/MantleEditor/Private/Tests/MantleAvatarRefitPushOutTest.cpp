// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarRefitFixer.h"
#include "MantleSkinnedMesh.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #153: pushing a garment out of the authoring body's skin, on synthetic surfaces. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarRefitPushOutTestPrivate
{
	/** A flat square of Cells x Cells cells, Size cm across, centred on the origin at height Z, every normal (0, 0, Up). */
	FMantleSkinnedMeshLOD Sheet(const int32 Cells, const float Size, const float Z, const float Up)
	{
		FMantleSkinnedMeshLOD Mesh;
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				Mesh.Positions.Add(FVector3f(Size * X / Cells - Size / 2, Size * Y / Cells - Size / 2, Z));
				Mesh.Normals.Add(FVector3f(0, 0, Up));
			}
		}
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const uint32 A = Y * (Cells + 1) + X;
				if (Up > 0)
				{
					Mesh.Indices.Append({ A, A + Cells + 1, A + 1, A + 1, A + Cells + 1, A + Cells + 2 });
				}
				else
				{
					Mesh.Indices.Append({ A, A + 1, A + Cells + 1, A + 1, A + Cells + 2, A + Cells + 1 });
				}
			}
		}
		return Mesh;
	}

	/** Two sheets as one mesh: B's vertices and triangles after A's. */
	FMantleSkinnedMeshLOD Join(const FMantleSkinnedMeshLOD& A, const FMantleSkinnedMeshLOD& B)
	{
		FMantleSkinnedMeshLOD Mesh = A;
		const uint32 Offset = Mesh.Positions.Num();
		Mesh.Positions.Append(B.Positions);
		Mesh.Normals.Append(B.Normals);
		for (const uint32 Index : B.Indices)
		{
			Mesh.Indices.Add(Index + Offset);
		}
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarRefitPushOutTest,
	"Mantle.Editor.AvatarRefit.PushOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAvatarRefitPushOutTest::RunTest(const FString& Parameters)
{
	using namespace MantleAvatarRefitPushOutTestPrivate;
	// The skin: a 40 cm sheet at Z 0 facing up (outside is +Z). Garments are 6 cm sheets over its middle, far from its edges.
	const FMantleSkinnedMeshLOD Skin = Sheet(20, 40.0f, 0.0f, 1.0f);

	// A single layer 1 cm under the skin is pushed out to the clearance.
	{
		FMantleSkinnedMeshLOD Garment = Sheet(6, 6.0f, -1.0f, 1.0f);
		FMantleRefitPushOut Out;
		FString Error;
		if (!TestTrue(TEXT("single layer: the push out runs"), FMantleAvatarRefit::PushOutOfBody(Garment, Skin, Out, Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(TEXT("single layer: every vertex was under the skin"), Out.InsideBefore, Out.Tracked);
		TestTrue(TEXT("single layer: it tracked the skin"), Out.Tracked > 0);
		TestEqual(TEXT("single layer: none is under its own shell"), Out.UnderShell, 0);
		TestEqual(TEXT("single layer: none is left under"), Out.InsideAfter, 0);
		TestTrue(TEXT("single layer: it moved"), Out.Moved > 0);
		for (const FVector3f& P : Garment.Positions)
		{
			if (P.Z < FMantleAvatarRefit::PushOutCm - 0.05f)
			{
				AddError(FString::Printf(TEXT("single layer: a vertex stayed at Z %.2f, under the clearance"), P.Z));
				break;
			}
		}
	}

	// A padded piece: a lining 0.4 cm under the skin facing it, under a shell 0.4 cm outside facing away (layer twins, 0.8 cm
	// apart). The lining is under its own shell: left where it is, and nothing moves. Before #153 its push was coupled to the
	// shell's and the whole piece went out.
	{
		FMantleSkinnedMeshLOD Garment = Join(Sheet(6, 6.0f, -0.4f, -1.0f), Sheet(6, 6.0f, 0.4f, 1.0f));
		const int32 Lining = (6 + 1) * (6 + 1);
		const TArray<FVector3f> Before = Garment.Positions;
		FMantleRefitPushOut Out;
		FString Error;
		if (!TestTrue(TEXT("padded: the push out runs"), FMantleAvatarRefit::PushOutOfBody(Garment, Skin, Out, Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(TEXT("padded: the lining was under the skin"), Out.InsideBefore, Lining);
		TestEqual(TEXT("padded: the lining is under its own shell"), Out.UnderShell, Lining);
		TestEqual(TEXT("padded: nothing moved"), Out.Moved, 0);
		TestEqual(TEXT("padded: the lining is left under"), Out.InsideAfter, Lining);
		for (int32 Vertex = 0; Vertex < Garment.Positions.Num(); ++Vertex)
		{
			if (!Garment.Positions[Vertex].Equals(Before[Vertex], 1.e-4f))
			{
				AddError(FString::Printf(TEXT("padded: vertex %d moved"), Vertex));
				break;
			}
		}
	}

	// Distance to an opening, along the surface: a 6 cm sheet's middle is 3 cm from its edge, its corners 0.
	{
		const FMantleSkinnedMeshLOD Garment = Sheet(6, 6.0f, -1.0f, 1.0f);
		const TArray<float> Opening = FMantleAvatarRefit::DistanceToOpenings(Garment);
		TestEqual(TEXT("openings: one distance per vertex"), Opening.Num(), Garment.NumVertices());
		TestEqual(TEXT("openings: a corner is on an opening"), Opening[0], 0.0f, 1.e-4f);
		TestEqual(TEXT("openings: the middle is 3 cm from the edge"), Opening[3 * 7 + 3], 3.0f, 1.e-3f);
	}

	// A wide piece 1 cm under the skin: its rim (within OpeningReachCm of the edge) is pushed out, its middle is under
	// cover and stays, so the piece is not stretched from the middle out.
	{
		FMantleSkinnedMeshLOD Garment = Sheet(20, 20.0f, -1.0f, 1.0f);
		const TArray<float> Opening = FMantleAvatarRefit::DistanceToOpenings(Garment);
		int32 Covered = 0;
		for (const float D : Opening) { Covered += D > FMantleAvatarRefit::OpeningReachCm ? 1 : 0; }
		FMantleRefitPushOut Out;
		FString Error;
		if (!TestTrue(TEXT("wide: the push out runs"), FMantleAvatarRefit::PushOutOfBody(Garment, Skin, Out, Error)))
		{
			AddError(Error);
			return false;
		}
		TestTrue(TEXT("wide: some of it is under cover"), Covered > 0);
		TestEqual(TEXT("wide: the covered middle is left"), Out.UnderCover, Covered);
		TestTrue(TEXT("wide: the rim moved"), Out.Moved > 0);
		TestTrue(TEXT("wide: the middle stayed under"), Out.InsideAfter >= Covered / 2);
		// The rim's push is smoothed inward a little; the middle, 10 cm from any edge, stays put to within a millimetre.
		TestTrue(TEXT("wide: the middle vertex stayed"), Garment.Positions[10 * 21 + 10].Equals(FVector3f(0, 0, -1.0f), 0.1f));
		TestFalse(TEXT("wide: the detail names the depths"), Out.Detail.IsEmpty());
	}

	// The same lining with no shell over it (its twin is under the skin too) is pushed as before.
	{
		FMantleSkinnedMeshLOD Garment = Join(Sheet(6, 6.0f, -1.2f, -1.0f), Sheet(6, 6.0f, -0.4f, 1.0f));
		FMantleRefitPushOut Out;
		FString Error;
		if (!TestTrue(TEXT("both under: the push out runs"), FMantleAvatarRefit::PushOutOfBody(Garment, Skin, Out, Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(TEXT("both under: no layer is under a shell outside the skin"), Out.UnderShell, 0);
		TestTrue(TEXT("both under: it moved"), Out.Moved > 0);
		TestEqual(TEXT("both under: none is left under"), Out.InsideAfter, 0);
	}
	return true;
}

#endif
