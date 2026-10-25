// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class USkeletalMesh;

/**
 * #774 (owner, 2026-10-08): As-is avatars are resized to the default standing height (FMantleAvatarHeight::TargetCm, the UEFN
 * driver's 165.6 cm), as Convert does, with Convert's +-FMantleAvatarHeight::ConvertedToleranceCm. The mesh is scaled
 * uniformly about its feet (the lowest vertex, on the vertical axis through the origin): vertices, morph deltas, the
 * reference pose's translations, and mesh-only sockets. The Convert route does this in its MetaHuman steps instead.
 */
class MANTLEEDITOR_API FMantleAvatarHeightFixer : public IMantleAvatarFixer
{
public:
	/** LOD 0's bind-pose height (highest Z minus lowest Z) and lowest Z, in cm; false when the mesh has no readable LOD 0. */
	static bool MeasureHeight(const USkeletalMesh& Mesh, double& OutHeightCm, double& OutFeetZ);

	/** The uniform scale that takes a body HeightCm tall to TargetCm; 1 when either is not a positive number. Pure (tested). */
	static double ScaleFor(double HeightCm, double TargetCm);

	/** A point scaled by K about the point (0, 0, FeetZ). Pure (tested). */
	static FVector3f ScalePoint(const FVector3f& Point, double K, double FeetZ);

	/**
	 * A NEW skeletal mesh, PackagePath/AssetName, copied from Source and scaled by K about (0, 0, FeetZ) (every LOD with
	 * geometry). Source is never modified; the new package is dirty, not saved. Null, with why, when it cannot be written.
	 */
	static USkeletalMesh* WriteScaled(USkeletalMesh* Source, double K, double FeetZ, const FString& PackagePath, const FString& AssetName,
		FString& OutError);

	virtual FName GetId() const override { return MantleAvatarFixerIds::AsIsHeight; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleAvatarHeightFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarHeightFixer();
