// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleCollision.h"

#include "Engine/StaticMesh.h"
#include "MantleCosmeticExport.h"
#include "MantleGlbWriter.h"
#include "MantleVehicleSource.h"

namespace MantleVehicleCollisionPrivate
{
	/** A position rounded to a thousandth of a centimeter: the key two vertices share when they are the same point. */
	FIntVector WeldKey(const FVector3f& Position)
	{
		return FIntVector(FMath::RoundToInt(Position.X * 1000.0f), FMath::RoundToInt(Position.Y * 1000.0f), FMath::RoundToInt(Position.Z * 1000.0f));
	}
}

int32 FMantleVehicleCollision::AddTriangles(const TConstArrayView<FVector3f> MeshPositions, const TConstArrayView<uint32> MeshIndices, const FTransform& ToComponent)
{
	using namespace MantleVehicleCollisionPrivate;
	TArray<FVector3f> Placed;
	Placed.Reserve(MeshPositions.Num());
	for (const FVector3f& Position : MeshPositions)
	{
		Placed.Add(FVector3f(ToComponent.TransformPosition(FVector(Position))));
	}
	int32 Added = 0;
	for (int32 First = 0; First + 2 < MeshIndices.Num(); First += 3)
	{
		const uint32 Raw[3] = { MeshIndices[First], MeshIndices[First + 1], MeshIndices[First + 2] };
		if (Raw[0] >= static_cast<uint32>(Placed.Num()) || Raw[1] >= static_cast<uint32>(Placed.Num()) || Raw[2] >= static_cast<uint32>(Placed.Num()))
		{
			continue;
		}
		const FVector3f& A = Placed[static_cast<int32>(Raw[0])];
		const FVector3f& B = Placed[static_cast<int32>(Raw[1])];
		const FVector3f& C = Placed[static_cast<int32>(Raw[2])];
		// Not a number, or no area: nothing to collide with (the cook drops the same).
		if (A.ContainsNaN() || B.ContainsNaN() || C.ContainsNaN() || FVector3f::CrossProduct(B - A, C - A).SizeSquared() <= UE_SMALL_NUMBER)
		{
			continue;
		}
		int32 Corners[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector3f& Position = Placed[static_cast<int32>(Raw[Corner])];
			const FIntVector Key = WeldKey(Position);
			if (const int32* Found = Welded.Find(Key))
			{
				Corners[Corner] = *Found;
			}
			else
			{
				Corners[Corner] = Positions.Add(Position);
				Welded.Add(Key, Corners[Corner]);
			}
		}
		// Two corners welded together: a sliver the weld collapsed.
		if (Corners[0] == Corners[1] || Corners[1] == Corners[2] || Corners[0] == Corners[2])
		{
			continue;
		}
		Indices.Append({ static_cast<uint32>(Corners[0]), static_cast<uint32>(Corners[1]), static_cast<uint32>(Corners[2]) });
		++Added;
	}
	return Added;
}

bool FMantleVehicleCollision::BoneRestPose(const TConstArrayView<FMantleGlbJoint> Joints, const FName Bone, FTransform& OutComponent)
{
	TArray<FTransform> Component;
	Component.Reserve(Joints.Num());
	int32 Found = INDEX_NONE;
	for (int32 Index = 0; Index < Joints.Num(); ++Index)
	{
		const FMantleGlbJoint& Joint = Joints[Index];
		// Parent-first: a parent not yet seen means the skeleton is not in order, and nothing is placed on it.
		if (Joint.Parent >= Index)
		{
			return false;
		}
		Component.Add(Joint.Parent >= 0 ? Joint.Local * Component[Joint.Parent] : Joint.Local);
		if (Found == INDEX_NONE && Joint.Name.ToString().Equals(Bone.ToString(), ESearchCase::IgnoreCase))
		{
			Found = Index;
		}
	}
	if (Found == INDEX_NONE)
	{
		return false;
	}
	OutComponent = Component[Found];
	return true;
}

FMantleVehicleCollision FMantleVehicleCollision::ForParts(const UMantleVehicleSource& Source, const TConstArrayView<FMantleGlbJoint> Joints, TArray<FString>& OutNotes)
{
	FMantleVehicleCollision Out;
	for (int32 Index = 0; Index < Source.Parts.Num(); ++Index)
	{
		const FMantleVehicleSourcePart& Part = Source.Parts[Index];
		const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		if (!Mesh)
		{
			continue; // CheckSource refused it
		}
		const bool bOnWheel = Source.Wheels.ContainsByPredicate([&Part](const FMantleVehicleSourceWheel& Wheel) { return Wheel.Bone.ToString().Equals(Part.Bone.ToString(), ESearchCase::IgnoreCase); });
		if (bOnWheel)
		{
			continue;
		}
		FTransform BoneInComponent;
		if (!BoneRestPose(Joints, Part.Bone, BoneInComponent))
		{
			OutNotes.Add(FString::Printf(TEXT("part %d ('%s') rides bone '%s', which the body does not have, so its surface is not in the car's shape"), Index, *Mesh->GetName(), *Part.Bone.ToString()));
			continue;
		}
		FMantleGlbMesh Read;
		TArray<int32> Slots;
		int32 Triangles = 0;
		FString Error;
		if (!FMantleCosmeticExport::ReadStaticMesh(Mesh, 0, Read, Slots, Triangles, Error))
		{
			OutNotes.Add(FString::Printf(TEXT("part %d ('%s') could not be read for the car's shape (%s), so its surface is not in it"), Index, *Mesh->GetName(), *Error));
			continue;
		}
		const FTransform ToComponent = Part.Transform * BoneInComponent;
		// Every material section's triangles, indices made absolute (a primitive's are relative to its first vertex).
		TArray<uint32> Absolute;
		for (const FMantleGlbPrimitive& Primitive : Read.Primitives)
		{
			Absolute.Reserve(Absolute.Num() + Primitive.Indices.Num());
			for (const uint32 Local : Primitive.Indices)
			{
				Absolute.Add(Local + static_cast<uint32>(Primitive.FirstVertex));
			}
		}
		Out.AddTriangles(Read.Positions, Absolute, ToComponent);
	}
	return Out;
}
