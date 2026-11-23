// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBlockingVolumeExport.h"

#include "MantlePlaceExporter.h"

#include "Components/BrushComponent.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "GameFramework/Volume.h"
#include "Model.h"

bool FMantleBlockingVolumeExport::IsBlockingVolume(const AActor* Actor)
{
	return Actor && Actor->IsA(ABlockingVolume::StaticClass());
}

bool FMantleBlockingVolumeExport::ReadLocalBox(const AActor* Actor, FVector& OutCenter, FVector& OutSize)
{
	const ABrush* Brush = Cast<ABrush>(Actor);
	if (!Brush || !Brush->Brush || !Brush->Brush->Polys)
	{
		return false;
	}

	// The polygons' corners, in the brush component's space (what the component's transform carries to the world).
	FBox Bounds(ForceInit);
	int32 Corners = 0;
	for (const FPoly& Poly : Brush->Brush->Polys->Element)
	{
		for (const FVector3f& Vertex : Poly.Vertices)
		{
			Bounds += FVector(Vertex);
			++Corners;
		}
	}
	if (Corners < 8 || !Bounds.IsValid)
	{
		return false;
	}
	const FVector Size = Bounds.GetSize();
	if (Size.X < UE_KINDA_SMALL_NUMBER || Size.Y < UE_KINDA_SMALL_NUMBER || Size.Z < UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// A box when every corner of every polygon lies at an extreme of all three axes (a cylinder's ring, stairs' steps and a
	// cut-off corner all have a corner somewhere between), and all eight corners of the box are used.
	const double Tolerance = FMath::Max(0.01, Size.GetMax() * 1.0e-4);
	uint8 CornersSeen = 0;
	for (const FPoly& Poly : Brush->Brush->Polys->Element)
	{
		for (const FVector3f& Vertex : Poly.Vertices)
		{
			int32 Index = 0;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Value = Vertex[Axis];
				if (FMath::Abs(Value - Bounds.Max[Axis]) <= Tolerance)
				{
					Index |= 1 << Axis;
				}
				else if (FMath::Abs(Value - Bounds.Min[Axis]) > Tolerance)
				{
					return false;
				}
			}
			CornersSeen |= static_cast<uint8>(1 << Index);
		}
	}
	if (CornersSeen != 0xFF)
	{
		return false;
	}
	OutCenter = Bounds.GetCenter();
	OutSize = Size;
	return true;
}

bool FMantleBlockingVolumeExport::HasNoCollision(const AActor* Actor)
{
	const ABrush* Brush = IsBlockingVolume(Actor) ? Cast<ABrush>(Actor) : nullptr;
	const UBrushComponent* Component = Brush ? Brush->GetBrushComponent() : nullptr;
	return Component && Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision;
}

bool FMantleBlockingVolumeExport::ReadWall(const AActor* Actor, FMantleBlockingWall& OutWall)
{
	const ABrush* Brush = IsBlockingVolume(Actor) ? Cast<ABrush>(Actor) : nullptr;
	const UBrushComponent* Component = Brush ? Brush->GetBrushComponent() : nullptr;
	if (!Component || Actor->HasAnyFlags(RF_Transient))
	{
		return false;
	}

	// The same reading a mesh's collision gets (ExporterReadMeshComponent): query only is overlap, and none is no wall.
	EMantleCollision Collision = EMantleCollision::Blocking;
	switch (Component->GetCollisionEnabled())
	{
	case ECollisionEnabled::NoCollision: return false;
	case ECollisionEnabled::QueryOnly:   Collision = EMantleCollision::Overlap; break;
	default: break;
	}

	FVector Center = FVector::ZeroVector;
	FVector Size = FVector::OneVector;
	if (!ReadLocalBox(Actor, Center, Size))
	{
		return false;
	}

	// The cube is 100 cm, centered on its own origin, so it is placed at the box's center and stretched to its size. A negative
	// scale only mirrors a box onto itself, so the stretch is the size without a sign.
	const FTransform& World = Component->GetComponentTransform();
	const FVector Scale = World.GetScale3D().GetAbs();
	OutWall.Actor = Actor;
	OutWall.Transform = FTransform(World.GetRotation(), World.TransformPosition(Center), Scale * Size / 100.0);
	OutWall.Collision = Collision;
	OutWall.bBlocksVisibility = FMantlePlaceExporter::BlocksVisibility(Component);
	return true;
}
