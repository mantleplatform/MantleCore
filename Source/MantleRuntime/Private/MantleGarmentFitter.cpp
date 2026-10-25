// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentFitter.h"

#include "MantleBodyTopologyData.h"
#include "MantleGarmentGeometry.h"
#include "MantleRBFInterpolation.h"
#include "MantleRuntime.h"
#include "Async/ParallelFor.h"
#include "Misc/ScopeLock.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleGarmentFitterPrivate
{
	/** An avatar within this of the source shape at every vertex is the source shape. */
	constexpr float IdentityToleranceCm = 1.0e-3f;

	/** These deform the main humanoid chain; dropping them is unrelated-skeleton retargeting, not a helper fallback. */
	bool IsRequiredGarmentBone(const FName Bone)
	{
		const FString Name = Bone.ToString();
		return Name == TEXT("pelvis") || Name == TEXT("head") || Name == TEXT("spine")
			|| Name == TEXT("spine_01") || Name == TEXT("spine_02") || Name == TEXT("spine_03")
			|| Name == TEXT("spine_04") || Name == TEXT("spine_05") || Name == TEXT("neck_01") || Name == TEXT("neck_02")
			|| Name == TEXT("clavicle_l") || Name == TEXT("clavicle_r")
			|| Name == TEXT("upperarm_l") || Name == TEXT("upperarm_r")
			|| Name == TEXT("lowerarm_l") || Name == TEXT("lowerarm_r")
			|| Name == TEXT("hand_l") || Name == TEXT("hand_r")
			|| Name == TEXT("thigh_l") || Name == TEXT("thigh_r")
			|| Name == TEXT("calf_l") || Name == TEXT("calf_r")
			|| Name == TEXT("foot_l") || Name == TEXT("foot_r");
	}

	/** Empty without neck samples, " plus N neck samples" with them. */
	FString DescribeNeckSamples(const int32 NumNeckSamples)
	{
		return NumNeckSamples == 0 ? FString() : FString::Printf(TEXT(" plus %d neck samples"), NumNeckSamples);
	}

	/**
	 * Source warped by Field into Fitted (a copy of Source): positions through the field; normals and
	 * tangents by warping a 1 cm offset along them and taking the difference (the engine's DeformPoints
	 * does the same for normals), the tangent re-orthogonalised to the new normal, the binormal sign kept;
	 * degenerate results keep the original direction.
	 */
	void WarpLOD(const FMantleWarpField& Field, const FMantleSkinnedMeshLOD& Source, FMantleSkinnedMeshLOD& Fitted)
	{
		const bool bTangents = Source.Tangents.Num() == Source.NumVertices();
		ParallelFor(Source.NumVertices(), [&Field, &Source, &Fitted, bTangents](const int32 Vertex)
		{
			const FVector3f P = Source.Positions[Vertex];
			const FVector3f NewP = Field.Evaluate(P);
			Fitted.Positions[Vertex] = NewP;

			const FVector3f Normal = Source.Normals[Vertex];
			FVector3f NewNormal = (Field.Evaluate(P + Normal) - NewP).GetSafeNormal();
			if (NewNormal.IsNearlyZero())
			{
				NewNormal = Normal;
			}
			Fitted.Normals[Vertex] = NewNormal;

			if (bTangents)
			{
				const FVector4f Tangent4 = Source.Tangents[Vertex];
				const FVector3f Tangent(Tangent4.X, Tangent4.Y, Tangent4.Z);
				FVector3f NewTangent = Field.Evaluate(P + Tangent) - NewP;
				NewTangent = (NewTangent - FVector3f::DotProduct(NewTangent, NewNormal) * NewNormal).GetSafeNormal();
				if (NewTangent.IsNearlyZero())
				{
					NewTangent = Tangent;
				}
				// W is the binormal sign: a smooth warp keeps the frame's handedness.
				Fitted.Tangents[Vertex] = FVector4f(NewTangent.X, NewTangent.Y, NewTangent.Z, Tangent4.W);
			}
		});
	}

	/** Mean and largest vertex move from Source to Fitted (same vertex count), cm. */
	void MoveStats(const FMantleSkinnedMeshLOD& Source, const FMantleSkinnedMeshLOD& Fitted, double& OutMeanCm, double& OutMaxCm)
	{
		double SumMove = 0.0;
		OutMaxCm = 0.0;
		const int32 N = FMath::Min(Source.NumVertices(), Fitted.NumVertices());
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const double Move = FVector3f::Dist(Source.Positions[Vertex], Fitted.Positions[Vertex]);
			SumMove += Move;
			OutMaxCm = FMath::Max(OutMaxCm, Move);
		}
		OutMeanCm = N > 0 ? SumMove / N : 0.0;
	}
}

bool FMantleWarpField::IsValid() const
{
	if (bIdentity)
	{
		return true;
	}
	const int32 S = SampleRestPositions.Num();
	return S > 0 && TargetSamples.Num() == S && SampleDeformations.Num() == S + 4;
}

FVector3f FMantleWarpField::Evaluate(const FVector3f& P) const
{
	if (bIdentity)
	{
		return P;
	}
	// The engine's UE::MeshResizing::Private::DeformPoint, term for term and in the same order
	// (MantleRBFInterpolation.cpp keeps the engine's own copy for the tests).
	const int32 S = SampleRestPositions.Num();
	FVector3f ControlPoint = P;
	for (int32 Sample = 0; Sample < S; ++Sample)
	{
		const FVector3f PositionDelta = P - SampleRestPositions[Sample];
		const float FunctionValue = FMath::Sqrt(FVector3f::DotProduct(PositionDelta, PositionDelta) + 1);
		ControlPoint += FunctionValue * SampleDeformations[Sample];
	}
	ControlPoint += SampleDeformations[S];
	ControlPoint += SampleDeformations[S + 1] * P.X;
	ControlPoint += SampleDeformations[S + 2] * P.Y;
	ControlPoint += SampleDeformations[S + 3] * P.Z;
	return ControlPoint;
}

