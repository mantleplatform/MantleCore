// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

class USkeletalMesh;

/**
 * The MetaHuman reference face topology: every face
 * exported at UE Optimized Medium has the same 6,872 triangles at LOD0 (High: LOD1; Low: none), and
 * triangle T is the same place on every such face. It is recorded by triangle
 * count alone; faces are not part of garment compatibility, only the collar's precision depends on it.
 */
struct MANTLERUNTIME_API FMantleFaceTopology
{
	static constexpr int32 ReferenceTriangles = 6872;

	/** Face has the reference face topology. */
	static bool IsReference(const FMantleSkinnedMeshLOD& Face) { return Face.NumTriangles() == ReferenceTriangles; }

	/** First LOD of Face with ReferenceTriangles triangles, INDEX_NONE when none has. */
	static int32 FindReferenceLOD(const USkeletalMesh* Face);

	/**
	 * The LOD of a cooked face (MSK1, read into CPU data) that the fit measures against, the way ReadFace
	 * picks one from an asset: the first with the reference face topology, else LOD0. Null without LODs.
	 */
	static const FMantleSkinnedMeshLOD* FitLOD(const FMantleSkinnedMesh& Face);

	/**
	 * Spike helper (like FMantleSkinnedMesh::ExtractFromSkeletalMesh, a dev and editor tool reading CPU
	 * copies of render data): the face's surface at its reference LOD (LOD0 when it has none, the rays
	 * then stand in for the correspondence): positions, normals and the whole triangle list, one section.
	 * No UVs or weights: the fit only needs the surface. False, with the reason in OutError, when the LOD
	 * or its CPU data is missing.
	 */
	static bool ReadFace(const USkeletalMesh* Face, FMantleSkinnedMeshLOD& Out, int32* OutLOD = nullptr, FString* OutError = nullptr);
};

/** How the neck samples were made. */
enum class EMantleNeckMethod : uint8
{
	None,

	/** Both faces have the reference face topology: matched triangle centroids. */
	Correspondence,

	/** The fallback: a fixed ray family from the neck axis hits each face. */
	Rays,
};

/**
 * The neck for the fit. The MetaHuman body mesh ends at the neck base: the neck and the neck base
 * a collar rests on are the FACE mesh's, and a body-only RBF fit has no sample under a collar. So the
 * fit's two shapes get neck samples appended after the body (which stays first, so every body index
 * holds): the source face's points and the wearer face's corresponding ones.
 *
 * Holds no UObjects; may run on any thread.
 */
struct MANTLERUNTIME_API FMantleNeckSamples
{
	/** Ray family: heights over the neck base x elevations x azimuths. */
	static constexpr int32 NumHeights = 5;
	static constexpr int32 NumElevations = 4;
	static constexpr int32 NumAzimuths = 24;

	/** Rays stop here (cm): past any neck, short of the far shoulder. */
	static constexpr float ReachCm = 25.0f;

	/** The neck column: within this of the z axis. The body's top inside it is the neck base; the face's vertices ColumnHeightCm over it give the axis. */
	static constexpr float ColumnRadiusCm = 12.0f;
	static constexpr float ColumnHeightCm = 6.0f;

	/** Correspondence region: source centroids from this far under the neck base to this far over it, within this of the axis. */
	static constexpr float CorrespondBelowBaseCm = 6.0f;
	static constexpr float CorrespondAboveBaseCm = 12.0f;
	static constexpr float CorrespondRadiusCm = 16.0f;

	static int32 NumRays() { return NumHeights * NumElevations * NumAzimuths; }

	/** Ray origin height over the neck base, cm. */
	static float HeightCm(const int32 Height) { return -1.0f + 2.0f * static_cast<float>(Height); }

	/** Ray elevation, degrees: 0 horizontal, negative down towards the neck base and the upper chest. */
	static float ElevationDeg(const int32 Elevation) { return Elevation == 0 ? 0.0f : (Elevation == 1 ? -25.0f : (Elevation == 2 ? -50.0f : -70.0f)); }

	/** Ray Index of a face whose neck axis passes through (Centre, BaseZ). */
	static void Ray(int32 Index, const FVector2f& Centre, float BaseZ, FVector3f& OutOrigin, FVector3f& OutDir);

	/** The neck base: the highest body vertex within ColumnRadiusCm of the z axis; -MAX_flt when there is none. */
	static float NeckBaseZ(TConstArrayView<FVector3f> Body);

	/** One face's ray hits over its body. */
	struct FRaySet
	{
		float BaseZ = 0.0f;
		FVector2f Centre = FVector2f::ZeroVector;
		int32 ColumnVerts = 0;

