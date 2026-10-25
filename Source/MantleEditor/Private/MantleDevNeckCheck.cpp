// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.NeckCheck
// Read-only diagnosis of the collar of clothing fitted on the standard body types made in memory. The client fits a collar from the face a
// garment was authored with (FMantleNeckSamples: the neck base is the highest body vertex within 12 cm of the z axis; the samples are the centroids
// of the face's triangles in a region around that base, paired with the wearer face's SAME-INDEX triangles). So this logs, per case:
//  - the neck seam: body and face in the same space (neck base, face column, distance from the body's neck ring to the nearest face vertex);
//  - whether a face's triangle T is the same place as triangle T of a built face (same-index vs nearest-centroid distance over the region).
// Cases: the standard bodies f_med_nrw and m_med_nrw made in memory, and the converted Aera (preview and built), which fitted correctly before.
// Nothing is changed, saved or uploaded. Logs MANTLE_NECK_CHECK lines.
#include "MantleAvatarContract.h"
#include "MantleBodyTopologyData.h"
#include "MantleFaceLayout.h"
#include "MantleFitBodies.h"

#include "Rendering/StaticMeshVertexBuffer.h"

#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleNeckCheck, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevNeckCheckPrivate
{
	// The client's numbers (MantleNeckSamples.h).
	constexpr float ColumnRadiusCm = 12.0f;
	constexpr float ColumnHeightCm = 6.0f;
	constexpr float CorrespondBelowBaseCm = 6.0f;
	constexpr float CorrespondAboveBaseCm = 12.0f;
	constexpr float CorrespondRadiusCm = 16.0f;
	constexpr int32 ReferenceFaceTriangles = 6872;

	struct FNeckCase
	{
		FString Name;
		bool bOk = false;
		FString Error;
		TArray<FVector3f> Body;
		int32 BodyLOD = INDEX_NONE;
		TArray<FVector3f> FacePositions;
		TArray<uint32> FaceIndices;
		int32 FaceLOD = INDEX_NONE;
		float BaseZ = -MAX_flt;
		/** The face's triangle centroids, by triangle index. */
		TArray<FVector3f> Centroids;
		/** FMantleFitBodies::FaceNumberingHash of the face: equal on faces that number their triangles alike. */
		uint32 FaceNumbering = 0;
		/** UV0 centroid of each face triangle, by triangle index (shape independent), and the triangle count of every face LOD. */
		TArray<FVector2f> UVCentroids;
		/** The three UV0s of each face triangle, by triangle index. */
		TArray<FVector2f> TriangleUVs;
		FString FaceLODs;
	};

	/** The first LOD of a body with the reference vertex count, its vertices; false when none. */
	bool ReadBody(const USkeletalMesh* Mesh, const int32 ReferenceVertices, FNeckCase& Out)
	{
		const int32 NumLODs = UMantleBodyTopologyData::GetNumLODs(Mesh);
		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			FMantleBodyLODTopology Topology;
			TArray<FVector3f> Positions;
			if (UMantleBodyTopologyData::ReadLOD(Mesh, LOD, Topology, &Positions) && Topology.VertexCount == ReferenceVertices)
			{
				Out.Body = MoveTemp(Positions);
				Out.BodyLOD = LOD;
				return true;
			}
		}
		Out.Error = TEXT("the body has no LOD with the reference vertex count");
		return false;
	}

	/** The face LOD the fit measures against: the first with the reference face topology, else LOD0 (the client's FitLOD). */
	bool ReadFace(const USkeletalMesh* Mesh, FNeckCase& Out)
	{
		const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || RenderData->LODRenderData.IsEmpty())
		{
			Out.Error = TEXT("the face has no render data");
			return false;
		}
		int32 Chosen = 0;
		for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
		{
			if (RenderData->LODRenderData[LOD].GetTotalFaces() == ReferenceFaceTriangles) { Chosen = LOD; break; }
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[Chosen];
		const FPositionVertexBuffer& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || !LOD.MultiSizeIndexContainer.IsIndexBufferValid())
		{
			Out.Error = TEXT("the face has no CPU positions or indices");
			return false;
		}
		for (uint32 Vertex = 0; Vertex < Positions.GetNumVertices(); ++Vertex) { Out.FacePositions.Add(Positions.VertexPosition(Vertex)); }
		LOD.MultiSizeIndexContainer.GetIndexBuffer(Out.FaceIndices);
		for (int32 Index = 0; Index < RenderData->LODRenderData.Num(); ++Index)
		{
			Out.FaceLODs += (Out.FaceLODs.IsEmpty() ? TEXT("") : TEXT("/")) + FString::FromInt(static_cast<int32>(RenderData->LODRenderData[Index].GetTotalFaces()));
		}
		const FStaticMeshVertexBuffer& UVBuffer = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		if (UVBuffer.GetNumTexCoords() > 0 && UVBuffer.GetTexCoordData())
		{
			for (int32 First = 0; First + 2 < Out.FaceIndices.Num(); First += 3)
			{
				Out.UVCentroids.Add((UVBuffer.GetVertexUV(Out.FaceIndices[First], 0) + UVBuffer.GetVertexUV(Out.FaceIndices[First + 1], 0) + UVBuffer.GetVertexUV(Out.FaceIndices[First + 2], 0)) / 3.0f);
				for (int32 Corner = 0; Corner < 3; ++Corner) { Out.TriangleUVs.Add(UVBuffer.GetVertexUV(Out.FaceIndices[First + Corner], 0)); }
			}
		}
		Out.FaceLOD = Chosen;
		Out.FaceNumbering = FMantleFitBodies::FaceNumberingHash(Mesh);
		for (int32 First = 0; First + 2 < Out.FaceIndices.Num(); First += 3)
		{
			Out.Centroids.Add((Out.FacePositions[Out.FaceIndices[First]] + Out.FacePositions[Out.FaceIndices[First + 1]] + Out.FacePositions[Out.FaceIndices[First + 2]]) / 3.0f);
		}
		return true;
	}

	bool InRegion(const FVector3f& Centroid, const float BaseZ)
	{
		return Centroid.Z >= BaseZ - CorrespondBelowBaseCm && Centroid.Z < BaseZ + CorrespondAboveBaseCm
			&& FMath::Sqrt(Centroid.X * Centroid.X + Centroid.Y * Centroid.Y) <= CorrespondRadiusCm;
	}

	void Describe(FNeckCase& Case)
	{
		float BaseZ = -MAX_flt;
		FBox BodyBox(ForceInit), FaceBox(ForceInit);
		for (const FVector3f& P : Case.Body)
		{
			BodyBox += FVector(P);
			if (P.X * P.X + P.Y * P.Y <= ColumnRadiusCm * ColumnRadiusCm) { BaseZ = FMath::Max(BaseZ, P.Z); }
		}
		for (const FVector3f& P : Case.FacePositions) { FaceBox += FVector(P); }
		Case.BaseZ = BaseZ;
		// The face's neck column just over the base: the axis the client casts or centres on.
		FVector2f Sum = FVector2f::ZeroVector;
		int32 Column = 0;
		for (const FVector3f& P : Case.FacePositions)
		{
			if (P.Z >= BaseZ && P.Z < BaseZ + ColumnHeightCm && P.X * P.X + P.Y * P.Y <= ColumnRadiusCm * ColumnRadiusCm) { Sum += FVector2f(P.X, P.Y); ++Column; }
		}
		// The body's neck ring (its top, within 1 cm of the base) against the nearest face vertex.
		TArray<FVector3f> Ring;
		for (const FVector3f& P : Case.Body)
		{
			if (P.X * P.X + P.Y * P.Y <= ColumnRadiusCm * ColumnRadiusCm && P.Z >= BaseZ - 1.0f) { Ring.Add(P); }
		}
		float RingAverage = -1.0f, RingMax = -1.0f;
		FMantleFitBodies::CompareNearest(Ring, Case.FacePositions, RingAverage, RingMax);
		int32 Region = 0;
		for (const FVector3f& Centroid : Case.Centroids) { Region += InRegion(Centroid, BaseZ) ? 1 : 0; }
		UE_LOG(LogMantleNeckCheck, Display,
			TEXT("MANTLE_NECK_CHECK case=%s faceLODTris=%s faceNumbering=%08x bodyLOD=%d bodyVerts=%d faceLOD=%d faceTris=%d baseZ=%.2f bodyZ=[%.2f, %.2f] faceZ=[%.2f, %.2f] faceColumnVerts=%d faceColumnCentre=(%.2f, %.2f) ringVerts=%d ringToFaceAvgCm=%.3f ringToFaceMaxCm=%.3f regionTris=%d"),
			*Case.Name, *Case.FaceLODs, Case.FaceNumbering, Case.BodyLOD, Case.Body.Num(), Case.FaceLOD, Case.Centroids.Num(), BaseZ, BodyBox.Min.Z, BodyBox.Max.Z, FaceBox.Min.Z, FaceBox.Max.Z, Column,
			Column ? Sum.X / Column : 0.0f, Column ? Sum.Y / Column : 0.0f, Ring.Num(), RingAverage, RingMax, Region);
	}

	/**
	 * Which triangles differ by UV centroid (A's triangle T against B's): per built-layout section, how many lie in A's collar region, how far apart
	 * they are, and whether the same triangle (the same three UVs, in any corner order) exists in B at another index (a renumbering) or not at all (a
	 * different triangle). The first differing triangles in the region are listed.
	 */
	void DescribeDiffering(const FNeckCase& A, const FNeckCase& B)
	{
		if (A.TriangleUVs.Num() != A.Centroids.Num() * 3 || B.TriangleUVs.Num() != B.Centroids.Num() * 3 || A.Centroids.Num() != B.Centroids.Num()) { return; }
		const FMantleFaceLayout* Layout = FMantleFaceLayout::Get();
		const auto SameCorners = [&A, &B](const int32 TriangleA, const int32 TriangleB)
		{
			bool bUsed[3] = { false, false, false };
			for (int32 CornerA = 0; CornerA < 3; ++CornerA)
			{
				bool bFound = false;
				for (int32 CornerB = 0; CornerB < 3 && !bFound; ++CornerB)
				{
					if (!bUsed[CornerB] && FVector2f::Distance(A.TriangleUVs[TriangleA * 3 + CornerA], B.TriangleUVs[TriangleB * 3 + CornerB]) <= 0.002f) { bUsed[CornerB] = true; bFound = true; }
				}
				if (!bFound) { return false; }
			}
			return true;
		};
		int32 DifferingInRegion = 0, RegionMoved = 0, RegionDifferent = 0, Listed = 0;
		TMap<FString, TPair<int32, int32>> PerSection; // (differing, of them in the region)
		for (int32 Triangle = 0; Triangle < A.Centroids.Num(); ++Triangle)
		{
			if (FVector2f::Distance(A.UVCentroids[Triangle], B.UVCentroids[Triangle]) <= 0.002f) { continue; }
			const bool bRegion = InRegion(A.Centroids[Triangle], A.BaseZ);
			FString Section = TEXT("?");
			if (Layout)
			{
				for (const FMantleFaceLayoutSection& Candidate : Layout->Sections)
				{
					if (Triangle >= Candidate.FirstIndex / 3 && Triangle < Candidate.FirstIndex / 3 + Candidate.NumTriangles) { Section = Candidate.Material; break; }
				}
			}
			TPair<int32, int32>& Count = PerSection.FindOrAdd(Section);
			++Count.Key;
			Count.Value += bRegion ? 1 : 0;
			if (!bRegion) { continue; }
			++DifferingInRegion;
			bool bElsewhere = false;
			for (int32 Other = 0; Other < B.Centroids.Num() && !bElsewhere; ++Other) { bElsewhere = SameCorners(Triangle, Other); }
			RegionMoved += bElsewhere ? 1 : 0;
			RegionDifferent += bElsewhere ? 0 : 1;
			if (Listed < 12)
			{
				++Listed;
				UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK differing a=%s b=%s triangle=%d section=%s uvCentroidA=(%.4f,%.4f) uvCentroidB=(%.4f,%.4f) sameTriangleElsewhereInB=%d"),
					*A.Name, *B.Name, Triangle, *Section, A.UVCentroids[Triangle].X, A.UVCentroids[Triangle].Y, B.UVCentroids[Triangle].X, B.UVCentroids[Triangle].Y, bElsewhere ? 1 : 0);
			}
		}
		for (const TPair<FString, TPair<int32, int32>>& Pair : PerSection)
		{
			UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK differing a=%s b=%s section=%s differing=%d inCollarRegion=%d"), *A.Name, *B.Name, *Pair.Key, Pair.Value.Key, Pair.Value.Value);
		}
		UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK differing a=%s b=%s collarRegion: differing=%d (same triangle at another index in B=%d, no such triangle in B=%d)"),
			*A.Name, *B.Name, DifferingInRegion, RegionMoved, RegionDifferent);
	}

	/** Triangle T of A against triangle T of B, and against B's nearest centroid, over A's collar region. */
	void ComparePair(const FNeckCase& A, const FNeckCase& B)
	{
		if (A.Centroids.Num() != B.Centroids.Num() || A.Centroids.IsEmpty())
		{
			UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK pair a=%s b=%s not compared: %d vs %d face triangles"), *A.Name, *B.Name, A.Centroids.Num(), B.Centroids.Num());
			return;
		}
		TArray<FVector3f> Probe;
		double SumSame = 0.0;
		float MaxSame = 0.0f;
		for (int32 Triangle = 0; Triangle < A.Centroids.Num(); ++Triangle)
		{
			if (!InRegion(A.Centroids[Triangle], A.BaseZ)) { continue; }
			Probe.Add(A.Centroids[Triangle]);
			const float Distance = FVector3f::Distance(A.Centroids[Triangle], B.Centroids[Triangle]);
			SumSame += Distance;
			MaxSame = FMath::Max(MaxSame, Distance);
		}
		float NearestAverage = -1.0f, NearestMax = -1.0f;
		FMantleFitBodies::CompareNearest(Probe, B.Centroids, NearestAverage, NearestMax);
		// The UV layout, triangle by triangle: zero everywhere means the two faces number their triangles alike (UVs do not depend on shape); a
		// difference means a different numbering or a different UV layout. Over all triangles, not only the collar region.
		int32 UVDiffer = 0;
		float UVMax = 0.0f;
		double UVSum = 0.0;
		if (A.UVCentroids.Num() == A.Centroids.Num() && B.UVCentroids.Num() == B.Centroids.Num())
		{
			for (int32 Triangle = 0; Triangle < A.UVCentroids.Num(); ++Triangle)
			{
				const float Distance = FVector2f::Distance(A.UVCentroids[Triangle], B.UVCentroids[Triangle]);
				UVSum += Distance;
				UVMax = FMath::Max(UVMax, Distance);
				UVDiffer += Distance > 0.002f ? 1 : 0;
			}
		}
		UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK pair a=%s b=%s uvCentroidSameIndex: trianglesDiffering=%d of %d, avg=%.5f max=%.5f"), *A.Name, *B.Name, UVDiffer,
			A.UVCentroids.Num(), A.UVCentroids.IsEmpty() ? -1.0 : UVSum / A.UVCentroids.Num(), UVMax);
		if (UVDiffer > 0) { DescribeDiffering(A, B); }
		UE_LOG(LogMantleNeckCheck, Display,
			TEXT("MANTLE_NECK_CHECK pair a=%s b=%s regionTris=%d sameIndexAvgCm=%.3f sameIndexMaxCm=%.3f nearestCentroidAvgCm=%.3f nearestCentroidMaxCm=%.3f"),
			*A.Name, *B.Name, Probe.Num(), Probe.IsEmpty() ? -1.0 : SumSame / Probe.Num(), MaxSame, NearestAverage, NearestMax);
	}

	/**
	 * A standard body type made in memory (nothing saved): the body and face the upload would use. bMatched: the face put into the built
	 * face layout (FMantleFaceLayout::Reorder), the way the upload does; the result of the match is logged.
	 */
	FNeckCase InMemory(const TCHAR* Id, const EMetaHumanBodyType Type, const int32 ReferenceVertices, const bool bMatched)
	{
		FNeckCase Case;
		Case.Name = FString(Id) + (bMatched ? TEXT("(matched face)") : TEXT("(fresh)"));
		FMantleFitBody Body;
		if (FMantleFitBodies::Make(Id, Type, Body, Case.Error))
		{
			if (!bMatched)
			{
				Case.bOk = ReadBody(Body.BodyMesh.Get(), ReferenceVertices, Case) && ReadFace(Body.FaceMesh.Get(), Case);
			}
			else if (ReadBody(Body.BodyMesh.Get(), ReferenceVertices, Case))
			{
				const FMantleFaceLayout* Layout = FMantleFaceLayout::Get();
				const int32 LOD = FMantleFaceLayout::FindFaceLOD(Body.FaceMesh.Get());
				FMantleGlbMesh Face;
				FString Note;
				if (!Layout) { Case.Error = TEXT("no built face layout (Resources/FaceLayout); Mantle.Dev.RecordFaceLayout records it"); }
				else if (LOD == INDEX_NONE) { Case.Error = TEXT("the in-memory face has no 6872-triangle LOD"); }
				else if (FMantleFaceLayout::ReadRender(Body.FaceMesh.Get(), LOD, Face, Case.Error))
				{
					const int32 SourceVerts = Face.Positions.Num();
					FString SourceSections;
					for (const FMantleGlbPrimitive& Primitive : Face.Primitives) { SourceSections += FString::Printf(TEXT(" %s:%dv/%dt"), *Face.MaterialNames[Primitive.MaterialIndex], Primitive.NumVertices, Primitive.Indices.Num() / 3); }
					if (Layout->Reorder(Face, Note, Case.Error))
					{
						UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK case=%s face match ok: %s"), *Case.Name, *Note);
						Case.FacePositions = Face.Positions;
						for (const FMantleGlbPrimitive& Primitive : Face.Primitives)
						{
							for (const uint32 Index : Primitive.Indices) { Case.FaceIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex)); }
						}
						for (int32 First = 0; First + 2 < Case.FaceIndices.Num(); First += 3)
						{
							Case.Centroids.Add((Case.FacePositions[Case.FaceIndices[First]] + Case.FacePositions[Case.FaceIndices[First + 1]] + Case.FacePositions[Case.FaceIndices[First + 2]]) / 3.0f);
							Case.UVCentroids.Add((Face.UVs[Case.FaceIndices[First]] + Face.UVs[Case.FaceIndices[First + 1]] + Face.UVs[Case.FaceIndices[First + 2]]) / 3.0f);
							for (int32 Corner = 0; Corner < 3; ++Corner) { Case.TriangleUVs.Add(Face.UVs[Case.FaceIndices[First + Corner]]); }
						}
						Case.FaceLOD = LOD;
						Case.FaceLODs = TEXT("matched");
						Case.bOk = true;
					}
					else { UE_LOG(LogMantleNeckCheck, Warning, TEXT("MANTLE_NECK_CHECK case=%s face match FAILED (%d vertices; sections%s): %s"), *Case.Name, SourceVerts, *SourceSections, *Case.Error); }
				}
			}
		}
		FMantleFitBodies::Release(Body);
		return Case;
	}

	/** The converted Aera: its built body and face (what fitted correctly), or its preview body and face from Creator. */
	FNeckCase Converted(const TCHAR* Name, const bool bPreview, const int32 ReferenceVertices)
	{
		FNeckCase Case;
		Case.Name = FString(Name) + (bPreview ? TEXT("(preview)") : TEXT("(built)"));
		const FString CharacterPackage = FString::Printf(TEXT("/Game/MetaHumans/%s/Converted/%s/%s"), Name, Name, Name);
		const FString Unpacked = FString::Printf(TEXT("/Game/MetaHumans/%s/Converted/%s/Unpacked/%s"), Name, Name, Name);
		if (!bPreview)
		{
			const USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("%s/Body/SKM_%s_BodyMesh.SKM_%s_BodyMesh"), *Unpacked, Name, Name));
			const USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("%s/Face/SKM_%s_FaceMesh.SKM_%s_FaceMesh"), *Unpacked, Name, Name));
			if (!Body || !Face) { Case.Error = TEXT("the converted body or face is not in the project"); return Case; }
			Case.bOk = ReadBody(Body, ReferenceVertices, Case) && ReadFace(Face, Case);
			return Case;
		}
		UMetaHumanCharacter* Character = LoadObject<UMetaHumanCharacter>(nullptr, *FString::Printf(TEXT("%s.%s"), *CharacterPackage, Name));
		UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
		if (!Character || !Subsystem) { Case.Error = TEXT("the converted character or MetaHuman Creator is unavailable"); return Case; }
		const bool bAdded = !Subsystem->IsObjectAddedForEditing(Character) && Subsystem->TryAddObjectToEdit(Character);
		FMetaHumanCharacterPreviewAssetOptions Options;
		FMetaHumanCharacterPreviewAssets Preview;
		if (Subsystem->IsObjectAddedForEditing(Character) && Subsystem->TryGetCharacterPreviewAssets(Character, Options, Preview) && Preview.BodyMesh && Preview.FaceMesh)
		{
			Case.bOk = ReadBody(Preview.BodyMesh.Get(), ReferenceVertices, Case) && ReadFace(Preview.FaceMesh.Get(), Case);
		}
		else { Case.Error = TEXT("Creator did not provide the preview body and face"); }
		if (bAdded) { Subsystem->RemoveObjectToEdit(Character); }
		return Case;
	}

	void Run(const TArray<FString>& Args)
	{
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		if (!Contract)
		{
			UE_LOG(LogMantleNeckCheck, Error, TEXT("MANTLE_NECK_CHECK error=the avatar contract is unavailable"));
			return;
		}
		TArray<FNeckCase> Cases;
		Cases.Add(InMemory(TEXT("m_med_nrw"), EMetaHumanBodyType::m_med_nrw, Contract->TopologyVertexCount, false));
		Cases.Add(InMemory(TEXT("f_med_nrw"), EMetaHumanBodyType::f_med_nrw, Contract->TopologyVertexCount, false));
		Cases.Add(InMemory(TEXT("m_med_nrw"), EMetaHumanBodyType::m_med_nrw, Contract->TopologyVertexCount, true));
		Cases.Add(InMemory(TEXT("f_med_nrw"), EMetaHumanBodyType::f_med_nrw, Contract->TopologyVertexCount, true));
		Cases.Add(Converted(TEXT("Aera"), false, Contract->TopologyVertexCount));
		Cases.Add(Converted(TEXT("Aera"), true, Contract->TopologyVertexCount));
		Cases.Add(Converted(TEXT("Bo"), false, Contract->TopologyVertexCount));
		for (FNeckCase& Case : Cases)
		{
			if (!Case.bOk)
			{
				UE_LOG(LogMantleNeckCheck, Warning, TEXT("MANTLE_NECK_CHECK case=%s skipped: %s"), *Case.Name, *Case.Error);
				continue;
			}
			Describe(Case);
		}
		// Is triangle T the same place on a preview face as on a built one? Aera(preview) against Aera(built) is the same character, so
		// same-index distance should be ~0; if it is large while the nearest-centroid distance is ~0, the preview face numbers its triangles differently.
		const auto Find = [&Cases](const TCHAR* Name) -> const FNeckCase* { return Cases.FindByPredicate([Name](const FNeckCase& Case) { return Case.bOk && Case.Name == Name; }); };
		const FNeckCase* AeraBuilt = Find(TEXT("Aera(built)"));
		const FNeckCase* AeraPreview = Find(TEXT("Aera(preview)"));
		const FNeckCase* BoBuilt = Find(TEXT("Bo(built)"));
		const FNeckCase* Male = Find(TEXT("m_med_nrw(fresh)"));
		const FNeckCase* Female = Find(TEXT("f_med_nrw(fresh)"));
		const FNeckCase* MaleMatched = Find(TEXT("m_med_nrw(matched face)"));
		const FNeckCase* FemaleMatched = Find(TEXT("f_med_nrw(matched face)"));
		if (AeraPreview && AeraBuilt) { ComparePair(*AeraPreview, *AeraBuilt); }
		if (Female && AeraBuilt) { ComparePair(*Female, *AeraBuilt); }
		if (Male && AeraBuilt) { ComparePair(*Male, *AeraBuilt); }
		if (Male && Female) { ComparePair(*Male, *Female); }
		// Two BUILT faces: do they pair correctly by triangle index?
		if (AeraBuilt && BoBuilt) { ComparePair(*AeraBuilt, *BoBuilt); ComparePair(*BoBuilt, *AeraBuilt); }
		if (AeraBuilt && Male) { ComparePair(*AeraBuilt, *Male); }
		// The in-memory faces put into the built layout: uvCentroidSameIndex should show ~0 differing triangles against Aera(built), and the
		// same-index distances should fall to the nearest-centroid level (shape differences only).
		for (const FNeckCase* Matched : { MaleMatched, FemaleMatched })
		{
			if (!Matched) { continue; }
			if (AeraBuilt) { ComparePair(*Matched, *AeraBuilt); }
			if (BoBuilt) { ComparePair(*Matched, *BoBuilt); }
		}
		if (MaleMatched && FemaleMatched) { ComparePair(*MaleMatched, *FemaleMatched); }
		UE_LOG(LogMantleNeckCheck, Display, TEXT("MANTLE_NECK_CHECK done"));
	}

	FAutoConsoleCommand GMantleDevNeckCheckCommand(
		TEXT("Mantle.Dev.NeckCheck"),
		TEXT("Mantle.Dev.NeckCheck -- read-only: logs MANTLE_NECK_CHECK lines about the neck seam (body vs face, same space) and whether a preview face's triangle T is the same place as a built face's ")
		TEXT("triangle T, for the standard bodies made in memory and the converted Aera (preview and built). Changes nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