bool FMantleGarmentFitter::ComputeSourceWeights(FMantleBodyShape& Shape, const int32 PointCount, const int32 ReferenceVertexCount,
	FString* OutError, double* OutMs)
{
	return ComputeSourceWeights(Shape, PointCount, ReferenceVertexCount, 0, OutError, OutMs);
}

bool FMantleGarmentFitter::ComputeSourceWeights(FMantleBodyShape& Shape, const int32 PointCount, const int32 ReferenceVertexCount, const int32 NumNeckSamples,
	FString* OutError, double* OutMs)
{
	using namespace MantleGarmentFitterPrivate;

	Shape.Weights = FMantleRBFInterpolationData();
	if (OutMs)
	{
		*OutMs = 0.0;
	}
	if (NumNeckSamples < 0 || Shape.Positions.Num() != ReferenceVertexCount + NumNeckSamples)
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("weights"), FString::Printf(TEXT("the shape has %d vertices; the reference topology has %d%s"),
			Shape.Positions.Num(), ReferenceVertexCount, *DescribeNeckSamples(NumNeckSamples)), OutError);
	}
	if (PointCount < 1)
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("weights"), FString::Printf(TEXT("%d interpolation points (at least 1)"), PointCount), OutError);
	}

	const double Start = MantleGarmentGeometry::NowMs();
	FMantleRBFInterpolation::GenerateWeights(TConstArrayView<FVector3f>(Shape.Positions), PointCount, Shape.Weights);
	const double Ms = MantleGarmentGeometry::NowMs() - Start;
	if (OutMs)
	{
		*OutMs = Ms;
	}

	FString Invalid;
	if (!Shape.ValidateWeights(Invalid))
	{
		Shape.Weights = FMantleRBFInterpolationData();
		return MantleGarmentGeometry::RefuseFit(TEXT("weights"), FString::Printf(TEXT("GenerateWeights with %d points gave unusable weights: %s"), PointCount, *Invalid), OutError);
	}
	int32 NeckCenters = 0;
	for (const int32 SampleIndex : Shape.Weights.SampleIndices)
	{
		NeckCenters += SampleIndex >= ReferenceVertexCount ? 1 : 0;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_GARMENTFIT weights shape=%s verts=%d neckSamples=%d points=%d samples=%d neckCenters=%d ms=%.1f bytes=%lld"),
		*UMantleBodyTopologyData::FormatHash(Shape.PositionsHash), Shape.Positions.Num(), NumNeckSamples, PointCount,
		Shape.Weights.SampleIndices.Num(), NeckCenters, Ms, Shape.GetWeightsBytes());
	return true;
}

TSharedPtr<const FMantleWarpField> FMantleGarmentFitter::ComputeField(const FMantleBodyShape& Shape, const TConstArrayView<FVector3f> AvatarPositions,
	const int32 ReferenceVertexCount, FString* OutError)
{
	return ComputeField(Shape, AvatarPositions, ReferenceVertexCount, 0, OutError);
}

TSharedPtr<const FMantleWarpField> FMantleGarmentFitter::ComputeField(const FMantleBodyShape& Shape, const TConstArrayView<FVector3f> AvatarPositions,
	const int32 ReferenceVertexCount, const int32 NumNeckSamples, FString* OutError)
{
	using namespace MantleGarmentFitterPrivate;

	if (NumNeckSamples < 0 || Shape.Positions.Num() != ReferenceVertexCount + NumNeckSamples)
	{
		MantleGarmentGeometry::RefuseFit(TEXT("field"), FString::Printf(TEXT("the shape has %d vertices; the reference topology has %d%s"),
			Shape.Positions.Num(), ReferenceVertexCount, *DescribeNeckSamples(NumNeckSamples)), OutError);
		return nullptr;
	}
	if (AvatarPositions.Num() != ReferenceVertexCount + NumNeckSamples)
	{
		MantleGarmentGeometry::RefuseFit(TEXT("field"), FString::Printf(TEXT("the avatar has %d vertices; the reference topology has %d%s"),
			AvatarPositions.Num(), ReferenceVertexCount, *DescribeNeckSamples(NumNeckSamples)), OutError);
		return nullptr;
	}
	FString Invalid;
	if (!Shape.ValidateWeights(Invalid))
	{
		MantleGarmentGeometry::RefuseFit(TEXT("field"), FString::Printf(TEXT("the shape's weights: %s"), *Invalid), OutError);
		return nullptr;
	}

	const double Start = MantleGarmentGeometry::NowMs();
	TSharedPtr<FMantleWarpField> Field = MakeShared<FMantleWarpField>();
	Field->SourceHash = Shape.PositionsHash;
	Field->AvatarHash = UMantleBodyTopologyData::ComputePositionsHash(AvatarPositions);
	Field->NumNeckSamples = NumNeckSamples;

	// The avatar is the shape itself: no warp (the RBF would only add noise).
	Field->bIdentity = true;
	for (int32 Vertex = 0; Vertex < AvatarPositions.Num(); ++Vertex)
	{
		if (!AvatarPositions[Vertex].Equals(Shape.Positions[Vertex], IdentityToleranceCm))
		{
			Field->bIdentity = false;
			break;
		}
	}

	if (!Field->bIdentity)
	{
		// The engine's contract: GenerateMeshSamples takes the target
		// positions of the SAMPLE vertices only, in SampleIndices order, not the whole target mesh, and
		// checks the count. ValidateWeights has put every index in range.
		const FMantleRBFInterpolationData& Weights = Shape.Weights;
		Field->SampleRestPositions = Weights.SampleRestPositions;
		Field->TargetSamples.Reserve(Weights.SampleIndices.Num());
		for (const int32 SampleIndex : Weights.SampleIndices)
		{
			Field->TargetSamples.Add(AvatarPositions[SampleIndex]);
		}
		FMantleRBFInterpolation::GenerateMeshSamples(Field->TargetSamples, Weights, Field->SampleDeformations);
		if (!Field->IsValid())
		{
			MantleGarmentGeometry::RefuseFit(TEXT("field"), FString::Printf(TEXT("GenerateMeshSamples gave %d deformations for %d samples (expected samples + 4)"),
				Field->SampleDeformations.Num(), Weights.SampleIndices.Num()), OutError);
			return nullptr;
		}
	}
	Field->Ms = MantleGarmentGeometry::NowMs() - Start;
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT field source=%s avatar=%s samples=%d identity=%d ms=%.3f"),
		*UMantleBodyTopologyData::FormatHash(Field->SourceHash), *UMantleBodyTopologyData::FormatHash(Field->AvatarHash),
		Field->SampleRestPositions.Num(), Field->bIdentity ? 1 : 0, Field->Ms);
	return Field;
}