		/** NumRays() entries: the first hit of each ray (zero on a miss) and whether it hit. */
		TArray<FVector3f> Points;
		TArray<uint8> Hit;
		int32 NumHit = 0;
		double Ms = 0.0;
	};

	/** Casts the rays at Face over Body (the neck base). False when nothing hits. */
	static bool Cast(const FMantleSkinnedMeshLOD& Face, TConstArrayView<FVector3f> Body, FRaySet& Out);

	/**
	 * Appends the ray correspondence: for every ray the source hit, the source hit and the wearer's hit,
	 * or, when the wearer's face missed it, the source hit moved by the mean wearer-minus-source offset of
	 * the ray's height ring (of every ray when the ring has none). Returns the number appended.
	 */
	static int32 AppendRays(const FRaySet& Source, const FRaySet& Wearer, TArray<FVector3f>& InOutSource, TArray<FVector3f>& InOutWearer, int32& OutFilled);

	/**
	 * P17g: the neck by face correspondence. Both faces must have the same triangles; the source centroids
	 * in the correspondence region around SourceBaseZ and the wearer's centroids of the same triangles.
	 * OutMaxJumpCm: the largest difference between neighbouring triangles' displacements in the region (a
	 * wrong index would jump by centimetres). OutTriangles (optional): each sample's triangle, ascending.
	 * False when the triangle counts differ or nothing is in the region.
	 */
	static bool Correspond(const FMantleSkinnedMeshLOD& SourceFace, const FMantleSkinnedMeshLOD& WearerFace, float SourceBaseZ,
		TArray<FVector3f>& OutSource, TArray<FVector3f>& OutWearer, float& OutMaxJumpCm, TArray<int32>* OutTriangles = nullptr);

	/** The samples for one (source, wearer) pair and how they were made. */
	struct FResult
	{
		EMantleNeckMethod Method = EMantleNeckMethod::None;

		/** Appended to the source shape and to the wearer's positions, in the same order. */
		TArray<FVector3f> Source;
		TArray<FVector3f> Wearer;

		float SourceBaseZ = 0.0f;
		float WearerBaseZ = 0.0f;

		/** Correspondence: the neighbour jump sanity figure. Rays: wearer misses filled from their ring. */
		float MaxJumpCm = 0.0f;
		int32 RaysFilled = 0;
		int32 SourceRayHits = 0;
		int32 WearerRayHits = 0;
		double Ms = 0.0;

		int32 Num() const { return Source.Num(); }

		/** "correspondence", "rays" or "none". Inline: a nested struct is not exported with its enclosing one. */
		const TCHAR* MethodName() const
		{
			return Method == EMantleNeckMethod::Correspondence ? TEXT("correspondence") : (Method == EMantleNeckMethod::Rays ? TEXT("rays") : TEXT("none"));
		}
	};

	/**
	 * The neck samples in the two halves the catalog needs: the plugin measures the source half at upload
	 * (its RBF weights take seconds, so they ride in the body shape, MBS1's NECK block), and the client the
	 * wearer's half at fit time. The source half does not depend on the wearer: by correspondence when
	 * SourceFace has the reference face topology (OutIds: the source triangles, ascending), else by the rays
	 * (OutIds: the rays the source face hit, ascending). OutSource is the source side of the samples for any
	 * wearer that takes the same method. False (nothing out) when the body has no neck base or nothing is
	 * found.
	 */
	static bool BuildSource(const FMantleSkinnedMeshLOD& SourceFace, TConstArrayView<FVector3f> SourceBody,
		EMantleNeckMethod& OutMethod, TArray<int32>& OutIds, TArray<FVector3f>& OutSource);

	/**
	 * The wearer's half: for a source half BuildSource made (Method, Ids ascending, Source in the same order),
	 * the wearer face's corresponding samples over WearerBody, paired as the source half is: the same triangles'
	 * centroids (Correspondence; WearerFace must have the reference face topology too), or each ray's hit on
	 * WearerFace, a miss filled from its height ring (Rays). Out.Source is Source and Out.Wearer one sample per
	 * id. False (Out.Method None, no samples) when the wearer cannot take the method (a face without the reference
	 * topology for correspondence, no ray hitting both faces) or the input does not hang together; the fit then
	 * runs from the body alone.
	 */
	static bool BuildWearer(EMantleNeckMethod Method, TConstArrayView<int32> Ids, TConstArrayView<FVector3f> Source,
		const FMantleSkinnedMeshLOD& WearerFace, TConstArrayView<FVector3f> WearerBody, FResult& Out);
};
