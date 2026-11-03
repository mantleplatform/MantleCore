// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleRimFinder.h"

#include "MantleGlbWriter.h"
#include "MantleVehicleConverter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleRimsPrivate
{
	const TCHAR* const RimWords[] = { TEXT("rim"), TEXT("wheel"), TEXT("alloy"), TEXT("hub"), TEXT("spoke") };
	const TCHAR* const NotRimWords[] = { TEXT("tire"), TEXT("tyre"), TEXT("rubber"), TEXT("brake"), TEXT("disc"), TEXT("caliper"), TEXT("steering") };

	bool HasAny(const FString& Lower, TConstArrayView<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

bool FMantleVehicleRims::NameIsRim(const FString& Section)
{
	using namespace MantleVehicleRimsPrivate;
	const FString Lower = Section.ToLower();
	return HasAny(Lower, MakeArrayView(RimWords)) && !HasAny(Lower, MakeArrayView(NotRimWords));
}

TArray<TArray<FString>> FMantleVehicleRims::Find(const FMantleGlbMesh& Body, const TConstArrayView<FName> Bones, const TConstArrayView<double> Radii,
	const TArray<FString>& SectionNames, TArray<FString>& OutNotes)
{
	TArray<TArray<FString>> Out;
	Out.SetNum(Bones.Num());
	const int32 N = Body.NumInfluences;
	if (N <= 0 || Bones.IsEmpty())
	{
		return Out;
	}

	// Each wheel's joint and where it sits in the body's space (the joints are parent-first).
	TArray<FTransform> World;
	World.SetNum(Body.Joints.Num());
	for (int32 Joint = 0; Joint < Body.Joints.Num(); ++Joint)
	{
		const FMantleGlbJoint& Info = Body.Joints[Joint];
		World[Joint] = World.IsValidIndex(Info.Parent) && Info.Parent < Joint ? Info.Local * World[Info.Parent] : Info.Local;
	}
	TArray<int32> WheelOfJoint;
	WheelOfJoint.Init(INDEX_NONE, Body.Joints.Num());
	for (int32 Wheel = 0; Wheel < Bones.Num(); ++Wheel)
	{
		const int32 Joint = FMantleVehicleConverter::JointIndex(Body, Bones[Wheel]);
		if (Joint != INDEX_NONE && WheelOfJoint[Joint] == INDEX_NONE)
		{
			WheelOfJoint[Joint] = Wheel;
		}
	}

	for (const FMantleGlbPrimitive& Primitive : Body.Primitives)
	{
		if (!SectionNames.IsValidIndex(Primitive.MaterialIndex) || Primitive.NumVertices <= 0)
		{
			continue;
		}
		const FString& Section = SectionNames[Primitive.MaterialIndex];
		if (!FMantleVehicleRims::NameIsRim(Section))
		{
			continue;
		}
		// Whose each of its vertices is: the wheel its heaviest influence is, when the vertex sits inside that wheel's radius.
		TArray<int32> Owned;
		Owned.Init(0, Bones.Num());
		int32 NumOwned = 0;
		for (int32 Vertex = Primitive.FirstVertex; Vertex < Primitive.FirstVertex + Primitive.NumVertices; ++Vertex)
		{
			if (!Body.InfluenceJoints.IsValidIndex(Vertex * N) || !Body.Positions.IsValidIndex(Vertex))
			{
				continue;
			}
			const int32 Joint = Body.InfluenceJoints[Vertex * N];
			if (!WheelOfJoint.IsValidIndex(Joint) || WheelOfJoint[Joint] == INDEX_NONE || Body.InfluenceWeights[Vertex * N] < 0.5f)
			{
				continue;
			}
			const int32 Wheel = WheelOfJoint[Joint];
			const double Radius = Radii.IsValidIndex(Wheel) ? Radii[Wheel] : 0.0;
			if (Radius > 0.0
				&& FVector::Dist(FVector(Body.Positions[Vertex]), World[Joint].GetLocation()) > Radius * RadiusMargin)
			{
				continue;
			}
			++Owned[Wheel];
			++NumOwned;
		}
		if (NumOwned < Primitive.NumVertices * RimShare)
		{
			OutNotes.Add(FString::Printf(TEXT("Section \"%s\" is named like a rim, but only %d%% of it belongs to the wheels, so it stays on the car: it is mixed with the body, and a wheel swap cannot hide it."),
				*Section, FMath::RoundToInt(100.f * NumOwned / Primitive.NumVertices)));
			continue;
		}
		for (int32 Wheel = 0; Wheel < Bones.Num(); ++Wheel)
		{
			if (Owned[Wheel] > 0)
			{
				Out[Wheel].AddUnique(Section);
			}
		}
	}
	return Out;
}