bool FMantleGarmentFitter::Fit(const FMantleSkinnedMesh& Garment, const FMantleWarpField& Field, const FMantleReferencePose& Wearer,
	FMantleSkinnedMesh& OutFitted, FString* OutError, FMantleGarmentFitStats* OutStats)
{
	using namespace MantleGarmentFitterPrivate;

	const double Start = MantleGarmentGeometry::NowMs();
	FString Invalid;
	if (!Garment.Validate(Invalid))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the garment is not a valid skinned mesh: %s"), *Invalid), OutError);
	}
	if (!Field.IsValid())
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the field is not valid (%d samples, %d targets, %d deformations)"),
			Field.SampleRestPositions.Num(), Field.TargetSamples.Num(), Field.SampleDeformations.Num()), OutError);
	}

	OutFitted = Garment;
	if (!Field.bIdentity)
	{
		for (int32 LODIndex = 0; LODIndex < Garment.LODs.Num(); ++LODIndex)
		{
			WarpLOD(Field, Garment.LODs[LODIndex], OutFitted.LODs[LODIndex]);
		}
	}

	FMantleGarmentFitStats Stats;
	if (!Rebind(OutFitted, Wearer, OutError, &Stats))
	{
		return false;
	}

	const FMantleSkinnedMeshLOD& Fitted0 = OutFitted.LODs[0];
	MoveStats(Garment.LODs[0], Fitted0, Stats.MeanMoveCm, Stats.MaxMoveCm);
	Stats.NumVertices = Fitted0.NumVertices();
	Stats.NumTriangles = Fitted0.NumTriangles();
	Stats.FitMs = MantleGarmentGeometry::NowMs() - Start;
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT fit verts=%d tris=%d identity=%d ms=%.3f moveMeanCm=%.3f moveMaxCm=%.3f remapped=%d"),
		Stats.NumVertices, Stats.NumTriangles, Field.bIdentity ? 1 : 0, Stats.FitMs, Stats.MeanMoveCm, Stats.MaxMoveCm, Stats.bBonesRemapped ? 1 : 0);
	if (OutStats)
	{
		*OutStats = Stats;
	}
	return true;
}

