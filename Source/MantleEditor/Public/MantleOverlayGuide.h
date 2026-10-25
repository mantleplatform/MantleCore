// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/**
 * #736: the painting guide for scar and tattoo overlay creators. A scar or tattoo is a mask painted in the fixed
 * converted-MetaHuman UV layout, which a painter cannot see in a blank canvas. The guide is a Size x Size image of
 * that layout: every UV triangle filled in a soft colour by body region (the region the triangle's skin weights say it
 * belongs to), faint triangle edges, brighter island borders, and the region's name written on its largest island.
 *
 * The UV channel read is channel 0 of LOD0, the channel the shared skin material's masks sample. Drawing is entirely on
 * the CPU (scanline fill, line drawing and a built-in bitmap font), so the same mesh gives the same bytes anywhere,
 * with or without a renderer.
 */
struct MANTLEEDITOR_API FMantleOverlayGuide
{
	/** The guide's side in pixels. */
	static constexpr int32 Size = 2048;

	/** The regions a triangle can fall in. Body: Head to FootR (+ Other); face: Face, Scalp, Ears, Neck (+ Other). */
	enum class ERegion : uint8
	{
		Head, Neck, Chest, Back, ArmL, ArmR, HandL, HandR, LegL, LegR, FootL, FootR,
		Face, Scalp, Ears, Other,
		Count
	};

	/** One UV triangle with the region it belongs to. UV is 0..1 across the texture, V down as stored. */
	struct FTriangle
	{
		FVector2f UV[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		ERegion Region = ERegion::Other;
	};

	/** What the guide shows for a region, e.g. "Chest & belly". */
	static FString RegionName(ERegion Region);

	/** Which region a skin-weight bone belongs to, by the bone-name table. Torso bones answer Chest; the caller splits front/back. */
	static ERegion ClassifyBone(const FString& BoneName, bool bFace);

	/**
	 * Reads LOD0's triangles from a converted MetaHuman body or face: UV channel 0, the region from the summed skin
	 * weights of each triangle's vertices, torso triangles split front/back by their normal against the body's forward
	 * (read from the foot-to-ball direction of the reference pose).
	 */
	static bool ReadMesh(const USkeletalMesh& Mesh, bool bFace, TArray<FTriangle>& OutTriangles, FString& OutError);

	/** Draws the guide image (Size x Size, opaque BGRA) from triangles. OutRegionsDrawn lists the region names that have triangles. */
	static void Render(TConstArrayView<FTriangle> Triangles, TArray<FColor>& OutPixels, TArray<FString>* OutRegionsDrawn = nullptr);

	/** MetaHuman Creator's template meshes: the converted avatars' UV layout. */
	static constexpr const TCHAR* TemplateBodyPath = TEXT("/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body.SKM_Body");
	static constexpr const TCHAR* TemplateFacePath = TEXT("/MetaHumanCharacter/Face/SKM_Face.SKM_Face");

	/**
	 * Draws the guide from MetaHuman Creator's template body or face, falling back to Mantle's reference body or face.
	 * Generates the shipped guides, and serves tests.
	 */
	static bool Build(bool bFace, TArray<FColor>& OutPixels, FString& OutError, TArray<FString>* OutRegionsDrawn = nullptr);

	/** Where the plugin ships the generated guide (Resources/OverlayGuides), or empty without the plugin. */
	static FString ShippedPath(bool bFace);

	/**
	 * Writes the guide as a PNG at Path: the shipped copy, so a creator needs no MetaHuman content, or a freshly built
	 * one when the plugin doesn't carry it or bRebuild is set (regenerating the shipped guides).
	 */
	static bool Write(bool bFace, const FString& Path, FString& OutError, bool bRebuild = false);

	/** The save dialog's default file name for a slot: MantleGuide_Face.png for *_face, else MantleGuide_Body.png. */
	static FString DefaultFileName(const FString& Slot);
};
