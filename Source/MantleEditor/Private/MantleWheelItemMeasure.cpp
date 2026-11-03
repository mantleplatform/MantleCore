// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelItemMeasure.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

FMantleWheelItemMeasure FMantleWheelItem::Measure(const TConstArrayView<FVector3f> Positions)
{
	FMantleWheelItemMeasure Out;
	if (Positions.Num() < MinVertices)
	{
		Out.Problem = TEXT("The wheel has too few points to measure. Use a model of the rim (and the tire, if you want one).");
		return Out;
	}
	double MinX = TNumericLimits<double>::Max();
	double MaxX = -TNumericLimits<double>::Max();
	double MaxRadius = 0.0;
	FBox2D Axle(ForceInit);
	for (const FVector3f& Point : Positions)
	{
		MinX = FMath::Min<double>(MinX, Point.X);
		MaxX = FMath::Max<double>(MaxX, Point.X);
		MaxRadius = FMath::Max(MaxRadius, FVector2D(Point.Y, Point.Z).Size());
		Axle += FVector2D(Point.Y, Point.Z);
	}
	Out.ReferenceRadius = MaxRadius;
	Out.ReferenceWidth = MaxX - MinX;
	Out.AxialCenter = (MaxX + MinX) / 2.0;
	Out.AxleOffset = Axle.GetCenter().Size();
	if (Out.ReferenceRadius < 1.0 || Out.ReferenceWidth < 0.1)
	{
		Out.Problem = Out.ReferenceWidth < 0.1
			? TEXT("The wheel is flat along its axle (X). Model the rim with its axle along the X axis, so it has a width.")
			: TEXT("The wheel is smaller than a centimeter across.");
		return Out;
	}
	// The axle runs along X through the origin, and the rim sits on it: a wheel that does not is not fitted wrongly, it is refused, with what to do.
	if (Out.ReferenceWidth > 2.0 * Out.ReferenceRadius)
	{
		Out.Problem = TEXT("The wheel is longer along X than it is round: its axle should run along the X axis, so turn the model.");
		return Out;
	}
	if (Out.AxleOffset > 0.1 * Out.ReferenceRadius)
	{
		Out.Problem = TEXT("The wheel's axle does not run through the model's origin (the point it turns about). Move the model so the middle of the rim is at the origin.");
		return Out;
	}
	if (FMath::Abs(Out.AxialCenter) > 0.1 * Out.ReferenceWidth + 0.1)
	{
		Out.Problem = TEXT("The wheel is not centered on its origin along the axle. Move the model so the middle of its width is at the origin.");
		return Out;
	}
	Out.bOk = true;
	return Out;
}

FMantleWheelItemMeasure FMantleWheelItem::MeasureMesh(const UStaticMesh& Mesh)
{
	const FStaticMeshRenderData* Data = Mesh.GetRenderData();
	if (!Data || Data->LODResources.IsEmpty())
	{
		FMantleWheelItemMeasure Out;
		Out.Problem = TEXT("The wheel model has no geometry.");
		return Out;
	}
	const FPositionVertexBuffer& Buffer = Data->LODResources[0].VertexBuffers.PositionVertexBuffer;
	TArray<FVector3f> Positions;
	Positions.Reserve(Buffer.GetNumVertices());
	for (uint32 Index = 0; Index < Buffer.GetNumVertices(); ++Index)
	{
		Positions.Add(Buffer.VertexPosition(Index));
	}
	return Measure(Positions);
}