bool FMantleGarmentFitter::Rebind(FMantleSkinnedMesh& Garment, const FMantleReferencePose& Wearer, FString* OutError,
	FMantleGarmentFitStats* OutStats)
{
	using namespace MantleGarmentFitterPrivate;

	FString Invalid;
	if (!Garment.Validate(Invalid))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("rebind"), FString::Printf(TEXT("the garment is not a valid skinned mesh: %s"), *Invalid), OutError);
	}
	if (!Wearer.Validate(Invalid))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("rebind"), FString::Printf(TEXT("the wearer's reference pose: %s"), *Invalid), OutError);
	}
	const int32 NumGarmentBones = Garment.Bones.Num();
	bool bSameTable = NumGarmentBones == Wearer.Num();
	for (int32 BoneIndex = 0; bSameTable && BoneIndex < NumGarmentBones; ++BoneIndex)
	{
		bSameTable = Garment.Bones[BoneIndex].Name == Wearer.Bones[BoneIndex].Name
			&& Garment.Bones[BoneIndex].ParentIndex == Wearer.Bones[BoneIndex].ParentIndex;
	}

	int32 UnusedDropped = 0;
	int32 AncestorRemapped = 0;
	if (!bSameTable)
	{
		// Garment bone -> wearer bone by name; missing helper bones use their nearest retained ancestor.
		TMap<FName, int32> WearerIndex;
		WearerIndex.Reserve(Wearer.Num());
		for (int32 BoneIndex = 0; BoneIndex < Wearer.Num(); ++BoneIndex)
		{
			WearerIndex.Add(Wearer.Bones[BoneIndex].Name, BoneIndex);
		}
		if (Wearer.Bones[0].Name != Garment.Bones[0].Name)
		{
			return MantleGarmentGeometry::RefuseFit(TEXT("rebind"), FString::Printf(TEXT("the garment's required root %s is not on the wearer"),
				*Garment.Bones[0].Name.ToString()), OutError);
		}
		TArray<bool> Weighted;
		Weighted.Init(false, NumGarmentBones);
		for (const FMantleSkinnedMeshLOD& LOD : Garment.LODs)
		{
			for (int32 Slot = 0; Slot < LOD.InfluenceBones.Num(); ++Slot)
			{
				if (LOD.InfluenceWeights[Slot] > 0 && LOD.InfluenceBones[Slot] < NumGarmentBones)
				{
					Weighted[LOD.InfluenceBones[Slot]] = true;
				}
			}
		}
		TArray<int32> Remap;
		Remap.Init(INDEX_NONE, NumGarmentBones);
		TArray<FString> Missing;
		TArray<FString> AncestorPairs;
		for (int32 BoneIndex = 0; BoneIndex < NumGarmentBones; ++BoneIndex)
		{
			if (const int32* Found = WearerIndex.Find(Garment.Bones[BoneIndex].Name))
			{
				Remap[BoneIndex] = *Found;
			}
			else if (IsRequiredGarmentBone(Garment.Bones[BoneIndex].Name))
			{
				Missing.Add(Garment.Bones[BoneIndex].Name.ToString());
			}
			else if (Weighted[BoneIndex])
			{
				for (int32 Ancestor = Garment.Bones[BoneIndex].ParentIndex; Ancestor != INDEX_NONE; Ancestor = Garment.Bones[Ancestor].ParentIndex)
				{
					if (const int32* FoundAncestor = WearerIndex.Find(Garment.Bones[Ancestor].Name))
					{
						Remap[BoneIndex] = *FoundAncestor;
						AncestorPairs.Add(FString::Printf(TEXT("%s -> %s"), *Garment.Bones[BoneIndex].Name.ToString(), *Wearer.Bones[*FoundAncestor].Name.ToString()));
						break;
					}
				}
				if (Remap[BoneIndex] == INDEX_NONE) { Missing.Add(Garment.Bones[BoneIndex].Name.ToString()); }
			}
			else
			{
				++UnusedDropped;
			}
		}
		if (Missing.Num() > 0)
		{
			return MantleGarmentGeometry::RefuseFit(TEXT("rebind"), FString::Printf(TEXT("%d required or weighted garment bones are not on the wearer (%s%s)"),
				Missing.Num(), *FString::Join(TArrayView<const FString>(Missing.GetData(), FMath::Min(Missing.Num(), 5)), TEXT(", ")),
				Missing.Num() > 5 ? TEXT(", ...") : TEXT("")), OutError);
		}
		for (FMantleSkinnedMeshLOD& LOD : Garment.LODs)
		{
			for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
			{
				const int32 Base = Vertex * LOD.NumInfluences;
				for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
				{
					const int32 Index = Base + Slot;
					const int32 Original = LOD.InfluenceBones[Index];
					if (LOD.InfluenceWeights[Index] == 0) { LOD.InfluenceBones[Index] = 0; continue; }
					AncestorRemapped += WearerIndex.Contains(Garment.Bones[Original].Name) ? 0 : 1;
					LOD.InfluenceBones[Index] = static_cast<uint16>(Remap[Original]);
					// Keep the first slot and its ordering; duplicate weights sum exactly, without rounding.
					for (int32 Previous = 0; Previous < Slot; ++Previous)
					{
						const int32 Earlier = Base + Previous;
						if (LOD.InfluenceWeights[Earlier] > 0 && LOD.InfluenceBones[Earlier] == LOD.InfluenceBones[Index])
						{
							LOD.InfluenceWeights[Earlier] += LOD.InfluenceWeights[Index];
							LOD.InfluenceWeights[Index] = 0;
							LOD.InfluenceBones[Index] = 0;
							break;
						}
					}
				}
			}
		}
		if (AncestorRemapped > 0)
		{
			// A refit or the builder's second rebind repeats the same remap: warn once per distinct remap, then Verbose.
			const FString Pairs = FString::Join(AncestorPairs, TEXT(", "));
			static FCriticalSection RemapLoggedLock;
			static TSet<FString> RemapLogged;
			bool bFirst = false;
			{
				FScopeLock Lock(&RemapLoggedLock);
				RemapLogged.Add(Pairs, &bFirst);
			}
			bFirst = !bFirst;
			if (bFirst)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT remapped %d helper-bone influences to their nearest wearer ancestors (%s)"), AncestorRemapped, *Pairs);
			}
			else
			{
				UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT remapped %d helper-bone influences to their nearest wearer ancestors (%s)"), AncestorRemapped, *Pairs);
			}
		}
	}

	Garment.Bones = Wearer.Bones;
	Garment.ReferencePose = Wearer.LocalTransforms;
	if (OutStats)
	{
		OutStats->bRebound = true;
		OutStats->bBonesRemapped = !bSameTable;
		OutStats->UnusedBonesDropped = UnusedDropped;
		OutStats->AncestorRemappedInfluences = AncestorRemapped;
	}
	return true;
}

FMantleBodyShape FMantleGarmentFitter::ExtendShape(const FMantleBodyShape& Body, const TConstArrayView<FVector3f> NeckSamples)
{
	FMantleBodyShape Out;
	Out.Positions.Reserve(Body.Positions.Num() + NeckSamples.Num());
	Out.Positions.Append(Body.Positions);
	Out.Positions.Append(NeckSamples.GetData(), NeckSamples.Num());
	Out.ReferencePose = Body.ReferencePose;
	Out.PositionsHash = UMantleBodyTopologyData::ComputePositionsHash(Out.Positions);
	return Out;
}

