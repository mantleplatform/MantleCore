// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/** Offline-approved source face + MetaHuman UV correspondence. No Groom data or UObject access. */
class MANTLERUNTIME_API FMantleHairHeadFit
{
public:
	static constexpr int32 Version = 3;
	/** Conservative same-chart LOD boundary drift: at most four texels at 2048 and one millimetre on either surface. */
	static constexpr float MaxBoundaryUV = 1.f/512.f;
	static constexpr float MaxBoundaryCorrectionCm = .1f;
	/** #784: card roots with no corresponding target skin borrow the nearest mapped root's offset, up to this share of an LOD's roots. */
	static constexpr float MaxBorrowedRootShare = .05f;
	/** Facial slots use target-face weights; scalp hair keeps rigid head weights and is rebound to Body. OutBorrowedRoots counts the roots that borrowed. */
	static bool Fit(const FMantleSkinnedMesh& Cards, const FMantleSkinnedMesh& SourceFace,
		const FMantleSkinnedMesh& TargetFace, const FMantleSkinnedMesh& Body, bool bFacial,
		FMantleSkinnedMesh& Out, FString& Error, int32* OutBorrowedRoots = nullptr);
	static bool IsFaceSkinned(const FMantleSkinnedMesh& Mesh);
};
