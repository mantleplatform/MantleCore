// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleRimFinder.h"

#include "MantleGlbWriter.h"
#include "MantleVehicleConverter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleRimsPrivate
{
	/** Whole words only: "trim" and "primary" are not "rim". */
	const TCHAR* const RimWords[] = { TEXT("rim"), TEXT("rims"), TEXT("wheel"), TEXT("wheels"), TEXT("alloy"), TEXT("alloys"), TEXT("hub"), TEXT("hubs"),
		TEXT("hubcap"), TEXT("hubcaps"), TEXT("spoke"), TEXT("spokes") };
	const TCHAR* const NotRimWords[] = { TEXT("tire"), TEXT("tires"), TEXT("tyre"), TEXT("tyres"), TEXT("rubber"), TEXT("brake"), TEXT("brakes"), TEXT("disc"),
		TEXT("discs"), TEXT("rotor"), TEXT("rotors"), TEXT("caliper"), TEXT("calipers"), TEXT("steering") };

	/** A name cut into lower-case words at anything that is not a letter or digit, at a lower-to-upper step (HubCap) and between letters and digits. */
	TArray<FString> NameWords(const FString& Name)
	{
		TArray<FString> Words;
		FString Word;
		TCHAR Last = 0;
		for (const TCHAR Character : Name)
		{
			if (!FChar::IsAlnum(Character))
			{
				if (!Word.IsEmpty())
				{
					Words.Add(Word.ToLower());
					Word.Reset();
				}
				Last = 0;
				continue;
			}
			const bool bStep = Last != 0 && ((FChar::IsLower(Last) && FChar::IsUpper(Character)) || (FChar::IsAlpha(Last) != FChar::IsAlpha(Character)));
			if (bStep && !Word.IsEmpty())
			{
				Words.Add(Word.ToLower());
				Word.Reset();
			}
			Word.AppendChar(Character);
			Last = Character;
		}
		if (!Word.IsEmpty())
		{
			Words.Add(Word.ToLower());
		}
		return Words;
	}

	bool HasAny(const TArray<FString>& Words, TConstArrayView<const TCHAR*> Wanted)
	{
		for (const TCHAR* Word : Wanted)
		{
			if (Words.Contains(FString(Word)))
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
	const TArray<FString> Words = NameWords(Section);
	return HasAny(Words, MakeArrayView(RimWords)) && !HasAny(Words, MakeArrayView(NotRimWords));
}

TArray<TArray<FString>> FMantleVehicleRims::Find(const FMantleGlbMesh& Body, const TConstArrayView<FName> Bones, const TConstArrayView<double> Radii,
	const TArray<FString>& SectionNames, TArray<FString>& OutNotes, TArray<double>* OutRimRadii)
{
	TArray<TArray<FString>> Out;
	Out.SetNum(Bones.Num());
	if (OutRimRadii)
	{
		OutRimRadii->Init(0.0, Bones.Num());
	}
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

	// A section is one material, however many chunks (primitives) of the body draw it: count over all of them before judging.
	struct FSectionCount
	{
		int32 Vertices = 0;
		int32 Owned = 0;
		TArray<int32> OwnedBy;
		TArray<double> Reach;
	};
	TMap<int32, FSectionCount> Counts;
	for (const FMantleGlbPrimitive& Primitive : Body.Primitives)
	{
		if (!SectionNames.IsValidIndex(Primitive.MaterialIndex) || Primitive.NumVertices <= 0 || !FMantleVehicleRims::NameIsRim(SectionNames[Primitive.MaterialIndex]))
		{
			continue;
		}
		FSectionCount& Count = Counts.FindOrAdd(Primitive.MaterialIndex);
		if (Count.OwnedBy.IsEmpty())
		{
			Count.OwnedBy.Init(0, Bones.Num());
			Count.Reach.Init(0.0, Bones.Num());
		}
		Count.Vertices += Primitive.NumVertices;
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
			// Around the wheel's axle (a car's wheels turn about Y), not in all directions: a wide rim is not far from its bone, it is wide.
			const FVector Offset = FVector(Body.Positions[Vertex]) - World[Joint].GetLocation();
			const double Around = FMath::Sqrt(FMath::Square(Offset.X) + FMath::Square(Offset.Z));
			if (Radius > 0.0 && Around > Radius * RadiusMargin)
			{
				continue;
			}
			Count.Reach[Wheel] = FMath::Max(Count.Reach[Wheel], Around);
			++Count.OwnedBy[Wheel];
			++Count.Owned;
		}
	}

	for (const TPair<int32, FSectionCount>& Pair : Counts)
	{
		const FString& Section = SectionNames[Pair.Key];
		const FSectionCount& Count = Pair.Value;
		if (Count.Owned < Count.Vertices * RimShare)
		{
			OutNotes.Add(FString::Printf(TEXT("Section \"%s\" is named like a rim, but only %d%% of it belongs to the wheels, so it stays on the car: it is mixed with the body, and a wheel swap cannot hide it."),
				*Section, FMath::RoundToInt(100.f * Count.Owned / Count.Vertices)));
			continue;
		}
		// A wheel has the section when a real share of the wheels' part of it is its own, not one stray vertex.
		const int32 Least = FMath::Max(MinWheelVertices, FMath::CeilToInt(Count.Owned * MinWheelShare));
		for (int32 Wheel = 0; Wheel < Bones.Num(); ++Wheel)
		{
			if (Count.OwnedBy[Wheel] >= Least)
			{
				Out[Wheel].AddUnique(Section);
				if (OutRimRadii)
				{
					(*OutRimRadii)[Wheel] = FMath::Max((*OutRimRadii)[Wheel], Count.Reach[Wheel]);
				}
			}
		}
	}
	return Out;
}