bool FMantleGarmentFitter::ComputeAuthoredClearance(const FMantleSkinnedMesh& Garment, const FMantleSkinnedMeshLOD& SourceBody, const FMantleSkinnedMeshLOD* SourceFace,
	FMantleGarmentAuthoredClearance& Out, FString* OutError)
{
	using namespace MantleGarmentFitterPrivate;

	Out = FMantleGarmentAuthoredClearance();
	if (Garment.LODs.Num() == 0 || Garment.LODs[0].NumVertices() == 0)
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("authored clearance"), TEXT("the garment has no LOD0 vertices"), OutError);
	}
	int32 FaceUntracked = 0;
	int32 FaceInside = 0;
	int32 LowerTracked = 0;
	// Every LOD has its own vertices, so each gets its own clearance over the same source surfaces.
	for (int32 LODIndex = 0; LODIndex < Garment.LODs.Num(); ++LODIndex)
	{
		const TArray<FVector3f>& Positions = Garment.LODs[LODIndex].Positions;
		FMantleGarmentClearance& Body = LODIndex == 0 ? Out.Body : Out.LowerBody.AddDefaulted_GetRef();
		if (!FMantleGarmentClearance::Compute(Positions, SourceBody, Body, FMantleGarmentClearance::DefaultSearchCm, OutError))
		{
			Out = FMantleGarmentAuthoredClearance();
			return false;
		}
		LowerTracked += LODIndex > 0 ? Body.NumTracked : 0;
		if (!SourceFace)
		{
			continue;
		}
		FMantleGarmentClearance& Face = LODIndex == 0 ? Out.Face : Out.LowerFace.AddDefaulted_GetRef();
		if (!FMantleGarmentClearance::Compute(Positions, *SourceFace, Face, FMantleGarmentClearance::FaceSearchCm, OutError))
		{
			Out = FMantleGarmentAuthoredClearance();
			return false;
		}
		// Not the face's open boundary (the chest and shoulder outline): a vertex nearest to it is not over the face.
		const int32 Untracked = Face.UntrackBoundary(*SourceFace);
		if (LODIndex == 0)
		{
			FaceUntracked = Untracked;
			for (int32 Vertex = 0; Vertex < Face.Triangle.Num(); ++Vertex)
			{
				FaceInside += (Face.IsTracked(Vertex) && Face.ClearanceCm[Vertex] < 0.0f) ? 1 : 0;
			}
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_GARMENTFIT authored verts=%d bodyTracked=%d bodyD0Min=%.2f bodyD0Mean=%.2f bodyD0Max=%.2f bodyMs=%.2f faceTracked=%d faceBoundaryUntracked=%d faceAuthoredInside=%d faceMs=%.2f lods=%d lowerBodyTracked=%d"),
		Garment.LODs[0].NumVertices(), Out.Body.NumTracked, Out.Body.MinClearanceCm, Out.Body.MeanClearanceCm, Out.Body.MaxClearanceCm, Out.Body.Ms,
		SourceFace ? Out.Face.NumTracked : -1, FaceUntracked, FaceInside, Out.Face.Ms, Out.NumLODs(), LowerTracked);
	return true;
}

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleGarmentFitterPrivate
{
	/** What SeamWeld found over every LOD: the seams (positions shared by two or more sections), the copies on them, and the time. */
	struct FSeamWeldStats
	{
		int32 Groups = 0;
		int32 Copies = 0;
		double Ms = 0.0;
	};

	/**
	 * LOD's seams between sections, from its positions as authored (before the warp): OutSeams gets
	 * per vertex its seam id when it shares its position (quantised to MantleGarmentGeometry::
	 * WeldQuantumCm) with a vertex of another section, else INDEX_NONE. The restores push each seam's
	 * copies as one (FMantleClearanceRestore::Restore). Adds to Stats.
	 */
	void SeamWeld(const FMantleSkinnedMeshLOD& LOD, TArray<int32>& OutSeams, FSeamWeldStats& Stats)
	{
		TArray<int32> Points;
		const int32 NumPoints = MantleGarmentGeometry::WeldByPosition(LOD.Positions, Points);
		TArray<int32> Sections;
		MantleGarmentGeometry::VertexSections(LOD, Sections);
		TArray<int32> PointSection;
		PointSection.Init(INDEX_NONE, NumPoints);
		TBitArray<> Across(false, NumPoints);
		for (int32 Vertex = 0; Vertex < Points.Num(); ++Vertex)
		{
			const int32 Point = Points[Vertex];
			const int32 Section = Sections[Vertex];
			if (Section == INDEX_NONE)
			{
				continue;
			}
			if (PointSection[Point] == INDEX_NONE)
			{
				PointSection[Point] = Section;
			}
			else if (PointSection[Point] != Section)
			{
				Across[Point] = true;
			}
		}
		TArray<int32> SeamOf;
		SeamOf.Init(INDEX_NONE, NumPoints);
		int32 NumSeams = 0;
		OutSeams.Init(INDEX_NONE, Points.Num());
		for (int32 Vertex = 0; Vertex < Points.Num(); ++Vertex)
		{
			const int32 Point = Points[Vertex];
			if (!Across[Point])
			{
				continue;
			}
			if (SeamOf[Point] == INDEX_NONE)
			{
				SeamOf[Point] = NumSeams++;
			}
			OutSeams[Vertex] = SeamOf[Point];
			++Stats.Copies;
		}
		Stats.Groups += NumSeams;
	}

	/**
	 * Stages 1 to 4 of FMantleGarmentFitter::FitToWearer on one LOD of Garment: the warp, the body
	 * and face clearance restores and the skin transfer and smoothing, against the wearer's LOD0, with
	 * Authored holding this LOD's clearances in Body and Face. Fitted starts as a copy of Authored0 and
	 * keeps the garment's bone table (the rebind is the caller's, once for every LOD). Seams are this
	 * LOD's seams between sections (SeamWeld), which both restores push as one. WearerFaceBones is the
	 * face's bone table (empty: the skin transfer takes the body's weights only). Report gets this LOD's
	 * numbers. FitToWearer has checked the sizes.
	 */
	bool FitLODToWearer(const FMantleSkinnedMesh& Garment, const FMantleSkinnedMeshLOD& Authored0, FMantleSkinnedMeshLOD& Fitted,
		const FMantleGarmentAuthoredClearance& Authored, const FMantleWarpField& Field, const FMantleSkinnedMesh& WearerBody,
		const FMantleSkinnedMeshLOD* WearerFace, const TConstArrayView<FMantleSkinnedMeshBone> WearerFaceBones, const TConstArrayView<int32> Seams,
		FMantleGarmentFitReport& Report, FString* OutError)
	{
		const int32 N = Authored0.NumVertices();
		const TArray<FVector3f>& WearerPositions = WearerBody.LODs[0].Positions;

		// 1. The warp. The identity (the garment on its own body) keeps the authored positions bit-exact.
		const double WarpStart = MantleGarmentGeometry::NowMs();
		if (!Field.bIdentity)
		{
			WarpLOD(Field, Authored0, Fitted);
		}
		Report.WarpMs = MantleGarmentGeometry::NowMs() - WarpStart;
		Report.bIdentity = Field.bIdentity;
		FMantleFaceFlips::Measure(Authored0, Fitted, Report.FlipsWarp);

		// 2. The body clearance restore. Every vertex the source face tracks (the collar, the neck
		// opening) is the face restore's; the body has no neck, and measuring the collar against its
		// neck-base ring tore the band on every cross-body fit.
		if (!Field.bIdentity)
		{
			FMantleGarmentClearance BodyTarget = Authored.Body;
			if (Authored.HasFace())
			{
				for (int32 Vertex = 0; Vertex < N; ++Vertex)
				{
					if (Authored.Face.IsTracked(Vertex) && BodyTarget.IsTracked(Vertex))
					{
						BodyTarget.Untrack(Vertex);
						++Report.LeftToFace;
					}
				}
			}
			if (!FMantleClearanceRestore::Restore(Fitted, BodyTarget, WearerPositions, &Authored0, FMantleGarmentClearance::DefaultSlackCm,
				FMantleGarmentClearance::MinKeptClearanceCm, FMantleGarmentClearance::DefaultSmoothIters, &Report.BodyRestore, OutError, Seams))
			{
				return false;
			}
			FMantleFaceFlips::Measure(Authored0, Fitted, Report.FlipsBodyRestore);
		}

		// 3. The face clearance restore: the collar keeps its AUTHORED clearance to the wearer's face.
		// Each vertex within reach of this face is tracked to its true nearest face triangle here (faces
		// differ in vertices) and restored to what it was authored with on the source face.
		if (!Field.bIdentity && WearerFace && Authored.HasFace())
		{
			FMantleGarmentClearance FaceTarget;
			if (!FMantleGarmentClearance::Compute(Fitted.Positions, *WearerFace, FaceTarget, FMantleGarmentClearance::FaceSearchCm, OutError))
			{
				return false;
			}
			FaceTarget.UntrackBoundary(*WearerFace);
			Report.FaceWithin = FaceTarget.NumTracked;
			int32 Both = 0;
			for (int32 Vertex = 0; Vertex < N; ++Vertex)
			{
				if (!FaceTarget.IsTracked(Vertex))
				{
					continue;
				}
				if (!Authored.Face.IsTracked(Vertex))
				{
					// No authored reference: left alone.
					FaceTarget.Untrack(Vertex);
					continue;
				}
				++Both;
				FaceTarget.ClearanceCm[Vertex] = Authored.Face.ClearanceCm[Vertex];
			}
			Report.FaceTracked = Both;
			if (Both > 0)
			{
				if (!FMantleClearanceRestore::Restore(Fitted, FaceTarget, WearerFace->Positions, &Authored0, FMantleGarmentClearance::DefaultSlackCm,
					FMantleGarmentClearance::MinKeptClearanceCm, FMantleGarmentClearance::DefaultSmoothIters, &Report.FaceRestore, OutError, Seams))
				{
					return false;
				}
				FMantleFaceFlips::Measure(Authored0, Fitted, Report.FlipsFaceRestore);
				// The invariant re-measured on the restored garment: a vertex the flip limiter zeroed, or the
				// smoothing left short, is still below its authored face clearance.
				FMantleGarmentClearance After;
				if (FMantleGarmentClearance::Compute(Fitted.Positions, *WearerFace, After, FMantleGarmentClearance::FaceSearchCm))
				{
					for (int32 Vertex = 0; Vertex < N; ++Vertex)
					{
						if (!FaceTarget.IsTracked(Vertex) || !After.IsTracked(Vertex))
						{
							continue;
						}
						const float Short = FaceTarget.ClearanceCm[Vertex] - After.ClearanceCm[Vertex];
						if (Short > FMantleGarmentClearance::DefaultSlackCm)
						{
							++Report.FaceStillBelow;
							Report.FaceStillWorstCm = FMath::Max(Report.FaceStillWorstCm, Short);
						}
					}
				}
			}
		}

		// 4. The wearer's skin weights at the authored correspondence (before the rebind: the transfer writes
		// the garment's own bone indices, the rebind moves them onto the wearer's table), smoothed faded by
		// the authored clearance. The collar and the neck opening lie over the face's neck, which bends with
		// neck_01 and neck_02 while the body's neck base stays on spine_04: within the face's reach they
		// take the face's weights too (measured on the restored garment), or the collar stays with the
		// chest and opens a gap at the back of the neck once the head tips forward. The face's share is smooth
		// around the collar and the smoothing covers the vertices that took it, even snug ones, or the rim goes
		// wavy as the neck bends.
		FMantleGarmentClearance FaceNow;
		FMantleSkinTransferFace TransferFace;
		if (WearerFace && WearerFaceBones.Num() > 0 && FMantleGarmentClearance::Compute(Fitted.Positions, *WearerFace, FaceNow, FMantleGarmentClearance::FaceSearchCm))
		{
			FaceNow.UntrackBoundary(*WearerFace);
			TransferFace.Surface = WearerFace;
			TransferFace.Bones = WearerFaceBones;
			TransferFace.Clearance = &FaceNow;
		}
		TArray<float> FaceShare;
		if (!FMantleGarmentSkinTransfer::Transfer(Fitted, Garment.Bones, Authored.Body, WearerBody, TransferFace, &Report.Transfer, OutError, &FaceShare))
		{
			return false;
		}
		int32 Matches = 0;
		int32 Tracked = 0;
		if (FMantleGarmentSkinTransfer::DominantMatch(Fitted, Garment.Bones, Authored.Body, WearerBody, Matches, Tracked) && Tracked > 0)
		{
			Report.DominantMatchPct = 100.0 * Matches / Tracked;
		}
		TArray<float> Factor;
		FMantleGarmentSkinTransfer::SmoothFactor(Authored.Body, FMantleGarmentSkinTransfer::DefaultSmoothTightCm, FMantleGarmentSkinTransfer::DefaultSmoothLooseCm, Factor, &FaceShare);
		if (!FMantleGarmentSkinTransfer::Smooth(Fitted, FMantleGarmentSkinTransfer::DefaultSmoothIters, &Factor, &Report.Smooth))
		{
			return MantleGarmentGeometry::RefuseFit(TEXT("fit"), TEXT("the transferred skin weights could not be smoothed"), OutError);
		}
		if (FMantleGarmentSkinTransfer::DominantMatch(Fitted, Garment.Bones, Authored.Body, WearerBody, Matches, Tracked) && Tracked > 0)
		{
			Report.SmoothDominantMatchPct = 100.0 * Matches / Tracked;
		}
		return true;
	}

	/** Garment LOD LODIndex's authored clearances as a one-LOD FMantleGarmentAuthoredClearance (Body, and Face when there is one). */
	FMantleGarmentAuthoredClearance AuthoredClearanceOfLOD(const FMantleGarmentAuthoredClearance& Authored, const int32 LODIndex)
	{
		FMantleGarmentAuthoredClearance Out;
		Out.Body = Authored.BodyOf(LODIndex);
		if (const FMantleGarmentClearance* Face = Authored.FaceOf(LODIndex))
		{
			Out.Face = *Face;
		}
		return Out;
	}
}

