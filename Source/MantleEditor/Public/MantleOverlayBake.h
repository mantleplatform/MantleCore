// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
class FMantleAvatarContract;

/**
 * #54: where a creator put a flat picture on the converted MetaHuman body or face. The picture is projected onto the
 * mesh from this transform (the projector looks along its local +X; local Y is the picture's right, local Z its up) and
 * baked into the overlay's UV-space mask.
 */
struct MANTLEEDITOR_API FMantleOverlayPlacement
{
	/** The projector in the mesh's own (reference pose) space, centimetres. */
	FTransform Transform = FTransform::Identity;
	/** The picture's width on the surface; its height follows the picture's shape. */
	float WidthCm = 10.f;
	/** How far either side of the projector's origin the picture reaches along its direction. */
	float DepthCm = 15.f;
	/** The share of the depth over which the mark fades out (0 = hard edge, 1 = fades from the middle). */
	float Falloff = 0.4f;
	/** How strongly a scar's normal map pushes (0 = flat). Scars only. */
	float Relief = 1.5f;
};

/**
 * #54: the bake. Pure CPU, so the same inputs give the same bytes with or without a renderer. For every texel of every UV
 * island of the mesh, the surface point it stands for is projected into the picture; where it lands inside the picture
 * and inside the depth, the mask takes the picture's strength, faded by depth and by how squarely the surface faces the
 * projector. Writing in UV space writes every island the picture touches, so a mark that wraps from chest to shoulder is
 * continuous; island borders are padded a few texels so the seams show no line.
 */
struct MANTLEEDITOR_API FMantleOverlayBake
{
	/** The mask and normal map side, always. */
	static constexpr int32 Size = 1024;
	/** Texels each island border is padded by. */
	static constexpr int32 Padding = 4;

	/** One surface triangle: position, normal and UV (0..1, V down as stored) per corner. */
	struct FTriangle
	{
		FVector3f Position[3];
		FVector3f Normal[3];
		FVector2f UV[3];
	};

	/**
	 * The skin a picture is placed on when the creator picks none: MetaHuman Creator's template body or face (the layout the painting guide and the
	 * shared skin material use; a built reference face's imported UVs are not that layout), else Mantle's reference body or face. Null when neither is in the project.
	 */
	static USkeletalMesh* DefaultMesh(bool bFace, const FMantleAvatarContract* Contract);

	/** The picture's strength at 0..1 across it: alpha when the picture has transparency, else brightness (white shows). */
	static TArray<uint8> PictureStrength(TConstArrayView<FColor> Pixels, int32 Width, int32 Height);

	/** LOD0 of a converted body or face: skin sections only, UDIM tiles folded back, as the painting guide reads them. */
	static bool ReadMesh(const USkeletalMesh& Mesh, TArray<FTriangle>& OutTriangles, FString& OutError);

	/**
	 * Bakes the picture (strength 0..255, Width x Height) through Placement onto Triangles. OutMask is Size x Size, one
	 * value per texel (255 = shows). When OutNormal is given it receives a Size x Size tangent-space normal map derived
	 * from the mask's relief, opaque, flat (128,128,255) where nothing is placed. Returns the number of texels painted.
	 */
	static int32 Bake(TConstArrayView<FTriangle> Triangles, TConstArrayView<uint8> Strength, int32 Width, int32 Height,
		const FMantleOverlayPlacement& Placement, TArray<uint8>& OutMask, TArray<FColor>* OutNormal = nullptr);

	/** The mask as the BGRA image the overlay checks and the export read: the value in red, green and blue, opaque. */
	static TArray<FColor> MaskToPixels(TConstArrayView<uint8> Mask);
};
