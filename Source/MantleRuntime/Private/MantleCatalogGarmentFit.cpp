// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCatalogGarmentFit.h"

#include "HAL/PlatformTime.h"
#include "MantleBodyShape.h"
#include "MantleRuntime.h"
#include "MantleSkinMask.h"

// Named, not anonymous: unity builds.
namespace MantleCatalogGarmentFitPrivate
{
	bool RefuseCatalogFit(const FString& Reason, FString* OutError)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT FAIL catalog: %s"), *Reason);
		if (OutError)
		{
			*OutError = Reason;
		}
		return false;
	}
}

bool FMantleCatalogGarmentFit::DeriveAuthoredClearance(const FMantleSkinnedMesh& Garment, const FMantleBodyShape& Source, const FMantleSkinnedMesh& Topology,
	FMantleGarmentAuthoredClearance& Out, FString* OutError, const FMantleSkinnedMeshLOD* SourceFace)
{
	using namespace MantleCatalogGarmentFitPrivate;
	Out = FMantleGarmentAuthoredClearance();
	if (Topology.LODs.Num() == 0)
	{
		return RefuseCatalogFit(TEXT("the wearer has no LOD0 to take the reference triangles from"), OutError);
	}
	const FMantleSkinnedMeshLOD& Reference = Topology.LODs[0];
	if (Source.Positions.Num() != Reference.NumVertices() || Reference.Normals.Num() != Reference.NumVertices() || Reference.NumTriangles() == 0)
	{
		return RefuseCatalogFit(FString::Printf(TEXT("the source shape has %d positions; the wearer's LOD0 has %d vertices, %d normals and %d triangles"),
			Source.Positions.Num(), Reference.NumVertices(), Reference.Normals.Num(), Reference.NumTriangles()), OutError);
	}

	// The source body as a surface: its own positions over the shared reference triangles. The wearer's normals
	// only orient each triangle (FMantleGarmentClearance's TriangleSign), which the shared winding decides.
	FMantleSkinnedMeshLOD SourceBody;
	SourceBody.Positions = Source.Positions;
	SourceBody.Normals = Reference.Normals;
	SourceBody.Indices = Reference.Indices;
	// The face it was authored with, when the item names one (a face without triangles is no face).
	const FMantleSkinnedMeshLOD* Face = SourceFace && SourceFace->NumTriangles() > 0 ? SourceFace : nullptr;
	if (!FMantleGarmentFitter::ComputeAuthoredClearance(Garment, SourceBody, Face, Out, OutError))
	{
		return false;
	}

	// #877: the skin it hides, decided on the body it was made on. Without it the dressed body takes the wearer fallback.
	TSharedRef<FMantleSkinMask> Mask = MakeShared<FMantleSkinMask>();
	FString MaskError;
	if (Garment.LODs.Num() > 0
		&& FMantleSkinMask::Derive(FMantleGarmentSurface(Garment.LODs[0]), Source.Positions, Reference.Normals, Reference.Indices, FMantleSkinMaskParams(), *Mask, &MaskError))
	{
		Mask->SourcePositionsHash = Source.PositionsHash;
		Out.SkinMask = Mask;
	}
	else
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT skin mask not derived (%s); the body under it takes the wearer fallback"),
			MaskError.IsEmpty() ? TEXT("the garment has no LOD0") : *MaskError);
	}
	return true;
}

bool FMantleCatalogGarmentFit::SharesSurface(const FMantleGarmentAuthoredClearance& Authored, const FMantleSkinnedMesh& WearerBody)
{
	if (WearerBody.LODs.Num() == 0)
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& Wearer = WearerBody.LODs[0];
	return Authored.Body.NumSurfaceVertices == Wearer.NumVertices() && Authored.Body.SurfaceIndices == Wearer.Indices;
}

bool FMantleCatalogGarmentFit::Fit(const FMantleSkinnedMesh& Garment, const FMantleWarpField& Field, const FMantleSkinnedMesh& WearerBody,
	const FMantleGarmentAuthoredClearance* Authored, FMantleSkinnedMesh& OutFitted, FString* OutError, FMantleCatalogFitResult* OutResult,
	const FMantleSkinnedMeshLOD* WearerFace, const TConstArrayView<FMantleSkinnedMeshBone> WearerFaceBones)
{
	const double Start = FPlatformTime::Seconds();
	FMantleCatalogFitResult Result;
	if (!Authored)
	{
		Result.WarpReason = TEXT("no authored clearance");
	}
	else if (Authored->NumLODs() != Garment.LODs.Num())
	{
		// FitToWearer fits every LOD through its own clearance; one derived for another LOD count is not this garment's.
		Result.WarpReason = FString::Printf(TEXT("the authored clearance covers %d of %d LODs"), Authored->NumLODs(), Garment.LODs.Num());
	}
	else if (!SharesSurface(*Authored, WearerBody))
	{
		Result.WarpReason = TEXT("the wearer's triangles differ from the authored clearance's");
	}

	bool bFitted = false;
	if (Result.WarpReason.IsEmpty())
	{
		Result.Path = EMantleCatalogFitPath::Wearer;
		// The wearer's face, for the collar's restore (FitToWearer runs it only when Authored has the face clearance too)
		// and, with its bones, for the collar's skin weights.
		const FMantleSkinnedMeshLOD* Face = WearerFace && WearerFace->NumTriangles() > 0 ? WearerFace : nullptr;
		bFitted = FMantleGarmentFitter::FitToWearer(Garment, *Authored, Field, WearerBody, Face, OutFitted, OutError, &Result.Report,
			Face ? WearerFaceBones : TConstArrayView<FMantleSkinnedMeshBone>());
	}
	else
	{
		Result.Path = EMantleCatalogFitPath::Warp;
		bFitted = FMantleGarmentFitter::Fit(Garment, Field, FMantleReferencePose::FromSkinnedMesh(WearerBody), OutFitted, OutError);
	}
	Result.Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
	if (OutResult)
	{
		*OutResult = MoveTemp(Result);
	}
	return bFitted;
}