bool FMantleGarmentFitter::FitToWearer(const FMantleSkinnedMesh& Garment, const FMantleGarmentAuthoredClearance& Authored, const FMantleWarpField& Field,
	const FMantleSkinnedMesh& WearerBody, const FMantleSkinnedMeshLOD* WearerFace, FMantleSkinnedMesh& OutFitted,
	FString* OutError, FMantleGarmentFitReport* OutReport, const TConstArrayView<FMantleSkinnedMeshBone> WearerFaceBones)
{
	using namespace MantleGarmentFitterPrivate;

	const double Start = MantleGarmentGeometry::NowMs();
	FMantleGarmentFitReport Report;
	FString Invalid;
	if (!Garment.Validate(Invalid))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the garment is not a valid skinned mesh: %s"), *Invalid), OutError);
	}
	if (Authored.NumLODs() != Garment.LODs.Num() || (Authored.HasFace() && Authored.LowerFace.Num() != Authored.LowerBody.Num()))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the authored clearance covers %d of the garment's %d LODs (made for another garment)"),
			Authored.NumLODs(), Garment.LODs.Num()), OutError);
	}
	if (!Field.IsValid())
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the field is not valid (%d samples, %d targets, %d deformations)"),
			Field.SampleRestPositions.Num(), Field.TargetSamples.Num(), Field.SampleDeformations.Num()), OutError);
	}
	for (int32 LODIndex = 0; LODIndex < Garment.LODs.Num(); ++LODIndex)
	{
		const int32 NumLODVertices = Garment.LODs[LODIndex].NumVertices();
		const FMantleGarmentClearance& Body = Authored.BodyOf(LODIndex);
		const FMantleGarmentClearance* Face = Authored.FaceOf(LODIndex);
		if (Body.Triangle.Num() != NumLODVertices || (Face && Face->Triangle.Num() != NumLODVertices))
		{
			return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the authored clearance of LOD%d has %d body and %d face entries for %d garment vertices (made for another garment)"),
				LODIndex, Body.Triangle.Num(), Face ? Face->Triangle.Num() : 0, NumLODVertices), OutError);
		}
		if (Body.NumSurfaceVertices != Authored.Body.NumSurfaceVertices)
		{
			return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the authored clearance of LOD%d was measured over a %d-vertex body, LOD0's over %d"),
				LODIndex, Body.NumSurfaceVertices, Authored.Body.NumSurfaceVertices), OutError);
		}
	}
	if (WearerBody.LODs.Num() == 0 || WearerBody.LODs[0].NumVertices() != Authored.Body.NumSurfaceVertices)
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), FString::Printf(TEXT("the wearer's body has %d LOD0 vertices; the garment's source body has %d"),
			WearerBody.LODs.Num() > 0 ? WearerBody.LODs[0].NumVertices() : 0, Authored.Body.NumSurfaceVertices), OutError);
	}
	if (!WearerBody.HasReferencePose())
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("fit"), TEXT("the wearer's body carries no reference pose to rebind to"), OutError);
	}
	const FMantleReferencePose WearerPose = FMantleReferencePose::FromSkinnedMesh(WearerBody);
	const FMantleSkinnedMeshLOD& Authored0 = Garment.LODs[0];
	const int32 N = Authored0.NumVertices();

	// The seams between sections of every LOD, once per fit, over the positions as authored (before the warp).
	const double WeldStart = MantleGarmentGeometry::NowMs();
	TArray<TArray<int32>> LODSeams;
	LODSeams.SetNum(Garment.LODs.Num());
	FSeamWeldStats Weld;
	for (int32 LODIndex = 0; LODIndex < Garment.LODs.Num(); ++LODIndex)
	{
		SeamWeld(Garment.LODs[LODIndex], LODSeams[LODIndex], Weld);
	}
	Weld.Ms = MantleGarmentGeometry::NowMs() - WeldStart;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_FIT seam weld: %d groups, %d copies, %.2f ms (lods=%d; groups of copies in two or more sections)"),
		Weld.Groups, Weld.Copies, Weld.Ms, Garment.LODs.Num());

	// 1 to 4 on LOD0 (the report's numbers), then on each lower LOD through its own clearance.
	OutFitted = Garment;
	if (!FitLODToWearer(Garment, Authored0, OutFitted.LODs[0], Authored, Field, WearerBody, WearerFace, WearerFaceBones, LODSeams[0], Report, OutError))
	{
		return false;
	}
	Report.NumLODs = Garment.LODs.Num();
	double LowerMatchSum = 0.0;
	int32 LowerMatchCount = 0;
	for (int32 LODIndex = 1; LODIndex < Garment.LODs.Num(); ++LODIndex)
	{
		const double LODStart = MantleGarmentGeometry::NowMs();
		FMantleGarmentFitReport LODReport;
		const FMantleGarmentAuthoredClearance LODAuthored = AuthoredClearanceOfLOD(Authored, LODIndex);
		if (!FitLODToWearer(Garment, Garment.LODs[LODIndex], OutFitted.LODs[LODIndex], LODAuthored, Field, WearerBody, WearerFace, WearerFaceBones, LODSeams[LODIndex], LODReport, OutError))
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_GARMENTFIT FAIL fit: LOD%d"), LODIndex);
			return false;
		}
		FMantleFaceFlips LODFinal;
		FMantleFaceFlips::Measure(Garment.LODs[LODIndex], OutFitted.LODs[LODIndex], LODFinal);
		Report.LowerLODsFlipsAdded += FMantleFaceFlips::CountAdded(LODReport.FlipsWarp, LODFinal);
		if (LODReport.SmoothDominantMatchPct >= 0.0)
		{
			LowerMatchSum += LODReport.SmoothDominantMatchPct;
			++LowerMatchCount;
		}
		Report.LowerLODsMs += MantleGarmentGeometry::NowMs() - LODStart;
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT wearer lod=%d verts=%d bodyRestore=[%s] faceRestore=[%s] transfer=%d/%d/%d dominantPct=%.1f ms=%.2f"),
			LODIndex, Garment.LODs[LODIndex].NumVertices(), *LODReport.BodyRestore.ToString(), *LODReport.FaceRestore.ToString(),
			LODReport.Transfer.FromBody, LODReport.Transfer.FromNearest, LODReport.Transfer.Kept, LODReport.SmoothDominantMatchPct, MantleGarmentGeometry::NowMs() - LODStart);
	}
	Report.LowerLODsSmoothDominantMatchPct = LowerMatchCount > 0 ? LowerMatchSum / LowerMatchCount : -1.0;

	// 5. The rebind (mandatory), every LOD at once.
	if (!Rebind(OutFitted, WearerPose, OutError, &Report.Rebind))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& Final = OutFitted.LODs[0];
	FMantleFaceFlips::Measure(Authored0, Final, Report.FlipsFinal);
	Report.FlipsAddedByRestores = FMantleFaceFlips::CountAdded(Report.FlipsWarp, Report.FlipsFinal);
	MoveStats(Authored0, Final, Report.MeanMoveCm, Report.MaxMoveCm);
	Report.Rebind.NumVertices = Final.NumVertices();
	Report.Rebind.NumTriangles = Final.NumTriangles();
	Report.Rebind.MeanMoveCm = Report.MeanMoveCm;
	Report.Rebind.MaxMoveCm = Report.MaxMoveCm;
	Report.TotalMs = MantleGarmentGeometry::NowMs() - Start;
	Report.Rebind.FitMs = Report.TotalMs;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_GARMENTFIT wearer verts=%d identity=%d warpMs=%.2f bodyRestore=[%s] leftToFace=%d faceWithin=%d faceTracked=%d faceRestore=[%s] faceStillBelow=%d transfer=%d/%d/%d transferFace=%d slots=%d->%d smoothPoints=%d dominantPct=%.1f/%.1f flipsWarp=%d flipsAdded=%d lods=%d lowerFlipsAdded=%d lowerDominantPct=%.1f lowerMs=%.2f totalMs=%.2f"),
		N, Field.bIdentity ? 1 : 0, Report.WarpMs, *Report.BodyRestore.ToString(), Report.LeftToFace, Report.FaceWithin, Report.FaceTracked,
		*Report.FaceRestore.ToString(), Report.FaceStillBelow, Report.Transfer.FromBody, Report.Transfer.FromNearest, Report.Transfer.Kept,
		Report.Transfer.FromFace, Report.Transfer.SlotsBefore, Report.Transfer.Influences, Report.Smooth.Points,
		Report.DominantMatchPct, Report.SmoothDominantMatchPct, Report.FlipsWarp.Flipped, Report.FlipsAddedByRestores,
		Report.NumLODs, Report.LowerLODsFlipsAdded, Report.LowerLODsSmoothDominantMatchPct, Report.LowerLODsMs, Report.TotalMs);
	if (OutReport)
	{
		*OutReport = Report;
	}
	return true;
}