TSharedPtr<const FMantleWarpField> FMantleCatalogGarmentFit::ComputeField(const FMantleBodyShape& Source, const FMantleSkinnedMesh& WearerBody, FString& OutError,
	const FMantleSkinnedMeshLOD* WearerFace, FMantleNeckSamples::FResult* OutNeck)
{
	if (OutNeck)
	{
		*OutNeck = FMantleNeckSamples::FResult();
	}
	if (WearerBody.LODs.Num() == 0)
	{
		OutError = TEXT("the wearer has no mesh");
		return nullptr;
	}
	const TArray<FVector3f>& WearerPositions = WearerBody.LODs[0].Positions;

	// Through the neck, when the shape carries one and the wearer's face can take it. The wearer's half of the
	// neck samples is appended to the wearer's positions, as the plugin appended the source half to the shape's.
	if (WearerFace && WearerFace->NumTriangles() > 0 && Source.Neck.IsSet())
	{
		FMantleNeckSamples::FResult Neck;
		if (FMantleNeckSamples::BuildWearer(Source.Neck.Method, Source.Neck.Ids, Source.Neck.Positions, *WearerFace, WearerPositions, Neck))
		{
			TArray<FVector3f> Avatar;
			Avatar.Reserve(WearerPositions.Num() + Neck.Num());
			Avatar.Append(WearerPositions);
			Avatar.Append(Neck.Wearer);
			FString NeckError;
			TSharedPtr<const FMantleWarpField> Field = FMantleGarmentFitter::ComputeField(Source.MakeNeckShape(), Avatar, Source.Positions.Num(), Neck.Num(), &NeckError);
			if (Field.IsValid())
			{
				UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT neck field method=%s samples=%d filled=%d identity=%d ms=%.2f"),
					Neck.MethodName(), Neck.Num(), Neck.RaysFilled, Field->bIdentity ? 1 : 0, Neck.Ms);
				if (OutNeck)
				{
					*OutNeck = MoveTemp(Neck);
				}
				return Field;
			}
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT neck: the field through the neck failed (%s); the fit runs from the body alone"), *NeckError);
		}
	}
	// The shape and the wearer's LOD0 must both be the reference topology; the fitter refuses otherwise.
	return FMantleGarmentFitter::ComputeField(Source, WearerPositions, Source.Positions.Num(), &OutError);
}

bool FMantleCatalogGarmentFit::ComputeNeck(const FMantleBodyShape& Shape, const FMantleSkinnedMeshLOD& SourceFace, const int32 PointCount,
	FMantleBodyShapeNeck& Out, FString* OutError, double* OutMs)
{
	using namespace MantleCatalogGarmentFitPrivate;
	Out = FMantleBodyShapeNeck();
	if (OutMs)
	{
		*OutMs = 0.0;
	}
	EMantleNeckMethod Method = EMantleNeckMethod::None;
	TArray<int32> Ids;
	TArray<FVector3f> Samples;
	if (!FMantleNeckSamples::BuildSource(SourceFace, Shape.Positions, Method, Ids, Samples))
	{
		return RefuseCatalogFit(TEXT("the face gives no neck samples over the body (no neck base, or no ray hits it)"), OutError);
	}
	FMantleBodyShape Extended = FMantleGarmentFitter::ExtendShape(Shape, Samples);
	FString Error;
	if (!FMantleGarmentFitter::ComputeSourceWeights(Extended, PointCount, Shape.Positions.Num(), Samples.Num(), &Error, OutMs))
	{
		return RefuseCatalogFit(FString::Printf(TEXT("the neck's warp weights: %s"), *Error), OutError);
	}
	Out.Method = Method;
	Out.Ids = MoveTemp(Ids);
	Out.Positions = MoveTemp(Samples);
	Out.Weights = MoveTemp(Extended.Weights);
	return true;
}

bool FMantleCatalogGarmentFit::TracksBody(const FString& ApiSlot)
{
	// Case-sensitive: API slot ids are lower case (FString's == ignores case).
	return ApiSlot.Equals(TEXT("top"), ESearchCase::CaseSensitive) || ApiSlot.Equals(TEXT("bottom"), ESearchCase::CaseSensitive)
		|| ApiSlot.Equals(TEXT("footwear"), ESearchCase::CaseSensitive);
}
